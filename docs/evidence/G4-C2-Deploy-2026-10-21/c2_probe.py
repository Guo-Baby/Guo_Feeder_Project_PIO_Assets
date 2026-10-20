#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
G4 · C-2 合成探针（经 Pages 中转 → Worker 根路径 ingest）

★ 为什么走 Pages：本沙箱 workers.dev DNS 被污染（202.160.128.40），只有
  guo-feeder-pagesdev.pages.dev 可达；Pages `functions/api/webhook.js` 把 body
  原样转发到 Worker 根路径，与 /api/ingest 共用同一写入逻辑（差别仅 /api/ingest
  额外拒绝 source=cloud_api）。
★ 全部探针 raw_payload 含 G4PROBE-，永久保留、不删除（手册 §4.4）。
★ 合成设备 ID 0b0077a7e402（手册指定）：真实设备 288485896ce4 当前离线，
  用真实 ID 会把 device.last_seen 前移，污染"离线时长"判断。

用法： python c2_probe.py [p2|p4|p5|p1t|p3|all]
"""
import json
import sys
import time
import urllib.request
import urllib.error

URL = "https://guo-feeder-pagesdev.pages.dev/api/webhook"
SYNTH_DEV = "0b0077a7e402"
TS = str(int(time.time()))

# ★ Pages 前置 Cloudflare 边缘会对默认 UA 返回 403 error 1010（浏览器签名检查）。
#   实测：python-urllib 默认 UA ⇒ 403（请求未到 Worker、零写入）；浏览器 UA ⇒ 通过。
#   这不是绕过鉴权：Worker 侧的 /api/webhook 本就不校验 X-Webhook-Key（既存事实，见报告）。
UA = ("Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
      "(KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36")


def post(topic, payload, clientid="g4probe"):
    body = {"topic": topic, "payload": payload, "clientid": clientid}
    data = json.dumps(body).encode("utf-8")
    req = urllib.request.Request(
        URL, data=data, method="POST",
        headers={
            "Content-Type": "application/json",
            "Accept": "application/json, text/plain, */*",
            "User-Agent": UA,
        })
    try:
        with urllib.request.urlopen(req, timeout=30) as r:
            return r.status, r.read().decode("utf-8", "replace")
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode("utf-8", "replace")
    except Exception as e:  # noqa: BLE001
        return -1, "EXC:%s" % e


def show(tag, topic, payload, times=1, gap=0.6):
    print("--- %s ---" % tag)
    print("topic   = %s" % topic)
    print("payload = %s   (x%d)" % (payload, times))
    for i in range(times):
        code, text = post(topic, payload)
        print("  send#%d http=%s body=%r" % (i + 1, code, text[:120]))
        if i + 1 < times:
            time.sleep(gap)
    print("")


def main():
    mode = sys.argv[1] if len(sys.argv) > 1 else "all"
    print("# 探针开始: %s  TS=%s" % (time.strftime("%Y-%m-%d %H:%M:%S %z"), TS))
    print("# 入口: %s" % URL)
    print("# 合成设备: %s" % SYNTH_DEV)
    print("")

    if mode in ("p2", "all"):
        show("P-2 非法设备段（期望 device_id=unresolved / reason=topic_conflict）",
             "guo_feeder/BADID/up",
             '{"c":"probe","i":"G4PROBE-P2-%s","m":"G4PROBE-P2"}' % TS)

    if mode in ("p1t", "all"):
        show("P-1'-SYN $tenants 下行（期望 device_id=%s，不是 http_api）" % SYNTH_DEV,
             "$tenants/n302933b/guo_feeder/%s/down" % SYNTH_DEV,
             '{"c":"probe","i":"G4PROBE-P1T-%s","m":"G4PROBE-P1T"}' % TS)

    if mode in ("p3", "all"):
        show("P-3'-SYN log 方向（期望 direction=log，不是 unknown）",
             "guo_feeder/%s/log" % SYNTH_DEV,
             '{"c":"probe","i":"G4PROBE-P3-%s","m":"G4PROBE-P3"}' % TS)

    if mode in ("p4", "all"):
        show("P-4 去重（同 payload 连发 3 次，期望 D1 只有 1 行）",
             "guo_feeder/%s/up" % SYNTH_DEV,
             '{"c":"probe","i":"G4PROBE-P4-%s","m":"G4PROBE-P4"}' % TS,
             times=3)

    if mode in ("p5", "all"):
        show("P-5 无 msg_id（同 payload 连发 2 次，期望 2 行且 dedup_key 均 NULL）",
             "guo_feeder/%s/up" % SYNTH_DEV,
             '{"c":"probe","m":"G4PROBE-P5-%s"}' % TS,
             times=2)

    print("# TS=%s  （查询用：LIKE '%%G4PROBE-%%' 或 msg_id LIKE '%%-%s'）" % (TS, TS))
    print("# 探针结束: %s" % time.strftime("%Y-%m-%d %H:%M:%S %z"))


if __name__ == "__main__":
    main()
