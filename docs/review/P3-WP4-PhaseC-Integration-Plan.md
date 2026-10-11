# P3 · DT:WP-4 —— **Phase C 集成计划**（`PLAN ONLY` · **不执行**）

> ## 🟡 `PLAN ONLY` —— 本文档**仅规划**，**不执行任何动作**，**不解除任何 Gate**
>
> - **不做**：不改 EMQX · 不执行 remote migration · 不部署 · 不改 `src/` · 不改 migration · 不改 `DT:WP-4` / `DT:WP-1/2/3` 冻结文档 · **不 push**。
> - **前置事实**：`DT:WP-4 = FROZEN`（`2097c4e`）· **`WP-4 = IMPLEMENTED`**（`9b25203`，见 `P3-WP4-Implementation-Completion.md`）。
> - **本文档定位**：把「已实现的代码 + 已落地的本地 schema」推进到「线上可用」所需的**集成动作计划**。
> - **⚠️ 开工前必须先读 §5（集成阻塞发现）** —— 其中 **CF-1 / CF-2 为硬阻塞**，**须人工裁决后方可开工**。

| 项 | 值 |
|---|---|
| **状态** | **`Phase C = PLANNED`（未开工）** |
| **Gate** | **未改动**：`G-1 HUMAN_REQUIRED` · `G-2 PASS` · `G-3 PASS` · `G-4 BLOCKED` · `G-5 PASS` |
| **`P3`** | **`NOT STARTED / PLANNED`** |
| **三级子计划** | **C-1** EMQX HTTP Action 迁移 · **C-2** 远端 D1 migration · **C-3** 固件兼容性核对 |

---

## §1 目标、范围与依赖顺序

**目标**：让**真实设备上行**经 **EMQX** 投递到 **`POST /api/history/ingest`**，并让**远端 D1** 拥有 `telemetry_history` / `log_history`。

| # | 依赖关系 | 说明 |
|---|---|---|
| 1 | **C-3（核对）→ C-1（EMQX）** | 若固件侧无法产出冻结信封，C-1 的切换**无意义**（见 §5 CF-1/CF-2） |
| 2 | **C-2（远端 schema）必须先于 C-1（Action 切换）** | 否则 Action 一旦切到新端点，而远端**无表** ⇒ 全部 `503`（`persist_failed`）⇒ 上游重投风暴 |
| 3 | **migration 先于 Worker** | `migrations/README.md` 铁律：`0003` 必须先于（或同批于）Worker 部署 |
| 4 | **C-1 与 C-2 不可同一批次抢跑** | 建议顺序：**C-2 落表 → 验证 → C-1 切流 → 观测** |

**范围（IN）**：EMQX 规则/动作目标切换 · 远端 D1 应用 `0003` · 固件兼容性核对与差异登记。
**范围（OUT）**：固件代码改动 · Worker 代码改动 · `log_ack`（`计划:WP-5`，受 `G-4`）· ACL 改动 · 新 topic 引入。

---

## §2 C-1 · EMQX HTTP Action 迁移计划（**PLAN ONLY**）

### 2.1 现状（已核实）

| 项 | 事实 | 来源 |
|---|---|---|
| 配置方式 | **只能通过 EMQX Cloud 控制台**（API：`GET /rule_engine/rules`、`/bridges`、`/actions`、`/connectors` **全部 403**） | `EMQX_Assets/config/data-integration/README.md` §1 |
| 投递体（现） | `{"topic":"<完整 topic>","clientid":"<client_id>","payload":"<原始载荷字符串>"}` · `POST` | 同上（由 Worker 侧 `mqtt_messages` 落库反推） |
| 历史目标 URL | `https://guo-feeder-api.guobaby.workers.dev/`（**P0 遗留根路径 · 无鉴权**） | 同上 |
| 迁移目标（`计划:WP-3` 记） | `POST /api/ingest` + `X-Webhook-Key` | `P3-History-D1-Plan.md` §WP-3 |
| **`DT:WP-4` 冻结改指** | **`POST /api/history/ingest`**（**新专用端点** · `H-WP4-14` = `CLOSED` 选择 B） | `P3-DT-WP4-History-Ingestion-Design.md` §9.1 |
| 规则覆盖范围 | `guo_feeder/#` —— **含 `/up`、`/down`、`/log`** | 同上 §1 表 |
| 已知行为 | 规则**把下行 publish 回投给 Worker webhook**（`device_id=http_api`，即 `credential_set` 帧记账） | `EMQX_Assets/.../P1-6-3-E-3-validation-report.md` #2289 |

> **★ 口径裁决（已登记）**：`计划:WP-3` 写「复用既有 `/api/ingest`」，而 **`DT:WP-4` 冻结改指新端点**。
> **以冻结设计为准**（`DT:WP-4` = 实施唯一基线）；分歧本身已作为 `F-WP4-10 → CLOSED（选择 B）` 留痕。

### 2.2 需要验证的 5 项（**用户指定**）

| # | 验证项 | 判据（`PASS` 的定义） | 当前 |
|---|---|---|---|
| **V-1** | **endpoint change** | 规则动作 URL 由旧根路径/`/api/ingest` 切到 **`/api/history/ingest`**；**不保留双投**（避免双写） | **`NOT RUN`** |
| **V-2** | **telemetry path** | 设备 `/up` 上行 → Action → 新端点 → `telemetry_history` **真实 +1 行**（`device_id` 取自 **topic**） | **`NOT RUN`** · ⚠️ **受阻（CF-2）** |
| **V-3** | **log path** | 设备 `/log` 上行 → Action → 新端点 → `log_history` **真实 +1 行**；`payload` 原样、未解码 | **`NOT RUN`** · ⚠️ **受阻（CF-1）** |
| **V-4** | **`mqtt_messages` isolation** | 新端点路径**不写 `mqtt_messages`**（该表计数**不变**）；且**明确**旧 `credential_set` 记账的**去留裁决** | **`NOT RUN`** · ⚠️ 见 §5 CF-3 |
| **V-5** | **authentication unchanged** | 鉴权**仍为** `X-Webhook-Key`（**不放宽**）· 无 key ⇒ **401** · `source=cloud_api` ⇒ **400** · **不新增认证体系** | 端点侧**本地已验证**（48/48 中 2 项）；**EMQX 侧请求头配置** = `NOT RUN` |

### 2.3 执行步骤（**计划，未执行**）

| 步 | 动作 | 类型 | 前置 |
|---|---|---|---|
| 1 | 记录**变更前快照**：现规则 SQL / 现 Action（URL、方法、请求头、Body 模板） | 只读 | — |
| 2 | 裁定 **规则 SQL 范围**（见 CF-3：`guo_feeder/#` 保留 vs 收敛为 `/up` + `/log`） | **人工裁决** | §5 CF-3 |
| 3 | 裁定 **log 域信封产出方式**（见 CF-1，**硬阻塞**） | **人工裁决** | §5 CF-1 |
| 4 | 裁定 **telemetry 数据源**（见 CF-2，**硬阻塞**） | **人工裁决** | §5 CF-2 |
| 5 | 在控制台**新建/修改**规则动作 → `POST /api/history/ingest` + `X-Webhook-Key` | **执行（需授权）** | 步 2–4 |
| 6 | 用**测试设备**发一条 `/up` / 一条 `/log`，观测 `wrangler tail` + D1 行数 | 验证 | 步 5 + C-2 完成 |
| 7 | 断言 V-1…V-5；记录 `PASS / NOT RUN / FAIL` | 验证 | 步 6 |
| 8 | 观测窗口后决定是否**回退**（见 §9 回滚） | 决策 | 步 7 |

### 2.4 风险与护栏

| # | 风险 | 护栏 |
|---|---|---|
| R1 | **Action 先行、远端无表** ⇒ 全量 `503` + 上游重投风暴 | **C-2 必先完成**（§1 依赖 2） |
| R2 | **规则变成第三套 telemetry policy**（在 EMQX 做阈值/降频 = 重定义） | `DT:WP-1` 冻结：**telemetry policy 唯一权威 = 设备侧**；EMQX **只执行设备侧口径**，**不得**自行设阈值 ⇒ 规则 SQL **不得**引入新阈值/降频条件 |
| R3 | 双投（旧 + 新端点）⇒ 双写/重复计数 | V-1 明确**不保留双投**；切换即替换 |
| R4 | `/down` 回投命中新端点 ⇒ `400 unknown_topic`（噪声）+ 既有记账中断 | 见 §5 CF-3 |
| R5 | 密钥明文暴露 | `WEBHOOK_SECRET` **只**存 Worker Secret + 控制台 Action 请求头；**永不入库/入仓** |
| R6 | 规则配置**无法用 API 读回** ⇒ 易漂移 | 变更后**必须**把 SQL / Action 快照**照抄回仓**（`EMQX_Assets/config/data-integration/`） |

---

## §3 C-2 · 远端 D1 migration 计划（**PLAN ONLY**）

### 3.1 需要验证的 4 项（**用户指定**）

| # | 验证项 | 内容 | 当前 |
|---|---|---|---|
| **W-1** | **remote database identity** | 目标库 = **`guofeeder`** · `database_id = 0d8385ba-a1a6-4fcc-8e43-ae61c014c43d` · 绑定名 **`guofeeder_DB`** · 配置 = `guo-feeder-api/wrangler.toml`；执行前**必须**用 `wrangler d1 list` / `whoami` 二次确认账号与库 ID | **`NOT RUN`** |
| **W-2** | **current migration state** | 远端已应用清单（预期 `0001` ✅ / `0002` ✅ / **`0003` 未应用**）—— **远端状态 = 未核实** | **`NOT RUN`**（本地为 0001/0002/0003 全部 ✅） |
| **W-3** | **`0003` checksum** | 执行前记录并比对：`sha256 = cd103cd2b4eef95ecc6164b031cfcda06d190ead7ef3c4ffbcd6a93ef4e882c8` · `git blob = 2a12f7718c7c299a81c75001b559188ff9b777c9` · **138 行** | 已记录（本地） |
| **W-4** | **rollback strategy** | 见 §3.3（**D1 migration 前向单向**；回滚 = **补偿式 DDL**，非 `down` 脚本） | **已规划** |

### 3.2 执行步骤（**计划，未执行**）

| 步 | 动作 | 前置 |
|---|---|---|
| 1 | `wrangler whoami` + `wrangler d1 list` 确认账号/库 ID 与 `wrangler.toml` **一致** | 授权 |
| 2 | `wrangler d1 migrations list guofeeder --remote --config guo-feeder-api/wrangler.toml`（**只读**）→ 记录现状（W-2） | 步 1 |
| 3 | 记录 `0003` 校验和（W-3）并**冻结**「即将执行的字节」 | — |
| 4 | **先做数据安全网**：导出受影响范围快照（至少 `sqlite_master` DDL + 行数基线） | 步 2 |
| 5 | 执行 `wrangler d1 migrations apply guofeeder --remote`（**需人工授权**） | 步 1–4 |
| 6 | 回读断言：两表存在 · **恰好 5 索引** · `PRAGMA table_info` 逐字段对齐 `DT:WP-2` · `ux_*_dedup` 唯一 | 步 5 |
| 7 | 断言 **`0001` / `0002` 未被改动**（对照执行前 DDL 快照） | 步 6 |
| 8 | 记录 `W-1…W-4` 结果 + 执行前后 DDL 差异 | 步 7 |

### 3.3 回滚策略（`W-4`）

| 项 | 内容 |
|---|---|
| **前提认知** | **D1 migration 是前向单向的**（无自动 `down`）；「回滚」= **手工补偿式 DDL** |
| **窗口内回滚（无数据，推荐）** | `DROP INDEX IF EXISTS ux_th_dedup/ix_th_device_ts/ix_th_device_type_ts/ux_lh_dedup/ix_lh_device_ts;` → `DROP TABLE IF EXISTS telemetry_history; DROP TABLE IF EXISTS log_history;` → 从 `d1_migrations` **仅删 `0003` 记账行** |
| **已有数据后回滚（不推荐）** | **⚠️ 会丢历史数据** ⇒ 必须先导出、后裁决；**默认不做** |
| **旧端点向后兼容** | 旧 `POST /api/ingest` **整段未改动** ⇒ 若 Action 回切，旧链路**立即可用**（**这是首选的回退路径**，代价最低） |
| **执行前提** | 「删表」属**破坏性操作** ⇒ 须**人工授权** + 前置导出快照 |

### 3.4 风险与护栏

| # | 风险 | 护栏 |
|---|---|---|
| R7 | 误连**别的库/账号** | 步 1 **强制**二次确认库 ID |
| R8 | 远端存在**计划外前置迁移** | 步 2 **只读**比对；差异 ⇒ **停止并报告** |
| R9 | 破坏性回滚误伤 | §3.3 分级 + **人工授权** |
| R10 | 与 Worker 部署**不同批** | `migrations/README.md` 铁律：**migration 先于 Worker** |

---

## §4 C-3 · 固件兼容性核对清单（**PLAN ONLY** · 只读核对）

### 4.1 用户指定的 5 项

| # | 项 | 期望（`DT:WP-4` 冻结） | 固件现状（**已核实**） | 结论 |
|---|---|---|---|---|
| **F-1** | **current firmware topic** | `guo_feeder/<device_id>/up` · `.../log` | ✅ **已存在**：`services/topic_renderer.h:28-30`（`GF_TOPIC_TPL_UP` / `GF_TOPIC_TPL_LOG` / `GF_TOPIC_TPL_DOWN`）· 运行期唯一渲染点 `cloud_render_topic()`（`cloud_manager.cpp:1988-1989`、`2514-2515`） | ✅ **一致** |
| **F-2** | **`device_id`** | 12 位小写 hex · **身份只来自 topic** | ✅ 固件 device_id 为 P0 冻结的 12-hex（`services/device_identity.h`）；topic 第 2 段即其值 | ✅ **一致** |
| **F-3** | **`seq`** | log 幂等键分量 · 设备生成 | ⚠️ **log 有**：`LogRecord.seq`（`log_events.h:461`，**从 1 起**，CBOR 批次头含 `seq_from`/`seq_to`，`log_cbor.h:151-152`）<br>❌ **telemetry 无**：**固件无 telemetry 发布者** | ⚠️ **部分**（见 CF-2） |
| **F-4** | **`boot_seq`** | log 幂等键分量 · 设备生成 | ✅ **log 有**：`LogRecord.boot_seq`（`log_events.h:462`）+ 批次头 `LOG_BKEY_BOOT_SEQ=2`（`log_events.h:536`、`log_cbor.h:150`）· uint32 | ✅ **一致**（仅 log 域需要） |
| **F-5** | **`payload_base64`** | log 承载字段（**信封明文**） | ❌ **固件不产生**：`/log` 发的是**原始二进制 CBOR**（`cloud_send_log()` → `cloud_mqtt_publish_binary()`，`cloud_manager.cpp:2504-2544`），**无 JSON 信封**、**无 `payload_base64`** | ❌ **不一致**（见 CF-1） |

### 4.2 附加核对（为 C-1 提供判据）

| # | 项 | 事实 | 含义 |
|---|---|---|---|
| F-6 | **`/up` 当前载荷** | **文本 JSON**（`cloud_send_up()` → `cloud_mqtt_publish_text()`，`cloud_manager.cpp:2463-2494`），且经 `cloud_compress_uplink()` **压成旧协议短键**（`c`/`i`/`s`/`r`/`v`/`count`） | **不是 telemetry EAV**（`ts`/`type`/`value`/`seq`）⇒ telemetry 域会**整批 `rejected`** |
| F-7 | **`/up` 实际用途** | **命令结果 / registry 结果**（`on_command_result()` → `cloud_send_up()`，`cloud_manager.cpp:1692-1699`） | `/up` ≠ telemetry 通道（**现状**） |
| F-8 | **`/log` 载荷结构** | **CBOR 定序数组**：`[fmt, event_dict_ver, boot_seq, seq_from, seq_to, count, drop_ring, drop_overflow, drop_unacked, self_degraded, flags, records[]]`，`records[]` 每项 = **`bstr(128 B)` 的 packed `LogRecord`**（`log_cbor.h:14-20`、`log_events.h:455-473`） | **键字段（`level`/`seq`/`boot_seq`/`timestamp`）在 128 B 二进制结构体内部** |
| F-9 | **设备时间戳** | `LogRecord.timestamp`（uint32）**在记录内部**；`uptime_ms` 亦有 | **不在信封层**（见 CF-1） |
| F-10 | **`/up` telemetry 设备侧策略** | **未实现**（无 `type`/`value`/`seq` 生产者；`DT:WP-1` 的「设备侧唯一权威」尚无落地代码） | 见 CF-2 |

---

## §5 ★ 集成阻塞发现（**须人工裁决后方可开工**）

> **纪律**：**发现设计冲突不自行解决** —— 以下**仅记录 + 提出建议**，**不修改任何冻结文档**。

| ID | 级别 | 发现 | 影响 | 建议 |
|---|---|---|---|---|
| **CF-1** | **🔴 硬阻塞** | **log 域信封无法产出**：`DT:WP-4` 冻结要求 log 信封**明文携带** `boot_seq`/`seq`（`H-WP4-16`）+ `payload_base64`；但固件**只发二进制 CBOR**（F-5/F-8/F-9），键字段**埋在 128 B packed 记录内部**。⇒ 三条既有约束**互斥**：① EMQX 规则引擎**无法解码 CBOR**；② **Worker 被明令禁止解码**（`H-WP4-05`）；③ 固件**当前不发 JSON 信封**。 | **log 域 ingest 无法真实跑通**（V-3 无法 `PASS`） | **三选一（人工裁决）**：(a) **固件侧**新增 JSON 信封发布（`payload_base64` + 明文键字段）—— 属固件工作流，**超出 WP-4**；(b) 在 EMQX 前加**独立解码中间层**（新组件，需新裁决）；(c) 重新裁决 `DT:WP-4` log 承载方式（**会触碰冻结设计，须人工**） |
| **CF-2** | **🔴 硬阻塞** | **telemetry 域无数据源**：固件**不存在** telemetry 发布者；`/up` 今日只跑**命令结果 / registry**（旧协议短键，F-6/F-7）⇒ 接入 `/up` 的 telemetry 记录会**全部 `rejected`**（缺 `ts`/`type`/`value`/`seq`）。 | **telemetry 域 ingest 无从验证**（V-2 无法 `PASS`） | 人工裁决：**telemetry 上行由哪一阶段实现**（固件侧 telemetry policy 落地 = WP-1 的实施）⇒ 建议**独立工作流**先行，Phase C 的 telemetry 项**顺延** |
| **CF-3** | **🟠 需裁决** | **`/down` 回投语义变化**：规则覆盖 `guo_feeder/#`（**含 `/down`**），现状把下行 publish **回投给 Worker** 记账（`credential_set` 帧，`mqtt_messages` #2289）。若 Action 切到 `/api/history/ingest`，`/down` 会命中新端点 ⇒ **`400 unknown_topic`**（无副作用但**噪声**），且**既有记账中断**。 | V-4 的「隔离」看似满足（新端点不写 `mqtt_messages`），但**旧记账能力被换掉** | 人工裁决：**(a)** 规则 SQL **收敛为** `guo_feeder/+/up` + `guo_feeder/+/log`（**推荐**，语义最干净）；**(b)** 保留 `#` 并接受 400 噪声；**(c)** 旧 Action 保留给 `/down`、新 Action 专责 `/up`+`/log`（双 Action，需接受**双投**风险 R3） |
| **CF-4** | **🟢 已登记** | **口径分歧**：`计划:WP-3` 记 Action → `/api/ingest`，`DT:WP-4` 冻结改指 `/api/history/ingest` | 无（**以冻结设计为准**） | 无需裁决；已在 C-1 §2.1 显式留痕 |

**⇒ 结论**：**C-1 的 V-2（telemetry）与 V-3（log）在 CF-1 / CF-2 裁决前无法开工**；
**C-2 不受 CF-1/CF-2 阻塞**（schema 与数据源无关），**可先行**。

---

## §6 允许 / 禁止清单（Phase C 执行期）

| ✅ 允许 | ❌ 禁止 |
|---|---|
| 只读核对（`wrangler whoami` / `d1 list` / `migrations list`） | 修改 `src/`（固件或 Worker） |
| 记录并冻结 `0003` 校验和 | 修改/新增 migration（除人工授权执行 `0003` 本身） |
| 在**授权后**执行 `0003` 的 `--remote` 应用 | 修改 `DT:WP-4` / `DT:WP-1/2/3` 冻结文档 |
| 在**授权后**于 EMQX 控制台切换 Action | 修改 ACL（P2-5 的 5/5 规则**保持不动**） |
| 变更后把规则/Action 快照**存档回仓** | 实现 `log_ack`（属 `计划:WP-5`，受 `G-4`） |
| 用测试设备观测（`wrangler tail` / D1 计数） | **push**（`push` 仍 `DEFERRED`）· 未经授权部署 |

---

## §7 HUMAN_REQUIRED 清单

| ID | 问题 | 关联 |
|---|---|---|
| **`H-WP4C-01`** | **log 域信封产出方式**（固件 JSON 信封 / 独立解码层 / 重裁 `DT:WP-4` 承载） | **CF-1（硬阻塞）** |
| **`H-WP4C-02`** | **telemetry 上行何时/由谁实现**（固件侧 telemetry policy 落地），Phase C 的 telemetry 项是否顺延 | **CF-2（硬阻塞）** |
| **`H-WP4C-03`** | **EMQX 规则 SQL 范围**（`#` 保留 / 收敛 `+/up`+`+/log` / 双 Action） | CF-3 |
| **`H-WP4C-04`** | **是否授权执行远端 `0003`**（`--remote`）· 是否需前置导出 | §3.2 |
| **`H-WP4C-05`** | **是否授权切换 EMQX Action**· 变更窗口 / 回退触发条件 | §2.3 |
| **`H-WP4C-06`** | **部署顺序确认**（`0003` → Worker → Action，且**不同批抢跑**） | §1 依赖 |

---

## §8 验收（四态严格区分 · **规划口径**）

| 项 | 目标态 | 判定者 |
|---|---|---|
| C-2（远端 schema） | `PASS`（两表 + 5 索引 + `0001/0002` 未变） | 静态 + 回读断言 |
| C-1 V-1（endpoint change） | `PASS` | 控制台快照 + 观测 |
| C-1 V-5（authentication unchanged） | `PASS` | 401/400 行为断言 |
| C-1 V-4（`mqtt_messages` isolation） | `PASS`（**且 CF-3 已裁决**） | D1 计数 |
| C-1 V-2 / V-3 | **`BLOCKED`**（CF-1 / CF-2 未裁决前**不得**声称通过） | — |
| 固件 telemetry publisher | **`NOT RUN`** | — |
| 固件 log JSON 信封 | **`NOT RUN`** | — |

> **⚠️ 术语铁律**：`NOT RUN` / `BLOCKED` / `HUMAN_REQUIRED` **一律 ≠ `PASS`**。

---

## §9 回滚与安全网（汇总）

| 层 | 回退手段 | 代价 |
|---|---|---|
| **EMQX Action** | 控制台**回切**到变更前 URL/模板（快照见 §2.3 步 1） | **最低** |
| **Worker 端点** | 旧 `POST /api/ingest` **整段未改动** ⇒ 旧链路随时可用 | **最低** |
| **远端 D1 `0003`** | 补偿式 DDL（§3.3）；**删表会丢历史数据** | **高**（需授权 + 导出） |
| **固件** | 本轮**无固件改动** ⇒ 无需回退 | — |

---

> **Phase C = PLANNED（NOT STARTED）. No EMQX change. No remote migration. No deploy. No push.**
> **C-1 V-2 / V-3 = BLOCKED by CF-1 / CF-2（须人工裁决）.**
> **C-2 is not blocked and may proceed first.**
> **No Gate changed. P3 remains NOT STARTED / PLANNED.**

---

*本计划于 **2026-10-11** 建立；前置 = `DT:WP-4 = FROZEN`（`2097c4e`）+ `WP-4 = IMPLEMENTED`（`9b25203`）；
为**规划文档**，不含任何执行动作、代码改动或配置变更。*
