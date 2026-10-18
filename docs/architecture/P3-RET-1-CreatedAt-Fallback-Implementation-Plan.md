# P3 · `RET-1` 实施任务单 —— `created_at` 兜底（方案 B）

> ## 状态：**`rev.5` · 实现与范围 = `ACCEPTED WITH KNOWN LIMITATIONS`（人工终审已确认）· 生产部署与行为验收 = `DEFERRED` · `H-RET-4` 本轮不批 · NOT COMMITTED**
>
> - **★ `rev.5`（`2026-10-18` 终审）**：人工确认 `ACCEPTED WITH KNOWN LIMITATIONS`，**严格限定为「实现与范围验收通过」**，
>   **不得解释为生产修复已完成**。正式交接记录 = `docs/review/RET-1-Final-Read-Only-Acceptance.md` **§9**；
>   本文 **§14** 为其在本任务单内的镜像。**`RET-1` 专项已结束**，不再主动扩展调查。
> - **★ 对外唯一口径**：**实现完成，未部署，生产风险仍在。**（❌ 不得写 `PASS` / `CLOSED` /「已修复」）
> - **设计已获批**（人工 `2026-10-18`）：`RET-1` 选 **方案 B**，并批准复议 `WP-7 · Q-2` 与 `DT:WP-3 · D-3`。
> - **★ `rev.3`（`2026-10-18`）设计侧已无未决项**：
>   - **`H-RET-1` = ✅ `APPROVED`**：**仅修改 `log_history` 的 retention 语义**；`telemetry_history` **保持不变**。
>   - **`H-RET-2` = ✅ `APPROVED` = B-1**：`log_history` 中 `ts > 0` 按 `ts` 判断过期；**`ts <= 0`（含负数）按 `created_at` 判断过期**。
>   - **效力边界（逐字）**：**本次仅批准设计语义，不代表批准修改代码或部署。**
> - **★ `rev.4`（`2026-10-18` · 实施完成）**：`H-RET-3` = **`APPROVED WITH CONSTRAINTS`** ⇒ 按批准范围实施完毕
>   （`src/retention.js` + 2 个测试文件 + 本文档）。**实施记录见 §13**（九节 + 偏差说明）。
>   **仍未部署**（生产仍是旧谓词）· **未 commit**（工作区变更保留）。
> - **部署未授权**：生产仍为旧谓词；**不得** `wrangler deploy`（`H-RET-4` **本轮不批准**）；**测试全绿 ≠ 部署授权**。
>
> **上位文档**：`docs/review/RET-1-Retention-Time-Boundary-Investigation.md`（调查 · `rev.3`）·
> `docs/architecture/P3-WP3-History-Storage-Policy-Design.md` **§7.4.1**（`D-3` 修订 · `rev.3`）·
> `docs/architecture/P3-WP7-Implementation-Record.md` **§2.1**（`Q-2` 改判）

---

## §1 背景与目标

| 项 | 内容 |
|---|---|
| **问题** | `ts = 0`（设备尚未同步时间）的记录被 `ts < cutoff` 判为超窗 ⇒ **每轮清理必删** |
| **已发生损失** | `2026-10-18 03:00Z`：`log_history` 的 `ts=0` 行 **26 → 0**，其中 **3 条 `CRITICAL`**（`code` 未留档 ⇒ **不可恢复**） |
| **目标** | `log_history` 的 `ts<=0` 改按 `created_at` 判超窗 ⇒ 开机/断网期的关键日志**至少保留一个完整窗口（180 d）** |
| **非目标** | ❌ 不恢复已删数据 · ❌ 不改窗口值（90/180）· ❌ 不改索引/migration · ❌ **不改 `telemetry_history`（`H-RET-1` 已裁决 = 保持不变）** · ❌ 不改固件 |
| **范围（★ `rev.3` 定稿）** | **`log_history` 单表**。`telemetry_history` 的 DELETE 语句**逐字不变**（仍 `WHERE device_id = ? AND ts < ?`，2 个绑定参数） |
| **`ts<0`（★ `rev.3` 定稿）** | **B-1**：与 `ts=0` **同支**，统一 `created_at < cutoff`。谓词以 `ts <= 0` 一支显式覆盖 |

---

## §2 `created_at` 证据（本轮实测，`2026-10-18` · 只读）

### 2.1 结构与语义

| 项 | 值 | 判读 |
|---|---|---|
| 列定义 | `created_at INTEGER NOT NULL DEFAULT (unixepoch())` | `migrations/0003_p3_history_tables.sql` · **已上线，不改** |
| 实际 NULL | **0** 行 | 非空约束成立 |
| `created_at <= 0` | **0** 行 | 无异常值 |
| 时间轴 / 单位 | **UTC Unix 秒**，与 `ts` **同轴同单位** | 可直接与 `cutoff` 比较，**无时区换算** |
| 生成方 | **D1（服务端）** | 与 `cutoff`（`controller.scheduledTime`，同为服务端）**同钟比较** |
| 覆盖 | `MIN = 1792085491`（`2026-10-15T17:31:31Z`）· `MAX = 1792298739` | 全表 166 行均有值（列有 DEFAULT，历史行**不缺失**） |

### 2.2 「只会延长、不会提前删除」的验证

**先给结构论证（比样本更强）**：`created_at` 是**入库时刻**，而入库必然发生在**事件之后**；
`cutoff` 是服务端"现在 − 180 d"。两者**同钟**。⇒ 删除条件 `created_at < cutoff` 成立的时刻
**必然晚于** `事件时刻 + 180 d` ⇒ **保留时长 ≥ 180 d**，不可能提前。

**再用实测校核（`ts > 0` 的 166 行）**：

| 指标 | 值 |
|---|---|
| `created_at < ts` 的行数 | **0**（无"负数滞后"） |
| 滞后 `created_at − ts` | `min = 0` · `avg = 25,663 s`（≈7.1 h）· `max = 770,260 s`（**≈8.9 d**） |
| 分布 | `<60 s`：97 行 · `60–600 s`：46 行 · `600–3600 s`：12 行 · **`≥1 d`：11 行**（离线补传） |
| `created_at == ts` | 9 行 |

**结论**：① 方向**单调安全**（0 例负滞后）；② **最坏观测滞后 ≈ 8.9 d** ⇒ 方案 B 在离线补传场景下
最多把保留期**多延 ~9 天**（方向安全，但须知情）。

> ⚠️ **证据等级**：以上为**单设备、166 行**样本（`PROXY`）。
> 但 §2.2 的**结构论证**不依赖样本量 ⇒ 「只会延长」的结论等级 = **`PROVEN（逻辑）`**，「最多延 ~9 d」= **`PROXY`**。

---

## §3 允许 / 禁止修改范围

| | 内容 |
|---|---|
| **允许（开工后）** | `Cloudflare_Assets/guo-feeder-api/src/retention.js`（**唯一产品代码文件**）· `test/current/retention_policy.test.mjs`（**含 stub 改造**）· `test/real_d1/retention_real_d1.sh` · 必要时 `test/real_d1/fixtures/**` · 本任务单的"实施记录"章节 |
| **禁止** | ❌ `migrations/**`（**不得新增/修改**）· ❌ `wrangler.toml`（不新增 `RETENTION_*` 生产变量）· ❌ `src/index.js` 的 `scheduled()` 签名 · ❌ `history-ingest.js` / `log-ack.js` · ❌ 固件仓 · ❌ EMQX 仓 · ❌ `telemetry_history` 的谓词（**`H-RET-1` 已裁决 = 保持不变**） |
| **禁止动作** | 部署 · 生产 D1 写入 · 手工补删/补写 · 用生产测试帧"验证" · 重跑生产验收并沿用旧 `PASS` |

---

## §4 功能要求（`F-*`）

| # | 要求 |
|---|---|
| **F-1** | `log_history` 删除谓词改为：`device_id = ? AND ( (ts > 0 AND ts < ?) OR (ts <= 0 AND created_at < ?) )`，`LIMIT ?` 保持 |
| **F-2** | `ts > 0` 的行为**逐字不变**（同 cutoff、同分批、同收敛判定） |
| **F-3** | `ts = 0` 的记录：`created_at >= cutoff` ⇒ **保留**；`created_at < cutoff` ⇒ 删除 |
| **F-4** | `ts < 0` **按 `H-RET-2` = B-1 实现**：与 `ts = 0` **完全同一判据**（`created_at < cutoff`）。**不得遗漏该分支**（遗漏 ⇒ 永久保留）；**不得**写成独立 `OR (ts < 0)` 分支（实测使计划退化为 `ux_lh_dedup`） |
| **F-5** | `telemetry_history` 的 SQL **保持原样**（2 个绑定参数）⇒ 需按表分支构造 SQL，**不得**再用单一模板字符串 |
| **F-6** | 参数绑定顺序必须与 SQL 占位符一一对应（`log`：`deviceId, cutoff, cutoff, batchSize`） |
| **F-7** | 收敛判定 `n < batchSize ⇒ 该设备本表删净` **不变**（OR 形态下语义仍成立） |
| **F-8** | 摘要行（`logRetentionSummary`）**不得新增**语义不明的字段；若需暴露新判据，须显式标注（如 `predicate=ts|ts+created_at`） |
| **F-9** | **★ `created_at` 的量纲必须是 UTC Unix 秒**（与 `ts`、`cutoff` 同轴同单位）。实现**不得**引入任何 `/1000`、`*1000`、时区换算或 `Date` 对象运算 —— `controller.scheduledTime`（毫秒）**只允许**在既有的 `nowSec` 派生处出现一次 |
| **F-10** | **★ `created_at` 的异常值（`<= 0`）按数值参与比较** ⇒ `0 < cutoff` ⇒ **删除**。**不得**为其开"永久保留"特例（那会成为新的无界增长面）。该分支必须有显式测试（`T-27`） |

## §5 非功能要求（`N-*`）

| # | 要求 |
|---|---|
| **N-1** | **索引**：`log_history` 的删除**必须**继续命中 `ix_lh_device_ts`。**禁止** `COALESCE(NULLIF(ts,0),created_at)` 形态（实测退化为 `ux_lh_dedup` 全设备扫） |
| **N-2** | **查询计划须以真实 `EXPLAIN QUERY PLAN` 取证**（写进实施记录，含逐行 plan 文本），**不得只凭本文档的推断** |
| **N-3** | **预算不变**：单表 DELETE 调用 ≤ `MAX_CALLS_PER_TABLE`（默认 20）· 正常 `Total ≤ 42` · `HARD_STOP = 45` 仍不可达 |
| **N-4** | **确定性**：谓词只依赖"行 + cutoff"，无随机 / 无 `now()` 竞态 / 无行序依赖 |
| **N-5** | **可回滚**：单文件（`retention.js`）+ 测试；回滚 = 还原该文件（不涉及 schema / 数据） |
| **N-6** | **无新增配置**：不引入 `RETENTION_TS0_DAYS` 之类参数（那是方案 C；本轮选 B） |
| **N-7** | **★ 旧 `PASS` 一律作废**：`WP-7` 的 L1 `99 PASS` / L2 `89 PASS` / `W7-*` `38/38 PASS` **全部验证的是旧谓词**。新语义落地后**必须**重跑**全套**相关套件并重新取证；**禁止**在报告里引用旧 `PASS` 作为新语义的证据（详见 §12） |
| **N-8** | **★ 测试替身同步**：L1 的 stub D1 **本身编码了旧语义**（§12.2）⇒ 必须同批改造；否则新 SQL 的 4 个绑定参数会被按 3 个解构，产生**假 PASS / 假 FAIL**。改造后 stub 仍需按"表"区分谓词 |

---

## §6 实施步骤

| 步 | 内容 | 产出 | 门 |
|---|---|---|---|
| **S0** | **等 `H-RET-1` / `H-RET-2` 书面裁决** | 裁决记录追加到 `Q-2 §2.1(5)` | ✅ **已满足（`2026-10-18`）**：仅 `log_history` · `ts<0` = B-1 |
| **S0.5** | **等 `H-RET-3` 实现授权** | 独立授权申请 `P3-RET-1-Implementation-Authorization-Request.md` | ⛔ **未授予 ⇒ 未开工** |
| **S1** | 把 `deleteSql` 改为**按表分支**（`buildDeleteSql(table)` + `buildDeleteArgs()`） | 代码 diff（仅 `retention.js`） | — |
| **S2** | 在**本地 D1**（`--local`，非生产）对**两种形态**（旧谓词 / 新 OR 谓词）跑 `EXPLAIN QUERY PLAN` | 计划文本逐行写入实施记录 | `N-2` / `T-31` |
| **S2.5** | 在**生产只读**环境跑同一 `EXPLAIN QUERY PLAN`（**只读 · 零写入**） | 计划文本逐行写入实施记录 | `T-33` |
| **S3** | L1 断言改造 + 新增（见 §7） | 测试 diff + 全绿输出 | `F-1…F-4` |
| **S4** | L2 真实 D1 引擎（`real_d1`）边界用例 | 脚本输出 | `F-3`/`N-3` |
| **S5** | `run_all` + `deploy --dry-run`（**不部署**） | 通过数 + bundle 大小 | 构建门 |

---

## §7 测试矩阵（`T-*`，全部为**开工后**要求）

### 7.1 L1（`test/current/retention_policy.test.mjs`）

| # | 用例 | 期望 |
|---|---|---|
| **T-1** | `ts = cutoff - 1`（`ts>0`） | **删除**（旧行为不变） |
| **T-2** | `ts = cutoff`（严格 `<`） | **保留** |
| **T-3** | `ts > cutoff`（未来时间保护） | **保留** |
| **T-4** | `ts = 0`，`created_at = cutoff - 1` | **删除** |
| **T-5** | **★ `ts = 0`，`created_at = cutoff`** | **保留**（边界） |
| **T-6** | **★ `ts = 0`，`created_at = cutoff + 1`** | **保留** |
| **T-7** | **★ `ts = 0` 且 `created_at` 远新于 cutoff**（模拟开机期记录） | **保留**（这是 `RET-1` 的核心断言） |
| **T-8** | `ts < 0` —— **按 B-1**：与 `ts = 0` **结果必须一致** | 见 §7.1.1 的 `T-20…T-24` + `T-24b`（**不是一条**，是 6 条断言） |
| **T-9** | `telemetry_history` 的 `ts=0` | **仍删除**（`H-RET-1` 已定 = 单表范围） |
| **T-10** | 同一设备混合 4 类行，一次批次内 | 只删应删的；`deleted` 计数精确 |
| **T-11** | `batchSize` 边界（恰好等于 / 小于） | 收敛判定正确（`n < batchSize ⇒ done`） |
| **T-12** | 预算：>= `maxCallsPerTable` | `truncated=true`，**不超预算** |

> **★ `rev.3` 一致性修正（重要）**：既有断言 `L1【1】"ts=0 被删"`（`retention_policy.test.mjs:194/209/210`）
> 作用对象是 **`telemetry_history`**（`left = stub.tables.telemetry_history`）⇒ 在 **`H-RET-1` = 仅 `log_history`**
> 的裁决下，它**验证的仍是有效语义**，因此 **必须保持原样、不得修改**（改了反而与裁决冲突）。
> `rev.2` 曾要求"修正该断言"，属**在 `H-RET-1` 未裁决时的保守假设**，现按裁决**撤销该要求**。
> ⇒ 真正需要新增的是 **`log_history` 侧的 `T-4…T-8` 独立断言组**（新组，不改旧组）。

### 7.1.1 ★ `ts < 0` 显式用例（`H-RET-2` = B-1 已裁决 · **5 条正向 + 1 条反向 = 6 条断言**）

> **设计要点**：B-1 的语义是"`ts<=0` 与 `ts=0` **同一判据**"，因此**不能**只测一条"负数被删"。
> 必须证明"负数**跟随 `created_at`**，而不是跟随数值大小"——否则写成 `OR (ts<0)` 立即删也能"通过"。

| # | 输入（`log_history`） | 期望 | 该条防什么 |
|---|---|---|---|
| **T-20** | `ts = -1` · `created_at = cutoff - 1` | **删除** | 负数不会因"两支都不匹配"被永久保留 |
| **T-21** | `ts = -1` · `created_at = cutoff` | **保留**（严格 `<`） | 负数**不是**"立即删" |
| **T-22** | `ts = -1` · `created_at = cutoff + 1` | **保留** | 负数**不是**"立即删"（更强） |
| **T-23** | `ts = -2147483648`（INT32 下界）· `created_at = cutoff - 1` | **删除** | 极端负数不会溢出/被特殊处理而漏删 |
| **T-24** | 同一设备、同一 `created_at = cutoff - 1` 的 `ts = 0` 与 `ts = -1` 两行 | **两行都被删**；且**同一次 run 内** `deleted` 计数含这两行 | `ts=0` 与 `ts<0` **判据一致**（成对断言，单看一条无法证明"同支"） |

> **反向断言（必须存在）**：**`T-24b`** —— 同时放入 `ts = 0 / created_at = cutoff + 1` 与 `ts = -1 / created_at = cutoff + 1`
> ⇒ 两行都应保留；若实现把负数单独判超窗，该断言会失败。**这一条是 B-2/B-3 的区分器。**

### 7.1.2 ★ `created_at` 边界 / 单位 / 异常值（**必须显式**）

| # | 项 | 期望 |
|---|---|---|
| **T-25** | **三点边界同批**：`ts=0` × `created_at ∈ {cutoff-1, cutoff, cutoff+1}` | 删 / 留 / 留（三条独立断言，**不得合并为一条**） |
| **T-26** | **单位 = UTC 秒**：断言 `cutoff < 1e11`（秒量级）且 `controller().scheduledTime > 1e12`（毫秒）且 `cutoff === Math.floor(scheduledTime/1000) - days*86400` | 若误用毫秒比较，`created_at`（≈1.79e9）会恒 `<` 毫秒级 cutoff ⇒ **全部误删**，此断言直接暴露 |
| **T-27** | `created_at = 0`（异常值 · DEFAULT 未被使用） | **删除**（`0 < cutoff`）——按 `F-10` 显式决策，**不**开"永久保留"特例 |
| **T-28** | `created_at` 为 **NULL** | **不可能**（列 `INTEGER NOT NULL DEFAULT (unixepoch())`）⇒ L1 断言的是**代码不依赖 NULL 语义**；L2 断言 `INSERT … created_at NULL` **被数据库拒绝**（见 `T-30`） |
| **T-29** | `ts` 为 **NULL** | 同上（`ts INTEGER NOT NULL`）⇒ 谓词**无需** `IS NULL` 分支；若有人加 `OR ts IS NULL` 反而应被审查（无索引路径 + 无实际可能） |

> **为何 `created_at <= 0` 选"删除"而非"保留"**：保留会给出一个**无界的**增长面（任何写入异常都变成永久行），
> 而 `0` 只可能来自显式写入（DEFAULT 不会产生 0）；`asInt` 与 `INSERT` 均未显式给 `created_at` ⇒
> **生产中该值出现即代表异常**，按"最旧"处理与旧行为一致，且不引入新特例。生产侧若有此类行，
> 由 `ST-2` 拦截并回设计层（**不得**静默保留）。

### 7.2 L2（`test/real_d1/retention_real_d1.sh` · 真 D1 引擎）

| # | 用例 | 期望 |
|---|---|---|
| **T-13** | 构造 `ts=0` + **显式 `created_at`** 新 / 旧各 1 行 | 只删旧的那条；**新的一行必须仍在**（并用 `SELECT COUNT(*)` 逐行核验） |
| **T-14** | `ts>0` 超窗行 1000+，`LIMIT 1000` | 分批续跑、`calls ≤ 20`、`Total ≤ 42` |
| **T-15** | `ts<0` 行（`-1` 与 INT32 下界各 1 行，`created_at` 一旧一新） | 与 `ts=0` **完全同支**：旧的删、新的留 |
| **T-16** | `EXPLAIN QUERY PLAN` 断言（**新谓词**） | **含 `ix_lh_device_ts`**、**不含 `SCAN log_history`**、**不含 `ux_lh_dedup` 作为唯一路径** |
| **T-30** | `INSERT … created_at NULL`（`log_history`） | **数据库拒绝**（`NOT NULL` 约束）⇒ 证明 `T-28` 的前提成立 |
| **T-31** | **本地 D1** 逐行 `EXPLAIN QUERY PLAN` 原文（旧谓词 vs 新谓词，两段都存） | 原文写入实施记录；两段的 `id/parent/detail` 逐行完整（**不得**只写"命中索引"） |
| **T-32** | **回归护栏**：L2 既有 `W7-B3`（`telemetry_history` 的 `ts=0` **仍删**） | **保持 `PASS` 且脚本该段零改动** —— 这是 `H-RET-1` 单表范围的**反向证明** |

### 7.3 生产验收（**仅部署授权后 · `H-RET-4`**）

| # | 项 | 期望 |
|---|---|---|
| **T-17** | 首轮 `scheduled` 摘要 | `deleted` 与预期一致；`ts=0` 存量**不再归零** |
| **T-18** | `ts=0` 行在清理后仍存在且 `created_at` 未超窗 | **保留** |
| **T-19** | `W7-C*` / `W7-F*` 回归 | 全部重跑，**不得沿用旧 `PASS`** |

### 7.4 生产**只读** `EXPLAIN`（**不属部署** · 但需同一批次授权）

| # | 项 | 期望 |
|---|---|---|
| **T-33** | 生产 `wrangler d1 execute --remote --command "EXPLAIN QUERY PLAN DELETE FROM log_history WHERE device_id='<占位>' AND ((ts>0 AND ts<1) OR (ts<=0 AND created_at<1)) LIMIT 1000;"` | **只读**（`EXPLAIN` 不执行删除）· 逐行 `id/parent/notused/detail` 原文入档 · 必须含 `ix_lh_device_ts` · 不含 `SCAN log_history` |

> **`T-33` 的纪律**：`EXPLAIN QUERY PLAN` **不修改任何行**（`rows_written` 必为 0），但仍**必须**：
> ① 使用**不存在的 `device_id` 占位**（避免任何可能的语义歧义）；② 执行前后各记录一次行数快照作为零写入自证；
> ③ **不得**以此为"生产验收"，它只证明**计划形态**，不证明**删除效果**。

---

## §8 验收标准（`A-*`）

| # | 标准 | 证据 |
|---|---|---|
| **A-1** | `ts=0` 且 `created_at` **未过期** ⇒ **保留** | `T-5`/`T-6`/`T-7` + `T-13` + `T-18` |
| **A-1b** | `ts=0` 且 `created_at` **已过期** ⇒ **删除** | `T-4` + `T-13` |
| **A-2** | `ts>0` 的删除行为**逐字不变** | `T-1…T-3` + L2 回归 |
| **A-3** | `ts<0` 与 `ts=0` **同一判据**（B-1）；**删除/保留两侧都有断言**；**不会永久保留**；**也不会被"立即删"** | `T-20…T-24` + `T-24b` + `T-15` |
| **A-4** | 删除仍命中 `ix_lh_device_ts`（**本地 + 生产只读 两份原文**） | `T-16` / `T-31` / `T-33` 的**逐行** `EXPLAIN` 原文 |
| **A-5** | 预算不变量 `I-P1…I-P6` 仍成立（`Total ≤ 42` · `HARD_STOP=45` 不可达） | `T-11`/`T-12` + 摘要 `calls` |
| **A-6** | `telemetry_history` 行为**逐字未变**（`H-RET-1` 单表裁决） | `T-9` + `T-32`（`W7-B3` 不改动仍 `PASS`）+ diff 复核 |
| **A-7** | 构建通过 | `deploy --dry-run` 的 bundle 大小 |
| **A-8** | 无 schema / 变量 / 其他模块变更（**含 migration 未动**） | `git status` + diff 范围复核 + `git diff --stat migrations/` 为空 |
| **A-9** | **`created_at` 量纲 = UTC 秒**（无 `/1000`、无时区换算） | `T-26` + 代码审查（`F-9`） |
| **A-10** | **`created_at` 异常值（`<=0`）有显式结论**（删除）且**无"永久保留"特例** | `T-27` + 代码审查（`F-10`） |
| **A-11** | **NULL 语义不引入额外分支**（两列均 `NOT NULL`） | `T-28`/`T-29` + `T-30`（DB 拒绝 NULL） |
| **A-12** | **旧 `PASS` 未被引用**：报告中的每个 `PASS` 都来自**本轮重跑**的**新语义**判据 | 报告 §4 的原始输出 + `run_all` 时间戳（`N-7` / §12） |

---

## §9 STOP 条件（触发即停并报告）

| # | 条件 |
|---|---|
| **ST-1** | 任何形态出现 `SCAN log_history`（全表扫描）或退化到 `ux_lh_dedup` 单路径 |
| **ST-2** | **生产侧**只读审计发现 `created_at` 有 NULL 或 `<= 0` 的真实行（⇒ DEFAULT 语义与假设不符，须回设计层）。**注**：L1/L2 的**构造性**用例（`T-27`）不受此限 —— 那是刻意构造以固化决策 |
| **ST-3** | `ts=0` 行在 L2 中**仍被删除**（修复无效） |
| **ST-4** | 预算被触发（`truncated` 在**正常数据量**下为真 ⇒ 视为实现缺陷） |
| **ST-5** | 需要改 migration / 新增索引才能通过 ⇒ **停止**，回到设计层 |
| **ST-6** | 发现 `telemetry_history` 也被意外改动 ⇒ 立即回滚该分支 |
| **ST-7** | 发现 `ts<0` 落入"两支都不匹配"的空隙（永久保留）⇒ **立即停止**（`B-1` 的直接违反） |
| **ST-8** | 出现"只能靠放宽 stub 断言才通过"的情形（真实语义未满足）⇒ 停并报告，**不得**以减少断言收场 |

## §10 人工审批点（`H-*`）

| # | 事项 | 状态 |
|---|---|---|
| **`H-RET-1`** | 表范围：仅 `log_history` 还是两表一致 | ✅ **`APPROVED`（`2026-10-18`）= 仅 `log_history`** |
| **`H-RET-2`** | `ts<0` 语义：B-1 / B-2 / B-3 | ✅ **`APPROVED`（`2026-10-18`）= B-1** |
| **`H-RET-3`** | **实现授权**（允许改 `retention.js` + 测试） | ✅ **`APPROVED WITH CONSTRAINTS`（`2026-10-18`）** —— 裁决原文见授权申请 **§14**；实施记录见本文 **§13** |
| **`H-RET-4`** | **部署授权**（`wrangler deploy` + 生产验收） | ⛔ **本轮不批准**（**非否决未来部署**）—— 须先闭合报告 §9.3 的 **5 项前置**；**`S3`（执行 `DELETE`）须独立授权**，不得因 `S1`/`S2` 完成而自动执行 |

---

## §11 最终实施报告格式（开工后）

1. **决策依据**：`H-RET-1` / `H-RET-2` 的书面裁决原文（`§12.1`）。
2. **代码差异**：仅 `retention.js` 的行数/片段；**逐字**给出新 SQL 与绑定参数（两表各一份）。
3. **查询计划**：改动前 / 后 `EXPLAIN QUERY PLAN` **逐行原文**对照（本地 `T-31` + 生产只读 `T-33`）。
4. **测试结果**：L1 各组通过数 · L2 各用例 · `run_all` 汇总 · 构建 bundle 大小；**必须附命令与输出时间戳**。
5. **断言变更清单**：改了哪几条、为什么（**逐条对应语义改判**）；并**显式列出"应当不变而未变"的断言**（如 `W7-B3`、L1【1】）。
6. **旧 `PASS` 作废声明**：逐条声明 `§12.2` 中哪些旧 `PASS` **不被引用**。
7. **容量影响**：`ts<=0` 行滞留量估算（行数 × 180 d × 378 B）。
8. **边界确认**：未部署 / 未改 migration / 未改 EMQX / 未改固件 / 未 commit。
9. **门禁**：`BT-1` / `G-4` / `WP-5` / `STOP-R1` 状态**逐条声明未变**。

---

## §12 裁决回填与"旧 `PASS` 作废"登记（`rev.3` · `2026-10-18`）

### 12.1 人工裁决原文（逐字）

> **`H-RET-1`：批准，仅修改 `log_history` 的 retention 语义；`telemetry` 保持不变。**
>
> **`H-RET-2`：批准 B-1；`log_history` 中 `ts>0` 按 `ts` 判断过期，`ts<=0` 按 `created_at` 判断过期。**
>
> **本次仅批准设计语义，不代表批准修改代码或部署。**

**由此产生的**三处**必改项**（若实现与之冲突即视为**未授权变更**）：

| # | 必改项 | 对应条款 |
|---|---|---|
| 1 | `log_history` 谓词 = `(ts > 0 AND ts < ?) OR (ts <= 0 AND created_at < ?)` | `F-1` / `F-4` |
| 2 | `telemetry_history` 语句**逐字不变**（`AND ts < ?` · 2 个绑定参数） | `F-5` / `A-6` / `T-32` |
| 3 | `ts<0` **必须**与 `ts=0` 同支，且**两侧（删/留）都有断言** | `A-3` / `T-20…T-24b` |

### 12.2 ★ 旧版测试验证的是**旧语义** —— 旧 `PASS` **一律不得复用**

> **纪律**：`WP-7` 的 `L1 99 PASS` / `L2 89 PASS` / 生产 `W7-A*…G* 38/38 PASS` **全部在旧谓词下取得**。
> 新谓词改变了"哪些行会被删"，因此**旧 `PASS` 对新语义零证明力**。实现完成后**必须整套重跑**，
> 报告中的每个结论都须来自**新一次运行**。

| 旧资产 | 旧结论 | 在新语义下的性质 | 处置 |
|---|---|---|---|
| L1 `retention_policy.test.mjs`【1】`ts=0` 被删（`:194/209/210`） | PASS | **仍有效** —— 作用对象是 **`telemetry_history`**，`H-RET-1` 已定"保持不变" | **保留原样**（`rev.2` 曾要求改，现撤销，见 §7.1 注） |
| L1 stub `makeRetentionStub` 的 `run()`（`:97` 三绑定解构 · `:121` `row.ts < cutoff`） | — | **★ 编码了旧语义**：新 `log` SQL 有 **4 个**绑定参数 ⇒ 解构会把 `limit` 取成第二个 `cutoff`；谓词也**不认识 `created_at`** | **必须改造**（`N-8`）：按表分支、按表谓词；改造后仍要能表达"恒满批"等故障注入 |
| L1 其余 90+ 断言 | PASS | 多数与谓词无关（预算 / 隔离 / 摘要），但**运行环境已变** | **重跑**，逐项确认，**不得**引用旧输出 |
| L2 `W7-B3`（`telemetry` `ts=0` **被删**） | PASS | **仍有效且必须不变** —— 这是单表裁决的反向证明 | **脚本该段零改动 + 重跑 `PASS`** |
| L2 其余 `W7-*`（分批 / 预算 / 续跑 / 非默认 batch） | PASS | 与谓词间接相关（`deleted` 计数、收敛判定） | **重跑**（`T-14` / `T-32`） |
| 生产 `W7-A*…G*` 38/38 | PASS | **旧语义专属**；且属部署阶段 | **作废**，`H-RET-4` 后按 `T-17…T-19` 重新取证 |
| `Phase 3` 的 `DEPLOYED & ACCEPTED` | ACCEPTED | 认证的是**旧谓词**的生产行为 | **不撤销**（历史事实），但**不构成**新语义的验收 |

### 12.3 与裁决的一致性自查（本次已执行）

| # | 检查项 | 结果 |
|---|---|---|
| 1 | 任务单是否仍写"`ts<0` 待裁决" | ❌ 曾有（`F-4` / `T-8` / `§10`）⇒ **已改为 B-1 定稿** |
| 2 | 任务单是否要求"修正 L1【1】`ts=0` 断言" | ❌ 曾有（`rev.2` 的 ★ 注）⇒ 该断言实为 **`telemetry`** 用例，与 `H-RET-1` 不冲突 ⇒ **已撤销该要求** |
| 3 | 是否存在"两表同改"的暗示 | ❌ `F-5` 已明确按表分支；`A-6` / `T-32` 提供反向证明 |
| 4 | `created_at` 单位是否有断言 | ❌ 曾无 ⇒ 新增 `F-9` / `T-26` / `A-9` |
| 5 | `created_at` 异常值是否有结论 | ❌ 曾无 ⇒ 新增 `F-10` / `T-27` / `A-10` |
| 6 | NULL 是否有显式处理 | ❌ 曾无 ⇒ 新增 `T-28`/`T-29`/`T-30` / `A-11`（结论：**两列均 `NOT NULL`，谓词无需 `IS NULL`**） |
| 7 | 是否有"生产只读 `EXPLAIN`"要求 | ❌ 曾只在实施记录里笼统提"生产只读" ⇒ 新增 `T-33`（含零写入自证） |
| 8 | 旧 `PASS` 是否被登记为不可复用 | ❌ 曾只有 `T-19` 一条 ⇒ 新增 `§12.2` 全量登记 + `A-12` |

> **结论**：设计侧**与两项裁决完全一致**，无剩余未决语义项。唯一未满足的前置是
> **`H-RET-3`（实现授权）**——须人工单独批准后方可开工。
>
> **★ `rev.4` 更新**：`H-RET-3` 已于 `2026-10-18` 以 **`APPROVED WITH CONSTRAINTS`** 批准并执行完毕（见 §13）。

---

## §13 实施记录（`rev.4` · `2026-10-18` · **IMPLEMENTED / NOT DEPLOYED / NOT COMMITTED**）

### 13.1 决策依据（`H-RET-3` 原文摘要）

> **`H-RET-3`：`APPROVED WITH CONSTRAINTS`**
> ① 允许实施：产品代码**仅** `src/retention.js`；允许改**两个 retention 测试文件 + 一个文档**；
> 按 **B-1** 实现；**必须修正 `makeRetentionStub`** 并执行新语义完整矩阵；**旧 PASS 不得冒充本轮结果**。
> ② **同批允许生产只读 `EXPLAIN`**（禁任何写入；须保存逐行四列原文 + 执行时间 + 目标 SQL；出现 `SCAN`/`ux_lh_dedup` 立即停）。
> ③ 禁止：改 `migrations/` / `wrangler.toml` / `src/index.js` / `telemetry_history` 保留逻辑；部署；改 Cron；
> 写生产 D1；改 EMQX；重启设备；`git commit/push/reset/clean`；解除四道门禁。

全文见 `P3-RET-1-Implementation-Authorization-Request.md` **§14**；约束→执行映射见其 **§14.1**（`C-1…C-11` 全部合规）。

### 13.2 代码差异（仅 `src/retention.js` · **+44 / −4**）

```js
// 新增（模块级纯函数 · 唯一实现位置）
export function buildDeleteStatement(table)
{
    if(table === "log_history")
    {
        // ★ 必须是 OR 形态（禁止 COALESCE 包裹）⇒ 保住 ix_lh_device_ts 的 ts 收窄
        return {
            sql: "DELETE FROM log_history"
                + " WHERE device_id = ?"
                + " AND ( (ts > 0 AND ts < ?) OR (ts <= 0 AND created_at < ?) )"
                + " LIMIT ?;",
            args: (deviceId, cutoff, batchSize) => [deviceId, cutoff, cutoff, batchSize]
        };
    }
    return {
        sql: `DELETE FROM ${table} WHERE device_id = ? AND ts < ? LIMIT ?;`,
        args: (deviceId, cutoff, batchSize) => [deviceId, cutoff, batchSize]
    };
}

// runOneTable() 内：原 1 行 `deleteSql` 模板 →  const del = buildDeleteStatement(table);
// bind：`.bind(deviceId, rec.cutoff, cfg.batchSize)` → `.bind(...del.args(deviceId, rec.cutoff, cfg.batchSize))`
```

| 项 | 结果 |
|---|---|
| **绑定顺序（`log`）** | `deviceId, cutoff, cutoff, batchSize`（4 占位符一一对应 · `F-6`） |
| **绑定顺序（`telemetry`）** | `deviceId, cutoff, batchSize`（**逐字未变**） |
| **未变更** | `resolveConfig` / `computeCutoffSec` / `deriveMaxTotalCalls` / `runRetention` / `runRetentionSafely` / `logRetentionSummary` 的签名与语义 |
| **头注释** | `D-3` 原文更新为复议后表述（并写明 `COALESCE` 禁用理由）——**仅注释，无逻辑** |
| **`migrations/`** | `git diff` **空** |
| **`wrangler.toml`** | `git diff` **空** |
| **`src/index.js`** | **本轮零改动**（`scheduled()` 与 `fetch()` 均未触碰；其工作区 diff 属 `BT-1` 既有工作，见 §13.8） |

**★ 偏差说明（必须记录）**：申请 §4.2 预计 `+10…+16 / −3`，**实际 `+44 / −4`**。原因：该区间只估了**可执行代码**，
未计入 ① `buildDeleteStatement` 的 JSDoc（约 19 行）② 文件头 `D-3` 注释修订（约 6 行）。
可执行代码净增约 **+20 / −3**。**结论：改动仍严格限于 1 个函数 + 1 处调用点，未扩散。**

### 13.3 测试结果（**全部为本轮重跑** · 附时间戳）

| 层 | 命令 | 结果 | 证据文件（gitignored） |
|---|---|---|---|
| **L1** | `node test/current/retention_policy.test.mjs` | **`total 142 / PASS 142 / FAIL 0`** · exit `0` · `2026-10-18T06:40:35Z` | `Cloudflare_Assets/.wrangler/ret1_l1_final.log` |
| **L2（第 1 遍）** | `RUN_ID=ret1a PORT=8799 bash test/real_d1/retention_real_d1.sh` | **`PASS 105 / FAIL 1`** · exit `1`（唯一 FAIL = `W7-G2` 环境条件，见下） | `.wrangler/ret1_l2_A.log` |
| **L2（第 2 遍）** | `RUN_ID=ret1b PORT=8800 bash …` | **`PASS 105 / FAIL 1`** · 断言**逐行与第 1 遍完全一致** | `.wrangler/ret1_l2_B.log` |
| **全量** | `node test/run_all.mjs` | **15 套件 `exit=0`** · 「所有套件通过」· `2026-10-18T06:40:39Z→06:40:48Z` | `.wrangler/ret1_runall_final.log` |
| **构建门** | `wrangler deploy --dry-run` | `exit 0` · **Total Upload 107.15 KiB / gzip 26.46 KiB** | 终端输出 |

**L1 增量**：`99 → 142`（**+43**），全部为新增组 `[15]…[19]`；既有 `[0]…[14]` **全部保留并通过**。

**★ `W7-G2` 的唯一 FAIL —— 已定性为「环境条件」，非本次回归**

| 项 | 事实 |
|---|---|
| 断言 | `history-ingest / presence / registry / emqx-admin / migrations` 的 `git status` 必须为 0 行 |
| 实际 | **2**（`src/history-ingest.js` · `src/emqx-admin.js`） |
| 成因 | 两者携带 **`BT-1` 的既有未提交改动**（`+113/−16`：`toTextColumn` 修复 + `phase`/`ms` 观测字段），**本次之前即存在** |
| 为何不回滚 | 授权明确要求「**保留所有既有工作区变更**」⇒ 回滚等同越界 |
| **越界判据（新增 `W7-G2a`）** | 这两个文件的**新增代码行**（排除注释）中 `DELETE FROM / runRetention / buildDeleteStatement / created_at` 命中数 = **0** ⇒ **不含任何 `RET-1` 语义** |
| 判定 | `W7-G2` = **`FAIL（环境条件 · 既有 BT-1 工作）`**；不是 `RET-1` 回归，也不得据此声称 L2 全绿 |

**L2 增量**：`89 → 106` 条断言（**+17**：`T-13`/`T-15`×3/`T-29`/`T-30`×2/`T-32`×2/`T-16`×3/`W7-G2a` 等），既有 `W7-A*…D*` **零改动并全部 `PASS`**。

### 13.4 查询计划（`EXPLAIN QUERY PLAN` · **逐行原文**）

**（a）本地 D1（真实引擎 · `T-31`）** —— `2026-10-18T06:40Z` · persist `wp7_retention_ret1a/state`

| 形态 | `id` | `parent` | `notused` | `detail` |
|---|---|---|---|---|
| **旧** `device_id=? AND ts<?` | 3 | 0 | 91 | `SEARCH log_history USING INTEGER PRIMARY KEY (rowid=?)` |
| | 7 | 0 | 0 | `LIST SUBQUERY 1` |
| | 10 | 7 | 46 | `SEARCH log_history USING COVERING INDEX ix_lh_device_ts (device_id=? AND ts<?)` |
| **新** `(ts>0 AND ts<?) OR (ts<=0 AND created_at<?)` | 3 | 0 | 91 | `SEARCH log_history USING INTEGER PRIMARY KEY (rowid=?)` |
| | 7 | 0 | 0 | `LIST SUBQUERY 1` |
| | 12 | 7 | 0 | `MULTI-INDEX OR` |
| | 13 | 12 | 0 | `INDEX 1` |
| | 21 | 13 | 49 | `SEARCH log_history USING INDEX ix_lh_device_ts (device_id=? AND ts>? AND ts<?)` |
| | 26 | 12 | 0 | `INDEX 2` |
| | 35 | 26 | 50 | `SEARCH log_history USING INDEX ix_lh_device_ts (device_id=? AND ts<?)` |

原文：`.wrangler/ret1_t31_local_explain.log`（含三条 SQL 与时间戳）。

**（b）生产只读（`T-33`）** —— `2026-10-18T06:29:46Z → 06:29:59Z` · `guofeeder` remote · `served_by_region=APAC / colo=SIN`

| 项 | 值 |
|---|---|
| **执行时间** | `06:29:46Z`（前快照）· `06:29:51Z` 附近（两条 `EXPLAIN`）· `06:29:59Z`（后快照） |
| **目标 SQL A（新谓词）** | `EXPLAIN QUERY PLAN DELETE FROM log_history WHERE device_id='__ret1_plan_probe__' AND ((ts > 0 AND ts < 1) OR (ts <= 0 AND created_at < 1)) LIMIT 1000;` |
| **目标 SQL B（基线）** | `EXPLAIN QUERY PLAN DELETE FROM log_history WHERE device_id='__ret1_plan_probe__' AND ts < 1 LIMIT 1000;` |
| **行数快照** | 前 **168** → 后 **168**（`rows_written: 0` · `changed_db: false` · `size_after: 1302528` 不变） |

新谓词计划（逐行原文）：

| `id` | `parent` | `notused` | `detail` |
|---|---|---|---|
| 3 | 0 | 91 | `SEARCH log_history USING INTEGER PRIMARY KEY (rowid=?)` |
| 7 | 0 | 0 | `LIST SUBQUERY 1` |
| 12 | 7 | 0 | `MULTI-INDEX OR` |
| 13 | 12 | 0 | `INDEX 1` |
| 21 | 13 | 49 | `SEARCH log_history USING INDEX ix_lh_device_ts (device_id=? AND ts>? AND ts<?)` |
| 26 | 12 | 0 | `INDEX 2` |
| 35 | 26 | 50 | `SEARCH log_history USING INDEX ix_lh_device_ts (device_id=? AND ts<?)` |

基线计划（逐行原文）：`id=3` 同上 · `id=7` `LIST SUBQUERY 1` · `id=10` `SEARCH log_history USING COVERING INDEX ix_lh_device_ts (device_id=? AND ts<?)`。

原文：`.wrangler/ret1_t33_prod_explain.log`（含时间戳、SQL、前后快照与完整 `meta`）。

**判据核对**

| `T-16` / `T-33` 判据 | 本地 | 生产 |
|---|---|---|
| 含 `ix_lh_device_ts` | ✅ | ✅ |
| **不含 `SCAN`** | ✅ | ✅ |
| **未退化为 `ux_lh_dedup` 单路径** | ✅ | ✅ |
| 出现即 `ST-1` 停止 | **未触发** | **未触发** |
| 零写入 | 本地只读 | ✅ `rows_written=0` / `changed_db=false` |

> **说明**：`id=3 … USING INTEGER PRIMARY KEY (rowid=?)` 在**基线计划里同样存在**（`DELETE … LIMIT` 的既有形态），
> 非本次引入；`notused` 位图差异仅表示索引列使用情况，不影响 "命中 `ix_lh_device_ts` 且无全表扫描" 的结论。
> `MULTI-INDEX OR` ⇒ 两条分支**各自**走 `ix_lh_device_ts`，与设计 §7.4.1(4) 的实测预期一致。

### 13.5 断言变更清单（逐条对应语义改判）

| # | 位置 | 动作 | 原因 |
|---|---|---|---|
| 1 | `makeRetentionStub.run()`（`:97` 三绑定解构 / `:121` 旧谓词） | **改造** | 新 `log` SQL 有 **4** 个绑定、且谓词含 `created_at` ⇒ 不改造会**静默错位**（`limit` 取成第二个 `cutoff`）并产生假 PASS/假 FAIL。新增**绑定数校验**（不符即抛错） |
| 2 | L1 `[1]` 的 `ts=0` 断言（`:194/209/210`） | **保留原样** | 其作用对象是 **`telemetry_history`**（`H-RET-1` = 仅 log）⇒ 语义未变；`rev.2` 曾要求改，已撤销 |
| 3 | L1 新增 `[15]…[19]`（43 条断言） | **新增** | 覆盖 `T-1…T-12` / `T-20…T-29` / 两表对比 / 谓词形态 |
| 4 | L2 `W7-B3`（telemetry 的 `ts=0` 仍删） | **零改动** | 单表裁决的**反向证明**（`T-32`） |
| 5 | L2 新增 `PHASE E`（16 条）+ `W7-G2a`（1 条） | **新增** | `T-13`/`T-15`/`T-29`/`T-30`/`T-16`/`T-32`；`W7-G2a` 为越界判据（不削弱 `W7-G2`） |
| 6 | `T-11a` 期望值 | **修正我的测试期望**（`batches 2 → 3`） | 末批**恰满**时 `n == batchSize` 不判删净 ⇒ 多一次空批确认。这是 **`F-7` 要求保持不变**的旧行为，**非实现缺陷**（第 1 次运行据此报 FAIL，已纠正期望而非改实现） |

**明确未改**：`[0]`/`[2]`…`[14]` 全部断言 · L2 `W7-A*…D*` · 窗口值（90/180）· 预算常量（`20` / `45` / `1000`）· 摘要字段。

### 13.6 旧 `PASS` 作废声明（本轮证据链）

| 旧资产 | 旧结论 | 本轮处置 |
|---|---|---|
| L1（旧）`99 PASS` | 旧谓词 | **作废** ⇒ 本轮 **142 PASS**（新语义，`06:40Z`） |
| L2（旧）`89 PASS` | 旧谓词 | **作废** ⇒ 本轮 **105 PASS / 1 FAIL(环境)**，两遍断言逐行一致 |
| 生产 `W7-A*…G*` `38/38` | 旧谓词 | **作废** ⇒ `H-RET-4` 后须按 `T-17…T-19` 重新取证；**本轮未取任何生产行为证据** |
| `Phase 3 DEPLOYED & ACCEPTED` | 旧谓词 | 不撤销（历史事实），**不构成**新语义验收 |
| `T-17`/`T-18`/`T-19`（生产验收） | — | **`NOT RUN`**（未部署 ⇒ 不得声称） |

> 本轮所有数字均可追溯到 `.wrangler/` 下的**原始日志**（含 UTC 时间戳）；报告中**无一处**引用旧输出。

### 13.7 容量影响

| 项 | 估算 |
|---|---|
| `ts<=0` 滞留 | `26 行/天 × 180 d × ≈378 B ≈ **1.7 MiB/设备**`（11 台 ≈ 19 MiB ≈ `5 GB` 的 0.4%） |
| 索引存储 | **0**（未新增索引） |
| 查询成本 | 新谓词为 `MULTI-INDEX OR`（**非 covering**）⇒ 每条分支多读表页；量级 = 每设备每批 ≤ `LIMIT 1000` 行，**预算内可接受** |
| `created_at` 无索引 | 仅作 `ts<=0` 一支的**残留过滤**，不产生额外索引扫描 |

### 13.8 边界确认（逐项）

| 项 | 状态 |
|---|---|
| 未部署 | ✅ 未执行 `wrangler deploy`（仅 `--dry-run`） |
| 未改 Cron | ✅ `wrangler.toml` `git diff` 空 |
| 未写生产 D1 | ✅ 仅 `EXPLAIN`；前后行数 168 → 168、`rows_written=0` |
| 未改 `migrations/` | ✅ `git diff` 空（`0001/0002/0003` 未动） |
| 未改 `src/index.js` | ✅ **本轮未触碰** —— mtime `2026-10-18 00:22:07`（**早于本轮 13:43**）；工作区上它的 diff（`+18/−4`）**全部是 `BT-1`「缺陷 C」的 `toTextColumn` 归一**（`mqtt_messages` TEXT 列），与 `RET-1` 无关 |
| 未改 `telemetry_history` 谓词 | ✅ 代码逐字 + `T-9`/`T-32` 双证 |
| 未改 EMQX | ✅ 零操作 |
| 未重启设备 | ✅ 未接串口 |
| 未 `commit`/`push`/`reset`/`clean` | ✅ 工作区变更全部保留 |
| 未解除门禁 | ✅ 见 §13.9 |

### 13.9 门禁（**逐条声明未变**）

| 门禁 | 状态 |
|---|---|
| **`BT-1`** | **OPEN**（未变） |
| **`G-4`** | **BLOCKED**（未变） |
| **`WP-5`** | **未解锁**（未变） |
| **`STOP-R1`** | **无书面解除**（未变） |
| **`H-RET-4`（部署）** | **未授予** —— 本轮**不构成**部署依据 |

### 13.10 残余风险 / 未覆盖面

| # | 项 | 说明 |
|---|---|---|
| 1 | 生产 `ts<0` 行 = 0 | `ts<0` 分支仅有**构造性**验证（L1/L2），无真实数据样本 |
| 2 | 生产删除效果未验证 | `T-17/T-18/T-19` 属 `H-RET-4`，本轮 `NOT RUN` |
| 3 | `W7-G2` 仍 FAIL | 由 `BT-1` 未提交工作导致；**要么** `BT-1` 收敛后自然转绿，**要么**在 `H-RET-4` 前单独裁决 |
| 4 | `created_at` 滞后上界（≈8.9 d · `PROXY`） | 保留期只会延长，方向安全 |
| 5 | 已删的 3 条 `CRITICAL` | **不可恢复**；本次修复只防未来 |

> **★ `2026-10-18` 最终只读验收**：报告 `docs/review/RET-1-Final-Read-Only-Acceptance.md` ⇒
> 建议 **`ACCEPTED WITH KNOWN LIMITATIONS`**（**仅"实现与范围"**；**不含生产效果**，仍为 `NOT DEPLOYED`）。
> 未解决项 `U-1…U-6` 已登记；其中 **`U-6` = 生产仍运行旧谓词 ⇒ `ts<=0` 的行仍会每日被删（持续风险，缓解只取决于 `H-RET-4`）**。
> `H-RET-4` 申请已在报告 **§5** 拟好（含待补充 `A-1…A-5`），**本轮未执行**。

---

## §14 ★ 人工终审与正式交接（`rev.5` · `2026-10-18` · **权威口径**）

> 本节为验收报告 **§9** 的镜像。**冲突时以 `docs/review/RET-1-Final-Read-Only-Acceptance.md` §9 为准。**

### 14.1 最终验收判定（与报告 §9.1 逐字一致）

| 验收维度 | 判定 | 依据与限制 |
|---|---|---|
| **实现范围** | **`ACCEPTED`** | 产品逻辑仅涉及 `buildDeleteStatement()` 与 `runOneTable()` 调用点；**未发现授权范围外改动** |
| **`telemetry_history` 不回归** | **`ACCEPTED`** | 原谓词未变，有**实现 · L1 · L2 三重证据** |
| **查询计划** | **`ACCEPTED`** | 本地与生产**均使用 `ix_lh_device_ts`**；**未发现 `SCAN` 或 `ux_lh_dedup`** |
| **自动化测试** | **`ACCEPTED WITH KNOWN LIMITATIONS`** | L1 / 整套测试 / `dry-run` 通过；**`W7-G2` 仍 `FAIL`，不得改写为 `PASS`** |
| **生产删除效果** | **`NOT VERIFIED`** | `T-17` / `T-18` / `T-19` **未执行** |
| **生产问题修复** | **`NOT COMPLETED`** | **未部署**，生产仍运行旧谓词 |

**人工对偏差的裁定**：`+480/−10` **已接受**（报告 §1.3 已分解为产品代码 / 注释 / 测试代码，确认**无功能扩散**）；
**不再为注释与测试行数另行专项调查**。`W7-G2`：**保留原始 `FAIL`**——现有证据**足以解释它为何可能受既有工作区状态影响**，
但**不足以证明根因已完全解决**。

### 14.2 状态与交接结论

| 项 | 状态 |
|---|---|
| 实现与范围 | **`ACCEPTED`** |
| 已知限制 | **保留 `W7-G2` `FAIL` + `U-1…U-6`** |
| 生产部署与行为验收 | **`DEFERRED`** |
| **交接表述** | **实现完成，未部署，生产风险仍在。** |

**后续纪律（逐条生效）**

1. **`RET-1` 专项验收结束** —— 不再主动扩展调查、不开新根因分析、不为消除 `W7-G2` 调整断言。
2. **后续开发继续推进**；未解决项 `U-1…U-6` 转为**生产观察事项**。
3. ❌ 不得把 `DEFERRED` 写成 `PASS` / `CLOSED` / 已修复。
4. ★ **任何依赖生产 `ts <= 0` 记录能够保留的功能，必须显式声明依赖，并在需要时单独申请相关生产操作授权。**
5. 后续开发**不得擅自扩大 `RET-1` 代码范围**；若新工作依赖 `RET-1` 未决行为，须指出依赖并单独申请决策。
6. 若发现**可能导致持续数据丢失的明确风险**，须记录并报告，**不得以「继续开发」为由掩盖**。

### 14.3 `H-RET-4` 前置（5 项，独立审批前必须闭合）

| # | 前置条件 |
|---|---|
| 1 | 明确**生产基线**与 `T-17` 的**精确预期值** |
| 2 | 明确**触发 `scheduled` 的方式**，以及**避免意外重复触发**的机制 |
| 3 | 确认**备份的实际保存位置、保留策略与可用性** |
| 4 | 定义**观察期判据**及**异常时的处置方式** |
| 5 | 明确 **`W7-G2` 是否构成下一阶段的阻断条件** |

> **★ 硬约束**：**备份与 Worker rollback ≠ D1 删除数据的可恢复性。**
> **`S3`（执行 `DELETE`）必须独立授权**；**不得因 `S1`、`S2` 已完成而自动执行 `S3`**。

### 14.4 门禁（终审确认未变）

`BT-1` = **`OPEN`** · `G-4` = **`BLOCKED`** · `WP-5` = **未解锁** · `STOP-R1` = **未解除**。
未部署 · 未写生产 D1 · 未改 Cron / migration / EMQX · 未 `commit` / `push` / `reset` / `clean` / `stash`。
