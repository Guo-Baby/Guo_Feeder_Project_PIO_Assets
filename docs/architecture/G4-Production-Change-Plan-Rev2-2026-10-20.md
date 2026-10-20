# G4 生产变更计划 · Rev.2.1（云端 `0007` + Worker 部署 + 生产验证）

- 日期：`2026-10-21`
- 状态：**REVISED（Rev.2 → Rev.2.1，关闭 `R-1`…`R-6`）—— 仍为申请，未执行任何生产变更**
- 取代：`G4-Production-Change-Plan-2026-10-20.md`（Rev.1，保留原文作审计留痕）
- 审查依据：`G4-Production-Change-Plan-Review-2026-10-20.md`（A–G）·
  `G4-S3-C1-Final-Review-2026-10-21.md`（`R-1`…`R-6` 的提出处）·
  `G4-R1-R5-Rev3-and-R6-Report-2026-10-21.md`（本轮修订报告）
- 目标库 / 目标 Worker：D1 `guofeeder`（binding `guofeeder_DB`）· `guo-feeder-api`

> ★ 本文件是**执行手册**，全部最终 SQL 只在这里出现一份（避免两份副本漂移）。
> 审查过程与"原 SQL 错在哪"见审查报告；本文件只写**要执行什么、怎么判、怎么退**。
> ★ **Rev.2.1 是原地修订**（不是新文件）：`git log -p` 可看到 Rev.2 → Rev.2.1 的逐行差异。

---

## 0. Rev.1 → Rev.2 的实质改动（摘要）

| # | Rev.1 的问题 | Rev.2 的处理 |
|---|---|---|
| 1 | `wrangler rollback <deployment-id>` | ❌ **错**。`wrangler 4.145.0` 的 `rollback [version-id]` 位置参数是 **version id**。已改（§4.5） |
| 2 | 行数一变就判 migration 失败 | ❌ 会与**正常并发写入**混淆。改为**按 `id` 界定的历史行指纹**（§3.3） |
| 3 | `② cloud_ack_ok ≈ ③ accepted` | ❌ 量纲错：`cloud_ack_ok` 是**批次**数，`accepted` 是**行**数。已删该等式（§5.4） |
| 4 | `Q-2 同 msg_id 重复率` | ❌ 同 `msg_id` 的 audit + echo **天然两行**；同 `msg_id` 不同 payload 也是合法两行。改为**索引完整性不变量** + 明确标注"近似"（§5.2） |
| 5 | `Q-3 created_at/1000 - timestamp` 未校验单位与语义 | ⚠️ 单位对（ms / 秒），但 `timestamp` 在**部分帧里是消息类型**，且设备时钟未同步时缺 `t`。已加区间过滤 + 必须同时报 `n`（§5.2） |
| 6 | `Q-4 created_at/300` 未说明单位 | ✅ 对 `log_history` 成立（`DEFAULT (unixepoch())` = 秒）；已显式写明**不得**用于 `mqtt_messages`（毫秒）（§5.2） |
| 7 | P-1…P-5 一律"构造消息" | ⚠️ 会写生产表、刷新 `device.last_seen`。改为**被动优先**：P-1/P-3 用真实流量，只有 P-2/P-4/P-5 需要合成，且带标记与排除规则（§4.4） |
| 8 | 前置条件含"等新固件上板" | ✅ 已过时：固件 `94ba711c…` 已于上一轮上板。改为**执行前实时复核**清单（§2） |
| 9 | 未记录 `LOG_BOOT_WATERMARK` 实际状态 | ❌ Rev.1 隐含"默认关闭"。实际 `wrangler.toml` 自 `362ddfd`（`2026-10-19`）起 `LOG_BOOT_WATERMARK = "1"` ⇒ **已启用**，本轮**不得改动**（§6.3） |
| 10 | 未验证 migration 的原子性与登记失败态 | ✅ 已本地实测（§3.4）：整文件原子，失败**零残留、零登记** |

---

## 0.1 Rev.2 → Rev.2.1 的实质改动（`R-1`…`R-6` 关闭记录）

| 项 | 原状态 | Rev.2.1 的处理 | 落点 |
|---|---|---|---|
| **`R-1`** 身份复核 | Rev.2 只查表结构，**没查** `database_id`/账号/绑定 | 新增 §2.A「四元身份一致性」：账号 / `wrangler.toml` 的 `database_id` / `d1 list` / 线上 version 元数据里的绑定，**四源交叉，全等才继续** | §2.A |
| **`R-2`** 制品一致性 | 无哈希校验 | 新增 §2.C：`sha256sum` 必须 == `90dc0ec0…2f1e`，且工作区 blob == `HEAD` blob == `eb300070…75d2` | §2.C · §3.2 |
| **`R-3`** 指纹边界 | 标题写"历史行未被改动"（**过强**） | 收窄为"未被四项统计量捕获的改动"；新增 **`V-2b`** 固定 ID 抽样逐行复核；抽样只作补充证据 | §3.3 |
| **`R-4`** 列/索引验证 | 只查列存在 + 索引名 + `LIKE '%WHERE…%'` | 新增 `V-1b`（列属性：无 `NOT NULL`/`DEFAULT`/`PRIMARY KEY`）+ `V-1c`（索引 `sql` **全文**留档并逐项比对） | §3.3 |
| **`R-5`** 半成品与恢复 | 只写"整文件原子 ⇒ 修好再 apply" | 新增 §3.7 **六状态矩阵**（本地逐态实测）+ **「`0008` 只建索引」在 state 3/4 不可用**的实证 + 伪成功陷阱 | §3.4 · §3.7 |
| **`R-6`** `echo` 判据 | `I-8`/`Q-5` 要求"audit>0 **且** echo>0"（**未限方向**） | 按方向收窄：`down` 才有 audit/echo 双写；`up` **不得**要求 echo；`log` 独立判；`unknown` 排除在正常计数外 | §5.2 · §5.3 · §5.5 |

★ 六项**全部是文档/判据修订**：不需要新的生产读数，也没有放宽任何已通过的门禁。
★ **反方向声明**：`R-6` 只是消除"不适用于 `up` 的 `echo` 断言"，**不构成** B-3 已解决的任何证据。

---

## 1. 变更范围（三件事，**顺序绑定**，逐项授权）

| # | 变更 | 制品 | 可独立回滚 | 单独授权 |
|---|---|---|---|---|
| **C-1** | 应用 `0007_mqtt_messages_dedup.sql` | `mqtt_messages.dedup_key`（列）+ `ux_mm_dedup_key`（**部分**唯一索引） | ✅ `DROP INDEX`（列保留） | 需 C-1 授权 |
| **C-2** | 部署 Worker（`B-1` / `B-1b` / `B-2` / `B-2b` / `B-2c`） | `device-registry.js` · `mqtt-dedup.js` · `index.js` · `history-ingest.js` | ✅ 回退到上一 **version id** | 需 C-2 授权 |
| **C-3** | 观察窗口与只读对账 | 只读 SQL + 串口只读命令 + EMQX 控制台只读 | 不适用 | 需 C-3 授权 |

```
C-1 ──→ C-2 ──→ C-3
```

★ 顺序理由：`insertMqttMessage()` 的 `INSERT` **显式列出** `dedup_key` 列。
列不存在 ⇒ 首次写入触发 `no such column` ⇒ 内置降级改为无键写入并 `console.error`
（不会整站 500），但**该 isolate 生命周期内**永久失去去重（`dedupDisabled` 是模块级变量）。

★ 反向顺序不是"禁止"而是**降级路径**：可用，但必须接受"这段时间没有去重"，
且**不得**作为正常部署路径。

---

## 2. 执行前实时复核（**必须逐项现查，不得引用本文件或任何历史报告的数字**）

### 2.A `R-1` 四元身份一致性（**第一道闸门，不过就停**）

生产 D1 的身份必须由**四个独立来源**交叉确认，**全等**才允许执行任何 DDL：

```bash
cd D:/Guo_Feeder_Project/Cloudflare_Assets
# ① 账号
node node_modules/wrangler/bin/wrangler.js whoami
# ② 仓库配置里的 database_id
grep -n "database_id\|database_name\|binding" guo-feeder-api/wrangler.toml
# ③ 账号下真实存在的库（名称 → id 映射，防"同名不同库"）
node node_modules/wrangler/bin/wrangler.js d1 list --json
# ④ 线上 Worker 版本元数据里的**实际绑定**（最强的"线上到底连哪个库"证据）
node node_modules/wrangler/bin/wrangler.js versions view <CURRENT_VERSION_ID>
```

| # | 来源 | 期望值（`2026-10-21` 基线，**执行时须重采**） |
|---|---|---|
| ① | `whoami` Account ID | `9e72d201efec6925b3a468a0e3fb4928` |
| ② | `wrangler.toml` | `binding=guofeeder_DB` · `database_name=guofeeder` · `database_id=0d8385ba-a1a6-4fcc-8e43-ae61c014c43d` |
| ③ | `d1 list` 中 `guofeeder` 的 `uuid` | 必须 == ② |
| ④ | 线上 version 的 `env.guofeeder_DB.id` | 必须 == ② |

★ **任一不等 ⇒ 立即停止**，不得以"库名相同"或"历史报告说是这个库"推断目标正确。
★ 四项里 ①③④ 都是**只读 API**；③ 若返回多个同名库，本身就是**歧义** ⇒ 停。
★ 若 `wrangler.toml` 出现 `[env.*]` 段，说明存在多环境 ⇒ 先确认用的是哪一份配置再继续
（`2026-10-21` 实测**无** `[env.*]`）。

### 2.B 复核清单（行号 `2.1`–`2.12`）

| # | 项 | 命令 | 记录内容 |
|---|---|---|---|
| 2.1 | 云端仓状态 | `git -C Cloudflare_Assets rev-parse HEAD origin/main` · `git status --porcelain` | HEAD == origin/main 且工作区干净 |
| 2.2 | 固件版本与哈希 | `sha256sum` 已归档件 · 串口 `cm {"cmd":"system","ob":"restart_status","id":"..."}` | 设备实跑固件的 sha256 / commit |
| 2.3 | 当前 `boot_seq` 与序号水位 | 串口 `cm {"cmd":"system","ob":"stats","id":"..."}` | `boot_seq` · `queue.acked_seq` · `counters.*` |
| 2.4 | 设备在线与日志链路 | `system.stats` 的 `queue.connected` / `cloud_batch_sent` 在 5 min 内是否增长 | 有增长 = 链路活 |
| 2.5 | 生产 D1 结构 | 见 §3.2 的 A-1/A-2/A-3 | `mqtt_messages` 的 `sql` 原文 + 无 `dedup_key` |
| 2.6 | migration 登记 | `npx wrangler d1 migrations list guofeeder --remote` | `0007` 必须**未应用**，且 `0001`–`0006` 已应用 |
| 2.7 | Worker 版本与回退目标 | `npx wrangler deployments list` · `npx wrangler versions list` | **同时记 `deployment id` 与 `version id`**；回退传 **version id** |
| 2.8 | 观察窗口负责人 | 人工填写 | 谁看 / 看多久 / 触发停止条件时谁下令回滚 |
| 2.9 | 授权 | 人工书面 | C-1 / C-2 / C-3 **分别**批准 |
| **2.10** | **四元身份一致性（`R-1`）** | §2.A 四条命令 | 四源全等；任一不等 ⇒ **停止** |
| **2.11** | **制品哈希（`R-2`）** | `sha256sum migrations/0007_mqtt_messages_dedup.sql` | 必须 == `90dc0ec0…2f1e`（全文见 §2.C） |
| **2.12** | **半成品状态判定（`R-5`）** | §3.7 的 `S-PROBE` 查询 | 必须落在 **`S0`**；其它状态 ⇒ 查 §3.7 决策表 |

★ 2.5 的 `sqlite_master.sql` 原文必须与本地 fixture
（`test/real_d1/fixtures/mqtt_messages.sql`）**逐列一致**；不一致 ⇒ **停止**，
因为 `0007` 只做 `ALTER`，不建表，schema 分叉会让"本地已验证"失效。

### 2.C `R-2` 制品一致性（**执行前必查，不符即停**）

```bash
cd D:/Guo_Feeder_Project/Cloudflare_Assets/guo-feeder-api
sha256sum migrations/0007_mqtt_messages_dedup.sql
git hash-object migrations/0007_mqtt_messages_dedup.sql      # 工作区 blob
git rev-parse HEAD:./migrations/0007_mqtt_messages_dedup.sql # HEAD blob
```

| 项 | 期望值（`2026-10-21` 审定） |
|---|---|
| SHA-256 | `90dc0ec0da0335e8f55cab22121843dc83fd1dacbb7b6d0379bbd9d7ff7e2f1e` |
| 工作区 blob | `eb300070be913ab272550cde4860448ed03575d2` |
| `HEAD` blob | 同上（**三者必须同时相等**） |

★ 三个值任一不符 ⇒ **不得应用**。哈希变了意味着"被审的制品 ≠ 将被执行的制品"，
必须重新审查（哪怕差异看起来只是注释）。
★ 为什么三者都要：`sha256sum` 证明**字节**没变，`git hash-object` 证明**内容**没被
换行/编码归一化骗过，`HEAD` blob 证明**它确实是被提交评审的那一份**（而不是本地改动）。

### 2.D 生产 `mqtt_messages` 是否被 retention 触碰（防止"指纹变化"被误判）

```bash
grep -n "mqtt_messages" src/retention.js || echo "retention 不涉及 mqtt_messages"
```

`2026-10-21` 实测：`retention.js` **不包含** `mqtt_messages`；cron 不会删它的行。
⇒ 若指纹真的变了，**不能**用"可能是 cron 删的"解释。

---

## 3. C-1：`0007` 应用

### 3.1 变更前快照（只读，全部存档，含时间戳）

```sql
-- A-1 结构原文（必须与 fixture 逐列一致）
SELECT sql FROM sqlite_master WHERE name='mqtt_messages';

-- A-2 确认无 dedup_key 列（期望 0）
SELECT COUNT(*) AS n FROM pragma_table_info('mqtt_messages') WHERE name='dedup_key';

-- A-3 现状规模
SELECT COUNT(*) AS n, MIN(id) AS min_id, MAX(id) AS max_id,
       COUNT(DISTINCT device_id) AS devs
  FROM mqtt_messages;

-- A-4 ★ 并发安全基线（**唯一**用于判"历史行未被改动"的锚点）
--   固定 id 上界，之后新增行不会进入这个集合
SELECT COUNT(*) AS n_old, MIN(id) AS min_id_old, MAX(id) AS max_id_old,
       SUM(LENGTH(COALESCE(raw_payload,''))) AS pl_len,
       SUM(LENGTH(COALESCE(msg_id,'')))      AS id_len,
       SUM(LENGTH(COALESCE(topic,'')))       AS tp_len
  FROM mqtt_messages
 WHERE id <= (SELECT MAX(id) FROM mqtt_messages);   -- ⇒ 记下 max_id_old 的值
```

★ 把 `A-4` 的结果记成 `FINGERPRINT_BEFORE = (n_old, min_id_old, max_id_old, pl_len, id_len, tp_len)`
和常量 `MAX_ID_BEFORE = max_id_old`。**之后一切"未改动"判定都用 `id <= MAX_ID_BEFORE` 重放。**

### 3.2 应用（**正式流程，禁止手执行**）

**执行前 60 秒内必须再跑一次（不许复用几分钟前的读数）：**

```bash
cd D:/Guo_Feeder_Project/Cloudflare_Assets/guo-feeder-api
# G-1 身份（§2.A 四源；③ 是对 `guofeeder` 是否存在同名多库的判定）
node ../node_modules/wrangler/bin/wrangler.js whoami
node ../node_modules/wrangler/bin/wrangler.js d1 list --json | grep -A2 '"guofeeder"'
# G-2 制品
sha256sum migrations/0007_mqtt_messages_dedup.sql     # 期望 90dc0ec0…2f1e
# G-3 登记状态（期望：**只列 0007**，且 0001–0006 已应用）
node ../node_modules/wrangler/bin/wrangler.js d1 migrations list guofeeder --remote
# G-4 半成品状态（期望 col/idx/reg 全 0 ⇒ S0）
node ../node_modules/wrangler/bin/wrangler.js d1 execute guofeeder --remote --json \
  --command "<§3.7 的 S-PROBE>"
```

**四条全绿才执行：**

```bash
cd D:/Guo_Feeder_Project/Cloudflare_Assets/guo-feeder-api
node ../node_modules/wrangler/bin/wrangler.js d1 migrations apply guofeeder --remote
echo "apply_exit=$?"
```

★ **禁止** `d1 execute --file migrations/0007_*.sql`：那会跳过 `d1_migrations` 登记
（`0004`/`0005` 已踩过，见 `migrations/README.md` §迁移状态说明 `D-1`）。

★★ **`apply_exit=0` 不是成功信号**（`2026-10-21` 本地实测）：在"`0007` 已登记但结构缺失"
的状态下，`apply` 会打印 `✅ No migrations to apply!` 并**返回 0**，而结构依然是坏的。
⇒ 成功**只能**由 §3.3 的 `V-1/V-1b/V-1c/V-2/V-2b/V-3` 六项共同判定。
反过来，`apply_exit=1` 一定是失败（本地实测：`duplicate column name` ⇒ exit 1）。

### 3.3 应用后验证（**立即**执行；六类检查相互独立，缺一不可）

```sql
-- V-1 结构：列 / 索引 / 是否部分索引 / 登记   （期望 1 / 1 / 1 / 1）
SELECT (SELECT COUNT(*) FROM pragma_table_info('mqtt_messages') WHERE name='dedup_key') AS col,
       (SELECT COUNT(*) FROM sqlite_master WHERE type='index' AND name='ux_mm_dedup_key') AS idx,
       (SELECT COUNT(*) FROM sqlite_master WHERE name='ux_mm_dedup_key'
          AND sql LIKE '%WHERE dedup_key IS NOT NULL%') AS partial_idx,
       (SELECT COUNT(*) FROM d1_migrations
          WHERE name='0007_mqtt_messages_dedup.sql') AS reg7;
```

```sql
-- V-1b ★ `R-4` 列属性（**只看"列存在"不够**）
--     期望：type=TEXT · notnull=0 · dflt_value=NULL · pk=0
SELECT name, type, "notnull" AS notnull, dflt_value, pk
  FROM pragma_table_info('mqtt_messages')
 WHERE name = 'dedup_key';
-- 判据：notnull 必须 0（0007 写的就是 `ADD COLUMN dedup_key TEXT`，无 NOT NULL / DEFAULT / PK）
--       type 必须 TEXT（若为 BLOB/无类型 ⇒ 有人手工建过列 ⇒ 停）
--       dflt_value 必须 NULL（有默认值 ⇒ 语义变了：NULL 键不再"未写"，会变成固定值 ⇒ 灾难）
```

```sql
-- V-1c ★ `R-4` 索引定义**全文**（留档 + 逐项比对，不得只看名字）
SELECT name, sql FROM sqlite_master WHERE name = 'ux_mm_dedup_key';
-- 期望（逐字，除空白外应完全一致）：
--   CREATE UNIQUE INDEX ux_mm_dedup_key
--       ON mqtt_messages(dedup_key)
--       WHERE dedup_key IS NOT NULL
-- 判据：① 含 UNIQUE ② 列是 (dedup_key) 且**只有这一列**
--       ③ 含 `WHERE dedup_key IS NOT NULL`（**部分**索引）
--       ④ 无 COLLATE / 无 DESC / 无附加表达式
```

```sql
-- V-2 ★ 历史行**未被四项统计量捕获的改动**（用固定 id 上界重放 A-4）
--     期望：n_old / pl_len / id_len / tp_len 与 FINGERPRINT_BEFORE 逐项相等；keyed 必为 0
SELECT COUNT(*) AS n_old,
       MIN(id) AS min_id_old, MAX(id) AS max_id_old,
       SUM(LENGTH(COALESCE(raw_payload,''))) AS pl_len,
       SUM(LENGTH(COALESCE(msg_id,'')))      AS id_len,
       SUM(LENGTH(COALESCE(topic,'')))       AS tp_len,
       SUM(CASE WHEN dedup_key IS NOT NULL THEN 1 ELSE 0 END) AS keyed
  FROM mqtt_messages
 WHERE id <= :MAX_ID_BEFORE;
```

```sql
-- V-2b ★ `R-3` 固定 ID 抽样**逐行**复核（补充证据，不是全表证明）
--   第一步：执行前取 25 个 id **并存盘**（确定性：ORDER BY id，不用 RANDOM）
SELECT id FROM mqtt_messages WHERE id <= :MAX_ID_BEFORE
 ORDER BY id LIMIT 5 OFFSET 0;      -- 再取 OFFSET 1430 / 2870 / 4300 / (n_old-5)
--   ⇒ 把 25 个 id 写成常量列表 SAMPLE_IDS 保存（**这是关键**）
--
--   第二步：执行后用 **id 列表**重放（**不要**重跑 OFFSET —— 期间若有新增/删除，
--   OFFSET 会错位，抽到的就不是同一批行了）
SELECT id, device_id, COALESCE(direction,'-') AS dir, COALESCE(cmd,'') AS cmd,
       COALESCE(msg_id,'') AS msg_id,
       LENGTH(COALESCE(topic,'')) AS tp_len,
       LENGTH(COALESCE(raw_payload,'')) AS pl_len,
       substr(COALESCE(raw_payload,''),1,32) AS pl_head
  FROM mqtt_messages
 WHERE id IN (:SAMPLE_IDS);         -- 期望返回 **25 行**
-- 判据：25 行**全部**存在且逐项相等（msg_id / tp_len / pl_len / pl_head / device_id / dir / cmd）
-- ★ 只能写"抽样未发现改动"，**不得**写"历史数据绝对未变"（见下方局限声明）
-- ★ 若返回 < 25 行 ⇒ 有历史行被删除 ⇒ **P0**
```

```sql
-- V-3 索引可用性（必须能走索引，不能"建了但条件不匹配"）
SELECT COUNT(*) AS keyed_total FROM mqtt_messages WHERE dedup_key IS NOT NULL;  -- 期望 0
```

判定：

| 检查 | 期望 | 不符含义 |
|---|---|---|
| `V-1.col/idx/partial_idx/reg7` | `1/1/1/1` | 任一为 0 ⇒ 见 §3.4 / §3.7 |
| `V-1b.notnull / type / dflt_value / pk` | `0 / TEXT / NULL / 0` | 列属性不符 ⇒ **P0**，停 + 人工（`SQLite` 无法安全改列属性） |
| `V-1c.sql` | 与期望逐字一致（含 `WHERE dedup_key IS NOT NULL`） | 索引定义不符 ⇒ **P0**，停 + 人工 |
| `V-2.n_old/pl_len/id_len/tp_len` | **等于** `FINGERPRINT_BEFORE` | 不等 ⇒ 见 §3.4（**先排除并发与统计假象，再定 P0**） |
| `V-2.keyed` | `0` | ≠0 ⇒ 有人回填 ⇒ 违反"不回填"边界 |
| `V-2b` 25 行 | 返回 25 行且逐项相等 | 少于 25 行 ⇒ **有删除 ⇒ P0**；某行字段不等 ⇒ **P0** |
| `V-3.keyed_total` | `0` | ≠0 ⇒ 同上 |

★ **`COUNT(*)` 全表变化不参与判定** —— 生产每时每刻都在写入（`/api/ingest` 仍用旧代码在跑），
全表行数变化是**正常现象**。Rev.1 的"行数未变"判据会稳定误报，已废弃。

★★ **`R-3` 指纹能力边界（必须与结论一起写，不得省略）**

`V-2` 的四个统计量（`COUNT` / `MIN(id)` / `MAX(id)` / 三个长度和）**只能发现**：

| 能发现 | 不能发现 |
|---|---|
| 行被**删除**（`n_old` 变小） | **等长内容替换**（如把 `msg_id` 的 `A` 改成 `B`，长度不变 ⇒ 三项长度和全不变） |
| 行被**新增**在 `id ≤ MAX_ID_BEFORE` 区间（`n_old` 变大） | **未纳入指纹的字段**变化（`device_id` / `direction` / `cmd` / `created_at` / `timestamp`） |
| **长度和**变化（任意字段变长/变短） | **删除后补写**（删 1 行、再插入 1 行且长度合计恰好相同 ⇒ `n_old` 与三个和都不变） |
| 任意历史行被写入 `dedup_key`（`keyed ≠ 0`） | 索引存在但**未被使用**（需 `EXPLAIN QUERY PLAN`，本手册不要求） |

⇒ 因此 `V-2` 的正确结论措辞是：**"未被四项统计量捕获的历史改动为零"**，
**不是**"历史数据绝对未变"。`V-2b` 的 25 行抽样是对上表的**补充**，同样**不是证明**。

### 3.4 失败分类与处置（**本阶段已本地实测原子性**）

本地实测（`2026-10-21`，隔离 persist 目录，见审查报告 §C.3）：

| 实验 | 结果 |
|---|---|
| 库中**无** `mqtt_messages` 时 `migrations apply` | ❌ `no such table: mqtt_messages`；`0001`–`0006` 已登记，**`0007` 未登记** |
| 库中**有** `mqtt_messages`（fixture 同构）+ 已有重复历史行 | ✅ `0007` 应用成功并**已登记**；列/索引/部分条件齐备；历史行数不变；历史行 `dedup_key` 全 NULL |
| 故意让一个 migration 的**第 3 条语句**失败 | ❌ 整文件回滚：`CREATE TABLE` 与 `ALTER` 的效果**都不存在**，登记 0 行 |

⇒ **同一 migration 文件是原子的**：不会出现"列建了但索引没建"的半成品，
也不会出现"执行了但没登记"。因此**恢复路径只有一个方向：修好原因再 apply**，
不需要（也**禁止**）手工补 SQL。

三类失败必须分开判：

| 类别 | 现象 | 判据 | 处置 |
|---|---|---|---|
| **结构错误** | `apply` 报错（如 `no such table` / `duplicate column`），`apply_exit=1` | 命令非 0 退出 + `V-1` 不全为 1 | **停止**。先按 §3.7 判定落到哪个状态，再决定"人工处理后重试"还是 `STOP + HUMAN_REQUIRED` |
| **登记错误** | DDL 生效但 `d1_migrations` 无 0007 | `V-1.col/idx=1` 且 `V-1.reg7=0` | **停止**。属 §3.7 **state 4**（或 state 3）⇒ 见决策表，**不得自动修** |
| **伪成功** | `✅ No migrations to apply` + `apply_exit=0`，但结构缺失 | `V-1` 任一为 0 | **停止**。属 state 5/6 ⇒ 见决策表。★ 这是最容易误判为成功的一类 |
| **并发写入噪声** | 全表行数变了 | `V-2` 指纹**不变** | ✅ **正常**，不处置。只有指纹变化才是问题 |

★ 生产 D1 的原子性**未在本阶段验证**（官方语义为每个 migration 文件在事务中执行；
本地引擎实测一致）。⇒ C-3 必须把 `V-1` 作为**每 10 min 一次**的复检项，
而不是"执行完看一眼就完事"。

### 3.5 回滚（若 C-1 后判定必须撤）

```sql
DROP INDEX IF EXISTS ux_mm_dedup_key;
```

| 项 | 回滚后的真实状态 |
|---|---|
| 唯一索引 | 立即消失 ⇒ **后续**写入恢复"每次投递都落一行" |
| `dedup_key` 列 | **保留**（历史行为 NULL）。SQLite `DROP COLUMN` 在 D1 上不保证可用，**不尝试** |
| 已被 `DO NOTHING` 拦截的记录 | ❌ **无法恢复**。`DROP INDEX` 只影响未来写入 |
| 历史行 | 不删除、不改写、不复活 |
| `d1_migrations` 登记 | **不变**（仍是"已应用"）⇒ 回滚后系统处于 **`S5′`**（`reg7=1` + `idx=0`），**不是** S0 |

★ ★ 最后一个格子是 `R-5` 带来的新认识：**`DROP INDEX` 之后的状态是 `S5′`，不是"回到执行前"。**
后果：**再也不能**用正式 `apply` 恢复（`apply` 会报 `No migrations to apply` 且 `exit=0`），
只能靠一个**新的** migration 重建索引。⇒ 回滚不是"撤销"，而是"进入一个需要新 migration 的状态"。

★ 因此 C-1 单独回滚**几乎无收益**（去重本来要靠 C-2 的代码才会发生）：
若 C-1 后决定不上 C-2，回滚与否都不影响生产语义，建议**保留索引待用**，
只把"不部署 C-2"作为结果记录。

### 3.6 C-1 明确不做的事

- ❌ 不回填历史 `dedup_key`
- ❌ 不删除 / 不改写任何历史行
- ❌ 不修改 `0001`–`0006` 任何文件
- ❌ 不手工 `d1 execute` 执行 `0007` 的 SQL
- ❌ 不给 `0007` 补 `CREATE TABLE mqtt_messages`（会与生产实际 schema 分叉）
- ❌ 不手工 `INSERT/UPDATE/DELETE d1_migrations`（**登记表只能由 `migrations apply` 写**）
- ❌ 不通过删除历史行 / 重建生产表来"让 migration 通过"

---

### 3.7 `R-5` 六状态矩阵、判定与恢复路径（**本轮核心**）

#### 3.7.1 判定查询（`S-PROBE`，纯只读，执行前 + 失败后都用同一条）

```sql
SELECT (SELECT COUNT(*) FROM pragma_table_info('mqtt_messages') WHERE name='dedup_key') AS col,
       (SELECT COUNT(*) FROM sqlite_master WHERE type='index' AND name='ux_mm_dedup_key') AS idx,
       (SELECT COUNT(*) FROM sqlite_master WHERE name='ux_mm_dedup_key'
          AND sql LIKE '%WHERE dedup_key IS NOT NULL%') AS partial_idx,
       (SELECT COUNT(*) FROM sqlite_master WHERE name='ux_mm_dedup_key'
          AND sql LIKE '%ON mqtt_messages(dedup_key)%') AS col_ok,
       (SELECT COUNT(*) FROM d1_migrations
          WHERE name='0007_mqtt_messages_dedup.sql') AS reg7,
       (SELECT "notnull" FROM pragma_table_info('mqtt_messages') WHERE name='dedup_key') AS nn,
       (SELECT COALESCE(dflt_value,'<none>') FROM pragma_table_info('mqtt_messages') WHERE name='dedup_key') AS dflt,
       (SELECT type FROM pragma_table_info('mqtt_messages') WHERE name='dedup_key') AS ctype;
```

#### 3.7.2 六状态表（**每格都是本地实测或可推导，标注证据等级**）

证据等级：`[实测]` = 本轮 `state_matrix.sh` 在本地真实 D1 引擎上跑出来的；
`[文档]` = 依据 wrangler 官方行为/本仓库既有记录推断；`[未验]` = 生产未验证。

| 状态 | `col` | `idx` | `reg7` | 与 0007 期望的差异 | 正式流程能否继续 | 结论 |
|---|---|---|---|---|---|---|
| **S0** 全新 | 0 | 0 | 0 | 无 | ✅ **能**，`apply` 直接成功 | 正常执行路径 |
| **S1** 已成功 | 1 | 1 | 1 | 无 | ✅ 不需要（已完成） | 无需动作；只做 C-3 复检 |
| **S2**（不存在，见注） | — | — | — | — | — | 归入 S4 / S0 |
| **S3** 列在 / 索引缺 / 未登记 | 1 | 0 | 0 | 多了列 | ❌ **不能**：`duplicate column name` `[实测]` | **`STOP + HUMAN_REQUIRED`** |
| **S4** 列+索引在 / 未登记 | 1 | 1 | 0 | 结构与期望一致，但**没登记** | ❌ **不能**：`duplicate column name` `[实测]` | **`STOP + HUMAN_REQUIRED`** |
| **S5** 已登记 / 结构缺 | 0 | 0 | 1 | 结构全缺 | ❌ 不能"修"：`apply` 报 **`No migrations to apply` 且 exit=0** `[实测]` ⇒ **伪成功** | **`STOP + HUMAN_REQUIRED`**（需授权一个新 migration） |
| **S5′** 已登记 / 索引缺 | 1 | 0 | 1 | 缺索引 | ✅ **能**：追加一个**只建索引**的新 migration 即可 `[实测]` | 需人工决定并授权新 migration |
| **S6** 结构在但定义不符 | 1 | 1（`partial_idx=0`） | 0 或 1 | 列属性或索引 SQL 不符 | ❌ 视情况；`apply` 同样会撞列 | **`STOP + HUMAN_REQUIRED`** |

> 注：审查清单里列的"state 2（已成功应用）"与 `S1` 相同，故本表不再单列；
> `S5′` 是从"已登记但结构缺"里**额外拆出**的子态（只缺索引），因为它的恢复 DDL
> 与 `S5`（列与索引都缺）**不同**：`S5′` 只需建索引，`S5` 必须同时加列。

#### 3.7.3 ★ 关键实证：「`0008` 只建索引」在 S3 / S4 **不可执行**

这是本轮必须纠正的一个直觉。**直觉**：既然只缺索引，写个 `0008` 只建索引就行。
**实测**：

| 实验 | `0007` 未登记 + 列已存在 + 放入一个只建索引的 `0008` | 结果 |
|---|---|---|
| `[实测]` | `apply` 列出 **`0007` 和 `0008` 两个待应用** | 执行 `0007` ⇒ `duplicate column name` ⇒ **中止** |
| `[实测]` | 事后状态 | `col=1 idx=0 reg7=0` **`reg8=0`** ⇒ **`0008` 根本没被执行** |

⇒ **`d1 migrations apply` 严格按文件名顺序执行，遇到第一个失败就整体中止，不会"跳过"失败的
`0007` 去跑 `0008`。** 因此"新增 `0008` 只建索引"**不能**作为 S3/S4 的恢复方案。

而 `S5′`（`0007` **已登记**、只缺索引）里，`0008` 是**唯一**且**实测可行**的恢复路径：

| 实验 | `reg7=1` + `idx=0` + 放入只建索引的 `0008` | 结果 |
|---|---|---|
| `[实测]` | `apply` | `0008 ✅`，`2 commands`，`col=1 idx=1 partial_idx=1 reg8=1` |

⇒ **同一个 `0008` 文件，在 S3/S4 里无效，在 S5′ 里有效。** 差别只在于 `0007` 是否已登记。
这就是为什么"半成品状态"必须先判定、再决定方案，**不能预先写好一个通用 `0008`**。

#### 3.7.4 各状态的恢复方案与副作用（**逐态**）

| 状态 | 可否用正式流程恢复 | 若人工批准的恢复路径 | 副作用 / 风险 | 恢复后必须重跑 |
|---|---|---|---|---|
| **S0** | ✅ 直接 `apply` | — | — | `V-1`/`V-1b`/`V-1c`/`V-2`/`V-2b`/`V-3` |
| **S1** | 不需要 | — | — | `V-1` 复检 |
| **S3** | ❌ | **两条，均需人工**：<br>① 先 `DROP COLUMN`（需先无索引依赖）回到 S0，再正式 `apply`；<br>② 放弃 0007，改由一个**内容等价**的新 migration 覆盖（但它仍排在 0007 之后 ⇒ 无效）⇒ **实际只有 ①** | ① 是**破坏性 DDL**：`dedup_key` 列上若有值会**永久丢失**。<br>`DROP COLUMN` 在 D1 **生产是否可用未验证**（本地可用：先 `DROP INDEX` 再 `DROP COLUMN` 成功 `[实测]`） | 重跑全部六项 + 确认 `d1_migrations` 里只有一条 0007 |
| **S4** | ❌ | 同 S3 的 ①（先 `DROP INDEX` + `DROP COLUMN` 回 S0，再 `apply`） | 同上。★ 注意：**不要**用"手工补登记"绕过（禁止） | 同上 |
| **S5** | ❌ 直接 `apply` 无效（伪成功） | 新增 **`0008` 加列+建索引**（`[实测]` 可行，`3 commands`） | 登记链变成 `0007(空转)+0008(实际生效)` ⇒ **审计上要写清"0007 登记了但没生效"**；后续任何人读 `d1_migrations` 都会误以为 0007 起过作用 | `V-1`（`reg7=1` 且 `reg8=1`）+ `V-1b` + `V-1c` + `V-2` + `V-2b` + `V-3` |
| **S5′** | ✅ 可（新 migration） | 新增 **`0008` 只建索引**（`[实测]` 可行，`2 commands`） | 无数据风险（只建索引）；同上登记语义问题 | 同上（`reg8=1`） |
| **S6** | ❌ | **`STOP + HUMAN_REQUIRED`**。索引定义错：可 `DROP INDEX` 后用新 migration 重建（可逆）；**列属性错**（`NOT NULL`/`DEFAULT`/非 `TEXT`）：SQLite 无法原地改列 ⇒ 只能重建表 ⇒ **禁止在本项目生产上做** | 列属性错 = **不可就地恢复** | — |

★ **`S3`/`S4` 为什么必须是 `STOP + HUMAN_REQUIRED`**：唯一可行路径（`DROP COLUMN`）是
**破坏性**且**生产未验证**的，并且它改变的是"已经被写过的列"。这属于"操作人员不应在生产
执行时临时决定"的类别。**手册的职责是让他停下来，而不是给他一条没验过的命令。**

#### 3.7.5 判定流程图（文字版，执行时照此走）

```
S-PROBE
 ├─ reg7=0, col=0                  → S0  → 走 §3.2 正式 apply
 ├─ reg7=1, col=1, idx=1, 定义正确  → S1  → 已完成，不重复执行
 ├─ reg7=0, col=1                  → S3/S4 → STOP + HUMAN_REQUIRED（§3.7.4 路径① 需授权）
 ├─ reg7=1, col=0                  → S5  → STOP + HUMAN_REQUIRED（需授权新 migration）
 ├─ reg7=1, col=1, idx=0           → S5′ → STOP + HUMAN_REQUIRED（需授权"只建索引"新 migration）
 └─ 定义不符（partial_idx=0 / nn=1 / dflt≠NULL / type≠TEXT）
                                   → S6  → STOP + HUMAN_REQUIRED
```

★ 任何分支都**不允许**：手工改 `d1_migrations`、跳过未登记 migration、删历史行、重建表。

#### 3.7.6 证据等级声明（**必须随结论一起引用**）

| 结论 | 等级 |
|---|---|
| `duplicate column name` 会让 `apply` 失败 | `[实测]` 本地真实 D1 引擎 |
| 失败后 `reg7` 仍为 0、`0008` 不被执行 | `[实测]` |
| `S5` 下 `apply` 报 `No migrations to apply` 且 `exit=0` | `[实测]` |
| 只建索引的 `0008` 在 `S5′` 可成功 | `[实测]` |
| 只建索引的 `0008` 在 `S3/S4` 不可成功 | `[实测]`（因为 `0007` 先失败） |
| 加列+建索引的 migration 在 `S5` 可成功 | `[实测]` |
| **生产 D1 上的上述行为完全一致** | **`[未验]`** —— 生产从未进入过这些状态，本地证据不能替代生产验证 |
| 生产 `DROP COLUMN` 可用 | **`[未验]`** —— 本地可用，生产**未试过**，本手册要求**不在生产首次尝试** |

证据原文：`docs/evidence/G4-R1R5-Rev3-2026-10-21/state-matrix.log`
（可复现脚本 `state_matrix.sh`，隔离 persist + 假 `database_id`，未接触生产）。

---

## 4. C-2：Worker 部署

### 4.1 部署前：**version id 与 deployment id 必须分开记**

```bash
cd D:/Guo_Feeder_Project/Cloudflare_Assets/guo-feeder-api
npx wrangler deployments list          # 最近 10 次部署（含 deployment id 与对应 version id）
npx wrangler versions list             # 最近 10 个 version
npx wrangler deployments status        # 当前生效的版本组合
```

记录三样：`CURRENT_DEPLOYMENT_ID` · `CURRENT_VERSION_ID` · `PREV_VERSION_ID`（回退目标）。

★ **回退传的是 version id**（`wrangler rollback [version-id]`，`wrangler 4.145.0` 实测帮助文本）。
Rev.1 写的"回滚用 deployment id"是错的。

### 4.2 部署

```bash
npx wrangler deploy
```

### 4.3 部署后的版本生效判定（**部署成功 ≠ 生效**）

| 判据 | 方法 | 说明 |
|---|---|---|
| 版本登记 | `wrangler deployments status` 显示的 version id == 本次构建 | 只证明**控制面**切换，不证明行为 |
| 行为证据 | §4.4 的 P-1'…P-5 | **唯一**可接受的生效证据 |

★ HTTP 200 / "Deployment successful" / Worker invocation success **均不构成**数据已入库的证据。

### 4.4 探针设计（**副作用已逐条核实**）

前置事实（代码核实，见审查报告 §D.2）：

- `/api/ingest` 需 `X-Webhook-Key`（= `env.WEBHOOK_SECRET`），**成功返回纯文本 `OK`**（无计数）
  ⇒ 探针**无法**从响应判断写入结果，**必须**用 D1 行数验证
- `/api/ingest` **拒绝** `source="cloud_api"`（400）⇒ 探针**不可能**触发 MQTT 下发 / 设备动作
- 但 `source != cloud_api` 时会走 `handleRegistrySideEffects()` ⇒ 对 `up|log` 主题 + 合法 12-hex 设备段
  会 `upsertDeviceOnIngest()`：**`device` 表 INSERT（`CLAIM_PENDING`）或刷新 `last_seen`**
- `deviceIdFromTopic(topic,{directions:['up','log']})` 是建档闸门 ⇒ 设备段非法时**不建档**

| 探针 | 是否需要合成 | 写入影响 | 验证判据 | 失败即回滚 |
|---|---|---|---|---|
| **P-1' `$tenants` 归属** | ❌ **被动**（用真实 `$tenants` 回流） | 零 | 窗口内 `substr(topic,1,9)='$tenants/'` 的行 `device_id` **必须是合法 12-hex** | **仍是 `http_api` ⇒ 回滚**（B-1b 未生效） |
| **P-1'' plain 归属（回归）** | ❌ **被动** | 零 | plain `guo_feeder/<id>/*` 行仍为合法 12-hex | 退化为 `http_api` ⇒ 回滚 |
| **P-3' 方向** | ❌ **被动**（设备本来就在发 `/log`） | 零 | 窗口内 `direction='log'` 行数 > 0 | 全为 `unknown` ⇒ 回滚 |
| **P-2 哨兵** | ✅ 必须合成 | 1 行 `device_id='unresolved'`；**不**建 device 行 | 该行 `device_id='unresolved'`（不是 `BADID`、不是 `http_api`） | 落成 `BADID`/`http_api` ⇒ 回滚 |
| **P-4 去重** | ✅ 必须合成（**同 payload 连发 3 次**） | 1 行；`device.last_seen` 前移 | `COUNT(*) WHERE msg_id='<P4 标记>'` **= 1** | 3 行 ⇒ 回滚（去重未生效） |
| **P-5 无 `msg_id`** | ✅ 必须合成（**同 payload 连发 2 次**） | 2 行；`last_seen` 前移 | 2 行且两行 `dedup_key` 均 **NULL** | 1 行 ⇒ **严重**（payload 又被当身份）⇒ 立即回滚 |

★★ **`P-1'` 的基线已更正（`2026-10-21 00:55`）**：原先以为"v47 已经会解析 `$tenants`，
`P-1'` 只是回归检查"——**错了**。实测：全库 **1 539 行** `$tenants/*/down` 的 `device_id`
**全部是 `http_api`**，最新一条就在刚刚。⇒ `P-1'` 是**真正的新行为检查**，基线是"错的"。
证据：`G4-S1-Live-Worker-Version-Evidence-2026-10-21.md` §4.1-B。

★ **预期会变坏的一类行（不得判为故障）**：plain 主题里现有 `device_id='phone'`（16 行）
与 `hd1_run_*`（7 行）——都是**非 12-hex 的 clientid 回落**。
`B-1b` 之后 `clientid` 只在"合法 12-hex"时才被采信 ⇒ 这些行会变成 `unresolved`。
**这是设计意图**（拒绝伪归属），C-2 验收时应作为**预期变化**登记，而不是回归。

**探针消息模板（`payload` 必须**逐字节相同**，否则 P-4 的键不同、必然 3 行）**

```json
P-2  topic="guo_feeder/BADID/up"
     payload={"c":"probe","i":"G4PROBE-P2-1729500000","m":"G4PROBE-P2"}

P-4  topic="guo_feeder/<REAL_DEV>/up"     ← 用真实设备 ID，避免新建 device 行
     payload={"c":"probe","i":"G4PROBE-P4-1729500000","m":"G4PROBE-P4"}   ×3 次（同一字节串）

P-5  topic="guo_feeder/<REAL_DEV>/up"
     payload={"c":"probe","m":"G4PROBE-P5-1729500000"}                     ×2 次（**无 `i`**）
```

**标记与隔离规则（**不得删除生产记录**）**

- 所有合成探针的 `raw_payload` 都含 `G4PROBE-`；P-5 没有 `msg_id`，因此**只能**用 payload 标记
- 全部 C-3 查询统一加 `AND COALESCE(raw_payload,'') NOT LIKE '%G4PROBE-%'`
- 探针行**永久保留**（`mqtt_messages` 不在 retention 范围）；只统计、不删除
- P-4/P-5 用真实设备 ID 的代价：`device.last_seen` 被前移到探针时刻。
  若设备此刻**离线**，这会污染"离线时长"判断 ⇒ **只在设备在线时做**；
  若必须离线做，改用合成 12-hex（`0b0077a7e402`），但必须接受 `device` 表**新增一行**
  `CLAIM_PENDING`（`device` 表 retired 语义不受影响，因为它是新设备）

★ 任何探针只要**无法证明**不会触发设备动作，就**不执行**，并把该判据标 `NOT RUN`。

### 4.5 观测可用性（**决定探针结论是否可信**）

| 通道 | 状态 | 依据 |
|---|---|---|
| Workers Logs（`[mqtt-dedup] outcome=…`） | **UNVERIFIED** | `wrangler.toml` 已 `[observability.logs] persist=true`；历史查询 API 返回 **403**（凭据权限），本阶段未重新验证 |
| `wrangler tail` | **UNVERIFIED** | 历史上 **502**；本阶段未试 |
| 只读 SQL（D1） | ✅ **可用**（本阶段已实测 `d1 execute --remote` 直连） | 作为 P-2/P-4/P-5 的**主**验证通道 |
| EMQX 运行指标 | ❌ **403** | 只读 `GET /clients*` 可用；数据集成/规则 API 全 403 |

⇒ **纪律**：P-2/P-4/P-5 的判定**只**用 D1 行数；
若要用 `outcome=duplicate` 计数，必须**先**证明 Logs 或 `tail` 可用，
否则该项标 `NOT RUN`（**不得**因为"部署成功 + 表里有 1 行"就宣称去重率已测）。

### 4.6 C-2 失败判据与回滚

**失败判据**（任一成立 ⇒ 回滚）

1. P-4 落 3 行（去重未生效）或 P-5 落 1 行（**误合并**，最严重）
2. P-1' 窗口内出现 `device_id='http_api'`（归属修复未生效）
3. `V-1.reg` 在部署后变为 0 / 索引消失（migration 被回滚）
4. `log_history` 增速相对部署前 30 min 基线下降 > 50%
5. 设备侧 `counters.give_up` / `drop_unacked` 在窗口内增长
6. `dedup_key` 唯一约束**抛出**（而非 `DO NOTHING`）⇒ 索引与写入不匹配

**回滚**

```bash
npx wrangler rollback <PREV_VERSION_ID> --message "G4 C-2 rollback: <原因>"
```

- 语义：回到旧代码 ⇒ **不再去重**（回到"每次投递都落一行"）
- ★ 回滚**不**改动 `0007` 的列与索引（旧代码不写该列，无害）
- ★ 回滚**不能**复活部署期间被 `DO NOTHING` 丢弃的独立事件（见 §7）

---

## 5. C-3：观察窗口与只读对账

### 5.1 单位与语义表（**先看这张表再看 SQL**）

| 表 / 字段 | 单位 | 语义 | 陷阱 |
|---|---|---|---|
| `mqtt_messages.created_at` | **毫秒**（`Date.now()`） | Worker 写入时刻 | 与 `log_history` 差 1000 倍 |
| `mqtt_messages.timestamp` | **秒**（`time_get()`），可能 NULL | 设备 `t` 字段 | ★ `t` **被复用**：分片 BEGIN 帧里是**消息类型**；`log_ack` 的 `p.t` 是 `seq_to`。设备时钟未同步时**不写** |
| `mqtt_messages.dedup_key` | 64 hex | 幂等键 = `SHA256(source‖dev‖dir‖cmd‖id:msgId‖ph:SHA256(payload))` | SQLite **无 SHA256** ⇒ 无法在 SQL 里重算 |
| `log_history.created_at` | **秒**（`DEFAULT (unixepoch())`） | 入库时刻 | — |
| `log_history.ts` | 秒 | 设备日志时刻 | — |
| `log_history` 唯一键 | — | `(device_id, boot_seq, seq)` ⇒ `ux_lh_dedup` | — |
| `log_conflict.*_at` | 秒 | 冲突首次/末次 | — |
| `log_boot_watermark.*_at` | 秒 | 水位 | `LOG_BOOT_WATERMARK="1"` **已启用** |

### 5.2 最终 SQL（**可直接执行**）

占位符：`:ts0_ms` 窗口起点（毫秒）· `:ts0_s` 窗口起点（秒）· `:dev` 设备 ID · `:boot` boot_seq。
**所有 `mqtt_messages` 查询都带探针排除** `AND COALESCE(raw_payload,'') NOT LIKE '%G4PROBE-%'`。

```sql
-- Q-1 归属分布（部署后 +1 h）
SELECT CASE WHEN length(device_id)=12 AND device_id NOT GLOB '*[^0-9a-f]*'
            THEN 'valid' ELSE 'invalid' END AS k,
       device_id, direction, COUNT(*) AS n
  FROM mqtt_messages
 WHERE created_at >= :ts0_ms
   AND COALESCE(raw_payload,'') NOT LIKE '%G4PROBE-%'
 GROUP BY k, device_id, direction
 ORDER BY n DESC LIMIT 30;
-- 期望：valid 占绝对多数；http_api 为 0；unresolved 仅可能来自真实畸形 topic
```

```sql
-- I-1 ★ 索引完整性**不变量**（不是"重复率"）：期望 0
SELECT COUNT(*) AS dup_key_groups FROM (
  SELECT dedup_key FROM mqtt_messages
   WHERE dedup_key IS NOT NULL
   GROUP BY dedup_key HAVING COUNT(*) > 1);
```

```sql
-- I-2 ★ 无降级证据：有 msg_id 的行必须**全部**有键；期望 null_key = 0
SELECT SUM(CASE WHEN dedup_key IS NULL THEN 1 ELSE 0 END) AS null_key,
       COUNT(*) AS total
  FROM mqtt_messages
 WHERE created_at >= :ts0_ms AND msg_id IS NOT NULL AND msg_id <> ''
   AND COALESCE(raw_payload,'') NOT LIKE '%G4PROBE-%';
```

```sql
-- A-1 近似"重复候选"（**只能叫近似**，见 §5.3 N-2）
SELECT COUNT(*) AS groups, COALESCE(SUM(n-1),0) AS extra_rows FROM (
  SELECT COUNT(*) AS n
    FROM mqtt_messages
   WHERE created_at >= :ts0_ms AND msg_id IS NOT NULL AND msg_id <> ''
     AND COALESCE(raw_payload,'') NOT LIKE '%G4PROBE-%'
   GROUP BY CASE WHEN substr(topic,1,9)='$tenants/' THEN 'echo' ELSE 'audit' END,
            device_id, direction, COALESCE(cmd,''), msg_id
  HAVING COUNT(*) > 1);
-- 期望：0（若 >0，先查是否为"同 msg_id 不同 payload"的**合法**两行）
```

```sql
-- Q-3 入库延迟（秒）；`n` 必须与极值一起报
SELECT COUNT(*) AS n,
       MIN(CAST(created_at/1000 AS INTEGER) - timestamp) AS min_s,
       MAX(CAST(created_at/1000 AS INTEGER) - timestamp) AS max_s,
       AVG(CAST(created_at/1000 AS INTEGER) - timestamp) AS avg_s
  FROM mqtt_messages
 WHERE created_at >= :ts0_ms
   AND timestamp BETWEEN 1600000000 AND (CAST(created_at/1000 AS INTEGER) + 300)
   AND COALESCE(raw_payload,'') NOT LIKE '%G4PROBE-%';
-- `timestamp` 区间过滤：剔除"消息类型"与"设备时钟未同步"的伪值
-- 结论只能写"趋势"，因为含设备时钟偏差
```

```sql
-- Q-4 log_history 5 分钟桶（`created_at` 单位 = 秒）
SELECT (created_at - (created_at % 300)) AS bucket_start_s,
       COUNT(*) AS n, COUNT(DISTINCT seq) AS uniq_seq
  FROM log_history
 WHERE device_id = :dev AND boot_seq = :boot AND created_at >= :ts0_s
 GROUP BY bucket_start_s ORDER BY bucket_start_s DESC LIMIT 12;
-- ★ 这套表达式**不得**用于 mqtt_messages（毫秒）⇒ 那会是 1000 倍错
```

```sql
-- Q-4b 窗口计数（对账用；`rows` 必须 == `uniq_seq`）
SELECT COUNT(*) AS rows, COUNT(DISTINCT seq) AS uniq_seq,
       MIN(seq) AS min_seq, MAX(seq) AS max_seq
  FROM log_history
 WHERE device_id = :dev AND boot_seq = :boot AND created_at >= :ts0_s;
```

```sql
-- Q-5 `R-6` 归因分布（**必须先看方向，再谈 audit/echo**）
--     ★ Rev.2 的错误：不分方向就要求 "audit > 0 且 echo > 0" ⇒ 对 up 永远误报
SELECT COALESCE(direction,'-') AS dir,
       CASE WHEN substr(topic,1,9)='$tenants/' THEN 'echo' ELSE 'audit' END AS src,
       COUNT(*) AS n
  FROM mqtt_messages
 WHERE created_at >= :ts0_ms
   AND COALESCE(raw_payload,'') NOT LIKE '%G4PROBE-%'
 GROUP BY dir, src
 ORDER BY dir, src;
-- 期望（`2026-10-21` 只读取证确定的真实分布）：
--   down : audit **可能** >0 / **可能** =0（见下方 R-6 说明）· echo > 0
--   up   : audit > 0 · echo **恒为 0**（**不是缺陷**）
--   log  : 视路由；**不得**套用 up/down 的 audit/echo 规则
--   unknown : 期望窗口内 = 0（旧路由残留）
```

```sql
-- Q-5a ★ `down` 方向专项（**唯一**适用"audit 与 echo 双写"的方向）
SELECT COUNT(*) AS n_down,
       SUM(CASE WHEN substr(topic,1,9)='$tenants/' THEN 1 ELSE 0 END) AS n_echo,
       SUM(CASE WHEN substr(topic,1,9)<>'$tenants/' THEN 1 ELSE 0 END) AS n_audit
  FROM mqtt_messages
 WHERE created_at >= :ts0_ms AND direction='down'
   AND COALESCE(raw_payload,'') NOT LIKE '%G4PROBE-%';
-- 硬判据：`n_echo > 0`（下行回环仍在）· 窗口 ≥ 10 min（echo 实测晚 33–37 s）
-- 软判据：`n_audit` —— **不得**因它为 0 而判失败，见下方说明
```

```sql
-- Q-5b ★ `up` 方向专项（**明令禁止**要求 echo）
SELECT COUNT(*) AS n_up,
       SUM(CASE WHEN substr(topic,1,9)='$tenants/' THEN 1 ELSE 0 END) AS n_echo_up,
       COUNT(DISTINCT device_id) AS devs,
       SUM(CASE WHEN msg_id IS NULL OR msg_id='' THEN 1 ELSE 0 END) AS no_msg_id
  FROM mqtt_messages
 WHERE created_at >= :ts0_ms AND direction='up'
   AND COALESCE(raw_payload,'') NOT LIKE '%G4PROBE-%';
-- 硬判据：`n_up > 0`（上行业务仍在）· 归属合法（Q-1）
-- ★ `n_echo_up` **不参与任何判定**（全库从无 echo/up；为 0 是正确状态）
-- ★ `no_msg_id` 是**近似**指标（≈15%），不是重复率（见 A-2）
```

```sql
-- Q-5c ★ `log` 方向：**不套用** audit/echo，改看日志契约
SELECT COUNT(*) AS rows_mm, COUNT(DISTINCT device_id) AS devs
  FROM mqtt_messages
 WHERE created_at >= :ts0_ms AND direction='log'
   AND COALESCE(raw_payload,'') NOT LIKE '%G4PROBE-%';
-- `log` 的**真正**判据在 `log_history`（Q-4b / I-3 / I-6），不在 `mqtt_messages`
-- ★ 禁止用 `mqtt_messages` 当 `log` 完整性的唯一证据（B-3 未定，且该表自 2026-10-16 起
--   只收 up/down，`/log` 走 `log_history`）
```

```sql
-- Q-5d ★ `unknown` 方向：只用于"旧路由残留"检测，**不得混入**正常计数
SELECT COUNT(*) AS n_unknown, MIN(created_at) AS first_ms, MAX(created_at) AS last_ms
  FROM mqtt_messages
 WHERE created_at >= :ts0_ms AND COALESCE(direction,'') NOT IN ('up','down','log');
-- 期望：0。若 >0 ⇒ 有未归类的路由在写库 ⇒ 记录并查 `direction` 赋值分支
```

★★ **`R-6` 为什么必须这么改（证据）**

| 事实 | 证据 |
|---|---|
| 全库 `echo` 仅存在于 `direction='down'`，累计 **1 537** 行 | `G4-S2-B3-EMQX-Evidence-2026-10-21.md` §4.1 |
| 全库**从未**出现 `echo` + `up` | 同上（`echo/up = 0`，含历史） |
| 原因：`$tenants/...` 副本**只有下行**（EMQX 侧只有一个方向的重发布规则） | 同上 §4.1 的规则对照 |
| `mqtt_messages` 自 `2026-10-16 01:20` 起**只收** `up`/`down`（`/log` 走 `log_history`） | 同上 §3 |
| `audit/down` 在 `2026-10-20 02:50`（+08）之后归零，**原因未查明** | 同上 §4.2 |

⇒ 因此 `up` 上"audit 与 echo 都 > 0"是**用错误前提写的判据**，会 100% 误报。
⇒ 而 `down` 上的 `n_audit` 也**不能**作硬判据（`audit/down` 已归零且原因未定）。
**`down` 的硬判据只能是 `n_echo > 0`**（下行回环确实在跑）。

★ **反方向声明（必须随结论一起写）**：
`echo` 判据的收窄**不证明** `up` 方向的审计/去重/入库链路正确，**也不证明** B-3 已解决。
`up` 的正确性靠 `I-1` / `I-2` / `A-1` / `Q-1` 与设备侧计数对账（`I-6` / `I-7`）来判，
**不靠** `echo` 是否存在。

```sql
-- Q-6 冲突与水位（利用已启用的 LOG_BOOT_WATERMARK；单位 = 秒）
SELECT COUNT(*) AS collision_rows, COALESCE(SUM(seen_count),0) AS seen
  FROM log_conflict WHERE kind='collision' AND last_seen_at >= :ts0_s;
SELECT device_id, max_boot_seq, rewind_count, ambiguous_count, status, updated_at
  FROM log_boot_watermark WHERE device_id = :dev;
-- 期望：collision_rows 新增 = 0；rewind_count 不增；max_boot_seq 与设备侧 :boot 一致
```

```sql
-- Q-7 探针行统计（**只统计，不删除**）
SELECT COUNT(*) AS probe_rows FROM mqtt_messages
 WHERE COALESCE(raw_payload,'') LIKE '%G4PROBE-%';
```

### 5.3 严格不变量 / 近似指标 / 不可测项（**必须按这三类分开写结论**）

**严格不变量**（违反 = 缺陷，必须回滚或升级）

| ID | 不变量 | 依据 |
|---|---|---|
| **I-1** | `dedup_key` 重复组 = 0 | 部分唯一索引 |
| **I-2** | 有 `msg_id` 的行 `dedup_key` 全非 NULL | `B-2b` 后键只由 `msg_id` 决定 |
| **I-3** | `log_history.rows == COUNT(DISTINCT seq)` | 唯一键 `(device, boot_seq, seq)` |
| **I-4** | 窗口内设备 `boot_seq` **不变** | `acked_seq` / `counters.*` 均为 RAM，重启归零 |
| **I-5** | `acked_seq` 窗口内**单调不减** | 设备侧高水位只增 |
| **I-6** | `rows(窗口内 log_history) ≤ Δcloud_records_sent` | 云端不可能有设备**从未首发**的记录 |
| **I-7** | `Δcloud_ack_ok ≤ Δcloud_batch_sent` | ACK 批次 ⊆ 发送批次 |
| **I-8** | `direction='down'` 窗口内 `echo` 行 > 0（`Q-5a.n_echo`） | 下行 `$tenants` 回环仍在；`source` 进键 ⇒ audit/echo 不可合并 |
| **I-8b** | `direction='up'`：**不要求** `echo`；要求 `n_up > 0` 且归属合法 | `echo` 只存在于下行（全库 1 537 行全为 `down`）⇒ 对 `up` 要求 echo = 永久误报（`R-6`） |
| **I-8c** | `direction='log'`：不套用 audit/echo；以 `log_history`（`I-3`/`I-6`）+ 设备计数判 | `/log` 走 `log_history`，`mqtt_messages` 自 `2026-10-16` 起只收 `up`/`down` |
| **I-8d** | `direction='unknown'` 窗口内 = 0 | 旧路由残留；若 >0 说明有未归类写入 |
| **I-8e** | `direction='down'` 的 `audit`（`Q-5a.n_audit`）：**不得**作为硬判据 | `audit/down` 自 `2026-10-20 02:50` 起归零，**原因未查明**（`B-3` 相关，独立取证中） |
| **I-9** | `d1_migrations` 中 `0007` = 1、索引在 | C-1 持续有效 |

**近似指标**（只能作趋势，必须附解释）

| ID | 指标 | 为什么只能是近似 |
|---|---|---|
| **A-1** | `Δcloud_records_sent − rows`（"发出未落库"差） | 含在途批次、被 `filtered/rejected`、被去重、Flash 补发；这些分量**当前不可观测** |
| **A-2** | `dedup_key IS NULL` 占比 | 真实含义 = "无 `msg_id` 的上行比例"（历史约 15%），不是重复率 |
| **A-3** | 入库延迟 `Q-3` | 含设备时钟偏差；`timestamp` 缺省时该行被排除 ⇒ 有选择偏差 |
| **A-4** | 非法归属占比 `Q-1` | `unresolved` 是**新增**语义，历史行里没有 |
| **A-5** | `offline_skip` | 是**轮数**不是条数，离线时膨胀极快（实测 11 s ⇒ 6 065），**不得**外推为"离线日志条数" |

**不可测项**（必须在报告里写 `NOT MEASURABLE`，不得用别的量代替）

| ID | 项 | 原因 |
|---|---|---|
| **N-1** | 云端 `accepted/duplicate/rejected/filtered` 逐请求计数 | `/api/history/ingest` **不打印**这四个计数（响应只回给 EMQX Action，不经我们） |
| **N-2** | 真实重复投递率 | 被唯一索引拦截 ⇒ D1 里看不到；只有 Worker 日志 `outcome=duplicate` 可见（Logs/tail **未验证**） |
| **N-3** | B-3 漏记的分层定位 | EMQX 数据集成/规则 API **403** |
| **N-4** | ingest 5xx 率 | 同上（Logs 403）；只能间接看 `mqtt_messages` 写入速率骤降 |

★ **Rev.1 的四个等式全部作废**，原因见审查报告 §E.4：
`①≈③.accepted+③.duplicate`（③不可观测）· `②≈③.accepted`（**量纲错**）
· `④==③.accepted 累计`（③不可观测）· `Q-2 同 msg_id 重复率`（语义错）。

### 5.4 设备侧锚点采集（串口只读，**不发任何指令**）

窗口开始 / 结束各一次：

```
cm {"cmd":"system","ob":"stats","id":"g4c3-<t0|t1>"}
```

记录：`boot_seq` · `queue.acked_seq` · `counters.cloud_records_sent` ·
`counters.cloud_batch_sent` · `counters.cloud_ack_ok` · `counters.cloud_ack_timeout` ·
`counters.cloud_retx` · `counters.cloud_ack_lost` · `counters.give_up` ·
`counters.drop_overflow` · `counters.drop_unacked` · `counters.offline_skip` · `counters.emit_total`

★ `available:false` 时 `counters` **整段缺席** ⇒ 按**键存在性**判可用，**不得当 0**。
★ 计数器全是 RAM、**重启归零** ⇒ 先确认 `boot_seq` 未变（I-4），否则该窗口作废重来。

### 5.5 窗口与停止条件

- 长度：部署后 **≥ 2 小时**（覆盖 ≥ 2 个 `$tenants` 回环周期 + ≥ 1 次设备重连）
- 采样：`+10 min` / `+30 min` / `+1 h` / `+2 h` —— 每次跑 §5.2 全部 SQL + §5.4
- 停止（任一 ⇒ 立即回滚并记录）
  1. `I-1` / `I-2` / `I-3` 任一违反
  2. P-5 落 1 行（误合并）
  3. `log_history` 增速下降 > 50%
  4. **`I-8` 违反**：`direction='down'` 的 `echo` 归零（窗口 ≥ 10 min 后）。
     ★ **只判 `down` 的 `echo`**：`up` 没有 `echo` 是**正确状态**，不得据此停止
     （Rev.2.0 写的是"audit 或 echo 任一归零"，会把 `up` 的正常情况当故障 ⇒ `R-6` 已修）
  5. `dedup_key` 唯一约束抛错
  6. 设备 `give_up` / `drop_unacked` 增长
  7. `boot_seq` 在窗口内变化（⇒ 计数器归零，窗口作废，按需重启窗口）
  8. `direction='unknown'` 新增 > 0（`I-8d`）

★ **本 2 小时窗口 ≠ `S-3-L3` 的 2.5 小时门禁窗口**。两者目的不同、前置不同、**不得互相替代**。

---

## 6. 门禁与未解决项（**本轮一条都不动**）

### 6.1 门禁状态（保持）

`BT-1 = CLOSED` · **`G-4 = BLOCKED`** · `WP-5` **未解锁** · `STOP-R1` **无解除文件**。
完成 C-1/C-2/C-3 **不等于** `G-4 PASS`，也**不解锁** `WP-5`。

### 6.2 B-3（漏记）保持"根因未定"

- B-1/B-2 的修复**不代表** B-3 已解决（三件独立问题）
- `mqtt_messages` **不得**作为日志完整性的唯一证据
- 在拿到 EMQX 控制台只读统计（规则 matched / Action failed·retry / 连接器成功率 / 队列丢弃）之前，
  只允许写"漏记存在，根因未定（分层候选：规则匹配 / Action 队列 / HTTP 重试 / Worker 侧）"

### 6.3 明确不碰的开关

- `LOG_BOOT_WATERMARK`：`wrangler.toml` 自 `362ddfd`（`2026-10-19`）起 = `"1"`，**已启用**。
  本轮**不修改**该值（既不开也不关）。Rev.1 与本报告早期文本里"默认关闭"的说法**已更正**。
- `G4-R4` 的 H-1…H-5 候选阈值：**仍未批准**，`system.memwatch` 不据此判 PASS/FAIL。

---

## 7. 回滚的**不对称影响**（必须写进变更记录）

> ★ `DROP INDEX` / `rollback` 都只能恢复**未来**的写入行为。
> 它们**不能**让部署期间被 `DO NOTHING` 丢掉的独立业务事件复活。

| 场景 | 回滚后 |
|---|---|
| 真重复投递被 `DO NOTHING` 丢弃 | ✅ 无损失（本来就该丢） |
| **误合并**（P-5 若落 1 行） | ❌ **永久丢失**，D1 无法恢复 |
| 回滚前已写入的行 | 不变（不删、不改） |
| `dedup_key` 列 | 保留（NULL 或已有值），旧代码不读 |
| `log_history` / `telemetry_history` | **完全不受影响**（独立契约） |

**本轮把"误合并"从"可能大量发生"压到"结构性不可能"**：`msg_id` 缺失 ⇒ 不产生键 ⇒ 不去重（`B-2b`）。
生产只读统计证明该风险真实存在（无 `msg_id` 1 696 行中 1 613 行落在 167 个同 payload 组，
按 payload 去重会吞 **1 446 行 / 85%**）。

---

## 8. 独立审批表（**不得合并为一个默认授权**）

### C-1 审批

| 项 | 内容 |
|---|---|
| 变更范围 | `ALTER TABLE mqtt_messages ADD COLUMN dedup_key TEXT` + `CREATE UNIQUE INDEX ux_mm_dedup_key … WHERE dedup_key IS NOT NULL`（**仅此两条语句**，无其它 DDL） |
| 执行前检查 | §2.A **四元身份全等**（`R-1`）· §2.C `0007` 三方哈希相等（`R-2`，`90dc0ec0…2f1e` / blob `eb300070…75d2`）· 清单 `2.1`–`2.6` 全绿 · §3.7 `S-PROBE` 落在 **`S0`**（`R-5`） |
| 成功判据 | `V-1` = `1/1/1/1` · `V-1b` = `notnull 0 / TEXT / dflt NULL / pk 0`（`R-4`）· `V-1c` 索引 `sql` 全文与期望逐字一致（`R-4`）· `V-2` 指纹与 `FINGERPRINT_BEFORE` 逐项相等且 `keyed=0`（`R-3`）· `V-2b` 25 行抽样逐项相等（`R-3`）· `V-3` = 0 |
| 失败判据 | `apply_exit ≠ 0`；或 `V-1` 任一为 0；或 `V-1b`/`V-1c` 不符；或 `V-2` 指纹变化；或 `V-2b` 不等。★ **`apply_exit=0` 与 `No migrations to apply` 都不构成成功** |
| 失败处置 | 按 §3.4 分类 → §3.7.2 定状态 → §3.7.4 查恢复路径。`S3`/`S4`/`S5`/`S6` 一律 **`STOP + HUMAN_REQUIRED`**；**不自动**跑 `0008`、**不自动**重跑 `0007`、**不** DROP 任何东西 |
| 回滚 | `DROP INDEX IF EXISTS ux_mm_dedup_key;`（列保留）。★ 回滚后状态是 **`S5′`**，不是 S0；恢复需新 migration |
| 所需授权 | 人工**书面**批准 C-1（`R-1`…`R-5` 关闭后才有意义） |
| 风险 | ① 建索引期间 `mqtt_messages` 写入短暂变慢（行数 5 743 级，可忽略）<br>② 若 `dedup_key` 列被意外加过默认值 ⇒ NULL 语义被破坏（`V-1b` 拦）<br>③ 若并发写入使 `V-2` 误报 ⇒ 按 §3.4 先排除再定 |

### C-2 审批

| 项 | 内容 |
|---|---|
| 变更范围 | 部署 `guo-feeder-api` 当前 `main`（含 B-1/B-1b/B-2/B-2b/B-2c）；**不改** `wrangler.toml` 任何 `vars` |
| 执行前检查 | C-1 已成功；§2.7 已记 `CURRENT_VERSION_ID` / `PREV_VERSION_ID`；§4.5 观测通道现状已记 |
| 成功判据 | P-1' 无 `http_api`；P-3' 有 `direction='log'`；P-2 落 `unresolved`；P-4 = 1 行；P-5 = 2 行且键 NULL |
| 失败判据 | §4.6 六条任一 |
| 回滚 | `npx wrangler rollback <PREV_VERSION_ID> --message "…"` |
| 所需授权 | 人工**书面**批准 C-2（**在 C-1 成功之后**） |

### C-3 审批

| 项 | 内容 |
|---|---|
| 变更范围 | **只读**：§5.2 SQL + §5.4 串口只读 + EMQX 控制台只读。不写、不删、不改配置 |
| 执行前检查 | 窗口负责人已定；`boot_seq` 基线已采；采样时间表已定；**`R-6` 已关闭**（`echo` 判据按方向收窄，见 §5.2 `Q-5`/`Q-5a`–`Q-5d`） |
| 成功判据 | 4 次采样 `I-1`…`I-9`（含 `I-8`/`I-8b`/`I-8c`/`I-8d`/`I-8e`）全成立；近似指标（`A-1`…`A-5`）有解释；不可测项如实标 `NOT MEASURABLE` |
| 失败判据 | §5.5 停止条件任一 |
| 回滚 | 不适用（只读）；若触发停止条件，按 C-2 回滚流程走 |
| 所需授权 | 人工**书面**批准 C-3 |

---

## 9. 附：本文件依据的代码事实（可复核）

| 事实 | 位置 |
|---|---|
| `0007` 两句 DDL | `migrations/0007_mqtt_messages_dedup.sql:43,46-48` |
| `mqtt_messages.created_at = Date.now()`（毫秒） | `src/index.js:653` |
| `timestamp = msg.t` | `src/index.js:591-592` |
| `direction` 含 `log` | `src/index.js:524-537` |
| `/api/ingest` 需 `X-Webhook-Key`；成功返回 `OK` | `src/index.js:284-295, 746-751` |
| `/api/ingest` 拒绝 `source="cloud_api"` | `src/index.js:388-396` |
| 建档副作用 `upsertDeviceOnIngest()` | `src/index.js:1234-1257` · `src/device-registry.js:333-360` |
| `[mqtt-dedup] outcome=` 日志 | `src/index.js:656-659` |
| 去重键公式（`msg_id` 必需） | `src/mqtt-dedup.js:141-167` |
| `ON CONFLICT(dedup_key) WHERE … DO NOTHING` | `src/mqtt-dedup.js:240-243` |
| 降级标志是**模块级**（isolate 生命周期） | `src/mqtt-dedup.js:182-196` |
| `log_history.created_at DEFAULT (unixepoch())` | `migrations/0003_p3_history_tables.sql:100` |
| `ux_lh_dedup (device_id, boot_seq, seq)` | `migrations/0003_p3_history_tables.sql:126-127` |
| `[observability.logs] persist=true` | `wrangler.toml` |
| `LOG_BOOT_WATERMARK="1"`（`362ddfd`，2026-10-19） | `wrangler.toml:40` |
| `cloud_ack_ok` 是**批次**计数 | `src/log/log_manager.cpp:3399` |
| `cloud_records_sent` 仅首次发出累加 | `src/log/log_manager.cpp:3660-3663` |
| `seq` 在 emit 时分配（含 DEBUG） | `src/log/log_manager.cpp:2712-2715` |
| `t` 在分片 BEGIN 帧里是消息类型 | `src/cloud/cloud_manager.cpp:900` |
| `t` 在 result/ack 帧里是 `time_get()` 秒 | `src/cloud/cloud_manager.cpp:1195,1216,1301` |
| `wrangler rollback [version-id]` | `wrangler 4.145.0 --help`（本阶段实测） |
