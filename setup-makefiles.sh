#!/bin/bash
#
# Copyright (C) 2016 The LineageOS Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#

set -e

# Load extract_utils and do some sanity checks
MY_DIR="${BASH_SOURCE%/*}"
if [[ ! -d "$MY_DIR" ]]; then MY_DIR="$PWD"; fi

CM_ROOT="$MY_DIR"/../../..

HELPER="$CM_ROOT"/tools/extract-utils/extract_utils.sh
if [ ! -f "$HELPER" ]; then
    echo "Unable to find helper script at $HELPER"
    exit 1
fi
. "$HELPER"

# Initialize the helper for common device
setup_vendor "$DEVICE_COMMON" "$VENDOR" "$CM_ROOT" true

# Copyright headers and common guards
write_headers "ef56 ef59 ef60 ef63 ef65"

write_makefiles "$MY_DIR"/proprietary-files.txt
write_makefiles "$MY_DIR"/proprietary-files-qc.txt

# The extract utils only know the apps of app/ and priv-app/: install the RRO
# of phh IMS to the product overlays.
cat << EOF >> "$ANDROIDMK"

include \$(CLEAR_VARS)
LOCAL_MODULE := PhhImsOverlay
LOCAL_MODULE_OWNER := $VENDOR
LOCAL_SRC_FILES := proprietary/product/overlay/PhhImsOverlay.apk
LOCAL_CERTIFICATE := platform
LOCAL_MODULE_TAGS := optional
LOCAL_MODULE_CLASS := APPS
LOCAL_DEX_PREOPT := false
LOCAL_MODULE_SUFFIX := .apk
LOCAL_MODULE_PATH := \$(TARGET_OUT_PRODUCT)/overlay
include \$(BUILD_PREBUILT)
EOF
printf '\n%s\n' "PRODUCT_PACKAGES += PhhImsOverlay" >> "$PRODUCTMK"

write_footers

# Reinitialize the helper for device
setup_vendor "$DEVICE" "$VENDOR" "$CM_ROOT"

# Copyright headers and guards
write_headers

write_makefiles "$MY_DIR"/../$DEVICE/device-proprietary-files.txt

write_footers
