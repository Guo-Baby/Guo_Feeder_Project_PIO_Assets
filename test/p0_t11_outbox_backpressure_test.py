#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""p0_t11_outbox_backpressure_test.py —— T11 §三/§五D：MQTT Outbox 背压与下行吞吐

P4 已记录的真机异常
--------------------------------------------------------------------
`MQTT outbox` 一度涨到 **5997 B**，期间 `esp_mqtt_client_enqueue(store=1)`
让主循环停摆约 20 s；网络恢复后同一用例复跑全绿。
当时只登记为"观察项"，**没有修** —— 本任务要求把这条发送路径修掉。

代码级事实（可核）
--------------------------------------------------------------------
① 日志批与命令结果**共用同一个 outbox**，且都是 `qos=1 / store=true`；
   outbox 是 FIFO ⇒ 结果帧排在日志批后面；
② 这个 IDF 版本（4.4 / Arduino 2.0.17）的头文件里**没有** `outbox_limit`，
   也就是"满了会拒绝"并不存在 —— outbox 只进不出，实测涨到 5997 B；
③ `network_timeout_ms` 默认 10 s：mqtt 任务在慢网络下持锁做阻塞 socket 写，
   而 `esp_mqtt_client_enqueue()` 要拿同一把 api 锁 ⇒ 主循环被拖长；
④ `cloud_process_rx_queue()` 旧实现**每轮只出 1 条**，环只有 4 槽
   ⇒ 突发时后续下行**静默丢弃**（QoS1 的 PUBACK 已被 esp-mqtt 回掉，
   broker 不会重投 ⇒ 命令永久丢失）。

修复取向（任务书明确禁止的写法已避开）
--------------------------------------------------------------------
· 不扩容 outbox、不扩容接收环（不靠内存掩盖处理能力）；
· 不静默丢日志、不静默丢结果；
· 发送失败**不得**触发非幂等命令的自动重发。

覆盖：D-1..D-8 + 负向探针

用法：python test/p0_t11_outbox_backpressure_test.py
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CLOUD = os.path.join(ROOT, "src", "cloud", "cloud_manager.cpp")

if not os.path.exists(CLOUD):
    print("FATAL 缺少源文件: " + CLOUD)
    sys.exit(2)

raw = open(CLOUD, encoding="utf-8", errors="replace").read()
TEXT = re.sub(r"/\*.*?\*/", "", raw, flags=re.S)
TEXT = re.sub(r"//[^\n]*", "", TEXT)

passes = [0]
failures = []


def ok(name, cond, detail=""):
    if cond:
        passes[0] += 1
        print("  PASS  " + name)
    else:
        failures.append(name + (("  <- " + detail) if detail else ""))
        print("  FAIL  " + name + (("  <- " + detail) if detail else ""))


def func_body(text, name):
    """取函数**定义**体（跳过前置声明）。见 p0_t11_command_queue_and_safety_test.py 同名说明。"""
    pat = re.compile(r"\b" + re.escape(name) + r"\s*\(")
    for m in pat.finditer(text):
        i = m.end()
        depth = 1
        while i < len(text) and depth > 0:
            if text[i] == "(":
                depth += 1
            elif text[i] == ")":
                depth -= 1
            i += 1
        j = i
        while j < len(text) and text[j] in " \t\r\n":
            j += 1
        if j >= len(text) or text[j] != "{":
            continue
        depth2 = 0
        for k in range(j, len(text)):
            if text[k] == "{":
                depth2 += 1
            elif text[k] == "}":
                depth2 -= 1
                if depth2 == 0:
                    return text[j:k + 1]
    return None


def macro_int(name, text=None):
    """读宏的整数值。

    ★ 必须支持传入 text：负向探针要把"被改坏的文本"传进来求值；
      首版这里硬读模块级 TEXT ⇒ 探针永远看到未改坏的 4，
      探针恒绿、等于没测（本文件首跑即踩到）。
    """
    src = TEXT if text is None else text
    m = re.search(r"#define\s+" + name + r"\s+\(?\s*([0-9]+)", src)
    return int(m.group(1)) if m else None


print("=" * 68)
print("p0_t11_outbox_backpressure_test —— Outbox 背压 / 下行吞吐")
print("=" * 68)

# --------------------------------------------------------------- D-1
print("\n[D-1] 高水位阈值（背压的判据来源）")
hw = macro_int("CLOUD_OUTBOX_HIGH_WATER_BYTES")
ok("D-1a 定义了 CLOUD_OUTBOX_HIGH_WATER_BYTES", hw is not None, "hw=%s" % hw)
ok("D-1b 阈值是有限正数且小于常见 outbox 观测峰值",
   hw is not None and 1024 <= hw <= 8192, "hw=%s" % hw)

# --------------------------------------------------------------- D-2
print("\n[D-2] 日志必须给命令结果让路")
log_body = func_body(TEXT, "cloud_send_log") or ""
ok("D-2a cloud_send_log 存在", log_body != "")
ok("D-2b 日志发送前读 outbox 尺寸",
   "esp_mqtt_client_get_outbox_size" in log_body)
ok("D-2c 达高水位即返回 false（不硬塞）",
   re.search(r"if\s*\(ob\s*>=\s*CLOUD_OUTBOX_HIGH_WATER_BYTES\)", log_body) is not None)
ok("D-2d 让路发生在真正 publish 之前",
   0 <= log_body.find("CLOUD_OUTBOX_HIGH_WATER_BYTES") < log_body.find("cloud_mqtt_publish_binary"),
   "gate@%d pub@%d" % (log_body.find("CLOUD_OUTBOX_HIGH_WATER_BYTES"),
                       log_body.find("cloud_mqtt_publish_binary")))

# --------------------------------------------------------------- D-3
print("\n[D-3] 让路 = 延后，不是丢弃（日志数据不得丢）")
ok("D-3a 让路分支只 return false（交给 LogManager 重发）",
   re.search(r"return false;\s*\}", log_body) is not None)
ok("D-3b 让路分支里没有任何「丢弃/清空」动作",
   all(k not in log_body for k in ("clear()", "discard", "drop_record")))

# --------------------------------------------------------------- D-4
print("\n[D-4] 命令结果**绝不**被让路（关键安全语义）")
up_body = func_body(TEXT, "cloud_send_up") or ""
res_body = func_body(TEXT, "on_command_result") or ""
ok("D-4a cloud_send_up 存在", up_body != "")
ok("D-4b 结果发送路径**不含**高水位门禁",
   "CLOUD_OUTBOX_HIGH_WATER_BYTES" not in up_body)
ok("D-4c on_command_result 不含高水位门禁（结果必须发）",
   "CLOUD_OUTBOX_HIGH_WATER_BYTES" not in res_body)
ok("D-4d 结果发送失败仍如实打印（不静默）",
   '"publish FAIL"' in TEXT or "[Cloud UP] FAIL" in TEXT)

# --------------------------------------------------------------- D-5
print("\n[D-5] 让路必须可观测")
ok("D-5a 有累计计数 s_log_deferred_count", "s_log_deferred_count" in TEXT)
ok("D-5b 有 5 s 节流上报（不刷屏）",
   re.search(r"s_log_deferred_last_ms\)\s*>=\s*5000", TEXT) is not None)
ok("D-5c 上报内容含当前 outbox 尺寸与阈值",
   "outbox=%d >= %d" in TEXT)

# --------------------------------------------------------------- D-6
print("\n[D-6] 下行吞吐：每轮多条 + 丢弃可观测")
drain = macro_int("MQTT_RX_DRAIN_PER_LOOP")
slots = macro_int("MQTT_RX_SLOT_COUNT")
ok("D-6a 定义了每轮处理量 MQTT_RX_DRAIN_PER_LOOP", drain is not None, "drain=%s" % drain)
ok("D-6b 每轮处理量 > 1（旧实现是 1，正是瓶颈）", drain is not None and drain > 1)
ok("D-6c 接收环槽数**未**被无脑扩容（内存约束）",
   slots is not None and slots <= 4, "slots=%s" % slots)

rx_body = func_body(TEXT, "cloud_process_rx_queue") or ""
ok("D-6d 处理循环按 DRAIN 上限逐条出队",
   "MQTT_RX_DRAIN_PER_LOOP" in rx_body and "cloud_rx_dequeue" in rx_body)

enq_body = func_body(TEXT, "cloud_rx_enqueue") or ""
ok("D-6e 环满有累计计数 s_rx_drop_count", "s_rx_drop_count" in enq_body)
ok("D-6f 环满上报有 5 s 节流", re.search(r"s_rx_drop_last_ms\)\s*>=\s*5000", enq_body) is not None)
ok("D-6g 非法长度也计数（s_rx_bad_len）", "s_rx_bad_len" in enq_body)

# --------------------------------------------------------------- D-7
print("\n[D-7] 发送失败**不得**触发非幂等命令自动重发")
pub = func_body(TEXT, "cloud_mqtt_publish_binary") or ""
pub2 = func_body(TEXT, "cloud_mqtt_publish_text") or ""
ok("D-7a publish 失败只记计数并返回 false",
   "cloud_note_publish_fail" in pub and "return false" in pub)
bad = ("workflow_enqueue_action", "command_manager_execute", "command_execute_action",
       "safety_stop_all", "startRequest")
ok("D-7b 发送路径不调用任何执行/入队入口",
   all(k not in pub and k not in pub2 for k in bad))
ok("D-7c 不存在「失败后重发上一条命令」的状态（无 last_cmd 缓存）",
   "last_cmd" not in TEXT and "resend_cmd" not in TEXT)

# --------------------------------------------------------------- D-8
print("\n[D-8] 不靠「加超时/加容量」掩盖")
ok("D-8a 没有放宽 network_timeout_ms 来回避问题",
   "network_timeout_ms" not in TEXT,
   "若确实需要修改，请显式评审而不是悄悄放大")
ok("D-8b 槽数与每轮处理量是两个独立宏，且各自用在自己的位置",
   macro_int("MQTT_RX_SLOT_COUNT") is not None
   and macro_int("MQTT_RX_DRAIN_PER_LOOP") is not None
   and "MQTT_RX_DRAIN_PER_LOOP" in (func_body(TEXT, "cloud_process_rx_queue") or "")
   and "MQTT_RX_SLOT_COUNT" in (func_body(TEXT, "cloud_rx_enqueue") or ""))

# --------------------------------------------------------------- 负向探针
print("\n[N] 负向探针（证明断言可失败）")


def probe(name, mutated, detector):
    ok(name, detector(mutated) is False, "探针未被识别 ⇒ 断言形同虚设")


probe("N-1 去掉日志高水位让路后能被识别",
      TEXT.replace("if(ob >= CLOUD_OUTBOX_HIGH_WATER_BYTES)", "if(false)"),
      lambda t: re.search(r"if\s*\(ob\s*>=\s*CLOUD_OUTBOX_HIGH_WATER_BYTES\)",
                          func_body(t, "cloud_send_log") or "") is not None)

probe("N-2 把每轮处理量改回 1 后能被识别",
      re.sub(r"#define MQTT_RX_DRAIN_PER_LOOP\s+4",
             "#define MQTT_RX_DRAIN_PER_LOOP  1", TEXT),
      lambda t: (macro_int("MQTT_RX_DRAIN_PER_LOOP", t) or 0) > 1)

_enq = func_body(TEXT, "cloud_rx_enqueue") or ""
probe("N-3 去掉环满计数后能被识别",
      TEXT.replace(_enq, _enq.replace("s_rx_drop_count", "__removed__")),
      lambda t: "s_rx_drop_count" in (func_body(t, "cloud_rx_enqueue") or ""))

print("\n" + "=" * 68)
print("PASS %d / FAIL %d" % (passes[0], len(failures)))
if failures:
    for f in failures:
        print("  ! " + f)
    return_code = 1
else:
    return_code = 0
sys.exit(return_code)
