# P2-N：DispenseGuard 埋点审查（Phase 3 第 ⑥ 项）

> 日期：2026-09-20
> 阶段：LogManager Phase 3 · DispenseGuard
> 结论：**DispenseGuard 在当前状态下没有任何合法埋点位置**
> 代码改动：**零**（`src/` 与 `test/` 均未触碰，`git diff --stat src/` 为空）
> 决策依据：用户裁决「不加 EventId，本次零代码改动」

---

## 0. 结论先行

| 问题 | 答案 |
|---|---|
| DispenseGuard 应该埋几个点？ | **0 个** |
| 是否重复埋点？ | **全部候选都会重复** —— 3 个候选点各被其它模块 100% 覆盖 |
| 有无"不重复"的埋点位置？ | 有且仅有 **1 处**：`event_subscribe()` 失败。但它**没有 EventId** |
| 本次是否落地？ | **否**。用户裁决不新增 EventId ⇒ `dispense_guard.cpp` **零改动**（`LV-1` 登记） |

> **一句话**：DispenseGuard 是**纯转发模块**（输入来自 `weight`、输出委托 `valve`），
> 它自身不产生任何业务事实 ⇒ **按"业务模块记业务事实"的原则，它本就不该有埋点**。
> 唯一属于它自己的事实（"订阅失败 ⇒ 我永久失效"）在**日志协议里没有对应的 EventId**。

---

## 1. 模块职责与代码事实

### 1.1 模块本体（全文 73 行，`src/dispense_guard.cpp`）

```cpp
#include <Arduino.h>
#include "dispense_guard.h"
#include "event_manager.h"
#include "valve.h"

// 职责: 监听重量异常 / 紧急关闭阀门
// 不参与: workflow状态 / command结果 / weight计算

static void dispense_guard_event_callback(const EventMessage &msg)
{
    if(msg.event != EVENT_WEIGHT_ERROR) return;          // ← 过滤
    Serial.println("[DispenseGuard] Weight error received");
    bool result = valve_force_close();                   // ← 委托给 valve
    if(result)  Serial.println("[DispenseGuard] Valve force closed");
    else        Serial.println("[DispenseGuard] Valve force close failed");
}

void dispense_guard_init()
{
    bool result = event_subscribe(EVENT_WEIGHT_ERROR, dispense_guard_event_callback);
    if(result) Serial.println("[DispenseGuard] Init OK");
    else       Serial.println("[DispenseGuard] Event subscribe failed");
}
```

**代码事实**：
- 无一行业务逻辑 —— 只有 1 次过滤 + 1 次转发 + 4 行 `Serial.println`
- **零日志引用**：全文件检索 `log_emit` / `log_manager.h` / `LogParamIn` / `LOG_` ⇒ **零命中**
- **零 System State**、**零 Config**、**零返回值接口**（`dispense_guard.h` 只有 `void dispense_guard_init()`）
- `#include` 图：`Arduino.h` / `dispense_guard.h` / `event_manager.h` / `valve.h` ⇒ **不依赖** `weight`、`log_manager`

### 1.2 数据流（只有 4 个可埋点的位置）

```
  weight.cpp:269/385/558            event_manager.cpp            dispense_guard.cpp              valve.cpp
  ┌──────────────────┐            ┌──────────────────┐        ┌────────────────────┐        ┌──────────────────────┐
  │ EVENT_WEIGHT_    │  publish   │  event_push()    │        │ 回调进入           │        │ valve_force_close()  │
  │ ERROR  ──────────┼───────────►│  入队 / 分发      ├───────►│ q2 过滤通过        ├───────►│ GPIO 写 0            │
  └──────────────────┘            └──────────────────┘        │ q1 valve_force_    │        │ q3 失败分支          │
                                                              │    close() 调用    │        └──────────────────────┘
                                                              └────────────────────┘
                                                                       ▲
                                                     q4  init: event_subscribe() 失败
```

| 位置 | 语义 | **是否已被他处记录** |
|---|---|---|
| **q1** 调用 `valve_force_close()` | "关阀动作发生" | ✅ **是** → `valve.cpp:470` `LOG_VALVE_FORCE_CLOSE`(0x0505 CRITICAL，带 5 s 门控) |
| **q2** 收到 `EVENT_WEIGHT_ERROR` | "重量异常" | ✅ **是** → `weight.cpp:236` `LOG_WEIGHT_ERROR_ENTER`(0x050C WARN) + `weight.cpp:244` `LOG_WEIGHT_ERROR_EXIT`(0x050D INFO) |
| **q3** `valve_force_close()` 返回 false | "关阀失败" | ✅ **是** → `valve.cpp:405` `LOG_VALVE_FORCE_CLOSE_FAILED`(0x0506 CRITICAL) |
| **q4** `event_subscribe()` 失败 | "本模块永久失效" | ❌ **否** → **全系统不可观测** |

---

## 2. 逐点重复性分析（对应用户要求"注意不要重复埋点"）

### 2.1 q1：调用 `valve_force_close()` —— ❌ **重复**

`valve.cpp:396-475` 的 `valve_force_close()` 内部已完整埋点：

```cpp
bool valve_force_close()
{
    if(valve_pin < 0) {
        // ---- P2-H 埋点：强制关阀失败（CRITICAL）----
        log_emit(LOG_VALVE_FORCE_CLOSE_FAILED, LOG_LVL_CRITICAL, p, 1);   // ← q3 已覆盖
        return false;
    }
    ...
    // =====================================================
    // P2-H 埋点：强制关阀（CRITICAL，去重门控 —— 决策 D1=A）
    // =====================================================
    {
        const uint8_t cause = VALVE_CAUSE_WEIGHT_ERROR;   // 本轮唯一来源 = dispense_guard
        if (cause != last_force_close_cause ||
            (uint32_t)(now_ms - last_force_close_log_ms) >= VALVE_FORCE_CLOSE_LOG_COOLDOWN_MS)
        {
            log_emit(LOG_VALVE_FORCE_CLOSE, LOG_LVL_CRITICAL, p, 2);      // ← q1 已覆盖
        }
    }
    return true;
}
```

> ★ **关键事实**：`valve.cpp:56` 的
> `VALVE_CAUSE_WEIGHT_ERROR = 1,   // dispense_guard 收到 EVENT_WEIGHT_ERROR`
> ⇒ **Valve 侧已经把"本次强关的来源是 dispense_guard"写进了日志的 `LOG_P_CAUSE` 参数里**。
> 也就是说：**从日志侧已经能分辨出这次关阀是不是 DispenseGuard 触发的** ——
> 在 DispenseGuard 里再记一条，是同一事实的第二份副本。

### 2.2 q2：收到 `EVENT_WEIGHT_ERROR` —— ❌ **重复，且是 N:1 冗余**

**两个独立的重复理由**：

1. **同一事实已记**：`weight.cpp` 已在 `weight_refresh_error_state()` 的 `error_state` **边沿**埋点
   ```cpp
   // weight.cpp:222-236（进入）
   if(error_state) {
       log_emit(LOG_WEIGHT_ERROR_ENTER, LOG_LVL_WARN, p, 3);   // CAUSE 位掩码 + 重量 + raw
   }
   ```
   且它**比 DispenseGuard 收到的更完整**（DispenseGuard 只拿到 `String data` 文本）。

2. **★ 计数语义被放大（更严重）**：
   `weight_refresh_error_state()` 是**状态边沿**记录 —— `err != error_state` 时才记
   ⇒ 一次持续 60 s 的重量异常 = **1 条** `LOG_WEIGHT_ERROR_ENTER`
   而 `weight_record_jump()`（`weight.cpp:252-276`）**每条跳变都 `event_push()` 一次**
   ⇒ 同一次异常在 DispenseGuard 侧的回调可能被**唤醒 N 次**（N 可达 5 次/5 s 窗口）
   ⇒ 若在 q2 埋点，同一次异常会记 **N 条**，与 weight 侧的 **1 条**形成 **N:1 冗余**

   > 注：`LOG_WEIGHT_ERROR_ENTER` 已在 LogManager 的**突发合并白名单**中
   > （`log_manager.cpp:141` `{ LOG_WEIGHT_ERROR_ENTER, LOG_LVL_WARN }`，窗口 `5000 ms`，
   >  `Σ LOG_P_COUNT` 守恒）—— 但**白名单是按 EventId 匹配的**，一条新 EventId 不会自动获得该保护。

### 2.3 q3：`valve_force_close()` 失败 —— ❌ **重复，且可观测性已由 P2-H 解决**

- `valve.cpp:405` 已埋 `LOG_VALVE_FORCE_CLOSE_FAILED`(0x0506, CRITICAL)
- `未修复的问题.md` 的 `VALVE-3` 已标 **✅ DONE（P2-H 已修复）**，原文：
  > `valve_force_close()` 的**失败分支完全不可观测** ⇒ ✅ **P2-H 已落地**…上板实测 **52 条** CRITICAL 记录
- ⚠️ **诚实补充（与 `VALVE-1` 的交点）**：`valve.cpp:398` 只判 `valve_pin < 0`。
  当阀门被 config **禁用但引脚已配置**时 `valve_pin >= 0`，会**跳过失败分支、返回 `true`**
  ⇒ DispenseGuard 会打印 `Valve force closed`（**假成功**）。
  但此时**也不会记录 q3** —— 因为 q3 分支根本没进。
  ⇒ 该场景的"关阀动作"仍会被 `valve.cpp:470` 的 `LOG_VALVE_FORCE_CLOSE` 记下（以 `CAUSE=1`）。
  ⇒ **结论不变：本模块无需为此补点**；`VALVE-1` 本身是生产代码缺陷（**OPEN，需单独批准修复**），
     **不属于日志接入范畴**。

### 2.4 q4：`event_subscribe()` 失败 —— ✅ **不重复（唯一合法埋点）**

`event_manager.cpp:392-420` 的 `event_subscribe()` 全文：

```cpp
bool event_subscribe(SystemEvent event, EventCallback callback)
{
    if (callback == nullptr)                       return false;   // ← 出口 1
    int eid = static_cast<int>(event);
    if (eid < 0 || eid >= MAX_EVENT_TYPE)          return false;   // ← 出口 2
    for (int i = 0; i < sub_count[eid]; i++)
        if (callback_table[eid][i] == callback)    return false;   // ← 出口 3（重复注册）
    if (sub_count[eid] >= MAX_EVENT_SUBSCRIBER)    return false;   // ← 出口 4（订阅表满）
    callback_table[eid][sub_count[eid]] = callback;
    sub_count[eid]++;
    return true;
}
```

**★ 4 个失败出口全部静默 —— 无 `Serial.print`、无 `log_emit`、无 System State。**
调用方 `main.cpp:500` `dispense_guard_init();` —— **忽略返回值**（`void` 函数，且无任何后续检查）。
`dispense_guard.h` 只有 `void dispense_guard_init();` —— **无查询接口**。

⇒ **该失败 100% 不可观测**（铁律 26：先证明"别人都看不见"，宿主归属就不言自明）。

**后果严重性**：订阅失败 ⇒ **重量异常 → 关阀** 这条安全链**永久失效**，
且 `EVENT_WEIGHT_ERROR` 依然会被 weight 正常发布（**看起来一切正常**）。

**为什么加一条不构成"重复"**：
- 时间上：**初始化期**（`main.cpp:500`）一次性事实，与运行期的 weight / valve 记录不重叠
- 语义上：主题是"**本模块失效**"，不是"重量异常"也不是"关阀" ⇒ 是**第三个独立事实**
- 且 `log_init()`（`main.cpp:444`）**早于** `dispense_guard_init()`（`main.cpp:500`）
  ⇒ RAM 环已就绪，记录不会因初始化未完成而丢

---

## 3. 裁决与落地

### 3.1 唯一合法埋点需要新 EventId

| 项 | 值 |
|---|---|
| 提议 EventId | `LOG_GUARD_SUBSCRIBE_FAILED` = **`0x0511`**（`0x0500` Water/Dispense 段尾，**从 `LOG_WEIGHT_CALIB_FAILED`(0x0510) 自然续号，不扩段**） |
| Level | `LOG_LVL_CRITICAL`（与 `LOG_VALVE_FORCE_CLOSE_FAILED` 0x0506 语义齐平 —— 都是"安全功能失效"） |
| 参数 | `LOG_P_MODULE`(0x1D，已冻结) + `LOG_P_CAUSE`(0x16，已冻结) |
| 依赖方向 | `dispense_guard → log_manager`（单向，与 P2-H Valve 同构） |
| 合同测试 | 建议同步 `log_events.h:597-602` 增加一条 `static_assert` |

### 3.2 用户裁决

> **「不加 EventId，本次零代码改动」**
> **「不增加」**（Level 问题一并作废）

⇒ **不落地**。`src/dispense_guard.cpp` 保持 73 行原状；`src/` 与 `test/` **零改动**。

**登记项**：`LV-1`（见 §5），建议与 `DSP-*` 一并进入 Phase 4 待决清单。

### 3.3 P2-N 最终埋点表

| # | 位置 | EventId | Level | 参数 | **落地** | 理由 |
|---|---|---|---|---|---|---|
| 1 | `dispense_guard_init()` 订阅失败分支 | ❌ 无（需 `0x0511`） | — | — | **否** | 唯一不重复的点，但无 EventId ⇒ 等裁决 |
| 2 | `dispense_guard_event_callback()` 过滤通过 | `LOG_WEIGHT_ERROR_ENTER` 0x050C（若复用） | WARN | — | **否** | **重复**：weight 已记 + N:1 放大 |
| 3 | `dispense_guard_event_callback()` 调用后 | `LOG_VALVE_FORCE_CLOSE` 0x0505（若复用） | CRITICAL | — | **否** | **重复**：valve 已记且带 5 s 门控 |
| 4 | `dispense_guard_event_callback()` 失败分支 | `LOG_VALVE_FORCE_CLOSE_FAILED` 0x0506（若复用） | CRITICAL | — | **否** | **重复**：valve 已记（VALVE-3 DONE） |

> **DispenseGuard 运行期埋点数 = 0** —— 这是**刻意的设计结论**，不是遗漏。
> 在项目词汇里 **"Dispense" 指供水**（`readme.md:96` Dispense Guard 定量**供水**保护模块；
> `log_events.h:264` 段注释 `0x05xx **Water**（Dispense / Valve / Weight）`）；
> 出粮模块另有其名（`readme.md:99/2737` **Motor**，`log_events.h:344` `0x0Fxx` 段已预留）。
> DispenseGuard 在可预见的未来都只是一个**安全转发器**，因此"零埋点"是它的稳态结论。

---

## 4. 验证

**V1 — 代码零改动（可复核）**

```
$ git diff --stat src/
（空）

$ wc -l src/dispense_guard.cpp
73 src/dispense_guard.cpp        # 与 HEAD 一致

$ grep -c "log_emit" src/dispense_guard.cpp
0
```

⇒ 无编译、无上板验证需求（未产生任何二进制差异）。

**V2 — 重复性事实核对（静态）**

| 断言 | 证据 |
|---|---|
| DispenseGuard 无任何日志引用 | `grep -n "log_emit\|log_manager\|LogParamIn\|LOG_" src/dispense_guard.cpp` → 零命中 |
| q1/q3 已被 valve 覆盖 | `grep -rn "LOG_VALVE_FORCE_CLOSE" src/` → `valve.cpp:405`(FAILED) + `valve.cpp:470`(CLOSE) |
| q2 已被 weight 覆盖 | `grep -n "log_emit" src/weight.cpp` → 5 处，含 `:236`(ENTER) / `:244`(EXIT) |
| q4 无任何覆盖 | `event_subscribe()` 4 个失败出口全部静默；`main.cpp:500` 忽略返回值；`dispense_guard.h` 无查询接口 |

**V3 — 全模块埋点宿主覆盖表（更新）**

| 模块 | `log_emit` 调用点数 | 状态 |
|---|---|---|
| `workflow.cpp` | 19 | ✅ P2-F |
| `time_manager.cpp` | 12 | ✅ P2-E |
| `valve.cpp` | 6 | ✅ P2-H |
| `cloud_manager.cpp` | 6 | ✅ P2-D |
| `weight.cpp` | 5 | ✅ P2-G |
| `command_manager.cpp` | 5 | ✅ P2-J |
| `wifi_module.cpp` | 5 | ✅ P2-C |
| `computer_reset.cpp` | 4 | ✅ P2-K |
| `config_manager.cpp` | 2 | ✅ P2-B |
| `capability_registry.cpp` | 2 | ✅ P2-L |
| `event_manager.cpp` | 2 | ✅ P2-M |
| **`dispense_guard.cpp`** | **0** | ⬜ **P2-N：无合法宿主（本报告结论）** |
| `oled.cpp` | 0 | ⛔ 已判无宿主不埋 |

⇒ 已接入 **12** 个模块；`dispense_guard` 与 `oled` 为**两类不同性质的"零埋点"**：
- `oled`：**无宿主**（`LOG_OLED_INIT_FAILED` 存在但判为无宿主）
- `dispense_guard`：**有宿主候选但无 EventId**（`LV-1`）

---

## 5. 未决 / 登记项

| ID | 内容 | 状态 | 说明 |
|---|---|---|---|
| **LV-1** | **`dispense_guard_init()` 的 `event_subscribe()` 失败不可观测** | 🟡 **OPEN（本报告登记）** | 订阅失败 ⇒ 安全链永久失效，静默。**需 1 个 EventId（建议 `0x0511`）** ⇒ 用户已裁决不加 ⇒ 保持零改动，登记备查。建议并入 Phase 4 待决清单（与 `DSP-*` 一起） |
| `VALVE-1` | `valve_force_close()` 等不检查 `initialized` ⇒ 禁用状态返回**假成功** | 🔴 OPEN（既有） | **生产代码缺陷**，需单独批准；**非日志范畴**，本报告只标注它与 §2.3 的交点 |
| `VALVE-5` | `dispense_guard` 无节流（每事件一次 `force_close`） | 🟡 OPEN（既有） | 已由 Valve 侧 5 s 日志门控兜住**日志侧**影响；**动作侧仍无节流** |

---

## 6. 方法论沉淀（新增铁律）

> ### ★ 铁律 29：**"纯转发模块"的埋点数天然为 0 —— 这是设计结论，不是遗漏。**
>
> 判据（三步）：
> 1. **列出该模块的全部数据流**（输入 / 内部 / 输出）
> 2. **逐个问"这个事实的定义点在哪"**（铁律 22）——
>    - 定义点在**别的模块** ⇒ 那一个模块记，本模块**不记**
>    - 定义点在**本模块** ⇒ 本模块记
> 3. **只剩"自己失效"这类自指事实**时 ⇒ 若协议里有 ID 就记，**没有就"零埋点 + 登记"**
>
> **DispenseGuard 的三重剥离**：
> - 输入事实（重量异常）→ 定义点 `weight.cpp` ⇒ **不记**
> - 输出事实（关阀）→ 定义点 `valve.cpp` ⇒ **不记**
> - 自指事实（订阅失败）→ 定义点本模块，但**无 EventId** ⇒ **登记 `LV-1`**
>
> **推论**：`event_manager.cpp`（P2-M）虽也是"总线"，但它**有自指事实**（队列满 / 风暴）⇒ 记 2 条；
> `dispense_guard.cpp` **连自指事实都没有 ID** ⇒ 记 0 条。
> **"总线类模块记几条"没有统一答案，必须逐模块剥离数据流。**

---

## 附：本轮核查的源文件

| 文件 | 用途 |
|---|---|
| `src/dispense_guard.cpp/.h` | 模块全文（73 行 / 3 行） |
| `src/valve.cpp` | `valve_force_close()` 埋点（:396-475）、`VALVE_CAUSE_*`（:55-59） |
| `src/weight.cpp` | `LOG_WEIGHT_ERROR_ENTER/EXIT` 边沿埋点（:222-246）、`weight_record_jump()`（:252-276） |
| `src/event_manager.cpp` | `event_subscribe()` 4 个静默失败出口（:392-420） |
| `src/log_events.h` | Water 段 EventId（:264-280）、ParamId（:140/:142）、Level Policy（:64-80）、合同测试（:597-642） |
| `src/log_manager.cpp` | 突发合并白名单（:137-145） |
| `src/main.cpp` | 初始化顺序（:444 `log_init()` → :500 `dispense_guard_init()`） |
| `readme.md` | 分层与"Dispense=供水 / Motor=投喂"命名事实（:96 / :99 / :41 / :2737） |
| `未修复的问题.md` | `VALVE-1` / `VALVE-3` / `VALVE-5` 现状 |

---

**最后更新**：2026-09-20 · P2-N DispenseGuard 埋点审查
**最终结论**：**埋点位置 = 0 个**；**全部运行期候选均重复**；唯一不重复点（订阅失败）**因缺 EventId 不落地** ⇒ `src/` 零改动。
