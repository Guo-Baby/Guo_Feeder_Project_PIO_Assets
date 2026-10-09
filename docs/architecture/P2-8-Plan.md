# P2-8 Plan（收口评审 + 文档同步）—— **PLAN ONLY / NOT STARTED**

> **性质**：**计划文档（documentation-only）**。本文件**仅描述 P2-8 的目标 / 计划 / 依赖 / 验收标准**，
> **未执行** P2-8 的任何内容。
> **状态**：**`P2-8 = NOT STARTED`**（等待人工批准开工）
> **上位冻结定义**：`P2-0-architecture-review-and-design-freeze.md` **§22** ——
> **P2-8 = 收口评审 + 文档同步**（`readme.md` · `docs/README.md` · `HANDOFF.md` · `cloud_protocol.md` §Homie 章节）
> **前置**：P2-7（见 `P2-7-Software-Readiness-Report.md` · `P2-7-Hardware-Deferral-Record.md`）

---

## 1. 目标（Goals）

| # | 目标 | 说明 |
|---|---|---|
| G-1 | **P2 收口评审** | 对 P2-0…P2-7 全部交付做一致性评审：契约、topic、ACL、预算、文档 vs 实现是否自洽 |
| G-2 | **文档同步（4 个权威文档）** | 把 P2（Homie Bridge）的最终形态写入项目权威文档，使「新会话/AI 读文档即可开工」 |
| G-3 | **P2-7 状态的正确传播** | 在文档中明确 **`SOFTWARE READY` / 硬件 `DEFERRED`（`NOT RUN`）** 的双重状态，杜绝"P2 已全绿"的误读 |
| G-4 | **遗留项集中登记** | 把 P2 期间的 OPEN/DEFERRED/待办（含 P2-4-OPEN-1、P2-3-OPEN、配置分歧）收拢到权威清单 |
| G-5 | **为下一阶段铺路** | 明确 P2 之后的边界（W-1 OLED / W-2 米家 / App 联调 / P3），不越过边界实现 |

---

## 2. 范围（Scope）

### 2.1 做（IN）

| 项 | 目标文件 | 现状（2026-10-09 核查） |
|---|---|---|
| P2 收口评审报告（新增） | `docs/architecture/P2-8-Closure-Review.md` | 不存在 |
| 文档总索引补 P2 条目 | `docs/README.md` | **P2-0/2-1…2-7 相关文档均未索引**（现仅列到 `Cloud-APP-Platform-Plan.md`） |
| 交接文档同步 | `HANDOFF.md` | **严重过期**（`最后更新 2026-10-04`，**全篇 0 处 P2 引用**） |
| MQTT 协议补 §Homie 章节 | `docs/interfaces/cloud_protocol.md` | **全篇 0 处 `homie`**（Homie 5 投影协议未写入协议权威文档） |
| 项目总说明补 Homie 模块 | `readme.md` | **0 处 Homie / P2 引用**（`src/homie/` 未在模块地图中出现） |

### 2.2 不做（OUT — 明确边界）

| 不做 | 原因 |
|---|---|
| ❌ 不实现 / 修改任何 `src/` 固件代码 | P2-8 = 收口评审 + 文档同步（冻结定义） |
| ❌ 不改 Cloudflare / EMQX 代码与 ACL contract | 同上；ACL 已于 P2-5 冻结 |
| ❌ 不改架构冻结文档（`P2-0-…freeze.md`） | 冻结内容；如需变更须走独立修订（如 P2-0.2 式） |
| ❌ 不把 P2-7 硬件项标为 PASS | 硬件仍 `NOT RUN`；P2-8 **只同步状态**，不改判据 |
| ❌ 不执行硬件测试 | 已延期（见 `P2-7-Hardware-Deferral-Record.md`） |
| ❌ 不启动 W-1 / W-2 / App 联调 / P3 的实现 | **不提前实现未来阶段功能** |
| ❌ 不 push（除非人工授权） | 延续既有纪律 |

---

## 3. 实施计划（Implementation Plan）

> 顺序即依赖序。**每步产出可独立 commit**（代码与文档分离；P2-8 预期**全部为 doc-only commit**）。

### WP-1 · 证据冻结与范围核对（前置门禁）

| 步骤 | 动作 | 判据 |
|---|---|---|
| 1.1 | 记录开工基线：三仓 HEAD（固件 / Cloudflare / EMQX）+ 工作树 clean | `git rev-parse HEAD` + `git status --porcelain` 均记录 |
| 1.2 | 核对 P2 交付物齐全：`src/homie/*`（4 文件）· `src/cloud/mqtt_transport.*` · `src/cloud/mqtt_routing.h` · `credential.js` 5 条 ACL · `acl-rules.v3.json` | 逐文件存在性 + 关键断言 |
| 1.3 | 重跑 P2 全量回归（不改代码，纯验证） | 结果与 P2-7 软件侧报告 §1.4 一致（728 PASS / 0 FAIL） |

### WP-2 · P2 收口评审报告（新增文档）

| 步骤 | 动作 |
|---|---|
| 2.1 | 逐条对齐 **§23 Freeze Checklist**（23 FROZEN + 3 DEFERRED）与实现现状 |
| 2.2 | 复核 **§19 三级成立判据** 的完成度：软件侧 PASS / 硬件 NOT RUN 分列（**不得合并**） |
| 2.3 | 复核资源预算（§17.2.1 ≤14 KiB）与 P2-6 Closure 一致性 |
| 2.4 | 复核 ACL ↔ 固件 topic 5/5 一致性（引用 P2-7 §1.3.3 与 broker 23/23） |
| 2.5 | 汇总 **OPEN / DEFERRED / 待办**：`P2-4-OPEN-1`（设备侧无法区分命令终态）· `P2-3-OPEN`（LogManager EventId 无冻结编号；Flash 已由 P2-0.2 解决）· 配置分歧两项（weight `dt/sck` 默认冲突、`weight.enable` 三方不一致）· `mqtt_keep_alive` 死配置 |
| 2.6 | 产出 `docs/architecture/P2-8-Closure-Review.md`，给出 **P2 阶段级结论**（软件侧 CLOSED / 硬件 DEFERRED） |

### WP-3 · `docs/README.md` 索引同步

| 步骤 | 动作 |
|---|---|
| 3.1 | 在 `docs/architecture/` 分类下补齐 P2 系列条目：`P2-0-…freeze.md`（P2-0.2）· `P2-5-Homie-ACL.md` · `P2-6-Resource-Acceptance.md` + `-Closure.md` · `P2-7-Hardware-Test-Readiness.md` · `P2-7-Software-Readiness-Report.md` · `P2-7-Hardware-Deferral-Record.md` · `P2-8-Plan.md` · `P2-8-Closure-Review.md` |
| 3.2 | 为每条加一行「一句话说明 + 状态」，并标注 P2-7 的**双重状态**（软件 READY / 硬件 DEFERRED） |
| 3.3 | 校验：文档中出现的**所有** P2 路径均真实存在（脚本化存在性检查） |

### WP-4 · `HANDOFF.md` 同步

| 步骤 | 动作 |
|---|---|
| 4.1 | 更新「最后更新」日期与 §1 必读清单（加入 P2 收口文档、P2-7 双状态） |
| 4.2 | 新增/更新「当前阶段状态」段：P2-0.2 FROZEN · P2-1…P2-6 CLOSED · **P2-7 软件侧 SOFTWARE READY / 硬件 DEFERRED（不阻塞）** · P2-8 |
| 4.3 | 记录**未 push** 状态与三仓 HEAD，便于下一个会话核对 |
| 4.4 | 记录已知设备端配置分歧与「硬件再入节点」（指向 Deferral Record §4） |

### WP-5 · `docs/interfaces/cloud_protocol.md` 补 §Homie

| 步骤 | 动作 |
|---|---|
| 5.1 | 新增 `§Homie（Homie 5 投影）` 章节：topic 结构（`homie/5/<id>/…`）· `$state` 生命周期（`init`→`ready`/`lost`）· `$description` 结构与体积上限（4096 B，实测 1,907/1,618 B）· 属性/命令映射摘要（4 node / 16 property / 6 命令）· `/set` 规则（裸值、非 JSON 信封；retained 一律丢弃） |
| 5.2 | 明确**与既有 `guo_feeder/<id>/{down,up,log}` 协议并存**的关系（单一 MQTT 客户端、Transport Facade 分派） |
| 5.3 | 标注**来源**为 §19/§12.2/§16.2 冻结条款（**不复制判据**，只引用），避免双份真源 |
| 5.4 | 显式声明：**该章节描述协议，不代表硬件已验证**（引用 Deferral Record） |

### WP-6 · `readme.md` 模块与架构同步

| 步骤 | 动作 |
|---|---|
| 6.1 | 源码地图补 `src/homie/`（`device_model` · `homie_projection.h` · `homie_bridge`），并说明与 `cloud/` 的并列关系与**单向依赖** |
| 6.2 | 架构节补 Homie 投影层的数据流（Device Model + Registry → Projection → Facade → MQTT） |
| 6.3 | 编译构型补 `GF_HOMIE_ENABLED`（默认 0）与 production 构型用法 |
| 6.4 | 不改动与 P2 无关的既有章节 |

### WP-7 · 收尾核对与提交

| 步骤 | 动作 |
|---|---|
| 7.1 | `git status` 复核"改动真的在"（对每个改动的 md 做 `git hash-object` vs `git rev-parse HEAD:<f>` 复核，**不用 md5/字节数**） |
| 7.2 | 文档内所有交叉引用路径存在性检查 |
| 7.3 | 输出 `git diff --stat` + 修改文件列表 + 评审结论摘要 |
| 7.4 | 按 WP 分 commit（doc-only，中文标题 `docs(p2-8): …`），**禁 `git add -A`** |
| 7.5 | 输出完整报告（含每个 commit 的 hash 与 message） |

---

## 4. 依赖条件（Dependencies）

### 4.1 已完成的前置（开工即可用）

| 依赖 | 状态 | 证据 |
|---|---|---|
| P2-0.2 架构冻结（含 §17.2.1 Flash 预算） | **FROZEN** | `ce948c3` |
| P2-1…P2-4 实现 + 回归 | **PASS** | 各阶段 commit + 软件侧报告 §1.4 |
| P2-5 Homie ACL（CF + EMQX，双路径收敛） | **CLOSED** | `392f181` / `eec9444` / `c1f525c` |
| P2-6 资源验收（新 ceiling） | **CLOSED / PASS** | `f924c5b` |
| P2-7 软件侧 | **SOFTWARE READY** | `9c632a8` + 本轮 deferral 记录 |
| P2-7 硬件侧 | **DEFERRED（`NOT RUN`）** | Deferral Record；**不阻塞 P2-8** |

### 4.2 开工需满足

| # | 条件 | 说明 |
|---|---|---|
| D-1 | 人工**批准开工** | 本文件为 PLAN ONLY；P2-8 未开始 |
| D-2 | 工作树 clean、三仓 HEAD 已记录 | WP-1 门禁 |
| D-3 | **接受**「P2-7 硬件项在 P2-8 中仍为 `NOT RUN`」 | P2-8 只同步状态，**不**制造硬件 PASS |
| D-4 | 不改冻结文档 | 如需改 `P2-0-…freeze.md` 须独立授权 |
| D-5 | 是否 push 需人工决定 | 默认不 push |

### 4.3 阻塞项

| 阻塞项 | 状态 |
|---|---|
| —— | **无**（P2-8 无硬阻塞；此前 P2-7 的 ACL 前置已于 P2-5 解除） |

---

## 5. 验收标准（Acceptance Criteria）

> 全部条目按 `PASS` / `FAIL` / `NOT RUN` / `HUMAN_REQUIRED` 明确分类。**硬件相关一律 `NOT RUN`。**

### 5.1 必须 PASS（Agent 可自动验证）

| # | 判据 | 类别 |
|---|---|---|
| A-1 | P2 交付物齐全（`src/homie/*` 4 文件 · `mqtt_transport.*` · `mqtt_routing.h` · ACL 5 条双路径） | **PASS 要求** |
| A-2 | P2 全量回归重跑：**728 PASS / 0 FAIL**（+7 已登记 legacy `EXPECTED_FAILURE`），与 P2-7 §1.4 一致 | **PASS 要求** |
| A-3 | 三构型构建 `grep -c 'error:'` = 0；无新增 warning | **PASS 要求** |
| A-4 | 文档索引（`docs/README.md`）中 **每一个** P2 路径均真实存在 | **PASS 要求** |
| A-5 | `cloud_protocol.md` 出现 §Homie 章节，且 topic/`$state`/`$description`/`/set` 描述与 §19/§12.2/§16.2 **一致**（脚本化文本断言） | **PASS 要求** |
| A-6 | `readme.md` 出现 `src/homie/` 模块条目与单向依赖说明 | **PASS 要求** |
| A-7 | `HANDOFF.md` 的「最后更新」为本次日期，且含 P2 阶段状态段 | **PASS 要求** |
| A-8 | `P2-8-Closure-Review.md` 存在，含 Freeze Checklist 逐条对齐 + OPEN/DEFERRED 清单 | **PASS 要求** |
| A-9 | 本次全部改动 **doc-only**：`git diff --stat` 中**无** `src/` · `platformio.ini` · `partitions.csv` · Cloudflare/EMQX 文件 | **PASS 要求** |
| A-10 | 冻结文档 `P2-0-…freeze.md` **未被修改** | **PASS 要求** |
| A-11 | 每个改动 md 的「改动真的在」复核通过（`git hash-object` == `git rev-parse HEAD:<f>`） | **PASS 要求** |

### 5.2 必须保持 NOT RUN（不得在 P2-8 内变成 PASS）

| # | 项 | 类别 |
|---|---|---|
| N-1…N-16 | P2-7 的全部硬件项（Valve 动作 · HX711 闭环 · 安全超时断电 · retained `/set` 真机 · 真机 discovery · LWT · 第二设备隔离 · `$state` 生命周期 · reconnect · Wi-Fi/MQTT 真机 · `weight.zero`/`weight-error`/OQ-3 真机 · boot 耗时 · 抓包 · Mijia） | **NOT RUN** |
| P2-8 Overall（含硬件） | 因硬件 `NOT RUN` ⇒ **不得**声明"P2 全阶段 PASS" | **NOT RUN / 部分** |

### 5.3 HUMAN_REQUIRED（人工裁决）

| # | 项 | 类别 |
|---|---|---|
| H-1 | P2 阶段级结论的**最终措辞**（软件侧 CLOSED + 硬件 DEFERRED 的双状态是否被接受） | **HUMAN_REQUIRED** |
| H-2 | 是否 push（固件 / Cloudflare / EMQX） | **HUMAN_REQUIRED** |
| H-3 | 是否清理 scratch（`.pio/build/p27sw*` · worktree `.p26base` / `.p2verify`） | **HUMAN_REQUIRED** |
| H-4 | 是否把配置分歧（`weight.dt/sck` · `weight.enable` · `mqtt_keep_alive`）立为修复任务 | **HUMAN_REQUIRED** |
| H-5 | P2-8 之后进入哪条线（App 联调 / W-1 / W-2 / P3） | **HUMAN_REQUIRED** |

### 5.4 明确定义为 FAIL 的情形

| # | 情形 |
|---|---|
| F-1 | 任何 `src/` / Cloudflare / EMQX / 冻结文档 被修改 |
| F-2 | 任何硬件项被标为 PASS，或出现 mock 冒充真机 |
| F-3 | 文档索引存在指向不存在文件的条目（死链） |
| F-4 | `cloud_protocol.md` §Homie 与冻结条款不一致（topic / `$state` / 判据漂移） |
| F-5 | 回归出现 FAIL 或 `UNEXPECTED_PASS` |

---

## 6. 风险与应对

| 风险 | 影响 | 应对 |
|---|---|---|
| **工作区 Markdown 被外部格式化器改写/截断** | 文档损坏、内容静默丢失 | 每次写入后**立即** `head/tail + 行数` 校验，提交后 `git hash-object` vs `git rev-parse HEAD:<f>` 复核（本项目已发生 1 次**文件头被截断 229 行**） |
| §Homie 章节与冻结条款**双真源漂移** | 协议文档与实现不一致 | 章节只**引用**冻结条款（§19/§12.2/§16.2），复制判据需加**校验断言**（A-5） |
| 文档同步把 P2-7 硬件项"顺手写成 PASS" | 违反纪律、误导后续 | A-9/N-1…N-16 硬约束 + 收口评审显式分列 |
| 范围蔓延到 W-1/W-2/P3 | 扩大 scope | §2.2 OUT 清单 + H-5 人工裁决 |
| `HANDOFF.md` 补写时误删既有内容 | 交接信息丢失 | 只做**增量**编辑；改后全文行数/关键小节存在性核对 |

---

## 7. 状态

```
P2-8              = NOT STARTED （本文件为 PLAN ONLY）
开工前置           = D-1 人工批准（无硬阻塞）
预计产出           = 1 份收口评审报告 + 4 个权威文档同步（全部 doc-only）
预计 commit        = 3~5 个 doc-only commit（按 WP 划分，中文标题 docs(p2-8): …）
验收               = §5（A-1…A-11 PASS；N-1…N-16 NOT RUN；H-1…H-5 HUMAN_REQUIRED）
```
