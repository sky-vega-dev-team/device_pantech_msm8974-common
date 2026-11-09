/*
 * Copyright (C) 2014 The Android Open Source Project
 * Copyright (C) 2016 The Mokee Project
 * Copyright (C) 2016 The CyanogenMod Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_TAG "FingerprintHal_msm8974"
#define LOG_NDEBUG 1

#include <dlfcn.h>
#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include <cutils/log.h>

#include "fpc1080.h"

/******************************************************************************/

static void notify_msg(fpc_fingerprint_device_t* vdev, const fingerprint_msg_t* msg) {
    fingerprint_notify_t notify;

    pthread_mutex_lock(&vdev->lock);
    notify = vdev->device.notify;
    pthread_mutex_unlock(&vdev->lock);

    if (notify != NULL)
        notify(msg);
}

static uint32_t get_active_gid(fpc_fingerprint_device_t* vdev) {
    uint32_t gid;

    pthread_mutex_lock(&vdev->lock);
    gid = vdev->active_gid;
    pthread_mutex_unlock(&vdev->lock);

    return gid;
}

static void send_error_notice(fpc_fingerprint_device_t* vdev, fingerprint_error_t error_info) {
    fingerprint_msg_t msg = {0};
    msg.type = FINGERPRINT_ERROR;
    msg.data.error = error_info;
    ALOGI("error_info=%d", (int)error_info);

    notify_msg(vdev, &msg);
}

static void send_acquired_notice(fpc_fingerprint_device_t* vdev,
        fingerprint_acquired_info_t acquired_info) {
    fingerprint_msg_t msg = {0};
    msg.type = FINGERPRINT_ACQUIRED;
    msg.data.acquired.acquired_info = acquired_info;
    ALOGI("acqu_info=%d", (int)acquired_info);

    notify_msg(vdev, &msg);
}

static void send_enroll_notice(fpc_fingerprint_device_t* vdev, uint32_t fid, uint32_t remaining) {
    fingerprint_msg_t msg = {0};
    msg.type = FINGERPRINT_TEMPLATE_ENROLLING;
    msg.data.enroll.finger.gid = get_active_gid(vdev);
    msg.data.enroll.finger.fid = fid;
    msg.data.enroll.samples_remaining = remaining;

    notify_msg(vdev, &msg);
}

static void send_authenticated_notice(fpc_fingerprint_device_t* vdev, uint32_t fid) {
    struct timespec ts;
    fingerprint_msg_t msg = {0};

    send_acquired_notice(vdev, FINGERPRINT_ACQUIRED_GOOD);

    msg.type = FINGERPRINT_AUTHENTICATED;
    msg.data.authenticated.finger.gid = get_active_gid(vdev);
    msg.data.authenticated.finger.fid = fid;
    msg.data.authenticated.hat.version = HW_AUTH_TOKEN_VERSION;
    msg.data.authenticated.hat.authenticator_type = htobe32(HW_AUTH_FINGERPRINT);

    pthread_mutex_lock(&vdev->lock);
    msg.data.authenticated.hat.challenge = vdev->op_id;
    msg.data.authenticated.hat.authenticator_id = vdev->authenticator_id;
    msg.data.authenticated.hat.user_id = vdev->secure_user_id;
    pthread_mutex_unlock(&vdev->lock);

    clock_gettime(CLOCK_MONOTONIC, &ts);
    msg.data.authenticated.hat.timestamp =
            htobe64((uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000);

    notify_msg(vdev, &msg);
}

static void send_remove_notice(fpc_fingerprint_device_t* vdev, uint32_t fid, uint32_t remaining) {
    fingerprint_msg_t msg = {0};
    msg.type = FINGERPRINT_TEMPLATE_REMOVED;
    msg.data.removed.finger.gid = get_active_gid(vdev);
    msg.data.removed.finger.fid = fid;
    msg.data.removed.remaining_templates = remaining;

    notify_msg(vdev, &msg);
}

static void send_enumerate_notice(fpc_fingerprint_device_t* vdev, uint32_t fid,
        uint32_t remaining) {
    fingerprint_msg_t msg = {0};
    msg.type = FINGERPRINT_TEMPLATE_ENUMERATING;
    msg.data.enumerated.finger.gid = get_active_gid(vdev);
    msg.data.enumerated.finger.fid = fid;
    msg.data.enumerated.remaining_templates = remaining;

    notify_msg(vdev, &msg);
}

/******************************************************************************/

static uint64_t now_ms(void) {
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static uint64_t get_64bit_rand(void) {
    uint64_t r = 0;

    arc4random_buf(&r, sizeof(r));
    return r != 0 ? r : 1;
}

static void get_template_path(fpc_fingerprint_device_t* vdev, char* path, size_t size) {
    pthread_mutex_lock(&vdev->lock);
    strlcpy(path, vdev->template_path, size);
    pthread_mutex_unlock(&vdev->lock);
}

/* Returns the number of enrolled fingers. Call with engine_lock held. */
static int list_fingers_locked(fpc_fingerprint_device_t* vdev, const char* path, int* list) {
    int count, kept = 0;
    int raw[FPC1080_MAX_FINGERS + 1];

    if (path[0] == '\0' || access(path, F_OK) != 0)
        return 0;

    count = vdev->engine.get_enrolled_finger_list(path, raw, FPC1080_MAX_FINGERS + 1);
    if (count < 0) {
        ALOGE("CS_GetEnrolledFingerList failed: %d", count);
        return 0;
    }
    if (count > FPC1080_MAX_FINGERS + 1)
        count = FPC1080_MAX_FINGERS + 1;

    for (int i = 0; i < count; i++) {
        if (raw[i] >= 1 && raw[i] <= FPC1080_MAX_FINGERS)
            list[kept++] = raw[i];
    }

    return kept;
}

static void remove_template_files(const char* path) {
    char backup[PATH_MAX];

    unlink(path);
    if (snprintf(backup, sizeof(backup), "%s.bak", path) < (int)sizeof(backup))
        unlink(backup);
}

/* The authenticator id has to change whenever the set of enrolled fingers changes. */
static void update_authenticator_id(fpc_fingerprint_device_t* vdev) {
    char path[PATH_MAX];
    int list[FPC1080_MAX_FINGERS];
    uint64_t mask = 0;
    int count;

    get_template_path(vdev, path, sizeof(path));

    pthread_mutex_lock(&vdev->engine_lock);
    count = list_fingers_locked(vdev, path, list);
    pthread_mutex_unlock(&vdev->engine_lock);

    for (int i = 0; i < count; i++)
        mask |= 1ULL << list[i];

    pthread_mutex_lock(&vdev->lock);
    vdev->authenticator_id = mask;
    pthread_mutex_unlock(&vdev->lock);
}

static bool operation_active(fpc_fingerprint_device_t* vdev, uint32_t generation) {
    bool active;

    pthread_mutex_lock(&vdev->lock);
    active = vdev->generation == generation;
    pthread_mutex_unlock(&vdev->lock);

    return active;
}

static void finish_operation(fpc_fingerprint_device_t* vdev, uint32_t generation) {
    pthread_mutex_lock(&vdev->lock);
    if (vdev->generation == generation)
        vdev->state = STATE_IDLE;
    pthread_mutex_unlock(&vdev->lock);
}

/* An operation was aborted: report it unless another one has replaced it. */
static void notify_if_canceled(fpc_fingerprint_device_t* vdev) {
    worker_state_t state;

    pthread_mutex_lock(&vdev->lock);
    state = vdev->state;
    pthread_mutex_unlock(&vdev->lock);

    if (state == STATE_IDLE)
        send_error_notice(vdev, FINGERPRINT_ERROR_CANCELED);
}

/*
 * Captures one swipe into vdev->frames and returns the number of frames.
 * Frames are read while the capture runs because the driver buffer is smaller
 * than a slow swipe. deadline_ms is ignored when 0.
 */
static int capture_swipe(fpc_fingerprint_device_t* vdev, uint32_t generation,
        uint64_t deadline_ms) {
    const size_t capacity = FPC1080_MAX_FRAMES * FPC1080_FRAME_SIZE;
    size_t offset = 0;
    int result = 0;
    int fd;

    fd = open(FPC1080_DEVICE, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        ALOGE("cannot open %s: %s", FPC1080_DEVICE, strerror(errno));
        return -errno;
    }

    if (ioctl(fd, FPC1080_IOCTL_START_CAPTURE, 0) != 0) {
        result = -errno;
        ALOGE("cannot start capture: %s", strerror(errno));
        close(fd);
        return result;
    }

    for (;;) {
        struct pollfd pfd = { .fd = fd, .events = POLLIN | POLLHUP };
        int ready;

        if (!operation_active(vdev, generation)) {
            ioctl(fd, FPC1080_IOCTL_ABORT_CAPTURE, 0);
            result = -ECANCELED;
            break;
        }
        if (deadline_ms != 0 && now_ms() >= deadline_ms) {
            ioctl(fd, FPC1080_IOCTL_ABORT_CAPTURE, 0);
            result = -ETIMEDOUT;
            break;
        }

        ready = poll(&pfd, 1, FPC1080_POLL_INTERVAL_MS);
        if (ready < 0) {
            if (errno == EINTR)
                continue;
            result = -errno;
            break;
        }
        if (ready == 0)
            continue;

        if (pfd.revents & POLLIN) {
            uint8_t scrap[FPC1080_FRAME_SIZE];
            uint8_t* target = offset < capacity ? vdev->frames + offset : scrap;
            size_t space = offset < capacity ? capacity - offset : sizeof(scrap);
            ssize_t count = read(fd, target, space);

            if (count < 0) {
                if (errno == EINTR)
                    continue;
                result = -errno;
                break;
            }
            if (target != scrap)
                offset += (size_t)count;
            continue;
        }

        if (pfd.revents & POLLHUP)
            break;
    }

    close(fd);
    return result < 0 ? result : (int)(offset / FPC1080_FRAME_SIZE);
}

static void discard_partial_enrollment(fpc_fingerprint_device_t* vdev, const char* path,
        uint32_t fid) {
    int count;
    int list[FPC1080_MAX_FINGERS];

    pthread_mutex_lock(&vdev->engine_lock);
    if (access(path, F_OK) == 0 && vdev->engine.is_enrolled((int)fid, path) > 0)
        vdev->engine.delete_template((int)fid, path);
    count = list_fingers_locked(vdev, path, list);
    if (count == 0)
        remove_template_files(path);
    pthread_mutex_unlock(&vdev->engine_lock);
}

static void do_enroll(fpc_fingerprint_device_t* vdev, uint32_t generation) {
    char path[PATH_MAX];
    uint32_t fid, timeout_sec;
    uint64_t deadline_ms = 0;
    int trial = 0;

    get_template_path(vdev, path, sizeof(path));

    pthread_mutex_lock(&vdev->lock);
    fid = vdev->enroll_fid;
    timeout_sec = vdev->enroll_timeout_sec;
    pthread_mutex_unlock(&vdev->lock);

    if (timeout_sec != 0)
        deadline_ms = now_ms() + (uint64_t)timeout_sec * 1000;

    while (trial < FPC1080_ENROLL_SAMPLES) {
        int frames, ret;

        frames = capture_swipe(vdev, generation, deadline_ms);
        if (frames == -ECANCELED) {
            discard_partial_enrollment(vdev, path, fid);
            notify_if_canceled(vdev);
            return;
        }
        if (frames < 0) {
            discard_partial_enrollment(vdev, path, fid);
            send_error_notice(vdev, frames == -ETIMEDOUT ? FINGERPRINT_ERROR_TIMEOUT
                                                         : FINGERPRINT_ERROR_HW_UNAVAILABLE);
            finish_operation(vdev, generation);
            return;
        }

        pthread_mutex_lock(&vdev->engine_lock);
        ret = vdev->engine.enrol(vdev->frames, frames, (int)fid, trial, path);
        pthread_mutex_unlock(&vdev->engine_lock);
        ALOGI("CS_Enrol: fid=%u trial=%d frames=%d ret=%d", fid, trial, frames, ret);

        if (ret == CS_ERROR_IMAGE_TOO_SHORT) {
            send_acquired_notice(vdev, FINGERPRINT_ACQUIRED_PARTIAL);
            continue;
        }
        if (ret != 0) {
            discard_partial_enrollment(vdev, path, fid);
            send_error_notice(vdev, FINGERPRINT_ERROR_UNABLE_TO_PROCESS);
            finish_operation(vdev, generation);
            return;
        }

        trial++;
        if (trial == FPC1080_ENROLL_SAMPLES)
            update_authenticator_id(vdev);
        send_acquired_notice(vdev, FINGERPRINT_ACQUIRED_GOOD);
        send_enroll_notice(vdev, fid, FPC1080_ENROLL_SAMPLES - trial);
    }

    finish_operation(vdev, generation);
}

static void do_scan(fpc_fingerprint_device_t* vdev, uint32_t generation) {
    char path[PATH_MAX];

    get_template_path(vdev, path, sizeof(path));

    for (;;) {
        int frames, ret;

        frames = capture_swipe(vdev, generation, 0);
        if (frames == -ECANCELED) {
            notify_if_canceled(vdev);
            return;
        }
        if (frames < 0) {
            send_error_notice(vdev, FINGERPRINT_ERROR_HW_UNAVAILABLE);
            finish_operation(vdev, generation);
            return;
        }

        pthread_mutex_lock(&vdev->engine_lock);
        vdev->engine.set_security_level(FPC1080_SECURITY_LEVEL);
        ret = vdev->engine.verify(vdev->frames, frames, path);
        pthread_mutex_unlock(&vdev->engine_lock);
        ALOGI("CS_Verify: frames=%d ret=%d", frames, ret);

        if (ret > 0) {
            send_authenticated_notice(vdev, (uint32_t)ret);
            finish_operation(vdev, generation);
            return;
        }
        if (ret == CS_ERROR_IMAGE_TOO_SHORT) {
            send_acquired_notice(vdev, FINGERPRINT_ACQUIRED_PARTIAL);
            continue;
        }
        if (ret == CS_ERROR_NO_MATCH || ret == 0) {
            send_authenticated_notice(vdev, 0);
            continue;
        }

        ALOGE("CS_Verify failed: %d", ret);
        send_acquired_notice(vdev, FINGERPRINT_ACQUIRED_INSUFFICIENT);
    }
}

static void* worker_thread(void* data) {
    fpc_fingerprint_device_t* vdev = (fpc_fingerprint_device_t*)data;

    for (;;) {
        worker_state_t state;
        uint32_t generation;

        pthread_mutex_lock(&vdev->lock);
        while (vdev->state == STATE_IDLE)
            pthread_cond_wait(&vdev->cond, &vdev->lock);
        state = vdev->state;
        generation = vdev->generation;
        pthread_mutex_unlock(&vdev->lock);

        if (state == STATE_EXIT)
            break;
        if (state == STATE_ENROLL)
            do_enroll(vdev, generation);
        else if (state == STATE_SCAN)
            do_scan(vdev, generation);
    }

    return NULL;
}

/* Call with vdev->lock held. */
static void request_state_locked(fpc_fingerprint_device_t* vdev, worker_state_t state) {
    vdev->state = state;
    vdev->generation++;
    pthread_cond_broadcast(&vdev->cond);
}

/******************************************************************************/

static uint64_t fingerprint_get_auth_id(struct fingerprint_device* device) {
    fpc_fingerprint_device_t* vdev = (fpc_fingerprint_device_t*)device;
    uint64_t authenticator_id;

    pthread_mutex_lock(&vdev->lock);
    authenticator_id = vdev->authenticator_id;
    pthread_mutex_unlock(&vdev->lock);

    return authenticator_id;
}

static int fingerprint_set_active_group(struct fingerprint_device* device, uint32_t gid,
        const char* path) {
    fpc_fingerprint_device_t* vdev = (fpc_fingerprint_device_t*)device;
    char template_path[PATH_MAX];

    if (path == NULL)
        return -EINVAL;
    if (snprintf(template_path, sizeof(template_path), "%s/%s", path, FPC1080_TEMPLATE_NAME) >=
            (int)sizeof(template_path))
        return -ENAMETOOLONG;

    pthread_mutex_lock(&vdev->lock);
    vdev->active_gid = gid;
    strlcpy(vdev->template_path, template_path, sizeof(vdev->template_path));
    pthread_mutex_unlock(&vdev->lock);

    update_authenticator_id(vdev);
    return 0;
}

static int fingerprint_authenticate(struct fingerprint_device* device, uint64_t operation_id,
        __unused uint32_t gid) {
    fpc_fingerprint_device_t* vdev = (fpc_fingerprint_device_t*)device;

    pthread_mutex_lock(&vdev->lock);
    vdev->op_id = operation_id;
    request_state_locked(vdev, STATE_SCAN);
    pthread_mutex_unlock(&vdev->lock);

    return 0;
}

static int fingerprint_enroll(struct fingerprint_device* device, const hw_auth_token_t* hat,
        __unused uint32_t gid, uint32_t timeout_sec) {
    fpc_fingerprint_device_t* vdev = (fpc_fingerprint_device_t*)device;
    char path[PATH_MAX];
    int list[FPC1080_MAX_FINGERS];
    uint32_t used = 0, fid;
    int count;

    if (!hat) {
        ALOGW("%s: null auth token", __func__);
        return -EPROTONOSUPPORT;
    }
    if (hat->version != HW_AUTH_TOKEN_VERSION)
        return -EPROTONOSUPPORT;

    pthread_mutex_lock(&vdev->lock);
    if (hat->challenge != vdev->challenge && !(hat->authenticator_type & HW_AUTH_FINGERPRINT)) {
        pthread_mutex_unlock(&vdev->lock);
        return -EPERM;
    }
    vdev->secure_user_id = hat->user_id;
    pthread_mutex_unlock(&vdev->lock);

    get_template_path(vdev, path, sizeof(path));

    pthread_mutex_lock(&vdev->engine_lock);
    count = list_fingers_locked(vdev, path, list);
    pthread_mutex_unlock(&vdev->engine_lock);

    for (int i = 0; i < count; i++)
        used |= 1u << list[i];
    for (fid = 1; fid <= FPC1080_MAX_FINGERS; fid++) {
        if (!(used & (1u << fid)))
            break;
    }
    if (fid > FPC1080_MAX_FINGERS) {
        send_error_notice(vdev, FINGERPRINT_ERROR_NO_SPACE);
        return 0;
    }

    pthread_mutex_lock(&vdev->lock);
    vdev->enroll_fid = fid;
    vdev->enroll_timeout_sec = timeout_sec;
    request_state_locked(vdev, STATE_ENROLL);
    pthread_mutex_unlock(&vdev->lock);

    return 0;
}

static uint64_t fingerprint_pre_enroll(struct fingerprint_device* device) {
    fpc_fingerprint_device_t* vdev = (fpc_fingerprint_device_t*)device;
    uint64_t challenge = get_64bit_rand();

    pthread_mutex_lock(&vdev->lock);
    vdev->challenge = challenge;
    pthread_mutex_unlock(&vdev->lock);

    return challenge;
}

static int fingerprint_post_enroll(struct fingerprint_device* device) {
    fpc_fingerprint_device_t* vdev = (fpc_fingerprint_device_t*)device;

    pthread_mutex_lock(&vdev->lock);
    vdev->challenge = 0;
    pthread_mutex_unlock(&vdev->lock);

    return 0;
}

static int fingerprint_cancel(struct fingerprint_device* device) {
    fpc_fingerprint_device_t* vdev = (fpc_fingerprint_device_t*)device;
    bool was_idle;

    pthread_mutex_lock(&vdev->lock);
    was_idle = vdev->state == STATE_IDLE;
    if (!was_idle)
        request_state_locked(vdev, STATE_IDLE);
    pthread_mutex_unlock(&vdev->lock);

    /* Otherwise the worker reports the cancellation once the capture is aborted. */
    if (was_idle)
        send_error_notice(vdev, FINGERPRINT_ERROR_CANCELED);

    return 0;
}

static int fingerprint_enumerate(struct fingerprint_device* device) {
    fpc_fingerprint_device_t* vdev = (fpc_fingerprint_device_t*)device;
    char path[PATH_MAX];
    int list[FPC1080_MAX_FINGERS];
    int count;

    get_template_path(vdev, path, sizeof(path));

    pthread_mutex_lock(&vdev->engine_lock);
    count = list_fingers_locked(vdev, path, list);
    pthread_mutex_unlock(&vdev->engine_lock);

    if (count == 0) {
        send_enumerate_notice(vdev, 0, 0);
        return 0;
    }

    for (int i = 0; i < count; i++)
        send_enumerate_notice(vdev, (uint32_t)list[i], (uint32_t)(count - i - 1));

    return 0;
}

static int fingerprint_remove(struct fingerprint_device* device, __unused uint32_t gid,
        uint32_t fid) {
    fpc_fingerprint_device_t* vdev = (fpc_fingerprint_device_t*)device;
    char path[PATH_MAX];
    int list[FPC1080_MAX_FINGERS];
    int count, remaining, ret = 0;

    get_template_path(vdev, path, sizeof(path));

    pthread_mutex_lock(&vdev->engine_lock);
    count = list_fingers_locked(vdev, path, list);
    if (fid != 0) {
        ret = vdev->engine.delete_template((int)fid, path);
        ALOGI("CS_DeleteTemplate: fid=%u ret=%d", fid, ret);
        remaining = ret < 0 ? count : list_fingers_locked(vdev, path, list);
    } else {
        for (int i = 0; i < count; i++)
            vdev->engine.delete_template(list[i], path);
        remaining = 0;
    }
    if (ret >= 0 && remaining == 0)
        remove_template_files(path);
    pthread_mutex_unlock(&vdev->engine_lock);

    if (ret < 0) {
        send_error_notice(vdev, FINGERPRINT_ERROR_UNABLE_TO_REMOVE);
        return 0;
    }

    update_authenticator_id(vdev);

    if (fid != 0) {
        send_remove_notice(vdev, fid, (uint32_t)remaining);
    } else if (count == 0) {
        send_remove_notice(vdev, 0, 0);
    } else {
        for (int i = 0; i < count; i++)
            send_remove_notice(vdev, (uint32_t)list[i], (uint32_t)(count - i - 1));
    }

    return 0;
}

static int set_notify_callback(struct fingerprint_device* device, fingerprint_notify_t notify) {
    fpc_fingerprint_device_t* vdev = (fpc_fingerprint_device_t*)device;

    if (notify == NULL)
        return -EINVAL;

    pthread_mutex_lock(&vdev->lock);
    device->notify = notify;
    pthread_mutex_unlock(&vdev->lock);

    return 0;
}

/******************************************************************************/

static int load_engine(fpc_engine_t* engine) {
    engine->handle = dlopen(FPC1080_ENGINE_LIB, RTLD_NOW | RTLD_LOCAL);
    if (engine->handle == NULL) {
        ALOGE("cannot load %s: %s", FPC1080_ENGINE_LIB, dlerror());
        return -ENODEV;
    }

    engine->enrol = (cs_enrol_t)dlsym(engine->handle, "CS_Enrol");
    engine->verify = (cs_verify_t)dlsym(engine->handle, "CS_Verify");
    engine->set_security_level =
            (cs_set_security_level_t)dlsym(engine->handle, "CS_SetSecurityLevel");
    engine->is_enrolled = (cs_is_enrolled_t)dlsym(engine->handle, "CS_IsEnrolled");
    engine->get_enrolled_finger_list =
            (cs_get_enrolled_finger_list_t)dlsym(engine->handle, "CS_GetEnrolledFingerList");
    engine->delete_template = (cs_delete_template_t)dlsym(engine->handle, "CS_DeleteTemplate");

    if (!engine->enrol || !engine->verify || !engine->set_security_level ||
            !engine->is_enrolled || !engine->get_enrolled_finger_list ||
            !engine->delete_template) {
        ALOGE("missing symbols in %s", FPC1080_ENGINE_LIB);
        dlclose(engine->handle);
        engine->handle = NULL;
        return -ENODEV;
    }

    return 0;
}

static void free_device(fpc_fingerprint_device_t* vdev) {
    if (vdev->engine.handle != NULL)
        dlclose(vdev->engine.handle);
    pthread_cond_destroy(&vdev->cond);
    pthread_mutex_destroy(&vdev->engine_lock);
    pthread_mutex_destroy(&vdev->lock);
    free(vdev->frames);
    free(vdev);
}

static int fingerprint_close(hw_device_t* device) {
    fpc_fingerprint_device_t* vdev = (fpc_fingerprint_device_t*)device;

    if (vdev == NULL)
        return -EINVAL;

    pthread_mutex_lock(&vdev->lock);
    request_state_locked(vdev, STATE_EXIT);
    pthread_mutex_unlock(&vdev->lock);

    pthread_join(vdev->worker, NULL);
    free_device(vdev);

    return 0;
}

static int fingerprint_open(const hw_module_t* module, const char __unused* id,
        hw_device_t** device) {
    fpc_fingerprint_device_t* vdev;
    int ret;

    if (device == NULL)
        return -EINVAL;

    vdev = (fpc_fingerprint_device_t*)calloc(1, sizeof(fpc_fingerprint_device_t));
    if (vdev == NULL)
        return -ENOMEM;

    pthread_mutex_init(&vdev->lock, NULL);
    pthread_mutex_init(&vdev->engine_lock, NULL);
    pthread_cond_init(&vdev->cond, NULL);

    vdev->frames = (uint8_t*)malloc(FPC1080_MAX_FRAMES * FPC1080_FRAME_SIZE);
    if (vdev->frames == NULL) {
        free_device(vdev);
        return -ENOMEM;
    }

    ret = load_engine(&vdev->engine);
    if (ret != 0) {
        free_device(vdev);
        return ret;
    }

    vdev->device.common.tag = HARDWARE_DEVICE_TAG;
    vdev->device.common.version = HARDWARE_MODULE_API_VERSION(2, 1);
    vdev->device.common.module = (struct hw_module_t*)module;
    vdev->device.common.close = fingerprint_close;

    vdev->device.pre_enroll = fingerprint_pre_enroll;
    vdev->device.enroll = fingerprint_enroll;
    vdev->device.post_enroll = fingerprint_post_enroll;
    vdev->device.get_authenticator_id = fingerprint_get_auth_id;
    vdev->device.set_active_group = fingerprint_set_active_group;
    vdev->device.authenticate = fingerprint_authenticate;
    vdev->device.cancel = fingerprint_cancel;
    vdev->device.enumerate = fingerprint_enumerate;
    vdev->device.remove = fingerprint_remove;
    vdev->device.set_notify = set_notify_callback;
    vdev->device.notify = NULL;

    vdev->state = STATE_IDLE;

    if (pthread_create(&vdev->worker, NULL, worker_thread, vdev) != 0) {
        free_device(vdev);
        return -ENOMEM;
    }

    *device = &vdev->device.common;

    return 0;
}

static struct hw_module_methods_t fingerprint_module_methods = {
    .open = fingerprint_open,
};

fingerprint_module_t HAL_MODULE_INFO_SYM = {
    .common = {
        .tag                = HARDWARE_MODULE_TAG,
        .module_api_version = FINGERPRINT_MODULE_API_VERSION_2_1,
        .hal_api_version    = HARDWARE_HAL_API_VERSION,
        .id                 = FINGERPRINT_HARDWARE_MODULE_ID,
        .name               = "Pantech FPC1080 Fingerprint HAL",
        .author             = "ljzyal(ljzyal@gmail.com)",
        .methods            = &fingerprint_module_methods,
    },
};
