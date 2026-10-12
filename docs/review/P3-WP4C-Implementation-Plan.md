# P3 · `WP4C` Telemetry Routing —— **实施计划 + 当前状态快照**（`PLAN ONLY` · **已 STOP**）

> ## 🛑 `STOPPED AT COMMIT 2` —— **未创建任何 EMQX 规则 · 未改动 Worker / firmware / migration / ACL**
>
> | 项 | 值 |
> |---|---|
> | **本文档性质** | **实施计划（Commit 1 产物）** + **EMQX / CF / D1 当前状态快照（实测）** |
> | **冻结依据（唯一，不得重设计）** | `docs/review/P3-WP4C-EMQX-Rule-Design.md`（🔒 `FROZEN`）+ `docs/review/P3-WP4C-Decisions.md` |
> | **基线** | 固件仓 HEAD = **`f31aabe`**（工作树 clean）· CF 仓 **`9b25203`** · EMQX 仓 **`eec9444`** |
> | **三 commit 任务执行结果** | **Commit 1 = ✅ 已完成（本文档）** · **Commit 2 = 🛑 `STOPPED`（不可执行）** · **Commit 3 = ⛔ 未开始** |
> | **本文档不做的** | ❌ 不创建 / 修改 EMQX 规则或连接器 · ❌ 不改 Worker · ❌ 不改 migration · ❌ 不改 firmware / ACL / topic · ❌ 不部署 · ❌ 不关闭 `H-WP4C-04/05/06` |
>
> **停因（一句话）**：**EMQX Cloud Serverless 的数据集成（规则 / 连接器 / 动作）只能控制台操作（API 全 403）**
> ⇒ 冻结设计的**实施动作无法由本代理执行**，且 **`H-WP4C-04` / `05` 授权仍未下达**、**目标端点尚未部署**
> ⇒ 依任务书「**发现冻结设计无法直接实施的问题：立即 STOP，创建 HUMAN_REQUIRED，不得自行修改设计**」，**停止在 Commit 2 之前**。

---

## §1 授权与执行条件核对（**执行前门禁**）

| # | 条件 | 要求 | 实测 | 判定 |
|---|---|---|---|---|
| A-1 | 设计已冻结 | `H-WP4C-03 = CLOSED` | ✅ `f31aabe` | ✅ `PASS` |
| A-2 | 规则可编程创建 | 需 API 或 CLI 可用 | 🔴 **API 全 `403`**（§2.2）· Serverless **无 CLI** | 🔴 **`FAIL`（不可程序化）** |
| A-3 | `H-WP4C-05`（Action 切换授权） | 需 **人工授权** | 🔴 `OPEN` | 🔴 **`FAIL`** |
| A-4 | `H-WP4C-04`（远端 `0003` 授权） | 需 **人工授权** | 🔴 `OPEN`；且 **`0003` 未应用**（§4） | 🔴 **`FAIL`** |
| A-5 | `H-WP4C-06`（部署顺序） | 需确认 | 🔴 `OPEN` | 🔴 **`FAIL`** |
| A-6 | 目标端点已部署 | `POST /api/history/ingest` 线上存在 | 🔴 **线上返回 catch-all `OK`（路由不存在）**（§3.2） | 🔴 **`FAIL`** |
| A-7 | 固件侧数据源 | telemetry / log 信封可产出 | 🟡 代码在树（`a89ee3c` / `dae7966`），**telemetry 默认关闭** | 🟡 **`PARTIAL`** |

> **⇒ A-2 / A-3 / A-4 / A-5 / A-6 五项不满足 ⇒ Commit 2 不具备执行条件。**
> **★ 纪律**：A-3/A-4/A-5 均为**人工授权项**，**代理不得自行裁决**（任务书明令：不得自行修改设计）。

---

## §2 EMQX 当前状态快照（**实测 · `2026-10-12`**）

### 2.1 环境标识

| 项 | 值 | 来源 |
|---|---|---|
| 部署 | EMQX Cloud **Serverless** | `.env` / `EMQX_Assets/README.md` |
| API Base | `https://n302933b.ala.cn-hangzhou.emqxsl.cn:8443/api/v5` | `EMQX_Assets/.env:13` |
| 凭据 | `EMQX_APP_ID = xd831a01`（App Secret 存 `.env`，**gitignored**，未入库/未打印） | `EMQX_Assets/.env` |
| 仓库基线 | EMQX 仓 HEAD = **`eec9444`**（工作树 clean） | `git log` |

### 2.2 API 可用性实测（**决定 Commit 2 可行性**）

> 方式：`curl --noproxy '*' -u <APP_ID>:<SECRET>`，**直连**（EMQX 可直连，无需代理）。**仅读，未做任何写操作。**

| # | 端点 | 期望（控制台专属资源） | **实测** |
|---|---|---|---|
| 1 | `GET /rule_engine/rules` | 规则列表 | 🔴 **`403`** |
| 2 | `GET /rules` | 规则列表（旧路径） | 🔴 **`403`** |
| 3 | `GET /connectors` | 连接器列表 | 🔴 **`403`** |
| 4 | `GET /actions` | 动作列表 | 🔴 **`403`** |
| 5 | `GET /bridges` | 桥接列表（旧路径） | 🔴 **`403`** |
| 6 | `GET /data_integration/rules` | 数据集成规则 | 🔴 **`403`** |
| 7 | `GET /authentication` | 认证配置 | 🔴 **`403`** |
| 8 | `GET /authorization/sources` | 授权源（ACL） | 🔴 **`403`** |
| 9 | `GET /nodes` | 节点信息 | 🔴 **`403`** |
| 10 | `GET /deployments` | 部署信息 | 🔴 **`403`** |
| 11 | `GET /clients?limit=5` | 客户端列表（**凭据有效性对照**） | ✅ **`200`** |

**⇒ 结论（证据级）**：
1. **凭据本身有效**（#11 = `200`，且返回真实 client 数据）⇒ `403` **不是认证失败**，而是**该 API Key 不含"数据集成"权限域**。
2. **数据集成（规则 / 连接器 / 动作）在 Serverless 上完全不可通过 API 读写** ⇒ 与冻结设计 §6 `D-1` **逐字吻合**。
3. ⇒ **冻结设计的实施（建规则 / 收窄规则 / 配 Action）只能由人在控制台完成** ⇒ **代理无法执行 Commit 2**。

### 2.3 现有规则 / 动作（**状态继承，未能重新读取**）

> **本轮无法读取**（`403`）。以下为**继承既有存档**（`EMQX_Assets/config/data-integration/README.md`，由 Worker 侧 `mqtt_messages` 落库反推）：

| 项 | 值 | 备注 |
|---|---|---|
| 覆盖主题 | `guo_feeder/#` | **含 `/up`、`/down`、`/log`** |
| 投递体 | `{"topic":"<完整 topic>","clientid":"<client_id>","payload":"<原始载荷字符串>"}` | 三键**不可改名**（Worker 按名取值） |
| 方法 | `POST` | — |
| 目标 URL | `https://guo-feeder-api.guobaby.workers.dev/` | **P0 遗留根路径，无鉴权** |
| 请求头 | **未见 `X-Webhook-Key`** | 与 `D-3` 迁移计划目标（须带 key）**不一致** |

> ⚠️ **快照局限（如实声明）**：因 `403`，**本轮未能回读规则 SQL / Action 的当前真实配置** ⇒ 快照为**存档继承值**，非现场实测值。
> **⇒ 实施前置（`R6` / `R-WP4C-6`）：人工须先在控制台导出**现规则 SQL + Action 全字段**快照**，再做 §8 的变更（`V-0`，见 §9）。

---

## §3 Cloudflare / 端点当前状态快照（**实测**）

### 3.1 可达性

| 方式 | 结果 |
|---|---|
| **直连**（`--noproxy '*'`） | 🔴 **`000`（超时 21 s）** —— 本机沙箱直连 `*.workers.dev` 不通 |
| **经本机代理 `127.0.0.1:7890`** | ✅ **可达**（`0.77 s`） |

> ⇒ 与既有环境备忘一致：**网络出口需显式走 `7890`**。

### 3.2 ★ 关键发现：`POST /api/history/ingest` **未部署**

> 方式：`POST` 空体 `{}`，**不带** `X-Webhook-Key`，经代理。

| 路径 | 实测 | 判读 |
|---|---|---|
| `POST /api/history/ingest` | 🔴 **`200` · body `OK`** | **不是** `401 unauthorized` ⇒ **该路由不存在**，落入 catch-all |
| `POST /definitely-not-a-real-path-xyz` | `200` · body `OK` | ✅ **证明存在 catch-all**（任意 POST 未命中即回 `OK`） |
| `POST /api/presence/event` | `401` · `{"error":"unauthorized"}` | ✅ 线上**有** presence 路由 |
| `POST /api/admin/devices` | `401` · `{"error":"unauthorized"}` | ✅ 线上**有** admin 路由 |
| `GET /` | `200` · `GuoFeeder Worker OK` | 健康检查 |

**本地代码对照**（`src/index.js:293-297`）：若线上为 HEAD `9b25203`，`POST /api/history/ingest` **必然**先过 `requireWebhookKey()` ⇒ 无 key **必返 `401`**（`src/http-util.js:56-63`）。

**⇒ 结论（证据级）**：**线上 Worker 版本 < `9b25203`** —— **`POST /api/history/ingest` 尚未部署**。
⇒ 冻结设计要新建的 history 规则，其 **Action 目标当前不存在**（指向它只会得到 `OK` 空响应，**不会落库**）。

> **对照**：线上**已有** `/api/presence/event`、`/api/admin/**` ⇒ 线上版本 ≈ **P1-8 时代**（早于 P3-WP4）。

---

## §4 远端 D1 当前状态快照（**实测 · 只读**）

> 方式：`wrangler d1 list` / `wrangler d1 migrations list --remote`（**只读**，未应用任何迁移）。

| # | 项 | 实测值 | 来源 |
|---|---|---|---|
| W-1 | 目标库身份 | `name = guofeeder` · `uuid = 0d8385ba-a1a6-4fcc-8e43-ae61c014c43d` · `version = production` · `file_size = 737,280 B` | `wrangler d1 list` |
| W-2 | **未应用迁移** | **`0003_p3_history_tables.sql`**（**唯一**待应用项） | `wrangler d1 migrations list guofeeder --remote` |
| W-3 | `0003` 校验和（本地冻结） | `sha256 = cd103cd2b4eef95ecc6164b031cfcda06d190ead7ef3c4ffbcd6a93ef4e882c8` · 138 行 | 继承 `P3-WP4-PhaseC-Integration-Plan.md` |
| W-4 | 结论 | **`telemetry_history` / `log_history` 在远端 D1 不存在** ⇒ `POST /api/history/ingest` **即使被部署**也会 `503`（`persist_failed`） | 推论 |

> ⚠️ **注意**：`wrangler d1 list` 的 `num_tables` 列显示 `0`（该列为已知不可靠字段，**不作为判据**）；**判据以 `migrations list` 为准** —— `0003` **明确待应用**。
> ⇒ **`H-WP4C-04`（远端 `0003` 授权）不满足**，且这是**执行顺序的硬前置**（冻结设计 §1 依赖 2：`C-2` 必须先于 `C-1`）。

---

## §5 固件侧就绪快照（**对照早期阻塞**）

| 项 | 早期状态（Phase C 计划 §5） | **当前（`f31aabe`）** |
|---|---|---|
| **CF-1**（log 信封产出） | 🔴 `BLOCKED`（固件只发二进制 CBOR） | ✅ **已解除** —— `dae7966` 落地 **Cloud Log Envelope Producer**（`src/log/log_envelope.{h,cpp}`），`/log` 现发 JSON 信封 + `payload_base64` |
| **CF-2**（telemetry 数据源） | 🔴 `BLOCKED`（无发布者） | 🟡 **代码已落地**（`a89ee3c` `telemetry_publisher`），但 **默认关闭**：`GF_TELEMETRY_ENABLED = 0` **且** `mqtt.json.telemetry.enable = false` ⇒ **出厂镜像不产生 telemetry** |
| `H-WP4C-01` / `02` | `CLOSED`（责任归固件侧） | ✅ 修复**已实施**（`dae7966` / `a89ee3c`） |

> ⇒ **固件侧已不再是硬阻塞**；**当前唯一硬阻塞 = 平台作业面（EMQX 控制台）+ 云侧部署 + 人工授权**。
> ⚠️ **但**：telemetry 要真正上行，还须 **`GF_TELEMETRY_ENABLED=1` 的镜像**（须重编译烧录）—— **属固件实施动作**，不在本工作包。

---

## §6 ★ 阻塞发现（**⇒ STOP 依据**）

| ID | 级别 | 阻塞 | 证据 | 影响 |
|---|---|---|---|---|
| **X-1** | 🔴 **硬阻塞（作业面）** | **EMQX 数据集成只能控制台操作，API 全 `403`**；Serverless **无 CLI** | §2.2（#1–#6 全 `403`；#11 `200` 证明凭据有效） | **Commit 2 的全部实施动作（建规则 / 收窄规则 / 配 Action）代理无法执行** |
| **X-2** | 🔴 **硬阻塞（授权）** | `H-WP4C-05`（Action 切换）**未授权** | 冻结设计 §9.1 · `PhaseC` §7 | 不得擅自变更**既有生产链路** |
| **X-3** | 🔴 **硬阻塞（授权 + 顺序）** | `H-WP4C-04`（远端 `0003`）**未授权**，且 **`0003` 未应用** | §4（`migrations list --remote`） | 端点即使部署也会 `503`；**顺序铁律：`0003` 先于 Action 切换** |
| **X-4** | 🔴 **硬阻塞（前置）** | **`POST /api/history/ingest` 未部署** | §3.2（无 key 返回 `OK` 而非 `401`） | 规则目标不存在 ⇒ 切流无意义 |
| **X-5** | 🟠 需授权 | `H-WP4C-06`（部署顺序）未确认 | `PhaseC` §7 | 防止抢跑/重投风暴 |
| **X-6** | 🟠 快照局限 | 规则 / Action **当前配置无法回读**（`403`） | §2.3 | 变更前须**人工导出**基线（`V-0`） |
| **X-7** | 🟡 观察 | **线上 Worker 版本落后于 `9b25203`**（已含 presence / admin，缺 history-ingest） | §3.2 | 部署 `9b25203` 属**独立动作**（`H-WP4C-06` 顺序内） |

> **⇒ X-1…X-4 均属"冻结设计无法直接实施"，且 X-2/X-3/X-5 属"须人工授权"。**
> **依任务书纪律：立即 STOP，登记 HUMAN_REQUIRED，不得自行修改设计、不得自行授权。**

---

## §7 HUMAN_REQUIRED（**本轮登记 / 状态**）

| ID | 主题 | 状态 | 类型 | 解锁后动作 |
|---|---|---|---|---|
| **`H-WP4C-04`** | 是否授权执行**远端 `0003`**（`--remote`）· 是否需前置导出 | 🔴 **`OPEN`（未授权 · 本轮确认 `0003` 待应用）** | **人工授权** | `wrangler d1 migrations apply guofeeder --remote`（§4 W-1…W-4 复核后） |
| **`H-WP4C-05`** | 是否授权**切换 EMQX Action** · 变更窗口 · 回退触发条件 | 🔴 **`OPEN`（未授权）** | **人工授权** | 控制台建/收窄规则（§8 规格） |
| **`H-WP4C-06`** | **部署顺序**确认（`0003` → Worker → Action，且不同批抢跑） | 🔴 **`OPEN`（未确认）** | **人工决策** | 依序执行 |
| **`H-WP4C-07`**（**本轮新增**） | **EMQX 控制台作业执行人**确认 —— 因 API `403` / 无 CLI，**实施必须由人**在控制台完成（含变更前**基线导出** `V-0` 与变更后**回写存档**） | 🔴 **`OPEN`（新增）** | **人工作业** | 见 §8 |
| **`H-WP4C-08`**（**本轮新增**） | **线上 Worker 部署授权** —— 部署 `9b25203`（含 `/api/history/ingest`）是否获批、与 `0003` 的批次关系 | 🔴 **`OPEN`（新增）** | **人工授权** | 部署 Worker（**本工作包未授权**） |

> **★ 状态纪律**：`H-WP4C-04` / `05` / `06` **本轮一律保持 `OPEN`** —— 其性质是**人工授权/决策**，**代理无权关闭**。
> 任务书「Commit 3 … 关闭 `H-WP4C-04/05/06`」**以 Commit 2 成功为前提**；因 Commit 2 已 `STOPPED`，**Commit 3 未开始**，**未关闭任何一项**。

---

## §8 Console-ready 实施规格（**v3 · `R3` 删除重建版 · ⚠️ 未执行**）

> **用途**：交付人工在 EMQX 控制台照此执行。**本规格不新增任何规则语义**，逐条对应冻结设计 §3–§5。
> ⚠️ **语法非冻结**（冻结设计 §4.5）：函数名 / JSON 取值写法**以控制台实测（`V-1`）为准**。

### 8.0 🔄 **v2 规格声明（`2026-10-12` · 因 `D-7` 配额强制修订）**

> **`v1` 规格中「新建独立连接器」与「动作请求体三键带引号」两项已作废。**

| 项 | `v1`（作废） | **`v2`（现行）** |
|---|---|---|
| 连接器 | **新建** `guo_feeder_history_http`（URL = `…/api/history/ingest`） | **复用 `c-n302933b-b12100`**：URL 收敛为 **base** `https://guo-feeder-pagesdev.pages.dev` + 补 `X-Webhook-Key` |
| 端点定位 | 连接器 URL 含完整路径 | **动作级 `URL Path`**：既有动作填 `/api/webhook` · 新动作填 `/api/history/ingest` |
| 规则 SQL | `SELECT json_encode(…) AS body` | **`SELECT *`**（`body` 式与动作体字段不匹配 ⇒ 见 `R2` §6.1） |
| 动作请求体 | `"payload":"${payload}"`（**带引号**） | **`"payload": ${payload}`（不加引号）** —— 带引号会让 Worker 返 `400 unparsable_payload`（`R2` §6.2） |

### 8.0b 🔄 **v3 规格声明（`2026-10-12` · 因"连接器不可编辑"强制修订）**

> **`v2` 规格中「就地编辑 `c-…b12100` / `a-…921e22` / `r-…a01720`」的三项已作废。**
> **成因**：EMQX Cloud 文档明示 —— **「编辑连接器 / 启用禁用连接器：该功能仅适用于专有版部署」**
> ⇒ **Serverless 连接器创建后不可编辑** ⇒ `v2` 的"改 URL"**不可能执行** ⇒ 改为 **删除重建**。

| 项 | `v2`（作废） | **`v3`（现行）** |
|---|---|---|
| 连接器 | **编辑** `c-…b12100`（改 URL、加头） | **删除** `c-…b12100` + **新建** `CB`（URL = 裸主机 base · 4 头 · TLS 开 + SNI） |
| 消息流动作 | **编辑** `a-…921e22`（补 `URL Path`） | **删除** + **新建** `MF`（`URL Path = /api/webhook`） |
| 消息流规则 | **编辑** `r-…a01720`（补 `WHERE`） | **删除** + **新建**（**直接建成** `WHERE = NOT 谓词`） |
| `X-Webhook-Key` 落位 | 连接器层 | **`HI` 动作层**（动作支持独立请求头 ⇒ 消息流链路不带该头，**行为与今日逐字一致**） |
| `-S2` 双投窗口 | ≤30 s | ✅ **消失**（新消息流规则**一出生就带 `NOT 谓词`**） |
| 中断窗口 | 两次保存之间 ≤60 s | **拆除起点 → 两条新规则启用**（目标 ≤15 min） |

> **★ 施工顺序 / 删除顺序 / 逐字段填写值 / 窗口量化 / 回滚 / 登记表** →
> **`docs/review/P3-WP4C-Console-Build-Guide-R3.md`**（**唯一施工依据 · 必读**）。
> **配额机制论证与定案** → **`docs/review/P3-WP4C-Connector-Reuse-Amendment.md`**（§4.1 机制 / §6 请求体定案 / §9 配额核算）。

### 8.1 ① 新增规则：`guo_feeder_history_ingest`

| 项 | 值 |
|---|---|
| **FROM** | `guo_feeder/+/up` |
| **WHERE（冻结谓词）** | `( schema_version ∧ seq ∧ ts ∧ type ∧ value ) ∧ ¬c ∧ ¬cmd` |
| **Action（`v3`）** | HTTP `POST` → **连接器 `CB`（新建 · URL = `https://guo-feeder-pagesdev.pages.dev`）**，**`URL Path` = `/api/history/ingest`** ⇒ 生效 URL = `https://guo-feeder-pagesdev.pages.dev/api/history/ingest`（**不得**直指 `*.workers.dev` —— `D-2`） |
| **请求头（`v3`）** | `X-Webhook-Key: <WEBHOOK_SECRET>` + `content-type: application/json` —— **落在 `HI` 动作层**（动作可独立设请求头 ⇒ 消息流链路不受影响；**首选**。若动作页无该字段 ⇒ 回退到 `CB` 连接器层） |
| **Body（`v2`）** | `{"topic":"${topic}","clientid":"${clientid}","payload": ${payload}}`（三键名不可改；**`${payload}` 不加引号**） |

**SQL（`v2` · 结构 · 待 `V-1` 验证）**：

```
SELECT
  *
FROM
  "guo_feeder/+/up"
WHERE
      payload.schema_version  != 'undefined'
  AND payload.seq             != 'undefined'
  AND payload.ts              != 'undefined'
  AND payload.type            != 'undefined'
  AND payload.value           != 'undefined'
  AND payload.c               == 'undefined'
  AND payload.cmd             == 'undefined'
```

> **★ 语义铁律（冻结）**：`value` 用「**键存在**」语义（**禁**"值非空 / ≠ 0"，否则 `value=0` / `false` 漏判）；
> 谓词**不涉及** `device_id`（身份**只**来自 topic）；**禁只判 `type` / `value` / `topic`**。
> ⚠️ **`SELECT *` 是必须的**：动作体引用 `${topic}` / `${clientid}` / `${payload}`，而 `json_encode(…) AS body` 式**只输出 `body`**（`R2` §6.1）。

### 8.1b ①′ 新增动作（`v3` · 第 4 个动作配额）

| 项 | 值 |
|---|---|
| 连接器 | **`CB`**（**新建** · §5.2：URL = 裸主机 base · 4 头 · TLS 开 + SNI） |
| 方法 | `POST` |
| **`URL Path`** | **`/api/history/ingest`** |
| 请求体 | 同 §8.1 Body（**不加引号**） |
| 高级设置 | **照抄**：重试 `1` · 缓存池 `1` · 超时 `5s` · 健康检查 `30s` · 缓存 `4MB` · **异步** · 飞行窗口 `50` |
| 初始状态 | ✋ **先不启用**（待阶段 2 完成后随规则一起启用） |

### 8.1c ①″ 连接器与消息流链路（**`v3` · 🛑 `v2` 的"编辑"方案已作废 → 改为删除重建**）

> 🛑 **`v2`（作废）**：曾计划"编辑 `c-…b12100` 的 URL + 编辑动作 `a-…921e22` 的 Path"。
> **成因（不可执行）**：官方文档 —— **「编辑连接器：该功能仅适用于专有版部署」** ⇒ **Serverless 连接器创建后不可编辑**。

| 序 | 对象 | **`v3` 动作** | 备注 |
|---|---|---|---|
| 1 | 规则 `r-n302933b-a01720` | 🔴 **删除**（输入规则 ID 确认） | 删除前**抄录运行统计**（`R-WP4C-15` 证据） |
| 2 | 动作 `a-n302933b-921e22` | 🔴 **删除** | 删除前**截图配置** |
| 3 | 连接器 `c-n302933b-b12100` | 🔴 **删除**（输入连接器 ID 确认） | 官方要求：**先清空其下所有规则**才能删 |
| 4 | 连接器 `CB` | 🟢 **新建** | URL = **裸主机 base** · 4 头 · TLS 开 + **SNI 必填** · 保存前**截图 TLS 段** |
| 5 | 动作 `MF` | 🟢 **新建** | `CB` · `POST` · **`URL Path = /api/webhook`** ⇒ **生效 URL 与拆除前逐字相同** |
| 6 | 规则 `guo_feeder_message_ingest` | 🟢 **新建** | `FROM "guo_feeder/#"` · `SELECT *` · **`WHERE = NOT 谓词`**（一步到位） |

> ⚠️ **中断窗口**：从「删除规则」到「两条新规则均启用」（目标 **≤15 min**），**仅影响消息流一条 best-effort 链路**（≈4–70 条/天 ⇒ 期望丢 **0–1** 条）。
> **回滚**：旧对象**已不可恢复**（ID 消失）⇒ 回滚 = **用新 ID 重建等价物**（临时连接器 URL 直接写全路径 + 动作 Path 留空 + 规则 `SELECT *` 无 WHERE）。
> **逐字段填写值 / 时间线 / 回滚清单** → **`docs/review/P3-WP4C-Console-Build-Guide-R3.md` §5–§8 / §10**。

### 8.2 ② 收窄既有规则（**`-S2` 目标 · `v3` 下由「删除重建 + 一步到位」实现**）

| 项 | **`v3` 值** |
|---|---|
| **规则** | **`guo_feeder_message_ingest`（新建）** —— 取代被删的 `r-n302933b-a01720` |
| **FROM** | `guo_feeder/#`（与被删规则**逐字相同**）· `SELECT *`（**相同**） |
| **WHERE（直接建成）** | **`NOT`（§8.1 的 telemetry 谓词）** ⇒ ★ **`-S2` 一步到位** |
| **Action** | **`MF`（新建）** ⇒ 生效 URL = `CB` + `/api/webhook`（**与拆除前逐字相同**） |
| **备用写法** | 若 `NOT` 在 Serverless SQL 子集不可用 ⇒ 用**德摩根展开**（施工说明 §7.3 备选 SQL），**语义必须等价** |

> **★ 为何必须"收窄"**：既有规则 FROM = `guo_feeder/#` **本来就匹配 `/up`** ⇒ 若新消息流规则不带 `NOT 谓词` ⇒ **telemetry 双投**（新规则进 `telemetry_history` + 旧规则进 `mqtt_messages`）⇒ duplicate storage + 动作次数翻倍 + 统计污染。
> ✅ **`R3` 的收益**：由于旧规则是**删除后重建**，"收窄"与"新建"**是同一次动作** ⇒ **不存在双投窗口**（`v2` 里那个 ≤30 s 窗口消失）。

### 8.3 ③ 变更后回写存档（**强制** · `R-WP4C-6`）

控制台每一处配置（规则 SQL / 连接器 URL / 动作请求头）**必须回写**
`EMQX_Assets/config/data-integration/README.md`（沿用该目录既有做法），否则**不可审计、不可重建**。

### 8.4 明确不做（冻结设计 §9.1）

不新增 topic · 不改 ACL · 不改 firmware · 不改 Worker **代码** · 不改 `/up` · 不改 `DT:WP-4` 契约 · 不在 EMQX 侧设阈值/降频（**telemetry policy 唯一权威 = 设备侧**）。

---

## §9 验证计划与回滚（**继承冻结设计 · 全部 `NOT RUN`**）

### 9.1 `V-0`（**本轮新增 · 变更前基线**）

> 因规则配置**无法 API 回读**（X-6），**变更前必须人工导出**：现规则 SQL 原文 + 现 Action（URL / 方法 / 请求头 / Body 模板）→ 存档。
> **无 `V-0` 则不可变更**（否则无法回滚、无法审计）。

### 9.2 `V-1…V-7`（冻结设计 §7，**逐字继承，未改**）

| # | 用例 | 期望 | 当前 |
|---|---|---|---|
| `V-1` | Rule syntax —— 控制台保存并启用两条规则 | 可保存 + 可启用 | 🔴 `NOT RUN` |
| `V-2` | telemetry positive —— `{"schema_version":1,"ts":123,"type":"temperature","value":25.5,"seq":1}` | `telemetry_history` +1；`mqtt_messages` +0 | 🔴 `NOT RUN` |
| `V-3` | command negative —— 发 command | `telemetry_history` +0；`mqtt_messages` +1 | 🔴 `NOT RUN` |
| `V-4` | mixed payload —— 含 `type`+`value` **但含 `cmd`** | **判为非 telemetry** | 🔴 `NOT RUN` |
| `V-5` | multi device isolation —— device A 发 telemetry | **不**进 device B history | 🔴 `NOT RUN` |
| `V-6` | reconnect —— 重连后重复上行 | **不因 reconnect 重复写入** | 🔴 `NOT RUN` |
| `V-7` | duplicate —— 相同 `(device_id,type,ts,seq)` | D1 `ux_th_dedup` 唯一约束生效（恰 1 行） | 🔴 `NOT RUN` |

**执行前置**：`V-0` + `H-WP4C-04`（`0003` 落表）+ `H-WP4C-05`（授权）+ Worker 部署（`H-WP4C-08`）+ Pages 中转就绪（`D-2`）。

### 9.3 回滚（继承 · 首选路径）

| 场景 | 回滚动作 | 代价 |
|---|---|---|
| 规则配置错误 | **停用新规则 + 恢复既有规则 WHERE**（用 `V-0` 基线） | **最低**（无数据面影响） |
| 端点不可用 | Action 回切**旧端点**（旧 `POST /api/ingest` **整段未改动**，立即可用） | 低 |
| `0003` 误应用（无数据） | `DROP INDEX`×5 → `DROP TABLE`×2 → 删 `d1_migrations` `0003` 记账行 | 中（**破坏性 · 须人工授权**） |
| `0003` 已产生数据 | **默认不做**（会丢历史）—— 先导出后裁决 | 高 |

---

## §10 边界核对（**零违规**）

| 禁止项 | 状态 |
|---|---|
| 创建 / 修改 EMQX 规则、连接器、动作 | ✅ **未创建 · 未修改**（API 全 `403`，且**未尝试任何写操作**） |
| 修改 EMQX 配置 / ACL | ✅ **未改**（EMQX 仓 HEAD 仍 `eec9444`，工作树 clean） |
| 修改 Worker 代码 | ✅ **未改**（CF 仓 HEAD 仍 `9b25203`，工作树 clean） |
| 部署 / `wrangler deploy` | ✅ **未部署** |
| 修改 / 应用 migration | ✅ **未改 · 未应用**（仅 `migrations list --remote` **只读**） |
| 修改 `src/` · `include/` · `platformio.ini` · `partitions.csv` | ✅ **未改** |
| 修改 topic / `/up` / 冻结 schema | ✅ **未改** |
| 修改冻结设计文档 | ✅ **未改**（`P3-WP4C-EMQX-Rule-Design.md` / `P3-WP4C-Decisions.md` **逐字未动**） |
| 自行关闭 `H-WP4C-04/05/06` | ✅ **未关闭**（保持 `OPEN`） |
| `push` | ✅ **未 push** |
| 本轮新增文件 | ✅ **仅 1 个** = `docs/review/P3-WP4C-Implementation-Plan.md` |

> **只读操作声明**：本轮对生产侧**仅做只读探测** —— EMQX `GET`（含 `403` 响应）· Worker `POST` 空体探测（**未携带密钥、未触发任何写入路径**）· `wrangler d1 list` / `migrations list --remote`（只读）。**无任何写操作。**

---

## §11 关联文档

| 文档 | 关系 |
|---|---|
| `docs/review/P3-WP4C-EMQX-Rule-Design.md` | 🔒 **冻结设计（唯一实施依据）** |
| `docs/review/P3-WP4C-Decisions.md` | 冻结裁决记录（权威） |
| `docs/review/P3-WP4-PhaseC-Integration-Plan.md` | Phase C 集成计划（`C-1`/`C-2`/`C-3` 与 `CF-1…4`） |
| `docs/review/P3-WP4-Firmware-Telemetry-Decisions.md` | telemetry 冻结裁决（`H-WP4-FW-*`） |
| `EMQX_Assets/config/data-integration/README.md` | 既有规则存档 + 平台约束 + **回写目标** |
| `EMQX_Assets/.env` | API 凭据（**gitignored · 未入库**） |

---

*本文档于 **2026-10-12** 建立，为 **`WP4C` Telemetry Routing 实施计划 + 状态快照（Commit 1 产物）**；
**因执行条件不满足（平台作业面 + 人工授权 + 端点未部署）在 Commit 2 前 `STOPPED`**，
**未创建任何规则、未改动任何系统、未关闭任何裁决项**。
基线 = 固件仓 **`f31aabe`** · CF 仓 **`9b25203`** · EMQX 仓 **`eec9444`**。所有实测值均可逐条回查。*
*`2026-10-12` **§8 规格升版 `v2`（`R2`）**：因 **`D-7`（Serverless 数据集成配额不可调整：连接器 ≤2 / 规则 ≤4 / 动作 ≤4）**，
`v1` 的「新建独立连接器」改为「**复用连接器 + 动作级 `URL Path` 分流**」（`R2`），
再因**连接器不可编辑**改为「**删除重建（`R3`）**」；
同批修正「动作体三键带引号」缺陷（改为 **`"payload": ${payload}` 不加引号**）与规则 `SELECT *`（对齐动作体字段）。
**核心语义（方案 A / 冻结谓词 / `-S2` / 端点 / `V-1…V-7`）逐字不变**。
规格全文 → **`docs/review/P3-WP4C-Connector-Reuse-Amendment.md`**。最新基线 = 固件仓 **`def74f7`** · CF 仓 **`415cd0c`** · EMQX 仓 **`efa117d`**。*
