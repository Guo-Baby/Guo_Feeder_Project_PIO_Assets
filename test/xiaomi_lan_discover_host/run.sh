#!/usr/bin/env bash
# =====================================================
# 米家 LAN 动态发现决策 —— 宿主侧探针（wasm32 freestanding + node）
#   ./run.sh                 # 编译并运行；退出码 0 = 全部用例通过
#
# 只编译纯头文件 src/app/xiaomi_lan_discover.h（零依赖）
# ⇒ 执行的是固件同一份代码（不是复制品）。
#
# 覆盖任务书 §3.1：双设备发现 / 身份归属 / IP 变化 / 缓存失效 /
#   WiFi 重连 / 认证失败 / 离线 / 不互换 / 重启不依赖旧 IP。
# =====================================================
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
cd "$ROOT"

CXX="${CXX:-/c/Program Files/LLVM/bin/clang++.exe}"
NODE="${NODE:-C:/Users/wang/.workbuddy/binaries/node/versions/22.22.2-6/node.exe}"
OUT="$ROOT/.pio/xlan_discover_host"
mkdir -p "$OUT"

COMMON="-target wasm32 -std=gnu++17 -nostdlib -fno-builtin -O1 -I src"
LINK="-Wl,--no-entry -Wl,--export-all"

"$CXX" $COMMON "$HERE/probe.cpp" $LINK -o "$OUT/probe.wasm" || exit 3
"$NODE" "$HERE/../device_model_host/run_probe.js" "$OUT/probe.wasm"
exit $?
