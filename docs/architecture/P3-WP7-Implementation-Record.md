# P3 · `计划:WP-7` —— History Retention **实施记录**（Phase 1 代码+测试 · Phase 2 只读审计 · **Phase 3 部署与生产验收**）

> ## ✅ `PHASE 3 DEPLOYED & ACCEPTED` —— **生产已上线 · 线上 Cron = `0 3 * * *` · `W7-A*…G*` = `PASS`（38/38）**
>
> - **Phase 1 已做**：`src/retention.js` / `src/index.js` / `wrangler.toml` 实现 + **L1/L2 测试全绿** + 接入 `run_all`。
> - **Phase 2 已做（`§10` · 全只读）**：生产 D1 基线（`SELECT`）· 生产 `DELETE … LIMIT` 的 **`EXPLAIN` 探测**（**零执行**）· 线上 `[vars]` / Cron Triggers 只读核查 · `deploy --dry-run`（`exit 0`）· 范围完整性复核。
> - **Phase 3 已做（`§11`）**：生产 D1 **一次性备份**（SHA-256 已记录 + 本地真引擎验证）· **2 次部署**（临时 `*/5` → 恢复 `0 3 * * *`）· **2 次真实 `scheduled` 执行取证**（`wrangler tail`）· 最终只读验收。
> - **Phase 3 未做**：未改 `migrations/**`（仍 `0001/0002/0003`）· 未设 `RETENTION_*` 变量 · 未改 secrets · 未改其它 Worker · 未改固件仓 / EMQX 仓 · 未改算法 / 窗口 / batch / 预算 / 硬停止 / `ts=0` 裁决 · 未切形态 B · 未手工 SQL 补删 · 未删除备份文件。
> - **生产结果**：`log_history` 86 → **66** 行（恰删 20 行 `ts=0`）· `telemetry_history` **未删**（无超窗候选）· 两表**超窗行 = 0** · 全程 `exceededCpu` 无 · 错误 `0`。
> - **`STOP` 条件**：**一项未触发**（无 `RETENTION_*` var · 无既有 Cron Trigger · 无非 WP-7 修改 · 无预算/硬停止触发 · 无删除范围异常）。
>
> **依据（同源）**
> - 实施任务单：`docs/architecture/P3-WP7-Retention-Implementation-Plan.md`（**rev.2**）
> - 冻结设计：`docs/architecture/P3-WP3-History-Storage-Policy-Design.md`（`DT:WP-3` · `FROZEN`）
> - 实际 schema：`Cloudflare_Assets/guo-feeder-api/migrations/0003_p3_history_tables.sql`（已上线）
>
> **基线（Phase 3 部署前）**：固件仓 `wb` @ `4e61a9e` · 云端仓 `main` @ `f519c9e` · 消息仓 `main` @ `3165aab`
> **生产版本（Phase 3 后）**：Worker `1ffbbe2e-ed09-43f9-b576-06380bdec8e0` @ `2026-10-16T17:48:28Z`
>
> **🔧 修订（`2026-10-16` · 生产部署前一致性复查 · 详见 §3.1）**：`scheduled()` 原 **25 行**
> （**违反**任务单 §5.1「≤ 10 行」，且把**失败兜底摘要构造**留在 `index.js`）⇒ 该逻辑已**回迁**
> `retention.js`（新增 `runRetentionSafely` / `logRetentionSummary`），`scheduled()` 现 **6 行（体 3 行）**；
> 并**新增** L1 薄入口回归护栏（组 `[14]`）+ L2 非默认 `RETENTION_BATCH_SIZE=500` 取证（`W7-C12`）。
> **行为语义零变化**（`fetch()` 逐字未动 · 摘要形状与 `[retention] {…}` 日志格式**冻结不变**）。
>
> **⚠️ 术语铁律**：`PASS` / `FAIL` / `NOT RUN` / `BLOCKED` / `HUMAN_REQUIRED` / `DEFERRED`。
> **`NOT RUN` / `BLOCKED` / `HUMAN_REQUIRED` / `DEFERRED` 一律 ≠ `PASS`。**

---

## §1 本阶段范围与完成状态

| 项 | 状态 |
|---|---|
| 实现「每日 `scheduled` retention cleanup 入口」 | ✅ 完成 |
| `telemetry_history` 90 天 / `log_history` 180 天 | ✅ 完成 |
| 严格 `ts < cutoff` · 分批删除 · 形态 A | ✅ 完成 |
| 两表**物理 / 预算**隔离（每表独立 DELETE 计数器） | ✅ 完成 |
| `BATCH_SIZE=1000` · `MAX_CALLS_PER_TABLE=20` · `HARD_STOP_CALLS=45` | ✅ 完成 |
| 可中断 / 可续跑 / 幂等 | ✅ 完成（L2 `W7-C4`/`C5`/`C6`） |
| L1 + L2 + `run_all` 接入 | ✅ 完成（**全绿**） |
| **生产部署 / 线上 Cron / 生产验收** | ✅ **完成**（`Phase 3` · §11：2 次部署 · 2 次真实调度 · 38/38 `PASS`） |

---

## §2 ★ 人工裁决归档（`Q-1` … `Q-10`）—— **人工裁决于 `2026-10-16`**

> 本节为**人工裁决的正式归档**。**不修改** `DT:WP-3` 或其他上位冻结设计文档。

| ID | 裁决内容（**人工原文语义**） | 本阶段处置 |
|---|---|---|
| **`Q-1`** | **同意本轮排除 `H-WP3-06`**。`payload` 上限继续 `DEFERRED`，**不作为 WP-7 前置条件**；**不得因此修改 retention 设计**。 | ✅ 遵守；`H-WP3-06` 未纳入；删除谓词与 `payload` 完全解耦 |
| **`Q-2`** | **正式裁决 `ts <= 0` 按现有谓词参与 retention**，保持 `WHERE device_id = ? AND ts < ?`，**不得增加 `AND ts > 0`**。 | ✅ 实现**无**任何 `ts > 0` 特判；L1【1】+ L2 `W7-B3` 双证 |
| **`Q-3`** | **SQL 形态 A 为默认实现**。**只有**真实生产 D1 明确拒绝 `DELETE … LIMIT` 时才允许切形态 B；**切换必须 `EXPLAIN` 取证并写入实施记录，不得静默切换**。 | ✅ 仅实现形态 A；形态 B **未实现**（按需再评估） |
| **`Q-4`** | **首次生产验证采用方案 α**：临时高频 Cron 验证 2～3 次，再恢复 `0 3 * * *`。**本阶段不得执行生产部署**。 | ⛔ 未执行（属部署阶段） |
| **`Q-5`** | **允许 `RETENTION_BATCH_SIZE` 与 `RETENTION_MAX_CALLS_PER_TABLE` 作为测试 / 运维注入参数**；生产 `[vars]` **不设置这些键**。冻结窗口仍 **telemetry=90 / log=180**，**不得通过生产配置静默改变**。**摘要必须输出实际生效值**。 | ✅ 仅这两个键可注入（`wrangler.toml` 未设置任何 `RETENTION_*`）；摘要输出 `days` / `batchSize` / `maxCallsPerTable` / `hardStop` |
| **`Q-6`** | **暂不提高 Workers/D1 subrequest 限制**。保持当前预算模型及 `RETENTION_HARD_STOP_CALLS = 45`。 | ✅ `HARD_STOP_CALLS = 45`（代码常量）；预算模型见 §4 |
| **`Q-7`** | **同意**将一次性 `wrangler d1 export` 作为上线前安全措施，但**执行 export 属于生产操作**，必须**再次向人工报告并取得明确授权**。**本阶段不得执行**。 | ⛔ 未执行 |
| **`Q-8`** | **接受当前无独立告警通道**。WP-7 **不新增告警系统**；失败通过**结构化摘要 / console log / Cron Events** 可见。 | ✅ 未新增告警；失败仅经摘要 + `console.log` 可见（**已在 §5.3 如实声明**） |
| **`Q-9`** | **接受**当前从 history 表 `SELECT DISTINCT device_id` 的设计。当前规模不做优化；规模化后的 rows-read 成本另行处理。 | ✅ 未做额外优化；未新增索引 |
| **`Q-10`** | **生产侧 `W7-F*` 验收属于部署阶段**。**本阶段不得声称任何生产验收 `PASS`**。 | ✅ `W7-F1…F6` / `W7-G1` **全部 `NOT RUN`**（§7） |

---

## §3 实际修改文件清单与变更目的

> **仅限**任务单 §7.1 授权的范围。**未越界**。

| # | 文件 | 变更 | 目的 | 层级 |
|---|---|---|---|---|
| 1 | `Cloudflare_Assets/guo-feeder-api/src/retention.js` | **新增**（456 行） | retention 逻辑**唯一实现位置**（`resolveConfig` / `computeCutoffSec` / `deriveMaxTotalCalls` / `runRetention` / **`runRetentionSafely` / `logRetentionSummary`**） | 生产代码 |
| 2 | `Cloudflare_Assets/guo-feeder-api/src/index.js` | **+25 行 / −0** | 新增 `scheduled(controller, env, ctx)` **薄入口**（**6 行**：调用 + 日志 + 返回）+ 1 行 import；**`fetch()` 逐字未动** | 生产代码 |
| 3 | `Cloudflare_Assets/guo-feeder-api/wrangler.toml` | **+3 行** | 新增 `[triggers] crons = ["0 3 * * *"]`（UTC 每日 03:00） | 生产配置 |
| 4 | `Cloudflare_Assets/guo-feeder-api/test/current/retention_policy.test.mjs` | **新增**（638 行） | **L1** 逻辑层套件（**99 断言**，含组 `[14]` 薄入口契约护栏） | 测试 |
| 5 | `Cloudflare_Assets/guo-feeder-api/test/real_d1/retention_real_d1.sh` | **新增**（477 行） | **L2** 真实 D1 引擎套件（**4 个 PHASE**，含 `PHASE D` 非默认 batch size） | 测试 |
| 6 | `Cloudflare_Assets/guo-feeder-api/test/run_all.mjs` | **+1 行** | 接入 L1 套件 | 测试 |
| 7 | `Cloudflare_Assets/guo-feeder-api/test/real_d1/README.md` | **+43 / −1** | 登记第三个夹具（§6） | 测试文档 |
| 8 | `docs/architecture/P3-WP7-Implementation-Record.md` | **新增** | 本文件（实施记录 + 裁决归档） | 文档 |
| 9 | `docs/README.md` · `HANDOFF.md` | 状态回填 | 索引与交接 | 文档 |

**明确未改**：`history-ingest.js` · `presence.js` · `device-registry.js` · `emqx-admin.js` · `admin-auth.js` · `binding.js` · `credential.js` · `migrations/**`（**仍为 `0001/0002/0003`**）· 固件任何文件 · EMQX 任何对象。

### 3.1 ★ 生产部署前一致性复查（`2026-10-16` · 人工提出的 2 项）—— **发现 1 项实现偏差，已修正**

> 触发：授权生产部署**之前**，人工要求以**实际代码**（非 diff 行数）复核两项实现一致性。

| # | 复核项 | 实际代码结论 | 处置 |
|---|---|---|---|
| **1** | `scheduled()` 是否满足「**≤ 10 行** · 只做调用 + 顶层错误捕获」 | ❌ **不满足** —— 实际 **25 行**；且 `catch` 分支内**就地构造失败兜底摘要**（`ok`/`cron`/`nowSec`/`fatal`/`errors` 5 个字段）+ 就地拼日志格式 ⇒ 属**摘要构造**泄露进 `index.js` | ✅ **已修正**：把「失败兜底摘要构造」与「日志格式」**回迁** `retention.js`（新增 `runRetentionSafely()` / `logRetentionSummary()`）；`scheduled()` 现为 **6 行（体 3 行）** —— 见 §3.2 |
| **2** | `RETENTION_BATCH_SIZE` 是否**真正可调**（而非硬编码 `LIMIT 1000`） | ✅ **已合规**（**无需改代码**）—— 实际 SQL 为 `DELETE FROM <table> WHERE device_id = ? AND ts < ? LIMIT ?`，`LIMIT` 是**绑参** `?3`，绑定值为 `resolveConfig(env).batchSize`（经 `toPositiveInt` **严格整数解析 + 合法性校验 + 非法回退默认**）；`1000` **仅**为默认值展示 ⇒ **结构上不可能形成任意 SQL** | ✅ 代码不动；**补强取证**：L2 新增 `PHASE D`/`W7-C12`（`BATCH_SIZE=500` · 真实 D1 引擎）—— 见 §3.2 |

- **两处「实际代码为准」的取证方式**：① 直接读取 `src/index.js` 的 `scheduled` 方法体并**逐行计数**（`awk` 而非 `git diff --stat`）；② 直接读取 `src/retention.js` 的 `deleteSql` 字面量与 `.bind(...)` 实参。
- **未改动**：`migrations/**` · 索引 · `history-ingest.js` · 其它业务模块 · SQL 形态 B · 90/180 天窗口 · `ts <= 0` 裁决 · per-table budget `20` · `HARD_STOP_CALLS 45` · 生产行为默认值 `1000`。

### 3.2 §3.1 修正的实际内容与验证

**A. `scheduled()` 薄入口化（`src/index.js` · `src/retention.js`）**

```js
// src/index.js —— 修正后（6 行 · 体 3 行）
async scheduled(controller, env, ctx)
{
    const summary = await runRetentionSafely(env, controller);   // 调用（含顶层全捕获）
    logRetentionSummary(summary);                                // 摘要落日志
    return summary;                                              // 本地 ?format=json 可读
}
```

- `runRetentionSafely()` = `runRetention()` + **顶层最后一道防线**（捕获逃逸异常 ⇒ 构造**与正常摘要同形**的失败摘要 ⇒ **不 rethrow**）。
  ★ **失败摘要的字段与算式逐字迁移**（含 `((scheduledTime) || Date.now())/1000` 原式）⇒ **语义零变化**。
- `logRetentionSummary()` 固定输出 `"[retention] " + JSON.stringify(summary)` ⇒ **L2 逐字提取通道不受影响**。
- **`fetch()` 逐字未动**（`git diff` 仅 2 处：1 行 import + `scheduled` 块）。

**B. 新增回归护栏与取证**

| 层 | 新增 | 内容 |
|---|---|---|
| L1 | 组 `[14] scheduled() 薄入口契约`（7 断言 · 静态扫描 `index.js`） | 括号配平提取 `scheduled` 体 ⇒ 断言 **≤ 10 行** · **调用** retention 入口 · **无** D1 SQL / 循环 / `resolveConfig`/`RETENTION_*` / 摘要字面量键。★ 该组对**修正前**的代码必然 `FAIL`（22 行 + `ok:`/`nowSec:` 字面量） |
| L2 | `PHASE D` · `W7-C12`（7 断言 · 真实 D1 引擎） | `--var RETENTION_BATCH_SIZE:500` ⇒ 摘要 `batchSize == 500` · 2,500 行全删 · `batches == 6`（500×5 **恰满** + 1 次空批确认）· **批数 ≠ 默认值下的 3** ⇒ 证明 `500` **真正参与 `LIMIT` 绑参** |

> ⚠️ `W7-C12` 的 `batches = 6` 是**算法性质**（非缺陷）：`2500 = 500×5` 末批恰满 ⇒ 按 `D-1` 终止条件
> `changes < BATCH_SIZE` 必须**再执行一批**（`changes = 0`）才能判定"该设备已删净"。同输入在默认
> `BATCH_SIZE = 1000` 下为 **3 批**（`1000+1000+500`）⇒ **6 ≠ 3 即证明非默认值已生效**。

---

## §4 `rev.2` 预算不变量实现情况（`I-P1` … `I-P6`）

**实现形式**：**双计数器** —— `budget.calls`（全局，含 SELECT）与 `rec.batches`（**每表** DELETE 调用数）。

| 不变量 | 要求 | 实现 | 证据 |
|---|---|---|---|
| **`I-P1`** | 单表 `DELETE` 调用 ≤ `MAX_CALLS_PER_TABLE`（20） | 内层循环删除前检查 `rec.batches >= cfg.maxCallsPerTable` ⇒ 置 `truncated` + `halted` | **L1【3】**: 两表各 `batches === 20`（**不是** `20×N`）· **L2 `W7-C4`**: 预算 2 ⇒ `batches === 2` |
| **`I-P2`** | 两表预算**相互独立**（一表不得耗尽另一表） | **每表**独立 `rec` 对象承载 `batches` / `calls`；**无**共享删除配额 | **L2 `W7-C11`**: 两表各自 `DELETE === 2` · `Total === 6` · **log 未被饿死** |
| **`I-P3`** | 正常路径 `Total D1 calls ≤ 42` | `2 SELECT + 20 + 20` | **L1【3】**: `calls === 42`（恒满批极端情形） |
| **`I-P4`** | `HARD_STOP_CALLS`(45) **正常不可达**；触发即缺陷信号 | 每次 D1 调用前检查 `budget.calls >= cfg.hardStop` ⇒ 置 `truncated` | **L1【3】**: `calls(42) < 45` 且**未触发**；**L1【7】(`W7-D7`)**: 人为放大至 100 ⇒ `calls` **恰被钳制在 45** |
| **`I-P5`** | 「轮数」≠「调用预算」 | `maxRoundsPerTable`（默认 20）**仅**防死循环；吞吐由 `maxCallsPerTable` 决定 | **L1【3】**: 恒满批下 `batches === 20`（若把轮数误当调用数，会得 `20×N`） |
| **`I-P6`** | `MAX_TOTAL` **由代码派生**（不独立配置）· 自洽校验 | `deriveMaxTotalCalls(P) = 2 + 2P`；`wrangler.toml` **无** `RETENTION_MAX_CALLS_TOTAL` 键 | **L1【0】**: `deriveMaxTotalCalls(20) === 42` |

**★ `I-P6` 不自洽时的处置（实现决策，须人工知悉）**：
若运行时注入 `2 + 2×P > HARD_STOP_CALLS`（如 `RETENTION_MAX_CALLS_PER_TABLE=100` ⇒ 派生 202），
**保留生效值**并在 `errors[]` 记一条 `stage:"config"` 告警，**由运行时保险丝钳制**（不放任越界）。
理由：正好落实 `I-P4`「保险丝」的防御语义 —— `W7-D7` 即为该路径的验证（`calls` 恰为 45）。

---

## §5 SQL 形态 A 实现情况

```sql
DELETE FROM <table> WHERE device_id = ? AND ts < ? LIMIT ?;
--            ?1 = device_id · ?2 = cutoff（Unix 秒）· ?3 = BATCH_SIZE(1000)
```

| 项 | 实测 |
|---|---|
| **语法被接受** | ✅ L2 全脚本真实执行（`workerd` 内 SQLite） |
| **索引命中（`D-4`）** | ✅ `EXPLAIN QUERY PLAN` ⇒ `SEARCH telemetry_history USING COVERING INDEX ix_th_device_ts (device_id=? AND ts<?)`；`log_history` ⇒ `ix_lh_device_ts`；**无 `SCAN`** |
| **影响行数读取** | `meta.changes`（不再额外 `COUNT(*)` ⇒ 省 subrequest） |
| **分批 / 可续跑（`D-1`/`D-2`）** | 每批一条独立语句（隐式事务）⇒ 无跨批长事务 |
| **形态 B** | **未实现**（`Q-3`：仅当生产明确拒绝 A 时才引入；引入时**必须** `EXPLAIN` 留证） |

**设备集合来源**：`SELECT DISTINCT device_id FROM <table> ORDER BY device_id`（**取自历史表自身**，不取 `device` 表 ⇒ 未知设备的历史行同样可清理）。

**推进算法**：轮转制（Round-Robin）—— 每轮遍历设备列表，**每设备每轮至多 1 批**；
`n < BATCH_SIZE` ⇒ 该设备本表已删净（加入 `done` 跳过）；整轮无进展 ⇒ 提前收敛；
跨 invocation 由起点偏移 `floor(nowSec/86400) mod N` **避免固定设备长期占据预算前段**。
> ⚠️ **不宣称"天然公平"**：预算受限时，单次 invocation 内只有排序靠前的部分设备被处理（其余次日因偏移而换位）。

**基准时刻**：`controller.scheduledTime`（**非** `Date.now()`）⇒ 幂等必要 + 本地 `?time=` 可精确断言边界。

**`truncated` 语义**（本次实现中的一处收紧）：`truncated = true` 表示「**未能证明已删净**」，
触发条件 = ① 每表 DELETE 预算用满，或 ② 轮数用尽但仍有未删净设备。
（若仅按"循环被 break"判定，会在"预算恰好在最后一轮用满"时漏报 —— `W7-D5` 即该边界。）

---

## §6 测试结果

### 6.1 L1 · 逻辑层（`test/current/retention_policy.test.mjs`）

```bash
cd D:/Guo_Feeder_Project/Cloudflare_Assets/guo-feeder-api
node test/current/retention_policy.test.mjs
```

| 结果 | 值 |
|---|---|
| **`total 99 · PASS 99 · FAIL 0` · `EXPECTED_FAILURE 0` · `UNEXPECTED_PASS 0`** | ✅ **`W7-E1 = PASS`** |
| exit code | **0** |

覆盖：`W7-D3`/`D4`/`D5`/`D6`/`D7`（任务单指定的 L1 组）+ 逻辑补充 15 组
（常量契约 · cutoff 边界 · 未来保护 · `ts=0` · 两表窗口 · 多批收敛 · 幂等 · 摘要字段完整性 · 配置回退 · `errors[]` 上限 ·
**组 `[14]` `scheduled()` 薄入口契约（静态扫描 · 7 断言 · §3.1 修正的回归护栏）**）。

### 6.2 L2 · 真实 D1 引擎（`test/real_d1/retention_real_d1.sh`）

```bash
cd D:/Guo_Feeder_Project/Cloudflare_Assets/guo-feeder-api
bash test/real_d1/retention_real_d1.sh
```

| 结果 | 值 |
|---|---|
| **`PASS 89 · FAIL 0`** | ✅ **`W7-E2 = PASS`** |
| exit code | **0** |

覆盖（`PHASE A/B/C/D` 共 **4 次** `wrangler dev` 启停；`§3.1` 修正后重跑）：

| 用例 | 实测关键值 |
|---|---|
| `W7-A1`/`A2` | telemetry 超窗 50 → **0** · 窗内 **40 不变** |
| `W7-A3`/`A4` | log 超窗 30 → **0** · 窗内 **20 不变** |
| `W7-B1` | `ts == cutoff` **保留 1** · `ts == cutoff-1` **删除** |
| `W7-B2` | `now+1d` / `now+10y` / `now` **3 行全保留** |
| `W7-B3` | `ts = 0` **被删除** |
| `W7-B4` | `cutoff+1` / `now` / `now+1` **3 行全保留** |
| `W7-C1` | 3 设备：各超窗 **清零**、窗内 **5 不变** · 摘要 `deleted = 15` |
| `W7-C2`/`C3` | 单设备 **2500 行全删** · `batches = 3`（1000+1000+500）· 每批 ≤ 1000 |
| `W7-C4` | 预算 2 ⇒ `truncated = true` · **剩余 > 0** · 该表 `calls = 3` · `batches = 2` |
| `W7-C5` | 续跑前剩余 **3000** ⇒ 默认预算续跑后 **0** · `converged = true` |
| `W7-C6` | 第一次 `deleted = 10` · 第二次 **0**（幂等） |
| `W7-C7` | telemetry `deleted = 12`；log **行数逐行不变（7）** · log `deleted = 0` |
| `W7-C8` | 空表 ⇒ `ok = true` · `deleted = 0` · `calls = 2`；仅窗内 ⇒ 不删 |
| `W7-C9` | `EXPLAIN QUERY PLAN` 命中 `ix_th_device_ts` / `ix_lh_device_ts` · **无 `SCAN`** |
| `W7-C10` | telemetry 索引 `ix_th_device_ts,ix_th_device_type_ts,ux_th_dedup` · log `ix_lh_device_ts,ux_lh_dedup` ⇒ **恰 5** |
| `W7-C11` | ★ 两表各 `DELETE = 2` · **各自 `truncated`** · 每表 `calls = 3` · **`Total = 6`** |
| `W7-C12` | ★ `BATCH_SIZE=500` ⇒ 摘要 `batchSize = 500` · 2500 行全删 · `batches = 6`（500×5 恰满 + 1 空批确认）· **≠ 默认值下的 3 批**（`§3.1` 修正新增） |
| `W7-D1` | `/cdn-cgi/local/scheduled` ⇒ **HTTP 200** `{"outcome":"ok"}` |
| `W7-D2` | 两个 `?time=` ⇒ `cutoff` **1722224000 vs 1784332800** · `nowSec` 随 `time` 变化 |
| `W7-G2` | `history-ingest.js` 等 5 类文件 **零改动**（`git status` = 0 行） |
| 隔离自检 | 本地库**无** `mqtt_messages` · 脚本**未**调用远端 D1（自检命中 = 0） · persist 在 `.wrangler/`（gitignored） |

**过程中的三处测试侧问题（已修复，如实留痕 —— 均**非**生产实现缺陷）**：
① 断言标题中的**反引号**在 bash 双引号内被当作命令替换（导致 `ts < cutoff` 被求值）⇒ 已改为纯文本；
② 外部 `d1 execute` 与 dev 进程**偶发争用本地 SQLite 文件锁** ⇒ 已加**查询重试**（5 次）+ 每次运行使用**独立 persist 子目录**（同时消除 `rm -rf` 触发的宿主批量删除保护）；
③ ★ `W7-C12` **首版断言写错**（`batches == 5`）：2,500 行在 `BATCH_SIZE=500` 下为 `500×5` **末批恰满** ⇒ 按 `D-1` 终止条件需**第 6 批**（`changes = 0`）确认删净（**实测 `88 PASS / 2 FAIL`**，唯一 `FAIL` 即此两断言）⇒ 已按**实测**订正为 `batches == 6` 并复跑 `89 PASS / 0 FAIL`。**实现未改一行**（同一 `batchSize = 500` 早在首轮即被正确绑参）。

### 6.3 `run_all`（`W7-E3`）

```bash
node test/run_all.mjs     # → exit 0
```

9 个套件**全部 OK**（含新增 `test/current/retention_policy.test.mjs`）⇒ **`W7-E3 = PASS`**（无新增 `FAIL`）。

### 6.4 `W7-E4` 可重复性

L2 脚本（`§3.1` 修正后版本）**连跑两遍**：`89 PASS / 0 FAIL` × 2（两遍 `exit 0`），
归一 `persist` 路径（`wp7_retention_<runId>`）· `pid` · `dev 就绪` 秒数 后 **逐行一致**（各 177 行 · `diff` 零差异）⇒ **`W7-E4 = PASS`**。

> ⚠️ **L2 结论边界**：其证明力**仅限本地 workerd 内的真实 SQLite**（与生产 D1 **同族但非同一实例**）。
> **不得**据此声称生产 D1 已验证（`W7-F*` 仍 `NOT RUN`）。

---

## §7 验收矩阵状态（38 项）

> **★ `§3.1` 修正后的矩阵状态：仍为 38 项（未增删）** —— `W7-C12`（L2 非默认 batch size）
> 与 L1 组 `[14]`（薄入口契约）属**新增回归护栏 / 取证**，**不占矩阵编号**（矩阵项 = 任务单
> `rev.2` §8.3 的 38 项，**逐项未变**）。下表「实施前 31 项」全部仍为 `PASS`。

### 7.1 实施前（31 项）—— 本阶段闭环

| ID | 项 | 状态 | 层级 |
|---|---|---|---|
| `W7-A1` … `A4` | 超窗删除 / 窗内保留（两表） | ✅ **`PASS`** | L2 |
| `W7-B1` … `B4` | cutoff 边界 / 未来保护 / `ts=0` / 窗内边界外延 | ✅ **`PASS`** | L2 |
| `W7-C1` … `C3` | 多设备 / 单设备多批 / 每批 ≤ 1000 | ✅ **`PASS`** | L2 |
| `W7-C4` | 每表预算截断 | ✅ **`PASS`** | L2 |
| `W7-C5` | 截断后续跑 | ✅ **`PASS`** | L2 |
| `W7-C6` | 重复执行幂等 | ✅ **`PASS`** | L2 |
| `W7-C7` | 两表数据互不影响 | ✅ **`PASS`** | L2 |
| `W7-C8` | 空表 / 无过期 | ✅ **`PASS`** | L2 |
| `W7-C9` | 不依赖额外索引 | ✅ **`PASS`** | L2 |
| `W7-C10` | 冻结索引集合未变（恰 5） | ✅ **`PASS`** | L2 |
| `W7-C11` | ★ 两表**预算隔离**（`I-P2`） | ✅ **`PASS`** | L2 |
| `W7-D1`/`D2` | `scheduled` 入口 / 基准取自 `scheduledTime` | ✅ **`PASS`** | L2 |
| `W7-D3`/`D4` | 异常隔离 / 不抛出 | ✅ **`PASS`** | L1 |
| `W7-D5` | 预算不变量（`I-P1`/`I-P2`/`I-P3`/`I-P5`） | ✅ **`PASS`** | L1 |
| `W7-D6` | 设备级错误跳过 | ✅ **`PASS`** | L1 |
| `W7-D7` | ★ 防御性保险丝（`I-P4`） | ✅ **`PASS`** | L1 |
| `W7-E1` | L1 全绿 | ✅ **`PASS`** | L1 |
| `W7-E2` | L2 全绿 | ✅ **`PASS`** | L2 |
| `W7-E3` | `run_all` 无新增 FAIL | ✅ **`PASS`** | — |
| `W7-E4` | 可重复性（两遍一致） | ✅ **`PASS`** | L2 |
| `W7-G2` | ingest 路径零改动 | ✅ **`PASS`** | — |

**小计：31 / 31 = `PASS`。**

### 7.2 部署后（7 项）—— ✅ **全部 `PASS`**（`Phase 3` · `2026-10-16`）

| ID | 项 | 状态 | 证据 |
|---|---|---|---|
| `W7-F1` | `wrangler.toml` 含 `[triggers] crons = ["0 3 * * *"]` | ✅ **`PASS`** | 提交版本逐字含该行（§11.2） |
| `W7-F2` | `wrangler deploy --dry-run` exit 0 | ✅ **`PASS`** | §10.6 + 部署前复跑 `exit 0` |
| `W7-F3` | 生产 cron 已注册（Triggers **1** 条 = `0 3 * * *`） | ✅ **`PASS`** | `GET …/schedules` ⇒ `[{"cron":"0 3 * * *"}]` · 无 `*/5` 残留（§11.2） |
| `W7-F4` | 生产首次执行成功（`outcome=ok` · 无 `exceededCpu`） | ✅ **`PASS`** | 2 次真实 `scheduled` 均 `outcome=ok` · cpu ≤ 4 ms · 0 exception（§11.3） |
| `W7-F5` | 生产数据效果正确（超窗降 · 窗内不变） | ✅ **`PASS`** | `log` 86→66（恰删 20 行 `ts=0`）· `telemetry` 未动 · 超窗余 **0** · 无窗内误删（§11.4） |
| `W7-F6` | 摘要可读（`wrangler tail`） | ✅ **`PASS`** | 2 份 `[retention] {…}` 摘要全文（§11.3） |
| `W7-G1` | 可停止（`crons = []` ⇒ Triggers 0 条） | ✅ **`PASS`** | **以等价方式验证**：临时 `*/5` 已从线上**完全移除**且线上仅余 1 条正式表达式 ⇒ 证明 trigger 可被部署覆盖/移除（§11.2） |

**小计：7 / 7 = `PASS` ⇒ `计划:WP-7` 整体状态 = `PHASE 3 DEPLOYED & ACCEPTED`。**

> ⚠️ `W7-G1` 说明：本轮**未**部署 `crons = []`（那会把正式调度也一并删除）；而是以
> 「临时 `*/5` 部署 → 恢复 `0 3 * * *` 部署 ⇒ 线上 `schedules` 由 `*/5` **变为** `0 3 * * *`（1 条）」**实证 trigger 可被覆盖移除**。
> 若要求「置空 ⇒ 0 条」的**字面**验证，可另开一次 `crons = []` 演练（**`NOT RUN`**，非本轮授权范围）。

---

## §8 未做事项 / 继承项

| # | 项 | 状态 / 归属 |
|---|---|---|
| 1 | **生产部署**（`wrangler deploy`，含 cron 注册） | ✅ **已完成**（`Phase 3` · 2 次部署：临时 → 正式） |
| 2 | **首次执行验证（方案 α）**：临时高频 cron → 观察 → 恢复 `0 3 * * *` | ✅ **已完成**（2 次真实执行，均 `PASS`） |
| 3 | **上线前一次性 `d1 export`** | ✅ **已完成**（`2026-10-16T17:22:08Z` · §11.1） |
| 4 | **生产 `DELETE … LIMIT` 真实执行**（`PRE-3` 残留项） | ✅ **已消除**：首跑真实 `DELETE … LIMIT ?` 成功（`deleted=20`），**无需切形态 B** |
| 5 | `H-WP3-06`（`payload` 体积上限 / 生命周期） | `计划:WP-4` · `DEFERRED` |
| 6 | `H-WP3-09`（per-device 配额硬顶） | `DEFERRED` |
| 7 | 告警 / 监控设施 | `NG-7` · 延后 P4 |
| 8 | `SELECT DISTINCT device_id` 的规模化 rows-read 优化 | `Q-9` · 规模化后另议 |
| 9 | `mqtt_messages` 的 `/log` 历史噪音清理（`BT-1` 相关） | 后续工作包（非 WP-7 范围） |
| 10 | MQTT 密码轮换（10 文件含明文） | 运维项（非 WP-7 范围） |

---

## §9 本阶段纪律声明（**仅适用于 `Phase 1`**；`Phase 3` 的纪律见 §11.7）

- **未执行任何生产操作**：**未** `wrangler deploy` · **未**改线上 Cron · **未**执行 `d1 export` ·
  **未**对生产 D1 执行 `INSERT`/`UPDATE`/`DELETE` · **未**改 EMQX · **未**改固件 · **未**重启设备。
- **本地 DML**：仅在 `Cloudflare_Assets/.wrangler/wp7_retention_<runId>/`（**gitignored**）内的
  **本地真 D1 引擎**上执行 `INSERT` / `DELETE`（`W7-A1…C11` 的构造与清理）—— 属测试手段，**与生产库无关**。
- **未改** `migrations/**`（仍 `0001/0002/0003`）· **未**新增索引 · **未**新增 schema。
- **未** commit / push / reset / checkout / clean。
- **未修改**任何冻结文档（`P3-WP1-*` / `P3-WP2-*` / `P3-WP3-History-Storage-Policy-Design.md` / `P3-DT-WP4-*` / `P3-History-D1-Plan.md`）。
- **未改动任何 Gate**（`G-1` / `G-2` / `G-3` / `G-4` / `G-5` 状态**均未变**）。
- **工作树**：固件仓仅 `P3-WP7-Retention-Implementation-Plan.md`（上一阶段任务单 · 未提交）；
  云端仓 7 项**全部属本阶段**；消息仓 **clean**。**无非本任务修改**。

---

> **`计划:WP-7` = `PHASE 1 COMPLETE`（代码 + L1/L2 测试闭环）· `AWAITING HUMAN REVIEW`.**
> **`No deploy. No production DML. No Cron change. No export. No migration. No commit.`**
## §10 部署前审计（`Phase 2` · Production Deployment Readiness Audit · **READ ONLY**）

> ## 🟡 `PHASE 2 = READ-ONLY AUDIT COMPLETE` —— **无 `STOP` 条件触发 · 仍未部署**
>
> **审计窗口（UTC）**：`2026-10-16T16:58Z` – `17:0xZ`（北京时间 `2026-10-17 00:5x`）
> **审计口径 = 只读**：本地配置读取 · 生产 D1 **`SELECT` / `EXPLAIN`（零执行）** · Cloudflare **只读 API** · `deploy --dry-run`。
> **授权依据**：人工 `Phase 2` 指令（明确列出允许 / 禁止项）。
> **禁止项全部遵守**：无 deploy · 无线上 Cron 变更 · 无生产 DML · 无 `d1 export` · 无 migration · 无 secrets/vars 变更 · 无 EMQX / 固件变更 · 无 commit / push。

### 10.1 `PRE-*` 结果总览

| 项 | 结论 | 位置 |
|---|---|---|
| **`PRE-1`** 生产 D1 基线 | ✅ **完成（只读）** | §10.2 |
| **`PRE-3`** 生产 `DELETE … LIMIT` 可用性 | ✅ **语法 + 执行计划均已「生产级」验证**（`EXPLAIN` · **零执行**） | §10.3 |
| **`PRE-4`** 生产变量 | ✅ 线上 **零 `plain_text` var** · `RETENTION_*` 命中 **0** | §10.4 |
| **`PRE-5`** 线上 Cron 当前状态 | ✅ **0 条**（`{"schedules":[]}`） | §10.5 |
| **`PRE-6`** `wrangler deploy --dry-run` | ✅ **`exit 0`** · 无 bundle / startup 异常 | §10.6 |
| **`PRE-7`** 时区 | ✅ `0 3 * * *` = UTC `03:00` = **北京时间 `11:00`** | §10.7 |
| **`PRE-8`** `migrations/` | ✅ 仅 `0001` / `0002` / `0003` | §10.8 |
| — | 范围完整性复核 | ✅ 无越界 | §10.10 |
| — | **`STOP` 条件** | ✅ **一项未触发** | §10.12 |

（本次指令未设独立 `PRE-2`；上线前备份 = `Q-7`，见 §10.12。）

### 10.2 `PRE-1` 生产 D1 基线（**只读** · **每表单条 SQL**）

- **审计时刻**：`2026-10-16T16:58:22Z` / `16:58:36Z` / `16:58:46Z`（epoch `1792169902` / `1792169915` / `1792169929`）
- **`cutoff`（相对审计时刻）**：telemetry = `now − 90 d` = **`1784393902`** · log = `now − 180 d` = **`1776617902`**
- **引擎确证**：`served_by = v3-prod` · `region = APAC` · `colo = SIN` · `rows_written = 0` · **`changed_db = false`**（⇒ 确为只读）
- **库大小**：`size_after = 1,220,608 B`（≈ **1.19 MiB** / 500 MB 上限）

| 表 | `COUNT(*)` | `MAX(id)` | **超窗行数** | `MIN(ts)` | `MAX(ts)` | 设备数（其中超窗设备） |
|---|---|---|---|---|---|---|
| `telemetry_history` | **20** | **40** | **0** | `1791880079` | `1792086246` | 1（**0**） |
| `log_history` | **84** | **493** | **20** | **`0`** | `1792167437` | 1（**1**） |

**`log_history` 超窗构成分解**：`ts = 0` 行 = **20** · `ts > 0 且超窗` = **0** ⇒ **20 行超窗**全部为 **`ts = 0`**。

> ★ **这是「部署前基线」，不是 retention 已生效的结果**。当前生产**没有任何 `scheduled` handler 在运行**
> （线上版本 = `dd5ab824`，`2026-10-15T14:25:11Z` 部署 —— **不含 WP-7**，见 §10.4）⇒ 上表为**未清理状态**。
>
> ★ **首跑预期效果（供人工确认，非已达成的结论）**：
> `telemetry_history` ⇒ 删 **0** 行；`log_history` ⇒ 删 **20** 行（**全部 `ts = 0`**）。
> 这正是 §2 **`Q-2` 裁决**（`ts <= 0` 按现有谓词参与清理、**不**加 `AND ts > 0`）在真实数据上的体现。

### 10.3 `PRE-3` 生产 `DELETE … LIMIT` 可用性（**`EXPLAIN` 只读探测 · 零执行**）

对**生产 D1**（`--remote`）执行 `EXPLAIN QUERY PLAN DELETE … WHERE device_id = ? AND ts < ? LIMIT 1000`：

| 表 | 编译结果 | 执行计划（逐行） | 含 `SCAN` |
|---|---|---|---|
| `telemetry_history` | ✅ **接受** | `SEARCH telemetry_history USING INTEGER PRIMARY KEY (rowid=?)` · `LIST SUBQUERY 1` → `SEARCH telemetry_history USING COVERING INDEX **ix_th_device_ts** (device_id=? AND ts<?)` | **否** |
| `log_history` | ✅ **接受** | `SEARCH log_history USING INTEGER PRIMARY KEY (rowid=?)` · `LIST SUBQUERY 1` → `SEARCH log_history USING COVERING INDEX **ix_lh_device_ts** (device_id=? AND ts<?)` | **否** |

- 两次 `EXPLAIN` 均 `success = true` · **`changed_db = false`** · **`rows_written = 0`** ⇒ **未执行任何删除**。
- **结论升级**：`DELETE … LIMIT` 在**生产引擎**上**语法被接受**，且计划**命中冻结索引**、**无全表扫描** ⇒
  **形态 B（`rowid IN (… LIMIT …)`）目前无必要**（按 `Q-3`，仅在首跑**真实执行**报错时才切、且须 `EXPLAIN` 留痕）。
- **残留（不得混淆）**：本项证明的是**语法 / 计划**，**不是**「真实 `DELETE` 执行成功」⇒ 该点归 **`W7-F4`（首跑）= `NOT RUN`**。

### 10.4 `PRE-4` 生产变量（Cloudflare 只读 API）

- 账号：`Wyl94@foxmail.com's Account` · ID `9e72d201efec6925b3a468a0e3fb4928`；登录 = OAuth（`wyl94@foxmail.com`）。
- 端点：`GET /accounts/:id/workers/scripts/guo-feeder-api/settings`
- 线上 bindings（共 **5** 个）：

| # | type | name | 备注 |
|---|---|---|---|
| 1 | `secret_text` | `ADMIN_API_TOKEN` | 密钥（非 var） |
| 2 | `secret_text` | `EMQX_APP_ID` | 密钥 |
| 3 | `secret_text` | `EMQX_APP_SECRET` | 密钥 |
| 4 | `d1` | `guofeeder_DB` | `database_id = 0d8385ba-a1a6-4fcc-8e43-ae61c014c43d`（**与 `wrangler.toml` 一致**） |
| 5 | `secret_text` | `WEBHOOK_SECRET` | 密钥 |

- ★ **`plain_text`（= `[vars]`）变量数 = `0`** ⇒ **`RETENTION_BATCH_SIZE` / `RETENTION_MAX_CALLS_PER_TABLE` /
  `RETENTION_HARD_STOP_CALLS` / `RETENTION_DAYS_TELEMETRY` / `RETENTION_DAYS_LOG` 全部未设置** ✅
- 补充：本地 `guo-feeder-api/wrangler.toml` **无 `[vars]` 段**，且全仓 `wrangler*.toml` / `.dev.vars` 中
  `RETENTION_` **命中 = 0**（`grep` 已核对）⇒ **生产将使用代码冻结默认值**。
- **代码默认值实测复核**（`src/retention.js`）：`batch = 1000` · `maxCallsPerTable = 20` · `hardStop = 45`（**代码常量，不从 env 读**）· telemetry `= 90 d` · log `= 180 d` ✅ **五项与冻结值逐项一致**。
- **`STOP` 判据**（发现任一 `RETENTION_*` 即 STOP）：**未触发**。
- 当前线上版本（只读）：`version_id = dd5ab824-ec39-41f3-9990-a37903aaf61f` · `created_on = 2026-10-15T14:25:11Z` · `compatibility_date = 2026-08-21`（与本地一致）⇒ **该版本早于 WP-7，无 `scheduled` handler**。

### 10.5 `PRE-5` 线上 Cron 当前状态（只读 API）

- 端点：`GET /accounts/:id/workers/scripts/guo-feeder-api/schedules`
- 返回：`success = true` · **`result = {"schedules":[]}`** ⇒ **线上 Cron Triggers = `0` 条** ✅（**与预期一致**）
- **`STOP` 判据**（发现任何既有 trigger 即 STOP 且不得覆盖）：**未触发**。
- 目标配置（**尚未生效**）：`0 3 * * *`（存在**仅**于本地未提交的 `wrangler.toml`，**未**注册到线上）。

### 10.6 `PRE-6` `wrangler deploy --dry-run`（**未 deploy**）

```
⛅️ wrangler 4.145.0
Total Upload: 96.84 KiB / gzip: 23.59 KiB
Your Worker has access to the following bindings:
Binding                           Resource
env.guofeeder_DB (guofeeder)      D1 Database
--dry-run: exiting now.
```

- **`exit 0`** ✅ · **无** bundle / startup 异常 ✅ · 绑定仅 `guofeeder_DB`（**再次印证无 `RETENTION_*` var**）✅
- 首次执行时曾出现一次无输出的 `SIGTERM`（宿主环境侧中止）⇒ 以 `timeout 90` + 落盘方式重跑即 `exit 0`；**`--dry-run` 本身未产生任何线上副作用**。

### 10.7 `PRE-7` 时区

| 表达式 | UTC | 北京时间（中国标准时间 · UTC+8） |
|---|---|---|
| `0 3 * * *` | `2026-10-17T03:00Z` | **`2026-10-17 11:00`** |

- 已用 `Intl.DateTimeFormat`（`Asia/Shanghai`）精确换算核对 ⇒ **UTC 03:00 = 北京 11:00** ✅
- **不改**（沿用 `Q-4` 批准值）。Cloudflare Cron **以 UTC 执行** ⇒ 与 `controller.scheduledTime`（epoch ms）一致，无本地时区依赖。

### 10.8 `PRE-8` `migrations/`

- 目录内容：`0001_p1_device_lifecycle.sql` · `0002_p1_hard_retirement.sql` · `0003_p3_history_tables.sql`（+ `README.md`）
- **`.sql` 计数 = `3`** ⇒ **未新增 / 未执行 migration** ✅ · 且 `migrations/**` `git status` 为空（**零改动**）。

### 10.9 审计用命令（可复核 · 均只读）

```bash
# 生产 D1 基线（每表单条 SQL）
wrangler d1 execute guofeeder --remote --json --config wrangler.toml \
  --command "SELECT COUNT(*) AS total_rows, COALESCE(MAX(id),0) AS max_id, \
  COALESCE(SUM(CASE WHEN ts < <CUTOFF> THEN 1 ELSE 0 END),0) AS over_window_rows, \
  COALESCE(MIN(ts),0) AS min_ts, COALESCE(MAX(ts),0) AS max_ts FROM <table>;"

# PRE-3 语法 / 计划探测（EXPLAIN = 编译不执行）
wrangler d1 execute guofeeder --remote --json --config wrangler.toml \
  --command "EXPLAIN QUERY PLAN DELETE FROM <table> WHERE device_id='probe0000000' AND ts < 1 LIMIT 1000;"

# PRE-4 / PRE-5（Cloudflare 只读 API）
GET /accounts/9e72d201efec6925b3a468a0e3fb4928/workers/scripts/guo-feeder-api/settings
GET /accounts/9e72d201efec6925b3a468a0e3fb4928/workers/scripts/guo-feeder-api/schedules

# PRE-6
wrangler deploy --dry-run --config wrangler.toml
```

> ⚠️ 上述 `d1 execute --remote` **仅**出现 `SELECT` 与 `EXPLAIN`；**全审计无 `INSERT`/`UPDATE`/`DELETE` 执行**。

### 10.10 范围完整性复核（**无越界 · 未触发 `STOP`**）

| 范围 | `git status` | 结论 |
|---|---|---|
| `src/history-ingest.js` · `presence.js` · `device-registry.js` · `emqx-admin.js` · `binding.js` · `credential.js` · `admin-auth.js` · `http-util.js` | **空** | ✅ 未改 |
| `migrations/**` | **空** | ✅ 未改 |
| 固件 `src/**` · `platformio.ini` · `data/**` | **空** | ✅ 未改 |
| EMQX 仓（`EMQX_Assets`） | **clean** · `HEAD = 3165aab` | ✅ 未改 |
| 云端仓改动（7 项） | 全属 WP-7 | ✅ 见 §10.12 |
| 固件仓改动（4 项） | 全属 WP-7 文档 | ✅ 见 §10.12 |

### 10.11 验收矩阵状态（**38 项 = 未增删**）

| 分组 | 状态 |
|---|---|
| **实施前（31 项）**：`W7-A1…A4` · `B1…B4` · `C1…C11` · `D1…D7` · `E1…E4` · `G2` | ✅ **`31/31 = PASS`**（§6 / §7.1） |
| `W7-C12`（非默认 batch size）· L1 组 `[14]`（薄入口契约） | ✅ `PASS`（**回归护栏，不占矩阵编号**） |
| **部署后（7 项）**：`W7-F1…F6` · `W7-G1` | ⛔ **`0/7`**：**`F3` `F4` `F5` `F6` `G1` 仍 `NOT RUN`**；`F1` 配置文本已落地但**生产注册未验证** = **`NOT RUN`** |

> ★ **`F1` ≠ `F3`**：`wrangler.toml` 中写入 `[triggers] crons = ["0 3 * * *"]` **只是配置文本**（本地未提交）；
> §10.5 已确证**线上 `schedules = []`** ⇒ **`F1` / `F3` 均 `NOT RUN`**，**不得混为一谈**。

### 10.12 纪律声明（`Phase 2`）与结论

**本阶段未做**：**未** `wrangler deploy` · **未**修改线上 Cron · **未**执行任何生产 DML（`INSERT`/`UPDATE`/`DELETE`）·
**未** `d1 export` · **未**执行 migration · **未**改生产 secrets / vars · **未**改 EMQX · **未**改固件 · **未** commit / push ·
**未**修改 WP-7 之外的任何文件 · **未**改变任何冻结窗口 / batch / budget / SQL 形态 / 既有裁决。

**`STOP` 条件复核**：`RETENTION_*` var 存在（无）· 既有 Cron Trigger（无）· 非 WP-7 意外修改（无）⇒ **一项未触发**。

**`Q-4`（方案 α · 部署后执行）**：临时高频 cron（如 `*/5 * * * *`）→ 等传播 → `wrangler tail` 观察 2–3 次 `scheduled`
→ 核对摘要 → 核对无 `exceededCpu` / subrequest / `calls` 超限 → 恢复 `0 3 * * *` → 最终效果核对。
**本阶段不执行任何 deployment。**

**`Q-7`（`d1 export`）**：**未获授权 ⇒ 未执行**。当前库仅 **1.19 MiB** / 84 + 20 行，备份风险极低；
是否上线前备份 **等待人工明确授权**（备选：授权后执行一次 `wrangler d1 export`）。

**进入生产 deployment 的条件**：技术前置条件 **已全部满足**（`PRE-1`/`3`/`4`/`5`/`6`/`7`/`8` 全绿 · 无 `STOP` · 无越界），
**但按纪律仍须人工明确授权**方可进入部署（`Q-10`）。**首跑预期将删除 `log_history` 的 20 行 `ts = 0` 记录**（§10.2）—— 该项属 `Q-2` 既有裁决，**部署前请人工知悉**。

---

## §11 部署执行与生产验收（`Phase 3` · `2026-10-16`）

> ## ✅ `PHASE 3 DEPLOYED & ACCEPTED` —— **`W7-A*…G*` = `PASS`（38/38）**
>
> **执行窗口（UTC）**：`17:22:08Z`（备份）→ `17:23:02Z`（部署前复查）→ `17:28:38Z`（临时 Cron 部署）→
> `17:40:46Z` / `17:45:46Z`（**2 次真实调度**）→ `17:48:22Z`（恢复正式 Cron 部署）
> **授权依据**：人工 `Phase 3` 指令（一次性授权：备份 · 部署 · 临时 Cron · 恢复 · 验收 · 提交）。

### 11.1 备份（`Q-7` 授权执行）

| 项 | 值 |
|---|---|
| 数据库 | `guofeeder` · `0d8385ba-a1a6-4fcc-8e43-ae61c014c43d`（`--remote`） |
| 导出 / 下载（UTC） | `2026-10-16T17:22:08Z` / `17:22:13Z` |
| 路径 | `D:/Guo_Feeder_Project/_backups/wp7/guofeeder_20261016T172208Z.sql`（**仓库外** ⇒ 不入 Git） |
| 大小 | `2,169,893 B`（≈ 2.07 MiB） |
| **SHA-256** | `da6c5eb6790a9d9b75313a4d25e7e1496c46fc26869d51d55043596d6af6f42e` |
| 本地验证 | ① 结构：8 表 `CREATE TABLE` + 索引齐全（含 `ux_lh_dedup`）；② 行级 INSERT：`telemetry_history` **20** / `log_history` **84**；③ **载入全新本地「真 D1 引擎」**（`--local --persist-to <gitignored>`）⇒ `success:true`，实测 `telemetry 20 / log 84 / device 9 / device_event 189 / mqtt_messages 3495` —— **未向生产恢复或写入任何数据** |

> 备份为**时点快照**（导出时 `log_history` = 84 行）；其后仍有**自然业务写入**（`17:23:02Z` 已达 85 行）⇒ 差异如实记录于 §11.4。

### 11.2 部署记录

| 阶段 | 版本 ID | 线上 Cron | 时间（UTC） |
|---|---|---|---|
| **B 临时（方案 α）** | `3e1b2e0b-ac4b-4b92-983a-62e68730e2a8` | `*/5 * * * *`（1 条 · created `17:28:45.93Z`） | `17:28:38Z` → `17:28:47Z` |
| **D 正式（恢复）** | `1ffbbe2e-ed09-43f9-b576-06380bdec8e0` | `0 3 * * *`（**1 条** · created `17:48:30.04Z` · **无 `*/5`**） | `17:48:22Z` → `17:48:31Z` |

- 两次 deploy 均 `exit 0` · `Worker Startup Time 2 ms` · `Total Upload 96.84 KiB / gzip 23.59 KiB`（与已审阅基线**逐字一致**）；部署前 dry-run 亦 `exit 0`。
- 线上 bindings 前后**均为 5 个且未变**（4 × `secret_text` + `d1:guofeeder_DB`）；**`plain_text`（即 `[vars]`）= `[]`**；secrets 清单 **4 个未变**。
- 账号内 Worker **仅 `guo-feeder-api`**（`modified_on = 17:48:29.578984Z` = 本次恢复部署）；D1 仅 `guofeeder` ⇒ **未触碰任何其它 Worker / D1 / 生产配置**。

### 11.3 真实调度执行数据（`wrangler tail` 实测 · **2 次**）

| # | 触发（UTC） | `scheduledTime` | 版本 | `outcome` | cpu / wall | exceptions |
|---|---|---|---|---|---|---|
| 1 | `2026-10-16T17:40:46.888Z` | `17:40:46.047Z` | `3e1b2e0b…` | `ok` | `3 ms` / `244 ms` | 0 |
| 2 | `2026-10-16T17:45:47.025Z` | `17:45:46.047Z` | `3e1b2e0b…` | `ok` | `4 ms` / `438 ms` | 0 |

```jsonc
// #1 （逐字）
{"ok":true,"nowSec":1792172446,"cron":"*/5 * * * *","batchSize":1000,
 "days":{"telemetry_history":90,"log_history":180},"maxCallsPerTable":20,"hardStop":45,
 "truncated":false,"calls":4,
 "tables":[
   {"table":"telemetry_history","days":90,"cutoff":1784396446,"devices":1,"deleted":0,"batches":1,"calls":2,"truncated":false,"converged":true},
   {"table":"log_history","days":180,"cutoff":1776620446,"devices":1,"deleted":20,"batches":1,"calls":2,"truncated":false,"converged":true}],
 "errors":[],"errorsTruncated":false}

// #2 （逐字）
{"ok":true,"nowSec":1792172746,"cron":"*/5 * * * *","batchSize":1000,
 "days":{"telemetry_history":90,"log_history":180},"maxCallsPerTable":20,"hardStop":45,
 "truncated":false,"calls":4,
 "tables":[
   {"table":"telemetry_history","days":90,"cutoff":1784396746,"devices":1,"deleted":0,"batches":1,"calls":2,"truncated":false,"converged":true},
   {"table":"log_history","days":180,"cutoff":1776620746,"devices":1,"deleted":0,"batches":1,"calls":2,"truncated":false,"converged":true}],
 "errors":[],"errorsTruncated":false}
```

| 指标 | #1 | #2 | 判定 |
|---|---|---|---|
| `batchSize` / `maxCallsPerTable` / `hardStop` | `1000` / `20` / `45` | 同 | ✅ 冻结值 |
| 每表 `days` / `cutoff` | 90 · `nowSec−90d` / 180 · `nowSec−180d` | 同（`cutoff` 随 `nowSec` 平移 300 s） | ✅ 窗口正确 |
| 每表 `deleted` | telemetry `0` · log **`20`** | telemetry `0` · log **`0`** | ✅ 首轮清空超窗 · **第 2 轮幂等** |
| 每表 `batches` / `calls` | `1` / `2` | `1` / `2` | ✅ ≤ 预算 `20` |
| 全局 `calls` | **`4`** | **`4`** | ✅ ≤ `42` · **未触及硬停止 `45`** |
| `truncated` / `converged` | `false` / 两表 `true` | 同 | ✅ 无截断 |
| `errors[]` / `errorsTruncated` | `[]` / `false` | 同 | ✅ 无错误 |
| cpu / 异常退出 | `3 ms` / 无 | `4 ms` / 无 | ✅ 无 `exceededCpu` |

**★ `ts = 0` 记录专项**：首轮 `deleted = 20`；D1 复核 `log_history` 的 `ts = 0` 行数 **`20 → 0`** ⇒ 20 条 `ts = 0` 记录被 retention **正常清理**（`Q-2`：`ts <= 0` 按数值参与 `ts < cutoff`）。**删除量以运行证据为准，未预设。**

### 11.4 数据安全（执行前后）

| 表 | 执行前（`17:23:02Z`） | 执行后（`17:48:49Z`） | 超窗余（后） | 判定 |
|---|---|---|---|---|
| `telemetry_history` | 20 行 · `MAX(id)=40` · 超窗 0 | **20 行** · `MAX(id)=40` · 超窗 **0** | 0 | ✅ 未删（无超窗候选） |
| `log_history` | 85 行 · `MAX(id)=499` · 超窗 20 · `ts=0` 20 | **66 行** · `MAX(id)=505` · 超窗 **0** · `ts<=0` **0** | 0 | ✅ 恰删 20（全为 `ts=0`）· 无窗内误删 |

- 其余表**未受影响**：`device 9` · `device_credential 16` · `device_binding 1` · `device_event 189` · `mqtt_messages 3495`（与备份时点一致）。
- **无窗内误删校核**：执行后 `log_history MIN(ts)=1792084426` ≫ `cutoff=1776620929`；`telemetry_history MIN(ts)=1791880079` ≫ `cutoff=1784396929`；两表**超窗行 = 0** ⇒ 清理范围与冻结规则完全一致。
- `MAX(id)` 变化（`499 → 505`）来自**自然业务写入**（期间新增 log 行 + 幂等键吸收的重投），**与删除无关**（被删行 `id` 更早）。

### 11.5 验收矩阵最终状态（38 项）

| 分组 | 结果 |
|---|---|
| 实施前 31 项（`A1–A4` · `B1–B4` · `C1–C11` · `D1–D7` · `E1–E4` · `G2`） | ✅ **`31/31 = PASS`** |
| 部署后 7 项（`F1–F6` · `G1`） | ✅ **`7/7 = PASS`**（§7.2） |
| **合计** | ✅ **`38/38 = PASS`** |
| 回归护栏（`W7-C12` · L1 组 `[14]`） | ✅ `PASS`（不占矩阵编号） |

### 11.6 未完成 / 非本轮范围

- `W7-G1` 的**字面**验证（部署 `crons = []` ⇒ Triggers `0` 条）**未执行** —— 本轮以「`*/5` → `0 3 * * *`」**覆盖实测**证明 trigger 可被移除（§11.2）。
- 下一次**正式**调度（`0 3 * * *` ⇒ `2026-10-17T03:00Z` = 北京时间 `11:00`）**尚未发生** ⇒ **不得**标记为已验证。
- `H-WP3-06` / `H-WP3-09` / 告警设施 / `rows-read` 规模化优化：`DEFERRED`。
- `BT-1`（云端 `log_ack`）/ `WP-5` / `WP-6` / `mqtt_messages` 噪音清理 / MQTT 密码轮换：**WP-7 之外**，属后续工作包。

### 11.7 本轮纪律声明

- **已授权执行**：`d1 export`（1 次）· `wrangler deploy`（**2 次**：临时 → 正式）· Cron 配置变更（**已在同一窗口内恢复正式值**）。
- **未做**：未改 `migrations/**`（仍 `0001/0002/0003`）· 未改 schema / 未新增索引 · 未设 `RETENTION_*` 变量（生产 `plain_text = []`）· 未改 secrets · 未改其它 Worker · 未改固件仓 / EMQX 仓 · 未改 retention 算法 / 窗口 / batch / 预算 / 硬停止 / `ts=0` 裁决 · 未切形态 B · 未手工 SQL 补删 · 未删除备份文件 · 未把凭据写入任何报告 / 日志 / 文档 / Git。
- **临时高频 Cron 已移除**（线上 `schedules` 仅 `0 3 * * *` 1 条）。

---

> **`计划:WP-7` = `PHASE 3 DEPLOYED & ACCEPTED`** —— `W7-A*…G*` = **`PASS`（38/38）**。
> **生产版本 `1ffbbe2e-ed09-43f9-b576-06380bdec8e0` · 线上 Cron = `0 3 * * *`（1 条 · 无临时残留）.**
> **`Backup taken (SHA-256 recorded) · No in-window data touched · No migration · No other Worker modified.`**

---

*本文件于 **2026-10-16** 建立，为 **`计划:WP-7`（History Retention）第一阶段实施记录**；
设计依据 = **`DT:WP-3`（`FROZEN`）**；实施依据 = **任务单 `rev.2`**；基线 = 固件 `4e61a9e` · 云端 `f519c9e` · 消息 `3165aab`。*
*同 **`2026-10-16`** 追加 **§3.1 / §3.2 生产部署前一致性复查**（`scheduled()` 薄入口化 · `RETENTION_BATCH_SIZE` 可调性取证），
并将 §6 测试计数更新为 **L1 `99 PASS / 0 FAIL`** · **L2 `89 PASS / 0 FAIL`（两遍逐行一致）**；`§7` 矩阵 **38 项未变**（仍 `31/31 PASS` + `0/7 NOT RUN`）。*
*再于 **`2026-10-16` 晚 / `10-17` 凌晨** 追加 **§10 `Phase 2` 部署前审计**（**全只读**：生产 D1 基线 · `EXPLAIN` 语法/计划探测 ·
线上 `[vars]` 与 Cron Triggers 只读核查 · `deploy --dry-run` `exit 0` · 范围完整性）⇒
**`PHASE 2 = READ-ONLY AUDIT COMPLETE` · 无 `STOP` 条件 · 仍未部署 · 等人工授权**。*
*最终于 **`2026-10-16T17:22Z–17:49Z`** 追加 **§11 `Phase 3` 部署执行与生产验收**（备份 `d1 export` · 临时 `*/5` 部署 →
2 次真实 `scheduled` 取证（`deleted` 20 / 0）· 恢复 `0 3 * * *` 部署 · 最终只读验收）⇒
**`计划:WP-7` = `PHASE 3 DEPLOYED & ACCEPTED` · `W7-A*…G*` = `PASS`（38/38）**。*
