# P3 Entry Gate Review（P3 实施入口评审）

> **性质**：**PLAN REVIEW ONLY（仅评审）**。本文件**不含代码、不执行任何实施动作**，只做**入口条件的核对与裁决留痕**。
> **状态**：**`P3 = NOT STARTED / PLANNED`** · **入口评审结论 = `NOT PASSED`（条件未齐）** · **`push = DEFERRED`**
> **评审对象**：`docs/architecture/P3-History-D1-Plan.md`（**R2**，`3383d5d`）
> **上位依据**：`Cloud-APP-Platform-Plan.md`（§10 P3 · §7.3）· `P2-8-Closure-Review.md` · `P2-8-Archive-Record.md` · `docs/issues/未修复的问题.md`（BT-1）
> **评审时点**：**2026-10-10** · 固件仓 HEAD = **`3383d5d`**（工作树 clean）
>
> **⚠️ 术语铁律（本项目统一状态）**：`PASS` · `NOT RUN` · `HUMAN_REQUIRED` · `BLOCKED` · `DEFERRED` · `CLOSED`
> **`NOT RUN` / `HUMAN_REQUIRED` / `BLOCKED` 一律 ≠ `PASS`**。本评审**不把任何未满足项写成 PASS**。

---

## 0. 结论摘要

| 维度 | 结论 |
|---|---|
| **A · P3 Plan R2 状态** | **`PASS`** —— 文件存在 · R2 修订记录存在 · Scope / WP / Acceptance 完整（见 §A） |
| **B · 前置阶段状态** | **`PASS`** —— P1 `CLOSED` · P2 `SOFTWARE CLOSED` · P3 `NOT STARTED`（见 §B） |
| **C · Entry Gate 条件** | **未通过** —— `G-1 HUMAN_REQUIRED` · `G-2 HUMAN_REQUIRED` · `G-3 HUMAN_REQUIRED` · `G-4 BLOCKED`（见 §C） |
| **当前允许** | **WP-1 设计准备** · **Telemetry Policy 讨论**（见 §3） |
| **当前禁止** | 建 migration · 改 Worker · 改 EMQX rule · 写 D1 · 部署任何 P3 实现（见 §4） |
| **P3 状态** | **`P3 = NOT STARTED / PLANNED`**（**未改变**） |

> **★ 一句话**：**计划本身已就绪（A/B 全 PASS），但「实施入口」尚未开放** ——
> 四项入口条件（`G-1…G-4`）中，一项待人工批准、两项待人工裁决、一项被 `BT-1` 阻塞。
> **⇒ 目前只允许做"设计准备与讨论"，不允许任何实施动作。**

---

## A. P3 Plan R2 状态检查（`PASS`）

| # | 检查项 | 结果 | 证据 |
|---|---|---|---|
| **A-1** | **文件存在** | **`PASS`** | `docs/architecture/P3-History-D1-Plan.md`（414 行 / 25,899 B） |
| **A-2** | **R2 修订记录存在** | **`PASS`** | 文件头「**修订：R2（2026-10-10 · 依人工审核意见）**」逐条登记 7 项变更；`git log` 见 **`3383d5d`** `docs(p3): revise P3 plan per review feedback` |
| **A-3** | **Scope 完整（IN / OUT）** | **`PASS`** | §2.1 IN = 任务 **3.1–3.7**；§2.2 OUT = Web UI · Android · 原生 App · 出粮机构 · P2-7 硬件验证 |
| **A-4** | **Work Package 完整** | **`PASS`** | §3 = **WP-1 … WP-7**（遥测策略 / D1 schema / EMQX 集成 / ingest Worker / LogHistory / 容量验证 / 保留策略） |
| **A-5** | **Acceptance 完整** | **`PASS`** | §5 = `PASS` A-1…A-7 · `NOT RUN` N-1…N-4 · `HUMAN_REQUIRED` H-1…H-6 · `BLOCKED` B-1 / B-2 |
| **A-6** | **Entry Gate 章节存在** | **`PASS`** | §4.3 P3 Entry Gate + §8 门禁 **G-1…G-5** |
| **A-7** | **R2 关键约束已就位** | **`PASS`** | WP-1「telemetry policy 唯一权威位置」· WP-4 Worker 边界收紧 · WP-2 `payload` + EAV 说明 · 容量预测改为理论估算口径 |

> **A 组小结**：**7/7 `PASS`** —— 计划文档本身**无缺口**，可作为评审对象。

---

## B. 前置阶段状态检查（`PASS`）

| # | 检查项 | 结果 | 证据 |
|---|---|---|---|
| **B-1** | **P1 `CLOSED`** | **`PASS`** | `HANDOFF.md` §11.1：「`P1（含 P1-5 / P1-6 / P1-7 / P1-8）= CLOSED`」；`P1-Cloud-Device-Lifecycle-Plan.md` |
| **B-2** | **P2 `SOFTWARE CLOSED`** | **`PASS`** | `P2-8-Closure-Review.md`：「`P2 = SOFTWARE CLOSED / HARDWARE DEFERRED`」（§23 Checklist 26/26 · §17.2.1 PASS · ACL 5/5） |
| **B-3** | **P3 `NOT STARTED`** | **`PASS`** | `HANDOFF.md` §11.1「下一阶段 = P3 · History / D1（NOT STARTED）」；本文件即入口评审，**尚未开始任何实施** |
| **B-4** | **P0 `CLOSED`**（P3 更早前置） | **`PASS`** | `HANDOFF.md` §11.1：「`P0 = CLOSED`」；`P0-Final-Review.md` |
| **B-5** | **P2 硬件部分状态如实** | **`DEFERRED` → `NOT RUN`** | P2-7 硬件 16 项保持 `NOT RUN`；**与 P3 无依赖**，可并行（R-1…R-6） |

> **B 组小结**：**前置阶段链条完整**（P0 / P1 / P2 均已收口）⇒ **P3 的技术前置无阻塞**。

---

## C. Entry Gate 条件检查（**未通过 —— 4 项中 0 项满足**）

| 门禁 | 条件 | **当前状态** | 依据 / 说明 |
|---|---|---|---|
| **G-1** | **P3 Plan approved（经人工审核通过）** | **`HUMAN_REQUIRED`** | 计划已提交（`3383d5d`），**人工批准尚未给出**；对应 Acceptance **H-6**。**本评审不替代批准动作。** |
| **G-2** | **Telemetry Policy 已冻结**（§7.3 终稿） | **`HUMAN_REQUIRED`** | `Cloud-APP-Platform-Plan.md` §7.3 标题仍为「**【待设计】，P3 必须定稿**」⇒ **未冻结**；对应 Acceptance **H-1**（阈值 `X` / 抽样周期 `N`）。 |
| **G-3** | **Schema 已冻结** | **`HUMAN_REQUIRED`** | `TelemetryHistory` / `LogHistory` 列集合**尚未定稿**；对应 Acceptance **H-2**（schema 是否锁死），并牵连 **H-3**（`mqtt_messages` 去留）· **H-4**（保留窗口）· **H-5**（与 `device_event` 合并）。 |
| **G-4** | **`BT-1` 已 `CLOSED`** | **`BLOCKED`** | `docs/issues/未修复的问题.md` 中 **`BT-1`（云端从不回送 `log_ack`）仍为 OPEN**（设备侧无解，需云端实现 `c:"log_ack"`）⇒ **WP-5 LogHistory 实施被阻塞**（对应 **B-2 / U-2**）。 |
| **G-5** | **P3 范围未扩大** | **`PASS`** | 计划 §2.2 OUT 清单明确；至今**未触碰** OUT 项。 |

> **★ C 组小结**：**入口未开放**。
> - **`G-1`** 待**人工批准**（本文件即为该批准提供依据）。
> - **`G-2` / `G-3`** 待**人工裁决**（属 H-1 / H-2…H-5，**agent 不代决**）。
> - **`G-4`** 被 **`BT-1`** 阻塞 —— **仅阻塞 WP-5**，**不阻塞 WP-1 / WP-2**。

---

## 3. 当前允许（`ALLOWED`）

> 「允许」= **可立即进行，且不产生任何实施副作用**（不写库、不改码、不部署）。

| # | 允许事项 | 边界 |
|---|---|---|
| **3.1** | **WP-1 设计准备** | 讨论 / 起草 **§7.3 终稿**（数据类型 · 频率 · 降频 · 成本）；**仅文档，不含任何设备侧代码** |
| **3.2** | **Telemetry Policy 讨论** | 明确「**唯一权威位置**」（建议=设备侧）· 三层职责分工（采样决策 / 过滤 / 传输存储）；**产出为文档结论** |
| **3.3** | **WP-2 schema 设计草案**（设计层） | 可先出**草案**（含 `payload` 体积限制策略 · EAV 说明）；**⚠️ 列集合 finalize 依赖 `G-2`（U-1）** |
| **3.4** | **本评审文档本身** | 评审 / 修订本文件，供人工批准 `G-1` |

> **★ 注意**：以上均属「**设计准备**」。**"设计准备" ≠ "实施"**。

---

## 4. 当前禁止（`FORBIDDEN`）

| # | 禁止事项 | 理由 |
|---|---|---|
| **4.1** | **创建 migration**（含 `0003_*`） | `G-3` 未裁决；schema 未冻结 |
| **4.2** | **修改 Cloudflare Worker**（ingest / 任何 `/api/**`） | `G-1` 未批准；且需 `G-2` 冻结后才谈实现 |
| **4.3** | **修改 EMQX rule / 数据集成** | 同上（WP-3 属"实现"类，受 `B-1` 阻塞） |
| **4.4** | **写入 D1**（建表 / 插数据 / 改 schema） | `G-3` 未裁决；本阶段**不允许任何 D1 变更** |
| **4.5** | **部署任何 P3 实现**（Worker 部署 / 规则下发 / Cron） | 入口未开放 |
| **4.6** | **改固件 `src/` / `platformio.ini` / partition** | P3 为云侧为主；且实施未授权 |
| **4.7** | **改冻结文档**（`P2-0-…freeze.md`） | 冻结基线不可擅改 |
| **4.8** | **`push`** | 归档裁决 = `DEFERRED`（见 `P2-8-Archive-Record.md`） |

> **★ 一句话**：**在 `G-1` 批准前，P3 只准"写文档、做讨论"，不准"动系统"。**

---

## 5. Acceptance（本项目统一六态）

### 5.1 `PASS`（已核对通过）

| ID | 项 |
|---|---|
| A-1 … A-7 | P3 Plan R2 的 7 项完整性检查（见 §A） |
| B-1 … B-4 | 前置阶段 P0 / P1 / P2 / P3 状态核对（见 §B） |
| G-5 | P3 范围未扩大 |

### 5.2 `NOT RUN`（P3 尚不可执行，**不得声称通过**）

| ID | 项 | 原因 |
|---|---|---|
| N-1 | 容量实测（11 台真实行数） | 需真实设备流量 |
| N-2 | 端到端落库验证（设备→EMQX→ingest→D1） | 需部署 + 真实流量 |
| N-3 | 保留策略实际生效验证 | 需部署 + 时间跨度 |
| N-4 | BT-1 修复后的上行升频实测 | 需设备侧负载评估 |

### 5.3 `HUMAN_REQUIRED`（人工裁决，**agent 不代决**）

| ID | 项 | 对应门禁 |
|---|---|---|
| H-1 | 阈值 `X` / 抽样周期 `N` 取值 | **G-2** |
| H-2 | **schema 是否锁死** | **G-3** |
| H-3 | `mqtt_messages` 去留 | **G-3** |
| H-4 | 保留窗口取值 | **G-3** |
| H-5 | 是否与 `device_event` 合并 | **G-3** |
| **H-6** | **是否批准 P3 进入实施** | **G-1** |

### 5.4 `BLOCKED`（当前无法推进）

| ID | 项 | 阻塞源 |
|---|---|---|
| B-1 | WP-3 / WP-4（**实现**） | `U-1`（telemetry policy 未冻结）—— **WP-1 / WP-2 不受阻** |
| B-2 | WP-5（**实施**） | `U-2`（**`BT-1` 未 `CLOSED`**） |

### 5.5 `DEFERRED`（已裁决延后）

| ID | 项 | 说明 |
|---|---|---|
| D-1 | **`push`** | 三仓 commit 仅留本地（固件 `origin/wb` 落后） |
| D-2 | **P2-7 硬件实物验证** | 16 项 `NOT RUN`，延至 Hardware Integration / App 联调（R-1…R-6） |
| D-3 | P2-8-KI-1 / KI-2 修复 | 保持 Known Issue，Hardware Integration 阶段重评估 |

### 5.6 `CLOSED`（已关闭）

| ID | 项 | 说明 |
|---|---|---|
| C-1 | **P0** | `CLOSED` |
| C-2 | **P1**（含 P1-5 / P1-6 / P1-7 / P1-8） | `CLOSED` |
| C-3 | **P2-0.2 / P2-1…P2-8** | `CLOSED`（P2 阶段级 = SOFTWARE CLOSED / HARDWARE DEFERRED） |

> **★ 纪律**：`NOT RUN` / `HUMAN_REQUIRED` / `BLOCKED` 均 **≠ `PASS`**；本评审**未把任何一项转为 PASS**。

---

## 6. Entry Gate 裁决表（**供人工批准**）

> **★ 本表为留痕位**：由人工在「裁决」列填写；**agent 不填**。

| 门禁 | 条件 | 当前状态 | **人工裁决**（待填） |
|---|---|---|---|
| **G-1** | P3 Plan approved | `HUMAN_REQUIRED` | ☐ 批准 ☐ 退回修改 ☐ 挂起 |
| **G-2** | Telemetry Policy 冻结 | `HUMAN_REQUIRED` | ☐ 批准 ☐ 退回修改 ☐ 挂起 |
| **G-3** | Schema 冻结 | `HUMAN_REQUIRED` | ☐ 批准 ☐ 退回修改 ☐ 挂起 |
| **G-4** | `BT-1` `CLOSED` | `BLOCKED` | ☐ 已安排修复 ☐ 暂缓（仅阻塞 WP-5） |
| **G-5** | 范围未扩大 | `PASS` | — |

**批准结论（待填）**：

```
P3 Entry Gate 总评     = ☐ PASSED（可进入实施）  ☐ NOT PASSED（继续设计准备）
批准人                 = ______________
批准日期               = ______________
附加条件               = ______________________________________
```

> **★ 重要**：**在 `G-1` 明确批准前，`P3` 状态保持 `NOT STARTED / PLANNED`**；
> 即便 `G-1` 批准，**`WP-5` 仍须等 `G-4`（`BT-1` `CLOSED`）**方可实施。

---

## 7. 纪律声明（本轮边界）

- **本文件为 PLAN REVIEW ONLY**：**不含代码**，**不执行任何实施动作**。
- **未创建 migration**；**未修改 Cloudflare Worker**；**未修改 EMQX 配置**；**未写 D1**；**未部署任何实现**。
- **未修改固件 `src/` / `platformio.ini` / partition**；**未修改冻结文档**。
- **未把任何 `NOT RUN` / `HUMAN_REQUIRED` / `BLOCKED` 转为 `PASS`**。
- **未替人工裁决** `G-1…G-4`（留空待填）。
- **未 `push`**。
- **`P3` 状态保持 `NOT STARTED / PLANNED`**。

---

*本文件于 **2026-10-10** 建立，为 **PLAN REVIEW ONLY**；用于人工批准 P3 实施入口。*
