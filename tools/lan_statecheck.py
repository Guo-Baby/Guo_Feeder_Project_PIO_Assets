#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""lan_statecheck.py —— 读米家 LAN 模块的 7 个 state 字段（只读，不发任何动作）"""
import json
import time

import serial

PORT, BAUD = "COM8", 115200
KEYS = [
    "xiaomi_feeder_enable", "xiaomi_feeder_online", "xiaomi_feeder_out_status",
    "xiaomi_plug_enable", "xiaomi_plug_online", "xiaomi_plug_power",
    "xiaomi_lan_last_result",
]


def drain(ser, sec):
    buf = []
    end = time.time() + sec
    while time.time() < end:
        d = ser.read(4096)
        if d:
            buf.append(d.decode("utf-8", "ignore"))
        time.sleep(0.02)
    return "".join(buf)


def main():
    ser = serial.Serial(PORT, BAUD, timeout=0.2)
    ser.setDTR(False)
    ser.rts = False
    log = drain(ser, 4.0)

    for i, k in enumerate(KEYS):
        line = "cm " + json.dumps({"cmd": "query_state", "ob": k, "id": "q%d" % i})
        ser.write((line + "\n").encode())
        log += drain(ser, 1.6)

    # 再等一轮探活（60s）后复查 last_result
    log += drain(ser, 65.0)
    line = "cm " + json.dumps({"cmd": "query_state", "ob": "xiaomi_lan_last_result", "id": "q9"})
    ser.write((line + "\n").encode())
    log += drain(ser, 2.0)
    ser.close()
    print(log)


if __name__ == "__main__":
    main()
