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

#include "sensor_exposure.h"

/*
 * Samsung S5K6B2YX gain, as the stock Pantech libraries program it: analog
 * gain only, 32 to the register for 1x, up to 16x. The sensor digital gain
 * is left alone; digital_gain_reg reports 1x.
 */
#define GAIN_MAX 16.0f
#define GAIN_REG_ONE 32
#define GAIN_REG_MAX 0x200
#define DIGITAL_GAIN_ONE 0x100

int32_t sensor_calculate_exposure(float real_gain, uint16_t line_count,
        sensor_exposure_info_t *exp_info)
{
    float gain = real_gain;
    uint16_t reg;

    if (gain < 1.0f)
        gain = 1.0f;
    else if (gain > GAIN_MAX)
        gain = GAIN_MAX;

    reg = (uint16_t)(gain * GAIN_REG_ONE);
    if (reg > GAIN_REG_MAX)
        reg = GAIN_REG_MAX;

    exp_info->reg_gain = reg;
    exp_info->digital_gain_reg = DIGITAL_GAIN_ONE;
    exp_info->line_count = line_count;
    exp_info->digital_gain = real_gain / ((float)reg / GAIN_REG_ONE);
    return 0;
}

int32_t sensor_fill_exposure_array(uint32_t gain, uint32_t line,
        uint32_t frame_length_lines, struct msm_camera_i2c_reg_setting *setting)
{
    struct msm_sensor_output_reg_addr_t *out = sensor_lib_ptr.output_reg_addr;
    struct msm_sensor_exp_gain_info_t *exp = sensor_lib_ptr.exp_gain_info;

    if (sensor_exposure_begin(setting, 2 + 2 + 2) < 0)
        return -1;

    sensor_exposure_add_reg16(setting, out->frame_length_lines, frame_length_lines);
    sensor_exposure_add_reg16(setting, exp->coarse_int_time_addr, line);
    sensor_exposure_add_reg16(setting, exp->global_gain_addr, gain & 0xffff);
    sensor_exposure_end(setting);
    return 0;
}
