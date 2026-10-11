# P3 · `WP4-FW-TELEMETRY` —— **实施记录 + 资源门禁判定**

> ## 🟠 `IMPLEMENTED` / **`RESOURCE BLOCKED`（启用）**
>
> | 项 | 值 |
> |---|---|
> | **性质** | **实施记录** —— 记录 Telemetry Publisher 的落地、测试与**资源门禁判定** |
> | **前置基线** | 固件 `81f6754`（`WP4-FW-LOG` 完成）· 文档同步 `e19c33f` |
> | **上游裁决** | **`H-WP4C-02` = `CLOSED`**（新增 Firmware Telemetry Publisher；**不复用 command uplink**）· **`H-WP4-FW-01` = `CLOSED`**（topic / domain 分离优先，Device 侧**不新增 topic**）· **`H-WP4-FW-08` = `CLOSED WITH GUARD`**（资源门禁） |
> | **实现状态** | **代码完成 · 编译期门禁默认 `OFF`**（`GF_TELEMETRY_ENABLED=0`） |
> | **启用状态** | 🔴 **`RESOURCE BLOCKED`** —— 启用增量 **+2,544 B** 远超剩余预算 **636 B** ⇒ **须人工重裁 ceiling**（见 §5） |

---

## §1 数据链路（实现）

```
System State（只读）
      ↓
telemetry_policy.h        ← 纯决策：采样周期 / 变化阈值 / 边沿去抖 / $state 迁移
      ↓
telemetry_envelope.h      ← 组 JSON 信封（有界、无动态分配）
      ↓
cloud_send_telemetry()    ← CloudManager **独立** transport（不经 cloud_compress_uplink）
      ↓
MQTT  guo_feeder/<device_id>/up      ← **topic 零改动**
```

> ★ **不复用 command uplink**：全链路**不**触碰 `cloud_compress_uplink()`，信封**不**含 `c/i/s/r` 短键，
> 也**不**被包成 `{"cmd":"result",…}`（`H-WP4C-02`）。
> ★ `CloudManager` **不解释**载荷（不知道 `type` 语义）—— 只做 transport。

---

## §2 新增 / 修改文件

| 文件 | 性质 | 说明 |
|---|---|---|
| `src/services/telemetry_policy.h` | **新增** | **freestanding** 纯决策（只依赖 `<stdint.h>`）· 无 MQTT / 无 FS / 可 wasm32 真跑 |
| `src/services/telemetry_envelope.h` | **新增** | **freestanding** JSON 组包（有界写出器 + 定点小数；**不用** snprintf / ArduinoJson / String） |
| `src/services/telemetry_publisher.{h,cpp}` | **新增** | glue：读 System State → policy → envelope → transport；**编译期门禁在此** |
| `src/cloud/cloud_manager.{h,cpp}` | 修改 | **纯新增** `cloud_send_telemetry()`；既有 4 个上行 API **签名/语义零改动** |
| `src/main.cpp` | 修改 | `telemetry_publisher_init()`（setup）· `telemetry_task()`（loop，置于 `MiThermometer_task()`/`weight_task()` 之后） |
| `test/telemetry_host/probe_telemetry.cpp` | **新增** | wasm32 真跑探针 |
| `test/telemetry_host/run_telemetry_test.py` | **新增** | 运行器（复用 `test/log_contract/run_wasm_probe.js`） |

---

## §3 数据源（**只用既有 System State，未臆造**）

| 观测 `type`（⚠️ 草案） | 来源 | 触发方式 |
|---|---|---|
| `climate.temperature` | `STATE_MI_THERMO_TEMP` | 变化 ≥ 0.5 °C **或** 5 min 抽样兜底 |
| `climate.humidity` | `STATE_MI_THERMO_HUMID` | 变化 ≥ 2 % **或** 5 min 抽样兜底 |
| `weight.value` | `STATE_WEIGHT_VALUE` | 变化 ≥ 20 g **或** 15 min 抽样兜底（`WP1:§5 H-W1-2`） |
| `weight.error` | `STATE_WEIGHT_ERROR` | 布尔边沿 |
| `valve.state` | `STATE_VALVE_STATUS` | 布尔边沿 |
| `wifi.state` | `STATE_WIFI_STATUS` | 布尔边沿 |
| `online.state` | `STATE_MQTT_STATUS` | `$state` 迁移（`init` / `ready` / `disconnected`） |
| `time.valid` | `STATE_TIME_VALID` | 布尔边沿 |

**明确未实现（禁止臆造）**：`water level`（无独立传感器）· `heartbeat`（`WP1:§5 H-W1-3` 冻结不产生）·
`lost`（broker LWT，**设备侧不可自报告** ⇒ `tp_online_should_emit()` 永不因 `lost` 触发）。

---

## §4 Envelope（⚠️ **PROVISIONAL**）

```json
{ "device_id":"…", "ts":…, "type":"…", "value":…, "source":"…", "seq":…, "schema_version":1 }
```

| 字段 | 说明 |
|---|---|
| `device_id` | 与 `/up` topic 同源（`device_id()`）；ingest 侧仍以 topic 为准 |
| `ts` | `STATE_TIME_VALID ? time_get() : 0`（**不伪造**时间） |
| `type` | ⚠️ **草案词表**（见 §3）—— **`H-WP4-FW-05` = `OPEN`** |
| `value` | 数值为 JSON number（定点 2 位小数、去尾零）；`online.state` 用字符串 `"ready"` 等 |
| `source` | 模块名（`mi_thermo` / `weight` / `valve` / `wifi` / `mqtt` / `time`） |
| `seq` | ⚠️ **进程内单调计数（不跨 boot 持久化）**—— **`H-WP4-FW-07` = `OPEN`** |
| `schema_version` | ⚠️ **草案初值 = 1** —— **`H-WP4-FW-05` = `OPEN`** |

> **⚠️ 本节不是冻结协议。** 三项未裁决项（`H-WP4-FW-05` / `06` / `07`）**全部登记为 `HUMAN_REQUIRED`**，
> 在人工裁决前**不得**被当作云端契约。

### §4.1 配置（`H-WP4-FW-06` = `OPEN`）

- **未**新增 `mqtt.json.telemetry` 键，**未**修改 `config_manager` —— 因为字段集 / 默认值**未冻结**，
  本工作流**不自行创造最终 schema**。
- 当前阈值 / 周期均为 `telemetry_policy.h` 内的**编译期草案常量**（`TP_*`），标注 `PROVISIONAL`。

---

## §5 🔴 资源门禁判定（`H-WP4-FW-08` = `CLOSED WITH GUARD`）

| 构型 | Flash（used） | RAM（used） | Flash 增量（对 `81f6754`） |
|---|---|---|---|
| **`WP4-FW-LOG` 基线**（`81f6754`） | **1,391,829 B** | **132,704 B** | — |
| **telemetry 门禁 OFF**（**默认**） | **1,391,877 B** | **132,704 B** | **+48 B**（仅 main.cpp 两次空实现调用） |
| **telemetry 门禁 ON**（`-DGF_TELEMETRY_ENABLED=1`） | **1,394,373 B** | **132,760 B** | **+2,544 B** |

构建：两者均 `exit=0` · **0 error** · `[SUCCESS]`（门禁 ON 用独立构建目录做 clean build）。

**P3 Flash 预算（`P2-0.2` §17.2.1 冻结）**：

```
ceiling（合计增量上限）        = 14,336 B
已用：P2-7 12,548 + WP4-FW-LOG 1,152  = 13,700 B
⇒ 本任务**开始前**剩余            =    636 B
```

| 判定 | 结果 |
|---|---|
| 门禁 **OFF**（默认构建） | **+48 B ≤ 636 B ⇒ `PASS`**（剩余 588 B） |
| 门禁 **ON**（启用 telemetry） | **+2,544 B > 636 B ⇒ 超预算 1,908 B** ⇒ 🔴 **`RESOURCE BLOCKED`** |

> ### 🔴 结论：**`RESOURCE BLOCKED`（telemetry 启用）**
>
> 依 `H-WP4-FW-08`（"若 Flash 增量超过预算，**必须停止并请求人工裁决**；禁止自行删除功能；禁止修改冻结协议"）：
> **本次不启用**，**保持 `GF_TELEMETRY_ENABLED=0` 默认**，并**请求人工裁决**。
> **未删除任何功能** · **未修改任何冻结协议** · **未降低日志可靠性**。

**可选处置方向（**须人工裁决，不在本工作流内执行**）**：

| 选项 | 说明 |
|---|---|
| **① 重裁 ceiling**（架构级） | `P2-0.2` §17.2.1 属**冻结架构条款** ⇒ 只能人工重裁（如 16 KiB） |
| **② 削减 telemetry 范围** | 例如只保留 climate + weight（去掉边沿类）—— 需人工决定砍哪些 |
| **③ 延期 telemetry** | 保持门禁 OFF，待后续阶段（如与 `WP5` `log_ack` 一并）再评估 |
| **④ 先做跨模块优化** | 参见 `P2-6-Optimization-Feasibility-Audit.md` 的既有优化项（不在本工作流范围） |

---

## §6 测试结果

`python test/telemetry_host/run_telemetry_test.py` ⇒ **`TELEMETRY TEST: ALL PASS`**（41 项断言）

| 要求 | 覆盖 | 结果 |
|---|---|---|
| **Telemetry policy unit test** | `interval`（抽样周期含 uint32 回绕 / period=0）· `threshold`（含 `==` 边界 / 负值）· `edge trigger`（首次 / 间隔内 / 间隔外）· `$state` 迁移（`lost` 永不触发） | **PASS** |
| **Telemetry envelope test** | 字段完整性（7 字段各 1 次）· JSON 结构合法（括号/引号配对 · 无 `nan`/`inf`）· 定点数值（正/负/零/`0.05`/去尾零）· 字符串值模式 · **`seq` 行为**（透传 / 不同 seq ⇒ 不同字节 / `uint32` 上限） | **PASS** |
| **Regression** | 信封**不含** `"c":` / `"cmd"` / `"status"` / `"i":`（⇒ **不是** command uplink 形态）· 含 `ts`/`type`/`value`/`seq`（⇒ ingest 可接受）· `log_contract` 套件仍 **5/5 ALL PASS**（log path 未受影响） | **PASS** |

---

## §7 边界核对（**零违规**）

| 约束 | 结果 |
|---|---|
| 不改 `LogManager` / Log Envelope / log storage / `/log` | ✅ 零改动（`git diff` 仅含本工作流文件） |
| 不改 command uplink（`cloud_send_up` / `cloud_compress_uplink` / `cloud_send_set`） | ✅ 零改动 |
| 不改 topic renderer / topic 模板 | ✅ 零改动 |
| 不新增 topic / 不新增 Cloud protocol version | ✅ topic 仍为 `guo_feeder/<device_id>/up` |
| 不改 `log_events.h` / `log_cbor.h` / `log_ack.h` | ✅ 零改动 |
| 无阻塞（无 `delay()` / `while` 死等）· 无动态分配 | ✅ 每轮最多发 `TP_MAX_EMIT_PER_TASK = 1` 条；信封用栈上 320 B 缓冲 |
| 不产生新 heap 泄漏 | ✅ 未引入任何 `new` / `malloc` / `String`（`cloud_send_telemetry()` 直接用 `cloud_mqtt_publish_binary()` 发原始字节） |

---

## §8 遗留与 `HUMAN_REQUIRED`

| ID | 内容 | 状态 |
|---|---|---|
| **`H-WP4-FW-05`** | `type` 封闭词表 · `value` 编码（number/string/小数位）· `schema_version` 初值 | **`OPEN`** ⇒ 本实现用**草案**并登记 |
| **`H-WP4-FW-06`** | `mqtt.json.telemetry` 字段集与默认值 | **`OPEN`** ⇒ **未**新增配置键 |
| **`H-WP4-FW-07`** | telemetry `seq` 语义（跨 boot 持久化？`ts` 无效时发 `0` 还是不发） | **`OPEN`** ⇒ 本实现用**进程内单调计数**（不持久化），登记待裁 |
| **`H-WP4-FW-08`** | Flash 预算处置 | **`CLOSED WITH GUARD`** ⇒ 本任务触发门禁 ⇒ 🔴 **`RESOURCE BLOCKED`**，**请人工裁决** |
| `H-WP4-FW-09` | 文档同步（`/up` telemetry 载荷形态）· 消费者清单 | 部分（`/log` 已同步；`/up` telemetry **待裁**） |
| `H-WP4C-03` | EMQX Rule scope（telemetry vs 命令结果的**分流机制**） | **`OPEN`** ⇒ `/up` 混流问题**仍未解**（`F-WP4-FW-2`） |
| `H-WP4C-04` / `05` / `06` | 远端 migration 授权 / Action 切换授权 / 部署顺序 | **`OPEN`** |

---

*本文件于 **2026-10-12** 建立，为 `WP4-FW-TELEMETRY` 的**实施记录与资源门禁判定**。
基线 = 固件仓 **`e19c33f`**（文档同步后）。*
