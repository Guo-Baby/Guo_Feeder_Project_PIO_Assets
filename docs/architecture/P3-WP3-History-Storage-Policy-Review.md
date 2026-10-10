# P3 · WP-3 —— History Storage Policy Review（自检审核报告 · **R3**）

> **性质**：**REVIEW / SELF-CHECK ONLY**。本文件**不含代码、不修改任何既有文档**，只做**对 `DT:WP-3` 冻结设计的自检、一致性核对、发现与风险登记**。
> **状态**：**`DT:WP-3 = FROZEN`（存储策略设计冻结完成）** · **`P3 = NOT STARTED / PLANNED`** · **`G-1` / `G-3` / `G-4` 未变** · **`push = DEFERRED`**
> **被检对象**：`docs/architecture/P3-WP3-History-Storage-Policy-Design.md`（`DT:WP-3` 设计 · **`FROZEN`**）
> **自检依据**：`P3-WP1-Telemetry-Policy-Final.md`（**`FROZEN`**）· `P3-WP2-History-Data-Model-Design.md`（**`FROZEN`**）· `P3-History-D1-Plan.md`（R2）· `migrations/README.md`
> **自检时点**：**2026-10-10** · R1 基线 = 固件仓 HEAD **`1fd06c1`** · R2 基线 = **`9eecaa5`** · **R3 基线 = `fd41972`**
> **★ 修订**：**R2（2026-10-10 · 依人工设计审核要求）** —— 被检对象按 6 项要求修订（编号解释 / retention 推荐基线 / 容量依据 / 删除硬规则 / 归档措辞 / `H-WP3` 重编号）⇒ 本报告同步：`F-WP3-3`/`F-WP3-4` 状态更新、新增 `F-WP3-9`。
> **★ 修订**：**R3（2026-10-10 · 依人工裁决回填）** —— 被检对象已 **`FROZEN`**（**人工已裁决批准**）：**`H-WP3-01…11` 全部裁决回填**（`CLOSED` 8 项 / `DEFERRED` 3 项）· retention **冻结 90 / 180 天** · cleanup = **`scheduled cleanup job`** · **no additional index** ⇒ 本报告同步：**§0 自检表**、**§6 发现**、**§7 风险**、**§8 裁决清单**、**新增 §9.2 `R3 落实情况`**。
>
> **⚠️ 术语铁律**：`PASS` · `NOT RUN` · `HUMAN_REQUIRED` · `BLOCKED` · `DEFERRED` · `CLOSED`。**`NOT RUN` / `HUMAN_REQUIRED` / `BLOCKED` 一律 ≠ `PASS`**。
> **★ 引用规范**：见被检对象 **`WP3:§1`** —— `计划:WP-n` = 计划轨；`DT:WP-n` = 设计轨；`WP1:§n` / `WP2:§n` = 冻结文档章节；`WP3:§n` = 被检对象章节；`P3-plan-§n:<id>` = 计划章节。

---

## §0 自检结论摘要

| 检查项 | 结果 |
|---|---|
| **是否违反 `WP-1`（Telemetry Policy 冻结）** | ✅ **否** —— `I-1…I-8` 逐项对齐（§2.1） |
| **是否违反 `WP-2`（Schema 冻结）** | ✅ **否** —— **零 schema 变更**；**不新增 / 不删改索引**（`H-WP3-10` = no additional index）；清理策略**适配**冻结索引（§2.2 / §3 / §4） |
| **是否引入 Worker policy** | ✅ **否** —— 清理**不在 ingest 路径**；Worker 职责边界未动（§3） |
| **是否修改实现** | ✅ **否** —— 零代码 / 零 DB / 零云端副作用（§4） |
| **是否解除或改动任何 Gate** | ✅ **否** —— `G-1` / `G-3` / `G-4` **均未变**（§5） |
| **是否冻结自身** | ✅ **是（依人工裁决）** —— 状态 = **`FROZEN`**；⚠️ **系人工裁决结果**，非本报告自行判定 |
| **是否新增 `HUMAN_REQUIRED`** | ✅ **否** —— `H-WP3-01…11` **已全部裁决**（`CLOSED` **8** · `DEFERRED` **3**，§8）；**无遗留 OPEN** |
| **是否把任何项写成 `PASS`** | ✅ **否** —— 容量数字全部标 **`NOT RUN`**（`WP3:§9`）；`CLOSED` 均系**人工裁决记录** |
| **是否 `push`** | ✅ **否** |
| **`P3` 状态** | **`NOT STARTED / PLANNED`**（**未变**） |

> **★ 一句话**：**`DT:WP-3` 为纯设计产物 —— 未越界、未违反 WP-1 / WP-2、未动实现、未改门禁；其 11 项 `H-WP3-*` 已由人工全部裁决 ⇒ `DT:WP-3 = FROZEN`（存储策略设计冻结）。**
> **⚠️ 冻结 ≠ 实施**：**WP-3 Frozen means storage policy design frozen. It does not mean implementation started.**

---

## §1 自检范围与方法

| 维度 | 方法 |
|---|---|
| **合规性** | 把 `WP3:§3` 的每条不变量与 `WP-1` / `WP-2` 冻结文档**逐条 diff** |
| **边界性** | 检查是否出现「Worker 判 telemetry / 阈值 / 降频 / 业务分类 / 在 ingest 内做删除」 |
| **实现性** | 检查是否出现代码 / migration / DDL 落地 / 部署动作 |
| **门禁性** | 检查是否声明任何 Gate 状态变更 |
| **冻结性** | 检查冻结是否**有裁决依据**（人工裁决）且**未越权自称冻结** |
| **风险性** | 登记设计与上游之间的**张力 / 待承接项 / 失效模式**（§6 / §7） |

**自检未做（明确声明）**：未运行任何代码；未连接 D1；未部署；未创建 migration；**未做容量实测**。

---

## §2 与 `WP-1` / `WP-2` 一致性检查

### 2.1 对 `WP-1`（Telemetry Policy 冻结）

| WP-1 冻结项 | WP-1 要求 | `DT:WP-3` 设计 | 结论 |
|---|---|---|---|
| **唯一权威位置** | 采样决策 = 设备侧；Worker 只 ingest/storage | `WP3:§3 I-1` · `WP3:§7.2` **拒绝**写入时清理 | ✅ **一致** |
| **Heartbeat** | 不进入 history（`H-W1-3`） | `WP3:§3 I-2`；§5/§6 **均不涉及 heartbeat** | ✅ **一致** |
| **System health** | 正常不落库；异常 → EVENT/ERROR（`H-W1-6`） | `WP3:§3 I-3` | ✅ **一致** |
| **Online state** | 仅迁移点；禁周期 offline（`H-W1-4`） | `WP3:§3 I-4`（未放宽） | ✅ **一致** |
| **Weight** | `weight.enable=false` ⇒ 无 weight history（`F-4`） | `WP3:§3 I-5`（未引入新 row 类型） | ✅ **一致** |
| **Log 分离** | log 不得混入 telemetry 表 | `WP3:§3 I-6` · `WP3:§10 Q-8` | ✅ **一致** |
| **设备 Flash Log ≠ 云端 LogHistory**（`F-6`） | 两者独立策略项 | `WP3:§6.6` **专节重申** | ✅ **一致** |
| **容量目标** | `< 1200 行/天`（保留） | `WP3:§5.1.1` / `§9.2` 沿用（**上限估算**） | ✅ **一致** |
| **引用规范** | 命名空间限定，禁裸 `R-n`（`F-1`） | 被检对象**全文**采用 `issues:` / `P3-plan-§n:` 限定 | ✅ **一致** |

> **⇒ §2.1 结论**：**`DT:WP-3` 设计未违反 `WP-1` 的任何冻结项。`PASS`（自检项）。**

### 2.2 对 `WP-2`（Schema 冻结）

| WP-2 冻结项 | WP-2 要求 | `DT:WP-3` 设计 | 结论 |
|---|---|---|---|
| **表结构 / 字段** | `telemetry_history` / `log_history` 冻结 | `WP3:§3 I-9`；§5/§6 **零字段变更** | ✅ **一致** |
| **索引集合** | `ix_th_device_ts` · `ix_th_device_type_ts` · `ux_th_dedup`；`ix_lh_device_ts` · `ux_lh_dedup` | `WP3:§3 I-9` 显式列出；**§7.4 `D-5`「不修改冻结索引」** + **`H-WP3-10` = no additional index** | ✅ **一致** |
| **无 `(type, ts)` 索引** | 已删除 | `WP3:§10 Q-3`（P3 禁跨设备聚合） | ✅ **一致** |
| **无 `(device_id, level, ts)` 索引** | 已暂缓 | `WP3:§6.2` 据此**冻结 L-A 统一窗口**（`H-WP3-02`） | ✅ **一致** |
| **`ts` = Unix 秒** | 冻结（`H-WP2-13`） | `WP3:§3 I-10` | ✅ **一致** |
| **幂等键** | telemetry `(device_id,type,ts,seq)`；log `(device_id,boot_seq,seq)` | `WP3:§3 I-11`（删除设计**不破坏**幂等键） | ✅ **一致** |
| **`log_history.payload` = `TEXT(JSON)`** | 冻结（`H-WP2-3`） | `WP3:§6.4` **不改类型**（上限/处置 = **`DEFERRED`**，见 `F-WP3-10`） | ✅ **一致** |
| **retention `DEFERRED` → WP-3** | `H-WP2-9` / `H-WP2-10` | `WP3:§5.1` / `§6.1` ⇒ **`H-WP3-01`/`02` = `CLOSED`**（**90 / 180 天**） | ✅ **承接并已裁决** |
| **`payload` 上限 `DEFERRED` → WP-3** | `H-WP2-8` | `WP3:§6.4` + **`H-WP3-06` = `DEFERRED`**（⚠️ 仍未闭环，`F-WP3-10`） | ⚠️ **承接但再次延期** |
| **`mqtt_messages` `DEFERRED`** | `H-WP2-11` | `WP3:§11.4` **仅登记**（**非本 WP 裁决项**） | ✅ **未越界** |

> **⇒ §2.2 结论**：**`DT:WP-3` 设计未修改 schema、未新增/删改索引，且其清理 / 删除策略显式适配冻结索引并固化为硬规则（`WP3:§7.4`）。`PASS`（自检项）。**
> **⚠️ 唯一未闭环项**：`H-WP3-06`（`payload` 上限）**再次 `DEFERRED`** ⇒ 见 `F-WP3-10`。

---

## §3 是否引入 Worker policy（**边界核对**）

> **`I-1`**：Worker = **ingest → validation → idempotency → persist**；**不含** telemetry 判断 / 阈值 / 降频 / 业务分类 / **运维删除**。

| Worker 禁止事项 | `DT:WP-3` 设计中是否出现 | 证据 |
|---|---|---|
| 判断 telemetry **是否应该保存** | ❌ **未出现** | `WP3:§5.1` 只谈**已落库行**的窗口保留 |
| **阈值 / 降频** | ❌ **未出现** | 阈值属设备侧（`WP1`）；`WP3:§7.2` 明确**不在 ingest 路径**做清理 |
| **业务分类** | ❌ **未出现** | `WP3:§6.2` 只按**已冻结的 `level` 取值**讨论 |
| **在 ingest 请求内做删除** | ❌ **未出现（且被显式拒绝）** | `WP3:§7.2` 候选 **B = 不推荐**，理由 = 违反 `I-1`；**`H-WP3-03` = `scheduled cleanup job`**（独立 Cron） |
| **幂等** | ✅ **出现（允许）** | 属 Worker 4 项职责之一（`计划:WP-4`） |

> **⇒ §3 结论**：**`DT:WP-3` 设计未让 Worker 承担 telemetry policy 职责；清理被明确移出 ingest 路径（Cron 独立），且已由 `H-WP3-03` 冻结为 `scheduled cleanup job`。`PASS`（自检项）。**

---

## §4 是否修改实现（**副作用核对**）

| 项 | 是否触碰 | 证据 |
|---|---|---|
| **固件 `src/`（cpp/h）** | ❌ **未触碰** | 本轮仅改 `docs/` 文档 |
| **`platformio.ini` / `partitions.csv`** | ❌ **未触碰** | 同上 |
| **D1 migration** | ❌ **未创建** | `WP3:§13`；**未新建 `0003_*`** |
| **D1 schema / 索引** | ❌ **未改动** | `WP3:§7.4 D-5` + `H-WP3-10`（no additional index） |
| **Cloudflare Worker** | ❌ **未修改** | `WP3:§13` |
| **EMQX rule** | ❌ **未修改** | `WP3:§13` |
| **D1 数据** | ❌ **未写入** | 未连接 D1 |
| **部署** | ❌ **未部署** | 无任何部署动作 |
| **已冻结文档** | ❌ **未修改** | `P2-0-…freeze.md` / `WP1` / `WP2` **均未动** |
| **既有 migration（`0001_*` / `0002_*`）** | ❌ **未修改** | 仅**只读引用** |
| **`P3-Entry-Gate-Review.md`** | ⚠️ **仅状态同步（新增 R4 记录）** | **人工要求同步**（"Entry Gate（如存在 WP-3 状态引用）"）；**未改任何 Gate 状态** |

> **★「非设计性」文件操作（透明留痕）**：R2 时工作区副本 `docs/README.md` 曾被外部格式化器替换为「过期内容 + 表格重排」⇒ 已**从 `HEAD` 还原**（`git hash-object` 校验）后再做最小编辑。**R3 未再遇该问题。**

> **⇒ §4 结论**：**零代码 / 零 DB / 零云端副作用。`PASS`（自检项）。**

---

## §5 是否解除或改动任何 Gate（**门禁核对**）

| 门禁 | 状态 | 变化 |
|---|---|---|
| **`G-1`** P3 Plan approved | **`HUMAN_REQUIRED`** | **未变** |
| **`G-2`** Telemetry Policy 冻结 | **`PASS`**（`DT:WP-1` 人工裁决记录） | **未变**（非本文件产生） |
| **`G-3`** Schema 冻结 | **`PASS`**（`DT:WP-2` 人工裁决记录） | **未变**（非本文件产生） |
| **`G-4`** `BT-1` `CLOSED` | **`BLOCKED`** | **未变** |
| **`G-5`** 范围未扩大 | **`PASS`** | **未变** |

> **⇒ §5 结论**：**本文件与被检对象均不解除、不修改任何 Gate。** `DT:WP-3` 属**设计层**，其裁决项（`H-WP3-*`）**未被指派到任何门禁**；**`DT:WP-3 = FROZEN` 不改变任何 Gate**。

---

## §6 发现清单（`F-WP3-*`）

> **格式**：ID · 严重度 · 位置 · 发现 · 状态 / 建议。

| ID | 严重度 | 位置 | 发现 | 状态 / 建议 |
|---|---|---|---|---|
| **`F-WP3-1`** | 🟢 **已闭环** | `WP3:§3 I-9` · `§5.2` · **`§7.4`** | **冻结索引集不含「单列 `ts` 索引」** ⇒ 全局 `DELETE ... WHERE ts < ?` 会全表扫描 | **已固化为冻结硬规则 `D-1…D-5`**（批处理 / deterministic / 不依赖不存在索引）；`计划:WP-7` 实施须遵守 |
| **`F-WP3-2`** | 🟢 **已闭环** | `WP3:§6.2` | **log 冻结索引不含 `(device_id, level, ts)`** ⇒ 分级 retention（L-B）无法走索引 | **`H-WP3-02` 已裁决 = L-A 统一窗口**；**`H-WP3-10` = no additional index** ⇒ **L-B 在 P3 已排除** |
| **`F-WP3-3`** | 🟢 **已闭环** | `WP2:§6.2` vs `计划:WP-7` | **retention 候选方向曾自相矛盾**（tele 90 / log 30 vs Telemetry 短 / Log 长） | **R2 由推荐基线收敛方向；R3 已由人工裁决冻结为 90 / 180 天** ⇒ **完全闭环** |
| **`F-WP3-4`** | 🟢 **已闭环** | `WP3:§1` | **WP 编号歧义**（`计划:WP-3` = EMQX 集成；设计轨同名） | **R2 已通过 §1 编号解释 + `DT:`/`计划:` 前缀解决**；**R3 由 `H-WP3-11` = `CLOSED` 正式确认**；计划文档未改 |
| **`F-WP3-5`** | 🟠 中（**已接受**） | `WP3:§8.3` | **窗口（90 天）与聚合（`DEFERRED`）的耦合**：> 90 天趋势**无支撑** | **本阶段明确接受的缺口**；须在 **P4（Web UI 长期趋势页）** 立项时评估（`WP3:§8.3`） |
| **`F-WP3-6`** | 🟡 低 | `WP3:§9` | **容量数字全为「理论估算」**（未实测） | 已全部标注 **`NOT RUN`**；**须 `计划:WP-6` 实测校准**；**不得据此下「安全」结论** |
| **`F-WP3-7`** | 🟡 低 | `H-WP2-11` · `WP3:§11.4` | **`mqtt_messages` 去留「承接方不明」** | **R2 已移出本 WP**（转为「仅登记」，`WP3:§11.4`）；**承接方仍待人工指定** |
| **`F-WP3-8`** | 🟢 提示 | `WP3:§9.3(b)` | **log 爆发上界**（2 Hz ⇒ ≤172,800 行/设备/天）在 `BT-1` 修复后可能被逼近 | 已登记缓解；**须与 `BT-1` 修复 + 设备侧负载评估同批**（`G-4` 门禁） |
| **`F-WP3-9`** | 🟡 低 | `WP3:§5.5` | **归档「承接方」未定**（`H-WP3-05` = `CLOSED`，措辞 = future phase · consumer TBD） | **措辞与裁决已合规**；**残留**：`consumer ownership` **仍 TBD** ⇒ 归档未实现前**超窗数据不可恢复**（与 `F-WP3-5` 联动） |
| **`F-WP3-10`** | 🔴 **高（R3 新增）** | `WP3:§6.4` · `§11.2` | **`H-WP3-06`（`payload` 生命周期）再次 `DEFERRED`** ⇒ **`payload` 上限缺失、处置未定** | ⚠️ **与 `计划:WP-2`「`payload` 不得是无界字段」的要求尚未闭环**；**`计划:WP-7`（LogHistory）实施前必须裁决** —— 见 `R-WP3-9` |

> **⇒ R3 后结论**：`F-WP3-1`/`2`/`3`/`4` **已闭环**；**新增 `F-WP3-10`（🔴 高，唯一高等级）** —— 属 **`H-WP3-06` `DEFERRED` 的直接后果**，**阻塞 LogHistory 实施**（非本设计冻结的阻塞项）。

---

## §7 风险列表（`R-WP3-*`）

| # | 风险 | 等级 | 说明 | 缓解（设计层） |
|---|---|---|---|---|
| **`R-WP3-1`** | **容量估算失真** | 🟠 中 | 全部数字为估算；行数用**上限**（非典型） | 标 **`NOT RUN`**；**`计划:WP-6` 实测**校验；**偏离时走新的人工裁决** |
| **`R-WP3-2`** | **log 爆发打满配额** | 🔴 高 | `BT-1` 修复后上行可升 2 Hz（`P3-plan-§6:R-4`） | 级别过滤 + log 独立窗口（180 天）+（可选）爆发硬顶；⚠️ **`payload` 上限缺失**（`F-WP3-10`）**削弱了该项缓解** |
| **`R-WP3-3`** | **删除全表扫描** | 🟠 中 | 若实施阶段忽略 `I-9`（用全局 `ts` 谓词） | **§7.4 硬规则 `D-1…D-5`**（批处理 / deterministic / 不依赖不存在索引） |
| **`R-WP3-4`** | **超窗数据不可恢复** | 🟠 中 | 无归档（`H-WP3-05` = future phase，**consumer TBD**）+ 无聚合（`DEFERRED`） ⇒ 超 90 天即不可恢复 | **已接受**（`WP3:§5.5` / `§8.3`）；P4 立项时评估 |
| **`R-WP3-5`** | ~~11 项 `H-WP3-*` 全未裁决~~ | ✅ **已关闭（R3）** | —— | **`H-WP3-01…11` 已全部裁决**（`CLOSED` 8 / `DEFERRED` 3） |
| **`R-WP3-6`** | ~~分级 retention 与索引不匹配~~ | ✅ **已关闭（R3）** | L-B 已排除 | **`H-WP3-02` = L-A**；**`H-WP3-10` = no additional index** |
| **`R-WP3-7`** | ~~编号口径混淆~~ | ✅ **已关闭（R3）** | —— | **`H-WP3-11` = `CLOSED`**（采纳 §1 口径） |
| **`R-WP3-8`** | **大事务删除导致锁 / 超时** | 🟠 中 | 若实施把整窗口删除包成单事务 | **`WP3:§7.4 D-1` / `D-2`** 明令**批处理 + 禁大事务**；**`H-WP3-08` = 冻结原则（不冻结 batch size）** |
| **`R-WP3-9`** | **`payload` 无上限 → 存储 / 行膨胀**（**R3 新增**） | 🔴 **高** | `H-WP3-06` `DEFERRED` ⇒ `payload`（`TEXT(JSON)`）**无体积上限**；单条上行可达 **4096 B** ⇒ 爆发时写放大严重 | ⚠️ **必须**在 `计划:WP-7`（LogHistory）**实施前**完成 `H-WP3-06` 裁决（上限 + 超限处置）；**本设计冻结不含该项** |

> **★ 当前最高等级**：**`R-WP3-2`（log 爆发）与 `R-WP3-9`（`payload` 无上限）—— 二者叠加**；`R-WP3-9` 的根因（`H-WP3-06` `DEFERRED`）**须在实施前解决**；`R-WP3-2` 的根因（`BT-1` / 设备侧负载）**不在本文件范围**（`G-4`）。

---

## §8 `H-WP3-*` 裁决清单（**与 `WP3:§11` 逐项一致 · 11 项 · 状态已回填**）

### 8.1 已裁决（`CLOSED` / `PASS`）—— 8 项

| ID | 问题 | **冻结决策** | 状态 |
|---|---|---|---|
| **`H-WP3-01`** | retention telemetry | **`telemetry_history` retention = 90 天** | **`CLOSED` / `PASS`** |
| **`H-WP3-02`** | retention log | **`log_history` retention = 180 天**；级别 = **L-A 统一窗口** | **`CLOSED` / `PASS`** |
| **`H-WP3-03`** | cleanup mechanism | **`scheduled cleanup job`**（Cron 定期） | **`CLOSED` / `PASS`** |
| **`H-WP3-05`** | archive policy | **future phase · consumer ownership TBD** | **`CLOSED`** |
| **`H-WP3-07`** | cold storage | **不引入冷热分层** | **`CLOSED`** |
| **`H-WP3-08`** | batch delete | **冻结原则（`D-1…D-5`）；不冻结 batch size** | **`CLOSED`** |
| **`H-WP3-10`** | additional index | **`no additional index`** | **`CLOSED`** |
| **`H-WP3-11`** | numbering clarification | **采纳 §1 口径**（`DT:WP-3` ↔ `计划:WP-7`） | **`CLOSED`** |

### 8.2 已延期（`DEFERRED`）—— 3 项

| ID | 问题 | **裁决** | 状态 |
|---|---|---|---|
| **`H-WP3-04`** | aggregation boundary | **`DEFERRED`**（P3 不建聚合表） | **`DEFERRED`** |
| **`H-WP3-06`** | payload lifecycle | **`DEFERRED`**（上限 / 处置 / 生命周期均未裁决）⚠️ **阻塞 LogHistory 实施** | **`DEFERRED`** |
| **`H-WP3-09`** | device quota | **`DEFERRED`**（P3 不引入配额） | **`DEFERRED`** |

### 8.3 仅登记（**非本 WP 裁决项**）

| ID | 项 | 归属 | 状态 |
|---|---|---|---|
| **`H-WP2-11`** | `mqtt_messages` 去留 | 既有 schema 生命周期管理 | **`DEFERRED`（仅登记）** |

> **★ 合计 11 项（+1 仅登记）—— 现已全部裁决（`CLOSED` 8 · `DEFERRED` 3），§8.1 无 OPEN。**
> **★ 纪律**：**`CLOSED` / `DEFERRED` 均为人工裁决结果**；本报告**未自行裁决任何一项**。
> **★ 编号迁移留痕**：R1 的 `H-WP3-03/05/06/07/08/12` 已按 R2 重排（见 `WP3:§11.3`）。

---

## §9 人工要求 → 落实情况（**逐条对照**）

### 9.1 R2 审核要求

| # | 人工要求 | 落实情况 | 位置 |
|---|---|---|---|
| **1** | 解决 WP 编号歧义（Design Track / 对应 `计划:WP-7` / 避免裸引用 / 增加编号解释章节） | ✅ **新增 §1 编号解释**（轨道定义 + 对应表 + 5 条引用规则 R-1…R-5）；**全文改用 `DT:` / `计划:` 前缀** | `WP3:§1` · §0 · 全文 |
| **2** | retention 增推荐基线（tele 90 d / log 180 d，非冻结） | ✅ **`WP3:§5.1` / `§6.1` 新增「★ 推荐基线」行**，注明**推荐 ≠ 冻结** | `WP3:§5.1` / `§6.1` |
| **3** | retention 章增容量依据（单设备日行数 / 窗口 / 规模） | ✅ **新增 `WP3:§5.1.1`**（模型 + 30/90/180 × 1/100/1000/10000 表 + 线性关系说明）；`§6.1` 同构 | `WP3:§5.1.1` / `§6.1` / `§9` |
| **4** | 强化 D1 SQLite 删除策略 | ✅ **新增 `WP3:§7.4` 硬规则 `D-1…D-5`** | `WP3:§7.4` · `§5.2` |
| **5** | 归档措辞（不写死 P4/P5） | ✅ **改为 `implementation deferred to a future phase · consumer ownership TBD`** | `WP3:§5.5` · `§2.3` · `§9.4` |
| **6** | 校验 `H-WP3` 清单（11 项） | ✅ **重编号为 `H-WP3-01…11`**；`mqtt_messages` **移出为「仅登记」** | `WP3:§11` · 本报告 §8 |

### 9.2 R3 裁决回填要求（**本轮**）

| # | 人工要求 | 落实情况 | 位置 |
|---|---|---|---|
| **1** | 回填 `H-WP3-01…11` 裁决 | ✅ **`WP3:§11` 重构为「裁决回填」**（§11.1 `CLOSED` 8 项 / §11.2 `DEFERRED` 3 项，含冻结决策原文） | `WP3:§11.1` / `§11.2` |
| **2** | 状态 `FREEZE CANDIDATE` → `FROZEN`；增英文声明；保持 `G-1`/`G-3`/`G-4`/`P3` | ✅ 标题 + §0 + 尾注改为 **`FROZEN`**；**新增英文冻结声明**（逐字保留）；**Gate 与 `P3` 状态逐项未变**（§0.1 / §5） | `WP3:§0` · §0.1 · §13 · 尾注 |
| **3** | 同步 Review / `docs/README.md` / Entry Gate | ✅ 本报告 **R3**；`docs/README.md` 状态行 + 两行索引；`P3-Entry-Gate-Review.md` **R4**（新增 `C-6` + `DT:WP-3` 说明） | 三文件 |

> **⇒ 全部要求均已落实，且**未**新增越界动作（无实施 / 无 migration / 无 Gate 变更 / 无代码）。**

---

## §10 纪律声明（本轮边界）

- **本文件为 `REVIEW / SELF-CHECK ONLY`**：**不含代码**，**不执行任何实施动作**。
- **本轮未修改** `P3-History-D1-Plan.md` · `Cloud-APP-Platform-Plan.md` · `P3-WP1-*`（含 `Final`）· `P3-WP2-*` · `cloud_protocol.md` · 冻结基线 `P2-0-…freeze.md`。
- **本轮仅对 `P3-Entry-Gate-Review.md` 做 WP-3 状态同步**（人工要求；**未改任何 Gate 状态**）。
- **未修改 `src/`** · `platformio.ini` · `partitions.csv` · 固件行为 · Cloudflare Worker · EMQX rule。
- **未创建 migration**（**未新建 `0003_*`**）· **未写 D1** · **未部署**。
- **未把任何 `NOT RUN` / `BLOCKED` 转为 `PASS`**；`CLOSED` / `DEFERRED` 均系**人工裁决记录**。
- **未自行解除或修改任何 Gate**（`G-1` / `G-2` / `G-3` / `G-4` / `G-5` 状态**均未变**）。
- **未**自行冻结本设计（**`FROZEN` 系人工裁决结果**）。
- **未 `push`**；**未进入下一 WP**。
- **`P3` 状态保持 `NOT STARTED / PLANNED`**。

---

> **`DT:WP-3 = FROZEN`（存储策略设计冻结完成）.**
> **WP-3 Frozen means storage policy design frozen. It does not mean implementation started.**
> **P3 remains NOT STARTED / PLANNED. No Gate changed.**
> **No implementation performed. No migration created.**

---

*本文件于 **2026-10-10** 建立，经 **R2** 与 **R3** 同步，为 **`DT:WP-3` 自检审核报告（`REVIEW ONLY`）**；不含任何实现、迁移或代码改动。*
*被检对象 = `P3-WP3-History-Storage-Policy-Design.md`（**`FROZEN`**）；R1 基线 = 固件仓 HEAD `1fd06c1`；R2 基线 = `9eecaa5`；**R3 基线 = `fd41972`**。*
