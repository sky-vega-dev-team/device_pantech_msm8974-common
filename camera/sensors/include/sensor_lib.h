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
 * Layout of the sensor libraries the hammerhead (Nexus 5, M4B30X)
 * mm-camera sensor module loads through <name>_open_lib().
 *
 * It follows the Qualcomm sensor_lib_t of that time, with the fields the
 * Nexus 5 stack added for camera HAL3. The offsets were taken from the
 * stock libmmcamera_imx179.so and from the way libmmcamera2_sensor_modules.so
 * reads them, and are checked at the end of this file. Names of fields whose
 * meaning is not settled carry a "reserved" or a descriptive guess.
 */

#ifndef __SENSOR_LIB_H__
#define __SENSOR_LIB_H__

#include <stdint.h>
#include <media/msm_cam_sensor.h>

/* sensor_output_t */
typedef enum {
    SENSOR_BAYER,
    SENSOR_YCBCR,
} sensor_output_format_t;

typedef enum {
    SENSOR_PARALLEL,
    SENSOR_MIPI_CSI,
    SENSOR_MIPI_CSI_1,
    SENSOR_MIPI_CSI_2,
} sensor_connection_mode_t;

typedef enum {
    SENSOR_8_BIT_DIRECT,
    SENSOR_10_BIT_DIRECT,
    SENSOR_12_BIT_DIRECT,
} sensor_raw_output_t;

typedef struct {
    sensor_output_format_t output_format;
    sensor_connection_mode_t connection_mode;
    sensor_raw_output_t raw_output;
    /* Colour filter arrangement reported to HAL3, 3 on the IMX179 */
    uint32_t filter_arrangement;
} sensor_output_t;

/* Solid colour test pattern data registers */
struct sensor_test_color_reg_t {
    uint16_t r_addr;
    uint16_t gr_addr;
    uint16_t b_addr;
    uint16_t gb_addr;
};

struct msm_sensor_output_reg_addr_t {
    uint16_t x_output;
    uint16_t y_output;
    uint16_t line_length_pclk;
    uint16_t frame_length_lines;
};

struct msm_sensor_exp_gain_info_t {
    uint16_t coarse_int_time_addr;
    uint16_t global_gain_addr;
    uint16_t vert_offset;
};

typedef enum {
    SENSOR_MODE_SNAPSHOT,
    SENSOR_MODE_RAW_SNAPSHOT,
    SENSOR_MODE_PREVIEW,
    SENSOR_MODE_VIDEO,
    SENSOR_MODE_VIDEO_HD,
    SENSOR_MODE_HFR_60FPS,
    SENSOR_MODE_HFR_90FPS,
    SENSOR_MODE_HFR_120FPS,
    SENSOR_MODE_HFR_150FPS,
    SENSOR_MODE_ZSL,
    SENSOR_MODE_INVALID,
} sensor_mode_t;

typedef struct {
    sensor_mode_t op_mode;
    uint32_t pixels_per_line;
    uint32_t lines_per_frame;
    uint32_t pclk;
    uint32_t max_fps;
    float digital_gain;
    float stored_digital_gain;
    /* 100.0 and 1.0 on the IMX179: ISO at unity gain and minimum gain */
    float iso_at_min_gain;
    float min_gain;
    uint32_t max_linecount;
    float max_gain;
} sensor_aec_data_t;

typedef struct {
    float focal_length;
    float pix_size;
    float f_number;
    float total_f_dist;
    float hor_view_angle;
    float ver_view_angle;
    /* 0.07 on the IMX179 */
    float min_focus_distance;
} sensor_lens_info_t;

/* Physical size of the pixel array in mm */
typedef struct {
    float width;
    float height;
} sensor_physical_size_t;

/* struct csi_lane_params_t comes from the kernel header */

struct sensor_pix_fmt_info_t {
    uint32_t fourcc;
};

typedef struct {
    uint16_t vc_cfg_size;
    struct msm_camera_csid_vc_cfg *vc_cfg;
    struct sensor_pix_fmt_info_t *pix_fmt_fourcc;
} sensor_stream_info_t;

typedef struct {
    sensor_stream_info_t *sensor_stream_info;
    uint16_t size;
} sensor_stream_info_array_t;

struct sensor_lib_reg_settings_array {
    struct msm_camera_i2c_reg_setting *reg_settings;
    uint32_t size;
};

typedef enum {
    SENSOR_SET_STOP_STREAM,
    SENSOR_SET_START_STREAM,
    SENSOR_SET_NEW_RESOLUTION,
    SENSOR_SEND_EVENT,
    SENSOR_SET_CSIPHY_CFG,
    SENSOR_SET_CSID_CFG,
    SENSOR_LOAD_CHROMATIX,
} sensor_res_cfg_type_t;

struct sensor_res_cfg_table_t {
    sensor_res_cfg_type_t *res_cfg_type;
    uint16_t size;
};

struct sensor_lib_out_info_t {
    uint16_t x_output;
    uint16_t y_output;
    uint16_t line_length_pclk;
    uint16_t frame_length_lines;
    uint32_t vt_pixel_clk;
    uint32_t op_pixel_clk;
    uint16_t binning_factor;
    float min_fps;
    float max_fps;
};

struct sensor_lib_out_info_array {
    struct sensor_lib_out_info_t *out_info;
    uint16_t size;
};

struct sensor_lib_csi_params_array {
    struct msm_camera_csi2_params **csi2_params;
    uint16_t size;
};

struct sensor_crop_parms_t {
    uint16_t top_crop;
    uint16_t bottom_crop;
    uint16_t left_crop;
    uint16_t right_crop;
};

struct sensor_lib_crop_params_array {
    struct sensor_crop_parms_t *crop_params;
    uint16_t size;
};

struct sensor_lib_chromatix_t {
    char *common_chromatix;
    char *camera_preview_chromatix;
    char *camera_snapshot_chromatix;
    char *camcorder_chromatix;
};

struct sensor_lib_chromatix_array {
    struct sensor_lib_chromatix_t *sensor_lib_chromatix;
    uint16_t size;
};

/* Size of the pixel array and its black level */
typedef struct {
    uint32_t width;
    uint32_t height;
    uint16_t black_level[4];
} sensor_pixel_array_t;

/* Part of the pixel array the sensor outputs */
typedef struct {
    uint32_t left;
    uint32_t top;
    uint32_t width;
    uint32_t height;
} sensor_active_array_t;

/* Result of sensor_calculate_exposure */
typedef struct {
    uint16_t reg_gain;
    uint16_t digital_gain_reg;
    uint16_t line_count;
    float digital_gain;
} sensor_exposure_info_t;

/*
 * The functions the sensor module calls. The library applies exposure and
 * frame length to the sensor itself, through the subdev it is given.
 */
typedef struct {
    /* Real gain and line count to register values */
    int32_t (*sensor_calculate_exposure)(float real_gain, uint16_t line_count,
        sensor_exposure_info_t *exp_info);
    /* gain is the analog register gain, the digital one in the top half */
    int32_t (*sensor_fill_exposure_array)(uint32_t gain, uint32_t line,
        uint32_t frame_length_lines, struct msm_camera_i2c_reg_setting *setting);
    /* Called on every start of frame */
    int32_t (*sensor_apply_exposure)(void);
    int32_t (*sensor_apply_initial_exposure)(void);
    int32_t (*sensor_set_resolution)(uint32_t res);
    int32_t (*sensor_set_frame_length_factor)(uint32_t factor);
    int32_t (*sensor_set_subdev_fd)(int32_t fd);
    int32_t (*sensor_get_gains)(uint32_t *gains);
} sensor_exposure_table_t;

typedef struct {
    struct msm_camera_sensor_slave_info *sensor_slave_info;
    sensor_output_t *sensor_output;
    struct sensor_test_color_reg_t *test_color_reg;
    struct msm_sensor_output_reg_addr_t *output_reg_addr;
    struct msm_sensor_exp_gain_info_t *exp_gain_info;
    sensor_aec_data_t *aec_info;
    uint16_t snapshot_exp_wait_frames;
    uint16_t sensor_num_frame_skip;
    uint16_t sensor_num_HDR_frame_skip;
    uint32_t sensor_max_pipeline_frame_delay;
    uint32_t exposure_table_size;
    sensor_lens_info_t *default_lens_info;
    sensor_physical_size_t *physical_size;
    struct csi_lane_params_t *csi_lane_params;
    struct msm_camera_csid_vc_cfg *csi_cid_params;
    sensor_stream_info_array_t *sensor_stream_info_array;
    uint32_t csi_cid_params_size;
    /* Test pattern modes: off, solid colour, colour bars, fade, PN9, custom */
    struct msm_camera_i2c_reg_setting *test_pattern_settings;
    struct sensor_lib_reg_settings_array *init_settings_array;
    struct msm_camera_i2c_reg_setting *start_settings;
    struct msm_camera_i2c_reg_setting *stop_settings;
    struct msm_camera_i2c_reg_setting *groupon_settings;
    struct msm_camera_i2c_reg_setting *groupoff_settings;
    struct sensor_res_cfg_table_t *sensor_res_cfg_table;
    struct sensor_lib_reg_settings_array *res_settings_array;
    struct sensor_lib_out_info_array *out_info_array;
    struct sensor_lib_csi_params_array *csi_params_array;
    struct sensor_lib_crop_params_array *crop_params_array;
    struct sensor_lib_chromatix_array *chromatix_array;
    sensor_pixel_array_t *pixel_array;
    sensor_active_array_t *active_array;
    sensor_exposure_table_t *exposure_func_table;
    uint32_t reserved[3];
    /* 0x0101 on the IMX179; the module reads the first two bytes */
    uint8_t reserved_flags[4];
    /*
     * Four black levels the module uses instead of 64 for each channel;
     * NULL on the IMX179. The module reads it, so the structure must not
     * end before it.
     */
    uint16_t *black_level;
} sensor_lib_t;

#ifdef __cplusplus
#define SENSOR_LIB_ASSERT(c) static_assert(c, #c)
#else
#define SENSOR_LIB_ASSERT(c) _Static_assert(c, #c)
#endif
#include <stddef.h>
SENSOR_LIB_ASSERT(sizeof(struct csi_lane_params_t) == 8);
SENSOR_LIB_ASSERT(sizeof(sensor_output_t) == 16);
SENSOR_LIB_ASSERT(sizeof(sensor_aec_data_t) == 44);
SENSOR_LIB_ASSERT(sizeof(sensor_lens_info_t) == 28);
SENSOR_LIB_ASSERT(sizeof(struct sensor_lib_out_info_t) == 28);
SENSOR_LIB_ASSERT(sizeof(struct sensor_lib_chromatix_t) == 16);
SENSOR_LIB_ASSERT(sizeof(sensor_stream_info_t) == 12);
SENSOR_LIB_ASSERT(sizeof(sensor_pixel_array_t) == 16);
SENSOR_LIB_ASSERT(sizeof(sensor_active_array_t) == 16);
SENSOR_LIB_ASSERT(sizeof(sensor_exposure_table_t) == 32);
SENSOR_LIB_ASSERT(offsetof(sensor_exposure_info_t, line_count) == 4);
SENSOR_LIB_ASSERT(offsetof(sensor_exposure_info_t, digital_gain) == 8);
SENSOR_LIB_ASSERT(offsetof(sensor_lib_t, sensor_output) == 4);
SENSOR_LIB_ASSERT(offsetof(sensor_lib_t, output_reg_addr) == 12);
SENSOR_LIB_ASSERT(offsetof(sensor_lib_t, aec_info) == 20);
SENSOR_LIB_ASSERT(offsetof(sensor_lib_t, snapshot_exp_wait_frames) == 24);
SENSOR_LIB_ASSERT(offsetof(sensor_lib_t, sensor_num_HDR_frame_skip) == 28);
SENSOR_LIB_ASSERT(offsetof(sensor_lib_t, sensor_max_pipeline_frame_delay) == 32);
SENSOR_LIB_ASSERT(offsetof(sensor_lib_t, exposure_table_size) == 36);
SENSOR_LIB_ASSERT(offsetof(sensor_lib_t, default_lens_info) == 40);
SENSOR_LIB_ASSERT(offsetof(sensor_lib_t, csi_lane_params) == 48);
SENSOR_LIB_ASSERT(offsetof(sensor_lib_t, csi_cid_params_size) == 60);
SENSOR_LIB_ASSERT(offsetof(sensor_lib_t, test_pattern_settings) == 64);
SENSOR_LIB_ASSERT(offsetof(sensor_lib_t, init_settings_array) == 68);
SENSOR_LIB_ASSERT(offsetof(sensor_lib_t, start_settings) == 72);
SENSOR_LIB_ASSERT(offsetof(sensor_lib_t, stop_settings) == 76);
SENSOR_LIB_ASSERT(offsetof(sensor_lib_t, sensor_res_cfg_table) == 88);
SENSOR_LIB_ASSERT(offsetof(sensor_lib_t, out_info_array) == 96);
SENSOR_LIB_ASSERT(offsetof(sensor_lib_t, chromatix_array) == 108);
SENSOR_LIB_ASSERT(offsetof(sensor_lib_t, pixel_array) == 112);
SENSOR_LIB_ASSERT(offsetof(sensor_lib_t, active_array) == 116);
SENSOR_LIB_ASSERT(offsetof(sensor_lib_t, exposure_func_table) == 120);
SENSOR_LIB_ASSERT(offsetof(sensor_lib_t, reserved_flags) == 136);
SENSOR_LIB_ASSERT(offsetof(sensor_lib_t, black_level) == 140);
SENSOR_LIB_ASSERT(sizeof(sensor_lib_t) == 144);
#undef SENSOR_LIB_ASSERT

#endif /* __SENSOR_LIB_H__ */
