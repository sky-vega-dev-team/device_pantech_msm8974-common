/*
 * Copyright (C) 2026 The LineageOS Project
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

#ifndef __AF_TUNE_H__
#define __AF_TUNE_H__

#include <stdint.h>

/* af_tune_parms_t of the hammerhead module, two rows of tunings */
#define AF_TUNE_SIZE 1828
#define AF_TUNE_ROWS 2

/* Per device, from tools/extract_af_tune.py */
extern const uint32_t pantech_af_tune_cam;
extern const uint8_t pantech_af_tune[AF_TUNE_ROWS][AF_TUNE_SIZE];

#endif /* __AF_TUNE_H__ */
