#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""飞度5 圆屏仪表 · 位图渲染

把 docs/UI规格-beta.html 的 beta-1 参数，按 466×466 真实像素渲染成：
  build/bg.png          静态背景（外圈/槽/色块/刻度/数字/单位字/水温标签）
  build/needle_body.png 指针针身（A8，灰度即 alpha，供 LVGL recolor）
  build/needle_hi.png   指针高光条（A8，恒白，叠在针身上）

设计单位 ≈ 屏幕像素（直径 464 vs 466，误差可忽略），圆心 (233,233)。
所有数值直接来自规格表，改设计先改规格表再回来改这里。
"""
import os
import numpy as np
from PIL import Image, ImageDraw, ImageFont, ImageFilter

# ---------- 画布 ----------
W = 466
CX = CY = 233.0
SS = 3                     # 超采样倍数


def _pick(cands):
    """按顺序挑第一个存在的字体，返回 (路径, ttc 索引)。Windows / Linux 通用。"""
    for path, idx in cands:
        if os.path.exists(path):
            return path, idx
    raise SystemExit('找不到可用字体，请修改 FONT_* 候选列表：%r' % (cands,))


# 中文：Noto Sans CJK（Linux）/ Noto Sans SC（Windows）/ 微软雅黑 / 黑体
FONT_CJK, F_SC = _pick([
    ('/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc', 2),
    ('/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttc', 2),
    ('C:/Windows/Fonts/NotoSansSC-VF.ttf', 0),
    ('C:/Windows/Fonts/msyh.ttc', 0),
    ('C:/Windows/Fonts/simhei.ttf', 0),
])
# 数字与拉丁文字用 Liberation Sans / Arial（度量接近，比 CJK 字体的数字窄且细）
FONT_LAT, _ = _pick([
    ('/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf', 0),
    ('C:/Windows/Fonts/arial.ttf', 0),
])

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'build')
os.makedirs(OUT, exist_ok=True)


def font(size, cjk=False, weight='Regular'):
    """数字/拉丁用 Liberation Sans / Arial；中文用 Noto Sans CJK SC。"""
    if cjk:
        path = FONT_CJK
        if weight != 'Regular':
            path = path.replace('Regular', weight)
        return ImageFont.truetype(path, int(size * SS), index=F_SC)
    return ImageFont.truetype(FONT_LAT, int(size * SS))



# ---------- 坐标系（超采样网格，单位：设计像素） ----------
_y, _x = np.mgrid[0:W * SS, 0:W * SS].astype(np.float32)
X = (_x + 0.5) / SS
Y = (_y + 0.5) / SS
DX = X - CX
DY = Y - CY
R = np.hypot(DX, DY)
# 与 SVG 一致：0° 在 12 点方向，顺时针增大
TH = np.degrees(np.arctan2(DX, -DY)) % 360.0


# ---------- 基础工具 ----------
def ang_in(a0, a1, pad=0.0):
    """角度区间掩码，支持 a1 > 360 的写法。"""
    span = a1 - a0
    aa = np.mod(TH - a0, 360.0)
    if span >= 360.0:
        return np.ones_like(R, dtype=bool)
    m = aa <= span
    if pad:
        m |= (np.mod(TH - a0 - pad, 360.0) <= span + pad * 2)
    return m


def disc(r):
    return R <= r


def ring(r, w):
    return (R <= r + w / 2) & (R >= r - w / 2)


def sector(r_in, r_out, a0, a1):
    return (R >= r_in) & (R <= r_out) & ang_in(a0, a1)


def arc(r, w, a0, a1):
    """带圆头的弧线。圆头用两个端点圆补。"""
    m = ring(r, w) & ang_in(a0, a1)
    for a in (a0, a1):
        ex = CX + r * np.sin(np.radians(a))
        ey = CY - r * np.cos(np.radians(a))
        m |= (np.hypot(X - ex, Y - ey) <= w / 2)
    return m


def lin_grad(stops, p0, p1):
    """沿 p0→p1 的线性渐变，返回 (H,W,3) float32 0..1。stops=[(t,(r,g,b)),...]"""
    dx, dy = p1[0] - p0[0], p1[1] - p0[1]
    L2 = max(dx * dx + dy * dy, 1e-9)
    t = np.clip(((X - p0[0]) * dx + (Y - p0[1]) * dy) / L2, 0.0, 1.0)
    out = np.zeros(X.shape + (3,), dtype=np.float32)
    ts = [s[0] for s in stops]
    cs = [np.array(s[1], dtype=np.float32) for s in stops]
    out[:] = cs[0]
    for i in range(len(ts) - 1):
        seg = (t >= ts[i]) & (t <= ts[i + 1])
        k = ((t - ts[i]) / max(ts[i + 1] - ts[i], 1e-9))[..., None]
        out[seg] = (cs[i] * (1 - k[seg]) + cs[i + 1] * k[seg])
    return out


def hx(s):
    s = s.lstrip('#')
    return tuple(int(s[i:i + 2], 16) / 255.0 for i in (0, 2, 4))


def flat(c):
    v = np.array(hx(c) if isinstance(c, str) else c, dtype=np.float32)
    return np.broadcast_to(v, X.shape + (3,)).copy()


# ---------- 画布缓冲 ----------
class Canvas:
    def __init__(self):
        self.rgb = np.zeros(X.shape + (3,), dtype=np.float32)
        self.a = np.zeros(X.shape, dtype=np.float32)

    def over(self, color, mask, opacity=1.0):
        a = mask.astype(np.float32) * opacity
        col = color if isinstance(color, np.ndarray) else flat(color)
        na = a + self.a * (1 - a)
        safe = np.maximum(na, 1e-6)
        self.rgb = (col * a[..., None] + self.rgb * self.a[..., None] * (1 - a[..., None])) / safe[..., None]
        self.a = na

    def shadow(self, mask, dx, dy, sigma, color, opacity):
        """把 mask 高斯模糊后按 (dx,dy) 偏移合成，用于 feDropShadow。"""
        img = Image.fromarray((np.clip(mask, 0, 1) * 255).astype(np.uint8), 'L')
        blur = np.array(img.filter(ImageFilter.GaussianBlur(sigma * SS)), dtype=np.float32) / 255.0
        sy, sx = int(round(dy * SS)), int(round(dx * SS))
        sh = np.zeros_like(blur)
        h, w = blur.shape
        ys0, ys1 = max(0, sy), min(h, h + sy)
        xs0, xs1 = max(0, sx), min(w, w + sx)
        sh[ys0:ys1, xs0:xs1] = blur[max(0, -sy):max(0, -sy) + (ys1 - ys0),
                                    max(0, -sx):max(0, -sx) + (xs1 - xs0)]
        # 以 alpha 权重叠加（阴影是半透明的，不能先铺一层不透明色）
        a = sh * opacity
        na = a + self.a * (1 - a)
        safe = np.maximum(na, 1e-6)
        col = flat(color)
        self.rgb = (col * a[..., None] + self.rgb * self.a[..., None] * (1 - a[..., None])) / safe[..., None]
        self.a = na

    def text(self, s, xy, fnt, color, anchor='mm', opacity=1.0, tracking=0):
        layer = Image.new('L', (W * SS, W * SS), 0)
        d = ImageDraw.Draw(layer)
        if tracking:
            # 手动加字距：逐字绘制
            total = sum(d.textlength(ch, font=fnt) + tracking * SS for ch in s) - tracking * SS
            x = xy[0] * SS
            if anchor[0] == 'm':
                x -= total / 2
            elif anchor[0] == 'r':
                x -= total
            for ch in s:
                d.text((x, xy[1] * SS), ch, font=fnt, fill=255, anchor='l' + anchor[1])
                x += d.textlength(ch, font=fnt) + tracking * SS
        else:
            d.text((xy[0] * SS, xy[1] * SS), s, font=fnt, fill=255, anchor=anchor)
        self.over(color, np.array(layer, dtype=np.float32) / 255.0, opacity)

    def image(self):
        arr = np.concatenate([np.clip(self.rgb, 0, 1), np.clip(self.a, 0, 1)[..., None]], -1)
        im = Image.fromarray((arr * 255).astype(np.uint8), 'RGBA')
        return im.resize((W, W), Image.LANCZOS)


# ============================================================
# 背景（两套主题：白天 / 黑夜）
# ============================================================
# 白天的配色 = beta-1 基线，逐字照抄，**不要改**（改了就偏离已验收的稿子）
THEME_DAY = dict(
    plate=('#ffffff', '#c9cdd4'),
    rim=('#ffffff', '#e6e9ee', '#c1c6ce', '#99a0aa'),
    bevel=('#ffffff', 0.92),
    inner_line=('#8d939d', 0.60),
    top_arc=('#ffffff', 0.72),
    slot=('#b6bac2', '#d8dbe1', '#eef0f3'),
    slot_wall=('#9aa0aa', 0.50),
    slot_rim=('#ffffff', 0.85),
    tick_major=('#5a5f68', 0.85),
    tick_minor=('#8a8f98', 0.45),
    face=('#fdfdff', '#d9dbe0'),
    face_shadow=('#6b7280', 0.14),
    face_edge=('#ffffff', 0.80),
    num='#2b2e35',
    label='#9aa0a9',
)

# 黑夜：同样的结构，把"亮金属"换成"暗金属"——
# 关键是明暗方向不能反：顶亮底暗表示凸起，凹槽依旧顶暗底亮。
THEME_NIGHT = dict(
    plate=('#3c424b', '#14161a'),
    rim=('#515863', '#3b414a', '#282c33', '#141619'),
    bevel=('#97a0ad', 0.30),
    inner_line=('#040507', 0.80),
    top_arc=('#c2cad6', 0.20),
    slot=('#101216', '#191c21', '#23272e'),
    slot_wall=('#050608', 0.70),
    slot_rim=('#5a626d', 0.45),
    tick_major=('#ced4dd', 0.90),
    tick_minor=('#7f868f', 0.55),
    face=('#2c313a', '#171a1f'),
    face_shadow=('#000000', 0.55),
    face_edge=('#565d67', 0.50),
    num='#e7eaef',
    label='#8c939c',
)


def render_bg(pal, out_name):
    """按主题渲染整张背景。pal 是上面两个 dict 之一。"""
    c = Canvas()

    # 1. 外底板 r=232（竖直渐变）
    g = lin_grad([(0.0, hx(pal['plate'][0])), (1.0, hx(pal['plate'][1]))],
                 (CX, CY - 232), (CX, CY + 232))
    c.over(g, disc(232))

    # 2. 金属环带 r=229 宽 6，竖直四停靠
    r0, r1, r2, r3 = pal['rim']
    g = lin_grad([(0.00, hx(r0)), (0.34, hx(r1)), (0.70, hx(r2)), (1.00, hx(r3))],
                 (CX, CY - 229), (CX, CY + 229))
    c.over(g, ring(229, 6))

    # 3. 外倒角高光
    c.over(pal['bevel'][0], ring(231.7, 1.5), pal['bevel'][1])

    # 4. 内缘暗线
    c.over(pal['inner_line'][0], ring(226.4, 1.2), pal['inner_line'][1])

    # 5. 顶部反射弧 ±52°
    c.over(pal['top_arc'][0], arc(229.6, 2.6, -52, 52), pal['top_arc'][1])

    # 6. 红区下沉槽 sector(198,226, 228..492)
    sector_mask = sector(198, 226, 228, 492)
    bx0, bx1 = CX - 226, CX + 226
    by0 = CY - 226
    by1 = CY + 151          # 228° 端点的 y
    g = lin_grad([(0.00, hx(pal['slot'][0])), (0.5, hx(pal['slot'][1])), (1.0, hx(pal['slot'][2]))],
                 (bx0, by0), (bx0 + (bx1 - bx0), by0 + 0.55 * (by1 - by0)))
    c.over(g, sector_mask)

    # 7. 槽内壁 / 外沿
    c.over(pal['slot_wall'][0], arc(199.5, 1.5, 228, 492), pal['slot_wall'][1])
    c.over(pal['slot_rim'][0], arc(224.5, 1.2, 228, 492), pal['slot_rim'][1])

    # 8. 色块（比槽窄 9px，四周露槽底）—— 两套主题同色，暖色在暗底上更亮眼
    c.over('#F0812E', sector(202.5, 221.5, 432.4, 455.6))
    c.over('#E23A2C', sector(202.5, 221.5, 456.4, 480))

    # 9. 刻度：每 500 一根
    for v in range(0, 10001, 500):
        a = np.radians(240 + 240 * v / 10000.0)
        major = (v % 1000 == 0)
        r1_ = 168 if major else 178
        x0, y0 = CX + r1_ * np.sin(a), CY - r1_ * np.cos(a)
        x1, y1 = CX + 192 * np.sin(a), CY - 192 * np.cos(a)
        vx, vy = x1 - x0, y1 - y0
        L2 = vx * vx + vy * vy
        t = np.clip(((X - x0) * vx + (Y - y0) * vy) / L2, 0, 1)
        d = np.hypot(X - (x0 + vx * t), Y - (y0 + vy * t))
        w = 2.2 if major else 1.0
        key = 'tick_major' if major else 'tick_minor'
        c.over(pal[key][0], d <= w / 2, pal[key][1])

    # 10. 内盘面 r=152（竖直渐变 + 投影）
    face = disc(152)
    c.shadow(face, 0, 4, 7, pal['face_shadow'][0], pal['face_shadow'][1])
    g = lin_grad([(0.0, hx(pal['face'][0])), (1.0, hx(pal['face'][1]))],
                 (CX, CY - 152), (CX, CY + 152))
    c.over(g, face)

    # 11. 盘面描边 r=151
    c.over(pal['face_edge'][0], ring(151, 1.0), pal['face_edge'][1])

    # 12. 刻度数字 0–10，r=129，27px
    fnum = font(27)
    for w in range(0, 10001, 1000):
        a = np.radians(240 + 240 * w / 10000.0)
        px_ = CX + 129 * np.sin(a)
        py_ = CY - 129 * np.cos(a)
        c.text(str(w // 1000), (px_, py_), fnum, pal['num'], anchor='mm')

    # 13. 单位小字（设计 y=203 → 屏 186）
    c.text('1/min ×1000', (CX, 186), font(12.5), pal['label'], anchor='mm', tracking=1.2)

    # 14. 水温行静态部分：标签与单位（设计 x=302/390 → 屏 195/283，y=366 → 349）
    c.text('水温', (195, 349), font(15, cjk=True), pal['label'], anchor='rm', tracking=1.0)
    c.text('°C', (283, 349), font(18), pal['label'], anchor='lm')

    img = c.image()
    img.save(os.path.join(OUT, out_name))
    print(out_name, img.size, img.mode)


render_bg(THEME_DAY, 'bg.png')
render_bg(THEME_NIGHT, 'bg_dark.png')



# ============================================================
# 指针（360×360，旋转中心在图片中心）
# ============================================================
NS = 360
NC = 180.0
_ny, _nx = np.mgrid[0:NS, 0:NS].astype(np.float32)
NX = _nx + 0.5 - NC
NY = _ny + 0.5 - NC

BODY = [(0, -178), (5.2, -20), (2.6, 54), (0, 60), (-2.6, 54), (-4.8, -20)]
HI = [(-1.6, -150), (1.4, -22), (0.3, 46)]


def poly_mask(pts):
    """even-odd 多边形填充 + 3x 抗锯齿（在 1x 精度上做超采样栅格化）。"""
    from PIL import ImageDraw as ID
    k = 4
    img = Image.new('L', (NS * k, NS * k), 0)
    ID.Draw(img).polygon([((p[0] + NC) * k, (p[1] + NC) * k) for p in pts], fill=255)
    return np.array(img.resize((NS, NS), Image.LANCZOS), dtype=np.float32) / 255.0


def seg_mask(pts, w):
    """沿折线加宽 w 的软边条带。"""
    m = np.zeros((NS, NS), dtype=np.float32)
    for i in range(len(pts) - 1):
        x0, y0 = pts[i]
        x1, y1 = pts[i + 1]
        vx, vy = x1 - x0, y1 - y0
        L2 = max(vx * vx + vy * vy, 1e-9)
        t = np.clip(((NX - x0) * vx + (NY - y0) * vy) / L2, 0, 1)
        d = np.hypot(NX - (x0 + vx * t), NY - (y0 + vy * t))
        m = np.maximum(m, np.clip(w / 2 + 0.5 - d, 0, 1))
    return m


body = poly_mask(BODY)
# 针尖白点 r=2.8，圆心 (0,-176)
body = np.maximum(body, np.clip(2.8 - np.hypot(NX - 0, NY + 176) + 0.5, 0, 1))

# 高光条直接压进 alpha：高光区 alpha 降到 0.55，recolor 后在浅色盘面上
# 会透出背景色，视觉上仍是高光。这样只有一个旋转图层，省一半时间。
_hi = seg_mask(HI, 1.7) * 0.45
body = np.clip(body * (1.0 - _hi), 0, 1)

Image.fromarray((body * 255).astype(np.uint8), 'L').save(os.path.join(OUT, 'needle_body.png'))
print('needle_body.png 覆盖率 %.3f（高光已并入）' % (body > 0.5).mean())


# ============================================================
# 轴心（含投影，盖在指针之上）
# ============================================================
HS = 152                       # 图片边长：r36 + 投影扩散
HC = HS / 2.0
_hy, _hx = np.mgrid[0:HS, 0:HS].astype(np.float32)
HX = _hx + 0.5 - HC
HY = _hy + 0.5 - HC
HR = np.hypot(HX, HY)

hub = Canvas.__new__(Canvas)   # 复用 Canvas，但换成 hub 的网格
hub.rgb = np.zeros((HS, HS, 3), dtype=np.float32)
hub.a = np.zeros((HS, HS), dtype=np.float32)
# 临时把模块级网格切过去，让 over/shadow 用 hub 坐标
_X, _Y, _R, _DX, _DY, _TH = X, Y, R, DX, DY, TH
X, Y, R, DX, DY, TH = HX, HY, HR, HX, HY, None

# hub 的网格是 1x（没有超采样），阴影参数要按 1x 走
_SS_SAVE = SS
SS = 1
hub.shadow(HR <= 36, 0, 7, 9, '#5b616b', 0.28)
SS = _SS_SAVE

# 金属面：径向渐变，高光偏左上 (cx=.36 cy=.28)
gx, gy, gr = -36 + 0.36 * 72, -36 + 0.28 * 72, 0.82 * 72
d = np.clip(np.hypot(HX - gx, HY - gy) / gr, 0, 1)
stops = [(0.00, hx('#ffffff')), (0.42, hx('#e4e7ec')), (0.78, hx('#c6cbd3')), (1.00, hx('#9ba1aa'))]
rg = np.zeros((HS, HS, 3), dtype=np.float32)
rg[:] = stops[0][1]
for i in range(len(stops) - 1):
    seg = (d >= stops[i][0]) & (d <= stops[i + 1][0])
    k = ((d - stops[i][0]) / max(stops[i + 1][0] - stops[i][0], 1e-9))[..., None]
    rg[seg] = stops[i][1] * (1 - k[seg]) + stops[i + 1][1] * k[seg]
hub.over(rg, HR <= 36)
hub.over('#8b919b', (HR <= 36.45) & (HR >= 35.55), 0.55)

# 内凹盘：竖直渐变，顶暗底亮（与外圈相反）
gd = np.clip((HY + 19) / 38.0, 0, 1)
dg = np.zeros((HS, HS, 3), dtype=np.float32)
d_stops = [(0.00, hx('#9aa0a9')), (0.60, hx('#d5d9df')), (1.00, hx('#f2f3f6'))]
dg[:] = d_stops[0][1]
for i in range(len(d_stops) - 1):
    seg = (gd >= d_stops[i][0]) & (gd <= d_stops[i + 1][0])
    k = ((gd - d_stops[i][0]) / max(d_stops[i + 1][0] - d_stops[i][0], 1e-9))[..., None]
    dg[seg] = d_stops[i][1] * (1 - k[seg]) + d_stops[i + 1][1] * k[seg]
hub.over(dg, HR <= 19)
hub.over('#9aa0aa', (HR <= 19.4) & (HR >= 18.6), 0.5)

# 轴心点 r=7
hub.over('#7d838c', HR <= 7)

X, Y, R, DX, DY, TH = _X, _Y, _R, _DX, _DY, _TH

arr = np.concatenate([np.clip(hub.rgb, 0, 1), np.clip(hub.a, 0, 1)[..., None]], -1)
Image.fromarray((arr * 255).astype(np.uint8), 'RGBA').save(os.path.join(OUT, 'hub.png'))
print('hub.png', HS, 'alpha 覆盖率 %.3f' % (hub.a > 0.01).mean())
