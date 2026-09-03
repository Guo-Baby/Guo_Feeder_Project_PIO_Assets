# Computer Reset Action 接入报告

> 需求来源：`AI_Task.md`《Computer Reset Action 功能需求文档》
> 日期：2026-09-03
> 状态：已编码完成，**未编译、未烧录、未 git commit**

---

## 1. 改动清单

| 文件 | 类型 | 说明 |
| --- | --- | --- |
| `src/computer_reset.h` | 新增 | GPIO 常量、HOLD 常量、对外 4 个接口 |
| `src/computer_reset.cpp` | 新增 | 模块实现 + `COMPUTER_RESET` Action 注册 |
| `src/main.cpp` | 修改 | 3 处接入 + 2 条串口调试命令 |

**未修改**：`command_manager.cpp` / `cloud_manager.cpp` / `workflow.cpp` / `system_command.*` / `config_manager.*` / `event_manager.*` / `system_state.*` / `capability_registry.*`。

Capability Registry 是启动期自动扫描 `workflow` 注册表生成的，新 Action 会被自动纳入，**无需任何改动**。

---

## 2. `src/main.cpp` 修改点与位置

| 位置 | 修改 |
| --- | --- |
| `#include` 区（:15） | 新增 `#include "computer_reset.h"` |
| `setup()` 第四层（:61） | 新增 `computer_reset_init();`（在 `valve_init()` 之后、`dispense_guard_init()` 之前） |
| `loop()`（:120） | 新增 `computer_reset_task();`（在 `valve_task()` 之后） |
| 串口调试命令（:178-190） | 新增 `computer_reset` / `computer_reset_status` 两条命令 |

**初始化层级说明**：`computer_reset_init()` 必须放在 `workflow_init()`（:57）之后，因为它内部调用 `workflow_register_action()`。放在第四层"业务模块注册"完全符合现有分层约定。

---

## 3. 模块设计要点

### 3.1 执行链路（未新增任何独立路径）

```
云端 → CloudManager → CommandManager(execute_action)
     → WorkflowManager(Temporary Action 队列)
     → COMPUTER_RESET Action(start/poll) → GPIO8
```

同一份 Descriptor 天然支持 Workflow Step 调用，模块不感知调用来源。

### 3.2 输出仲裁（关键设计）

GPIO8 是**单例硬件资源**，但可能有多个 Action 实例并发（多个 Workflow 并行）。因此没有让每个实例各自拉高/拉低，而是：

```
active_pulse_count > 0  → 保持 HIGH
active_pulse_count == 0 → 拉低
```

- `start()`：`count++` → 置 HIGH
- `poll()` 到期：`count--`，**归零者负责拉低**
- `reset()`：若实例仍持有上下文则释放（计数归零时拉低）

`used` 标记保证 release 幂等，杜绝计数下溢误拉低正在进行的脉冲。

### 3.3 安全兜底（AI_TASK §8）

三条独立保障，确保任何退出路径 GPIO8 最终为 LOW：

1. **正常路径**：`poll()` 判定 800ms 到期 → 释放 → 拉低
2. **reset 路径**：Action 被 Stop / Clear / 重置时释放上下文
3. **task 兜底**（`computer_reset_task()`）：输出 HIGH 持续超过 `800 + 1200 = 2000ms` 仍未被拉低 → 无条件 `force_idle()`

第 3 条覆盖了"poll 因实例被强制回收而不再被调用"这类极端路径。

### 3.4 上下文静态池

`ComputerResetCtx ctx_pool[4]`，无动态分配。`manual` 位区分：
- `false`：Workflow Action 产生，由 `poll()` 回收
- `true`：`computer_reset_trigger()` 产生，由 `computer_reset_task()` 回收

### 3.5 明确未做的事（AI_TASK 约束）

| AI_TASK 条款 | 落实情况 |
| --- | --- |
| §7 不增加可配置参数、不改 ConfigManager | ✅ `param_count = 0`，未触碰 config |
| §9 不使用 Critical Operation | ✅ 未调用任何 `system_command_critical_operation_*` |
| §10 不改 Restart 机制 | ✅ 未调用 `ESP.restart()` |
| §13 非阻塞 | ✅ 全程 `millis()` 判定，无 `delay()` |
| §16 不改 SystemState / EventManager | ✅ 只用 `Serial` 打印，未新增 State / Event |

---

## 4. 一处与需求文档的字面偏差（需你确认）

`AI_TASK.md §11` 写的是 Action 名称使用 `computer_reset`；但 §17.9 要求"遵循现有 Action / Capability Registry 的代码风格"，而项目现有 runtime id 全部为大写下划线。

**当前实现**：

```cpp
.id     = "COMPUTER_RESET"      // runtime id，大写下划线，风格一致
.module = "computer_reset"      // 模块名，小写
```

现有 id 参照：`VALVE_OPEN` / `VALVE_CLOSE` / `WEIGHT_ZERO` / `MI_THERMO_START_SCAN` / `MI_THERMO_STOP_SCAN`。

**如果坚持用小写 `computer_reset` 作为 runtime id**，改 `computer_reset.cpp:212` 一行即可（云端命令的 `ob` 字段同步改）。目前倾向保持大写，与 Capability Registry 的 ID 排序/展示风格统一。

---

## 5. 云端下发指令

### 5.1 方式 A：旧格式透传（推荐先测这条）

无需 stable_id / version，最简单：

```bash
python tools/mqtt_send.py '{"cmd":"execute_action","id":"8001","ob":"COMPUTER_RESET"}'
```

- `cmd` = `execute_action` → CommandManager `execute_router`
- `ob` = Action runtime id
- `id` 每条命令必须唯一（设备按 cmd_id 去重）
- `pl` 可省略（本 Action 无参数）

### 5.2 方式 B：新格式（稳定短 ID，需要 version + stable_id）

```bash
# 第一步：查 Action Registry，拿到 version 和 COMPUTER_RESET 的 stable_id
python tools/mqtt_send.py '{"c":"registry","i":"8000","k":0}'

# 第二步：用查到的 v / k 下发
python tools/mqtt_send.py '{"c":"action","i":"8002","v":<version>,"k":<stable_id>}'
```

`k=0` 对应 `REGISTRY_ACTION`（`cloud_manager.h:82`）。

**⚠️ 重要**：新增 Action 后，Capability Registry 的 **version 会 +1**，且 stable_id 是按 runtime_id 字符串升序重新分配的，**所有 Action 的 stable_id 都可能变化**。云端若缓存了旧 mapping，会因 version 不匹配被设备拒绝（`ERROR_VERSION_MISMATCH`）。云端必须重新拉取 registry。

当前 6 个 Action 按升序排列后，推算结果如下（**以设备实际返回为准**）：

```
0 = COMPUTER_RESET
1 = MI_THERMO_START_SCAN
2 = MI_THERMO_STOP_SCAN
3 = VALVE_CLOSE
4 = VALVE_OPEN
5 = WEIGHT_ZERO
```

### 5.3 查询类辅助命令

```bash
# 查询已注册 Action（CommandManager 旧接口，返回 name/id/module/description）
python tools/mqtt_send.py '{"c":"query","i":"8003","p":{"q":"actions"}}'

# 查询 Registry（新接口，返回 stable_id + runtime_id 映射）
python tools/mqtt_send.py '{"c":"registry","i":"8004","k":0}'
```

---

## 6. 建议测试顺序

### 6.1 串口本地验证（不需要 MQTT）

```
computer_reset              → 返回 ret = 1，GPIO8 变 HIGH
computer_reset_status       → 800ms 后查，应返回 active state: 0
```

预期串口日志：

```
[ComputerReset] Manual pulse started (active=1)
[ComputerReset] GPIO8 = HIGH
[ComputerReset] GPIO8 = LOW          <- 约 800ms 后
```

### 6.2 云端临时 Action

```bash
python tools/mqtt_send.py '{"cmd":"execute_action","id":"8010","ob":"COMPUTER_RESET"}'
```

预期上行两条：

1. `status: "accepted"`, `message: "Action queued"`, `action: "COMPUTER_RESET"`, `instance_id: N`
2. 约 800ms 后：`type: "action_result"`, `action: "COMPUTER_RESET"`, 结果 success

串口对应日志：

```
[Workflow] critical op acquired (temp action id=N, act=COMPUTER_RESET, count=1)
[ComputerReset] Pulse started (hold=800 ms, active=1)
[ComputerReset] GPIO8 = HIGH
[ComputerReset] Pulse finished (elapsed=800 ms, active=0)
[ComputerReset] GPIO8 = LOW
[Workflow] critical op released (temp action id=N, act=COMPUTER_RESET, count=0)
```

**关键判据**：`critical op released` 必须出现，`count` 必须回到 0。

### 6.3 边界条件

| 场景 | 预期 |
| --- | --- |
| 连续下发 2 条 `COMPUTER_RESET` | 串行执行（临时 Action 队列队头阻塞），第二条在第一条完成后才启动；总时长约 1.6s |
| 下发不存在的 Action id | `CMD_ERROR_EXECUTION` / `Failed to queue action`（`find_action_descriptor` 返回 nullptr） |
| 执行期间下发 `restart` | 临时 Action 持有 Critical Operation，重启被推迟到脉冲结束；不影响 800ms 脉冲本身 |
| 重复 cmd_id | `[Cloud] Command duplicate`，被丢弃 |
| 模块未初始化时执行 | `Action rejected: module not initialized`（只要 main.cpp 接入正确就不会出现） |

---

## 7. 已知限制 / 后续可选项

1. **无 SystemState / Event 上报**：AI_TASK 未要求且 §16 禁止改这两个模块，当前只有 Serial 日志。若云端需要感知"电脑重启执行过"，需另立需求新增 `EVENT_COMPUTER_RESET` 与对应 State（要改 3 处文件）。
2. **无执行次数限制**：重复快速触发会连续打脉冲，可能损伤主板。若需要最小间隔保护，可在模块内加冷却期（不改动其他模块即可实现）。
3. **未做 git commit**，等你编译验证通过后再决定。
