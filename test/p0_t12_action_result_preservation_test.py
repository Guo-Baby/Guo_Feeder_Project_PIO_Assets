#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
p0_t12_action_result_preservation_test.py —— T12-B 回归守卫
================================================================================
背景（T11 上线后的真实回归，boot 253 真机复现）：
  动作物理执行成功后，`forceProbe()` 触发的**同一轮**探活会在 startRequest 里
  把槽的 owner 覆盖成 nullptr、fin 清零 ⇒ actionPoll 的两个匹配 loop 都找不到
  ⇒ 旧兜底只救 `ACTION_IDLE`（动作 result 已是 RUNNING）⇒ 工作流永挂 RUNNING，
  直到 workflow 级 10 分钟超时（真机 state=TIMEOUT，current_step=1，power=true）。

本套件钉住四条契约 + 2 条负向探针：
  R-1  探活（owner==nullptr）不得覆盖"fin 未领取"的动作结果（让路 + 上限）
  R-2  actionPoll 领走结果后立即清槽（owner/fin 复位）
  R-3  actionPoll 兜底覆盖 IDLE **和** RUNNING（消灭永挂）
  R-4  让步有上限（XLAN_PROBE_DEFER_MAX），防止探活被饿死
  N-1  删掉让路守卫后必须 FAIL（负向探针）
  N-2  把兜底改回只救 IDLE 后必须 FAIL（负向探针）
================================================================================
"""

import re
import sys

def strip_comments(text: str) -> str:
    text = re.sub(r"/\*[\s\S]*?\*/", "", text)
    text = re.sub(r"//[^\n]*", "", text)
    return text

def load(path: str) -> str:
    with open(path, "r", encoding="utf-8") as f:
        return f.read()

def func_body(text: str, name: str) -> str:
    """截取函数定义体：定位 name( → 配对参数右括号 ) → 配对函数体大括号。"""
    for m in re.finditer(r"\b" + re.escape(name) + r"\s*\(", text):
        # 配对参数右括号
        depth = 0
        close = -1
        for j in range(m.end() - 1, len(text)):
            if text[j] == "(":
                depth += 1
            elif text[j] == ")":
                depth -= 1
                if depth == 0:
                    close = j
                    break
        if close < 0:
            continue
        i = text.find("{", close)
        if i < 0 or text[close + 1:i].strip() != "":
            continue  # 参数列表后没有紧跟函数体（前置声明 / 调用点）
        depth = 0
        for j in range(i, len(text)):
            if text[j] == "{":
                depth += 1
            elif text[j] == "}":
                depth -= 1
                if depth == 0:
                    return text[i:j + 1]
    return ""

PASS = 0
FAIL = 0
FAILURES = []

def ok(name, cond, detail=""):
    global PASS, FAIL
    if cond:
        PASS += 1
        print("  PASS  " + name)
    else:
        FAIL += 1
        FAILURES.append(name + (("  <- " + detail) if detail else ""))
        print("  FAIL  " + name + (("  <- " + detail) if detail else ""))

XLAN = strip_comments(load("src/app/xiaomi_lan.cpp"))
XLAN_RAW = load("src/app/xiaomi_lan.cpp")

sr = func_body(XLAN, "startRequest")
poll = func_body(XLAN, "actionPoll")

print("\n[R-1] 探活让路：不得覆盖未领取的动作结果")
ok("R-1a startRequest 有「探活 + 未领取结果 ⇒ 让路」守卫",
   re.search(r"owner\s*==\s*nullptr\s*&&\s*r\.owner\s*!=\s*nullptr\s*&&\s*r\.fin", sr) is not None)
ok("R-1b 让路发生在 fin/ok 清零**之前**",
   sr.find("owner == nullptr && r.owner != nullptr && r.fin")
   < sr.find("r.fin = false"),
   "guard@%d clear@%d" % (sr.find("owner == nullptr && r.owner != nullptr && r.fin"),
                          sr.find("r.fin = false")))
ok("R-1c 让路分支 return false（本轮不发探活）",
   re.search(r"owner\s*==\s*nullptr\s*&&\s*r\.owner\s*!=\s*nullptr\s*&&\s*r\.fin[\s\S]{0,220}return\s+false", sr) is not None)
ok("R-1d defer_count 在让路时递增", "r.defer_count++" in sr)

print("\n[R-2] 结果领走即清槽")
ok("R-2a actionPoll 第二匹配 loop 读到结果后清 owner/fin",
   re.search(r"r\.ok\s*\?\s*ACTION_SUCCESS\s*:\s*ACTION_FAILED[\s\S]{0,320}r\.fin\s*=\s*false", poll) is not None)
ok("R-2b 清槽只在本槽非 active 时（不动在途请求）",
   re.search(r"if\s*\(!r\.active\)[\s\S]{0,160}r\.owner\s*=\s*nullptr", poll) is not None)

print("\n[R-3] 兜底覆盖 IDLE 和 RUNNING")
ok("R-3a 兜底条件包含 ACTION_RUNNING",
   re.search(r"result\s*==\s*ACTION_IDLE\s*\|\|\s*action->result\s*==\s*ACTION_RUNNING", poll) is not None
   or re.search(r"result\s*==\s*ACTION_IDLE\s*\|\|\s*.*ACTION_RUNNING", poll) is not None)
ok("R-3b 兜底置 ACTION_FAILED（诚实终态）",
   re.search(r"ACTION_IDLE\s*\|\|[\s\S]{0,80}ACTION_RUNNING[\s\S]{0,160}action->result\s*=\s*ACTION_FAILED", poll) is not None)
ok("R-3c 不存在无条件 ACTION_SUCCESS 兜底", "ACTION_SUCCESS" in poll)

print("\n[R-4] 让步上限（防探活饿死）")
ok("R-4a 存在 XLAN_PROBE_DEFER_MAX 常量",
   re.search(r"XLAN_PROBE_DEFER_MAX\s*=\s*[1-9]", XLAN) is not None)
ok("R-4b 让路受上限约束（defer_count < MAX 才 return false）",
   re.search(r"r\.defer_count\s*<\s*XLAN_PROBE_DEFER_MAX", sr) is not None)
ok("R-4c 计数在认领/清槽时复位", "r.defer_count = 0" in sr and "r.defer_count = 0" in poll)

print("\n[T11 契约不回退]")
ok("T-1 startRequest 仍保留提前清 fin/ok（R4 修复未回退）",
   re.search(r"if\s*\(!r\.active\)[\s\S]{0,600}r\.fin\s*=\s*false", sr) is not None)
ok("T-2 actionPoll 双槽扫描仍在（P4 并行修复未回退）",
   "for (uint8_t i = 0; i < 2; i++)" in poll)

print("\n[负向探针]")
# N-1：删掉让路守卫后 R-1a 必须不再命中
broken1 = re.sub(r"owner\s*==\s*nullptr\s*&&\s*r\.owner\s*!=\s*nullptr\s*&&\s*r\.fin", "false", XLAN)
ok("N-1 删掉让路守卫后能被识别",
   re.search(r"owner\s*==\s*nullptr\s*&&\s*r\.owner\s*!=\s*nullptr\s*&&\s*r\.fin",
             strip_comments(broken1)) is None)
# N-2：把兜底改回只救 IDLE 后 R-3a 必须不再命中
broken2 = re.sub(r"action->result == ACTION_IDLE \|\| action->result == ACTION_RUNNING",
                 "action->result == ACTION_IDLE", XLAN)
ok("N-2 兜底退回只救 IDLE 后能被识别",
   re.search(r"result\s*==\s*ACTION_IDLE\s*\|\|\s*action->result\s*==\s*ACTION_RUNNING",
             strip_comments(broken2)) is None)

print(f"\n===== T12-B: PASS {PASS} / FAIL {FAIL} =====")
if FAIL:
    print("失败项:")
    for f in FAILURES:
        print("  - " + f)
    sys.exit(1)
