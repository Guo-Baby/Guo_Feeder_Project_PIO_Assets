import serial, time, json, urllib.request, sys, os, re

sys.path.insert(0, r'D:/Guo_Feeder_Project/Cloudflare_Assets/scripts')
import importlib.util
spec = importlib.util.spec_from_file_location('g4', r'D:/Guo_Feeder_Project/Cloudflare_Assets/scripts/g4_remote_collect.py')
m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m)

OUT = 'docs/evidence/LogAckSplit-2026-10-23/reboot-ack.log'
SECS = int(os.environ.get('SECS', '80'))

s = serial.Serial()
s.port = 'COM8'; s.baudrate = 115200; s.timeout = 0.2
s.dtr = False; s.rts = False
s.open()
time.sleep(0.3)
s.read(8192)

# 云端下发重启（不是串口复位 ⇒ 一次性、干净）
cid = 'rb%d' % (int(time.time()) % 1000000)
body = {"device_id": m.DEFAULT_DEVICE, "c": "system", "i": cid,
        "p": {"o": "reboot"}, "source": "cloud_api"}
req = urllib.request.Request(m.WORKER_URL, data=json.dumps(body).encode(),
    headers={"Content-Type":"application/json","User-Agent":m.BROWSER_UA,"Accept":"*/*"}, method="POST")
try:
    with m._http_opener().open(req, timeout=25) as r:
        print('>>> reboot sent HTTP', r.status, 'cid=%s' % cid)
except Exception as e:
    print('>>> reboot send err', type(e).__name__, str(e)[:100])

buf = b''
t0 = time.time()
while time.time() - t0 < SECS:
    d = s.read(8192)
    if d: buf += d
s.close()
open(OUT, 'wb').write(buf)
txt = buf.decode('utf-8', errors='replace')

print('>>> captured %d bytes / %ds' % (len(buf), SECS))
print()
for ln in txt.splitlines():
    if any(k in ln for k in ('MQTT subscribe', 'MQTT subscribed', 'MQTT connected',
                             'boot validated', 'loaded', '[Log Cloud] ack ok',
                             'ack timeout', 'PANIC', 'Guru', 'log-ack')):
        print('  ' + ln.strip()[:160])
print()
print('ack ok =', len(re.findall(r'\[Log Cloud\] ack ok', txt)),
      '| timeout =', len(re.findall(r'ack timeout retry', txt)))
