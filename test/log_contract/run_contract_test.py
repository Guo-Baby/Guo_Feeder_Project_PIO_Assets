#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
P1.1 Contract Test 运行器（离线，无需硬件/Arduino 框架）

用法:
    python test/log_contract/run_contract_test.py

原理:
    log_events.h 是 freestanding 的（只依赖 stdint.h / stddef.h），
    因此可以用项目自身的交叉编译器直接做 -fsyntax-only 编译，
    在数秒内完成全部 static_assert 的求值，不必等完整 PlatformIO 构建。

测试内容:
    [1] 正向探针 probe_ok.cpp  → 必须编译通过（证明契约值全部正确）
    [2] 负向探针 probe_bad.cpp → 必须编译失败（证明断言真的被执行）

只用 [1] 是不够的：若头文件的断言被某种方式跳过，[1] 也会"通过"。
[2] 的存在使 Contract Test 具备"可证伪性"。
"""

import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))          # 项目根
SRC = os.path.join(ROOT, "src")

GXX_CANDIDATES = [
    r"D:\platformIO\packages\toolchain-xtensa-esp32s3\bin\xtensa-esp32s3-elf-g++.exe",
    r"C:\Users\wang\.platformio\packages\toolchain-xtensa-esp32s3\bin\xtensa-esp32s3-elf-g++.exe",
]

CXXFLAGS = ["-std=gnu++11", "-fsyntax-only", "-I" + SRC]


def find_gxx():
    for p in GXX_CANDIDATES:
        if os.path.isfile(p):
            return p
    return None


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


def main():
    gxx = find_gxx()
    if gxx is None:
        print("SKIP: 未找到 xtensa-esp32s3-elf-g++，无法离线运行 Contract Test")
        print("      （完整 PlatformIO 构建通过同样已证明断言成立）")
        return 2

    print("toolchain: %s" % gxx)
    print("src      : %s" % SRC)
    print("-" * 56)

    ok1 = run(gxx, os.path.join(HERE, "probe_ok.cpp"), True, "1/2 positive")
    ok2 = run(gxx, os.path.join(HERE, "probe_bad.cpp"), False, "2/2 negative")

    print("-" * 56)
    if ok1 and ok2:
        print("CONTRACT TEST: ALL PASS")
        return 0
    print("CONTRACT TEST: FAIL")
    return 1


if __name__ == "__main__":
    sys.exit(main())
