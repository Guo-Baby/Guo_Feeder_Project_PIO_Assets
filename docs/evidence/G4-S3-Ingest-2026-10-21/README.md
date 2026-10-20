# S-3 入站 S2 校验 + `observe` 部署 · 证据索引（`2026-10-21 03:00–03:20`）

**结论速览**

| 项 | 结果 |
|---|---|
| 本地套件 | **25 套全部通过**（24 既有 + 1 新增），`run_all.mjs` `exit=0` |
| 新增 S-3 套件 | **71 / 71 PASS** |
| 本地 `wrangler dev --local` 端到端 | **15 / 15 用例无失配**（含 40 条洪泛） |
| 生产部署 | `observe` 模式已上线：version **`1fb56499-49fb-4e07-b2c6-6fd35cf60329`**（100%） |
| `enforce` | ❌ **未启用**（版本 env 实证 `INGEST_VALIDATION ("observe")`） |
| 生产 D1 写入 | 本次部署**零写入**（`changed_db=false` / `rows_written=0`） |

★ **本目录所有"落库"证据都来自本地 miniflare D1，不是生产 D1。** 生产只做了只读查询。

---

## 文件索引

| 文件 | 内容 | 关键结果 |
|---|---|---|
| `tests-all-suites.txt` | `node test/run_all.mjs` 全量输出（25 套） | `exit=0`，末行「所有套件通过」 |
| `deploy-pre.txt` | 部署前源码标识 + 制品 sha256 + 当前生产版本 | `HEAD=e1e8b79…` · `index.js=cedbe9ad…` · 现网 `d7fcd4f3-…` |
| `deploy-log.txt` | `wrangler deploy` 完整输出与时间 | `03:15:54`→`03:16:08` · `deploy_exit=0` · 新 version `1fb56499-…` |
| `deploy-post-verify.txt` | 生效判定：100% + env 绑定 | `1fb56499-…` **100%** · `INGEST_VALIDATION ("observe")` · D1 `0d8385ba-…` 未变 |
| `s3_local_e2e.py` | 本地端到端脚本（可复现） | — |
| `local-e2e-run.txt` | 15 个用例的 HTTP 结果 | `cases=15 mismatched=0` |
| `local-d1-rows.txt` | 本地 D1 逐行落库（原始 JSON） | 11 行（含 1 行去重命中） |
| `dev-console.log` | `wrangler dev` 控制台全量日志 | 54 请求 → 仅 3 条 `[validate]` + 3 条 `[unresolved]` warn |
| `prod-d1-snapshot.txt` | 部署后生产 D1 只读快照 | `n=5756` · `id_gap=22` · `keyed=9` · `changed_db=false` |
| `prod-real-traffic.txt` | 生产真实流量只读取样（**非本任务交付项**，见 §3） | 见 §3 |

---

## 1. 本地端到端结果（`local-e2e-run.txt` + `local-d1-rows.txt`）

设备段用 **合法 12-hex** 合成 ID `0b0053a00e2e`（本地专用，生产不存在）。

| 用例 | 输入 | HTTP | 落库结果（本地 D1） |
|---|---|---|---|
| L-1 | `guo_feeder/<dev>/up` | 200 | `device_id=0b0053a00e2e` · `dir=up` · `dedup_key=KEY` |
| L-2 | `$tenants/<t>/guo_feeder/<dev>/down` | 200 | `device_id=0b0053a00e2e` · `dir=down` ★ 归属正确 |
| L-3 | `$tenants/<t>/guo_feeder/<dev>/log` | 200 | `dir=log` ★ `B-2c` 生效 |
| O-1 | **空 topic** | 200 | **已写入** · `unresolved` / `unknown` ★ observe 未改变行为 |
| O-2 | **空 payload** | 200 | **已写入** · `dedup_key=NULL` |
| O-3 | `guo_feeder/<dev>/upx`（非法方向） | 200 | **已写入** · `unknown` |
| O-4 | `$SYS/broker/uptime`（族名不符） | 200 | **已写入** · `unknown` |
| D-1..3 | 同 `msg_id` 同 payload **×3** | 200 | **只 1 行**（`inserted`→`duplicate`→`duplicate`） |
| N-1..2 | 缺 `msg_id`（`i` 为空）**×2** | 200 | **2 行**，`dedup_key` 均 `NULL`（未误合并） |
| C-1 | `cloud_api` + `device_id=BADID` | **400** | `invalid_device_id`（原行为不变） |
| C-2 | `/api/ingest` 无 key | **401** | `unauthorized`（鉴权未改动） |
| F-1 | 洪泛 40 条空 topic | 200×40 | 全 200；日志仅 1 条 `topic_missing`（防洪生效） |

## 2. 日志防洪与"无泄露"实测

54 次请求（含 40 条恶意空 topic）的实际日志量：

- `[ingest][validate]` = **3 行**（3 种不同 reason 各 1 行）
- `[ingest][unresolved]` = **3 行**
- 若"每条一打"，本应是 **≈44 + 44 = 88 行** ⇒ 实际压缩到 **6 行**

**密钥泄露检查**：`grep -ciE "test-key|WEBHOOK_SECRET|ADMIN_API_TOKEN|EMQX_APP_SECRET|password"` 命中 **3 行**，
逐行核实全部是 wrangler 启动横幅里的**变量名 + `("(hidden)")`**（`dev-console.log:10,11,13`），
**没有任何密钥值**。日志正文只含 reason / 计数 / 主题长度，**不含 payload、不含请求头**。

## 3. ★ 生产真实流量（`prod-real-traffic.txt`）—— 顺带观察，**不是**本任务交付项

部署后只读查询发现**设备已恢复上行**（`2026-10-20 18:51–18:52 UTC`，约 `02:51 +0800`）：

| id | topic | device_id | dir | dedup_key |
|---|---|---|---|---|
| 5774–5778 | `$tenants/n302933b/guo_feeder/288485896ce4/down` | **`288485896ce4`** | `down` | `KEY` |

`$tenants` 归属统计：`288485896ce4` **5 行**（`mx=5778`）· `http_api` 1539 行（**全部 ≤ id 5763**，即 C-2 之前）。

> ★ 这是**第一次**出现 `$tenants/*/down` 的 `device_id` 为合法 12-hex 的**真实**行
> ⇒ 为 C-2 的 `P-1'` **真实流量版本**提供了直接证据。
> ⚠️ **本报告不据此宣告 C-2 通过** —— 是否关闭 `C2-P1P-REAL` 由人工认定（属 C-2 范围）。
> ⚠️ 这 5 行**写于 S-3 部署之前**（`03:16:08`），因此**不能**用来证明 S-3 的 observe 指标。

## 4. observe 指标可观测性（**如实声明**）

| 项 | 事实 |
|---|---|
| 计数器作用域 | `isolate`（module 作用域） |
| 持久化 | **无**（不落 D1 / KV / 表） |
| 重启后 | **归零**（隔离实例回收、重新部署、冷启动皆然） |
| 可观测出口 | 仅 Workers Logs（`console.warn`，`[observability.logs] persist=true`） |
| 生产是否已能读数 | ❌ **不能** —— S-3 部署后至今**无真实流量**（最近一行 `02:52`，部署 `03:16`），且 `tail` 可用性未验证（`C2-OBS`） |
| 结论 | 生产 `invalid_total` 目前 **`NOT MEASURABLE`**，**不是** 0 |

## 5. 与生产的差异声明

| 维度 | 本地验证 | 生产 |
|---|---|---|
| 运行时 | workerd 本地进程 | Cloudflare 边缘 |
| D1 | miniflare SQLite（`--local`） | 生产 D1（本轮**只读**） |
| 网络 | `127.0.0.1` | TLS + 边缘 WAF + `workers.dev` |
| 结论适用范围 | JS 行为 + 真实 SQL 语义 | 仅"版本与 env 绑定已生效" |

## 复现

```bash
# 1) 全量套件
cd D:/Guo_Feeder_Project/Cloudflare_Assets/guo-feeder-api && node test/run_all.mjs

# 2) 本地端到端（先起 dev）
node ../node_modules/wrangler/bin/wrangler.js d1 migrations apply guofeeder --local
node ../node_modules/wrangler/bin/wrangler.js dev --port 8792 --local
S3_BASE=http://127.0.0.1:8792 S3_BURST=40 python s3_local_e2e.py
```
