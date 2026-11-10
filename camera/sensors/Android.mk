LOCAL_PATH := $(call my-dir)

# Sensor libraries for the hammerhead camera daemon, one per sensor.
# The register tables differ between devices and live in
# <sensor>/data/$(TARGET_DEVICE).

PANTECH_CAMERA_SENSORS := imx135 imx214 s5k6b2yx

define pantech-camera-sensor
include $$(CLEAR_VARS)
LOCAL_MODULE := libmmcamera_$(1)
LOCAL_SRC_FILES := \
    common/sensor_exposure.c \
    $(1)/$(1)_exposure.c \
    $(1)/data/$(TARGET_DEVICE)/$(1)_lib.c
LOCAL_C_INCLUDES := $$(LOCAL_PATH)/include
LOCAL_HEADER_LIBRARIES := generated_kernel_headers
LOCAL_SHARED_LIBRARIES := liblog
LOCAL_CXX_STL := none
LOCAL_CFLAGS := -Wall -Werror -fvisibility=hidden
LOCAL_MODULE_TAGS := optional
LOCAL_32_BIT_ONLY := true
LOCAL_VENDOR_MODULE := true
include $$(BUILD_SHARED_LIBRARY)
endef

$(foreach s,$(PANTECH_CAMERA_SENSORS),\
    $(if $(wildcard $(LOCAL_PATH)/$(s)/data/$(TARGET_DEVICE)),\
        $(eval $(call pantech-camera-sensor,$(s)))))

# Calibration libraries for the EEPROMs of the kernel, by qcom,eeprom-name
PANTECH_CAMERA_EEPROMS := rohm_eeprom rumba_sa_eeprom

define pantech-camera-eeprom
include $$(CLEAR_VARS)
LOCAL_MODULE := libmmcamera_$(1)
LOCAL_SRC_FILES := eeprom/pantech_eeprom.c
LOCAL_SHARED_LIBRARIES := libdl liblog
LOCAL_CXX_STL := none
LOCAL_CFLAGS := -Wall -Werror -fvisibility=hidden -DEEPROM_NAME=$(1)
LOCAL_MODULE_TAGS := optional
LOCAL_32_BIT_ONLY := true
LOCAL_VENDOR_MODULE := true
include $$(BUILD_SHARED_LIBRARY)
endef

$(foreach e,$(PANTECH_CAMERA_EEPROMS),$(eval $(call pantech-camera-eeprom,$(e))))
