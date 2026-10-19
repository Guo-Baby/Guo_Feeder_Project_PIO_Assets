"""BT-1 验收：COM8 只读串口捕获（不发送、不复位）。

要点（项目铁律）：
  · 打开前先 dtr=False / rts=False，避免 CH343 上的 DTR 跳变触发 MCU 复位；
  · 只读，不写串口；
  · 每 10 s 打印一次 ### alive 心跳，便于判断进程是否存活（不要用第二个进程探测占用）。
"""
import serial
import sys
import time
import datetime

PORT = sys.argv[1] if len(sys.argv) > 1 else "COM8"
DUR = float(sys.argv[2]) if len(sys.argv) > 2 else 180.0
OUT = sys.argv[3] if len(sys.argv) > 3 else "bt1_serial.log"

s = serial.Serial()
s.port = PORT
s.baudrate = 115200
s.bytesize = 8
s.parity = "N"
s.stopbits = 1
s.timeout = 0.3
s.dtr = False
s.rts = False
s.open()

t0 = time.time()
last_hb = 0.0
nbytes = 0
with open(OUT, "wb") as f:
    f.write(("# open %s at %s dtr=%s rts=%s\n" % (
        PORT,
        datetime.datetime.utcnow().strftime("%Y-%m-%d %H:%M:%S UTC"),
        s.dtr, s.rts)).encode())
    while time.time() - t0 < DUR:
        chunk = s.read(4096)
        if chunk:
            f.write(chunk)
            f.flush()
            nbytes += len(chunk)
        now = time.time() - t0
        if now - last_hb >= 10.0:
            last_hb = now
            print("### alive t=%.0fs bytes=%d" % (now, nbytes), flush=True)
    f.write(("\n# closed at %s total=%d\n" % (
        datetime.datetime.utcnow().strftime("%Y-%m-%d %H:%M:%S UTC"), nbytes)).encode())

s.close()
print("### done total_bytes=%d" % nbytes, flush=True)
