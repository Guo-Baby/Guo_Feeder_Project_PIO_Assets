# P3 · DT:WP-4 —— History Ingestion **实施完成记录**（`IMPLEMENTATION COMPLETION RECORD`）

> ## ✅ `WP-4 = IMPLEMENTED`（**实施已完成并提交**）· 本文档 = **记录**，非设计、非计划
>
> - 本文档**只记录既有事实**：**不改** `DT:WP-4` 冻结设计 · **不改** `DT:WP-1/2/3` 冻结文档 · **不改** 代码 · **不建 migration** · **不改 EMQX** · **不执行部署**。
> - **`DT:WP-4 = FROZEN` 状态保持不变** —— 冻结设计**仍是唯一实施基线**（本文档不覆盖它）。
> - ⚠️ **`WP-4 = IMPLEMENTED` ≠ "已上线"** —— **EMQX Action 切换**与**远端 D1 migration** **均未执行**（见 §4 / Phase C 计划）。

| 项 | 值 |
|---|---|
| **状态** | **`WP-4 = IMPLEMENTED`**（代码 + migration + 测试已提交）· **未部署** |
| **设计冻结 commit** | **`2097c4e`**（固件仓 · `docs(p3-wp4): freeze history ingestion design r5`）· **`DT:WP-4 = FROZEN`** |
| **实施 commit** | **`9b25203`**（Cloudflare 仓 · `feat(p3-wp4): implement history ingestion persistence`） |
| **记录时点** | **2026-10-11** |
| **Gate 影响** | **无** —— `G-1 HUMAN_REQUIRED` · `G-2 PASS` · `G-3 PASS` · `G-4 BLOCKED` · `G-5 PASS` **均未变** · `P3 = NOT STARTED / PLANNED` |
| **push / 部署** | **均未执行**（`push` 仍为 `DEFERRED`） |

---

## §1 基线

| # | 项 | 值 | 说明 |
|---|---|---|---|
| 1 | **DT:WP-4 冻结 commit** | **`2097c4e`** | 固件仓 · `docs(p3-wp4): freeze history ingestion design r5`（4 文件 +1372/−1）· 冻结时 `H-WP4` 17 项 = `CLOSED` 15 / `OPEN` 2 |
| 2 | **implementation commit** | **`9b25203`** | Cloudflare 仓 · `feat(p3-wp4): implement history ingestion persistence`（9 文件 +1481/−37） |
| 3 | **基线前后快照** | 固件仓 `2097c4e`（clean）· CF 仓 `9b25203`（clean）· EMQX 仓 `eec9444`（clean） | 三仓均 clean；**均未 push** |
| 4 | **`DT:WP-2`（schema）** | `P3-WP2-History-Data-Model-Design.md` · **`FROZEN`** · `G-3 = PASS` | migration 的**唯一 schema 依据** |
| 5 | **`DT:WP-3`（存储策略）** | `P3-WP3-History-Storage-Policy-Design.md` · **`FROZEN`** | retention **90 / 180 天** · cleanup = `scheduled cleanup job` · **`no additional index`** |

> **★ 提交顺序铁律（已遵守）**：Phase A 设计冻结（`2097c4e`，固件仓）**先于** Phase B 实施（`9b25203`，CF 仓）；
> 两者**分属不同仓、分开提交**（代码与文档分离）。

---

## §2 实施范围（5 项，全部落地）

| # | 交付物 | 位置 | 说明 |
|---|---|---|---|
| 1 | **`POST /api/history/ingest`** | `Cloudflare_Assets/guo-feeder-api/src/index.js` | **新增专用端点**（`H-WP4-14` = `CLOSED` · 选择 B）· **早退路由**（置于旧 `/api/ingest` 之前）· **不写 `mqtt_messages`** |
| 2 | **`history-ingest.js`**（522 行） | `.../src/history-ingest.js` | 端点全部业务：鉴权 → 信封解析 → topic→域（`up`/`log`）→ identity（**只信 topic**）→ `ID-6` 一致性 → REVOKED 守卫 → **逐条校验** → `INSERT OR IGNORE` 批量 → 三态响应 |
| 3 | **`http-util.js`**（61 行） | `.../src/http-util.js` | `jsonResponse()` / `requireWebhookKey()` 由 `index.js` **抽出共享**（供 3 个入口共用**同一份**鉴权语义）· **语义逐字不变** |
| 4 | **`0003_p3_history_tables.sql`**（138 行） | `.../guo-feeder-api/migrations/0003_p3_history_tables.sql` | **`DT:WP-2` 冻结 schema 逐字落地**：2 表 + **恰好 5 索引** · **与冻结定义差异 = 0** |
| 5 | **real D1 verification** | `.../guo-feeder-api/test/real_d1/`（`history_ingest_real_d1.sh` + `README.md`） | **真实 D1 引擎**端到端验证（受跟踪资产，一条命令可复现） |

**辅助改动（1 项）**：`test/current/history_ingest.test.mjs`（逻辑层套件）+ `test/run_all.mjs`（注册 1 行）+ `migrations/README.md`（增 §8 登记）。

### 2.1 `0003_*` 与 `DT:WP-2` 的**差异 = 0**（逐项核对）

| 冻结定义（`WP2:§3.1` / `§4.2` / `§5`） | migration 实际 | 差异 |
|---|---|---|
| `telemetry_history`：`id, device_id, ts, type, value, source, seq, schema_version, created_at` | 同名同序同类型 | **0** |
| `log_history`：`id, device_id, ts, level, code, boot_seq, seq, message, payload, created_at` | 同名同序同类型 | **0** |
| 幂等唯一键 = telemetry `(device_id,type,ts,seq)` / log `(device_id,boot_seq,seq)` | `ux_th_dedup` / `ux_lh_dedup` | **0** |
| 查询索引 = `(device_id,ts DESC)` × 2 + `(device_id,type,ts DESC)` | `ix_th_device_ts` / `ix_th_device_type_ts` / `ix_lh_device_ts` | **0** |
| 冻结**未纳入**的索引（`ix_th_type_ts` / `ix_lh_device_level_ts`） | **未创建** | **0** |
| `payload` 物理类型 = `TEXT` | `TEXT`（**未改语义**） | **0** |
| retention / cleanup | **未含任何 DELETE / TTL / 归档** | **0** |
| 既有 `0001_*` / `0002_*` | **0 行改动** | **0** |

> **★ 显式声明**：**未新增字段 · 未新增表 · 未新增索引 · 未加 CHECK / FK / 触发器 · 未改 `payload` 语义 · 未改 retention / cleanup**。
> **外键刻意不声明**：`DT:WP-2` 冻结字段表未列出 FK（且 SQLite / D1 默认不强制）；历史行允许先于 `device` 建档（`DT:WP-4` §2.6 `ID-4`）。

---

## §3 验证结果

### 3.1 ✅ 真实 D1 —— **48 / 48 PASS**

| 项 | 值 |
|---|---|
| 方法 | **真实 D1 引擎**（本地 `wrangler dev --local` → workerd / miniflare 的本地 D1，跑**真实 SQLite**）—— **不用 stub 冒充** |
| 架构 | **server A（已应用 `0001`/`0002`/`0003`）** = 正常路径；**server B（未迁移 · 独立 persist dir）** = 触发**真实 `no such table`** ⇒ 验证 **5xx** 语义 |
| 复现命令 | `bash guo-feeder-api/test/real_d1/history_ingest_real_d1.sh` |
| 结果 | **`PASS = 48` · `FAIL = 0`** |
| 落库终态 | `telemetry_history = 5` 行 · `log_history = 3` 行（基线 0 / 0） |

**覆盖（映射 `DT:WP-4` §7 的 8 项测试要求 + 扩展 7 项）**：

| # | 用例 | 断言 |
|---|---|---|
| 1 | 单条 telemetry（N=1） | 200 · `accepted=1` · 真实落库 · `value` 十进制字符串 · `created_at` 由 D1 生成 |
| 2 | batch telemetry（N=3） | 200 · `accepted=3` · 行数 +3 |
| 3 | 单条 log（信封级 · `payload_base64`） | 200 · 落 `log_history` · **`payload` 原样保存（未解码 / 未改写）** |
| 4 | **duplicate replay** | 同 `(device_id, boot_seq, seq)` ⇒ `accepted=0` / `duplicate=1` / **行数不增** |
| 5 | batch log（N=2） | 200 · `accepted=2` |
| 6 | **malformed envelope**（缺 `topic`） | **400** · `error=missing_topic` · **DB 计数不变** |
| 7 | 记录级非法（缺 `seq`） | 200 · `rejected=1` · **该条不入库**（记录级失败不阻塞整批） |
| 8 | **`schema_version > 1`** | 200 · `accepted=1` · **`schema_version` 原样 = 3**（forward compatibility） |
| 9 | log 信封 `device_id` ≠ topic（`ID-6`） | 400 · `error=device_id_mismatch` |
| 10 | 无 `X-Webhook-Key` | 401 |
| 11 | `source=cloud_api` | 400 · `error=cloud_api_not_allowed` |
| 12 | 未知 topic 族 | 400 · `error=unknown_topic`（**无第二套 topic 映射**） |
| 13 | **★ 5xx retry 场景** | 真实 D1 缺表 ⇒ **503** · `error=persist_failed`（暂时失败 ⇒ 上游重投 ⇒ 幂等吸收） |
| 14 | 边界隔离 | 旧 `/api/ingest` 无 key 仍 401（**未改动**）· 新端点**不写 `mqtt_messages`** |
| 15 | 幂等由 DB 保证 | `ux_th_dedup` / `ux_lh_dedup` 真实存在 |

### 3.2 ✅ 回归 —— **6 / 6 PASS**

| 项 | 结果 |
|---|---|
| `test/run_all.mjs` | **6 / 6 套件 OK**（含 legacy 7 项 `EXPECTED_FAILURE`，属预期）· **`UNEXPECTED_PASS = 0`** · **无回归** |
| 说明 | **helper 抽取（`http-util.js`）未改变既有行为** —— 旧 `/api/ingest` / `/api/presence/event` 语义不变 |

### 3.3 ⚠️ 分层声明（**两类结论不得混用**）

| 层 | 位置 | 结果 | 可标记为 |
|---|---|---|---|
| **真实 D1 层** | `test/real_d1/` | **48/48** | ✅ **PASS（真实 D1 验证）** |
| **逻辑层（护栏）** | `test/current/history_ingest.test.mjs` | 32/32 | ⚠️ 回归护栏 —— **不得**当作数据库验证 |

---

## §4 未完成（**明确列出 · 均未执行**）

| # | 未完成项 | 归属 | 状态 | 依据 |
|---|---|---|---|---|
| 1 | **EMQX HTTP Action switch** | `计划:WP-3`（EMQX 集成） | **`NOT RUN`** | 需在 **EMQX Cloud 控制台**手工操作（API 对 rules/actions/connectors 返回 403）⇒ 见 Phase C §C-1 |
| 2 | **remote D1 migration** | 部署动作 | **`NOT RUN`** | 本轮**仅本地**（`--local`）应用；远端 `--remote` **需人工授权** ⇒ 见 Phase C §C-2 |
| 3 | **`log_ack`（WP-5）** | `计划:WP-5` | **`OUT OF SCOPE`** | 受 **`G-4`（`BT-1`）= `BLOCKED`**；`DT:WP-4` 明确**界外** |
| 4 | **`H-WP4-10`**（`ts` 合理性窗口） | — | **`OPEN`** | `DT:WP-4` §10.1：**明确界外**（属未来策略），可延后 |
| 5 | **`H-WP4-12`**（`log_ack` 归属） | `计划:WP-5` | **`OPEN`** | `DT:WP-4` §10.1：**明确界外** + 受 `G-4`，可延后 |

**另：跨 WP 待办（非 WP-4 范围，仅登记）**
- **`H-WP3-06`**（`payload` **业务**体积上限）：Worker 侧已定（遵守平台 request body 上限 + 超限拒收）；**业务上限仍归 `DT:WP-3`**。
- **设备侧「禁止上传含 secret 的日志内容」义务**（`H-WP4-17` 裁决的责任转移）：**须在固件侧落地**，未见实现。
- **部署顺序**：`0003` 与 `POST /api/history/ingest` **必须同批上线**，且 **migration 先于 Worker**。

---

## §5 边界与纪律（**本轮严格遵守**）

- ✅ **未修改** `src/`（固件）· `platformio.ini` · `partitions.csv` · 固件任何行为。
- ✅ **未修改** `DT:WP-4` 冻结设计文档（**逐字未动**）· 未修改 `DT:WP-1/2/3` 冻结文档 · 未修改 `P3-History-D1-Plan.md` · 未修改 `cloud_protocol.md`。
- ✅ **未修改** `0001_*` / `0002_*`（P-D1）· 未新建 `0004_*`。
- ✅ **未修改** EMQX 配置（EMQX 仓 clean）· **未执行** remote migration · **未部署** · **未 push**。
- ✅ 提交均**显式 `git add`** 全量核对；提交前 blob 完整性 **9/9 一致**。
- ✅ **未改动任何 Gate**（见文首表）。

---

## §6 环境留痕（供后续复现）

| # | 事实 | 影响 |
|---|---|---|
| 1 | `wrangler dev --local` 下 **`--var WEBHOOK_SECRET:...` 优先级高于 `.dev.vars`** | 曾致首轮请求**全部 401**（已定位并修正脚本取值） |
| 2 | `wrangler d1 migrations apply\|execute --local --persist-to <dir>` 的 persist 目录**必须与** `wrangler dev --persist-to` 一致 | 否则「迁移已应用但 server 看不到表」 |
| 3 | 本地 wrangler（`Cloudflare_Assets/node_modules/wrangler` 4.145）**自带 `@cloudflare/workerd-windows-64`** | 真实 D1 验证**可离线完成**，无需登录 / 联网 |
| 4 | 触发「**真实** 5xx」的方法 = **未迁移的第二个 persist dir + 第二台 dev server** | 真实 `no such table`，**不用 stub 冒充** |

---

## §7 结论与下一步

**结论**：`DT:WP-4` 的**实施部分（代码 + migration + 测试）已完成并提交**（`9b25203`），
真实 D1 端到端 **48/48 PASS**、全量回归 **6/6 PASS**；
`0003_*` 与 `DT:WP-2` 冻结 schema **差异 = 0**。

**⇒ 但「上线」尚未发生**：EMQX Action 切换 + 远端 D1 migration 均**未执行** ⇒ 见 **`P3-WP4-PhaseC-Integration-Plan.md`**（**仅规划**）。

**下一步（待人工批准）**：按 Phase C 计划执行 **C-1（EMQX Action 切换）** 与 **C-2（远端 D1 migration）**；
**⚠️ C-1/C-3 存在集成阻塞发现，须先人工裁决**（见 Phase C §5）。

---

> **WP-4 = IMPLEMENTED（implementation committed · NOT deployed · NOT pushed）.**
> **DT:WP-4 = FROZEN（design unchanged · single implementation baseline）.**
> **EMQX Action switch = NOT RUN. Remote D1 migration = NOT RUN.**
> **No Gate changed. P3 remains NOT STARTED / PLANNED.**

---

*本记录于 **2026-10-11** 建立；依据固件仓 `2097c4e`（DT:WP-4 冻结）与 Cloudflare 仓 `9b25203`（实施提交）；为**事实记录**，不含任何设计变更、代码改动或部署动作。*
