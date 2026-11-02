#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""把背景/指针/轴心合成成整屏预览，用于上板前目视验收。

图层顺序必须与 LVGL 端一致：背景 → 指针（含高光，已并入 alpha）→ 轴心。

用法:
    python tools/preview.py            # 白天 + 黑夜，各四态
"""
import os
import sys
import numpy as np
from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
BUILD = os.path.join(HERE, '..', 'build')

W = 466
CX = CY = 233
RMAX = 10000
A0, SW = 240, 240


def _pick(cands):
    for path, idx in cands:
        if os.path.exists(path):
            return path, idx
    raise SystemExit('找不到可用字体: %r' % (cands,))


F_TEMP, F_IDX = _pick([
    ('/usr/share/fonts/opentype/noto/NotoSansCJK-Light.ttc', 2),
    ('C:/Windows/Fonts/NotoSansSC-VF.ttf', 0),
    ('C:/Windows/Fonts/msyh.ttc', 0),
    ('C:/Windows/Fonts/simhei.ttf', 0),
])

STOP = [(0.00, (34, 197, 94)), (0.40, (74, 222, 128)), (0.60, (250, 204, 21)),
        (0.78, (251, 146, 60)), (0.86, (239, 68, 68)), (1.00, (220, 38, 38))]

# 水温文字：白天深色字 / 黑夜浅色字，与固件 ui_gauge.c 的 temp_color() 保持一致
TEMP_COL = {
    'day':   {'normal': (43, 46, 53),   'cold': (46, 127, 208), 'hot': (226, 58, 44)},
    'night': {'normal': (231, 234, 239), 'cold': (110, 168, 232), 'hot': (239, 96, 80)},
}


def mix(v):
    t = v / RMAX
    for i in range(len(STOP) - 1):
        a, b = STOP[i], STOP[i + 1]
        if t <= b[0]:
            k = (t - a[0]) / (b[0] - a[0])
            return tuple(int(a[1][j] + (b[1][j] - a[1][j]) * k) for j in range(3))
    return STOP[-1][1]


def temp_color(c, theme):
    pal = TEMP_COL[theme]
    if c >= 105:
        return pal['hot']
    if c < 55:
        return pal['cold']
    return pal['normal']


def tinted(alpha_img, rgb):
    a = np.array(alpha_img, dtype=np.float32) / 255.0
    out = np.zeros((alpha_img.size[1], alpha_img.size[0], 4), dtype=np.uint8)
    out[..., 0], out[..., 1], out[..., 2] = rgb
    out[..., 3] = (a * 255).astype(np.uint8)
    return Image.fromarray(out, 'RGBA')


def compose(bg0, nb, hub, rpm, clt, theme, out_path):
    img = bg0.copy()
    col = mix(rpm)
    ang = A0 + SW * rpm / RMAX
    NS = nb.size[0]
    NC = NS / 2 - 0.5
    HS = hub.size[0]

    layer = tinted(nb, col).rotate(-ang, resample=Image.BICUBIC, center=(NC, NC))
    img.alpha_composite(layer, (int(CX - NS / 2), int(CY - NS / 2)))
    img.alpha_composite(hub, (int(CX - HS / 2), int(CY - HS / 2)))

    d = ImageDraw.Draw(img)
    f = ImageFont.truetype(F_TEMP, 46, index=F_IDX)
    d.text((239, 349), str(clt), font=f, fill=temp_color(clt, theme), anchor='mm')
    img.convert('RGB').save(out_path)
    return col


STATES = [(1450, 41, 'idle'), (5200, 84, 'cruise'), (8800, 89, 'hot'), (9800, 108, 'redline')]


def main():
    nb = Image.open(os.path.join(BUILD, 'needle_body.png')).convert('L')
    hub = Image.open(os.path.join(BUILD, 'hub.png')).convert('RGBA')

    for theme, fname in (('day', 'bg.png'), ('night', 'bg_dark.png')):
        path = os.path.join(BUILD, fname)
        if not os.path.exists(path):
            print('跳过', theme, '：缺', fname)
            continue
        bg0 = Image.open(path).convert('RGBA')
        for rpm, clt, name in STATES:
            out = os.path.join(BUILD, f'preview_{theme}_{name}.png')
            c = compose(bg0, nb, hub, rpm, clt, theme, out)
            print(f'preview_{theme}_{name}.png  rpm={rpm:5d} clt={clt:3d} 针色={c}')


if __name__ == '__main__':
    main()
