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
> **🔄 回填（`2026-10-12` · `V-0` 收口）**：`H-WP4C-10-A` **裁决 = 方案 A（Pages Proxy）**（§5 · §7 `R-WP4C-12` 收敛）；
> **`V-0` = ✅ `CLOSED WITH GUARD`**（§6）⇒ **`H-WP4C-09-A` = ✅ `CLOSED WITH GUARD`**（§5）；
> **新增 `R-WP4C-15`**（既有消息链路 ≈90% 失败 · **污染 `V-3` 判据**）· **`R-WP4C-16`**（连接器 TLS 明细保存后不可回读）。
> 导出记录 → **`EMQX_Assets/config/data-integration/P3-WP4C-V0-EMQX-Baseline.md`**。
> **★ 仍卡在 `H-WP4C-11`（Pages 中转未部署）** ⇒ EMQX 侧变更**继续 `STOP`**（纪律 §6A.4 条件 1）。
>
> **🔄 回填（`2026-10-12` · `H-WP4C-11` 部署与验证 —— 本工作包首个生产变更）**：人工明示授权后
> **新增并部署** Pages 中转函数 `guo-feeder-pagesdev/functions/api/history/ingest.js`
> （Pages 新部署 = **`61b4938e-fa2e-42da-b950-f527781f79f9`**；回滚目标 = `5790b3c5-…`）⇒
> **端到端验证 `PASS 15 / FAIL 0`**（A 段 7/7 · B 段 8/8；D1 落库 `telemetry ×4 + log ×1`，**已定点清理并复核归零**）⇒
> **`H-WP4C-11` = ✅ `CLOSED`** · **`V-1` = 🟢 `PASS`（带说明）**（§6）· **`H-WP4C-09-B` = 🟡 `READY (R3.1)`**（§5）；
> **纪律 §6A.4 `STOP` 条款已无成立条件**；**核查旧连接器收存** ⇒ **确认收到并存储**（`mqtt_messages` **2458** 行且仍在增长）。
> 记录 → **`docs/review/P3-WP4C-History-Relay-Deployment-Verification.md`**（新增 **`R-WP4C-17`**）。
>
> **🔄 回填（`2026-10-12` · 人工施工中 · `R3.1` SQL 修正）**：人工已**拆除 3 个旧对象**（`r-…a01720` / `a-…921e22` / `c-…b12100`）
> 并**新建连接器 `CB` = `c-n302933b-0c1cb0`**（URL 裸主机 · SNI 已填 · 4 头，**配置逐项正确**）；
> 但**新建规则 A 时保存失败** ⇒ 控制台报 **`11: syntax error before: '='`** ⇒
> **根因 = 我方的 SQL 错误**：① **`==` 不存在**（EMQX SQL 相等只有单 `=`）② "键存在"应以官方 **`is_null` / `is_not_null`** 表达。
> ⇒ **`R3.1` 修正**（**仅改写法，语义不变**）· **新增 `R-WP4C-20`**（§7）· **新增强制验证步**（规则页调试试跑）。
> 施工依据 → **`P3-WP4C-Console-Task-Sheet.md`**（照抄版）+ `…-Console-Build-Guide-R3.md`。

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
| **`H-WP4C-09-A`** | **EMQX Console Operator**（拆自 `H-WP4C-09`） | **人工作业** | 登录控制台 → 导出 **Rule SQL / Action / Connector** → 保存 **`V-0` 基线** → **脱敏回写** `EMQX_Assets` | ✅ **`CLOSED WITH GUARD`**（`2026-10-12` —— **连接器 2/2 · 规则 3/3 含 SQL 全文 · 动作 3/3 含高级设置 · D1 计数已采 · 回滚已签署**；**guard = TLS 明细不可回读 ⇒ 新建连接器保存前截图 + 保存后实测**，`R-WP4C-16`） |
| **`H-WP4C-09-B`** | **EMQX Rule Implementation**（拆自 `H-WP4C-09`） | **生产变更** | **`R3`：删除** `r-…a01720` → `a-…921e22` → `c-…b12100` ⇒ **新建** 连接器 `CB`（裸主机 base）+ 动作 `MF`（`/api/webhook`）/`HI`（`/api/history/ingest`）+ 规则 `guo_feeder_message_ingest`（**取反谓词**）/`guo_feeder_history_ingest`（**冻结谓词**）；**presence 链路零接触** | 🟡 **`READY (R3.1)`** —— **执行进度**：⏹ 拆除 3 个旧对象 ✅ 已完成；✅ 连接器 **`c-n302933b-0c1cb0`** 已建；🛑 规则 SQL 曾报 **`syntax error before: '='`** ⇒ **`R3.1` 已修正谓词写法（`is_null`/`is_not_null`）**；**仍须人工控制台执行**（代理无控制台 / API 权限）。施工依据 → **`P3-WP4C-Console-Task-Sheet.md`** + `…-Console-Build-Guide-R3.md` |
| **`H-WP4C-10`** | **Action 目标形态裁决（父项）** | **人工决策** | 冻结设计 `D-2` 称 EMQX **打不通 `*.workers.dev`**、须 **Pages 中转**；但历史存档称现有规则指向 `*.workers.dev` 根路径且「已在线上运行」（**矛盾**，见 `R-WP4C-12`） | 🔒 **`CLOSED`**（`2026-10-12` 随 `-10-A` 裁决关闭） |
| **`H-WP4C-10-A`** | **`Action Target = Pages Proxy` 或 `Worker Direct`** | **人工决策** | ✅ **裁决 = 方案 A（`Pages Proxy`）**（口径：「裁决仍然使用 pages 代理，workers 无法直连」）⇒ `D-2` 成立 ⇒ `R-WP4C-12` **收敛** | 🔒 **`CLOSED`** |
| **`H-WP4C-11`** | **Pages 中转函数 `/api/history/ingest` 新增 + 部署** | **人工授权 · 已执行** | 方案 A 落地前置：新增 `guo-feeder-pagesdev/functions/api/history/ingest.js`（照抄 `presence/event.js` + **`X-Webhook-Key` 透传**）+ 部署 + 实测 | ✅ **`CLOSED`**（`2026-10-12` —— 已部署 `61b4938e-…` · 端到端 **`PASS 15 / FAIL 0`** · 冒烟脚本 `scripts/smoke-history-relay.mjs` 可复现） |

> **★ 原「两条并行前置」均已闭合**：✅ **`H-WP4C-09-A`**（`V-0` 基线，`CLOSED WITH GUARD`）**∥** ✅ **`H-WP4C-11`**（Pages 中转，`CLOSED`）。
> **⇒ 当前无纪律层面的 `STOP`**；剩余阻塞 = **人工执行能力**（`H-WP4C-09-B` 须在 EMQX 控制台操作，代理无控制台 / API 权限）。

---

## §6 Validation Report —— `V-0…V-7`

| # | 用例 | 状态 | 说明 |
|---|---|---|---|
| **`V-0`** | 变更前 EMQX 基线导出 | ✅ **`CLOSED WITH GUARD`** | **已完成**（人工控制台只读导出 + 只读 D1 计数）：连接器 **2/2** · 规则 **3/3（SQL 全文）** · 动作 **3/3（含高级设置）** · **D1 基线**（`th=0` / `lh=0` / **`mm=2458`**，近 24 h **13**）· **回滚已签署**；**guard** = TLS 明细**平台不可回读** ⇒ 新建连接器**保存前截图 + 保存后实测**（`R-WP4C-16`）。明细 → `EMQX_Assets/config/data-integration/P3-WP4C-V0-EMQX-Baseline.md` |
| **`V-1`** | Rule syntax（保存 + 启用） | 🔴 **`BLOCKED`** | 依赖 ③④⑤ |
| **`V-2`** | telemetry positive（history +1 / `mqtt_messages` +0） | 🔴 **`BLOCKED`** | 同上 |
| **`V-3`** | command negative（history +0 / `mqtt_messages` +1） | 🔴 **`BLOCKED`** | 同上 |
| **`V-4`** | mixed payload（含 `cmd` ⇒ 非 telemetry） | 🔴 **`BLOCKED`** | 同上 |
| **`V-5`** | multi device isolation | 🔴 **`BLOCKED`** | 同上 |
| **`V-6`** | reconnect（不重复写入） | 🔴 **`BLOCKED`** | 同上 |
| **`V-7`** | duplicate（D1 unique constraint） | 🔴 **`BLOCKED`** | **索引已就位**（`ux_th_dedup`/`ux_lh_dedup` ✅），但仍需真实投递才能验证 |

> **⇒ `CF-2` 仍 `BLOCKED`**（判据 = `V-1…V-7` 通过）。
>
> **★ 命名空间警示（防误读）**：上表的 **`V-1…V-7` = 冻结验证集**（`P3-WP4C-EMQX-Rule-Design.md`），
> **不同于**纪律 **§6A.3 的准入序列 `V-0…V-5`**（控制台变更准入流程）。两者**编号重叠但语义不同**。
>
> **★ 准入序列（纪律 §6A.3）现状**（`2026-10-12` · `H-WP4C-11` 部署后）：
> **准入 `V-0`（导出基线）= ✅ 已完成**（`CLOSED WITH GUARD`）·
> **准入 `V-1`（确认目标 Action 可达）= 🟢 `PASS`（带说明）** —— **证据**：既存 presence 连接器 `c-n302933b-291200` 已在生产**持续投递**到**同一 host** `https://guo-feeder-pagesdev.pages.dev`（`device_event` 近 7 d **114** 次）⇒ **EMQX → `*.pages.dev` 可达性 + TLS 链路已被现场证实**；新端点 `GET/POST /api/history/ingest` 已在线且行为正确（中转层 7/7 `PASS`）。
> **尚须补**：**新连接器本身**的可达性须在 **准入 `V-2` 保存后实测**（`R-WP4C-16` guard）。
> 明细 → **`docs/review/P3-WP4C-History-Relay-Deployment-Verification.md`** §7。

---

## §7 Risk Report（**新增，不隐藏**）

| ID | 级别 | 内容 | 依据 / 缓解 |
|---|---|---|---|
| **`R-WP4C-11`** | 🟠 中 | **★ 部署范围超出 WP-4 一项**：为满足 `H-WP4C-08`（部署 `9b25203`），实际带上了其祖先链上的 **`392f181`（`feat(api): add device-scoped Homie ACL to credential template`）** ⇒ 本次部署**同时**改变了**签发凭据时的 Homie ACL 模板**（属 P2-5 范畴，非 WP-4）。 | 部署内容 = **人工授权的 CF 仓 HEAD**，**授权即蕴含**；但**须显式知晓**。缓解：该项为 **P2-5 已验收工作**（`P2-5 Homie ACL` 文档）；如认为不该随批上线 ⇒ 可回滚至 `846384cd` 后按需重部署 |
| **`R-WP4C-12`** | 🟢 **`CLOSED`（收敛）** | **★ 环境差异（已由人工裁决收敛）**：`D-2`（EMQX 打不通 `*.workers.dev`）**经 `H-WP4C-10-A` 确认成立**；存档 §1 的 `workers.dev` 根路径 = **Worker 侧上游目标**（P0 遗留）**反推**所得，**不是** EMQX 侧连接器 URL（既有连接器 `c-n302933b-b12100` 实指 `guo-feeder-pagesdev.pages.dev`）⇒ 二者**不同层**，原"矛盾"消解。 | 结论：**Action Target = Pages 域名**；落地前置 = **`H-WP4C-11`** |
| **`R-WP4C-13`** | 🟡 低 | **EMQX 配置无 API ⇒ 漂移不可审计**（已在 `R-WP4C-6` 登记；本轮**以证据升级**：读 + 写 API 全 `403`，且 `POST` 亦 `403`）。 | 控制台配置**必须回写** `EMQX_Assets/config/data-integration/`（`H-WP4C-09` 内含） |
| **`R-WP4C-14`** | 🟡 低 | **端点已上线但无消费方**：`/api/history/ingest` **现已部署且端到端验证可用**（`H-WP4C-11` = `CLOSED`），但**当前仍无 EMQX 规则指向它** ⇒ **无生产流量**。 | 端点为**鉴权端点**（`X-Webhook-Key`，无 key ⇒ `401`，已实测）· 端点仅接受 `guo_feeder/<12hex>/{up,log}` 形态（`unknown_topic` ⇒ `400`）⇒ 误配写库面受限 · 保密钥 |
| **`R-WP4C-15`** | 🔴 **高（判据污染）** | **★ 既有消息链路大面积失败（`V-0` 实测发现）**：规则 **`r-n302933b-a01720`（`SELECT * FROM "guo_feeder/#"` · 无 WHERE）** + 动作 **`a-n302933b-921e22`**（连接器 `c-n302933b-b12100` → `…/api/webhook`）**触发 ≈ 19,000+ / 失败 ≈ 17,000+（≈90%）**。候选原因：**H-1** 该动作请求体 **`"payload": ${payload}` 占位符未加引号** ⇒ 非 JSON 对象载荷产生**非法 JSON** ⇒ Pages `functions/api/webhook.js` 的 `request.json()` 抛错 ⇒ **`400 Invalid JSON`**（CF 仓源码已确认该分支）⇒ 依 `D-4` **重试放大**；**H-2** `guo_feeder/#` 覆盖 **`…/log`**（历史固件为**二进制 CBOR**）⇒ 几乎必然非法 JSON；**H-3** Pages/Worker 侧曾有变更导致部分时段整体失败。（见 `EMQX_Assets/config/data-integration/P3-WP4C-V0-EMQX-Baseline.md` **§3.7**） | **① 判据**：`V-3`（command negative：`mqtt_messages` +1）**建立在"旧链路正常"之上** ⇒ **必须先采 `V-0` 计数/成功率基线**（`V0:§3.4`），结果按**差分解读**，否则会把**既有故障**误判为**本工作包回归**。**② 范围**：**本工作包不修**（`-S2` 只加 `WHERE`，**不动** `SELECT *` 与动作请求体；改它属 **P1-6-2 / 独立工作包**）。**③ 证据（`2026-10-12` 补充）**：`V-0` **D1 基线交叉印证** —— `mqtt_messages` 总库存仅 **2458** 条（**最新记录 ≈采集前 1 h** ⇒ **链路仍在投递，是低成功率非完全失效**）；近 24 h **13** 条 / 近 7 d **191** 条；动作**最大重试 = 1**（非无限重试）· **请求模式 = 异步 + 缓存 4 MB**。人工提供的失败日志形态 = **资源健康告警**（`resource down` / `alarm_is_activated`，**30 s 节奏**）⇒ **资源 down 期间一切投递记失败**（放大效应；**该片段属 `c-…291200`（presence），消息流侧同类告警待证**）。**仍缺**：`a-n302933b-921e22` / `c-…b12100` 的失败明细（`V0:§3.6` 第 2 项）⇒ 归因未定论。**④ 新证据（`2026-10-12` · `H-WP4C-11` 验证期）**：`mqtt_messages` 按 topic 分布实测 —— **`…/log` 主题 = 0 行**（31 个 topic 中**无一个** `/log`），而 `…/up` **1918** / `…/down` **531** ⇒ **H-2 假设获直接支持**（`/log` 载荷为二进制 CBOR ⇒ Pages `webhook.js` 的 `request.json()` 必失败 ⇒ `400` ⇒ 重试放大）；**旁证**：presence 链路（同 host）**近 7 d 成功 114 次** ⇒ 失败**不是** host/TLS/网络问题。**⑤ 人工裁示（`2026-10-12`）**：*「失败明细暂时不提供……先做测试，看我们的 EMQX 是否足够靠谱，如果 OK，就先忽略这个问题」* ⇒ **本工作包按「暂不追修」处置**，风险保持登记 |
| **`R-WP4C-17`** | 🟡 低 | **★ 幂等冲突仍消耗 `AUTOINCREMENT`**（`H-WP4C-11` 验证期实测）：`INSERT OR IGNORE` 在**唯一索引冲突**（`ux_th_dedup` / `ux_lh_dedup`）时**仍推进 `AUTOINCREMENT` 序列** ⇒ `telemetry_history.id` / `log_history.id` **出现跳号**（实测 `1 → 3`；`B-2` 的重复投递所致）。**不影响行数与语义**。 | **影响面**：仅当未来**按 `id` 增量**做对账 / 游标分页时须注意（应改用 `(device_id, ts, seq)` 等业务键或 `created_at`）· **无需变更**（`W-1` 禁 DDL，且唯一索引行为属 SQLite 既有语义） |
| **`R-WP4C-18`** | 🟠 中 | **★ 连接器共用 ⇒ 消息流与 history 强耦合**（`R2` 新增 · `R3` 保留）：`CB` 同时承载 `/api/webhook` 与 `/api/history/ingest`；**改连接器（URL / TLS / 头 / 超时）会同时影响两条链路**。 | ① **只改 URL 与请求头**，不动 TLS / 超时；② 变更后**必须同时实测两条路径**；③ 回滚必须**成对**（动作 Path + 连接器 URL）；④ 回写存档时**两条链路配置一起记** |
| **`R-WP4C-18b`** | 🟠 中 | **★ 连接器不可编辑 ⇒ 删除不可逆**（**`R3` 新增**）：官方文档明示 **「编辑连接器 / 启用禁用连接器：仅适用于专有版部署」** ⇒ `R2` 的"就地改写"**不可执行**；改走**删除重建**后，旧对象 `c-…b12100` / `a-…921e22` / `r-…a01720` 的 **ID 永久消失、无法还原**，其**运行统计**（`R-WP4C-15` 的唯一量化证据）**一并丢失**。 | ① **施工前必须**抄录旧规则/动作运行统计 + 截图配置（施工说明 **§3 采集 6 项** · 回写 **§11.2 存证**）；② 新对象 ID **登记入册**（**§11.1**）；③ 回滚 = **用新 ID 重建等价物**（**施工说明 §10**），**不是**"恢复原样" |
| **`R-WP4C-19`** | 🟡 低 | **★ 拆除—重建中断窗口**（`R2` 新增"编辑窗口"· `R3` **重定义为拆除窗口**）：`R3` 下窗口 = **「删除规则」→「两条新规则均启用」**（目标 **≤15 min**）⇒ 期间消息流短暂失效。 | ① **§3 采集全部做在前面**（拆除后只剩连点操作）；② 变更前后对 `mqtt_messages` **计数做差分核验**；③ 影响面**仅**消息流一条 **best-effort** 链路（≈4–70 条/天 ⇒ **期望丢 0–1 条**）；④ 不接受零窗口替代（`R-E` 需改端点契约，**违反冻结设计**） |
| **`R-WP4C-16`** | 🟠 中 | **★ 连接器 TLS 明细「保存后不可回读」** —— EMQX 控制台在连接器保存后**不再展示 TLS 段（SNI / 验证服务器证书 / CA）**（人工实测，`2026-10-12`）⇒ **`V-0` 的 TLS 项永久无法完整采集**；现有两个连接器仅有人工**回忆值**（SNI 已填 / CA 未设置 / **口径与目标域表述不一致**）。叠加：**健康检查（30 s）会对端点发探测**，其 `resource down` 告警与**真实投递失败在统计上混淆** | **guard ①**：新建连接器时**保存前截图 TLS 段**（唯一可留痕形态）→ 归档 `EMQX_Assets/config/data-integration/assets/wp4c-v2v4/`（打码）· **guard ②**：保存后**实测验收**（「测试连接」到达 **HTTP 层** = TLS 通；出现 `TLS/certificate/ssl` ⇒ 先查 SNI；端点 `POST` 无 key ⇒ `401`）· **禁止**用 `README.md` §6.4 的书写值充当实测基线（§6A.2 第 3 条）· `V-2` 后须**观察 resource down 告警频率** |
| **`R-WP4C-20`** | 🔴 **高（曾致施工中断）** | **★ 规则 SQL 语法子集**（**`R3.1` 新增** · 控制台实测触发）：`R3` 交付的谓词含 **`==`** ⇒ 控制台报 **`11: syntax error before: '='`**（解析器 trace 显示 `payload.c ==` 被词法拆成**两个** `=` token）。**根因**：EMQX 规则 SQL 的"完全相等"**只有单个 `=`**（官方 *Rule SQL Reference*），**`==` 不存在**；且"键是否存在"**应以内置函数 `is_null` / `is_not_null` 表达**（官方 *Built-in SQL Functions*：`is_null(Term)` = 判 `undefined`/未赋值，**路径中间字段缺失亦返回 `true`**），而非 `!= 'undefined'` 字符串比较。 | ① **`R3.1` 修正**：谓词改用 **`is_not_null(payload.<五键>)` ∧ `is_null(payload.c)` ∧ `is_null(payload.cmd)`**；旧规则取反 = **德摩根展开**（只用 `OR`/`AND`/`is_*`，**无 `NOT`、无括号**，避开子集风险）；② **最后备选**：`'undefined'` 字符串比较（**相等必须单 `=`**）—— 仅当 `is_null` 未知时使用；③ **强制新增验证步**：保存前必须用规则页 **「启用调试 + 发送消息」** 跑两条样例（`{"msg":"hello"}` ⇒ history **不匹配** / message **匹配**；telemetry 信封 ⇒ 反之），**符合才保存**；④ 同批修正 `Implementation-Plan` §8（`v3.1`）· 冻结设计 §4.5/§10.4 · 施工任务单/说明。**教训**：`§4.5` 已声明"语法非冻结"，但 `R1–R3` 仍在**未实测**的情况下把写法写成定稿 ⇒ 今后凡涉 EMQX SQL **必须先控制台试跑再定稿** |

---

## §8 Gate 状态

| Gate | 状态 | 变化 |
|---|---|---|
| `DT:WP-4` | `FROZEN` | 未动 |
| `H-WP4C-03` | 🔒 `CLOSED` | 未动 |
| `H-WP4C-04…08` | 🔒 `CLOSED` | 未动（授权已下达） |
| `H-WP4C-09-A` / `-09-B` | ✅ `CLOSED WITH GUARD` / 🟡 **`READY (R3.1)`** | **★ 本轮变更** —— `-09-B` 依赖已全部闭合（`-09-A` + `H-WP4C-11`）；**施工中**：旧对象 3 个已拆除 · 连接器 `c-n302933b-0c1cb0` 已建 · 规则 SQL 已按 `R3.1` 修正（`is_null`/`is_not_null`） |
| **`H-WP4C-10` / `-10-A`** | 🔒 **`CLOSED`** | **★ 本轮变更** —— **裁决 = 方案 A（`Pages Proxy`）** |
| **`H-WP4C-11`** | ✅ **`CLOSED`** | **★ 本轮变更** —— Pages 中转**已新增并部署**（`61b4938e-…`）+ 端到端 `PASS 15 / FAIL 0` |
| **准入 `V-0` / `V-1`**（纪律 §6A.3） | ✅ `CLOSED WITH GUARD` / 🟢 **`PASS`（带说明）** | **★ 本轮变更** —— 基线已固化 · 目标可达性由 presence 连接器同 host 生产投递作证 |
| **`CF-2`** | 🔴 **`BLOCKED`** | **未解除**（判据 = **冻结验证集 `V-1…V-7`**，仍全未跑） |
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

*本文档于 **2026-10-12** 建立，为 **`WP4C` 部分实施记录**（步骤 ①② 完成 · ③ ✅ 由人工完成（`V-0`）· ④ ✅ Pages 中转部署并验证）；
**未绕过平台限制、未修改冻结设计**。基线 = 固件仓 **`d146308`** · CF 仓 **`9b25203`**（已部署为 `99e6d937`）· EMQX 仓 **`eec9444`**。*
*`2026-10-12` **第七阶段增补**：**`H-WP4C-11` = ✅ `CLOSED`**（Pages 中转部署 `61b4938e-…` + 端到端 `PASS 15 / FAIL 0`）· **准入 `V-1` = 🟢 `PASS`（带说明）** · **`H-WP4C-09-B` = 🟡 `READY`** · 新增 **`R-WP4C-17`** · `R-WP4C-15` 获新证据（`…/log` **0 行**）。
**本阶段唯一生产变更 = 1 次已授权的 Pages 部署**（另含合成测试行的写入与定点清理）；**零 EMQX 写 · 零 Worker 部署 · 零 migration · 零固件改动**。
记录 → **`docs/review/P3-WP4C-History-Relay-Deployment-Verification.md`**。最新基线 = 固件仓 **`ef49db5`** · CF 仓 **`9b25203`**（+ 本次新增）· EMQX 仓 **`b6bdbe2`**。*
*`2026-10-12` **`R2` 设计修订回填**：新增 **`D-7`（Serverless 配额不可调整）** ⇒ 冻结设计升级 **`R2`** ·
`Implementation-Plan` §8 规格升版 **`v2`**（连接器复用 + 动作级路径）· **`H-WP4C-09-B` = 🟡 `READY (R2)`** ·
新增 **`R-WP4C-18`（连接器复用耦合）/ `-19`（连接器编辑窗口 ≤60 s）**。
**本阶段零生产变更**（仅文档）。最新基线 = 固件仓 **`def74f7`** · CF 仓 **`415cd0c`** · EMQX 仓 **`efa117d`**。*
*`2026-10-12` **`R3` 设计修订回填**：人工反馈 **「连接器建立好后无法修改变更，只能删除旧的建立新的」** ——
官方文档逐字确认 **「编辑连接器 / 启用禁用连接器：该功能仅适用于专有版部署」** ⇒ **Serverless 连接器不可编辑**
⇒ `R2` 的就地改写**不可执行** ⇒ **施工形态改为「删除重建」**（新连接器 `CB` + 动作 `MF`/`HI` + 规则 ×2；**presence 零接触**）。
冻结设计 → **`R3`** · `Implementation-Plan` §8 规格 → **`v3`** · **`H-WP4C-09-B` = 🟡 `READY (R3)`** ·
新增 **`R-WP4C-18b`（删除不可逆 ⇒ 施工前必须抄录运行统计 + 截图配置）** · **`R-WP4C-19` 重定义为"拆除—重建窗口 ≤15 min"**。
★ **净收益**：**`-S2` 双投窗口彻底消失**（消息流规则一出生即带 `NOT 谓词`）。**施工规格 → `docs/review/P3-WP4C-Console-Build-Guide-R3.md`**。
**本阶段零生产变更**（仅文档）。最新基线 = 固件仓 **`1d15104`** · CF 仓 **`415cd0c`** · EMQX 仓 **`3ea08fe`**。*
*`2026-10-12` **`R3.1` 回填（人工施工中反馈驱动）**：人工**已拆除 3 个旧对象**并**新建连接器 `CB = c-n302933b-0c1cb0`**；
新建规则时控制台报 **`11: syntax error before: '='`** ⇒ 定位为**我方 `R1–R3` 的 SQL 写法错误**（**`==` 不被 EMQX SQL 支持**；
"键存在"应使用官方 **`is_null` / `is_not_null`**）⇒ **`R3.1` 修正 = 仅改谓词写法，语义逐字不变**。
新增 **`R-WP4C-20`**（🔴 高 · 曾致施工中断）· **新增强制验证步**（规则保存前必须用规则页调试试跑两条样例）。
同步：冻结设计 **§4.5 / §10.4** → `R3.1` · `Implementation-Plan` §8 → **`v3.1`** · 施工任务单/说明 → `R3.1` · **`H-WP4C-09-B` = 🟡 `READY (R3.1)`**。
**本阶段零生产变更**（仅文档）。最新基线 = 固件仓 **`4c74d3a`** · CF 仓 **`415cd0c`** · EMQX 仓 **`a73968a`**。*
