# LogManager P2-F —— Workflow 接入前审查（0918）

> 基线：HEAD `504a92c`，回归 **196/196 = 100%**。
> 审查范围：`src/workflow.cpp`（4465 行）。**本报告只做审查，未改任何生产代码。**

---

## 0. 文件命名澄清

任务描述里的 `src/workflow_manager.cpp` **不存在**。实际文件是 **`src/workflow.cpp`**（4465 行），
矩阵 §7 / Progress 用的也一直是这个名字 ⇒ 属**笔误**，不影响范围，按 `workflow.cpp` 执行。

---

## 1. Workflow 生命周期状态流转

```c
enum WorkflowState { IDLE=0, RUNNING, WAITING, FINISHED, TIMEOUT, ERROR };  // workflow.h:102-110
```

```
                 workflow_start()                     workflow_terminate()
   IDLE ──────────────────────────────► RUNNING ─────────────────────────► FINISHED / TIMEOUT / ERROR
     ▲                                     │                                        │
     │                                     │ workflow_task() 每 tick 驱动            │
     │                                     │ Trigger → SUCCESS ⇒ current_step++      │
     │                                     │ Action  → SUCCESS ⇒ current_step++      │
     └─────────────────────────────────────┴────────────────────────────────────────┘
        workflow_stop() / workflow_disable() / workflow_clear() / workflow_delete()
        （强制终止：置 IDLE，且**先 release**）
```

| 环节 | 位置 | 任务上下文 |
|---|---|---|
| 启动 | `workflow_start()` :3497 | **esp-mqtt 任务**（由 `command_manager.cpp:720` 的 execute_action 调用） |
| 驱动 | `workflow_task()` :3963（`main.cpp:583`，每 loop） | **Arduino loop 任务** |
| 收口 | `workflow_terminate()` :3644（**6 个调用点，全在 `workflow_task()` 内**） | loop 任务 |

**★ 跨任务事实（决定了埋点纪律）**：`start` 在 mqtt 任务、`terminate` 在 loop 任务，
两者可能在不同核并发 ⇒ 沿用项目铁律 **「先 Release、后置 state」**（:3617-3634 有详细论证）。

**次要发现**：`WORKFLOW_WAITING` **从未被赋值**（全文件只有 `IDLE`/`RUNNING`/`ERROR`/
`FINISHED`/`TIMEOUT` 被写入）⇒ **死枚举值**。不影响接入，仅记录。

---

## 2. Critical Op acquire / release 生命周期审计

### 2.1 三个 acquire —— 全部配对

| # | acquire 位置 | 宿主 | 持有的标志 | release 路径 | acquire 后到函数返回之间是否有 return |
|---|---|---|---|---|---|
| ① | :3530 | `workflow_start()` | `workflow_critical_held[i]=true` :3540 | `workflow_critical_release_by_index()`（**幂等**） | ✅ **无**（acquire 前有 4 个 return：:3504/:3506/:3508/:3527） |
| ② | :1711 | `workflow_mark_step_dirty()` | `wf_dirty_critical_held=true` :1719 | :1817（save 全成功）/ :1905（delete 清掉最后一个 dirty） | ✅ **无**（acquire 后只有 `return true` :1727） |
| ③ | :3767 | `enqueue_temp_action()` | `item.critical_held=true` :3859 | `temp_action_critical_release()`（**幂等**） | ✅ **无**（acquire 前有 3 个 return：:3741/:3746/:3752） |

**两个释放函数本身都是幂等的**（靠持有标志短路），所以"多路径调用同一释放"不会 underflow：

- `workflow_critical_release_by_index()` :119 —— 引用点 :3009(`workflow_clear`) / :3650(`terminate`) /
  :4338(`workflow_disable`) / :4358(`workflow_stop`)
- `temp_action_critical_release()` :216 —— 引用点 :253(`temp_action_complete`) / :3917(`cleanup` 兜底)

### 2.2 `workflow_terminate()` —— 唯一收口，**零提前 return**

```c
static void workflow_terminate(Workflow &wf, WorkflowState state)
{
    workflow_critical_release_by_index(workflow_index_of(&wf));   // ① release 是第一句
    wf.state = state;                                            // ②
    workflow_notify_finish(wf);                                  // ③
}
```

**函数体没有任何 `return`**（三个语句顺序执行到底）⇒ **不存在绕过 release 的路径**。

历史坑已修复且在注释里留了论证（:3636-3639）：release **不能**放进 `workflow_notify_finish()`，
因为它在 `finish_callback == nullptr` 时提前 return，而**事件触发/定时器触发的 Workflow 没有回调**
⇒ 那类 Workflow 会漏 release ⇒ count 永不归零 ⇒ **系统永久无法重启**。

### 2.3 6 个 terminate 调用点（全在 `workflow_task()` 内）

| # | 行 | 触发条件 | 终态 | 后面是否 `continue` |
|---|---|---|---|---|
| 1 | :3984 | `now - start_time > timeout_ms` | TIMEOUT | ✅ :3988 |
| 2 | :3993 | `current_step >= step_count` | FINISHED | ✅ :3997 |
| 3 | :4010 | Trigger 实例 `== nullptr` | ERROR | ✅ :4014 |
| 4 | :4054 | `trigger->state == TRIGGER_FAILED` | ERROR | ✅ :4059 |
| 5 | :4071 | Action 实例 `== nullptr` | ERROR | ✅ :4075 |
| 6 | :4102 | `action->result == ACTION_FAILED` | ERROR | 循环体末尾，自然结束本轮 |

### 2.4 "绕过 terminate 直接置终态"排查

逐行核验所有 `WORKFLOW_FINISHED / TIMEOUT / ERROR / RUNNING / WAITING` 出现点后：

- **唯一**一处不经 terminate 的终态赋值 = **:3331-3332 `workflow.state = WORKFLOW_ERROR;`**
  位于 `workflow_parse_json()` —— **加载/解析期**（Step0 不是 Trigger ⇒ 定义非法）。
  **判定：安全**。解析期该 Workflow 从未 start ⇒ `workflow_critical_held[i]` 必为 `false`
  ⇒ **无需 release，不存在泄漏**。
- 其余终态全部来自 `workflow_terminate(wf, STATE)` 的**参数**（:3986/:3995/:4012/:4056/:4073/:4104）。

### 2.5 不变量核验（workflow_dirty 侧）

不变量：**`wf_dirty_critical_held == true ⇒ dirty_any() == true`**。

全文件**只有 2 处**清 dirty，且**都在同一函数内紧跟 release 判定、中间无 return**：

| 清 dirty 处 | 紧随的 release 判定 |
|---|---|
| :1802 `dirty_workflow_clear(wf)`（save 循环内） | :1806 `if(!dirty_any())` → :1817 release ✅ |
| :1910 `dirty_workflow_clear(workflow_index)`（delete） | 其**之前** :1890-1908 已按 `only_this` 判定并 release ✅ |

⇒ **不变量成立，无泄漏路径。**

### 2.6 结论：是否存在永久锁死风险

**结论：当前实现无可达的永久锁死路径，Critical Op 生命周期是安全的。** 可以开始 P2-F 实现。

⚠️ 但有一个**脆弱点（不是缺陷）**，实现时**不要动它**：

```c
bool workflow_save_transaction()
{
    if(!dirty_any()) { return true; }     // ← :1732 早于 release
```

这行的安全性**完全依赖** 2.5 的不变量。若将来出现"清空 dirty 但不 release"的新路径，
则 `held==true` 而 `dirty_any()==false` ⇒ 本函数**永远在 :1732 返回** ⇒ release 变成
**不可达代码** ⇒ 永久锁死。⇒ 建议在 P2-F（以及后续）**严格保持该结构不变**。

---

## 3. ★★ 发现一个真实设计问题（**只报告，不修复**）

### 3.1 现象

`workflow_save_since_ms` 是一个"延迟保存窗口"时间戳：

```c
#define WORKFLOW_SAVE_DELAY_MS (5UL * 60UL * 1000UL)   // :1286 —— 5 分钟
static void workflow_delayed_save_poll()               // :2987，每 loop 调用一次（:3968）
{
    if(workflow_save_since_ms == 0) return;                                  // :2989
    if((unsigned long)(millis() - workflow_save_since_ms)
        >= WORKFLOW_SAVE_DELAY_MS) { workflow_save_transaction(); }          // :2994
}
```

全部写入点：

| 行 | 动作 | 说明 |
|---|---|---|
| :1294 | `= 0` | 初值 |
| :1725 | `= millis()` | `mark_step_dirty()` 首次产生 dirty 时**武装**窗口 |
| :1813 | `= 0` | **仅 save 全部成功**时解除 |
| :1853 | `= millis()` | `workflow_request_save()` 重新武装 |
| :1914 | `= 0` | delete 清空全部 dirty 时 |

**⇒ 两条失败路径不重置该时间戳：**

1. **:1744-1748 `def_buf == NULL`** → `return false`（:1747），**不重置**
2. **:1821-1830 partial**（`dirty_any()` 仍真）→ **不重置**

### 3.2 后果：失败后**每个 loop 迭代重试一次**

下次 poll 时 `millis() - workflow_save_since_ms` **仍然 ≥ 5 分钟**（因为戳记没变）⇒
`workflow_save_transaction()` **每次 loop 被调用一次**，直到故障自愈。每次执行：

- `workflow_storage_alloc_definition()` —— **≈8.6 KB 堆/PSRAM 分配**（:1741）
- 逐 dirty Workflow：`workflow_build_definition()` + `workflow_storage_save()`
  —— 后者**没有"内容未变则跳过"短路**（已核验 `workflow_storage.cpp:1253-1300`），
  每次都会 `bin_storage_mkdir()` + 真实落盘 + CRC
- 现有的 `Serial.printf("[Workflow] save transaction: FAILED (dirty kept for retry)")`（:1836）**每 loop 刷一条**

⇒ 持续性存储故障（flash 满/坏、LittleFS 异常）或 PSRAM 耗尽时 = **无界重试**：
loop 负载升高 + 高频分配/释放 + 串口刷屏。部分失败时成功者会被清 dirty，
故后续迭代只重试失败的那个（仍会做 8.6 KB 分配 + mkdir 尝试）。

> 复现手段（现有钩子）：`wfst failwf <wf>` → `workflow_storage_test_fail_wf()`
> （`workflow_storage.cpp:1528`）。⚠️ 它是**一次性**的（触发后 `s_test_fail_wf = -1`），
> 所以**无法**复现"持久失败"，但**能证明**"失败后立刻又发起一次事务"
> （正确实现应等下一个 5 分钟窗口 ⇒ 若观测到立即重试即证实未重置）。

### 3.3 为什么这是 P2-F 的**阻塞项**（必须处理）

矩阵 §7.2 计划的两条埋点**正好落在这两条每-loop 可达的路径上**：

| 计划埋点 | 位置 | 朴素埋点的后果 |
|---|---|---|
| `LOG_WF_RUNTIME_ALLOC_FAILED` | :1745（`def_buf == NULL`） | **每 loop 一条 ERROR** |
| `LOG_WF_SAVE_FAILED` / `SAVE_PARTIAL` | :1848 / :1848 partial | **每 loop 各一条 WARN** |

这会在**毫秒级**打满 64 槽 RAM 环，把真正的 WARN+ 冲掉 —— 正是 P2 一直要防的洪泛。

### 3.4 处置

**按项目规则：不修复业务逻辑（超出 P2 范围），只让埋点对该缺陷免疫。**

- `WF_RUNTIME_ALLOC_FAILED` → **1 把边沿锁**（同一失败事件最多 1 条）
- `WF_SAVE_PARTIAL` → **1 把边沿锁**（同上）
- `WF_SAVE_FAILED` → **每 slot 1 把边沿锁**（≤16 条/事件，保留"是哪几个 slot 失败"的诊断）
- 解锁点统一选在"**该 slot 保存成功**"处（:1802）与"**整事务全成功**"处（:1813）
  ⇒ 与 P2-E 的 RTC 边沿锁同一范式：**条件消失才解锁**，失败**再次出现**能重新上报。

**⇒ 业务侧问题（重试风暴）列为未决事项，等单独评审。** 见 §6。

---

## 4. 矩阵与代码的不一致（4 项，需裁决）

| # | 不一致 | 影响 | 建议 |
|---|---|---|---|
| ① | 矩阵 §7.2 说 acquire 被拒"**建议新增 `LOG_WF_STOPPED`** 或复用 `LOG_WF_FAILED`"。实际 `LOG_WF_STOPPED` **不存在**（P2 定版"暂不新增 EventId"） | 语义妥协 | **复用 `LOG_WF_FAILED`(0x0404) + `LOG_P_CAUSE`**。注意"启动被安全窗口拒绝"≠"运行失败"，已在文档标注 |
| ② | 矩阵把 Trigger 失败映射 `LOG_WF_FAILED`、Action 失败映射 `LOG_WF_ACTION_FAILED`，但 `workflow_terminate()` **只收到 `state`**（两者都是 `WORKFLOW_ERROR`）⇒ **terminate 内无法区分** | 若在 terminate 内埋点，两个 EventId 无法正确分工 | **埋点放 6 个调用点**（不是 terminate 内）。调用点还能拿到 `wf.current_step` / 具体成因 ⇒ **诊断信息更强**，且插在 `workflow_terminate()` **之前**不改变任何 return 路径 |
| ③ | `LOG_WF_SAVE_PARTIAL_RETRY_OK`(0x040C) 需要"上次是 partial"的**新观测状态**；矩阵自己标注"ID 保留不变；是否实现待 P2 决定" | 与"低侵入/不新增状态"冲突，价值边际（`SAVE_PARTIAL` 已能报出问题） | **不实现**，记为「有 ID / 需新增状态 / 本阶段不做」。与 `LOG_TIME_NTP_FAIL`、`LOG_CLOUD_FRAG_FAIL` 同处理 |
| ④ | **作用域**：矩阵 §7 把 `capability_registry.cpp`（**Registry 段 0x0B**）与 `workflow_storage.cpp` 都列在 "Workflow" 名下；但**约定的接入顺序**是 `… → BLE → Command/Event/OLED/**Registry**`，Registry 在最后且独立 | 范围可能被放大 | **P2-F 只做 `workflow.cpp`**；`capability_registry.cpp` 留到 Registry 阶段，`workflow_storage.cpp` 的独立事件留待评审。**请确认** |

---

## 5. 推荐埋点位置（含频率与限流策略）

**频率分析（决定策略）**：

| 路径 | 调用频率 | 结论 |
|---|---|---|
| `workflow_task()` | 每 loop | 埋点**只能**放状态迁移边沿 |
| `workflow_start()` / `terminate()` | 每次运行 1 次 | 可直接埋 |
| `workflow_save_transaction()` | 正常 ≤1 次/5min（有 dirty 才跑）；**故障时每 loop** | **必须边沿锁** |
| `workflow_create/update_meta/delete` | 云命令驱动，低频 | 可直接埋（每次调用 1 条） |
| `workflow_update_step_param()` / `mark_step_dirty()` | **一次云推送最多 16×16 = 256 次** | ⛔ **不埋点**（高频） |
| `capability_registry` | 独立模块 | 不在 P2-F 范围（见 §4④） |

| # | 位置 | 事件 | Level | 参数 | 策略 |
|---|---|---|---|---|---|
| 1 | `workflow_start()` 成功尾部（:3581 前） | `WF_START` | INFO | `SLOT`, `STEPS_DONE` | 每启动 1 条 |
| 2 | `workflow_start()` acquire 被拒（:3537 前） | `WF_FAILED` | WARN | `SLOT`, `CAUSE` | 每次拒绝 1 条（安全窗口内，本就很罕见） |
| 3 | terminate 调用点 :3984（超时） | `WF_TIMEOUT` | WARN | `SLOT`, `TIMEOUT_MS`, `STUCK_STEP` | 1 条 |
| 4 | terminate 调用点 :3993（完成） | `WF_FINISHED` | INFO | `SLOT`, `DURATION_MS`, `STEPS_DONE` | 1 条 |
| 5 | terminate 调用点 :4010（Trigger 实例缺失） | `WF_FAILED` | WARN | `SLOT`, `FAIL_STEP`, `CAUSE` | 1 条 |
| 6 | terminate 调用点 :4054（Trigger FAILED） | `WF_FAILED` | WARN | `SLOT`, `FAIL_STEP`, `CAUSE` | 1 条 |
| 7 | terminate 调用点 :4071（Action 实例缺失） | `WF_ACTION_FAILED` | WARN | `SLOT`, `FAIL_STEP`, `CAUSE` | 1 条 |
| 8 | terminate 调用点 :4102（Action FAILED） | `WF_ACTION_FAILED` | WARN | `SLOT`, `FAIL_STEP` | 1 条 |
| 9 | 临时 Action 超时（:4122 前） | `WF_TEMP_ACTION_TIMEOUT` | WARN | `TIMEOUT_MS`, `DURATION_MS` | 每超时 1 条 |
| 10 | `def_buf == NULL`（:1746 前） | `WF_RUNTIME_ALLOC_FAILED` | ERROR | `NEED_BYTES` | **边沿锁 ×1** |
| 11 | 逐 Workflow 保存失败（:1792 前） | `WF_SAVE_FAILED` | WARN | `SLOT`, `ERR_CODE` | **每 slot 边沿锁**（≤16/事件） |
| 12 | `dirty` 残留（:1826 前） | `WF_SAVE_PARTIAL` | WARN | `SAVED`, `TOTAL` | **边沿锁 ×1** |
| 13 | create / update_meta / delete 成功 | `WF_CRUD` | INFO | `SLOT`, `OP`, `VARIANT` | 每次调用 1 条 |
| 14 | `workflow_migrate_to_storage()` 完成 | `WF_MIGRATED` | INFO | `COUNT`, `STAGED_COUNT` | 1 条（仅在有迁移时） |

**参数纪律**：
- **不新增 EventId / ParamId**（全部复用冻结定义，已逐个核对存在）。
- 主键一律用 **`LOG_P_SLOT`(0x01)**（= `p.id`，整数）；**不用** `LOG_P_WF_ID`(0x02) /
  `LOG_P_ACTION_ID`(0x0B)（STR 语义、当前不可达）。
- 临时 Action 超时**不用** `ACTION_ID`，改用 `TIMEOUT_MS` vs `DURATION_MS` 的**超期量**表达。
- 单条最多 3 个参数（≤ `LOG_MAX_PARAMS`=8）。

**明确不做的**：
- `LOG_WF_SAVE_PARTIAL_RETRY_OK`(0x040C)（§4③）
- `workflow_update_step_param()` / `mark_step_dirty()` 内的逐 step 日志（§5 频率分析）
- 保存**成功**的逐 Workflow INFO（用户明确要求"不要大量 INFO 垃圾" ⇒ **只记失败**；
  成功由 `WF_CRUD` / `WF_MIGRATED` 与串口保留）

---

## 6. 未决事项（本阶段只记录）

| ID | 事项 | 影响 | 状态 |
|---|---|---|---|
| **WF-1** | **保存失败/分配失败后重试风暴**：`:1744-1748` 与 `:1821-1830` 两条失败路径**不重置** `workflow_save_since_ms` ⇒ `workflow_delayed_save_poll()` 每个 loop 触发一次完整保存事务（8.6 KB 分配 + 落盘尝试 + 串口刷屏），直到故障自愈 | loop 负载、内存抖动、串口洪泛；**且是埋点洪泛的根因** | ⏳ **未修**（超出 P2 范围，P2-F 只让埋点免疫）。建议单独评审：最小修法＝在两条失败路径补 `workflow_save_since_ms = millis();`（重新武装 5 分钟窗口），但这属**行为变更**，需独立提交与回归。<br>**★ 实测补充（P2 实施阶段）**：两次独立观测证实 —— **151 s / 5828 次**、**84 s / 3399 次**（≈**39–40 次/秒**，串口 ≈77 行/秒）。触发条件已明确：**控制台 `wfc create` 建出的"不完整定义"永远过不了校验（`INVALID_ARGUMENT`）** ⇒ 一旦它有 Dirty，5 分钟延迟窗口过后就进入永久风暴。边沿锁已使埋点在风暴期新增记录 **0 条**（Guide §15.5） |
| WF-2 | `WORKFLOW_WAITING` 从未被赋值（死枚举值） | 无功能影响 | ⏳ 记录 |
| WF-3 | `workflow_task()` 临队尾分支 `if(!item.completed) break; else break;`（:4201-4210）两分支相同、注释写反 ⇒ 实际每 tick 只处理 1 个临时 Action | 无功能影响（有 `cleanup()` 兜底） | ⏳ 记录 |
| WF-4 | 矩阵 §7 作用域含 `capability_registry.cpp`（Registry 段）与 `workflow_storage.cpp` | 范围界定 | ⏳ **待确认**（建议按约定顺序留到各自阶段） |

---

## 7. 审查结论

1. **Critical Op 生命周期安全**：3 个 acquire 全部配对、两个释放函数幂等、
   `workflow_terminate()` 零提前 return、6 个调用点全覆盖、
   **无可达的绕过路径，无永久锁死风险** ⇒ **可以开始 P2-F 实现**。
2. **发现一个真实设计问题（WF-1，重试风暴）**，与 Critical Op 无关，
   **只报告不修复**；但**埋点必须对它免疫**（边沿锁），否则会引入洪泛。
3. **4 项矩阵/代码不一致已列出**，其中③④需要你确认取舍。
4. 埋点方案：**14 个位置 / 11 个冻结 EventId / 零新增 EventId·ParamId / 主键用 SLOT**，
   高频成功路径不埋、保存失败路径全部边沿锁。
