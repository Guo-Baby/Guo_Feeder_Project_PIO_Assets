# -*- coding: utf-8 -*-
"""PC 端 MQTT 联调工具（P1.4 / P1.5 使用）。

用于在电脑侧订阅 `guo_feeder/log`、确认设备真实发出的 Log Batch，
以及向 `guo_feeder/down` 回发 `log_ack` 驱动 P1.5 的 ACK 路径。

用法:
    python test/mqtt_log_probe.py probe                 # 连接 + ACL 探测（订阅/发布）
    python test/mqtt_log_probe.py listen  [秒数]         # 订阅 guo_feeder/log 并打印（默认 30s）
    python test/mqtt_log_probe.py ack <boot> <from> <to> # 发 log_ack 到 down
    python test/mqtt_log_probe.py raw <topic> <text>     # 任意主题发一条（ACL 探测）
    python test/mqtt_log_probe.py decode <hex>           # 解出 CBOR 批次头（离线可用）

凭据默认读 `data/config/mqtt.json`，可用 --user/--pass 覆盖。
"""
import json
import os
import ssl
import sys
import time

import paho.mqtt.client as mqtt

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)


def load_conf():
    with open(os.path.join(ROOT, "data", "config", "mqtt.json"), "r", encoding="utf-8") as f:
        cfg = json.load(f)
    ca = os.path.join(ROOT, "data", "emqxsl-ca.crt")
    return cfg, ca


def make_client(client_id, user=None, pw=None, on_log=None):
    try:  # paho-mqtt 2.x
        c = mqtt.Client(mqtt.CallbackAPIVersion.VERSION1, client_id=client_id,
                        protocol=mqtt.MQTTv311)
    except AttributeError:  # paho-mqtt 1.x
        c = mqtt.Client(client_id=client_id, protocol=mqtt.MQTTv311)
    if user:
        c.username_pw_set(user, pw)
    if on_log:
        c.on_log = on_log
    return c


def connect(cfg, ca, client_id, user=None, pw=None, verbose=True):
    c = make_client(client_id, user or cfg["username"], pw or cfg["password"])
    c.tls_set(ca_certs=ca, certfile=None, keyfile=None,
              cert_reqs=ssl.CERT_REQUIRED,
              tls_version=ssl.PROTOCOL_TLS_CLIENT)
    rc = {"conn": None, "sub": []}

    def on_connect(cl, ud, flags, rcode, props=None):
        rc["conn"] = rcode

    def on_subscribe(cl, ud, mid, granted, props=None):
        rc["sub"].append((mid, granted))

    c.on_connect = on_connect
    c.on_subscribe = on_subscribe
    c.connect(cfg["mqtt_server"], int(cfg["mqtt_port"]), keepalive=30)
    c.loop_start()
    t0 = time.time()
    while rc["conn"] is None and time.time() - t0 < 10:
        time.sleep(0.05)
    if verbose:
        print("[probe] CONNACK rcode =", rc["conn"], "(0 = 成功)")
    return c, rc


def cmd_probe(cfg, ca):
    print("=== 目标 EMQX ===")
    print("  host =", cfg["mqtt_server"], "port =", cfg["mqtt_port"])
    print("  user =", cfg["username"])
    print("  down =", cfg["subscribe_topic"], " up =", cfg["publish_topic"])
    print()

    # ① 订阅 guo_feeder/log（验证 subscribe ACL）
    c, rc = connect(cfg, ca, "wb_probe_sub")
    got = []
    c.on_message = lambda cl, ud, msg: got.append((msg.topic, msg.payload))
    c.subscribe("guo_feeder/log", qos=1)
    c.subscribe("guo_feeder/down", qos=1)
    t0 = time.time()
    while len(rc["sub"]) < 2 and time.time() - t0 < 8:
        time.sleep(0.05)
    print("[probe] SUBACK:", rc["sub"], "(granted 128 表示失败)")
    print()

    # ② 尝试发布 guo_feeder/log（验证 publish ACL）
    payload = b"\xa1\x00\x02"  # 极小 CBOR map {0:2}
    info = c.publish("guo_feeder/log", payload, qos=1)
    info.wait_for_publish(timeout=5)
    print("[probe] PUBLISH guo_feeder/log rc =", info.rc, "(0 = 已受理)")
    time.sleep(1.0)
    print("[probe] 自发自收 =", "OK" if any(t == "guo_feeder/log" for t, _ in got) else "未收到")
    c.disconnect()
    c.loop_stop()


def cmd_listen(cfg, ca, secs=30):
    c, rc = connect(cfg, ca, "wb_probe_listen")
    n = [0]

    def on_message(cl, ud, msg):
        n[0] += 1
        print("[log #%d] topic=%s len=%d" % (n[0], msg.topic, len(msg.payload)))
        print("  hex:", msg.payload.hex())
        if msg.topic == "guo_feeder/log":
            print("  " + repr(cbor_decode_batch(msg.payload)))
        sys.stdout.flush()

    c.on_message = on_message
    c.subscribe("guo_feeder/log", qos=1)
    t0 = time.time()
    while len(rc["sub"]) < 1 and time.time() - t0 < 8:
        time.sleep(0.05)
    print("[probe] 已订阅 guo_feeder/log, SUBACK =", rc["sub"], "监听", secs, "秒 ...")
    time.sleep(secs)
    print("[probe] 共收到", n[0], "条")
    c.disconnect()
    c.loop_stop()


# =====================================================
# 极简 CBOR 解码器：只覆盖设备实际发出的子集
#   array(n) / unsigned int / byte string
# 刻意不依赖 cbor2，避免额外安装
# =====================================================

BATCH_KEYS = ["fmt", "event_dict_ver", "boot_seq", "seq_from", "seq_to", "count",
              "drop_ring", "drop_overflow", "drop_unacked", "self_degraded",
              "flags", "records"]


def _cbor_read(data, pos):
    """返回 (value, new_pos)。bstr 返回 bytes，array 返回 list。"""
    ib = data[pos]
    pos += 1
    major = ib >> 5
    ai = ib & 0x1F
    if ai < 24:
        n = ai
    elif ai == 24:
        n = data[pos]; pos += 1
    elif ai == 25:
        n = (data[pos] << 8) | data[pos + 1]; pos += 2
    elif ai == 26:
        n = (data[pos] << 24) | (data[pos + 1] << 16) | (data[pos + 2] << 8) | data[pos + 3]
        pos += 4
    else:
        raise ValueError("unsupported additional info %d" % ai)

    if major == 0:
        return n, pos
    if major == 2:
        return data[pos:pos + n], pos + n
    if major == 4:
        out = []
        for _ in range(n):
            v, pos = _cbor_read(data, pos)
            out.append(v)
        return out, pos
    raise ValueError("unsupported major type %d" % major)


def cbor_decode_batch(payload):
    """解出批次头 dict；records 只报条数与首条前 16 字节（避免刷屏）。"""
    arr, _ = _cbor_read(payload, 0)
    head = {}
    for i, k in enumerate(BATCH_KEYS):
        v = arr[i]
        if k == "records":
            head["records"] = "%d 条" % len(v)
            head["rec0_head"] = v[0][:16].hex() if v else None
        else:
            head[k] = v
    return head


def cmd_decode(hexstr):
    data = bytes.fromhex(hexstr.replace(" ", "").replace("\n", ""))
    print("[probe] 批次长度 =", len(data), "B")
    print(json.dumps(cbor_decode_batch(data), ensure_ascii=False, indent=2))


def cmd_ack(cfg, ca, boot, frm, to):
    c, rc = connect(cfg, ca, "wb_probe_ack")
    msg = json.dumps({"c": "log_ack", "i": "ack%d" % int(time.time()),
                      "p": {"b": int(boot), "f": int(frm), "t": int(to)}})
    info = c.publish("guo_feeder/down", msg, qos=1)
    info.wait_for_publish(timeout=5)
    print("[probe] PUBLISH down rc =", info.rc, "msg =", msg)
    time.sleep(1.0)
    c.disconnect()
    c.loop_stop()


def cmd_raw(cfg, ca, topic, text):
    c, rc = connect(cfg, ca, "wb_probe_raw")
    info = c.publish(topic, text, qos=1)
    info.wait_for_publish(timeout=5)
    print("[probe] PUBLISH", topic, "rc =", info.rc)
    time.sleep(1.0)
    c.disconnect()
    c.loop_stop()


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return
    cfg, ca = load_conf()
    op = sys.argv[1]
    if op == "probe":
        cmd_probe(cfg, ca)
    elif op == "listen":
        cmd_listen(cfg, ca, int(sys.argv[2]) if len(sys.argv) > 2 else 30)
    elif op == "ack":
        cmd_ack(cfg, ca, sys.argv[2], sys.argv[3], sys.argv[4])
    elif op == "raw":
        cmd_raw(cfg, ca, sys.argv[2], sys.argv[3])
    elif op == "decode":
        cmd_decode(sys.argv[2])
    else:
        print(__doc__)


if __name__ == "__main__":
    main()
