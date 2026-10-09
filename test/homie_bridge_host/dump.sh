#!/usr/bin/env bash
# =====================================================
# P2-7 · Homie 软件侧制品 Dump 运行器（wasm32 + node，无硬件 / 无 broker）
#
#   ./dump.sh
#   CXX=clang++ NODE=<node> ./dump.sh
#
# 说明：本脚本**只打印**纯逻辑层制品（topic / $description / ID / set parser），
#       不做判定、不影响既有回归（homie_bridge_host/run.sh 未改动）。
#       退出码：0 = 运行成功（打印完整）。
# =====================================================
set -u

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
cd "$ROOT"

CXX="${CXX:-clang++}"
NODE="${NODE:-C:/Users/wang/.workbuddy/binaries/node/versions/22.22.2-6/node.exe}"
RUNNER="$ROOT/test/device_model_host/run_probe.js"

# freestanding Arduino 垫片（仅类型声明；不参与固件构建）
SHIM="$ROOT/.pio/p21check"
if [ ! -f "$SHIM/Arduino.h" ]; then
  mkdir -p "$SHIM"
  printf '#pragma once\n#include <stdint.h>\nstruct String {};\n' > "$SHIM/Arduino.h"
fi

OUT="$ROOT/.pio/hb_host"
mkdir -p "$OUT"

COMMON="-target wasm32 -std=gnu++17 -nostdlib -fno-builtin -O1 -I $SHIM -I src"
LINK="-Wl,--no-entry -Wl,--export-all"

echo "### compile + run homie software artifacts dump"
$CXX $COMMON "$HERE/artifacts_dump.cpp" src/homie/device_model.cpp $LINK -o "$OUT/artifacts_dump.wasm" || exit 3
"$NODE" "$RUNNER" "$OUT/artifacts_dump.wasm"
RC=$?
echo "dump_exit=$RC"
exit $RC
