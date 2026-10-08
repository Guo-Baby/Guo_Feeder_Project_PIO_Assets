#!/usr/bin/env bash
# =====================================================
# Device Model 宿主探针（wasm32 + node，无 CRT / 无硬件）
#
#   ./run.sh                 # 正/负向探针各编译并运行
#   CXX=clang++ NODE=node ./run.sh
#
# 退出码：0 = 正向全通过 **且** 负向如期失败（可证伪）
# =====================================================
set -u

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
cd "$ROOT"

CXX="${CXX:-clang++}"
NODE="${NODE:-C:/Users/wang/.workbuddy/binaries/node/versions/22.22.2-6/node.exe}"

# freestanding Arduino 垫片（仅语法/类型；不参与固件构建）
SHIM="$ROOT/.pio/p21check"
if [ ! -f "$SHIM/Arduino.h" ]; then
  mkdir -p "$SHIM"
  printf '#pragma once\n#include <stdint.h>\nstruct String {};\n' > "$SHIM/Arduino.h"
fi

OUT="$ROOT/.pio/dm_host"
mkdir -p "$OUT"

COMMON="-target wasm32 -std=gnu++17 -nostdlib -fno-builtin -O1 -I $SHIM -I src"
LINK="-Wl,--no-entry -Wl,--export-all"

echo "### [1/2] POSITIVE probe"
$CXX $COMMON "$HERE/dm_probe.cpp" src/homie/device_model.cpp $LINK -o "$OUT/dm_probe.wasm" || exit 3
"$NODE" "$HERE/run_probe.js" "$OUT/dm_probe.wasm"
POS=$?
echo "positive_exit=$POS"

echo
echo "### [2/2] NEGATIVE probe (must report failures)"
$CXX $COMMON "$HERE/dm_probe_negative.cpp" src/homie/device_model.cpp $LINK -o "$OUT/dm_probe_neg.wasm" || exit 3
"$NODE" "$HERE/run_probe.js" "$OUT/dm_probe_neg.wasm"
NEG=$?
echo "negative_exit=$NEG"

echo
if [ "$POS" -eq 0 ] && [ "$NEG" -ne 0 ]; then
  echo "### RESULT: harness OK (positive PASS, negative correctly FAILED)"
  exit 0
else
  echo "### RESULT: harness INVALID (positive=$POS negative=$NEG)"
  exit 1
fi
