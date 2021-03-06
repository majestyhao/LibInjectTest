/*
 * 将某动态链接库注入目标进程，使其中途执行我们想要其执行的函数（下文皆唤作"hook函数"）
 *
 */


#include <stdio.h>
#include <stdlib.h>
#include <asm/user.h>
#include <asm/ptrace.h>
#include <sys/ptrace.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <dlfcn.h>
#include <dirent.h>
#include <unistd.h>
#include <string.h>
#include <elf.h>
#include <android/log.h>

#if defined(__i386__)
#define pt_regs         user_regs_struct
#endif

#define ENABLE_DEBUG 1

#if ENABLE_DEBUG
#define  LOG_TAG "INJECT"
#define  LOGD(fmt, args...)  __android_log_print(ANDROID_LOG_DEBUG,LOG_TAG, fmt, ##args)
#define DEBUG_PRINT(format,args...) \
    LOGD(format, ##args)
#else
#define DEBUG_PRINT(format,args...)
#endif

#define CPSR_T_MASK     ( 1u << 5 )

const char *libc_path = "/system/lib/libc.so";
const char *linker_path = "/system/bin/linker";

/* ref: play with ptrace, part I
   读取　traced 内寄存器内容至buf
   然并卵*/
int ptrace_readdata(pid_t pid,  uint8_t *src, uint8_t *buf, size_t size)
{
    uint32_t i, j, remain;
    uint8_t *laddr;

    union u {
        long val; // 当以满4字节读取内容时，直接使用 long 变量
        char chars[sizeof(long)]; // 最后不满4字节的内容，使用 char 变量
    } d;

    j = size / 4;
    remain = size % 4;

    laddr = buf;

    for (i = 0; i < j; i ++) {
        d.val = ptrace(PTRACE_PEEKTEXT, pid, src, 0);
        memcpy(laddr, d.chars, 4);
        src += 4;
        laddr += 4;
    }

    if (remain > 0) {
        d.val = ptrace(PTRACE_PEEKTEXT, pid, src, 0);
        memcpy(laddr, d.chars, remain);
    }

    return 0;
}

/* 将长度为Size字节的本trace进程的从data地址开始的数据写入到tracee的dest开始处内存 */
int ptrace_writedata(pid_t pid, uint8_t *dest, uint8_t *data, size_t size)
{
    uint32_t i, j, remain;
    uint8_t *laddr;

    union u {
        long val;
        char chars[sizeof(long)];
    } d;

    j = size / 4;
    remain = size % 4;

    laddr = data;

    for (i = 0; i < j; i ++) {
        memcpy(d.chars, laddr, 4);
        ptrace(PTRACE_POKETEXT, pid, dest, d.val);

        dest  += 4;
        laddr += 4;
    }

    if (remain > 0) {
        d.val = ptrace(PTRACE_PEEKTEXT, pid, dest, 0);
        for (i = 0; i < remain; i ++) {
            d.chars[i] = *laddr ++;
        }

        ptrace(PTRACE_POKETEXT, pid, dest, d.val);
    }

    return 0;
}

/* 修改tracee进程的寄存器, stack和PC使得其跳转到运行位于addr的函数 */
#if defined(__arm__)
int ptrace_call(pid_t pid, uint32_t addr, long *params, uint32_t num_params, struct pt_regs* regs)
{
    uint32_t i;
    // 前面４个参数存放到寄存器里
    for (i = 0; i < num_params && i < 4; i ++) {
        regs->uregs[i] = params[i];
    }

    //
    // push remain params into stack
    //
    if (i < num_params) {
    	// 栈顶指针sp往低地址移动，减去剩余参数的地址数（栈顶往“上”挪以容下剩余参数）
        regs->ARM_sp -= (num_params - i) * sizeof(long);
        // 往tracee的栈写入剩余参数
        ptrace_writedata(pid, (void *)regs->ARM_sp, (uint8_t *)&params[i], (num_params - i) * sizeof(long));
    }

    // 将PC寄存器指向目标函数, PS:　与x86不同，ARM中IP不是指令计数器，而是通用寄存器
    regs->ARM_pc = addr;
    if (regs->ARM_pc & 1) {
        /* 16位的thumb格式 */
        regs->ARM_pc &= (~1u);
        regs->ARM_cpsr |= CPSR_T_MASK;
    } else {
        /* arm格式 */
        regs->ARM_cpsr &= ~CPSR_T_MASK;
    }

    regs->ARM_lr = 0;

    // 将构造好的寄存器内容写入目标进程寄存器
    if (ptrace_setregs(pid, regs) == -1
    		//　恢复目标进程执行，将从调用函数地址即addr开始执行，参数为新赋值过来的寄存器内容
            || ptrace_continue(pid) == -1) {
        printf("error\n");
        return -1;
    }

    int stat = 0;
    waitpid(pid, &stat, WUNTRACED);
    while (stat != 0xb7f) {
        if (ptrace_continue(pid) == -1) {
            printf("error\n");
            return -1;
        }
        waitpid(pid, &stat, WUNTRACED);
    }

    return 0;
}

#elif defined(__i386__)
long ptrace_call(pid_t pid, uint32_t addr, long *params, uint32_t num_params, struct user_regs_struct * regs)
{
    regs->esp -= (num_params) * sizeof(long) ;
    ptrace_writedata(pid, (void *)regs->esp, (uint8_t *)params, (num_params) * sizeof(long));

    long tmp_addr = 0x00;
    regs->esp -= sizeof(long);
    ptrace_writedata(pid, regs->esp, (char *)&tmp_addr, sizeof(tmp_addr));

    regs->eip = addr;

    if (ptrace_setregs(pid, regs) == -1
            || ptrace_continue( pid) == -1) {
        printf("error\n");
        return -1;
    }

    int stat = 0;
    waitpid(pid, &stat, WUNTRACED);
    while (stat != 0xb7f) {
        if (ptrace_continue(pid) == -1) {
            printf("error\n");
            return -1;
        }
        waitpid(pid, &stat, WUNTRACED);
    }

    return 0;
}
#else
#error "Not supported"
#endif

/* 获取tracee寄存器值保存至regs内　*/
int ptrace_getregs(pid_t pid, struct pt_regs * regs)
{
    if (ptrace(PTRACE_GETREGS, pid, NULL, regs) < 0) {
        perror("ptrace_getregs: Can not get register values");
        return -1;
    }

    return 0;
}

int ptrace_setregs(pid_t pid, struct pt_regs * regs)
{
    if (ptrace(PTRACE_SETREGS, pid, NULL, regs) < 0) {
        perror("ptrace_setregs: Can not set register values");
        return -1;
    }

    return 0;
}

/* 使得tracee进程继续运行直到下一个syscall */
int ptrace_continue(pid_t pid)
{
    if (ptrace(PTRACE_CONT, pid, NULL, 0) < 0) {
        perror("ptrace_cont");
        return -1;
    }

    return 0;
}

/* ref: play with trace, part II */
int ptrace_attach(pid_t pid)
{
    if (ptrace(PTRACE_ATTACH, pid, NULL, 0) < 0) {
        perror("ptrace_attach");
        return -1;
    }

    int status = 0;
    waitpid(pid, &status , WUNTRACED);

    return 0;
}

int ptrace_detach(pid_t pid)
{
    if (ptrace(PTRACE_DETACH, pid, NULL, 0) < 0) {
        perror("ptrace_detach");
        return -1;
    }

    return 0;
}

/* 获取动态库(module_name)加载入目标进程(pid)后的起始地址
 * HSQ说然而此函数并没有什么鸟用
 * “系统调用层的地址各个进程都一样，无需重新计算偏移，可直接使用”
 * */
void* get_module_base(pid_t pid, const char* module_name)
{
    FILE *fp;
    long addr = 0;
    char *pch;
    char filename[32];
    char line[1024];

    if (pid < 0) {
        /* self process */
    	// /proc/self 是一个有趣的子目录，它使得程序可以方便地使用 /proc 查找本进程地信息
    	// /proc/self 是一个链接到 /proc 中访问 /proc 的进程所对应的 PID 的目录的符号链接。
        snprintf(filename, sizeof(filename), "/proc/self/maps", pid);
    } else {
    	// 利用 /proc/pid/maps 文件可以得到进程pid的地址空间进而得到module_name(.so)映射到内存的起始地址
    	// snprintf将"/proc/%d/maps"复制到filename
        snprintf(filename, sizeof(filename), "/proc/%d/maps", pid);
    }

    // 读入"/proc/%d/maps"
    fp = fopen(filename, "r");

    if (fp != NULL) {
    	// 每次读入一行，
    	// e.g. 402cd000-402cf000 r-xp 00000000 103:04 667812    /data/local/tmp/libhello.so
        while (fgets(line, sizeof(line), fp)) {
        	// strstr() 函数搜索module_name在line中的第一次出现
            if (strstr(line, module_name)) {
            	// 以"-"为分隔符将每一行字符串内容分开，得到402cd000
                pch = strtok(line, "-");
                // 将pch内容换成无符号长整形addr
                addr = strtoul(pch, NULL, 16);

                if (addr == 0x8000)
                    addr = 0;

                break;
            }
        }

        fclose(fp) ;
    }

    // "无类型指针”,可以指向任何数据类型
    return (void *)addr;
}

/* 获取目标进程内某个动态库函数module_name的某函数在该进程的虚拟内存的地址
 * local_addr存放该函数在tracer所在位置
 * */
void* get_remote_addr(pid_t target_pid, const char* module_name, void* local_addr)
{
    void* local_handle, *remote_handle;

    // tracer内相应动态库地址
    local_handle = get_module_base(-1, module_name);
    // tracee内相应动态库地址
    remote_handle = get_module_base(target_pid, module_name);

    DEBUG_PRINT("[+] get_remote_addr: local[%x], remote[%x]\n", local_handle, remote_handle);

    // local_addr - local_handle = offset,　再加上remote_handle,　得到目标进程相应函数的地址
    // 因为同一module_name动态库内某函数所在位置是一致的，所以对于tracer和trace offset是相同的
    // 只要找到动态库在目标进程的位置，再加上offset即是该函数在tracee的位置
    void * ret_addr = (void *)((uint32_t)local_addr + (uint32_t)remote_handle - (uint32_t)local_handle);

#if defined(__i386__)
    if (!strcmp(module_name, libc_path)) {
        ret_addr += 2;
    }
#endif
    return ret_addr;
}

/* 通过进程名寻找进程号　*/
int find_pid_of(const char *process_name)
{
    int id;
    pid_t pid = -1;
    DIR* dir;
    FILE *fp;
    char filename[32];
    char cmdline[256];

    struct dirent * entry;

    if (process_name == NULL)
        return -1;

    dir = opendir("/proc");
    if (dir == NULL)
        return -1;

    while((entry = readdir(dir)) != NULL) {
        id = atoi(entry->d_name);
        if (id != 0) {
            sprintf(filename, "/proc/%d/cmdline", id);
            fp = fopen(filename, "r");
            if (fp) {
                fgets(cmdline, sizeof(cmdline), fp);
                fclose(fp);

                if (strcmp(process_name, cmdline) == 0) {
                    /* process found */
                    pid = id;
                    break;
                }
            }
        }
    }

    closedir(dir);
    return pid;
}

/* 获取函数调用后的返回值　*/
long ptrace_retval(struct pt_regs * regs)
{
#if defined(__arm__)
    return regs->ARM_r0;
#elif defined(__i386__)
    return regs->eax; // rax for x64
#else
#error "Not supported"
#endif
}

/* 获取程序计数器内容　*/
long ptrace_ip(struct pt_regs * regs)
{
#if defined(__arm__)
    return regs->ARM_pc;
#elif defined(__i386__)
    return regs->eip;
#else
#error "Not supported"
#endif
}

/* ptrace_call的封装　*/
int ptrace_call_wrapper(pid_t target_pid, const char *func_name,
		void *func_addr, long *parameters, int param_num, struct pt_regs *regs)
{
	// func_addr 保存tracee进程内要调用的函数地址；parameters保存函数调用参数，param_num为参数个数；regs保存tracee内寄存器内容
    DEBUG_PRINT("[+] Calling %s in target process.\n", func_name);
    if (ptrace_call(target_pid, (uint32_t)func_addr, parameters, param_num, regs) == -1)
        return -1;

    if (ptrace_getregs(target_pid, regs) == -1)
        return -1;
    DEBUG_PRINT("[+] Target process returned from %s, return value=%x, pc=%x \n",
            func_name, ptrace_retval(regs), ptrace_ip(regs));
    return 0;
}

/* 实现核心功能--注入 */
int inject_remote_process(pid_t target_pid, const char *library_path,
		const char *function_name, const char *param, size_t param_size)
{
    int ret = -1;
    // 存放目标进程地址
    void *mmap_addr, *dlopen_addr, *dlsym_addr, *dlclose_addr, *dlerror_addr;
    void *local_handle, *remote_handle, *dlhandle;
    uint8_t *map_base = 0;　// 存放目标进程mmap获取的内存块地址，mmap将一个文件或者其它对象映射进内存
    uint8_t *dlopen_param1_ptr, *dlsym_param2_ptr, *saved_r0_pc_ptr, *inject_param_ptr, *remote_code_ptr, *local_code_ptr;

    struct pt_regs regs, original_regs;
    extern uint32_t _dlopen_addr_s, _dlopen_param1_s, _dlopen_param2_s, _dlsym_addr_s, \
        _dlsym_param2_s, _dlclose_addr_s, _inject_start_s, _inject_end_s, _inject_function_param_s, \
        _saved_cpsr_s, _saved_r0_pc_s;

    uint32_t code_length;
    long parameters[10];

    DEBUG_PRINT("[+] Injecting process: %d\n", target_pid);

    // step 1. attach到目标进程
    if (ptrace_attach(target_pid) == -1)
        goto exit;

    // step 2. save context, 保存目标进程被注入前的寄存器内容
    // 方便注入完成后恢复
    if (ptrace_getregs(target_pid, &regs) == -1)
        goto exit;

    /* save original registers */
    memcpy(&original_regs, &regs, sizeof(regs));

    /* step 3. 获取目标进程存放mmap()代码的地址，执行mmap调用，
     * 在目标进程分配一块地址，用于存放后面要注入的库路径和相关函数地址等
     * mmap()会开辟一块内存，用于将一个文件或者其它对象映射进内存
     */
    // 寻找目标进程mmap的地址
    // libc为c语音标准库，一般进程都会加载；libc.so包含mmap函数
    mmap_addr = get_remote_addr(target_pid, libc_path, (void *)mmap);
    DEBUG_PRINT("[+] Remote mmap address: %x\n", mmap_addr);

    parameters[0] = 0;  // addr
    parameters[1] = 0x4000; // size
    parameters[2] = PROT_READ | PROT_WRITE | PROT_EXEC;  // prot
    parameters[3] =  MAP_ANONYMOUS | MAP_PRIVATE; // flags
    parameters[4] = 0; //fd
    parameters[5] = 0; //offset

    // 目标进程执行mmap
    if (ptrace_call_wrapper(target_pid, "mmap", mmap_addr, parameters, 6, &regs) == -1)
        goto exit;

    map_base = ptrace_retval(&regs);// mmap调用后返回值存入regs内的ax寄存器，syscall之前ax存调用号，调用后存返回值

    // step 4. 获取目标进程动态库的几个函数，并将要注入的so的路径写入刚刚申请的内存初始地址
    // dlopen（）函数以指定模式打开指定的动态链接库文件，并返回一个句柄给dlsym（）的调用进程。使用dlclose（）来卸载打开的库。
    dlopen_addr = get_remote_addr(target_pid, linker_path, (void *)dlopen);　// 找到dlopen()的地址
    dlsym_addr = get_remote_addr(target_pid, linker_path, (void *)dlsym);　// 找到dlsys()地址
    dlclose_addr = get_remote_addr(target_pid, linker_path, (void *)dlclose);　// 找到dlclose()地址
    dlerror_addr = get_remote_addr(target_pid, linker_path, (void *)dlerror);　// 找到dlerror()地址

    DEBUG_PRINT("[+] Get imports: dlopen: %x, dlsym: %x, dlclose: %x, dlerror: %x\n",
            dlopen_addr, dlsym_addr, dlclose_addr, dlerror_addr);

    printf("library path = %s\n", library_path);
    // 将要注入的so的路径写入刚刚申请的内存初始地址，作为即将要调用的dlopen函数的参数parameters[0]
    ptrace_writedata(target_pid, map_base, library_path, strlen(library_path) + 1);

    // step 5. 在目标进程内调用dlopen函数加载要注入的so
    // 完成后so已经被注入目标进程的地址空间内了
    // 当库被装入后，可以把 dlopen() 返回的句柄作为给 dlsym() 的第一个参数，以获得符号在库中的地址。
    // 使用这个地址，就可以获得库中特定函数的指针，并且调用装载库中的相应函数。
    parameters[0] = map_base;
    parameters[1] = RTLD_NOW| RTLD_GLOBAL;

    if (ptrace_call_wrapper(target_pid, "dlopen", dlopen_addr, parameters, 2, &regs) == -1)
        goto exit;

    // step 6. 在目标进程内调用dlsym函数获取刚刚注入的so里的hook函数
    void * sohandle = ptrace_retval(&regs);

#define FUNCTION_NAME_ADDR_OFFSET       0x100
    ptrace_writedata(target_pid, map_base + FUNCTION_NAME_ADDR_OFFSET, function_name, strlen(function_name) + 1);
    parameters[0] = sohandle;
    parameters[1] = map_base + FUNCTION_NAME_ADDR_OFFSET;

    if (ptrace_call_wrapper(target_pid, "dlsym", dlsym_addr, parameters, 2, &regs) == -1)
        goto exit;

    // step 7. 在目标进程内调用hook函数
    void * hook_entry_addr = ptrace_retval(&regs); // dlsys()返回hook函数地址
    DEBUG_PRINT("hook_entry_addr = %p\n", hook_entry_addr);

#define FUNCTION_PARAM_ADDR_OFFSET      0x200
    ptrace_writedata(target_pid, map_base + FUNCTION_PARAM_ADDR_OFFSET, param, strlen(param) + 1);
    parameters[0] = map_base + FUNCTION_PARAM_ADDR_OFFSET;
    function_name
    if (ptrace_call_wrapper(target_pid, "hook_entry", hook_entry_addr, parameters, 1, &regs) == -1)
        goto exit;

    printf("Press enter to dlclose and detach\n");
    getchar();
    parameters[0] = sohandle;

    if (ptrace_call_wrapper(target_pid, "dlclose", dlclose, parameters, 1, &regs) == -1)
        goto exit;

    /* restore */
    // step 8. 恢复目标进程的寄存器，detach　ptrace
    ptrace_setregs(target_pid, &original_regs);
    ptrace_detach(target_pid);
    ret = 0;

exit:
    return ret;
}

int main(int argc, char* argv[]) {
    pid_t target_pid;
    // 通过指定目标进程名（即可执行文件名）完成注入
    /* target_pid = find_pid_of("//system/bin/surfaceflinger");
    if (-1 == target_pid) {
        printf("Can't find the process\n");
        return -1;
    }
    */

    // 通过指定进程号完成注入
    if (argc == 0) {
    	printf("Please input the pid!");
    	exit(-1);
    } else
    	target_pid = atoi(argv[1]);

    // 将位于library_path的动态链接库的function_name注入到target_pid并执行
    const char *library_path = "/data/local/tmp/libhello.so";
    const char *function_name = "hook_entry";
    const char *function_parameters = "I'm parameter!";
    inject_remote_process(target_pid, library_path, function_name, function_parameters, strlen(function_parameters));
    return 0;
}
