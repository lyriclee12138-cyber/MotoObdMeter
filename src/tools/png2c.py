#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""位图 → LVGL 图像描述符（C 数组）

输出的 .c 文件按 LVGL 9 的 lv_image_dsc_t 布局生成，可直接 lv_image_set_src()。

  v2/bg_day_static.png    RGB565   白天静态层（底弧 + 刻度 + 刻度数字）
  v2/bg_night_static.png  RGB565   黑夜静态层

  由 tools/gauge_art_v2.py 渲染。进度弧 / 转速数字 / 水温由 LVGL 运行时画。
"""
import os
import sys
from functools import partial

import numpy as np
from PIL import Image

BUILD = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'build')
OUTDIR = os.path.join(BUILD, 'assets')
os.makedirs(OUTDIR, exist_ok=True)

HEADER = '''//
// 自动生成，请勿手改。
// 源：tools/gauge_art.py 渲染 build 下的 png，再由 tools/png2c.py 转成本文件
//
#include "lvgl.h"
#include "gauge_assets.h"

'''

EPILOG = '''
const gauge_asset_t gauge_assets[] = {
'''


def emit_array(f, name, values, per_line=16):
    f.write(f'static const uint8_t {name}[] = {{\n')
    for i in range(0, len(values), per_line):
        chunk = values[i:i + per_line]
        f.write('    ' + ','.join(f'0x{v:02X}' for v in chunk) + ',\n')
    f.write('};\n\n')


def rgb565_bytes(img, bg=(255, 255, 255)):
    """RGB565 没有 alpha 通道，透明像素必须合成到一个底色上。

    bg 要按图来定：背景图的透明区是圆盘外的四个角，正常显示时被圆屏裁掉看不见，
    但左右滑动时会转进可视区 —— 合成到白色的话，黑夜模式下会看到四个刺眼的白角。
    所以背景图必须合成到各自主题的底色（见 ITEMS）。"""
    a = np.array(img.convert('RGBA'), dtype=np.float32)
    alpha = a[..., 3:4] / 255.0
    base = np.array(bg, dtype=np.float32).reshape(1, 1, 3)
    rgb = a[..., :3] * alpha + base * (1 - alpha)
    r = (rgb[..., 0] * 31 / 255).round().astype(np.uint16)
    g = (rgb[..., 1] * 63 / 255).round().astype(np.uint16)
    b = (rgb[..., 2] * 31 / 255).round().astype(np.uint16)
    v = ((r << 11) | (g << 5) | b).astype('<u2')     # LVGL 期望小端
    return v.tobytes()


def a8_bytes(img):
    return np.array(img.convert('L'), dtype=np.uint8).tobytes()


def argb8888_bytes(img):
    a = np.array(img.convert('RGBA'), dtype=np.uint8)
    out = np.empty((a.shape[0], a.shape[1], 4), dtype=np.uint8)
    # lv_color32_t 的字段顺序是 blue, green, red, alpha（小端 uint32 即 0xAARRGGBB）
    out[..., 0] = a[..., 2]     # B
    out[..., 1] = a[..., 1]     # G
    out[..., 2] = a[..., 0]     # R
    out[..., 3] = a[..., 3]     # A
    return out.tobytes()


# 背景图的透明区（圆盘外的四角）合成到各自主题的底色，和 ui_theme.c 里的 p->bg 保持一致。
# 白天 #F2F4F7、黑夜 #0E1013；改主题配色时这两个值要跟着改。
ITEMS = [
    ('bg_day',   'v2/bg_day_static.png',   'RGB565', rgb565_bytes, 2),
    ('bg_night', 'v2/bg_night_static.png', 'RGB565', rgb565_bytes, 2),
]

CF_MAP = {'RGB565': 'LV_COLOR_FORMAT_RGB565', 'L8': 'LV_COLOR_FORMAT_L8',
          'A8': 'LV_COLOR_FORMAT_A8', 'ARGB8888': 'LV_COLOR_FORMAT_ARGB8888'}


def main():
    c_path = os.path.join(OUTDIR, 'gauge_assets.c')
    h_path = os.path.join(OUTDIR, 'gauge_assets.h')
    with open(c_path, 'w', encoding='utf-8') as f:
        f.write(HEADER)
        meta = []
        for name, png, cf, conv, bpp in ITEMS:
            img = Image.open(os.path.join(BUILD, png))
            w, h = img.size
            data = conv(img)
            emit_array(f, f'{name}_map', data)
            stride = w * bpp
            f.write(f'''const lv_image_dsc_t {name}_img = {{
    .header.magic = LV_IMAGE_HEADER_MAGIC,
    .header.cf = {CF_MAP[cf]},
    .header.flags = 0,
    .header.w = {w},
    .header.h = {h},
    .header.stride = {stride},
    .header.reserved_2 = 0,
    .data_size = {len(data)},
    .data = {name}_map,
}};

''')
            meta.append((name, w, h, cf, len(data)))
            print(f'{png:18s} {w}x{h} {cf:9s} {len(data)/1024:8.1f} KB')

        f.write(EPILOG)
        for name, w, h, cf, size in meta:
            f.write(f'    {{ "{name}", &{name}_img }},\n')
        f.write('};\n')
        f.write(f'const size_t gauge_asset_count = {len(meta)};\n')

    with open(h_path, 'w', encoding='utf-8') as f:
        f.write('''#pragma once
#include "lvgl.h"

typedef struct {
    const char *name;
    const lv_image_dsc_t *img;
} gauge_asset_t;

extern const gauge_asset_t gauge_assets[];
extern const size_t gauge_asset_count;

''')
        for name, *_ in ITEMS:
            f.write(f'extern const lv_image_dsc_t {name}_img;\n')

    print('\n输出:', c_path)
    print('输出:', h_path)


if __name__ == '__main__':
    main()
