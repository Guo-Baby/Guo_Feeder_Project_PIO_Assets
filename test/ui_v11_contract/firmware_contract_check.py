#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
UI v1.1 固件契约核对（源码级，不需要设备）

用途
----
把「云端 78c6bec 的契约」与「固件实际分派代码」逐条对齐，作为**可重复执行**的
回归守卫。它不能替代真机验证，但能防止两类最常见的事故：

  1. 云端白名单里有、固件其实没有对应命令（"目录里有就算支持"）
  2. 固件后来改了分派/字段，云端契约悄悄失效

覆盖
----
  F-3  ACK / result 帧的实际字段名与 status 取值
  F-4  M8 的 22 条只读命令是否真实存在、参数是否对得上、是否无执行器副作用
  F-1  c="safety" 的 stop_all / clear
  F-2  system.force_restart 与 RESTART_FORCE_PENDING
  F-5  阀门安全超时默认值 60 与钳位 10~300
  F-6  safety_outputs_init 的调用位置（config_init 之后、wifi_init 之前）

运行
----
    python test/ui_v11_contract/firmware_contract_check.py
退出码 0 = 全通过；1 = 有失败项。
"""

import re
import sys
import pathlib

ROOT = pathlib.Path(__file__).resolve().parents[2]
SRC = ROOT / "src"


def read(rel):
    p = SRC / rel
    if not p.exists():
        print("!! 缺少源文件: " + str(p))
        sys.exit(2)
    return p.read_text(encoding="utf-8", errors="replace")


CM = read("services/command_manager.cpp")
CC = read("cloud/cloud_manager.cpp")
SC = read("services/system_command.cpp")
SH = read("services/system_command.h")
VALVE = read("app/valve.cpp")
CFG = read("services/config_manager.cpp")
MAIN = read("main.cpp")
SAFETY = read("app/safety.cpp")

RESULTS = []


def check(name, ok, detail=""):
    RESULTS.append((name, bool(ok), detail))


def has(text, pattern, flags=0):
    return re.search(pattern, text, flags) is not None


# ---------------------------------------------------------------- F-3 帧字段
check(
    "F3-01 ACK 用 c 字段（cloud_manager.cpp ack[\"c\"]=\"ack\"）",
    has(CC, r'ack\["c"\]\s*=\s*"ack"'),
    "cloud_manager.cpp",
)
check(
    "F3-02 ACK 用 i 字段关联 cmd_id（ack[\"i\"]=cmd_id）",
    has(CC, r'ack\["i"\]\s*=\s*cmd_id'),
    "cloud_manager.cpp",
)
check(
    "F3-03 result 用 cmd 字段（doc[\"cmd\"]=\"result\"）",
    has(CM, r'doc\["cmd"\]\s*=\s*"result"'),
    "command_manager.cpp",
)
check(
    "F3-04 result 用 id 字段关联 cmd_id（doc[\"id\"]=…cmd_id）",
    has(CM, r'doc\["id"\]\s*=\s*[A-Za-z_][\w\.]*cmd_id'),
    "command_manager.cpp",
)
for st in ("success", "error", "failed", "timeout", "rebooting"):
    check(
        'F3-05 status="%s" 存在' % st,
        has(CM, r'doc\["status"\]\s*=\s*[^;]*"%s"' % st),
        "command_manager.cpp",
    )
check(
    "F3-06 result 帧带 type=\"command\"",
    has(CM, r'doc\["type"\]\s*=\s*"command"'),
    "command_manager.cpp",
)

# ---------------------------------------------------------------- F-1 safety
check('F1-01 一级命令名 "safety" 有分派', has(CM, r'command\s*==\s*"safety"'))
check("F1-02 safety.stop_all 分支存在", has(CM, r'object\s*==\s*"stop_all"'))
check("F1-03 safety.clear 分支存在", has(CM, r'object\s*==\s*"clear"'))
check(
    "F1-04 stop_all 调用 workflow_stop（账本层）",
    has(SAFETY, r"workflow_stop\("),
    "app/safety.cpp",
)
check(
    "F1-05 stop_all 调用 valve_force_close（物理层）",
    has(SAFETY, r"valve_force_close\("),
    "app/safety.cpp",
)
check(
    "F1-06 stop_all 调用 stepper_emergency_stop",
    has(SAFETY, r"stepper_emergency_stop\("),
    "app/safety.cpp",
)
check(
    "F1-07 stop_all 调用 vibration_stop",
    has(SAFETY, r"vibration_stop\("),
    "app/safety.cpp",
)
check(
    "F1-08 stop_all 也停 test_stepper（长通电风险）",
    has(SAFETY, r"test_stepper_stop\("),
    "app/safety.cpp",
)
check(
    "F1-09 报告 physical_verified 恒 false（不谎称已物理停止）",
    has(CM, r'response\["physical_verified"\]\s*=\s*false')
    and has(SAFETY, r"\.verified\s*=\s*false"),
)
check(
    "F1-10 急停锁阻止 Workflow 重新启动",
    has(CM, r"safety_is_latched\(\)"),
)
check(
    "F1-11 latched 在停硬件之前置位（防同 loop 重开阀）",
    SAFETY.find("s_latched = true;") < SAFETY.find("valve_force_close("),
    "app/safety.cpp 顺序",
)

# ---------------------------------------------------------------- F-2 强制重启
check('F2-01 system.force_restart 分支存在', has(CM, r'object\s*==\s*"force_restart"'))
check("F2-02 存在 RESTART_FORCE_PENDING 状态", "RESTART_FORCE_PENDING" in SH)
check(
    "F2-03 强制倒计时 300 ms",
    has(SH, r"SYSTEM_FORCE_RESTART_DELAY_MS\s+300UL"),
    "system_command.h",
)
check(
    "F2-04 强制路径不检查 critical 归零",
    # force_restart() 体内不应出现 s_critical_count 的**判断**（只允许记录日志）
    not has(
        SC[
            SC.find("bool system_command_force_restart()"): SC.find(
                "bool system_command_force_restart_pending("
            )
        ],
        r"if\s*\(\s*cnt\s*!=\s*0",
    ),
    "system_command.cpp",
)
check(
    "F2-05 强制重启前落 boot flag（防 CFG-ROLLBACK）",
    has(CM, r"config_boot_validate\(\)"),
    "command_manager.cpp",
)
check(
    "F2-06 强制重启仍走 system_command 的唯一 restart 归属",
    SC.count("ESP.restart();") == 2,
    "system_command.cpp 中 ESP.restart() 次数=%d（普通+强制各一次）" % SC.count("ESP.restart();"),
)
check(
    "F2-07 不声称配置完整（存在 warning 文案）",
    has(CM, r"不保证配置完整"),
)

# ---------------------------------------------------------------- F-4 只读命令
QUERY_IDS = [
    "query_state", "query_actions", "query_triggers", "query_workflows",
    "query_capabilities", "query_action_registry", "query_trigger_registry",
    "query_workflow_registry",
]
for q in QUERY_IDS:
    check('F4-query %s 有分派' % q, has(CM, r'command\s*==\s*"%s"' % q))

for obj, label in [
    ("memory", "system.memory"), ("flash", "system.flash"),
    ("stats", "system.stats"), ("loop", "system.loop"),
    ("memwatch", "system.memwatch"), ("restart_status", "system.restart_status"),
    ("time", "system.time"), ("config_query", "system.config_query"),
]:
    check('F4-system %s 有分派' % label, has(CM, r'object\s*==\s*"%s"' % obj))

for w in ("workflow.sync_info", "workflow.list", "workflow.get"):
    check('F4-workflow %s 有分派' % w, has(CM, r'command\s*==\s*"%s"' % w))

check(
    "F4-registry k 参数分派（REGISTRY_ACTION/TRIGGER/WORKFLOW）",
    all(x in CC for x in ("REGISTRY_ACTION", "REGISTRY_TRIGGER", "REGISTRY_WORKFLOW")),
    "cloud_manager.cpp",
)
check(
    "F4-param query_state 必须带 object（缺则报错）",
    has(CM, r'cmd\.object\.length\(\)\s*==\s*0[\s\S]{0,200}Missing .object'),
    "command_manager.cpp",
)


# 只读 handler 不得有执行器副作用：截取函数体近似判断
def body_of(fn_name):
    """
    取函数的**定义**体。

    ★ 必须用 rfind：这些函数在文件前部有前向声明，`find` 会命中声明，
      截出来的"函数体"其实是声明到下一个分隔注释之间的无关代码
      （第一版就是这么误报的 10 条 FAIL）。
    """
    marker = "static bool " + fn_name + "("
    i = CM.rfind(marker)
    if i < 0:
        return ""
    j = CM.find("\n// =====", i + len(marker))
    return CM[i: j if j > 0 else i + 4000]


SIDE_EFFECT_RE = re.compile(
    r"valve_open\(|valve_close\(|valve_toggle\(|stepper_start|stepper_emergency_stop|"
    r"vibration_start\(|config_save\(|config_set|ESP\.restart\("
)
for fn in ("command_query_state", "command_query_actions", "command_query_workflows",
           "command_system_memory", "command_system_flash", "command_system_loop",
           "command_system_restart_status", "command_workflow_sync_info",
           "command_workflow_list", "command_workflow_get"):
    body = body_of(fn)
    check("F4-pure %s 无执行器/写入副作用" % fn, body != "" and not SIDE_EFFECT_RE.search(body))

# ---------------------------------------------------------------- F-5 阀门超时
check(
    "F5-01 默认值 60（config_get_valve_safety_timeout_sec）",
    has(CFG, r'\["safety_timeout_sec"\]\s*\|\s*60'),
    "config_manager.cpp",
)
check(
    "F5-02 默认值不再是 300",
    not has(CFG, r'\["safety_timeout_sec"\]\s*\|\s*300'),
)
check(
    "F5-03 钳位下限 10",
    has(VALVE, r"timeout_sec\s*<\s*10[\s\S]{0,60}timeout_sec\s*=\s*10"),
    "valve.cpp",
)
check(
    "F5-04 钳位上限 300 保留",
    has(VALVE, r"timeout_sec\s*>\s*300[\s\S]{0,60}timeout_sec\s*=\s*300"),
    "valve.cpp",
)
check(
    "F5-05 超时检测仍在 valve_task 内生效",
    has(VALVE, r"now\s*-\s*open_start_time\s*>=\s*safety_timeout_ms"),
    "valve.cpp",
)

# ---------------------------------------------------------------- F-6 启动 GPIO
check(
    "F6-01 safety_outputs_init 已实现",
    has(SAFETY, r"void safety_outputs_init\(\)"),
)
check(
    "F6-02 先 gpio_config(OUTPUT) 再 gpio_set_level（顺序）",
    SAFETY.find("gpio_config(&io)") < SAFETY.find("gpio_set_level((gpio_num_t)pin, safe_level)"),
)
check(
    "F6-03 三个执行器引脚均处理（valve / stepper_en / vibration）",
    all(k in SAFETY for k in ("config_get_valve_gpio_pin",
                              "config_get_stepper_en_pin",
                              "config_get_vibration_relay_pin")),
)
_i_cfg = MAIN.find("config_init();")
_i_safety = MAIN.find("safety_outputs_init();")
_i_wifi = MAIN.find("wifi_init();")
_i_ble = MAIN.find("ble_init();")
check(
    "F6-04 调用顺序：config_init → safety_outputs_init → ble/wifi",
    -1 < _i_cfg < _i_safety and _i_safety < _i_ble and _i_safety < _i_wifi,
    "index: config=%d safety=%d ble=%d wifi=%d" % (_i_cfg, _i_safety, _i_ble, _i_wifi),
)
check(
    "F6-05 安全电平 = 有效电平取反",
    has(SAFETY, r"active_level\s*\?\s*0\s*:\s*1"),
)

# ------------------------------------------------ F-1 补强（锁存覆盖启动入口）
SAFETY_H = read("app/safety.h")
WF_CPP = read("automation/workflow.cpp")
WF_H = read("automation/workflow.h")

check(
    "F1b-01 safety_action_blocked_by_latch 已声明+实现",
    "safety_action_blocked_by_latch" in SAFETY_H and "safety_action_blocked_by_latch" in SAFETY,
)
_ALLOW = ("VALVE_CLOSE", "STEPPER_EMERGENCY_STOP", "TEST_STEPPER_STOP",
          "VIBRATION_STOP", "MI_THERMO_STOP_SCAN")
# 2026-10-23 裁决：这两项**必须**在锁存期间被拒绝（已从白名单移除）
_BLOCK = ("VALVE_OPEN", "STEPPER_DISPENSE_ROTATE", "STEPPER_ROTATE_STEPS",
          "TEST_STEPPER_START", "VIBRATION_START", "COMPUTER_RESET",
          "WEIGHT_ZERO", "MI_THERMO_START_SCAN")
_wl = SAFETY[SAFETY.find("kLatchAllowedActions"):] if "kLatchAllowedActions" in SAFETY else ""
_wl = _wl[:_wl.find("};") + 2] if "};" in _wl else _wl
check(
    "F1b-02 白名单含全部停止类/传感器类（%d 项）" % len(_ALLOW),
    all(('"%s"' % a) in _wl for a in _ALLOW),
    "缺: " + ",".join(a for a in _ALLOW if ('"%s"' % a) not in _wl),
)
check(
    "F1b-03 白名单**不含**任何启动类 Action（失败即拒绝）",
    not any(('"%s"' % a) in _wl for a in _BLOCK),
    "误放行: " + ",".join(a for a in _BLOCK if ('"%s"' % a) in _wl),
)
check(
    "F1b-04 workflow_start 受锁存拦截（Trigger 自动启动也被覆盖）",
    has(WF_CPP, r"if\(safety_is_latched\(\)\)"),
)
check(
    "F1b-05 enqueue_temp_action 受锁存拦截（Command 下发唯一入口）",
    has(WF_CPP, r"if\s*\(safety_action_blocked_by_latch\(id\)\)"),
)
check(
    "F1b-06 workflow_action_start 兜底拦截",
    has(WF_CPP, r"safety_action_blocked_by_latch\(action->descriptor->id\)"),
)
check(
    "F1b-07 workflow_cancel_pending_temp_actions 声明+实现",
    "workflow_cancel_pending_temp_actions" in WF_H
    and "workflow_cancel_pending_temp_actions" in WF_CPP,
)
check(
    "F1b-08 stop_all 顺序：先立牌子 → 停 workflow → 取消排队 → 停硬件",
    SAFETY.find("s_latched = true;") < SAFETY.find("workflow_stop(w->id)")
    < SAFETY.find("workflow_cancel_pending_temp_actions()")
    < SAFETY.find("valve_force_close()"),
)
check(
    "F1b-09 command_execute_action 有可读拒绝原因（不只在 Workflow 层拦）",
    has(CM, r"safety_action_blocked_by_latch\(cmd\.object\)"),
)

# ------------------------------------------------ F-2 校验（boot flag 回读）
CFG_H = read("services/config_manager.h")
check(
    "F2b-01 config_boot_flag_present 声明+实现（独立回读）",
    "config_boot_flag_present" in CFG_H and "config_boot_flag_present" in CFG,
)
check(
    "F2b-02 boot_flag_written = 写入结果 && 回读存在",
    has(CM, r"boot_flag_written\s*=\s*flag_write_ok\s*&&\s*flag_present"),
)
check(
    "F2b-03 强制重启显式声明不保证配置完整",
    has(CM, r'config_integrity_guaranteed"\]\s*=\s*false'),
)
check(
    "F2b-04 强制重启上报 critical_at_request（突破保护的客观证据）",
    has(CM, r'"critical_at_request"\]\s*=\s*critical_at_request'),
)

# ---------------------------------------------------------------- 输出
print("=" * 72)
print("UI v1.1 固件契约核对（源码级）")
print("=" * 72)
failed = 0
for name, ok, detail in RESULTS:
    if ok:
        print("  PASS  " + name)
    else:
        failed += 1
        print("  FAIL  " + name + ("   << " + detail if detail else ""))

print("-" * 72)
print("total %d | PASS %d | FAIL %d" % (len(RESULTS), len(RESULTS) - failed, failed))
print()
print("说明：本脚本只核对**源码形态**，不证明设备行为。")
print("      标注 [未验证] 的项必须靠真机（串口 + 万用表）确认。")
sys.exit(0 if failed == 0 else 1)
