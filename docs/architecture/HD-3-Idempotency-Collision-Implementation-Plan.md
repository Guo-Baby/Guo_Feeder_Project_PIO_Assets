# HD-3 · 幂等键碰撞（`ts=0` × 序号归零）—— 只读核验与实施计划

> ## 状态：**`PLAN ONLY`**（**`rev.3`** · `2026-10-18`）
>
> - **`rev.3`（`2026-10-18`）= `H-3-2A` 生产只读查询已执行并归档**：新增 **§15.10 实测记录**
>   （`2026-10-18T13:40Z` · **6 条语句** · 6/6 `rows_written=0` · `changed_db=false` · `size_after` 恒定）。
>   **关键实测**：生产 schema 与仓库 `0003` **逐字一致**（1 表 + 2 索引，**仅 1 个 `UNIQUE`**，**无 `CHECK`**）·
>   当前表内 **180 行 / 1 台设备 / 2.804 天**（`10-15T17:31Z`–`10-18T12:49Z`）· `ts_zero = 0` ·
>   `boot_seq` 82–104 **无重复**（8 个值有行，15 个缺号）· `seq` 105298–110903。
>   **★ 定级未变**：`log`/`telemetry` = `CODE-PROVEN RISK`；"是否曾发生真实碰撞/丢失" = `UNRESOLVED`。
>   **配套实施记录**（`A-1` / `B-3` **本地实现 · 未部署 · 未烧录**）：
>   `docs/architecture/HD-3-A1-B3-Implementation-Record.md`（`IMPLEMENTED - LOCAL ONLY`）。
>
> - **`rev.2`（`2026-10-18`）= `H-3-2` 只读查询方案定稿**：新增 **§15**（查询清单 `Q-0…Q-7` · 成本/敏感性/替代 ·
>   执行顺序与最小集 · 证据边界 `E-1…E-6` · 判读矩阵 · `H-3-2-a…f`）；并**撤回** `rev.1` 中两处
>   "只读查询可把定级抬到 `CONFIRMED` / 降到 `NOT REPRODUCIBLE`"的表述（方法学不成立，理由见 §7.2 / §15.6）。
>   **★ `rev.2` 未执行任何查询**，也不改变任何风险等级与门禁。
>   **配套执行清单**（另立文件、可审批）：`docs/architecture/HD-3-H3-2A-Read-Only-Execution-Checklist.md`
>   （`rev.1` · **`APPROVAL PENDING`** · 6 条语句 = 3 `SELECT` + 3 `EXPLAIN`）。
> - **本文件只做两件事**：① 只读核实"不同日志是否可能被判重复而静默丢弃"；② 给出方案比较与实施计划。
> - **未改任何产品代码 / 测试 / migration / 配置**；**未部署**；**未执行任何生产写入**；**未 commit**。
> - **门禁未变**：`BT-1` = `OPEN` · `G-4` = `BLOCKED` · `WP-5` 未解锁 · `STOP-R1` 无书面解除。
> - **不复测、不改写** `RET-1` 的历史验收结论（`RET-1` = 实现完成 · 未部署 · 生产风险仍在）。
>
> **上位/关联文档**：`docs/interfaces/cloud_protocol.md` §4.5.4 / §8.2 ·
> `docs/review/P3-WP4-Firmware-Telemetry-Decisions.md`（§3 派生后果 2 · `R-FRZ-2`）·
> `docs/review/Phase-C-C1-Telemetry-Publisher-Implementation-Review.md`（`P-4` / `HD-3` / `S-6`）·
> `docs/review/RET-1-Retention-Time-Boundary-Investigation.md`（**§6 `G-4`**）·
> `docs/architecture/P3-WP6-Capacity-Verification-Report.md` §7.3

---

## §1 背景与问题定义

### 1.1 待验证命题（原始表述）

> 固件日志的 `ts=0` 与 `seq` 归零或重置可能共同导致云端幂等键碰撞，
> 使不同的日志记录在 `INSERT OR IGNORE` 时被判定为重复，从而静默丢失。

### 1.2 ★ 核验后的修正（**本文件最重要的一条**）

**该命题对 `log_history` 域不成立；对 `telemetry_history` 域成立。** 二者不可混谈：

| 域 | 实际去重键 | `ts` 是否参与 | "`seq` 归零"是否真实 | 结论 |
|---|---|---|---|---|
| **`log_history`** | `(device_id, boot_seq, seq)` | ❌ **不参与** | ❌ `seq` **跨重启持久化**（`meta.bin` 区间预留） | `ts=0` 与去重**无关**；碰撞的真因是 **`boot_seq` 复用 + `seq` 复用** |
| **`telemetry_history`** | `(device_id, type, ts, seq)` | ✅ 参与 | ✅ `s_seq` **每次 boot 归零** | 命题**成立**（`ts=0` 时跨 boot 的 `(dev,type,0,n)` 必碰撞） |

⇒ 因此本文件把原命题拆成两个独立的子问题，分别定级（§7）。**`ts=0` 在 log 域只是"与碰撞同窗口"的相关标记，不是原因**——这一点如果搞错，会导致"修 `ts=0` 就能解决碰撞"的错误结论。

### 1.3 为什么要现在查

| 事实 | 来源 |
|---|---|
| `ts=0` 行**确实在生产产生**（单次冷启动一次爆发 26 行，含 3 条 `CRITICAL`） | `RET-1` 调查（已实证） |
| 该批行已因保留策略被删（`26 → 0`） | `RET-1`（不可恢复） |
| "`ts=0` 行是否已成为 `ux_lh_dedup` 碰撞源（`boot_seq` 归零？）" **尚未取证** | `RET-1` §6 **`G-4`**（标注"代理（只读）"，未执行） |
| telemetry 侧碰撞面**在设计评审中已登记**但**从未闭环** | `R-FRZ-2` / `P-4` / `HD-3`（均 `OPEN`） |

---

## §2 当前实际代码链路（逐跳字段变化）

| 跳 | 位置 | 实际行为（代码取证） |
|---|---|---|
| **① 产生** | `src/log/log_manager.cpp:1765-1782` | `rec.timestamp = 0` 预置；仅当 `time_get() > 0` 才写入并对 `flags` 置 `LOG_FLAG_TIMESTAMP_VALID`（`log_events.h:391`）⇒ **未同步时 `ts` 恒为 0** |
| **② 编号** | `log_manager.cpp:1851-1853` | 临界区内 `s_seq_last++`；`rec.seq = s_seq_last`；`rec.boot_seq = s_boot_seq` |
| **③ 落 Flash** | `log_manager.cpp` 段环形（16 段 × 31 槽 = **496 条**） | 记录原样写入段；**INFO 也消耗 seq，但不落 Flash** ⇒ Flash 内 `seq` **稀疏** |
| **④ 组信封** | `log_envelope.h:23` / `log_envelope.cpp:295` | `{device_id, boot_seq, seq, ts, level, payload_base64}`；`ts` 直接取 `rec.timestamp`（未同步 = `0`） |
| **⑤ 上行** | MQTT `guo_feeder/<dev>/log` → EMQX 规则 A | topic 携带 `device_id`；载荷为上述 JSON |
| **⑥ 归一化** | `Cloudflare_Assets/guo-feeder-api/src/history-ingest.js:566-640` | `ts = asInt(rec.ts ?? env.ts)`；`bootSeq`/`seq` 同理（记录级优先，回落信封）；**`asInt(0)` = `0`（合法整数）⇒ `ts=0` 不被拒收** |
| **⑦ 入库** | 同文件 `:682` | `INSERT OR IGNORE INTO log_history (device_id, ts, level, code, boot_seq, seq, message, payload) VALUES (…)` |
| **⑧ 冲突** | D1 唯一索引 `ux_lh_dedup` | 冲突 ⇒ 静默不插入；`meta.changes = 0` |
| **⑨ 计数** | `history-ingest.js:319` | `duplicate = rows.length - inserted` ⇒ **"被忽略"一律计为 duplicate** |
| **⑩ 采信** | `src/log-ack.js:137` | 对 `changes=0` 的行做 `SELECT 1 … WHERE device_id=? AND boot_seq=? AND seq=?` ⇒ 只能证明"**该键已有行**"，不能证明"**是同一逻辑记录**" |

> **★ 关键结构事实**：`payload_base64` 在云端是**不透明字符串**（不解 CBOR），因此固件写在 CBOR 批次头里的
> `LOG_BATCH_FLAG_SEQ_RELIABLE` / `self_degraded` / `drop_*` **云端一律读不到**（`grep self_degraded|seq_reliable src/` = 0 命中）。

---

## §3 固件 `seq` 生命周期（H1）

### 3.1 定义与持久化机制

| 项 | 事实 | 取证位置 |
|---|---|---|
| 类型/范围 | `uint32_t`；`0` = 未分配（`log_ack.h:42` 明确 0 视为非法） | `log_events.h:461-462` |
| 初值 | `s_seq_last = 0`（静态） | `log_manager.cpp:106` |
| 分配 | 临界区内 `s_seq_last++`，单调，**只在区间耗尽时才重新预留** | `:1851` |
| 持久化 | `meta.bin`（32 B，含 magic/version/boot_seq/**seq_reserved**/corrupt/CRC32） | `:1297-1370` |
| 预留粒度 | `LOG_SEQ_RESERVE = 256`：`base = reserved+1`，`reserved += 256` **立即落盘** | `log_events.h:511` · `:1434-1458` |
| 重启恢复 | `seq_init()`：`have_meta ? reserved = meta : reserved = ceil(max_seq/256)*256`；`s_boot_seq = boot + 1` | `:1460-1506` |
| 重启后的 `s_seq_last` | **只按 `s_seq_base - 1` 设定，不扫描 Flash 实际最大 seq** | `:1449-1452` · `:1499-1502` |
| 写失败降级 | `s_seq_reliable = false`，仍继续分配（`base = s_meta_reserved+1`） | `:1495-1505` · `:1830-1842` |

### 3.2 ★ 序号重放的全部已发现路径

| # | 路径 | 触发条件 | `boot_seq` | `seq` | 是否会产生**已用过的 `(boot_seq,seq)`** | 证据 |
|---|---|---|---|---|---|---|
| **R-1** | `meta.bin` **写失败**（且持续） | LittleFS 满 / 只读 / IO 异常（`meta_write` 返回 false） | **冻结在 `persisted+1`**（每次 boot 同值） | 每 boot 从 `stale_reserved+1` 重新开始 | **✅ 会**（两 boot 同 `boot_seq` + 同 `seq` 区间） | `:1439-1456`（失败不更新持久值）· `:1490` · `:1499` |
| **R-2** | **`meta.bin` 丢失/CRC 错 + Flash 段也空** | 全片擦除 / 分区格式化 / 重新配网时 `uploadfs` / 段全损 | `boot = 0` ⇒ **`1`** | `max_seq = 0` ⇒ **从 `1`** | **✅ 会**（重放该设备"第一次开机"的号码段） | `:1313-1338`（读失败）· `:1481-1486`（重建）· `:1490` |
| **R-3** | `meta.bin` 丢失但 Flash 段完整 | CRC/版本不符、文件被删 | `1` | 由 `max_seq` 续写（**不复用**） | ❌ 不会（`seq` 已越过历史值） | `:1479-1490` · `:1387-1420` |
| **R-4** | `uint32` 溢出 | `boot_seq` 每次开机 +1；`seq` 每 256 一条 | — | — | ❌ 实际不可达（按 1000 条/天计需 > 1 万年） | 类型分析 |
| **R-5** | 单 boot 内重复分配 | 预留失败但 `s_seq_last` 继续 `++` | 不变 | **不复用**（`if (s_seq_last < base-1)` 保护） | ❌ 不会 | `:1449-1452` · `:1838-1841` |

**结论（H1）**：`seq` **不是"每次开机归零"**（这与 `telemetry` 的 `s_seq` 有本质区别）。
log 域的序号重放需要 **`meta` 写失败（R-1）** 或 **`meta` + 段同时丢失（R-2）** 才可达；
且必须 `boot_seq` 与 `seq` **同时**重复，才会产生与历史行相同的去重键。

### 3.3 次要观察（非碰撞，但同源）

- 预留失败后 `s_seq_limit < s_seq_last` ⇒ **每次 emit 都重试一次 `meta_write`**（Flash IO 反复失败），属**写放大/卡顿**面，非本次目标但应在实施时一并复核。
- `meta.bin` **只有一份**，无备份槽、无 A/B 轮换 ⇒ 单点失效直接落到 R-1/R-2。

---

## §4 `ts=0` 的产生路径

| # | 条件 | 代码 | 结果 |
|---|---|---|---|
| **P-1** | 从未同步过时间（无 RTC 值 / SNTP 未成功） | `log_manager.cpp:1774-1782`：`time_get() > 0` 才写 | `timestamp = 0`，`flags` 不置 VALID ⇒ 信封 `ts: 0` |
| **P-2** | 冷启动早期（时间同步完成前产生的记录） | 同上 | 同上（**时间到位后立即恢复正常**） |
| **P-3** | 同步后回退到 0 | 未见代码路径 ⇒ **`UNRESOLVED`**（不在本轮推断范围） | — |

**与序号重放的真实关联**：两者**都集中在"开机初期"**（时间未同步 + meta 刚读/刚写），所以**同时出现**是常态；
但**因果上互不依赖**——`ts=0` 不影响 log 去重键，序号重放也不需要 `ts=0`。
⇒ **"修 `ts=0`" 不能消除 log 碰撞；"修序号重放" 也不能消除 `ts=0`**（后者归 `RET-1` 的 `created_at` 兜底，已实现未部署）。

---

## §5 云端幂等键与 D1 唯一约束（H2）

### 5.1 约束实况（以已应用 migration 为准）

| 项 | 值 | 位置 |
|---|---|---|
| 表 | `log_history`（`id` 自增主键；`device_id`/`ts`/`level`/`boot_seq`/`seq` 均 `NOT NULL`） | `migrations/0003_p3_history_tables.sql:72-101` |
| **唯一索引** | **`ux_lh_dedup ON log_history(device_id, boot_seq, seq)`** | `:126-127` |
| 普通索引 | `ix_lh_device_ts(device_id, ts DESC)` | `:122-123` |
| 协议契约 | "去重键 = `(device_id, boot_seq, seq)`；重传时 `seq` 不变" | `docs/interfaces/cloud_protocol.md:801` |
| telemetry 对应 | `ux_th_dedup ON telemetry_history(device_id, type, ts, seq)` | `:117-118` |

### 5.2 应用层是否有"去重键构造"

**没有独立构造**：云端**不做应用层去重**，只依赖数据库唯一约束 + `INSERT OR IGNORE`。
`normalizeLog()` 只保证类型与存在性（`asInt` 接受整数与纯数字字符串；`-?\d+` ⇒ **负数也接受**），**不做任何唯一性判断**。

### 5.3 冲突时"被忽略的内容"与可观测性

| 问题 | 事实 |
|---|---|
| 冲突时谁赢 | **先到者保留**，后到者**整行丢弃**（`message`/`payload`/`ts`/`level`/`code` **全部丢失**，不合并、不更新） |
| 调用方能否识别 | **只能识别"这行没新增"**：`duplicate = rows.length - inserted`；**无法区分**"合法重试"与"不同记录撞键" |
| 是否存在其他会导致忽略的约束 | `INSERT OR IGNORE` 会忽略 `NOT NULL`/`CHECK` 违反；但 `normalizeLog` 已保证五个键字段非空、`level` 已白名单 ⇒ 实际只剩 `ux_lh_dedup`（**已在代码注释中明示**：`changes=0` 的原因"未知"，不得反推 duplicate，见 `history-ingest.js:697-700`） |
| 是否是"应用已证明是合法重试" | ❌ **不是**。`log-ack.js` 的存在性校验（`:137`）只回答"该键有行吗"，不回答"是不是同一条日志" |

> ⇒ **H2 结论**：`INSERT OR IGNORE` 的"忽略"在语义上是 **"键已存在"**，而不是 **"已证明是重传"**。
> 当前系统**没有任何环节**能区分这两件事。这是本缺陷的**可观测性根因**。

---

## §6 四类碰撞案例推演（H3 · 纯代码推演，未写任何数据）

记号：`D` = 设备 12 hex；`B` = `boot_seq`；`S` = `seq`；`K` = 云端去重键。

### 6.1 案例 1 · 同一条日志正常重试（**应保持幂等**）

| 项 | 内容 |
|---|---|
| 输入 | 同一条 `LogRecord`（`S=110885`, `B=104`, `ts>0`）连发 3 次（ACK 超时重试） |
| 固件 | 重试**不重新分配 seq**（同一记录在 `s_cloud_q` 中保留原 `seq`） |
| Envelope | 三次完全相同的 `{D,B=104,S=110885,ts,level,payload_base64}` |
| K | `(D,104,110885)` ×3 |
| DB | 第 1 次插入（`changes=1`）；第 2、3 次命中 `ux_lh_dedup`（`changes=0`） |
| 是否误丢 | ❌ 不误丢（**应为 duplicate**） |
| 结论 | ✅ **幂等语义正确**；生产实证见 `P3-WP6-Capacity-Verification-Report.md:438`（`boot_seq/seq` 无重复） |

### 6.2 案例 2 · 不同日志、不同 `seq`、但 `ts=0`（**原命题关注点**）

| 项 | 内容 |
|---|---|
| 输入 | 同一次 boot 内两条不同记录：`(B=104,S=110885)` 与 `(B=104,S=110886)`，均 `ts=0` |
| 固件 | `seq` 单调递增 ⇒ 两值不同 |
| Envelope | `ts:0` 但 `S` 不同 |
| K | `(D,104,110885)` vs `(D,104,110886)` ⇒ **不同** |
| DB | 两行都插入成功 |
| 是否误丢 | ❌ **不碰撞** |
| 结论 | ★ **`ts=0` 单独不足以造成 log 碰撞**（`ts` 不在键内）。同形记录在 telemetry 域则**会**碰撞（键含 `ts`） |

### 6.3 案例 3 · `seq` 重置后，不同日志复用同序号 + `ts=0`（**真实碰撞面**）

| 项 | 内容 |
|---|---|
| 触发 | 路径 **R-1**（`meta.bin` 持续写失败）或 **R-2**（`meta`+段同时丢失） |
| 输入 | Boot N：`(B=105, S=1201)` 已上传入库（`ts=0`，`level=CRITICAL`）。Boot N+1：`meta` 写失败 ⇒ `s_boot_seq` 仍 `105`，`seq` 从 `1201` 重新开始 ⇒ 新记录 `(B=105,S=1201)`（内容完全不同） |
| Envelope | 键字段**逐字相同**，仅 `payload_base64`/`message`/`phase` 不同 |
| K | `(D,105,1201)` = 已存在 |
| DB | `INSERT OR IGNORE` ⇒ `changes=0` ⇒ **整行丢弃** |
| 是否误丢 | ✅ **是**（不同日志被判重复） |
| 可观测性 | 响应只多一个 `duplicate`；设备侧（若 ACK 已上线）会把该批当"已确认"⇒ **本地段可回收 ⇒ 该日志永久消失** |
| 结论 | 🔴 **CODE-PROVEN RISK**（路径可达、条件明确）。**`ts=0` 不是必要条件**（`ts>0` 也会撞，只要键相同） |

### 6.4 案例 4 · `ts` 有效但 `seq` 重复 / 字段缺失 / 归零

| 变体 | 处理 | 是否误丢 |
|---|---|---|
| `ts>0` 且 `(B,S)` 与历史相同 | 同案例 3（键命中） | ✅ 会 |
| `ts>0`、`(B,S)` 不同 | 正常入库 | ❌ 不会 |
| `boot_seq` 缺失（信封无该字段） | `normalizeLog` 回落 `env.boot_seq`；仍缺失 ⇒ `asInt(null)=null` ⇒ **记录级 `rejected`** | ❌ 不会丢（有 `rejected` 计数） |
| `seq = 0` | `asInt(0)=0` ⇒ 通过校验 ⇒ 入库为 `seq=0`；若同 `(D,B)` 再出现一次 `seq=0` ⇒ **碰撞** | ⚠️ **会**（固件侧 0 表示"未分配"，但云端不校验） |
| `boot_seq = 0` | 同上（固件 `boot_seq` 最小为 1，但云端不校验） | ⚠️ 同形风险 |
| 负数字段（畸形请求） | `asInt` 接受负整数 ⇒ 入库；`(-1)` 与 `(-1)` 同键 ⇒ 碰撞 | ⚠️ 仅畸形输入可达 |

> ⇒ 云端**对键字段无值域校验**（不校验 `seq>0`、`boot_seq>0`、`ts` 范围）。这是**放大面**，
> 但不是正常固件路径可达的（`s_boot_seq` 从 1 起、`seq` 从 1 起）。

---

## §7 风险证据等级与未决事项（H4）

### 7.1 定级

| 域 / 项 | 等级 | 依据 | **不能**由本证据推出什么 |
|---|---|---|---|
| **log 域碰撞（`boot_seq`+`seq` 重放）** | **`CODE-PROVEN RISK`** | 代码路径 R-1/R-2 明确可达（§3.2）；`ux_lh_dedup` 已应用；`INSERT OR IGNORE` 行为确定 | ❌ 不能推出"生产已发生过"；❌ 不能推出"每台设备都会遇到" |
| **telemetry 域碰撞（`ts=0` + `seq` 归零）** | **`CODE-PROVEN RISK`** | `s_seq` 每 boot 归零（`telemetry_publisher.cpp:144`）；`ts=0` 抑制**未实施**（`cloud_protocol.md` §4.5.4 自述）；`ux_th_dedup` 已应用 | ❌ 不能推出"正在丢" —— 该通道当前被配置门关闭（`mqtt.telemetry_enable=false`，`WP-6` `H-A` 实测） |
| **"不同日志被忽略"的实测证据** | **`UNRESOLVED`** | 云端**结构上无法区分**（§5.3）；`RET-1` `G-4`（只读查 `boot_seq` 分布）**未执行** | ❌ 不能因为没有丢失记录就认定"不存在风险"（观测盲区，不是阴性证据） |
| **`ts=0` 的同步失败根因（RTC/VL/未装）** | **`UNRESOLVED`** | `RET-1` `G-2` 未闭环 | — |
| **`meta.bin` 写入失败在真机上的发生频率** | **`UNRESOLVED`** | 无计数器上报（`corrupt` 只累加不出口） | — |
| **同 boot 内 seq 复用** | **`NOT REPRODUCIBLE`** | 临界区保护 + `s_seq_last` 单调（`:1449`,`:1838` 守卫）；已覆盖全部 `++` 点（`:1851`,`:3153`） | 覆盖范围 = 这两个分配点；若有第三方直接写 `s_seq_last` 则本结论失效（`grep` 未见） |

### 7.2 决定定级的关键缺口（只需一次只读）

| ID | 缺口 | 取数方式 | **该查询实际能产出什么** |
|---|---|---|---|
| **`G-4`**（继承 `RET-1`） | 生产 `log_history` 的 `boot_seq` 分布：是否出现 `boot_seq` 小值反复出现 | **✅ 已执行 `2026-10-18T13:40Z`**（6 条语句，见 **§15.10**） | **只是线索**：可回答"生产是否存在序号重放形态"；<br>**不能**单独把定级抬到 `CONFIRMED`，**也**不能因未见异常就降为 `NOT REPRODUCIBLE`。<br>**实测**：该窗口内 1 台设备 / 180 行 / `boot_seq` 82–104 **无重复** ⇒ 未观察到重放 |

> **★ `rev.2` 修正（原表述已撤销）**：`rev.1` 曾写"能把等级抬到 `CONFIRMED` 或 `NOT REPRODUCIBLE`" ——
> 该表述**方法学上不成立**。原因：`log_history` 只保存**成功插入的胜者**，被 `INSERT OR IGNORE` 丢弃的输入
> **不留任何行**；且唯一约束保证**同键至多一行** ⇒ 现存的表**结构上不可能**提供"同键多输入 / 内容不同"的证据。
> 因此该查询只能改变**对"重放是否发生"的置信度**，不能改变**"是否已发生内容碰撞"的证据等级**。
> 完整论证与判读矩阵见 **§15.3 / §15.6 / §15.7**。

> **本轮不执行**：本任务未获生产只读取数授权，也没有设备侧计数器证据。**不做推测性定级**。

---

## §8 备选修复方案比较

### 方案 A · 固件侧（让序号空间不再重放）

| 子案 | 做法 | 评价 |
|---|---|---|
| **A-1 现状增强（推荐起点）** | ① `meta.bin` 改 **A/B 双槽 + 序号**（写入失败仍有上一槽）；② **写失败时用 `flash_find_max_seq()` 重建基线**（而非沿用过期 `meta`）；③ `boot_seq` 写失败时改用**随机/时间派生值**（仍在冻结的 `boot_seq` 字段内，不动契约） | 最小、无契约变更、无 migration；把"重放"变成"不重放" |
| A-2 `ts` 未同步时**不发日志** | 与 `cloud_protocol.md` §4.5.4 的 telemetry 策略对齐 | ❌ 对 log **不适用**：开机期恰恰是最需要日志的窗口（`RET-1` 的实证损失就是这类） |
| A-3 信封新增 `boot_id`/`content_hash` 字段 | 彻底消除歧义 | ❌ **动冻结 6 字段契约** ⇒ 架构变更 + 云端解析改造，超出最小修复 |
| A-4 `meta` 失败即停止写日志 | 宁丢不重放 | ⚠️ 可作为**最后降级档**，但会牺牲可观测性；应作为人工裁决项（`H-3-1`） |

### 方案 B · 云端侧（改去重键或规则）

| 子案 | 做法 | 评价 |
|---|---|---|
| B-1 键加 `ts` | `(device_id,boot_seq,seq,ts)` | ❌ 对 log **无效**（`ts=0` 时全为 0，不增加区分度）；且会让**同一记录在 `ts` 变化时重复入库**（例如重试时 `rec.timestamp` 不变，尚可；但语义上把"时间"塞进身份键很脆） |
| B-2 键加 `created_at` / `id` | 用入库时刻 | ❌ **破坏幂等**（每次重传都是新行） |
| **B-3 碰撞可见化（推荐）** | `changes=0` 时把已有行的 `payload` 与来件比对：**相同 ⇒ 真重试**；**不同 ⇒ `collision`**（新计数 + 结构化日志，**不改变写入语义**） | ✅ 最小、无 migration、可回滚；把"无法区分"变成"可区分"；成本可复用 `log-ack.js` 已有的一次 `SELECT`（`:137`），**不额外增加查询数** |
| B-4 冲突时**覆盖**旧行 | `INSERT … ON CONFLICT DO UPDATE` | ❌ 会让重传反复覆写（`created_at` 变化），且掩盖问题 |

### 方案 C · 存储模型/约束

| 子案 | 做法 | 评价 |
|---|---|---|
| C-1 去掉唯一约束 | 允许重复行 | ❌ **放弃幂等** ⇒ 重传风暴会放大写入（`BT-1` 未修时 ×6~×30） |
| C-2 按 boot 分区/分表 | `log_history_<boot>` | ❌ 查询与保留策略全面复杂化，收益不足 |
| C-3 键加 `content_hash` 列 | `(device_id,boot_seq,seq,content_hash)` | ✅ 语义最干净（同内容 ⇒ 幂等；不同内容 ⇒ 不碰撞），但需 **migration + 回填 + 固件/Worker 传 hash**，且旧行 `content_hash` 为空需定义语义 ⇒ **高风险，仅作升级路径** |

---

## §9 推荐方案及选择理由

**推荐 = `A-1` + `B-3`（组合，分两步独立授权）**

| 理由 | 说明 |
|---|---|
| 对症 | 碰撞的**根因在设备侧**（序号空间重放），云端**不可能**凭现有信息恢复被丢弃的内容 ⇒ 必须在源头不再重放 |
| 最小 | A-1 只动 `log_manager.cpp` 的 `meta` 写入与 `seq_init` 回退分支；B-3 只改 `history-ingest.js` 的冲突分支（+1 计数 + 日志），**不动 schema、不动 migration、不动契约** |
| 保留幂等 | 两者都不触及"同键同内容 ⇒ 吸收"的正确语义（案例 1 必须继续 100% 幂等） |
| 可验证 | A-1 可用现成的 `s_meta_fail_next` 注入（`log_manager.cpp:1346`,`:3530`）做**故障注入验收**；B-3 可在 L2 `real_d1` 上构造"同键不同 payload"用例 |
| 可回滚 | 均为单文件级改动，回刷/回滚即恢复；无数据不可逆变更（被丢弃的历史行**不可恢复**——只能防未来） |

**不建议本轮做**：A-3 / C-3（动契约或 migration）；B-1 / B-2（无效或破坏幂等）；C-1（放弃幂等）。
**不建议"先修 `ts=0` 再说"**：对 log 无效（§6.2），属另一问题域（`RET-1` 已用 `created_at` 兜底解决保留问题，未部署）。

---

## §10 实施阶段划分（**均须另行授权**）

| 阶段 | 内容 | 前置 | 状态 |
|---|---|---|---|
| **S0** | 人工裁决 `H-3-1…H-3-6`（§13） | — | ⛔ **本文件交付后停在此处** |
| **S1** | 只读取证 `G-4`（`boot_seq` 分布）+ 如有条件取设备侧 `corrupt` 计数 | `H-3-2`（**查询方案已定稿 = §15**） | ⛔ 未授权（**`rev.2`：方案已审查，仅待批**） |
| **S2** | 固件 `A-1`（`meta` 双槽 + 恢复优先级 + fail-closed） | `H-3-1` / 固件批次授权 | 🟡 **本地已实现（`2026-10-18`）· 未烧录** |
| **S3** | 云端 `B-3`（冲突内容比对 + `collision` 计数，走日志不走响应体） | 独立实现授权 | 🟡 **本地已实现（`2026-10-18`）· 未部署** |
| **S4** | 验收：L1 契约探针（`test/log_contract`）+ L2 `real_d1` 碰撞用例 + 生产观察 `duplicate`/`collision` | 部署授权 | ⛔ 未授权 |
| **S5**（可选） | `C-3`（`content_hash` 列 + migration + 回填） | 独立架构裁决 | ❌ 不建议本轮 |

> `A-1` 与 `B-3` **必须分开授权**：前者需刷机（`P2` = `HARDWARE DEFERRED`），后者属云端发布。

---

## §11 测试矩阵与验收标准

### 11.1 固件（`test/log_contract` 契约探针，wasm32 真跑）

| # | 用例 | 期望 |
|---|---|---|
| **T-1** | `meta` 正常：两次 boot 的 `(boot_seq, seq)` 不重叠 | `boot_seq` 严格 +1；`seq` 严格递增 |
| **T-2** | `s_meta_fail_next` 注入 1 次（预留失败） | `seq` **不回退**（`s_seq_last` 保持），`seq_reliable=false` 可观测 |
| **T-3** | **持续**写失败 + 模拟重启 ⇒ 新 boot 的 `seq` 基线 ≥ 已写入的最大 `seq` | **不得**复用任何已分配值（A-1 的核心断言） |
| **T-4** | `meta` CRC 损坏（段完整） | `boot_seq` 重建为 1 但 `seq` 由 `max_seq` 续写；**不产生** Case 3 |
| **T-5** | `meta` + 段全空（模拟全擦） | 允许从 `(1,1)` 重新开始 —— **这是已知可接受残留**，但必须在文档中登记（云端侧由 `G-4` 观测） |
| **T-6** | `ts` 未同步（`time_get()<=0`） | `timestamp=0`、无 VALID 位（既有行为，不得改） |
| **T-7** | `seq` 双分配点（`:1851` 与 `:3153`）一致性 | 两处都受同一守卫（防未来新增分配点绕过） |

### 11.2 云端（L1 `test/current` + L2 `test/real_d1`）

| # | 用例 | 期望 |
|---|---|---|
| **T-8** | 同键同 payload 重投（案例 1） | `accepted=0`、`duplicate=N`、**`collision=0`**（幂等不回归） |
| **T-9** | 同键**不同 payload**（案例 3 构造） | 仍只入库 1 行（语义不变），但 **`collision=1`** 且结构化日志含 `device_id/boot_seq/seq` |
| **T-10** | `seq=0` / `boot_seq=0` / 负值（案例 4 放大面） | 保持现状（不新增拒收）**或**按裁决拒收 —— 属 `H-3-4` 范围，**不得默认改** |
| **T-11** | `EXPLAIN`：冲突比对查询不得退化为全表扫描 | 命中 `ux_lh_dedup` / `ix_lh_device_ts` |
| **T-12** | 预算：`B-3` 不新增每请求 D1 查询数 | 复用 `log-ack` 既有 `SELECT`；`subrequest ≤ 50`（Free 上限） |

### 11.3 验收标准（`ACC-*`）

| # | 标准 |
|---|---|
| **ACC-1** | 案例 1 幂等性**零回归**（重投不产生新行、`collision=0`） |
| **ACC-2** | 案例 3 在固件侧**不再可达**（`T-3` 通过 = 序号不重放） |
| **ACC-3** | 云端**能区分**"重试"与"碰撞"（`T-9`） |
| **ACC-4** | 无 schema / migration / 契约字段变更（`git diff --stat` 仅 2 个产品文件） |
| **ACC-5** | 旧固件（未刷机）与新 Worker 兼容：不刷机的设备仍按现状工作，碰撞几率不升高 |
| **ACC-6** | 生产观察 1 个自然周期内 `collision` 计数与 `duplicate` 分离可读 |

---

## §12 兼容性 / 迁移 / 回滚 / 数据丢失风险

| 维度 | A-1（固件） | B-3（云端） |
|---|---|---|
| 旧设备兼容 | ✅ `meta` 格式升级需 `LOG_META_VERSION` 处理（**版本不符 ⇒ 走重建路径**，等价 R-3，安全） | ✅ 新 Worker 接受旧设备载荷，无字段依赖 |
| 旧 envelope 兼容 | ✅ 字段与顺序不变 | ✅ 不变 |
| DB 迁移 | ❌ 无 | ❌ 无（仅新增计数与日志） |
| 历史数据 | 不变 | 不变（不回填、不修正历史行） |
| 回滚 | 回刷上一版固件即可；**已重放的 `seq` 无法追回** | 回滚 Worker 版本即可（`wrangler rollback`） |
| **不可恢复风险** | 一旦发生碰撞，被丢弃的行**永久缺失**（云端无内容、设备侧最终被淘汰） | 同上（`B-3` 只能**发现**，不能恢复） |
| 对 ACK 影响 | `seq` 不再重放 ⇒ ACK 区间语义更可靠 | 无（ACK 逻辑未变） |
| 对吞吐影响 | 失败回退时多一次 Flash 扫描（496 条读），仅异常路径 | 冲突时多一次行读取（可与既有 `SELECT 1` 合并） |

> **★ 明确声明**：本缺陷若已发生过，**丢失的数据不可恢复**；修复只能"防未来"。
> 这与 `RET-1` 的结论一致（已被删除的行不可恢复）。

---

## §13 需人工决定的事项

| ID | 事项 | 选项 | 建议 |
|---|---|---|---|
| **`H-3-1`** | `meta` 写失败时的降级策略 | ① 继续写 + `boot_seq` 随机派生（不重放，可观测性保留）· ② 停止写日志（宁丢不重放）· ③ 维持现状 | **①**（重放是更坏的结果） |
| **`H-3-2`** | 是否授权**一次生产只读取证**（`RET-1` `G-4`） | ✅ **已批准并执行（`2026-10-18T13:40Z` · 6 条语句 · 零写入）** | **结果见 §15.10**。**★ `rev.2` 修正**：原写"能把定级从 `CODE-PROVEN` 推到 `CONFIRMED` 或反之" —— **已撤销**，见 §7.2 与 §15.6 |
| **`H-3-3`** | telemetry `ts=0` 抑制（`cloud_protocol.md` §4.5.4 **自述未实施**）是否同批做 | 同批 / 单列 `HD-3` | 单列（telemetry 当前配置关闭，非现网风险） |
| **`H-3-4`** | 云端是否对键字段做值域校验（`seq>0`、`boot_seq>0`、`ts` 合理范围） | 是 / 否 | **否**（本轮只做 `B-3`，值域校验属独立契约变更） |
| **`H-3-5`** | 是否引入 `C-3`（`content_hash` 列） | 是 / 否 | **否**（migration 风险 > 收益） |
| **`H-3-6`** | 固件批次授权（刷机）与云端发布窗口是否同批 | 同批 / 分开 | **分开**（`P2` = `HARDWARE DEFERRED`） |

---

## §14 阶段状态与禁止事项

### 14.1 状态

| 项 | 值 |
|---|---|
| 本文件 | **`PLAN ONLY`**（**`rev.2`** · `2026-10-18`） |
| 调查完成度 | 代码链路 ✅ · `seq` 生命周期 ✅ · `ts=0` 路径 ✅ · 幂等键/约束 ✅ · 四案例 ✅ · **`H-3-2` 方案 ✅（§15）· 生产只读实测 ✅（§15.10 · 6 条 · 零写入）** |
| 未完成 | 生产只读取证 `G-4`（**执行清单已定稿 = `HD-3-H3-2A-Read-Only-Execution-Checklist.md`（`rev.1` · `APPROVAL PENDING` · 6 条语句），仍未授权执行**）· `ts=0` 同步失败根因（`G-2`，未授权）· 真机 `corrupt` 计数（无出口） |
| 产品代码 | **零改动** |
| 部署 / 生产写入 | **无** |
| 门禁 | `BT-1 OPEN` · `G-4 BLOCKED` · `WP-5` 未解锁 · `STOP-R1` 无解除 —— **均未变** |
| `RET-1` | **不复测、不改写**其结论（实现完成 · 未部署 · 生产风险仍在） |

### 14.2 禁止事项（本阶段及其后续，直到另行授权）

1. ❌ 不得据本文件直接改固件 / 云端代码 / migration / schema / 索引 / 配置。
2. ❌ 不得部署、不得写生产 D1、不得触发 Cron、不得发 MQTT、不得改 EMQX、不得重启设备。
3. ❌ 不得为了"验证碰撞"向生产写入同键不同内容的记录。
4. ❌ 不得把 `CODE-PROVEN RISK` 表述为"已发生生产事故"，也不得因"没找到丢失记录"表述为"不存在风险"。
5. ❌ 不得把 `B-1`（键加 `ts`）当作 log 域的修复方案。
6. ❌ 不得顺手修改 `RET-1` 的 `created_at` 兜底语义（那是保留期问题，与本缺陷不同域）。
7. ❌ 不得把本缺陷与 `WP-6`、`BT-1` 的遗留项合并处理。

---

## §15 `H-3-2` 生产只读查询方案（`PLAN ONLY` · `READ-ONLY DESIGN REVIEW`）

> **状态**：**`PLAN UPDATED`**（`rev.2` · `2026-10-18`）—— 本方案**只做查询设计**。
> **未执行任何生产查询**（含 `SELECT` / `EXPLAIN`）· 未连接生产库 · 未调用生产 API · 未写任何数据 ·
> 未改代码 / migration / 配置 · 未做任何 Git 操作 · 门禁未变。
> **本节可被授权的唯一动作**：按 §15.5 的最小集执行**一次**只读统计。**不是**实施、**不是**部署、**不是**修复。

### 15.1 结论先行：三类问题的可回答性

| 问题 | 可回答性 | 依据 |
|---|---|---|
| 生产是否真有 `ux_lh_dedup` / `ix_lh_device_ts`（而非只存在于 migration） | ✅ **能直接回答** | `Q-0` 读 `sqlite_master` |
| 生产 `log_history` 的总量、设备数、时间窗、`ts` 三态、键字段值域 | ✅ **能直接回答** | `Q-1` 单次扫描 |
| 生产是否出现 **`boot_seq` 重放形态**（同一 `boot_seq` 值在相隔很久的两个时段各有一批行） | 🟡 **只能给线索** | `Q-2b` / `Q-3`；解释见 §15.6 |
| 同一 `(device_id, boot_seq, seq)` 是否**收到过多条输入** | ❌ **不能** | 表只存胜者；输入流不落库（§15.3） |
| 那些输入的内容是否**不同** | ❌ **不能** | 被 `OR IGNORE` 丢弃的行零留存（§15.3） |
| 是否**确有一条不同内容的日志**因键冲突未写入 | ❌ **不能** | 同上；`duplicate` 计数不区分原因 |
| 能否区分**合法 MQTT 重试**与**真实不同日志碰撞** | ❌ **不能** | `duplicate = rows.length - inserted`（`history-ingest.js:319`）在语义上只是"本次没新增" |
| `ts=0` 的规模 | ✅ **能直接回答** | `Q-1` 的 `ts_zero` 列 |
| `ts=0` 能否证明 log 幂等键碰撞 | ❌ **绝对不能** | `ux_lh_dedup` = `(device_id, boot_seq, seq)`，**`ts` 不在键内**（`0003:126`） |

### 15.2 本轮核对的代码与 schema 依据

| 文件 | 位置 | 核对到的实际事实 |
|---|---|---|
| `migrations/0003_p3_history_tables.sql` | `:72-108` | `log_history` 列 = `id, device_id, ts, level, code, boot_seq, seq, message, payload, created_at`；`code/message/payload` **可空**，其余 `NOT NULL` |
| 同上 | `:122` | `ix_lh_device_ts ON log_history(device_id, ts DESC)` |
| 同上 | `:126` | `CREATE UNIQUE INDEX ux_lh_dedup ON log_history(device_id, boot_seq, seq)` ⇒ **幂等键 = 三列，不含 `ts`** |
| `src/history-ingest.js` | `:682-684` | `INSERT OR IGNORE INTO log_history (device_id, ts, level, code, boot_seq, seq, message, payload)` |
| 同上 | `:566-640`（`normalizeLog`） | `ts/boot_seq/seq` 均由 `asInt()` 保证为**整数**（`/^-?\d+$/` 亦接受**负数**）；`level` 白名单；非法则 `return null`（计 `rejected`，**不进入 SQL**） |
| 同上 | `:193-197` | `deviceId` 来自 topic 第 2 段或 `clientid`；**空串直接 400** ⇒ 不可能以 `NULL` 绑定 |
| 同上 | `:696-701` | `changes === 0` 注释明写"**原因未知**，不得在此反推 duplicate" |
| 同上 | `:319` | `duplicate: rows.length - inserted` ⇒ **不落库、不分原因** |
| `src/log-ack.js` | `:136-137` | `SELECT 1 AS hit FROM log_history WHERE device_id=? AND boot_seq=? AND seq=? LIMIT 1` ⇒ 只证明"**该键有行**" |
| `src/index.js` | `:289-307` | `/api/history/ingest` **硬性早退**，注释明写"本端点**不写 `mqtt_messages`**" |
| `src/index.js` | `:573-636` | `mqtt_messages` 仅由旧 `/api/ingest` webhook 写入；`raw_payload` = `JSON.stringify(redactSecrets(payload))`（**脱敏后的序列化副本，非原始字节**） |
| `migrations/README.md` | `:175` | `mqtt_messages.created_at` 单位 = **毫秒**（`Date.now()`），与 `log_history.created_at`（秒）**不同** |

**由上述事实推出的两条硬结论**（用于 §15.3）：

1. **`log` 域 `INSERT OR IGNORE` 的静默忽略只有唯一来源 = `ux_lh_dedup` 冲突。**
   `id` 为 `AUTOINCREMENT` 不可能冲突；五个 `NOT NULL` 列由 `normalizeLog` 与 `deviceId` 非空校验
   保证非 `NULL`（空串合法，不触发 `NOT NULL`）；表**无 `CHECK` 约束**。
   ⇒ 对 `log` 域，"`changes = 0`" 与 "`ux_lh_dedup` 冲突"**当前等价**
   （**前提**：schema 与 `normalizeLog` 校验集合不变；若未来加 `CHECK`，此等价失效）。
2. **表内不可能存在"同键多行"。** 唯一索引保证 `(device_id, boot_seq, seq)` 至多一行
   ⇒ 任何"键重复"的查询**必然 0 行**，它不是证据，只是约束自检。

### 15.3 ★ 结构性障碍：为什么现存表**无法**证明内容碰撞

碰撞的完整证据链需要四个环节，缺一不可：

| 环节 | 需要什么 | 现存表是否具备 |
|---|---|---|
| ① 输入留存 | 同键的**每条输入**（含被丢弃者） | ❌ **不具备**。`IGNORE` 的行不产生任何记录 |
| ② 内容可比 | 各输入的内容或**可靠指纹** | ❌ 只保存胜者；`payload` 是 base64 原文（不透明），且无 `content_hash` 列 |
| ③ 丢弃可归因 | 能区分"重试"与"撞键" | ❌ `duplicate` 单一计数，不落库 |
| ④ 时间可对齐 | 同一键的多次尝试时刻 | ❌ 只有胜者的 `created_at` |

**推论（本轮最重要的结论）**：

> **对历史上已丢弃的记录，单靠现存 `log_history` 表无法证明、也无法排除"曾发生真实内容碰撞"。**
> 即使 `boot_seq` 重放**确定发生过**，被吞掉的日志也**不会在表里留下任何痕迹**。
> 因此"查询未发现异常"**不是阴性证据**，只是"观测盲区未被照亮"。

反例说明（帮助判读者建立直觉）：设某设备第一次 boot 写日志 `seq = 1…10`（`boot_seq = 104`），
第二次 boot 因 `meta.bin` 写失败**复用** `boot_seq = 104`、并从 `seq = 1` 重新开始写 3 条。
则这 3 条**全部撞键被吞**，而表内 `boot_seq = 104` 的 `seq` 集合仍是 `1…10`，
`created_at` 全部落在第一次 boot 期间 —— **从表面看完全正常**。
反之若第二次写了 15 条，`seq = 11…15` 会**成功插入**，表现为"同一 `boot_seq` 有两段 `created_at`"，
这才是可被 `Q-3` 捕获的**线索**（且仍是线索，不是证明）。

### 15.4 候选查询清单

**通用约束（对全部查询生效）**

- 一律用 `wrangler d1 execute guofeeder --remote --json`，**单条语句**、**只读**、**不加 `--file`**（避免多语句）。
- 执行前每条先跑 `EXPLAIN QUERY PLAN`（同样只读），确认扫描形态与本节预期一致；不一致则**停止并报告**。
- 输出中的 `device_id` 属设备标识（12 位小写 hex，MAC 派生）⇒ 写入报告时**掩码**（建议保留前 4 位）。
- **不取** `payload` / `message` 正文（`Q-5` 除外，且需单独授权）。
- **成本一律不给臆测数字**：由 `Q-1` 的 `rows` 反推；本节只给**复杂度量级**与**触发停止的阈值**。

| ID | 目的 | 扫描形态（预期） | 复杂度 | 输出敏感性 | 执行条件 |
|---|---|---|---|---|---|
| **`Q-0`** | 约束/索引实况 | 系统表，**不扫业务表** | `O(索引数)` | 无数据 | 无条件（**必跑**） |
| **`Q-1`** | 总量 + 时间窗 + `ts` 三态 + 键值域 | **全表扫 1 次** | `O(N)` | 聚合值，无 `device_id` | 无条件（**必跑**） |
| **`Q-2a`** | 每 `(设备, boot)` 的行数与 `seq` 范围 | **覆盖索引扫描**（预期 `ux_lh_dedup`） | `O(N)`（不读表体） | 含 `device_id` | 建议跑（**最小集**） |
| **`Q-2b`** | 同上 + `created_at` 跨度（**重放线索主查**） | 全表扫（需回表取 `created_at`） | `O(N)` + 分组 | 含 `device_id` | 建议跑 |
| **`Q-3`** | 同一 `(设备, boot)` 内 `created_at` 断层 | 全表扫 + 窗口排序 | `O(N log N)` | 含 `device_id` | **可选**（需先定阈值） |
| **`Q-4`** | 可疑 `boot` 的定点形态（脱敏） | 索引前缀查找 | `O(k)` | 含 `device_id` + `level/code` | 仅当 `Q-2b/Q-3` 命中可疑 |
| **`Q-5`** | 内容比对（`payload` 片段 / `message`） | 索引前缀查找 | `O(k)` | **高**（日志正文） | ⛔ **默认不做**，需单独授权 |
| **`Q-6`** | `mqtt_messages` 历史旁证 | 全表扫（`topic` 无索引，**待确认**） | `O(M)`，`M` 可能远大于 `N` | 含 `device_id` | ⛔ **默认不做**（见下） |
| **`Q-7`** | "同键多行"反证 | 分组 | `O(N)` | 无 | ❌ **不执行**（必然 0 行，`Q-0` 已覆盖） |

#### `Q-0` · 约束与索引实况

```sql
SELECT type, name, tbl_name, sql
FROM sqlite_master
WHERE tbl_name = 'log_history'
ORDER BY type, name;
```

> **★ `rev.2` 定稿形态**：去掉了 `type = 'index'` 过滤 ⇒ **一条语句同时返回**
> ① `type='table'` 的主表 DDL ② 该表上全部索引行 ⇒ 可同时核对"表结构 / 列约束"与"唯一索引列序"，
> 无需拆成两条。执行清单见 `docs/architecture/HD-3-H3-2A-Read-Only-Execution-Checklist.md`（§3.2）。

- **能回答**：生产是否真有 `ux_lh_dedup`（DDL 含 `UNIQUE`）与 `ix_lh_device_ts`，其列顺序是否与 `0003:122/126` 一致；
  以及**主表 DDL 是否与 `0003:72-108` 逐字一致**（这是"生产 schema = 仓库 schema"的**唯一**取证方式）。
- **不能回答**：任何数据分布；也不能证明索引"当前有效"（只证明 DDL 存在）。
- **说明**：`PRAGMA` 类命令在该库有被拒先例（`SQLITE_AUTH`），`sqlite_master` 是普通系统表，通常可读；
  若被拒 ⇒ 记为 `UNRESOLVED`，**不改用其他手段绕过**。

#### `Q-1` · 单次扫描取全部标量指标（**合并 4 个问题，省 3 次全表扫**）

```sql
SELECT COUNT(*)                                        AS rows,
       COUNT(DISTINCT device_id)                       AS devices,
       MIN(created_at)                                 AS ca_min,
       MAX(created_at)                                 AS ca_max,
       SUM(CASE WHEN ts = 0  THEN 1 ELSE 0 END)        AS ts_zero,
       SUM(CASE WHEN ts < 0  THEN 1 ELSE 0 END)        AS ts_neg,
       SUM(CASE WHEN ts > 0  THEN 1 ELSE 0 END)        AS ts_pos,
       SUM(CASE WHEN boot_seq <= 0 THEN 1 ELSE 0 END)  AS boot_nonpos,
       SUM(CASE WHEN seq <= 0 THEN 1 ELSE 0 END)       AS seq_nonpos,
       MIN(boot_seq) AS boot_min, MAX(boot_seq) AS boot_max,
       MIN(seq)      AS seq_min,  MAX(seq)      AS seq_max
FROM log_history;
```

- **能回答**：表规模、设备数、观测窗口、`ts=0` 的**规模**、键字段是否存在非正值（`0` / 负数）。
- **不能回答**：`ts=0` 与碰撞的任何因果关系（`ts` 不在键内）；也不能回答"某设备是否重放"。
- **成本**：一次全表扫，`O(N)`，输出**单行**。`N` 以本查询的 `rows` 为准（本节不代填）。
- **★ 门槛的性质**：13 个指标要求遍历**每一行** ⇒ **任何行数门槛都不能保护这条查询本身免于扫描**；
  `rows` 门槛只是**执行后**"是否继续跑后续查询"的判据。
- **空表边界**：表为空时各 `SUM(...)` 返回 `NULL`（不是 `0`）⇒ 判读时以 `rows = 0` 为准，勿把 `NULL` 读成"无异常"。
- **替代**：无更低成本方案（要这几个指标就得扫一次）。
- **★ 判读红线**：`ts_zero > 0` **不得**写成"存在碰撞风险证据"；`boot_nonpos > 0` / `seq_nonpos > 0`
  **不得**写成"已碰撞"，只能写成"键值域存在异常输入，可能放大碰撞面"。

#### `Q-2a` · 按 `(device_id, boot_seq)` 聚合（**轻量版 · 建议作为最小集**）

```sql
SELECT substr(device_id, 1, 6) || '..' || substr(device_id, -2) AS dev_tag,
       boot_seq,
       COUNT(*)  AS n,
       MIN(seq)  AS seq_min,
       MAX(seq)  AS seq_max
FROM log_history
GROUP BY device_id, boot_seq
ORDER BY device_id, boot_seq;
```

> **★ `rev.2` 同步**：`SELECT` 输出**必须**用 `dev_tag` 掩码（`GROUP BY` 仍用原列 `device_id`，
> 否则会把不同设备合并统计）。`dev_tag` = 前 6 位 + `..` + 末 2 位 ≈ **32 bit**，
> **存在碰撞可能，不是严格唯一的匿名 ID** ⇒ 报告中不得写"唯一标识一台设备"。
> 定稿 SQL 与判定规则见 `HD-3-H3-2A-Read-Only-Execution-Checklist.md` **§3.6**。

- **扫描形态**：三列全在 `ux_lh_dedup(device_id, boot_seq, seq)` 内 ⇒ **预期为覆盖索引扫描，不回表**。
  ⚠️ 优化器也可能选 `ix_lh_device_ts`；**以 `EXPLAIN QUERY PLAN` 实际输出为准**，本节不作断言。
- **能回答**：每台设备出现过哪些 `boot_seq`；每个 `boot` 的 `seq` 区间是否**离奇**（如 `seq_max` 远大于
  `n` ⇒ 大量缺口；或 `seq_min` 不是小数）。
- **不能回答**：`boot_seq` 是否**重放**（同一值出现两次，在聚合后**看起来完全一样**）⇒ 必须配 `Q-2b`/`Q-3`。
- **输出敏感性**：只输出 `dev_tag`（已掩码）；**不输出完整 `device_id`**。

#### `Q-2b` · 同上 + `created_at` 跨度（**重放线索主查**）

```sql
SELECT device_id, boot_seq,
       COUNT(*)  AS n,
       MIN(seq)  AS seq_min,
       MAX(seq)  AS seq_max,
       MIN(created_at) AS ca_min,
       MAX(created_at) AS ca_max,
       MAX(created_at) - MIN(created_at) AS span_s
FROM log_history
GROUP BY device_id, boot_seq
ORDER BY device_id, boot_seq;
```

- **能回答**：某 `(设备, boot_seq)` 的入库时间跨度。**正常的单次 boot 应集中在一次上电周期内**；
  若 `span_s` 远大于该设备的典型在线时长（例如 > 24 h），即**重放线索**。
- **不能回答**：`span_s` 大也可能来自**补传/积压重投**（设备离线后集中上传），两者**无法由本表区分** ⇒ 只是线索。
- **成本**：需读 `created_at` ⇒ **回表**，`O(N)` + 分组；比 `Q-2a` 贵，但仍是一次扫描。
- **替代**：先跑 `Q-2a`（覆盖索引）；仅在需要判断"是否跨时段"时才跑 `Q-2b`。

#### `Q-3` · `boot` 内 `created_at` 断层（**可选 · 参数需人工定**）

```sql
WITH seqd AS (
  SELECT device_id, boot_seq, created_at,
         LAG(created_at) OVER (
             PARTITION BY device_id, boot_seq ORDER BY created_at
         ) AS prev_ca
  FROM log_history
)
SELECT device_id, boot_seq, prev_ca, created_at,
       created_at - prev_ca AS gap_s
FROM seqd
WHERE prev_ca IS NOT NULL
  AND created_at - prev_ca > 21600      -- 6 h · 阈值待人工确认
ORDER BY gap_s DESC
LIMIT 50;
```

- **能回答**：同一 `(设备, boot_seq)` 内是否存在"长时间无入库"的断层 ⇒ **重放的最强形态线索**
  （同一 boot 不可能横跨长时间离线）。
- **不能回答**：断层**本身**不证明第二次的日志被吞 —— 它也可能只是"第二次 boot 的 `seq` 超出原区间、
  全部成功插入"（那种情况**没有丢失**）。⇒ **断层 = 重放线索，≠ 丢失证据**。
- **不该误归因**：`log_history` 的保留窗口为 **180 d**（`WP-7`），**远大于 6 h** ⇒ 该断层**不可能由保留期清理造成**
  （除非窗口被另行改小）。同理也不由 `RET-1` 的 `ts<=0` 语义造成 —— 那只影响"哪些行被删"，不影响"断层是否存在"。
- **成本**：全表扫 + 窗口排序 `O(N log N)`；`LIMIT 50` 只削输出，**不削扫描**。
- **更低成本替代**：**只跑 `Q-2b`**（一次扫描即得 `span_s`），用"跨度是否 > 24 h"做**粗判**，
  不定位断层具体位置。若粗判未命中 ⇒ **不必跑 `Q-3`**（省一次 `O(N log N)`）。
- **窗口函数可用性**：D1 基于现代 SQLite，预期支持；**未在本轮验证**（本轮不得执行任何查询）。
  若执行时报语法错 ⇒ **停止**，改用下方降级式（**仅在 `N` 很小、人工确认后**）。

<details>
<summary>降级式（窗口函数不可用时的替代，O(N²) 最坏，慎用）</summary>

```sql
SELECT a.device_id, a.boot_seq,
       a.created_at AS ca_prev, b.created_at AS ca_next,
       b.created_at - a.created_at AS gap_s
FROM log_history a
JOIN log_history b
  ON b.device_id = a.device_id
 AND b.boot_seq  = a.boot_seq
 AND b.created_at = (SELECT MIN(c.created_at) FROM log_history c
                     WHERE c.device_id = a.device_id
                       AND c.boot_seq  = a.boot_seq
                       AND c.created_at > a.created_at)
WHERE b.created_at - a.created_at > 21600
ORDER BY gap_s DESC
LIMIT 50;
```
</details>

#### `Q-4` · 可疑 `boot` 的定点形态（脱敏版）

```sql
SELECT seq, ts, level, code,
       CASE WHEN message IS NULL THEN 0 ELSE length(message) END AS mlen,
       CASE WHEN payload IS NULL THEN 0 ELSE length(payload) END AS plen,
       created_at
FROM log_history
WHERE device_id = ? AND boot_seq = ?
ORDER BY seq
LIMIT 200;
```

- **能回答**：某可疑 `boot` 内 `seq` 的**连续性**、`ts` 与 `created_at` 的**倒挂**（`created_at ≪ ts` 或 `ts = 0`）、
  `level` 构成、`payload` 体量。
- **不能回答**：仍不能回答"是否丢过"——它只描述**幸存者**。
- **成本**：`ux_lh_dedup` 前缀 `(device_id, boot_seq)` 查找，`O(k)`；`LIMIT 200` 有界。
- **敏感性**：只输出**长度**，不输出正文 ⇒ 可用于报告。

#### `Q-5` · 内容比对（⛔ **默认不做**）

```sql
SELECT seq, ts, level, code, message,
       substr(payload, 1, 24) AS payload_head
FROM log_history
WHERE device_id = ? AND boot_seq = ? AND seq IN (?, ?);
```

- **唯一用途**：人工核对"某两个特定 `seq` 的内容是否像同一段程序里连续产生的"。
- **为何默认不做**：`payload` 是设备 CBOR 的 base64 文本，`message` 是中文描述，可能含配置值 / SSID 等；
  且**即便内容不同，也无法证明"曾被丢弃的那条"存在**（因为被丢的那条不在这张表里）。
- **结论**：本查询**不能**回答 H-3-2B 的核心问题 ⇒ 收益低于隐私成本。**建议不纳入授权范围。**

#### `Q-6` · `mqtt_messages` 历史旁证（⛔ **默认不做**）

```sql
SELECT COUNT(*) AS n, MIN(created_at) AS ca_min_ms, MAX(created_at) AS ca_max_ms
FROM mqtt_messages
WHERE topic LIKE 'guo_feeder/%/log';
```

- **前提**：`mqtt_messages` **只由旧 `/api/ingest` 写入**（`index.js:573`）；当前 log 链路走
  `/api/history/ingest` 并**硬性早退、不写该表**（`index.js:289-307`）。
  ⇒ 除非 EMQX 仍有 Action 指向旧 `/api/ingest`，否则**该表不含当前 log 流**。
  **本轮未查 EMQX 规则**（不在授权范围）⇒ 该前提 = **待人工确认**。
- **即便有数据也不能直接用**：`raw_payload` 是 `JSON.stringify(redactSecrets(payload))`
  ⇒ **已脱敏、已重序列化**，不是原始帧；`created_at` 是**毫秒**，与 `log_history` 秒级不可直接比较。
- **成本**：`topic` 无索引（待确认）⇒ 全表扫；该表自 P0 起累积，`M` 可能**远大于** `N`。
- **更低成本替代**：**先只读查 EMQX 规则的 Action 目标**（一次 API 调用、零 D1 成本）——
  若规则只指向 `/api/history/ingest`，则本表对 log 流**无数据**，直接跳过 `Q-6`（推荐路径）。
- **结论**：**默认不做**。若未来需要 `raw_payload` 作证据，须单独授权并接受"脱敏副本 ≠ 原始帧"的证明力折损。

#### `Q-7` · "同键多行"反证（❌ **不执行**）

```sql
SELECT device_id, boot_seq, seq, COUNT(*) AS c
FROM log_history
GROUP BY device_id, boot_seq, seq
HAVING c > 1;
```

- 由 `ux_lh_dedup` 保证**必然 0 行** ⇒ 无信息量。保留在此仅为**说明为何不列入执行清单**（避免后续误加）。
- 若真返回非 0 行 ⇒ 说明唯一索引缺失或失效，**属重大发现**，应立即停止并报告（不自行修复）。

### 15.5 执行顺序与最小集

| 序 | 查询 | 是否最小集 | 停止条件 |
|---|---|---|---|
| 1 | `Q-0` | ✅ | 无 `ux_lh_dedup` ⇒ **停止**（约束假设崩塌，全部结论作废） |
| 2 | `Q-1` | ✅ | `rows` 超出人工设定上限（建议 `> 100 万`）⇒ **停止**，重新评估成本 |
| 3 | `Q-2a` | ✅ | 覆盖索引预期不成立 ⇒ 记录实际计划，继续（不改 SQL 绕过） |
| 4 | `Q-2b` | 建议 | `span_s` 前列出现 > 24 h ⇒ 记为线索，**不追查** |
| 5 | `Q-3` | 可选 | 窗口函数报错 ⇒ **停止**该条，不用降级式（除非单独批准） |
| 6 | `Q-4` | 条件 | 仅在 4/5 命中可疑 `boot` 时，对 **≤ 3 个** `boot` 执行 |
| — | `Q-5` / `Q-6` / `Q-7` | ⛔ | **不在本轮授权范围** |

**总量**：最小集 = **3 条业务查询**（`Q-0`/`Q-1`/`Q-2a`）；建议集 = **4 条**（+`Q-2b`）。

> **★ 授权计数口径（`rev.2` 增补）**：生产调用必须**分开计数**，不得只报业务查询数 ——
> 最小集 = **3 `SELECT` + 3 `EXPLAIN QUERY PLAN` = 6 条语句**（每条业务查询各配 1 条计划检查）。
> **★ 授权执行顺序（`rev.2`）**：`E-0 → E-1 → E-2 → Q-0 → Q-1 → Q-2a`
> —— **三条 `EXPLAIN` 全部先跑完**（此时尚未读取任何业务行），计划无异常才进入业务查询。
> **定稿执行清单**（逐条 SQL 原文 · 成本与扫描范围 · 停止条件 · 隐私与掩码规则 · 未解决前提 · 签字区）见
> `docs/architecture/HD-3-H3-2A-Read-Only-Execution-Checklist.md`（`rev.2` · **已获 6 条执行授权**）。

### 15.6 证据边界：什么只能算线索

| 观测结果 | **可以**说 | **不可以**说 |
|---|---|---|
| `boot_seq` 出现小值、且同值有多批行 | "存在序号重放的**形态**" | ❌ "已发生碰撞" · ❌ "已丢失日志" |
| 同一 `boot` 的 `created_at` 有 > 6 h 断层 | "该 `boot_seq` 疑似被两次使用" | ❌ "第二次的日志被吞了"（可能 `seq` 超出而全部写入） |
| `ts_zero > 0` | "时间未同步确有发生" | ❌ "与 log 碰撞有关"（`ts` 不在键内） |
| `boot_nonpos > 0` / `seq_nonpos > 0` | "键值域存在异常输入" | ❌ "已碰撞"（只能说明碰撞面被放大） |
| 全部查询**未见异常** | "在**该观测窗口内**未见重放形态" | ❌ `NOT REPRODUCIBLE` · ❌ `PASS` · ❌ `CLOSED` · ❌ "无风险" |
| `duplicate > 0`（`wrangler tail` 实时可见） | "有行未新增" | ❌ "发生碰撞"（无法区分合法重试） |

**要真正证明"曾发生真实内容碰撞/丢失"，必须补齐下列之一**（现存表均不具备）：

| # | 额外证据 | 性质 | 当前可得性 |
|---|---|---|---|
| `E-1` | **输入侧留存**：同键多条原始帧（EMQX 侧留存 / `mqtt_messages.raw_payload`） | 直接 | ⛔ 需确认 EMQX Action 目标；且 `raw_payload` 为脱敏副本 |
| `E-2` | **设备侧未 ACK 队列 / 段内容**（串口只读 dump 与 D1 对账） | 直接 | ⛔ 需设备授权（`P2` = `HARDWARE DEFERRED`） |
| `E-3` | **部署后的 `collision` 计数**（`B-3`） | 前瞻 | ⛔ 属 `S3`，需独立实现 + 部署授权 |
| `E-4` | **可靠内容指纹**（`C-3` `content_hash`） | 前瞻 | ⛔ 需 migration（已判"不建议本轮"） |
| `E-5` | **D1 Time Travel 历史快照比对**（同键在不同时点的内容） | 间接 | ⛔ 需生产读取授权；且只能发现"删除后重插"，**不能**发现"从未写入" |
| `E-6` | **Workers Logs 历史**（`duplicate` 时序） | 间接 | ⛔ 保留期有限，且不区分原因 |

> **结论**：`E-1…E-6` 中**没有一项**能仅凭现有授权取得 ⇒ **`H-3-2` 的查询只能改善"重放是否发生"的置信度，
> 不能把 `UNRESOLVED` 变为 `CONFIRMED`，也不能变为 `NOT REPRODUCIBLE`。** 定级维持
> `log`/`telemetry` = `CODE-PROVEN RISK` · "是否实际发生" = `UNRESOLVED`。

### 15.7 判读矩阵（防止把线索读成结论）

| `Q-2b` / `Q-3` 结果 | 对"重放是否发生" | 对"是否已丢失" | 允许的动作 |
|---|---|---|---|
| 同一 `boot_seq` 出现 > 24 h 跨度 **或** > 6 h 断层 | **线索偏强** | **仍未知** | 记入报告；**不追查**、不改定级 |
| `boot_seq` 严格单调、无断层、无重复 | **该窗口内未见重放** | **仍未知** | 记入报告；**禁止**写 `NOT REPRODUCIBLE` |
| `Q-0` 显示无 `ux_lh_dedup` | — | — | **立即停止**并报告（约束假设崩塌） |
| `Q-1` 的 `rows` 远大于预期 | — | — | **停止**，重估成本与范围 |

### 15.8 需人工决定（`H-3-2` 的子项）

| ID | 事项 | 选项 | 建议 |
|---|---|---|---|
| **`H-3-2-a`** | 授权范围 | 最小集（3 条）/ 建议集（4 条）/ 全套（含 `Q-3`） | **建议集**（4 条，零写入） |
| **`H-3-2-b`** | `Q-3` 的断层阈值 | 6 h / 12 h / 24 h / 不跑 | **6 h**（最敏感，但误报多；须接受"只是线索"） |
| **`H-3-2-c`** | `Q-1` 的 `rows` 上限（超出即停） | 10 万 / 100 万 / 不设限 | **100 万** |
| **`H-3-2-d`** | `device_id` 是否掩码 | 掩码 / 明文 | **掩码**（报告保留前 4 位） |
| **`H-3-2-e`** | 是否同批授权 `Q-5`（内容片段） | 是 / 否 | **否**（不能回答核心问题，隐私成本高） |
| **`H-3-2-f`** | 是否同批确认 EMQX Action 当前目标（决定 `Q-6` 是否有意义） | 是 / 否 | **是**（只读查规则，成本低；但属另一系统，需单独授权） |

### 15.9 本轮边界自检

| 项 | 状态 |
|---|---|
| 生产 `SELECT` / `EXPLAIN` | **0 条**（本节全部为**待授权**的 SQL 文本） |
| 连接生产库 / 调生产 API / 访问管理端点 | **无** |
| 写入 / migration / 部署 / Cron / EMQX | **无** |
| 设备命令 / 重启 | **无** |
| 产品代码 / 测试 / 配置 | **零改动** |
| Git（`commit`/`push`/`reset`/`clean`/`stash`/`checkout`/`rebase`/`merge`/`cherry-pick`） | **未执行** |
| 风险等级 | **未变**（`log`/`telemetry` = `CODE-PROVEN RISK`；"是否实际发生" = `UNRESOLVED`） |
| 门禁 | `BT-1 OPEN` · `G-4 BLOCKED` · `WP-5` 未解锁 · `STOP-R1` 无解除 —— **均未变** |
| 本轮未决策项 | `H-3-1` / `H-3-3` / `H-3-4` / `H-3-5` / `H-3-6` **一律不触碰** |
| `RET-1` / `WP-6` | **未复测、未改写、未修改** |

> **唯一允许的下一步**：人工批准 `H-3-2-a`（+ `b`/`c`/`d`）后，按 §15.5 执行**一次**只读统计并出报告。
> **执行完毕也仍是 `UNRESOLVED`**（除非同时取得 `E-1…E-6` 之一）。

---

### 15.10 ★ `H-3-2A` 生产只读实测记录（`2026-10-18T13:40Z` · **6 条语句** · 零写入）

> **授权依据**：人工 `2026-10-18` 批准 `H-3-2A`，**预算上限 6 条 SQL**，禁增查。
> 执行清单 `docs/architecture/HD-3-H3-2A-Read-Only-Execution-Checklist.md`（`rev.2`）。
> **实际执行 = 6 条，无超出**。原始 JSON 留档（gitignored）：`.wrangler/hd3_E0.json` / `hd3_E1.json` /
> `hd3_E2.json` / `hd3_Q0.json` / `hd3_Q1.json` / `hd3_Q2a.json`（`.wrangler/hd3_run.log` 含每条 SQL 与 UTC 时间戳）。

#### 15.10.1 执行环境（已核对）

| 项 | 值 |
|---|---|
| 目标库 | `guofeeder`（`database_id = 0d8385ba-a1a6-4fcc-8e43-ae61c014c43d`，绑定 `guofeeder_DB`，`wrangler.toml:9-15`） |
| 账户 | `wyl94@foxmail.com` · Account ID `9e72d201efec6925b3a468a0e3fb4928`（`wrangler whoami` 实核） |
| 执行位置 | `Cloudflare_Assets/guo-feeder-api` · `--remote` · `--json` · 单语句 · 无 `--file` |
| 服务端 | `served_by = v3-prod` · `served_by_region = APAC` · `served_by_colo = SIN` · `served_by_primary = true` |
| 代理 | 执行前 `unset http_proxy https_proxy HTTP_PROXY HTTPS_PROXY ALL_PROXY all_proxy` |

#### 15.10.2 六条语句（顺序 = 授权给定）

| # | ID | 结果 | `rows_read` | `rows_written` | `changed_db` | `sql_duration_ms` |
|---|---|---|---|---|---|---|
| 1 | `E-0` | `SCAN sqlite_master` + `USE TEMP B-TREE FOR ORDER BY` | 0 | 0 | false | 0.524 |
| 2 | `E-1` | `USE TEMP B-TREE FOR count(DISTINCT)` + `SCAN log_history`（仅 1 个 TEMP B-TREE） | 0 | 0 | false | 0.309 |
| 3 | `E-2` | **`SCAN log_history USING COVERING INDEX ux_lh_dedup`**（无 TEMP B-TREE） | 0 | 0 | false | 0.173 |
| 4 | `Q-0` | 1 表 + 2 索引（见 15.10.3） | 31 | 0 | false | 0.213 |
| 5 | `Q-1` | 单行 13 指标（见 15.10.3） | 180 | 0 | false | 1.055 |
| 6 | `Q-2a` | 8 个分组（见 15.10.3） | 180 | 0 | false | 0.309 |

> **零写入三重自证**：6/6 条 `rows_written = 0`、`changed_db = false`、`size_after` 恒为 `1306624`（6 条完全一致）。
> **计划门 G0 通过**：三条 `EXPLAIN` 均在**读取任何业务行之前**完成，且与预期一致（`E-2` 恰为覆盖索引）。

#### 15.10.3 观察到的事实

**（A）`Q-0` · 生产 schema 与仓库 `0003` 逐字一致 —— 这是本轮最确定的一条**

| 对象 | 生产实际 DDL（原文） | 与 `0003` 比对 |
|---|---|---|
| `log_history` 表 | `id INTEGER PRIMARY KEY AUTOINCREMENT` · `device_id TEXT NOT NULL` · `ts INTEGER NOT NULL` · `level TEXT NOT NULL` · `code INTEGER` · `boot_seq INTEGER NOT NULL` · `seq INTEGER NOT NULL` · `message TEXT` · `payload TEXT` · `created_at INTEGER NOT NULL DEFAULT (unixepoch())` | ✅ **10 列全一致**（`code/message/payload` 可空亦一致） |
| `ix_lh_device_ts` | `CREATE INDEX ix_lh_device_ts ON log_history(device_id, ts DESC)` | ✅ `0003:122` |
| `ux_lh_dedup` | `CREATE UNIQUE INDEX ux_lh_dedup ON log_history(device_id, boot_seq, seq)` | ✅ `0003:126` |

⇒ **§3.2.1 通过**（键 = `(device_id, boot_seq, seq)`）；**§3.2.2 通过**（`UNIQUE` 索引**仅此 1 个**，无第二个唯一性来源）；
**§3.2.3 通过**（四类忽略来源中：① 唯一键唯一 ② 四列均 `NOT NULL` ③ **无 `CHECK`** ④ `AUTOINCREMENT` 成立）。
**额外非 `UNIQUE` 索引 = 0**（索引总数恰为 2，无运维临时索引）。

**（B）`Q-1` · 全表聚合（`rows_read = 180`，与 `rows` 相等 ⇒ 完整扫描已发生）**

| 指标 | 值 |
|---|---|
| `rows` | **180**（≪ 100 万门槛） |
| `devices` | **1** |
| `ca_min` / `ca_max` | `1792085491` → `1792327742` = **`2026-10-15T17:31:31Z` → `2026-10-18T12:49:02Z`**（跨度 **2.804 d**，折 **64.2 行/天**） |
| `ts_zero` / `ts_neg` / `ts_pos` | **0** / **0** / **180** |
| `boot_nonpos` / `seq_nonpos` | **0** / **0** |
| `boot_min` / `boot_max` | **82** / **104** |
| `seq_min` / `seq_max` | **105298** / **110903**（跨度 5606） |

**（C）`Q-2a` · 8 个 `(dev_tag, boot_seq)` 分组**（`dev_tag = ` `288485..e4`，**唯一一个**，与 `devices = 1` 交叉一致 ⇒ 未观察到掩码碰撞）

| `boot_seq` | `n` | `seq_min`..`seq_max` | 区间跨度 | 区间内缺口 |
|---|---|---|---|---|
| 82 | 1 | 105298..105298 | 1 | 0 |
| 84 | 1 | 105796..105796 | 1 | 0 |
| 91 | 1 | 107535..107535 | 1 | 0 |
| 93 | 7 | 108066..108075 | 10 | 3 |
| 101 | 9 | 110088..110101 | 14 | 5 |
| 102 | 93 | 110342..110434 | 93 | 0 |
| 103 | 19 | 110601..110619 | 19 | 0 |
| 104 | 49 | 110855..110903 | 49 | 0 |

- **`boot_seq` 无任何重复**：8 个值互不相同（**未观察到**"同一 `boot_seq` 分处两个时段"的重放形态）。
- **`boot_seq` 有 15 个缺号**：`83, 85–90, 92, 94–100`。
- **`seq` 消耗/入库比 ≈ 31.1×**（跨度 5606 / 180 行）：大量 `seq` 被消耗但未形成行。
- `boot=102/103/104` 的区间内**零缺口**（`n` 与跨度相等）⇒ 这三个 boot 的 `seq` 是**逐号连续入库**的。

#### 15.10.4 只能作为线索（🟡 不得升级为结论）

| 现象 | 可以写 | 不能写 |
|---|---|---|
| `boot_seq` **无重复**（8/8 互异） | "在 **`2026-10-15T17:31Z`–`10-18T12:49Z`、1 台设备、180 行** 的窗口内，**未观察到**序号重放形态" | ❌ `NOT REPRODUCIBLE` · ❌ "不存在风险" · ❌ "历史未丢失" |
| `boot_seq` 缺 15 个、`n` 从 1 到 93 剧烈不均 | "各 boot 的日志产量差异极大，缺号 boot 可能**未产生可入库日志**或**未上传**" | ❌ "缺号 = 该 boot 的日志被吞"（缺号同样可能只是没有 WARN+/ERROR 事件） |
| `seq` 跨度是行数的 **31×** | "存在大量**未被入库**的序号消耗（来源未确认）" | ❌ "31× 差额 = 丢失量"（INFO/DEBUG 消耗与上传策略、`filtered` 计数均可能造成） |
| `ts_zero = 0` | "**当前表内**无 `ts=0` 行" | ❌ "时间同步问题不存在"（`RET-1` 已证 10-18 03:00Z 删掉 26 条 `ts=0`）· ❌ "与碰撞无关可忽略" |
| `boot_min = 82`（非 1） | "**未观察到** `boot_seq` 从 1 重放" | ❌ "`boot_seq` 永不重放"（`R-1`/`R-2` 代码路径仍在） |

#### 15.10.5 仍无法回答（❌ 结构不变）

- 同一 `(device_id, boot_seq, seq)` 是否**收到过多条输入** —— 输入流不落库（§15.3 环节 ①）。
- 那些输入的内容是否**不同** —— 被 `OR IGNORE` 丢弃的行零留存（环节 ②）。
- 是否**确有一条不同内容的日志**未写入 —— 同上。
- 能否区分**合法重试**与**真实碰撞** —— `duplicate` 单一计数（环节 ③）。
- **`Q-0` 虽然证明了"`changes=0` ⟺ 键已存在"的唯一归因**，但"键已存在"**仍不等于**"内容相同"（§15.2 归因上限）。

#### 15.10.6 与查询前判断的对比

| 查询前判断 | 实测 | 一致性 |
|---|---|---|
| 生产 schema 可能与仓库不一致（`U-1` 未知） | **逐字一致**（表 + 2 索引） | **一致（不确定性已消除）** |
| 表规模 10² 行（据 `RET-1` 的 168 行外推） | **180 行** | ✅ 一致 |
| 设备数可能达 11 台（`device` 表规模） | **仅 1 台** | **不一致** ⇒ 其余 10 台无 `log_history` 数据，**不得**把本结论外推到它们 |
| `ts=0` 可能仍有存量 | **0 行** | 与 `RET-1`「26 条已在 03:00Z 被删」一致 |
| `boot_seq` 可能从 1 反复重放（`G-4` 原假设） | **最小 82，无重复** | **不一致** ⇒ 该假设在本窗口**未获支持**（但代码路径未消失） |
| `E-2` 可能回表（走 `ix_lh_device_ts`） | **覆盖索引，无 TEMP B-TREE** | ✅ 优于预期 |

#### 15.10.7 本轮结论（**定级不变**）

| 项 | 结论 |
|---|---|
| `log` / `telemetry` 域风险 | **`CODE-PROVEN RISK`**（**未变**）—— 代码路径 `R-1`/`R-2` 仍在（§3.2），本次只读**不可能**改变它 |
| "是否曾发生真实内容碰撞/丢失" | **`UNRESOLVED`**（**未变**）—— 本轮查询结构上不能回答（15.10.5） |
| 本轮**新增**可确定的事实 | ① 生产 schema = 仓库 `0003`（逐字）② 仅 1 个 `UNIQUE` 索引 ③ 无 `CHECK` ④ 6/6 语句零写入 ⑤ 当前窗口内 1 台设备、180 行、**无 `boot_seq` 重复** |
| 本轮**不能**推出 | `CONFIRMED` / `NOT REPRODUCIBLE` / `PASS` / `CLOSED` / "无风险"（§15.6 禁令全部有效） |
| `G-4` 状态 | **仍 `BLOCKED`** —— 本次给出的是**该窗口内的阴性观测**，不构成 `G-4` 的解除条件（解除定义见 `RET-1` 报告 §9.5） |
