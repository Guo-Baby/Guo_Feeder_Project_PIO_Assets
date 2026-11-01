#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""lan_enable_once.py —— 把 enable 写盘（会重启），重启后读 xiaomi_lan_last_result 判定探活是否真的跑过。

只发 config_set / config_save / query_state；**不发任何动作命令**。
"""
import json
import time

import serial

PORT, BAUD = "COM8", 115200


def drain(ser, sec):
    buf, end = [], time.time() + sec
    while time.time() < end:
        d = ser.read(4096)
        if d:
            buf.append(d.decode("utf-8", "ignore"))
        time.sleep(0.02)
    return "".join(buf)


def send(ser, obj, p, cid):
    ser.write(("cm " + json.dumps({"cmd": obj, "id": cid, "p": p}) + "\n").encode())


def main():
    ser = serial.Serial(PORT, BAUD, timeout=0.2)
    ser.setDTR(False)
    ser.rts = False
    log = drain(ser, 4.0)

    send(ser, "system", {"module": "xiaomi_lan", "key": "feeder_enable", "value": True}, "e1")
    log += drain(ser, 3.0)
    send(ser, "system", {"module": "xiaomi_lan", "key": "plug_enable", "value": True}, "e2")
    log += drain(ser, 3.0)
    send(ser, "system", {"ob": "config_save"}, "e3")
    log += drain(ser, 30.0)          # 等落盘 + 自动重启 + boot

    send(ser, "query_state", {"ob": "xiaomi_feeder_enable"}, "e4")
    log += drain(ser, 2.0)
    send(ser, "query_state", {"ob": "xiaomi_lan_last_result"}, "e5")
    log += drain(ser, 2.0)
    log += drain(ser, 65.0)          # 等一轮探活
    send(ser, "query_state", {"ob": "xiaomi_lan_last_result"}, "e6")
    log += drain(ser, 2.0)
    send(ser, "query_state", {"ob": "xiaomi_feeder_online"}, "e7")
    log += drain(ser, 2.0)
    ser.close()
    print(log)


if __name__ == "__main__":
    main()
