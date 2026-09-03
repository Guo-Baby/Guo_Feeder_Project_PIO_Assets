#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
Guo Feeder MQTT 测试客户端（仅用于联调，不修改设备配置）

作用：
  - 作为"云端"角色连接到 EMQX broker（TLS）
  - 订阅设备上行 topic (guo_feeder/up)，打印设备回包
  - 向设备下行 topic (guo_feeder/down) 下发只读查询命令

本脚本只下发读-only 命令（config_query keys_only），绝不改参数。

上行解码：优先按 JSON 解析；若失败（理论上不会，当前固件上行是紧凑 JSON 文本）
再尝试 CBOR，最后原样打印。

用法：
  python tools/mqtt_test_client.py
"""
import json
import ssl
import sys
import time

BROKER = "n302933b.ala.cn-hangzhou.emqxsl.cn"
PORT = 8883
CA = r"D:\Guo_Feeder_Project\PIO_Assets\Guo_Feeder_Project\data\emqxsl-ca.crt"
USER = "workbuddy"
PASS = "workbuddy"
SUB = "guo_feeder/up"
PUB = "guo_feeder/down"
CLIENT_ID = "workbuddy_test_%d" % int(time.time())

# 只读查询命令（keys_only，查全部模块名）。主命令 i=1001，兜底 i=10011。
# 用两个不同 id 是为了规避设备端 cmd_id 去重：若主命令在设备连上 MQTT 之前
# 已下发且丢失，兜底命令用不同 id 仍能被设备处理。
COMMANDS = [
    {"c": "system", "i": "1001", "p": {"o": "config_query", "keys_only": True}},
    {"c": "system", "i": "10011", "p": {"o": "config_query", "keys_only": True}},
]

# 设备启动到连上 WiFi+MQTT 的等待时间（秒）
WAIT_BEFORE_PUBLISH = 20
# 每条命令之间留出的响应窗口（秒）
BETWEEN = 18
# 末尾额外监听时间（秒）
TAIL = 15


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
    dec = try_decode(msg.payload)
    tag = dec[0]
    body = dec[1]
    print("=" * 60)
    print("[UP][%s] topic=%s" % (tag, msg.topic))
    if tag == "raw":
        print(body)
    else:
        print(json.dumps(body, ensure_ascii=False, indent=2) if isinstance(body, (dict, list)) else body)
    print("=" * 60)


def on_publish(client, userdata, mid, *a):
    print("[TEST] published mid=%s" % mid)


def main():
    client = pick_client()
    client.username_pw_set(USER, PASS)
    try:
        client.tls_set(ca_certs=CA, cert_reqs=ssl.CERT_REQUIRED)
    except Exception as e:
        print("[TEST] tls_set failed: %s" % e)
        sys.exit(1)
    client.on_connect = on_connect
    client.on_message = on_message
    client.on_publish = on_publish

    print("[TEST] connecting %s:%d as %s" % (BROKER, PORT, CLIENT_ID))
    try:
        client.connect(BROKER, PORT, keepalive=60)
    except Exception as e:
        print("[TEST] connect exception: %s" % e)
        sys.exit(1)
    client.loop_start()

    print("[TEST] waiting %ds for device to boot & connect MQTT..." % WAIT_BEFORE_PUBLISH)
    time.sleep(WAIT_BEFORE_PUBLISH)

    for idx, cmd in enumerate(COMMANDS):
        payload = json.dumps(cmd)
        info = client.publish(PUB, payload, qos=1)
        print("[DOWN] -> %s (mid=%s)" % (payload, info.mid))
        if idx < len(COMMANDS) - 1:
            time.sleep(BETWEEN)

    print("[TEST] tail listening %ds for late responses..." % TAIL)
    time.sleep(TAIL)
    client.loop_stop()
    print("[TEST] done")


if __name__ == "__main__":
    main()
