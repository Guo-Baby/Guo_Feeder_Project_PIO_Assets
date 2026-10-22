import serial, time, sys, re

PORT = sys.argv[1] if len(sys.argv) > 1 else 'COM8'
OUT  = 'docs/evidence/TestStepper-2026-10-22/ledc-fix-log.txt'
RUN_SECS = 2          # 只通电 2 秒（仅验证 LEDC 能建立，不做动作测试）

s = serial.Serial()
s.port = PORT; s.baudrate = 115200; s.timeout = 0.2
s.dtr = False; s.rts = False
s.open()
time.sleep(0.3)

def drain(secs):
    buf = b''
    t0 = time.time()
    while time.time() - t0 < secs:
        d = s.read(4096)
        if d: buf += d
    return buf

drain(2.0)                       # 排空（打开端口可能触发的 boot，不作判据）

# ★ 单次复位（只一次，避免 CFG-ROLLBACK）
s.setRTS(True); time.sleep(0.15); s.setRTS(False)
print('>>> RESET (single) %s' % time.strftime('%H:%M:%S'))

boot = drain(25.0)

# 串口命令：走正式 Action 链路
s.write(b'tstep_start 100 0\r\n')
print('>>> sent: tstep_start 100 0')
run_log = drain(RUN_SECS)

s.write(b'tstep_stop\r\n')
print('>>> sent: tstep_stop')
stop_log = drain(4.0)

s.close()

all_bytes = boot + run_log + stop_log
open(OUT, 'wb').write(all_bytes)
txt_full = all_bytes.decode('utf-8', errors='replace')

print()
print('=== [TStep] 相关行 ===')
for ln in txt_full.splitlines():
    if '[TStep]' in ln or 'ledc' in ln.lower() or 'LEDC' in ln:
        print('  ' + ln.strip()[:150])

print()
print('=== 断言 ===')
ledc_err = bool(re.search(r'No more LEDC channels available|ledcSetup.*rejected|start failed', txt_full))
print('  [TStep] Init          :', 'OK' if '[TStep] Init' in txt_full else 'MISS')
print('  LEDC 报错             :', 'FAIL (still present)' if ledc_err else 'OK (none)')
print('  START 成功            :', 'OK' if re.search(r'\[TStep\] START:', txt_full) else 'MISS')
print('  STOP 成功             :', 'OK' if re.search(r'\[TStep\] STOP', txt_full) else 'MISS')
print('  boot validated        :', 'OK' if 'boot validated' in txt_full else 'MISS')
print('  panic                 :', 'FAIL' if re.search(r'PANIC|Guru Meditation|Stack canary', txt_full) else 'OK (none)')
