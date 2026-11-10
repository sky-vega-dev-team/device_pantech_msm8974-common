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
 * Calibration data of the Pantech camera modules (ROHM and Rumba SA OTP),
 * for the hammerhead sensor module.
 *
 * The stock Pantech libraries use the KitKat layout of the EEPROM data,
 * which the hammerhead module cannot read. All they take from the OTP is
 * the auto focus range and one set of white balance ratios, at the same
 * places for both modules; lens shading is not used.
 *
 * EEPROM_NAME is the qcom,eeprom-name of the kernel node.
 */

#define LOG_TAG "pantech_eeprom"

#include <dlfcn.h>
#include <stddef.h>
#include <stdint.h>

#include <log/log.h>

/*
 * Offsets in the sensor_eeprom_data_t of the hammerhead module, taken from
 * the stock libmmcamera_imx179_eeprom.so and the module's own calibration
 * code.
 */
#define DATA_BUFFER 0x28            /* uint8_t *, the OTP contents */
#define DATA_ITEMS 0x44             /* uint32_t af, wb, lsc, dpc */
#define DATA_WB_R_OVER_G 0x54       /* float[WB_LIGHTS] */
#define DATA_WB_B_OVER_G 0x84       /* float[WB_LIGHTS] */
#define DATA_WB_GR_OVER_GB 0xb4     /* float */
#define DATA_AF_MACRO 0xb8          /* uint16_t */
#define DATA_AF_INFINITY 0xba       /* uint16_t */

#define WB_LIGHTS 12

enum { ITEM_AF, ITEM_WB, ITEM_LSC, ITEM_DPC, ITEM_COUNT };

/* OTP layout, as the stock libraries read it */
#define OTP_WB_R_OVER_G 0x000       /* little endian, Q10 */
#define OTP_WB_B_OVER_G 0x002
#define OTP_WB_GR_OVER_GB 0x004
#define OTP_AF_INFINITY 0x37a       /* big endian */
#define OTP_AF_MACRO 0x37c
#define WB_SCALE (1.0f / 1024)

#define FIELD(data, off, type) ((type *)((char *)(data) + (off)))

#define _STR(x) #x
#define STR(x) _STR(x)
#define _CAT(a, b) a##b
#define CAT(a, b) _CAT(a, b)

typedef void (*calibration_func)(void *);

/* eeprom_lib_func_t */
static struct {
    calibration_func get_calibration_items;
    calibration_func format_calibration_data;
    calibration_func reserved;
    calibration_func do_af_calibration;
    calibration_func do_wbc_calibration;
    calibration_func do_lsc_calibration;
    void *get_dpc_calibration_info;
    void *get_raw_data;
} func_table;

static uint16_t le16(const uint8_t *p)
{
    return p[0] | p[1] << 8;
}

static uint16_t be16(const uint8_t *p)
{
    return p[0] << 8 | p[1];
}

static int valid(uint16_t v)
{
    return v != 0 && v != 0xffff;
}

static void get_calibration_items(void *data)
{
    const uint8_t *otp = *FIELD(data, DATA_BUFFER, uint8_t *);
    uint32_t *items = FIELD(data, DATA_ITEMS, uint32_t);
    uint16_t macro = be16(otp + OTP_AF_MACRO);
    uint16_t infinity = be16(otp + OTP_AF_INFINITY);

    /*
     * The module scales the actuator tuning to the OTP range assuming a
     * single region, which turns the four regions of the Pantech tunings
     * into steps that run past the DAC range. The Pantech tunings cover
     * the range of the modules as they are; leave the AF calibration out.
     */
    items[ITEM_AF] = 0;
    items[ITEM_WB] = valid(le16(otp + OTP_WB_R_OVER_G)) &&
            valid(le16(otp + OTP_WB_B_OVER_G)) &&
            valid(le16(otp + OTP_WB_GR_OVER_GB));
    items[ITEM_LSC] = 0;
    items[ITEM_DPC] = 0;

    /* The stock libraries do not check the OTP; leave out what is unset */
    if (!items[ITEM_WB])
        ALOGW("%s: OTP without WB", STR(EEPROM_NAME));
    if (!valid(macro) || !valid(infinity) || macro == infinity)
        ALOGW("%s: OTP without AF", STR(EEPROM_NAME));
}

static void format_calibration_data(void *data)
{
    const uint8_t *otp = *FIELD(data, DATA_BUFFER, uint8_t *);
    float r_over_g = le16(otp + OTP_WB_R_OVER_G) * WB_SCALE;
    float b_over_g = le16(otp + OTP_WB_B_OVER_G) * WB_SCALE;
    int i;

    /* The OTP has one set of ratios; use it for every light */
    for (i = 0; i < WB_LIGHTS; i++) {
        FIELD(data, DATA_WB_R_OVER_G, float)[i] = r_over_g;
        FIELD(data, DATA_WB_B_OVER_G, float)[i] = b_over_g;
    }
    *FIELD(data, DATA_WB_GR_OVER_GB, float) = le16(otp + OTP_WB_GR_OVER_GB) * WB_SCALE;
    *FIELD(data, DATA_AF_MACRO, uint16_t) = be16(otp + OTP_AF_MACRO);
    *FIELD(data, DATA_AF_INFINITY, uint16_t) = be16(otp + OTP_AF_INFINITY);

    ALOGI("%s: AF macro %u infinity %u, WB r/g %f b/g %f gr/gb %f", STR(EEPROM_NAME),
            *FIELD(data, DATA_AF_MACRO, uint16_t), *FIELD(data, DATA_AF_INFINITY, uint16_t),
            r_over_g, b_over_g, *FIELD(data, DATA_WB_GR_OVER_GB, float));
}

/*
 * The calibration itself is done by the sensor module, which has loaded
 * this library; take its functions from there.
 */
__attribute__((visibility("default"))) void *CAT(EEPROM_NAME, _open_lib)(void)
{
    void *module;

    if (func_table.format_calibration_data)
        return &func_table;

    module = dlopen("libmmcamera2_sensor_modules.so", RTLD_NOW | RTLD_NOLOAD);
    if (!module) {
        ALOGE("%s: sensor module not loaded: %s", STR(EEPROM_NAME), dlerror());
        return NULL;
    }
    func_table.do_af_calibration =
            (calibration_func)dlsym(module, "eeprom_autofocus_calibration");
    func_table.do_wbc_calibration =
            (calibration_func)dlsym(module, "eeprom_whitebalance_calibration");
    func_table.do_lsc_calibration =
            (calibration_func)dlsym(module, "eeprom_lensshading_calibration");
    dlclose(module);
    if (!func_table.do_af_calibration || !func_table.do_wbc_calibration ||
            !func_table.do_lsc_calibration) {
        ALOGE("%s: sensor module without calibration functions", STR(EEPROM_NAME));
        return NULL;
    }

    func_table.get_calibration_items = get_calibration_items;
    func_table.format_calibration_data = format_calibration_data;
    return &func_table;
}
