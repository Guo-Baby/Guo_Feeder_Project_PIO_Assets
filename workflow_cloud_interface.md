# Workflow 云端适配接口文档

**项目：Guo Feeder / 郭氏自动猫粮机**
**目标平台：ESP32-S3 N16R8**
**适用对象：Cloud / Web / App / UI 开发人员**
**固件基线：commit `56b3d55`**

> 本文档是 Cloud 侧对接 Workflow 的**唯一契约**。
> 设备内部的 BIN 格式、LittleFS 布局、事务实现属于私有实现，
> 本文档**不描述、也不允许** Cloud 侧依赖它们。

---

## 目录

1. [分层架构与数据流](#1-分层架构与数据流)
2. [Workflow 概念模型](#2-workflow-概念模型)
3. [JSON 数据模型](#3-json-数据模型)
4. [Definition 与 Runtime](#4-definition-与-runtime)
5. [Workflow 修改限制](#5-workflow-修改限制)
6. [Variant 语义](#6-variant-语义)
7. [Capability Registry 与 Stable ID](#7-capability-registry-与-stable-id)
8. [命令传输格式](#8-命令传输格式)
9. [命令详解](#9-命令详解)
10. [错误码](#10-错误码)
11. [云端同步流程](#11-云端同步流程)
12. [设备端可用 Action / Trigger 清单](#12-设备端可用-action--trigger-清单)
13. [UI 对接注意事项](#13-ui-对接注意事项)
14. [边界与限制](#14-边界与限制)
15. [测试与验证方法](#15-测试与验证方法)

---

## 1. 分层架构与数据流

```text
                       CLOUD / APP
                           │
                           │ JSON
                           ▼
                    ┌───────────────┐
                    │ CloudManager  │  MQTT / 云协议 / 紧凑键
                    └───────┬───────┘
                            │
                            ▼
                    ┌───────────────┐
                    │CommandManager │  命令路由 / 生命周期
                    │   workflow.*  │
                    └───────┬───────┘
                            │
                            ▼
                    ┌───────────────┐
                    │ Workflow.cpp  │  ★ JSON ↔ Definition 语义转换边界
                    │               │
                    │  Variant      │
                    └───────┬───────┘
                            │
                ┌───────────┴───────────┐
                │                       │
                ▼                       ▼
        Workflow Runtime        WorkflowStorage
                                        │
                                        ▼
                              Meta + Step BIN
                                        │
                                        ▼
                                    LittleFS


        Capability Registry  （仅 Stable ID 映射层）
                │
                ├── Action Stable ID
                ├── Trigger Stable ID
                └── Workflow Stable ID
                              │
                              └── object_version = workflow.variant
```

### 1.1 各层职责（不得混层）

| 层 | 负责 | 不负责 |
|---|---|---|
| CloudManager | MQTT、云协议、上行紧凑编码 | Workflow 业务语义 |
| CommandManager | 命令路由、参数提取、错误码、生命周期 | 读写 LittleFS / BIN |
| Workflow.cpp | **JSON ↔ Workflow Definition 语义转换**、Variant | MQTT |
| WorkflowStorage | BIN 序列化、原子事务、备份 | 业务语义、云协议 |
| Capability Registry | Stable ID ↔ Runtime ID 映射 | Workflow 业务逻辑 |

**铁律**：CommandManager **不允许**直接操作 WorkflowStorage / BinStorage / FileStorage。

### 1.2 数据流方向

下行（Cloud 写设备）：

```text
Cloud → JSON → CloudManager → CommandManager → Workflow.cpp
      → Workflow Definition → WorkflowStorage → Meta/Step BIN → LittleFS
```

上行（Cloud 读设备）：

```text
LittleFS → WorkflowStorage → Workflow.cpp → Definition
         → JSON → CommandManager → CloudManager → Cloud
```

---

## 2. Workflow 概念模型

一个 Workflow 是一条**顺序执行**的步骤流水线：

```text
Workflow
  └── Step[]            （最多 16 步，0 基索引）
        ├── Step 0      ★ 必须是 Trigger（触发器）
        └── Step 1..N       Trigger 或 Action
```

- **Step 0 = Trigger**：决定这条 Workflow **何时开始**。
- **Step 1+ = Trigger / Action**：
  - `Action` 立即执行（或异步推进），完成后进入下一步。
  - `Trigger` 作为中间步时表示"在此等待某个条件成立"。
- 执行是**非阻塞状态机**：设备不会因为 Workflow 卡住而死机。

> 设备端不存在"并行分支 / 条件跳转 / 循环"语法。UI 若需要这些语义，
> 请在 Cloud 侧拆成多条 Workflow × 外部调度实现。

---

## 3. JSON 数据模型

### 3.1 Workflow 对象

```json
{
    "id": "morning_water",
    "name": "早间供水",
    "variant": 3,
    "enable": true,
    "timeout_ms": 600000,
    "steps": [
        {
            "type": "trigger",
            "id": "timer",
            "params": { "type": "daily", "hour": 8, "minute": 0 }
        },
        {
            "type": "action",
            "id": "VALVE_OPEN",
            "params": {}
        },
        {
            "type": "trigger",
            "id": "delay",
            "params": { "seconds": 30 }
        },
        {
            "type": "action",
            "id": "VALVE_CLOSE",
            "params": {}
        }
    ]
}
```

| 字段 | 类型 | 必填 | 说明 |
|---|---|---|---|
| `id` | string | ✅ | **Workflow 的业务身份**，设备内唯一。非空。 |
| `name` | string | ✕ | 显示名。缺省时回落为 `id`。 |
| `variant` | uint32 | ✕ | 内容版本号。**由设备维护**，云端提交时该字段被忽略（见 §6）。 |
| `enable` | bool | ✕ | 是否参与自动触发。缺省 `true`（注意：**设备 BIN 加载路径缺省为 `false`**，见 §5.3）。 |
| `timeout_ms` | uint32 | ✕ | 整条 Workflow 的超时（毫秒）。缺省 `600000`（10 分钟）。 |
| `steps` | array | ✅ | 步骤数组，最多 16 项。为空时 Workflow 无实际行为。 |

### 3.2 Step 对象

```json
{ "type": "trigger", "id": "timer", "params": { "hour": 8 } }
{ "type": "action",  "id": "VALVE_OPEN", "params": {} }
```

| 字段 | 类型 | 必填 | 说明 |
|---|---|---|---|
| `type` | string | ✅ | `"trigger"` 或 `"action"`。缺省按 `"action"` 处理。 |
| `id` | string | ✅ | **Runtime ID**，必须与设备注册的 Action/Trigger id 完全一致（见 §12）。 |
| `params` | object | ✕ | 参数键值对，最多 8 个。 |

### 3.3 参数值类型

设备按 JSON 值的实际类型自动映射，**不需要类型标注**：

| JSON 值 | 设备类型 |
|---|---|
| `8` | `PARAM_INT` |
| `1.5` | `PARAM_FLOAT` |
| `true` / `false` | `PARAM_BOOL` |
| `"daily"` | `PARAM_STRING` |

### 3.4 JSON 兼容性承诺

- 本 Schema **保持历史 `workflow.json` 格式不变**，仅新增 `variant` 一个字段。
- 已有字段名、结构、语义**不做任何改动**。
- 云端可以安全地把 `workflow.get` 的返回对象原样改几个字段后回传 `workflow.set`。

### 3.5 未知 `id` 的行为（重要）

若 `step.id` 不是设备已注册的 Runtime ID：

- 设备 **不会报错**，仍会保存这个 Step；
- 但它**不会被实例化**，运行时被跳过，`workflow.get` 会原样回显（`id` 保留）；
- 也就是说：**写错 id 不会失败，但也不会执行**。

> 请云端在提交前用 §12 的清单校验 `step.id`，或对 `workflow.get`
> 的返回结果做一致性比对，避免"保存成功但设备不执行"的静默故障。

---

## 4. Definition 与 Runtime

```text
Workflow Definition   （持久化对象，可被 Cloud 修改）
        │
        │ start
        ▼
Workflow Runtime Snapshot   （执行期的内存快照）
```

**关键结论**：

1. Workflow 运行时使用 **Runtime Snapshot**。
2. 因此 Cloud 修改 Definition **不会影响正在执行的 Runtime**。
3. 正在运行的 Workflow 会**继续按启动时那份快照**跑到结束 / 超时。
4. 修改在下一次启动 Workflow 时生效。

对 Cloud 的含义：

- 不需要"修改前先停止 Workflow"。
- 但若 UI 希望"改完立刻生效"，需要 Cloud 侧自行在合适的时机重新触发。

---

## 5. Workflow 修改限制

### 5.1 RUNNING 状态拒绝整体替换

`workflow.set` / `workflow.create` 走**整体替换**语义（先清空全部 Step
Definition，再逐个写入）。因为 Step Definition 是 Runtime 的取材来源，
运行中替换会让在飞执行异常结束。

设备的行为：

```text
workflow.state == WORKFLOW_RUNNING
    → workflow.set / workflow.create 直接拒绝
    → 错误码 13 (REJECTED)
```

`workflow.delete` **不会**强制终止当前 Runtime，只标记 `valid=false`。

### 5.2 参数修改存在 Definition / Runtime 隔离

改参数改的是 Definition；已在跑的 Runtime 仍用旧参数。

### 5.3 `enable` 的两条路径缺省值不同（已知差异，务必注意）

| 路径 | `enable` 缺省 |
|---|---|
| JSON 导入（`workflow.create` / `set`） | `true` |
| 设备 BIN 加载（重启后从 Flash 恢复） | `false` |

**建议**：Cloud 提交 Workflow 时**始终显式给出 `enable`**，不要依赖缺省值。

### 5.4 云端不需要理解的东西

以下全部属于设备内部机制，Cloud **不要**也不能操作：

`Step BIN` / `Meta BIN` / `LittleFS` / `WorkflowStorage` /
`CRC` / `staged transaction` / `.tmp` / `.bak` / `transaction ID` /
`Dirty bitmap` / `Critical Operation`

---

## 6. Variant 语义

### 6.1 定义

每个 Workflow Slot **独立**拥有一个 32 位无符号整数 `variant`：

```text
Workflow #0 → variant
Workflow #1 → variant
...
Workflow #15 → variant
```

### 6.2 自增规则

| 操作 | variant 变化 |
|---|---|
| `create` | 置为 **1** |
| `set`（内容**真的**变了） | **+1** |
| `set`（内容与当前**完全一致**） | **不变** |
| `delete` | **+1** |
| 重启 | **保持不变**（从 Meta BIN 恢复） |

示例：

```text
create  → variant = 1
set     → variant = 2
set     → variant = 3
delete  → variant = 4
```

### 6.3 "内容相同"的判定方式

设备用 **canonical 文本比较**，与 JSON 字段书写顺序、参数书写顺序无关：

- 固定字段顺序输出；
- 参数按 `name` 升序排序；
- 每个值带类型前缀（`i` / `f` / `b` / `s`），避免 `1` 与 `"1"` 混淆。

因此"打开编辑 → 原样保存"不会让 variant 虚增。

### 6.4 云端应当怎么做

- **只读**：把 `variant` 当作"内容是否变化"的指纹。
- **不要写**：提交时给 `variant` 赋值没有任何效果，设备会自行覆盖。
- **不要比时间**：`variant` 没有时间语义，**不能**用来判断"谁更新"。

---

## 7. Capability Registry 与 Stable ID

### 7.1 三个概念严格分离

| 概念 | 含义 | 变化时机 |
|---|---|---|
| `stable_id` | 某个对象的**紧凑定位编号**（0 基） | 对象集合变化时**整组重排** |
| `variant` | **单个 Workflow** 的内容版本 | 该 Workflow 内容变化时 +1 |
| `registry_version` | **整体 Mapping** 的版本 | 任一 `runtime_id` / `stable_id` / `object_version` 变化时 +1 |

`registry_checksum` 是 Mapping 的校验值，用于快速判断"是否真的变了"。

### 7.2 Stable ID 的生成规则（Cloud 必须理解）

```text
1. 收集当前全部 runtime_id
2. 按 runtime_id 字符串升序排序
3. 排序后的下标即 stable_id
```

**因此 `stable_id` ≠ Workflow Slot 编号。**

实测证据（同一次运行，新增 `cm_a` 前后）：

```text
新增前： #0 daily_valve_test  #1 daily_valve_test1  #2 queue_test
新增后： #0 cm_a  #1 daily_valve_test  #2 daily_valve_test1  #3 queue_test
            ▲
            cm_a 字母序在前 → 顶到 0，其余全部 +1 平移
```

> 注意：新增 / 改名的 Workflow 若字母序靠前，会让**后面所有对象**
> 的 `stable_id` 整体平移。这不是 bug，是排序编号的固有性质。

### 7.3 object_version

```text
CapabilityMapping.object_version = workflow.variant
```

即：Registry 里每个 Workflow 条目的 `object_version` 就是它的 `variant`。

作用链：

```text
Workflow 内容变化
    → variant++
    → object_version 变化
    → registry_checksum 变化
    → registry_version++
    → 云端 list 比对发现不同
    → workflow.get 拉取新内容
```

### 7.4 云端使用 Stable ID 的正确姿势

**可以**：在同一次同步会话中，用 `stable_id` 快速定位对象。

**不可以**：

- ❌ 把它当作对象永久 ID 存库并长期引用。
- ❌ 假设"同一 `id` 的 `stable_id` 永远不变"。

**正确做法**：

- 以 **`id` 作为业务主键**，`stable_id` 只作本次会话的加速器。
- 持久化 `stable_id` / `id` / `variant` / `valid` 四元组。
- 当 `registry_version` 变化时，**重新拉取 `workflow.list` 并重建映射**。

---

## 8. 命令传输格式

### 8.1 两条入口，同一条链路

| 入口 | 格式 | 用途 |
|---|---|---|
| MQTT / 云 | `{"c":"workflow.list","i":"<唯一id>","p":{...}}` | 生产 |
| 串口调试 | `cm {"cmd":"workflow.list","ob":"-","id":"<唯一id>","p":{...}}` | 现场调试 |

两者进入的是**同一个** `command_manager_execute()`，行为完全一致。

### 8.2 请求字段

`CommandMessage` 逻辑字段：

| 逻辑名 | 串口 `cm` | 云协议 | 说明 |
|---|---|---|---|
| `command` | `cmd` | `c` | 命令名，如 `workflow.list` |
| `object` | `ob` | `o` | 对象名（Workflow 命令不支持该定位方式，见 §9.3） |
| `cmd_id` | `id` | `i` | **每条命令必须唯一**，设备按它去重与匹配回调 |
| `payload` | `p` / `pl` | `p` | 参数对象 |

> `cmd_id`（`i`）重复会被直接拒绝（错误码 8）。请 Cloud 侧使用
> 递增序号或 UUID。

### 8.3 上行格式

CommandManager 产出**明文逻辑形式**：

```json
{
    "registry_version": 19,
    "registry_checksum": 1473447791,
    "count": 3,
    "dirty": false,
    "status": "success",
    "cmd": "result",
    "id": "a1",
    "command": "workflow.sync_info",
    "timestamp": 1789151238
}
```

CloudManager 会压缩为**线上紧凑形式**（`s/c/i/t/o/k`）：

```json
{
    "s": 0,
    "c": "result",
    "i": "a1",
    "t": 1789151238,
    "registry_version": 19,
    "registry_checksum": 1473447791,
    "count": 3,
    "dirty": false,
    "command": "workflow.sync_info"
}
```

**紧凑键映射表**：

| 逻辑字段 | 紧凑键 | 含义 |
|---|---|---|
| `status` | `s` | `0` = 成功；`1` = 失败 |
| `cmd` | `c` | 固定 `"result"` |
| `id` | `i` | 原样回显请求的 `cmd_id` |
| `timestamp` | `t` | 设备 Unix 时间（未同步到时间时为 0/缺省） |
| `object` / `action` / `workflow` / `key` / `ob` | `o` | 对象负载 |
| `stable_id` | `k` | 稳定编号 |

其余业务字段（`variant` / `valid` / `slot` / `dirty` / `count` /
`registry_version` / `registry_checksum` / `workflows` / `steps` …）
**原样透传，不改名**。

**错误上行**：

```json
{ "m": "workflow stable_id not found", "e": 5, "i": "g1", "t": 1789151399, "type": "command" }
```

`e` = 错误码（见 §10），`m` = 可读消息。

---

## 9. 命令详解

> 以下示例统一使用串口形式 `cm {...}`，字段与云协议一一对应。
> 每条命令的 `id` 都必须唯一。

### 9.1 `workflow.sync_info` —— 轻量同步入口

**请求**

```json
cm {"cmd":"workflow.sync_info","id":"9010","p":{}}
```

**参数**：无。

**返回**

```json
{
    "s": 0, "c": "result", "i": "9010", "t": 1789151238,
    "registry_version": 19,
    "registry_checksum": 1473447791,
    "count": 3,
    "dirty": false,
    "command": "workflow.sync_info"
}
```

| 字段 | 说明 |
|---|---|
| `registry_version` | 整体 Mapping 版本 |
| `registry_checksum` | Mapping 校验值 |
| `count` | 当前 Workflow 对象数（**含 `valid=false` 的已删对象**） |
| `dirty` | 是否存在**尚未落盘**的修改 |

**variant 行为**：无。
**dirty 行为**：只读。
**用途**：一次往返判断"要不要继续同步"。`registry_version` +
`registry_checksum` 都没变 → 整轮同步可跳过。

> `sync_info` **不返回** Workflow 列表，也不返回完整 Workflow。

---

### 9.2 `workflow.list` —— 同步入口（摘要）

**请求**

```json
cm {"cmd":"workflow.list","id":"9011","p":{}}
```

**参数**：无。

**返回**

```json
{
    "s": 0, "c": "result", "i": "9011", "t": 1789151267,
    "registry_version": 20,
    "registry_checksum": 2676047148,
    "count": 4,
    "dirty": true,
    "workflows": [
        { "stable_id": 0, "id": "cm_a",             "variant": 1, "valid": true  },
        { "stable_id": 1, "id": "daily_valve_test", "variant": 1, "valid": true  },
        { "stable_id": 2, "id": "daily_valve_test1","variant": 1, "valid": true  },
        { "stable_id": 3, "id": "queue_test",       "variant": 1, "valid": true  }
    ],
    "command": "workflow.list"
}
```

| 字段 | 说明 |
|---|---|
| `stable_id` | 本次 Mapping 下的定位编号（§7.2，会整体重排） |
| `id` | **业务主键** |
| `variant` | 该 Workflow 的内容版本 |
| `valid` | `false` = 已被 `workflow.delete` 逻辑删除 |
| `count` | 数组长度（含 `valid=false`） |

**variant 行为**：无。
**dirty 行为**：只读。
**注意**：**不在 list 中返回 Steps**。要完整内容必须 `workflow.get`。

---

### 9.3 `workflow.get` —— 拉取单个完整 Workflow

**请求**（三种定位方式，优先级从高到低）

```json
cm {"cmd":"workflow.get","id":"9012","p":{"stable_id":0}}
cm {"cmd":"workflow.get","id":"9013","p":{"id":"cm_a"}}
cm {"cmd":"workflow.get","id":"9014","ob":"cm_a"}
```

| 参数 | 说明 |
|---|---|
| `p.stable_id` | 按 Stable ID 定位（优先） |
| `p.id` | 按业务 id 定位 |
| `ob` | 第三顺位：对象名字段 |

**返回**

```json
{
    "s": 0, "c": "result", "i": "9012", "t": 1789151273, "k": 0,
    "o": {
        "id": "cm_a",
        "name": "A",
        "variant": 1,
        "enable": true,
        "timeout_ms": 30000,
        "steps": [
            { "type": "trigger", "id": "timer_daily", "params": { "hour": 9, "minute": 30 } },
            { "type": "action",  "id": "VALVE_OPEN" }
        ]
    },
    "valid": true,
    "command": "workflow.get"
}
```

| 字段 | 说明 |
|---|---|
| `o` | 完整 Workflow 对象（逻辑名为 `workflow`），**可直接回传 `workflow.set`** |
| `k` | 该对象的 `stable_id`（逻辑名 `stable_id`） |
| `valid` | 放在 `o` **外面**：`o` 是纯持久化 Schema，不掺设备侧删除标记 |

**variant 行为**：无。
**dirty 行为**：只读。
**错误**：`5`（对象不存在）。

> `o` 内部**不含** `valid`。判断是否已删除请看顶层 `valid`。

---

### 9.4 `workflow.create` —— 新建

**请求**

```json
cm {"cmd":"workflow.create","id":"9015","p":{"workflow":{
  "id":"morning_water",
  "name":"早间供水",
  "enable":true,
  "timeout_ms":600000,
  "steps":[
    {"type":"trigger","id":"timer","params":{"type":"daily","hour":8,"minute":0}},
    {"type":"action","id":"VALVE_OPEN"}
  ]
}}}
```

| 参数 | 说明 |
|---|---|
| `p.workflow` | **必填**。完整 Workflow 对象（必须嵌套在 `workflow` 键下） |
| `p.workflow.id` | 必填，非空 |

> **形态唯一**：内容必须嵌套在 `p.workflow` 下。
> **不支持**把 Workflow 字段平铺到 `p` 上 —— 因为 `set` 的定位字段同样是
> `p.id`，平铺形态会让"只想定位"的请求被误判为"完整替换"，
> 从而清空目标 Workflow。设备对此类请求统一返回错误码 `6`。

**返回**

```json
{
    "s": 0, "c": "result", "i": "9015", "t": 1789151262, "k": 0,
    "slot": 3, "variant": 1, "dirty": true,
    "command": "workflow.create"
}
```

**行为**

| 项目 | 结果 |
|---|---|
| `variant` | 置 **1** |
| `valid` | `true` |
| `dirty` | `true` |
| Slot 选择 | 优先未使用 Slot；无空位时**回收已 `delete` 的 Slot** |
| 立即可见 | ✅ `list` / `get` 立刻可见（无需重启） |
| Registry | 立即 rescan → `registry_version++` |

**错误**：`6`（缺 `workflow` / 缺 `id`）、`12`（16 个 Slot 全满且无可回收）、`13`（运行中 / Critical 获取失败）。

---

### 9.5 `workflow.set` —— 整体替换

**请求**

```json
cm {"cmd":"workflow.set","id":"9016","p":{
  "id":"morning_water",
  "workflow":{
    "id":"morning_water",
    "name":"早间供水 v2",
    "enable":true,
    "timeout_ms":600000,
    "steps":[
      {"type":"trigger","id":"timer","params":{"type":"daily","hour":7,"minute":30}},
      {"type":"action","id":"VALVE_OPEN"}
    ]
  }
}}
```

| 参数 | 说明 |
|---|---|
| `p.id` / `p.stable_id` / `ob` | 定位目标对象（同 §9.3 规则） |
| `p.workflow` | **必填**。**完整**新内容（**不支持 PATCH**，也不支持平铺到 `p` 上） |

> **缺少 `p.workflow` 时返回错误码 `6`，不会清空目标对象。**
> 这是刻意的保护：避免"只想定位"的半截请求被当成整体替换执行。

**返回（内容真的变了）**

```json
{
    "s": 0, "c": "result", "i": "9016", "t": 1789151278, "k": 0,
    "slot": 3, "variant": 2, "changed": true, "dirty": true,
    "command": "workflow.set"
}
```

**返回（内容完全一致，幂等）**

```json
{
    "s": 0, "c": "result", "i": "9017", "t": 1789151284,
    "slot": 3, "variant": 2, "changed": false, "dirty": true,
    "command": "workflow.set"
}
```

**行为**

| 情况 | variant | `changed` | dirty |
|---|---|---|---|
| 内容有变化 | **+1** | `true` | `true` |
| 内容完全一致 | **不变** | `false` | 不变 |

- 整体替换：新 `steps` 比旧的少时，**尾部 Step 真的消失**。
- 空 `steps` 合法：Workflow 存在但无行为。
- 超过 16 步：**截断到 16** 并继续（不报错）。

**错误**：`5`、`6`、`13`（运行中 / Critical 获取失败）。

---

### 9.6 `workflow.delete` —— 逻辑删除

**请求**

```json
cm {"cmd":"workflow.delete","id":"9018","p":{"id":"morning_water"}}
cm {"cmd":"workflow.delete","id":"9019","p":{"stable_id":0}}
```

**返回**

```json
{
    "s": 0, "c": "result", "i": "9018", "t": 1789151305,
    "slot": 4, "variant": 2, "dirty": true,
    "command": "workflow.delete"
}
```

**行为**

```text
valid   = false
variant = variant + 1
dirty   = true
```

- **不物理删除** Step BIN，也不立即删除文件。
- 不会终止正在运行的 Runtime。
- 删除后到重启前的窗口内，`workflow.list` **仍然能看到它**，
  标记为 `valid=false` —— 这是**设计行为**，让 Cloud 明确得知
  "对象被删了"，而不是"它凭空消失"。
- 重启后该 Slot 不再作为有效 Workflow 加载。
- 无需重启即可被新 `create` 回收（`workflow_find_free_slot` 第二轮）。

**variant 行为**：`+1`。
**错误**：`5`。

---

### 9.7 `workflow.save` —— 全局 Dirty 落盘

**请求**

```json
cm {"cmd":"workflow.save","id":"9020","p":{"restart":false}}
cm {"cmd":"workflow.save","id":"9021","p":{"restart":true}}
```

| 参数 | 类型 | 缺省 | 说明 |
|---|---|---|---|
| `p.restart` | bool | `true` | 保存成功后是否请求安全重启 |

**返回（成功）**

```json
{
    "s": 0, "c": "result", "i": "9020", "t": 1789151341,
    "saved": true, "dirty": false, "restarting": false,
    "registry_version": 26,
    "command": "workflow.save"
}
```

**返回（部分失败）**

```json
{
    "m": "workflow save failed (dirty kept for retry)",
    "e": 10, "i": "9022", "t": 1789151374, "type": "command"
}
```

**语义（★本次重点）**

`workflow.save` 是**全局**操作，不是保存某一个 Workflow。它必须：

```text
1. 处理【全部】Dirty Workflow（不是第一个，也不是调用方指定的那个）
2. 每一个都落盘成功
        ↓
3. 才允许 clear dirty
4. 才允许 release Workflow Edit Critical
5. 若 restart=true，才请求 SystemCommand Restart
```

**任一个失败**：

```text
Dirty 保留  +  Critical 保留  →  返回错误码 10
之后可直接重试 workflow.save，剩余对象会继续落盘
```

**decision 表**

| 场景 | `saved` | `dirty` | `restarting` | 重启 |
|---|---|---|---|---|
| 有 Dirty，全部成功，`restart:true` | `true` | `false` | `true`（请求成功时） | 是 |
| 有 Dirty，全部成功，`restart:false` | `true` | `false` | `false` | 否 |
| **无 Dirty**，`restart:true` | `true` | `false` | **`false`** | **否（不空转重启）** |
| 任一失败 | 错误码 10 | `true`（保留） | — | 否 |

**variant 行为**：无（只落盘）。
**错误**：`10`（保存失败，Dirty 已保留）。

> 落盘是**异步安全重启**：写入成功后才请求重启，因此不会出现
> "重启后修改丢失"。

---

## 10. 错误码

| `e` | 名称 | 触发场景 |
|---|---|---|
| `1` | Unknown command | 命令名不存在（如 `workflow.foo`） |
| `2` | Missing command | 请求缺命令名 |
| `3` | Missing object | 缺少定位字段 |
| `4` | Action not found | 动作不存在 |
| `5` | **Workflow not found** | `stable_id` / `id` 找不到对象 |
| `6` | **Invalid payload / missing workflow** | payload 非对象、缺 `workflow`、缺 `id` |
| `7` | Queue full | 命令运行时队列满 |
| `8` | Duplicate cmd_id | `i` / `id` 重复 |
| `9` | System | 系统级错误 |
| `10` | Execution | `workflow.save` 保存失败（Dirty 已保留） |
| `11` | **Invalid JSON payload** | payload 不是合法 JSON |
| `12` | **No free slot** | Workflow 槽位满（16）且无可回收 |
| `13` | **Rejected** | 运行中 / Critical 获取失败 |

错误上行示例：

```json
{"m":"Unknown workflow command: workflow.foo","e":1,"i":"g2","t":1789151404,"type":"command"}
{"m":"workflow stable_id not found","e":5,"i":"g1","t":1789151399,"type":"command"}
{"m":"Missing 'workflow' object","e":6,"i":"g3","t":1789151409,"type":"command"}
```

---

## 11. 云端同步流程

### 11.1 完整同步（首次 / 怀疑不一致）

```text
1. workflow.sync_info
       ↓ 比对 registry_version + registry_checksum
       ↓ 都相同 → 结束（可跳过整轮）
2. workflow.list
       ↓ 逐条比对 (id, variant, valid)
       ↓ 与服务端 Cache 一致 → 跳过
       ↓ 不一致 → 记入待拉取集合
3. 对每个变化的 Workflow: workflow.get
       ↓
4. 用返回的 o 覆盖服务端缓存；用顶层 valid 决定"有效/已删"
       ↓
5. 重建 stable_id ↔ id 映射（registry_version 变了就必须重建）
```

### 11.2 增量同步（常规轮询）

```text
workflow.sync_info
   ↓ registry_version 未变
   结束
   ↓ 变了
workflow.list → get 仅 variant 变化的对象 → 更新 Cache
```

### 11.3 设备重启后的同步（★关键机制）

设备**不会**主动上报"我重启了"，也不会在启动后主动上传全部 Workflow。
由 Cloud 负责在检测到设备重新上线后重新同步：

```text
Device reboot
    ↓
Device online（MQTT 重连）
    ↓
Cloud 发起 workflow.sync_info
    ↓
workflow.list
    ↓
比较 variant / valid / registry 信息
    ↓
只 GET 有变化的 Workflow
```

> 因此 Cloud 侧的 MQTT 上线事件处理里，
> **必须**包含一次 `workflow.sync_info`。

### 11.4 云端写入流程

```text
UI 编辑
  ↓
组装完整 Workflow JSON（显式给出 enable）
  ↓
create / set
  ↓
（可选）轮询 workflow.list 确认 variant 已更新
  ↓
workflow.save  {"restart": true}
  ↓
设备落盘 → 请求安全重启 → 重连后由 §11.3 重新同步
```

> **推荐**：一批修改合并成一次 `workflow.save`，避免多次重启。

---

## 12. 设备端可用 Action / Trigger 清单

> 以下为基线 `56b3d55` 实测注册结果（ACTION 7 个 / TRIGGER 3 个）。
> **`step.id` 必须逐字符匹配**。清单随固件版本变化，请以设备
> `registry` 查询为准。

### 12.1 Trigger（3）

| Runtime ID | 名称 | 参数 | 说明 |
|---|---|---|---|
| `timer` | Timer | `type`(string) `hour`(int 0-23) `minute`(int 0-59) `year`(int) `month`(int 1-12) `day`(int 1-31) `day_of_week`(int 0-6, 0=周日) | 系统定时触发器，`type` 取 `once` / `daily` / `weekly` |
| `delay` | Delay | `seconds`(int 1-3600) | 延时触发器 |
| `weight_decrease` | 重量减少 | `gram`(float, `0 < gram ≤ 300`) | 重量减少到阈值时触发 |

### 12.2 Action（7）

| Runtime ID | 名称 | 参数 | 说明 |
|---|---|---|---|
| `VALVE_OPEN` | 电磁阀开 | 无 | 打开电磁阀 |
| `VALVE_CLOSE` | 电磁阀关 | 无 | 关闭电磁阀 |
| `COMPUTER_RESET` | 电脑重启 | 无 | GPIO8 输出 800ms 高电平脉冲 |
| `WEIGHT_ZERO` | 称重归零 | 无 | HX711 零点校准（异步） |
| `MI_THERMO_START_SCAN` | 开启温湿度扫描 | 无 | 米家温湿度计扫描 |
| `MI_THERMO_STOP_SCAN` | 关闭温湿度扫描 | 无 | 停止米家温湿度计扫描 |
| `MQTT_TEST` | MQTT长度测试 | `length`(int, byte) | 测试用 |

### 12.3 最小可用 Workflow 示例

定时 8:00 开阀 30 秒后关阀：

```json
{
    "id": "morning_water",
    "name": "早间供水",
    "enable": true,
    "timeout_ms": 120000,
    "steps": [
        { "type": "trigger", "id": "timer",     "params": { "type": "daily", "hour": 8, "minute": 0 } },
        { "type": "action",  "id": "VALVE_OPEN",  "params": {} },
        { "type": "trigger", "id": "delay",       "params": { "seconds": 30 } },
        { "type": "action",  "id": "VALVE_CLOSE", "params": {} }
    ]
}
```

---

## 13. UI 对接注意事项

### 13.1 UI 只能操作这 7 个命令

```text
workflow.sync_info
workflow.list
workflow.get
workflow.create
workflow.set
workflow.delete
workflow.save
```

**禁止**让 UI 感知 `Step BIN` / `Meta BIN` / `LittleFS` / `WorkflowStorage`。

### 13.2 读写路径

```text
UI Model → Workflow JSON → workflow.set  → ESP32
ESP32   → Workflow JSON → workflow.get  → UI Model
```

**`workflow.get` 的 `o` 可以直接作为 UI 编辑器的输入**，
编辑完再原样回传 `workflow.set`（整体替换，不是 PATCH）。

### 13.3 四个必须遵守的约定

1. **显式写 `enable`**（§5.3 两条路径缺省值不同）。
2. **`step.id` 用 §12 清单校验**（写错不会报错，但不会执行）。
3. **不要持久化 `stable_id` 作为主键**，用 `id`（§7.4）。
4. **判断"已删除"看顶层 `valid`**，不是看对象是否存在（§9.6）。

### 13.4 推荐 UI 状态机

```text
[已同步] --用户编辑--> [本地草稿]
      --提交--> create/set --成功--> [已提交未落盘: dirty=true]
      --workflow.save--> [已落盘] --重启+重连--> sync_info/list --> [已同步]
```

若 `dirty=true` 而用户离开页面，应提示"修改尚未落盘"。

---

## 14. 边界与限制

| 项目 | 限制 | 超限行为 |
|---|---|---|
| Workflow 数量 | **16** | `create` 返回 `12 NO_FREE_SLOT`（若无可回收的已删 Slot） |
| 每 Workflow Step 数 | **16** | 超出**截断**，不报错 |
| 每 Step 参数个数 | **8** | 超出部分丢弃 |
| `variant` | uint32 | 溢出回绕到 1（不归 0） |
| `id` | 非空字符串 | 空 → 错误码 6 |
| 运行中整体替换 | 禁止 | 错误码 13 |
| 命令 `i`/`id` | 必须唯一 | 重复 → 错误码 8 |

**尚未覆盖验证**：16 Workflow × 16 Step 满载场景（见测试报告"未验证边界"）。

---

## 15. 测试与验证方法

### 15.1 串口直通（无需 MQTT）

`cm` 走的是与 MQTT **完全相同**的 `command_manager_execute()` 链路：

```text
cm {"cmd":"workflow.sync_info","id":"t1","p":{}}
```

### 15.2 脚本化回归

打开串口会复位开发板，单条手敲会与 boot 日志竞争，因此使用批处理脚本：

```bash
python .pio/serial_batch.py <COM口> <日志文件> <用例文件> [单条等待秒]
```

用例文件格式（`|||` 后为期望子串）：

```text
cm {"cmd":"workflow.list","id":"t1"} ||| "valid"
cm {"cmd":"workflow.save","id":"t2","p":{"restart":false}} ||| "saved":true
```

已落地用例：

| 文件 | 覆盖 |
|---|---|
| `.pio/wf_sync_tests.txt` | §33–§37：基础 / 创建 / 修改 / 幂等 / 删除 / 多对象 save / **保存失败** / 错误码 |
| `.pio/wf_reboot_tests.txt` | §35：重启后 variant 保持、删除不复活 |

### 15.3 故障注入（保存失败测试）

```text
wfst failwf <n>     # 让第 n 个 Workflow 的下一次 save 失败（一次性）
wfst fail <n>       # 让下一次 save 的第 n 个 Step 写失败（一次性）
wfst abort1         # 预提交阶段中断
wfst abort2         # 提交后中断
```

典型序列（验证"部分失败不得清空全部 Dirty"）：

```text
wfst failwf 6                                    # 让 slot 6 保存失败
cm set slot5 ...                                 # 两个对象都改脏
cm set slot6 ...
cm workflow.save {"restart":false}   → e=10, dirty 仍为 true
cm workflow.save {"restart":false}   → saved=true, dirty=false
```

---

## 附录 A：命令速查表

| 命令 | 参数 | 关键返回 | variant | dirty |
|---|---|---|---|---|
| `workflow.sync_info` | — | `registry_version` `registry_checksum` `count` `dirty` | — | 只读 |
| `workflow.list` | — | `workflows[{stable_id,id,variant,valid}]` | — | 只读 |
| `workflow.get` | `stable_id` \| `id` \| `ob` | `o{...}` `k` `valid` | — | 只读 |
| `workflow.create` | `workflow{...}` | `slot` `stable_id` `variant` | `=1` | `true` |
| `workflow.set` | `id`+`workflow{...}` | `slot` `variant` `changed` | `+1`／不变 | 变化时为 `true` |
| `workflow.delete` | `stable_id` \| `id` | `slot` `variant` | `+1` | `true` |
| `workflow.save` | `restart`(缺省 true) | `saved` `dirty` `restarting` `registry_version` | — | 全部成功才清零 |

## 附录 B：相关文档

| 文档 | 内容 |
|---|---|
| `readme.md` | 项目总纲、四层架构 |
| `workflow_cloud_sync_progress.md` | 本任务进度与测试证据 |
| `workflow_storage改造进度.md` | BIN 存储层实现 |
| `cloud_protocol.md` | 云协议总览 |
| `critical_operation接入规范.md` | Safe Restart 与 Critical 接入 |
