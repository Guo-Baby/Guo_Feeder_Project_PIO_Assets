"""log_history 解码器（可复用）

背景：`log_history.payload` 是 **base64 的 CBOR**，`message`/`code` 常为 null，
`created_at` 是**秒**（`mqtt_messages` 是毫秒）。直接 SELECT 看不出内容。

用法：
  python decode_log_history.py [分钟数] [过滤关键字]
  python decode_log_history.py 60                 # 最近 60 分钟全部
  python decode_log_history.py 60 WEIGHT VALVE    # 只显示事件名含关键字的

★ 用 Write 工具维护本文件，不要用 shell heredoc 重写 —— Git Bash 会把正则里的
  \\d 变成 /d，导致匹配恒为 0。
"""

import base64
import importlib.util
import re
import struct
import sys
import time
import datetime

FW = r'D:/Guo_Feeder_Project/PIO_Assets/Guo_Feeder_Project'
COLLECTOR = r'D:/Guo_Feeder_Project/Cloudflare_Assets/scripts/g4_remote_collect.py'

LEVEL = {0: 'DEBUG', 1: 'INFO', 2: 'WARN', 3: 'ERROR', 4: 'CRITICAL'}
# LogParamType: 1=I32 2=U32 3=F32 4=BOOL 5=ENUM 6=STR 7=I8 8=U16 0=U8
TYPE_FMT = {0: '<B', 1: '<i', 2: '<I', 3: '<f', 4: '<I', 5: '<I', 6: '<I',
            7: '<b', 8: '<H'}


def load_maps():
    src = open(FW + '/src/log/log_events.h', encoding='utf-8', errors='replace').read()
    ev = {int(h, 16): n for n, h in
          re.findall(r'(LOG_[A-Z0-9_]+)\s*=\s*0x([0-9A-Fa-f]{4})', src)}
    pm = {int(h, 16): n for n, h in
          re.findall(r'(LOG_P_[A-Z0-9_]+)\s*=\s*0x([0-9A-Fa-f]{2})', src)}
    return ev, pm


def cbor(b, i=0):
    ib = b[i]
    mt = ib >> 5
    ai = ib & 0x1f
    i += 1
    if ai < 24:
        v = ai
    elif ai == 24:
        v = b[i]; i += 1
    elif ai == 25:
        v = struct.unpack('>H', b[i:i + 2])[0]; i += 2
    elif ai == 26:
        v = struct.unpack('>I', b[i:i + 4])[0]; i += 4
    elif ai == 27:
        v = struct.unpack('>Q', b[i:i + 8])[0]; i += 8
    else:
        raise ValueError('ai %d' % ai)
    if mt == 0:
        return v, i
    if mt == 1:
        return -1 - v, i
    if mt == 2:
        return b[i:i + v], i + v
    if mt == 3:
        return b[i:i + v].decode('utf-8', 'replace'), i + v
    if mt == 4:
        out = []
        for _ in range(v):
            x, i = cbor(b, i)
            out.append(x)
        return out, i
    if mt == 5:
        d = {}
        for _ in range(v):
            k, i = cbor(b, i)
            x, i = cbor(b, i)
            d[k] = x
        return d, i
    if mt == 7:
        return None, i
    raise ValueError('mt %d' % mt)


def decode(payload_b64, pm):
    env, _ = cbor(base64.b64decode(payload_b64))
    rec = env[11][0] if len(env) > 11 and isinstance(env[11], list) else env[-1][0]
    level = rec[1]
    pc = rec[3]
    seq = struct.unpack('<I', rec[4:8])[0]
    boot = struct.unpack('<I', rec[8:12])[0]
    eid = struct.unpack('<H', rec[12:14])[0]
    up = struct.unpack('<I', rec[16:20])[0]
    ts = struct.unpack('<I', rec[20:24])[0]
    kv = {}
    for k in range(pc):
        off = 28 + k * 6
        pid = rec[off]
        ptype = rec[off + 1]
        b4 = rec[off + 2:off + 6]
        fmt = TYPE_FMT.get(ptype, '<I')
        try:
            kv[pm.get(pid, 'p%d' % pid)] = struct.unpack(fmt, b4)[0]
        except struct.error:
            kv[pm.get(pid, 'p%d' % pid)] = int.from_bytes(b4, 'little')
    return dict(level=level, seq=seq, boot=boot, eid=eid, uptime=up, ts=ts, params=kv)


def main():
    mins = int(sys.argv[1]) if len(sys.argv) > 1 else 60
    filters = [a.upper() for a in sys.argv[2:]]

    spec = importlib.util.spec_from_file_location('g4', COLLECTOR)
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)

    ev, pm = load_maps()
    t0 = int(time.time() - mins * 60)
    rows = m.d1("SELECT ts, seq, boot_seq, level, payload, created_at FROM log_history "
                "WHERE created_at > " + str(t0) + " ORDER BY created_at, seq")
    print('窗口：最近 %d 分钟   记录数 %d' % (mins, len(rows or [])))
    print('%-9s %-7s %-5s %-9s %-30s %s' % ('时间', 'seq', 'boot', '级别', '事件', '参数'))
    print('-' * 118)
    shown = 0
    for x in (rows or []):
        try:
            d = decode(x['payload'], pm)
        except Exception as e:
            print('  seq=%s 解码失败 %s' % (x.get('seq'), e))
            continue
        name = ev.get(d['eid'], '0x%04X' % d['eid'])
        if filters and not any(f in name.upper() for f in filters):
            continue
        # ★ 记录内的 ts 在开机早期为 0（RET-1），此时回退用入库时间 created_at（秒）
        tm = '--:--:--'
        for cand in (d['ts'], x.get('created_at')):
            if cand and cand > 1e9:
                tm = datetime.datetime.fromtimestamp(cand).strftime('%H:%M:%S')
                break
        ps = ' '.join('%s=%s' % (k.replace('LOG_P_', ''), round(v, 2) if isinstance(v, float) else v)
                       for k, v in d['params'].items())
        print('%-9s %-7s %-5s %-9s %-30s %s' % (
            tm, d['seq'], d['boot'], LEVEL.get(d['level'], d['level']), name, ps))
        shown += 1
    print('-' * 118)
    print('显示 %d 条' % shown)


if __name__ == '__main__':
    main()
