# P3 · WP-2 —— History Data Model Review（自检审核报告）

> **性质**：**REVIEW / SELF-CHECK ONLY**。本文件**不含代码、不修改任何既有文档**，只做**对 WP-2 设计的自检与合规核对**。
> **状态**：**`P3 = NOT STARTED / PLANNED`** · **`G-3` 仍 `HUMAN_REQUIRED`** · **未解除任何 Gate** · **`push = DEFERRED`**
> **被检对象**：`docs/architecture/P3-WP2-History-Data-Model-Design.md`（WP-2 设计 · `DESIGN ONLY` · **R2 / G-3 候选**）
> **自检依据**：`P3-WP1-Telemetry-Policy-Final.md`（**冻结**）· `P3-History-D1-Plan.md`（R2）· `migrations/README.md`
> **自检时点**：**2026-10-10** · 固件仓 HEAD = **`24ffc2a`**（工作树 clean）
> **★ 修订**：**R2（2026-10-10）** —— 被检对象已修订为 **G-3 候选**（`value` 物理 `TEXT` · telemetry 增 `seq`/`schema_version`、删 `boot_seq`/`trace_id` · 幂等键明确 · 索引调整 · `H-WP2` 重编号）。**自检四项结论不变**；**`H-WP2` 清单同步更新（§6）**。
>
> **⚠️ 术语铁律**：`PASS` · `NOT RUN` · `HUMAN_REQUIRED` · `BLOCKED` · `DEFERRED` · `CLOSED`。**一律 ≠ `PASS`**。

---

## 0. 自检结论摘要

| 检查项 | 结果 |
|---|---|
| **是否违反 `WP-1`（Telemetry Policy 冻结）** | ✅ **否** —— 九类落库判定**逐项一致**（详见 §2） |
| **是否引入 Worker policy** | ✅ **否** —— 设计中 Worker **仅** ingest → validation → idempotency → insert（详见 §3） |
| **是否修改实现** | ✅ **否** —— 未动 `src/` / Worker / EMQX / migration / D1（详见 §4） |
| **是否解除任何 Gate** | ✅ **否** —— `G-1` / `G-2` / `G-3` / `G-4` / `G-5` 状态**全部未变**（详见 §5） |
| **是否与 `WP-1` 冲突** | ✅ **无冲突**（§2 逐条） |
| **是否新增 `HUMAN_REQUIRED`** | ⚠️ **是（预期内）** —— **待裁决 `H-WP2-1`/`3`/`5`/`8`/`9`/`10`/`11`/`12`/`13`**（§6.1），**属设计产出的待裁决项，非违规**；`H-WP2-2`/`4`/`6`/`7` **已由 R2 裁决**（§6.2） |
| **是否 `push`** | ✅ **否** |
| **`P3` 状态** | **`NOT STARTED / PLANNED`**（**未变**） |

> **★ 一句话**：**WP-2 设计为纯设计产物，未越界、未违反 `WP-1`、未动实现、未解除 Gate；其 `H-WP2-*` 待裁决项属 `G-3` 的正常输入。**

---

## 1. 自检范围与方法

| 维度 | 方法 |
|---|---|
| **合规性** | 把 WP-2 设计的每条落库判定与 `WP-1` 冻结文档**逐条 diff** |
| **边界性** | 检查设计中是否出现"Worker 判 telemetry / 阈值 / 降频 / 业务分类"的表述 |
| **实现性** | 检查是否出现代码 / migration / DDL 落地 / 部署动作 |
| **门禁性** | 检查是否声明任何 Gate 状态变更 |

**自检未做（明确声明）**：未运行任何代码；未连接 D1；未部署；未创建 migration。

---

## 2. 是否违反 WP-1（**逐项核对**）

| WP-1 冻结项 | WP-1 要求 | WP-2 设计 | 结论 |
|---|---|---|---|
| **唯一权威位置** | 采样决策 = 设备侧；EMQX 过滤；Worker 只存储 | §2.3/§10 明确 Worker「校验→幂等→写」；**未重新定义阈值** | ✅ **一致** |
| **Heartbeat** | **不进入 history**（`H-W1-3`） | §4 表第 7 行 = **无目标表**；§5 无 heartbeat 索引 | ✅ **一致** |
| **Weight** | `weight.enable=false` ⇒ **不产生 weight telemetry / error history**（`F-4`） | §4 表第 3 行**明写**该依赖 | ✅ **一致** |
| **Online state** | 仅迁移点 `init`/`ready`/`disconnected`/`lost`；**不记**周期 online / heartbeat-timeout offline（`H-W1-4`） | §4 表第 6 行**逐条一致** | ✅ **一致** |
| **System health** | normal **no history**；abnormal **→ EVENT/ERROR**（`H-W1-6`） | §4 表第 9 行 + §4.1 明写 | ✅ **一致** |
| **Log 分离** | Log **不得混入**普通 telemetry 表 | §2.3 方案 C：**独立 `log_history`** | ✅ **一致** |
| **设备 Flash Log 级别 ≠ 云端 LogHistory 存储级别**（`F-6`） | 两者**独立策略项** | §4.2 明写「与设备 Flash 级别**独立**」 | ✅ **一致** |
| **Water** | 当前无传感器 ⇒ 由 Weight 派生 / **避免重复** | §4 表第 4 行明写 | ✅ **一致** |
| **容量目标** | `< 1200 行/设备/天`（保留） | §7.1 保留；§7 明确"估算非 PASS" | ✅ **一致** |
| **引用规范** | 命名空间限定，禁裸 `R-n`（`F-1`） | §4 表第 1 行用 **`issues:R-3`**（限定） | ✅ **一致** |

> **⇒ §2 结论**：**WP-2 设计未违反 `WP-1` 的任何冻结项。** **`PASS`（自检项）。**

---

## 3. 是否引入 Worker policy（**边界核对**）

> **`WP-2` 设计原则 1**：Worker = **MQTT ingest → validation → idempotency → D1 insert**；**不负责** telemetry 判断 / 阈值判断 / 降频 / 业务分类。

| Worker 禁止事项 | WP-2 设计中是否出现 | 证据 |
|---|---|---|
| 判断 telemetry **是否应该保存** | ❌ **未出现** | §10 明写"**Telemetry Policy 已由设备侧冻结**"；§2.3 图标 Worker 只做 insert |
| **阈值判断** | ❌ **未出现** | §3/§4 **无任何阈值逻辑**；阈值 `X` 属设备侧（`WP-1` `H-W1-1`） |
| **降频** | ❌ **未出现** | §5/§6 只谈索引与 retention（**存储层**），**非**采样降频 |
| **业务分类** | ❌ **未出现** | §4 的 `type` 映射是**设备侧已定口径的翻译**，非 Worker 分类逻辑 |
| **幂等 / 去重** | ✅ **出现（允许）** | §4.2 `ux_lh_dedup` · §5.1 `ux_th_dedup` —— **幂等属 Worker 的 4 项职责之一**（`WP-4`） |

> **⇒ §3 结论**：**设计中 Worker 未承担任何 telemetry policy 职责**；仅保留**允许的** ingest / validation / idempotency / insert。**`PASS`（自检项）。**

---

## 4. 是否修改实现（**副作用核对**）

| 项 | 是否触碰 | 证据 |
|---|---|---|
| **固件 `src/`（cpp/h）** | ❌ **未触碰** | 本轮仅新增 `docs/` 文档 |
| **`platformio.ini` / `partitions.csv`** | ❌ **未触碰** | 同上 |
| **固件行为** | ❌ **未触碰** | 无代码改动 |
| **D1 migration** | ❌ **未创建** | §1 明写 **No migration**；`0003_*` 属实施阶段 |
| **Cloudflare Worker** | ❌ **未修改** | §10 明写 |
| **EMQX rule** | ❌ **未修改** | §10 明写 |
| **D1 数据** | ❌ **未写入** | 未连接 D1 |
| **部署** | ❌ **未部署** | 无任何部署动作 |
| **已冻结文档** | ❌ **未修改** | `P2-0-…freeze.md` / `P3-WP1-Telemetry-Policy-Final.md` **未动** |
| **既有 migration（`0001_*` / `0002_*`）** | ❌ **未修改** | 仅**只读引用**其表结构（`device_event` 等） |

> **⇒ §4 结论**：**零代码 / 零 DB / 零云端副作用。** **`PASS`（自检项）。**

---

## 5. 是否解除任何 Gate（**门禁核对**）

| 门禁 | WP-2 后的状态 | 变化 |
|---|---|---|
| **`G-1`** P3 Plan approved | **`HUMAN_REQUIRED`** | **未变** |
| **`G-2`** Telemetry Policy 冻结 | **`PASS`**（**已于 WP-1 解除**，`e015a29`） | **本轮未再变更** |
| **`G-3`** Schema 冻结 | **`HUMAN_REQUIRED`** | **未变** —— WP-2 **只是设计输入**；**列集合仍待裁决** |
| **`G-4`** `BT-1` `CLOSED` | **`BLOCKED`** | **未变** |
| **`G-5`** 范围未扩大 | **`PASS`** | **未变** |

> **⇒ §5 结论**：**WP-2 未解除任何 Gate**；**`G-3` 仍 `HUMAN_REQUIRED`**。（`G-2` 的 `PASS` 是 WP-1 的既有结果，非本轮产生。）

---

## 6. `HUMAN_REQUIRED` 清单（**`H-WP2-*` · R2 重编号**）

### 6.1 待裁决（OPEN）

| ID | 问题 | 本设计建议（**仅建议**） | 关联 |
|---|---|---|---|
| **`H-WP2-1`** | 双表架构（方案 C）是否批准 | **推荐 C**（`telemetry_history` + `log_history` 分域二表） | `WP-1` 分离原则 · R2 `WP-2` |
| **`H-WP2-3`** | 是否保存原始 payload | telemetry 不存原始；log 保留 `payload`（限体积） | R2 `WP-2` |
| **`H-WP2-5`** | **表命名最终确认** | 建议 snake_case（对齐 `device_event`） | §2.3 |
| **`H-WP2-8`** | **`payload` 最大长度** / 超限处置 | 建议给字节上限 | R2 `WP-2` |
| **`H-WP2-9`** | **telemetry retention 周期** | 候选 **90 天** | R2 `H-4` |
| **`H-WP2-10`** | **log retention 周期** | 候选 **30 天** | R2 `H-4` |
| **`H-WP2-11`** | `mqtt_messages` 去留 | 本设计不裁决 | R2 `H-3` |
| **`H-WP2-12`** | 是否与 `device_event` 合并 | **不建议合并**（职责不同） | R2 `H-5` |
| **`H-WP2-13`** | `ts` 取值口径（设备 vs ingest） | 建议设备时间（`time-valid` 时），否则回退 ingest | `§12.5` `time-valid` |

### 6.2 已由 R2 修订裁决（**不再为 `HUMAN_REQUIRED`**）

| 原 ID | 项 | **R2 裁决** |
|---|---|---|
| `H-WP2-2` | retention 周期（合并项） | **拆分为 `H-WP2-9`（telemetry）+ `H-WP2-10`（log）** |
| `H-WP2-4` | 是否需要 `schema_version` | **✅ 纳入**：`INTEGER NOT NULL DEFAULT 1` |
| `H-WP2-6` | 是否需要 `boot_seq`（telemetry 域） | **✅ 删除**（仅保留于 `log_history`） |
| `H-WP2-7` | 是否需要 `seq` + 幂等唯一键 | **✅ 纳入 `seq`（NOT NULL）**；键 = `(device_id, type, ts, seq)` |

> **★ 编号迁移（透明留痕）**：原 `H-WP2-9`（`mqtt_messages`）→ **`H-WP2-11`** · 原 `H-WP2-10`（`device_event` 合并）→ **`H-WP2-12`** · 原 `H-WP2-11`（`ts` 口径）→ **`H-WP2-13`**（腾出 9/10 承载 retention 两项）。
> **★ 纪律**：§6.1 各项**一律 `HUMAN_REQUIRED`**，**agent 不代决**。它们是 **`G-3`（Schema 冻结）** 的裁决内容；**本轮未裁决任何一项**。

---

## 7. 纪律声明（本轮边界）

- **本文件为 REVIEW / SELF-CHECK ONLY**：**不含代码**，**不执行任何实施动作**。
- **未修改** `P3-WP2-History-Data-Model-Design.md`（被检对象**原样保留**）· `P3-WP1-Telemetry-Policy-Final.md` · `P3-History-D1-Plan.md` · `cloud_protocol.md` · 冻结文档。
- **未修改 `src/`** · `platformio.ini` · 固件行为 · Cloudflare Worker · EMQX rule。
- **未创建 migration** · **未写 D1** · **未部署**。
- **未把任何 `NOT RUN` / `HUMAN_REQUIRED` / `BLOCKED` 转为 `PASS`**。
- **未解除任何 Gate**。
- **未 `push`**。
- **`P3` 状态保持 `NOT STARTED / PLANNED`**。

---

> **P3 remains NOT STARTED / PLANNED.**
> **No implementation performed.**
> **No migration created. No Gate released.**

---

*本文件于 **2026-10-10** 建立，为 **WP-2 自检审核报告（REVIEW ONLY）**；不含任何实现、迁移或代码改动。*
*被检对象 = `P3-WP2-History-Data-Model-Design.md`（R2 / G-3 候选）；基线 = 固件仓 HEAD `24ffc2a`。*
