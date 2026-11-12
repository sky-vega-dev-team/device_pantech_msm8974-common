/*
 * Copyright (C) 2026 The LineageOS Project
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "android.hardware.power-service.msm8974"

#include <aidl/android/hardware/power/BnPower.h>
#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/strings.h>

#include <chrono>
#include <mutex>
#include <string>
#include <thread>

using ::aidl::android::hardware::power::Mode;
using ::android::base::ReadFileToString;
using ::android::base::Trim;
using ::android::base::WriteStringToFile;

namespace aidl {
namespace android {
namespace hardware {
namespace power {
namespace impl {

/*
 * There is no perf lock server on msm8974: mpdecision was replaced by the
 * bricked hotplug driver and perfd never existed for this platform. Handle
 * the modes that would need a perf lock here, with the kernel knobs.
 * Touch boost is done in the kernel by cpu_boost.
 */
static constexpr const char* kMinCpusOnline = "/sys/kernel/msm_mpdecision/conf/min_cpus_online";
static constexpr const char* kInteractiveBoost = "/sys/devices/system/cpu/cpufreq/interactive/boost";
static constexpr const char* kTimerRate = "/sys/devices/system/cpu/cpufreq/interactive/timer_rate";

// Display off: sample the load less often, like the stock 8974 power HAL.
static constexpr const char* kTimerRateDisplayOff = "50000";
// Drop the launch boost if the framework never ends it.
static constexpr auto kMaxLaunchDuration = std::chrono::seconds(5);

static std::mutex gLock;
static bool gLaunching;
static unsigned int gLaunchId;
static std::string gSavedMinCpus;
static std::string gSavedTimerRate;

static void writeNode(const char* node, const std::string& value) {
    if (!WriteStringToFile(value, node)) {
        PLOG(ERROR) << "Failed to write " << value << " to " << node;
    }
}

static std::string readNode(const char* node, const char* fallback) {
    std::string value;
    if (!ReadFileToString(node, &value)) {
        PLOG(ERROR) << "Failed to read " << node;
        return fallback;
    }
    value = Trim(value);
    return value.empty() ? fallback : value;
}

// Called with gLock held.
static void setLaunchBoostLocked(bool enabled) {
    if (enabled == gLaunching) return;

    if (enabled) {
        gSavedMinCpus = readNode(kMinCpusOnline, "1");
        writeNode(kMinCpusOnline, "4");
        writeNode(kInteractiveBoost, "1");
    } else {
        writeNode(kInteractiveBoost, "0");
        writeNode(kMinCpusOnline, gSavedMinCpus);
    }
    gLaunching = enabled;
}

static void setLaunchBoost(bool enabled) {
    std::lock_guard<std::mutex> lock(gLock);

    setLaunchBoostLocked(enabled);
    if (!enabled) return;

    unsigned int id = ++gLaunchId;
    std::thread([id] {
        std::this_thread::sleep_for(kMaxLaunchDuration);
        std::lock_guard<std::mutex> lock(gLock);
        if (gLaunching && id == gLaunchId) {
            LOG(INFO) << "Launch boost timed out";
            setLaunchBoostLocked(false);
        }
    }).detach();
}

static void setInteractive(bool interactive) {
    std::lock_guard<std::mutex> lock(gLock);

    if (!interactive) {
        if (gSavedTimerRate.empty()) gSavedTimerRate = readNode(kTimerRate, "");
        writeNode(kTimerRate, kTimerRateDisplayOff);
    } else if (!gSavedTimerRate.empty()) {
        writeNode(kTimerRate, gSavedTimerRate);
        gSavedTimerRate.clear();
    }
    // Cores are taken offline on suspend by the hotplug driver itself.
}

bool isDeviceSpecificModeSupported(Mode type, bool* _aidl_return) {
    switch (type) {
        case Mode::LAUNCH:
        case Mode::INTERACTIVE:
            *_aidl_return = true;
            return true;
        default:
            return false;
    }
}

bool setDeviceSpecificMode(Mode type, bool enabled) {
    switch (type) {
        case Mode::LAUNCH:
            setLaunchBoost(enabled);
            return true;
        case Mode::INTERACTIVE:
            setInteractive(enabled);
            return true;
        default:
            return false;
    }
}

}  // namespace impl
}  // namespace power
}  // namespace hardware
}  // namespace android
}  // namespace aidl
