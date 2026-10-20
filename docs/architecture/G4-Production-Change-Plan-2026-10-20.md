# G4 生产变更申请（云端 `0007` + Worker 部署 + 生产验证）

> ## ⚠️ 本文件（Rev.1）**已被 Rev.2 取代，不再具备执行效力**
>
> - **执行手册**：`G4-Production-Change-Plan-Rev2-2026-10-20.md`（**只看这一份**）
> - **修订理由**：`G4-Production-Change-Plan-Review-2026-10-20.md`（A–G，逐条列明本文件的 10 处问题）
>
> **本文件已知的 4 处实质错误（保留原文作审计留痕，请勿据此执行）**：
> 1. §4.1/§4.4「回滚用 **deployment id**」❌ —— `wrangler rollback [version-id]` 的位置参数是 **version id**
> 2. §3.3「行数未变 = 成功」❌ —— 生产持续有并发写入，该判据会**稳定误报**
> 3. §5.3「`② cloud_ack_ok ≈ ③ accepted`」❌ —— `cloud_ack_ok` 是**批次**数，`accepted` 是**行**数，量纲不同
> 4. §5.2「`Q-2` 同 `msg_id` 重复率」❌ —— 同 `msg_id` 的 audit/echo **天然两行**，同 `msg_id` 不同 payload 也合法两行

- 日期：`2026-10-20`
- 状态：**已被 Rev.2 取代（SUPERSEDED）—— 原文保留，不再更新**
- 适用仓库：`Cloudflare_Assets/guo-feeder-api`（云端）
- 关联文档：
  - `docs/architecture/G4-R4-R5-Joint-Investigation-Report.md`
  - `docs/architecture/G4-R5-B1-B2-Implementation-and-Local-Acceptance-Report.md`
  - `docs/architecture/G4-R4-Gate-Revision-Draft-2026-10-20.md`
  - `docs/architecture/G4-Log-Credibility-and-Observability-Report-2026-10-20.md`（本轮）
  - `migrations/README.md`（迁移状态唯一准绳）

> ★ 本文件**只是申请**。任务书 §2.2-9 明确：远端 D1 migration、Worker/Pages 生产部署
> 均**不在**本轮自动执行授权内。下文"执行步骤"必须由人工明确授权后才可操作。

---

## 1. 变更范围（三件事，**顺序绑定**）

| # | 变更 | 制品 | 是否可独立回滚 |
|---|---|---|---|
| C-1 | 应用 migration `0007_mqtt_messages_dedup.sql` | `ALTER TABLE mqtt_messages ADD COLUMN dedup_key TEXT` + 部分唯一索引 `ux_mm_dedup_key` | ✅（`DROP INDEX`，列可留） |
| C-2 | 部署 Worker（含 B-1 / B-1b / B-2 / B-2b / B-2c） | `src/device-registry.js`、`src/mqtt-dedup.js`、`src/index.js`、`src/history-ingest.js` | ✅（回退到上一 deployment） |
| C-3 | 生产验证与观察窗口 | 只读查询 + EMQX 控制台只读统计 | 不适用（只读） |

★ **顺序不可颠倒**：

```
C-1（先建列建索引）  →  C-2（再部署 Worker）  →  C-3（观察）
```

理由：`insertMqttMessage()` 的 `INSERT` 显式列出 `dedup_key` 列。
若 Worker 先上线而列不存在 ⇒ 首次写入触发 `no such column` ⇒
内置降级会**自动**退回无键写入并 `console.error`（不会整站 500），
但该实例在本次 isolate 生命周期内**永久失去去重**。

★ **反向顺序（先 C-2 再 C-1）不是禁止，而是降级路径**：可用，但必须接受
"这段时间没有去重"，并且**不得**把它当作正常部署路径。

---

## 2. 变更前置条件（**全部满足才可开工**）

1. [ ] 本地验收全绿（本轮已完成，见 §6 证据）
2. [ ] `git status` 干净、`HEAD == origin/main`（云端仓）
3. [ ] 已确认生产 D1 的**当前** `mqtt_messages` 行数（变更前基线）
4. [ ] 已确认生产 `mqtt_messages` **没有** `dedup_key` 列
       （`SELECT COUNT(*) FROM pragma_table_info('mqtt_messages') WHERE name='dedup_key'` = 0）
5. [ ] 已记录**当前** Worker version id 与 deployment id（回退目标）
6. [ ] 已确认 `migrations` 表里 `0007` 未登记
7. [ ] 已获得人工**书面**授权（本任务的运行环境不构成授权）
8. [ ] 已确定观察窗口负责人（谁看、看多久、什么条件下停）

---

## 3. C-1：migration `0007` 应用

### 3.1 应用前快照（只读，必须存档）

```bash
# ① 表结构
npx wrangler d1 execute guofeeder --remote \
  --command "SELECT sql FROM sqlite_master WHERE name='mqtt_messages'"

# ② 行数 + 键空间分布（作为变更后对比基线）
npx wrangler d1 execute guofeeder --remote \
  --command "SELECT COUNT(*) AS n, COUNT(DISTINCT device_id) AS devs FROM mqtt_messages"

# ③ 确认无 dedup_key 列（必须 = 0）
npx wrangler d1 execute guofeeder --remote \
  --command "SELECT COUNT(*) AS n FROM pragma_table_info('mqtt_messages') WHERE name='dedup_key'"

# ④ 确认 0007 未登记
npx wrangler d1 execute guofeeder --remote \
  --command "SELECT name FROM d1_migrations ORDER BY id DESC LIMIT 5"
```

★ 以上四条输出**必须**贴进执行记录（含时间戳）。缺任一条 ⇒ 不得继续。

### 3.2 应用

```bash
npx wrangler d1 migrations apply guofeeder --remote
```

★ **不许**用 `d1 execute --file` 手工执行 `0007`：那会跳过 `d1_migrations` 登记，
使 `migrations status` 与事实不一致（历史 `0004/0005` 已踩过，见 `D-1`）。

### 3.3 应用后验证（**立即**执行，任一不符即回滚）

| 检查 | 期望 | 命令要点 |
|---|---|---|
| 列存在 | `1` | `pragma_table_info('mqtt_messages') WHERE name='dedup_key'` |
| 索引存在 | `1` | `sqlite_master WHERE name='ux_mm_dedup_key'` |
| 索引为**部分**索引 | `1` | `sqlite_master.sql LIKE '%WHERE dedup_key IS NOT NULL%'` |
| 行数未变 | = 变更前基线 | `SELECT COUNT(*) FROM mqtt_messages` |
| 历史键全 NULL | = 总行数 | `COUNT(*) WHERE dedup_key IS NULL` |
| 0007 已登记 | `1` | `d1_migrations` |

★ **语义边界**：本次**不**回填历史行的 `dedup_key`。
⇒ 历史行（`dedup_key IS NULL`）**永远**不受唯一索引约束，
既不会被去重，也不会被合并。这是设计，不是遗漏。

### 3.4 C-1 失败判据与回滚

**失败判据**（任一成立 ⇒ 立即回滚）

- `ALTER TABLE` 报错或超时（D1 对 `ALTER` 有超时限制）
- 索引创建失败（例如存在历史重复键 —— 但历史键为 NULL，理论上不可能）
- 应用后 `COUNT(*)` 与基线不一致（**说明发生了非预期写入，优先级最高**）

**回滚**

```sql
DROP INDEX IF EXISTS ux_mm_dedup_key;
```

- 效果：立即恢复"无去重"写入语义（与修复前一致）
- `dedup_key` 列**保留**（NULL，无副作用；SQLite 的 `DROP COLUMN` 在 D1 上不保证可用）
- ★ **回滚不能让"已被去重掉的独立业务事件"复活** —— 见 §7

---

## 4. C-2：Worker 部署

### 4.1 部署前

```bash
# 记录当前线上版本（回退目标）
npx wrangler deployments list
npx wrangler versions list
```

★ **必须区分** `version id` 与 `deployment id`（历史踩坑：`826332ed` 是 version，
`c666aeeb` 是 deployment）。回滚用 **deployment id**。

### 4.2 部署

```bash
npx wrangler deploy
```

### 4.3 如何确认生产实例**真的**在用新版本

HTTP 200 / "Deployment successful" **不构成**证据。必须做**行为探针**：

| 探针 | 方法 | 新版本特征 |
|---|---|---|
| P-1 归属 | 构造一条 `$tenants/<t>/guo_feeder/<dev>/down` 的 ingest（或等设备真实下行） | `mqtt_messages.device_id` = `<dev>`（**不是** `http_api`） |
| P-2 哨兵 | 构造一条 `guo_feeder/BADID/up` | `device_id` = `unresolved`（**不是** `BADID`/`http_api`） |
| P-3 方向 | 构造一条 `guo_feeder/<dev>/log` | `direction` = `log`（旧版本为 `unknown`） |
| P-4 去重 | 同一 `msg_id` 连发 3 次 | 日志出现 `[mqtt-dedup] outcome=inserted` 1 次 + `duplicate` 2 次；表内 1 行 |
| P-5 无键 | 同一 payload、`msg_id` 缺失，连发 2 次 | 两次都是 `outcome=no_key`；表内 **2 行**（刻意保留） |

★ P-4/P-5 依赖 `console.log`，需 `wrangler tail` 或 Logs 可用。
若 Logs 403（历史已知限制），P-4/P-5 改为**只读 SQL 验证**：

```sql
-- 同一 msg_id 应只有 1 行
SELECT msg_id, COUNT(*) AS n FROM mqtt_messages
 WHERE msg_id IS NOT NULL AND created_at > :deploy_ts
 GROUP BY msg_id HAVING n > 1 ORDER BY n DESC LIMIT 10;
-- 期望：0 行
```

### 4.4 C-2 失败判据与回滚

**失败判据**（任一成立 ⇒ 回滚）

- ingest 出现 5xx（`DB ERROR` / `persist_failed`）比例 > 1%（5 分钟内）
- `mqtt_messages` 写入速率骤降 > 50%（相对部署前 30 分钟基线）
- `/api/history/ingest` 出现 `invalid_device_id` **持续**上升（可能误伤了正常设备）
- `log_history` 新增行数下降 > 50%（日志入库被阻断）

**回滚**

```bash
npx wrangler rollback <deployment-id>
```

- 语义：回到旧代码 ⇒ **不再去重**（回到"每次投递都落一行"的旧行为）
- ★ 回滚**不**修改 `0007` 已建的列与索引（无害；旧代码不写该列）

---

## 5. C-3：生产验证与观察窗口

### 5.1 必须能测的五个比率

| 指标 | 口径 | 采集方式 |
|---|---|---|
| **归属正确率** | `device_id` 为合法 12-hex 的行 / 总行数 | 只读 SQL（§5.2 Q-1） |
| **重复率** | 同 `msg_id` 出现 >1 次的比例 | Q-2 |
| **漏记率** | 设备发出 − Broker 收到 − 云端入库 | **需 EMQX 侧计数**（§5.4），当前**不可得** |
| **错误率** | ingest 5xx / 总请求 | Cloudflare 分析或 `tail` |
| **入库延迟** | `created_at`(Worker) − 设备 `ts` | Q-3 |

### 5.2 只读 SQL 清单（可直接执行）

```sql
-- Q-1 归属分布（部署后 1 小时看一次）
SELECT CASE WHEN LENGTH(device_id)=12 AND device_id NOT GLOB '*[^0-9a-f]*'
            THEN 'valid' ELSE 'invalid' END AS k,
       device_id, COUNT(*) AS n
  FROM mqtt_messages WHERE created_at > :ts0
 GROUP BY k, device_id ORDER BY n DESC LIMIT 20;

-- Q-2 重复 msg_id
SELECT COUNT(*) AS dup_groups FROM (
  SELECT msg_id FROM mqtt_messages
   WHERE created_at > :ts0 AND msg_id IS NOT NULL AND msg_id <> ''
   GROUP BY msg_id HAVING COUNT(*) > 1);

-- Q-3 入库延迟（秒）
SELECT MIN(created_at/1000 - timestamp) AS min_s,
       MAX(created_at/1000 - timestamp) AS max_s,
       AVG(created_at/1000 - timestamp) AS avg_s
  FROM mqtt_messages
 WHERE created_at > :ts0 AND timestamp IS NOT NULL AND timestamp > 0;

-- Q-4 log_history 入库速率（5 分钟桶）
SELECT (created_at/300) AS bucket, COUNT(*) AS n
  FROM log_history WHERE created_at > :ts0
 GROUP BY bucket ORDER BY bucket DESC LIMIT 12;

-- Q-5 audit / echo 是否都还在（B-2 的核心不变量）
SELECT CASE WHEN topic LIKE '$tenants/%' THEN 'echo' ELSE 'audit' END AS src,
       COUNT(*) AS n
  FROM mqtt_messages WHERE created_at > :ts0
 GROUP BY src;
-- 期望：两者都 > 0（echo 晚 33–37 s，观察窗口需 ≥ 10 min）
```

### 5.3 日志端到端对账（设备 → D1）

**四段数据，缺一不可**：

| 段 | 来源 | 命令/查询 |
|---|---|---|
| ① 设备发出 | `system.stats.counters.cloud_records_sent`（**本轮新修**，原恒 0） | MQTT 下行 `system.stats` |
| ② 设备已确认 | `counters.cloud_ack_ok` | 同上 |
| ③ 云端接收/入库 | `log_history` 行数 + `/api/history/ingest` 响应 `{accepted,duplicate,rejected,filtered}` | Q-4 / Pages 日志 |
| ④ 权威总数 | `SELECT COUNT(*) FROM log_history WHERE device_id=:dev AND boot_seq=:b` | 只读 SQL |

判定式（同一时间窗口内）：

```
① ≈ ③.accepted + ③.duplicate      （丢 1 条就差分 1）
② ≈ ③.accepted                     （ACK 只在该行确实存在时发出）
④ == ③.accepted 的累计（本 boot）  （唯一键 (device, boot_seq, seq) 保证）
```

★ **不得**用"HTTP 200"或"Worker invocation success"替代 ③；
★ **不得**用"MQTT PUBACK"替代 ②（PUBACK 只证明 broker 收到）。

### 5.4 漏记率（B-3）—— 当前**测不了**，只列取证清单

`mqtt_messages` 会漏记（`G4-R5` B-3），根因未定。要分阶段定位，**必须**拿到
EMQX 侧计数（EMQX REST API 的规则/数据集成端点在当前凭据下返回 **403**）：

1. 数据集成 → 各 Action 的 **matched / failed / retried** 计数
2. Action 的**队列长度**与丢弃计数（`queue` / `inflight`）
3. Connector（HTTP → Pages）的**成功率 / 超时**
4. 规则 SQL 的命中数（`guo_feeder/+/up`、`+/log`、`+/down` 分别）
5. broker 侧 `recv_cnt` / `send_cnt`（EMQX `/clients/{id}` 只读可用）

★ 在拿到以上任一项之前，**只允许**写"漏记存在，根因未定（分层候选：规则匹配 / Action 队列 / HTTP 重试 / Worker 侧）"，
**不得**写"丢在 EMQX"或"丢在 Worker"。

### 5.5 观察窗口与停止条件

- **窗口长度**：部署后 **≥ 2 小时**（覆盖至少 2 个 `$tenants` 回环周期 + 1 次设备重连）
- **采样节奏**：部署 +10 min / +30 min / +1 h / +2 h 各执行一次 §5.2
- **停止条件（任一触发 ⇒ 立即回滚并记录）**
  1. 归属正确率 < 99%
  2. ingest 5xx > 1%
  3. `log_history` 增速下降 > 50%
  4. audit 或 echo 任一归零（说明去重吃掉了回环证据）
  5. 出现 `dedup_key` 唯一约束**抛出**（不是 `DO NOTHING`）——说明索引定义与写入不匹配
  6. 设备侧 `counters.give_up` / `drop_unacked` 在窗口内增长

---

## 6. 本地验收证据（申请依据）

| 项目 | 结果 | 证据 |
|---|---|---|
| 云端全量回归 | **24 套件全绿** | `test/run_all.mjs` |
| B-1 单元 | **68/68** | `test/current/g4r5_b1_topic_device_id.test.mjs` |
| B-2 单元 | **49/49** | `test/current/g4r5_b2_dedup_key.test.mjs` |
| B-2 真 D1 引擎 | **19/19** | `test/real_d1/mqtt_dedup_real_d1.sh` |
| 历史入库 真 D1 | **123/123** | `test/real_d1/history_ingest_real_d1.sh` |
| 历史入库 逻辑层 | **35/35** | `test/current/history_ingest.test.mjs` |

---

## 7. 回滚的**真实影响**（必须写进变更记录）

> ★ 删除唯一索引**只能**恢复"后续重复写入能力"。
> 它**不能**让部署期间已被 `DO NOTHING` 丢弃的独立业务事件复活。

具体语义：

| 场景 | 回滚后状态 |
|---|---|
| 真正的重复投递被 `DO NOTHING` 丢弃 | ✅ 无损失（本来就该丢） |
| **误合并**（若发生） | ❌ **永久丢失**，无法从 D1 恢复 |
| 回滚前已写入的行 | 保持不变（不删除、不改写） |
| `dedup_key` 列 | 保留（NULL 或已有值），不影响旧代码 |
| `log_history` / `telemetry_history` | **完全不受影响**（独立契约） |

**本轮已把"误合并"的风险从"可能大量发生"降到"结构性不可能"**：
`msg_id` 缺失时**不产生键**（`B-2b`），因此不存在"凭 payload 合并独立事件"的路径。
生产只读统计已证明这一风险是真实的（见 §8）。

---

## 8. 支撑决策的生产只读证据（变更前采集，`2026-10-20`）

| 查询 | 结果 | 含义 |
|---|---|---|
| `msg_id` 覆盖 | `has_msg_id`: down 2001 / 其余；`no_msg_id`: unknown 876 / up 606 / down 214（共 **1 696**，约 14.9%） | 约 15% 的行**没有**稳定消息 ID |
| payload-only 合并影响 | 1 696 行中有 **1 613** 行落在 **167** 个"payload 完全相同"分组；按 payload 去重会**吞掉 1 446 行** | 等级 B 若启用，删行比例 **85%** |
| 抽样（最大分组） | `{"cmd":"system","id":"online","src":"device"}` **472 次**，跨度 **3 384 464 655 ms ≈ 39 天** | **独立心跳**，不是重复投递 |
| 抽样（次大） | `{"cmd":"execute_action","id":"8010","ob":"COMPUTER_RESET"}` **73 次**，跨度 ≈ 36 天 | **复用常量 id 的独立命令** |
| 抽样（真重复） | `{"c":"log_ack","p":{"b":1,"f":261,"t":261}}` **11 次**，跨度 **2 731 ms** | 这才是"短窗口内重复投递" |

⇒ 结论：**payload 不能作为业务事件身份**。`B-2b` 停用等级 B 是**被数据强制**的决定，
不是保守偏好。代价是"缺 `msg_id` 的上行仍可能留重复行"，已登记为接受项。

---

## 9. 申请结论

| 项 | 结论 |
|---|---|
| 是否具备进入生产变更阶段的条件 | ✅ **具备（限 C-1 + C-2）** |
| 是否建议现在执行 | ⚠️ **建议等设备侧新固件先上板**（`system.memwatch` + `cloud_records_sent`），否则 §5.3 的四段对账缺第①段 |
| `0007` 是否可单独先应用 | ✅ 可以（纯加法，历史行不受影响），但**不会**带来任何行为改善 —— 去重要靠 Worker |
| B-3 是否被本变更修复 | ❌ **没有**。B-3 是漏记，与 B-1/B-2 无关 |
| 需要的授权形式 | 人工**逐项**授权 C-1、C-2、C-3，不接受"打包授权" |
