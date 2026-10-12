# P3 · `H-WP4C-10` —— **Action 目标形态裁决记录**（✅ `DECIDED` · **方案 A = `Pages Proxy`**）

> ## ✅ `DOC-ONLY` · **已裁决（人工）** —— **裁决结果 = 方案 A（`Pages Proxy`）** · **未创建 / 未修改任何 EMQX Rule · Action · Connector · 未部署任何代码**
>
> | 项 | 值 |
> |---|---|
> | **本文档性质** | **裁决记录（Decision Record）** —— `H-WP4C-10` / `H-WP4C-10-A` 的**人工裁决留痕** |
> | **裁决编号** | **`H-WP4C-10-A`** —— *Action Target = Pages Proxy 或 Worker Direct* |
> | **状态** | ✅ **`CLOSED`** —— **人工裁决 = 方案 A（`Pages Proxy`）**（`2026-10-12` · 见 §5.1） |
> | **裁决依据（人工口径）** | **「`workers` 无法直连」** ⇒ 冻结设计 `D-2` **成立**；`R-WP4C-12` 的矛盾**就此收敛**（见 §1.3 / §4） |
> | **上游依据** | `H-WP4C-10`（实施记录 §5 登记）· 冻结设计 `D-2`（`P3-WP4C-EMQX-Rule-Design.md` §6）· `R-WP4C-12`（实施记录 §7） |
> | **执行约束** | `docs/review/P3-WP4C-Implementation-Discipline.md` **§6A `EMQX Console Boundary Rule`**（📌 `BINDING`） |
> | **基线** | 固件仓 HEAD = **`0c8dc68`**（工作树 clean）· CF 仓 **`9b25203`** · EMQX 仓 **`eec9444`** |
> | **本文档不做的** | ❌ 不创建 / 修改 EMQX 规则、连接器、动作 · ❌ 不部署 Worker / Pages · ❌ 不改 migration · ❌ 不改冻结设计 · ❌ 不 push |
>
> **★ 裁决只决定「Action 的 URL 写成什么形态」** —— **不解除** `CF-2`、**不等于**管线打通、
> **不等于** `H-WP4C-09-B` 可开工（另有**页面中转部署**前置，`H-WP4C-11`）。
> 纪律 §6A.4 的 `STOP` 条件 1 由「形态未定」**转为**「**中转未部署 ⇒ 目标仍不可达**」⇒ **EMQX 侧变更继续 `STOP`**。
>
> **🔄 更新（`2026-10-12` · `H-WP4C-11` 已部署）**：Pages 中转 `/api/history/ingest` **已新增并部署并端到端验证**（`PASS 15 / FAIL 0`）⇒
> **纪律 §6A.4 三个 `STOP` 条件均不再成立**（目标可达 · `V-0` 已固化 · 回滚已签署）；§5.2 开工门**六项全绿**。
> 记录 → `docs/review/P3-WP4C-History-Relay-Deployment-Verification.md`。

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

### 1.3 裁决后的收敛（**`2026-10-12` 更新**）

| # | 项 | 收敛结论 |
|---|---|---|
| 1 | **`D-2`（EMQX 打不通 `*.workers.dev`）** | ✅ **确认成立** —— 人工裁决口径：「**`workers` 无法直连**」 |
| 2 | **`R-WP4C-12`（`D-2` 与存档 §1 矛盾）** | ✅ **收敛（CLOSED）** —— 存档 §1 的 `workers.dev` 根路径是**由 Worker 侧落库反推**的**上游目标**（P0 遗留），**不是** EMQX 侧连接器 URL；二者本就不在同一层 |
| 3 | **★ 既存实测旁证（最强）** | EMQX 仓 §6.4 记录：**既有连接器 `c-n302933b-b12100`（`guo_feeder/#` → `/api/webhook`）指向 `https://guo-feeder-pagesdev.pages.dev`，且长期可用** ⇒ **Pages 中转账单在本平台已跑在生产**，方案 A **不是新方案，而是既有形态的复制** |
| 4 | **⇒ 方案选择** | ✅ **方案 A（`Pages Proxy`）** —— 与 `D-2` 一致，且**有生产在跑的先例**（第 3 项） |
| 5 | **本裁决** | **只解决「EMQX 侧 Action 的 URL 写成 Pages 域名」**；**不解决** Pages 侧是否已有可用的 `/api/history/ingest` 转发函数（见 `H-WP4C-11`） |

---

## §2 方案 A —— **Pages Proxy**（EMQX Action → Pages 中转 → Worker）· ✅ **已采纳**

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

### 2.3 前置成本（**⚠️ 已裁决采纳 A ⇒ 本项成为 GATING 前置**）

| # | 项 | 现状（实测） |
|---|---|---|
| 1 | Pages 工程的 `/api/history/ingest` **函数尚不存在** | 工程 `guo-feeder-pagesdev/functions/api/` 下现有 `command.js` · `presence/event.js` · `webhook.js` —— **无** `history/ingest.js` |
| 2 | ⇒ 采用方案 A **必须先新增该 Pages Function 并部署 Pages 工程** | 属**部署动作** ⇒ **须另行授权** = **`H-WP4C-11`**（本轮登记） |
| 3 | 新增文件路径（**待授权后实施**） | `Cloudflare_Assets/guo-feeder-pagesdev/functions/api/history/ingest.js` —— **照抄** `functions/api/presence/event.js` 的逐字转发模式（不解析 / 密钥透传 / 上游不可达返 `502`），仅把上游改为 `…/api/history/ingest` |
| 4 | 中转→Worker 连通性需**实测** | 判据同 P1-6-2：`POST` 不带 `X-Webhook-Key` ⇒ **`401`** = 链路通到 Worker；**`502`** = 中转自身转发失败；**404 / `200`+HTML** = 路径写错 |
| 5 | **★ 部署会改变 CF 仓 HEAD ↔ 线上版本的对应关系** | 当前 CF 仓 HEAD `9b25203` **已部署**（= `99e6d937`）⇒ **repo == deployed**。新增 function 后须**同批部署**，以维持该等价关系（避免"仓里有、线上没有"的漂移） |

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

## §3 方案 B —— **Worker Direct**（EMQX Action → `*.workers.dev`）· ❌ **未采纳（裁决排除）**

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
| 1 | `D-2`：EMQX 打不通 `*.workers.dev`、可达 `*.pages.dev` | 冻结设计 `P3-WP4C-EMQX-Rule-Design.md` §6 | ✅ **已被人工裁决确认**（`H-WP4C-10-A` · 「`workers` 无法直连」） | 出自 2026-10-05 实测记录（P1-6-2） |
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
| **裁决人** | **人工**（项目负责人） |
| **状态** | ✅ **`CLOSED`** |
| **裁决结果** | ✅ **方案 A = `Pages Proxy`** |
| **裁决口径（人工原话）** | **「裁决仍然使用 pages 代理，workers 无法直连」** |
| **裁决后产出** | ① 目标 URL 形态 = **Pages 域名**（`https://guo-feeder-pagesdev.pages.dev/api/history/ingest`）② **Pages Function 新增 + 部署授权 = `H-WP4C-11`**（本轮登记）③ §3.3 三项证据**无需**（方案 B 已排除） |
| **解锁对象** | 纪律 §6A.3 的 **`V-1`（确认目标 Action 可达）** —— ✅ **已 `PASS`（带说明）**（`H-WP4C-11` 已部署 + 同 host 由 EMQX 生产投递作证 · 见 `P3-WP4C-History-Relay-Deployment-Verification.md` §7） |
| **下游阻塞项** | `H-WP4C-09-B`（Rule 实施）= 🟡 **`READY`**（`-09-A` ✅ + `H-WP4C-11` ✅ ⇒ **依赖已全部闭合**；**待人工控制台执行**） |

### 5.1 裁决留痕（**权威**）

| # | 项 | 值 |
|---|---|---|
| 1 | **裁决时间** | **`2026-10-12`**（本地 `GMT+8`） |
| 2 | **裁决方式** | **人工口头裁决**（对话中下达），由代理**登记留痕** |
| 3 | **裁决原文** | **「裁决仍然使用 pages 代理，workers 无法直连」** |
| 4 | **结论** | **Action Target = `Pages Proxy`** ⇒ `D-2` 成立 ⇒ `R-WP4C-12` **收敛 `CLOSED`** |
| 5 | **状态变更** | `H-WP4C-10` = 🔴 `OPEN` → ✅ **`CLOSED`** · `H-WP4C-10-A` = 🔴 `OPEN` → ✅ **`CLOSED`** |
| 6 | **裁决**不**解除** | ❌ 不解除 `CF-2`（判据仍是 `V-1…V-7`）· ❌ 不授权 Pages 部署（须 `H-WP4C-11`）· ❌ 不授权 EMQX 变更（须 `H-WP4C-09-A` 完成） |
| 7 | **目标 URL（结论）** | `POST https://guo-feeder-pagesdev.pages.dev/api/history/ingest` —— **EMQX 侧填这个**；**上游**（EMQX 不要填）= `https://guo-feeder-api.guobaby.workers.dev/api/history/ingest` |

> **★ 记录纪律**：本条为**人工裁决**，代理仅**登记**，**未代作裁决**；与纪律 §6A.2「禁止代理自行选择」不冲突。
> **★ 参照物**：同一 Pages 域名下的 **`/api/presence/event`** 中转（`presence/event.js`）与**既有连接器 `c-n302933b-b12100`** 均已在生产运行 ⇒ 本次为**同形态复制**。

---

## §6 执行前**必答清单**（= `H-WP4C-09-B` 的**开工门** · 逐项）

> 裁决已完成；以下转为 **EMQX 变更开工前**必须全部满足的条件（任一"否" ⇒ `STOP`）。

| # | 问题 | 当前状态 | 若否 ⇒ |
|---|---|---|---|
| 1 | `D-2` 是否复核确认（EMQX 对 `*.workers.dev` 不可达）？ | ✅ **已确认**（`H-WP4C-10-A` 裁决 · §5.1） | —— |
| 2 | Pages 工程是否**已新增并部署** `/api/history/ingest` Function？ | ✅ **已部署并验证**（`H-WP4C-11 = CLOSED` · `2026-10-12` · 部署 `61b4938e-…` · 中转层 + 鉴权边界 **7/7 `PASS`**） | —— |
| 3 | 是否已确认**完整端点**（非基础路径 + 追加）＝ `https://guo-feeder-pagesdev.pages.dev/api/history/ingest`？ | ✅ **已定形**（§5.1 第 7 项）· ✅ **已实测**（`GET` ⇒ `200 "History Relay OK"`；`POST` 无 key ⇒ `401`） | **`STOP`** —— 不得配置 Action URL |
| 4 | 是否已确认 `X-Webhook-Key` 与 Worker Secret **一致**（且**不入仓**）？ | ✅ **已确认**（中转**透传**密钥 · 端到端写入 **8/8 `PASS`** 即证明密钥一致；密钥值**未入仓**） | **`STOP`** —— 配置必返 `401` |
| 5 | 是否已具备**回滚方案**（回切旧端点 / 停用新规则，纪律 §2.1 / §4）？ | ✅ **已签署**（`V0` §5.2） | **`STOP`** —— 纪律 §6A.4 条件 3 |
| 6 | 是否已取得 **`V-0` 基线**（`H-WP4C-09-A` 完成）？ | ✅ **已固化**（`CLOSED WITH GUARD`） | **`STOP`** —— 纪律 §6A.4 条件 2 |

---

## §7 代理约束（本文档**不做**什么）

| # | 约束 |
|---|---|
| 1 | **不代作裁决** —— 本轮裁决由**人工**下达，代理**仅登记**（§5.1）；历史版本中"不选择/不推荐"的立场见 §5.1 说明 |
| 2 | **不实测可达性** —— 不向 EMQX 发起任何写请求；不重试 `403` 端点；不换凭据试权限 |
| 3 | **不代填配置** —— 不部署 Pages Function（须 `H-WP4C-11`），不修改连接器 / 动作配置 |
| 4 | **不关闭未闭合裁决项** —— `H-WP4C-09-A` / `H-WP4C-09-B` / `H-WP4C-11` 状态由人工推进 |
| 5 | **不推进 EMQX 变更** —— 依纪律 §6A.4，`STOP` 条件 1（**目标仍不可达**：中转未部署）与条件 2（**基线未导出**）**均成立** ⇒ EMQX 侧**持续 `STOP`** |
| **★ 本条已于 `2026-10-12` 失效** | `H-WP4C-11` 部署完成后，**`STOP` 条件不再成立** ⇒ 代理约束由「不推进 EMQX 变更」回归为「**不代执行控制台作业**」（`H-WP4C-09-B` 须人工） |

---

## §8 影响面（裁决结果的下游影响）

| 裁决结果 | 需要的新增工作 | 影响 |
|---|---|---|
| ✅ **方案 A（Pages Proxy）—— 已采纳** | 新增 `Cloudflare_Assets/guo-feeder-pagesdev/functions/api/history/ingest.js` + Pages 部署（**`H-WP4C-11`**，须授权） | EMQX 侧配置**无额外风险**；多一跳 RTT（`R-WP4C-8` 已登记）；**保持 CF 仓 HEAD == 线上版本**的等价关系 |
| ❌ 方案 B（Worker Direct）—— 未采纳 | —— | 与 `D-2` 冲突；投递必然失败（`R-WP4C-12` **已收敛为 CLOSED**） |

> 两方案**均要求**：**新建独立连接器**（不复用消息流连接器 `c-n302933b-b12100`）· **`X-Webhook-Key` 必须配置** · **Body 三键不可改名**。
> **采纳 A 后的连接器 TLS 段**：**照抄** EMQX 仓 §6.4（启用 TLS · **SNI = `guo-feeder-pagesdev.pages.dev`（必填）** · 验证服务器证书 = 关 · CA 留空）。

---

## §9 边界核对（**零违规**）

| 禁止项 | 状态 |
|---|---|
| 创建 / 修改 EMQX Rule · Action · Connector | ✅ **未做**（**未调用任何写 API**） |
| 部署 Worker / Pages | ✅ **本轮未部署**（本裁决阶段 CF 仓 HEAD 仍 `9b25203`，工作树 clean）<br>**🔄 后续**：`2026-10-12` 由 **`H-WP4C-11`** 另批完成 **1 次 Pages 部署**（`61b4938e-…`）；**Worker 未部署** |
| 执行 / 修改 migration | ✅ **未执行 · 未改** |
| 修改 firmware / `src/` / ACL / topic / `/up` | ✅ **未改**（固件仓仅 `docs/`） |
| 修改冻结设计（含 `D-2`） | ✅ **未改** |
| 关闭 `HUMAN_REQUIRED` | ✅ **未自行关闭** —— 本轮 **`H-WP4C-10` / `-10-A` 的 `CLOSED` 源自人工裁决**（§5.1）；**新增 `H-WP4C-11`** 保持 `OPEN` |
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

*本文档于 **`2026-10-12`** 建立（**待裁决记录**），同日**由人工裁决后更新为裁决记录** —— **裁决结果 = 方案 A（`Pages Proxy`）**（§5.1）。
**零生产变更**（本轮仅 `docs/`），**未部署任何代码**，**未创建 / 修改任何 EMQX 配置**。
基线 = 固件仓 **`0c8dc68`** · CF 仓 **`9b25203`** · EMQX 仓 **`eec9444`**。所有事实均可逐条回查。*
*`2026-10-12` **第七阶段回填**：**`H-WP4C-11` 已部署并端到端验证**（Pages `61b4938e-…` · `PASS 15 / FAIL 0`）⇒
本裁决的**落地前置已闭合** · **§5.2 开工门六项全绿** · 纪律 §6A.4 `STOP` 条件**均不再成立** · 准入 **`V-1` = 🟢 `PASS`（带说明）**；
**裁决阶段本身仍为零生产变更**（部署由后续 `H-WP4C-11` 另批授权完成）。
最新基线 = 固件仓 **`ef49db5`** · CF 仓 **`9b25203`**（+ 本次新增）· EMQX 仓 **`b6bdbe2`**。*
