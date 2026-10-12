# P3 · **`Phase C-C1`** —— Telemetry Publisher → MQTT `/up` → EMQX Rule A → `telemetry_history` 首条真实数据（**实施前审查**）

> ## 📋 `REVIEW ONLY` —— **本轮零代码改动 · 零烧录 · 零生产变更**
>
> | 项 | 值 |
> |---|---|
> | **性质** | **实施前审查（Pre-Implementation Review）** —— 判定 Phase C 第一工作包**设计是否完整**、可否开工 |
> | **工作包定义** | 设备侧 telemetry 上行打通：`telemetry_publisher` → `guo_feeder/<device_id>/up` → **EMQX 规则 A** → Pages 中转 → Worker `/api/history/ingest` → D1 **`telemetry_history` 首条真实数据** |
> | **基线** | 固件仓 **`dd78a29`**（tag `wp4c-closure-20261012`）· CF 仓 Worker 生产版 **`99e6d937`** · EMQX 仓 **`3165aab`**（同 tag） |
> | **上游状态** | `WP4C` = ✅ `CLOSED`（`CF-2` `CLOSED` · `V-1…V-7` 全 `PASS`）· `H-WP4-FW-05/06/07/08` = 全 `CLOSED` · `H-WP4C-02/03` = `CLOSED` |
> | **本轮不做** | ❌ 不开启 `telemetry.enable` · ❌ 不烧录固件 · ❌ 不改 EMQX Rule / Action / Connector · ❌ 不改 Worker · ❌ 不改 D1 schema · ❌ 不改 `src/` |
> | **审查对象（只读）** | `src/services/telemetry_publisher.{h,cpp}` · `src/services/telemetry_envelope.h` · `src/services/telemetry_policy.h` · `src/cloud/cloud_manager.{h,cpp}`（`cloud_send_telemetry`）· `src/main.cpp`（setup/loop 接线）· `Cloudflare_Assets/guo-feeder-api/src/history-ingest.js` · `migrations/0003_p3_history_tables.sql` |
> | **判定** | ⚠️ **`CONDITIONAL GO`** —— 设计面完整，但存在 **1 项必须先裁决的阻断项**（`HD-1`：运行时配置门未实现）与 4 项待定/风险项；详见 §10 / §11 |

---

## §1 Objective（目标）

在**不改动任何冻结协议、不改动云端、不新增 EMQX 对象**的前提下，让**真实设备**产生第一条落入 D1 `telemetry_history` 的 telemetry 记录，并形成可复验的证据链。

**为什么这一包先做**：WP4C 已把「路由」打通（`V-1…V-7` 用**合成探针**走通全链），但真实数据侧仍是 0 —— `telemetry_history` 之所以无数据，唯一原因是**设备固件默认不产生 telemetry**（编译门 + 运行时门双关）。打通本包后，WP4C 的「生产观察窗口」才具备可观测量（此前已因设备未上生产而无法得出结论）。

**成功判据（唯一）**：D1 `telemetry_history` 出现**首条真实设备数据**，且 `device_id` = 该设备 topic 第 2 段、`type` ∈ 冻结词表、`value` 与设备实际读数一致；Worker 响应 `accepted ≥ 1`。

---

## §2 Current Baseline（当前基线 · 实测）

### 2.1 固件侧

| 项 | 现状 | 出处 |
|---|---|---|
| 产出器 | **已落地**：`telemetry_publisher.{h,cpp}` + `telemetry_envelope.h` + `telemetry_policy.h`（均为 freestanding 或薄 glue） | `src/services/` |
| 编译期门 | **`GF_TELEMETRY_ENABLED = 0`（默认关闭）** ⇒ 空实现、零 Flash/零 RAM | `telemetry_publisher.h:42-44` · `telemetry_publisher.cpp:22` |
| 运行时门 | ⚠️ **未实现** —— 代码**不读取** `mqtt.json.telemetry.enable`（`src/` 全域无该键读取；`H-WP4-FW-06` 冻结时明示「未改 `src/` / 未改 `ConfigManager`」） | `grep telemetry src/**` · Telemetry-Decisions §H-WP4-FW-06 |
| 接线 | `telemetry_publisher_init()`（setup 第 7 层附加）· `telemetry_task()`（loop，置于 `MiThermometer_task()` / `weight_task()` **之后**） | `main.cpp:607` / `main.cpp:673` |
| 首版范围 | **KEEP = `climate.temperature` / `climate.humidity` / `weight.value`**；DEFER（边沿类 5 条：`online.state` / `time.valid` / `weight.error` / `valve.state` / `wifi.state`）—— **能力保留、首版不产生** | `telemetry_publisher.cpp:176-193` |

### 2.2 云端 / 数据侧

| 项 | 现状 |
|---|---|
| EMQX 规则 A | **`r-n302933b-cf0cf2`**（`FROM "guo_feeder/+/up"` · `WHERE` = `is_not_null` 五键 `∧ is_null(c) ∧ is_null(cmd)`）· 已启用 |
| 动作 HI | **`a-n302933b-126ec6`** → `URL 路径 = /api/history/ingest` + `X-Webhook-Key`（动作层） |
| Worker | 生产版本 **`99e6d937`**；`POST /api/history/ingest` 无 key ⇒ `401`；三态响应 `{accepted, duplicate, rejected}` |
| D1 | `0003` applied；`telemetry_history` 就绪；幂等唯一索引 **`ux_th_dedup = (device_id, type, ts, seq)`** |
| D1 基线（施工前） | `telemetry_history = 0` · `log_history = 0` · `mqtt_messages = 2458` · `device_event(ffffffffffff) = 0` · `last_seen = 1791811429` |
| 配额 | 连接器 **2/2** · 规则 **4/4** · 动作 **4/4** —— **三项全满**（任何新增必须删旧建新） |

### 2.3 既有证据（本包之前）

- **冻结验证集 `V-1…V-7` 全 `PASS`**（probe8/probe9）：其中 `V-2` 已证明「合法 telemetry 信封 ⇒ `telemetry_history +1` / `mqtt_messages +0`」—— **本包只是把信封的来源从合成探针换成真实设备**。
- **五轮 `wrangler tail` 实证**：`/api/history/ingest` 163 B ⇒ **200**。
- ⇒ **本包不存在"未验证的云侧路径"**；风险集中在**设备侧开关与真实读数**。

---

## §3 Scope Boundary（边界）

| | 内容 |
|---|---|
| ✅ **In** | ① 固件编译门启用（`-DGF_TELEMETRY_ENABLED=1`）② （待 `HD-1`）运行时门实现或确认豁免 ③ 烧录与冷启动验证 ④ 设备端配置生命周期 ⑤ MQTT `/up` 信封逐字核对 ⑥ D1 首条真实数据验证 ⑦ 稳定性观察 |
| ❌ **Out** | ① 改 EMQX 规则 / 动作 / 连接器（**配额已满**）② 改 Worker 解析或校验边界 ③ 改 D1 schema / migration ④ `/log` 域路由与其信封（属本包**后续**步骤，另立包）⑤ `type` 词表 / `value` 编码 / 阈值 / `seq` 语义（**均已冻结**）⑥ 云端下发 telemetry 配置（`H-WP4-FW-06` 明示不支持）⑦ 边沿类 5 条 DEFER 项 |

---

## §4 Architecture Review（架构审查）

| # | 检查项 | 结论 | 依据 |
|---|---|---|---|
| A-1 | **是否违反 Firmware 分层规则** | 🟢 **合规** | `services/telemetry_publisher.cpp` → `cloud/cloud_manager.h` 的依赖**有先例**（`src/log/log_manager.cpp:28` 同型：上层模块调用 Cloud 传输层）；产出器本身**不碰 MQTT / LittleFS / 网络堆栈** |
| A-2 | **是否绕过 `CommandManager`** | 🟢 **未绕过（正确）** | 产出器**不** include `command_manager.h`；全链**不**经 `cloud_compress_uplink()`，信封**不**含 `c/i/s/r` 短键、**不**被包成 `{"cmd":"result",…}`（`H-WP4C-02` = `CLOSED`）· 回归测试已断言「信封不含 `"c":` / `"cmd"`」 |
| A-3 | **是否绕过 `EventManager`** | 🟢 **未绕过（设计即轮询）** | 无事件注册；`telemetry_task()` 由 `loop()` 每轮调用（`main.cpp:673`）；放置于 `MiThermometer_task()` / `weight_task()` **之后** ⇒ 本轮状态最新 |
| A-4 | **是否引入 `CloudManager` 耦合** | 🟡 **已裁决的窄耦合** | 新增**单一**接口 `cloud_send_telemetry(const char*)`（`cloud_manager.h:73` / `.cpp:2558`）：载荷**不解释**（视作不透明字节）· **不排队 / 不落盘 / 不重试**（fire-and-forget）· 复用既有 `cloud_mqtt_publish_binary()`，既有 4 个上行 API **签名与语义零改动**。反向耦合不存在（CloudManager 不知道 `type` 语义） |
| A-5 | **是否影响已有 WP4C topic / 路由** | 🟢 **零影响** | Topic 仍取 `mqtt_pub_topic` = **`guo_feeder/<device_id>/up`**（`topic 零改动`，`H-WP4-FW-01`）⇒ **规则 A 的 `FROM "guo_feeder/+/up"` 直接命中**，不需要任何 EMQX 侧改动 |
| A-6 | **非阻塞性** | 🟢 **满足** | 无 `delay()` / 无 `while` 死等 / 无动态分配；每轮最多 1 条（`TP_MAX_EMIT_PER_TASK = 1`）且在成功发送后 **立即 `return`**；MQTT 未连 ⇒ 直接返回并计入 `skipped_offline` |
| A-7 | **状态放置（"禁 static" 铁律）** | 🟢 **不冲突** | `s_ready / s_seq / s_stats` 为**文件内 static**；本模块**非** Action/Trigger 框架（无 `inst->runtime` 语义），且只运行于 `loopTask` 单线程上下文 ⇒ 无跨任务共享；与 `log_manager` 等既有服务同型 |
| A-8 | **只读 System State** | 🟢 **满足** | 仅 `state_get_float()` / `state_is_valid()` / `state_get_bool()`；**无写入**、无新全局裸变量 |

> **小结**：架构面无阻断项；`A-4` 的窄耦合属已裁决设计（`H-WP4C-02`），无需重开。

---

## §5 Protocol Review（协议审查）

### 5.1 信封与云端契约对照

| 字段 | 固件实测行为（代码） | Cloud 侧要求 | 判定 |
|---|---|---|---|
| **`schema_version`** | 恒 `TELEM_SCHEMA_VERSION = 1` | 缺省 1；`>1` 原样保存（前向兼容）；**非正整数 ⇒ 记录 rejected** | ✅ 匹配 |
| **`ts`** | `STATE_TIME_VALID ? (uint32_t)time_get() : 0`（**不伪造时间**） | `asInt` 接受整数 ⇒ `0` **合法**；Rule A 只判「键存在」 | ✅ 匹配（⚠️ 见 `P-4`） |
| **`type`** | 首版三条：`climate.temperature` / `climate.humidity` / `weight.value` | 开放词汇（**不按 type 过滤**）；冻结词表 T1 点分命名空间 | ✅ 与冻结词表**逐字一致** |
| **`value`** | 三态：number → JSON number（定点 **2 位小数、去尾零**）· string → JSON string · boolean → `true/false`（**禁** `0/1`） | 物理列 `TEXT NOT NULL`；Worker 接受 `string` 或有限 `number` ⇒ `String(v)`；**`null` ⇒ 记录 rejected** | ✅ 匹配（⚠️ 见 `P-3`） |
| **`seq`** | `++s_seq`（boot lifetime 单调，**不持久化**） | 必需整数；与 `(device_id,type,ts,seq)` 组成幂等键 | ✅ 匹配（⚠️ 见 `P-4`） |
| `device_id` | **不出现在信封**（身份由 topic 第 2 段承载） | telemetry 域**不要求** `device_id`；身份提取**唯一来源 = topic 第 2 段** | ✅ 匹配（仅 log 域做一致性校验，本包不涉及） |
| `source` | **不发** | 开放词汇；缺省 ⇒ `NULL` | ✅ 匹配（列可空） |

### 5.2 入站形状链路（端到端逐段核对）

```
设备  MQTT publish  topic = guo_feeder/<device_id>/up      payload = 5 字段信封（无 c/cmd）
  ↓
规则 A r-n302933b-cf0cf2   FROM "guo_feeder/+/up"  ∧  is_not_null(ts,type,value,seq,schema_version) ∧ is_null(c) ∧ is_null(cmd)
  ↓  动作 a-n302933b-126ec6  POST /api/history/ingest  + X-Webhook-Key（动作层）
     body = {"topic":"${topic}", "clientid":"${clientid}", "payload": ${payload}}   ← ${payload} 不加引号
  ↓
Worker  /api/history/ingest
     domainFromTopic(body.topic) → parts.length == 3 ∧ parts[0] == "guo_feeder" ∧ parts[2] == "up" ⇒ "telemetry"
     extractRecords("telemetry", body) → body.payload 为对象 ⇒ [payload]（N=1）
     normalizeTelemetry → (device_id, ts, type, value, source=NULL, seq, schema_version)
  ↓
D1  INSERT OR IGNORE INTO telemetry_history(device_id, ts, type, value, source, seq, schema_version)
     幂等键 ux_th_dedup = (device_id, type, ts, seq)；created_at 由 DEFAULT 生成
```

| # | 检查项 | 判定 |
|---|---|---|
| P-5.1 | **topic 形态** | ✅ 设备侧 topic 零改动，天然满足 `guo_feeder/<device_id>/<up>` 三段式；`device_id` 需为 12 位小写 hex（`V-5` 已反向实证：11 位会被 `invalid_device_id` 拒） |
| P-5.2 | **信封经动作体转发后仍为对象** | ✅ `payload` 字段直接内联（无引号包裹）⇒ Worker `request.json()` 得到嵌套对象；`V-1…V-7` 已实证 |
| P-5.3 | **逐记录失败不影响整批** | ✅ Worker 记录级 rejected 不改变 HTTP 状态（仍 200）⇒ ⚠️ **静默丢样的观测盲区**（见 `P-3`） |
| P-5.4 | **幂等可观测** | ✅ 响应三态 `{accepted, duplicate, rejected}` ⇒ 可据此判 `P-4` 碰撞 |
| P-5.5 | **重复投递** | ✅ 幂等键吸收；`V-7` 已实证「同 tuple 发两次 ⇒ 恰 1 行」 |
| P-5.6 | **`value` 非有限值（NaN / ≥100000）** | ⚠️ `te_fixed2()` 退化为 JSON **`null`** ⇒ **Worker 记录级 rejected**（HTTP 200、`rejected+1`、**不落库**）⇒ 见 `P-3` |
| P-5.7 | **`ts = 0` + 重启后 `seq` 归零** | ⚠️ 幂等键可碰撞（同 `type`、同 `ts=0`、同小 `seq`）⇒ `INSERT OR IGNORE` **静默吸收**，表现为「发了两条只落一条」⇒ 见 `P-4` |

> **小结**：协议面**无形态不匹配**。两处风险（`P-3` / `P-4`）均为**边界场景**，不阻断开工，但必须在部署前**显式裁决**（`HD-3`）或纳入观察判据。

---

## §6 Resource Review（资源审查）

| # | 维度 | 实测 / 评估 | 对照 `H-WP4-FW-08` |
|---|---|---|---|
| R-1 | **Flash 增量（门禁 ON）** | **+2,544 B**（基线 `81f6754` = 1,391,829 B → **1,394,373 B**） | 🟢 **不受增量 ceiling 约束** —— `H-WP4-FW-08` 覆盖裁决已把 `telemetry v1` 从「P3 合计增量 ceiling（剩余 636 B）」**豁免**，改为**整体固件安全余量检查** |
| R-2 | **Flash 总占用率（G-1）** | 1,394,373 / 2,097,152 ≈ **66.5 %** | 🟢 阈值 = **≥85 % 停止**；余量充足 |
| R-3 | **RAM 增量（G-2）** | **+56 B**（132,704 → **132,760 B** ≈ **40.5 %** / 327,680） | 🟢 既有 DRAM / 栈检查**不放宽**，实测远低于阈值 |
| R-4 | **loopTask 影响** | 每轮 1 次函数调用；命中时 **≤1 次** publish；信封缓冲 **`TELEM_ENV_MAX_BYTES = 192 B`** 位于**栈**上 | 🟢 相对 loopTask 16 KB / 约 6.5 KB 余量可忽略。⚠️ `P-5`：实施记录 §7 记为「320 B 缓冲」，与代码 **192 B** 不一致（文档漂移） |
| R-5 | **队列 / 存储影响** | **无队列、不落盘、不重试**（fire-and-forget）；离线期间样本**丢弃**（仅累加 `skipped_offline`） | 🟢 无内存 / 无 LittleFS / 无堆积风险。⚠️ 语义提示：**无断点续传**（v1 冻结如此；`log` 域另有 Flash 队列，二者独立） |
| R-6 | **诊断可观测性** | `TelemetryStats{emitted, transport_fail, build_fail, skipped_offline}` 只读快照 | 🟢 具备最小可观测面（⚠️ 当前无串口 / 云侧暴露出口，属实施细节） |
| R-7 | **构建验证** | 门禁 ON / OFF 均 `exit=0` · 0 error · `[SUCCESS]`（ON 用独立构建目录 clean build） | 🟢 可复现 |

> **小结**：资源面无阻断项；**启用前须重跑一次 clean build 并实测 Flash/RAM**（`V-C1-10`），与上表数值对照（偏离需记录）。

---

## §7 Deployment Plan（首次启用步骤 · 待 `HD-1` 裁决后执行）

> ⚠️ **本计划本轮不执行**。所有步骤的**执行前置** = §10 `HD-1` 裁决通过 + 人工批准批次。

| 步 | 动作 | 关键点 |
|---|---|---|
| **1** | **只读预备** | 确认固件 HEAD = `dd78a29`（或经批准的新文档提交）· 记 D1 基线（`th=0` / `mm=2458`）· MQTT 订阅端就绪 |
| **2** | **编译** | `-DGF_TELEMETRY_ENABLED=1`；**独立构建目录 clean build**；记录 Flash/RAM 实测值（对照 1,394,373 / 132,760） |
| **3** | **烧录** | 按技能 `pio-esp32-windows-workflow`：**禁 `uploadfs`** · 注意增量编译 safe-delete / COM 口占用 / esptool 软复位等已知坑 |
| **4** | **冷启动观测（★ 关键）** | 串口确认 `telemetry_publisher_init()` 只记基线**不发**；确认 MQTT 连上后 `telemetry_task()` 的**实际行为** —— ⚠️ **当前无运行时门 ⇒ 一旦连接即开始上行**；若 `HD-1` 裁决为"先实现运行时门"，则本步改为**先验证门为 off 时零上行** |
| **5** | **运行时配置（视 `HD-1`）** | `config set` → `config save` → 自动重启（10 s）→ `config_query` 复查；⚠️ 「set+save 两步」，漏 `save` 也会 5 min 后自动 save + 重启；`valid` 是 RAM 标志，重启后 `get` 返 error 5 |
| **6** | **MQTT 订阅验证** | 订阅 `guo_feeder/<device_id>/up`，逐字核对 5 字段 + `value` 三态；留存 3–5 条样本 |
| **7** | **云端验证** | `wrangler tail`（**走代理 `127.0.0.1:7890`**）观察 `/api/history/ingest` ⇒ **200**；D1 查询 `telemetry_history` 首行 |
| **8** | **稳定性观察** | 连续 ≥30 min：`seq` 单调 · 无 watchdog / 重启 · 无 `resource down` 告警 · `accepted/duplicate/rejected` 比例合理 |

---

## §8 Verification Matrix（验证矩阵）

| ID | 检查项 | 期望 | 方法 | 判据 |
|---|---|---|---|---|
| `V-C1-1` | 规则 A 命中 | 触发计数 **+1** | EMQX 控制台规则运行统计（或 tail） | 计数增长 |
| `V-C1-2` | 信封形态 | **5 字段**且顺序/无多余键 | MQTT 订阅原文 | 逐字比对 |
| `V-C1-3` | `value` 三态编码 | number = JSON number（2 位小数去尾零）· 无 `0/1` 编码 bool | MQTT 原文 | 逐字比对 |
| `V-C1-4` | Worker 接收 | `200` + `accepted ≥ 1` | `wrangler tail`（代理 7890） | 状态码 + 计数 |
| `V-C1-5` | D1 落库 | `telemetry_history` 首行，列对齐（`device_id/ts/type/value/seq/schema_version`） | D1 只读查询 | 行存在且字段一致 |
| `V-C1-6` | 幂等 | 同 tuple 重发 ⇒ `duplicate ≥ 1` 且**行数不变** | tail + D1 | 行数不增 |
| `V-C1-7` | `seq` 单调 | 单调递增、无跳变（重启归零属预期） | MQTT 样本序列 | 单调 |
| `V-C1-8` | 反例（命令回包） | `telemetry_history +0` · `mqtt_messages +1` | 下发一条命令 + D1 | 分流正确 |
| `V-C1-9` | 设备稳定性 | ≥30 min 无重启 / 无 watchdog | 串口 + `device_event` | 无异常事件 |
| `V-C1-10` | 资源实测 | Flash/RAM 与 §6 对照（偏离 ≤ 可解释范围） | 构建输出 | 数值记录在案 |

---

## §9 STOP Conditions（必须停止的条件）

> 触发**任一**条件 ⇒ **立即停止并向人工报告**；**禁止**为绕过而改 EMQX / Worker / D1 / 冻结协议。

| # | 条件 |
|---|---|
| S-1 | **topic 不符合 v1 形态**（≠ `guo_feeder/<12hex>/up`）⇒ 停 |
| S-2 | **规则 A 未命中**（运行统计不动 / 无 `/api/history/ingest` 请求）⇒ 停 |
| S-3 | **Worker 返回异常**：非 `200`，或 `unknown_topic` / `invalid_device_id` / `unauthenticated` / `persist_failed` ⇒ 停 |
| S-4 | **D1 写入失败或 schema 不匹配**（列名 / NOT NULL / 类型冲突）⇒ 停 |
| S-5 | **`value` 记录级 rejected**（`P-3`：非有限值 ⇒ `null`）出现 ⇒ 停并评估加"非有限值不发"护栏 |
| S-6 | **`duplicate` 异常增长**（`P-4`：`ts=0` + `seq` 归零 ⇒ 幂等碰撞）⇒ 停并裁决 `HD-3` |
| S-7 | **`seq` 非单调**或明显跳变（超出重启归零解释）⇒ 停 |
| S-8 | **设备稳定性异常**：重启 / watchdog / 掉线循环 ⇒ 停并回退镜像 |
| S-9 | **资源超阈值**：Flash ≥85 %（G-1）或 RAM 检查不过（G-2）⇒ 停 |
| S-10 | 任何**需要改动** EMQX 对象 / Worker / D1 / 冻结协议**才能继续**的情形 ⇒ 停（**超出本包边界**） |

---

## §10 Open Human Decisions（待人工裁决 · 开工前必须闭环）

| ID | 议题 | 背景 | 建议 / 影响 |
|---|---|---|---|
| **`HD-1`** 🔴 **阻断级** | **运行时配置门（`mqtt.json.telemetry.enable`）是否要在本包实现？** | `H-WP4-FW-06` 冻结了**配置模型**并声明「`enable=false` ⇒ `telemetry_task()` 直接返回」；但**当前代码不读该键**（冻结时明确「未改 `src/` / 未改 `ConfigManager`」）⇒ **两道门目前只有一道**。若直接烧门禁 ON 的镜像，**一上电且 MQTT 连通即开始上行，且无法在不重烧的前提下关闭** | **选项 A（推荐）**：本包内**先实现运行时门**（读配置 + 缺省 false），再启用 ⇒ 与冻结行为一致、具备"软开关"与快速回退；代价 = 少量代码 + 一次重新冻结/评审<br>**选项 B**：接受"仅编译期门"（烧录即生效，回退 = 重烧关闭版镜像）；须**显式记录**与 `H-WP4-FW-06` 运行行为的偏差 |
| **`HD-2`** 🟠 | **`interval_ms`（单一整数）如何映射到两条周期常量？**（`TP_CLIMATE_PERIOD_MS = 5 min` / `TP_WEIGHT_PERIOD_MS = 15 min`） | `H-WP4-FW-06` 明示：映射方式「由**实施任务**确定」，不改变字段集与默认值 | 选项：统一取代（都用 `interval_ms`）/ 派生（weight = 3×）/ 分层（climate 用 `interval_ms`，weight 固定 15 min）——**须在实现运行时门时一并定稿** |
| **`HD-3`** 🟠 | **`ts` 无效（未同步）时：仍发 `ts = 0`，还是暂缓发送？** | 现状 = 发 `ts=0`（"不伪造时间"）；与 `seq` 重启归零组合会产生幂等碰撞面（`P-4`）。另：`H-WP4-FW-06` 已显式声明该策略**不属配置字段**、按实施细节跟踪（残留 `R-FRZ-2`） | 选项：维持现状 + 监控 `duplicate` / 或"未同步不发"（更保守，但会丢失启动初期样本） |
| **`HD-4`** 🟡 | **首版 KEEP 集是否维持 temperature / humidity / weight？** | DEFER 的 5 条边沿类**能力保留、首版不产生** | 建议维持（与 `WP4-FW-TELEMETRY` 裁决一致）；如需启用边沿类，须单独裁决（会改变上行量与观察判据） |
| **`HD-5`** 🟡 | **生产观察窗口的判据与时长** | 观察窗口已与 WP4C tag 归档**解耦**（tag 已于 `dd78a29` / `3165aab` 打出）；窗口起点 = **本包部署** | 建议判据：规则失败率与 presence 同量级 · 无 `resource down` 反复告警 · `telemetry_history` 稳定增长 · `duplicate/rejected` 比例可解释 |

---

## §11 Findings Register（审查发现）

| ID | 级别 | 发现 | 影响 | 处置建议 |
|---|---|---|---|---|
| **`P-1`** | 🔴 高 | **运行时配置门未实现**：`telemetry.enable` 在 `src/` 中**无任何读取**；代码只有编译期门 | 与 `H-WP4-FW-06` 冻结的**运行行为**不一致；启用后**无软开关**，回退只能重烧 | ⇒ `HD-1`（**开工前必须裁决**） |
| **`P-2`** | 🟠 中 | **`interval_ms` 与两条周期常量的映射未定**（冻结文档明确留给实施任务） | 实现运行时门时无法一次落地 | ⇒ `HD-2` |
| **`P-3`** | 🟠 中 | **非有限值退化**：`te_fixed2()` 对 NaN / ≥100000 输出 JSON **`null`** ⇒ Worker `normalizeTelemetry` **记录级 rejected**（HTTP 仍 200）⇒ **静默丢样**，设备侧 `emitted` 仍计数为成功 | 观测盲区：看似"发出成功"，云端实际未落库 | 建议固件侧增加"**非有限值不发**"护栏（并计入 `build_fail` 或专用计数）；或纳入 `HD-3` 一并裁决 |
| **`P-4`** | 🟠 中 | **`ts=0` + 重启后 `seq` 归零 ⇒ 幂等键碰撞面**（`ux_th_dedup = (device_id,type,ts,seq)`）⇒ `INSERT OR IGNORE` **静默吸收** | 时间未同步场景下重启后可能出现"发了不落库" | ⇒ `HD-3`；部署后**监控 `duplicate` 计数**（`S-6`） |
| **`P-5`** | 🟡 低 | **文档漂移**：`P3-WP4-Firmware-Telemetry-Implementation.md` §4 仍描述**7 字段信封（含 `device_id`/`source`）**、§6 断言"7 字段各 1 次"、§7 记"栈上 320 B 缓冲"（代码为 **192 B** = `TELEM_ENV_MAX_BYTES`）；§8 仍列 `H-WP4-FW-05/06/07 = OPEN`（实际均已 `CLOSED`） | 实施者可能按旧形态核对，产生误判 | 建议**随本包做一次 docs-only 同步**（不改代码）：标注该文档为"实施时点快照（`GF_TELEMETRY_ENABLED=0`）"，并指向 `P3-WP4-Firmware-Telemetry-Decisions.md` 为权威 |
| **`P-6`** | 🟡 低 | **代码头注释残留**：`telemetry_policy.h:12-18` 仍标 `PROVISIONAL（草案，未冻结）` 并列出 `H-WP4-FW-05/06/07 = OPEN`；`telemetry_publisher.h:23` 仍写 `H-WP4-FW-06 OPEN` | 与裁决记录（三项已 `CLOSED`）矛盾，易误读为"协议未定稿" | 建议 docs/code-comment 同步时一并更正（**仅注释**，语义不变） |
| **`P-7`** | 🟢 提示 | 信封**不含 `source`**（`v1` 冻结即如此）⇒ 云端 `source` 落 `NULL` | 丧失"模块级溯源"维度 | 无需处理（v1 明确不做）；若未来需要，属协议扩展 |

---

## §12 结论与开工条件

| 项 | 结论 |
|---|---|
| **架构** | 🟢 无阻断 —— 分层合规、不经 `CommandManager`/`EventManager`、topic 零改动、非阻塞、无动态分配 |
| **协议** | 🟢 形态全匹配（5 字段 / 三态编码 / topic 三段式 / 幂等键 / 三态响应）；⚠️ 两处边界场景 `P-3` / `P-4` 需裁决或监控 |
| **资源** | 🟢 无阻断 —— 依 `H-WP4-FW-08` 覆盖裁决，`telemetry v1` 不受增量 ceiling 约束；Flash ≈66.5 %（G-1 阈值 85 %）· RAM +56 B（G-2 不放宽）· loopTask 影响可忽略 |
| **总体判定** | ⚠️ **`CONDITIONAL GO`** —— 设计面完整、云端链路已被 `V-1…V-7` 实证；**但 `HD-1`（运行时配置门）为开工前必须闭环的阻断项**，`HD-2`/`HD-3` 建议同批裁决 |
| **开工前置** | ① `HD-1` 裁决（选项 A / B）② `HD-2` / `HD-3` 裁决 ③ 本审查经人工批准 ④ 另行授权烧录批次（本包**不含**任何生产变更） |
| **本轮边界** | 仅 `docs/`；**零代码 · 零烧录 · 零 EMQX · 零 Worker · 零 D1** |

---

*本文件于 **`2026-10-13`** 建立，为 **`Phase C-C1`** 的**实施前审查**（`REVIEW ONLY`）。
基线 = 固件仓 **`dd78a29`**（tag `wp4c-closure-20261012`）· EMQX 仓 **`3165aab`**。
所有 `file:line` 与资源数字取自基线实测/既有实施记录，可逐条回查；
**本文件不含任何代码改动、配置变更或生产操作**。*
