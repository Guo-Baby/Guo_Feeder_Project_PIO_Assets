# Workflow 云端适配接口文档

**项目：Guo Feeder / 郭氏自动猫粮机**
**目标平台：ESP32-S3 N16R8**
**适用对象：Cloud / Web / App / UI 开发人员**
**固件基线：commit `cfc8dbc`（Cloud Contract 修订版）**

> 本文档是 Cloud 侧对接 Workflow 的**唯一契约**。
> 设备内部的 BIN 格式、LittleFS 布局、事务实现属于私有实现，
> 本文档**不描述、也不允许** Cloud 侧依赖它们。

---

## 0. Workflow 定位模型（★ 最重要，先读这一节）

```text
                    Workflow
                       │
              ┌────────┴────────┐
              │                 │
           p.id             workflow.id
              │                 │
     唯一 Slot 定位         用户业务 ID
       （整数 0..15）        （字符串）
              │                 │
        不可修改             可重复
              │                 │
      Cloud 用它定位       Cloud 不得用它
         对象                 做唯一主键
```

| | `p.id` | `workflow.id` |
|---|---|---|
| 是什么 | 设备内部 **Slot 索引** | 用户定义的**业务 ID / 名称** |
| 类型 | 整数 `0..15` | 字符串 |
| 唯一性 | **唯一**（一个 Slot 一个对象） | **不保证唯一，允许重复** |
| 能否修改 | 不能（由设备分配） | 可以任意改 |
| 用途 | `create`/`set`/`delete`/`get` 的**唯一定位键** | 展示、业务语义 |
| 出现在哪 | 命令的 `p.id`，以及 `list`/`get` 响应的 `slot` 字段 | Workflow JSON 的 `id` 字段 |

**铁律**

1. 修改类命令（`workflow.set` / `workflow.delete`）**必须**用 `p.id` 定位。
   设备**不会**根据 `workflow.id` 去搜索对象。
2. `workflow.id` **允许重复**：两个 Slot 可以叫同一个名字，甚至内容完全一致。
   `workflow.create` 不会因为 `workflow.id` 重复而失败。
3. `p.id` 与 `workflow.id` **没有任何唯一性关系**，设备不做 `p.id == workflow.id` 校验。
4. `stable_id` 是 Capability Registry 的**映射元数据**，不是 Workflow 主键（见 §7）。

**错误示范 / 正确示范**

```jsonc
// ✗ 错：把业务 ID 当定位键
{ "cmd": "workflow.set", "p": { "id": "morning", "workflow": { ... } } }
//   → 设备会尝试把 "morning" 解析成整数 Slot，失败 → error 6

// ✓ 对：用 Slot 定位，workflow.id 只是内容
{ "cmd": "workflow.set", "p": { "id": 3, "workflow": { "id": "morning", ... } } }
```

---

## 目录

0. [Workflow 定位模型（★最重要）](#0-workflow-定位模型-最重要先读这一节)
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
| `id` | string | ✅ | **用户业务 ID / 名称**。非空，**允许重复**，设备不做唯一性校验。 |
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
| `type` | string | ✅ | `"trigger"` 或 `"action"`。**必填，缺失一律拒绝（error 11）**，不会默认成 `action`。 |
| `id` | string | ✅ | **Runtime ID**，必须与设备注册的 Action/Trigger id 完全一致（见 §12）。 |
| `params` | object | ✕ | 参数键值对，**最多 8 个**；超过 8 个整体拒绝（error 11），不截断。 |

> **为什么 `type` 不能默认？** 用户本想表达 Trigger 却漏写 `type` 时，
> 静默默认成 `action` 会产生危险的语义错误（该等待的条件变成了立即执行）。
> 同理 `steps > 16` 时设备**不会**"只保存前 16 个" —— 静默丢数据比报错更糟。

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

> **Workflow Definition** 是设备侧对 Workflow 的**结构化定义表示**，
> 与 Cloud 的 Workflow JSON 对应。Workflow 启动时会从 Definition
> 创建 **Runtime Snapshot**。
> 持久化由 WorkflowStorage **独立负责**，Cloud 不需要感知具体的
> BIN / Meta / LittleFS 实现。

```text
Cloud JSON
     ↓
Workflow Definition
     ↓
Runtime Snapshot        （执行期内存快照）
```

同时存在一条**独立**路径：

```text
Workflow Definition
     ↓
WorkflowStorage
     ↓
Flash
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
| `create` | 置为 **1**（即使已存在内容完全相同的另一个 Workflow，也是 1） |
| `set`（内容**真的**变了） | **+1** |
| `set`（内容与当前**完全一致**） | **不变** |
| `delete`（目标当前 `valid=true`） | **+1** |
| `delete`（目标已经 `valid=false`） | **不变**（幂等 no-op，见 §9.6） |
| 重启 | **保持不变**（从 Meta BIN 恢复） |

示例：

```text
create   → variant = 1
set      → variant = 2
set      → variant = 3
delete   → variant = 4
delete   → variant = 4   ← 第二次 delete 是 no-op
```

> **`variant` 是"内容版本号 / 修订号"，不是内容 Hash，也不是内容指纹。**
> 它只表达"这个对象被改过几次"，不能用来判断两份内容是否相同 ——
> 两个内容完全相同的 Workflow 完全可能拥有不同的 `variant`。

### 6.3 "内容相同"的判定方式

设备比较 Workflow 的**语义内容**：

- **JSON 成员顺序不影响语义**：`{"id":"x","enable":true}` 与
  `{"enable":true,"id":"x"}` 视为相同；
- 参数成员顺序同样不影响语义；
- `variant` 本身**不参与**比较（它正是被本次操作改变的字段）；
- 字段缺省值、类型归一规则由固件内部实现（canonical 文本比较），
  Cloud 无需感知具体算法。

因此"打开编辑 → 原样保存"不会让 variant 虚增。

### 6.4 云端应当怎么做

- **只读**：把 `variant` 当作"这个对象的内容是不是变过了"的修订号。
- **不要写**：提交时给 `variant` 赋值没有任何效果，设备会自行覆盖。
- **不要当指纹**：不能由 `variant` 相同推断内容相同，反之亦然。
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

- 以 **`p.id`（Slot）作为 Workflow 的唯一主键**；
  `workflow.id` 只是业务名称，**不唯一**，不能当主键。
- `stable_id` 只作本次会话的加速器（例如 `workflow.get` 的入口）。
- 持久化 `slot` / `id` / `variant` / `valid` 四元组。
- 当 `registry_version` 变化时，**重新拉取 `workflow.list` 并重建映射**。

### 7.5 两种定位方式不得混用

| 场景 | 用哪个 |
|---|---|
| 修改 / 删除某个 Workflow | **`p.id`（Slot）** |
| 查询某个 Workflow | **`p.id`（Slot）**，`stable_id` 亦可（会解析回 Slot） |
| Capability Registry 映射（紧凑编码） | `stable_id` |
| 业务展示 / 命名 | `workflow.id` |

> `workflow.id` 允许重复，因此它**不能**作为任何单对象操作的定位条件。
> 设备已移除"按 `workflow.id` 搜索对象"的能力。

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
| `registry_version` | **整体** Capability Mapping 的版本。它可能因为 Action / Trigger / **Workflow** 任一类映射变化而变，**不能**据此推断"只有 Workflow 变了" |
| `registry_checksum` | Mapping 的完整性校验值。**必须与 `registry_version` 组合判断**（`version + checksum`） |
| `count` | 当前 **占用 Slot 数**（**含 `valid=false` 的逻辑删除对象**）。**不等于**"可执行 Workflow 数量" |
| `dirty` | **全局 Dirty**：RAM 中"至少存在一个 Workflow 修改尚未持久化"。它**不告诉你是哪一个** |

**variant 行为**：无。
**dirty 行为**：只读。
**用途**：一次往返判断"要不要继续同步"。`registry_version` +
`registry_checksum` 都没变 → 整轮同步可跳过。
发生变化 → 继续 `workflow.list`，由列表判断具体是什么变了。

> `sync_info` **不返回** Workflow 列表，也不返回完整 Workflow。
>
> ⚠️ `count` 只是"存在多少个 Workflow object / 占用 Slot"，
> 要知道真正有效（可执行）的数量，请读 `workflow.list` 的
> `workflows[].valid` 自行统计。

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
        { "slot": 0, "id": "cm_a",             "variant": 1, "valid": true,  "stable_id": 0 },
        { "slot": 3, "id": "daily_valve_test", "variant": 1, "valid": true,  "stable_id": 1 },
        { "slot": 4, "id": "daily_valve_test1","variant": 1, "valid": true,  "stable_id": 2 },
        { "slot": 7, "id": "queue_test",       "variant": 1, "valid": false, "stable_id": 3 }
    ],
    "command": "workflow.list"
}
```

| 字段 | 说明 |
|---|---|
| **`slot`** | **设备 Slot 编号 = `p.id`。这是云端的唯一定位键** |
| `id` | 用户业务 ID。**允许重复**，不是主键 |
| `variant` | 该 Workflow 的内容版本（修订号） |
| `valid` | `false` = 已被 `workflow.delete` 逻辑删除（重启后消失） |
| `stable_id` | 本次 Mapping 下的紧凑编号（§7.2，会整体重排，**不得长期引用**） |
| `count` | **占用 Slot 数**，含 `valid=false` 的对象 |

> ⚠️ **`count` ≠ 可执行 Workflow 数量。** 它包含 `valid=false` 的
> 逻辑删除对象（它们仍占用 Slot，直到重启才真正消失）。
> 要统计有效数量请自行过滤 `workflows[].valid == true`。

**variant 行为**：无。
**dirty 行为**：只读。
**注意**：**不在 list 中返回 Steps**。要完整内容必须 `workflow.get`。
**注意**：列表按 **Slot 升序**输出，与 `stable_id` 顺序无关。

---

### 9.3 `workflow.get` —— 拉取单个完整 Workflow

**请求**

```json
cm {"cmd":"workflow.get","id":"9012","p":{"id":3}}
cm {"cmd":"workflow.get","id":"9013","p":{"stable_id":0}}
```

| 参数 | 说明 |
|---|---|
| **`p.id`** | **Slot 编号（整数 0..15）。首选、也是唯一的精确定位方式** |
| `p.stable_id` | Registry 映射入口，设备解析回 Slot（§7）。重复 `workflow.id` 时可能不唯一 |

> `workflow.id` **不是**合法的定位条件（允许重复），设备已移除该能力。
> 用业务 ID 作为 `p.id` 会得到 **error 6**（不是整数）。

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
    "slot": 3,
    "command": "workflow.get"
}
```

| 字段 | 说明 |
|---|---|
| `o` | 完整 Workflow 对象（逻辑名为 `workflow`），**可直接回传 `workflow.set`** |
| `k` / `slot` / `valid` | **Response metadata**，不在 `o` 里，回传时不要带回去 |
| `k` | 该对象的 `stable_id`（紧凑键名） |
| `slot` | 该对象的 Slot（= `p.id`） |
| `valid` | `false` = 已逻辑删除 |

**variant 行为**：无。
**dirty 行为**：只读。

### 9.3.1 `valid=false` 时的行为（★）

```text
当前 RAM 中 valid=false（刚 delete，未重启）
    → workflow.get 成功返回，顶层带 "valid": false
    → o 中是该 Slot 当前真实内容（Steps 通常已被清空）

设备 reboot 之后
    → 该 Slot 不再作为有效 Workflow 加载
    → workflow.get 返回 error 5（WORKFLOW_NOT_FOUND）
```

**错误**：`5`（Slot 不存在 / 未占用）、`6`（`p.id` 不是整数）。

> `o` 内部**不含** `valid`。判断是否已删除请看顶层 `valid`。
> 回传 `workflow.set` 时只取 `o` 的内容，**不要**把 `k/slot/valid/dirty`
> 等 metadata 混进 Workflow JSON。

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
| `variant` | 置 **1**（即使已存在内容完全相同的另一个 Workflow，也是 1） |
| `valid` | `true` |
| `dirty` | `true` |
| Slot 选择 | 优先未使用 Slot；无空位时**回收已 `delete` 的 Slot**。**Slot 选择是设备内部实现，Cloud 不得依赖分配顺序**，只用返回的 `slot` 做后续定位 |
| 立即可见 | ✅ `list` / `get` 立刻可见（无需重启） |
| Registry | 立即 rescan → `registry_version++` |

**重复 `workflow.id` 完全合法（§3 / §4.1）**

```text
Slot 3: id = "test"      ✅
Slot 7: id = "test"      ✅   不报错
Slot 3 与 Slot 7 内容完全一致   ✅   也允许
```

**错误**：`6`（缺 `workflow` / 缺 `id`）、`11`（`type` 缺失或非法 / `steps>16` / `params>8`）、
`12`（16 个 Slot 全满且无可回收）、`13`（运行中 / Critical 获取失败）。

---

### 9.5 `workflow.set` —— 整体替换

**请求**

```json
cm {"cmd":"workflow.set","id":"9016","p":{
  "id":3,
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
| **`p.id`** | **Slot 编号（整数）。唯一定位方式，必填** |
| `p.stable_id` | 备选：Registry 映射入口（会解析回 Slot） |
| `p.workflow` | **必填**。**完整**新内容（**不支持 PATCH**，也不支持平铺到 `p` 上） |

> **只改 `p.id` 指定的那一个 Slot。** 设备**不会**根据 `workflow.id`
> 去搜索对象 —— 即使 Slot 7 也叫 `"morning_water"`，它也**不会**被改动。
>
> **缺少 `p.workflow` 时返回错误码 `6`，不会清空目标对象。**
> 这是刻意的保护：避免"只想定位"的半截请求被当成整体替换执行。

**`workflow.id` 可以自由改名，也可以改得和别人重复（§6）**

```text
p.id = 3, workflow.id = "another_name"   ✅ 允许
p.id = 3, workflow.id = "morning_water"（与 Slot 7 同名）  ✅ 允许
```

设备**不做** `p.id == workflow.id` 之类的校验 —— 二者没有唯一性关系。

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
- `steps > 16`：**整体拒绝**（错误码 `11`），不截断。
- 单个 Step 的 `params > 8`：**整体拒绝**（错误码 `11`），不截断。
- Step 的 `type` 缺失或非法：**整体拒绝**（错误码 `11`），不默认成 `action`。
- 校验在**修改之前**执行：被拒绝的请求不会破坏目标 Workflow 的现有内容。

**错误**：`5`（Slot 不存在）、`6`（缺 `workflow` / `p.id` 非整数）、
`11`（内容非法：`type` 缺失 / `steps>16` / `params>8`）、
`13`（运行中 / Critical 获取失败）。

---

### 9.6 `workflow.delete` —— 逻辑删除

**请求**

```json
cm {"cmd":"workflow.delete","id":"9018","p":{"id":3}}
cm {"cmd":"workflow.delete","id":"9019","p":{"stable_id":0}}
```

**返回（第一次删除）**

```json
{
    "s": 0, "c": "result", "i": "9018", "t": 1789151305,
    "slot": 3, "variant": 4, "deleted": true, "noop": false, "dirty": true,
    "command": "workflow.delete"
}
```

**返回（重复删除，幂等 no-op）**

```json
{
    "s": 0, "c": "result", "i": "9020", "t": 1789151311,
    "slot": 3, "variant": 4, "deleted": false, "noop": true, "dirty": false,
    "command": "workflow.delete"
}
```

**状态机（§8）**

```text
delete(valid=true)
    ↓
valid   = false
variant = variant + 1

delete(valid=false)        ← 重复删除
    ↓
success / no-op
  · 不再次 variant++
  · 不产生新的 Dirty
  · 不触发 Registry 变化
  · 不触发额外保存
```

**其它行为**

- **不物理删除** Step BIN，也不删除文件；只把 `meta.valid` 置 0 并**立即写盘**。
- 不会终止正在运行的 Runtime。
- 删除后到重启前的窗口内，`workflow.list` **仍然能看到它**，
  标记为 `valid=false` —— 这是**设计行为**，让 Cloud 明确得知
  "对象被删了"，而不是"它凭空消失"。
- 重启后该 Slot 不再作为有效 Workflow 加载，此时再 `delete` 返回 **error 5**。
- 无需重启即可被新 `create` 回收（`workflow_find_free_slot` 第二轮）。

**variant 行为**：`+1`（仅第一次）；重复删除不变。
**错误**：`5`（Slot 不存在 / 未占用）。

> **与需求 §8.1 的一处差异（已知，未改）**：需求文字描述 delete 后
> "进入 Dirty 状态"，而当前实现是**立即落盘并清掉本对象 Dirty**。
> 原因：`workflow_delete()` 若保留 Dirty，后续 `save_transaction()`
> 会把该对象重新写回并把 `meta.valid` 置回 `true` —— 等于删除被撤销。
> 这是已验证稳定的既有设计，本次按 §3「不修改已验证稳定的
> Dirty/Critical 基础架构」保持不变。对 Cloud 的影响：
> delete 之后**不需要**再调 `save` 来持久化删除（调了也是 no-op）。

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

`workflow.save` 在**逻辑上是全局**操作（一次调用处理所有 Dirty），
但**物理层是逐 Workflow 独立事务** —— 不是所有 Workflow 的一个
真正的全局 Flash 原子事务：

```text
workflow.save
     ↓
遍历【当前全部】Dirty Workflow
     ↓
逐个执行自己的持久化事务
     ↓
成功的 → 立即清掉【它自己】的 Dirty
失败的 → 保留【它自己】的 Dirty
     ↓
只要存在任意失败 → 整个 workflow.save 命令返回失败
```

因此下面这种结果**是允许且正常的**：

```text
Workflow A save 成功 → dirty=false
Workflow B save 失败 → dirty=true
Workflow C save 成功 → dirty=false

workflow.save 整体返回 → 失败（错误码 10）
```

> ⚠️ 接口语义**不是**"任何一个失败时所有 Workflow 都保持 Dirty"。
> 成功的对象已经落盘并清掉了自己的 Dirty；只有失败的还留着。
> 重试 `workflow.save` 只会重新尝试剩余的那几个。

**Critical 释放规则**

```text
全部 Dirty 清空  → 释放 Workflow Edit Critical，允许安全重启
仍有 Dirty 残留  → 【不】释放，系统继续阻止重启
```

**decision 表**

| 场景 | `saved` | `dirty` | `restarting` | 重启 |
|---|---|---|---|---|
| 有 Dirty，全部成功，`restart:true` | `true` | `false` | `true` | 是（随后断开重连） |
| 有 Dirty，全部成功，`restart:false` | `true` | `false` | `false` | 否 |
| 部分失败 | 错误码 `10` | `true`（失败对象保留） | — | 否 |
| **无 Dirty**，`restart:true` | `true` | `false` | **`false`** | **否（不空转重启）** |

**`saved=true` 的含义**：Save operation 成功完成。
它**不代表**本次一定发生了 Flash 写入 —— 无 Dirty 时不会有写入。

**`restarting=true` 的含义**：重启请求**已被设备接受 / 安排**。
它**不代表**设备已经完成 reboot。Cloud 应预期：

```text
command response
     ↓
MQTT disconnect
     ↓
device reconnect
     ↓
workflow.sync_info
     ↓
必要时 list / get
```

设备**不会**主动发送 "I have rebooted"，也不需要额外的
Cloud Workflow State Machine。

**variant 行为**：无（只落盘）。
**错误**：`10`（保存失败，失败对象的 Dirty 已保留）。

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
| `5` | **Workflow not found** | `p.id` 越界 / Slot 未占用（含重启后已删对象）；`stable_id` 找不到 |
| `6` | **Invalid param / missing workflow** | payload 非对象、缺 `workflow`、缺 `id`、**`p.id` 不是整数 Slot** |
| `7` | Queue full | 命令运行时队列满 |
| `8` | Duplicate cmd_id | `i` / `id` 重复 |
| `9` | System | 系统级错误 |
| `10` | Execution | `workflow.save` 保存失败（Dirty 已保留） |
| `11` | **INVALID_PAYLOAD** | payload 不是合法 JSON；**Step 缺 `type`** / `type` 非法；**`steps > 16`**；**`params > 8`** |
| `12` | **No free slot** | Workflow 槽位满（16）且无可回收 |
| `13` | **Rejected** | 运行中 / Critical 获取失败 |

> §18/§19 提到的 "INVALID_PAYLOAD" 在设备上就是错误码 **11**
> （`CMD_ERROR_INVALID_PAYLOAD`）。需求文字里出现的 "error 6" 是
> 早期草案的写法，以本表为准。

错误上行示例（实测）

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
       ↓ 逐条比对 (slot, variant, valid)
       ↓ 与服务端 Cache 一致 → 跳过
       ↓ 不一致 → 记入待拉取集合
3. 对每个变化的 Workflow: workflow.get  {"id": <slot>}
       ↓
4. 用返回的 o 覆盖服务端缓存；用顶层 valid 决定"有效/已删"
       ↓
5. 重建 slot ↔ 缓存 映射；registry_version 变了则重建 stable_id 映射
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

### 13.3 必须遵守的约定

1. **用 `slot`（= `p.id`）作为 Workflow 的唯一主键**。
   `workflow.id` 允许重复，**不能**当主键；`stable_id` 会整体重排。
2. **显式写 `enable`**（§5.3 两条路径缺省值不同）。
3. **`step.id` 用 §12 清单校验**（写错不会报错，但不会执行）。
4. **`type` 必须显式给出**，`steps` ≤ 16，`params` ≤ 8（否则 error 11）。
5. **判断"已删除"看顶层 `valid`**，不是看对象是否存在（§9.6）。
6. **不要把 metadata 混进 Workflow JSON**：
   `variant` / `slot` / `stable_id` / `dirty` / `registry_version` / `valid`
   都是**只读**的 Response metadata，回传 `workflow.set` 时只取 `o`（或
   `workflow`）的内容。

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
| 每 Workflow Step 数 | **16** | 超出**整体拒绝**（错误码 `11`），**不截断** |
| 每 Step 参数个数 | **8** | 超出**整体拒绝**（错误码 `11`），**不丢弃** |
| Step `type` | 必填 | 缺失 / 非法 → **整体拒绝**（错误码 `11`），不默认 |
| `variant` | uint32 | 溢出回绕到 1（不归 0） |
| `id`（workflow.id） | 非空字符串 | 空 → 错误码 6。**允许重复** |
| `p.id`（Slot） | 整数 `0..15` | 非整数 → 错误码 6；未占用 → 错误码 5 |
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
python test/serial_batch.py <COM口> <日志文件> <用例文件> [单条等待秒]
```

用例文件格式（`|||` 后为期望子串）：

```text
cm {"cmd":"workflow.list","id":"t1"} ||| "valid"
cm {"cmd":"workflow.get","id":"t2","p":{"id":3}} ||| "valid":true
cm {"cmd":"workflow.save","id":"t3","p":{"restart":false}} ||| "saved":true
```

已落地用例（在受跟踪的 `test/` 目录下；`.pio/` 被 gitignore）：

| 文件 | 覆盖 |
|---|---|
| `test/wf_sync_tests.txt` | §33–§37：基础 / 创建 / 修改 / 幂等 / 删除 / 多对象 save / **保存失败** / 错误码 |
| `test/wf_reboot_tests.txt` | §35：重启后 variant 保持、删除不复活 |
| `test/wf_final_tests.txt` | 导出导入往返 / 参数类型 / 槽位回收 / registry 联动 / 错误码 |
| `test/wf_reboot_final_tests.txt` | §35：variant 跨重启、已删对象不复活 |
| **`test/wf_contract_tests.txt`** | **Cloud Contract 修订 Test 1–20**（重复 id / 完全相同 Workflow / p.id 精确定位 / valid=false get / delete 幂等 / count 含 invalid / stable_id 重排 / type 缺失 / >8 params / >16 steps / JSON 字段顺序 / save 部分失败 / no-dirty save / save+restart） |
| **`test/wf_contract_reboot_tests.txt`** | **Test 6 / Test 20**：reboot 后 get → error 5、完整重新同步 |

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
| `workflow.list` | — | `workflows[{slot,id,variant,valid,stable_id}]` `count` | — | 只读 |
| `workflow.get` | **`id`(Slot)** \| `stable_id` | `o{...}` `slot` `k` `valid` | — | 只读 |
| `workflow.create` | `workflow{...}` | `slot` `stable_id` `variant` | `=1` | `true` |
| `workflow.set` | **`id`(Slot)**+`workflow{...}` | `slot` `variant` `changed` | `+1`／不变 | 变化时为 `true` |
| `workflow.delete` | **`id`(Slot)** \| `stable_id` | `slot` `variant` `deleted` `noop` | `+1`／不变（幂等） | 视实现 |
| `workflow.save` | `restart`(缺省 true) | `saved` `dirty` `restarting` `registry_version` | — | 各对象自己清 |

> 所有定位参数中的 `id` 都是 **Slot 整数**，不是 `workflow.id`。

## 附录 B：相关文档

| 文档 | 内容 |
|---|---|
| `readme.md` | 项目总纲、四层架构 |
| `workflow_cloud_sync_progress.md` | 本任务进度与测试证据 |
| `workflow_storage改造进度.md` | BIN 存储层实现 |
| `cloud_protocol.md` | 云协议总览 |
| `critical_operation接入规范.md` | Safe Restart 与 Critical 接入 |
