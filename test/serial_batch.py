# -*- coding: utf-8 -*-
"""通用串口批量测试：只复位一次 → 保留 boot 日志 → 顺序发命令。

用法:
    python serial_batch.py <port> <logfile> <cmds_file> [quiet_sec]

cmds_file：每行一条命令；空行与 # 开头行忽略。
每行可写成 "命令 ||| 期望子串"（可选），命中则记 [EXPECT-OK]。
quiet_sec：**未命中**时期望静默的秒数（默认 2.5）。
           用于等待同步阻塞型命令（如 logt flush 落盘 62 条约需 2.4s）。
命中期望串后只再等 HIT_GRACE 秒即返回 —— 大量即时
（flash/stats/emit）命令因此从 ~3.1s 降到 ~0.5s。
"""
import serial
import sys
import time
import datetime

PORT = sys.argv[1]
LOG = sys.argv[2]
CMDS = sys.argv[3]
QUIET = float(sys.argv[4]) if len(sys.argv) > 4 else 2.5
HIT_GRACE = float(sys.argv[5]) if len(sys.argv) > 5 else 0.35

logf = open(LOG, "w", encoding="utf-8", errors="replace")


def emit(s):
    logf.write(s + "\n")
    logf.flush()


def ts():
    return datetime.datetime.now().strftime("%H:%M:%S.%f")[:-3]


cmds = []
with open(CMDS, "r", encoding="utf-8") as f:
    for ln in f:
        ln = ln.strip()
        if not ln or ln.startswith("#"):
            continue
        if "|||" in ln:
            c, exp = ln.split("|||", 1)
            cmds.append((c.strip(), exp.strip()))
        else:
            cmds.append((ln, None))

ser = serial.Serial(PORT, 115200, timeout=0.3)
ser.setDTR(False)
ser.rts = False

# ---- 采集 boot 日志直到连续 1.5s 静默 ----
emit("===== BOOT =====")
last = time.time()
buf = b""
while True:
    d = ser.read(256)
    if d:
        buf += d
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            line = line.replace(b"\r", b"")
            if line:
                emit("[%s] %s" % (ts(), line.decode("utf-8", "replace")))
        last = time.time()
    elif time.time() - last >= 1.5:
        break

for cmd, expect in cmds:
    emit("===== CMD: %s =====" % cmd)
    ser.reset_input_buffer()
    ser.write((cmd + "\n").encode("utf-8"))
    time.sleep(0.15)
    buf = b""
    last = time.time()
    deadline = time.time() + 20.0
    hit = False
    while time.time() < deadline:
        d = ser.read(256)
        if d:
            buf += d
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                line = line.replace(b"\r", b"")
                if not line:
                    continue
                s = line.decode("utf-8", "replace")
                emit("[%s] %s" % (ts(), s))
                if expect and expect in s:
                    hit = True
            last = time.time()
        else:
            # 命中期望串后响应已完整，只需短暂静默即可进入下一条
            # （部分命令如 logt flush 会同步阻塞数秒，必须等它真正返回）；
            # 未命中时仍按 QUIET 等待，避免把慢命令误判成 MISS。
            grace = HIT_GRACE if hit else QUIET
            if time.time() - last >= grace:
                break
    if expect:
        emit("[%s] >>> EXPECT %r : %s" % (ts(), expect, "OK" if hit else "MISS"))
    time.sleep(0.2)

emit("===== END =====")
ser.close()
logf.close()
print("done")
