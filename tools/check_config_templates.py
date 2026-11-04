#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
check_config_templates.py —— 配置模板 ↔ 代码读取键 一致性守卫
================================================================================
背景（2026-11-05 遥测中断事故）
--------------------------------------------------------------------------------
`config_get_telemetry_enable()` 读 `module_ro(CONFIG_MODULE_MQTT)["telemetry_enable"]`，
但 `data/config/mqtt.json(.example)` 里**没有这个键**（2026-11-04 的 open-source
清理提交 `9224b20` 重写模板时把它删掉了）。

后果不是"读不到就崩"，而是**静默降级**：
    键缺失 → getter 的 `| false` 兜底 → 运行时门关闭 →
    `telemetry_task()` 最前置 return → 零遥测上行；
而 log 通道不受该门控制 ⇒ 设备在线、命令正常、日志正常，
**只有历史曲线停了，且设备侧一行日志都没有**。故障静默了 10 小时。

⇒ 这类"代码读的键 / 模板提供的键"不一致，必须由机器在构建期拦住，
  不能依赖任何人记得。

检查内容
--------------------------------------------------------------------------------
1. **硬门控键**（CRITICAL）：必须出现在对应模板中，缺失 ⇒ exit 1。
   这些键是"缺了就会静默关闭核心链路"的开关，值得硬失败。
2. **软一致性**（WARN）：从 `config_manager.cpp` 提取所有
   `module_ro(CONFIG_MODULE_X)["key"]` 读取点，与模板键集合比对。
   代码读但模板没有 ⇒ 打印 WARN（不失败，因为大量键靠 `| 默认值` 合法兜底）。
   模板有但代码不读 ⇒ 打印 WARN（可能是死配置，值得清理）。

用法
--------------------------------------------------------------------------------
    python tools/check_config_templates.py           # 正常检查
    python tools/check_config_templates.py --strict  # WARN 也算失败

退出码：0 = 通过 / 1 = 硬门控键缺失（或 --strict 下有 WARN）
================================================================================
"""

import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CFG_DIR = os.path.join(ROOT, "data", "config")
SRC = os.path.join(ROOT, "src", "services", "config_manager.cpp")
HDR = os.path.join(ROOT, "src", "services", "config_manager.h")

# ── 硬门控键：缺失即失败 ──
#   选取标准 = "该键为 false/缺失时会静默关闭一条核心链路，且不产生任何告警"
CRITICAL_KEYS = {
    ("mqtt", "telemetry_enable"): "遥测上行总门（缺失 ⇒ 历史曲线永久无数据，无告警）",
    ("weight", "enable"): "称重通道使能（false ⇒ weight.value 永不产生）",
    ("xiaomi_lan", "feeder_enable"): "米家喂食器 LAN 控制使能",
    ("xiaomi_lan", "plug_enable"): "米家插座 LAN 控制使能",
}


def load_module_macros():
    """从 config_manager.h 提取 CONFIG_MODULE_xxx → 字符串值。"""
    macros = {}
    if not os.path.isfile(HDR):
        return macros
    with open(HDR, encoding="utf-8", errors="replace") as f:
        text = f.read()
    for m in re.finditer(r'^\s*#define\s+(CONFIG_MODULE_\w+)\s+"([^"]*)"', text, re.M):
        macros[m.group(1)] = m.group(2)
    return macros


def template_keys(module):
    """返回该模块模板提供的键集合。优先 .json（生产），其次 .json.example。"""
    out = {}
    for suffix in (".json", ".json.example"):
        path = os.path.join(CFG_DIR, module + suffix)
        if not os.path.isfile(path):
            continue
        try:
            with open(path, encoding="utf-8") as f:
                data = json.load(f)
        except Exception as e:
            out[suffix] = ("__parse_error__", str(e))
            continue
        if isinstance(data, dict):
            out[suffix] = set(data.keys())
    return out


def code_read_keys(macros):
    """从 config_manager.cpp 提取 (module, key) 读取点。"""
    pairs = set()
    if not os.path.isfile(SRC):
        return pairs
    with open(SRC, encoding="utf-8", errors="replace") as f:
        text = f.read()

    # 去掉行注释，避免把注释里的示例当成真实读取点
    text = re.sub(r"//[^\n]*", "", text)

    pat = re.compile(r'module_ro\(\s*(CONFIG_MODULE_\w+)\s*\)\s*\[\s*"([^"]+)"\s*\]')
    for m in pat.finditer(text):
        module = macros.get(m.group(1))
        if module:
            pairs.add((module, m.group(2)))
    return pairs


def main():
    strict = "--strict" in sys.argv

    macros = load_module_macros()
    if not macros:
        print("[FAIL] 无法从 config_manager.h 解析 CONFIG_MODULE_* 宏")
        return 1

    reads = code_read_keys(macros)
    print("[info] 代码读取点 %d 个，模块宏 %d 个" % (len(reads), len(macros)))

    # ── 1. 硬门控键 ──
    #
    # ★ 判定必须是 **AND（每个存在的模板都要有）**，不能是 OR（任一有即可）。
    #   负向自检抓到过这个漏洞：当时 `data/config/mqtt.json`（本地生产）
    #   含 `telemetry_enable` 而 `.json.example`（公开契约 / 播种源）缺它，
    #   OR 判定直接 PASS —— 而事故的真正成因恰恰就是"某个模板缺键"。
    #   只要存在一条"用缺键模板播种"的路径，故障就会复发。
    failures = []
    for (module, key), why in sorted(CRITICAL_KEYS.items()):
        tpl = template_keys(module)
        if not tpl:
            failures.append("%s.%s —— 模板文件不存在（%s.json / .json.example）[%s]"
                            % (module, key, module, why))
            continue
        bad = []
        good = []
        for suffix, keys in tpl.items():
            if isinstance(keys, tuple):
                bad.append("%s 解析失败(%s)" % (suffix, keys[1]))
                continue
            (good if key in keys else bad).append(suffix)
        if bad:
            failures.append("%s.%s —— 模板缺该键: %s（含有的: %s）[%s]"
                            % (module, key, ", ".join(bad),
                               ", ".join(good) if good else "无", why))
        else:
            print("[ OK ] 硬门控 %s.%s  (%s 全部含)"
                  % (module, key, ", ".join(good)))

    # ── 2. 软一致性 ──
    warns = []
    by_module = {}
    for module, key in reads:
        by_module.setdefault(module, set()).add(key)

    for module in sorted(by_module):
        tpl = template_keys(module)
        if not tpl:
            warns.append("%s —— 代码读取 %d 个键，但无模板文件"
                         % (module, len(by_module[module])))
            continue
        # 以 .json.example 为主（公开契约）；没有则用 .json
        keys = tpl.get(".json.example")
        if isinstance(keys, tuple) or keys is None:
            keys = tpl.get(".json")
        if isinstance(keys, tuple) or keys is None:
            continue
        missing = sorted(by_module[module] - keys)
        if missing:
            warns.append("%s —— 代码读取但模板未声明: %s"
                         % (module, ", ".join(missing)))

    for w in warns:
        print("[WARN] " + w)

    # ── 3. 结论 ──
    print("")
    if failures:
        print("[FAIL] 硬门控键缺失 %d 项：" % len(failures))
        for f in failures:
            print("   - " + f)
        return 1

    if strict and warns:
        print("[FAIL] --strict：存在 %d 条 WARN" % len(warns))
        return 1

    print("[PASS] 硬门控键齐备%s"
          % ("；软一致性 WARN 不影响退出码" if warns else "；无 WARN"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
