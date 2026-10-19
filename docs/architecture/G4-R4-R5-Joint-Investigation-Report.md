# G4-R4 / G4-R5 联合只读调查报告

> 性质：**只读调查 + 方案比较 + 实施计划**。**不进入实施**。
> 调查窗口：`2026-10-20T03:22Z – 03:5xZ`（本地 `2026-10-20 03:2x – 03:5x`）
> 设备：`288485896ce4`（**本轮未与设备交互**：未发命令、未开串口、未复位、未烧录）
> 关联：`G-4` 门禁（`BT-1-Acceptance-Report.md` §6.4/§6.5 `GATE-WP5` ②）· `G4-R4`（DRAM 口径）· `G4-R5`（`mqtt_messages` 漏记/重复）
> ★ 门禁状态**未变**：`BT-1 = CLOSED` · **`G-4 = BLOCKED`** · `WP-5` 未解锁 · `STOP-R1` 无解除。

---

## 1. 调查摘要与基线

### 1.1 三仓基线（实测，非推断）

| 仓 | 分支 | HEAD | 工作区 | 与远端 |
|---|---|---|---|---|
| 固件 `PIO_Assets/Guo_Feeder_Project` | `wb` | **`bc585e8`** | 1 个**有意未跟踪**文件（`docs/evidence/G-4-Stats-2026-10-20/pre-flash-firmware.bin`，1.4 MB） | `origin/main == bc585e8`（`ahead 0`） |
| 云端 `Cloudflare_Assets` | `main` | **`ccb5839`** | 干净 | `ahead 0` |
| 消息 `EMQX_Assets` | `main` | **`ec2364f`** | 干净 | — |

### 1.2 生产部署基线（**不以提交记录代替部署核实**）

| 项 | 实测值 | 取证方式 |
|---|---|---|
| Worker 线上版本 | **`826332ed-3a73-42c4-a090-a5bf737dc210`**（创建 `2026-10-19T12:56:06Z`），100% 流量 | `wrangler deployments list` |
| 回退目标 | `f9647a00-e079-4de9-bcf1-8c9bc5344185`（`2026-10-18T20:18:43Z`） | 同上 |
| 源码与部署的对应 | ⚠️ **不能由时间戳证明**：部署 `12:56:06Z` **早于** `ccb5839` 的提交 `17:48:57Z`，而 `ccb5839` 恰恰**改动了 `src/index.js`（140 行）与 `src/emqx-admin.js`（7 行）** ⇒ 只能判为**强推断**（依据 `2026-10-19` 已完成的 P-2 真机 E2E 行为：错误 topic 返回 `no_matching_subscribers`、正确 topic 2.7 s 三帧闭合），**不是字节级证据** | `wrangler deployments list` + `git show --stat ccb5839` + P-2 E2E 报告 |
| D1 库 | `guofeeder`（`0d8385ba-a1a6-4fcc-8e43-ae61c014c43d`） | `wrangler.toml` |
| 设备固件 / boot | `72d7a610…744c`（commit `23bee8e`）· `boot_seq = 23` | 上一轮证据（**本轮未复查设备**） |

> ⚠️ **本轮未核实**：EMQX 规则/动作的**运行态配置**（API 全 403，见 §9-G1）。所有 EMQX 侧论述均基于 `2026-10-17` 控制台人工转录快照 `EMQX-Live-Rules-Snapshot-2026-10-17.md`，该快照**本身已标注 3 处证据缺口**（`G-1`/`G-2`/`G-3`）。

### 1.3 一句话结论

| 项 | 结论 |
|---|---|
| **G4-R4** | `<70%` **从未被批准**（源表原文写"未批准"，是"建议阈值"），且**空载基线即 77.08%** ⇒ 该阈值在本设备**不可满足**，永远只能输出 `FAIL`。真正该保护的资源是 **`largest_free_block`（碎片）/ `minimum_free`（绝对余量）/ 分配失败**，不是"占用率"。**推荐方案 4**（`<70%` 降为参考，硬门禁改用风险导向多指标）；**数值需你批准**。 |
| **G4-R5** | 拆成**三个独立缺陷**：**①`device_id` 归属错误**（`$tenants/` 与 `/down` 都无法解析 ⇒ 回落到 `http_api`，已污染 **1 463 行**，含 **1 202 条 `log_ack`**）；**②确定性双写**（每条下行命令写 2 行：Worker 审计行 + EMQX 回环行，`topic` 前缀可区分）；**③漏记 ~10.8%**（`down` 零丢失、`/log` 通道**零丢失**，只有 `/up` 经 rule B 的通道丢 ⇒ **锁定在 rule B 的动作**）。**D1 侧只能消重，不能补漏**；补漏需 EMQX 控制台权限。 |

---

## 2. 任务 A：G4-R4 —— DRAM 门禁口径

### 2.1 现行门禁原文（含出处）

| 项 | 内容 | 出处 |
|---|---|---|
| 指标 | 内部 DRAM **峰值占用率** | `docs/review/BT-1-Log-ACK-Read-Only-Investigation-and-Fix-Plan.md` **§6.2 行 897** |
| 阈值 | **< 70%** | 同上；`§6.4` 行 937 · `§6.5` 行 964（`GATE-WP5` ② · `T10`） |
| 公式 | `(internal.total − internal.minimum_free) / internal.total` | `G-4-Gate-Revision-Record-2026-10-20.md` §1 |
| 测量方法 | `heap_caps_get_free_size(MALLOC_CAP_8BIT / SPIRAM)` **最小水位**，周期采样 | `BT-1` 计划 §6.2 |
| 建议时长 | 连续 2 h 稳态 + 30 min 突发 | 同上 |

### 2.2 ★ 关键发现：`<70%` **是"建议阈值"，从未获批**

源表 **§6.2 标题**即写明："**待测项，不编数值**"，列头为"**建议阈值（需人工批准）**"。
`§6.5` 的分阶段门禁表对该行的批准状态原文：**"`未批准`（`< 70%` 同上）"**。

⇒ **形式结论**：把它当"硬性产品要求"或"已批准门禁"表述**不成立**；它的实际身份是
**`HUMAN_REQUIRED` 的待批建议值**。这也解释了为什么它从未在本设备上满足过。

> 但**历史 `FAIL` 照旧保留、不改写**（用户 `2026-10-20 01:20` 已裁决）：历史报告的表述问题另立，
> 不追溯修改判定。

### 2.3 字段精确定义（读代码，非文档转述）

`src/services/system_command.cpp::syscmd_memory()` + `fill_ram_block()`：

| 字段 | 取值 | 语义 | 类型 |
|---|---|---|---|
| `internal.total` | `ESP.getHeapSize()` | **内部 heap 区总容量**（本设备实测 250 428 – 251 172 B，**随时刻小幅浮动**） | 当前值 |
| `internal.free` | `ESP.getFreeHeap()` | 当前空闲 | **瞬时** |
| `internal.used` | `total − free`（下界钳 0） | 当前已用 | **瞬时** |
| `internal.largest_free_block` | `ESP.getMaxAllocHeap()` | **当前最大可分配连续块**（碎片直接指标） | **瞬时** |
| `internal.minimum_free` | `ESP.getMinFreeHeap()` | **自 boot 起最小空闲**（分配器维护，单调不增，**不依赖采样**） | **历史极值（per-boot）** |
| `external.*` | `ESP.getPsramSize()` 等；无 PSRAM 时全 0 | PSRAM | 瞬时/极值 |

★ **`system.stats` 不含任何内存字段**（已核对 `syscmd_stats` 全部键名：`queue`/`flash`/`counters`/`boot_seq`/`uptime_ms`…）⇒ 内存必须**并行发 `system.memory`**，无法与 `stats` 原子同帧。

### 2.4 三个口径缺陷（这是 `G4-R4` 的实质）

| # | 缺陷 | 证据 | 后果 |
|---|---|---|---|
| **D-1** | **分子分母不同源**：`minimum_free` 是**历史最低**，`total` 是**当前值**，二者不是同一时刻 | `total` 实测在 250 428–251 172 B 间浮动；`minimum_free` 发生在过去某刻 | 计算出的"峰值占用率"**不是任何一个真实时刻的状态**；误差量级小（±0.05 pp）但**语义不成立** |
| **D-2** | **分母选了 heap 区而非 SRAM 总量**，且与 PlatformIO 的百分比**同形不同义** | `syscmd_memory` = 251 KB 口径；PlatformIO `RAM: 133344 (40.7%)` 的分母是 **327 680**（SRAM 总量）⇒ 两者相差 30 个百分点 | 任何"某报告说 40% 某报告说 77%"的对照**必须先对齐分母**，否则是假矛盾 |
| **D-3** | **绝对阈值对"已高度占用"的设备失效**：空载即 77.08% | `(251060−57544)/251060 = 77.08%`；压力后 `(251172−57356)/251172 = 77.16%` | 阈值**恒 FAIL**，**测不出压力增量**（真正的风险信号是 +300 B / +0.09 pp） |

### 2.5 历史三次测量的原始数据与出处

| 时间（UTC） | 场景 | `total` | `minimum_free` | 占用率 | 出处 |
|---|---|---|---|---|---|
| `2026-10-19 15:31` | 方案 A（35 条突发） | 250 996 | 43 168 | **82.8%** | `G-4-Root-Cause-Investigation-Report.md` §（DRAM 行） |
| `2026-10-19 16:30` | boot 22 空载基线 | 251 060 | 57 544 | **77.08%** | `G-4-System-Stats-…-Report.md` §门禁表 |
| `2026-10-19 16:50` | 105 条命令四级压力后 | 251 172 | 57 356 | **77.16%** | 同上 |
| `2026-10-19 18:38` | boot 23（S-3 固件） | 250 428 | 55 180 | 77.97% | `docs/evidence/S-3-Flash-2026-10-20/post-flash-state.txt` |

> ★ **跨 boot 不可比**：`minimum_free` 每个 boot 周期重新累计（`G4-M1`）。上表第 4 行只说明"boot 23 的基线量级与 boot 22 相近"，**不能**据此说"内存变好了/变坏了"。

**★ 一个必须点明的事实**：历史上**唯一有意义的恶化信号**是方案 A 期间 `minimum_free` 从 **61 124 → 43 168（−29.4%）**（`G-4-Root-Cause-Investigation-Report.md:81`）。
而在占用率口径下，这只是"82.8% → 77.08%"，**两次都判 `FAIL`** ⇒ **该指标无法区分"内存掉了 29.4%"和"什么都没发生"**。这是 D-3 的直接实证，也是推荐方案 ④ 的核心理由。

### 2.6 真正需要保护的资源（A2）

| 资源 | 当前实测（boot 23 · `18:38Z`） | 能证明什么 | **不能**证明什么 |
|---|---|---|---|
| 剩余 DRAM | `free = 65 960 B`（26.3%） | 有富余 | 不能证明"突发时也够" |
| 最大连续块 | `largest_free_block = 57 332 B` | 碎片不严重（是 `free` 的 86.9%） | 不能证明"任何尺寸分配都成功" |
| 运行时最低 | `minimum_free = 55 180 B` | 本 boot 内最坏还有 55 KB | **不是**泄漏证据；也**不能跨 boot 比较** |
| 栈余量 | **无任何可用出口**（仅 3 处 `uxTaskGetStackHighWaterMark` 打在串口日志里，`workflow.create` / `CapRegistry.sync`，**不经命令暴露**） | — | **栈风险完全不可观测** |
| 分配失败 | 代码只有 `heap_caps_malloc` 返回 NULL 后的局部日志（`[CapRegistry] sync oom` 等），**无全局计数器** | — | **"零分配失败"无法取证** |

**当前证据能得出的最强结论**：在 boot 22/23 的**缩减配置**（无秤、OLED 未实际传输、无电机模块）下，内部 DRAM 的**最坏余量约 55 KB**，`largest_free_block` 全程未下降。**不能**推出"完整配置下安全"，也**不能**推出"无泄漏"。

### 2.7 方案比较（A3）

| 方案 | 工程含义 / 安全边界 | 需要的变更 | 需重烧？ | 需目标板数据 | 误报 / 漏报 | 维护成本 | 与历史证据兼容 |
|---|---|---|---|---|---|---|---|
| **① 保留 `<70%`** | 名义上最保守，实际**恒 FAIL** ⇒ 门禁永久卡死 | 无 | 否 | 已有 | **100% 误报**（基线即 77%）；漏报压力增量（+0.09 pp 被 77% 掩盖） | 0 | ✅ 字面兼容，但**无法解锁** |
| **② 修正口径后仍用 `<70%`** | 想把 77% 修到 70% 以下，只能换分母（如剔除 WiFi/TLS 静态占用）或改成"相对基线" | 改公式 + 重算全部历史 | 否 | 需重算 4 组历史数据 | 中：换分母易变成"为了过门禁而换尺子" | 中（每换分母都要重写历史） | ❌ **实质是追溯改写**，与"不改写历史"冲突 |
| **③ 多指标硬门禁** | 直接对准真实失效模式：碎片、绝对余量、漂移、分配失败、栈 | **代码**：暴露栈水位 + 分配失败计数（新 `system.health` 或并入 `system.memory`）；文档改写门禁 | **是（1 次）** | 需新 boot 基线 + 完整配置窗口 | 低（每项各有物理含义）；漏报主要是"慢泄漏"（需长窗口） | 中（5 项阈值需一并维护） | ✅ 历史 `FAIL` 保留，新口径**只用于未来** |
| **④ `<70%` 降为参考 + 风险指标做硬门禁** | = ③ 但显式把占用率**降级为 informational**，避免"参考值被当门禁"的历史重演 | 同 ③ + 在门禁文件里写明"`<70%` 仅供参考" | **是（1 次）** | 同 ③ | 同 ③ | 同 ③（+1 行文档说明） | ✅ 完全兼容：历史判定不动，参考值继续可算 |

**推荐：方案 ④**。理由：
1. **不追溯改写**任何历史判定（方案 ② 会破坏这条铁律）；
2. 对准真实失效模式——**分配失败**由 `largest_free_block` 与绝对余量驱动，而不是"用了百分之几"；
3. 与已裁决的 `TRIAL-DRAM`（T-1…T-4）**同构**，只是把"仅试运行"提升为"正式门禁"，减少一次口径迁移；
4. 顺带把"栈余量 / 分配失败"这两个**当前完全不可观测**的项纳入，这是 ③④ 相对 ①② 的最大增益。

### 2.8 候选阈值（**推导依据已给，但数值必须由你批准**）

| # | 指标 | 候选阈值 | 推导依据 |
|---|---|---|---|
| H-1 | `largest_free_block` | **≥ 32 KiB** | 固件最大单笔内部 RAM 动态分配 = `LOG_CLOUD_QUEUE_SLOTS(128) × LOG_RECORD_SIZE(128) = 16 384 B`（`log_manager.cpp:3853`，PSRAM 失败时的**内部 RAM 回落路径**）。取 **2×** 余量 ⇒ 32 KiB。当前 57 332 B，余量 **1.75×** |
| H-2 | `minimum_free` | **≥ 24 KiB** | H-1 需求 16 KiB + 8 KiB 供 TLS/WiFi 瞬态。当前 55 180 B |
| H-3 | 稳态漂移 | **≤ 1 KiB / 30 min**（斜率） | 慢泄漏检测；`TRIAL-DRAM` T-1 的放宽版（试运行用 512 B 静置差） |
| H-4 | 分配失败计数 | **= 0**（需新增计数） | 直接失效证据；当前无此计数 |
| H-5 | `loopTask` 栈 HWM | **≥ 2 KiB**（`loopTask` 栈 16 KB） | 项目铁律"单对象 >1 KB 禁上栈"；当前无出口 |
| — | 占用率 `(total−min_free)/total` | **仅记录，不判定** | 见 D-1/D-2/D-3 |

★ **H-1…H-5 全部是"候选值"**，本报告不代替你批准；`H-4`/`H-5` 需要**代码改动 + 1 次烧录**才有数据。

### 2.9 采集时机建议（省一次设备占用）

`G4-R4` 需要的证据与 **S-3 已延期的 2.5 h 门禁窗口**（`S-3-Spec-Revision-R3.md` §2）**完全同窗口**：
同一窗口可同时产出 `loop()` P99 + DRAM（`free`/`min_free`/`largest`/漂移）+ 计数器。
⇒ **建议合并执行，不要为 `G4-R4` 单独再开一次窗口。**

---

## 3. 任务 B：G4-R5 —— `mqtt_messages` 重复与漏记

### 3.1 完整链路（以当前代码为准）

```
设备 (esp-mqtt, QoS1)
  │  guo_feeder/<dev>/up   ← result / ack / s:1
  │  guo_feeder/<dev>/log  ← 日志帧
  ▼
EMQX broker
  ├─ 规则 A  r-…cf0cf2  FROM "guo_feeder/+/up","guo_feeder/+/log"
  │     WHERE (7 项 up 谓词) OR (log 谓词)         → 动作 a-…（Config 未取证）
  │     → Pages /api/history/ingest → Worker → D1: telemetry_history / log_history
  │                                              （**INSERT OR IGNORE** + 唯一索引）
  └─ 规则 B  r-…f731cf  FROM "guo_feeder/#"
        WHERE (7 项 OR 展开) AND NOT (topic =~ 'guo_feeder/+/log')
        → 动作 a-…（Config 未取证）→ Pages /api/webhook → Worker "/" (P0 旧通路)
                                                    → D1: mqtt_messages（**裸 INSERT**）

Worker 侧另一条写入口（**不经 EMQX**）：
  Pages /api/command → body.source="cloud_api" → Worker "/"
        ├─ INSERT mqtt_messages（**审计行**，device_id 显式取自 body）  ← 写点 1
        └─ publishToDevice()  → EMQX HTTP API publish（**默认 QoS 0**）
                                  └→ 若 topic 匹配规则 B ⇒ 回环 ⇒ 再办一次写点 2
```

**代码位置（逐一核对）**

| 环节 | 位置 |
|---|---|
| 路由与早退 | `src/index.js:194`（`fetch`）· `:249` presence · `:270` history-ingest · `:207` admin |
| `source` 判定 | `src/index.js:373` `body.source \|\| "mqtt"` |
| `topic` / `device_id` 派生 | `src/index.js:418`（cloud_api 显式构造 + `isValidDeviceId` 拒绝）· `:450`（MQTT 路径 `body.topic` + `deviceIdFromTopic(topic) \|\| body.clientid`） |
| 方向判定 | `src/index.js:495`（仅看 topic 后缀 `/up` / `/down`） |
| **`mqtt_messages` 写入** | `src/index.js:584`（裸 `INSERT`，**无去重、无幂等键、无唯一约束**） |
| 写入失败处理 | `src/index.js:658` catch ⇒ `500 "DB ERROR"` |
| 下行发布 | `src/index.js:696` → `emqx-admin.js:399 publishToDevice(… { qos = 0 })` |
| `log_ack` 发布 | `log-ack.js:726` → `publishToDevice(… { qos: 1, timeoutMs })`；**无审计行写入** |
| 幂等范式（对照） | `history-ingest.js:688/717/720` `INSERT OR IGNORE` + `ux_th_dedup` / `ux_lh_dedup` |

### 3.2 表结构与约束（实测）

```sql
-- mqtt_messages（P3 之前的既有表，仓库内没有它的 migration；mqtt-columns.js:23 已注明）
id INTEGER PRIMARY KEY AUTOINCREMENT, device_id TEXT NOT NULL, topic TEXT, direction TEXT,
raw_payload TEXT, cmd TEXT, msg_id TEXT, object TEXT, stable_id INTEGER, version INTEGER,
status INTEGER, error_code INTEGER, message TEXT, timestamp INTEGER, payload_json TEXT,
created_at INTEGER, status_text TEXT, error_text TEXT, msg_type_text TEXT, msg_type INTEGER
-- ★ 除自增主键外：0 个唯一约束、0 个索引
```

对照（这正是 `/log` 通道**零丢失零重复**的原因）：

```sql
CREATE UNIQUE INDEX ux_lh_dedup ON log_history(device_id, boot_seq, seq);
CREATE UNIQUE INDEX ux_th_dedup ON telemetry_history(device_id, type, ts, seq);
```

### 3.3 逐 ID 对账（原始数据，窗口 `2026-10-19 16:32:26 – 16:42:25Z`）

**源端唯一消息集合** = 四级突发实投 **105** 条命令（`g4L1/L2/L3/L4…`，取自 `docs/evidence/G-4-Stats-2026-10-20/burst-L{1..4}.json`）。
每命令的**预期帧**（已由单 ID 全量取证确认，例 `g4L10045595`）：
`down`(审计) + `down`(回环) + `up`(ack) + `up`(result s:1) + `up`(result s:0) = **5 行**。

| 集合 | 实测 | 预期 | 差 |
|---|---|---|---|
| **下行审计行**（`topic=guo_feeder/<dev>/down`, `device_id=288485896ce4`） | **105** | 105 | **0** |
| **下行回环行**（`topic=$tenants/n302933b/…`, `device_id=`**`http_api`**） | **105** | 105 | **0**（但**归属错误**） |
| **上行 `ack`** | **96**（96 个 ID） | 105 | **−9（8.6%）** |
| **上行 `result` s:1（queued）** | **96 行 / 95 个 ID** | 105 | **−9 行**，另 **+1 重复** |
| **上行 `result` s:0（final）** | **89** | 105 | **−16（15.2%）** |
| **上行合计** | **281 行 / 97 个 ID** | 315 | **−34（10.8%）** |
| **D1 侧总行数（`g4L%`）** | **491**（210 down + 281 up） | 525 | −34 |

**逐 ID 异常清单（16 个 ID）**：

| ID | ack | s:1 | s:0 |
|---|---|---|---|
| `g4L30939127` | 1 | **2** ← 唯一真实重复 | 1 |
| `g4L32839127` | 1 | 0 | 0 |
| `g4L33039127` / `g4L33239127` | 0 | 0 | 0 |
| `g4L33339127` | 0 | 1 | 0 |
| `g4L437/438/440/442/449…75145` | 1 | 1 | **0** |
| `g4L439/441/443/445/446/447…75145` | **0** | **0** | **0** |
| `g4L44475145` | 1 | 0 | 0 |

★ **丢失按秒聚簇**（同一 `HHMMSS` 后缀的 ID 成组丢失），**不是随机分布** ⇒ 指向"某一秒内后端短暂拥塞/丢弃"，而不是"某类帧永久不匹配"。

### 3.4 三段交叉验证（这决定了"漏在哪一段"）

| 环节 | 证据 | 结论 |
|---|---|---|
| **设备是否发出** | 串口 `[Cloud UP] OK` **347** / `[Cloud UP] FAIL` **0** / `MQTT offline` **0**（`cloud_send_up` 用 **QoS1**，`OK` 表示**收到 PUBACK**） | ✅ **设备全部发出，Broker 全部确认收到** |
| **Worker 是否处理失败** | Cloudflare GraphQL `workersInvocationsAdaptive`（`16:30–16:45Z`）：**845 次调用，`status=success`，`errors=0`** | ✅ **Worker 一次 5xx 都没有** ⇒ 不是"写了但失败" |
| **D1 是否写失败/被删** | 表内无 `msg_id IS NULL` 的兜底行；无删除逻辑；`mqtt_messages` 无 retention | ✅ **不是"写进去了又被删"** |
| **`/log` 通道对照（决定性微分）** | `log_history` boot 22：**146 行，`seq 5377–5522`，`min+count−1 == max` ⇒ 零缺口** | ✅ **同窗口、同设备、同库，规则 A 通道 100% 到达** ⇒ 丢失**只发生在规则 B 的动作** |

⇒ **漏记发生在「EMQX 规则 B 的动作 → Pages → Worker」这一段，且丢的是报文本身（未到 Worker），不是写入失败。**

### 3.5 重复的两类（性质完全不同）

**D-1 确定性双写（105/105 = 100%）**

| 行 | `topic` | `device_id` | `raw_payload` |
|---|---|---|---|
| id 4801 | `guo_feeder/288485896ce4/down` | `288485896ce4` | `{"device_id":"…","c":"system","i":"g4L10045595","p":{…},"source":"cloud_api"}` |
| id 4802 | **`$tenants/n302933b/`**`guo_feeder/288485896ce4/down` | **`http_api`** | **同上（逐字节相同）** |

⇒ 一条命令 = **两个写入口各写一行**：Worker 审计行 + EMQX 回环行。**不是重试。**

**D-2 偶发重投（1/315 ≈ 0.3%）**

`g4L30939127` 的 `result s:1` 两行：id 5087 (`…766684`) 与 id 5092 (`…768210`)，间隔 **1.53 s**，`raw_payload` 长度均 **98 B**、内容相同（`t` 相同）。
⇒ 与"动作重试"一致（首次请求慢/超时 → 重试 → 两次都写成功）。

### 3.6 ★ 附带发现：`device_id` 归属错误（本报告首次定位到代码级根因）

`src/device-registry.js::deviceIdFromTopic()`：

```js
if (parts.length !== 3) return null;                            // ① $tenants/<t>/guo_feeder/<id>/down = 5 段 ⇒ null
if (parts[0] !== "guo_feeder") return null;
if (parts[2] !== "up" && parts[2] !== "log") return null;        // ② 明确不接受 /down ⇒ 任何 down 都 null
```

⇒ 对 `/down` 上的任何消息（包括 `$tenants/…` 的回环），`deviceIdFromTopic()` 恒返回 `null`
⇒ `index.js:452` 回落 `body.clientid`；而 EMQX HTTP API 发布方的 clientid = **`http_api`**
⇒ **`device_id` 被写成 `http_api`**。

**实测量化**：

| `device_id` | 行数 | 时间跨度 | 性质 |
|---|---|---|---|
| `288485896ce4` | 1 895 | 10-03 → 10-19 | 真实设备（含 `up` 全量与 `down` 审计行） |
| `guo_feeder_001` | 1 660 | 08-23 → 10-01 | 旧设备/回滚指纹 |
| **`http_api`** | **1 463** | 08-23 → 10-19 | **伪设备**：含 **1 202 条 `log_ack`** 回环 + 105 条 `g4L` 回环 + 其余历史 |
| `mqttx_9c9a7813` / `phone` / `cloud_api` / `dev_288485896ce4` / `wb_log_mirror` … | 149 / 96 / 43 / 77 / 88 | — | 测试与工具残留 |

**影响（很重要）**：
1. **按 `device_id` 过滤的正常查询看不到这些行** ⇒ 历史上"某 ID 只有 1 行"与"某 ID 有 2 行"两种结论**都曾出现**，其实是**同一份数据的不同过滤方式**，并非口径漂移。
2. **`log_ack` 的云端记录 100% 落在 `http_api` 名下，且没有任何其它冗余**（`log-ack.js` 不写审计行）⇒ **ACK 审计既单源、又错主、又经同一有损通道**。
3. `device_id` 是 `NOT NULL` 却未校验 ⇒ 伪设备不会报错，**静默污染**。

### 3.7 对 `log_history` / `telemetry_history` / ACK 判定的影响（分别给证据）

| 表 / 判定 | 是否受影响 | 证据 |
|---|---|---|
| `log_history` | **不受影响** | boot 22 `seq 5377–5522` 零缺口；写入走 `INSERT OR IGNORE` + `ux_lh_dedup` |
| `telemetry_history` | **不受影响**（同机制） | `ux_th_dedup` 存在；本轮窗口无 telemetry 突发 |
| **ACK 判定（业务正确性）** | **不受影响** | 设备侧 `acked_seq = 5377 + emit − 1` 恒等、`ack_timeout=0`、`drop_unacked=0`；ACK 由 **QoS1** 发布且 `OK` 表示已收 PUBACK |
| **ACK 云端审计** | **受影响** | 唯一记录 = 回环行 ⇒ 同时承受 (a) 10.8% 漏记 (b) `device_id=http_api` 错主 (c) 无冗余 |
| `mqtt_messages` 作为验收证据 | **受影响** | 漏 10.8% + 伪设备 + 双写 ⇒ **不能单独作为"设备是否发出/云端是否收到"的证据**（`G4-R1`/`G4-R5` 结论维持） |

### 3.8 假重复 / 假漏记的可能来源（逐条排除）

| 可能来源 | 是否成立 | 证据 |
|---|---|---|
| 查询时间窗口 | ❌ | 窗口覆盖 16:32:26–16:42:25，末条 up 在 16:41:02，早于窗口末 |
| 分页 / `LIMIT` | ❌ | 计数用 `COUNT(*)`，无分页 |
| 时间戳精度 | ❌ | `created_at` 为**毫秒**（`mqtt_messages`），重复两行相差 1 530 ms，**可分辨** |
| `msg_id` 复用 | ❌ | 105 个 `g4L` ID 互不相同；且设备侧 30 s 内同 ID 会被丢 |
| **按 `device_id` 过滤** | ✅ **是"假零重复/假少行"的来源** | 见 §3.6-1：105 条回环行不在 `288485896ce4` 名下 |
| NULL `msg_id` 兜底 | ❌ | 窗口内 `msg_id IS NULL` = **0 行** |
| **被 retention 清理掉** | ❌ | `src/retention.js:58` `RETENTION_TABLES = ["telemetry_history","log_history"]`；全文件 `mqtt_messages` 命中 **0** 次 ⇒ 该表**无任何清理逻辑**，不存在"记录过又被删" |

> ⚠️ **对历史记录的更正说明**（不修改历史文件，只在此登记）：
> `G4-R5` 早期表述"下行 105 个 ID / 210 行、result 96 个 ID / 185 行"中的 **210 行成立**（未按 `device_id` 过滤时）；
> "185 行"对应的过滤条件当时未记录，**本次按可复现口径重测为 `up` 全程 281 行 / 97 个 ID**。
> 两者**不矛盾**，属**同源不同切片**；今后引用必须写明 `WHERE` 子句。

---

## 4. 根因分类

### 4.1 已证实（直接证据，可复现）

| # | 结论 | 证据 |
|---|---|---|
| **V-1** | `mqtt_messages` **无任何幂等约束**（仅自增主键） | `sqlite_master.sql` 实测 |
| **V-2** | 每条下行命令被**两个独立写入口**各写一行（审计 + 回环），**100% 复现** | 105/105；`topic` 前缀 `plain` vs `$tenants/`；`raw_payload` 逐字节相同 |
| **V-3** | `/down` 的 `device_id` **解析失败并回落 `clientid`** ⇒ 回环行归属 `http_api` | `deviceIdFromTopic` 两处早退（3 段限制 + 不接受 `/down`）；1 463 行实证 |
| **V-4** | 设备侧**全部发出且 Broker 确认** | 串口 `[Cloud UP] OK=347 / FAIL=0`；`cloud_send_up` 用 QoS1 |
| **V-5** | Worker 在窗口内**零 5xx / 零 error** | GraphQL 845 次调用 `status=success` |
| **V-6** | 突发期间 `/log` 通道**零丢失** | `log_history` boot 22 `seq 5377–5522` 连续 |
| **V-7** | 上行共丢 **34/315 = 10.8%**，按秒聚簇 | 逐 ID 对账表（§3.3） |
| **V-8** | 存在 **1 例**动作重投型重复（1.53 s 间隔、载荷相同） | `g4L30939127` |
| **V-9** | `device_id` 未校验，伪设备行静默入库 | 表定义 `TEXT NOT NULL` + 700+ 非设备行 |

### 4.2 强推断（证据指向明确，但缺一环直接证据）

| # | 推断 | 依据 | 缺的那一环 |
|---|---|---|---|
| **I-1** | 漏记**发生在 EMQX 规则 B 的动作层**（重试耗尽 / `request_ttl` 超期 / 队列丢弃），而非 Pages、Worker、D1 | V-4（源端发出）+ V-5（Worker 无错）+ V-6（同窗口另一条动作零丢失，构成**微分对照**） | **EMQX 动作运行指标**（`failed` / `dropped` / `retried`）——API 403，控制台未取证 |
| **I-2** | 丢失与"后端瞬时拥塞"相关（下行为同批命令时 up 帧延迟 ~30 s） | 突发期 down(16:40:01–16:40:17) 与首批 up(16:40:33) 有 ~16–32 s 落差；BT-1 曾观测"首轮 ACK 迟到 14.2 s" | 动作队列深度/延迟曲线（同上，不可读） |
| **I-3** | `log_ack` 的云端记录**唯一来源**是该回环行 | `log-ack.js` 无 INSERT；`mqtt_messages` 是唯一落点 | —（但"是否曾有过审计行"需查历史提交，本轮未查） |

### 4.3 未确认

- 规则 B 动作的**具体配置**（`max_retries` / `request_ttl` / `pool_size` / `query_mode` / `buffer`）——控制台不可读，**不得猜测**。
- Pages `/api/webhook` 是否参与了丢失（它在路径上，但 Worker 侧 0 错误说明"若 Pages 转发了就一定成功"；反过来，Pages 若失败则 Worker 根本不会收到请求 —— **两种可能无法区分**）。
- 规则 A 与规则 B 的**动作是否共用连接器**（若共用，"一条无损一条丢"就更难解释；若独立，则 I-1 更硬）。
- 丢 34 条的具体秒级窗口内是否有部署/冷启动/限流事件。

---

## 5. 推荐方案、成本、风险、依赖

### 5.1 G4-R4

| 项 | 内容 |
|---|---|
| **推荐** | **方案 ④**：`<70%` 降为**参考指标**；硬门禁改用 `H-1…H-5`（§2.8）多指标 |
| 成本 | 固件：`system.memory` 增 `stack_loop_hwm` + 全局 `alloc_fail_count`（估 **+50 行 / 3 文件**）· **1 次烧录**；文档：门禁条款改写 + 新修订记录 |
| 风险 | ① 若 H-1/H-2 取太紧 ⇒ 未来接 OLED/秤后误报；② `alloc_fail_count` 需覆盖**所有** `heap_caps_malloc`/`new` 调用点，漏一处就等于漏报 |
| 依赖 | 用户批准数值；**与 S-3 的 2.5 h 窗口同批采集**（§2.9） |
| 不做会怎样 | `G-4` 该项永久 `FAIL`，`WP-5` 永久不解锁 —— 但设备本身没有因此更危险 |

### 5.2 G4-R5（**必须拆成三件事，不要打包成一个"修复"**）

| 子项 | 内容 | 成本 | 需设备？ | 需 EMQX 控制台？ |
|---|---|---|---|---|
| **B-1 归属修复** | `deviceIdFromTopic()` 增加 `$tenants/<tenant>/` 归一 + 接受 `/down`；并给 `device_id` 加白名单校验（拒绝 `http_api` 等非 12-hex） | 低（~30 行 + 单测） | 否 | 否 |
| **B-2 幂等去重** | 新 migration `0007`：`ALTER TABLE mqtt_messages ADD COLUMN dedup_key TEXT` + `CREATE UNIQUE INDEX ux_mm_dedup(dedup_key)`（**旧行 NULL 不冲突**）；Worker 计算稳定键后 `INSERT OR IGNORE`；回写 `dedup_key` 需**新增**列（**不改 0006 及更早**） | 中（~120 行 + 测试） | 否 | 否 |
| **B-3 漏记治理** | 读动作指标 → 提高 `max_retries` / `request_ttl` / `buffer`，或把审计改由规则 A 通道承载 | 高（需控制台操作 + 复核） | 否 | **是** |

**B-2 的键设计（需你确认语义，二者互斥）**

| 选项 | 键 | 效果 | 代价 |
|---|---|---|---|
| **K-1「合并双写」** | `device_id?+direction+msg_id+cmd+status+timestamp+payload_sha`（**不含 topic，不含 created_at**） | 下行命令由 **2 行 → 1 行**（保留先到者） | 失去"EMQX 侧确认"行；若先到的是审计行，则**看不出回环是否到达** |
| **K-2「保留双写、只去重投」** | 同上 + **`writer` 判别位**（`audit` / `echo`） | 下行仍 2 行（语义明确），只消除 D-2 类重投 | 表行数仍翻倍；需 `writer` 字段或从 topic 派生 |

> ★ **推荐 K-2**：它满足"不把不同业务消息错误合并"，且不丢失"MQTT 真实收到"的证据；
> 但**是否接受"每条命令 2 行"的存储成本，必须由你裁决**（这是产品/口径选择，不是技术选择）。
> ★ **K-1 的隐藏问题**：B-3 未解决前，回环行有 10.8% 概率缺失；若 K-1 依赖"回环行"作唯一记录，会**放大漏记**。

### 5.3 依赖关系

```
B-1（归属）  ── 独立，可先做；修完才能让 B-2 的键稳定（否则键里的 device_id 是 http_api）
B-2（幂等）  ── 依赖 B-1 的 device_id 正确性
B-3（补漏）  ── 仅依赖 EMQX 权限，与 B-1/B-2 无耦合；但它决定"漏记率"能否下降
G4-R4       ── 依赖 1 次烧录 + 2.5 h 窗口（与 S-3 合并）
```

---

## 6. 需要你决定的问题（按阻塞程度排序）

| # | 问题 | 阻塞什么 | 我的建议 |
|---|---|---|---|
| **Q-1** | `G4-R4` 采用方案 ④？`H-1…H-5` 的**数值**批准与否？ | 阻塞 `G-4` 该项的一切后续（文档 + 代码 + 窗口） | 采纳 ④；H-1 ≥32 KiB / H-2 ≥24 KiB 先按候选走，`H-4`/`H-5` 随下次烧录落地 |
| **Q-2** | `mqtt_messages` 去重取 **K-1（合并双写）** 还是 **K-2（保留双写、只去重投）**？ | 阻塞 B-2 的表结构与键设计 | **K-2**；但若你希望行数减半，选 K-1 并接受"失去回环证据" |
| **Q-3** | 是否授权**读 EMQX 控制台**动作指标/配置（人工截图即可）？ | 阻塞 B-3 的根因定案（否则漏记只能用"外部现象"描述） | 需要，且**只读**：动作的 `failed`/`dropped`/`retried` + `max_retries`/`request_ttl` |
| **Q-4** | 历史"`<70%` 是硬门禁"的表述，是否要在**新修订记录**里更正为"未批准的建议值"？（**不回改历史报告**） | 影响后续所有引用 | 建议在**新记录**中说明，历史文件不动 |
| **Q-5** | 命令下发是否从 **QoS 0** 提升到 **QoS 1**（与 `log_ack` 一致）？ | 与 B-3 同批更合适 | 建议升，但**单独一次变更加一次验证**，不与 B-1/B-2 混提 |
| **Q-6** | `http_api` 等 **1 463 行历史伪设备数据**是否要处理（迁移/标注/不动）？ | 影响历史查询与统计 | 建议**不动历史**，只在 B-1 修完后**新增数据正确**；若需统计，用 `WHERE device_id GLOB '[0-9a-f]*'` 过滤 |

---

## 7. 下一阶段实施任务清单与验收标准

> 以下**全部未执行**。每阶段都写明进入/退出/证据/回退。

### Phase 1 —— 修复设计与自动化测试（**不碰生产**）

| 项 | 内容 |
|---|---|
| **进入条件** | Q-1 / Q-2 已裁决（Q-3 可在 Phase 3 前补） |
| **任务** | ① `B-1` 设计：`deviceIdFromTopic` 归一规则 + 拒绝白名单<br>② `B-2` 设计：`0007` migration 草案（新增列 + 唯一索引 + 回填策略）、`dedup_key` 计算函数、`INSERT OR IGNORE` 语义、冲突可观测性（`changes()==0` 计数）<br>③ `G4-R4` 设计：`system.memory` 增字段 / `alloc_fail_count` 埋点清单（逐个 `heap_caps_malloc` 调用点）<br>④ **幂等性测试用例集**：同一帧投递 1/2/3 次、不同帧同 `msg_id`、`$tenants` 与 plain 双 topic、NULL `msg_id`、并发同键 |
| **退出条件** | 设计文档 + 测试用例清单评审通过（**不写生产代码也能评审**） |
| **证据** | 设计文档（含键推导表、SQL 草案、回填边界） |
| **回退** | 无（纯文档） |

### Phase 2 —— 本地实施与验收

| 项 | 内容 |
|---|---|
| **进入条件** | Phase 1 退出；`wrangler d1 --local --persist-to` 环境可用 |
| **任务** | 实施 B-1 / B-2 + 单元/集成测试；用**真 D1 引擎（local）**跑：重投测试、部分成功测试（模拟 `run()` 抛错）、回填测试（历史 1 895+1 463 行不冲突） |
| **退出条件** | ① `test/current/` 与 `test/real_d1/` 全绿 ② `run_all.mjs` 注册新用例 ③ **`mqtt_messages` 行数幂等断言**：同帧投 N 次 ⇒ 行数增量 ≤ 1 |
| **证据** | 测试输出、`EXPLAIN` 计划（唯一索引命中）、diff |
| **回退** | 丢弃分支，不部署 |
| **必须依赖真实环境** | EMQX 的 `$tenants` 前缀行为、QoS 与 PUBACK —— **本地无法模拟**（用录制的真实报文做 fixture） |

### Phase 3 —— 云端部署（**与烧录解绑**）

| 项 | 内容 |
|---|---|
| **进入条件** | Phase 2 退出；Q-3 已答（若选 B-3）；**单独授权** |
| **最小操作集** | ① `wrangler d1 migrations apply 0007 --remote`（**只新增列 + 索引，不改旧列**）② `wrangler deploy`（版本号记录）③ *可选*：EMQX 控制台调整动作参数（**人工**） |
| **风险** | ① 加唯一索引时若历史数据已有重复键 ⇒ **创建失败**（缓解：`dedup_key` 默认 NULL，旧行全 NULL，SQLite 允许多个 NULL）② `INSERT OR IGNORE` 会把**真正的写入失败**也静默（缓解：显式判断 `changes()==0` 并区分"键冲突" vs "忽略"） |
| **回退** | Worker：`wrangler rollback` 到 `826332ed`；D1：`DROP INDEX ux_mm_dedup`（列可留，NULL 不影响旧逻辑） |
| **禁止** | 与固件烧录捆绑；修改 `0006` 及更早 migration |

### Phase 4 —— 生产验证

| 项 | 内容 |
|---|---|
| **进入条件** | Phase 3 完成；设备在线且 `boot_seq` 已知 |
| **方法** | 受控突发 **35 条只读命令**（复用已验证的 `g4_burst.py`，不引入新变量），随后逐 ID 对账 |
| **通过条件** | ① `device_id='http_api'` **新增行 = 0** ② 重复键冲突 **只应出现在 D-2 类**（≤1/300）③ `down` 审计行 **35/35** ④ `log_history` 仍零缺口 ⑤ 设备 `drop_unacked=0`、无重启 |
| **停止条件** | 出现 `http_api` 新行 / 重复率 > 2% / 设备异常 / `log_history` 出现缺口 ⇒ 立即停并回退 Worker |
| **必须诚实记录的项** | **漏记率不会因 B-1/B-2 改善**；B-3 未做时，`mqtt_messages` 仍**不能**单独作为"云端是否收到"的证据 |

---

## 8. 本次只读操作声明

| 类别 | 是否有变更 |
|---|---|
| 固件源码 / 构建 / 烧录 / 复位 / 串口 / 设备命令 | **无**（全程未与设备交互） |
| 云端源码 / Worker 部署 / Pages 部署 | **无** |
| D1 结构 / 数据 / migration | **无**（**仅 SELECT**；`changed_db=false` 由 `wrangler` 元数据回执） |
| EMQX 规则 / 动作 / 连接器 / 配置 | **无**（未打开控制台；仅查只读 `/clients` 类 API **亦未调用**） |
| 既有文档 / 证据文件 | **无修改**（含 `docs/issues/未修复的问题.md`：本轮**未登记**，登记留待下一阶段授权，以避免修改既有文档） |
| 新增文件 | ① **本报告**（唯一交付物）② `.pio/g45_graphql.mjs`（gitignored 的本地只读查询脚本，仅读取 wrangler OAuth 令牌并发 GraphQL **查询**，**未打印/未落盘令牌**） |
| 门禁状态 | **未变**：`BT-1 CLOSED` · **`G-4 BLOCKED`** · `WP-5` 未解锁 · `STOP-R1` 无解除 |

---

## 9. 未能访问的证据及其影响

| # | 缺失 | 原因 | 对结论的影响 | 替代取证方法 |
|---|---|---|---|---|
| **G1** | EMQX **规则/动作运行指标**（`failed` / `dropped` / `retried` / 队列深度）与动作参数 | 数据集成 API **全 403**；控制台为唯一通道，本轮未授权/未截图 | **I-1 只能是"强推断"**，不能升为"已证实"；无法量化"丢的是重试耗尽还是队列丢弃" | 控制台「动作 → 运行统计/设置」人工截图（只读） |
| **G2** | 规则 A 的 **log 分支完整谓词** | `2026-10-17` 快照截断于第 17 行（`G-1`） | 不影响 `mqtt_messages` 结论（那是规则 B），但影响"为什么 A 无损"的机理解释 | 控制台补一张完整截图 |
| **G3** | 规则 A/B 动作是否**共用连接器** | 同上（`G-3`） | 无法判定"一条无损一条丢"是否因连接器隔离 | 控制台「连接器」页 |
| **G4** | **Pages** 侧调用日志（`/api/webhook` 是否曾收到并转发） | Pages 未开 Workers Logs 持久化 / 无 tail 历史 | **无法排除** Pages 层失败（虽然它与"Worker 0 错误"不矛盾） | 开启 Pages 日志后重跑受控突发 |
| **G5** | 设备侧**逐帧**序号与 D1 的映射（`log_history` 有 `seq`，`/up` 帧只有 `t` 秒级） | 上行帧无单调序号 | 无法把"哪一秒的哪一帧"与设备内部事件精确对齐 | 固件给 `/up` 帧加单调 `n`（属新功能，需授权） |
| **G6** | 本轮**未复查设备**（`boot_seq`、`system.memory` 现状） | 任务书要求"不向设备发送命令" | 本报告引用的内存数值是**上一轮**（`18:38Z`）的快照，**不是当前值** | 下次授权时读一次 `system.memory` |

---

## 10. 一句话交接

`G4-R4` 是**口径问题**（阈值从未获批且不可满足），`G4-R5` 是**三个可分离的工程缺陷**（归属错 / 双写 / 漏记），
其中 **B-1（归属）最便宜且影响最大**、**B-2（幂等）可本地闭环**、**B-3（补漏）必须拿到 EMQX 控制台读数才能定案**。
两者都**不需要动设备**即可推进设计，真正需要设备的只有 `G4-R4` 的取证窗口 —— 而那正好可以与已延期的 2.5 h 窗口合并。
