/*
 * Copyright (C) 2014 The Android Open Source Project
 * Copyright (C) 2016 The CyanogenMod Project
 * Copyright (C) 2016 The Mokee Project
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

#ifndef FPC1080_HAL_H
#define FPC1080_HAL_H

#include <limits.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>

#include <hardware/fingerprint.h>
#include <hardware/hardware.h>

#define FPC1080_DEVICE "/dev/fpc1080"
#define FPC1080_ENGINE_LIB "/vendor/lib/libfpc1080_jni.so"
#define FPC1080_TEMPLATE_NAME "csfp.tpl"

#define FPC1080_IOCTL_START_CAPTURE 0xFC00
#define FPC1080_IOCTL_ABORT_CAPTURE 0xFC01
#define FPC1080_IOCTL_START_NAVIGATION 0x4004FC04
#define FPC1080_NAVIGATION_MODE_SWIPE 4

#define FPC1080_FRAME_SIZE 1024
#define FPC1080_MAX_FRAMES 500
#define FPC1080_POLL_INTERVAL_MS 100

#define FPC1080_MAX_FINGERS 5
#define FPC1080_ENROLL_SAMPLES 3
#define FPC1080_SECURITY_LEVEL 2

/* Return values of the Crucialtec engine */
#define CS_ERROR_NO_MATCH (-60006)
#define CS_ERROR_IMAGE_TOO_SHORT (-60011)

typedef int (*cs_enrol_t)(uint8_t *frames, int frame_count, int finger, int trial,
                          const char *path);
typedef int (*cs_verify_t)(uint8_t *frames, int frame_count, const char *path);
typedef void (*cs_set_security_level_t)(int level);
typedef int (*cs_is_enrolled_t)(int finger, const char *path);
typedef int (*cs_get_enrolled_finger_list_t)(const char *path, int *list, int size);
typedef int (*cs_delete_template_t)(int finger, const char *path);

typedef struct fpc_engine_t {
    void *handle;
    cs_enrol_t enrol;
    cs_verify_t verify;
    cs_set_security_level_t set_security_level;
    cs_is_enrolled_t is_enrolled;
    cs_get_enrolled_finger_list_t get_enrolled_finger_list;
    cs_delete_template_t delete_template;
} fpc_engine_t;

typedef enum worker_state_t {
    STATE_IDLE = 0,
    STATE_ENROLL,
    STATE_SCAN,
    STATE_EXIT
} worker_state_t;

typedef struct fpc_fingerprint_device_t {
    fingerprint_device_t device;  // "inheritance"

    pthread_t worker;
    pthread_cond_t cond;
    pthread_mutex_t lock;          // state, generation and identifiers below
    pthread_mutex_t engine_lock;   // serializes all calls into the engine
    worker_state_t state;
    uint32_t generation;           // bumped whenever the requested operation changes

    uint64_t op_id;
    uint64_t challenge;
    uint64_t secure_user_id;
    uint64_t authenticator_id;
    uint32_t active_gid;
    uint32_t enroll_fid;
    uint32_t enroll_timeout_sec;
    char template_path[PATH_MAX];

    fpc_engine_t engine;
    uint8_t *frames;
} fpc_fingerprint_device_t;

#endif  // FPC1080_HAL_H
