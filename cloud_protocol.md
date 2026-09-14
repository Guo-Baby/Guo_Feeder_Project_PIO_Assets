# Cloud Protocol 云通信协议文档

> 项目：Guo Feeder Project（ESP32-S3 N16R8）
> 版本：**V2.0**
> 日期：2026-09-15
> 代码基线：git `ce9ba37`
> 事实来源：**`src/cloud_manager.cpp` / `.h` 的实际实现**
> 配套文档：`workflow_cloud_interface.md`（Workflow 云端同步契约）、`config_manager接口文档.md`

---

## 0. 文档定位与本次修订说明

### 0.1 本文档是什么

描述**设备 ↔ 云平台**之间 MQTT 通信的权威规范：连接参数、Topic 布局、报文外壳、
字段缩写、命令翻译规则、ACK、分片、错误码。

**凡是本文档与代码不一致的地方，以代码为准，并请回来修正本文档。**

### 0.2 为什么要重写（V1.0 的问题）

V1.0 描述的是一套**从未落地**的设计，与当前代码存在系统性偏差。为避免继续误导，
本次按代码事实重写。主要偏差如下：

| 项 | V1.0 的描述 | 代码实际行为 |
|---|---|---|
| Broker | 巴法云 | EMQX Cloud（`n302933b.ala.cn-hangzhou.emqxsl.cn`） |
| Topic | `{uid}/up`、`{uid}/down` | `guo_feeder/up`、`guo_feeder/down`（完整字符串，非 `{uid}` 拼接） |
| 顶层字段 | `v` / `mid` / `d` / `t` / `ts` / `dat` | 上行实际用 **`s` / `c` / `i` / `o` / `k` / `p` / `t` / `e` / `m`** |
| 单条上限 | ≤1024 B（平台限制） | `CLOUD_MSG_LIMIT_MAX = 8128`（`MQTT_BUFFER 8192 - 64`），默认阈值 8000 |
| `t` 字段含义 | `type`（消息类型） | 实际 `t` = **timestamp**；消息类型由 `c`（command）承载 |
| 编码 | "正式用 CBOR" | **上行目前全部是 JSON 文本**。虽然开了 `ARDUINOJSON_USE_CBOR=1`，但 `cloud_send_up()` 走的是 `cloud_compress_uplink()`（文本级字段缩写）+ `cloud_mqtt_publish_text()` |
| 二进制接口 | — | `cloud_send_up_cbor` / `cloud_send_set_cbor` / `cloud_send_up_binary` **头文件有声明、无实现**（无调用点，故链接不报错） |
| 枚举存放 | `cloud_protocol_enum.h` | 该文件**不存在**；枚举实际定义在 `src/cloud_manager.h` |

### 0.3 与相关文档的分工

| 文档 | 负责 |
|---|---|
| **本文档** | 传输层、Topic、报文外壳、字段缩写、ACK、分片、错误码 |
| `workflow_cloud_interface.md` | `workflow.*` 七条命令的请求/响应/Variant/Dirty 语义 |
| `config_manager接口文档.md` | `config_query` / `config_set` 等系统命令 |
| `readme.md` §2.5 / §4.12 | CloudManager 的架构定位（概述层，不重复协议细节） |

---

## 1. 传输层

### 1.1 MQTT 连接参数

来源：`data/config/mqtt.json`（字段名 → `config_get_mqtt_*()` → `cloud_init()`）。

| 配置项 | 字段 | 示例值 | 说明 |
|---|---|---|---|
| Broker 主机 | `mqtt_server` | `n302933b.ala.cn-hangzhou.emqxsl.cn` | |
| 端口 | `mqtt_port` | `8883` | **8883 → `MQTT_TRANSPORT_OVER_SSL`**；否则 TCP |
| Client ID | `client_id` | `guo_feeder_001` | |
| 用户名 | `username` | `GuoFeederDevice` | |
| 密码 | `password` | *（敏感）* | **不写入文档**；⚠️ 见 §9.1 |
| 订阅 Topic | `subscribe_topic` | `guo_feeder/down` | 下行 |
| 发布 Topic | `publish_topic` | `guo_feeder/up` | 上行 |
| CA 证书路径 | `ca_path` | `/emqxsl-ca.crt` | LittleFS 路径；缺失时打日志但继续（不校验） |
| 最大重试次数 | `retry_max` | `30` | ≤0 时用内置 `MQTT_RETRY_MAX = 10` |
| 休眠重试间隔 | `sleep_retry_interval` | `1800000`（30 min） | ≤0 时用内置 `MQTT_FAIL_SLEEP_TIME = 3600000` |
| Keep Alive | `mqtt_keep_alive` | `60` | 由 esp-mqtt 底层保活，**应用层无心跳** |
| ~~重试间隔~~ | `retry_interval` | `10000` | ⚠️ **死配置**：代码中声明了该变量但从未读取（见 §9.2） |

> **Topic 是完整字符串，不是模板。** 设备不会把 `{uid}` 替换成任何值；
> `subscribe_topic` / `publish_topic` 原样使用。

### 1.2 编译期常量（`cloud_manager.cpp`）

| 常量 | 值 | 说明 |
|---|---|---|
| `CLOUD_MQTT_QOS` | `1` | QoS1，进 outbox，等 PUBACK |
| `CLOUD_MQTT_STORE` | `true` | 传给 `esp_mqtt_client_enqueue()` 的 `store` |
| `CLOUD_MQTT_BUFFER_SIZE` | `8192` | MQTT 收发缓冲 |
| `CLOUD_DEFAULT_MSG_LIMIT` | `8000` | 分片阈值（可由 `change_msg_limit` 改） |
| `CLOUD_MSG_LIMIT_MIN` / `_MAX` | `64` / `8128` | 阈值钳位范围 |
| `MQTT_DUP_CACHE_SIZE` | `10` | 下行去重缓存条数 |
| `MQTT_DUP_CACHE_TTL_MS` | `30000` | 去重有效期 |
| `MQTT_RX_SLOT_COUNT` × `_SIZE` | `4` × `8192` | 下行接收环形槽（回调只入队） |

### 1.3 Topic 规范

**当前（V2.0）**：

| 方向 | Topic | 变量 | 订阅/发布 |
|---|---|---|---|
| 下行（Cloud → Device） | `guo_feeder/down` | `mqtt_sub_topic` | **订阅**（`MQTT_EVENT_CONNECTED` 时 1 次） |
| 上行（Device → Cloud） | `guo_feeder/up` | `mqtt_pub_topic` | 发布 |

**⚠️ 命名反常（记录，本次不改行为）**：`cloud_send_set()` 也发布到 `mqtt_pub_topic`
（`cloud_manager.cpp:1465`），并非 `mqtt_sub_topic`。"set / up" 这两个名字在代码中
**都指向同一个上行 Topic**，区别只在串口日志标签。云端不应依赖这个区分。

### 1.4 连接生命周期

```
WiFi 断 → cloud_event_callback(EVENT_WIFI_DISCONNECTED)
            → esp_mqtt_client_stop()，mqtt_connected=false
WiFi 通 → cloud_event_callback(EVENT_WIFI_CONNECTED)
            → wifi_connected=true，清零重试计数
                    ↓
        cloud_task() 发现未连接 → cloud_connect()
                    ↓
        MQTT_EVENT_CONNECTED
            → mqtt_connected=true，置 mqtt_connect_pending
            → esp_mqtt_client_subscribe(down, QoS1)
            → cloud_process_mqtt_events()（主循环）
                 ├─ state_set_bool(STATE_MQTT_STATUS, true)
                 ├─ event_push(EVENT_CLOUD_CONNECTED)
                 └─ cloud_send_set(上线通知)          ← 见 §4.4
```

**重连与休眠（三级）**：

| 级别 | 条件 | 行为 |
|---|---|---|
| ① esp-mqtt 自动重连 | 连接未建立且 client 已 start | 由 esp-mqtt 内部按自身退避重试；每次 `MQTT_EVENT_DISCONNECTED` 使 `mqtt_retry_count++` |
| ② 进入休眠 | `mqtt_retry_count >= retry_max`（配置 30，默认 10） | 打印 `[Cloud] enter sleep retry`；`esp_mqtt_client_stop()`；`mqtt_sleep_mode = true` |
| ③ 休眠唤醒 | 距进入休眠 ≥ `sleep_retry_interval`（配置 30 min） | 清零重试计数 → `cloud_connect()` |

**状态写入**：`STATE_MQTT_STATUS` / `STATE_MQTT_RETRY_COUNT` /
`STATE_MQTT_LAST_CONNECT_TIME` / `STATE_MQTT_LAST_ERROR`。

---

## 2. 报文外壳与字段

### 2.1 两种格式并存

设备**同时支持**两种下行报文格式，由是否存在 `c` 字段判定：

```c
bool compact = !doc["c"].isNull();    // cloud_manager.cpp:1048
```

| | **旧格式**（长字段，调试首选） | **新格式**（紧凑字段，正式） |
|---|---|---|
| 判定 | 无 `c`，通常有 `cmd` | 有 `c` |
| 命令 | `cmd` | `c` |
| 命令 ID | `id` | `i` |
| 对象 | `ob` | `p.o` / `p.object` / `ob` |
| 载荷 | `pl` | `p` |
| 时间戳 | `ts` | `t` |
| 版本 | — | `v` |
| Stable ID | — | `k` |

> **为什么保留旧格式**：无需构造 `v`/`k`（registry 版本与 stable id），
> 手工用串口 / MQTT 工具调试时最省事。**旧格式走"原样透传"**，
> `cmd` 直接作为 CommandManager 的命令名，不做任何翻译。

### 2.2 旧格式字段表

| 字段 | 含义 | 类型 | 必需 |
|---|---|---|---|
| `cmd` | 命令名，直接透传给 CommandManager | string | ✅ |
| `ob` | 操作对象（Action / Workflow / 模块名） | string | 视命令 |
| `id` | 命令唯一 ID，用于关联响应与去重 | string | ✅ |
| `pl` | 载荷对象 | object | 可选 |
| `src` | 来源（如 `"cloud"`） | string | 可选 |
| `ts` | Unix 时间戳 | number | 可选 |

### 2.3 新格式字段表

| 字段 | 含义 | 类型 | 必需 |
|---|---|---|---|
| `c` | 命令（`"action"` / `"workflow"` / `"registry"` / `"system"` / `"query"` / 其他透传名） | string | ✅ |
| `i` | 命令唯一 ID | string | ✅ |
| `v` | Registry 版本号（`c=action`/`workflow` 时**必填且必须匹配**） | number/string | 条件必填 |
| `k` | Stable ID 或 RegistryType（含义随 `c` 变化） | number/string | 条件必填 |
| `p` | 载荷对象 | object | 可选 |
| `t` | Unix 时间戳 | number | 可选 |

### 2.4 上行紧凑键映射（重要）

所有上行消息在 `cloud_send_up()` 内部经 `cloud_compress_uplink()` 做**字段名缩写**，
再发往 `up` Topic。**云端必须按下表解缩写。**

| 缩写 | 全称 | 说明 |
|---|---|---|
| `s` | status | `CloudStatus` 枚举（见 §7.1）；字符串 `success/accepted/running/failed/error/timeout/rebooting` 会被自动转成枚举 |
| `c` | cmd | 命令名 |
| `i` | id | 命令 ID（源字段 `id` 或 `command_id`） |
| `o` | object | 对象（源字段优先级：`object` > `action` > `workflow` > `key` > `ob`） |
| `p` | payload | 载荷（源字段 `payload` 或 `pl`） |
| `t` | timestamp | Unix 秒；**`time_get()` 返回 0（时间无效）时该字段被省略** |
| `e` | error | 错误码（源字段 `error_code` 或 `error`） |
| `m` | message | 可读文本 |
| `v` | version | 版本号 |
| `k` | stable_id | Stable ID |

**其余未列出的字段原样保留**（`cloud_compress_uplink()` 末尾会遍历复制）。

**Registry 查询结果走特化分支**（见 §4.3），字段为 `{c, i, s, r, v, n, cs, d}`。

---

## 3. 下行：Cloud → Device

### 3.1 处理流程（回调不做业务）

```
MQTT_EVENT_DATA（esp-mqtt 任务）
    └─ 长消息重组（mqtt_large_rx_buffer）
        └─ cloud_rx_enqueue()          ← 仅 memcpy 进 4×8192 环形槽
                                          （满则丢弃并打日志）
cloud_task()（loopTask）
    └─ cloud_process_rx_queue()
        └─ cloud_process_rx_message()
             ① DynamicJsonDocument(4096) 解析；失败 → 丢弃
             ② 取 c/cmd、i/id；任一为空 → 丢弃
             ③ cloud_check_duplicate_cmd() 查重 → 命中则丢弃
             ④ if (c == "change_msg_limit") → 协议级处理 + return（§3.4）
             ⑤ cloud_translate_command() → 失败则 cloud_send_protocol_error()
             ⑥ cloud_send_ack()
             ⑦ command_manager_execute()
```

**铁律**：MQTT 回调线程**不做任何业务**，只做入队。所有解析 / ACK / 执行都在主循环。

### 3.2 去重

- 缓存 `cmd_id` 最近 **10** 条，有效期 **30 s**，FIFO 覆盖。
- 命中则**整条消息丢弃**（不回 ACK、不回错误）。
- 因此**每条命令的 `id` / `i` 必须唯一**，否则会被静默丢弃。

### 3.3 命令翻译规则

旧格式（`compact == false`）：`cmd` 与 `ob` **原样透传**，无翻译、无版本校验。

新格式（`compact == true`）：

| `c` | 校验 | 翻译结果 |
|---|---|---|
| `action` / `execute_action` | `v` 必须 == `capability_get_action_version()`；`k` 必须能解析出 runtime_id | `command="execute_action"`，`object=<runtime_id>` |
| `workflow` / `execute_workflow` | `v` 必须 == `capability_get_workflow_version()`；`k` 必须能解析出 runtime_id | `command="execute_workflow"`，`object=<runtime_id>` |
| `registry` | `k` ∈ {0,1,2} | `k=0`→`query_action_registry`；`1`→`query_trigger_registry`；`2`→`query_workflow_registry` |
| `system` | — | `command="system"`，`object = p.o`（或 `p.object`） |
| `query` | — | `command = p.q`，`object = p.o` |
| 其他 | — | `c` 直接作为命令名透传，`object = p.o` / `p.object` / `ob` |

**校验失败** → `cloud_send_protocol_error()`（不进 CommandManager）：

```json
{"c":"result","i":"<cmd_id>","s":5,"e":<error>,"m":"<原因>"}
```

### 3.4 协议级消息：`change_msg_limit`（旁路 CommandManager）

```
{"cmd":"change_msg_limit","id":"1001","pl":4096}          ← 旧格式
{"c":"change_msg_limit","i":"1001","p":4096}              ← 新格式
```

- 在 `cloud_process_rx_message()` 中于 CommandManager **之前**处理（`:1067`）。
- 载荷支持三种形态：数字 / 字符串（按 `:` 或全角 `：` 取后半段转 int）/
  对象（键 `lengh` 或 `length`，⚠️ `lengh` 是代码中的拼写错误，为兼容保留）。
- `< 64` → 返回 `s=5, e=1, m="invalid msg limit"`；`> 8128` → 钳位到 8128。
- 成功返回：`{"c":"result","i":"...","s":0,"m":"msg_limit=N","l":N}`。

> **扩展约定**：未来新增"协议级"消息（不进 CommandManager、不需要业务执行）
> 一律照此落点，在 `change_msg_limit` 分支旁加 `else if` 即可。

---

## 4. 上行：Device → Cloud

**统一入口**：`cloud_send_up(const char* message)` → 压缩 → `up` Topic。
`cloud_send_set()` 也发往 `up` Topic（见 §1.3）。

### 4.1 ACK（命令已受理）

在**命令执行之前**发出（`cloud_process_rx_message` 步骤 ⑥ → ⑦）。

```json
{
  "c": "ack",
  "i": "<cmd_id>",
  "o": "<object>",
  "p": { "result": "received" },
  "t": 1785514668
}
```

**语义**：仅表示"设备已收到并受理"，**不代表执行完成**。
`o` 在 object 为空时省略；`t` 在时间无效时省略。

### 4.2 Result（命令执行结果）

由 CommandManager 生成 → `command_manager_set_result_callback(on_command_result)`
→ `cloud_send_up()`。典型结构：

```json
{
  "s": 0, "c": "result", "i": "<cmd_id>", "t": 1785514668,
  "o": { "...": "业务数据" },
  "command": "workflow.list",
  "status": "success"
}
```

错误结构（`command_send_error()`）：

```json
{
  "cmd": "result", "id": "<cmd_id>", "status": "error",
  "error_code": 5, "message": "..."
}
```

经缩写后 `status`→`s`、`error_code`→`e`、`message`→`m`。

### 4.3 Registry 查询结果

`cloud_compress_uplink()` 对含 `registry` 字段的结果走**特化分支**：

```json
{
  "c": "result",
  "i": "<cmd_id>",
  "s": 0,
  "r": 0,              // RegistryType: 0=action 1=trigger 2=workflow
  "v": 15,             // registry version
  "n": 7,              // count
  "cs": 3735928559,    // checksum
  "d": [ [0,"VALVE_OPEN"], [1,"VALVE_CLOSE"] ]   // [stable_id, runtime_id] 数组对
}
```

**注意**：这是唯一会**压缩嵌套对象结构**（把 `entries[{stable_id,runtime_id}]`
压成 `d[[k,id]]`）的分支。其他消息一律按 §2.4 映射。

### 4.4 上线通知

`MQTT_EVENT_CONNECTED` 后的第一件事（`cloud_process_mqtt_events()`）：

```
cloud_send_set("{\"cmd\":\"system\",\"id\":\"online\",\"src\":\"device\"}")
```

注意它走的是 `cloud_send_set()`，但**同样发到 `up` Topic**。

### 4.5 上行路径小结

```
业务模块 / CommandManager
   │  result_callback（cloud_init() 内注册）
   ▼
on_command_result(json) ──► cloud_send_up(json)
                                  │
                                  ├─ cloud_compress_uplink()  【文本缩写，非 CBOR】
                                  ▼
                        cloud_mqtt_publish_text(up, ...)
                                  ▼
                        esp_mqtt_client_enqueue(qos=1, store=true)
```

⚠️ **离线时 `cloud_send_up()` 直接返回 false，不排队、不落盘**。
需要离线可靠性的数据（如日志）**不能依赖这条路径** —— 这正是 §8 规划 `log` Topic
与设备侧独立环形缓冲的原因。

---

## 5. 分片协议

**状态**：`cloud_publish_fragmented()` 已实现，但 `cloud_send_up()` /
`cloud_send_set()` **均未调用它**。当前上行**实际上不分片**。
以下为已实现的协议，供未来启用（或大消息场景）参考。

触发条件：序列化后长度 > `cloud_msg_limit`（默认 8000）。

| 段 | 载荷 | 说明 |
|---|---|---|
| BEGIN | `{"f":0,"i":"m<millis>","n":<总片数>,"l":<总长度>,"t":<类型>,"c":<crc32>}` | `t`: 0=UP, 1=SET |
| DATA | `[idx_lo, idx_hi, ...data]` | 前 2 字节为小端片序号 |
| END | `{"f":2,"i":"m<millis>"}` | |

- `FragmentType`：`FRAG_BEGIN=0` / `FRAG_DATA=1` / `FRAG_END=2`
- CRC32：标准 IEEE 802.3（init `0xFFFFFFFF`，poly `0xEDB88320` reflected，final xor `0xFFFFFFFF`），覆盖范围 = 整条原始消息。
- 每片大小 = `cloud_msg_limit - 2`。

---

## 6. 错误码

### 6.1 协议层（`ErrorCode`，`cloud_manager.h`）

由 `cloud_translate_command()` / `cloud_send_protocol_error()` 产生，
**命令未进入 CommandManager**。

| 值 | 名称 | 含义 |
|---|---|---|
| 0 | `ERROR_NONE` | 无错误 |
| 1 | `ERROR_INVALID_COMMAND` | 非法 / 缺失命令 |
| 2 | `ERROR_INVALID_OBJECT` | 非法对象，`k`(stable id) 不存在，或 registry type 非法 |
| 3 | `ERROR_VERSION_MISMATCH` | `v` 与设备当前 registry version 不匹配 |
| 4 | `ERROR_NOT_FOUND` | — |
| 5 | `ERROR_BUSY` | — |
| 6 | `ERROR_TIMEOUT` | — |
| 7 | `ERROR_INTERNAL` | — |

### 6.2 业务层（`command_manager.cpp`）

命令已进入 CommandManager，由 `command_send_error()` 产生，回填到 `e` 字段。

| 值 | 名称 | 含义 |
|---|---|---|
| 1 | `CMD_ERROR_UNKNOWN_COMMAND` | 未知命令 |
| 2 | `CMD_ERROR_MISSING_COMMAND` | 缺命令 |
| 3 | `CMD_ERROR_MISSING_OBJECT` | 缺对象 |
| 4 | `CMD_ERROR_ACTION_NOT_FOUND` | Action 不存在 |
| 5 | `CMD_ERROR_WORKFLOW_NOT_FOUND` | **Workflow 不存在**（含 slot 越界 / 重启后已删对象） |
| 6 | `CMD_ERROR_PARAM` | 参数错误 / 缺 `workflow` 对象 |
| 7 | `CMD_ERROR_QUEUE_FULL` | 运行时槽位满 |
| 8 | `CMD_ERROR_DUPLICATE_CMD_ID` | 命令 ID 重复 |
| 9 | `CMD_ERROR_SYSTEM` | 系统错误 |
| 10 | `CMD_ERROR_EXECUTION` | 执行失败（含 `workflow.save` 落盘失败） |
| 11 | `CMD_ERROR_INVALID_PAYLOAD` | 载荷非法：`type` 缺失 / `steps>16` / `params>8` |
| 12 | `CMD_ERROR_NO_FREE_SLOT` | Workflow 槽位已满（16）且无可回收 |
| 13 | `CMD_ERROR_REJECTED` | 修改被拒（运行中 / Critical acquire 失败） |

> **Workflow 相关错误的完整语义见 `workflow_cloud_interface.md` §10。**

---

## 7. 枚举定义

### 7.1 `CloudStatus`（上行 `s`）

| 值 | 名称 | 说明 |
|---|---|---|
| 0 | `STATUS_OK` | 成功（字符串 `success`） |
| 1 | `STATUS_ACCEPTED` | 已受理（`accepted`） |
| 2 | `STATUS_RUNNING` | 执行中（`running`、`rebooting`） |
| 3 | `STATUS_FAILED` | 失败（`failed`、`timeout`） |
| 4 | `STATUS_ERROR` | 协议错误（`error`） |

### 7.2 `RegistryType`（下行 `k`，`c=registry` 时）

| 值 | 名称 |
|---|---|
| 0 | `REGISTRY_ACTION` |
| 1 | `REGISTRY_TRIGGER` |
| 2 | `REGISTRY_WORKFLOW` |

### 7.3 其他

- `FragmentType`：`FRAG_BEGIN=0` / `FRAG_DATA=1` / `FRAG_END=2`
- `CloudMessageType`：`CLOUD_MSG_COMMAND=0` / `_RESULT` / `_ACK` / `_REGISTRY` / `_FRAGMENT`
  （⚠️ 该枚举描述的是**设计意图**。当前代码并不用 `t` 承载消息类型 —— `t` 是 timestamp）

---

## 8. Topic 规划与扩展（含 Log Topic 预留）

### 8.1 当前实现

```
MQTT
├── down   guo_feeder/down    Cloud → Device   命令 + 协议级消息
└── up     guo_feeder/up      Device → Cloud   ACK / Result / Registry / 上线通知
```

### 8.2 🔒 预留：`log` Topic（规划中，**尚未实现**）

> 本节为**前瞻性规范**，代码当前**未实现**。
> 完整设计见 `log模块历史/LogManager详细设计规划0915.md`。

**目标形态**：

```
MQTT
├── down   guo_feeder/down    命令 + log_ack
├── up     guo_feeder/up      业务上行（语义不变）
└── log    guo_feeder/log     ★ 仅 LogManager 的结构化日志批次
```

**为什么独立成 Topic**：

| 理由 | 说明 |
|---|---|
| 不污染既有 `up` 契约 | `cloud_compress_uplink()` 是按字段名映射的缩写器，日志批次结构完全不同 |
| 消费路径不同 | 业务上行进"命令 / 状态"链路；日志进"时序 / 告警"链路 |
| 流量特征不同 | 业务上行是偶发突发；日志是**可抑制的批量流**，需要独立限流 |
| 离线补偿不同 | 只有日志有"补发历史"需求；混入 `up` 会让"补发"变成"重放业务消息" |
| 可独立关闭 | 云端可在不中断业务的前提下停掉日志订阅 |

**规划要点**（实现时以设计文档为准）：

| 项 | 规划值 |
|---|---|
| 新增配置 | `data/config/mqtt.json` 增加 `log_topic`（默认 `guo_feeder/log`） |
| 载荷编码 | **CBOR**（定序数组），批 ≤16 条记录，单条 ≤4096 B，不启用分片 |
| 记录格式 | 128 B 定长：`version / level / flags / param_count / seq / boot_seq / event_id / uptime_ms / timestamp / blob / params[8] / crc32` |
| 落盘级别 | 仅 WARN / ERROR / CRITICAL（INFO 不落 Flash） |
| QoS / store | QoS1，**store=0**（耐久性由设备自身的 Flash 段环保证，避免双重持久化） |
| ACK | **批次级**，走 **`down`** Topic：`{"c":"log_ack","i":"<id>","p":{"b":<boot_seq>,"f":<seq_from>,"t":<seq_to>}}`<br>在 `cloud_process_rx_message()` 里像 `change_msg_limit` 一样**旁路 CommandManager**（§3.4） |
| 去重键 | 云端用 `(device_id, boot_seq, seq)` 幂等；重传时 `seq` 不变 |
| 新增订阅 | **无**。设备对 `log` 只发布不订阅；`log_ack` 复用已有的 `down` 订阅 |

**CloudManager 侧的最小改造（4 处纯新增，现有函数签名 0 改动）**：

```c
enum CloudRoute { CLOUD_ROUTE_UP = 0, CLOUD_ROUTE_LOG = 1 };

bool cloud_send_route(CloudRoute route, const uint8_t* data, size_t len);

typedef void (*CloudLogAckCallback)(uint16_t boot_seq, uint32_t from, uint32_t to);
void cloud_set_log_ack_callback(CloudLogAckCallback cb);
```

依赖注入在 `main.cpp` 完成：**LogManager 不 include `cloud_manager.h`，
CloudManager 也不 include `log_manager.h`**（避免循环依赖）。

**⚠️ 上线前置条件**：需确认 EMQX 的 ACL 允许用户 `GuoFeederDevice` 发布到
`guo_feeder/log`。若被拒，表现为 `cloud_send_route` 返回 false（串口打
`[Cloud] publish FAIL guo_feeder/log`），而业务 `up` 完全正常。

### 8.3 新增 Topic 的通用规范

1. **配置化**：新 Topic 必须在 `mqtt.json` 增加独立字段，**禁止**从 `publish_topic` 推导。
2. **纯新增**：CloudManager 的既有发送函数签名不得改变。
3. **单向**：设备侧新增 Topic 若只需发布，不必新增订阅；其 ACK / 控制消息复用 `down`。
4. **旁路**：协议级消息（如 `log_ack`）在 `cloud_process_rx_message()` 中
   于 CommandManager 之前处理，不占用命令命名空间、不进 `cmd_id` 去重缓存。
5. **文档同步**：新增 Topic 必须同步更新本文档 §1.3 与 §8。

---

## 9. 已知问题与待办

| # | 问题 | 位置 | 影响 | 建议 |
|---|---|---|---|---|
| 9.1 | **MQTT 调试横幅明文打印密码** | `cloud_manager.cpp:1205-1228` | 串口泄露凭据；日志通道建成后风险放大 | 整块加 `#ifdef`，且永不输出 password |
| 9.2 | `retry_interval` 声明但从未读取 | `cloud_manager.cpp:68` | 配置项无效（当前 10000 不起作用） | 要么接入退避逻辑，要么从配置移除 |
| 9.3 | `cloud_send_up_cbor` / `_set_cbor` / `_up_binary` **有声明无实现** | `cloud_manager.h:37-50` | 误导后续开发者；`log` Topic 需要真正的二进制通道 | 实现或删除 |
| 9.4 | `cloud_send_set()` 与 `cloud_send_up()` **发往同一 Topic** | `:1465` / `:1495` | "set" 名不副实；文档易误读 | 保持现状（不改行为），本文档已注明 |
| 9.5 | `cloud_publish_fragmented()` 未被上行主路径调用 | — | 超 8128 B 的消息会发布失败而非分片 | 明确是否需要启用 |
| 9.6 | `change_msg_limit` 的 `lengh` 拼写错误 | `:834` | 为兼容保留的错误键名 | 保留兼容，新键优先 |
| 9.7 | **`platformio.ini` 的 `platform` 与 `lib_deps` 未固定版本** | `platformio.ini` | 构建漂移会改变 LittleFS / 二进制格式行为 | 固定版本（独立任务） |
| 9.8 | 上行实际是 JSON 文本，但 `ARDUINOJSON_USE_CBOR=1` 已开 | — | 带宽未达设计预期 | `log` Topic 规划为 CBOR 是迁移的第一步 |

---

## 10. 调试与测试

### 10.1 串口透传（推荐）

CloudManager 的下行解析与 MQTT 走**同一条** `command_manager_execute()` 链路，
因此可用串口直接验证协议，无需 MQTT：

```
cm {"cmd":"workflow.list","ob":"-","id":"8010","p":{}}
cm {"c":"workflow.list","i":"8010","p":{}}
```

批量回归：

```bash
python test/serial_batch.py <COM> <日志文件> <用例文件> [每条等待秒]
# 用例格式：  cm {...} ||| 期望子串
```

### 10.2 观测点

| 串口标签 | 含义 |
|---|---|
| `[Cloud] MQTT RX` | 收到下行报文（随后打印完整 JSON） |
| `[Cloud] UP-ACK id=... obj=...` | 发出 ACK |
| `[Cloud] UP-RESULT len=...` | 发出命令结果 |
| `[Cloud] UP JSON` | 上行报文全文（压缩后） |
| `[Cloud] ENQ id=... outbox=N->M len=...` | 已进 esp-mqtt outbox |
| `[Cloud] publish FAIL <topic> len=...` | 发布失败（`msg_id < 0`） |
| `[Cloud] MQTT published/ack msg_id=... outbox=...` | 收到 PUBACK |
| `[Cloud] enter sleep retry` | 重试达上限，进入休眠 |

### 10.3 验证清单

- [ ] `up` 与 `down` 互不串扰（用 MQTT.fx 分别订阅观察）
- [ ] 同一 `id` 连发两次 → 第二次被去重丢弃（串口 `[Cloud] Command duplicate`）
- [ ] `v` 故意写错 → 返回 `s=5, e=3`
- [ ] `k` 越界 → 返回 `s=5, e=2`
- [ ] `change_msg_limit` → 回 `s=0, m="msg_limit=N"`
- [ ] 断网 → `STATE_MQTT_STATUS=false`，重连后发出 `{"cmd":"system","id":"online"}`
- [ ] **（未来）`log` Topic**：`up` 与 `log` 互不污染；`log` 被 ACL 拒绝时业务不受影响

---

## 附录 A：命令清单（速查）

| 分类 | 命令 | 文档 |
|---|---|---|
| 执行 | `execute_action` / `c:"action"` | `readme.md` §6 |
| 执行 | `execute_workflow` / `c:"workflow"` | `readme.md` §7 |
| 查询 | `query_...` / `c:"query"`（`p.q`） | `readme.md` §8 |
| 系统 | `system` / `c:"system"`（`p.o`） | `config_manager接口文档.md`、`system_command接口文档.md` |
| Registry | `query_action/trigger/workflow_registry` / `c:"registry"` | 本文档 §4.3 |
| 协议级 | `change_msg_limit` | 本文档 §3.4 |
| Workflow 管理 | `workflow.sync_info / list / get / create / set / delete / save` | **`workflow_cloud_interface.md`** |
| 日志（规划） | `log_ack`（下行） | 本文档 §8.2 |

---

*本文档由代码实读重写（基线 `ce9ba37`）。若后续修改 CloudManager 的报文结构、
字段缩写、Topic 或错误码，必须同步更新本文档 —— 见 §0.1 的约定。*
