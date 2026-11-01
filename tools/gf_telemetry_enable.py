#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
gf_telemetry_enable.py —— 通过项目既有配置链路启用并**落盘**遥测运行时开关

为什么不能只用 config_set
------------------------
`config_set` 只写 RAM，重启即失效。遥测要真正生效必须：
    config_set(mqtt.telemetry_enable=true) → config_save（落盘 Flash）

本脚本在**一次串口会话**内完成并逐步回读，避免"设置→重启→值丢失"的假象。

安全
----
· 只改 `mqtt.telemetry_enable` 一个键；不触碰其他键、不清 NVS。
· 不打印任何凭据（本脚本不涉及凭据）。
· 不进入 REPL，不执行动作命令。

用法
----
    python tools/gf_telemetry_enable.py            # 设置 + 落盘 + 回读
    python tools/gf_telemetry_enable.py --verify   # 只读回读（不写）
"""
import argparse
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from gf_cmd import Monitor, send, wait_result  # noqa: E402

MODULE = "mqtt"
KEY = "telemetry_enable"


def query(mon, timeout=15.0):
    cid = "tq%d" % (int(time.time() * 1000) % 100000)
    send(mon, {"cmd": "system", "id": cid, "ob": "config_query", "p": {"module": MODULE}})
    st, payload, _ = wait_result(mon, cid, timeout)
    if st != "done":
        return st, None
    return st, (payload or {}).get("data") or {}


def set_key(mon, value, timeout=15.0):
    cid = "ts%d" % (int(time.time() * 1000) % 100000)
    send(mon, {"cmd": "system", "id": cid, "ob": "config_set",
               "p": {"module": MODULE, "key": KEY, "value": value}})
    return wait_result(mon, cid, timeout)[0]


def save(mon, timeout=25.0):
    cid = "tv%d" % (int(time.time() * 1000) % 100000)
    send(mon, {"cmd": "system", "id": cid, "ob": "config_save", "p": {}})
    st, payload, _ = wait_result(mon, cid, timeout)
    return st, payload


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="COM8")
    ap.add_argument("--boot-wait", type=float, default=16.0)
    ap.add_argument("--verify", action="store_true")
    ns = ap.parse_args()

    log = ".pio/gf_telemetry_%s.log" % time.strftime("%H%M%S")
    os.makedirs(os.path.dirname(log) or ".", exist_ok=True)
    mon = Monitor(ns.port, 115200, log)
    try:
        print("[telem] 打开串口，等启动 %.0fs ..." % ns.boot_wait)
        time.sleep(ns.boot_wait)

        st, data = query(mon)
        cur = (data or {}).get(KEY, "<absent>")
        print("[1] 当前 %s.%s = %r" % (MODULE, KEY, cur))
        if ns.verify:
            return 0 if cur is True else 3

        if cur is True:
            print("[2] 已为 true，跳过写入")
        else:
            st2 = set_key(mon, True)
            print("[2] config_set true -> %s" % st2)
            time.sleep(0.4)
            st, data = query(mon)
            v2 = (data or {}).get(KEY, "<absent>")
            print("[3] 写入后回读 = %r  (期望 True)" % v2)
            if v2 is not True:
                print("RESULT: SET_FAILED")
                return 2

        st4, pl4 = save(mon)
        ok4 = (pl4 or {}).get("saved")
        print("[4] config_save -> %s  saved=%r" % (st4, ok4))
        if st4 != "done":
            print("RESULT: SAVE_FAILED")
            return 3

        time.sleep(0.6)
        st, data = query(mon)
        v5 = (data or {}).get(KEY, "<absent>")
        print("[5] 落盘后回读 = %r" % (v5,))
        print("\nRESULT: %s" % ("PERSIST_OK" if v5 is True else "PERSIST_UNKNOWN"))
        print("提示：重新打开串口（会复位）后再用 --verify 可验证跨重启持久性")
        return 0 if v5 is True else 4
    finally:
        print("[serial log] %s" % os.path.abspath(log))
        mon.close()


if __name__ == "__main__":
    sys.exit(main())
