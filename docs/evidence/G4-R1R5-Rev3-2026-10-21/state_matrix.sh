#!/usr/bin/env bash
# G4 · R-5 状态矩阵实测（**完全本地**：隔离 persist 目录 + 假 database_id，绝不接触生产 D1）
# 目的：验证「正式 migration 流程」在 6 种半成品状态下的真实行为，
#       特别是「0008 只建索引」能否救 state 3/4。
# 用法：bash state_matrix.sh   （在 Cloudflare_Assets 目录下）
set -u
BASE=".wrangler/g4r5_matrix_$(date +%Y%m%d%H%M%S)"
WR="node node_modules/wrangler/bin/wrangler.js"
FIX="guo-feeder-api/test/real_d1/fixtures/mqtt_messages.sql"
unset http_proxy https_proxy HTTP_PROXY HTTPS_PROXY ALL_PROXY all_proxy
export WRANGLER_SEND_METRICS=false CI=1

mkcfg () { # $1=dir  $2..=要放入的 migration 文件
  local d="$1"; shift
  mkdir -p "$d/migrations"
  cat > "$d/wrangler.toml" <<'EOF'
name = "stateprobe"
main = "src.js"
compatibility_date = "2026-08-21"
[[d1_databases]]
binding = "DB"
database_name = "stateprobe"
database_id = "00000000-0000-0000-0000-000000000000"
EOF
  echo 'export default { fetch() { return new Response("ok"); } };' > "$d/src.js"
  for f in "$@"; do cp "$f" "$d/migrations/"; done
}

APPLY () { $WR d1 migrations apply stateprobe --local --persist-to "$1" -c "$2/wrangler.toml" 2>&1 \
            | sed 's/\x1b\[[0-9;]*m//g' | grep -E "0007|0008|0009|ERROR|commands executed|No migrations|✅|X " ; }
Q ()     { $WR d1 execute stateprobe --local --persist-to "$1" -c "$2/wrangler.toml" --json --command "$3" 2>&1 \
            | sed -n '/"results"/,/]/p' | tr -d ' \n'; echo; }
PREP ()  { mkdir -p "$1"; $WR d1 execute stateprobe --local --persist-to "$1" -c "$2/wrangler.toml" --file "$FIX" >/dev/null 2>&1; }
DDL ()   { $WR d1 execute stateprobe --local --persist-to "$1" -c "$2/wrangler.toml" --command "$3" 2>&1 \
            | sed 's/\x1b\[[0-9;]*m//g' | grep -E "ERROR|executed" | head -2; }

PROBE='SELECT (SELECT COUNT(*) FROM pragma_table_info('"'"'mqtt_messages'"'"') WHERE name='"'"'dedup_key'"'"') AS col,
 (SELECT COUNT(*) FROM sqlite_master WHERE name='"'"'ux_mm_dedup_key'"'"') AS idx,
 (SELECT COUNT(*) FROM sqlite_master WHERE name='"'"'ux_mm_dedup_key'"'"' AND sql LIKE '"'"'%WHERE dedup_key IS NOT NULL%'"'"') AS partial_idx,
 (SELECT COUNT(*) FROM d1_migrations WHERE name LIKE '"'"'%0007%'"'"') AS reg7,
 (SELECT COUNT(*) FROM d1_migrations WHERE name LIKE '"'"'%0008%'"'"') AS reg8,
 (SELECT COUNT(*) FROM d1_migrations WHERE name LIKE '"'"'%0009%'"'"') AS reg9'

M7="guo-feeder-api/migrations/0007_mqtt_messages_dedup.sql"
mkcfg "$BASE/cfgA" "$M7"
printf 'CREATE UNIQUE INDEX IF NOT EXISTS ux_mm_dedup_key\n    ON mqtt_messages(dedup_key)\n    WHERE dedup_key IS NOT NULL;\n' > "$BASE/0008_only_index.sql"
printf 'ALTER TABLE mqtt_messages ADD COLUMN dedup_key TEXT;\nCREATE UNIQUE INDEX IF NOT EXISTS ux_mm_dedup_key\n    ON mqtt_messages(dedup_key)\n    WHERE dedup_key IS NOT NULL;\n' > "$BASE/0009_add_col_and_index.sql"
mkcfg "$BASE/cfgB" "$M7" "$BASE/0008_only_index.sql"
mkcfg "$BASE/cfgC" "$M7" "$BASE/0009_add_col_and_index.sql"

echo "############ S0 对照：干净表 ############"
PREP "$BASE/s0" "$BASE/cfgA"; APPLY "$BASE/s0" "$BASE/cfgA"; Q "$BASE/s0" "$BASE/cfgA" "$PROBE"

echo; echo "############ S3 列在 / 索引缺 / 0007 未登记 ############"
PREP "$BASE/s3" "$BASE/cfgA"; DDL "$BASE/s3" "$BASE/cfgA" "ALTER TABLE mqtt_messages ADD COLUMN dedup_key TEXT"
APPLY "$BASE/s3" "$BASE/cfgA"; Q "$BASE/s3" "$BASE/cfgA" "$PROBE"

echo; echo "############ S3+0008 同状态但已放入 0008（关键：0008 会被执行吗？）############"
PREP "$BASE/s3b" "$BASE/cfgB"; DDL "$BASE/s3b" "$BASE/cfgB" "ALTER TABLE mqtt_messages ADD COLUMN dedup_key TEXT"
APPLY "$BASE/s3b" "$BASE/cfgB"; Q "$BASE/s3b" "$BASE/cfgB" "$PROBE"

echo; echo "############ S4 列+索引都在 / 0007 未登记 ############"
PREP "$BASE/s4" "$BASE/cfgA"; DDL "$BASE/s4" "$BASE/cfgA" "ALTER TABLE mqtt_messages ADD COLUMN dedup_key TEXT"
DDL "$BASE/s4" "$BASE/cfgA" "CREATE UNIQUE INDEX ux_mm_dedup_key ON mqtt_messages(dedup_key) WHERE dedup_key IS NOT NULL"
APPLY "$BASE/s4" "$BASE/cfgA"; Q "$BASE/s4" "$BASE/cfgA" "$PROBE"

echo; echo "############ S5a 0007 已登记 / 索引被删 / 放入 0008（索引型）############"
PREP "$BASE/s5a" "$BASE/cfgA"; APPLY "$BASE/s5a" "$BASE/cfgA" >/dev/null
DDL "$BASE/s5a" "$BASE/cfgA" "DROP INDEX ux_mm_dedup_key"
echo "-- drop 后 --"; Q "$BASE/s5a" "$BASE/cfgA" "$PROBE"
echo "-- 换用含 0008 的配置 apply --"; APPLY "$BASE/s5a" "$BASE/cfgB"; Q "$BASE/s5a" "$BASE/cfgB" "$PROBE"

echo; echo "############ S5 0007 已登记 / 列与索引都缺 —— 伪成功陷阱 ############"
PREP "$BASE/s5" "$BASE/cfgA"; APPLY "$BASE/s5" "$BASE/cfgA" >/dev/null
echo "-- 先 DROP COLUMN（索引还在，预期被拒）--"
DDL "$BASE/s5" "$BASE/cfgA" "ALTER TABLE mqtt_messages DROP COLUMN dedup_key"
DDL "$BASE/s5" "$BASE/cfgA" "DROP INDEX ux_mm_dedup_key"
echo "-- 再 DROP COLUMN --"
DDL "$BASE/s5" "$BASE/cfgA" "ALTER TABLE mqtt_messages DROP COLUMN dedup_key"
echo "-- 结构：col=0 idx=0 reg7=1 --"; Q "$BASE/s5" "$BASE/cfgA" "$PROBE"
echo "-- 此时 apply（关键：是否报 No migrations to apply 且 exit=0）--"
APPLY "$BASE/s5" "$BASE/cfgA"; $WR d1 migrations apply stateprobe --local --persist-to "$BASE/s5" -c "$BASE/cfgA/wrangler.toml" >/dev/null 2>&1; echo "apply_exit=$?"
echo "-- 结构仍坏 --"; Q "$BASE/s5" "$BASE/cfgA" "$PROBE"
echo "-- 换用含 0009（加列+建索引）的配置 --"; APPLY "$BASE/s5" "$BASE/cfgC"; Q "$BASE/s5" "$BASE/cfgC" "$PROBE"

echo; echo "############ S6 结构在但索引定义错（非部分索引）############"
PREP "$BASE/s6" "$BASE/cfgA"; DDL "$BASE/s6" "$BASE/cfgA" "ALTER TABLE mqtt_messages ADD COLUMN dedup_key TEXT"
DDL "$BASE/s6" "$BASE/cfgA" "CREATE UNIQUE INDEX ux_mm_dedup_key ON mqtt_messages(dedup_key)"
echo "-- 错误索引 sql --"; Q "$BASE/s6" "$BASE/cfgA" "SELECT sql FROM sqlite_master WHERE name='ux_mm_dedup_key'"
APPLY "$BASE/s6" "$BASE/cfgA"; Q "$BASE/s6" "$BASE/cfgA" "$PROBE"
echo "############ 结束（BASE=$BASE）############"
