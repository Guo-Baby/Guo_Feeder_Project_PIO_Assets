# Cloud Protocol 云通信协议文档

> 项目：Guo Feeder Project（ESP32-S3 N16R8）
> 版本：**V2.0**
> 日期：2026-09-15
> 代码基线：git `ce9ba37`
> 事实来源：**`src/cloud/cloud_manager.cpp` / `.h` 的实际实现**
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
| 枚举存放 | `cloud_protocol_enum.h` | 该文件**不存在**；枚举实际定义在 `src/cloud/cloud_manager.h` |

### 0.3 与相关文档的分工

| 文档 | 负责 |
|---|---|
| **本文档** | 传输层、Topic、报文外壳、字段缩写、ACK、分片、错误码；**§11 = 管理端「控制面 API」**（属**控制面**，**非**设备 MQTT 协议） |
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
| Client ID | `client_id` | `guo_feeder_001` | ⚠️ **必须全局唯一**：固定值会让第二台设备触发 session takeover 把第一台踢下线 ⇒ **P0 改为 `dev_<device_id>`** |
| 用户名 | `username` | `GuoFeederDevice` | P1 改为 `dev_<device_id>`（一机一账号） |
| 密码 | `password` | *（敏感）* | **不写入文档**；⚠️ 见 §9.1 |
| 订阅 Topic | `subscribe_topic` | `guo_feeder/down` | 下行；**P0 起为模板** `guo_feeder/<device_id>/down`（见下方说明） |
| 发布 Topic | `publish_topic` | `guo_feeder/up` | 上行；**P0 起为模板** `guo_feeder/<device_id>/up` |
| CA 证书路径 | `ca_path` | `/emqxsl-ca.crt` | LittleFS 路径；缺失时打日志但继续（不校验） |
| 最大重试次数 | `retry_max` | `30` | ≤0 时用内置 `MQTT_RETRY_MAX = 10` |
| 休眠重试间隔 | `sleep_retry_interval` | `1800000`（30 min） | ≤0 时用内置 `MQTT_FAIL_SLEEP_TIME = 3600000` |
| Keep Alive | `mqtt_keep_alive` | `60` | 由 esp-mqtt 底层保活，**应用层无心跳** |
| ~~重试间隔~~ | `retry_interval` | `10000` | ⚠️ **死配置**：代码中声明了该变量但从未读取（见 §9.2） |

> **V2.0 语义：Topic 原样使用，设备不会替换 `{uid}`。** 三个 Topic 配置项都是**完整字符串**，
> `cloud_init()` 读入后直接使用（`cloud_manager.cpp:1462–1479`）。
>
> **★ V3 变更（P0）** —— 见 `docs/architecture/P0-设备身份与Topic隔离设计.md`：
> 引入**唯一一种**占位符 `<device_id>`（如 `guo_feeder/<device_id>/down`），由固件新增的
> `device_topic_render()` **显式替换**后使用。**与上面这句不冲突**，因为：
>
> | 写法 | 谁替换 | 固件是否处理 |
> |---|---|---|
> | `{uid}` | — | ❌ **永远不替换**（V1.0 遗留写法，已废弃） |
> | `${username}` / `${clientid}` | **EMQX ACL** 侧 | ❌ 固件**不处理**（属云侧占位符，故意用不同写法区分） |
> | `<device_id>` | **固件 `device_topic_render()`** | ✅ 显式替换（P0 新增能力） |
>
> 且 **"不含 `<device_id>` 的模板原样放行"** —— 这是 P0 的 legacy 兼容与**配置级回滚通路**
> （把 Topic 改回 `guo_feeder/down` 即回到 V2.0 行为，**无需回退固件**）。

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

> **★ 第二条命名空间**：自 P2 起设备**同时**在 `homie/5/<device_id>/#` 上做 **Homie 5 投影**（与上表并存，非替代）。
> 完整契约见 **§12**；**第三条上行 `log`（`guo_feeder/<device_id>/log`）已于 P3 落地**（§8.2）。
>
> **★ `/up` 上并存三类载荷**：命令结果 / 上线通知（§4.1–§4.4）· **telemetry v1（§4.5）**。
> 三者**共用同一 topic**（`guo_feeder/<device_id>/up`，**不新增 topic**）⇒ **分流责任在 EMQX Rule 层**
> （`H-WP4C-03`；**Worker 不解析 payload，firmware 不改 topic**）。

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
bool compact = !doc["c"].isNull();    // cloud_manager.cpp:1154
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

> **⚠️ telemetry（§4.5）不经本节映射** —— `cloud_send_telemetry()` 把信封 JSON **原文**直接发往 `up`，
> **不经过 `cloud_compress_uplink()`**、**不做字段缩写**（同理，`/log` 的 Cloud Log Envelope 亦不经缩写，见 §8.2）。
> ⇒ 云端**不得**对 telemetry 报文套用本表解缩写。

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

### 3.3.1 凭据类命令：`system.credential_set`（Phase D-2 新增）

**用途**：云端 provisioning 下发一台设备**专属**的 MQTT 凭据（一机一密）。

下行：

```json
{
  "c": "system",
  "i": "cred_288485896ce4_g2",
  "t": 1785514668,
  "p": {
    "o": "credential_set",
    "action": "install",
    "username": "dev_288485896ce4",
    "password": "<32 字符 base62>",
    "generation": 2,
    "slot": 1
  }
}
```

| 字段 | 必需 | 说明 |
|---|---|---|
| `o` | ✅ | 固定 `credential_set` |
| `action` | 可选 | 目前仅 `install`（缺省即 `install`） |
| `username` | ✅ | 必须为 `dev_<device_id>` |
| `password` | ✅ | 32 字符 base62 |
| `generation` | ✅ | **必须严格递增**（小于等于已生效值 ⇒ `e:16` 拒绝，防重放） |
| `slot` | 可选 | 0/1（建议槽位；设备在"不覆盖当前生效槽"的前提下采纳） |

**★ 来源安全门控（冻结：Phase C Q13 —— `CommandMessage.source` 是 security context）**

| `source` | 是否允许 `credential_set` |
|---|---|
| `cloud`（CloudManager 下行） | ✅ **唯一合法来源** |
| `serial`（串口 `cm {}`） | ❌ 拒绝（`e:13`），**不写 NVS** |
| 空 / 内部构造 | ❌ 拒绝（`e:13`） |

⇒ `credential_set` **不进入普通业务命令通路**：`system_router` 以 allow-list 方式
**最先**拦截该对象，走独立分支（解析 → 门控 → 转交 CredentialManager），
不与其他 `system` 对象共用通路，也不触发任何业务副作用（不注册 Action/Trigger、不改 Workflow/config）。

**受理帧（`status=accepted`，★ 不含 `confirmed`）**：

```json
{"cmd":"result","id":"cred_288485896ce4_g2","command":"system",
 "object":"credential_set","status":"accepted","stage":"installing","generation":2}
```

**确认帧（`credential_confirm`，★ 唯一可触发云端 `PROVISIONING → ACTIVE`）**：

由设备在**用新凭据真实连上（CONNACK 成功）并完成 promote** 后上报：

```json
{"cmd":"result","id":"cred_288485896ce4_g2","command":"result",
 "object":"credential_set","status":"success","confirmed":true,
 "generation":2,"message":"connack_ok"}
```

失败帧（不回 `confirmed`）：

```json
{"cmd":"result","id":"...","object":"credential_set","status":"error",
 "generation":2,"message":"auth_failed"   // 或 timeout / net_error
}
```

> **★ 冻结语义（Phase C Q10）**：**只有** `object=="credential_set"` **且** `confirmed==true`
> 才允许云端把凭据从 `PROVISIONING` 升为 `ACTIVE`。
> **普通命令 ACK / 受理帧一律不得改变凭据状态** —— 这也是受理帧刻意不带 `confirmed` 的原因。

**设备侧行为（Phase D-2）**：

1. 写入**非当前生效**的 slot，状态 `TESTING`（不覆盖 `ACTIVE` ⇒ 失败即回退，不影响现有连接）
2. 用该凭据试连，`client_id` 使用**试连专用**值 `dev_<device_id>_t<generation>`
   （与正式 `client_id` 不同 ⇒ 不踢掉既有会话，冻结：方案 B）
3. CONNACK 成功 ⇒ promote（置 `ACTIVE`、旧槽置 `INVALID`、记录 `generation`）⇒ 上报确认帧
4. **认证类失败**（CONNACK 4/5）⇒ 丢弃候选槽，回退原凭据
5. **网络类失败**（TLS/DNS/超时）⇒ **不判凭据失败**，计次重试，超时（45 s / 5 次）后才回退
6. 全程**不打印凭据明文**（串口、日志、上行载荷）

> **★ 确认帧的投递保证（Phase D-3 上板实测补丁）**
>
> 确认帧（以及回滚失败帧）走 `esp_mqtt_client_enqueue`（QoS 1 + store）⇒
> **"入队成功" ≠ "已投递"**。而 promote 会立即促使 CloudManager 重建客户端
> （`stop + destroy`），若紧接着重建，**队列里的确认帧会被一起销毁**
> —— 实测现象：串口出现 `enqueue OK` + `[Cloud UP] OK`，但云端**永远收不到 confirm**
> （凭据停在 `PROVISIONING`，300 s 后被对账标为 `timeout:confirm`）。
>
> **因此 CloudManager 在凭据驱动的换连前，必须先等待 outbox 排空**：
> 非阻塞地在 `cloud_task()` 里轮询 `esp_mqtt_client_get_outbox_size()`，
> 满足「outbox == 0 且已过最短宽限 400 ms」或「超过上界 2500 ms」才执行重建。
> 串口标志行：`[Cloud] credential switch: outbox=N -> rebuild`（超时则带 `(drain timeout)`）。

**禁用与保护**：
- `username` / `password` **不得**出现在任何上行载荷、日志或 D1 中（密码只允许 `cloud → device` 单向）
- 凭据类命令的**下行原始报文不打印**（CloudManager 只打印命令名与 id）

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

### 4.5 Telemetry（遥测上行 · **已冻结 v1**）

> **状态**：**schema 已冻结**（`H-WP4-FW-05` / `06` / `07` / `08` = **`CLOSED`**）· **固件已实现 publisher**
> （`src/services/telemetry_publisher.*`，冻结对齐 `d45f7c3` → 优化 `9dfc7b5`）· **默认 runtime disabled**。
> 权威：`docs/review/P3-WP4-Firmware-Telemetry-Decisions.md`（冻结裁决）·
> `docs/review/P3-WP4-Firmware-Telemetry-Design-Freeze-Review.md`（设计评审）。
> ⚠️ **端到端尚未联调**（EMQX Rule 分流未落地 ⇒ `H-WP4C-03`，见 `docs/review/P3-WP4C-EMQX-Rule-Design.md`）。

**Topic（与业务上行共用 · 未新增）**：`guo_feeder/<device_id>/up`（模板 `GF_TOPIC_TPL_UP`）

#### 4.5.1 Payload contract（v1 · **5 字段** · 字段顺序即契约）

```json
{
  "ts": 1785514668,
  "type": "climate.temperature",
  "value": 25.3,
  "seq": 42,
  "schema_version": 1
}
```

| 字段 | 类型 | 说明 |
|---|---|---|
| `ts` | **number** | Unix **秒** |
| `type` | **string** | 类型标识符（T1 点分命名空间；**封闭词表 · 追加式演进 · 永不改名**） |
| `value` | **number \| string \| boolean** | 观测值（编码规则见 §4.5.2） |
| `seq` | **number** | `uint32` · **boot lifetime 计数**（见 §4.5.3） |
| `schema_version` | **number** | **v1 = `1`** |

**🔴 明确不进入 payload**：

| 不发送 | 原因 |
|---|---|
| **`device_id`** | **身份来源 = MQTT topic**（`guo_feeder/<device_id>/up` 的**第 2 段**）—— payload 内**不重复** |
| **`source`** | 来源由 `type` 的域名前缀隐含（`climate.*` / `weight.*`） |
| `boot_seq` | v1 不使用（`seq` 语义见 §4.5.3） |

> ⇒ **设备身份必须从 topic 提取**（对齐 `DT:WP-4` `ID-3`：payload 内 `device_id` **永远忽略**）。
> 云端 `telemetry_history.device_id` 由**服务端**按 topic / auth 上下文填充。

#### 4.5.2 `value` 编码规则（冻结 · **三态**）

| 逻辑类型 | JSON 形态 | 例 |
|---|---|---|
| **number** | **JSON number** | `25.3` · `1042.0` |
| **state** | **JSON string** | `"ready"` |
| **boolean** | **JSON boolean** | `true` / `false` |

**🔴 禁止**：

| 禁止项 | 说明 |
|---|---|
| **bool 使用 `0` / `1`** | 布尔值**必须**是 JSON `true` / `false`，**不得**用数字编码 |
| **自定义字符串 bool** | 不得用 `"true"` / `"false"` / `"yes"` / `"on"` 等字符串代替 JSON boolean |

> **单位不进载荷** —— 单位绑定在 `type` 上（`climate.temperature` = °C · `climate.humidity` = %RH · `weight.value` = g）。
> 小数位：定点 **2 位**、去尾零（由设备侧 `type` 语义决定）。

#### 4.5.3 `seq` 语义（冻结）

| 项 | 冻结值 |
|---|---|
| 形态 | **`uint32`** |
| 生命周期 | **boot lifetime 单调计数**（每次开机归零） |
| 持久化 | **不持久化**（不写 NVS、不落盘） |
| 与 log 的关系 | **不复用 log `seq`**（不借用 `boot_seq` / `seq`） |

**云端幂等键** = **`(device_id, type, ts, seq)`**（`ux_th_dedup`，`migrations/0003_p3_history_tables.sql`）。

#### 4.5.4 时间有效性（**implementation constraint** · 非配置项）

| 条件 | 行为 |
|---|---|
| **`ts` 无效**（时间未同步） | **telemetry value sample 不发送** ⇒ **不产生 `ts = 0` 的历史数据** |
| `ts` 有效 | 正常上报（`ts` = Unix 秒） |

**为什么**：`telemetry_history` 是时间序列（查询路径 `ix_th_device_ts`）；`ts = 0` 的行**无查询价值**；
且 `seq` 不持久化 ⇒ 跨 boot 的 `(device_id, type, 0, seq)` 会**碰撞并被幂等静默吸收**。

> ⚠️ 本条是 **implementation constraint**，**不是配置项**（不进 `mqtt.json`）。
> ⚠️ **实现状态**：当前 publisher **尚未**执行该抑制（仍按 `ts = 0` 上报）⇒ 属**待实施项**（残留 `R-FRZ-2`）。

#### 4.5.5 配置与门禁（**两道门**）

| 门 | 层 | 默认 |
|---|---|---|
| **`GF_TELEMETRY_ENABLED`** | **编译期** | **`0`（关闭）** —— 代码**不进入镜像**（零成本空实现） |
| **`mqtt.json` → `telemetry.enable`** | **运行时** | **`false`** —— 已编译但**不发送** |

> **有效发送 = 两者皆真**。另：`telemetry.interval_ms` 默认 **`300000`**（5 min）。
> 权威：`docs/review/P3-WP4-Firmware-Telemetry-Decisions.md` §`H-WP4-FW-06 Telemetry Configuration Freeze`。

#### 4.5.6 消费路径（规划）

```
设备 ──MQTT──► guo_feeder/<device_id>/up
                    │
                    ├─(命令结果 / 上线通知)──► 既有 command 链路
                    └─(telemetry v1)─────────► EMQX Rule 分流 ──► POST /api/history/ingest ──► telemetry_history
```

> ⚠️ **分流尚未落地**：命令结果与 telemetry **共用 `/up`** ⇒ 需 **EMQX Rule 层分流**（`H-WP4C-03`）。
> **Worker 不解析 telemetry · firmware 不改 topic · 不改 `/up`**。

#### 4.5.7 首版范围（v1 **KEEP**）

`climate.temperature` · `climate.humidity` · `weight.value`

**DEFER**（**不删除能力**，后续启用无需重新冻结协议）：
`valve.state` · `wifi.state` · `online.state` · `time.valid` · `weight.error`

### 4.6 上行路径小结

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

telemetry_publisher ──► cloud_send_telemetry(json 原文)
                                  │  ★ 不经 cloud_compress_uplink（§4.5）
                                  ▼
                        cloud_mqtt_publish_binary(up, ...)   ← 同一 up Topic
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
| 13 | `CMD_ERROR_REJECTED` | 修改被拒（运行中 / Critical acquire 失败 / **凭据命令来源不合法**） |
| 14 | `CMD_ERROR_CRED_INVALID_PAYLOAD` | 凭据载荷非法（缺 `username`/`password`/`generation`、超长、`action` 不支持） |
| 15 | `CMD_ERROR_CRED_STORE_FAIL` | 凭据写入 NVS（`gfcred`）失败 |
| 16 | `CMD_ERROR_CRED_GENERATION_STALE` | `generation` 非递增 ⇒ 判为陈旧/重放 |

> **Workflow 相关错误的完整语义见 `workflow_cloud_interface.md` §10。**
> **凭据类命令（14–16）语义见 §3.3.1。**

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

## 8. Topic 规划与扩展（`log` Topic **已实现**）

### 8.1 当前实现

```
MQTT
├── down   guo_feeder/down    Cloud → Device   命令 + 协议级消息
└── up     guo_feeder/up      Device → Cloud   ACK / Result / Registry / 上线通知
```

### 8.2 ✅ `log` Topic（**已实现** · P3 · `WP4-FW-LOG`）

> **状态**：**固件侧已实现**（Committish `dae7966` · 2026-10-11）。
> 实施规格见 `docs/review/P3-WP4-Firmware-Log-Implementation.md`；
> 设计评审与人工裁决留痕见 `docs/review/P3-WP4-Firmware-Integration-Design-Review.md`（§9）。
> ⚠️ **软件侧已就绪，端到端 `NOT RUN`**（未部署 / 未联调；EMQX Action 切换尚未授权）。

**Topic（未变）**：

```
MQTT
├── down   guo_feeder/down    命令 + log_ack
├── up     guo_feeder/up      业务上行（语义不变）
└── log    guo_feeder/<device_id>/log   ★ LogManager 结构化日志上行
```

> `log_topic` 配置键的默认值为 **`guo_feeder/<device_id>/log`**（模板 `GF_TOPIC_TPL_LOG`）。

#### 8.2.1 上行载荷契约（**已变更**）

| | 变化前 | **变化后（当前实现）** |
|---|---|---|
| `/log` 载荷 | 设备直接发送 **CBOR payload**（批次，≤16 条记录） | 设备发送 **Cloud Log Envelope JSON**（逐条，`count = 1`） |

**Cloud Log Envelope（JSON · 字段顺序即契约）**：

```json
{
  "device_id":      "<12 位小写 hex，必须与 topic 中的 <device_id> 一致>",
  "boot_seq":       <uint32>,
  "seq":            <uint32>,
  "ts":             <uint32>,
  "level":          <uint8>,
  "payload_base64": "<Base64>"
}
```

**`payload_base64` 的确切定义**：

```
payload_base64 = Base64( 完整 log_cbor_encode_batch(count = 1) 的 CBOR bytes )
               = Base64( 12 项定序数组 + 1 × bstr(128) )
```

- **不是**裸 128 B `LogRecord`；**不做**二次 JSON 化 / 字段映射 / 截断 / 自定义信封内部字段。
- 字节数随记录内容浮动（`seq` / `ts` 的 CBOR 整数长度决定），**上限 = `log_cbor_batch_max_size(1)` = `188 B`**。
- 逐条语义：**1 个 MQTT log message = 1 条 `log_history` record**（`count = 1`）。

#### 8.2.2 边界与不变量（**冻结**）

| 项 | 约定 |
|---|---|
| **Worker remains CBOR-blind** | 云端 ingest **不解析 `payload_base64`**：只做 Base64 承载，按**不透明字符串**原样落库 |
| **Worker 不解析 `payload_base64`** | 同上；Worker 侧不存在任何 CBOR 解码路径 |
| **CBOR schema ≠ cloud envelope schema** | **两层，互不隶属**：外层 = 本节的 envelope JSON（cloud 契约）；内层 = 冻结的 LogManager CBOR 批次（`src/log/log_cbor.h`，键值 `0..11`）。**内层不属于 cloud envelope schema**，其消费者是 LogManager 语义层，不是 ingest |
| **LogRecord binary format unchanged** | 128 B packed 布局**逐字未改**（`src/log/log_events.h`） |
| **topic unchanged** | `guo_feeder/<device_id>/log`；**不新增 topic / 不新增 ACL**（P2 的 5/5 覆盖已含 `/log`） |
| **protocol version unchanged** | **不新增 protocol version**；命令协议与 telemetry 协议**均未改动** |
| **侧信道计数** | `drop_*` / `flags.seq_reliable` 仍置于 **CBOR 批次头**（键值不变），随 `payload_base64` 承载 |

#### 8.2.3 传输参数（实现值）

| 项 | 值 |
|---|---|
| 配置键 | `data/config/mqtt.json` → `log_topic` |
| QoS / store | QoS1，**store=0**（耐久性由设备自身 Flash 段环保证，避免双重持久化） |
| 分片 | **不启用**（单条信封远小于 4096 B 上界） |
| 编码位置 | `src/log/log_envelope.{h,cpp}`（freestanding · 可在主机 wasm32 字节级验证） |
| 发送入口 | `cloud_send_log()`（**CloudManager 只做 transport，不解释载荷**） |
| 新增订阅 | **无**。设备对 `log` 只发布不订阅 |
| ACK | 设计为**批次级**、走 **`down`** Topic（`{"c":"log_ack",…}`，旁路 CommandManager）—— ⚠️ **见 §8.2.4：`log_ack` 尚未实现** |
| 去重键 | 云端用 `(device_id, boot_seq, seq)` 幂等；重传时 `seq` 不变 |

#### 8.2.4 ⚠️ 已知遗留（**登记，未修复**）

| 项 | 说明 |
|---|---|
| **`log_ack` 未实现** | 属 `计划:WP-5`（受 `G-4`）⇒ 设备侧 Flash 段环**无回收路径**，容量**硬上限 496 条** |
| **吞吐上限** | `count = 1` + `LOG_TX_MIN_INTERVAL_MS = 500 ms` ⇒ 上行 **≤ 2 msg/s** |
| **端到端未验证** | 设备未烧录、EMQX Action 未切换、Worker 未联调 ⇒ `NOT RUN` |

> **历史沿革**：本节原为「`log` Topic 预留（规划中，尚未实现）」，描述的是 CBOR 批次 + 批次级 ACK 的规划形态。
> 该规划中的 **CBOR 编码器**（`log_cbor.h`）与 **128 B 记录格式**均已按原样实现且**未变更**；
> 仅**上行承载形态**由「原始 CBOR 批次」改为「逐条 Cloud Log Envelope JSON」。
> 更早的完整设计见 `docs/archive/log/LogManager详细设计规划0915.md`。

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

## 11. Control Plane API（管理端 HTTP 契约 —— **非设备协议**）

> ⚠️ **本章属于「控制面（Control Plane）」，不属于设备 MQTT 协议。**
> §1–§10 描述的是**设备 ↔ 云**的 MQTT 报文；本章描述的是**云端 Worker 暴露给管理端（运维 / APP 后端）的 HTTP 端点**。
> 二者**没有直接关系**：绑定 / 解绑 / 转让是**控制面操作，不要求设备在线**。
> **代码真源**：`Cloudflare_Assets/guo-feeder-api/src/` 下的 `binding.js` · `index.js` · `credential.js`。
> 设计稿与实现的差异桥接见固件仓 `docs/architecture/P1-5-Final-Review.md`；**本章与代码冲突时以代码为准。**

### 11.1 通用约定

| 项 | 值 |
|---|---|
| 前缀 | `/api/admin/**` |
| 鉴权 | `Authorization: Bearer <ADMIN_API_TOKEN>`（缺失 / 错误 / 未配置 ⇒ **同一个 401 响应体**） |
| 方法 | 变更一律 `POST`；查询 `GET` |
| 响应 | `application/json`；错误体 `{"error":"<snake_case>", …}`；成功体含 `http` 字段（与状态码同值） |
| `user_id` | 正则 `^[A-Za-z0-9_.:@-]{1,64}$`；**区分大小写** |
| `device_id` | 正则 `^[0-9a-f]{12}$`（12 位小写 hex） |
| `reason` | 可选字符串，**≤200 字符**；超限返回 `400 invalid_reason`（**不截断**） |
| 幂等 | **语义幂等** —— 重复调用不改状态、不写事件；**不引入** `Idempotency-Key` |
| 与凭据的边界 | **绑定 / 解绑 / 转让一律不吊销凭据**（归属关系 ≠ MQTT 身份）；**绑定不要求设备在线** |

> **owner 的唯一真源 = `device_binding WHERE unbound_at IS NULL`。** `device.state` 只是**摘要**，
> **禁止**以"设备状态是 `BOUND`"反推存在 active binding。

### 11.2 `POST /api/admin/device/:id/bind`

**请求**：`{ "user_id": "…", "reason": "…"? }` —— `user_id` **必填**

| 情形 | HTTP | 响应 |
|---|---|---|
| 首次绑定 | 200 | `{http:200, device_id, user_id, bound:true, idempotent:false}` |
| 同一 owner 重复 | 200 | `{http:200, device_id, user_id, idempotent:true}`（**零写入**） |
| 已有**其他** owner | 422 | `{error:"already_bound", device_id, owner}` |
| `user_id` 非法 / 缺失 | 400 | `{error:"invalid_user_id"}` |
| 设备不存在 | 404 | `{error:"device_not_found"}` |
| 未注册（`FACTORY` / `CLAIM_PENDING`） | 422 | `{error:"device_not_registered"}` |
| 已退役 | 422 | `{error:"device_revoked"}` |
| 事务失败 | 500 | `{error:"batch_failed", device_id}` |

**幂等行为**：已是同一 owner ⇒ 直接返回 `idempotent:true`，**不写 binding、不写事件、不改 device**。

### 11.3 `POST /api/admin/device/:id/unbind`

**请求**：`{ "reason": "…"? }`

- **`forced` 规则**：由服务端**固定为 `true`**，**客户端不可控制** —— 请求体里若带 `forced` 一律被忽略，
  事件 `payload.forced` 恒为 `true`。
- **副作用**：关闭 binding + `device.state→UNBOUND` + `unbind` 事件（**同一事务**）；**不吊销凭据**。

| 情形 | HTTP | 响应 |
|---|---|---|
| 正常解绑 | 200 | `{http:200, device_id, user_id:"<prev>", unbound:true, idempotent:false}` |
| 无 active binding | 200 | `{http:200, device_id, user_id:null, idempotent:true}`（**零写入**） |
| `device_id` 非法 | 400 | `{error:"invalid_device_id"}` |
| 设备不存在 | 404 | `{error:"device_not_found"}` |
| 未注册 | 422 | `{error:"device_not_registered"}` |
| 已退役 | 422 | `{error:"device_revoked"}` |
| 事务失败 | 500 | `{error:"batch_failed", device_id}` |

> **已退役（`REVOKED`）设备的绑定只能由 `revoke` 路径关闭**，普通 `unbind` 一律拒绝（422）。

### 11.4 `POST /api/admin/device/:id/transfer`

**请求**：`{ "to_user_id": "…", "from_user_id": "…"?, "reason": "…"? }` —— `to_user_id` **必填**

- `from_user_id` 是**可选 CAS**：提供且与当前 owner 不符 ⇒ `422 binding_conflict` 且**零写入**。
- **不降级**：无 active binding ⇒ `422 not_bound`（**不会**退化成 bind）。

| 情形 | HTTP | 响应 |
|---|---|---|
| 成功 | 200 | `{http:200, device_id, from, to, transferred:true, idempotent:false}` |
| 入参非法 | 400 | `{error:"invalid_user_id"}` |
| 设备不存在 | 404 | `{error:"device_not_found"}` |
| 未注册 / 已退役 | 422 | `{error:"device_not_registered"}` / `{error:"device_revoked"}` |
| 无 active binding | 422 | `{error:"not_bound", device_id}` |
| **CAS 不符** | 422 | `{error:"binding_conflict", device_id, owner}` |
| 事务失败 | 500 | `{error:"batch_failed", device_id}` |

### 11.5 `GET /api/admin/device/:id/binding`

**响应**：`{http:200, device_id, current_owner, user_id, history}`

- `current_owner`：active binding 行 **或 `null`** —— **owner 的唯一真源**。
- `history`：**含已关闭的历史绑定**（追加式，不删行）。
- **响应不含 `device.state`**，也不含任何凭据字段。

| 情形 | HTTP | 响应 |
|---|---|---|
| 有 / 无 owner | 200 | 上述结构（无 owner ⇒ `current_owner:null`、`user_id:null`、`history:[]`） |
| `device_id` 非法 | 400 | `{error:"invalid_device_id"}` |
| 设备不存在 | 404 | `{error:"device_not_found"}` |

### 11.6 `GET /api/admin/user/:userId/devices`

- 路径 `userId` 需 URL 解码（允许字符含 `@ : . - _`）；query `limit` 可选（默认 200，内部钳位 1..500）。
- **响应**：`{http:200, user_id, devices:[…], count}` —— 只含 **active binding 投影**（一用户多设备，1:N 不设上限）。

| 情形 | HTTP | 响应 |
|---|---|---|
| 正常 | 200 | 上述结构 |
| `userId` 非法 | 400 | `{error:"invalid_user_id"}` |
| 路径形状不符 | 404 | `{error:"not_found"}` |
| 非 GET 方法 | 405 | `{error:"method_not_allowed"}` |

### 11.7 `POST /api/admin/device/:id/revoke`（退役）

**请求**：`{ "reason": "…"? }`（默认 `retire`）

**响应**：`{http:200, device_id, revoked:true, acl_tightened:true, kicked, binding_closed:<bool>, detail}`

- **不变量 INV-4**：退役**必须关闭** active binding —— **同一 D1 事务**内依次：
  关 binding → 写 `unbind` 事件 → 凭据 `REVOKED` → `device.state='REVOKED'` → 写 `retire` 事件。
- 设备**无** active binding 时 `binding_closed:false`，且**不写** `unbind` 事件。
- ⚠️ **已知限制**：ACL 收紧与踢会话是 **EMQX 外部调用，不在 D1 事务内**；事务失败时 ACL / 会话已生效
  （不可逆），但凭据仍为 `REVOKED` ⇒ 设备无法接入（偏向保守）。

### 11.8 与设备协议的关系（重申）

| 维度 | 设备协议（§1–§10） | 控制面 API（本章） |
|---|---|---|
| 传输 | MQTT（8883 / 8084） | HTTPS（Cloudflare Worker） |
| 鉴权 | 一机一账号 + ACL | `ADMIN_API_TOKEN`（Bearer） |
| 方向 | 设备 ↔ 云 | 管理端 → 云 |
| 与设备在线的关系 | 需在线 | **不需要**（归属与连通性解耦） |
| 影响面 | 命令执行 / 遥测上报 | 归属关系 / 凭据台账 |

---

## 12. Homie 5 投影协议（设备侧）

> **性质**：**设备 ↔ 云** 的第二条 topic 命名空间，与 §1–§10 的 `guo_feeder/<device_id>/{down,up,log}` **并存**（不是替代）。
> **权威来源**：`docs/architecture/P2-0-architecture-review-and-design-freeze.md`（**FROZEN**：§8 Topic · §10 `$state` · §11 `$description` ·
> §12 映射 · §13 校验 · §14 `/set` · §16 ACL · §17.2.1 预算）。
> **本节只**摘要**已冻结的实现契约**，**不重新设计协议**；与冻结文档冲突时**以冻结文档为准**。
> **模块说明**：`src/homie/readme.md` · **收口结论**：`docs/architecture/P2-8-Closure-Review.md`。
>
> **★ 验证状态**：本节的软件侧契约**已验证**；**真机 / 真 broker 端到端为 `NOT RUN`（硬件 DEFERRED）** ——
> 见 `docs/architecture/P2-7-Hardware-Deferral-Record.md`。**本文档描述协议，不代表硬件已验证。**

### 12.1 开关与实现边界

| 项 | 值 |
|---|---|
| 编译期开关 | `GF_HOMIE_ENABLED`（**默认 0**；为 0 时该模块全部为空实现 ⇒ 生产零代价） |
| 依赖方向 | `homie_bridge → cloud/mqtt_transport → cloud_manager → esp_mqtt_client`（**单向**） |
| MQTT 客户端 | **单客户端**（仍归 `cloud_manager`）；Homie 只经 Transport Facade 收发 |
| 执行路径 | `/set` **必须**经 `command_manager_execute()`（`source="homie"`），**不得绕过 CommandManager** |
| 投影范围 | **仅 Device Model 属性**；**Workflow 不投影**（§23 #22 / AD-9） |

### 12.2 Topic 结构

**base** = `homie/5/<device_id>/`

| 方向 | Topic | retained | QoS |
|---|---|:---:|:---:|
| pub | `homie/5/<id>/$state` | **是**（规范强制） | 1 |
| pub | `homie/5/<id>/$description` | **是**（规范强制） | 1 |
| pub | `homie/5/<id>/<node>/<prop>` | 见 §12.5 | 1 |
| sub | `homie/5/<id>/+/+/set` | — | 1 |
| LWT | `homie/5/<id>/$state` = `lost` | 是（规范） | 1 |

> **QoS1 是 Guo Feeder 的 transport profile 选择，不是 Homie 规范要求**（AD-3a…3d）。
> `$description` 在 **Registry rebuild 后不重生成、不重发布、不改 contract**，仅打 fingerprint 陈旧 `WARN`（§23 #13）。

### 12.3 `$state` 生命周期

- 6 态取 4：**`init` → `ready`**；`disconnected` / `lost` 由 **broker LWT** 表达（设备**不自行**发 `lost`）。
- **`ready` = 五条件合取**：① MQTT 已连 ② `$description` 已发 ③ 只读初值已发 ④ Capability Registry 已就绪 ⑤ CommandManager 已初始化。
- 断连后重连**幂等重走** `init → ready`。

### 12.4 `$description`

- **仅 boot `init` 阶段生成一次**并 retained 发布；**硬上限 4096 B**，超限则**不发布坏描述并停在 `init`**。
- 结构：`{"homie":"5.0","version":<fnv(desc/action/trigger)>,"nodes":{ <node>: {name,type,properties:{…}} }}`。
- **设备级 `name` 省略**（P2 不引入友好名，D-8 → P3）；**无 `$target`**（D-13）。
- 实测体积：**1,907 B**（16 属性）/ **1,618 B**（13 属性，`weight.enable=false`）—— 均不分片（< `CLOUD_MQTT_BUFFER_SIZE`）。
- **boot-time 可用性过滤**：写侧要求属性的**全部** Action 已注册；无自身 Action 的只读属性按 HomieGate 锚定的 Action 判定。
  典型后果：`weight.enable=false` ⇒ `water/weight` · `water/weight-error` · `water/zero` **同时不出现**（16 → 13 属性）—— **预期行为**。

### 12.5 属性与命令映射（摘要）

> 完整语义定义在 **Device Model**（`src/homie/device_model.h`，`constexpr` 表，**DRAM 静态 0 B**）；此处只列大纲。

| node | property | datatype | 可写 | 命令映射 |
|---|---|:---:|:---:|---|
| `water` | `valve` | `enum("open,close")` | ✅ | `VALVE_OPEN` / `VALVE_CLOSE` |
| `water` | `weight` / `weight-error` | `float` / `boolean` | — | — |
| `water` | `zero` | `boolean` | ✅ | `WEIGHT_ZERO` |
| `climate` | `temperature` / `humidity` / `battery` / `scan-enabled` | `float` / `boolean` | — | — |
| `climate` | `scan` | `enum("start,stop")` | ✅ | `MI_THERMO_START_SCAN` / `MI_THERMO_STOP_SCAN` |
| `system` | `mqtt-connected` / `wifi-rssi` / `wifi-signal` / `time-valid` / `event-queue` / `mqtt-retries` | `boolean` / `integer` / `enum` | — | — |
| `control` | `computer-reset` | `boolean` | ✅ | `COMPUTER_RESET` |

合共 **4 node / 16 property / 6 命令**。**node / property id 一律语义字符串**（`[a-z0-9-]`）；
**禁止使用 `stable_id` 作为 Homie 身份**（其为 runtime_id 字母序 index，会平移）。

### 12.6 `/set`（客户端 → 设备）

- 载荷 = **裸值**（**不是** JSON 信封），topic = `homie/5/<id>/<node>/<prop>/set`。
- **`retain=1` 一律丢弃**（覆盖 场景 A 实时 retained publish 与 场景 B broker 重连重放；**判据只有 `retain`**，禁用启发式替代）。
- 校验顺序：① 属于本设备 namespace 且恰好 3 段 ⇒ ② 属性存在 / `settable` / dtype / enum 成员 / 数值范围 ⇒ ③ **显式映射表**得 `action_id`
  （**客户端永远无法提供或覆盖 `action_id`**）⇒ ④ `command_manager_execute({source:"homie"})`。
- 失败（越权 / 未知 / 只读 / 非法值 / retained）⇒ **不执行、不发布**，WARN 落 LogManager。
- 结果收尾：pending 表（定容 4）+ 30 s 兜底超时，映射为 SUCCESS / FAILED / TIMEOUT / REJECTED / COMPLETED。

### 12.7 ACL namespace（设备级隔离）

| 规则 | permission |
|---|---|
| `homie/5/<id>/#` | **publish** |
| `homie/5/<id>/+/+/set` | **subscribe** |

- `<id>` 段**逐字枚举，禁止通配**（`homie/5/#` / `homie/5/+/…` / `#` 一律**拒绝**） ⇒ 跨设备隔离。
- ACL **只做 namespace 隔离**，**不承担 per-property capability authorization**（AD-11 / §16.4；能力约束由 Device Model 校验负责）。
- 设备账号需**同时**持有本条 Homie 两条 **与** §1–§10 的 P1 三条业务 topic ⇒ **PUT 为全量覆盖，必须一次写全 5 条**。

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
| 日志 | `log_ack`（下行）—— ⚠️ **尚未实现**（`计划:WP-5`，受 `G-4`；见 §8.2.4） | 本文档 §8.2 |

---

*本文档由代码实读重写（基线 `ce9ba37`）。若后续修改 CloudManager 的报文结构、
字段缩写、Topic 或错误码，必须同步更新本文档 —— 见 §0.1 的约定。*

*§11（Control Plane API）于 **2026-10-04** 依 P1-5 实现补入。控制面契约变更须同步更新
`docs/architecture/P1-5-Final-Review.md`；设备协议部分（§1–§10）不受其影响。*

*§8.2（`log` Topic）于 **2026-10-12** 依 P3 `WP4-FW-LOG` 实现同步：载荷形态由「原始 CBOR 批次」改为
「逐条 Cloud Log Envelope JSON」；**未新增 protocol version**，命令协议与 telemetry 协议**未改动**。
依据 `docs/review/P3-WP4-Firmware-Log-Implementation.md`（实施规格）与
`docs/review/P3-WP4-Firmware-Integration-Design-Review.md`（§9 人工裁决）。*

*§4.5（Telemetry 遥测上行）于 **2026-10-12** 依 P3 `WP4-FW-TELEMETRY` 冻结同步补入：**telemetry v1 schema
已冻结**（信封 **5 字段** `{ts,type,value,seq,schema_version}`；**`device_id` 由 MQTT topic 承载、`source` 不发送**；
`value` **三态编码**且**禁止数字编码 bool**；`seq` = **boot lifetime 计数、不持久化**），**固件 publisher 已实现但默认关闭**
（`GF_TELEMETRY_ENABLED=0` · `telemetry.enable=false`）。**未新增 topic、未新增 protocol version、未改命令协议**。
依据 `docs/review/P3-WP4-Firmware-Telemetry-Decisions.md`（冻结裁决）与
`docs/review/P3-WP4-Firmware-Telemetry-Design-Freeze-Review.md`（设计评审）。
⚠️ **分流未落地**（`H-WP4C-03`）⇒ 见 `docs/review/P3-WP4C-EMQX-Rule-Design.md`。*

*§12（Homie 5 投影协议）于 **2026-10-10** 依 P2 收口补入。该节**只摘要**
`docs/architecture/P2-0-architecture-review-and-design-freeze.md` 的冻结契约（§8/§10–§14/§16/§17.2.1），
**不构成第二真源**；与冻结文档冲突时**以冻结文档为准**。Homie 设备协议变更须同步更新
`src/homie/readme.md` 与 `docs/architecture/P2-8-Closure-Review.md`。*
*★ **验证状态**：Homie 软件侧契约已验证；**真机 / 真 broker 端到端为 `NOT RUN`（硬件 DEFERRED）**，
见 `docs/architecture/P2-7-Hardware-Deferral-Record.md`。*
