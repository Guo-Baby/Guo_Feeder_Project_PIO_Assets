#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
gf_telem_check.py —— 遥测开关（mqtt.telemetry_enable）单会话探测

为什么必须"单会话"
------------------
`gf_cmd.py` 每次调用都会 open/close 串口；而**打开串口本身会让 ESP32 复位**
（本项目实测：`rst:0x1 (POWERON)`，boot_seq 递增）。复位会清空 RAM，
而 `config_set` **只写 RAM**。⇒ 用两次 `gf_cmd.py` 调用做"设置→回读"，
回读时设备已重启、值已丢失，会得出"设置无效"的假结论。

因此本脚本在一次串口会话内完成：

    ① 读 mqtt 模块全量配置（含 telemetry_enable 原值）
    ② config_set mqtt.telemetry_enable = true（仅 RAM）
    ③ 回读，确认写入生效
    ④ config_set 还原为原值（仅 RAM）
    ⑤ 回读，确认还原

安全约束
--------
· **不调用 config_save** ⇒ 不落盘，重启即失效，对生产配置零持久影响。
· 只碰 `mqtt.telemetry_enable` 一个键，不触碰其他生产配置。
· 脱敏：凭据只报长度，MAC 只报后 4 位。
· 不执行任何动作（不出粮、不通断电）。

用法
----
    python tools/gf_telem_check.py [--port COM8] [--boot-wait 18]
"""
import argparse
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from gf_cmd import Monitor, send, wait_result, summarize  # noqa: E402

MODULE = "mqtt"
KEY = "telemetry_enable"


def q(mon, module, key=None, timeout=15.0):
    """config_query + 等终态。"""
    p = {"module": module}
    if key:
        p["key"] = key
    cid = "t%d" % (int(time.time() * 1000) % 100000)
    send(mon, {"cmd": "system", "id": cid, "ob": "config_query", "p": p})
    st, payload, raw = wait_result(mon, cid, timeout)
    return st, payload


def setv(mon, module, key, value, timeout=15.0):
    """config_set（仅 RAM，不 save）。"""
    cid = "w%d" % (int(time.time() * 1000) % 100000)
    send(mon, {
        "cmd": "system", "id": cid, "ob": "config_set",
        "p": {"module": module, "key": key, "value": value},
    })
    st, payload, raw = wait_result(mon, cid, timeout)
    return st, payload


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="COM8")
    ap.add_argument("--boot-wait", type=float, default=18.0,
                    help="开串口会复位设备，等待其启动完成")
    ap.add_argument("--log", default=None)
    ns = ap.parse_args()

    log = ns.log or (".pio/gf_telem_%s.log" % time.strftime("%H%M%S"))
    os.makedirs(os.path.dirname(log) or ".", exist_ok=True)
    mon = Monitor(ns.port, 115200, log)

    try:
        print("[0] 串口已打开（设备随之复位）。等待启动 %.0fs ..." % ns.boot_wait)
        time.sleep(ns.boot_wait)

        # ① 原值
        st, d = q(mon, MODULE)
        if st != "done":
            print("[1] 读取 mqtt 模块失败: %s" % st)
            return 2
        data = (d or {}).get("data") or {}
        orig = data.get(KEY, "<absent>")
        print("[1] 原值 %s.%s = %r" % (MODULE, KEY, orig))
        print("    mqtt 模块摘要: %s" % summarize(data))

        # ② 设为 true
        st2, d2 = setv(mon, MODULE, KEY, True)
        print("[2] config_set true -> %s" % st2)

        # ③ 回读
        time.sleep(0.5)
        st3, d3 = q(mon, MODULE)
        v3 = ((d3 or {}).get("data") or {}).get(KEY, "<absent>")
        print("[3] 回读 %s = %r  (期望 True)" % (KEY, v3))

        # ④ 还原
        if orig == "<absent>":
            # 键原本不存在：还原成 false（与固件默认等价的显式值）
            st4, _ = setv(mon, MODULE, KEY, False)
            print("[4] 原键不存在，写回 false -> %s" % st4)
        else:
            st4, _ = setv(mon, MODULE, KEY, orig)
            print("[4] 还原为 %r -> %s" % (orig, st4))

        # ⑤ 回读
        time.sleep(0.5)
        st5, d5 = q(mon, MODULE)
        v5 = ((d5 or {}).get("data") or {}).get(KEY, "<absent>")
        print("[5] 回读 %s = %r" % (KEY, v5))

        print("[6] 未调用 config_save ⇒ 未落盘；重启后 RAM 值自动失效")

        ok = (st3 == "done" and v3 is True)
        print("\nRESULT write_verified=%s" % ("YES" if ok else "NO"))
        return 0 if ok else 3
    finally:
        print("[serial log] %s" % os.path.abspath(log))
        mon.close()


if __name__ == "__main__":
    sys.exit(main())
