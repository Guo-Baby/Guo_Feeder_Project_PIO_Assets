import serial, time, sys, re

PORT, SECS, OUT = 'COM8', 45, 'docs/evidence/LogAckSplit-2026-10-23/ack-after-worker-deploy.txt'

s = serial.Serial()
s.port = PORT; s.baudrate = 115200; s.timeout = 0.2
s.dtr = False; s.rts = False
s.open()          # ★ 不复位：只观察，不打断运行

buf = b''
t0 = time.time()
while time.time() - t0 < SECS:
    d = s.read(8192)
    if d: buf += d
s.close()
open(OUT, 'wb').write(buf)
txt = buf.decode('utf-8', errors='replace')

print('>>> captured %d bytes / %d s -> %s' % (len(buf), SECS, OUT))
print()
ack_ok   = len(re.findall(r'\[Log Cloud\] ack ok', txt))
timeout_ = len(re.findall(r'ack timeout retry', txt))
giveup   = len(re.findall(r'giveup|gave up', txt))
print('=== 指标 ===')
print('  ack ok        : %d' % ack_ok)
print('  ack timeout   : %d' % timeout_)
print('  giveup        : %d' % giveup)
print()
print('=== 尾部 12 行 ===')
for ln in txt.splitlines()[-12:]:
    print('  ' + ln.strip()[:160])
