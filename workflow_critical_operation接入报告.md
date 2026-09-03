# Workflow 模块 Critical Operation 接入修改报告

- **修改文件**：`src/workflow.cpp`（本次**仅**改动此文件，未修改 `workflow.h` 及其他任何模块）
- **依据文档**：`critical_operation接入规范.md` §3（标准接入模式）、§5（各模块接入点）
- **修改日期**：2026-09-03
- **验证状态**：未编译、未烧录（按要求由开发者实测）

---

## 一、设计要点

### 1.1 计数模型：每个执行体各自 +1 / -1

按需求所述，Workflow 与临时 Action 都可能并发多个，因此采用"每个执行体独立计数"：

| 执行体 | acquire 时机 | release 时机 |
|---|---|---|
| Workflow | `workflow_start()` 成功启动前 | 执行完毕（FINISHED / TIMEOUT / ERROR）或被强制终止 |
| 临时 Action | `enqueue_temp_action()` 入队前 | 执行完毕（SUCCESS / FAILED / 超时） |

多个 Workflow + 多个临时 Action 并发时，critical count 为各自之和，**全部结束才归零**，系统才会进入 10s 安全窗口并重启。

### 1.2 为什么用"持有标记"数组而不是裸计数

新增 `static bool workflow_critical_held[WORKFLOW_MAX_COUNT]`（`workflow.cpp:91`），与 `workflows[]` 下标一一对应；临时 Action 则在 `TempActionItem` 内新增 `bool critical_held`（`workflow.cpp:153`）。

作用有三个：

1. **release 幂等** —— 未持有就不调用 `release()`，避免触发 system_command 的 `critical op release underflow` 配对错误日志；
2. **覆盖无回调的 Workflow** —— 事件触发 / 定时器触发的 Workflow 没有 `finish_callback`，若把 release 写进 `workflow_notify_finish()` 内部会因提前 return 而漏掉；
3. **覆盖强制终止路径** —— `workflow_stop()` / `workflow_disable()` / `workflow_clear()` 也必须释放，否则计数泄漏导致**系统永久无法重启**。

> 之所以不改 `workflow.h` 的 `Workflow` 结构体而用平行数组，是因为本次改动范围限定在 `workflow.cpp`。

### 1.3 时序约束：`workflow_init()` 早于 `system_command_init()`

`main.cpp` 中 `workflow_init()`（:57）早于 `command_manager_init()`（:71）→ `system_command_init()`。因此 `workflow_init()` 中**只重置标记数组，不调用任何 system_command API**（`workflow.cpp:734-740`），避免在自旋锁初始化前触碰 system_command 内部状态。

---

## 二、修改点清单

### 2.1 基础设施

| 行号 | 内容 |
|---|---|
| `:9` | 新增 `#include "system_command.h"` |
| `:66-90` | Critical Operation 保护的设计说明注释块 |
| `:91` | 新增 `static bool workflow_critical_held[WORKFLOW_MAX_COUNT]` |
| `:94-110` | 新增 `workflow_index_of()` —— 由 `Workflow*` 反查 `workflows[]` 下标，越界返回 -1 |
| `:112-133` | 新增 `workflow_critical_release_by_index()` —— 幂等释放，仅在持有标记为真时 release |
| `:153` | `TempActionItem` 新增 `bool critical_held` 字段 |
| `:209-234` | 新增 `temp_action_critical_release()` —— 临时 Action 幂等释放 |
| `:238-257` | 新增 `temp_action_complete()` —— **临时 Action 结束统一收口**（标记完成 → 释放 → 回调） |
| `:1642-1655` | 新增 `workflow_terminate()` —— **Workflow 终止统一收口**（写状态 → 释放 → 回调） |

### 2.2 Acquire（开始保护）

| 行号 | 位置 | 行为 |
|---|---|---|
| `:1537-1573` | `workflow_start()` | 在 Reset Step 之前 acquire。失败则打印 `[Workflow] critical op acquire rejected, workflow start aborted: <id>` 并 **return false**（不启动该 Workflow） |
| `:1750-1772` | `enqueue_temp_action()` | 在申请实例之后、写队列之前 acquire。失败则 `temp_action_free_instance(inst)` 归还实例后 **return false**（不执行该 Action），避免实例池泄漏 |

两处 acquire 均**严格遵守规范 §3**：`acquire()` 返回 false 时放弃本次操作，且**不调用** `release()`。

### 2.3 Release —— Workflow 的 6 条结束路径

全部改为调用 `workflow_terminate(wf, state)`，覆盖成功 / 失败 / 超时三种结果：

| 行号 | 结束原因 | 终态 |
|---|---|---|
| `:1975` | Workflow 整体超时 | `WORKFLOW_TIMEOUT` |
| `:1984` | 所有 Step 执行完毕 | `WORKFLOW_FINISHED` |
| `:2001` | Step0 Trigger 实例为空 | `WORKFLOW_ERROR` |
| `:2045` | Trigger 执行失败 | `WORKFLOW_ERROR` |
| `:2062` | Action 实例为空 | `WORKFLOW_ERROR` |
| `:2093` | Action 执行失败 | `WORKFLOW_ERROR` |

### 2.4 Release —— 临时 Action 的 3 条结束路径

全部改为调用 `temp_action_complete(item, result)`：

| 行号 | 结束原因 | result |
|---|---|---|
| `:2128` | 等待超时 | `ACTION_FAILED` |
| `:2142` | Action 实例为空（防御分支） | `ACTION_FAILED` |
| `:2185` | 执行完毕（成功或失败） | `ACTION_SUCCESS` / `ACTION_FAILED` |

### 2.5 Release —— 强制终止与防御路径

| 行号 | 位置 | 说明 |
|---|---|---|
| `:1062` | `workflow_clear()` | reload / 重新解析 JSON 会清空所有运行态，必须先释放。**注意**：`workflow_reload()` 只拦截了"有临时 Action 在跑"，未拦截运行中的 Workflow，因此这里是关键补漏点 |
| `:2328` | `workflow_disable()` | 禁用会强制结束正在运行的 Workflow |
| `:2348` | `workflow_stop()` | 强制停止同样属于"结束" |
| `:1912` | `workflow_temp_action_cleanup()` | 防御性兜底。正常路径下 Item 完成时标记已置 false，此处为空操作；仅当将来新增绕过 `temp_action_complete()` 的分支时才真正生效 |
| `:1933` | `workflow_temp_action_cleanup()` | Item 回收时重置 `critical_held = false` |
| `:734-740` | `workflow_init()` | 仅 `memset` 标记数组，**不调用** system_command API（见 1.3 节） |

---

## 三、行为变化说明（需留意）

1. **重启期间新的 Workflow / 临时 Action 会被拒绝**
   系统进入 `RESTART_PENDING`（10s 安全窗口）后，`workflow_start()` 返回 false、`workflow_enqueue_action()` 返回 false。调用方（CommandManager）会收到 `CMD_ERROR_EXECUTION` / 执行失败应答。这是规范的预期行为（§8：PENDING 后禁止新的 Critical Operation）。

2. **一处顺带的既有缺陷修正**（`workflow.cpp:2137-2146`）
   原代码中"Action 实例为空"分支只置 `completed=true` / `result=FAILED`，**不调用回调**，会导致命令永远不上报。现已并入 `temp_action_complete()`，会正常回调上报失败。
   该分支实践中不可达（`enqueue_temp_action()` 保证 `item.instance != nullptr`），因此属于纯防御性修正，不改变正常运行行为。

3. **新增串口日志**（便于实测观察配对）

   ```
   [Workflow] critical op acquired (workflow idx=N, id=xxx, count=N)
   [Workflow] critical op released (workflow idx=N, count=N)
   [Workflow] critical op acquired (temp action id=N, act=xxx, count=N)
   [Workflow] critical op released (temp action id=N, count=N)
   [Workflow] critical op acquire rejected, workflow start aborted: xxx
   [Workflow] critical op acquire rejected, temp action aborted: xxx
   ```

---

## 四、建议实测项

下发命令工具：`python tools/mqtt_send.py '<json>'`（每条命令 `i` 必须唯一）。
状态查询：`{"c":"system","i":"9xxx","p":{"o":"restart_status"}}`，关注 `critical_operations` 字段。

| # | 测试项 | 步骤 | 预期 |
|---|---|---|---|
| 1 | 空闲基线 | 直接查 `restart_status` | `critical_operations: 0` |
| 2 | Workflow 执行中阻塞重启 | 启动一个耗时较长的 Workflow → 下发 `system.restart` → 查状态 | `state:"requested"`，`critical_operations:1`，设备**不重启**；Workflow 结束后 count 归 0 → 10s 后重启 |
| 3 | 临时 Action 阻塞重启 | 下发临时 Action 命令 → 下发 `system.restart` → 查状态 | 同 #2，count 至少为 1；Action 完成（含超时）后归零 |
| 4 | **并发计数** | 同时启动 2 个 Workflow + 2 个临时 Action → 查状态 | `critical_operations == 4`；随各执行体结束逐个递减，全结束后归零 |
| 5 | **超时释放** | 下发一个必然超时的临时 Action（或临时调小 `timeout_ms`）→ 观察 | 串口 `Temp action timeout` 后紧随 `critical op released`，count 归零，**不得**残留 |
| 6 | **失败释放** | 下发一个必然失败的 Workflow / Action | 串口先报错再 `critical op released`，count 归零 |
| 7 | **强制停止释放** | Workflow 运行中下发 `workflow_stop` | `critical op released (workflow idx=N)`，count 归零；再下发 restart 应能正常重启 |
| 8 | **PENDING 后被拒绝** | 先 `system.restart` 等进入 `pending` → 立即启动 Workflow / Action | 串口 `critical op acquire rejected ... aborted`，命令返回失败；实例池不泄漏（可连续重试验证） |
| 9 | **配对无下溢** | 全流程跑一遍后观察串口 | 绝不能出现 `[SYSCMD][E] critical op release underflow` —— 出现即配对 bug |
| 10 | reload 期间不泄漏 | Workflow 运行中触发 `workflow_reload` | `workflow_clear()` 释放全部持有计数，count 归零 |

**最关键的两条判据**：
- 正常执行完后 `critical_operations` 必须回到 **0**（否则系统永久无法重启）；
- 串口**不得**出现 `critical op release underflow`（否则 acquire/release 不配对）。

---

## 五、未改动说明

- 未修改 `workflow.h`（用平行静态数组代替给 `Workflow` 结构体加字段）
- 未修改 `system_command.cpp/.h`、`command_manager.cpp`、`config_manager.cpp` 及任何其它模块
- 未新增任何重启机制；重启仍唯一由 `system_command.cpp` 的 `ESP.restart()` 执行
- 未编译、未烧录，按要求交由开发者实测
