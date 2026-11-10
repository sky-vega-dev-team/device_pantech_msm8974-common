#!/usr/bin/env python3
#
# Copyright (C) 2026 The LineageOS Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""
Turn a stock Pantech sensor library into C data laid out for the hammerhead
sensor module (see include/sensor_lib.h).

The Pantech libraries use the Qualcomm sensor_lib_t of the KitKat camera
stack. This reads it from the ELF file, follows its pointers through the
R_ARM_RELATIVE relocations, and prints the register tables, resolutions,
CSI parameters and chromatix names as C. Values the Pantech layout does not
have come from the JSON file given with --extra.

usage: extract_sensor_lib.py <libmmcamera_<name>.so> <name> <device> --extra <json>

The JSON file holds the values for all devices; an object under "devices"
named after <device> overrides them for that device.
"""

import argparse
import json
import struct
import subprocess
import re
import sys


class Elf:
    def __init__(self, path):
        self.data = open(path, 'rb').read()
        self.segs = []
        for line in subprocess.run(['readelf', '-lW', path], capture_output=True,
                                   text=True, check=True).stdout.splitlines():
            p = line.split()
            if p and p[0] == 'LOAD':
                self.segs.append((int(p[2], 16), int(p[1], 16), int(p[4], 16),
                                  int(p[5], 16)))
        self.rel = set()
        for m in re.finditer(r'^([0-9a-f]{8})\s+\S+\s+R_ARM_RELATIVE',
                             subprocess.run(['readelf', '-rW', path], capture_output=True,
                                            text=True, check=True).stdout, re.M):
            self.rel.add(int(m.group(1), 16))
        self.syms = {}
        for line in subprocess.run(['readelf', '-sW', '--dyn-syms', path], capture_output=True,
                                   text=True, check=True).stdout.splitlines():
            p = line.split()
            if len(p) >= 8 and p[3] == 'FUNC' and p[6] != 'UND':
                self.syms[p[7]] = int(p[1], 16)

    def off(self, va):
        for v, o, fs, ms in self.segs:
            if v <= va < v + fs:
                return va - v + o
        raise ValueError('address %#x is not in the file' % va)

    def read(self, va, n):
        """n bytes at va; .bss and the rest of a segment past its file data read as 0."""
        for v, o, fs, ms in self.segs:
            if v <= va and va + n <= v + ms:
                head = max(0, min(n, v + fs - va))
                return self.data[va - v + o:va - v + o + head] + bytes(n - head)
        raise ValueError('address %#x is not mapped' % va)

    def u8(self, va): return self.read(va, 1)[0]
    def u16(self, va): return struct.unpack('<H', self.read(va, 2))[0]
    def u32(self, va): return struct.unpack('<I', self.read(va, 4))[0]
    def f32(self, va): return struct.unpack('<f', self.read(va, 4))[0]

    def ptr(self, va):
        """Pointer stored at va, or None for NULL."""
        v = self.u32(va)
        if v == 0:
            return None
        if va not in self.rel:
            raise ValueError('%#x does not hold a pointer' % va)
        return v

    def string(self, va):
        o = self.off(va)
        return self.data[o:self.data.index(b'\0', o)].decode()

    def open_lib_result(self, name):
        """Address <name>_open_lib returns: ldr rX, [pc, #imm]; add rX, pc."""
        fn = self.syms['%s_open_lib' % name] & ~1
        ins = self.u16(fn)
        if ins & 0xf800 != 0x4800:
            raise ValueError('unexpected %s_open_lib' % name)
        lit = ((fn + 4) & ~3) + (ins & 0xff) * 4
        add = self.u16(fn + 2)
        if add & 0xff78 != 0x4478:
            raise ValueError('unexpected %s_open_lib' % name)
        return (self.u32(lit) + fn + 2 + 4) & 0xffffffff


# Offsets in the Pantech (Qualcomm KitKat) sensor_lib_t
PT = dict(sensor_output=4, output_reg_addr=8, exp_gain_info=12, aec_info=16,
          snapshot_exp_wait_frames=20, sensor_num_frame_skip=22,
          sensor_num_HDR_frame_skip=24, exposure_table_size=26,
          default_lens_info=28, csi_lane_params=32, csi_cid_params=36,
          sensor_stream_info_array=40, csi_cid_params_size=44,
          init_settings_array=48, start_settings=52, stop_settings=56,
          groupon_settings=60, groupoff_settings=64, sensor_res_cfg_table=68,
          res_settings_array=72, out_info_array=76, csi_params_array=80,
          crop_params_array=84, chromatix_array=88)

ADDR_TYPE = {1: 'MSM_CAMERA_I2C_BYTE_ADDR', 2: 'MSM_CAMERA_I2C_WORD_ADDR',
             3: 'MSM_CAMERA_I2C_3B_ADDR'}
DATA_TYPE = {1: 'MSM_CAMERA_I2C_BYTE_DATA', 2: 'MSM_CAMERA_I2C_WORD_DATA',
             3: 'MSM_CAMERA_I2C_SET_BYTE_MASK', 4: 'MSM_CAMERA_I2C_UNSET_BYTE_MASK',
             5: 'MSM_CAMERA_I2C_SET_WORD_MASK', 6: 'MSM_CAMERA_I2C_UNSET_WORD_MASK',
             7: 'MSM_CAMERA_I2C_SET_BYTE_WRITE_MASK_DATA'}
RES_CFG = ['SENSOR_SET_STOP_STREAM', 'SENSOR_SET_START_STREAM',
           'SENSOR_SET_NEW_RESOLUTION', 'SENSOR_SEND_EVENT', 'SENSOR_SET_CSIPHY_CFG',
           'SENSOR_SET_CSID_CFG', 'SENSOR_LOAD_CHROMATIX']


def flt(v):
    return ('%.6g' % v) + ('' if 'e' in ('%.6g' % v) or '.' in ('%.6g' % v) else '.0') + 'f'


class Gen:
    def __init__(self, elf, name):
        self.e = elf
        self.n = name
        self.out = []

    def emit(self, s=''):
        self.out.append(s)

    def reg_setting(self, va, cname):
        """struct msm_camera_i2c_reg_setting at va, with its register array."""
        e = self.e
        arr, size = e.ptr(va), e.u16(va + 4)
        addr, data, delay = e.u32(va + 8), e.u32(va + 12), e.u16(va + 16)
        if size:
            self.emit('static struct msm_camera_i2c_reg_array %s_array[] = {' % cname)
            for i in range(size):
                a = arr + 8 * i
                self.emit('    { 0x%04x, 0x%04x, %d },' % (e.u16(a), e.u16(a + 2), e.u32(a + 4)))
            self.emit('};')
            self.emit()
        return ('{\n    .reg_setting = %s,\n    .size = %d,\n    .addr_type = %s,\n'
                '    .data_type = %s,\n    .delay = %d,\n}') % (
                    ('%s_array' % cname) if size else 'NULL', size,
                    ADDR_TYPE.get(addr, str(addr)), DATA_TYPE.get(data, str(data)), delay)

    def named_reg_setting(self, field):
        va = self.e.ptr(self.base + PT[field])
        cname = '%s_%s' % (self.n, field)
        body = self.reg_setting(va, cname)
        self.emit('static struct msm_camera_i2c_reg_setting %s = %s;' % (cname, body))
        self.emit()
        return '&' + cname

    def reg_settings_array(self, field):
        e = self.e
        arr_va = e.ptr(self.base + PT[field])
        sets, size = e.ptr(arr_va), e.u32(arr_va + 4)
        cname = '%s_%s' % (self.n, field)
        bodies = [self.reg_setting(sets + 20 * i, '%s_%d' % (cname, i)) for i in range(size)]
        self.emit('static struct msm_camera_i2c_reg_setting %s_settings[] = {' % cname)
        for b in bodies:
            self.emit('    ' + b.replace('\n', '\n    ') + ',')
        self.emit('};')
        self.emit()
        self.emit('static struct sensor_lib_reg_settings_array %s = {' % cname)
        self.emit('    .reg_settings = %s_settings,' % cname)
        self.emit('    .size = %d,' % size)
        self.emit('};')
        self.emit()
        return '&' + cname

    def run(self, extra):
        e, n = self.e, self.n
        self.base = b = e.open_lib_result(n)
        lib = {}

        so = e.ptr(b + PT['sensor_output'])
        self.emit('static sensor_output_t %s_sensor_output = {' % n)
        self.emit('    .output_format = %d,' % e.u32(so))
        self.emit('    .connection_mode = %d,' % e.u32(so + 4))
        self.emit('    .raw_output = %d,' % e.u32(so + 8))
        self.emit('    .filter_arrangement = %d,' % extra['filter_arrangement'])
        self.emit('};')
        self.emit()

        c = extra['test_color_reg']
        self.emit('static struct sensor_test_color_reg_t %s_test_color_reg = {' % n)
        self.emit('    0x%04x, 0x%04x, 0x%04x, 0x%04x,' % tuple(c))
        self.emit('};')
        self.emit()

        o = e.ptr(b + PT['output_reg_addr'])
        self.emit('static struct msm_sensor_output_reg_addr_t %s_output_reg_addr = {' % n)
        self.emit('    0x%04x, 0x%04x, 0x%04x, 0x%04x,' % tuple(e.u16(o + 2 * i) for i in range(4)))
        self.emit('};')
        self.emit()

        g = e.ptr(b + PT['exp_gain_info'])
        self.emit('static struct msm_sensor_exp_gain_info_t %s_exp_gain_info = {' % n)
        self.emit('    .coarse_int_time_addr = 0x%04x,' % e.u16(g))
        self.emit('    .global_gain_addr = 0x%04x,' % extra.get('global_gain_addr', e.u16(g + 2)))
        self.emit('    .vert_offset = %d,' % e.u16(g + 4))
        self.emit('};')
        self.emit()

        a = e.ptr(b + PT['aec_info'])
        self.emit('static sensor_aec_data_t %s_aec_info = {' % n)
        self.emit('    .iso_at_min_gain = %s,' % flt(extra['iso_at_min_gain']))
        self.emit('    .min_gain = %s,' % flt(extra['min_gain']))
        self.emit('    .max_linecount = %d,' % extra.get('max_linecount', e.u32(a + 32)))
        self.emit('    .max_gain = %s,' % flt(e.f32(a + 28)))
        self.emit('};')
        self.emit()

        l = e.ptr(b + PT['default_lens_info'])
        names = ['focal_length', 'pix_size', 'f_number', 'total_f_dist',
                 'hor_view_angle', 'ver_view_angle']
        self.emit('static sensor_lens_info_t %s_default_lens_info = {' % n)
        for i, f in enumerate(names):
            self.emit('    .%s = %s,' % (f, flt(e.f32(l + 4 * i))))
        self.emit('    .min_focus_distance = %s,' % flt(extra['min_focus_distance']))
        self.emit('};')
        self.emit()

        self.emit('static sensor_physical_size_t %s_physical_size = { %s, %s };' % (
            n, flt(extra['physical_size'][0]), flt(extra['physical_size'][1])))
        self.emit()

        cl = e.ptr(b + PT['csi_lane_params'])
        self.emit('static struct csi_lane_params_t %s_csi_lane_params = {' % n)
        self.emit('    .csi_lane_assign = 0x%04x,' % e.u16(cl))
        self.emit('    .csi_lane_mask = 0x%02x,' % e.u8(cl + 2))
        self.emit('    .csi_if = %d,' % e.u8(cl + 3))
        self.emit('    .csid_core = { %d, %d },' % (e.u8(cl + 4), e.u8(cl + 5)))
        self.emit('    .csi_phy_sel = %d,' % e.u8(cl + 6))
        self.emit('};')
        self.emit()

        ncid = e.u16(b + PT['csi_cid_params_size'])
        cid = e.ptr(b + PT['csi_cid_params'])
        cid_addr = {}
        self.emit('static struct msm_camera_csid_vc_cfg %s_cid_cfg[] = {' % n)
        for i in range(ncid):
            va = cid + 3 * i
            cid_addr[va] = i
            self.emit('    { %d, 0x%02x, 0x%02x },' % (e.u8(va), e.u8(va + 1), e.u8(va + 2)))
        self.emit('};')
        self.emit()

        sa = e.ptr(b + PT['sensor_stream_info_array'])
        sinfo, ssize = e.ptr(sa), e.u16(sa + 4)
        for i in range(ssize):
            s = sinfo + 12 * i
            vsize, vc, pf = e.u16(s), e.ptr(s + 4), e.ptr(s + 8)
            self.emit('static struct sensor_pix_fmt_info_t %s_pix_fmt%d[] = {' % (n, i))
            for k in range(vsize):
                self.emit('    { 0x%08x },' % e.u32(pf + 4 * k))
            self.emit('};')
            self.emit()
        self.emit('static sensor_stream_info_t %s_stream_info[] = {' % n)
        for i in range(ssize):
            s = sinfo + 12 * i
            vsize, vc = e.u16(s), e.ptr(s + 4)
            if vc not in cid_addr:
                raise ValueError('stream %d does not point into the CID table' % i)
            self.emit('    { %d, &%s_cid_cfg[%d], %s_pix_fmt%d },' % (vsize, n, cid_addr[vc], n, i))
        self.emit('};')
        self.emit()
        self.emit('static sensor_stream_info_array_t %s_stream_info_array = {' % n)
        self.emit('    .sensor_stream_info = %s_stream_info,' % n)
        self.emit('    .size = %d,' % ssize)
        self.emit('};')
        self.emit()

        for f in ('start_settings', 'stop_settings', 'groupon_settings', 'groupoff_settings'):
            lib[f] = self.named_reg_setting(f)
        lib['init_settings_array'] = self.reg_settings_array('init_settings_array')
        lib['res_settings_array'] = self.reg_settings_array('res_settings_array')

        rc = e.ptr(b + PT['sensor_res_cfg_table'])
        rtab, rsize = e.ptr(rc), e.u16(rc + 4)
        self.emit('static sensor_res_cfg_type_t %s_res_cfg[] = {' % n)
        for i in range(rsize):
            self.emit('    %s,' % RES_CFG[e.u32(rtab + 4 * i)])
        self.emit('};')
        self.emit()
        self.emit('static struct sensor_res_cfg_table_t %s_res_table = {' % n)
        self.emit('    .res_cfg_type = %s_res_cfg,' % n)
        self.emit('    .size = %d,' % rsize)
        self.emit('};')
        self.emit()

        # The Pantech sensor_lib_out_info_t ends with a mode, which the
        # hammerhead one does not have. The newer libraries (ef63, ef65) also
        # have a 32 bit field between vt_pixel_clk and op_pixel_clk; tell the
        # two apart by binning_factor, which is a small number.
        oa = e.ptr(b + PT['out_info_array'])
        oinfo, osize = e.ptr(oa), e.u16(oa + 4)
        extra_clk = 4 if e.u32(oinfo + 16) > 0xffff else 0
        stride = 32 + extra_clk
        self.out_info = []
        self.emit('static struct sensor_lib_out_info_t %s_out_info[] = {' % n)
        for i in range(osize):
            r = oinfo + stride * i
            x, y, llp, fll = (e.u16(r + 2 * k) for k in range(4))
            vt = e.u32(r + 8)
            r += extra_clk
            op = e.u32(r + 12)
            binning, mn, mx = e.u16(r + 16), e.f32(r + 20), e.f32(r + 24)
            self.out_info.append((x, y))
            self.emit('    {')
            self.emit('        /* %dx%d, Pantech mode %d */' % (x, y, e.u32(r + 28)))
            self.emit('        .x_output = %d,' % x)
            self.emit('        .y_output = %d,' % y)
            self.emit('        .line_length_pclk = %d,' % llp)
            self.emit('        .frame_length_lines = %d,' % fll)
            self.emit('        .vt_pixel_clk = %d,' % vt)
            self.emit('        .op_pixel_clk = %d,' % op)
            self.emit('        .binning_factor = %d,' % binning)
            self.emit('        .min_fps = %s,' % flt(mn))
            self.emit('        .max_fps = %s,' % flt(mx))
            self.emit('    },')
        self.emit('};')
        self.emit()
        self.emit('static struct sensor_lib_out_info_array %s_out_info_array = {' % n)
        self.emit('    .out_info = %s_out_info,' % n)
        self.emit('    .size = %d,' % osize)
        self.emit('};')
        self.emit()

        # msm_camera_csi2_params: the hammerhead lut params add vc_cfg_size
        ca = e.ptr(b + PT['csi_params_array'])
        cptrs, csize = e.ptr(ca), e.u16(ca + 4)
        for i in range(csize):
            p = e.ptr(cptrs + 4 * i)
            num_cid = e.u8(p + 8)
            vcs = []
            for k in range(num_cid):
                vc = e.ptr(p + 12 + 4 * k)
                if vc not in cid_addr:
                    raise ValueError('CSI params %d do not point into the CID table' % i)
                vcs.append('&%s_cid_cfg[%d]' % (n, cid_addr[vc]))
            phy = p + 0x4c
            self.emit('static struct msm_camera_csi2_params %s_csi_params_%d = {' % (n, i))
            self.emit('    .csid_params = {')
            self.emit('        .lane_cnt = %d,' % e.u8(p))
            self.emit('        .lane_assign = 0x%04x,' % e.u16(p + 2))
            self.emit('        .phy_sel = %d,' % e.u8(p + 4))
            self.emit('        .lut_params = {')
            self.emit('            .num_cid = %d,' % num_cid)
            self.emit('            .vc_cfg = { %s },' % ', '.join(vcs))
            self.emit('            .vc_cfg_size = sizeof(struct msm_camera_csid_vc_cfg),')
            self.emit('        },')
            self.emit('    },')
            self.emit('    .csiphy_params = {')
            self.emit('        .lane_cnt = %d,' % e.u8(phy))
            self.emit('        .settle_cnt = 0x%02x,' % e.u8(phy + 1))
            self.emit('        .lane_mask = 0x%04x,' % e.u16(phy + 2))
            self.emit('        .combo_mode = %d,' % e.u8(phy + 4))
            self.emit('        .csid_core = %d,' % e.u8(phy + 5))
            self.emit('    },')
            self.emit('};')
            self.emit()
        self.emit('static struct msm_camera_csi2_params *%s_csi_params_list[] = {' % n)
        for i in range(csize):
            self.emit('    &%s_csi_params_%d,' % (n, i))
        self.emit('};')
        self.emit()
        self.emit('static struct sensor_lib_csi_params_array %s_csi_params_array = {' % n)
        self.emit('    .csi2_params = %s_csi_params_list,' % n)
        self.emit('    .size = %d,' % csize)
        self.emit('};')
        self.emit()

        cr = e.ptr(b + PT['crop_params_array'])
        crop, crsize = e.ptr(cr), e.u16(cr + 4)
        self.emit('static struct sensor_crop_parms_t %s_crop_params[] = {' % n)
        for i in range(crsize):
            self.emit('    { %d, %d, %d, %d },' % tuple(e.u16(crop + 8 * i + 2 * k) for k in range(4)))
        self.emit('};')
        self.emit()
        self.emit('static struct sensor_lib_crop_params_array %s_crop_params_array = {' % n)
        self.emit('    .crop_params = %s_crop_params,' % n)
        self.emit('    .size = %d,' % crsize)
        self.emit('};')
        self.emit()

        # The Pantech chromatix entries name 6 (ef60) or 7 libraries, the
        # hammerhead ones 4. Each entry starts with the common chromatix, so
        # the next pointer to the same name marks the size of an entry.
        ch = e.ptr(b + PT['chromatix_array'])
        chs, chsize = e.ptr(ch), e.u16(ch + 4)
        stride = 28
        if chsize > 1:
            first = e.u32(chs)
            stride = next(k for k in range(4, 32, 4) if e.u32(chs + k) == first)
        self.emit('static struct sensor_lib_chromatix_t %s_chromatix[] = {' % n)
        for i in range(max(chsize, osize)):
            names = []
            for k in range(4):
                p = e.ptr(chs + stride * min(i, chsize - 1) + 4 * k)
                names.append('"%s"' % e.string(p) if p else 'NULL')
            self.emit('    {')
            if i >= chsize:
                self.emit('        /* Not in the Pantech library, which has fewer'
                          ' chromatix entries than resolutions */')
            for f, v in zip(['common_chromatix', 'camera_preview_chromatix',
                             'camera_snapshot_chromatix', 'camcorder_chromatix'], names):
                self.emit('        .%s = %s,' % (f, v))
            self.emit('    },')
        self.emit('};')
        self.emit()
        self.emit('static struct sensor_lib_chromatix_array %s_chromatix_array = {' % n)
        self.emit('    .sensor_lib_chromatix = %s_chromatix,' % n)
        self.emit('    .size = %d,' % max(chsize, osize))
        self.emit('};')
        self.emit()

        tp = extra['test_pattern_reg']
        self.emit('static struct msm_camera_i2c_reg_array %s_test_pattern_regs[] = {' % n)
        for v in range(3):
            self.emit('    { 0x%04x, %d, 0 },' % (tp, v))
        self.emit('};')
        self.emit()
        self.emit('/* Off, solid colour and colour bars */')
        self.emit('static struct msm_camera_i2c_reg_setting %s_test_pattern_settings[6] = {' % n)
        for v in range(3):
            self.emit('    { &%s_test_pattern_regs[%d], 1, MSM_CAMERA_I2C_WORD_ADDR, '
                      'MSM_CAMERA_I2C_BYTE_DATA, 0 },' % (n, v))
        self.emit('};')
        self.emit()

        pa = extra['pixel_array']
        self.emit('static sensor_pixel_array_t %s_pixel_array = {' % n)
        self.emit('    .width = %d,' % pa[0])
        self.emit('    .height = %d,' % pa[1])
        self.emit('    .black_level = { %d, %d, %d, %d },' % tuple([extra['black_level']] * 4))
        self.emit('};')
        self.emit()
        aa = extra['active_array']
        self.emit('static sensor_active_array_t %s_active_array = { %d, %d, %d, %d };' % ((n,) + tuple(aa)))
        self.emit()

        self.emit('sensor_lib_t sensor_lib_ptr = {')
        rows = [
            ('sensor_slave_info', 'NULL'),
            ('sensor_output', '&%s_sensor_output' % n),
            ('test_color_reg', '&%s_test_color_reg' % n),
            ('output_reg_addr', '&%s_output_reg_addr' % n),
            ('exp_gain_info', '&%s_exp_gain_info' % n),
            ('aec_info', '&%s_aec_info' % n),
            ('snapshot_exp_wait_frames', str(e.u16(b + PT['snapshot_exp_wait_frames']))),
            ('sensor_num_frame_skip', str(e.u16(b + PT['sensor_num_frame_skip']))),
            ('sensor_num_HDR_frame_skip', str(e.u16(b + PT['sensor_num_HDR_frame_skip']))),
            ('sensor_max_pipeline_frame_delay', str(extra['max_pipeline_frame_delay'])),
            ('exposure_table_size', str(extra['exposure_table_size'])),
            ('default_lens_info', '&%s_default_lens_info' % n),
            ('physical_size', '&%s_physical_size' % n),
            ('csi_lane_params', '&%s_csi_lane_params' % n),
            ('csi_cid_params', '%s_cid_cfg' % n),
            ('sensor_stream_info_array', '&%s_stream_info_array' % n),
            ('csi_cid_params_size', str(ncid)),
            ('test_pattern_settings', '%s_test_pattern_settings' % n),
            ('init_settings_array', lib['init_settings_array']),
            ('start_settings', lib['start_settings']),
            ('stop_settings', lib['stop_settings']),
            ('groupon_settings', lib['groupon_settings']),
            ('groupoff_settings', lib['groupoff_settings']),
            ('sensor_res_cfg_table', '&%s_res_table' % n),
            ('res_settings_array', lib['res_settings_array']),
            ('out_info_array', '&%s_out_info_array' % n),
            ('csi_params_array', '&%s_csi_params_array' % n),
            ('crop_params_array', '&%s_crop_params_array' % n),
            ('chromatix_array', '&%s_chromatix_array' % n),
            ('pixel_array', '&%s_pixel_array' % n),
            ('active_array', '&%s_active_array' % n),
            ('exposure_func_table', '&sensor_exposure_func_table'),
            ('reserved_flags', '{ 0x01, 0x01 }'),
        ]
        for f, v in rows:
            self.emit('    .%s = %s,' % (f, v))
        self.emit('};')
        self.emit()
        self.emit('SENSOR_LIB_EXPORT void *%s_open_lib(void)' % n)
        self.emit('{')
        self.emit('    sensor_exposure_reset();')
        self.emit('    return &sensor_lib_ptr;')
        self.emit('}')
        return self.out


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('lib')
    ap.add_argument('name')
    ap.add_argument('device')
    ap.add_argument('--extra', required=True)
    a = ap.parse_args()
    extra = json.load(open(a.extra))
    extra.update(extra.pop('devices', {}).get(a.device, {}))
    g = Gen(Elf(a.lib), a.name)
    body = g.run(extra)
    print('/*')
    print(' * Generated by tools/extract_sensor_lib.py from the stock libmmcamera_%s.so' % a.name)
    print(' * of the Pantech %s and %s. Do not edit.' % (a.device, a.extra.split('/')[-1]))
    print(' */')
    print()
    print('#include "sensor_exposure.h"')
    print()
    print('\n'.join(body))


if __name__ == '__main__':
    sys.exit(main())
