#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
生成 data/config/version.json（ConfigManager 版本基线）。

用法:
    python tools/gen_config_version.py [--dir data/config] [--version 1]

背景
----
`data/` 被 .gitignore 忽略（含 WiFi 密码 / CA 证书等），
因此 data/config/version.json 不在版本库里，clone 后不会存在。

设备侧已有自愈（config_init 步骤 6 的 bootstrap_version_file），
缺少该文件时会自动建立版本基线。但为了让 uploadfs 之后版本号
【立即】就位（而不是等第一次启动自愈），仍建议随镜像一起烧录。

本脚本扫描 <dir>/*.json（跳过 version.json 自身与 .bak），
为每个模块写入初始版本号。

⚠️ 与 src/config_manager.cpp 的 CONFIG_MODULE_* 保持一致：
   若新增模块，本脚本会自动纳入（按文件名），无需改动。
"""
import argparse
import json
import os
import sys

SKIP_SUFFIX = (".bak",)
SKIP_NAME = {"version.json"}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dir", default=os.path.join("data", "config"))
    ap.add_argument("--version", type=int, default=1)
    args = ap.parse_args()

    if not os.path.isdir(args.dir):
        print("[ERR] 目录不存在: %s" % args.dir, file=sys.stderr)
        return 1

    modules = []
    for name in sorted(os.listdir(args.dir)):
        if not name.endswith(".json"):
            continue
        if name in SKIP_NAME:
            continue
        if name.endswith(SKIP_SUFFIX):
            continue
        modules.append(name[:-len(".json")])

    if not modules:
        print("[ERR] %s 下没有找到任何模块 JSON" % args.dir, file=sys.stderr)
        return 1

    doc = {m: args.version for m in modules}
    out = os.path.join(args.dir, "version.json")
    with open(out, "w", encoding="utf-8") as f:
        json.dump(doc, f, separators=(",", ":"), ensure_ascii=False)
        f.write("\n")

    print("[GEN] %s" % out)
    print("[OK ] %d 个模块，初始版本 = %d" % (len(modules), args.version))
    print("      %s" % ", ".join(modules))
    return 0


if __name__ == "__main__":
    sys.exit(main())
