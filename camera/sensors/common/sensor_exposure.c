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

#define LOG_TAG "sensor_exposure"

#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/system_properties.h>

#include <log/log.h>

#include "sensor_exposure.h"

/* No resolution chosen yet; the stock libraries start from the same value */
#define RES_INVALID 8

/* Read by the HAL, see publish_output_size */
#define SENSOR_OUTPUT_PROP "vendor.camera.sensor.output"

/* Set by the HAL, see apply_ois_mode and follow_full_size */
#define HAL_VIDEO_PROP "vendor.camera.hal.video"
#define HAL_FULL_SIZE_PROP "vendor.camera.hal.full_size"

/* The highest pixel clock one VFE of the MSM8974PRO-AA takes */
#define VFE_MAX_PIXEL_CLK 320000000

/* Frames after the start to set the OIS mode once more */
#define OIS_MODE_RETRY_FRAME 10

/* frame_length_factor is Q10 */
#define FL_FACTOR_SHIFT 10
#define FL_FACTOR_ONE (1 << FL_FACTOR_SHIFT)

static struct {
    int32_t fd;
    uint32_t res;
    uint32_t fl_factor;

    /* Last result of sensor_calculate_exposure */
    uint32_t req_gain;
    uint32_t req_line;

    /* What the sensor was last asked for */
    uint32_t cur_gain;
    uint32_t cur_line;
    uint32_t cur_fl;

    /* Gain and frame length to write on the next frame */
    uint32_t pending_gain;
    uint32_t pending_fl;
    uint8_t pending;

    /* OIS mode last asked of the kernel, and frames since the start */
    int32_t ois_mode;
    uint32_t frames;
} st;

static void full_size_start(void);

void sensor_exposure_reset(void)
{
    memset(&st, 0, sizeof(st));
    st.res = RES_INVALID;
    /* The stock libraries start from 0, which gives a frame length of 0 */
    st.fl_factor = FL_FACTOR_ONE;
}

void sensor_exposure_add_reg(struct msm_camera_i2c_reg_setting *setting, uint16_t addr,
        uint16_t data)
{
    setting->reg_setting[setting->size].reg_addr = addr;
    setting->reg_setting[setting->size].reg_data = data;
    setting->reg_setting[setting->size].delay = 0;
    setting->size++;
}

void sensor_exposure_add_reg16(struct msm_camera_i2c_reg_setting *setting, uint16_t addr,
        uint16_t data)
{
    sensor_exposure_add_reg(setting, addr, data >> 8);
    sensor_exposure_add_reg(setting, addr + 1, data & 0xff);
}

static void add_regs(struct msm_camera_i2c_reg_setting *setting,
        const struct msm_camera_i2c_reg_setting *from)
{
    uint16_t i;

    for (i = 0; i < from->size; i++)
        sensor_exposure_add_reg(setting, from->reg_setting[i].reg_addr,
                from->reg_setting[i].reg_data);
}

int32_t sensor_exposure_begin(struct msm_camera_i2c_reg_setting *setting, uint16_t count)
{
    if (!setting || !setting->reg_setting)
        return -1;
    if (sensor_lib_ptr.groupon_settings->size + count +
            sensor_lib_ptr.groupoff_settings->size > SENSOR_EXPOSURE_MAX_REGS) {
        ALOGE("%s: %u registers do not fit", __func__, count);
        return -1;
    }

    setting->size = 0;
    add_regs(setting, sensor_lib_ptr.groupon_settings);
    return 0;
}

void sensor_exposure_end(struct msm_camera_i2c_reg_setting *setting)
{
    add_regs(setting, sensor_lib_ptr.groupoff_settings);
    setting->addr_type = MSM_CAMERA_I2C_WORD_ADDR;
    setting->data_type = MSM_CAMERA_I2C_BYTE_DATA;
    setting->delay = 0;
}

static int32_t calculate_exposure(float real_gain, uint16_t line_count,
        sensor_exposure_info_t *exp_info)
{
    int32_t rc;

    if (!exp_info)
        return -1;

    rc = sensor_calculate_exposure(real_gain, line_count, exp_info);
    if (rc < 0)
        return rc;

    st.req_gain = exp_info->reg_gain | (uint32_t)exp_info->digital_gain_reg << 16;
    st.req_line = exp_info->line_count;
    return 0;
}

/* Frame length for the current resolution, stretched to fit the line count */
static int32_t frame_length(uint32_t *fl)
{
    struct sensor_lib_out_info_array *out = sensor_lib_ptr.out_info_array;
    uint32_t vert_offset = sensor_lib_ptr.exp_gain_info->vert_offset;

    if (st.fd <= 0) {
        ALOGE("%s: no subdev fd (%d)", __func__, st.fd);
        return -1;
    }
    if (!out || !out->out_info) {
        ALOGE("%s: no output info", __func__);
        return -1;
    }
    if (st.res >= out->size) {
        ALOGE("%s: resolution %u of %u", __func__, st.res, out->size);
        return -1;
    }

    *fl = (out->out_info[st.res].frame_length_lines * st.fl_factor) >> FL_FACTOR_SHIFT;
    if (st.req_line + vert_offset > *fl)
        *fl = st.req_line + vert_offset;
    return 0;
}

static int32_t write_exposure(uint32_t gain, uint32_t fl)
{
    struct msm_camera_i2c_reg_array regs[SENSOR_EXPOSURE_MAX_REGS];
    struct msm_camera_i2c_reg_setting setting;
    struct sensorb_cfg_data cfg;
    int32_t rc;

    memset(&setting, 0, sizeof(setting));
    memset(regs, 0, sizeof(regs));
    setting.reg_setting = regs;

    rc = sensor_fill_exposure_array(gain, st.req_line, fl, &setting);
    if (rc < 0 || !setting.size) {
        ALOGE("%s: no registers to write (%d)", __func__, rc);
        return 0;
    }

    memset(&cfg, 0, sizeof(cfg));
    cfg.cfgtype = CFG_WRITE_I2C_ARRAY;
    cfg.cfg.setting = &setting;
    cfg.setting_size = sizeof(setting);
    if (ioctl(st.fd, VIDIOC_MSM_SENSOR_CFG, &cfg) < 0) {
        ALOGE("%s: VIDIOC_MSM_SENSOR_CFG: %s", __func__, strerror(errno));
        return -EIO;
    }
    return 1;
}

/*
 * The hammerhead camera daemon turns the OIS on in its preview and capture
 * mode for every session and never changes the mode. The HAL knows when a
 * video stream is configured and says so in a property; follow it, so that
 * video gets the movie mode of the OIS. The kernel refuses OIS commands for
 * sensors without one.
 */
static void apply_ois_mode(int force)
{
    char value[PROP_VALUE_MAX] = "";
    struct sensorb_cfg_data cfg;
    enum ois_mode_t mode;

    if (st.fd <= 0)
        return;

    __system_property_get(HAL_VIDEO_PROP, value);
    mode = value[0] == '1' ? OIS_MODE_VIDEO : OIS_MODE_PREVIEW_CAPTURE;
    if (!force && (int32_t)mode == st.ois_mode)
        return;
    st.ois_mode = mode;

    memset(&cfg, 0, sizeof(cfg));
    cfg.cfgtype = CFG_SET_OIS_MODE;
    cfg.cfg.setting = &mode;
    ioctl(st.fd, VIDIOC_MSM_SENSOR_CFG, &cfg);
}

/* Called on every start of frame */
static int32_t apply_exposure(void)
{
    uint32_t fl, gain, next_fl;
    int32_t rc;

    /* The daemon may turn the OIS on after the first frames */
    apply_ois_mode(++st.frames == OIS_MODE_RETRY_FRAME);

    rc = frame_length(&fl);
    if (rc < 0)
        return rc;

    if (st.cur_gain == st.req_gain && st.cur_line == st.req_line &&
            st.cur_fl == fl && !st.pending)
        return 0;

    if (st.pending) {
        /* Write the gain and frame length held back last frame */
        gain = st.pending_gain;
        next_fl = st.pending_fl;
    } else {
        gain = st.cur_gain;
        next_fl = st.cur_fl;
    }

    rc = write_exposure(gain, next_fl);

    /* The line count went out now, the new gain and frame length go next */
    if (st.pending && st.cur_gain == st.req_gain && st.cur_fl == fl) {
        st.pending_gain = 0;
        st.pending_fl = 0;
        st.pending = 0;
    } else {
        st.pending_gain = st.req_gain;
        st.pending_fl = fl;
        st.pending = 1;
    }

    if (rc <= 0)
        return rc;

    st.cur_gain = st.req_gain;
    st.cur_line = st.req_line;
    st.cur_fl = fl;
    return 0;
}

/* Called once when streaming starts: write everything at once */
static int32_t apply_initial_exposure(void)
{
    uint32_t fl;
    int32_t rc;

    rc = frame_length(&fl);
    if (rc < 0)
        return rc;

    st.frames = 0;
    apply_ois_mode(1);

    rc = write_exposure(st.req_gain, fl);
    if (rc == 0)
        return 0;

    /* Write the same values once more on the next frame */
    st.pending_gain = st.req_gain;
    st.pending_fl = fl;
    st.pending = 1;
    if (rc < 0)
        return rc;

    st.cur_gain = st.req_gain;
    st.cur_line = st.req_line;
    st.cur_fl = fl;
    return 0;
}

/*
 * The 3A regions of HAL3 come in active array coordinates, but the stats
 * modules of the hammerhead daemon take them in those of the sensor output,
 * and nothing tells the HAL what the sensor outputs. Leave it in a property
 * for the HAL: "<active width>x<active height>:<output width>x<output height>".
 */
static void publish_output_size(uint32_t res)
{
    struct sensor_lib_out_info_t *info = &sensor_lib_ptr.out_info_array->out_info[res];
    sensor_active_array_t *active = sensor_lib_ptr.active_array;
    char value[PROP_VALUE_MAX];

    if (!active)
        return;
    snprintf(value, sizeof(value), "%ux%u:%ux%u", active->width, active->height,
            info->x_output, info->y_output);
    if (__system_property_set(SENSOR_OUTPUT_PROP, value) < 0)
        ALOGE("%s: could not set %s to %s", __func__, SENSOR_OUTPUT_PROP, value);
}

static int32_t set_resolution(uint32_t res)
{
    struct sensor_lib_out_info_array *out = sensor_lib_ptr.out_info_array;

    if (!out) {
        ALOGE("%s: no output info", __func__);
        return -1;
    }
    if (res >= out->size) {
        ALOGE("%s: resolution %u of %u", __func__, res, out->size);
        return -1;
    }
    st.res = res;
    publish_output_size(res);
    return 0;
}

static int32_t set_frame_length_factor(uint32_t factor)
{
    st.fl_factor = factor;
    return 0;
}

static int32_t set_subdev_fd(int32_t fd)
{
    if (fd <= 0) {
        ALOGE("%s: invalid fd %d", __func__, fd);
        return -1;
    }
    st.fd = fd;
    full_size_start();
    return 0;
}

static int32_t get_gains(uint32_t *gains)
{
    if (!gains) {
        ALOGE("%s: no buffer", __func__);
        return -1;
    }
    gains[0] = st.req_gain & 0xffff;
    gains[1] = st.req_gain >> 16;
    return 0;
}

/*
 * The hammerhead sensor module takes the first resolution that fits a
 * stream, and the first one of all for a full size snapshot, so the
 * generator puts the full size mode that runs fastest first. When it needs
 * both VFEs, it would then also be taken for preview and video that a binned
 * mode serves at the same rate on one VFE, at a lower rate in the end, as the
 * CPP cannot keep up with full size frames for them. The HAL says in a
 * property whether a session has a full size stream; while it has none, make
 * the first mode look slower than the others, so that the module passes it
 * over, until the next session.
 */
#define FULL_SIZE_STANDBY_FPS 15.0f

static float full_size_fps;

static int full_size_needs_care(void)
{
    struct sensor_lib_out_info_array *out = sensor_lib_ptr.out_info_array;
    uint16_t i;

    if (!out || out->size < 2)
        return 0;
    for (i = 1; i < out->size; i++) {
        if (out->out_info[i].x_output > out->out_info[0].x_output ||
                out->out_info[i].y_output > out->out_info[0].y_output)
            return 0;
    }
    if (out->out_info[0].op_pixel_clk <= VFE_MAX_PIXEL_CLK)
        return 0;
    for (i = 1; i < out->size; i++)
        if (out->out_info[i].op_pixel_clk <= VFE_MAX_PIXEL_CLK &&
                out->out_info[i].max_fps >= out->out_info[0].max_fps)
            return 1;
    return 0;
}

static void follow_full_size(void)
{
    char value[PROP_VALUE_MAX] = "";
    struct sensor_lib_out_info_t *first = &sensor_lib_ptr.out_info_array->out_info[0];

    __system_property_get(HAL_FULL_SIZE_PROP, value);
    /* Until the HAL says otherwise, as when the daemon reads the modes */
    first->max_fps = value[0] == '0' ? FULL_SIZE_STANDBY_FPS : full_size_fps;
}

static void *full_size_thread(void *arg)
{
    uint32_t serial = 0;

    (void)arg;
    for (;;) {
        follow_full_size();
        /* Any property change; the first wait returns at once */
        __system_property_wait(NULL, serial, &serial, NULL);
    }
    return NULL;
}

/*
 * Started once the sensor is used. The daemon loads the library of every
 * sensor it probes and unloads those that do not match, so keep this one
 * loaded for the thread.
 */
static void full_size_start(void)
{
    static int started;
    pthread_t thread;
    Dl_info info;

    if (started)
        return;
    started = 1;
    if (!full_size_needs_care())
        return;
    if (!dladdr((void *)full_size_start, &info) || !info.dli_fname ||
            !dlopen(info.dli_fname, RTLD_NOW | RTLD_NODELETE)) {
        ALOGE("%s: could not keep the library loaded", __func__);
        return;
    }
    full_size_fps = sensor_lib_ptr.out_info_array->out_info[0].max_fps;
    follow_full_size();
    if (pthread_create(&thread, NULL, full_size_thread, NULL))
        ALOGE("%s: no thread to follow %s", __func__, HAL_FULL_SIZE_PROP);
    else
        pthread_detach(thread);
}

sensor_exposure_table_t sensor_exposure_func_table = {
    .sensor_calculate_exposure = calculate_exposure,
    .sensor_fill_exposure_array = sensor_fill_exposure_array,
    .sensor_apply_exposure = apply_exposure,
    .sensor_apply_initial_exposure = apply_initial_exposure,
    .sensor_set_resolution = set_resolution,
    .sensor_set_frame_length_factor = set_frame_length_factor,
    .sensor_set_subdev_fd = set_subdev_fd,
    .sensor_get_gains = get_gains,
};
