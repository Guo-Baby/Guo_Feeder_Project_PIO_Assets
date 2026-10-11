# P3 · `H-WP4C-03` —— **EMQX Rule telemetry 分流设计**（Design Proposal）

> ## 🟠 `DESIGN ONLY` · **未实施 · 未改动任何 `src/` · 未创建任何 EMQX 规则**
>
> | 项 | 值 |
> |---|---|
> | **本文档性质** | **设计提案（scope proposal）** —— 供人工 freeze；**不含实施** |
> | **基线** | 固件仓 HEAD = **`e551d1e`**（工作树 clean）· CF 仓 **`9b25203`** · EMQX 仓 **`eec9444`** |
> | **上游依据** | `H-WP4-FW-01`（**已 `CLOSED`**：方案 A 双 Action 分流）· `DT:WP-4` §9.1（端点 = `POST /api/history/ingest`）· `cloud_protocol.md` §4.5（telemetry uplink 已冻结） |
> | **本文档不做的** | ❌ 不创建 / 修改 EMQX 规则或连接器 · ❌ 不改 Worker · ❌ 不改 migration · ❌ 不改 firmware · ❌ 不改 `/up` · ❌ 不部署 |
>
> **`H-WP4C-03` 状态 = `OPEN`（不变）** —— 本文档只提交 **scope proposal**，**等待人工 freeze**。

---

## §0 依据与约束（不可协商）

| # | 约束 | 来源 |
|---|---|---|
| C-1 | **EMQX Rule 是唯一分流责任方** | `H-WP4-FW-01`（`CLOSED`） |
| C-2 | **Worker 不解析 telemetry、不做 command/telemetry 判断** | `H-WP4-FW-01` · `DT:WP-4` §5.1.1 |
| C-3 | **firmware 不得修改 topic** | `H-WP4C-03` 任务约束 · `DT:WP-4` 入口族冻结 |
| C-4 | **不得修改 `/up`**（frozen topic） | `DT:WP-4` 冻结 · P2 ACL 5/5 覆盖 |
| C-5 | 分流**不得**引入 Worker policy | `DT:WP-4` §5.1.1（"禁止"清单） |

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
| P-3 | **既有消息规则 `guo_feeder/#` 同样匹配 `/up` 与 `/log`** | ⇒ **telemetry 会被既有规则一并捕获**（见 §4.3 的关键推论） |

### 1.3 分流目标（一句话）

**同一条 `/up` 上的 telemetry 报文 → `POST /api/history/ingest`；命令结果 / 上线通知 → 保持既有链路不变。**
**且**：不得产生**双投**（同一条报文同时进两条链路）。

---

## §2 设计候选

> 三个候选**均不修改 `/up`**（C-4）。差别在"判别依据"与"改动落在哪一层"。

### 2.1 方案 A —— **payload schema 判断**（EMQX Rule SQL 谓词）

**判别依据**：报文的 **payload 字段集合**。

```
telemetry  ⟺  has(ts) ∧ has(type) ∧ has(value) ∧ has(seq) ∧ has(schema_version)
              ∧ ¬has(c) ∧ ¬has(cmd)
其他       ⟺  ¬(上式)   → 命令链路
```

**实现形态**：EMQX Rule 的 `WHERE` 子句做字段存在性判断（JSON 解码后）。**两条规则 + 非重叠谓词**：

| 规则 | FROM | WHERE | Action |
|---|---|---|---|
| `guo_feeder_history_ingest`（**新增**） | `guo_feeder/+/up` | **telemetry 谓词**（上式） | `POST …/api/history/ingest` |
| 既有消息 ingest 规则（**修改 WHERE**） | `guo_feeder/#` | **`¬` telemetry 谓词**（见 §4.3） | 既有端点（不变） |

| 维度 | 评估 |
|---|---|
| **改动范围** | EMQX 侧：**1 新增规则 + 1 既有规则 WHERE 收窄**（控制台操作）；firmware / Worker / D1 **零改动** |
| **backward compatibility** | ✅ **命令链路零改动**（谓词只做"排除"，不改变其余报文的路由）；⚠️ 既有规则的 WHERE 一旦写错会**误伤命令结果** ⇒ 须按 §6 验证 |
| **firmware impact** | ✅ **零** |
| **cloud complexity** | 中：EMQX 谓词需在**真实版本**上验证语法与语义（§6 V-1/V-2） |

### 2.2 方案 B —— **metadata / header 判断**

**判别依据**：MQTT **user property**（v5）或其它消息元数据。

| 维度 | 评估 |
|---|---|
| **可行性** | 🔴 **不可行（证据级）** —— 真机 `proto_ver = 4`（**MQTT 3.1.1**，见 EMQX 仓 §6.10 实测）⇒ **无 user property 概念**。要启用需**同时**升级 firmware MQTT 客户端到 v5 **并**确认 broker / ACL 兼容 |
| **改动范围** | firmware（`esp-mqtt` v5 user property 设置）+ 可能 broker 侧配置 |
| **backward compatibility** | ⚠️ 协议层升级影响**全部**上行/下行报文（远超本议题） |
| **firmware impact** | 🔴 **高**（违反 C-3 精神 + 违反"优先不修改 firmware"要求） |
| **cloud complexity** | 低（元数据判别本身简单） |

> ⇒ **B 在本轮被排除**：前提（MQTT v5）当前**不成立**，且改动远超"分流"议题。

### 2.3 方案 C —— **topic extension**

**判别依据**：独立 topic（如 `guo_feeder/<device_id>/up/telemetry` 或 `guo_feeder/<device_id>/telemetry`）。

| 维度 | 评估 |
|---|---|
| **改动范围** | firmware（新增 topic 渲染 + 发布路径）+ **ACL 新增条目**（现 ACL v3 `deviceRuleTemplate` 仅 3 条：sub `down` / pub `up` / pub `log`）+ `DT:WP-4` **入口族冻结修正** + EMQX Rule |
| **backward compatibility** | ✅ 旧 topic 不变；但**引入第三/第四条入口** ⇒ `DT:WP-4` 的"域→表映射"需重开 |
| **firmware impact** | 🔴 **高** —— 违反 **C-3（firmware 不得修改 topic）** ⇒ **需先解冻** |
| **cloud complexity** | 低（topic 匹配最干净、零 payload 解析） |

> ⇒ **C 域隔离最彻底，但代价最高**：它同时触发 **firmware 改动 + ACL 扩张 + 冻结文档修正** 三项。

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
| 当前可行 | ✅ **是** | 🔴 **否**（proto_ver 4） | ⚠️ 需解冻 |

---

## §3 推荐方案 = **A（payload schema 判断）**

### 3.1 推荐理由

1. **唯一同时满足三项硬要求**（任务书"优先选择"）：**不修改 firmware** ✅ · **不修改 frozen topic** ✅ · **最小迁移成本** ✅。
2. **C-1/C-2 合规**：分流**完全在 EMQX 层**发生；Worker 仍只做"收到即写"（`DT:WP-4` §5.1.1 的 structural validation 边界**不变**）。
3. **B 当前不可行**（`proto_ver = 4`），C 需解冻 —— 二者都不是"本轮可落地"的选项。
4. **可逆性最好**：谓词是纯"路由选择"，**不改变任何 payload 语义**；回滚 = 停用新规则 + 恢复既有 WHERE。

### 3.2 ⚠️ 与 `H-WP4-FW-01` "topic/domain 分离优先" 的张力（**须人工知晓**）

`H-WP4-FW-01` 的冻结原文含 **"topic / domain separation 优先于 payload inspection"**。
**但在 `/up` 共用且冻结不变的前提下，纯 topic 分离在数学上不可能** —— 判别信息**只能**来自 payload 或元数据。
因此：

| 读法 | 结论 |
|---|---|
| "优先" = **偏好**（在可选时优先 topic 分离） | ⇒ **A 合规**：topic 分离当前**不可选**（C 需解冻），A 是次优但唯一可行项 |
| "优先" = **禁令**（任何情况下不得 payload inspection） | ⇒ **A 不合规**，必须走 **C**（先解冻 `DT:WP-4` 入口族 + ACL 扩张 + firmware 改 topic） |

> ⇒ **本节是本次 scope proposal 最需要人工裁决的一点**（见 §8 `H-WP4C-03-S1`）。
> 注意：`H-WP4-FW-01` 的"Worker 不解析 payload"禁令针对 **Worker**；A 的谓词发生在 **EMQX Rule**，**不违反** C-2 的字面。

### 3.3 谓词设计（判别子）

**telemetry 谓词（要求 5 项齐备 + 2 项排除）**：

| 判据 | 理由 |
|---|---|
| `has(type)` | v1 必填 |
| `has(seq)` | v1 必填 |
| `has(ts)` | v1 必填 |
| `has(value)` | v1 必填（**含 `0` / `false` 也必须算"存在"** —— 用"键存在"而非"值非空"判断） |
| **`has(schema_version)`** | ★ **最强判别子** —— 命令结果 / 上线通知**从不**携带该字段，且业务对象内极少出现 |
| `¬has(c)` | 命令结果经缩写后必有 `c` |
| `¬has(cmd)` | 上线通知 / 错误结果含 `cmd` |

**为什么不能只用 `type`**：命令结果的 `o`（object）内可能含业务 `type` 字段（如 workflow / action 对象）
⇒ **单字段判别会产生假阳性** ⇒ 必须以 **`schema_version` + `seq` + `ts` + `type` + `value` 的合取**为准。

### 3.4 反例守卫（**默认落入命令链路**）

**未匹配 telemetry 谓词的一切报文 → 命令链路**（保持既有行为）。
⇒ 未来的新上行类型**默认不会被误判为 telemetry**（fail-safe 方向正确：宁可命令链路多收，不可污染历史表）。

---

## §4 部署约束（**实测事实，直接影响落地**）

| # | 约束 | 来源 | 影响 |
|---|---|---|---|
| D-1 | **EMQX Cloud Serverless 的规则 / 连接器 / 动作只能控制台配置**（API `GET /rule_engine/*`、`/connectors`、`/actions` **全 403**） | EMQX 仓 `config/data-integration/README.md` §1 | ⇒ 本设计**无法脚本化落地**，须**控制台人工操作 + 仓库存档** |
| D-2 | **EMQX 打不通 `*.workers.dev`**（连接被拒），但可达 `*.pages.dev` | EMQX 仓 §6.0.1 | ⇒ telemetry 的 HTTP Action **不应直指 Worker 域名**，须经 **Pages 中转**（或在验证后确认可达性） |
| D-3 | 数据集成**按动作执行次数计费**；`guo_feeder/#` 规则当前覆盖 `…/up` | EMQX 仓 §6.4 | ⇒ 谓词必须**精确**，避免把命令结果引流到历史端点造成无谓计费 |
| D-4 | EMQX 把**非 2xx 视为失败并重试** | EMQX 仓 §6.4 | ⇒ 分流错误会**放大**为持续重试（与 `DT:WP-4` §7.4 的失败模型相互印证） |
| D-5 | 真机 **`proto_ver = 4`**（MQTT 3.1.1） | EMQX 仓 §6.10 | ⇒ **方案 B 不可用**（§2.2） |

### 4.3 ★ 关键推论：**必须同时收窄既有规则，否则会产生双投**

既有消息规则 FROM = **`guo_feeder/#`** ⇒ 它**已经**匹配 `/up` 上的 telemetry 与 `/log`。
若只"新增一条 history 规则"而**不动既有规则**，则 telemetry 报文会**同时**：
- 投递到 `POST /api/history/ingest`（新规则）✅
- 投递到既有 ingest 端点（既有规则）❌ ⇒ **进入 `mqtt_messages` 记账**

⇒ **A 的完整落地 = ① 新增 history 规则 ② 把既有规则的 `WHERE` 收窄为 `¬telemetry 谓词`**。
**②是不可省略的一步**，且是**本次改动的主要风险点**（写错会误伤命令结果）。

> ⚠️ **附带观察（不属本议题，仅登记）**：`/log` 自 `dae7966` 起已改为 **JSON 信封**（不再是二进制 CBOR）
> ⇒ 既有 `guo_feeder/#` 规则**现在也会匹配 `/log`**，将其写入 `mqtt_messages`。
> 该行为是否可接受**不在 `H-WP4C-03` 范围**（属 log 链路议题），但**应在 freeze 时一并知晓**。

---

## §5 验证计划（**全部 `NOT RUN`**）

> 未授权创建规则 ⇒ **本轮零执行**。以下为 freeze 后须执行的判据。

| # | 用例 | 期望 | 证据分级 |
|---|---|---|---|
| **V-1** | **谓词真值表**（离线） | 对 (a) 命令结果 / (b) 上线通知 / (c) telemetry v1 / (d) registry 结果 四类真实样本，谓词取值**全部正确** | `STATIC`（可用仓库内样本离线断言） |
| **V-2** | **EMQX SQL 语法与语义**（控制台调试） | 谓词在**部署的 EMQX 版本**上可解析；`has()` / JSON 解码行为符合预期 | `LIVE`（须人工控制台） |
| **V-3** | **既有规则不被误伤** | 命令结果 / 上线通知**仍**进入既有端点（`mqtt_messages` 持续增长、无 4xx） | `LIVE` |
| **V-4** | **telemetry 正确落库** | 一条 telemetry 报文 ⇒ `telemetry_history` **恰 1 行**；`mqtt_messages` **无新增** | `LIVE` |
| **V-5** | **无双投** | 同一 `(device_id,type,ts,seq)` 在两条链路中**只出现一次**（历史表） | `LIVE` |
| **V-6** | **假阳性守卫** | 携带业务 `type` 字段的命令结果**不得**被判为 telemetry | `LIVE` |
| **V-7** | **Pages 中转可达性** | Action 能到达中转 → 中转能到 Worker（返回 `401`/`200` 而非 502/超时） | `LIVE` |

> **执行前置**：`D-1`（控制台）+ `D-2`（中转就绪）+ 端点已部署（`H-WP4C-04`/`05`）。

---

## §6 风险登记

| ID | 级别 | 风险 | 缓解 |
|---|---|---|---|
| `R-WP4C-1` | 🔴 高 | **既有规则 WHERE 写错 ⇒ 命令结果被误投历史端点**（逐条 4xx + 重试 + 命令可观测性断链） | V-3 必测；先在**测试设备**上验证；保留即时回滚（恢复 WHERE） |
| `R-WP4C-2` | 🟠 中 | **谓词假阴性 ⇒ telemetry 静默丢失**（落入命令链路） | V-1/V-4；谓词取"合取 + 键存在"语义；上线后核对 `telemetry_history` 行数 |
| `R-WP4C-3` | 🟠 中 | EMQX 版本差异 ⇒ SQL 函数不可用 | V-2 先在控制台验证语法再保存 |
| `R-WP4C-4` | 🟠 中 | **双投**（遗漏 §4.3 的既有规则收窄） | 把"收窄既有 WHERE"列为**强制步骤**并纳入 V-5 |
| `R-WP4C-5` | 🟡 低 | 计费放大（谓词不准导致无谓动作执行） | V-1 真值表 + 上线后观测动作计数 |
| `R-WP4C-6` | 🟡 低 | 控制台配置**无 API 自动化** ⇒ 漂移不可审计 | 按 EMQX 仓既有做法：**控制台配置必须回写仓库存档** |

---

## §7 `H-WP4C-03` —— **Scope Proposal**（待人工 freeze）

| 项 | 内容 |
|---|---|
| **ID** | `H-WP4C-03` |
| **状态** | 🔴 **`OPEN`** —— **scope proposal 完成，等待人工 freeze** |
| **推荐方案** | **A（payload schema 判断）**，判别子见 §3.3 |
| **落地范围** | ① 新增规则 `guo_feeder_history_ingest`（FROM `guo_feeder/+/up`，WHERE telemetry 谓词，Action → `POST …/api/history/ingest`）② **收窄既有消息规则的 WHERE** 为 `¬telemetry 谓词` |
| **明确不做** | 不新增 topic · 不改 ACL · 不改 firmware · 不改 Worker · 不改 `/up` · 不改 `DT:WP-4` 契约 |
| **待裁点 `H-WP4C-03-S1`** | §3.2 的 **"分离优先"读法** —— 若裁定为"禁令"，则本提案作废，**改走 C**（需先解冻 `DT:WP-4` 入口族 + ACL 扩张 + firmware 改 topic） |
| **待裁点 `H-WP4C-03-S2`** | 是否接受**先收窄既有规则**（§4.3 的强制第 ② 步）—— 这是对**既有生产链路**的改动，须显式授权 |
| **待裁点 `H-WP4C-03-S3`** | Pages 中转（`D-2`）的归属：新建中转函数（`…/api/history/ingest`）属**本工作包**还是**独立工作包** |
| **与 Gate 的关系** | **不解除任何 Gate**；`CF-2` 仍受 `H-WP4C-04/05/06` 阻塞 |

---

## §8 边界核对（**零违规**）

| 禁止项 | 状态 |
|---|---|
| 创建 EMQX rule | ✅ **未创建** |
| 修改 EMQX 配置 / 连接器 / ACL | ✅ **未改** |
| 修改 Worker | ✅ **未改** |
| 修改 D1 migration | ✅ **未改** |
| 修改 `src/` / `/up` | ✅ **未改** |
| 新增文档 | ✅ **唯一** = `docs/review/P3-WP4C-EMQX-Rule-Design.md` |

---

## §9 关联文档

| 文档 | 关系 |
|---|---|
| `docs/interfaces/cloud_protocol.md` **§4.5** | telemetry uplink 冻结契约（**输入**） |
| `docs/review/P3-WP4-Firmware-Telemetry-Decisions.md` | 冻结裁决（信封 / 值编码 / seq / 范围 / 路由） |
| `docs/review/P3-WP4-Firmware-Integration-Design-Review.md` §9 | `H-WP4-FW-01` 裁决留痕（方案 A 双 Action 分流） |
| `docs/architecture/P3-DT-WP4-History-Ingestion-Design.md` §9.1 | 端点定稿 = `POST /api/history/ingest`（选择 B） |
| `EMQX_Assets/config/data-integration/README.md` | 既有规则 / 连接器存档 + 部署约束（`D-1…D-5`） |
| `EMQX_Assets/config/acl-rules.v3.json` | 现行 ACL（`deviceRuleTemplate` 3 条） |

---

*本文档于 **2026-10-12** 建立，为 **`H-WP4C-03` 的 EMQX Rule telemetry 分流设计提案**；
**只做设计、影响评估与 scope proposal**，**不含任何配置改动或实施**。
基线 = 固件仓 HEAD **`e551d1e`** · CF 仓 **`9b25203`** · EMQX 仓 **`eec9444`**。
所有 `file:line` 与平台事实均取自基线，可逐条回查。*
