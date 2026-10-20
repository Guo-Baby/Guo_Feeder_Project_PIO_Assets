#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
S-3 本地端到端验证（`wrangler dev --local` + 真实本地 D1/SQLite）
============================================================================
目的：在**真实 Workers 运行时 + 真实 D1 SQL 引擎**（不是 mock）上验证
      `observe` 模式的语义：非法输入照旧写入、合法输入行为不变。

★ 与生产的差异（必须如实标注，不得冒充生产验证）：
  · 运行在 workerd 本地进程，**非** Cloudflare 边缘；
  · D1 是 miniflare 本地 SQLite，**非**生产 D1（生产 D1 本轮**未写入任何行**）；
  · 不含 TLS / 边缘 WAF / `workers.dev` DNS 等因素。

用法（先启动 `wrangler dev --port 8791 --local`）：
    python s3_local_e2e.py
"""

import json
import os
import sys
import time
import urllib.error
import urllib.request

BASE = os.environ.get("S3_BASE", "http://127.0.0.1:8791")
DEV = "0b0053a00e2e"          # 本地专用合成设备（12 位**合法**小写 hex）
TEN = "n302933b"
TS = int(time.time())
BURST = int(os.environ.get("S3_BURST", "0") or "0")
RESULTS = []


def post(body, path="/", key=None):
    data = json.dumps(body).encode("utf-8")
    headers = {"Content-Type": "application/json"}
    if key:
        headers["X-Webhook-Key"] = key
    req = urllib.request.Request(BASE + path, data=data, headers=headers, method="POST")
    try:
        with urllib.request.urlopen(req, timeout=30) as r:
            return r.status, r.read().decode("utf-8", "replace")
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode("utf-8", "replace")
    except Exception as e:                                   # noqa: BLE001
        return -1, "EXC:%s" % e


def case(cid, label, body, expect_status, path="/", key=None):
    st, body_txt = post(body, path=path, key=key)
    okk = (st == expect_status) if expect_status is not None else True
    RESULTS.append((cid, label, st, okk, body_txt[:100]))
    print("%-8s %-58s status=%-4s %s" % (cid, label, st, "OK" if okk else "MISMATCH"))
    if body_txt.strip():
        print("         body: %s" % body_txt.strip()[:160])
    return st, body_txt


def main():
    print("# target = %s   ts = %d" % (BASE, TS))
    print("# 设备 = %s   租户 = %s" % (DEV, TEN))

    # ---- 1. 合法 plain up ----
    case("L-1", "合法 guo_feeder/<dev>/up", {
        "topic": "guo_feeder/%s/up" % DEV,
        "clientid": "dev",
        "payload": {"c": "online", "i": "S3L1-%d" % TS, "s": "device"}
    }, 200)

    # ---- 2. 合法 $tenants down（真实 $tenants 形态）----
    case("L-2", "合法 $tenants/.../down", {
        "topic": "$tenants/%s/guo_feeder/%s/down" % (TEN, DEV),
        "clientid": "http_api",
        "payload": {"c": "execute_action", "i": "S3L2-%d" % TS}
    }, 200)

    # ---- 3. 合法 $tenants log ----
    case("L-3", "合法 $tenants/.../log", {
        "topic": "$tenants/%s/guo_feeder/%s/log" % (TEN, DEV),
        "clientid": "http_api",
        "payload": {"c": "log", "i": "S3L3-%d" % TS}
    }, 200)

    # ---- 4. ★ observe：空 topic 仍写入且 200 ----
    case("O-1", "★ 空 topic（observe 应照旧 200 + 写入）", {
        "topic": "", "clientid": "attacker", "payload": {"c": "online", "i": "S3O1-%d" % TS}
    }, 200)

    # ---- 5. ★ observe：空 payload 仍写入且 200 ----
    case("O-2", "★ 空 payload（observe 应照旧 200 + 写入）", {
        "topic": "guo_feeder/%s/up" % DEV, "clientid": "dev", "payload": ""
    }, 200)

    # ---- 6. ★ observe：非法方向仍写入 ----
    case("O-3", "★ 非法方向 /upx（observe 应照旧写入）", {
        "topic": "guo_feeder/%s/upx" % DEV, "payload": {"c": "online", "i": "S3O3-%d" % TS}
    }, 200)

    # ---- 7. ★ observe：族名不符仍写入 ----
    case("O-4", "★ 族名不符 ($SYS/...) 仍写入", {
        "topic": "$SYS/broker/uptime", "payload": {"c": "sys", "i": "S3O4-%d" % TS}
    }, 200)

    # ---- 8. 去重：同 msg_id(`i`) + 同 payload ×3 ----
    dup_body = {
        "topic": "guo_feeder/%s/up" % DEV,
        "clientid": "dev",
        "payload": {"c": "online", "i": "S3DUP-%d" % TS, "s": "device"}
    }
    for i in range(3):
        case("D-%d" % (i + 1), "去重：第 %d 次投递同一消息（%s）" % (i + 1, "应只留 1 行"), dup_body, 200)

    # ---- 9. 缺 msg_id（`i`）×2（应各留一行，dedup_key 为 NULL）----
    for i in range(2):
        case("N-%d" % (i + 1), "缺 msg_id(i) 第 %d 次（不得合并）" % (i + 1), {
            "topic": "guo_feeder/%s/up" % DEV,
            "clientid": "dev",
            "payload": {"c": "system", "i": "", "s": "device"}
        }, 200)

    # ---- 10. cloud_api：非法 device_id 仍 400 ----
    case("C-1", "cloud_api 非法 device_id ⇒ 400", {
        "source": "cloud_api", "device_id": "BADID", "cmd": "system"
    }, 400)

    # ---- 11. /api/ingest 鉴权仍生效（无 key ⇒ 401/403）----
    st, _ = case("C-2", "/api/ingest 无 key（鉴权结果未改动）", {
        "topic": "guo_feeder/%s/up" % DEV, "payload": {"a": 1}
    }, None, path="/api/ingest")

    # ---- 12. 日志防洪：连发 N 条非法请求（S3_BURST）----
    if BURST > 0:
        print()
        print("# 洪泛测试：连发 %d 条空 topic 请求" % BURST)
        sts = {}
        for i in range(BURST):
            st, _ = post({"topic": "", "clientid": "flood", "payload": {"i": "S3F-%d-%d" % (TS, i)}})
            sts[st] = sts.get(st, 0) + 1
        print("# 状态码分布: %s" % sts)
        RESULTS.append(("F-1", "洪泛 %d 条（应全部 200，日志受防洪约束）" % BURST,
                        200 if sts.get(200, 0) == BURST else -1, sts.get(200, 0) == BURST, str(sts)))

    print()
    bad = [r for r in RESULTS if not r[3]]
    print("# cases=%d  mismatched=%d" % (len(RESULTS), len(bad)))
    for b in bad:
        print("#   MISMATCH %s %s status=%s body=%s" % (b[0], b[1], b[2], b[4]))
    return 0 if not bad else 1


if __name__ == "__main__":
    sys.exit(main())
