#!/usr/bin/env python3
# =====================================================
# P2-2 · Transport Facade —— 静态结构校验
#
# 断言 P2-0 冻结的架构边界与"业务路径不变"在**源码层面**成立：
#   1) Facade header 不暴露 esp-mqtt / ArduinoJson / Homie / CloudManager 类型
#   2) 依赖方向单向（CloudManager 不 include mqtt_transport / homie；Facade 不碰 esp_mqtt）
#   3) 全固件只有 1 个 esp_mqtt_client（init / register_event 各 1 处）
#   4) 业务下行管线逐项保留（translate / ACK / dedupe / change_msg_limit / log_ack）
#   5) 业务 subscribe 调用逐字保留
#   6) foreign handler 只在 loop 上下文执行（不在 mqtt_event_handler 内）
#   7) P1-7 production topic enforcement 保留
#   8) retain 默认 false（业务语义不变）
#   9) 无新增 delay()
#
# 用法: python test/p2_2_transport_check.py
# 退出码: 0 = 全部 PASS；1 = 有 FAIL
# =====================================================
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "src")

results = []


def check(name, ok, detail=""):
    results.append((name, bool(ok), detail))


def read(rel):
    with open(os.path.join(ROOT, rel), "r", encoding="utf-8") as f:
        return f.read()


def strip_code(text):
    """去掉注释与字符串/字符字面量内容，避免字面量/注释造成假命中。"""
    out = []
    i = 0
    n = len(text)
    while i < n:
        c = text[i]
        if c == '/' and i + 1 < n and text[i + 1] == '/':
            while i < n and text[i] != '\n':
                i += 1
            continue
        if c == '/' and i + 1 < n and text[i + 1] == '*':
            i += 2
            while i + 1 < n and not (text[i] == '*' and text[i + 1] == '/'):
                i += 1
            i += 2
            continue
        if c == '"' or c == "'":
            quote = c
            i += 1
            while i < n and text[i] != quote:
                if text[i] == '\\':
                    i += 1
                i += 1
            i += 1
            out.append('""')
            continue
        out.append(c)
        i += 1
    return "".join(out)


def extract_body(src, needle):
    # 注意：同一签名可能先有**前置声明**，故取**最后一次**出现（定义处）。
    i = src.rfind(needle)
    j = src.index('{', i)
    depth = 0
    k = j
    while k < len(src):
        if src[k] == '{':
            depth += 1
        elif src[k] == '}':
            depth -= 1
            if depth == 0:
                return src[i:k + 1]
        k += 1
    return src[i:]


def gather_src():
    files = {}
    for dirpath, _dirs, names in os.walk(SRC):
        for nm in names:
            if nm.endswith((".cpp", ".h", ".c", ".hpp")):
                p = os.path.join(dirpath, nm)
                rel = os.path.relpath(p, ROOT).replace("\\", "/")
                with open(p, "r", encoding="utf-8", errors="replace") as f:
                    files[rel] = f.read()
    return files


FILES = gather_src()

transport_h = read("src/cloud/mqtt_transport.h")
transport_cpp = read("src/cloud/mqtt_transport.cpp")
cm_cpp = read("src/cloud/cloud_manager.cpp")
cm_h = read("src/cloud/cloud_manager.h")

transport_h_code = strip_code(transport_h)
transport_cpp_code = strip_code(transport_cpp)
cm_cpp_code = strip_code(cm_cpp)

# ---- 1) Facade header hygiene ----
for bad in ["esp_mqtt", "mqtt_client.h", "esp_event", "ArduinoJson", "homie/",
            "cloud_manager.h", "Arduino.h", "String"]:
    check("mqtt_transport.h 不含 '%s'" % bad, bad not in transport_h_code)
check("mqtt_transport.h 仅 include stddef/stdint",
      set(re.findall(r'#include\s*<([^>]+)>', transport_h)) <= {"stddef.h", "stdint.h"})
check("mqtt_transport.h 声明 4 个冻结原型",
      all(fn in transport_h for fn in [
          "mqtt_transport_publish", "mqtt_transport_subscribe",
          "mqtt_transport_set_foreign_handler", "mqtt_transport_is_connected"]))

# ---- 2) 依赖方向单向 ----
check("mqtt_transport.cpp 不 include esp-mqtt/mqtt_client.h",
      "mqtt_client.h" not in transport_cpp_code and "esp_mqtt" not in transport_cpp_code)
check("mqtt_transport.cpp 不调用 esp_mqtt_client_*",
      "esp_mqtt_client_" not in transport_cpp_code)
check("cloud_manager.cpp 不 include mqtt_transport.h",
      "mqtt_transport.h" not in cm_cpp_code)
check("cloud_manager.cpp 不 include homie/",
      not re.search(r'#include\s*"homie/', cm_cpp_code))
check("cloud_manager.cpp 不 include homie_bridge",
      "homie_bridge" not in cm_cpp_code)
check("cloud_manager.h 不 include mqtt_transport.h",
      "mqtt_transport" not in strip_code(cm_h))

# ---- 3) 单 MQTT client ----
all_code = "\n".join(strip_code(v) for v in FILES.values())
init_cnt = all_code.count("esp_mqtt_client_init(")
reg_cnt = all_code.count("esp_mqtt_client_register_event(")
handle_def = all_code.count("esp_mqtt_client_handle_t mqtt_client")
check("全固件 esp_mqtt_client_init( 恰 1 处", init_cnt == 1, "count=%d" % init_cnt)
check("全固件 esp_mqtt_client_register_event( 恰 1 处", reg_cnt == 1, "count=%d" % reg_cnt)
check("全固件 esp_mqtt_client_handle_t mqtt_client 定义恰 1 处", handle_def == 1,
      "count=%d" % handle_def)
check("mqtt_transport.cpp 不创建 client",
      "esp_mqtt_client_init" not in transport_cpp_code)

# ---- 4) 业务下行管线逐项保留 ----
rx = extract_body(cm_cpp_code, "static void cloud_process_rx_message(")
for token in ["cloud_translate_command", "cloud_send_ack", "command_manager_execute",
              "cloud_check_duplicate_cmd", "change_msg_limit", "LOG_ACK_COMMAND",
              "cloud_handle_change_msg_limit", "s_log_ack_cb"]:
    check("业务管线保留: %s" % token, token in rx)

# 业务 ACK 顺序：send_ack 出现在 command_manager_execute 之前
check("ACK 顺序保留 (send_ack 早于 execute)",
      rx.index("cloud_send_ack(cmd.cmd_id") < rx.index("command_manager_execute(cmd)"))

# ---- 5) 业务 subscribe 逐字保留 ----
check("业务 subscribe 调用逐字保留",
      "esp_mqtt_client_subscribe(" in cm_cpp_code
      and "mqtt_sub_topic.c_str()" in cm_cpp_code
      and "CLOUD_MQTT_QOS);" in cm_cpp)

# ---- 6) foreign handler 只在 loop 上下文 ----
check("cloud_manager.cpp 只 include mqtt_routing.h（复用纯逻辑）",
      '#include "cloud/mqtt_routing.h"' in cm_cpp)
handler_body = extract_body(cm_cpp_code, "static void mqtt_event_handler(")
check("mqtt_event_handler 不调用 s_foreign_cb",
      "s_foreign_cb" not in handler_body)
check("mqtt_event_handler 不调用 foreign handler（无 process_foreign）",
      "cloud_transport_process_foreign" not in handler_body)
check("s_foreign_cb 调用恰 1 处（仅 process_foreign）",
      cm_cpp_code.count("s_foreign_cb(") == 1)
task_body = extract_body(cm_cpp_code, "void cloud_task(")
check("cloud_task 调用 cloud_transport_process_foreign（loop 上下文交付）",
      "cloud_transport_process_foreign()" in task_body)
check("cloud_rx_enqueue 从 dispatch 调用（业务路径入口唯一）",
      "cloud_rx_enqueue(data, len)" in extract_body(cm_cpp_code, "static void cloud_transport_dispatch_rx("))

# ---- 7) P1-7 enforcement ----
check("P1-7: GF_ENV_PRODUCTION 保留", "GF_ENV_PRODUCTION" in cm_cpp_code)
check("P1-7: s_mqtt_precheck_failed 保留", "s_mqtt_precheck_failed" in cm_cpp_code)
connect_body = extract_body(cm_cpp_code, "static bool cloud_connect()")
check("P1-7: cloud_connect 首语句即 precheck 早退",
      connect_body[connect_body.index('{') + 1:][:200].find("if(s_mqtt_precheck_failed)") >= 0)

# ---- 8) retain 默认 false ----
check("publish 助手 retain 默认 false（binary）",
      "bool retain = false" in cm_cpp_code)
check("cloud_transport_publish 显式转发 retain",
      "retain," in extract_body(cm_cpp_code, "bool cloud_transport_publish("))

# ---- 9) 无新增 delay ----
check("cloud_manager.cpp 无 delay( 调用", "delay(" not in cm_cpp_code)
check("mqtt_transport.cpp 无 delay( 调用", "delay(" not in transport_cpp_code)

# ---- 10) 有限容量 ----
check("registry 容量为编译期常量",
      "MQTT_ROUTE_MAX_FOREIGN" in read("src/cloud/mqtt_routing.h"))
check("foreign 队列容量为编译期常量",
      "MQTT_FOREIGN_QUEUE_SLOTS" in cm_cpp)

# ---- 输出 ----
fails = 0
for name, ok, detail in results:
    tag = "PASS" if ok else "FAIL"
    if not ok:
        fails += 1
    line = "  %-4s %s" % (tag, name)
    if detail and not ok:
        line += "  [%s]" % detail
    print(line)

print("\n----")
print("total=%d pass=%d fail=%d" % (len(results), len(results) - fails, fails))
sys.exit(1 if fails else 0)
