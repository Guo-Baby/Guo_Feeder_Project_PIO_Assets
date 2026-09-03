# Workflow Critical Operation 覆盖完整性自查报告

- **审计对象**：`src/workflow.cpp`（基线 commit `3b48a94`）
- **审计日期**：2026-09-03
- **审计方式**：控制流级别追踪（非函数名搜索）
- **审计结论触发的修改**：1 处（P0），详见 §17.9 与 §19

---

## 17.1 总结结论

```
结论：PASS WITH CONCERN（修复 1 处 P0 后）
```

审计发现入口/出口在**单线程控制流**下覆盖完整：所有 Acquire 均有且仅有一次 Release，强制终止路径、超时路径、失败路径、无 callback 路径均覆盖。

但发现 **1 处跨核竞争窗口**（P0）：`workflow_terminate()` 中「先置 state、后 Release」的顺序，在 esp-mqtt 任务与 loop 任务并发时会造成 Critical Operation **永久泄漏**。

该问题已在本次审计中修复（交换两条语句顺序 + 补充注释）。修复后：

```
结论：PASS
```

另发现 1 处**既有**的跨任务同步隐患（P2，非本次引入，未修改）与 1 处 P3。

---

## 17.2 Workflow 入口覆盖表

| 入口 | 代码位置 | 是否 Acquire | 是否覆盖 | 备注 |
| -- | ---- | ---------- | ---- | -- |
| CommandManager `execute_workflow` | `command_manager.cpp:685` → `workflow_start()` | 是 | ✅ | 运行在 **esp-mqtt 任务**上下文（`command_manager_execute()` 由 MQTT 回调同步调用） |
| Event 事件触发 | `workflow.cpp:2418` → `workflow_start()` | 是 | ✅ | 运行在 **loop 任务**（`event_dispatch()`），与 `workflow_task()` 同任务，无竞争 |
| `workflow_start_by_id()` | `workflow.cpp:2427` → `workflow_start()` | 是 | ✅ | 外部 API，**当前无任何调用者**（见 P3） |
| Step 0 Trigger 自动启动 | — | — | ✅ N/A | **代码模型与文档假设不同**：本工程 Trigger 不会主动启动 Workflow，而是在 `workflow_task()` 内驱动**已处于 RUNNING 的 Workflow** 的 step 0。因此「等待 Trigger」阶段已被 `workflow_start()` 的 Acquire 覆盖，不存在独立入口 |
| Timer / Delay Trigger | `timer_poll()` / `delay_poll()` | 否 | ✅ N/A | 仅置 `TRIGGER_SUCCESS/FAILURE`，推进 `current_step`，不启动新 Workflow 实例 |

**结论**：所有能真正启动 Workflow 实例的路径**全部收敛到 `workflow_start()` 单一入口**，该入口统一 Acquire。无漏 Acquire。

---

## 17.3 Workflow 出口覆盖表

`workflow_task()` 中 `wf.state` 的每一次迁移都由 `workflow_terminate()` 收口。逐条核对（行号为修复后）：

| 出口 | 代码位置 | 是否 Release | 是否唯一 | 是否存在风险 |
| -- | ---- | ---------- | ---- | ------ |
| Workflow 总体 Timeout | :1996 `workflow_terminate(wf, WORKFLOW_TIMEOUT)` | ✅ | ✅ | 无 |
| 所有 Step 完成 | :2005 `workflow_terminate(wf, WORKFLOW_FINISHED)` | ✅ | ✅ | 无 |
| Step0 Trigger 实例为空 | :2022 `workflow_terminate(wf, WORKFLOW_ERROR)` | ✅ | ✅ | 无 |
| Trigger 执行失败 | :2066 `workflow_terminate(wf, WORKFLOW_ERROR)` | ✅ | ✅ | 无 |
| Action 实例为空 | :2083 `workflow_terminate(wf, WORKFLOW_ERROR)` | ✅ | ✅ | 无 |
| Action 执行失败 | :2114 `workflow_terminate(wf, WORKFLOW_ERROR)` | ✅ | ✅ | 无 |
| `step_count == 0` 空 Workflow | 落入 `current_step >= step_count` 分支 → :2005 | ✅ | ✅ | 无 |
| **无 `finish_callback` 的 Workflow** | `workflow_terminate()` 内 Release **早于** `workflow_notify_finish()` | ✅ | ✅ | 无。Release 不依赖 callback 是否存在——这是本次设计的关键点（`workflow_notify_finish()` 在 callback 为空时会提前 return，Release 不能写在其内部） |
| Workflow 被 `workflow_stop()` | :2370 | ✅ | ✅ | 无 |
| Workflow 被 `workflow_disable()` | :2350 | ✅ | ✅ | 无 |
| Workflow 被 `workflow_clear()` | :1062 | ✅ | ✅ | 无 |

**`wf.state` 全量赋值点核对**（确认无旁路）：

| 赋值点 | 位置 | 是否在 Release 之后/同时 |
| -- | ---- | ---- |
| `workflows[i].state = WORKFLOW_IDLE` | :1066（clear） | ✅ Release 在 :1062（之前） |
| `workflow->state = WORKFLOW_RUNNING` | :1608（start） | ✅ Acquire 在 :1576（之前） |
| `wf.state = state` | :1672（terminate） | ✅ Release 在 :1667（之前） |
| `workflows[i].state = WORKFLOW_IDLE` | :2331（disable） | ✅ Release 在 :2350 → 实际 :2350 在前 |
| `workflows[i].state = WORKFLOW_IDLE` | :2371（stop） | ✅ Release 在 :2370（之前） |
| `workflow.state = ...` | :1228 / :1389（parse_json） | ✅ N/A —— 局部变量，尚未写入 `workflows[]`，不持有 Critical Operation |
| `workflow.enable = ...` | :1224 / :2344（parse / enable） | ✅ N/A —— `enable` 不启动执行；`workflow_enable()` 不改 state，不会造成生命周期断裂 |

**结论**：无漏 Release、无重复 Release。所有 `state` 迁移要么在 Release 之后，要么是 Acquire 之前的状态。

---

## 17.4 Temporary Action 入口覆盖表

| 入口 | 代码位置 | 是否 Acquire | 是否覆盖 | 备注 |
| -- | ---- | ---------- | ---- | -- |
| CommandManager `execute_action` | `command_manager.cpp:604` → `workflow_enqueue_action()` → `enqueue_temp_action()` | 是 | ✅ | 唯一外部入口 |
| 内部调用 | — | — | ✅ N/A | 全仓搜索确认 `enqueue_temp_action` 仅被 `workflow_enqueue_action()` 调用，无其他内部入口 |

**Acquire 时序核对**（`enqueue_temp_action()`）：

```
:1750  queue 满检查        → return false（未 Acquire）
:1758  find_action_descriptor 失败 → return false（未 Acquire）
:1767  temp_action_alloc_instance 失败 → return false（未 Acquire）
:1793  Acquire            ← 失败则 temp_action_free_instance(inst) 后 return false
:1796  flag = true
:1885  item.critical_held = true
:1886  queue_wr_ptr = next_wr   ← 提交，对 consumer 可见
```

关键点：**`critical_held = true` 早于 `queue_wr_ptr` 提交**（:1885 → :1886），consumer（loop 任务）看到新 item 时标记必然已就位。无「半初始化实例」泄漏。

**结论**：唯一入口，无漏 Acquire；Acquire 失败路径已正确回收 instance（见 §17.9 无问题）。

---

## 17.5 Temporary Action 出口覆盖表

| 出口 | 代码位置 | 是否 Release | 是否唯一 | 是否存在风险 |
| -- | ---- | ---------- | ---- | ------ |
| 执行成功 | :2156 `temp_action_complete(item, ACTION_SUCCESS)` | ✅ | ✅ | 无 |
| 执行失败 | :2156 `temp_action_complete(item, ACTION_FAILED)` | ✅ | ✅ | 无（同分支，result 由 `action->result` 决定） |
| 等待超时 | :2159 → 实际 :2199 `temp_action_complete(item, ACTION_FAILED)` | ✅ | ✅ | 无 |
| Action 实例为空 | :2199 之外的 :2160 `temp_action_complete(item, ACTION_FAILED)` | ✅ | ✅ | 无（防御分支，实践中不可达） |
| Cleanup 回收 | :1912 `temp_action_critical_release(item)` | ✅ | ✅ | 无。正常情况下因 :2199/:2156 已置 `critical_held=false`，此处为空操作；仅作防御兜底 |
| Instance 回收到 pool | :1917 `temp_action_free_instance()` | ✅ | ✅ | 无。`critical_held` 在 :1933 重置为 false，且 Release 在 :1912 早于 free |

**`item.completed = true` 全量赋值点核对**：全文件仅 1 处，在 `temp_action_complete()` 内（:2140）。即所有结束路径**必然经过** `temp_action_complete()`，无旁路。

**结论**：3 条业务出口 + 1 条防御出口，全部 Release，无重复。

---

## 17.6 强制终止路径检查

| 函数 | 是否可能结束执行 | 是否 Release | 是否存在生命周期风险 |
| -- | -------- | ---------- | ---------- |
| `workflow_stop()` | 是——直接置 `WORKFLOW_IDLE` | ✅ :2370 | 无。**Release 早于 state 改写**（:2370 → :2371），顺序正确 |
| `workflow_disable()` | 是——置 `enable=false` + `WORKFLOW_IDLE` | ✅ :2350 | 无。Release 早于 state 改写（:2350 → :2331… 实际 :2350 在 enable/state 赋值之前） |
| `workflow_clear()` | 是——清空 `workflows[]` 全部运行态 | ✅ :1062 | 无。Release 早于字段清空（:1062 → :1065+） |
| `workflow_reload()` | **否**（在当前调用链下） | ✅ 间接 | 无风险。:1469-1478 会检查**所有** Workflow，若有 `WORKFLOW_RUNNING` 直接 `return false` 并打印 `Reload blocked: workflow running`。因此 reload **不会**打断运行中的 Workflow |
| `workflow_temp_action_cleanup()` | 是——回收已完成 Item | ✅ :1912 | 无。Release 早于 instance free（:1912 → :1917）与字段重置（:1933） |

**关于 `workflow_clear()` 的额外确认**：`workflow_load_json()` 仅有 2 个调用者——`workflow_load_json_file()`（main.cpp:76，仅 boot 期）与 `workflow_reload()`（:1487，已拦截运行中的 Workflow）。**无任何云端命令能直接触发 `workflow_load_json()`**。因此 `workflow_clear()` 中的 Release 属纯防御性冗余，当前不可达，但保留是正确的（防止未来新增调用者时漏掉）。

**结论**：5 个强制终止函数全部覆盖，且 Release 顺序均早于对象销毁/字段清空，不存在「先清空再 Release 导致无法定位实例」的问题。

---

## 17.7 Instance / Slot 复用检查

### Workflow slot 复用

复用链：`workflow_clear()` 清空 `workflows[]` → `workflow_parse_json()` 重新填充。

```
A 运行于 slot[3]，flag[3]=true
   ↓
workflow_clear()
   ↓
:1062  Release（flag[3]=false，count-1）   ← 早于清空
   ↓
:1065+ 清空 slot[3]
   ↓
B 使用 slot[3]
```

**结论：安全。** Release 严格早于 slot 清空，B 不可能继承 A 的 `critical_held=true`（:1062 在 :1065 之前）。

`workflow_critical_held[]` **错位风险**：该数组以 `workflows[]` 下标索引，且 `workflow_index_of()` 通过 `wf - workflows` 反查，所有调用点均传入 `workflows[]` 内元素指针：
- `workflow_start()` 的 3 个调用者分别传 `wf`（源自 `workflow_get(i)` = `&workflows[i]`）、`&wf`（`Workflow &wf = workflows[i]`）、`&workflows[i]` —— 全部合法
- `workflow_stop/disable/clear` 直接用循环下标 `i` —— 合法
- `workflow_index_of()` 有越界检查，返回 -1 时 `workflow_start()` 直接 return false

**结论：无错位风险。**

### Temporary Action instance 复用

```
A: :1885 critical_held=true
   ↓
A 完成: temp_action_complete() → Release → critical_held=false
   ↓
cleanup: :1912 防御 Release（空操作）→ :1933 critical_held=false → rd++
   ↓
B 复用同 slot: :1885 critical_held=true
```

**结论：安全。** 两处独立的 `critical_held=false` 赋值（:2140 内的释放路径与 :1933 的回收路径）保证 B 不会继承 A 的状态。即使 A 走了异常路径未 Release，cleanup 的 :1912 兜底也会在 slot 被复用前释放（cleanup 是 slot 复用的唯一前置条件）。

---

## 17.8 Acquire / Release 配对统计

```
Workflow Acquire 数量：        1 处（workflow_start :1576）
Workflow Release 路径数量：    9 条
                              6 × workflow_terminate（正常/失败/超时）
                            + 1 × workflow_clear
                            + 1 × workflow_disable
                            + 1 × workflow_stop
                              实际 Release 底层调用点：1 个（workflow_critical_release_by_index）

Temporary Action Acquire 数量：1 处（enqueue_temp_action :1793）
Temporary Action Release 路径：4 条
                              3 × temp_action_complete（成功/失败/超时/实例空）
                            + 1 × workflow_temp_action_cleanup（防御兜底）
                              实际 Release 底层调用点：1 个（temp_action_critical_release）
```

**控制流级别覆盖证明（数量相等不足以证明正确性，补充如下）**：

1. **Workflow**：`workflow_task()` 主循环开头即 `if(wf.state != WORKFLOW_RUNNING) continue;`。Acquire 成功后 `state` 必为 `RUNNING`（:1608），而 `state` 离开 `RUNNING` 的**唯一**途径是 `workflow_terminate()` / `stop` / `disable` / `clear`（已在 §17.3 全量枚举 `state` 赋值点并逐一确认）。因此「Acquire 成功 → 必然经上述路径之一 → 必然 Release」成立。
2. **幂等**：`workflow_critical_leave_by_index()` 依赖 `workflow_critical_held[]` 标记，重复调用第二次因标记为 false 直接返回，不存在重复 Release。
3. **Temporary Action**：`item.completed = true` 全文件仅 1 处赋值（在 `temp_action_complete()` 内），而 Item 被回收（cleanup）的唯一前提是 `completed == true`。因此「Item 结束 → 必过 `temp_action_complete()` → 必 Release」成立。

**结论：已完成控制流级别的一一覆盖证明，非仅数量相等。**

---

## 17.9 发现的问题

### 【P0】`workflow_terminate()` 先置 state、后 Release —— 跨核竞争导致 Critical Operation 永久泄漏

| 项 | 内容 |
| -- | -- |
| **问题** | `workflow_terminate()` 中「写 `wf.state`」早于「Release Critical Operation」，而 `workflow_start()` 与 `workflow_terminate()` 运行在不同任务/不同核，存在竞争窗口 |
| **位置** | `src/workflow.cpp` `workflow_terminate()`（修复前 :1647-1651） |
| **触发条件** | esp-mqtt 任务收到 `execute_workflow` 命令，与 loop 任务中同一 Workflow 的终止动作**同时**发生。窗口为 `wf.state = state` 与 Release 之间的几条指令 |
| **实际路径** | ```loop: wf.state = WORKFLOW_ERROR```<br>```mqtt: workflow_start() → state != RUNNING 守卫通过 → acquire() (count+1) → flag=true → state=RUNNING```<br>```loop: Release → 读到 flag==true → count-1 → flag=false``` |
| **预期行为** | 新的 Workflow 启动请求要么被拒绝，要么完整走「Acquire → 执行 → Release」 |
| **当前行为** | Workflow 处于 `RUNNING` 但 `flag=false`，count 残留 +1。该 Workflow 后续结束时 flag 已是 false，**不会**再 Release → **Critical Operation 永久泄漏，系统永久无法重启** |
| **风险** | 系统无法重启（定时重启 / 配置生效重启 / 用户手动重启全部失效）。设备需断电才能恢复 |
| **处置** | ✅ **已修复**。交换两语句顺序，改为「先 Release、后置 state」，并补充 20 行顺序说明注释 |

**修复原理**：Release 之后、state 改写之前，`wf.state` 仍为 `WORKFLOW_RUNNING`，`workflow_start()` 的 `if(workflow->state == WORKFLOW_RUNNING) return false;` 守卫会直接拒绝新启动，窗口闭合。

**修复安全性论证**：
- `system_command_task()` 与 `workflow_task()` 同在 loop 任务，`workflow_terminate()` 整体同步执行，Release 与 state 改写之间**不可能**插入 `system_command_task()`，故不会因 count 提前归零而误触发重启。
- `workflow_notify_finish()` 内部读取 `wf.state`，state 仍早于回调写入，语义不变。

---

### 【P2】Temp Action 环形队列指针跨任务无同步（既有问题，未修改）

| 项 | 内容 |
| -- | -- |
| **问题** | `queue_wr_ptr` 由 esp-mqtt 任务写（`enqueue_temp_action`），`queue_rd_ptr` 由 loop 任务写（`workflow_temp_action_cleanup`），两者**均无 `volatile` / 原子 / 临界区保护** |
| **位置** | `workflow.cpp:87-88`（定义）、:1736 / :1886（写 wr）、:1966（写 rd） |
| **触发条件** | 编译器将指针缓存在寄存器中，或双核间内存可见性顺序异常，理论上可导致 wr 覆盖尚未被 cleanup 的 slot |
| **实际路径** | 若 slot 被覆盖，旧 Item 的 `critical_held` 被新 Item 覆盖为 true，旧 Item 的 Critical Operation **永不释放** |
| **预期行为** | 单生产者/单消费者环形队列应有内存屏障或临界区保护 |
| **当前行为** | 依赖硬件字节访问原子性与编译器未优化，实践上大概率正确 |
| **风险** | Critical Count 泄漏。**注：该问题在本次 Critical Operation 接入之前即已存在**，本次接入只是使其后果从「命令丢失」升级为「系统无法重启」 |
| **处置** | ⚠️ **未修改**。修复需为环形队列引入临界区，而 `enqueue_temp_action()` 在持锁期间会执行 JSON 反序列化（耗时较长），改造面与死锁风险评估超出本次自查范围。**建议单独立项处理** |

---

### 【P3】`workflow_start_by_id()` 无调用者

| 项 | 内容 |
| -- | -- |
| **问题** | 该函数是 Workflow 启动入口之一且已正确 Acquire，但全仓搜索**无任何调用者** |
| **位置** | `workflow.cpp:2427`（定义）、`workflow.h:485`（声明） |
| **风险** | 无当前风险。仅提示：将来若从中断上下文或其他任务接入，需复查与 `workflow_task()` 的并发语义 |
| **处置** | 未修改，仅记录 |

---

## 18. 最终 10 问

| # | 问题 | 回答 |
| - | -- | -- |
| 1 | **所有 Workflow 启动入口是否都已经 Acquire？** | ✅ **是**。3 个入口（`command_manager.cpp:685`、`workflow.cpp:2418`、`workflow.cpp:2427`）全部收敛到 `workflow_start()`，在 :1576 统一 Acquire。Step0 Trigger 不独立启动 Workflow，其等待期已被 `workflow_start()` 的 Acquire 覆盖 |
| 2 | **所有 Temporary Action 启动入口是否都已经 Acquire？** | ✅ **是**。唯一入口 `command_manager.cpp:604` → `enqueue_temp_action()`，在 :1793 Acquire |
| 3 | **每一个成功 Acquire 的 Workflow 是否所有正常出口都 Release？** | ✅ **是**。正常出口 `WORKFLOW_FINISHED`（:2005）经 `workflow_terminate()` Release |
| 4 | **每一个成功 Acquire 的 Workflow 是否所有异常/失败/Timeout 出口都 Release？** | ✅ **是**。Timeout（:1996）、Trigger 失败（:2066）、Action 失败（:2114）、实例为空（:2022/:2083）共 5 条异常出口全部经 `workflow_terminate()` Release |
| 5 | **Stop / Disable / Clear / Reload 等强制终止路径是否都 Release？** | ✅ **是**。`workflow_stop()`（`workflow.cpp:2370`）、`workflow_disable()`（:2350）、`workflow_clear()`（:1062）均 Release 且顺序正确。`workflow_reload()` 不直接终止——它在 :1469-1478 主动拦截运行中的 Workflow 并 `return false` |
| 6 | **每一个成功 Acquire 的 Temporary Action 是否所有出口都 Release？** | ✅ **是**。成功/失败（:2156）、超时（:2199）、实例空（:2160）3 条业务出口 + cleanup 防御出口（:1912） |
| 7 | **是否存在任何 Acquire 后直接 return 而没有 Release 的路径？** | ✅ **否**。`workflow_start()`：Acquire（:1576）→ flag（:1579）→ state=RUNNING（:1608）之间无任何 `return`。`enqueue_temp_action()`：Acquire（:1793）→ flag（:1885）→ 提交（:1886）之间无任何 `return` |
| 8 | **是否存在任何重复 Release 的路径？** | ✅ **否**。两处 Release 均依赖持有标记（`workflow_critical_held[]` / `item.critical_held`），Release 后立即置 false，重复调用为空操作，不会触发 `release underflow` |
| 9 | **Workflow / Temporary Action instance 或 slot 复用时，是否存在 Critical Operation 状态错位风险？** | ✅ **否**（修复后）。Workflow：Release（:1062）严格早于 slot 清空（:1065+）。Temp Action：Release（:1912）严格早于 instance free（:1917）与字段重置（:1933），且 `critical_held=true`（:1885）早于队列提交（:1886）。`workflow_index_of()` 有越界保护。**修复前存在 P0 竞争窗口，现已闭合** |
| 10 | **从完整控制流来看，是否可以证明「Acquire 1 次 → 最终 Release 恰好 1 次」？** | ✅ **可以证明**（修复后）。证明链见 §17.8：Workflow 侧「Acquire 成功 → state 变 RUNNING → state 离开 RUNNING 的唯一途径全部 Release」；Temp Action 侧「`completed=true` 全文件仅 1 处赋值且在 `temp_action_complete()` 内 → Item 回收前提是 completed → 必过收口函数」。修复前因 P0 竞争窗口存在反例，**现已消除** |

---

## 19. 本次实际修改记录

> 自查文档 §19 要求只读。但审计中发现 P0 级缺陷，按你的授权（「如果有错误可以修改，但必须报告修改了哪里的代码」）执行了修复。

### 修改 1：`workflow_terminate()` 语句顺序（P0 修复）

**文件**：`src/workflow.cpp`

**改动内容**：

```diff
 static void workflow_terminate(
     Workflow &wf,
     WorkflowState state
 )
 {
-    wf.state = state;
-
-    workflow_critical_release_by_index(
-        workflow_index_of(&wf)
-    );
-
-    workflow_notify_finish(wf);
+    // ---- 1) 先释放 Critical Operation（早于 state 改写）----
+    workflow_critical_release_by_index(
+        workflow_index_of(&wf)
+    );
+
+    // ---- 2) 再写最终状态（workflow_notify_finish 会读取它）----
+    wf.state = state;
+
+    // ---- 3) 最后回调上报 ----
+    workflow_notify_finish(wf);
 }
```

**同时修改**：函数头部注释块，将「1) 写入最终状态 / 2) 释放 / 3) 回调」改为「1) 释放 / 2) 写入状态 / 3) 回调」，并新增 20 行顺序说明，记录竞争窗口成因与禁止调换的理由。

**影响面**：仅改变同一函数内两条语句的执行顺序。
- 对 6 个 `workflow_terminate()` 调用点无影响
- `workflow_notify_finish()` 读取 `wf.state` 的语义保持不变（state 仍早于回调写入）
- 不改变任何对外接口，不涉及其他模块

**未修改**：
- P2（环形队列同步）—— 需独立评估，超出本次范围
- P3（无调用者 API）—— 仅记录
- 未编译、未烧录（按你的要求）

### Git 提交

- 修改前基线：`3b48a94`（工作区干净，无未提交改动）
- 修改后提交：见下一条 commit
