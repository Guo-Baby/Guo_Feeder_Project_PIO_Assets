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
>
> **🔄 更新记录（2026-10-12 · 第六阶段 —— `V-0` 收口）**：补齐 **动作高级设置**（重试 `1` / 超时 `5s` / 健康检查 `30s` / 缓存 `4MB` / **异步**）· **`c-…b12100` 请求头全文**（**无任何鉴权头**）· **TLS 明细确定为平台不可回读**（⇒ `R-WP4C-16` + 双 guard）· **D1 计数基线已采**（`th=0` / `lh=0` / `mm=2458`；近 24 h `13`）· **回滚方案已签署**（代理依人工委托拟制，执行仍由人工）。
> ⇒ **`V-0` = ✅ `CLOSED WITH GUARD`** · **`H-WP4C-09-A` = ✅ `CLOSED WITH GUARD`** · **`C-4` = ✅ `PASS`（带 guard）** ⇒ 门禁 **`PASS 4 / BLOCKED 3`**。
> **唯一剩余前置 = `H-WP4C-11`**（Pages 中转新增 + 部署）⇒ **EMQX 侧变更继续 `STOP`**（§6A.4 条件 1）。
>
> **🔄 更新记录（`2026-10-12` · 第七阶段 —— `H-WP4C-11` 部署与端到端验证）**：人工明示授权（*「授权你，开始部署，做完整测试确保可用」*）⇒
> **Pages 中转函数 `functions/api/history/ingest.js` 已新增并部署**（新部署 = **`61b4938e-fa2e-42da-b950-f527781f79f9`**；回滚目标 = `5790b3c5-…`）。
> **端到端验证全通**：中转层 + 鉴权边界 **7/7** · 写入用例 **8/8**（`PASS 15 / FAIL 0`）· D1 落库 **telemetry ×4 + log ×1**（合成 `device_id`，**已定点清理**）。
> ⇒ **`H-WP4C-11` = ✅ `CLOSED`** · **`V-1` = 🟢 `PASS`（带说明 —— 由既存 presence 连接器在生产上同 host 持续投递作证）** ·
> **`H-WP4C-09-B` = 🟡 `READY`**（两条依赖均已闭合，**执行仍须人工控制台**）。
> **纪律 §6A.4 `STOP` 条件 1（目标不可达）不再成立**；**本阶段唯一生产变更 = 1 次已授权的 Pages 部署**。
> **核查结论（人工指定重点）**：旧连接器**确实被 CF 收到并存储** —— `mqtt_messages` **2458 行**且**仍在增长**（最新 `2026-10-12 06:33:36 UTC`）；
> **★ 新证据**：`…/log` 主题 **0 行** ⇒ 支持 `R-WP4C-15` 的「`/log` 二进制 CBOR ⇒ `400 Invalid JSON`」候选机制。
> 记录 → **`docs/review/P3-WP4C-History-Relay-Deployment-Verification.md`**（新增 **`R-WP4C-17`**）。
>
> **🔄 更新记录（`2026-10-12` · 第八阶段 —— `R2` 设计修订：Serverless 连接器配额）**：人工核实 **EMQX Cloud Serverless 数据集成配额不可调整**
> —— **连接器 ≤2（当前 2/2，余量 0）** · 规则 ≤4 · 动作 ≤4 · 单规则动作 =1 ⇒ `R1` 依赖的「**新建独立连接器**」**不可能实现**。
> ⇒ **新增 `D-7`** · **冻结设计升级为 `R2`**（新增 §10 修订节）· **`Implementation-Plan` §8 规格升版 v2** ·
> **`H-WP4C-09-B` = 🟡 `READY (R2)`**（施工以 `v2` 规格为准）。
> **`R2` 形态 = 复用 `c-…b12100`（URL 收敛为 base）+ 动作级 `URL Path` 分流**；
> **同批修正**：动作请求体 `"payload": ${payload}`（**不加引号**）—— 原 `v1` 的带引号写法会让 Worker 返 `400 unparsable_payload`。
> **核心语义（方案 A / 冻结谓词 / `-S2` / Pages Proxy / 端点 / `V-1…V-7`）逐字不变**；新增 **`R-WP4C-18`（连接器复用耦合）· `-19`（编辑窗口）**。
> 明细 → **`docs/review/P3-WP4C-Connector-Reuse-Amendment.md`**（**本阶段仍零生产变更**）。
>
> **🔄 更新记录（`2026-10-12` · 第九阶段 —— `R3` 设计修订：**连接器不可编辑**）**：人工反馈 **「连接器建立好后无法修改变更，只能删除旧的建立新的」** ——
> 官方文档逐字确认：**「编辑连接器 / 启用禁用连接器：该功能**仅适用于专有版部署**」** ⇒ **Serverless 连接器创建后不可编辑**
> ⇒ **`R2` 的「就地改写 `c-…b12100` 的 URL」不可能执行** ⇒ **施工形态改为「删除重建」**。
> **`R3` 形态**：删除 `c-…b12100` / `a-…921e22` / `r-…a01720` ⇒ **新建** 连接器 `CB`（URL = 裸主机 base）+ 动作 `MF`（`/api/webhook`）/ `HI`（`/api/history/ingest`）+ 规则 ×2；**presence 链路零接触**。
> ⇒ **`H-WP4C-09-B` = 🟡 `READY (R3)`**（施工以施工说明为准）· **冻结设计 → `R3`** · **规格 → `v3`** · **新增 `R-WP4C-18b`（删除不可逆）**。
> ★ **两项净收益**：① **`-S2` 双投窗口彻底消失**（新消息流规则**一出生即带 `NOT 谓词`**）② 连接器内容回归**全新且全量留痕**。
> ★ **两项代价**：① 删除**不可逆**（旧 ID 消失 ⇒ 施工前必须抄录运行统计 + 截图配置）② 中断窗口由「≤60 s」变为「**拆除→重建 ≤15 min**」（**仅消息流一条 best-effort 链路** ⇒ 期望丢 0–1 条）。
> **施工说明（唯一依据）** → **`docs/review/P3-WP4C-Console-Build-Guide-R3.md`**（**本阶段仍零生产变更**）。

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
| **`D-7`** ⭐ **R2 新增** | **平台配额（不可调整）** | 🔒 **连接器 ≤2 · 规则 ≤4 · 动作 ≤4 · 单规则动作 =1** ⇒ **`R2` 修订：单连接器复用 + 动作级 `URL Path` 分流**；**`R3` 追加**：因**连接器不可编辑（仅专有版）** ⇒ 施工改为 **删除重建**（冻结设计 **§10** · 明细 → `docs/review/P3-WP4C-Connector-Reuse-Amendment.md` · 施工 → `docs/review/P3-WP4C-Console-Build-Guide-R3.md`） |
| — | **`H-WP4C-03` 状态** | 🔒 **`CLOSED`** |
| — | **`H-WP4C-04…08` 状态** | 🔒 **`CLOSED`**（§6A —— 实施阶段授权，**≠ 执行完成**） |
| — | **`H-WP4C-09` 状态** | 🟠 **已拆分** ⇒ **`-09-A`（Console Operator）= ✅ `CLOSED WITH GUARD`** · **`-09-B`（Rule Implementation）= 🟡 `READY (R3)`**（依赖已全部闭合 · **待人工控制台执行 · 规格见 `P3-WP4C-Console-Build-Guide-R3.md`**）（§6B） |
| — | **`H-WP4C-10` / `H-WP4C-10-A` 状态** | 🔒 **`CLOSED`** —— **裁决 = 方案 A（`Pages Proxy`）**（`2026-10-12` · 见 `P3-WP4C-Action-Target-Decision.md` §5.1） |
| — | **`H-WP4C-11` 状态** | ✅ **`CLOSED`**（`2026-10-12` —— **Pages 中转已新增并部署** `61b4938e-…` + 端到端验证 `PASS 15 / FAIL 0`） |
| — | **纪律 §6A.3 准入序列 `V-0…V-5`** | **`V-0` = ✅ 已完成（`CLOSED WITH GUARD`）** · **`V-1` = 🟢 `PASS`（带说明）** · `V-2…V-5` = 🔴 待人工控制台执行（`H-WP4C-09-B`） |
| — | **`CF-2` 状态** | 🔴 **仍 `BLOCKED`**（判据 = `V-1…V-7` **全项**；现仅 **`V-1`** 部分达成） |

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
| **状态** | ✅ **`CLOSED WITH GUARD`**（`2026-10-12`） |
| **进度（`2026-10-12` 实测）** | ✅ **连接器 2 / 2**（`c-…291200` presence · `c-…b12100` 消息流，均 `*.pages.dev`；**URL / 请求头 / 连接参数全录**）· ✅ **规则 3 / 3（含 SQL 全文）** · ✅ **动作 3 / 3**（请求体 + 请求头 + **高级设置**：重试 `1` / 超时 `5s` / 健康检查 `30s` / 缓存 `4MB` / **异步**）· ✅ **D1 计数基线**（`th=0` · `lh=0` · **`mm=2458`**，近 24 h **13**）· ✅ **回滚方案已签署**（`V0` §5.2） |
| **guard（2 条）** | ① 连接器 **TLS 明细保存后不可回读** ⇒ **新建连接器必须「保存前截图 + 保存后实测」**（`R-WP4C-16`）② **不得**用 `README.md` §6.4 的书写值充当实测基线（纪律 §6A.2 第 3 条） |
| **附带发现** | **`R-WP4C-15`** —— 既有 `guo_feeder/#` 链路 ≈90% 失败（19k 触发 / 17k 失败；**D1 交叉印证**：库存仅 2458 且最新记录 ≈1 h 前 ⇒ 低成功率非完全失效）；**污染 `V-3` 判据** ⇒ 已采计数基线 |
| **完成后解锁** | **`H-WP4C-09-B`**（由 `BLOCKED` → 可执行；**另须** `H-WP4C-11` 已部署） |

### `H-WP4C-09-B` —— **EMQX Rule Implementation**（拆自 `H-WP4C-09` · 🟡 **依赖已闭合 · 待人工执行**）

| 项 | 内容 |
|---|---|
| **主题** | **EMQX Rule Implementation** —— 落地纪律 §3 的 ④⑤（= 纪律 §6A.3 的 `V-2` / `V-4`） |
| **依赖（均已闭合 ✅）** | ✅ **`H-WP4C-09-A = CLOSED WITH GUARD`**（`V-0` 基线已固化）· ✅ **`H-WP4C-10-A` 已裁决**（Action 形态 = 方案 A）· ✅ **`H-WP4C-11 = CLOSED`**（Pages 中转**已部署并验证**） |
| **允许动作（`R3`）** | **删除** `r-…a01720` → **删除** `a-…921e22` → **删除** `c-…b12100` ⇒ **新建** 连接器 `CB` · **新建** 动作 `MF`（`/api/webhook`）/ `HI`（`/api/history/ingest`）· **新建** 规则 `guo_feeder_message_ingest`（**直接带 `NOT 谓词`**）/ `guo_feeder_history_ingest`（**冻结谓词**） |
| **强制** | **禁止双投**（靠"消息流规则一出生即带 `NOT 谓词`"保证）· **禁**额外连接器（配额 2/2）· **禁**第 5 条规则 / 第 5 个动作 · **presence 链路零接触**（1 连接器 + 2 动作 + 2 规则） |
| **施工前必做** | **抄录**旧规则/动作运行统计（`R-WP4C-15` 证据）· **截图**旧连接器与旧动作配置 · **采** D1 计数基线（施工说明 §3） |
| **执行方** | **人工（EMQX 控制台）** —— 代理**无**控制台 / API 权限（EMQX 数据集成 API `403` · 无 CLI，见 `D-4`） |
| **状态** | 🟡 **`READY (R3)`** —— **依赖已全部满足，可开工**；**施工规格 = `docs/review/P3-WP4C-Console-Build-Guide-R3.md`**；**不等于已执行** |

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

### `H-WP4C-11` —— **Pages 中转函数新增 + 部署授权**（**由 `H-WP4C-10-A` 裁决派生 · ✅ 已部署并验证**）

| 项 | 内容 |
|---|---|
| **主题** | **Pages 中转函数 `/api/history/ingest` 的新增与部署** —— 方案 A 的**落地前置** |
| **原因** | EMQX **不可达 `*.workers.dev`**（`D-2` 已确认）⇒ Action 只能指向 `*.pages.dev` ⇒ **必须有 Pages 侧转发函数**（**部署前不存在**：工程内仅 `command.js` / `presence/event.js` / `webhook.js`） |
| **允许动作（已执行）** | ① **新增** `Cloudflare_Assets/guo-feeder-pagesdev/functions/api/history/ingest.js`（**照抄** `presence/event.js` 的逐字转发模式 · 并**额外增加 `X-Webhook-Key` 透传**，因 Worker 侧该端点**必须**鉴权）② **部署** Pages 工程 ③ **实测**中转可达性 |
| **禁止（均已遵守）** | ❌ 未改 Worker（`guo-feeder-api`）· ❌ 未改 EMQX · ❌ 未改冻结设计 · ❌ 未修改既有 `/api/webhook` / `/api/presence/event` / `/api/command` |
| **★ 一致性要求** | ✅ **新增与部署同批完成** ⇒ **CF 仓 HEAD（`9b25203` + 本次新增）= 线上版本**（Worker 侧线上仍 `99e6d937`，**未重部署**） |
| **部署结果** | Pages 新部署 = **`61b4938e-fa2e-42da-b950-f527781f79f9`**（Production · branch `main`）· 回滚目标 = **`5790b3c5-ff84-4539-bc03-0810401abd39`**· 仅 Functions bundle 变更（静态资产未变） |
| **验证结果** | ✅ **`PASS 15 / FAIL 0`**（A 段 7/7 中转层 + 鉴权边界；B 段 8/8 端到端写入）· D1 落库 **telemetry ×4 + log ×1**（已定点清理）· 既有三端点**回归通过**· **可复现脚本** `scripts/smoke-history-relay.mjs` |
| **状态** | ✅ **`CLOSED`**（`2026-10-12`） |
| **解锁对象** | `WP4C-IMPL:C-5` 的 **GATING 前置之一已闭合**（另一项 `H-WP4C-09-A` 亦已 `CLOSED WITH GUARD`）⇒ **`H-WP4C-09-B` 的依赖已全部满足** |
| **明细文档** | **`docs/review/P3-WP4C-History-Relay-Deployment-Verification.md`** |

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
| `H-WP4C-09-B` | EMQX Rule Implementation（**`R3` 删除重建**：删 3 件 → 建 连接器 `CB` + 动作 `MF`/`HI` + 规则 ×2） | —（本轮新增） | 🟡 **`READY (R3)`**（`-09-A` + `H-WP4C-11` **依赖已全部闭合**；**执行仍须人工控制台** —— 代理无 EMQX 控制台 / API 权限；**规格 = `P3-WP4C-Console-Build-Guide-R3.md`**） | §6B |
| `H-WP4C-10` | Action 目标形态裁决（**父项**） | 🔴 `OPEN` | 🔒 **`CLOSED`**（随 `-A` 裁决关闭） | §6B |
| `H-WP4C-10-A` | `Action Target = Pages Proxy` / `Worker Direct` | —（本轮新增） | 🔒 **`CLOSED`** —— **裁决 = 方案 A（`Pages Proxy`）** | §6B |
| `H-WP4C-11` | Pages 中转函数 `/api/history/ingest` 新增 + 部署授权 | —（本轮新增） | ✅ **`CLOSED`**（已部署 `61b4938e-…` + 端到端验证 `PASS 15 / FAIL 0`） | §6B |
| `H-WP4-FW-05` | 信封 v1 schema | `CLOSED` | `CLOSED`（不变） | — |
| `H-WP4-FW-06` | `mqtt.json.telemetry` 配置 | `CLOSED` | `CLOSED`（不变） | — |
| `H-WP4-FW-07` | telemetry `seq` 语义 | `CLOSED` | `CLOSED`（不变） | — |
| `H-WP4-FW-08` | Flash 预算处置 | `CLOSED` | `CLOSED`（不变） | — |
| `H-WP4-FW-09` | `/up` telemetry 载荷文档同步 | `CLOSED` | `CLOSED`（不变） | — |

### 7.2 `CF-2` 状态

| 项 | 值 |
|---|---|
| **`CF-2`** | 🔴 **`BLOCKED`（仍 `BLOCKED`）** —— **资源 blocker 已移除** · **`H-WP4C-03` 已关闭** · **`H-WP4C-04…08` 授权已下达（§6A）** |
| **仍受阻于** | **实施未闭环** —— ✅ `0003` **已落表** · ✅ Worker **已部署（`99e6d937`）** · ✅ Action 形态**已裁决（方案 A）** · ✅ **Pages 中转已部署并验证**（`H-WP4C-11 = CLOSED`）· ✅ `V-0` 基线**已固化** · ❌ EMQX 规则**未建**（准入 `V-2`/`V-4` 待人工控制台执行 `H-WP4C-09-B`）· **冻结验证集 `V-1…V-7` 全未跑**（仅**准入序列**的 `V-0` / `V-1` 已完成） |
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
| `C-4` | **EMQX baseline（`V-0`）** | **三份基线**（Rule SQL / Action / Connector）已导出并**脱敏**归档 | ✅ **`PASS`（带 guard）** —— 连接器 2/2 · 规则 3/3（SQL 全文）· 动作 3/3（含高级设置）· D1 计数 · 回滚已签署；**guard = TLS 明细不可回读**（`R-WP4C-16` · `H-WP4C-09-A` = `CLOSED WITH GUARD`） |
| `C-5` | **Rule deployment（`R3`：拆除 + 重建）** | 连接器 `CB` **已建** **且** 两条新规则**已启用**（`history` 带谓词 · `message` 带 `NOT 谓词`） | 🔴 **`BLOCKED`** —— **前置已全部闭合**（`C-4` ✅ · `H-WP4C-11` ✅）；**唯一依赖 = 人工控制台执行 `H-WP4C-09-B`（`R3` 规格）** |
| `C-6` | **验证 `V-1…V-7`** | 冻结验证集**全项 `PASS`** | 🔴 **`BLOCKED`** —— 依赖 `C-5`（**准入序列**的 `V-0` / `V-1` 已完成 —— 见 §1 与纪律 §6A.3；**冻结验证集 `V-1…V-7` 仍未跑**） |
| `C-7` | **存档回写** | 控制台配置已回写 `EMQX_Assets/config/data-integration/` | 🔴 **`BLOCKED`** —— 依赖 `C-5` |

> **汇总**：`PASS = 5`（`C-1` / `C-2` / `C-3` / **`C-4`（带 guard）** / **`C-5` 的前置** —— 即 `H-WP4C-11` 中转部署 ✅）· `BLOCKED = 2`（`C-5` 的**执行** / `C-6`）· `FAIL = 0`。
> **★ 唯一剩余前置（`2026-10-12` · `R3` 施工说明下达后）**：**`H-WP4C-09-B`** —— **人工在 EMQX 控制台**按
> **`docs/review/P3-WP4C-Console-Build-Guide-R3.md`** 执行：**拆除**旧消息流链路 → **新建** 连接器 `CB` + 动作 `MF`/`HI` + 规则 ×2（`guo_feeder_history_ingest` 带**冻结谓词**；`guo_feeder_message_ingest` 带 **`NOT 谓词`**）。
> ⚠️ **`R3` 新增风险 `R-WP4C-18b`**：**删除不可逆**（旧对象 ID 消失）⇒ **施工前必须**抄录旧规则/动作运行统计 + 截图配置（施工说明 §3 / §11.2）。
> **两侧前置均已就绪**：✅ `V-0` 基线（含 `V-3` 判据的噪声基线）**已固化** · ✅ Pages 中转**已部署并端到端验证**（`PASS 15 / FAIL 0`）。
> **纪律 §6A.4 的 `STOP` 条件 1（目标不可达）已不再成立**；`STOP` 条件 2（`V-0` 未导出）亦**已于第六阶段解除**。⇒ **当前无纪律层面的 `STOP`**；剩余阻塞是**人工执行能力**（代理无控制台 / API 权限），**不是**设计、授权或环境问题。

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
| **`H-WP4C-09-A`** | **EMQX Console Operator** —— 登录控制台 · 导出 Rule SQL / Action / Connector · 保存 `V-0` 基线 · **脱敏回写** `EMQX_Assets` | ✅ **`CLOSED WITH GUARD`**（§6B · **guard = TLS 明细不可回读 ⇒ 新建连接器保存前截图 + 保存后实测**） | **`WP4C-IMPL:C-4` = ✅ `PASS`** · 解锁 `-09-B` |
| **`H-WP4C-09-B`** | **EMQX Rule Implementation** —— **`R3`：删除** `r-…a01720`/`a-…921e22`/`c-…b12100` ⇒ **新建** 连接器 `CB` + 动作 `MF`（`/api/webhook`）/`HI`（`/api/history/ingest`）+ 规则 `guo_feeder_message_ingest`（**带 `NOT 谓词`**）/`guo_feeder_history_ingest`（**冻结谓词**） | 🟡 **`READY (R3)`**（§6B · **依赖已全部闭合**；**施工规格 = `P3-WP4C-Console-Build-Guide-R3.md`**；**执行仍须人工控制台**） | **`WP4C-IMPL:C-5`** |
| **`H-WP4C-10`** | Action 目标形态裁决（**父项**） | 🔒 **`CLOSED`**（§6B · 随 `-A` 裁决关闭） | 已解 |
| **`H-WP4C-10-A`** | **Action Target 形态** | 🔒 **`CLOSED`** —— **裁决 = 方案 A（`Pages Proxy`）**（§6B · §5.1 留痕） | 已解 |
| **`H-WP4C-11`** | **Pages 中转函数 `/api/history/ingest` 新增 + 部署**（方案 A 落地前置） | ✅ **`CLOSED`**（§6B · 已部署 `61b4938e-…` · 端到端 `PASS 15 / FAIL 0` · 清理已复核） | **`WP4C-IMPL:C-5`** · 准入 `V-1` ✅ |
| **`R-WP4C-8` / `9`** | D1 写入吞吐 / 成本量化 | 🟠 待**实施前**评估 | 放量前置 |
| `R-FRZ-2` | `seq` 不持久化 × `ts=0` 的跨 boot 幂等碰撞面 | 🟠 实施细节 | `H-WP4-FW-06` / 实施细化 |

> ✅ **已关闭**：**`H-WP4C-03`**（设计冻结）· **`H-WP4C-04…08`**（实施阶段授权，§6A）—— 均不再列为剩余 `HUMAN_REQUIRED`。
> ⚠️ **`CLOSED` ≠ 执行完成**：`H-WP4C-04…08` 仅代表**授权已下达**；**执行**仍须按 `P3-WP4C-Implementation-Discipline.md` 的固定顺序逐步推进，**每步产出报告**。
> 🟠 **`H-WP4C-09` 已拆分（不关闭）**：**`-A`（只读导出 · ✅ `CLOSED WITH GUARD`）** 与 **`-B`（生产变更 · 🟡 `READY`）** 责任分离，**`-A` 完成 ≠ `-B` 可开工**（§6B —— `-B` 另须 `H-WP4C-11`）。
> ✅ **`H-WP4C-10` / `-10-A` 已由人工裁决关闭**（`2026-10-12`）：**`Action Target = Pages Proxy`**；派生的 **`H-WP4C-11`**（Pages 中转部署）**已 ✅ `CLOSED`**。
> ✅ **纪律 §6A.4 `STOP` 条款已无成立条件**：条件 1 = 目标可达（中转已部署 + 同 host 由 EMQX 生产投递作证）· 条件 2 = `V-0` 基线**已导出**。
> **⇒ 后续 EMQX 侧变更的准入，取决于 `H-WP4C-09-B` 的人工执行，而不再取决于任何未闭合前置。**
> 实施前准备清单与基线采集流程见 **`docs/review/P3-WP4C-Pre-Implementation-Checklist.md`**；控制台边界规则见纪律 **§6A**；
> **导出与回写目标文件**见 **`EMQX_Assets/config/data-integration/P3-WP4C-V0-EMQX-Baseline.md`**（`V-0` 基线）与同目录 **`P3-WP4C-V2V4-Config-After.md`**（变更后存档）。

---

## §9 边界核对（**零违规**）

| 禁止项 | 状态 |
|---|---|
| 创建 EMQX rule | ✅ **未创建** |
| 修改 EMQX 配置 / 连接器 / ACL | ✅ **未改**（EMQX 仓 HEAD 仍 `eec9444`） |
| 修改 Worker（`Cloudflare_Assets/guo-feeder-api`） | ✅ **未改**（源码零改动；线上仍 `99e6d937`，**未重部署**） |
| 修改 D1 migration | ✅ **未改** |
| 修改 `src/` / `include/` / `platformio.ini` / `partitions.csv` | ✅ **未改** |
| 修改 topic / `/up` / frozen telemetry schema | ✅ **未改** |
| 修改冻结设计文档（`P2-0-…freeze.md` / `DT:WP-4`） | ✅ **未改** |
| 启用 `GF_TELEMETRY_ENABLED` | ✅ **未启用**（保持 `0`） |
| `push` | ✅ **未 push** |
| **★ `2026-10-12` 第七阶段生产变更** | ⚠️ **1 次 Pages 部署**（**已授权** —— `H-WP4C-11`；`61b4938e-…`；**仅 Functions bundle**）+ **D1 合成测试行写入与定点清理**（`changes` 10 行 · 已复核归零） |
| `2026-10-12` 第七阶段**未做**的 | ✅ 未创建 / 修改任何 EMQX Rule · Action · Connector · 未部署 Worker · 未跑 migration · 未改固件 · 未改冻结设计 |
| 新增 / 修改文档 | ✅ **`docs/` + CF 仓新增 1 函数 + 1 冒烟脚本**（见 §10） |
| **前序阶段（第一…第六阶段）生产侧操作** | ✅ **零生产变更** —— 未向 EMQX 发起写请求 · 未部署 · 未执行 migration |

---

## §10 修改文件

### 10.1 本轮（**第九阶段 · `R3` 设计修订：连接器不可编辑 ⇒ 删除重建**）

| 文件 | 仓库 | 性质 |
|---|---|---|
| `docs/review/P3-WP4C-Console-Build-Guide-R3.md` | 固件 | **新增** —— **`R3` 施工说明（唯一施工依据）**：§1 平台事实 8 条（含官方原文）· §2 对象清单 + 依赖审计 + 配额账 · §3 施工前采集 6 项 · §4 拆除顺序（规则→动作→连接器）· §5 建连接器逐字段 · §6 建动作 ×2 · §7 建规则 ×2（含 `NOT` 与德摩根两种写法）· §8 时间线 · §9 验证 · §10 回滚 · §11 登记表 |
| `docs/review/P3-WP4C-Connector-Reuse-Amendment.md` | 固件 | **追加 `R3` 覆盖声明** —— §4.2 / §5 施工清单**作废**；新增风险 **`R-WP4C-18b`**（删除不可逆） |
| `docs/review/P3-WP4C-EMQX-Rule-Design.md` | 固件 | **升级 `R2` → `R3`** —— banner / §10（10.1 三栏对照 + 10.2 架构图 + §11 关联）；**语义逐字不变** |
| `docs/review/P3-WP4C-Implementation-Plan.md` | 固件 | **§8 规格 `v2` → `v3`** —— 新增 §8.0b 声明；§8.1 / §8.1b / §8.1c / §8.2 全部改为删除重建 |
| `docs/review/P3-WP4C-Decisions.md` | 固件 | **更新** —— banner 第九阶段 · §1 · §6B（`-09-B` → `READY (R3)`）· §7.1 · §7.2（`C-5`）· §7.3 · §8 · §10 |
| `docs/README.md` | 固件 | **索引** —— 新增施工说明条目 + 状态词同步 |
| `config/data-integration/README.md` · `P3-WP4C-V2V4-Config-After.md` | EMQX | **更新** —— §7.1.1 铁律改为「删除重建」；`V2V4` 回写模板改为 `R3` 形态（新 ID 登记 + 被删对象存证） |

> **★ 边界**：本阶段**零生产变更** —— **零 EMQX 写操作 · 零部署 · 零 migration · 零 firmware 修改 · 零 `push`**；**文档中不含任何密钥值**。

### 10.2 第七阶段（**`H-WP4C-11` Pages 中转部署与端到端验证**）

| 文件 | 仓库 | 性质 |
|---|---|---|
| `guo-feeder-pagesdev/functions/api/history/ingest.js` | **CF** | **新增** —— Pages 中转函数（136 行 · 逐字转发 + `X-Webhook-Key` 透传 + 上游不可达 `502`） |
| `scripts/smoke-history-relay.mjs` | **CF** | **新增** —— 部署后冒烟测试（只读 / `--write` 两态 · 不打印密钥 · 末尾打印定点清理 SQL） |
| `README.md` | **CF** | **更新** —— 记录新端点与冒烟脚本 |
| `docs/review/P3-WP4C-History-Relay-Deployment-Verification.md` | 固件 | **新增** —— **本工作包部署与验证记录**（部署事实 / 验证矩阵 / D1 落库证据 / 旧链路收存核查 / `V-1` 判定 / 新增 `R-WP4C-17`） |
| `docs/review/P3-WP4C-Decisions.md` | 固件 | **更新** —— 本文件（banner 第七阶段 · §1 · §6B（`H-WP4C-11` → `CLOSED`；`-09-B` → `READY`）· §7.1 / 7.2 / 7.3 · §8 · §9 · §10） |
| `docs/review/P3-WP4C-Implementation-Record.md` | 固件 | **回填** —— §5 / §6（`V-1` → `PASS`）/ §7（`R-WP4C-15` 补证 + 新增 `R-WP4C-17`） |
| `docs/review/P3-WP4C-Implementation-Discipline.md` | 固件 | **状态回填** —— §6A.3 准入序列 `V-1` 状态 + §6A.4 `STOP` 条款现行判定 |
| `docs/review/P3-WP4C-Action-Target-Decision.md` | 固件 | **状态回填** —— §5.2 开工门第 2 项（Pages Function）已由「未部署」→「已部署并验证」 |
| `docs/README.md` | 固件 | **索引** —— 新增本工作包验证记录条目 + 状态词同步 |
| `config/data-integration/P3-WP4C-V2V4-Config-After.md` · `README.md` | EMQX | **更新** —— 追加「中转已就绪」前置事实与冒烟结果 |

> **★ 边界**：本阶段**唯一生产变更 = 1 次已授权的 Pages 部署**；**零 EMQX 写操作 · 零 Worker 部署 · 零 migration · 零 firmware 修改 · 零 `push`**。

### 10.3 第四阶段（`H-WP4C-10-A` 裁决下达 + `V-0` 导出工件）

| 文件 | 仓库 | 性质 |
|---|---|---|
| `docs/review/P3-WP4C-Action-Target-Decision.md` | 固件 | **裁决回填** —— `DECISION PENDING` → **`DECIDED`（方案 A）**；新增 §1.3（收敛）· §5.1（裁决留痕）；§2/§3 标注采纳 / 排除；§6 转为开工门清单 |
| `docs/review/P3-WP4C-Decisions.md` | 固件 | **更新** —— banner · §1 · §6B（`-10/-10-A` → `CLOSED` + **新增 `H-WP4C-11`**）· §7.1 · §7.2 · **§7.3（`C-5` 依赖改为 `C-4` + `H-WP4C-11`）** · §8 |
| `docs/review/P3-WP4C-Implementation-Record.md` | 固件 | **回填** —— §5 拆分后形态同步 + `R-WP4C-12` 收敛 |
| `docs/review/P3-WP4C-Implementation-Discipline.md` | 固件 | **状态回填** —— §6A.3 准入序列 `V-1` 的当前状态与解锁条件 |
| `docs/README.md` | 固件 | **索引** —— 状态词同步（新增 `V-0` 工件指向） |
| `config/data-integration/P3-WP4C-V0-EMQX-Baseline.md` | EMQX | **新增 + 回填 + 收口** —— `V-0` **导出记录**（**`CLOSED WITH GUARD`**）：2 连接器 / 3 规则 SQL / 3 动作（含高级设置）+ D1 计数 + 回滚签署 + **限知项 §3.6**（TLS 不可回读）+ **异常 §3.7**（`R-WP4C-15` / `R-WP4C-16`） |
| `config/data-integration/P3-WP4C-V2V4-Config-After.md` | EMQX | **新增** —— 变更后（`V-2`/`V-4`）配置**回写模板** |
| `config/data-integration/README.md` | EMQX | **更新** —— 新增 §7 指向两个 WP4C 工件 |

> **★ 边界**：**全部为 `docs/`** —— 零 EMQX 写操作 · 零 Worker / Pages 部署 · 零 migration · 零 firmware 修改 · 零 `push`。

### 10.4 第三阶段（实施前裁决补充）

| 文件 | 性质 |
|---|---|
| `docs/review/P3-WP4C-Implementation-Discipline.md` | **增补** —— **§6A `EMQX Console Boundary Rule`**（控制台边界定性 · 代理禁止清单 · 准入序列 `V-0…V-5` · `STOP` 条款）+ §7 关联文档 + 页脚 |
| `docs/review/P3-WP4C-Action-Target-Decision.md` | **新增** —— `H-WP4C-10` / `H-WP4C-10-A` **待裁决记录**（方案 A / B · 优点 / 风险 · 证据对照 · 必答清单 · **不选择**） |
| `docs/review/P3-WP4C-Decisions.md` | **更新** —— 本文件（banner · §1 总览 · **§6B 裁决补充** · §7.1 状态 · §7.2 `CF-2` · **§7.3 实施门禁 Checklist** · §8 剩余 `HUMAN_REQUIRED`） |
| `docs/review/P3-WP4C-Implementation-Record.md` | **回填** —— §5 `H-WP4C-09` / `10` 登记同步为**拆分后形态**（跨文档一致性必需） |
| `docs/README.md` | **索引** —— 新增 `Action-Target-Decision` 条目；更新纪律 / 裁决条目的状态词 |

> **★ 边界**：以上**全部为 `docs/`** —— 零 EMQX 写操作 · 零 Worker 部署 · 零 migration · 零 firmware 修改 · 零 `push`。

### 10.5 第一 / 第二阶段（留存）

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
*`2026-10-12` **第六阶段收口**：**`V-0` = ✅ `CLOSED WITH GUARD`** · **`H-WP4C-09-A` = ✅ `CLOSED WITH GUARD`** · **`C-4` = ✅ `PASS`（带 guard）**（门禁 `PASS 4 / BLOCKED 3`）；
**新增 `R-WP4C-16`**（连接器 TLS 明细**保存后不可回读** ⇒ guard = 保存前截图 + 保存后实测）；**D1 基线已采**（`mm=2458`）；**回滚方案已签署**（代理拟制 · 人工执行）。
**唯一剩余前置 = `H-WP4C-11`**（Pages 中转部署）⇒ EMQX 侧变更继续 `STOP`。**仍为零生产变更**（EMQX 零写 · D1 仅只读 `SELECT`）。*
*`2026-10-12` **第七阶段（`H-WP4C-11` 部署与验证）**：人工明示授权后**新增并部署** Pages 中转函数
`guo-feeder-pagesdev/functions/api/history/ingest.js`（Pages 新部署 = **`61b4938e-fa2e-42da-b950-f527781f79f9`**）⇒
**端到端验证 `PASS 15 / FAIL 0`**（A 段 7/7 中转层 + 鉴权边界 · B 段 8/8 写入 · D1 落库 `telemetry ×4 + log ×1` **已定点清理**）⇒
**`H-WP4C-11 = ✅ CLOSED`** · **`V-1` = 🟢 `PASS`（带说明）** · **`H-WP4C-09-B` = 🟡 `READY`**（依赖已全部闭合）·
**新增 `R-WP4C-17`**（`INSERT OR IGNORE` 幂等冲突仍消耗 `AUTOINCREMENT`）· **`R-WP4C-15` 获新证据**（`…/log` 主题 **0 行**）。
**本阶段唯一生产变更 = 1 次已授权的 Pages 部署**；**零 EMQX 写 · 零 Worker 部署 · 零 migration · 零固件改动**。*
*`2026-10-12` **第八阶段（`R2` 设计修订 · Serverless 配额）**：人工核实配额**不可调整**（**连接器 ≤2，当前 2/2**）⇒
`R1` 的「新建独立连接器」不可行 ⇒ **`R2`** = **复用 `c-…b12100`（URL 收敛为 base）+ 动作级 `URL Path` 分流**；
新增 **`D-7`** · **`R-WP4C-18` / `-19`**；同批修正动作体 **`"payload": ${payload}` 不加引号**（带引号 ⇒ Worker `400`）。
**核心语义逐字不变**。*
*`2026-10-12` **第九阶段（`R3` 设计修订 · 连接器不可编辑）**：官方文档逐字确认 **「编辑连接器 / 启用禁用连接器：该功能仅适用于专有版部署」**
⇒ `R2` 的「就地改写连接器 URL」**不可执行** ⇒ **`R3`** = **删除重建**（删 `r-…a01720`/`a-…921e22`/`c-…b12100` ⇒ 建 `CB` + `MF`/`HI` + 规则 ×2）；
**冻结设计 → `R3`** · **规格 → `v3`** · **`-09-B` = 🟡 `READY (R3)`** · **新增 `R-WP4C-18b`（删除不可逆）**。
★ **净收益**：**`-S2` 双投窗口消失**（新消息流规则一出生即带 `NOT 谓词`）· 连接器内容**全新且全量留痕**。
★ **代价**：删除不可逆 ⇒ **施工前须抄录运行统计 + 截图配置**；中断窗口「≤60 s」→「**拆除→重建 ≤15 min**」（仅消息流 best-effort 链路）。
**施工规格（唯一依据）** → **`docs/review/P3-WP4C-Console-Build-Guide-R3.md`**。**本阶段零生产变更**（EMQX 零写 · 零部署 · 零固件改动）。*
