#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Config dirty 契约核对（源码级，不需要设备）—— T05 · 2026-11-01

用途
----
`config_query` 在 2026-11-01 增补了两个**顶层**键（纯新增、不改既有键）：

    dirty         : { module_name: bool }   —— 该模块 RAM 里有未落盘修改
    restart_in_ms : number                  —— 自动保存+重启窗口的剩余毫秒（0=无）

前端靠它把「Config dirty」从"会话内存猜"变成"设备侧事实"。
这个脚本作为**可重复执行的回归守卫**，防两类事故：

  1. 固件后来重构 `exec_query_all` 时把 dirty / restart_in_ms 漏掉
     ⇒ 前端静默退回"未知"，刷新后配置条又不出现（缺陷复发）
  2. 前端改字段名（dirty → is_dirty / restart_in_ms → remain_ms）
     ⇒ 契约悄悄断裂，而两边单测都不报错

它**不能**替代真机验证（真机还要确认 config_set → dirty=true → config_save → dirty=false）。

覆盖
----
  D-1  ConfigModule 结构里有 `dirty` 字段（事实来源）
  D-2  `exec_query_all` 输出 `dirty` 映射，且取的是 `s_modules[i].dirty`
  D-3  `exec_query_all` 输出 `restart_in_ms`，来源是 `config_restart_pending()`
  D-4  `exec_query_module` 也输出 `dirty`（与全量口径一致，避免两页自相矛盾）
  D-5  既有的 `data` / `versions` **没有被改动**（兼容性）
  D-6  没有新增命令类型（仍是 CONFIG_CMD_QUERY_* 三条查询）
  D-7  前端解析的字段名与固件完全一致（跨仓核对，缺失则跳过并提示）

运行
----
    python test/ui_v11_contract/config_dirty_contract.py
退出码 0 = 全通过；1 = 有失败项。
"""

import re
import sys
import pathlib

HERE = pathlib.Path(__file__).resolve()
ROOT = HERE.parents[2]                      # PIO 工程根（Guo_Feeder_Project）
SRC = ROOT / "src"
CF = ROOT.parents[1] / "Cloudflare_Assets"  # 工作区根下的云端仓（跨仓核对用）

fails = []
passes = []


def ok(name, cond, detail=""):
    if cond:
        passes.append(name)
        print("  PASS  " + name)
    else:
        fails.append(name + (("  <- " + detail) if detail else ""))
        print("  FAIL  " + name + (("  <- " + detail) if detail else ""))


def read(rel, base=None):
    p = (base or SRC) / rel
    if not p.exists():
        print("!! 缺少源文件: " + str(p))
        return ""
    return p.read_text(encoding="utf-8", errors="replace")


def func_body(src, signature, stop_pat=r"\nstatic ExecResult |\nstatic bool |\nbool |\nvoid "):
    """截取一个函数体（从 signature 起，到下一个顶层定义为止）。"""
    i = src.find(signature)
    if i < 0:
        return ""
    m = re.search(stop_pat, src[i + len(signature):])
    j = i + len(signature) + (m.start() if m else len(src))
    return src[i:j]


def main():
    cm_h = read("services/config_manager.h")
    cm = read("services/config_manager.cpp")

    print("==== T05 · Config dirty 契约核对 ====\n")

    # ---------------- D-1 结构里有 dirty 字段 ----------------
    m = re.search(r"struct\s+ConfigModule\s*\{(.*?)\n\};", cm_h, re.S)
    struct_body = m.group(1) if m else ""
    ok("D-1a 找到 ConfigModule 结构定义", bool(struct_body))
    ok("D-1b ConfigModule 有 dirty 字段（RAM 未落盘标志）",
       bool(re.search(r"\bbool\s+dirty\b", struct_body)),
       struct_body[:200])

    # ---------------- D-2 exec_query_all 输出 dirty 映射 ----------------
    qa = func_body(cm, "static ExecResult exec_query_all(")
    ok("D-2a 找到 exec_query_all 实现", bool(qa))
    ok("D-2b 创建 dirty 子对象",
       bool(re.search(r'out\["dirty"\]\s*\.to<JsonObject>\(\)', qa)),
       qa[:0] or "")
    ok("D-2c dirty 取值来自 s_modules[i].dirty（不是猜的）",
       bool(re.search(r"dirtyMap\[name\]\s*\.set\(\s*s_modules\[i\]\.dirty\s*\)", qa)),
       "")
    ok("D-2d dirty 写入失败会被检查（内存不足不静默）",
       "dirtyMap[name].set(s_modules[i].dirty)" in qa
       and qa.count("result_fail(") >= 2)

    # ---------------- D-3 restart_in_ms 来源正确 ----------------
    ok("D-3a exec_query_all 输出 restart_in_ms",
       bool(re.search(r'out\["restart_in_ms"\]\s*\.set\(', qa)), qa[-400:])
    ok("D-3b 剩余时间来自 config_restart_pending()（含 SystemCommand 重启窗口，不自己算）",
       "config_restart_pending(" in qa, qa[-400:])
    ok("D-3c 无待重启时明确写 0（不省略键，避免前端把缺失当 0）",
       bool(re.search(r"if\s*\(\s*!\s*pending\s*\)\s*\{?\s*remainMs\s*=\s*0", qa)),
       qa[-400:])

    # ---------------- D-4 单模块查询口径一致 ----------------
    qm = func_body(cm, "static ExecResult exec_query_module(")
    ok("D-4a 找到 exec_query_module 实现", bool(qm))
    ok("D-4b 单模块查询也输出 dirty（与全量一致）",
       qm.count('out["dirty"].set(m->dirty)') >= 2,
       "出现 %d 次（keys_only 分支 + 完整分支应各一次）"
       % qm.count('out["dirty"].set(m->dirty)'))

    # ---------------- D-5 既有字段未被改动 ----------------
    ok("D-5a 全量查询仍输出 data 与 versions",
       'out["data"].to<JsonObject>()' in qa and 'out["versions"].to<JsonObject>()' in qa)
    ok("D-5b 单模块查询仍输出 data 与 version（单数）",
       'out["data"].set(' in qm and 'out["version"].set(' in qm)
    ok("D-5c 没有把 dirty 塞进 data（否则会被当成配置键写回 Flash）",
       not re.search(r'data.*\["dirty"\]', qa))

    # ---------------- D-6 没有新增命令类型 ----------------
    ok("D-6a 查询仍走 CONFIG_CMD_QUERY_FIELD / MODULE / ALL 三条",
       "CONFIG_CMD_QUERY_FIELD" in cm and "CONFIG_CMD_QUERY_MODULE" in cm
       and "CONFIG_CMD_QUERY_ALL" in cm)
    ok("D-6b exec_query_all 仍挂在 CONFIG_CMD_QUERY_ALL 分支上",
       bool(re.search(r"case\s+CONFIG_CMD_QUERY_ALL:\s*\n\s*r\s*=\s*exec_query_all\(", cm)))

    # ---------------- D-7 跨仓字段名一致 ----------------
    gf_app = CF / "guo-feeder-pagesdev/assets/js/gf-app.js"
    gf_core = CF / "guo-feeder-pagesdev/assets/js/gf-ui-core.js"
    if gf_app.exists() and gf_core.exists():
        app = gf_app.read_text(encoding="utf-8", errors="replace")
        core = gf_core.read_text(encoding="utf-8", errors="replace")
        ok("D-7a 前端调用 parseConfigDirty 解析回帧",
           "parseConfigDirty(" in app)
        ok("D-7b 前端按 restart_in_ms 取值（字段名与固件一致）",
           'restart_in_ms' in core, "")
        ok("D-7c 前端按 dirty 取值（字段名与固件一致）",
           'p.dirty' in core or '.dirty &&' in core, "")
        ok("D-7d 前端把 dirty 写进 S.configDeviceDirty（唯一落点）",
           "S.configDeviceDirty = {" in app)
    else:
        print("  SKIP  跨仓核对（找不到 Cloudflare_Assets：%s）" % CF)

    print("\n==== 汇总 PASS=%d FAIL=%d ====" % (len(passes), len(fails)))
    if fails:
        print("\n失败项：")
        for f in fails:
            print("  - " + f)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
