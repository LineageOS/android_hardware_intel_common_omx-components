LOCAL_PATH := $(call my-dir)
include $(CLEAR_VARS)

LOCAL_SRC_FILES := \
	sst.cpp

LOCAL_MODULE_TAGS := optional
LOCAL_MODULE := libwrs_omxil_intel_mrst_sst

LOCAL_CPPFLAGS :=

LOCAL_LDFLAGS :=

LOCAL_SHARED_LIBRARIES := \
	libwrs_omxil_common \
	liblog \
	libmixcommon \
	libmixaudio

LOCAL_C_INCLUDES := \
	$(TARGET_OUT_HEADERS)/wrs_omxil_core \
	$(TARGET_OUT_HEADERS)/khronos/openmax \
	$(PV_INCLUDES) \
	$(TARGET_OUT_HEADERS)/libmixcommon \
	$(TARGET_OUT_HEADERS)/libmixaudio

include $(BUILD_SHARED_LIBRARY)
