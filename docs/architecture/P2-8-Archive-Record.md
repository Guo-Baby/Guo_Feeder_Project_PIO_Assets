# P2-8 Archive Record（P2 阶段归档记录）

> **性质**：**归档记录（documentation-only）**。本文件**只记录人工裁决与最终状态**，不实现、不修复、不改代码。
> **状态**：**`P2-8 = CLOSED`** · **`P2 = SOFTWARE CLOSED / HARDWARE DEFERRED`** · **`P2-9 = NOT STARTED`** · **`push = DEFERRED`**
> **上位冻结文档**：`P2-0-architecture-review-and-design-freeze.md`（**§19** P2-7 判据 · **§23** Freeze Checklist · **§17.2.1** Flash 预算 · **§16** ACL）
> **配套文档**：`P2-8-Closure-Review.md`（收口评审）· `P2-8-Plan.md`（计划与 H-1…H-5）· `P2-7-Hardware-Deferral-Record.md` · `P2-6-Resource-Acceptance-Closure.md` · `docs/issues/未修复的问题.md`（KI 登记）
>
> **⚠️ 术语铁律**：`PASS` · `FAIL` · `NOT RUN` · `HUMAN_REQUIRED` · `DEFERRED` · `CLOSED`
> **`HUMAN_REQUIRED` / `NOT RUN` / `DEFERRED` 一律 ≠ `PASS`**。本记录**不把任何硬件未测试项转换为 PASS**。

---

## 0. 结论摘要

| 维度 | 结论 |
|---|---|
| **P2-8** | **`CLOSED`**（收口评审 `93d7abf` + 文档同步 `c2dea6d`；**P2-8-KI-1/KI-2 的登记不影响 CLOSED**） |
| **P2 阶段级** | **`SOFTWARE CLOSED`** · **硬件 `DEFERRED`（`NOT RUN`，不阻塞）** |
| **push** | **`DEFERRED`** —— 全部 commit **仅保留在本地**，不推送 |
| **scratch** | **`RETAINED`** —— 验证证据环境**保留，不删除、不修改** |
| **P2-8-KI-1 / KI-2** | **`DEFERRED`** —— 保持 **Known Issue**，**不立修复任务**，留待 **Hardware Integration** 阶段重新评估 |
| **P2-9** | **可开始**（技术前置无阻塞）——**但须人工下达指令后方可启动** |

> **★ 一句话**：P2-8 已收口并归档；三类人工裁决（**不 push / 不清理 scratch / KI 不修复**）已落地为记录；
> **P2 的软件与契约部分完整 CLOSED**，硬件部分保持 `DEFERRED`/`NOT RUN`；**P2-9 未被启动**，等待人工下一步指令。

---

## 1. 归档决策（人工裁决落地）

本轮裁决共 **3 项**，全部为**"维持现状"型**决策，**不涉及任何文件内容变更**（除本记录本身）。

### 1.1 D-1 · 不 push（`push = DEFERRED`）

| 项 | 内容 |
|---|---|
| **裁决** | **不 push**。保持所有 commit 在本地。 |
| **影响范围** | 固件仓 · Cloudflare 仓 · EMQX 仓（三仓均已 commit 但未推送） |
| **状态** | **`DEFERRED`**（非 FAIL、非取消；随时可在人工授权后恢复） |

**三仓本地基线（归档时点）**：

| 仓库 | 本地 HEAD | 远端状态 |
|---|---|---|
| 固件 `PIO_Assets/Guo_Feeder_Project` | **`c2dea6d`** | `origin/wb` 停在 **`623da59`** ⇒ **本机领先若干 commit，未推** |
| 云端 `Cloudflare_Assets/guo-feeder-api` | **`392f181`** | 未推 |
| 消息 `EMQX_Assets` | **`eec9444`** | 未推 |

> **说明**：`push` 属发布动作，与"代码正确性"无关。**`push = DEFERRED` 不改变任何阶段结论**（P2 的 CLOSED / PASS 判据均为本地可验证项，不依赖远端）。

### 1.2 D-2 · 不清理 scratch（`scratch = RETAINED`）

| 项 | 内容 |
|---|---|
| **裁决** | **不清理 scratch**。保留全部验证证据环境。 |
| **理由（人工给定）** | 这些目录属于 **P2-6 / P2-7 验证证据环境**。 |
| **约束** | **不要删除，不要修改。** |
| **状态** | **`RETAINED`** |

### 1.3 D-3 · P2-8-KI-1 / KI-2 保持 Known Issue

| 项 | 内容 |
|---|---|
| **裁决** | **保持 Known Issue 状态**。 |
| **约束** | **不创建修复任务** · **不修改代码** · **不修改配置默认值**。 |
| **确认事项** | ① **已登记** · ② **不影响 P2-8 CLOSED** · ③ **后续在 Hardware Integration 阶段重新评估**。 |
| **状态** | **`DEFERRED`** |

---

## 2. push 状态（`DEFERRED`）

- **本轮及此前各阶段的 commit 一律未推送**，保持在本地。
- **不执行** `git push`（含任何形式：普通 push / force push / push 到新分支）。
- **恢复条件**：仅在**人工明确授权**后，方可恢复推送；**恢复时须先核对**远端 `origin/wb` 与本地基线，避免非快进（non-fast-forward）。
- **与既有声明的关系**：`P2-8-Closure-Review.md` §9.2 的「**未 push**」描述的是**评审时点**的事实；本记录描述的是**归档时点**的**决策**（`DEFERRED`）。两者**不冲突**：前者为事实陈述，后者为决策登记。

---

## 3. scratch 保留清单（`RETAINED`）

**归档时点存在性核验（全部 EXISTS）**：

| 路径（相对 `Guo_Feeder_Project/`） | 归属 | 状态 |
|---|---|---|
| `.pio/build/p27swoff` | P2-7 三构型独立 clean build（A default） | **EXISTS · 保留** |
| `.pio/build/p27swon` | P2-7 三构型独立 clean build（**B Homie-enabled**） | **EXISTS · 保留** |
| `.pio/build/p27swprod` | P2-7 三构型独立 clean build（C production+Homie） | **EXISTS · 保留** |
| `.p26base`（worktree，`@623da59`） | P2-6/P2-7 **基线对照 worktree** | **EXISTS · 保留** |
| `.p2verify`（worktree） | P2 验证 environment | **EXISTS · 保留** |

> **说明**：上述目录**均在 `.gitignore` 覆盖范围**（不会进入版本库），因此**保留与否完全不影响仓库清洁度**。
> 除上表外，`P2-6 Flash Optimization / Feasibility Audit` 期间产生的 `.pio/build/p27on` / `p27lto*` / `p27noexc` / `p27nsp` 等实验构型目录**同样一并保留**（本轮**不做任何清理**）。

**未执行的操作（明确声明）**：未删除任何目录；未修改任何 scratch 内容；未运行任何清理脚本。

---

## 4. P2-8-KI-1 / KI-2 登记确认（`DEFERRED`）

**登记位置**：`docs/issues/未修复的问题.md` →「🆕 P2-8 阶段遗留项登记（2026-10-10 · **仅登记，不修复**）」

| ID | 摘要 | 处置 | 本次裁决确认 |
|---|---|---|---|
| **P2-8-KI-1** | `weight` 代码默认 `dt=4` / `sck=5` 与 OLED I2C（`sda=4`/`scl=5`）**引脚冲突**（设备端 `weight.json` 缺 `dt`/`sck` 键时触发；当前 `weight.enable=false` 且仓库文件写 6/7 ⇒ **无实际影响**） | 仅登记 | **已登记 ✓** · **不影响 P2-8 CLOSED ✓** · **Hardware Integration 阶段重新评估 ✓** |
| **P2-8-KI-2** | `weight.enable` **三方不一致**（仓库 `false` / 代码缺键默认 `true` / 设备端缺键 ⇒ 实际 `true`）⇒ 未接 HX711 时产生非阻塞告警噪声 | 仅登记 | **已登记 ✓** · **不影响 P2-8 CLOSED ✓** · **Hardware Integration 阶段重新评估 ✓** |

> **两者为何不影响 `P2-8 = CLOSED`**：P2-8 的验收标准（`P2-8-Plan.md` §5）判定的是**评审与文档交付**，
> 而 KI-1/KI-2 均为**已登记、已定性的非缺陷已知项**（§23 Checklist 未涵盖、§19 判据未依赖、P2-6 资源判据未依赖）。
> 二者**既不构成 FAIL（F-1…F-5 均不命中），也不阻塞任何验收项**。⇒ **CLOSED 结论成立**。

**未执行的操作（明确声明）**：未修改任何默认值；未修改配置系统；未修改 `weight` 模块；未创建修复任务；未改动任何源码。

---

## 5. HUMAN_REQUIRED 事项（H-1…H-5）处置

来源：`P2-8-Plan.md` §5.3。

| ID | 事项 | 本轮状态 |
|---|---|---|
| **H-1** | P2 阶段级结论的最终措辞（软件侧 CLOSED + 硬件 DEFERRED 双状态是否被接受） | **已接受**（本轮裁决以"P2-8 已完成"确认双状态措辞） |
| **H-2** | 是否 push | **`RESOLVED` → 不 push（`DEFERRED`）**（见 §1.1） |
| **H-3** | 是否清理 scratch | **`RESOLVED` → 不清理（`RETAINED`）**（见 §1.2） |
| **H-4** | 是否把配置分歧立为修复任务 | **`RESOLVED` → 不立任务，保持 KI（`DEFERRED`）**（见 §1.3；范围亦涵盖 `mqtt_keep_alive` 死配置——同为已知项，同样不修） |
| **H-5** | P2-8 之后进入哪条线（App 联调 / W-1 / W-2 / P3） | **仍 `HUMAN_REQUIRED`**（见 §8；本轮**未裁决**，等待人工下一步指令） |

---

## 6. P2 最终状态（归档时点）

```
P2-0.2             = CLOSED / FROZEN      （Flash ceiling ≤ 14 KiB，见 P2-0 §17.2.1）
P2-1 … P2-6        = CLOSED
P2-7 软件侧         = SOFTWARE READY / CLOSED
P2-7 硬件侧         = DEFERRED → NOT RUN  （HUMAN_REQUIRED；不阻塞）
P2-7 含硬件 Overall = NOT RUN             （§19 要求 Step 1~10 全 PASS ⇒ 不宣称 PASS）
P2 §23 Checklist   = 23 FROZEN 全 COMPLIANT + 3 DEFERRED 全 HELD
P2-8               = CLOSED               （评审 93d7abf + 文档同步 c2dea6d + 本归档记录）
P2 阶段级           = SOFTWARE CLOSED / HARDWARE DEFERRED
P2-9               = NOT STARTED
push               = DEFERRED
scratch            = RETAINED
```

**四态分布（阶段级）**：

| 分类 | 内容 |
|---|---|
| **PASS** | §23 Checklist 26/26 对齐 · §17.2.1 Flash 预算 `+12,548 ≤ 14,336` · ACL/topic 5/5 双向覆盖 · 真实 broker 23/23 · 回归 728 PASS / 0 FAIL · 静态契约 · Homie 软件侧制品 |
| **NOT RUN** | §19 依赖实物/真 broker 的端到端步骤（Valve 动作 · HX711 闭环 · 电磁阀断电 · retained `/set` · discovery · LWT · 第二设备隔离 等 16 项） |
| **HUMAN_REQUIRED** | H-5（下一阶段走向）；真机执行须人工完成 |
| **DEFERRED** | P2-7 硬件验证（延至 Hardware Integration / App 联调）· push · P2-8-KI-1/KI-2 修复 · `mqtt_keep_alive` 死配置 |

---

## 7. P2-9 是否可以开始

| 维度 | 结论 |
|---|---|
| **技术前置** | **无硬阻塞** —— P2-0.2 已冻结 · P2-1…P2-6 已 CLOSED · P2-7 软件就绪且硬件部分已正式 `DEFERRED`（**不阻塞**）· P2-8 已 CLOSED |
| **可开始性** | **可开始**（技术层面） |
| **启动条件** | **须人工明确下达指令后方可启动** —— 本轮**未启动**，也未做任何 P2-9 相关设计或实现 |
| **边界** | P2-9 的**范围、目标、验收标准均尚未定义**（不在本记录内定义，亦不推定） |

> **★ 纪律**：**"可开始" ≠ "已开始"**。本记录仅陈述技术前置已清空这一**事实**，**不代表**授权进入 P2-9。

---

## 8. 下一阶段建议入口（供人工抉择，非裁决）

**候选线（对应 `P2-8-Plan.md` H-5）**：

| 入口 | 说明 | 备注 |
|---|---|---|
| **A · Hardware Integration / 真机回归** | 按 `P2-7-Hardware-Deferral-Record.md` §5 的 **R-1…R-6 返回节点**执行硬件侧验证（并随 App 联调做端到端） | **唯一能让 16 项 `NOT RUN` 转为实测结果的路径** |
| **B · App 联调（与 A 合并）** | 硬件就绪后，与 App 侧一起做端到端联调（R-6） | 依赖硬件可用 |
| **C · 其他软件线（W-1 / W-2 / P3）** | 纯软件推进（不依赖硬件） | 具体范围须人工定义 |

**建议**：**以 A/B（Hardware Integration + App 联调）为首选返回点** —— 因为 P2 唯一未完成部分即硬件端到端，
且 3 项 KI 的重新评估（§1.3）与 2 项配置分歧的最终处置都挂在**真机阶段**。**最终入口由人工裁决（H-5 仍未定）。**

---

## 9. 未解决事项列表

| # | 事项 | 分类 | 处置 / 去向 |
|---|---|---|---|
| 1 | §19 硬件端到端 16 项 | **`NOT RUN`** | 待 **Hardware Integration**（R-1…R-6） |
| 2 | **P2-8-KI-1**（weight `dt/sck` 默认与 OLED I2C 冲突） | **`DEFERRED`** | 已登记；Hardware Integration 阶段重新评估；**本轮不修** |
| 3 | **P2-8-KI-2**（`weight.enable` 三方不一致） | **`DEFERRED`** | 已登记；真机前人工 `config_set`+`config_save` 写 `false`；**本轮不修** |
| 4 | `mqtt_keep_alive` 死配置（生效 120 s，`config_set/save` 无效） | **`DEFERRED`** | 既有登记；属 P2 范围外，**不立任务** |
| 5 | 第二设备 isolation 实测 | **`NOT RUN`** | 需第二台真实 ESP32-S3 |
| 6 | 真机 discovery / `$state=ready` / LWT 验证 | **`NOT RUN`** | 需真机 + 真 broker |
| 7 | retained `/set` 真机两场景（A/B） | **`NOT RUN`** | 需真机 |
| 8 | **H-5**：P2-8 之后进入哪条线 | **`HUMAN_REQUIRED`** | 等待人工裁决 |
| 9 | push 三仓 | **`DEFERRED`** | 等待人工授权 |
| 10 | scratch 清理 | **`DEFERRED`**（保留） | 等待人工授权（当前裁决：保留） |

---

## 10. 纪律声明（本轮边界）

- **本记录为 documentation-only**：**未修改任何 `src/` 固件源码、`platformio.ini`、partition**。
- **未修改 Cloudflare / EMQX 代码与 ACL contract**（两仓 clean，无改动）。
- **未修改任何冻结架构文档**（`P2-0-…freeze.md` 一字未动）。
- **未把任何硬件未测试项转换为 PASS**；`NOT RUN` / `HUMAN_REQUIRED` / `DEFERRED` 三态保持严格区分。
- **未修复 P2-8-KI-1 / KI-2**（仅确认登记状态）；**未改任何默认值 / 配置系统 / weight 模块**。
- **未清理、未修改任何 scratch 目录**。
- **未 push**。
- **未进入 P2-9**，未定义其范围，未开发任何 P2 范围外功能。
- **未扩大 P2 scope**。

---

*本记录于 **2026-10-10** 依人工归档裁决建立；为 P2 阶段的最终归档凭据。*
