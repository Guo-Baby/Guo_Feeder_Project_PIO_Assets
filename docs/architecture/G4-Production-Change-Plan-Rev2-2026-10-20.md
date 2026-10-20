# G4 生产变更计划 · Rev.2（云端 `0007` + Worker 部署 + 生产验证）

- 日期：`2026-10-21`
- 状态：**REVISED —— 仍为申请，未执行任何生产变更**
- 取代：`G4-Production-Change-Plan-2026-10-20.md`（Rev.1，保留原文作审计留痕）
- 审查依据：`G4-Production-Change-Plan-Review-2026-10-20.md`（A–G，本文件的每条修改理由都在那里）
- 目标库 / 目标 Worker：D1 `guofeeder`（binding `guofeeder_DB`）· `guo-feeder-api`

> ★ 本文件是**执行手册**，全部最终 SQL 只在这里出现一份（避免两份副本漂移）。
> 审查过程与"原 SQL 错在哪"见审查报告；本文件只写**要执行什么、怎么判、怎么退**。

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

★ 2.5 的 `sqlite_master.sql` 原文必须与本地 fixture
（`test/real_d1/fixtures/mqtt_messages.sql`）**逐列一致**；不一致 ⇒ **停止**，
因为 `0007` 只做 `ALTER`，不建表，schema 分叉会让"本地已验证"失效。

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

```bash
cd D:/Guo_Feeder_Project/Cloudflare_Assets/guo-feeder-api
npx wrangler d1 migrations apply guofeeder --remote
```

★ **禁止** `d1 execute --file migrations/0007_*.sql`：那会跳过 `d1_migrations` 登记
（`0004`/`0005` 已踩过，见 `migrations/README.md` §迁移状态说明 `D-1`）。

### 3.3 应用后验证（**立即**执行；四类检查相互独立，缺一不可）

```sql
-- V-1 结构：列 / 索引 / 是否部分索引 / 登记   （期望 1 / 1 / 1 / 1）
SELECT (SELECT COUNT(*) FROM pragma_table_info('mqtt_messages') WHERE name='dedup_key') AS col,
       (SELECT COUNT(*) FROM sqlite_master WHERE type='index' AND name='ux_mm_dedup_key') AS idx,
       (SELECT COUNT(*) FROM sqlite_master WHERE name='ux_mm_dedup_key'
          AND sql LIKE '%WHERE dedup_key IS NOT NULL%') AS partial_idx,
       (SELECT COUNT(*) FROM d1_migrations
          WHERE name='0007_mqtt_messages_dedup.sql') AS reg;

-- V-2 ★ 历史行未被改动（用固定 id 上界重放 A-4；期望与 FINGERPRINT_BEFORE 逐项相等）
SELECT COUNT(*) AS n_old,
       SUM(LENGTH(COALESCE(raw_payload,''))) AS pl_len,
       SUM(LENGTH(COALESCE(msg_id,'')))      AS id_len,
       SUM(LENGTH(COALESCE(topic,'')))       AS tp_len,
       SUM(CASE WHEN dedup_key IS NOT NULL THEN 1 ELSE 0 END) AS keyed
  FROM mqtt_messages
 WHERE id <= :MAX_ID_BEFORE;
-- ★ keyed 必须 = 0（不回填历史键）

-- V-3 索引可用性（必须能走索引，不能"建了但条件不匹配"）
SELECT COUNT(*) AS keyed_total FROM mqtt_messages WHERE dedup_key IS NOT NULL;  -- 期望 0
```

判定：

| 检查 | 期望 | 不符含义 |
|---|---|---|
| `V-1.col/idx/partial_idx/reg` | `1/1/1/1` | 任一为 0 ⇒ 见 §3.4 分类 |
| `V-2.n_old/pl_len/id_len/tp_len` | **等于** `FINGERPRINT_BEFORE` | 不等 ⇒ 历史行被改动 ⇒ **P0，立即停并升级** |
| `V-2.keyed` | `0` | ≠0 ⇒ 有人回填 ⇒ 违反"不回填"边界 |
| `V-3.keyed_total` | `0` | ≠0 ⇒ 同上 |

★ **`COUNT(*)` 全表变化不参与判定** —— 生产每时每刻都在写入（`/api/ingest` 仍用旧代码在跑），
全表行数变化是**正常现象**。Rev.1 的"行数未变"判据会稳定误报，已废弃。

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
| **结构错误** | `apply` 报错（如 `no such table` / `duplicate column`） | 命令非 0 退出 + `V-1` 全 0 | **停止**。`no such table` ⇒ 生产表缺失（严重异常，升级人工）；`duplicate column` ⇒ 0007 曾被部分手工执行（查 `dedup_key` 是否已存在再定） |
| **登记错误** | DDL 生效但 `d1_migrations` 无 0007 | `V-1.col/idx=1` 且 `V-1.reg=0` | **停止**。说明有人绕过登记执行过 SQL ⇒ 需人工决定"补登记"还是"回滚重来"，**不得自动修** |
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

★ 因此 C-1 单独回滚**几乎无收益**（去重本来要靠 C-2 的代码才会发生）：
若 C-1 后决定不上 C-2，回滚与否都不影响生产语义，建议**保留索引待用**，
只把"不部署 C-2"作为结果记录。

### 3.6 C-1 明确不做的事

- ❌ 不回填历史 `dedup_key`
- ❌ 不删除 / 不改写任何历史行
- ❌ 不修改 `0001`–`0006` 任何文件
- ❌ 不手工 `d1 execute` 执行 `0007` 的 SQL
- ❌ 不给 `0007` 补 `CREATE TABLE mqtt_messages`（会与生产实际 schema 分叉）

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
| **P-1' 归属** | ❌ **被动**（用真实 `$tenants` 回流） | 零 | 窗口内 `substr(topic,1,9)='$tenants/'` 的行 `device_id` 为合法 12-hex（旧版是 `http_api`） | 出现 `http_api` ⇒ 回滚 |
| **P-3' 方向** | ❌ **被动**（设备本来就在发 `/log`） | 零 | 窗口内 `direction='log'` 行数 > 0（旧版落 `unknown`） | 全为 `unknown` ⇒ 回滚 |
| **P-2 哨兵** | ✅ 必须合成 | 1 行 `device_id='unresolved'`；**不**建 device 行 | 该行 `device_id='unresolved'`（不是 `BADID`、不是 `http_api`） | 落成 `BADID`/`http_api` ⇒ 回滚 |
| **P-4 去重** | ✅ 必须合成（**同 payload 连发 3 次**） | 1 行；`device.last_seen` 前移 | `COUNT(*) WHERE msg_id='<P4 标记>'` **= 1** | 3 行 ⇒ 回滚（去重未生效） |
| **P-5 无 `msg_id`** | ✅ 必须合成（**同 payload 连发 2 次**） | 2 行；`last_seen` 前移 | 2 行且两行 `dedup_key` 均 **NULL** | 1 行 ⇒ **严重**（payload 又被当身份）⇒ 立即回滚 |

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
-- Q-5 audit / echo 都还在（B-2 的核心不变量）
SELECT CASE WHEN substr(topic,1,9)='$tenants/' THEN 'echo' ELSE 'audit' END AS src,
       COUNT(*) AS n
  FROM mqtt_messages
 WHERE created_at >= :ts0_ms
   AND COALESCE(raw_payload,'') NOT LIKE '%G4PROBE-%'
 GROUP BY src;
-- 期望：两者都 > 0。echo 实测晚 33–37 s ⇒ 判"归零"的窗口必须 ≥ 10 min
```

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
| **I-8** | audit > 0 **且** echo > 0 | `source` 进键 ⇒ 两类不可合并 |
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
  4. audit 或 echo 任一归零（窗口 ≥ 10 min 后）
  5. `dedup_key` 唯一约束抛错
  6. 设备 `give_up` / `drop_unacked` 增长
  7. `boot_seq` 在窗口内变化（⇒ 计数器归零，窗口作废，按需重启窗口）

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
| 变更范围 | `ALTER TABLE mqtt_messages ADD COLUMN dedup_key TEXT` + `CREATE UNIQUE INDEX ux_mm_dedup_key … WHERE dedup_key IS NOT NULL` |
| 执行前检查 | §2.1–§2.6 全绿；`A-1` 结构与 fixture 逐列一致；`0007` 未登记 |
| 成功判据 | `V-1` = `1/1/1/1`；`V-2` 指纹与 `FINGERPRINT_BEFORE` 逐项相等且 `keyed=0`；`V-3` = 0 |
| 失败判据 | `apply` 非 0 退出；或 `V-1` 任一为 0；或 `V-2` 指纹变化 |
| 回滚 | `DROP INDEX IF EXISTS ux_mm_dedup_key;`（列保留）。★ 不需要、也不得手工补 SQL |
| 所需授权 | 人工**书面**批准 C-1 |

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
| 执行前检查 | 窗口负责人已定；`boot_seq` 基线已采；采样时间表已定 |
| 成功判据 | 4 次采样 `I-1…I-9` 全成立；近似指标（A-1…A-5）有解释；不可测项如实标 `NOT MEASURABLE` |
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
