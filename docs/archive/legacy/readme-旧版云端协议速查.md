# 【历史副本】readme 旧版「云端通信协议（MQTT JSON）」章节

> ⚠️ **本文件是 2026-10-02 从 `readme.md` 移出的旧版副本，仅供追溯，请勿据此实现。**
>
> 它与权威文档 `docs/interfaces/cloud_protocol.md`（V2.0，按代码实读重写）**内容重复且已过时**
> （例：其中引用的 `cloud_manager.cpp` 行号已漂移，实测为 `:1154` 而非 `:1048`）。
>
> **权威文档**：
> - `docs/interfaces/cloud_protocol.md` —— 协议规范唯一权威
> - `docs/interfaces/workflow_cloud_interface.md` —— Workflow 云端契约
> - `readme.md` §2.5.1 / §4.12 —— 架构层面速查

---

# 云端通信协议（MQTT JSON）

## 1. 通信模型

设备与云端通过 MQTT 通信。

通信采用统一 JSON 消息格式：

- 云端 → 设备：发送控制命令
- 设备 → 云端：返回 ACK、执行结果、状态、数据

所有消息均使用同一个 JSON 外壳。

### 1.1 Topic

| Topic | 方向 | 状态 | 承载 |
|---|---|---|---|
| `guo_feeder/down` | Cloud → Device | ✅ 已实现 | 命令 + 协议级消息（含未来的 `log_ack`） |
| `guo_feeder/up` | Device → Cloud | ✅ 已实现 | ACK / Result / Registry / 上线通知 |
| `guo_feeder/log` | Device → Cloud | 🔒 **预留** | **仅日志批次**（Log 模块，尚未实现） |

> Log 独立成 Topic 的原因与接入规范见 §2.5.1 与 `docs/interfaces/cloud_protocol.md` §8。

### 1.2 两种报文格式并存

设备**同时支持**两种下行格式，由是否存在 `c` 字段判定：

```c
bool compact = !doc["c"].isNull();    // cloud_manager.cpp:1154
```

- **旧格式**（长字段）：`cmd` / `ob` 原样透传，无需版本与 Stable ID，**调试首选**。
- **新格式**（紧凑字段）：经 `cloud_translate_command()` 翻译，`v` / `k` 校验。

> ⚠️ **本章为概述。权威规范见 `docs/interfaces/cloud_protocol.md`（V2.0，按代码实读重写）。**

---

# 2. 统一 JSON 格式（核心协议）

## 2.1 旧格式（长字段，调试首选）

所有 MQTT 消息必须符合以下结构：

```json
{
    "cmd":"execute_action",
    "ob":"VALVE_OPEN",
    "id":"1785514667334",
    "pl":{},
    "src":"cloud",
    "ts":1785514667
}
```

### 字段说明

| 字段 | 含义 | 类型 | 说明 |
|---|---|---|---|
| `cmd` | command | string | 消息类型，决定消息行为；**原样透传给 CommandManager** |
| `ob` | object | string | 操作对象或目标，由各模块定义其动作函数名 |
| `id` | cmd_id | string | 消息关联 ID，用于匹配请求和响应；**每条必须唯一**，重复会被去重丢弃 |
| `pl` | payload | object | 自定义数据区域 |
| `src` | source | string | 消息来源 |
| `ts` | timestamp | number | Unix 时间戳 |

## 2.2 新格式（紧凑字段，正式）

```json
{
    "c":"action",
    "i":"1785514667334",
    "v":3,
    "k":0,
    "p":{}
}
```

| 字段 | 含义 | 类型 | 说明 |
|---|---|---|---|
| `c` | command | string | `action` / `workflow` / `registry` / `system` / `query` / 其他透传命令名 |
| `i` | id | string | 命令唯一 ID |
| `v` | version | number | Registry 版本号（`c=action` / `workflow` 时必填且须匹配） |
| `k` | kind | number | Stable ID；`c=registry` 时为 RegistryType（0=Action / 1=Trigger / 2=Workflow） |
| `p` | payload | object | 载荷；对象取 `p.o` 或 `p.object` |
| `t` | timestamp | number | Unix 时间戳 |

## 2.3 上行紧凑键（设备 → 云端）

设备发出前经 `cloud_compress_uplink()` 缩写字段名，**云端必须解缩写**：

| 缩写 | 全称 | 说明 |
|---|---|---|
| `s` | status | `CloudStatus` 枚举 |
| `c` | cmd | 命令名 |
| `i` | id | 命令 ID |
| `o` | object | 对象（源优先级 `object` > `action` > `workflow` > `key` > `ob`） |
| `p` | payload | 载荷 |
| `t` | timestamp | **时间无效时该字段被省略** |
| `e` | error | 错误码 |
| `m` | message | 可读文本 |
| `v` | version | 版本号 |
| `k` | stable_id | Stable ID |

---

# 3. 字段设计原则

## command

表示消息意图。


Command Manager 根据 command 进行解析和路由。


---

## object


表示操作目标。


例如：

```
VALVE_OPEN

FEED_WORKFLOW

SYSTEM

WEIGHT
```


---

## id

用于关联一次完整通信过程。


例如：

云端发送：

```
1785514667334
```


设备返回：

```
1785514667334
```


表示属于同一次请求。


一个 id 可以对应多个响应消息。


例如：

```
1785514667334
 |
 +---- ack
 |
 +---- action_result
 |
 +---- state_report
```



---

## payload


自定义数据区域。


Command Manager 不解析具体业务内容。


payload 由目标模块自行定义。


例如 Action 参数：


```json
{
    "id":"cmd_001",

    "command":"execute_action",

    "object":"MOTOR_MOVE",

    "payload":
    {
        "speed":100,
        "position":500
    },

    "timestamp":1785514667,

    "source":"cloud"
}
```


Motor 模块自行解析 payload。


---

# 4. Command 枚举


## 4.1 云端 → 设备


请求类型：

```text
execute_action

execute_workflow


query_state

query_actions

query_triggers

query_workflows


system_restart

system_sync_time

system_get_time


query_config

update_config
```


---

## 4.2 设备 → 云端


响应类型：

```text
ack


action_result

workflow_result


state_report


config_upload

log_upload          ← 注：走独立 log Topic（预留，见 §2.5.1）


error


device_online

device_offline
```

| 响应 | Topic | 说明 |
|---|---|---|
| `ack` / `action_result` / `workflow_result` / `state_report` / `config_upload` / `error` / `device_online` | **`up`** | 业务上行，现有语义不变 |
| `log_upload` | **`log`（预留）** | 结构化日志批次。**不混入 `up`** —— 它是可抑制的批量流，且有离线补发需求 |


---

# 5. ACK协议


设备收到命令后**立即**返回 ACK —— 在命令执行**之前**发出。


实际报文（`cloud_send_ack()`，`cloud_manager.cpp:782`）：

```json
{
    "c":"ack",
    "i":"cmd_001",
    "o":"VALVE_OPEN",
    "p":
    {
        "result":"received"
    },
    "t":1785514668
}
```

经 `cloud_compress_uplink()` 缩写后字段名不变（`c` / `i` / `o` / `p` / `t` 已是缩写）。

> 旧格式（长字段）写法等价：
> `{"cmd":"ack","id":"cmd_001","ob":"VALVE_OPEN","pl":{"result":"received"},"ts":...}`
> —— 但设备**发出**的是上面那一份（紧凑格式）。

说明：

ACK 仅表示：

```
设备已经收到消息
```


不代表执行完成。

字段省略规则：`o` 在 object 为空时省略；`t` 在 `time_get()` 返回 0（时间无效）时省略。

发送顺序（`cloud_process_rx_message`）：

```
④ change_msg_limit（协议级）→ ⑤ 命令翻译（失败则协议错误）
→ ⑥ cloud_send_ack()  ← 这里
→ ⑦ command_manager_execute()  ← 真正的执行
```


---

# 6. Action执行协议


## 请求


云端：

```json
{
    "id":"cmd_001",

    "command":"execute_action",

    "object":"VALVE_OPEN",

    "payload":
    {
    },

    "timestamp":1785514667,

    "source":"cloud"
}
```


---

## 执行完成


成功：

```json
{
    "id":"cmd_001",

    "command":"action_result",

    "object":"VALVE_OPEN",

    "payload":
    {
        "state":"success"
    },

    "timestamp":1785514670,

    "source":"device"
}
```


失败：

```json
{
    "id":"cmd_001",

    "command":"action_result",

    "object":"VALVE_OPEN",

    "payload":
    {
        "state":"failed",
        "error":"timeout"
    },

    "timestamp":1785514670,

    "source":"device"
}
```


---

# 7. Workflow执行协议


请求：

```json
{
    "id":"cmd_002",

    "command":"execute_workflow",

    "object":"FEED_CAT",

    "payload":
    {
    },

    "timestamp":1785514700,

    "source":"cloud"
}
```


结果：

```json
{
    "id":"cmd_002",

    "command":"workflow_result",

    "object":"FEED_CAT",

    "payload":
    {
        "state":"success"
    },

    "timestamp":1785514800,

    "source":"device"
}
```


---

# 8. 查询协议


所有查询统一使用：

```
query_xxx
```


例如：

查询系统状态：

```json
{
    "id":"cmd_003",

    "command":"query_state",

    "object":"system",

    "payload":
    {},

    "timestamp":1785514900,

    "source":"cloud"
}
```


返回：

```json
{
    "id":"cmd_003",

    "command":"state_report",

    "object":"system",

    "payload":
    {
        "state":"success",

        "data":
        {
            "wifi":true,
            "valve":false
        }
    },

    "timestamp":1785514901,

    "source":"device"
}
```


---

# 9. Source来源


用于日志和追踪。


当前：

```text
cloud

device

ui

local
```


未来新增控制端无需修改协议。


---

# 10. 时间格式


设备内部统一使用：

```
Unix timestamp
```


例如：

```
1785514667
```


进入用户显示层时转换为：

```
2026-08-01 02:51:07
```


转换由 Cloud Manager 或 UI 层完成。


---

# 11. MQTT发送规则

## 11.1 现有实现（topic/up 与 topic/set）

⚠️ **注意**：代码中 `cloud_send_set()` 与 `cloud_send_up()` **都发往 `publish_topic`
（`guo_feeder/up`）**，区别只在串口日志标签。因此下表的"set"在当前实现里
**不会产生第二个 Topic**，仅作为历史语义保留。

规则：

|消息|方式|实际 Topic|
|-|-|-|
|ACK|up|`guo_feeder/up`|
|Action结果|up|`guo_feeder/up`|
|Workflow结果|up|`guo_feeder/up`|
|状态上传|up|`guo_feeder/up`|
|配置上传|up|`guo_feeder/up`|
|上线通知|set|`guo_feeder/up`（同上）|

## 11.2 日志发送（🔒 预留，Log 模块启用后生效）

|消息|方式|Topic|说明|
|-|-|-|-|
|日志批次|**log**|`guo_feeder/log`|**独立于 up**，CBOR 编码，批 ≤16 条 |
|`log_ack`|下行|`guo_feeder/down`|复用已有订阅，**不新增订阅** |

```
LogManager
    │  upload_callback（main.cpp 注入）
    ▼
CloudManager ──► MQTT guo_feeder/log
    ▲
    └── log_ack 经 guo_feeder/down 回到 LogManager
```

- **LogManager 不直接调用 MQTT Client**（`AI_RULES` §1 分层要求）。
- Log 使用 `store=0`（耐久性由设备自身的 Flash 段环保证，避免双重持久化）。
- 云端对日志批次需按 `(device_id, boot_seq, seq)` 幂等去重。
- 完整规范见 `docs/interfaces/cloud_protocol.md` §8.2。


---

# 12. 扩展原则


协议冻结以下部分：

固定：

```
id
command
object
payload
timestamp
source
```


新增功能：

只允许增加：

```
command枚举
object定义
payload结构
协议级消息（照 change_msg_limit 的落点，旁路 CommandManager）
Topic（如 log），但必须配置化且既有函数签名不变
```


禁止修改基础 JSON 外壳。

补充（V2.0，按代码实读修订）：

- 基础外壳存在**两套**：旧格式 `cmd/ob/id/pl/src/ts` 与新格式 `c/i/v/k/p/t`，
  二者由是否存在 `c` 判定，**都已冻结，不得单方面废弃**。
- 上行紧凑键集合（`s/c/i/o/p/t/e/m/v/k`）已冻结。
- 命令 ID 唯一性与 10 条 / 30 s 去重语义已冻结。
- 错误码取值（协议层 0–7、业务层 1–13）已冻结。

> 权威规范见 `docs/interfaces/cloud_protocol.md`。




# 
# Workflow Trigger / Action 模块注册规范
#

## 1. 概述

所有业务模块通过 Workflow Framework 注册自身提供的：

- Trigger（触发器）
- Action（动作）

注册完成后，由 Workflow Registry 统一维护模块能力信息。

其他模块：

- Command Manager
- UI
- Cloud Manager
- 自动化流程编辑器

均通过 Workflow 查询接口获取可用 Trigger / Action。

模块自身不维护额外注册表。
---

# 2. 模块注册结构

Workflow 模块结构：

```
Module
 |
 |
 +---- Trigger Descriptor
 |
 +---- Action Descriptor
 |
 +---- Register Function
```

运行流程：

```
Workflow Step
       |
       |
 Trigger / Action Instance
       |
       |
 Descriptor
       |
       |
 start()
       |
       |
 poll()
       |
       |

 SUCCESS / FAILED
```

# 3. Trigger 注册模板


## 3.1 Trigger Reset（必须）


用于 Workflow 重新执行前初始化状态。


```cpp
static void module_trigger_reset(
    WorkflowTriggerInstance *trigger
)
{
    if(trigger == nullptr)
        return;
    trigger->state =
        TRIGGER_IDLE;
    trigger->running =
        false;
    // 清理模块私有 runtime

}
```


---

## 3.2 Trigger Start（必须）


Workflow 第一次进入 Trigger Step 时调用。

```cpp
static void module_trigger_start(
    WorkflowTriggerInstance *trigger
)
{
    if(trigger == nullptr)
        return;
    // 初始化 runtime

    trigger->state =
        TRIGGER_RUNNING;
}
```


---

## 3.3 Trigger Poll（必须）


Workflow Task 周期调用。

要求：

- 非阻塞
- 完成后修改 state


```cpp
static void module_trigger_poll(
    WorkflowTriggerInstance *trigger
)
{
    if(trigger == nullptr)
        return;


    // 查询条件


    if(condition_ok)
    {
        trigger->state =
            TRIGGER_SUCCESS;
    }
}
```


---

# 4. Trigger Descriptor


示例：


```cpp
static WorkflowParam trigger_params[] =
{
    {
        "threshold",
        PARAM_INT,
        "",
        "触发阈值"
    }
};


static const WorkflowTriggerDescriptor
module_trigger_desc =
{
    .id =
        "MODULE_TRIGGER",
    .name =
        "触发名称",
    .module =
        "module",
    .description =
        "触发说明",
    .params =
        trigger_params,
    .param_count =
        1,
    .reset =
        module_trigger_reset,
    .start =
        module_trigger_start,
    .poll =
        module_trigger_poll
};
```

---

# 5. Action 注册模板


Action 支持：

- 同步动作
- 异步动作


生命周期：


```
reset()
   |
start()
   |
poll()
   |
SUCCESS / FAILED
```
---

# 5.1 Action Reset


```cpp
static void module_action_reset(
    WorkflowActionInstance *action
)
{
    if(action == nullptr)
        return;


    action->result =
        ACTION_IDLE;


    action->running =
        false;


    action->runtime =
        nullptr;
}
```


---

# 5.2 Action Start
第一次执行 Action 时调用。
```cpp
static void module_action_start(
    WorkflowActionInstance *action
)
{
    if(action == nullptr)
        return;
    // 执行动作
    action->result =
        ACTION_RUNNING;
    // 如果立即完成

    action->result =
        ACTION_SUCCESS;
}
```
---

# 5.3 Action Poll
异步任务继续执行。
```cpp
static void module_action_poll(
    WorkflowActionInstance *action
)
{
    if(action == nullptr)
        return;
    // 查询执行状态
    if(done)
    {
        action->result =
            ACTION_SUCCESS;
    }
    if(error)
    {
        action->result =
            ACTION_FAILED;
    }
}
```
---

# 6. Action Descriptor

示例：

```cpp
static WorkflowParam action_params[] =
{
    {
        "duration",
        PARAM_INT,
        "ms",
        "执行时间"
    }
};

static const WorkflowActionDescriptor
module_action_desc =
{
    .id =
        "MODULE_ACTION",
    .name =
        "动作名称",
    .module =
        "module",
    .description =
        "动作说明",
    .params =
        action_params,
    .param_count =
        1,
    .reset =
        module_action_reset,
    .start =
        module_action_start,
    .poll =
        module_action_poll
};
```

---

# 7. 模块统一注册入口


每个模块提供：

```cpp
void module_workflow_register();
```

示例：
```cpp
void module_workflow_register()
{
    workflow_register_trigger(
        &module_trigger_desc
    );


    workflow_register_action(
        &module_action_desc
    );
}
```
系统启动时调用：

```cpp
module_workflow_register();
```



---

# 8. ID 命名规范


| 类型 | 格式 | 示例 |
|-|-|-|
| Trigger | 大写 + 下划线 | `WEIGHT_ABOVE` |
| Action | 大写 + 下划线 | `VALVE_OPEN` |



规则：

- 全部大写
- 单词之间使用 `_`
- 必须唯一
- 不允许重复注册



---

# 9. 参数规范


统一使用：

```cpp
WorkflowParam
```
结构：

```cpp
{
    name,
    type,
    unit,
    description
}
```
示例：

```cpp
{
    "speed",
    PARAM_INT,
    "rpm",
    "电机转速"
}
```
参数类型：
| 类型 | 说明 |
|-|-|
| PARAM_INT | 整数 |
| PARAM_FLOAT | 浮点 |
| PARAM_BOOL | 布尔 |
| PARAM_STRING | 字符串 |
---

# 10. 查询接口
Workflow Registry 提供：

## 查询 Trigger
```cpp
workflow_get_trigger_count();

workflow_get_trigger_descriptor(index);
```
## 查询 Action
```cpp
workflow_get_action_count();

workflow_get_action_descriptor(index);
```

Command Manager / UI 不直接访问模块。

统一通过 Workflow Framework 获取能力。
---

# 11. Cloud Command 流程


云端下发：


```
Cloud

 |

Cloud Manager

 |

Command Manager

 |

Workflow Action Registry

 |

workflow_enqueue_action()

 |

Temporary Action Queue

 |

Action Instance

 |

start()

 |

poll()

 |

SUCCESS / FAILED

```



---

# 12. 开发注意事项


## Trigger

必须保证：

- start 非阻塞
- poll 非阻塞
- 不使用 delay()
- 完成修改 state


例如：

```cpp
trigger->state =
    TRIGGER_SUCCESS;
```

---

## Action
同步动作：

```
start()

直接 SUCCESS
```


异步动作：

```
start()

ACTION_RUNNING


poll()

ACTION_SUCCESS
```

禁止：

- 阻塞等待
- while等待完成
- delay()
- 自建任务循环
---

# 13. 当前支持模块

| 模块 | Trigger | Action |
|-|-|-|
| Timer | ✅ | ❌ |
| Delay | ✅ | ❌ |
| Valve | ❌ | ✅ |
| Motor | 待开发 | 待开发 |


---

# 14. 总结

Workflow Framework 负责：

- 生命周期管理
- Step 调度
- Trigger / Action Registry
- JSON解析
- Cloud Action 调用
- 临时 Action 队列

业务模块只负责：

- Descriptor定义
- start()
- poll()
- reset()
实现模块与自动化系统完全解耦。