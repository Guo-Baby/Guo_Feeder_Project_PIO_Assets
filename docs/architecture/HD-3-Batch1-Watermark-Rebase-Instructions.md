# 批次 1 实施指令：R-11 水位 rebase

> 交付对象：本地 AI Agent（单批可独立执行）
> 依据：`HD-3-Risk-Closure-Plan.md`（`rev.2`）§1 / §2
> 前置状态：云端 `main @ 362ddfd`；`0001…0005` 已应用；Worker `508461f3` 在跑
> **本指令不隐含任何生产授权**

---

## 1. 目标

在**不改动设备协议、不改 ACK、不碰 `log_history`** 的前提下，为 `log_boot_watermark` 增加
「世代（epoch）+ 分离观测与判定」模型与受控 rebase 管理接口，消除当前 **96.4% 的误报**，
同时**保留真回退的检出能力**，并对不可消除的歧义盲区做出显式声明。

**必须达成**

1. rebase 后，设备正常重启（`boot_seq` 停在基线或小幅波动）**不再累加** `rewind_count`；
2. 真回退（`boot < 当前判定基线`）**仍被计数**；
3. 歧义带观测**只进 `observed_max_boot_seq` / `ambiguous_count`**，不动判定基线、不计 rewind；
4. 每次 rebase 有**不可篡改的审计行**，旧基线/旧计数**永久留存**；
5. 并发、重放、CAS 失败、审计失败的行为**可验证**。

---

## 2. 实施范围（允许改动的文件）

| 类型 | 路径 | 说明 |
|---|---|---|
| 新增（草案 migration） | `migrations/drafts/0006_log_boot_watermark_epoch.sql` | **必须放 `drafts/`**（`apply` 不会拾取） |
| 新增（模块） | `src/log-boot-watermark-rebase.js` | 纯函数 + 条件批量构建 + 结果解析 |
| 修改 | `src/log-boot-watermark.js` | upsert 接入 `epoch/prev_epoch_max/observed_max/ambiguous_count`；分类改四态 |
| 修改 | `src/index.js` | 在既有 `/api/admin/` 分支内加 rebase 路由（复用 `handleAdmin`） |
| 新增（测试） | `test/current/log_boot_watermark_rebase.test.mjs` | ≥ 30 项 |
| 新增（夹具） | `test/real_d1/watermark_rebase_real_d1.sh` | 5 类场景（本地 workerd） |
| 修改 | `test/run_all.mjs` | 注册新套件（**只加一行**） |
| 新增（文档） | `docs/architecture/HD-3-Rebase-Runbook.md` | 运维 runbook + 盲区声明 + `0004/0005` 状态澄清（`D-1`） |

**不得改动**：`migrations/0001…0005`（含注释）、`log-ack.js` 的 ACK 逻辑、`history-ingest.js` 的响应体、
`wrangler.toml`、`prisma`/schema 之外任何文件、任何固件文件。

---

## 3. 要求

### 3.1 迁移草案（`0006`，**不得 apply**）

1. `ALTER TABLE log_boot_watermark ADD COLUMN`：`epoch`(NOT NULL DEFAULT 1)、`epoch_started_at`、
   `prev_epoch_max`(NULL)、`observed_max_boot_seq`(NOT NULL DEFAULT 0)、`ambiguous_count`(NOT NULL DEFAULT 0)、
   `last_rebase_id`(NULL)、`last_rebase_at`(NULL)；
2. 新建 `log_boot_watermark_audit`（`rebase_id` PK + §2.5 字段）+ `ix_lbwa_device_created`；
3. 对既有行做**一次性回填**：`observed_max_boot_seq = max_boot_seq`、`epoch = 1`、其余默认；
4. 全部 `IF NOT EXISTS` / 幂等；**不得** `DROP`、**不得** 改 `rewind_count` 历史值。

### 3.2 分类函数（纯逻辑，可单测）

`classify(prev, boot, seq)` → 四态之一：`new_epoch` / `ambiguous_forward` / `same` / `rewind_candidate`。

- `prev` 含：`max_boot_seq`、`prev_epoch_max`、`epoch`；
- 判定顺序**严格**为：
  1. `prev_epoch_max !== null && boot > prev_epoch_max` → `new_epoch`
  2. `prev_epoch_max !== null && boot > max_boot_seq && boot <= prev_epoch_max` → `ambiguous_forward`
  3. `boot === max_boot_seq` → `same`
  4. `boot < max_boot_seq` → `rewind_candidate`
  5. 其余（`prev_epoch_max === null && boot > max_boot_seq`）→ `new_epoch`（普通前进）
- `boot`/`seq` 解析必须用**严格整数**（复用 `toInt`，拒绝 `null`/`""`/`" 12 "`/`bool`）。

### 3.3 upsert 改造

- `max_boot_seq` 仍 `MAX()`（单调）；但**歧义带观测不得抬高它** ⇒ 用 SQL `CASE` 或先算分类再决定绑定值；
- 新增 `observed_max_boot_seq = MAX(observed_max_boot_seq, excluded.observed_max_boot_seq)`；
- `ambiguous_count` 累加；`epoch` 只在 `new_epoch` 且越过 `prev_epoch_max` 时处理（**ingest 不改 epoch**，epoch 只由 rebase 推进）；
- **ingest 路径绝不修改** `prev_epoch_max`、`last_rebase_id`。

### 3.4 管理接口

`POST /api/admin/device/:deviceId/watermark/rebase`

- 鉴权：复用 `authorizeAdmin`（Bearer + timing-safe）；**未配置令牌统一 401**；
- 校验：`deviceId` 用 `isValidDeviceId`；`rebase_id` 必须是非空字符串（建议 UUID）；
  `reason ∈ {full_erase_confirmed, factory_reset, field_replacement, other}`；`evidence` 非空（≤512 字）；
  `expected_epoch`、`expected_max_boot_seq`、`confirm_boot_seq` 必须是整数；
- `confirm_boot_seq` 必须 `<= max(observed_max_boot_seq, 请求值)` 且 `>= 0`；**不接受**大于观测值的"乐观"基线；
- `dry_run=true`：只读校验 + 返回 `would_apply`，**零写入**（不写 watermark、不写 audit）；
- 响应码：`200 applied` / `200 dry_run` / `200 replay`（幂等回放）/ `400 bad_request` / `401` / `409 cas_mismatch` / `500 audit_write_failed` / `503 outcome_unknown`。

### 3.5 CAS + 审计原子性（**硬要求**）

1. 使用**单次 `db.batch([updateStmt, auditStmt])`**（两条语句，顺序固定）；
2. `updateStmt` 的 `WHERE` 必须同时含 `device_id`、`epoch = expected_epoch`、`max_boot_seq = expected_max_boot_seq`，
   并在 `SET` 中写 `last_rebase_id = :rid`；
3. `auditStmt` 必须是 `INSERT ... SELECT ... FROM log_boot_watermark WHERE device_id=:dev AND last_rebase_id=:rid`
   （**条件插入**；未命中时插 0 行）；
4. **必须**读 `results[0].meta.changes` 与 `results[1].meta.changes`，并断言：
   - `changes[0] === 1 && changes[1] === 1` → `applied`
   - `changes[0] === 0 && changes[1] === 0` → `cas_mismatch`（409）
   - **其它组合** → `500 transaction_inconsistent`，并 `console.error` 记录两个 changes 值；
5. **不得**只依赖"batch 没抛错"；**不得**用 `INSERT OR IGNORE`（会吞 NOT NULL 违约）；
6. 请求开头先查 `audit WHERE rebase_id = ?`：存在则**零写入**回放；并发抢先导致 PK 冲突时，
   捕获异常 → 重查 audit → 回放；仍查不到 → `500`。

### 3.6 测试（**≥ 30 项**）

`test/current/log_boot_watermark_rebase.test.mjs` 至少覆盖：

| 组 | 项数 | 内容 |
|---|---|---|
| A 分类 | ≥10 | 四态边界；`prev_epoch_max=null` 时；`boot` 恰好等于 `max`/`prev_epoch_max`；`boot=0`；大整数 |
| B 严格解析 | ≥4 | `null` / `""` / `" 12 "` / `true` 不得被当作合法 `boot` |
| C CAS 语义 | ≥6 | 命中 / epoch 不匹配 / max 不匹配 / 双 0 / 异常组合 → 500 |
| D 审计 | ≥5 | 条件插入只在命中时 1 行；旧值取自行；`rewind_count_before` 留存；失败回滚 |
| E 幂等 / dry_run | ≥6 | 同 `rebase_id` 回放零写入；dry_run 零写入；参数不同 ⇒ 400 |
| F 鉴权 | ≥3 | 无 token / 错 token / 未配置 token ⇒ 401 |

**不得**减少既有 `log_boot_watermark.test.mjs`（48 项）的断言；如因签名变化需调整，**只能新增适配层**，断言数不得下降。

### 3.7 真实 D1 验证（本地 workerd，隔离目录，**不连生产**）

`test/real_d1/watermark_rebase_real_d1.sh`，5 类场景：

1. rebase 后 `boot=18` 连续 3 次重投 ⇒ `rewind_count` **不增**、`obs_count` 增；
2. rebase 后 `boot=12` ⇒ `rewind_count` **+1**（真回退仍可检出）；
3. rebase 后 `boot=100` ⇒ `ambiguous_count` +1、`max_boot_seq` **仍 18**、`observed_max_boot_seq=100`；
4. rebase 后 `boot=120` ⇒ `max_boot_seq=120`、`prev_epoch_max` 变 `NULL`、`rewind` 不增；
5. 同 `rebase_id` 重放 ⇒ 审计行数不变、`epoch` 不再 +1、返回 `replay`。

脚本要求：`--local --persist-to <独立 run 目录>`；**不得**出现 `--remote`；运行前打印目标库与隔离证据。

---

## 4. 约束

- 只动 §2 列出的文件；**不得**改固件、`log-ack.js` ACK 逻辑、HTTP 四字段契约、MQTT；
- `0006` **必须**留在 `migrations/drafts/`；**不得** `mv` 到 `migrations/`；**不得** apply（本地临时库除外）；
- **不得**部署 Worker、**不得**执行生产 rebase、**不得**写生产 D1；
- 既有 `status` 四值语义保持不变（`open/reviewed/benign/incident`）；`benign` **不作为**审计替代；
- 所有新增 SQL 必须参数化，禁止字符串拼接用户输入；
- 代码风格跟随现有文件（4 空格、`if(...)` 同行括号、中文注释）。

---

## 5. 验收标准

| # | 判据 |
|---|---|
| A1 | `node test/current/log_boot_watermark_rebase.test.mjs` → **≥30 PASS / 0 FAIL** |
| A2 | `node test/current/log_boot_watermark.test.mjs` → **仍 48 PASS / 0 FAIL** |
| A3 | `node test/run_all.mjs` → **全部 exit=0**（套件数 = 原 19 + 1） |
| A4 | `bash test/real_d1/watermark_rebase_real_d1.sh` → **5 场景全 PASS / 0 FAIL / EXIT=0** |
| A5 | `git status` 中**没有** `migrations/0006_*.sql`（只在 `drafts/`） |
| A6 | 手工 grep 证明：`migrations/0001…0005` 内容未变（`git diff --stat` 为空） |
| A7 | 报告明确写出 `0006 = NOT APPLIED`、`Worker = NOT DEPLOYED`、`生产 rebase = NOT RUN` |

---

## 6. 必交报告（逐项，不可合并）

1. **改动清单**：每个文件的 `+/-` 行数与作用；
2. **CAS 判定实现**：贴出读取 `meta.changes` 的代码段与三分支处理；
3. **分类函数真值表**：四态 × 边界（含 `prev_epoch_max=null`）；
4. **测试结果**：A1–A4 的命令、EXIT、PASS/FAIL 数；
5. **真实 D1 隔离证据**：目标库路径、`--local`、persist 目录、`git check-ignore` 结果；
6. **未执行项**：`0006` 未应用 / 未部署 / 未生产 rebase —— 逐条标 `NOT RUN`；
7. **盲区声明**：`(max, prev_epoch_max]` 内不计 rewind，写入 runbook；
8. **D-1 处理**：README/runbook 中如何澄清 `0004/0005` 已转正（附原文）。

---

## 7. 失败 / 停止条件

出现以下任一，**立即停止**并报告，不得"绕过去"：

1. 单 batch 无法保证"UPDATE 与审计同成同败"（例如 `meta.changes` 缺失或顺序不符）⇒ **停**，改用"两阶段 + 补偿表"方案并重新评审；
2. CAS 判定出现"1 命中 / 0 审计"或"0 命中 / 1 审计"⇒ **停**（说明底层语义与文档不符）；
3. 既有 48 项水位测试无法保持通过且无法用适配层解决 ⇒ **停**；
4. 需要改 `log_history`、ACK、协议才能继续 ⇒ **停**（超出授权）；
5. 真实 D1 夹具无法证明隔离（出现 `--remote`）⇒ **停**。

---

## 8. 允许的 Git 操作

- **允许**：`git status` / `git diff` / `git diff --cached` / `git log` / `git show`（只读）；
- **允许**：对本批新增/修改文件 `git add`（**仅在用户明确要求提交时**）；
- **禁止**：`commit`（未授权时）、`push`、`reset --hard`、`clean`、`stash`、切分支、`rebase`、`merge`、`cherry-pick`；
- **禁止**：`git add -A` / `git add .`；只能逐文件 add 并核对 `git diff --cached --name-only`。

---

## 9. 明确禁止事项

1. 部署 / 回滚 Worker；apply 任何 migration 到生产；写生产 D1；
2. 修改 `migrations/0001…0005`（含注释）；
3. 修改 `log-ack.js` 的 ACK 判定、`history-ingest.js` 的四字段响应体；
4. 修改固件、EMQX 规则、`wrangler.toml` 的 `[vars]`；
5. 用"降低 `max_boot_seq`"或"清零 `rewind_count`"来消除误报；
6. 把 `rewind_count` 表述为"独立事故数"；
7. 声称"rebase 后回退检测完全恢复"（盲区仍在）；
8. 为了测试通过而放宽/删除既有断言；
9. 提交其他任务的未提交修改。
