# -*- coding: utf-8 -*-
"""遗留风险 #4 复核：串口调试命令是否绕过急停锁存。

背景（要推翻或确认的旧结论）
----------------------------
第三轮报告写"串口调试命令 tstep_start / wf_start 绕过锁存"。核查代码发现：
main.cpp 里这些串口命令**都走 `workflow_enqueue_action()` / `workflow_start()`**，
而这两处在第二轮已加了锁存拦截 ⇒ 结论应改为"已被覆盖"，但必须真机证明。

★ 教训（本轮的两次坑，都写进代码注释了）
-----------------------------------------
1. 串口 `cm <json>` 用的是**未压缩**键 `{"cmd":…,"ob":…}`；
   MQTT 用的是压缩键 `{"c":…,"p":{"o":…}}`。
   混用会得到 `s=4 "Missing 'command' field"` ⇒ **锁存根本没置上**，
   后续"验证被拒"全是假的。脚本里必须先断言锁存已置，才继续。
2. 绝不发送 `wfc run <供水流程 slot>`。锁存若没置上就是真的放水。
   本脚本**完全不发 wfc run**：`workflow_start` 的拦截点由 MQTT 侧
   （T4-07，锁存中 workflow 被拒）与代码位置共同证明。

安全
----
* 只发"锁存中被拒"方向的命令；不驱动步进（DRV8825 损坏，不发 tstep_start）。
* 每次发危险命令前都先断言 `latched=true`。
"""

import argparse
import json
import sys
import time

import serial

RESULTS = []


def rec(case, desc, ok, detail="", status=None):
    if status is None:
        status = "PASS" if ok else "FAIL"
    RESULTS.append({"case": case, "desc": desc, "status": status, "detail": str(detail)[:400]})
    print("  [%-8s] %-12s %s   %s" % (case, status, desc, str(detail)[:170]))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="COM8")
    ap.add_argument("--secs", type=float, default=40.0)
    ap.add_argument("--out", default=".pio/serial_latch.log")
    ap.add_argument("--json", default=".pio/serial_latch.json")
    args = ap.parse_args()

    ser = serial.Serial()
    ser.port = args.port
    ser.baudrate = 115200
    ser.timeout = 0.25
    ser.dtr = False
    ser.rts = False
    ser.open()
    ser.write(b"\r\n")

    lines = []
    f = open(args.out, "w", encoding="utf-8", newline="\n")
    t0 = time.time()

    def pump(sec):
        end = time.time() + sec
        while time.time() < end:
            raw = ser.readline()
            if not raw:
                continue
            ln = raw.decode("utf-8", "replace").rstrip("\r\n")
            if not ln:
                continue
            lines.append(ln)
            f.write("%7.2f %s\n" % (time.time() - t0, ln))
            f.flush()

    def send(cmd, wait=2.5):
        print("[TX] " + cmd)
        base = len(lines)
        ser.write((cmd + "\r\n").encode())
        pump(wait)
        return "\n".join(lines[base:])

    # ★ 打开串口**不一定**复位 ⇒ 设备可能早就启动完毕、日志很稀疏。
    #   这里只要证明"串口在收数据"，不要按行数设高门槛。
    pump(20.0)
    rec("S-01", "串口已连通（收到日志行）", len(lines) > 5, "lines=%d" % len(lines))

    # ① 用**未压缩**键置锁存
    seg = send('cm {"cmd":"safety","ob":"stop_all"}')
    latch_set = '"latched":true' in seg.replace(" ", "") or '"latched": true' in seg
    rec("S-02", "串口 cm 通路置锁存成功（回帧 latched=true）", latch_set,
        [ln for ln in seg.split("\n") if "latched" in ln][:1])

    if not latch_set:
        rec("S-03", "锁存未置 ⇒ 后续命令**不发送**（避免真开阀）", False,
            "fail-safe：跳过", "BLOCKED")
        f.close()
        try:
            ser.close()
        except Exception:
            pass
        with open(args.json, "w", encoding="utf-8", newline="\n") as jf:
            json.dump(RESULTS, jf, ensure_ascii=False, indent=2)
        return 1

    # ② 锁存期间：串口启动类 Action 必须被拒（= tstep_start 走的同一个闸门）
    seg = send('cm {"cmd":"execute_action","ob":"VIBRATION_START"}')
    rec("S-03", "锁存期间串口 execute_action(VIBRATION_START) 被拒",
        "latch" in seg.lower(),
        [ln for ln in seg.split("\n") if "latch" in ln.lower()][:1])

    seg = send('cm {"cmd":"execute_action","ob":"TEST_STEPPER_START"}')
    rec("S-04", "锁存期间串口 execute_action(TEST_STEPPER_START) 被拒（tstep_start 同闸门）",
        "latch" in seg.lower(),
        [ln for ln in seg.split("\n") if "latch" in ln.lower()][:1])

    # ③ 锁存期间停止类仍可用
    seg = send('cm {"cmd":"execute_action","ob":"VALVE_CLOSE"}')
    rec("S-05", "锁存期间停止类 VALVE_CLOSE 仍可用",
        '"status":"success"' in seg.replace(" ", "") or '"s":0' in seg.replace(" ", "")
        or '"s":1' in seg.replace(" ", ""),
        [ln for ln in seg.split("\n") if "RESULT" in ln][:1])

    # ④ 解锁收尾
    seg = send('cm {"cmd":"safety","ob":"clear"}')
    rec("S-06", "clear 后锁存解除",
        '"latched":false' in seg.replace(" ", "") or '"latched": false' in seg,
        [ln for ln in seg.split("\n") if "latched" in ln][:1])

    pump(1.5)
    f.close()
    try:
        ser.close()
    except Exception:
        pass

    print("\n===== 汇总 =====")
    for st in ("PASS", "FAIL", "NOT VERIFIED", "BLOCKED"):
        print("  %-13s %d" % (st, sum(1 for r in RESULTS if r["status"] == st)))
    with open(args.json, "w", encoding="utf-8", newline="\n") as jf:
        json.dump(RESULTS, jf, ensure_ascii=False, indent=2)
    print("  -> %s" % args.json)
    return 0 if all(r["status"] != "FAIL" for r in RESULTS) else 1


if __name__ == "__main__":
    sys.exit(main())
