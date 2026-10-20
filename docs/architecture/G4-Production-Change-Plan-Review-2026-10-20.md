# G4 生产变更计划 · 审阅修订报告（A–G）

- 日期：`2026-10-21`
- 性质：**审阅与修订**。不含任何生产变更；未应用 migration、未部署 Worker、未改 EMQX、未烧录、未改设备配置
- 被审对象：`G4-Production-Change-Plan-2026-10-20.md`（Rev.1）
- 修订产物：`G4-Production-Change-Plan-Rev2-2026-10-20.md`（Rev.2，执行手册；全部最终 SQL 只在其中出现一份）
- 本轮采集的证据：`docs/evidence/G4-ChangePlan-Rev2-2026-10-21/`

> ★ 本报告的写法约定：**"报告说" = 既有文档的断言，"实测/读码" = 本轮核实结果**。
> 凡二者不一致，一律以实测/读码为准，并在 A.3 显式列差异。

---

## A. 基线核实报告

### A.1 仓库与同步状态（实测，`2026-10-21 00:0x`）

| 仓 | 分支 | HEAD | `origin/main` | 工作区 | 判定 |
|---|---|---|---|---|---|
| 固件 `PIO_Assets/Guo_Feeder_Project` | `wb` | `9f6f7e7` | `9f6f7e7` | 干净 | ✅ 同步 |
| 云端 `Cloudflare_Assets` | `main` | `e1e8b79` | `e1e8b79` | 干净 | ✅ 同步 |
| 消息 `EMQX_Assets` | `main` | `ec2364f` | — | 干净 | 本轮**未动** |

★ 固件仓的远端分支名是 `main`，本地工作分支叫 `wb`，推送方式是 `git push origin wb:main`
⇒ **判同步只看 `origin/main`**（`origin/wb` 是历史遗留，严重滞后，用它判进度会得出错误结论）。

### A.2 被审文件的实际版本

| 文件 | 行数 | 结论 |
|---|---|---|
| `docs/architecture/G4-Production-Change-Plan-2026-10-20.md` | 341 | Rev.1，与审阅意见描述的版本一致（§5.2 的 Q-1…Q-5、§4.3 的 P-1…P-5 均在） |
| `docs/architecture/G4-Log-Credibility-and-Observability-Report-2026-10-20.md` | 432 | 与审阅意见一致 |
| `docs/architecture/G4-R4-R5-Joint-Investigation-Report.md` | 488 | 一致 |
| `docs/architecture/G4-R5-B1-B2-Implementation-and-Local-Acceptance-Report.md` | 297 | 一致 |
| `migrations/README.md` | 396 | `0007` 状态 = **NOT APPLIED**，与事实一致 |
| `migrations/0007_mqtt_messages_dedup.sql` | 75 | 两句 DDL + 不变式注释，与 Rev.1 §3 描述一致 |
| `src/mqtt-dedup.js` | 300 | 与报告一致（`msg_id` 缺失 ⇒ 返回 `null`） |
| `src/device-registry.js` / `src/history-ingest.js` / `src/index.js` | — | 与报告一致（B-1b 信任闸门、`direction` 补 `log`） |

### A.3 ★ 报告与代码/配置的差异（**逐条列明**）

| # | 报告的说法 | 实测 / 读码结果 | 影响 |
|---|---|---|---|
| **A-1** | 可信度报告 §B.5.1：「`LOG_BOOT_WATERMARK` **仍是默认关闭的开关**」 | ❌ **错**。`guo-feeder-api/wrangler.toml:40` 有 `[vars] LOG_BOOT_WATERMARK = "1"`，由 `362ddfd`（`2026-10-19`，"应用 0004/0005 到生产 D1、部署 Worker 并启用水位"）引入 ⇒ **生产已启用**（代码默认值才是关闭） | ①"不得擅自开启"这一条实际是"**不得改动**"；②`log_conflict` / `log_boot_watermark` **可以**作为 C-3 的现成监测源；③问题册 `LOG-BOOT-WM` 条目需更正 |
| **A-2** | Rev.1 §4.1：「回滚用 **deployment id**」 | ❌ **错**。`wrangler 4.145.0` 的 `rollback [version-id]`，位置参数是 **version id** | 按 Rev.1 执行会拿到 `version-id` 位置却传 deployment id ⇒ 回滚失败或回退到错误版本 |
| **A-3** | 可信度报告 §B.2 表头未区分 `timestamp` 语义 | ⚠️ `mqtt_messages.timestamp` 存的是设备 `t` 字段，而 `t` **被复用**：普通上行是 `time_get()`（**秒**）；分片 BEGIN 帧是**消息类型**；`log_ack` 的 `p.t` 是 `seq_to` | `Q-3` 直接相减会混入伪值 ⇒ 必须加时间区间过滤 |
| **A-4** | Rev.1 §5.2 `Q-4` 用 `created_at/300` 分桶但未写单位 | ✅ 对 `log_history` **成立**（`created_at DEFAULT (unixepoch())` = 秒）；但 `mqtt_messages.created_at` 是 **毫秒** | 同一表达式跨表使用会差 1000 倍 |
| **A-5** | Rev.1 §5.3 用 `② cloud_ack_ok ≈ ③ accepted` | ❌ **量纲错**：`cloud_ack_ok` 在 `cloud_handle_ack()` 里**每接受一个 ACK 帧 +1**，是**批次**数；`accepted` 是**行**数 | 该等式永远不成立 ⇒ 会产生假告警 |
| **A-6** | Rev.1 §5.3 用 `④ == ③.accepted 累计（本 boot）` | ⚠️ `③` 在**我方不可观测**（见 A-7）；且 `seq` **不是** boot 内从 1 开始（见 A-8） | 等式两端都不可得 ⇒ 删 |
| **A-7** | Rev.1 §5.3 说 ③ 取自「`/api/history/ingest` 响应 `{accepted,duplicate,rejected,filtered}`」 | ❌ 响应确实有这四个字段，但 `history-ingest.js` **不打印**它们（无 `console.log`）；响应只回给 EMQX Action | 我方拿不到 ③ ⇒ 只能用 D1 增量替代 `accepted`，`duplicate/rejected/filtered` 标 `NOT MEASURABLE` |
| **A-8** | `docs/interfaces/log_envelope_contract.md:58`：「`seq` = **boot 内**日志序号」 | ❌ 与实现不符。`seq` 由 `s_meta_slot_seq + 1` **持久化推进**（`log_manager.cpp:1975`），跨 boot 连续（真机样例 `boot_seq=94, seq=108289`） | 不得假设"新 boot 从 1 开始"；`④` 只能按 `boot_seq` **分组**统计，不能按"从 1 累加" |
| **A-9** | 未提及 | ⚠️ `seq` 在 `log_emit_internal()` 里对**所有等级**分配，而 **DEBUG 不上云** ⇒ 云端 `seq` 空间**天然有洞** | **禁止**使用"`seq` 连续"作完整性判据 |
| **A-10** | Rev.1 §2-2：「建议等设备侧新固件先上板」 | ⚠️ **已过时**。固件 `sha256=94ba711c…`（commit `313e82e`）已于上一轮烧录并在跑（`boot_seq=25`） | 前置条件改为"执行前**实时复核**"，不得引用历史状态 |

★ 未发现差异的部分（明确记录，避免"只报坏消息"）：`0007` 的 DDL 与文档逐字一致；
`migrations/README.md` 的 `NOT APPLIED` 与事实一致；B-2b 的"缺 `msg_id` 不产生键"与代码一致；
`log_history` 的 `INSERT OR IGNORE` + `ux_lh_dedup` 与文档一致。

---

## B. 变更计划修订报告

### B.1 修改的文件

| 文件 | 动作 | 说明 |
|---|---|---|
| `docs/architecture/G4-Production-Change-Plan-Rev2-2026-10-20.md` | **新增** | 修订版执行手册（取代 Rev.1 的执行效力） |
| `docs/architecture/G4-Production-Change-Plan-2026-10-20.md` | **加顶部横幅** | 标注"已被 Rev.2 取代"，**原文一字未删**（审计留痕） |
| `docs/architecture/G4-Log-Credibility-and-Observability-Report-2026-10-20.md` | **追加勘误节** | 不改原文，新增 `§B.6 勘误`，逐条指向本报告 A.3 |
| `docs/issues/未修复的问题.md` | 追加 + 更正 | 新增本轮 8 项发现；更正 `LOG-BOOT-WM` 的"默认关闭"表述 |
| `HANDOFF.md` · `docs/README.md` | 更新索引 | 指向 Rev.2 与本报告 |

### B.2 每项修改的原因与验收语义变化

| 修改 | 原因 | 修改前语义 | 修改后语义 |
|---|---|---|---|
| 行数判据 → **id 界定指纹** | 生产每时每刻都在写入（`/api/ingest` 旧代码仍在跑） | 「行数变了就失败」⇒ **稳定误报** | 「`id <= MAX_ID_BEFORE` 的 COUNT + 长度和 + `keyed`」⇒ 对并发**免疫**，且能真正检出"历史行被删/被改" |
| 回滚 `deployment-id` → **`version-id`** | `wrangler 4.145.0` 语义 | 命令参数错位 | 与 CLI 语义一致，可执行 |
| 四段对账等式 → **不变量 + 近似 + 不可测** 三分 | 两个量不可观测、一个量纲错、一个位移错 | 四条"等式" ⇒ 无法执行也无法判 | 9 条**严格不变量**（违反即缺陷）+ 5 条近似（必须附解释）+ 4 条 `NOT MEASURABLE` |
| `Q-2` → `I-1` + `I-2` + `A-1` | 同 `msg_id` 的 audit/echo 天然两行；同 `msg_id` 不同 payload 也合法两行 | "重复率"会接近 100%，全是假阳性 | `I-1` = 索引完整性（必须 0）；`I-2` = 无降级（必须 0）；`A-1` = 明确标注"近似候选" |
| 探针一律合成 → **被动优先** | 合成探针会写 `mqtt_messages` 并刷新 `device.last_seen` | 5 个探针都改生产数据 | P-1'/P-3' **零写入**（用真实流量）；只有 P-2/P-4/P-5 需要合成，带 `G4PROBE-` 标记 + 全查询排除 |
| 新增 §4.5 观测可用性表 | `tail`/Logs 历史 403/502，但不能因此"假装测到了" | 隐含"Logs 可用" | 明确 `UNVERIFIED` + 只能用 D1 判 P-2/P-4/P-5 |
| 新增 §5.1 单位/语义表 | 毫秒/秒混用、`t` 字段复用 | 单位散落在正文 | 一处集中，SQL 前必读 |
| 前置条件改为实时复核 | 固件已上板，历史状态会过期 | 引用报告数字 | 9 项现场命令 + 记录 |
| `LOG_BOOT_WATERMARK` 表述 | 实际已启用 | "默认关闭" ⇒ 可能被"顺手打开" | "已启用，本轮**不得改动**" |

★ 验收语义的**总体收紧**：Rev.1 的判据是"能算出数就算成功"，Rev.2 的判据是
"**先证明这个数在语义上成立**，再算"。凡是无法成立的，一律降级为近似或标 `NOT MEASURABLE`。

---

## C. C-1 核实报告（migration `0007`）

### C.1 实际 SQL（`migrations/0007_mqtt_messages_dedup.sql`）

```sql
ALTER TABLE mqtt_messages ADD COLUMN dedup_key TEXT;                    -- :43
CREATE UNIQUE INDEX IF NOT EXISTS ux_mm_dedup_key                       -- :46-48
    ON mqtt_messages(dedup_key)
    WHERE dedup_key IS NOT NULL;
```

核实结论：**仅两列改动**（一列 + 一个**部分**唯一索引）；**零表重建**；不回填；不删改历史。

### C.2 写入侧是否与之匹配

`src/mqtt-dedup.js:240-243`：

```sql
... ON CONFLICT(dedup_key) WHERE dedup_key IS NOT NULL DO NOTHING
```

- 冲突目标 = `dedup_key`，与索引列一致 ✅
- 部分索引谓词 `IS NOT NULL` 与 `ON CONFLICT … WHERE` 谓词**逐字一致** ✅（这是 SQLite 的硬要求，不一致会直接报错）
- 用的是 `ON CONFLICT … DO NOTHING` 而**不是** `INSERT OR IGNORE` ⇒ 只有该唯一冲突被吞，其它错误（如 `NOT NULL`）照常抛 ✅
- `changes` 判定：`1 → INSERTED`、`0 → DUPLICATE`、其它/不可得 ⇒ 留痕且**不猜测** ✅

### C.3 本地实测（本轮新增，隔离 persist，**未接触远端**）

| 实验 | 命令 | 结果 |
|---|---|---|
| **E-1** 正式流程在**缺表**的库上 | `d1 migrations apply guofeeder --local` | `0001`–`0006` ✅；`0007` ❌ `no such table: mqtt_messages`；`d1_migrations` 中**只有 0001–0006** |
| **E-2** 正式流程在**有表**的库上（fixture 同构 + 预置 2 行完全重复） | 同上 | `0007` ✅ 已登记；`col=1 / idx=1 / rows=2 / nullkeys=2` ⇒ **历史重复行保留、未回填** |
| **E-3** migration **原子性**（第 3 条语句故意失败） | 临时工程，`0001` = `CREATE TABLE` + `ALTER ADD COLUMN` + `SELECT * FROM 不存在` | 失败后 `tbl=0 / col_b=0 / reg=0` ⇒ **整文件回滚，零残留、零登记** |

★ E-1/E-2 的结论与 `migrations/README.md` 的说明一致：
`mqtt_messages` 没有任何 migration 定义它（P0 时代的既有生产表）⇒ 本地必须先用 fixture 建表。
**远端不存在此问题**（表在生产库里已存在且有数据），但**必须**在执行前用 `A-1`
把生产 `sqlite_master.sql` 与 fixture 逐列比对（Rev.2 §2.5）。

★ E-3 的意义：**不存在"列建了但索引没建"的半成品状态**，也不存在"执行了但没登记"。
⇒ 恢复路径只有一个方向：修好原因再 `apply`；**禁止**手工补 SQL。

### C.4 并发写入的影响（Rev.1 的判据错在哪）

Rev.1 §3.3 的「行数未变 = 成功」在生产上**必然误报**：

- `/api/ingest` 旧代码仍在持续写入 `mqtt_messages`（每条上行一行）
- C-1 只是 DDL，不改变写入速率 ⇒ 应用期间"行数变化"是**正常**的
- 反过来，真正的风险（历史行被删/被改）恰恰**不会**表现为"总行数不变"

→ Rev.2 改用 **id 界定的指纹**：`WHERE id <= MAX_ID_BEFORE` 求
`COUNT(*)` + `SUM(LENGTH(raw_payload))` + `SUM(LENGTH(msg_id))` + `SUM(LENGTH(topic))`
再比对 `keyed = COUNT(dedup_key IS NOT NULL)`（必须 0）。
该集合**不随新插入变化**，且四个分量合起来足以检出"删行 / 改内容 / 回填键"。

### C.5 验证方法是否独立

Rev.2 §3.3 的 `V-1` 用**一个查询四个独立子查询**分别验证：
列存在 / 索引存在 / 索引是**部分**索引（`sql LIKE '%WHERE dedup_key IS NOT NULL%'`）/ 登记存在。
四项分开判，避免"索引存在但条件不匹配"被 `idx=1` 掩盖。

★ 这四项**全部**在本地 E-2 中实测为 1（`test/real_d1/mqtt_dedup_real_d1.sh` 的 `R-1a/R-1b/R-1c` 同源）。

### C.6 回滚边界（是否已核实）

| 项 | 核实结果 |
|---|---|
| `DROP INDEX` 恢复"无去重"写入 | ✅ 真 D1 本地引擎 `R-8a/R-8b`：删索引后同键可再插 2 行 |
| 列保留 | ✅ `R-8c`：删索引后列仍在 |
| 能否复活已被拦截的记录 | ❌ **不能**。`DROP INDEX` 只影响未来写入 |
| 是否尝试 `DROP COLUMN` | ❌ **不尝试**（D1 上不保证可用，且风险/收益不成比例） |

### C.7 C-1 结论

- migration SQL、索引定义、写入侧冲突目标、回滚命令**全部核实通过**
- 并发写入的判定方法**已修正**为 id 界定指纹
- 失败分类**已建立**（结构错误 / 登记错误 / 并发噪声），且**有实验依据**（E-3）
- 唯一未验证项：**生产 D1 的 migration 原子性**（本地引擎实测原子；生产按官方语义应一致，
  但本阶段**未在生产验证**）⇒ Rev.2 把 `V-1` 定为 C-3 每 10 min 的复检项

---

## D. C-2 核实报告（Worker 部署与行为验证）

### D.1 部署与回滚目标的正确记录方式

```bash
npx wrangler deployments list    # 最近 10 次（deployment id + 对应 version id）
npx wrangler versions list       # 最近 10 个 version
npx wrangler deployments status  # 当前生效组合
npx wrangler rollback <version-id>   # ★ 位置参数是 version id
```

- `wrangler 4.145.0` 的 `rollback --help` 明确写：`wrangler rollback [version-id]` —
  `POSITIONALS: version-id — The ID of the Worker Version to rollback to`
- **两个 ID 都必须记**，但**回滚只传 version id**。Rev.1 的写法会失败
- 本阶段**未**执行 `deployments list`（属生产只读 API 调用，不在本阶段授权范围）
  ⇒ 当前 `CURRENT_VERSION_ID` / `PREV_VERSION_ID` 状态：**UNVERIFIED（待执行前采集）**
  （`HANDOFF.md` 记的 `826332ed` / `c666aeeb` / `f9647a00` 是上一轮的值，**不得**直接当现值）

### D.2 探针的实际实现与副作用（**逐条读码核实**）

| 事实 | 位置 | 对探针的影响 |
|---|---|---|
| `/api/ingest` 必须带 `X-Webhook-Key`（`env.WEBHOOK_SECRET`），否则 401 | `index.js:284-295` · `http-util.js:46-52` | 探针需要生产密钥 ⇒ 属**生产写入**，须授权，且**不得**写入日志/仓库 |
| 成功响应是纯文本 `OK`（**无计数**） | `index.js:746-751` | 探针**无法**从响应判断写入结果 ⇒ 必须查 D1 |
| `/api/ingest` 拒绝 `source="cloud_api"`（400） | `index.js:388-396` | 探针**不可能**触发 MQTT 下发 / 设备动作 ✅ |
| `source != cloud_api` ⇒ `handleRegistrySideEffects()` | `index.js:681-691` | ★**副作用**：对 `up|log` + 合法 12-hex 设备段会 `upsertDeviceOnIngest()` |
| `upsertDeviceOnIngest()`：`INSERT … CLAIM_PENDING` / `UPDATE last_seen = MAX(...)` | `device-registry.js:333-360` | ★**副作用**：用**合成**设备 ID ⇒ `device` 表**新增一行**；用**真实**设备 ID ⇒ `last_seen` 前移（单调 `MAX`，不会回退） |
| 建档闸门是 `deviceIdFromTopic(topic,{directions:['up','log']})` | `index.js:1238-1242` | `guo_feeder/BADID/up` 的设备段非法 ⇒ **不建档** ✅（P-2 无 device 副作用） |
| 退役守卫：`REVOKED` 设备直接 `{ignored:'device_revoked'}` 200 | `index.js:491-509` | 探针打到退役设备会被静默忽略 ⇒ 探针**必须**用当前活跃设备 |
| 去重结果只进 `console.log("[mqtt-dedup] outcome=…")` | `index.js:656-659` | 只有 Logs/tail 可见（**UNVERIFIED**） |

**P-1…P-5 逐项判定（Rev.1 → Rev.2）**

| 原探针 | Rev.1 设计 | 核实结果 | Rev.2 处理 |
|---|---|---|---|
| P-1 归属 | 构造 `$tenants/.../down` ingest | 真实流量**本来就有** `$tenants` 回流（规则 B，晚 33–37 s）；构造反而多写一行 | ❌ 改为**被动** P-1'（零写入） |
| P-2 哨兵 | 构造 `guo_feeder/BADID/up` | 合法：不建档；会写 1 行 `device_id='unresolved'` | ✅ 保留为合成探针 + `G4PROBE-` 标记 + 查询排除 |
| P-3 方向 | 构造 `guo_feeder/<dev>/log` | 设备**本来就在发** `/log`；构造会刷新 `last_seen` | ❌ 改为**被动** P-3'（零写入） |
| P-4 去重 | 同 `msg_id` 连发 3 次，看 Logs 的 `inserted/duplicate` | Logs 不可靠 ⇒ 不可判；且"按 `msg_id` 分组"与真实键不符（键含 `source/dir/cmd/payload`） | ✅ 改为 **D1 行数 = 1**（权威），Logs 只作补充 |
| P-5 无键 | 同 payload、无 `msg_id` 连发 2 次，期望 2 行 | 正确，但要保证 payload **逐字节相同**、且无 `msg_id` | ✅ 保留，判据明确为「2 行 + 两行 `dedup_key` 均 NULL」 |

★ **P-4 的"按 `msg_id` 分组"错在哪**：真实去重键是
`SHA256(source‖device‖direction‖cmd‖"id:"+msgId‖"ph:"+SHA256(raw_payload))`。
- 同一业务消息的 audit + echo ⇒ 同 `msg_id`、**不同** `source` ⇒ **两行，合法**
- 同 `msg_id` 但 payload 不同 ⇒ **不同键** ⇒ **两行，故意保留**（INV-K2）
⇒ 只按 `msg_id` 分组会把这两类合法行算成"重复"，得出的"重复率"接近 100%，全是假阳性。
Rev.2 的 P-4 因此改为「**同一条 payload 连发 3 次，D1 必须只有 1 行**」——
这是对键语义的**直接**验证，不依赖任何分组。

### D.3 观测通道可用性

| 通道 | 状态 | 结论 |
|---|---|---|
| D1 只读 SQL | ✅ **可用**（本阶段已实测远端直连） | P-2/P-4/P-5 的**主**判定通道 |
| `wrangler tail` | **UNVERIFIED**（历史 502；本轮未试） | 不得当作可用 |
| Workers Logs 查询 | **UNVERIFIED**（配置已 `persist=true`，但历史 API 403） | 不得当作可用 |
| EMQX 运行指标 | ❌ 403（`/clients*` 只读可用） | B-3 只能靠控制台人工截图 |

⇒ **纪律**：凡依赖 Logs/tail 才能得出的结论（如"`duplicate` 计数"），
在证明通道可用之前一律标 `NOT RUN`，**不得**用"表里只有 1 行"倒推"去重率已测"。

### D.4 仍无法验证的项目

| 项 | 状态 | 原因 |
|---|---|---|
| 当前线上 version/deployment id | **UNVERIFIED** | 本阶段未调生产只读 API |
| 新版本在**全部** isolate 上生效 | **无法证明** | Workers 无版本化实例清单；只能靠行为探针 + 时间窗 |
| `outcome=duplicate` 计数 | **NOT RUN** | Logs/tail 未验证 |
| 探针是否被 EMQX/Pages 中转层改写 | **NOT RUN** | 探针直连 Worker，绕过 Pages；若直连被拦则须改走 Pages（需另验证） |
| 回滚后旧 isolate 的行为 | **无法证明** | 只能靠版本登记 + 行为探针 |

---

## E. C-3 指标与 SQL 审查报告

> 最终 SQL 全文见 Rev.2 §5.2（**单一副本**，避免两处漂移）。本节只做逐项审查。

### E.1 逐项审查表

| 原 SQL | 字段 / 单位（实测） | 原问题 | 修正 | 结论 |
|---|---|---|---|---|
| **Q-1 归属** | `device_id TEXT NOT NULL`；`created_at` **ms** | 未写 `:ts0` 单位；未排除探针行；未按 `direction` 分组 | 加 `>= :ts0_ms`、加探针排除、加 `direction` 维度 | ✅ 修正后可用 |
| **Q-2 重复** | `msg_id TEXT`；`dedup_key TEXT` | ❌ **语义错**：同 `msg_id` 的 audit/echo 天然两行；同 `msg_id` 不同 payload 也合法两行 | 拆成 `I-1`（索引完整性必须 0）+ `I-2`（有 `msg_id` 必须都有键）+ `A-1`（明确标"近似候选"） | ✅ 已重写 |
| **Q-3 延迟** | `created_at` **ms**；`timestamp` **秒**且可能 NULL、可能是**消息类型** | 单位对，但 `t` 被复用 ⇒ 伪值混入；未报样本数 | 加 `timestamp BETWEEN 1600000000 AND created_at/1000+300`；强制同时报 `n`；结论限"趋势" | ✅ 修正后可用（仍含设备时钟偏差） |
| **Q-4 分桶** | `log_history.created_at` **秒**（`DEFAULT (unixepoch())`） | 未写单位；未限定设备/boot | `/300` **成立**；改为 `created_at - created_at % 300` 更可读；加 `device_id`/`boot_seq` | ✅ 修正后可用；**禁止**用于 `mqtt_messages` |
| **Q-5 audit/echo** | `topic TEXT` | `LIKE '$tenants/%'` 可用但 `_`/`%` 是通配符（当前表名无 `_`，仍属脆弱） | 改 `substr(topic,1,9)='$tenants/'` | ✅ 修正后可用 |
| **新增 Q-6** | `log_conflict.*_at` 秒 / `log_boot_watermark.*_at` 秒 | Rev.1 完全没有用到这两张表（尽管水位已启用） | 新增 collision 新增数 + 水位一致性 | ✅ 新增 |
| **新增 Q-7** | — | Rev.1 没规定探针行如何处理（"不得删除"与"不得污染统计"冲突） | 只统计、不删除、全查询排除 | ✅ 新增 |

### E.2 严格不变量（**可判真假**，见 Rev.2 §5.3 的 I-1…I-9）

要点与依据：

| ID | 为什么是**严格**的 |
|---|---|
| I-1 `dedup_key` 重复组 = 0 | 部分唯一索引的结构性保证；非 0 只能意味着索引缺失/被删 |
| I-2 有 `msg_id` ⇒ 键非 NULL | `B-2b` 后键**只**由 `msg_id` 决定（payload 只作附加成分）；NULL 只能意味着降级态 |
| I-3 `rows == COUNT(DISTINCT seq)` | `ux_lh_dedup (device_id, boot_seq, seq)` |
| I-4 窗口内 `boot_seq` 不变 | `acked_seq` / `counters.*` 都是 RAM，重启归零 ⇒ 变则窗口内数据不可比 |
| I-5 `acked_seq` 单调不减 | 设备侧高水位只增（`if (new_acked > s_cloud_acked_seq)`） |
| I-6 `rows ≤ Δcloud_records_sent` | 云端不可能有设备**从未首发**的记录（`cloud_records_sent` 只在 `s_cloud_retry_n == 0` 时累加） |
| I-7 `Δack_ok ≤ Δbatch_sent` | ACK 批次 ⊆ 发送批次 |
| I-8 audit > 0 且 echo > 0 | `source` 进键 ⇒ 两类**结构上**不可合并 |
| I-9 `0007` 已登记且索引在 | C-1 的持续有效性 |

★ **I-6 的方向必须写对**：`rows ≤ sent`（不是 `=`）。
`Δsent − rows` 的差额包含"在途 + 被过滤 + 被拒收 + 被去重 + Flash 补发"，
这些分量**当前不可观测** ⇒ 差额只能作近似 `A-1`。

### E.3 近似指标（**必须附解释，不得当结论**）

| ID | 定义 | 为什么只能是近似 |
|---|---|---|
| A-1 | `Δcloud_records_sent − Δrows` | 见 I-6 说明 |
| A-2 | `dedup_key IS NULL` 占比 | 真实含义是"无 `msg_id` 的上行比例"（历史约 15%），**不是**重复率 |
| A-3 | 入库延迟分布 | 含设备时钟偏差；`timestamp` 缺失的行被排除 ⇒ 有选择偏差 |
| A-4 | 非法归属占比 | `unresolved` 是新增语义，历史行里没有基线 |
| A-5 | `offline_skip` | 是**轮数**不是条数（实测 11 s ⇒ 6 065），不得外推 |

### E.4 ★ Rev.1 四条对账等式为何全部作废

| 原式 | 作废原因 |
|---|---|
| `① ≈ ③.accepted + ③.duplicate` | ③ **不可观测**（`/api/history/ingest` 不打印计数，响应只回 EMQX） |
| `② ≈ ③.accepted` | ❌ **量纲错**：② 是**批次**数，③.accepted 是**行**数 |
| `④ == ③.accepted 的累计（本 boot）` | ③ 不可观测；且 `seq` **不**是 boot 内从 1 开始（跨 boot 连续）⇒ "本 boot 累计"无法用 seq 边界表达 |
| `Q-2 同 msg_id 重复率` | 语义错（audit/echo 天然两行；同 `msg_id` 不同 payload 也合法两行） |

★ 若要**真正闭合**对账，需要一次**新的代码改动**：在 `history-ingest.js` 里把
四字段计数打一行日志（`console.log("[history] dev=… accepted=… duplicate=… rejected=… filtered=…")`）。
这属于**生产代码变更**，**不在本次授权范围**，且必须在 Logs/tail 可用性得到证明之后才有意义。
⇒ 已登记为 Rev.2 之外的建议项 `LOG-COUNTERS`（见 F.1），**本轮不做**。

### E.5 仍需生产数据才能验证的指标

| 指标 | 需要什么 |
|---|---|
| 归属正确率（`Q-1`） | 部署后 ≥ 1 h 的真实流量（含 `$tenants` 回流） |
| 索引完整性（`I-1`） | 部署后真实写入若干行 |
| 无降级（`I-2`） | 同上 |
| 延迟分布（`Q-3`） | 设备时钟已同步的窗口 |
| 冲突/水位（`Q-6`） | 至少一个完整 boot 周期 |
| 真实重复率 | **当前不可得**（需 Logs + 代码改动） |
| 漏记分层（B-3） | EMQX 控制台只读统计 |

---

## F. 上线前置条件与遗留风险

### F.1 分级清单（本轮新增/更正的项）

| ID | 级别 | 项 | 是否阻止试运行 | 观察 / 关闭条件 |
|---|---|---|---|---|
| **`C1-REG`** | MAJOR（流程） | 生产 migration **原子性**未在生产验证（本地实测原子） | ❌ 不阻止 | 执行后 `V-1` 四子项 + 每 10 min 复检；异常即停 |
| **`C2-VER`** | **BLOCKER（对 C-2 而言）** | 当前线上 `version_id` / `deployment_id` **未采集**（本阶段属生产只读 API，未授权） | ✅ 阻止 C-2 执行 | 执行前 §2.7 采集并记录 |
| **`C2-OBS`** | MAJOR | Logs / `tail` 可用性 **UNVERIFIED** ⇒ 重复率、5xx 率测不了 | ❌ 不阻止 C-2 | 只读 SQL 判 P-2/P-4/P-5；其余标 `NOT RUN` |
| **`C2-SIDE`** | MAJOR | 合成探针会写 `mqtt_messages` 并刷新 `device.last_seen` | ❌ | 用真实设备 ID + `G4PROBE-` 标记 + 全查询排除；只在设备在线时做 |
| **`LOG-COUNTERS`** | MAJOR（建议） | `/api/history/ingest` 不打印四字段计数 ⇒ ③ 不可观测 | ❌ | 需**单独授权**的代码改动；本轮不做 |
| **`SEQ-SEM`** | MINOR（文档） | `log_envelope_contract.md:58` 说 `seq` 是"boot 内序号"，实现是**跨 boot 连续** | ❌ | 修文档（本轮已在本报告登记，未改契约文件） |
| **`TS-OVERLOAD`** | MINOR | `mqtt_messages.timestamp` 存的是复用字段 `t`（秒 / 消息类型 / seq_to） | ❌ | `Q-3` 用区间过滤；长期应新增独立列（需 migration，未授权） |
| **`WM-1`** | MINOR（更正） | 问题册 `LOG-BOOT-WM` 原写"默认关闭" ⇒ 实际**已启用**（`362ddfd`） | ❌ | 本轮**不改动**该开关；监测 `log_boot_watermark.rewind_count` |
| **`B-3`** | **BLOCKER（仅对"以 `mqtt_messages` 为唯一证据的验收"）** | 漏记，根因**未定** | ❌ 不阻止运行；✅ 阻止据此下结论 | EMQX 控制台只读统计（规则 matched / Action failed·retry / 连接器成功率 / 队列丢弃） |
| **`G4-R4-T`** | MAJOR（门禁） | `<70%` 仍是**未批准**的历史建议值 | ❌ | 人工裁决；代码里只有 `candidate:true` |

### F.2 进入 C-1 / C-2 / C-3 各自还缺什么

**C-1 缺**
1. 人工书面授权
2. §2.5 的生产 `sqlite_master.sql` 原文（须与 fixture 逐列一致）
3. `FINGERPRINT_BEFORE` + `MAX_ID_BEFORE` 已记录
4. `wrangler d1 migrations list --remote` 证明 `0007` 未应用

**C-2 缺**
1. C-1 已成功（`V-1 = 1/1/1/1`）
2. `CURRENT_VERSION_ID` / `PREV_VERSION_ID` 已采集（**当前缺失，`C2-VER`**）
3. 设备在线（否则 P-4/P-5 的 `last_seen` 副作用会污染离线判定）
4. 观测通道现状已记录（Logs/tail 是否可用）
5. 人工书面授权（**C-1 成功之后**）

**C-3 缺**
1. C-2 已成功且 P-1'–P-5 有结论
2. 窗口负责人 / 采样时间表 / 停止条件下令人
3. `boot_seq` 基线（窗口内必须不变）
4. 接受"重复率、5xx 率、B-3 分层"三项为 `NOT MEASURABLE`

### F.3 设备侧日志对账风险（重点）

| 风险 | 依据 | 缓解 |
|---|---|---|
| `boot_seq` 变化 ⇒ 计数器归零 | `acked_seq` / `counters.*` 全在 RAM | I-4：窗口内变则作废重来 |
| `seq` 空间有洞 | DEBUG 在 emit 时也占 seq，但不上云 | **禁止**用"seq 连续"判完整性 |
| `ACK` 只证明"键存在" | `H-3-7-a` 已批准的设计（`collision` 也 ACK） | 只能写"键已存在"，不能写"内容一致" |
| `boot_seq` 回退造成整批假重复 | 唯一键撞历史 ⇒ `INSERT OR IGNORE` 丢弃 ⇒ 存在即 ACK | 监测 `log_conflict.kind='collision'` + `log_boot_watermark.rewind_count` |
| `mqtt_messages` 自身不可信 | B-3 已证漏记；历史上有伪 `device_id` | 凡以它为证据必须用串口/设备计数交叉验证 |

---

## G. 独立人工审批清单

> 三阶段的**范围、检查、成功、失败、回滚、授权**六项全部见 Rev.2 §8。
> 本节省略重复内容，只列出**审批人必须逐条勾选**的要点，防止"打包授权"。

### G.1 C-1（migration）

- [ ] 范围确认：只加 1 列 + 1 个**部分**唯一索引；不回填、不删改、不手执行 SQL
- [ ] 执行前：§2.1–§2.6 全绿；`A-1` 与 fixture 逐列一致；`0007` 未登记
- [ ] 成功判据：`V-1 = 1/1/1/1`；`V-2` 指纹与 `FINGERPRINT_BEFORE` 逐项相等且 `keyed=0`
- [ ] 失败判据：`apply` 非 0 退出 / `V-1` 任一为 0 / `V-2` 指纹变化
- [ ] 回滚：`DROP INDEX IF EXISTS ux_mm_dedup_key;`（列保留；不手工补 SQL）
- [ ] **本阶段独立授权**（不与 C-2 合并）

### G.2 C-2（Worker 部署）

- [ ] 范围确认：只部署 `main` 当前代码；**不改** `wrangler.toml` 任何 `vars`（含 `LOG_BOOT_WATERMARK`）
- [ ] 执行前：C-1 成功；`CURRENT_VERSION_ID` / `PREV_VERSION_ID` 已记；观测通道现状已记
- [ ] 成功判据：P-1' 无 `http_api` · P-3' 有 `log` · P-2 落 `unresolved` · **P-4 = 1 行** · **P-5 = 2 行且键 NULL**
- [ ] 失败判据：Rev.2 §4.6 六条
- [ ] 回滚：`npx wrangler rollback <PREV_VERSION_ID> --message "…"`
- [ ] **在 C-1 成功之后单独授权**

### G.3 C-3（观察窗口）

- [ ] 范围确认：**只读**（SQL + 串口只读 + EMQX 控制台只读）；不写、不删、不改配置
- [ ] 执行前：窗口负责人 / 采样表 / 停止条件下令人已定；`boot_seq` 基线已采
- [ ] 成功判据：4 次采样 `I-1…I-9` 全成立；`A-1…A-5` 有解释；`N-1…N-4` 如实标 `NOT MEASURABLE`
- [ ] 失败判据：Rev.2 §5.5 停止条件
- [ ] 回滚：不适用（只读）；触发停止条件则转 C-2 回滚流程
- [ ] **单独授权**，且明确"本窗口 ≠ S-3-L3 的 2.5 h 门禁窗口"

### G.4 三条**不得跨越**的线（写进审批记录）

1. 完成 C-1/C-2/C-3 **不等于** `G-4 PASS`，**不解锁** `WP-5`，**不解除** `STOP-R1`
2. B-1/B-2 的修复**不代表** B-3 已解决；`mqtt_messages` 不得作唯一完整性证据
3. `G4-R4` 的 H-1…H-5 阈值**仍是候选**；`LOG_BOOT_WATERMARK` **已启用，本轮不得改动**
