"""云端下发重启 → 抓 boot → 统计 log_ack 重复指纹。

用途：ACL 改完后，设备的订阅是"连接时"发起的；改 ACL 前被拒的订阅不会自动补建，
      必须让设备重连一次才会重新 SUBSCRIBE。本脚本一次完成"重启 + 观察 + 判据"。

判据（双发期 DUAL=1）：
  同一 seq 到 2 次 => 两个 topic 都送达 => ACL 生效
  同一 seq 到 1 次 => 只有旧 /down 在送 => 新 topic 仍被拒

★ 用 Write 工具写入（shell heredoc 会把正则里的 \\d 变成 /d）。
"""

import json
import re
import sys
import time
import urllib.request
from collections import Counter

import serial

sys.path.insert(0, r'D:/Guo_Feeder_Project/Cloudflare_Assets/scripts')
import importlib.util

_spec = importlib.util.spec_from_file_location(
    'g4', r'D:/Guo_Feeder_Project/Cloudflare_Assets/scripts/g4_remote_collect.py')
m = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(m)

PORT = 'COM8'
SECS = int(sys.argv[1]) if len(sys.argv) > 1 else 100
OUT = 'docs/evidence/LogAckSplit-2026-10-23/reboot-probe.log'


def send_cloud_reboot():
    cid = 'rb%d' % (int(time.time()) % 1000000)
    body = {"device_id": m.DEFAULT_DEVICE, "c": "system", "i": cid,
            "p": {"o": "reboot"}, "source": "cloud_api"}
    req = urllib.request.Request(
        m.WORKER_URL, data=json.dumps(body).encode(),
        headers={"Content-Type": "application/json",
                 "User-Agent": m.BROWSER_UA, "Accept": "*/*"},
        method="POST")
    try:
        with m._http_opener().open(req, timeout=25) as r:
            print('>>> reboot sent HTTP %s  cid=%s' % (r.status, cid))
    except Exception as e:
        print('>>> reboot send err %s %s' % (type(e).__name__, str(e)[:120]))
    return cid


def main():
    s = serial.Serial()
    s.port = PORT
    s.baudrate = 115200
    s.timeout = 0.2
    s.dtr = False
    s.rts = False
    s.open()                 # 不复位：靠云端 reboot 命令触发重启
    time.sleep(0.3)
    s.read(8192)

    send_cloud_reboot()

    buf = b''
    t0 = time.time()
    while time.time() - t0 < SECS:
        d = s.read(8192)
        if d:
            buf += d
    s.close()
    with open(OUT, 'wb') as f:
        f.write(buf)

    text = buf.decode('utf-8', errors='replace')
    print('>>> captured %d bytes / %ds -> %s' % (len(buf), SECS, OUT))
    print()

    print('=== 订阅与连接关键行 ===')
    for ln in text.splitlines():
        if any(k in ln for k in ('MQTT subscribe', 'MQTT log-ack', 'MQTT connected',
                                 'boot validated', 'loaded ', 'PANIC', 'Guru')):
            print('  ' + ln.strip()[:150])
    print()

    frames = re.findall(r'"c"\s*:\s*"log_ack".*?"i"\s*:\s*"ack:(\d+):(\d+)"', text)
    cnt = Counter(seq for _b, seq in frames)
    dups = {k: v for k, v in cnt.items() if v > 1}
    ack_ok = len(re.findall(r'\[Log Cloud\] ack ok', text))
    ack_to = len(re.findall(r'ack timeout retry', text))

    print('=== 指标 ===')
    print('  ack_ok            = %d' % ack_ok)
    print('  ack_timeout       = %d' % ack_to)
    print('  ack frames        = %d' % len(frames))
    print('  unique seq        = %d' % len(cnt))
    print('  duplicated seq    = %d' % len(dups))
    print('  dup detail        = %s' % (dict(sorted(dups.items())[:6]) if dups else 'NONE'))
    print()

    if not cnt:
        print('judgement: 无 ack 帧 —— 观察窗内未产生日志，或 boot 未完成')
        return 2
    if dups:
        print('judgement: **ACL 已生效** —— 同一 seq 到 2 次，两个 topic 同时送达')
        return 0
    print('judgement: 只有一条链路在送 —— 新 topic 仍未被授权')
    return 1


if __name__ == '__main__':
    sys.exit(main())
