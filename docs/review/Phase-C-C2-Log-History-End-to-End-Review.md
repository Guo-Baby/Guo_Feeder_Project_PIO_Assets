# Phase C-C2 —— `/log` → `log_history` 端到端收口 · 实施前审查（`REVIEW ONLY`）

> ## 🟡 `REVIEW ONLY` —— 本文档**仅审查与规划**，**不执行任何生产变更**
>
> - **不做**：不改 EMQX 规则 / 动作 / 连接器 · 不改固件代码 · 不改 Worker · 不建 / 改 D1 migration · 不部署 · 不 push。
> - **前置事实**：`WP4C = CLOSED`（`CF-2`）· **`Phase C-C1`（遥测发布器 + `HD-1` 运行时门）已完成、上板验证并 push**（固件仓 HEAD `73bf330`）。
> - **判定**：⚠️ **`CONDITIONAL GO / BLOCKED-1`** —— **唯一阻塞 = EMQX 缺 `/log` 分流，而规则配额 `4/4` 已满** ⇒ 需**人工裁决路由方案**后方可实施。
> - **本文档定位**：把「已就绪的固件信封 + 已就绪的 Worker 端点」推进到「`/log` 真实落 `log_history`」所需的**集成决策与验证计划**。

| 项 | 值 |
|---|---|
| **状态** | ✅ **`Phase C-C2 = CLOSED`（`2026-10-16`）** —— 施工已完成 · `V-3.1…V-3.9` 全 `PASS` · 收口报告 `Phase-C-C2-Final-Closure-Report.md`（历史：`PLANNED / BLOCKED` → `REV-3` `PRE-FLIGHT COMPLETE / BLOCKED` → **`CLOSED`**） |
| **基线** | 固件仓 **`1ebae81`**（收口前）· EMQX 仓 **`3165aab`** · Cloudflare 仓 **`e328a11`**（收口前） |
| **对应** | **Phase C 集成计划 `C-1 · V-3（log path）`**（`P3-WP4-PhaseC-Integration-Plan.md` §2.2） |
| **门禁** | `G-1 HUMAN_REQUIRED` · `G-4 BLOCKED` **均未变**（属 P3 计划门禁，与 C2 收口无耦合） |
| **生产变更** | 本文件**零**；施工由**人工**在 EMQX 控制台完成（改 **2 条规则** · 配额 `4/4` 不变） |

> 🔄 **决策更新（`2026-10-15`）**：**`HR-C2-1` = ✅ 方案 B（改规则 A + B · 配额中立）** 已裁决。
> 落地方案 → **`docs/review/Phase-C-C2-Log-Routing-Console-Task-Sheet.md`**（控制台施工任务单 · `PLAN ONLY` · **`REV-2`** · ~~待人工审核 `HR-C2-4/5/6/7`~~ → ✅ **已全部收口（`2026-10-16` · 见 §12）**）：解除冻结约束 / `log_predicate` 定义 / 施工顺序与作业授权 / **`/log` 排除策略（策略 A）+ 候选 SQL**。
> 本文件的 §5 方案评估、§6 配额分析、§7 验证矩阵、§12 裁决表**仍为有效依据**；`HR-C2-2` 因未选 A 而**不适用**。
>
> 🔴 **`REV-2` 关键修订（`2026-10-15` · 任务单 §10 对照）**：
> - **问题二** —— 原任务单「两 predicate 字段集不相交 ⇒ 必然互斥」**论证作废**（`seq` / `ts` 交集存在，混合信封理论可同时满足）⇒ 改为**分层组合依据**（真实 topic + envelope 契约 + 规则逻辑 + Worker 按 topic 域选表 · 任务单 §2.4），**不宣称绝对安全**。
> - **问题三** —— `/log` 排除**由「排除符合 log_predicate 的消息」（= 策略 B）改为「按 topic domain 排除」（策略 A）** ⇒ 所有 `guo_feeder/+/log` 一律不进旧路径；候选 SQL **待控制台语法验证**（任务单 §2.2.2 / §2.2.3）。
> - **问题四** —— 验证分层 **L0 推演 / L1 模拟 / L2 真实 MQTT**；**仅 L2 为验收依据**；不可执行项标 `NOT RUN` / `BLOCKED`（任务单 §4）。
> - **过渡期双写** —— 人工**已接受**且**不要求改 A→B 顺序**；文档仍如实说明，**不再作为阻塞理由**（任务单 §3.1）。
>
> 🔴 **`REV-3` 施工前只读核查（`2026-10-15` · 任务单 §11 对照）**：
> - **`LIVE CONFIGURATION: NOT VERIFIED`** —— 代理无 EMQX 控制台 / 数据集成 API 只读权限（`403`）⇒ 规则 A/B 的 SQL **仅为存档转录**（任务单 §1.3）。
> - **策略 A 静态影响分析**（`CONFIRMED`/`INFERRED`/`BLOCKED` · 任务单 §2.5）：**Presence 解耦 = `CONFIRMED`**（`FROM` 无交集）· **topic 集穷举 = `BLOCKED`** ⇒ 白名单候选的回归风险登记为 **`HR-C2-7a/7b`**。
> - **★ 新发现 `F-C2-8`（P0 · 阻塞 `V-3.3`）**：**`level` 契约不一致** —— 冻结协议 / 固件信封 = **整数 `uint8`**，Worker `normalizeLog` = **字符串 `{WARN,ERROR,CRITICAL}`** ⇒ **真实 `/log` 记录将被逐条拒收** ⇒ `log_history` 恒 0（**即使路由修好**）⇒ 登记 **`HR-C2-8`**（任务单 §2.6）。
> - **验证判据补齐**：`V-3.4` 最终态隔离（关联字段 + `t0` 须在 B 之后 · 任务单 §4.5）· `V-3.7` 畸形 `/log` 七类矩阵（任务单 §4.6）· 未验证项汇总（任务单 §4.7）。

---

## §1 Objective

让**设备真实 `/log` 上行**（Cloud Log Envelope JSON）经 EMQX 投递到 **`POST /api/history/ingest`**，并在远端 D1 **`log_history` 落真实 +1 行**（幂等键 `(device_id, boot_seq, seq)`）——即闭合 **Phase C 的 `V-3`（log path）** 与 `CF-1` 的端到端环节。

---

## §2 Current baseline（只读核实 · 全部有据）

### 2.1 固件侧 —— **已就绪（`CF-1` 产出器无条件在役）**

| 项 | 事实 | 依据 |
|---|---|---|
| 产出器 | `src/log/log_envelope.{h,cpp}`（`freestanding`，主机可字节级验证） | `dae7966 = feat(firmware): add cloud log envelope producer`，**是 HEAD 的祖先** |
| 接入方式 | `log_manager.cpp` 的 `cloud_build_envelope()`（定义 `L2267` / **调用点 `L2758`**）**无条件取代**原「CBOR 批次」构造 ⇒ **当前构建的 `/log` 载荷 = JSON envelope**（非二进制） | 已核实（`cloud_build_cbor` 已不存在于 `log_manager.cpp`） |
| 信封字段（顺序即契约） | `{device_id, boot_seq, seq, ts, level, payload_base64}` | `log_envelope.h` §"Envelope（JSON，字段顺序即契约）" |
| `payload_base64` | = `log_cbor_encode_batch(count = 1)` 的 **188 B CBOR 原样 Base64**（252 字符；`LOG_ENV_*` 常量有 `static_assert`） | `log_envelope.h` L36–55 |
| 开关 | **无编译门 · 无运行时开关** ⇒ **不可单独关闭/回退**（见 §11 `F-C2-1`） | `grep` 全 `src/` 无 `GF_LOG_ENVELOPE*` |

> **推论**：**设备（`04aca12` 构建，含 `dae7966`）在产生日志时，`/log` 已在发 JSON envelope**。

### 2.2 EMQX 侧 —— 规则 / 动作现状（配额全满）

| 对象 | ID | `FROM` | `WHERE` | 动作 | 端点 |
|---|---|---|---|---|---|
| 规则 A `guo_feeder_history_ingest` | `r-n302933b-cf0cf2` | `guo_feeder/+/up` | **冻结 telemetry 谓词** | `HI` `a-…126ec6` | `/api/history/ingest` + `X-Webhook-Key` |
| 规则 B `guo_feeder_message_ingest` | `r-n302933b-f731cf` | `guo_feeder/#` | **谓词之否定** | `MF` `a-…7241e2` | `/api/webhook` |
| presence connected | `r-n302933b-de1540` | `$events/client_connected` | （裸 `${body}`） | `a-…999c12` | `/api/presence/event` |
| presence disconnected | `r-n302933b-7e8a01` | `$events/client_disconnected` | （裸 `${body}`） | `a-…0209ad` | `/api/presence/event` |

- **配额：连接器 `2/2` · 规则 `4/4` · 动作 `4/4`** —— **三项全满**（`P3-WP4C-V2V4-Config-After.md` §2.3b 配额核对 · `P3-WP4C-Console-Build-Guide-R3.md` §1 平台事实）。
- 规则 SQL 权威原文见 `P3-WP4C-V2V4-Config-After.md` §2.4 / §2.5（`R3.1` 首选写法）。

### 2.3 Cloudflare 侧 —— **`/log` 域已就绪**

| 项 | 事实 | 依据 |
|---|---|---|
| 端点 | `POST /api/history/ingest`（`X-Webhook-Key` 强制）· 早退路由，**不写 `mqtt_messages`** | `guo-feeder-api/src/index.js` · `history-ingest.js` |
| 域判定 | `TOPIC_DOMAIN = { up: "telemetry", log: "log" }` ⇒ **topic 第 3 段**决定域；未知 topic ⇒ `400 unknown_topic` | `history-ingest.js:47 / 203` |
| log 域必需字段 | `payload_base64`（或 `payload` 字符串承载）+ `ts` / `level` / `boot_seq` / `seq` | `history-ingest.js:372–433` |
| 落库 | `INSERT OR IGNORE INTO log_history (device_id, ts, level, code, boot_seq, seq, message, payload)`；`payload` **原样**（Worker **不解码** `payload_base64`） | `history-ingest.js:476` |

> **⇒ Worker 侧的信封字段契约与固件信封（§2.1）逐项匹配** —— 无需任何 Worker 改动。

### 2.4 旧端点 `/api/webhook`（现 `/log` 的去向）

- `POST /api/webhook`：`JSON.parse(body.payload)` → 写 **`mqtt_messages`**；**非 JSON ⇒ `400 Invalid JSON`**（`index.js:514–522 / 572`）。
- 二进制 `/log` 时代 ⇒ 400（`CF-1` 症状）；**信封 `/log` 时代 ⇒ 合法 JSON ⇒ 落 `mqtt_messages`（表错）**。

### 2.5 D1 基线

`telemetry_history = 0`（**待 `Phase C-C1` 真实遥测落库验证**）· **`log_history = 0`** · `mqtt_messages = 2458`（`2026-10-12` 施工前实测 · `V2V4` §0.6）。

---

## §3 ★ The single gap —— **路由**（其余环节全就绪）

```
设备 /log（JSON envelope：device_id/boot_seq/seq/ts/level/payload_base64）
        │
        ▼
   EMQX 规则 A（FROM "guo_feeder/+/up"）── ✗ 不匹配（/log 不在 FROM 内 · 信封亦缺 schema_version/type/value）
        │
        ▼
   EMQX 规则 B（FROM "guo_feeder/#" + 谓词之否定）── ✓ 匹配（信封无 schema_version ⇒ 否定为真）
        │
        ▼
   动作 MF → POST /api/webhook ── ✗ 错端点
        │
        ▼
   落 mqtt_messages（表错）── ✗ 永远到不了 /api/history/ingest
        │
        ✗  log_history 恒为 0
```

**结论**：
- **固件（§2.1）· Worker（§2.3）· D1（§2.5）均已就绪**；
- **唯一缺口 = EMQX 缺少把 `/log` 投递到 `/api/history/ingest` 的分流**；
- 而该分流**无法用"新增一条规则"实现** —— **规则配额 `4/4` 已满**（§2.2）。

> 🔴 **`REV-3` 更正（`2026-10-15` · **只报告，不重裁**）**：本节「**唯一缺口 = 路由**」的表述**已不完整**。
> 施工前只读核查发现**第二个、与路由独立的缺口** —— **`F-C2-8`：`level` 契约不一致**（§11）：
> 固件信封以**整数 `uint8`** 产出 `level`，而 Worker `normalizeLog` 要求**字符串 `{WARN,ERROR,CRITICAL}`** ⇒
> **即使路由完全修好，真实 `/log` 记录仍会被逐条拒收，`log_history` 恒为 0**。
> ⇒ 更准确的表述：**§3 的「路由」是 `V-3.3` 的「必要不充分」条件**；
> **`V-3.3` 的完整前置 = 路由修复 ∧ `F-C2-8` 处置**（后者 → **`HR-C2-8`** · 任务单 §2.6）。
> **本文档不自行改裁 `HR-C2-1`（方案 B）等既有决策**；该更正仅影响「验收能否达成」的判断。

> 该缺口与冻结设计**一致可预期**：`P3-WP4C-Console-Task-Sheet.md` §15 第 5 条已明示「未来固件 log 信封落地后…**届时需新增一条 log 域规则**（属未来工作包，现在不建）」—— **本包即该"未来工作包"**。

---

## §4 Scope boundary

**IN**：EMQX `/log` 分流（新增 / 改规则）· 端到端验证 `V-3` · 变更后存档回写。
**OUT**：固件代码改动（已就绪）· Worker 改动 · D1 schema / migration · `log_ack`（`计划:WP-5`，受 `G-4`）· ACL 改动 · 新 topic 引入 · telemetry 项（属 `Phase C-C1`）。

---

## §5 Route options（**均需人工裁决**）

| 方案 | 做法 | 规则配额 | 主要风险 | 冻结一致性 |
|---|---|---|---|---|
| **A · 新增 log 规则** | 新建 `FROM "guo_feeder/+/log"` → 动作 `HI`（`/api/history/ingest` + key） | **需释放 1 槽**（否则 `5/4` 超限） | 释放槽须动 presence / 消息流 | ✅ 与冻结预期一致（"新增 log 域规则"），但**配额使"直接新增"不可行** |
| **B · 改规则 A + B（配额中立）** | 规则 A `FROM` 扩为 `"guo_feeder/+/up","guo_feeder/+/log"`，`WHERE` 变 `telemetry_predicate OR log_predicate`；**同步**把规则 B 的否定扩为排除 log | **4/4（不新增）** | 改动**两条冻结规则**；若 B 未同步 ⇒ **双投** | ✗ 违反冻结「`FROM` / 谓词不动」 |
| **C · 退休规则 B** | 用 log 规则替换消息流规则（放弃 `mqtt_messages`） | 4/4 | 丢失 legacy `mqtt_messages` 落库（P0 遗留） | 需**设计级裁决**（退役 legacy） |
| **D · Worker 分流** | 在 `/api/webhook` 内识别 log 信封 → 写 `log_history` | 0 | 违反 Worker 边界（`H-WP4-05`）· `/api/webhook` **无鉴权** | ✗ |

**推荐**：
- **首选 `B`（配额中立）** —— 不新增规则、不退役 legacy，代价是改动冻结规则 A/B（需人工解除冻结约束）；
- **备选 `A`** —— 若人工可接受「合并 presence 两条为一条」或退役某条以释放槽位；
- **不推荐 `C` / `D`**。

---

## §6 ★ 规则配额问题（**本包的关键决策点**）

规则槽 `4/4` 已用满（A · B · presence ×2）。任一「新增规则」方案都须**先释放 1 槽**：

| 释放方式 | 做法 | 代价 |
|---|---|---|
| **A1 · 合并 presence 两条为一条** | 若 connected / disconnected 两动作体均为**裸 `${body}`** 且可共用 ⇒ 合并 `FROM "$events/client_connected","$events/client_disconnected"` → 1 规则 1 动作 | 触碰**在役** presence 链路（`CF-2` 刚收口，风险敏感） |
| **A2 · 退休消息流规则 B** | 放弃 `mqtt_messages`（legacy） | 设计级退役决策 |

⇒ **无论 A1 / A2 都是设计级改动 ⇒ 触发新的 `HUMAN_REQUIRED`**。

---

## §7 Verification matrix（`V-3` · 实施后方可执行）

| # | 步骤 | 判据（`PASS` 定义） | 证据 | 当前状态 |
|---|---|---|---|---|
| `V-3.1` | 设备发出 ≥1 条 `/log`（JSON 信封） | MQTT `guo_feeder/<device_id>/log` 出现 `{device_id,boot_seq,seq,ts,level,payload_base64}`；**记录 `boot_seq`/`seq` 供关联** | MQTT 订阅抓包 | ✅ `PASS` |
| `V-3.2` | 观测 Worker | `wrangler tail` 命中 **`/api/history/ingest`**（**非** `/api/webhook`）· **且记录响应 `rejected` 计数**（⚠️ `200` 本身非通过判据） | tail 日志 | ✅ **`PASS`（口径修订）** —— 响应体计数通道不存在 ⇒ 由 `V-3.7a` 本地真引擎等价取证 |
| `V-3.3` | D1 落库 | `log_history` **相对基线 +≥1 行**；`payload` **原样**（未解码）；幂等键唯一；`created_at` 由 D1 生成 | D1 查询（前后增量） | ✅ `PASS`（`0 → 13` · 原 `BLOCKED`） |
| `V-3.4` | 隔离 | `mqtt_messages` **不因 `/log` 增长**（**B 改完后** · 按 topic + **`(boot_seq, seq)` 联合键**关联 · **不看总行数**） | D1 查询（任务单 §4.5） | ✅ `PASS`（`id>3483` 的 `/log` 行 = `0`） |
| `V-3.5` | 鉴权不变 | 无 `X-Webhook-Key` ⇒ `401`（不放宽） | 探针 | ✅ `PASS`（本地真 D1 层套件用例 #10 · 鉴权层零改动） |
| `V-3.6` | 回归 | `/up` telemetry 仍落 `telemetry_history` · presence 仍 200 · 其余三端点不变 | 复测 | ✅ `PASS`（`telemetry_history` `+5` · 含 `schema_version` 的 `mqtt_messages` 行 = `0`） |
| `V-3.7` | 畸形 / 无效 `/log` 处置 | 按策略 A 逐类核对四要素 | tail + D1 + 本地隔离 | ✅ `PASS`（`a` 实测 · `b` 构造性 + 人工核实） |
| `V-3.8` | Presence 回归 | 真实上下线 ⇒ 落 `device_event` | tail + D1 | ✅ `PASS`（`187/216 → 189/218` · `+2`） |
| `V-3.9` | 命令下行 + webhook 回归 | 命令回包落 `mqtt_messages`；现行 webhook 通路行为不变 | tail + D1 | ✅ `PASS`（★ 实际路径 = **`POST /`**，非 `/api/webhook`） |

> ✅ **`2026-10-16` 收口**：验证矩阵已扩为 **`V-3.1…V-3.9`**（+ 畸形 `/log` 处置 / presence 回归 / 命令下行回归），
> 并补齐 `V-3.4`（任务单 §4.5）与 `V-3.7`（任务单 §4.6）判据；**全部 `PASS`**（`V-3.2` = 口径修订 · `V-3.7b` = 构造性 + 人工核实）。
> 收口报告 → **`Phase-C-C2-Final-Closure-Report.md`**。

> **观测纪律**：`/log` 仅在设备**产生日志**时上行 ⇒ 需构造触发（见 §10）或等待自然日志；不得以「窗口内无日志」判定 FAIL。

---

## §8 STOP conditions（**任一命中即停**）

1. **规则配额未释放** ⇒ **不得**尝试新增规则（控制台会拒绝 / 超限）；
2. **方案未经人工裁决**（`HR-C2-1/2`）⇒ 不得改动**冻结规则 A / B**；
3. 存在**双投**风险且未消除 ⇒ 不得启用；
4. D1 缺 `log_history` 基线对照 ⇒ 不得宣称 `PASS`；
5. 规则 SQL / 动作 URL **不可回读**（API `403`）⇒ 一律以 `V-0` / `V2V4` 存档为准，**不得猜测**；
6. 出现 `/up telemetry` 或 presence 回归 ⇒ 立即回滚至变更前（见 §9）。

---

## §9 Rollback / 安全网

| 层 | 回退手段 | 代价 |
|---|---|---|
| EMQX 规则 / 动作 | 控制台回切到变更前 SQL / URL（快照见 `V2V4` §2.4 / §2.5） | 最低 |
| 新增的 log 规则 | 删除即可（不影响 A / B） | 最低 |
| 远端 D1 | **无 schema 变更** ⇒ 无需回退（`log_history` 表已在） | — |
| 固件 | **本轮无固件改动** ⇒ 无需回退 | — |

---

## §10 可执行 / 必须人工

| 类别 | 项 |
|---|---|
| **可先做（只读 / HIL）** | 确认设备 `/log` 当前载荷形态（订阅 + 触发日志）· 记录 D1 计数基线 · 本文档 |
| **必须人工（不可代理）** | EMQX 控制台**规则改动**（API 对 rules / actions / connectors 全 `403`）· **配额释放裁决** |

---

## §11 Findings register（**仅登记，不修复**）

| ID | 级别 | 发现 |
|---|---|---|
| `F-C2-1` | 🟠 | 固件 log envelope **无编译门 / 无运行时开关** ⇒ 已随镜像生效、**不可单独回退**（回退 = 重烧） |
| `F-C2-2` | 🟠 | `/log` 现被规则 B 误路由至 `/api/webhook` ⇒ 落 **`mqtt_messages`（表错）**（信封为合法 JSON ⇒ 不再 400） |
| `F-C2-3` | 🔴 | 规则配额 **`4/4` 全满** ⇒ 「新增 log 规则」**不可直接实施** |
| `F-C2-4` | 🟢 | Worker `/api/history/ingest` 对 `/log` **已就绪**（`TOPIC_DOMAIN` / 字段契约 / 落库 SQL 齐备） |
| `F-C2-5` | 🟡 | `telemetry_history` 仍为 0 ⇒ **`Phase C-C1` 的真实遥测落库（`V-2`）亦待验证**（与本包并行项）—— **✅ 已闭合（`2026-10-14` · `V-2.1…V-2.6` 全 `PASS` · D1 `+8`）** |
| `F-C2-6` | 🟠 | **原计划的路由隔离论证有误**（任务单 `REV-1`）：「两 predicate 字段集不相交 ⇒ 必然互斥」**不成立**（`seq`/`ts` 为交集，混合信封理论可同时满足）⇒ `REV-2` 已改正为**分层组合依据**并登记 `BR-C2-1`/`BR-C2-2`（任务单 §2.4） |
| `F-C2-7` | 🟠 | **原计划的 `/log` 排除不完整**（任务单 `REV-1` 的 `NOT telemetry AND NOT log_predicate`）⇒ 缺字段 `/log` 仍可能进旧路径（= 策略 B）⇒ `REV-2` 改为**策略 A 按 topic domain 排除**（候选 SQL **待控制台验证** · 任务单 §2.2） |
| `F-C2-8` | 🔴 | **★ `level` 契约不一致（`REV-3` 发现 · P0）**：冻结协议 `cloud_protocol.md` §8.2.1 与固件 `log_envelope.cpp` 均以 **整数 `uint8`** 产出 `level`；而 Worker `history-ingest.js` 的 `normalizeLog` 要求 **字符串 ∈ `{WARN,ERROR,CRITICAL}`**（`asText` + `LOG_LEVELS`）⇒ **真实 `/log` 记录逐条被拒**（HTTP 200 + `rejected`）⇒ **`log_history` 恒 0**。**与 EMQX 路由无关**，但**决定 `V-3.3` 不可能 `PASS`**。处置 → **`HR-C2-8`**（独立问题 · 本任务不修）。<br>🔎 **独立核查已出**（`2026-10-15`）→ **`docs/review/F-C2-8-Log-Level-Contract-Assessment.md`**：判定 **`CONFIRMED`** · 契约争议 **`CQ-1/2/3`** · 与既有登记 **`F-WP4-FW-9`** 的关系（**其前提已被推翻，须升格重估**） |

---

## §12 Open Human Decisions → ✅ **全部收口（`2026-10-16`）**

| ID | 事项 | 选项 | 收口结论 |
|---|---|---|---|
| **`HR-C2-1`** | **`/log` 分流方案** | ✅ **已裁决 = B（改规则 A+B · 配额中立）**（`2026-10-15`） | 已实施 |
| **`HR-C2-2`** | 若选 A：**如何释放槽位** | 🚫 **不适用**（未选 A） | — |
| **`HR-C2-3`** | 是否接受 `/log` **现阶段落 `mqtt_messages`** | ✅ **已接受**（人工 `2026-10-15`） | 过渡双写已实测终止（`01:20:22`） |
| **`HR-C2-4`（新）** | 解除规则 A / B 的**冻结约束** | ✅ **事实已满足** | 规则 A/B 已改并生效 ⇒ 收口 |
| **`HR-C2-5`（新）** | **`log_predicate` 定义**批准 | ✅ **事实已满足** | 规则 A 已含 `log_predicate` 且真机命中 ⇒ 收口 |
| **`HR-C2-6`（新）** | **施工顺序 + 控制台作业授权** | ✅ **事实已满足** | A 先 B 后已执行 ⇒ 收口 |
| **`HR-C2-7`（`REV-2`）** | **`/log` 排除策略 = 策略 A（按 topic domain）** | ✅ **事实已满足** | 规则 B 按 topic 排除已生效（`V-3.4` = `0`）⇒ 收口 |
| **`HR-C2-7a` / `HR-C2-7b`（`REV-3`）** | 候选形态：`7a` 黑名单 / `7b` 白名单 | ✅ **采用 `7a`（黑名单）** | 生产规则 B = `NOT ( topic =~ 'guo_feeder/+/log' )`，**人工已核实** ⇒ 收口 |
| **`HR-C2-8`（`REV-3` · 🔴 P0）** | **`level` 契约不一致处置** | ✅ **已修并部署** | `F-C2-8` 修复随 Worker `dd5ab824` 上线 ⇒ 收口 |

> ✅ **人工确认事实归档**（文档收口动作 · 非新裁决）：EMQX 控制台**实际规则 A/B 已人工检查、规则正确**；AI 测试代码行为符合预期；
> **不再因 `403` 将规则 A/B 视为 `UNKNOWN`**（`LIVE CONFIGURATION: VERIFIED`）。

---

## §13 Deliverables / next → ✅ **已完成**

1. ✅ **本文（Review）** → 裁决 **`HR-C2-1 = B` · `HR-C2-3 = 接受`**（`2026-10-15`）；
2. ✅ **施工规格已出** → `Phase-C-C2-Log-Routing-Console-Task-Sheet.md`（`REV-3`）→ **人工控制台施工完成**（改规则 A / B · 2 条 · 配额 `4/4` 不变）→ 已回写存档（任务单 §12 + 本文件）；
3. ✅ 执行 **§7 验证矩阵 `V-3.1…V-3.9`** → **全部 `PASS`**；
4. ✅ 收口 `Phase C`（`C-1` 的 `V-2` telemetry + `V-3` log 双路径）；
5. ✅ **`HR-C2-8`（`F-C2-8` `level` 契约）已修复并部署** ⇒ `V-3.3` `BLOCKED` → `PASS`。

**★ 收口报告** → **`docs/review/Phase-C-C2-Final-Closure-Report.md`**（`Final Audit = PASS` · **`Phase C-C2 = CLOSED`**）。

---

> **Phase C-C2 = ✅ `CLOSED`（`2026-10-16`）.** 变更面 = **EMQX 2 条规则**（人工控制台）· **Worker `history-ingest.js`**（`F-C2-8`/`F-C2-9` 修复 · 已部署 `dd5ab824`）· 配额 `4/4` 不变.
> **`V-3.1…V-3.9` 全 `PASS`**（`V-3.2` = 口径修订 · `V-3.7b` = 构造性 + 人工核实）.
> **`LIVE CONFIGURATION: VERIFIED`**（人工核实规则 A/B）.
> **No Gate changed. `G-1 HUMAN_REQUIRED` · `G-4 BLOCKED` unchanged.**
> **后续工作（非本阶段未完成项）**：`BT-1`/`WP-5` `log_ack`（`O-1` = 其现网症状 · `MINOR`）· `V-3.2` observability enhancement · `HD-2`/`HD-3`/`P-3` · 历史噪音清理与 MQTT 密码轮换.

---

*本文件于 **`2026-10-14`** 建立，为 **`Phase C-C2`（`/log` → `log_history`）** 的**实施前审查**（`REVIEW ONLY`）；**`2026-10-15` 修订**（决策回填 + `REV-2`/`REV-3` 施工前核查更正）；**`2026-10-16` 收口**（`Phase C-C2 = CLOSED` · `V-3.1…V-3.9` 全 `PASS`）。*
*`REV-3` 基线 = 固件仓 **`859888c`** · EMQX 仓 **`3165aab`** · Cloudflare 仓 **`415cd0c`**；收口基线 = 固件仓 **`1ebae81`**（收口前）· EMQX 仓 **`3165aab`** · Cloudflare 仓 **`e328a11`**（收口前）；本文件**不含任何生产变更、代码改动或配置变更**。*
