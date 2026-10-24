# Guo Feeder —— 阶段 2 收尾推送 + 阶段 3「日志与写入可靠性」只读审计报告

- 日期：2026-10-25 00:36–01:10（本地）
- 仓库：`Cloudflare_Assets`（`main`，HEAD `0e8bc7a` = `origin/main`）· 固件仓（`wb`，HEAD `34ffd86` = `origin/main`）
- 阶段 3 状态：**只读审计完成，未改任何生产代码/配置/表结构**；实施待批准

---

## 第一部分：阶段 2 推送结果

### 1.1 推送前核查（不信旧报告，重新抓远端）

| 项 | 实测 |
|---|---|
| 分支 / HEAD | `main` / `0e8bc7a` |
| `origin/main`（fetch 后） | `8e8d3e0` |
| 领先/落后 | **3 / 0**（不是旧报告说的 4 / 0 —— 那次把 `75b22b8` 之外又算进了一个） |
| 快进可行性 | `git merge-base --is-ancestor origin/main HEAD` ⇒ **YES** |
| 工作区 | 仅 `?? scripts/enforce_watch.sh`（未跟踪，未混入） |

待推 3 个提交（32 文件 / +3513 −151）：

```
0e8bc7a perf(ui): 连接状态请求 in-flight 合并
e5bd26f feat(ui): 阶段 2 —— EMQX 权威在线状态 / 快捷操作 / 轮询治理
75b22b8 feat(d1): 新建 guofeeder-v2 + 0000 空库引导迁移
```

推送前专项检查：

| 检查 | 结果 |
|---|---|
| `enforce_watch.sh` 是否在提交里 | **否**（`git log origin/main..HEAD -- scripts/enforce_watch.sh` 无输出） |
| 是否含 `dev.vars` / `secrets` / `.env` | **否** |
| 是否含 `backups/` 或 SQL dump | **仅** `migrations/0000_*.sql` 与 `0009_*.sql`（迁移本体，非备份） |
| 是否强推 / 是否 reset | **均未使用** |

### 1.2 推送结果（GitHub 侧独立核实）

```
8e8d3e0..0e8bc7a  main -> main
```

| 项 | 本地 | 远端（`git ls-remote` + `gh api`） |
|---|---|---|
| HEAD | `0e8bc7a37ebe6f6efe8b5d6fd4c1be0b898ed794` | **同** |
| 领先/落后 | 0 / 0 | 0 / 0 |

固件仓（阶段 2 报告在那边，一并推）：

```
3cdd216..34ffd86  wb -> main        # 2 个提交，只含 2 个 .md
```
`origin/main` = `34ffd86` = 本地，0/0。

> 说明：`origin/wb` 是陈旧跟踪 ref（`8376c6e`），按项目约定**不是同步目标**，未动。

---

## 第二部分：阶段 3 进度总览

| 工作项 | 状态 |
|---|---|
| A-1 完整写入路径追踪 | ✅ 完成（含代码行号） |
| A-2 写入类型分类（启动/每批/冲突/重试/审计） | ✅ 完成 |
| A-3 频率、重复比、D1 用量、失败处理 | ✅ 完成（有生产实测数字） |
| A-4 开关到底控制什么 | ✅ 完成（含一个反直觉结论） |
| A-5 可证明安全的优化机会 | ✅ 提出 1 个（**待批准**），另发现 1 个既有缺陷 |
| B 全链路 ACK/重放审计 | ✅ 完成（云+设备两端代码对照） |
| B 特别核查 ACK topic | ✅ 完成（实现 = `/log/ack`；发现 1 处注释过期） |
| C 方案 + 审批清单 | ✅ 见 §7 |
| 实施 | ⛔ **未开始**（按审批门槛停在此处） |

---

## 第三部分：生产配置与关键路径核验

### 3.1 生产配置（从 Cloudflare 侧读，不看本地 toml）

```
Worker 66d45848-5617-4853-ac6f-c5e3ccda2614  @100%
env.guofeeder_DB        (632b1224-… guofeeder-v2)
env.INGEST_VALIDATION   ("observe")
env.LOG_ACK_DUAL_PUBLISH("0")
env.LOG_BOOT_WATERMARK  ("1")
```
三项基线**与任务书一致**。

### 3.2 日志写入链路（`/api/history/ingest`，`log` 域）

| # | 步骤 | 位置 |
|---|---|---|
| 1 | 解析信封 → 逐条校验/过滤 | `history-ingest.js` 主循环 |
| 2 | **`rejected` / `filtered` 不进 `rows[]`** | `history-ingest.js:280-290` |
| 3 | `recordBootWatermark()`（**在 persist 之前**） | `history-ingest.js:305-318` |
| 4 | `persist()` = `INSERT OR IGNORE`，`written[k] = meta.changes > 0` | `:324` / `:752-759` |
| 5 | `ackPersistedLogs()`（**在 persist 成功之后**） | `:356` |
| 6 | 冲突事件落库（**在 ACK 之后**） | `log-ack.js:820` |

顺序与设计文档一致：**水位 → 持久化 → ACK → 冲突记录**。

### 3.3 `LOG_BOOT_WATERMARK` 到底控制什么（反直觉但重要）

- 它只控制 `recordBootWatermark()` 是否运行；**不控制** ACK、不影响 `log_history` 写入、不影响去重。
- 关闭它 = 失去"设备曾经到过哪个 boot_seq"的记忆，`rewind_count` / `ambiguous_count` / rebase 前置校验全部失效 ⇒ **不建议关**（优化目标是降频，不是关闭）。
- 开启后**每批固定成本** = 1 次 `SELECT` + 1 次 `batch`（每设备 1 条 upsert），**与"水位是否变化"无关**。

---

## 第四部分：已确认问题 / 推测问题（分级）

### 4.1 【已确认·高】水位表在生产上"卡死"，但仍按批全额付费

**证据（旧库线上数据，一次性只读查询）**

```json
{"device_id":"288485896ce4","epoch":2,"max_boot_seq":21,"prev_epoch_max":117,
 "observed_max_boot_seq":117,"ambiguous_count":27310,"rewind_count":42,
 "obs_count":27854,"status":"open"}
```

- `max_boot_seq = 21`（判定水位）而设备早已跑到 `boot_seq ≈ 98`（新库实测）。
- `ambiguous_count = 27,310 / obs_count 27,854 = 98.1%` ⇒ **几乎每条记录都落在歧义带**。
- 成因**有据可查**：`log_boot_watermark_audit` 唯一一行

```
rebase_id=rb-20261019-0001  epoch 1→2  max_boot_seq 117→21
prev_epoch_max_after=117    reason=full_erase_confirmed
evidence="RD-8：2026-10-19 02:31 双擦记录；recovery=PROVISION；设备连续上报 boot_seq=21"
actor=admin#f8122859  result=applied
```

⇒ 这是 `R-11` 歧义带的**预期行为**（防止擦除设备立刻把水位抬回去），不是 bug。
但现实后果是：**在设备重启超过 117 之前，水位永远不会推进**，而每批依然写一次库。

**风险等级：中**（无数据错误；纯粹是"付了钱没有产出"+"诊断信号被淹没"）。

### 4.2 【已确认·低】`max_seq_at_max` 在 0006 之后不再推进（诊断列失真）

- `analyzeBatch()` 返回 `nextSeq = (advBoot > prevMax) ? advSeq : 0`
- epoch upsert 的 `max_seq_at_max` CASE 用的是 `nextSeq` ⇒ **"same" 批传 0** ⇒ `MAX(旧值, 0)` 不变
- legacy SQL 用的是真实批次最大 seq（会推进）⇒ **epoch 化时丢失了这一行为**

**证据**：真实 D1 套件 `W-3c` / `W-5b` 两条断言失败（期望 2、实得 1），
且 `log-boot-watermark.js` 自 `8e8d3e0` 起 `git diff` **零改动** ⇒ 属**历史失败**，
不是本次引入。

**影响**：无（`prev.maxSeq` 被读入但 `analyzeBatch` 从不使用它）。但它意味着
"水位表的诊断列不可信"，所以**不能拿它做容量/健康判断**。

### 4.3 【已确认·设计决策】"键存在即 ACK" 允许碰撞记录被确认

`H-3-7-a`（已批准）的语义是 **"幂等键已存在"**，**不保证本次提交的内容已持久化**：

- 同键不同内容（`collision`）**仍然 ACK**（`log-ack.js:665-668`）⇒
  设备收到 ACK 会推进 `acked_seq` 并可回收该段 Flash ⇒ 设备侧那一份**唯一副本**被删除，
  而云端保留的是**先到的那一份内容**。
- 代码注释给出的理由成立：不 ACK 会导致设备反复重投、`collision` 计数按重投次数放大，
  且设备没有第二条上行通道能"换个方式"送出。

**这不是缺陷，是已批准的取舍**；但它意味着"下行 ACK = 内容已完整落库"**不成立**，
必须在任何"日志完整性"承诺里写清楚。**风险等级：低（有 log_conflict 留证 + 有告警日志）**。

### 4.4 【已确认·文档】`log-ack.js` 头部协议块仍写 `/down`

第 60 行写 `topic = guo_feeder/<device_id>/down`，而实现（第 174 行）是
`guo_feeder/<device_id>/log/ack`，`/down` 只作 `LOG_ACK_DUAL_PUBLISH=1` 的**双发**目标
（生产为 `0`）。**纯注释过期，零行为影响**，但会误导后来人（历史上误判过一次）。

### 4.5 【推测·需设备侧确认】`filtered`（DEBUG）是否会形成空洞

- 固件 `log_events.h:648` 的 static_assert 强制 **DEBUG 既不上行也不落 Flash**
  ⇒ 云端 `filtered` **应当恒为 0**；若非 0，说明协议漂移（可作为一条观测断言）。
- 因此"DEBUG 被过滤后设备永远等不到 ACK ⇒ 空洞 ⇒ 段不可回收"这条**不成立**。
- ⚠️ 结论依赖"固件当前版本与仓库一致"（设备已烧录 `a7d1703` 之后的版本）。
  **未在真机上验证 `filtered == 0`**（需一条真实 WARN 批次 + tail 观测）。

### 4.6 【未验证】`INGEST_VALIDATION="observe"` 的实际影响面

配置为 `observe`（只观测不拦截）。本次**未**逐条核对这些校验在 `observe` 下的
处置差异（是否需要 ACK、是否计入 `rejected`）。属 Work Item B 的**未覆盖分支**，
建议单列一条待办（不在本阶段擅自切换该配置）。

---

## 第五部分：D1 写入频率与可优化空间

### 5.1 实测（旧库 `guofeeder`，日志风暴期，官方逐 SQL 统计）

| SQL | 执行次数 | rows read | rows written |
|---|---|---|---|
| `INSERT INTO log_boot_watermark … ON CONFLICT` | **10,714** | 21,428 | **32,142** |
| `SELECT … FROM log_boot_watermark …`（每批一次） | 11,856 | 10,769 | 0 |
| `INSERT OR IGNORE INTO log_history` | 11,725 | 75,117 | 38,046 |
| `INSERT INTO log_conflict` | 1,227 | 11,157 | 4,680 |
| **当日全部 written** | — | 7,449,500 | **89,485** |

| 日 | 水位 upsert exec | 水位 written | 全天 written | 水位占比 |
|---|---|---|---|---|
| 2026-10-23 | 10,714 | 32,142 | 89,485 | **35.9%** |
| 2026-10-24（至 12:52Z） | 11,234 | 33,702 | 79,268 | **42.5%** |

**批次频率 ≈ 11,000–11,700 次/日**（≈ 8 次/分钟，与"每 5 s 一条 WEIGHT_ERROR 被攒批"吻合）。

### 5.2 这些写入里有多少是"有产出"的

- `advance`（推进判定水位）次数 **≤ `max_boot_seq` = 21 次（累计，非每日）**
- `rewind` 观测 42 次（累计）
- `ambiguous` 27,310 次（累计，占 98.1%）
- ⇒ **累计约 27,854 次观测中，真正改变判定水位的 ≤ 21 次（0.075%）**

### 5.3 列级语义（决定"能不能省"的关键）

| 列 | 用途 | 能否不写 |
|---|---|---|
| `max_boot_seq` / `epoch` / `prev_epoch_max` | **判定输入**（歧义带/回退判定） | ❌ 变化时必须写 |
| `observed_max_boot_seq` | rebase 上界校验（`confirm <= observed`） | ❌ 前沿推进时须写 |
| `rewind_count` / `last_rewind_*` | 回退证据 | ❌ 有 rewind 时须写 |
| `last_seen_at` | **rebase 前置条件④**（`REBASE_STALE_SEC = 30 天`） | ⚠️ 不能长期不写 |
| `obs_count` / `ambiguous_count` | 纯观测计数（判定不读） | ✅ 可采样 |
| `max_seq_at_max` | 纯诊断（且已确认失真，§4.2） | ✅ 可不写 |
| `updated_at` | 纯元数据 | ✅ 可不写 |

> A-4 的结论：**开关控制的是"整条水位机制"，不是"写多写少"**；
> 而机制的成本几乎全部花在"只更新观测计数"的批次上 ⇒ **可省的是"写"，不是"机制"**。

### 5.4 建议的最小优化（**待批准**）

**时间节流写**（不改判定逻辑、不改表、不加迁移）：

```text
写库条件（任一成立）：
  ① action ∈ {init, advance, rewind_only}         // 判定字段变化 ⇒ 立即写
  ② rewind > 0                                    // 回退证据 ⇒ 立即写
  ③ observedMax > prev.observedMax                // 观测前沿推进（rebase 上界）⇒ 立即写
  ④ now - prev.last_seen_at ≥ W                   // 保 last_seen_at 新鲜（W 默认 600 s）
```

- **需要**在 `buildSelectSql()` 里多选 `last_seen_at`（同一行，**0 额外行读**）；
- 未命中的批次**跳过整个 `batch()`**（不写）；
- 常量 `W` 建议做成常量或 `env.LOG_BOOT_WM_WRITE_MIN_SEC`（默认 `600`），**不改现有开关语义**。

**预期效果（按 5.1 的实测基数计算，非估算）**

| 项 | 现状 | 节流后（W=600 s） | 变化 |
|---|---|---|---|
| 水位 upsert 执行 | ~11,000 /日 | **≤ 144 /日** | **−98.7%** |
| 水位 rows written | ~32,000 /日 | **~430 /日** | **−98.7%** |
| 占全天 written | 36–42% | **< 0.5%** | — |
| 水位 SELECT | ~11,800 /日 | 不变（仍每批一次） | 0 |
| `last_seen_at` 最旧 | 秒级 | ≤ 10 分钟 | 仍 ≪ 30 天 ⇒ rebase 安全 |

**明确代价（必须先认）**：`obs_count` / `ambiguous_count` 从"精确累计"变成"节流采样"，
两者都**不参与任何判定**（`analyzeBatch` 只读 `max`/`epoch`/`prevEpochMax`/`observedMax`），
但会让"水位卡死程度"的观测变成粗粒度。若要求计数精确 ⇒ 本方案不适用，需要另议
（例如由 cron 每日汇总，但那要新表 ⇒ 本阶段禁止）。

### 5.5 不建议做的

| 方案 | 否决理由 |
|---|---|
| 关闭 `LOG_BOOT_WATERMARK` | 丢失擦除/回退检测与 rebase 前置校验，属削弱可靠性 |
| 改成"仅 `advance` 才写" | 当前设备**永远不 advance**（§4.1）⇒ `last_seen_at` 冻结 ⇒ 30 天后 rebase 永久不可用 |
| 去掉每批 `SELECT`（isolate 缓存水位） | 多 isolate 下会用陈旧水位做判定 ⇒ 可能误判回退/掩盖真回退 |
| 合并进 `log_history` 写入 | 两者是不同表、不同事务语义；合并需要改表结构（本阶段禁止） |

---

## 第六部分：ACK / 重放可靠性结论

### 6.1 逐条核对（任务书要求项）

| 要求 | 结论 | 证据 |
|---|---|---|
| ACK 只能在已持久化或确认存在时发出 | ✅ **满足** | `history-ingest.js:356` 在 `:324 persist` 之后；`written[k]` 来自 `meta.changes>0`；未新增行必须 `fetchExistingRow` 独立证明 |
| `accepted` / `duplicate` 语义明确 | ✅ **满足** | `accepted` = 本次插入成功；`duplicate` = 键存在（独立 SELECT）；**不把"未落库"当已持久化**（`:754-755` 明确注释不得反推） |
| `rejected` / `filtered` 不被错误确认 | ✅ **满足** | 两者根本不进 `rows[]`（`:280-290`）⇒ 永不被 ACK |
| ACK 失败不破坏已完成的 D1 持久化 | ✅ **满足** | 发布在 persist 之后；`try/catch` 全捕获；HTTP 响应**不含** ACK 结果（`:340` 注释 + `:352` 之后的独立 try） |
| ACK 失败不让设备提前回收 | ✅ **满足** | 设备端 `log_ack_classify`：未覆盖 ⇒ `PARTIAL/IGNORE`；`GIVE_UP`（retry>5）**不推进** `acked_seq` 且登记 hole（`log_manager.cpp:3054`、`log_ack.h:175-185`） |
| 重试/重复/乱序/中断可恢复 | ✅ **满足（有界）** | 幂等键 `ux_lh_dedup(device_id,boot_seq,seq)`；Action 5 s 超时重投 ⇒ 撞键 ⇒ `duplicate` ⇒ 重发 ACK（设备判 `DUPLICATE` 无副作用）；`ACK_MAX_PER_REQUEST=20` + 3 s 预算 ⇒ 超出记 `skipped`，由设备重投收敛 |
| `acked_seq` / `gc_seq` / 回收一致性 | ✅ **满足** | `gc_seq = min(acked_seq, gc_floor-1)`（连续水位）⇒ `gc_seq < acked_seq` 即"存在被越过但未确认的旧记录"；回收要求"段末 seq ≤ gc_seq **且** 不与任何 hole 相交" |
| 保留 `log_conflict` / rewind / 重放保护 | ✅ 未削弱 | 冲突落库在 ACK 之后、失败不影响 ACK（`:814-830`） |
| 不假设天然有序 | ✅ 已按无序设计 | 逐条 ACK（`f = t = seq`，**绝不发 min/max 区间**）；乱序由水位分类（advance/ambiguous/rewind）承担 |

### 6.2 ACK topic（特别核查）

```js
logAckTopic(deviceId)        ⇒ `guo_feeder/<id>/log/ack`   // 生产主通道
legacyLogAckTopic(deviceId)  ⇒ `guo_feeder/<id>/down`      // 仅 LOG_ACK_DUAL_PUBLISH=1 时补发
```

- 生产 `LOG_ACK_DUAL_PUBLISH="0"` ⇒ **只发 `/log/ack`**，`/down` 上**没有** ACK。
- 载荷：`{"c":"log_ack","i":"ack:<boot>:<seq>","p":{"b":<boot>,"f":<seq>,"t":<seq>}}`，`qos=1`。
- 4 段 topic 是**有意**的：`domainFromTopic()` 只认 3 段 ⇒ 即使 EMQX 规则漏配，也会被
  `unknown_topic` 挡在入库之外。
- ⚠️ **`log_ack.test.mjs` 的 `T01f` / `T02e` 仍断言"发到 `/down`"** ⇒ 这 2 条是
  `LOG-ACK-SPLIT` 之后遗留的**过期断言**（历史失败，非产品缺陷）。

### 6.3 综合结论

> 云端"持久化 → ACK"的红线**没有被破坏**，且设备端"ACK → acked_seq → 回收"
> 有 hole 保护。**未发现需要修改 ACK/重放/回收逻辑的缺陷。**
>
> 阶段 3 唯一值得做的写入优化是 §5.4（水位节流），它与 ACK 链路**完全无关**，
> 不改任何可靠性语义。

---

## 第七部分：最小改动建议与审批清单

### 7.1 建议 A（低风险，无需改可靠性逻辑）：水位写入节流

- 文件：`guo-feeder-api/src/log-boot-watermark.js`（+ 常量）
- 内容：`buildSelectSql()` 多选 `last_seen_at`；`recordBootWatermark()` 在 `pending` 里做门槛判断，跳过不满足条件的设备
- 影响：**仅** `log_boot_watermark` 的写频率；判定逻辑、SQL 语义、表结构、响应契约**全部不变**
- 预期：水位 written −98.7%，占全天写 < 0.5%
- 风险：观测计数变采样值；`last_seen_at` 最长 10 分钟陈旧（rebase 阈值 30 天）
- 回滚：单函数级 revert，无迁移、无数据回填

### 7.2 建议 B（低风险，可选）：修正 `max_seq_at_max`（§4.2）

- 把 epoch upsert 的 `nextSeq` 改为"批次最大 seq"（`a.maxSeqAtMax`）而非 `a.nextSeq`
- 影响：仅该诊断列恢复推进；**不参与任何判定** ⇒ 零语义风险
- 价值：让 `W-3c` / `W-5b` 两条失败断言转绿，恢复"诊断可用"
- 注意：这会改变已写入的历史值含义（旧值冻结，新值开始推进）⇒ 需在报告登记

### 7.3 建议 C（零行为）：修正 `log-ack.js` 头部 `/down` 过期注释（§4.4）

### 7.4 【必须单独批准】以下一律未做

| # | 事项 | 为什么需要单独批准 |
|---|---|---|
| P-1 | 建议 A 落地（改 `log-boot-watermark.js`） | 触及写入路径与观测口径 |
| P-2 | 建议 B 落地（改 epoch upsert 绑定） | 触及水位表写入语义 |
| P-3 | 是否接受 `obs_count`/`ambiguous_count` 变为采样 | 观测口径决定 |
| P-4 | `W` 取值（600 s / 3600 s / 其他） | 影响 rebase 可用性的边界 |
| P-5 | 部署顺序与开关（是否用 `env` 控制灰度） | 生产发布决策 |
| P-6 | 修 `log_ack.test.mjs` 的 2 条过期断言（`/down` → `/log/ack`） | 改的是契约断言，需确认口径 |
| P-7 | 是否为 §4.1 的"水位卡死"做**运维处置**（等设备重启超 117 / 再做一次 rebase） | 涉及生产数据判断 |

---

## 第八部分：测试结果

### 8.1 本次运行（日志/ACK/水位相关）

| 套件 | 类型 | 结果 |
|---|---|---|
| `log_ack` | mock | 58 / **2 FAIL**（T01f/T02e：过期 `/down` 断言，见 §6.2） |
| `log_ack_edge` | mock | 48 / 0 |
| `log_ack_contract` | mock | 57 / 0 |
| `log_ack_publish_strict` | mock | 76 / 0 |
| `log_ack_timeout_phase` | mock | 36 / 0 |
| `log_ack_collision` | mock | 61 / 0 |
| `log_conflict` | mock | 54 / 0 |
| `log_conflict_retry` | mock | 38 / 0 |
| `log_bootseq_guard` | mock | 31 / 0 |
| `log_boot_watermark` | mock | 48 / 0 |
| `log_boot_watermark_rebase` | mock | 97 / 0 |
| `history_ingest` | mock | 35 / 0 |
| `log_level_contract` | mock | 95 / 0 |
| `real_d1/history_ingest` | **真实 D1 引擎** | **124 / 0**（含本次修正的 1 条过期断言） |
| `real_d1/log_boot_watermark` | **真实 D1 引擎** | **43 / 2 FAIL**（W-3c / W-5b，见 §4.2） |

**mock 合计**：734 PASS / 2 FAIL（两套件，均为过期断言）。
**真实 D1**：167 PASS / 2 FAIL。

> "mock" 判定依据：这些套件用手写 `prepare(sql)` 桩（`test/current/*.test.mjs`），
> 不经过真实 SQLite 引擎 ⇒ 覆盖的是**调用顺序与桩返回值**，不是 SQL 语义。
> 真实 D1 套件走 `wrangler d1 --local`（miniflare），覆盖唯一索引、`ON CONFLICT`、
> `MAX()`、`pragma` 等**只有真引擎才能证明**的行为。

### 8.2 历史失败 vs 本次新增（逐条定性）

| 失败 | 定性 | 依据 |
|---|---|---|
| `log_ack` T01f / T02e | **历史**（`LOG-ACK-SPLIT` 遗留） | 断言写 `/down`，实现是 `/log/ack`；实现自 `8e8d3e0` 起零改动 |
| `W-3c` / `W-5b` | **历史**（0006 epoch 化引入） | 期望值基于 legacy SQL 行为；`log-boot-watermark.js` 自 `8e8d3e0` 起 `git diff` 空 |
| ~~`history_ingest` "本地库无 mqtt_messages 表"~~ | **本次 D1 提交引入的过期断言**（已在本次修正） | 0000 迁移让空库也建出该表；改为断言"表存在但 0 行"⇒ 124/0 |
| ~~`W-2e` 索引数 5→7~~ | 同上（0009 按设计新增 2 索引） | 改为断言"0003 的 5 个索引仍在"⇒ 通过 |
| ~~`log_boot_watermark` "本地库无 mqtt_messages"~~ | 同上 | 同上 |

> ⚠️ **诚实记录**：后三条是**我在 D1 提交（`75b22b8`）里引入的测试基线假红**，
> 因为没有把 `real_d1/*.sh` 纳入该次回归范围（只跑了 `run_all.mjs` + 两个 real_d1 脚本）。
> 本次已修正断言（**只改测试脚本，未改任何产品代码**），并在此明确登记。

### 8.3 覆盖缺口（未验证，不宣称）

| 缺口 | 说明 |
|---|---|
| ACK 发布失败 → 设备重投 → 再 ACK 的**端到端** | 需要真实 EMQX + 真机；现有测试只覆盖"单次发布的判定" |
| 两个并发请求含相同 `(boot,seq)` | 无并发测试（D1 唯一索引保证不会重复插入，但 ACK 双发未测） |
| `filtered` 在生产恒为 0 | 未在真机验证（§4.5） |
| `INGEST_VALIDATION="observe"` 下校验失败条目的 ACK 处置 | 未覆盖（§4.6） |
| `ACK_PUBLISH_TIMEOUT_MS=1500` 是否够 | 历史生产样本 `4 published / 3 unknown(TIMEOUT)`，**仍未调参**（既有 `Q-9`） |

---

## 第九部分：本次是否触及生产环境

| 对象 | 是否改动 |
|---|---|
| 固件 / 设备端协议 | **未改**（仅只读阅读 `log_ack.h` / `log_manager.cpp` / `log_events.h`） |
| D1 迁移 `0001–0009` | **未改**；**未新增**迁移 |
| 表结构 | **未改** |
| EMQX 规则 / Action / Connector / 认证 / ACL | **未改** |
| Worker / Pages 代码 | **未改**（阶段 3 只读） |
| 生产配置（`LOG_BOOT_WATERMARK` / `LOG_ACK_DUAL_PUBLISH` / `INGEST_VALIDATION`） | **未改** |
| 生产数据 | 只有**只读查询**（`SELECT`）；无 INSERT/UPDATE/DELETE |
| 测试脚本 | ✅ 改了 2 个 `real_d1/*.sh` 的**过期断言**（见 §8.2），未动产品代码 |

> 阶段 2 的推送与部署发生在第一部分（已在上次报告登记），阶段 3 本身**零生产变更**。

---

## 第十部分：下一步建议

1. **优先批 P-1 / P-4**（水位节流 + `W` 取值）——这是本阶段唯一"付了钱没产出"的确定项，
   且与 ACK 链路完全无关，改动面最小。
2. **P-7 单独决策**：当前水位卡在 21 是 rebase 的合法结果；要不要处理（等设备自然越过 117，
   或再做一次 rebase）**属于运维判断，不是代码问题**，我不替你决定。
3. **P-6 顺手修**（2 条 `/down` 过期断言），让 `log_ack` 套件转绿——纯测试，零风险。
4. §8.3 的端到端缺口建议列入阶段 3 的**验证任务**（真机 + EMQX），但这需要设备在线时间窗。
5. 阶段 4（MQTT 客户端与安全）按计划独立开展；UI 控制命令迁 MQTT 待全部阶段结束后再议。
