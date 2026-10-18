# HD-3 · `H-3-2A` 生产只读执行清单（`APPROVAL PENDING` · `PLAN ONLY`）

> ## 状态：**`PLAN UPDATED`**（**`rev.2`** · `2026-10-18`）· **已获执行授权（6 条语句）**
>
> **`rev.2` 修订（`2026-10-18`，按授权要求，共 5 项）**：
> ① **三块分离判断** —— `ux_lh_dedup` 的**定义核对**（§3.2.1）/ **其他 `UNIQUE` 的影响**（§3.2.2）/
> **`INSERT OR IGNORE` 的忽略归因**（§3.2.3）拆成三节，禁止合并成"schema 通过/不通过"；
> ② **执行计划改为"预期"** —— `E-0`/`E-1`/`E-2` 均标注"未验证"；不符时**唯一动作 = 停止**，
> **不得现场改 SQL 重试**（改写属方案变更）；
> ③ **`Q-1` 明确为完整聚合扫描** —— 13 个指标要求遍历全表，**行数门槛不能保护该查询本身免于扫描**，
> 只能是执行后的"是否继续"判据；
> ④ **`dev_tag` 明确为"稳定截断标签"而非唯一 ID** —— 8/12 hex ≈ 32 bit ⇒ **存在碰撞可能**，
> 报告中不得称其唯一标识设备；
> ⑤ **执行顺序改为授权给定的顺序** —— `E-0 → E-1 → E-2 → Q-0 → Q-1 → Q-2a`（三条 `EXPLAIN` 全部先跑完）。
> 完整 SQL、**6 条**预算、输出字段与停止条件**全部保留未删**。
>
> - **本文件只定义"执行步骤与计数"**，**不定义语义**。语义权威 = `HD-3-Idempotency-Collision-Implementation-Plan.md`
>   **§15**（`rev.2`）。若二者冲突，以 §15 为准，并须报告冲突（不得自行取舍）。
> - **未执行任何生产语句**（`SELECT` / `EXPLAIN` 均为 **0 条**）· 未连接生产 D1 · 未调生产 API / 管理端点 ·
>   未写数据 · 未部署 · 未改 Cron / EMQX · 无设备操作 · 未运行测试 · **无任何 Git 操作**。
> - **本文件不是授权**。批准前不得据此执行任何语句。
> - **门禁未变**：`BT-1` = `OPEN` · `G-4` = `BLOCKED` · `WP-5` 未解锁 · `STOP-R1` 无书面解除。
>   **"是否曾发生真实内容碰撞/丢失" = `UNRESOLVED`**（本轮不改变）。

---

## §1 授权标的与范围

| 项 | 内容 |
|---|---|
| **授权标的** | 在**生产 D1** 上执行 **3 条业务只读查询 + 3 条 `EXPLAIN QUERY PLAN`**，合计 **6 条语句** |
| **查询** | `Q-0`（schema / 唯一约束 / 索引实况）· `Q-1`（全表聚合标量）· `Q-2a`（按 `(device_id, boot_seq)` 聚合） |
| **不在范围** | `Q-2b` / `Q-3` / `Q-4` / `Q-5` / `Q-6` / `Q-7`（§15.4 的其余查询）**一律不执行** |
| **不在范围** | 任何 `INSERT` / `UPDATE` / `DELETE` / `PRAGMA` / `VACUUM` / `migration` / 部署 / Cron |
| **不在范围** | `payload` / `message` / 凭证 / 日志正文的读取（`Q-5` 已被排除） |
| **不在范围** | `mqtt_messages` 的任何查询（`Q-6` 已被排除） |
| **产出** | 一份只读报告：聚合值 + 匿名化的 `(dev_tag, boot_seq, n, seq_min, seq_max)` 清单 + 6 条语句的原始返回 |

> **★ 范围锁**：若执行中发现需要超出上述 6 条语句才能回答，**必须停止并重新申请**，不得"顺手多跑一条"。

---

## §2 语句总清单与计数

| # | ID | 类型 | 语句性质 | 是否访问业务表 | 计入授权 |
|---|---|---|---|---|---|
| **1** | `E-0` | `EXPLAIN QUERY PLAN` | 计划检查（**不执行** `Q-0`） | 否（仅 `sqlite_master`） | ✅ |
| **2** | `E-1` | `EXPLAIN QUERY PLAN` | 计划检查（**不执行** `Q-1`） | 否（只编译计划） | ✅ |
| **3** | `E-2` | `EXPLAIN QUERY PLAN` | 计划检查（**不执行** `Q-2a`） | 否（只编译计划） | ✅ |
| **4** | `Q-0` | `SELECT` | 业务查询 · 元数据 | 否（系统表） | ✅ |
| **5** | `Q-1` | `SELECT` | 业务查询 · 聚合 | **是（完整全表扫）** | ✅ |
| **6** | `Q-2a` | `SELECT` | 业务查询 · 分组 | **是**（预期覆盖索引） | ✅ |

> **★ 顺序由授权直接给定（`rev.2`）**：**三条 `EXPLAIN` 全部先跑完**，确认计划无异常后才开始业务查询。
> 这样任何计划异常都能在**尚未读取任何业务行**时就中止本批。

**计数结论（精确）**

| 口径 | 数量 |
|---|---|
| **业务查询（`SELECT`）** | **3** |
| **执行计划检查（`EXPLAIN QUERY PLAN`）** | **3** |
| **合计语句数** | **6** |
| 写入语句 | **0** |
| 默认不包含的可选项（见 §8） | +1（仅在人工勾选后计入，届时总数 = 7） |

> **说明**：`EXPLAIN QUERY PLAN` **只输出计划、不执行被解释的语句** ⇒ 它不产生行扫描、不改数据。
> 本项目把它**计入授权计数**（不隐去、不合并），因为它是 6 次独立的生产调用。

---

## §3 六条语句（逐条）

### 3.1 `E-0` · 计划检查 for `Q-0`

```sql
EXPLAIN QUERY PLAN
SELECT type, name, tbl_name, sql
FROM sqlite_master
WHERE tbl_name = 'log_history'
ORDER BY type, name;
```

| 项 | 内容 |
|---|---|
| **预期**（**未验证**，仅供比对） | `SCAN sqlite_master`（可能带 `USE TEMP B-TREE FOR ORDER BY`；系统表 < 50 行） |
| 与实际不符时 | **停止并记录实际计划原文**。**不得**"现场调 SQL 再试"（改写 = 方案变更，须另行批准） |
| 成本 | 可忽略；`sqlite_master` 行数 = 本库表数 + 索引数 |
| 输出敏感性 | 计划文本，无数据 |
| 停止条件 | 语句被拒（`SQLITE_AUTH` / `not authorized`）⇒ **停止整批**（连 schema 都无法取证，后续结论无依据） |

### 3.2 `Q-0` · schema / 唯一约束 / 索引实况

```sql
SELECT type, name, tbl_name, sql
FROM sqlite_master
WHERE tbl_name = 'log_history'
ORDER BY type, name;
```

**为什么用一条而不是两条**：`sqlite_master` 中 `tbl_name = 'log_history'` 会**同时**返回
① `type='table'` 的主表 DDL ② 该表上的**全部索引**行（含 `ix_lh_device_ts` / `ux_lh_dedup`）
⇒ 一条语句即可同时确认"表结构"与"索引/唯一约束"，无需拆分。

| 输入列 | 来源 | 用途 |
|---|---|---|
| `sqlite_master.tbl_name` | SQLite 系统表 | 过滤出 `log_history` 的表行与索引行 |
| `sqlite_master.type` | 同上 | 区分 `table` / `index` |
| `sqlite_master.name` | 同上 | 取对象名（`log_history` / `ix_lh_device_ts` / `ux_lh_dedup`） |
| `sqlite_master.sql` | 同上 | **取 DDL 原文**用于逐字比对 `0003` |

| 输出列 | 用途 | 敏感性 |
|---|---|---|
| `type` | 判定表行 / 索引行 | 无 |
| `name` | 判定索引是否存在 | 无 |
| `tbl_name` | 归属确认 | 无 |
| `sql` | **逐字**核对列定义与 `UNIQUE` 关键字 | 无（DDL 不含业务数据） |

**判定规则 —— ★ 必须分三块独立判断，不得合并成一条"schema 通过 / 不通过"**
（`rev.2` 修订：原稿把"唯一键定义""其他 UNIQUE 约束""`OR IGNORE` 归因"混在一张表里，
会让人误以为"唯一索引存在 ⇒ 忽略原因已确定"。三者是三件不同的事。）

#### 3.2.1 唯一键**定义**核对（只回答"键是什么"）

| 期望 | 仓库依据 | 不符时 |
|---|---|---|
| 存在 `type='table'` 且 `name='log_history'` | `0003:72` | **停止整批**（表缺失 ⇒ 所有假设作废） |
| 主表 DDL 含 10 列：`id, device_id, ts, level, code, boot_seq, seq, message, payload, created_at` | `0003:72-108` | **停止**并报告差异（不得按仓库 schema 强行解读后续结果） |
| 存在 `name='ix_lh_device_ts'`，定义含 `(device_id, ts DESC)` | `0003:122` | **停止**（`Q-2a` 的计划前提崩塌） |
| 存在 `name='ux_lh_dedup'`，DDL **含 `UNIQUE`**，列序 `(device_id, boot_seq, seq)` | `0003:126` | **停止整批**（唯一键定义与预期不符 ⇒ 键的语义无法确认） |

> 本块**只**确认"键由哪三列组成、是否唯一"。**它不回答**"忽略是否只由这个键造成"（→ 3.2.2）
> 也**不回答**"被忽略的那条内容是否不同"（→ §15.6 / §6 禁令 4）。

#### 3.2.2 其他 `UNIQUE` 约束的**影响**（只回答"还有没有第二个唯一性来源"）

| 观测 | 处置 | 理由（必须分开陈述，不得省略） |
|---|---|---|
| 除 `ux_lh_dedup` 外，**还存在其它 `sql` 含 `UNIQUE` 的索引** | **停止整批**，报告其 `name`/`sql` 原文 | `INSERT OR IGNORE` 会**吞掉任何**唯一冲突 ⇒ "`changes=0`"将**无法唯一归因**到 `ux_lh_dedup`。**不得**在本次执行中自行论证"该索引的列组合不可能冲突"从而放行 —— 那属设计评估，须人工裁决 |
| 仅存在 `ux_lh_dedup` 一个 `UNIQUE` | 继续（本块通过） | 与 `0003:126` 一致 |
| 存在额外**非 `UNIQUE`** 索引 | **记录并继续**（不阻断） | 只影响查询计划选择，不产生插入冲突；须在报告中列出，且 `E-2` 仍要求走 `ux_lh_dedup` |

#### 3.2.3 `INSERT OR IGNORE` 的**忽略归因**（只回答"原因是否唯一"）

`INSERT OR IGNORE` 会静默忽略**四类**违反，本批只能用 `Q-0` 的 DDL 文本**逐类排除**，**不得**用运行时行为反推：

| # | 可能的忽略来源 | 排除依据 | 无法排除时 |
|---|---|---|---|
| ① | `UNIQUE` 索引冲突 | 3.2.2 通过（仅一个 `UNIQUE`） | 见 3.2.2 ⇒ 停 |
| ② | `NOT NULL` 违反 | `seq`/`boot_seq`/`ts`/`device_id` 在 DDL 中均 `NOT NULL`，且 `normalizeLog` 保证非 `null`（`history-ingest.js:566-640`）、`deviceId` 空串即 400（`:193-197`） | **停止**并报告（可能不止唯一索引吞行） |
| ③ | `CHECK` 约束违反 | 主表 DDL 中**不得出现 `CHECK`** | **停止**并报告 |
| ④ | 主键冲突 | `id INTEGER PRIMARY KEY AUTOINCREMENT` ⇒ 由数据库分配，不可能冲突 | 若 DDL 非 `AUTOINCREMENT` 形态 ⇒ **停止** |

> **★ 归因上限（写死）**：即使 ①②③④ 全部排除，本块也只能得出
> **"`changes = 0` ⟺ `ux_lh_dedup` 键已存在"** —— 这**仍然不等于**"内容相同"。
> 后者需要读取被丢弃的输入，而它**不存在于任何表**（§15.3）。**不得**把本块结果写成"已能识别碰撞"。

| 项 | 内容 |
|---|---|
| 扫描范围 | `sqlite_master`（系统表），**不访问 `log_history` 数据页** |
| 是否全表扫 | 否（"全表"= 系统表，行数 ≤ 数十） |
| 是否回表 / 排序 | 无回表；可能有 `TEMP B-TREE`（见 `E-0`），成本可忽略 |
| 成本依据 | 系统表规模由"表数 + 索引数"决定（本库 4 张 P1 表 + 2 张 P3 表 + 索引），**具体行数待 `Q-0` 输出确认** |
| 停止条件 | 见 §3.2.1 / §3.2.2 / §3.2.3 的**任一**"停止"项 ⇒ **停止整批**，不再执行 `Q-1` / `Q-2a` |
| 执行前需确认 | 该库 `sqlite_master` 是否可读（`migrations/README.md` 的 `V4` 曾以此作为验证 SQL，但**未见其在本库的成功记录**） |

> **★ schema 一致性前提**：仓库 `0003` 是否已**完整**应用到生产，**本轮无法确认**。
> 已有证据只到"生产存在 `log_history` 表且可查"（`RET-1` 收尾时生产 `EXPLAIN … FROM log_history` 返回 168 行，
> `2026-10-18T06:29:46Z`），**索引定义从未在生产侧取证**。
> ⇒ **不得假定生产 schema = 仓库 schema；`Q-0` 的唯一目的就是消除该假定。**

### 3.3 `E-1` · 计划检查 for `Q-1`

```sql
EXPLAIN QUERY PLAN
SELECT COUNT(*)                                        AS rows,
       COUNT(DISTINCT device_id)                       AS devices,
       MIN(created_at)                                 AS ca_min,
       MAX(created_at)                                 AS ca_max,
       SUM(CASE WHEN ts = 0  THEN 1 ELSE 0 END)        AS ts_zero,
       SUM(CASE WHEN ts < 0  THEN 1 ELSE 0 END)        AS ts_neg,
       SUM(CASE WHEN ts > 0  THEN 1 ELSE 0 END)        AS ts_pos,
       SUM(CASE WHEN boot_seq <= 0 THEN 1 ELSE 0 END)  AS boot_nonpos,
       SUM(CASE WHEN seq <= 0 THEN 1 ELSE 0 END)       AS seq_nonpos,
       MIN(boot_seq) AS boot_min, MAX(boot_seq) AS boot_max,
       MIN(seq)      AS seq_min,  MAX(seq)      AS seq_max
FROM log_history;
```

| 项 | 内容 |
|---|---|
| **预期**（**未验证**，仅供比对） | `SCAN log_history`（无 `WHERE`、无 `GROUP BY`、无 `ORDER BY`） |
| **可接受的变体** | 若走 `SCAN log_history USING COVERING INDEX ix_lh_device_ts`（更便宜）⇒ **可接受** |
| 需警惕 | 出现 `USE TEMP B-TREE FOR COUNT(DISTINCT)` ⇒ **可接受**（仅 1 次，`N` 小），但须记录；若出现**第 2 个** `TEMP B-TREE` ⇒ **停止并报告**（说明计划偏离） |
| 与实际不符时 | **停止并记录实际计划原文**。**不得**改写 SQL（如把 `COUNT(DISTINCT)` 拆出去）后继续 —— 那是另一份方案 |
| 成本 | 一次**完整聚合扫描** + 逐行解码 6 列 ⇒ `O(N)`；**无法用 `WHERE` 收窄**（统计定义要求全表） |
| **★ 门槛的性质（`rev.2` 明确）** | 本查询的 13 个指标（含 `MIN`/`MAX`/`SUM` 与 `COUNT(DISTINCT)`）要求**遍历全表每一行** ⇒ **没有任何行数门槛能保护这条查询本身免于扫描**。`rows > 1_000_000` 只是**执行后的**判据，只能决定"**是否继续跑后续查询**"，不能降低本条已发生的扫描成本。⇒ 本条在批准时即应被视为"**无条件全表扫**" |
| 输出敏感性 | 全部为聚合标量，**不含 `device_id`、不含正文** |
| 停止条件 | ① `rows > 1_000_000` ⇒ 停 ② 任一语句返回 `meta.changed_db === true` 或 `meta.rows_written !== 0` ⇒ **立即停**（只读失效，属重大异常） ③ `rows = 0` ⇒ 记录"表为空"，**跳过 `Q-2a`**（无意义） |

### 3.4 `Q-1` · 全表聚合标量

```sql
SELECT COUNT(*)                                        AS rows,
       COUNT(DISTINCT device_id)                       AS devices,
       MIN(created_at)                                 AS ca_min,
       MAX(created_at)                                 AS ca_max,
       SUM(CASE WHEN ts = 0  THEN 1 ELSE 0 END)        AS ts_zero,
       SUM(CASE WHEN ts < 0  THEN 1 ELSE 0 END)        AS ts_neg,
       SUM(CASE WHEN ts > 0  THEN 1 ELSE 0 END)        AS ts_pos,
       SUM(CASE WHEN boot_seq <= 0 THEN 1 ELSE 0 END)  AS boot_nonpos,
       SUM(CASE WHEN seq <= 0 THEN 1 ELSE 0 END)       AS seq_nonpos,
       MIN(boot_seq) AS boot_min, MAX(boot_seq) AS boot_max,
       MIN(seq)      AS seq_min,  MAX(seq)      AS seq_max
FROM log_history;
```

**输入字段（全部来自 `log_history`，逐列核对 `0003:72-108`）**

| 列 | 类型 / 约束 | 本查询用途 |
|---|---|---|
| `device_id` | `TEXT NOT NULL` | `COUNT(DISTINCT …)` ⇒ 设备数（**不输出任何具体值**） |
| `created_at` | `INTEGER NOT NULL DEFAULT (unixepoch())`（**秒**） | 观测窗口 `ca_min` / `ca_max` |
| `ts` | `INTEGER NOT NULL`（**秒**；可为 `0` / 负） | `ts` 三态计数 |
| `boot_seq` | `INTEGER NOT NULL` | 非正值计数 + 值域 `boot_min`/`boot_max` |
| `seq` | `INTEGER NOT NULL` | 非正值计数 + 值域 `seq_min`/`seq_max` |
| `level` / `code` / `message` / `payload` / `id` | — | **不读取**（不进入任何表达式） |

**输出字段与用途**

| 输出 | 回答什么 | 敏感性 |
|---|---|---|
| `rows` | 表规模 ⇒ 后续成本与停止门槛的唯一依据 | 无 |
| `devices` | 有多少设备在表内 | 无 |
| `ca_min` / `ca_max` | 观测窗口（UTC 秒） | 无 |
| `ts_zero` / `ts_neg` / `ts_pos` | `ts=0` 的**规模** | 无 |
| `boot_nonpos` / `seq_nonpos` | 键字段是否存在 `0` / 负数输入 | 无 |
| `boot_min` / `boot_max` / `seq_min` / `seq_max` | 键值域（供 `Q-2a` 交叉核对） | 无 |

**判读红线（写死在清单里，防止报告越界）**

- `ts_zero > 0` ⇒ **只能**写"时间未同步确有发生"。**不得**写"与 log 碰撞有关"（`ts` 不在 `ux_lh_dedup` 内，`0003:126`）。
- `boot_nonpos > 0` / `seq_nonpos > 0` ⇒ **只能**写"键值域存在异常输入，可能放大碰撞面"。
- **空表边界**：`rows = 0` 时各 `SUM(...)` 返回 `NULL`（不是 `0`）⇒ 以 `rows = 0` 判定，**不得**把 `NULL` 读成"无异常"。

### 3.5 `E-2` · 计划检查 for `Q-2a`

```sql
EXPLAIN QUERY PLAN
SELECT substr(device_id, 1, 6) || '..' || substr(device_id, -2) AS dev_tag,
       boot_seq,
       COUNT(*)  AS n,
       MIN(seq)  AS seq_min,
       MAX(seq)  AS seq_max
FROM log_history
GROUP BY device_id, boot_seq
ORDER BY device_id, boot_seq;
```

| 项 | 内容 |
|---|---|
| **预期**（**未验证**，仅供比对） | detail 含 **`COVERING INDEX ux_lh_dedup`**（`device_id, boot_seq, seq` 三列全在索引内 ⇒ 不回表） |
| **也视为通过** | detail 含 `COVERING INDEX` 且索引名为 `ux_lh_dedup`（大小写/措辞差异不算偏离，但须原文记录） |
| **必须停止** | detail 出现 `USING INDEX ix_lh_device_ts`（非 covering ⇒ 回表取 `seq`） |
| **必须停止** | detail 出现 `USE TEMP B-TREE FOR GROUP BY`（说明未利用索引有序性） |
| **必须停止** | detail 出现 `SCAN log_history` 且**无**任何索引限定（全表扫） |
| 成本 | `O(N)` 索引扫描，**不读表体**；`ORDER BY` 与 `GROUP BY` 同序 ⇒ 预期无额外排序 |
| 输出敏感性 | 计划文本，无数据 |
| 停止条件 | 上述任一"必须停止"命中 ⇒ **停止并报告原计划原文**，**不得改写 SQL 绕过**（改写属方案变更，须重新申请） |
| **★ "预期"的含义** | 本表是**代码与索引结构推出的预期**，**不是**已验证事实 ⇒ 实际计划以 `E-2` 输出为准；**若实际与预期不符，只有"停止"一个动作**，不存在"现场调优"选项 |

### 3.6 `Q-2a` · 按 `(device_id, boot_seq)` 聚合（匿名化）

```sql
SELECT substr(device_id, 1, 6) || '..' || substr(device_id, -2) AS dev_tag,
       boot_seq,
       COUNT(*)  AS n,
       MIN(seq)  AS seq_min,
       MAX(seq)  AS seq_max
FROM log_history
GROUP BY device_id, boot_seq
ORDER BY device_id, boot_seq;
```

**关键设计说明（两条，缺一即错）**

1. **`GROUP BY` 用原列 `device_id`，`SELECT` 才输出前缀** ⇒ 分组**不受掩码影响**（若按掩码分组，两台设备可能被合并统计）。
2. **`dev_tag` 是"稳定截断标签"，不是严格唯一的匿名 ID**：同一 `device_id` 在任何一次执行、任何一条结果中
   恒得同一个 `dev_tag`（纯函数式 `substr`，无随机、无 `datetime`、无 `random`）；但
   前缀 6 hex + 尾 2 hex = **8/12 hex ≈ 32 bit 空间** ⇒ **存在碰撞可能**：
   **两个不同 `device_id` 可以映射到同一个 `dev_tag`**。因此：
   - 报告中**不得**写"`dev_tag` 唯一标识一台设备"，只能写"**同一 `dev_tag` 的行来自同一台或**（极少见）**碰撞的多台设备**"；
   - 若出现"同一 `dev_tag` 但 `boot_seq` 序列明显不属于同一台设备"（如 `boot_seq` 同时出现两组互不相交的连续段）
     ⇒ 标 `dev_tag 疑似碰撞`，**报告但不改 SQL、不补查**；
   - 若需要更强的唯一性，属**另一份方案**（如加长前缀），须另行申请，本批不得改。

| 输入列 | 来源 | 用途 |
|---|---|---|
| `device_id` | `log_history.device_id`（`TEXT NOT NULL`） | **分组键** + 掩码来源 |
| `boot_seq` | `log_history.boot_seq`（`INTEGER NOT NULL`） | 第二分组键 + 直接输出（非敏感） |
| `seq` | `log_history.seq`（`INTEGER NOT NULL`） | `MIN` / `MAX` |

| 输出列 | 用途 | 敏感性 |
|---|---|---|
| `dev_tag` | 稳定的匿名设备标识（**不可反推完整 ID**） | 低（**已掩码**） |
| `boot_seq` | 序号重放形态的第一观察维度 | 无 |
| `n` | 该 `(dev, boot)` 的行数 | 无 |
| `seq_min` / `seq_max` | 该 `(dev, boot)` 的序号区间 ⇒ 与 `n` 比较可看缺口 | 无 |

**明确不输出**：`device_id` 全文 · `ts` · `level` · `code` · `message` · `payload` · `created_at` · `id`。

| 项 | 内容 |
|---|---|
| 成本 | 预期 `O(N)` 覆盖索引扫描；**不得回表** |
| 排序 | `GROUP BY` / `ORDER BY` 同序 ⇒ 预期无 `TEMP B-TREE` |
| 停止条件 | ① `E-2` 命中"必须停止"⇒ 停 ② 返回行数 > **500**（`devices × boots` 异常多）⇒ 停并报告 ③ `meta.changed_db === true` ⇒ 立即停 |
| **判读红线** | 出现同一 `dev_tag` 的**重复 `boot_seq`** 或 `seq` 区间异常 ⇒ **只能**写"疑似的重放形态（线索）"，**不得**写"发生碰撞 / 丢失日志" |

---

## §4 执行顺序与前置条件

### 4.1 顺序（**严格串行，任一步停止即终止整批**）

```
E-0 → E-1 → E-2 →［G0：三条计划全部符合预期？］
    → Q-0 →［G1：schema 三块判定通过？］
    → Q-1 →［G2：rows 门槛 / 只读自证通过？］
    → Q-2a →［G3：分组数 ≤ 500？］→ 出报告
```

| 门 | 卡点 | 不通过时 |
|---|---|---|
| **G0** | `E-0`/`E-1`/`E-2` **三条全部**返回且计划符合 §3.1/§3.3/§3.5 的预期（`E-2` 须含 `COVERING INDEX ux_lh_dedup`） | **停止整批**——此时**尚未读取任何业务行**，成本最低。**不得**改写 SQL 后重试 |
| **G1** | `Q-0` 通过 §3.2.1 全部项 + §3.2.2（无第二个 `UNIQUE`）+ §3.2.3（`NOT NULL`/无 `CHECK`/`AUTOINCREMENT` 可排除） | **停止整批**，报 `BLOCKED` |
| **G2** | `Q-1` 的 `rows ≤ 1_000_000` 且 `meta.changed_db = false` 且 `rows_written = 0` | **停止**（`rows` 门槛**事后**生效，不影响 `Q-1` 已完成的扫描） |
| **G3** | `Q-2a` 行数 ≤ 500 | **停止**并报告（其后无更多查询，故仅影响分析范围） |

### 4.2 执行环境前置（**均需在批准时一并确认**）

| # | 前提 | 现状 | 未确认时的处置 |
|---|---|---|---|
| `P-1` | 目标库 = `guofeeder`（`database_id = 0d8385ba-a1a6-4fcc-8e43-ae61c014c43d`，绑定 `guofeeder_DB`） | 仓库 `wrangler.toml:9-15` 已载明 | 未确认 ⇒ **不得执行** |
| `P-2` | `wrangler` 凭据仍有效且对该库有**读**权限 | 历史事实：`wyl94@foxmail.com` 具备 d1 权限（**凭证时效未在本轮验证**） | 401/403 ⇒ **停止**，不改用其他通道 |
| `P-3` | 工作目录必须是 `Cloudflare_Assets/guo-feeder-api`（`--config wrangler.toml` 相对路径） | 项目约定（诊断脚本同此） | 目录错 ⇒ 可能指向错误库，**禁止猜测** |
| `P-4` | 需 `unset http_proxy https_proxy HTTP_PROXY HTTPS_PROXY ALL_PROXY all_proxy` | 本机沙箱会注入代理，干扰 `wrangler` | 未清 ⇒ 网络错误，**不得重试绕过** |
| `P-5` | `sqlite_master` 可读（`Q-0` 依赖） | **未验证**（`PRAGMA` 类有被拒先例，`sqlite_master` 未见成功记录） | 被拒 ⇒ `Q-0` = `UNRESOLVED`，**停止整批** |

### 4.3 命令形态（**供审批人审阅，本轮不执行**）

```bash
cd D:/Guo_Feeder_Project/Cloudflare_Assets/guo-feeder-api
unset http_proxy https_proxy HTTP_PROXY HTTPS_PROXY ALL_PROXY all_proxy
node ../node_modules/wrangler/bin/wrangler.js d1 execute guofeeder \
     --remote --config wrangler.toml --json --command "<上表 SQL 原文>"
```

> **禁止**：`--file`（多语句）、`--local`（会给出本地假象）、`--preview`、任何 `wrangler d1 execute --command "…; …"` 的复合语句。

---

## §5 输出与隐私规则

| 规则 | 要求 |
|---|---|
| `device_id` | **必须掩码**为 `dev_tag`（= 前 6 位 + `..` + 末 2 位，SQL 见 §3.6）；同一设备**跨结果保持同一 `dev_tag`** |
| 反向映射表 | **不得**写入任何文档 / 报告 / 提交；如需保留，只留在审批人本地 |
| 日志正文 | **不输出** `message` / `payload`（本批 3 条查询均不读取这两列） |
| 凭证 | 本批不涉及任何凭证列；若 `Q-0` 的 DDL 意外含敏感默认值 ⇒ 记录前先由人工判断 |
| 汇总口径 | `Q-1` **只输出聚合标量**，无任何逐行数据 |
| 无关设备 | 不额外查询 `device` / `device_credential` / `mqtt_messages` 等表；不输出与本任务无关的设备数据 |
| 原始返回 | 6 条语句的原始 `--json` 输出**完整留档**（含 `meta.rows_written` / `meta.changed_db`），作为零写入自证 |

---

## §6 结果解释边界（**五条禁令，违反即报告作废**）

1. **`ts = 0` 的存在，不能证明 log 唯一键碰撞。** 唯一键 = `(device_id, boot_seq, seq)`，**`ts` 不在键内**（`0003:126`）。
2. **同一设备出现重复 `boot_seq`，或 `seq` 出现断层，只能作为调查线索。** 断层也可能来自"第二次 boot 的 `seq` 超出原区间、全部成功插入"（该情形**没有丢失**）。
3. **查询未发现异常，不能证明没有发生过历史内容碰撞或数据丢失。** `log_history` 只保存胜者，`INSERT OR IGNORE` 丢弃的行**零留存** ⇒ 观测盲区，不是阴性证据。
4. **即便确认 `INSERT OR IGNORE` 因唯一键冲突忽略了输入，也不能仅凭此区分"合法重试"与"不同内容的碰撞"。** `duplicate = rows.length - inserted`（`history-ingest.js:319`）不分原因。
5. **本轮统计结果不得单独把风险等级改为** `CONFIRMED` / `NOT REPRODUCIBLE` / `PASS` / `CLOSED`。
   允许的措辞上限：**"在该观测窗口内（未）观察到重放形态"**。

---

## §7 未解决前提（批准前需一并确认）

| # | 未解决项 | 影响 | 建议 |
|---|---|---|---|
| `U-1` | **生产 schema 是否与仓库 `0003` 逐字一致** | 决定 `Q-1`/`Q-2a` 的字段解读是否成立 | 由 `Q-0` 回答；**在此之前不得假定一致** |
| `U-2` | `sqlite_master` 在生产是否可读 | 决定 `Q-0` 能否完成 | 若被拒 ⇒ `BLOCKED` |
| `U-3` | 生产 `log_history` 当前**精确行数** | 决定 `Q-1` 的真实成本与是否触及 100 万门槛 | 由 `Q-1` 的 `rows` 回答；**不得预填** |
| `U-4` | `Q-2a` 是否真走 `COVERING INDEX ux_lh_dedup` | 决定是否回表 | 由 `E-2` 回答；不符即停 |
| `U-5` | `wrangler` 凭据时效 | 决定能否连上 | 批准时确认 |
| `U-6` | 执行时点是否与 `03:00Z` 的 `scheduled` retention 重叠 | 重叠期间表内容可能被清理 ⇒ 统计口径漂移 | **避开 `02:55–03:10Z` 窗口**（`log` 保留期 180 d，实际删除量预期为 0，但避免争议） |

> **成本量级的历史依据（非本轮实测，仅作量级参考）**：
> `RET-1` 收尾时生产 `log_history` = **168 行**（`2026-10-18T06:29:46Z`，`168 → 168`，`rows_written=0`）；
> `WP-6` 实测**单设备**典型 **57 行/天**（1 台设备 · 5 天）。
> ⇒ 当前量级为 **10² 行**，`Q-1` / `Q-2a` 的全表/索引扫描预期为**秒级以下**。
> **但这两个数字都不是本轮实测，不得写进结果报告作为"当前行数"**；`Q-1` 的 `rows` 才是。

---

## §8 默认**不包含**的可选项（需单独勾选）

| 选项 | 语句 | 何时需要 | 默认 |
|---|---|---|---|
| `OPT-1` | 在 `Q-2a` 之后再执行一次 `Q-1`（**+1 条 SELECT**），比对 `rows` 前后一致 | 需要"行数守恒"的强自证时 | ❌ 不含（`meta.changed_db` 已足够） |
| `OPT-2` | `Q-2b`（`created_at` 跨度） | `Q-2a` 出现可疑形态后 | ❌ 不含（本批范围外） |
| `OPT-3` | `Q-3`（断层检测） | `Q-2b` 命中后 | ❌ 不含 |
| `OPT-4` | `Q-4`（定点脱敏形态） | `Q-2a` 命中可疑 `boot` 后 | ❌ 不含 |

> 勾选 `OPT-1` 后**总语句数 = 7**（4 SELECT + 3 EXPLAIN）；其余选项属**另一批授权**。

---

## §9 边界自检（本文件编写过程）

| 项 | 状态 |
|---|---|
| 生产 `SELECT` 执行次数 | **0** |
| 生产 `EXPLAIN` 执行次数 | **0** |
| 连接生产 D1 / 调用生产 API / 管理端点 | **无** |
| 写入 / migration / 部署 / Cron / EMQX | **无** |
| 设备命令 / 重启 | **无** |
| 产品代码 / 测试 / 配置改动 | **零** |
| 测试运行 | **无** |
| Git（`commit`/`push`/`reset`/`clean`/`stash`/`checkout`/切分支/`rebase`/`merge`/`cherry-pick`） | **未执行** |
| `RET-1` 验收 / `WP-6` 文档 | **未修改**  |
| README | **未修改**（遵嘱） |
| 风险等级 | **未变**：`log` / `telemetry` = `CODE-PROVEN RISK`；"是否实际发生" = `UNRESOLVED` |
| 门禁 | `BT-1 OPEN` · `G-4 BLOCKED` · `WP-5` 未解锁 · `STOP-R1` 无解除 —— **均未变** |

---

## §10 审批签字区

| 项 | 选择 |
|---|---|
| **批准 `H-3-2A` 执行本清单（6 条语句）** | ☐ **批准**　☐ **驳回**　☐ **附条件**（条件：__________） |
| 语句总数确认（**3 SELECT + 3 EXPLAIN = 6**） | ☐ 已知悉（`EXPLAIN` 已计入，未隐去） |
| `Q-0` 若发现 schema 不符 ⇒ **停止整批** | ☐ 已知悉 |
| `Q-2a` 若未走 `COVERING INDEX ux_lh_dedup` ⇒ **停止不改写** | ☐ 已知悉 |
| 100 万行门槛仅为初步值，**低于门槛不代表安全** | ☐ 已知悉 |
| `device_id` 掩码规则（`substr(1,6)..substr(-2)`） | ☐ 同意　☐ 改为：__________ |
| 是否勾选 `OPT-1`（+1 条，总 7 条） | ☐ 不勾　☐ 勾选 |
| 执行时点避开 `02:55–03:10Z` | ☐ 已知悉 |
| **本批准 ≠ 部署授权 ≠ 修复授权 ≠ 改变风险等级** | ☐ 已知悉 |
| 签字 / 日期 | ____________________ |

> **申请方声明**：本文件提交后**立即停止**。未获书面批准前，
> 生产语句执行次数保持 **0**；不连接生产、不执行 SQL、不做任何 Git 操作。
