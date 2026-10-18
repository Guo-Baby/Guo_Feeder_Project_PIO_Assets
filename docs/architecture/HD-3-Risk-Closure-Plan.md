# HD-3 剩余风险收尾：调查与实施方案（`rev.2`）

> 日期：`2026-10-19 03:10`（GMT+8）
> 类型：**调查 / 设计 / 文档**（本轮**未**实施任何业务代码、DB 写入、迁移应用、部署或真机操作）
> 状态：**`PLAN READY`**
> 前序：`HD-3-RealDevice-Verification-and-Release.md`（`rev.5`）· `HD-3-PowerCycle-Monitoring-Baseline.md`（§8/§9）

---

## 0. 状态核验（只读，`03:09`）

### 0.1 Git（起始 = 当前，本轮无 Git 操作）

| 仓 | 分支 | HEAD | 工作区 |
|---|---|---|---|
| 固件 | `wb` | **`7e73177`** | 11 项：5 他任务 `M` + 3 本轮 HD-3 文档 `M` + 3 `??` |
| 云端 | `main` | **`362ddfd`** | **干净** |
| EMQX | `main` | `ec2364f` | **干净** |

暂存区：三仓均空。

### 0.2 设备与监测

| 项 | 值 |
|---|---|
| 监测进程 | **仍在运行**（`pid=40080`，`02:29:15` 启动，约 `04:29` 自然结束） |
| 观测方式 | **只读日志文件**，未打开 COM8、未起第二串口进程 |
| 启动次数 | `recovery=` **14 条**（`boot 5…18`） |
| 是否额外复位 | **否**（最后心跳 `03:09:01`；`02:45:36` 后无新 `recovery=`） |
| 设备当前 `boot_seq` | **18** |

### 0.3 云端

`max_boot_seq=117` · `obs=276` · `rewind=266` · `log_history=453` · `log_conflict=0` · Worker `508461f3` · `LOG_BOOT_WATERMARK="1"` · `0001…0005` 已应用。

### 0.4 本轮新增的技术事实（**均已代码核实**）

| # | 事实 | 出处 | 影响 |
|---|---|---|---|
| **F-1** | `LOG_P_MAX = 0x59`，PID 已用满 `0x00…0x59`（90 个，无重复） | `src/log/log_events.h:200`；`sort/uniq` 核对 | 新增 4 个 PID **必须**抬 `LOG_P_MAX` |
| **F-2** | 有两处 `static_assert(LOG_P_MAX == 0x59)` | `log_events.h:619`、`test/log_contract/probe_ok.cpp:76` | 抬 `LOG_P_MAX` 必须同步改这两处，否则编译失败 |
| **F-3** | `LOG_MAX_PARAMS = 8`；启动事件当前只带 **1** 个参数（`LOG_P_INIT_MS`） | `log_events.h:419`；`main.cpp:628` | 加 4 个 ⇒ 5 个，**不超限** |
| **F-4** | **仓库内没有任何 TLV 参数解码器**：`probe_envelope.cpp` 只置 `param_count = 0`，不解析 PID | `probe_envelope.cpp:164` | “旧解码器跳过未知 PID”**无法用现有代码证明**，因为没有解码器 |
| **F-5** | 云端把 `payload_base64` 当**不透明数据** | `history-ingest.js:23` 注释 | 加 PID **对云端零影响**（云端不按 PID 解析） |
| **F-6** | `log_envelope_contract.md`（209 行）**未定义参数表与未知 PID 规则** | 全文检索无 `param`/`TLV`/`0x21` | 需在批次 3 补文档，不能声称“契约已支持” |
| **F-7** | D1 `batch()` = **SQL 事务**；失败时**回滚整个序列**；返回**按序**的 `D1Result[]`，每条含 `meta.changes` | Cloudflare 官方文档（`batch()` 段） | CAS 可用 `results[i].meta.changes` 判定；原子性有官方依据 |
| **F-8** | 现有 `log-conflict.js` 只 `await db.batch(...)`，**不检查 `meta.changes`** | `src/log-conflict.js` | 说明“batch 成功 ≠ 命中”，是本轮要求补的判定点 |

### 0.5 文档缺陷 `D-1`（`0004/0005` 文件头仍写“草案”）

**处置已定**（`P-5`）：**不改写已应用 migration 的历史内容**；改在正式 README 中澄清“已转正并应用”，并解释旧文件头的历史状态。随批次 1 的文档变更一起做。

---

## 1. 决策记录（`P-1` … `P-6`）

| # | 决策 | 状态 | 理由 | 未解决风险 / 授权边界 |
|---|---|---|---|---|
| **P-1** | 接受存在**代际歧义**，但**不接受**原 B-3 的含糊分类；改为「分离观测与判定」模型，保留不可消除的盲区 | **已定** | 原规则把 `19–117` 一刀切成 `stale`，会让新代正常前进**不可见**，而“抬高水位”又会让误报回归 | 盲区（`(epoch_max, prev_epoch_max]` 内不计 rewind）**不可消除**，除非上 `C-2` |
| **P-2** | 短期 `C-1 + C-4`；`C-2` 暂缓；`C-3` 不实施 | **已定** | `C-2` 仅覆盖“NVS 存活”，无法解决双擦除；`C-3` 要改 ACK，代价大于收益 | 内容静默丢失**仍不可在设备侧消除** |
| **P-3** | 原则批准评估并实施 `O-A` 的 4 个加性 PID，**但须先完成 PID / 契约 / 解码器核验** | **有条件批准** | 加性、云端不解析；但 `LOG_P_MAX` 与两处 `static_assert` 必须同步（`F-1/F-2`） | 核验不足 ⇒ 批次 3 必须报 `BLOCKED`，不得直接声称向后兼容 |
| **P-4** | 构造真实生产冲突样本 **暂缓** | **暂缓** | 需先单独提交身份、数据边界、清理策略与风险方案 | 线上 `log_conflict` 保持 0 行 ⇒ `H-3-7-b-2` 维持 `OPEN` |
| **P-5** | 用正式 README/说明纠正 `0004/0005` 状态，**不改** migration SQL | **已定** | 已应用文件的内容应保持与实际应用一致 | 文件头仍“过时”，靠 README 指路 |
| **P-6** | 四道门禁保持原状 | **已定** | 本方案不触碰门禁流程 | `BT-1 OPEN` · `G-4 BLOCKED` · `WP-5` 未解锁 · `STOP-R1` 无解除 |

---

## 2. R-11：水位 rebase（修订）

### 2.1 根因（不变）

`log-boot-watermark.js:75` 的 upsert 用 `max_boot_seq = MAX(...)` —— 只升不降。
RD-8 整片擦除把设备打回 `boot_seq=1`，水位停在擦除前的 **117** ⇒ 之后每次正常启动都被计入 `rewind`（实测 `266/276 = 96.4%`）。

**根因不是数值错误，而是缺少“世代”概念。**

### 2.2 管理接口（保持不变，作为待实施方案）

```
POST /api/admin/device/:deviceId/watermark/rebase
Authorization: Bearer <ADMIN_API_TOKEN>
{
  "rebase_id": "uuid-v4",
  "reason": "full_erase_confirmed",
  "evidence": "RD-8 2026-10-19 02:31，NVS+LittleFS 双擦，串口 recovery=PROVISION",
  "expected_epoch": 1,
  "expected_max_boot_seq": 117,
  "confirm_boot_seq": 18,
  "dry_run": false
}
```

- 复用 `admin-auth.js:73` 的 `authorizeAdmin`（Bearer + timing-safe，未配置时统一 401）。
- 挂载在 `index.js` 的 `/api/admin/` 分支内（`handleAdmin`），**不新增路由根**。
- 响应（管理面，**不是**设备四字段契约）：

```json
{"ok":true,"device_id":"...","epoch":2,"max_boot_seq":18,"prev_epoch_max":117,
 "epoch_max_before":117,"rewind_count_reset_from":266,"rebase_id":"...","applied":true,"dry_run":false}
```

### 2.3 ★ 两种分类模型（本轮修订核心）

rebase 后：`epoch=2`、`max_boot_seq=18`、`prev_epoch_max=117`。
`19–117` 这个区间**天然二义**：可能是新代正常前进，也可能是旧代迟到。**没有任何字段可以可靠区分**（`F-4` 已证仓库无解码器，消息也不带可信代际标识）。

#### 模型 A：保守模式（保留完整歧义区间）

| 观测 | 分类 | 动作 |
|---|---|---|
| `boot > prev_epoch_max`（>117） | `new_epoch` | 清空 `prev_epoch_max`，`max := boot`，恢复常规判定 |
| `max < boot <= prev_epoch_max` | `ambiguous_stale` | **只 +`ambiguous_count`**；`max` **不前进**；不计 rewind |
| `boot == max` | `same` | `obs_count++` |
| `boot < max` | `rewind_candidate` | `rewind_count++` |

**代价**：新代从 19 走到 117 期间（可能数十次真实启动），水位**一直停在 18** ⇒ 这段时间里连“正常前进”也看不见，`max_boot_seq` 严重滞后。

#### 模型 B：分离观测与判定（**推荐**）

把“看见什么”和“据此判定什么”拆成两个字段：

| 字段 | 语义 | 谁能写 |
|---|---|---|
| `observed_max_boot_seq` | **所有观测**的最大 `boot_seq`（含歧义样本） | 任何观测都更新 |
| `max_boot_seq` | **判定基线**（只在新代被确认后前进） | 仅 `new_epoch` 事件 |
| `ambiguous_count` | 落在歧义带的观测数 | 只累加 |
| `prev_epoch_max` | 上一代峰值（歧义带上界） | rebase 写入，`new_epoch` 时清空 |

| 观测 | 分类 | `observed_max` | `max` | `rewind` |
|---|---|---|---|---|
| `boot > prev_epoch_max` | `new_epoch` | ↑ | **↑ 并清空 `prev_epoch_max`** | 不计 |
| `max < boot <= prev_epoch_max` | `ambiguous_forward` | **↑** | **不动** | **不计** |
| `boot == max` | `same` | — | — | — |
| `boot < max` | `rewind_candidate` | — | — | **+1** |

**推荐 B 的理由**：

1. 至少能**看见**新代在前进（`observed_max` 单调上升），运维不用等到 118 才知道设备活着；
2. `max` 保持"可信判定基线"的语义，**不会**被旧代迟到日志污染；
3. 歧义带**可度量**：`observed_max` 与 `prev_epoch_max` 一比较，就知道还要多久走出歧义；
4. 恢复条件明确：首次 `boot > prev_epoch_max` ⇒ 清空边界、`max := boot`，**此后完全恢复**常规检测。

**代价（必须写进运维手册）**：

- 歧义带内 `rewind` **不计数** ⇒ 这期间真发生回退**检不出来**；
- 该盲区**无法**通过软件消除（缺可信代际标识），只能靠 `C-2` 或"人工看设备侧 `[log] recovery=`"兜底；
- **不得**宣称"rebase 后回退检测完全恢复"。

### 2.4 ★ CAS 命中与审计原子性（本轮新增硬要求）

**F-7** 给了原子性依据，但 **F-8** 说明"batch 成功 ≠ 命中"。因此规定：

1. **不得**只看 `await db.batch()` 是否抛错；
2. 必须读回 **`results[i].meta.changes`**，且索引与语句顺序**严格对应**（官方文档：按序返回）；
3. 判定式：`changes(update) === 1` **且** `changes(audit) === 1`；两者不一致 ⇒ 视为**事务语义异常**，返回 `500` 并**不**声称成功。

**实现方式（不依赖 `changes()` SQL 函数）**——单 batch 两条语句：

```sql
-- ① 条件更新（CAS）：同时写入 last_rebase_id 作为"已应用"标记
UPDATE log_boot_watermark
   SET epoch             = epoch + 1,
       prev_epoch_max    = max_boot_seq,
       max_boot_seq      = :new_base,
       observed_max_boot_seq = MAX(observed_max_boot_seq, :new_base),
       rewind_count      = 0,
       ambiguous_count   = 0,
       last_rebase_id    = :rid,
       last_rebase_at    = unixepoch(),
       updated_at        = unixepoch()
 WHERE device_id = :dev
   AND epoch = :expected_epoch
   AND max_boot_seq = :expected_max;

-- ② 审计：仅当①真的命中（标记已写入）才插入，值直接取行上的 OLD/NEW
INSERT INTO log_boot_watermark_audit
       (rebase_id, device_id, epoch_before, epoch_after,
        max_boot_seq_before, max_boot_seq_after, prev_epoch_max_after,
        rewind_count_before, ambiguous_count_before,
        reason, evidence, actor, created_at, result)
SELECT :rid, device_id, epoch - 1, epoch,
       prev_epoch_max, max_boot_seq, prev_epoch_max,
       :rewind_before, :ambiguous_before,
       :reason, :evidence, :actor, unixepoch(), 'applied'
  FROM log_boot_watermark
 WHERE device_id = :dev AND last_rebase_id = :rid;
```

- ①未命中 ⇒ `last_rebase_id` 不变 ⇒ ②插入 **0 行** ⇒ **不会**产生误导性 `applied` 审计；
- ①命中 ⇒ ②恰好 1 行，`epoch_before/after`、旧基线都来自**行本身**，不靠调用方传值；
- 两条在同一 `batch()` ⇒ 官方事务语义保证**同成同败**。

**幂等（重复 `rebase_id`）**：

- 请求开头先 `SELECT ... FROM audit WHERE rebase_id = ?`；
- 命中 ⇒ **不写任何东西**，直接回放首次结果（`applied:true/false` 原样返回）；
- 未命中但并发抢先 ⇒ ②触发 PK 冲突 ⇒ 整个 batch 回滚（①也不生效）⇒ 捕获后重查 audit，回放结果；
- **不用** `INSERT OR IGNORE`（它会连 NOT NULL 违约一起吞掉，掩盖真实错误）。

**失败矩阵**：

| 情形 | 行为 |
|---|---|
| `expected_epoch` / `expected_max_boot_seq` 不匹配 | ① 0 行 ⇒ ② 0 行 ⇒ 返回 `409 cas_mismatch`，**无任何写入、无审计** |
| 审计插入失败（约束/类型） | batch 抛错 ⇒ ① 回滚 ⇒ 水位**保持原值** ⇒ `500 audit_write_failed` |
| D1 超时 / 网络异常 | 结果不确定 ⇒ 返回 `503 outcome_unknown`，**提示调用方用同一 `rebase_id` 重试**（重试安全，因幂等） |
| `dry_run=true` | 只做 SELECT + 校验，返回"将会怎样"，**不写 watermark、不写 audit** |
| 重复 `rebase_id` | 回放首次结果，**零写入** |

### 2.5 数据模型（`0006` 草案，本轮**不创建文件、不 apply**）

对 `log_boot_watermark` 增列（`ADD COLUMN`，常量默认值）：

| 列 | 类型 | 说明 |
|---|---|---|
| `epoch` | INTEGER NOT NULL DEFAULT 1 | 世代序号 |
| `epoch_started_at` | INTEGER | 本世代起点 |
| `prev_epoch_max` | INTEGER NULL | 上一代峰值；`NULL` = 无歧义带 |
| `observed_max_boot_seq` | INTEGER NOT NULL DEFAULT 0 | 观测最大值（**不含判定**） |
| `ambiguous_count` | INTEGER NOT NULL DEFAULT 0 | 歧义带观测数 |
| `last_rebase_id` | TEXT NULL | 最近一次已应用的 rebase（用于审计条件插入） |
| `last_rebase_at` | INTEGER NULL | 最近一次 rebase 时间 |

审计表 `log_boot_watermark_audit`：`rebase_id`(PK) / `device_id` / `epoch_before` / `epoch_after` / `max_boot_seq_before` / `max_boot_seq_after` / `prev_epoch_max_after` / `rewind_count_before` / `ambiguous_count_before` / `reason` / `evidence` / `actor` / `created_at` / `result`。
索引：`ix_lbwa_device_created(device_id, created_at DESC)`。

> ⚠️ 现有 `rewind_count` 的**历史值**在 rebase 时归零，但**旧值写入审计行**（`rewind_count_before`）⇒ 不灭迹。

### 2.6 八个必答问题（修订后）

| # | 问题 | 答案 |
|---|---|---|
| 1 | 允许 / 拒绝条件 | **允许**：`status IN ('open','reviewed')` + `expected_epoch`/`expected_max_boot_seq` 双 CAS + `confirm_boot_seq` 与请求一致且 ≤ 证据中的设备当前值 + `evidence` 非空 + `reason` 合法。<br>**拒绝**：`status='incident'`；任一 CAS 不匹配；`prev_epoch_max` 非空且未消解（说明还没走出上一代，需先确认是否同一事件）；缺 evidence；`rebase_id` 已用但参数不同 |
| 2 | 合法重置 vs 未调查回退 | **只能靠外部证据**（`[Log] recovery=PROVISION`、擦除记录）。无证据 ⇒ 停在 `reviewed`，**不得** rebase |
| 3 | 审计字段 | 见 §2.5；旧基线、旧 `rewind_count` **永久留存** |
| 4 | 与并发 ingest | CAS 争同一行；ingest 的 `MAX()` 先提交则 CAS 失败 ⇒ 重读后重试（≤3 次），仍失败返回 `409` |
| 5 | 迟到 / 重投 / 撞键 | 迟到 ⇒ `ambiguous_forward`（`observed` 升、`max` 不动）；重投 ⇒ `same`；撞键 ⇒ 仍由 B-3 三态处理，**rebase 不触碰 `log_history`** |
| 6 | 防未授权 / 重放 | Bearer + timing-safe；`rebase_id` 主键去重；仅 `POST`；可选 `request_ts` ±5 min；令牌未配置统一 401 |
| 7 | 失败是否保持原值 | **是**。CAS 未命中 ⇒ 零写入；审计失败 ⇒ batch 回滚。结果不确定 ⇒ 用同一 `rebase_id` 重试（幂等） |
| 8 | 如何测 | 真实 D1 五类：① rebase 后连续 `boot=18` 重投 ⇒ `rewind` 不增；② `boot=12` ⇒ **仍计** `rewind`；③ `boot=100` ⇒ `ambiguous`、`max` 不变、`observed=100`；④ `boot=120` ⇒ `new_epoch`、`max=120`、`prev_epoch_max=NULL`；⑤ 同 `rebase_id` 重放 ⇒ 审计行数不变、`epoch` 不再 +1 |

### 2.7 推荐

**方案 B（管理接口 + 世代模型）+ 模型 B（分离观测与判定）**。

| 维度 | 评价 |
|---|---|
| 安全性 | 高：双 CAS + 条件审计 + batch 事务 + 幂等键 |
| 复杂度 | 中：`0006`（纯 `ADD COLUMN` + 1 表 1 索引）+ 1 模块 + 1 路由 + 2 套测试 |
| 运维成本 | 低：一次 HTTP；`dry_run` 可预演 |
| **不可消除的盲区** | `(max, prev_epoch_max]` 内**不计 rewind**；`observed_max` 可佐证设备仍在前进 |
| 明确拒绝的做法 | 只改小 `max` · `DELETE`+`INSERT` · 定期清零 `rewind_count` · 用 `INSERT OR IGNORE` 吞审计冲突 |

---

## 3. R-1 / R-2：保留短期缓解

| 项 | 决策 | 说明 |
|---|---|---|
| **C-1** | **采用** | watermark 出 `REWIND_CANDIDATE` + 设备侧 `[Log] recovery=` 人工核对；**不自动处置**；附 runbook（在批次 1 文档里给） |
| **C-4** | **采用** | 冲突落库失败 ⇒ 同请求**重试 1 次**；仍失败 ⇒ 结构化 `console.error`（含 `device_id/boot_seq/seq/kind`）；**只能降低**证据丢失概率，**不能保证绝不丢失** |
| **C-2** | **暂缓** | NVS 存 `log_gen` 解决不了"NVS + LittleFS 同丢"；需要可信服务端身份/代际时另行立项 |
| **C-3** | **不实施** | 不因疑似回退而拒 ACK / 标记整批；不改 ACK 语义 |

### 3.1 已知限制（必须写进风险表）

1. 整片擦除后设备侧**无任何信息**可自证"被擦过"（`recovery=PROVISION` 只在串口）；
2. 若云端历史含 `boot_seq=1..N`，重放会撞 `ux_lh_dedup`，内容被 `OR IGNORE` 丢弃；
3. 该丢弃**不可逆**，只能靠 `log_conflict` 事后留证（且 ACK 后 D1 失败时连证据都会丢）；
4. `C-4` 只是把"完全静默"降为"大概率有日志可查"。

---

## 4. A1-03：启动日志可观测性（`O-A`）

### 4.1 现状事实

- 启动事件 `LOG_SYS_BOOT_COMPLETE = 0x0101`，当前只带 `LOG_P_INIT_MS`（`main.cpp:628`）；
- 已有 `LOG_P_BOOT_SEQ = 0x21`；
- `LOG_P_MAX = 0x59` 且**两处 static_assert** 锁定（`F-1/F-2`）；
- `LOG_MAX_PARAMS = 8`，加 4 个后共 5 个，**不超限**（`F-3`）；
- **仓库内无 TLV 解码器**（`F-4`）；云端把 payload 当不透明（`F-5`）；契约文档**未**定义参数表（`F-6`）。

### 4.2 设计（加性）

在 `LOG_SYS_BOOT_COMPLETE` 追加 4 个 PID：`reserved_hw` / `recovery_src` / `failclosed` / `trace_corrupt`。
每次启动**只发一次**；不新增 event ID、topic、envelope；不发高频日志。

### 4.3 实施前必须完成的核验（否则报 `BLOCKED`）

| # | 核验项 | 通过判据 |
|---|---|---|
| V-1 | PID 空位 | 在 `0x00…0x59` 之外选 4 个（建议 `0x5A…0x5D`），**并同步抬 `LOG_P_MAX` + 改两处 static_assert + `probe_ok.cpp`**；全仓 grep 无冲突 |
| V-2 | 参数数量 | 启动事件 ≤ `LOG_MAX_PARAMS(8)`（5 ≤ 8 ✓），但**必须**用测试断言，不靠估算 |
| V-3 | 未知 PID 行为 | **现有仓库无解码器** ⇒ 必须二选一：<br>(a) 在批次 3 补一个**最小解码器/测试**，显式断言"遇到未知 PID 跳过且不报错"；<br>(b) 出具"无解码器 + 云端不透明"的证据链，并**在契约文档中新增参数表与未知 PID 规则** |
| V-4 | 契约文档 | `docs/interfaces/log_envelope_contract.md` 或新建 `log_params_contract.md` 登记 4 个 PID 的类型/单位/取值域 |
| V-5 | failclos 边界 | 若设备处于 `failclosed=1`，`log_emit()` 会被短路 ⇒ **该诊断日志发不出去**。必须在文档写明"看不到这条 ≠ 没问题"，并保证**不因发日志失败而改变启动流程** |

### 4.4 明确不做

- 不把积压量/上传进度做成新遥测（需协议授权，且会推翻 `WP-6` 容量结论）；
- 不发周期性统计日志；
- 不改 `LOG_SYS_BOOT_COMPLETE` 之外的任何事件。

---

## 5. 风险清单与系统级关闭条件（`rev.2`）

| ID | 风险 | 状态 | 关闭条件 | 谁决定 |
|---|---|---|---|---|
| **R-1** | 整片擦除后 `boot_seq` 从 1 重放 | `OPEN`（设备侧无解） | 只能"检测 + 人工处置" | 产品 |
| **R-2** | 撞历史键 ⇒ 内容静默丢失 | `OPEN`（结构性） | `C-2` 或服务端代际；单独不闭合 | 产品 + 协议 |
| **R-2b** | ACK 后冲突证据丢失 | `OPEN`（`C-4` 缓解） | 重试 + 结构化告警落地；**无法无损** | 技术 |
| **R-11** | 水位被历史峰值污染 | `OPEN`（方案 `rev.2` 已出） | 批次 1 实施 + 盲区声明 | 技术执行 / 产品接受盲区 |
| **R-11b** | rebase 后歧义带盲区 | **新增**，`ACCEPTED WITH LIMITATION`（仅当 `P-1` 批准） | 无法消除 | 产品 |
| **A1-03** | 远程可观测性 | `PARTIAL`（仅串口） | 批次 3 落地 | 接口负责人 |
| **H-3-7-b-2** | `log_conflict` 生产路径未验证 | `OPEN` | `P-4` 暂缓 ⇒ 保持 `OPEN` | 用户 |
| **R-3** | 物理断电 | `PASS (14 cycles)` | —— | —— |
| **R-3b** | 掉电写窗口 | `ACCEPTED RISK` | 已裁决不做检测 | 已决 |
| **D-1** | `0004/0005` 头部"草案" | `OPEN`（文档） | 批次 1 的 README 澄清 | 技术 |
| **BT-1 / G-4 / WP-5 / STOP-R1** | 门禁 | `OPEN` / `BLOCKED` / 未解锁 / 无解除 | 各自流程 | 用户 |

### 5.1 系统级关闭条件

1. `R-1` 有产品结论；2. `R-2` 有结论；3. `R-11`（含 `R-11b` 盲区）实施并声明；4. `A1-03` 落地或明确接受仅串口；5. `R-2b`/`H-3-7-b-2` 状态明确且有人负责；6. 四道门禁按各自流程关闭（**本任务不代决**）。

---

## 6. 批次划分

| 批次 | 主题 | 实施指令 | 本轮状态 |
|---|---|---|---|
| **1** | R-11 rebase（`0006` 草案 + 模块 + 管理接口 + 测试） | `HD-3-Batch1-Watermark-Rebase-Instructions.md` | **指令已出，待批准执行** |
| **2** | `C-4` 冲突证据补偿 | `HD-3-Batch2-Conflict-Evidence-Instructions.md` | **指令已出，待批准执行** |
| **3** | `A1-03` 启动 4 个 PID | `HD-3-Batch3-Boot-Diagnostics-PID-Instructions.md` | **指令已出，待批准执行** |
| **4** | `C-2` 可信代际标识 | —— | **暂缓**（仅保留启动条件，见 §7） |

---

## 7. 批次 4（暂缓）：`C-2` 启动条件

只有同时满足才可立项：

1. 明确**可信代际来源**（服务端签发 / 产线写入 / 安全存储）；
2. 明确**离线行为**（长期无网时代际如何推进、如何避免回退）；
3. 明确**双擦除恢复**（NVS + LittleFS 同丢后的身份与代际重建）；
4. 明确**协议兼容**（老固件缺字段的默认值、唯一键迁移、历史行回填）；
5. 明确**风险边界**（误判代价、回滚方案、双写过渡期）；
6. 取得**单独授权**。

---

## 8. 结论

**`PLAN READY`** —— `P-1…P-6` 已落定；`R-11` 的分类模型改为「分离观测与判定」，并**明确保留不可消除的代际盲区**；
CAS 与审计原子性有官方事务语义 + `meta.changes` 判定 + 条件插入三重保证；
批次 1–3 的独立实施指令已生成，**均不隐含生产授权**；批次 4 明确暂缓。

**未做的事**（重申）：未改业务代码、未建/应用 migration、未写 D1、未部署/回滚 Worker、未操作设备、未提交 commit、未动门禁。
