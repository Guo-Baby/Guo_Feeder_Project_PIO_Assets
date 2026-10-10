# P3 · WP-2 —— History Data Model Review（自检审核报告）

> **性质**：**REVIEW / SELF-CHECK ONLY**。本文件**不含代码、不修改任何既有文档**，只做**对 WP-2 设计的自检与合规核对**。
> **状态**：**`P3 = NOT STARTED / PLANNED`** · **`G-3 = PASS`（已解除）** · **`G-1` / `G-4` 未变** · **`push = DEFERRED`**
> **被检对象**：`docs/architecture/P3-WP2-History-Data-Model-Design.md`（WP-2 设计 · **R2 → R3 → R4 / `FROZEN`（G-3 冻结版）**）
> **自检依据**：`P3-WP1-Telemetry-Policy-Final.md`（**冻结**）· `P3-History-D1-Plan.md`（R2）· `migrations/README.md`
> **自检时点**：**2026-10-10** · R2 基线 HEAD = **`24ffc2a`** · R3 基线 HEAD = **`6cbd519`** · **R4 基线 HEAD = `b047166`**（工作树 clean）
> **★ 修订**：**R2（2026-10-10）** —— 被检对象已修订为 **G-3 候选**（`value` 物理 `TEXT` · telemetry 增 `seq`/`schema_version`、删 `boot_seq`/`trace_id` · 幂等键明确 · 索引调整 · `H-WP2` 重编号）。**自检四项结论不变**；**`H-WP2` 清单同步更新（§6）**。
> **★ 修订**：**R3（2026-10-10）** —— 被检对象**同步人工审核裁决**：**`H-WP2-5`/`12`/`13` → `CLOSED`** · **`H-WP2-9`/`10`/`11` → `DEFERRED`** · **`WP-2` 状态提升为 `DESIGN COMPLETE`**（**`G-3` 仍 `HUMAN_REQUIRED`**）。**自检四项结论仍不变**；**`H-WP2` 清单同步（§6）**。
> **★ 修订**：**R4（2026-10-10 · Schema Freeze）** —— 被检对象**同步人工审核裁决（最终冻结）**：**`H-WP2-1`/`3` → `CLOSED`** · **`H-WP2-8` → `DEFERRED`** · 被检对象已 **`FROZEN`**（**`WP-2 = CLOSED`**）⇒ **`G-3 = PASS`**（**依人工裁决**）。**自检四项结论仍不变**；**`H-WP2` 清单同步（§6）**。
>
> **⚠️ 术语铁律**：`PASS` · `NOT RUN` · `HUMAN_REQUIRED` · `BLOCKED` · `DEFERRED` · `CLOSED`。**一律 ≠ `PASS`**。

---

## 0. 自检结论摘要

| 检查项 | 结果 |
|---|---|
| **是否违反 `WP-1`（Telemetry Policy 冻结）** | ✅ **否** —— 九类落库判定**逐项一致**（详见 §2） |
| **是否引入 Worker policy** | ✅ **否** —— 设计中 Worker **仅** ingest → validation → idempotency → insert（详见 §3） |
| **是否修改实现** | ✅ **否** —— 未动 `src/` / Worker / EMQX / migration / D1（详见 §4） |
| **是否解除任何 Gate** | ⚠️ **`G-3` 已解除（`PASS`）** —— 系**人工裁决记录**，**非本报告自行判定**；**`G-1` / `G-4` 未变**（详见 §5） |
| **是否与 `WP-1` 冲突** | ✅ **无冲突**（§2 逐条） |
| **是否新增 `HUMAN_REQUIRED`** | ✅ **无** —— 全部 `H-WP2-*` 已 **`CLOSED`**（`1`/`2`/`3`/`4`/`6`/`7`/`5`/`12`/`13`）或 **`DEFERRED`**（`8`/`9`/`10`/`11`）；**§6.1 已清空**（§6.2 / §6.3） |
| **是否 `push`** | ✅ **否** |
| **`P3` 状态** | **`NOT STARTED / PLANNED`**（**未变**） |

> **★ 一句话**：**WP-2 设计为纯设计产物，未越界、未违反 `WP-1`、未动实现；其 `H-WP2-*` 已由人工裁决完毕（`CLOSED` / `DEFERRED`）⇒ `G-3` 据此解除（`PASS`）。**

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
| **`G-2`** Telemetry Policy 冻结 | **`PASS`**（**已于 WP-1 解除**，`e015a29`） | **未变** |
| **`G-3`** Schema 冻结 | **`PASS`**（**已于 R4 解除**） | ⚠️ **已解除** —— **依人工裁决**（`H-WP2-*` 全部 `CLOSED`/`DEFERRED`；`H-WP2-1`/`3` = `CLOSED`，`H-WP2-8` = `DEFERRED`）；**非本报告自行判定** |
| **`G-4`** `BT-1` `CLOSED` | **`BLOCKED`** | **未变** |
| **`G-5`** 范围未扩大 | **`PASS`** | **未变** |

> **⇒ §5 结论**：**`G-3` 依人工裁决解除（`PASS`）**；`G-1` / `G-4` **未变**。（`G-2` / `G-3` 的 `PASS` 分别系 WP-1 / WP-2 的**人工裁决记录**，非本报告产生。）

---

## 6. 裁决清单（**`H-WP2-*` · R2 重编号 · R3 / R4 同步人工裁决**）

### 6.1 仍待裁决（OPEN · `HUMAN_REQUIRED`）

> **（无）** —— 全部 `H-WP2-*` 已 `CLOSED` 或 `DEFERRED`；**`G-3` 的裁决输入已清空**。

| ID | 问题 | 状态 |
|---|---|---|
| — | **（无待裁决项）** | — |

### 6.2 已裁决（CLOSED）

#### 6.2.1 由 R2 修订裁决

| 原 ID | 项 | **R2 裁决** |
|---|---|---|
| `H-WP2-2` | retention 周期（合并项） | **拆分为 `H-WP2-9`（telemetry）+ `H-WP2-10`（log）** |
| `H-WP2-4` | 是否需要 `schema_version` | **✅ 纳入**：`INTEGER NOT NULL DEFAULT 1` |
| `H-WP2-6` | 是否需要 `boot_seq`（telemetry 域） | **✅ 删除**（仅保留于 `log_history`） |
| `H-WP2-7` | 是否需要 `seq` + 幂等唯一键 | **✅ 纳入 `seq`（NOT NULL）**；键 = `(device_id, type, ts, seq)` |

#### 6.2.2 ★ 由人工审核裁决（R3）

| ID | 项 | **人工裁决** | 状态 |
|---|---|---|---|
| `H-WP2-5` | 表命名最终确认 | **采用 `telemetry_history` / `log_history`** | **`CLOSED`** |
| `H-WP2-12` | 是否与 `device_event` 合并 | **保持独立**（生命周期事件 ≠ 运行历史事件） | **`CLOSED`** |
| `H-WP2-13` | `ts` 取值口径 | **冻结 = Unix timestamp（秒）** | **`CLOSED`** |

#### 6.2.3 ★ 由人工审核裁决（R4 · Schema Freeze）

| ID | 项 | **人工裁决** | 状态 |
|---|---|---|---|
| `H-WP2-1` | 双表架构（方案 C）是否批准 | **采用方案 C**（`telemetry_history` + `log_history`） | **`CLOSED`** |
| `H-WP2-3` | `log_history.payload` 存储格式 | **冻结 = `TEXT(JSON)`**（物理 `TEXT`，内容 JSON 字符串） | **`CLOSED`** |

### 6.3 已延期（DEFERRED）

| ID | 项 | 归属 | 状态 |
|---|---|---|---|
| `H-WP2-8` | `payload` 最大长度 / 超限处置 | **WP-3 History Storage Policy**（属存储策略，非 schema） | **`DEFERRED`** |
| `H-WP2-9` | telemetry retention 周期 | **WP-3 History Storage Policy** | **`DEFERRED`** |
| `H-WP2-10` | log retention 周期 | **WP-3 lifecycle policy** | **`DEFERRED`** |
| `H-WP2-11` | `mqtt_messages` 去留 | 既有 schema 生命周期管理 | **`DEFERRED`** |

> **★ 编号迁移（透明留痕）**：原 `H-WP2-9`（`mqtt_messages`）→ **`H-WP2-11`** · 原 `H-WP2-10`（`device_event` 合并）→ **`H-WP2-12`** · 原 `H-WP2-11`（`ts` 口径）→ **`H-WP2-13`**（腾出 9/10 承载 retention 两项）。
> **★ 纪律（R3 / R4 同步）**：**§6.1 已清空**；**`H-WP2-1`/`3`/`5`/`12`/`13` 已 `CLOSED`**；**`H-WP2-8`/`9`/`10`/`11` 已 `DEFERRED`**（移交 WP-3 / 既有 schema 生命周期管理）。**`G-3` 据此解除（`PASS`）**；**`G-1` / `G-4` 未变**。

---

## 7. 纪律声明（本轮边界）

- **本文件为 REVIEW / SELF-CHECK ONLY**：**不含代码**，**不执行任何实施动作**。
- **本轮未修改** `P3-WP1-Telemetry-Policy-Final.md` · `P3-History-D1-Plan.md` · `cloud_protocol.md` · 冻结文档。（**被检对象 `P3-WP2-History-Data-Model-Design.md` 已由 R3 / R4 同步人工裁决** —— 属**被检对象本身**，**非本报告**对其做设计改动。）
- **未修改 `src/`** · `platformio.ini` · 固件行为 · Cloudflare Worker · EMQX rule。
- **未创建 migration** · **未写 D1** · **未部署**。
- **未把任何 `NOT RUN` / `HUMAN_REQUIRED` / `BLOCKED` 转为 `PASS`**。
- **未自行解除任何 Gate**（`G-3` 的 `PASS` 系**依人工裁决记录**，非本报告自行判定）。
- **未 `push`**。
- **`P3` 状态保持 `NOT STARTED / PLANNED`**。

---

> **P3 remains NOT STARTED / PLANNED.**
> **No implementation performed.**
> **No migration created. `G-3` released by human decision (`PASS`); `G-1` / `G-4` unchanged.**

---

*本文件于 **2026-10-10** 建立，经 **R2** / **R3** / **R4** 同步，为 **WP-2 自检审核报告（REVIEW ONLY）**；不含任何实现、迁移或代码改动。*
*被检对象 = `P3-WP2-History-Data-Model-Design.md`（R2 → **R3 → R4 / `FROZEN`**）；R2 基线 = 固件仓 HEAD `24ffc2a`；R3 基线 = 固件仓 HEAD `6cbd519`；**R4 基线 = 固件仓 HEAD `b047166`**。*
