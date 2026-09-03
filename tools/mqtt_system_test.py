#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
Guo Feeder SystemCommand MQTT 测试客户端（仅只读 + 安全重启，绝不改参数）

下发三条 V1 系统命令，监听设备上行回包：
  - system.memory  (i=2001)  Internal RAM / External PSRAM
  - system.flash   (i=2002)  Internal/External Flash + 递归文件列表
  - system.restart (i=2003)  安全重启（复用 ConfigManager 10s 窗口，末尾发送）

restart 放在最后：下发后设备立即回 accepted，10s 后 ESP.restart()，
本客户端会在 TAIL 窗口内看到设备 MQTT 掉线再重新上线（online）。
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
CLIENT_ID = "workbuddy_sys_%d" % int(time.time())

# 只读 + 安全重启命令（按 memory / flash / restart 顺序；restart 最后）
COMMANDS = [
    {"c": "system", "i": "2001", "p": {"o": "memory"}},
    {"c": "system", "i": "2002", "p": {"o": "flash"}},
    {"c": "system", "i": "2003", "p": {"o": "restart"}},
]

WAIT_BEFORE_PUBLISH = 25   # 设备启动 + WiFi + MQTT 连接
BETWEEN = 15               # 每条命令响应窗口
TAIL = 30                  # restart 后 10s 重启 + 重连监听


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
    tag, body = dec[0], dec[1]
    print("=" * 60)
    print("[UP][%s] topic=%s" % (tag, msg.topic))
    if tag == "raw":
        print(body)
    else:
        print(json.dumps(body, ensure_ascii=False, indent=2)
              if isinstance(body, (dict, list)) else body)
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

    print("[TEST] waiting %ds for device boot & MQTT connect..." % WAIT_BEFORE_PUBLISH)
    time.sleep(WAIT_BEFORE_PUBLISH)

    for idx, cmd in enumerate(COMMANDS):
        payload = json.dumps(cmd)
        info = client.publish(PUB, payload, qos=1)
        print("[DOWN] -> %s (mid=%s)" % (payload, info.mid))
        if idx < len(COMMANDS) - 1:
            time.sleep(BETWEEN)

    print("[TEST] tail listening %ds (capture restart + reconnect)..." % TAIL)
    time.sleep(TAIL)
    client.loop_stop()
    print("[TEST] done")


if __name__ == "__main__":
    main()
