# P3 · `H-WP4C-10` —— **Action 目标形态裁决记录**（`DECISION PENDING` · 🛑 **等待人工**）

> ## 🛑 `DOC-ONLY` · **待裁决** —— **未创建 / 未修改任何 EMQX Rule · Action · Connector · 未部署任何代码**
>
> | 项 | 值 |
> |---|---|
> | **本文档性质** | **待裁决记录（Decision Pending）** —— 登记 `H-WP4C-10` 的两个候选方案，**等待人工裁决** |
> | **裁决编号** | **`H-WP4C-10-A`** —— *Action Target = Pages Proxy 或 Worker Direct* |
> | **状态** | 🔴 **`OPEN`**（**代理不选择、不推荐、不预设**） |
> | **上游依据** | `H-WP4C-10`（实施记录 §5 登记）· 冻结设计 `D-2`（`P3-WP4C-EMQX-Rule-Design.md` §6）· `R-WP4C-12`（实施记录 §7） |
> | **执行约束** | `docs/review/P3-WP4C-Implementation-Discipline.md` **§6A `EMQX Console Boundary Rule`**（📌 `BINDING`） |
> | **基线** | 固件仓 HEAD = **`d08927c`**（工作树 clean）· CF 仓 **`9b25203`** · EMQX 仓 **`eec9444`** |
> | **本文档不做的** | ❌ 不创建 / 修改 EMQX 规则、连接器、动作 · ❌ 不部署 Worker / Pages · ❌ 不改 migration · ❌ 不改冻结设计 · ❌ 不 push |
>
> **★ 重要**：本文档**只登记候选与风险**。**方案 A / B 的取舍必须由人工裁决** ——
> 代理**不得**自行选择、不得以"更合理"为由预设默认值。裁决前，纪律 §6A.4 的 **`STOP` 条件 1 持续成立**。

---

## §1 问题陈述

### 1.1 待裁问题

冻结设计要求 `guo_feeder_history_ingest` 规则的 **HTTP Action** 指向
**`POST /api/history/ingest`**（`DT:WP-4` §9.1 冻结端点）。**但「Action 的 URL 应写成什么形态」尚未裁决**：

| 待定项 | 说明 |
|---|---|
| **目标主机** | **Pages 中转域名**（`*.pages.dev`）还是 **Worker 域名**（`*.workers.dev`） |
| **完整 URL** | `<主机>/api/history/ingest` —— 路径**不可**追加到连接器基础路径之后（EMQX 的「URL 路径」是**追加**语义，见 `EMQX_Assets` §6.0.1 警告） |
| **连接器** | **必须新建独立 HTTP Server 连接器**（URL 直写完整端点），**不复用**既有消息流连接器 |

### 1.2 为什么必须由人工裁决

| # | 事实 | 后果 |
|---|---|---|
| 1 | 冻结设计 `D-2` 断言 **EMQX 打不通 `*.workers.dev`（连接被拒绝）**、可达 `*.pages.dev` | ⇒ 若 `D-2` 成立，**方案 B 必然投递失败** |
| 2 | `EMQX_Assets` 存档 §1 记录：数据集成**已在线上运行**，且**历史目标 URL = `https://guo-feeder-api.guobaby.workers.dev/`（根路径，无鉴权）** | ⇒ 与 `D-2` **表面矛盾**（`R-WP4C-12`） |
| 3 | 上述存档**自认**该 URL 是 **"P0 遗留根路径"**，并在 §2 明确「按 §3 直接改 URL **会连不上**，须经 Pages 中转」 | ⇒ 存档 §1 的 URL **不能**作为"EMQX 可达 `workers.dev`"的证据 |
| 4 | EMQX 数据集成 API **读 + 写全 `403`**、**无 CLI** ⇒ **代理无法实测** Action 可达性 | ⇒ 可达性**只能**由人工在控制台侧实测（= 准入序列 `V-1`） |

> **⇒ 结论**：**这是环境事实问题，不是设计问题**。设计已冻结（`D-2` 倾向中转），**但事实未证**。
> 依纪律 §6A.2，代理**不得**用历史文档替代实测、**不得**猜测 Action 配置 ⇒ **交由人工裁决**（`H-WP4C-10-A`）。

---

## §2 方案 A —— **Pages Proxy**（EMQX Action → Pages 中转 → Worker）

### 2.1 形态

```
EMQX Rule（guo_feeder_history_ingest）
      │  HTTP POST
      ▼
Pages Function  https://<pages 域名>/api/history/ingest     ← EMQX 填这个
      │  逐字转发（不解析 JSON · 密钥原样透传 · 不持有密钥）
      ▼
Worker          https://guo-feeder-api.guobaby.workers.dev/api/history/ingest
      │  history-ingest.js
      ▼
D1              telemetry_history / log_history
```

### 2.2 优点

| # | 优点 | 依据 |
|---|---|---|
| 1 | **与冻结设计一致** | 冻结设计 `D-2`：EMQX 可达 `*.pages.dev`，须经 Pages 中转 |
| 2 | **隔离 `workers.dev`** | EMQX 侧只暴露 Pages 域名；Worker 域名**不进入 EMQX 配置**，便于后续给 Worker 换域名 / 挂自定义域而不动 EMQX |
| 3 | **便于后续认证控制** | 中转层是**唯一收口点**：将来加 IP 白名单 / 签名校验 / 限流，**不改 EMQX、不改 Worker** |
| 4 | **已有可复用的先例** | 同 Pages 工程 `/api/presence/event`（P1-6-2）**已长期在用**，实现模式（逐字转发 / 不解析 / 密钥透传 / 上游不可达返 `502`）可直接照抄 |

### 2.3 前置成本（**须知晓**）

| # | 项 | 现状（实测） |
|---|---|---|
| 1 | Pages 工程的 `/api/history/ingest` **函数尚不存在** | 工程 `guo-feeder-pagesdev/functions/api/` 下现有 `command.js` · `presence/event.js` · `webhook.js` —— **无** `history/ingest.js` |
| 2 | ⇒ 采用方案 A **必须先新增该 Pages Function 并部署 Pages 工程** | 属**部署动作**，**不在本阶段范围**（本阶段零部署）⇒ 须作为 `H-WP4C-09-B` 的**前置**另行授权 |
| 3 | 中转→Worker 连通性需**实测** | 判据同 P1-6-2：`POST` 不带 `X-Webhook-Key` ⇒ **`401`** = 链路通到 Worker；**`502`** = 中转自身转发失败 |

### 2.4 方案 A 对应的 Action 配置（**规格草案 · ⚠️ 未执行**）

| 项 | 值 |
|---|---|
| **连接器** | **新建**独立 HTTP Server 连接器（**不复用**既有消息流连接器） |
| **URL** | `POST https://<pages 域名>/api/history/ingest`（**EMQX 侧填完整端点**） |
| **方法** | `POST` |
| **Content-Type** | `application/json` |
| **请求头** | `X-Webhook-Key: <WEBHOOK_SECRET>`（**必须**；否则 Worker 返 `401`） |
| **Body** | `{"topic":"${topic}","clientid":"${clientid}","payload":"${payload}"}`（**三键不可改名**） |

> **⚠️ 状态**：以上为**规格草案**（承接 `P3-WP4C-Implementation-Plan.md` §8.1），**未执行、未验证**。
> **待 `H-WP4C-10-A` 裁决为 A 后**，仍须以控制台**实测**（`V-1`）为准。

---

## §3 方案 B —— **Worker Direct**（EMQX Action → `*.workers.dev`）

### 3.1 形态

```
EMQX Rule（guo_feeder_history_ingest）
      │  HTTP POST
      ▼
Worker  https://guo-feeder-api.guobaby.workers.dev/api/history/ingest     ← EMQX 填这个
      │  history-ingest.js
      ▼
D1      telemetry_history / log_history
```

### 3.2 风险

| # | 风险 | 依据 |
|---|---|---|
| 1 | **与 `D-2` 冲突** | 冻结设计 `D-2` 明确：EMQX **打不通 `*.workers.dev`**（连接被拒绝）⇒ 若属实，**投递必然失败** |
| 2 | **无法确认 EMQX 网络可达性** | 数据集成 API **全 `403`** + **无 CLI** ⇒ 代理侧**没有任何**实测手段；`EMQX_Assets` §1 的 workers.dev 根路径 URL 是 **P0 遗留记录**（存档自认「直接改 URL 会连不上」）⇒ **不构成可达性证据** |
| 3 | **失败形态不友好** | 若不可达 ⇒ 连接被拒绝 ⇒ `D-4`（非 2xx 视为失败并**重试**）⇒ **重试放大 + 计费放大**（`R-WP4C-8` / `R-WP4C-9`） |
| 4 | **绕过冻结约束的定性风险** | 采用 B **等于**在未获证据的情况下**改写冻结设计的落地方式** ⇒ 须有明确的人工裁决背书，**不得由代理自行采用** |

### 3.3 采用方案 B 的**必要证据**（人工须提供，缺一不可）

| # | 证据 | 取得方式（人工） |
|---|---|---|
| 1 | EMQX **实测**可达 `*.workers.dev` | 控制台建**一条测试规则 / 测试连接**，观察 HTTP Action 返回码（**不得**以"以前能通"代替） |
| 2 | 该证据**可复现**且**留痕** | 截图 / 返回码 / 时间 / 操作者，随 `V-0` 基线一并归档 |
| 3 | 冲突已**正式登记** | 在 `EMQX_Assets/config/data-integration/` 与冻结设计之间写明：`D-2` 在**当前平台版本**上是否失效（若失效 ⇒ 属**设计层事实变更**，须另开裁决，**不得静默采用**） |

> **★ 若采用 B 且无上述证据 ⇒ 违反纪律 §6A.2 第 2/3 条（猜测 Action 配置 / 以历史文档替代 `V-0`）。**

---

## §4 证据对照表（**谁可以作证据，谁不能**）

| # | 观测 | 来源 | 可否作证据 | 说明 |
|---|---|---|---|---|
| 1 | `D-2`：EMQX 打不通 `*.workers.dev`、可达 `*.pages.dev` | 冻结设计 `P3-WP4C-EMQX-Rule-Design.md` §6 | ⚠️ **仅设计断言** | 出自 2026-10-05 实测记录（P1-6-2），**须在当前平台版本复核** |
| 2 | 存档 §1：历史目标 URL = `https://guo-feeder-api.guobaby.workers.dev/`（根路径） | `EMQX_Assets/config/data-integration/README.md` §1 | ❌ **不可** | 由 Worker 侧 `mqtt_messages` **反推**；存档自认为 **P0 遗留路径**，且 §2 明确其**不可直接迁移使用** |
| 3 | 存档 §6.0.1：presence 入口**已走 Pages 中转**并**长期在跑** | 同上 §6.0.1 | ✅ **可**（较强） | **同一平台、同一时期、同一类 HTTP Action** 的成功先例 |
| 4 | Pages 工程**已有**中转函数（`presence/event.js`）实现 | CF 仓 `guo-feeder-pagesdev/functions/api/presence/event.js` | ✅ **可** | 逐字转发 / 不解析 / 密钥透传 / 上游不可达返 `502` —— **模式可直接复用** |
| 5 | Pages 工程**没有** `/api/history/ingest` 函数 | CF 仓 `guo-feeder-pagesdev/functions/api/`（实测清单） | ✅ **可** | ⇒ 方案 A **有前置部署成本**（见 §2.3） |
| 6 | EMQX 数据集成 API **读 + 写全 `403`** | 实施记录 §4.1（实测矩阵） | ✅ **可** | ⇒ 代理侧**无法**实测可达性 ⇒ 必须人工 |

> **★ 证据力小结（不构成推荐）**：现有**可采信**证据指向「**Pages 中转可用**」（证据 #3 / #4）与「**`workers.dev` 直连未获证据**」（证据 #2 被否）；
> 但**这不等同于裁决** —— `D-2` 须在当前平台版本复核（证据 #1 的 ⚠️），**取舍得由人工作出**。

---

## §5 裁决项 —— `H-WP4C-10-A`

| 项 | 内容 |
|---|---|
| **ID** | **`H-WP4C-10-A`** |
| **主题** | **Action Target 形态裁决** |
| **待决问题** | **Action Target = `Pages Proxy`（方案 A）或 `Worker Direct`（方案 B）？** |
| **裁决人** | **人工**（EMQX 控制台责任人 / 项目负责人） |
| **状态** | 🔴 **`OPEN`** |
| **代理立场** | **不选择 · 不推荐 · 不预设默认值**（纪律 §6A.2） |
| **裁决后产出** | ① 明确的目标 URL 形态（写进 `V-0` 归档）② 若选 A：Pages Function 的新增与部署授权（另立授权项）③ 若选 B：§3.3 三项证据齐备 |
| **解锁对象** | 纪律 §6A.3 的 **`V-1`（确认目标 Action 可达）** ⇒ 进而解锁 `H-WP4C-09-B`（Rule 实施） |

---

## §6 裁决前的**必答清单**（人工，逐项）

| # | 问题 | 若答"否" |
|---|---|---|
| 1 | 是否已在**当前平台版本**上复核 `D-2`（EMQX 对 `*.workers.dev` 的可达性）？ | **`STOP`** —— 不得选 B（§3.3） |
| 2 | 若选 **方案 A**：是否已确认 Pages 工程可新增并部署 `/api/history/ingest` Function？ | **`STOP`** —— 目标形态不成立 |
| 3 | 是否已确认 `<pages 域名>`（**完整端点**，非基础路径 + 追加）？ | **`STOP`** —— 不得配置 Action URL |
| 4 | 是否已确认 `X-Webhook-Key` 与 Worker Secret **一致**（且**不入仓**）？ | **`STOP`** —— 配置必返 `401` |
| 5 | 是否已具备**回滚方案**（回切旧端点 / 停用新规则，见纪律 §2.1 / §4）？ | **`STOP`** —— 不得开始变更（纪律 §6A.4 条件 3） |
| 6 | 是否已取得 **`V-0` 基线**（`H-WP4C-09-A` 完成）？ | **`STOP`** —— 不得变更既有规则（纪律 §6A.4 条件 2） |

---

## §7 代理约束（本文档**不做**什么）

| # | 约束 |
|---|---|
| 1 | **不选择方案** —— 不写"推荐 A / 建议 B"，不设默认值，不以篇幅 / 排版暗示倾向 |
| 2 | **不实测可达性** —— 不向 EMQX 发起任何写请求；不重试 `403` 端点；不换凭据试权限 |
| 3 | **不代填配置** —— 不部署 Pages Function，不修改连接器 / 动作配置 |
| 4 | **不关闭裁决项** —— `H-WP4C-10` / `H-WP4C-10-A` **保持 `OPEN`** |
| 5 | **不推进实施** —— 依纪律 §6A.4，`STOP` 条件 1（Action endpoint 未确认）**已成立** ⇒ EMQX 侧**持续 `STOP`** |

---

## §8 影响面（裁决结果的下游影响）

| 裁决结果 | 需要的新增工作 | 影响 |
|---|---|---|
| **方案 A（Pages Proxy）** | 新增 `guo-feeder-pagesdev/functions/api/history/ingest.js` + Pages 部署（**须另行授权**） | EMQX 侧配置**无额外风险**；多一跳 RTT（`R-WP4C-8` 已登记） |
| **方案 B（Worker Direct）** | 无需中转；但须提供 §3.3 三项证据 | 与 `D-2` 冲突**须正式登记**；投递失败风险高（`R-WP4C-12`） |

> 两方案**均要求**：**新建独立连接器**（不复用消息流连接器）· **`X-Webhook-Key` 必须配置** · **Body 三键不可改名**。

---

## §9 边界核对（**零违规**）

| 禁止项 | 状态 |
|---|---|
| 创建 / 修改 EMQX Rule · Action · Connector | ✅ **未做**（**未调用任何写 API**） |
| 部署 Worker / Pages | ✅ **未部署**（CF 仓 HEAD 仍 `9b25203`，工作树 clean） |
| 执行 / 修改 migration | ✅ **未执行 · 未改** |
| 修改 firmware / `src/` / ACL / topic / `/up` | ✅ **未改**（固件仓仅 `docs/`） |
| 修改冻结设计（含 `D-2`） | ✅ **未改** |
| 关闭 `HUMAN_REQUIRED` | ✅ **未关闭**（`H-WP4C-10` / `-10-A` 保持 `OPEN`） |
| `push` | ✅ **未 push** |

> **只读声明**：本轮生产侧**零操作**（未发任何 EMQX 请求；未调用 `wrangler`）。本文档事实均取自**基线仓内既有文档与已跟踪源码**，可逐条回查。

---

## §10 关联文档

| 文档 | 关系 |
|---|---|
| `docs/review/P3-WP4C-EMQX-Rule-Design.md` | 🔒 冻结设计（`D-2` 上游依据 · 本文档**不改它**） |
| `docs/review/P3-WP4C-Implementation-Discipline.md` | 📌 **§6A `EMQX Console Boundary Rule`**（准入序列 `V-0…V-5` · `STOP` 条款） |
| `docs/review/P3-WP4C-Decisions.md` | 裁决记录（`H-WP4C-10-A = OPEN` 登记处 · §6B） |
| `docs/review/P3-WP4C-Implementation-Record.md` | 实施记录（`R-WP4C-12` 矛盾来源 · `H-WP4C-10` 原始登记） |
| `docs/review/P3-WP4C-Implementation-Plan.md` | 实施计划 §8.1（console-ready 规格草案） |
| `EMQX_Assets/config/data-integration/README.md` | 平台约束 §6.0.1（Pages 中转先例）· 既有 URL 记录 §1 · **回写目标** |
| `Cloudflare_Assets/guo-feeder-pagesdev/functions/api/presence/event.js` | 中转实现先例（模式参考） |

---

*本文档于 **2026-10-12** 建立，为 **`H-WP4C-10-A` 待裁决记录**；
**零生产变更**，**未选择方案**，**未关闭任何裁决项**。
基线 = 固件仓 **`d08927c`** · CF 仓 **`9b25203`** · EMQX 仓 **`eec9444`**。所有事实均可逐条回查。*
