#include <stdio.h>
#include <unistd.h>
#include <sys/ptrace.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <sys/reg.h>

int main() {
  puts("Parent started");
  pid_t pid;
  pid = fork();
  if(pid < 0) {
    puts("fork() failed");
    return(-1);
  }
  if(pid == 0) {
      /* child process */
    ptrace(PTRACE_TRACEME, 0, 0, 0);
    puts("Child sleeping...");
    sleep(1);
    puts("Child exec...");
    execlp("./target", "target", NULL);
  } else {
    printf("Child PiD == %d\n", pid);
    int status = 0;
    struct rusage ru;
    wait4(pid, &status, 0, &ru);
    long rax_rt = ptrace(PTRACE_PEEKUSER, pid, 8 * RAX, 0);
    printf("Child execve() returned with %ld\n",rax_rt);
    ptrace(PTRACE_SYSCALL, pid, 0, 0);
    int intocall=1;
    while(1) {
      wait4(pid, &status, 0, &ru);
      if (WIFEXITED(status)){
        puts("Child Exited");
        break;
      }
      long _ORIG_RAX = ptrace(PTRACE_PEEKUSER, pid, 8 * ORIG_RAX, 0);
      long _RAX = ptrace(PTRACE_PEEKUSER, pid, 8 * RAX, 0);
      if(intocall) {
        printf("Entering SYSCALL %ld .... ", _ORIG_RAX);
        intocall = 0;
      } else {
        printf("Exited with %ld\n",_RAX);
        intocall = 1;
      }
      ptrace(PTRACE_SYSCALL, pid, 0, 0);
    }
  }
}
