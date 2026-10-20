#!/usr/bin/env bash
# G4 · C-2 部署前基线（只读）。用法：bash c2_pre.sh
set +e
EV=/d/Guo_Feeder_Project/PIO_Assets/Guo_Feeder_Project/docs/evidence/G4-C2-Deploy-2026-10-21
API=/d/Guo_Feeder_Project/Cloudflare_Assets/guo-feeder-api
WR="node ../node_modules/wrangler/bin/wrangler.js"
unset https_proxy http_proxy HTTPS_PROXY HTTP_PROXY
mkdir -p "$EV"
cd "$API" || exit 1

Q () { $WR d1 execute guofeeder --remote --json --command "$1" 2>&1; }

echo "# 部署前基线采集: $(date '+%F %T %z')"

echo
echo "=== S-1 源码制品（commit + 关键文件 sha256）==="
echo "HEAD=$(git rev-parse HEAD)"
echo "origin/main=$(git rev-parse origin/main)"
echo "worktree_dirty=$(git status --porcelain | wc -l)"
sha256sum src/index.js src/mqtt-dedup.js src/device-registry.js src/history-ingest.js

echo
echo "=== S-2 mqtt_messages 总览 ==="
Q "SELECT COUNT(*) AS n, MIN(id) AS mn, MAX(id) AS mx, SUM(CASE WHEN dedup_key IS NOT NULL THEN 1 ELSE 0 END) AS keyed FROM mqtt_messages;"

echo
echo "=== S-3 方向分布 ==="
Q "SELECT COALESCE(direction,'-') AS dir, COUNT(*) AS n FROM mqtt_messages GROUP BY dir ORDER BY n DESC;"

echo
echo "=== S-4 \$tenants 行归属（部署前应全为 http_api）==="
Q "SELECT device_id, COUNT(*) AS n, MAX(created_at) AS last_ms FROM mqtt_messages WHERE substr(topic,1,9)='\$tenants/' GROUP BY device_id ORDER BY n DESC LIMIT 10;"

echo
echo "=== S-5 plain 行归属（含预期会变成 unresolved 的 clientid 回落）==="
Q "SELECT device_id, COUNT(*) AS n FROM mqtt_messages WHERE substr(topic,1,9)<>'\$tenants/' GROUP BY device_id ORDER BY n DESC LIMIT 15;"

echo
echo "=== S-6 device 表 ==="
Q "SELECT COUNT(*) AS n_devices, MAX(last_seen) AS max_last_seen FROM device;"

echo
echo "=== S-7 log_history 近 30 min 行数（失败判据 4 的基线）==="
Q "SELECT COUNT(*) AS n_30min FROM log_history WHERE created_at >= (unixepoch() - 1800);"

echo
echo "=== S-8 最新 5 行（时间参考）==="
Q "SELECT id, device_id, COALESCE(direction,'-') AS dir, created_at, COALESCE(msg_id,'') AS msg_id FROM mqtt_messages ORDER BY id DESC LIMIT 5;"

echo
echo "=== S-9 采集结束: $(date '+%F %T %z') ==="
