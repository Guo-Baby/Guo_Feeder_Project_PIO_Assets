# P3 · `WP4C` —— **实施检查清单 + 人工操作清单 + 基线采集流程**（`CHECKLIST ONLY` · **STOP 边界保持**）

> ## 🛑 本文档 = **实施前准备（零生产变更）** —— **未创建任何 EMQX 规则 · 未部署 · 未应用 migration · 未改 firmware**
>
> | 项 | 值 |
> |---|---|
> | **本文档性质** | **实施前准备工件**：检查清单 + 人工操作清单 + 基线采集流程 + `HUMAN_REQUIRED` 登记 |
> | **冻结依据（唯一，不得重设计）** | `docs/review/P3-WP4C-EMQX-Rule-Design.md`（🔒 `FROZEN`）· `docs/review/P3-WP4C-Implementation-Plan.md` · `docs/review/P3-WP4C-Decisions.md` |
> | **基线** | 固件仓 HEAD = **`b8af006`** · CF 仓 **`9b25203`** · EMQX 仓 **`eec9444`**（三方工作树 clean） |
> | **本文档不做的** | ❌ 不改 EMQX（**未调用任何写 API**）· ❌ 不改 Worker · ❌ 不部署 · ❌ 不执行 migration · ❌ 不改 firmware / ACL / topic · ❌ **不关闭任何 `HUMAN_REQUIRED`** · ❌ 不 push |
>
> **★ 定位**：把冻结设计**转写为可执行的人工作业工件**。**实施本身仍 `STOPPED`**（见 §6 判定）。

---

## §0 边界与依据

| # | 约束 | 来源 |
|---|---|---|
| C-1 | **EMQX Rule 是唯一分流责任方** | `H-WP4-FW-01`（`CLOSED`） |
| C-2 | Worker **不解析** telemetry、不做 command/telemetry 判断 | `H-WP4-FW-01` · `DT:WP-4` §5.1.1 |
| C-3 | **不得修改 firmware topic** | 冻结设计 `C-3` |
| C-4 | **不得修改 `/up`**（frozen topic） | `DT:WP-4` 冻结 |
| C-5 | **不得依赖 MQTT v5 metadata**（真机 `proto_ver = 4`） | 冻结设计 `C-6` / `D-5` |
| C-6 | **不得新增 telemetry topic** | 冻结设计 `C-7` |
| C-7 | **不得在 EMQX 侧设阈值 / 降频**（policy 唯一权威 = 设备侧） | `DT:WP-1` |
| C-8 | 变更**不得**触碰 ACL（P2-5 的 5/5 规则保持不动） | `PhaseC` §6 |

---

## §1 实施检查清单（**Pre-flight · 逐项 gate**）

> **判据**：任一 🔴 未过 ⇒ **不得进入实施**。当前状态列 = `2026-10-12` 实测。

### 1.1 授权门（**人工）**

| # | 检查项 | 判据 | 当前 |
|---|---|---|---|
| `G-1` | `H-WP4C-04` 远端 `0003` 执行授权 | 书面授权 + 变更窗口 | 🔴 **未授权** |
| `G-2` | `H-WP4C-05` EMQX Action 切换授权 | 书面授权 + 回退触发条件 | 🔴 **未授权** |
| `G-3` | `H-WP4C-06` 部署顺序确认 | 明确「`0003` → Worker → Action」且不同批抢跑 | 🔴 **未确认** |
| `G-4` | `H-WP4C-07` 控制台执行人 + 配置导出责任 | 具名责任人 + 导出存档承诺 | 🔴 **未指派** |
| `G-5` | `H-WP4C-08` Worker 部署授权 | 书面授权（含与 `0003` 的批次关系） | 🔴 **未授权** |

### 1.2 环境门（**实测**）

| # | 检查项 | 判据 | 当前 | 证据 |
|---|---|---|---|---|
| `E-1` | EMQX 部署可访问 | 控制台可登录、部署在线 | 🟡 **待人工核对**（API 凭据有效但无数据集成域权限） | §3 `B-1` |
| `E-2` | 目标端点已部署 | `POST /api/history/ingest` 线上存在（无 key ⇒ **`401`**） | 🔴 **`FAIL`** —— 线上返 catch-all **`200 OK`** | §3 `B-4` |
| `E-3` | 远端表已就绪 | `telemetry_history` / `log_history` 存在于远端 D1 | 🔴 **`FAIL`** —— `0003` **待应用** | §3 `B-5` |
| `E-4` | Pages 中转就绪 | EMQX 可达 `*.pages.dev`；中转可达 Worker | 🔴 **未就绪**（未建立） | 冻结设计 `D-2` |

### 1.3 顺序门（**冻结铁律**）

| # | 检查项 | 判据 | 当前 |
|---|---|---|---|
| `S-1` | `0003` **先于** Action 切换 | 依赖 2（否则全量 `503` + 上游重投风暴） | ⏸ 未开始 |
| `S-2` | migration **先于/同批于** Worker | `migrations/README.md` 铁律 | ⏸ 未开始 |
| `S-3` | 新增规则与收窄既有规则**同一批** | 否则**双投**（见 §2.2） | ⏸ 未开始 |

### 1.4 配置门（**console-ready 规格见 `Implementation-Plan.md` §8**）

| # | 检查项 | 判据 | 当前 |
|---|---|---|---|
| `C-1` | 谓词语法在本版本可用 | `V-1` 控制台可保存 + 可启用 | ⏸ `NOT RUN` |
| `C-2` | 新增规则 FROM = `guo_feeder/+/up`（**不用 `#`**） | 收敛匹配面 | ⏸ 待配置 |
| `C-3` | 谓词 = `(schema_version ∧ seq ∧ ts ∧ type ∧ value) ∧ ¬c ∧ ¬cmd` | **禁只判 `type`/`value`/`topic`** | ⏸ 待配置 |
| `C-4` | `value` 用「**键存在**」语义 | 禁"值非空 / ≠ 0"（否则 `0`/`false` 漏判） | ⏸ 待配置 |
| `C-5` | 既有规则 WHERE 改为 **`NOT` 谓词** | **强制第 2 步** | ⏸ 待配置 |
| `C-6` | Action URL **不直指 `*.workers.dev`** | 须经 Pages 中转（`D-2`） | ⏸ 待配置 |
| `C-7` | Action 带 `X-Webhook-Key`（与 Worker Secret 一致） | 否则全 `401` | ⏸ 待配置 |

### 1.5 存档门（**强制 · `R-WP4C-6`**）

| # | 检查项 | 判据 | 当前 |
|---|---|---|---|
| `A-1` | 变更**前**基线已导出并入库（`V-0`） | 规则 SQL + Action + Connector 三份快照 | 🔴 **未导出**（§3） |
| `A-2` | 变更**后**配置已回写 `EMQX_Assets/config/data-integration/` | 含新 SQL / Action 全字段 | ⏸ 未开始 |
| `A-3` | 验证记录 `V-1…V-7` 已留痕 | 每项 `PASS/FAIL` | ⏸ `NOT RUN` |

---

## §2 人工操作清单（**控制台 step-by-step · 未执行**）

> **前提**：§1 全部 🟢。**执行人** = `H-WP4C-07` 指派的责任人。**工具** = EMQX Cloud 控制台（API 不可用 —— `403`）。

### 2.1 变更前（只读 · 必须先做）

| 步 | 操作 | 产出 | 完成判据 |
|---|---|---|---|
| `P-1` | 记录变更窗口 / 回退触发条件 / 执行人 | 变更单 | 已签署 |
| `P-2` | **导出**现规则 SQL 原文 | `B-1` 快照 | 全文入库 |
| `P-3` | **导出**现 Action 全字段（URL / 方法 / 请求头 / Body 模板） | `B-2` 快照 | 全文入库 |
| `P-4` | **导出**现 Connector 配置 | `B-3` 快照 | 全文入库 |
| `P-5` | **确认**线上 Worker 版本 | `B-4` 记录 | 版本 ID 入库 |
| `P-6` | **确认**远端 D1 migration 状态 | `B-5` 记录 | 待应用清单入库 |
| `P-7` | 记录 D1 行数基线（`telemetry_history` / `mqtt_messages`） | 基线计数 | 已记录 |

### 2.2 变更（**两步强制 · 必须同批**）

| 步 | 操作 | 关键字段 | 完成判据 |
|---|---|---|---|
| `P-8` | **新建**规则 `guo_feeder_history_ingest` | FROM `guo_feeder/+/up` · WHERE = 冻结谓词 · Action → Pages 中转 `/api/history/ingest` + `X-Webhook-Key` | 规则**已启用**（`V-1`） |
| `P-9` | **收窄**既有消息规则 WHERE | 由原条件改为 **`NOT`（冻结谓词）** | 规则**已启用** |
| `P-10` | **不得**第 3 条规则、**不得**双 Action、**不得**保留双投 | — | 无双投（`V-2`「`mqtt_messages` +0」） |

> **★ 为何 `P-8` / `P-9` 必须同批**：既有规则 FROM = `guo_feeder/#` **本来就匹配 `/up`** ⇒ 只做 `P-8` ⇒ telemetry **双投**（新规则进 `telemetry_history` + 既有规则进 `mqtt_messages`）⇒ 重复存储 + 动作次数翻倍 + 统计污染（冻结设计 §5 `-S2`）。

### 2.3 变更后（验证 + 回写）

| 步 | 操作 | 判据 |
|---|---|---|
| `P-11` | 用**测试设备**发 1 条 telemetry（§`V-2` 载荷） | `telemetry_history` **+1**；`mqtt_messages` **+0** |
| `P-12` | 发 1 条 command | `telemetry_history` **+0**；`mqtt_messages` **+1** |
| `P-13` | 发 1 条 mixed（含 `type`+`value` **但含 `cmd`**） | **判为非 telemetry**（`V-4`） |
| `P-14` | 重复投递同 `(device_id,type,ts,seq)` | 落库**恰 1 行**（`V-7` · `ux_th_dedup`） |
| `P-15` | 观测窗口：`wrangler tail` + D1 行数 | 无重投风暴 / 无 `503` |
| `P-16` | **回写**配置到 `EMQX_Assets/config/data-integration/` | `A-2` 完成 |
| `P-17` | 记录 `V-1…V-7` 结果 | `A-3` 完成 |
| `P-18` | 决定是否回滚（见 §3.4） | 已决策 |

---

## §3 实施前基线采集流程（`B-1…B-5`）

> **★ 设计原则**：**凡可 API/CLI 读的，用只读命令自动采集；凡 API `403` 的，用控制台人工导出**（冻结设计 `D-1`）。

### `B-1` EMQX 当前 Rule SQL 导出

| 项 | 值 |
|---|---|
| **目的** | 冻结「变更前的字节」，作为**回滚基线**与审计锚点（`V-0`） |
| **自动化** | 🔴 **不可** —— `GET /rule_engine/rules`、`/rules`、`/data_integration/rules` **全 `403`**（§`B-6` 实测矩阵） |
| **方法（人工）** | 控制台 → **数据集成 → 规则** → 逐条打开 → **复制 SQL 全文** |
| **记录格式** | 规则名 · FROM · WHERE（SQL 原文）· 启用状态 · 导出时间 · 导出人 |
| **入库位置** | `EMQX_Assets/config/data-integration/README.md`（沿用既有做法） |
| **当前状态** | 🔴 **未采集**（仅继承存档推断值：FROM = `guo_feeder/#`） |
| **证据级别** | `LIVE`（须人工）+ `STATIC`（入库后可离线回查） |

### `B-2` Action 配置导出

| 项 | 值 |
|---|---|
| **目的** | 冻结 Action 的 **URL / 方法 / 请求头 / Body 模板**（回滚与审计） |
| **自动化** | 🔴 **不可** —— `GET /actions`、`/bridges` **`403`** |
| **方法（人工）** | 控制台 → 数据集成 → 规则 → **动作** → 记录 **URL · 方法 · 请求头（含 `X-Webhook-Key` 是否配置）· Body 模板** |
| **⚠️ 密钥纪律** | `X-Webhook-Key` 的**值不得入库**；仅记录「**已配置 / 未配置**」与轮换方式 |
| **记录格式** | 归属规则 · URL · 方法 · Content-Type · 请求头清单（**值脱敏**）· Body 模板 · 超时/重试参数 |
| **当前状态** | 🔴 **未采集**（继承推断：URL = `https://guo-feeder-api.guobaby.workers.dev/`，**无 key**） |

### `B-3` Connector 配置导出

| 项 | 值 |
|---|---|
| **目的** | 记录既有 Connector（承载 Action 的出站通道）配置，判定复用 / 新建 |
| **自动化** | 🔴 **不可** —— `GET /connectors` **`403`** |
| **方法（人工）** | 控制台 → 数据集成 → **连接器** → 记录类型 / 目标 / 参数（**密钥脱敏**） |
| **记录格式** | 连接器名 · 类型 · 目标 URL · 认证方式（脱敏）· 引用它的规则 |
| **当前状态** | 🔴 **未采集** |

### `B-4` Worker 当前生产版本确认 —— ✅ **已采集（只读）**

| 项 | 值 |
|---|---|
| **目的** | 确认线上是否已含 `/api/history/ingest`（决定 `H-WP4C-08` 必要性） |
| **自动化** | ✅ **可** —— `npx wrangler deployments list --name guo-feeder-api`（**只读**） |
| **实测结果** | **最新（100%）= `846384cd-8b84-488b-a7a1-3ee5f9fae52a`** · 创建于 **`2026-10-07T16:54:23.549Z`** · 备注 **"N-3 secret redaction (d604f2e)"** |
| **旁证（端到端探测）** | `POST /api/history/ingest`（**无 key**）→ 线上返 **`200` body `OK`**（catch-all）；而本地 `9b25203` 该路径**必返 `401`** ⇒ **线上版本 < `9b25203`** |
| **结论** | 🔴 **CF 仓 HEAD `9b25203`（含 `/api/history/ingest`）未部署** ⇒ 部署属 `H-WP4C-08` |
| **环境** | 账号 `Wyl94@foxmail.com's Account`（`9e72d201efec6925b3a468a0e3fb4928`） |

### `B-5` D1 migration 当前状态确认 —— ✅ **已采集（只读）**

| 项 | 值 |
|---|---|
| **目的** | 确认 `0003` 是否已应用（决定 `H-WP4C-04` 必要性） |
| **自动化** | ✅ **可** —— `npx wrangler d1 migrations list guofeeder --remote --config wrangler.toml`（**只读**） |
| **实测结果** | **待应用 = `0003_p3_history_tables.sql`**（**唯一**）；`0001` / `0002` 已应用 |
| **库身份** | `name = guofeeder` · `uuid = 0d8385ba-a1a6-4fcc-8e43-ae61c014c43d` · `version = production` · `file_size = 737,280 B` |
| **`0003` 本地校验和** | `sha256 = cd103cd2b4eef95ecc6164b031cfcda06d190ead7ef3c4ffbcd6a93ef4e882c8` · 138 行 |
| **结论** | 🔴 **远端无 `telemetry_history` / `log_history`** ⇒ 端点即使部署也会 `503`（`persist_failed`） |
| **⚠️ 陷阱** | `wrangler d1 list` 的 **`num_tables` 列显示 `0` 不可信**（本库实际有表）⇒ **判据以 `migrations list` 为准** |

### `B-6` 采集侧 API 可用性矩阵（**实测 · 决定自动化边界**）

| 端点 | 实测 | 含义 |
|---|---|---|
| `GET /rule_engine/rules` | 🔴 `403` | 规则 **不可读** |
| `GET /rules` / `/data_integration/rules` | 🔴 `403` | 同上 |
| `GET /connectors` | 🔴 `403` | 连接器 **不可读** |
| `GET /actions` / `/bridges` | 🔴 `403` | 动作 **不可读** |
| `GET /authentication` / `/authorization/sources` | 🔴 `403` | 认证 / ACL **不可读** |
| `GET /clients?limit=5` | ✅ **`200`** | **凭据有效**（⇒ `403` = **权限域缺失**，非认证失败） |

> **⇒ `B-1` / `B-2` / `B-3` 只能人工控制台导出**；`B-4` / `B-5` 已用只读命令完成。

### 3.4 回滚基线（继承冻结设计 · 首选路径）

| 场景 | 回滚动作 | 代价 |
|---|---|---|
| 规则配置错误 | 用 `B-1` 基线**恢复既有 WHERE** + 停用新规则 | **最低**（无数据面影响） |
| 端点不可用 | Action **回切旧端点**（旧 `POST /api/ingest` **整段未改动**，立即可用） | 低 |
| `0003` 误应用（无数据） | `DROP INDEX`×5 → `DROP TABLE`×2 → 删 `d1_migrations` `0003` 记账行 | 中（**破坏性 · 须授权**） |
| `0003` 已产生数据 | **默认不做**（会丢历史）—— 先导出后裁决 | 高 |

---

## §4 `HUMAN_REQUIRED` 登记（`H-WP4C-04…08` · **全部保持 `OPEN`**）

| ID | 主题 | 类型 | 内容 / 判据 | 状态 |
|---|---|---|---|---|
| **`H-WP4C-04`** | **`0003` migration 执行授权** | 人工授权 | 授权 `wrangler d1 migrations apply guofeeder --remote`；是否需前置导出（`B-5` 复核）；变更窗口 | 🔴 **`OPEN`** |
| **`H-WP4C-05`** | **EMQX Action 切换授权** | 人工授权 | 授权在控制台**新增规则 + 收窄既有规则**；回退触发条件；变更窗口 | 🔴 **`OPEN`** |
| **`H-WP4C-06`** | **部署顺序确认** | 人工决策 | 明确「`0003` → Worker → Action」且**不同批抢跑**（§1.3 `S-1…S-3`） | 🔴 **`OPEN`** |
| **`H-WP4C-07`** | **EMQX Console 执行人与配置导出责任** | 人工作业 | ① 指派**具名执行人**；② 承诺变更前导出 `B-1`/`B-2`/`B-3`；③ 变更后**回写仓库存档**（`A-2`） | 🔴 **`OPEN`** |
| **`H-WP4C-08`** | **Worker 部署授权** | 人工授权 | 授权部署 `9b25203`（含 `/api/history/ingest`）；与 `0003` 的批次关系（须合 `S-2`） | 🔴 **`OPEN`** |

> **★ 状态纪律**：**全部 5 项均为人工授权 / 决策 / 作业项 —— 代理无权关闭、无权代为授权。本轮一律保持 `OPEN`。**

---

## §5 Gate 状态（**本轮未改动任何 Gate**）

| Gate | 状态 | 备注 |
|---|---|---|
| `DT:WP-4` | `FROZEN` | 未动 |
| `H-WP4C-03` | 🔒 `CLOSED` | 设计冻结（`f31aabe`）—— **设计冻结 ≠ 实施完成** |
| `CF-1` | 🟢 `RESOLVED` | log 信封产出已落地（`dae7966`） |
| `CF-2` | 🔴 `BLOCKED` | 代码在树（`a89ee3c`）但**默认关闭**（`GF_TELEMETRY_ENABLED=0` + `telemetry.enable=false`） |
| `Phase C` | 🟡 `PLANNED / BLOCKED` | 未开工 |
| `G-1 / G-4` | `HUMAN_REQUIRED` / `BLOCKED` | 未动 |
| `P3` | `NOT STARTED / PLANNED` | 未动 |

---

## §6 是否具备进入实施阶段的条件 —— **判定：🔴 否（`NOT READY`）**

| # | 必要条件 | 状态 |
|---|---|---|
| 1 | 授权门 `G-1…G-5` 全过 | 🔴 **0 / 5** |
| 2 | 环境门 `E-2`（端点已部署） | 🔴 **`FAIL`**（线上 = `846384cd`，早于 `9b25203`） |
| 3 | 环境门 `E-3`（远端表已就绪） | 🔴 **`FAIL`**（`0003` 待应用） |
| 4 | 环境门 `E-4`（Pages 中转就绪） | 🔴 **未就绪** |
| 5 | 存档门 `A-1`（变更前基线） | 🔴 **未导出**（`B-1/2/3` 须人工） |
| 6 | 固件侧数据源 | 🟡 `CF-1` 已解 / `CF-2` **默认关闭**（须 `GF_TELEMETRY_ENABLED=1` 镜像） |

> **⇒ `NOT READY` —— 不进入实施。**
> **解锁顺序（冻结铁律，`S-1…S-3`）**：
> `H-WP4C-04`（`0003` 落表）→ `H-WP4C-08`（部署 Worker）→ `H-WP4C-07`（人工导出基线 + 控制台作业）→ `H-WP4C-05`（授权切流，**与收窄同批**）→ `V-1…V-7` 验证。

---

## §7 边界核对（**零违规**）

| 禁止项 | 状态 |
|---|---|
| 修改 / 创建 EMQX 规则、连接器、动作 | ✅ **未做**（**未调用任何写 API**；读 API 全 `403`） |
| 修改 EMQX 配置 / ACL | ✅ **未改**（EMQX 仓 HEAD 仍 `eec9444`，工作树 clean） |
| 修改 / 部署 Worker | ✅ **未改 · 未部署**（CF 仓 HEAD 仍 `9b25203`，工作树 clean；仅 `deployments list` **只读**） |
| 执行 / 修改 migration | ✅ **未执行**（仅 `migrations list --remote` **只读**） |
| 修改 firmware / `src/` / topic / ACL | ✅ **未改** |
| 关闭任何 `HUMAN_REQUIRED` | ✅ **未关闭**（`H-WP4C-04…08` 全 `OPEN`） |
| 修改冻结设计文档 | ✅ **未改**（`P3-WP4C-EMQX-Rule-Design.md` 逐字未动） |
| `push` | ✅ **未 push** |
| 本轮新增文档 | ✅ **唯一** = `docs/review/P3-WP4C-Pre-Implementation-Checklist.md` |

> **只读操作声明**：本轮生产侧仅 `GET`（EMQX，含 `403`）· `wrangler whoami` / `deployments list` / `d1 list` / `d1 migrations list --remote`（**全部只读**）。**无任何写操作。**

---

## §8 关联文档

| 文档 | 关系 |
|---|---|
| `docs/review/P3-WP4C-EMQX-Rule-Design.md` | 🔒 **冻结设计（唯一实施依据）** |
| `docs/review/P3-WP4C-Implementation-Plan.md` | 实施计划 + 状态快照（含 **console-ready 规格 §8**） |
| `docs/review/P3-WP4C-Decisions.md` | 冻结裁决记录（含 `HUMAN_REQUIRED` 登记） |
| `docs/review/P3-WP4-PhaseC-Integration-Plan.md` | Phase C 计划（`C-1`/`C-2`/`C-3`） |
| `EMQX_Assets/config/data-integration/README.md` | 既有规则存档 + 平台约束 + **回写目标** |

---

*本文档于 **2026-10-12** 建立，为 **`WP4C` 实施前准备工件（检查清单 / 人工操作清单 / 基线采集流程 / `HUMAN_REQUIRED` 登记）**；
**零生产变更**，**未关闭任何裁决项**，**未修改任何冻结文档**。
基线 = 固件仓 **`b8af006`** · CF 仓 **`9b25203`** · EMQX 仓 **`eec9444`**。所有实测值均可逐条回查。*
