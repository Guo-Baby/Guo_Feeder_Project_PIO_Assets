# -*- coding: utf-8 -*-
"""T8：阀门 60 秒安全超时实机测试（串口 + MQTT 同会话）

判定要点
--------
1. **是谁关的阀**：只认 `[Valve] Safety timeout! Forced close (open > N ms)`。
   - DispenseGuard 强关 打的是 `[Valve] FORCE CLOSE (pin=.., level=0)`（无 Safety timeout 行）
   - 正常关阀 打的是 `[Valve] CLOSE (pin=.., level=0)`
   ⇒ 三者可严格区分，不靠猜。
2. **实际开启时长**：用串口时间戳算 `[Valve] OPEN` → `[Valve] Safety timeout` 的差值。
3. **关阀后不得被重新打开**：关闭后继续观察 10 s 的 `valve_status`。

安全前置（脚本自己检查，不满足就中止）
--------------------------------------
- `weight.enable == false`（否则 DispenseGuard 会随机抢先强关，测试无意义）
- `valve.safety_timeout_sec == 60`
- 无 RUNNING/WAITING 的 Workflow（避免定时触发起阀干扰）
- 阀门初始关闭
- 急停未锁存

用法
----
    python t_valve_timeout.py --device-id 288485896ce4 --confirm-supply
    （--confirm-supply 必须显式给出：表示现场已布好容器、有人监护）
"""

import argparse
import json
import os
import re
import ssl
import sys
import threading
import time

import serial
import paho.mqtt.client as mqtt

BROKER = "n302933b.ala.cn-hangzhou.emqxsl.cn"
PORT = 8883
CA = os.path.join(
    os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))),
    "data", "emqxsl-ca.crt",
)
USER, PASS = "workbuddy", "workbuddy"

EXPECT_TIMEOUT_S = 60
RESULTS = []


def rec(case, desc, ok, detail="", status=None):
    if status is None:
        status = "PASS" if ok else "FAIL"
    RESULTS.append({"case": case, "desc": desc, "status": status, "detail": str(detail)[:400]})
    print("  [%-8s] %-12s %s   %s" % (case, status, desc, str(detail)[:180]))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--device-id", required=True)
    ap.add_argument("--port", default="COM8")
    ap.add_argument("--confirm-supply", action="store_true")
    ap.add_argument("--observe", type=float, default=90.0)
    ap.add_argument("--serial-out", default=".pio/valve_timeout.log")
    ap.add_argument("--json", default=".pio/valve_timeout.json")
    args = ap.parse_args()

    if not args.confirm_supply:
        print("[ABORT] 需要 --confirm-supply（现场布好容器/有人监护）")
        return 2

    sub, pub = "guo_feeder/%s/up" % args.device_id, "guo_feeder/%s/down" % args.device_id

    ser = serial.Serial()
    ser.port = args.port
    ser.baudrate = 115200
    ser.timeout = 0.2
    ser.dtr = False
    ser.rts = False
    ser.open()
    ser.dtr = False
    ser.rts = True
    time.sleep(0.15)
    ser.rts = False
    time.sleep(0.05)
    print("[probe] RTS reset pulse sent")

    lines, lock = [], threading.Lock()
    stop = {"v": False}

    def reader():
        f = open(args.serial_out, "w", encoding="utf-8", newline="\n")
        t0 = time.time()
        while not stop["v"]:
            try:
                raw = ser.readline()
            except Exception:
                break
            if not raw:
                continue
            ln = raw.decode("utf-8", "replace").rstrip("\r\n")
            if not ln:
                continue
            ts = time.time() - t0
            with lock:
                lines.append((ts, ln))
            f.write("%7.2f %s\n" % (ts, ln))
            f.flush()
        f.close()

    threading.Thread(target=reader, daemon=True).start()

    frames = []

    def on_connect(c, u, flags, rc, *a):
        print("[MQTT] connected rc=%s" % (getattr(rc, "value", rc),))
        c.subscribe(sub, qos=1)

    def on_message(c, u, msg):
        try:
            obj = json.loads(msg.payload.decode("utf-8", "replace"))
        except Exception:
            return
        frames.append(obj)

    try:
        from paho.mqtt.client import CallbackAPIVersion
        cli = mqtt.Client(CallbackAPIVersion.VERSION2, client_id="wb_vt_%d" % int(time.time()))
    except Exception:
        cli = mqtt.Client(client_id="wb_vt_%d" % int(time.time()))
    cli.username_pw_set(USER, PASS)
    cli.tls_set(ca_certs=CA, cert_reqs=ssl.CERT_REQUIRED)
    cli.on_connect = on_connect
    cli.on_message = on_message
    cli.connect(BROKER, PORT, keepalive=60)
    cli.loop_start()

    def send(cid, body, wait=16.0, want=1):
        body = dict(body)
        body["i"] = cid
        before = len(frames)
        cli.publish(pub, json.dumps(body), qos=1)
        print("[DOWN] %s" % json.dumps(body, ensure_ascii=False))
        t0 = time.time()
        got = []
        while time.time() - t0 < wait:
            got = [f for f in frames[before:]
                   if (f.get("i") == cid or f.get("id") == cid)
                   and (f.get("c") or f.get("cmd")) == "result"]
            if len(got) >= want:
                break
            time.sleep(0.15)
        return got

    def cfg(module):
        got = send("vt-cfg-%d" % int(time.time() * 1000),
                   {"c": "system", "p": {"o": "config_query", "module": module}}, wait=18.0, want=2)
        for f in got:
            if isinstance(f.get("data"), dict):
                return f["data"]
        return None

    def state(key):
        got = send("vt-q-%d" % int(time.time() * 1000),
                   {"c": "query", "p": {"q": "query_state", "o": key}}, wait=12.0)
        return got[0].get("value") if got else None

    print("  ... 等 boot + MQTT（22 s）")
    time.sleep(22.0)

    print("\n===== 前置条件 =====")

    wcfg = cfg("weight") or {}
    rec("V-01", "weight.enable == false", wcfg.get("enable") is False, "weight.enable=%r" % (wcfg.get("enable"),))

    vcfg = cfg("valve") or {}
    rec("V-02", "valve.safety_timeout_sec == 60", vcfg.get("safety_timeout_sec") == 60,
        "safety_timeout_sec=%r" % (vcfg.get("safety_timeout_sec"),))

    got = send("vt-clear", {"c": "safety", "p": {"o": "clear"}}, wait=12.0)
    latched = got[0].get("latched") if got else "?"
    rec("V-03", "急停未锁存", latched is False, "latched=%r" % (latched,))

    v0 = state("valve_status")
    rec("V-04", "阀门初始关闭", str(v0) == "false", "valve_status=%r" % (v0,))

    wfs = send("vt-wfs", {"c": "query", "p": {"q": "query_workflows"}}, wait=12.0)
    runs = []
    if wfs:
        for w in (wfs[0].get("workflows") or []):
            if str(w.get("state", "")).upper() in ("RUNNING", "WAITING"):
                runs.append(w.get("id"))
    rec("V-05", "无 RUNNING/WAITING 的 Workflow（避免定时触发起阀）", not runs, "runs=%r" % (runs,))

    if not (wcfg.get("enable") is False and vcfg.get("safety_timeout_sec") == 60
            and latched is False and str(v0) == "false" and not runs):
        print("\n[ABORT] 前置条件不满足，**不开阀**。")
        stop["v"] = True
        time.sleep(0.4)
        try:
            cli.loop_stop(); cli.disconnect(); ser.close()
        except Exception:
            pass
        return 1

    # 取真实 registry（动作帧必须 v+k）
    act_ver, act_map = None, {}
    for f in send("vt-reg", {"c": "registry", "k": 0}, wait=12.0):
        if f.get("s") == 0:
            act_ver = f.get("v")
            for pair in (f.get("d") or []):
                if isinstance(pair, list) and len(pair) >= 2:
                    act_map[str(pair[1])] = int(pair[0])
    print("[REG] action v=%r n=%d" % (act_ver, len(act_map)))

    print("\n===== 开阀并观察 60 s 安全超时 =====")
    t_open_wall = time.time()
    got = send("vt-open", {"c": "action", "v": act_ver, "k": act_map.get("VALVE_OPEN")}, wait=12.0)
    rec("V-06", "VALVE_OPEN 受理", bool(got) and got[0].get("s") in (0, 1),
        json.dumps(got[0], ensure_ascii=False)[:180] if got else "无 result")

    # 轮询：每 5 s 一次，直到出现 Safety timeout 或超时上限
    t_end = time.time() + args.observe
    polls = []
    hit_timeout = False
    while time.time() < t_end:
        time.sleep(5.0)
        with lock:
            if any("Safety timeout" in ln for _, ln in lines):
                hit_timeout = True
        v = state("valve_status")
        elapsed = time.time() - t_open_wall
        polls.append((round(elapsed, 1), v))
        print("    +%5.1fs valve_status=%r" % (elapsed, v))
        if hit_timeout:
            break

    print("  ... 继续观察 10 s（确认不被重新打开）")
    time.sleep(10.0)

    stop["v"] = True
    time.sleep(0.4)
    try:
        cli.loop_stop(); cli.disconnect(); ser.close()
    except Exception:
        pass

    with lock:
        rows = list(lines)
    text = "\n".join(ln for _, ln in rows)

    # ---------- 判定 ----------
    rec("V-07", "串口出现 [Valve] Safety timeout 行", hit_timeout,
        [ln for _, ln in rows if "Safety timeout" in ln][:1])

    limits = re.findall(r"Safety timeout! Forced close \(open > (\d+) ms\)", text)
    rec("V-08", "超时阈值就是 60000 ms", ("60000" in limits),
        "实测阈值=%r" % (limits,))

    # 实际开启时长（串口时间戳）
    t_open_s = None
    t_stop_s = None
    for ts, ln in rows:
        if t_open_s is None and "[Valve] OPEN" in ln:
            t_open_s = ts
        if t_open_s is not None and "Safety timeout" in ln:
            t_stop_s = ts
            break
    dur = (t_stop_s - t_open_s) if (t_open_s is not None and t_stop_s is not None) else None
    rec("V-09", "实际开启时长 ≈ 60 s（59.0~61.5 s）",
        dur is not None and 59.0 <= dur <= 61.5, "实测=%.2f s" % (dur,) if dur else "未测到")

    # 触发原因必须是 Safety timeout，不是 DispenseGuard
    idx = text.find("Safety timeout")
    seg = text[max(0, idx - 400):idx] if idx >= 0 else ""
    rec("V-10", "关阀前 400 字符内没有 DispenseGuard 强关（排除误判触发源）",
        "FORCE CLOSE" not in seg, "segment 含 FORCE CLOSE=%s" % ("FORCE CLOSE" in seg))

    v_after = polls[-1][1] if polls else None
    rec("V-11", "超时后 valve_status=false", str(v_after) == "false", "最后轮询=%r" % (v_after,))

    # 关阀后 10 s 内是否被重新打开（看串口有没有新的 OPEN）
    opens = [ln for _, ln in rows if "[Valve] OPEN" in ln]
    rec("V-12", "全程只开阀一次（关闭后未被重新打开）", len(opens) == 1, "OPEN 行数=%d" % len(opens))

    rec("V-13", "END 状态：阀门关闭", str(v_after) == "false", "valve_status=%r" % (v_after,))

    print("\n===== 汇总 =====")
    for st in ("PASS", "FAIL", "NOT VERIFIED", "BLOCKED"):
        print("  %-13s %d" % (st, sum(1 for r in RESULTS if r["status"] == st)))
    with open(args.json, "w", encoding="utf-8", newline="\n") as f:
        json.dump({"results": RESULTS, "polls": polls,
                   "open_serial_ts": t_open_s, "stop_serial_ts": t_stop_s,
                   "duration_s": dur}, f, ensure_ascii=False, indent=2)
    print("  -> %s" % args.json)
    return 0 if all(r["status"] != "FAIL" for r in RESULTS) else 1


if __name__ == "__main__":
    sys.exit(main())
