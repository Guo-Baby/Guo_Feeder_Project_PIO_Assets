#!/usr/bin/env bash
# =====================================================
# MemWatch 宿主机探针（wasm32 freestanding + node，无硬件 / 无 libc）
#
#   ./run.sh                 # 编译并运行；退出码 = 探针报告的 fail 数
#   CXX=clang++ NODE=node ./run.sh
#
# 为什么不用原生可执行文件：
#   本机 clang++ 无 MSVC libc 头文件（stdio.h 找不到）⇒ 采用与
#   test/device_model_host 相同的 wasm32 freestanding 方案，
#   由探针自带 memcpy/memset，纯逻辑在 node 里真实执行。
# =====================================================
set -u

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
cd "$ROOT"

CXX="${CXX:-clang++}"
NODE="${NODE:-C:/Users/wang/.workbuddy/binaries/node/versions/22.22.2-6/node.exe}"

OUT="$ROOT/.pio/g4r4mem_host"
mkdir -p "$OUT"

FLAGS="-target wasm32 -std=gnu++17 -nostdlib -fno-builtin -O1 -Wall -Wextra -Wno-unused-parameter"
INC="-I src"
LINK="-Wl,--no-entry -Wl,--export-all"

echo "### compile probe (wasm32 freestanding)"
# 只编译**纯计算层**（`mem_watch_calc.h` 是 header-only）。
# `src/services/mem_watch.cpp` 依赖 ESP-IDF（heap_caps / FreeRTOS），**不**参与主机编译。
$CXX $FLAGS $INC -DMEMWATCH_HOST_TEST \
     "$HERE/mem_watch_probe.cpp" \
     $LINK -o "$OUT/mem_watch_probe.wasm" || exit 3

echo "### run probe (node)"
"$NODE" "$HERE/run_probe.js" "$OUT/mem_watch_probe.wasm"
FAIL=$?

echo
if [ "$FAIL" -eq 0 ]; then
  echo "### RESULT: PASS (0 failures)"
  exit 0
fi
echo "### RESULT: FAIL ($FAIL failures)"
exit 1
