# Workflow Storage + Definition / Runtime Separation 改造需求

## 1. 改造目标

在现有 Workflow Manager 稳定执行框架基础上，引入：

1. Workflow Definition / Runtime 完全分离
2. Step 级 BIN 持久化
3. Workflow Meta 有效性管理
4. 256 bit Step Dirty Bitmap
5. Dirty Transaction Critical Operation
6. 多 Step 一次性事务保存
7. 原子写入 + CRC
8. Workflow 删除采用 Invalid 标记
9. 运行中的 Workflow 可以修改 Definition
10. 当前运行继续使用旧 Runtime Snapshot
11. 下一次运行使用新的 Definition
12. Workflow.cpp 与 LittleFS 完全解耦
13. 所有文件操作统一通过 FileStorage

---

# 2. 必须保持的现有约束

以下常量不得改变：

```cpp
WORKFLOW_MAX_PARAM = 8
WORKFLOW_MAX_STEP  = 16
WORKFLOW_MAX_COUNT = 16
```

因此：

```text
16 Workflow
×
16 Step
=
256 Step Slot
```

Dirty Bitmap：

```cpp
uint32_t dirty_bitmap[8];
```

共：

```text
256 bit
=
32 byte
```

---

# 3. 文件布局

建议：

```text
/workflow/
    meta.bin

    W00S00.bin
    W00S01.bin
    ...
    W00S15.bin

    W01S00.bin
    ...
    W15S15.bin
```

内部索引采用 0-based：

```text
workflow_index = 0..15
step_index     = 0..15
```

Slot：

```cpp
slot = workflow_index * WORKFLOW_MAX_STEP + step_index;
```

注意：

**256 个可能的 Step Slot 不代表启动时必须创建 256 个物理文件。**

不存在的 / 未使用 Step 不需要提前创建。

---

# 4. WorkflowMeta

Meta 只负责 Workflow 级别的持久化生命周期信息。

至少包含：

```text
Magic
Format Version
Workflow Valid Bitmap
CRC32
```

其中：

```text
valid bitmap = 16 bit
```

表示：

```text
bit 0 -> Workflow 0
bit 1 -> Workflow 1
...
bit 15 -> Workflow 15
```

---

## 4.1 Meta 不保存 Runtime

不得持久化：

```text
RUNNING
current_step
start_time
runtime
callback
Action result
Trigger runtime
```

这些都是 RAM Runtime 状态。

设备 reboot 后：

```text
所有 Workflow 从 IDLE 重新开始
```

不得尝试恢复一个掉电前正在执行的 Workflow。

---

# 5. Workflow Definition

Definition 是 Workflow 的持久化数据在 RAM 中的运行表示。

Definition 至少包含：

```text
Workflow ID
Workflow Name
Enable
Timeout
Step Count
Step Definitions
```

其中：

```text
Step Count
```

属于 Workflow Definition，而不是 WorkflowMeta。

例如：

```text
Workflow 4

step_count = 8
```

表示：

```text
Step 0
Step 1
Step 2
Step 3
Step 4
Step 5
Step 6
Step 7
```

有效。

```text
Step 8..15
```

不存在，不允许执行。

Workflow 执行完成的判断必须使用：

```cpp
current_step >= step_count
```

而不是：

```cpp
current_step >= WORKFLOW_MAX_STEP
```

---

# 6. Step Definition

Step Definition 只能保存可持久化的数据。

不得包含：

```text
function pointer
descriptor pointer
runtime pointer
callback
running
result
timer runtime
delay runtime
Action Instance
Trigger Instance
```

Step Definition 应保存类似：

```text
Step Type
Instance Type
Action / Trigger ID
Parameter Count
Parameters
```

Descriptor 必须通过 ID 在启动 / 加载时重新解析：

```text
stored Action ID
      ↓
find_action_descriptor()
      ↓
Descriptor
```

不得将函数指针直接写入 BIN。

---

# 7. Step BIN

每个 Step 对应一个独立 BIN：

```text
W00S00.bin
W00S01.bin
...
```

BIN 必须是自描述、可校验的数据。

建议至少包含：

```text
Magic
Format Version
Workflow Index
Step Index
Payload Length
Step Definition Payload
CRC32
```

具体字段大小可以根据现有代码和 FileStorage API 做最小化设计。

要求：

* 可验证文件是否属于正确 Workflow
* 可验证文件是否属于正确 Step
* 可验证格式版本
* 可验证 Payload 完整性
* 不保存任何指针

---

# 8. Definition 加载策略

采用：

## 启动时加载所有 Valid Workflow Definition

流程：

```text
Boot
  |
  v
读取 meta.bin
  |
  v
读取 Valid Bitmap
  |
  v
遍历 Valid Workflow
  |
  v
读取该 Workflow 的 Step BIN
  |
  v
建立 Definition RAM
```

不读取 Invalid Workflow。

不要求启动时读取所有 256 个 Slot。

例如：

```text
Workflow 0 Valid
Workflow 1 Invalid
Workflow 2 Valid
```

则只加载：

```text
Workflow 0
Workflow 2
```

---

# 9. Step Definition 与 Runtime 必须彻底分离

当前代码中：

```text
ActionInstance / TriggerInstance
```

同时承担：

```text
Definition
Runtime
```

必须拆开。

目标：

```text
Workflow
 |
 +-- Definition
 |
 +-- Runtime
```

其中：

```text
Definition
    |
    +-- Step 0
    +-- Step 1
    +-- ...
    +-- Step N
```

Runtime：

```text
Workflow Start
      |
      v
Runtime Pool
      |
      v
从 Definition 创建 Snapshot
```

---

# 10. Runtime Snapshot

Runtime 必须在：

```cpp
workflow_start()
```

时创建。

创建时：

```text
Definition
    |
    | value copy
    v
Runtime Snapshot
```

尤其是：

```text
params
```

必须进行值拷贝。

Runtime 不允许：

```cpp
const WorkflowParamValue* params;
```

长期引用一个可能被修改的 Definition。

Runtime 必须拥有自己的运行参数快照。

---

# 11. Runtime Pool

不得恢复为每次：

```cpp
malloc()
free()
```

动态申请 Runtime。

推荐保留现有 PSRAM Pool 思路，但改为：

```text
统一 Runtime Pool
```

而不是：

```text
256 Trigger Instance
256 Action Instance
```

因为一个 Step 同时只能属于：

```text
Trigger
或
Action
```

建议建立统一：

```text
StepRuntime Pool
```

最大容量：

```text
WORKFLOW_MAX_COUNT * WORKFLOW_MAX_STEP
=
256
```

Runtime Pool 位于 PSRAM。

采用固定槽位 + 使用状态管理。

不得在 Workflow 执行期间频繁 malloc/free。

---

# 12. Runtime 生命周期

```text
workflow_start()
      |
      v
申请 Runtime Pool slot
      |
      v
从 Definition 构造 Snapshot
      |
      v
执行 Workflow
      |
      v
Workflow Finish / Error / Timeout / Stop
      |
      v
释放 Runtime Pool slot
```

Runtime Pool slot 释放后：

```text
descriptor
String
params
runtime
callback
result
running
```

等运行时字段必须按现有 Instance 生命周期规则完整清理。

---

# 13. 运行中的 Workflow 修改

Definition / Runtime 分离完成后：

**允许运行中的 Workflow 修改自己的 Definition。**

例如：

```text
Workflow 4 正在运行

Runtime:
    Step1 参数 = 20g

用户修改 Definition:
    Step1 参数 = 30g
```

当前运行：

```text
继续使用 20g
```

下一次运行：

```text
使用 30g
```

修改 Definition：

```text
不得修改当前 Runtime Snapshot
```

---

# 14. Workflow 之间互不影响

例如：

```text
Workflow 4 RUNNING
Workflow 5 IDLE
```

修改 Workflow 5：

```text
只修改 Workflow 5 Definition
```

不得影响：

```text
Workflow 4 Runtime
```

修改 Workflow 4：

```text
只修改 Workflow 4 Definition
```

也不得影响：

```text
Workflow 4 当前 Runtime
```

---

# 15. Dirty Bitmap

使用：

```cpp
uint32_t dirty_bitmap[8];
```

映射：

```cpp
slot = workflow_index * WORKFLOW_MAX_STEP + step_index;
```

例如：

```text
W00S00 -> bit 0
W00S15 -> bit 15
W01S00 -> bit 16
W15S15 -> bit 255
```

提供内部操作：

```text
mark_dirty()
clear_dirty()
is_dirty()
has_any_dirty()
```

---

# 16. Critical Operation：采用“Dirty Transaction”模型

**不是每个 Step 一个 Critical Operation。**

而是：

> 一批尚未持久化的 Workflow 修改，共享一个 Critical Operation。

状态：

```text
没有 Dirty
    |
    | 第一次修改产生 Dirty
    v
Critical +1
    |
    | 后续继续修改
    | 不再增加 Critical
    |
    | 可以修改 1 个、10 个、30 个 Step
    |
    v
Save Transaction
    |
    | 全部成功
    v
清除 Dirty
    |
    v
Critical -1
```

---

# 17. Critical +1 的准确时机

只有：

```text
当前不存在任何 Dirty
```

并且：

```text
本次修改使至少一个 Step 从 Clean -> Dirty
```

时：

```cpp
system_command_critical_operation_acquire();
```

也就是说：

```text
has_any_dirty() == false
```

→ 本次第一次产生 Dirty：

```text
Critical +1
```

之后：

```text
Dirty -> Dirty
```

不得再次 Critical++。

---

# 18. Critical -1 的准确时机

执行一次完整 Save Transaction。

例如有：

```text
30 个 Dirty Step
```

保存：

```text
S01
S02
...
S30
```

全部成功，并且：

* BIN 写入成功
* CRC 校验成功
* Meta 必要更新成功
* Dirty 清理成功

才：

```cpp
system_command_critical_operation_release();
```

---

# 19. 不允许通过后台轮询决定 Critical

不得设计：

```text
每个 loop：

检查 dirty
检查 critical
检查 dirty
检查 critical
```

来决定何时 release。

Save Transaction 自己就是事务边界：

```text
Save Start
    |
    v
保存所有 Dirty
    |
    v
全部成功？
    |
   Yes
    |
    v
Clear Dirty
    |
    v
Critical--
```

因此：

**没有持续轮询 Critical 的必要。**

Dirty Bitmap 本身只用于：

* 判断是否有修改
* 找到需要保存的 Step
* 保存后清除对应 Dirty

---

# 20. 保存多个 Step 必须是一个事务

例如：

```text
W01S01 Dirty
W01S02 Dirty
W01S03 Dirty
...
W10S08 Dirty
```

总共 30 个。

Save：

```text
Critical = 1

写 Step1
写 Step2
...
写 Step30

全部成功

↓

清 Dirty

↓

Critical = 0
```

任何一个 Step 写失败：

```text
不得 release Critical
不得认为事务成功
不得允许系统因本次修改而安全重启
```

未成功持久化的 Dirty 必须保留。

---

# 21. 多次修改的行为

允许用户在保存前连续修改：

```text
修改 W01S01
修改 W01S02
修改 W01S03
修改 W04S07
...
```

所有修改：

```text
直接修改 Definition RAM
```

并：

```text
mark_dirty()
```

只在第一次从：

```text
Clean -> Dirty
```

时：

```text
Critical++
```

---

# 22. 延迟保存

参考 ConfigManager 当前机制。

第一次 Workflow 修改：

```text
启动延迟保存计时
```

后续修改：

```text
刷新 / 延续已有保存窗口
```

具体 5 分钟行为应与 ConfigManager 当前已验证逻辑保持一致。

到期：

```text
Save Transaction
```

保存所有 Dirty Step。

---

# 23. 显式 Save

如果未来 CommandManager 提供：

```text
workflow_save
```

则立即触发：

```text
Save Transaction
```

不得再等待 5 分钟。

---

# 24. Restart 行为

收到：

```text
system_restart
```

如果 Workflow 存在 Dirty：

```text
先执行 Workflow Save Transaction
```

成功后：

```text
Critical release
```

然后调用现有：

```cpp
system_command_request_restart();
```

不得重新设计 SystemCommand 的 Restart 状态机。

继续复用现有：

```text
RESTART_REQUESTED
        |
        v
Critical == 0
        |
        v
RESTART_PENDING
        |
        v
10 秒安全窗口
        |
        v
ESP.restart()
```

---

# 25. Save 失败

如果任意 Dirty Step 保存失败：

```text
Critical 不释放
Dirty 不得错误清除
```

系统不能因为 Save 失败而继续执行本次 Restart。

必须保留错误状态，允许后续重新 Save。

不得因为 Save 失败直接清空 RAM Definition。

---

# 26. Workflow 删除

删除 Workflow：

**不删除 Step BIN。**

例如：

```text
Workflow 4
```

删除：

```text
meta.valid[4] = false
```

即可。

原有：

```text
W04S00.bin
W04S01.bin
...
```

保留。

---

# 27. 删除正在运行的 Workflow

如果 Workflow 4 当前正在运行：

```text
删除 Workflow4
```

只修改：

```text
Definition / Valid
```

不得销毁当前 Runtime Snapshot。

当前运行：

```text
继续执行
```

运行结束后：

```text
Workflow4 不再允许下一次启动
```

---

# 28. Workflow 重建

例如 Workflow 4 已删除：

```text
valid[4] = false
```

重新创建：

```text
修改 W04S00
修改 W04S01
...
```

全部 Definition 建立完成。

保存：

```text
Step BIN 全部成功
       |
       v
Meta valid[4] = true
       |
       v
提交完成
```

**Valid 必须最后提交。**

---

# 29. Meta 与 Step 的提交顺序

必须：

```text
Step BIN
   ↓
全部写成功
   ↓
CRC 成功
   ↓
Meta 更新
   ↓
Meta 原子提交
```

不得：

```text
先 Meta = Valid
再写 Step
```

否则掉电可能产生：

```text
Meta 宣布 Workflow 有效
但是 Step 不完整
```

---

# 30. Step BIN CRC

每个 Step BIN 必须能够独立验证。

加载时：

```text
读取 BIN
    |
    v
检查 Header
    |
    v
检查 Length
    |
    v
计算 CRC
    |
    v
CRC 正确？
```

失败：

```text
该 Workflow 标记 Invalid / Error
```

不得因为一个 Workflow 的 Step 损坏而破坏其他 Workflow。

---

# 31. Workflow Definition 加载

加载：

```text
meta
 ↓
valid workflow
 ↓
读取 step_count
 ↓
读取 Step 0..step_count-1
```

只加载：

```text
0 <= step < step_count
```

不得读取：

```text
step_count .. 15
```

空 Step 不参与执行。

---

# 32. Workflow 执行完成条件

当前 Workflow：

```text
step_count = 8
```

执行：

```text
Step 0
Step 1
...
Step 7
```

之后：

```cpp
current_step >= step_count
```

Workflow：

```text
WORKFLOW_FINISHED
```

然后向现有 CommandManager / CloudManager 回传：

```text
workflow_result
```

保持当前已有成功 / 失败 / timeout 回调机制。

不得修改现有 CommandManager 的结果协议，除非当前代码结构确实要求适配。

---

# 33. Step 0 规则

继续保持现有规则：

```text
Step 0 必须是 Trigger
```

Workflow Start：

```text
skip_first_step = true
```

时：

```text
current_step = 1
```

不得因为 Storage 改造而改变现有 Workflow 语义。

---

# 34. Workflow 校验

只做结构性校验：

* Workflow 数量是否合法
* Step Count <= 16
* Step 0 是否为 Trigger
* Step ID 是否存在
* Action / Trigger 是否注册
* Parameter 数量是否合法
* Parameter 类型是否合法
* BIN 是否完整
* CRC 是否正确
* Format Version 是否兼容

不得判断用户业务逻辑是否“合理”。

例如：

```text
Trigger 永远不会发生
```

不是 Storage 层的错误。

---

# 35. Workflow Timeout

继续保持现有：

```text
Workflow Timeout
```

机制。

目标：

**Workflow 不得无限运行。**

Storage 改造不得改变：

```text
start_time
timeout_ms
WORKFLOW_TIMEOUT
```

等运行时语义。

---

# 36. CommandManager 分层

CommandManager 不得：

```text
直接修改 Workflow 内部数组
```

不得：

```text
直接调用 FileStorage
```

不得：

```text
直接操作 BIN
```

调用链：

```text
CloudManager
      |
      v
CommandManager
      |
      v
WorkflowManager API
      |
      +---- 修改 Definition
      |
      +---- Dirty
      |
      +---- Save
      |
      v
FileStorage
      |
      v
LittleFS
```

---

# 37. 所有 Workflow 文件操作必须通过 FileStorage

Workflow.cpp 不得出现：

```cpp
LittleFS.open()
LittleFS.remove()
LittleFS.rename()
```

也不得直接包含：

```cpp
LittleFS.h
```

Workflow 文件读写必须通过：

```cpp
file_storage_*
```

接口。

---

# 38. 所有 Workflow 业务逻辑放在 workflow.cpp

本次 Workflow 改造：

Workflow 业务逻辑必须继续集中在：

```text
workflow.cpp
workflow.h
```

包括：

* Workflow Definition
* Runtime
* Runtime Pool
* Snapshot
* Step Count
* Workflow CRUD
* Workflow Validation
* Dirty 管理
* Workflow Save orchestration
* Workflow Load orchestration
* Workflow 执行
* Workflow Runtime 生命周期

FileStorage 只负责：

```text
文件读写
```

不得把 Workflow 业务规则塞进 FileStorage。

---

# 39. Workflow.cpp 不负责 BIN 底层 IO

例如：

错误：

```cpp
workflow.cpp:

LittleFS.open(...)
```

也不应该在 Workflow.cpp 中自己实现：

```text
tmp file
rename
CRC
```

正确：

```cpp
workflow.cpp
    |
    v
file_storage_write_atomic()
```

Workflow.cpp 负责决定：

```text
写哪个文件
写什么数据
什么时候写
哪些文件必须写
什么时候提交 Meta
```

FileStorage 负责：

```text
如何可靠地写文件
```

---

# 40. 现有 Workflow 执行逻辑必须尽量保留

当前 Workflow.cpp 已经包含大量经过测试的逻辑。

本次不得无理由重写：

* Action 生命周期
* Trigger 生命周期
* Temp Action
* Workflow callback
* timeout
* event callback
* CommandManager 接口
* Critical Operation 运行体机制

本次重点是：

```text
Storage
+
Definition/Runtime Separation
```

而不是重新实现 Workflow Engine。

---

# 41. Runtime Pool 与 Temporary Action Pool 必须继续独立

现有：

```text
Workflow Runtime Pool
```

与：

```text
temp_action_instances[8]
```

继续保持物理隔离。

不得因为统一 Runtime Pool 而让：

```text
Workflow Action Instance
```

和：

```text
Temporary Action Instance
```

共享同一个 instance。

---

# 42. Temporary Action 行为不得改变

CommandManager：

```text
execute_action
```

仍然使用：

```text
Temporary Action
```

机制。

不得因为 Definition / Runtime Separation 将其改成 Workflow Step。

---

# 43. Runtime Snapshot 的关键安全要求

Runtime Snapshot 一旦创建：

```text
Definition 后续任何修改
```

都不得影响：

```text
Runtime Snapshot
```

尤其禁止：

```cpp
runtime.params = definition.params;
```

这种长期引用。

必须实现：

```text
value copy
```

---

# 44. Descriptor

Descriptor：

```text
const WorkflowActionDescriptor*
const WorkflowTriggerDescriptor*
```

继续由编译期注册表提供。

不得持久化 Descriptor。

BIN 只保存：

```text
Action / Trigger ID
```

加载后：

```text
ID
 ↓
find_descriptor()
 ↓
const descriptor*
```

---

# 45. Definition 修改与 Runtime 修改完全分离

用户命令：

```text
update workflow
```

只允许修改：

```text
Definition
```

不得修改：

```text
Runtime
```

Runtime 只能由：

```text
workflow_start
workflow_task
workflow_terminate
workflow_stop
```

等执行生命周期管理。

---

# 46. Dirty 的生命周期

```text
Clean
 |
 | 修改第一个 Step
 v
Dirty
 |
 | 可以继续修改任意 Step
 |
 v
Save
 |
 +----失败----> Dirty 保持
 |
 +----成功----> Dirty 清除
                  |
                  v
                Clean
```

Critical：

```text
Clean -> Dirty
    +1

Dirty -> Dirty
    0

Dirty -> Clean
    -1
```

---

# 47. 性能原则

不得为了 Dirty / Critical 实现持续后台扫描。

Dirty Bitmap 的扫描只发生在：

```text
需要 Save 时
```

保存时本来就必须找到 Dirty Step，因此这个扫描是必要工作。

Critical counter 不需要：

```text
每 loop 检查
```

也不需要：

```text
每个 Dirty Step 一个 counter
```

---

# 48. 掉电安全

必须保证：

### Case 1

RAM 修改，尚未 Save：

```text
掉电
```

结果：

```text
RAM 修改丢失
Flash 保留旧 Definition
```

这是可接受行为。

---

### Case 2

Step BIN 写入过程中掉电：

使用：

```text
file_storage_write_atomic()
```

保证旧文件尽可能保持完整。

---

### Case 3

多个 Step 保存：

```text
S01 成功
S02 成功
S03 失败
```

不得宣布整个事务成功。

Dirty / Critical 必须保留，允许后续重试。

---

### Case 4

所有 Step 成功，但 Meta 尚未提交：

```text
掉电
```

旧 Meta 保持。

因此 Workflow 仍按照旧 Valid 状态恢复。

---

### Case 5

Meta 已提交：

```text
Workflow Valid
```

则所有本次需要提交的 Step 必须已经成功写入。

因此：

**Meta 永远最后提交。**

---

# 49. Git 要求

修改任何项目文件之前：

```bash
git status
```

如果存在用户已有未提交修改：

* 保护用户修改
* 不 reset
* 不 checkout
* 不 stash
* 不 `git add .`
* 必要时先 checkpoint commit

修改过程中：

```bash
git diff
```

确认只修改本次 Workflow 相关内容。

完成：

```bash
git status
git diff
```

确认没有无关修改。

然后创建独立 commit。

建议：

```text
feat: refactor workflow storage and runtime isolation
```

---

# 50. 实施顺序

必须严格按照以下顺序：

### Phase 1

Definition / Runtime Separation

先解决：

```text
params 与 runtime 共用一个 instance
```

---

### Phase 2

Runtime Pool

将 Runtime 改为固定 Pool。

---

### Phase 3

Workflow Definition RAM

建立独立 Definition。

---

### Phase 4

Step BIN + Meta

通过 FileStorage 实现持久化。

---

### Phase 5

Dirty Bitmap

加入：

```text
uint32_t dirty_bitmap[8]
```

---

### Phase 6

Dirty Transaction + Critical

实现：

```text
Clean -> Dirty
    Critical++

Dirty -> Clean
    Critical--
```

---

### Phase 7

Save Transaction

实现：

```text
dirty steps
    ↓
全部保存
    ↓
Meta
    ↓
清 Dirty
    ↓
Critical release
```

---

### Phase 8

Workflow CRUD

实现：

```text
create
update
delete
```

---

### Phase 9

Running Workflow Definition 修改

验证：

```text
Running Runtime
        +
Definition 修改
        =
Runtime 不受影响
```

---

### Phase 10

完整回归测试

至少测试：

1. 空 Workflow
2. 单 Step Workflow
3. 16 Step Workflow
4. 16 Workflow
5. 多 Workflow 同时运行
6. 修改 Idle Workflow
7. 修改 Running Workflow
8. 删除 Idle Workflow
9. 删除 Running Workflow
10. 修改多个 Step
11. 30 个 Dirty Step 一次保存
12. 保存失败
13. BIN CRC 错误
14. Meta CRC 错误
15. 重启期间存在 Dirty
16. Restart + Dirty
17. Workflow Timeout
18. Workflow Error
19. Temporary Action
20. Event Trigger
21. Timer Trigger

---

# 51. 本阶段明确禁止

不得：

* 修改 Workflow 最大数量
* 修改最大 Step 数
* 修改最大 Parameter 数
* 改变 Step 0 Trigger 规则
* 改变 Workflow Timeout
* 改变 Temporary Action 架构
* 让 CommandManager 直接操作 Storage
* 让 Workflow.cpp 直接访问 LittleFS
* 将 Descriptor / function pointer 写入 BIN
* 保存 Runtime 状态到 Flash
* 为每个 Dirty Step 单独增加 Critical Operation
* 用持续 loop 轮询 Dirty 来维护 Critical
* 为 Runtime 使用频繁 malloc/free
* 因“业务逻辑可能不合理”拒绝合法结构
* 一次修改只保存部分 Dirty Step 后就宣布事务成功

---

# 52. 最终目标架构

```text
                     CloudManager
                          |
                          v
                   CommandManager
                          |
                          v
                  WorkflowManager
                          |
             +------------+-------------+
             |                          |
             v                          v
      Workflow Definition          Runtime Pool
             |                          |
             |                    Runtime Snapshot
             |                          |
             +------------+-------------+
                          |
                          v
                  Dirty Transaction
                          |
                          v
                    FileStorage
                          |
                          v
                       LittleFS
                          |
          +---------------+---------------+
          |                               |
          v                               v
     Workflow Meta                    Step BIN
     meta.bin                    W00S00.bin ...
```

核心原则：

> **Definition 是“下一次执行什么”；Runtime 是“这一次正在执行什么”。**

> **修改 Definition 不影响正在运行的 Runtime。**

> **Dirty 表示 RAM Definition 与 Flash Definition 不一致。**

> **Critical Operation 表示存在尚未完成持久化的用户修改事务。**

> **Step BIN 负责保存数据，Meta 负责 Workflow Valid 生命周期，FileStorage 负责底层文件可靠读写。**

本阶段完成后，Workflow Manager 应能够在不破坏现有 Workflow Engine 行为的前提下，实现可靠的 Step 级持久化、运行时隔离和安全重启。
