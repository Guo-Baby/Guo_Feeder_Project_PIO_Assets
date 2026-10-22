"""LOG-ACK-SPLIT 指纹判据：统计同一 seq 的 log_ack 到达次数。

判据（双发期 LOG_ACK_DUAL_PUBLISH="1"）：
  同一 seq 到达 2 次  => 旧 /down 与新 /log/ack 同时送达 => ACL 已生效
  同一 seq 到达 1 次  => 只有一条链路在送        => 新 topic 未授权

注意：本脚本只读串口，**不复位**，不做任何破坏性操作。
★ 用 Write 工具写入，不要用 shell heredoc —— Git Bash 会把正则里的 \d 变成 /d。
"""

import re
import sys
import time
from collections import Counter

import serial

PORT = 'COM8'
SECS = int(sys.argv[1]) if len(sys.argv) > 1 else 45
OUT = 'docs/evidence/LogAckSplit-2026-10-23/probe-dual.log'


def capture():
    s = serial.Serial()
    s.port = PORT
    s.baudrate = 115200
    s.timeout = 0.2
    s.dtr = False
    s.rts = False
    s.open()          # 不复位：只观察
    buf = b''
    t0 = time.time()
    while time.time() - t0 < SECS:
        d = s.read(8192)
        if d:
            buf += d
    s.close()
    with open(OUT, 'wb') as f:
        f.write(buf)
    return buf


def main():
    buf = capture()
    text = buf.decode('utf-8', errors='replace')

    # 形如 {"c":"log_ack","i":"ack:68:18758","p":{...}}
    frames = re.findall(r'"c"\s*:\s*"log_ack".*?"i"\s*:\s*"ack:(\d+):(\d+)"', text)
    seqs = [seq for _boot, seq in frames]
    cnt = Counter(seqs)
    dups = {k: v for k, v in cnt.items() if v > 1}

    ack_ok = len(re.findall(r'\[Log Cloud\] ack ok', text))
    ack_to = len(re.findall(r'ack timeout retry', text))

    print('captured bytes      = %d / %ds' % (len(buf), SECS))
    print('ack_ok lines        = %d' % ack_ok)
    print('ack_timeout lines   = %d' % ack_to)
    print('ack frames          = %d' % len(seqs))
    print('unique seq          = %d' % len(cnt))
    print('duplicated seq      = %d' % len(dups))
    print('dup detail          = %s' % (dups if dups else 'NONE'))
    if cnt:
        print('seq range           = %s .. %s' % (min(cnt), max(cnt)))
    print()

    if not cnt:
        print('judgement: 无 ack 帧 —— 设备可能无新日志产生，或串口无输出')
        return 2
    if len(dups) >= max(2, len(cnt) // 3):
        print('judgement: **ACL 已生效** —— 每条 seq 到 2 次，两个 topic 同时送达')
        return 0
    print('judgement: 只有一条链路在送 —— 新 topic 仍未被授权')
    return 1


if __name__ == '__main__':
    sys.exit(main())
