/*
 * Copyright (C) 2026 The LineageOS Project
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Toggles the airplane mode, so that the modem detaches and attaches again: the
 * network only learns a new voice domain preference of the modem on an attach.
 * Started by init when qmi_pcscf changed it (SKT).
 */

#define LOG_TAG "ims_reattach"

#include <binder/IBinder.h>
#include <binder/IResultReceiver.h>
#include <binder/IServiceManager.h>
#include <binder/IShellCallback.h>
#include <binder/ProcessState.h>
#include <fcntl.h>
#include <log/log.h>
#include <unistd.h>
#include <utils/Condition.h>
#include <utils/Mutex.h>

using namespace android;

namespace {

class ResultReceiver : public BnResultReceiver {
  public:
    void send(int32_t resultCode) override {
        AutoMutex _l(mMutex);
        mResult = resultCode;
        mHaveResult = true;
        mCondition.signal();
    }

    int32_t waitForResult() {
        AutoMutex _l(mMutex);
        while (!mHaveResult) mCondition.wait(mMutex);
        return mResult;
    }

  private:
    Mutex mMutex;
    Condition mCondition;
    bool mHaveResult = false;
    int32_t mResult = 0;
};

// "cmd connectivity airplane-mode <state>", straight through binder
bool setAirplaneMode(const char* state) {
    sp<IBinder> connectivity = defaultServiceManager()->waitForService(String16("connectivity"));
    if (connectivity == nullptr) {
        ALOGE("No connectivity service");
        return false;
    }

    int fd = open("/dev/null", O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        ALOGE("Could not open /dev/null");
        return false;
    }
    Vector<String16> args;
    args.add(String16("airplane-mode"));
    args.add(String16(state));
    sp<ResultReceiver> result = new ResultReceiver();
    status_t err = IBinder::shellCommand(connectivity, fd, fd, fd, args, nullptr, result);
    int32_t rc = err == OK ? result->waitForResult() : -1;
    close(fd);

    if (err != OK || rc != 0) {
        ALOGE("airplane-mode %s failed: %d %d", state, err, rc);
        return false;
    }
    return true;
}

}  // namespace

int main() {
    ProcessState::self()->startThreadPool();

    ALOGI("Attaching again for the new voice domain preference");
    if (!setAirplaneMode("enable")) return 1;
    sleep(5);
    return setAirplaneMode("disable") ? 0 : 1;
}
