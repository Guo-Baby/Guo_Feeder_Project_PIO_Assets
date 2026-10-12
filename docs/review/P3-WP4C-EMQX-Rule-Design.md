# P3 · `H-WP4C-03` —— **EMQX Rule telemetry 分流设计**（🔒 FROZEN DESIGN **R3.1**）

> ## 🔒 `FROZEN` **R3.1** · **未实施 · 未创建任何 EMQX 规则 · 未改动任何 `src/` / Worker / migration**
>
> | 项 | 值 |
> |---|---|
> | **本文档性质** | **冻结设计（唯一实施依据 · `R3.1`）** —— 后续 EMQX 实施（控制台）**只能**依此执行（**形态以 §10 修订节为准 · 谓词写法以 §4.5 / §10.4 为准**） |
> | **冻结依据** | 本仓 `docs/review/P3-WP4C-Decisions.md`（`H-WP4C-03` 人工裁决） |
> | **基线** | 固件仓 HEAD = **`cdcc766`**（R1）→ **`def74f7`**（R2）→ **`1d15104`**（R3）→ **`4c74d3a`**（R3.1）· CF 仓 **`9b25203`** → **`415cd0c`** · EMQX 仓 **`eec9444`** → **`efa117d`** → **`3ea08fe`** → **`a73968a`** |
> | **上游依据** | `H-WP4-FW-01`（**已 `CLOSED`**：方案 A 双 Action 分流）· `DT:WP-4` §9.1（端点 = `POST /api/history/ingest`）· `cloud_protocol.md` §4.5（telemetry uplink 已冻结） |
> | **本文档不做的** | ❌ 不创建 / 修改 EMQX 规则或连接器 · ❌ 不改 Worker · ❌ 不改 migration · ❌ 不改 firmware · ❌ 不改 `/up` · ❌ 不部署 |
>
> **`H-WP4C-03` 状态 = `CLOSED`（本轮冻结）** —— 设计已冻结，**实施本身仍属后续工作包**。
> **`CF-2` 保持 `BLOCKED`**（本文档**不解除**任何 Gate / CLOSED 事实；见 §9）。
>
> ---
>
> ### 🔄 **`R2` 修订（`2026-10-12`）—— 平台配额 `D-7` 强制变更「连接器形态」**
>
> > **成因**：EMQX Cloud Serverless **数据集成配额不可调整** —— **连接器 ≤2（当前 2/2，余量 0）** ·
> > 规则 ≤4 · 动作 ≤4 · 单规则动作 =1。⇒ `R1` 依赖的「**新建独立连接器**」**在配额上不可能实现**。
> >
> > **处理**：新增 **§10（`R2` 修订节）** 与 **`D-7`**；**核心语义全部保留**（方案 A · 冻结谓词 · `-S2` · Pages Proxy · 端点 · `V-1…V-7`），
> > **仅变更连接器形态** = **单连接器复用 + 动作级 `URL Path` 分流**。
> >
> > **规格全文（配置项 / 施工顺序 / 窗口 / 回滚）** → **`docs/review/P3-WP4C-Connector-Reuse-Amendment.md`**（`DECIDED`）。
>
> ---
>
> ### 🔄 **`R3` 修订（`2026-10-12`）—— 平台的「连接器不可编辑」强制变更**施工形态**（**语义零变更**）**
>
> > **成因**：EMQX Cloud 官方文档明示 —— **「编辑连接器 / 启用禁用连接器：该功能**仅适用于专有版部署**」**
> > ⇒ **Serverless 连接器创建后不可编辑** ⇒ `R2` 的「改 `c-…b12100` 的 URL」**不可能执行**。
> >
> > **处理**：施工手段由「**就地编辑**」改为「**删除重建**」——
> > 删除 `c-…b12100` + `a-…921e22` + `r-…a01720`，改为**新建** 连接器 `CB` + 动作 `MF`/`HI` + 规则 ×2。
> >
> > **★ 语义仍逐字不变**：方案 A · 冻结谓词（§4）· `-S2` 目标（§5）· `Action Target = Pages Proxy` · 端点 `/api/history/ingest` · `V-1…V-7`（§7）。
> > **★ 且更优**：新消息流规则**一出生即带 `NOT 谓词`** ⇒ §5.1 的**双投窗口消失**。
> > **★ 代价**：删除**不可逆**（旧 ID 消失）⇒ 施工前必须抄录运行统计 + 截图配置（新增风险 `R-WP4C-18b`）；中断窗口由「≤60 s」变为「拆除→重建 ≤15 min」（**仅消息流 best-effort 链路**）。
> >
> > **施工规格（唯一依据）** → **`docs/review/P3-WP4C-Console-Build-Guide-R3.md`**。
>
> ---
>
> ### 🔄 **`R3.1` 修订（`2026-10-12`）—— 控制台实测 `==` 语法错 ⇒ 修正「SQL 谓词写法」（**语义零变更**）**
>
> > **成因**：控制台保存规则时报 **`11: syntax error before: '='`**；解析器 trace 显示
> > `payload.c == 'undefined'` 被词法拆成**两个** `=` token ⇒ **EMQX 规则 SQL 的"相等"只有单个 `=`，`==` 不存在**。
> > 且"键是否存在"应当用**官方内置函数** `is_null()` / `is_not_null()`（而非 `!= 'undefined'` 字符串比较）。
> >
> > **处理**：**仅改写法** —— 谓词改为 `is_not_null(payload.<五键>)` ∧ `is_null(payload.c)` ∧ `is_null(payload.cmd)`；旧规则取反 = 之德摩根展开（只用 `OR`/`AND`/`is_*`）。
> > **新增强制验证步**：保存前必须用规则页 **「启用调试 + 发送测试消息」** 跑两条样例（见 §10.4）。
> >
> > **★ 语义仍逐字不变**（§4.1 的**逻辑谓词**未动；本节属 §4.5「非冻结契约」范围内修正）。
> > **明细** → **§10.4** · **风险登记** → **`R-WP4C-20`**。
> >
> > **英文冻结声明（R3.1）**：*`R3.1` corrects the SQL predicate **syntax** only (`==` is invalid; use `is_null`/`is_not_null`).
> > The frozen routing **semantics** are unchanged.*
>
> > **英文冻结声明（总）**：*`H-WP4C-03` Frozen means the EMQX telemetry routing design is frozen.
> > It does not mean the rule has been created, the Worker/migration changed, or the pipeline deployed.
> > **R2/R3 change the connector topology / build method only — not the routing semantics.***

---

## §0 依据与约束（不可协商）

| # | 约束 | 来源 |
|---|---|---|
| C-1 | **EMQX Rule 是唯一分流责任方** | `H-WP4-FW-01`（`CLOSED`） |
| C-2 | **Worker 不解析 telemetry、不做 command/telemetry 判断** | `H-WP4-FW-01` · `DT:WP-4` §5.1.1 |
| C-3 | **firmware 不得修改 topic** | `H-WP4C-03` 冻结约束 · `DT:WP-4` 入口族冻结 |
| C-4 | **不得修改 `/up`**（frozen topic） | `DT:WP-4` 冻结 · P2 ACL 5/5 覆盖 |
| C-5 | 分流**不得**引入 Worker policy | `DT:WP-4` §5.1.1（"禁止"清单） |
| C-6 | **不得依赖 MQTT v5 metadata / user property** | 真机 `proto_ver = 4`（§6 `D-5`） |
| C-7 | **不得新增 telemetry topic** | `H-WP4C-03` 冻结约束（§3 方案 C 已排除） |

---

## §1 问题陈述

### 1.1 现状：`/up` 上的**三类载荷混流**

```
device ──MQTT──► guo_feeder/<device_id>/up
                     ├─ (a) 命令结果      {"c":"result","i":…,"s":…,"o":…}     ← 经 cloud_compress_uplink
                     ├─ (b) 上线通知      {"cmd":"system","id":"online",…}     ← cloud_send_set
                     └─ (c) telemetry v1 {"ts":…,"type":…,"value":…,"seq":…,"schema_version":1}  ← 原样 JSON（不压缩）
```

- (a)/(b) 是**命令链路**（消费方 = `mqtt_messages` 记账 + 命令可观测性）；
- (c) 是**遥测链路**（消费方 = `telemetry_history`，经 `POST /api/history/ingest`）；
- **三者共用同一 topic** —— 这是 `DT:WP-4` 入口族冻结（`/up` + `/log`）的**直接后果**。

### 1.2 为什么必须分流

| # | 事实 | 后果 |
|---|---|---|
| P-1 | `DT:WP-4` 的 ingest 对"缺 `ts`/`type`/`value`/`seq`"的记录**逐条 `rejected`** | 若命令结果被投到 history 端点 ⇒ **逐条 4xx**（噪声 + 重试） |
| P-2 | telemetry 若被投到**命令端点** ⇒ 进入 `mqtt_messages` 记账 | 污染记账链路、放大写量 |
| P-3 | **既有消息规则 `guo_feeder/#` 同样匹配 `/up` 与 `/log`** | ⇒ **telemetry 会被既有规则一并捕获**（见 §5 的关键推论） |

### 1.3 分流目标（一句话）

**同一条 `/up` 上的 telemetry 报文 → `POST /api/history/ingest`；命令结果 / 上线通知 → 保持既有链路不变。**
**且**：不得产生**双投**（同一条报文同时进两条链路）。

---

## §2 设计候选（评估留痕）

> 三个候选**均不修改 `/up`**（C-4）。差别在"判别依据"与"改动落在哪一层"。
> 本节保留评估过程；**冻结结论见 §3**。

### 2.1 方案 A —— **payload schema 判断**（EMQX Rule SQL 谓词）

**判别依据**：报文的 **payload 字段集合**（见 §4 冻结谓词）。

**实现形态**：EMQX Rule 的 `WHERE` 子句做字段存在性判断（JSON 解码后）。**两条规则 + 非重叠谓词**：

| 规则 | FROM | WHERE | Action |
|---|---|---|---|
| `guo_feeder_history_ingest`（**新增**） | `guo_feeder/+/up` | **telemetry 谓词**（§4） | `POST …/api/history/ingest` |
| 既有消息 ingest 规则（**修改 WHERE**） | `guo_feeder/#` | **`¬` telemetry 谓词**（见 §5） | 既有端点（不变） |

| 维度 | 评估 |
|---|---|
| **改动范围** | EMQX 侧：**1 新增规则 + 1 既有规则 WHERE 收窄**（控制台操作）；firmware / Worker / D1 **零改动** |
| **backward compatibility** | ✅ **命令链路零改动**（谓词只做"排除"，不改变其余报文的路由）；⚠️ 既有规则的 WHERE 一旦写错会**误伤命令结果** ⇒ 须按 §7 验证 |
| **firmware impact** | ✅ **零** |
| **cloud complexity** | 中：EMQX 谓词需在**真实版本**上验证语法与语义（§7 V-1 / V-4） |

### 2.2 方案 B —— **metadata / header 判断**（🔴 已排除）

**判别依据**：MQTT **user property**（v5）或其它消息元数据。

| 维度 | 评估 |
|---|---|
| **可行性** | 🔴 **不可行（证据级）** —— 真机 `proto_ver = 4`（**MQTT 3.1.1**，见 EMQX 仓 §6.10 实测）⇒ **无 user property 概念**。要启用需**同时**升级 firmware MQTT 客户端到 v5 **并**确认 broker / ACL 兼容 |
| **改动范围** | firmware（`esp-mqtt` v5 user property 设置）+ 可能 broker 侧配置 |
| **backward compatibility** | ⚠️ 协议层升级影响**全部**上行/下行报文（远超本议题） |
| **firmware impact** | 🔴 **高**（违反 C-3 / C-6 精神） |
| **cloud complexity** | 低（元数据判别本身简单） |

> ⇒ **B 被排除**：前提（MQTT v5）当前**不成立**，且改动远超"分流"议题。**冻结为禁止项（C-6）。**

### 2.3 方案 C —— **topic extension**（🔴 已排除）

**判别依据**：独立 topic（如 `guo_feeder/<device_id>/up/telemetry` 或 `guo_feeder/<device_id>/telemetry`）。

| 维度 | 评估 |
|---|---|
| **改动范围** | firmware（新增 topic 渲染 + 发布路径）+ **ACL 新增条目**（现 ACL v3 `deviceRuleTemplate` 仅 3 条：sub `down` / pub `up` / pub `log`）+ `DT:WP-4` **入口族冻结修正** + EMQX Rule |
| **backward compatibility** | ✅ 旧 topic 不变；但**引入第三/第四条入口** ⇒ `DT:WP-4` 的"域→表映射"需重开 |
| **firmware impact** | 🔴 **高** —— 违反 **C-3（firmware 不得修改 topic）** ⇒ **需先解冻** |
| **cloud complexity** | 低（topic 匹配最干净、零 payload 解析） |

> ⇒ **C 被排除**（**冻结为禁止项 C-7**）：域隔离最彻底，但代价最高 —— 它同时触发 **firmware 改动 + ACL 扩张 + 冻结文档修正**三项。

### 2.4 对比矩阵

| 维度 | **A（payload 谓词）** | B（metadata） | C（topic extension） |
|---|---|---|---|
| 修改 firmware | ✅ **否** | ❌ 是（v5 升级） | ❌ 是（topic 渲染） |
| 修改 frozen topic `/up` | ✅ **否** | ✅ 否 | ✅ 否（新增） |
| 新增 topic / ACL 条目 | ✅ **否** | ✅ 否 | ❌ 是（+ACL 扩张） |
| 修改既有 EMQX 规则 | ⚠️ **是**（WHERE 收窄） | 是 | 是 |
| 迁移成本 | **最低** | 高（协议升级） | 中高（三项联动） |
| payload inspection | ⚠️ 有（EMQX 层） | ✅ 无 | ✅ 无 |
| 对 `/up` 混合的容忍 | 需谓词可靠 | — | 根除 |
| 当前可行 | ✅ **是** | 🔴 **否**（proto_ver 4） | ⚠️ 需解冻 ⇒ **禁** |

---

## §3 🔒 冻结分流方案 = **A（EMQX Rule Engine payload predicate routing）**

### 3.1 冻结原文

> **冻结分流方案 = EMQX Rule Engine payload predicate routing（方案 A）。**
>
> **禁止**：
> - ❌ firmware topic 修改
> - ❌ MQTT v5 metadata 依赖
> - ❌ Worker telemetry parsing
> - ❌ 新增 telemetry topic

### 3.2 冻结架构

```
                          MQTT
                           │
              guo_feeder/<device_id>/up
                           │
                           ▼
                  ┌─────────────────┐
                  │ EMQX Rule Engine│   ← 唯一分流责任方（C-1）
                  └────────┬────────┘
                           │  payload predicate（§4）
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

### 3.3 冻结理由

1. **唯一同时满足三项硬要求**：**不修改 firmware** ✅ · **不修改 frozen topic** ✅ · **最小迁移成本** ✅。
2. **C-1/C-2 合规**：分流**完全在 EMQX 层**发生；Worker 仍只做"收到即写"（`DT:WP-4` §5.1.1 的 structural validation 边界**不变**）。
3. **B 当前不可行**（`proto_ver = 4`），C 被 C-7 禁止 —— 二者都不是可落地选项。
4. **可逆性最好**：谓词是纯"路由选择"，**不改变任何 payload 语义**；回滚 = 停用新规则 + 恢复既有 WHERE。

### 3.4 ★ 与 `H-WP4-FW-01` "topic/domain 分离优先" 的张力 —— **已由本裁决消解**

`H-WP4-FW-01` 的冻结原文含 **"topic / domain separation 优先于 payload inspection"**。
**但在 `/up` 共用且冻结不变的前提下，纯 topic 分离在数学上不可能** —— 判别信息**只能**来自 payload 或元数据。

**本裁决的读法（冻结）**：「**优先**」= **偏好**（在**可选**时优先 topic 分离）。
⇒ 当前 topic 分离**不可选**（方案 C 被 C-7 禁止、方案 B 前提不成立）⇒ **A 是唯一可落地项，合规。**
并已确认：`H-WP4-FW-01` 的"Worker 不解析 payload"禁令**针对 Worker**；A 的谓词发生在 **EMQX Rule**，**不违反** C-2 的字面。

> 本节替代原 `scope proposal` 的待裁点 `H-WP4C-03-S1`（**已由人工裁决关闭**）。

---

## §4 🔒 冻结 telemetry 判别条件（Rule predicate）

### 4.1 冻结原文

> **Rule predicate 必须同时满足：**

```
telemetry  ⟺  (
                 schema_version 存在
             AND seq            存在
             AND ts             存在
             AND type           存在
             AND value          存在
              )
             AND NOT command envelope
             AND NOT command result envelope
```

形式化为：

```
telemetry  ⟺  ( schema_version ∧ seq ∧ ts ∧ type ∧ value )
             ∧ ¬ c
             ∧ ¬ cmd
```

### 4.2 逐条判据与理由

| 判据 | 角色 | 理由 |
|---|---|---|
| `schema_version` 存在 | **必填** | ★ **最强判别子** —— 命令结果 / 上线通知**从不**携带该字段 |
| `seq` 存在 | **必填** | v1 必填 |
| `ts` 存在 | **必填** | v1 必填 |
| `type` 存在 | **必填** | v1 必填 |
| `value` 存在 | **必填**（**含 `0` / `false` 也必须算"存在"** —— 用"**键存在**"而非"值非空"判断） | v1 必填 |
| `¬ c` | 排除 | 命令结果经缩写后**必有** `c` |
| `¬ cmd` | 排除 | 上线通知 / 错误结果**含** `cmd` |

### 4.3 🔒 冻结禁止项（判别条件）

> **禁止**：
> - ❌ 仅判断 `type`
> - ❌ 仅判断 `value`
> - ❌ 仅判断 topic
>
> **原因**：**command payload 内可能出现业务字段** —— 命令结果的 `o`（object）内可能含业务 `type` 字段（如 workflow / action 对象）
> ⇒ **单字段判别会产生假阳性** ⇒ 必须以 **`schema_version` + `seq` + `ts` + `type` + `value` 的合取 + 两个排除项**为准。

> **★ 语义注记（实现级）**：`value` 的存在性判断**必须**用"键存在"语义（`has` / `is_not_null` 一类），
> **不得**写成"值非空 / 值 ≠ 0"（否则 `value=0`、`value=false` 的合法样本会被漏判 ⇒ 假阴性）。
> 谓词**不涉及** `device_id` —— 身份**只**来自 topic（与 `cloud_protocol.md` §4.5 一致：`device_id` 不进 payload）。

### 4.4 反例守卫（**默认落入命令链路**）

**未匹配 telemetry 谓词的一切报文 → 命令链路**（保持既有行为）。
⇒ 未来的新上行类型**默认不会被误判为 telemetry**（fail-safe 方向正确：宁可命令链路多收，不可污染历史表）。

### 4.5 SQL 形态草稿（**非冻结契约 · `R3.1` 已按控制台实测修正**）

> ⚠️ **`R3.1`（`2026-10-12`）修正**：本节的 `!= 'undefined'` 形态**已被控制台实测否决**（两处错误）：
> ① **`==` 不存在** —— EMQX 规则 SQL 的"相等"只有**单个 `=`** ⇒ `payload.c == 'undefined'` 报 **`syntax error before: '='`**（控制台实测）；
> ② **"键是否存在"应使用官方内置函数 `is_null()` / `is_not_null()`**（`is_null(Term)` = 判 **undefined/未赋值**；`is_not_null(Term)` = 反之；**路径中间字段缺失亦返回 `true`**）。
> ⇒ 下表为**修正后形态**；`'undefined'` 字符串比较仅作**最后备选**（且必须单 `=`）。
> **冻结的是 §4.1 的逻辑谓词**（键存在语义），**不是字面写法**。

**首选（`R3.1` 定稿）**：

```
SELECT
  *
FROM
  "guo_feeder/+/up"
WHERE
      is_not_null(payload.schema_version)
  AND is_not_null(payload.seq)
  AND is_not_null(payload.ts)
  AND is_not_null(payload.type)
  AND is_not_null(payload.value)
  AND is_null(payload.c)
  AND is_null(payload.cmd)
```

**最后备选（仅当 `is_null` 报"未知函数" · 注意 `=` 是单等号）**：

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
  AND payload.c               = 'undefined'
  AND payload.cmd             = 'undefined'
```

> ★ **`SELECT *` 而非 `SELECT json_encode(…) AS body`**：动作体引用 `${topic}` / `${clientid}` / `${payload}`，
> 若只输出 `body` 一个字段 ⇒ 变量取空 ⇒ 投递失败（`R2` §6.1 已定案）。`SELECT *` 与生产在跑的消息流规则同构。

（既有消息规则 WHERE 取**上式之否定**，见 §5；`R3.1` 下即 `is_null(...) OR ... OR is_not_null(payload.c) OR is_not_null(payload.cmd)`。）

---

## §5 🔒 冻结旧 Rule 处理策略（`-S2`）

### 5.1 ★ 关键推论：**必须同时收窄既有规则，否则会产生双投**

既有消息规则 FROM = **`guo_feeder/#`** ⇒ 它**已经**匹配 `/up` 上的 telemetry 与 `/log`。
若只"新增一条 history 规则"而**不动既有规则**，则 telemetry 报文会**同时**：
- 投递到 `POST /api/history/ingest`（新规则）✅
- 投递到既有 ingest 端点（既有规则）❌ ⇒ **进入 `mqtt_messages` 记账**

### 5.2 冻结原文

> **新增 telemetry rule 时：**
> **旧规则（FROM `guo_feeder/#` → ACTION `mqtt_messages`）不能保持无限匹配。**
>
> **冻结策略**：
> ```
> mqtt_messages rule  =  guo_feeder/#  AND  NOT telemetry_predicate
> ```
> 即：**普通消息进入 `mqtt_messages`；telemetry 只进入 history。**

### 5.3 冻结落地步骤（两步**均强制**，缺一即违规）

| 步 | 动作 | 性质 |
|---|---|---|
| **①** | **新增**规则 `guo_feeder_history_ingest`（FROM `guo_feeder/+/up`，WHERE = §4.1 谓词，Action → `POST …/api/history/ingest`） | 新增 |
| **②** | **收窄**既有消息规则的 `WHERE` 为 **`¬` telemetry 谓词**（`guo_feeder/# AND NOT telemetry_predicate`） | **修改既有生产链路** |

> ⚠️ **②是不可省略的一步**，且是**本次实施的主要风险点**（写错会误伤命令结果 ⇒ `R-WP4C-1`）。

### 5.4 冻结动机

> **避免**：duplicate storage（双投）· action 次数增加（白计费）· 后续统计污染。

### 5.5 ⚠️ 附带观察（**不属本议题，仅登记**）

`/log` 自 `dae7966` 起已改为 **JSON 信封**（不再是二进制 CBOR）
⇒ 既有 `guo_feeder/#` 规则**现在也会匹配 `/log`**，将其写入 `mqtt_messages`。
该行为是否可接受**不在 `H-WP4C-03` 范围**（属 log 链路议题），但**应在实施时一并知晓**（见 §8 `R-WP4C-10`）。

---

## §6 🔒 冻结 EMQX 平台限制说明（**实测事实 · 直接影响实施方式**）

| # | 约束 | 来源 | 影响 |
|---|---|---|---|
| **D-1** | **EMQX Cloud Serverless 的规则 / 连接器 / 动作只能控制台配置**（API `GET /rule_engine/*`、`/connectors`、`/actions` **全 403**） | EMQX 仓 `config/data-integration/README.md` §1 | ⇒ 本设计**无法脚本化落地**，须**控制台人工操作 + 仓库存档** |
| **D-2** | **EMQX 打不通 `*.workers.dev`**（连接被拒），但可达 `*.pages.dev` | EMQX 仓 §6.0.1 | ⇒ telemetry 的 HTTP Action **不得**直指 Worker 域名，须经 **Pages 中转** |
| **D-3** | 数据集成**按动作执行次数计费**；`guo_feeder/#` 规则当前覆盖 `…/up` | EMQX 仓 §6.4 | ⇒ 谓词必须**精确**，避免把命令结果引流到历史端点造成无谓计费（`R-WP4C-5` / `R-WP4C-9`） |
| **D-4** | EMQX 把**非 2xx 视为失败并重试** | EMQX 仓 §6.4 | ⇒ 分流错误会**放大**为持续重试（与 `DT:WP-4` §7.4 的失败模型相互印证） |
| **D-5** | 真机 **`proto_ver = 4`**（MQTT 3.1.1） | EMQX 仓 §6.10 | ⇒ **方案 B 不可用**（§2.2 / C-6） |
| **D-6** | Serverless 规则 SQL 为**子集**，函数可用性依版本而异 | EMQX 平台性质 | ⇒ 谓词语法**必须**先在控制台验证再保存（§7 V-1 / `R-WP4C-3` / `R-WP4C-7`） |
| **`D-7`** ⭐ **R2 新增** | **数据集成配额（不可调整）：连接器 ≤2 · 规则 ≤4 · 动作 ≤4 · 单规则动作 = 1** | 控制台实测（人工 · `2026-10-12`）+ EMQX Cloud 文档《数据集成简介》 | ⇒ **"一个端点一个连接器"不可行** ⇒ `R2` 修订：**单连接器复用 + 动作级 `URL Path` 分流**（§10）；**后续任何新增端点都不得再依赖新连接器** |

### 6.1 冻结实施方式（因人工作业而必须固化）

> **当前环境**：EMQX Cloud Serverless。**Rule / Connector / Action 创建需要控制台操作**；**API 自动化不可用（403）**；**Worker 无法作为直接 action endpoint**（D-2）。
>
> **因此实施阶段需要**：

```
Human operation:
    EMQX Console（创建/收窄规则 + 连接器 + 动作）

   +
Verification:
    device publish
        ↓
    Rule match
        ↓
    D1 history insert
```

> **★ 配置漂移控制（强制）**：控制台所做的每一处配置（规则 SQL / 连接器 URL / 动作请求头）**必须回写**
> `EMQX_Assets/config/data-integration/README.md` 存档（沿用该文档既有做法），否则**不可审计、不可重建**（`R-WP4C-6`）。

---

## §7 🔒 冻结验证计划（**全部 `NOT RUN`**）

> 未授权创建规则 ⇒ **本轮零执行**。以下为 freeze 后须执行的**判据**，**冻结不可删改**。

| # | 用例 | 输入 / 操作 | 期望 |
|---|---|---|---|
| **V-1** | **Rule syntax** | 在控制台保存并启用两条规则（新 history 规则 + 收窄后的既有规则） | 规则**可以保存并启用**（SQL 在部署版本上可解析，无语法/函数错误） |
| **V-2** | **telemetry positive** | 发送 `{"schema_version":1,"ts":123,"type":"temperature","value":25.5,"seq":1}` | `telemetry_history` **+1**；`mqtt_messages` **+0** |
| **V-3** | **command negative** | 发送 command（命令结果 / 上线通知） | `telemetry_history` **+0**；`mqtt_messages` **+1** |
| **V-4** | **mixed payload** | 载荷**包含** `type` + `value`，**但存在** `cmd` | **判定为"不是 telemetry"** ⇒ `telemetry_history` +0；`mqtt_messages` +1 |
| **V-5** | **multi device isolation** | device A 发 telemetry | **不能**进入 **device B** 的 history（按 `device_id` 隔离，`device_id` 取自 topic） |
| **V-6** | **reconnect** | 设备 MQTT 重连后重复上行 | Rule **不因 MQTT reconnect 重复写入** |
| **V-7** | **duplicate** | 相同 `(device_id, type, ts, seq)` 重复投递 | 验证 D1 **unique constraint**（`ux_th_dedup`）生效，落库**恰 1 行** |

**补充前置（STATIC · 离线 · 不计入冻结列表）**：谓词对 (a) 命令结果 / (b) 上线通知 / (c) telemetry v1 / (d) registry
结果**四类真实样本**的**真值表**校验（可用仓库内样本离线断言）—— 作为 V-1 上线前的**廉价前置筛查**（原提案的 `V-1 真值表` 归入此处）。

**执行前置**：`D-1`（控制台）+ **Pages 中转就绪**（`D-2`）+ 端点已部署（`H-WP4C-04` / `05`）。
原提案的 `V-7 Pages 中转可达性`（Action 能到达中转 → 中转能到 Worker，返回 `401`/`200` 而非 502/超时）**归入执行前置核查**。

> **编号说明**：本节 `V-1…V-7` 采用**冻结裁决**的定义（与 `P3-WP4C-Decisions.md` 一致）。
> 原 `scope proposal` 的 7 条编号中，"谓词真值表"→**本补充前置**，"Pages 中转可达性"→**执行前置**，
> 其余四条（语法 / 正例 / 反例 / 假阳性守卫）**语义已并入** V-1 / V-2 / V-3 / V-4。**实现层面无信息丢失。**

---

## §8 风险登记（含本轮新增）

| ID | 级别 | 风险 | 缓解 |
|---|---|---|---|
| `R-WP4C-1` | 🔴 高 | **既有规则 WHERE 写错 ⇒ 命令结果被误投历史端点**（逐条 4xx + 重试 + 命令可观测性断链） | V-3 必测；先在**测试设备**上验证；保留即时回滚（恢复 WHERE） |
| `R-WP4C-2` | 🟠 中 | **谓词假阴性 ⇒ telemetry 静默丢失**（落入命令链路） | 补充前置真值表 + V-2；谓词取"合取 + **键存在**"语义；上线后核对 `telemetry_history` 行数 |
| `R-WP4C-3` | 🟠 中 | EMQX 版本差异 ⇒ SQL 函数不可用 | V-1 先在控制台验证语法再保存 |
| `R-WP4C-4` | 🟠 中 | **双投**（遗漏 §5 的既有规则收窄） | 把"收窄既有 WHERE"列为**强制步骤**并纳入 V-2/V-3 |
| `R-WP4C-5` | 🟡 低 | 计费放大（谓词不准导致无谓动作执行） | 补充前置真值表 + 上线后观测动作计数 |
| `R-WP4C-6` | 🟡 低 | 控制台配置**无 API 自动化** ⇒ 漂移不可审计 | 按 EMQX 仓既有做法：**控制台配置必须回写仓库存档**（§6.1） |
| **`R-WP4C-7`** | 🟠 **中** | **★ 新增 —— Rule Engine 限制**：Serverless 规则 SQL 为**子集**（`D-6`），JSON 解码 / 存在性函数（`has` / `is_not_null` / `json_decode`）在**具体版本**上可能**缺失或语义不同**；规则规模 / 单规则字段数亦有上限 | V-1 强制；谓词**结构**保持最小（仅 7 个字段谓词）；**保留纯 topic 兜底**（`guo_feeder/+/up` 已是最小匹配面）；若函数不可用 ⇒ 记录并在实施阶段**回退到可用等价写法**（属实施细化，**不改冻结谓词逻辑**） |
| **`R-WP4C-8`** | 🟠 **中** | **★ 新增 —— D1 action 限制**：telemetry 为**周期性写入**（与 presence 低频不同）⇒ ① Pages 中转 + Worker + D1 三段链路**增加 RTT**，EMQX Action 超时后**按 `D-4` 重试** ⇒ 重复动作 + 重复计费；② D1**写入吞吐 / 单次请求写量**限制 ⇒ 高频下可能 `d1_batch_failed`（500）⇒ 触发重试 | 上线前用**单台设备**小流量试跑并观测 Action 成功率 / D1 写量；**不新增 batch**（`count=1`，`DT:WP-4` 冻结）；若吞吐不足 ⇒ **上报并暂停放量**（不自行放宽冻结） |
| **`R-WP4C-9`** | 🟠 **中** | **★ 新增 —— 成本风险**：数据集成**按动作次数计费**（`D-3`）；telemetry 默认 `interval_ms = 300000`（5 min）× **3 个 KEEP 通道**（temperature / humidity / weight，每通道**独立报文**）⇒ 理论 **≈ 864 条/天/设备**（1×288×3），**比 presence（≈ 22 条/天）高约 1–2 个数量级**；阈值触发可能进一步增加 | **登记并要求实施前量化**：以设备台数 × 周期 × 通道数估算日动作次数，与预算比对；如需抑制 ⇒ **只能调 `payload` 侧策略**（改造 `telemetry.interval_ms` / 收敛 KEEP 集 ⇒ 属 `H-WP4-FW-06` / 范围决策），**不得**在本工作包内改冻结协议 |

---

## §9 状态与边界

### 9.1 `H-WP4C-03` 状态

| 项 | 值 |
|---|---|
| **ID** | `H-WP4C-03` |
| **状态** | 🔒 **`CLOSED`**（设计冻结；**实施本身仍属后续工作包**） |
| **冻结方案** | **A（EMQX Rule Engine payload predicate routing）** |
| **冻结谓词** | `( schema_version ∧ seq ∧ ts ∧ type ∧ value ) ∧ ¬ c ∧ ¬ cmd`（§4） |
| **冻结旧 Rule 策略** | `mqtt_messages rule = guo_feeder/# AND NOT telemetry_predicate`（§5 · 两步强制） |
| **冻结平台限制** | `D-1…D-6` + 实施方式（§6） |
| **冻结验证计划** | `V-1…V-7`（§7） |
| **明确不做** | 不新增 topic · 不改 ACL · 不改 firmware · 不改 Worker · 不改 `/up` · 不改 `DT:WP-4` 契约 · 不创建 EMQX 规则 |
| **与 Gate 的关系** | **不解除任何 Gate**；`CF-2` **仍 `BLOCKED`**（受 `H-WP4C-04/05/06` 阻塞） |

### 9.2 边界核对（**零违规**）

| 禁止项 | 状态 |
|---|---|
| 创建 EMQX rule | ✅ **未创建** |
| 修改 EMQX 配置 / 连接器 / ACL | ✅ **未改** |
| 修改 Worker | ✅ **未改** |
| 修改 D1 migration | ✅ **未改** |
| 修改 `src/` / `/up` / topic | ✅ **未改** |
| 修改冻结设计文档（`P2-0-…freeze.md` / `DT:WP-4`） | ✅ **未改** |
| 新增文档 | ✅ `docs/review/P3-WP4C-EMQX-Rule-Design.md`（本文件，冻结升级）+ `docs/review/P3-WP4C-Decisions.md`（裁决记录） |

---

## §10 🔄 **`R2` / `R3` / `R3.1` 修订 —— 连接器形态 + SQL 写法（平台约束强制）**

> **`R1` 的可实施性依赖「新建独立连接器」；`D-7`（配额 2/2）使其不可能** ⇒ `R2` 改为**复用**；
> 又因**连接器不可编辑**（仅专有版可编辑）⇒ `R3` 改为**删除重建**；
> 再因**控制台实测 `==` 语法错** ⇒ **`R3.1` 修正谓词写法**。本节为**唯一的形态/写法修正**。
> **核心语义（方案 A · 冻结谓词 §4 的逻辑 · `-S2` §5 · Pages Proxy · 端点 `/api/history/ingest` · `V-1…V-7`）逐字不变。**

### 10.1 修订前后对照

| 项 | `R1`（原冻结） | `R2` | **`R3`（现行）** |
|---|---|---|---|
| 连接器 | **新建** `guo_feeder_history_http`（URL = `…/api/history/ingest`） | **复用既有** `c-n302933b-b12100`，URL 收敛为 **base** | **删除** `c-…b12100` + **新建** `CB`（URL = **裸主机 base** `https://guo-feeder-pagesdev.pages.dev`） |
| 端点定位 | 连接器 URL 含完整路径 | **动作级 `URL Path`**：`/api/webhook` · `/api/history/ingest` | **同 `R2`**（`MF` → `/api/webhook` · `HI` → `/api/history/ingest`） |
| 鉴权头 | 新建连接器自带 `X-Webhook-Key` | 补到**复用连接器** | **落在 `HI` 动作层**（动作支持独立请求头 ⇒ 消息流链路**不带**该头） |
| 消息流规则 | 既有规则**就地收窄** `WHERE` | 同 `R1` | **删除重建**，**直接写成** `WHERE = NOT 谓词` ⇒ **双投窗口消失** |
| 规则 / 动作 SQL 语义 | 不变 | **不变** | **不变**（谓词 · `-S2` 目标 · 端点 · 3 键体契约） |
| 动作请求体 | `{"topic":"${topic}","clientid":"${clientid}","payload":"${payload}"}`（**§8.1 原文**） | **修正为不加引号**：`"payload": ${payload}` —— 理由见 §10.3 | **同 `R2`** |

### 10.2 冻结架构（`R3` 形态 · 语义与 `R2` 等价）

```
        guo_feeder/<device_id>/up
                  │
        ┌─────────┴─────────┐
        │  EMQX Rule Engine │  ← 唯一分流责任方（**不变**）
        └─────────┬─────────┘
   telemetry 谓词真 │ 假
        ┌─────────┴─────────┐
        ▼                   ▼
  动作 HI（新建）        动作 MF（新建）
  Path /api/history/ingest  Path /api/webhook
        └────────┬──────────┘
                 │  共用同一连接器 CB（新建 · base = 裸主机）
                 ▼
        https://guo-feeder-pagesdev.pages.dev/<path>
                 │
        ┌────────┴────────┐
        ▼                 ▼
  /api/history/ingest  /api/webhook
   telemetry_history   mqtt_messages
```

> **`R3` 对象**：连接器 `CB` · 动作 `MF`（消息流）/ `HI`（history）· 规则 `guo_feeder_message_ingest` / `guo_feeder_history_ingest`
> —— 取代被删的 `c-…b12100` / `a-…921e22` / `r-…a01720`（**新 ID 施工时登记**）。
> **presence 链路（1 连接器 + 2 动作 + 2 规则）零接触。**

### 10.3 同批修正的规格缺陷（**未冻结部分 · 允许修正**）

`R1` 的 `Implementation-Plan` §8.1 存在**两处内部不自洽**（照原样施工 ⇒ **`V-2` 必失败**）：

1. 规则 `SELECT … AS body`（只输出 `body`）↔ 动作体引用 `${topic}/${clientid}/${payload}`（**字段不存在**）；
2. 动作体 `"payload":"${payload}"`（**带引号**）⇒ payload 变**字符串** ⇒ Worker `history-ingest.js` 的 `extractRecords()` 对 telemetry 返回 `null` ⇒ **`400 unparsable_payload`**。

**`R2` 定案**（与**已在生产验证**的消息流动作同构）：
**规则** `SELECT * FROM "guo_feeder/+/up" WHERE <冻结谓词>` · **动作体** `{"topic":"${topic}","clientid":"${clientid}","payload": ${payload}}`（**不加引号**）。

> ⚠️ §4.5 的「SQL 形态**非冻结契约**」在此适用；**谓词逻辑本身（§4.1）仍逐字冻结**。
> 明细与全部候选评估 → **`docs/review/P3-WP4C-Connector-Reuse-Amendment.md`**。

### 10.4 🔄 **`R3.1` —— SQL 谓词写法修正（控制台实测驱动）**

> **成因**：`R3` 交付的规则 SQL 在控制台**保存失败**，报 **`11: syntax error before: '='`**（用户实测，`2026-10-12`）。
> 由解析器 trace 定位到 **`payload.c == 'undefined'`** —— 词法阶段被拆成**两个** `=` token（`{'COMPARISON',11,'='},{'COMPARISON',11,'='}`）。

| # | `R3` 的写法 | 事实（EMQX 官方） | `R3.1` 修正 |
|---|---|---|---|
| 1 | `payload.c == 'undefined'` | 规则 SQL 的"完全相等"**只有单个 `=`**（Rule SQL Reference: `=` = "Check if the two operands are completely equal"）；**`==` 不存在**。`!=` 合法 | 改用 **`is_null(payload.c)`**（等价语义） |
| 2 | `payload.x != 'undefined'`（"键存在"判定） | 官方内置函数才是一等公民：**`is_null(Term)` = 判 `undefined`（未赋值）**；**`is_not_null(Term)` = 反之**；**路径中间字段缺失亦返回 `true`**（`is_null(payload.t.field)` on `{"msg":"hello"}` ⇒ `true`，EMQX 维护者原话） | 全部改用 **`is_not_null(...)` / `is_null(...)`** |

**`R3.1` 定稿谓词**（= §4.5 首选形态）：

```
      is_not_null(payload.schema_version)         ← 正条件 ×5（键存在）
  AND is_not_null(payload.seq)
  AND is_not_null(payload.ts)
  AND is_not_null(payload.type)
  AND is_not_null(payload.value)
  AND is_null(payload.c)                          ← 反条件 ×2（键不存在）
  AND is_null(payload.cmd)
```

**旧规则（`guo_feeder/#`）取反（`-S2` 目标 · 只含 `OR` / `AND` / `is_*`，无 `NOT`、无括号）**：

```
      is_null(payload.schema_version)
   OR is_null(payload.seq)
   OR is_null(payload.ts)
   OR is_null(payload.type)
   OR is_null(payload.value)
   OR is_not_null(payload.c)
   OR is_not_null(payload.cmd)
```

> ★ **语义（§4.1 冻结的逻辑谓词）逐字未变** —— 改的只是**写法**（`R3.1` 属 §4.5「非冻结契约」范围内的修正）。
> ★ **强制新增验证步**：规则保存前，必须用规则页的 **「启用调试 + 发送测试消息」** 以两条样例验证
> （`{"msg":"hello"}` ⇒ history 规则**不匹配** / message 规则**匹配**；telemetry 信封 ⇒ 反之），**符合后才保存**。
> ★ **最后备选**（仅当 `is_null` 报"未知函数"）：回到 `'undefined'` 字符串比较，但**相等必须单 `=`**。
> 登记风险 → **`R-WP4C-20`**（`P3-WP4C-Implementation-Record.md`）。

---

## §11 关联文档

| 文档 | 关系 |
|---|---|
| **`docs/review/P3-WP4C-Decisions.md`** | **★ 本设计的冻结裁决记录（权威）** |
| **`docs/review/P3-WP4C-Console-Task-Sheet.md`** | **★★★ 施工任务单（照抄版 · 唯一"照着点"的依据）** —— 旧对象原文 / 删除清单 / 新建逐字段值 / **完整 SQL** |
| **`docs/review/P3-WP4C-Console-Build-Guide-R3.md`** | **★★ `R3`/`R3.1` 施工说明** —— 建连接器 / 建动作 / 建规则逐字段 + 删除顺序 + 窗口 + 回滚 |
| `docs/review/P3-WP4C-Connector-Reuse-Amendment.md` | `R2`/`R3` 修订裁决（机制论证 §4.1 · 请求体定案 §6 · 配额核算 §9） |
| `docs/interfaces/cloud_protocol.md` **§4.5** | telemetry uplink 冻结契约（**输入**） |
| `docs/review/P3-WP4-Firmware-Telemetry-Decisions.md` | 冻结裁决（信封 / 值编码 / seq / 范围 / 路由） |
| `docs/review/P3-WP4-Firmware-Integration-Design-Review.md` §9 | `H-WP4-FW-01` 裁决留痕（方案 A 双 Action 分流） |
| `docs/architecture/P3-DT-WP4-History-Ingestion-Design.md` §9.1 | 端点定稿 = `POST /api/history/ingest`（选择 B） |
| `EMQX_Assets/config/data-integration/README.md` | 既有规则 / 连接器存档 + 部署约束（`D-1…D-5`）· **控制台配置回写目标** |
| `EMQX_Assets/config/acl-rules.v3.json` | 现行 ACL（`deviceRuleTemplate` 3 条，本轮**未改**） |

---

*本文档于 **2026-10-12** 由 **`scope proposal`** 升级为 **`FROZEN DESIGN`**（`H-WP4C-03` 冻结）；
**只做设计冻结与环境事实登记**，**不含任何配置改动或实施**。
基线 = 固件仓 HEAD **`cdcc766`** · CF 仓 **`9b25203`** · EMQX 仓 **`eec9444`**。
所有 `file:line` 与平台事实均取自基线，可逐条回查。*
*`2026-10-12` **升级为 `R2`**：新增 **`D-7`（Serverless 数据集成配额，不可调整）** 与 **§10 `R2` 修订节** ——
**连接器形态**由「新建独立连接器」改为「**复用 `c-…b12100` + 动作级 `URL Path` 分流**」；
**核心语义（方案 A / 冻结谓词 / `-S2` / Pages Proxy / 端点 `/api/history/ingest` / `V-1…V-7`）逐字不变**。
*`2026-10-12` **升级为 `R3`**（成因：官方文档确认 **「编辑连接器仅适用于专有版部署」** ⇒ `R2` 的就地改写**不可执行**）——
**施工形态**改为 **删除重建**（新连接器 `CB` + 动作 `MF`/`HI` + 规则 ×2 · **presence 链路零接触**）；
**语义仍逐字不变**，且 **§5.1 双投窗口消失**；新增风险 **`R-WP4C-18b`**（删除不可逆 ⇒ 施工前须抄录运行统计 + 截图配置）。
施工规格（唯一依据） → **`docs/review/P3-WP4C-Console-Build-Guide-R3.md`**。*
规格全文 → **`docs/review/P3-WP4C-Connector-Reuse-Amendment.md`**。
*`2026-10-12` **升级为 `R3.1`**（成因：**控制台实测 `syntax error before: '='`** —— `R3` 谓词里的 `==` 不被 EMQX SQL 支持）——
新增 **§10.4**（谓词写法修正：改用官方 **`is_null` / `is_not_null`**）· §4.5 草稿同步修正；
**语义仍逐字不变**，新增风险 **`R-WP4C-20`**，新增强制验证步（规则页**调试试跑**）。
施工规格（唯一依据） → **`docs/review/P3-WP4C-Console-Task-Sheet.md`** + `…-Console-Build-Guide-R3.md`。*
最新基线 = 固件仓 **`4c74d3a`** · CF 仓 **`415cd0c`** · EMQX 仓 **`a73968a`**；**已建对象** = 连接器 `c-n302933b-0c1cb0`。*
