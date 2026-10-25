# Guo Feeder 优化 Rev.2 · 阶段一数据库路线决策报告

> 日期：2026-10-24 22:20 前后（本地分析与官方文档核查）
> 性质：只读调查 + 路线比较 + 实施计划。**未查生产 D1、未建库、未迁移、未改 binding、未部署、未提交 git。**
> 前置文档：`GuoFeeder-Optimization-Phase1-PreDeploy-Review-2026-10-24.md`、`GuoFeeder-Optimization-Plan-Rev2-2026-10-24.md`

---

## 0. 结论（先看这 5 条）

1. **推荐路线 A：保留现有数据库**，且**不需要升级付费套餐** —— 因为一个关键前提被证伪：
   **现有库只有 16,573 行 / 7.80 MB**，不是"很大"。所谓"7.3M 行"是 **Rows Read（扫描放大）**，不是存储量。
2. **建索引的真实成本 ≈ 1.5 万写 / 1.5 万读**（= 4 张表行数之和），约占免费日写配额 **15%**、日读配额 **0.3%**，耗时秒级。**30 秒单查上限与写配额都不构成障碍。**
3. **路线 B 全部子方案被否决**，除"无收益"外还有一条**硬阻断**：`mqtt_messages` 表**不在任何迁移文件中创建**（属迁移体系之外的遗留表），而 `0007` 却对它执行 `ALTER` ⇒ **新建库跑 `migrations apply` 会在 0007 处失败**，整个库不可用。这是一个**独立的灾难恢复缺口**，与选哪条路线无关，必须单独立项修复。
4. **B 的隐性代价被低估**：`mqtt_messages`（7,844 行 / 4.82 MB，占全库 62%）正是 **G4-R1 / G4-R5** 的取证载体；换成空库等于把这些未关闭任务的证据切断，而没有任何成本收益。
5. **顺带修正上一份报告的两处过度估计**：① "建索引 ≈ 211 万写 / 21× 配额"是**保留期反推的上界**，实际按备份只有 1.5 万写；② "+9.5k~11.7k 写/日"把**执行次数当成了成功插入数**（违反指标纪律），实际应 ≤ 当日成功插入数（全表仅 5,414 行 ⇒ 日增量上限 5,414，估 **+1k~5.4k/日，约 +1%~+6%**）。

---

## 1. 基线与核心事实更正

### 1.1 仓库/线上基线

| 项 | 值 |
|---|---|
| 云端仓 | `main` @ `8e8d3e0`（阶段一产物未提交：5 改 + 4 新增） |
| 线上 Worker | `e85c8a2f`（UTC 2026-10-23T22:37 = 北京 10-24 06:37） |
| D1 库 | `guofeeder` / `0d8385ba-a1a6-4fcc-8e43-ae61c014c43d`，绑定名 `guofeeder_DB` |
| 已应用迁移 | `0001`（2026-10-03）… `0007`；`0008` 于 10-23 备份后应用；**`0009` 未应用** |
| Pages | **无 D1 binding**（经 Worker 代理），切换数据库只需改 Worker 一处 |

### 1.2 ★ 真实数据量（本地备份离线统计，不查生产）

来源：`backups/guofeeder-pre0008-20261023.sql`（2026-10-23 23:38 导出的**全库** dump，含 `d1_migrations` 7 条记录 = 0001–0007）。逐行统计 `INSERT INTO` 语句：

| 表 | 行数 | 数据字节 | 单行均值 | 占比 |
|---|---|---|---|---|
| **mqtt_messages** | **7,844** | 4.82 MB | 644 B | **62%** |
| **log_history** | **5,414** | 2.04 MB | 394 B | 26% |
| log_conflict | 1,669 | 0.60 MB | 376 B | 8% |
| telemetry_history | 1,217 | 0.23 MB | 203 B | 3% |
| device_event | 385 | 0.11 MB | 289 B | 1% |
| device_credential | 16 | 5.5 KB | — | — |
| device | 10 | 2.4 KB | — | — |
| device_binding | 1 | 0.2 KB | — | — |
| log_boot_watermark | 1 | 0.5 KB | — | — |
| log_boot_watermark_audit | 1 | 0.5 KB | — | — |
| **合计** | **16,573** | **7.80 MB** | — | 100% |

**这张表推翻了本次路线讨论的前提。**

### 1.3 为什么会有"数据很大"的错觉 —— 必须区分的三个量

| 量 | 10-23 数值 | 含义 |
|---|---|---|
| **Rows Read** | 7,299,585 | 查询**扫描**的行数（缺索引 ⇒ 每条列表查询把整个设备分区扫一遍再排序） |
| **Rows Written** | 88,457 | 写入行数（含索引维护 + **重复尝试**） |
| **实际存储** | **16,573 行 / 7.80 MB** | 真实记录数 |

⇒ **7.3M 与 1.66 万差了 440 倍**，全部来自"扫描放大"，与存储量无关。这是本项目配额事件的**根本性质**：问题不在"数据多装不下"，而在"每次查询都要把少量数据反复扫几百遍"。

### 1.4 第四个反直觉事实：77% 的日志写入是重复尝试

- `log_history` INSERT **执行 11,725 次/日**，但全表**只有 5,414 行**（累计）
- ⇒ 当日绝大多数执行是 `INSERT OR IGNORE` **撞 `ux_lh_dedup` 被丢弃**的重复帧
- 成因（已记录在 MEMORY）：设备重发 + EMQX `$tenants/` 与 plain topic **双副本投递**（设计行为）
- **意义**：这不仅解释写量，也说明**修索引不会显著增加写**（撞键的插入不产生索引项）

---

## 2. Cloudflare 官方规则（采集于 2026-10-24）

来源：<https://developers.cloudflare.com/d1/platform/limits/>（Last updated Apr 21, 2026）、<https://developers.cloudflare.com/workers/platform/pricing/>

### 2.1 硬上限（**不分套餐，付费也提不上去**）

| 项 | 值 | 对本项目的影响 |
|---|---|---|
| **单条 SQL 查询时长** | **30 秒** | 建索引/大 DELETE 的**真正**限制；与付费无关 |
| 单条 SQL 语句长度 | 100 KB | 无影响 |
| 单查询绑定参数 | 100 | `buildSelectSql(n)` 按设备数拼，当前 1 设备，无影响 |
| 单表列数 | 100 | 无影响 |
| 单行/字符串/BLOB | 2 MB | 无影响（`raw_payload` 必须 < 2 MB，已有截断策略） |
| 每次 Worker 调用可发查询数 | **Free 50 / Paid 1000** | 现有 snapshot 5 条 + 会话 2 条，安全；批量任务需注意 |
| 每库并发连接 | 6 | 无影响 |

> 官方原文（关键）："Data migrations like a large `UPDATE` or `DELETE` affecting millions of rows **must be run in batches**. A single query that attempts to modify hundreds of thousands of rows or hundreds of MBs of data at once **will exceed execution limits**."

### 2.2 套餐额度

| 项 | Workers Free | Workers Paid（$5/月，按账户） |
|---|---|---|
| Rows Read | **5,000,000/日** | 首 250 亿/月，+$0.001/百万 |
| Rows Written | **100,000/日** | 首 5,000 万/月，+$1.00/百万 |
| **最大单库容量** | **500 MB** | **10 GB**（官方明示不可再提） |
| 账户存储总量 | 5 GB | 1 TB（首 5 GB 含） |
| 数据库个数 | **10** | 50,000 |
| Time Travel（时间点恢复） | **7 天** | 30 天 |
| 重置 | **UTC 00:00 每日** | 按订阅续费日 |

### 2.3 降级（Paid → Free）的官方说法

官方定价页**只**给了一条降级前置条件，且只针对 Durable Objects 的 KV 后端：

> "If you wish to downgrade from a Workers Paid plan to a Workers Free plan, you must first ensure that you have deleted all Durable Object namespaces with the key-value storage backend."

**未找到**任何关于"降级时数据超免费额度如何处置"的明文规定。

⇒ **判定：降级可行性未验证**（属"未找到支持证据"）。这也削弱了"临时升级再降回"这一战术的确定性——**路线 A 不需要它，正因如此更优**。

---

## 3. 路线 A：保留现有数据库（推荐）

### 3.1 逐条回答要求的 8 个问题

**A-1｜现有套餐额度是否够建索引？**
够，且**不需要付费**。按备份实测行数：4 张目标表合计 **14,860 行**。官方："DDL 可能计入读/写"。按最坏情形（每行 1 读 + 1 写 + 索引项写）估算：**≈1.5 万读 / 1.5 万写**。

- 占免费日读：1.5 万 ÷ 500 万 = **0.3%**
- 占免费日写：1.5 万 ÷ 10 万 = **15%**（单条索引最大者 `mqtt_messages` 7,844 行 ⇒ ≈7.8k 写 = 7.8%）
- 单查时长：7,844 行建 B 树索引在毫秒级，**远离 30 秒上限**

**A-2｜临时升级的费用与降级条件？**
$5/月（账户级，含 Workers/KV/DO/D1 的月额度）。降级规则仅有 DO 前置条件，**超额度数据的处置未明文** ⇒ 若走"升级再降级"，存在**不确定性风险**。**本路线不需要升级，故不涉及。**

**A-3｜付费是否真能解决 DDL 限制？**
**不能**。付费只抬"日/月配额"，**30 秒单查上限是平台硬限制**。反过来说：本项目数据量（1.5 万行）**本来就不触碰 30 秒限制**，所以"付不付费"与"能不能建索引"在本项目**解耦**。

**A-4｜四条索引是否仍必要？能否分批？**
必要（各自对应一条占日读 45.7%/16.9%/4.8%/0.9% 的查询）。**建议分两批**，把失败影响面压到最小：

| 批次 | 索引 | 依据 |
|---|---|---|
| 批 1 | `ix_lh_device_id`（log_history） + `ix_th_device_id`（telemetry） | 覆盖日读 50.5% |
| 批 2 | `ix_mm_device_msg_id`（mqtt_messages） + `ix_ev_device_id`（device_event） | 覆盖日读 17.8% |

批间留 ≥1 次观测，确认写量无异常再继续。

**A-5｜失败/部分成功/超时如何恢复？**
`CREATE INDEX IF NOT EXISTS` **幂等** ⇒ 直接重跑。失败时不会留下半成品索引（SQLite 建索引是单事务，失败即回滚该条）。若批 1 成功批 2 失败 ⇒ 系统仍可用（只是少两条优化），可择日重试。**不需要回滚数据**（DDL 不改数据）。彻底回退：`DROP INDEX`（4 条，见 0009 文件内注释）。

**A-6｜建索引后如何验证？**
① `EXPLAIN QUERY PLAN` 看是否 `SEARCH ... USING INDEX`（生产实测该语句 **0 行读**，见 §6 预算）；② GraphQL `d1QueriesAdaptiveGroups` 看 Q1/Q3/Q4/Q10 的 `rowsRead/exec` 是否从 2 万级降到几十；③ UI 手测日志翻页/帧查询/曲线；④ `rowsWritten` 归一化对比确认无异常放大。

**A-7｜保留全部旧数据的长期成本？**
按实测单行体积推算：

| 表 | 增长速率 | 一年新增 | 备注 |
|---|---|---|---|
| `mqtt_messages` | 1,157 行/日 × 644 B ≈ **0.71 MB/日** | **≈259 MB/年** | ⚠️ **无保留策略**（`RETENTION_TABLES` 只含 telemetry/log）⇒ 约 **1.9 年**触 500 MB 单库上限 |
| `log_history` | 唯一行约 1k~2.7k/日 × 394 B ≈ 0.4~1.1 MB/日 | 受 180 天保留约束，稳态 **≈70~190 MB** | 有保留策略 |
| `telemetry_history` | 约 600 行/日 × 203 B | 受 90 天约束，稳态 **≈11 MB** | 有保留策略 |

⇒ **存储不是近期问题**，但 `mqtt_messages` 缺保留策略是**可预见的 1.9 年后故障**，应单独立项（见 §7-④）。

**A-8｜升级后能否可靠降回？**
官方未明示 ⇒ **不依赖**。本路线全程 Free，无此风险。

### 3.2 路线 A 的风险清单

| 风险 | 概率 | 影响 | 缓解 |
|---|---|---|---|
| 当日写配额被 DDL 吃掉 15% 导致设备日志写入受阻 | 低 | 中 | 选在低活跃时段执行；执行前看当日 `rowsWritten` 余量（GraphQL，免费）；分两批 |
| 索引未被采用（计划未变） | 低 | 低 | `EXPLAIN` 验证；未采用则 STOP，不改范围 |
| 写量意外放大 | 低 | 中 | 归一化对比；>2× 估算则 STOP + `DROP INDEX` |
| `d1_migrations` 顺序/版本冲突 | 极低 | 中 | 单独 `--command` 执行 DDL，不用 `migrations apply`（保持 0009 为草案，见 §7-③） |

---

## 4. 路线 B：新建数据库（不推荐）

### 4.1 ★ 先讲三条否决理由（不止"没收益"）

**(1) 硬阻断：新库无法从迁移体系建起**

`mqtt_messages` 是**迁移体系之外的遗留表**——`migrations/README.md` 明写"完全不动"，`0001` 不创建它，而 `0007_mqtt_messages_dedup.sql` 第 43 行是：

```sql
ALTER TABLE mqtt_messages ADD COLUMN dedup_key TEXT;
```

在一个**全新空库**上按顺序应用 0001→0009：`0007` 会因 `no such table: mqtt_messages` **直接失败**，迁移链断在 0007，其后 0008/0009 不会应用 ⇒ **新库缺少 `ui_session` / `command_audit` / `safety_event`**，Worker 全面不可用。

可行的补救是"先手工 bootstrap 这张表再 apply 迁移"，其 DDL 只能从两个地方取得：
- `backups/guofeeder-pre0008-20261023.sql`（生产全量 dump，**权威**，含 `dedup_key`）
- `guo-feeder-api/test/real_d1/fixtures/mqtt_messages.sql`（**不含** `dedup_key`，是 0007 之前的形态）

⇒ B 路线**必须多一步迁移体系外的 bootstrap**，这是**新增的、非幂等的、必须人工核对的**操作环节。

**(2) 证据切断：`mqtt_messages` 是未关闭任务的取证载体**

- `G4-R1`：`mqtt_messages` 在高并发窗口漏记上行帧（`OPEN`）
- `G4-R5`：再次漏记且出现重复行（`未修`）

这两项的**全部证据就在这 7,844 行里**（占全库 62%）。新库若不迁移它 ⇒ 证据只能从归档库取；若迁移它 ⇒ 就回到了"迁移全部数据"（B3）。**两种情形都没有带来任何好处。**

**(3) 收益为零：省不下任何资源**

阶段一的核心收益（读 −94%）**完全来自索引 + 停止 COUNT 调用**，与"库是新的还是旧的"无关。B 路线唯一可能的收益是"DDL 更快"，而 A 的 DDL 已经是秒级。

### 4.2 三个子方案逐条评估

#### B1｜新库，不迁移历史（旧库归档）

| 问题 | 结论 |
|---|---|
| 复用 0001–0008 建结构？ | ❌ **不能直接复用**，缺 `mqtt_messages`，0007 失败（见 4.1-1） |
| 0009 何时建？ | 建表后、导入前（空表 ⇒ 瞬时） |
| 保留去重/水位/审计结构？ | **必须保留**。去掉 `ux_lh_dedup` 会让重复帧全部入库（写量翻倍、数据膨胀）；去掉 `log_boot_watermark` 只是丢证据；`log_conflict` 是撞键取证 |
| `device_id`/设备注册/会话/审计？ | 设备表空 ⇒ 首帧自动 `INSERT ... 'CLAIM_PENDING'`（`device-registry.js:341`）**自愈**；但 `device_credential` 空 ⇒ 凭据台账断代（不影响 MQTT 认证，ACL 在 EMQX，`buildAclRules(deviceId)` 不读表）；`ui_session` 空 ⇒ **所有人被登出**；`command_audit` 空 ⇒ UI 命令历史清空 |
| 切换期日志不丢/不重？ | **关键结论：不会批量重传**。ACK 由 `buildLogAckPayload(bootSeq, seq)` 从**收到的帧**回显，`acked_seq` 存在**设备侧**（`log-ack.js:17` 注释："设备收到 ACK 会推进 `acked_seq` 并据此删除 Flash 段"）。已是 acked 的旧日志**不会**被重新上传 ⇒ 空库不会引发补传风暴（这条与"直觉"相反，但是代码确证） |
| 新旧同时写入 / 状态分叉？ | 靠**单 binding 切换**天然避免；切换窗口内**不得**有在途命令 |
| 如何切 binding / 验证 / 回退？ | 改 `wrangler.toml` 一处 `database_id` → deploy；回退=改回 + deploy（旧库未被写，7 天 Time Travel 兜底） |
| 是否需要停机/冻结/双读/双写？ | 需要 ~1~3 分钟**命令静默窗口**；**不需要双写**（双写必然分叉，明确否决） |
| 旧库保留多久？ | 建议 90 天只读归档，再导出归档到本地/R2 后删除 |
| 长期是否再遇同样问题？ | 会：`mqtt_messages` 无保留策略 ⇒ 1.9 年后触 500 MB |

**结论：技术上可行（补 bootstrap 后），但纯粹是用"证据丢失 + 额外 bootstrap 风险"换"零收益"。**

#### B2｜新库，只迁必要数据

迁移集（估算 ≤2 千行，成本可忽略）：

| 表 | 迁？ | 理由 |
|---|---|---|
| `device` / `device_credential` / `device_binding` | ✅ 全量 | 身份与凭据台账，重建需重新配网/签发 |
| `ui_session` | ✅ 全量（≤20 行） | 避免全员登出 |
| `safety_event` | ✅ 全量 | 安全审计，**不可丢** |
| `command_audit` | ✅ 最近 7 天 | UI 命令历史连续性 |
| `telemetry_history` | ✅ 最近 7 天 | 曲线有数据可看 |
| `log_history` | ❌ 0 行 | 量大且价值低（多为重复/错误告警） |
| `mqtt_messages` | ❌ 0 行 | 量最大（62%）…但它正是 G4 证据 ⇒ **只能留在归档库** |
| `log_conflict` / `log_boot_watermark*` | ❌ 0 行 | 新库重新初始化即可（首帧 init，不会误报 rewind） |

**结论：B2 是 B 系列里唯一"看起来合理"的，但仍需 bootstrap + 切 binding + 证据分裂，且收益依旧为零。相比 A，它只是把"保留数据"换成"少占 7.8 MB"。**

#### B3｜新库，迁移全部历史

| 项 | 估算 |
|---|---|
| 读（旧库导出） | 16,573 行 ⇒ 免费日读 0.3% |
| 写（新库导入，含索引维护） | 16,573 × ≈4 ≈ **6.6 万行写** ⇒ 免费日写 **66%**（可一天做完，但极吃紧） |
| 索引重建 | 建在空表上 ⇒ 瞬时；或导入后建 ⇒ 1.5 万写 |
| 停机 | 需较长静默窗口（导入 7.8 MB SQL 约数分钟） |
| 迁移成本 vs 收益 | **成本 > 收益**（A 的 DDL 只花 1.5 万写，B3 花 6.6 万写 + 停机 + 双库核对） |

**结论：B3 在所有维度上都劣于 A —— 同样的"保留全部数据"，A 只需 1.5 万写，B3 需 6.6 万写且要停机。**

### 4.3 B 路线共同的技术前提（若将来仍要走 B）

1. **必须**先 bootstrap `mqtt_messages`（建议固化脚本，见 §7-②）
2. 必须验证 `migrations apply` 在空库上跑通 0001→0009（本地 `--local` 即可演练，**不耗生产配额**）
3. 必须确认 `device_credential` 迁入后 `getDevice()` 字段白名单仍匹配（`account_deleted_at` 等 0002 新增列）
4. 必须保留旧库 ≥90 天，且 `Time Travel` 窗口内（Free 7 天）不删

---

## 5. 统一对比表

价格/限额行标注 **[官方]**，占比行标注 **[实测/估算]**。

| 维度 | **A 保留现有库（推荐）** | B1 新库无历史 | B2 新库迁必要 | B3 新库全迁 |
|---|---|---|---|---|
| 一次性费用 | **$0** [官方：免费用量内] | $0 | $0 | $0（若一天内做完） |
| 是否需升级 Paid | **不需要** | 不需要 | 不需要 | 不需要（但逼近上限） |
| D1 Rows Read | **≈1.5 万**（DDL） | ≈1 千（DDL 空表） | ≈5 千 | ≈1.7 万 + 1.5 万 |
| D1 Rows Written | **≈1.5 万（日配额 15%）** | ≈2 千 | ≈1 万 | **≈6.6 万（66%）** |
| 执行耗时 | **秒级 ×4** | ~30 分钟 | ~1 小时 | 数小时（含取数/导入/核对） |
| 存储变化 | +≈0.3 MB（索引） | −7.8 MB（旧库仍占） | −5 MB 量级 | ≈0 |
| 数据损失 | **无** | log/telemetry/mqtt/audit 全丢 | 丢 log/mqtt（含 G4 证据） | 无 |
| 生产切换风险 | **低**（只加索引，可 `DROP`） | 中高（bootstrap + binding） | 中高 | 高（停机 + 双库核对） |
| 日志链路兼容 | **完全不变** | 需 bootstrap 才能跑 | 同 B1 | 同 B1 + 导入期风险 |
| 回退难度 | **极低**（4 条 DROP INDEX） | 中（改回 binding + deploy） | 中 | 高 |
| 长期维护成本 | 低（需补 mqtt 保留策略） | 低 | 低 | 低 |
| 对既有任务/证据 | **零影响** | ⚠️ G4-R1/R5 证据分裂 | ⚠️ 同 | 无影响 |
| 30 秒单查上限 [官方] | 远离 | 远离 | 远离 | 导入需分批（`d1 execute --file` 逐语句） |

---

## 6. 推荐：路线 A（保留现有库，全程免费）

### 6.1 选择理由（按权重）

1. **前提被证伪**：真实数据 7.8 MB / 16,573 行 ⇒ "大表 DDL 风险"不存在（1.5 万写 = 日配额 15%）。
2. **收益与库的年龄无关**：读 −94% 来自索引 + 去 COUNT，不是来自"换库"。
3. **B 有硬阻断**：新库缺 `mqtt_messages` ⇒ `0007` 失败，需额外 bootstrap，多一处人工环节。
4. **B 破坏未关闭任务的证据**：`mqtt_messages` 是 G4-R1/R5 的唯一取证载体。
5. **A 的回退成本近乎零**：索引可 DROP，不改数据、不改 binding、不碰 EMQX。
6. **不依赖"降级可行性"这个未验证项**：全程 Free。

### 6.2 实施计划（分 6 步，每步独立可停）

> 前置：**需用户授权**（见 §8）。全部命令均在本地执行，除标注「生产」的步骤外不触线上。

**Step 0 — 预检（0 成本，本地+免费 API）**
1. `git diff` 复核阶段一代码；两套测试全绿（除 21 项既有失败）
2. GraphQL 取**当日** UTC `rowsWritten`，确认剩余配额 > 5 万（给 DDL 留余量）
3. 记录基线：当前 24h 的 `rowsRead`、`rowsWritten`、Q1/Q3/Q4/Q10 的 `rd/exec`
4. 确认 `migrations` 目录版本已到 0008 已应用状态（读 `d1_migrations`，1 条查询，≈7 行读）

**Step 1 — 部署代码（不含任何 DDL）**〔生产〕
- Worker `deploy`（`read-api.js` 的 `counts` opt-in）→ Pages `pages deploy`（首页缓存/去 COUNT）
- **预期读降 ~22%**（snapshot COUNT 19.8% + metrics 2.2%），写不变
- 验证：UI 首页/设备页正常、`counts_included=false`、GraphQL 看 COUNT 类 exec 归零
- 回退：`wrangler rollback` + Pages 回退上一部署

**Step 2 — 索引批 1**〔生产 DDL，需单独授权〕
```bash
wrangler d1 execute guofeeder --remote --command \
  "CREATE INDEX IF NOT EXISTS ix_lh_device_id ON log_history(device_id, id);"
wrangler d1 execute guofeeder --remote --command \
  "CREATE INDEX IF NOT EXISTS ix_th_device_id ON telemetry_history(device_id, id);"
```
- 预期 **≈6.6k 写**（5,414 + 1,217），占日配额 6.6%
- 立即验证：`EXPLAIN QUERY PLAN` 两条（≈0 行读）

**Step 3 — 观测 24h**
- 判定：Q1 `rd/exec` 从 21,827 → <100；Q4 从 3,141 → <100；`rowsWritten` 归一化增幅 ≤ +6%
- **STOP**：若计划未变 / 写量 >2× 估算 / 出现 any D1 error ⇒ 停，回报

**Step 4 — 索引批 2**〔生产 DDL，需单独授权〕
```bash
wrangler d1 execute guofeeder --remote --command \
  "CREATE INDEX IF NOT EXISTS ix_mm_device_msg_id ON mqtt_messages(device_id, msg_id, id);"
wrangler d1 execute guofeeder --remote --command \
  "CREATE INDEX IF NOT EXISTS ix_ev_device_id ON device_event(device_id, id);"
```
- 预期 ≈8.2k 写（7,844 + 385），占 8.2%
- 若批 1 已让当日写量偏高，**顺延到次日 UTC 00:00 之后**

**Step 5 — 终验 + 归档**
- 三张表 UI 实测（日志翻页/帧查询/曲线/审计）+ `EXPLAIN` 四条 + GraphQL 24h 对比
- 输出阶段一验收报告（读降幅、写增幅、计划证据、回退演练记录）

### 6.3 请求预算（严格遵守）

| 步骤 | D1 读 | D1 写 | 备注 |
|---|---|---|---|
| Step 0 | ≤10 行 | 0 | `d1_migrations` 1 条 + GraphQL 0 |
| Step 1 | 0 | 0 | 仅部署 |
| Step 2 DDL | ≈6.6k | ≈6.6k | 一次性 |
| Step 2 验证 | ≈0 | 0 | `EXPLAIN`（实测 0 行读） |
| Step 3 | 0 | 0 | GraphQL 不计 D1 |
| Step 4 DDL | ≈8.2k | ≈8.2k | 一次性 |
| **合计** | **≈1.5 万**（日配额 0.3%） | **≈1.5 万**（日配额 15%） | |

---

## 7. 与阶段一其他工作的关系（四件事分开管理）

| # | 工作 | 能否独立推进 | 依赖 | 回滚 |
|---|---|---|---|---|
| ① | Worker `read-api.js` `counts` opt-in | ✅ **可独立**，立即 | 无 | `wrangler rollback` |
| ② | Pages 首页缓存/去 COUNT/手动刷新 | ✅ **可独立**，立即 | 无 | Pages 回退部署 |
| ③ | Migration 0009 四条索引 | ⏸ **建议改为 `--command` 手工 DDL**（不写入 `d1_migrations`，保持草案状态，随时 DROP） | ① 完成后（先拿到 22% 收益，再看索引增量） | 4 条 `DROP INDEX` |
| ④ | `log_boot_watermark` 写降频 | ⏸ 需单独授权（改代码） | 无（与库路线无关） | 代码回退 + 开关 `LOG_BOOT_WATERMARK=0` |
| ⑤ | 21 项既有测试失败 | ✅ 可独立立项 | 无 | 仅测试文件 |

**关于 ④ 的新增发现（本轮顺带）**：按备份实测，`log_history` 全表仅 5,414 行，而日 INSERT 尝试 11,725 次 ⇒ **约 77% 是重复撞键**。这意味着**写量的最大杠杆不是 watermark（32,142 行/日），而是"同一帧被投递两次"**（`$tenants/` + plain 双副本）。两者都要做，但优先级应重新排序：**先在阶段三量化双副本投递占比**，再决定 watermark 值不值得改代码。

**关于 ③ 的 0009 文件定位**：既然改为手工 DDL，`0009_ui_read_indexes.sql` 应**保留为"参考/回滚脚本"**而非待应用迁移（否则未来任何 `migrations apply` 都会把索引带进来，且与手工执行的版本号混淆）。建议改名/移入 `scripts/ddl/`（需授权，本轮未动）。

---

## 8. 需要另行批准的操作清单

| # | 操作 | 风险级别 | 成本 | 回退 |
|---|---|---|---|---|
| P1 | Worker 部署（`counts` opt-in） | 低 | 0 | `wrangler rollback` |
| P2 | Pages 部署（首页/去 COUNT） | 低 | 0 | 回退上一部署 |
| P3 | DDL 批 1（2 条 CREATE INDEX） | 中 | ≈6.6k 写 | 2 条 DROP INDEX |
| P4 | DDL 批 2（2 条 CREATE INDEX） | 中 | ≈8.2k 写 | 2 条 DROP INDEX |
| P5 | （可选）`0009` 文件改名为参考脚本 | 极低 | 0 | 改名回退 |
| P6 | （可选）`mqtt_messages` 保留策略（新迁移 0010） | 中 | 0 | 改回 env / 不应用 |
| P7 | （可选）bootstrap SQL 固化（灾难恢复缺口） | 低 | 0 | 删除文件 |

**明确不申请**：新建/切换数据库、删除或截断数据、修改 binding、改 EMQX/ACL、改 watermark 生产开关、烧录固件。

---

## 9. 旧任务与证据保护

- **未关闭任何任务**；未因"换库"声称任何故障已解决（本路线也不换库）。
- G4-R1（`OPEN`）、G4-R5（`未修`）、G4-R2（`OPEN`）、G4-M1（部分关闭）、B-1（`pending`）、S-3-O2（已裁决·非缺陷）、WP-5（待排期）、G-4（**PASS**）—— 状态全部不动。
- **证据保护**：G4-R1/R5 的证据载体是 `mqtt_messages`（7,844 行）。路线 A 保留它 ⇒ **无需额外归档动作即可继续取证**。这是 A 相对 B 的实质优势，写入本报告作为必要条件。
- ⚠️ 仍存在的文档冲突（上轮已登记）：`docs/issues/未修复的问题.md` 第 390/1211/1243 行写 `G-4=BLOCKED`，实际 `PASS`（`G4-Window-Verdict-2026-10-21.md:63`）。本轮未改权威文档。

---

## 10. 引用、假设与未解决问题

### 10.1 引用（采集日期 2026-10-24）

| 内容 | 来源 |
|---|---|
| D1 限额（30 s / 500 MB·10 GB / 50·1000 查询 / 10·50,000 库 / Time Travel 7·30 天） | <https://developers.cloudflare.com/d1/platform/limits/>（Last updated Apr 21, 2026） |
| Workers Paid $5/月、D1 额度、索引 +1 行写、UTC 00:00 重置 | <https://developers.cloudflare.com/workers/platform/pricing/> · <https://developers.cloudflare.com/d1/platform/pricing/> |
| 降级仅 DO 前置条件、超额度处置未明示 | 同上（pricing 页，唯一提及 downgrade 的段落） |
| 表行数/体积 16,573 行 / 7.80 MB | **本地** `backups/guofeeder-pre0008-20261023.sql` 离线统计 |
| 逐 SQL 读写（7,299,585 / 88,457 / 60,020） | **官方** GraphQL `d1AnalyticsAdaptiveGroups` / `d1QueriesAdaptiveGroups`（10-23 UTC） |
| `mqtt_messages` 无迁移 | **代码**：`migrations/README.md` §0 + `0007:43` 的 `ALTER` + 全仓 grep 无 `CREATE TABLE mqtt_messages`（除备份与测试夹具） |
| ACK 不依赖库、设备自注册 | **代码**：`log-ack.js:17/202`、`device-registry.js:341` |
| Pages 无 D1 binding | **代码**：`guo-feeder-pagesdev/` 无 `wrangler.toml`，session-store 走 Worker 代理 |

### 10.2 假设（明确标注为估算）

- 建索引成本 ≈ 表行数（读）+ 表行数（写）：官方只说"DDL 可能计入读写"，**未给逐行公式** ⇒ 1.5 万是**量级估算**，验收以 GraphQL 实测为准。
- 备份文件与"当前"库的差异：备份为 10-23 23:38，之后有新增写入（10-24 活动）⇒ 当前行数可能略高，但仍在**同一量级**（万级），不影响结论（配额余量 85%）。
- 单行体积按 dump 字节/行数折算，未扣除 SQL 语法开销（实际更小）。
- `mqtt_messages` 1,157 行/日 来自 10-23 单日统计，未必是长期稳定速率。

### 10.3 未解决/待核实

1. **降级规则**：Paid→Free 时超额度数据如何处置 —— 官方未明示（本路线不需要）。
2. **`mqtt_messages` 生产索引**：备份只含表定义，未核当前是否有额外索引（`PRAGMA index_list`，≈1 行读，需授权）。
3. **双副本投递的真实占比**：77% 重复是"重发 + `$tenants` 双投"的合计，两者各占多少未分离；影响阶段三优先级。
4. **`0009` 手工 DDL 与 `d1_migrations` 的一致性**：手工建的索引不记版本，未来 `migrations apply` 会重复执行 `IF NOT EXISTS`（无害），但需在文档固化，避免"以为没建过"。

---

## 11. 本轮变更与声明

- 新增：本报告 `docs/review/GuoFeeder-Optimization-DB-Route-Decision-2026-10-24.md`
- 修订：`GuoFeeder-Optimization-Phase1-PreDeploy-Review-2026-10-24.md` §4.6（补更正注记：211 万行为上界，实测 1.5 万）
- 未改动：任何生产 D1、Worker/Pages、EMQX、固件、迁移 0001–0008、去重键、ACK/重试/补传/rewind 语义、Git（未提交）

**STOP：等待审阅与授权，不进入实施。**
