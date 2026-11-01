#!/usr/bin/env bash
# =====================================================
# Workflow 定时触发策略 —— 宿主机探针（wasm32 freestanding + node）
#
#   ./run.sh                    # 编译并运行；退出码 0 = 全部通过
#   CXX=clang++ NODE=node ./run.sh
#
# 为什么不用原生可执行文件：
#   本机 clang++ 无 MSVC libc 头文件（stdio.h 找不到）⇒ 采用与
#   test/loop_stats_host、test/mem_watch_host 相同的 wasm32 freestanding 方案，
#   由探针自带 memcpy/memset，纯逻辑在 node 里真实执行。
#
# 只编译**纯策略头**（src/automation/wf_sched_policy.h 是 header-only）：
#   workflow.cpp 依赖 Arduino/ESP-IDF/Preferences/LittleFS，不参与主机编译。
# =====================================================
set -u

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
cd "$ROOT"

CXX="${CXX:-clang++}"
NODE="${NODE:-C:/Users/wang/.workbuddy/binaries/node/versions/22.22.2-6/node.exe}"

OUT="$ROOT/.pio/wfsched_host"
mkdir -p "$OUT"

FLAGS="-target wasm32 -std=gnu++17 -nostdlib -fno-builtin -O1 -Wall -Wextra -Wno-unused-parameter"
INC="-I src"
LINK="-Wl,--no-entry -Wl,--export-all"

echo "### compile probe (wasm32 freestanding)"
$CXX $FLAGS $INC -DWF_SCHED_HOST_TEST \
     "$HERE/sched_probe.cpp" \
     $LINK -o "$OUT/sched_probe.wasm" || exit 3

echo "### run probe (node)"
"$NODE" "$HERE/run_probe.js" "$OUT/sched_probe.wasm"
FAIL=$?

echo
if [ "$FAIL" -eq 0 ]; then
  echo "### RESULT: PASS (0 failures)"
  exit 0
fi
echo "### RESULT: FAIL ($FAIL failures)"
exit 1
