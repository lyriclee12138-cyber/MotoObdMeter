#!/usr/bin/env bash
# 重新生成中文字体子集。
#
# 字符集不手写——直接从 firmware/receiver/main/*.c 里扫出所有中文字符。
# 手写字符集漏一个字，界面上那个字就是空白，而且不会报错，很难查（已踩过）。
#
# 三条硬性要求：
#   1. --no-compress  —— 不加的话生成 RLE 压缩字形（bitmap_format=1），
#      而本工程 CONFIG_LV_USE_FONT_COMPRESSED 是关的，LVGL 解不出来，屏幕上一片空白。
#   2. --lv-include lvgl.h —— 否则生成文件里是 #ifdef LV_LVGL_H_INCLUDE_SIMPLE 的空分支。
#   3. 必须是静态 TTF。NotoSansSC-VF.ttf 是可变字体（CFF2），lv_font_conv 转出来字形是空的。
#
# 另一条易错点（在 C 代码里，不在本脚本）：给 label 设字体必须在 set_text 之前，
# 否则 LVGL 会用没有中文字形的默认字体算内容宽度 → 标签 0 宽 → 整段中文被裁掉。
#
# 用法：./tools/gen_fonts.sh
set -e
cd "$(dirname "$0")/.."

export PATH="/c/Users/Lee/.workbuddy/binaries/node/versions/22.22.2-6:$PATH"
FONT="C:/Windows/Fonts/Deng.ttf"        # 等线，静态 TTF，字形干净
[ -f "$FONT" ] || FONT="C:/Windows/Fonts/simhei.ttf"
OUT=firmware/receiver/main/fonts

# 只从「字符串字面量」里抽非 ASCII 字符（注释里的不算，否则字符集会膨胀到几百个）。
# 注意必须收「所有非 ASCII」，不能只收中文：像 "USB 供电 · 已充满" 里的 "·"(U+00B7)
# 既不在 ASCII 区也不在 CJK 区，漏掉就会渲染成空白，看起来像多个空格。
CHARS=$(python - <<'PY'
import glob, re
chars = set()
for f in glob.glob('firmware/receiver/main/*.c') + glob.glob('firmware/receiver/main/*.h'):
    src = open(f, encoding='utf-8').read()
    src = re.sub(r'/\*.*?\*/', '', src, flags=re.S)      # 块注释
    src = re.sub(r'//[^\n]*', '', src)                   # 行注释
    for lit in re.findall(r'"((?:[^"\\\n]|\\.)*)"', src):  # 字符串字面量
        for ch in lit:
            if ord(ch) > 0x7F:
                chars.add(ch)
print(''.join(sorted(chars)))
PY
)
echo "字符集共 $(printf %s "$CHARS" | wc -m) 个字：$CHARS"
[ -n "$CHARS" ] || { echo "!! 没扫到中文，检查源码编码"; exit 1; }

gen() {
    local size=$1 name=$2
    npx --yes lv_font_conv --size "$size" --bpp 4 --format lvgl \
        --font "$FONT" -r 0x20-0x7F --symbols "$CHARS" \
        --lv-font-name "$name" --no-compress --lv-include lvgl.h \
        -o "$OUT/$name.c"
    echo "生成 $OUT/$name.c"
}

gen 16 font_cjk_16
gen 22 font_cjk_22
gen 32 font_cjk_32

# 校验：没有 RLE 压缩
for f in "$OUT"/font_cjk_*.c; do
    if grep -q "bitmap_format = 1" "$f"; then
        echo "!! $f 还是压缩字形，界面会一片空白"; exit 1
    fi
done
echo "全部无压缩，OK"
