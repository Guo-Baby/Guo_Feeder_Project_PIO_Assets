# WorkflowManager 当前架构分析

分析基准：当前工作区 `src/workflow.h`（428 行）与 `src/workflow.cpp`（2140 行）；接口参照为定版 CommandManager（`main@2156800` 恢复版本，`src/command_manager.h/cpp`）。

本文档仅为当前代码真实状态分析，不包含重构方案。

---

## 1. WorkflowManager 当前职责

WorkflowManager 是工作流自动化框架与唯一 Action 执行者，当前代码中的实际职责：

- **注册表管理**：维护 Trigger / Action 描述符注册表（`trigger_registry` / `action_registry`，各 32 槽），提供 `workflow_register_trigger()` / `workflow_register_action()`。
- **Workflow 定义与实例池**：`Workflow workflows[16]` 静态数组；运行实例（Trigger/Action Instance）池在 `workflow_init()` 中按 `WORKFLOW_MAX_COUNT * WORKFLOW_MAX_STEP = 256` 个从 PSRAM（回退普通 RAM）分配。
- **非阻塞执行引擎**：`workflow_task()` 由 main loop 周期调用，驱动各 Workflow 的 Trigger/Action step 前进。
- **临时 Action 队列**：`workflow_enqueue_action()` 供外部（CommandManager）以"不创建 Workflow"的方式执行单个 Action，环形队列容量 8，带独立超时。
- **JSON 配置**：`workflow_load_json / workflow_load_json_file / workflow_save_json_file / workflow_export_json / workflow_reload`，来源为 LittleFS 文件（main.cpp 加载 `/workflow.json`）。
- **内置触发器**：`workflow_init()` 注册 Timer（once/daily/weekly）与 Delay 两个系统 Trigger。
- **事件响应**：`workflow_event_init()` / `workflow_event_callback()` 存在，但受 `WORKFLOW_EVENT_ENABLED = 0` 控制，默认不注册任何事件监听。

职责边界（代码事实）：

- 不感知 Cloud / Command / JSON 协议语义；不直接调用 CommandManager。
- 为 CommandManager 提供临时 Action 入口，但**不向任何上层回报结果**（无完成回调）。

## 2. Workflow 创建流程

创建分两个阶段：

**编译期注册（descriptor 注册）**：

- `workflow_register_action(desc)` / `workflow_register_trigger(desc)`：按 id 查重后写入注册表；当前实际调用者为 `valve_init()`（注册 VALVE_OPEN / VALVE_CLOSE 两个 Action）与 `workflow_init()`（注册 timer / delay 两个 Trigger）。

**运行期加载（实例化）**：

1. `workflow_load_json_file(path)`：从 LittleFS 读文件 → `workflow_load_json(String)`；
2. `workflow_load_json(String)`：`deserializeJson` → 缓存 `workflow_json_cache` → `workflow_parse_json(doc)`；
3. `workflow_parse_json(doc)`：
   - 先 `workflow_clear()` 清空数据（不清实例池），重置 `trigger_instance_index / action_instance_index`；
   - 遍历 `doc["workflows"]`，逐个填充 `Workflow`（id/name/enable/timeout_ms，state=IDLE，current_step=0）；
   - 遍历每个 `steps`：`type=="trigger"` 时按 step id 查 Trigger 描述符并从 trigger 实例池取实例（设置 id/descriptor/params/callback=workflow_trigger_callback）；`type=="action"` 同理取 Action 实例（callback=workflow_action_callback）；
   - **查不到描述符的 step**：实例置空，`step_index++` 后 continue（该 step 被跳过但仍计入 step_count）；
   - Step0 必须是 Trigger，否则该 Workflow 置 `WORKFLOW_ERROR`；
   - 参数经 `workflow_parse_params()` 解析为 `WorkflowParamValue[]`（int/float/bool/string）；
   - 完成后 `workflow_count = index`，`json_state = WORKFLOW_JSON_READY`；
   - `#if WORKFLOW_EVENT_ENABLED`（当前为 0）内才调用 `workflow_event_init()`。

## 3. Workflow 执行流程

`workflow_task()`（main loop 调用，非阻塞）：

```
for 每个 workflow:
  非 enable / 非 RUNNING → 跳过
  (millis - start_time) > timeout_ms → state = WORKFLOW_TIMEOUT
  current_step >= step_count → state = WORKFLOW_FINISHED
  否则取 steps[current_step]:
    Trigger step:
      未 running → descriptor->start()（置 RUNNING）
      已 running → descriptor->poll()
      TRIGGER_SUCCESS → current_step++
      TRIGGER_FAILED → WORKFLOW_ERROR
    Action step:
      未 running → workflow_action_start()（descriptor->start；start 后 result 仍 IDLE 则置 RUNNING）
      已 running → workflow_action_poll()（descriptor->poll）
      ACTION_SUCCESS → current_step++
      ACTION_FAILED → WORKFLOW_ERROR
```

启动入口 `workflow_start(workflow, skip_first_step)`：

- 前置条件：非空、`enable == true`、state 非 RUNNING；
- 重置全部 step（descriptor->reset + waiting=false）；
- `state = RUNNING`，`current_step = skip_first_step ? 1 : 0`，`start_time = millis()`。

完成/失败后**只修改 `wf.state` 字段，无任何上层通知**。

## 4. Workflow 状态机

枚举 `WorkflowState`（workflow.h）：`WORKFLOW_IDLE / RUNNING / WAITING / FINISHED / TIMEOUT / ERROR`。

当前代码中的实际状态转换：

| 转换 | 触发点 |
|---|---|
| IDLE → RUNNING | `workflow_start()`（或 `workflow_start_by_id()` / 事件回调） |
| RUNNING → FINISHED | `workflow_task()`：current_step 走完 |
| RUNNING → TIMEOUT | `workflow_task()`：`(now - start_time) > timeout_ms` |
| RUNNING → ERROR | Step0 非 Trigger（parse 期置 ERROR）；Trigger 或 Action 失败 |
| RUNNING → IDLE | `workflow_stop(id)` |
| 任意 → IDLE | `workflow_disable(id)` |

注意：`WORKFLOW_WAITING` 枚举存在，但当前代码中**没有任何赋值路径**。

## 5. Action 调用流程

存在两条独立路径：

**路径 A：Workflow 内 Action step**（`workflow_task()` → `workflow_action_start/poll`）

- 首次进入：`descriptor->start(action)`；start 后 `result == ACTION_IDLE` 则置 `ACTION_RUNNING`；
- 后续轮询：`descriptor->poll(action)`；poll 后 `result != RUNNING` 则 `running = false`；
- `ACTION_SUCCESS` → 前进到下一步；`ACTION_FAILED` → Workflow 置 ERROR。

**路径 B：临时 Action（外部调用）**（`workflow_enqueue_action()` → `enqueue_temp_action()` → `workflow_task()` 的 Temp Action 段）

- 环形队列（`MAX_TEMP_ACTIONS = 8`），满则拒绝；
- 分配唯一 `instance_id`（`temp_action_instance_counter`，溢出避开 0）；
- `workflow_task()` 轮询：未 running → start；running → poll；`SUCCESS/FAILED` 或超时（`(millis - start_time) > timeout_ms`）→ 置 completed 并调用 `item.callback(instance_id, result)`（**2 参回调**）；
- 完成后释放实例（`temp_action_free_instance`）并推进读指针。

Action 描述符生命周期函数指针：`reset` / `start` / `poll`。当前 valve 模块的 Action 在 `start` 内同步置结果，无需异步轮询。

## 6. Callback 机制

当前存在的回调：

- **实例级回调（未充分使用）**：
  - `WorkflowTriggerCallback(WorkflowTriggerInstance*, WorkflowTriggerState)`；
  - `WorkflowActionCallback(WorkflowActionInstance*, WorkflowActionResult)`；
  - parse 时赋给实例 `callback`，但引擎正常路径不调用；仅 `workflow_action_poll()` 在 descriptor/poll 缺失时调用 action callback 上报 FAILED；`workflow_trigger_callback` / `workflow_action_callback` 定义在 cpp 尾部，仅回写实例 result/state。
- **临时 Action 回调（实际生效）**：`TempActionItem.callback = void(*)(uint32_t instance_id, WorkflowActionResult result)`（2 参），由 `workflow_task()` 在完成或超时时调用；`workflow_enqueue_action()` 默认参数为 `nullptr`。

当前**不存在**：

- Workflow 完成回调（`WorkflowResultCallback` 在整个 workflow.* 中零出现）；
- command_id 参与的任何回调；
- 上层结果通知路径。

## 7. 与 CommandManager 的接口关系

以定版 CommandManager（main@2156800）为准，逐接口核对：

| CommandManager 调用 | 定版期望 | workflow 现状 |
|---|---|---|
| `workflow_enqueue_action(...)` | 6 参：`(id, payload String, &instance_id, command_id, CommandTempActionCallback(4参), timeout)` | workflow.h 声明 6 参但回调形参为 `void(*)(uint32_t, WorkflowActionResult)`（2 参，默认 nullptr），且**无 command_id 形参**；workflow.cpp 定义使用未定义的 `CommandTempActionCallback`（该类型只在 command_manager.h 定义，workflow.cpp 未包含） |
| `workflow_start(wf, true, command_id, WorkflowResultCallback)` | 4 参（带回调与 command_id） | 当前仅 2 参：`workflow_start(Workflow*, bool)`，无回调、无 command_id |
| `workflow_get_count / workflow_get / workflow_get_action_count / workflow_get_action_descriptor / workflow_get_trigger_count / workflow_get_trigger_descriptor` | 与现状一致 | 均存在且匹配 |
| `workflow_temp_action_is_complete` | 定版 CommandManager 已不调用（注释声明禁止轮询） | workflow.h:399 有声明，cpp **无定义** |

重点结论（代码事实）：

- **command_id 传递链路：不存在**。整个 workflow.h/cpp 中 `command_id` 零出现；临时 Action 唯一的关联 token 是 `instance_id`，回调仅携带 `(instance_id, result)`。
- **工作流执行完成后的结果返回路径：不存在**。完成/超时/失败只置 `wf.state`；没有任何回调/事件把结果送回 CommandManager。定版 CommandManager 的 Workflow 运行时记录只能依赖自身超时（600s）清理。
- workflow.cpp 当前因引用未定义的 `CommandTempActionCallback`、且 `workflow_enqueue_action` 头/源签名不一致而**无法通过编译**（属既有状态，已记录为 Phase 2 适配点）。

## 8. 当前存在的问题

以下为事实清单，不含方案：

1. **声明/定义不一致**：`workflow_enqueue_action` 在 workflow.h（2 参回调）与 workflow.cpp（`CommandTempActionCallback` 4 参）签名冲突；cpp 中该类型未定义（未包含 command_manager.h，也无本地 typedef）。
2. **未定义类型引用**：workflow.cpp:1579 使用 `CommandTempActionCallback`，当前 TU 内不可见 → 编译错误；`enqueue_temp_action` 的形参与 `TempActionItem.callback` 均为 2 参，与 4 参回调不匹配。
3. **声明无定义**：`workflow_temp_action_is_complete`（workflow.h:399）无实现。
4. **定义无声明**：`workflow_find_action`、`workflow_save_json`、`workflow_destroy_all_instances` 在 cpp 中为非 static 外部符号，头文件未声明。
5. **未使用结构**：`WorkflowActionTicket`（workflow.h:384）定义但无任何使用。
6. **缺少 Workflow 完成通知**：无 `WorkflowResultCallback`、无完成回调、无 command_id；上层无法感知执行结果。
7. **事件链路默认关闭**：`WORKFLOW_EVENT_ENABLED = 0`，`workflow_event_init()` 仅在 `#if` 内被调用，事件触发订阅实际不生效。
8. **`WORKFLOW_WAITING` 无赋值路径**：枚举存在但引擎从不进入该状态。
9. **未知 step id 处理**：parse 时查不到描述符的 step 被跳过但仍计入 `step_count`，且对应实例为 nullptr（运行时遇 nullptr 会置 WORKFLOW_ERROR，行为依赖数据）。
10. **状态无上层确认**：`workflow_start` 对 FINISHED/TIMEOUT/ERROR 状态的 Workflow 允许直接重新 start（会 reset 全部 step），无外部确认机制。
11. **超时判断细节**：Workflow 与临时 Action 超时均用 `>`（非 `>=`）；`start_time` 用 `millis()`，与 `timeout_ms`（默认 600000）语义一致。

## 9. 可能需要修改的文件列表

基于当前接口关系的事实推导（仅列出与定版 CommandManager 接口存在差异或声明/定义不一致的文件，不含修改方案）：

- `src/workflow.h`：`workflow_enqueue_action` 回调签名与 cpp 不一致；`workflow_start` 缺最终接口所需的回调/command_id 形参；`workflow_temp_action_is_complete` 声明无定义；与 cpp 中未声明外部函数（`workflow_find_action` 等）的对齐。
- `src/workflow.cpp`：`workflow_enqueue_action` 定义中的 `CommandTempActionCallback` 类型不可见；临时 Action 回调调用点（2 参）与最终回调契约不一致；无工作流完成通知路径。
- `src/command_manager.h`：`CommandTempActionCallback` / `WorkflowResultCallback` 当前定义于该头文件，是 workflow 侧若引用这些类型时的依赖来源（依赖方向问题，记录为接口关系事实）。

## 10. 关键函数、结构体、枚举定义和作用

### 枚举（均定义于 workflow.h）

| 枚举 | 成员 | 作用 |
|---|---|---|
| `WorkflowState` | IDLE / RUNNING / WAITING / FINISHED / TIMEOUT / ERROR | Workflow 生命周期状态 |
| `WorkflowStepType` | ACTION / TRIGGER | step 类别 |
| `WorkflowTriggerState` | IDLE / RUNNING / SUCCESS / FAILED | Trigger 实例状态 |
| `WorkflowActionResult` | IDLE / RUNNING / SUCCESS / FAILED | Action 实例执行结果 |
| `WorkflowParamType` | INT / FLOAT / BOOL / STRING | 参数类型 |
| `WorkflowJsonState` | EMPTY / LOADING / READY / ERROR | JSON 加载状态 |
| `WorkflowInstanceType` | TRIGGER / ACTION | 实例联合类型标记 |

### 结构体（定义于 workflow.h；TempActionItem 为 cpp 私有）

| 结构体 | 作用 |
|---|---|
| `Workflow` | 工作流定义与运行时（id/name/enable/start_time/timeout_ms/state/steps[16]/step_count/current_step） |
| `WorkflowStep` | 单步（type/instance_type/id/instance 联合/waiting） |
| `StepInstance` | union：trigger 或 action 实例指针 |
| `WorkflowTriggerDescriptor` | Trigger 描述符（id/name/module/params/reset/start/poll） |
| `WorkflowActionDescriptor` | Action 描述符（id/name/module/params/reset/start/poll） |
| `WorkflowTriggerInstance` | Trigger 运行实例（descriptor/id/params/state/timer_runtime/delay_runtime/running/callback） |
| `WorkflowActionInstance` | Action 运行实例（descriptor/id/params/result/running/runtime 私有上下文/callback） |
| `WorkflowParamValue` | 参数值（name/type + 四类值） |
| `WorkflowParam` | 参数描述（name/type/unit/description，给 UI 用） |
| `TimerRuntime` / `DelayRuntime` | 内置 Timer/Delay 触发器运行数据 |
| `WorkflowActionTicket` | 未使用（声明于 workflow.h:384） |
| `TempActionItem`（cpp） | 临时 Action 队列项（instance_id/desc/params/completed/result/start_time/instance/2参callback/timeout_ms） |

### 关键函数

| 函数 | 位置 | 作用 |
|---|---|---|
| `workflow_init()` | cpp:596 | 初始化计数、分配实例池（PSRAM 优先）、注册 timer/delay、初始化临时队列 |
| `workflow_register_trigger / workflow_register_action` | cpp:725/763 | 描述符注册（id 查重） |
| `workflow_start(Workflow*, bool)` | cpp:1381 | 重置步骤并置 RUNNING；**无回调/command_id 参数** |
| `workflow_start_by_id(String, bool)` | cpp:2108 | 按 id 转发 workflow_start |
| `workflow_task()` | cpp:1649 | 核心非阻塞状态机 + 临时 Action 队列轮询与回调 |
| `workflow_enqueue_action(...)` | cpp:1574 | 外部临时 Action 入口；**头/源签名不一致，cpp 使用未定义的 CommandTempActionCallback** |
| `enqueue_temp_action(...)`（static） | cpp:1478 | 环形队列入队，分配唯一 instance_id |
| `workflow_temp_action_pending_count()` | cpp:1633 | 等待中的临时 Action 数 |
| `workflow_temp_action_is_complete(...)` | workflow.h:399 | **只有声明，无定义** |
| `workflow_action_start/poll/reset` | cpp:803/828/856 | Action step 的启动/轮询/重置封装 |
| `workflow_trigger_start/poll/reset` | cpp:1415/1434/1458 | Trigger step 的启动/轮询/重置封装 |
| `workflow_parse_json(JsonDocument&)` | cpp:1040 | JSON → Workflow/实例 解析 |
| `workflow_load_json / workflow_load_json_file` | cpp:1246/1266 | JSON 加载（文件走 LittleFS） |
| `workflow_save_json_file / workflow_export_json` | cpp:1291/1907 | 保存/导出 JSON |
| `workflow_reload()` | cpp:1322 | 用缓存重载（RUNNING 或临时队列非空时拒绝） |
| `workflow_clear()` | cpp:918 | 清数据（不清实例池） |
| `workflow_enable / disable / stop` | cpp:2003/2017/2032 | 启停控制 |
| `workflow_event_init / workflow_event_callback` | cpp:2053/2083 | 事件订阅与响应（默认禁用） |
| `workflow_get_count / get / get_action_count / get_action_descriptor / get_trigger_count / get_trigger_descriptor` | cpp:873-908 | 查询接口（CommandManager 使用） |
| `workflow_find_action(String)` | cpp:885 | 按 id 查 Action 描述符（头文件未声明） |
| `workflow_trigger_callback / workflow_action_callback` | cpp:2118/2130 | 实例回调实现（仅回写 state/result） |
| `workflow_destroy_all_instances()` | cpp:163 | 析构并释放实例池（头文件未声明） |

### 重点核查结论

- **`workflow_enqueue_action`**：对外临时 Action 入口；环形队列（8）；唯一 `instance_id`；完成/超时回调为 **2 参** `(instance_id, result)`；当前头/源签名不一致，且 cpp 引用了本 TU 不可见的 `CommandTempActionCallback`。
- **`workflow_start`**：仅 2 参 `(Workflow*, bool skip_first_step)`，只负责状态与 step 重置，**无回调、无 command_id**。
- **`WorkflowResultCallback`**：在 workflow.* 中**不存在**（定版 command_manager.h 中有定义）。
- **command_id 传递链路**：**不存在**（workflow.h/cpp 全文无 `command_id`）。
- **工作流执行完成后的结果返回路径**：仅 `wf.state` 置位（FINISHED/TIMEOUT/ERROR），可经 `workflow_get()` 查询；**无任何主动通知/回调到 CommandManager**。
