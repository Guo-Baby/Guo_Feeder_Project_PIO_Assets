# Phase 4：Dispense 日志边界审查 + 接入

> 日期：2026-09-20
> 阶段：Phase 4 · Dispense（= **DispenseGuard 安全响应器**，非完整出粮控制模块）
> 约束：不设计电机 / Stepper / 出粮状态机 / Workflow Action / 粮仓逻辑
> 用户指令：**实现前先输出审查**（§七）· **禁止直接修改 `log_events.h`** · 只增加观测能力

---

## 0. 结论速览

| 问题 | 结论 |
|---|---|
| **1.** 监听位置 | `src/dispense_guard.cpp:23` `dispense_guard_event_callback()`，订阅注册在 `dispense_guard_init()`（`main.cpp:500` 调用） |
| **2.** 调用链 | 用户给的链**正确**，但需补 2 个事实点：`event_push()` 由 **3 处** 发出；`valve_force_close()` **全仓库唯一调用者就是 `dispense_guard.cpp:37`** |
| **3.** 是否重复 | 重量异常（`weight.cpp` 5 处 `log_emit`）与关阀（`valve.cpp` 6 处）**均已覆盖** ⇒ 这两项**不埋**（与用户判断一致） |
| **★ 4.** 唯一可复用 ID | **`FAILED`(0x0503) 之外，无人精确匹配**。四个已冻结的 Dispense ID（0x0501–0x0504）语义**均不对位**（详见 §7.3） |
| **★ 5.** 是否需要新增 | **技术上需要**（1 个 EventId）⇒ 按 §七 要求**先提理由、不直接改** ⇒ **需裁决后实施** |
| **6.** 聚合方案 | 复用 LogManager **既有**突发合并白名单（`s_coalesce_targets[]`），**零新机制**；`Σ LOG_P_COUNT = 真实响应次数` |
| **★ 7.** 一个实现陷阱 | 合并器要求被合并的 EventId **至少带 1 个参数**（`log_coalesce_emit_summary()` 的 `base_n == 0` 会**静默丢弃**折叠计数）⇒ 埋点**必须带参** |

---

## 一、确认当前代码事实

### 1.1 监听位置

| 项 | 代码事实 |
|---|---|
| 回调函数 | `src/dispense_guard.cpp:23` `static void dispense_guard_event_callback(const EventMessage &msg)` |
| 过滤 | `dispense_guard.cpp:27-32`：`if (msg.event != EVENT_WEIGHT_ERROR) return;` |
| 订阅注册 | `dispense_guard.cpp:58-61` `event_subscribe(EVENT_WEIGHT_ERROR, dispense_guard_event_callback)` |
| 调用者 | `main.cpp:500` `dispense_guard_init();`（**忽略返回值**） |
| 头文件 | `dispense_guard.h` —— **全文 3 行**，仅 `void dispense_guard_init();` |

### 1.2 调用链（逐段核对）

用户给出的链：

```
weight.cpp → event_push() → event_manager → dispense_guard callback → valve_force_close()
```

**核对结果：正确。** 补充 3 个精确事实：

```
① 发布（3 处，全部在 weight.cpp）
   weight.cpp:268  weight_record_jump()            "Weight jump detected"        CRITICAL / STATE
   weight.cpp:384  weight_task() 无数据 5 s 通道    "HX711 not ready"            HIGH     / STATE
   weight.cpp:557  weight_trigger_start() 前置检查  "Weight error, trigger aborted" CRITICAL / STATE
   weight.cpp:578  weight_trigger_start() 参数非法  "Invalid gram param, use 20" NORMAL   / STATE
   └─ 另：weight.cpp:269/385/558/579 —— 共 4 个 event_push 调用点
        ↓
② 入队 / 分发（event_manager.cpp）
   event_push()  →  队列（EVENT_QUEUE_SIZE=32）
   event_dispatch()  →  callback_table[eid][i]()      ← 每 loop 最多 drain 4 条
        ↓
③ 响应（dispense_guard.cpp，唯一订阅方）
   dispense_guard_event_callback()  line 23
     ├─ line 27  if (msg.event != EVENT_WEIGHT_ERROR) return;    ← 过滤
     ├─ line 33  Serial.println("[DispenseGuard] Weight error received")
     ├─ line 36  bool result = valve_force_close();              ← ★ 唯一调用点
     └─ line 38/46 Serial.println(成功 / 失败)
        ↓
④ 执行（valve.cpp）
   valve_force_close()  line 396
     ├─ line 398  if (valve_pin < 0) → 埋 0x0506 后 return false   ← 失败分支
     ├─ line 413  gpio_set_level(...)                              ← 强制写电平（绕过 50 ms 防风暴）
     ├─ line 416  current_state = false
     ├─ line 418  valve_update_state()                             ← STATE_VALVE_STATUS
     ├─ line 424  open_start_time = 0
     ├─ line 427  event_push(EVENT_VALVE_CLOSE, "force close", …)
     └─ line 470  埋 0x0505（CRITICAL，5 s 门控）
```

**★ 关键事实（本轮首次确认）**：`valve_force_close()` 的**全仓库调用点只有 1 个** —— `dispense_guard.cpp:37`。

```
$ grep -rn "valve_force_close" src/
src/dispense_guard.cpp:37:        valve_force_close();     ← 唯一调用
src/valve.cpp:396:bool valve_force_close()             ← 定义
src/valve.h:41:bool valve_force_close();               ← 声明
src/log_manager.cpp:139:  （注释）
src/valve.cpp:49/454:      （注释）
```

⇒ 这条链是**单入口、单出口的确定性链路**，不存在第二条来源。

### 1.3 确认：WeightManager 已有日志 —— **不埋**（用户判断正确）

```
$ grep -c "log_emit(" src/weight.cpp  →  5
```

| 位置 | EventId | Level | 参数 |
|---|---|---|---|
| `weight.cpp:236` | `LOG_WEIGHT_ERROR_ENTER` 0x050C | WARN | `CAUSE` 位掩码 + `WEIGHT_G` + `RAW` |
| `weight.cpp:244` | `LOG_WEIGHT_ERROR_EXIT` 0x050D | INFO | `CAUSE` + `DURATION_MS` + `WEIGHT_G` |
| `weight.cpp:341` | `LOG_WEIGHT_ZERO_DONE` 0x050E | INFO | `OFFSET`/`SAMPLES`/`SAVED` |
| `weight.cpp:350` | `LOG_WEIGHT_CALIB_FAILED` 0x0510 | ERROR | `OFFSET` + `ERR_CODE` |
| `weight.cpp:632` | `LOG_WEIGHT_TRIGGER_FIRED` 0x050F | INFO | `TARGET_G`/`DELTA_G`/`WEIGHT_G` |

⇒ **"重量异常发生"已有权威记录**（且比 Dispense 侧能看到的信息更完整）⇒ **Dispense 侧绝不重复记录**。

### 1.4 确认：Valve 已有日志 —— **不埋**（用户判断正确）

```
$ grep -c "log_emit(" src/valve.cpp  →  6
```

| 位置 | EventId | Level | 关键语义 |
|---|---|---|---|
| `valve.cpp:137` | `LOG_VALVE_RATE_LIMITED` 0x050B | WARN | 50 ms 防风暴跳过 |
| `valve.cpp:168` | `LOG_VALVE_OPEN` 0x0509 | INFO | 正常开启（`STATE`/`WAS`） |
| `valve.cpp:181` | `LOG_VALVE_CLOSE` 0x050A | INFO | 正常关闭（+ `VALVE_OPEN_MS`） |
| `valve.cpp:384` | `LOG_VALVE_SAFETY_TIMEOUT` 0x0508 | WARN | 安全超时（一次性报告锁） |
| `valve.cpp:405` | `LOG_VALVE_FORCE_CLOSE_FAILED` 0x0506 | **CRITICAL** | 强制关阀失败（`pin<0`） |
| `valve.cpp:470` | `LOG_VALVE_FORCE_CLOSE` 0x0505 | **CRITICAL** | 强制关阀成功（`CAUSE` + `VALVE_OPEN_MS`，**5 s 门控**） |

⇒ **"阀门关闭动作本身"已有权威记录** ⇒ **Dispense 侧绝不重复记录**。

---

## 二、确定唯一日志需求

### 2.1 语义（按用户 §二）

> **"Dispense 模块响应重量异常事件，并触发阀门安全动作"**

### 2.2 ★ 关键发现：现有日志**已经能**区分情况 A / 情况 B

用户的目的（§二）：区分

| 情况 | 链路 | **现有日志** |
|---|---|---|
| **A** | `Workflow → Valve` | `LOG_VALVE_OPEN`(0x0509) / `LOG_VALVE_CLOSE`(0x050A)，**INFO** |
| **B** | `Weight → Event → Dispense → Valve` | `LOG_VALVE_FORCE_CLOSE`(0x0505)，**CRITICAL**，`CAUSE = VALVE_CAUSE_WEIGHT_ERROR(1)` |

⇒ **两条路径产生不同的 EventId**（0x0509/0x050A vs 0x0505），且 `valve.cpp:56` 的注释
`VALVE_CAUSE_WEIGHT_ERROR = 1, // dispense_guard 收到 EVENT_WEIGHT_ERROR`
说明 **Valve 侧已经把"本次强关的来源 = dispense_guard"写进了 `LOG_P_CAUSE`**。
⇒ 单看**来源区分**这一项，**§二 的目的已由现有日志满足**（不需要新日志）。

### 2.3 ★ 但 §五 提出了一个**现有日志无法满足**的诉求

用户 §五：

> "安全动作次数：不变。日志记录数量：下降。**COUNT 守恒：ΣCOUNT = 实际 Dispense 响应次数**。"

**现有 `LOG_VALVE_FORCE_CLOSE`(0x0505) 无法满足**，因为它有 **5 s 门控**：

```cpp
// valve.cpp:52 / 461-471
static const uint32_t VALVE_FORCE_CLOSE_LOG_COOLDOWN_MS = 5000UL;
if (cause != last_force_close_cause ||
    (uint32_t)(now_ms - last_force_close_log_ms) >= VALVE_FORCE_CLOSE_LOG_COOLDOWN_MS)
{   ... log_emit(LOG_VALVE_FORCE_CLOSE, ...); }
```

⇒ 5 s 内发生 N 次强制关阀，**Valve 只记 1 条，且不带任何计数** ⇒ **真实次数丢失**。

```cpp
// valve.cpp:454 注释原文
// 为何必须门控：valve_force_close() 无状态判等，实测权重跳变期可达 6~20 次/s，
// 而 LOG_VALVE_FORCE_CLOSE 是 CRITICAL => 落 Flash(496 条环) + 进云队列(128 槽)
// => 峰值下 83s 即可冲光全部历史日志。
```

**⇒ 这就是新日志的增量价值（唯一站得住的理由）：**

> **`LOG_VALVE_FORCE_CLOSE` 是"带冷却的动作记录"，Dispense 侧需要的是"无冷却、带计数的决策记录"。**
> 二者**不是同一事实**：
> · Valve 侧 = **执行器动作**（GPIO 写入尝试，可能假成功 / 可能 `pin<0` 失败）
> · Dispense 侧 = **决策**（我响应了这次事件，且我发起了安全动作）
>
> 且 Valve 侧的 5 s 冷却**必然丢失次数**，Dispense 侧的聚合**必然守恒次数** ⇒ 互补，非冗余。

### 2.4 明确不记录（按用户 §二）

| 不记录 | 原因 |
|---|---|
| 每一次 `EVENT_WEIGHT_ERROR` | ❌ `weight.cpp:236` 已记（`0x050C`，边沿 + 突发合并） |
| `EventManager` 转发 | ❌ P2-M 已定：EventManager 只记自身异常（队列满 / 风暴） |
| `Valve FORCE_CLOSE` 重复调用 | ❌ `valve.cpp:470` 已记（`0x0505`，CRITICAL，5 s 门控） |

---

## 三、埋点位置

### 3.1 唯一的业务决策点

```
dispense_guard.cpp
  23  static void dispense_guard_event_callback(const EventMessage &msg)
  27  {
  28      if (msg.event != EVENT_WEIGHT_ERROR)
  31          return;                                    ← 过滤
                                                      
      ┌──────────────────────────────────────────┐
      │ ★ 埋点位置：此处（过滤通过之后、      │  ← 本次唯一埋点
      │   valve_force_close() 调用之前）        │
      └──────────────────────────────────────────┘
                                                      
  33      Serial.println("[DispenseGuard] Weight error received");
  36      bool result = valve_force_close();            ← 安全动作（零改动）
  38-49   成功/失败串口打印                            ← 零改动
  50  }
```

**位置判据（三条，全部满足）**：
1. **过滤之后** ⇒ 只有真正的 `EVENT_WEIGHT_ERROR` 才记，不会记到其它事件
2. **调用之前** ⇒ 记录的是"**决定**发起安全动作"，与 Valve 的"**动作已执行**"在因果上分离（顺序正确：决策 → 执行）
3. **在回调内** ⇒ 与安全动作**同一上下文**，无跨任务、无锁需求、无 PSRAM 需求

### 3.2 明确不埋的位置（按用户 §三）

| 位置 | 不埋理由 |
|---|---|
| `event_push()`（`event_manager.cpp`） | P2-M 已冻结：只记自身运行异常；且 `event_push()` 是**搬运**，不是事实定义点（铁律 25） |
| `event_dispatch()`（`event_manager.cpp`） | 同上；且是分发循环，与"是不是 Dispense 响应"无关 |
| `valve_force_close()` 内部（`valve.cpp`） | 已埋 `0x0505`/`0x0506`；用户 §六.5 亦禁止改 Valve 区分来源 |

⇒ **避免 `Weight + Event + Dispense + Valve` 形成重复链**（用户原话）。

**埋点后完整链路（每段恰好一条记录，无重复）**：

```
① weight.cpp:236   LOG_WEIGHT_ERROR_ENTER 0x050C WARN    ← 异常事实（weight 定义点）
② 【本次新增】      LOG_DISPENSE_*        详见 §7         ← 决策事实（dispense_guard 定义点）
③ valve.cpp:470    LOG_VALVE_FORCE_CLOSE  0x0505 CRITICAL ← 执行事实（valve 定义点）
```

---

## 四、Critical 事件日志过滤设计（**只限制日志，不限制安全动作**）

### 4.1 安全动作路径：零改动

| 检查项 | 结论 |
|---|---|
| 是否新增 `debounce` / `delay` / `ignore` / `return`？ | ❌ **无** |
| 是否修改 `msg.event != EVENT_WEIGHT_ERROR` 过滤？ | ❌ **无** |
| 是否修改 `valve_force_close()` 调用？ | ❌ **无** |
| 是否修改 EventManager 策略 / 优先级 / 风暴参数？ | ❌ **无**（用户 §六.1/§六.2） |
| 短时间多个重量异常，Dispense 是否仍全部响应？ | ✅ **是**（回调无任何提前返回） |

**埋点的物理形态**：在"过滤通过"与"调用 `valve_force_close()`"之间插入一个**纯观测块**，
不改变任何控制流（无 `return`、无分支、返回值不影响后续）：

```c
/* 纯观测：不参与控制流，无 return，不影响 result 判定 */
{ LogParamIn p[...]; log_emit(...); }
valve_force_close();      /* ← 原有调用，逐字不变 */
```

### 4.2 日志侧保护：由 LogManager 承担（见 §五）

⇒ 安全动作**永不受限**；日志数量**受限**。这两件事被物理隔离在两个模块里：
- 安全动作：`dispense_guard.cpp` / `valve.cpp`（**本次零改动**）
- 日志限流：`log_manager.cpp`（既有合并白名单，**加 1 行**）

---

## 五、日志聚合要求

### 5.1 复用 LogManager **既有**突发合并机制（**零新机制**）

项目已有完整实现（P2-I 为 `LOG_WEIGHT_ERROR_ENTER` 引入），**键位是"白名单 + 窗口"**：

```cpp
// log_manager.h:60
#define LOG_COALESCE_WINDOW_MS    5000u

// log_manager.cpp:137-142
static const LogCoalesceTarget s_coalesce_targets[] = {
    { LOG_WEIGHT_ERROR_ENTER, LOG_LVL_WARN }
};
```

**机制语义（逐条核对用户 §五 的要求）**：

| 用户要求 | 既有机制行为 | 是否满足 |
|---|---|---|
| 首次立即记录 | `log_coalesce_filter()` 分支②：接受本条，**立即入环**，并标注 `LOG_P_COUNT = 1` | ✅ |
| 窗口内重复：只累计 COUNT | 分支①：`return true`（**不占 seq、不进 RAM 环**），`s_coalesce_folded++` | ✅ |
| 安全动作次数不变 | 合并发生在 `log_emit()` 内，**安全动作早已在回调里完成**，物理无关 | ✅ |
| ΣCOUNT = 实际响应次数 | 窗口到期由 `log_coalesce_tick()`（在 `log_task()` 里）发一条汇总，`LOG_P_COUNT = folded` | ✅ |
| 不改 EventManager / Weight / Valve / Dispense 安全逻辑 | 改动**只在 `log_manager.cpp`** | ✅ |

### 5.2 ★ 实现陷阱（必须写进代码注释）

```cpp
// log_manager.cpp:173-181
static void log_coalesce_emit_summary(LogEventId event_id, LogLevel level,
                                      const LogParamIn *base, uint8_t base_n,
                                      uint16_t folded)
{
    if (base == nullptr || base_n == 0 ||
        base_n >= LOG_MAX_PARAMS || folded == 0)
    {
        return;                    // ← ★ base_n == 0 时**静默丢弃折叠计数**！
    }
    ...
}

// log_manager.cpp:245
if (!log_coalesce_is_target(event_id) || param_count >= LOG_MAX_PARAMS)
{
    return false;                  // ← ★ param_count 已达上限时不合并
}
```

⇒ **两条硬约束**：
1. 被合并的埋点**必须至少带 1 个参数**（否则汇总记录无法生成 ⇒ **ΣCOUNT 不守恒**）
2. 参数个数必须 **< `LOG_MAX_PARAMS`(8)**（合并器要追加 `LOG_P_COUNT` 占 1 位）

⇒ 本次埋点固定带 **1 个参数**（`LOG_P_CAUSE`），合计 **2 个**（含合并器追加的 `LOG_P_COUNT`）⇒ 安全。

### 5.3 窗口与频率对照

| 项 | 值 |
|---|---|
| 合并窗口 | `LOG_COALESCE_WINDOW_MS` = **5000 ms**（与 Valve 的 `VALVE_FORCE_CLOSE_LOG_COOLDOWN_MS` 同量级，**天然对齐**） |
| 实测最坏频率 | **6~20 次/s**（`valve.cpp:454` 注释，R-3 实测 B2 会话 50 次/175 s，峰值 6 次/s） |
| 不合并时 5 s 内记录数 | 30 ~ 100 条 |
| 合并后 5 s 内记录数 | **2 条**（1 条立即 + 1 条汇总），ΣCOUNT = 30~100 |
| 信息损失 | **0**（次数守恒） |

---

## 六、禁止事项核对

| # | 禁止项 | 本次设计 |
|---|---|---|
| 1 | 修改 EventManager 风暴参数 | ✅ 不涉及 `event_manager.cpp` |
| 2 | 修改 `EVENT_WEIGHT_ERROR` 策略 | ✅ 不涉及（`EVENT_POLICY_STATE` 原样） |
| 3 | 给 Dispense 增加自己的事件 ID | ⚠️ **这是本审查的核心冲突点，见 §7.4** |
| 4 | 在 Weight 中增加 Dispense 日志 | ✅ 不涉及 `weight.cpp` |
| 5 | 在 Valve 中区分调用来源 | ✅ 不涉及 `valve.cpp`（`LOG_P_CAUSE` 既有字段**原样保留，不修改**） |
| 6 | 通过 Workflow 绕路 | ✅ 不涉及 `workflow.cpp`，不新增 Action |

---

## 七、需要先确认的问题（用户 §七 的四问）

### 7.1 当前 Dispense / DispenseGuard 是否已有日志接口？

**❌ 完全没有。**

```
$ grep -n "log_emit\|log_manager\|LogParamIn\|LOG_" src/dispense_guard.cpp src/dispense_guard.h
（零命中）
$ grep -c "log_emit(" src/dispense_guard.cpp
0
```

`#include` 图：`Arduino.h` / `dispense_guard.h` / `event_manager.h` / `valve.h` ⇒ **不依赖 `log_manager`**。

### 7.2 当前 Valve 日志是否能区分调用来源？

**✅ 能，且已经能。**

- 正常路径（情况 A）：`LOG_VALVE_OPEN`(0x0509) / `LOG_VALVE_CLOSE`(0x050A)，INFO
- 安全路径（情况 B）：`LOG_VALVE_FORCE_CLOSE`(0x0505)，**CRITICAL**，且携带 `LOG_P_CAUSE = 1`

```cpp
// valve.cpp:55-59（冻结定义，本次不改）
enum : uint8_t {
    VALVE_CAUSE_WEIGHT_ERROR   = 1,   // dispense_guard 收到 EVENT_WEIGHT_ERROR
    VALVE_CAUSE_MANUAL_COMMAND = 2,   // 预留
    VALVE_CAUSE_SAFETY_TIMEOUT = 3    // 预留
};
```

⇒ **结论：用户 §八 V4（区分安全来源 vs Workflow 来源）在现有日志下已可验证**，
不需要为"来源区分"新增任何日志。**新日志的价值在 §2.3（次数守恒），不在来源区分。**

### 7.3 当前已有 LogEvent 中是否存在可复用 ID？

**0x0500 段尚有 4 个已冻结、usage = 0 的 Dispense ID：**

```cpp
// log_events.h:264-268
// ---- 0x05xx Water（Dispense / Valve / Weight）----
LOG_DISPENSE_START   = 0x0501,   // INFO
LOG_DISPENSE_DONE    = 0x0502,   // INFO
LOG_DISPENSE_FAILED  = 0x0503,   // WARN
LOG_DISPENSE_TIMEOUT = 0x0504,   // WARN
```

**逐一对位本需求「Dispense 响应重量异常 → 触发安全动作」：**

| 候选 | Level | 语义 | 对位判定 |
|---|---|---|---|
| `0x0501 START` | INFO | "Dispense **开始**" | ❌ **语义反向**。安全响应是"**终止**供水"（关阀），不是"开始"。且供水场景里 `DISPENSE_START` 的语义位置已被 `LOG_VALVE_OPEN`(0x0509) 占用 ⇒ 复用会**伪造一次"开始"** |
| `0x0502 DONE` | INFO | "Dispense **完成**" | ❌ 反向，且"完成"是 Valve 的事实 |
| `0x0503 FAILED` | WARN | "Dispense **失败**" | ⚠️ **最接近但会撒谎**。R-8 已定性：跳变**误报**占多数（`未修复的问题.md`：「V4 实测自动恢复、无残留误报」），此时**根本没有供水过程在跑** ⇒ 记 "DISPENSE_FAILED" 是**假事实** |
| `0x0504 TIMEOUT` | WARN | "Dispense **超时**" | ❌ 无关 |

⇒ **结论：4 个候选语义均不对位，无法在不引入假事实的前提下复用。**

### 7.4 是否需要复用已有日志事件？+ 新增 ID 的理由（按用户 §七：先提理由，不直接改）

**冲突点**：
- 用户 §六.3 禁止"给 Dispense 增加自己的事件 ID"
- 但 §7.3 显示：**0x0500 段无可精确复用的 ID**

**⇒ 必须由用户裁决。三个可选方案：**

| 方案 | 做法 | 优点 | 缺点 |
|---|---|---|---|
| **方案 1（推荐）** | **新增 1 个 EventId** `LOG_DISPENSE_SAFETY_RESPONSE = 0x0511`（`0x0500` 段尾，从 `LOG_WEIGHT_CALIB_FAILED`(0x0510) **自然续号，不扩段、不改既有编号**），Level **WARN** | ① 语义精确（"安全响应被触发"）② 计数权威 ③ `0x0511` 与 `LV-1` 建议的 ID **同段可并列** ④ WARN ⇒ Flash + Cloud（跨重启可查） | 违反 §六.3（需豁免）；`log_events.h` 需加 1 行 |
| **方案 2** | **复用 `LOG_DISPENSE_START`(0x0501, INFO)** | ✅ 零新增 ID（严格满足 §六.3） | ❌ **语义伪造**（记一次并不存在的"开始"）；INFO ⇒ **只上云不落 Flash**，用户 §五 假设的"进 Flash 队列"压力**实际不存在**（但仍会压 128 槽云队列） |
| **方案 3** | **不新增日志，仅声明现有日志已满足** | ✅ 零改动 | ❌ §五 的 **ΣCOUNT 守恒**无法实现（`0x0505` 的 5 s 门控丢次数） |

**推荐理由（方案 1 的正面论证）**：

1. **语义必要性**：本需求记录的是"**决策**"（Dispense 决定响应），`0x0500` 段既有的
   `DISPENSE_START/DONE/FAILED/TIMEOUT` 描述的是"**一次供水过程**"的生命周期 ⇒ **范畴不同**。
2. **与 `LV-1` 同源**：P2-N 已登记 `LV-1`（订阅失败不可观测，建议 `0x0511`）。
   ⇒ 若同时批准，可一次把 `0x0500` 段补齐（**2 个 ID：0x0511 / 0x0512**），或按本文建议
   **本次只用 `0x0511`**，`LV-1` 留待后续。
3. **Level 选 WARN 的定量理由**：WARN 与 CRITICAL 的 `(Flash, Cloud)` 路由**完全相同**（`{1,1}`，见 `log_events.h:67-69`）；
   但本记录**不应**用 CRITICAL —— CRITICAL 会触发 `log_flush_requested()`（立即 flush + 提升云优先级），
   而这条记录**不是**需要抢通道的故障（真正的故障是 `0x0505`，它才是 CRITICAL）⇒ **WARN 是精确档位**。
4. **INFO 不可接受的定量理由**：INFO = Cloud YES / Flash NO（`log_events.h:66`）⇒
   重启后**无法从 Flash 补发**（`log_manager.h:77` 注释原文：「INFO 是 Cloud YES / Flash NO —— 不落盘，无法从 Flash 补发」）；
   而安全响应记录在离线场景下**必须**能跨重启留存。

**⇒ 待裁决项：批准方案 1（新增 `0x0511`，WARN）还是采用方案 2（复用 `0x0501`，INFO）？**

---

## 八、验证方案（V1–V4）

> 工具：`logt stats` / `logt flash` / `logt fseg` / `logt fver`（串口控制台，`main.cpp:663`）
> 前提（铁律）：**精确计数前必须先清零** —— `logt fwipe` + `mwipe` + `creset` + `logt reset`

### V1 —— Workflow 调用 Valve：**不应有 Dispense 日志**

| 步骤 | 操作 | 期望 |
|---|---|---|
| 1 | `logt reset`，记录基线 (`stats`) | 基线快照 |
| 2 | 触发正常阀门动作（`execute_action VALVE_OPEN` / `VALVE_CLOSE`） | `LOG_VALVE_OPEN`/`CLOSE`(0x0509/0x050A) INFO |
| 3 | `logt stats` + `logt flash` | `flash +0`（INFO 不落 Flash）；**逐条核对 Flash 无 0x0511** |

**断言**：`0x0511` 计数 **= 0**；只有 Workflow / Valve 相关记录。

### V2 —— 单次 Weight Error：三段式记录

| 步骤 | 操作 | 期望 |
|---|---|---|
| 1 | 清零基线（`fwipe`+`mwipe`+`creset`+`reset`） | — |
| 2 | 制造**单次**重量异常（拔 HX711 信号线 ⇒ 5 s 后 `not_ready` 通道触发，`weight.cpp:384`） | 串口出现 `[DispenseGuard] Weight error received` |
| 3 | `logt flash` 逐条读 | ① `0x050C` WARN（weight）② **`0x0511` WARN（Dispense）** ③ `0x0505` CRITICAL（valve） |

**断言**：三段齐全、**各 1 条**、`CAUSE` 参数正确。

### V3 —— 连续多个 Weight Error：**动作不降、日志聚合、COUNT 守恒**

| 步骤 | 操作 | 期望 |
|---|---|---|
| 1 | 清零基线 | — |
| 2 | 制造**连续**重量异常（HX711 跳变通道，实测可达 6~20 次/s） | 串口 `[DispenseGuard] Weight error received` / `[Valve] FORCE CLOSE` **计数不减少** |
| 3 | 统计串口打印次数 = 真实响应次数 `N` | — |
| 4 | `logt flash` 逐条核对 `0x0511`，累加 `LOG_P_COUNT` | **`Σ LOG_P_COUNT == N`** |
| 5 | 对比：`0x0505` 记录数 | 因 5 s 门控 **≪ N**（证明二者互补、非冗余） |

**★ 可脱离硬件验证的补充路径**（项目既有铁律："折叠/去重/门控类验证用**定向注入**"）：

```
logt fill warn 10 511      # 定向注入 10 条 LOG_DISPENSE_SAFETY_RESPONSE(WARN)
logt stats                 # 期望 emit/consumed 只 +2（1 条立即 + 1 条汇总）
logt flash                 # 期望 2 条，第二条 LOG_P_COUNT == 9
```
⇒ **`1 + 9 = 10` = 真实次数**。**无需 HX711、无需 MQTT 即可验证聚合正确性。**

### V4 —— 区分安全来源 vs Workflow 来源

| 来源 | 记录 | 断言 |
|---|---|---|
| Workflow → Valve（情况 A） | `0x0509` / `0x050A`，**无** `0x0511`、**无** `0x0505` | ✅ |
| Weight → Event → Dispense → Valve（情况 B） | `0x050C` + **`0x0511`** + `0x0505`(CAUSE=1) | ✅ |

**断言**：两类来源在 EventId 集合上**完全可分**（A 集合 ∩ B 集合 = ∅）。

---

## 九、实施清单（**待方案裁决后执行**）

### 9.1 若批准方案 1（新增 `0x0511`）—— 3 处改动

| # | 文件 | 改动 | 行数 |
|---|---|---|---|
| 1 | `src/log_events.h` | 新增 `LOG_DISPENSE_SAFETY_RESPONSE = 0x0511, // WARN`（`0x0500` 段尾续号） | +1 |
| 2 | `src/log_manager.cpp` | `s_coalesce_targets[]` 追加 `{ LOG_DISPENSE_SAFETY_RESPONSE, LOG_LVL_WARN }` | +1 |
| 3 | `src/dispense_guard.cpp` | `#include "log_manager.h"` + 回调内 1 处 `log_emit`（含 `LOG_P_CAUSE=1`） | +~12 |

**`dispense_guard.cpp` 的精确 diff（示意）**：

```c
// 新增 include
#include "log_manager.h"   // Phase 4：观测埋点（EventId / ParamId + log_emit）

// callback 内，过滤之后、valve_force_close() 之前
    Serial.println("[DispenseGuard] Weight error received");

    /* ---- Phase 4 埋点：安全响应决策（WARN）----
     * 记的是"决定响应"，不是"重量异常"（weight.cpp:236 已记 0x050C）
     * 也不是"关阀动作"（valve.cpp:470 已记 0x0505）。
     * 纯观测：无 return、无分支、不影响下方 valve_force_close() 的调用与 result。
     * 突发合并：本 EventId 在 LogManager 白名单内 ⇒ 5 s 窗口内重复折叠为
     * LOG_P_COUNT，Σ COUNT = 真实响应次数（与 valve 侧 5 s 门控互补，非冗余）。
     * 必须带 1 个参数（LOG_P_CAUSE）：合并器 log_coalesce_emit_summary()
     * 在 base_n == 0 时会静默丢弃折叠计数。 */
    {
        LogParamIn p[1];
        p[0] = log_arg_u32(LOG_P_CAUSE, DISPENSE_CAUSE_WEIGHT_ERROR);
        log_emit(LOG_DISPENSE_SAFETY_RESPONSE, LOG_LVL_WARN, p, 1);
    }

    bool result = valve_force_close();      // ← 原有调用，逐字不变
```

**CAUSE 常量（模块内 `enum`，与 `valve.cpp:55-59` 同构、同值）**：
```c
enum : uint8_t {
    DISPENSE_CAUSE_WEIGHT_ERROR = 1    // 与 VALVE_CAUSE_WEIGHT_ERROR 同值，便于云端交叉关联
};
```

### 9.2 若采用方案 2（复用 `0x0501`，INFO）

仅改 2 处：`log_manager.cpp` 白名单（`{ LOG_DISPENSE_START, LOG_LVL_INFO }`）+ `dispense_guard.cpp` 埋点。
**但必须同时接受两个后果**：① 语义伪造"开始"；② 只上云不落 Flash。

### 9.3 提交

```
代码：feat(log): add dispense safety response logging
文档：docs(progress): record dispense logging completion
```
提交前后 `git diff --stat src/` 逐次核对；**不 squash**。

---

## 附录 A：本轮修正的既有文档错误

| 文件 | 原文 | 修正 |
|---|---|---|
| `log模块历史/P2N-DispenseGuard埋点审查0920.md:202` | `LOG_P_CAUSE`(**0x1F**) | `LOG_P_CAUSE`(**0x16**) |
| `未修复的问题.md:366` | `LOG_P_CAUSE`(**0x1F**) | `LOG_P_CAUSE`(**0x16**) |

> 事实来源：`log_events.h:133` `LOG_P_CAUSE = 0x16`；`log_events.h:142` `LOG_P_REASON = 0x1F`（两者**不可混用**）

## 附录 B：本轮核查的源文件

| 文件 | 用途 |
|---|---|
| `src/dispense_guard.cpp` | 回调全文、唯一决策点定位 |
| `src/valve.cpp` | `valve_force_close()` 全文、6 处埋点、`VALVE_CAUSE_*`、5 s 门控 |
| `src/weight.cpp` | 4 个 `event_push` 调用点、5 处埋点 |
| `src/event_manager.cpp` | `event_push()` / `event_dispatch()` / `event_subscribe()` |
| `src/log_manager.cpp` | 突发合并全实现（`:131-322`）、`log_emit` / `log_task` / `log_coalesce_tick` |
| `src/log_manager.h` | `LOG_COALESCE_WINDOW_MS`、合并语义契约 |
| `src/log_events.h` | EventId 段表、ParamId 表、Level Policy、`LOG_MAX_PARAMS` |
| `src/main.cpp` | 初始化顺序、`logt` 控制台命令集 |
| `未修复的问题.md` | R-3 频率实测、`VALVE-1/3/5`、`LV-1` |

---

**最后更新**：2026-09-20 · Phase 4 Dispense 日志边界审查
**当前状态**：**审查完成，等待 §7.4 方案裁决后实施**（本轮未改 `src/`）
