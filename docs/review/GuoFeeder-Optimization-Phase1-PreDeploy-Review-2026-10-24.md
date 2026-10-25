# Guo Feeder 优化 Rev.2 · 阶段一「部署准备与证据审查」报告

> 日期：2026-10-24（UTC 10-24 14:00 前后取数）
> 范围：阶段一部署前检查 / 指标口径修正 / 0009 审查 / 阶段三只读审计
> **本轮未部署、未应用迁移、未改线上 EMQX / ACL / 固件、未提交 git。**
> 上游文档：`GuoFeeder-Optimization-Plan-Rev2-2026-10-24.md`、`GuoFeeder-Optimization-Phase1-Report-2026-10-24.md`、
> 原始证据 `docs/evidence/D1-Worker-Usage-2026-10-23/*.json`

---

## 0. 结论摘要（先看这 6 条）

1. **阶段一产物齐全、与代码一致**，但原报告两处表述必须修正：
   - 「全量 813 PASS」**只是 `tests/` 目录**的口径。Worker 自有的 `guo-feeder-api/test/` 26 套件当时**未跑** —— 而它正是 `read-api.js` 的覆盖者。补跑后发现 **2 项真回归（我引入）**，已修复；现 `read_api` 46/46 PASS。
   - 「3.25 行写 ≈ 1 表 + 2 索引」是**推断**。官方实测为 **3.24 行写/次**，但按其索引数（2 个）应得 3.00 —— **存在 +8% 未解释**，已登记为待核实，不得作为决策依据。
2. **★ 新增一条部署阻断级风险（此前完全遗漏）**：官方文档明确 **DDL（CREATE INDEX）也计 Rows Read / Rows Written**。若 `log_history` 有 20 万行，建一条索引 ≈ 20 万写 = **2 倍日配额**；若达到 180 天保留上限（≈211 万行）则为 **21 倍配额**。**0009 在免费套餐上很可能直接失败**，必须先量体再决定，或换付费窗口。
   > **【2026-10-24 22:2x 更正】** 该风险**已被实测证伪**：本地备份离线统计显示全库仅 **16,573 行 / 7.80 MB**（`log_history` 5,414 行），四条索引 DDL ≈ **1.5 万写 = 免费日配额 15%**，秒级完成。**不构成阻断**。见 §4.6 更正块与
   > `GuoFeeder-Optimization-DB-Route-Decision-2026-10-24.md`（结论：保留现有库，全程免费）。
3. **0009 的四条索引与实际生产 SQL 逐字对齐**（非推测）：`log WHERE device_id=? ORDER BY id DESC`、`mqtt WHERE msg_id=? AND device_id=? ORDER BY id ASC`、`telemetry/device_event` 同型。本地 `EXPLAIN` 前后对比：BEFORE 有 `USE TEMP B-TREE`（mqtt 更差：`SCAN mqtt_messages`）→ AFTER 全部纯索引 SEARCH。
4. **索引与去重唯一索引不冲突**：`ux_lh_dedup(device_id,boot_seq,seq)`、`ux_th_dedup`、`ux_mm_dedup_key(dedup_key)` 的前缀与 `(device_id,id)` / `(device_id,msg_id,id)` **不同族**，职责不重叠；本轮不动任何去重键。
5. **读收益重算**（同等工作负载，10-23 基线 7,449,500 读）：索引可治 **68.3%**，移除 COUNT 调用者 **22.0%**，合计 **90.3%**；加上修掉我自己的验证脚本（3.8%）后，**估算落点 44~46 万读（−93.9%~−94.1%）**。标注为**估算**，依赖"索引 seek 行数"假设。
6. **阶段三只读审计关键结论**：`log_boot_watermark` 是**纯写入证据表**——除自身模块外**无任何读取方**（`log_ack` 零引用、bootseq guard 读的是 `log_history`）。其开关 `LOG_BOOT_WATERMARK` **生产已设为 "1"**，占用 **32,142 写/日（36%）**。**一行配置**即可清零，但会失去 HD-3·A-1 证据 ⇒ 列为决策项，不擅自改。

---

## 1. 基线（实际仓库状态，非报告引用）

| 项 | 值 | 证据 |
|---|---|---|
| 固件仓 | `wb` @ `3cdd216`，工作区 8 个未跟踪文档（无代码改动） | `git status` |
| 云端仓 | `main` @ `8e8d3e0`，**5 改 + 4 新增**（阶段一产物，未提交） | `git status` |
| 线上 Worker | `e85c8a2f-1efe-42aa-a557-6a141959a2c5`，创建 **2026-10-23T22:37:42Z** = **北京 10-24 06:37** | `wrangler deployments status` |
| 线上 Pages | 阶段一代码**未部署**（本会话无 deploy 动作） | 命令历史 |
| 数据库 | `guofeeder_DB` / `0d8385ba-…`，迁移 0001–0008 已应用，**0009 未应用** | 迁移目录 + 本会话无 apply |

**时区口径说明**：Cloudflare 免费额度按 **UTC 00:00** 重置（官方文档明示）。本项目历史文档混用 UTC 与北京时（如上面 `22:37Z` = 次日 06:37 北京），**凡引用日期必须成对标注**，本轮全部按 UTC 标注并在数据表中注明。

**未提交改动清单（云端仓）**

| 文件 | 性质 |
|---|---|
| `guo-feeder-api/src/read-api.js` | 代码（snapshot `counts` opt-in + 校验前置） |
| `guo-feeder-pagesdev/assets/js/gf-app.js` | 代码（首页缓存驱动/去 COUNT/手动刷新） |
| `guo-feeder-api/migrations/0009_ui_read_indexes.sql` | 迁移**草案**（未应用） |
| `scripts/d1_usage_report.py` | 只读观测脚本 |
| `scripts/local_index_plan_check.py` | 本地计划对比脚本 |
| `tests/api/snapshot_counts.test.mjs` | 新增测试 |
| `tests/pages/ui_app.test.mjs`、`ui_app_runtime.test.mjs`、`.gitignore` | 测试与忽略项 |
| `guo-feeder-api/test/current/read_api.test.mjs` | **本轮修正**（契约对齐） |

---

## 2. 测试核验（补跑 + 归因）

### 2.1 两套测试框架，此前只跑了一套

| 框架 | 套件数 | 结果 | 说明 |
|---|---|---|---|
| `tests/`（Pages/UI + 部分 API） | 10 | **813 PASS / 0 FAIL** | 原报告口径 |
| `guo-feeder-api/test/`（Worker 契约：BT-1/HD-3/retention/ingest） | 26 | **1,677 PASS / 21 FAIL** | **此前未跑** |

### 2.2 21 项失败全部为既有（用独立 worktree 在 HEAD 复现）

方法：`git worktree add /tmp/gf_head_check HEAD`（不动当前工作区），在纯净 HEAD 上跑同 5 个套件。

| 套件 | 纯净 HEAD `8e8d3e0` | 当前工作区 | 归因 |
|---|---|---|---|
| `read_api` | 43 PASS / 0 FAIL | 曾 41/2 → **已修 46/0** | ★ **我引入的回归**（见 2.3） |
| `log_ack` | 58 PASS / **2 FAIL** | 58 / 2 | 既有（topic 断言写的是 `.../down`，实际 `.../log/ack` —— BT-1 拆分 topic 后测试未同步） |
| `cloud_api_downlink_topic` | 19 PASS / **17 FAIL** | 19 / 17 | 既有（mock 下 401/CONFIG_MISSING） |
| `retention_policy` | 141 / **1 FAIL** | 141 / 1 | 既有（`scheduled()` 体 21 行 > 任务单 §5.1 的 10 行约束） |
| `s3_ingest_validate` | 78 / **1 FAIL** | 78 / 1 | 既有（自述"属既有行为"） |

**结论**：阶段一改动**未引入任何残留失败**；21 项失败与改动无关，应作为独立缺陷登记（**不属本阶段范围，本轮不修**）。

### 2.3 我引入的真回归（已修）

`counts` 从「默认计算」改为「opt-in」改变了端点契约，`read_api.test.mjs` 的 R-6-3（期待 `counts.log===2094`）、R-6-5（期待 6 次 DB 调用）随之失败。
修正方式：把断言改为**新契约**并补 3 项：

- `R-6-3` 默认 ⇒ `counts === null` 且 `counts_included === false`
- `R-6-5` 默认 ⇒ **5 次** DB 调用（无 COUNT）
- 新增 `R-6-8/9/10`：`counts=1` ⇒ 真实计数 / 6 次调用 / COUNT 带 `device_id` 过滤

⇒ `read_api` **46 PASS / 0 FAIL**。

### 2.4 待办（测试覆盖缺口，登记不修）

- `log_ack` 与 `cloud_api_downlink_topic` 的既有失败说明**这两个套件当前不可用作验收依据**；若后续动日志链路，需先修测试再谈 PASS。
- `tests/` 与 `test/` 两套框架**无统一入口**，容易再次出现"只跑一半"。建议阶段二加一行 `npm test` 聚合（工作量小，收益是防再犯）。

---

## 3. Cloudflare 指标定义（官方原文）与本项目口径

来源：<https://developers.cloudflare.com/d1/platform/pricing/>（本轮检索确认，2026-10-24）。

> **Rows read** measure how many rows a query reads (scans), regardless of the size of each row. … A query that filters on an unindexed column may return fewer rows to your Worker, but is still required to read (scan) more rows to determine which subset to return.
>
> **Rows written** measure how many rows were written to D1 database. Write operations include INSERT, UPDATE, and DELETE. Each of these operations contribute towards rows written. A query that INSERT 10 rows into a users table would count as 10 rows written.
>
> **DDL operations** (for example, CREATE, ALTER, and DROP) … **may contribute to a mix of read rows and write rows.**
>
> **Indexes will add an additional written row** when writes include the indexed column, as there are two rows written: one to the table itself, and one to the index. … Writing to columns referenced in an index will add **at least one (1)** additional row written…
>
> 免费额度：Rows read **5,000,000/日**，Rows written **100,000/日**，Storage 5 GB。**Free limits reset daily at 00:00 UTC。**
>
> 每查询返回 `meta.rows_read` / `meta.rows_written`；Dashboard 与 Analytics API 提供同口径数据。

### 3.1 术语严格区分（本项目此前混用过）

| 概念 | 定义 | 可否当"记录数" |
|---|---|---|
| `COUNT(*)` | 指定时刻+过滤条件下的记录数 | ✅ 精确，但成本 = 扫全分区 |
| `MAX(id)` | 最大 ID 值 | ❌ 不是记录数 |
| `MAX(id)` 增量 | 两次采样之差 | ⚠️ 仅当**同表、同设备、无删除、无 ID 复用**时才是"新增条数"的代理 |
| `Rows Written` | 按官方口径的写入行数（含索引维护） | ❌ **不等于**成功插入的业务记录数 |
| `Rows Read` | 扫描行数 | ❌ 与返回行数无关 |
| 执行次数 | 语句执行次数 | ❌ 不等于记录数 |

### 3.2 生产实测倍率（官方统计反推，非估算）

数据源：`d1-queries-2026-10-23.json`（10-23 UTC 全天，53 个查询形态，共 60,210 次执行）。

| 语句 | exec | Rows Read | Rows Written | wr/exec | rd/exec |
|---|---|---|---|---|---|
| `INSERT OR IGNORE INTO log_history` | 11,725 | 75,117 | 38,046 | **3.24** | 6.4 |
| `INSERT INTO log_boot_watermark`(upsert) | 10,714 | 21,428 | 32,142 | **3.00** | 2.0 |
| `INSERT INTO log_conflict` | 1,227 | 11,157 | 4,680 | 3.81 | 9.1 |
| `INSERT OR IGNORE INTO telemetry_history` | 1,025 | 4,896 | 4,080 | **3.98** | 4.8 |
| `INSERT INTO mqtt_messages` | 1,120 | 1,120 | 3,228 | 2.88 | 1.0 |
| `INSERT INTO command_audit` | 227 | 2,025 | 1,575 | **6.94** | 8.9 |
| `UPDATE command_audit SET state=…`（5 种） | 751 | 751 | 2,253 | **3.00** | 1.0 |
| `UPDATE ui_session SET last_seen_at` | 871 | 871 | 871 | **1.00** | 1.0 |

**模型校验**：
- `telemetry_history` 有 **3 个索引**（`ix_th_device_ts`、`ix_th_device_type_ts`、`ux_th_dedup`）⇒ 1 表 + 3 索引 = 4.00，实测 **3.98** ✅ **模型成立**。
- `command_audit` 的 `UPDATE` 触及 `state` 列（被 `ix_ca_state`、`ix_ca_retain` 覆盖）⇒ 1 + 2 = 3.00，实测 **3.00** ✅。
- `ui_session.last_seen_at` 无索引 ⇒ 1.00，实测 **1.00** ✅。

**未解释项（登记为待核实，不作决策依据）**：
`log_history` 仅 2 个索引（`ix_lh_device_ts`、`ux_lh_dedup`）⇒ 模型预测 3.00，实测 **3.24（+8%）**。两种互斥假设：
① 生产库存在**迁移文件之外的第三个索引**（人工建过）；
② `INSERT OR IGNORE` 冲突路径仍计入部分写。
**验证办法（低成本，需授权，预算 ≈ 3 行读）**：
`PRAGMA index_list(log_history)`（1 行）+ 一次受控单行 INSERT 并读 `meta.rows_written`（1 写）。二者都不改业务数据。

### 3.3 由此推翻/修正的历史表述

| 原表述 | 修正后 |
|---|---|
| 「log 当前 3.25 行写 ≈ 1 表 + 2 索引 ⇒ 4.25」 | 实测 **3.24**；官方仅保证"每个索引 ≥ +1 行写"。**新增 1 索引 ⇒ 估算 +1 行写/成功插入**，不是精确等式 |
| 「17 号审计：会话 touch 是主要写源」 | ❌ 已被官方数据推翻（871 写/日 = 1.0%），17 号文档已加修正注记 |
| Rev.2「可让 84% 的读消耗归零」 | 精确化：索引可治 **68.3%** + COUNT 调用者移除 **22.0%** = **90.3%**（见 §5.1） |
| Rev.2「813 PASS 全量」 | 实为 `tests/` 单框架；**另一框架 1,677 PASS / 21 FAIL（全既有）** |

---

## 4. Migration 0009 部署前审查（按指令 8 项逐条）

### 4.1 迁移前实际表结构与索引清单（源码口径）

| 表 | 主键 | 现有索引 | 本轮新增 |
|---|---|---|---|
| `log_history` | `id INTEGER PK AUTOINCREMENT` | `ix_lh_device_ts(device_id, ts DESC)`、`ux_lh_dedup(device_id, boot_seq, seq)` | `ix_lh_device_id(device_id, id)` |
| `telemetry_history` | 同上 | `ix_th_device_ts`、`ix_th_device_type_ts`、`ux_th_dedup(device_id,type,ts,seq)` | `ix_th_device_id(device_id, id)` |
| `mqtt_messages` | `id` | `ux_mm_dedup_key(dedup_key)` | `ix_mm_device_msg_id(device_id, msg_id, id)` |
| `device_event` | `id` | `ix_event_device_time(device_id, created_at DESC)`、`ix_event_type_time` | `ix_ev_device_id(device_id, id)` |

⚠️ **生产实际索引未核实**（不查生产）。差异风险：若生产有手工索引，可能与新索引职责重叠 ⇒ 部署前用 `PRAGMA index_list(<table>)`（4 条，各 1 行读）确认。

### 4.2 四个候选索引 ↔ 真实生产 SQL（逐字）

| # | 生产 SQL（10-23 官方原文） | exec | 读 | 覆盖索引 |
|---|---|---|---|---|
| Q1 | `SELECT id, ts, level, code, boot_seq, seq, message FROM log_history WHERE device_id = ? ORDER BY id DESC LIMIT ?` | 156 | 3,404,970 | `ix_lh_device_id` ✅ 等值前缀 + 排序键 |
| Q4 | `SELECT id, ts, type, value, source FROM telemetry_history WHERE device_id = ? ORDER BY id DESC LIMIT ?` | 114 | 358,116 | `ix_th_device_id` ✅ |
| Q3 | `SELECT … FROM mqtt_messages WHERE msg_id = ? AND device_id = ? ORDER BY id ASC LIMIT ?` | 152 | 1,262,610 | `ix_mm_device_msg_id` ✅ 双等值 + 排序键 |
| Q10 | `SELECT * FROM device_event WHERE device_id = ? ORDER BY id DESC LIMIT ?` | 110 | 68,430 | `ix_ev_device_id` ✅ |

### 4.3 本地查询计划与适用边界

脚本：`scripts/local_index_plan_check.py`（本地 SQLite 复刻 schema，非生产）。

```
[BEFORE] Q1  SEARCH log_history USING INDEX ix_lh_device_ts (device_id=?) | USE TEMP B-TREE FOR ORDER BY
[BEFORE] Q3  SCAN mqtt_messages                       ← 整表扫（唯一索引是 dedup_key，与查询无关）
[AFTER ] Q1  SEARCH log_history USING INDEX ix_lh_device_id (device_id=?)
[AFTER ] Q3  SEARCH mqtt_messages USING INDEX ix_mm_device_msg_id (device_id=? AND msg_id=?)
RESULT: PASS（5/5 计划改善）
```

**边界（必须如实标注）**：
- 本地库是**合成小数据**，SQLite 规划器在极大数据下**理论上**可能改选；但本例是"等值 + ORDER BY 索引列"，属稳定形态。
- **生产计划尚未验证**（不查生产）。
- ⚠️ 修正一处原判读：**Q2 的 COUNT 不会因索引而变便宜**——`COUNT(*)` 必须遍历该设备全部索引项；`EXPLAIN` 显示前后都用 covering index，行数不变。**COUNT 的唯一解是"不调用"**，不是加索引。

### 4.4 覆盖性、冗余与写入成本

**冗余判定**
- `ix_lh_device_id(device_id, id)` vs `ux_lh_dedup(device_id, boot_seq, seq)`：**不冗余**。前者第二列是 `id`（rowid），后者是 `boot_seq` —— 无法互相替代排序。
- `ix_lh_device_id` vs `ix_lh_device_ts(device_id, ts DESC)`：**职责不同**（按 id 翻页 vs 按时间窗过滤），保留两条。但**存在可优化空间**：若未来 `log_history` 只按 id 翻页，可评估删 `ix_lh_device_ts`（需先证明 retention 的 `ts` 谓词不再用它）——**本轮不动**。
- 不建 `(device_id, ts, id)` 三列：与 `ix_lh_device_ts` 前缀重叠，写成本翻倍、收益边际。

**写入成本（估算，模型见 §3.2）**
| 表 | 相关写入 exec/日 | 估算新增写 | 说明 |
|---|---|---|---|
| `log_history` | 11,725 | **+9.5k ~ +11.7k** | 成功插入率约 81%（由 3.24/4.00 反推），冲突不增索引项 |
| `telemetry_history` | 1,025 | +1.0k | |
| `mqtt_messages` | 1,157 | +1.2k | |
| `device_event` | 41 | +0.04k | |
| **合计** | | **≈ +11.7k（+13.2%，基线 88,457）** | 官方保证"每索引 ≥ +1"，故为**下界量级** |

**存储（修正原稿的错误假设）**：原稿按"4 万行/日"算出 0.8 MB/日。实测 log 为 **11,725 行/日** ⇒ 索引约 **0.23 MB/日**，180 天 ≈ **42 MB**（免费额度 5 GB，可忽纳）。

### 4.5 与去重索引的职责冲突证明

- 去重契约=`(device_id, boot_seq, seq)` / `(device_id, type, ts, seq)` / `dedup_key`，服务于 `INSERT OR IGNORE` 的**幂等冲突判定**与 `log_conflict` / BT-1 / HD-3 协议。
- 新索引服务于 `WHERE device_id=? ORDER BY id`，**不参与任何唯一性判定**（全部是 `CREATE INDEX`，非 `UNIQUE`）。
- ⇒ **无冲突**。0009 不改表、不改列、不改约束、不删索引。

### 4.6 ★ DDL 自身的成本（此前遗漏，可能阻断部署）

> **【2026-10-24 22:2x 更正 · 重要】**
> 下表是**按保留期反推的上界**，**不是实际行数**。依据 `backups/guofeeder-pre0008-20261023.sql`
> （10-23 23:38 全库 dump）离线逐行统计，**真实总量只有 16,573 行 / 7.80 MB**：
> `log_history` **5,414 行**（非 211 万）、`telemetry` 1,217、`mqtt_messages` 7,844、`device_event` 385。
> ⇒ 四条索引的 DDL 实测成本约 **1.5 万读 / 1.5 万写 = 免费日配额 0.3% / 15%**，
> **秒级完成，不构成部署阻断**；本节"21× 配额"的结论**作废**（仅保留为"若将来真增长到 200 万行时的边界分析"）。
> 决定：**保留现有库、全程免费**，详见 `GuoFeeder-Optimization-DB-Route-Decision-2026-10-24.md`。

官方："DDL operations (CREATE, ALTER, DROP) … **may contribute to a mix of read rows and write rows**"。

建索引需扫描全表 + 写入索引项 ⇒ **成本 ≈ 表行数（读）+ 表行数（写）**。

| 表 | 行数上界（按保留策略 × 当前速率） | 建索引估算写 |
|---|---|---|
| `log_history` | 11,725 × 180 天 ≈ **2.11M** | 最坏 **211 万写 = 21× 日配额** |
| `telemetry_history` | 1,025 × 90 天 ≈ 92k | 9.2 万写 ≈ 1× 日配额 |
| `mqtt_messages` | 1,157/日 × 无保留上限 | **未知，可能很大** |
| `device_event` | 41/日 | 很小 |

**⇒ 判定：0009 不能"直接 apply"。** 必须先量体（见 §4.7），且在**当日写配额有余量**的窗口执行；若 `log_history` > 约 5 万行，免费套餐极可能中途触顶（写配额 10 万/日）。

**缓解方案（按推荐序）**
1. **先量体**：`SELECT MAX(id) FROM log_history`（rowid 上 O(1)，≈1 行读）× 4 表，合计 **≈4 行读**。
2. 若最大表 > 5 万行：**临时升级 Workers Paid（$5/月）**跑 DDL，之后可退回 Free（数据保留）。
3. 或**分表推进**：先建小表（`device_event`/`telemetry`），`log_history`/`mqtt_messages` 待付费窗口。
4. ❌ 不可用"缩短保留期再建索引"作为默认手段——那是**删数据**，需单独授权。

### 4.7 失败 / 部分执行 / 回滚

- **执行粒度**：wrangler 会按迁移文件记录版本号；D1 对文件内的多语句是否**同事务**，本轮**未验证**（不查生产）。因此**按单条执行 + 逐条校验**，不用整文件一键 apply。
- **部分执行**：`CREATE INDEX IF NOT EXISTS` 幂等 ⇒ 重跑安全；若第 2 条失败，第 1 条已生效 ⇒ **不会造成数据损坏**（只多一个索引）。
- **回滚**：文件内注释已给出 4 条 `DROP INDEX`（**故意不建 .sql**，避免被 wrangler 当迁移执行）。索引删除**不改数据**，回滚安全。
- **不可回滚项**：无（本迁移不改数据）。

### 4.8 部署后如何验证（含 STOP 判据）

| 指标 | 方法 | 预算 |
|---|---|---|
| 索引是否被采用 | `EXPLAIN QUERY PLAN <Q1/Q3/Q4/Q10>`（**生产实测 8 次执行 / 0 行读**，见证据 JSON） | ≈0 行读 × 4 |
| 读是否下降 | GraphQL `d1QueriesAdaptiveGroups` 逐 SQL 的 `rowsRead`（Q1 应从 21,827/次 → 数十行） | 0（分析 API 不计 D1） |
| 写是否异常 | 同 API 的 `rowsWritten`，与 §4.4 估算比对 | 0 |
| 分页正确性 | UI 日志/帧页翻页条数、`next_before_id` 连续性 | 由 UI 手测 |

**STOP（立即停并回报）**：
1. 任一条 `EXPLAIN` 显示**未采用**新索引（仍是 TEMP B-TREE / SCAN）；
2. `rowsWritten` 增幅 **> 2× 估算上限**（即 > +23.4k/日）；
3. DDL 执行报错、或触顶当日写配额；
4. 出现任何"改表/改约束/动去重键"的需求 ⇒ **超出授权，停**。

---

## 5. 阶段一验收定义（可测量版）

### 5.1 读降幅 ≥ 70%

- **基线**：`d1AnalyticsAdaptiveGroups` 的 **UTC 日** `rowsRead`（10-23 = **7,299,585**；查询级汇总 7,449,500，差异 2.0% 属聚合口径不同，**配额口径用日级**、**排序用查询级**）。
- **优化后**：部署后**同一 UTC 日**（`D+1`）同口径取值。
- **归一化**：日负载差异大（10-21 = 95.6 万读、10-23 = 730 万读），故**必须先归一化**：
  `每千次 Worker 请求的读量 = rowsRead ÷ (requests/1000)`
  10-23：7,299,585 ÷ 26.75 = **272,900 读/千请求**。
- **判定**：`归一化后降幅 = 1 − 新值/旧值 ≥ 70%`。估算落点 −93.9%~−94.1%（见 5.4），**余量充足**。
- **不得**用"绝对读量下降"直接判胜（可能只是当天用得少）。

### 5.2 写增幅 ≤ 5%

- **基线**：10-23 `rowsWritten = 88,457`（查询级 89,485）。
- **问题**：写量几乎全部由**设备日志产生量**驱动，与 UI 无关 ⇒ 必须归一化，否则"今天设备少发日志"会伪造出"优化成功"。
- **归一化口径**：`每千条 log_history 插入的写量 = rowsWritten ÷ (log INSERT exec / 1000)`。
  10-23：88,457 ÷ 11.725 = **7,544 写/千条日志**。
- **判定**：新增索引后该值应上升 ≈ +1,000（+13.2%）。
- ⚠️ **如实说明**：Rev.2 的"写增幅 ≤ 5%"在当前设计下**不可能达成** —— 因为索引必然增加写，而阶段三（watermark 降频）尚未实施。**必须成对验收**：`索引(+13%) + watermark(−36%) → 净 −23%`。故本条应改为：**「指数合计量」验收：`Δ索引 ≤ +15%` 且 `Δ合并后 ≤ −20%`**。此项需用户裁决（见 §10 D5）。

### 5.3 列表查询不退化 / COUNT 依赖移除

- 列表：`EXPLAIN` 显示纯索引 + `LIMIT` 生效；翻页返回条数与 `next_before_id` 单调；**响应体字段不变**（仅少 `counts`，且 `counts_included=false` 显式声明）。
- COUNT：代码级断言 `S-16-1..7`（首页/设备页 `counts=0`、无 `metrics` 调用、`counts=null` 契约）+ Worker 测试 `R-6-3/5/8/9/10`；**并由 10-24 观测确认** `SELECT (SELECT COUNT(*)…)` 的 exec **降为 0**（GraphQL 逐 SQL 可见）。

### 5.4 读收益重算（估算，非实测）

| 项 | 10-23 读 | 优化后估算 | 依据 |
|---|---|---|---|
| Q1 log 列表 | 3,404,970（45.7%） | 156 × 20~60 ≈ 3.1k~9.4k | 索引 seek |
| Q3 mqtt 列表 | 1,262,610（16.9%） | 152 × 2~10 ≈ 0.3k~1.5k | 双等值 |
| Q4 telemetry | 358,116（4.8%） | 114 × 20~60 ≈ 2.3k~6.8k | |
| Q10 device_event | 68,430（0.9%） | 110 × 20~60 ≈ 2.2k~6.6k | |
| Q2 snapshot COUNT | 1,471,424（19.8%） | **0** | UI 不再调用 |
| metrics COUNT | 165,428（2.2%） | **0** | 同上 |
| 我自己的验证脚本 | 286,064（3.8%） | **0** | 改写为 `LIMIT 1` 取 boot_seq |
| 其余 46 种 | 432,458 | 432,458 | 未触及 |
| **合计** | **7,449,500** | **≈ 44~46 万（−93.9%~−94.1%）** | 估算 |

**不确定度来源（如实）**：① 索引 seek 的实际行数（本地无法等价测量）；② 部署当日负载结构不同；③ 生产是否复用同一计划（未验证）。

### 5.5 本地 vs 生产（禁止混淆）

本轮所有数字分三类：**官方测量**（GraphQL/Dashboard，标 UTC）、**本地测量**（SQLite EXPLAIN、测试 PASS 数）、**估算**（收益、写增量）。**没有任何"生产验收结果"** —— 一次生产调用都没做。

---

## 6. 生产部署审查包（备好，未执行）

| 项 | 内容 |
|---|---|
| 变更范围 | ① Worker：`read-api.js`（snapshot `counts` opt-in + 校验前置）② Pages：`gf-app.js`（首页缓存驱动、去 COUNT、手动刷新）③ D1：0009 四条索引（**单独决定**）。不改表/不改约束/不改去重键 |
| 影响面 | 只读端点契约新增 `counts_included`；`counts` 默认 null。**唯一调用方是 UI 自身，已适配**（全仓 grep 确认：除 UI 与测试外无调用方） |
| 备份 | 0009 仅加索引、不改数据 ⇒ **数据备份非必需**。⚠️ 若坚持备份：`wrangler d1 export` 会**全表读**（≈表行数），本身耗配额，须计入预算 |
| 部署前检查 | ① 当日 UTC 写配额余量（`d1Analytics` 当日 rowsWritten）② 4 表 `MAX(id)` 量体 ③ `PRAGMA index_list` 核现有索引 ④ `git diff` 复核 ⑤ 两套测试全绿（除 §2.2 既有失败） |
| 部署时序 | **代码先、索引后**（代码是零风险收益：−22%），索引待量体+配额确认后再单独执行 |
| 部署后观测 | `D+1` UTC 取 `d1Analytics` + `d1Queries`；比对 Q1/Q3/Q4/Q10 的 rd/exec、COUNT 类 exec 是否归零、rowsWritten 归一化值 |
| 回滚 | 代码：`wrangler rollback`（Worker 保留历史版本）；索引：4 条 `DROP INDEX`；UI：Pages 回退上一部署 |
| 失败判据 | §4.8 四条 STOP |
| **请求预算** | 部署前 ≤ 8 行读（MAX(id)×4 + index_list×4）、部署后分析 API **0 行读**、`EXPLAIN`×4 ≈ 0 行读、UI 手测 ≤ 20 次请求。**合计 ≤ 30 行 D1 读、0 写** |

---

## 7. 前端请求与命令响应语义（P2-4 前置核查）

### 7.1 `counts` 契约兼容性（全调用方）

| 场景 | 响应 | 兼容性 |
|---|---|---|
| 默认（不传 `counts`） | `counts: null`、`counts_included: false` | UI 已不读 `counts`；旧外部调用若读 `counts.log` 会得 `null` ⇒ **需在部署说明中标注为破坏性变更** |
| `counts=0` | 同上 | UI 全部走这条 |
| `counts=1` | 真实三值 + `counts_included: true` | 测试 `R-6-8..10` 覆盖 |
| 非法 `counts`/`recent` | **400，且 0 次 SQL**（校验已前置） | 修正了"先白跑 5 条查询再报错" |

**调用方清点**：`gf-app.js`（2 处快照调用，均带 `counts=0`）、`read_api.test.mjs`、`snapshot_counts.test.mjs`。**无脚本/无第三方**。

### 7.2 现有轮询语义（P2-4 要改的对象）

`runCommand`（`gf-app.js:262-292`）实测行为：

| 项 | 现状 | 问题 |
|---|---|---|
| 间隔 | 固定 **1500 ms** | 无退避；设备慢时白打 |
| 上限 | **75,000 ms** ⇒ 最多 **50 次** | 每轮 = 1 HTTP + 会话校验(2 D1) + 1 audit 读 ⇒ 单条慢命令 ≈ **150 D1 ops** |
| 页面切换 | **不取消** | 用户离开页面后循环仍在跑（真实缺陷，最多再打 50 次） |
| 页面隐藏 | **无检查** | 后台标签页照轮询 |
| 回帧取回 | 终态后**只取一次** `/api/read/history/frames` | ✅ 合理 |

### 7.3 帧保留规则（P2-4 红线）

`/api/read/history/frames` 的实现是 `WHERE msg_id=? [AND device_id=?] ORDER BY id ASC LIMIT ?`，**不做任何去重、不按类型过滤** ⇒ ACK / result / alert 全部保留。**这是正确行为，不得改动。**

`latestFramePayload()` 取"最后一个可解析 payload"——**它只能用于"取结果值"，不能用于判定类型**。已有真实踩坑记录：验证脚本曾把 ACK 帧误当 result 帧。

**P2-4 的去重规则（必须遵守）**：
1. 去重键 = `(frame.id)`（或 `(cmd_id, direction, s)`），**禁止**按 `cmd_id` 单键去重 ⇒ 否则会丢 ACK 与 result 之一；
2. 轮询退避：建议 1.5s → 2s → 3s → 5s（上限 5s），总时长不变 75s（请求数 50 → ~20）；
3. 页面切换 / `visibilitychange` = hidden ⇒ **立即停止**并保留已得状态；
4. 旧请求不得覆盖新状态：沿用现有 `S.deviceGen` / `S.logGen` 代际守卫，但 `runCommand` 目前**没有**代际守卫 ⇒ 需补。

---

## 8. 阶段三只读审计初步结果

### 8.1 `log_boot_watermark` 职责审计

**全量引用清单（代码/迁移/开关/运行时）**

| 位置 | 作用 |
|---|---|
| `src/log-boot-watermark.js`（655 行） | 读旧水位（1 SELECT）+ 逐设备 upsert（1 batch）；纯函数 `analyzeBatch` 分类 advance / rewind / ambiguous |
| `src/log-boot-watermark-rebase.js`（708 行） | 人工"水位换代"：CAS + 审计写入 `log_boot_watermark_audit`（幂等键 `rebase_id`） |
| `src/history-ingest.js:309` | **唯一生产调用点**，条件：`domain==="log" && rows.length>0 && env.LOG_BOOT_WATERMARK==="1"` |
| `src/index.js:1387` | `POST …/watermark/rebase` 管理端点（UI **不调用**） |
| `src/read-api.js:679` | 仅回显开关值（`env.LOG_BOOT_WATERMARK`），**不读表** |
| `src/log-bootseq-guard.js` | **读 `log_history`**（`LOG_BOOT_SPAN_SQL`），**不读水位表** |
| `wrangler.toml:40` | `LOG_BOOT_WATERMARK = "1"` ⇒ **生产开启** |
| 迁移 `0005` / `0006` | 表 + 3 索引（PK autoindex、`ix_lbw_updated`、`ix_lbw_rewind`）+ 审计表 |

**关键判定**：
- 对 **日志补传 / ACK / 重试 / 幂等**：**零耦合**。`log-ack.js` 对 `watermark` 的引用数 = **0**；`acked_seq` 走独立路径。
- 对 **`boot_seq` rewind 检测**：watermark 是**摄取时**的候选检测器；但另有 `log-bootseq-guard`（cron，读 `log_history`，默认关）可作为替代取证路径。
- 对 **首页统计**：无关（首页从未读它）。
- ⇒ **它是纯"证据/审计"表：写多读零**。

**成本**：32,142 写/日（36% 的全部写），10,714 次 upsert，**每次 3.00 行写**。

**四个处置方案对比（本轮只比较，不改）**

| 方案 | 日写 | 证据影响 | 改动 | 风险 |
|---|---|---|---|---|
| A 保留现状 | 32,142 | 完整 | 无 | 持续吞噬 32% 写配额 |
| **B 仅状态变化时写**（推荐） | **≈ 每次 boot 前进/回退才写**（估 <100/日） | 保留 advance/rewind/ambiguous 证据；**失去 `obs_count`/`last_seen_at` 活性** | 代码（加 `if (a.nextMax !== prev.max \|\| rewind \|\| ambiguous)` 守卫）+ 测试 | 中：需重跑 watermark 套件（现有 2 套 47 项） |
| C 仅移除 `obs_count` | **仍 32,142** | 无改善 | 小 | ❌ 无效（upsert 仍每批执行） |
| D 关开关（`LOG_BOOT_WATERMARK="0"`） | **0** | **完全失去 HD-3·A-1 证据** | **一行配置，零代码** | 高（证据断链，HD-3 已 CLOSED 的依据失效） |
| E 彻底删除表/代码 | 0 | 同 D + 历史审计记录丢失 | 大（含迁移） | 高，不建议 |

**推荐**：以 **B** 为主（保留证据 + 削 99% 写）；**D 仅作应急闸门**（配额告急时临时关，事后补采）。**不推荐 E**。

### 8.2 唯一 ID / 排序 / 去重的分离设计（仅设计）

**现状的四层身份（已存在，互不替代）**
1. **记录主键**：`id INTEGER PRIMARY KEY AUTOINCREMENT` —— 全库唯一、单调（不复用），用于排序与翻页。
2. **业务去重键**：`(device_id, boot_seq, seq)` —— 设备语义幂等；`ux_lh_dedup` 唯一约束。
3. **协议序列号**：`seq`（设备侧单调）+ `boot_seq`（重启代次）+ `acked_seq`（ACK 水位）。
4. **云端关联**：`cmd_id`（UI 生成 `ui-NNNNNN-slug`，30 s 去重窗口）。

**候选"时间戳组合 ID"评估**

| 方案 | 唯一性 | 单调性 | 主要风险 |
|---|---|---|---|
| `ts(ms) + type + 随机后缀` | 概率唯一（需后缀） | ❌ 并发同时刻 | 同步多设备同毫秒、时钟回拨 |
| `ts(us)` | 依赖时钟精度 | ❌ | ESP32 `gettimeofday` 精度/漂移；断电回拨 |
| `device_id + boot_seq + seq`（现有） | **确定性唯一** | 按设备单调 | 需设备侧 seq 不重置（HD-3 已处理 rewind） |
| 雪花式（时间+机器+序列） | 确定唯一 | 近似单调 | 需新增 40+ bit 机器位分配；跨设备时钟偏差 |

**结论**：**不引入新 ID 方案**。现有 `AUTOINCREMENT id`（排序）+ `(device_id,boot_seq,seq)`（去重）已分别满足两类需求，且**已被 BT-1/HD-3 的协议契约冻结**。若未来确需"业务可读 ID"，只能**追加列**（不动主键/不动去重键），并单独立测试矩阵。

**要求（写入计划，不实施）**：任何变更必须先提供"并发插入 / 同毫秒多行 / 时钟回拨 / 批量重试 / 历史数据兼容"五类用例矩阵。

### 8.3 重复错误日志降噪设计（仅设计）

**现状成因（实测）**：`HX711 未接 + weight.enable=true` ⇒ 每 5 s 一条 `WEIGHT_ERROR` ⇒ ≈ 720 条/小时 ⇒ 触发 850 批/小时的上传 + 每条 ≈3.24 写。
（用户已**临时关闭 weight 模块**，但通用机制仍需设计。）

**设计（渐进降频，保信息不丢）**
| 环节 | 规则 |
|---|---|
| 首次 | **立即记录**（不改） |
| 重复 | 同 `(事件码, 原因)` 连续重复 ⇒ 第 2 次起按 1,2,4,8,… 分钟倍增，**上限 60 min** |
| 状态变化 | 错误码/参数变化 ⇒ **重置窗口并立即记录**（不同错误互不影响，按 code 分桶） |
| 恢复 | **必发**恢复事件（即使持续期被压制），携带"期间被压制条数"摘要 |
| 重启 | 窗口状态**内存态清零**，但首条必记录；`boot_seq` 变化视为窗口重置 |
| 不丢信息 | 压制只减"重复样本"，保留 首条 + 摘要 + 恢复 ⇒ 诊断能力不降 |

**两种措施必须分开**（成本/适用面不同）
| 措施 | 位置 | 效果 | 代价 |
|---|---|---|---|
| 配置关闭模块（`weight.enable=false`） | 设备配置 | 立即 −100% 该类日志 | 失去称重功能；**不通用** |
| 固件渐进降噪 | 固件 | 通用（任何高频错误均可） | 需改固件 + 烧录 + EventId 语义扩展（**需先确认不新增 EventId**） |

⚠️ **红线**：不得通过"丢弃错误"降写；不得新增 `EventId` / `device_event.type`（架构冻结）。**本轮不烧录、不改配置。**

---

## 9. 旧任务状态与证据差异

| 任务 | 台账/文档状态 | 本轮核对 | 差异/说明 |
|---|---|---|---|
| **G4-R1** | `OPEN`（issues:394） | 同 | `mqtt_messages` 突发漏记，`INSUFFICIENT_EVIDENCE`（EMQX vs Worker 无法区分） |
| **G4-R2** | `OPEN`（issues:395） | 同 | `drop_*` 侧信道未落库；修法需 DDL 或 `system.stats` 上报 |
| **G4-M1** | **部分关闭**（issues:1022/1037） | 同 | 保留约束：`minimum_free` **仅同 boot 可比** |
| **B-1**（deviceIdFromTopic） | `pending`（#102） | 同 | 与 G4-R5 同族 |
| **S-3-O2** | **已裁决（非缺陷）** | 同 | 预算改 15 µs；表述只能"预算内"，**不得写 PASS** |
| **WP-5** | 门禁已解除（G-4 PASS），**待排期** | 同 | 涉及日志协议改动，须后于阶段三 |
| **G-4** | ⚠️ **文档冲突** | **PASS**（`G4-Window-Verdict-2026-10-21.md:63`） | `docs/issues/未修复的问题.md` 第 390/1211/1243 行仍写 `BLOCKED` —— 属**未回填的旧表述**，非状态反复。建议后续回填（本轮不改权威文档） |
| #64 / #106 / #144 / #146 | `in_progress` | 同 | 均为 ACK/WP-A/WP-B 的收尾项；**与本次优化无依赖** |
| 22 项 `pending` | 未动 | 未动 | **本轮未关闭、未降级、未合并任何任务** |

**依赖关系（新增）**：阶段三 P3-1（watermark 降频）**依赖** 阶段 A/B 的写配额基线；阶段 D（MQTT 客户端）**依赖** G4-R1/R2 的取证结论（否则无法证明"直连后仍可审计"）。

---

## 10. 需要用户裁决（本轮不动）

| # | 决策 | 选项 | 我的建议 |
|---|---|---|---|
| **D1** | 0009 是否应用、何时应用 | ①先量体再定 ②直接应用 ③暂不应用 | **①量体**（≈4 行读），据 `MAX(id)` 决定；若 log > 5 万行则走 D2 |
| **D2** | 若表很大：如何获得写配额 | ①临时升 Paid（$5/月）②只建小表索引 ③缩短保留期（删数据） | **①**（可退回 Free；不删数据） |
| **D3** | 部署时序 | ①代码先、索引后 ②同批 | **①**（代码 −22% 零风险，索引待量体） |
| **D4** | `counts` 破坏性变更是否接受 | ①接受（`null`+`counts_included`）②保持默认带 counts | **①**（唯一调用方已适配） |
| **D5** | 写验收口径 | ①指数合计（索引 +13% / 合并 −23%）②各自独立验收 | **①**，否则 Rev.2 的"≤+5%"永不成立 |
| **D6** | metrics 端点 | ①保留但默认关 ②加近似参数 ③下线 | **①**（运维需要，但 UI 不再调用） |
| **D7** | watermark 处置 | A 保留 / **B 状态变化才写** / D 关开关 / E 删除 | **B**；D 仅作应急闸门 |
| **D8** | 是否修 21 项既有测试失败 | ①单独立项 ②纳入阶段二 | **①**（与本优化无关，混入会污染验收） |
| **D9** | 是否授权 3 项低成本生产核实（`PRAGMA index_list`、4×`MAX(id)`、1 次受控 INSERT 读 `meta`） | ①授权 ②暂不 | **①**（合计 ≈10 行读 + 2 写，可解释 §3.2 的 +8% 异常） |

---

## 11. 本轮变更文件清单

**代码/测试（本地，未提交）**
- `guo-feeder-api/test/current/read_api.test.mjs` —— 修正 `counts` 契约断言（2 项改新契约 + 3 项新增）

**文档（新增）**
- `docs/review/GuoFeeder-Optimization-Phase1-PreDeploy-Review-2026-10-24.md`（本文件）

**文档（修订）**
- `docs/review/GuoFeeder-Optimization-Plan-Rev2-2026-10-24.md` —— 追加 §12「复核修正」，修正 813 口径 / 3.25 推断 / 84% 收益 / 写验收口径
- `guo-feeder-api/migrations/0009_ui_read_indexes.sql` —— 注释内成本模型改为实测值 + **新增 DDL 建索引成本警告**

**未改动（明确声明）**：生产 D1、Worker/Pages 部署、EMQX 认证/ACL、固件、迁移 0001–0008、任何去重键、日志 ACK/重试/补传/rewind 语义。

---

## 12. 本轮证据等级总表

| 结论 | 等级 |
|---|---|
| 两套测试的 PASS/FAIL 数 | **本地实测**（本轮跑） |
| 21 项失败为既有 | **本地实测**（纯净 worktree 对照） |
| 10-23 读/写/查询数与逐 SQL 明细 | **官方测量**（GraphQL） |
| 每语句 wr/exec 倍率 | 官方测量 ÷ 官方执行次数（**派生**） |
| 「1 表 + k 索引」模型 | **官方文档 + 本地三处校验吻合**；`log_history` +8% **未解释** |
| 索引 seek 后行数（20~60） | **估算**（本地无法等价测量） |
| 读降幅 −93.9%~−94.1% | **估算** |
| 写增量 +11.7k（+13.2%） | **估算**（官方"每索引 ≥+1"为下界） |
| 建索引消耗 ≈ 表行数 | **官方定义 + 推理**；**规模化后果未验证** |
| 生产 `EXPLAIN` 会用到新索引 | ❌ **未验证** |
| watermark 无外部读取方 | **代码确证**（全仓 grep） |
| watermark 生产开启 | **代码确证**（`wrangler.toml:40`）+ 官方写量吻合 |

---

**STOP：本轮到此结束，未部署、未应用迁移、未做任何线上变更。等用户审阅与明确授权。**
