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
 * Put the Pantech actuator tuning into the hammerhead camera sensor module.
 *
 * The module builds in a table of auto focus tunings, two rows of one entry
 * for each actuator camera name, and uses the entry that matches the
 * camera name the kernel reports, the cell-index of the actuator node. Its
 * entries are for Qualcomm reference actuators; the one for the cell-index
 * of the Pantech actuators drives a VCM at another address. Load as a shim
 * of the module and replace that entry with the tuning of the stock Pantech
 * module, before the module runs.
 */

#define LOG_TAG "af_tune_shim"

#include <link.h>
#include <stddef.h>
#include <string.h>

#include <log/log.h>

#include "af_tune.h"

#define MODULE_NAME "libmmcamera2_sensor_modules.so"
#define MODULE "/" MODULE_NAME

/* The table in the M4B30X module, and the camera names of its entries */
#define TABLE_OFFSET 0x15004
#define TABLE_ENTRIES 5
#define TABLE_VERSION 0x301
static const uint32_t table_cams[TABLE_ENTRIES] = { 0, 1, 2, 3, 5 };

struct module_info {
    ElfW(Addr) base;
    ElfW(Addr) data_end;
};

static int find_module(struct dl_phdr_info *info, size_t size, void *arg)
{
    struct module_info *m = arg;
    size_t len = info->dlpi_name ? strlen(info->dlpi_name) : 0;
    int i;

    (void)size;
    if (len < strlen(MODULE) || strcmp(info->dlpi_name + len - strlen(MODULE), MODULE))
        return 0;

    m->base = info->dlpi_addr;
    for (i = 0; i < info->dlpi_phnum; i++) {
        const ElfW(Phdr) *ph = &info->dlpi_phdr[i];

        if (ph->p_type == PT_LOAD && (ph->p_flags & PF_W) &&
                ph->p_vaddr + ph->p_memsz > m->data_end)
            m->data_end = ph->p_vaddr + ph->p_memsz;
    }
    return 1;
}

/* Fields of an entry */
#define TUNE_FAR_END 82 /* the two make up the number of steps */
#define TUNE_STEPS_EXTRA 90
#define TUNE_REGION_SIZE 826
#define TUNE_REGIONS 832
#define TUNE_REGION_STRIDE 6 /* step_bound[2], code_per_step */
#define TUNE_MAX_REGIONS 8

static uint16_t get16(const uint8_t *p, int off)
{
    uint16_t v;

    memcpy(&v, p + off, sizeof(v));
    return v;
}

/*
 * The module keeps a table of the lens position of each step, of as many
 * entries as there are steps, and fills it up to the near bound of the last
 * region. The Pantech tunings end that region on the number of steps, one
 * past the last step, which the Pantech module and the kernel make room for
 * but this module does not. No step goes that far, the kernel moves to
 * steps below the number of steps only, so end the regions on the last step.
 */
static void fit_regions(uint8_t *entry)
{
    uint16_t steps = get16(entry, TUNE_FAR_END) + get16(entry, TUNE_STEPS_EXTRA) + 1;
    uint16_t regions = get16(entry, TUNE_REGION_SIZE);
    uint16_t i, bound;

    if (regions > TUNE_MAX_REGIONS)
        regions = TUNE_MAX_REGIONS;
    for (i = 0; i < regions; i++) {
        uint8_t *near = entry + TUNE_REGIONS + i * TUNE_REGION_STRIDE;

        memcpy(&bound, near, sizeof(bound));
        if (bound >= steps) {
            bound = steps - 1;
            memcpy(near, &bound, sizeof(bound));
        }
    }
}

static uint8_t *table_entry(ElfW(Addr) base, int row, int entry)
{
    return (uint8_t *)(base + TABLE_OFFSET) + (row * TABLE_ENTRIES + entry) * AF_TUNE_SIZE;
}

__attribute__((constructor)) static void af_tune_shim_init(void)
{
    struct module_info m = { 0, 0 };
    uint32_t v[2];
    int row, entry, target = -1;

    if (!dl_iterate_phdr(find_module, &m)) {
        ALOGE("%s not loaded", MODULE_NAME);
        return;
    }
    if (m.data_end < TABLE_OFFSET + AF_TUNE_ROWS * TABLE_ENTRIES * AF_TUNE_SIZE) {
        ALOGE("%s is too small for the tuning table", MODULE_NAME);
        return;
    }

    /* Make sure this is the module the offset was taken from */
    for (row = 0; row < AF_TUNE_ROWS; row++) {
        for (entry = 0; entry < TABLE_ENTRIES; entry++) {
            memcpy(v, table_entry(m.base, row, entry), sizeof(v));
            if (v[0] != TABLE_VERSION || v[1] != table_cams[entry]) {
                ALOGE("unknown %s, not changing its tuning", MODULE_NAME);
                return;
            }
        }
    }
    for (entry = 0; entry < TABLE_ENTRIES; entry++)
        if (table_cams[entry] == pantech_af_tune_cam)
            target = entry;
    if (target < 0) {
        ALOGE("no entry for actuator camera name %u", pantech_af_tune_cam);
        return;
    }

    for (row = 0; row < AF_TUNE_ROWS; row++) {
        memcpy(table_entry(m.base, row, target), pantech_af_tune[row], AF_TUNE_SIZE);
        fit_regions(table_entry(m.base, row, target));
    }
    ALOGI("installed the tuning of actuator camera name %u", pantech_af_tune_cam);
}
