#!/usr/bin/env bash
# =====================================================
# P2-2 · MQTT Transport —— 宿主探针（wasm32 + node，无 CRT / 无硬件）
#
#   ./run.sh
#   CXX=clang++ NODE=<node> ./run.sh
#
# 退出码：0 = 探针全通过（fail == 0）
# =====================================================
set -u

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
cd "$ROOT"

CXX="${CXX:-clang++}"
NODE="${NODE:-C:/Users/wang/.workbuddy/binaries/node/versions/22.22.2-6/node.exe}"
RUNNER="$ROOT/test/device_model_host/run_probe.js"   # 通用 wasm 探针运行器（复用）

OUT="$ROOT/.pio/tp_host"
mkdir -p "$OUT"

COMMON="-target wasm32 -std=gnu++17 -nostdlib -fno-builtin -O1 -I src"
LINK="-Wl,--no-entry -Wl,--export-all"

echo "### compile + run transport probe"
$CXX $COMMON "$HERE/tp_probe.cpp" $LINK -o "$OUT/tp_probe.wasm" || exit 3
"$NODE" "$RUNNER" "$OUT/tp_probe.wasm"
RC=$?
echo "probe_exit=$RC"
exit $RC
