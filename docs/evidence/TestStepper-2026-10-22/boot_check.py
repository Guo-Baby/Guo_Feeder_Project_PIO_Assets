import serial, time, sys, re

PORT = sys.argv[1] if len(sys.argv) > 1 else 'COM8'
SECS = int(sys.argv[2]) if len(sys.argv) > 2 else 55
OUT  = sys.argv[3] if len(sys.argv) > 3 else 'docs/evidence/TestStepper-2026-10-22/boot-log.txt'

s = serial.Serial()
s.port = PORT
s.baudrate = 115200
s.timeout = 0.2
s.dtr = False
s.rts = False
s.open()
time.sleep(0.3)

# 排空打开端口可能触发的那次 boot（不作为判据）
t0 = time.time()
while time.time() - t0 < 2.0:
    s.read(4096)

# ★ 单次复位（只一次！避免 CFG-ROLLBACK：连续两次复位且首次未 validate 会回滚配置）
s.setRTS(True)
time.sleep(0.15)
s.setRTS(False)
print('>>> RESET-PULSED (single) %s' % time.strftime('%H:%M:%S'))

buf = b''
t0 = time.time()
while time.time() - t0 < SECS:
    d = s.read(4096)
    if d:
        buf += d
s.close()

open(OUT, 'wb').write(buf)
txt = buf.decode('utf-8', errors='replace')
lines = txt.splitlines()
print('>>> captured %d bytes, %d lines -> %s' % (len(buf), len(lines), OUT))
print()

keys = ('[TStep]', 'boot validated', 'Boot', 'PANIC', 'panic', 'abort', 'WDT',
        'Guru', 'Stack canary', 'loaded', 'Init:', 'WiFi', 'MQTT', '[CFG]', 'test_stepper')
print('=== 关键行 ===')
shown = 0
for ln in lines:
    if any(k in ln for k in keys):
        print('  ' + ln.strip()[:160])
        shown += 1
        if shown > 60:
            break
print()
print('=== 断言 ===')
print('  panic/abort/WDT :', 'FAIL' if re.search(r'PANIC|Guru Meditation|abort\(\)|Stack canary|WDT', txt) else 'OK (none)')
print('  boot validated  :', 'OK' if 'boot validated' in txt else 'MISS')
print('  [TStep] Init    :', 'OK' if '[TStep] Init' in txt else 'MISS')
print('  WiFi connected  :', 'OK' if re.search(r'WiFi.*(connected|CONNECTED|got ip)', txt, re.I) else 'MISS')
print('  MQTT connected  :', 'OK' if re.search(r'MQTT.*(connected|CONNECTED)', txt, re.I) else 'MISS')
