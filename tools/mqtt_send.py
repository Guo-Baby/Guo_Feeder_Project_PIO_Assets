#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
Guo Feeder 通用 MQTT 下发/抓包工具

用法:
    python tools/mqtt_send.py '<json1>' '<json2>' ...

示例（只读查询）:
    python tools/mqtt_send.py "{\"c\":\"system\",\"i\":\"3001\",\"p\":{\"o\":\"memory\"}}"

可选环境变量:
    WAIT_CONNECT  连接后等待秒数（默认 3；设备已在线时无需长等）
    BETWEEN       两条命令之间的响应窗口秒数（默认 8）
    TAIL          最后一条命令后的监听秒数（默认 8）

安全提醒: 本工具不做命令内容校验。请勿用它下发改参/写入类命令，
除非明确知道后果（config_set / config_module / config_reset 等属写操作）。
"""
import json
import os
import ssl
import sys
import time

BROKER = "n302933b.ala.cn-hangzhou.emqxsl.cn"
PORT = 8883
CA = r"D:\Guo_Feeder_Project\PIO_Assets\Guo_Feeder_Project\data\emqxsl-ca.crt"
USER = "workbuddy"
PASS = "workbuddy"
# --- P0-5：Topic V3 参数化（topic 含 device_id；**刻意不提供通配回退**）---
def _strip_device_id_args(argv):
    """剔除 --device-id <id> / --device-id=<id>，返回其余位置参数。"""
    out, i = [], 0
    while i < len(argv):
        a = argv[i]
        if a == "--device-id":
            i += 2
            continue
        if a.startswith("--device-id="):
            i += 1
            continue
        out.append(a)
        i += 1
    return out


def _require_device_id():
    """设备 ID：--device-id <id> > 环境变量 GF_DEVICE_ID；缺失则退出。

    Topic V3 的 topic 形如 guo_feeder/<device_id>/...，必须显式指定。
    **不提供 guo_feeder/+/up 通配回退**：那会造成多设备混流假象，
    而且 `+` 占整整一级，并不能匹配 legacy 的 guo_feeder/up。
    """
    argv = sys.argv[1:]
    did = ""
    for i, a in enumerate(argv):
        if a == "--device-id" and i + 1 < len(argv):
            did = argv[i + 1]
        elif a.startswith("--device-id="):
            did = a.split("=", 1)[1]
    did = (did or os.environ.get("GF_DEVICE_ID", "")).strip()
    if not did:
        print("[ERR] 缺少设备 ID：请用 --device-id <id> 或环境变量 GF_DEVICE_ID")
        print("      原因：Topic V3 的 topic 含 device_id，必须显式指定。")
        sys.exit(2)
    return did


DEVICE_ID = _require_device_id()
SUB = "guo_feeder/%s/up" % DEVICE_ID
PUB = "guo_feeder/%s/down" % DEVICE_ID
CLIENT_ID = "workbuddy_send_%d" % int(time.time())

WAIT_CONNECT = int(os.environ.get("WAIT_CONNECT", "3"))
BETWEEN = int(os.environ.get("BETWEEN", "8"))
TAIL = int(os.environ.get("TAIL", "8"))


def try_decode(payload):
    try:
        return ("json", json.loads(payload))
    except Exception:
        pass
    try:
        import cbor2
        return ("cbor", cbor2.loads(payload))
    except Exception:
        pass
    return ("raw", payload)


def pick_client():
    try:
        from paho.mqtt.client import Client as MQTTClient, CallbackAPIVersion
        return MQTTClient(CallbackAPIVersion.VERSION2, client_id=CLIENT_ID)
    except Exception:
        from paho.mqtt.client import Client as MQTTClient
        return MQTTClient(client_id=CLIENT_ID)


def on_connect(client, userdata, flags, rc, *a):
    print("[TEST] connected rc=%s" % str(rc))
    if rc != 0:
        print("[TEST] !! connect failed, check network/credentials/CA")
        return
    client.subscribe(SUB, qos=1)
    print("[TEST] subscribed %s (qos=1)" % SUB)


def on_message(client, userdata, msg):
    tag, body = try_decode(msg.payload)[0], try_decode(msg.payload)[1]
    print("=" * 60)
    print("[UP][%s] topic=%s" % (tag, msg.topic))
    if tag == "raw":
        print(body)
    else:
        print(json.dumps(body, ensure_ascii=False, indent=2)
              if isinstance(body, (dict, list)) else body)
    print("=" * 60)


def main():
    cmds = _strip_device_id_args(sys.argv[1:])
    if not cmds:
        print(__doc__)
        sys.exit(1)

    client = pick_client()
    client.username_pw_set(USER, PASS)
    try:
        client.tls_set(ca_certs=CA, cert_reqs=ssl.CERT_REQUIRED)
    except Exception as e:
        print("[TEST] tls_set failed: %s" % e)
        sys.exit(1)
    client.on_connect = on_connect
    client.on_message = on_message

    print("[TEST] connecting %s:%d as %s" % (BROKER, PORT, CLIENT_ID))
    try:
        client.connect(BROKER, PORT, keepalive=60)
    except Exception as e:
        print("[TEST] connect exception: %s" % e)
        sys.exit(1)
    client.loop_start()
    time.sleep(WAIT_CONNECT)

    for idx, raw in enumerate(cmds):
        info = client.publish(PUB, raw, qos=1)
        print("[DOWN] -> %s (mid=%s)" % (raw, info.mid))
        if idx < len(cmds) - 1:
            time.sleep(BETWEEN)

    time.sleep(TAIL)
    client.loop_stop()
    print("[TEST] done")


if __name__ == "__main__":
    main()
