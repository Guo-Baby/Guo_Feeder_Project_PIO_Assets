# P3 · `H-WP4C-03` —— **EMQX Telemetry Routing 冻结裁决记录**（Decision Record · `FROZEN INPUT`）

> ## 🔒 `DOC-ONLY` · **仅留痕 · 未创建任何 EMQX 规则 · 未改动 `src/` / Worker / EMQX / migration**
>
> | 项 | 值 |
> |---|---|
> | **本文档性质** | **人工冻结裁决记录（权威）** —— `H-WP4C-03` 的最终裁决 |
> | **被冻结的设计** | `docs/review/P3-WP4C-EMQX-Rule-Design.md`（由 `scope proposal` 升级为 `FROZEN DESIGN`） |
> | **基线** | 固件仓 HEAD = **`cdcc766`**（工作树 clean）· CF 仓 **`9b25203`** · EMQX 仓 **`eec9444`** |
> | **本文档不做的** | ❌ 不创建 / 修改 EMQX 规则或连接器 · ❌ 不改 Worker · ❌ 不改 migration · ❌ 不改 firmware · ❌ 不改 `/up` · ❌ 不部署 |
>
> **状态变更**：`H-WP4C-03` = **`OPEN` → 🔒 `CLOSED`**（设计冻结）。
> **`CF-2` = 保持 `BLOCKED`**（本裁决**不解除** `CF-2`；实施仍在后续工作包，见 §6）。
>
> > **英文冻结声明**：*`H-WP4C-03` Frozen means the EMQX telemetry routing design is frozen.
> > It does not mean the rule has been created, the Worker/migration changed, or the pipeline deployed.*
>
> **🔄 更新记录（2026-10-12 · 第二阶段）**：新增 **§6A `Human Decisions` —— 实施阶段授权** ⇒
> **`H-WP4C-04` / `05` / `06` / `07` / `08` 全部 `CLOSED`**（授权已下达，**≠ 执行完成**）；
> `CF-2` **仍 `BLOCKED`**（判据改为 **`V-1…V-7` 通过**）。执行约束见 `docs/review/P3-WP4C-Implementation-Discipline.md`（`BINDING`）。
>
> **🔄 更新记录（2026-10-12 · 第三阶段 —— 实施前裁决补充）**：**`H-WP4C-09` 已拆分**为
> **`H-WP4C-09-A`（EMQX Console Operator · 🔴 `OPEN`）** + **`H-WP4C-09-B`（EMQX Rule Implementation · 🔴 `BLOCKED`）**；
> 并**登记 `H-WP4C-10-A`**（Action Target 形态裁决 · 🔴 `OPEN`，明细见 `docs/review/P3-WP4C-Action-Target-Decision.md`）。
> **`H-WP4C-04…08` 保持 `CLOSED`（不变）** · **`CF-2` 仍 `BLOCKED`** · 新增 **§6B**（拆分与登记）+ **§7.3**（实施门禁 Checklist）。
> **本阶段零生产变更**（仅 `docs/`）；EMQX 控制台边界规则见纪律 **§6A `EMQX Console Boundary Rule`**。
>
> **🔄 更新记录（2026-10-12 · 第四阶段 —— `H-WP4C-10-A` 裁决下达）**：**人工裁决 = 方案 A（`Pages Proxy`）**（裁决口径：「**裁决仍然使用 pages 代理，workers 无法直连**」）
> ⇒ **`H-WP4C-10` / `H-WP4C-10-A` = 🔒 `CLOSED`**（留痕见 `docs/review/P3-WP4C-Action-Target-Decision.md` **§5.1**）· **`D-2` 确认成立** ⇒ **`R-WP4C-12` 收敛 `CLOSED`**。
> **新增 `H-WP4C-11`**（Pages 中转函数 `/api/history/ingest` 新增 + 部署授权）= 🔴 **`OPEN`** ⇒ 成为 **`C-5` 的新 GATING 前置**。
> **`H-WP4C-09-A` 仍 `OPEN` · `-09-B` 仍 `BLOCKED`**；**`CF-2` 仍 `BLOCKED`**；本阶段**仍零生产变更**。
>
> **🔄 更新记录（2026-10-12 · 第五阶段 —— `V-0` 部分导出回填）**：人工在 EMQX 控制台完成**只读导出** ⇒
> **`V-0` = 🟠 `PARTIAL`**（**连接器 2/2 · 规则 3/3 含 SQL 全文 · 动作 3/3**；**缺**：连接器 **TLS 明细（SNI / 验证证书 / CA）** · `c-…b12100` **完整请求头** · **动作高级设置** · **D1 计数** · **回滚签署**）
> ⇒ **`H-WP4C-09-A` = 🔴 `OPEN` → 🟠 `PARTIAL`**（**不得置为 `CLOSED`**）· **`WP4C-IMPL:C-4` = 🟠 `PARTIAL`**；
> **现场证实**：**两个连接器 + 三个动作全部指向 `*.pages.dev`**（**无一指向 `*.workers.dev`**）⇒ `D-2` / `H-WP4C-10-A` 裁决获现场证据支持；
> **新增 `R-WP4C-15`**（既有 `guo_feeder/#` 规则 ≈90% 失败 ⇒ **污染 `V-3` 判据**）。
> 导出记录 → **`EMQX_Assets/config/data-integration/P3-WP4C-V0-EMQX-Baseline.md`**（本阶段**仍零生产变更**）。

---

## §1 裁决总览

| # | 裁决项 | 结论 |
|---|---|---|
| **D-1** | **分流方案** | 🔒 **冻结 = 方案 A（EMQX Rule Engine payload predicate routing）** |
| **D-2** | **telemetry 判别条件** | 🔒 **冻结 = `( schema_version ∧ seq ∧ ts ∧ type ∧ value ) ∧ ¬ c ∧ ¬ cmd`** |
| **D-3** | **旧 Rule 处理策略（`-S2`）** | 🔒 **冻结 = `mqtt_messages rule = guo_feeder/# AND NOT telemetry_predicate`**（两步强制） |
| **D-4** | **EMQX 平台限制** | 🔒 **冻结登记 `D-1…D-6` + 控制台人工实施方式** |
| **D-5** | **验证计划** | 🔒 **冻结 `V-1…V-7`** |
| **D-6** | **新增风险登记** | 🔒 **新增 `R-WP4C-7` / `R-WP4C-8` / `R-WP4C-9`** |
| — | **`H-WP4C-03` 状态** | 🔒 **`CLOSED`** |
| — | **`H-WP4C-04…08` 状态** | 🔒 **`CLOSED`**（§6A —— 实施阶段授权，**≠ 执行完成**） |
| — | **`H-WP4C-09` 状态** | 🟠 **已拆分** ⇒ **`-09-A`（Console Operator）= 🟠 `PARTIAL`（主体已导出，缺 5 项）** · **`-09-B`（Rule Implementation）= 🔴 `BLOCKED`**（§6B） |
| — | **`H-WP4C-10` / `H-WP4C-10-A` 状态** | 🔒 **`CLOSED`** —— **裁决 = 方案 A（`Pages Proxy`）**（`2026-10-12` · 见 `P3-WP4C-Action-Target-Decision.md` §5.1） |
| — | **`H-WP4C-11` 状态** | 🔴 **`OPEN`** —— **Pages 中转函数新增 + 部署授权**（方案 A 落地前置 · §6B） |
| — | **`CF-2` 状态** | 🔴 **仍 `BLOCKED`**（不变；判据 = `V-1…V-7` 通过） |

---

## §2 `H-WP4C-03` · 冻结分流方案

### 2.1 冻结原文

> **冻结分流方案 = EMQX Rule Engine payload predicate routing。**
>
> **禁止**：
> - ❌ firmware topic 修改
> - ❌ MQTT v5 metadata 依赖
> - ❌ Worker telemetry parsing
> - ❌ 新增 telemetry topic

### 2.2 冻结架构

```
                          MQTT
                           │
              guo_feeder/<device_id>/up
                           │
                           ▼
                  ┌─────────────────┐
                  │ EMQX Rule Engine│   ← 唯一分流责任方
                  └────────┬────────┘
                           │  payload predicate（§3）
              ┌────────────┴────────────┐
              │                         │
   telemetry 谓词为真           telemetry 谓词为假
              │                         │
              ▼                         ▼
    POST /api/history/ingest     既有 ingest 端点
              │                         │
              ▼                         ▼
      telemetry_history           mqtt_messages
```

### 2.3 方案 B / C 的处置（**冻结为禁止项**）

| 方案 | 处置 | 依据 |
|---|---|---|
| **B（MQTT v5 metadata / user property）** | 🔴 **禁止** —— 真机 `proto_ver = 4`（MQTT 3.1.1）**无 user property**；启用需升级 firmware 到 v5（超议题） | `D-5` · C-6 |
| **C（topic extension）** | 🔴 **禁止** —— 会触发 firmware topic 改动 + ACL 扩张 + `DT:WP-4` 入口族解冻 **三项联动** | C-3 · C-7 |

### 2.4 ★ 「分离优先」张力 —— 已消解（替代原 `-S1`）

`H-WP4-FW-01` 含 **"topic / domain separation 优先于 payload inspection"**。
**本裁决读法（冻结）**：**「优先」= 偏好**（**可选**时优先 topic 分离）。
⇒ 在 `/up` 共用且冻结不变的前提下，topic 分离**不可选**（B 前提不成立 / C 被禁止）⇒ **A 合规、唯一可落地**。
且 `H-WP4-FW-01` 的"Worker 不解析 payload"禁令**针对 Worker**；A 的谓词在 **EMQX Rule**，**不违反**。

> ⇒ 原 `scope proposal` 的待裁点 **`H-WP4C-03-S1` = 已关闭**。

---

## §3 `H-WP4C-03` · 冻结 telemetry 判别条件

### 3.1 冻结原文

> **Rule predicate 必须同时满足：**
>
> ```
> telemetry  ⟺  ( schema_version ∧ seq ∧ ts ∧ type ∧ value )
>              ∧ ¬ c
>              ∧ ¬ cmd
> ```
>
> 即：
> ```
> telemetry =
> (
>   schema_version
>   AND seq
>   AND ts
>   AND type
>   AND value
> )
> AND
> NOT c
> AND
> NOT cmd
> ```

### 3.2 冻结禁止项

> **禁止**：
> - ❌ 仅判断 `type`
> - ❌ 仅判断 `value`
> - ❌ 仅判断 topic
>
> **原因**：**command payload 内可能出现业务字段**（如命令结果的 `o` 对象内含业务 `type` 字段）
> ⇒ 单字段判别会产生**假阳性**。

### 3.3 语义注记（实现级 · 冻结）

| 项 | 规则 |
|---|---|
| `value` 存在性 | **必须**用"**键存在**"语义（`has` / `is_not_null` 一类）；**禁止**写成"值非空 / ≠ 0"（否则合法 `value=0` / `value=false` 会被漏判 ⇒ 假阴性） |
| `device_id` | **不进**谓词；身份**只**来自 topic（与 `cloud_protocol.md` §4.5 一致） |
| SQL 语法 | **非冻结**；以 §5 `V-1` 在部署版本上实测为准（`R-WP4C-3` / `R-WP4C-7`） |

---

## §4 `H-WP4C-03` · 冻结旧 Rule 处理策略（`-S2`）

### 4.1 冻结原文

> **新增 telemetry rule 时：旧规则（FROM `guo_feeder/#` → ACTION `mqtt_messages`）不能保持无限匹配。**
>
> **冻结策略**：
> ```
> mqtt_messages rule  =  guo_feeder/#  AND  NOT telemetry_predicate
> ```
> 即：**普通消息进入 `mqtt_messages`；telemetry 只进入 history。**
>
> **避免**：duplicate storage · action 次数增加 · 后续统计污染。

### 4.2 ★ 关键推论（为什么 `-S2` 不可省略）

既有消息规则 FROM = **`guo_feeder/#`** ⇒ 它**已经**匹配 `/up` 上的 telemetry。
**只新增不作收窄** ⇒ telemetry 会**双投**（新规则进 history ✅ + 既有规则进 `mqtt_messages` ❌）。

### 4.3 冻结落地步骤（两步**均强制**）

| 步 | 动作 | 性质 |
|---|---|---|
| **①** | **新增** `guo_feeder_history_ingest`（FROM `guo_feeder/+/up`，WHERE = §3.1 谓词，Action → `POST …/api/history/ingest`） | 新增 |
| **②** | **收窄**既有消息规则 WHERE 为 **`¬` telemetry 谓词** | **修改既有生产链路**（风险点，`R-WP4C-1` / `R-WP4C-4`） |

> ⇒ 原 `scope proposal` 的待裁点 **`H-WP4C-03-S2` = 已关闭（授权执行）**。

---

## §5 `H-WP4C-03` · 冻结 EMQX 平台限制说明 + 验证计划

### 5.1 冻结平台限制（实测事实 · 直接决定实施方式）

> **当前环境**：**EMQX Cloud Serverless**。
> - **Rule / Connector / Action 创建需要控制台操作**；
> - **API 自动化不可用（403）**；
> - **Worker 无法作为直接 action endpoint**（EMQX 打不通 `*.workers.dev`，须经 Pages 中转）。
>
> **因此实施阶段需要**：
>
> ```
> Human operation:
>     EMQX Console（创建 / 收窄规则 + 连接器 + 动作）
>
>    +
> Verification:
>     device publish  →  Rule match  →  D1 history insert
> ```

**冻结约束表**

| # | 约束 | 来源 |
|---|---|---|
| `D-1` | Serverless 规则 / 连接器 / 动作**只能控制台**配置（API **403**） | EMQX 仓 §1 |
| `D-2` | EMQX **打不通 `*.workers.dev`**，可达 `*.pages.dev` ⇒ 须 **Pages 中转** | EMQX 仓 §6.0.1 |
| `D-3` | 数据集成**按动作次数计费**；`guo_feeder/#` 当前覆盖 `/up` | EMQX 仓 §6.4 |
| `D-4` | EMQX 把**非 2xx 视为失败并重试** | EMQX 仓 §6.4 |
| `D-5` | 真机 **`proto_ver = 4`**（MQTT 3.1.1） | EMQX 仓 §6.10 |
| `D-6` | Serverless 规则 SQL 为**子集**，函数可用性依版本而异 | 平台性质 |

> **★ 强制**：控制台所做的每一处配置**必须回写** `EMQX_Assets/config/data-integration/README.md` 存档（漂移控制，`R-WP4C-6`）。
> ⇒ 原 `scope proposal` 的待裁点 **`H-WP4C-03-S3`（Pages 中转归属）= 已关闭**：中转属**实施工作包**的前置，不在本冻结范围内展开。

### 5.2 冻结验证计划（**全部 `NOT RUN`**）

| # | 用例 | 输入 / 操作 | 期望 |
|---|---|---|---|
| **V-1** | Rule syntax | 控制台保存并启用两条规则 | **可以保存并启用** |
| **V-2** | telemetry positive | `{"schema_version":1,"ts":123,"type":"temperature","value":25.5,"seq":1}` | history **+1**；`mqtt_messages` **+0** |
| **V-3** | command negative | 发送 command | history **+0**；`mqtt_messages` **+1** |
| **V-4** | mixed payload | 含 `type` + `value`，但**存在** `cmd` | **不是 telemetry** |
| **V-5** | multi device isolation | device A telemetry | **不能**进入 device B 的 history |
| **V-6** | reconnect | 重连后重复上行 | **不因 reconnect 重复写入** |
| **V-7** | duplicate | 相同 `(device_id, type, ts, seq)` | 验证 D1 **unique constraint**（`ux_th_dedup`） |

> **补充前置（STATIC · 离线）**：谓词真值表校验（4 类真实样本）—— 不计入冻结列表。
> **执行前置**：`D-1` + **Pages 中转就绪**（`D-2`）+ 端点已部署（`H-WP4C-04` / `05`）。

---

## §6 新增风险登记（本轮）

> **任务要求**：若发现 **Rule Engine 限制 / D1 action 限制 / 成本风险**，登记新的 `R-*`。以下三项均为**本轮新增**。

| ID | 级别 | 风险 | 缓解 |
|---|---|---|---|
| **`R-WP4C-7`** | 🟠 中 | **Rule Engine 限制** —— Serverless 规则 SQL 为**子集**（`D-6`）：JSON 解码 / 存在性函数（`has` / `is_not_null` / `json_decode`）在**具体版本**上可能缺失或语义不同；规则规模 / 单规则字段数有上限 | V-1 强制；谓词**结构**保持最小（7 项）；保留 `guo_feeder/+/up` 最小匹配面；若函数不可用 ⇒ 记录并回退到**可用等价写法**（**不改冻结谓词逻辑**） |
| **`R-WP4C-8`** | 🟠 中 | **D1 action 限制** —— telemetry 为**周期性写入**：① Pages 中转 + Worker + D1 三段链路**增加 RTT**，Action 超时后**重试**（`D-4`）⇒ 重复动作 + 重复计费；② D1 **写入吞吐 / 单次写量**上限 ⇒ 高频下 `d1_batch_failed`(500) ⇒ 重试 | **单台设备小流量试跑**，观测 Action 成功率 / D1 写量；**不新增 batch**（`count=1` 冻结）；吞吐不足 ⇒ **上报并暂停放量**（不自行放宽冻结） |
| **`R-WP4C-9`** | 🟠 中 | **成本风险** —— 按动作次数计费（`D-3`）；telemetry 默认 `interval_ms=300000`（5 min）× **3 KEEP 通道**（temperature / humidity / weight，**每通道独立报文**）⇒ 理论 **≈ 864 条/天/设备**，**比 presence（≈ 22 条/天）高约 1–2 个数量级** | **实施前量化**：台数 × 周期 × 通道数 → 日动作次数 vs 预算；需抑制时**只能调 payload 侧策略**（`telemetry.interval_ms` / 收敛 KEEP 集 ⇒ 属 `H-WP4-FW-06` / 范围决策），**不在本工作包内改冻结协议** |

> 既有 `R-WP4C-1…6`（既有规则误伤 / 假阴性 / 版本差异 / 双投 / 计费 / 漂移）**全部保留**，详见设计文档 §8。

---

## §6A `Human Decisions` —— 实施阶段授权（**2026-10-12**）

> **性质**：**人工裁决留痕（权威）** —— 关闭 `H-WP4C-04…08`，**授权进入实施阶段**。
> **执行约束**：实施纪律见 **`docs/review/P3-WP4C-Implementation-Discipline.md`**（📌 `BINDING`）。

### `H-WP4C-04` —— 远端 migration `0003_p3_history_tables.sql` 执行授权

| 项 | 内容 |
|---|---|
| **裁决** | 🔒 **`CLOSED`** |
| **允许执行** | `wrangler d1 migrations apply guofeeder --remote` |
| **要求** | **执行前**记录当前 migration 状态 · **执行后**确认 `0003` applied · **记录执行时间与结果** |

### `H-WP4C-05` —— EMQX Action 切换授权

| 项 | 内容 |
|---|---|
| **裁决** | 🔒 **`CLOSED`** |
| **允许** | **新增** telemetry history rule · **收窄**旧 `mqtt_messages` rule · 执行 Action 切换 |
| **必须遵守** | **新 Rule 与旧 Rule 修改必须作为同一变更窗口** · **不允许只新增 Rule** · **不允许产生 telemetry 双投** |

### `H-WP4C-06` —— 部署顺序确认

| 项 | 内容 |
|---|---|
| **裁决** | 🔒 **`CLOSED`** |
| **确认顺序** | `0003 migration` → `Worker deployment` → `EMQX Rule change` → `Validation` |
| **禁止** | **调整顺序** |

### `H-WP4C-07` —— EMQX Console 执行人与配置导出责任

| 项 | 内容 |
|---|---|
| **裁决** | 🔒 **`CLOSED`** |
| **实施前必须完成** | **Rule SQL 导出** · **Action 配置导出** · **Connector 配置导出** |
| **敏感信息（禁止入仓）** | **webhook key** · **secret** |
| **仅保存** | **secret 存在性** · **配置结构** · **非敏感 metadata** |

### `H-WP4C-08` —— Worker 部署授权

| 项 | 内容 |
|---|---|
| **裁决** | 🔒 **`CLOSED`** |
| **允许部署** | CF 仓 **`9b25203`** |
| **目标** | 包含 **`POST /api/history/ingest`** |
| **部署后必须确认** | **deployment version** · **时间** · **endpoint 可访问性** |

> **★ 阻塞守恒（重要）**：以上 `CLOSED` = **授权已下达**，**不等于**执行完成。
> 其中 **`H-WP4C-07` 的导出动作**受平台限制（EMQX 数据集成 API **`403`** · 无 CLI）**只能人工在控制台完成**；
> 若实际执行时无法完成导出，须 **`STOP` + 新 `HUMAN_REQUIRED`**（**不得跳过** —— 见纪律 §3 禁止项 3）。

---

## §6B `Human Decisions` —— **实施前裁决补充**（**2026-10-12 · 第三阶段**）

> **性质**：**人工裁决留痕（权威）** —— 依 `WP4C` 实施准备阶段要求，**拆分 `H-WP4C-09`** 并**登记 `H-WP4C-10` 的子裁决 `H-WP4C-10-A`**。
> **状态纪律**：**`H-WP4C-09` 不关闭**（**只拆分**）；`H-WP4C-04…08` **保持 `CLOSED`（不变）**；**不新增任何授权**（本阶段**零生产变更**）。
> **执行约束**：`docs/review/P3-WP4C-Implementation-Discipline.md` **§6A `EMQX Console Boundary Rule`**（**同期新增**）。
> **不做**：❌ 不创建 / 不修改 EMQX Rule · Action · Connector · ❌ 不部署代码 · ❌ 不改冻结设计。

### `H-WP4C-09-A` —— **EMQX Console Operator**（拆自 `H-WP4C-09` · **人工作业**）

| 项 | 内容 |
|---|---|
| **主题** | **EMQX Console Operator** —— 取得 **`V-0` 基线**的**唯一**合法路径 |
| **需要人工完成** | ① **登录 EMQX Console** ② **导出 Rule SQL** ③ **导出 Action 配置** ④ **导出 Connector 配置** ⑤ **保存 `V-0` 基线** ⑥ **脱敏后回写** `EMQX_Assets` |
| **脱敏要求** | **`X-Webhook-Key` / secret 的值禁入仓** —— 仅记录「已配置 / 未配置」与轮换方式（`H-WP4C-07`） |
| **回写位置** | **`EMQX_Assets/config/data-integration/P3-WP4C-V0-EMQX-Baseline.md`**（`V-0` 导出记录；§3 填写区 · 本次已回填主体） |
| **判据** | 三份快照（Rule SQL / Action / Connector）**全文归档**且 **secret 已脱敏** |
| **状态** | 🟠 **`PARTIAL`（进行中 · 不得置为 `CLOSED`）** |
| **进度（`2026-10-12` 实测）** | ✅ **连接器 2 / 2**（`c-…291200` presence · `c-…b12100` 消息流，均 `*.pages.dev`）· ✅ **规则 3 / 3（含 SQL 全文）** · ✅ **动作 3 / 3**（含请求体 + 动作层请求头）· 🛑 **未取得**：**连接器 TLS 明细（SNI / 验证服务器证书 / CA）** · `c-…b12100` **完整请求头** · **动作高级设置** · **D1 计数** · **回滚方案签署**（清单 → `V0` §3.6） |
| **附带发现** | **`R-WP4C-15`** —— 既有 `guo_feeder/#` 规则 ≈90% 失败（19k 触发 / 17k 失败）；**污染 `V-3` 判据** ⇒ 须先采计数基线（`V0` §3.7） |
| **完成后解锁** | **`H-WP4C-09-B`**（由 `BLOCKED` → 可执行；**另须** `H-WP4C-11` 已部署） |

### `H-WP4C-09-B` —— **EMQX Rule Implementation**（拆自 `H-WP4C-09` · **受依赖阻塞**）

| 项 | 内容 |
|---|---|
| **主题** | **EMQX Rule Implementation** —— 落地纪律 §3 的 ④⑤（= 纪律 §6A.3 的 `V-2` / `V-4`） |
| **依赖** | 🔴 **`H-WP4C-09-A = CLOSED`**（**无 `V-0` 基线不得变更**）· **且 `H-WP4C-10-A` 已裁决**（Action 目标形态） |
| **允许动作** | **新增** `guo_feeder_history_ingest` · **修改** 既有 `guo_feeder/#` Rule 的 **WHERE** |
| **强制** | **两步同批**（新增 + 收窄）· **禁**只新增 · **禁**双投 · **禁**第 3 条规则 / 双 Action |
| **状态** | 🔴 **`BLOCKED`**（依赖 `H-WP4C-09-A` + `H-WP4C-10-A`） |

### `H-WP4C-10` / `H-WP4C-10-A` —— **Action 目标形态裁决**（**人工决策 · ✅ 已裁决**）

| 项 | 内容 |
|---|---|
| **`H-WP4C-10`** | **主题**：Action 目标形态（`Pages Proxy` vs `Worker Direct`）· **状态**：🔒 **`CLOSED`**（`2026-10-12` 随 `-A` 裁决一并关闭） |
| **`H-WP4C-10-A`** | **主题**：**Action Target 形态** · **状态**：🔒 **`CLOSED`** · **裁决 = ✅ 方案 A（`Pages Proxy`）** |
| **裁决口径** | **「裁决仍然使用 pages 代理，workers 无法直连」**（人工原话 · 见 `P3-WP4C-Action-Target-Decision.md` §5.1） |
| **收敛** | **`D-2` 确认成立** ⇒ **`R-WP4C-12` 收敛 `CLOSED`**（存档 §1 的 `workers.dev` 记录 = Worker 侧**上游目标**反推，非 EMQX 侧连接器 URL） |
| **目标端点（结论）** | `POST https://guo-feeder-pagesdev.pages.dev/api/history/ingest`（**EMQX 侧填这个**）；上游 = `…workers.dev/api/history/ingest` |
| **明细文档** | **`docs/review/P3-WP4C-Action-Target-Decision.md`**（方案 A / B · 证据对照 · 必答清单） |
| **解锁对象** | 纪律 §6A.3 的 **`V-1`** —— ⚠️ **但须待 `H-WP4C-11` 部署完成后**才能真正判 `V-1` |

### `H-WP4C-11` —— **Pages 中转函数新增 + 部署授权**（**本轮新增 · 由 `H-WP4C-10-A` 裁决派生**）

| 项 | 内容 |
|---|---|
| **主题** | **Pages 中转函数 `/api/history/ingest` 的新增与部署** —— 方案 A 的**落地前置** |
| **原因** | EMQX **不可达 `*.workers.dev`**（`D-2` 已确认）⇒ Action 只能指向 `*.pages.dev` ⇒ **必须有 Pages 侧转发函数**（**当前不存在**：工程内仅 `command.js` / `presence/event.js` / `webhook.js`） |
| **允许动作（授权后）** | ① **新增** `Cloudflare_Assets/guo-feeder-pagesdev/functions/api/history/ingest.js`（**照抄** `presence/event.js` 的逐字转发模式）② **部署** Pages 工程 ③ **实测**中转可达性（`POST` 无 key ⇒ **`401`** = 通到 Worker） |
| **禁止** | ❌ 不改 Worker（`guo-feeder-api`）· ❌ 不改 EMQX · ❌ 不改冻结设计 · ❌ 不修改既有 `/api/webhook` / `/api/presence/event` / `/api/command` |
| **★ 一致性要求** | 须**新增与部署同批完成**，以维持 **CF 仓 HEAD == 线上版本** 的等价关系（当前 HEAD `9b25203` = 线上 `99e6d937`） |
| **状态** | 🔴 **`OPEN`** |
| **解锁对象** | `WP4C-IMPL:C-5` 的 **GATING 前置之一**（另一项 = `H-WP4C-09-A` 的 `V-0` 基线） |

> **★ 为何必须拆分 `H-WP4C-09`**：原项把**两种性质截然不同**的作业压在一项内 ——
> **`-A` = 只读导出（人工作业，可立即开工）**；**`-B` = 生产变更（受 `V-0` 与 Action 形态双重前置阻塞）**。
> 拆分后**责任与依赖各自闭环**：`-A` 完成**不构成** `-B` 的开工许可（`-B` 仍须 `H-WP4C-10-A`）。

---

## §7 状态

### 7.1 状态变更表

| ID | 主题 | **本记录前** | **本记录后** | 依据 |
|---|---|---|---|---|
| **`H-WP4C-03`** | EMQX Rule 分流机制（rule scope） | 🔴 `OPEN`（design proposal 就绪） | 🔒 **`CLOSED`** | 本文档 §2–§5 |
| `H-WP4C-04` | 远端 `0003` migration 授权 | 🔴 `OPEN` | 🔒 **`CLOSED`**（§6A —— 允许 `wrangler d1 migrations apply guofeeder --remote`） | §6A |
| `H-WP4C-05` | EMQX Action 切换授权 | 🔴 `OPEN` | 🔒 **`CLOSED`**（§6A —— 新增 + 收窄**须同一变更窗口**，禁双投） | §6A |
| `H-WP4C-06` | 部署顺序 | 🔴 `OPEN` | 🔒 **`CLOSED`**（§6A —— `0003` → Worker → EMQX → Validation，**禁调整**） | §6A |
| `H-WP4C-07` | EMQX Console 执行人 + 配置导出责任 | 🔴 `OPEN` | 🔒 **`CLOSED`**（§6A —— 实施前须导出 Rule SQL / Action / Connector；**secret 禁入仓**） | §6A |
| `H-WP4C-08` | Worker 部署授权 | 🔴 `OPEN` | 🔒 **`CLOSED`**（§6A —— 允许部署 `9b25203`） | §6A |
| `H-WP4C-09` | EMQX 控制台人工执行（纪律 §3 的 ③④⑤） | 🔴 `OPEN` | 🟠 **已拆分（不关闭）** ⇒ 由 `-09-A` / `-09-B` 承载 | §6B |
| `H-WP4C-09-A` | EMQX Console Operator（导出并回写 `V-0` 基线） | —（本轮新增） | 🔴 **`OPEN`**（人工作业，可立即开工） | §6B |
| `H-WP4C-09-B` | EMQX Rule Implementation（新增 + 收窄，**同批**） | —（本轮新增） | 🔴 **`BLOCKED`**（依赖 `-09-A` 且 **`H-WP4C-11`** 已部署） | §6B |
| `H-WP4C-10` | Action 目标形态裁决（**父项**） | 🔴 `OPEN` | 🔒 **`CLOSED`**（随 `-A` 裁决关闭） | §6B |
| `H-WP4C-10-A` | `Action Target = Pages Proxy` / `Worker Direct` | —（本轮新增） | 🔒 **`CLOSED`** —— **裁决 = 方案 A（`Pages Proxy`）** | §6B |
| `H-WP4C-11` | Pages 中转函数 `/api/history/ingest` 新增 + 部署授权 | —（本轮新增） | 🔴 **`OPEN`**（方案 A 的落地前置） | §6B |
| `H-WP4-FW-05` | 信封 v1 schema | `CLOSED` | `CLOSED`（不变） | — |
| `H-WP4-FW-06` | `mqtt.json.telemetry` 配置 | `CLOSED` | `CLOSED`（不变） | — |
| `H-WP4-FW-07` | telemetry `seq` 语义 | `CLOSED` | `CLOSED`（不变） | — |
| `H-WP4-FW-08` | Flash 预算处置 | `CLOSED` | `CLOSED`（不变） | — |
| `H-WP4-FW-09` | `/up` telemetry 载荷文档同步 | `CLOSED` | `CLOSED`（不变） | — |

### 7.2 `CF-2` 状态

| 项 | 值 |
|---|---|
| **`CF-2`** | 🔴 **`BLOCKED`（仍 `BLOCKED`）** —— **资源 blocker 已移除** · **`H-WP4C-03` 已关闭** · **`H-WP4C-04…08` 授权已下达（§6A）** |
| **仍受阻于** | **实施未闭环** —— ✅ `0003` **已落表** · ✅ Worker **已部署（`99e6d937`）** · ✅ Action 形态**已裁决（方案 A）** · ❌ **Pages 中转未部署**（`H-WP4C-11`）· ❌ EMQX 规则**未建**（基线未导出 `H-WP4C-09-A`）· **`V-1…V-7` 全 `BLOCKED`** |
| **`CF-1`** | 🟢 `RESOLVED`（保持） |
| **其他 Gate** | `DT:WP-4 = FROZEN` · `Phase C = PLANNED / BLOCKED` · `G-4 = BLOCKED`（**均不变**） |

> **★ 重要**：`H-WP4C-03 = CLOSED` **只表示设计已冻结**；`H-WP4C-04…08 = CLOSED` **只表示授权已下达**。
> **两者均不等于** `CF-2` 解除、**不等于**实施完成、**不等于**管线打通。
> `CF-2` 的解除**判据 = `V-1…V-7` 通过**（端到端 telemetry / log 落库验证）。

### 7.3 实施门禁 Checklist（`WP4C-IMPL` 命名空间 · 当前状态 · `2026-10-12`）

> **★ 命名空间声明（防混淆）**：本表 `C-1…C-7` 属 **`WP4C-IMPL` 命名空间** ——
> **不同于** `P3-WP4C-Pre-Implementation-Checklist.md` §0「边界约束 `C-1…C-8`」与 §1.4「配置门 `C-1…C-7`」，
> 也**不同于** `P3-WP4-PhaseC-Integration-Plan.md` 的「`C-1` / `C-2` / `C-3`」子计划。
> **引用时须写全**（如 `WP4C-IMPL:C-4`）。**编号不代表执行顺序** —— 执行顺序以纪律 **§3** / **§6A.3** 为准。

| ID | 门禁项 | 判据 | 状态 |
|---|---|---|---|
| `C-1` | **授权门** | `H-WP4C-04…08` 全部 `CLOSED`（§6A） | ✅ **`PASS`** |
| `C-2` | **Worker 部署** | 生产版本 = `99e6d937-4408-47b7-8170-4bfa71c9dfec`；`POST /api/history/ingest` 无 key ⇒ **`401`** | ✅ **`PASS`** |
| `C-3` | **D1 migration** | 远端 `0003` **applied**；`telemetry_history` / `log_history` 存在；**恰好 5 索引**；列对齐 `DT:WP-2` | ✅ **`PASS`** |
| `C-4` | **EMQX baseline（`V-0`）** | **三份基线**（Rule SQL / Action / Connector）已导出并**脱敏**归档 | 🟠 **`PARTIAL`** —— **主体已导出**（2 连接器 / 3 规则 SQL / 3 动作）；**缺 TLS 明细 + 完整请求头 + 高级设置 + 计数 + 回滚签署**（`V0` §3.6 · `H-WP4C-09-A`） |
| `C-5` | **Rule deployment（`V-2` + `V-4`）** | 新规则**已启用** **且** 旧规则**已收窄**（**同批**） | 🔴 **`BLOCKED`** —— 依赖 `C-4`（`V-0` 基线）**+ `H-WP4C-11`**（Pages 中转已部署）；执行方 = `H-WP4C-09-B` |
| `C-6` | **验证 `V-1…V-7`** | 冻结验证集**全项 `PASS`** | 🔴 **`BLOCKED`** —— 依赖 `C-5` |
| `C-7` | **存档回写** | 控制台配置已回写 `EMQX_Assets/config/data-integration/` | 🔴 **`BLOCKED`** —— 依赖 `C-5` |

> **汇总**：`PASS = 3`（`C-1` / `C-2` / `C-3`）· `PARTIAL = 1`（`C-4`）· `BLOCKED = 3`（`C-5` / `C-6` / `C-7`）· `FAIL = 0`。
> **★ 并行两前置（`2026-10-12`）**：**（人）`C-4` = `H-WP4C-09-A`（🟠 `PARTIAL`）** —— 补齐 `V0` §3.6 的 **5 项**（TLS 明细 / 完整请求头 / 高级设置 / D1 计数 / 回滚签署）；
> **（机）`H-WP4C-11`** —— Pages 中转函数 `/api/history/ingest` 新增 + 部署（`Action Target = Pages Proxy` 已裁决）。
> **两者闭合后**方可执行 `C-5`（`V-2` + `V-4` 同批）；★ 另须先采 **`V-0` 计数/成功率基线**（`R-WP4C-15` ⇒ 否则 `V-3` 判据被既有故障污染）。

---

## §8 剩余 `HUMAN_REQUIRED`

| ID | 主题 | 状态 | 阻塞对象 |
|---|---|---|---|
| **`H-WP4C-04`** | 远端 `0003` migration 授权 | 🔒 **`CLOSED`**（§6A） | CF 侧 |
| **`H-WP4C-05`** | EMQX Action 切换授权 | 🔒 **`CLOSED`**（§6A） | 端到端链路 |
| **`H-WP4C-06`** | 部署顺序 | 🔒 **`CLOSED`**（§6A） | 端到端链路 |
| **`H-WP4C-07`** | EMQX Console 执行人 + 配置导出责任（因 API `403` / 无 CLI，导出须人工） | 🔒 **`CLOSED`**（§6A） | 人工作业 |
| **`H-WP4C-08`** | Worker 部署授权（线上 `846384cd` 早于 `9b25203`） | 🔒 **`CLOSED`**（§6A） | CF 侧 |
| **`H-WP4C-09`** | EMQX 控制台人工执行（③④⑤） —— **已拆分**（**不关闭**） | 🟠 **已拆分** ⇒ `-09-A` / `-09-B`（§6B） | 拆分项 |
| **`H-WP4C-09-A`** | **EMQX Console Operator** —— 登录控制台 · 导出 Rule SQL / Action / Connector · 保存 `V-0` 基线 · **脱敏回写** `EMQX_Assets` | 🟠 **`PARTIAL`**（§6B · 主体已导出：**2 连接器 / 3 规则 SQL / 3 动作**；**缺 5 项** → `V0` §3.6） | **`WP4C-IMPL:C-4`** · 解锁 `-09-B` |
| **`H-WP4C-09-B`** | **EMQX Rule Implementation** —— 新增 `guo_feeder_history_ingest` + **同批**收窄既有 `guo_feeder/#` WHERE | 🔴 **`BLOCKED`**（§6B · 依赖 `-09-A` **且** `H-WP4C-11` 已部署） | **`WP4C-IMPL:C-5`** |
| **`H-WP4C-10`** | Action 目标形态裁决（**父项**） | 🔒 **`CLOSED`**（§6B · 随 `-A` 裁决关闭） | 已解 |
| **`H-WP4C-10-A`** | **Action Target 形态** | 🔒 **`CLOSED`** —— **裁决 = 方案 A（`Pages Proxy`）**（§6B · §5.1 留痕） | 已解 |
| **`H-WP4C-11`** | **Pages 中转函数 `/api/history/ingest` 新增 + 部署授权**（方案 A 落地前置） | 🔴 **`OPEN`**（§6B · 新增 `guo-feeder-pagesdev/functions/api/history/ingest.js` + 部署 + `401` 实测） | **`WP4C-IMPL:C-5`** · 准入 `V-1` |
| **`R-WP4C-8` / `9`** | D1 写入吞吐 / 成本量化 | 🟠 待**实施前**评估 | 放量前置 |
| `R-FRZ-2` | `seq` 不持久化 × `ts=0` 的跨 boot 幂等碰撞面 | 🟠 实施细节 | `H-WP4-FW-06` / 实施细化 |

> ✅ **已关闭**：**`H-WP4C-03`**（设计冻结）· **`H-WP4C-04…08`**（实施阶段授权，§6A）—— 均不再列为剩余 `HUMAN_REQUIRED`。
> ⚠️ **`CLOSED` ≠ 执行完成**：`H-WP4C-04…08` 仅代表**授权已下达**；**执行**仍须按 `P3-WP4C-Implementation-Discipline.md` 的固定顺序逐步推进，**每步产出报告**。
> 🟠 **`H-WP4C-09` 已拆分（不关闭）**：**`-A`（只读导出 · `OPEN`）** 与 **`-B`（生产变更 · `BLOCKED`）** 责任分离，**`-A` 完成 ≠ `-B` 可开工**（§6B）。
> ✅ **`H-WP4C-10` / `-10-A` 已由人工裁决关闭**（`2026-10-12`）：**`Action Target = Pages Proxy`**；派生 **`H-WP4C-11`**（Pages 中转部署授权 · 🔴 `OPEN`）。
> 🔴 **EMQX 侧仍持续 `STOP`**（纪律 §6A.4）：条件 1 = **目标仍不可达**（中转未部署）· 条件 2 = **`V-0` 基线未导出**。
> 实施前准备清单与基线采集流程见 **`docs/review/P3-WP4C-Pre-Implementation-Checklist.md`**；控制台边界规则见纪律 **§6A**；
> **导出与回写目标文件**见 **`EMQX_Assets/config/data-integration/P3-WP4C-V0-EMQX-Baseline.md`**（`V-0` 基线）与同目录 **`P3-WP4C-V2V4-Config-After.md`**（变更后存档）。

---

## §9 边界核对（**零违规**）

| 禁止项 | 状态 |
|---|---|
| 创建 EMQX rule | ✅ **未创建** |
| 修改 EMQX 配置 / 连接器 / ACL | ✅ **未改**（EMQX 仓 HEAD 仍 `eec9444`） |
| 修改 Worker（`Cloudflare_Assets`） | ✅ **未改** |
| 修改 D1 migration | ✅ **未改** |
| 修改 `src/` / `include/` / `platformio.ini` / `partitions.csv` | ✅ **未改** |
| 修改 topic / `/up` / frozen telemetry schema | ✅ **未改** |
| 修改冻结设计文档（`P2-0-…freeze.md` / `DT:WP-4`） | ✅ **未改** |
| 启用 `GF_TELEMETRY_ENABLED` | ✅ **未启用**（保持 `0`） |
| `push` / 部署 | ✅ **未 push · 未部署** |
| 新增 / 修改文档 | ✅ **仅 `docs/`**（见 §10） |
| **本轮（第三阶段）生产侧操作** | ✅ **零操作** —— 未向 EMQX 发起任何请求 · 未调用 `wrangler` · 未部署 · 未执行 migration |

---

## §10 修改文件

### 10.1 本轮（**第四阶段 · `H-WP4C-10-A` 裁决下达 + `V-0` 导出工件**）

| 文件 | 仓库 | 性质 |
|---|---|---|
| `docs/review/P3-WP4C-Action-Target-Decision.md` | 固件 | **裁决回填** —— `DECISION PENDING` → **`DECIDED`（方案 A）**；新增 §1.3（收敛）· §5.1（裁决留痕）；§2/§3 标注采纳 / 排除；§6 转为开工门清单 |
| `docs/review/P3-WP4C-Decisions.md` | 固件 | **更新** —— banner · §1 · §6B（`-10/-10-A` → `CLOSED` + **新增 `H-WP4C-11`**）· §7.1 · §7.2 · **§7.3（`C-5` 依赖改为 `C-4` + `H-WP4C-11`）** · §8 |
| `docs/review/P3-WP4C-Implementation-Record.md` | 固件 | **回填** —— §5 拆分后形态同步 + `R-WP4C-12` 收敛 |
| `docs/review/P3-WP4C-Implementation-Discipline.md` | 固件 | **状态回填** —— §6A.3 准入序列 `V-1` 的当前状态与解锁条件 |
| `docs/README.md` | 固件 | **索引** —— 状态词同步（新增 `V-0` 工件指向） |
| `config/data-integration/P3-WP4C-V0-EMQX-Baseline.md` | EMQX | **新增 + 回填** —— `V-0` **导出记录**（`PARTIAL`）：2 连接器 / 3 规则 SQL / 3 动作 + **待补清单 §3.6** + **异常 §3.7**（`R-WP4C-15`） |
| `config/data-integration/P3-WP4C-V2V4-Config-After.md` | EMQX | **新增** —— 变更后（`V-2`/`V-4`）配置**回写模板** |
| `config/data-integration/README.md` | EMQX | **更新** —— 新增 §7 指向两个 WP4C 工件 |

> **★ 边界**：**全部为 `docs/`** —— 零 EMQX 写操作 · 零 Worker / Pages 部署 · 零 migration · 零 firmware 修改 · 零 `push`。

### 10.2 第三阶段（实施前裁决补充）

| 文件 | 性质 |
|---|---|
| `docs/review/P3-WP4C-Implementation-Discipline.md` | **增补** —— **§6A `EMQX Console Boundary Rule`**（控制台边界定性 · 代理禁止清单 · 准入序列 `V-0…V-5` · `STOP` 条款）+ §7 关联文档 + 页脚 |
| `docs/review/P3-WP4C-Action-Target-Decision.md` | **新增** —— `H-WP4C-10` / `H-WP4C-10-A` **待裁决记录**（方案 A / B · 优点 / 风险 · 证据对照 · 必答清单 · **不选择**） |
| `docs/review/P3-WP4C-Decisions.md` | **更新** —— 本文件（banner · §1 总览 · **§6B 裁决补充** · §7.1 状态 · §7.2 `CF-2` · **§7.3 实施门禁 Checklist** · §8 剩余 `HUMAN_REQUIRED`） |
| `docs/review/P3-WP4C-Implementation-Record.md` | **回填** —— §5 `H-WP4C-09` / `10` 登记同步为**拆分后形态**（跨文档一致性必需） |
| `docs/README.md` | **索引** —— 新增 `Action-Target-Decision` 条目；更新纪律 / 裁决条目的状态词 |

> **★ 边界**：以上**全部为 `docs/`** —— 零 EMQX 写操作 · 零 Worker 部署 · 零 migration · 零 firmware 修改 · 零 `push`。

### 10.3 前序阶段（第一 / 第二阶段 · 留存）

| 文件 | 性质 |
|---|---|
| `docs/review/P3-WP4C-EMQX-Rule-Design.md` | **升级** —— `scope proposal` → **`FROZEN DESIGN`**（冻结方案 / 谓词 / `-S2` / 平台限制 / `V-1…V-7` / 新增 `R-WP4C-7…9`） |
| `docs/review/P3-WP4C-Decisions.md` | **新增** —— 本文件（`H-WP4C-03` 冻结裁决记录） |
| `docs/review/P3-WP4-Firmware-Telemetry-Decisions.md` | **状态回填（仅 banner / 状态行）** —— `H-WP4C-03` 由 `OPEN` → `CLOSED`；`CF-2` 行同步（**跨文档一致性必需**） |
| `docs/README.md` | **索引** —— 新增本文件条目；更新 Rule 设计文档条目的状态词 |

---

*本文档于 **2026-10-12** 建立，为 **`H-WP4C-03` 的人工冻结裁决记录**；
**仅留痕，不含任何配置改动、规则创建或实施**。
基线 = 固件仓 HEAD **`cdcc766`**（工作树 clean）· CF 仓 **`9b25203`** · EMQX 仓 **`eec9444`**。
所有 `file:line` 与平台事实取自基线，可逐条回查。*
*`2026-10-12` **第三阶段增补**：§6B（`H-WP4C-09` 拆分 + `H-WP4C-10-A` 登记）· §7.3（实施门禁 Checklist，`PASS 3 / BLOCKED 4`）；
**仍为零生产变更**（仅 `docs/`），**未关闭任何 `HUMAN_REQUIRED`**。*
*`2026-10-12` **第四阶段增补**：**`H-WP4C-10-A` 人工裁决 = 方案 A（`Pages Proxy`）** ⇒ `H-WP4C-10`/`-10-A` **`CLOSED`** · `D-2` 确认成立 ⇒ `R-WP4C-12` **收敛** · **新增 `H-WP4C-11`**（Pages 中转部署授权，🔴 `OPEN`）；
`C-5` 依赖改为 **`C-4` + `H-WP4C-11`**。**仍为零生产变更**（固件仓 + EMQX 仓 `docs/`），**未部署任何代码 · 未创建任何 EMQX 配置**。*
*`2026-10-12` **第五阶段回填**：**`V-0` = 🟠 `PARTIAL`**（连接器 2/2 · 规则 3/3 · 动作 3/3；缺 TLS 明细 / 完整请求头 / 高级设置 / 计数 / 回滚签署）⇒ `H-WP4C-09-A` = 🟠 `PARTIAL` · `C-4` = 🟠 `PARTIAL`；
**新增 `R-WP4C-15`**（既有链路 ≈90% 失败 ⇒ 污染 `V-3`）。**仍为零生产变更**。*
