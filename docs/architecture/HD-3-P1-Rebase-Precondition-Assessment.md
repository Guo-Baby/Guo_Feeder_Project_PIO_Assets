# 生产 rebase（`P-1`）前置条件核验与回退方案

- 核验日期：**2026-10-19 21:08**（UTC `13:08`）
- 设备：`288485896ce4`
- 性质：**只读核验 + 零写入 dry-run**。**未执行正式 rebase**，生产水位未变。
- 结论：**技术前提全部满足**（`would_apply: true`）；**仍缺"执行授权"这一项非技术前提**。

> 凭证纪律：本次使用所有者提供的生产 `ADMIN_API_TOKEN` 完成 dry-run。
> **令牌值不写入本文件、不写入任何仓库文件、不落盘**（仅存在于该次 shell 进程环境）。

---

## 1. 结论摘要

| 维度 | 状态 | 说明 |
|---|---|---|
| 认证 | ✅ **通过** | `Authorization: Bearer <生产 ADMIN_API_TOKEN>` 返回 `200`；此前 401 的根因确认为**本地 `.dev.vars` 令牌 ≠ 生产 secret**，**不是**生产缺 secret |
| 业务前置（Runbook §2.1 七条） | ✅ **全部满足** | 见 §3 逐条 |
| dry-run 结果 | ✅ `would_apply: true` | `epoch 1→2` · `max_boot_seq 117→21` · `prev_epoch_max=117` |
| dry-run 零写入 | ✅ **已验证** | 调用后水位与审计表**逐字段不变**，审计仍 0 行 |
| 回退方案 | ✅ 有，但**非对称** | rebase **不可逆**（审计记账）；需人工 SQL + 新审计说明（见 §5） |
| **执行授权** | ⛔ **未授予** | Runbook §8：所有生产操作需**单独明确授权**；仓库有代码 ≠ 已授权 |

---

## 2. 现状（只读查询，`2026-10-19 13:05–13:08 UTC`）

### 2.1 云端水位行 `log_boot_watermark`

| 字段 | 值 |
|---|---|
| `device_id` | `288485896ce4` |
| `max_boot_seq` | **117** |
| `observed_max_boot_seq` | **117** |
| `epoch` | **1** |
| `epoch_started_at` | `NULL` |
| `prev_epoch_max` | `NULL` |
| `ambiguous_count` | 0 |
| `rewind_count` | **433** |
| `last_rewind_at` / `last_rewind_boot` | `1792414807` / **21** |
| `status` | **`open`** |
| `last_seen_at` | `1792414807`（`2026-10-19 13:00:07 UTC`，约 6 分钟前） |
| `last_rebase_id` / `last_rebase_at` | `NULL` / `NULL` |
| `first_seen_at` | `1792345018`（`2026-10-18 17:36:58 UTC`） |

### 2.2 设备侧实际代数

| 证据 | 值 |
|---|---|
| 最新日志行 | `boot_seq=21` · `seq=5169` · `ts=1792414737` · `created_at=1792414741`（`13:00:41 UTC`） |
| 前一条 | `boot_seq=21` · `seq=5168` · `created_at=1792414685` |
| 结论 | 设备**当前稳定在 `boot=21`**，且仍在正常上报（非离线推测） |

> 118 ↔ 21 的差不是"设备乱跳"，而是 **10-19 02:31 全擦后 boot 归 1 重新累计**，
> 而云端 `max_boot_seq=117` 是**擦除前**的历史高水位 ⇒ 每次上报 `boot=21 < 117` 都被判 `REWIND_CANDIDATE`。

### 2.3 审计表现状

`log_boot_watermark_audit` = **0 行**（从未 rebase 过，与 `last_rebase_id=NULL` 一致）。

---

## 3. Runbook §2.1 七条逐条核对

| # | 条件 | 实际值 | 判定 |
|---|---|---|---|
| 1 | `status ∈ {open, reviewed}` | `open` | ✅ |
| 2 | `expected_epoch` / `expected_max_boot_seq` 与库中当前值一致 | 请求 `1 / 117` vs 库 `1 / 117` | ✅ |
| 3 | `confirm_boot_seq < max_boot_seq` | `21 < 117` | ✅ |
| 4 | `confirm_boot_seq <= observed_max_boot_seq` | `21 <= 117` | ✅ |
| 5 | `observed_max_boot_seq >= 1` | `117` | ✅ |
| 6 | `now - last_seen_at <= 30 天` | 约 6 分钟 | ✅ |
| 7 | `reason ∈ 枚举` 且 `evidence` 非空 | `full_erase_confirmed` + 设备侧擦除记录（`RD-8`，`2026-10-19 02:31`，`recovery=PROVISION`，`boot_seq=1`） | ✅ |

> 条件 7 的 `evidence` 是**唯一无法由系统自证**的项（设计上的已知限制，Runbook §2.1 已声明）：
> 服务端无法证明设备此刻真在 `confirm_boot_seq`。本次以 **设备当前连续上报 `boot=21`** +
> **10-19 02:31 双擦记录**作为依据。

---

## 4. dry-run 实测（零写入）

**请求**（`POST /api/admin/device/288485896ce4/watermark/rebase`）：

```json
{
  "rebase_id": "rb-20261019-precheck-01",
  "reason": "full_erase_confirmed",
  "evidence": "precheck dry_run: device boot=21 seq=5169 13:00:41Z vs watermark max=117",
  "expected_epoch": 1,
  "expected_max_boot_seq": 117,
  "confirm_boot_seq": 21,
  "dry_run": true
}
```

**响应**（`HTTP 200`）：

```json
{
  "ok": true,
  "applied": false,
  "dry_run": true,
  "would_apply": true,
  "device_id": "288485896ce4",
  "rebase_id": "rb-20261019-precheck-01",
  "epoch_now": 1,
  "epoch_after": 2,
  "max_boot_seq_now": 117,
  "max_boot_seq_after": 21,
  "prev_epoch_max_after": 117,
  "observed_max_boot_seq": 117,
  "rewind_count_reset_from": 433,
  "ambiguous_count_reset_from": 0
}
```

**零写入验证**（调用后立即复查）：

| 字段 | 调用前 | 调用后 |
|---|---|---|
| `epoch` | 1 | **1** |
| `max_boot_seq` | 117 | **117** |
| `prev_epoch_max` | `NULL` | **`NULL`** |
| `rewind_count` | 433 | **433** |
| `last_rebase_id` | `NULL` | **`NULL`** |
| 审计表行数 | 0 | **0** |

⇒ dry-run 与文档一致：**使用同一套校验、零写入**。

---

## 5. 回退方案（Runbook §6 复核）

⚠️ **rebase 在语义上不可"撤销"**——它是"换代"而不是"改一个值"：

| 手段 | 可行性 | 代价 |
|---|---|---|
| 再跑一次 rebase 把 `max_boot_seq` 调回 117 | ❌ **不允许** | `confirm_boot_seq < max_boot_seq` 是硬条件；且 `confirm <= observed_max_boot_seq`，而 rebase 后 `observed` 仍为 117，但 `max` 已降到 21 ⇒ "上调"被结构性禁止 |
| 人工 SQL `UPDATE` 回 117 + 新的审计说明 | ⚠️ 唯一可行 | 需**另一次授权**；必须**新增**审计行说明原因，**禁止删除**原审计行（那是唯一留证） |
| 关闭水位功能 | ✅ | `[vars] LOG_BOOT_WATERMARK=0` + 重新 `deploy`（**需部署授权**；不删表、不改数据）。副作用：`REWIND_CANDIDATE` 检测整体停用 |
| 从 D1 备份恢复 | ⚠️ 重 | 会连带回退其它表，代价远大于收益；不建议为此单行使用 |

**回退前必须先留存的前置快照**（建议在正式执行前落档）：

```sql
SELECT * FROM log_boot_watermark WHERE device_id='288485896ce4';
-- 期望：epoch=1, max_boot_seq=117, observed=117, rewind_count=433
```

审计行本身即包含 `max_boot_seq_before` / `epoch_before` / `params_hash` / `actor` / `reason` / `evidence`，
**这行就是回退依据**，因此"审计插入失败 ⇒ 整体回滚"的设计必须依赖（已验证：`meta.changes` 1/1）。

---

## 6. 执行后的预期与盲区（必须同步告知）

1. `epoch 1→2`、`max_boot_seq 117→21`、`prev_epoch_max=117`、`rewind_count` 归零。
2. `[log-bootwm]` 不再对 `boot=21` 报 `REWIND_CANDIDATE`（这是本次要消除的噪声）。
3. **检测盲区**：`(21, 117]` 区间的真回退**不会被计数**，直到设备真实 `boot > 117`。
   期间回退判据只能靠**设备侧** `[Log] recovery=` 与 `boot_seq` 连续性（Runbook §4）。
4. 历史数据不受影响：`log_history` 中 `boot_seq` 111–117 的旧行**仍在**（rebase 只改判定水位，不删数据）。

---

## 7. 还差什么（非技术前提）

| # | 项 | 谁来做 | 说明 |
|---|---|---|---|
| A-1 | **执行授权** | 所有者 | Runbook §8：`dry_run → 正式` 是同一次授权的两个步骤还是一事一授，需明确 |
| A-2 | **`rebase_id` 定稿** | 所有者 | 本次预检用 `rb-20261019-precheck-01`；正式执行**建议换新 ID**（避免与预检混淆），例如 `rb-20261019-0001` |
| A-3 | **`evidence` 定稿** | 所有者 | 必须能指向设备侧证据（`RD-8` 擦除记录 / 串口 `recovery=PROVISION`） |
| A-4 | **回退授权**（如发生） | 所有者 | 人工 SQL 回退需单独批准；不接受"先跑再说" |
| A-5 | **执行窗口** | 所有者 | 建议在设备空闲、无人在场喂食时执行；操作本身**不触碰设备**（纯云端一行 UPDATE） |

> 本次核验**未**调用 `dry_run: false`，`max_boot_seq` 仍为 **117**。

---

## 8. 建议的执行序列（待授权）

```
1) 留存水位快照（SELECT *）           —— 只读
2) dry_run（新 rebase_id，同参数）     —— 零写入，确认 would_apply=true
3) 正式调用（同 rebase_id，dry_run:false）
4) 立即核验：水位行 4 字段 + 审计行 1 条（meta.changes 1/1）
5) 观察 10 分钟：[log-bootwm] 不再出现 REWIND_CANDIDATE
6) 若第 3 步返回 503 outcome_unknown ⇒ 用**同一** rebase_id 重试（不要换 ID）
```

**注意**：`rebase_id` 幂等是"同 ID 同参数回放"，**不是**"同 ID 可重复执行"。
一旦成功，再想改必须换新 ID + 新授权。
