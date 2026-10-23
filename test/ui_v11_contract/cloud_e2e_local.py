# -*- coding: utf-8 -*-
"""端到端：真实设备压缩帧 → 本地 Worker（wrangler dev）→ 真实本地 D1

为什么要在本地做
----------------
生产 D1 **没有** `0008` 的三张表（ui_session / command_audit / safety_event），
所以生产环境里 `/api/ui/command` 会 503。为了验证 `stateFromFrame` 修复
在**真实 SQLite**（而不是内存桩）上生效，用 `wrangler dev --local` +
本地 D1（已单独应用 0008，**不碰生产**）跑一遍完整链路。

链路
----
  ① POST /api/ui/command  (Bearer)  → dev Worker → 真实 EMQX 发布（只读查询）
                                   → 本地 D1 落一条 command_audit(requesting→published)
  ② 真实设备回帧（MQTT /up 抓到）    → POST /api/ingest (X-Webhook-Key)
                                   → applyDeviceFrameToCommand → stateFromFrame → SQL UPDATE
  ③ 读本地 D1 的 command_audit 行，断言终态

用法
----
    python cloud_e2e_local.py --device-id 288485896ce4 --base http://127.0.0.1:8791
"""

import argparse
import json
import os
import re
import ssl
import sys
import time
import urllib.request

import paho.mqtt.client as mqtt

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DEV_VARS = os.path.join(ROOT, "..", "..", "Cloudflare_Assets", "guo-feeder-api", ".dev.vars")
DEV_VARS = os.path.normpath(DEV_VARS)
CA = os.path.join(ROOT, "data", "emqxsl-ca.crt")

BROKER = "n302933b.ala.cn-hangzhou.emqxsl.cn"
PORT = 8883
USER, PASS = "workbuddy", "workbuddy"

RESULTS = []


def rec(case, desc, ok, detail="", status=None):
    if status is None:
        status = "PASS" if ok else "FAIL"
    RESULTS.append({"case": case, "desc": desc, "status": status, "detail": str(detail)[:400]})
    print("  [%-8s] %-12s %s   %s" % (case, status, desc, str(detail)[:180]))


def load_vars():
    out = {}
    with open(DEV_VARS, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#") or "=" not in line:
                continue
            k, v = line.split("=", 1)
            out[k.strip()] = v.strip().strip('"')
    return out


def http_json(method, url, token=None, body=None, headers=None, timeout=30):
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(url, data=data, method=method)
    req.add_header("content-type", "application/json")
    if token:
        req.add_header("authorization", "Bearer " + token)
    for k, v in (headers or {}).items():
        req.add_header(k, v)
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            raw = r.read().decode("utf-8", "replace")
            try:
                return r.status, json.loads(raw or "{}")
            except Exception:
                return r.status, {"_raw": raw[:400]}
    except urllib.error.HTTPError as e:
        raw = e.read().decode("utf-8", "replace")
        try:
            return e.code, json.loads(raw or "{}")
        except Exception:
            return e.code, {"_raw": raw[:400]}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--device-id", required=True)
    ap.add_argument("--base", default="http://127.0.0.1:8791")
    ap.add_argument("--json", default=".pio/cloud_e2e.json")
    args = ap.parse_args()

    env = load_vars()
    token = env.get("ADMIN_API_TOKEN", "")
    whk = env.get("WEBHOOK_SECRET", "")
    if not token or not whk:
        print("[ABORT] .dev.vars 缺 ADMIN_API_TOKEN / WEBHOOK_SECRET")
        return 2

    # 本地 dev 不走系统代理
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
    urllib.request.install_opener(opener)

    print("\n===== E2E-0 健康检查 =====")
    st, body = http_json("GET", args.base + "/api/ui/catalog", token=token)
    rec("E0-1", "dev Worker /api/ui/catalog 可达", st == 200 and bool(body.get("queries")),
        "http=%s queries=%d" % (st, len(body.get("queries") or [])))

    st, body = http_json("GET", args.base + "/api/ui/catalog")
    rec("E0-2", "无 Bearer ⇒ 401", st == 401, "http=%s" % st)

    # ---- ① 先订阅 /up（必须在提交命令之前，否则会错过回帧）
    print("\n===== E2E-2 订阅设备上行 =====")
    frames = []
    sub = "guo_feeder/%s/up" % args.device_id

    def on_connect(c, u, flags, rc, *a):
        c.subscribe(sub, qos=1)

    def on_message(c, u, msg):
        try:
            frames.append(json.loads(msg.payload.decode("utf-8", "replace")))
        except Exception:
            pass

    try:
        from paho.mqtt.client import CallbackAPIVersion
        cli = mqtt.Client(CallbackAPIVersion.VERSION2, client_id="wb_e2e_%d" % int(time.time()))
    except Exception:
        cli = mqtt.Client(client_id="wb_e2e_%d" % int(time.time()))
    cli.username_pw_set(USER, PASS)
    cli.tls_set(ca_certs=CA, cert_reqs=ssl.CERT_REQUIRED)
    cli.on_connect = on_connect
    cli.on_message = on_message
    cli.connect(BROKER, PORT, keepalive=60)
    cli.loop_start()
    time.sleep(3.0)
    rec("E2-0", "MQTT 已订阅 /up（先订阅再下发）", True, sub)

    # ---- ② 提交一条只读查询（会真的发到设备，只读无副作用）
    print("\n===== E2E-1 提交只读查询 =====")
    cmd_id = "e2e%08d" % (int(time.time()) % 100000000)
    payload = {
        "cmd_id": cmd_id,
        "device_id": args.device_id,
        "family": "query",
        "action_id": "workflow.sync_info",
        "params": {},
        "actor": "e2e-test"
    }
    st, body = http_json("POST", args.base + "/api/ui/command", token=token, body=payload)
    rec("E1-1", "提交只读查询 → 202 已发布", st == 202 and body.get("published") is True,
        "http=%s state=%r executed=%r" % (st, body.get("state"), body.get("executed")))
    rec("E1-2", "响应明确 executed=false / verified=false",
        body.get("executed") is False and body.get("verified") is False,
        "executed=%r verified=%r" % (body.get("executed"), body.get("verified")))

    if st != 202:
        print("[ABORT] 提交失败：%s" % json.dumps(body, ensure_ascii=False)[:400])
        return 1

    st, st_body = http_json("GET", args.base + "/api/ui/command/" + cmd_id + "?device_id=" + args.device_id,
                            token=token)
    rec("E1-3", "发布后状态非终态", st == 200 and st_body.get("terminal") is False,
        "state=%r terminal=%r" % (st_body.get("state"), st_body.get("terminal")))

    # ---- ③ 等真实设备回帧
    print("\n===== E2E-2b 等待真实回帧 =====")
    got = []
    t0 = time.time()
    while time.time() - t0 < 20 and not got:
        got = [f for f in frames if f.get("i") == cmd_id or f.get("id") == cmd_id]
        time.sleep(0.2)
    cli.loop_stop()
    try:
        cli.disconnect()
    except Exception:
        pass

    rec("E2-1", "抓到设备真实回帧", bool(got), json.dumps(got[-1], ensure_ascii=False)[:200] if got else "无")
    if not got:
        return 1

    frame = got[-1]
    rec("E2-2", "回帧是压缩形态（有整数 s、无 status）",
        isinstance(frame.get("s"), int) and "status" not in frame,
        "keys=%s" % sorted(frame.keys()))

    # ---- ③ 把真实帧喂给 dev Worker 的 ingest
    print("\n===== E2E-3 真实帧回灌 ingest =====")
    st, body = http_json("POST", args.base + "/api/ingest", headers={"X-Webhook-Key": whk},
                         body={"topic": "guo_feeder/%s/up" % args.device_id,
                               "payload": json.dumps(frame, ensure_ascii=False)})
    rec("E3-1", "ingest 接受真实帧", st in (200, 201), "http=%s body=%s" % (st, json.dumps(body, ensure_ascii=False)[:160]))

    time.sleep(1.5)
    st, st_body = http_json("GET", args.base + "/api/ui/command/" + cmd_id + "?device_id=" + args.device_id,
                            token=token)
    rec("E3-2", "审计状态被真实帧推进到终态 done",
        st == 200 and st_body.get("state") == "done",
        "state=%r effective=%r terminal=%r" % (st_body.get("state"),
                                               st_body.get("effective_state"),
                                               st_body.get("terminal")))
    rec("E3-3", "first_frame_at 已落（设备已确认）",
        bool(st_body.get("first_frame_at")), "first_frame_at=%r" % (st_body.get("first_frame_at"),))

    print("\n===== 汇总 =====")
    for stt in ("PASS", "FAIL", "NOT VERIFIED", "BLOCKED"):
        print("  %-13s %d" % (stt, sum(1 for r in RESULTS if r["status"] == stt)))
    with open(args.json, "w", encoding="utf-8", newline="\n") as f:
        json.dump({"results": RESULTS, "cmd_id": cmd_id, "frame": frame}, f,
                  ensure_ascii=False, indent=2)
    print("  -> %s  cmd_id=%s" % (args.json, cmd_id))
    return 0 if all(r["status"] != "FAIL" for r in RESULTS) else 1


if __name__ == "__main__":
    sys.exit(main())
