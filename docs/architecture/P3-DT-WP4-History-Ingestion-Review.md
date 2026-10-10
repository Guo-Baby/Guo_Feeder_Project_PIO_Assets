# P3 · DT:WP-4 —— History Ingestion & Cloud Pipeline **Review**（自检审核报告 · `REVIEW ONLY` · **R5**）

> ## 🟢 `REVIEW ONLY`（**设计已冻结 · 实施唯一基线 · 不解除任何门禁 · 不含实现**）
>
> - 本文件 = 对 **`P3-DT-WP4-History-Ingestion-Design.md`**（**R5**）的**自检审核报告**；**不含代码**、**不创建 migration**、**不改 D1 / Worker / EMQX / `src/`**。
> - **审核性质**：**设计一致性自检 + 冲突登记核查 + 边界核对 + R5 裁决落实核对 + 收口完备性核对** —— **不作裁决**（裁决权归人工，见 Design §10）。
> - **★ 声明**：
>   > **WP-4 Review only means design self-check completed.**
>   > **It does not mean the design is approved, frozen, or implemented.**
> - **状态**：**`DT:WP-4 = FROZEN` · R5 冻结回填（15 `CLOSED` / 2 `OPEN`）（**设计已冻结 · 实施唯一基线 · 人工批准**）** · **`P3 = NOT STARTED / PLANNED`**
> - **基线**：固件仓 HEAD **`fee6b77`**（`DT:WP-3` 冻结提交 · 工作树 clean）· 审核对象 = `P3-DT-WP4-History-Ingestion-Design.md`（R1 → R2 → R3 → R4 → **R5**）
> - **★ R5 修订标记**：本轮审核**仅复核 R5 的裁决落实**（`H-WP4-14` → `CLOSED` · **选择 B = 新增专用 history ingest endpoint `POST /api/history/ingest`**）**与由此引发的状态收敛**（`F-WP4-10` → `CLOSED`；状态升为 `FREEZE CANDIDATE` → **`FROZEN`（人工批准）**）；**未改任何冻结文档、未改任何 Gate、未实现任何代码**。
>
> **★ 引用规范**：同 Design §1.1（`DT:` = 设计轨 · `计划:` = 计划轨 · **禁裸 `WP-n`**）；`WP1:` / `WP2:` / `WP3:` = 三份冻结文档章节。
>
> **★ 关联文件**：**`docs/review/P3-WP4-Design-Review-Resolution.md`** = 裁决回填的**正式审查报告**（裁决列表 / 状态变化表 / INV 检查 / 冻结结论）—— 与本文件**互为补充**。

---

## §0 自检摘要（**八项必查 · R5**）

| # | 自检项 | 结论 | 证据 |
|---|---|---|---|
| **1** | **是否违反 `DT:WP-1` / `DT:WP-2` / `DT:WP-3` 冻结内容？** | ✅ **未违反** —— 跨冻结冲突已在 R3 收敛（传输编码 ≠ 落库语义）；**R5 未触及任何冻结内容**（端点定稿 = **接口面**决策，**不涉及 schema / 索引 / retention / cleanup**） | §2 · Design §9.1 / §11 |
| **2** | **是否引入 Worker policy（第三套 telemetry policy）？** | ✅ **未引入 · 且边界已显式固化** —— **§5.1.1**：**允许 5 类（纯结构） / 禁止 5 类（业务）**；**R5 未扩大 Worker 职责** | Design §5.1.1 · `INV-14` |
| **3** | **是否产生实现副作用（代码 / DB / 云端）？** | ✅ **零副作用** —— 本轮**仅 docs 文件**（**新端点仅设计定名，未写 Worker / 未改 EMQX**） | §7 |
| **4** | **是否解除 / 改动任何 Gate？** | ✅ **未改动** —— `G-1…G-5` 状态**逐项未变** | §8 |
| **5** | **R5 裁决是否全部落实？** | ✅ **1/1 落实**（`14` → `CLOSED` · 选择 B）；**已补端点定义 / EMQX Action 指向关系 / authentication 继承规则 / 与旧 endpoint 边界** | §4 · §6 |
| **6** | **`INV-1…INV-14` 是否仍全部满足？** | ✅ **全部满足**（**未新增字段 / 索引 / schema / retention / cleanup / topic / ACL / migration**；**新增端点属接口面定稿**，**不改任何表结构**） | §5 |
| **7** | **端点决策（选择 B）是否已落定且自洽？** | ✅ **已落定** —— **新专用端点 `POST /api/history/ingest`**（不写 `mqtt_messages`）；**旧 `POST /api/ingest` 保持现状不改**；**与 `ID-1` / `T-4` 身份与守卫语义一致** | Design §9.1 · §2.6 |
| **8** | **收口完备性（R5）** | ✅ **17 项均有明确终态**（`CLOSED` 15 / `OPEN` 2）；**`OPEN` 2 项均标「明确界外 · 可延后」**；**「实施前必裁」项 = 0**；**无编号空洞、无悬空引用** | §6 |

---

## §1 审核范围与方法

| 项 | 内容 |
|---|---|
| **审核对象** | `docs/architecture/P3-DT-WP4-History-Ingestion-Design.md`（**R5**） |
| **审核方法** | ① 对**上游三份冻结文档**逐条口径核对；② 对**计划轨 `计划:WP-3` / `计划:WP-4`** 边界核对；③ 对**现状 Worker 代码**（只读）行为核对；④ **R5 裁决落实逐项核对**；⑤ **冲突登记完整性 + 收口完备性**核查 |
| **不做的** | **不裁决**任何 `OPEN` 项（`10` / `12`） · **不修改** 设计稿结论 · **不修改** 任何上游冻结文档 · **不进入实施**（**新端点仅为设计定名**） |
| **产物** | 本报告（`REVIEW ONLY`）+ `docs/review/P3-WP4-Design-Review-Resolution.md` |

---

## §2 与 `DT:WP-1` / `DT:WP-2` / `DT:WP-3` 一致性逐条核对

### 2.1 与 `DT:WP-1`（Telemetry Policy · `FROZEN`）

| # | 冻结内容 | 本设计的体现 | 结论 |
|---|---|---|---|
| 1 | **唯一权威位置 = 设备侧**；EMQX 只执行过滤；Worker 只 ingest/storage | Design §2.1 三层责任表 · **§5.1.1** | ✅ 一致 |
| 2 | **heartbeat 不落库**（`H-W1-3`） | Design `INV-2`；**Worker 不做 heartbeat 判断（§5.1.1 禁止项②）⇒ 不落库由上游保证**；`F-WP4-1` **已按裁决 `CLOSED`** | ✅ **一致（边界已定）** |
| 3 | **online 仅迁移点**（`H-W1-4`） | Design `INV-4` · §2.3 | ✅ 一致 |
| 4 | **system health 正常不落库**（`H-W1-6`） | Design `INV-3` | ✅ 一致 |
| 5 | **`weight.enable=false` ⇒ 无 weight 数据**（`F-4`） | Design `INV-5`；Worker **不感知**该开关 | ✅ 一致 |
| 6 | **设备 Flash 级别 ≠ 云端级别**（`F-6`） | Design `INV-7` · §4.2 | ✅ 一致 |
| 7 | **9 类分类**（§2） | Design §2.3 域 → 表映射；**不改任何落库判定** | ✅ 一致 |
| 8 | **Log 受 `BT-1` 门禁**（§3.8） | Design §4.3 · §10.5（登记 `G-4`）；**`H-WP4-12` 保持 `OPEN`（界外）** | ✅ 一致 |

### 2.2 与 `DT:WP-2`（History Data Model · `FROZEN`）

| # | 冻结内容 | 本设计的体现 | 结论 |
|---|---|---|---|
| 1 | `telemetry_history` 字段集合（8 列） | Design §5.2 **逐字段对齐**；**未新增字段** | ✅ 一致 |
| 2 | `log_history` 字段集合（10 列） | Design §5.2 同上 | ✅ 一致 |
| 3 | **幂等键**：telemetry `(device_id,type,ts,seq)` / log `(device_id,boot_seq,seq)` | Design §6 · `INV-8`；**幂等键完整性检查列入「允许的结构校验」（§5.1.1 ⑤）** | ✅ 一致 |
| 4 | **索引集合** | Design §6.2 **复用唯一索引**；§8.2 **不增不减** | ✅ 一致 |
| 5 | **`ts` = Unix 秒**（`H-WP2-13`） | Design `INV-10`；**仅校验存在 + 类型** | ✅ 一致 |
| 6 | `value` 物理 `TEXT` + `schema_version` 解释 | Design §3.3；**`schema_version > 1` 接收 + 校验格式 + 原样保存；不解释未知字段（`H-WP4-04`）** | ✅ **一致（前向兼容）** |
| 7 | 两表**物理分离** | Design `INV-6` · §2.1 | ✅ 一致 |
| 8 | **`payload` 内容 = `JSON` 字符串**（`H-WP2-3` 冻结） | ✅ **R3 已闭环**：**`Base64` = 传输承载**，**落库语义仍依本冻结**；**禁止修改 payload（`H-WP4-04` 禁令③）** | ✅ 一致 |

### 2.3 与 `DT:WP-3`（Storage Policy · `FROZEN`）

| # | 冻结内容 | 本设计的体现 | 结论 |
|---|---|---|---|
| 1 | telemetry retention = **90 天** | Design `INV-11`；**ingest 不感知窗口** | ✅ 一致 |
| 2 | log retention = **180 天**（L-A） | Design `INV-11` | ✅ 一致 |
| 3 | cleanup = **scheduled cleanup job** | Design §7.5（**唯一交点是同一张表**） | ✅ 一致 |
| 4 | 删除硬规则 `D-1…D-5` | Design §7.5（**cleanup 专属**）；**`H-WP4-08` 明确「DLQ 不进入 WP-4」⇒ 无第二删除路径** | ✅ 一致 |
| 5 | `H-WP3-10` **no additional index** | Design §6.2 · §8.2（**继承为硬约束**） | ✅ 一致 |
| 6 | `H-WP3-06` `payload` 上限 `DEFERRED` | Design §4.2 · §5.1 `V-L5` · `F-WP4-9`（**Worker 侧遵守平台 request body 上限 + 拒收超限**；业务上限仍归 `DT:WP-3`） | ✅ 一致（缺口承接） |

> **★ 一致性总评（R5）**：**三份冻结文档的「字段 / 类型 / 索引 / retention / cleanup / 语义」全部未被违反**；**R5 的端点裁决为「接口面」决策（新增专用端点），不触及任何表结构 / 索引 / 保留策略**，**未引入任何新的跨冻结分歧**。

---

## §3 Worker 边界核对（**`计划:WP-4` 4 职责 / 4 禁止 + 显式边界**）

| 项 | 计划要求 | Design 体现 | 结论 |
|---|---|---|---|
| **职责①** 边界校验 | `X-Webhook-Key` · `device_id` 形状 · REVOKED 守卫 · 必需字段存在性 | §5.1 `V-L0…V-L5` · §5.2 · **§5.1.1（允许 5 类）** | ✅ |
| **职责②** 批量写入 | 批量 INSERT / `D1 batch` | §8.1 · `W-2`（支持 N=1 / N>1） | ✅ |
| **职责③** 幂等 | 唯一键 / `INSERT OR IGNORE` | §6（`IA`/`IB`）；**幂等键完整性检查（§5.1.1 ⑤）** | ✅ |
| **职责④** 持久化 | 通过校验的记录**原样**写入 | §8（**无改写**）；**`schema_version` 未知版本亦**原样保存**（§3.3）** | ✅ |
| **禁止①** 判断「该不该存」 | — | §5.1 V-禁止 · **§5.1.1（禁止 5 类）** · `INV-14` | ✅ |
| **禁止②** 判断「重量变化是否有效」 | — | §5.1 V-禁止 · §5.1.1 禁止⑤ | ✅ |
| **禁止③** 实现业务降频 | — | §3.2 · **§5.1.1 禁止④** | ✅ |
| **禁止④** 解释设备业务语义 | — | §2.5 · §3.3 · **§5.1.1 禁止①③** · `H-WP4-17`（禁改 / 禁解释 payload） | ✅ |
| **★ R2 核对** | `H-WP4-05` 三项禁令 | §4.2 禁止清单 · `L-5` 取消解码 | ✅ |
| **★ R3 核对** | `H-WP4-17` 四项禁令 | §4.2 · §9.2 | ✅ |
| **★ R4 核对** | `H-WP4-04` 三项禁令（**禁拒绝未知版本 / 禁解释未知字段 / 禁改 payload**） | §3.3（R4 补充段）· §5.4 `R-4` | ✅ |
| **★ R4 核对** | `H-WP4-09` 允许 5 / 禁止 5 | **§5.1.1 独立小节**（可复用判据） | ✅ |
| **★ R5 核对** | **`H-WP4-14` 端点边界** —— 新端点**不写 `mqtt_messages`** · **不扩大 Worker 职责**（仍仅 4 项） | §9.1（R5 裁决块）· §2.1 链路图 | ✅ |

> **★ R5 关键确认（端点边界）**：**选择 B 的实质是「隔离副作用」而非「增加能力」** —— **新端点仍只做 ingest / validation / idempotency / persist（4 职责不变）**；**「不写 `mqtt_messages`」= 去掉旧端点的副带写入** ⇒ **更贴合 `INV-1` / `INV-14`**，**未构成 Worker policy 扩张**。
> **★ R5 关键确认（旧端点不动）**：**旧 `POST /api/ingest` 保持现状** ⇒ **既有调用方与既有 `mqtt_messages` 写入行为不受影响**（**无破坏性变更**）。

---

## §4 冲突与发现清单（`F-WP4-*` · **R5 重新评估**）

| ID | 发现 | R1 等级 | **R5 状态** | 依据 |
|---|---|---|---|---|
| **`F-WP4-1`** | heartbeat 的**结构性空档** | 🟠 | ✅ **`CLOSED`（裁决）** | **R4：`H-WP4-09` 关闭 ⇒ Worker 不做 heartbeat 判断，由上游保证** |
| **`F-WP4-2`** | **`log_ack` 通路缺失（`BT-1`）** 与 ingest 重传语义的交互 | 🔴 | **`OPEN`** | `H-WP4-12`（**界外 · `G-4`** · 可延后） |
| **`F-WP4-3`** | telemetry 入口 topic 三套口径并存 | 🔴 | ✅ **`CLOSED`** | R3：`H-WP4-01` / `13` |
| **`F-WP4-4`** | CBOR 二进制 vs JSON 信封 | 🔴 | ✅ **`CLOSED`** | `H-WP4-05`：`payload_base64` |
| **`F-WP4-5`** | telemetry `ts` / `seq` 无来源 | 🔴 | ✅ **`CLOSED`** | `H-WP4-02` + `03` |
| **`F-WP4-6`** | `INSERT OR IGNORE` 吞 NOT NULL | 🟠 | ✅ **`CLOSED`（缓解）** | `H-WP4-11` + `W-4` |
| **`F-WP4-7`** | REVOKED 返 200（确认项） | 🟢 | **`CONFIRMED`** | 非缺陷 |
| **`F-WP4-8`** | 解码失败两难 | 🔴 | ✅ **`CLOSED`** | `H-WP4-05` |
| **`F-WP4-9`** | `payload` 无**业务**上限 | 🔴 | ⚠️ **部分保留** —— ✅ **Worker 侧处置已定**；⚠️ **业务上限仍归 `DT:WP-3`** | `H-WP3-06` |
| **`F-WP4-10`** | `/api/ingest` 复用 ⇒ 写 `mqtt_messages`；旧 webhook 鉴权被注释 | 🟠 | ✅ **`CLOSED`（R5 裁决）** | **R5：`H-WP4-14` = 选择 B（新专用端点）⇒ 不写 `mqtt_messages` · 职责单一 · 鉴权语义自洽** |
| **`F-WP4-11`** | `payload` 原始落库 ↔ N-3 脱敏 | 🟠 | ✅ **`CLOSED`（责任转移）** | R3：`H-WP4-17` |
| **`F-WP4-12`** | 不解码 CBOR ↔ log 逐条幂等键 | 🔴 | ✅ **`CLOSED`** | R3：`H-WP4-16` |

> **★ 统计（R5）**：`CLOSED` / 已缓解 **9**（`1` / `3` / `4` / `5` / `6` / `8` / `10` / `11` / `12`）· **部分保留 1**（`9` · Worker 侧已定、业务上限外挂）· **`OPEN` 1**（`2` · `log_ack` · 界外 / `G-4`）· **`CONFIRMED` 1**（`7`）。
> **★ 审核确认**：**`F-WP4-10` 系依 `H-WP4-14` 裁决收敛**（**非设计自行解决**）；**残留 `OPEN` 1 项（`F-WP4-2`）明确界外**，**不构成链路硬阻塞**。

---

## §5 风险列表（`R-WP4-*` · **R5 更新**）

| # | 风险 | R1 | **R3 后** | **R4** | **R5** | 缓解 |
|---|---|---|---|---|---|---|
| **`R-WP4-1`** | 幂等键不可构造 | 🔴 | 🟡 | 🟡 | 🟡（不变） | `H-WP4-02` / `03`；**残余**：`seq` 跨 boot 规则转交设备协议 |
| **`R-WP4-2`** | 入口 topic 未统一 | 🔴 | 🟢 | 🟢 | 🟢（不变） | R3：`H-WP4-01` / `13` |
| **`R-WP4-3`** | `payload` 无上限 | 🔴 | 🟡 | 🟡 | 🟡（不变） | Worker 侧已定；**业务上限仍归 `DT:WP-3`** |
| **`R-WP4-4`** | 敏感值明文落库 | 🟠 | 🟡（转移） | 🟡 | 🟡（不变） | R3：责任归设备侧（**须在固件侧落地**） |
| **`R-WP4-5`** | 重投风暴 / 静默丢批 | 🟠 | 🟢 | 🟢 | 🟢（不变） | `H-WP4-07` |
| **`R-WP4-6`** | 静默丢数据 | 🟠 | 🟡 | 🟡 | 🟡（不变） | `H-WP4-11` + `W-4` |
| **`R-WP4-7`** | 写放大 | 🟡 | 🟡 | 🟡 | 🟡（不变） | 索引已冻结；只能批量化 |
| **`R-WP4-8`** | 越权写入 | 🟡 | 🟢 | 🟢 | 🟢（不变） | `ID-1` / `ID-3` / `ID-6` |
| **`R-WP4-9`** | log 逐条幂等不可实现 | — | 🟢 | 🟢 | 🟢（不变） | R3：`H-WP4-16` |
| **`R-WP4-10`** | **长期重试耗尽后数据丢失**（无 DLQ） | — | — | 🟡（已接受） | 🟡（不变） | **R4：`H-WP4-08` 明令 DLQ 界外** ⇒ 兜底 = 上游重投 + 幂等；⚠️ **代价已登记未来版本**（§10.6） |
| **`R-WP4-11`** | **共享 key 无第二因子**（无限流 / IP 限制 / 轮换） | — | — | 🟡（已接受） | 🟡（不变） | **R4：`H-WP4-15` 明令不引入**；保留 `X-Webhook-Key` + EMQX ACL + Cloudflare 基础防护 |
| **`R-WP4-12`** | **新增路由与 EMQX Action 指向变更的实施面** | — | — | — | 🟢 **新增（低 · 已登记）** | **R5：`H-WP4-14` = 选择 B** —— 端点**仅设计定名**；**EMQX 指向变更属实施期动作**（**本文件不改 EMQX**）；**旧端点不动 ⇒ 无破坏性变更** |

> **★ 说明**：`R-WP4-10` / `11` 系 **R4 裁决的"已知并接受"残余风险**（**非缺陷、非未决**）；**`R-WP4-12` 系 R5 端点裁决的实施面提示**（**已知 · 待实施期处理**）—— 均已按纪律**登记**，供人工在冻结批准时知悉。
> **★ R5 收敛**：**原「端点未定」相关残余（`R-WP4-2` 语境）已随 `H-WP4-14` 关闭而彻底收敛**。

---

## §6 `H-WP4-*` 清单汇总（**审核核对 · R5**）

| 项 | 数量 | 状态 |
|---|---|---|
| **`H-WP4-01…17`** | **17** | **`CLOSED` 15**（R2：`02` / `03` / `05` / `06` / `07` / `11` · R3：`01` / `13` / `16` / `17` · R4：`04` / `08` / `09` / `15` · **R5：`14`**）· **`OPEN` 2**（`10` / `12`） |
| **`OPEN` 定位** | 2 | **`10` / `12` = 均「明确界外 · 可延后」**；**「实施前必裁」项 = 0** |
| **承接的上游 `H-*`** | 3（`H-WP3-06` · `H-WP2-3` · `H-WP2-11`） | **仅引用，未重开** ✅ |
| **`F-WP4-*`** | **12** | **`CLOSED` 9 · 部分保留 1 · `OPEN` 1 · `CONFIRMED` 1** ✅ |

> **★ 校对结论**：**编号连续（01–17）· 无重复 · 无跳号 · 无空洞**；**R5 无新增编号、无重编号**；**每项均有明确终态**；**留痕见 Design §10.0-R2 / §10.0-R3 / §10.0-R4 / §10.0-R5 / §10.1 / §10.4 / §10.6**。
> **★ `H-WP4-14` 终态确认**：**`CLOSED`** —— **裁决 = 选择 B（新增专用 history ingest endpoint）**；**A/B 对比与风险分析已作为留档保留在 Design §9.1**；**未新增第二套幂等键 / 未改 schema**。

---

## §7 副作用核对（**doc-only 证明**）

| 检查项 | 结果 |
|---|---|
| **固件仓新增 / 修改（仅 `docs/`）** | **修改** `docs/architecture/P3-DT-WP4-History-Ingestion-Design.md`（R5）· `docs/architecture/P3-DT-WP4-History-Ingestion-Review.md`（R5）· `docs/review/P3-WP4-Design-Review-Resolution.md`（R5）· `docs/README.md`（**仅索引 / 状态同步**） |
| **`src/`** | **未改动** |
| **`platformio.ini` / `partitions.csv`** | **未改动** |
| **migration（`0003_*`）** | **未创建** —— `guo-feeder-api/migrations/` 仍只有 `0001_*` / `0002_*` |
| **D1 / Worker / EMQX** | **未改动** · **未部署** · **未新增 topic / 未改 ACL**；⚠️ **新端点 `POST /api/history/ingest` 仅为设计定名（未写 Worker 代码 · 未改 EMQX Action）** |
| **冻结文档**（`DT:WP-1` / `DT:WP-2` / `DT:WP-3` / `P2-0-…freeze.md` / `cloud_protocol.md` / `P3-History-D1-Plan.md`） | **逐字未动** |
| **Gate** | **未改动**（见 §8） |
| **`commit` / `push`** | **均未执行**（按指示留待人工**冻结批准**） |

---

## §8 Gate 状态核对

| 门禁 | 状态 | 本轮改动 |
|---|---|---|
| **`G-1`** | **`HUMAN_REQUIRED`** | **否** |
| **`G-2`** | **`PASS`** | **否** |
| **`G-3`** | **`PASS`** | **否** |
| **`G-4`** | **`BLOCKED`** | **否** |
| **`G-5`** | **`PASS`** | **否** |

> **★ 声明**：本文件**不解除、不新增、不映射任何门禁**；⚠️ **`DT:WP-4 = FROZEN`（设计冻结）不构成 `G-1` 的批准** —— **实施入口门禁仍须人工裁决**。

---

## §9 审核结论（**R5**）

| 项 | 结论 |
|---|---|
| **R5 裁决落实** | ✅ **1/1 全部落实**；**端点裁决 = 选择 B**（新专用 `POST /api/history/ingest`）；**已补端点定义 / EMQX Action 指向 / authentication 继承 / 旧端点边界** |
| **设计完整性** | ✅ 覆盖 10 项设计范围 与 §0–§10 必需章节，并补 `INV` / 一致性 / 纪律 / §5.1.1 / §13.1 |
| **与上游一致性** | ✅ **字段 / 类型 / 索引 / retention / cleanup / 语义全部一致** |
| **Worker 边界** | ✅ **4 职责 / 4 禁止** + `H-WP4-05`（R2）+ `H-WP4-17`（R3）+ `H-WP4-04`（R4）+ **`H-WP4-14` 端点边界（R5）** + **§5.1.1 显式边界** |
| **冲突处置** | ✅ **12 项 `F-WP4-*` + 17 项 `H-WP4-*` 全部登记，无自行裁决** |
| **副作用** | ✅ **doc-only**（§7） |
| **Gate** | ✅ **未改动**（§8） |
| **当前状态** | **`DT:WP-4 = FROZEN` · R5 冻结回填（15 `CLOSED` / 2 `OPEN`）**（**设计已冻结 · 实施唯一基线 · 人工批准**） |
| **是否达到 FROZEN** | ✅ **是** —— **设计已冻结**；**硬阻塞 0**；**「实施前必裁」项 0**；**剩余 2 项 `OPEN` 均明确界外、可延后** |

> **★ 剩余 `OPEN` 2 项（冻结后仍成立 · 非裁决 · 供实施期知悉）**：
> ① **`H-WP4-10`（`ts` 窗口）** = **明确界外**（属未来策略），**可延后**；
> ② **`H-WP4-12`（`log_ack`）** = **明确界外**（属 `计划:WP-5` 且受 `G-4`），**可延后**。
> **★ 已接受的残余风险（须人工知悉）**：`R-WP4-10`（无 DLQ ⇒ 重试耗尽后可能丢数据）· `R-WP4-11`（共享 key 无第二因子）· `R-WP4-12`（新端点实施面 · 低 · 已登记）。
> **★ 实施前置（不属本设计范围）**：**新端点实现** · **EMQX Action 指向变更** · **`deviceIdFromTopic()` helper 复用** —— 均属实施阶段动作。
> **★ 跨 WP 待办**：`DT:WP-3` `H-WP3-06`（`payload` **业务**上限）；**设备侧「禁止上传含 secret 日志」义务须在固件侧落地**。

---

> **DT:WP-4 Review only means design self-check completed. It does not mean the design is approved, frozen, or implemented.**
> **R5: 1 H-WP4 item closed by human decision (H-WP4-14 = option B, dedicated history ingest endpoint); 15 CLOSED / 2 OPEN. FROZEN — this design is the single implementation baseline.**
> **Endpoint decision: new `POST /api/history/ingest` (option B). Legacy `POST /api/ingest` unchanged. Design-only — no Worker / EMQX change.**
> **P3 remains NOT STARTED / PLANNED. No Gate changed.**
> **No implementation. No Worker code. No migration. No DDL. No deploy. No new topic. No ACL change. No DLQ. No rate limiting. The new endpoint is design-only.**

---

*本文件于 **2026-10-10** 建立（R1），经 **R2 / R3 / R4 / R5** 修订，为 **`DT:WP-4` 设计稿的自检审核报告（`REVIEW ONLY`）**；不含任何实现、迁移或代码改动。*
*审核对象 = `P3-DT-WP4-History-Ingestion-Design.md`（R5）；基线 = 固件仓 HEAD `fee6b77`（`DT:WP-3` 冻结提交 · 工作树 clean）。*
