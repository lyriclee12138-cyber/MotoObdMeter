#!/usr/bin/env python3
"""读板子串口日志（Windows COM 口）。

用法: python log.py [秒数] [端口]
注意: ESP32-S3 的 USB-JTAG 必须 DTR=False / RTS=False，否则一行都读不到。
"""
import sys
import time
import serial

PORT = sys.argv[2] if len(sys.argv) > 2 else 'COM4'
SECS = float(sys.argv[1]) if len(sys.argv) > 1 else 12.0

s = serial.Serial()
s.port = PORT
s.baudrate = 115200
s.timeout = 0.2
s.dtr = False
s.rts = False
s.open()
s.reset_input_buffer()

t0 = time.time()
buf = b''
while time.time() - t0 < SECS:
    buf += s.read(8192)
s.close()

for line in buf.decode('utf-8', 'replace').splitlines():
    if line.strip():
        print(line)
