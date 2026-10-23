# -*- coding: utf-8 -*-
"""通用「改一个 Config 参数 → 保存 → 重启 → 回读」工具（串口 + MQTT 同会话）

为什么必须串口+MQTT 同会话
-------------------------
`config_save` 成功后设备**必然重启**。要证明"真的落盘了"必须同时看到：
  ① `config saved` + 重启流程（串口）
  ② 重启后模块初始化行反映新值（串口）
  ③ `config_query` 回读等于目标（MQTT 第二帧）

用法
----
    python cfg_set_and_verify.py --module weight --key enable --value false \
        --expect-init "[Weight] Disabled by config" --serial-out .pio/w1.log

`--value` 自动推断类型：true/false → bool，纯整数 → int，含小数点 → float，其余 → 字符串。
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

RESULTS = []


def rec(case, desc, ok, detail="", status=None):
    if status is None:
        status = "PASS" if ok else "FAIL"
    RESULTS.append({"case": case, "desc": desc, "status": status, "detail": str(detail)[:400]})
    print("  [%-8s] %-12s %s   %s" % (case, status, desc, str(detail)[:180]))


def coerce(text):
    low = text.strip().lower()
    if low in ("true", "false"):
        return low == "true"
    if re.fullmatch(r"-?\d+", text.strip()):
        return int(text.strip())
    if re.fullmatch(r"-?\d+\.\d+", text.strip()):
        return float(text.strip())
    return text


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--device-id", required=True)
    ap.add_argument("--module", required=True)
    ap.add_argument("--key", required=True)
    ap.add_argument("--value", required=True)
    ap.add_argument("--expect-init", default="", help="重启后串口应出现的子串")
    ap.add_argument("--forbid-init", default="", help="重启后串口不应出现的子串")
    ap.add_argument("--port", default="COM8")
    ap.add_argument("--verify-only", action="store_true",
                    help="只读验证当前值 + 启动日志，不做 config_set / config_save")
    ap.add_argument("--expect-zero-fc", action="store_true",
                    help="重启后不应出现 FORCE CLOSE（weight 关闭时用）")
    ap.add_argument("--serial-out", default=".pio/cfg_serial.log")
    ap.add_argument("--json", default=".pio/cfg_result.json")
    args = ap.parse_args()

    target = coerce(args.value)
    sub, pub = "guo_feeder/%s/up" % args.device_id, "guo_feeder/%s/down" % args.device_id

    ser = serial.Serial()
    ser.port = args.port
    ser.baudrate = 115200
    ser.timeout = 0.2
    ser.dtr = False
    ser.rts = False
    ser.open()

    # ★ 烧录用了 `--after no_reset` ⇒ 芯片停在下载模式，只打开串口**不会**启动。
    #   必须 DTR=False（IO0 高，正常启动）+ RTS 脉冲（EN 低 → 复位）。
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
            with lock:
                lines.append(ln)
            f.write("%7.2f %s\n" % (time.time() - t0, ln))
            f.flush()
        f.close()

    threading.Thread(target=reader, daemon=True).start()

    frames = []

    def on_connect(c, u, flags, rc, *a):
        code = getattr(rc, "value", rc)
        print("[MQTT] connected rc=%s" % (code,))
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
        cli = mqtt.Client(CallbackAPIVersion.VERSION2, client_id="wb_cfg_%d" % int(time.time()))
    except Exception:
        cli = mqtt.Client(client_id="wb_cfg_%d" % int(time.time()))
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

    def read_key():
        """config_query 是两帧；从第二帧的 data 里取 key。"""
        got = send("cfg-q-%d" % int(time.time() * 1000),
                   {"c": "system", "p": {"o": "config_query", "module": args.module}},
                   wait=18.0, want=2)
        for f in got:
            blob = json.dumps(f, ensure_ascii=False)
            m = re.search(r'"%s"\s*:\s*(true|false|-?\d+(?:\.\d+)?|"[^"]*")' % re.escape(args.key), blob)
            if m:
                raw = m.group(1)
                if raw == "true":
                    return True
                if raw == "false":
                    return False
                if raw.startswith('"'):
                    return raw[1:-1]
                return float(raw) if "." in raw else int(raw)
        return None

    print("  ... 等首次 boot + MQTT 就绪（22 s）")
    time.sleep(22.0)

    print("\n===== 配置变更：%s.%s -> %r =====" % (args.module, args.key, target))

    before = read_key()
    rec("C-01", "读取当前 %s.%s" % (args.module, args.key), before is not None, "现值=%r" % (before,))

    if args.verify_only:
        rec("C-02", "verify-only 模式：跳过写入", True,
            "只验证当前值与启动日志（不做 config_set / config_save）", "PASS")
        after = before
    else:
        got = send("cfg-set", {"c": "system", "p": {"o": "config_set", "module": args.module,
                                                    "key": args.key, "value": target}}, wait=16.0)
        rec("C-02", "config_set 受理", bool(got) and got[0].get("s") in (0, 1),
            json.dumps(got[0], ensure_ascii=False)[:200] if got else "无 result")

        got = send("cfg-save", {"c": "system", "p": {"o": "config_save"}}, wait=18.0)
        rec("C-03", "config_save 受理", bool(got) and got[0].get("s") in (0, 1),
            json.dumps(got[0], ensure_ascii=False)[:200] if got else "无 result")

        print("  ... 等待安全重启（52 s）")
        time.sleep(52.0)
        after = read_key()

    rec("C-04", "当前 %s.%s == %r" % (args.module, args.key, target),
        after == target, "现值=%r（改前 %r）" % (after, before))

    with lock:
        text = "\n".join(lines)

    # ★ 必须只看**最后一次启动之后**的日志：捕获窗口里包含改配置之前的那次 boot，
    #   全局匹配会把"旧启动的 Init OK"算进来。
    rst = text.rfind("rst:0x")
    after_rst = text[rst:] if rst >= 0 else text

    if args.verify_only:
        # 只读模式本来就没有 config_save，不能要求出现 "config saved"
        rec("C-05", "串口出现重启（verify-only 不检查 config saved）", rst >= 0,
            "rst=%s" % (rst >= 0,))
    else:
        rec("C-05", "串口出现 config saved + 重启",
            ("config saved" in text) and (rst >= 0),
            "saved=%s rst=%s" % ("config saved" in text, rst >= 0))

    if args.expect_init:
        rec("C-06", "重启后串口出现预期初始化行", args.expect_init in after_rst,
            "pattern=%r" % args.expect_init)
    if args.forbid_init:
        rec("C-07", "重启后串口**不**出现被禁止的行", args.forbid_init not in after_rst,
            "pattern=%r" % args.forbid_init)

    # 附带观测：DispenseGuard 强关次数（只在 after_rst 里数，才有意义）
    n_fc = after_rst.count("FORCE CLOSE")
    n_we = after_rst.count("Weight error received")
    print("  [OBS     ] %-12s 重启后 FORCE CLOSE=%d / Weight error=%d" % ("观测", n_fc, n_we))
    RESULTS.append({
        "case": "C-OBS",
        "desc": "重启后 DispenseGuard 强关/重量错误次数",
        "status": ("FAIL" if (args.expect_zero_fc and n_fc > 0) else "PASS"),
        "detail": "FORCE CLOSE=%d Weight error=%d" % (n_fc, n_we)
    })

    stop["v"] = True
    time.sleep(0.4)
    try:
        cli.loop_stop()
        cli.disconnect()
        ser.close()
    except Exception:
        pass

    print("\n===== 汇总 =====")
    for st in ("PASS", "FAIL", "NOT VERIFIED", "BLOCKED"):
        print("  %-13s %d" % (st, sum(1 for r in RESULTS if r["status"] == st)))
    with open(args.json, "w", encoding="utf-8", newline="\n") as f:
        json.dump(RESULTS, f, ensure_ascii=False, indent=2)
    print("  -> %s" % args.json)
    return 0 if all(r["status"] != "FAIL" for r in RESULTS) else 1


if __name__ == "__main__":
    sys.exit(main())
