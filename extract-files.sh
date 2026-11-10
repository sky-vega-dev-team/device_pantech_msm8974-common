#!/bin/bash
#
# Copyright (C) 2016 The LineageOS Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#

set -e

# Load extractutils and do some sanity checks
MY_DIR="${BASH_SOURCE%/*}"
if [[ ! -d "$MY_DIR" ]]; then MY_DIR="$PWD"; fi

CM_ROOT="$MY_DIR"/../../..

HELPER="$CM_ROOT"/vendor/lineage/build/tools/extract_utils.sh
if [ ! -f "$HELPER" ]; then
    echo "Unable to find helper script at $HELPER"
    exit 1
fi
. "$HELPER"

if [ $# -gt 2 ]; then
    echo "$0: bad number of arguments"
    echo ""
    echo "usage: $0 [PATH_TO_EXPANDED_ROM] [PATH_TO_QC_BLOBS]"
    echo ""
    echo "If PATH_TO_EXPANDED_ROM is not specified, blobs will be extracted from"
    echo "the device using adb pull. PATH_TO_QC_BLOBS defaults to the extracted"
    echo "Nexus 5 blobs in vendor/lge/hammerhead/proprietary."
    exit 1
fi

SRC="${1:-adb}"
SRC_QC="${2:-$CM_ROOT/vendor/lge/hammerhead/proprietary}"

# Initialize the helper for common device
setup_vendor "$DEVICE_COMMON" "$VENDOR" "$CM_ROOT" true

extract "$MY_DIR"/proprietary-files.txt "$SRC"
extract "$MY_DIR"/proprietary-files-qc.txt "$SRC_QC"

# Reinitialize the helper for device
setup_vendor "$DEVICE" "$VENDOR" "$CM_ROOT"

extract "$MY_DIR"/../$DEVICE/device-proprietary-files.txt "$SRC"

"$MY_DIR"/setup-makefiles.sh
