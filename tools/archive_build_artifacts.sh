#!/usr/bin/env bash
# =====================================================
# archive_build_artifacts.sh —— 把 .pio/build 下的固件产物**独立归档**
#
# 为什么必须这么做（2026-10-29 · WP-H 事故教训）
# ----------------------------------------------
# PlatformIO 的 `pio run` 在**每次启动**时都会调用 `clean_build_dir()`：
#
#   platformio/run/helpers.py:
#     checksum = compute_project_checksum(config)
#     if <已有 project.checksum> != checksum:
#         fs.rmtree(build_dir)        # ← 整个 .pio/build 被删！
#
# 而 `compute_project_checksum()`（platformio/project/helpers.py:93）=
#     SHA1( PIO 版本 + **整份 platformio.ini 的 JSON** + include/src/lib 下
#           **所有 .c/.cc/.cpp/.h/.hpp/.s/.S 的路径清单** )
#
# ⇒ 只要**改了 platformio.ini**（哪怕只是新增一个 [env:...] 段），
#   或**增删/改名任何源文件**，下一次 `pio run` 就会把 `.pio/build/`
#   **整个删掉重建**，历史 env 的 firmware.bin 一并消失。
#   （本轮实测：xf3 / xm1 / xp1 / xd2 / xlan / nt1 / esp32-s3-devkitc-1 全被清空，
#     而 `.pio/build_homie` 等**在 `.pio/build` 之外**的目录完好。）
#
# 结论：**产物必须在构建完成的当时归档**，不能指望它留在 .pio/build 里。
#
# 本脚本的行为
# ------------
#   · **只读 .pio/build，只写 docs/evidence/build-archive/**；
#   · 不删除、不移动、不清理任何东西；
#   · 按 SHA-256 **去重**（同一固件不重复存）；
#   · 每个产物记 manifest：env / 大小 / mtime / sha256 / git HEAD / dirty 数 / 编译宏。
#
# 用法
# ----
#     bash tools/archive_build_artifacts.sh            # 归档（幂等）
#     bash tools/archive_build_artifacts.sh --dry-run  # 只看会归档什么
# =====================================================
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT" || exit 1

ARCHIVE="$ROOT/docs/evidence/build-archive"
INDEX="$ARCHIVE/INDEX.tsv"
DRY=0
[ "${1:-}" = "--dry-run" ] && DRY=1

mkdir -p "$ARCHIVE"
if [ ! -f "$INDEX" ]; then
    printf 'archived_at\tenv\tsize\tsha256\trel_path\n' > "$INDEX"
fi

GIT_HEAD="$(git rev-parse --short HEAD 2>/dev/null || echo unknown)"
GIT_DIRTY="$(git status --porcelain 2>/dev/null | wc -l | tr -d ' ')"
NOW="$(date '+%Y%m%d-%H%M%S')"

# firmware.bin 有两种布局：
#   .pio/build/<env>/firmware.bin
#   .pio/build/<env>/<board>/firmware.bin
mapfile -t BINS < <(find .pio/build -name "firmware.bin" -type f 2>/dev/null | sort)

if [ "${#BINS[@]}" -eq 0 ]; then
    echo "== 没有找到 .pio/build 下的 firmware.bin =="
    exit 0
fi

NEW=0
SKIP=0
for BIN in "${BINS[@]}"; do
    SHA="$(sha256sum "$BIN" | cut -d' ' -f1)"
    SIZE="$(stat -c%s "$BIN")"
    MT="$(stat -c%y "$BIN" | cut -c1-19)"
    # env 名：.pio/build/<env>/...
    ENV_NAME="$(echo "$BIN" | sed -E 's#^\.pio/build/([^/]+)/.*#\1#')"

    if grep -q "	$SHA	" "$INDEX" 2>/dev/null; then
        SKIP=$((SKIP + 1))
        continue
    fi

    DEST_DIR="$ARCHIVE/$NOW-$ENV_NAME"
    DEST="$DEST_DIR/${SHA:0:12}-$ENV_NAME.bin"

    if [ "$DRY" = "1" ]; then
        echo "[dry-run] 将归档 $BIN -> $DEST ($SIZE B, $SHA)"
        NEW=$((NEW + 1))
        continue
    fi

    mkdir -p "$DEST_DIR"
    cp "$BIN" "$DEST"
    {
        echo "固件产物归档（archive_build_artifacts.sh）"
        echo "=========================================="
        echo "archived_at   = $(date '+%Y-%m-%d %H:%M:%S')"
        echo "env           = $ENV_NAME"
        echo "src_path      = $BIN"
        echo "size_bytes    = $SIZE"
        echo "mtime         = $MT"
        echo "sha256        = $SHA"
        echo "git_HEAD      = $GIT_HEAD"
        echo "git_dirty     = $GIT_DIRTY"
        echo "pio_version   = $(/d/platformIO/penv/Scripts/pio.exe --version 2>/dev/null | tail -1 || echo unknown)"
        echo "telemetry     = climate.temperature=$(grep -a -c 'climate.temperature' "$BIN" 2>/dev/null || echo 0)"
        echo
        echo "注：本文件为**归档副本**；.pio/build 可能被 pio run 的 auto-clean 清除，"
        echo "    以本归档为追溯依据。"
    } > "$DEST.manifest.txt"

    printf '%s\t%s\t%s\t%s\t%s\n' "$(date '+%Y-%m-%d %H:%M:%S')" "$ENV_NAME" "$SIZE" "$SHA" \
        "${DEST#$ROOT/}" >> "$INDEX"
    echo "归档 $ENV_NAME -> ${DEST#$ROOT/}  ($SIZE B, ${SHA:0:12}…)"
    NEW=$((NEW + 1))
done

echo
echo "== 小结：新增 $NEW，已存在跳过 $SKIP =="
echo "== 索引：${INDEX#$ROOT/} =="
