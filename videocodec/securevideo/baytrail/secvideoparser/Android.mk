LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)
LOCAL_COPY_HEADERS_TO := secvideoparser
LOCAL_COPY_HEADERS := secvideoparser.h
include $(BUILD_COPY_HEADERS)

include $(CLEAR_VARS)
LOCAL_PREBUILT_LIBS := libsecvideoparser.so
LOCAL_MODULE_TAGS := optional
include $(BUILD_MULTI_PREBUILT)
