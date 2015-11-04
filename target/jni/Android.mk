LOCAL_PATH := $(call my-dir)
include $(CLEAR_VARS)
LOCAL_MODULE    := target
LOCAL_SRC_FILES := ../target.c
# BUILD_EXECUTABLE指明生成可执行的二进制文件
include $(BUILD_EXECUTABLE)