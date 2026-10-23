# -*- coding: utf-8 -*-
"""部署后验证（生产 Worker + Pages）—— 只读与非破坏性

原则
----
* 只发**只读**命令（`family=query`），且先订阅 /up 确认"没发出去"。
* 不打印任何密钥；密钥从 `Cloudflare_Assets/.secrets.local` 读取。
* 生产 D1 **没有** 0008 的三张表 ⇒ 涉及审计/会话的端点必须是 503（fail-closed），
  这本身就是一条要验的行为：**拒绝**而不是"发了但没记"。

用法
----
    python prod_postdeploy_check.py --device-id 288485896ce4
"""

import argparse
import json
import os
import ssl
import sys
import time
import urllib.error
import urllib.request

import paho.mqtt.client as mqtt

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SECRETS = os.path.normpath(os.path.join(ROOT, "..", "..", "Cloudflare_Assets", ".secrets.local"))
CA = os.path.join(ROOT, "data", "emqxsl-ca.crt")

WORKER = "https://guo-feeder-api.guobaby.workers.dev"
PAGES = "https://guo-feeder-pagesdev.pages.dev"
PROXY = "http://127.0.0.1:7890"
UA = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) Chrome/126.0"

BROKER = "n302933b.ala.cn-hangzhou.emqxsl.cn"
PORT = 8883
MQ_USER, MQ_PASS = "workbuddy", "workbuddy"

RESULTS = []


def rec(case, desc, ok, detail="", status=None):
    if status is None:
        status = "PASS" if ok else "FAIL"
    RESULTS.append({"case": case, "desc": desc, "status": status, "detail": str(detail)[:400]})
    print("  [%-8s] %-12s %s   %s" % (case, status, desc, str(detail)[:170]))


def load_secrets():
    out = {}
    with open(SECRETS, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#") or "=" not in line:
                continue
            k, v = line.split("=", 1)
            out[k.strip()] = v.strip().strip('"')
    return out


def opener(use_proxy):
    """★ 默认**不跟随 302**：urllib 会自动跟随，导致中间件的 302 被看成 200，
    把"未登录被重定向"误判成"页面可直达"。这里用自定义 handler 关掉。"""
    class NoRedirect(urllib.request.HTTPRedirectHandler):
        def redirect_request(self, req, fp, code, msg, headers, newurl):
            return None

    if use_proxy:
        return urllib.request.build_opener(
            urllib.request.ProxyHandler({"http": PROXY, "https": PROXY}), NoRedirect())
    return urllib.request.build_opener(urllib.request.ProxyHandler({}), NoRedirect())


def http(method, url, use_proxy, token=None, body=None, headers=None, timeout=30):
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(url, data=data, method=method)
    req.add_header("content-type", "application/json")
    req.add_header("user-agent", UA)
    if token:
        req.add_header("authorization", "Bearer " + token)
    for k, v in (headers or {}).items():
        req.add_header(k, v)
    op = opener(use_proxy)
    try:
        with op.open(req, timeout=timeout) as r:
            raw = r.read().decode("utf-8", "replace")
            try:
                return r.status, json.loads(raw or "{}"), dict(r.headers)
            except Exception:
                return r.status, {"_raw": raw[:300]}, dict(r.headers)
    except urllib.error.HTTPError as e:
        raw = e.read().decode("utf-8", "replace")
        try:
            return e.code, json.loads(raw or "{}"), dict(e.headers)
        except Exception:
            return e.code, {"_raw": raw[:300]}, dict(e.headers)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--device-id", required=True)
    ap.add_argument("--json", default=".pio/prod_check.json")
    args = ap.parse_args()

    sec = load_secrets()
    token = sec.get("ADMIN_API_TOKEN", "")
    if not token:
        print("[ABORT] 无 ADMIN_API_TOKEN")
        return 2

    print("\n===== P1 Worker 鉴权 =====")
    st, b, _ = http("GET", WORKER + "/api/ui/catalog", True)
    rec("P1-1", "无 Bearer ⇒ 401", st == 401, "http=%s" % st)
    st, b, _ = http("GET", WORKER + "/api/ui/catalog", True, token="wrong-token")
    rec("P1-2", "错 Bearer ⇒ 401", st == 401, "http=%s" % st)
    st, b, _ = http("GET", WORKER + "/api/ui/catalog", True, token=token)
    rec("P1-3", "正确 Bearer ⇒ 200 且含能力目录",
        st == 200 and len(b.get("queries") or []) == 22,
        "http=%s queries=%d actions=%d" % (st, len(b.get("queries") or []), len(b.get("actions") or [])))

    print("\n===== P2 会话端点（D1 无 0008 ⇒ 必须 503）=====")
    st, b, _ = http("POST", WORKER + "/api/ui/session", True, token=token,
                    body={"action": "issue", "ttl_seconds": 60})
    rec("P2-1", "issue ⇒ 503 session_store_unavailable（fail-closed，不降级放行）",
        st == 503 and b.get("error") == "session_store_unavailable",
        "http=%s error=%r hint=%r" % (st, b.get("error"), (b.get("hint") or "")[:40]))

    print("\n===== P3 命令端点（D1 无 0008 ⇒ 必须 503 且不下发）=====")
    cmd_id = "prd%08d" % (int(time.time()) % 100000000)
    frames = []
    sub = "guo_feeder/%s/up" % args.device_id

    def on_connect(c, u, f, rc, *a):
        c.subscribe(sub, qos=1)

    def on_message(c, u, msg):
        try:
            frames.append(json.loads(msg.payload.decode("utf-8", "replace")))
        except Exception:
            pass

    try:
        from paho.mqtt.client import CallbackAPIVersion
        cli = mqtt.Client(CallbackAPIVersion.VERSION2, client_id="wb_prd_%d" % int(time.time()))
    except Exception:
        cli = mqtt.Client(client_id="wb_prd_%d" % int(time.time()))
    cli.username_pw_set(MQ_USER, MQ_PASS)
    cli.tls_set(ca_certs=CA, cert_reqs=ssl.CERT_REQUIRED)
    cli.on_connect = on_connect
    cli.on_message = on_message
    cli.connect(BROKER, PORT, keepalive=60)
    cli.loop_start()
    time.sleep(3.0)

    st, b, _ = http("POST", WORKER + "/api/ui/command", True, token=token, body={
        "cmd_id": cmd_id, "device_id": args.device_id,
        "family": "query", "action_id": "workflow.sync_info",
        "params": {}, "actor": "prod-check"
    })
    rec("P3-1", "提交命令 ⇒ 503 audit_unavailable（审计写不进去就不下发）",
        st == 503 and b.get("error") == "audit_unavailable",
        "http=%s error=%r" % (st, b.get("error")))

    time.sleep(8.0)
    hit = [f for f in frames if f.get("i") == cmd_id or f.get("id") == cmd_id]
    rec("P3-2", "确认**没有**下发到设备（8 s 内无该 cmd_id 回帧）", not hit,
        "回帧数=%d" % len(hit))
    cli.loop_stop()
    try:
        cli.disconnect()
    except Exception:
        pass

    print("\n===== P4 真实压缩帧 ingest（0008 缺失 ⇒ 只入 mqtt_messages）=====")
    whk = sec.get("WEBHOOK_SECRET", "")
    frame = {"s": 0, "c": "result", "i": "chk%08d" % (int(time.time()) % 100000000),
             "t": int(time.time()), "command": "probe"}
    st, b, _ = http("POST", WORKER + "/api/ingest", True,
                    headers={"X-Webhook-Key": whk},
                    body={"topic": "guo_feeder/%s/up" % args.device_id,
                          "payload": json.dumps(frame)})
    rec("P4-1", "ingest 接受真实压缩帧（200）", st == 200, "http=%s body=%s" % (st, str(b)[:80]))

    st, b, _ = http("POST", WORKER + "/api/ingest", True, headers={"X-Webhook-Key": "bad"},
                    body={"topic": "guo_feeder/%s/up" % args.device_id, "payload": "{}"})
    rec("P4-2", "错 webhook key ⇒ 401", st == 401, "http=%s" % st)

    print("\n===== P5 只读端点（不依赖 0008）=====")
    st, b, _ = http("GET", WORKER + "/api/history/telemetry?limit=3", True, token=token)
    rec("P5-1", "/api/history/telemetry 可用", st == 200, "http=%s rows=%s" % (st, len(b.get("rows") or [])))
    st, b, _ = http("GET", WORKER + "/api/admin/device/" + args.device_id + "/snapshot", True, token=token)
    rec("P5-2", "device snapshot 可用", st == 200, "http=%s keys=%s" % (st, sorted(b.keys())[:5]))
    st, b, _ = http("GET", WORKER + "/api/admin/metrics", True, token=token)
    rec("P5-3", "/api/admin/metrics 可用", st == 200, "http=%s" % st)
    st, b, _ = http("GET", WORKER + "/api/history/telemetry?limit=3", True)
    rec("P5-4", "无 Bearer 的只读端点 ⇒ 401", st == 401, "http=%s" % st)

    print("\n===== P6 Pages（不需登录即可验证的闸门）=====")
    st, b, h = http("GET", PAGES + "/app.html", False)
    rec("P6-1", "无会话访问 /app.html ⇒ 302（中间件保护生效）", st == 302, "http=%s loc=%s" % (st, h.get("location")))
    st, b, h = http("GET", PAGES + "/assets/js/gf-api.js", False)
    rec("P6-2", "静态资源可访问（未被中间件误拦）", st == 200, "http=%s" % st)
    st, b, _ = http("GET", PAGES + "/api/auth/session", False)
    rec("P6-3", "/api/auth/session 无会话 ⇒ authenticated=false",
        st in (200, 401) and (b.get("authenticated") is False or st == 401),
        "http=%s body=%s" % (st, str(b)[:120]))
    st, b, _ = http("POST", PAGES + "/api/command", False, body={"cmd_id": "x", "family": "query"})
    rec("P6-4", "无会话提交命令 ⇒ 401（Pages 闸门）", st == 401, "http=%s" % st)

    print("\n===== 汇总 =====")
    for stt in ("PASS", "FAIL", "NOT VERIFIED", "BLOCKED"):
        print("  %-13s %d" % (stt, sum(1 for r in RESULTS if r["status"] == stt)))
    with open(args.json, "w", encoding="utf-8", newline="\n") as f:
        json.dump(RESULTS, f, ensure_ascii=False, indent=2)
    print("  -> %s" % args.json)
    return 0 if all(r["status"] != "FAIL" for r in RESULTS) else 1


if __name__ == "__main__":
    sys.exit(main())
