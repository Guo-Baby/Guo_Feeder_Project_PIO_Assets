#!/usr/bin/env python3
# =====================================================
# P2-4 · Homie Bridge /set · 结果观察 · 状态反馈 —— 静态结构校验
#
# 断言（P2-0 AD-7a/AD-14a/§7.4/§19.5 + P2-4 任务）：
#   1) retained /set：`retain` 是**唯一**判据，且是入口第一件事 → 丢弃且早于任何解析
#      （场景 A 实时 retained publish / 场景 B broker retained replay ⇒ 同一判定）
#   2) 禁止启发式（payload/topic/时间/cmd_id）参与 retain 判定
#   3) pending 表：纯逻辑单一真源（homie_projection.h），定容 4，终态自动出表
#   4) 结果分类：PENDING / SUCCESS / FAILED / TIMEOUT / REJECTED / COMPLETED
#   5) 不新增 CommandManager callback / 不新增第二套 result system
#   6) 只读轮询（get_runtime）+ 不写 System State（状态反馈 = System State 事实源）
#   7) $description 仍为 boot-time 快照（无 runtime 重建点）
#   8) 全固件仍只有 1 个 esp_mqtt_client；非阻塞
#
# 用法: python test/p2_4_homie_set_check.py
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
            q = c
            i += 1
            while i < n and text[i] != q:
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
    i = src.find(needle)
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

bridge_cpp = read("src/homie/homie_bridge.cpp")
proj_h = read("src/homie/homie_projection.h")

bridge_code = strip_code(bridge_cpp)
proj_code = strip_code(proj_h)
BRIDGE_ENABLED = bridge_code.split("#else")[0]

# =====================================================
# 1) retained /set：retain 是唯一判据且在第一件事
# =====================================================
set_body = extract_body(bridge_code, "static void homie_on_foreign(")
check("找到 homie_on_foreign() 处理体", len(set_body) > 0)

i_guard = set_body.find("if(retain)")
i_parse = set_body.find("homie_parse_set_topic")
i_resolve = set_body.find("dm_resolve_command")
check("① retain 判定存在", i_guard >= 0)
check("① retain 判定在解析之前（第一件事）",
      i_guard >= 0 and i_parse > i_guard)
check("① retain 判定在命令映射之前", i_guard >= 0 and i_resolve > i_guard)

# `retain` 在函数体内**只出现两次**：形参声明 + 守卫（⇒ 判定后不再被引用）
check("① retain 在 homie_on_foreign 内仅出现 2 次（形参 + 守卫）",
      set_body.count("retain") == 2,
      "count=%d" % set_body.count("retain"))

# 守卫块：`if(retain)` 起，到下一个 `return;` 止 —— 必须立刻 return
guard_end = set_body.find("return;", i_guard)
guard = set_body[i_guard:guard_end + len("return;")] if i_guard >= 0 and guard_end > i_guard else ""
check("① 守卫块立即 return（不做任何其它动作）",
      guard.find("return;") >= 0 and
      "dm_find" not in guard and "execute" not in guard)

# 2) 禁止启发式
for bad in ["retain &&", "&& retain", "retain||", "||retain",
            "if(retain==1)", "if(retain == 1)"]:
    check("② 无 retain 复合/启发式判定: %s" % bad, bad not in set_body)
check("② 守卫块不含 cmd_id（禁止用 cmd_id 判 replay）", "cmd_id" not in guard)
check("② 守卫块不含 millis / 时间（禁止时间启发式）",
      "millis" not in guard and "time(" not in guard)
check("② 守卫块不含 payload / data 读取（禁止 payload 启发式）",
      "data[" not in guard and "payload" not in guard)

# =====================================================
# 3) pending 表：纯逻辑单一真源
# =====================================================
for fn in ["homie_pending_init", "homie_pending_find", "homie_pending_count",
           "homie_pending_add", "homie_pending_remove", "homie_pending_tick",
           "homie_classify_observed", "homie_outcome_is_terminal"]:
    check("③ 纯逻辑导出 %s()" % fn, ("inline" in proj_code) and (fn + "(" in proj_code))
check("③ 定容常量 HOMIE_PENDING_CAP = 4", "HOMIE_PENDING_CAP       4" in proj_h)
check("③ 结论枚举 HomieOutcome 完整（6 态）",
      all(k in proj_code for k in ["HOMIE_OUTCOME_PENDING", "HOMIE_OUTCOME_SUCCESS",
                                   "HOMIE_OUTCOME_FAILED", "HOMIE_OUTCOME_TIMEOUT",
                                   "HOMIE_OUTCOME_REJECTED", "HOMIE_OUTCOME_COMPLETED"]))
check("③ 观察态枚举 HomieObservedState 完整（5 态）",
      all(k in proj_code for k in ["HOMIE_OBS_RUNNING", "HOMIE_OBS_SUCCESS",
                                   "HOMIE_OBS_FAILED", "HOMIE_OBS_TIMEOUT",
                                   "HOMIE_OBS_RELEASED"]))

tick_body = extract_body(proj_code, "inline HomieOutcome homie_pending_tick(")
check("③ tick 内对终态自动出表（无永久 pending）",
      "homie_pending_remove(" in tick_body)
check("③ tick 内超时自动出表",
      "homie_pending_remove(" in tick_body and "HOMIE_OUTCOME_TIMEOUT" in tick_body)
add_body = extract_body(proj_code, "inline bool homie_pending_add(")
check("③ add 拒绝重复 cmd_id（幂等）",
      "homie_pending_find(" in add_body)
check("③ add 表满返回 false（不覆盖）", "return false;" in add_body)
check("③ RELEASED → COMPLETED 映射",
      "HOMIE_OBS_RELEASED:returnHOMIE_OUTCOME_COMPLETED" in proj_code.replace(" ", ""))

# =====================================================
# 4) bridge 使用纯表（无本地重复实现 / 无第二套队列）
# =====================================================
check("④ bridge 持有 HomiePendingTable（纯逻辑类型）",
      "HomiePendingTable s_pending;" in bridge_cpp)
check("④ bridge 不再定义本地 pending 结构",
      "struct HomiePending\n" not in bridge_cpp and "HomiePending s_pending[" not in bridge_cpp)
check("④ pending 定容与纯逻辑一致（static_assert）",
      "static_assert(HOMIE_PENDING_MAX == HOMIE_PENDING_CAP" in bridge_cpp)
check("④ homie_init 清零 pending 表", "homie_pending_init(&s_pending)" in bridge_cpp)
check("④ 轮询走纯 tick", "homie_pending_tick(" in bridge_cpp and
      "&s_pending" in bridge_cpp)
check("④ 入表走纯 add", "homie_pending_add(&s_pending, cid" in bridge_cpp)

# =====================================================
# 5) 结果观察（只读轮询；不新增 callback / 不新增 result system）
# =====================================================
check("⑤ 只读轮询 command_manager_get_runtime",
      "command_manager_get_runtime(" in bridge_code)
check("⑤ 不注册 CommandManager 结果回调（AD-7a）",
      "command_manager_set_result_callback" not in bridge_code)
check("⑤ 不读全局 last_result（避免跨命令串扰的伪结果）",
      "command_manager_last_result" not in bridge_code)
check("⑤ execute() 拒绝 ⇒ REJECTED 分类",
      "homie_log_outcome(HOMIE_OUTCOME_REJECTED)" in bridge_cpp)
check("⑤ CommandState → 观察态映射存在",
      "homie_observed_from_rt(" in bridge_cpp)
check("⑤ 命令执行仍走 command_manager_execute（唯一入口）",
      "command_manager_execute(m)" in set_body)

# =====================================================
# 6) 状态反馈 = System State 事实源（bridge 不写 state / 不碰硬件）
# =====================================================
check("⑥ bridge 不写 System State（无 state_set_*）",
      not re.search(r'\bstate_set_\w+\s*\(', bridge_code))
check("⑥ bridge 只读 System State",
      "state_is_valid(" in bridge_code and "state_get_" in bridge_code)
for bad in ["digitalWrite", "gpio_set_level", "valve_open(", "valve_close(",
            "weight_zero_calibrate", "MiThermometer", "workflow_enqueue_action"]:
    check("⑥ bridge 不直连硬件/执行层: %s" % bad, bad not in bridge_code)
check("⑥ 结论日志不改写 property 值（log_outcome 内无 mqtt_transport_publish）",
      "mqtt_transport_publish" not in extract_body(bridge_code, "static void homie_log_outcome("))

# =====================================================
# 7) $description 仍为 boot-time 快照
# =====================================================
init_body = extract_body(BRIDGE_ENABLED, "void homie_init(")
task_body = extract_body(BRIDGE_ENABLED, "void homie_task(")
check("⑦ description 仅 INIT 相位生成一次",
      task_body.count("homie_build_and_publish_description()") == 1)
check("⑦ READY 相位不重建 description",
      "homie_build_and_publish_description" not in
      extract_body(BRIDGE_ENABLED, "case HOMIE_PHASE_READY:"))
check("⑦ homie_init 不发布消息", "mqtt_transport_publish" not in init_body)
check("⑦ 运行期不调用 capability_registry_rescan",
      "capability_registry_rescan" not in bridge_code)

# =====================================================
# 8) 单 MQTT client / 非阻塞
# =====================================================
check("⑧ 全固件仍只有 1 个 esp_mqtt_client_init",
      ALL_CODE.count("esp_mqtt_client_init(") == 1,
      "count=%d" % ALL_CODE.count("esp_mqtt_client_init("))
check("⑧ bridge 不 include mqtt_client.h",
      "mqtt_client.h" not in bridge_code)
check("⑧ bridge 无 delay(", "delay(" not in bridge_code)
check("⑧ bridge 无 vTaskDelay", "vTaskDelay" not in bridge_code)
check("⑧ bridge 无 while(1) 死等",
      not re.search(r'while\s*\(\s*(1|true)\s*\)', bridge_code))

# =====================================================
# 9) 不修改 CommandManager 业务语义（仅只读 API 使用）
# =====================================================
cm_h = strip_code(read("src/services/command_manager.h"))
for api in ["command_manager_execute", "command_manager_get_runtime"]:
    check("⑨ bridge 只使用既有 API: %s" % api, api in bridge_code)

# -----------------------------------------------------
n_pass = sum(1 for _, ok, _ in results if ok)
n_fail = len(results) - n_pass
for name, ok, detail in results:
    if not ok:
        line = "FAIL: " + name
        if detail:
            line += "  [" + detail + "]"
        print(line)
print("total=%d pass=%d fail=%d" % (len(results), n_pass, n_fail))
sys.exit(0 if n_fail == 0 else 1)
