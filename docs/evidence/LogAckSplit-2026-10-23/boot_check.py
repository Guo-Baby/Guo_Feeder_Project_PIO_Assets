import serial, time, sys, re

PORT = 'COM8'
OUT  = 'docs/evidence/LogAckSplit-2026-10-23/boot-log.txt'

s = serial.Serial()
s.port = PORT; s.baudrate = 115200; s.timeout = 0.2
s.dtr = False; s.rts = False
s.open()
time.sleep(0.3)

# 排空打开端口可能触发的 boot（不作判据）
t0 = time.time()
while time.time() - t0 < 2.0:
    s.read(8192)

# ★ 单次复位（只一次，避免 CFG-ROLLBACK）
s.setRTS(True); time.sleep(0.15); s.setRTS(False)
print('>>> RESET (single) %s' % time.strftime('%H:%M:%S'))

buf = b''
t0 = time.time()
while time.time() - t0 < 60:
    d = s.read(8192)
    if d: buf += d
s.close()

open(OUT, 'wb').write(buf)
txt = buf.decode('utf-8', errors='replace')
print('>>> captured %d bytes -> %s' % (len(buf), OUT))
print()

keys = ('MQTT log-ack', 'MQTT subscribe', 'MQTT subscribed', 'MQTT connected',
        'boot validated', 'loaded', '[Log Cloud]', 'PANIC', 'Guru')
print('=== 关键行 ===')
for ln in txt.splitlines():
    if any(k in ln for k in keys):
        print('  ' + ln.strip()[:170])

print()
print('=== 断言 ===')
print('  ack topic 已渲染 :', 'OK' if re.search(r'MQTT log-ack=guo_feeder/\w+/log/ack', txt) else 'MISS')
print('  ack 订阅已发起   :', 'OK' if 'subscribe(log-ack)' in txt else 'MISS')
print('  boot validated   :', 'OK' if 'boot validated' in txt else 'MISS')
print('  panic            :', 'FAIL' if re.search(r'PANIC|Guru Meditation|Stack canary', txt) else 'OK (none)')
