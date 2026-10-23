# -*- coding: utf-8 -*-
"""T5：强制重启（system.force_restart）—— 串口 + MQTT 同会话取证

判据（全部来自串口与回帧，不靠推断）
------------------------------------
  T5-1  收到 force_restart 回帧（不保证一定收到 —— 300 ms 后就复位，
        回帧可能来不及出 outbox；**收不到不代表命令没执行**）
  T5-2  串口出现 "[Safety] stop_all"（先停硬件）
  T5-3  串口出现 "boot validated"（启动标记写入）
  T5-4  串口出现 "FORCE restart requested" / "FORCE restart window elapsed"
  T5-5  串口出现**第二次** "[CFG][I] init done"（确实重启了）
  T5-6  第二次启动**没有** "recovering (boot_ok=0"（没有因标记缺失而回滚）
  T5-7  重启后配置未回滚：仍打印 safety=60 s（T6 刚设的值）
  T5-8  重启后 MQTT 重新连上

⚠️ 强制中断 Flash 写入的场景**不做**（无法在不破坏配置的前提下构造），
   报告里记为 NOT VERIFIED。
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
    print("  [%-8s] %-12s %s   %s" % (case, status, desc, str(detail)[:170]))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--device-id", required=True)
    ap.add_argument("--port", default="COM8")
    ap.add_argument("--out", default=".pio/t5_serial.log")
    ap.add_argument("--json", default=".pio/t5_result.json")
    ap.add_argument("--boot-wait", type=float, default=20.0)
    ap.add_argument("--after", type=float, default=55.0)
    args = ap.parse_args()

    did = args.device_id
    sub, pub = "guo_feeder/%s/up" % did, "guo_feeder/%s/down" % did

    ser = serial.Serial()
    ser.port = args.port
    ser.baudrate = 115200
    ser.timeout = 0.2
    ser.dtr = False
    ser.rts = False
    ser.open()

    lines, lock = [], threading.Lock()
    stop = {"v": False}

    def reader():
        f = open(args.out, "w", encoding="utf-8", newline="\n")
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
        c.subscribe(sub, qos=1)
        print("[MQTT] connected/subscribed")

    def on_message(c, u, msg):
        try:
            obj = json.loads(msg.payload.decode("utf-8", "replace"))
        except Exception:
            return
        frames.append(obj)
        print("[UP] %s" % json.dumps(obj, ensure_ascii=False)[:200])

    try:
        from paho.mqtt.client import CallbackAPIVersion
        cli = mqtt.Client(CallbackAPIVersion.VERSION2, client_id="wb_t5_%d" % int(time.time()))
    except Exception:
        cli = mqtt.Client(client_id="wb_t5_%d" % int(time.time()))
    cli.username_pw_set(USER, PASS)
    cli.tls_set(ca_certs=CA, cert_reqs=ssl.CERT_REQUIRED)
    cli.on_connect = on_connect
    cli.on_message = on_message
    cli.connect(BROKER, PORT, keepalive=60)
    cli.loop_start()

    print("  ... 等首次 boot + MQTT 就绪（%.0f s）" % args.boot_wait)
    time.sleep(args.boot_wait)

    print("\n===== T5 强制重启 =====")
    cid = "wbt5-fr"
    before = len(frames)
    cli.publish(pub, json.dumps({"c": "system", "i": cid, "p": {"o": "force_restart"}}), qos=1)
    print("[DOWN] {\"c\":\"system\",\"i\":\"%s\",\"p\":{\"o\":\"force_restart\"}}" % cid)

    # 给它 3 s 收 result（300 ms 后就会复位，很可能收不到）
    t0 = time.time()
    fr = None
    while time.time() - t0 < 3.0:
        for f in frames[before:]:
            if (f.get("i") == cid) and (f.get("c") or f.get("cmd")) == "result":
                fr = f
                break
        if fr:
            break
        time.sleep(0.1)

    if fr is None:
        rec("T5-1", "收到 force_restart 回帧", False,
            "未收到（复位 300 ms 后发生，outbox 可能来不及发出 —— 属已知时序风险，不判固件失败）",
            "NOT VERIFIED")
    else:
        rec("T5-1", "收到 force_restart 回帧", True, json.dumps(fr, ensure_ascii=False)[:220])
        rec("T5-1b", "回帧含 boot_flag_written / critical_at_request",
            "boot_flag_written" in fr and "critical_at_request" in fr,
            "boot_flag_written=%r critical=%r" % (fr.get("boot_flag_written"), fr.get("critical_at_request")))

    print("  ... 观察重启（%.0f s）" % args.after)
    time.sleep(args.after)

    stop["v"] = True
    time.sleep(0.4)
    try:
        cli.loop_stop()
        cli.disconnect()
        ser.close()
    except Exception:
        pass

    with lock:
        text = "\n".join(lines)
        nlines = len(lines)

    boots = text.count("[CFG][I] init done")
    # ★ 打开串口**不一定**复位（本机实测有时不复位）⇒ 不能断言"至少 2 次 boot"。
    #   改为以重启标记为锚点，只断言"重启之后确实又完整启动了一次"。
    rst_pos = text.rfind("rst:0x")
    after_rst = text[rst_pos:] if rst_pos >= 0 else ""
    rec("T5-2", "串口出现 [Safety] stop_all（先停硬件）",
        "[Safety] stop_all" in text, "")
    rec("T5-3", "串口出现 boot validated（启动标记写入）",
        "boot validated" in text, "出现 %d 次" % text.count("boot validated"))
    rec("T5-4", "串口出现 FORCE restart 相关日志",
        ("FORCE restart requested" in text) or ("FORCE restart window elapsed" in text),
        "requested=%s elapsed=%s" % ("FORCE restart requested" in text,
                                     "FORCE restart window elapsed" in text))
    rec("T5-5", "确实发生重启（串口出现 rst:0x 复位向量）",
        rst_pos >= 0, "出现 %d 次 rst:0x" % text.count("rst:0x"))
    rec("T5-5b", "重启后走完完整启动（rst 之后有 init done + 10/10 模块）",
        ("[CFG][I] init done" in after_rst) and ("loaded 10/10 modules" in after_rst),
        "after-rst 长度 %d 字符" % len(after_rst))
    rec("T5-5c", "复位原因 = software（区别于上电/看门狗）",
        "reset reason=software" in after_rst, "")

    rollback = re.search(r"recovering \(boot_ok=0", text)
    rec("T5-6", "重启后未因启动标记缺失触发回滚", rollback is None,
        "命中 recovering(boot_ok=0)" if rollback else "无 recovering(boot_ok=0)")

    safe60 = len(re.findall(r"\[Valve\] Init OK[^\n]*safety=60 s", text))
    safe_other = re.findall(r"\[Valve\] Init OK[^\n]*safety=(\d+) s", text)
    rec("T5-7", "重启后配置未回滚（safety 仍为 60 s）", safe60 >= 1,
        "safety 出现序列: %s（60 命中 %d 次）" % (safe_other, safe60))

    rec("T5-8", "重启后 MQTT 重新连接", text.count("MQTT connected") >= 2,
        "MQTT connected 出现 %d 次" % text.count("MQTT connected"))

    # 新增的启动期安全电平也必须**在重启后的那次启动**里再次出现
    rec("T5-9", "重启后仍执行 [Safety] outputs init done",
        "[Safety] outputs init done" in after_rst,
        "after-rst 内命中 %d 次" % after_rst.count("[Safety] outputs init done"))

    # 额外观测：HX711 未接时 DispenseGuard 的强关次数（不计入 PASS/FAIL）
    n_fc = text.count("[Valve] FORCE CLOSE")
    print("  [OBS     ] %-12s 本次会话 [Valve] FORCE CLOSE 共 %d 次（HX711 未接导致）" % ("观测", n_fc))

    print("\n===== 汇总 =====")
    for st in ("PASS", "FAIL", "NOT VERIFIED", "BLOCKED"):
        print("  %-13s %d" % (st, sum(1 for r in RESULTS if r["status"] == st)))
    with open(args.json, "w", encoding="utf-8", newline="\n") as f:
        json.dump(RESULTS, f, ensure_ascii=False, indent=2)
    print("  串口日志 %d 行 -> %s" % (nlines, args.out))
    return 0 if all(r["status"] != "FAIL" for r in RESULTS) else 1


if __name__ == "__main__":
    sys.exit(main())
