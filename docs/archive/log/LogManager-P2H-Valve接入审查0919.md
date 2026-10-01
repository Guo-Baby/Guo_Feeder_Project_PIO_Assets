# P2-H Valve / DispenseGuard 接入前审查报告

> 日期：2026-09-19
> 阶段：P2-H（Valve）**接入前审查** —— **未修改任何生产代码、未创建 commit**
> 审查对象：`src/valve.cpp`(350) / `src/valve.h`(42) / `src/dispense_guard.cpp`(73) / `src/dispense_guard.h`(2)
> 依据：仓库 `docs/P2_Log_Integration_Matrix.md` §9、`src/log_events.h`、`docs/LogManager-Integration-Guide.md`

---

## 0. 结论速览

| 项 | 结论 |
|---|---|
| **A. `LOG_VALVE_FORCE_CLOSE`** | ⚠️ **当前不可直接埋点**。`valve_force_close()` 是**纯事件**（无状态判等，已关闭也会全量执行）。实测**每个异常事件产生 2 次调用**，B2 会话 **50 次 / 175 s**，峰值 **6 次/s** ⇒ **必须先定去重策略** |
| **B. 状态机完整性** | 🛑 **STOP —— 矩阵隐含的 6 个状态有 4 个不存在**。实际只有 `bool current_state` 一个二值量，无 OPENING / CLOSING / ERROR / FORCE_CLOSE |
| **C. Critical Op / 安全路径** | 🛑 **发现 1 处安全语义缺陷（VALVE-1）**：`valve_force_close()` / `valve_open()` / `valve_close()` **均不检查 `initialized`** ⇒ 模块禁用或未完成 init 时**返回 `true`（假成功）但 GPIO 从未配置**。另发现 **VALVE-2**（安全超时重复 push） |
| **D. EventId** | 🛑 **发现 3 处矩阵与代码不一致**（`FORCE_CLOSE_FAILED` 有失败分支 / "IMM" 语义与实现不符 / 建议新增 `NOT_READY` 违反冻结）。7 个 VALVE EventId **全部存在于 `log_events.h` 且全部无宿主**；**未新增任何 EventId** |
| **E. 问题登记** | 新增 **VALVE-1..VALVE-5**、**R-6**、`NC-11..NC-13` 共 **9 项**（详见 §E，已同步 `未修复的问题.md`） |

> **一句话**：Valve 段**不能照矩阵直接埋点**。必须先拍板 §「决策清单」的 **D1（去重）** 与 **D2（VALVE-1 是否本轮修）**，且 **B 项 4 个状态不存在**需你确认"按实际状态机重写矩阵 §9"还是"维持现状只埋事件"。

---

## A. `LOG_VALVE_FORCE_CLOSE` 语义审查

### A.1 是事件还是状态迁移？

**答：纯事件，不是状态迁移。**

`src/valve.cpp:321-351`：

```c
bool valve_force_close()
{
    if(valve_pin < 0) { return false; }
    int level = active_level ? 0 : 1;
    gpio_set_level((gpio_num_t)valve_pin, level);   // 无条件写
    current_state = false;                           // 无条件赋值
    valve_update_state();
    open_start_time = 0;
    event_push(EVENT_VALVE_CLOSE, "force close", "valve", EVENT_PRIORITY_CRITICAL, EVENT_POLICY_STATE, 0);
    Serial.printf("[Valve] FORCE CLOSE (pin=%d, level=%d)\n", valve_pin, level);
    return true;                                     // 除 valve_pin<0 外恒为 true
}
```

与状态迁移型（`valve_set_gpio()` :92）对比：

| | `valve_set_gpio()`（状态迁移型） | `valve_force_close()`（事件型） |
|---|---|---|
| 状态判等 | ✅ `if (open == current_state) return;`（:97）⇒ **天然边沿去重** | ❌ **无判等**，已关闭也全量执行 |
| 防风暴 | ✅ 50 ms 冷却（:103） | ❌ 故意绕过（设计如此） |
| 可重复次数 | 状态不变 ⇒ 0 次 | **无限次** |

⇒ **调用一次就执行一次**，包括"阀门本来就是关的"这种 no-op 场景。

### A.2 当前调用频率（★ 本次审查最重要的定量结论）

**唯一调用点**：`src/dispense_guard.cpp:37`（全仓仅此一处，已 grep 确认）。
触发源：`dispense_guard` 订阅 `EVENT_WEIGHT_ERROR`（`dispense_guard.cpp:58`）。

**`EVENT_WEIGHT_ERROR` 的 4 个发布点**（`src/weight.cpp`）：

| # | 位置 | 条件 | 是否有节流 | 实测贡献 |
|---|---|---|---|---|
| ① | `:269` `weight_record_jump()` | 相邻窗口跳变 ≥50 g | ❌ **无节流**（每窗口最多 1 次） | 主要来源 |
| ② | `:385` not-ready | HX711 连续 5 s 无数据 | ✅ 5 s 一次 | 次要 |
| ③ | `:523` `weight_trigger_start()` | 起手时 `STATE_WEIGHT_ERROR==true` | ❌ 每 trigger 一次 | 取决于 Workflow |
| ④ | `:544` `weight_trigger_start()` | `gram<=0 或 >300` 非法参数 | ❌ 每 trigger 一次 | 取决于 Workflow |

**硬性上限**：`event_manager.cpp:9-10`
```c
#define EVENT_STORM_WINDOW_MS    500
#define EVENT_STORM_MAX_PER_EVENT 5
```
⇒ `EVENT_WEIGHT_ERROR` **每 500 ms 最多 5 条 ⇒ 10 条/秒**。

**上板实测（复用 P2-G 既有会话日志，未重新烧录）**：

| 会话 | FORCE CLOSE 条数 | 会话时长 | 说明 |
|---|---|---|---|
| A | 2 | ~60 s | 静置 |
| C | 2 | ~60 s | 静置 |
| D | 4 | ~90 s | 静置 |
| E | 2 | ~90 s | 静置 |
| **B2** | **50** | **~175 s** | **含 Flash 填充（FS 阻塞诱发重量异常）** |
| 本次只读观测 | **0** | 90 s | 静置、无出水（`.pio/p15run/valve_watch.log`） |

**★ 实测关键形态 —— 严格成对（×2）**：
```
[00:02:10.214] [DispenseGuard] Weight error received
[00:02:10.214] [Valve] FORCE CLOSE (pin=12, level=0)
[00:02:10.214] [DispenseGuard] Valve force closed
[00:02:10.214] [DispenseGuard] Weight error received     ← 第二次，同一毫秒
[00:02:10.214] [Valve] FORCE CLOSE (pin=12, level=0)
[00:02:10.214] [DispenseGuard] Valve force closed
```
B2 的 50 条 = **25 对 × 2**，每一对都在同一毫秒、中间夹完整的 guard 打印序列。

**峰值**（B2）：
```
00:02:10.214 / 00:02:10.617 / 00:02:11.116 / 00:02:11.547   → 4 对 / 1.33 s
⇒ 事件 3 次/s ⇒ force_close 6 次/s
```

**已排除的 ×2 成因**（逐项核实）：
- ❌ 重复注册：`event_subscribe()` 有防重复注册（`event_manager.cpp:299-306`），且串口 `[DispenseGuard] Init OK` 只出现 1 次
- ❌ `remove_event()` 缺陷：实现正确（前移覆盖 + `queue_count--`，`:139-146`）
- ❌ dispatch 级联：`valve_force_close()` 内 push 的是 `EVENT_VALVE_CLOSE`，与 `EVENT_WEIGHT_ERROR` 不同，且 `dispense_guard` 不订阅它
- ❌ `event_dispatch()` 单点调用：全仓仅 `main.cpp:576`，在 loop 任务上下文

⇒ **×2 成因未定位**，记为 **R-6**（见 §E）。**但无论成因如何，"一个异常事件 ⇒ 2 条 CRITICAL 记录"是板上稳定复现的事实**。

**频率汇总**：

| 口径 | 数值 |
|---|---|
| 实测常态（静置） | 0 ~ 0.05 次/s |
| 实测峰值（B2） | **6 次/s** |
| 理论上限（风暴抑制 × ×2） | **20 次/s** |

### A.3 是否存在已有状态变量可作为边沿锁？

| 候选 | 位置 | 可用性 |
|---|---|---|
| **`current_state`**（`static bool`） | `valve.cpp:31` | ✅ **存在且可直接用**。但语义有代价：加 `if (!current_state) return true;` 会**改变现有"无条件强制写 GPIO"的设计意图**（原作者注释 `valve.cpp:318`："不判断当前状态 / 强制同步 GPIO"）⇒ 需你拍板 |
| `open_start_time` | `valve.cpp:36` | ⚠️ 可用作"本次开阀时长"，但 force_close 里被清零，不能单独当锁 |
| `STATE_VALVE_STATUS` | `system_state.h:31` | ⚠️ 是 `current_state` 的镜像（`valve_update_state()`），等价 |
| `last_operation_time` | `valve.cpp:39` | ❌ 只被 `valve_set_gpio()` 维护，force_close **故意绕过** |
| `initialized` | `valve.cpp:32` | ⚠️ 存在但**当前未被 force_close 使用**（这正是 VALVE-1） |

⇒ **有现成边沿锁（`current_state`），但用它等于改语义**；也可另加一个"上次 force-close 的 cause + 时间戳"静态只读量（P2-G 的 `ERROR_EXIT` 就是这么做的：加 2 个**只读**观测变量、不改状态机）。

### A.4 是否需要去重 / 聚合？

**需要。** 三条量化理由：

1. **云队列**：`LOG_CLOUD_QUEUE_SLOTS = 128`（`log_manager.h:61`）。CRITICAL 也走 `cloud_queue_push()`（`log_manager.cpp:1746` 附近，WARN+ 是 Cloud YES）⇒ **峰值 6 条/s ⇒ 21 s 填满**；理论上限 20 条/s ⇒ **6.4 s 填满**，之后开始 `drop_overflow` 淘汰历史。
2. **Flash 环**：16 段 × 31 条 = **496 条**。CRITICAL 落盘（`log_level_to_flash(CRITICAL)==true`）⇒ **峰值 83 s 写满一整轮**（把历史全淘汰）；理论上限 **25 s**。
   ⇒ 也就是说：**一次真实的出水异常，可以在一分半钟内把设备全部历史日志冲光**。
3. **语义冗余**：成对 ×2 里有一半是**对已关闭阀门的 no-op**，记录下来是纯噪声。

### A.5 CRITICAL + IMM 的实际写入行为（★ 与矩阵描述不符）

矩阵 §9 写："安全事件，**必须立即 flush**"。

**实际实现**（`log_manager.cpp:1683-1702`）：

```c
const bool is_critical = (lv == LOG_LVL_CRITICAL);
...
if (is_critical)
{
    s_stats.critical_seen++;
    // 冻结语义（P1 修订）：CRITICAL = Flash 立即 + 进入
    // "最高优先级下一批次"，**不建独立通道、不承诺秒级**。
    // 边沿触发：只置一次，避免重复 flush。
    if (!s_flush_requested) { s_flush_requested = true; s_stats.flush_requests++; }
}
```

⇒ **"IMM" 的真实含义 = 置一次 `s_flush_requested` 标志**，本轮 `log_task()` 阶段 2 就把这批落盘；**它不是独立通道、不绕过 RAM 环、不绕过云队列、不承诺秒级上报**。

⇒ **推论**：CRITICAL **不能防住 A.4 的队列/Flash 冲爆**。把 `LOG_VALVE_FORCE_CLOSE` 标成 CRITICAL **不会**让它更安全，只会让它**优先落盘**。**去重仍然必须由调用侧（Valve 模块）做**。

---

## B. Valve 状态机完整性（🛑 STOP 项）

你要求核对的 6 个状态，与代码实际对照：

| 矩阵隐含状态 | 代码实际 | 证据 |
|---|---|---|
| **OPEN** | ✅ 存在（但**不是状态变量**，是 `current_state == true`） | `valve.cpp:31` `static bool current_state` |
| **CLOSE** | ✅ 存在（`current_state == false`） | 同上 |
| **OPENING** | ❌ **不存在** | 全仓无此概念；开阀是**瞬时**的（`gpio_set_level` 后即完成，无动作时长状态） |
| **CLOSING** | ❌ **不存在** | 同上 |
| **ERROR** | ⚠️ **只有事件、没有状态**。`EVENT_VALVE_ERROR` 仅在安全超时时 push（`valve.cpp:310`），**无任何状态变量记录"阀门处于错误"** | `system_state.h` 中只有 `STATE_VALVE_STATUS`（bool），无 VALVE_ERROR |
| **FORCE_CLOSE** | ❌ **不存在**。`valve_force_close()` 执行完把 `current_state` 置 false，与"普通关闭"**在状态上完全无法区分** | `valve.cpp:331` |

**实际存在的全部"状态"**：

```
static bool current_state;        // valve.cpp:31  —— 二值：false=关 / true=开
STATE_VALVE_STATUS  (bool)        // system_state.h:31 —— current_state 的镜像
unsigned long open_start_time;    // valve.cpp:36  —— 0=未开启，非0=本次开启时刻
bool initialized;                 // valve.cpp:32  —— 但未被 force_close/open/close 检查（VALVE-1）
```

**无 enum、无状态机变量、无 OPENING/CLOSING/ERROR/FORCE_CLOSE。**

🛑 **按你的规则「如果矩阵中的状态不存在，停止并报告」—— 在此停止，等待你的决策**（见决策清单 **D3**）。

---

## C. Critical Op / 安全路径审查

### C.0 是否接入 CriticalOperation（Safe Restart）

**未接入，且不应接入。** 全仓 acquire 点只有 `config_manager.cpp:1628` / `time_manager.cpp:637,978` / `workflow.cpp:1771,3704,3965`。阀门操作与重启无关，`valve.cpp` / `dispense_guard.cpp` 中 `critical_op` 零引用 ⇒ **符合预期，不是缺陷**。

### C.1 `valve_force_close()`（`valve.cpp:321`）

| 检查项 | 结果 |
|---|---|
| 提前 return | ✅ 有 1 处（`valve_pin < 0`），但 **该分支完全不可观测**（无日志、无事件，`dispense_guard` 只打一行串口）⇒ **VALVE-3** |
| 未释放资源 | ✅ 无（无锁、无缓冲、无 acquire） |
| 重入风险 | ✅ **无跨任务重入**：`dispense_guard_event_callback` 由 `event_dispatch()` 调用（`main.cpp:576`），在 **loop 任务**；`event_dispatch()` 单次最多 4 条 ⇒ 一轮 loop 最多 4 次 force_close |
| 高频调用风险 | 🛑 **有**，见 A.2（峰值 6 次/s，理论 20 次/s） |
| **🛑 不检查 `initialized`** | **VALVE-1**：`valve_init()` 在 `enable==false`（:198）或 `valve_pin<0`（:204）时提前 return，而 `valve_pin` 已在 :187 赋值。此后 `valve_force_close()` 只查 `valve_pin<0` ⇒ **对未 `gpio_config()` 的引脚执行 `gpio_set_level()` 并返回 `true`** ⇒ **调用方误以为关阀成功**。同理影响 `valve_open()`(:244) / `valve_close()`(:254) / `valve_toggle()`(:265) |

### C.2 `valve_close()`（`valve.cpp:252`）

| 检查项 | 结果 |
|---|---|
| 提前 return | `valve_pin < 0` ⇒ 同 VALVE-1 家族 |
| **返回值语义不精确** | 返回 `!current_state`。若阀门**本来就是关的**（例如刚 force_close 过），返回 `true` 但**什么都没做** ⇒ 与注释"返回 true 表示成功"不符 ⇒ **VALVE-4**（会影响 `LOG_VALVE_CLOSE` 的语义："成功"应区分"真的关了一次" vs "本来就是关的"，可用 `LOG_P_WAS` 表达） |
| 防风暴静默跳过 | `valve_set_gpio()` 因 50 ms 冷却 return 时，`valve_close()` 仍按当前状态返回 ⇒ 同上，静默 no-op |

### C.3 `dispense_guard_event_callback()`（`dispense_guard.cpp:23`）

| 检查项 | 结果 |
|---|---|
| 提前 return | ✅ 只订阅 `EVENT_WEIGHT_ERROR`，非目标事件立即 return |
| 未释放资源 | ✅ 无 |
| 重入风险 | ✅ 无（loop 单线程） |
| 高频调用风险 | 🛑 **有** —— 它无条件把每个 `EVENT_WEIGHT_ERROR` 转成一次 force_close，**无任何节流/去重/状态判等** ⇒ 这是 A.2 频率问题的**根因所在**（放大环节） |
| **在 dispatch 循环内 push 事件** | ⚠️ `event_dispatch()` 的 `remove_event()` 已完成、`dispatch_msg` 是拷贝 ⇒ 安全；但新 push 的 `EVENT_VALVE_CLOSE`（CRITICAL）可能在**同一轮 dispatch** 被取出 ⇒ 同轮级联，可接受（无订阅者） |

### C.4 `valve_task()` 安全超时（`valve.cpp:305`）—— **VALVE-2**

```c
if (now - open_start_time >= safety_timeout_ms) {
    Serial.printf("[Valve] Safety timeout! Forced close ...\n");
    valve_set_gpio(false);                                    // ← 可能因 50ms 防风暴被跳过
    event_push(EVENT_VALVE_ERROR, "Safety timeout", ...);     // ← 无论是否真的关掉都 push
}
```

**缺陷**：`open_start_time` 只在 `valve_set_gpio(false)` **成功**时才清零（`:123`）。若本次被防风暴（50 ms）跳过 ⇒ `open_start_time` 保持旧值 ⇒ **下一轮 loop 立即再次进入超时分支** ⇒ 连续多轮重复 `Serial.printf` + `event_push(EVENT_VALVE_ERROR)`。
防风暴窗口 50 ms、loop 无 delay ⇒ **一次真实超时可产生几十条重复事件/打印**。

⇒ **若把 `LOG_VALVE_SAFETY_TIMEOUT` 埋在 `valve_task():305`，会得到同一事件的重复记录**。必须先加"超时已处理"一次性锁，或改埋在 `valve_set_gpio()` 真正完成关闭处。

---

## D. EventId 审查（🛑 发现 3 处不一致）

### D.1 定义存在性

`src/log_events.h:269-275` —— **7 个 VALVE EventId 全部已定义，未新增任何 EventId** ✅

| EventId | 值 | Level | 宿主（有无真实调用点） |
|---|---|---|---|
| `LOG_VALVE_FORCE_CLOSE` | 0x0505 | CRITICAL | ❌ **无宿主**（`valve.cpp` 中 `log_emit` 计数 = **0**）→ 宿主 = `valve.cpp:321` ⚠️ 需先去重 |
| `LOG_VALVE_FORCE_CLOSE_FAILED` | 0x0506 | CRITICAL | ❌ 无宿主 → 宿主 = `valve.cpp:323`（`valve_pin<0`）✅ **可埋**（推翻矩阵结论，见 D.2①） |
| `LOG_VALVE_OVERFLOW_RISK` | 0x0507 | CRITICAL | ❌ **无宿主且无检测代码** ⇒ 沿用 P2 惯例"无宿主就不埋"（= 既有 `P0-4`） |
| `LOG_VALVE_SAFETY_TIMEOUT` | 0x0508 | WARN | ❌ 无宿主 → 候选 `valve.cpp:305` ⚠️ 有重复 push 问题（VALVE-2） |
| `LOG_VALVE_OPEN` | 0x0509 | INFO | ❌ 无宿主 → 候选 `valve_set_gpio():118` ✅ **天然边沿去重，可直接埋** |
| `LOG_VALVE_CLOSE` | 0x050A | INFO | ❌ 无宿主 → 候选 `valve_set_gpio():121` ✅ **同上，可直接埋** |
| `LOG_VALVE_RATE_LIMITED` | 0x050B | WARN | ❌ 无宿主 → 候选 `valve_set_gpio():103` ✅ **可直接埋**（见 D.2③） |

`grep -c log_emit src/valve.cpp src/dispense_guard.cpp` = **0 / 0** ⇒ 确认 Valve 段**完全未接入**。

**ParamId 全部存在、无需新增**（`LOG_P_MAX = 0x59` 未被触碰）：

| ParamId | 值 | 用途 |
|---|---|---|
| `LOG_P_CAUSE` | 0x16 | force_close 成因 |
| `LOG_P_VALVE_OPEN_MS` | 0x48 | 本次开阀时长 |
| `LOG_P_GAIN_AFTER_CLOSE_G` | 0x49 | 关阀后残余增重 |
| `LOG_P_STATE` / `LOG_P_WAS` | 0x44 / 0x47 | 目标态 / 变化前态 |
| `LOG_P_LIMIT_MS` | 0x14 | 防风暴窗口 |
| `LOG_P_ERR_CODE` | 0x0C | 失败码 |

### D.2 🛑 矩阵与代码不一致（3 处）

**① 矩阵说"`valve_force_close()` **永远返回 true**（无失败分支）⇒ `FORCE_CLOSE_FAILED` 无处可埋" —— ❌ 错**

证据 `valve.cpp:323`：
```c
if(valve_pin < 0)    {        return false;    }
```
⇒ **有失败分支**。`LOG_VALVE_FORCE_CLOSE_FAILED` **可以埋**（`CAUSE = 1` 表示 `valve_pin<0`；若采纳 VALVE-1 的修复，还可区分 `CAUSE = 2` 未初始化）。
（另：这条失败分支**当前完全不可观测** ⇒ VALVE-3）

**② 矩阵说 CRITICAL（IMM）"**必须立即 flush**" —— ❌ 与实现不符**

实际是"**置一次 `s_flush_requested` 标志**"，**不建独立通道、不绕过 RAM 环（64 槽）、不绕过云队列（128 槽）、不承诺秒级**（`log_manager.cpp:1683-1702`，原文注释即为"不承诺秒级"）。
⇒ **矩阵这句会误导实现者以为"标成 CRITICAL 就安全了"**，实际 A.4 的冲爆风险依然存在。**建议修正矩阵 §9 该单元格**。

**③ 矩阵说 `LOG_VALVE_RATE_LIMITED`"⚠️ 高频调用源会重复触发，**需边沿/计数去重**" —— ❌ 担忧不成立**

证据：`valve_set_gpio()` 的**顺序**是
```c
if (open == current_state) return;   // :97   ← 先判等
if (now - last_operation_time < 50) { ... return; }  // :103  ← 后防风暴
```
⇒ 防风暴检查**只在"真的要切换状态"时才执行**。要在 50 ms 内触发限流，必须有人**连续切换开→关→开**。
实测：**所有 11 个既有会话中 `Operation too frequent` 出现次数 = 0**（A/B/B2/C/D/E/P2G/P2G2/P2G3/P2G5/P2G6 全为 0）。
⇒ **可直接埋，不需要去重**。

### D.3 🛑 无宿主的 EventId（按 P2 惯例：不埋、只登记）

| EventId | 状态 | 处置建议 |
|---|---|---|
| `LOG_VALVE_NOT_READY` | **矩阵"建议新增"，但 `log_events.h` 中不存在** | 🛑 **新增 EventId 已被你明确禁止** ⇒ **不埋**。候选替代：`valve_init()` 的禁用/未配置分支改用 `LOG_VALVE_CLOSE`+`CAUSE`？语义牵强 ⇒ **建议直接不埋，登记为缺口**（NC-11） |
| `LOG_VALVE_OVERFLOW_RISK` | 已定义，**但代码中无任何"关阀后残余增重"检测** | **不埋**（= 既有 `P0-4`，需单独评审安全逻辑） |
| `valve_init()` `:227/:231` 注册失败 | 矩阵自己标"⚠️ EventId 缺口"，建议借用 `LOG_WF_RUNTIME_ALLOC_FAILED` + `LOG_P_ACTION_ID` | 🛑 **不同意借用**：该 EventId 属 Workflow 段语义（"runtime 分配失败"），Valve 的 action **注册**失败不是运行时分配失败，参数也不匹配 ⇒ **建议不埋，登记为缺口**（NC-12） |

---

## E. 新增问题登记（已同步 `未修复的问题.md`）

| 编号 | 标题 | 证据 | 影响 | 建议修复方案 | 复测方法 |
|---|---|---|---|---|---|
| **VALVE-1** 🔴 | `valve_force_close()` / `valve_open()` / `valve_close()` / `valve_toggle()` **均不检查 `initialized`**，对未 `gpio_config()` 的引脚写电平并**返回 `true`** | `valve.cpp:187` 先赋值 `valve_pin`；`:198`/`:204` 提前 return 时 `initialized` 仍为 false；`:323`/`:244`/`:254`/`:265` 只查 `valve_pin<0` | **安全语义缺陷**：模块被 config 禁用时，"强制关阀"返回成功但**阀门实际没被驱动**，调用方（`dispense_guard`）会打印 "Valve force closed" 误报 | 在四个函数的守卫条件中补 `!initialized`（或抽一个 `valve_ready()` 内联判定）。**属生产代码改动 ⇒ 需你单独批准** | `config_set valve/enable=false` + `config_save`（重启）→ 发 `EVENT_WEIGHT_ERROR` → 观察是否仍打印 "Valve force closed"；修复后应打印失败 |
| **VALVE-2** 🟠 | 安全超时分支**重复 push** `EVENT_VALVE_ERROR` | `valve.cpp:305-311`：`open_start_time` 只在 `valve_set_gpio(false)` 成功时清零（`:123`）；被 50 ms 防风暴跳过则保持不变 ⇒ 下一轮 loop 再次进入 | 一次真实超时产生**几十条**重复事件/打印；若直接埋 `LOG_VALVE_SAFETY_TIMEOUT` 会得到重复记录 | 加一次性锁（如 `safety_tripped` 静态标志），或把埋点移到"真正完成关闭"处 | 把 `safety_timeout_sec` 调成 10 s → 开阀 → 等 10 s → 统计 `EVENT_VALVE_ERROR` 条数（应 = 1） |
| **VALVE-3** 🟠 | `valve_force_close()` 的**失败分支完全不可观测** | `valve.cpp:323` 返回 false 后无日志、无事件；`dispense_guard.cpp:46` 只 `Serial.println` | 最危险的场景（关阀失败）在云端/Flash 中**不留任何痕迹** | 埋 `LOG_VALVE_FORCE_CLOSE_FAILED`（CRITICAL）+ `LOG_P_CAUSE` | 同 VALVE-1 复测；另可用 `config_set valve/gpio_pin=-1` 制造 `valve_pin<0` |
| **VALVE-4** 🟡 | `valve_close()` / `valve_open()` 返回值语义不精确 | `valve.cpp:257` 返回 `!current_state`；阀门本来就是关的也返回 `true` | 埋 `LOG_VALVE_CLOSE` 时无法区分"真的关了一次" vs "no-op" | 埋点带 `LOG_P_WAS`（变化前状态）即可表达，**不必改返回值** | 连续两次 `valve_close` 串口命令，观察 `LOG_P_WAS` |
| **VALVE-5** 🟠 | `dispense_guard` **无条件**把每个 `EVENT_WEIGHT_ERROR` 转成一次 force_close，无节流/去重/判等 | `dispense_guard.cpp:36-37` | 这是 A.2 频率问题的**放大环节**；配合 ×2 使 CRITICAL 记录可达 6~20 条/s | 见决策 D1 | 见决策 D1 的验证脚本 `.pio/p15run/valve_watch.py` |
| **R-6** 🟠 | `valve_force_close()` **成对调用（×2）**，成因未定位 | B2 会话 50 条 = 25 对，每对**同毫秒**；已排除重复注册 / `remove_event` 缺陷 / dispatch 级联 / dispatch 多点调用 | 使所有基于 `EVENT_WEIGHT_ERROR` 的动作**翻倍**；若属 EventManager 通用缺陷，影响**所有订阅者** | 先定位：在 `dispense_guard_event_callback` 入口打印 `sub_count` 与队列快照，或在 `event_dispatch()` 加一次性调试计数。**属 EventManager 范畴，建议与 P2-H 解耦、单独评审** | 跑 B2 段并统计 `FORCE CLOSE` 的配对率（当前 100%） |
| **NC-11** ⚪ | `LOG_VALVE_NOT_READY` **未定义**（矩阵"建议新增"） | `log_events.h` 无此符号 | 阀门被禁用/引脚未配置时**无日志信号** | **禁止新增 EventId** ⇒ 不埋，登记缺口 | — |
| **NC-12** ⚪ | `valve_init()` Action 注册失败**无合适 EventId** | `valve.cpp:227-232` 只打串口 | 注册失败不可观测 | 不同意借用 `LOG_WF_RUNTIME_ALLOC_FAILED`（语义不符）⇒ 不埋 | — |
| **NC-13** ⚪ | `LOG_VALVE_OPEN` / `LOG_VALVE_CLOSE` / `LOG_VALVE_RATE_LIMITED` / `LOG_VALVE_SAFETY_TIMEOUT` **上板验证未做** | Valve 段 `log_emit` 计数 = 0，尚未接入 | — | 实现阶段用 `valve_open` / `valve_close` / `valve_toggle` 串口命令触发（无 Workflow） | 串口发 `valve_open` → 应出现 `LOG_VALVE_OPEN` |

---

## 决策清单（请逐项拍板）

**D1 — `LOG_VALVE_FORCE_CLOSE` 的去重策略（必选，阻塞实现）**

| 选项 | 做法 | 代价 |
|---|---|---|
| **A（推荐）** | **加 2 个只读静态量做"cause 变化 或 冷却窗口"门控**：仅在「距上次 force-close 记录 ≥ T（建议 5 s）」**或**「CAUSE 发生变化」时 emit，其余只累加计数并在下一条 emit 里带上"被折叠条数" | 极端突发下仍可能漏记细节；但完全不改动状态机、不改 GPIO 行为 |
| **B** | **用 `current_state` 做边沿锁**：`if (!current_state) { /*只计数*/ } else { emit(); }` | 会**丢掉"对已关闭阀门的 force close"**（当前设计认为它是有意义的强制同步）；且**改变现有语义**（`valve.cpp:318` 注释明确写"不判断当前状态"） |
| **C** | 不改调用侧，**只在 Log 层做计数聚合**（类比 Cloud 的 publish 失败聚合） | LogManager 目前**没有**通用聚合机制，需新增 ⇒ 超出 P2 范围 |

**D2 — VALVE-1（`initialized` 未检查）是否纳入 P2-H 实现阶段？**
- A：本轮**只审查不修**（P2-H 保持"纯增量埋点"），VALVE-1 登记待单独评审
- B：本轮**顺带修**（会在 `valve.cpp` 产生删除/修改行，违背"纯增量"）

**D3 — B 项 STOP：矩阵 6 状态 vs 实际 2 状态**
- A：**按实际代码重写矩阵 §9**（只保留 OPEN/CLOSE 两个事件点 + `current_state` 二值），不虚构状态
- B：**维持矩阵**，实现时只埋"事件"不埋"状态迁移"

**D4 — `LOG_VALVE_SAFETY_TIMEOUT` 的重复 push（VALVE-2）**
- A：实现时**在 `valve_task()` 加一次性锁**（新增 1 个静态只读标志，P2-G 同款做法）
- B：**改埋在 `valve_set_gpio()` 真正完成关闭处**（但这会与"普通关闭"混淆，需靠 `LOG_P_CAUSE` 区分）
- C：**本轮不埋** `SAFETY_TIMEOUT`，登记待修

**D5 — `LOG_VALVE_FORCE_CLOSE_FAILED` 是否埋？**
- A：**埋**（推翻矩阵"无失败分支"结论；`valve_pin<0` 是真实失败分支）
- B：不埋

**D6 — `LOG_VALVE_NOT_READY` / `valve_init()` 注册失败（NC-11 / NC-12）**
- A：**均不埋**，登记缺口（与 P2-E `NTP_FAIL`、P2-D `FRAG_FAIL` 的"无宿主就不埋"惯例一致）
- B：指定一个已有 EventId 复用（请指明）

**D7 — R-6（force_close ×2）是否在 P2-H 定位？**
- A：**不在本轮定位**（与 Log 解耦，登记待单独评审；去重方案 D1 可先把影响压住）
- B：本轮一并定位（需改 `event_manager.cpp` 加调试，超出 Valve 范围）

---

## 附：本次审查用到的只读证据

| 文件 | 说明 |
|---|---|
| `.pio/p15run/valve_watch.py` | **新建**：只读串口观测器（90 s，**未烧录、未改生产代码**） |
| `.pio/p15run/valve_watch.log` | 本次静置观测结果：**FORCE CLOSE = 0** |
| `.pio/p15run/{A,B,B2,C,D,E,P2G*}.log` | P2-G 既有会话日志（复用统计，未重跑） |
| `docs/P2_Log_Integration_Matrix.md` §9 | 矩阵原文（已发现 3 处与代码不一致） |

---

# 【P2-H 实现与验证结果】（2026-09-19 追加）

> 本节为审查报告的执行结果。**七项决策全部按 A 执行**（D1~D7）。
> 生产代码改动：**仅 `src/valve.cpp`，净增量 `+125 / −1`**。
> `src/log_events.h` / `src/event_manager.cpp` / `src/event_manager.h` / `src/valve.h` / `src/dispense_guard.cpp`
> **全部零改动**；**未新增任何 EventId / ParamId**（`LOG_P_MAX = 0x59` 未被触碰）。

## 1. 决策执行对照

| 决策 | 执行结果 |
|---|---|
| **D1=A** FORCE_CLOSE 用 cause 变化 / 5 s 冷却窗口去重，**只限制日志、不改 GPIO 行为** | ✅ 门控加在 `valve_force_close()` **全部执行完成之后**（GPIO → 状态 → SystemState → `open_start_time` → 事件 → Serial 之后），**未写 `if(!need_log) return;`**，**未改成 `current_state` 边沿判断** |
| **D2=A** VALVE-1 本轮不修，只登记 | ✅ `src/valve.cpp` 未补 `initialized` 检查；`未修复的问题.md` 保留 `VALVE-1`（🔴） |
| **D3=A** 按真实代码模型重写矩阵，不虚构状态 | ✅ `docs/P2_Log_Integration_Matrix.md` §9 **已整节重写**（状态只有 OPEN/CLOSE 二值；事件 6 类；另附"原矩阵 3 处错误"对照表） |
| **D4=A** SAFETY_TIMEOUT 增加一次性报告锁后再埋点 | ✅ `static bool safety_timeout_reported`；**原有 `event_push` 保留不动**，只有 `log_emit` 受锁保护；锁在"真实开启/关闭成功"时解除 |
| **D5=A** 埋 `LOG_VALVE_FORCE_CLOSE_FAILED` | ✅ 埋于 `valve_pin < 0` 失败分支，`CAUSE=1`；**不等 VALVE-1 修复** |
| **D6=A** NOT_READY / init 注册失败不埋；不新增 / 不借用 EventId | ✅ 均未埋；矩阵中"建议新增 `LOG_VALVE_NOT_READY`"与"借用 `LOG_WF_RUNTIME_ALLOC_FAILED`"均**未采纳**（登记 `NC-11`/`NC-12`） |
| **D7=A** R-6（force_close ×2）本轮不定位 | ✅ 仅登记；`event_manager.cpp` 零改动 |

## 2. 埋点清单（6 个 / 6 个冻结 EventId）

| # | 位置 | EventId | Level | Params |
|---|---|---|---|---|
| 1 | `valve_set_gpio()` open 分支 | `LOG_VALVE_OPEN` 0x0509 | INFO | `STATE(bool)`、`WAS(bool)` |
| 2 | `valve_set_gpio()` close 分支 | `LOG_VALVE_CLOSE` 0x050A | INFO | `STATE`、`WAS`、`VALVE_OPEN_MS` |
| 3 | `valve_set_gpio()` 防风暴 `return` 分支 | `LOG_VALVE_RATE_LIMITED` 0x050B | WARN | `LIMIT_MS` |
| 4 | `valve_force_close()` 尾部（门控） | `LOG_VALVE_FORCE_CLOSE` 0x0505 | CRITICAL | `CAUSE`、`VALVE_OPEN_MS` |
| 5 | `valve_force_close()` `valve_pin<0` 分支 | `LOG_VALVE_FORCE_CLOSE_FAILED` 0x0506 | CRITICAL | `CAUSE` |
| 6 | `valve_task()` 超时分支（一次性锁） | `LOG_VALVE_SAFETY_TIMEOUT` 0x0508 | WARN | `OPEN_MS`、`LIMIT_MS` |

**`CAUSE` 冻结值**：`1 = WEIGHT_ERROR` / `2 = MANUAL_COMMAND`（预留）/ `3 = SAFETY_TIMEOUT`（预留）。
**刻意省略** `LOG_P_GAIN_AFTER_CLOSE_G`（无"关阀后残余增重"检测，`P0-4`）—— **不填伪造的 0**。

## 3. 上板验证结果（COM8，固件 `.pio/build/p2h`）

| # | 验证项 | 结果 | 证据 |
|---|---|---|---|
| 1 | `LOG_VALVE_OPEN` / `LOG_VALVE_CLOSE` | ✅ 通过 | `seq=523 INFO LOG_VALVE_OPEN STATE=True WAS=False`；`seq=524 INFO LOG_VALVE_CLOSE STATE=False WAS=True VALVE_OPEN_MS=2006`。**重复 `valve_open()` 只产生一条**（天然边沿去重） |
| 2 | `FORCE_CLOSE` 门控 | ✅ **精确通过** | 清零后 149.7 s 窗口：**10 次调用 → Δcrit = 4**，**离线按 5 s 规则模拟 = 4**，理论上限 30；crit 增量点与前置 FORCE_CLOSE 间隔 1.03/0.66/0.66/0.66 s（**零孤儿**） |
| 3 | `FORCE_CLOSE_FAILED` | ✅ 通过 | `gpio_pin=-1`：串口 `FORCE CLOSE` 计数 = **0**，`guard_rx = 54` ⇒ **52 条** `CRITICAL LOG_VALVE_FORCE_CLOSE_FAILED CAUSE=1` |
| 4 | `SAFETY_TIMEOUT` 一次性锁 | ✅ 通过 | `safety_timeout_sec=2`，**6 轮超时 → 每轮恰好 1 条**（`OPEN_MS=2000`、`LIMIT_MS=2000`），伴随 1×`LOG_VALVE_OPEN` + 1×`LOG_VALVE_CLOSE(OPEN_MS=2000)` |
| 5 | `RATE_LIMITED` | ⚪ 未触发（`NC-13`） | 需 50 ms 内二次真实切换，串口命令精度不足；实测 11 个会话均为 0 |
| 6 | 回归 | ⛔ **168/195（26 MISS）阻塞** | 根因 `R-7`（见下），**非代码缺陷** |

## 4. 验证方法学（两条新手法，后续复用）

1. **`logt stats` 的 `crit=` / `emit=` 增量 = 设备侧计量**：云队列溢出时云端捕获会丢记录，这个不会。
   **已单独验证该计数不重复计数**：`logt fill crit 62` ⇒ `Δcrit` **恰好 = 62**（`emit`/`flash`/`cloud` 各 +62）。
2. ⚠️ **不清零 Flash 时 `crit` 会被"回放 / 前序 Boot 记录"污染**：未 `fwipe` 时观测到 `crit` 增量点与
   任何 `FORCE_CLOSE` 相差 **18 ~ 37 s**（"孤儿"），一度误判为"门控失效"；执行
   `fwipe` + `mwipe` + `creset` + `reset` 后**孤儿全部消失**。
   ⇒ **做精确计数实验前必须先清零。**

## 5. ⛔ 阻塞项：回归夹具与跨模块埋点根本冲突（`R-7`）

**现象**：A 42/56 · B 64/65 · C 20/21 · D 23/30 · E 19/23 = **168/195（26 MISS）**。

**根因**：A 段实测发生 **69 次** `valve_force_close()`（P2-G 那一轮仅 **2** 次）。
经 5 s 门控后仍多出约 **34 条 CRITICAL** 记录 ⇒ 落 Flash + 进云队列 ⇒
F0/F1/F2 的**精确记账断言全部失准**（`total=40`/`recs=9`/`replay=31`/`replay=40`/`rarmed=0`/`replay_done=1`/
`evict_inf=9`/`qdrop=12`/`total=8`×2/`giveup=1`×2/`fdrop=8`），C/D/E 的计数断言同理。

**为什么"按 P2-G 方式修夹具"在此不适用**：这 26 条断言就是 **FIX-1/2/3 的记账本体**
（`evict_inf + qdrop == 淘汰总数` 这类恒等式）。改成结构性判据 = **删除这套夹具的核心判别力** = 降断言。

**为什么"夹具自我清零"救不了**：每段开头的 `fwipe`+`mwipe`+`reset` 只能清"之前"的记录，
清不掉段内持续产生的干扰；而干扰是**突发式**（静置实测 0.08 ~ 0.51 次/s，峰值 2~7 次/s）⇒ 期望值不可确定。

**A/B 归因实验（排除"是 P2-H 代码放大了异常"）**：

| 固件 | 观测窗口 | `valve_force_close()` 次数 | 折算速率 |
|---|---|---|---|
| P2-G（`.pio/build/p2g`） | 45 s | 6 | 0.13 /s |
| P2-H（`.pio/build/p2h`） | 45 s | 23 | 0.51 /s |
| P2-H | 120 s | 10 | **0.083 /s** |
| P2-H（回归 A 段，含 12 次 `flush`） | ~170 s | **69** | 0.41 /s |

⇒ 区间跨度达 6 倍，**且同一固件两次测量差 6 倍** ⇒ **突发式方差，不是代码放大**；
A 段偏高是因为**夹具自身的 `flush` 阻塞诱发了跳变**（新发现 `R-8`）。

## 6. 新增问题（已同步 `未修复的问题.md`）

| 编号 | 标题 | 优先级 |
|---|---|---|
| **VALVE-6** | `FORCE_CLOSE_FAILED` 未门控（实测 54 次调用 → 52 条 CRITICAL） | 🔴 |
| **R-7** | 195 回归夹具与跨模块 WARN+/CRITICAL 埋点根本冲突（回归阻塞） | 🔴 |
| **R-8** | FS 阻塞诱发重量跳变（A 段 69 次 vs 静置 0.08 次/s） | 🟠 |
| **NC-14** | `SAFETY_TIMEOUT` 的"防风暴跳过⇒重复进分支"路径本身未复现 | ⚪ |
| **NC-15** | `FORCE_CLOSE` 门控的"cause 变化"分支未真正区分（本轮 cause 恒为 1） | ⚪ |
| **D-3** | 临时改过 `valve/gpio_pin`(-1→12) 与 `valve/safety_timeout_sec`(300→10→2→300)，**均已恢复** | — |
| **T-4** | 回归期间"注入 ACK"与"跨模块埋点"是两个独立的夹具杀手 | — |

**状态变更**：`VALVE-2` → ✅ **已解决（P2-H）**；`VALVE-3` → ✅ **已解决（P2-H）**；`VALVE-4` → 已缓解（用 `LOG_P_WAS` 表达）。

## 7. 待拍板（P2-H 实现后新增）

| # | 事项 | 建议 |
|---|---|---|
| **E1** | `R-7` 回归夹具冲突如何解决 | **路径 1：给 weight 模块加 `enable` 配置**（`valve`/`rtc` 已有同类项）⇒ 回归期间关掉重量采样 ⇒ **195 条断言原样全绿** |
| **E2** | `VALVE-6` 是否加同款 5 s 门控 | 建议加 |
| **E3** | `VALVE-1` 是否单独开一轮修 | 建议单独评审 |
| **E4** | `R-8` 是否单独评审 | 建议单独评审（Weight 鲁棒性） |
