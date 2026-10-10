# P3 · DT:WP-4 Design Review Resolution（设计审查裁决落实报告 · **DOC-ONLY** · **R5**）

> ## 📋 报告定位
>
> - **本文件 = `DT:WP-4`（History Ingestion & Cloud Pipeline Design）设计审查的「裁决落实报告」** —— 记录 **人工裁决 → 设计修订 → 一致性复核 → 收口** 的完整闭环。
> - **性质**：**`DOC-ONLY`（不含代码 / 不含 migration / 不含部署）**；**不冻结设计、不解除任何门禁**。
> - **状态**：**`DT:WP-4 = FROZEN` · R5 冻结回填（15 `CLOSED` / 2 `OPEN`）（设计已冻结 · 实施唯一基线 · 人工批准）** · **`P3 = NOT STARTED / PLANNED`**
> - **基线**：固件仓 HEAD **`fee6b77`**（`DT:WP-3` 冻结提交 · 工作树 clean）
> - **审查对象**：`docs/architecture/P3-DT-WP4-History-Ingestion-Design.md`（R1 → R2 → R3 → R4 → **R5**）
> - **配套文件**：`docs/architecture/P3-DT-WP4-History-Ingestion-Review.md`（R5 自检审核）
>
> **★ 引用规范**：`DT:` = 设计轨 · `计划:` = 计划轨 · **禁裸 `WP-n`**；`WP1:` / `WP2:` / `WP3:` = 三份冻结文档章节；`计划:§n` = `P3-History-D1-Plan.md` 章节。
>
> **★ 轮次**：**R2（2026-10-10）** 关闭 6 项 · **R3（2026-10-11）** 关闭 4 项 · **R4（2026-10-11）** 关闭 4 项 · **R5（2026-10-11）** 关闭 1 项 —— 本轮（R5）**仅落实人工裁决、不重新解释**。

---

## §1 本轮裁决列表（**R5 · 人工裁决 · 1 项 → `CLOSED`**）

> **★ 来源**：人工裁决指令（2026-10-11）· **本报告仅落实，不重新解释**。

### 1.1 `H-WP4-14` —— 端点复用 vs 新增（**最终裁决 = 选择 B**）

| 项 | 内容 |
|---|---|
| **裁决** | **选择 B = 新增 history ingest endpoint（新增专用端点）** |
| **理由（裁决原文）** | ① **`DT:WP-4` history ingestion 不继承旧 `/api/ingest` 的 `mqtt_messages` side effect** · ② **避免双写一致性问题** · ③ **保持职责单一** |
| **端点定义** | **`POST /api/history/ingest`** —— 请求体 = Design §2.4 / §2.5 信封（`topic` / `clientid` / `payload`；log 域另含 `payload_base64` + `device_id` / `boot_seq` / `seq`）；响应 = §6.4 三态统计 `{accepted, duplicate, rejected}` |
| **EMQX Action 指向关系** | EMQX 规则 SQL 的 **HTTP Action 指向新端点 `POST /api/history/ingest`**（**不再指向旧 `/api/ingest`**）—— ⚠️ **该指向变更属实施期 EMQX 配置动作**（`计划:WP-3`）；**本报告 / 设计文件均不改 EMQX 配置** |
| **authentication 继承规则** | ① **`X-Webhook-Key` 强制**（继承 `/api/ingest` 现状语义 · 不放宽）· ② **`source=cloud_api` 拒收**（继承现状守卫）· ③ **`/api/admin/**` 仍走 `ADMIN_API_TOKEN`**（不变）· ④ **不引入第二因子**（`H-WP4-15`） |
| **与旧 endpoint 边界** | **旧 `POST /api/ingest` 保持现状（不改）** —— 含其既有 `mqtt_messages` 写入副作用与既有调用方；**新旧端点互不依赖**；**新端点不写 `mqtt_messages`**；`deviceIdFromTopic()` 身份解析**语义继承**（实施期可共享 helper · 属实施细节） |
| **落实位置** | Design **§9.1**（整节改写：**R5 裁决块**（6 维表）+ **A/B 对比留档（标注已采纳 B）**）· **§2.1**（链路图 `/api/ingest` → `/api/history/ingest` + **R5 更新说明**）· **§2.4**（通用信封 `source` 行 → **新端点拒 `cloud_api`**；承载段 → **「history ingest 端点」**）· **§1.2 `U-4`**（Action 指向差异登记）· **§10.0-R5**（新增留痕）· **§10.1**（收窄为 2 项）· **§10.5**（该行标 `CLOSED`）· **§10.6**（新增实施前置行）· **§11**（R5 检查结论）· **§12**（现状 / 计划对齐行）· **§13 / §13.1** · 页脚 |
| **状态** | **`CLOSED`**（**派生**：`F-WP4-10` 随之 **`CLOSED`**） |

### 1.2 保持 `OPEN` 的其余 2 项（**明确界外 · 可延后**）

| ID | 内容 | 现状 |
|---|---|---|
| `H-WP4-10` | `ts` 合理性窗口 | **`OPEN`**（**明确界外** · 未来策略） |
| `H-WP4-12` | `log_ack` 归属 | **`OPEN`**（**明确界外** · 属 `计划:WP-5` 且受 `G-4`） |

> **★ R5 无新增编号** —— 本轮未发现新的派生缺口；`H-WP4` 总数保持 **17**。
> **★ 「实施前必裁」项清零** —— **`H-WP4-14`（端点）为原唯一「实施前必裁」项，已由本轮裁决关闭**；**剩余 2 项均明确界外、可延后**。
> **★ B 方案的实施前置（透明登记 · 不属本设计范围）**：**新端点实现** · **EMQX HTTP Action 指向变更** · **`deviceIdFromTopic()` helper 复用** —— 均属**实施阶段动作**（`计划:WP-3` / `计划:WP-4`）。

---

## §2 修改文件列表

| 文件 | 类型 | 变更 | 行数 |
|---|---|---|---|
| `docs/architecture/P3-DT-WP4-History-Ingestion-Design.md` | **修改**（R4 → R5） | 头部状态块（+ **FROZEN** 英文声明 + R5 修订标记）+ §0 + §1.2 + §2.1 / §2.4 + **§9.1（整节改写）** + §10（§10.0-R5 新增 · §10.1 / §10.2 / §10.4 / §10.5 / §10.6）+ §11 / §12 / §13 / §13.1 / 页脚 | 858 → **892** |
| `docs/architecture/P3-DT-WP4-History-Ingestion-Review.md` | **修改**（R4 → R5） | 整篇重写为 R5（**八项必查** / 一致性 / 边界 / F / R / H 汇总 / 结论 / 端点决策自洽核对） | 226 → **本版** |
| `docs/review/P3-WP4-Design-Review-Resolution.md` | **修改**（R4 → R5） | 本报告 | 274 → **本版** |
| `docs/README.md` | **修改** | **仅索引 / 状态同步**（状态行 + 3 行索引） | 197 → **197** |

> **★ 本报告不含任何其他改动**；**无新增代码文件、无 migration、无配置变更、无新增 topic / ACL**；⚠️ **新端点 `POST /api/history/ingest` 仅为设计定名**（**未写 Worker 代码 · 未改 EMQX Action**）。

---

## §3 `H-WP4` 状态变化表（**R1 → R2 → R3 → R4 → R5**）

| ID | R1 | R2 | R3 | R4 | **R5** | 变化 |
|---|---|---|---|---|---|---|
| `H-WP4-01` | `OPEN` | `OPEN` | **`CLOSED`** | `CLOSED` | `CLOSED` | —（R3） |
| `H-WP4-02` | `OPEN` | **`CLOSED`** | `CLOSED` | `CLOSED` | `CLOSED` | —（R2） |
| `H-WP4-03` | `OPEN` | **`CLOSED`** | `CLOSED` | `CLOSED` | `CLOSED` | —（R2） |
| `H-WP4-04` | `OPEN` | `OPEN` | `OPEN` | **`CLOSED`** | `CLOSED` | —（R4） |
| `H-WP4-05` | `OPEN` | **`CLOSED`** | `CLOSED` | `CLOSED` | `CLOSED` | —（R2） |
| `H-WP4-06` | `OPEN` | **`CLOSED`** | `CLOSED` | `CLOSED` | `CLOSED` | —（R2） |
| `H-WP4-07` | `OPEN` | **`CLOSED`** | `CLOSED` | `CLOSED` | `CLOSED` | —（R2） |
| `H-WP4-08` | `OPEN` | `OPEN` | `OPEN` | **`CLOSED`** | `CLOSED` | —（R4） |
| `H-WP4-09` | `OPEN` | `OPEN` | `OPEN` | **`CLOSED`** | `CLOSED` | —（R4） |
| `H-WP4-10` | `OPEN` | `OPEN` | `OPEN` | `OPEN` | **`OPEN`** | —（界外 · 可延后） |
| `H-WP4-11` | `OPEN` | **`CLOSED`** | `CLOSED` | `CLOSED` | `CLOSED` | —（R2） |
| `H-WP4-12` | `OPEN` | `OPEN` | `OPEN` | `OPEN` | **`OPEN`** | —（界外 · `G-4` · 可延后） |
| `H-WP4-13` | `OPEN` | `OPEN` | **`CLOSED`** | `CLOSED` | `CLOSED` | —（R3） |
| **`H-WP4-14`** | `OPEN` | `OPEN` | `OPEN` | `OPEN` | **`CLOSED`** | ✅ **R5 关闭**（**选择 B：新增专用端点 `POST /api/history/ingest`** · 不写 `mqtt_messages` · 职责单一） |
| `H-WP4-15` | `OPEN` | `OPEN` | `OPEN` | **`CLOSED`** | `CLOSED` | —（R4） |
| **`H-WP4-16`** | —（不存在） | `OPEN`（新增） | **`CLOSED`** | `CLOSED` | `CLOSED` | —（R3） |
| **`H-WP4-17`** | —（不存在） | `OPEN`（新增） | **`CLOSED`** | `CLOSED` | `CLOSED` | —（R3） |

**汇总**：**`CLOSED` 15 · `OPEN` 2 · 合计 17**（R1 = 15 全 `OPEN`；R2 = 6 `CLOSED`；R3 = 10 `CLOSED`；R4 = 14 `CLOSED`；**R5 = 15 `CLOSED` / 2 `OPEN`**）。

---

## §4 `F-WP4` 风险重新评估

| ID | 发现 | R1 | R2 | R3 | R4 | **R5** | 消解依据 / 现状 |
|---|---|---|---|---|---|---|---|
| `F-WP4-1` | heartbeat 结构性空档 | 🟠 | `OPEN` | `OPEN` | ✅ `CLOSED` | `CLOSED` | R4：`H-WP4-09` |
| `F-WP4-2` | `log_ack` 缺失（`BT-1`） | 🔴 | `OPEN` | `OPEN` | `OPEN` | **`OPEN`** | `H-WP4-12` · `G-4` · **界外可延后** |
| `F-WP4-3` | 入口 topic 三套口径 | 🔴 | `OPEN` | ✅ `CLOSED` | `CLOSED` | `CLOSED` | R3：`H-WP4-01` / `13` |
| `F-WP4-4` | CBOR vs JSON 信封 | 🔴 | ✅ `CLOSED` | `CLOSED` | `CLOSED` | `CLOSED` | `H-WP4-05` |
| `F-WP4-5` | `ts` / `seq` 无来源 | 🔴 | ✅ `CLOSED` | `CLOSED` | `CLOSED` | `CLOSED` | `H-WP4-02` + `03` |
| `F-WP4-6` | `OR IGNORE` 吞 NOT NULL | 🟠 | ✅ `CLOSED`（缓解） | `CLOSED` | `CLOSED` | `CLOSED` | `H-WP4-11` + `W-4` |
| `F-WP4-7` | REVOKED 返 200 | 🟢 | `CONFIRMED` | `CONFIRMED` | `CONFIRMED` | `CONFIRMED` | 非缺陷 |
| `F-WP4-8` | 解码失败两难 | 🔴 | ✅ `CLOSED` | `CLOSED` | `CLOSED` | `CLOSED` | `H-WP4-05` |
| `F-WP4-9` | `payload` 无**业务**上限 | 🔴 | ⚠️ 部分保留 | ⚠️ 部分保留 | ⚠️ 部分保留 | ⚠️ **部分保留** | **Worker 侧已定**；业务上限承 `H-WP3-06` |
| **`F-WP4-10`** | `/api/ingest` 复用写 `mqtt_messages` | 🟠 | `OPEN` | `OPEN` | `OPEN` | ✅ **`CLOSED`（R5 裁决）** | **R5：`H-WP4-14` = 选择 B（新专用端点）⇒ 不写 `mqtt_messages` · 职责单一 · 鉴权语义自洽** |
| `F-WP4-11` | `payload` 落库 ↔ N-3 脱敏 | 🟠 | ⚠️ 升级 🔴 | ✅ `CLOSED`（转移） | `CLOSED` | `CLOSED` | R3：`H-WP4-17` |
| `F-WP4-12` | 不解码 CBOR ↔ 逐条幂等键 | — | `OPEN` | ✅ `CLOSED` | `CLOSED` | `CLOSED` | R3：`H-WP4-16` |

**统计**：**`CLOSED` 9 · 部分保留 1 · `OPEN` 1 · `CONFIRMED` 1**（共 12）。

**风险（`R-WP4-*`）变化**：

| ID | R1 → R2 | R3 | R4 | **R5** |
|---|---|---|---|---|
| `R-WP4-1` 幂等键不可构造 | 🔴 → 🟡 | 🟡 | 🟡 | 🟡（不变） |
| `R-WP4-2` 入口 topic 未统一 | 🔴 → 🔴 | ✅ → 🟢 | 🟢 | 🟢（不变 · 端点定稿后彻底收敛） |
| `R-WP4-3` `payload` 无上限 | 🔴 → 🔴（上升） | → 🟡 | 🟡 | 🟡（不变） |
| `R-WP4-4` 敏感值明文落库 | 🟠 → 🔴 | → 🟡（转移） | 🟡 | 🟡（不变） |
| `R-WP4-5` 重投风暴 / 静默丢批 | 🟠 → 🟢 | 🟢 | 🟢 | 🟢（不变） |
| `R-WP4-6` 静默丢数据 | 🟠 → 🟡 | 🟡 | 🟡 | 🟡（不变） |
| `R-WP4-7` 写放大 | 🟡 | 🟡 | 🟡 | 🟡（不变） |
| `R-WP4-8` 越权写入 | 🟡 | → 🟢 | 🟢 | 🟢（不变） |
| `R-WP4-9` log 逐条幂等不可实现 | — → 🔴 | ✅ → 🟢 | 🟢 | 🟢（不变） |
| `R-WP4-10` 重试耗尽后数据丢失（无 DLQ） | — | — | 🟡（已知并接受 · `H-WP4-08`） | 🟡（不变） |
| `R-WP4-11` 共享 key 无第二因子 | — | — | 🟡（已知并接受 · `H-WP4-15`） | 🟡（不变） |
| **`R-WP4-12`** 新增路由 + EMQX Action 指向变更的实施面 | — | — | — | 🟢 **新增（低 · 已登记 · 端点仅设计定名；旧端点不动 ⇒ 无破坏性变更）** |

---

## §5 `INV-1 ~ INV-14` 检查结果

| ID | 不变量 | 检查结果 |
|---|---|---|
| **INV-1** | telemetry policy 唯一权威位置 = 设备侧；Worker 只 ingest / validation / idempotency / persist | ✅ **满足** —— `§5.1.1` 显式固化边界；**R5：新端点不扩大 Worker 职责（仍 4 项）** |
| **INV-2** | heartbeat 不进入历史库 | ✅ **满足** —— Worker 不做 heartbeat 判断（禁止项②）⇒ 由上游保证 |
| **INV-3** | system health 正常不落库 | ✅ **满足** —— 未涉及 |
| **INV-4** | online 仅记录迁移点 | ✅ **满足** —— 未涉及 |
| **INV-5** | `weight.enable=false` ⇒ 无 weight 数据 | ✅ **满足** —— Worker 不感知该开关 |
| **INV-6** | telemetry 与 log 物理分离 | ✅ **满足** —— 两域链路仍独立（§3 / §4） |
| **INV-7** | 设备 Flash Log 级别 ≠ 云端级别 | ✅ **满足** —— §4.2 只设计云端侧 |
| **INV-8** | **幂等键冻结** | ✅ **满足** —— **未引入第二套键**；**R5：端点变更不影响幂等键定义** |
| **INV-9** | **schema / 字段 / 索引集合不得改** | ✅ **满足** —— **未新增字段 · 索引 · 类型**（`payload` 仍 `TEXT`）；**新端点属接口面，不改表结构** |
| **INV-10** | 时间单位 = Unix 秒 | ✅ **满足** —— 单位固定；Worker 不换算、不判断窗口 |
| **INV-11** | retention 90 / 180 天；cleanup = scheduled job | ✅ **满足** —— **未改**；**新端点不涉及 cleanup 边界** |
| **INV-12** | ingest 与 cleanup 完全解耦 | ✅ **满足** —— §7.5 未改；**新端点仍不介入 cleanup** |
| **INV-13** | `0001_*` / `0002_*` 不得编辑；本阶段不新增 `0003_*` | ✅ **满足** —— **未创建任何 migration** |
| **INV-14** | Worker 不得成为第三套 telemetry policy | ✅ **满足** —— **`§5.1.1` 明令禁 telemetry policy / heartbeat 判断 / 业务价值判断 / 降频 / 有效性裁决**；**R5：去掉副带写入 ≠ 增加能力** |

**结论**：

| 检查维度 | 结论 |
|---|---|
| **无新增字段** | ✅ 是 |
| **无新增索引** | ✅ 是 |
| **无修改 schema** | ✅ 是（`payload` 物理类型仍 `TEXT`） |
| **无改变 retention** | ✅ 是（90 / 180 未动） |
| **无改变 cleanup 边界** | ✅ 是（`scheduled` 未动） |
| **无新增 topic / ACL / migration** | ✅ 是 |
| **端点（接口面）** | ⚠️ **设计定名新端点 `POST /api/history/ingest`** —— **属接口面决策**，**非 schema / 索引 / 保留策略变更**；**未写 Worker 代码 · 未改 EMQX Action** |
| **无新增 DLQ / 限流 / 第二套 policy** | ✅ 是 |
| **`INV-1…INV-14`** | ✅ **14/14 满足** |
| **跨冻结冲突** | ✅ **已清零**（R3 收敛）· **R4 / R5 未新增** |
| **两表隔离 / ingest-cleanup 解耦 / Worker 非第三套 policy** | ✅ **三项均保持** |

---

## §6 当前是否达到 FROZEN

### 6.1 判据

| 判据 | 结果 |
|---|---|
| **设计链路完整**（入口 → 校验 → 幂等 → 写入 → 错误处置） | ✅ **完整** |
| **入口 topic / 命名口径** | ✅ **已定**（`H-WP4-01` / `13`） |
| **入口端点** | ✅ **已定（R5 选择 B）**（`POST /api/history/ingest` · `H-WP4-14`） |
| **EMQX Action 指向关系** | ✅ **已定**（指向新端点；实施期配置动作） |
| **authentication 继承规则 / 与旧 endpoint 边界** | ✅ **已定** |
| **log 幂等可构造性** | ✅ **已定**（`H-WP4-16`） |
| **`payload` 跨冻结冲突** | ✅ **已收敛**（`H-WP4-17`） |
| **`schema_version` 前向兼容** | ✅ **已定**（`H-WP4-04`） |
| **失败模型 / DLQ 边界** | ✅ **已定**（`H-WP4-08`） |
| **Worker 校验边界** | ✅ **已显式固化**（`§5.1.1` · `H-WP4-09`） |
| **安全防护层** | ✅ **已定**（`H-WP4-15`） |
| **裁决覆盖率** | ✅ **15 / 17 ≈ 88% `CLOSED`** ⇒ **硬阻塞 0 · 「实施前必裁」0** |
| **残余 `OPEN`** | **2 项** —— `10` / `12`（**均界外 · 可延后**） |
| **实施门禁** | ❌ **`G-1` = `HUMAN_REQUIRED`**（未批准）· LogHistory 另受 **`G-4` = `BLOCKED`** |

### 6.2 硬阻塞项（**R5 后 = 0**）

| # | 项 | R5 结论 |
|---|---|---|
| 1 | ~~`H-WP4-01` / `13`（入口 topic + 命名）~~ | ✅ **R3 已关闭** |
| 2 | ~~`H-WP4-16`（log 键字段透出）~~ | ✅ **R3 已关闭** |
| 3 | ~~`H-WP4-17`（`payload` 形态）~~ | ✅ **R3 已关闭** |
| 4 | ~~`H-WP4-04`（`schema_version` > 1）~~ | ✅ **R4 已关闭** |
| 5 | ~~`H-WP4-09`（校验边界不明确）~~ | ✅ **R4 已关闭** |
| 6 | ~~`H-WP4-14`（端点 A/B · **「实施前必裁」**）~~ | ✅ **R5 已关闭**（**选择 B**：新专用端点 `POST /api/history/ingest`） |
| 7 | `H-WP3-06`（`payload` **业务**上限） | ⚠️ **Worker 侧解已定**（平台上限 + 拒收）；**业务上限仍归 `DT:WP-3`** ⇒ **不阻塞链路施工** |

### 6.3 ★ 结论

> ## ✅ **`FROZEN`（设计已冻结 · 实施唯一基线 · 人工批准）**
>
> **已达状态**：
> - ✅ **R5 裁决（`H-WP4-14` = 选择 B）已落实到设计稿**（端点定义 / EMQX Action 指向 / authentication 继承 / 旧端点边界 **四要素齐备**）
> - ✅ **`INV-1…INV-14` 全部满足**（**无新增字段 / 索引 / schema / retention / cleanup / migration / DLQ / 限流**；**新端点属接口面**）
> - ✅ **未触碰任何冻结约束 / 未解除任何 Gate / 零实现副作用**
> - ✅ **硬阻塞清零 · 「实施前必裁」清零 · 收敛项 `F-WP4-10` 已按裁决关闭**
>
> **但**：
> - ⚠️ **冻结 = 设计冻结 —— ≠ "实施已完成" · ≠ "实施已开始"**；
> - ⚠️ **实施仍受 `G-1`**（LogHistory 另受 `G-4`）；
> - ⚠️ **新端点的实现 + EMQX Action 指向变更属实施期动作**（**不在本设计范围**）。

### 6.4 下一步建议（**供人工参考，非裁决**）

| 优先级 | 动作 |
|---|---|
| **P0** | **设计已冻结（`FROZEN`）⇒ 进入实施**：**新端点 `POST /api/history/ingest` 实现 + EMQX Action 指向变更**（属 `计划:WP-3` / `计划:WP-4`） |
| **P1** | 评估 **`G-1`**（实施入口门禁）；⚠️ **LogHistory 落库另受 `G-4`（`BLOCKED`）** |
| **P2** | `H-WP4-10` / `12`（**明确界外**）→ **可延后至未来阶段**（`log_ack` 属 `计划:WP-5` 且受 `G-4`） |
| **跨 WP** | `DT:WP-3` **`H-WP3-06`**（`payload` 业务上限）；**设备侧「禁止上传含 secret 日志」义务须在固件侧落地** |
| **未来版本（已登记）** | **DLQ**（`R-WP4-10`）· **限流 / IP 限制 / key rotation**（`R-WP4-11`） |

---

## §7 边界与纪律核对

| 检查项 | 结果 |
|---|---|
| **修改固件代码（`src/`）** | ❌ **未修改** |
| **修改 Worker 实现** | ❌ **未修改**（**新端点仅设计定名，未写任何 Worker 代码**） |
| **修改 EMQX 配置 / ACL / topic / Action** | ❌ **未修改**（**未新增 topic、未新增订阅、未改 ACL、未改 Action 指向**） |
| **创建 migration（`0003_*`）** | ❌ **未创建** |
| **修改冻结文档**（`DT:WP-1` / `DT:WP-2` / `DT:WP-3` / `P2-0-…freeze.md` / `cloud_protocol.md` / `P3-History-D1-Plan.md`） | ❌ **未修改**（**逐字未动**） |
| **提前解除 Gate** | ❌ **未解除**（`G-1…G-5` 逐项未变） |
| **`H-WP4-14` 处理** | ✅ **依人工裁决关闭**（**未自行裁决** —— 裁决与理由均来自人工指令，本报告仅落实） |
| **`git commit` / `git push`** | ⚠️ **随 R5 冻结提交入库**（`docs(p3-wp4): freeze history ingestion design r5`）· **未 push**（按指示） |
| **本报告权限** | **仅 `docs/` 内修订**（Design / Review / 本报告 / README 索引） |

---

> **DT:WP-4 = FROZEN · R5 冻结回填（15 `CLOSED` / 2 `OPEN`）· 设计已冻结 · 实施唯一基线 · 人工批准.**
> **R5 decision: H-WP4-14 = option B — a dedicated history ingest endpoint `POST /api/history/ingest`. The legacy `POST /api/ingest` is unchanged. Design-only — no Worker / EMQX change.**
> **No Gate changed. No implementation. No Worker code. No migration. No DDL. No deploy. No new topic. No ACL change. No DLQ. No rate limiting. No commit. No push.**

---

*本文件于 **2026-10-10** 建立（R2），经 **R3 / R4 / R5** 修订，为 **`DT:WP-4` 设计审查裁决落实报告（`DOC-ONLY`）**；不含任何实现、迁移或代码改动。*
*基线 = 固件仓 HEAD `fee6b77`（`DT:WP-3` 冻结提交 · 工作树 clean）。*
