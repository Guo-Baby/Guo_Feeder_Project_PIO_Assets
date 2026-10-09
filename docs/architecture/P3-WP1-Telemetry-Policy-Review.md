# P3 · WP-1 —— Telemetry Policy Review（遥测策略设计审核报告）

> **性质**：**REVIEW ONLY（仅审核）**。本文件**不含代码**、**不修改任何既有文档**，只产出**审核结论与人工裁决材料**。
> **状态**：**`P3 = NOT STARTED / PLANNED`** · **`P3 Entry Gate = NOT PASSED`**（`G-2` **未解除**）· **`push = DEFERRED`**
> **被审对象**：`P3-WP1-Telemetry-Policy-Design.md`（WP-1 设计草案）
> **上位依据**：`Cloud-APP-Platform-Plan.md`（**§7.3 遥测 → 历史数据策略【待设计】** · **§7.4 瓶颈结论**）· `P3-History-D1-Plan.md`（**R2**，`3383d5d`）· `P3-Entry-Gate-Review.md`（`836791b`）
> **接口依据**：`docs/interfaces/cloud_protocol.md`（**§8.2** log Topic 预留 · **§12.3** `$state` 生命周期 · **§12.4** `$description` · **§12.5** 属性/命令映射 · **§12.6** `/set`）
> **评审时点**：**2026-10-10** · 固件仓 HEAD = **`91bd5f2`**（工作树 clean）
>
> **⚠️ 术语铁律**：`PASS` · `NOT RUN` · `HUMAN_REQUIRED` · `BLOCKED` · `DEFERRED` · `CLOSED`
> **`NOT RUN` / `HUMAN_REQUIRED` / `BLOCKED` 一律 ≠ `PASS`**。
>
> **⚠️ 边界**：本审核**不冻结**任何策略；**不解除 `G-2`**；**不替人工裁决**。裁决（= 解 `G-2`）须由**人工**依 §4 的 Decision List 作出。

---

## 0. 审核结论摘要

| 维度 | 结论 |
|---|---|
| **① 唯一权威位置** | **正确** —— 与 `P3-History-D1-Plan.md` WP-1「★★ 架构约束」**逐条一致**；**未发现隐含的第二套 / 第三套 policy**（详见 §2.1） |
| **② 数据分类完整性** | **不完整** —— 8 类覆盖 §7.3，但 **`system/*` 的 5 个遥测属性未分类**（`F-2`）；**未显式区分 telemetry / control event**（`F-3`） |
| **③ 降频策略** | **方向正确**；Weight `X` / `N`、Online persistence、Heartbeat 均**如实标 `HUMAN_REQUIRED`**；Log 的 `BT-1` 依赖**判断正确**但**引用命名空间歧义**（`F-1`）· **落库级别口径未声明**（`F-6`） |
| **④ 文档一致性** | `§7.3` / R2 计划 / `cloud_protocol.md` 三源**口径一致**（§2.5 抽查）；无冲突性引入 |
| **⑤ 是否可作 §7.3 终稿基础** | ✅ **是** —— **架构骨架可直接沿用**；建议先吸收 `F-1…F-6` 的**澄清性修订**后再冻结（见 §5） |
| **⑥ 当前阻塞项** | **`G-2` Telemetry Policy 未冻结**（`HUMAN_REQUIRED`）· **`G-1` 计划待批准**（`HUMAN_REQUIRED`）· **`G-4` `BT-1` 未 `CLOSED`**（`BLOCKED`，**仅 WP-5**） |

> **★ 一句话结论**：**WP-1 草案的架构是正确的、可作为 §7.3 终稿的基础；但存在 6 处需澄清/补齐之处（其中 `F-2` `F-3` 为分类完整性缺口），且 5 项参数必须由人工裁决。⇒ 建议先修订、再由人工冻结。**

---

## 1. 审核范围与方法

**范围**：仅对 `P3-WP1-Telemetry-Policy-Design.md` 的**设计正确性**作静态审核 —— 不含实现、不含代码。

**方法**：把草案的每条断言与**三份权威源**逐项比对（`grep` 计数 + 逐行核对），并核对文档中所有**引用 ID / 常量 / 属性名**是否真实存在。

| 比对维度 | 权威源 | 手段 |
|---|---|---|
| 架构分层（谁定义 / 谁执行） | `P3-History-D1-Plan.md` **WP-1 · WP-4** | 逐条对齐 |
| 数据类别 ↔ 属性模型 | `cloud_protocol.md` **§12.5**（Device Model 摘要） | 属性名逐个计数 |
| 数据类别 ↔ §7.3 表 | `Cloud-APP-Platform-Plan.md` **§7.3** | 6 行逐项覆盖检查 |
| Online / Log 语义 | `cloud_protocol.md` **§12.3 · §8.2** + P1-6 `stale` 语义 | 术语 + 语义核对 |
| 引用 ID 真实性 | `docs/issues/未修复的问题.md` · `P3-History-D1-Plan.md` §6 | ID 存在性 + 命名空间核对 |

**审核未做（明确声明）**：未运行任何代码；未连接任何 broker；未读 D1；未做任何实测。

---

## 2. 架构正确性审核

### 2.1 唯一权威位置（**逐项核对**）

| 核对项 | 草案位置 | 权威源 | 结论 |
|---|---|---|---|
| **设备侧 = 采样决策** | §1.2 / §1.3 | R2 WP-1 表「设备侧 → 采样决策」 | ✅ **一致** |
| **EMQX = 只执行过滤** | §1.2 / §1.3 | R2 WP-1 表「EMQX → 过滤，不重新定义阈值」 | ✅ **一致** |
| **Worker = 只负责存储** | §1.2 / §1.3 | R2 WP-1 表「Worker → 传输与存储，不做业务判断」 | ✅ **一致** |
| **Worker 4 职责** | §1.2（隐含） | R2 **WP-4**：边界校验 / 批量写入 / 幂等 / 持久化 | ✅ **一致** |
| **Worker 禁止项** | §1.4 反模式 | R2 **WP-4**：禁判"该不该存"/"重量是否有效"/实现降频/解释语义 | ✅ **一致** |
| **禁止三层同时实现降频** | §1.1 | R2 WP-1 铁律 | ✅ **一致** |

**隐含第二套 / 第三套 policy 检查**（逐项确认草案内部**无越权**）：

| 草案中的降频要素 | 声明所在层 | 是否越权 |
|---|---|---|
| Weight「变化 > Xg 才记 / 每 N 分钟一条」 | **设备侧**（§3.3） | ✅ 未越权 |
| Water / Climate「每 5–15 分钟一条 或 超阈值」 | **设备侧**（§3.4 / §3.5） | ✅ 未越权 |
| EVENT「去重 / 边沿策略」 | **设备侧决策层**（§3.1 注明） | ✅ 未越权 |
| Log「级别过滤 + `LOG_TX_MIN_INTERVAL_MS` 发送限流」 | **设备侧**（§3.8，`LOG_TX_MIN_INTERVAL_MS` 实存于 `src/log/log_events.h:524`） | ✅ 未越权 |
| Online「仅记录迁移点」 | **设备侧状态**（§3.6） | ✅ 未越权 |

> **⇒ `§2.1` 结论**：**唯一权威位置 = 设备侧**成立；**EMQX / Worker 均被正确定位为"执行者"**；**草案内部未出现任何"第二套阈值"或"Worker 内业务判断"**。**该项 `PASS`。**

### 2.2 数据分类完整性

**对照 `cloud_protocol.md` §12.5（4 node / 16 property / 6 命令）逐项归属检查**：

| `cloud_protocol` §12.5 属性 | 草案归属 | 结论 |
|---|---|---|
| `water/valve` | 类别 2 Valve action | ✅ 已覆盖 |
| `water/weight` · `water/weight-error` · `water/zero` | 类别 3 Weight | ✅ 已覆盖 |
| `climate/temperature` · `climate/humidity` | 类别 5 Temp/Humidity | ✅ 已覆盖 |
| `climate/battery` · `climate/scan-enabled` | §3.5 注 → **显式排除**（元数据） | ✅ 已显式处置 |
| `system/mqtt-connected` | 类别 6 Online state（§2 来源列） | ✅ 已覆盖 |
| **`system/wifi-rssi`** | **未出现（0 次）** | ⚠️ **缺口（`F-2`）** |
| **`system/wifi-signal`** | **未出现（0 次）** | ⚠️ **缺口（`F-2`）** |
| **`system/time-valid`** | **未出现（0 次）** | ⚠️ **缺口（`F-2`）** |
| **`system/event-queue`** | **未出现（0 次）** | ⚠️ **缺口（`F-2`）** |
| **`system/mqtt-retries`** | **未出现（0 次）** | ⚠️ **缺口（`F-2`）** |
| `control/computer-reset` | **未出现（0 次）** | ⚠️ **未声明**（属控制面，见 `F-3`） |

**硬件能力假设检查**（"是否臆造不存在的硬件"）：

| 类别 | 草案表述 | 结论 |
|---|---|---|
| **Water（水位）** | §2 注 + §3.4"**当前无该传感器** ⇒ 由 Weight 派生" | ✅ **未臆造**，如实标注 |
| **Temp/Humidity** | §3.5"**条件落库（Mijia 可用时）**" | ✅ **条件化正确**（Mijia `LYWSD03MMC` 非核心切片） |
| **Weight** | §3.3"高频" | ⚠️ 未提 `weight.enable` 依赖 ⇒ `F-4` |

**telemetry 与 control event 区分检查**：

- 草案**全文档未出现**「control event / 控制事件」（计数 0）⇒ **未显式区分**（`F-3`）。
- 风险：`cloud_protocol.md` §12.6 的 `/set` → action（`VALVE_OPEN` / `VALVE_CLOSE` / `WEIGHT_ZERO` / `COMPUTER_RESET`）是**客户端下行指令**；而 §7.3 的「供水动作」是**设备侧动作观测**。二者语义不同 ⇒ 若不声明边界，落地时可能把"客户端命令流"误当 telemetry 入 `TelemetryHistory`。

> **⇒ `§2.2` 结论**：**分类基本正确但不完整** —— `system/*` **5 个遥测属性未分类**（`F-2`）· **未区分 telemetry / control event**（`F-3`）· **Weight 未声明 `weight.enable` 依赖**（`F-4`）。**该项 = 需修订。**

### 2.3 降频策略审核

| 数据类 | 草案降频 | 审核意见 |
|---|---|---|
| **Weight** | 阈值 `X` + 抽样 `N`；标 `HUMAN_REQUIRED`（H-W1-1/-2） | ✅ **方向正确、未代决**；⚠️ 缺少 `weight.enable` 前置依赖（`F-4`） |
| **Online** | 「仅记录 `$state` 迁移点」；并明确「**不得用周期性 offline 事件当在线历史**」 | ✅ **语义正确**（与 P1-6「`last_seen` 超时只标 `stale`、不产生 `offline`」一致）；⚠️ 术语 `disconnect` 应作 `disconnected`（`F-5`）；`sleeping` / `alert` 未声明处置 |
| **Heartbeat** | ❌ **不落库**（"降频到 0"） | ✅ **与 §7.3 一致**；标 `HUMAN_REQUIRED`（H-W1-3） |
| **Log** | 级别过滤（INFO 不落 Flash）+ 发送限流；**受 `BT-1` 门禁** | ✅ **`BT-1` 依赖判断正确**（`BT-1` 确为 OPEN，`docs/issues/未修复的问题.md:323`）；⚠️ **引用 `R-4` 命名空间歧义**（`F-1`）· **云端落库级别口径未声明**（`F-6`） |

> **⇒ `§2.3` 结论**：**降频方向与分层正确**；未替人工决定任何参数。**需修订处** = `F-1`（引用）· `F-4`（依赖）· `F-5`（术语）· `F-6`（口径）。

### 2.4 引用真实性抽查（ID / 常量 / 属性名）

| 草案引用 | 真实性 | 证据 |
|---|---|---|
| `R-3`（§3.1「跳变源导致 `dispense_guard` 反复强制关阀」） | ✅ **内容真实**，但属 **`docs/issues/未修复的问题.md`** 命名空间 | `:327` |
| `R-4`（§3.8「修 `BT-1` 后上行升至 2 Hz（×30）」） | ✅ **内容真实**，但属 **`P3-History-D1-Plan.md` §6 Risks** 命名空间 | `:375` |
| `LOG_TX_MIN_INTERVAL_MS`（§3.8） | ✅ 实存 | `src/log/log_events.h:524` |
| `BT-1`（§3.8） | ✅ OPEN | `docs/issues/未修复的问题.md:323` |
| `U-3` / `mqtt.json.telemetry`（§5） | ✅ 预登记 | `P3-History-D1-Plan.md:297` |

> **⚠️ `R-n` 命名空间碰撞**（详见 `F-1`）：本仓至少 4 套 `R-n` ——
> ① `docs/issues/未修复的问题.md`（`R-3` = 跳变源强制关阀）
> ② `P3-History-D1-Plan.md` §6（`R-3` = **schema lock-in**；`R-4` = **log volume**）
> ③ `P2-7-Hardware-Deferral-Record.md`（返回节点 `R-1…R-6`）
> ④ `P1-*` / `P2-Flash-Budget-Reconciliation.md`（各自 `R-4`）
> ⇒ **同一份草案内 `R-3` 与 `R-4` 取自不同命名空间**，易误读。

---

## 3. 发现清单（Findings）

> **分级**：🔴 高（影响落地正确性）· 🟠 中（分类/引用歧义）· 🟡 低（术语/口径）
> **均仅"建议"，本轮**不修改**任何被审文档（见 §5）。**

### `F-1` · `R-n` 引用命名空间混用 — 🟠 中

| 项 | 内容 |
|---|---|
| **文档位置** | `P3-WP1-Telemetry-Policy-Design.md` §3.1（`R-3`）· §3.8（`R-4`） |
| **冲突内容** | 同文档内 `R-n` 取自两个命名空间：§3.1 的 `R-3` ← `issues` 文档；§3.8 的 `R-4` ← `P3 plan §6`。而 `issues` 文档**无 `R-4`**（计数 0）；`P3 plan §6` 的 `R-3` 又是 **schema lock-in**。跨文档还有第 ③④ 套 `R-n`。 |
| **修改建议** | 引用加前缀：`issues#R-3` / `P3-plan§6#R-4`；或在 §3 表加「**引用命名空间**」列 |

### `F-2` · `system/*` 遥测属性未分类（分类完整性缺口） — 🟠 中

| 项 | 内容 |
|---|---|
| **文档位置** | §2 分类表 · §3 逐类定义 · §4 汇总矩阵 |
| **冲突内容** | `cloud_protocol.md` §12.5 的 `system` node 有 6 属性；草案只覆盖 `mqtt-connected`，**`wifi-rssi` / `wifi-signal` / `time-valid` / `event-queue` / `mqtt-retries` 全部 0 次**，既未归类也未显式排除（对比 §3.5 对 `climate/battery` 做了显式排除）。 |
| **修改建议** | **新增第 9 类「System health」**（`wifi-*` / `time-valid` / `event-queue` / `mqtt-retries`），给 5 维定义（建议：**低采样 + 短生命周期 + 仅异常时落库**）；若决定不落库，也须**显式排除**并写明理由 |

### `F-3` · 未显式区分 telemetry 与 control event — 🟠 中

| 项 | 内容 |
|---|---|
| **文档位置** | §2 / §3.2 |
| **冲突内容** | 全文无「control event / 控制事件」表述。`/set` → action（`VALVE_OPEN`/`VALVE_CLOSE`/`WEIGHT_ZERO`/`COMPUTER_RESET`，`cloud_protocol.md` §12.6）属**下行控制指令**；§7.3「供水动作」属**设备侧观测**。边界未声明 ⇒ 落地时可能把客户端命令流误入 `TelemetryHistory`。 |
| **修改建议** | §2 增「**纳入 / 排除**」声明：**纳入** = 设备侧观测（action result · state change）；**排除** = 客户端 `/set` 指令本身与 `down` 命令帧；`control/computer-reset` 明确归为**控制面**（仅记录动作结果，不记录命令） |

### `F-4` · Weight 类未声明 `weight.enable` 前置依赖 — 🟠 中

| 项 | 内容 |
|---|---|
| **文档位置** | §3.3 · §4 矩阵 |
| **冲突内容** | 草案标 Weight「高频 / 条件落库」，但未提其依赖 `weight.enable`（当前**设备端 = `false`**，且 `P2-8-KI-2` 三方不一致）。据 `cloud_protocol.md` §12.4：`weight.enable=false` ⇒ `water/weight` · `water/weight-error` · `water/zero` **同时不出现**（16→13 属性）⇒ **无 weight telemetry**。 |
| **修改建议** | §3.3 增「**前置依赖 `weight.enable=true`**；受 `P2-8-KI-2` 配置一致性影响（未修复前实测可能取不到 weight）」 |

### `F-5` · Online state 迁移点术语与 §12.3 不一致 — 🟡 低

| 项 | 内容 |
|---|---|
| **文档位置** | §3.6（写 "disconnect"） |
| **冲突内容** | `cloud_protocol.md` §12.3 规范名为 **`disconnected` / `lost`**；草案用非规范名 `disconnect`；且 `sleeping` / `alert`（Homie 5 其余两态）**未声明处置**。 |
| **修改建议** | 统一为 `init` / `ready` / `disconnected` / `lost`；并声明 `sleeping` / `alert` **本设备不使用 ⇒ 显式排除** |

### `F-6` · Log 类"落库级别"口径未声明（可能与设备 Flash 口径混淆） — 🟡 低

| 项 | 内容 |
|---|---|
| **文档位置** | §3.8「仅 **WARN / ERROR / CRITICAL 落设备 Flash**」 |
| **冲突内容** | 该句描述的是**设备 Flash 落盘级别**（`cloud_protocol.md` §8.2）；但本类真正要定的是**云端 `LogHistory` 落库级别**。两者是否同一集合，草案未声明。 |
| **修改建议** | 显式写明「**`LogHistory` 落库级别 = `{WARN, ERROR, CRITICAL}`，与设备侧一致**」，并注明依据 §8.2 |

### 逐项确认无问题（**正向结论**）

| ID | 项 | 结论 |
|---|---|---|
| **P-1** | 三层职责 / 禁止三层降频 | ✅ 与 R2 WP-1 逐条一致 |
| **P-2** | Worker 4 职责 / 4 禁止 | ✅ 与 R2 WP-4 逐项一致 |
| **P-3** | Water 无传感器 | ✅ **如实标注、未臆造** |
| **P-4** | Heartbeat 不落库 | ✅ 与 §7.3 一致 |
| **P-5** | 容量措辞 | ✅ 已为「理论估算」口径；`< 1200 行/天` **目标保留** |
| **P-6** | Online 不得用周期 offline 事件 | ✅ 与 P1-6 `stale` 语义一致 |

---

## 4. ★ HUMAN_REQUIRED Decision List（人工裁决清单）

> **★ 纪律**：以下 **5 项全部 `HUMAN_REQUIRED`** —— **本报告不替人工决策**。
> 它们正是 **`G-2`（Telemetry Policy 冻结）** 的裁决内容；**裁决完成后**方可产出 §7.3 终稿。
> 「推荐方向」**仅为建议**，**非决定**。

### `H-W1-1` · Weight 阈值 `X`

| 项 | 内容 |
|---|---|
| **决策问题** | 水箱重量变化**超过多少克**才记一条？（直接决定 `TelemetryHistory` 中 Weight 行数与灵敏度） |
| **当前建议** | 使 Weight 类落在 **< 300 行/天**；考虑 HX711 噪声底噪（死区须大于噪声幅度，避免"静置即刷屏"） |
| **可选方案** | **(a)** 小阈值（如 5 g）→ 灵敏度高、行数多 · **(b)** 中阈值（如 20 g）→ 平衡 · **(c)** 大阈值（如 50 g）→ 省额度、灵敏度低 |
| **技术影响** | `X` 越小 ⇒ 行数越多（逼近/超 1200 行/天上限）·越大 ⇒ 小口饮水/慢渗漏可能丢失 ·**该值须与 `N` 协同**（否则兜底抽样会掩盖真实变化） |
| **推荐方向** | **建议 `X` ≈ 噪声 2–3 倍（中档），并以 WP-6 实测回校**；**不代决具体数值** |
| **关联** | `P3-History-D1-Plan.md` **H-1** · `§7.3` 表「重量」行 |

### `H-W1-2` · Weight 抽样周期 `N`

| 项 | 内容 |
|---|---|
| **决策问题** | **至少每多少分钟**记一条 Weight（兜底保底频率，防止"长期无变化"导致数据断档）？ |
| **当前建议** | 与 `X` 协同；使 Weight 类 **< 300 行/天**（`N ≥ 5 min` ⇒ ≤288 行/天，已接近上限） |
| **可选方案** | **(a)** `N = 5 min`（≤288/天）· **(b)** `N = 15 min`（≤96/天）· **(c)** `N = 30 min`（≤48/天） |
| **技术影响** | `N` 越小 ⇒ 基线越密、越费额度 ·越大 ⇒ 故障期"重量平台"可能不可见 ·**与 `X` 叠加**（两条规则取并集 ⇒ 实际行数 = 变化触发 + 兜底触发） |
| **推荐方向** | **建议 `N ≈ 15 min`（兜底），主力靠 `X` 触发**；**不代决** |
| **关联** | `P3-History-D1-Plan.md` **H-1** · §3.3 |

### `H-W1-3` · Heartbeat 是否完全不落库

| 项 | 内容 |
|---|---|
| **决策问题** | 心跳是否**完全不进入历史库**？在线态由**谁**表达？ |
| **当前建议** | **保持"不落库"**（`§7.3` 已如此约定）；在线态由 `H-W1-4` 的迁移点表达 |
| **可选方案** | **(a)** 完全不落库（`§7.3` 现状）· **(b)** 落"降频心跳"（如每 30 min 一条）· **(c)** 仅落异常（超时/重连） |
| **技术影响** | **(a)** 省额度（**这是把总量压到 < 1200 行/天的关键一项**）但**无法从历史库观察"设备是否活着"**（须靠 `H-W1-4`）· **(b)(c)** 增加行数、但与 Online 类功能重叠 |
| **推荐方向** | **建议 (a) 不落库**；若需"存活证据"，走 **(c) 仅落异常**（不落稳态心跳）——**不代决** |
| **关联** | `§7.3`「心跳」行 · `U-3`（device heartbeat 未落地）· 风险 `R-1` |

### `H-W1-4` · Online state 是否落库 + 记录哪些迁移

| 项 | 内容 |
|---|---|
| **决策问题** | 在线状态**是否**落库？若落，记录**哪些迁移点**（`init`→`ready` / `disconnected` / LWT `lost`）？ |
| **当前建议** | **仅记录迁移点**（不记录稳态）；**不得**用周期性 `offline` 事件当在线历史（P1-6：`last_seen` 超时只标 `stale`） |
| **可选方案** | **(a)** 全部迁移点（`init`/`ready`/`disconnected`/`lost`）· **(b)** 仅"上/下线"两点（`ready` / `lost`）· **(c)** 不落库（在线态仅实时查询，不入历史） |
| **技术影响** | **(a)** 可用性审计最完整、行数 < 50/天 · **(b)** 更省、但丢"重连抖动"细节 · **(c)** 历史库无法回溯可用性 · ⚠️ **判据必须只取 `$state`/连接事件**（`stale` 不是"下线"） |
| **推荐方向** | **建议 (a)**（迁移点本就稀疏，`< 50 行/天`，信息价值高）；**不代决** |
| **关联** | `cloud_protocol.md` §12.3 · P1-6 `stale` 语义 · `P3-History-D1-Plan.md` H-4（保留窗口） |

### `H-W1-5` · Telemetry config key 命名

| 项 | 内容 |
|---|---|
| **决策问题** | 设备侧阈值/频率的**配置项键名**如何命名？（走现有 `config`，**不新增 Action / 不新增 Config 模块**） |
| **当前建议** | 沿用 **`mqtt.json.telemetry`**（`P3-History-D1-Plan.md` `U-3` 已预登记该落点，**不新增 Config 模块**） |
| **可选方案** | **(a)** `mqtt.json.telemetry.{weight_threshold_g, weight_interval_min, ...}`（复用 `mqtt` 模块）· **(b)** 新建 `data/config/telemetry.json`（**须改 4 处 ConfigManager 登记** ⇒ 与本阶段"不新增模块"冲突）· **(c)** 并入既有 `weight.json` |
| **技术影响** | **(a)** 零新增模块、配置改动最小；但语义上"遥测"与"MQTT 连接"混居一文件 · **(b)** 语义清晰但**触发 Config 模块登记（4 处）+ 配置迁移**，**扩大范围** · **(c)** 与 Weight 强耦合、但"水位/温湿度"等类无处安放 |
| **推荐方向** | **建议 (a)**（Z 最小、与 `U-3` 预登记一致）；**(b) 因会扩大 scope，不建议在本阶段采用** ——**不代决** |
| **关联** | `P3-History-D1-Plan.md` WP-1 / `U-3` · `§7.3`「设备侧」手段 |

> **★ 5 项合计**：**`H-W1-1`…`H-W1-5` 全部 `HUMAN_REQUIRED`** —— **本轮无一项被裁决、无一项被转为 `PASS`**。

---

## 5. 是否可作 §7.3 终稿基础 + 建议修订

| 项 | 结论 |
|---|---|
| **可作 §7.3 终稿基础？** | ✅ **是** —— 架构骨架（唯一权威位置 · 8 类 · 逐类 5 维 · 已知 `HUMAN_REQUIRED`）**可直接沿用** |
| **是否建议进入人工冻结阶段？** | ⚠️ **建议"先修订、再冻结"** —— 吸收 `F-1…F-6`（均为**澄清性/补齐性**，**不改架构方向**）后，冻结质量更高 |
| **修订是否会改变裁决内容？** | **否** —— `F-2`（补第 9 类）与 `F-3`（边界声明）会**新增**需确认项，但 **`H-W1-1…5` 的裁决问题不变** |

### 5.1 建议修改列表（**本轮不执行**）

| # | 目标文档 | 建议修改 | 对应 |
|---|---|---|---|
| 1 | `P3-WP1-Telemetry-Policy-Design.md` §3.1 / §3.8 | `R-3`/`R-4` 加命名空间前缀 | `F-1` |
| 2 | 同上 §2 / §3 / §4 | 新增第 9 类 **System health**（`system/*` 五属性），或显式排除 | `F-2` |
| 3 | 同上 §2 | 增「纳入 / 排除」声明（telemetry vs control event） | `F-3` |
| 4 | 同上 §3.3 | 增 `weight.enable` 前置依赖 | `F-4` |
| 5 | 同上 §3.6 | `disconnect` → `disconnected`；声明 `sleeping`/`alert` 排除 | `F-5` |
| 6 | 同上 §3.8 | 声明云端 `LogHistory` 落库级别口径 | `F-6` |

> **替代路径**：上述 6 项修订**已全部体现在**本轮新增的 `P3-WP1-Telemetry-Policy-Final-Draft.md`（终稿候选）中 —— 人工可直接以 **Final-Draft** 为冻结基线，从而**无需再改草案**。

### 5.2 不修改列表（**本轮确认不动**）

| 文档 | 理由 |
|---|---|
| `Cloud-APP-Platform-Plan.md` **§7.3** | 冻结前的权威源；**§7.3 终稿须由人工批准后并入**，本阶段不改 |
| `P3-History-D1-Plan.md`（R2） | 已 `3383d5d` 定稿（R2）；本审核未发现需回改之处 |
| `P3-Entry-Gate-Review.md` | `836791b` 定稿；`G-2` **仍不解除** |
| `docs/interfaces/cloud_protocol.md` | 接口契约；本审核**只读**引用 |
| 冻结文档 / 任何代码 / migration / EMQX / Worker | **禁止触碰** |

### 5.3 阻断项汇总

| ID | 项 | 状态 | 影响范围 |
|---|---|---|---|
| **`G-1`** | P3 Plan 批准 | **`HUMAN_REQUIRED`** | 全部实施 |
| **`G-2`** | Telemetry Policy 冻结 | **`HUMAN_REQUIRED`** | 本 WP-1 的产出目标；阻塞 WP-3/WP-4 实现 |
| **`G-3`** | Schema 冻结 | **`HUMAN_REQUIRED`** | WP-2 finalize |
| **`G-4`** | `BT-1` `CLOSED` | **`BLOCKED`** | **仅 WP-5**（Log 落库） |

---

## 6. 纪律声明（本轮边界）

- **本文件为 REVIEW ONLY**：**不含代码**，**不执行任何实施动作**。
- **未修改** `P3-WP1-Telemetry-Policy-Design.md`（被审对象**原样保留**）· **未修改** `P3-History-D1-Plan.md` · **未修改** `P3-Entry-Gate-Review.md` · **未修改** `Cloud-APP-Platform-Plan.md` §7.3 · **未修改** `cloud_protocol.md`。
- **未修改 `src/`** · `platformio.ini` · ConfigManager · MQTT 实现。
- **未修改 EMQX rule** · 未修改 **Cloudflare Worker** · **未创建 migration** · **未写 D1** · **未部署**。
- **未修改冻结文档**；**未把任何 `NOT RUN` / `HUMAN_REQUIRED` / `BLOCKED` 转为 `PASS`**。
- **未解除 `G-2`**；**未替人工裁决** §4 的 5 项。
- **未 `push`**。
- **`P3` 状态保持 `NOT STARTED / PLANNED`**。

---

> **P3 remains NOT STARTED / PLANNED.**
> **No implementation performed.**

---

*本文件于 **2026-10-10** 建立，为 **WP-1 审核报告（REVIEW ONLY）**；不含任何实现、迁移或代码改动。*
*被审对象 `P3-WP1-Telemetry-Policy-Design.md` 的评审基线 = 固件仓 HEAD `91bd5f2`。*
