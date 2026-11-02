#!/usr/bin/env bash
# 出一条可以放到手机上烧录的固件包（接收端 + 发送端一起出）。
#
# 用法：
#   ./tools/win/pack_firmware.sh            # 两套都出（默认）
#   ./tools/win/pack_firmware.sh receiver   # 只出圆屏接收端
#   ./tools/win/pack_firmware.sh sender     # 只出发送端
#
# 每次都会重写各自的 main/version_env.h（内含编译时刻），所以 version.c 必然重编、
# 固件里的版本串必然跟着变 —— 这是判断「烧进去的到底是不是新固件」的唯一可靠依据。
# 版本串形如 v2026.11.02-153012（精确到秒，同一分钟内出两次包也不会撞车）。
# 收发两端用**同一个**版本串，一眼能看出这两个包是一起出的。
#
# 产物在 firmware_out/：
#   fiddle5_receiver_<时间戳>_full.bin  从 0x0 烧（bootloader + 分区表 + app）
#   fiddle5_receiver_<时间戳>_app.bin   从 0x10000 烧（只换 app）
#   fiddle5_sender_<时间戳>_full.bin    同上，发送端（开发板 N16R8）
#   fiddle5_sender_<时间戳>_app.bin
#   烧录说明.txt                         两个包的参数与核对方法

set -e
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="$ROOT/../firmware_out"
SMB="/y/symFiddle5OBD/firmware"

RECV="$ROOT/firmware/receiver"
SEND="$ROOT/firmware/sender"

WHICH="${1:-both}"
case "$WHICH" in
    receiver|sender|both) ;;
    *) echo "用法: $0 [receiver|sender|both]"; exit 2 ;;
esac

mkdir -p "$OUT"

VER="v$(date '+%Y.%m.%d-%H%M%S')"
TS="$(date '+%Y-%m-%d %H:%M')"
BASE_TS="${VER#v}"

echo "== 版本 $VER =="

# write_version <main 目录>
# 内容每次都不同 → version.c 必然重编 → 固件里的版本串必然更新。
write_version() {
    cat > "$1/main/version_env.h" <<EOF
#pragma once

/* 自动生成，不要手改 —— 由 tools/win/pack_firmware.sh 每次出包时重写。
 * 内容带时间戳，所以每次出包 version.c 都会重编，固件里的版本串必然更新。 */
#define SYM_VERSION_STR "${VER}"
#define SYM_BUILD_TS    "${TS}"
EOF
}

# build_one <目录> <工程名> <产物前缀>
build_one() {
    local dir="$1" proj="$2" name="$3"

    echo "== 编译 $proj =="
    if ! "$ROOT/tools/win/idf.bat" -B "$dir/build" -C "$dir" build > /tmp/pack_${proj}.log 2>&1; then
        echo "!! $proj 编译失败"
        iconv -f GBK -t UTF-8 /tmp/pack_${proj}.log 2>/dev/null | grep -iE ": error:|undefined reference" | head -15
        exit 1
    fi
    iconv -f GBK -t UTF-8 /tmp/pack_${proj}.log 2>/dev/null | grep -aE "${proj}.bin binary size" | head -1

    echo "== 合并 $proj =="
    "$ROOT/tools/win/idf.bat" -B "$dir/build" -C "$dir" merge-bin -o "${proj}_merged.bin" > /tmp/pack_merge.log 2>&1

    cp "$dir/build/${proj}_merged.bin" "$OUT/${name}_full.bin"
    cp "$dir/build/${proj}.bin"       "$OUT/${name}_app.bin"
}

if [ "$WHICH" = "receiver" ] || [ "$WHICH" = "both" ]; then
    write_version "$RECV"
    build_one "$RECV" fiddle5_receiver "fiddle5_receiver_${BASE_TS}"
fi

if [ "$WHICH" = "sender" ] || [ "$WHICH" = "both" ]; then
    write_version "$SEND"
    build_one "$SEND" symfiddle_sender "fiddle5_sender_${BASE_TS}"
fi

echo "== 产物 =="
cd "$OUT"
for f in *.bin; do
    case "$f" in
        *"$BASE_TS"*) ;;
        *) continue ;;
    esac
    python - "$f" <<'PY'
import sys, hashlib
p = sys.argv[1]
d = open(p, 'rb').read()
print("  %-46s %7.2f MB  md5=%s  %s"
      % (p, len(d) / 1048576,
         hashlib.md5(d).hexdigest(),
         "OK" if d[0] == 0xE9 else "!! 镜像头异常"))
PY
done

cat > "$OUT/烧录说明.txt" <<EOF
fiddle5 · 手机烧录说明
====================================
版本号   ${VER}   （收发两端同一个版本串）
编译时间 ${TS}


【两个包，别烧错板子】

圆屏接收端  fiddle5_receiver_${BASE_TS}_full.bin / _app.bin
            ESP32-S3 · 微雪 AMOLED 1.75C 圆屏
            Flash 大小必须写 32MB

发送端      fiddle5_sender_${BASE_TS}_full.bin / _app.bin
            ESP32-S3 开发板 N16R8（双 USB-C 那块）
            Flash 大小 16MB


【用哪个文件】

*_full.bin   从 0x0 烧，包含 bootloader + 分区表 + 应用
              工具不能改烧录地址时用这个（默认就是 0x0，最保险）

*_app.bin    从 0x10000 烧，只换应用，保留板上已有 bootloader
              工具能改地址时用这个，快一点，且不碰分区表


【关键参数】填错可能烧完不启动

  芯片型号     ESP32-S3
  烧录地址     0x0（full） / 0x10000（app）
  Flash 模式   DIO
  Flash 频率   80MHz
  波特率       460800 或 921600（不稳就降到 115200）

  圆屏接收端   Flash 大小 32MB   ← 不是 4MB
  发送端       Flash 大小 16MB


【怎么确认烧进去了】

圆屏接收端：烧完进设置页，滑到最底部看「版本」卡片
    显示 ${VER}  → 成功
    没变或更旧     → 没烧进去，重烧

发送端：没有屏幕，只能看串口。启动第一行会打
    固件版本 ${VER}（编译于 ${TS}）
  手机端看不到串口也没关系 —— 烧完看圆屏设置页里的「发送端」是否变成
  「已连接」就够了：发送端跑起来并在发数据，那里才会变。


【手机端的 Flash 工具不发硬复位】

烧完屏幕卡住不重启是正常的，手动断电重启即可，不代表烧录失败。
版本号才是判据。


【发送端烧完没有任何数据？】

1. 先按住板上的 BOOT 键 1 秒 —— 会进入模拟数据模式，
   圆屏应该立刻开始扫表（转速在 1450~9200 之间来回）。
   扫表 = ESP-NOW 和屏幕都正常，问题在 CAN 侧。
2. 松开 BOOT 恢复真实数据，屏幕上水温会变成 "--"（还没读到 ECU）。
3. 接车后看串口日志，或看圆屏是否能读到转速。


【设置不会丢】

主题（白天/黑夜）和亮度存在接收端的 NVS 分区，烧这两个文件都不动 NVS。
发送端的里程/运行时长也在自己的 NVS 里，用 full 从 0x0 烧也不会清（除非工具整片擦除）。


【回滚】

文件名带秒级时间戳，旧包不会被覆盖，烧回旧的 full.bin 即可。
EOF

echo ""
echo "圆屏设置页应显示：$VER"
echo "发送端串口首行应显示：固件版本 $VER"

# ---------- 同步到共享盘 ----------
if [ -d "$SMB" ]; then
    if cp "$OUT"/*"$BASE_TS"*.bin "$OUT/烧录说明.txt" "$SMB/" 2>/dev/null; then
        echo ""
        echo "已同步到共享盘：\\\\192.168.50.1\\asus_share\\symFiddle5OBD\\firmware\\"
    else
        echo ""
        echo "!! 共享盘同步失败（文件已在本地 $OUT，可手动拷）"
    fi
else
    echo ""
    echo "共享盘 Y: 不可用，跳过同步（本地包在 $OUT）"
fi
