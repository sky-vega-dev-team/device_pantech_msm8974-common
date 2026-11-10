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

/*
 * Exposure handling shared by the sensor libraries.
 *
 * The hammerhead sensor module leaves frame by frame exposure to the sensor
 * library: it hands over the sensor subdev fd, the resolution and a frame
 * length factor, asks for a gain and line count to be computed, and calls
 * the library on every start of frame to write them. common/sensor_exposure.c
 * does this the way the stock libmmcamera_imx179.so does: the line count
 * takes effect at once, the gain and the frame length one frame later.
 *
 * A sensor provides the two functions below and the sensor_lib_t named
 * sensor_lib_ptr. Each library is its own shared object, so the state in
 * sensor_exposure.c belongs to one sensor.
 */

#ifndef __SENSOR_EXPOSURE_H__
#define __SENSOR_EXPOSURE_H__

#include "sensor_lib.h"

#define SENSOR_LIB_EXPORT __attribute__((visibility("default")))

/* Room for the registers of one exposure update */
#define SENSOR_EXPOSURE_MAX_REGS 16

extern sensor_lib_t sensor_lib_ptr;
extern sensor_exposure_table_t sensor_exposure_func_table;

/*
 * Turn real_gain into register values. Sets reg_gain to the analog gain,
 * digital_gain_reg to the sensor digital gain, line_count, and digital_gain
 * to the part of real_gain left for the ISP.
 */
int32_t sensor_calculate_exposure(float real_gain, uint16_t line_count,
        sensor_exposure_info_t *exp_info);

/*
 * Fill setting with the registers for one exposure. gain holds reg_gain in
 * its low and digital_gain_reg in its high 16 bits. setting->reg_setting has
 * room for SENSOR_EXPOSURE_MAX_REGS entries.
 */
int32_t sensor_fill_exposure_array(uint32_t gain, uint32_t line,
        uint32_t frame_length_lines, struct msm_camera_i2c_reg_setting *setting);

/* Forget the exposure state; called from <name>_open_lib() */
void sensor_exposure_reset(void);

#endif /* __SENSOR_EXPOSURE_H__ */
