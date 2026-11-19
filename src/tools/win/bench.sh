#!/usr/bin/env bash
# 编译 → 烧录 → 复位 → 采集性能日志，一条命令跑完。
#
# 用法: ./bench.sh [采集秒数]
#   例: ./bench.sh 20
#
# 依赖: tools/win/idf.bat（ESP-IDF 环境）、tools/win/log.py（读串口）
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)          # 项目根 src/
RECV="$ROOT/firmware/receiver"
IDF="$HERE/idf.bat"
PY=/c/Espressif/tools/python/v5.5.5/venv/Scripts/python.exe
PORT=${PORT:-COM4}
SECS=${1:-16}

if ! "$IDF" -B "$RECV/build" -C "$RECV" build > "$HERE/bench_build.log" 2>&1; then
    echo "!! 编译失败"; iconv -f GBK -t UTF-8 "$HERE/bench_build.log" | grep -iE "error" | head -8; exit 1
fi
# idf.py 有时 ninja 失败也返回 0，必须再查日志
if grep -qE "ninja failed|compilation terminated|: error:" "$HERE/bench_build.log"; then
    echo "!! 编译失败（详见 tools/win/bench_build.log）"
    iconv -f GBK -t UTF-8 "$HERE/bench_build.log" | grep -iE ": error:|ninja failed" | head -6
    exit 1
fi

if ! "$IDF" -B "$RECV/build" -C "$RECV" -p "$PORT" flash > "$HERE/bench_flash.log" 2>&1; then
    echo "!! 烧录失败"; tail -5 "$HERE/bench_flash.log"; exit 1
fi

sleep 2
# idf.py flash 的 hard reset 对本板不可靠，必须再用 esptool 显式复位一次
"$PY" -m esptool --chip esp32s3 -p "$PORT" --after hard_reset run > /dev/null 2>&1
python "$HERE/log.py" "$SECS" "$PORT" | grep -E "perf: 绘制|ui:|receiver:|E \(" | tail -12
