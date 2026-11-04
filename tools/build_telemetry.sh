#!/usr/bin/env bash
# =====================================================
# build_telemetry.sh —— 可复现的"遥测构型"构建 + 产物断言
#
# 为什么需要它
# ------------
# `GF_TELEMETRY_ENABLED` 默认 0 ⇒ 普通 `pio run` **不编入**遥测发布实现。
# 历史上靠 `PLATFORMIO_BUILD_FLAGS=...` 临时注入，既不可复现，也容易与
# "无遥测版"产物混淆（两者只差 1.7 KB，靠肉眼看大小根本分不清）。
#
# 本脚本做三件事：
#   ① 用**显式环境** esp32-s3-telemetry 构建（宏定义在 platformio.ini 里，可追溯）
#   ② 构建后**断言**产物里必须有 `climate.temperature`（遥测通道表字面量）
#      —— 没有就直接 exit 1，绝不产出"看着成功其实没遥测"的固件
#   ③ 打印绝对路径 / 大小 / SHA-256 / 构建时刻，便于归档与烧录前核对
#
# 用法
# ----
#     bash tools/build_telemetry.sh            # 增量构建
#     bash tools/build_telemetry.sh --clean    # 干净重建
# =====================================================
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT" || exit 1

ENV_NAME="esp32-s3-telemetry"
BIN="$ROOT/.pio/build/$ENV_NAME/firmware.bin"
PIO="${PIO:-/d/platformIO/penv/Scripts/pio.exe}"

CLEAN=0
[ "${1:-}" = "--clean" ] && CLEAN=1
if [ "${1:-}" = "--clean" ]; then CLEAN=1; fi

echo "== env: $ENV_NAME =="
echo "== 时间: $(date '+%Y-%m-%d %H:%M:%S') =="

# ---------------------------------------------------------------------
# ★ [WP-H · 2026-10-29] 构建前先归档现有产物。
#
# `pio run` **每次启动**都会调 `clean_build_dir()`：只要 `project.checksum`
# 与当前不一致（= platformio.ini 改过 / 增删过源文件），就直接
# `fs.rmtree(.pio/build)` ⇒ 历史 env 的 firmware.bin **全部消失**。
# 本轮就是这样丢掉了 xf3/xm1/xp1/xd2/xlan/nt1/默认 env 的产物。
# ⇒ 本次构建一旦改动配置，旧产物会先被删，所以必须**在调用 pio 之前**归档。
# ---------------------------------------------------------------------
if [ -d "$ROOT/.pio/build" ]; then
    bash "$ROOT/tools/archive_build_artifacts.sh" \
        || echo "WARN: 产物归档失败（继续构建；但 .pio/build 可能被 auto-clean 清空）"
    echo
fi

if [ "$CLEAN" = "1" ]; then
    echo "== clean =="
    "$PIO" run -e "$ENV_NAME" -t clean >/dev/null 2>&1
fi

# ---------------------------------------------------------------------
# ★★ [2026-11-05 · 遥测中断事故] 构建前先校验配置模板 ↔ 代码读取键。
#
# 事故成因之一：`9224b20`（2026-11-04 open-source 清理）重写
# `data/config/mqtt.json.example` 时把 `"telemetry_enable"` 删掉了。
# 产物本身"构建成功"，固件也照常运行 —— 因为缺键只导致运行时门读成
# false，遥测静默、log 照传，**没有任何构建期信号**。
# ⇒ 把"模板必须提供关键门控键"变成构建前置硬断言，缺一个就不产出固件。
# ---------------------------------------------------------------------
echo "== 配置模板守卫 =="
"${PYTHON:-python}" "$ROOT/tools/check_config_templates.py" || {
    echo "ASSERT_FAILED: 配置模板缺少关键门控键 ⇒ 拒绝产出'看着成功其实会静默'的固件"
    exit 1
}
echo

echo "== 构建 =="
"$PIO" run -e "$ENV_NAME" 2>&1 | tail -12
RC=${PIPESTATUS[0]}
if [ "$RC" != "0" ]; then
    echo "BUILD_FAILED rc=$RC"
    exit 1
fi

if [ ! -f "$BIN" ]; then
    echo "MISSING_ARTIFACT $BIN"
    exit 1
fi

# ---- 核心断言：必须含遥测通道字面量 ----
HITS=$(grep -a -c "climate.temperature" "$BIN" 2>/dev/null || echo 0)
WHITS=$(grep -a -c "weight.value" "$BIN" 2>/dev/null || echo 0)
if [ "$HITS" = "0" ]; then
    echo "ASSERT_FAILED: 产物不含 climate.temperature ⇒ 遥测实现未被编入"
    exit 1
fi

# ---- [2026-10-30 集成轮] 米家诊断键 / 动作 ID 必须真的进包 ----
# 为什么必须断言：这些字符串分别来自 system_state.cpp 的键表和
# xiaomi_lan.cpp 的动作描述符。只要有一处没编进去，UI 就会显示
# "State not found" / "未知动作"，而构建却"成功"——正是历史上
# "已实现 ≠ 已部署"的翻车点。缺任何一个都判失败。
ASSERT_LITS="xiaomi_feeder_last_err xiaomi_plug_last_err xiaomi_feeder_last_ok_ts xiaomi_plug_last_ok_ts xiaomi_lan_recv xiaomi_lan_badsum xiaomi_lan_timeout xiaomi_plug_autooff_sec xiaomi_feeder_configured xiaomi_plug_configured XIAOMI_PLUG_ON_60S XIAOMI_FEEDER_STATUS XIAOMI_PLUG_STATUS"
MISSING=""
for LIT in $ASSERT_LITS; do
    N=$(grep -a -c "$LIT" "$BIN" 2>/dev/null || echo 0)
    if [ "$N" = "0" ]; then MISSING="$MISSING $LIT"; fi
done
if [ -n "$MISSING" ]; then
    echo "ASSERT_FAILED: 产物缺少字面量 ⇒$MISSING"
    exit 1
fi
echo "assert-new: 11/11 诊断键与动作 ID 均在产物中"

SIZE=$(stat -c%s "$BIN")
SHA=$(sha256sum "$BIN" | cut -d' ' -f1)
MT=$(stat -c%y "$BIN" | cut -c1-19)

echo
echo "== 产物 =="
echo "path      : $BIN"
echo "size      : $SIZE B"
echo "sha256    : $SHA"
echo "mtime     : $MT"
echo "assert    : climate.temperature=$HITS, weight.value=$WHITS  (>=1 视为通过)"
echo "RESULT    : OK"
