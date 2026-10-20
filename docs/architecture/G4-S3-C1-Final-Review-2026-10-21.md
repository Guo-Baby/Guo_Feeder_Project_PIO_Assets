# 阶段 3 报告：C-1（`0007` 生产 D1 migration）最终执行前审查

- 审查日期：`2026-10-21`（`00:33`–`00:40`，本机时间 UTC+8）
- 审查对象：`G4-Production-Change-Plan-Rev2-2026-10-20.md`（**Rev.2 是唯一有效手册**；Rev.1 仅历史）
- 审查方式：**只读**（生产 D1 `SELECT` 只读查询 + 仓库代码/迁移/测试只读核对）
- 前置：阶段 1（`G4-S1-…`）与阶段 2（`G4-S2-…`）均已完成并落盘
- **最终结论：`CONDITIONAL`** —— 材料基本具备，但有 **5 项必须补齐的前置条件**（全部是文档修订，不需新证据）
- ★ 本报告**不是**执行许可；C-1 执行仍需人工单独授权

---

## A. 生产 D1 身份与 migration 前置条件

### A.1 数据库身份（本轮实测）

| 项 | 值 | 来源 |
|---|---|---|
| Cloudflare 账号 | `9e72d201efec6925b3a468a0e3fb4928`（`oxmail.com` 名下唯一账号） | 阶段 1 `whoami` |
| D1 database_name | `guofeeder` | `wrangler.toml:13` |
| D1 database_id | `0d8385ba-a1a6-4fcc-8e43-ae61c014c43d` | `wrangler.toml:15` |
| binding | `guofeeder_DB` | `wrangler.toml:11` |
| 线上 Worker 实际绑定 | `env.guofeeder_DB → 0d8385ba-…` ✅ **一致** | 阶段 1 `versions view 826332ed` |
| 服务端落地 | `served_by = v3-prod` · `region = APAC` · `colo = SIN` | 每次查询的 `meta` |
| 写入可见性 | `changed_db=false`、`rows_written=0`（全部为 SELECT） | 同上 |

★ 结论：**身份无歧义**（单一账号、单一 database_id、Worker 绑定与配置一致）。
★ 但 Rev.2 **没有**把"核对 database_id / account / binding"写成执行步骤 —— 见 §D 的 `R-1`。

### A.2 `0007` 的四层独立状态（**全部为"未应用"**）

| 检查 | 结果 | 期望（变更前） |
|---|---|---|
| `pragma_table_info('mqtt_messages')` 含 `dedup_key` | **0** | 0 ✅ |
| `sqlite_master` 含 `ux_mm_dedup_key` | **0** | 0 ✅ |
| 该索引的 `sql LIKE '%WHERE dedup_key IS NOT NULL%'` | **0** | 0 ✅ |
| `d1_migrations` 含 `0007_mqtt_messages_dedup.sql` | **0** | 0 ✅ |

**第二条独立证据**：`wrangler d1 migrations list guofeeder --remote`

```
Migrations to be applied:
┌──────────────────────────────┐
│ 0007_mqtt_messages_dedup.sql │
└──────────────────────────────┘
```

⇒ 只有 `0007` 待应用 ⇒ `0001`–`0006` 已应用。

★ **"有列/有索引" 与 "已登记" 是两个独立事实**，本轮四项全部为 0 且 `migrations list` 只列 `0007`，
两种口径互相印证。Rev.2 §3.3 的 `V-1` 正是这四项并列，**设计正确**。

证据：`s3-d1-baseline.txt` · `s1-…`（migrations list 输出见本报告引用）

### A.3 表结构是否与"本地已验证"一致（**必须**，否则本地结论失效）

生产 `sqlite_master.sql`（原文）：

```
CREATE TABLE mqtt_messages (     id INTEGER PRIMARY KEY AUTOINCREMENT,      device_id TEXT NOT NULL,
topic TEXT,     direction TEXT,       raw_payload TEXT,       cmd TEXT,     msg_id TEXT,
object TEXT,     stable_id INTEGER,      version INTEGER,       status INTEGER,
error_code INTEGER,       message TEXT,       timestamp INTEGER,       payload_json TEXT,
created_at INTEGER , status_text TEXT, error_text TEXT, msg_type_text TEXT, msg_type INTEGER)
```

与 `test/real_d1/fixtures/mqtt_messages.sql` 逐列比对：**20 列，列名与顺序完全一致，无缺无多**。

★ 附注：`created_at INTEGER ,` 多一个空格、且 `status_text`/`error_text`/`msg_type_text`/`msg_type`
排在 `created_at` 之后 —— 这是历史上多次 `ALTER TABLE ADD COLUMN` 留下的顺序痕迹，
**正是 `0007` 这种"只 ADD COLUMN"迁移能安全工作的前提**（表不是重建来的）。
⇒ 生产与 fixture **同构**成立，本地 R-1…R-8 的结论可以引用。

★ 索引：`sqlite_master WHERE tbl_name='mqtt_messages'` 只有表本身，**该表当前没有任何索引**
⇒ `ux_mm_dedup_key` 不会与既有索引重名或冲突。

### A.4 历史数据不会被回填 / 合并 / 删除（**迁移文本层面可证**）

`migrations/0007_mqtt_messages_dedup.sql` 全文只有两条语句：

```sql
ALTER TABLE mqtt_messages ADD COLUMN dedup_key TEXT;      -- 第 43 行
CREATE UNIQUE INDEX IF NOT EXISTS ux_mm_dedup_key         -- 第 46–48 行
    ON mqtt_messages(dedup_key) WHERE dedup_key IS NOT NULL;
```

- **无** `UPDATE` / `INSERT` / `DELETE` / `DROP` / `CREATE TABLE`
- `ADD COLUMN` 无 `DEFAULT` ⇒ 历史行该列为 **NULL**（不会被写入值）
- **部分**索引 `WHERE dedup_key IS NOT NULL` ⇒ 历史 NULL 行**不受约束**，既不会被去重也不会被合并
⇒ **"不回填、不合并、不删除"是结构性成立，不是承诺**。

文件完整性（供执行前比对）：

```
sha256 = 90dc0ec0da0335e8f55cab22121843dc83fd1dacbb7b6d0379bbd9d7ff7e2f1e
git blob = eb300070be913ab272550cde4860448ed03575d2（== HEAD:guo-feeder-api/migrations/0007_…）
行数 = 74
```

### A.5 执行前基线能否安全取得

**能**，且本轮已**演练**（只读）：

| 项 | 值（`2026-10-21 00:33:56 +08`） |
|---|---|
| `COUNT(*)` | **5743** |
| `MIN(id)` / `MAX(id)` | 1 / **5761** |
| 负载长度和 `pl_len` | 817 566 |
| `msg_id` 长度和 `id_len` | 36 129 |
| `topic` 长度和 `tp_len` | 154 790 |
| `COUNT(DISTINCT device_id)` | 73 |

★ 这组值**只是演练基线**，不是执行基线：设备随时可能写入。执行时必须**重新采集**。
★ 注意 `MAX(id)=5761 > COUNT(*)=5743` ⇒ **历史上有 18 个 id 缺口**（删过行）。
`retention.js` 的 `RETENTION_TABLES` 只含 `telemetry_history`/`log_history`，**不含 `mqtt_messages`**
⇒ 缺口不是 cron 造成的，且 **cron 不会在执行期间删这张表的行**。

---

## B. 并发写入与历史数据指纹：审查

### B.1 审查对象

Rev.2 §3.1 `A-4` / §3.3 `V-2`：

```sql
-- 采集（一次查询内自洽：max_id 与聚合同源）
SELECT COUNT(*) AS n_old, MIN(id), MAX(id),
       SUM(LENGTH(COALESCE(raw_payload,''))), SUM(LENGTH(COALESCE(msg_id,''))),
       SUM(LENGTH(COALESCE(topic,'')))
  FROM mqtt_messages
 WHERE id <= (SELECT MAX(id) FROM mqtt_messages);
```

```sql
-- 复核（用常量 MAX_ID_BEFORE 重放，并与 keyed 一起看）
SELECT COUNT(*), SUM(LENGTH(...)), SUM(LENGTH(...)), SUM(LENGTH(...)),
       SUM(CASE WHEN dedup_key IS NOT NULL THEN 1 ELSE 0 END) AS keyed
  FROM mqtt_messages WHERE id <= :MAX_ID_BEFORE;
```

### B.2 它如何排除并发写入（**成立**）

- 集合由 `id <= MAX_ID_BEFORE` 界定，`id` 是 `INTEGER PRIMARY KEY AUTOINCREMENT` ⇒ **严格单调**
- 迁移期间新写入的行 `id > MAX_ID_BEFORE` ⇒ **不进入集合** ⇒ 指纹不受正常写入影响
- 全表 `COUNT(*)` 变化**不参与判定**（Rev.2 已明确废弃 Rev.1 的"行数未变"判据）✅

### B.3 它能检出什么

| 变化 | 能否检出 | 机制 |
|---|---|---|
| 历史行被删除 | ✅ | `COUNT` 下降 |
| 历史行被合并（重复行被删） | ✅ | `COUNT` 下降 |
| 历史行内容被替换（长度变化） | ✅ | 三个 `SUM(LENGTH())` 之一变化 |
| 历史行 `dedup_key` 被回填 | ✅ | `keyed` 由 0 变正 |
| 历史行内容被替换但**长度不变** | ❌ | ← 见 B.4 |
| 历史行 `timestamp`/`status`/`created_at` 被改（长度不变） | ❌ | ← 见 B.4 |
| 索引/列被第三方再次改动 | 部分 | `keyed` 可发现回填，但发现不了"索引被删" |

### B.4 ★ 局限（Rev.2 现有表述**过强**，必须收窄）

Rev.2 §3.3 的表格标题是"**历史行未被改动**"，并给出"不等 ⇒ 历史行被改动 ⇒ P0"。
实际上该指纹**只是必要条件的代理**：

1. **不覆盖同长度内容替换**：`SUM(LENGTH())` 对"等长改写"完全不敏感
2. **不覆盖非文本列**（`stable_id`/`version`/`status`/`error_code`/`timestamp`）
3. **不覆盖"删除 N 行 + 新增 N 行到同一 id 区间"**（`id` 是自增，正常不会复用，但手工 `INSERT` 可指定 id）
4. 它**不是**对整个数据库的校验，**仅**覆盖 `mqtt_messages` 这张表在 `id <= MAX_ID_BEFORE` 上的一阶统计量
5. 生产**历史上已有 18 个 id 缺口** ⇒ 不能用"`id` 连续"当完整性证据

⇒ 结论文字必须改为："指纹**不变**只能说明'未发生可被这四项统计量捕获的改动'，
**不能**证明所有历史行绝对未变"。若要做强校验，需对固定 id 区间做**逐行抽取复核**（如
`SELECT id, length(raw_payload), msg_id FROM mqtt_messages WHERE id IN (…抽样 50 个…) ORDER BY id`），
成本可接受且能显著提升可信度 —— 建议加入 `V-2b`（见 §D `R-3`）。

---

## C. migration 原子性与失败恢复

### C.1 本地证据是什么

`docs/evidence/G4-ChangePlan-Rev2-2026-10-21/`：

| 实验 | 内容 | 结果 |
|---|---|---|
| E-1 | 正式 `d1 migrations apply --local`，库中**无** `mqtt_messages` | `0001`–`0006` ✅ 登记；`0007` ❌ `no such table`；**`0007` 不登记** |
| E-2 | 同流程，库中**有**同构表 + 预置 2 行完全重复 | `0007` ✅ 应用并登记；`col/idx/partial_idx/reg = 1/1/1/1`；历史 2 行**仍在**、键全 NULL |
| E-3 | 故意让一个 migration 的第 3 条语句失败（`CREATE` + `ALTER` + 失败语句） | 失败后 `tbl=0 / col_b=0 / reg=0` ⇒ **整文件回滚、零残留、零登记** |

### C.2 它**证明了**什么

- 结论只在**本地 D1 引擎（workerd / miniflare，`--local --persist-to`）**上成立
- 证明：**同一 migration 文件内的语句是原子的**；失败时**不会**留下"列建了但索引没建"或"执行了但没登记"
- 证明：`0007` 在"有表 + 有历史重复数据"时可成功，且**不追溯合并**历史重复行
- 证明：正式流程会**拾取** `0007`（E-1 中它被尝试执行并失败，而不是被跳过）

### C.3 它**没有证明**什么（不得外推）

1. **生产 D1 的原子性未验证**。生产 `d1 migrations apply` 与本地引擎是两套执行栈；
   官方语义为"每个 migration 文件在事务中执行"，但**本轮没有生产侧的失败注入证据**
2. 未证明生产 `ALTER TABLE` 在**大表 + 并发写入**下的耗时/锁行为（本表 5 743 行，风险低但未测）
3. 未证明生产在 migration 期间**已有连接**上的行为（新列对旧 Workers 不可见，无害）
4. E-3 的失败形态是"语法/引用错误"，**没有**覆盖"磁盘/超时导致的连接中断"

⇒ 结论只能写"**本地**整文件失败无残留"，**不能**写成"生产保证原子"。

### C.4 部分唯一索引的条件、冲突语义、历史 NULL

| 项 | 事实 | 影响 |
|---|---|---|
| 索引条件 | `WHERE dedup_key IS NOT NULL` | NULL 行不进索引 ⇒ 历史行永不冲突 |
| 写入冲突子句 | `ON CONFLICT(dedup_key) WHERE dedup_key IS NOT NULL DO NOTHING` | 谓词与索引**逐字一致**（SQLite 硬要求，不一致会直接报错） |
| 是否吞掉其它错误 | **否**（不是 `INSERT OR IGNORE`）⇒ `NOT NULL` 等照常抛 | 不会静默吞真实失败 |
| 对现有业务写入的影响 | **零**：当前线上代码（v47）根本不写 `dedup_key` 列 | 即使先建索引后部署，旧代码行为不变 |

### C.5 失败后的恢复路径与副作用

Rev.2 §3.4 给出三类失败与处置，本轮核对后**基本成立**，但**恢复动作本身未定义到可执行粒度**：

| 情形 | 现状 | 缺什么 |
|---|---|---|
| `apply` 报错（结构错误） | "停止 + 人工" | 未给出"`duplicate column` 时如何确认是历史手工执行"的**只读**判定语句（`pragma_table_info` + `d1_migrations` 组合判） |
| DDL 生效但未登记 | "停止 + 人工" | 未给出两条可选恢复路径的**副作用对比**（补登记 vs `DROP INDEX` 重来） |
| 并发写入导致统计变化 | 已明确"指纹不变即正常" | ✅ 已覆盖 |

★ **若生产出现"列已加、索引未建"的非原子半成品**（本地证据不支持但生产未验证），
唯一安全恢复是 **新增 `0008` 只建索引**（保持登记链完整），**不得**手工 `CREATE INDEX`。
Rev.2 未写这一点 ⇒ 见 §D `R-5`。

---

## D. 执行与验证方案：逐项对照

| # | 任务书要求 | Rev.2 是否明确规定 | 判定 | 最小修订 |
|---|---|---|---|---|
| 1 | 执行前**数据库身份**再次核实 | ❌ 只查了表结构（§2.5），没查 `database_id`/账号/绑定 | **缺** | **`R-1`**：§2 增加 3 行 —— `wrangler whoami`（账号）、`wrangler.toml` 的 `database_id`、`wrangler deployments status` 的 version 元数据里 `env.guofeeder_DB` 的 id，三者必须一致且等于 `0d8385ba-…` |
| 2 | 执行前 **migration 状态**再次核实 | ✅ §2.6 `migrations list --remote` + §3.3 `V-1.reg` | 满足 | — |
| 3 | 执行前**基线采集与归档** | ✅ §3.1 `A-1`…`A-4` + 指纹 | 满足 | 建议补"基线文件路径与时间戳写进变更记录" |
| 4 | **唯一**正式执行方式 | ✅ §3.2 `d1 migrations apply`，并明令禁止 `--file` | 满足 | 建议补一条：执行前 `sha256sum migrations/0007_…` 必须等于评审值（**`R-2`**） |
| 5 | 执行后**列/索引/partial/登记/指纹**独立验证 | ✅ §3.3 `V-1`/`V-2`/`V-3` | **基本满足** | 补 `dedup_key` **列属性**检查（`NOT NULL`/`DEFAULT` 应为无）+ 索引 `sql` 全文留档（**`R-4`**）；补抽样逐行复核（**`R-3`**） |
| 6 | 失败时的**停止条件/错误处理/恢复边界** | ✅ §3.4 三类失败 | **部分** | 补"半成品"情形的唯一恢复路径（**`R-5`**） |
| 7 | 生产并发下**哪些差异是预期的** | ⚠️ 只写了"全表行数变化正常" | **部分** | 补：新 `device_id` 出现、`direction` 分布变化、`echo` 行比例变化（阶段 2 §4.2 已证会发生）、历史 18 个 id 缺口 ⇒ 均**不得**判为失败（**`R-5`**） |
| 8 | 哪些结果**必须人工判定** | ⚠️ 散落在"停止/升级"语句里 | **部分** | 补一张"自动判 / 人工判"两列清单（**`R-5`**） |
| 9 | 指纹表述不得过强 | ❌ 标题写"历史行未被改动" | **缺** | 按 §B.4 收窄（**`R-3`**） |

★ 以上 5 条（`R-1`…`R-5`）**全部是文档修订**，不需要新的生产读数、不需要新测试。

★ **另有一项不属于 C-1 但必须在 C-3 之前改**：阶段 2 §4.1 证明 `echo` 只存在于 `down`，
Rev.2 §5.3 的 `I-8`（audit>0 且 echo>0）与真机验收清单的 `Q-5` 会**永远误报** ⇒ 需按方向收窄（`R-6`）。

---

## E. 依赖与状态纪律

| 项 | 核对结果 |
|---|---|
| 阶段 1 是否足以支撑 C-2 的版本识别与回滚 | ✅ `CURRENT_VERSION_ID = 826332ed-…`（v47）· `PREV_VERSION_ID = f9647a00-…`（v46）· 回退参数类型已核实（version id）· 两版本绑定/变量/secret 逐项一致 ⇒ **`C2-VER` 解除** |
| 阶段 1 的残留风险 | ⚠️ 线上版本**无法追溯到 commit**（`Source: Unknown`，且部署时刻无对应提交）；且**2026-10-19 20:57–10-20 02:50 的 193 行 plain-`down` 归到了设备 id**，说明 v47 **已含 `down` 方向解析**（强推断）⇒ C-2 的 "P-1' 旧版特征" 描述要跟着改 |
| 阶段 2 对 C-1 的影响 | **不改变 C-1 的结论**。C-1 只加列+索引，不改任何写入语义；`B-3`（漏记）与它无关 |
| 阶段 2 对 C-3 的影响 | ⚠️ 两处必须修：`I-8`/`Q-5` 按方向收窄（`R-6`）；"设备发出 vs 行数"类口径**禁止**使用（`mqtt_messages` 自 `2026-10-16` 起只收 `up`） |
| `G-4 = BLOCKED` | ✅ **未变** |
| `WP-5` 未解锁 | ✅ **未变** |
| `BT-1 = CLOSED` | ✅ **未变** |
| `STOP-R1` 无解除文件 | ✅ **未变** |
| `G4-R4` 阈值 | ✅ **未改动**（仍为未批准的候选值） |
| `LOG_BOOT_WATERMARK` | ✅ **未改动**；且本轮在**线上版本元数据**里确认它为 `"1"`（比读仓库配置更强） |
| C-1 / C-2 / C-3 独立审批 | ✅ 本报告不构成任何一项的授权 |

---

## F. 结论

### 最终状态：`CONDITIONAL`

**理由**：C-1 的技术前提**已全部核实且全部为"未应用"**，本地证据链完整、生产表结构与 fixture 同构、
迁移文本无可疑 DML、基线可安全采集；但执行手册在 **5 个具体环节**上未达到任务书要求
（身份复核缺、执行前文件哈希核对缺、指纹表述过强、列属性/未覆盖项缺、半成品恢复路径缺）。
这 5 项都是**文档修订**，不改代码、不产生新风险。

⇒ 修完 `R-1…R-5`（以及 C-3 的 `R-6`）后，可重新评定为 `READY FOR HUMAN APPROVAL`；
**即便评为 READY，也只是"等待人工授权"，不是执行许可**。

### 不得据本轮得出的结论

- ❌ "C-1 可以在生产保证原子" —— 生产原子性**未验证**
- ❌ "B-3 已定位/已排除" —— 根因仍**未定**（阶段 2）
- ❌ "线上代码就是某个 commit" —— 线上版本**无法追溯**（阶段 1）
- ❌ "指纹证明历史数据绝对未变" —— 只是四项统计量的必要条件（§B.4）
- ❌ "本地 19/19 全绿 ⇒ 生产 READY" —— 不接受

### 阻塞项清单（按最小修订要求）

| ID | 阻塞什么 | 文档位置 | 最小修订 |
|---|---|---|---|
| `R-1` | C-1 的"身份复核" | Rev.2 §2 | 增加账号 / `database_id` / 线上绑定三处一致性检查 |
| `R-2` | C-1 的"制品一致性" | Rev.2 §3.2 | 执行前 `sha256sum` 与评审值比对（`90dc0ec0…`） |
| `R-3` | C-1 的"历史未变"表述 | Rev.2 §3.3 | 收窄为"未被四项统计量捕获的改动"；新增 `V-2b` 抽样逐行复核 |
| `R-4` | C-1 的"验证完整性" | Rev.2 §3.3 | 增 `dedup_key` 列属性（无 `NOT NULL`/`DEFAULT`）+ 索引 `sql` 全文留档 |
| `R-5` | C-1 的"失败/恢复/预期差异" | Rev.2 §3.4、§5.3 | 补半成品唯一恢复路径（新增 `0008`）、预期差异清单、自动判/人工判两列清单 |
| `R-6` | C-3 的判据正确性 | Rev.2 §5.3 `I-8` · 验收清单 `Q-5` | `echo` 判据按 `direction='down'` 收窄 |
