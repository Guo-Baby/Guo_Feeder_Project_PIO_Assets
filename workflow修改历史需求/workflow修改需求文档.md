# Workflow 云端同步与 CommandManager 后续完善需求文档

**项目：Guo Feeder / 郭氏自动猫粮机**
**任务：Workflow 云端同步架构完善、PSRAM 优化、接口文档与项目文档同步**
**目标平台：ESP32-S3 N16R8**
**任务原则：保持现有 Workflow / Storage / Capability Registry 架构，不做无必要的大范围重构。**

---

# 一、任务目标

在当前已经完成的 Workflow Variant + CommandManager `workflow.*` 基础上，继续完成以下工作：

1. 完善 Workflow 云端同步协议。
2. 明确 `Workflow Variant` 与 `Capability Registry Version` 的职责关系。
3. 增加 Workflow 同步信息接口 `workflow.sync_info`。
4. 完善 `workflow.list`，增加 `valid` 字段。
5. 确保 `workflow.save` 一次性保存所有 Dirty Workflow，并正确清理全部 Dirty 状态。
6. 优化大对象内存分配：优先使用 PSRAM，避免不必要地占用内部 DRAM。
7. 不要求将 JSON 长期存储在 PSRAM；JSON 仅作为 Cloud/CommandManager 通信格式。
8. 输出一份完整的 **Workflow 云端适配接口/架构文档**，供未来 Web/App/UI 开发使用。
9. 更新 `README.md` 中的 Workflow 架构说明。
10. 建立可恢复的任务进度文档，保证任务中断后可以继续。
11. 所有代码修改必须具有 Git 可回溯性。
12. 完成脚本化回归测试；本次不要求 MQTT 实机测试。

---

# 二、最重要的架构原则

## 2.1 Workflow 是设备侧的业务真源

Workflow 的真实数据结构和持久化格式属于设备内部实现。

Cloud 不应该理解：

* Step BIN
* Meta BIN
* WorkflowStorage 内部事务
* LittleFS
* BIN CRC
* staged transaction
* `.tmp`
* `.bak`
* storage transaction ID

Cloud 只应该理解 Workflow 的**逻辑 JSON 数据模型**和同步元数据。

数据流必须保持：

```text
Cloud
  ↓ JSON
CloudManager
  ↓
CommandManager
  ↓
Workflow.cpp
  ↓
Workflow Definition / Runtime
  ↓
WorkflowStorage
  ↓
Meta + Step BIN
  ↓
LittleFS
```

反向：

```text
LittleFS
  ↓
WorkflowStorage
  ↓
Workflow.cpp
  ↓
Workflow Definition
  ↓ JSON
CommandManager
  ↓
CloudManager
  ↓
Cloud
```

**Workflow.cpp 是 JSON 与设备内部 Workflow 数据结构之间的语义转换边界。**

CommandManager 不允许直接操作 WorkflowStorage / BinStorage / FileStorage。

---

# 三、Variant 设计

## 3.1 Workflow Variant

每一个 Workflow Slot 独立拥有自己的：

```cpp
uint32_t variant;
```

即：

```text
Workflow #0 → variant
Workflow #1 → variant
Workflow #2 → variant
...
Workflow #15 → variant
```

Variant 是 **32 位无符号整数**。

32 位范围已经远远足够，不需要 64 位。

---

## 3.2 Variant 自增规则

创建：

```text
create → variant = 1
```

成功修改：

```text
set → variant++
```

逻辑删除：

```text
delete → variant++
```

没有发生实际内容变化：

```text
variant 不增加
```

例如：

```text
create:
variant = 1

set:
variant = 2

set:
variant = 3

delete:
variant = 4
```

重启后 Variant 必须从 Meta BIN 正确恢复。

**绝对禁止出现重启后 Variant 回退。**

---

# 四、Capability Registry Version 与 Workflow Variant 的关系

必须保持以下概念严格分离。

## 4.1 Workflow Variant

表示：

> 某一个 Workflow 对象自身发生过多少次逻辑版本变化。

例如：

```text
WF_A variant=5
WF_B variant=2
WF_C variant=11
```

---

## 4.2 Capability Registry Workflow Version

Capability Registry 的 Workflow Registry Version 表示：

> 当前 Workflow Mapping 整体发生变化的版本。

例如：

```text
Registry Version = 20
```

它不是某一个 Workflow 的 Variant。

---

## 4.3 两者关系

Capability Registry 中：

```cpp
CapabilityMapping.object_version
```

对于 Workflow 必须保存：

```cpp
object_version = workflow.variant;
```

因此：

```text
Capability Registry
        |
        +-- stable_id
        +-- runtime_id
        +-- object_version
                         |
                         └── Workflow.variant
```

Registry Version：

```text
整个 Mapping 的版本
```

Workflow Variant：

```text
单个 Workflow 的版本
```

两者不能混淆。

---

# 五、Workflow List 必须增加 valid

当前：

```json
{
    "stable_id": 0,
    "id": "WF1",
    "variant": 5
}
```

修改为至少：

```json
{
    "stable_id": 0,
    "id": "WF1",
    "variant": 5,
    "valid": true
}
```

如果设备 RAM 中暂时仍保留已经执行 delete 的 Workflow 对象，则：

```json
{
    "stable_id": 0,
    "id": "WF1",
    "variant": 6,
    "valid": false
}
```

## 5.1 删除语义

`workflow.delete` 不执行物理删除。

只改变：

```text
valid = false
variant++
dirty = true
```

Step BIN 可以继续保留。

重启后：

```text
valid=false
```

的 Workflow 不应该被重新作为有效 Workflow 加载。

---

# 六、云端同步机制

云端应该把设备 Workflow 看成一个**可同步的设备状态缓存**。

同步流程：

```text
Cloud
  |
  | workflow.list
  ↓
ESP32
  |
  | registry_version
  | registry_checksum
  | workflow[]
  ↓
Cloud
```

Cloud 首先比较 Registry 信息以及每个 Workflow：

```text
stable_id
id
variant
valid
```

如果对应 Workflow 的 Variant 与 Cloud Cache 一致：

```text
不需要 workflow.get
```

如果 Variant 不一致：

```text
workflow.get
```

拉取完整 JSON。

---

# 七、设备重启后的同步

设备不会依赖主动通知 Cloud：

```text
ESP32 reboot
```

而应该由 Cloud 自己负责在检测到设备重新在线后执行：

```text
workflow.list
```

然后重新同步。

因此：

```text
Device reboot
    ↓
Device online
    ↓
Cloud workflow.list
    ↓
比较 Variant / valid / Registry 信息
    ↓
只 GET 有变化的 Workflow
```

这是 Cloud 端同步机制的重要组成部分。

设备端不需要为了 Workflow 同步增加额外的“启动后主动上传全部 Workflow”机制。

---

# 八、增加 workflow.sync_info

增加：

```text
workflow.sync_info
```

用于快速获得 Workflow 同步所需要的整体信息。

建议至少返回：

```json
{
    "status": "success",
    "registry_version": 20,
    "registry_checksum": 123456789,
    "count": 4,
    "dirty": false
}
```

如果当前架构已经可以通过 `workflow.list` 完整满足同步需求，则 `sync_info` 可以作为更轻量的同步入口。

原则：

```text
sync_info
    ↓
判断是否需要进一步同步

list
    ↓
比较每个 Workflow

get
    ↓
获取变化的 Workflow 完整 JSON
```

不要让 `sync_info` 返回完整 Workflow。

---

# 九、workflow.list 的职责

`workflow.list` 是云端同步入口。

它只提供摘要，不返回完整 Steps。

至少包含：

```json
{
    "stable_id": 0,
    "id": "WF1",
    "variant": 5,
    "valid": true
}
```

整体返回：

```json
{
    "status": "success",
    "registry_version": 20,
    "registry_checksum": 123456789,
    "count": 4,
    "dirty": false,
    "workflows": [
        {
            "stable_id": 0,
            "id": "WF1",
            "variant": 5,
            "valid": true
        }
    ]
}
```

不要在 list 中返回完整 Workflow。

---

# 十、workflow.get

用于获取单个 Workflow 完整逻辑数据。

支持：

```json
{
    "stable_id": 0
}
```

或者：

```json
{
    "id": "WF1"
}
```

返回：

```json
{
    "status": "success",
    "stable_id": 0,
    "workflow": {
        "id": "WF1",
        "variant": 5,
        "enable": true,
        "timeout_ms": 0,
        "steps": [
            ...
        ]
    }
}
```

JSON 格式原则上继续沿用当前 `workflow.json` 的格式。

只增加必要的：

```text
variant
```

不要重新设计整个 Workflow JSON Schema。

---

# 十一、Workflow JSON 必须保持兼容

当前 Workflow JSON 结构类似：

```json
{
    "id": "...",
    "name": "...",
    "enable": true,
    "timeout_ms": 0,
    "steps": [
        ...
    ]
}
```

本次继续沿用。

增加：

```json
"variant": 5
```

最终类似：

```json
{
    "id": "WF1",
    "name": "Example",
    "variant": 5,
    "enable": true,
    "timeout_ms": 0,
    "steps": [...]
}
```

除必要新增字段外，不要无意义修改已有字段名称和结构。

---

# 十二、workflow.create

创建 Workflow：

```text
variant = 1
```

成功后：

```text
valid = true
dirty = true
```

设备自动选择空闲 Slot 或按照当前已有 CommandManager 约定处理 Slot。

创建成功后必须立即能够被：

```text
workflow.list
workflow.get
```

看到。

不能出现：

```text
create 成功
但 workflow_count 没有更新
导致重启前无法查询
```

这一类问题。

---

# 十三、workflow.set

`workflow.set` 使用**完整 Workflow 替换模型**。

不实现 PATCH。

Cloud 发送：

```text
完整 Workflow JSON
```

设备：

```text
JSON
 ↓
Workflow.cpp
 ↓
验证
 ↓
更新 Definition
 ↓
variant++
 ↓
Dirty
```

成功后：

```text
variant = old_variant + 1
```

如果提交的内容与当前内容完全相同：

```text
不得无意义增加 variant
```

即：

```text
真正发生逻辑变化 → variant++

没有发生变化 → variant 不变
```

---

# 十四、workflow.delete

删除：

```text
valid=false
variant++
dirty=true
```

不物理删除：

```text
Step BIN
```

也不要求立即删除旧文件。

重启后：

```text
valid=false
```

的 Workflow 不应该重新作为有效 Workflow 出现。

---

# 十五、workflow.save —— 本次重点完善

`workflow.save` 是**全局 Workflow Dirty 保存操作**。

它不是保存某一个 Workflow。

执行：

```text
workflow.save
```

必须保存当前所有：

```text
Dirty Workflow
```

包括：

```text
Workflow #0 dirty
Workflow #3 dirty
Workflow #7 dirty
...
```

必须在一次完整 Save Transaction 中处理。

---

## 15.1 Save 完成条件

只有：

```text
所有 Dirty Workflow
```

均成功持久化之后，才允许：

```text
clear dirty
release Workflow Edit Critical
```

不能出现：

```text
Workflow A 保存成功
Workflow B 保存失败
↓
直接 dirty=false
```

这是禁止的。

正确：

```text
A save OK
B save OK
C save OK
↓
全部成功
↓
dirty bitmap 清零
↓
Critical release
```

如果任何一个 Workflow 保存失败：

```text
Dirty 必须保留
Critical 必须保留
```

以便之后继续保存。

---

# 十六、workflow.save 与 Restart

默认：

```json
{
    "restart": true
}
```

执行：

```text
save all dirty workflows
    ↓
确认全部成功
    ↓
clear dirty
    ↓
release Workflow Edit Critical
    ↓
请求 SystemCommand Restart
```

如果：

```json
{
    "restart": false
}
```

则：

```text
保存
↓
clear dirty
↓
不重启
```

---

## 16.1 Dirty=false 时

如果当前：

```text
dirty == false
```

执行：

```text
workflow.save
```

不得无意义触发重启。

即：

```text
无 Dirty
    ↓
saved=true
dirty=false
restarting=false
```

---

# 十七、PSRAM 内存策略

ESP32-S3 N16R8 有约 8 MB PSRAM。

本次必须继续优化大型 Workflow / Capability Registry 临时对象。

原则：

### 大对象

优先：

```cpp
heap_caps_malloc(..., MALLOC_CAP_SPIRAM)
```

失败后：

```cpp
heap_caps_malloc(..., MALLOC_CAP_8BIT)
```

作为 DRAM fallback。

---

# 十八、不要把 JSON 当成 PSRAM 数据库

这是非常重要的原则。

Workflow 的 JSON 是：

```text
Cloud / CommandManager ↔ Workflow.cpp
```

的通信格式。

不是内部长期缓存格式。

不要为了使用 PSRAM 而：

```text
Workflow → JSON String → PSRAM
```

长期保存。

也不要：

```text
Capability Registry → JSON → PSRAM
```

---

# 十九、Workflow 内部数据应保持结构化

Workflow 已经拥有自己的：

```text
Workflow
WorkflowDefinition
Step Definition
Runtime
Meta
```

等结构。

这些结构已经是设备内部的数据模型。

如果需要扩大容量：

```text
大数组 / 大型 Definition / 临时 Mapping
        ↓
PSRAM
```

而：

```text
高频访问的小型状态
计数
标志
指针
少量 Meta
```

继续留在内部 DRAM。

---

# 二十、Capability Registry PSRAM

Capability Registry 的大型 Mapping / 临时工作区优先使用 PSRAM。

特别是：

```text
CapabilityMapping[]
CapabilityRegistryTable
rescan 临时缓冲区
```

等对象。

禁止为了简单而把大型对象重新放回 loopTask 栈。

同时保留：

```text
DRAM fallback
```

防止 PSRAM 分配失败导致功能不可用。

---

# 二十一、栈安全要求

不得把以下大型对象直接放到函数栈：

```text
CapabilityRegistryTable
CapabilityMapping[32]
大型 WorkflowDefinition
大型 JsonDocument
```

如果确实需要动态创建：

```text
PSRAM → DRAM fallback
```

并正确处理：

```text
String constructor
String destructor
```

不能直接对含有 `String` 的结构进行错误的：

```cpp
memcpy
```

或未经构造的裸内存使用。

---

# 二十二、Capability Registry 的职责保持不变

Capability Registry 仍然只是：

```text
Stable ID Mapping Layer
```

不要把 Workflow 业务逻辑迁移进去。

它只读取 Workflow 暴露的公开查询接口。

Workflow：

```text
负责 Workflow 业务
```

Capability Registry：

```text
负责 Stable ID
```

CommandManager：

```text
负责命令路由
```

CloudManager：

```text
负责 MQTT / Cloud Protocol
```

不得混层。

---

# 二十三、Workflow 运行机制必须写入云端适配文档

必须新增一份独立文档：

```text
docs/workflow_cloud_interface.md
```

或项目现有文档目录中合理的位置。

该文档必须向未来 Web/App/Cloud 开发人员解释：

## 23.1 Workflow 是什么

Workflow：

```text
Workflow
  └── Step[]
        ├── Trigger
        └── Action
```

Step 0 为 Trigger。

Step 1+ 可以是 Trigger / Action。

---

## 23.2 Workflow Definition 与 Runtime 的关系

必须说明：

```text
Definition
    ↓ start
Runtime Snapshot
```

Workflow 运行时使用 Runtime Snapshot。

因此：

```text
Cloud 修改 Definition
```

不会直接改变：

```text
已经正在执行的 Runtime
```

运行中的 Workflow 应继续按照启动时的 Runtime Snapshot 执行。

---

## 23.3 Workflow 修改限制

需要说明：

* 某些结构修改在 Workflow RUNNING 时被拒绝。
* 参数修改存在 Definition / Runtime 隔离。
* Workflow 删除不会强制杀死当前 Runtime。
* Storage Dirty / Critical 属于设备内部机制。
* Cloud 不需要理解这些内部实现细节。

---

# 二十四、云端开发人员必须理解 Stable ID

云端不能假设：

```text
stable_id 永久等于某个固定数字
```

Stable ID 是 Capability Registry 根据 Runtime ID Mapping 产生的。

云端同步时应该优先保存：

```text
stable_id
id
variant
valid
```

如果 Registry Version 变化：

```text
重新获取 list
```

必要时重新解析 Stable ID。

---

# 二十五、云端同步不能只依赖 Stable ID

Stable ID 是快速定位工具。

真正的 Workflow 对象身份仍然应该结合：

```text
id
variant
valid
```

以及 Registry 信息。

如果：

```text
registry_version
```

发生变化：

Cloud 应重新同步 Registry / Workflow list。

---

# 二十六、未来 UI 开发注意事项

未来 Web/App UI 不应该直接操作：

```text
Step BIN
Meta BIN
LittleFS
WorkflowStorage
```

UI 应该只操作：

```text
workflow.list
workflow.get
workflow.create
workflow.set
workflow.delete
workflow.save
workflow.sync_info
```

UI 编辑 Workflow 时：

```text
Cloud UI Model
      ↓
Workflow JSON
      ↓
workflow.set
      ↓
ESP32
```

读取：

```text
ESP32
      ↓
workflow.get
      ↓
Workflow JSON
      ↓
Cloud UI Model
```

---

# 二十七、必须记录完整命令接口

云端适配文档必须记录至少：

```text
workflow.sync_info
workflow.list
workflow.get
workflow.create
workflow.set
workflow.delete
workflow.save
```

每个命令说明：

```text
请求格式
参数
返回格式
错误
variant 行为
dirty 行为
```

---

# 二十八、错误处理

继续使用现有 CommandManager 错误码体系。

至少保留：

```text
1  Unknown command
5  Workflow not found
6  Invalid payload / missing workflow
11 Invalid JSON payload
12 No free slot
13 Rejected
```

不要为了本次任务重新设计整个错误码体系。

---

# 二十九、Git 强制要求

这是本次任务的硬性要求。

## 修改前

必须执行：

```bash
git status
git log -n 10 --oneline
```

确认当前工作区状态。

如果存在用户未提交修改：

```text
不得覆盖
不得 reset
不得 stash 用户工作
不得 git add .
```

---

## 每个逻辑阶段必须 Commit

至少按照以下阶段建立 Git commit：

### Commit 1

Workflow 同步协议 / valid / sync_info 完善。

### Commit 2

workflow.save 全 Dirty 保存机制完善。

### Commit 3

PSRAM / 栈安全优化。

### Commit 4

Workflow 云端适配文档。

### Commit 5

README 更新。

如果实际修改量适合合并，可以合理调整，但必须保证：

```text
每个逻辑阶段均可独立回退。
```

---

# 三十、任务进度文档

必须维护：

```text
docs/workflow_cloud_sync_progress.md
```

每完成一个阶段立即更新。

不得等整个任务完成后才写。

---

## 进度文档至少包含

```text
当前任务
当前阶段
已完成内容
当前 Commit
测试结果
已发现问题
已解决问题
未完成事项
下一步
```

示例：

```text
# Workflow Cloud Sync Progress

## Current Phase
Phase 3 - PSRAM Optimization

## Completed
- valid added to workflow.list
- workflow.sync_info added
- workflow.save all-dirty verified

## Current Commit
xxxxxxx

## Tests
16/16 PASS

## Known Issues
...

## Next
Update workflow_cloud_interface.md
```

这样如果 Codex 会话中断，下一次可以首先读取：

```text
docs/workflow_cloud_sync_progress.md
```

然后继续执行。

---

# 三十一、README.md 修改要求

必须更新：

```text
README.md
```

但 README 不需要成为完整技术文档。

只需要说明当前 Workflow 架构：

```text
Workflow
├── Definition
├── Runtime
├── WorkflowStorage
├── Variant
├── Capability Registry
└── CommandManager / Cloud Sync
```

并说明：

```text
Cloud 使用 JSON
Device 内部使用 BIN
Workflow Variant 用于对象版本同步
Capability Registry 提供 Stable ID
```

详细协议放在：

```text
workflow_cloud_interface.md
```

避免 README 过度膨胀。

---

# 三十二、测试要求

本次必须完成脚本化测试。

不要求 MQTT 实机测试。

但必须通过：

```text
command_manager_execute()
```

的实际完整命令链测试。

也就是说：

```text
模拟 Cloud JSON
    ↓
CommandManager
    ↓
Workflow
    ↓
Storage
```

不能只测试底层函数。

---

# 三十三、必须测试的核心场景

至少测试：

### 基础

```text
workflow.sync_info
workflow.list
workflow.get
```

### 创建

```text
create
list
get
```

### 修改

```text
set
variant++
get
```

### 无变化

```text
set identical data
variant 不应无意义增加
```

### 删除

```text
delete
variant++
valid=false
list 可见
```

### Save

至少：

```text
修改 Workflow A
修改 Workflow B
修改 Workflow C

workflow.save
```

验证：

```text
A persisted
B persisted
C persisted
dirty=false
```

---

# 三十四、Save 失败测试

如果已有测试注入机制，应增加：

```text
A save success
B save failure
```

验证：

```text
dirty != false
```

即：

```text
不能因为部分成功就清空整个 Dirty 状态。
```

随后重新执行：

```text
workflow.save
```

应能够完成剩余保存。

---

# 三十五、重启持久化测试

至少验证：

```text
variant=2
save
reboot
list
```

必须仍然：

```text
variant=2
```

同时：

```text
delete
save
reboot
list
```

被删除 Workflow 不得重新出现。

---

# 三十六、Capability Registry 测试

验证：

```text
Workflow Variant
```

发生变化后：

```text
CapabilityMapping.object_version
```

正确反映新的 Variant。

同时：

```text
Registry Version
```

与：

```text
Workflow Variant
```

不能混淆。

---

# 三十七、PSRAM 测试

启动时确认：

```text
PSRAM size
PSRAM free
```

并对大型 Registry / Workflow 缓冲区打印或验证：

```text
PSRAM allocation success
```

如果 PSRAM 不可用，应验证 DRAM fallback 不会崩溃。

---

# 三十八、最终交付物

本次任务最终必须得到：

```text
1. Workflow.cpp / 相关代码完善
2. CommandManager.cpp / 相关代码完善
3. Capability Registry 必要修改
4. WorkflowStorage 必要修改
5. README.md 更新
6. Workflow 云端适配文档
7. Workflow 修改接口说明
8. workflow_cloud_sync_progress.md
9. Git commits
10. 测试报告
```

---

# 三十九、最终报告必须回答的问题

完成后必须明确报告：

1. `workflow.list` 是否包含 `valid`？
2. `workflow.sync_info` 是否完成？
3. Variant 是否为 32 位？
4. 每个 Workflow 是否独立维护 Variant？
5. Workflow Variant 与 Registry Version 是否严格分离？
6. `workflow.save` 是否保存全部 Dirty Workflow？
7. Save 部分失败时 Dirty 是否保留？
8. Save 成功后 Dirty Bitmap 是否完整清零？
9. Restart 是否只在 Save 全部成功后触发？
10. 大型 Registry / Workflow 缓冲区是否优先使用 PSRAM？
11. 是否避免把 JSON 作为长期内部缓存？
12. Workflow JSON 是否保持现有格式并仅增加必要字段？
13. Workflow 云端适配文档是否完成？
14. README 是否更新？
15. Progress 文档是否完整？
16. 所有修改是否有 Git commit？
17. 回归测试是否全部通过？
18. 是否存在尚未验证的边界条件？

---

# 四十、禁止事项

本次任务禁止：

* 重写已经验证的 WorkflowStorage 事务架构。
* 改变 Workflow BIN 格式，除非本任务确实必要。
* 把 Cloud 协议逻辑塞进 WorkflowStorage。
* 把 MQTT 逻辑塞进 Workflow.cpp。
* 让 CommandManager 直接访问 LittleFS。
* 用 JSON 作为 Workflow 长期内部存储格式。
* 把大型对象重新放回任务栈。
* 使用 `git add .`。
* reset 用户已有修改。
* stash 用户已有修改。
* 为了“优化”而重构无关模块。
* 无意义改变现有 Workflow JSON 字段。
* 把 Capability Registry Version 当成 Workflow Variant。
* 为没有实际内容变化的 set 无意义增加 Variant。
* Save 部分成功后清空全部 Dirty。

---

# 四十一、最终架构总览

最终应该保持：

```text
                       CLOUD / APP
                           │
                           │ JSON
                           ▼
                    ┌───────────────┐
                    │ CloudManager  │
                    └───────┬───────┘
                            │
                            ▼
                    ┌───────────────┐
                    │CommandManager │
                    │               │
                    │ workflow.*    │
                    └───────┬───────┘
                            │
                            ▼
                    ┌───────────────┐
                    │ Workflow.cpp  │
                    │               │
                    │ JSON ↔        │
                    │ Definition    │
                    │               │
                    │ Variant       │
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


        Capability Registry
                │
                ├── Action Stable ID
                ├── Trigger Stable ID
                └── Workflow Stable ID
                              │
                              └── object_version
                                      =
                                  Workflow.variant
```

核心同步关系：

```text
Registry Version
    ↓
判断 Mapping 是否变化

Workflow Variant
    ↓
判断单个 Workflow 是否变化

Workflow JSON
    ↓
只有真正变化的 Workflow 才需要 get
```

最终形成：

```text
sync_info
    ↓
list
    ↓
compare variant
    ↓
get changed workflows
    ↓
Cloud Cache synchronized
```

这是未来 Cloud UI / Web UI / App UI 对接 Workflow 的标准入口。
