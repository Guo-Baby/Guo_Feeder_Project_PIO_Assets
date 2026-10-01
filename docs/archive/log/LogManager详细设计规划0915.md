# LogManager + MQTT Log Topic 详细设计规划报告

> 项目：Guo Feeder Project（ESP32-S3 N16R8）
> 日期：2026-09-15
> 代码基线：git `ce9ba37`（工作区含用户未提交的文档重组，本报告未触碰任何文件）
> 依据：`log模块历史/代码审计需求0914.md`、`log模块历史/LogManager全系统审计报告0914.md`
> **性质：纯设计规划。未修改任何 `.cpp` / `.h` / JSON / 测试 / README。**

---

> # ⚠️ 本报告已被 P1 契约修订（2026-09-15）
>
> 经人工评审，本报告的**总体架构、Queue 方案 A、三 Topic、CloudManager 最小改造、
> 无独立 Task、段式 Flash 环、递归防护**均获通过并保留。
>
> 但以下 **8 项设计结论已被修订**，修订后的权威版本见
> **`log模块历史/LogManager-P1契约冻结0915.md`**：
>
> | # | 项 | 处置 | 本报告对应章节 |
> |---|---|---|---|
> | 1 | 🔴 "INFO 不落 Flash" 的 3 条例外 | **全部撤销**；禁止 EventId 级 `persist` 覆盖 | §11.7（整节作废）、§15.1 |
> | 2 | 🔴 `flags.bit6 uploaded` | **删除**（与"零原地修改"矛盾） | §7.2、§11.4 |
> | 3 | 🔴 `seq` 跨重启重复 | 改为 **Boot 预留区间**（允许空洞） | §7.4、§11.5、§13.2 |
> | 4 | 🟡 `boot_seq` uint16 | 改 **uint32** | §7.2、§7.4 |
> | 5 | 🟡 段容量 31/32 未决 | **冻结 31 条/段**（3984 B） | §11.2、§22.2 |
> | 6 | 🟡 at-least-once 未提升为契约 | 写入正式契约 | §13 |
> | 7 | 🟡 `FORCE_ADVANCE` 混入核心协议 | **降级为可选策略，v1 默认关闭** | §13.2 |
> | 8 | 🟡 CRITICAL "≤16 s" 承诺 | 改为"下一批优先"，**不承诺秒级** | §15.3 |
>
> **本报告中与上述 8 项冲突的描述一律以 P1 契约冻结文档为准。**
> 另：评审确认 **EMQX 尚未配置 `guo_feeder/log`**，
> 故 §21 的 Cloud 组（N1–N8、N10–N11）与 MultiTopic 的 Log 相关项在配置好之前为
> **NOT TESTABLE**；设备侧测试全部可正常执行。

---

---

## 目录

1. [当前系统理解](#1-当前系统理解)
2. [现有 CloudManager / MQTT 架构审计](#2-现有-cloudmanager--mqtt-架构审计)
3. [LogManager 总体架构](#3-logmanager-总体架构)
4. [MQTT 多 Topic 架构](#4-mqtt-多-topic-架构)
5. [Queue 架构比较](#5-queue-架构比较)
6. [推荐 Queue 方案](#6-推荐-queue-方案)
7. [LogRecord 最终建议](#7-logrecord-最终建议)
8. [LogParam 最终建议](#8-logparam-最终建议)
9. [LogEventId 完整分类](#9-logeventid-完整分类)
10. [State Context 设计](#10-state-context-设计)
11. [Flash Ring Buffer](#11-flash-ring-buffer)
12. [Cloud Log Upload](#12-cloud-log-upload)
13. [ACK / Retry / Dedup](#13-ack--retry--dedup)
14. [Offline / Reconnect](#14-offline--reconnect)
15. [Priority](#15-priority)
16. [Task / Thread / Non-blocking](#16-task--thread--non-blocking)
17. [Recursive Logging Protection](#17-recursive-logging-protection)
18. [CloudManager 最小改造](#18-cloudmanager-最小改造)
19. [Config](#19-config)
20. [Module Integration](#20-module-integration)
21. [Test Plan](#21-test-plan)
22. [RAM / Flash / CPU 资源估算](#22-ram--flash--cpu-资源估算)
23. [风险与未决问题](#23-风险与未决问题)
24. [最终推荐架构](#24-最终推荐架构)
25. [后续 Coding Phase 拆分](#25-后续-coding-phase-拆分)

附录 A：参数字典（LogParamId）
附录 B：本报告实测引用清单

---

## 1. 当前系统理解

### 1.1 平台与资源事实

| 项 | 值 | 来源 |
|---|---|---|
| MCU | ESP32-S3，双核，Xtensa LX7 | `platformio.ini` |
| Flash | 16 MB（`board_upload.flash_size = 16MB`） | `platformio.ini` |
| PSRAM | 8 MB OPI（N16R8），实测 `PSRAM size: 8386279` | 启动日志 |
| 分区 | `app0` 0x10000/2 MB，`app1` 0x210000/2 MB，`littlefs` 0x410000/**0xBF0000 = 12.5 MB**，`nvs` 0x5000，`otadata` 0x2000 | `partitions.csv` |
| loopTask 栈 | **16384 B**（`-DARDUINO_LOOP_STACK_SIZE=16384`），实测 HWM ≈ 9876 | `platformio.ini` + 上板日志 |
| DRAM 静态占用 | 约 130 KB / 327 KB | `platformio.ini` 注释 |
| 框架 | Arduino（非 ESP-IDF 原生），无任何 `ESP_LOG*` 调用 | 全项目检索 |

> ⚠️ **本报告新增的风险项**：`platform = espressif32` **未固定版本**，`lib_deps` 四个库也**未固定版本**。Log 涉及 Flash 写入与二进制序列化，构建环境漂移会直接改变 LittleFS 行为。建议在 Coding Phase 前固定（属独立问题，本报告只提出）。

### 1.2 初始化与主循环时序（Log 的挂载点依据）

`setup()` 八层顺序（`src/main.cpp`）：

```
① Serial.begin(115200) + delay(1000)
   LittleFS.begin() ─┬─ json_storage_init()
                     ├─ bin_storage_init()      → bin_storage_set_log_callback()
                     └─ workflow_storage_init()
   system_state_init()
   config_init()                    ← 【version 日志 / Boot Validation 起点】
   event_manager_init()
② ble_init() → wifi_init()
③ workflow_init()
④ weight_init / valve_init / computer_reset_init / dispense_guard_init
   oled_init / oled_event_init / time_init()   ← time_init 必须晚于 oled_init
   test_mqtt_init() / MiThermometerInit()
⑤ command_manager_init() → command_manager_set_log_callback()
   cloud_init()                     ← 内部注册 command_manager_set_result_callback()
⑥ workflow_load_from_storage() / workflow_load_json_file()
⑦ capability_registry_init()
⑧ config_boot_validate()           ← 最后一行；写 .bootok 标记
```

`loop()` 顺序（**Log 任务的插入位置**）：

```
system_command_task()   ← 唯一 ESP.restart() 出口
wifi_task() → event_dispatch() → time_task() → cloud_task()
command_manager_task() → config_task()
workflow_task() → oled_task() → weight_task() → valve_task()
computer_reset_task() → MiThermometer_task()
test_mqtt_task()        ← 测试代码，README §九 要求移入 backup
serial_debug_command_process()
```

**关键结论**：LogManager 必须在 `config_init()` 与 `cloud_init()` **之后**初始化（它要读配置、要有上行通路），`log_task()` 应挂在 `loop()` 中 `config_task()` 附近。

### 1.3 现状：LogManager 完全不存在

| 检索 | 结果 |
|---|---|
| `log_manager` / `log_init` / `log_task` / `log_emit` | **0 处** |
| `ESP_LOG*` 宏 | **0 处**（项目不用 ESP-IDF 日志体系） |
| 已有 `set_log_callback` 的模块 | **5 个**：`bin_storage`、`json_storage`、`file_storage`、`config_manager`、`command_manager` |
| main.cpp 已注册回调用途 | **2 个**：`bin_storage`（:74）、`command_manager`（:114） |
| 需求文档 Log 规划 | `需求文档.md` §5：等级 VERBOSE/DEBUG/INFO/WARN/ERROR、输出串口 + MQTT（后续）、内存 Ring Buffer、Config 控制等级；开发顺序第 5 位 |

**本设计对既有 `set_log_callback` 范式的态度**：**沿用形态，但不作为主通道**。

理由：现有 5 个回调的签名是 `void(const char *level, const char *message)` —— **自由文本**。它无法承载结构化事件与 typed 参数，且日志文案在 5 个模块内各自拼装（`CONFIG_LOG_BUF_SIZE 192` 截断）。若把 LogManager 建成"接这 5 个回调再把文本写 Flash"，就退化成文本日志系统，与本需求"结构化历史事实"的目标冲突。

**正确定位**：这 5 个回调是**过渡期诊断通道**（继续指向 Serial），LogManager 走**新的结构化 API**。详见 §17.3。

---

## 2. 现有 CloudManager / MQTT 架构审计

### 2.1 真实发送路径（逐层实读 `src/cloud_manager.cpp`）

```
业务模块（CommandManager / 其他）
        │  CommandResultCallback（函数指针，cloud_init() 内注册）
        ▼
on_command_result(json)                       cloud_manager.cpp:1164
        │  仅 Serial.printf("[Cloud] UP-RESULT len=...")
        ▼
cloud_send_up(const char* message)            cloud_manager.cpp:1474
        │
        ├─ if (!mqtt_connected) return false   ← 直接丢弃，无重试、无落盘
        ├─ cloud_compress_uplink(text → compact)  :522   【JSON 文本压缩，非 CBOR】
        ▼
cloud_mqtt_publish_text(topic, text, qos)     cloud_manager.cpp:375
        │
        ├─ CLOUD_MQTT_QOS == 1 时调用
        ▼
esp_mqtt_client_enqueue(client, topic, data, len, 1, 0, /*store=*/true)
        ▼
        esp-mqtt 内部 outbox（非本项目代码）
```

### 2.2 审计发现（全部实测，按影响排序）

#### 发现 1【架构级】CloudManager **没有任何应用层 TX 队列**

`cloud_send_up()` 是**同步直发**：压缩 → `esp_mqtt_client_enqueue()` → 返回 bool。全项目检索 `tx_queue` / `send_queue` / `pending_queue` **均为 0 处**；出现的 `outbox` 全部是 `esp_mqtt_client_get_outbox_size()` 的**观测值**，不是本项目的队列。

**这直接决定 §5 的方案选择**：所谓"方案 B：统一 Cloud TX Queue"，在当前代码里**需要从零新建一个队列并让全部业务上行改道** —— 不是"扩展现有队列"，而是**新增一个中间层**。这与需求 §四"最小改动扩展现有 CloudManager"直接冲突。

#### 发现 2【架构级】上行是 **JSON 文本**，不是 CBOR

| 证据 | 位置 |
|---|---|
| `ARDUINOJSON_USE_CBOR=1` 已开启 | `platformio.ini` |
| 但 `cloud_send_up` 走 `cloud_compress_uplink()` → `serializeJson` 风格的**文本压缩** | `cloud_manager.cpp:1488,1494` |
| 且最终调用 `cloud_mqtt_publish_text()` | 同上 |
| `cloud_send_up_cbor` / `cloud_send_set_cbor` / `cloud_send_up_binary` | **头文件声明 3 个，实现 0 个** |

```c
// cloud_manager.h:37-50  ← 声明存在
bool cloud_send_up_cbor(const uint8_t* data, size_t length);
bool cloud_send_set_cbor(const uint8_t* data, size_t length);
bool cloud_send_up_binary(const uint8_t *data, size_t length);
// cloud_manager.cpp     ← 三个函数体均不存在（经 grep 确认）
// 且全项目无任何调用点，所以链接期不报错
```

**结论**：项目**目前没有任何可用的二进制上行通道**。Log 若要走 CBOR，必须补上。这是本设计对 CloudManager 的**最大改造点**。

#### 发现 3【安全】MQTT 调试横幅打印明文密码

`cloud_manager.cpp:1205-1228` 输出 `host / port / client_id / username / **password**`。实测 `data/config/mqtt.json` 中 `"password": "GuoBaby"` 会明文上串口。

**对 Log 的直接影响**：一旦 Log 通道建成，任何"顺手把 Cloud 连接信息记一条"的做法都会把凭据写进 Flash 并上云。必须在 P0 阶段消除。

#### 发现 4【协议债】`cloud_protocol.md` 与代码/配置已不一致

| 项 | `cloud_protocol.md` | 实际代码/配置 |
|---|---|---|
| Broker | 巴法云 | `n302933b.ala.cn-hangzhou.emqxsl.cn`（EMQX Cloud） |
| Topic | `{uid}/up`、`{uid}/down` | `guo_feeder/up`、`guo_feeder/down` |
| 单条上限 | **≤1024 B**（平台限制） | `CLOUD_MSG_LIMIT_MAX = 8128`（`MQTT_BUFFER 8192 - 64`） |
| 顶层字段 | `v/mid/d/t/ts/dat` | **代码用的是 `s/c/i/o/k/p`**（`cloud_compress_uplink` 的映射表） |
| `CloudMessageType` | `cmd/res/ack/reg/frag` | 代码里是 `CLOUD_MSG_COMMAND/RESULT/ACK/REGISTRY/FRAGMENT`，但**压缩键实际是 `c`(cmd) + `i`(id)**，与文档的 `t`+`mid` 不同 |

**结论**：`cloud_protocol.md` 描述的是**另一套未落地的设计**。新增 Log Topic 前必须先决定"以哪一份为准"，否则 Log 的 ACK 消息会建立在错误契约上。**这是本设计最需要人工确认的前置项（见 §23-Q1）。**

#### 发现 5【能力可用】分片机制存在但未接入上行主路径

`cloud_publish_fragmented()` 已实现（BEGIN/DATA/END，含 CRC32），但 `cloud_send_up()` **不调用它**。仅当未来接入 `cloud_send_up_binary` 时才可能启用。

**对 Log 的意义**：Log 批次设计为**不超限**（见 §12），因此**不需要分片**。分片作为安全网保留即可。

#### 发现 6【可复用】`change_msg_limit` 提供"协议级消息旁路 CommandManager"的先例

`cloud_process_rx_message()` (`:1024`) 的分发顺序：

```
① 解析 JSON（DynamicJsonDocument 4096）
② 取 c/cmd、i/id，校验非空
③ 查重（cloud_check_duplicate_cmd，10 条 / 30 s TTL）
④ if (c == "change_msg_limit") → cloud_send_ack() + cloud_handle_change_msg_limit() + return
                                                   ↑ 绕过 CommandManager
⑤ else → cloud_translate_command() → command_manager_execute()
```

**这是 Log ACK 下行处理的现成落点**：在 ④ 旁边加一个 `else if (c == "log_ack")` 分支即可，无需改动 CommandManager、无需新增消息类型枚举。见 §18。

#### 发现 7【观测】现有下行 ACK 是"收到即回"，与业务结果分离

`cloud_send_ack(cmd_id, object)` (`:782`) 在**命令执行之前**就发出（`:1093` → `:1095`），内容固定为 `{"c":"ack","i":<id>,"o":<obj>,"p":{"result":"received"},"t":<unix>}`。也就是说：

- **传输层 ACK**（MQTT PUBACK）= esp-mqtt 内部处理
- **协议层 ACK**（`c:"ack"`）= 表示"命令已收到并受理"
- **结果上报**（`CLOUD_MSG_RESULT` → `cloud_send_up`）= 业务结果

**对 Log 的启示**：Log 已有 3 层语义可用。建议 Log 只新增**一个**应用层 ACK（`c:"log_ack"`），**不要**再叠加 `c:"ack"` 双确认 —— 避免重复设计（AI_RULES §6）。

#### 发现 8【可复用】下行接收已经是"回调只入队、主循环处理"

```
mqtt_event_handler (esp-mqtt 任务)
   └─ MQTT_EVENT_DATA → 长消息重组 → cloud_rx_enqueue()   ← 仅 memcpy 进 4×8192 环形
cloud_task() (loopTask)
   └─ cloud_process_rx_queue() → cloud_process_rx_message()   ← 解析/ACK/执行
```

`cloud_rx_enqueue/dequeue`（`:175`/`:193`）是本项目**已有的环形队列范式**：定长槽、`head/tail/count`、满则丢弃并打日志。**Log 的 RAM 环应沿用同一形态**（不另创风格）。

#### 发现 9【风险】QoS1 + `store=true` 的 outbox 持久化与 Log 的 Flash 策略叠加

`cloud_mqtt_publish_text/binary` 固定使用 `CLOUD_MQTT_STORE true`（`:22`）。`esp_mqtt_client_enqueue(..., store=1)` 会让 esp-mqtt 将未确认的 QoS1 消息存入其内部存储（NVS 方向）。

**对 Log 的影响**：Log 已经有自己的 Flash 环作为**耐久层**。若 Log 批次也走 `store=true`，同一批日志会被**双份持久化**（esp-mqtt 内部 + Log 自己的环），造成不必要的 NVS 擦写。

**建议**：Log 的发布路径使用 `store=0`；断网时**根本不应该调用 publish**（先落自己的环）。见 §18。

#### 发现 10【现状】重复命令缓存只覆盖下行

`MQTT_DUP_CACHE_SIZE 10` / `TTL 30 s`，`cloud_check_duplicate_cmd()` 仅用于**下行命令**去重（`:1061`）。上行去重依赖 QoS1 + 云端。

**对 Log 的影响**：Log 的线上去重必须由**云端**用 `(device_id, boot_seq, seq)` 完成；设备端只保证 **seq 单调且重传时 seq 不变**（幂等重传）。见 §13。

### 2.3 Up / Down 的当前语义（必须保持不变）

| Topic | 配置值 | 变量 | 用途 | 订阅/发布 |
|---|---|---|---|---|
| Down | `guo_feeder/down` | `mqtt_sub_topic` | 云端下发命令 | `esp_mqtt_client_subscribe()` 于 `MQTT_EVENT_CONNECTED` |
| Up | `guo_feeder/up` | `mqtt_pub_topic` | 一切设备上行 | `cloud_send_up()` / `cloud_send_set()` |

**注意一个既有的命名反常**：`cloud_send_set()` 也发到 `mqtt_pub_topic`（`:1465`），不是 `mqtt_sub_topic`。即"set"这个名字与实际去向不符（`cloud_publish_fragmented` 里也用 `mqtt_sub_topic` 作为 `CLOUD_FRAG_SET` 的目标，`:448`）。**本设计不建议顺手改名**（违反 §17"不应该修改"），只在此记录，供后续独立清理。

---

## 3. LogManager 总体架构

### 3.1 定位

**服务层，与 `ConfigManager` / `EventManager` 同级**，排在四层架构的第三层。

```
┌──────────────────────────────────────────────────────┐
│ 应用 / 能力层   Valve · Weight · DispenseGuard ·      │
│                Mijia · OLED · ComputerReset          │
└───────────────────────┬──────────────────────────────┘
                        │ log_emit(结构化事件)
┌───────────────────────┴──────────────────────────────┐
│ 自动化层  WorkflowManager · CapabilityRegistry        │
└───────────────────────┬──────────────────────────────┘
                        │
┌───────────────────────┴──────────────────────────────┐
│ 服务层   SystemState · EventManager · TimeManager     │
│          WiFiModule  · CommandManager                 │
│          ConfigManager   ★ LogManager                 │
│                              │                        │
│                              ├─→ BinStorage → LittleFS│
│                              └─→ upload_cb（注入）     │
└───────────────────────┬──────────────────────────────┘
                        │  函数指针注入（main.cpp 绑定）
┌───────────────────────┴──────────────────────────────┐
│ 云通信层  CloudManager（MQTT / 分片 / ACK / 压缩）      │
└──────────────────────────────────────────────────────┘
```

### 3.2 内部分层（模块内 5 段，职责单一）

```
① log_emit()            对外唯一写入口。级别过滤 → 组装 Record → 入 RAM 环。
                        无 malloc / 无 Flash / 无 MQTT / 无 Serial。
                        ↓
② RAM Ring (定长)       64 槽 × 128 B，PSRAM。head/tail/count（沿用
                        cloud_rx_enqueue 形态）。满 → 丢最低级 + 计数。
                        ↓
③ Policy Router         按 Level 决定去向：
                        · DEBUG → 丢弃（编译期若关则不入队）
                        · INFO  → 只上云
                        · WARN+ → 上云 + 落 Flash
                        ↓
④ Flash Segment Ring    16 段 × 4 KB，/log/。仅 WARN/ERROR/CRITICAL。
                        批提交；CRITICAL 立即提交。
                        ↓
⑤ Uploader (log_task)   在线：组批 → CBOR → 注入的 upload_cb()
                        离线：只落 Flash；重连后 FIFO 补发
                        ACK：由 CloudManager 下行回调推进 tail
```

### 3.3 数据流（三种网络状态）

```
【在线】
  log_emit → RAM 环 → log_task 组批(≤16) → CBOR → upload_cb → Log Topic
                     └(WARN+)→ Flash 段环
  Cloud 回 log_ack → 推进 Flash tail（删已确认段）

【离线】
  log_emit → RAM 环(INFO 在此耗尽，溢出即丢)
                     └(WARN+)→ Flash 段环（唯一耐久路径）

【重连】
  log_task 检测 cloud 在线 → 先按 seq 升序补发 Flash 未确认段
                          → 再发 RAM 中的 INFO
```

### 3.4 明确不做什么

| 不做 | 理由 |
|---|---|
| 取代 SystemState | State = 当前事实；Log = 历史事实。二者语义不同（需求 §23-17） |
| 取代 EventManager | Event 有一半是给 Workflow Trigger 的内部信号，自动转 Log 会污染历史 |
| 取代 WorkflowManager | Log 只观测，不执行 |
| 自己连 MQTT | 需求 §四；且违反 AI_RULES §1 分层 |
| 新增独立 Task | 见 §16.4（LittleFS 单写者约束） |
| 建立 Workflow Cloud State Machine | 需求 §六；Log 同步只用 sync/ACK 计数，不建状态机 |

---

## 4. MQTT 多 Topic 架构

### 4.1 三 Topic 最终形态

```
MQTT
├── down   guo_feeder/down     Cloud → Device    （命令 + log_ack）
├── up     guo_feeder/up       Device → Cloud    （业务上行，语义不变）
└── log    guo_feeder/log      Device → Cloud    （仅 LogManager 批次）
```

| Topic | 语义 | 谁发 | 载荷形态 | QoS |
|---|---|---|---|---|
| `down` | 云端指令 | Cloud | JSON（`c`/`i`/`p`） | 1 |
| `up` | 业务上报 | CloudManager（代各模块） | JSON 文本（现有） | 1 |
| `log` | 历史日志批次 | **仅 LogManager（经 CloudManager）** | **CBOR 二进制** | 1 |

### 4.2 为什么必须是独立 Topic（而不是在 Up 里加 type）

| 理由 | 说明 |
|---|---|
| **防止污染既有 Up 契约** | `cloud_compress_uplink()` 是按字段名做映射的（`status→s`、`command→c`…）。Log 的批次结构完全不同，混入需改映射表 → 违反需求 §十七 |
| **云端消费路径不同** | 业务上行进"命令/状态"处理链路；Log 进"时序库/告警"链路。同 Topic 会迫使云端做二次分流 |
| **流量特征不同** | 业务上行是**偶发突发**（命令结果）；Log 是**可抑制的批量流**。混在一起无法独立限流 |
| **离线补偿不同** | 只有 Log 有"补发历史"需求。混入 Up 会让"补发"变成"重放业务消息"，语义危险 |
| **可独立关闭** | 云端可在不中断业务的前提下关闭 Log 订阅 |

### 4.3 Topic 命名与配置

```json
// data/config/mqtt.json（新增 1 个字段，不改动现有 8 个）
"log_topic": "guo_feeder/log"
```

派生策略建议：**新增独立配置字段**，而非"从 publish_topic 推导"。理由：派生规则（把 `/up` 换成 `/log`）是隐式契约，一旦有人改了 `publish_topic` 就会静默错位。

### 4.4 订阅策略

**日志 Topic 不需要订阅**（设备是发布者）。因此：

- `MQTT_EVENT_CONNECTED` 里**只新增发布能力**，订阅调用维持原样（1 次 `subscribe(down)`）。
- **`log_ack` 由 `down` Topic 承载**，不新增订阅。这是把改动压到最小的关键决定。

### 4.5 ⚠️ 运维前置条件（必须人工确认）

EMQX Cloud 的 ACL（`acl.conf`）通常默认允许 `{username}/{发布者自选}` 形式的主题，但**是否允许 `guo_feeder/log`** 取决于该部署的策略配置。

- 若 ACL 拒绝，发布将返回 `msg_id < 0`（`cloud_mqtt_publish_binary` 会打印 `[Cloud] publish FAIL <topic>`）→ 表现为"Log 永远发不出去，但业务正常"。
- **验证方法（Coding Phase 前）**：用 MQTT.fx / MQTT Explorer 以同样的 `username/password` 登录，向 `guo_feeder/log` 发一条消息，观察是否被拒。
- 备选方案：复用 `guo_feeder/up` 并在载荷内区分（`{"c":"log"...}`），代价是放弃 §4.2 的全部优点。**这是本设计的第一个"必须先确认"项。**

---

## 5. Queue 架构比较

### 5.1 两个方案的真实形态（按当前代码画）

**方案 A：Log 独立队列**

```
LogManager ──log_emit──► RAM 环 ──log_task──► upload_cb ──┐
                                                          │ 新增 1 个 route
业务模块 ──cloud_send_up──► 压缩 ──► esp_mqtt_client_enqueue ──► esp-mqtt outbox ──► MQTT /up
                                                          │
                                        cloud_send_route(CLOUD_ROUTE_LOG, ...) ──► MQTT /log
```

**方案 B：统一 Cloud TX Queue**

```
LogManager ──┐
             ├─► CloudManager 新增 TX Queue ──► Topic Router ──┬─► /up
业务模块 ────┘                                                 └─► /log
```

### 5.2 逐项比较（需求 §五 的 15 个维度）

| # | 维度 | 方案 A（独立队列） | 方案 B（统一 TX 队列） | 胜 |
|---|---|---|---|---|
| 1 | **RAM 占用** | 128 B × 64 = **8 KB**（PSRAM），业务侧 **0** | 共享队列必须按 `max(业务峰值 + Log 峰值)` 定容；且仍需 Log 自己的 Flash 暂存区 → **更大** | A |
| 2 | **Queue 长度** | Log 队列长度只由 Log 速率决定，可静态界定（64 槽足够，见 §22） | 无法静态界定：业务突发（如 UI 连发 20 条 config_set）会与 Log 洪峰叠加 | A |
| 3 | **满时行为** | 丢最低级（先 DEBUG/INFO，最后 WARN）+ `drop_count`；**业务完全不受影响** | 必须引入优先级仲裁；否则要么丢业务、要么丢 CRITICAL。**这是新增子系统**（违反 AI_RULES §6） | A |
| 4 | **Log 是否挤占业务** | **不可能**（两条独立路径） | **可能**：Log 洪峰填满共享队列 → 业务上行被丢 | A |
| 5 | **业务是否阻塞 Log** | **不会**（Log 有自己的环） | **会**：业务突发时 Log 排队 | A |
| 6 | **CRITICAL 优先级** | 无需优先级算法：CRITICAL 只需"提前 flush"（§15.3），队列本身仍 FIFO | 需要优先级队列或旁路插入逻辑 → 复杂度显著上升 | A |
| 7 | **WARN/ERROR 离线行为** | 落 Flash 段环；RAM 环腾空给新日志 → **不丢** | 共享队列的持久化语义不明：业务消息**不应**落盘，Log **必须**落盘 → 需要在同一队列内区分存储策略 | A |
| 8 | **重连后发送顺序** | Log 按 `seq` FIFO 补发；业务上行**完全不受 Log 补发影响**（esp-mqtt outbox 独立） | 单一 FIFO 会把"补发的历史日志"排在"新业务结果"之前 → **业务延迟**，且 Log 补发量大时会长时间压制业务 | **A（决定性）** |
| 9 | **ACK 处理** | 两套语义清晰分离：业务用现有 `PUBACK`+`c:"ack"`+`c:"result"`；Log 用新 `c:"log_ack"`（批次、seq 区间） | 同一队列内混合两类 ACK，需要按消息类型分派 → 等于把 §2.2 发现 6 的旁路逻辑复制一遍 | A |
| 10 | **Retry** | Log 自己的重试计数 + 退避，**与业务重试隔离**，不会把 `mqtt_retry_count` 搅浑 | 混在一起：Log 重试会推高整体的重试计数，干扰 `STATE_MQTT_RETRY_COUNT` 的语义（该值已用于"10 次进 1 小时休眠"，误判会导致设备静默） | **A（安全性）** |
| 11 | **Sequence 设计** | Log 用自己的 `seq`（全局单调，重传不变）→ 天然幂等 | 共享队列的序号对业务无意义，Log 仍需独立 `seq` → **两份序号** | A |
| 12 | **独立 Topic 状态** | 可维护 `log_online` / `log_backlog_depth` / `log_last_ack`，与业务状态解耦 | 单一状态无法区分"业务通畅但 Log 被拒" | A |
| 13 | **代码复杂度** | 新增：1 个 route 枚举 + 1 个二进制发布函数 + 1 个 ack 回调 + 1 个下行分支 ≈ **4 处** | 新增：队列结构 + 优先级 + 双存储策略 + 消息类型分派 + 迁移全部业务上行 ≈ **跨 10+ 处，且改到已验证稳定路径** | A |
| 14 | **对 CloudManager 侵入程度** | **极小**：纯新增（additive）。`cloud_send_up` 等**一行不改** | **大**：`cloud_send_up` / `cloud_send_set` / `on_command_result` / `cloud_send_ack` 全部改为"入队" → 现有 QoS/outbox 观测全部失效 | A |
| 15 | **云端实现复杂度** | Log Topic 独立消费者，独立 schema，独立幂等键 | 云端仍需按 `t`/`c` 分流 → **并不更简单**，且多了一层"设备端已经混过"的耦合 | A |

**比分：A 胜 15 / 15。**

### 5.3 补充：方案 B 唯一"看似"的优势与反驳

| 方案 B 的表面优势 | 反驳 |
|---|---|
| "统一入口便于限流与优先级" | 当前系统**不需要**跨通道限流。业务上行实测是偶发（命令结果），Log 是可抑制的批量流。**两者本就不需要互相仲裁。** |
| "少一个回调、少一处代码" | 恰恰相反：B 需要新增队列 + 分派 + 优先级，代码更多。A 只需 4 处纯新增。 |
| "避免两条路径行为不一致" | Log 与业务的**行为本来就应当不同**（耐久策略、补发、ACK 语义都不同）。统一反而是错误的抽象。 |

---

## 6. 推荐 Queue 方案

### 6.1 结论

> **采用方案 A：Log 独立队列 + CloudManager 仅新增一个上行 route。**

### 6.2 为什么（三句话）

1. **当前 CloudManager 根本没有 TX 队列**，"方案 B"实际是**新建**一个中间层并让全部业务上行改道 —— 这是重构，不是扩展，直接违反"最小侵入"与"不修改已验证稳定路径"。
2. **RAM 的 Log 环本来就必须存在**（离线缓冲 + 批量提交都需要它），所以方案 A 的队列**不是额外开销**，而是把已有设计复用为"队列"。
3. **唯一真正致命的差异是重连补发**：Log 有历史补发需求、业务没有。共用一个 FIFO 会让"补发 500 条历史日志"排在"一条新的命令结果"前面 —— 这是**不可接受的业务延迟**。

### 6.3 实现要点（不改现有函数）

- Log 的 RAM 环 = LogManager 私有，**CloudManager 不感知**。
- CloudManager 只新增：`CloudRoute` 枚举 + `cloud_send_route()` + `cloud_set_log_ack_callback()` + `down` 分发里一个 `log_ack` 分支。
- **不改** `cloud_send_up` / `cloud_send_set` / `cloud_upload_json` / `cloud_mqtt_publish_*` 的签名与行为。

---

## 7. LogRecord 最终建议

### 7.1 首要设计决定：**一套语义 schema，两种编码**

| 编码 | 用途 | 形态 | 理由 |
|---|---|---|---|
| **Flash 编码** | 本地耐久 | **定长 128 B 打包二进制** | 段内定长寻址、可整体 CRC、无解析开销、便于丢弃半写尾记录 |
| **Wire 编码** | 上云 | **CBOR**（整数键或定序数组） | 自描述、可扩展（云端可跳过未知键）、体积小 |

**为什么不让两者完全同一份字节**：Flash 需要为"掉电半写可检测"与"定长寻址"服务，因此必须有固定偏移与尾部 CRC；Wire 需要为"schema 演进"服务，因此需要自描述结构。强行统一会同时牺牲两边。**但字段语义、字段 ID、事件 ID 必须共用同一份字典**（附录 A），这才是"一致"的正确含义。

> **⚠️ 本节布局已由 P1 契约冻结文档 §2 修订为 v2**（`version = 2`）：
> **删除 `persist` 与 `uploaded` 两个位**，`boot_seq` 升级为 **uint32**，偏移相应调整。
> 以下为**修订后**的冻结布局。

### 7.2 Flash 记录布局（定长 128 B，**v2**）

```
偏移  长度  字段            说明
─────────────────────────────────────────────────────────────────────
0     1    version         记录格式版本 = **2**（不是事件字典版本）
1     1    level           LogLevel: 0=DEBUG 1=INFO 2=WARN 3=ERROR 4=CRITICAL
2     1    flags           bit0 timestamp_valid
                           bit1 context_present （blob 区有有效内容）
                           bit2-7 保留（写 0，解码必须忽略）
                           ★ 已删除：原 bit1 persist、原 bit6 uploaded
3     1    param_count     0..8
─────────────────────────────────────────────────────────────────────
4     4    seq             uint32  全局单调，**允许空洞、不允许重复**
8     4    boot_seq        uint32  本次开机代次（log_init 时 +1 并落盘）
12    2    event_id        uint16  LogEventId
14    2    packed          bit0-2 context_kind；bit3-15 保留
16    4    uptime_ms       uint32  millis() 取值（本次开机内单调）
20    4    timestamp       uint32  Unix 秒；无效时写 0
─────────────────────────────────────────────────────────────────────
24    2    blob_len        uint16  context blob 有效字节数（0..32）
26    2    reserved16      写 0
─────────────────────────────────────────────────────────────────────
28    48   params[8]       每项 6 B：{ id:u8, type:u8, value:u32le }
                            （48 B = 8 × 6，紧凑排布，读取用 memcpy）
76    32   blob            context / 短字符串区（见 §10）
─────────────────────────────────────────────────────────────────────
108   4    crc32           对 offset 0..107 计算（CRC32 与 BinStorage 一致）
112   16   reserved        预留扩展，写 0，解码必须忽略
─────────────────────────────────────────────────────────────────────
合计  128 B
```

**不变量**：**LogRecord 不知道自己是否已被上传，也不知道自己是否该被持久化。**
持久化由 Level Policy（§15.1）唯一决定；上传状态由"所在段文件是否仍存在"表达。

**字节序**：全部小端（与 ESP32-S3 一致，与 `workflow_storage.cpp` 的 `put_u32` 一致）。

### 7.3 逐条回答需求 §六 的 14 个问题

| # | 问题 | 答案 | 说明 |
|---|---|---|---|
| 1 | 哪些字段**必须存在** | `version` `level` `flags` `param_count` `seq` `boot_seq` `event_id` `uptime_ms` `timestamp`(+valid) `crc32` | 缺任一都会破坏排序/去重/损坏检测 |
| 2 | 哪些**可以省略** | `device_id`（见 #8）、独立的 schema version（并入 `version`）、`params`/`blob`（允许 0）、`reserved` | — |
| 3 | 哪些**只适合 Flash** | `crc32`、`flags.uploaded`、`flags.persist`、`reserved` | CRC 由 Flash 半写检测需要；`uploaded` 是本机诊断位，云端不需要 |
| 4 | 哪些**只适合 Cloud** | 批次头（`batch_size`、`seq_from`、`seq_to`、`drop_count`、`ack_required`） | 属于"一次上传会话"而非"一条日志" |
| 5 | Cloud 与 Flash 是否同一 Record | **语义同一、编码不同**（见 §7.1） | — |
| 6 | 是否需要 record version | **必须**（1 B） | 未来加字段/改布局时，否则老固件读新记录会误判 |
| 7 | 是否需要 schema version | **不需要独立字段**；另设 **事件字典版本**，只出现在批次头（一次/会话） | 事件字典变更频率远低于记录频率，放头里省 100+ B/批 |
| 8 | 是否需要 device ID | **不在记录内**。设备身份由 MQTT Client ID（`guo_feeder_001`）+ Topic 隐含；云端按连接维度补全 | 128 B 里省 6-16 B，且避免"设备改 ID 后历史记录身份混乱" |
| 9 | 是否需要 boot sequence | **必须**（uint16） | 时间无效期间区分"不同次开机"的**唯一**依据 |
| 10 | 是否需要 monotonic sequence | **必须**（uint32） | 去重、断点续传、乱序检测 |
| 11 | timestamp 无效时怎么办 | `timestamp = 0` 且 `flags.bit0 = 0`；**照常产生、照常入队，不延迟** | 见下 #12 |
| 12 | Time 尚未同步时怎么办 | **不延迟上传**。排序降级为 `(boot_seq, uptime_ms)` | 延迟会引入 RAM 暂存 + 补写逻辑（复杂且易错），而 `boot_seq` 已能可靠排序 |
| 13 | 如何区分不同次启动 | `boot_seq` 单调递增（持久化） | 若 Flash 不可用导致 `boot_seq` 无法持久化 → 写 0 并在 `boot_complete` 记录中标记 `boot_seq_unreliable` |
| 14 | 云端如何去重 | 主键 `(device_id, boot_seq, seq)`；重传时 `seq` 不变 ⇒ 幂等 | 见 §13.4 |

> **⚠️ 本节已由 P1 契约冻结文档 §3 修订。**
> 原方案（"每次 flush 后更新 `last_seq`"）存在**跨重启 seq 重复**漏洞：
> RAM 中尚未 flush 的 seq 在重启后会被重新分配。现改为 **Boot 预留 seq 区间**。

### 7.4 `boot_seq` 与 `seq` 的持久化（本设计唯一新增的持久状态）

**漏洞回顾**（P1 评审发现）：

```
meta.last_seq = 100
  → emit 101, 102, 103（仍在 RAM，未 flush）
  → ESP.restart()
启动后 meta.last_seq 仍 = 100
  → 下一条 seq = 101        ← 与重启前的 101 重复
```

即"跨重启单调"实际只做到"**已 flush 的**跨重启单调"，与声明不符。

**修复：reserve-on-boot（Boot 时预留区间）**

```
/log/meta.bin  （固定 32 B，fmt_version = 2）
  offset 0   4   magic          0x474C4F47 ("GLOG")
  offset 4   1   fmt_version    = 2
  offset 5   1   reserved
  offset 6   2   reserved16
  offset 8   4   boot_seq       uint32  本次启动代次
  offset 12  4   seq_reserved   uint32  已"预留出去"的 seq 高水位
  offset 16  4   corrupt_count  uint32  历史损坏段计数
  offset 20  8   reserved
  offset 28  4   crc32          覆盖 [0..27]
```

```
log_init():
    seq_base          = meta.seq_reserved + 1
    meta.seq_reserved += LOG_SEQ_RESERVE      // 默认 256
    meta.boot_seq     += 1
    写 meta.bin（1 次）                        ← 该区间被永久预留

log_emit():
    seq = seq_base + (本轮已用计数++)
    if (seq > meta.seq_reserved):             // 区间用尽
        meta.seq_reserved += LOG_SEQ_RESERVE
        写 meta.bin（1 次）
```

| 性质 | 说明 |
|---|---|
| 唯一性 | ✅ seq 一旦预留，永不再次分配 |
| 单调性 | ✅ 跨重启严格递增 |
| **空洞** | ⚠️ **允许且预期**。掉电会浪费未使用的 seq；**seq 只需唯一/单调，不需要连续** |
| meta 写次数 | Boot 1 次 + 每 256 条 1 次 —— **比"每次 flush 更新"更少** |
| `boot_seq` | **uint32**（P1 由 uint16 升级），在 `log_init()` 时 +1，**不是** Boot 完成时 |

**Flash 不可用时的降级**：meta 写入失败 → `s_seq_reliable = false`，
后续批次头 `flags.bit0 = 0`（见 §12.2），云端对该批按"可能重复"处理。

**关键设计**：`meta.bin` 是**可重建的加速缓存**，真相源永远是段文件内容。
即便 meta 丢失，也可由段扫描重建 `seq_reserved`（取所有已存记录的最大 seq 上取整到区间边界），
此时 `seq_reliable = false`。

> **设计取舍说明**：`last_seq` 与 `boot_seq` 放同一个 32 B 小文件，是因为它们**总是同时变更**（每次 flush）。拆成两个文件会引入"两次写、可能只成功一次"的不一致。合并后一次 `bin_storage_write_atomic` 即可保证一致。

---

## 8. LogParam 最终建议

### 8.1 结构

```c
// 磁盘/线上 6 B 紧凑形态（与 §7.2 offset 24 的 params[] 一致）
enum LogParamType : uint8_t {
    LOGP_I32   = 1,   // 有符号整数
    LOGP_U32   = 2,   // 无符号整数（slot / version / seq / 时长）
    LOGP_F32   = 3,   // 浮点（重量 g、温度 °C、电压 V）
    LOGP_BOOL  = 4,   // 0/1
    LOGP_ENUM  = 5,   // 枚举整数（原因 / 来源 / 状态）——解码按事件的字典解释
    LOGP_STR   = 6,   // 短字符串：value = (blob_off << 16) | blob_len
    LOGP_I8    = 7,   // 小整数（RSSI 等）
    LOGP_U16   = 8    // 中等整数（端口等）
};
```

**没有 `LOGP_LONG`**：ESP32 上 `long` 就是 32 位，与 `I32` 重复。
**没有动态 `String` 类型**：字符串一律走 blob（见 §8.3）。

### 8.2 内存中的构造形态（供 emitter 使用）

```c
struct LogParamIn {
    uint8_t id;      // LogParamId（附录 A），同一事件内不得重复
    uint8_t type;    // LogParamType
    union { int32_t i; uint32_t u; float f; } v;
};
// sizeof == 8（含 2 B padding，换取 4 B 对齐的取值便利）
```

**参数个数上限 8**（与 §7.2 的 `params[8]` 一致）。超出 → **编译期或运行期拒绝**，绝不截断（需求 §19 精神）。

### 8.3 字符串处理（关键设计点）

**问题**：`wf_id` 最长 16 B、`module` 最长 16 B、`cmd_id` 可能是 "8010" 这类短串、`path` 可能很长。

**方案**：32 B 的 **Context Blob** 兼任"短字符串池"。

```c
// emitter 侧：由 LogManager 提供的定长暂存
struct LogStrRef { uint16_t off; uint16_t len; };
```

| 规则 | 内容 |
|---|---|
| 单串上限 | **24 B**（留 8 B 给 context 数值字段，见 §10） |
| 超长处理 | **截断**并在 blob 中标记（blob 末尾保留 1 位标志） |
| 编码 | **UTF-8 原样**（项目已有中文名如 `每日开阀测试1`，实测 16+ B；截断需保证不切半个汉字 → 按码点边界截断） |
| 是否哈希 | 长字符串（如 `path`）**只用 basename 且 ≤24 B**；SSID 用 **哈希后的 uint16**（`ssid_hash`），绝不存明文 |

**为什么不给 params 里的 STR 分配 8 B 以上**：会让 128 B 记录装不下 8 个数值参数。体积必须守恒，因此字符串统一外置到 blob。

### 8.4 API 形态建议

```c
// log_manager.h —— 对外只有 5 个函数
bool log_init(void);
void log_task(void);                                     // loop() 中调用
bool log_emit(LogEventId id, const LogParamIn *p, uint8_t n);
bool log_emit_ctx(LogEventId id, const LogParamIn *p, uint8_t n,
                  const LogContext *ctx, uint8_t ctx_kind);   // 带 context
void log_set_upload_callback(LogUploadCallback cb);

// 诊断（不上报自身故障时使用）
struct LogStats { uint32_t emitted, dropped_ring, dropped_overflow,
                  flash_written, flash_failed, uploaded, ack_lost,
                  reentrant_drop, encode_failed; };
void log_get_stats(LogStats &out);
```

**便捷宏（推荐，减少调用点出错概率）**：

```c
#define LOG_INFO(id, ...)   LOG_AT(LOGLEVEL_INFO,  id, __VA_ARGS__)
#define LOG_WARN(id, ...)   LOG_AT(LOGLEVEL_WARN,  id, __VA_ARGS__)
#define LOG_ERROR(id, ...)  LOG_AT(LOGLEVEL_ERROR, id, __VA_ARGS__)
#define LOG_CRIT(id, ...)   LOG_AT(LOGLEVEL_CRIT,  id, __VA_ARGS__)

// 参数构造宏（类型由调用点声明，编译期检查个数）
#define LP_U32(pid, val)   ...
#define LP_F32(pid, val)   ...
#define LP_ENUM(pid, val)  ...
#define LP_STR(pid, s)     ...
```

**明确禁止的 API 形态**（沿用审计报告结论）：

| 禁止 | 原因 |
|---|---|
| `log_info(const String&)` | 堆分配 + 碎片；无法承载类型语义 |
| `log_printf(fmt, ...)` | 嵌入式下可变参数易错、无法编译期裁剪、体积失控 |
| `log(const char* json)` | 调用方构造 JSON → 违反 `AGENTS.md` "内部模块不处理 JSON" |
| 任何同步写 Flash 的接口 | 违反非阻塞铁律 |
| 把 `LogParam` 传成指针数组再让 LogManager 保留引用 | 必须**调用内拷贝**，调用方栈帧返回即失效 |

---

## 9. LogEventId 完整分类

### 9.1 ID 空间规划

采用**段式 ID**，为未来模块预留空间，避免"下一个模块加进来就要重排"：

| 段 | 域 | 已用 | 预留 |
|---|---|---|---|
| `0x00xx` | 保留（NONE / 自检） | 1 | 255 |
| `0x01xx` | System / Boot / Restart | 12 | 244 |
| `0x02xx` | Config | 10 | 246 |
| `0x03xx` | Storage | 6 | 250 |
| `0x04xx` | Workflow | 12 | 244 |
| `0x05xx` | Water（Dispense / Valve / Weight） | 16 | 240 |
| `0x06xx` | WiFi | 7 | 249 |
| `0x07xx` | MQTT / Cloud / **Log 自身** | 11 | 245 |
| `0x08xx` | Time / RTC | 10 | 246 |
| `0x09xx` | Mijia BLE | 4 | 252 |
| `0x0Axx` | Command | 4 | 252 |
| `0x0Bxx` | Capability Registry | 2 | 254 |
| `0x0Cxx` | Event Manager | 2 | 254 |
| `0x0Dxx` | ComputerReset | 3 | 253 |
| `0x0Exx` | OLED / UI | 1 | 255 |
| `0x0Fxx` | **Motor（未来）** | 0 | 256 |

**合计已定义 100 个事件**（其中约 65 个是本次必须新增，其余为同一模块内的细分，见下表）。

### 9.2 完整分类表

> 列含义：**Level** = 建议等级；**Flash** = 是否落盘；**Cloud** = 是否上云；**RT** = 实时性（`NO`=不入 Log / `LOW`=可批量延迟 / `NORMAL`=尽快 / `IMM`=立即）；**Ctx** = 是否附带 State Context（见 §10）。

#### 0x01xx System / Boot / Restart

| ID | 事件 | Level | Flash | Cloud | RT | 参数（id:type） | Ctx | 产生位置 |
|---|---|---|---|---|---|---|---|---|
| 0x0101 | `LOG_SYS_BOOT_COMPLETE` | INFO | NO | YES | NORMAL | `boot_seq:U16` `reset_reason:ENUM` `init_ms:U32` `loaded:U8` `total:U8` `count:U16` | SYSTEM | `main.cpp setup()` 末尾（`config_boot_validate()` 后） |
| 0x0102 | `LOG_SYS_BOOT_INCOMPLETE_PREV` | **CRITICAL** | YES | YES | IMM | `reset_reason:ENUM` `boot_seq:U16` | SYSTEM | `config_manager.cpp config_init()`（`last_boot_ok==false`，`:1066`） |
| 0x0103 | `LOG_SYS_RESET_ABNORMAL` | **CRITICAL** | YES | YES | IMM | `reset_reason:ENUM` | SYSTEM | `system_command.cpp system_command_init()`（`:222` 已有 `esp_reset_reason()`） |
| 0x0104 | `LOG_SYS_RESET_NORMAL` | INFO | NO | YES | NORMAL | `reset_reason:ENUM` | SYSTEM | 同上 |
| 0x0105 | `LOG_SYS_INIT_FAILED` | **ERROR** | YES | YES | NORMAL | `module:STR` `err_code:I32` | SYSTEM | `main.cpp setup()` 各层 init 返回 false 处 |
| 0x0106 | `LOG_SYS_FS_MOUNT_FAILED` | **CRITICAL** | —* | YES | IMM | `err_code:I32` | SYSTEM | `main.cpp`（LittleFS.begin 失败）。*FS 不可用，**不可能落盘** |
| 0x0107 | `LOG_SYS_PSRAM_ALLOC_FAILED` | **CRITICAL** | YES | YES | IMM | `which_pool:ENUM` `need_bytes:U32` `free_bytes:U32` | SYSTEM | `workflow.cpp workflow_init()` / `capability_registry.cpp` |
| 0x0108 | `LOG_SYS_HEAP_LOW` | WARN | NO | YES | NORMAL | `free_bytes:U32` `min_free:U32` | SYSTEM | 可选：`state` 边沿检测 |
| 0x0109 | `LOG_SYS_RESTART_REQUESTED` | INFO | NO | YES | NORMAL | `state:ENUM` `count:U16` `remain_ms:U32` | SYSTEM | `system_command.cpp system_command_request_restart()` |
| 0x010A | `LOG_SYS_RESTART_EXECUTED` | INFO | NO | YES | **IMM** | `remain_ms:U32` | SYSTEM | `system_command.cpp`（`ESP.restart()` 前，`:312`） |
| 0x010B | `LOG_SYS_RESTART_CANCELLED` | INFO | NO | YES | NORMAL | — | — | `config_restart cancel` 路径 |
| 0x010C | `LOG_SYS_CRITICAL_OP_UNDERFLOW` | **CRITICAL** | YES | YES | IMM | `count:U16` `was:ENUM` | SYSTEM | `system_command.cpp critical_operation_release()` |

#### 0x02xx Config

| ID | 事件 | Level | Flash | Cloud | RT | 参数 | Ctx | 产生位置 |
|---|---|---|---|---|---|---|---|---|
| 0x0201 | `LOG_CFG_LOAD_DONE` | INFO | NO | YES | NORMAL | `loaded:U8` `total:U8` `init_ms:U32` | — | `config_manager.cpp config_init()` |
| 0x0202 | `LOG_CFG_MODULE_LOAD_FAILED` | **ERROR** | YES | YES | NORMAL | `module:STR` | — | `config_manager.cpp`（`module not loaded`） |
| 0x0203 | `LOG_CFG_CHANGE_APPLIED` | INFO | NO | YES | NORMAL | `module:STR` `key:STR` `version:U32` | — | `config_manager.cpp exec_set_field()`（**落盘后**才发，不是入队时） |
| 0x0204 | `LOG_CFG_SAVE_OK` | INFO | NO | YES | NORMAL | `count:U16` `version:U32` | — | `config_manager.cpp config_save()` |
| 0x0205 | `LOG_CFG_COMMIT_FAILED_ROLLBACK` | **ERROR** | YES | YES | NORMAL | `stage:ENUM` `module:STR` | SYSTEM | `config_manager.cpp commit_fail_and_rollback()` |
| 0x0206 | `LOG_CFG_RECOVERED_FROM_BACKUP` | WARN | YES | YES | NORMAL | `module:STR` `reason:ENUM` | SYSTEM | `config_manager.cpp recover_module()` |
| 0x0207 | `LOG_CFG_VERSION_REBUILT` | WARN | YES | YES | NORMAL | `count:U16` | — | `config_manager.cpp bootstrap_version_file()` |
| 0x0208 | `LOG_CFG_FACTORY_RESET` | WARN | YES | YES | NORMAL | `module:STR` | — | `config_manager.cpp exec_reset()` |
| 0x0209 | `LOG_CFG_WRITE_REJECTED` | WARN | NO | YES | NORMAL | `module:STR` `key:STR` `err_code:I32` | — | `config_manager.cpp`（unknown field / 校验失败） |
| 0x020A | `LOG_CFG_RESTART_TIMEOUT` | INFO | NO | YES | NORMAL | `module:STR` | — | `config_manager.cpp config_task()` 5 分钟倒计时到期 |

> **明确不产生 Log**：`config_cmd_enqueue()` 的 `enqueue accepted, pending=N`（DEBUG）。理由：UI 调参会高频触发，落盘会迅速消耗擦写寿命（审计 §4.8）。

#### 0x03xx Storage

| ID | 事件 | Level | Flash | Cloud | RT | 参数 | Ctx | 产生位置 |
|---|---|---|---|---|---|---|---|---|
| 0x0301 | `LOG_STG_FS_UNAVAILABLE` | **CRITICAL** | —* | YES | IMM | `path:STR` `err_code:I32` | SYSTEM | `json_storage.cpp` / `bin_storage.cpp` init |
| 0x0302 | `LOG_STG_ATOMIC_WRITE_FAILED` | **ERROR** | YES | YES | NORMAL | `path:STR` `stage:ENUM` `err_code:I32` | — | 三层存储的原子写失败处 |
| 0x0303 | `LOG_STG_CRC_FAILED` | **CRITICAL** | YES | YES | IMM | `path:STR` `stored_crc:U32` `calc_crc:U32` | — | `workflow_storage.cpp workflow_storage_load()` |
| 0x0304 | `LOG_STG_TXN_RECOVERED` | WARN | YES | YES | NORMAL | `slot:U8` `staged_count:U8` `action:ENUM` | — | `workflow_storage.cpp` recover |
| 0x0305 | `LOG_STG_WRITE_VERIFY_FAILED` | **ERROR** | YES | YES | NORMAL | `path:STR` | — | `*_write_atomic()` 大小校验失败 |
| 0x0306 | `LOG_STG_READ_FAILED` | WARN | NO | YES | NORMAL | `path:STR` `err_code:I32` | — | 三层读取失败 |

#### 0x04xx Workflow

| ID | 事件 | Level | Flash | Cloud | RT | 参数 | Ctx | 产生位置 |
|---|---|---|---|---|---|---|---|---|
| 0x0401 | `LOG_WF_START` | INFO | NO | YES | NORMAL | `slot:U8` `wf_id:STR` `variant:U32` `source:ENUM` | WORKFLOW | `workflow.cpp workflow_start()` |
| 0x0402 | `LOG_WF_FINISHED` | INFO | NO | YES | NORMAL | `slot:U8` `wf_id:STR` `duration_ms:U32` `steps_done:U8` | — | `workflow.cpp workflow_terminate(WORKFLOW_FINISHED)` —— **当前 0 日志** |
| 0x0403 | `LOG_WF_TIMEOUT` | WARN | YES | YES | NORMAL | `slot:U8` `wf_id:STR` `timeout_ms:U32` `stuck_step:U8` | WORKFLOW | `workflow_terminate(WORKFLOW_TIMEOUT)` |
| 0x0404 | `LOG_WF_FAILED` | WARN | YES | YES | NORMAL | `slot:U8` `wf_id:STR` `fail_step:U8` `action_id:STR` `err_code:I32` | WORKFLOW | `workflow_terminate(WORKFLOW_ERROR)` |
| 0x0405 | `LOG_WF_ACTION_FAILED` | WARN | YES | YES | NORMAL | `slot:U8` `fail_step:U8` `action_id:STR` `err_code:I32` | — | Action poll 返回 FAILED 处 |
| 0x0406 | `LOG_WF_SAVE_FAILED` | WARN | YES | YES | NORMAL | `slot:U8` `err_code:I32` | — | `workflow.cpp workflow_save_transaction()` |
| 0x0407 | `LOG_WF_SAVE_PARTIAL` | WARN | YES | YES | NORMAL | `count:U16` `active:U8` | — | 同上（部分成功，Dirty 残留） |
| 0x0408 | `LOG_WF_CRUD` | INFO | NO | YES | NORMAL | `op:ENUM` `slot:U8` `wf_id:STR` `variant:U32` | — | `command_manager.cpp command_workflow_create/set/delete` |
| 0x0409 | `LOG_WF_MIGRATED` | INFO | NO | YES | NORMAL | `count:U16` | — | `main.cpp` / `workflow.cpp workflow_migrate_to_storage()` |
| 0x040A | `LOG_WF_TEMP_ACTION_TIMEOUT` | WARN | YES | YES | NORMAL | `action_id:STR` `timeout_ms:U32` | — | `workflow.cpp` Temp Action 超时 |
| 0x040B | `LOG_WF_RUNTIME_ALLOC_FAILED` | **ERROR** | YES | YES | NORMAL | `slot:U8` | SYSTEM | `workflow.cpp` 实例池耗尽 |
| 0x040C | `LOG_WF_SAVE_PARTIAL_RETRY_OK` | INFO | NO | YES | NORMAL | `count:U16` | — | 可选：重试后成功（用于确认"部分失败可恢复"） |

> **明确不产生 Log**：`wf_dirty_changed`（`dirty=[3 5 ]`）、`wf_critical_op`（acquired/released）、`[WF][DBG]` 全系列 → 全部保留在 DEBUG/Serial。

#### 0x05xx Water（Dispense / Valve / Weight）

| ID | 事件 | Level | Flash | Cloud | RT | 参数 | Ctx | 产生位置 |
|---|---|---|---|---|---|---|---|---|
| 0x0501 | `LOG_DISPENSE_START` | INFO | NO | YES | NORMAL | `slot:U8` `target_g:F32` `start_g:F32` `source:ENUM` | DISPENSE | `weight.cpp weight_trigger_start()` |
| 0x0502 | `LOG_DISPENSE_DONE` | INFO | NO | YES | NORMAL | `target_g:F32` `start_g:F32` `final_g:F32` `delta_g:F32` `duration_ms:U32` `stop_reason:ENUM` | DISPENSE | `weight.cpp weight_trigger_poll()` 达标分支 —— **当前缺失** |
| 0x0503 | `LOG_DISPENSE_FAILED` | WARN | YES | YES | NORMAL | `target_g:F32` `delta_g:F32` `stop_reason:ENUM` `duration_ms:U32` | DISPENSE | 未达标结束路径 |
| 0x0504 | `LOG_DISPENSE_TIMEOUT` | WARN | YES | YES | NORMAL | `target_g:F32` `delta_g:F32` `timeout_ms:U32` | DISPENSE | 供水超时 |
| 0x0505 | `LOG_VALVE_FORCE_CLOSE` | **CRITICAL** | YES | YES | **IMM** | `reason:ENUM` `weight_g:F32` `valve_open_ms:U32` | DISPENSE | `dispense_guard.cpp dispense_guard_event_callback()` |
| 0x0506 | `LOG_VALVE_FORCE_CLOSE_FAILED` | **CRITICAL** | YES | YES | **IMM** | `err_code:I32` | DISPENSE | 同上 else 分支 |
| 0x0507 | `LOG_VALVE_OVERFLOW_RISK` | **CRITICAL** | YES | YES | **IMM** | `gain_after_close_g:F32` `window_ms:U32` | DISPENSE | **需新增检测**（审计 §16.5） |
| 0x0508 | `LOG_VALVE_SAFETY_TIMEOUT` | WARN | YES | YES | NORMAL | `open_ms:U32` `limit_ms:U32` | — | `valve.cpp valve_task()` 安全超时 |
| 0x0509 | `LOG_VALVE_OPEN` | INFO | NO | YES | NORMAL | `source:ENUM` | — | `valve.cpp valve_set_gpio()` |
| 0x050A | `LOG_VALVE_CLOSE` | INFO | NO | YES | NORMAL | `source:ENUM` | — | 同上 |
| 0x050B | `LOG_VALVE_RATE_LIMITED` | WARN | NO | YES | NORMAL | `open_ms:U32` | — | `valve.cpp`（`MIN_OPERATION_INTERVAL_MS` 命中） |
| 0x050C | `LOG_WEIGHT_ERROR_ENTER` | WARN | YES | YES | NORMAL | `cause:ENUM` `jump_count:U8` `raw:I32` `weight_g:F32` | WEIGHT | `weight.cpp weight_refresh_error_state()` |
| 0x050D | `LOG_WEIGHT_ERROR_EXIT` | INFO | NO | YES | NORMAL | `duration_ms:U32` | — | 同上（1→0 边沿） |
| 0x050E | `LOG_WEIGHT_ZERO_DONE` | INFO | NO | YES | NORMAL | `offset:I32` `samples:U16` `saved:BOOL` | — | `weight.cpp weight_task()` 调零完成 |
| 0x050F | `LOG_WEIGHT_TRIGGER_FIRED` | INFO | NO | YES | NORMAL | `delta_g:F32` `target_g:F32` | — | Weight Trigger 触发 |
| 0x0510 | `LOG_WEIGHT_CALIB_FAILED` | **ERROR** | YES | YES | NORMAL | `module:STR` `err_code:I32` | — | 调零保存失败 |

> **明确不产生 Log**：HX711 `raw/filtered/gram` 高频值（保持注释 / 编译期宏）；`weight_trigger_success` 与 `dispense_done` 合并为一条（0x0502），避免重复。

#### 0x06xx WiFi

| ID | 事件 | Level | Flash | Cloud | RT | 参数 | Ctx | 产生位置 |
|---|---|---|---|---|---|---|---|---|
| 0x0601 | `LOG_WIFI_CONNECT_START` | INFO | NO | YES | NORMAL | `ssid_hash:U16` | — | `wifi_module.cpp`（**不记明文 SSID**） |
| 0x0602 | `LOG_WIFI_CONNECTED` | INFO | NO | YES | NORMAL | `connect_ms:U32` `rssi:I8` | SYSTEM | `wifi_module.cpp` |
| 0x0603 | `LOG_WIFI_CONNECT_TIMEOUT` | WARN | YES | YES | NORMAL | `timeout_ms:U32` `attempt_n:U8` | — | `wifi_module.cpp` |
| 0x0604 | `LOG_WIFI_LOST` | WARN | YES | YES | NORMAL | `connected_ms:U32` | SYSTEM | `wifi_module.cpp` |
| 0x0605 | `LOG_WIFI_RECONNECT_TRY` | WARN | NO | YES | NORMAL | `attempt_n:U8` | — | `wifi_module.cpp` |
| 0x0606 | `LOG_WIFI_PROVISION_ENTER` | INFO | **NO**（🔴 P1 撤销例外） | YES* | NORMAL | `mode:ENUM` | — | **未实现**（审计 §16.3）。*无网，联网后补发 |
| 0x0607 | `LOG_WIFI_PROVISION_DONE` | INFO | **NO**（🔴 P1 撤销例外） | YES* | NORMAL | `result:ENUM` | — | 同上 |

#### 0x07xx MQTT / Cloud / Log 自身

| ID | 事件 | Level | Flash | Cloud | RT | 参数 | Ctx | 产生位置 |
|---|---|---|---|---|---|---|---|---|
| 0x0701 | `LOG_MQTT_CONNECTED` | INFO | NO | YES | NORMAL | `retry_n:U8` `connected_ms:U32` | SYSTEM | `cloud_manager.cpp cloud_process_mqtt_events()` |
| 0x0702 | `LOG_MQTT_DISCONNECTED` | WARN | YES | YES | NORMAL | `outbox:U8` `connected_ms:U32` | SYSTEM | 同上 |
| 0x0703 | `LOG_MQTT_SLEEP_ENTER` | **ERROR** | YES | YES | **IMM** | `retry_n:U8` `sleep_ms:U32` | SYSTEM | `cloud_manager.cpp cloud_task()`（`MQTT_RETRY_MAX=10` 后 1 h 休眠） |
| 0x0704 | `LOG_MQTT_PUBLISH_FAIL` | WARN | YES | YES | NORMAL | `stage:ENUM` | — | `cloud_mqtt_publish_text/binary`（`msg_id < 0`） |
| 0x0705 | `LOG_MQTT_CMD_EXEC_FAILED` | WARN | YES | YES | NORMAL | `cmd_id:STR` `cmd:STR` `obj:STR` `err_code:I32` | — | `on_command_result()` / `cloud_process_rx_message()` |
| 0x0706 | `LOG_CLOUD_FRAG_FAIL` | WARN | NO | YES | NORMAL | `count:U16` | — | `cloud_publish_fragmented()` |
| 0x0707 | `LOG_LOG_UPLOAD_FAIL` | WARN | **NO** | YES | NORMAL | `retry_n:U8` `err_code:I32` | — | LogManager uploader（**自身故障，见 §17**） |
| 0x0708 | `LOG_LOG_ACK_TIMEOUT` | WARN | NO | YES | NORMAL | `seq:U32` `retry_n:U8` | — | 同上 |
| 0x0709 | `LOG_LOG_RING_OVERFLOW` | WARN | **NO** | YES | NORMAL | `count:U16` | — | Flash 段环淘汰最旧段 —— **不可落盘（环已满）** |
| 0x070A | `LOG_LOG_ACK_LOST` | WARN | YES | YES | NORMAL | `seq:U32` `attempt_n:U8` | — | ACK 长时间未达后强制推进 tail |
| 0x070B | `LOG_LOG_SELF_DEGRADED` | **ERROR** | YES | YES | NORMAL | `kind:ENUM` | — | Flash 写连续失败 → 降级为纯 RAM |

#### 0x08xx Time / RTC

| ID | 事件 | Level | Flash | Cloud | RT | 参数 | Ctx | 产生位置 |
|---|---|---|---|---|---|---|---|---|
| 0x0801 | `LOG_TIME_NTP_OK` | INFO | NO | YES | NORMAL | `server:STR` `unix:U32` `drift_ms:I32` | SYSTEM | `time_manager.cpp`（**loop 确认后，非 callback**） |
| 0x0802 | `LOG_TIME_NTP_FAIL` | WARN | YES | YES | NORMAL | `last_ok_age_ms:U32` | — | `time_manager.cpp time_task()` |
| 0x0803 | `LOG_TIME_VALID_ENTER` | INFO | **NO**（🔴 P1 撤销例外） | YES | NORMAL | `state:ENUM` | SYSTEM | `time_update_valid_state()` —— 首次 0→1 是重启后的关键上下文 |
| 0x0804 | `LOG_TIME_INVALID_ENTER` | WARN | YES | YES | NORMAL | `state:ENUM` | SYSTEM | 同上（1→0） |
| 0x0805 | `LOG_TIME_RTC_PROBE` | INFO / WARN | 失败 YES | YES | NORMAL | `addr:U8` `err_code:I32` | — | `time_rtc_probe()` |
| 0x0806 | `LOG_TIME_RTC_BOOT_RESTORE` | INFO | NO | YES | NORMAL | `unix:U32` `drift_ms:I32` | — | RTC → System Time 硬同步 |
| 0x0807 | `LOG_TIME_RTC_CALIBRATED` | INFO | NO | YES | NORMAL | `unix:U32` `drift_ms:I32` | — | `time_rtc_calibrate()` |
| 0x0808 | `LOG_TIME_RTC_WRITE_FAILED` | WARN | YES | YES | NORMAL | `err_code:I32` | — | 同上 |
| 0x0809 | `LOG_TIME_RTC_VL_FLAG` | WARN | YES | YES | NORMAL | — | — | `rtc_read_time()`（VL 置位） |
| 0x080A | `LOG_TIME_RTC_BCD_INVALID` | WARN | YES | YES | NORMAL | `raw:I32` | — | 同上 |

> **明确不产生 Log**：`[Time] NTP skip: no wifi` → DEBUG（审计 §16.10）。

#### 0x09xx Mijia BLE

| ID | 事件 | Level | Flash | Cloud | RT | 参数 | Ctx | 产生位置 |
|---|---|---|---|---|---|---|---|---|
| 0x0901 | `LOG_BLE_DATA_DECODED` | INFO | NO | YES | LOW | `temp:F32` `humid:F32` `batt_v:F32` `mac_suffix:STR` | — | `MiThermometer.cpp`（**限流：值变化超阈值或距上次 ≥10 min**） |
| 0x0902 | `LOG_BLE_DECODE_FAIL` | WARN | NO（限流） | YES | LOW | `fail_kind:ENUM` `count:U16` | — | `MiThermometer.cpp` |
| 0x0903 | `LOG_BLE_SENSOR_LOST` | WARN | YES | YES | NORMAL | `fail_count:U16` | — | 进入 LEVEL2 |
| 0x0904 | `LOG_BLE_SCAN_DISABLED` | INFO | NO | YES | LOW | `level:ENUM` | — | `enter LEVEL1/LEVEL2` |

> **明确不产生 Log**：`ADV len=..` 逐字节 hex（**在 NimBLE 回调内，必须宏隔离**，审计 §16.7）。

#### 0x0Axx Command

| ID | 事件 | Level | Flash | Cloud | RT | 参数 | Ctx | 产生位置 |
|---|---|---|---|---|---|---|---|---|
| 0x0A01 | `LOG_CMD_RUNTIME_QUEUE_FULL` | WARN | YES | YES | NORMAL | `active:U8` `capacity:U8` | — | `command_manager.cpp command_runtime_insert()` |
| 0x0A02 | `LOG_CMD_RUNTIME_TIMEOUT` | WARN | YES | YES | NORMAL | `cmd_id:STR` `cmd:STR` `timeout_ms:U32` | — | `command_runtime_scan_timeouts()` |
| 0x0A03 | `LOG_CMD_REJECTED` | WARN | NO | YES | NORMAL | `cmd_id:STR` `cmd:STR` `err_code:I32` | — | 路由拒绝（版本不匹配 / 非法对象 / 运行中） |
| 0x0A04 | `LOG_CMD_APPLIED` | INFO | NO | YES | NORMAL | `cmd_id:STR` `cmd:STR` `obj:STR` `duration_ms:U32` | — | 可选：仅写类命令（config_set / workflow CRUD） |

> **明确不产生 Log**：`cmd_result` 全量 JSON（已在 Cloud 通道，重复）、`cmd_dup_cmd_id`（DEBUG）。

#### 0x0Bxx Capability Registry / 0x0Cxx Event / 0x0Dxx ComputerReset / 0x0Exx OLED / 0x0Fxx Motor

| ID | 事件 | Level | Flash | Cloud | RT | 参数 | Ctx | 产生位置 |
|---|---|---|---|---|---|---|---|---|
| 0x0B01 | `LOG_REG_REBUILT` | INFO | NO | YES | NORMAL | `type:ENUM` `version:U32` `count:U16` `checksum:U32` | — | `capability_registry.cpp registry_sync()`（仅 `rebuild`，`reuse` 降 DEBUG） |
| 0x0B02 | `LOG_REG_SAVE_FAILED` | **ERROR** | YES | YES | NORMAL | `type:ENUM` `version:U32` | — | 同上 |
| 0x0C01 | `LOG_EVT_QUEUE_FULL` | WARN | NO（计数） | YES | NORMAL | `dropped_total:U32` `queue_size:U8` | — | `event_manager.cpp event_push()` |
| 0x0C02 | `LOG_EVT_STORM_DROPPED` | WARN | NO（计数） | YES | NORMAL | `dropped_total:U32` | — | 同上 |
| 0x0D01 | `LOG_CRESET_PULSE` | INFO | NO | YES | NORMAL | `hold_ms:U32` `source:ENUM` | — | `computer_reset.cpp` |
| 0x0D02 | `LOG_CRESET_SAFETY_TIMEOUT` | WARN | YES | YES | NORMAL | `hold_ms:U32` | — | `computer_reset_task()` |
| 0x0D03 | `LOG_CRESET_POOL_EXHAUSTED` | **ERROR** | YES | YES | NORMAL | `capacity:U8` | — | `computer_reset.cpp` |
| 0x0E01 | `LOG_OLED_INIT_FAILED` | WARN | NO | YES | NORMAL | `err_code:I32` | — | `oled.cpp oled_init()` |
| 0x0F01+ | **Motor（预留）** | — | — | — | — | — | — | 未来模块，ID 段已留 |

### 9.3 统计与裁剪原则

| 项 | 值 |
|---|---|
| 已定义事件 | **100** |
| 其中"必须新增"（当前完全不存在） | **约 65** |
| 其中"已有 Serial 输出、需改为结构化" | **约 30** |
| 其中"可选/低优先级" | **约 5** |
| **明确不进入 LogManager 的现有日志类型** | DEBUG 级全部 + 18 类（审计报告 §15） |

**裁剪三原则**（写代码时必须遵守）：

1. **状态迁移点才发 Log，不在 poll 里发**。`workflow_task()` / `valve_task()` / `weight_task()` 每轮执行 → 零 Log。
2. **成功路径只保留"业务事实"，不保留"函数调用"**。`Action 成功` 不记，`Workflow 完成` 记。
3. **计数优于逐条**。`event_queue_full` 用 `dropped_total` 一条，而非每次丢弃一条。

---

## 10. State Context 设计

### 10.1 原则

> **一个 Log = Event + 必要 Context**（需求 §九）。
> Context 由**产生方**填充，不由 LogManager 去反查各模块内部结构（避免 LogManager 依赖业务模块，见 §14）。

### 10.2 Context 种类与布局（复用 §7.2 的 32 B blob）

```c
enum LogContextKind : uint8_t {
    LOGCTX_NONE     = 0,   // blob_len = 0
    LOGCTX_SYSTEM   = 1,   // 12 B
    LOGCTX_WORKFLOW = 2,   // 16 B
    LOGCTX_DISPENSE = 3,   // 20 B
    LOGCTX_WEIGHT   = 4,   // 16 B
    LOGCTX_MQTT     = 5,   // 12 B
    LOGCTX_STRING   = 6,   // 纯短字符串池（无 context，仅承载 STR 参数）
};
```

| Kind | 字段（偏移 : 类型） | 大小 | 说明 |
|---|---|---|---|
| `SYSTEM` | `0:u8 reset_reason` `1:u8 time_valid` `2:u8 wifi_state` `3:u8 mqtt_state` `4:u16 heap_free_kb` `6:u16 boot_seq` `8:u32 uptime_ms` | **12 B** | 用于 BOOT / 恢复 / 存储故障 / 重启 |
| `WORKFLOW` | `0:u8 slot` `1:u8 step_index` `2:u8 state` `3:u8 reserved` `4:u32 variant` `8:u32 elapsed_ms` `12:u32 timeout_ms` | **16 B** | 用于 WF 失败 / 超时 / START |
| `DISPENSE` | `0:f32 target_g` `4:f32 start_g` `8:f32 current_g` `12:u32 valve_open_ms` `16:u32 timeout_ms` | **20 B** | 用于供水未达标 / 强制关阀 / 溢水风险 |
| `WEIGHT` | `0:i32 raw` `4:i32 filtered` `8:f32 gram` `12:u8 cause` `13:u8 jump_count` `14:u16 reserved` | **16 B** | 用于重量异常 |
| `MQTT` | `0:u8 retry_n` `1:u8 outbox` `2:u8 sleep_mode` `3:u8 reserved` `4:u32 connected_ms` `8:u32 last_error` | **12 B** | 用于 MQTT 断开 / 休眠 |
| `STRING` | 若干变长短串，用 `LogStrRef{off,len}` 引用 | ≤32 B | 见 §8.3 |

### 10.3 与 STR 参数共存规则

blob 只有 32 B，因此：

| 规则 | 内容 |
|---|---|
| **互斥** | 一条记录要么带 **数值型 Context**（SYSTEM/WORKFLOW/DISPENSE/WEIGHT/MQTT），要么带 **纯字符串池**（STRING），**不能同时** |
| 例外 | 当一个事件**既需要上下文又需要字符串**（如 `LOG_WF_FAILED` 需要 `wf_id` + `action_id` 两个串 + WORKFLOW 上下文）→ 采用 **"数值进参数、字符串进 blob、Context 只保留最关键的 4 B"** 的降级策略：WF_FAILED 的 Ctx 取 `{slot, fail_step, err_code, reserved}`（4 B），blob 留给两个字符串 |
| 实现 | 由各事件的**字典条目**声明"是否允许 Context / blob 用途"，编译期固定，不做运行期协商 |

### 10.4 明确不在 Context 内的字段

| 字段 | 理由 |
|---|---|
| 全部 19 个 System State 键 | 复制整表既费 RAM 又无诊断价值 |
| Config 全部内容 | 太大；只记 `version` |
| Workflow 全部 Step 内容 | 太大；只记 `slot/id/variant/step_index` |
| `STATE_WEIGHT_VALUE` 的历史滤波窗口 | 只保留当前值 |
| 网络 RSSI 周期值 | 用 `LOG_WIFI_CONNECTED` 的快照值，不做周期记录 |

---

## 11. Flash Ring Buffer

### 11.1 首要约束：LittleFS 是 copy-on-write

**这是本节所有结论的基础，必须先讲清楚。**

LittleFS 是**日志结构 + COW** 文件系统（不是 FAT 那样的原地更新 FS）。含义：

| 操作 | 实际代价 |
|---|---|
| 修改文件中 128 B（原地覆盖一条记录） | 不是"写 128 B"，而是**重写该文件所在的整个 block（默认 4 KB）**，并更新 metadata pair |
| 在文件末尾追加 4 KB | **较便宜**（分配新 block + 更新 metadata pair） |
| 创建 / 删除小文件 | 便宜（metadata 操作） |

**结论**：需求 §十 提出"优先考虑减少 Flash 原地修改"是**完全正确**的方向。因此：

- ❌ **不采用**"固定 512 槽环形 + 每条原地覆盖 + 每条 uploaded 位原地置位"的方案。那会产生**每条一次 4 KB block 重写**，是设计中最坏的形态。
- ✅ **采用**"顺序追加的段文件 + 整段删除"的方案。段一旦写满就不再修改，确认后**整文件删除**。

### 11.2 段式环形结构

```
/log/
├── meta.bin            32 B   固定（见 §7.4）：boot_count / last_seq / corrupt_count
├── s0000000.log        4 KB   已封段（满 N=32 条）
├── s0000001.log        4 KB   已封段
│   ...
├── s000000F.log        ≤4 KB  当前写入段（未满）
└── （最多 16 个段文件）
```

| 项 | 值 | 说明 |
|---|---|---|
| 记录大小 | **128 B 定长** | 段内按 `index = (offset - 16) / 128` 直接定位 |
| 段头 | 16 B | `magic(4) + seg_index(4) + first_seq(4) + crc32(4)`（`crc32` 覆盖段头前 12 B） |
| 段容量 | **31 条**（P1 冻结） | 16 + 31×128 = **3984 B < 4096**，严格落在单个 LittleFS block 内 |
| 段数上限 | **16** | 16 × 31 = **496 条**，16 × 3984 B ≈ **63.7 KB** |
| 总占用 | **≈ 63.7 KB / 12.5 MB（0.5%）** | 无需新分区 |

> **P1 冻结说明**：原写 32 条（4112 B）会**跨两个 block**，使 LittleFS 写入行为不确定。
> 31 条（3984 B）干净落在单 block 内，少 16 条容量无产品意义。**不再"编码阶段再决定"。**
| 段文件名 | `s%07u.log`（7 位零填充，**字典序 = 数值序**） | 便于 `bin_storage_foreach` 后直接排序 |
| 复用 BinStorage | `write` / `read` / `remove` / `foreach` / `crc32` | BinStorage 头文件已明确"用于大文件（BIN / OTA / **Log**）" |

### 11.3 逐条回答需求 §十 的 16 个问题

| # | 问题 | 结论与理由 |
|---|---|---|
| 1 | 固定长度还是变长记录 | **定长 128 B**。定长使段内寻址 O(1)、CRC 可整段校验、半写尾记录检测简单。变长会让"丢弃残缺尾记录"变成解析问题 |
| 2 | 最终 Record Size | **128 B**（含 16 B 段头独立）。理由：对齐一个 4 KB block 的 1/32；容纳 8 参数 + 32 B context 后仍有 20 B 预留 |
| 3 | Ring Buffer 总大小 | **496 条 ≈ 63.7 KB**（16 段 × **31** 条，P1 冻结） |
| 4 | Head / Tail | **不用滑动 head/tail 指针**。改为**段序号**：写入段 `write_seg`、未确认最旧段 `ack_seg`。语义等价但更省写：确认一批 = 删除一整个段文件，**不修改任何已有数据** |
| 5 | 是否需要 sequence | **需要**（Record 内 `seq` uint32 + 段头 `first_seq`）。段头冗余存 `first_seq` 是为了"光看段头就能判断段区间"，避免读整段 |
| 6 | 是否需要 CRC | **需要两层**：① 段头 CRC32（12 B → 4 B）；② 每条记录 CRC32（104 B → 4 B）。记录级 CRC 用于丢弃半写尾记录；段头 CRC 用于识别被破坏的段文件 |
| 7 | 掉电恢复 | 启动时：按名排序段文件 → 读段头（CRC 失败则该段整体作废并删除）→ 从末段第 0 条起逐条校验 → **遇到第一条 CRC 失败即视为 append 点**，其后字节被忽略（**不清理、不重写**，下次写入直接覆盖即可）；若末段已满 32 条 → 逻辑上已封段，下次写入新建段 |
| 8 | 部分写入 | 半写记录 CRC 必然失败 → 被 #7 的规则丢弃。**不需要专用修复逻辑** |
| 9 | CRC 错误 | 记录级：丢弃该条及其后（视为 append 点）。段头级：删除整段 + `meta.corrupt_count++` + 发 `LOG_STG_CRC_FAILED`（CRITICAL） |
| 10 | Ring 满后覆盖谁 | **删除最旧的段**（FIFO）。理由：最旧记录诊断价值最低；按段删除 = 1 次文件删除，最省 Flash |
| 11 | CRITICAL 是否允许覆盖 WARN | **允许**。理由：CRITICAL 产生后**立即单独 flush**（见 #14），它是最新的段，需要 **496** 条更新的记录才能把它挤出环 —— 按 §22 的速率估算这是**数周**量级。为它单独设计保留区属于过度设计 |
| 12 | 是否需要独立 Critical 区 | **不需要**。理由同上；且独立区会引入"两套容量/两套 ack"的复杂度 |
| 13 | 是否需要 batch write | **需要**。WARN/ERROR 累积到 **8 条** 或 **5 秒**（先到者）后一次写入。CRITICAL 立即单条写 |
| 14 | 是否需要立即 flush | **CRITICAL 需要**（单条立即写 + 立即尝试上传）；WARN/ERROR 走批量 |
| 15 | Flash 擦写寿命 | 见 §11.6 |
| 16 | 如何减少 Flash 写次数 | ① 只有 WARN+ 落盘（INFO 不落）；② 段式追加、整段删除 ⇒ **零原地修改**；③ 批量提交（8 条/次）；④ `meta.bin` 与段写入**合并为一次**操作序列（见 §11.5）；⑤ ACK 只删段文件，不改数据 |

### 11.4 `uploaded` 位 vs `head/tail` 消费者指针 —— 明确推荐

> **⚠️ P1 修订**：下方方案 (b) 的结论不变且加强 —— 不仅"不采用 `uploaded` 位"，
> 而且 **Record 格式中根本不存在该位**（已从 §7.2 的 `flags` 中删除 bit6）。

| 方案 | 机制 | 代价 | 结论 |
|---|---|---|---|
| **(a) 每条记录 `uploaded` 位** | ACK 后回写每条记录的第 2 字节 | **每条 = 一次 4 KB block 重写**。一批 16 条 = 16 次 block 重写。LittleFS 下这是最坏形态 | **❌ 否决** |
| **(b) head/tail 消费者指针（本设计）** | 写入指针 = 段序号（由目录扫描推导）；消费指针 = 已确认最旧段（由**删除段文件**表达） | ACK 一批 = **1 次文件删除**；无原地修改 | **✅ 采用** |

**为什么 (b) 不需要持久化 head/tail**：

- **head（写入位置）**：可由"按名排序的段列表 + 末段的记录数（由 CRC 扫描得出）"**完全推导**，无需持久化。
- **tail（未确认最旧）**：由"仍存在的段文件"**天然表达** —— 已确认的段被删除，剩下的第一个就是 tail。
- 因此只有 `boot_seq` / `seq_reserved` / `corrupt_count` 需要持久化（`meta.bin`，32 B，见 §7.4）。

> 这正是需求 §十 所期望的"优先考虑减少 Flash 原位修改"的最优解：**把状态编码进文件系统的目录结构，而不是编码进记录内部。**

**🔴 P1 强调（防止后续实现走回头路）**：

Record 的 `flags` 中**不保留** `uploaded` 位，也**不保留** `persist` 位。
理由：

1. `uploaded` 一旦存在，后续实现者极可能写出"ACK 后回写 uploaded 位"，
   把上面刚否决的 4 KB block 重写方案又引回来 —— **与"零原地修改"直接矛盾**。
2. `persist` 与 §15.1 的 Level Policy **语义重复**：持久化由 Level 唯一决定，
   不是 Record 自身的生命周期状态。

**不变量：LogRecord 不知道自己是否已被上传，也不知道自己是否该被持久化。**

### 11.5 一致性：段写入与 `meta.bin` 的顺序

```
flush(records[0..n]):
  ① 若当前段剩余空间 < n×128 → 新建段文件（seg_index = 上一个 + 1，first_seq = 首条 seq）
  ② 向当前段写入 n×128 B（顺序追加）
  ③ （P1 修订）**不再在 flush 时更新 seq** —— seq 已由 Boot 时的区间预留保证唯一
     （见 §7.4）。本步仅保留"元信息一致性"用途，可为 no-op。
```

| 掉电点 | 后果 | 是否可接受 |
|---|---|---|
| ① 之后 | 存在一个空段文件 | ✅ 下次启动扫描到空段，继续在其中写入 |
| ② 中途 | 末段尾部有半写记录 | ✅ CRC 失败 → 视为 append 点，丢弃 |
| ~~② 完成、③ 之前~~ | （P1 后不再存在该风险） | ✅ seq 唯一性由 Boot 预留保证，不依赖 flush 后更新 |
| ③ 中途 | meta 半写 | ✅ meta 有 CRC；失败则重新扫描重建 |

> **关键设计**：`meta.bin` 是**可重建的加速缓存**，不是唯一真相源。真相源永远是"段文件的内容"。这消除了任何"双写不一致"的风险。

### 11.6 Flash 擦写寿命估算

| 项 | 值 |
|---|---|
| WARN+ 记录率（§22 估算） | 5–50 条/日（异常日）；多数日子 0–5 条 |
| 每批写入的数据量 | 8 条 × 128 B = 1 KB |
| 段文件大小 | ≈ 4.1 KB（1 个 block 量级） |
| **每日写入的段数** | ≤ 7 段/日（最坏 50 条/日 ÷ 8 条/批 ÷ 32 条/段 ≈ 0.2 段/日；按"每批一个新 block"悲观估算 ≈ 7 个 block/日） |
| **每日 block 擦写** | ≤ 10 次（含 meta.bin 的 2 次/批） |
| ESP32-S3 内置 Flash 寿命 | 约 100,000 次/block |
| 年化 | ≤ 3,650 次/年 ⇒ **理论寿命 > 27 年** |
| 真实约束 | 小文件碎片化。**注意**：段文件只有 16 个固定槽位、且按序删除最旧 —— 碎片风险极低（属于"少量长寿命文件反复创建/删除"，LittleFS 的 metadata pair 轮换能很好处理） |

**结论：容量与寿命均有 10 倍以上余量。** 真正的风险不是磨损，而是"把 INFO 也写进 Flash"——那会使写入量放大 5–10 倍并挤占 WARN 的环形空间。**坚持"INFO 不落 Flash"。**

### ~~11.7 必须落 Flash 的 INFO 例外（仅 3 条）~~ —— 🔴 **整节作废（P1 撤销）**

> **本节已被 P1 契约冻结文档 §1 撤销。**
>
> **撤销理由（评审意见）**：这不是"这三条事件有没有价值"的问题，而是
> **一旦引入 Level 之外的持久化例外，"Level 决定策略"这一不变量就失效**。
> 后续每一条 INFO 都会有人来申请例外，最终退化为逐事件人工配置：
>
> ```
> INFO
>  ├── 普通 INFO：不存
>  ├── Provision INFO：存
>  ├── Time INFO：存
>  ├── 某个 Workflow INFO：觉得也挺重要……
>  └── 又加一个例外……
> ```
>
> **v1 不引入 Level 之外的任何持久化例外规则。** 记录格式中的 `persist` 位已删除（§7.2）。
>
> **例外需求的正确归属**："配网过程中掉电导致信息丢失"属于
> **Boot / Provision 状态持久化**，应作为**独立机制**设计（例如一个小的
> provisioning 状态文件），**不得通过给 Log Level 开例外来实现**。
>
> 以下为被撤销的原内容（保留仅供追溯，**不得实施**）：
>
> | 事件 | 原理由 |
> |---|---|
> | ~~`LOG_WIFI_PROVISION_ENTER/DONE`（0x0606/0607）~~ | ~~配网期间无网，只能联网后补发~~ |
> | ~~`LOG_TIME_VALID_ENTER`（0x0803）~~ | ~~"无时间→有时间"的转折点~~ |

**P1 后的正确行为**：这 3 条事件改为**普通 INFO**（Flash NO / Cloud YES）。
§9 分类表中它们的 "Flash" 列应读为 **NO**。

---

## 12. Cloud Log Upload

### 12.1 批次策略

| 项 | 值 | 理由 |
|---|---|---|
| 批大小 | **≤ 16 条** | 16 × 128 B ≈ 2 KB 原始；CBOR 后 ≈ 1.2–2 KB |
| 触发条件 | 累积 ≥ 8 条 **或** 距上次发送 ≥ 5 s **或** 有 CRITICAL **或** 重连后立即 | 平衡延迟与开销 |
| 最大 payload | **4096 B** 硬上限（同时受 `cloud_msg_limit` 约束） | 远低于 `CLOUD_MSG_LIMIT_MAX 8128`；也低于 `CLOUD_MQTT_BUFFER_SIZE 8192` |
| 是否需要分片 | **不需要**。若某批序列化后仍超 4096 B（理论上不会），**减小批大小重编码**，而不是分片 | 分片会引入 BEGIN/DATA/END 三段协议与云端重组，对日志批次属过度设计。`cloud_publish_fragmented` 保留为安全网但不启用 |
| 发送频率上限 | **最快 1 批 / 500 ms** | 防止 Log 洪峰冲击 MQTT 与内网；满环（496 条 ≈ 31 批）**典型**约 16 s 排空（**非承诺**） |
| QoS | **1** | 与现有一致 |
| `store` 参数 | **0**（不持久化到 esp-mqtt outbox） | 耐久性由 Log 自己的段环提供；避免双重持久化（§2.2 发现 9） |

### 12.2 编码：CBOR

**结论：正式上行走 CBOR；JSON 仅用于调试期（编译期开关）。**

| 方案 | 体积（16 条估计） | 评价 |
|---|---|---|
| JSON 文本 | ≈ 4.5–6 KB | ❌ 超 4096 预算，且字段名重复 16 次 |
| **CBOR 定序数组** | ≈ 1.2–1.6 KB | ✅ **推荐** |
| CBOR 整数键 map | ≈ 1.6–2.0 KB | ✅ 可接受，调试更友好 |
| 原始 128 B 打包 | ≈ 2 KB | 最小，但云端要硬编码 Flash 布局 → schema 演进不友好 |

**推荐：CBOR 定序数组**。每条记录编码为固定顺序的 CBOR 数组：

```
record = [ version, level, flags, boot_seq, event_id, uptime_ms,
           timestamp, params, ctx ]
params = [ [param_id, param_type, value], ... ]   // 数组更省，无需重复 key
ctx    = null | [ ctx_kind, <ctx bytes> ]
```

批次外层用**整数键 map**（字段少，可读性收益大于体积成本）：

```
{
  0: 1,              // fmt  = 1        （批次格式版本）
  1: <event_dict_ver>,// 事件字典版本
  2: <boot_seq>,      // 本批记录所属 boot_seq
  3: <seq_from>,      // 本批最小 seq（含）
  4: <seq_to>,        // 本批最大 seq（含）
  5: <count>,         // 本批记录数
  6: <drop_ring>,     // 累计"RAM 环丢弃"计数（自上次上报）
  7: <drop_overflow>, // 累计"段环淘汰"计数（自上次上报）
  8: <drop_unacked>,  // ★ P1 新增：其中"未确认即被淘汰"的数量
  9: <self_degraded>, // 累计"LogManager 自降级"计数（见 §17）
  10: <flags>,        // ★ P1 新增：bit0 = seq_reliable（0 = 不可靠，可能重复）
  11: [ record, ... ] // 记录数组
}
```

> **P1 修订**：`fmt` 由 1 升为 **2**（Record v2 布局 + 新增两个字段）。

**为什么批次头要携带丢弃计数**：因为丢弃本身就是**不可落盘的事件**
（`LOG_LOG_RING_OVERFLOW` 无法写进已满的环），必须用"下一批的头部"这一侧信道传递。
这是本设计对"LogManager 自身故障不得递归 Logging"的一个具体回应。

> **P1 新增字段说明**
> - `drop_unacked`：区分"已确认后被淘汰"（无害）与"未确认就被淘汰"（真丢数据）。
>   这是评估 §13.2 `FORCE_ADVANCE` 是否需要引入的**关键实测指标**。
> - `flags.bit0 seq_reliable`：§7.4 的 seq 区间预留若因 Flash 不可用而失败，
>   该位置 0，云端对该批按"可能重复"处理。

### 12.3 与现有 `cloud_compress_uplink` 的关系

**Log 批次绝不走 `cloud_compress_uplink()`。**

理由：该函数的逻辑是"按字段名映射缩写"（`status→s`、`command→c`…），是为**业务 JSON** 设计的。Log 是二进制 CBOR，送进去会被 `deserializeJson` 拒绝（返回 false），然后被当作"未压缩的原文"直发 —— 结果就是把 CBOR 当文本发出去。

**因此 Log 必须走独立的二进制发布路径**，见 §18。

---

## 13. ACK / Retry / Dedup

### 13.1 ACK 模型

| 项 | 设计 |
|---|---|
| 载体 | `down` Topic（不新增订阅） |
| 消息 | `{"c":"log_ack","i":"<云端生成唯一id>","p":{"b":<boot_seq>,"f":<seq_from>,"t":<seq_to>}}` |
| 粒度 | **批次级**（`seq_from`..`seq_to` 连续区间） |
| 语义 | "该区间的记录已被云端**持久化接受**" |
| 设备反应 | 若 `b == 当前 boot_seq` 且 `f <= 未确认最小 seq` ⇒ 推进 `ack_seg`（删除已完全确认的段文件） |

**为什么是批次级而不是单条级**：

1. 批次内的 `seq` **按构造就是连续的**（FIFO 组批），因此一个区间即可完整表达，无需逐条位图。
2. 单条级 ACK 需要云端对每条产生应答 → 16 倍消息量，且需要位图或稀疏集合 → 复杂度与带宽都变差。
3. 单条级 ACK 会让"部分确认"成为常态，而本设计的 tail 是**段级**的 —— 部分确认无法映射到"删除哪一段"，反而需要额外的位图持久化。**批次级与段级删除天然对齐。**

### 13.2 ACK 丢失 / 超时 / 重传

```
状态机（非阻塞，全部在 log_task() 内推进）

IDLE
  │ 有可发批次且 cloud 在线
  ▼
SENDING ──(publish 成功)──► WAIT_ACK（记 send_ms、retry_n=0）
  │                              │
  │(publish 失败)                 │ 收到 log_ack（覆盖本次 seq 区间）
  ▼                              ├──► 推进 ack_seg → IDLE
RETRY_WAIT ◄─────────────────────┤
  │ 退避到期                      │ 超时 ACK_TIMEOUT_MS（默认 15 s）
  └──► SENDING（retry_n++）       ▼
                            retry_n++ → SENDING（**seq 不变**，幂等重传）
                                 │
                                 │ retry_n >= MAX(5)
                                 ▼
                       ┌──────────────────────────────────┐
                       │ 【P1：v1 默认路径】               │
                       │ GIVE_UP_NOT_ADVANCE              │
                       │   保留记录，不推进 tail           │
                       │   继续正常 FIFO 发送后续记录       │
                       │   段环满时该段被 FIFO 淘汰         │
                       │   → drop_unacked++               │
                       └──────────────────────────────────┘
                                 │
                                 │ （可选策略，v1 默认**关闭**）
                                 │ retry_n >= MAX 且 最旧未确认已超 STALE(30 min)
                                 ▼
                            FORCE_ADVANCE：推进 ack_seg + 计数 ack_lost + 发
                            LOG_LOG_ACK_LOST（WARN）→ IDLE
```

| 参数 | 建议值 | 说明 |
|---|---|---|
| `ACK_TIMEOUT_MS` | 15000 | 远超一次 MQTT 往返 |
| `MAX_RETRY` | 5 | |
| 退避 | 2 s → 4 s → 8 s → 16 s → 32 s（上限 60 s） | 避免与 MQTT 重连风暴叠加 |
| ~~`STALE_MS`~~ | ~~1800000（30 min）~~ | 🔴 **P1 撤销**：不进入 v1 核心协议 |

#### 🔴 P1 修订：`FORCE_ADVANCE` 降级为可选恢复策略（**v1 默认关闭**）

> **评审意见**：它等于设备单方面决定"我不知道云端收没收到，但我等够了，
> 所以把本地唯一副本删了" —— 这把系统从"**尽可能不丢 WARN+**"变成
> "**某些情况下主动允许丢 WARN+**"。这是**产品可靠性策略**，不是技术细节，
> 在云端真实跑起来并拿到实测数据前**不应冻结**。

| | v1 默认 | 可选策略（未冻结） |
|---|---|---|
| 行为 | **GIVE_UP_NOT_ADVANCE**：保留记录，不推进 tail | `FORCE_ADVANCE`：强制推进 tail |
| 是否丢数据 | 仅在**段环满被 FIFO 淘汰**时丢（有上界） | 主动删除未确认段 |
| 可观测性 | `drop_unacked` 计数 | `ack_lost` 计数 |
| 配置 | 无（默认行为） | 独立开关，**默认 false** |

**为什么 v1 默认路径不会死锁**：ACK 永不返回时，未确认段保留在环里，
但**新记录继续写入、继续发送**，段环满时最旧段被 FIFO 淘汰
（无论是否确认）→ 计入 `drop_overflow` 与 `drop_unacked`。
因此最坏后果被**环容量（496 条）自然限界**，且有计数可见 —— **不会卡死**。

**何时再评估**：云端真实运行 ≥ 2 周后，依据实测 `drop_unacked` 决定
是否引入 `FORCE_ADVANCE`（或改为"未确认段保留更久 / 增大环"）。
**若引入，必须作为独立可配置项，默认关闭。**

> 注：无论采用哪条路径，Cloud 侧**都必须**能用 `(device_id, boot_seq, seq)` 幂等去重
> —— 因为 §5 已确定传输语义是 **at-least-once**（见 §13.4）。

### 13.3 与 MQTT QoS1 / 现有 `c:"ack"` 的关系

| 层 | 机制 | 保证 | 是否新增 |
|---|---|---|---|
| L1 传输 | MQTT PUBACK（QoS1 + `store`） | 送达 **Broker** | 否（已有） |
| L2 协议受理 | `c:"ack"`（`result:"received"`） | 命令已受理 | 否（已有，**Log 不使用**） |
| L3 业务结果 | `c:"result"`（CLOUD_MSG_RESULT） | 业务执行完毕 | 否（已有，Log 不使用） |
| **L4 Log 持久化确认** | **`c:"log_ack"`** | 云端**已持久化** | **✅ 新增（仅此一项）** |

**明确不叠加**：Log 批次**不**触发 `c:"ack"`。理由：`c:"ack"` 的语义是"命令已受理"，对日志批次无意义；叠加会造成"一条 Log 批产生 2 条上行"的反向浪费（需求 §六"不为了理论上的完美增加过度复杂机制"）。

### 13.4 去重（云端职责，设备端配合）

| 项 | 内容 |
|---|---|
| 幂等键 | `(device_id, boot_seq, seq)` |
| 设备端保证 | ① 同一记录**重传时 seq 不变**；② `seq` 全局单调递增（跨重启不重复，除 Flash 全损重建的极端情况）；③ 一次会话内不重排 |
| 云端职责 | UPSERT（唯一索引）+ 忽略已存在记录 |
| 为什么不用 `mid`/消息 id | MQTT 消息 id 由 esp-mqtt 分配，跨重连会复用，不适合做业务幂等键 |
| `boot_seq` 回绕 | 🔴 **P1 修订：`boot_seq` 由 uint16 升级为 uint32**（记录内 +2 B，128 B 仍有 16 B reserved，不为 2 B 牺牲幂等键的生命周期）。4.29e9 次开机，无需考虑回绕 |

### 13.5 🔴 P1 新增：正式传输语义契约

```
Device:  at-least-once delivery（至少一次投递）
Cloud:   idempotent deduplication（幂等去重）
```

**必须显式声明**（防止后续误解）：

| 断言 | 正误 |
|---|---|
| ACK 表示"云端已持久化该 seq 区间" | ✅ 正确 |
| ACK 表示"该区间不会再次出现" | ❌ **错误** |

**同一区间会被重复投递的场景**（云端必须能接受）：

1. 设备重启后重放**未确认段**（§14.1）
2. ACK 在网络中丢失后的**重传**（§13.2，重传时 `seq` 不变）
3. 同一 seq 记录跨段时，段未完全确认前的多次发送

> **不得让后续实现者把 ACK 误解为 exactly-once。**
> 本设计不提供 exactly-once；去重责任在云端，依据是 `(device_id, boot_seq, seq)`。

---

## 14. Offline / Reconnect

### 14.1 三种网络状态的行为

| 状态 | INFO | WARN / ERROR / CRITICAL |
|---|---|---|
| **在线** | RAM 环 → 组批 → CBOR → Log Topic（不落 Flash） | RAM 环 → 落 Flash（批量 / CRITICAL 立即）→ 组批 → 发送 → 等 ACK → 删段 |
| **离线（MQTT 断）** | RAM 环 → 溢满即丢（`drop_ring++`）；**不落 Flash** | RAM 环 → 落 Flash；ACL 断则 RAM 环内等待 |
| **重连** | **先补发 Flash 未确认段（按 seq 升序）**，补发完成后再发新 INFO | 与补发一起来自 Flash；新产生的 WARN 追加到段尾（自然排在后面） |

### 14.2 需求 §十二 逐条回答

| 问题 | 回答 |
|---|---|
| Flash 中旧 Log 与新 Log 的发送顺序 | **严格按 `seq` 升序 FIFO**。旧（Flash 未确认）→ 新。理由：批次 ACK 依赖区间连续性；乱序会让区间断裂，被迫退化为逐条 ACK |
| 新产生 Log 是否需要优先发送 | **不需要**。理由：Flash 补发量有硬上限（**最多 496 条 = 31 批**，P1 段容量冻结后），在 500 ms/批的限速下约 16 s 排空（**仅为典型值，非承诺**）。为"优先"引入乱序，代价（区间断裂 + 逐条 ACK + 云端乱序处理）远大于收益 |
| CRITICAL 是否需要抢占 | **不抢占（不新建路径）**，但**立即 flush 落盘 + 提前触发当前批次发送**。⚠️ P1 措辞修正：这是"**进入最高优先级的下一发送批次**"，**不是**"立即发送"；**不承诺秒级**。理由见 §15.3 |
| Flash backlog 太大怎么办 | 不可能"太大"：段环**硬上限 496 条**（P1 冻结 31 条/段 × 16 段），超出即淘汰最旧段并计入 `drop_overflow` / `drop_unacked`。云端会在下一批的头部看到该计数。**这是"有意限流"，不是缺陷** |
| 云端 ACK 失败怎么办 | 见 §13.2：重试 5 次 → **v1 默认保留记录不推进**（`GIVE_UP_NOT_ADVANCE`）；段环满时 FIFO 淘汰并计 `drop_unacked`。`FORCE_ADVANCE` 为**可选策略，默认关闭** |
| 网络频繁上下线怎么办 | ① 发送频率上限 500 ms/批，天然抑制风暴；② 每次 `MQTT_EVENT_CONNECTED` 只重置一次"会话"，不重置 `seq`；③ **不做"每次重连都全量重发"** —— 只发未确认部分（幂等 seq 保证云端可去重） |

### 14.3 Offline 期间 RAM 环的行为（唯一可能丢数据的地方）

| 级别 | RAM 环满时的处置 | 是否计数器 | 理由 |
|---|---|---|---|
| INFO | **最先丢弃**（`drop_ring++`） | ✅ | INFO 不上云不落盘，属"尽力而为" |
| WARN | 其次丢弃（`drop_ring++`） | ✅ | 但注意：WARN 已**先落 Flash**，所以 RAM 里的副本可丢 |
| ERROR | 再次 | ✅ | 同上（已落 Flash） |
| CRITICAL | **最后丢弃** | ✅ | 同上；且 CRITICAL 率极低，实际上永远轮不到它被丢 |

**关键点：WARN+ 在"入 RAM 环"之后**立即**尝试落 Flash**，因此 RAM 环满导致的丢失**不影响耐久性**（Flash 里已有）。RAM 环只是"待发送缓冲区"，不是"唯一副本"。这是本设计能容忍"队列满即丢"的根本原因。

> 反过来说：**如果 Flash 不可用**（LittleFS 损坏），WARN+ 就真的只剩 RAM 副本了。此时应发 `LOG_LOG_SELF_DEGRADED`（ERROR，通过下一批头部侧信道或串口），并**放宽 RAM 环的丢弃策略**（改为"只丢 INFO，绝不动 WARN+"）—— 这是一个明确的降级分支，需在编码阶段实现。

---

## 15. Priority

### 15.1 五级在四个位置的优先级关系

| Level | RAM 环 | Flash 段环 | Cloud 批次 | MQTT TX | 落盘 | 上云 |
|---|---|---|---|---|---|---|
| **DEBUG** | 不入队（编译期可整体关闭） | ❌ | ❌ | ❌ | NO | NO |
| **INFO** | 入队（可被丢弃） | ❌ | 普通批次 | 限速 500 ms/批 | NO | YES |
| **WARN** | 入队（落盘后可丢副本） | ✅ 批量 | 普通批次 | 同上 | YES | YES |
| **ERROR** | 入队（落盘后可丢副本） | ✅ 批量 | **可提前 flush**（不等 5 s） | 同上 | YES | YES |
| **CRITICAL** | 入队（最后丢弃） | ✅ **立即单条** | **进入最高优先级下一批次** | 同上 | YES | YES |

> **🔴 P1 冻结**：上表即**唯一**的 Level 策略。
> - **无任何 EventId 级例外**（撤销原 §11.7 的 3 条 INFO 例外）。
> - **无 `persist` 覆盖机制**（该位已从 Record 删除）。
> - CRITICAL 的 Cloud 列是"**下一批优先**"，**不是**"立即发送"，**不承诺秒级**（§15.3）。

### 15.2 编译期级别裁剪

```c
// platformio.ini（建议）
-DLOG_LEVEL_MIN=1        // 0=DEBUG 1=INFO 2=WARN 3=ERROR 4=CRITICAL
                         // 生产固件建议 1（保留 INFO 上云）；排障时设 0
```

`LOG_DEBUG(...)` 在 `LOG_LEVEL_MIN > 0` 时**展开为空**（不产生代码、不产生字符串常量）。这是"避免业务代码大量 Serial 输出"（需求文档 §5）最有效的手段 —— 比运行期判断省 Flash。

### 15.3 CRITICAL 是否应该拥有独立发送路径 —— 明确回答：**不需要**

> **⚠️ P1 措辞修正**：原第 1 条给出"≤16 秒"的端到端上界，容易被误读为 SLA。
> 实际上 CRITICAL 是"**尽快进入发送调度**"，不是"**立即发送**"。
> **本设计不承诺任何秒级端到端延迟。**

**结论**：CRITICAL **不绕过普通 Log 队列**，也不新建 MQTT 路径。

| 维度 | 行为 |
|---|---|
| **Flash** | **立即**（单条 flush，不等 8 条 / 5 s 批量窗口） |
| **Cloud** | **进入最高优先级的"下一发送批次"**（提前 flush 当前待发批次） |
| **独立 MQTT 通道** | ❌ **不做** |
| **端到端延迟** | ❌ **不承诺秒级** |

**措辞约束（防止后续误读）**：

- ✅ 正确表述：CRITICAL **尽快进入发送调度**。
- ❌ 错误表述：CRITICAL **立即发送** / **秒级到达**。

**理由（四条）**：

| # | 理由 |
|---|---|
| 1 | **排空时间已被环容量限界**。Flash 段环硬上限 **496 条**（P1 冻结）；受 500 ms/批 + 16 条/批限速，典型排空约 16 s —— **这是参考值，不是保证**，真实延迟取决于 backlog 深度与网络质量 |
| 2 | **独立路径会破坏 `seq` 连续性**，使批次 ACK 的"区间"模型失效 → 必须引入逐条 ACK 或位图 → 复杂度与带宽同时劣化 |
| 3 | **独立路径需要独立的容量与独立的持久化语义**，而 CRITICAL 的发生率是 **0–2 次/日**，为它建专用通道属于典型的过度设计（需求 §十三"避免过度设计"） |
| 4 | **CRITICAL 已立即落 Flash**。即使上传被延迟或失败，**证据不会丢**。上传只是"让云端尽快知道"，不是"保证不丢" —— 而"不丢"已由"立即落盘"满足 |

**如果未来产品确实要求"CRITICAL 优先到云"**，本设计给出**唯一推荐的最小扩展**（供人工决策，默认不实现）：

> 在 CRITICAL 发生时，**把当前批次连同这条 CRITICAL 一起发出**（即"提前 flush"），并允许**已经排好队的旧批次继续**。也就是"提前发送**新的**批次"，而不是"抢在旧批次之前"。这样 `seq` 区间仍然是"两个不相邻但各自连续的区间"，ACK 仍可表达，只是云端要先缓存一个空洞。
>
> 即便如此，这仍然是"**下一批优先**"，不是独立的 MQTT 通道，也不构成延迟承诺。

---

## 16. Task / Thread / Non-blocking

### 16.1 `log_emit()` 的契约（必须在头文件中逐条写明）

| 属性 | 结论 |
|---|---|
| **阻塞** | **绝不阻塞**。只做：级别过滤、128 B 组装、memcpy 入环、计数器。**无 malloc、无 Flash、无 MQTT、无 Serial、无 `delay()`、无等待** |
| **执行时间** | 上界约 **2–5 µs**（含一次 128 B memcpy + 短临界区） |
| **Task 安全** | ✅ 是。用**临界区**（`portMUX_TYPE` spinlock，与 `system_command.cpp` 的 Critical Operation 计数同一范式）保护环的 `head/count` |
| **ISR 安全** | ❌ **明确不支持，且禁止调用**。理由：组装 Record 需要读时间/配置（非 ISR 安全），且临界区在 ISR 中会引入延迟。**ISR 侧应改为 `event_push()`，由主循环记 Log** |
| **回调上下文** | ⚠️ **允许但有约束**。仅当参数**全部为标量**（无 STR）且**不读取任何返回 `String` 的 getter**（如 `state_get_string`）时可调用。理由：`String` 会堆分配，在 BLE/MQTT 回调中不可接受 |
| **重入** | ❌ 禁止。由 `s_in_emit` 守卫（§17） |
| **可调用者** | loopTask（绝大多数模块）、esp-mqtt 任务（Cloud/Command 结果路径）。这两者都必须满足"标量参数"约束 |

### 16.2 各潜在调用点的判定

| 上下文 | 是否允许 `log_emit()` | 说明 |
|---|---|---|
| `loop()` 内的各 `*_task()`（workflow/valve/weight/config/time） | ✅ 推荐 | 主力调用场景 |
| `dispense_guard.cpp` 的 `event_callback`（由 `event_dispatch()` 在 loopTask 调用） | ✅ 推荐 | **这是强制关阀 CRITICAL 的最佳落点** |
| `MiThermometer.cpp` 的 `onResult()`（NimBLE 回调任务） | ⚠️ **禁止直接调用** | 改为：只置标志/推 Event，由 `MiThermometer_task()` 记 Log。原因：NimBLE 回调内已有"逐字节 `Serial.printf`"的历史问题（审计 §16.7），不得再叠加任何非必要工作 |
| esp-mqtt event handler | ⚠️ **禁止** | 同上；改为置 `mqtt_disconnect_pending` 一类标志，由 `cloud_task()` 记 Log（**现有代码已经是这个范式，直接沿用**） |
| HX711 / `weight.cpp` 的采样路径 | ❌ | 高频；且已有 `weight_refresh_error_state()` 的**边沿触发**在 loopTask 中执行 —— 在那里发 Log |
| Timer / ISR | ❌ | 见 §16.1 |
| `workflow.cpp` 的 Action `poll()` 回调 | ⚠️ 谨慎 | Action 的 `poll()` 在 loopTask 执行 → ✅ 允许，但只允许"状态迁移点"（FAILED），不允许每次 poll |

### 16.3 是否需要独立 Log Task —— 明确回答：**不需要，且不应该**

| 方案 | 评价 |
|---|---|
| **在现有 `loop()` 中调用 `log_task()`（推荐）** | ✅ 与项目"单一 loopTask 驱动一切"的架构一致（AI_RULES §4/§6）；**保持 LittleFS 单写者** |
| 新建独立 Log Task | ❌ **危险**。ConfigManager、WorkflowStorage、BinStorage 全部在 loopTask 中访问 LittleFS。LittleFS 的 Arduino 封装**不是线程安全的**。新建任务会引入真实的并发损坏风险，需要全项目加锁，属大规模重构 |

**LittleFS 单写者**是本设计最重要的并发结论，应写入头文件注释。

### 16.4 非阻塞的具体保证

`log_task()` 每次调用的工作量上界：

```
log_task():
  ├─ ① 时钟/状态维护                      ~1 µs
  ├─ ② 取出 RAM 环中至多 1 批（≤16 条）    ~2 µs
  ├─ ③ 若需要落 Flash：写 ≤1 段（≤4 KB）   最坏 ~10–50 ms  ← 唯一的长耗时点
  ├─ ④ 若需要上传：CBOR 编码 + publish     ~1–3 ms
  └─ ⑤ 处理 ack 超时                       ~1 µs
```

**约束（必须实现）**：

| 约束 | 值 | 目的 |
|---|---|---|
| 每轮最多写 **1** 个段 | 1 | 保证单轮 loop 有界 |
| 两次 Flash 写的最小间隔 | **20 ms** | 避免连续 loop 都在写 Flash |
| CRITICAL flush **不受**间隔限制，但仍受"每轮最多 1 次写"限制 | — | 兼顾及时性与有界性 |
| 每轮最多发 **1** 批 | 1 | 同上 |
| 两次发送的最小间隔 | **500 ms** | 抑制 Log 洪峰 |
| **绝不**在任何路径等待 ACK | — | 非阻塞铁律 |

**对既有实时性的影响评估**：

| 关注点 | 评估 |
|---|---|
| 阀门安全超时（`valve_task`） | 最坏被延迟 ~50 ms，而安全超时量级为**秒** ⇒ 可忽略 |
| Workflow Step 时序 | Step 之间是 ms–s 量级；50 ms 抖动可接受 |
| HX711 采样（10 SPS = 100 ms/样本） | 50 ms 延迟 < 采样周期 ⇒ 不丢样本 |
| BLE 扫描窗口 | 「扫描窗口」为秒级 ⇒ 可忽略 |
| **结论** | ✅ 不影响非阻塞契约。若未来发现 50 ms 仍偏大，可**把段从 4 KB 减小到 2 KB**（约 20 ms），或改用 `bin_storage_write_chunk` 把一个段拆成多轮写入 |

### 16.5 并发访问的临界区（唯一需要加锁的地方）

```
保护对象：RAM 环的 head / count（生产者 = log_emit，消费者 = log_task）
手段：portMUX_TYPE + portENTER_CRITICAL / portEXIT_CRITICAL
粒度：仅包住"判断满 + memcpy + 推进 head"，不含级别过滤
       （过滤在锁外做，因为它只读配置）
最坏持锁时间：一次 128 B memcpy ≈ 1 µs 以内
```

**不需要加锁的地方**：Flash 写入、CBOR 编码、MQTT 发布 —— 全部只在 loopTask 中执行，天然串行。

---

## 17. Recursive Logging Protection

### 17.1 递归是怎么发生的（三个真实路径）

```
路径 1：Log 自身 Flash 写失败
  log_task → BinStorage::write 失败 → BinStorage 打 E 级日志
            → 若 BinStorage 的 log_callback 指向 LogManager → log_emit → 又写 Flash → 又失败 → ...

路径 2：Log 上传失败
  log_task → upload_cb → cloud_send_route → publish 失败
            → 若 CloudManager 的错误日志指向 LogManager → log_emit → 又尝试上传 → ...

路径 3：Log 编码失败
  log_task → CBOR 编码失败 → log_emit(LOG_LOG_SELF_DEGRADED) → 又编码 → 又失败 → ...
```

### 17.2 四层防护（全部必须实现）

#### 防护 1：`log_emit()` 重入守卫（最外层）

```c
static volatile bool s_in_emit = false;

bool log_emit(...) {
    if (s_in_emit) { s_stats.reentrant_drop++; return false; }  // ← 计数，绝不递归
    s_in_emit = true;
    ...  // 过滤 / 组装 / 入环
    s_in_emit = false;
    return ok;
}
```

**语义**：`log_emit()` 的**任何**内部路径都不得再调用 `log_emit()`。违反即被静默计数丢弃。O(1)，无栈增长风险。

#### 防护 2：I/O 区不产生 Log，只置"待报"标志

`log_task()` 中的 Flash 写 / CBOR 编码 / 上传三段，**不得**直接调用 `log_emit()`：

```c
typedef enum {
    LOGPEND_NONE = 0,
    LOGPEND_FLASH_WRITE_FAIL,
    LOGPEND_FLASH_DEGRADED,
    LOGPEND_ENCODE_FAIL,
    LOGPEND_UPLOAD_FAIL,
    LOGPEND_ACK_LOST,
    LOGPEND_RING_OVERFLOW,
} LogPendingSelfReport;

static uint8_t s_pending_self_report = LOGPEND_NONE;
static uint32_t s_pending_self_count = 0;
```

流程：

```
① log_task 开头（**I/O 区之外**）：
     if (s_pending_self_report != LOGPEND_NONE) {
         log_emit(映射的事件ID, {count}, 1);     // ← 安全：此时不在 I/O 区
         s_pending_self_report = LOGPEND_NONE;
     }
② I/O 区（Flash / CBOR / publish）：
     失败 → 只设 s_pending_self_report + s_pending_self_count++，**不调 log_emit**
```

**为什么放到"下一轮开头"而不是"本轮末尾"**：避免同一轮内"刚发完自定义事件又要发送"的时序纠缠，且保证 `s_in_emit` 一定是 false。**代价是本轮自故障延后一轮上报（≤ 一次 loop），完全可接受。**

#### 防护 3：LogManager **不注册**存储层/云层的文本日志回调

| 回调 | 是否接入 LogManager | 理由 |
|---|---|---|
| `bin_storage_set_log_callback` | ❌ **不接入**（继续指向 `bin_log_serial`） | 若接入，路径 1 会立刻成环。且 BinStorage 的文本日志是**裸文本**，没有结构化价值 |
| `json_storage_set_log_callback` | ❌ 不接入（建议新增指向 Serial，见 §23-Q2） | 同上 |
| `file_storage_set_log_callback` | ❌ 不接入 | 同上 |
| `config_manager` 的 `cfg_log` | ❌ 不接入 | 结构化版本由 ConfigManager **自己**用新 API 发（`LOG_CFG_*`） |
| `command_manager` 的 `command_log` | ❌ 不接入 | 同上（`LOG_CMD_*`） |
| `cloud_manager` | ❌ 不接入 | 同上（`LOG_MQTT_*`） |

**核心原则**：**Log 的采集是"模块主动结构化上报"，不是"劫持其他模块的文本日志"。** 这同时解决了三个问题：递归、双份日志、以及文本无法结构化。

#### 防护 4：故障状态用"侧信道"而非自记

无法落盘/无法发送的故障（如段环溢出、ack 丢失）通过**下一批的批次头**携带（§12.2 的 `drop_ring`/`drop_overflow`/`self_degraded`），以及 `log_get_stats()` 供串口诊断查询。

**如果永远无法发送**（长期离线）→ 这些计数保留在 RAM，并在下一次 CONNECTED 后的第一批带走。**极端情况（设备从未联网且环已满）→ 计数在 RAM 中累加，重启丢弃** —— 这是可接受的，因为此时**设备本来就无法上报任何东西**。

### 17.3 边界：LogManager 启动期不得产 Log

`log_init()` 在 `config_init()` 之后、`cloud_init()` 之后执行。此前发生的启动日志（LittleFS 挂载、Config 加载、Workflow 池分配）**无法进入 LogManager**。

**对策**：这些信息由**事后的聚合记录**补偿：

- `LOG_SYS_BOOT_COMPLETE`（0x0101）带 `boot_seq / reset_reason / init_ms / loaded / total / count` —— 覆盖"启动整体是否成功"。
- 启动期各模块的**失败**由 `LOG_SYS_INIT_FAILED` / `LOG_SYS_FS_MOUNT_FAILED` 在 `setup()` 末尾**补发**（把启动期的失败原因暂存在静态变量里，`log_init()` 后统一补发）。

**这是一条明确的实现要求**：`main.cpp setup()` 需要一个小的"启动失败暂存"（≤4 项），由 `log_init()` 之后统一补发。否则"启动失败"这类最重要的诊断会**永久丢失**。

---

## 18. CloudManager 最小改造

### 18.1 现有接口（**一个都不改**）

```c
// cloud_manager.h —— 以下签名与行为全部保持不变
void cloud_init();
void cloud_task();
bool cloud_send_set(const char* message);
bool cloud_send_up(const char *message);
bool cloud_upload_json(JsonDocument& doc);
bool cloud_is_connected();
```

### 18.2 需要新增（纯 additive，4 处）

#### 新增 1：`CloudRoute` 枚举 + 主题访问

```c
// cloud_manager.h
enum CloudRoute {
    CLOUD_ROUTE_UP  = 0,   // guo_feeder/up   （现有业务上行，语义不变）
    CLOUD_ROUTE_LOG = 1    // guo_feeder/log  （新增，仅 LogManager 使用）
};
```

```c
// cloud_manager.cpp 内部
static String mqtt_log_topic;   // cloud_init() 中从 config 读取

static const String& cloud_topic_of(CloudRoute route)
{
    return (route == CLOUD_ROUTE_LOG) ? mqtt_log_topic : mqtt_pub_topic;
}
```

#### 新增 2：二进制发布入口（**补上项目缺失的能力**）

```c
// cloud_manager.h
// 以二进制 payload 发布到指定 route。
//   · route 未配置 / MQTT 离线 → 返回 false（**不排队、不重试**，由调用方负责）
//   · qos 固定 1；store 固定 0（耐久性由调用方保证，避免 esp-mqtt outbox 双重持久化）
bool cloud_send_route(CloudRoute route, const uint8_t* data, size_t len);
```

```c
// cloud_manager.cpp
bool cloud_send_route(CloudRoute route, const uint8_t* data, size_t len)
{
    if (data == nullptr || len == 0) return false;
    if (!mqtt_connected)             return false;
    const String& topic = cloud_topic_of(route);
    if (topic.length() == 0)         return false;
    if (len > cloud_msg_limit)       return false;   // 调用方负责减小批次
    return cloud_mqtt_publish_binary_nostore(topic, data, len, 1);
}
```

> 说明：需要一个 `store=0` 的内部发布函数。现有的 `cloud_mqtt_publish_binary()` 硬编码 `CLOUD_MQTT_STORE`，它是 **static 内部函数**，因此**新增一个变体不算改公共接口**（不违反"签名保持不变"）。

**同时建议（可选，与 Log 独立）**：把已经声明但未实现的 `cloud_send_up_binary` / `cloud_send_up_cbor` / `cloud_send_set_cbor` **要么实现、要么从 `.h` 中删除**。留着"声明存在但永不定义"的接口会误导后续开发者（本报告在 §2.2 发现 2 已记录）。

#### 新增 3：Log ACK 下行回调

```c
// cloud_manager.h
typedef void (*CloudLogAckCallback)(
    uint16_t boot_seq,
    uint32_t seq_from,
    uint32_t seq_to
);
void cloud_set_log_ack_callback(CloudLogAckCallback cb);
```

#### 新增 4：`down` 分发中的 `log_ack` 分支

```c
// cloud_manager.cpp :: cloud_process_rx_message()
// 位置：紧跟现有 change_msg_limit 分支之后（:1075 之后）

if (strcmp(c, "log_ack") == 0)
{
    if (s_log_ack_cb != nullptr)
    {
        JsonVariant p = compact ? doc["p"] : doc["pl"];
        s_log_ack_cb(
            (uint16_t)(p["b"] | 0),
            (uint32_t)(p["f"] | 0),
            (uint32_t)(p["t"] | 0));
    }
    return;    // 不进 CommandManager，不回 c:"ack"
}
```

**为什么放在这里而不是 CommandManager**：与 `change_msg_limit` 完全同理 —— 这是**协议级**消息，不属于业务命令，不应污染 CommandManager 的命令空间与 `cmd_id` 去重缓存。

### 18.3 依赖注入（在 `main.cpp` 中完成，两个方向都不 include）

```c
// main.cpp
static void cloud_log_upload_adapter(const uint8_t* data, size_t len)
{
    cloud_send_route(CLOUD_ROUTE_LOG, data, len);   // 唯一的适配点
}

void setup()
{
    ...
    command_manager_init();
    command_manager_set_log_callback(command_log_serial);
    cloud_init();

    // ---- LogManager：必须在 config_init + cloud_init 之后 ----
    log_init();
    log_set_upload_callback(cloud_log_upload_adapter);   // LogManager → CloudManager（注入）
    cloud_set_log_ack_callback(log_on_ack);              // CloudManager → LogManager（注入）
    ...
    config_boot_validate();

    // 补发启动期事件（§17.3）
    log_emit_boot_summary();
}

void loop()
{
    system_command_task();
    wifi_task();
    event_dispatch();
    time_task();
    cloud_task();
    command_manager_task();
    config_task();
    log_task();               // ← 新增，位置紧随 config_task
    workflow_task();
    ...
}
```

**依赖关系**：

```
        main.cpp
       ╱        ╲
      ▼          ▼
 LogManager ──(函数指针)──► CloudManager
      │                       │
      │ include               │ include
      ▼                       ▼
 bin_storage.h           system_state.h
 time_manager.h          config_manager.h
 config_manager.h        command_manager.h
 system_state.h          capability_registry.h
                        （**不 include log_manager.h**）
```

**结论：不产生循环依赖。** LogManager 不 include `cloud_manager.h`；CloudManager 不 include `log_manager.h`。二者都不知道对方存在，由 `main.cpp` 绑定。

### 18.4 需求 §十七 的"必须修改 / 不应该修改"清单

#### ✅ 必须修改

| 文件 | 改动 | 规模 |
|---|---|---|
| `src/cloud_manager.h` | 新增 `CloudRoute`、`cloud_send_route`、`CloudLogAckCallback`、`cloud_set_log_ack_callback` | +约 25 行（纯新增） |
| `src/cloud_manager.cpp` | 读 `log_topic` 配置；新增 `store=0` 二进制发布；`cloud_process_rx_message` 加 1 个分支 | +约 45 行 |
| `src/main.cpp` | `log_init()` / `log_set_upload_callback()` / `cloud_set_log_ack_callback()` / `log_task()` / 启动失败暂存与补发 | +约 40 行 |
| `src/config_manager.h` / `.cpp` | 新增 `CONFIG_MODULE_LOG`；`CONFIG_MODULE_COUNT` 8→9；`kModuleNames[]` 加一项；`config_get_mqtt_log_topic()` | +约 15 行 |
| `data/config/mqtt.json` | 新增 `log_topic` 字段 | +1 行 |
| `data/config/log.json` | **新建**（§19） | 新文件 |
| `tools/gen_config_version.py` | 无需改（自动扫描 `data/config/*.json`） | 0 |
| **新建** `src/log_manager.h` / `src/log_manager.cpp` / `src/log_events.h` | 新模块 | — |
| 各业务模块 | 在**状态迁移点**插入 `log_emit(...)`（分期进行，见 §25） | 每模块 +5~20 行 |

#### ❌ 不应该修改

| 文件 / 内容 | 理由 |
|---|---|
| `cloud_send_up` / `cloud_send_set` / `cloud_upload_json` 的签名与实现 | 需求 §十七"不因为 LogManager 而改变现有 Up 协议语义" |
| `cloud_compress_uplink()` | 它是业务 JSON 的压缩器，Log 不走它 |
| `cloud_mqtt_publish_text()` / `cloud_mqtt_publish_binary()` | 现有行为被业务依赖；只**新增** `store=0` 变体 |
| `cloud_publish_fragmented()` | Log 不需要分片 |
| `mqtt_event_handler()` 的订阅逻辑 | 不新增订阅（ACK 走 `down`） |
| `command_manager.cpp` / `command_manager.h` | Log 完全旁路 CommandManager |
| `workflow.cpp` / `workflow_storage.cpp` 的存储与事务逻辑 | 需求 §二十二；只**新增** `log_emit` 调用 |
| `system_state.cpp` / `system_state.h` 的状态表 | 不新增 State（Log 有自己的计数）<br>⚠️ **例外**：若决定把 `log stats` 暴露到 State，需按"新增 State 必改三处"的规范同步改 `system_state.h` 枚举 / `.cpp` state_map / 产生模块。**本设计默认不做**（改用 `log_get_stats()` 诊断接口 + 串口命令） |
| `event_manager.cpp` 的事件表与风暴抑制 | Log 不自动消费 Event；仅在少数点显式订阅（若需要） |
| `critical_operation` 的 acquire/release 配对 | Log 不涉及重启与 Flash 事务保护 |
| 任何现有 `Serial.*` 调用 | **不删除**。DEBUG 通道与 Log 通道**并存**（需求 §六"高频 Debug 数据继续留在 Debug/Serial"） |

---

## 19. Config

### 19.1 原则

> **真正需要用户/云端调整的才进 Config；内部实现参数一律编译期常量。**

理由：Config 每次改动都会触发"原子写 + Active/Backup 轮转 + 可能重启"，把内部调参暴露出去会把"改一个批量大小"变成"写 Flash + 重启"。

### 19.2 建议新增 Config 模块 `log`（9 个模块中的第 9 个）

`data/config/log.json`：

```json
{
    "enabled": true,
    "level_min_cloud": 1,
    "level_min_flash": 2,
    "upload_enable": true,
    "flash_enable": true,
    "critical_immediate": true
}
```

| 字段 | 类型 | 默认 | 说明 | 为什么进 Config |
|---|---|---|---|---|
| `enabled` | bool | `true` | Log 总开关。false 时 `log_emit()` 立即返回（零开销） | 现场排障时**关闭**日志以减少干扰；客户可关 |
| `level_min_cloud` | int 0..4 | `1`（INFO） | 上云最低级别 | 流量敏感场景可调到 2（只传 WARN+） |
| `level_min_flash` | int 0..4 | `2`（WARN） | 落盘最低级别 | 与需求"WARN/ERROR/CRITICAL 写 Flash"一致，且允许云端调高 |
| `upload_enable` | bool | `true` | 是否上云（离线设备的省电/省流量开关） | 用户可关 |
| `flash_enable` | bool | `true` | 是否落 Flash | 媒介寿命极端敏感场景可关 |
| `critical_immediate` | bool | `true` | CRITICAL 是否立即 flush + 立即发送 | 保留"退化为纯批量"的能力（极端省电场景） |

**注意**：`enabled` / `upload_enable` / `flash_enable` 的改动**立即生效，不需要重启**（LogManager 每次 `log_emit` 读一次缓存值，缓存由 `config` 变更时刷新或每 N 秒轮询一次）。这与"Config 改动需重启"的既有约定不同 —— **因此需要在接口文档中显式说明本模块的例外**，并在 `log.json` 的 README/注释里标注。

### 19.3 明确**不**进 Config 的（编译期常量）

| 常量 | 建议值 | 为什么不暴露 |
|---|---|---|
| `LOG_RECORD_SIZE` | 128 | 改它会同时改 Flash 布局与 Wire schema ⇒ 需版本迁移，不是"配置" |
| `LOG_SEG_RECORDS` | 32（可调 31） | 纯内部容量策略 |
| `LOG_SEG_MAX` | 16 | 同上 |
| `LOG_RAM_RING_SLOTS` | 64 | 同上 |
| `LOG_BATCH_MAX` | 16 | 同上 |
| `LOG_FLUSH_RECORDS` / `LOG_FLUSH_MS` | 8 / 5000 | 同上 |
| `LOG_ACK_TIMEOUT_MS` / `LOG_ACK_MAX_RETRY` / `LOG_ACK_STALE_MS` | 15000 / 5 / 1800000 | 同上 |
| `LOG_TX_MIN_INTERVAL_MS` | 500 | 同上 |
| `LOG_FLASH_MIN_INTERVAL_MS` | 20 | 同上 |
| `LOG_LEVEL_MIN`（编译期） | 1 | 已在 `platformio.ini`（§15.2） |
| Topic 名 `log_topic` | `guo_feeder/log` | ⚠️ **这个进 `mqtt.json`**（属通信配置，不是 Log 内部参数） |

### 19.4 新增模块的完整改动清单（这是一个已知的跨模块过程）

按项目既有约定，新增一个 Config 模块需要同步改：

1. `src/config_manager.h`：新增 `#define CONFIG_MODULE_LOG "log"`
2. `src/config_manager.cpp`：
   - `#define CONFIG_MODULE_COUNT` **8 → 9**
   - `kModuleNames[CONFIG_MODULE_COUNT]` 增加 `CONFIG_MODULE_LOG`
   - 新增字段 getter/setter（`config_get_log_*` / `config_set_log_*`）
   - `config_set` 的字段白名单（静态校验表）
3. `data/config/log.json`：新建
4. `data/config/version.json`：**重新生成**（`tools/gen_config_version.py`，脚本自动扫描 → 无需改脚本）
5. `tools/gen_config_version.py`：**无需改动**（已验证为自动扫描 `data/config/*.json`）

---

## 20. Module Integration

### 20.1 接入矩阵

| # | 模块 | 接入方式 | 需要的**额外**改动 | 优先级 | 改动量 |
|---|---|---|---|---|---|
| 1 | **System / Boot** | `main.cpp setup()` + `system_command.cpp` 直接 `log_emit` | ① 启动期失败暂存（§17.3）② `reset_reason` 已在 `system_command.cpp:222` 缓存，**API 现成**（`system_command_reset_reason()` / `_name()`） | **P0** | ~30 行 |
| 2 | **Workflow** | `workflow.cpp` 在 `workflow_terminate()` 等**状态迁移点**发 | 无（`slot/id/variant/state/step` 在函数内都可得） | **P0** | ~25 行 |
| 3 | **Water 链路** | `weight.cpp` / `valve.cpp` / `dispense_guard.cpp` | ① `dispense_guard` 需要"供水目标/起始重量"——需 **新增只读 getter** 或由 `weight.cpp` 提供 ② `0x0507 溢水风险`需**新增检测逻辑**（审计 §16.5） | **P0** | ~40 行 + 新检测 |
| 4 | **Config** | `config_manager.cpp` 的 `cfg_log` 点位旁并行发结构化事件 | 无 | P1 | ~15 行 |
| 5 | **Storage** | `json_storage` / `bin_storage` / `workflow_storage` 的错误返回处 | 无（错误码已有） | P1 | ~20 行 |
| 6 | **WiFi** | `wifi_module.cpp` 状态机迁移点 | 需要 `connect_ms` / `rssi`（`STATE_WIFI_RSSI` 已有；耗时需加计时） | P1 | ~20 行 |
| 7 | **MQTT / Cloud** | `cloud_manager.cpp` 的 `cloud_process_mqtt_events()` 等 | 需要 `connected_ms` / `retry_n`（`STATE_MQTT_RETRY_COUNT` 已有；耗时需加计时） | P1 | ~20 行 |
| 8 | **Time / RTC** | `time_manager.cpp` 的 valid 迁移与 RTC 路径 | 无 | P1 | ~20 行 |
| 9 | **Capability Registry** | `capability_registry.cpp registry_sync()` | 无 | P2 | ~8 行 |
| 10 | **Command** | `command_manager.cpp` 的超时/拒绝点 | 无 | P2 | ~10 行 |
| 11 | **Event Manager** | `event_manager.cpp event_push()`（用**累计计数**，不逐条） | 无（`event_get_drop_count()` 已有） | P2 | ~6 行 |
| 12 | **ComputerReset** | `computer_reset.cpp` | 无 | P3 | ~8 行 |
| 13 | **Mijia BLE** | `MiThermometer_task()`（**不在 NimBLE 回调**） | 需要"值变化超阈值 / 距上次 ≥10 min"的**限流逻辑** | P3 | ~15 行 |
| 14 | **OLED** | `oled.cpp oled_init()` | 无 | P3 | ~3 行 |

### 20.2 接入的原则（写代码时必须遵守）

| 原则 | 说明 |
|---|---|
| **只在状态迁移点发** | `if (new != old) log_emit(...)`。已有范式：`weight_refresh_error_state()` 的 `if(err != error_state)` |
| **不改函数的返回类型与语义** | Log 是**旁路**。任何"为了记日志而改返回值/加错误路径"的做法都违反最小侵入 |
| **不引入新的 getter 除非必要** | 优先在**已有的产生点**发（那里数据一定在手）。仅当"使用方需要产生方的数据"时才加 getter（如 `dispense_guard` 需要 target_g） |
| **一次接入一个模块并上板验证** | 与项目既有习惯一致（每阶段 commit） |
| **旧 `Serial` 输出保留** | Debug 通道与 Log 通道并存；Log 是"结构化子集"，不是"替换" |

### 20.3 明确不接入的模块

| 模块 | 理由 |
|---|---|
| `test_mqtt.cpp` | 测试代码，README §九 要求移入 `backup/` |
| `oled_animation.h` | 纯显示逻辑，无历史价值 |
| `serial_debug_command_process` 等自测控制台 | 只在开发者接串口时存在 |
| `data/` 下的脚本与生成器 | 主机侧工具 |

---

## 21. Test Plan

> 只做测试设计，不写测试。测试实施复用项目已有的**串口批量回归**范式（`test/serial_batch.py` + 用例 txt）。

> ⚠️ **EMQX 现状约束（已知）**：`guo_feeder/log` **尚未在 EMQX 配置**，
> 用户会在需要端到端验证时再添加。因此：

| 测试组 | 可否执行 | 说明 |
|---|---|---|
| Log Core（C1–C8） | ✅ 可测 | 纯设备侧 |
| Flash（F1–F12） | ✅ 可测 | 纯设备侧，含掉电注入 |
| **Cloud（N1–N8、N10–N11）** | ⚠️ **EMQX 配置前 NOT TESTABLE** | 无真实 log Topic 消费者 |
| **N9（Log Topic 被拒）** | ✅ **天然可测** | EMQX 未配置 = 天然的"发布被拒"场景，正好验证"发不出去时不崩、不递归、业务 `up` 不受影响" |
| MultiTopic（T1–T5） | ⚠️ Log 相关项 **NOT TESTABLE** | 同上 |
| 高负载（L1–L7） | ✅ 可测 | 设备侧负载与降级 |

**开发期替代验证手段**：① 串口 `logt dump` / `logt stats` 直接观察 RAM 环与段环；
② 本地 mosquitto broker（改 `mqtt.json` 指向）做端到端联调；
③ 待 EMQX 添加 `guo_feeder/log` 后补齐 N/T 组。

### 21.1 Log Core

| # | 用例 | 方法 | 期望 |
|---|---|---|---|
| C1 | 各级别入队 | 分别 `log_emit` DEBUG/INFO/WARN/ERROR/CRITICAL | DEBUG 不入队（除非编译期开）；其余按策略分流 |
| C2 | 参数类型往返 | 覆盖 I32/U32/F32/BOOL/ENUM/STR/I8/U16，含极值（0/负数/最大值/32 位浮点） | Flash 回读与 Wire 解码均一致 |
| C3 | 参数个数边界 | 0 个 / 8 个 / 试图 9 个 | 0 与 8 正常；9 被拒绝（编译期或运行期），**不截断** |
| C4 | 字符串边界 | 24 B 整 / 25 B 超长 / 中文 UTF-8 | 截断不切断码点；无越界 |
| C5 | Queue 满 | 离线状态下灌入 > 64 条 INFO | 丢最先的最旧/最低级，`drop_ring` 计数正确 |
| C6 | 多 Task 并发 | esp-mqtt 任务发（模拟命令结果）与 loopTask 发同时进行，共 10⁴ 次 | 无丢失、无 CRC 错误、计数总和 = 发出数 |
| C7 | 编译期裁剪 | `LOG_LEVEL_MIN=4` 构建 | DEBUG/INFO/WARN/ERROR 宏展开为空；固件体积明显下降 |
| C8 | 重入防护 | 用测试钩子让 I/O 区失败并触发 `log_emit` 路径 | `reentrant_drop` 增加，**无递归、无栈增长、无死循环** |

### 21.2 Flash

| # | 用例 | 方法 | 期望 |
|---|---|---|---|
| F1 | 正常写与回读 | 写 100 条 WARN，重启后读 | 全部可读，seq/boot_seq/timestamp 一致 |
| F2 | 段切换 | 写 40 条（跨 2 段） | 第 2 个段文件出现，段头 `first_seq` 正确 |
| F3 | Ring Full 覆盖 | 写 > **496** 条（P1 冻结容量） | 最旧段被删除，`drop_overflow` / `drop_unacked` 累加；最新条可读 |
| F4 | 重启恢复 | 写 → 重启 → 继续写 | append 点正确，**无重复 seq**（P1 区间预留），无丢失已确认数据 |
| F4b | **seq 跨重启唯一性**（P1 专项） | 写若干条 → **不等 flush 直接 restart** → 再写若干条 | 两次的 seq **无交集**；允许空洞 |
| F5 | CRC 单条损坏 | 用测试钩子翻转某条记录 1 bit | 该条及其后被丢弃；**CRC 失败按"段头/记录级"分别处理** |
| F6 | 段头损坏 | 翻转段头 1 bit | 整段被删除 + `corrupt_count++` + `LOG_STG_CRC_FAILED` |
| F7 | Partial Write（掉电半写） | 复用 `wfst` 的 `fail`/`abort` 注入范式，让段写中途失败 | 半写记录被丢弃；已完整写入的记录可读 |
| F8 | Power Loss 组合 | 在"段写完成"后注入掉电 | （P1 后 seq 唯一性不再依赖 flush 后更新 meta）seq 仍唯一 |
| F8b | **seq 预留失败降级** | 令 meta.bin 写入失败 | `seq_reliable = false`，批次头 `flags.bit0 = 0`，不崩溃 |
| F9 | 空段文件 | 手工创建 0 字节段文件 | 启动后正常在其内继续写 |
| F10 | `/log` 目录缺失 | 删除目录 | 自动重建；不崩溃 |
| F11 | Flash 不可用 | 模拟 LittleFS 失败 | 降级为纯 RAM；发 `LOG_LOG_SELF_DEGRADED`；**不崩溃、不递归** |
| F12 | 写速率与寿命 | 连续写 10⁴ 条（加速测试） | 无碎片化异常；段文件数恒 ≤16；耗时统计用于校准 §22 |

### 21.3 Cloud

| # | 用例 | 方法 | 期望 |
|---|---|---|---|
| N1 | Online 正常批 | MQTT 在线，产生 20 条 INFO | 按 ≤16 条/批发出；CBOR 可解；`seq_from/to` 正确 |
| N2 | Offline 累积 | 断网 → 产生 100 条 WARN → 恢复 | 全部按 seq 升序补发；无丢失（除环淘汰） |
| N3 | Reconnect 补发顺序 | 断网期间产生 WARN + 恢复后立刻产生 INFO | 先 Flash 旧 WARN，后新 INFO |
| N4 | ACK 正常 | 云端返回 `log_ack` | 对应段文件被删除；`log_pending_flash` 归零 |
| N5 | ACK 丢失 | 不返回 ACK | 15 s 后重传（seq 不变）；5 次后仍无 → 30 min 后 `FORCE_ADVANCE` + `LOG_LOG_ACK_LOST` |
| N6 | ACK 乱序/回退 | 返回较小的 `seq_to` | 忽略（tail 单调） |
| N7 | Duplicate | 同一批重传 | 设备端发出相同 seq；**云端幂等（云端侧验证）** |
| N8 | Batch 边界 | 精确 16 / 17 / 0 条 | 16 一批；17 → 16+1；0 不发 |
| N9 | 远端关闭 Log Topic | 用 broker ACL 拒绝 | `cloud_send_route` 返回 false；Log 侧不崩、不递归；**业务上行不受影响** |
| N10 | 大 backlog 限速 | 造 496 条 backlog | ≤ 500 ms/批 → ≤16 s 排空；期间业务上行延迟无增长 |
| N11 | CRITICAL 及时性 | backlog 100 条时触发 CRITICAL | CRITICAL 落盘立即；发送"提前 flush"；观测端到端延迟 ≤ 1 批 |

### 21.4 Multi Topic（需求 §二十 必测）

| # | 用例 | 期望 |
|---|---|---|
| T1 | `Up → Up Topic` | `guo_feeder/up` 收到 Command Result / Workflow Result / Registry 等；**载荷格式与改造前完全一致**（字节级对比） |
| T2 | `Log → Log Topic` | `guo_feeder/log` **只**收到日志批次；`up` 上不出现任何日志 |
| T3 | `Down → Down Topic` | 命令与 `log_ack` 都在 `down`；命令仍进 CommandManager，`log_ack` 不进 |
| T4 | **互不污染** | ① Log Topic 被拒时业务正常 ② 业务洪峰时日志不丢（落 Flash）③ 两者同时高频时 MQTT outbox 行为正常 |
| T5 | 订阅不变 | 抓包确认 CONNECT 后只有 1 个 SUBSCRIBE（`down`） |

### 21.5 高负载（需求 §二十 最后一项）

| # | 场景 | 期望 |
|---|---|---|
| L1 | 大量 DEBUG | 编译期关闭时不产生任何代码；开启时不影响 loop 周期（仅串口） |
| L2 | 大量 INFO（10³/分钟） | RAM 环溢出丢弃 + 计数；**不写 Flash**；loop 周期增量 < 1% |
| L3 | 连续 WARN | 按 8 条/批落盘；段轮换正常；无掉帧 |
| L4 | 连续 ERROR | 每批提前 flush；发送限速生效 |
| L5 | CRITICAL 风暴（人为注入 100 条） | 每条立即落盘（1 段/轮上限生效）；不阻塞 loop；不递归 |
| L6 | **"Log 不拖垮业务"** | 在 L2–L5 全程同时运行一个 4-step 供水 Workflow | Workflow 时序抖动 ≤ 一次 Flash 写（≤50 ms）；供水结果正确；阀门响应正常 |
| L7 | 混合最坏 | L5 + 断网重连 + 32 批补发 | loop 周期最大值有界；无看门狗复位 |

### 21.6 需要的测试基础设施（实施前准备）

| 需求 | 说明 |
|---|---|
| Log 注入测试命令 | 复用 `serial_debug_command_process` 风格，新增 `logt` 控制台：`logt emit <id> <level> <n>` / `logt fill <n>` / `logt dump` / `logt stats` / `logt corrupt <seg> <off>` / `logt failseg <n>` |
| 掉电注入 | 复用 `workflow_storage.cpp` 已有的 `workflow_storage_test_fail_step` / `test_fail_wf` 范式（**已有先例，直接照搬形态**） |
| 回读校验 | 复用 `tools/` 下生成器脚本的校验思路（读文件 → 校验 magic/CRC） |
| 云端侧 mock | 需要能返回 `log_ack` 的最小消费者（或手工用 MQTT.fx 发 `{"c":"log_ack","i":"1","p":{"b":1,"f":1,"t":16}}`） |

---

## 22. RAM / Flash / CPU 资源估算

### 22.1 RAM

| 项 | 大小 | 存放 | 说明 |
|---|---|---|---|
| RAM 环（64 槽 × 128 B） | **8 KB** | **PSRAM** | 待发送缓冲 |
| 段写缓冲（16 + **31**×128 = **3984 B**） | **4.0 KB** | **PSRAM 优先**，失败回退 DRAM | 与项目既有范式一致（`workflow.cpp` 的池分配就是 PSRAM 优先）；P1 冻结 31 条/段后正好 < 4 KB |
| CBOR 编码缓冲 | **4 KB** | **静态 DRAM** | 编码是 CPU 密集路径，放 DRAM 更快 |
| `meta.bin` 读写缓冲 | 32 B | DRAM | |
| 统计/状态/配置缓存 | ~120 B | DRAM | |
| 调用点 `LogParamIn[]`（8 × 8 B） | 64 B × 调用深度（≤3） | 调用方栈 | 有界 |
| **合计** | **PSRAM ≈ 12 KB / DRAM ≈ 4.2 KB** | | |

**可行性**：DRAM 静态占用从 ~130 KB → ~134.2 KB（**仍远低于 327 KB**）。PSRAM 8 MB 中占 12 KB。**结论：无内存压力。**

> ⚠️ 注意：`LogParamIn[]` 出现在**调用方栈**上，而 loopTask 栈为 16 KB（HWM 余量约 6.5 KB）。3 层嵌套 × 64 B = 192 B，**安全**。但必须禁止在调用点构造更大的临时对象（这也是"禁止传 `String`"的原因之一）。

### 22.2 Flash

| 项 | 大小 | 占比 |
|---|---|---|
| 段文件（16 × **3984 B**） | **≈ 63.7 KB** | 12.5 MB 的 **0.5%** |
| `meta.bin` | 32 B | — |
| **合计** | **≈ 63.7 KB** | **0.5%** |

**不需要修改 `partitions.csv`**（这是明确结论：用 LittleFS 内的独立目录 `/log/`，而非新分区）。理由：改分区表需要重烧整个 Flash 布局，风险与成本都远高于收益。

> **P1 冻结**：段容量已定为 **31 条/段**（16 + 31×128 = 3984 B < 4096，严格落在单个 LittleFS block 内），
> 总容量 **496 条**。原"编码阶段再决定 31 或 32"的选项**取消** —— 32 条（4112 B）会跨两个 block，
> 使写入行为不确定；少 16 条容量无产品意义。

### 22.3 擦写寿命

见 §11.6：**≤ 10 次 block 擦写/日，理论寿命 > 27 年**。

### 22.4 CPU

| 操作 | 估算 | 频率 |
|---|---|---|
| `log_emit()` | **2–5 µs**（128 B 组装 + memcpy + 临界区） | 每次事件 |
| CBOR 编码 16 条 | **1–3 ms** | 每批（≤2 次/秒） |
| LittleFS 段写入（~4 KB） | **10–50 ms** | ≤1 次/loop，间隔 ≥20 ms |
| `log_ack` 处理 | ~1 µs | 每批 |
| `log_task()` 空转（无事可做） | **< 2 µs** | 每 loop |

**对 loop 周期的影响**：

| 场景 | 单轮 loop 增量 |
|---|---|
| 常态（无 WARN、无待发） | **< 5 µs**（可忽略） |
| 有 INFO 待发 | +1–3 ms（每 500 ms 一次） |
| 有 WARN 待落盘 | +10–50 ms（每 ≥20 ms 最多一次；实际频率极低） |
| **最坏单轮** | **≈ 53 ms** |

**评估**：阀门安全超时为**秒级**、HX711 采样周期 **100 ms**、Workflow Step 为 ms–s 级 ⇒ **53 ms 的最坏单轮延迟不违反任何实时约束**。若未来需要更小，可把段拆为 2 KB（约 20 ms）或使用 `bin_storage_write_chunk` 分轮写入。

### 22.5 与"非阻塞铁律"的一致性

| 铁律 | 是否满足 | 证据 |
|---|---|---|
| 禁 `delay()` | ✅ | `log_task()` 无 delay；节流用 `millis()` 比较 |
| 禁 while 死等 | ✅ | 无任何等待循环 |
| 禁阻塞回调 | ✅ | `log_emit` 上界 5 µs |
| 禁同步等硬件完成 | ✅ | 不等 ACK；Flash 写是同步 API 但单轮有界 |
| 队列定容 | ✅ | RAM 环 64 槽固定；段环 16 段固定 |
| 队列满不溢出 | ✅ | 丢弃 + 计数（AI_RULES §3 明确要求） |
| 每个任务都有执行机会 | ✅ | `log_task()` 每轮调用，且**最多**做 1 次 Flash 写 + 1 次发送 |

---

## 23. 风险与未决问题

### 23.1 必须由人工确认才能进入 Coding Phase（阻塞项）

| # | 问题 | 影响 | 本报告的倾向 |
|---|---|---|---|
| ~~**Q1**~~ | ~~`cloud_protocol.md` 与代码严重不一致……Log 的 ACK 契约应以哪份为准？~~ | — | ✅ **已解决（2026-09-15）**：`cloud_protocol.md` 已按代码实读重写为 **V2.0**，以代码实际行为为准（`c`/`i`/`p` + 紧凑键）。**不再阻塞** |
| **Q2** | EMQX 的 ACL 是否允许 `guo_feeder/log` 这个 Topic？ | 若不允，整个 Log Topic 方案失效，须退化为"复用 up + 载荷区分" | ✅ **已获用户答复（2026-09-15）**：**EMQX 尚未配置该 Topic**，用户会在需要端到端验证时再添加。⇒ 不阻塞编码；Cloud 组测试在配置好前标记 **NOT TESTABLE**（见 §21.0） |
| **Q3** | 云端是否接受 `(device_id, boot_seq, seq)` 幂等去重？ | at-least-once 语义下云端必须幂等 | **必须接受** —— 这是 §13.5 的正式契约，不是可选项。⚠️ 原"容忍 `FORCE_ADVANCE` 重复批次"部分已随 `FORCE_ADVANCE` 降级而**不再是前置条件**（v1 默认不强制推进，但仍会因重传产生重复） |
| ~~**Q4**~~ | ~~`INFO 不落 Flash` 是否确认？~~ | — | ✅ **已确认（P1 评审）**：**无任何例外**。撤销原 §11.7 的 3 条 INFO 例外；禁止 EventId 级 `persist` 覆盖 |
| **Q5** | 是否允许把 `log.json` 的 3 个开关做成**立即生效**（不需重启），与"Config 改动需重启"的既有约定例外？ | 影响 LogManager 与 ConfigManager 的交互形态 | 倾向允许（否则现场排障必须重启） |

> **当前剩余真实阻塞项：仅 Q5。**
> Q1 已随 `cloud_protocol.md` V2.0 解决；Q2 已由用户明确（待配置，不阻塞编码）；
> Q3 已固化为契约；Q4 已由评审拍板。

### 23.2 前置修复项（不属 LogManager，但强烈建议先做）

| # | 问题 | 位置 | 建议 | 与本任务的关系 |
|---|---|---|---|---|
| **P1** | JsonStorage / FileStorage 日志回调从未注册 → **约 88 处 E/W 日志静默丢弃** | `main.cpp:74,114` | 补两行注册到 `Serial` | 若不做，Config 存储故障的真实原因在 Log 里也看不到 |
| **P2** | `event_names[]` 从索引 10 起整体错位；`SYSTEM_EVENT_COUNT = EVENT_ERROR+1 = 15` 而阀门事件值为 14/15/16 | `event_manager.cpp:37,82` | 统一表与枚举顺序 | Log 若需要 `event_to_string()` 会产出**错误名称** |
| **P3** | MQTT 调试横幅明文打印 password | `cloud_manager.cpp:1205-1228` | 整块 `#ifdef` 且永不输出 password | Log 通道一旦建成，凭据泄露面扩大 |
| **P4** | NimBLE 回调内逐字节 `Serial.printf`（29–62 次/包） | `MiThermometer.cpp:117-124` | 宏隔离或删除 | 该回调禁止再叠加任何工作（§16.2） |
| **P5** | `test_mqtt.cpp` 仍在 setup/loop | `main.cpp:107,186` | 移入 `backup/` | 避免生产固件含测试代码与额外串口输出 |

### 23.3 设计层面的风险（已有对策）

| # | 风险 | 对策 | 残余风险 |
|---|---|---|---|
| R1 | Flash 写阻塞 loop | 每轮 ≤1 次写 + 20 ms 间隔 + 段 ≤4 KB | 最坏单轮 53 ms（§22.4，可接受） |
| R2 | Log 自身故障递归 | 四层防护（§17） | 低 |
| R3 | ACK 永不到达 → 段环卡死 | `FORCE_ADVANCE` 兜底（§13.2） | 依赖云端幂等（Q3） |
| R4 | `ssid_hash` 仍可能被反推 | 用截断哈希 + 不记明文 | 低（SSID 本身弱隐私） |
| R5 | `boot_seq` uint16 回绕 | 65536 次开机 ≈ 10 年以上 | 可升级 uint32（+2 B/记录） |
| R6 | 段文件碎片化 | 固定 16 槽 + 按序删除最旧 | 低（F12 用例验证） |
| R7 | 时间无效期间记录无法按时间排序 | `(boot_seq, uptime_ms)` 二级排序 | 云端需实现该排序规则（**需写入接口文档**） |
| R8 | 新增 Config 模块导致 `version.json` 需重建 | 用 `tools/gen_config_version.py` 自动扫描 | 低（已有工具） |
| R9 | `platformio.ini` 与 `lib_deps` **未固定版本** | 建议在 P0 固定 | 中等（构建漂移会影响二进制格式） |
| R10 | 大 `LogParam` 常量字符串占 Flash | 事件名/参数名**不下发**（云端按 ID 渲染文案）⇒ 固件内无文案表 | 低 |

### 23.4 需求中的"建议测试"项，本环境无法执行的

| 需求条目 | 可否执行 | 说明 |
|---|---|---|
| Test 12（Action/Trigger Registry 变化，需求 §27-12） | ❌ **NOT TESTABLE** | 设备无运行时增删 Action/Trigger 的命令接口，需重新编译固件才能改注册集合 |
| MQTT 端到端全链路 | ⚠️ **部分可测** | 设备侧可测；云端侧需真实消费者（或 MQTT.fx 手工模拟 `log_ack`） |
| 掉电恢复 | ✅ 可测 | 复用 `wfst` 的 `fail`/`abort` 注入范式（已有先例） |

---

## 24. 最终推荐架构

### 24.1 一页总览

```
┌──────────────────────── 应用 / 自动化层 ────────────────────────┐
│  Valve · Weight · DispenseGuard · Mijia · OLED · ComputerReset  │
│  WorkflowManager · CapabilityRegistry                           │
│              │ log_emit(id, params[], ctx)   ← 仅标量参数        │
└──────────────┼──────────────────────────────────────────────────┘
               ▼
┌──────────────────────── 服务层：LogManager ─────────────────────┐
│ ① log_emit()   过滤 → 128B 组装 → 入环   【<5 µs，无 malloc】    │
│ ② RAM Ring     64 × 128 B (PSRAM 8 KB)   【满则丢最低级 + 计数】  │
│ ③ Policy       DEBUG✗  INFO→云  WARN+→云+Flash                  │
│ ④ Flash Seg    16 段 × ~4 KB (/log/)     【零原地修改】          │
│ ⑤ log_task()   每 loop ≤1 次写 + ≤1 次发 【节流 20ms / 500ms】   │
└──┬───────────────────────────────────────────┬──────────────────┘
   │ BinStorage → FileStorage → LittleFS       │ upload_cb（main.cpp 注入）
   ▼                                           ▼
┌──────────────────────── 云通信层：CloudManager ─────────────────┐
│ cloud_send_up()  ──────────► guo_feeder/up    （业务，语义不变） │
│ cloud_send_route(LOG, ...) ► guo_feeder/log   （仅日志，CBOR）   │
│ down 分发：change_msg_limit ／ ★log_ack → 回调 LogManager        │
└─────────────────────────────────────────────────────────────────┘
                     ▲
              MQTT / EMQX (8883, TLS)
```

### 24.2 核心参数速查

> **⚠️ P1 已修订**：以下为**修订后**的冻结值（与 `LogManager-P1契约冻结0915.md` 一致）。

| 项 | 值 |
|---|---|
| 记录 | **128 B 定长**（8 参数 + 32 B context blob + 双层 CRC），**`version = 2`** |
| `flags` | bit0 `timestamp_valid`、bit1 `context_present`、bit2-7 reserved（**无 `persist`、无 `uploaded`**） |
| RAM 环 | **64 槽 = 8 KB**（PSRAM） |
| Flash 段环 | **16 段 × 31 条 = 496 条 ≈ 63.7 KB**（0.5% of 12.5 MB） |
| 落盘级别 | **WARN / ERROR / CRITICAL**（**无任何 INFO 例外**，禁止 EventId 级覆盖） |
| 上云级别 | **INFO / WARN / ERROR / CRITICAL** |
| `seq` | uint32，**Boot 预留区间**（默认 256），**允许空洞、不允许重复** |
| `boot_seq` | **uint32**，`log_init()` 时 +1 并落盘 |
| 幂等键 | `(device_id, boot_seq, seq)` |
| Wire 编码 | **CBOR 定序数组**，批 ≤16 条，≤4096 B，批次头 `fmt = 2` |
| 传输语义 | **at-least-once**（设备）+ **云端幂等去重**；ACK ≠ "不会再出现" |
| ACK | **批次级**（`seq_from`..`seq_to`），载体 `down` |
| 强制推进 | 🔴 **撤销**：v1 默认 `GIVE_UP_NOT_ADVANCE`（保留记录）；`FORCE_ADVANCE` 为**可选策略、默认关闭、不冻结** |
| CRITICAL | **Flash 立即** + **Cloud 进入最高优先级下一批次**；**不建独立通道**；**不承诺秒级** |
| 任务 | **无独立 Task**；`log_task()` 在 `loop()` 中（保证 LittleFS 单写者） |
| 递归防护 | 重入守卫 + 待报标志 + 不劫持文本回调 + 侧信道计数 |
| CloudManager 改动 | **4 处纯新增**，现有函数 0 改动 |
| Config | **1 个新模块 `log`（6 个字段）**；其余全为编译期常量 |
| 新事件 | **100 个 LogEventId**（约 65 个全新） |
| 参数字典 | **86 个 LogParamId**（附录 A） |

### 24.3 满足需求 §二十三 的 20 条设计原则（逐条对照）

| # | 原则 | 满足方式 |
|---|---|---|
| 1 | 最小侵入 | CloudManager 4 处纯新增；现有函数签名 0 改动 |
| 2 | 复用现有 CloudManager | 只加 route，不建新传输层 |
| 3 | LogManager 不直接操作 MQTT | 通过注入的 `upload_cb` |
| 4 | 三 Topic 语义清晰分离 | §4.1 表；Log 不混入 Up |
| 5 | Log 不污染现有 Up 协议 | 不修改 `cloud_send_up` / `cloud_compress_uplink` |
| 6 | 结构化 Log，无大量动态 String | 128 B 定长 + 类型化参数 + 24 B 截断短串 |
| 7 | DEBUG 不进正式 Log | 编译期宏 + 运行期过滤双保险 |
| 8 | INFO 不写 Flash | 3 条例外（配网 / 时间生效）已显式列出 |
| 9 | WARN/ERROR/CRITICAL 写 Flash | §15.1 矩阵 |
| 10 | CRITICAL 尽可能快速上传 | 立即落盘 + 提前 flush（≤16 s 上界） |
| 11 | 离线时 WARN+ 不丢 | 段环耐久；RAM 环满只影响"待发副本" |
| 12 | Flash 是故障保险，不是日志数据库 | 66 KB 环形、拒绝 INFO 落盘、明确淘汰语义 |
| 13 | 不允许 Log 阻塞正常业务 | 每轮有界 + 节流（§16.4） |
| 14 | 不允许递归 Logging | 四层防护（§17） |
| 15 | 不过度复杂 | 否决统一 TX 队列、否决 CRITICAL 独立通道、否决分片 |
| 16 | 不改已稳定的 Workflow / Cloud Contract | 只加 `log_emit` 调用；不动任何事务/契约逻辑 |
| 17 | State 是当前事实，Log 是历史事实 | §3.4 明确不做自动转换 |
| 18 | 一个 Log = Event + 必要 Context | 32 B 定长 blob + 6 种 Context 类型 |
| 19 | 时间无效时仍能可靠排序 | `timestamp_valid` + `boot_seq` + `uptime_ms` + `seq` |
| 20 | 所有设计结合当前代码 | 全部结论基于实测（附录 B） |

---

## 25. 后续 Coding Phase 拆分

> 每个 Phase 独立可编译、可上板、可 commit。
> **P1 已按评审拆为 P1.0（契约 Review，仅冻结不编码）与 P1.1（落头文件）。**

| Phase | 内容 | 产出 | 依赖 | 预估改动 |
|---|---|---|---|---|
| **P0 前置修复** | ① 修 `event_names[]` 错位 ② MQTT banner 去密码 ③ 注册 `json_storage` / `file_storage` 日志回调（→ Serial）④ BLE 逐字节 hex 宏隔离 ⑤ `platformio.ini` 固定 platform 与 lib 版本 | 5 个独立小 commit | 无 | ~50 行 |
| | ~~`test_mqtt` 移入 `backup/`~~ | **可选、可延后**（与 Log 弱相关，不纳入 P0 硬性项） | — | — |
| **P1.0 Log Contract Review** | **仅冻结，不编码**。冻结 6 项：① Level Policy ② LogEventId ③ LogParamId ④ LogRecord 128 B（v2）⑤ Flash Segment（31 条/段）⑥ Cloud Batch / ACK 契约（at-least-once） | `log模块历史/LogManager-P1契约冻结0915.md` | 无 | 0 行代码 |
| **P1.1 契约落地** | 冻结通过后写 `src/log_events.h`（100 个 LogEventId + 86 个 LogParamId + Level/枚举/字典表 + 128 B 记录布局常量 + 策略表）。**只写头文件，不写实现** | `src/log_events.h` | **P1.0** | ~500 行（纯声明/表） |
| **P2 Log Core** | `log_init` / `log_emit` / 级别过滤 / RAM 环 + 临界区 / 编译期宏 / 丢弃计数 / `LOG_*` 便捷宏 | `log_manager.h/.cpp`（RAM 部分）+ `platformio.ini` | P1 | ~400 行 |
| **P3 Flash 段环** | `/log/` 段式环（基于 BinStorage）+ `meta.bin` + 启动扫描重建 + 半写丢弃 + 批量/立即 flush + `log_get_stats` | 上同 + `boot_seq` | P2 | ~500 行 |
| **P4 CBOR 批次编码** | 批次头 + 记录数组编码 + 4096 B 预算自适应降批 + 编码失败降级 | 上同 | P2 | ~250 行 |
| **P5 多 Topic 通道** | `CloudRoute` + `cloud_send_route` + `store=0` 发布 + `log_topic` 配置 + `log_ack` 下行分支 + `cloud_set_log_ack_callback` + main.cpp 注入 | `cloud_manager.h/.cpp` + `main.cpp` + `mqtt.json` | P4 | ~70 行（CloudManager） |
| **P6 ACK / Retry / Dedup** | 发送状态机 + 退避 + **v1 默认 `GIVE_UP_NOT_ADVANCE`**（`FORCE_ADVANCE` 预留开关但默认关闭）+ 侧信道计数（`drop_unacked` / `flags.seq_reliable`）+ 段删除推进 | `log_manager.cpp` | P5 | ~200 行 |
| **P7 模块接入（分批 commit）** | 7a System/Boot → 7b Workflow → 7c Water 链路（含 0x0507 新检测）→ 7d Config/Storage → 7e WiFi/MQTT → 7f Time/RTC → 7g Registry/Command/Event → 7h BLE/CRESET/OLED | 各模块 + 每批 1 commit + 上板验证 | P3,P6 | 每批 5–40 行 |
| **P8 Context Snapshot** | 6 种 Context 的填充与序列化；挂接 S1–S6 触发点（Boot / CRITICAL / 供水未达标 / WF 失败 / MQTT 休眠 / Storage 恢复） | `log_manager.cpp` + 各模块 | P7 | ~200 行 |
| **P9 Config + 测试** | `log.json` 模块接入（含 9 模块计数）+ 立即生效机制 + `logt` 测试控制台 + 执行 §21 全部用例 + 长稳 72 h | `config_manager.*` + `log.json` + `test/` | P8 | ~200 行 + 测试资产 |
| **P10 文档** | `workflow_cloud_interface.md` 同级新增 `log_cloud_interface.md`（云端契约）；`readme.md` §4.x 补 Log 章节；修订 `cloud_protocol.md` 的 Topic 与 `log_ack`；进度台账 | 3–4 个文档 | P9 | — |

### 25.1 关键依赖

```
P0 ──┐
     ├──► P7a (System/Boot 最早可接，因为它不依赖 Flash 段环)
P1 ──┴──► P2 ──┬──► P3 ──┐
               └──► P4 ──┴──► P5 ──► P6 ──► P7 ──► P8 ──► P9 ──► P10
```

### 25.2 每阶段的验收标准

| Phase | 验收 |
|---|---|
| P0 | 串口能看到 json/file_storage 的 E/W 日志；`event_to_string(EVENT_VALVE_OPEN)` 返回正确名；串口无 password |
| **P1.0** | **契约文档评审通过**（不产生代码）。6 项冻结值无争议；与本报告 §7/§11/§13/§15 已同步修订 |
| P1.1 | 编译通过（仅头文件）；事件/参数 ID 无重复；字典表覆盖 §9/附录 A；**`persist`/`uploaded` 字段不存在** |
| P2 | 上板：`logt fill 100` 后 `drop_ring` 计数正确；loop 周期增量 < 1% |
| P3 | 上板：写满 16 段 → 最旧段被删；重启后 append 点正确；注入半写 → 该条被丢弃；**F4b：未 flush 即 restart → seq 无交集** |
| P4 | 上板：抓包/replay 得到可解 CBOR；16 条批次 ≤ 4096 B |
| P5 | 上板：`guo_feeder/log` 收到日志；`guo_feeder/up` 内容与改造前字节一致<br>⚠️ **EMQX 未配置该 Topic 时此项为 NOT TESTABLE**，改测 N9（发布被拒时的降级） |
| P6 | 上板：不发 ACK → 5 次重传（seq 不变）→ **v1 保留记录不推进**；`drop_unacked` 计数正确；`FORCE_ADVANCE` 开关存在且默认为关 |
| P7 | 每个子批：对应事件在真实业务场景下产生，且**不影响**该业务的既有串口行为与结果 |
| P8 | 在 CRITICAL/未达标场景下，记录内含完整 Context |
| P9 | §21 全部用例 PASS 或明确 NOT TESTABLE；72 h 长稳无复位、Flash 段数 ≤16 |
| P10 | 文档与代码一致；云端可依据文档独立实现消费者 |

---

## 附录 A：参数字典（LogParamId）

> 全局共享，跨事件复用。每个事件在自己的字典条目里声明"使用哪些 id + 类型"。
> 类型缩写：`I32` `U32` `F32` `BOOL` `ENUM` `STR` `I8` `U16`

| ID | 名称 | 类型 | 单位/取值 |
|---|---|---|---|
| 0x00 | `P_NONE` | — | 保留 |
| 0x01 | `P_SLOT` | U8 | Workflow Slot 0..15 |
| 0x02 | `P_WF_ID` | STR | ≤16 B |
| 0x03 | `P_VARIANT` | U32 | |
| 0x04 | `P_VARIANT_PREV` | U32 | |
| 0x05 | `P_TRIGGER_SOURCE` | ENUM | 0=timer 1=event 2=weight 3=cmd |
| 0x06 | `P_DURATION_MS` | U32 | ms |
| 0x07 | `P_STEPS_DONE` | U8 | |
| 0x08 | `P_TIMEOUT_MS` | U32 | ms |
| 0x09 | `P_STUCK_STEP` | U8 | |
| 0x0A | `P_FAIL_STEP` | U8 | |
| 0x0B | `P_ACTION_ID` | STR | ≤16 B |
| 0x0C | `P_ERR_CODE` | I32 | 复用各模块错误码 |
| 0x0D | `P_TARGET_G` | F32 | g |
| 0x0E | `P_START_G` | F32 | g |
| 0x0F | `P_FINAL_G` | F32 | g |
| 0x10 | `P_DELTA_G` | F32 | g |
| 0x11 | `P_STOP_REASON` | ENUM | 0=达标 1=超时 2=保护 3=手动 4=WF失败 |
| 0x12 | `P_SOURCE` | ENUM | 0=workflow 1=cmd 2=protect |
| 0x13 | `P_OPEN_MS` | U32 | ms |
| 0x14 | `P_LIMIT_MS` | U32 | ms |
| 0x15 | `P_WEIGHT_G` | F32 | g |
| 0x16 | `P_CAUSE` | ENUM | 0=NO_DATA 1=RAW_ZERO 2=JUMP |
| 0x17 | `P_JUMP_COUNT` | U8 | |
| 0x18 | `P_RAW` | I32 | HX711 原始 |
| 0x19 | `P_FILTERED` | I32 | |
| 0x1A | `P_OFFSET` | I32 | 调零偏移 |
| 0x1B | `P_SAMPLES` | U16 | |
| 0x1C | `P_SAVED` | BOOL | |
| 0x1D | `P_MODULE` | STR | ≤16 B |
| 0x1E | `P_KEY` | STR | ≤16 B |
| 0x1F | `P_REASON` | ENUM | 通用原因（各事件自定义映射） |
| 0x20 | `P_RESET_REASON` | ENUM | `esp_reset_reason_t` 原值 |
| 0x21 | `P_BOOT_SEQ` | U16 | |
| 0x22 | `P_INIT_MS` | U32 | ms |
| 0x23 | `P_LOADED` | U8 | |
| 0x24 | `P_TOTAL` | U8 | |
| 0x25 | `P_PATH` | STR | basename，≤24 B |
| 0x26 | `P_STAGE` | ENUM | 存储/提交阶段 |
| 0x27 | `P_STORED_CRC` | U32 | |
| 0x28 | `P_CALC_CRC` | U32 | |
| 0x29 | `P_RETRY_N` | U8 | |
| 0x2A | `P_SLEEP_MS` | U32 | ms |
| 0x2B | `P_OUTBOX` | U8 | |
| 0x2C | `P_CONNECTED_MS` | U32 | ms |
| 0x2D | `P_CMD_ID` | STR | ≤8 B |
| 0x2E | `P_CMD` | STR | ≤12 B |
| 0x2F | `P_OBJ` | STR | ≤24 B |
| 0x30 | `P_ATTEMPT_N` | U8 | |
| 0x31 | `P_RSSI` | I8 | dBm |
| 0x32 | `P_CONNECT_MS` | U32 | ms（WiFi 连接耗时） |
| 0x33 | `P_SSID_HASH` | U16 | 截断哈希（**不记明文**） |
| 0x34 | `P_SERVER` | STR | NTP 服务器名，≤24 B |
| 0x35 | `P_UNIX` | U32 | Unix 秒 |
| 0x36 | `P_DRIFT_MS` | I32 | ms |
| 0x37 | `P_ADDR` | U8 | I2C 地址 |
| 0x38 | `P_WHICH_POOL` | ENUM | 0=trigger 1=action 2=step_def 3=other |
| 0x39 | `P_NEED_BYTES` | U32 | |
| 0x3A | `P_FREE_BYTES` | U32 | |
| 0x3B | `P_TEMP` | F32 | °C |
| 0x3C | `P_HUMID` | F32 | % |
| 0x3D | `P_BATT_V` | F32 | V |
| 0x3E | `P_MAC_SUFFIX` | STR | 后 4 位 hex，≤5 B |
| 0x3F | `P_FAIL_COUNT` | U16 | |
| 0x40 | `P_DROPPED_TOTAL` | U32 | |
| 0x41 | `P_ACTIVE` | U8 | |
| 0x42 | `P_CAPACITY` | U8 | |
| 0x43 | `P_HOLD_MS` | U32 | ms |
| 0x44 | `P_STATE` | ENUM | 通用状态机状态 |
| 0x45 | `P_COUNT` | U16 | 通用计数 |
| 0x46 | `P_OP` | ENUM | 0=create 1=set 2=delete |
| 0x47 | `P_WAS` | ENUM | 变更前的状态 |
| 0x48 | `P_VALVE_OPEN_MS` | U32 | ms |
| 0x49 | `P_GAIN_AFTER_CLOSE_G` | F32 | g（溢水证据） |
| 0x4A | `P_WINDOW_MS` | U32 | ms |
| 0x4B | `P_VERSION` | U32 | Config/Registry 版本 |
| 0x4C | `P_CHECKSUM` | U32 | |
| 0x4D | `P_REG_TYPE` | ENUM | 0=action 1=trigger 2=workflow |
| 0x4E | `P_STAGED_COUNT` | U8 | |
| 0x4F | `P_ARCHIVE_ACTION` | ENUM | 0=publish 1=discard |
| 0x50 | `P_LAST_OK_AGE_MS` | U32 | ms |
| 0x51 | `P_BLE_LEVEL` | ENUM | 0=正常 1=LEVEL1 2=LEVEL2 |
| 0x52 | `P_FAIL_KIND` | ENUM | 0=decrypt 1=mic 2=decode 3=scan |
| 0x53 | `P_QUEUE_SIZE` | U8 | |
| 0x54 | `P_DROP_RING` | U32 | Log 侧信道：RAM 环丢弃 |
| 0x55 | `P_DROP_OVERFLOW` | U32 | Log 侧信道：段环淘汰 |
| 0x56 | `P_MIN_FREE` | U32 | 最小空闲堆 |
| 0x57 | `P_PENDING` | U8 | 待处理数 |
| 0x58 | `P_REMAIN_MS` | U32 | ms |

**校验规则**：ID 唯一；每个事件字典条目声明的 (id, type) 必须与上表类型一致（编译期用 `constexpr` 表检查或在评审时人工核对）。

---

## 附录 B：本报告实测引用清单

> 全部结论基于以下**实读**（非推测）。行号为 `ce9ba37` 基线。

### 源代码

| 文件 | 关键引用 |
|---|---|
| `src/cloud_manager.h` | 全部 108 行；`:37-50` 三个未实现的 binary/cbor 声明 |
| `src/cloud_manager.cpp` | `:21-39` QOS/STORE/常量；`:44-120` 状态与缓冲；`:127-218` 事件回调与 RX 环；`:219-316` `mqtt_event_handler`（订阅在 `:233`）；`:321-426` 两个 publish 函数；`:435-511` 分片；`:522+` 上行压缩；`:782-802` `cloud_send_ack`；`:808+` `change_msg_limit`；`:1024-1100` `cloud_process_rx_message`（分支顺序）；`:1164-1174` `on_command_result`；`:1176-1276` `cloud_connect`（含 `:1205-1228` 密码横幅）；`:1281-1340` `cloud_init`；`:1455-1515` `cloud_send_set` / `cloud_send_up` / `cloud_upload_json`；`:175/193` RX 环入队出队 |
| `src/main.cpp` | `setup()` 全部八层；`loop()` 全部任务顺序与注释 |
| `src/system_state.h` | 全部 144 行（19 个状态键） |
| `src/event_manager.h` | 全部 147 行（17 个事件 + `SYSTEM_EVENT_COUNT`） |
| `src/bin_storage.h` | 全部 343 行，含"用于大文件（BIN / OTA / **Log**）"与"未来 Log Manager 完成后由上层注册回调统一接管" |
| `src/time_manager.h` | `time_get()` 无效返回 0；`time_query()` 的 source/last_ntp_sync |
| `src/config_manager.cpp` | `:24-37` 常量（`CONFIG_MODULE_COUNT 8`、`CONFIG_LOG_BUF_SIZE 192`）；`:57-70` `kModuleNames[]`；`:1058-1104` `last_boot_ok` 判定；`:1315` `config_task`；`:1329` `config_boot_validate` |
| `src/command_manager.h` | `:126` `CommandLogCallback`；`:129` `CommandResultCallback`；`:181` `command_manager_set_result_callback` |
| `src/system_command.cpp` | `:46-48` `s_reset_reason`；`:95` `system_command_reset_reason_name()`；`:222` `esp_reset_reason()` 缓存；`:488` getter；`:312` 唯一 `ESP.restart()` |
| `src/system_command.h` | `:130/139/142` critical op；`:100-116` restart API；`:151-155` reset reason API |
| `src/json_storage.h` / `src/file_storage.h` | `:41-47` / `:47-53` 日志回调声明（**main.cpp 未注册**） |

### 配置与工程

| 文件 | 关键内容 |
|---|---|
| `partitions.csv` | `littlefs 0x410000 / 0xBF0000`（12.5 MB） |
| `platformio.ini` | `ARDUINOJSON_USE_CBOR=1`；`ARDUINO_LOOP_STACK_SIZE=16384`；**`platform` 与 `lib_deps` 未固定版本** |
| `data/config/mqtt.json` | 实际 broker（EMQX）、`subscribe_topic: guo_feeder/down`、`publish_topic: guo_feeder/up`、明文 password |
| `data/config/` | 8 个模块 JSON + `version.json`（新增 log 模块后为 9） |

### 文档

| 文档 | 用途 |
|---|---|
| `AI_RULES.md`（全文） | 分层方向、回调限制、队列满策略（"拒绝新项 + 警告日志"）、非阻塞铁律、避免重复系统、内存管理 |
| `cloud_protocol.md`（全文） | 与代码严重不一致（§2.2 发现 4）；`log_upload` 未实现 |
| `需求文档.md` §5 | Log 模块规划（VERBOSE…ERROR、串口 + MQTT、内存 Ring Buffer、Config 控等级） |
| `log模块历史/LogManager全系统审计报告0914.md`（1386 行） | 本设计的输入：约 582 处日志点、约 88 处静默丢弃、19 状态键、17 事件、Level 定义、完整分类表、"不应进入 Log"18 类、"发现但不修改"11 项 |
| `log模块历史/代码审计需求0914.md` | 上一轮审计的需求书 |

---

## 结论：成熟度判定

### ✅ 已足够成熟，可直接进入 Coding Phase

| 设计项 | 成熟度依据 |
|---|---|
| **Queue 架构（方案 A）** | 15 个维度逐项比较，A 全胜；且方案 B 需从零新建队列（当前 CloudManager 无 TX 队列，已实测） |
| **三 Topic 语义分离** | 依赖关系清晰，Up 契约 0 改动，改动仅 4 处纯新增 |
| **128 B 定长 Record 布局** | 字段、偏移、大小、字节序全部冻结；能满足 14 个设计问题 |
| **LogParam 6 B 紧凑形态 + blob 字符串池** | 避免了动态 String；体积守恒已核算 |
| **LogEventId 100 个分类** | 段式 ID 规划 + 每事件 10 列（Level/参数/位置/Flash/Cloud/RT/Ctx）已冻结 |
| **段式 Flash 环（零原地修改）** | 基于 LittleFS COW 特性推导；head/tail 可由目录结构推导，无需持久化；容量/寿命有 10 倍以上余量 |
| **ACK / Retry** | 状态机、超时、退避给出具体数值；`FORCE_ADVANCE` **已降级为可选策略（v1 默认关闭，不冻结）** |
| **CRITICAL 不建独立通道** | 4 条理由成立；措辞已改为"**下一批优先**"，**不承诺秒级** |
| **无独立 Task + LittleFS 单写者** | 项目架构一致性的硬结论；附每轮工作量上界与实时性影响评估 |
| **递归 Logging 四层防护** | 三条真实递归路径已识别，每条都有对应机制 |
| **CloudManager 最小改造** | 4 处新增，含具体代码级建议；改动清单与"不该改"清单已逐项列出 |
| **Config 范围** | 6 个字段进 Config，10 类内部参数明确不进 |
| **测试规划** | 5 组（Core/Flash/Cloud/MultiTopic/高负载）共 48 个用例，含基础设施准备 |

### ⚠️ P1 评审后：阻塞项状态更新（2026-09-15）

| # | 问题 | 状态 |
|---|---|---|
| **Q1** | `cloud_protocol.md` 以哪份契约实现 `log_ack`？ | ✅ **已解决** —— 该文档已按代码实读重写为 **V2.0**，以代码实际行为为准 |
| **Q2** | EMQX ACL 是否允许 `guo_feeder/log`？ | ✅ **已获答复** —— EMQX **尚未配置**，用户会在需要端到端验证时添加。**不阻塞编码**；Cloud 组测试在此期间标记 NOT TESTABLE（见 §21.0） |
| **Q3** | 云端是否接受 `(device_id, boot_seq, seq)` 幂等？ | ✅ **已固化为正式契约**（§13.5 at-least-once + 云端幂等）。原"容忍 `FORCE_ADVANCE` 重复"部分随该机制降级而不再是前置条件 |
| **Q4** | `INFO 不落 Flash` 是否最终确认？ | ✅ **已确认** —— **无任何例外**，撤销原 3 条例外，禁止 EventId 级 `persist` 覆盖 |
| **Q5** | `log.json` 的开关是否允许"立即生效"？ | ⏳ **仍未确认** —— **当前唯一剩余阻塞项** |

### 🔴 需先经 **P1.0 Log Contract Review** 才能落代码

以下 6 项须以 `log模块历史/LogManager-P1契约冻结0915.md` 的冻结值为准：

1. **Level Policy**（无例外）
2. **LogEventId**（100 个）
3. **LogParamId**（86 个）
4. **LogRecord 128 B v2**（无 `persist` / `uploaded`；`boot_seq` uint32）
5. **Flash Segment**（31 条/段）
6. **Cloud Batch / ACK 契约**（at-least-once + 云端幂等）

> **P1.0 通过后才写 `src/log_events.h`（P1.1）。**

### 📌 建议的下一步

1. 人工回答 Q1–Q5（尤其 Q2 可用 MQTT.fx **实测 5 分钟**解决，成本极低、收益最大）。
2. 确认后执行 **P0 前置修复**（5 个独立小 commit，与本设计解耦，可立即开始）。
3. 冻结 **P1（`log_events.h` 契约）** —— 这是所有后续 Phase 的唯一依赖。

---

*本报告为纯设计规划产出。全部结论基于对代码与文档的实测通读（附录 B），未修改任何项目文件，未执行任何 git 操作。*
