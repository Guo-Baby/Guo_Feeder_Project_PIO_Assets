# docs/ 文档索引

> 本目录存放 Guo Feeder 项目的**说明文档 / 接口文档 / 规范 / 归档**。
> 源码在 `../src/`，工程总览见 `../readme.md`。
>
> ★ **新会话请先读 `../HANDOFF.md`** —— 项目当前状态 / 必读文档清单 / 下一步计划。
>
> 整理日期：**2026-10-01**（根目录文档分类归位 + 历史归档）；
> **2026-10-02** 复核并修正过时内容、移出重复章节。

---

## 目录导航

| 目录 | 放什么 | 是否现行 |
|---|---|---|
| [`requirements/`](#requirements-需求) | 产品级需求 | ✅ 现行 |
| [`architecture/`](#architecture-架构) | 架构设计 | ✅ 现行 |
| [`interfaces/`](#interfaces-接口--协议) | **接口 / 协议契约**（最重要） | ✅ 现行 |
| [`modules/`](#modules-模块设计说明) | 各模块设计说明 | ✅ 现行 |
| [`specs/`](#specs-规范--模板--集成指南) | 规范 / 模板 / 集成指南 | ✅ 现行 |
| [`issues/`](#issues-问题清单) | 问题跟踪清单 | ✅ 现行 |
| [`scripts/`](#scripts-脚本) | 部署 / 工具脚本 | ✅ 现行 |
| [`archive/`](#archive-历史归档) | 历史进度、阶段报告、旧需求 | 📦 仅存档 |

---

## requirements/ 需求

| 文档 | 说明 |
|---|---|
| `需求文档.md` | **自动猫咪饮水系统软件需求设计文档 V2.1** —— 产品级需求（水箱 / 重力供水 / 电磁阀 / HX711 按重量控水） |

## architecture/ 架构

| 文档 | 说明 |
|---|---|
| `docs/architecture/Cloud-APP-Platform-Plan.md` | **云端 + 安卓端正式架构规划 v2** —— EMQX Serverless + Cloudflare Worker/Pages/D1 + Homie 5 + MQTT-Tiles；含 P0–P6 实施路线、容量核算、设备身份与 ACL 模型。**P0–P6 的实施依据** |
| `P0-设备身份与Topic隔离设计.md` | **★ P0 实施级设计（FROZEN · v1.2）** —— D1/D2/D3/D5/D7 定案 · `device_id` **三层身份来源**与生命周期（5 场景）· `dver` 迁移硬约束 · 身份边界 · Topic V3 + `topic_render()` · **legacy 回退的生产环境边界** · EMQX ACL 模型 · APP `client_id` 约束 · DEF-2 修复 · 上线顺序与回滚 · 验收 DoD |
| `P0-实现清单.md` | **★ P0 施工工单** —— 冻结基线（含 **P0 不包含**范围清单）· **P0-1…P0-5** 逐项（目标 / 修改文件 / 修改函数 / 修改内容 / 禁止 / 风险 / 验证方法）· 串口用例 S-1…S-10 · MQTT 用例 M-1…M-3 · 静态验证 A-1…A-4 |
| `P0-Final-Review.md` | **★ P0 验收快照（Final Baseline）** —— 冻结基线（Identity / MQTT / ACL / 构建环境）· 已知限制 **L-1…L-7（有意保留，非缺陷）** · P0-1…P0-5 提交与资源增量 · 上板验收（S-1…S-11 / M-1…M-3）· **排障铁律**（`subscribed` ≠ 授权等）· 遗留事项 |
| `P1-Cloud-Device-Lifecycle-Plan.md` | **★ P1 架构规划（已冻结 · 2026-10-02 审核通过）** —— 从「设备身份隔离」升级到「**设备生命周期管理**」：注册状态机（**FACTORY / CLAIM_PENDING / REGISTERED / BOUND / UNBOUND / REVOKED**）· **一机一凭据**（create/rotate/revoke）· **ACL 动态授权时序** · APP 绑定模型 · D1 表结构草案 · 在线状态同步 · **P1 范围冻结** + 前置实测项 + **P1 Architecture Freeze Decision（9 条）** |
| `P1-实现清单.md` | **★ P1 施工工单（仅规划，不编码）** —— **P1-0 前置实测（A-1/A-4/A-5/A-6，阻塞项）** · P1-1…P1-8 逐项（目标 / 预期修改 / 依赖 / 禁止 / 风险 / 验证方法）· 云端 / 设备侧 / 越权用例表 · 验收 DoD · 实施顺序 · 审核清单 |
| `P1-0-EMQX-Capability-Test.md` | **★ P1-0 平台能力实测（已完成）** —— A-4/A-4b（`client_id` 上限 15323）· A-5（账号可程序化增删，上限≈2000）· A-1/A-1c（**ACL 生效 ≈1.3 s，且只作用于新连接**）· A-6（无 `$SYS`，有 `$events/*`）· **A-7（create user 幂等：`201` / `409 ALREADY_EXISTS`、不覆盖密码、可 at-least-once retry）** + **§Design Impact Freeze**（revoke = ACL change + session termination）与 **§A-7 Design Impact** |
| `P1-2-P1-4-注册与凭据签发设计.md` | **★ Phase C 设计（已冻结 · 2026-10-03）** —— P1-2+P1-4 **合并闭环**：发现建档 → 准入 → 建账号 → 写 ACL → **就绪判定（设备 CONNACK 自证）** → 落库 → 下发 → 升级 ACTIVE；含状态/四态映射、失败矩阵、对账、API 契约草案、给 Phase D 的接口冻结，**文末 §Architecture Freeze Decision（Q1–Q13）为不可擅改基线** |
| `P1-3-PhaseD-实施计划.md` | **★ Phase D 实施计划 + §7 结果（D-1/D-2/D-3 已完成）** —— **D-1 云端基础能力**（EMQX API wrapper / admin auth / credential generator / device registry）· **D-2 固件**（`gfcred` 双 slot / `CredentialManager` / `credential_set` / `credential_confirm`）· **D-3 联调**；含冻结边界、文件修改总表、风险登记、接口与验收判据 |
| `PhaseD-Final-Review.md` | **★ Phase D 最终评审（已通过 · 2026-10-04）** —— 范围 / **Q1–Q13 与 P0 边界逐条核对** / 交付物 / **真机 51-51 验收明细** / **三处缺陷与修复**（outbox 排空 · 帧分类 · 切主索引）/ **验收 DoD 对照** / 遗留与风险 / **数据集成端点迁移步骤** / **给 P1-5 的接口与前置** |
| `P1-5-Device-Binding-Design.md` | **★ P1-5 绑定设计（Phase P1-5-A 设计稿 · 正文保留为史料）** —— ownership 模型 · **binding 状态机**（含与凭据流程的相容性核对）· **claim flow**（三种 claim 的区分）· **transfer 原子事务** · API contract · event model · **failure matrix F-1…F-20** · **INV-1…INV-7 不变量** · 必须修改的既有代码（`revokeDevice` 补退役配对）· 测试用例 T-1…T-19 |
| `P1-5E-Implementation-Plan.md` | **★ P1-5-E 实施计划（已实施 · P1-5-1…P1-5-4 完成）** —— 只读代码基线（D1 事务唯一先例 = `activateCredential` / `device_event` 写入方式 / admin 路由结构）· **P1-5-1…P1-5-5 拆分** · **文件级 新增/修改/禁止修改 清单** · API 契约定稿（6 端点 + 错误码）· **事务设计**（bind/unbind/transfer/revoke 的精确语句表 + 冲突检测）· **T-1…T-20 测试映射**（本地 / D1 直查 / 部署版真机三段）· **风险评审 7-1…7-6** · Ready-to-Code 清单 |
| `P1-5-Final-Review.md` | **★ P1-5 收口评审（Final Review · 实施完成 2026-10-04）** —— **设计稿 ↔ 实现的审计桥接**：交付物 · **Δ-A…Δ-K 差异表** · **最终 API 契约（6 端点）** · **INV-1…INV-7 落地**（含 **INV-4 退役必关 binding**）· 事务设计 · 验收结果 · 遗留问题 · 给 P1-5-5 的前置。**对接 APP / 运维看这一篇** |
| `jsonstorage开发架构.md` | JsonStorage 存储层架构设计 |

### P2 · Homie Bridge（设备侧 Homie 5 投影）—— 阶段索引

> **阶段状态**：**`P2 = SOFTWARE CLOSED / HARDWARE DEFERRED`** —— `P2-0.2 = FROZEN` · `P2-1…P2-6 = CLOSED` ·
> **`P2-7 = SOFTWARE READY` · 硬件 `DEFERRED`（`NOT RUN`，不阻塞）** · `P2-8 = CLOSED（评审 + 归档）` · **下一阶段 = `P3 · History / D1`（P2 编号结束于 P2-8）** ·
> **`push = DEFERRED` · `scratch = RETAINED`**。
> 权威依据：`P2-0-architecture-review-and-design-freeze.md`（**§19 / §23 / §17.2.1 / §16**）。

| 文档 | 说明 |
|---|---|
| **`P2-0-architecture-review-and-design-freeze.md`** | **★ P2 架构冻结（FROZEN · 含 P2-0.2 修订）＝P2 不可擅改基线** —— Device Model / Transport Facade / Homie Projection 三层设计；**§23 Freeze Checklist（23 FROZEN + 3 DEFERRED）** · **§19 P2-7 三级成立判据** · **§17.2.1 Flash 预算 `≤ 14 KiB`** · **§16 ACL 模型与职责边界** · §22 依赖图 |
| `P2-8-Closure-Review.md` | **★ P2 收口评审（本阶段权威结论）** —— WP-1 基线冻结与「无未记录代码修改」核验 · **§23 逐条对齐（26/26）** · **§19 完成度（PASS / NOT RUN 分列）** · §17.2.1 预算 · ACL/topic 契约 · 四态分类总表 · 遗留项登记 · `stable_id` 专项审查 · 阶段级结论 |
| `P2-8-Plan.md` | P2-8 计划（收口评审 + 文档同步）：目标 / WP-1…WP-7 / 依赖 / 验收 A-1…A-11 · N-1…N-16 · H-1…H-5 · F-1…F-5（**PLAN ONLY 记录**） |
| `P2-8-Archive-Record.md` | **★ P2-8 归档记录** —— 归档裁决落地（**不 push（`DEFERRED`）/ 不清理 scratch（`RETAINED`）/ KI 保持 Known Issue**）· `P2-8-KI-1`/`KI-2` 登记确认 · H-1…H-5 处置 · P2 最终状态 · 下一阶段（`P3 · History / D1`）可开始性 · 未解决事项列表 |
| `P2-7-Software-Readiness-Report.md` | **P2-7 软件侧 readiness（无硬件条件下的全部验证）** —— A. 已验证 PASS（构建 / 资源零回归 / 静态契约 / 728 断言回归 / Homie 制品 / EMQX 真实 broker）· B. NOT RUN（N-1…N-16）· C. HUMAN_REQUIRED checklist |
| `P2-7-Hardware-Deferral-Record.md` | **P2-7 硬件延期记录** —— 决策与原因 · 16 项硬件项（保持 `NOT RUN`）· **未来返回节点 R-1…R-6**（Valve → HX711 → 真机 MQTT/Homie → 第二设备隔离 → retained/LWT/discovery → **App 联调**）· 再入纪律 |
| `P2-7-Hardware-Test-Readiness.md` | 实物测试准备清单 —— §19 摘要判据 · 责任矩阵 · 需要人工准备的真实设备（A–F）· **接线清单（Valve `GPIO12` / HX711 `DT6·SCK7` / 安全铁律）** · 待确认项 Q-1…Q-8 |
| `P2-6-Resource-Acceptance-Closure.md` | **P2-6 资源验收（新 ceiling ≤14 KiB 下）** —— 四构型段级实测 · `B − base = +12,548 B` **PASS** · 逐项资源复核（DRAM/PSRAM/栈/$description/IRAM） |
| `P2-6-Resource-Acceptance.md` | P2-6 资源验收（**旧预算下的 FAIL 记录，历史保留**）—— 用于对照新 ceiling 的修订依据 |
| `P2-6-Optimization-Feasibility-Audit.md` | Flash 优化可行性审计（`B. POSSIBLE WITH TRADE-OFF`）· Δ 可二分 · 编译选项实测（`-fno-exceptions` 有效 / `-flto` 构建失败） |
| `P2-Flash-Budget-Reconciliation.md` | Flash 预算对账审计（Q1–Q8）· 归因 **`D. 预算与功能共同漂移`** · 重冻选项 A/B/C |
| `P2-5-Homie-ACL.md` | **P2-5 Homie ACL 实施与验证** —— 设备级 5 条 ACL（`credential.js` ⇄ `acl-rules.v3.json` 双路径收敛）· PUT 全量覆盖 · 真实 EMQX 授权实测 23/23 · 补写流程 |

> **设备侧 Homie 模块说明**：`src/homie/readme.md`（职责 / topic / property / `/set` 流程）

### P3 · History / D1 —— 阶段索引

> **阶段状态**：**`P3 = NOT STARTED / PLANNED`** —— 计划已立项（**PLAN ONLY**）；实施须先过 `P3-History-D1-Plan.md` **§8 门禁（G-1…G-5）**并经人工批准。
> **`WP-1 = CLOSED`**（Telemetry Policy 已冻结 `P3-WP1-Telemetry-Policy-Final.md` ⇒ **`G-2` 已解除**）；**`WP-2 = CLOSED`（Schema Design Frozen，`P3-WP2-History-Data-Model-Design.md` ⇒ `G-3` 已解除）**；**`WP-3 = FROZEN`**（History Storage Policy **设计冻结（R3）** · `P3-WP3-History-Storage-Policy-Design.md` ⇒ retention **90 / 180 天** · cleanup **`scheduled cleanup job`** · **`no additional index`** · **不解除任何门禁**）；**`DT:WP-4 = FROZEN` · R5 冻结回填（15 `CLOSED` / 2 `OPEN`）**（History Ingestion & Cloud Pipeline · `P3-DT-WP4-History-Ingestion-Design.md` · **`DESIGN ONLY` · 设计已冻结 · 实施唯一基线 · 人工批准 · 不解除任何门禁 · 无实现** · **入口族统一 `guo_feeder/<device_id>/…` · 端点定稿 = 新专用 `POST /api/history/ingest`（选择 B · 不写 `mqtt_messages`）· 失败模型固化（`schema_version` 前向兼容 · DLQ 界外 · structural validation 边界显式化 · 不引入限流）** · **剩余 `OPEN` = `10` / `12`（均界外可延后）· 无「实施前必裁」项**）；`G-1` / `G-4` **仍未通过**。
> **`P2` 编号结束于 `P2-8`**；**`P3` = 下一主阶段**。

| 文档 | 说明 |
|---|---|
| `P3-History-D1-Plan.md` | **★ P3 实施计划（PLAN ONLY）** —— 背景（P2 已建通信基础设施 / P3 建历史数据平台 / 为 P4 Web UI 铺路）· Scope（**IN 3.1–3.7** / OUT）· **WP-1…WP-7**（遥测策略 · D1 schema 设计 · EMQX 集成 · ingest Worker · LogHistory · 容量验证 · 保留策略）· Dependencies（前置已满足 / **未满足 U-1…U-3**）· Acceptance（**PASS / NOT RUN / HUMAN_REQUIRED / BLOCKED**）· Risks · 门禁 **G-1…G-5** |
| `P3-Entry-Gate-Review.md` | **★ P3 实施入口评审（PLAN REVIEW ONLY · R2 → R3）** —— **A** Plan R2 状态（7/7 `PASS`）· **B** 前置阶段（P0/P1/P2 `CLOSED` · P3 `NOT STARTED`）· **C** Entry Gate（`G-1` `HUMAN_REQUIRED` · **`G-2` `PASS`（已解除）** · **`G-3` `PASS`（已解除）** · `G-4` `BLOCKED`）· **当前允许**（WP-1 `CLOSED` / **WP-2 `CLOSED`（Schema Design Frozen）** / WP-3·WP-4 设计层）· **当前禁止**（建 migration / 改 Worker / 改 EMQX rule / 写 D1 / 部署）· **六态 Acceptance** · **人工裁决留痕表** |
| `P3-WP1-Telemetry-Policy-Design.md` | **★ WP-1 遥测策略设计（DESIGN ONLY · 草案）** —— §1 **唯一权威位置 = 设备侧**（EMQX 只执行过滤 / Worker 只负责存储）+ 反模式 · §2 **全量 8 类**分类 · §3 **逐类 5 维**（落库/频率/降频/生命周期/成本） · §4 汇总矩阵 · §5 **待人工裁决 5 项 `HUMAN_REQUIRED`**（阈值 `X` · 周期 `N` · heartbeat · online persistence · config key） · §6 与 `§7.3` / `P3-History-D1-Plan.md` R2 / `cloud_protocol.md` 一致性 |
| `P3-WP1-Telemetry-Policy-Review.md` | **★ WP-1 审核报告（REVIEW ONLY）** —— §2 架构正确性审核（**唯一权威位置逐条核对** · 数据分类完整性 · 降频策略 · 引用真实性）· §3 **发现清单 `F-1…F-6`**（`R-n` 命名空间混用 · `system/*` 未分类 · 未区分 telemetry/control · `weight.enable` 依赖 · `$state` 术语 · Log 落库级别口径；各含**位置/冲突/建议**）· §4 **`HUMAN_REQUIRED` Decision List**（`H-W1-1…H-W1-5`；ID/问题/建议/选项/影响/推荐方向）· §5 冻结建议与**修改/不修改列表** |
| `P3-WP1-Telemetry-Policy-Final-Draft.md` | **★ §7.3 终稿候选（`FINAL DRAFT`，已被 `…Final.md` 取代）** —— 依审核 `F-1…F-6` 修订：§1 唯一权威位置 · §2 **9 类**（补 **System health**）+ **telemetry/control 边界声明** · §3 逐类 5 维 · §4 矩阵 + **不可加性说明** · §5 **`HUMAN_REQUIRED` `H-W1-1…H-W1-6`** |
| **`P3-WP1-Telemetry-Policy-Final.md`** | **★★ §7.3 Telemetry Policy 最终冻结（`FROZEN` · `WP-1 = CLOSED` · **`G-2` 解冻依据**）** —— §1 唯一权威位置（设备侧 / EMQX 过滤 / Worker ingest·storage）· §2 **9 类** + **telemetry/control 边界** + **`system/*` 归类** · §3 逐类 5 维（含 `weight.enable` 依赖 · `$state` 词表含 `stale` · **Log 双级别分离**）· §4 矩阵 · §5 **人工裁决 `H-W1-1…H-W1-6` 冻结决策**（`N = 15 min` · heartbeat 不落库 · online 迁移 `init`/`ready`/`disconnected`/`lost` · `mqtt.json.telemetry` · system health 仅异常）· §8 `NOT RUN` 待实测项 |
| **`P3-WP2-History-Data-Model-Design.md`** | **★★ WP-2 历史数据模型设计最终冻结（`FROZEN` · `WP-2 = CLOSED`（Schema Design Frozen）· **`G-3` 解冻依据**）** —— §1.2 **schema 状态分类**（`FDD` / `SC` / `HR`）· §2 **方案比较 A/B/C**（**方案 C 分域二表已批准** · **EAV 方向不变**）· §3 主表 `telemetry_history`（`id`/`device_id`/`ts`/`type`/**`value` 物理 `TEXT`**/`source`/**`seq` NOT NULL**/**`schema_version` DEFAULT 1**/`created_at`；**删 `boot_seq`/`trace_id`**）+ 逻辑类型 `number`/`string`/`json` · §3.4 **幂等键**（telemetry `(device_id,type,ts,seq)` / log `(device_id,boot_seq,seq)`）· §4 九类映射（`log_history.payload` = **`TEXT(JSON)`** 冻结）· §5 索引（保留 `device_id+ts` · `device_id+type+ts`；**删 `type+ts`**；log `device_id+level+ts` **暂缓**）· §6 生命周期（**retention `DEFERRED`→WP-3**）· §7 容量（**估算非 PASS**）· §8 **`H-WP2-*` 已全部裁决**（**`CLOSED` `1`/`3`/`5`/`12`/`13`** · **`DEFERRED` `8`/`9`/`10`/`11`**；§8.1 已清空） |
| `P3-WP2-History-Data-Model-Review.md` | **★ WP-2 自检审核报告（`REVIEW ONLY` · R2 → R3 → R4）** —— 自检四项：**是否违反 `WP-1`**（逐项核对，无冲突）· **是否引入 Worker policy**（无，仅 ingest/validation/idempotency/insert）· **是否修改实现**（零代码/零 DB/零云端副作用）· **是否解除 Gate**（**`G-3` 依人工裁决解除 = `PASS`**；`G-1` / `G-4` 未变）· §6 `H-WP2-*` 清单（**R4 同步**：OPEN **0** 项 · `CLOSED` **9** 项 · `DEFERRED` **4** 项） |
| **`P3-WP3-History-Storage-Policy-Design.md`** | **★★ WP-3 历史存储策略设计（**`FROZEN`（设计冻结 · R3 裁决回填）** · `DT:WP-3` ↔ `计划:WP-7` · **不解除任何门禁**）** —— **§1 编号解释**（Design Track ↔ Plan Track 解耦，**全文禁裸 `WP-n`**）· §3 **不变量 `I-1…I-12`** · §4 **冻结方案 = S-B（固定窗口 TTL）** · §5 **telemetry 策略**（**retention 冻结 = 90 天** · **§5.1.1 容量依据** · **按 `device_id` 分批删除** · 不做分区 / 冷热 / 聚合）· §6 **log 策略**（**retention 冻结 = 180 天** · 级别 **L-A 统一窗口** · `payload` 生命周期/上限 **`H-WP3-06` = `DEFERRED`** ⚠️ · **设备 Flash Log ≠ 云端 LogHistory**）· §7 清理机制（**冻结 = `scheduled cleanup job`**）+ **§7.4 删除硬规则 `D-1…D-5`**（**冻结原则 / 不冻结 batch size**）· §8 聚合（**`H-WP3-04` = `DEFERRED`**）· §9 容量模型 1/100/1000/10000 台（**估算 `NOT RUN`**）· §10 查询原则 `Q-1…Q-8` · §11 **裁决回填（`CLOSED` 8 项 / `DEFERRED` 3 项）**；归档 = **future phase · consumer ownership TBD**；**`no additional index`** |
| `P3-WP3-History-Storage-Policy-Review.md` | **★ WP-3 自检审核报告（`REVIEW ONLY` · **R3**）** —— §0 自检摘要（未违反 `WP-1`/`WP-2` · 未引入 Worker policy · 零实现副作用 · **未改任何 Gate** · **冻结系人工裁决**）· §2 与 `WP-1`/`WP-2` **逐条一致性核对** · §3 Worker 边界核对 · §4 副作用核对（含 README 被外部格式化器改写**已从 HEAD 还原**的留痕）· §5 Gate 核对（`G-1`/`G-3`/`G-4` 未变）· §6 **发现 `F-WP3-1…10`**（`1`/`2`/`3`/`4` **已闭环**；**`F-WP3-10` 🔴 `payload` 无上限 —— 由 `H-WP3-06` `DEFERRED` 引起**）· §7 **风险 `R-WP3-1…9`**（`5`/`6`/`7` **已关闭**；**新增 `R-WP3-9`**）· §8 **裁决清单（`CLOSED` 8 / `DEFERRED` 3）** · §9 **R2 + R3 要求逐条对照** |
| **`P3-DT-WP4-History-Ingestion-Design.md`** | **★★ DT:WP-4 历史接入与云端管道设计（`DESIGN ONLY` · **`FROZEN` · R5 冻结回填（15 `CLOSED` / 2 `OPEN`）· 设计已冻结 · 实施唯一基线 · 人工批准 · 不解除任何门禁 · 无实现**）** —— **`DT:WP-4` ↔ `计划:WP-3`（EMQX 集成）+ `计划:WP-4`（ingest Worker）** · §0 状态 + **Gate `G-1…G-5` 逐项未改** · §1 **编号解释**（Design ↔ Plan 解耦 · **禁裸 `WP-n`**）+ 上位依据 U-1…U-6 · §2 **数据入口设计**（链路总览 · **入口族统一 `guo_feeder/<device_id>/…`** · 域→表映射 · **信封解析 + log 键字段 `device_id`/`boot_seq`/`seq`/`payload_base64`** · **device identity（只信 topic；`ID-3` 唯一；`ID-6` 一致性）**）· §3 **Telemetry pipeline `T-1…T-9`** + 归一化红线（**R4：禁按 `type` 丢弃**）+ **§3.3 `schema_version` 处理（R4：`> 1` 接收 + 校验格式 + 原样保存 · forward compatibility）** · §4 **Log pipeline `L-1…L-9`**（`payload_base64` 承载 · **传输编码 ≠ 落库语义** · Worker 不脱敏 · **`log_ack` 界外受 `G-4`**）· §5 **Validation `V-L0…V-L5`**（**`V-L5` = 平台 request body 上限 + 拒收超限**）+ 服务级/记录级失败清单 + **§5.1.1（R4）Worker structural validation 边界：允许 5 类 / 禁止 5 类** + **`INSERT OR IGNORE` 吞 NOT NULL 警示** · §6 **幂等设计**（复用冻结唯一索引 · 零新增索引 · log 键三分量由信封明文携带）· §7 **错误处置**（E-1…E-6 · **§7.4 R4 失败模型固化：临时 5xx→重投→幂等吸收 / 永久 4xx→不回投 · DLQ 不进入 WP-4** · **§7.5 ingest ↔ cleanup 强制解耦**）· §8 **D1 写入策略（禁 DDL）** + 写成本意识 + **`W-1…W-5`** · §9 **安全边界**（**§9.1 R5 端点定稿 = 新专用 `POST /api/history/ingest`（选择 B · 不写 `mqtt_messages` · `X-Webhook-Key` 强制 · 拒 `cloud_api` · 旧 `/api/ingest` 保持现状）** · **不引入限流 / IP 限制 / key rotation；保留 EMQX ACL + Cloudflare 基础防护**）· §10 **`H-WP4-01…17`（`CLOSED` 15 / `OPEN` 2）** + **`F-WP4-1…12`** · §11 **`INV-1…INV-14`** · §12 一致性 · §13 纪律 + **§13.1 冻结确认** |
| `P3-DT-WP4-History-Ingestion-Review.md` | **★ DT:WP-4 自检审核报告（`REVIEW ONLY` · **R5** · 设计已冻结 · 实施唯一基线）** —— §0 **自检八项**（跨冻结已收敛 · Worker policy 未引入且**边界已固化（§5.1.1）** · 零副作用 · **未改任何 Gate** · **R5 裁决 1/1 落实** · `INV-1…14` 全满足 · **端点决策（选择 B）已落定** · **收口完备性：17 项均有终态**）· §1 审核范围与方法 · §2 **与三份冻结文档逐条一致性核对**（WP-1 8 / WP-2 8 / WP-3 6）· §3 **Worker 边界核对**（4 职责 / 4 禁止 + `H-WP4-05` / `17` / `04` 禁令 + **§5.1.1 允许 5 / 禁止 5**）· §4 **发现 `F-WP4-1…12`（R5：`CLOSED` 9 / 部分保留 1 / `OPEN` 1 / `CONFIRMED` 1 · `F-WP4-10` 依裁决关闭）** · §5 **风险 `R-WP4-1…11`（`R-WP4-2` 端点相关残余收敛；其余为「已知并接受」）** · §6 **`H-WP4-*` 清单核对（17 项 · `CLOSED` 15 / `OPEN` 2 · 无新增无重编号无空洞）** · §7 **doc-only 副作用核对** · §8 Gate 核对 · §9 审核结论 + **剩余 `OPEN` 2 项处理建议（均界外可延后 · 无实施前必裁）** |
| **`P3-WP4-Firmware-Integration-Design-Review.md`**（`docs/review/`） | **★★ DT:WP-4 固件侧实施前设计评审（`DESIGN REVIEW ONLY` · 基线 `b813022` · **§9 = 人工裁决留痕**）** —— `WP4-FW-LOG`（Cloud Log Envelope）+ `WP4-FW-TELEMETRY`（Telemetry Publisher）两条工作流 · 数据链路 / 模块边界 / 冻结约束检查（Worker CBOR-blind · 幂等键不变 · 不复用 command uplink · 不新增 topic；**no Cloud protocol regression = `ATTENTION`**）· 影响面与风险登记 `F-WP4-FW-1…10` / `R-WP4-FW-1…11` · **`§9` 裁决：`H-WP4-FW-01`/`02`/`03`/`04` = `CLOSED` · `H-WP4-FW-08` = `CLOSED WITH GUARD`**（`01` = 方案 A 双 Action 分流，**topic / domain 分离优先于 payload inspection**，Worker 不解析 payload）· 残留 `H-WP4-FW-05`/`06`/`07`/`09` = `OPEN` |
| **`P3-WP4-Firmware-Log-Implementation.md`**（`docs/review/`） | **★★ `WP4-FW-LOG` 实施规格记录（`IMPLEMENTATION SPEC` · 冻结输入）** —— §1 数据链路（`LogRecord` → CBOR(`count=1`) → Base64 → Cloud Log Envelope JSON → `cloud_send_log()` → `guo_feeder/<device_id>/log`）· §2 固定规则 `R-1…R-7` + **§2.1 `payload_base64` 精确字节边界**（= `log_cbor_encode_batch(count=1)` 的 188 B，**非**裸 128 B `LogRecord`）+ **§2.2 侧信道计数仍留在 CBOR 批次头** · §3 模块职责与 Envelope 六字段 · §4 资源门禁 `G-1…G-6`（`H-WP4-FW-08` = `CLOSED WITH GUARD`）· §5 允许/禁止清单 |
| **`P3-WP4-Firmware-Telemetry-Implementation.md`**（`docs/review/`） | **★★ `WP4-FW-TELEMETRY` 实施记录 + 资源门禁判定（`IMPLEMENTED` / **`RESOURCE BLOCKED`（启用）**）** —— 数据链路（System State → `telemetry_policy.h` → `telemetry_envelope.h` → `cloud_send_telemetry()` → `/up`）· **不复用 command uplink**（`H-WP4C-02`）· 8 类观测与触发方式（**未臆造** `water level` / `heartbeat` / `lost`）· **Envelope 7 字段（`device_id`/`ts`/`type`/`value`/`source`/`seq`/`schema_version`）标注 `PROVISIONAL`**（`H-WP4-FW-05`/`06`/`07` = `OPEN`，**未新增配置键**）· 测试 **41 项断言 ALL PASS**（policy / envelope / regression）· 🔴 **资源门禁**：门禁 OFF = **+48 B `PASS`**；门禁 ON（`-DGF_TELEMETRY_ENABLED=1`）= **+2,544 B > 剩余 636 B ⇒ `RESOURCE BLOCKED`** ⇒ **默认保持 OFF，请求人工裁决**（`H-WP4-FW-08`） |
| **`P3-WP4-Design-Review-Resolution.md`**（`docs/review/`） | **★★ DT:WP-4 设计审查裁决落实报告（`DOC-ONLY` · **R5** 冻结回填）** —— §1 **本轮（R5）裁决列表**（`H-WP4-14` **最终裁决 = 选择 B：新增专用 history ingest endpoint `POST /api/history/ingest`** —— **理由：不继承旧 `/api/ingest` 的 `mqtt_messages` side effect / 避免双写一致性问题 / 保持职责单一**；**已补端点定义 · EMQX Action 指向关系 · authentication 继承规则 · 与旧 endpoint 边界**；**R5 无新增编号**）· §2 修改文件列表 · §3 **`H-WP4` 状态变化表（R1→R2→R3→R4→R5：`CLOSED` 15 · `OPEN` 2 · 共 17）** · §4 **`F-WP4-1…12` 风险重新评估**（`CLOSED` 9 / 部分保留 1 / `OPEN` 1 / `CONFIRMED` 1）+ `R-WP4-1…11` · §5 **`INV-1…INV-14` 检查（14/14 满足 · 无新增字段/索引/schema/retention/cleanup/migration/DLQ/限流；端点属接口面定稿）** · §6 **冻结结论 = ✅ `FROZEN`（硬阻塞 0 · 人工批准）** · §7 边界与纪律核对（未改 firmware/Worker/EMQX/ACL · 未建 migration · 未解除 Gate · **未写新端点实现** · 未 commit/push） |
| **`P3-WP4-Firmware-Telemetry-Design-Freeze-Review.md`**（`docs/review/`） | **★★ `WP4-FW-TELEMETRY` 设计冻结评审（`DESIGN REVIEW ONLY` · 基线 `baeb177`）** —— §1 实现审查（`telemetry_policy.h` freestanding/纯函数 · `telemetry_envelope.h` 字段 · `telemetry_publisher.cpp` 门禁与 System State 读取 · `cloud_send_telemetry()` 独立 transport）· **发现 `F-WP4-FW-TR-1…5`**（🔴 `-TR-2` 多发 `device_id` · 🔴 `-TR-3` bool 编码自相矛盾）· §2 **Flash 逐 TU 归因**（增量 **全来自 `telemetry_publisher.cpp.o`**；`cloud_manager`/`main` 目标文件 OFF↔ON **逐字节相同**）· §3 **协议冻结建议**（`type` = **T1 点分命名空间** · `value` = **V1 number\|string** · `schema_version` 保留 · `seq` 建议 B）· §4 **`/up` 分流**（推荐 **A1 = EMQX Rule 层谓词**）· §5 `HUMAN_REQUIRED` · §6 推荐实施路径（GATE 1/2/3） · **顶部 = 裁决回填 banner** |
| **`P3-WP4C-EMQX-Rule-Design.md`**（`docs/review/`） | **★★ `H-WP4C-03` EMQX Rule telemetry 分流设计（🔒 `FROZEN DESIGN` **`R2`** · 唯一实施依据 · 基线 `def74f7`）** —— 🔄 **`R2`（`2026-10-12`）：新增 `D-7`（Serverless 数据集成配额不可调整：连接器 ≤2 / 规则 ≤4 / 动作 ≤4）与 §10 修订节 ⇒ 连接器形态改为「单连接器复用 + 动作级 `URL Path` 分流」；核心语义（方案 A / 冻结谓词 / `-S2` / Pages Proxy / 端点 / `V-1…V-7`）逐字不变** —— §0 七条不可协商约束（EMQX Rule 唯一分流责任方 · Worker 不解析 · firmware 不改 topic · 不改 `/up` · 不引入 Worker policy · 禁 MQTT v5 metadata · 禁新增 topic）· §1 问题（`/up` 承载命令结果 / 上线通知 / **telemetry v1** 三类，且**既有 `guo_feeder/#` 规则同样匹配**）· §2 **候选评估留痕**（A payload schema 判断 / B metadata，**因真机 `proto_ver=4` 排除** / C topic extension，**因 C-7 排除**）· **§3 🔒 冻结方案 = A（EMQX Rule Engine payload predicate routing）** + 冻结架构 + **§3.4 「分离优先」张力已消解**（替代原 `-S1`）· **§4 🔒 冻结谓词 = `(schema_version ∧ seq ∧ ts ∧ type ∧ value) ∧ ¬c ∧ ¬cmd`**（含"键存在"语义注记 · 禁止单字段判别）· **§5 🔒 冻结旧 Rule 策略 = `mqtt_messages rule = guo_feeder/# AND NOT telemetry_predicate`**（两步强制；`-S2` 已关闭）+ **§5.3 ★ 双投推论**· **§6 🔒 冻结平台限制 `D-1…D-6`** + 控制台人工实施方式（配置回写强制）· **§7 🔒 冻结验证计划 `V-1…V-7`（全 `NOT RUN`）** · **§8 风险 `R-WP4C-1…9`（本轮新增 `-7` Rule Engine 限制 / `-8` D1 action 限制 / `-9` 成本风险）** · §9 状态与边界（`H-WP4C-03 = CLOSED` · `CF-2` 仍 `BLOCKED`）· §10 关联文档 |
| **`P3-WP4C-Decisions.md`**（`docs/review/`） | **★★ `H-WP4C-03` EMQX Telemetry Routing 冻结裁决记录（`FROZEN INPUT` · 权威）** —— §1 裁决总览（D-1…D-6）· §2 **冻结分流方案 = A**（+ 冻结架构 + B/C 处置 + 「分离优先」张力消解 ⇒ 替代 `-S1`）· §3 **冻结判别条件**（`(schema_version ∧ seq ∧ ts ∧ type ∧ value) ∧ ¬c ∧ ¬cmd`；禁止仅判 `type`/`value`/`topic`；"键存在"语义注记）· §4 **冻结旧 Rule 策略 `-S2`**（`guo_feeder/# AND NOT telemetry_predicate`；两步强制 + 双投推论）· §5 **冻结 EMQX 平台限制 `D-1…D-6`** + 控制台人工实施方式 + **冻结验证计划 `V-1…V-7`** · §6 **新增风险 `R-WP4C-7`（Rule Engine 限制）/ `-8`（D1 action 限制）/ `-9`（成本风险）** · §7 状态（**`H-WP4C-03` `OPEN` → `CLOSED`**；`CF-2` 仍 `BLOCKED`）· §8 剩余 `HUMAN_REQUIRED`（`H-WP4C-04/05/06`）· §9 边界核对（**未创建任何规则 · 未改 EMQX/Worker/migration/src**）· §10 修改文件 · **🔄 第三阶段（`2026-10-12` · 实施前裁决补充）：§6B `Human Decisions`** —— **`H-WP4C-09` 拆分为 `-09-A`（EMQX Console Operator · 🔴 `OPEN`）/ `-09-B`（EMQX Rule Implementation · 🔴 `BLOCKED`）** + **`H-WP4C-10` / `-10-A`（Action 目标形态 · 🔴 `OPEN` · 代理不选择）** · **§7.3 实施门禁 Checklist（`WP4C-IMPL` 命名空间 · `PASS 3`（`C-1` 授权 / `C-2` Worker `99e6d937` / `C-3` D1 `0003`）· `BLOCKED 4`（`C-4` EMQX baseline / `C-5` Rule deployment / `C-6` `V-1…V-7` / `C-7` 存档回写））** · **`H-WP4C-04…08` 保持 `CLOSED` · `CF-2` 仍 `BLOCKED`** · **🔄 第四阶段（`2026-10-12`）：`H-WP4C-10-A` 人工裁决 = 方案 A（`Pages Proxy`）⇒ `H-WP4C-10`/`-10-A` `CLOSED` · `D-2` 确认 ⇒ `R-WP4C-12` 收敛 · 新增 `H-WP4C-11`（Pages 中转部署）· `C-5` 依赖改为 `C-4` + `H-WP4C-11`** · **`V-0` 导出工件见 EMQX 仓 `config/data-integration/P3-WP4C-V0-EMQX-Baseline.md`** · **🔄 第五阶段（`2026-10-12`）：`V-0` = 🟠 `PARTIAL`（连接器 2/2 · 规则 3/3 含 SQL 全文 · 动作 3/3 已取得；**缺** TLS 明细 / 完整请求头 / 高级设置 / D1 计数 / 回滚签署）⇒ `H-WP4C-09-A` = 🟠 `PARTIAL` · `C-4` = 🟠 `PARTIAL` · **新增 `R-WP4C-15`**（既有链路 ≈90% 失败 ⇒ 污染 `V-3` 判据）· **现场证实两个连接器 + 三个动作全部指向 `*.pages.dev`**（无 `workers.dev`） · **🔄 第六阶段（`V-0` 收口）：`V-0` = ✅ `CLOSED WITH GUARD` · `H-WP4C-09-A` = ✅ `CLOSED WITH GUARD` · `C-4` = ✅ `PASS`（带 guard）⇒ 门禁 `PASS 4 / BLOCKED 3`；新增 `R-WP4C-16`（TLS 明细保存后不可回读 ⇒ 保存前截图 + 保存后实测）；D1 基线 `mm=2458`；回滚已签署；唯一剩余前置 = `H-WP4C-11`（Pages 中转部署） · **🔄 第七阶段（`H-WP4C-11` 部署与验证）：`H-WP4C-11` = ✅ `CLOSED`（Pages 部署 `61b4938e-…`；回滚目标 `5790b3c5-…`）· 端到端 `PASS 15 / FAIL 0` · 准入 `V-1` = 🟢 `PASS`（带说明）· `H-WP4C-09-B` = 🟡 `READY`（依赖已全部闭合）· 纪律 §6A.4 三个 `STOP` 条件均不再成立 · 新增 `R-WP4C-17` · `…/log` 主题 0 行（`R-WP4C-15` 获新证据）· 旧连接器收存核查 = 已收到并存储（`mm=2458` 且仍在增长） · **🔄 第八阶段（`R2` 设计修订）：`D-7` = 配额不可调整（连接器 2/2 余量 0）⇒ 冻结设计升级 `R2`（§10）· `Implementation-Plan` §8 规格升版 `v2` · `H-WP4C-09-B` = 🟡 `READY (R2)` · 新增 `R-WP4C-18`/`-19` · 同批修正动作体引号缺陷** |
| **`P3-WP4C-Implementation-Plan.md`**（`docs/review/`） | **★★ `WP4C` Telemetry Routing 实施计划 + 当前状态快照（`PLAN ONLY` · **§8 规格 = `v2`（`R2`）** · 基线 `f31aabe`）** —— §8.0 **`v2` 规格声明（`R2`：连接器复用 + 动作级路径 + 请求体去引号；`v1` 的「新建独立连接器」与「`"payload":"${payload}"` 带引号」已作废）** · §8.1 / §8.1b / §8.1c / §8.2 · §1 授权与执行条件核对（`A-1…A-7`）· §2 **EMQX 快照**（**API 全 `403`** —— 规则/连接器/动作不可编程；`/clients` `200` 证明凭据有效 ⇒ **实施只能控制台人工**）· §3 **CF 快照**（★ **`POST /api/history/ingest` 未部署** —— 无 key 返 `200 OK` 而非 `401`，证明线上版本 **< `9b25203`**）· §4 **D1 快照**（`migrations list --remote` ⇒ **`0003` 待应用** ⇒ 远端无 `telemetry_history`）· §5 固件侧就绪（`CF-1` 已解除 / `CF-2` 代码在树但**默认关闭**）· §6 **阻塞发现 `X-1…X-7`（⇒ STOP 依据）**· §7 `HUMAN_REQUIRED`（`H-WP4C-04/05/06` 保持 `OPEN` + **新增 `H-WP4C-07` 控制台作业人 / `H-WP4C-08` Worker 部署授权**）· §8 **Console-ready 实施规格**（规则 SQL + Action 配置 + `-S2` 收窄两步，**未执行**）· §9 验证 `V-0…V-7` 与回滚 · §10 边界核对（**零违规 · 仅只读探测**） |
| **`P3-WP4C-Pre-Implementation-Checklist.md`**（`docs/review/`） | **★★ `WP4C` 实施前准备工件（`CHECKLIST ONLY` · 🛑 **STOP 边界保持** · 基线 `b8af006`）** —— §1 **实施检查清单 `G-1…G-5`（授权门 · **0/5**）/ `E-1…E-4`（环境门）/ `S-1…S-3`（顺序门）/ `C-1…C-7`（配置门）/ `A-1…A-3`（存档门）** · §2 **人工操作清单 `P-1…P-18`**（变更前基线 → `P-8` 新增规则 + `P-9` 收窄既有规则**同批** → 验证 + 回写）· §3 **实施前基线采集流程 `B-1…B-5`**（EMQX Rule SQL / Action / Connector **须人工控制台导出（API `403`）**；**Worker 生产版本 = `846384cd`（早于 `9b25203`）** · **D1 `0003` 待应用** —— 两者已只读实测）+ `B-6` API 可用性矩阵 · §4 **`HUMAN_REQUIRED` 登记 `H-WP4C-04…08`（全 `OPEN`）** · §5 Gate 状态 · §6 **进入实施阶段判定 = 🔴 `NOT READY`** · §7 边界核对（**零生产变更 · 仅只读**） |
| **`P3-WP4C-Implementation-Discipline.md`**（`docs/review/`） | **★★ `WP4C` 实施阶段纪律（`BINDING` · 约束后续所有生产变更）** —— §1 **冻结优先原则**（禁改：`H-WP4C-03` 冻结设计 / telemetry predicate / `/up` / firmware topic / Worker parsing 边界；发现不足 ⇒ `STOP` + 新 `HUMAN_REQUIRED`）· §2 **生产变更纪律**（变更前：配置基线 / 版本 / migration / 回滚信息；变更中：一次一步 + 逐步记录 + 失败即停；变更后：验证 + 回写存档 + 提交记录）· §3 **不可跳过步骤纪律**（固定顺序 `0003 → Worker → EMQX 基线 → 建 Rule → 收窄 Rule → V-1…V-7 → 存档`；**禁先建 Rule / 禁先切 Action / 禁无基线改 EMQX / 禁未验证宣布完成**）· §4 **回滚纪律**（6 类触发 + 优先级「停流 → 恢复旧 Rule → 恢复 Action → 分析」）· §5 四类报告要求 · §6 冲突处置 · **§6A `EMQX Console Boundary Rule`（`2026-10-12` 增补 · **控制台操作 = 生产人工变更** · 代理**硬禁**：猜 Rule SQL / 猜 Action 配置 / 以历史文档替代 `V-0` 导出 / 无法验证权限时绕过 API / 代填并宣称已导出 · **变更准入序列 `V-0…V-5`**（导出基线 → 确认 Action 可达 → 建新 Rule → 验证分流 → 收窄旧 Rule → 验证 command 不受影响）· **`STOP` 条款**：Action endpoint 未确认 / Rule baseline 未保存 / 回滚方案不存在 ⇒ 立即 `STOP`）** |
| **`P3-WP4C-Implementation-Record.md`**（`docs/review/`） | **★★ `WP4C` 实施记录（`PARTIAL` · **Step ①② 完成 · Step ③ ✅ 由人工完成（`V-0` `CLOSED WITH GUARD`）· Step ④ ✅ Pages 中转部署并验证**）** —— §1 执行摘要 · §2 **步骤 ① D1 `0003`（✅ `0003` applied · 8 commands / 1.27 ms · 2 表 + **恰好 5 索引** · 列对齐 `DT:WP-2` · `0001/0002` 未动）** · §3 **步骤 ② Worker 部署（✅ `846384cd` → **`99e6d937`** · `/api/history/ingest` 由 catch-all `200 OK` → **`401`**（路由上线）；既有路由未回归）** · §4 **步骤 ③ EMQX 基线导出 🛑 `STOPPED`（读 + 写 API 全 `403` · 无 CLI ⇒ 只能控制台人工）** · §5 新增 **`H-WP4C-09`（控制台人工执行 ③④⑤ · **已拆分 ⇒ `-09-A` `OPEN` / `-09-B` `BLOCKED`**）/ `H-WP4C-10`（Action 目标形态：Pages 中转 vs `*.workers.dev` · **⇒ `-10`/`-10-A` `CLOSED`（裁决 = 方案 A `Pages Proxy`）· 派生 `H-WP4C-11`（Pages 中转部署）`OPEN`**）** · §6 **`V-0` ✅ `CLOSED WITH GUARD` / 准入 `V-1` 🟢 `PASS`（带说明）/ 冻结 `V-1…V-7` 仍未跑** · §7 **风险 `R-WP4C-11`（部署带上 `392f181` Homie ACL）/ **`-12`（`D-2` 与存档矛盾 ⇒ **已收敛 `CLOSED`**）/ `-13`/`-14` · **`-15`（既有 `guo_feeder/#` 链路 ≈90% 失败 · 19k 触发 / 17k 失败 ⇒ **污染 `V-3` 判据**；本工作包不修 · **第七阶段新增证据：`…/log` 主题 0 行**）· **`-16`（连接器 TLS 明细保存后不可回读 ⇒ guard = 保存前截图 + 保存后实测；健康检查告警与真实失败混淆）· `-17`（幂等冲突仍消耗 `AUTOINCREMENT` ⇒ `id` 跳号；不影响行数/语义） · **`-18`（连接器复用 ⇒ 消息流与 history 强耦合）· `-19`（连接器编辑窗口 ≤60 s · 仅消息流）** · §8 Gate · §9 回滚（两步）· §10 边界核对（**零违规 · 未绕平台限制 · 第七阶段仅 1 次已授权 Pages 部署**） |
| **`P3-WP4C-Action-Target-Decision.md`**（`docs/review/`） | **★★ `H-WP4C-10` Action 目标形态裁决记录（✅ `DECIDED` · **裁决 = 方案 A（`Pages Proxy`）** · 基线 `0c8dc68`）** —— §1 问题（Action URL 形态未定：目标主机 + 完整端点 + **必须新建独立连接器**；**属环境事实问题，非设计问题**）· §2 **方案 A = `Pages Proxy`**（EMQX → Pages 中转 → Worker `/api/history/ingest`；优点：**与冻结设计一致**（`D-2`）· **隔离 `workers.dev`** · **便于后续认证控制**（唯一收口点）；前置：Pages 工程**尚无** `/api/history/ingest` 函数，须新增 + 部署（**另需授权**））· §3 **方案 B = `Worker Direct`**（风险：**与 `D-2` 冲突** · **无法确认 EMQX 网络可达性** · 失败形态引发 `D-4` 重试放大 · 无证据采用 = 改写冻结落地方式；**采用须先备 §3.3 三项证据**）· §4 **证据对照表**（存档 §1 的 workers.dev 根路径 URL **不可作证据**；Pages 中转先例（presence）**可**）· §5 **`H-WP4C-10-A` 裁决项（✅ `CLOSED` · **裁决 = 方案 A**）** · §5.2 **开工门 6 项（🔄 第七阶段：六项全绿）** · §6 **裁决前必答清单 6 项**（任一答否 ⇒ `STOP`）· §7 代理约束 · §8 影响面 · §9 边界核对（**本裁决阶段零生产变更 · 未部署代码 · 未创建 EMQX 配置**；后续由 `H-WP4C-11` 另批完成 1 次 Pages 部署） |
| **`P3-WP4C-Connector-Reuse-Amendment.md`**（`docs/review/`） | **★★★ `R2` 设计修订裁决记录（🔒 `DECIDED` · 成因 = `D-7` Serverless 配额不可调整 · 基线 `def74f7`）** —— §1 **配额实测**（连接器 **2/2 余量 0** · 规则 3/4 · 动作 3/4 · 单规则动作 1；官方文档交叉核对）· §2 为何是**设计修订**而非实施细节 · §3 **候选评估**（`R-A′` 采纳 / `R-B` 配额阻断 / `R-C` presence 复用（fallback）/ `R-D` Pages 分流（**违反冻结 `C-1`**）/ `R-E` 零窗口备选）· §4 **采纳方案 `R-A′`**（单连接器 + 动作级 `URL Path`；**base 取裸主机** —— 对"追加/替换"两种拼接语义均成立；配置变更 6 项 + 高级设置照抄 + 鉴权头落位）· §5 **施工顺序 5 阶段** + **唯一中断窗口量化**（仅消息流 · ≤60 s · 期望丢 0–1 条）+ **回滚** + 双投窗口说明 · §6 **★ 同批修正的规格缺陷**（规则 `SELECT` 输出与动作体字段不匹配 · **`"payload":"${payload}"` 带引号 ⇒ Worker `400 unparsable_payload`**；定案 = `SELECT *` + 不加引号）· §7 **`D-7`** · §8 **新增 `R-WP4C-18`（连接器复用耦合）/ `-19`（编辑窗口）** · §9 **配额核算（变更后规则 4/4 · 动作 4/4 用满）** · §10 边界与状态 · §11 关联文档 |
| **`P3-WP4C-History-Relay-Deployment-Verification.md`**（`docs/review/`） | **★★★ `H-WP4C-11` Pages 中转部署与端到端验证记录（✅ `CLOSED` · **本工作包首个生产变更** · 基线 `ef49db5`）** —— §1 执行摘要（7 步全绿）· §2 **部署事实**（Pages 新部署 `61b4938e-fa2e-42da-b950-f527781f79f9` · 回滚目标 `5790b3c5-…` · 仅 Functions bundle；中转函数形态表：逐字转发 / **`X-Webhook-Key` 必透传** / 上游不可达 `502` / 非 POST 健康串）· §3 **验证矩阵**（A 段 7/7 中转层 + 鉴权边界 · B 段 8/8 写入 · **`PASS 15 / FAIL 0`** · 可复现命令 `scripts/smoke-history-relay.mjs`）· §4 **D1 落库证据**（telemetry ×4 + log ×1 逐列；证 `value`=TEXT / `payload` 原样搬运 / `created_at` 由 D1 生成）+ **定点清理复核归零** · §5 **生产变更边界逐项核对** · §6 **★ 旧连接器「CF 是否收到并存储」核查**（`mqtt_messages` **2458** 行 · `/up` 1918 / `/down` 531 / **`/log` 0** · 31 topic / 61 device · 按日活跃度 · 生产设备实样 · presence 旁证 7d 114）· §7 **准入 `V-1` 判定 = 🟢 `PASS`（带说明）** · §8 下游影响（`-09-B` → `READY` · 新增 `R-WP4C-17`）· §9 修改文件 |
| **`EMQX_Assets/config/data-integration/P3-WP4C-V0-EMQX-Baseline.md`**（EMQX 仓） | **★★ `WP4C` 的 `V-0` 导出记录（✅ **`CLOSED WITH GUARD`** · 人工作业）** —— §1 **总量事实**（**连接器 2 · 规则 3 · 动作 3**）+ **3 条定性**（presence 规则**已存在** / `guo_feeder/#` 规则确认 = `r-n302933b-a01720` / **两个连接器与三个动作全部指向 `*.pages.dev`** ⇒ `D-2` 现场证实）· §2 只读操作步骤 `D-1…D-6` · §3 **导出记录**（**3 条规则 SQL 全文** + **3 条动作**（`POST` · **URL 路径全为空** · presence 用裸 `${body}`、消息流用三键模板 · **§3.2.2 高级设置：重试 `1` / 超时 `5s` / 健康检查 `30s` / 缓存 `4MB` / 异步**） + **2 条连接器**（含连接池/超时参数；★ **`c-…b12100` 无任何鉴权头**、`c-…291200` 带 `X-Webhook-Key`）+ **§3.4 D1 计数基线**（`th=0` / `lh=0` / **`mm=2458`**；近 24 h **13** / 近 7 d **191**） + **§3.5 与仓库记载的 8 条差异** + **§3.6 限知项 + 双 guard** + **§3.7 异常（既有链路 ≈90% 失败：resource down 告警 + D1 交叉印证 ⇒ `R-WP4C-15` / `R-WP4C-16`）** · §4 脱敏 · §5 判据 + **回滚方案（已签署）** · §6 下游接口 |
| **`EMQX_Assets/config/data-integration/P3-WP4C-V2V4-Config-After.md`**（EMQX 仓） | **★★ `WP4C` 变更后配置存档模板（🛑 **未变更** · **中转已就绪 ⇒ 可开工**）** —— §0 **前置事实（🔄 第七阶段追加：Pages 中转已部署并端到端验证）** · §1 变更元数据（**`V-2`+`V-4` 须同一窗口**）· §2 **`V-2` 新连接器 + 新规则 `guo_feeder_history_ingest`**（URL = Pages 域名）· §3 **`V-4` 收窄既有规则（改前/改后 SQL）** · §4 **`V-1…V-7` 结果 + 计数对照** · §5 回滚记录 · §6 **与 `V-0` 的差异清单（强制逐条）** |
| **`P3-WP4-Firmware-Telemetry-Decisions.md`**（`docs/review/`） | **★★ `WP4-FW-TELEMETRY` 冻结裁决记录（`FROZEN INPUT` · 权威）** —— §0 裁决总览 · §1 **Envelope v1 = 5 字段**（`ts`/`type`/`value`/`seq`/`schema_version`；**删 `device_id`（改用 MQTT topic identity）+ 删 `source`**）· §2 **`value` 三态编码**（number→JSON number · state→JSON string · **boolean→JSON boolean，禁 0/1**）· §3 **`seq` = boot lifetime 计数 · 不持久化 · 不复用 log seq** · §4 资源裁决（**A/B/E 授权，禁 C/D ArduinoJson·CBOR**）· §5 首版范围（**KEEP** temp/humid/weight；**DEFER** valve/wifi/online/time.valid）· §6 `/up` 路由（**topic 保持，分流责任 = EMQX Rule**）· §7 状态汇总 · §8 净影响 · §9 `HUMAN_REQUIRED` · §10 边界核对 · §11 关联文档 · **`## H-WP4-FW-08 Resource Budget Override Decision`**（人工批准超出 `P2-0` Flash ceiling；新政策 = **整体固件安全余量检查**；Guard G-1…G-4）· **`## H-WP4-FW-06 Telemetry Configuration Freeze`**（`mqtt.json.telemetry` **仅 `enable` + `interval_ms`**，默认 `false`/`300000`；**两道门**：`GF_TELEMETRY_ENABLED` 编译期 · `telemetry.enable` 运行时）· **⇒ `H-WP4-FW-05/06/07/08` 全部 `CLOSED`** |

## interfaces/ 接口 / 协议

> **接入新功能前必读。** 这是对外/对云契约的唯一权威来源。

| 文档 | 说明 |
|---|---|
| `cloud_protocol.md` | **Cloud Protocol V2.0** —— MQTT 连接参数、报文外壳、字段缩写、命令翻译、ACK、分片、错误码、**`log` Topic（已实现，§8.2）**、**Telemetry 遥测上行（v1 已冻结，§4.5）** |
| `workflow_cloud_interface.md` | Workflow 云端接口契约（Workflow / Variant / 云端同步命令） |
| `config_manager接口文档.md` | ConfigManager 对外接口（模块配置读写、版本、原子写） |
| `json_storage接口文档.md` | JsonStorage 对外接口 |
| `system_command接口文档.md` | SystemCommand 对外接口（restart 等系统级命令） |

## modules/ 模块设计说明

| 文档 | 对应源码 |
|---|---|
| `bin_storage开发说明0910.md` | `src/storage/bin_storage.*` / `file_storage.*` |
| `wifi开发笔记.md` | `src/services/wifi_module.*`（开发笔记与踩坑） |

> ⚠️ **2026-10-01 更正确认**：仓库根目录原有 4 个 `.d` 文件
> （`cloud_manager.d` / `command_manager.d` / `valve.d` / `workflow.d`）
> **不是设计文档**，而是 GCC 的**依赖文件**（首行形如 `cloud_manager.o: src\cloud_manager.cpp \`，
> 生成于 2026-08-03，还引用着分层前的旧路径）⇒ 已删除，并在 `.gitignore` 加入 `*.d`
> 防止再次误入库。**PlatformIO 的依赖文件正常位于 `.pio/build/<env>/`（已被忽略）。**

## specs/ 规范 / 模板 / 集成指南

| 文档 | 说明 |
|---|---|
| `critical_operation接入规范.md` | **Critical Operation 接入规范** —— 关键操作（Flash 写等）的统一接入要求 |
| `新增动作模板.md` | **新增 Workflow Action 的标准模板与编码规范** |
| `LogManager-Integration-Guide.md` | LogManager 集成指南 |
| `P2_Log_Integration_Matrix.md` | **LogManager P2 埋点覆盖权威矩阵**（§15 为最终覆盖状态） |

## issues/ 问题清单

| 文档 | 说明 |
|---|---|
| `未修复的问题.md` | **全项目问题权威清单** —— `LOG-n` / `VALVE-n` / `R-n` / `T-n` / `EVT-n` / `LV-n` / `P0-n` / `DSP-n` / `PROTO-n` / `OLED-n` |

## scripts/ 脚本

| 文件 | 说明 |
|---|---|
| `setup-cloud-app-env.ps1` | 云端 / 安卓端本地工具链部署脚本（**默认 dry-run**，加 `-Apply` 执行）。⚠️ 必须保持纯 ASCII（PowerShell 5.1 按 ANSI 解码无 BOM 的 `.ps1`，中文注释会直接 `ParserError`） |

---

## archive/ 历史归档

> **仅作史料留存，不代表当前实现。** 排查问题时如果发现文档与代码不一致，**以代码为准**。

| 子目录 | 内容 | 数量 |
|---|---|---|
| `archive/log/` | LogManager 开发全过程：审计报告、详细设计规划、P1.1–P1.5 / P2F–P2N 各阶段报告与审查、Phase4/Phase5 边界审查、R7/R8 性能分析、板级测试报告 | 37 |
| `archive/workflow/` | Workflow 改造历程：需求文档、架构适配 CommandManager 报告、storage 改造进度、cloud_sync 进度与测试报告、critical_operation 接入与自查 | 17 |
| `archive/legacy/` | 其他一次性文档：`AI_TASK.md`（TimeManager V2 需求，已完成）、`computer_reset接入报告.md`、`config_version与workflow初始bin-进度.md`、`bin_storage开发需求.md`、`整理方案-文档与源码目录重构.md`（2026-10-01 整理方案）、`readme-旧版云端协议速查.md`（2026-10-02 自 readme 移出的重复章节） | 6 |

> **★ 历史路径映射（2026-10-02 补）**：归位时**未改写归档文档内部**的相互引用（史料保持原样），
> 因此在 `archive/` 里读到旧路径属正常，按下表换算：

| 文档内写的旧路径 | 现在的真实位置 |
|---|---|
| `log模块历史/<file>.md` | `docs/archive/log/<file>.md` |
| `workflow修改历史需求/<file>.md` | `docs/archive/workflow/<file>.md` |
| `docs/LogManager-P2-Progress.md` | `docs/archive/log/LogManager-P2-Progress.md` |
| `docs/LogManager-Integration-Guide.md` | `docs/specs/LogManager-Integration-Guide.md` |
| `docs/P2_Log_Integration_Matrix.md` | `docs/specs/P2_Log_Integration_Matrix.md` |

> **现行文档**（`requirements/` `architecture/` `interfaces/` `modules/` `specs/` `issues/`）中的同类引用
> 已于 **2026-10-02 全部更正为真实路径**（42 处）；只有 `archive/` 内部保留旧写法。

---

## ⚠️ 已知待办：文档中的明文凭据

以下**仍被 git 跟踪**的文档里含真实 MQTT 凭据（broker 域名 / 用户名 / 密码），已列入待办：

| 文件 | 处理建议 |
|---|---|
| `interfaces/cloud_protocol.md` | 替换为占位符 |
| `archive/log/LogManager-P1.5-Board-Test-Report0918.md` | 替换为占位符 |

> `data/config/mqtt.json` 与 `AI_RULES.md` 已通过 `.gitignore` 移出版本控制。
> **注意**：凭据已在 git **历史**中，`gitignore` 无法清除历史 —— 彻底修复需**轮换 MQTT 密码**，
> 或重写 git 历史。计划在 P0/P1「Topic V3 + 一机一密」改造时一并轮换
> （见 `architecture/Cloud-APP-Platform-Plan.md`）。

---

## 维护约定

1. **新增模块说明 / 接口文档** → 放入对应分类目录，并回来更新本索引。
2. **阶段报告 / 进度台账 / 一次性需求** → 完成后移入 `archive/<模块>/`，**不要留在根目录或分类目录**。
3. **接口 / 协议变更** → 必须同步更新 `interfaces/` 下的权威文档。
4. 根目录只保留：构建配置、AI 工程文档（`readme.md` / `AGENTS.md` / `AI_CONTEXT.md` / `AI_RULES.md`）
   与**会话交接文档 `HANDOFF.md`**。
5. **`readme.md` 只做「总说明」**：协议细节、示例、错误码表一律放 `interfaces/`，
   避免同一内容两处维护后产生分歧（2026-10-02 已据此移出 1251 行重复内容）。
