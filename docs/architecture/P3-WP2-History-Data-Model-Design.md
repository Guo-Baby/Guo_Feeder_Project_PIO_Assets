# P3 · WP-2 —— History Data Model Design（历史数据模型设计）

> ## 📐 `DESIGN ONLY`（**仅设计**）—— 无实现、无 migration、无部署
>
> - 本文件 = **P3 历史数据模型设计**（`TelemetryHistory` / `LogHistory` 域）；**不含代码**、**不创建任何 migration 文件**、**不写 D1**、**不改 Worker / EMQX**。
> - **`P3 = NOT STARTED / PLANNED`**（**未改变**）· **`G-3`（Schema 冻结）** 仍为 **`HUMAN_REQUIRED`** · **未解除任何 Gate**。
>
> **状态**：**`WP-2 = DESIGN`（设计已产出，未冻结）** · **`P3 = NOT STARTED / PLANNED`** · **`push = DEFERRED`**
> **上位依据**：`P3-WP1-Telemetry-Policy-Final.md`（**§7.3 已冻结**）· `P3-History-D1-Plan.md`（**R2** · **WP-2**）· `Cloud-APP-Platform-Plan.md` **§7.3 / §7.4**
> **接口依据**：`docs/interfaces/cloud_protocol.md`（**§8.2** log Topic · **§12.3** `$state` · **§12.4** `$description` · **§12.5** 属性映射）
> **D1 现状依据**：`Cloudflare_Assets/guo-feeder-api/migrations/`（`0001_*` / `0002_*` · **既有 `device` / `device_credential` / `device_binding` / `device_event`**）· `migrations/README.md`
> **设计时点**：**2026-10-10** · 固件仓 HEAD = **`e015a29`**（工作树 clean）
>
> **⚠️ 术语铁律**：`PASS` · `NOT RUN` · `HUMAN_REQUIRED` · `BLOCKED` · `DEFERRED` · `CLOSED`。**`NOT RUN` / `HUMAN_REQUIRED` / `BLOCKED` 一律 ≠ `PASS`**。
>
> **⚠️ 本文件不冻结 schema** —— 列集合的 finalize = **`G-3`**，须由**人工**依 §8 的 `H-WP2-*` 裁决作出。

---

## §1 状态声明

| 项 | 内容 |
|---|---|
| **WP-2 性质** | **`DESIGN ONLY`** —— 只产出**数据模型设计 / Schema 候选 / 字段定义 / 索引设计 / 生命周期策略 / 待裁决项** |
| **P3 状态** | **`NOT STARTED / PLANNED`**（**未改变**） |
| **本文件未做** | **No implementation** · **No migration**（**未创建 `0003_*`**）· **No D1 write** · **No Worker change** · **No EMQX change** · **No deploy** |
| **Gate 影响** | **未解除任何 Gate**；**`G-3` 仍 `HUMAN_REQUIRED`**；`G-1` / `G-4` 状态**未改** |
| **依赖状态** | `U-1`（telemetry policy）**已解决**（WP-1 冻结）⇒ **schema 设计可推进**；**列集合 finalize 仍待 `G-3`** |
| **本文件也不做的** | **不改** `src/` · **不改** `platformio.ini` · **不改**固件行为 · **不改**已冻结文档（`P2-0-…freeze.md` · `P3-WP1-Telemetry-Policy-Final.md`） |

---

## §2 数据模型总体方案（方案比较与推荐）

### 2.1 三种候选方案

| # | 方案 | 形态 |
|---|---|---|
| **A** | **方案 A —— 统一 `history_event` 单表** | 一张表承载**全部**历史（telemetry + log 混在一起），靠 `type` 区分 |
| **B** | **方案 B —— 按类型拆表** | 每类度量一张表：`weight_history` · `climate_history` · `valve_history` · `event_history` … |
| **C** | **方案 C —— 分域二表**（推荐） | **`telemetry_history`**（统一 EAV，含 `type` 维度）**＋ 独立 `log_history`** |

### 2.2 对比分析

| 维度 | **A 统一单表** | **B 按类型拆表** | **C 分域二表（推荐）** |
|---|---|---|---|
| **查询复杂度** | 中 —— 需按 `type` 过滤 / 透视；跨类型聚合稍繁 | **低**（每表直查）但**跨类型聚合难**（UNION 多表） | 中低 —— telemetry 内按 `type` 过滤（**单表**）；log 独立直查 |
| **D1 成本** | **最低**（1 表 + 1 索引族） | **高** —— N 表 × N 索引；DDL 与索引维护随类型增长 | **低**（2 表 + 每表 2–3 索引） |
| **扩展性** | **高** —— 新增类型**零 DDL** | **低** —— 新增类型**必须建表 + 新 migration** | **高**（telemetry 加 `type` 零 DDL；log 结构固定） |
| **APP 使用便利** | 中 —— 单查询可画多曲线，但需 `type` 过滤 | **中** —— 曲线清晰但"一次取多指标"要多次查询 | **高** —— telemetry 单表返回某设备多指标曲线；log 独立视图 |
| **与 `WP-1` 冻结一致** | ❌ **违反**「History 与 Log 分离」原则（log 会混入） | ⚠️ 部分（需自行保证 log 单独一表） | ✅ **一致**（History / Log **物理分离**） |
| **与 R2 `WP-2` 一致** | ❌ | ❌ | ✅ **一致**（R2 已规划 `TelemetryHistory` + `LogHistory` 两表） |
| **容量控制** | 中（单表易扫） | 中（分表各自保留窗口更灵活） | **高**（两域**各自** retention，独立清理） |
| **数据隔离** | 中 | 中 | **高**（log 长文本不污染 telemetry 表） |

### 2.3 ★ 推荐方案 = **C（分域二表）**

> **推荐理由**：
> ① **符合 `WP-1` 冻结原则**（History 与 Log **必须分离**）；
> ② **符合 R2 `WP-2` 既定规划**（`TelemetryHistory` + `LogHistory`）；
> ③ **D1 成本最低且扩展性最好**（telemetry 统一表 = 新增度量零 DDL；log 结构固定）；
> ④ **多设备隔离天然满足**（两表均以 `device_id` 为一级字段）；
> ⑤ **容量控制最优**（两域各自 retention / 删除窗口，可独立裁决）。
>
> **不推荐 A**：与 WP-1 冻结的"History/Log 分离"**直接冲突**（长文本 log 会污染 telemetry 查询与索引）。
> **不推荐 B**：每类一张表 ⇒ **DDL 与索引数量随类型增长**，新增度量**必须建 migration**，与 EAV"零 DDL 扩展"的初衷相悖；且跨类型曲线需 UNION。
>
> **物理命名（对齐既有 snake_case 约定）**：`telemetry_history` · `log_history`
> （R2 `WP-2` 的逻辑名 `TelemetryHistory` / `LogHistory` 与之**一一对应**；**命名风格属 `H-WP2-5` 裁决项**。）

```
                    ┌───────────────────────────────────────────┐
   device(telemetry)│  EMQX 过滤（执行设备侧口径，不重定义）        │
   ────────────────▶│      ↓                                     │
                    │  Worker: validation → idempotency → INSERT  │
                    └───────────────┬───────────────┬───────────┘
                                    │               │
                       telemetry 域 │               │ log 域
                                    ▼               ▼
                        ┌───────────────────┐  ┌───────────────────┐
                        │ telemetry_history │  │   log_history     │
                        │ (device_id, ts,   │  │ (device_id, ts,   │
                        │  type, value, …)  │  │  level, code, …)  │
                        └───────────────────┘  └───────────────────┘
                         保留窗口：短（高频）      保留窗口：长（诊断）
```

---

## §3 History 主表设计（`telemetry_history`）

### 3.1 字段定义

| 字段 | 类型 | 说明 |
|---|---|---|
| `id` | `INTEGER PRIMARY KEY AUTOINCREMENT` | 主键（行标识） |
| `device_id` | `TEXT NOT NULL` | **设备 ID**（12 位小写 hex，**一级隔离字段**）；对齐 `device.device_id` |
| `ts` | `INTEGER NOT NULL` | **Unix 时间（秒）** —— ⚠️ **项目时间单位 = 秒**（仅既有 `mqtt_messages.created_at` 为毫秒） |
| `type` | `TEXT NOT NULL` | **数据类型**（度量 / 类别标识）—— EAV 的"维度"（见 §4） |
| `value` | `REAL` / `TEXT` / `JSON` | **数据** —— 数值用 `REAL`，枚举/字符串用 `TEXT`，结构化用 `TEXT(JSON)`（见 §3.3 类型策略） |
| `source` | `TEXT` | **来源**（如 `homie` / `derived` / `system`）—— 便于溯源与排障 |
| `created_at` | `INTEGER NOT NULL DEFAULT (unixepoch())` | **写入时间**（ingest 时刻）—— 与 `ts`（观测时刻）分离 |

> **对齐既有约定**：`created_at INTEGER NOT NULL DEFAULT (unixepoch())` **沿用 `device_event` 的写法**；`device_id` 语义**对齐 `device` 表**（P0 冻结的 12 位小写 hex）。

### 3.2 附加字段讨论（**是否需要 `boot_seq` / `seq` / `schema_version` / `trace_id`**）

| 字段 | 作用 | 是否建议纳入 | 理由 |
|---|---|---|---|
| **`boot_seq`** | 设备 **Boot 序号**（跨重启关联） | ⚠️ **可选（建议纳入，弱依赖）** | 与 Log 域共用；用于跨重启排序 / 去重。**telemetry 的 `ts` 已足够**，但**与 `log_history` 对齐**时有用 ⇒ **列 `H-WP2-6`** |
| **`seq`** | 设备侧**单调序号**（去重 / 丢包检测） | ⚠️ **可选（建议纳入）** | **幂等关键**：`(device_id, type, ts)` 在同一秒内可能重复（事件成对、多指标同秒）⇒ **`seq` 提供稳定去重键**。**列 `H-WP2-7`** |
| **`schema_version`** | EAV `value` 的**编码版本** | ⚠️ **可选（建议纳入，整数）** | 未来 `value` 编码方式变更（如 JSON 结构演进）时的**向后兼容锚点**；成本极低（1 字节级）。**列 `H-WP2-4`** |
| **`trace_id`** | 跨域**关联**（telemetry ↔ log ↔ 事件） | ❌ **不建议（P3）** | EAV 历史表**不需要**端到端追踪；跨域关联由 `(device_id, ts 窗口)` 足够。**留待 P4/P5** |

> **★ 结论（建议）**：**`seq` 建议纳入**（幂等核心）；`boot_seq` / `schema_version` **可选**；`trace_id` **不建议**。**最终取舍 = `H-WP2-4` / `H-WP2-6` / `H-WP2-7` 裁决。**

### 3.3 `value` 类型策略

| 情形 | `value` 形态 | 例 |
|---|---|---|
| 单值数值 | `REAL` | `weight.value = 1234.5` |
| 枚举 / 字符串 | `TEXT` | `valve.action = "open"` · `online.state = "ready"` |
| **结构化**（事件细节） | `TEXT`（**JSON 字符串**） | `event = {"code":"VALVE_ERROR","detail":…}` |

> **★ EAV 风险（沿用 R2 `WP-2` 登记）**：`type` + `value` 属 **MVP EAV** —— **优点**：扩展简单（新增度量**零 DDL**）；**风险**：**查询复杂**（按 `type` 过滤/透视）、**类型弱化**（`value` 无法强类型）。**P3 接受该方案**；未来可依 P4/P5 需求迁移为 **typed telemetry schema**（每类度量独立列/表）。**本轮不改 schema。**

---

## §4 Telemetry 类型映射（基于 WP-1 的 9 类）

> **冻结来源**：`P3-WP1-Telemetry-Policy-Final.md`。**本表不得改变 WP-1 的落库判定** —— 只把其翻译为**表的 `type` 取值与落库目标**。

| # | WP-1 类别 | **history 处理** | `type` 取值（建议） | 目标表 |
|---|---|---|---|---|
| **1** | **EVENT / ERROR** | ✅ **落库（100%）**；须**去重 / 边沿**（`issues:R-3` 跳变源） | `event` | `telemetry_history` |
| **2** | **Valve action** | ✅ **落库（100%）** | `valve.action` | `telemetry_history` |
| **3** | **Weight** | ⚠️ **条件落库** —— **`weight.enable=false` ⇒ 不产生 weight telemetry、不产生 weight error history** | `weight.value` · `weight.error` · `weight.zero` | `telemetry_history` |
| **4** | **Water** | ⚠️ **当前无独立传感器 ⇒ 不落库**（水量由 Weight 派生，**避免重复**）；未来引入则落库 | `water.level`（未来） | `telemetry_history` |
| **5** | **Climate（Temp/Humidity）** | ⚠️ **条件落库**（Mijia 可用时） | `climate.temperature` · `climate.humidity` | `telemetry_history` |
| **6** | **Online state** | ⚠️ **仅迁移点** —— **记录** `init`/`ready`/`disconnected`/`lost`；**不记**周期 online / heartbeat-timeout offline | `online.state` | `telemetry_history` |
| **7** | **Heartbeat** | ❌ **不进入 history**（**不产生 D1 row**） | — | **（无）** |
| **8** | **Log** | ✅ **落库，但入独立域** | — | **`log_history`**（**不混入 telemetry**） |
| **9** | **System health** | ❌ **normal：no history**；⚠️ **abnormal：转 EVENT/ERROR**（落类别 1） | `system.health`（仅异常） | `telemetry_history` |

### 4.1 特别处理（**冻结项，不得改写**）

| 项 | 冻结要求（来自 WP-1） | 本设计如何体现 |
|---|---|---|
| **Heartbeat** | **必须保持不进入 history** | §4 表第 7 行 = **无目标表**；§5 无 heartbeat 索引 |
| **Weight** | **`weight.enable=false` ⇒ no history** | §4 表第 3 行明写；`water/weight`·`weight-error`·`zero` 在 `$description` 中**消失** ⇒ **无行** |
| **System health** | **`H-W1-6`**：normal **no history**；abnormal **→ EVENT/ERROR** | §4 表第 9 行：不设独立 `type` 稳态流，**异常时以 `event` 落库** |
| **Log 分离** | **Log 不得混入普通 telemetry 表** | 方案 C：**独立 `log_history`** 表（§2.3 / §3 之外的独立设计） |

### 4.2 `log_history`（独立域，摘要）

| 字段 | 类型 | 说明 |
|---|---|---|
| `device_id` | `TEXT NOT NULL` | 同上（一级隔离） |
| `ts` | `INTEGER NOT NULL` | Unix 秒 |
| `level` | `TEXT` | `WARN` / `ERROR` / `CRITICAL`（**云端存储级别 = `H-W1-…` F-6 冻结项**；**与设备 Flash 级别独立**） |
| `code` | `INTEGER` / `TEXT` | EventId |
| `boot_seq` | `INTEGER` | Boot 序号（**跨重启关联 + 去重键组成**） |
| `seq` | `INTEGER` | 设备侧日志序号（**与 `BT-1` ACK 关联**） |
| `message` | `TEXT` | **简要描述**（人读；**截断存储**） |
| `payload` | `TEXT` | **结构化 JSON**（机读；**不建索引**；**须有体积上限**，见 R2 `WP-2`） |
| `created_at` | `INTEGER NOT NULL DEFAULT (unixepoch())` | 写入时间 |

> **去重键（幂等）**：`(device_id, boot_seq, seq)` —— 与 `cloud_protocol.md` §8.2 **既定去重键一致**。
> **`payload` 体积上限** = **待定**（R2 `WP-2` 要求定稿时给出）⇒ **列 `H-WP2-8`**。

---

## §5 索引设计

### 5.1 `telemetry_history`

| 索引 | 定义 | 用途 | 必要性 |
|---|---|---|---|
| **`ix_th_device_ts`** | `(device_id, ts DESC)` | **主查询模式**：「某设备某时间窗历史」 | ✅ **必须** |
| **`ix_th_device_type_ts`** | `(device_id, type, ts DESC)` | **APP 曲线**：「某设备某指标曲线」 | ✅ **强烈建议** |
| `ix_th_type_ts` | `(type, ts DESC)` | 跨设备按度量聚合（运维） | ⚠️ 可选 |
| **`ux_th_dedup`**（候选） | `(device_id, type, ts, seq)` UNIQUE | **幂等去重**（WP-4 要求） | ⚠️ **依赖 `H-WP2-7`**（是否纳入 `seq`） |

**讨论 —— 是否需要 `device_id + type + ts`？**
> **建议 = 需要。** 理由：APP 的**核心视图是"单设备单指标曲线"**；仅有 `(device_id, ts)` 时，取单指标仍要**回表过滤 `type`**（额外扫描）。`(device_id, type, ts DESC)` 让**曲线查询走覆盖索引**。**代价**：每行多一个索引 ⇒ 写放大与存储上升（D1 影响**候选，非结论**）。

### 5.2 `log_history`

| 索引 | 定义 | 用途 |
|---|---|---|
| **`ix_lh_device_ts`** | `(device_id, ts DESC)` | 「某设备某时间窗日志」 |
| **`ux_lh_dedup`** | `(device_id, boot_seq, seq)` UNIQUE | **幂等去重**（`BT-1` / `§8.2` 既定键） |
| `ix_lh_device_level_ts` | `(device_id, level, ts DESC)` | 「只看 ERROR/CRITICAL」 | 

> **索引纪律**：**D1 每个索引都计入写成本与存储** ⇒ 索引**不是越多越好**；上表已按"必须有 / 强烈建议 / 可选"分级。**最终集合 = `G-3` 裁决。**

### 5.3 与既有表的索引风格对齐

既有 `device_event` 采用 `ix_event_device_time (device_id, created_at DESC)` —— **本设计的 `ix_th_device_ts (device_id, ts DESC)` 与之同构**（**风格一致**）。

---

## §6 生命周期策略（**仅设计，禁止实现**）

### 6.1 三个动作（设计层）

| 动作 | 说明 | 本阶段 |
|---|---|---|
| **retention** | 按窗口**保留**；超窗数据**分批删除**（按 `ts` 走索引） | **仅设计** |
| **archive** | 超窗数据**导出归档**（如 R2 / 对象存储）后再删 | **仅设计** |
| **delete** | 按 `ts` **批量删除**（避免全表扫描） | **仅设计** |

### 6.2 保留窗口（**全部 `HUMAN_REQUIRED`，此处仅为候选**）

| 域 | **候选窗口**（**非结论**） | 理由倾向 |
|---|---|---|
| **telemetry** | **90 天**（候选） | 高频运行数据，**偏查询** ⇒ 可较短 |
| **event** | **180 天**（候选） | 故障 / 安全追溯 ⇒ 较长 |
| **log** | **30 天**（候选） | 调试诊断量最大 ⇒ 最短以控容量 |

> **★ 纪律**：以上**全部为候选**，**一律 `HUMAN_REQUIRED`**（对应 `H-WP2-2`；并牵连 R2 `H-4`）。**本文件不裁决任何窗口**，**也不实现任何删除**。
> **域间差异（沿用 R2 `WP-7`）**：**Telemetry 短 / Log 长**（或按上表——**具体值待裁决**）；`log_history` **可独立保留窗口**。

---

## §7 容量估算（**估算，非 PASS**）

### 7.1 单设备

| 项 | 值 | 来源 |
|---|---|---|
| 目标 | **< 1200 行/设备/天** | `WP-1` 冻结（**保留**） |

### 7.2 11 台设备

| 项 | 计算 | 结果 |
|---|---|---|
| 每日行数 | 11 × 1200 | **≤ 13,200 行/天** |
| 每月行数 | 13,200 × 30 | ≈ 396,000 行/月 |
| 每年行数 | 13,200 × 365 | ≈ 4,818,000 行/年 |

### 7.3 D1 免费额度影响

> **⚠️ 定性讨论（不作为结论）**：
> - D1 免费额度对**行数 / 存储 / 读次数 / 写次数****均有**限制（**具体阈值以 Cloudflare 官方为准，本文件不引用具体数字**）。
> - **`13,200 行/天` 是「上限估算」**，**非典型值**（WP-1 §4 已声明各类上限**不可相加**）。
> - **瓶颈更可能在「Worker 请求/天 + D1 写行/天」的组合**（`Cloud-APP-Platform-Plan.md` **§7.4**），**而非**单纯存储。
> - **心跳不落库**是把总量压回 `< 1200 行/天` 的关键（`WP-1` `H-W1-3`）。

> **★ 明确声明**：**以上全部为「估算」** —— **不是 `PASS`、不是实测**。
> **实际消耗须由 WP-6 实测确认**（真实设备流量 / 真实行数），**不得据估算下"安全"结论**。

---

## §8 待人工裁决项（**`H-WP2-1…H-WP2-11`**）

> **★ 纪律**：以下**一律 `HUMAN_REQUIRED`**；本设计**不替人工裁决**。**它们是 `G-3`（Schema 冻结）的裁决内容。**

| ID | 问题 | 本设计建议（**仅建议**） | 关联 |
|---|---|---|---|
| **`H-WP2-1`** | **history 是否统一表**（方案 A/B/C 取舍） | **推荐 C（`telemetry_history` + `log_history` 分域二表）** | `WP-1` 分离原则 · R2 `WP-2` |
| **`H-WP2-2`** | **retention 周期**（telemetry / event / log） | 候选：telemetry **90d** · event **180d** · log **30d** | R2 `H-4` · §6 |
| **`H-WP2-3`** | **是否保存原始 payload** | telemetry **不建议**存原始（只存 `value`）；**log 保留 `payload`（限体积）** | §4.2 · R2 |
| **`H-WP2-4`** | **是否需要 `schema_version`** | **建议纳入**（整数；`value` 编码演进锚点） | §3.2 |
| **`H-WP2-5`** | **表命名风格**（`telemetry_history` vs `TelemetryHistory`） | **建议 snake_case**（对齐既有 `device_event`） | §2.3 |
| **`H-WP2-6`** | **是否需要 `boot_seq`**（telemetry 域） | ⚠️ 可选（与 log 域对齐时有用） | §3.2 |
| **`H-WP2-7`** | **是否需要 `seq` + 幂等唯一键** | **建议纳入 `seq`** ⇒ `ux_th_dedup (device_id,type,ts,seq)` | §3.2 · §5.1 · WP-4 幂等 |
| **`H-WP2-8`** | **`log_history.payload` 体积上限与超限处置** | 建议给**字节上限**（截断 / 丢弃 / 仅保键） | R2 `WP-2` |
| **`H-WP2-9`** | **`mqtt_messages` 去留**（重构 / 退役 / 拆分） | 本设计**不做裁决**；`migrations/README.md` 已登记「P3 才重构」 | R2 `H-3` |
| **`H-WP2-10`** | **是否与既有 `device_event` 合并** | **不建议合并**（`device_event` = **控制面生命周期事件**；History = **设备遥测** ⇒ **职责不同**） | R2 `H-5` |
| **`H-WP2-11`** | **`ts` 取值口径**（设备观测时刻 vs ingest 时刻） | 建议 `ts` = **设备时间**（`time-valid` 时）；**否则回退 ingest 时刻** | §12.5 `time-valid` |

> **★ `H-WP2-10` 特别说明**：既有 `device_event`（P1 建，`migrations/0001_*`）记录 `register`/`bind`/`online`/`offline`/`retire` 等**控制面生命周期事件**，容量 `< 100 行/天`；与 `telemetry_history`（设备**遥测**，量级 ≤1200 行/天）**职责、量级、用途均不同** ⇒ **建议保持分离**（**不合并**），但**最终由人工裁决（R2 `H-5`）**。

---

## §9 与上游文档一致性

| 引用 | 位置 | 本设计如何对齐 |
|---|---|---|
| `P3-WP1-Telemetry-Policy-Final.md` | §2 九类 / §5 裁决 | §4 逐项映射；**不改写任何落库判定**；heartbeat 不落库 / weight.enable 依赖 / system health 仅异常 / log 分离 **逐条一致** |
| `P3-History-D1-Plan.md`（R2） | **WP-2** | 两表（`TelemetryHistory`+`LogHistory`）· 索引 `ix_th_device_ts`/`ix_lh_device_ts` · `payload` 体积策略 · EAV 说明 · **逐条覆盖** |
| 同上 | WP-4 Worker 边界 | §1/§2.3 **分域**便于 Worker「校验→幂等→写」；Worker **不做业务判断**（§10 声明） |
| 同上 | `0003_*` 纪律 | **本文件不建 migration**；`0001_*`/`0002_*` **不动** |
| `Cloud-APP-Platform-Plan.md` | §7.3 / §7.4 | 类别 ↔ §7.3 表对齐；瓶颈 = Worker 请求 + D1 写行（§7.2/§7.3） |
| `docs/interfaces/cloud_protocol.md` | §8.2 | `log_history` 去重键 `(device_id, boot_seq, seq)` **与既定一致** |
| 同上 | §12.3 / §12.5 | Online state 只取迁移点；`type` 取值对应 `water/*`·`climate/*`·`system/*`·`control/*` |
| `migrations/README.md` | 约定 | 时间单位 = **秒**（除 `mqtt_messages`）· `device_id` 对齐 `device` · 索引风格对齐 `device_event` |

> **★ 一致性结论**：本设计**未修改**任何被引用文档；**未与 `WP-1` 冻结冲突**。

---

## §10 边界声明（本轮）

- **本文件为 `DESIGN ONLY`**：**不含代码**、**不创建 migration**、**不写 D1**、**不部署**。
- **Worker 边界保持冻结**：Worker = **MQTT ingest → validation → idempotency → D1 insert**；**不负责** telemetry 判断 / 阈值判断 / 降频 / 业务分类（**Telemetry Policy 已由设备侧冻结**）。
- **未修改** `src/` · `platformio.ini` · 固件行为。
- **未修改** Cloudflare Worker · EMQX rule。
- **未修改已冻结文档**（`P2-0-…freeze.md` · `P3-WP1-Telemetry-Policy-Final.md`）。
- **未解除任何 Gate**；**`G-3` 仍 `HUMAN_REQUIRED`**。
- **未 `push`**。
- **`P3` 状态保持 `NOT STARTED / PLANNED`**。

---

> **WP-2 = DESIGN ONLY.**
> **P3 remains NOT STARTED / PLANNED.**
> **No implementation. No migration. No deploy.**

---

*本文件于 **2026-10-10** 建立，为 **WP-2 历史数据模型设计（`DESIGN ONLY`）**；不含任何实现、迁移或代码改动。*
*基线 = 固件仓 HEAD `e015a29`；D1 现状 = `guo-feeder-api/migrations/0001_*` + `0002_*`。*
