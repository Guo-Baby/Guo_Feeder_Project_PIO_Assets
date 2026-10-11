#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Telemetry 主机测试运行器（离线，无需硬件 / Arduino / MQTT 对端）

用法:
    python test/telemetry_host/run_telemetry_test.py

原理:
    telemetry_policy.h / telemetry_envelope.h 都是 freestanding 的
    （只依赖 stdint.h），因此可用主机 clang++ 编到 wasm32 并由 node
    **真实执行**，对决策逻辑与 wire 格式做逻辑级验证。

    wasm32 的 wasm 运行器复用 test/log_contract/run_wasm_probe.js
    （该脚本是通用的：只需导出 probe_run / probe_log_ptr / probe_log_len）。

退出码: 0 = 全部通过；1 = 有失败；2 = 环境缺失 / 编译失败
"""

import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))          # 项目根
SRC = os.path.join(ROOT, "src")
SRC_SERVICES = os.path.join(SRC, "services")

JS_RUNNER = os.path.join(ROOT, "test", "log_contract", "run_wasm_probe.js")

HOST_CXX_CANDIDATES = [
    r"C:\Program Files\LLVM\bin\clang++.exe",
    r"C:\msys64\mingw64\bin\g++.exe",
    r"C:\MinGW\bin\g++.exe",
]

NODE_CANDIDATES = [
    r"C:\Users\wang\.workbuddy\binaries\node\versions\22.22.2-6\node.exe",
    r"C:\Users\wang\.workbuddy\binaries\node\versions\22.22.2-3\node.exe",
    r"D:\Program Files\nodejs\node.exe",
]


def find_host_cxx():
    for p in HOST_CXX_CANDIDATES:
        if os.path.isfile(p):
            return p
    for name in ("clang++", "g++"):
        w = shutil.which(name)
        if w:
            return w
    return None


def find_node():
    for p in NODE_CANDIDATES:
        if os.path.isfile(p):
            return p
    return shutil.which("node")


def main():
    cxx = find_host_cxx()
    node = find_node()

    print("clang++ : %s" % (cxx or "(未找到)"))
    print("node    : %s" % (node or "(未找到)"))
    print("src     : %s" % SRC)
    print("-" * 56)

    if cxx is None or node is None:
        miss = []
        if cxx is None:
            miss.append("clang++/g++")
        if node is None:
            miss.append("node")
        print("TELEMETRY TEST: SKIP : 未找到 %s" % "/".join(miss))
        return 2

    if not os.path.isfile(JS_RUNNER):
        print("TELEMETRY TEST: FAIL : 缺少 wasm 运行器 %s" % JS_RUNNER)
        return 2

    out_dir = os.path.join(ROOT, ".pio")
    os.makedirs(out_dir, exist_ok=True)
    wasm = os.path.join(out_dir, "probe_telemetry.wasm")

    cmd = [cxx, "--target=wasm32", "-std=gnu++11", "-nostdlib", "-fno-builtin",
           "-O1", "-I" + SRC, "-I" + SRC_SERVICES,
           os.path.join(HERE, "probe_telemetry.cpp"),
           "-Wl,--no-entry", "-Wl,--export-all", "-o", wasm]

    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=180)
    except Exception as e:                                  # noqa: BLE001
        print("TELEMETRY TEST: FAIL : 编译异常 %s" % e)
        return 2

    if r.returncode != 0 or not os.path.isfile(wasm):
        print("TELEMETRY TEST: FAIL : wasm 编译失败")
        print("---- 编译器输出 ----")
        print("\n".join(((r.stdout or "") + (r.stderr or "")).strip().splitlines()[:16]))
        print("--------------------")
        return 2

    try:
        r2 = subprocess.run([node, JS_RUNNER, wasm],
                            capture_output=True, text=True, timeout=120)
    except Exception as e:                                  # noqa: BLE001
        print("TELEMETRY TEST: FAIL : 运行异常 %s" % e)
        return 2

    for line in (r2.stdout or "").strip().splitlines():
        print("   " + line)

    if r2.returncode != 0:
        print("TELEMETRY TEST: FAIL （exit=%d）" % r2.returncode)
        return 1

    print("-" * 56)
    print("TELEMETRY TEST: ALL PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
