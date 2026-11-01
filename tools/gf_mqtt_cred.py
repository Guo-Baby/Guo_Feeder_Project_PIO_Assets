#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
gf_mqtt_cred.py —— 从设备读取 MQTT 连接信息并落到**临时文件**（供诊断进程使用）

安全纪律
--------
· **绝不打印** password / username 原文；只打印长度与是否为空。
· 落盘文件权限尽量收紧；调用方用完必须删除。
· 只读 `config_query mqtt`，不写任何配置。

用法
----
    python tools/gf_mqtt_cred.py --out /tmp/gf_mq.json
"""
import argparse
import json
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from gf_cmd import Monitor, send, wait_result  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="COM8")
    ap.add_argument("--out", required=True)
    ap.add_argument("--boot-wait", type=float, default=16.0)
    ns = ap.parse_args()

    log = ".pio/gf_cred_%s.log" % time.strftime("%H%M%S")
    os.makedirs(os.path.dirname(log) or ".", exist_ok=True)
    mon = Monitor(ns.port, 115200, log)
    try:
        print("[cred] 打开串口（设备会复位），等待 %.0fs ..." % ns.boot_wait)
        time.sleep(ns.boot_wait)

        cid = "c%d" % (int(time.time() * 1000) % 100000)
        send(mon, {"cmd": "system", "id": cid, "ob": "config_query", "p": {"module": "mqtt"}})
        st, payload, _ = wait_result(mon, cid, 15.0)
        if st != "done":
            print("[cred] config_query 失败: %s" % st)
            return 2

        data = (payload or {}).get("data") or {}
        need = ["mqtt_server", "mqtt_port", "client_id", "username", "password",
                "subscribe_topic", "publish_topic", "log_topic"]
        cred = {}
        for k in need:
            v = data.get(k)
            cred[k] = v if v is not None else ""
        missing = [k for k in ("mqtt_server", "username", "password", "publish_topic", "subscribe_topic")
                   if not cred.get(k)]
        if missing:
            print("[cred] 配置缺字段: %s" % ",".join(missing))
            return 3

        with open(ns.out, "w", encoding="utf-8") as f:
            json.dump(cred, f)
        try:
            os.chmod(ns.out, 0o600)
        except Exception:
            pass

        print("[cred] 已写入 %s" % ns.out)
        print("[cred] server=%s port=%s client_id=%s"
              % (cred["mqtt_server"], cred["mqtt_port"], cred["client_id"]))
        # 只报长度，不报任何凭据内容
        print("[cred] username_len=%d password_len=%d"
              % (len(cred["username"]), len(cred["password"])))
        print("[cred] up_topic=%s down_topic=%s"
              % (cred["publish_topic"], cred["subscribe_topic"]))
        return 0
    finally:
        mon.close()


if __name__ == "__main__":
    sys.exit(main())
