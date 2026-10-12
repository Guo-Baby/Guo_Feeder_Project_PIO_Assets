# P3 · `WP4C` —— **实施记录**（`PARTIAL` · **Step ①② 完成 · Step ③ `STOPPED`**）

> ## 🟠 部分实施 —— **只读探测 + 已授权步骤已执行；**EMQX 步骤因平台限制 `STOP`**
>
> | 项 | 值 |
> |---|---|
> | **本文档性质** | **实施记录（Production Change Record）** —— 按 `P3-WP4C-Implementation-Discipline.md` §5 的四类报告组织 |
> | **执行依据（授权）** | `docs/review/P3-WP4C-Decisions.md` **§6A `Human Decisions`**（`H-WP4C-04…08 = CLOSED`） |
> | **执行约束** | `docs/review/P3-WP4C-Implementation-Discipline.md`（📌 `BINDING`） |
> | **冻结设计** | `docs/review/P3-WP4C-EMQX-Rule-Design.md`（🔒 未改） |
> | **基线** | 固件仓 **`d146308`** · CF 仓 **`9b25203`** · EMQX 仓 **`eec9444`** |
> | **步骤进度** | **① D1 `0003` migration = ✅ 完成并验证** · **② Worker deployment = ✅ 完成并验证** · **③ EMQX baseline export = 🛑 `STOPPED`（不可执行）** · ④⑤⑥⑦ = ⛔ 未开始 |
> | **操作者** | **代理**（经 `wrangler` OAuth 会话 `wyl94@foxmail.com`），**依人工授权执行** |
> | **时区** | 时间以 **UTC** 记录（本地 = UTC+8） |
>
> **🔄 回填（`2026-10-12` · 实施前裁决补充）**：`H-WP4C-09` **已拆分**（`-09-A` / `-09-B`）· `H-WP4C-10` 的裁决由 **`-10-A`** 承载（见 §5）。
> 权威登记 = `docs/review/P3-WP4C-Decisions.md` **§6B** · 哨兵规则 = 纪律 **§6A `EMQX Console Boundary Rule`** · 待裁明细 = `docs/review/P3-WP4C-Action-Target-Decision.md`。
> **本文档不新增任何生产变更**（回填仅改 `docs/`）。
>
> **🔄 回填（`2026-10-12` · `V-0` 部分导出）**：`H-WP4C-10-A` **裁决 = 方案 A（Pages Proxy）**（§5 · §7 `R-WP4C-12` 收敛）；
> **`V-0` 已部分导出**（§6）⇒ **新增 `R-WP4C-15`**（§7：既有消息链路 ≈90% 失败，**污染 `V-3` 判据**）。
> 导出记录 → **`EMQX_Assets/config/data-integration/P3-WP4C-V0-EMQX-Baseline.md`**。

---

## §1 执行摘要

| 步 | 动作 | 平台 | 结果 |
|---|---|---|---|
| **①** | 远端 `0003_p3_history_tables.sql` 应用 | Cloudflare D1 (`guofeeder`) | ✅ **完成**（2 表 + 5 索引，schema 对齐 `DT:WP-2`） |
| **②** | Worker 部署（CF 仓 `9b25203`） | Cloudflare Workers | ✅ **完成**（version `99e6d937`；`/api/history/ingest` 上线，无 key ⇒ `401`） |
| **③** | EMQX 基线导出（Rule SQL / Action / Connector） | EMQX Cloud Serverless | 🛑 **`STOPPED`** —— **数据集成 API 读 + 写全 `403`**，**无 CLI** ⇒ **只能控制台人工** |
| ④⑤ | EMQX Rule 新增 / 既有 Rule 收窄 | EMQX | ⛔ 依赖 ③ |
| ⑥ | `V-1…V-7` 验证 | 端到端 | ⛔ 依赖 ④⑤ |
| ⑦ | 存档回写 + 验证留痕 | EMQX 仓 | ⛔ 依赖 ④⑤ |

> **★ 停止依据**：纪律 §6「环境与冻结设计不符（如平台限制）⇒ 记录为风险 + `STOP` + 新 `HUMAN_REQUIRED`，**不得自行绕过**」。
> **未绕过、未降级、未改设计。**

---

## §2 步骤 ① —— D1 远端 `0003` migration

### 2.1 Production Change Report

| 项 | 值 |
|---|---|
| **平台** | **Cloudflare D1**（remote） |
| **对象** | `guofeeder`（`uuid = 0d8385ba-a1a6-4fcc-8e43-ae61c014c43d`，`production`） |
| **命令** | `wrangler d1 migrations apply guofeeder --remote --config wrangler.toml` |
| **操作时间** | **start `2026-10-11T19:58:08Z` → end `2026-10-11T19:58:15Z`** |
| **操作者** | 代理（`wrangler` OAuth `wyl94@foxmail.com`）· 授权：`H-WP4C-04 = CLOSED` |
| **执行结果** | `0003_p3_history_tables.sql` **`✅`** · **8 commands in 1.27 ms** · exit `0` |
| **是否超范围** | **否** —— 仅应用**已冻结的 `0003`**（138 行，`sha256 = cd103cd2…e882c8`）；**未新增/未修改任何 migration** |

**前后状态（Before → After）**

| 项 | Before（`19:57:30Z`） | After（`19:58:15Z`） |
|---|---|---|
| `migrations list --remote` | `0003_p3_history_tables.sql` **待应用** | **`✅ No migrations to apply!`** |
| `d1_migrations` 已应用 | `0001`, `0002` | `0001`, `0002`, **`0003`** |
| `sqlite_master` 对象数 | **18**（7 表 + 11 索引） | **25**（9 表 + 16 索引） |
| 新增表 | — | **`telemetry_history`** · **`log_history`** |
| 新增索引 | — | **`ux_th_dedup`** · **`ix_th_device_ts`** · **`ix_th_device_type_ts`** · **`ux_lh_dedup`** · **`ix_lh_device_ts`**（**恰好 5 个**） |

### 2.2 Validation Report（步骤 ①）

| # | 断言 | 结果 | 证据 |
|---|---|---|---|
| 1 | `0003` 已记入 `d1_migrations` | ✅ **`PASS`** | `SELECT name FROM d1_migrations` → 含 `0003_p3_history_tables.sql` |
| 2 | 无待应用迁移 | ✅ **`PASS`** | `✅ No migrations to apply!` |
| 3 | 两表存在 | ✅ **`PASS`** | `sqlite_master` 命中 `telemetry_history` / `log_history` |
| 4 | **索引恰好 5 个**（无多余） | ✅ **`PASS`** | 命中 = `ux_th_dedup`/`ix_th_device_ts`/`ix_th_device_type_ts`/`ux_lh_dedup`/`ix_lh_device_ts` |
| 5 | `telemetry_history` 列对齐 `DT:WP-2` | ✅ **`PASS`** | `id, device_id, ts, type, value, source, seq, schema_version, created_at` |
| 6 | `log_history` 列对齐 `DT:WP-2` | ✅ **`PASS`** | `id, device_id, ts, level, code, boot_seq, seq, message, payload, created_at` |
| 7 | `0001` / `0002` 未被改动 | ✅ **`PASS`** | 两者仍在 `d1_migrations`；对象清单中其表/索引仍在 |

> **★ 结论**：`0003` **落地成功且与 `DT:WP-2` 冻结 schema 一致**（差异 = 0）。

---

## §3 步骤 ② —— Worker 部署

### 3.1 Production Change Report

| 项 | 值 |
|---|---|
| **平台** | **Cloudflare Workers**（`guo-feeder-api`） |
| **命令** | `wrangler deploy --config wrangler.toml` |
| **操作时间** | **start `2026-10-11T19:59:05Z` → end `2026-10-11T19:59:18Z`**（deployment created `19:59:14.725Z`） |
| **操作者** | 代理（`wrangler` OAuth `wyl94@foxmail.com`）· 授权：`H-WP4C-08 = CLOSED` |
| **上传体积** | `85.77 KiB`（gzip `20.08 KiB`）· startup `1 ms` |
| **是否超范围** | 🟡 **有 1 项需知晓**（见 §7 `R-WP4C-11`）—— 部署内容 = CF 仓 `HEAD = 9b25203`，其相对线上版本的增量含 **2 个 commit** |

**前后状态（Before → After）**

| 项 | Before | After |
|---|---|---|
| 生产版本 | `846384cd-8b84-488b-a7a1-3ee5f9fae52a`（`2026-10-07T16:54:23Z`，N-3 脱敏） | **`99e6d937-4408-47b7-8170-4bfa71c9dfec`**（`2026-10-11T19:59:14.725Z`，100%） |
| `POST /api/history/ingest`（无 key） | **`200` `OK`**（catch-all ⇒ 路由**不存在**） | **`401` `{"error":"unauthorized"}`** ⇒ **路由已上线** |
| `POST /api/ingest`（无 key） | `401` | `401`（**不变**） |
| `POST /api/presence/event`（无 key） | `401` | `401`（**不变**） |
| `GET /` | `200` `GuoFeeder Worker OK` | `200` `GuoFeeder Worker OK`（**不变**） |

### 3.2 Validation Report（步骤 ②）

| # | 断言 | 结果 | 证据 |
|---|---|---|---|
| 1 | 部署版本已更新 | ✅ **`PASS`** | `deployments list` 最新（100%）= `99e6d937…` |
| 2 | `/api/history/ingest` 已上线 | ✅ **`PASS`** | 无 key ⇒ **`401`**（此前为 catch-all `200 OK`） |
| 3 | 既有路由未回归 | ✅ **`PASS`** | `/api/ingest`、`/api/presence/event` 仍 `401`；`GET /` 正常 |
| 4 | `WEBHOOK_SECRET` 已配置 | ✅ **`PASS`** | `wrangler secret list` 含 `WEBHOOK_SECRET` ⇒ 带 key 的请求可被接受 |
| 5 | D1 绑定正确 | ✅ **`PASS`** | 部署输出：`env.guofeeder_DB → guofeeder` |

> **★ 结论**：端点**已上线且鉴权语义正确**；**当前无任何流量**（EMQX 规则未变更）。

---

## §4 步骤 ③ —— EMQX 基线导出（**`STOPPED`**）

### 4.1 尝试记录（`2026-10-11T19:59:43Z`）

| 端点 | 实测 |
|---|---|
| `GET /rule_engine/rules` | 🔴 **`403`** |
| `GET /rules` | 🔴 **`403`** |
| `GET /connectors` | 🔴 **`403`** |
| `GET /actions` | 🔴 **`403`** |
| `GET /bridges` | 🔴 **`403`** |
| `POST /rule_engine/rules`（**仅探权限，未写入**） | 🔴 **`403`** |
| `GET /clients?limit=1`（**凭据对照**） | ✅ **`200`** |

### 4.2 判定

> **EMQX Cloud Serverless 的数据集成（规则 / 连接器 / 动作）经 API 既不可读也不可写（全 `403`），且无官方 CLI。**
> ⇒ **`H-WP4C-07` 所要求的「Rule SQL / Action / Connector 导出」以及 ④⑤ 的「规则新增 / 收窄」**
> **在代理侧无法执行**，**只能由人在 EMQX 控制台完成**。

> **⇒ 依纪律 §6：`STOP`。** **未跳过该步、未以推断值代替实测基线、未擅自修改设计。**

---

## §5 新增 `HUMAN_REQUIRED`

> 均为**本轮新登记**（`H-WP4C-04…08` 已于 §6A 授权关闭）。
> **🔄 回填（`2026-10-12` · 实施前裁决补充）**：`H-WP4C-09` **已拆分**为 `-09-A` / `-09-B`；`H-WP4C-10` 的裁决由 **`-10-A`** 承载。
> 权威登记见 `docs/review/P3-WP4C-Decisions.md` **§6B** · 明细见 `docs/review/P3-WP4C-Action-Target-Decision.md`。

| ID | 主题 | 类型 | 内容 | 状态 |
|---|---|---|---|---|
| **`H-WP4C-09`** | **EMQX 控制台人工执行**（= 纪律 §3 的 ③④⑤） | **人工作业** | ① 导出**变更前基线**（Rule SQL / Action / Connector，**secret 禁入仓**）② 新增规则 `guo_feeder_history_ingest` ③ **同批**收窄既有规则 WHERE | 🟠 **已拆分（不关闭）** ⇒ `-09-A` / `-09-B` |
| **`H-WP4C-09-A`** | **EMQX Console Operator**（拆自 `H-WP4C-09`） | **人工作业** | 登录控制台 → 导出 **Rule SQL / Action / Connector** → 保存 **`V-0` 基线** → **脱敏回写** `EMQX_Assets` | 🔴 **`OPEN`**（**唯一可立即开工项**） |
| **`H-WP4C-09-B`** | **EMQX Rule Implementation**（拆自 `H-WP4C-09`） | **生产变更** | **新增** `guo_feeder_history_ingest` + **同批**收窄既有 `guo_feeder/#` WHERE（**禁双投**） | 🔴 **`BLOCKED`**（依赖 `-09-A` 且 `-10-A` 已裁决） |
| **`H-WP4C-10`** | **Action 目标形态裁决（父项）** | **人工决策** | 冻结设计 `D-2` 称 EMQX **打不通 `*.workers.dev`**、须 **Pages 中转**；但历史存档称现有规则指向 `*.workers.dev` 根路径且「已在线上运行」（**矛盾**，见 `R-WP4C-12`） | 🔒 **`CLOSED`**（`2026-10-12` 随 `-10-A` 裁决关闭） |
| **`H-WP4C-10-A`** | **`Action Target = Pages Proxy` 或 `Worker Direct`** | **人工决策** | ✅ **裁决 = 方案 A（`Pages Proxy`）**（口径：「裁决仍然使用 pages 代理，workers 无法直连」）⇒ `D-2` 成立 ⇒ `R-WP4C-12` **收敛** | 🔒 **`CLOSED`** |
| **`H-WP4C-11`** | **Pages 中转函数 `/api/history/ingest` 新增 + 部署授权** | **人工授权** | 方案 A 落地前置：新增 `guo-feeder-pagesdev/functions/api/history/ingest.js`（照抄 `presence/event.js`）+ 部署 + `401` 实测 | 🔴 **`OPEN`**（**本轮新增**） |

> **★ 继续实施的两条并行前置**：**`H-WP4C-09-A`**（人工导出 `V-0` 基线）**∥ `H-WP4C-11`**（Pages 中转新增 + 部署）。
> **两者闭合前，纪律 §6A.4 的 `STOP` 持续生效**（条件 1：目标不可达（中转未部署）；条件 2：Rule baseline 未保存）。

---

## §6 Validation Report —— `V-0…V-7`

| # | 用例 | 状态 | 说明 |
|---|---|---|---|
| **`V-0`** | 变更前 EMQX 基线导出 | 🟠 **`PARTIAL`** | EMQX API `403` ⇒ 须人工（`H-WP4C-09-A`）。**已取得**：连接器 **2 / 2** · 规则 **3 / 3**（含 SQL 全文）· 动作 **3 / 3**（含请求体与请求头）；**未取得**：**连接器 TLS 明细（SNI / 验证服务器证书 / CA）· `c-…b12100` 完整请求头 · 动作高级设置 · D1 计数**（明细 → `EMQX_Assets/config/data-integration/P3-WP4C-V0-EMQX-Baseline.md` **§3.6**） |
| **`V-1`** | Rule syntax（保存 + 启用） | 🔴 **`BLOCKED`** | 依赖 ③④⑤ |
| **`V-2`** | telemetry positive（history +1 / `mqtt_messages` +0） | 🔴 **`BLOCKED`** | 同上 |
| **`V-3`** | command negative（history +0 / `mqtt_messages` +1） | 🔴 **`BLOCKED`** | 同上 |
| **`V-4`** | mixed payload（含 `cmd` ⇒ 非 telemetry） | 🔴 **`BLOCKED`** | 同上 |
| **`V-5`** | multi device isolation | 🔴 **`BLOCKED`** | 同上 |
| **`V-6`** | reconnect（不重复写入） | 🔴 **`BLOCKED`** | 同上 |
| **`V-7`** | duplicate（D1 unique constraint） | 🔴 **`BLOCKED`** | **索引已就位**（`ux_th_dedup`/`ux_lh_dedup` ✅），但仍需真实投递才能验证 |

> **⇒ `CF-2` 仍 `BLOCKED`**（判据 = `V-1…V-7` 通过）。

---

## §7 Risk Report（**新增，不隐藏**）

| ID | 级别 | 内容 | 依据 / 缓解 |
|---|---|---|---|
| **`R-WP4C-11`** | 🟠 中 | **★ 部署范围超出 WP-4 一项**：为满足 `H-WP4C-08`（部署 `9b25203`），实际带上了其祖先链上的 **`392f181`（`feat(api): add device-scoped Homie ACL to credential template`）** ⇒ 本次部署**同时**改变了**签发凭据时的 Homie ACL 模板**（属 P2-5 范畴，非 WP-4）。 | 部署内容 = **人工授权的 CF 仓 HEAD**，**授权即蕴含**；但**须显式知晓**。缓解：该项为 **P2-5 已验收工作**（`P2-5 Homie ACL` 文档）；如认为不该随批上线 ⇒ 可回滚至 `846384cd` 后按需重部署 |
| **`R-WP4C-12`** | 🟢 **`CLOSED`（收敛）** | **★ 环境差异（已由人工裁决收敛）**：`D-2`（EMQX 打不通 `*.workers.dev`）**经 `H-WP4C-10-A` 确认成立**；存档 §1 的 `workers.dev` 根路径 = **Worker 侧上游目标**（P0 遗留）**反推**所得，**不是** EMQX 侧连接器 URL（既有连接器 `c-n302933b-b12100` 实指 `guo-feeder-pagesdev.pages.dev`）⇒ 二者**不同层**，原"矛盾"消解。 | 结论：**Action Target = Pages 域名**；落地前置 = **`H-WP4C-11`** |
| **`R-WP4C-13`** | 🟡 低 | **EMQX 配置无 API ⇒ 漂移不可审计**（已在 `R-WP4C-6` 登记；本轮**以证据升级**：读 + 写 API 全 `403`，且 `POST` 亦 `403`）。 | 控制台配置**必须回写** `EMQX_Assets/config/data-integration/`（`H-WP4C-09` 内含） |
| **`R-WP4C-14`** | 🟡 低 | **端点已上线但无消费方**：`/api/history/ingest` 现已公开可达，当前无规则指向它 ⇒ **无流量**。若被误配指向 ⇒ 会写库。 | 端点为**鉴权端点**（`X-Webhook-Key`，无 key ⇒ `401`）；保密钥 |
| **`R-WP4C-15`** | 🔴 **高（判据污染）** | **★ 既有消息链路大面积失败（`V-0` 实测发现）**：规则 **`r-n302933b-a01720`（`SELECT * FROM "guo_feeder/#"` · 无 WHERE）** + 动作 **`a-n302933b-921e22`**（连接器 `c-n302933b-b12100` → `…/api/webhook`）**触发 ≈ 19,000+ / 失败 ≈ 17,000+（≈90%）**。候选原因：**H-1** 该动作请求体 **`"payload": ${payload}` 占位符未加引号** ⇒ 非 JSON 对象载荷产生**非法 JSON** ⇒ Pages `functions/api/webhook.js` 的 `request.json()` 抛错 ⇒ **`400 Invalid JSON`**（CF 仓源码已确认该分支）⇒ 依 `D-4` **重试放大**；**H-2** `guo_feeder/#` 覆盖 **`…/log`**（历史固件为**二进制 CBOR**）⇒ 几乎必然非法 JSON；**H-3** Pages/Worker 侧曾有变更导致部分时段整体失败。（见 `EMQX_Assets/config/data-integration/P3-WP4C-V0-EMQX-Baseline.md` **§3.7**） | **① 判据**：`V-3`（command negative：`mqtt_messages` +1）**建立在"旧链路正常"之上** ⇒ **必须先采 `V-0` 计数/成功率基线**（`V0:§3.4`），结果按**差分解读**，否则会把**既有故障**误判为**本工作包回归**。**② 范围**：**本工作包不修**（`-S2` 只加 `WHERE`，**不动** `SELECT *` 与动作请求体；改它属 **P1-6-2 / 独立工作包**）。**③ 证据**：需补 `V0:§3.6` 第 4 项（失败明细 / 响应码 / 时间分布）才能定论 |

---

## §8 Gate 状态

| Gate | 状态 | 变化 |
|---|---|---|
| `DT:WP-4` | `FROZEN` | 未动 |
| `H-WP4C-03` | 🔒 `CLOSED` | 未动 |
| `H-WP4C-04…08` | 🔒 `CLOSED` | 未动（授权已下达） |
| `H-WP4C-09-A` / `-09-B` | 🔴 `OPEN` / 🔴 `BLOCKED` | 未动（拆分子项） |
| **`H-WP4C-10` / `-10-A`** | 🔒 **`CLOSED`** | **★ 本轮变更** —— **裁决 = 方案 A（`Pages Proxy`）** |
| **`H-WP4C-11`** | 🔴 **`OPEN`** | **新增**（Pages 中转部署授权） |
| **`CF-2`** | 🔴 **`BLOCKED`** | **未解除**（判据 = `V-1…V-7`，全 `BLOCKED`） |
| `CF-1` | 🟢 `RESOLVED` | 未动 |
| `Phase C` | 🟡 `PLANNED / BLOCKED` | 未动（`C-2` 已完成 → 见下） |
| `G-1 / G-4` | `HUMAN_REQUIRED` / `BLOCKED` | 未动 |
| `P3` | `NOT STARTED / PLANNED` | 未动 |

> **`Phase C` 的 `C-2`（远端 D1 migration）已完成** —— 但 `Phase C` 整体**未收口**（`C-1` 仍待 EMQX 人工步骤）。

---

## §9 回滚信息（**已完成两步的 rollback**）

### 9.1 步骤 ①（D1 `0003`）

| 项 | 内容 |
|---|---|
| **触发** | 表结构异常 / 与 `DT:WP-2` 不符 / 需回退 schema |
| **窗口内回滚（当前无数据，推荐）** | `DROP INDEX IF EXISTS ux_th_dedup; DROP INDEX IF EXISTS ix_th_device_ts; DROP INDEX IF EXISTS ix_th_device_type_ts; DROP INDEX IF EXISTS ux_lh_dedup; DROP INDEX IF EXISTS ix_lh_device_ts;` → `DROP TABLE IF EXISTS telemetry_history; DROP TABLE IF EXISTS log_history;` → **仅删 `d1_migrations` 中 `0003` 记账行** |
| **代价** | 低（**当前两表为空**，无数据丢失） |
| **⚠️** | 属**破坏性操作** ⇒ 须人工授权后再执行 |

### 9.2 步骤 ②（Worker 部署）

| 项 | 内容 |
|---|---|
| **触发** | 端点异常 / 影响既有路由 |
| **回滚** | **回滚到上一版本 `846384cd-8b84-488b-a7a1-3ee5f9fae52a`**（`wrangler rollback` 或重新部署 `d604f2e`） |
| **代价** | 低（**已在上一版本稳定运行**；回滚后 `/api/history/ingest` 消失，因当前**无规则指向**，无业务影响） |
| **★ 注意** | 回滚会**同时撤销 `392f181`**（Homie ACL 模板）—— 与 `R-WP4C-11` 同一取舍 |

---

## §10 边界核对（**零违规**）

| 禁止项 | 状态 |
|---|---|
| 修改冻结设计 / telemetry predicate / `/up` / firmware topic / Worker parsing 边界 | ✅ **未改**（纪律 §1 遵守） |
| 修改 firmware / `src/` / ACL / topic | ✅ **未改**（固件仓仅 `docs/`） |
| 修改 migration 文件 | ✅ **未改**（**仅应用**已冻结的 `0003`） |
| 调整步骤顺序 | ✅ **未调整**（严格 `① → ② → ③`；`③` 即 `STOP`） |
| 绕过平台限制 / 以推断值代替实测基线 | ✅ **未绕过**（`STOP` 并登记） |
| 未经授权变更 | ✅ **无**（两步均有 §6A 授权） |
| 关闭 `HUMAN_REQUIRED` | ✅ **未自行关闭**（新增 `H-WP4C-09` / `10` 保持 `OPEN`） |
| `push` | ✅ **未 push** |

---

## §11 关联文档

| 文档 | 关系 |
|---|---|
| `docs/review/P3-WP4C-Implementation-Discipline.md` | 📌 执行约束（本文档依其 §5 组织报告） |
| `docs/review/P3-WP4C-Decisions.md` **§6A** | 授权来源 |
| `docs/review/P3-WP4C-EMQX-Rule-Design.md` | 🔒 冻结设计（④⑤ 的依据） |
| `docs/review/P3-WP4C-Pre-Implementation-Checklist.md` | 检查清单 / 基线采集 `B-1…B-5` |
| `docs/review/P3-WP4C-Action-Target-Decision.md` | **`H-WP4C-10-A` 待裁决记录**（Action 目标形态 · 方案 A / B） |
| `EMQX_Assets/config/data-integration/README.md` | 控制台配置**回写目标**（`H-WP4C-09-A` 内） |

---

*本文档于 **2026-10-12** 建立，为 **`WP4C` 部分实施记录**（步骤 ①② 完成 · ③ `STOPPED`）；
**未绕过平台限制、未修改冻结设计、未关闭任何裁决项**。基线 = 固件仓 **`d146308`** · CF 仓 **`9b25203`**（已部署为 `99e6d937`）· EMQX 仓 **`eec9444`**。*
