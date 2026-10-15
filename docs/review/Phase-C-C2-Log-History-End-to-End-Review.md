# Phase C-C2 —— `/log` → `log_history` 端到端收口 · 实施前审查（`REVIEW ONLY`）

> ## 🟡 `REVIEW ONLY` —— 本文档**仅审查与规划**，**不执行任何生产变更**
>
> - **不做**：不改 EMQX 规则 / 动作 / 连接器 · 不改固件代码 · 不改 Worker · 不建 / 改 D1 migration · 不部署 · 不 push。
> - **前置事实**：`WP4C = CLOSED`（`CF-2`）· **`Phase C-C1`（遥测发布器 + `HD-1` 运行时门）已完成、上板验证并 push**（固件仓 HEAD `73bf330`）。
> - **判定**：⚠️ **`CONDITIONAL GO / BLOCKED-1`** —— **唯一阻塞 = EMQX 缺 `/log` 分流，而规则配额 `4/4` 已满** ⇒ 需**人工裁决路由方案**后方可实施。
> - **本文档定位**：把「已就绪的固件信封 + 已就绪的 Worker 端点」推进到「`/log` 真实落 `log_history`」所需的**集成决策与验证计划**。

| 项 | 值 |
|---|---|
| **状态** | **`Phase C-C2 = PLANNED / BLOCKED`**（唯一阻塞：`/log` 路由 + 规则配额） |
| **基线** | 固件仓 **`73bf330`** · EMQX 仓 **`3165aab`** · Cloudflare 仓（`/api/history/ingest` 已上线） |
| **对应** | **Phase C 集成计划 `C-1 · V-3（log path）`**（`P3-WP4-PhaseC-Integration-Plan.md` §2.2） |
| **门禁** | `G-1 HUMAN_REQUIRED` · `G-4 BLOCKED` **均未变** |
| **生产变更** | **零** |

> 🔄 **决策更新（`2026-10-15`）**：**`HR-C2-1` = ✅ 方案 B（改规则 A + B · 配额中立）** 已裁决。
> 落地方案 → **`docs/review/Phase-C-C2-Log-Routing-Console-Task-Sheet.md`**（控制台施工任务单 · `PLAN ONLY` · **待人工审核 `HR-C2-4/5/6`**：解除冻结约束 / `log_predicate` 定义 / 施工顺序与作业授权）。
> 本文件的 §5 方案评估、§6 配额分析、§7 验证矩阵、§12 裁决表**仍为有效依据**；`HR-C2-2` 因未选 A 而**不适用**。

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

| # | 步骤 | 判据（`PASS` 定义） | 证据 |
|---|---|---|---|
| `V-3.1` | 设备发出 ≥1 条 `/log`（JSON 信封） | MQTT `guo_feeder/<device_id>/log` 出现 `{device_id,boot_seq,seq,ts,level,payload_base64}` | MQTT 订阅抓包 |
| `V-3.2` | 观测 Worker | `wrangler tail` 命中 **`/api/history/ingest`**（**非** `/api/webhook`）· `200 {"accepted":1,…}` | tail 日志 |
| `V-3.3` | D1 落库 | `log_history` **+1 行**；`payload` **原样**（未解码）；幂等键唯一；`created_at` 由 D1 生成 | D1 查询 |
| `V-3.4` | 隔离 | `mqtt_messages` **不因 `/log` 增长** | D1 计数对照 |
| `V-3.5` | 鉴权不变 | 无 `X-Webhook-Key` ⇒ `401`（不放宽） | 探针 |
| `V-3.6` | 回归 | `/up` telemetry 仍落 `telemetry_history` · presence 仍 200 · 其余三端点不变 | 复测 |

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
| `F-C2-5` | 🟡 | `telemetry_history` 仍为 0 ⇒ **`Phase C-C1` 的真实遥测落库（`V-2`）亦待验证**（与本包并行项） |

---

## §12 Open Human Decisions

| ID | 事项 | 选项 |
|---|---|---|
| **`HR-C2-1`** | **`/log` 分流方案** | ✅ **已裁决 = B（改规则 A+B · 配额中立）**（`2026-10-15`） |
| **`HR-C2-2`** | 若选 A：**如何释放槽位** | 🚫 **不适用**（未选 A） |
| **`HR-C2-3`** | 是否接受 `/log` **现阶段落 `mqtt_messages`**（过渡噪声） | 🟡 建议接受（方案 B 下改完即消除 · 详见任务单 §7） |
| **`HR-C2-4`（新）** | 解除对规则 A / B 的**冻结约束**（冻结要求 `FROM`/谓词不动；方案 B 须改） | 🔴 待裁 → 任务单 §7 |
| **`HR-C2-5`（新）** | **`log_predicate` 定义**批准（5 键「存在」语义） | 🔴 待裁 → 任务单 §2.3 / §7 |
| **`HR-C2-6`（新）** | **施工顺序（A 先 → B 后）+ 控制台作业授权** | 🔴 待裁 → 任务单 §3 / §7 |

---

## §13 Deliverables / next

1. **本文（Review）** → **人工审核** → 裁决 **`HR-C2-1` / `HR-C2-2` / `HR-C2-3`** ⇒ ✅ **`HR-C2-1 = B`（`2026-10-15`）**；
2. ✅ **施工规格已出** → `docs/review/Phase-C-C2-Log-Routing-Console-Task-Sheet.md`（方案 B · 照抄版）→ **待人工审核 `HR-C2-4/5/6`** → 控制台**施工**（改规则 A / B）→ 回写存档（`V2V4` + 任务单）；
3. 执行 **§7 验证矩阵 `V-3`** → 记录 `PASS / NOT RUN / BLOCKED`；
4. 收口 `Phase C`（`C-1` 的 `V-2` telemetry + `V-3` log 双路径）。

---

> **Phase C-C2 = PLANNED / BLOCKED（未开工）. No EMQX change. No firmware change. No deploy. No push.**
> **唯一阻塞 = `/log` 路由 + 规则配额（`4/4`）. 固件 / Worker / D1 均已就绪.**
> **No Gate changed. `G-1 HUMAN_REQUIRED` · `G-4 BLOCKED` unchanged.**

---

*本文件于 **`2026-10-14`** 建立，为 **`Phase C-C2`（`/log` → `log_history`）** 的**实施前审查**（`REVIEW ONLY`）。
基线 = 固件仓 **`73bf330`** · EMQX 仓 **`3165aab`**；**不含任何生产变更、代码改动或配置变更**。*
