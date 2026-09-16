#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Log 契约测试运行器（离线，无需硬件/Arduino 框架/MQTT 对端）

用法:
    python test/log_contract/run_contract_test.py

原理:
    log_events.h / log_cbor.h / log_ack.h 都是 freestanding 的
    （只依赖 stdint.h / stddef.h / string.h），因此：
      · 可用项目自身的交叉编译器做 -fsyntax-only 编译，
        在数秒内完成全部 static_assert 的求值，不必等完整 PlatformIO 构建；
      · 可用主机 clang++ 编到 wasm32 并由 node **真实执行**，
        对 wire 格式与 ACK 状态机做逻辑级验证。

测试内容:
    [1] 正向探针 probe_ok.cpp    → 必须编译通过（证明契约值全部正确）
    [2] 负向探针 probe_bad.cpp   → 必须编译失败（证明断言真的被执行）
    [3] CBOR 探针 probe_cbor.cpp → wasm32 真跑：P1.4 批次编码逐字节验证
    [4] ACK  探针 probe_ack.cpp  → wasm32 真跑：P1.5 ACK/退避/放弃/段可删性

只用 [1] 是不够的：若头文件的断言被某种方式跳过，[1] 也会"通过"。
[2] 的存在使 Contract Test 具备"可证伪性"。
[3][4] 是本项目在**没有开发板**时唯一能"真跑"的路子 ——
    本机 clang 缺 MSVC/CRT 库，链接不了原生控制台程序，wasm 零依赖。
"""

import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))          # 项目根
SRC = os.path.join(ROOT, "src")

GXX_CANDIDATES = [
    r"D:\platformIO\packages\toolchain-xtensa-esp32s3\bin\xtensa-esp32s3-elf-g++.exe",
    r"C:\Users\wang\.platformio\packages\toolchain-xtensa-esp32s3\bin\xtensa-esp32s3-elf-g++.exe",
]

HOST_CXX_CANDIDATES = [
    r"C:\Program Files\LLVM\bin\clang++.exe",
    r"C:\msys64\mingw64\bin\g++.exe",
    r"C:\MinGW\bin\g++.exe",
]

CXXFLAGS = ["-std=gnu++11", "-fsyntax-only", "-I" + SRC]


def find_gxx():
    for p in GXX_CANDIDATES:
        if os.path.isfile(p):
            return p
    return None


def find_host_cxx():
    for p in HOST_CXX_CANDIDATES:
        if os.path.isfile(p):
            return p
    for name in ("clang++", "g++"):
        w = shutil.which(name)
        if w:
            return w
    return None


NODE_CANDIDATES = [
    r"C:\Users\wang\.workbuddy\binaries\node\versions\22.22.2-3\node.exe",
    r"D:\Program Files\nodejs\node.exe",
]


def find_node():
    for p in NODE_CANDIDATES:
        if os.path.isfile(p):
            return p
    w = shutil.which("node")
    return w


def run(gxx, cpp, expect_ok, label):
    cmd = [gxx] + CXXFLAGS + [cpp]
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=180)
        rc = r.returncode
        out = (r.stdout or "") + (r.stderr or "")
    except Exception as e:                                  # noqa: BLE001
        print("[%s] EXEC ERROR: %s" % (label, e))
        return False

    compiled_ok = (rc == 0)
    if expect_ok:
        passed = compiled_ok
        verdict = "PASS" if passed else "FAIL"
        detail = "编译通过（期望通过）" if passed else "编译失败（期望通过）"
    else:
        passed = not compiled_ok
        verdict = "PASS" if passed else "FAIL"
        if passed:
            mark = "NEGATIVE PROBE" in out
            detail = "编译失败且命中负向探针断言（期望失败）" if mark else \
                     "编译失败但未命中目标断言（需人工确认）"
            if not mark:
                passed = False
                verdict = "FAIL"
        else:
            detail = "编译通过（期望失败）—— 断言未生效！"

    print("[%s] %s : %s" % (label, verdict, detail))
    if not passed:
        tail = "\n".join(out.strip().splitlines()[:14])
        print("---- 编译器输出 ----")
        print(tail)
        print("--------------------")
    return passed


def run_wasm_probe(stem, label, pass_msg):
    """把 freestanding 探针编到 wasm32 并用 node **真实执行**。
    返回 True/False/None(跳过)。

    为什么绕道 wasm32：本机 clang 没有 MSVC/CRT 库（libcmt.lib 等缺失），
    链接不了原生控制台程序；而 wasm32 只要编译器自带内建头，零依赖。
    log_cbor.h / log_ack.h 都是 freestanding 的，所以这条路可行 ——
    于是 CBOR 字节格式与 ACK 状态机都能在**没有开发板**时被真实验证。
    """
    cxx = find_host_cxx()
    node = find_node()

    if cxx is None or node is None:
        miss = []
        if cxx is None:
            miss.append("clang++/g++")
        if node is None:
            miss.append("node")
        print("[%s] SKIP : 未找到 %s" % (label, "/".join(miss)))
        return None

    # 产物写进 .pio/（被 gitignore），不要污染受跟踪的 test/
    out_dir = os.path.join(ROOT, ".pio")
    if not os.path.isdir(out_dir):
        os.makedirs(out_dir, exist_ok=True)

    wasm = os.path.join(out_dir, "probe_%s.wasm" % stem)
    cmd = [cxx, "--target=wasm32", "-std=gnu++11", "-nostdlib", "-fno-builtin",
           "-O1", "-I" + SRC, os.path.join(HERE, "probe_%s.cpp" % stem),
           "-Wl,--no-entry", "-Wl,--export-all", "-o", wasm]
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=180)
    except Exception as e:                                  # noqa: BLE001
        print("[%s] EXEC ERROR: %s" % (label, e))
        return False

    if r.returncode != 0 or not os.path.isfile(wasm):
        print("[%s] FAIL : wasm 编译失败" % label)
        print("---- 编译器输出 ----")
        print("\n".join(((r.stdout or "") + (r.stderr or "")).strip().splitlines()[:14]))
        print("--------------------")
        return False

    try:
        r2 = subprocess.run([node, os.path.join(HERE, "run_wasm_probe.js"), wasm],
                            capture_output=True, text=True, timeout=120)
    except Exception as e:                                  # noqa: BLE001
        print("[%s] EXEC ERROR: %s" % (label, e))
        return False

    for line in (r2.stdout or "").strip().splitlines():
        print("   " + line)

    if r2.returncode != 0:
        print("[%s] FAIL : 探针未通过（exit=%d）" % (label, r2.returncode))
        if r2.stderr:
            print("   stderr: " + r2.stderr.strip().splitlines()[0])
        return False

    print("[%s] PASS : %s" % (label, pass_msg))
    return True


def main():
    host = find_host_cxx()
    gxx = find_gxx()
    node = find_node()

    print("clang++  : %s" % (host or "(未找到)"))
    print("node     : %s" % (node or "(未找到)"))
    print("x-compile: %s" % (gxx or "(未找到)"))
    print("src      : %s" % SRC)
    print("-" * 56)

    ok1 = ok2 = None
    if gxx is not None:
        ok1 = run(gxx, os.path.join(HERE, "probe_ok.cpp"), True, "1/4 positive")
        ok2 = run(gxx, os.path.join(HERE, "probe_bad.cpp"), False, "2/4 negative")
    else:
        print("[1/4 positive] SKIP : 未找到 xtensa 交叉编译器")
        print("[2/4 negative] SKIP : 同上")

    ok3 = run_wasm_probe(
        "cbor", "3/4 cbor    ",
        "21 B 头逐字节匹配 + records 130 B/条 + 越界/上界保护")

    ok4 = run_wasm_probe(
        "ack", "4/4 ack     ",
        "IGNORE/DUPLICATE/ACCEPT/PARTIAL 分类 + 覆盖前缀 + 段可删性 + 退避 + 放弃门槛")

    print("-" * 56)
    results = [ok1, ok2, ok3, ok4]
    if any(r is False for r in results):
        print("CONTRACT TEST: FAIL")
        return 1
    if any(r is None for r in results):
        print("CONTRACT TEST: PARTIAL PASS（有步骤被跳过，见上）")
        return 0
    print("CONTRACT TEST: ALL PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
