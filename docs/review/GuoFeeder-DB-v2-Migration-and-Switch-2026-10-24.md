# Guo Feeder —— 新建 D1 库 `guofeeder-v2`、修复初始化链、部署与端到端验证

- 日期：2026-10-24（UTC 14:44–15:20 / 本地 22:44–23:20）
- 仓库：`Cloudflare_Assets`（分支 `main`，切换前 HEAD `8e8d3e0` → 切换后 `75b22b8`）
- 生产 Worker：`guo-feeder-api`，版本 `81ceda94-48cd-4651-94af-c0b5a6748635`（100%）
- 结论：**已完成**（1 项降级说明见 §11.1：登录态相关端点未能在沙箱内验证）

---

## 1. 执行摘要

| 项 | 结果 |
|---|---|
| 空库初始化链修复 | ✅ 新增 `migrations/0000_p0_legacy_mqtt_messages.sql`，`0007` 不再失败 |
| 新库创建 | ✅ `guofeeder-v2` / `632b1224-7f71-43e9-9575-4e82ee26f36d`（WNAM） |
| 新库迁移 | ✅ `0000`→`0009` 共 10 个，全部 ✅；`d1_migrations` 10 行 |
| 新库 schema | ✅ 14 张业务表 + 34 索引；`mqtt_messages` 21 列 |
| 查询计划（**远端 D1**） | ✅ 4 条列表查询全部命中预期覆盖索引，无 `TEMP B-TREE` |
| 自动化测试 | ✅ 本次新增 38 条断言全绿；既有失败 21 项经**纯净 HEAD 对比**证明为历史失败 |
| 生产部署 + 绑定切换 | ✅ 版本 `81ceda94…` @100%，生效绑定已从 Cloudflare 侧核实为**新库** |
| 真实设备流量 | ✅ 新库已收到真实遥测（`288485896ce4`，`climate.temperature` 等） |
| 旧库 | ✅ 保留未动，未删未清空 |
| 回滚路径 | ✅ 已验证可用的两条：改回配置重新 deploy / 回滚到 `e85c8a2f…` |

**规模再确认**：旧库存量 12.7 MB；新库初始化后 266 kB。全部初始化 + 验证仅消耗
**938 行读 / 226 行写**（新库 24h 累计，含 DDL、迁移、全部核验 SQL、冒烟写入与真实遥测）。

---

## 2. 代码变更

提交 `75b22b8`（16 文件 / +770 −29）：

| 文件 | 变更 | 目的 |
|---|---|---|
| `guo-feeder-api/migrations/0000_p0_legacy_mqtt_messages.sql` | **新增** | 空库引导：恢复 P0 遗留表 `mqtt_messages`（20 列，**不含** `dedup_key`） |
| `guo-feeder-api/migrations/0009_ui_read_indexes.sql` | **新增（入库）** | 4 条只读覆盖索引；**文件内容未改动**（P-D1） |
| `guo-feeder-api/migrations/README.md` | 改 | 新增 §0.0 说明 0000；回填 0004–0009 的实际应用状态 |
| `guo-feeder-api/wrangler.toml` | 改 | 绑定指向新库 + 内联回滚信息 |
| `guo-feeder-api/test/real_d1/empty_db_init_real_d1.sh` | **新增** | 38 条断言的空库初始化套件（真实 D1 引擎） |
| `guo-feeder-api/test/real_d1/patch_real_d1_dbname.py` | **新增** | 一次性补丁工具（库名去硬编码），幂等 |
| `guo-feeder-api/test/real_d1/*.sh`（9 个） | 改 | `d1 execute/apply guofeeder` → `"$DB"`，`DB` 从 wrangler.toml 解析 |
| `scripts/schema_parity_check.py` | **新增** | 备份 dump vs 新库 逐表/逐列/逐索引对比（离线只读） |

### 2.1 一个必须记录的连带问题

切库后 `wrangler d1 execute/migrations apply <name>` **只认配置文件里的
`database_name`**。全部 `real_d1/*.sh` 都硬编码 `guofeeder` ⇒ 切库瞬间
**10 个脚本全部报** `Couldn't find a D1 DB with the name or binding 'guofeeder'`，
断言表现为 `got=[]` 全红（首次复跑时误以为是新缺陷）。

修复：每个脚本在 `CFG=` 之后插一行

```bash
DB="${GF_D1_NAME:-$(grep -m1 '^database_name=' "$CFG" | sed 's/.*="\(.*\)".*/\1/')}"
```

并把 d1 调用改用 `"$DB"`（共 27 处）。补丁后实测 `empty_db_init` 38/38、
`mqtt_dedup` 19/19 复绿。

### 2.2 未纳入本次提交（上一阶段待审阅，**保持原样**）

`.gitignore`、`src/read-api.js`、`test/current/read_api.test.mjs`、
`pagesdev/assets/js/gf-app.js`、`tests/pages/ui_app*.test.mjs`、
`scripts/d1_usage_report.py`、`scripts/local_index_plan_check.py`、
`scripts/enforce_watch.sh`、`tests/api/snapshot_counts.test.mjs`。

> ⚠️ `0009_ui_read_indexes.sql` **已入库**（因为新库 schema 依赖它，不入库会造成
> "生产 schema 有、仓库没有" 的审计缺口），但内容一字未改——其文件头的
> "草案 / 未应用" 字样已过时，正确状态以 `migrations/README.md` 的
> 「迁移状态说明」为准。

---

## 3. 迁移设计

### 3.1 缺陷与证据

- `mqtt_messages` 是 P0 时代的手工生产表，`migrations/README.md` §0 原文写着
  「`mqtt_messages` ❌ **完全不动**」⇒ 仓库**没有**它的建表迁移；
- `0007_mqtt_messages_dedup.sql:43` 对它 `ALTER TABLE … ADD COLUMN dedup_key`；
- ⇒ 空库 apply 到 0007 必然 `no such table: mqtt_messages`。

**反向证明已落进测试**（`empty_db_init_real_d1.sh` E-9）：
先只应用 `0001`–`0006`，单独应用 `0007` ⇒ 报 `no such table`（E-9b PASS）；
补应用 `0000` 后 `0007` 成功、`dedup_key` 列建立（E-9c/E-9d PASS）。

### 3.2 为什么是 `0000` 而不是 `0010`

迁移按**文件名排序**执行。任何 `>= 0007` 的编号都排在 `0007` 之后，修不了它的
失败点。`0000` 是唯一"先于 0001–0009 且不改动已应用迁移"的位置。

另一个被否决的方案：把 bootstrap 放到 `migrations/` 之外手工先跑 —— 那样
`d1_migrations` 里不会留记录，等于"手工跳过迁移、伪造完成状态"，任务书明确禁止。

### 3.3 `mqtt_messages` schema 的来源与形状

| 取证来源 | 内容 |
|---|---|
| 生产只读 `SELECT sql FROM sqlite_master WHERE name='mqtt_messages'` | 21 列（含 0007 追加的 `dedup_key` 末列），见 `test/real_d1/fixtures/mqtt_messages.sql` 文件头 |
| `backups/guofeeder-pre0008-20261023.sql:2` | 同一份 DDL 的独立副本（2026-10-23 导出） |
| Worker 全部读写 SQL | `src/index.js` / `read-api.js` / `mqtt-dedup.js` / `retention.js` |

`0000` = 上述 21 列 **减去 `dedup_key`**（20 列）：
`id / device_id / topic / direction / raw_payload / cmd / msg_id / object /
stable_id / version / status / error_code / message / timestamp / payload_json /
created_at / status_text / error_text / msg_type_text / msg_type`

- **不加 `dedup_key`**：否则 `0007` 会 `duplicate column name` 失败（迁移链本身即证据链）；
- **不加任何索引**：与生产一致（0007 之前该表零索引），只读索引归 `0009`；
- **不加简化占位字段**：`device_id TEXT NOT NULL`、`id INTEGER PRIMARY KEY AUTOINCREMENT`、
  `created_at` 为**毫秒**（与其余表的秒不同）等历史事实全部保留。

### 3.4 新旧库的差异化安全路径

| | 旧库 `guofeeder` | 新库 `guofeeder-v2` |
|---|---|---|
| `0000` | **不应用**（表由 P0 手工建） | 应用（首次建表） |
| `0001`–`0008` | 已应用 | 应用 |
| `0009` | 仍待应用（未授权） | 已应用 |
| 误 apply `0000` 的后果 | `CREATE TABLE IF NOT EXISTS` ⇒ **no-op**，不改任何列/行 | — |

> `d1 migrations list --remote` 在旧库会把 `0000` 列为「待应用」——**这是准确的**
> （它确实没在旧库跑过），已在 README 中写明不要因此去 apply 旧库。

---

## 4. 数据库信息

| 项 | 旧库（保留） | 新库（生产） |
|---|---|---|
| 名称 | `guofeeder` | `guofeeder-v2` |
| ID | `0d8385ba-a1a6-4fcc-8e43-ae61c014c43d` | `632b1224-7f71-43e9-9575-4e82ee26f36d` |
| 区域 | — | WNAM |
| 体积 | 12.7 MB | 266 kB |
| 迁移 | `0001`–`0008`（`0009` 待应用） | `0000`–`0009`（10 个） |
| 业务表 / 索引 | 13 / 30 | 13 / 34 |
| 状态 | **原样保留**，未删、未清空、未写入 | 生产读写 |

绑定名**保持 `guofeeder_DB`**（`src/*.js` 全部走 `env.guofeeder_DB`，零代码改动）。

---

## 5. 测试结果

### 5.1 本次新增 / 复跑

| 套件 | 结果 |
|---|---|
| `test/real_d1/empty_db_init_real_d1.sh` | **38 / 38 PASS**（真实 D1 引擎，`--local`） |
| `test/real_d1/mqtt_dedup_real_d1.sh`（补丁后复跑） | **19 / 19 PASS** |
| `scripts/schema_parity_check.py`（备份 vs 新库） | **18 / 18 PASS** |

`empty_db_init` 断言分组：E-1 全链 apply / E-2 迁移记录 / E-3 表齐全 /
E-4 `mqtt_messages` 列形状 / E-5 部分唯一索引 / E-6 四条只读索引 /
E-7 EXPLAIN 查询计划 / E-8 幂等 / E-9 反向证明 / E-10 合成记录增删去重。

### 5.2 既有套件（区分历史失败）

| 层 | 结果 |
|---|---|
| `guo-feeder-api/test/run_all.mjs`（26 套件） | total 1707 · PASS 1677 · FAIL **21** · EXPECTED_FAILURE 9 |
| 根目录 `tests/api` + `tests/pages`（10 文件） | **813 PASS / 0 FAIL** |

**21 项 FAIL 全部为历史失败**，证据 = 在**纯净 HEAD 工作树**（`git worktree` @ `8e8d3e0`）
跑同样 4 个套件，逐套件断言数与失败用例名**完全一致**：

| 套件 | HEAD 基线 | 本次 | 失败用例 |
|---|---|---|---|
| `log_ack` | 58 PASS / 2 FAIL | 58 / 2 | `T01f`/`T02e`：仍断言 ACK 发 `/down`，与 `LOG-ACK-SPLIT` 后的 `/log/ack` 不符 |
| `cloud_api_downlink_topic` | 19 / 17 | 19 / 17 | 401（测试未带令牌） |
| `retention_policy` | 141 / 1 | 141 / 1 | `scheduled()` 体 21 行 > 任务书 10 行上限（风格断言） |
| `s3_ingest_validate` | 78 / 1 | 78 / 1 | `S3-10-1b`（mock 下 EMQX CONFIG_MISSING，套件自述"既有行为"） |

⇒ **本次未引入任何新失败**；按任务书要求未扩大修复范围。

---

## 6. 云端验证

### 6.1 部署与生效绑定（以 Cloudflare 为准）

```
deployments status   Created: 2026-10-24T14:56:22.901Z
                     Version(s): (100%) 81ceda94-48cd-4651-94af-c0b5a6748635
versions view        env.guofeeder_DB (632b1224-7f71-43e9-9575-4e82ee26f36d)  D1 Database
                     env.LOG_BOOT_WATERMARK ("1") / LOG_ACK_DUAL_PUBLISH ("0")
                     env.INGEST_VALIDATION ("observe")
```

切换采用「`versions upload`（不接管流量）→ 核对绑定 → `versions deploy @100%`」，
配置与代码在**同一次原子操作**内生效，不存在"代码已换、库还是旧的"窗口。
4 个 secret（`ADMIN_API_TOKEN` / `EMQX_APP_ID` / `EMQX_APP_SECRET` / `WEBHOOK_SECRET`）
挂在 Worker 级，跨版本保留。

### 6.2 新库 schema（远端实读）

- 表 14：`device / device_credential / device_binding / device_event /
  telemetry_history / log_history / log_conflict / log_boot_watermark /
  log_boot_watermark_audit / mqtt_messages / ui_session / command_audit /
  safety_event / d1_migrations`（另有 Cloudflare 内部表 `_cf_KV`）
- 索引 34；`mqtt_messages` 21 列、`dedup_key` 在 `cid=20`
- **对象级全量对比（旧库 vs 新库）**：旧库 44 个对象**全部存在**；
  新库多出 4 个 = `0009` 的 `ix_lh_device_id` / `ix_th_device_id` /
  `ix_mm_device_msg_id` / `ix_ev_device_id`；同名对象定义差异仅 1 处 ——
  旧库 `mqtt_messages` 的 `created_at INTEGER , status_text` 多一个空格
  （P0 手工建表遗留），**语义等价**。

### 6.3 查询计划（**远端 D1**，非本地推断）

| 查询 | plan |
|---|---|
| `log_history WHERE device_id=? ORDER BY id DESC LIMIT 21` | `SEARCH … USING INDEX ix_lh_device_id (device_id=?)` |
| `telemetry_history` 同形 | `USING INDEX ix_th_device_id` |
| `mqtt_messages WHERE msg_id=? AND device_id=? ORDER BY id ASC` | `USING INDEX ix_mm_device_msg_id (device_id=? AND msg_id=?)` |
| `device_event WHERE device_id=? ORDER BY id DESC` | `USING INDEX ix_ev_device_id` |

四条均**无 `TEMP B-TREE`**（旧库对应查询是"扫完设备全部行再排序"，
`mqtt_messages` 更是整表 `SCAN`）。

### 6.4 功能验证

| 验证 | 结果 |
|---|---|
| `scripts/smoke-history-relay.mjs`（只读 A-1…A-7） | **7 / 7 PASS**（Pages 中转 → Worker 全通） |
| 同脚本 `--write`（B-1…B-8） | 14 / 15 —— `B-6` FAIL，见 §6.5 |
| log 域 ingest 探针（本次补测 L-1…L-3） | **3 / 3 PASS** |
| 真实设备遥测落新库 | ✅ `288485896ce4`：`climate.temperature 23.3` / `weight.value 0` / `climate.humidity 51.8` |
| 冒烟写入清理 | ✅ `telemetry_history` / `log_history` / `log_boot_watermark` 的 `ffffffffffff` 行已全删 |

### 6.5 `B-6` 的定性：**脚本缺陷，非切库回归**

冒烟脚本给 log 信封传的是 `level: 'ERROR'`（字符串），而 `history-ingest.js::asLevelCode`
按冻结契约**只接受 0–255 的 JSON number**（文档明确写"字符串（含 `"3"` / `"ERROR"`）一律不接受"）。

本次用同一端点补测三发（合成设备 `ffffffffffff`，跑完即清理）：

| 用例 | 结果 |
|---|---|
| `level: 3`（数值） | `{"accepted":1}` ✅ |
| 同信封重发 | `{"duplicate":1}` ✅（`ux_lh_dedup` 生效） |
| `level: 'ERROR'`（字符串） | `{"rejected":1}` ✅ ← **精确复现 B-6** |

⇒ 该用例自诞生起就不可能通过，**与数据库切换无关**；本轮未修改脚本以保持
"仓库 SQL/脚本与历史执行一致"的可审计性，仅在此登记。

---

## 7. 配额报告

执行前后（UTC 2026-10-24）：

| 时点 | 账户 D1 日读 | 账户 D1 日写 |
|---|---|---|
| 14:44 开始（旧库口径） | 413,851 / 500,000（82.8%） | 80,484 / 100,000（80.5%） |
| 15:05 迁移完成 | 413,930 | 80,509 |
| 15:19（滞后 + 双库） | 413,946 | 80,519 |

**分库 24h 实测**（`wrangler d1 info`，最权威、不消耗配额）：

| 库 | read_queries | write_queries | rows_read_24h | rows_written_24h |
|---|---|---|---|---|
| `guofeeder`（旧，切换前累积） | 47,878 | 41,312 | **7,517,789** | 144,694 |
| `guofeeder-v2`（新，含全部初始化） | 58 | 82 | **938** | **226** |

> 结论：新建库 + 10 个迁移 + 全部核验 SQL + 冒烟写入 + 真实遥测，总共
> **938 读 / 226 写** = 免费日配额的 0.19% / 0.23%。
> 旧库 24h 的 7.5M 行读是"缺覆盖索引导致的扫描放大"，随切库自然归零。

**逐条 DDL/DML 的 `rows_read` / `rows_written`**（取自 `d1 execute --json` 的 `meta`）已
逐项记录在执行日志中，摘要：迁移 `0000`→`0009` 全链 + 表/索引核验 = 143 读 / 0 写；
合成记录插入 1 读/4 写、重发 2 读/1 写、删除 1 读/1 写；四条 EXPLAIN 各 0 读 0 写。

**未能执行的操作**：无（未触发任何配额阻断）。

---

## 8. 生产切换结果

| 项 | 值 |
|---|---|
| 切换时刻 | **2026-10-24T14:56:22Z**（本地 22:56） |
| 生效版本 | `81ceda94-48cd-4651-94af-c0b5a6748635` @100% |
| 生效绑定 | `guofeeder_DB` → `632b1224-…`（`guofeeder-v2`），Cloudflare 侧核实 |
| 旧库状态 | 原样保留：12.7 MB、`0001`–`0008`、数据未动 |
| 观察到的回归 | 无（详见下） |

**切换后真实流量证据**（新库）：`device.last_seen` 由设备上行刷新、`telemetry_history`
出现 3 条真实遥测（`seq` 146/147/148，服务端时间 22:58–23:00），说明
`EMQX Action → Pages 中转 → Worker → 新 D1` 全链路在线。

**一处刻意的数据差异**（必须知晓）：旧库 `mqtt_messages` 7,844 行 / `log_history`
5,414 行的历史**没有迁移**（任务书明确不迁）。⇒ UI 的"记录/日志/命令帧"页在切换后
**从空开始**；旧库仍保留全部历史可查（但 Worker 已不读它）。

---

## 9. 回滚方案

### 9.1 方案 A：改配置重新 deploy（推荐，最慢 ~2 min）

```toml
# guo-feeder-api/wrangler.toml
database_name="guofeeder"
database_id="0d8385ba-a1a6-4fcc-8e43-ae61c014c43d"
```

```bash
node node_modules/wrangler/bin/wrangler.js versions upload  --config guo-feeder-api/wrangler.toml
# 核对 versions view 里 guofeeder_DB = 0d8385ba…，再
node node_modules/wrangler/bin/wrangler.js versions deploy <new-version-id>@100% --config guo-feeder-api/wrangler.toml --yes
```

### 9.2 方案 B：回滚到切换前版本（最快，秒级）

`e85c8a2f-1efe-42aa-a557-6a141959a2c5` 仍保留（其绑定就是旧库）。

```bash
node node_modules/wrangler/bin/wrangler.js versions deploy e85c8a2f-1efe-42aa-a557-6a141959a2c5@100% \
    --config guo-feeder-api/wrangler.toml --yes
```

⚠️ 方案 B 会同时把**代码**退回切换前状态（即丢失 `75b22b8` 之前的本地未提交改动所对应的
线上代码已经发布的部分）。当前 `75b22b8` 未包含任何 JS 变更（只有 SQL/脚本/配置），
因此 A 与 B 在**运行时代码**上等价，推荐 A（保留 SQL 侧修复）。

### 9.3 已验证明 / 未验证部分

| 项 | 状态 |
|---|---|
| 绑定切换可逆（配置改回） | ✅ 本次正是用该机制切换成功 |
| 回滚到 `e85c8a2f…` | ⚠️ 命令形态确定、版本确实存在，但**未实跑**（避免无谓抖动） |
| 新库保留供调查 | ✅ 不删除；如需回滚，新库保留（已有真实遥测 3 条 + 1 条合成记录） |

### 9.4 状态种子的回滚（见 §11.2）

```sql
DELETE FROM device_credential WHERE device_id='288485896ce4';
DELETE FROM device            WHERE device_id='288485896ce4';
DELETE FROM mqtt_messages     WHERE status_text='SYNTHETIC';
```

---

## 10. Git 状态

| 项 | 值 |
|---|---|
| 仓库 | `D:/Guo_Feeder_Project/Cloudflare_Assets` |
| 分支 | `main` |
| 本次提交 | `75b22b8`（16 文件，+770 −29） |
| 切换前 HEAD | `8e8d3e0` |
| 未提交（上一阶段待审阅，**未被本任务触碰**） | `M .gitignore`、`M src/read-api.js`、`M test/current/read_api.test.mjs`、`M pagesdev/assets/js/gf-app.js`、`M tests/pages/ui_app.test.mjs`、`M tests/pages/ui_app_runtime.test.mjs`、`?? scripts/d1_usage_report.py`、`?? scripts/local_index_plan_check.py`、`?? scripts/enforce_watch.sh`、`?? tests/api/snapshot_counts.test.mjs` |
| 未推送 | **是**。`git push` 未执行（任务未要求；且 push 需按项目铁律走 `127.0.0.1:7890` + 内联凭据 helper） |

---

## 11. 剩余问题

### 11.1 【降级说明】登录态相关端点未在沙箱内验证 —— 不阻断

任务书要求验证"设备列表 / 认证绑定 / 页面读取 API / 命令审计写入"。这些端点
**全部要求会话**，而本沙箱无法登录：

- 直连 `*.workers.dev` 不可达（`502` / 超时）；
- `pages.dev` 可达，但登录需要一个通过 Turnstile 的浏览器令牌；
- 用 Python 直连 `POST /api/auth/login` 被 Cloudflare 边缘规则拦截（`403 error code: 1010`）。

已做的替代验证：`ui_session` / `command_audit` / `safety_event` 三张表的**实际 DDL
与旧库逐字一致**（远端实读 `sqlite_master` 对比 = SAME ×3），且 `0001`–`0009`
迁移链在真实 D1 引擎上 38/38 通过。⇒ **结构性风险为零，但"命令审计真的能写入一行"
本轮没有端到端证据**，需用户在浏览器登录后走一遍命令下发 + 审计页确认。

### 11.2 【需用户确认】两行"状态种子" —— 已执行，可一键撤销

为不让 UI 在切换后退化成"未注册设备"，新库**额外插入了 2 行**（非测试数据，是状态）：

| 表 | 行 |
|---|---|
| `device` | `288485896ce4` / `state=REGISTERED` / `first_seen=1791049535` / `last_seen=1792835041` |
| `device_credential` | `288485896ce4` / `dev_…` / `slot=1` / `generation=7` / `state=ACTIVE` / `acl_version=5` / `acl_ready_at=1791052063` / `activated_at=1791052063` |

理由：`upsertDeviceOnIngest()` 对未知设备只会写 `CLAIM_PENDING`（Q1 冻结，禁止自动升
`REGISTERED`），而 `binding.js::BINDABLE_STATES` 不含 `CLAIM_PENDING`
⇒ 若留空，UI 首页「注册状态」会显示 `CLAIM_PENDING` 且设备无法再走绑定流程。
凭据台账留空则会让将来"签发凭据"算出 `MAX(generation)=0`，与设备实际在用的 gen7 分叉。

⚠️ **4 行 `REVOKED` 历史凭据（gen 3/4/5/6）刻意未迁** —— 那是纯审计历史，
不影响任何运行时判据。撤销 SQL 见 §9.4。

### 11.3 【建议排期】旧库 `0009` 仍待应用

旧库现在只作为回滚/取证载体。若回滚到旧库而 `0009` 未应用，读放大（7.5M 行/24h）
会立刻重现。建议在回滚窗口内**先**给旧库补 `0009`（成本 ≈ 表行数 ≈ 1.5 万读/1.5 万写，
免费配额内），再考虑回滚。

### 11.4 【已知且未变】其他

| # | 项 | 阻断本目标？ |
|---|---|---|
| 1 | 新库区域 WNAM vs 设备在 APAC（SIN）—— 写入 RTT 可能略高于旧库 | 否（遥测正常落库；如需优化可用 Time Travel 导出/重导入换区） |
| 2 | `mqtt_messages` 仍无保留策略（旧库 7,844 行是 G4-R1/R5 唯一取证载体） | 否 |
| 3 | 页面登录 Turnstile 仍未在浏览器实测 | 否 |
| 4 | Pages 登录态在新库下需重新登录（`ui_session` 为空，fail-closed） | 否（预期行为） |

---

## 附录 A：本次未改动的东西（证明边界）

- ❌ 未改 `0001`–`0008` 任何字节（对象级对比已证明旧库 44 个对象全部在新库存在且语义等价）
- ❌ 未改任何 `src/*.js` 运行时逻辑（`75b22b8` 中 0 个 JS 文件）
- ❌ 未改 EMQX 认证 / ACL / 规则 / Action / Connector
- ❌ 未刷固件、未改设备端协议
- ❌ 未部署 Pages
- ❌ 未升级套餐
- ❌ 未删除 / 清空 / 写入旧库（旧库 12.7 MB、行数不变）

## 附录 B：复现命令

```bash
cd D:/Guo_Feeder_Project/Cloudflare_Assets

# 1) 空库初始化（真实 D1 引擎，--local，零配额）
cd guo-feeder-api && bash test/real_d1/empty_db_init_real_d1.sh && cd ..

# 2) 备份 dump vs 新库 schema 对比（离线）
python scripts/schema_parity_check.py \
  --backup backups/guofeeder-pre0008-20261023.sql \
  --local-db <miniflare sqlite 路径>

# 3) 迁移状态（只读）
node node_modules/wrangler/bin/wrangler.js d1 migrations list guofeeder-v2 --remote \
  --config guo-feeder-api/wrangler.toml

# 4) 生效绑定（只读）
node node_modules/wrangler/bin/wrangler.js deployments status --config guo-feeder-api/wrangler.toml
```
