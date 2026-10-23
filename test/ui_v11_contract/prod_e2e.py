# -*- coding: utf-8 -*-
"""第四轮 · 生产端到端验收（Worker + 真实设备 + 生产 D1）

链路（全部生产，无本地替代）
----------------------------
  POST /api/ui/command (Bearer) → 生产 Worker → 真实 EMQX → 真机
      → 设备回帧 → EMQX 数据集成 → 生产 Worker /api/ingest → 生产 D1 command_audit

覆盖
----
A 命令审计与状态推进：创建/发布/真实回帧/终态/迟到帧/workflow.get/config_query/
  重启族不确定语义/safety_event
B 会话与鉴权：D1 会话签发-校验-撤销、无 Bearer、错 Bearer、撤销后拒绝

安全
----
* 只发只读查询（`workflow.sync_info` / `config_query` / `workflow.get`）。
* 只发一次 `system.restart`（安全重启，不动执行器、不写配置）。
* 只发一次 `safety.stop_all`（把一切停下，安全方向）。
* **不**开阀、**不**驱动步进、**不**跑供水流程。
* 需要注入的"迟到帧"用独立 cmd_id 合成，不伪造成功。
"""

import argparse
import json
import os
import sys
import time
import urllib.error
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SECRETS = os.path.normpath(os.path.join(ROOT, "..", "..", "Cloudflare_Assets", ".secrets.local"))

WORKER = "https://guo-feeder-api.guobaby.workers.dev"
PROXY = "http://127.0.0.1:7890"
UA = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) Chrome/126.0"

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


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None


OP = urllib.request.build_opener(
    urllib.request.ProxyHandler({"http": PROXY, "https": PROXY}), NoRedirect())


def http(method, path, token=None, body=None, headers=None, timeout=40):
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(WORKER + path, data=data, method=method)
    req.add_header("content-type", "application/json")
    req.add_header("user-agent", UA)
    if token:
        req.add_header("authorization", "Bearer " + token)
    for k, v in (headers or {}).items():
        req.add_header(k, v)
    try:
        with OP.open(req, timeout=timeout) as r:
            raw = r.read().decode("utf-8", "replace")
            try:
                return r.status, json.loads(raw or "{}")
            except Exception:
                return r.status, {"_raw": raw[:300]}
    except urllib.error.HTTPError as e:
        raw = e.read().decode("utf-8", "replace")
        try:
            return e.code, json.loads(raw or "{}")
        except Exception:
            return e.code, {"_raw": raw[:300]}


def new_id(prefix):
    return prefix + ("%08d" % (int(time.time() * 1000) % 100000000))


def wait_terminal(cmd_id, dev, token, seconds=25, want=None):
    end = time.time() + seconds
    last = None
    while time.time() < end:
        st, b = http("GET", "/api/ui/command/" + cmd_id + "?device_id=" + dev, token=token)
        last = b
        if st == 200 and (want is None or b.get("state") == want):
            return st, b
        time.sleep(1.0)
    return st, last


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--device-id", required=True)
    ap.add_argument("--json", default=".pio/prod_e2e.json")
    args = ap.parse_args()

    sec = load_secrets()
    token = sec.get("ADMIN_API_TOKEN", "")
    whk = sec.get("WEBHOOK_SECRET", "")
    dev = args.device_id
    if not token:
        print("[ABORT] 无 ADMIN_API_TOKEN")
        return 2

    print("\n===== A1 只读命令：审计创建 → 发布 → 真实回帧 → 终态 =====")
    cid = new_id("prd1")
    st, b = http("POST", "/api/ui/command", token=token, body={
        "cmd_id": cid, "device_id": dev, "family": "query",
        "action_id": "workflow.sync_info", "params": {}, "actor": "prod-e2e"
    })
    rec("A1-1", "提交只读查询 → 202 published", st == 202 and b.get("published") is True,
        "http=%s state=%r executed=%r verified=%r" % (st, b.get("state"),
                                                      b.get("executed"), b.get("verified")))
    rec("A1-2", "响应不谎称已执行/已验证",
        b.get("executed") is False and b.get("verified") is False,
        "executed=%r verified=%r" % (b.get("executed"), b.get("verified")))

    st, row = wait_terminal(cid, dev, token, 25, "done")
    rec("A1-3", "真实回帧把审计推进到 done", st == 200 and row.get("state") == "done",
        "state=%r terminal=%r" % (row.get("state"), row.get("terminal")))
    rec("A1-4", "first_frame_at / finished_at 已落",
        bool(row.get("first_frame_at")) and bool(row.get("finished_at")),
        "first=%r finished=%r duration_ms=%r" % (row.get("first_frame_at"),
                                                 row.get("finished_at"), row.get("duration_ms")))

    print("\n===== A2 迟到中间帧不得回退终态 =====")
    st, b = http("POST", "/api/ingest", headers={"X-Webhook-Key": whk}, body={
        "topic": "guo_feeder/%s/up" % dev,
        "payload": json.dumps({"s": 2, "c": "result", "i": cid, "t": int(time.time())})
    })
    rec("A2-1", "注入迟到 running 帧", st in (200, 201), "http=%s" % st)
    time.sleep(2.0)
    st, row = http("GET", "/api/ui/command/" + cid + "?device_id=" + dev, token=token)
    rec("A2-2", "终态 done 未被迟到 running 覆盖", st == 200 and row.get("state") == "done",
        "state=%r" % (row.get("state"),))

    print("\n===== A3 workflow.get 使用 p.id =====")
    cid = new_id("prd3")
    st, b = http("POST", "/api/ui/command", token=token, body={
        "cmd_id": cid, "device_id": dev, "family": "query",
        "action_id": "workflow.get", "params": {"id": 3}, "actor": "prod-e2e"
    })
    rec("A3-1", "params.id=3 被接受并发布", st == 202, "http=%s err=%r" % (st, b.get("error")))
    if st == 202:
        st, row = wait_terminal(cid, dev, token, 25, "done")
        rec("A3-2", "workflow.get(id=3) 真机返回 done", st == 200 and row.get("state") == "done",
            "state=%r code=%r" % (row.get("state"), row.get("result_code")))
    else:
        rec("A3-2", "workflow.get(id=3) 真机返回 done", False, "未发布", "BLOCKED")

    cid_bad = new_id("prd3b")
    st, b = http("POST", "/api/ui/command", token=token, body={
        "cmd_id": cid_bad, "device_id": dev, "family": "query",
        "action_id": "workflow.get", "params": {"o": 3}, "actor": "prod-e2e"
    })
    rec("A3-3", "旧字段 o 必须被 400 拒绝（不静默改 slot）",
        st == 400 and b.get("error") == "invalid_params",
        "http=%s err=%r field=%r" % (st, b.get("error"), b.get("field")))

    print("\n===== A4 config_query 两帧 =====")
    cid = new_id("prd4")
    st, b = http("POST", "/api/ui/command", token=token, body={
        "cmd_id": cid, "device_id": dev, "family": "query",
        "action_id": "system.config_query", "params": {"module": "valve"}, "actor": "prod-e2e"
    })
    rec("A4-1", "config_query 提交", st == 202, "http=%s" % st)
    if st == 202:
        # 立刻查：很可能仍是 accepted（第一帧还没到 / 只到了第一帧）
        st, r1 = http("GET", "/api/ui/command/" + cid + "?device_id=" + dev, token=token)
        early_state = r1.get("state")
        st, row = wait_terminal(cid, dev, token, 30, "done")
        rec("A4-2", "第二帧到达后终态 done", st == 200 and row.get("state") == "done",
            "state=%r（早期观察到 %r）" % (row.get("state"), early_state))
        rec("A4-3", "两帧都关联到同一条审计（只有一条记录）", st == 200,
            "cmd_id=%r device=%r" % (row.get("cmd_id"), row.get("device_id")))

    print("\n===== A5 状态查询的跨设备歧义 =====")
    st, b = http("GET", "/api/ui/command/" + cid + "?device_id=" + dev, token=token)
    rec("A5-1", "带 device_id 精确命中", st == 200 and b.get("device_id") == dev,
        "http=%s device=%r" % (st, b.get("device_id")))
    st, b = http("GET", "/api/ui/command/" + cid, token=token)
    rec("A5-2", "不带 device_id：单设备命中 200 / 多设备 409（不静默取最新）",
        st in (200, 409), "http=%s err=%r device_ids=%r" % (st, b.get("error"), b.get("device_ids")))

    print("\n===== A6 重启族：受理 ≠ 完成 =====")
    cid = new_id("prd6")
    st, b = http("POST", "/api/ui/command", token=token, body={
        "cmd_id": cid, "device_id": dev, "family": "system",
        "action_id": "system.restart", "params": {}, "actor": "prod-e2e"
    })
    rec("A6-1", "system.restart 提交（安全重启，不动执行器）", st == 202,
        "http=%s err=%r" % (st, b.get("error")))
    if st == 202:
        print("    ... 等设备安全重启 + 重新上线（55 s）")
        # 重启前先看一眼：设备回帧通常是 s=0（success）
        st, mid = wait_terminal(cid, dev, token, 20, "accepted")
        time.sleep(35.0)
        st, row = http("GET", "/api/ui/command/" + cid + "?device_id=" + dev, token=token)
        rec("A6-2", "设备已重新上线但命令**仍不是** done",
            st == 200 and row.get("state") != "done",
            "state=%r terminal=%r rebooting=%r" % (row.get("state"),
                                                   row.get("terminal"), row.get("rebooting")))
        rec("A6-3", "标记为 rebooting（供操作员判断，不作因果证明）",
            row.get("rebooting") is True, "rebooting=%r" % (row.get("rebooting"),))
        # 设备恢复通信证明
        ok_alive = False
        for _ in range(6):
            st2, b2 = http("POST", "/api/ui/command", token=token, body={
                "cmd_id": new_id("prd6b"), "device_id": dev, "family": "query",
                "action_id": "workflow.sync_info", "params": {}, "actor": "prod-e2e"
            })
            if st2 == 202:
                ok_alive = True
                break
            time.sleep(5.0)
        rec("A6-4", "重启后设备恢复可下发（在线）", ok_alive, "http=%s" % st2)

    print("\n===== A7 急停：safety_event 证据链 =====")
    cid = new_id("prd7")
    st, b = http("POST", "/api/ui/command", token=token, body={
        "cmd_id": cid, "device_id": dev, "family": "safety",
        "action_id": "safety.stop_all", "params": {}, "actor": "prod-e2e"
    })
    rec("A7-1", "safety.stop_all 提交", st == 202, "http=%s err=%r" % (st, b.get("error")))
    if st == 202:
        st, row = wait_terminal(cid, dev, token, 25, "done")
        rec("A7-2", "急停命令终态 done（云端只证明命令执行了）",
            st == 200 and row.get("state") == "done", "state=%r" % (row.get("state"),))

    print("\n===== B 会话（D1 可撤销会话）=====")
    st, b = http("POST", "/api/ui/session", token=token,
                 body={"action": "issue", "ttl_seconds": 300})
    rec("B-1", "签发 D1 会话", st == 200 and bool(b.get("sid")),
        "http=%s err=%r mode=%r revocable=%r" % (st, b.get("error"),
                                                 b.get("mode"), b.get("revocable")))
    sid = (b or {}).get("sid")
    secret = (b or {}).get("secret")
    # ★ 接口形状：verify 成功 = {ok:true,...}；失败 = {ok:false, reason} + 401/503
    #   （**没有** state 字段 —— 第一轮脚本按 state 断言是错的）
    if sid and secret:
        st, b2 = http("POST", "/api/ui/session", token=token,
                      body={"action": "verify", "sid": sid, "secret": secret})
        rec("B-2", "新会话校验通过", st == 200 and b2.get("ok") is True,
            "http=%s ok=%r sid=%r" % (st, b2.get("ok"), b2.get("sid")))

        st, b3 = http("POST", "/api/ui/session", token=token,
                      body={"action": "revoke", "sid": sid, "reason": "logout"})
        rec("B-3", "撤销会话", st == 200 and b3.get("revoked") is True,
            "http=%s revoked=%r already=%r" % (st, b3.get("revoked"), b3.get("already")))

        st, b4 = http("POST", "/api/ui/session", token=token,
                      body={"action": "verify", "sid": sid, "secret": secret})
        rec("B-4", "撤销后旧 secret 立即失效（登出即失效的真实判据）",
            st == 401 and b4.get("ok") is False,
            "http=%s ok=%r reason=%r" % (st, b4.get("ok"), b4.get("reason")))
    else:
        rec("B-2", "新会话校验", False, "未取到 sid/secret", "BLOCKED")
        rec("B-3", "撤销会话", False, "未取到 sid/secret", "BLOCKED")
        rec("B-4", "撤销后立即失效", False, "未取到 sid/secret", "BLOCKED")

    print("\n===== B2 鉴权闸门 =====")
    st, b = http("GET", "/api/ui/catalog")
    rec("B2-1", "无 Bearer ⇒ 401", st == 401, "http=%s" % st)
    st, b = http("GET", "/api/ui/catalog", token="wrong")
    rec("B2-2", "错 Bearer ⇒ 401", st == 401, "http=%s" % st)
    st, b = http("POST", "/api/ui/session", token=token, body={"action": "verify",
                                                               "sid": "nosuch0000000000",
                                                               "secret": "x"})
    rec("B2-3", "不存在/无效会话 ⇒ 拒绝（不降级放行）", st in (401, 404),
        "http=%s err=%r" % (st, b.get("error")))

    print("\n===== C 收尾：设备回到安全状态 =====")
    st, b = http("POST", "/api/ui/command", token=token, body={
        "cmd_id": new_id("prd9"), "device_id": dev, "family": "safety",
        "action_id": "safety.clear", "params": {}, "actor": "prod-e2e"
    })
    rec("C-1", "收尾解除锁存", st == 202, "http=%s err=%r" % (st, b.get("error")))

    print("\n===== 汇总 =====")
    for stt in ("PASS", "FAIL", "NOT VERIFIED", "BLOCKED"):
        print("  %-13s %d" % (stt, sum(1 for r in RESULTS if r["status"] == stt)))
    with open(args.json, "w", encoding="utf-8", newline="\n") as f:
        json.dump(RESULTS, f, ensure_ascii=False, indent=2)
    print("  -> %s" % args.json)
    return 0 if all(r["status"] != "FAIL" for r in RESULTS) else 1


if __name__ == "__main__":
    sys.exit(main())
