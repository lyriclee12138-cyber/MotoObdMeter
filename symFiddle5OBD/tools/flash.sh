#!/bin/bash
# 接收端烧录脚本
#   用法： ./tools/flash.sh [串口]
#   不传串口时自动找 /dev/ttyACM* 或 /dev/ttyUSB*
set -e

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
source "$HOME/esp/env.sh"

PORT="${1:-}"
if [ -z "$PORT" ]; then
    for p in /dev/ttyACM0 /dev/ttyACM1 /dev/ttyUSB0 /dev/ttyUSB1; do
        [ -e "$p" ] && PORT="$p" && break
    done
fi

if [ -z "$PORT" ]; then
    echo "没找到串口。插好屏后重试，或手动指定： $0 /dev/ttyACM0"
    echo "当前可用串口："
    ls /dev/ttyACM* /dev/ttyUSB* 2>/dev/null || echo "  （一个都没有）"
    exit 1
fi

echo "使用串口：$PORT"
cd "$ROOT/firmware/receiver"

# 板子若是 USB-JTAG/Serial 复合设备，端口可能会在烧录过程中重新枚举
idf.py -B build_v2 -p "$PORT" flash

echo
echo "烧录完成。看日志请运行："
echo "  source ~/esp/env.sh && cd $ROOT/firmware/receiver && idf.py -B build_v2 -p $PORT monitor"
echo "退出 monitor 用 Ctrl+]"
