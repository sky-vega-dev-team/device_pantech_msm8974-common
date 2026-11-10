LOCAL_PATH := $(call my-dir)

# Running the hammerhead ISP module on both VFEs, loaded as its shim
include $(CLEAR_VARS)
LOCAL_MODULE := libshim_isp_dual
LOCAL_SRC_FILES := isp_dual_shim.c
LOCAL_SHARED_LIBRARIES := liblog
LOCAL_CXX_STL := none
LOCAL_CFLAGS := -Wall -Werror -fvisibility=hidden
LOCAL_MODULE_TAGS := optional
LOCAL_32_BIT_ONLY := true
LOCAL_VENDOR_MODULE := true
include $(BUILD_SHARED_LIBRARY)
