#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""仪表盘 v2 · 极简风格素材渲染

设计语言（参考两张用户选的图）：
  · 中心超大数字是主角，其余全是辅助
  · 外圈一条 270° 的进度弧，暗色底轨 + 彩色进度
  · 刻度稀疏克制，不用金属质感、不用内阴影
  · 白天/黑夜两套配色，靠底色和文字色区分，强调色不变

静态部分（本脚本烘焙）：
  背景色、底弧、刻度、刻度数字
动态部分（固件里用 LVGL 画）：
  进度弧、中心转速数字、水温数字

坐标系与固件严格一致：
  角度以 12 点为 0°、顺时针为正，0 rpm → -135°，10000 rpm → +135°
  x = CX + r*sin(θ)，y = CY - r*cos(θ)
"""
import os
from PIL import Image, ImageDraw, ImageFont

W = 466
CX = CY = W / 2.0
SS = 4                       # 超采样倍数

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'build', 'v2')
os.makedirs(OUT, exist_ok=True)

# ---------- 几何 ----------
ANG_START = -135.0          # 0 rpm 的角度（左下）
ANG_SPAN = 270.0            # 扫过角度，总缺口 90° 在正下方
RPM_MAX = 10000

R_ARC = 194.0               # 进度弧中心半径
ARC_W = 11.0                # 进度弧宽度
R_TICK_OUT = 219.0          # 刻度外端
R_TICK_IN_MINOR = 207.0     # 小刻度内端
R_TICK_IN_MAJOR = 203.0     # 主刻度内端（更长）
R_LABEL = 165.0             # 刻度数字的半径（往里收，别贴着弧）

# ---------- 配色 ----------
DAY = {
    'bg':          (245, 246, 248),
    'track':       (214, 219, 227),
    'tick_minor':  (182, 189, 200),
    'tick_major':  (124, 131, 142),
    'label':       (128, 134, 144),
    'text':        (24, 26, 30),
    'dim':         (146, 152, 162),
    'cold':        (56, 138, 255),
    'hot':         (238, 76, 56),
}

NIGHT = {
    'bg':          (8, 9, 11),
    'track':       (28, 31, 36),
    'tick_minor':  (44, 48, 54),
    'tick_major':  (76, 81, 89),
    'label':       (108, 115, 128),
    'text':        (255, 255, 255),
    'dim':         (108, 115, 128),
    'cold':        (74, 158, 255),
    'hot':         (240, 80, 60),
}

# 进度弧的三段强调色（低 / 中 / 高），固件里要做同样的插值
ARC_LO = (46, 229, 157)     # 绿 #2EE59D
ARC_MID = (245, 197, 66)    # 黄 #F5C542
ARC_HI = (240, 80, 60)      # 红 #F0503C

FONT_LAT = 'C:/Windows/Fonts/arial.ttf'
FONT_BOLD = 'C:/Windows/Fonts/arialbd.ttf'


def font(size, bold=False):
    path = FONT_BOLD if bold else FONT_LAT
    return ImageFont.truetype(path, int(size * SS))


def polar(r, deg):
    import math
    a = math.radians(deg)
    return (CX + r * math.sin(a), CY - r * math.cos(a))


def arc_color(frac):
    """转速比例 → 弧的颜色，绿→黄→红 两段线性插值。"""
    if frac <= 0.5:
        t = frac / 0.5
        a, b = ARC_LO, ARC_MID
    else:
        t = (frac - 0.5) / 0.5
        a, b = ARC_MID, ARC_HI
    return tuple(int(a[i] + (b[i] - a[i]) * t) for i in range(3))


# ---------- 静态背景 ----------
def render_bg(pal, path):
    img = Image.new('RGB', (W * SS, W * SS), pal['bg'])
    d = ImageDraw.Draw(img)

    # 底弧：PIL 的 0° 在 3 点方向，我们的 0° 在 12 点，所以整体减 90
    bbox = [(CX - R_ARC) * SS, (CY - R_ARC) * SS,
            (CX + R_ARC) * SS, (CY + R_ARC) * SS]
    p0 = ANG_START - 90.0
    p1 = ANG_START + ANG_SPAN - 90.0
    d.arc(bbox, p0, p1, fill=pal['track'], width=int(ARC_W * SS))

    # 刻度
    for v in range(0, RPM_MAX + 1, 500):
        deg = ANG_START + ANG_SPAN * v / RPM_MAX
        major = (v % 1000 == 0)
        r_in = R_TICK_IN_MAJOR if major else R_TICK_IN_MINOR
        x0, y0 = polar(r_in, deg)
        x1, y1 = polar(R_TICK_OUT, deg)
        w = 2.6 if major else 1.4
        d.line([x0 * SS, y0 * SS, x1 * SS, y1 * SS],
               fill=pal['tick_major'] if major else pal['tick_minor'],
               width=max(1, int(w * SS)))

    # 刻度数字 0..10
    f = font(27)
    for v in range(0, RPM_MAX + 1, 1000):
        deg = ANG_START + ANG_SPAN * v / RPM_MAX
        x, y = polar(R_LABEL, deg)
        d.text((x * SS, y * SS), str(v // 1000), font=f,
               fill=pal['label'], anchor='mm')

    img = img.resize((W, W), Image.LANCZOS)
    img.save(path)
    print('saved', path)


# ---------- 预览合成（含动态元素） ----------
def render_preview(pal, rpm, coolant, path, bg_path):
    img = Image.open(bg_path).convert('RGB').resize((W * SS, W * SS), Image.LANCZOS)
    d = ImageDraw.Draw(img)

    frac = max(0.0, min(1.0, rpm / RPM_MAX))
    color = arc_color(frac)

    # 进度弧
    if frac > 0.001:
        bbox = [(CX - R_ARC) * SS, (CY - R_ARC) * SS,
                (CX + R_ARC) * SS, (CY + R_ARC) * SS]
        p0 = ANG_START - 90.0
        p1 = ANG_START + ANG_SPAN * frac - 90.0
        d.arc(bbox, p0, p1, fill=color, width=int(ARC_W * SS))

        # 末端圆头，让弧线看起来像有个"针尖"
        ex, ey = polar(R_ARC, ANG_START + ANG_SPAN * frac)
        rr = ARC_W / 2.0
        d.ellipse([(ex - rr) * SS, (ey - rr) * SS,
                   (ex + rr) * SS, (ey + rr) * SS], fill=color)

    # 中心转速数字
    txt = '%.1f' % (rpm / 1000.0)
    d.text((CX * SS, 208 * SS), txt, font=font(112, bold=True),
           fill=pal['text'], anchor='mm')
    d.text((CX * SS, 272 * SS), '×1000 RPM', font=font(19),
           fill=pal['dim'], anchor='mm')

    # 水温
    if coolant < 55:
        c = pal['cold']
    elif coolant >= 105:
        c = pal['hot']
    else:
        c = pal['text']
    d.text((CX * SS, 392 * SS), str(coolant), font=font(40, bold=True),
           fill=c, anchor='mm')
    # °C 紧跟数字右侧
    wnum = d.textlength(str(coolant), font=font(40, bold=True)) / SS
    d.text(((CX + wnum / 2 + 6) * SS, 398 * SS), '°C', font=font(20),
           fill=pal['dim'], anchor='lm')

    img = img.resize((W, W), Image.LANCZOS)
    img.save(path)
    print('saved', path)


if __name__ == '__main__':
    day_bg = os.path.join(OUT, 'bg_day_static.png')
    night_bg = os.path.join(OUT, 'bg_night_static.png')
    render_bg(DAY, day_bg)
    render_bg(NIGHT, night_bg)

    for rpm, clt, tag in [(1600, 62, 'idle'), (4600, 84, 'cruise'), (8800, 96, 'hot')]:
        render_preview(NIGHT, rpm, clt, os.path.join(OUT, f'night_{tag}.png'), night_bg)
    render_preview(DAY, 4600, 84, os.path.join(OUT, 'day_cruise.png'), day_bg)
