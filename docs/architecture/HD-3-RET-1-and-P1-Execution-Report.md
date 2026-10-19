# RET-1 生产行为验收 + P-1 Watermark Rebase 执行报告

- 执行时间：**2026-10-19 21:35–21:52**（UTC `13:35`–`13:52`）
- 设备：`288485896ce4`
- 执行顺序：阶段 A（RET-1 核验）→ A 结果检查 → 阶段 B 前置复核 → dry-run → 正式 rebase → 复核 → 10 分钟观察
- 结论：**阶段 A = PASS**（代码已部署 / 追认 / 行为验收三项独立成立）；**阶段 B = PASS**（`epoch 1→2`，`max_boot_seq 117→21`，审计 1 条，事务 1/1）

> 凭证纪律：生产 `ADMIN_API_TOKEN` 仅存在于当次 shell 进程环境，**未写入本报告、未落盘任何文件**。

---

## 阶段 A：RET-1 部署登记与生产行为验收

### A1. 部署事实

| 项 | 证据 |
|---|---|
| 提交 | `a3473eb06165e8b36f360b09aa5010b92e6b0c4e` · `2026-10-18 20:07:15 +0800`（= `12:07:15 UTC`） |
| 提交内容 | `src/retention.js +44/−4` · `test/current/retention_policy.test.mjs +320/−6` · `test/real_d1/retention_real_d1.sh +116/−0`（3 文件，`480 insertions / 10 deletions`） |
| 与部署的对应 | `f9647a00` 部署于 `2026-10-18 20:18:43 UTC`，晚于提交 **8 小时 11 分**（同一工作树） |
| 当前版本是否仍含 | ✅ `826332ed`（`2026-10-19 12:56:06 UTC`）构建产物中命中 `ts <= 0 AND created_at`（`dryrun/index.js`） |

**影响范围**（逐字核对，非推断）：

```sql
-- log_history（已改）
DELETE FROM log_history WHERE device_id = ?
  AND ( (ts > 0 AND ts < ?) OR (ts <= 0 AND created_at < ?) ) LIMIT ?;
-- telemetry_history（逐字未变 · H-RET-1 = 仅 log）
DELETE FROM telemetry_history WHERE device_id = ? AND ts < ? LIMIT ?;
```

`git show a3473eb` 中对 `telemetry` 的改动**全部是注释与测试断言**（`T-9`：`telemetry` SQL 与绑定顺序逐字不变），
**没有**任何 `+`/`-` 行改动 telemetry 的谓词。

> **任务要求"不得仅凭提交时间推断代码被执行"** —— 故 A1 只确立"代码在版本中"，
> "是否真的执行"由 A2 独立证明（见下）。

### A2. 生产行为验收

#### A2-1 取证通道现状

| 通道 | 结果 |
|---|---|
| Workers Logs 查询 API（`/workers/observability/telemetry/query`） | ❌ **403** `Authentication error`（当前 OAuth 令牌无该端点权限） |
| `wrangler tail` | ❌ **502**（代理隧道）／直连被沙箱拦截 |
| Cloudflare GraphQL `workersInvocationsAdaptive` | ✅ **可用** —— 改由此证明 cron 执行 |

> 因此 `[retention] deleted=N` 的**日志原文未取得**。行为结论由 **cron 执行证据 + 删除未发生 + 批次上限**三方推出。

#### A2-2 cron 执行证据（GraphQL，只读）

逐日 `02:58–03:06Z` 窗口：

| 日期 | 调用 | 错误 |
|---|---|---|
| `2026-10-17` | `03:00:14Z` | 0 |
| `2026-10-18` | `03:00:14Z` | 0 |
| **`2026-10-19`** | **`03:00:14Z`** | **0** |

**三天同一秒**出现，与 `[triggers] 0 3 * * *` 的调度指纹一致 ⇒ 该次为 cron。
（`10-19 02:30–03:30Z` 的完整窗口共 8 次调用，其余时间散乱，属设备上行。）

#### A2-3 数据侧证据

| 查询 | 结果 |
|---|---|
| `log_history` 中 `ts<=0` | **238 行** · `ts_min=ts_max=0` · `created_at` `1792344057–1792354919`（`10-18 17:20:57Z – 20:21:59Z`） |
| `telemetry_history` | 20 行 · `ts<=0` = **0 行** |
| 默认批次 | `RETENTION_BATCH_SIZE = 1000` |

#### A2-4 推理链（区分「新谓词保留」与「cron 没跑」）

1. cron 在 `10-19 03:00:14Z` **确实执行**，`errors=0`（A2-2）。
2. 那 238 行**在 cron 之前就已存在**（`created_at` 全部早于 `03:00:14Z`）。
3. 旧谓词 `ts < cutoff`（`cutoff ≈ now − 90 d`）会命中**全部** `ts=0` 行。
4. 批次上限 **1000 > 238** ⇒ 单条 `DELETE` 即可全删，不存在"删不完"。
5. 实测 **0 行减少** ⇒ 执行的只能是新谓词（`ts<=0` 走 `created_at`，均在 90 天窗口内 ⇒ 保留）。

**结论：`RET-1` 的 `created_at` 兜底已在生产真实执行。**

`telemetry_history` 未被波及：谓词逐字未变（源码 + `T-9` 断言）；生产侧 `ts<=0` 计 0 行，无异常删除迹象。

### A3. 登记与追认（三字段独立）

| 字段 | 状态 |
|---|---|
| **① 代码已部署** | ✅ `DEPLOYED`（`f9647a00` 起，`826332ed` 仍在） |
| **② 所有者追认** | ✅ `RATIFIED`（`2026-10-19 21:35`，仅追认"已部署"这一事实） |
| **③ 生产行为验收** | ✅ `PASS`（覆盖 `10-19 03:00:14Z` 这一次 cron） |

登记册 `docs/issues/未修复的问题.md` 已**追加**更正块（**保留原快照与历史结论**，不覆盖）。
追认**不**代表批准任何额外 retention 策略、删除规则或数据库修改。

### A4. 阶段 A 未解决问题

| # | 问题 | 级别 |
|---|---|---|
| A-1 | `[retention] deleted=N` 日志原文取不到（Logs 端点 403 / tail 502）⇒ 结论为推理非直读 | MINOR（取证通道） |
| A-2 | 行为验收只覆盖 `10-19` 一次 cron，持续正确性未逐日复核 | MINOR |
| A-3 | `H-RET-4` 的**授权缺口**已追认，但"先上线后追认"的流程偏差本身应记入纪律条款 | MAJOR（流程） |

---

## 阶段 B：P-1 生产 Watermark Rebase

### B1. 执行参数（与任务单一致，未改动）

```json
{
  "device_id": "288485896ce4",
  "rebase_id": "rb-20261019-0001",
  "reason": "full_erase_confirmed",
  "expected_epoch": 1,
  "expected_max_boot_seq": 117,
  "confirm_boot_seq": 21,
  "evidence": "RD-8：2026-10-19 02:31 双擦记录；recovery=PROVISION；设备当前连续上报 boot_seq=21，最新 seq=5169。"
}
```

### B2. 写入前重新核验（`13:37:51 UTC`，全部重新查询，不引用旧报告）

| 条件 | 实测 | 判定 |
|---|---|---|
| 设备正确 | `device_id=288485896ce4` 唯一行 | ✅ |
| `status` | `open` | ✅ |
| `epoch` | 1 | ✅ |
| `max_boot_seq` | 117 | ✅ |
| `observed_max_boot_seq` | 117 | ✅ |
| `prev_epoch_max` | `NULL` | ✅ |
| `rewind_count` | **433** | ✅（与 dry-run 预期一致） |
| `last_rebase_id` / `last_rebase_at` | `NULL` / `NULL` | ✅（从未 rebase） |
| `confirm_boot_seq=21` 约束 | `21 < 117` 且 `21 <= 117` | ✅ |
| 设备上报新鲜度 | 最新 `boot=21 · seq=5169 · created_at 13:00:41Z`；`last_seen_at=13:00:07Z`（约 38 分钟前，远在 30 天内） | ✅ |
| `reason` ∈ 枚举 / `evidence` 非空 ≤512 | `full_erase_confirmed` / 长度合规 | ✅ |
| 认证 | dry-run 返回 200 即证明令牌有效 | ✅ |
| 请求目标 | `https://guo-feeder-api.guobaby.workers.dev`（生产 Worker，非 Pages、非本地） | ✅ |
| 审计表 | **0 行** | ✅ |
| 快照留存 | `b2-watermark-before.json` / `b2-audit-before.json` / `b2-device-latest.json` | ✅ |

### B3. dry-run（`13:39:44 UTC`，最终参数）

```json
{"ok":true,"applied":false,"dry_run":true,"would_apply":true,
 "device_id":"288485896ce4","rebase_id":"rb-20261019-0001",
 "epoch_now":1,"epoch_after":2,
 "max_boot_seq_now":117,"max_boot_seq_after":21,
 "prev_epoch_max_after":117,"observed_max_boot_seq":117,
 "rewind_count_reset_from":433,"ambiguous_count_reset_from":0}
```

**零写入复核**（`13:40:09 UTC`）：`epoch=1` · `max_boot_seq=117` · `prev_epoch_max=NULL` ·
`rewind_count=433` · `last_rebase_id=NULL` · `last_rebase_at=NULL` · 审计 **0 行** —— 与 dry-run 前**逐字段一致**。

### B4. 正式执行（`13:40:26 UTC`，`dry_run:false`，同 ID 同参数）

```json
{"ok":true,"applied":true,"replay":false,
 "device_id":"288485896ce4","rebase_id":"rb-20261019-0001",
 "epoch":2,"max_boot_seq":21,"prev_epoch_max":117,
 "rewind_count_reset_from":433,"ambiguous_count_reset_from":0,
 "changes":{"update":1,"audit":1}}
```

`changes = update 1 / audit 1` ⇒ **CAS 更新与审计插入在同一事务内全部命中**（无部分成功）。
无 `503 outcome_unknown`，未发生重试。

### B5. 执行后核验（`13:40:35 UTC`）

**水位行**：

| 字段 | 期望 | 实测 | 判定 |
|---|---|---|---|
| `epoch` | 2 | **2** | ✅ |
| `max_boot_seq` | 21 | **21** | ✅ |
| `prev_epoch_max` | 117 | **117** | ✅ |
| `rewind_count` | 0 | **0** | ✅ |
| `ambiguous_count` | 0 | **0** | ✅ |
| `last_rebase_id` | `rb-20261019-0001` | **一致** | ✅ |
| `last_rebase_at` / `epoch_started_at` | 已写入 | `1792417227`（`13:40:27Z`）| ✅ |
| `status` | 不变 | `open` | ✅ |

**审计行（恰好 1 条）**：

```
rebase_id             rb-20261019-0001
device_id             288485896ce4
epoch_before/after    1 → 2
max_boot_seq_before   117 → after 21
prev_epoch_max_after  117
rewind_count_before   433
ambiguous_count_before 0
reason                full_erase_confirmed
evidence              RD-8：2026-10-19 02:31 双擦记录；recovery=PROVISION；设备当前连续上报 boot_seq=21，最新 seq=5169。
actor                 admin#f8122859（指纹，非明文）
params_hash           92ba5c4fac9795cd
result                applied
created_at            1792417227
```

### B6. 10 分钟观察（`13:40:53`–`13:51:55 UTC`，6 次采样）

| 时刻（UTC） | `rewind_count` | rebase 后新入日志 |
|---|---|---|
| 13:40:53 | 0 | 0 |
| 13:43:05 | **0** | 2 |
| 13:45:18 | **0** | 4 |
| 13:47:30 | **0** | 4 |
| 13:49:42 | **0** | 4 |
| 13:51:55 | **0** | 4 |

新入日志（`boot_seq` 全为 21）：`seq 5170→5173`，`created_at 13:42:23 – 13:44:23Z`。

⇒ **rebase 后设备继续以 `boot=21` 上报 4 个批次，`rewind_count` 保持 0**（未再累加）。
最终水位 `last_seen_at = 13:44:22Z`、`updated_at` 同步推进 ⇒ 观测链路正常。

> ⚠️ **`[log-bootwm]` 的控制台行文本未直接读取**（tail 502 / Logs 403）。
> 上述"未再报 `REWIND_CANDIDATE`"是**由 `rewind_count` 不再增长推出的**（该计数只在判定 rewind 时递增），
> 不是日志直读——两者不得混为一谈。

### B7. 盲区（本次执行成功**不消除**）

rebase 后 `prev_epoch_max=117`、`max_boot_seq=21`：
**`(21, 117]` 区间内的真实回退，不会被原高水位比较机制识别**，直到设备真实 `boot > 117`。
该期间的唯一可靠判据是**设备侧** `[Log] recovery=` 与 `boot_seq` 连续性（Runbook §4）。

---

## 生产变更清单

| # | 变更 | 类型 | 结果 |
|---|---|---|---|
| 1 | `log_boot_watermark` 单行 UPDATE（`288485896ce4`） | 生产 D1 写入 | ✅ 1 行 |
| 2 | `log_boot_watermark_audit` INSERT | 生产 D1 写入 | ✅ 1 行 |
| 3 | 上述二者 | 同一 `db.batch()` 事务 | ✅ `1/1` |
| 4 | Worker 部署 | — | **NOT RUN**（本任务未部署） |
| 5 | migration apply | — | **NOT RUN** |
| 6 | Schema/DDL 变更 | — | **NOT RUN** |
| 7 | 设备固件 / 复位 / 烧录 | — | **NOT RUN** |
| 8 | EMQX 配置 / ACL | — | **NOT RUN** |
| 9 | Git commit / push | — | **NOT RUN** |
| 10 | dry-run 三次（预检、B3、正式前） | 只读 | 均零写入（已逐字段复核） |
| 11 | GraphQL 只读查询（cron 证据） | 只读 | 无写入 |

**受影响的其它表**：无。`log_history` / `telemetry_history` / `mqtt_messages` 本次未删除、未修改。

---

## 结论

| 对象 | 状态 | 依据 |
|---|---|---|
| **RET-1 · 代码已部署** | ✅ `DEPLOYED` | `a3473eb` 提交 → `f9647a00` 部署（+8h11m）→ `826332ed` 仍含 |
| **RET-1 · 所有者追认** | ✅ `RATIFIED` | 本任务授权范围 |
| **RET-1 · 生产行为验收** | ✅ `PASS`（覆盖 `10-19 03:00:14Z` 一次 cron） | cron 执行 + 238 行未删 + 批次 1000 |
| **P-1 · 正式 rebase** | ✅ `PASS` | `changes 1/1` · 水位四字段正确 · 审计 1 条 · 10 分钟无 rewind 复现 |
| **P-1 · 回退** | — | **NOT RUN**（无需回退；本任务亦未获逆向修复授权） |

**不得夸大**：
- RET-1 的行为结论是**推理链**（cron 证据 + 数据未删 + 批次上限），**不是**日志直读。
- P-1 的"不再误报"是**计数不再增长**的推断，**不是**日志行直读。
- 二者均只覆盖**本次观察窗口**，不构成长期保证。

---

## 剩余门禁（本轮**无任何关闭证据**）

| 门禁 | 状态 | 说明 |
|---|---|---|
| `BT-1` | **OPEN** | 本轮未做 BT-1 验收流程 |
| `G-4` | **BLOCKED** | 依赖 BT-1，仍未关闭 |
| `WP-5` | **未解锁** | 依赖 G-4 |
| `STOP-R1` | **未解除** | 无书面解除文件 |

---

## 证据归档

| 目录 | 内容 |
|---|---|
| `docs/evidence/HD-3-RET-1-2026-10-19/` | GraphQL 查询体与响应、`ts<=0` 数据快照、README（含推理链与未取得项） |
| `docs/evidence/HD-3-P1-2026-10-19/` | `b2` 前置快照 3 份 · `b3` dry-run 响应与零写入复核 · `b4` 正式执行响应 · `b5` 水位与审计 · `b6` 10 分钟观察与新增日志 |
| `docs/evidence/HD-3-P2-2026-10-19/` | （前一批）P-2 部署与 E2E 原始证据 |
