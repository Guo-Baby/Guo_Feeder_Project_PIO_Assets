# ESP32 Automatic Cat Food Powder Dispenser
# Cloud Protocol + Capability Registry Incremental Implementation Task

Version: 1.0
Date: 2026-08-16

---

# 0. Document Purpose

本文件是交给本地 AI Coding Agent 执行的完整闭环开发任务。

目标不是重构现有工程，而是在现有已经能够稳定运行的 Action / Trigger / Workflow / CommandManager / CloudManager 架构上，以“增量新增”为原则，建立：

1. Capability Registry
2. Action / Trigger / Workflow Stable ID 映射
3. Registry Version
4. Registry Flash 持久化
5. Registry RAM Runtime Cache
6. Registry 查询命令
7. Cloud Protocol 的 CBOR Binary 传输
8. 自动单包 / 分片传输框架
9. Workflow Version 校验基础
10. 为未来 APP 解码、Workflow 上传下载、大文件传输预留协议基础

核心原则：

> 原有业务框架优先保持不动。  
> 不修改 WorkflowActionDescriptor、WorkflowTriggerDescriptor、WorkflowInstance、ActionInstance、CommandMessage 等既有核心结构体。  
> 不改变已有 Action / Trigger / Workflow 的注册方式。  
> 新功能尽量作为独立模块增加，通过少量调用点接入。

GitHub 当前工程为：
`https://github.com/Guo-Baby/Guo_Feeder_Project`

---

# 1. 当前工程事实

当前工程已经存在：

- workflow.cpp / workflow.h
- command_manager.cpp / command_manager.h
- cloud_manager.cpp / cloud_manager.h
- valve.cpp / valve.h
- weight.cpp / weight.h
- test_mqtt.cpp / test_mqtt.h
- main.cpp
- config_manager.cpp / config_manager.h
- system_state.cpp / system_state.h

当前 Workflow Framework 中：

- `WORKFLOW_MAX_PARAM = 8`
- `WORKFLOW_MAX_STEP = 16`
- `WORKFLOW_MAX_COUNT = 16`

现有 Action / Trigger Descriptor 已包含：

- id
- name
- module
- description
- params
- param_count
- handler / reset / start / poll 等执行相关信息

严禁为了 Stable ID 修改上述 Descriptor 结构体。

当前工程已有 Action / Trigger 注册机制，后续 Capability Registry 必须扫描和复用已有 Registry，而不是复制一套 Action 注册系统。

---

# 2. 最终架构

最终数据流：

```text
Existing Action / Trigger / Workflow Registry
                    |
                    v
       CapabilityRegistryManager
                    |
          +---------+---------+
          |         |         |
          v         v         v
       Action    Trigger   Workflow
       Mapping   Mapping   Mapping
          |         |         |
          +---------+---------+
                    |
             RAM Runtime Cache
                    |
                    v
               CommandManager
                    |
                    v
               CloudManager
                    |
                    v
              MQTT / CBOR
                    |
                    v
               Cloud / APP
```

Flash 只负责持久化。

运行期间查询全部优先从 RAM Runtime Cache 获取。

---

# 3. Capability Registry 三张独立 Mapping 表

必须建立三个独立逻辑表：

```text
Action Mapping
Trigger Mapping
Workflow Mapping
```

推荐持久化文件：

```text
/action_registry.bin
/trigger_registry.bin
/workflow_registry.bin
```

文件名可以根据现有工程 LittleFS 规范调整，但必须保持三类独立。

原因：

- Action 变化频率低
- Trigger 变化频率低
- Workflow 变化频率高

三者不能共享一个版本号。

---

# 4. Stable ID 定义

Stable ID 是 Cloud Protocol 使用的短 ID。

Stable ID 不写入原有 Descriptor。

Stable ID 不修改原有 Runtime ID。

Stable ID 只存在于 Capability Registry Mapping 中。

---

## 4.1 Action Stable ID

按照当前有效 Action Registry 的稳定顺序生成：

```text
0
1
2
3
...
```

Stable ID 优先使用最小能够覆盖当前上限的无符号整数类型。

当前 Action 数量上限较小，协议编码时必须尽量压缩。

不要使用：

- SHA
- MD5
- 长字符串 Hash
- 原 Action ID 作为网络对象 ID

Stable ID 的意义是：

```text
Protocol ID
```

不是：

```text
Runtime ID
```

---

## 4.2 Trigger Stable ID

同理：

```text
0
1
2
3
...
```

独立于 Action Stable ID。

Action 0 与 Trigger 0 可以同时存在。

因为它们属于不同 Registry Namespace。

---

## 4.3 Workflow Stable ID

Workflow 当前上限为：

```text
WORKFLOW_MAX_COUNT = 16
```

因此 Stable ID 可以直接使用：

```text
0 ~ 15
```

Workflow Stable ID 对应当前 Workflow Registry 中的稳定位置。

---

# 5. Stable ID 与 Registry 顺序

当前系统已经存在固定的模块初始化 / Registry 建立过程。

Capability Registry 必须在所有 Action / Trigger / Workflow 注册完成以后执行。

禁止在模块尚未完成注册时建立 Mapping。

启动流程：

```text
系统启动
    |
    v
各模块初始化
    |
    v
Action / Trigger / Workflow Registry 完成
    |
    v
CapabilityRegistryManager 初始化
    |
    v
扫描当前 Registry
    |
    v
生成 / 校验 Mapping
    |
    v
建立 RAM Runtime Cache
    |
    v
系统进入正常运行
```

Stable ID 不应简单依赖“编译器随机顺序”。

必须以现有 Workflow Framework 实际 Registry 的稳定顺序为依据。

---

# 6. Registry 校验机制

每一个 Registry 必须拥有：

```text
version
count
checksum
mapping
```

例如：

```text
Action Registry
version = 7
count = 4

0 -> VALVE_OPEN
1 -> VALVE_CLOSE
2 -> VALVE_TOGGLE
3 -> WEIGHT_READ

checksum = CRC32(...)
```

Checksum 只用于判断当前 Registry 与 Flash 中保存的 Mapping 是否一致。

Checksum 不作为 Stable ID。

---

## 6.1 Checksum 输入

必须按照固定顺序，对当前 Registry 中每一个条目的稳定身份信息计算 checksum。

至少包含：

```text
entry.id
```

推荐同时加入 entry 顺序信息。

不要加入：

- 函数指针
- runtime pointer
- instance pointer
- RAM 地址
- 运行时状态

因为这些信息不是稳定信息。

---

## 6.2 校验成功

如果：

```text
current checksum == stored checksum
```

则：

```text
读取 Flash Mapping
更新 RAM Runtime Cache
保持原 version
```

不重新生成。

---

## 6.3 校验失败

如果：

```text
current checksum != stored checksum
```

则：

```text
重新生成 Mapping
version = old version + 1
写入 Flash
更新 RAM Runtime Cache
```

如果 Flash 文件不存在、损坏、版本非法，也必须视为 Mapping 不存在并重新生成。

---

# 7. Registry Version

使用：

```cpp
uint32_t
```

版本号。

规则：

```text
1
2
3
4
...
```

禁止使用时间戳作为 Registry Version。

原因：

Registry Version 表示“协议能力表的逻辑版本”，不是创建时间。

初始版本：

```text
1
```

每次 Mapping 真正发生变化：

```text
version++
```

版本溢出按照 uint32_t 自然回绕处理即可。

---

# 8. Version 的语义

必须明确：

## Action Registry Version

表示：

> 当前设备 Action 能力映射版本。

## Trigger Registry Version

表示：

> 当前设备 Trigger 能力映射版本。

## Workflow Registry Version

表示：

> 当前设备 Workflow 映射版本。

三者互相独立。

---

# 9. RAM Runtime Cache

Capability Registry 初始化完成以后，必须建立 RAM 常驻 Runtime Cache。

目的：

- CloudManager 查询不能每次访问 Flash
- CommandManager 查询不能每次访问 Flash
- Action / Trigger / Workflow Stable ID 翻译不能依赖 Flash
- 减少 Flash 读取
- 提高查询速度

RAM Cache 至少能够支持：

```text
stable_id -> original registry id
```

以及：

```text
original registry id -> stable_id
```

双向查询。

---

# 10. Registry 查询接口

Capability Registry 必须提供统一查询接口。

逻辑上至少支持：

```text
get_action_count()
get_trigger_count()
get_workflow_count()

get_action_version()
get_trigger_version()
get_workflow_version()

get_action_by_stable_id()
get_trigger_by_stable_id()
get_workflow_by_stable_id()

get_action_stable_id(original_id)
get_trigger_stable_id(original_id)
get_workflow_stable_id(original_id)

build_action_registry_payload()
build_trigger_registry_payload()
build_workflow_registry_payload()
```

具体函数命名由 Coding Agent 根据现有工程风格决定，但功能必须完整。

---

# 11. Capability Registry 不负责 MQTT

Capability Registry 只负责：

- Registry 扫描
- Mapping 建立
- Mapping 校验
- Flash 持久化
- RAM Cache
- Registry 数据编码

Capability Registry 不允许：

- 直接调用 MQTT
- 直接操作 CloudManager
- 直接执行 Action
- 直接执行 Workflow
- 解析 CommandMessage
- 处理 MQTT callback

这样保持层次清晰。

---

# 12. CommandManager 查询职责

CommandManager 负责：

```text
收到 query capability 命令
        |
        v
识别查询对象
        |
        v
调用 CapabilityRegistryManager
        |
        v
得到 Registry 数据
        |
        v
生成 CommandResult
        |
        v
交给 CloudManager
```

CommandManager 不负责：

- 生成 Stable ID
- 管理 Flash Mapping
- 计算 Registry checksum
- 保存 Registry

这些属于 Capability Registry。

---

# 13. 新增 Command Tree

Cloud Protocol 顶层 Command 分类最终采用：

```text
EXECUTE
QUERY
SYSTEM
CONFIG
TRANSFER
```

这是协议逻辑分类。

---

## 13.1 EXECUTE

包括：

```text
execute_action
execute_workflow
```

---

## 13.2 QUERY

至少包括：

```text
query_state
query_actions
query_triggers
query_workflows
query_action_registry
query_trigger_registry
query_workflow_registry
```

如果最终发现 `query_actions / query_triggers / query_workflows` 已经可以直接承载 Registry 查询，则允许复用已有 command。

不要重复创建没有必要的新 Command。

---

## 13.3 SYSTEM

预留：

```text
system_restart
system_reconnect_wifi
system_sync_time
system_get_time
```

以后增加系统级命令时继续增加枚举。

---

## 13.4 CONFIG

至少预留：

```text
query_config
update_config
```

Config 仍由 ConfigManager 负责。

CloudManager / CommandManager 不得直接操作 config.json。

---

## 13.5 TRANSFER

预留：

```text
transfer_begin
transfer_chunk
transfer_end
transfer_ack
transfer_abort
```

用于：

- workflow.json
- config.json
- registry.bin
- log
- 后续其他文件

---

# 14. Command Message 基础字段

保持现有六字段，不进行结构体重构：

```text
command
object
cmd_id
payload
source
timestamp
```

协议设计原则：

> 内部 CommandMessage 继续保持现状。

不要为了 CBOR 或 Stable ID 修改 CommandMessage 结构体。

CloudManager 在编码 / 解码时做协议转换。

---

# 15. Object 新定义

Object 不再使用长字符串作为主要网络标识。

对于能力类对象：

```text
object_type
stable_id
```

逻辑上表示：

```text
ACTION + 3
TRIGGER + 2
WORKFLOW + 5
```

最终 Binary / CBOR 编码时使用紧凑整数。

原始 Action ID 只作为 Registry Mapping 内容传输，不作为每一次执行命令的主要对象 ID。

---

# 16. Command 编码

Command 类型使用固定枚举。

例如：

```text
EXECUTE_ACTION
EXECUTE_WORKFLOW
QUERY_STATE
QUERY_ACTIONS
QUERY_TRIGGERS
QUERY_WORKFLOWS
SYSTEM_RESTART
SYSTEM_RECONNECT_WIFI
SYSTEM_SYNC_TIME
SYSTEM_GET_TIME
QUERY_CONFIG
UPDATE_CONFIG
TRANSFER_BEGIN
TRANSFER_CHUNK
TRANSFER_END
TRANSFER_ACK
TRANSFER_ABORT
```

实际数值由协议实现统一定义。

一旦发布，不允许随意重新编号。

新增命令只能使用新的枚举值。

---

# 17. Source 编码

Source 同样使用短枚举：

```text
CLOUD
DEVICE
UI
LOCAL
APP
```

具体数值固定。

Source 主要用于：

- 日志
- 消息追踪
- 调试
- 请求来源判断

---

# 18. Timestamp

协议 timestamp 使用：

```text
uint32_t Unix timestamp
```

协议不再同时传：

```text
readable time string
```

APP / Cloud UI 负责把 timestamp 转换成人类可读时间。

---

# 19. Payload

Payload 继续由业务模块定义。

CommandManager 不解析具体 Action Payload。

例如：

```text
Action:
Valve Open

payload:
{
    duration: 10
}
```

未来可以在协议层采用 CBOR 紧凑 key。

但不要强制修改所有 Action 当前 Payload 解析代码。

优先保持兼容。

---

# 20. CBOR

CloudManager 负责：

```text
JSON String
   |
   v
ArduinoJson
   |
   v
CBOR Binary
   |
   v
MQTT publish
```

业务层仍可以调用：

```text
cloud_send_up(json)
cloud_send_set(json)
```

业务代码不需要知道 CBOR。

ArduinoJson CBOR 功能使用：

```text
ARDUINOJSON_USE_CBOR=1
```

不要使用错误的：

```text
ARDUINOJSON_ENABLE_CBOR
```

---

# 21. JSON / CBOR 兼容策略

开发阶段优先保证：

```text
业务层 JSON
CloudManager 内部 CBOR
MQTT Binary
```

CloudManager 必须能够根据当前协议模式进行编码。

不要修改：

- test_mqtt.cpp
- WorkflowManager
- Action
- Trigger

除非编译依赖确实无法避免。

---

# 22. 自动消息分片

CloudManager 必须建立统一的自动分片层。

调用方只需要：

```text
cloud_send_up(...)
```

无需知道消息是否超过 MQTT 单包限制。

逻辑：

```text
消息生成
   |
   v
计算编码后长度
   |
   +---- <= 单包安全上限 ----> 普通发送
   |
   +---- > 单包安全上限 -----> Transfer 分片
```

不要根据 JSON 原文长度判断。

必须根据：

```text
最终 MQTT Binary Payload 长度
```

判断。

---

# 23. Transfer Protocol

大文件 / 大消息统一采用：

```text
TRANSFER_BEGIN
TRANSFER_CHUNK
TRANSFER_END
```

必要时：

```text
TRANSFER_ACK
TRANSFER_ABORT
```

---

## 23.1 Begin

包含：

```text
transfer_id
file_type / object_type
total_size
total_chunks
version
checksum
```

---

## 23.2 Chunk

包含：

```text
transfer_id
chunk_index
offset
chunk_length
data
chunk_checksum
```

---

## 23.3 End

包含：

```text
transfer_id
total_chunks
total_size
final_checksum
```

---

# 24. Chunk 必须可重复接收

MQTT QoS1 是 At Least Once。

因此 Chunk 协议必须允许重复。

收到：

```text
transfer_id = X
chunk_index = 10
```

如果已经保存：

```text
直接确认
不要重复写入
```

不能假设 QoS1 = Exactly Once。

---

# 25. QoS

现有 PubSubClient 不满足后续可靠文件传输需求。

后续需要评估并替换为支持 QoS1 的 MQTT Client。

要求：

- MQTT QoS1
- publish QoS1
- subscribe QoS1
- reconnect 后状态恢复
- 与现有 MQTT topic 兼容
- API 尽可能接近现有 PubSubClient，降低 CloudManager 改动量

替换 MQTT 库属于独立任务。

不要在 Capability Registry 第一阶段同时替换 MQTT 库。

---

# 26. ACK

协议 ACK 与 MQTT QoS1 是两个不同层次：

```text
MQTT QoS1
=
MQTT Broker 已收到 / 协议层确认

Application ACK
=
设备业务层已收到并接受该 transfer / command
```

普通 Command：

```text
收到命令
    |
    v
立即 Application ACK
    |
    v
异步执行
    |
    v
action_result / workflow_result
```

Transfer：

```text
BEGIN
ACK

CHUNK
ACK

END
ACK
```

但不要强制所有普通状态上报都增加业务 ACK。

只有需要可靠交付的命令 / Transfer 使用 Application ACK。

---

# 27. Workflow Version

Workflow 是用户可修改对象。

因此 Workflow 必须拥有独立：

```text
workflow_version : uint32_t
```

Workflow Version 必须持久化。

每次 Workflow 内容真正发生变化：

```text
workflow_version++
```

Workflow JSON 保存机制继续复用现有 workflow.cpp 已有保存逻辑。

不要重写现有 Workflow JSON 存储系统。

---

# 28. Workflow 执行版本校验

执行 Workflow 时，Cloud Command 必须能够携带：

```text
workflow_stable_id
workflow_version
```

设备执行前：

```text
cloud_version == local_workflow_version
```

才允许执行。

不一致：

```text
拒绝执行
返回 version mismatch
```

响应必须告诉 Cloud / APP：

```text
local_version
requested_version
```

后续再通过 Transfer / Query 完成同步。

---

# 29. Workflow 更新

用户更新 Workflow 时：

```text
APP
 ↓
Cloud
 ↓
TRANSFER
 ↓
ESP32
 ↓
临时接收
 ↓
校验完整文件
 ↓
调用现有 Workflow 保存机制
 ↓
workflow_version++
 ↓
重新生成 Workflow Registry Mapping
 ↓
更新 RAM
 ↓
保存 Registry
```

必须保证：

> 文件完整校验成功以后才替换正式 workflow.json。

禁止半文件覆盖正式 Workflow。

---

# 30. Registry 查询数据

APP 需要能够通过查询获得完整能力描述。

至少包括：

```text
registry_type
version
count
checksum
entries[]
```

Action Entry 至少包含：

```text
stable_id
original_id
name
module
description
param_count
params[]
```

Parameter 至少包含：

```text
name
type
unit
description
```

Trigger 同理。

Workflow Entry 至少包含：

```text
stable_id
workflow_id
name
description
```

具体字段应尽可能复用现有 Descriptor 数据，不复制业务逻辑。

---

# 31. APP 解码原则

ESP32 不负责针对某一个 APP 写特殊协议。

协议只定义：

```text
Command Enum
Object Enum / Stable ID
CBOR Schema
Transfer Schema
Registry Schema
```

APP 实现同样的 Decoder。

APP 可以：

```text
query_actions
     ↓
receive Registry
     ↓
decode CBOR
     ↓
cache
     ↓
UI
```

因此换手机不需要重新编译 ESP32。

增加 Action 不需要重新编译 APP。

---

# 32. Registry 缓存原则

APP 可以缓存：

```text
Action Registry
Trigger Registry
Workflow Registry
```

并保存：

```text
device_id
registry_type
version
checksum
```

设备版本一致：

```text
不重新下载
```

设备版本变化：

```text
重新 query
```

---

# 33. 文件传输与 Registry

Registry BIN 未来允许通过 Transfer Protocol 上传到 APP。

但是 APP 正常情况下优先直接请求：

```text
query_actions
query_triggers
query_workflows
```

只有大于单包限制时 CloudManager 自动转成 Transfer。

因此业务层无需知道：

```text
单包
```

还是：

```text
分片
```

---

# 34. System State

当前 System State 仍采用已有 SystemStateKey / state_map 机制。

本次任务：

> 不重构 System State。

未来如果需要自动发现 State，可以另建：

```text
State Registry
```

但不作为本次 Capability Registry 的强制范围。

---

# 35. Config

Config 继续由 ConfigManager 管理。

本次 Cloud Protocol 只负责：

```text
query_config
update_config
```

CommandManager 路由：

```text
CommandManager
 ↓
ConfigManager
```

不得让 CloudManager 直接写 config.json。

---

# 36. 日志

本次任务不实现完整 SystemLog。

但 Cloud Protocol 必须预留：

```text
log_upload
```

以及 Transfer 支持。

未来 SystemLog 模块独立实现：

```text
SystemLog
 ↓
Flash queue
 ↓
批量写入
 ↓
Cloud Transfer
```

本次不修改所有业务模块加入 Log 调用。

---

# 37. 严格禁止的修改

除非编译不可避免，否则禁止：

1. 修改 WorkflowActionDescriptor
2. 修改 WorkflowTriggerDescriptor
3. 修改 WorkflowActionInstance
4. 修改 WorkflowTriggerInstance
5. 修改 Workflow
6. 修改 CommandMessage
7. 修改 TempAction 生命周期
8. 修改 Workflow 执行状态机
9. 修改 Action Handler
10. 修改 Trigger Handler
11. 重写 Workflow JSON
12. 重写 Config Manager
13. 修改 test_mqtt.cpp
14. 为 Stable ID 新增字段到 Descriptor

Stable ID 必须通过新增 Capability Registry 实现。

---

# 38. 任务拆分规则

这是本任务最重要的工程执行规则：

> 每一个任务尽量只修改一个文件。

完成一个文件：

```text
编译
```

确认成功以后：

```text
再修改下一个文件
```

禁止一次性修改十几个文件再统一编译。

---

# 39. TASK 01 - 新建 capability_registry.h

只创建：

```text
src/capability_registry.h
```

内容：

- Registry 类型定义
- Stable ID 类型
- Version 类型
- Mapping Entry 定义
- Registry Query API
- 初始化 API
- RAM Cache API
- Payload Build API

此阶段：

- 不修改旧文件
- 不接入 main
- 不接入 CommandManager
- 不接入 CloudManager

目标：

```text
编译通过
```

---

# 40. TASK 02 - 新建 capability_registry.cpp

只创建：

```text
src/capability_registry.cpp
```

实现：

- 初始化
- Registry 扫描
- Stable ID 分配
- checksum
- version
- Flash Mapping 读取
- Flash Mapping 写入
- RAM Cache
- 双向查询
- Registry payload 构造

此阶段：

- 不修改 Workflow
- 不修改 CommandManager
- 不修改 CloudManager
- 不修改 main

如果必须依赖 Workflow API，只使用已有公开接口。

目标：

```text
编译通过
```

---

# 41. TASK 03 - Capability Registry 单独初始化验证

只修改：

```text
src/main.cpp
```

增加：

```text
CapabilityRegistry 初始化调用
```

必须保证：

```text
所有 Action / Trigger / Workflow 注册完成
```

以后再：

```text
CapabilityRegistry begin
```

增加最少量 Serial Debug：

```text
Action Registry
Trigger Registry
Workflow Registry
version
count
checksum
```

目标：

首次启动生成 BIN。

第二次启动：

```text
checksum相同
```

不重新生成。

修改一个 Action ID 后重新编译：

```text
checksum不同
version++
重新生成Mapping
```

---

# 42. TASK 04 - CommandManager 增加 Registry Query 路由

修改：

```text
src/command_manager.cpp
```

必要时才修改：

```text
src/command_manager.h
```

但是如果可以只修改 cpp，则优先只修改 cpp。

增加：

```text
query_action_registry
query_trigger_registry
query_workflow_registry
```

或者复用现有：

```text
query_actions
query_triggers
query_workflows
```

优先复用已有命令，避免重复协议。

CommandManager 只负责路由：

```text
Command
 ↓
CapabilityRegistry
 ↓
CommandResult
```

不得在 CommandManager 中自己扫描 Action Registry。

---

# 43. TASK 05 - CloudManager 增加 CBOR 编码层

只修改：

```text
src/cloud_manager.cpp
```

目标：

保持原有业务调用：

```text
cloud_send_up(json)
cloud_send_set(json)
```

不变。

CloudManager 内部：

```text
JSON
 ↓
ArduinoJson
 ↓
CBOR
 ↓
MQTT binary
```

必须保留调试能力。

开发阶段可以通过配置开关选择：

```text
JSON
CBOR
```

便于测试。

目标：

现有 Action / Workflow 流程不受影响。

---

# 44. TASK 06 - CloudManager 增加自动分片

继续只修改：

```text
src/cloud_manager.cpp
```

目标：

统一处理：

```text
message <= limit
```

直接发送。

```text
message > limit
```

自动：

```text
BEGIN
CHUNK...
END
```

业务模块不感知。

Transfer ID 使用：

```text
cmd_id
```

或内部生成唯一 transfer_id。

每个 Chunk 必须具备：

```text
transfer_id
chunk_index
offset
length
data
checksum
```

---

# 45. TASK 07 - MQTT Client QoS1 替换

这是独立任务。

先确认目标 MQTT 库：

- 支持 ESP32-S3
- 支持 MQTT QoS1
- 支持 binary payload
- 支持 subscribe QoS1
- 支持 publish QoS1
- API 稳定
- 内存占用可接受
- 尽可能减少 CloudManager 改动

不要和 Capability Registry 同时修改。

替换后先只验证：

```text
普通 MQTT command
QoS1 publish
QoS1 subscribe
reconnect
```

确认稳定后再继续 Transfer ACK。

---

# 46. TASK 08 - Transfer ACK

在 QoS1 稳定以后实现：

```text
TRANSFER_ACK
```

要求：

- BEGIN ACK
- CHUNK ACK
- END ACK
- 重复 Chunk 可重复 ACK
- 不重复写入
- 支持超时
- 支持重试
- 支持 abort

注意：

MQTT QoS1 与 Application ACK 必须分开理解。

---

# 47. TASK 09 - Workflow Version

优先只修改：

```text
src/workflow.cpp
```

必要时再修改：

```text
src/workflow.h
```

要求：

- 保留现有 Workflow JSON
- 增加 Workflow Version 持久化
- 内容变化 version++
- 读取 JSON 时恢复 version
- 保存成功以后再更新正式 version

如果当前 Workflow JSON 格式已有明确结构，优先增加一个顶层 version 字段。

---

# 48. TASK 10 - Workflow Execute Version Validation

修改：

```text
src/command_manager.cpp
```

在 execute_workflow 路由中：

```text
读取请求 workflow_version
        |
        v
与本地 version 比较
        |
        +---- 相同 ----> 正常执行
        |
        +---- 不同 ----> 拒绝执行
```

返回：

```text
version_mismatch
local_version
requested_version
```

不得修改 Workflow 执行引擎本身。

---

# 49. TASK 11 - Workflow Update Transfer

在 Transfer 协议完成以后实现：

```text
workflow upload
```

流程：

```text
BEGIN
 ↓
CHUNK
 ↓
END
 ↓
完整 checksum 校验
 ↓
临时文件
 ↓
调用现有 Workflow 保存机制
 ↓
成功后替换正式 workflow.json
 ↓
workflow_version++
 ↓
重新生成 Workflow Registry
```

失败：

```text
不得破坏当前有效 workflow.json
```

---

# 50. TASK 12 - Registry Query Full Payload

完善：

```text
query_actions
query_triggers
query_workflows
```

返回完整 Descriptor 信息：

Action：

```text
stable_id
id
name
module
description
params
```

Trigger：

同理。

Workflow：

```text
stable_id
id
name
description
version
```

CloudManager 自动决定：

```text
single packet
```

或者：

```text
transfer
```

---

# 51. TASK 13 - Protocol Compatibility Test

必须测试：

## Test A

Action：

```text
1
2
3
...
```

连续执行。

确认 Stable ID 与原 Action ID Mapping 正确。

## Test B

重启设备。

确认：

```text
version不变
checksum不变
stable mapping不变
```

## Test C

新增 Action。

确认：

```text
checksum变化
version++
```

## Test D

删除 Action。

确认：

```text
checksum变化
version++
```

## Test E

修改 Workflow。

确认：

```text
workflow_version++
workflow_registry_version++
```

## Test F

Query Registry。

确认 APP / 测试工具可以正确 Decode。

## Test G

构造超过 MQTT 单包限制的数据。

确认自动进入：

```text
BEGIN
CHUNK
END
```

## Test H

模拟丢失一个 Chunk。

确认：

```text
不能错误完成
```

重传后可以恢复。

## Test I

重复发送一个 Chunk。

确认：

```text
不会重复破坏文件
```

---

# 52. Protocol Example

逻辑消息：

```text
execute_action
ACTION
stable_id = 3
cmd_id = 10086
payload = {...}
source = APP
timestamp = 178...
```

CBOR / Binary 层应该尽量压缩成：

```text
command
object_type
stable_id
cmd_id
payload
source
timestamp
```

不要在最终 Binary Payload 中重复发送：

```text
"execute_action"
"VALVE_OPEN"
"cloud"
"timestamp"
```

长字符串只在：

```text
Registry Description
```

等确实需要人类 / APP 显示的地方出现。

---

# 53. Registry Example

Action Registry：

```text
type = ACTION
version = 7
count = 4

0:
  id = "VALVE_OPEN"
  name = "打开阀门"
  module = "valve"
  description = "打开供水阀门"
  params:
    duration
    ...

1:
  id = "VALVE_CLOSE"
  name = "关闭阀门"

2:
  id = "VALVE_TOGGLE"
  name = "切换阀门"

3:
  id = "MQTT_TEST"
  name = "MQTT测试"
```

APP 可以缓存整张表。

执行时只需要：

```text
ACTION + 0
```

即可表示：

```text
VALVE_OPEN
```

---

# 54. 版本一致性原则

必须明确区分：

```text
Registry Version
```

和：

```text
Workflow Version
```

以及：

```text
Firmware Version
```

三者不是一个东西。

Firmware 更新可能：

```text
Action Registry unchanged
```

也可能：

```text
Action Registry changed
```

只有 Registry 内容变化时 Registry Version 才变化。

---

# 55. 安全原则

收到：

```text
stable_id
```

以后，Capability Registry 必须能够判断：

```text
stable_id 是否存在
```

不存在：

```text
返回 invalid_object
```

禁止：

```text
stable_id
 ↓
直接当作 Runtime Array Index
```

必须经过 Registry Mapping。

这样避免：

```text
非法 ID
越界
错误 Action
```

---

# 56. 不允许依赖“云端永远正确”

即使：

```text
APP registry_version
==
device registry_version
```

设备仍然必须检查：

```text
stable_id 是否有效
```

因为网络数据不能被默认视为可信。

这是低成本的安全检查。

---

# 57. 性能原则

Capability Registry：

- 初始化时允许读取 Flash
- 初始化后禁止频繁读 Flash
- 查询从 RAM
- Stable ID 查询从 RAM
- Mapping 序列化从 RAM
- Flash 只在 Mapping 变化时写

不得每条 MQTT 消息读取 Registry BIN。

---

# 58. 内存原则

ESP32-S3 当前拥有：

```text
8 MB PSRAM
```

Registry Runtime Cache 可以优先放在 PSRAM，但如果实际数据很小且 SRAM 足够，也允许放普通 RAM。

原则：

> 不为了“必须使用 PSRAM”而强制浪费 PSRAM。

如果使用 PSRAM，必须保证生命周期为：

```text
begin()
 ↓
allocate
 ↓
runtime常驻
```

不得每次 query 动态分配大块 String。

---

# 59. String / Memory Fragmentation

CloudManager 当前存在 String 使用。

本次改造不要大面积重构 String。

但新增 Registry / Transfer 代码：

- 避免无意义 String 拼接
- 优先 reserve
- 避免循环中不断创建临时 String
- 大 Payload 尽量使用固定 / 可控 Buffer
- Transfer Chunk 不允许造成持续内存增长

---

# 60. 最终完整系统

完成以后：

```text
                         APP
                          |
                          | MQTT
                          v
                    Cloud / Broker
                          |
                          v
                    CloudManager
                          |
              +-----------+-----------+
              |                       |
              v                       v
          Command Route          Transfer
              |                       |
              v                       v
       CommandManager             File/Data
              |
      +-------+--------+
      |       |        |
      v       v        v
   Execute  Query    System
      |
      v
WorkflowManager

CapabilityRegistryManager
      |
      +---- Action Mapping
      +---- Trigger Mapping
      +---- Workflow Mapping
      |
      +---- RAM Cache
      +---- Flash Persistence
```

---

# 61. 完成标准

本任务全部完成以后必须满足：

1. 原 Action / Trigger / Workflow 可以继续正常执行。
2. 原 CommandMessage 结构体不需要重构。
3. 原 Descriptor 不增加 Stable ID 字段。
4. Action 可以获得稳定短 ID。
5. Trigger 可以获得稳定短 ID。
6. Workflow 可以获得稳定短 ID。
7. 三类 Registry 有独立 Version。
8. 三类 Registry 有独立 checksum。
9. Registry Mapping 持久化到 Flash。
10. Registry Mapping 启动后进入 RAM。
11. 正常运行期间不依赖 Flash 查询。
12. Query 可以获取完整能力描述。
13. APP 可以自行 Decode。
14. CloudManager 可以自动 JSON -> CBOR -> MQTT Binary。
15. 大消息自动分片。
16. Transfer 支持 checksum。
17. Transfer 支持重复 Chunk。
18. MQTT 后续支持 QoS1。
19. Application ACK 与 QoS1 分层。
20. Workflow 有独立 Version。
21. Workflow Version 不一致时禁止执行。
22. Workflow 更新不会破坏现有有效文件。
23. 新增 Action 不需要修改 APP 的 Action 枚举。
24. 新增 Trigger 不需要修改 APP 的 Trigger 枚举。
25. 新增 Workflow 不需要重新编译 APP。
26. 新手机第一次连接可以通过 Query 获取完整能力表。
27. 整体架构保持原有分层，不发生大规模重构。

---

# 62. 最终开发纪律

AI Coding Agent 必须遵守：

> 先读现有代码，再决定接入点。

> 不允许因为“更漂亮”而重构旧代码。

> 不允许修改已经稳定工作的 Workflow / Action / Trigger 执行逻辑。

> 不允许把 Capability Registry 逻辑塞进 CloudManager。

> 不允许让 CommandManager 直接读取 Flash。

> 不允许让 CloudManager 直接执行 Action。

> 不允许让 APP 依赖 ESP32 Runtime ID。

> 不允许把 Stable ID 写入原有 Descriptor。

> 不允许把 MQTT QoS1 与 Application ACK 混为一谈。

> 不允许把 Registry Version 与 Firmware Version 混为一谈。

> 每个开发任务完成后必须先编译验证，再进入下一任务。

---

# 63. 本次任务的核心思想

最终形成：

```text
Existing Framework
        +
Capability Registry
        +
Stable ID
        +
Version
        +
Checksum
        +
RAM Cache
        +
Flash Persistence
        +
CBOR
        +
Automatic Transfer
        +
QoS1
        +
Application ACK
```

而不是：

```text
重写整个 Framework
```

最终目标：

> **设备能力动态发现 + 稳定短 ID + 版本一致性 + 高效二进制协议 + 自动大消息分片 + 可靠传输，同时保持现有业务代码基本不动。**
