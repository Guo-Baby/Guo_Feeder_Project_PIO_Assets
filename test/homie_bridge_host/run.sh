#!/usr/bin/env bash
# =====================================================
# P2-3 · Homie Projection —— 宿主探针（wasm32 + node，无 CRT / 无硬件 / 无 broker）
#
#   ./run.sh
#   CXX=clang++ NODE=<node> ./run.sh
#
# 退出码：0 = 全部断言通过（fail == 0）
# =====================================================
set -u

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
cd "$ROOT"

CXX="${CXX:-clang++}"
NODE="${NODE:-C:/Users/wang/.workbuddy/binaries/node/versions/22.22.2-6/node.exe}"
RUNNER="$ROOT/test/device_model_host/run_probe.js"   # 通用 wasm 探针运行器（复用）

# freestanding Arduino 垫片（仅类型声明；不参与固件构建）
# homie_projection.h → device_model.h → system_state.h → Arduino.h
SHIM="$ROOT/.pio/p21check"
if [ ! -f "$SHIM/Arduino.h" ]; then
  mkdir -p "$SHIM"
  printf '#pragma once\n#include <stdint.h>\nstruct String {};\n' > "$SHIM/Arduino.h"
fi

OUT="$ROOT/.pio/hb_host"
mkdir -p "$OUT"

COMMON="-target wasm32 -std=gnu++17 -nostdlib -fno-builtin -O1 -I $SHIM -I src"
LINK="-Wl,--no-entry -Wl,--export-all"

echo "### compile + run homie projection probe"
$CXX $COMMON "$HERE/hb_probe.cpp" src/homie/device_model.cpp $LINK -o "$OUT/hb_probe.wasm" || exit 3
"$NODE" "$RUNNER" "$OUT/hb_probe.wasm"
RC=$?
echo "probe_exit=$RC"
exit $RC
