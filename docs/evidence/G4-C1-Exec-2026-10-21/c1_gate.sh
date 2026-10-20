#!/usr/bin/env bash
# G4 · C-1 执行前硬门禁（只读）。用法：bash c1_gate.sh
# 全部命令只读：whoami / d1 list / versions view / sha256sum / migrations list / SELECT
set +e
CF=/d/Guo_Feeder_Project/Cloudflare_Assets
API=$CF/guo-feeder-api
WR="node ../node_modules/wrangler/bin/wrangler.js"
unset https_proxy http_proxy HTTPS_PROXY HTTP_PROXY

cd "$API" || exit 1
echo "# 门禁采集时间: $(date '+%F %T %z')"
echo "# wrangler: $($WR --version 2>/dev/null | tail -1)"
echo
echo "=== G-0 云端仓状态（期望 HEAD == origin/main，工作区干净）==="
git rev-parse HEAD
git rev-parse origin/main
git status --porcelain
echo
echo "=== G-1a 账号身份（whoami）==="
$WR whoami 2>&1 | grep -vE "Token|token"
echo
echo "=== G-1b d1 list（同名多库判定）==="
$WR d1 list --json 2>&1 | grep -E '"uuid"|"name"'
echo
echo "=== G-1c 线上 version 元数据里的实际绑定 ==="
$WR deployments status 2>&1 | grep -E "Created|Version|version" | head -6
$WR versions view 826332ed-3a73-42c4-a090-a5bf737dc210 2>&1 | grep -iE "Version ID|guofeeder_DB|LOG_BOOT_WATERMARK"
echo
echo "=== G-2 制品三方哈希（期望 sha256=90dc0ec0… / blob=eb300070… / HEAD 同）==="
sha256sum migrations/0007_mqtt_messages_dedup.sql
echo "worktree_blob=$(git hash-object migrations/0007_mqtt_messages_dedup.sql)"
echo "HEAD_blob=$(git rev-parse HEAD:./migrations/0007_mqtt_messages_dedup.sql)"
echo
echo "=== G-3 migration 登记（期望只列 0007）==="
$WR d1 migrations list guofeeder --remote 2>&1 | tail -12
echo
echo "=== G-4 半成品状态 S-PROBE（期望 col/idx/partial_idx/col_ok/reg7 全 0 ⇒ S0）==="
$WR d1 execute guofeeder --remote --json --command "SELECT (SELECT COUNT(*) FROM pragma_table_info('mqtt_messages') WHERE name='dedup_key') AS col, (SELECT COUNT(*) FROM sqlite_master WHERE type='index' AND name='ux_mm_dedup_key') AS idx, (SELECT COUNT(*) FROM sqlite_master WHERE name='ux_mm_dedup_key' AND sql LIKE '%WHERE dedup_key IS NOT NULL%') AS partial_idx, (SELECT COUNT(*) FROM sqlite_master WHERE name='ux_mm_dedup_key' AND sql LIKE '%ON mqtt_messages(dedup_key)%') AS col_ok, (SELECT COUNT(*) FROM d1_migrations WHERE name='0007_mqtt_messages_dedup.sql') AS reg7, (SELECT \"notnull\" FROM pragma_table_info('mqtt_messages') WHERE name='dedup_key') AS nn, (SELECT COALESCE(dflt_value,'<none>') FROM pragma_table_info('mqtt_messages') WHERE name='dedup_key') AS dflt, (SELECT type FROM pragma_table_info('mqtt_messages') WHERE name='dedup_key') AS ctype;" 2>&1 | grep -E '"col"|"idx"|"partial_idx"|"col_ok"|"reg7"|"nn"|"dflt"|"ctype"|changed_db|rows_written'
echo
echo "=== 门禁结束: $(date '+%F %T %z') ==="
