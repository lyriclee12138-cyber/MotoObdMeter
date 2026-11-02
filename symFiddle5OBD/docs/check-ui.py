#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""飞度5 圆屏仪表 · UI 基线一致性校验

用途：改完 gauge-ui*.html 后跑一遍，自动比对 beta-1 规格。
     python3 docs/check-ui.py docs/gauge-ui.html
     python3 docs/check-ui.py            # 默认校验 beta 基线

校验三类内容：
  A. 必备参数 —— 规格表第三节的硬性数值，缺一个就报错
  B. 禁用元素 —— beta-1 明确不含的东西，出现就报错（防止"又加回去了"）
  C. 结构完整性 —— 标签闭合、绘制顺序

退出码 0 = 全部通过；1 = 有 FAIL。
"""
import sys
import re

# ---------- A. 必备参数（与 UI规格-beta.html 第三节一一对应） ----------
REQUIRED = [
    # 画布与角度
    ("角度起点 A0=240",        "A0=240"),
    ("量程 RMAX=10000",        "RMAX=10000"),
    ("扫角 SW=240",            "SW=240"),
    ("圆心 (340,250)",         "CX=340, CY=250"),
    # 外圈
    ("外底板 r=232",           'r="232"'),
    ("环带 r=229 sw=6",        'r="229"'),
    ("外倒角 r=231.7",         'r="231.7"'),
    ("内缘暗线 r=226.4",       'r="226.4"'),
    ("顶部反射弧 A229.6",      "A229.6 229.6"),
    # 红区槽
    ("槽 sector(198,226)",     "sector(198,226,228,492)"),
    ("槽内壁 199.5",           "arcPath(199.5,228,492)"),
    ("槽外沿 224.5",           "arcPath(224.5,228,492)"),
    ("橙块 432.4-455.6",       "sector(202.5,221.5,432.4,455.6)"),
    ("红块 456.4-480",         "sector(202.5,221.5,456.4,480)"),
    # 刻度与数字
    ("刻度 168→192",           "x2:PX(192,a)"),
    ("次刻度起点 178",         "maj?168:178"),
    ("数字半径 129",           "PX(129,aa)"),
    ("数字字号 27",            "'font-size':27"),
    ("数字必须为 v/1000",      "t.textContent = w/1000;"),
    ("单位小字 1/min",         "1/min ×1000"),
    # 盘面
    ("内盘面 r=152",           'r="152"'),
    ("盘面白边 r=151",         'r="151"'),
    # 水温行
    ("水温标签 x=302",         'id="tLabel" x="302"'),
    ("温度值 x=346",           'id="tVal" x="346"'),
    ("°C x=390",               'id="tUnit" x="390"'),
    ("温度值字号 46",          'font-size="46"'),
    ("水温行 y=366",           'y="366"'),
    # 指针
    ("指针尾部 y=310",         "L340 310"),
    ("指针尖端 y=72",          "M340 72"),
    ("指针描边系数 0.62",      "dark(c,0.62)"),
    ("指针投影中性灰",         "#5d5f66"),
    # 轴心
    ("轴心 r=36",              'r="36"'),
    ("内凹盘 r=19",            'r="19"'),
    ("轴心点 r=7",             'r="7"'),
    ("金属渐变偏心 cx=.36",    'cx="0.36"'),
    # 颜色表
    ("绿 #22c55e",             "[0,[34,197,94]]"),
    ("亮绿 #4ade80",           "[0.40,[74,222,128]]"),
    ("黄 #facc15",             "[0.60,[250,204,21]]"),
    ("橙 #fb923c",             "[0.78,[251,146,60]]"),
    ("红 #ef4444",             "[0.86,[239,68,68]]"),
    ("深红 #dc2626",           "[1,[220,38,38]]"),
    ("温度冷车 #2E7FD0",       "#2E7FD0"),
    ("温度过热 #E23A2C",       "#E23A2C"),
]

# ---------- B. 禁用元素（beta-1 明确不含） ----------
FORBIDDEN = [
    ("底部灰弧（±38°）",       "M198.6 431"),
    ("底部弧颜色 #8c929c",     "#8c929c"),
    ("盘面装饰弧 r=134",       'r="134"'),
    ("温度计裁剪 thermoClip",  "thermoClip"),
    ("温度计本体 thBody",      "thBody"),
    ("温度计图标 g#thermo",    'id="thermo"'),
    ("轴心外台 hubBaseG",      "hubBaseG"),
    ("轴心凹槽环 hubGrooveG",  "hubGrooveG"),
    ("轴心凸起面 hubTopG",     "hubTopG"),
]

# ---------- C. 绘制顺序（必须递增） ----------
ORDER = [
    ('外底板',   'r="232" fill="url(#plateG)"'),
    ('环带',     'r="229" fill="none"'),
    ('槽底',     'id="slot"'),
    ('刻度',     'id="ticks"'),
    ('内盘面',   'r="152" fill="url(#faceG)"'),
    ('数字',     'id="nums"'),
    ('水温行',   'id="tempRow"'),
    ('指针',     'id="needle"'),
    ('轴心',     'id="hub"'),
]


def check(path):
    s = open(path, encoding='utf-8').read()
    fails = []

    print("A. 必备参数")
    for name, pat in REQUIRED:
        ok = pat in s
        print(f"   {'OK  ' if ok else 'FAIL'}  {name}")
        if not ok:
            fails.append(f"缺少 {name}（期望 `{pat}`）")

    print("\nB. 禁用元素")
    for name, pat in FORBIDDEN:
        hit = pat in s
        print(f"   {'FAIL' if hit else 'OK  '}  {name}")
        if hit:
            fails.append(f"出现了不该有的 {name}（`{pat}`）")

    print("\nC. 绘制顺序")
    pos = []
    for name, pat in ORDER:
        i = s.find(pat)
        if i < 0:
            fails.append(f"找不到 {name}（`{pat}`）")
            pos.append(-1)
        else:
            pos.append(i)
    valid = [p for p in pos if p >= 0]
    ordered = valid == sorted(valid)
    print(f"   {'OK  ' if ordered else 'FAIL'}  DOM 顺序单调递增")
    if not ordered:
        fails.append("绘制顺序被打乱，检查 5.1 节")

    # 轴心必须在指针之后（贯穿感）
    ni, hi = s.find('id="needle"'), s.find('id="hub"')
    if not (0 <= ni < hi):
        fails.append("轴心必须画在指针之后，否则失去贯穿感")
        print("   FAIL  轴心在指针之后")
    else:
        print("   OK    轴心在指针之后")

    print("\nD. 结构完整性")
    for t in ['html', 'head', 'body', 'div', 'svg', 'defs', 'g', 'text', 'table', 'script', 'style']:
        o = len(re.findall(r'<%s[ >]' % t, s))
        c = len(re.findall(r'</%s>' % t, s))
        ok = (o == c) or t in ('circle',)
        print(f"   {'OK  ' if ok else 'FAIL'}  <{t}> 开 {o} / 闭 {c}")
        if not ok:
            fails.append(f"<{t}> 标签不配平（{o}/{c}）")

    print("\n" + "=" * 52)
    if fails:
        print(f"结果：{len(fails)} 项不通过")
        for f in fails:
            print("  · " + f)
        return 1
    print("结果：全部通过，与 beta-1 规格一致")
    return 0


if __name__ == '__main__':
    target = sys.argv[1] if len(sys.argv) > 1 else 'docs/gauge-ui-beta.html'
    print(f"校验目标：{target}\n")
    sys.exit(check(target))
