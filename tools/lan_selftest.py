#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
lan_selftest.py —— 米家 LAN 模块上机自检（**不触发任何物理动作**）

安全边界（硬性）：
  · 只发 query_state / query_actions / config_set / system.lan_prov
  · **从不**发 XIAOMI_FEED / XIAOMI_PLUG_ON / XIAOMI_PLUG_OFF
  · 探测请求是 get_properties（只读）
  · 不打印任何 Token（只打印 ok/失败码）

判定用途：
  1) 模块是否加载、3 个 Action 是否注册
  2) 7 个 system state 字段是否出现
  3) lan_prov 校验（非法 token 必须被拒）
  4) 合法格式 token 落 NVS 后，探活是否按"失败即离线、不重试"工作
"""
import sys
import time

import serial

PORT = "COM8"
BAUD = 115200

# 合成 Token（32 hex，**格式合法但非真实值**）：用于验证存储与失败路径，
# 绝不会让真机执行任何动作（设备校验和不符 ⇒ 直接丢弃）。
SYNTH_TOKEN = "0" * 32

# ★ [2026-11-04] IP 不再是配置项：设备广播发现并只存 RAM，脚本不再传 IP。
#   （本机独立交叉验证仍可用 MiLAN_Test 的 devices.local.json。）


def drain(ser, seconds, sink):
    end = time.time() + seconds
    while time.time() < end:
        data = ser.read(4096)
        if data:
            sink.append(data.decode("utf-8", "ignore"))


def wait_quiet(ser, sink, quiet=1.2, timeout=20.0):
    """等串口静默（表示命令处理完）"""
    start = time.time()
    last = time.time()
    while time.time() - start < timeout:
        data = ser.read(4096)
        if data:
            sink.append(data.decode("utf-8", "ignore"))
            last = time.time()
        elif time.time() - last > quiet:
            return
        time.sleep(0.05)


def send(ser, sink, obj, p=None, cid="t1"):
    payload = {"cmd": obj.split("/")[0], "id": cid}
    if "/" in obj:
        payload["ob"] = obj.split("/")[1]
    if p is not None:
        payload["p"] = p
    line = "cm " + __import__("json").dumps(payload, ensure_ascii=False)
    ser.write((line + "\n").encode("utf-8"))
    wait_quiet(ser, sink)


def main():
    sink = []
    ser = serial.Serial(PORT, BAUD, timeout=0.2)
    ser.setDTR(False)
    ser.rts = False
    drain(ser, 4.0, sink)          # boot 日志

    # 1) 模块加载 + Action 注册
    send(ser, sink, "query_actions", cid="s1")
    # 2) state 字段
    send(ser, sink, "query_state", cid="s2")
    # 3) 非法 token 必须被拒
    send(ser, sink, "system/lan_prov",
         {"dev": "feeder", "token": "zz"}, cid="s3")
    # 4) 合法格式（合成）token 写 NVS（★ 不再传 ip）
    send(ser, sink, "system/lan_prov",
         {"dev": "feeder", "token": SYNTH_TOKEN}, cid="s4")
    send(ser, sink, "system/lan_prov",
         {"dev": "plug", "token": SYNTH_TOKEN}, cid="s5")
    # 5) 打开 enable（RAM only，不落盘 ⇒ 不触发重启）
    send(ser, sink, "system/config_set",
         {"module": "xiaomi_lan", "key": "feeder_enable", "value": True}, cid="s6")
    send(ser, sink, "system/config_set",
         {"module": "xiaomi_lan", "key": "plug_enable", "value": True}, cid="s7")

    # 6) 等一轮探活（60s 周期 + 超时 0.8/3s）
    drain(ser, 75.0, sink)

    # 7) 再读状态
    send(ser, sink, "query_state", cid="s8")

    ser.close()
    print("".join(sink))


if __name__ == "__main__":
    sys.exit(main())
