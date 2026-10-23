# -*- coding: utf-8 -*-
"""T6：阀门安全超时（读 → 改 60 → 保存 → 重启 → 回读）

为什么必须"串口 + MQTT 同会话"
------------------------------
`config_save` 成功后会**必然重启**（SystemCommand 安全重启）。要证明：
  ① 保存真的落了盘（不是只改了 RAM）
  ② 重启后 `[Valve] Init OK (... safety=60 s)` 用的是新值
  ③ 重启前后 boot 序号变了（证明是重启而不是命令没生效）
就必须在同一个串口会话里既下发 MQTT 命令、又持续读串口。

安全
----
* 只改 `valve.safety_timeout_sec` 一个字段，不碰 wifi / mqtt / 凭据。
* 改完立刻 `config_save`，设备自行安全重启（10 s 安全窗口 + boot）。
* 本脚本**不**做长时间供水；60 s 超时行为在有 HX711 时另测（本轮 BLOCKED）。

用法
----
    C:/Users/wang/.workbuddy/binaries/python/envs/default/Scripts/python.exe \
        test/ui_v11_contract/t6_valve_timeout.py --device-id 288485896ce4 --port COM8
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

TARGET_SEC = 60
RESULTS = []


def rec(case, desc, ok, detail="", status=None):
    if status is None:
        status = "PASS" if ok else "FAIL"
    RESULTS.append({"case": case, "desc": desc, "status": status, "detail": str(detail)[:400]})
    print("  [%-8s] %-12s %s   %s" % (case, status, desc, str(detail)[:170]))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--device-id", required=True)
    ap.add_argument("--port", default="COM8")
    ap.add_argument("--secs", type=float, default=110.0)
    ap.add_argument("--out", default=".pio/t6_serial.log")
    ap.add_argument("--json", default=".pio/t6_result.json")
    args = ap.parse_args()

    did = args.device_id
    sub = "guo_feeder/%s/up" % did
    pub = "guo_feeder/%s/down" % did

    # ---------------- 串口 ----------------
    ser = serial.Serial()
    ser.port = args.port
    ser.baudrate = 115200
    ser.timeout = 0.2
    ser.dtr = False
    ser.rts = False
    ser.open()                      # 打开即复位 ⇒ 抓到一次完整 boot（基线）

    lines = []
    lock = threading.Lock()
    stop_flag = {"v": False}

    def reader():
        f = open(args.out, "w", encoding="utf-8", newline="\n")
        t0 = time.time()
        while not stop_flag["v"]:
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
                lines.append(ln)
            f.write("%7.2f %s\n" % (ts, ln))
            f.flush()
        f.close()

    th = threading.Thread(target=reader, daemon=True)
    th.start()

    # ---------------- MQTT ----------------
    frames = []

    def on_connect(c, u, flags, rc, *a):
        c.subscribe(sub, qos=1)

    def on_message(c, u, msg):
        try:
            obj = json.loads(msg.payload.decode("utf-8", "replace"))
        except Exception:
            return
        frames.append(obj)
        print("[UP] %s" % json.dumps(obj, ensure_ascii=False)[:220])

    try:
        from paho.mqtt.client import CallbackAPIVersion
        cli = mqtt.Client(CallbackAPIVersion.VERSION2,
                          client_id="wb_t6_%d" % int(time.time()))
    except Exception:
        cli = mqtt.Client(client_id="wb_t6_%d" % int(time.time()))
    cli.username_pw_set(USER, PASS)
    cli.tls_set(ca_certs=CA, cert_reqs=ssl.CERT_REQUIRED)
    cli.on_connect = on_connect
    cli.on_message = on_message
    cli.connect(BROKER, PORT, keepalive=60)
    cli.loop_start()

    def send(cid, body, wait=12.0, want=1):
        body = dict(body)
        body["i"] = cid
        before = len(frames)
        cli.publish(pub, json.dumps(body), qos=1)
        print("[DOWN] %s" % json.dumps(body, ensure_ascii=False))
        t0 = time.time()
        got = []
        while time.time() - t0 < wait:
            got = [f for f in frames[before:]
                   if (f.get("i") == cid or f.get("id") == cid) and (f.get("c") or f.get("cmd")) == "result"]
            if len(got) >= want:
                break
            time.sleep(0.15)
        return got

    # 等 boot 与 MQTT 就绪
    time.sleep(22.0)

    # 先取真实 registry（动作帧必须带 v + k，不能硬编码）
    act_ver = None
    act_map = {}
    for f in send("wbt6-reg", {"c": "registry", "k": 0}, wait=12.0):
        if f.get("s") == 0:
            act_ver = f.get("v")
            for pair in (f.get("d") or []):
                if isinstance(pair, list) and len(pair) >= 2:
                    act_map[str(pair[1])] = int(pair[0])
    print("[REG] action v=%r n=%d" % (act_ver, len(act_map)))

    print("\n===== T6 阀门安全超时 =====")

    # ① 读现值（异步两帧）
    got = send("wbt6-01", {"c": "system", "p": {"o": "config_query", "module": "valve"}}, wait=16.0, want=2)
    cur = None
    for f in got:
        blob = json.dumps(f, ensure_ascii=False)
        m = re.search(r'"safety_timeout_sec"\s*:\s*(\d+)', blob)
        if m:
            cur = int(m.group(1))
            break
    rec("T6-01", "读取 valve.safety_timeout_sec", cur is not None, "现值=%r" % (cur,))

    # ② 设为 60
    got = send("wbt6-02", {"c": "system", "p": {"o": "config_set", "module": "valve",
                                                "key": "safety_timeout_sec", "value": TARGET_SEC}},
               wait=14.0)
    rec("T6-02", "config_set safety_timeout_sec=60",
        bool(got) and got[0].get("s") in (0, 1), json.dumps(got[0], ensure_ascii=False)[:200] if got else "无 result")

    # ③ 保存（会触发重启）
    got = send("wbt6-03", {"c": "system", "p": {"o": "config_save"}}, wait=16.0)
    rec("T6-03", "config_save 受理",
        bool(got) and got[0].get("s") in (0, 1), json.dumps(got[0], ensure_ascii=False)[:200] if got else "无 result")

    # ④ 等重启 + 重新连接
    print("  ... 等待安全重启（50 s）")
    time.sleep(50.0)

    with lock:
        text = "\n".join(lines)

    # 保存/重启的串口证据
    rec("T6-04", "串口出现 config saved + 重启流程",
        ("config saved" in text) and ("restart" in text.lower()),
        "saved=%s restart=%s" % ("config saved" in text, "restart" in text.lower()))
    safe_lines = [ln for ln in lines if "[Valve] Init OK" in ln]
    after60 = [ln for ln in safe_lines if "safety=60 s" in ln]
    before300 = [ln for ln in safe_lines if "safety=300 s" in ln]
    rec("T6-05", "重启后 [Valve] Init OK 打印 safety=60 s",
        len(after60) >= 1, "命中 %d 条（重启前 300 的 %d 条）" % (len(after60), len(before300)))

    # ⑤ 回读确认持久化
    got = send("wbt6-06", {"c": "system", "p": {"o": "config_query", "module": "valve"}}, wait=16.0, want=2)
    val = None
    for f in got:
        blob = json.dumps(f, ensure_ascii=False)
        m = re.search(r'"safety_timeout_sec"\s*:\s*(\d+)', blob)
        if m:
            val = int(m.group(1))
            break
    rec("T6-06", "重启后回读 safety_timeout_sec == 60", val == TARGET_SEC, "回读=%r" % (val,))

    # ⑥ 正常开阀 / 关阀（短时，不做长时间供水）
    def act_frame(name):
        if act_ver is None or name not in act_map:
            return None
        return {"c": "action", "v": act_ver, "k": act_map[name]}

    fo = act_frame("VALVE_OPEN")
    fc = act_frame("VALVE_CLOSE")
    if fo is None or fc is None:
        rec("T6-07", "开阀/关阀（registry 缺失）", False, "registry 未取到", "BLOCKED")
    else:
        got = send("wbt6-07", fo, wait=12.0)
        rec("T6-07", "开阀受理（短时）", bool(got) and got[0].get("s") in (0, 1),
            json.dumps(got[0], ensure_ascii=False)[:160] if got else "无 result")
        time.sleep(2.0)
        got = send("wbt6-08", fc, wait=12.0)
        rec("T6-08", "关阀受理", bool(got) and got[0].get("s") in (0, 1),
            json.dumps(got[0], ensure_ascii=False)[:160] if got else "无 result")
    time.sleep(1.0)
    got = send("wbt6-09", {"c": "query", "p": {"q": "query_state", "o": "valve_status"}}, wait=12.0)
    v = got[0].get("value") if got else None
    rec("T6-09", "关阀后 valve_status=false", str(v) == "false", "value=%r" % (v,))

    stop_flag["v"] = True
    time.sleep(0.4)
    try:
        cli.loop_stop()
        cli.disconnect()
    except Exception:
        pass
    try:
        ser.close()
    except Exception:
        pass

    print("\n===== 汇总 =====")
    for st in ("PASS", "FAIL", "NOT VERIFIED", "BLOCKED"):
        print("  %-13s %d" % (st, sum(1 for r in RESULTS if r["status"] == st)))
    with open(args.json, "w", encoding="utf-8", newline="\n") as f:
        json.dump(RESULTS, f, ensure_ascii=False, indent=2)
    return 0 if all(r["status"] != "FAIL" for r in RESULTS) else 1


if __name__ == "__main__":
    sys.exit(main())
