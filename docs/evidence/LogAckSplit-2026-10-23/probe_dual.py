import serial, time, re
from collections import Counter

s = serial.Serial()
s.port = 'COM8'; s.baudrate = 115200; s.timeout = 0.2
s.dtr = False; s.rts = False
s.open()
buf = b''
t0 = time.time()
while time.time() - t0 < 40:
    d = s.read(8192)
    if d: buf += d
s.close()
open('docs/evidence/LogAckSplit-2026-10-23/probe-dual.log', 'wb').write(buf)
t = buf.decode('utf-8', errors='replace')
seqs = re.findall(r'"c":"log_ack","i":"ack:/d+:(/d+)"', t)
c = Counter(seqs)
dup = {k: v for k, v in c.items() if v > 1}
print('RX log_ack frames :', len(seqs))
print('unique seq        :', len(c))
print('duplicated seq    :', len(dup))
print('samples           :', sorted(dup.items(), key=lambda x: -x[1])[:6])
print()
print('judgement: 2 次 = 两个 topic 都通(ACL 已生效) / 1 次 = 只有一条通')
