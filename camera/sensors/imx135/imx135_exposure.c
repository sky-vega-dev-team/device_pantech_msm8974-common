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
 * Sony IMX135 gain, as the stock Pantech libraries program it: analog gain
 * 256 / (256 - ANA_GAIN_GLOBAL) up to 8x, and above that the digital gain
 * of all four channels, up to 64x in all. The analog gain register is
 * the low byte at 0x0205 only.
 *
 * The stock libraries write the digital gain one step low (32 * gain - 1,
 * read back as (reg + 1) / 256); write it as the Q8 value it is.
 */
#define ANALOG_GAIN_MAX 8.0f
#define TOTAL_GAIN_MAX 64.0f
#define ANALOG_REG_SCALE 256.0
#define ANALOG_REG_MAX 224
#define DIGITAL_GAIN_ONE 0x100

/* DIG_GAIN_GR, DIG_GAIN_R, DIG_GAIN_B, DIG_GAIN_GB */
#define DIGITAL_GAIN_ADDR 0x020e
#define DIGITAL_GAIN_CHANNELS 4

static float register_to_real_gain(uint16_t analog, uint16_t digital)
{
    float gain;

    if (analog > ANALOG_REG_MAX)
        analog = ANALOG_REG_MAX;
    gain = (float)(ANALOG_REG_SCALE / (ANALOG_REG_SCALE - analog));
    if (digital > DIGITAL_GAIN_ONE)
        gain *= (float)digital / DIGITAL_GAIN_ONE;
    return gain;
}

int32_t sensor_calculate_exposure(float real_gain, uint16_t line_count,
        sensor_exposure_info_t *exp_info)
{
    float analog = real_gain;
    uint16_t digital = DIGITAL_GAIN_ONE;

    if (real_gain < 1.0f) {
        analog = 1.0f;
    } else if (real_gain > ANALOG_GAIN_MAX) {
        float total = real_gain > TOTAL_GAIN_MAX ? TOTAL_GAIN_MAX : real_gain;

        digital = (uint16_t)(total / ANALOG_GAIN_MAX * DIGITAL_GAIN_ONE);
        analog = ANALOG_GAIN_MAX;
    }

    exp_info->reg_gain = (uint16_t)(ANALOG_REG_SCALE - ANALOG_REG_SCALE / analog);
    exp_info->digital_gain_reg = digital;
    exp_info->line_count = line_count;
    exp_info->digital_gain = real_gain /
            register_to_real_gain(exp_info->reg_gain, digital);
    return 0;
}

int32_t sensor_fill_exposure_array(uint32_t gain, uint32_t line,
        uint32_t frame_length_lines, struct msm_camera_i2c_reg_setting *setting)
{
    struct msm_sensor_output_reg_addr_t *out = sensor_lib_ptr.output_reg_addr;
    struct msm_sensor_exp_gain_info_t *exp = sensor_lib_ptr.exp_gain_info;
    uint16_t digital = gain >> 16;
    int i;

    if (sensor_exposure_begin(setting, 2 + 2 + 1 + 2 * DIGITAL_GAIN_CHANNELS) < 0)
        return -1;

    sensor_exposure_add_reg16(setting, out->frame_length_lines, frame_length_lines);
    sensor_exposure_add_reg16(setting, exp->coarse_int_time_addr, line);
    sensor_exposure_add_reg(setting, exp->global_gain_addr, gain & 0xff);
    for (i = 0; i < DIGITAL_GAIN_CHANNELS; i++)
        sensor_exposure_add_reg16(setting, DIGITAL_GAIN_ADDR + 2 * i, digital & 0xfff);
    sensor_exposure_end(setting);
    return 0;
}
