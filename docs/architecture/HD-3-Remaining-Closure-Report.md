# `HD-3` 剩余事项闭环报告（A-1 / B-3 / H-3-7）

> **执行时间**：`2026-10-18 23:49 – 2026-10-19 00:25`（本地）
> **基线**：`HD-3-A1-B3-Closure-Report.md`（A-1 `CONDITIONAL` · B-3 `CONDITIONAL` · 整体 `CONDITIONAL`）
> **本轮授权**：`H-3-7-a/b/c` 三项决策 + A-1 继续闭环（不含真机 / 不含生产应用）
> **未执行**：生产 D1 查询写入 · migration 应用 · Worker/Pages 部署 · EMQX/Cron 变更 · 设备命令/烧录/OTA · Git 任何操作

---

## 1. 最终状态

| 项 | 状态 | 依据（可核验） |
|---|---|---|
| **A-1** | **`CONDITIONAL`** | 8 个代码缺陷全部闭环（含本轮新增的竞态复核与注入诊断）；场景矩阵 ①–⑥/⑧ 有本地可执行证据（wasm 真跑 63 项 + 跨重启状态机 13 项）；**矩阵 ⑦（整片擦除）仍未关闭**；`A1-03` 远程可观测仍为方案；**真机 8 项 `NOT RUN`** |
| **B-3** | **`PASS`（限 B-3 自身范围）** | `B3-01/02/03/05` 代码闭环 + `B3-04` 已由 `H-3-7-a` 裁决并落地为文档与实现注释 + `B3-06` 记为边界；L1 61 项、真实 D1 53 项全绿；响应体/`duplicate`/查询次数零变化 |
| **H-3-7** | **`PARTIAL`** | `a` = 已裁决并落地（保持 ACK + 契约澄清）；`b` = **本地实现 + 本地真实 D1 验证通过，但 migration `NOT APPLIED`、Worker `NOT DEPLOYED`**；`c` = 契约文档已更新 |
| **整体 HD-3** | **`CONDITIONAL`** | 两条原阻断（`A1-01`/`A1-02`）已闭环；剩余为"真机未验证 + 服务端检测未部署 + 整片擦除边界未关闭 + 远程可观测仅方案" |

**状态口径**：`PASS` = 代码 + 可在本地执行的验证全闭环且无未决代码缺陷；`CONDITIONAL` = 存在必须真机 / 必须人工裁决 / 必须额外授权的未验证项；`PARTIAL` = 部分子项完成且其余有明确未完成原因。

---

## 2. 缺陷闭环表

### 2.1 A-1（`A1-01` … `A1-09`）

| ID | 状态 | 修改文件 | 测试证据 | 剩余风险 |
|---|---|---|---|---|
| `A1-01` | **`CLOSED`**（模拟 + 代码审查） | `src/log/log_meta_evidence.h`(新) · `log_manager.cpp` · `log_manager.h` · `main.cpp` · `device_identity.{h,cpp}` | `E5b`（NVS 优先）· `E10f/g`（格式化后 `boot=3`，**不是 1**）· `E5g`（无证据 + 身份已存在 ⇒ 隔离） | 矩阵 ⑦ 整片擦除不可分辨 ⇒ **未关闭**（服务端兜底未部署） |
| `A1-02` | **`CLOSED`**（模拟） | `log_manager.cpp`(`quarantine_enter` / `seq_init`) · `log_meta_evidence.h` | `E10h/i`（⇒ 隔离）· `E10j/k`（**双槽+段全有效时隔离仍生效**） | 双标记均写失败时跨重启不保（已显式告警） |
| `A1-03` | **`PARTIAL`** | `log_manager.h`(+6 字段) · `log_manager.cpp` · `main.cpp`(`[LogT] meta` 第二行 + 开机 `src=`/`failclosed=`) | 编译通过；串口可读 | **无 MQTT/远程通道** ⇒ 方案 O-7（见 §5），**未实现** |
| `A1-04` | **`CLOSED`** | `log_meta_evidence.h`(`seg_last_usable` / `align_reserve`) · `log_manager.cpp`(`flash_seg_high_water`) | `E7f/E7g`（`0xFFFFFFFF` ⇒ 耗尽哨兵，**不回绕成 0**）· `E9a–d` | 无 |
| `A1-05` | **`CLOSED`** | `log_manager.cpp`(`meta_slot_write_raw` 回读逐字节校验 + `mstage 3` 注入) | 编译通过；注入钩子就位 | 真机未跑（`RD-6`） |
| `A1-06` | **`CLOSED`** | `log_manager.cpp`(`log_meta_wipe` 恢复"legacy+双槽"语义 / 新增 `_legacy`/`_slots` / `log_meta_test_fail_stage`) · `main.cpp`(`mstage`/`lqclear`/`levwipe`) | 钩子就位；`mwipe` 语义已在头文件注明 | `mcorrupt` 仍只作用于 legacy（已注明，属设计） |
| `A1-07` | **`CLOSED`** | `log_meta_evidence.h`(`next_boot_seq`) · `log_manager.cpp`(`seq_reserve_next` 溢出闸门) | `E8a–h`（饱和、不回绕、标记不可继续） | 无 |
| `A1-08` | **`CLOSED`** | `log_manager.cpp`(`meta_write` 改 临时文件 + rename) | 编译通过 | 镜像失败时旧固件读到上一代值（可接受，已置 `seq_reliable=false`） |
| `A1-09` | **`CLOSED`** | 本报告 §6 | 实测 `probe_meta_slot` **32** 项（原文档 41 为笔误）· `probe_meta_evidence` **63** 项 | 无 |
| **本轮新增** | `A1-10` | `log_manager.cpp`：`log_set_first_boot()` 未调用时 `seq_init()` 打 WARN | 代码审查 | 无（纯诊断） |

**本轮对 A-1 的增量**（未推倒重做，只补强）：

1. `first_boot` **注入审计**：新增 `s_first_boot_set`，未注入时开机 WARN（避免"新设备被误判隔离且无人发现"）。
2. 场景矩阵**写进代码**（`log_meta_evidence.h` 头部 8 行权威表）与专项文档（§3）。
3. 竞态/依赖复核 8 项（见 `HD-3-A1-Recovery-Scenarios-and-RealDevice-Matrix.md` §2），结论：无新增竞态、无循环依赖；`log_manager.cpp` 仍**不** include `services/*`（只接收注入 bool）。

### 2.2 B-3（`B3-01` … `B3-06`）

| ID | 状态 | 修改文件 | 测试证据 | 剩余风险 |
|---|---|---|---|---|
| `B3-01` | **`CLOSED`** | `src/log-ack.js` | L1 `B3/B4`（`exists_unavailable` 区分"键不存在"与"查询失败"）· 真实 D1 `R1b` | 计数只进日志 |
| `B3-02` | **`CLOSED`** | `src/log-ack.js` | L1 `D2`（摘要级别）+ `unverified>0 ⇒ console.error` | 无告警通道（仅日志） |
| `B3-03` | **`CLOSED`** | `test/current/log_ack_collision.test.mjs` | `C4` 改 `===`（`same+collision+unverified === duplicate`）· `C2` 空断言换成 summary 行断言 · C 组标题改正 | 无 |
| `B3-04` | **`CLOSED`（由 `H-3-7-a` 裁决）** | `src/log-ack.js` 注释 · `docs/interfaces/cloud_protocol.md` §8.2.5 | L1 `E2`（碰撞仍 ACK）· 真实 D1 `R2a` | 内容静默丢失仍存在（**语义已明确**，不再是"误报为已保存"） |
| `B3-05` | **`CLOSED`** | `src/log-ack.js`(`diffLogFields` 数值分支) · `src/log-conflict.js`(同逻辑单一来源) | L1 `A14–A18`（`""`/`" 12 "`/`true` ⇒ `differs`；`"12"` ⇒ `same`） | 无 |
| `B3-06` | **`DOCUMENTED`（不改）** | `src/log-ack.js` 注释 | — | base64 重编码仍会命中 `collision`（安全的假阳性） |

### 2.3 H-3-7

| 子项 | 状态 | 落地内容 | 未完成原因 |
|---|---|---|---|
| `H-3-7-a`（保持碰撞 ACK） | **`DONE`** | 未改任何行为；`log-ack.js` 就地注释 + 协议文档 §8.2.5 明确"ACK = 键存在 ≠ 内容已持久化" | — |
| `H-3-7-b`（`log_conflict`） | **`LOCAL DONE / NOT APPLIED`** | 草案 migration + `src/log-conflict.js` + 接线 + L1 50 项 + 真实 D1 54 项 | 生产 D1 **未建表**、Worker **未部署**（本轮禁止） |
| `H-3-7-c`（契约更新） | **`DONE`** | `docs/interfaces/cloud_protocol.md` 新增 **§8.2.5**（ACK 语义 / 三态 / 为何 ACK / 追踪方式 / 不变契约） | — |

---

## 3. 冲突追踪设计（`log_conflict`）

### 3.1 schema

```sql
CREATE TABLE log_conflict (
    id            INTEGER PRIMARY KEY AUTOINCREMENT,
    conflict_key  TEXT    NOT NULL,      -- 稳定冲突标识（16 hex 指纹）
    device_id     TEXT    NOT NULL,
    boot_seq      INTEGER NOT NULL,      -- 幂等键分量（与 ux_lh_dedup 同义）
    seq           INTEGER NOT NULL,      -- 幂等键分量
    kind          TEXT    NOT NULL CHECK (kind IN ('collision','unverified')),
    diff_fields   TEXT    NOT NULL DEFAULT '',   -- 差异字段名，逗号分隔、字典序
    existing_fp   TEXT,                  -- 既有行内容指纹（16 hex）
    incoming_fp   TEXT,                  -- 本次输入内容指纹（16 hex）
    first_seen_at INTEGER NOT NULL DEFAULT (unixepoch()),
    last_seen_at  INTEGER NOT NULL DEFAULT (unixepoch()),
    seen_count    INTEGER NOT NULL DEFAULT 1,
    status        TEXT    NOT NULL DEFAULT 'open' CHECK (status IN ('open','acked','resolved'))
);
CREATE UNIQUE INDEX ux_lc_key        ON log_conflict(conflict_key);
CREATE INDEX        ix_lc_device_seq ON log_conflict(device_id, boot_seq, seq);
```

**位置**：`migrations/drafts/0004_log_conflict.sql` —— `drafts/` 子目录 ⇒ `wrangler d1 migrations apply` **不会拾取**（已由夹具 `L-1a` 断言）。

### 3.2 幂等键与聚合

```
conflict_key = FNV1a-32×2( device_id | boot_seq | seq | kind | diff_fields | existing_fp | incoming_fp )
```

- **同一冲突反复重投** ⇒ 同 key ⇒ `ON CONFLICT DO UPDATE SET seen_count = seen_count + 1, last_seen_at = unixepoch()` ⇒ **不新增行**（真实 D1 `L-4`：3 次上报仍 1 行，`seen_count=3`）；
- **同键但差异字段不同** ⇒ 不同 key ⇒ **新行**（`L-5`：payload 冲突 + message 冲突 = 2 行）—— 语义上这是"另一次冲突"；
- 指纹是**非加密**哈希（目的只有聚合与比较，不用于安全），同步计算、跨 runtime 一致（`Math.imul`）。

### 3.3 最小证据（隐私边界）

| 存 | 不存 |
|---|---|
| 差异**字段名**（`payload` / `message` …） | payload / message **原文** |
| 两侧**指纹**（16 hex，不可逆） | 原始 MQTT 字节 |
| 定位三元组（`device_id/boot_seq/seq`）+ 计数 + 时间 | 任何凭据 / 用户数据 |

真实 D1 `L-3i` 断言：列内**不含** base64 明文。

### 3.4 失败处理

| 情形 | 行为 |
|---|---|
| 表未创建 / 迁移未应用 | `catch` ⇒ `conflict_failed=n` + 每 isolate **一次** `console.warn`；**HTTP 仍 200、ACK 仍发送、`duplicate` 不变**（真实 D1 `L-7` 用"改名表"实测） |
| DB 绑定缺失 | `skipped`，零调用 |
| 单条超上限 | 单请求最多 20 条，超出计 `skipped` |
| 高频重投 | 聚合进 `seen_count`（**因为碰撞也 ACK**，设备会停止重投 ⇒ 计数不会无限放大） |

### 3.5 容量影响

- 只在 `collision` / `unverified` 时写入（正常路径 **0 次**额外 D1 调用、0 行）；
- 冲突行极稀疏（每次设备序号复用事件 1 行），单行 ≈ 200 B；
- **无自动清理**：与 `log_history`/`telemetry_history` 不同，冲突属审计数据，建议保留并人工 `status` 收敛；若将来量大，可另立 retention 规则（**需单独授权**，不改现有 5 索引）。
- 与 `log_history` 的边界：**冲突永不写入 `log_history`**（真实 D1 `L-8` 断言行数与首行内容均未变）。

---

## 4. 服务端 `boot_seq` 回退检测方案

| 项 | 内容 |
|---|---|
| **要解决的问题** | 整片擦除（NVS + LittleFS 同时丢）后设备从 `boot_seq=1` 重放 ⇒ 复用云端历史幂等键（矩阵 ⑦） |
| **实现** | `src/log-bootseq-guard.js`：单条只读 `SELECT`（`GROUP BY device_id, boot_seq` + 时间窗 + `LIMIT 50`） |
| **判据** | 同一 `(device_id, boot_seq)` 的行跨度 **≥ 7 天** 且行数 ≥ 2 ⇒ **候选** |
| **输出** | `[log-bootseq] … candidates=… detail=[{dev,b,rows,span_days}] note=BOOT_SEQ_REUSE_CANDIDATE(not proof)` |
| **接入** | `index.js::scheduled()`，`env.LOG_BOOTSEQ_GUARD === "1"` 才运行 ⇒ **默认关闭**，未启用时零额外 D1 调用、cron 行为不变 |
| **协议影响** | **无**（不新增接口/字段/表、不写库、不改 cron 表达式） |
| **当前状态** | **仅本地实现**（L1 31 项全绿）；**未部署**；真实数据命中率 **`NOT RUN`** |
| **局限（必须承认）** | 设备长期不重启（> 7 天）同样命中 ⇒ 只是候选，不得自动处置、不得写成"已确认" |
| **精确化的两条路（未做，需授权）** | ① `device_log_boot_hw` 水位表（新表 + ingest 侧 O(1) 判定）；② 设备侧上报 `recovery=PROVISION`（协议字段） |

> **结论**：矩阵 ⑦ 的风险**保持未关闭**。本轮**不**把该方案标记为 PASS。

---

## 5. `A1-03` 远程可观测性方案（仅方案）

| 方案 | 需改协议 | 需改设备 | 实时性 | 结论 |
|---|---|---|---|---|
| **O-7 服务端"日志静默"检测**（cron 统计每设备 `MAX(created_at)`，超 N 天无新增 ⇒ 告警） | **否** | **否** | 天级 | **推荐（零协议变更）**；缺点：不能区分"隔离 / 离线 / 掉电" |
| O-3 新增 MQTT 健康 Topic | 是 | 是 | 秒级 | 信号最强，须单独协议授权 |
| O-6 扩展 `system_command` status 响应字段 | 是 | 是 | 请求级 | 侵入较小，仍需契约评审 |
| O-4 用 `log` envelope 携带告警 | 否 | 是 | — | ❌ 隔离期间禁止分配 seq ⇒ 关键场景恰好不可用 |

**推荐路径**：先做 O-7（纯服务端，可与 §4 的扫描共用同一次 cron）；若需区分**原因**，再走 O-3/O-6 的正式评审。两者本轮**均未实现**。

---

## 6. 测试报告

### 6.1 实际执行

| # | 测试 | 层级 | 结果 | 退出码 |
|---|---|---|---|---|
| 1 | `python test/log_contract/run_contract_test.py` | **纯逻辑 · wasm32 真执行** | **8/8 步 ALL PASS**；`probe_meta_slot` **32** · `probe_meta_evidence` **63** | `0` |
| 2 | `pio run -e esp32-s3-devkitc-1` | **编译** | `SUCCESS` 48.18 s · RAM 40.5%（132 744 B）· Flash 66.7%（1 398 769 B） | `0` |
| 3 | `node test/run_all.mjs` | **L1 全量（18 套件）** | **18/18 `exit=0`** | `0` |
| 4 | `node test/current/log_conflict.test.mjs` | **L1 逻辑替身** | **50 PASS / 0 FAIL** | `0` |
| 5 | `node test/current/log_bootseq_guard.test.mjs` | **L1 逻辑替身** | **31 PASS / 0 FAIL** | `0` |
| 6 | `node test/current/log_ack_collision.test.mjs` | **L1 逻辑替身** | **61 PASS / 0 FAIL** | `0` |
| 7 | `bash test/real_d1/log_conflict_real_d1.sh` | **真实 D1 引擎（本地 workerd）** | **54 PASS / 0 FAIL**（`log_history=2` · `log_conflict=2` · payload 冲突 `seen_count=3`） | `0` |
| 8 | `bash test/real_d1/b3_collision_real_d1.sh` | **真实 D1 引擎** | **53 PASS / 0 FAIL**（回归，验证冲突追踪未影响 B-3） | `0` |
| 9 | `bash test/real_d1/retention_real_d1.sh` | **真实 D1 引擎** | **105 PASS / 1 FAIL**（回归 `scheduled()` 入口；唯一的 FAIL = `W7-G2` 工作区洁净度守卫，见下） | `1` |

### 6.2 失败历史（保留，未删断言）

| 次 | 现象 | 根因 | 处置 |
|---|---|---|---|
| `log_conflict` L1 首轮 | **10 项 FAIL**（`no_db` / 零 upsert） | **实现缺陷**：`persistConflictEvents` 读的绑定名写成 `guo_feeder_DB`，真实绑定是 `guofeeder_DB` ⇒ 冲突永远静默降级 | 修实现（模块内注明"曾误写"），断言未改 |
| 同套件第二轮 | 1 项 FAIL（`upserts=2`） | **测试替身缺陷**：`upsert` 在 `bind()` 与 `run()` 各计一次 | 修替身（只计 `bind`） |
| `log_conflict` real_d1 首轮 | 全红（`400 Bad Request`） | **夹具缺陷**：`body()` 形参多了一个 `<desc>` ⇒ JSON 字段整体错位 | 修夹具参数表 |
| 同夹具第二轮 | `L-1a` FAIL | **夹具缺陷**：`rm -rf` 被沙箱批量删除保护拦下 ⇒ 复用旧 persist 目录（表已存在） | 改为**每次独立 run 目录**（不删），同时提升"重复执行可预测" |
| 同夹具第三轮 | 1 项 FAIL（`published=0`） | **断言过严**：本地夹具无 EMQX 凭据 ⇒ 发布必然 `CONFIG_MISSING` | 改断言为 `attempted=1` + `failed=1`（并注明属预期环境），**未删除**任何语义断言 |
| `run_all` 中途 | `log_bootseq_guard` 出现两次 | 注册脚本重复插入 | 去重（现为 18 套件） |

### 6.2b `retention_real_d1.sh` 的 1 项 FAIL（**环境条件，非回归**）

| 项 | 内容 |
|---|---|
| FAIL 断言 | `W7-G2 历史路径/禁用文件零改动` → 期望 `0`，实际 `2` |
| 命中清单 | ` M guo-feeder-api/src/history-ingest.js`（**前序轮次** B-3 注释改动）· `?? guo-feeder-api/migrations/drafts/`（**本轮** H-3-7-b 草案） |
| 为什么必然 FAIL | 该断言用 `git status --porcelain` 判定，而本工作区**故意**保留未提交成果 ⇒ 脚本自身已注明"这是**环境条件**，本脚本不得为通过而暂存/回滚他人改动" |
| 真正的越界判据 | `W7-G2a`（命中文件的**新增代码行**是否含 RET-1 令牌）⇒ **PASS = 0** |
| 本轮影响 | 从 1 项变为 2 项，增量 = 新增的 `migrations/drafts/`（**草案，未应用**）；不涉及 RET-1 语义，未改 `0001–0003` |
| 结论 | **不构成回归**；retention 的 104 项功能性断言全部 PASS |

### 6.3 未运行（明确保留）

| 项 | 状态 | 原因 |
|---|---|---|
| 真机 8 项（格式化 / 连续两次重启 / NVS 擦除 / 半写 / 回读异常 / 断电 / 整片擦除 / 正常重启） | **`NOT RUN`** | 无设备接入 + 属破坏性测试，需单独授权（矩阵见 §7） |
| `log_conflict` migration 生产应用 | **`NOT APPLIED`** | 本轮禁止应用 migration / 部署 |
| `log_bootseq_guard` 在生产数据上的命中率与查询代价 | **`NOT RUN`** | 需生产只读查询（禁止） |
| `A1-03` O-7 静默检测 | **`NOT IMPLEMENTED`** | 仅方案（§5） |
| 真实 D1 上的 `unverified` 分支（缺列形态） | **`NOT RUN`** | 真实 D1 必回全列 ⇒ 结构上不可在真实引擎复现；由 L1 `A10/A11/A12` 覆盖 |
| 多行批（`PERSIST_CHUNK > 1`）的 `meta.changes` 组合 | **`NOT RUN`** | 既有夹具均为单条；属可补项 |

---

## 7. 真机验证包（本轮只准备，不执行）

> 完整矩阵（前置条件 / 步骤 / 预期 / **禁止行为** / 数据恢复 / 失败处置 / 授权）见
> `docs/architecture/HD-3-A1-Recovery-Scenarios-and-RealDevice-Matrix.md` §6。摘要：

| 编号 | 场景 | 核心预期 | 需授权 |
|---|---|---|---|
| `RD-1` | 正常启动 ×3 | `recovery=NVS`；`boot_seq` 单调 +1 | 烧录 + 串口 |
| `RD-2` | LittleFS 格式化 | `recovery=NVS`、`boot=N+1`；**不得**出现 `PROVISION` | 破坏性 |
| `RD-3` | 连续两次重启（段损坏+双槽失效） | 两次都 `QUARANTINE`；第 2 次**不得**变 `PROVISION` | 破坏性 |
| `RD-4` | NVS 证据擦除（日志区健在） | `recovery=SLOT/LEGACY/SEGMENT`，不判全新 | 破坏性 |
| `RD-5` | 元数据半写（`mstage 2`） | 旧槽有效、`boot_seq` 不回退 | 破坏性 |
| `RD-6` | 写后回读异常（`mstage 3`） | `seq_reliable=0`，下次预留重试成功 | 破坏性 |
| `RD-7` | 断电恢复（≥20 次） | 无回退、无重复 `(boot,seq)` | 破坏性 + 断电 |
| `RD-8` | 整片擦除（矩阵 ⑦ 取证） | **预期会** `PROVISION`/`boot=1` ⇒ 用于**取证**，不是验收 | 破坏性 + 烧录 + 云端只读 |

**通用数据保护**：执行前导出云端该设备 `boot_seq` 分布/行数/时间范围作基线；全程录串口（`[Log] recovery=` / `[Log] seq:` / `[LogT] meta` 两行）；任一场景出现 `boot_seq` 回退或非 `RD-8` 的 `PROVISION` ⇒ **立即停测取证**。

---

## 8. 协议兼容性确认

| 契约 | 是否变化 | 证据 |
|---|---|---|
| HTTP 响应体（`accepted/duplicate/rejected/filtered`） | **未变** | L1 `C1/C2/C3/C5` + 真实 D1 每用例 `fourok` 断言"恰好四字段" |
| `duplicate` 口径 | **未变** | `duplicate` 仍是"未新增行 且 键存在"；`same/collision/unverified` 是其**子集细分** |
| 既有 ACK 行为 | **未变**（`H-3-7-a`） | 碰撞仍 ACK；L1 `E2`、真实 D1 `R2a` |
| 设备端重试语义 | **未变** | 本次未触碰 `log_manager.cpp` 的重试/退避/放弃门槛 |
| `log_history` schema / 唯一键 / 索引 | **未变** | 未新增/修改 migration（`0001–0003` 零改动） |
| 日志记录格式（128 B）/ 段头（16 B）/ meta 槽（32 B） | **未变** | `M1–M11` 契约探针继续全绿 |
| MQTT envelope / `log_ack` 帧字段 | **未变** | 仅文档 §8.2.5 澄清语义 |
| 查询次数（每未新增行 1 次） | **未变** | L1 `B7`（`existsQueries===3`）；冲突只在冲突时追加 1 次 `batch()` |
| 新增 D1 表 | **仅草案，未应用** | `migrations/drafts/`（`apply` 不拾取，`L-1a` 断言） |
| cron 行为 | **未变**（默认关闭） | `env.LOG_BOOTSEQ_GUARD !== "1"` ⇒ 零额外调用 |

---

## 9. 工作区保护确认

### 9.1 未执行的操作

| 禁止项 | 状态 |
|---|---|
| `git reset` / `clean` / `stash` / 切分支 / rebase / merge / cherry-pick / commit / push | **未执行**（只用 `git status` / `diff` / `rev-parse` 只读） |
| 生产 D1 查询 / 写入 | **未执行**（全部 `--local --persist-to`；两个夹具自检"远端标志命中 = 0"） |
| migration 应用（含本地草案到生产） | **未执行**（草案仅在一次性本地临时库应用） |
| Worker / Pages 部署 | **未执行** |
| EMQX / Cron / `wrangler.toml` | **未改动**（新开关走 `env` 变量，未写入配置文件） |
| 设备命令 / 重启 / 烧录 / OTA / 破坏性测试 | **未执行**（仅本地 `pio run` 编译） |
| 门禁（`BT-1 OPEN` / `G-4 BLOCKED` / `WP-5` / `STOP-R1`） | **未触碰** |
| RET-1 | **未触碰** |
| 其他任务遗留差异 | **未触碰**（`HANDOFF.md`、`docs/README.md`、`P3-*`、`未修复的问题.md` 原样） |

### 9.2 本轮工作区变化

**固件仓（`wb` @ `3acf91c`，未提交）**

| 类型 | 文件 |
|---|---|
| 新增 | `docs/architecture/HD-3-Remaining-Closure-Report.md`（本文件）· `docs/architecture/HD-3-A1-Recovery-Scenarios-and-RealDevice-Matrix.md` |
| 修改（本轮） | `src/log/log_manager.cpp`（`first_boot` 注入审计）· `src/log/log_meta_evidence.h`（场景矩阵注释）· `docs/interfaces/cloud_protocol.md`（§8.2.5） |
| 修改（前序轮次，未动） | `src/log/log_manager.h` · `src/main.cpp` · `src/services/device_identity.{h,cpp}` · `test/log_contract/run_contract_test.py` · `test/log_contract/probe_meta_slot.cpp` |

**云端仓（`main` @ `a3473eb`，未提交）**

| 类型 | 文件 |
|---|---|
| 新增 | `src/log-conflict.js` · `src/log-bootseq-guard.js` · `migrations/drafts/0004_log_conflict.sql` · `test/current/log_conflict.test.mjs` · `test/current/log_bootseq_guard.test.mjs` · `test/real_d1/log_conflict_real_d1.sh` |
| 修改（本轮） | `src/log-ack.js`（冲突接线 + `compareLogContent` 复用 `diffLogFields`）· `src/index.js`（cron 接入，默认关闭）· `test/run_all.mjs`（+2 套件） |
| 修改（前序轮次，未动） | `src/history-ingest.js` |

> ⚠️ 本轮**没有** `git add` / `git commit`；全部差异留在工作区，等待人工分批提交。

---

## 10. 最终未决事项（仅需人工决策/授权）

| # | 事项 | 需要 | 推荐 |
|---|---|---|---|
| 1 | `log_conflict` 草案投入生产（移到 `migrations/0004_log_conflict.sql` 并应用 + 部署 Worker） | **migration 应用 + 部署授权** | 批准：否则冲突只活在 3 天日志窗内 |
| 2 | `LOG_BOOTSEQ_GUARD=1` 是否在生产开启（含首次真实命中率评估） | **生产配置 + 只读查询授权** | 先在运维侧手动跑一次只读查询评估噪声，再决定默认值 |
| 3 | `A1-03` 远程可观测：是否实现 O-7（日志静默检测） | 小型实现授权（零协议变更） | 批准 O-7；O-3/O-6 留待需要区分原因时再评审 |
| 4 | 矩阵 ⑦（整片擦除）最终定义 | 产品/运维裁决 | 明确定义为"受控恢复出厂"，并以上表第 1/2 项为兜底 |
| 5 | 真机验证包 `RD-1`…`RD-8` | **设备 + 破坏性测试授权** | 按 `RD-3`→`RD-2`→`RD-7` 顺序（先验证阻断项） |
| 6 | `boot_seq` 精确判定（水位表 vs 设备上报） | 二选一 + 相应授权 | 水位表（不动设备协议） |
| 7 | `unverified` 分支与多行批的真实 D1 覆盖 | 夹具扩展授权 | 低优先；当前由 L1 覆盖 |

**结论**：本轮**可以**报告"A-1 的 8 个代码缺陷已闭环、B-3 已闭环、H-3-7 的 a/c 已落地、b 已本地实现"，
但**不得**把"真机未跑、migration 未应用、服务端检测未部署、O-7 未实现"标记为 PASS ——
整体 HD-3 维持 **`CONDITIONAL`**。
