#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""p4_wire_envelope_contract_test.py —— 固件「配置结果 → 出线报文」契约守卫

为什么需要这条测试（2026-11-01 上板实测的真实缺陷）
----------------------------------------------------
ConfigManager 的 exec_query_all() / exec_query_module() 把查询结果写在 JSON
文档的**顶层**（data / version / versions / dirty / restart_in_ms）。

而 command_manager.cpp 里生成 `config_result` 出线报文的封包函数是**白名单式**
拷贝 —— 只显式挑了 data / version / versions。

⇒ T05 新增的 `dirty` / `restart_in_ms` 在发出前被**静默丢弃**（无日志、无错误码）：
   固件侧单测绿（config_manager 确实 set 了）
   前端侧单测绿（parseConfigDirty 确实能解析）
   **但线上功能是死的** —— 前端永远读不到 dirty。

该缺陷只有"真机串口 query 一次"才能暴露，任何纯源码单测都发现不了。
本测试把这条契约变成机器可检断言，防止下一次"结果层加键、封包层忘了登记"。

判据
----
  ① A = ConfigManager 各 query 结果的**顶层字面量键**集合
  ② B = 封包函数里 `rv["..."]` 的**转发键**集合
  ③ 断言  A − {data, keys}  ⊆  B
     （`data` / `keys` 走「整体拷贝」通道，由 else 分支承载，不需逐键登记）

配套**负向探针**：把 B 中的 restart_in_ms 去掉后，检查器必须报错。
没有负向探针的测试是不可证伪的（参见 test/log_contract 的同款约定）。

用法：python test/p4_wire_envelope_contract_test.py
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CFG = os.path.join(ROOT, "src", "services", "config_manager.cpp")
CMDM = os.path.join(ROOT, "src", "services", "command_manager.cpp")

# 「整体拷贝」通道：这些键由封包函数的 else 分支
# （doc["data"].set(rv)，把整份 rv 拷进去）承载，不需要逐键转发。
#   · data    —— 查询类主载荷，有专门的 doc["data"] 分支
#   · keys    —— 单模块 keys_only 结果（与 data 互斥，落到 else 分支）
#   · modules —— 全量 keys_only 结果（同上）
WHOLESALE = {"data", "keys", "modules"}

QUERY_FUNCS = ("exec_query_all", "exec_query_module")

failures = []
passes = [0]


def ok(name, cond, detail=""):
    if cond:
        passes[0] += 1
        print("  PASS  " + name)
    else:
        failures.append(name + (("  <- " + detail) if detail else ""))
        print("  FAIL  " + name + (("  <- " + detail) if detail else ""))


def strip_comments(text):
    """去掉 // 与 /* */ 注释（避免注释里的键名污染字面量集合）。"""
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    text = re.sub(r"//[^\n]*", "", text)
    return text


def func_body(text, name):
    """按大括号配对截取函数体；找不到返回 None。"""
    m = re.search(r"\b" + re.escape(name) + r"\s*\(", text)
    if m is None:
        return None
    i = text.find("{", m.end())
    if i < 0:
        return None
    depth = 0
    for j in range(i, len(text)):
        ch = text[j]
        if ch == "{":
            depth += 1
        elif ch == "}":
            depth -= 1
            if depth == 0:
                return text[i:j + 1]
    return None


def result_top_keys(cfg_src):
    """ConfigManager 各 query 结果文档的顶层字面量键。"""
    keys = set()
    for fn in QUERY_FUNCS:
        body = func_body(cfg_src, fn)
        if body is None:
            return None, "未找到函数 %s（契约守卫需要更新锚点）" % fn
        # out["xxx"] —— 只取**顶层**写法；嵌套都通过 JsonObject 变量间接操作。
        keys |= set(re.findall(r'\bout\s*\[\s*"([A-Za-z0-9_]+)"\s*\]', body))
    return keys, ""


def envelope_forwarded_keys(cmd_src):
    """封包函数里 rv["xxx"] 的转发键集合。"""
    idx = cmd_src.find('doc["type"] = "config_result"')
    if idx < 0:
        return None, "未找到 config_result 封包点"
    # 向前找到函数起点：取包含该赋值的最内层函数
    head = cmd_src[:idx]
    starts = [m.start() for m in re.finditer(r"^\s*(?:static\s+)?[A-Za-z_][\w:<>*&\s]*\b\w+\s*\([^;]*\)\s*$",
                                             head, flags=re.M)]
    start = starts[-1] if starts else 0
    body = func_body(cmd_src[start:], extract_name(cmd_src[start:]))
    if body is None:
        body = cmd_src[start:]
    return set(re.findall(r'\brv\s*\[\s*"([A-Za-z0-9_]+)"\s*\]', body)), ""


def extract_name(chunk):
    m = re.search(r"\b(\w+)\s*\(", chunk)
    return m.group(1) if m else ""


def check(cfg_src, cmd_src):
    """返回 (ok, 缺失键集合, 说明)。"""
    a, err = result_top_keys(cfg_src)
    if err:
        return False, set(), err
    b, err = envelope_forwarded_keys(cmd_src)
    if err:
        return False, set(), err
    need = a - WHOLESALE
    missing = need - b
    return (len(missing) == 0), missing, "结果层键=%s 转发键=%s" % (
        sorted(a), sorted(b))


def main():
    print("=" * 66)
    print("p4_wire_envelope_contract_test —— 配置结果 → 出线报文 契约守卫")
    print("=" * 66)

    for p in (CFG, CMDM):
        if not os.path.exists(p):
            print("  FATAL 缺少源文件: " + p)
            return 2

    cfg_src = strip_comments(open(CFG, encoding="utf-8", errors="replace").read())
    cmd_src = strip_comments(open(CMDM, encoding="utf-8", errors="replace").read())

    print("\n[C-1] 正向：结果层每个顶层键都必须被封包层转发")
    good, missing, detail = check(cfg_src, cmd_src)
    ok("C-1a 结果层/转发键集合可解析", good or bool(missing), detail)
    ok("C-1b 无遗漏键", good, "遗漏: %s" % sorted(missing) if missing else "")

    print("\n[C-2] 逐键断言（回归时能直接指出是哪个键被丢了）")
    a, _ = result_top_keys(cfg_src)
    b, _ = envelope_forwarded_keys(cmd_src)
    for k in sorted((a or set()) - WHOLESALE):
        ok("C-2·%s 已转发" % k, k in b, "封包层缺少 rv[\"%s\"] 转发" % k)

    print("\n[C-3] 负向探针：人为删掉 restart_in_ms 的转发后，检查器必须报错")
    broken = cmd_src.replace('rv["restart_in_ms"]', 'rv["__removed__"]')
    ok("C-3a 探针确实改动了源文本", broken != cmd_src)
    bad_ok, bad_missing, _ = check(cfg_src, broken)
    ok("C-3b 检查器能识别遗漏", (bad_ok is False) and ("restart_in_ms" in bad_missing),
       "missing=%s" % sorted(bad_missing))

    print("\n" + "=" * 66)
    print("PASS %d / FAIL %d" % (passes[0], len(failures)))
    if failures:
        for f in failures:
            print("  ! " + f)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
