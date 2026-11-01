#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
gf_ble_watch.py —— BLE 温湿度长周期监听器（只读串口）

用途
----
覆盖固件真实 BLE 调度：扫描窗口 15/30 分钟 + 休息 48–60 分钟 + 下一轮窗口，
并把**非 verbose** 的关键标记实时解析出来，避免人工盯守。

固件真实参数（src/app/MiThermometer.cpp）
-----------------------------------------
    BOOT_DELAY           15 s
    SCAN_WINDOW          LEVEL0=15 min / LEVEL1=30 min
    每轮 SCAN_ON         1 s（BLE interval=160/window=160 ⇒ 窗口内接近满占空）
    每轮 SCAN_OFF        1.8–2.3 s 随机
    SLEEP                48–60 min 随机
    失败阈值             连续 4 个窗口未同时拿到温+湿 ⇒ LEVEL2 永久停用

哪些标记**一定**会打印（非 verbose）
------------------------------------
    [MiThermo] init OK
    [MiThermo] scan failed count=N level=M      ← 窗口失败（含等级）
    [MiThermo] LEVEL2: scanning disabled        ← 永久停用
    [MiThermo] temp + humidity received, BLE scan stopped   ← 成功
    [MiThermo] enter sleep <N> sec              ← 窗口结束 + 下一轮预测基准
    rst:0xN (...)                               ← 复位原因
    boot_seq=N                                  ← 重启唯一硬证据

以下标记受 MI_THERMO_DEBUG_VERBOSE 控制（默认 0 ⇒ **不会出现**），
因此"没有这几行"不代表故障，报告中必须说明：
    enter scan window / scan window finished / wakeup from sleep /
    scan success / decrypt OK

用法
----
    python tools/gf_ble_watch.py --duration 7200
    python tools/gf_ble_watch.py --duration 7200 --no-restart   # 不主动复位
"""
import argparse
import json
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from gf_cmd import Monitor  # noqa: E402

# (正则, 标签, 是否"事件"型（用于摘要计数）)
PATTERNS = [
    (re.compile(r"rst:0x([0-9a-fA-F]+)\s*\(([^)]*)\)"), "RESET", True),
    (re.compile(r"boot_seq=(\d+)"), "BOOT_SEQ", True),
    (re.compile(r"\[MiThermo\] init OK"), "THERMO_INIT", True),
    (re.compile(r"\[MiThermo\] collection disabled by config"), "THERMO_CFG_OFF", True),
    (re.compile(r"\[MiThermo\] scan failed count=(\d+) level=(\d+)"), "SCAN_FAILED", True),
    (re.compile(r"\[MiThermo\] LEVEL2: scanning disabled"), "LEVEL2_DISABLED", True),
    (re.compile(r"\[MiThermo\] temp \+ humidity received"), "THERMO_SUCCESS", True),
    (re.compile(r"\[MiThermo\] enter sleep (\d+) sec"), "SLEEP", True),
    (re.compile(r"\[Cloud TELEM\]"), "CLOUD_TELEM", True),
    (re.compile(r"\[Cloud (?:LOG|CMD|UP)\]"), "CLOUD_PUB", False),
]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="COM8")
    ap.add_argument("--duration", type=float, default=7200.0,
                    help="总监听秒数（默认 7200 = 120 min）")
    ap.add_argument("--boot-wait", type=float, default=20.0,
                    help="开串口会复位设备，等待启动完成再开始计时观察")
    ap.add_argument("--no-restart", action="store_true",
                    help="不主动发重启（默认也不发：开串口本身已触发复位）")
    ap.add_argument("--log", default=None)
    ap.add_argument("--marks", default=None, help="事件标记 jsonl 输出")
    ns = ap.parse_args()

    log = ns.log or (".pio/ble_watch_%s.log" % time.strftime("%H%M%S"))
    marks = ns.marks or (log + ".marks.jsonl")
    os.makedirs(os.path.dirname(log) or ".", exist_ok=True)

    mon = Monitor(ns.port, 115200, log)
    t_open = time.time()
    print("[watch] port opened at %s (opening the port resets the ESP32)"
          % time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(t_open)))
    print("[watch] boot wait %.0fs ..." % ns.boot_wait, flush=True)

    fmarks = open(marks, "w", encoding="utf-8")
    counts = {}
    events = []

    try:
        # ★ 不复位起点：启动阶段的 rst:0xN / boot_seq 必须被采到（重启唯一硬证据）。
        #   因此从 0 开始扫，elapsed 以"开串口"为零点；boot-wait 结束后打一个
        #   OBS_START 标记，正式观察时长从该标记起算。
        scan_from = 0
        t_start = t_open
        time.sleep(ns.boot_wait)
        obs_begin = time.time()
        end_at = obs_begin + ns.duration
        ev0 = {
            "t_wall": time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(obs_begin)),
            "t_elapsed_s": round(obs_begin - t_open, 1),
            "tag": "OBS_START",
            "line": "observation begins (duration %.0f s)" % ns.duration,
        }
        fmarks.write(json.dumps(ev0, ensure_ascii=False) + "\n")
        fmarks.flush()
        print("[%7.1fs %s] %-16s %s"
              % (obs_begin - t_open, ev0["t_wall"][11:], "OBS_START", ev0["line"]),
              flush=True)
        last_hb = time.time()
        sleep_target = None  # 最近一次 enter sleep 的秒数
        sleep_at = None

        print("[watch] observation starts %s, duration %.0f min"
              % (time.strftime("%H:%M:%S"), ns.duration / 60.0), flush=True)

        while time.time() < end_at:
            chunk = mon.since(scan_from)
            if chunk:
                scan_from = mon.size()
                for ln in chunk.replace("\r", "\n").split("\n"):
                    s = ln.strip()
                    if not s:
                        continue
                    for rx, tag, is_event in PATTERNS:
                        m = rx.search(s)
                        if not m:
                            continue
                        now = time.time()
                        el = now - t_start
                        ev = {
                            "t_wall": time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(now)),
                            "t_elapsed_s": round(el, 1),
                            "tag": tag,
                            "line": s[:300],
                        }
                        fmarks.write(json.dumps(ev, ensure_ascii=False) + "\n")
                        fmarks.flush()
                        if is_event:
                            counts[tag] = counts.get(tag, 0) + 1
                            events.append(ev)
                            extra = ""
                            if tag == "SLEEP":
                                sleep_target = int(m.group(1))
                                sleep_at = now
                                nxt = now + sleep_target
                                extra = "  -> next window predicted at %s" % time.strftime(
                                    "%H:%M:%S", time.localtime(nxt))
                            print("[%7.1fs %s] %-16s %s%s"
                                  % (el, ev["t_wall"][11:], tag, s[:160], extra),
                                  flush=True)
                        break

            now = time.time()
            if now - last_hb >= 60.0:
                last_hb = now
                el = now - t_start
                remain = end_at - now
                hb = "[hb %6.1fs] remain=%5.0fmin rx=%dKB err=%d counts=%s" % (
                    el, remain / 60.0,
                    int(getattr(mon, "rx_bytes", 0) / 1024),
                    int(getattr(mon, "read_errors", 0)),
                    ",".join("%s=%d" % (k, v) for k, v in sorted(counts.items())) or "none")
                if sleep_at and sleep_target:
                    hb += " | next_window_at=%s" % time.strftime(
                        "%H:%M:%S", time.localtime(sleep_at + sleep_target))
                print(hb, flush=True)
            time.sleep(1.0)

        print("\n[watch] observation window finished at %s"
              % time.strftime("%Y-%m-%d %H:%M:%S"), flush=True)
        print("[watch] event counts: %s" % json.dumps(counts, ensure_ascii=False))
        print("[watch] serial log: %s" % os.path.abspath(log))
        print("[watch] marks jsonl: %s" % os.path.abspath(marks))
        return 0
    finally:
        fmarks.close()
        mon.close()


if __name__ == "__main__":
    sys.exit(main())
