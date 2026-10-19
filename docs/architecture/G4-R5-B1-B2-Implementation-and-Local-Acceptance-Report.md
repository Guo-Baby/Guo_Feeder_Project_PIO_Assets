# G4-R5 / G4-R4 · Phase 1–2 实施与本地验收报告

> 阶段：`Phase 1`（设计）+ `Phase 2`（本地实施与验收） · 日期：`2026-10-20 03:35–04:2x`
> 授权范围：`G4-R4` 采纳方案 ④ · `G4-R5` **B-1**（归属修复）+ **B-2**（K-2 幂等）
> **不**含：B-3（EMQX 配置）· 任何生产操作 · 远端 migration · Worker/Pages 部署 · 设备操作
> 依据：`docs/architecture/G4-R4-R5-Joint-Investigation-Report.md`（Phase 0 调查）

---

## 1. 结论摘要

| 项 | 结果 |
|---|---|
| **B-1（topic → device_id 归属）** | ✅ **已实施**，单元测试 **52/52 PASS** |
| **B-2（K-2 幂等去重）** | ✅ **已实施**，单元测试 **43/43 PASS** + **真 D1 引擎 18/18 PASS** |
| **G4-R4（门禁修订草案）** | ✅ **已交付草案**（`G4-R4-Gate-Revision-Draft-2026-10-20.md`），**未实施固件代码** |
| 全量回归 | ✅ **24 套件全绿**（含 2 处旧契约标记 `EXPECTED_FAILURE`） |
| 未覆盖场景 | 见 §7 |
| **B-3（漏记）** | ❌ **未解决**，证据缺口见 §8 |
| 门禁状态 | `G-4 BLOCKED` · `WP-5 未解锁` · `BT-1 CLOSED` · `STOP-R1 无解除` —— **全部未动** |

> ★ 本报告**不**声称 B-3 已解决，**不**声称生产链路已通过验收（本地测试 ≠ 生产验收）。

---

## 2. B-1：归属修复

### 2.1 根因（代码级，已证实）

旧 `deviceIdFromTopic()`（`src/device-registry.js`）有两个**叠加**缺陷：

1. **方向白名单只有 `up|log`**，且要求 topic **恰好 3 段** ⇒
   - `/down` 全部解析失败；
   - `$tenants/<t>/guo_feeder/<id>/down`（5 段）也全部失败。
2. 失败后 `index.js` 回落 `body.clientid` —— 而 EMQX 经 **HTTP API** 发布时该值是**内部标识**。

**生产实测污染图谱**（`mqtt_messages` 按 `device_id` 分组，只读查询）：

| `device_id` | 行数 | 性质 |
|---|---|---|
| `288485896ce4` | 1 895 | ✅ 正确 |
| `guo_feeder_001` | 1 660 | 历史伪 ID（topic = `guo_feeder/up`，**2 段无 ID**） |
| **`http_api`** | **1 465** | ❌ EMQX 内部标识；其中 **1 227 行** topic = `$tenants/n302933b/guo_feeder/288485896ce4/down` ⇒ **真实 ID 就在 topic 里** |
| `mqttx_9c9a7813` | 149 | 测试客户端 |
| `dev_288485896ce4` | 77 | 带前缀连接名（topic = `guo_feeder/288485896ce4/up`，本可解析） |
| `cloud_api` | 43 | 旧代码字面量（已删的 `publishToEMQX` 遗留） |

> ⚠️ **对调查报告 §3.6 的更正**（不改写历史文件，仅在此登记）：
> 调查报告中写"log_ack 351 行落 `unknown`"**有误** —— `unknown` 是 `direction` 的字面量，不是 `device_id`。
> 实测 `log_ack` 行归属为 `http_api`（1 204）/ `wb_log_mirror`（88）/ `wb_p15_ackprobe`（17）。
> **结论方向不变**（伪 ID 污染），但具体数值以本报告为准。

### 2.2 实现

| 文件 | 改动 |
|---|---|
| `src/device-registry.js` | 新增 `TOPIC_DIRECTIONS`（`up/down/log`）、`UNRESOLVED_DEVICE_ID`（哨兵 `"unresolved"`）；重写 `deviceIdFromTopic(topic, {directions})`；新增 `resolveIngestDeviceId()` |
| `src/index.js` | `device_id` 改由 `resolveIngestDeviceId()` 求出；解析失败写**显式哨兵**并 `console.warn` 留痕；`handleRegistrySideEffects` 显式传 `{directions:["up","log"]}` **保持原语义** |
| `src/history-ingest.js` | 同样收窄为 `{directions:["up","log"]}`（历史入库只处理 `up`/`log` 域） |

**解析规则**（严格，不做无约束切片）：
1. 按 `/` 切段；首段为 `$tenants` 且总段数 ≥ 3 ⇒ **固定剥 2 段**（`$tenants` + `<tenant>`），余下按 plain 解析；否则失败
2. 剥前缀后必须**恰好 3 段**
3. 第 1 段**逐字等于** `guo_feeder`
4. 第 3 段在 `directions` 白名单内
5. 第 2 段通过 `isValidDeviceId()`（12 位小写 hex）
任一步不满足 ⇒ `null`。

**不再回落伪 ID**：解析失败 ⇒ `unresolved`（显式哨兵，**不可能**通过 `isValidDeviceId()`，因此不会污染退役守卫 / 建档 / 聚合）。
唯一例外：`clientid` **本身**是合法设备 ID 时才采信（保留 P0 回落能力）。

### 2.3 兼容性核实（避免错误拒绝合法业务）

| 场景 | 处理 |
|---|---|
| `/down` 下行（生产 1 227 行） | ✅ **从拒绝变为可解析** —— 本次修复的核心收益 |
| `$tenants/` 副本 | ✅ 可解析，且与 plain **解析结果一致** |
| `guo_feeder/up`（历史 2 段，1 660 行） | ⚠️ 无法解析 ⇒ 改标 `unresolved`。**原值 `guo_feeder_001` 同样是伪 ID** ⇒ 改标后**不会更差**，且更诚实。该 topic 属 P0 遗留，**新固件不再产生** |
| `$tenants/<t>/guo_feeder/down`（缺 ID 段，35 行） | ⚠️ 无法解析 ⇒ `unresolved`（**不猜**） |
| 管理 API（`source==="cloud_api"` 下发分支） | ✅ 不受影响（device_id 从 `body` 取，不走本函数） |
| 建档 / 历史入库 | ✅ 显式收窄方向 ⇒ **行为零改变** |

### 2.4 测试（`test/current/g4r5_b1_topic_device_id.test.mjs` · **52/52 PASS**）

覆盖：常量契约 · plain 三方向 · tenant 三方向 · 非法 ID（大写/11/13 位/非 hex/空/历史伪 ID）· 畸形 topic（null/数字/对象/空串/2 段/4 段/族名错/方向不在白名单/`$tenants` 段数不足/仅 `$tenants`）· `directions` 收窄的 5 种组合 · `resolveIngestDeviceId` 13 例（含 `http_api`/`dev_`/`mqttx_` 不回落）· **5 条真实生产 topic 形态回归**。

---

## 3. B-2：K-2 幂等设计与实现

### 3.1 设计要点

| 维度 | 决策 |
|---|---|
| **来源分离** | `audit`（规则 A，plain topic）/ `echo`（规则 B，`$tenants/` 副本，实测晚 33–37 s）。**`source` 进键** ⇒ 两者**永不合并**（回环证据保留） |
| **键公式** | `key = SHA256( base \u0001 idPart \u0001 phPart )`，其中 `base = source \u0001 deviceId \u0001 direction \u0001 cmd` |
| **身份强度** | A：`msg_id` 非空 ⇒ `idPart="id:"+msgId` **且** `phPart` 也入键<br>B：`msg_id` 空 + payload 非空 ⇒ 仅 `phPart="ph:"+SHA256(payload)`<br>C：两者皆空 ⇒ **返回 `null`** ⇒ 列写 NULL ⇒ **不做幂等** |
| **规范化** | 全部 `String()` 后 `trim()`；null/空串视为缺失；`cmd` 缺失占位 `-` |
| **不用时间** | `created_at` **不入键**（每次投递都不同 ⇒ 用它等于永不幂等） |
| **碰撞风险** | SHA-256 256 bit，10⁷ 条下碰撞概率 ≈ 10⁻⁶³；**不截断**（64 hex 完整入库）。代价：索引 +64 B/行（已知成本） |
| **不误合并优先** | 情况 A 中 `msg_id` 与 payload 哈希**同时**入键 ⇒ "同 `msg_id` 但内容不同"**不合并**（宁可多留一行） |

### 3.2 migration 草案（`0007_mqtt_messages_dedup.sql` · **NOT APPLIED**）

```sql
ALTER TABLE mqtt_messages ADD COLUMN dedup_key TEXT;
CREATE UNIQUE INDEX IF NOT EXISTS ux_mm_dedup_key
    ON mqtt_messages(dedup_key) WHERE dedup_key IS NOT NULL;
```

- **零表重建**、只做加法、**不修改任何已应用 migration**
- **部分索引** ⇒ NULL 互不相等 ⇒ **历史行不受约束、不被合并、不被删除**
- **不回填**历史键（回填需另行设计 + 单独授权）

> ★ **新发现（调查报告未记录）**：`mqtt_messages` 表**没有任何 migration 定义**（P0 时代既有生产表，`migrations/README.md` 写着"完全不动"）。
> ⇒ `0007` 是它的**第一个** migration；⇒ 本地重建库必须先应用 fixture `test/real_d1/fixtures/mqtt_messages.sql`；
> ⇒ **不得**给 `0007` 补 `CREATE TABLE IF NOT EXISTS`（会与生产实际 schema 分叉）。已写入 `migrations/README.md`。

### 3.3 冲突语义（**区分预期冲突与真实失败**）

- 使用 **`ON CONFLICT(dedup_key) WHERE dedup_key IS NOT NULL DO NOTHING`**
  —— **不是** `INSERT OR IGNORE`（后者会吞掉 NOT NULL / CHECK / 类型等**全部**错误）
- `changes=1` ⇒ `inserted`；`changes=0` ⇒ `duplicate`（**预期**）；`changes` 不可得 ⇒ 按 `inserted` 计并 `console.warn`（**不猜**）
- **其它任何错误照常上抛** ⇒ 走 `failed`
- **可审计**：每次写入 `console.log("[mqtt-dedup] outcome=… source=… msg_id=…")`

### 3.4 部署顺序守卫

migration 必须先于（或同批于）Worker 部署，否则列不存在 ⇒ 写入失败。
已内置**降级**：首次遇 `no such column: dedup_key` ⇒ 记住降级态 ⇒ 退回无键写入 + `console.error` **显式告警**
⇒ 后果退化为"失去去重"，**不是**整站 500。（**不得**依赖该降级作为正常路径。）

### 3.5 测试

**单元测试** `test/current/g4r5_b2_dedup_key.test.mjs` · **43/43 PASS**
覆盖：来源分类 6 例 · 键稳定性 3 例 · **来源隔离**（audit≠echo）· 强度分级 A/B/C · **不误合并** 5 例（同 msg_id 不同内容 / 不同 msg_id / 不同 cmd / 不同 device / 不同 direction）· 规范化 4 例 · 写入分支 14 例（含 **真实错误上抛**、**缺列降级**、changes 不可得）· 投递 1/2/3 次（键层面）· audit+echo 共存。

**真实 D1 本地引擎** `test/real_d1/mqtt_dedup_real_d1.sh` · **18/18 PASS**

| # | 断言 | 结果 |
|---|---|---|
| R-00 | 隔离自检：可执行行中远端标志出现 0 次 | ✅ |
| R-01 | fixture 建表成功（生产同构） | ✅ |
| R-1a/b/c | 0007 应用后：列存在 / 索引存在 / **是部分索引** | ✅ |
| R-1d | ★ 应用 0007 **不改变**历史行数（4→4） | ✅ |
| R-1e/f | 历史行 `dedup_key` 全 NULL；**历史重复行保留**（2 行） | ✅ |
| R-2 | 两个 NULL 键行**都**写入（NULL 互不相等） | ✅ |
| R-3 | ★ 同一 audit 帧投递 **3 次 ⇒ 只落 1 行** | ✅ |
| R-4 | ★ 同一业务消息 audit + echo ⇒ **2 行都保留** | ✅ |
| R-5 | ★ 同 `msg_id` 不同内容 ⇒ 2 行都保留 | ✅ |
| R-7/7b | ★ NOT NULL 违反 ⇒ **报错且未落库**（未被吞） | ✅ |
| R-8a/b/c | 回滚：删索引 ⇒ 同键可再插；列保留无害 | ✅ |

---

## 4. G4-R4：门禁修订草案

交付 `docs/architecture/G4-R4-Gate-Revision-Draft-2026-10-20.md`（**草案，未实施固件代码**）：

- `<70%` 标记为**未批准的历史建议值**（`BT-1` 计划 §6.2 原文措辞即"建议阈值"，无批准记录）⇒ `HUMAN_REQUIRED`
- **历史 FAIL 全部保留**，不追溯改写（82.8% / 77.08% / 77.16% / 77.97%）
- `<70%` **降级为参考指标**（informational，不参与判定）的正式修订记录
- **H-1…H-5 明确标注为候选阈值，未批准**
- 6 项观测字段设计（定义/单位/采样时机/生命周期/可观测性/测试方式）
- **分配失败覆盖边界盘点**（见下）

### ★ 分配失败计数的覆盖边界（**不得宣称全覆盖**）

| 类别 | 数量 | 可覆盖？ |
|---|---|---|
| `heap_caps_malloc()` 显式调用 | **22 处 / 6 文件**（多数成对：SPIRAM → `8BIT` 回退） | ✅ **可** |
| `new` 运算符 | **0 处**（精确匹配后确认；此前"9 处"是 `newData` 等变量名误匹配，**已更正**） | — |
| Arduino `String` | **498 处 / 26 文件** | ❌ **不可**（内部走 `malloc`/`realloc`） |
| 第三方库（ArduinoJson / esp-mqtt / **mbedTLS** / BLE / LittleFS / WiFi） | — | ❌ **不可** |
| `std::vector` | 1 处 | ⚠️ 部分（需重载 `operator new`） |

⇒ **"分配失败计数 = 0"只能表述为"本项目显式分配点未失败"**，
**不得**表述为"系统无任何分配失败"，**更不得**推导为"不存在内存风险"。

单次最大内部 RAM 分配 = **16 384 B**（`LOG_CLOUD_QUEUE_SLOTS(128) × LOG_RECORD_SIZE(128)`，`log_manager.cpp:3853`，PSRAM 失败时的 DRAM 回退路径）⇒ H-1（≥32 KiB）的推导依据**成立**。

---

## 5. 文件清单

### 5.1 新增（云端仓）

| 文件 | 说明 |
|---|---|
| `guo-feeder-api/src/mqtt-dedup.js` | **新模块**：来源分类 / 幂等键 / 幂等写入 |
| `guo-feeder-api/migrations/0007_mqtt_messages_dedup.sql` | **新 migration**（`NOT APPLIED`） |
| `guo-feeder-api/test/current/g4r5_b1_topic_device_id.test.mjs` | B-1 测试（52） |
| `guo-feeder-api/test/current/g4r5_b2_dedup_key.test.mjs` | B-2 单测（43） |
| `guo-feeder-api/test/real_d1/mqtt_dedup_real_d1.sh` | B-2 真 D1 验证（18） |

### 5.2 修改（云端仓）

| 文件 | 改动 |
|---|---|
| `guo-feeder-api/src/device-registry.js` | +113：`deviceIdFromTopic` 重写 + 新导出 |
| `guo-feeder-api/src/index.js` | 写入改幂等；device_id 改 `resolveIngestDeviceId`；import |
| `guo-feeder-api/src/history-ingest.js` | 收窄 `directions`（保持原语义） |
| `guo-feeder-api/test/current/redaction.test.mjs` | 旧契约改 `EXPECTED_FAILURE` + 补新契约 |
| `guo-feeder-api/test/current/mqtt_result_frame.test.mjs` | 同上（19 列 → 20 列） |
| `guo-feeder-api/test/run_all.mjs` | 注册 2 个新套件 |
| `guo-feeder-api/migrations/README.md` | 0007 状态 + `mqtt_messages` 无 migration 的事实 |

### 5.3 新增（固件仓，仅文档）

| 文件 | 说明 |
|---|---|
| `docs/architecture/G4-R4-Gate-Revision-Draft-2026-10-20.md` | G4-R4 草案 |
| `docs/architecture/G4-R5-B1-B2-Implementation-and-Local-Acceptance-Report.md` | 本报告 |

---

## 6. 测试与构建结果

| 项 | 结果 |
|---|---|
| `node test/run_all.mjs`（24 套件） | ✅ **全部 exit=0** |
| B-1 套件 | 52/52 PASS |
| B-2 单测 | 43/43 PASS |
| B-2 真 D1 | 18/18 PASS |
| `redaction`（旧契约） | 102 PASS + **1 EXPECTED_FAILURE**（旧语义，已标记未删） |
| `mqtt_result_frame`（旧契约） | 37 PASS + **1 EXPECTED_FAILURE**（19 列，已标记未删） |
| 固件构建 | **未执行**（本阶段无固件改动） |

> 两处 `EXPECTED_FAILURE` 是**契约变更**，旧断言**保留未删未弱化**，并各补了新契约断言。

---

## 7. 未覆盖场景（诚实列出）

| # | 场景 | 原因 |
|---|---|---|
| U-1 | **并发同键写入**（多请求同时到） | 真 D1 本地引擎为单连接顺序执行，**无法**真实并发；D1 本身串行化写，行为预期与顺序一致，但**未实测** |
| U-2 | **D1 写入部分失败 / 重试** | 未模拟 `run()` 部分成功；单测用 fake DB 覆盖了"抛错即上抛"分支，**未**覆盖 batch 部分成功 |
| U-3 | **端到端重复投递**（真 EMQX 重投） | 需生产链路；本阶段禁止触碰 EMQX |
| U-4 | 部署后**去重计数**的实际值 | 需 Phase 4 生产验证 |
| U-5 | `msg_id` 缺失行的**实际去重率** | 取决于真实 payload 分布，本地无法评估 |
| U-6 | G4-R4 的 H-3/H-4/H-5 数据 | 需固件改动 + 烧录 + 长窗口 |

---

## 8. ★ B-3 尚未解决的证据缺口

**B-3（漏记）本阶段未处理，也未被 B-1/B-2 解决。** 明确区分：

- B-1 修的是"**归属错**"（行落到了错误的 device_id）
- B-2 修的是"**重复行**"（同一来源重复投递）
- **B-3 是"该到的没到"**（`up/result` 仅 96/105 个 ID）—— **不同问题，尚未解决**

证据缺口（与调查报告 §9 一致，本阶段**未能**补齐）：

| # | 缺失 | 影响 |
|---|---|---|
| G1 | EMQX 规则/动作运行指标（`failed`/`dropped`/`retried`/队列深度） | 数据集成 API **全 403** ⇒ B-3 只能"强推断"（丢在 Worker 之前），**无法定案** |
| G3 | 规则 A/B 是否共用连接器 | 无法判定"一条无损一条丢"的机理 |
| G4 | Pages `/api/webhook` 调用日志 | **无法排除** Pages 层失败 |
| G5 | 上行帧无单调序号 | 无法把"哪一秒哪一帧"与设备事件精确对齐 |

⇒ **补 B-3 的唯一定案路径 = EMQX 控制台「动作运行统计」人工截图**（只读），需你授权/提供。

---

## 9. 后续授权清单

| # | 事项 | 仓 | 风险 | 回滚 |
|---|---|---|---|---|
| P3-1 | **应用 `0007` 到远端 D1** | 云端 | 中（表已有生产数据；`ADD COLUMN` 为 O(1) 零重建；**索引会在已有行上建** ⇒ 需评估耗时/锁） | 只删索引（列保留无害）；**列不可靠 DROP**（见 migration 文件头） |
| P3-2 | **部署 Worker**（含 B-1/B-2 代码） | 云端 | 中（**必须先于或与 P3-1 同批**） | `wrangler rollback` 到 version `f9647a00`(v46) 或上一 version |
| P3-3 | **生产验证**（部署前后对账：重复率 / 漏记率 / 归属正确率 / 失败重试 / ACK / 健康） | 云端 | 低（只读 + 受控突发） | — |
| P3-4 | **B-3 取证**：EMQX 控制台动作运行统计截图 | — | 只读 | — |
| P3-5 | **G4-R4 固件观测代码**（F-1…F-4）+ 1 次烧录 | 固件 | 中（需重启） | 回烧已归档回退件 |
| P3-6 | **G4-R4 阈值裁决** | — | — | — |

> ⚠️ **P3-1 与 P3-2 的绑定关系**：Worker 代码依赖 `dedup_key` 列。
> 虽然已内置"缺列降级"，但**正常路径**必须是 migration 先/同批。

---

## 10. 变更声明（严格逐项）

| 类别 | 本阶段 |
|---|---|
| 代码（云端 JS） | ✅ **有**（B-1/B-2，见 §5） |
| 代码（固件 C/C++） | ❌ **无** |
| migration 文件 | ✅ **新增 `0007`（草案）**；❌ **未应用**，❌ **未修改任何已应用 migration** |
| 远端 D1 / 生产数据 | ❌ **未触碰**（仅**只读查询**用于取证） |
| Worker / Pages 部署 | ❌ **无** |
| EMQX 规则 / Action / 连接器 / QoS | ❌ **无** |
| 设备：烧录 / 复位 / 下发命令 | ❌ **无** |
| 门禁状态（`G-4` / `WP-5` / `BT-1` / `STOP-R1`） | ❌ **全部未动** |
| 历史验收结论 | ❌ **未改写**（2 处旧契约以 `EXPECTED_FAILURE` 保留并可见） |

本阶段完成后停止，等待人工验收；**不**自动进入 Phase 3。
