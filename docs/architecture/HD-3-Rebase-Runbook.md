# HD-3 水位 rebase 运维手册（`R-11`）

> 版本：`rev.1`（`2026-10-19`）
> 适用：`log_boot_watermark` 的受控换代（epoch）
> 状态：**接口已实现（本地）· `0006` 未应用 · Worker 未部署**
> 关联：`HD-3-Risk-Closure-Plan.md`（`rev.2`）§2 · 批次 1 指令

---

## 1. 为什么需要这个操作

`log_boot_watermark.max_boot_seq` 是**单调标量**（`MAX()` 只升不降）。
一旦设备发生**合法的整片擦除**（`NVS + LittleFS` 同丢 ⇒ `boot_seq` 回 1），
历史峰值会永久压住设备真实值：

```
历史上到过 117 → 擦除后设备从 1 重新计数 → 每次正常启动（如 18）都被判 rewind
实测：rewind_count 266 / obs_count 276 = 96.4% 误报
```

结果是 `rewind_count` 从"异常信号"退化为"几乎等于观测数的计数器"，
`status` 长期 `open`，形成**告警疲劳**。

本操作通过引入**世代（epoch）**把"这次下调"变成一件**有审计、可复核**的事，
同时**保留**"曾经到过 117"这一事实（写进 `prev_epoch_max` 与审计表）。

---

## 2. 允许与拒绝

### 2.1 允许（全部满足）

| # | 条件 | 说明 |
|---|---|---|
| 1 | `status ∈ {open, reviewed}` | `incident` 必须先人工处置 |
| 2 | `expected_epoch` / `expected_max_boot_seq` 与库中**当前值**一致 | CAS；不一致说明期间有变化，需重新核对 |
| 3 | `confirm_boot_seq < max_boot_seq` | rebase 必须是"下调"，否则无意义 |
| 4 | `confirm_boot_seq <= observed_max_boot_seq` | **上界来自服务端记录的观测**，请求不能自证 |
| 5 | `observed_max_boot_seq >= 1` | 必须真的观测过 |
| 6 | `now - last_seen_at <= 30 天` | 观测太旧则不可信 |
| 7 | `reason` ∈ 枚举 且 `evidence` 非空 | 审计必需 |

### 2.2 必须拒绝

- `status = incident`；
- 任一 CAS 不匹配；
- `confirm >= max_boot_seq`（不是下调）；
- `confirm > observed_max_boot_seq`（超出服务端观测）；
- 观测超过 30 天（先让设备上报再操作）；
- 同一 `rebase_id` 被用于**不同设备或不同参数**（防 ID 复用掩盖操作）。

> ⚠️ **服务端无法证明设备此刻真的在 `confirm_boot_seq`**（消息不带可信代际标识）。
> 因此 `evidence` 是必需项，且必须能指向**设备侧证据**（串口 `[Log] recovery=PROVISION`、
> 擦除操作记录、工单号）。这是**设计上的已知限制**，不是遗漏。

---

## 3. 操作步骤

### 3.1 前置取证（必须）

1. 设备侧：串口确认 `[Log] recovery=PROVISION`（或 `factory_reset` 记录），
   并记录当时的 `boot_seq`；
2. 云端：确认该设备最近的观测时间与 `observed_max_boot_seq`；
3. 记录 `epoch` 与 `max_boot_seq` 当前值（CAS 用）。

### 3.2 先 dry-run

```bash
curl -sS -X POST \
  "https://<worker>/api/admin/device/<device_id>/watermark/rebase" \
  -H "Authorization: Bearer $ADMIN_API_TOKEN" \
  -H "content-type: application/json" \
  -d '{
    "rebase_id": "rb-20261019-0001",
    "reason": "full_erase_confirmed",
    "evidence": "RD-8 2026-10-19 02:31 NVS+LittleFS 双擦，recovery=PROVISION，boot_seq=1",
    "expected_epoch": 1,
    "expected_max_boot_seq": 117,
    "confirm_boot_seq": 18,
    "dry_run": true
  }'
```

返回 `would_apply: true` 且 `epoch_after / max_boot_seq_after / prev_epoch_max_after`
符合预期后，再执行正式调用（`dry_run: false` 或省略）。

> `dry_run` 与正式执行**使用同一套校验**；它**零写入**（不写水位、不写审计）。

### 3.3 执行

同一 `rebase_id`，`dry_run: false`。

### 3.4 成功后核验

```sql
SELECT epoch, max_boot_seq, prev_epoch_max, observed_max_boot_seq,
       rewind_count, ambiguous_count, last_rebase_id
  FROM log_boot_watermark WHERE device_id = '<id>';

SELECT * FROM log_boot_watermark_audit WHERE rebase_id = '<rid>';
```

期望：`epoch` +1、`max_boot_seq` = `confirm`、`prev_epoch_max` = 旧峰值、
`rewind_count = 0`、审计 1 行且 `max_boot_seq_before` = 旧峰值。

---

## 4. ★ 已知盲区（必须原样告知使用者）

rebase 后：

```
max_boot_seq     = 18      ← 判定基线
prev_epoch_max   = 117     ← 歧义带上界
observed_max_boot_seq = 100（随观测上升）
```

`19 … 117` 这个区间的 `boot_seq` 有两种可能，**无法区分**：

- 旧代迟到日志（擦除前发出、现在才到）；
- 新代正常前进（设备从 1 重新计数，已到 100）。

因此本设计规定：

| 区间 | 处理 |
|---|---|
| `boot > 117` | `new_epoch`：清空 `prev_epoch_max`、`max := boot`，**完全恢复**常规检测 |
| `18 < boot <= 117` | `ambiguous_forward`：只抬 `observed_max_boot_seq`、`ambiguous_count++`；**`max` 不动、不计 rewind** |
| `boot < 18` | `rewind_candidate`（照常计数） |

**后果**：在歧义带内，**真回退检测是关闭的**。
这是没有"可信代际标识"时的**结构性代价**，不能用软件消除。

**运维含义**：

- 不要用 `rewind_count` 当告警阈值（它是观测次数，重投会放大）；
- 歧义带期间，回退的唯一可靠判据是**设备侧** `[Log] recovery=` 与 `boot_seq` 连续性；
- 看到 `ambiguous_count` 增长 = **正常**（设备在前进），不是异常。

---

## 5. 幂等、并发与失败

| 情形 | 行为 |
|---|---|
| 同 `rebase_id` 同参数重放 | 回放首次结果，**零写入** |
| 同 `rebase_id` 换设备/换参数 | `409 rebase_id_conflict` |
| 并发（另一个请求先成功） | CAS 未命中 ⇒ `0/0` ⇒ `409 cas_mismatch` |
| 审计写入失败 | 同一 `batch()` 事务 ⇒ 整体回滚，水位不变 |
| `batch()` 抛错但审计已存在 | 视为**已提交**，返回 `200 replay`（`recovered_from_error`） |
| `batch()` 抛错且审计不存在 | `503 outcome_unknown` ⇒ **用同一 `rebase_id` 重试**（不要换 ID） |
| `changes` 组合异常（如 `1/0`） | `500 transaction_inconsistent` + `audit_present` 供人工核验 |

> **判据**：`batch()` = SQL 事务，失败会回滚整串；返回**按序**的 `D1Result[]`。
> 所以必须读 `meta.changes`，不能只看"有没有抛错"。

---

## 6. 回滚

- `0006` 是**加列 + 加表**，无数据破坏；`epoch/prev_epoch_max` 只增字段。
- rebase 本身**不可"撤销"**（审计已记账）。若确需恢复旧水位：
  1. 从 `log_boot_watermark_audit` 取 `max_boot_seq_before`；
  2. **另一次** rebase 不能上调（`confirm < max` 是硬条件）⇒ 需人工 SQL + 新的审计说明；
  3. **不得删除审计行**（那是唯一留证）。
- 关闭水位功能：`LOG_BOOT_WATERMARK=0` 后重新 `deploy`（**不删表**）。

---

## 7. 迁移状态澄清（`D-1`）

`migrations/0004_log_conflict.sql` 与 `migrations/0005_log_boot_watermark.sql`
的**文件头仍写着「草案，禁止应用 / 位置 `drafts/`」**，这是**历史文本**：

| 文件 | 文件头写的 | 实际状态 |
|---|---|---|
| `0004_log_conflict.sql` | 草案 / `drafts/` | **已转正**（`362ddfd`）并**已应用**到生产 D1 |
| `0005_log_boot_watermark.sql` | 草案 / `drafts/` | **已转正**（`362ddfd`）并**已应用**到生产 D1 |
| `0006_log_boot_watermark_epoch.sql` | ★ **已更正**（`2026-10-19`）：仅描述「它是什么」，不维护应用状态 | **已转正**（位于 `migrations/` 顶层）并**已应用**于 `2026-10-18 20:18:26 UTC`（`d1_migrations` id=6） |

**为什么不改 0004 / 0005 的文件头**：改已应用 migration 的内容会让"仓库里的文件"与
"当时实际执行过的 SQL"不一致，破坏审计链。以本表为准。

**为什么 0006 改了**：只改**注释**（`--` 行），SQL 语义零改动；且按其时结论
「在 SQL 文件头维护易变的应用状态本身就是缺陷」（`D-1`），故改为不描述状态、
状态统一由 `migrations/README.md` 与 `d1_migrations` 承载。

---

## 8. 生产操作授权

本手册描述的**所有**操作（`0006` 应用、Worker 部署、真实 rebase）都需要
**单独明确授权**。仓库中存在代码 ≠ 已授权执行。

---

## 9. 执行记录

| 日期（UTC） | `rebase_id` | 设备 | 变更 | 结果 |
|---|---|---|---|---|
| `2026-10-19 13:40:27` | `rb-20261019-0001` | `288485896ce4` | `epoch 1→2` · `max 117→21` · `prev_epoch_max=117` · `rewind_count 433→0` | **`applied`**（`changes 1/1`，审计 1 条）· 10 分钟观察无 rewind 复现 |

> 详细证据：`docs/architecture/HD-3-RET-1-and-P1-Execution-Report.md` ·
> `docs/evidence/HD-3-P1-2026-10-19/`。
> 执行后仍处于 §4 的**盲区**：`(21,117]` 区间的真回退暂不可识别（直到 `boot>117`）。
