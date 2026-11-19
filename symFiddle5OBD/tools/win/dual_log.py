"""同时抓两个串口的日志，带前缀输出。用于两端联调排查。"""
import sys
import threading
import time

import serial

PORTS = [("SEND", "COM3"), ("RECV", "COM4")]
SECS = float(sys.argv[1]) if len(sys.argv) > 1 else 8.0

stop = time.time() + SECS


def worker(tag, port):
    try:
        s = serial.Serial()
        s.port = port
        s.baudrate = 115200
        s.timeout = 0.2
        s.dtr = False
        s.rts = False
        s.open()
    except Exception as e:                                    # noqa: BLE001
        print(f"[{tag}] 打开 {port} 失败: {e}", flush=True)
        return

    buf = b""
    while time.time() < stop:
        try:
            d = s.read(2048)
        except Exception:                                     # noqa: BLE001
            break
        if not d:
            continue
        buf += d
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            txt = line.decode("utf-8", "replace").rstrip("\r")
            if txt.strip():
                print(f"[{tag}] {txt}", flush=True)
    s.close()


ths = [threading.Thread(target=worker, args=(t, p), daemon=True) for t, p in PORTS]
for t in ths:
    t.start()
for t in ths:
    t.join(timeout=SECS + 3)
print("=== done ===", flush=True)
