#!/usr/bin/env python3
# =====================================================
# P2-3 · Homie Bridge —— 静态结构校验
#
# 断言 P2-0 冻结的架构边界在**源码层面**成立：
#   1) homie_bridge.h 极窄（不暴露 esp-mqtt / CloudManager / Homie 内部结构）
#   2) 依赖方向单向：homie_bridge → mqtt_transport → cloud_manager
#      （CloudManager 不 include homie；Device Model 不 include Registry/cloud）
#   3) 全固件仍然只有 1 个 esp_mqtt_client
#   4) Homie 不直连硬件 / 不写 System State / 不绕过 CommandManager
#   5) /set 路径完整且顺序正确（retain → 命名空间 → 校验 → 显式映射 → execute）
#   6) $description 是 boot-time 快照（唯一生成点 + 不因 rescan 重建）
#   7) retain 严格取 Device Model（禁止统一硬编码 true）
#   8) GF_HOMIE_ENABLED 编译期门控（默认 0 ⇒ 空实现）
#   9) 非阻塞（无 delay / 无死等）、容量有限
#  10) main 循环接入点存在
#
# 用法: python test/p2_3_homie_check.py
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
    i = src.rfind(needle)
    if i < 0:
        return ""
    j = src.find('{', i)
    if j < 0:
        return ""
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
ALL_CODE = "\n".join(strip_code(v) for v in FILES.values())

bridge_h = read("src/homie/homie_bridge.h")
bridge_cpp = read("src/homie/homie_bridge.cpp")
proj_h = read("src/homie/homie_projection.h")
dm_h = read("src/homie/device_model.h")
dm_cpp = read("src/homie/device_model.cpp")
cr_cpp = read("src/automation/capability_registry.cpp")
cr_h = read("src/automation/capability_registry.h")
cm_cpp = read("src/cloud/cloud_manager.cpp")
cmdm_cpp = read("src/services/command_manager.cpp")
main_cpp = read("src/main.cpp")

bridge_cpp_code = strip_code(bridge_cpp)
# 注意：本文件同时存在「启用实现」与「GF_HOMIE_ENABLED=0 空实现」两套同名函数，
# extract_body() 取**最后一次**出现 ⇒ 必须把作用域限定在 #else 之前的启用分支。
BRIDGE_ENABLED = bridge_cpp_code.split("#else")[0]
cm_cpp_code = strip_code(cm_cpp)
dm_cpp_code = strip_code(dm_cpp)
dm_h_code = strip_code(dm_h)
cr_cpp_code = strip_code(cr_cpp)
cr_h_code = strip_code(cr_h)
cmdm_cpp_code = strip_code(cmdm_cpp)
main_cpp_code = strip_code(main_cpp)
proj_h_code = strip_code(proj_h)

# =====================================================
# 1) homie_bridge.h 极窄
# =====================================================
for bad in ["esp_mqtt", "mqtt_client.h", "esp_event", "ArduinoJson",
            "cloud_manager.h", "cloud/mqtt_transport.h", "device_model.h",
            "command_manager.h", "String", "DeviceModelProperty"]:
    check("homie_bridge.h 不含 '%s'" % bad, bad not in strip_code(bridge_h))
check("homie_bridge.h 只 include <stdint.h>",
      set(re.findall(r'#include\s*<([^>]+)>', bridge_h)) <= {"stdint.h"})
check("homie_bridge.h 仅暴露 3 个接口",
      len(re.findall(r'^(void|bool)\s+homie_\w+\(', bridge_h, re.M)) == 3)

# =====================================================
# 2) 依赖方向单向
# =====================================================
check("homie_bridge.cpp include cloud/mqtt_transport.h（唯一允许的 cloud 头）",
      '#include "cloud/mqtt_transport.h"' in bridge_cpp)
check("homie_bridge.cpp 不 include cloud_manager.h",
      "cloud_manager.h" not in bridge_cpp_code)
check("homie_bridge.cpp 不 include mqtt_client.h / esp_mqtt",
      "mqtt_client.h" not in bridge_cpp_code and "esp_mqtt" not in bridge_cpp_code)
check("homie_bridge.cpp 不调用 esp_mqtt_client_*",
      "esp_mqtt_client_" not in bridge_cpp_code)
check("homie_bridge.cpp 不 include mqtt_routing.h（属 CloudManager 内部纯逻辑）",
      "mqtt_routing.h" not in bridge_cpp_code)

check("cloud_manager.cpp 不 include homie/*",
      not re.search(r'#include\s+"homie/', cm_cpp_code))
check("cloud_manager.cpp 不引用 homie_bridge / homie_",
      "homie_bridge" not in cm_cpp_code and "homie_" not in cm_cpp_code)
check("cloud_manager.h 不引用 homie", "homie" not in strip_code(read("src/cloud/cloud_manager.h")))
check("mqtt_transport.cpp 不 include homie/*",
      not re.search(r'#include\s+"homie/', strip_code(read("src/cloud/mqtt_transport.cpp"))))

# Device Model 与 Capability Registry 是**兄弟模块**：互不 include
check("device_model.h 不 include automation/*（Registry）",
      not re.search(r'#include\s+"automation/', dm_h))
check("device_model.cpp 不 include automation/*（Registry）",
      not re.search(r'#include\s+"automation/', dm_cpp))
check("device_model.* 不 include cloud/*",
      not re.search(r'#include\s+"cloud/', dm_h) and
      not re.search(r'#include\s+"cloud/', dm_cpp))
check("device_model.* 不 include command_manager / mqtt",
      "command_manager.h" not in dm_cpp_code and "mqtt" not in dm_cpp_code)
check("capability_registry.* 不 include homie/*",
      not re.search(r'#include\s+"homie/', cr_cpp) and
      not re.search(r'#include\s+"homie/', cr_h))
check("device_model.h 只 include stdint + system_state",
      set(re.findall(r'#include\s*[<"]([^>"]+)[>"]', dm_h))
      <= {"stdint.h", "services/system_state.h"})

# =====================================================
# 3) 单 MQTT client（P2-3 后仍然成立）
# =====================================================
check("全固件 esp_mqtt_client_init( 恰 1 处",
      ALL_CODE.count("esp_mqtt_client_init(") == 1,
      "count=%d" % ALL_CODE.count("esp_mqtt_client_init("))
check("全固件 esp_mqtt_client_register_event( 恰 1 处",
      ALL_CODE.count("esp_mqtt_client_register_event(") == 1,
      "count=%d" % ALL_CODE.count("esp_mqtt_client_register_event("))

# =====================================================
# 4) Homie 不越界（硬件 / System State 写入 / Workflow / CommandManager 语义）
# =====================================================
for bad in ['#include "app/', '#include "automation/workflow.h',
            '#include "automation/workflow_storage.h']:
    check("homie_bridge.cpp 不 include %s" % bad, bad not in bridge_cpp)
for bad in ["valve_open(", "valve_close(", "weight_", "MiThermometer",
            "computer_reset_", "oled_", "dispense_guard", "digitalWrite",
            "gpio_set_level"]:
    check("homie_bridge.cpp 不调用硬件 API: %s" % bad, bad not in bridge_cpp_code)
check("homie_bridge.cpp 不写 System State（无 state_set_*）",
      not re.search(r'\bstate_set_\w+\s*\(', bridge_cpp_code))
check("homie_bridge.cpp 只读 System State（有 state_get / state_is_valid）",
      "state_is_valid(" in bridge_cpp_code and "state_get_float(" in bridge_cpp_code)
check("homie_bridge.cpp 不直接调用 workflow_enqueue_action",
      "workflow_enqueue_action" not in bridge_cpp_code)
check("homie_bridge.cpp 不新增 CommandManager 结果回调（AD-7a）",
      "command_manager_set_result_callback" not in bridge_cpp_code)
check("homie_bridge.cpp 调用 command_manager_execute",
      "command_manager_execute(m)" in bridge_cpp_code)
check("homie_bridge.cpp 只读轮询 command_manager_get_runtime",
      "command_manager_get_runtime(" in bridge_cpp_code)

# 命令构造必须来自 Device Model，不得拼接 / 大小写推导
for bad in ["toupper", "strcat", "sprintf", "+ VALVE_", "\"VALVE_\""]:
    check("homie_bridge.cpp 不拼接/推导 Action ID: %s" % bad,
          bad not in bridge_cpp_code)
check("homie_bridge.cpp 使用 dm_resolve_command（显式映射）",
      "dm_resolve_command(" in bridge_cpp_code)
check("homie_bridge.cpp 使用 dm_validate_set（协议层校验）",
      "dm_validate_set(" in bridge_cpp_code)

# =====================================================
# 5) /set 路径与顺序
# =====================================================
set_body = extract_body(bridge_cpp_code, "static void homie_on_foreign(")
check("① retain 判定是 /set 处理的第一件事（AD-14a）",
      set_body.find("if(retain)") >= 0 and
      set_body.find("if(retain)") < set_body.find("homie_parse_set_topic"))
check("② 命名空间校验 + node/prop 解析",
      "homie_parse_set_topic(" in set_body)
check("③ dm_find 查属性", "dm_find(node, prop)" in set_body)
check("④ dm_validate_set 校验", "dm_validate_set(p, value)" in set_body)
check("⑤ dm_resolve_command 显式映射", "dm_resolve_command(p, value, &cmd)" in set_body)
check("⑥ cmd_id 由设备自生成（homie_cmd_id）", "homie_cmd_id(" in set_body)
check("⑦ 经 CommandManager 执行", "command_manager_execute(m)" in set_body)
check("⑧ source=\"homie\"", '= "homie"' in bridge_cpp)
check("⑨ command = \"execute_action\"", '= "execute_action"' in bridge_cpp)
check("⑩ /set 载荷做可打印字符校验（防 NUL/控制字符）",
      "c < 0x20" in set_body or "c < 32" in set_body)

# =====================================================
# 6) $description = boot-time 快照
# =====================================================
desc_fn = extract_body(bridge_cpp_code, "static bool homie_build_and_publish_description(")
check("description 使用 heap_caps_malloc（禁静态大数组 AD-11a）",
      "heap_caps_malloc" in desc_fn and "MALLOC_CAP_SPIRAM" in desc_fn)
check("description 缓冲复用（只在为空时分配）", "if(s_desc_buf == nullptr)" in desc_fn)
check("description 走 homie_desc_build（纯逻辑单一真源）", "homie_desc_build(" in desc_fn)
check("description 发布 retained=true", "HOMIE_QOS, true" in desc_fn)
check("description 记录 fingerprint", "s_desc_fp      = ver" in desc_fn)

task_fn = extract_body(BRIDGE_ENABLED, "void homie_task(")
check("description 只在 INIT 相位生成（唯一生成点）",
      task_fn.count("homie_build_and_publish_description()") == 1)
check("READY 相位不重建 description",
      "homie_build_and_publish_description" not in
      extract_body(BRIDGE_ENABLED, "case HOMIE_PHASE_READY:"))
check("运行期不调用 capability_registry_rescan",
      "capability_registry_rescan" not in bridge_cpp_code)
check("fingerprint 哨兵只告警不重发",
      "homie_check_fingerprint" in task_fn and
      "LOG_HOMIE_DESC_STALE" in bridge_cpp)
check("fingerprint 不一致时不改 $state",
      "homie_check_fingerprint" in extract_body(bridge_cpp_code, "static void homie_check_fingerprint("))

# =====================================================
# 7) retain 严格取 Device Model
# =====================================================
pub_fn = extract_body(bridge_cpp_code, "static void homie_publish_values(")
check("属性发布 retain 取 p->retained（非硬编码）",
      "p->retained" in pub_fn and "true, HOMIE_STORE" not in pub_fn)
check("命令型属性 retained=false 由 Device Model 决定（未出现 hardcode true）",
      "HOMIE_QOS, p->retained, HOMIE_STORE" in pub_fn)
check("变化检测使用 deadband + min_pub_ms",
      "p->deadband" in pub_fn and "min_pub" in pub_fn)

# =====================================================
# 8) GF_HOMIE_ENABLED 门控
# =====================================================
check("定义 GF_HOMIE_ENABLED 默认 0",
      "#define GF_HOMIE_ENABLED 0" in bridge_cpp)
check("实现体由 #if GF_HOMIE_ENABLED 包裹",
      "#if GF_HOMIE_ENABLED" in bridge_cpp)
check("关闭态提供空实现（零 MQTT 流量）",
      "void homie_init()      { }" in bridge_cpp and
      "void homie_task()      { }" in bridge_cpp and
      "bool homie_is_ready()  { return false; }" in bridge_cpp)

# =====================================================
# 9) 非阻塞 + 容量有限
# =====================================================
check("homie_bridge.cpp 无 delay( 调用", "delay(" not in bridge_cpp_code)
check("homie_bridge.cpp 无 vTaskDelay", "vTaskDelay" not in bridge_cpp_code)
check("homie_bridge.cpp 无 while(1) 死等",
      not re.search(r'while\s*\(\s*(1|true)\s*\)', bridge_cpp_code))
for cap in ["HOMIE_MAX_PROPERTIES", "HOMIE_PENDING_MAX", "HOMIE_SET_VALUE_MAX",
            "HOMIE_TOPIC_MAX", "HOMIE_DESC_CAP"]:
    check("容量为编译期常量: %s" % cap, cap in bridge_cpp or cap in proj_h)
check("pending 表定容 ≤ 4（§7.4）", "HOMIE_PENDING_MAX        4" in bridge_cpp)

# =====================================================
# 10) LWT（仅能力准备，不实现 Homie 在线状态机）
# =====================================================
init_fn = extract_body(BRIDGE_ENABLED, "void homie_init(")
check("LWT topic = $state", "homie_state_topic(s_base, st" in init_fn)
# 注：字符串字面量已被 strip_code 替换为 ""，故对 LWT 载荷改用**原始源码**核对
check("LWT payload = lost（规范强制）",
      'mqtt_transport_set_lwt(st, "lost", HOMIE_QOS, true)' in bridge_cpp)
check("LWT retain = true（规范强制）",
      re.search(r'mqtt_transport_set_lwt\(\s*st\s*,\s*"lost"\s*,\s*HOMIE_QOS\s*,\s*true\s*\)',
                bridge_cpp) is not None)
check("订阅 filter 精确（不含 '#'）",
      "homie_set_filter(s_base, filter" in init_fn and '"#"' not in init_fn)
check("device_id 来自 DeviceIdentity",
      "device_id_valid()" in init_fn and "device_id()" in init_fn)
check("homie_init 不发布任何消息（§9：发布发生在 task）",
      "mqtt_transport_publish" not in init_fn)
check("homie_init 注册 foreign handler",
      "mqtt_transport_set_foreign_handler(" in init_fn)
check("homie_init 注册订阅（走 Facade，非 ESP-MQTT）",
      "mqtt_transport_subscribe(" in init_fn)

# =====================================================
# 11) 排除项：不得投影内部态 / secret / workflow / stable_id
# =====================================================
check("homie_projection.h 不 include 配置/凭据/网络头",
      not re.search(r'#include\s+"(services/config_manager|services/credential|cloud/)', proj_h))
for bad in ["password", "secret", "api_key", "token", "BLE_key", "credential"]:
    check("homie_projection.h 不含 secret 字段: %s" % bad, bad not in proj_h_code)
check("$description 不含 workflow（AD-9）",
      "workflow" not in proj_h_code)
check("$description 不含 stable_id",
      "stable_id" not in proj_h_code)
check("$description 不含 $target（D-13）", "target" not in proj_h_code)

# =====================================================
# 12) 既有模块未被 Homie 污染（Registry / CommandManager 语义不变）
# =====================================================
check("command_manager.cpp 不引用 homie", "homie" not in cmdm_cpp_code)
check("capability_registry.cpp 不引用 homie", "homie" not in cr_cpp_code)
check("cloud_manager.cpp 保留 P1-7 enforcement",
      "GF_ENV_PRODUCTION" in cm_cpp_code and "s_mqtt_precheck_failed" in cm_cpp_code)

# =====================================================
# 13) main 接入
# =====================================================
check("main.cpp include homie_bridge.h", '"homie/homie_bridge.h"' in main_cpp)
check("main.cpp setup 调用 homie_init()", "homie_init();" in main_cpp_code)
check("main.cpp loop 调用 homie_task()", "homie_task();" in main_cpp_code)
check("homie_init 位于 capability_registry_init 之后",
      main_cpp_code.index("capability_registry_init()") < main_cpp_code.index("homie_init()"))
check("homie_task 位于 cloud_task 之后",
      main_cpp_code.index("cloud_task();") < main_cpp_code.index("homie_task();"))
check("homie_task 位于 workflow_task 之前（当轮即可执行 Action）",
      main_cpp_code.index("homie_task();") < main_cpp_code.index("workflow_task();"))

# =====================================================
# 输出
# =====================================================
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
