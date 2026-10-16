# P3 · `计划:WP-7` —— History Retention 实施任务单（`PLAN ONLY` · **未编码** · **未部署**）

> ## 🟡 `PLAN ONLY` —— 本文件**只是实施方案**，**不含任何生产变更**
>
> - **本文件不修改**：`src/**`（Worker / 固件）· `wrangler.toml` · `migrations/**` · D1 · EMQX 规则 / 连接器 / Action / ACL · 任何正式验收矩阵。
> - **本文件不执行**：`INSERT` / `UPDATE` / `DELETE`（对**任何**库，含本地）· 部署 · 重启 · `commit` / `push`。
> - **本文件性质**：把 **`DT:WP-3`（`FROZEN`）** 的**存储策略设计**翻译为**可执行、可验收、可回滚**的实施任务单。
>
> **状态**：`WP-7 = PLAN DRAFT`（**待人工审核**）· **`P3` 实施状态未变** · **未解除任何门禁**（`G-1` `HUMAN_REQUIRED` · `G-4` `BLOCKED` **均未变**）· ★ **`rev.2`**（`2026-10-16` · 采纳外部 review：**修正 D1 调用预算模型** + 新增预算隔离测试；详见 §11.0）  
> **冻结依据（**权威**）**：`docs/architecture/P3-WP3-History-Storage-Policy-Design.md`（**`FROZEN`** · `H-WP3-01…11` 已裁决）
> **上游约束**：`P3-WP1-Telemetry-Policy-Final.md`（`FROZEN`）· `P3-WP2-History-Data-Model-Design.md`（`FROZEN` · Schema Freeze · `G-3`）· `P3-DT-WP4-History-Ingestion-Design.md`（`FROZEN`）· `P3-History-D1-Plan.md`（`计划:WP-7`）  
> **实际 schema 依据**：`Cloudflare_Assets/guo-feeder-api/migrations/0003_p3_history_tables.sql`（**已上线** · 5 个冻结索引）  
> **本文件基线**：固件仓 `wb` @ **`4e61a9e`** · 云端仓 `main` @ **`f519c9e`** · 消息仓 `main` @ **`3165aab`**（**三仓 `== origin/main` · clean**）
>
> **⚠️ 术语铁律**：`PASS` · `FAIL` · `NOT RUN` · `BLOCKED` · `HUMAN_REQUIRED` · `DEFERRED`。**`NOT RUN` / `BLOCKED` / `HUMAN_REQUIRED` / `DEFERRED` 一律 ≠ `PASS`。**  
> **⚠️ 本文件不含任何 `PASS`** —— 所有验收项当前均为 `NOT RUN`。



---

## §0 本文件定位

| 项        | 内容                                                                                                                                                                                |
| -------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| **轨**    | **计划轨（Plan Track）** ⇒ 本文件凡引用**计划**条目一律写 **`计划:WP-n`**；凡引用**设计轨**条目一律写 **`DT:WP-n`**（**禁裸写 `WP-n`**，`DT:WP-3` §1）                                                                  |
| **编号**   | 本文件 = **`计划:WP-7`** 的实施任务单；其**设计来源** = **`DT:WP-3`**（`P3-WP3-History-Storage-Policy-Design.md`，`FROZEN`）                                                                          |
| **上位**   | `DT:WP-3` §4.2（方案 = **S-B 固定窗口 TTL**）· §5.1 / §6.1（窗口 **90 / 180 天**）· §7.3（机制 = **scheduled cleanup job**）· **§7.4（删除硬规则 `D-1…D-5`）** · §11.1（`H-WP3-01/02/03/08/10` = `CLOSED`） |
| **实施落点** | **仅云端仓** `Cloudflare_Assets/guo-feeder-api`（新增 `src/retention.js` · `src/index.js` 增 `scheduled()` · `wrangler.toml` 增 `[triggers]`）· **零固件 / 零 EMQX / 零 migration 改动**           |
| **生产变更** | **本文件 = 零**。**实施阶段**的生产变更 = **1 次 Worker 部署**（含 cron 注册）；**无 schema 变更、无 migration**                                                                                              |
| **门禁**   | **本文件不解除任何门禁** —— `G-1` `HUMAN_REQUIRED` · `G-4` `BLOCKED` 未变                                                                                                                     |

### 0.1 ★ 与 `DT:WP-3` 的双前置（**需人工确认口径**）

`DT:WP-3` §11.5 原文声明：

> **`计划:WP-7`（保留策略实施）**：⚠️ **仍受 `G-1` 批准 + `H-WP3-06`（payload 上限）未决 双重前置**

而**本轮任务指令**显式把 **`H-WP3-06` 列入"明确排除"清单**。二者口径不完全一致，处置如下（**透明记录，不擅自改冻结文档**）：

| 项                                     | 处置                                                                                                                                                                                                                             |
| ------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| **`G-1`**                             | ⚠️ **未变（`HUMAN_REQUIRED`）** —— 本文件为 **`PLAN DRAFT`**，其"审核通过"即构成 **`G-1` 的批准动作**（由人工执行）                                                                                                                                         |
| **`H-WP3-06`（`payload` 体积上限 / 生命周期）** | **按本轮指令排除** —— 依据：① 该问题属 **ingest 校验面（`计划:WP-4`）**，**不进删除谓词**；② retention 删除只依赖 `device_id + ts`（`D-3`），**与 `payload` 体积/是否置空完全解耦**；③ 其实际影响面 = **log 表体积估算与爆发风险（`CR-2`）**，**不影响 WP-7 的删除正确性**。⇒ **登记为残留缺口（§12 `Q-1`），不阻塞本任务单** |
| **`H-WP3-09`（per-device 配额）**         | `DEFERRED` —— **WP-7 不做**每设备硬顶（与本任务一致）                                                                                                                                                                                         |

> **★ 结论**：本任务单**按本轮指令执行**（`H-WP3-06` 排除），并**如实登记**该残留缺口与文档口径差异（§12 `Q-1`），**交由人工在批准 `G-1` 时一并确认**。

---

## §1 范围

### 1.1 IN（`计划:WP-7` 范围内 —— 恰 5 项）

| #     | 内容                                                   | 落点                                            |
| ----- | ---------------------------------------------------- | --------------------------------------------- |
| **1** | `telemetry_history` retention（**90 天**）              | `src/retention.js`                            |
| **2** | `log_history` retention（**180 天**）                   | `src/retention.js`                            |
| **3** | Cron `scheduled` handler                             | `src/index.js` + `wrangler.toml` `[triggers]` |
| **4** | 本地真 D1 测试夹具（`test/real_d1/`）+ 逻辑层套件（`test/current/`） | `test/**`                                     |
| **5** | 实施文档与状态回填                                            | 本文件 + `docs/README.md` / `HANDOFF.md`         |

### 1.2 OUT（**明确排除** —— 本 WP 不做）

| 项                                          | 归属 / 理由                                                                        |
| ------------------------------------------ | ------------------------------------------------------------------------------ |
| `mqtt_messages` 历史 `/log` **867 行噪音**      | 既有 schema 生命周期（`H-WP2-11` · `DEFERRED`）⇒ **须单独授权 + 备份**                        |
| **MQTT 密码轮换**（10 文件含明文）                    | 独立运维事项                                                                         |
| **`H-WP3-06`**（`payload` 上限 / 生命周期 / 超限处置） | 属 `计划:WP-4` ingest 校验面（§0.1）                                                   |
| **`H-WP3-09`**（per-device 配额硬顶）            | `DEFERRED`                                                                     |
| **ingest 校验逻辑**（`history-ingest.js`）       | `计划:WP-4`，已收口                                                                  |
| **EMQX 规则 / 连接器 / Action / ACL**           | 本 WP 零接触                                                                       |
| **固件 `src/`**                              | 本 WP 零接触                                                                       |
| **新增数据库 index**                            | **`H-WP3-10` = `no additional index`（`CLOSED`）**                               |
| **任何其他 schema 变更 / migration**             | `I-9` / `I-12` —— migration **止于 `0003_*`**，本 WP **不新建 `0004_*`**              |
| **聚合 / 降采样 / 归档 / 冷热分层**                   | `H-WP3-04` `DEFERRED` · `H-WP3-05` `CLOSED`（不实现归档）· `H-WP3-07` `CLOSED`（不引入分层） |
| **Web UI / 查询 API**                        | P4                                                                             |

### 1.3 不变量（**违反任一条 ⇒ 本任务单作废**）

`DT:WP-3` §3 的 `I-1…I-12` **全部适用**，实施时以下 5 条为**直接约束**：

| ID       | 不变量                                                                     | 对 WP-7 的直接含义                                            |
| -------- | ----------------------------------------------------------------------- | ------------------------------------------------------- |
| **I-1**  | Worker = ingest / validation / idempotency / persist；**清理不在 ingest 路径** | 删除**只**出现在 `scheduled()` 路径；`history-ingest.js` **零改动** |
| **I-6**  | telemetry 与 log **物理分离**（两表）                                            | 删除**分表**执行，**不 UNION / 不 JOIN**（`Q-8`）                  |
| **I-9**  | **schema 已冻结**（字段 / 类型 / **索引集合**）                                      | 删除**须适配**冻结索引；**不得**新增索引                                |
| **I-10** | 时间单位 = **Unix 秒**（`ts` / `created_at`）                                  | cutoff 计算全部用**秒**                                       |
| **I-12** | `0001_*` / `0002_*` / `0003_*` **不得编辑**；后续**新增** `000N_*`               | 本 WP **不新建 migration**                                  |

---

## §2 【A】Goals / Requirements

### 2.1 功能目标（**可判定**）

| #       | 目标                                                | 判据（概要，详见 §9）                          |
| ------- | ------------------------------------------------- | ------------------------------------- |
| **G-1** | `telemetry_history` 中 `ts` 超过 **90 天**的行被**逐步删除** | `W7-A1`                               |
| **G-2** | `log_history` 中 `ts` 超过 **180 天**的行被**逐步删除**      | `W7-A3`                               |
| **G-3** | 窗口**内**数据**一律不动**                                 | `W7-A2` / `W7-A4`                     |
| **G-4** | 由 **Cloudflare Cron Triggers** 定时驱动，**不依赖任何请求**   | `W7-D1` / `W7-F3`                     |
| **G-5** | 删除**不阻塞 ingest**、**不污染** ingest 指标                | `W7-G2`（`history-ingest.js` diff = 0） |
| **G-6** | 全程**不新增索引**、**不改冻结索引**                            | `W7-C10`                              |
| **G-7** | 行为**确定性**、**幂等**、**可中断续跑**                        | `W7-B1` / `W7-C5` / `W7-C6`           |
| **G-8** | 结果**可观测**（结构化摘要进日志）                               | `W7-D2`                               |

### 2.2 非目标（**Non-Goals** —— 明确不做，避免范围蔓延）

| #        | 非目标                       | 说明                                                                      |
| -------- | ------------------------- | ----------------------------------------------------------------------- |
| **NG-1** | **不做归档 / 备份**             | 超窗数据**直接删除、不可恢复**（`H-WP3-05` `CLOSED`）—— 见 §12 `Q-7`                    |
| **NG-2** | **不做聚合 / 降采样**            | `H-WP3-04` `DEFERRED`；P3 无聚合表                                           |
| **NG-3** | **不做 per-device 配额**      | `H-WP3-09` `DEFERRED`                                                   |
| **NG-4** | **不做 `payload` 截断 / 置空**  | `H-WP3-06` `DEFERRED`（本 WP 排除）；`payload` **与行同寿命**（`DT:WP-3` §6.3 设计倾向） |
| **NG-5** | **不改 ingest 路径**          | `I-1`：**不**在 `/api/history/ingest` 内顺带删除（`DT:WP-3` §7.2 方案 B **已否决**）   |
| **NG-6** | **不做惰性删除**                | 方案 C **已否决**（不约束冷设备）                                                    |
| **NG-7** | **不做告警设施 / 监控面板**         | `DT:WP-3` §9.4「增长监控 / 告警 = 延后续阶段」⇒ 本 WP **仅日志**                         |
| **NG-8** | **不做 VACUUM / 空间回收**      | D1 侧无用户可控 VACUUM；删除后空间由平台管理                                             |
| **NG-9** | **不做 `created_at` 维度的清理** | 删除**只用 `ts`**（观测时刻）；`created_at` **不参与谓词**（见 §3.2）                      |

---

## §3 【B】Retention semantics（**语义冻结**）

### 3.1 cutoff 计算（**唯一公式**）

```
nowSec   = floor(controller.scheduledTime / 1000)          # Cron 触发时刻（Unix 秒）
cutoff   = nowSec - RETENTION_DAYS[table] * 86400          # 秒
谓词      : ts < cutoff                                     # ★ 严格小于
```

| 项                                      | 冻结取值                                               | 依据                                               |
| -------------------------------------- | -------------------------------------------------- | ------------------------------------------------ |
| **`RETENTION_DAYS.telemetry_history`** | **90**                                             | `H-WP3-01` = `CLOSED`                            |
| **`RETENTION_DAYS.log_history`**       | **180**                                            | `H-WP3-02` = `CLOSED`（级别 = **L-A 统一窗口**）         |
| **秒/天**                                | **86400**（固定）                                      | UTC **无 DST** ⇒ 无夏令时跳变；**不使用** `24*3600` 之外的历法计算 |
| **基准时刻**                               | **`controller.scheduledTime`**（**非** `Date.now()`） | ★ 见 §3.4（**确定性 + 可测性**）                          |

> **★ 为什么固定 86400 而非"日历天"**：`ts` 是 Unix 秒（`I-10`），窗口以**物理秒跨度**表达 ⇒ **确定性**、**与执行时刻无关的算术**、**无时区依赖**。`90 * 86400 = 7,776,000 s` · `180 * 86400 = 15,552,000 s`。

### 3.2 边界语义（**冻结**）

| 情形                             | 行为                | 理由                                                                                                   |
| ------------------------------ | ----------------- | ---------------------------------------------------------------------------------------------------- |
| `ts == cutoff`                 | **保留**（不删）        | 谓词为**严格** `ts < cutoff` ⇒ 窗口 = **`[cutoff, +∞)`**（**闭下界**）。**INCLUSIVE KEEP**                        |
| `ts == cutoff - 1`             | **删除**            | 严格小于                                                                                                 |
| `ts > nowSec`（**未来时间**）        | **保留**            | `cutoff <= nowSec < ts` ⇒ **数学上不可能满足** `ts < cutoff` ⇒ **未来数据天然受保护**，**无需特判**                        |
| `ts == nowSec`                 | **保留**            | 同上                                                                                                   |
| **`ts IS NULL`**               | **不可能发生**         | `ts INTEGER NOT NULL`（`0003_*` 冻结 schema）⇒ **由 schema 排除**，**不加防御性分支**                               |
| `ts <= 0`（含 `ts = 0`）          | **按数值参与比较 ⇒ 被删除** | `0 < cutoff`（cutoff 为当下的正数）⇒ **会被删**。★ 此为**明示语义**，见 §12 `Q-2`（**待人工确认或接受**）                          |
| `ts` 为**极大值**（如 `2^31-1` / 远期） | **保留**            | 同"未来时间"                                                                                              |
| **`created_at`**               | **不参与谓词**         | 写入时刻与观测时刻**分离**（`WP2:§3.1`）⇒ 设备时钟漂移导致 `ts` 远过去时，该行**按 `ts` 判超窗**（**符合 `DT:WP-3` §5.1「按 `ts` 窗口保留」**） |

> **★ 严格 `<` 的取舍论证**：采用严格小于 ⇒ 窗口是"恰好 90 天"的**闭区间下界**；若采用 `<=`，则 `ts == cutoff` 被删，窗口实际为 90 天**减 1 秒**。**严格小于 = 窗口不小于设计值**（更保守，且不牺牲任何设计意图）。  
> **★ 可重入性**：谓词**只有 `device_id`（等值）+ `ts`（范围）**，**无 `now()` 竞态、无 `rowid`、无随机**（`D-3`）⇒ **同一 `cutoff` 输入 ⇒ 同一结果**。

### 3.3 90 / 180 天的**精确定义**（一句话）

> **`telemetry_history` 保留「`ts >= scheduledTime - 7,776,000`」的行；`log_history` 保留「`ts >= scheduledTime - 15,552,000`」的行。其余（`ts` 更早者）为删除候选。**

### 3.4 ★ 基准时刻 = `scheduledTime`（**关键设计决策**）

| 候选                             | 评价                                                                                                                                      |
| ------------------------------ | --------------------------------------------------------------------------------------------------------------------------------------- |
| **`Date.now()`**               | ❌ **不确定** —— 同一 cron 事件在**重试**或**手工触发**时取值不同 ⇒ 边界行为不稳定；本地无法精确复现                                                                         |
| **`controller.scheduledTime`** | ✅ **采用** —— ① 由平台给出，**同一 cron 事件固定**；② 本地测试端点支持 **`?time=<ms>` 覆盖**（**已实测**，见 §7.4）⇒ **边界可精确断言**；③ 重试时 `scheduledTime` **不变** ⇒ 幂等的必要条件 |

> **⇒ 因此**：**cutoff 完全由 `scheduledTime` 决定**，Handler **不得**读取 `Date.now()`。

---

## §4 【C】Batch deletion design

### 4.1 硬规则映射（`DT:WP-3` §7.4 —— 逐条落实）

| 规则                               | 本设计的落实                                                                                                                                                |
| -------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------- |
| **D-1 必须分批**                     | 每批 `LIMIT 1000`（§4.4）；**单条 DELETE 影响行数 ≤ 1000**                                                                                                       |
| **D-2 禁大事务、可中断续跑**               | **每批 = 一条独立 D1 语句**（隐式事务、自动提交）⇒ **无跨批长事务**；谓词纯 `ts` ⇒ **中断后重跑即续跑**                                                                                    |
| **D-3 谓词只依赖 `device_id` + `ts`** | 全部 DELETE 谓词 = `device_id = ? AND ts < ?`（+ 批限）—— **无 `rowid` / `now()` / 随机 / 顺序依赖**；**最终收敛状态唯一**                                                    |
| **D-4 不依赖不存在的索引**                | 两表**均无单列 `ts` 索引** ⇒ **绝不**写 `DELETE ... WHERE ts < ?`（会全表扫描）；**一律带 `device_id` 前缀**。**已 `EXPLAIN` 取证命中 `ix_th_device_ts` / `ix_lh_device_ts`**（§4.3） |
| **D-5 不改 WP-2 冻结索引**             | **不新增 / 不删除 / 不修改任何索引**；**无 migration**；`W7-C10` 断言索引集合保持不变                                                                                           |

### 4.2 `device_id` 集合的获取

```sql
SELECT DISTINCT device_id FROM <table> ORDER BY device_id;
```

| 决策                       | 内容                                                                       | 理由                                                                                              |
| ------------------------ | ------------------------------------------------------------------------ | ----------------------------------------------------------------------------------------------- |
| **来源 = 历史表自身**           | **不**从 `device` 表取                                                       | 历史行**允许先于 `device` 建档**（`DT:WP-4` §2.6 `ID-4`：未知设备不视为错误）⇒ 从 `device` 表取会**漏掉**这些设备 ⇒ **残留永不清理** |
| **`ORDER BY device_id`** | **顺序固定** ⇒ **可复现**（★ 但"确定性"**不依赖**顺序 —— 见 §4.7） | 顺序只影响"本批先取候选集的哪一部分"，**不影响最终删除集合**；固定顺序便于复现与日志比对 |
| **成本**                   | 走 `ix_th_device_ts` / `ix_lh_device_ts`（首列即 `device_id`）⇒ **索引扫描**，非全表扫描 | `EXPLAIN` 取证见 §4.3；返回行数 = **设备数**（非行数）⇒ 反序列化开销可控（11 台 = 11 行）                                   |
| **不设 `LIMIT`**           | 全量枚举                                                                     | 若加 `LIMIT` ⇒ 尾部设备**永不被清理**。11 台规模成本可忽略；规模化见 §4.7 公平性                                            |
| **`UNION` 两表设备集合？**      | ❌ **不做**                                                                 | 两表**独立处理**（`I-6` / `Q-8`：不 UNION / 不 JOIN）；各自枚举                                                 |

### 4.3 单批 DELETE SQL（**主形态 + 兜底形态**）

**★ 主形态 A（默认）**：

```sql
DELETE FROM <table> WHERE device_id = ? AND ts < ? LIMIT ?;
```

| 项              | 内容                                                                                                                                                                          |
| -------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| **绑参**         | `?1 = device_id` · `?2 = cutoff` · `?3 = BATCH_SIZE`（1000）                                                                                                                  |
| **语法可用性**      | **已用 `EXPLAIN` 实测**：本地 D1 引擎（workerd 内 SQLite，与生产 D1 同一族）**接受**该语法，生成 47 条 VDBE 指令（含 `Delete` + 3 条 `IdxDelete`：`ux_th_dedup` / `ix_th_device_type_ts` / `ix_th_device_ts`） |
| **索引命中**       | `EXPLAIN` 计划**引用 `ix_th_device_ts`**（`log_history` 引用 `ix_lh_device_ts`）⇒ **`D-4` 满足**（命中**实际存在**的索引；**未**依赖 `ts` 单列索引）                                                     |
| **`LIMIT` 语义** | **最多**影响 1000 行；**不带 `ORDER BY`** ⇒ **不保证**是哪 1000 行。★ 但**谓词集合恒定**，循环至 `changes == 0` ⇒ **最终状态唯一**（`D-3` 的实质=结果可重复，**满足**）                                                  |
| **影响行数读取**     | `meta.changes`（D1 返回）⇒ **不需要**额外 `SELECT COUNT(*)`（省 subrequest）                                                                                                            |
| **SQL 长度**     | 远小于 **100 KB** 上限（D1 限制）                                                                                                                                                    |
| **绑参数量**       | 3 ≤ **100**（D1 限制）                                                                                                                                                          |


**☆ 兜底形态 B（仅当生产拒绝形态 A 时启用）**：

```sql
DELETE FROM <table> WHERE device_id = ? AND ts < ?
  AND rowid IN (SELECT rowid FROM <table> WHERE device_id = ? AND ts < ? LIMIT ?);
```

| 项 | 内容 |
|---|---|
| **存在理由** | 形态 A 的 `DELETE … LIMIT` 在标准 SQLite 中需编译选项 `SQLITE_ENABLE_UPDATE_DELETE_LIMIT`；**本地引擎已接受**，但**生产 D1 服务端的编译选项不可经 API 读取**（实测 `PRAGMA compile_options` ⇒ `SQLITE_AUTH`；`sqlite_version()` ⇒ `not authorized`）⇒ **保守起见预置等价兜底** |
| **等价性** | 子查询**只依赖 `device_id + ts`**；`rowid` 仅作**行的物理定位句柄**，**不参与判定哪些行"超窗"** ⇒ 循环至 `changes == 0` 后**最终状态与形态 A 完全一致** |
| **`D-3` 论证** | 「删除**候选集**」由 `device_id + ts` 唯一确定；`LIMIT` 只决定**本批取哪一部分候选**（`D-1` 的批处理要求）⇒ **不违反** `D-3` 的实质（**结果确定性 / 可重复**） |
| **切换规则** | **只有**当形态 A 在生产实测（§8.2 `PRE-3`）报语法/权限错误时，才切形态 B，并在实施记录中**留痕**（**不得**静默切换） |
| **★ 强制附加验证（**B 被启用时**）** | 形态 B **一旦启用**，**必须**对其执行 `EXPLAIN QUERY PLAN` 并**留证**：确认 (a) 子查询**命中** `ix_th_device_ts` / `ix_lh_device_ts`，(b) **未**退化成大范围 `SCAN <table>`，(c) 外层 `rowid` 定位走 PK/rowid。**不通过 ⇒ `S-2`（STOP）** | ⚠️ 本轮**已对 A 取证**，但**未对 B 取证** ⇒ B 的取证是**启用前置条件**，不是"可选" |

### 4.4 `LIMIT 1000` 的依据（**batch size = 1000**）

| # | 依据 | 出处 |
|---|---|---|
| **1** | **Cloudflare 官方 D1 文档原文**：*"Data migrations like a large `UPDATE` or `DELETE` affecting millions of rows must be run in batches. A single query that attempts to modify hundreds of thousands of rows or hundreds of MBs of data at once will exceed execution limits. Break the work into smaller chunks (e.g., **processing 1,000 rows at a time**) to stay within platform limits."* | D1 Limits 文档 · "Query performance" |
| **2** | `H-WP3-08` = `CLOSED`：**冻结「原则」（`D-1…D-5`），不冻结具体 batch size** ⇒ batch size 属**实施期可调参数** | `DT:WP-3` §7.4 / §11.1 |
| **3** | 单批行数越少 ⇒ 事务越小、锁窗口越短、wall-time 越短（`D-1` / `D-2` 更稳）；1000 是**官方示例值**，平衡"批数 × 调用开销" | — |

> **★ 可调性声明（**本任务单的要求**）**：`BATCH_SIZE` **可调**（见 §4.6），**但调整须满足**：① 满足 `D-1`（分批）② 使**全局 D1 调用预算**（§4.5）不超 **subrequest 上限** ③ 调整须在实施记录中留痕。**默认实施基准 = 1000**（本轮批准值）。

### 4.5 推进算法（**轮转制 · Round-Robin**）

```text
calls = 0                                                   # 全局 D1 调用计数（含 SELECT）
for table in [telemetry_history, log_history]:              # 顺序固定（见 §4.6 预算独立 ⇒ 顺序不影响分配）
    cutoff = nowSec - RETENTION_DAYS[table] * 86400
    devices = SELECT DISTINCT device_id FROM table ORDER BY device_id    # ← 1 次 D1 调用
    calls += 1
    tableCalls = 0                                          # ★ 本表 DELETE 调用计数（**不含**上面那次 SELECT）
    tableTruncated = false
    if devices 为空: 记录空表；continue
    start = floor(nowSec / 86400) mod len(devices)           # ★ 跨 invocation 公平起点偏移
    order = rotate(devices, start)
    done  = {}                                              # 本表已删净的设备
    for round in 1..MAX_ROUNDS_PER_TABLE:                   # ★ 仅"死循环保护"（**不是**吞吐预算）
        progressed = false
        for d in order:
            if d in done: continue
            if tableCalls >= MAX_CALLS_PER_TABLE:            # ★ **每表 DELETE 预算 = 吞吐预算**
                tableTruncated = true; break
            if calls >= HARD_STOP_CALLS:                     # ★ 防御性保险丝（正常路径**不可达**）
                tableTruncated = true; break
            n = DELETE FROM table WHERE device_id = d AND ts < cutoff LIMIT BATCH_SIZE
            calls += 1; tableCalls += 1
            if n < BATCH_SIZE: done.add(d)                  # 该设备已删净（本批未满 ⇒ 无更多超窗行）
            if n > 0: progressed = true
        if tableTruncated or not progressed: break           # 预算耗尽 / 全表无进展 ⇒ 收敛退出
```

> **★ 关键区分（**本任务单的核心修正点**）**：`MAX_ROUNDS_PER_TABLE` 是**轮数**（外层循环上限）；
> **真正决定吞吐与预算的是 `MAX_CALLS_PER_TABLE`（= 每表 **DELETE 调用次数** 上限）**。
> 若把"轮数"误当作"调用次数"，则 20 轮 × N 设备 = **最多 20×N 次 DELETE** ⇒ **预算模型失效**。
> 本设计**以调用计数为准**（`tableCalls` / `calls` 双计数器），**轮数只防死循环**。

| 设计点 | 取值 / 机制 | 依据 |
|---|---|---|
| **多 device 推进** | **轮转**：每轮遍历设备列表，**每设备每轮最多 1 批** | ★ **精确表述**：**单次 invocation 内**，单设备最多获得 1 个 batch / round；**跨 invocation** 由每日起点偏移避免"固定设备长期占据预算前段位置"。**不宣称"天然公平"**（在预算受限时，单次 invocation 内**只有前 N 个设备**能获得预算，见下方注） |
| **避免永久卡在某个 `device_id`** | ① **轮转**（单设备每轮 ≤ 1 批）② **起点偏移** `floor(nowSec/86400) mod N`（**跨天起点不同**）③ **每表 DELETE 调用上限**（单表**无法**独占全局/另一表预算） | 三条机制共同保证；起点偏移在 **11 台规模非必需**，为**规模化预留** |
| **收敛判定** | `n < BATCH_SIZE` ⇒ 该设备**本轮已知**删净 ⇒ 加入 `done` 跳过后续轮 | 省 D1 调用（**关键**：无积压设备只花 1 次调用） |
| **提前退出** | 整轮 `progressed == false` ⇒ 收敛 ⇒ 提前结束该表 | 省调用与 wall time |
| **调用预算（**两层，**不可混为**一层）** | ① **每表**：`tableCalls >= MAX_CALLS_PER_TABLE`（20）⇒ `tableTruncated = true` ② **全局保险丝**：`calls >= HARD_STOP_CALLS`（45）⇒ 立即停止 | **吞吐由 ① 决定**（§4.6 不变量 `I-P1`/`I-P2`）；**② 正常路径不可达**，纯防御（`I-P4`） |
| **表顺序** | `telemetry_history` → `log_history`（**固定**） | **两表 DELETE 预算各自独立**（§4.6 `I-P2`）⇒ 顺序**不产生**预算优先权（★ 这是本轮修正的关键结论） |
| **并发** | Cron **每日 1 次** ⇒ 正常**无**并发；D1 单库**串行**处理查询 ⇒ 即便手动触发与 cron 撞车，**同谓词 DELETE 相互不冲突**（各自删已删行 ⇒ `changes` 变小），**最终收敛** | `DT:WP-3` §5.2 ⑤（幂等 / 可重入） |

> **★ 公平性注（**措辞精确化 · 采纳 review**）**：**不再宣称"天然公平 / 单设备无法独占预算"**。
> 准确表述为：**单次 invocation 内，单设备最多获得 1 个 batch/round**（防**单设备独占**）；**跨 invocation 由每日起点偏移**（`floor(nowSec/86400) mod N`）**避免固定设备长期获得预算前段位置**（防**饿死**）。
> ⚠️ 当预算受限且设备数 > 预算批数时，**单次 invocation 内只有排序靠前的部分设备被处理**，其余设备**本次不处理**、**次日**因起点偏移而**换到前列** ⇒ **跨 invocation 渐进覆盖**。**11 台规模**下两者均**非瓶颈**。

### 4.6 调用预算（**★ 由平台限制倒推 —— 本设计的核心约束**）

| 平台限制 | 值（**Workers Free**） | 出处 |
|---|---|---|
| **Subrequests per invocation** | **50** | Workers Limits |
| **Queries per Worker invocation（D1）** | **50**（Free）/ 1000（Paid） | D1 Limits |
| **CPU time per Cron Trigger** | **10 ms**（Free）/ 30 s（Paid，< 1 h 间隔） | Workers Limits |
| **Duration（wall time）per Cron Trigger** | **15 min** | Workers Limits |
| **Cron Triggers per account** | **5**（Free） | Workers Limits |
| **D1 max SQL query duration** | **30 s / 单语句** | D1 Limits |
| **D1 同时连接数** | 6 / invocation | D1 Limits |
| ★ **D1 查询等待不计 CPU** | *"Waiting on network requests (such as `fetch()` calls, KV reads, or **database queries**) does **not** count toward CPU time."* ⇒ **删除耗时主体（D1 I/O）不占 10 ms CPU 预算** | Workers Limits |

**预算表（默认参数）**：

| 项 | 每次 invocation 上限 | 说明 |
|---|---|---|
| `SELECT DISTINCT device_id` | **2**（每表 1 次） | **固定**开销（不随设备数/行数变化） |
| **`DELETE` 调用 · `telemetry_history`** | **≤ 20** | `MAX_CALLS_PER_TABLE = 20` ⇒ ★ **这才是吞吐预算** |
| **`DELETE` 调用 · `log_history`** | **≤ 20** | 同上 · **与 telemetry 完全独立**（`I-P2`） |
| **正常路径理论上限** | **42** = `2 + 20 + 20` | ≤ **50**（Free subrequest）⇒ **余量 8** |
| **全局防御性硬闸** | **45** = `HARD_STOP_CALLS` | ★ **正常路径不可达**（42 < 45）· **纯保险丝**（`I-P4`） |

> **★ 为什么"每表独立预算"而不用共享池**：本轮验收要求「**两张表互不影响**」（`W7-C7` / `W7-C11`）。
> **`MAX_CALLS_PER_TABLE` 使该要求同时成立于数据面与预算面** —— ⚠️ **此结论成立的前提**是"每表各自的 DELETE 调用计数独立"（`I-P2`），**而非**"全局 45 次先到先得 + 固定表顺序"。
> **⇒ 本轮修正**：旧稿以"全局 45 + 固定顺序"控制 ⇒ telemetry 具**事实上的预算优先权** ⇒ 与"两表独立预算"**自相矛盾**。**现改为 per-table 计数**（`tableCalls`）⇒ 矛盾消解。
>
> **★ 吞吐校核（11 台 · 稳态）**：`telemetry_history` 每日新增 ≈ 11 × 1200 = **13,200 行/天**（上限估算）⇒ 每日需删的"越过 90 天者"同样 ≈ **13,200 行** ⇒ 预算 = **20 次 DELETE × 1000 行 = 20,000 行/天** ⇒ **余量 ≈ 51%**。
> ★ **该计算成立的前提**：20 = **每表每天最多 20 次 DELETE**（**不是** 20 轮）。`log_history` 需求**远小**（普通情况 ≈ 30 行/设备/天）⇒ 20,000 行/天**充裕**。
> **⚠️ 积压收敛（首跑）**：若窗口从"未清理"状态切换到"已清理"，**首次**需删历史积压（当前 D1 实际行数极小，见 §8.1 `PRE-1`）⇒ 单日预算可能不足 ⇒ **截断（`truncated=true`）为设计内行为**，**次日续跑直至收敛**（`W7-C4` / `W7-C5`）。
> **⚠️ CPU 10 ms**：删除**主体为 D1 I/O（不计 CPU）**；JS 侧仅"循环控制 + 读取 `meta.changes` + 拼装摘要" ⇒ 42 次调用的 JS 开销远小于 10 ms。**仍须**：① 不在 handler 内做字符串大拼接（用数组 + 末尾 `JSON.stringify`）② 摘要**只含计数**（不含行数据）。

### 4.6.1 ★ D1 调用预算不变量（**实现必须满足 —— 本节为冻结口径**）

```text
SELECT calls          = 2                                     # 每表 1 次（固定）
DELETE calls / table  <= MAX_CALLS_PER_TABLE        = 20      # ★ 吞吐预算（per-table）
DELETE calls total    <= 20 + 20                    = 40
Total D1 calls        <= 2 + 40                     = 42      # ★ 正常路径理论上限
HARD_STOP_CALLS       = 45                                    # ★ 防御性保险丝（正常不可达）
42 <= 50 (Free subrequest limit)                              # 余量 = 8
```

| ID | 不变量 | 违反后果 |
|---|---|---|
| **I-P1** | **单表 `DELETE` 调用 ≤ `MAX_CALLS_PER_TABLE`（20）** | 单表吞吐失控 |
| **I-P2** | **两表 `DELETE` 预算相互独立**（各 ≤ 20）⇒ **一表不得耗尽另一表预算** | `W7-C7` / **`W7-C11`** 失败（"两表互不影响"在预算面不成立） |
| **I-P3** | **正常路径 `Total D1 calls ≤ 42`** | 实现与冻结预算模型不一致 |
| **I-P4** | **`HARD_STOP_CALLS`（45）在正常路径下不得被触发**；一旦触发 ⇒ **立即 `truncated` + 记日志**，视为**实现缺陷信号** | 触发即说明 `I-P1` / `I-P2` 失效 |
| **I-P5** | **`MAX_CALLS_PER_TABLE`（吞吐预算）与 `MAX_ROUNDS_PER_TABLE`（死循环保护）是两个不同概念**，**不得互相替代** | ★ **本轮修正的根因**（旧稿把 20 轮误当 20 次调用 ⇒ 实际最坏 20×N 次） |
| **I-P6** | **参数自洽**：`2 + 2 × MAX_CALLS_PER_TABLE <= HARD_STOP_CALLS`（默认 `42 <= 45` ✅）；**`MAX_TOTAL_CALLS` 由代码派生，不得独立配置** | 放任冲突 ⇒ 吞吐预算与保险丝**互相矛盾**（如 `MAX_CALLS_PER_TABLE=100` ⇒ 派生 `202 ≫ 45`） |

> **★ `HARD_STOP_CALLS` 的定位（**采纳 review 意见**）**：它**不是**正常吞吐预算，**纯粹**是"**最后一层防御性安全闸 / 保险丝**"。
> **正常算法在数学上永远到不了 45**（42 < 45）⇒ **若被触发，说明实现偏离了冻结预算模型**（典型 = `I-P5` 的违反，即把"轮数"当成"调用次数"）⇒ **应立即 `truncated` 并记录**，且在实施记录中**作为缺陷调查线索**。
> **⇒ 因而**：`HARD_STOP_CALLS` **不是经济参数、不需生产调优**；生产 `[vars]` **无需设置**（§6.5），其值由代码常量给出。

### 4.7 重入 / 幂等 / 续跑（**性质说明**）

| 性质 | 成立理由 |
|---|---|
| **幂等** | 谓词只由 `device_id + ts(< cutoff)` 决定 ⇒ **重复执行同一 `cutoff`**：首次删净后，后续调用 `changes` **全为 0** |
| **可中断** | 每批独立提交 ⇒ 任意时刻中断（wall time / 预算 / 平台回收）**不产生半完成事务** |
| **可续跑** | 下次 invocation **重新枚举设备 + 重新删除剩余超窗行** ⇒ 无需持久游标 |
| **确定性（★ 措辞收紧 · 采纳 review）** | **最终删除集合**由**谓词集合** `{ device_id ∈ D ∧ ts < cutoff }` **唯一确定**，**与"`LIMIT` 本批选中了哪一部分候选"无关**。在**相同数据集合 + 相同 `scheduledTime`** 下，`cutoff` 与设备排序确定。⚠️ **不宣称"同一 `scheduledTime` ⇒ 同一设备顺序"** —— 两次执行之间设备集合可变（新设备出现 / 设备被删）⇒ **设备顺序可能不同**，但**收敛终态仍由谓词唯一决定**。 |
| **无状态** | **不引入**任何进度表 / KV / 游标 ⇒ **零 schema 变更**（`I-9` / `I-12` 满足） |

---

## §5 【D】Cron design

### 5.1 入口与调度

| 项 | 内容 |
|---|---|
| **Handler** | `src/index.js` 的 `export default { async fetch(...), async scheduled(controller, env, ctx) }` —— **`fetch` 不动，仅新增 `scheduled`** |
| **实现落点** | **新增** `src/retention.js`（导出 `RETENTION_POLICY` / `computeCutoffSec` / `runRetention`）；`scheduled` 内**只做**调用 + 顶层错误捕获（**≤ 10 行**） |
| **调度** | `wrangler.toml` 新增：`[triggers]` + `crons = ["0 3 * * *"]` —— **UTC 每日 03:00**（**已批准值**） |
| **时区** | Cron **以 UTC 执行**（Cloudflare 文档明示）⇒ **与 `scheduledTime`（epoch ms）一致**，无本地时区依赖 |
| **`controller` 字段** | `cron`（**已实测**：`"0 3 * * *"`）· `scheduledTime`（epoch **ms**，**已实测**可被 `?time=` 覆盖）· `type`（**本地实测 = `undefined`** ⇒ **不得**依赖 `type` 分支）· `noRetry()`（**不使用**，见 §5.3） |
| **`ctx.waitUntil`** | **不使用** —— handler 内 `await` 全部工作；`waitUntil` 仅能在响应后延长 30 s，本场景**无收益**（Cron 的 wall time 上限是 **15 min**） |
| **返回值** | 返回结构化摘要对象（**本地 `?format=json` 可直接读到**，`W7-D1`） |

### 5.2 摘要结构（**可观测性** —— `G-8` / `W7-D2`）

```jsonc
{
  "ok": true,                      // 是否无致命错误
  "nowSec": 1792132800,            // = floor(scheduledTime/1000)
  "cron": "0 3 * * *",
  "batchSize": 1000,
  "days": { "telemetry_history": 90, "log_history": 180 },  // ★ 实际生效窗口（防"静默偏离冻结窗口"）
  "maxCallsPerTable": 20,          // ★ 吞吐预算（生效值）
  "hardStop": 45,                  // ★ 防御性保险丝（只读展示 ⇒ 便于事后判定是否被触发）
  "truncated": false,              // 任一张表被截断 ⇒ true
  "calls": 23,                     // ★ 全局 D1 调用数（含 2 次 SELECT）· 正常应 ≤ 42
  "tables": [
    { "table": "telemetry_history", "days": 90, "cutoff": 1784356800,
      "devices": 1, "deleted": 1200,
      "batches": 2,                // = 本表 DELETE 调用次数（≤ maxCallsPerTable）
      "calls": 3,                  // ★ = 1 SELECT + 2 DELETE（不变量 I-P1 的直接证据）
      "truncated": false, "converged": true },
    { "table": "log_history", "days": 180, "cutoff": 1776571200,
      "devices": 1, "deleted": 0,
      "batches": 0,
      "calls": 1,                  // ★ = 1 SELECT + 0 DELETE
      "truncated": false, "converged": true }
  ],
  "errors": [],                    // 上限 16 条（见 §5.3）
  "errorsTruncated": false         // ★ 超出上限时为 true
}
```

> **★ 摘要只含计数与标识，不含任何行内容** ⇒ 无隐私面、无大字符串、CPU/内存友好。
> **★ 摘要必须包含**：`days`（实际生效窗口）· `maxCallsPerTable` · `hardStop` · 每表 `calls`（= SELECT + DELETE 调用数）⇒ **足以事后自证** `I-P1` / `I-P2` / `I-P3` / `I-P4` 是否成立（**无需**额外查询）。
> **★ `errors[]` 上限 16 条** + `errorsTruncated` 标志（见上表）。

### 5.3 异常处理（**冻结**）

| 层 | 策略 | 理由 |
|---|---|---|
| **表级隔离** | 每表**独立** `try/catch` ⇒ 一表失败**不阻断**另一表 | `W7-D3`；两表**互不影响** |
| **设备级隔离** | 单设备 DELETE 失败 ⇒ 记入 `errors[]`，**跳过该设备**继续下一个（**不重试**） | 防单点故障拖垮整轮；下轮/次日自然重试 |
| **`errors[]` 长度上限（★ 采纳 review · 增强项）** | 最多 **16** 条；超出 ⇒ **停止追加**并置 `errorsTruncated = true` | 防"设备数扩大 + 批量失败"时摘要膨胀（仍满足"不含行数据"，但**条数**此前无界） |
| **不抛出（不触发 CF 重试）** | Handler **捕获全部异常**，返回 `{ok:false, errors:[...]}`，**不 rethrow** | `DT:WP-3` §7.3：**删除失败不得影响 ingest / 查询；只记日志 / 告警（不重试风暴）** ⇒ **主动放弃**平台自动重试 |
| **`noRetry()`** | **不调用** | 语义等价于"不抛出"；**不引入**额外平台语义依赖 |
| **日志** | `console.log(JSON.stringify(summary))`（**1 行**） | 与仓库既有风格一致（`history-ingest.js` 用 `console.error` 打点）；`wrangler tail` 可读 |
| **告警** | **无**（`NG-7`） | 设计 §9.4：监控/告警延后续阶段。**如实声明**：失败**仅**在日志可见 |

---

## §6 【E】Testing

### 6.1 两层（**沿用仓库既有分层，两类结论不得混用**）

| 层 | 路径 | 引擎 | 可声称的结论 |
|---|---|---|---|
| **L1 逻辑层** | `test/current/retention_policy.test.mjs`（**新增**） | `test/lib/mock_d1.mjs`（**内存替身**） | 仅**逻辑/契约**结论；**不得**冒充数据库验证 |
| **L2 真实 D1 层** | `test/real_d1/retention_real_d1.sh`（**新增**，沿用 `history_ingest_real_d1.sh` / `v37a_log_reject_matrix.sh` 范式） | **本地 workerd 内真实 D1**（`wrangler dev --local` + `wrangler d1 execute --local`） | **PASS（真实 D1 验证）** |

> **★ 与既有套件的关系**：同层、同约定、**独立 persist 目录与端口**（`.wrangler/wp7_retention/`· 端口 `8798`）⇒ **互不干扰**；L2 脚本**绝不** `--remote`（脚本内自救断言，沿用 `v37a` 做法）。

### 6.2 隔离证明（**脚本内自动断言**）

| # | 断言 | 方式 |
|---|---|---|
| **1** | 本地 persist 位于 `Cloudflare_Assets/.wrangler/`（**已 gitignore**） | `git check-ignore` 通过 |
| **2** | **零远端调用** | 脚本内自检 `grep` ⇒ 命中数 = **0**（沿用 `v37a` 的反自检技巧） |
| **3** | 测试设备身份 = **合成 12-hex**（**非**生产 `288485896ce4`） | 变量逐字比对 |
| **4** | 本地库**无** `mqtt_messages` 表（`0001–0003` 只建 device/credential/binding/event/history） | `sqlite_master` 查询 |
| **5** | 索引集合与 `0003_*` **逐字一致（恰 5 个）** | `sqlite_master` 查询（`W7-C10`） |

### 6.3 数据生成技巧（**性能**）

> ⚠️ `wrangler d1 execute --local` **每次调用都要起 workerd**（约 1–2 s）⇒ **逐行 INSERT 不可行**。
> ✅ **用递归 CTE 一条语句批量生成**：

```sql
INSERT INTO telemetry_history (device_id, ts, type, value, seq, schema_version)
WITH RECURSIVE gen(i) AS (SELECT 1 UNION ALL SELECT i+1 FROM gen WHERE i < 2500)
SELECT '0a1b2c3d4e5f',
       :base_ts - i,                       -- 可控超窗/窗内
       'weight.value', '1', i, 1
FROM gen;
```

（`log_history` 同理；`boot_seq` / `seq` 用 `i` 保证 `ux_lh_dedup` 不撞。）

### 6.4 测试矩阵（**覆盖任务要求的 12 项 + 预算 / 隔离 / 契约补充项**）

> 编号 = **`W7-*`**（前缀未在仓内使用，**无冲突**）。

| ID | 用例 | 层 | 构造 / 动作 | 断言 |
|---|---|---|---|---|
| **W7-A1** | **超窗数据删除**（telemetry） | L2 | `ts = cutoff_ref - k`（k ≥ 1）注入 N 行 | 该集合行数 ⇒ **0** |
| **W7-A2** | **窗内数据不删除**（telemetry） | L2 | `ts >= cutoff_ref` 注入 M 行 | 该集合行数 ⇒ **M**（不变） |
| **W7-A3** | **超窗数据删除**（log · 180 d） | L2 | 同 A1，窗口 180 d | **0** |
| **W7-A4** | **窗内数据不删除**（log） | L2 | 同 A2 | **M** |
| **W7-B1** | ★ **cutoff 边界** | L2 | 用 **`?time=` 固定 `scheduledTime`** ⇒ 精确构造 `ts == cutoff` / `ts == cutoff-1` | 前者 **保留 1 行**；后者 **删除**（**精确、无抖动**） |
| **W7-B2** | **未来时间保护** | L2 | `ts = nowSec + 86400`、`ts = nowSec + 10 年` | 行数**不变**；`ts == nowSec` 亦不变 |
| **W7-B3** | **`ts = 0` 行为** | L2 | 注入 `ts = 0` | **被删除**（与 §3.2 明示语义一致）⇒ 与文档一致即 PASS |
| **W7-B4** | **窗内边界外延** | L2 | `ts = cutoff`、`cutoff+1`、`nowSec`、`nowSec+1` | 全部**保留** |
| **W7-C1** | **多 `device_id`** | L2 | 3 个合成设备，各有超窗/窗内行 | 每设备超窗清零、窗内不变；摘要 `devices = 3` |
| **W7-C2** | **单设备 > 1000 行 ⇒ 多批** | L2 | 单设备注入 **2,500** 超窗行 | 2,500 行**全部**删除；摘要 `batches >= 3` |
| **W7-C3** | **分批**（每批 ≤ `BATCH_SIZE`） | L2 | 同 C2 | 摘要逐批 `changes` ≤ 1000（或总量 / 批数关系自洽） |
| **W7-C4** | **每表预算截断** | L2 | `--var RETENTION_MAX_CALLS_PER_TABLE=2` + 大量超窗行 | 摘要 `truncated == true`；**剩余超窗行 > 0**；该表 `calls == 1(SELECT) + 2(DELETE) == 3`（★ `I-P1` 的直接证据） |
| **W7-C5** | ★ **中途停止后续跑** | L2 | 先小预算触发一次（截断）⇒ **再**用默认预算触发 | 第二次后剩余超窗行 ⇒ **0**；`converged == true` |
| **W7-C6** | **重复执行幂等** | L2 | 连续触发 **2 次**（预算充足） | 第 2 次 `deleted` 全 **0**；两表行数**不变** |
| **W7-C7** | **两表数据互不影响** | L2 | 仅 telemetry 有超窗行 | `log_history` 行数**逐行不变**；摘要 log 项 `deleted = 0`（★ 仅证明**数据面**不误删） |
| **W7-C8** | **空表 / 无过期数据** | L2 | ① 两表全空 ② 仅有窗内行 | 不报错；`deleted = 0`；`ok == true`；摘要结构完整 |
| **W7-C9** | **不依赖额外 index** | L2 | `EXPLAIN QUERY PLAN` 两条 DELETE | 计划含 `SEARCH … USING INDEX ix_th_device_ts` / `ix_lh_device_ts`；**无 `SCAN <table>`** |
| **W7-C10** | **未修改冻结索引** | L2 | `sqlite_master` 索引清单 | `telemetry_history` 3 个 + `log_history` 2 个（**恰 5**）· 名称与 `0003_*` **逐字一致** |
| **W7-C11** | ★ **预算隔离（**采纳 review · 新增**）** | L2 | **两表各自都有大量超窗数据** + `--var RETENTION_MAX_CALLS_PER_TABLE=2` | telemetry `DELETE ≤ 2` · log `DELETE ≤ 2` · **两者均 `truncated == true`** · `Total D1 calls ≤ 2(SELECT) + 2 + 2 == 6` ⇒ ★ **证明"一表不得耗尽另一表预算"**（`I-P2`）· 若 telemetry 抢光预算导致 log `DELETE == 0` ⇒ **FAIL** |
| **W7-D1** | **`scheduled` 入口可达** | L2 | `curl /cdn-cgi/local/scheduled?cron=...&time=...&format=json` | HTTP **200** + `{"outcome":"ok"}`（**已实测通道可达**） |
| **W7-D2** | **`cron` / `scheduledTime` 被使用** | L2 | 用**两个不同 `?time=`** 触发 | 摘要 `nowSec` / `cutoff` **随之变化**（证明基准取自 `scheduledTime`，非 `Date.now()`） |
| **W7-D3** | **异常隔离（单表失败不阻断另一表）** | **L1** | stub D1 对 `log_history` 抛错 | telemetry 仍完成；`errors[]` 含 log 项；`ok == false` |
| **W7-D4** | **不因错误抛出** | **L1** | 同上 | handler **正常返回**（无 throw）；摘要可序列化 |
| **W7-D5** | **预算不变量（`I-P1`/`I-P2`/`I-P3`/`I-P5`）** | **L1** | stub 恒返回满批（模拟"无穷积压"） | **每表** `DELETE calls == MAX_CALLS_PER_TABLE`（**恰 20**，**不是** `20 × N`）· `calls == 42` · 两表**各自**截断 · **不无限循环** |
| **W7-D6** | **设备级错误跳过** | **L1** | 仅中间设备抛错 | 后续设备**仍被处理**；`errors[]` 含该设备 |
| **W7-D7** | ★ **防御性保险丝（`I-P4`）** | **L1** | `RETENTION_MAX_CALLS_PER_TABLE=100`（人为放大，越过自洽上限）+ stub 恒返回满批 | `calls` **被 `HARD_STOP_CALLS`(45) 钳制** · `truncated == true` · **不得越界**（证明保险丝有效） |
| **W7-E1** | **L1 套件全绿** | L1 | `node test/current/retention_policy.test.mjs` | `FAIL = 0`（且 `UNEXPECTED_PASS = 0`） |
| **W7-E2** | **L2 套件全绿** | L2 | `bash test/real_d1/retention_real_d1.sh` | `FAIL = 0`；exit 0 |
| **W7-E3** | **纳入 `run_all`** | — | `node test/run_all.mjs` | **无新增 FAIL**（新增条目 OK） |
| **W7-E4** | **可重复性** | L2 | L2 脚本**连跑两遍** | 两遍断言结果**逐行一致**（沿用 `v37a` 的 diff 比对做法） |


> **★ `W7-B1` 的确定性来源**：`?time=<ms>` **实测**可覆盖 `controller.scheduledTime` ⇒ **边界用例无需"相对时间 + 安全边距"的妥协**，可做**精确断言**。这是本设计选择 `scheduledTime` 作为基准的直接收益（§3.4）。

### 6.5 测试参数注入方式（**运维可调参数，非测试后门**）

> **★ 命名对应**：正文不变量 / 伪代码使用**代码内常量名**（`MAX_CALLS_PER_TABLE` · `HARD_STOP_CALLS` · `MAX_ROUNDS_PER_TABLE` · `BATCH_SIZE`），其 **env 键名**为对应 `RETENTION_*`（见下表）。

| 变量 | 默认 | 语义 | 生产可配？ |
|---|---|---|---|
| **`RETENTION_MAX_CALLS_PER_TABLE`** | **20** | ★ **每表 DELETE 调用上限 = 吞吐预算**（`I-P1`） | ✅ **可调**（唯一"经济"参数） |
| **`RETENTION_BATCH_SIZE`** | **1000** | 每批行数（`D-1`） | ✅ **可调** |
| `RETENTION_DAYS_TELEMETRY` | **90** | 窗口（天） | ⛔ **不得设置**（冻结值 `H-WP3-01`） |
| `RETENTION_DAYS_LOG` | **180** | 窗口（天） | ⛔ **不得设置**（冻结值 `H-WP3-02`） |
| `RETENTION_MAX_ROUNDS_PER_TABLE` | **20** | 每表**轮数**上限（**死循环保护**，**非**预算 —— `I-P5`） | ⚪ 一般不需改 |
| **`RETENTION_HARD_STOP_CALLS`** | **45** | ★ **防御性保险丝**（正常不可达 —— `I-P4`） | ⛔ **不得设置**（保险丝须恒定） |
| ~~`RETENTION_MAX_CALLS_TOTAL`~~ | — | **不作为独立配置项** ⇒ **由代码派生** `2 + 2 × MAX_CALLS_PER_TABLE`（默认 = **42**） | ⛔ **禁止新增该键**（`I-P6`：避免配置互相矛盾） |

> **★ 纪律**：① 默认值**必须**等于**冻结值**（90 / 180 / §4.4）；② 唯一定义处在 `src/retention.js`；③ **生产 `[vars]` 只允许**设置 `RETENTION_MAX_CALLS_PER_TABLE` 与 `RETENTION_BATCH_SIZE`（"经济"参数）；**`RETENTION_DAYS_*` 与 `RETENTION_HARD_STOP_CALLS` 生产不得设置**（列入部署前检查 `PRE-4`）；④ 摘要**必须**输出实际生效的 `days` / `maxCallsPerTable` / `hardStop` ⇒ 防"静默偏离"；⑤ ★ **`MAX_TOTAL_CALLS` 不是配置项**，由代码派生（= 42）⇒ **结构上杜绝"配置互相矛盾"**（采纳 review）；⑥ ★ **参数自洽校验（`I-P6`）**：若 `2 + 2 × MAX_CALLS_PER_TABLE > HARD_STOP_CALLS` ⇒ **拒绝该配置**（记 `errors[]` 并回退默认）**或钳制**，**不得**放任矛盾（默认 42 ≤ 45 ✅）。
> **★ 与既有做法一致**：`test/real_d1/history_ingest_real_d1.sh` 已用 `--var WEBHOOK_SECRET:...` 注入（**同一模式**）。

---

## §7 【F】Deployment（**本阶段不执行**）

### 7.1 实施阶段的变更清单（**预计**）

| # | 文件 | 变更 | 层级 |
|---|---|---|---|
| 1 | `Cloudflare_Assets/guo-feeder-api/src/retention.js` | **新增** | **生产代码** |
| 2 | `Cloudflare_Assets/guo-feeder-api/src/index.js` | **+`scheduled()`**（≤ 10 行，`fetch` 不动） | **生产代码** |
| 3 | `Cloudflare_Assets/guo-feeder-api/wrangler.toml` | **+`[triggers] crons = ["0 3 * * *"]`** | **生产配置** |
| 4 | `Cloudflare_Assets/guo-feeder-api/test/current/retention_policy.test.mjs` | **新增** | 测试（L1） |
| 5 | `Cloudflare_Assets/guo-feeder-api/test/real_d1/retention_real_d1.sh` | **新增** | 测试（L2） |
| 6 | `Cloudflare_Assets/guo-feeder-api/test/run_all.mjs` | **+1 条目** | 测试 |
| 7 | `Cloudflare_Assets/guo-feeder-api/test/real_d1/README.md` | **登记新脚本** | 测试文档 |
| 8 | `docs/architecture/P3-WP7-Implementation-Record.md` | **新增**（实施记录） | 文档 |
| 9 | `docs/README.md` · `HANDOFF.md` | 状态回填 | 文档 |

> **★ 明确不改**：`history-ingest.js` · `presence.js` · `device-registry.js` · `emqx-admin.js` · `migrations/**` · 固件任何文件 · EMQX 任何对象。

### 7.2 部署前检查（**`PRE-*`** —— 逐项留证）

| ID | 检查 | 判据 |
|---|---|---|
| **PRE-1** | **生产 D1 基线**（单条 SQL 取 `COUNT(*)` + `MAX(id)`，**明确查询时刻**） | 记录两表当前行数与超窗行估算；**避免非原子基线**（历史教训） |
| **PRE-2** | **L1 + L2 全绿** | `W7-E1` / `W7-E2` / `W7-E4` |
| **PRE-3** | ★ **生产 `DELETE … LIMIT` 支持性** | **本文件已用本地 `EXPLAIN` 取证**；**部署后首跑**须在 `wrangler tail` 确认无语法/权限错误 ⇒ 若失败 ⇒ **切形态 B**（§4.3）并在实施记录留痕 |
| **PRE-4** | **生产 `[vars]` 未设置** `RETENTION_*` 5 键 | dashboard / `wrangler.toml` 核对 |
| **PRE-5** | **`[triggers]` 现状** | 当前 **无** cron（已核实）⇒ 部署**新增** 1 条，**不覆盖**既有（无既有） |
| **PRE-6** | `wrangler deploy --dry-run` | exit 0；bundle 大小与 startup 无异常（`limits` 1 s startup） |
| **PRE-7** | **时区与时刻** | cron `0 3 * * *` = **UTC 03:00** = **北京时间 11:00** ⇒ 与低峰期预期一致 |
| **PRE-8** | `migrations` **无新增** | `ls migrations/` 仍为 `0001/0002/0003`（**本 WP 不发 migration**） |

### 7.3 部署步骤（**顺序**）

1. **代码 + triggers 一次部署**：`wrangler deploy`（把 `src/retention.js` / `index.js` / `wrangler.toml` 一并上线）。
   > ⚠️ `wrangler deploy` 会**用配置中的 triggers 覆盖线上 triggers**；当前线上**无** triggers ⇒ 净效果 = **新增 1 条**。
2. **等待传播**：Cron 变更**最长 15 分钟**生效（官方明示）。
3. **核对注册**：dashboard → Workers & Pages → `guo-feeder-api` → Settings → Triggers → **Cron Triggers** 应显示 `0 3 * * *`；或 `wrangler deployments list` 核对版本。
4. **首次执行验证（**二选一，需人工选择**）**：
   - **方案 α（推荐 · 更快取证）**：先以**高频 cron 临时验证**（如 `*/5 * * * *`）部署 ⇒ 用 `wrangler tail` 观察 2–3 次成功的 `scheduled` 调用（核对摘要、无 `exceededCpu`、无 subrequest 超限）⇒ **再**改回 `0 3 * * *` 部署。
   - **方案 β（更保守 · 不打乱生产节奏）**：直接上 `0 3 * * *` ⇒ **次日**用 `wrangler tail` + Cron Events + D1 `SELECT` 核对首次执行。
   > ⚠️ **两方案都需 2 次（α）或 1 次（β）部署**；每次改 cron 都有 ≤ 15 min 传播延迟。**需人工裁决**（§12 `Q-4`）。
5. **效果核对**：D1 `SELECT` 两表 `COUNT(*)` / `MAX(id)` / 超窗行数（**期望：超窗行下降；窗内行不变**）。

### 7.4 验证方式（**生产侧可用手段**）

| 手段 | 用途 | 备注 |
|---|---|---|
| `wrangler tail` | 观察 `scheduled` 调用的 `console.log` 摘要 | **须走代理 `127.0.0.1:7890`**（既有教训）；tail **不含响应体** ⇒ 只能靠**日志**（本设计**已主动**把摘要写日志，正是为规避"tail 无响应体"这一既有取证缺口） |
| **Cron Events**（dashboard） | 最近 **100 次** cron 调用历史 | 官方能力（`Settings → Trigger Events`） |
| `wrangler d1 execute --remote --command "SELECT ..."` | 只读核对行数变化 | **仅 `SELECT`**；**明确查询时刻**；**单条 SQL 同时取 `COUNT` + `MAX(id)`** |
| **本地 `?time=` 触发** | **仅本地**（`/cdn-cgi/local/scheduled`） | **生产无**手动触发端点 ⇒ ⚠️ **这是本设计的已知运维约束**，**必须**在实施记录中声明 |

> **★ `H-WP3-*` 与 `N-3` 的关系**：`P3-History-D1-Plan.md` §5.2 `N-3`「保留策略实际生效验证」被标注为 **必须 `NOT RUN`**（计划阶段不得声称通过）。**本任务单不改该状态**；部署后的实际生效验证属**实施阶段**，须在实施记录中给出，且**不得**在计划阶段写成 `PASS`。

### 7.5 停止 / 回滚策略

| 动作 | 手段 | 效果 | 备注 |
|---|---|---|---|
| **★ 立即停止清理（首选）** | `wrangler.toml` 设 `[triggers] crons = []` ⇒ `wrangler deploy` | **移除全部 Cron Triggers** ⇒ **清理立即停止**（数据不再被删） | 官方明示：**空数组 ⇒ 移除全部**；⚠️ 需 ≤ 15 min 传播 |
| **保留现状不触碰** | `wrangler.toml` 中**删除** `[triggers]` 段（= `undefined`） | 线上 triggers **保持原样** | ⚠️ **回滚代码版本时若想保留 cron，须注意此语义差异** |
| **回滚代码** | `wrangler rollback [version-id]` | 回退 Worker **代码** | ⚠️⚠️ **★ 不会回滚 triggers** —— 见下方"两个独立回滚面" |
| ★★ **两个独立回滚面（**必须记住的运维陷阱**）** | **① 代码版本**（`wrangler rollback` / 再 deploy 旧代码）· **② Cron 配置**（`[triggers]` / 面板） | 两者**互不联动** | **★ 危险场景**：若回滚到"**没有 retention 的旧代码**"而 **trigger 仍在** ⇒ 旧代码**仍会收到 `scheduled` 调用**（旧代码无 `scheduled` handler ⇒ 行为未定义/报错，或静默无事）⇒ **必须每次同时检查两个面**。**结论：代码 rollback ≠ Cron rollback**（在本任务单与后续实施记录中**均加粗保留**） |
| **恢复清理** | `crons = ["0 3 * * *"]` ⇒ `wrangler deploy` | 恢复每日清理 | — |

> **★ 数据面不可回滚（**关键风险**）**：删除**无归档**（`NG-1` · `H-WP3-05` `CLOSED`）⇒ **已删行不可恢复**。**缓解**：① `PRE-1` 基线留证；② `DT:WP-3` §9.4 的监控/告警虽延后，但**摘要日志**可事后审计；③ 首次上线建议**先核对窗口正确**（若窗口配错，删除**不可逆**）⇒ **`PRE-7` + `W7-B1/B2/B4` 必须全绿后才能上线**；④ 可选：上线前用 `wrangler d1 export` 做**一次性备份**（**需人工授权**，§12 `Q-7`）。
> **★ D1 自带 Time Travel**：免费版 **7 天**（官方）；⚠️ **不得**当作回滚方案（它会**恢复整库**到某时点，副作用大于收益）—— **仅登记**。

---

## §8 【G】Acceptance Criteria（**验收矩阵**）

> **判定规则**：任一项 `FAIL` ⇒ **不得**上线；`NOT RUN` **不得**折算 `PASS`。

### 8.1 实施前（**代码 + 本地验证** —— 可在本仓内闭环）

| ID | 项 | **PASS 判据** | 当前 |
|---|---|---|---|
| **W7-A1** | telemetry 超窗删除 | 超窗集合行数 = **0** | `NOT RUN` |
| **W7-A2** | telemetry 窗内保留 | 窗内集合行数 = **注入值** | `NOT RUN` |
| **W7-A3** | log 超窗删除（180 d） | = **0** | `NOT RUN` |
| **W7-A4** | log 窗内保留 | = **注入值** | `NOT RUN` |
| **W7-B1** | **cutoff 边界精确** | `ts == cutoff` **保留**；`ts == cutoff-1` **删除** | `NOT RUN` |
| **W7-B2** | 未来时间保护 | 未来 / `now` 行数**不变** | `NOT RUN` |
| **W7-B3** | `ts = 0` 语义 | **被删除**（与 §3.2 一致） | `NOT RUN` |
| **W7-B4** | 窗内边界外延 | 全部**保留** | `NOT RUN` |
| **W7-C1** | 多设备 | 各设备超窗清零、窗内不变 | `NOT RUN` |
| **W7-C2** | 单设备 > 1000 行多批 | 2,500 行**全删**；`batches >= 3` | `NOT RUN` |
| **W7-C3** | 分批 | 每批 `changes <= BATCH_SIZE` | `NOT RUN` |
| **W7-C4** | **每表预算截断** | `truncated == true` 且剩余 > 0 · 该表 `calls == 1(SELECT)+2(DELETE)` | `NOT RUN` |
| **W7-C5** | 截断续跑 | 二次触发后剩余 = **0** | `NOT RUN` |
| **W7-C6** | 重复幂等 | 第二次 `deleted` 全 0；行数不变 | `NOT RUN` |
| **W7-C7** | 两表数据互不影响 | log 行数**逐行不变** | `NOT RUN` |
| **W7-C8** | 空表 / 无过期 | `ok == true`；`deleted == 0` | `NOT RUN` |
| **W7-C9** | 不依赖额外 index | `EXPLAIN` 命中 `ix_*_device_ts`；无 `SCAN <table>` | `NOT RUN` |
| **W7-C10** | 索引集合未变 | **恰 5 个** · 名称与 `0003_*` 逐字一致 | `NOT RUN` |
| **W7-C11** | ★ **两表预算隔离（`I-P2`）** | 两表 `DELETE ≤ 2` · **各自** `truncated` · `Total ≤ 6` · **log 不被 telemetry 饿死** | `NOT RUN` |
| **W7-D1** | scheduled 入口 | 本地 `200` + `outcome=ok` | `NOT RUN` |
| **W7-D2** | 基准取自 `scheduledTime` | 两个 `?time=` ⇒ `cutoff` 不同 · 与 `Date.now()` 无关 | `NOT RUN` |
| **W7-D3** | 异常隔离 | 一表失败另一表完成 | `NOT RUN` |
| **W7-D4** | 不抛出 | handler 正常返回 | `NOT RUN` |
| **W7-D5** | **预算不变量（`I-P1`/`I-P2`/`I-P5`）** | 每表 `DELETE == 20`（**非 `20×N`**）· `calls == 42` · 无死循环 | `NOT RUN` |
| **W7-D6** | 设备级跳过 | 后续设备仍处理 | `NOT RUN` |
| **W7-D7** | **防御性保险丝（`I-P4`）** | `calls` 被 45 钳制 · `truncated` · 不越界 | `NOT RUN` |
| **W7-E1** | L1 全绿 | `FAIL = 0` | `NOT RUN` |
| **W7-E2** | L2 全绿 | `FAIL = 0` · exit 0 | `NOT RUN` |
| **W7-E3** | `run_all` 无新增 FAIL | exit 0 | `NOT RUN` |
| **W7-E4** | 可重复性 | 两遍逐行一致 | `NOT RUN` |
| **W7-G2** | **ingest 路径零改动** | `git diff` 对 `history-ingest.js` = **0 行** | `NOT RUN` |

### 8.2 部署后（**生产** —— 须单独授权）

| ID | 项 | **PASS 判据** | 当前 |
|---|---|---|---|
| **W7-F1** | `wrangler.toml` 含 `[triggers] crons = ["0 3 * * *"]` | 逐字一致 | `NOT RUN` |
| **W7-F2** | dry-run 通过 | exit 0 | `NOT RUN` |
| **W7-F3** | 生产 cron 已注册 | dashboard Triggers 显示 **1 条** | `NOT RUN` |
| **W7-F4** | 生产首次执行成功 | `outcome = ok` · **无** `exceededCpu` · **无** subrequest/calls 超限 | `NOT RUN` |
| **W7-F5** | 生产数据效果正确 | 超窗行下降 · 窗内行**不变** | `NOT RUN` |
| **W7-F6** | 摘要可读 | `wrangler tail` 可见结构化摘要（含 `days` / `cutoff` / `deleted`） | `NOT RUN` |
| **W7-G1** | 可停止 | `crons = []` + deploy ⇒ Triggers 显示 **0 条** | `NOT RUN` |

### 8.3 计数小计（**逐项留证 —— 数字须自洽**）

| 组 | 项 | 数量 | 所在节 |
|---|---|---|---|
| `A` 超窗 / 窗内（两表） | `A1`–`A4` | **4** | §8.1 |
| `B` 边界 / 未来 / `ts=0` | `B1`–`B4` | **4** | §8.1 |
| `C` 批 / 预算 / 隔离 / 索引 | `C1`–`C11` | **11** | §8.1 |
| `D` 入口 / 基准 / 异常 / 预算闸 | `D1`–`D7` | **7** | §8.1 |
| `E` 两层测试 / `run_all` / 可重复 | `E1`–`E4` | **4** | §8.1 |
| `G2` ingest 零改动 | `G2` | **1** | §8.1 |
| **小计（实施前）** | — | **31** | §8.1 |
| `F` 部署后 | `F1`–`F6` | **6** | §8.2 |
| `G1` 可停止 | `G1` | **1** | §8.2 |
| **小计（部署后）** | — | **7** | §8.2 |
| **★ 合计** | `W7-*` | **38** | §8.1 + §8.2 |

> **⚠️ 计数说明（★ 采纳 review 的"35/36"指正）**：`G1` / `G2` **编号连续但分列两节**（`G2` 属"实施前"、`G1` 属"部署后"）⇒ 易误数。**本节以显式小计表为准**（旧稿"35 项"为笔误；原表实为 36 项，**新增 `C11` / `D7` 后 = 38 项**）。
> **★ 结论状态**：**`计划:WP-7 = NOT RUN`（全部 **38** 项）**。本任务单**不含任何 `PASS`**。

---

## §9 【H】STOP conditions

> **触发任一条 ⇒ 立即停止，报告人工，不得自行改设计。**

| ID | 条件 | 处置 |
|---|---|---|
| **S-1** | **`D-1…D-5` 任一条无法满足** | **STOP** ⇒ 报告冲突点与证据 |
| **S-2** | 形态 A **与**形态 B **均被生产 D1 拒绝** | **STOP** ⇒ 需重新设计（可能触发"改 schema / 加索引"⇒ **须人工授权**，且与 `D-5` / `H-WP3-10` 冲突） |
| **S-3** | 需**新增索引 / 改 schema / 发 migration** 才能满足验收 | **STOP**（`D-5` / `I-9` / `I-12` 禁） |
| **S-4** | 需**修改生产代码**以满足**本轮范围外**的验收（如 `payload` 上限 ⇒ 属 `计划:WP-4`） | **STOP** ⇒ 报告并请示 |
| **S-5** | **subrequest / CPU / wall time 限制**导致**无法在单次 invocation 内取得有意义的进展**（例如预算 < 1 批） | **STOP** ⇒ 需改频率 / 分批策略 / 升级计划 |
| **S-6** | 发现**删除会波及窗内数据**（窗口计算错误 / 边界误判） | **STOP**（**数据不可恢复**） |
| **S-7** | **`H-WP3-06`（`payload` 上限）** 被人工判定为**必须先行**解决 | **STOP** ⇒ 转回 `计划:WP-4` 或单独裁决（§0.1） |
| **S-8** | 部署阶段发现**线上 cron 与预期不符**（如已存在他人配置的 triggers） | **STOP** ⇒ **不得覆盖**，先报告 |
| **S-9** | 工作树出现**非本任务**的意外修改 | **STOP** ⇒ **不覆盖 / 不 reset / 不 checkout**，报告 |
| **S-10** | 需**生产数据备份 / 导出**（如 `d1 export`）以获得安全网 | **STOP** ⇒ 须人工授权（§12 `Q-7`） |
| **S-11** | 发现**冻结窗口（90 / 180）**需要调整 | **STOP** ⇒ `DT:WP-3` §5.1：**须以新的人工裁决调整（不自动变更）** |
| **S-12** | **`ts <= 0` 的处置口径**被人工要求**保护**（不删） | **STOP** ⇒ 需变更谓词（§12 `Q-2`），**须重新走 D-3 论证** |
| **S-13** | ★ **`I-P1`–`I-P6` 任一预算不变量被证明无法满足**（例如实现上做不到"每表独立计数"） | **STOP** ⇒ 报告冲突点 + 证据；**不得**退回"全局共享池"**而不先改文档**（§4.6.1） |

---

## §10 与 `DT:WP-3` 的一致性自查

| `DT:WP-3` 要求 | 本任务单如何满足 |
|---|---|
| §4.2 **S-B 固定窗口 TTL** | §3 固定窗口 = 严格 `< cutoff`；**窗内保留原始粒度**（无聚合、无摘要化） |
| §5.1 telemetry = **90 天** | §3.1 `RETENTION_DAYS.telemetry_history = 90` |
| §6.1 log = **180 天** · **L-A 统一窗口** | §3.1 `= 180`；**三级同一窗口**（**不分级** ⇒ **无 `level` 谓词**） |
| §7.3 机制 = **scheduled cleanup job** | §5 `scheduled()` + Cron Triggers（**不**在 ingest 路径 · **不**惰性） |
| **§7.4 `D-1` 分批** | §4.4 `LIMIT 1000` |
| **§7.4 `D-2` 禁大事务 · 可续跑** | §4.7 每批独立提交；§4.5 无游标续跑 |
| **§7.4 `D-3` 谓词只依赖 `device_id + ts`** | §4.3 逐字；§3.2 严格 `<`；无 `now()` 竞态（基准 = `scheduledTime`） |
| **§7.4 `D-4` 不依赖不存在索引** | §4.3 **已 `EXPLAIN` 取证**命中 `ix_th_device_ts` / `ix_lh_device_ts`；**绝不** `WHERE ts < ?` 单列 |
| **§7.4 `D-5` 不改冻结索引** | §1.2 OUT + `W7-C10` 断言 |
| §5.2 ① 目标集合 = 遍历 `device_id` | §4.2 `SELECT DISTINCT device_id` |
| §3 `I-1` 清理不在 ingest 路径 | §1.2 OUT（**不改** `history-ingest.js`）· `W7-G2` |
| §3 `I-6` 两表物理分离 | §4.5 分表独立处理 · `W7-C7` |
| §11.1 `H-WP3-08` 批大小属实施参数 | §4.4 **可调 + 留痕**；默认 1000 |
| §11.1 `H-WP3-10` 不新增索引 | §1.2 OUT · `W7-C10` |
| §11.2 `H-WP3-04/06/09` `DEFERRED` | §1.2 OUT（不做聚合 / 不做配额 / `payload` 排除） |
| §9 容量口径 = **理论估算（非 `PASS`）** | §4.6 吞吐校核**标注为估算**；**不得**据估算下"安全"结论 |
| §7.4 `D-1` 分批 + **平台调用预算** | §4.6.1 不变量 **`I-P1`–`I-P6`**（每表 `DELETE ≤ 20` · 正常 `Total ≤ 42` · 保险丝 `45` **不可达**） |


> **★ 一致性结论**：本任务单**未引入**与 `DT:WP-3`（`FROZEN`）冲突的口径；**未修改**任何冻结文档。

---

## §11 待人工裁决 / 需注意（**STOP 之外，但须知晓**）

### 11.0 ★ 本轮 review 采纳清单（`2026-10-16`）

| review 意见 | 严重度 | 处置 |
|---|---|---|
| **§4.6 把 20 rounds 当成 20 calls** | 🔴 **严重** | ✅ **已修正**：引入 `MAX_CALLS_PER_TABLE`（= **DELETE 调用计数**，吞吐预算）；轮数**仅**防死循环（`I-P5`）· 伪代码双计数器 |
| **"两表独立预算"与全局预算矛盾** | 🔴 **严重** | ✅ **已修正**：改 **per-table 计数** + `I-P2`；`MAX_TOTAL` **由代码派生**（`I-P6`）；§4.6 论证重写 |
| **缺预算隔离测试** | 🟠 中 | ✅ **已新增 `W7-C11`**（两表各 `≤ 2` DELETE · 各自 `truncated` · `Total ≤ 6`） |
| **`HARD_STOP=45` 定位不清** | 🟠 中 | ✅ **已重定义**为**防御性保险丝**（`I-P4`）：正常**数学上不可达**，**触发即缺陷信号**；加 `W7-D7` 验证 |
| **形态 B 未做 `EXPLAIN`** | 🟡 小 | ✅ **已加**：B **启用时强制** `EXPLAIN` 留证（§4.3 新行） |
| **确定性表述过强** | 🟡 小 | ✅ **已收紧**：改述为"**最终删除集合由谓词唯一确定**"（§4.7） |
| **Round-Robin "天然公平"** | 🟡 小 | ✅ **已精确化**：§4.5 新增"公平性注"（单 invocation 内单设备 ≤ 1 批；跨 invocation 靠起点偏移） |
| **`errors[]` 无上限** | 🟡 小 | ✅ **已加**：上限 **16** 条 + `errorsTruncated`（§5.3） |
| **35 / 36 项统计** | 🟡 文档 | ✅ **已修正**：原表实为 **36** 项（旧稿写 35 为笔误）⇒ 新增 `C11`/`D7` 后 **38 项** · 加 §8.3 小计表 |
| **`RETENTION_MAX_CALLS` 语义不清** | 🟡 小 | ✅ **已改名**：`RETENTION_MAX_CALLS_PER_TABLE` / `RETENTION_HARD_STOP_CALLS`；**禁止**独立配置 `MAX_TOTAL` |
| `wrangler rollback` 不回滚 triggers | ✅ 已保留 | ✅ **已加粗为独立行**（§7.5「两个独立回滚面」） |
| `ts = 0` 默认参与比较 | ✅ 赞成 | ✅ **保持默认**（`Q-2` 见下） |
| `ctx.waitUntil` 不使用 · α 优先 · `export` 非强制 | ✅ 赞成 | ✅ **保持不变**（`Q-4` / `Q-7`） |

### 11.1 待裁决 / 需注意项

| ID | 项 | 建议 | 影响面 |
|---|---|---|---|
| **Q-1** | **`H-WP3-06` 双重前置口径差异** —— `DT:WP-3` §11.5 称"实施前必须裁决"，本轮指令**显式排除** | **按本轮指令执行**（排除）；**登记残留**；**建议**在批准 `G-1` 时一并确认"`payload` 上限延后不影响 retention 正确性" | 仅 `log` 表**体积估算 / 爆发风险（`CR-2`）**，**不涉**删除逻辑 |
| **Q-2** | **`ts <= 0`（含 `ts = 0`）的处理** | ✅ **review 已确认默认正确**：按 §3.2 **参与比较 ⇒ 被删**（语义最简、无特判、`D-3` 最纯）。**不要**为保护少量异常数据而在 retention 逻辑里偷偷加 `AND ts > 0` —— 那会破坏"删除集合完全由 `device_id + ts` 决定"。若未来确需保留异常数据，**另走人工裁决** | 少量历史异常行 |
| **Q-3** | **形态 A / B 的选择** | **建议**：默认 **A**（字面最贴合 `D-3`、更简洁）；**`PRE-3` 失败时切 B**，**留痕**（§4.3） | 仅 SQL 形态 |
| **Q-4** | **首次部署验证策略**（方案 α 高频临时验证 / 方案 β 直接每日 + 次日核对） | ★ **建议 α**（review 亦明确偏 α）：**临时高频 cron → 观察 2 次成功 → 立即恢复 `0 3 * * *`**（**不要**让临时频率长期挂着）。每次核对：是否触发 / 摘要是否正常 / `calls ≤ 42` / `truncated` 是否符合预期 / **无 `exceededCpu`** / **无 subrequest 错** / D1 行数变化正确。代价 = 2 次部署 + 2 次 ≤ 15 min 传播 | 部署节奏 |
| **Q-5** | **`RETENTION_*` env 覆盖是否允许** | ✅ **允许，但已收窄**：**仅** `RETENTION_MAX_CALLS_PER_TABLE` / `RETENTION_BATCH_SIZE`（"经济"参数）生产可配；**`RETENTION_DAYS_*` 与 `RETENTION_HARD_STOP_CALLS` 生产不得设置**；**`MAX_TOTAL` 禁设**（由代码派生，`I-P6`）；摘要**必须**输出实际生效值 | 配置面 |
| **Q-6** | **`limits.subrequests` 是否提高**（Free 50 → 更高） | **暂不做**（当前预算 42 ≤ 50 已够）；若规模化后不足，**另行评估**（Free 版是否允许提高待验证） | 规模化 |
| **Q-7** | **上线前是否做一次性 D1 备份**（`wrangler d1 export`） | ✅ **review 赞成保持"非强制 + 须人工授权"**（备份**不是** retention 功能本身的技术前置）。**建议**：因 `DELETE` **不可恢复**，实际首次生产上线前**做一次**；但**实现 agent 不得自行决定**（`S-10`） | 数据安全网 |
| **Q-8** | **无告警通道** | 如实声明：失败**仅**在 `console.log` / Cron Events 可见（`NG-7`）；**建议**在 P4 观测设施就绪后补 | 可观测性 |
| **Q-9** | **`SELECT DISTINCT device_id` 的规模化成本** | 11 台可忽略；规模化后为**读放大**项（D1 "rows read" 配额）。**建议**：先记录，规模化时再优化（如按 `last_seen` 预筛） | 规模化 |
| **Q-10** | **`ctry`/`W7-F*` 生产验收** 属部署阶段 | **须单独授权**（本轮仅任务单） | 流程 |

---

## §12 本阶段纪律声明

- **本文件 = `PLAN ONLY`**：**未**修改 `src/**` · **未**改 `wrangler.toml` · **未**改 `migrations/**` · **未**改 D1 · **未**改 EMQX · **未**部署 · **未**重启 · **未** commit / push。
- **未执行任何 DML**：**未** `INSERT` / `UPDATE` / `DELETE`（**对任何库**）。
- **只读取证清单（本轮实际执行）**：
  | 取证 | 手段 | 结果 |
  |---|---|---|
  | SQLite 版本 / 编译选项 | `PRAGMA compile_options` / `sqlite_version()`（**本地**） | **被拒**（`SQLITE_AUTH` / `not authorized`）⇒ 改为语法探测 |
  | `DELETE … LIMIT` 语法 | **`EXPLAIN`**（**本地** · **零数据修改**） | ✅ **被接受**（47 条 VDBE · 含 `Delete` + `IdxDelete`） |
  | 索引命中（`D-4`） | `EXPLAIN`（**本地**） | ✅ 计划引用 `ix_th_device_ts` / `ix_lh_device_ts` |
  | `scheduled` 端点 + `time` 覆盖 | 临时探针 Worker（`.wrangler/` scratch · **探针文件不在仓库**） | ✅ `/cdn-cgi/local/scheduled?cron=…&time=…&format=json` ⇒ `200 {"outcome":"ok"}`；`?time=` **生效** |
  | `--test-scheduled` | `wrangler dev --help` | ✅ 存在 |
  | 平台限制 | Cloudflare 官方文档（Workers / D1 Limits / Cron Triggers） | ✅ 已引用具体数字与原文 |
- **未修改**任何冻结文档（`P3-WP1-*` / `P3-WP2-*` / `P3-WP3-History-Storage-Policy-Design.md` / `P3-DT-WP4-*` / `P3-History-D1-Plan.md`）。
- **未改动任何 Gate**（`G-1` / `G-2` / `G-3` / `G-4` / `G-5` 状态**均未变**）。
- ★ **`rev.2` 修订（`2026-10-16`）**：采纳外部 review ⇒ 修正 **§4.5 / §4.6 的 D1 调用预算模型**（`MAX_ROUNDS_PER_TABLE` ≠ `MAX_CALLS_PER_TABLE` · 新增不变量 **`I-P1`–`I-P6`**）· 新增 **`W7-C11`（预算隔离）** 与 **`W7-D7`（防御性保险丝）** · 收紧 §4.7 确定性表述 · 形态 B **强制 `EXPLAIN`** · `errors[]` 上限 16 · 修正项数（**36 → 38**）。**本轮仍为 `PLAN ONLY`** —— `rev.2` **不引入任何代码 / 配置 / DML**。
- **本文件唯一产物**；**已 STOP，等待下一步授权，不得自行进入编码阶段**。

---

> **`计划:WP-7 = PLAN DRAFT`（待人工审核）.**
> **No code. No config change. No migration. No DML. No deploy. No commit.**
> **`G-1` `HUMAN_REQUIRED` · `G-4` `BLOCKED` unchanged. `P3` 实施状态未变.**
> **所有 38 项验收 = `NOT RUN`**（小计见 §8.3）**.**

---

*本文件于 **2026-10-16** 建立（**`rev.2`** 同日采纳外部 review 修正），为 **`计划:WP-7`（History Retention）的实施任务单（`PLAN ONLY`）**；设计依据 = **`DT:WP-3`（`FROZEN`）**；schema 依据 = **`migrations/0003_*`（已上线）**；基线 = 固件 `4e61a9e` · 云端 `f519c9e` · 消息 `3165aab`。*
*不含任何生产变更、代码改动、配置变更或数据库操作。*
