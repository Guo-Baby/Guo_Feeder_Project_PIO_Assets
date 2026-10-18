# `RET-1` 实现授权申请（`H-RET-3`）

> ## 状态：**`H-RET-3` = ✅ `APPROVED WITH CONSTRAINTS`（`2026-10-18`）· 实施已按批准范围执行 · 实现与范围 = `ACCEPTED WITH KNOWN LIMITATIONS` · 生产部署 = `DEFERRED` · `H-RET-4` = ⛔ 本轮不批准 · 未提交**
>
> **申请日期**：`2026-10-18`　**批准日期**：`2026-10-18`　**申请对象**：人工（项目所有者）
> **申请标的**：授权修改 `Cloudflare_Assets/guo-feeder-api/src/retention.js` **及其两个测试文件**，
> 以落地 **`RET-1` 方案 B** 的**已裁决语义**。
>
> **前置已满足**：`H-RET-1`（表范围）= ✅ **仅 `log_history`** · `H-RET-2`（`ts<0`）= ✅ **B-1** ·
> **`H-RET-3`（实现授权）= ✅ `APPROVED WITH CONSTRAINTS`**（裁决原文见 **§14**）。
> **`H-RET-4`（部署）= ⛔ 本轮不批准**（**非否决未来部署**）：须先闭合 **§15.2** 的 5 项前置；
> **即使全部测试通过也不代表获得部署授权**。
>
> **★ 终审（`2026-10-18`）**：实现与范围 = **`ACCEPTED WITH KNOWN LIMITATIONS`**（**不得解释为生产修复已完成**）；
> 正式交接记录 = `docs/review/RET-1-Final-Read-Only-Acceptance.md` **§9** · 本文 **§15**。
>
> **上位文档**：
> - 语义定稿：`docs/architecture/P3-RET-1-CreatedAt-Fallback-Implementation-Plan.md`（**`rev.5`** · §13 实施记录 · §14 终审）
> - 根因与方案比较：`docs/review/RET-1-Retention-Time-Boundary-Investigation.md`（`rev.3`）
> - 冻结项修订：`P3-WP3-History-Storage-Policy-Design.md` **§7.4.1**（`D-3`）· `P3-WP7-Implementation-Record.md` **§2.1**（`Q-2`）

---

## §1 申请摘要（一页）

| 项 | 内容 |
|---|---|
| **要解决的事实** | `2026-10-18 03:00Z`，`log_history` 的 `ts=0` 行 **26 → 0**，其中 **3 条 `CRITICAL`**；`code` 未留档 ⇒ **不可恢复** |
| **要落地的语义** | `log_history`：`ts>0` 按 `ts` 判过期；**`ts<=0`（含负数）按 `created_at`** 判过期 |
| **不改的东西** | `telemetry_history` 谓词 · 窗口值 90/180 · 索引 · migration · `wrangler.toml` · `src/index.js` · EMQX · 固件 |
| **产品代码改动面** | **1 个文件**：`src/retention.js`（预计 **+10…+16 / −3**，净 **+7…+13** 行） |
| **测试改动面** | **2 个文件**：L1 套件（含 **stub 必须改造**）+ L2 脚本（**仅新增段，既有段零改动**） |
| **风险方向** | **单向安全**：`created_at` 必然 ≥ 事件时刻 ⇒ 只会**延长**保留，**不会提前删除**（结构论证，非样本推断） |
| **回滚** | 单文件还原 + `wrangler rollback`（**不涉及 schema、不涉及数据**）；**但已删行不可恢复** |
| **需人工签字** | 本文件 §13 |

---

## §2 依据（裁决原文，逐字）

> **`H-RET-1`：批准，仅修改 `log_history` 的 retention 语义；`telemetry` 保持不变。**
>
> **`H-RET-2`：批准 B-1；`log_history` 中 `ts>0` 按 `ts` 判断过期，`ts<=0` 按 `created_at` 判断过期。**
>
> **本次仅批准设计语义，不代表批准修改代码或部署。**

**⇒ 本申请正是"代码修改"这一次单独批准的动作。** 未获批准前，`retention.js` 保持逐字不变。

---

## §3 精确文件范围

### 3.1 申请授权修改（**穷举**，共 3 个代码/测试文件 + 1 个文档）

| # | 文件 | 现状 | 允许的改动 |
|---|---|---|---|
| 1 | `Cloudflare_Assets/guo-feeder-api/src/retention.js` | 456 行 | **仅** `runOneTable()` 内的 `deleteSql` 构造与 `.bind(...)` 实参（`:219` / `:249-252`）；可新增 1 个模块级纯函数（`buildDeleteStatement`） |
| 2 | `.../test/current/retention_policy.test.mjs` | 638 行 | **stub 改造**（`:97` 绑定解构 · `:121` 谓词）+ **新增** `log_history` 侧断言组；**不改** `telemetry` 侧既有断言 |
| 3 | `.../test/real_d1/retention_real_d1.sh` | 477 行 | **仅新增** `PHASE E`（新谓词用例）；**既有 `W7-B*` / `W7-C*` 段零改动** |
| 4 | `docs/architecture/P3-RET-1-CreatedAt-Fallback-Implementation-Plan.md` | — | 追加"实施记录"章节（§11 要求） |

### 3.2 **禁止触碰**（越界即视为未授权变更）

| 类别 | 具体 |
|---|---|
| **Schema / migration** | `migrations/0001*` / `0002*` / `0003*` —— **零改动**（`created_at` 列已存在且 `NOT NULL DEFAULT (unixepoch())`） |
| **配置** | `wrangler.toml`（不新增 `RETENTION_*`；`Q-5` 纪律不变）· `[triggers]` **不动** |
| **其他产品代码** | `src/index.js`（含 `scheduled()` 签名）· `history-ingest.js` · `log-ack.js` · `mqtt-columns.js` · `emqx-admin.js` · `presence.js` · `device-registry.js` · `admin-auth.js` · `binding.js` · `credential.js` |
| **另一张表** | `telemetry_history` 的删除语句 —— **必须逐字保留** `WHERE device_id = ? AND ts < ?`（2 个绑定参数） |
| **其他仓库** | 固件仓（`PIO_Assets/Guo_Feeder_Project`）· EMQX 仓（`EMQX_Assets`）—— **零改动** |
| **数据** | 生产 D1 **零写入**（**唯一例外**：`§6.3` 的 `EXPLAIN` 只读，`rows_written` 必为 0） |
| **动作** | 不得 `wrangler deploy` · 不得 `git commit/push/reset/clean` · 不得重启设备 · 不得生成/注入日志帧 |

---

## §4 预计代码差异（`retention.js`）

### 4.1 现状（逐字，`src/retention.js`）

```js
// :219
const deleteSql = `DELETE FROM ${table} WHERE device_id = ? AND ts < ? LIMIT ?;`;   // 形态 A

// :249-252
const result = await db
    .prepare(deleteSql)
    .bind(deviceId, rec.cutoff, cfg.batchSize)
    .run();
```

**问题**：`deleteSql` 是**单一模板**，两表共用 ⇒ 无法表达"仅 `log_history` 换谓词"（`F-5`）。

### 4.2 拟改动形态（**示意 · 未落盘 · 供授权审查**）

```js
// 新增（模块级纯函数 · 无 I/O · 可单测）
function buildDeleteStatement(table)
{
    if(table === "log_history")                       // ★ H-RET-1：仅此表
    {
        return {
            sql: `DELETE FROM log_history
                   WHERE device_id = ?
                     AND ( (ts > 0 AND ts < ?) OR (ts <= 0 AND created_at < ?) )
                   LIMIT ?;`,
            args: (deviceId, cutoff, batchSize) => [deviceId, cutoff, cutoff, batchSize]
        };
    }
    return {                                          // telemetry：逐字保留（F-5）
        sql: `DELETE FROM ${table} WHERE device_id = ? AND ts < ? LIMIT ?;`,
        args: (deviceId, cutoff, batchSize) => [deviceId, cutoff, batchSize]
    };
}

// :219 → 
const del = buildDeleteStatement(table);

// :249-252 →
const result = await db.prepare(del.sql).bind(...del.args(deviceId, rec.cutoff, cfg.batchSize)).run();
```

| 指标 | 预计 |
|---|---|
| 行数变化 | **+10…+16 / −3**（净 **+7…+13**，误差 ±3）；差异来源 = 是否把两条 SQL 用模板或多行字面量书写。实际以 `git diff --stat` 为准，**超出区间须在报告中解释** |
| 新绑定参数顺序 | `deviceId, cutoff, cutoff, batchSize`（**与占位符一一对应**，`F-6`） |
| 变更函数 | **仅** `runOneTable()` + 新增纯函数 |
| 未变更 | `resolveConfig` / `computeCutoffSec` / `deriveMaxTotalCalls` / `runRetention` / `runRetentionSafely` / `logRetentionSummary` 的**签名与语义** |

### 4.3 三条**硬性形态约束**（违反即 `STOP`）

| # | 约束 | 理由 |
|---|---|---|
| 1 | **禁止** `COALESCE(NULLIF(ts,0),created_at) < ?` | 生产 `EXPLAIN` 实测退化为 `ux_lh_dedup (device_id=?)` ⇒ **该设备全扫** |
| 2 | **禁止** 独立 `OR (ts < 0)` 分支 | 实测同样退化为 `ux_lh_dedup`；且 `B-1` 已裁决"同支" |
| 3 | **禁止** `AND ts > 0`（作为表级过滤） | 会把 `ts<=0` 整类排除在删除之外 ⇒ **永久保留**（`ST-7`） |

> 注意（区分）：`OR` 内部的 `ts > 0` **是分支条件，不是**表级过滤 —— 形态 4.2 已正确。

---

## §5 不变式 / 预算影响

| 不变量 | 变更前 | 变更后 | 判据 |
|---|---|---|---|
| `I-P1` 单表 DELETE ≤ `maxCallsPerTable`（20） | 成立 | **成立** —— 调用次数由 `rec.batches` 计数，与谓词无关 | `T-12` |
| `I-P3` 正常 `Total ≤ 42`（2 SELECT + 40 DELETE） | 成立 | **成立** | `T-12` / 摘要 `calls` |
| `I-P4` `HARD_STOP = 45` 不可达 | 成立 | **成立** | L1 `[7]` 重跑 |
| 收敛判定 `n < batchSize ⇒ done` | 成立 | **仍成立** —— OR 形态不改变"本批是否删满"的语义 | `T-11` |
| `D-1` 分批 / `D-2` 无大事务 | 成立 | **成立**（仍 `LIMIT ?` 单语句） | 代码审查 |
| `D-3` 确定性（改判后版本） | — | **成立**：谓词 = 纯函数（行 + `cutoff`），无 `now()` / 随机 / 行序依赖 | 代码审查 |

**★ 为什么"只会延长、不会提前删"（结构论证，非样本推断）**

1. `created_at` = **D1 入库时刻**（`INSERT` 列表**不含** `created_at` ⇒ DEFAULT `unixepoch()`，见 `history-ingest.js:682`）。
2. 入库**必然晚于**事件发生 ⇒ `created_at ≥ 事件时刻`。
3. `cutoff` = 服务端"现在 − 180 d"，与 `created_at` **同钟**（均为服务端 `unixepoch` 轴）。
4. ⇒ `created_at < cutoff` 成立时，必已过"事件时刻 + 180 d" ⇒ **保留时长 ≥ 180 d**。

**★ 为什么不会"滑动窗口"**：`INSERT OR IGNORE`（`history-ingest.js:682`）在 `ux_lh_dedup` 冲突时**不更新**
已有行 ⇒ 重复补传**不会刷新 `created_at`** ⇒ 同一条日志的删除时刻**确定**（`D-3` 的目的保持）。

**容量影响**：`ts<=0` 行按 `26 行/天 × 180 d × ≈378 B ≈ **1.7 MiB/设备**`（11 台 ≈ 19 MiB ≈ 5 GB 的 0.4%）⇒ 不构成约束。

---

## §6 测试矩阵（本次申请须交付的**新增/改造**项）

> 完整编号见任务单 **§7**。本节只列**本次新增或必须改造**的项；编号与任务单**一一对应**。

### 6.1 L1（`test/current/retention_policy.test.mjs`）

| 组 | 项 | 内容 |
|---|---|---|
| **stub 改造** | `N-8` | `:97` 三绑定解构 → 按表解构；`:121` `row.ts < cutoff` → 按表谓词（`log` 用 OR + `created_at`）**★ 不改则新 SQL 的 4 个绑定会被错位解释** |
| **新组 `[15]`** `log`：`ts>0` 不变 | `T-1` / `T-2` / `T-3` | `cutoff-1` 删 · `cutoff` 留 · `>cutoff` 留（**这些是老语义的有效部分，仍需在新环境下重跑**） |
| **新组 `[16]`** `log`：`ts=0` 兜底 | `T-4` / `T-5` / `T-6` / `T-7` | 旧 `created_at` 删 · `=cutoff` 留 · `+1` 留 · **远新于 cutoff 留**（`RET-1` 核心） |
| **新组 `[17]`** `log`：`ts<0`（B-1） | `T-20…T-24` + `T-24b` | `-1` 旧删 · `-1` 边界留 · `-1` 新留 · **INT32 下界旧删** · **`ts=0` 与 `ts<0` 同 `created_at` ⇒ 同结果** · **反向：`created_at > cutoff` 的负数必须留**（区分 B-2/B-3） |
| **新组 `[18]`** `created_at` 边界/单位/异常 | `T-25` / `T-26` / `T-27` / `T-28` / `T-29` | 三点同批 · **量纲 = 秒**（`cutoff < 1e11` · `scheduledTime > 1e12`） · `created_at=0` ⇒ 删 · NULL 不可能（两列 `NOT NULL`） |
| **回归** | `T-9` | `telemetry_history` 的 `ts=0` **仍删** ⇒ 单表裁决的反向证明 |
| **混合/预算** | `T-10` / `T-11` / `T-12` | 4 类行混合 · `batchSize` 边界 · 预算截断 |
| **保留** | L1【1】`:194/209/210` | **不修改**（作用对象是 `telemetry`） |

### 6.2 L2（`test/real_d1/retention_real_d1.sh` · 真 D1 引擎）

| 项 | 内容 |
|---|---|
| **新 `PHASE E`** | `T-13`（`ts=0` 新旧各 1 行，**显式写入 `created_at`**）· `T-15`（`ts=-1` 与 INT32 下界，各一旧一新）· `T-30`（`INSERT … created_at NULL` 必须**报错**）· `T-16`（新谓词 `EXPLAIN`） |
| **既有段** | `W7-A*…D*` **全部保留原样并重跑**；**`W7-B3`（`telemetry` `ts=0` 仍删）零改动 + 必须 `PASS`**（`T-32`） |

### 6.3 `EXPLAIN QUERY PLAN` 验收（**逐行原文，不接受结论性描述**）

| # | 环境 | 要求 |
|---|---|---|
| **`T-31`** | **本地 D1**（`wrangler d1 execute --local`） | **两段**都取：① 旧谓词 `ts < ?`；② 新 OR 谓词。每段输出**完整 `id / parent / notused / detail` 四列逐行**，禁止只写"命中 `ix_lh_device_ts`" |
| **`T-33`** | **生产只读**（`--remote`，`EXPLAIN` 不执行） | 同上；`device_id` 用**不存在的占位**；执行前后各一次行数快照（零写入自证）；**不得**据此声称任何生产验收 |

**通过判据（三个形态都取，缺一不可）**

| 形态 | 期望计划 | 判定 |
|---|---|---|
| 旧 `device_id=? AND ts<?` | `USING COVERING INDEX ix_lh_device_ts (device_id=? AND ts<?)` | ✅ 基线 |
| 新 `((ts>0 AND ts<?) OR (ts<=0 AND created_at<?))` | **`MULTI-INDEX OR`**：`INDEX 1/2` 均 = `ix_lh_device_ts` | ✅ 通过 |
| 任一出现 `SCAN log_history` / 仅 `ux_lh_dedup (device_id=?)` | — | ❌ **`ST-1` ⇒ 停止** |

---

## §7 复现命令（供批准后执行，**当前一律不执行**）

```bash
# 环境：★ 调 node / wrangler 前必须清空沙箱代理
unset http_proxy https_proxy HTTP_PROXY HTTPS_PROXY ALL_PROXY all_proxy

cd D:/Guo_Feeder_Project/Cloudflare_Assets/guo-feeder-api

# L1
node test/current/retention_policy.test.mjs

# L2（真实 D1 引擎 · workerd · 不碰生产）
bash test/real_d1/retention_real_d1.sh

# 全量回归
node test/run_all.mjs

# 构建门（不部署）
npx wrangler deploy --dry-run

# 本地查询计划（只读）
npx wrangler d1 execute guofeeder --local --command \
  "EXPLAIN QUERY PLAN DELETE FROM log_history WHERE device_id='probe' AND ((ts>0 AND ts<1) OR (ts<=0 AND created_at<1)) LIMIT 1000;"
```

> **`L2` 至少跑两遍**，逐行比对（`WP-7` 的既有纪律：两次输出必须**逐行一致**，否则不具确定性）。

---

## §8 回滚方案

| 层 | 手段 | 影响 | 前置 |
|---|---|---|---|
| **L-1 代码** | 还原 `src/retention.js`（单文件）· `git checkout -- src/retention.js`（**须另批授权**） | 立即回到旧谓词 | 无 |
| **L-2 线上** | `wrangler rollback` 到当前生产版本 **`72023596-657a-4a65-aa62-81848540f324`** | 秒级生效；**Cron 配置不受影响**（代码版本与 Cron 是两个独立回滚面） | 部署授权 |
| **L-3 Cron** | `[triggers]` **不动** ⇒ 无需回滚；仅在"清理本身有害"时才考虑 `crons = []`（**注意语义**：`[]` = **删除全部** triggers，`undefined` = 保留现状） | 停止清理 | 部署授权 |
| **L-4 数据** | **不可回滚** —— 删除无回收站 | 只能靠部署前 `d1 export` 备份（**须单独授权**，`Q-7`） | `Q-7` |

> **★ 回滚的可达性声明**：本改动是**单文件、单函数、无 schema** ⇒ L-1/L-2 均为分钟级。
> **但**：若新谓词**误删**了本应保留的行，**数据不可恢复** ⇒ 这是本申请**唯一不可逆风险**。
> 缓解：`created_at ≥ 事件时刻` 的结构性单调（§5）使"误删"方向**几乎只可能来自实现缺陷**（绑定错位），
> 而非语义本身 ⇒ 因此 `§6.1` 的 stub 改造与 `T-26` 量纲断言是本申请的**最高优先级护栏**。

---

## §9 旧 `PASS` 作废声明（**不可复用**）

> **事实**：`WP-7` 的 `L1 99 PASS` · `L2 89 PASS` · 生产 `W7-A*…G* 38/38 PASS` —— **全部在旧谓词下取得**。
> 新谓词改变了"哪些行会被删" ⇒ **旧结论对新语义零证明力**。

| 旧资产 | 处置 |
|---|---|
| L1【1】`ts=0` 被删（`:194/209/210`） | **保留原样**（对象是 `telemetry`，裁决不变） |
| L1 stub（`:97` / `:121`） | **必须改造**（它编码了旧语义） |
| L1 其余 90+ 断言 | **重跑**，不引用旧输出 |
| L2 `W7-B3` | **零改动 + 重跑 `PASS`** |
| L2 其余段 | **重跑** |
| 生产 `W7-A*…G*` 38/38 | **作废**；`H-RET-4` 后按 `T-17…T-19` 重新取证 |

**⇒ 实施报告（任务单 §11）中，每个 `PASS` 必须能追到"本轮运行的原始输出 + 时间戳"，否则视为伪造确定性。**

---

## §10 残余风险与未覆盖面

| # | 风险 | 定性 | 处置 |
|---|---|---|---|
| **R-1** | 离线补传使 `created_at` 远晚于 `ts`（实测最坏 **≈8.9 d**）⇒ `ts=0` 行最多**多留约 9 天** | `PROXY`（单设备 166 行样本） | 知情接受；容量影响可忽略 |
| **R-2** | `created_at` 滞后过大（如设备离线 30 d 补传）⇒ 保留期实际**多 30 d** | 结构上安全（只延长） | 知情接受 |
| **R-3** | 生产 `ts<0` 行数 = **0** ⇒ `ts<0` 分支**无真实数据验证**，仅构造性验证（L1/L2） | 已知缺口 | 接受：固件 `timestamp` 为 `uint32_t`，**不可能产生负数** |
| **R-4** | `ts=0` 的**真实发生率**仍是单次冷启动的爆发样本（26 条 / 6 h），非稳态 | `PROXY` | 不阻塞；上线后继续观察 |
| **R-5** | 两表语义**有意不一致**（`telemetry` 仍删 `ts=0`） | **已裁决** | 由 `T-32` 固化为"不变"的反向证明；日后若要一致，须另开裁决 |
| **R-6** | 已删的 3 条 `CRITICAL` **仍不可恢复** | 不可逆 | 本修复**只防未来**，不追过往 |

---

## §11 执行顺序（批准后）

| 步 | 内容 | 门 |
|---|---|---|
| **E-1** | 改造 L1 stub（先让测试**能**表达新语义） | — |
| **E-2** | 改 `retention.js`（`buildDeleteStatement`） | `F-1…F-10` |
| **E-3** | 补 L1 `[15]…[18]` 组 | `T-1…T-12` / `T-20…T-29` |
| **E-4** | 补 L2 `PHASE E` | `T-13` / `T-15` / `T-16` / `T-30` |
| **E-5** | 本地 `EXPLAIN` 两段取证 | `T-31` |
| **E-6** | `run_all` + `deploy --dry-run` | 构建门 |
| **E-7** | 生产只读 `EXPLAIN`（若同批授权） | `T-33` |
| **E-8** | 写实施记录（任务单 §11 九项） | — |
| **E-9** | **停止** —— 等 `H-RET-4` 部署授权 | ⛔ |

> **`E-9` 是硬边界**：实施完成**不等于**允许部署。`H-RET-4` 未批前，生产保持旧谓词。

---

## §12 完成定义（Definition of Done · 仅针对实现）

- [ ] `retention.js` 仅 `runOneTable()` + 1 个纯函数变更，`git diff --stat` 符合 §4.2
- [ ] `telemetry_history` 语句**逐字未变**（`git diff` 中该行不出现）
- [ ] `migrations/**` **零差异**
- [ ] `wrangler.toml` **零差异**
- [ ] L1 全绿（含新 `[15]…[18]`），输出**含时间戳**
- [ ] L2 全绿 **×2 逐行一致**（含 `W7-B3` 未改动仍 `PASS`）
- [ ] `run_all` `exit 0`
- [ ] `deploy --dry-run` `exit 0`（**未真部署**）
- [ ] `T-31` 本地 `EXPLAIN` 两段逐行原文入档；**无 `SCAN` / 无 `ux_lh_dedup` 单路径**
- [ ] 任务单 §11 的九节报告齐备
- [ ] 明确声明：**未部署 / 未 commit / 未改 EMQX / 未改固件 / 未写生产 D1**

---

## §13 授权签字区

| 项 | 状态 |
|---|---|
| **`H-RET-3` 实现授权** | ☑ **批准（`APPROVED WITH CONSTRAINTS`）**　☐ 驳回　☐ 附条件批准 |
| 授权范围确认 | ☑ 仅 §3.1 的 3 个代码/测试文件 + 1 个文档 |
| 生产只读 `EXPLAIN`（`T-33`）是否同批允许 | ☑ **允许**（`T-33` 已执行，原文见任务单 §13.4） |
| 明确**不含**部署 | ☑ 已知悉（`H-RET-4` 另行申请） |
| 签字 / 日期 | 人工裁决　`2026-10-18` |
| **`H-RET-4`（部署）** | ⛔ **本轮不批准**（`2026-10-18` 终审）—— 非否决未来部署；须先闭合 **§15.2** 的 5 项前置（原 `A-1…A-5`），**未执行** |

---

## §14 ★ 裁决原文（`2026-10-18` · 逐字归档）

> ### 正式裁决：批准 `H-RET-3`，附带边界
> **`H-RET-3`：`APPROVED WITH CONSTRAINTS`**
>
> #### 1. 允许实施
> - 产品代码仅修改 `src/retention.js`。
> - 允许修改申请中明确列出的两个 retention 测试文件及一个文档。
> - 按 B-1 实现：`log_history` 的 `ts>0` 按 `ts` 判断保留期；`ts<=0` 按 `created_at` 判断保留期。
> - 必须修正 `makeRetentionStub`，并执行新语义的完整测试矩阵；旧 PASS 不得冒充本轮测试结果。
>
> #### 2. 同批允许生产只读 `EXPLAIN`：是
> - 仅执行申请范围内的只读查询计划检查，**不执行**生产 `DELETE`、`INSERT`、`UPDATE` 或其他数据写入。
> - 必须保存**逐行原始输出**，包括 `id`、`parent`、`notused`、`detail`，同时记录**执行时间**和**目标 SQL**。
> - 出现 `SCAN` 或计划退化到 `ux_lh_dedup`，**立即停止并报告**，不得擅自修改方案绕过门槛。
>
> #### 3. 明确禁止
> - 不修改 `migrations/`、`wrangler.toml`、`src/index.js` 或 `telemetry_history` 的保留逻辑。
> - 不部署 Worker、不修改 Cron、不写生产 D1、不修改 EMQX、不重启设备。
> - 不执行 Git `commit`、`push`、`reset`、`clean`；保留所有既有工作区变更。
> - 不自动解除 `BT-1`、`G-4`、`WP-5` 或 `STOP-R1` 门禁。
>
> **交付要求**：实施后先提交本地测试报告、差异清单、逐项验收证据和生产只读 `EXPLAIN` 原始输出，等待审查。
> **即使全部测试通过，也不代表获得部署授权**；任何超出上述范围的变更必须另行申请。

### 14.1 约束 → 执行映射（**逐条可核验**）

| # | 约束 | 执行结果 |
|---|---|---|
| C-1 | 产品代码仅 `src/retention.js` | ✅ `git status` 仅 1 个产品文件变更（`+44/−4`） |
| C-2 | 两个测试文件 + 一个文档 | ✅ `test/current/retention_policy.test.mjs` · `test/real_d1/retention_real_d1.sh` · 任务单（§13 实施记录） |
| C-3 | 按 B-1 实现 | ✅ `((ts > 0 AND ts < ?) OR (ts <= 0 AND created_at < ?))` |
| C-4 | 修正 `makeRetentionStub` + 全套新语义测试 | ✅ stub 按表解构 / 按表谓词 + 绑定数校验；L1 `142 PASS`、L2 见 §13.3 |
| C-5 | 旧 `PASS` 不得冒充本轮 | ✅ 全部为本轮重跑输出（含时间戳）；任务单 §13.6 逐条声明 |
| C-6 | 生产只读 `EXPLAIN` | ✅ 仅 `EXPLAIN QUERY PLAN`；前后行数快照；逐行四列原文入档 |
| C-7 | 出现 `SCAN` / `ux_lh_dedup` 立即停 | ✅ **未出现**（本地 + 生产均命中 `ix_lh_device_ts`） |
| C-8 | 不改 `migrations/` / `wrangler.toml` / `src/index.js` / telemetry 谓词 | ✅ `git diff` 四项**全空** |
| C-9 | 不部署 / 不写生产 D1 / 不改 EMQX / 不重启设备 | ✅ 未执行 |
| C-10 | 不 `commit`/`push`/`reset`/`clean` | ✅ 未执行（工作区变更保留） |
| C-11 | 不解除四道门禁 | ✅ `BT-1` / `G-4` / `WP-5` / `STOP-R1` 状态未改 |

---

## §15 ★ 终审回填：`H-RET-4` 与正式交接（`2026-10-18` · **权威**）

> 本节回填人工对本授权链路的最终裁定。**冲突时以
> `docs/review/RET-1-Final-Read-Only-Acceptance.md` §9 为准**（本节为其镜像）。

### 15.1 终审结论（逐项）

| 验收维度 | 判定 | 依据与限制 |
|---|---|---|
| **实现范围** | **`ACCEPTED`** | 产品逻辑仅 `buildDeleteStatement()` + `runOneTable()` 调用点；**未发现授权范围外改动** |
| **`telemetry_history` 不回归** | **`ACCEPTED`** | 原谓词未变；实现 · L1 · L2 **三重证据** |
| **查询计划** | **`ACCEPTED`** | 本地与生产均 `ix_lh_device_ts`；**无 `SCAN` / 无 `ux_lh_dedup`** |
| **自动化测试** | **`ACCEPTED WITH KNOWN LIMITATIONS`** | L1 / 整套 / `dry-run` 通过；**`W7-G2` 仍 `FAIL`，不得改写为 `PASS`** |
| **生产删除效果** | **`NOT VERIFIED`** | `T-17` / `T-18` / `T-19` **未执行** |
| **生产问题修复** | **`NOT COMPLETED`** | **未部署**，生产仍运行旧谓词 |

**偏差裁定**：`+480/−10`（含 `retention.js` `+44/−4`）**已接受**——已分解为产品代码 / 注释 / 测试代码，
确认**无功能扩散**；**不再为注释与测试行数另行专项调查**。

**`W7-G2`**：**保留原始 `FAIL`**。现有证据**足以解释它为何可能受既有工作区状态影响**，
但**不足以证明其根因已完全解决**。

### 15.2 `H-RET-4` = **本轮不批准**（5 项前置，独立审批前必须闭合）

| # | 前置条件 |
|---|---|
| 1 | 明确**生产基线**与 `T-17` 的**精确预期值** |
| 2 | 明确**触发 `scheduled` 的方式**，以及**避免意外重复触发**的机制 |
| 3 | 确认**备份的实际保存位置、保留策略与可用性** |
| 4 | 定义**观察期判据**及**异常时的处置方式** |
| 5 | 明确 **`W7-G2` 是否构成下一阶段的阻断条件** |

> **★ 硬约束（人工原话要点）**：**备份与 Worker rollback 不应被等同于 D1 删除数据的可恢复性。**
> **`S3` 是会执行 `DELETE` 的步骤，必须独立授权**——**不能因为 `S1`、`S2` 已完成就自动执行 `S3`**。

### 15.3 正式交接

| 项 | 状态 |
|---|---|
| 实现与范围 | **`ACCEPTED`** |
| 已知限制 | **保留 `W7-G2` `FAIL` + `U-1…U-6`** |
| 生产部署与行为验收 | **`DEFERRED`** |
| **交接表述（唯一口径）** | **实现完成，未部署，生产风险仍在。** |

**生效纪律**：`RET-1` 专项**已结束**（不再主动扩展调查）· 后续开发继续推进 ·
❌ 不得把 `DEFERRED` 写成 `PASS`/`CLOSED`/已修复 · ★ **任何依赖生产 `ts<=0` 记录被保留的功能，
必须显式声明依赖并单独申请相关生产操作授权**。
