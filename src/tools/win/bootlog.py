#!/usr/bin/env python3
"""复位板子并抓启动日志（用于看 init 阶段的一次性打印）。

用法: python bootlog.py [秒数] [端口]
"""
import sys
import time
import serial

SECS = float(sys.argv[1]) if len(sys.argv) > 1 else 4.0
PORT = sys.argv[2] if len(sys.argv) > 2 else 'COM4'

s = serial.Serial()
s.port = PORT
s.baudrate = 115200
s.timeout = 0.2
s.dtr = False
s.rts = False
s.open()

s.setRTS(True)          # EN 拉低 = 复位
time.sleep(0.08)
s.setRTS(False)         # 放开 = 运行

t0 = time.time()
buf = b''
while time.time() - t0 < SECS:
    buf += s.read(8192)
s.close()

for line in buf.decode('utf-8', 'replace').splitlines():
    if line.strip():
        print(line)
