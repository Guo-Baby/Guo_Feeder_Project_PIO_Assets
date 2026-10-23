# -*- coding: utf-8 -*-
"""T1 启动观测探针 —— 只读串口，抓 boot 日志并给出关键判定。

用法：
    C:/Users/wang/.workbuddy/binaries/python/envs/default/Scripts/python.exe \
        test/ui_v11_contract/boot_probe.py --port COM8 --secs 75 --out .pio/t1_boot.log

注意
----
* 打开串口本身会让 CH343 拉 DTR/RTS ⇒ 板子复位 ⇒ 正好抓到完整 boot。
* 本脚本**只读**，不发任何命令、不改任何状态。
* 判定项（打印到 stdout，同时写入 `<out>.json`）：
    - `[Safety] outputs init done` 是否在 `wifi_init`/`[WiFi]` 之前出现
    - `[CFG][I] boot validated`
    - `[Safety]` 各引脚安全电平
    - RTC / HX711 相关的"预期缺失"错误行
    - 崩溃 / 看门狗 / 栈溢出 / 堆损坏 / Guru Meditation 痕迹
"""

import argparse
import json
import re
import sys
import time

import serial


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="COM8")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--secs", type=float, default=75.0)
    ap.add_argument("--out", default=".pio/t1_boot.log")
    ap.add_argument("--reset", action="store_true",
                    help="打开后脉冲 RTS 复位（烧录用了 --after no_reset 时必须加）")
    args = ap.parse_args()

    ser = serial.Serial()
    ser.port = args.port
    ser.baudrate = args.baud
    ser.timeout = 0.2
    ser.dtr = False
    ser.rts = False
    ser.open()

    if args.reset:
        # DTR=False ⇒ IO0 保持高（正常启动，不进下载模式）
        # RTS 拉低 ⇒ EN 拉低 ⇒ 复位；随后释放
        ser.dtr = False
        ser.rts = True
        time.sleep(0.15)
        ser.rts = False
        time.sleep(0.05)
        print("[probe] RTS pulse sent (reset)")

    lines = []
    t0 = time.time()
    with open(args.out, "w", encoding="utf-8", newline="\n") as f:
        f.write("### boot probe start %s\n" % time.strftime("%Y-%m-%d %H:%M:%S"))
        while time.time() - t0 < args.secs:
            try:
                raw = ser.readline()
            except Exception:
                break
            if not raw:
                continue
            line = raw.decode("utf-8", "replace").rstrip("\r\n")
            if not line:
                continue
            lines.append(line)
            f.write("%7.2f %s\n" % (time.time() - t0, line))
            f.flush()
    ser.close()

    text = "\n".join(lines)

    def idx(pat, start=0):
        for i, ln in enumerate(lines):
            if pat in ln:
                return i
        return -1

    checks = []
    i_safety = idx("[Safety] outputs init done")
    # 实际固件打印的是 "WiFi init..."（main.cpp），"init: WiFi" 是历史形态
    i_wifi = idx("WiFi init")
    if i_wifi < 0:
        i_wifi = idx("[WiFi]")
    if i_wifi < 0:
        i_wifi = idx("WiFi connecting")
    checks.append({
        "name": "T1-01 [Safety] outputs init done 出现",
        "ok": i_safety >= 0,
        "detail": "第 %d 行" % i_safety if i_safety >= 0 else "未出现",
    })
    checks.append({
        "name": "T1-02 该行早于 WiFi 初始化",
        "ok": i_safety >= 0 and i_wifi >= 0 and i_safety < i_wifi,
        "detail": "safety=%d wifi=%d" % (i_safety, i_wifi),
    })
    checks.append({
        "name": "T1-03 [CFG][I] boot validated",
        "ok": "boot validated" in text,
        "detail": "",
    })
    checks.append({
        "name": "T1-04 三个执行器引脚安全电平已打印",
        "ok": all(("[Safety] %s" % t) in text for t in ("valve:", "stepper_en:", "vibration:")),
        "detail": "",
    })

    fatal = [p for p in ("Guru Meditation", "Stack canary", "heap_caps_",
                         "abort()", "assert failed", "task_wdt",
                         "LoadProhibited", "StoreProhibited") if p in text]
    checks.append({
        "name": "T1-05 无崩溃/看门狗/栈溢出痕迹",
        "ok": not fatal,
        "detail": ",".join(fatal),
    })

    # 预期缺失（RTC 未接 / HX711 未接）—— 不算失败，单列
    expected = []
    if re.search(r"RTC.*(not found|fail|error|absent)|rtc.*WARN", text, re.I):
        expected.append("RTC 缺失相关日志")
    if re.search(r"HX711|weight.*(error|not ready|timeout)", text, re.I):
        expected.append("HX711/weight 相关日志")

    checks.append({
        "name": "T1-06 MQTT 已连接",
        "ok": ("MQTT connected" in text) or ("MQTT subscribed" in text),
        "detail": "",
    })

    print("=" * 70)
    for c in checks:
        print("  %-52s %s  %s" % (c["name"], "PASS" if c["ok"] else "FAIL", c["detail"]))
    print("-" * 70)
    print("  预期缺失（不算失败）：%s" % (", ".join(expected) if expected else "无"))
    print("  抓取 %d 行 / %.1f s -> %s" % (len(lines), args.secs, args.out))

    with open(args.out + ".json", "w", encoding="utf-8", newline="\n") as f:
        json.dump({"checks": checks, "expected_missing": expected,
                   "lines": len(lines)}, f, ensure_ascii=False, indent=2)
    return 0 if all(c["ok"] for c in checks) else 1


if __name__ == "__main__":
    sys.exit(main())
