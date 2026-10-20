#!/usr/bin/env bash
# G4 · C-1 执行后六项验证（只读）。用法：bash c1_verify.sh
# 判据见 G4-Production-Change-Plan-Rev2-2026-10-20.md §3.3
set +e
EV=/d/Guo_Feeder_Project/PIO_Assets/Guo_Feeder_Project/docs/evidence/G4-C1-Exec-2026-10-21
API=/d/Guo_Feeder_Project/Cloudflare_Assets/guo-feeder-api
WR="node ../node_modules/wrangler/bin/wrangler.js"
unset https_proxy http_proxy HTTPS_PROXY HTTP_PROXY
cd "$API" || exit 1

echo "# 六项验证开始: $(date '+%F %T %z')"

# ---------- V-1 / V-1b / V-1c ----------
{ echo "# 采集时间: $(date '+%F %T %z')"
  echo "=== V-1 结构：col / idx / partial_idx / reg7（期望 1/1/1/1）==="
  $WR d1 execute guofeeder --remote --json --command "SELECT (SELECT COUNT(*) FROM pragma_table_info('mqtt_messages') WHERE name='dedup_key') AS col, (SELECT COUNT(*) FROM sqlite_master WHERE type='index' AND name='ux_mm_dedup_key') AS idx, (SELECT COUNT(*) FROM sqlite_master WHERE name='ux_mm_dedup_key' AND sql LIKE '%WHERE dedup_key IS NOT NULL%') AS partial_idx, (SELECT COUNT(*) FROM d1_migrations WHERE name='0007_mqtt_messages_dedup.sql') AS reg7;" 2>&1
  echo "=== V-1b 列属性（期望 type=TEXT / notnull=0 / dflt_value=NULL / pk=0）==="
  $WR d1 execute guofeeder --remote --json --command "SELECT name, type, \"notnull\" AS notnull, dflt_value, pk FROM pragma_table_info('mqtt_messages') WHERE name = 'dedup_key';" 2>&1
  echo "=== V-1c 索引定义全文（期望 UNIQUE + 单列 dedup_key + WHERE dedup_key IS NOT NULL）==="
  $WR d1 execute guofeeder --remote --json --command "SELECT name, sql FROM sqlite_master WHERE name = 'ux_mm_dedup_key';" 2>&1
} > "$EV/v1-structure.txt" 2>&1

# ---------- V-2 历史行指纹（固定 id 上界 5763）----------
{ echo "# 采集时间: $(date '+%F %T %z')"
  echo "# MAX_ID_BEFORE = 5763（执行前基线）"
  echo "# FINGERPRINT_BEFORE = n_old=5745 min_id=1 max_id=5763 pl_len=817694 id_len=36151 tp_len=154882"
  echo "=== V-2 重放（期望四项与基线逐项相等；keyed 必为 0）==="
  $WR d1 execute guofeeder --remote --json --command "SELECT COUNT(*) AS n_old, MIN(id) AS min_id_old, MAX(id) AS max_id_old, SUM(LENGTH(COALESCE(raw_payload,''))) AS pl_len, SUM(LENGTH(COALESCE(msg_id,''))) AS id_len, SUM(LENGTH(COALESCE(topic,''))) AS tp_len, SUM(CASE WHEN dedup_key IS NOT NULL THEN 1 ELSE 0 END) AS keyed FROM mqtt_messages WHERE id <= 5763;" 2>&1
} > "$EV/v2-fingerprint-after.txt" 2>&1

# ---------- V-2b 固定 25 行逐行复核 ----------
IDS=$(grep -E '^[0-9]' "$EV/SAMPLE_IDS.txt" | tail -1)
{ echo "# 采集时间: $(date '+%F %T %z')"
  echo "# SAMPLE_IDS = $IDS"
  echo "# 期望：返回 25 行且逐项与 v2b 执行前一致；<25 行 ⇒ 有删除 ⇒ P0"
  echo "=== V-2b 重放（按固定 id 列表，不重跑 OFFSET）==="
  $WR d1 execute guofeeder --remote --json --command "SELECT id, device_id, COALESCE(direction,'-') AS dir, COALESCE(cmd,'') AS cmd, COALESCE(msg_id,'') AS msg_id, LENGTH(COALESCE(topic,'')) AS tp_len, LENGTH(COALESCE(raw_payload,'')) AS pl_len, substr(COALESCE(raw_payload,''),1,32) AS pl_head FROM mqtt_messages WHERE id IN ($IDS) ORDER BY id;" 2>&1
} > "$EV/v2b-sample-post.txt" 2>&1

# ---------- V-3 索引可用性 ----------
{ echo "# 采集时间: $(date '+%F %T %z')"
  echo "=== V-3 keyed_total（期望 0：不回填）==="
  $WR d1 execute guofeeder --remote --json --command "SELECT COUNT(*) AS keyed_total FROM mqtt_messages WHERE dedup_key IS NOT NULL;" 2>&1
  echo "=== V-3b 索引是否可被引用（EXPLAIN QUERY PLAN，只读）==="
  $WR d1 execute guofeeder --remote --json --command "EXPLAIN QUERY PLAN SELECT id FROM mqtt_messages WHERE dedup_key = 'x';" 2>&1
} > "$EV/v3-keyed.txt" 2>&1

echo "# 六项验证结束: $(date '+%F %T %z')"
