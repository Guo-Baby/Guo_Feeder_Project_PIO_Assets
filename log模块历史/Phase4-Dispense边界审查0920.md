# Phase 4 前置审查：Dispense 模块边界设计

> 日期：2026-09-20
> 阶段：Phase 4 前置（**纯架构审查，零代码改动**）
> 方法：**按代码事实分析**（`#include` 图 + 符号引用 + 数据流），不按旧文档名称推断
> 约束：禁改 `src/` · 禁改 `test/` · 禁新增 EventId / ParamId / System State / Config 参数 / enable 开关 · 禁改现有模块接口

---

## 0. 结论速览

| 问题 | 结论 |
|---|---|
| 1. 是否存在真实 Dispense 模块？ | **否**。只有 `dispense_guard.cpp/.h`（74 行，一个订阅 + 一个关阀调用） |
| 2. 电机 / 步进 / 出粮状态机 / 出粮 Action 是否存在？ | **全部不存在** |
| 3. 重量反馈闭环是否存在？ | **存在，但属于 Water 域，且由 Workflow 组合实现**（`VALVE_OPEN` → `weight_decrease` Trigger → `VALVE_CLOSE`），不是模块内闭环 |
| 4. 出粮是否应作 Workflow Action？ | **是**。且"Command Action"**不是独立层** —— 它是同一份 Descriptor 的第二种调用来源 |
| 5. Dispense 读重量用哪种方式？ | **必须用 `weight_get_gram()` 查询 API**，**绝不能读 System State**（陈值最长 30 s，有硬代码证据） |
| 6. DispenseGuard 能否守护出粮？ | **不能**（它只调 `valve_force_close()`）。出粮急停是 Dispense 自身状态机的终止分支 |
| 7. Dispense 日志 ID 是否已存在？ | **已存在 4 个且冻结**：`0x0501–0x0504`，但 **usage = 0**，且落在 **Water 段**（语义冲突，见 DSP-2） |
| 8. System State 要加几个？ | **建议 1 个**（`STATE_DISPENSE_ACTIVE`，bool）。三态枚举属内部状态机，不进 System State |

> ### ★ 本轮最重要的代码事实：命名冲突
> 项目现有词汇里 **"Dispense" = 供水/注水**，而**出粮模块在架构里已经叫 "Motor"**：
> - `readme.md:96`：**Dispense Guard 定量供水安全保护模块**
> - `readme.md:99`：**Motor 步进电机投喂模块，目前属于后续开发能力**
> - `readme.md:41`：应用/能力层清单 = `Valve Weight Dispense Guard Mijia / OLED **Motor** 其他未来业务模块`
> - `readme.md:2737`：`| Motor | 待开发 | 待开发 |`
> - `log_events.h:344`：`// ---- 0x0Fxx Motor（未来模块，ID 段已预留）----`
> - `log_events.h:264`：`// ---- 0x05xx Water（Dispense / Valve / Weight）----`
> - `未修复的问题.md:463`：`| — | **Dispense（注水过程）** |`
>
> ⇒ 本轮用户指令中的 **"Dispense（出粮）"** 与项目中既有的 **"Dispense（注水）"** 是两个不同事物。
> **推荐解法见 DSP-1：两层分离 —— `DispenseManager`（出粮业务编排，新名）+ `Motor`（驱动，沿用既有名）。**

---

## 1. 当前代码事实

### 1.1 已存在的模块（用户清单逐项核对）

| 层 | 用户列出 | 代码事实 | 文件 |
|---|---|---|---|
| Application | Valve | ✅ 存在 | `src/valve.cpp/.h`（475 / 43 行） |
| Application | Weight | ✅ 存在 | `src/weight.cpp/.h`（747 / 42 行） |
| Application | DispenseGuard | ✅ 存在（**但极简**） | `src/dispense_guard.cpp/.h`（74 / 3 行） |
| Application | MiThermometer | ✅ 存在 | `src/MiThermometer.cpp/.h` |
| Application | OLED | ✅ 存在 | `src/oled.cpp/.h` |
| Automation | Workflow Manager | ✅ 存在 | `src/workflow.cpp/.h`（4726 / 827 行） |
| Services | Command Manager | ✅ 存在 | `src/command_manager.cpp/.h`（3343 行） |
| Services | Event Manager | ✅ 存在 | `src/event_manager.cpp/.h` |
| Services | Log Manager | ✅ 存在 | `src/log_manager.cpp/.h` |
| Services | System State | ✅ 存在 | `src/system_state.cpp/.h` |
| Services | Config Manager | ✅ 存在 | `src/config_manager.cpp/.h` |
| Cloud | Cloud Manager | ✅ 存在 | `src/cloud_manager.cpp/.h` |

**用户清单未列出但确实存在的模块**（影响依赖方向判断）：

| 模块 | 层 | 说明 |
|---|---|---|
| `computer_reset` | Application | GPIO8 脉冲；已注册 `COMPUTER_RESET` Action |
| `time_manager` | Services | NTP + RTC(PCF8563T) |
| `capability_registry` | Automation | Stable ID 映射层（P2-L 已接入日志） |
| `wifi_module` | Services | WiFi 状态机 |
| `system_command` | Services | **全系统唯一 `ESP.restart()`** + 控制台命令 |
| `json_storage` / `file_storage` / `bin_storage` / `workflow_storage` | Storage | 四条持久化链路 |
| `HX711` | 第三方驱动 | 被 `weight.cpp` 直接包含（`#include <HX711.h>`） |
| `test_mqtt` | ⚠️ 测试代码 | `main.cpp:589` 注释：`//测试代码，需要删除` |

### 1.2 Dispense 相关的全部代码事实

**`src/dispense_guard.h`（全文 3 行）**
```cpp
#pragma once
void dispense_guard_init();
```
⇒ **零状态、零参数、零查询接口、零状态机。**

**`src/dispense_guard.cpp`（全文 74 行）** —— 只做一件事：
```cpp
static void dispense_guard_event_callback(const EventMessage &msg)
{
    if(msg.event != EVENT_WEIGHT_ERROR) return;
    Serial.println("[DispenseGuard] Weight error received");
    bool result = valve_force_close();
    ...
}
void dispense_guard_init()
{
    event_subscribe(EVENT_WEIGHT_ERROR, dispense_guard_event_callback);
}
```
⇒ **DispenseGuard 的能力范围 = "收到重量异常 → 关水阀"。没有任何电机/出粮概念。**

**`dispense` 关键字在全 `src/` 的命中点（共 8 处，无一处是模块本体）**

| 文件:行 | 内容 | 性质 |
|---|---|---|
| `dispense_guard.cpp:2/23/55/60` | 自身 | 模块本体 |
| `dispense_guard.h:3` | 自身 | 模块本体 |
| `main.cpp:21/500` | `#include` + `dispense_guard_init()` | 接线 |
| `valve.cpp:56/458` | `VALVE_CAUSE_WEIGHT_ERROR = 1, // dispense_guard 收到 EVENT_WEIGHT_ERROR` | 注释/常量语义 |

### 1.3 五项能力核查（逐项给证据）

| 能力 | 是否存在 | 证据 |
|---|---|---|
| **电机控制代码** | ❌ **不存在** | 全 `src/` 检索 `motor`/`stepper`/`auger`/`servo`/`ledc`/`PWM` → 仅命中 `log_events.h:345 LOG_MOTOR_RESERVED_BASE = 0x0F00` 与 `readme.md` 文字；**无任何 `.cpp` 实现** |
| **步进驱动代码** | ❌ **不存在** | 同上；`src/` 无 `stepper.*` / `motor.*` 文件 |
| **出粮状态机** | ❌ **不存在** | `src/` 无任何相关状态枚举 |
| **出粮 Action** | ❌ **不存在** | `workflow_register_action()` 全部 8 个调用点：<br>`valve.cpp:286/289`（VALVE_OPEN/CLOSE）、`weight.cpp:707`（WEIGHT_ZERO）、`computer_reset.cpp:413`、`MiThermometer.cpp:692/697`、`test_mqtt.cpp:278`、`workflow.cpp` 内置（timer/delay 是 Trigger）<br>⇒ **无出粮 Action** |
| **重量反馈闭环** | ⚠️ **部分存在，属 Water 域** | 闭环由 **Workflow 组合** 实现，不是模块内闭环：<br>① `valve.cpp:82` 注册 `VALVE_OPEN` Action<br>② `weight.cpp:122` 注册 `weight_decrease` **Trigger**（`weight.cpp:616`：`weight_loss = start_weight - current_weight; if(weight_loss >= trigger_gram) → TRIGGER_SUCCESS`）<br>③ `valve.cpp:94` 注册 `VALVE_CLOSE` Action<br>⇒ 即"开阀 → 等重量减少 N 克 → 关阀" |

### 1.4 ★ 明确标记

> ## **Dispense 当前属于未来 Capability，不提前虚构接口。**
>
> 依据：
> 1. `src/` 无 `dispense.cpp/.h`
> 2. `未修复的问题.md:463` 已登记：「⚠️ **模块本身不存在**（只有 `dispense_guard.cpp/.h`，无 `dispense.cpp`）」
> 3. `未修复的问题.md:550` 路线图 B7：「🔴 **模块本身不存在** ⇒ 移出埋点清单，归入 Phase 4 业务开发」
> 4. 需求文档全文检索 `出粮`/`电机`/`粮桶`/`螺旋`/`步进` → **零命中**（V2.1 需求只覆盖饮水系统）
>
> ⇒ 本阶段**只定义边界，不定义接口签名**。

---

## 2. Dispense 未来职责定义

### 2.1 推荐的两层拆分（与既有命名对齐）

```
DispenseManager（新增，业务编排层）
    ├── 接收出粮请求（Workflow Step Action / Command 临时 Action）
    ├── 管理出粮过程状态机（内部，不进 System State）
    ├── 用 weight_get_gram() 做闭环判停
    ├── 判定完成 / 超时 / 卡粮
    └── 输出 Action 结果（ACTION_SUCCESS / ACTION_FAILED）
                    │
                    ▼（单向依赖）
Motor Driver（新增，硬件驱动层，沿用 readme 既有名 "Motor"）
    ├── GPIO / PWM / 步进脉冲输出
    ├── 起停、速度设定
    └── 驱动级异常上报（堵转、无响应）
```

**为什么必须拆两层**（代码事实依据）：
- `readme.md:41/99/2737` 已把未来出粮模块命名为 **Motor**
- `log_events.h:344` 已为 **Motor** 预留 `0x0Fxx` 段
- `readme.md:1700-1726` 已给出一个**未实现的 payload 契约示例**：
  ```json
  {"command":"execute_action","object":"MOTOR_MOVE",
   "payload":{"speed":100,"position":500}}
  ```
  ⇒ 既有预期里 Motor 是 **speed/position（开环步进）**，而用户要求的是 **target_weight（闭环按重）**。
  ⇒ **这两者不是同一个 Action**（见 DSP-10），拆层后可由 `Motor` 提供开环能力、`DispenseManager` 在其上加闭环。

### 2.2 DispenseManager 输入（最小参数集）

| 参数 | 类型 | 单位 | 说明 |
|---|---|---|---|
| `target_g` | `PARAM_FLOAT` | 克 | 目标出粮重量。**闭环判据** |
| `timeout_ms` | `PARAM_INT` | ms | 本次出粮超时上限（**必填**，理由见 §3.4） |
| `speed` | `PARAM_INT` | — | 电机速度（可选，与 `MOTOR_MOVE` 的 `speed` 对齐） |

**硬约束**：`WORKFLOW_MAX_PARAM = 8`（`workflow.h:50`）。建议 **≤ 3 个**。

### 2.3 建议**不要**在 v1 引入的参数

| 被建议过 | 不引入的理由 |
|---|---|
| `mode`（开环/闭环枚举） | 增加一个枚举维度 ⇒ 状态机分支翻倍。开环 vs 闭环应由 **"是否传 `target_g`"** 表达，或拆成两个 Action（见 DSP-10） |
| `motor_steps` / `accel` / `current_limit` | 属 **Motor 驱动** 或 **Config**，不是业务请求参数 |
| `jam_threshold`（卡粮阈值） | 属 Config（且卡粮判据本身待定，见 DSP-5） |
| `calibration` | **已在 Weight 模块**（`config_get_weight_scale()` / `config_get_weight_zero_offset()`），**禁止复制**（见 §9.3） |

### 2.4 Dispense **不负责**（明确排除）

| 不负责 | 归属 |
|---|---|
| Workflow 调度 / 何时出粮 | `WorkflowManager` |
| MQTT 通信 / 云端协议 | `CloudManager` |
| 日志存储 / 落盘策略 | `LogManager` |
| Event 分发 | `EventManager` |
| Config 存储 / 参数校验落盘 | `ConfigManager` |
| 重量采样 / 跳变检测 | `Weight`（已有，禁止复制） |
| 关水阀安全动作 | `DispenseGuard`（已有） |
| 重启执行 | `SystemCommand`（全系统唯一 `ESP.restart()`） |

---

## 3. Workflow 与 Dispense 的关系

### 3.1 现有 Action 机制的代码事实

**Descriptor 契约**（`workflow.h:241-264`）：
```cpp
struct WorkflowActionDescriptor {
    const char *id, *name, *module, *description;
    WorkflowParam *params;  uint8_t param_count;
    void (*reset)(WorkflowActionInstance *);
    void (*start)(WorkflowActionInstance *);
    void (*poll )(WorkflowActionInstance *);
};
```
**Instance 契约**（`workflow.h:299-321`）：
```cpp
struct WorkflowActionInstance {
    const WorkflowActionDescriptor *descriptor;
    String id;
    WorkflowParamValue params[WORKFLOW_MAX_PARAM];  // 8
    uint8_t param_count;
    WorkflowActionResult result;   // IDLE / RUNNING / SUCCESS / FAILED
    bool running;
    void *runtime;                 // ★ 模块私有上下文（注释原文："电机、阀门等使用"）
    WorkflowActionCallback callback;
};
```
> ★ `workflow.h:317` 的注释原文就是 **"电机、阀门等使用"** —— 架构在设计时已为电机预留了 `runtime` 槽位。

**两条调用路径（代码事实）**：

```
路径 A：Workflow Step
  workflow_task()  ──► step.instance.action ──► start() / poll()
  workflow.cpp:4296-4356，受 wf.timeout_ms 外约束（默认 600000 ms）

路径 B：Command 临时 Action
  CloudManager ──► CommandManager::command_execute_action()      (command_manager.cpp:746)
              ──► workflow_enqueue_action(id, payload, ...)      (workflow.h:738)
              ──► temp_action_queue（FIFO）                       (workflow.cpp:4360)
              ──► 引擎驱动 start()/poll()                        (workflow.cpp:4419-4449)
              ──► command_temp_action_callback ──► 云端结果
```

### 3.2 "Command Action" 是不是独立层？ —— **不是**

代码事实：`workflow_enqueue_action()`（`workflow.h:738`）的第一个参数就是 **action id**，它查的是**同一张 Action 注册表**。
`valve.cpp:22-24` 的既有契约写得很清楚：

> ```
> //   - 同一份 Action 描述符同时支持 Workflow Step 调用与 Command 临时 Action 调用
> //     （模块只实现 descriptor 的 reset/start/poll，不感知调用来源）
> ```

⇒ **Dispense 只需实现一份 Descriptor，两条路径自动都通。不需要"Command Action"这一层。**

### 3.3 推荐链路（对用户建议的修正）

用户建议：
```
WorkflowManager ↓ Command Action ↓ DispenseManager ↓ Motor
```

**修正后**（两条并列，共用同一份 Descriptor）：

```
【编排路径】 WorkflowManager(Step Action) ─────┐
                                              ├─► DispenseManager(Action Descriptor) ──► Motor Driver ──► 硬件
【直控路径】 Cloud → CommandManager            │
             → workflow_enqueue_action ───────┘
             → command_temp_action_callback → Cloud

【安全旁路】 EventManager(EVENT_WEIGHT_ERROR) → DispenseGuard → valve_force_close()   ← 与出粮无关，保持不动
```

**Workflow 不直接控制 GPIO** —— 这条原则在现有代码中已被严格遵守：`workflow.cpp` **零处** `gpio_set_level()`；GPIO 只在 `valve.cpp:150/412` 与 `computer_reset.cpp` 出现。⇒ Dispense 必须同样把 GPIO 关在 Motor 驱动层内。

### 3.4 ★ 队头阻塞风险（必须写进设计约束）

`workflow.cpp:4462-4471`：
```cpp
if(!item.completed)
{
    // 队头未完成，等待下一次workflow_task()
    break;
}
```
⇒ **Temp Action 队列是严格 FIFO + 队头阻塞**，容量 `MAX_TEMP_ACTIONS = 8`（`workflow.cpp:146` 注释：实际可用 7 个）。

**后果**：出粮是**数十秒级**长时动作。它一旦占住队头，**后续所有临时 Action（含关阀、查询类）会被全部堵住**，直到它完成或超时。

**默认值陷阱**：`command_manager.cpp:78` `COMMAND_ACTION_TIMEOUT_MS = 600000UL`（10 分钟），`workflow_enqueue_action()` 默认 `timeout_ms = 600000`（`workflow.h:744`）。
⇒ 若 Dispense 不显式传 `timeout_ms`，一次卡粮会**堵死临时 Action 队列 10 分钟**。

**设计约束**：
1. `timeout_ms` **必须作为必填参数**（或模块内强制钳位到保守上限，建议 ≤ 60 s）
2. 出粮期间硬件必须能被**独立停止**（Dispense 自身的超时分支，不依赖 CommandManager 超时）
3. ⇒ 这也是 §2.2 把 `timeout_ms` 列为**必填参数**的直接理由

---

## 4. WeightManager 与 Dispense 的关系

### 4.1 Weight 现有职责（代码事实，`weight.cpp`）

| 职责 | 位置 | 说明 |
|---|---|---|
| HX711 采样 | `weight.cpp:303-513` | `is_ready()` 触发，非阻塞，每次读一次 |
| 5 点窗口滤波 | `weight.cpp:157-177` | 去最大去最小取平均 |
| 当前重量 | `weight.cpp:729` `weight_get_gram()` | 返回模块内 `current_weight` |
| 异常检测 | `weight.cpp:189-247` | 无数据 5 s / raw 零 5 s / 5 s 内 5 次跳变 |
| System State | `weight.cpp:284-293` | `STATE_WEIGHT_VALUE` / `STATE_WEIGHT_ERROR` |
| Event 发布 | `weight.cpp:268/384/557` | `EVENT_WEIGHT_ERROR` |
| Workflow Trigger | `weight.cpp:122` | `weight_decrease`（重量减少 N 克） |
| Workflow Action | `weight.cpp:141` | `WEIGHT_ZERO`（零点校准） |

### 4.2 ★ 决定性代码事实：System State 的重量是**陈值**

```cpp
// weight.cpp:59-61
#define WEIGHT_IDLE_STATE_UPDATE_MS 30000UL

// weight.cpp:284-293
static void weight_update_state_value()
{
    unsigned long now = millis();
    if(weight_active ||                                    // ← 只在 Trigger 运行时为真
       (now - last_state_update_ms >= WEIGHT_IDLE_STATE_UPDATE_MS))   // ← 否则 30 秒才更新一次
    {
        state_set_float(STATE_WEIGHT_VALUE, current_weight);
        last_state_update_ms = now;
    }
}
```

而 `weight_active` 只在 `weight_decrease` **Trigger** 运行时被置真：
```cpp
// weight.cpp:592
weight_active = true;    // 仅 weight_trigger_start() 内
```

⇒ **结论（三种方案的裁决）**：

| 方案 | 裁决 | 理由 |
|---|---|---|
| **A. Dispense 读 System State 重量** | ❌ **禁止** | `weight_active=false` 时 `STATE_WEIGHT_VALUE` **最长 30 s 才更新一次**。出粮控制用它会拿到陈值 ⇒ 严重过冲 |
| **B. WeightManager 提供查询 API** | ✅ **推荐** | `weight_get_gram()` **已存在**（`weight.cpp:729`），返回模块内 `current_weight`，**每 ~0.5 s 更新**（5 点 × 10 Hz）。**零新增接口** |
| **C. 事件通知** | ⚠️ **不作为主反馈** | ① `EVENT_POLICY_STATE` 是覆盖式、只传 `String data`，无法承载数值契约；② 事件到 Workflow 订阅受 `EVT-1` 缺陷阻断（见 §6）；③ 采样是"就绪即算"，事件化等于把 0.5 s 周期的事实变成异步通知，无收益 |

### 4.3 反馈精度约束（必须写进设计）

- 重量更新周期 **≈ 500 ms**（`weight.cpp:42` 注释：「HX711 约 10Hz，每 5 次有效采样计算一次重量，约 0.5s 更新一次」）
- ⇒ **闭环控制分辨率 ≈ 2 Hz**
- ⇒ 出粮判停必须考虑**过冲**：机械惯性 + 0.5 s 观测延迟
- ⇒ 建议 **Dispense 侧做提前量补偿**（在 `target_g - margin` 处减速/停转），**不要要求 Weight 提高采样率**（会牵动 `R-8` 与滤波窗口，属 Weight 内部冻结边界）

### 4.4 ⚠️ 跨域耦合风险（新增发现）

`R-8`（混合窗口假跳变，**未修**，见 `未修复的问题.md`）会在重量抖动时误发 `EVENT_WEIGHT_ERROR`
⇒ 经 `dispense_guard` ⇒ `valve_force_close()` ⇒ **关水阀**。

出粮过程中电机振动很可能引发重量抖动 ⇒ **出粮会连带误关水阀**。
⇒ 已登记为 **DSP-6**。

---

## 5. DispenseGuard 关系

### 5.1 DispenseGuard 的能力边界（代码事实）

| 项 | 事实 |
|---|---|
| 订阅 | **仅** `EVENT_WEIGHT_ERROR`（`dispense_guard.cpp:59`） |
| 动作 | **仅** `valve_force_close()`（`dispense_guard.cpp:37`） |
| 判据 | **无**（无条件转发，见 `VALVE-5`） |
| 与电机 | **零耦合**（`dispense_guard.cpp` 只 `#include` 了 `Arduino.h` / `dispense_guard.h` / `event_manager.h` / `valve.h`） |

⇒ **DispenseGuard 不具备停止电机的能力，也没有任何出粮概念。**

### 5.2 异常归属边界

| 异常 | 归属 | 理由 |
|---|---|---|
| 电机运行超时 | **Dispense** | "本次业务失败"，Dispense 自己停电机 + 置 `ACTION_FAILED` + 记 `LOG_DISPENSE_TIMEOUT` |
| 达不到目标重量 | **Dispense** | 同上，业务判据在 Dispense |
| 卡粮（重量不增 / 堵转） | **Dispense（判据）+ Motor（原始信号）** | "多久没增重算卡粮"是业务策略；堵转电流/无响应是驱动事实 |
| 电机驱动级故障（GPIO 失败 / 无响应） | **Motor 驱动** | 硬件事实的定义点在驱动层（铁律 22/25） |
| 重量突变 / 传感器异常 | **Weight（检测）** | 已有，不动 |
| 安全关闭水阀 | **DispenseGuard** | 唯一职责，不动 |
| **出粮时电机必须急停** | **Dispense 自身终止分支** | Guard 没有 stop-motor 能力；硬塞进去需改 `dispense_guard` 接口（本轮禁止） |

### 5.3 原则性建议

> **不要把 DispenseGuard 改造成"通用安全层"。**
>
> 理由（代码事实）：
> 1. 它现在的价值恰恰来自 **"单一、高优先级、可穷举验证"** —— 只有一条输入、一条输出
> 2. 一旦加入电机分支，它的订阅矩阵与动作矩阵都变成 2×2，`VALVE-5`（无节流）这类已知缺陷的修复复杂度翻倍
> 3. 出粮的急停**语义不同**：关阀是"断电即安全"（弹簧复位），停电机是"断电即停止"—— 两者都由**执行模块自身的终止分支**完成更直接
>
> ⇒ **建议：DispenseGuard 保持"水安全"职责不变；出粮安全由 Dispense 状态机的终止分支 + Motor 驱动的硬件兜底（类似 Valve 的 `safety_timeout_ms`）承担。**
> ⇒ 若未来确实需要跨域统一安全层，应**新建**模块而不是扩展 Guard（登记为 **DSP-7**）。

---

## 6. Event 设计原则（**不新增，只列候选**）

### 6.1 硬约束（先说清楚为什么"现在不能加"）

`event_manager.h:35-38` 的注释规定了新增事件的位置与同步要求：
```cpp
//所有workflow event trigger对应的新增事件放在event_error之前
//并且同步到cpp文件内的枚举函数SystemEvent event_from_string(const String &str);
//以及String event_to_string(SystemEvent event);
//SYSTEM_EVENT_COUNT 自动更新，无需维护
```

而 **EVT-1 缺陷（未修）**：
```cpp
// event_manager.h:53
#define SYSTEM_EVENT_COUNT (EVENT_ERROR + 1)      // = 14
```
但 `EVENT_VALVE_OPEN=14 / CLOSE=15 / ERROR=16` 已越界 ⇒ **`event_from_string()` 永远解析不到阀门事件**，Workflow 触发器静默失败。

⇒ **现在新增任何 SystemEvent，只要它需要被 Workflow 订阅，就会撞同一个缺陷。**

### 6.2 未来候选事件（仅供评估，本轮不新增）

| 候选事件 | 建议发布方 | 说明 |
|---|---|---|
| `EVENT_DISPENSE_STARTED` | **Dispense** | 业务事实的定义点在 Dispense |
| `EVENT_DISPENSE_COMPLETED` | **Dispense** | 同上 |
| `EVENT_DISPENSE_FAILED` | **Dispense** | 同上 |
| `EVENT_MOTOR_STALL` | **Motor 驱动** | 硬件事实的定义点在驱动 |
| `EVENT_MOTOR_TIMEOUT` | **Motor 驱动** | 同上 |

### 6.3 判定：**哪些不该由 Dispense 发布**

| 事件 | 不该由 Dispense 发布的理由 |
|---|---|
| `EVENT_WEIGHT_ERROR` | 属于 **Weight**（`weight.cpp:268/384/557` 已发布） |
| `EVENT_VALVE_*` | 属于 **Valve** |
| 命令执行结果 | 属于 **CommandManager**（`EVENT_COMMAND_RESULT` 已存在） |
| 出粮完成 → 但作为 Workflow 结论 | 由 Workflow 的 `ACTION_SUCCESS` 表达，**不需要额外事件** |

**原则**：铁律 22（定义点 ≠ 触发点）+ 铁律 25（总线只搬运，不代记）。
⇒ **Dispense 只发布"出粮过程自身的业务事实"；重量、阀门、命令结果一律不代发。**

### 6.4 建议

1. **v1 可以不加任何事件** —— 出粮结果已经能通过 `ACTION_SUCCESS/FAILED` + `LOG_DISPENSE_*` 完整表达，事件是可选的（OLED/Cloud 订阅才需要）
2. **若必须加，先修 `EVT-1`**（登记为 **DSP-3**）
3. 事件**不是**重量反馈通道（见 §4.2 方案 C）

---

## 7. Log 埋点宿主规划

### 7.1 ★ 现有 ID 事实：4 个 Dispense ID 已冻结存在，但 usage = 0

```cpp
// log_events.h:264-268
// ---- 0x05xx Water（Dispense / Valve / Weight）----
LOG_DISPENSE_START             = 0x0501,   // INFO
LOG_DISPENSE_DONE              = 0x0502,   // INFO
LOG_DISPENSE_FAILED            = 0x0503,   // WARN
LOG_DISPENSE_TIMEOUT           = 0x0504,   // WARN
```

全 `src/` 检索 `LOG_DISPENSE` ⇒ **只命中 `log_events.h` 的 4 个定义行，零个 `log_emit` 调用**
⇒ **已冻结、无宿主**（与 `未修复的问题.md:463` 登记一致：「4 个全部 usage=0」）

同时：
```cpp
// log_events.h:344-345
// ---- 0x0Fxx Motor（未来模块，ID 段已预留）----
LOG_MOTOR_RESERVED_BASE        = 0x0F00
```

### 7.2 语义冲突（登记为 DSP-2）

| 事实 | 含义 |
|---|---|
| `0x0501–0x0504` 在 **Water** 段 | 项目原意：Dispense = **注水过程** |
| `0x0Fxx` 段留给 **Motor** | 项目原意：Motor = **投喂（出粮）** |
| 本轮用户要求 | Dispense = **出粮**（电机） |

⇒ **两个可选方案**（均需一次决策，本轮禁止新增 ID 故只提建议）：

| 方案 | 做法 | 优点 | 缺点 |
|---|---|---|---|
| **A. 沿用 0x0501–0x0504** | 出粮直接用这 4 个，把段注释从 `Water` 改为 `Dispense(Food+Water)` | **零新增 ID**（符合"不新增协议元素"） | 段名与 `DispenseGuard`(供水) 长期语义混杂 |
| **B. 在 0x0Fxx 新开 0x0F01–0x0F04** | 出粮走 Motor 段，0x0501–0x0504 留给未来真正的"供水过程"模块 | 语义干净，与 readme 的 Motor 命名一致 | **需新增 4 个 EventId**（违反本轮冻结约束，须单独立项评审） |

### 7.3 宿主分配（按铁律 25：谁"定义"事实谁记）

| 层 | 记录内容 | 说明 |
|---|---|---|
| **DispenseManager** | `START` / `DONE` / `FAILED` / `TIMEOUT` | 业务结果的**定义点**在此 |
| **Motor 驱动** | 驱动异常（堵转、无响应、GPIO 失败） | 硬件事实的**定义点**在驱动 |
| **Weight** | 重量异常 | **已有**（`LOG_WEIGHT_ERROR_ENTER/EXIT` 等），不动 |
| **EventManager** | ❌ **不记** Dispense 业务事件 | P2-M 已定：只记自身异常（队列满 / 风暴） |
| **Workflow** | 只记 `LOG_WF_*` 编排层事实 | **不替 Dispense 记业务结果**（会重复） |
| **DispenseGuard** | 不新增 | 其关阀动作已由 Valve 侧 `LOG_VALVE_FORCE_CLOSE` 覆盖 |

### 7.4 高频告警约束（沿用既有铁律）

- 铁律 5：**失败是状态不是事件** ⇒ 用**边沿锁**而非降频
- 铁律 6：高频路径用 **N 次记 1 且 ≥60 s** 聚合（`Σ COUNT = 真实次数`）
- 铁律 13：**门控只能加在"全部执行完成之后”**，绝不提前 return
- 铁律 14：**"取值用于日志"的变量须在源头清零前取**（`OPEN_MS` 在 Valve 踩过 3 次）

---

## 8. System State 需求

### 8.1 现有 System State 风格（代码事实）

`system_state.h:20-61` 全部条目中，业务模块相关的是：
```cpp
STATE_VALVE_STATUS,      // bool
STATE_WEIGHT_VALUE,      // float
STATE_WEIGHT_ERROR,      // bool
```
⇒ **现有风格 = "一个事实一个 bool/float"，没有任何三态枚举。**
（三态是 `STATE_WIFI_STATE`（0 idle/1 connecting/2 connected/3 disconnected）这样的**通信链路状态机**，不是业务模块状态机。）

### 8.2 建议

| 候选 | 建议 | 理由 |
|---|---|---|
| `STATE_DISPENSE_ACTIVE`（bool） | ✅ **建议加 1 个** | **系统级**：OLED 需要显示"正在出粮"、Cloud 需要上报、其它模块需要互斥判断 |
| `DISPENSE_IDLE / DISPENSING / ERROR` 三态 | ❌ **不加** | 这是 **Dispense 内部状态机**，放模块内 `static`。塞进 System State 会破坏"全局状态中心只放跨模块共享事实"的原则 |
| 出粮进度（已出 g / 目标 g） | ❌ **不加** | 高频（2 Hz）、只在过程内有意义；应通过**命令结果 + 日志参数**携带 |
| `STATE_DISPENSE_ERROR`（bool） | ⚠️ **暂缓** | 先看是否需要跨模块消费；若只有 Dispense 自己用，就不进 |

### 8.3 原则复述

> **不要把模块内部状态全部塞入 System State。**
>
> 判据（可直接复用）：**"有没有第二个模块需要读它？"**
> - 有 ⇒ 系统级，进 System State
> - 没有 ⇒ 模块内部 `static`
>
> 现有反例可鉴：`STATE_WEIGHT_VALUE` 因为更新策略（30 s）与真实值（0.5 s）脱节，已经造成"读 System State 会拿到陈值"的陷阱（§4.2）。⇒ **新加状态必须明确"谁写、多久写一次、读者期望的新鲜度"。**

---

## 9. Storage / Config 需求

### 9.1 三类数据分离

| 类别 | 例子 | 归属 | 落哪里 |
|---|---|---|---|
| **配置数据** | GPIO 引脚、默认速度、默认目标重量、超时上限、卡粮判据 | **ConfigManager** | `data/config/<module>.json` |
| **运行状态** | 本次出粮进度、电机当前相位/步数、状态机当前态 | **Dispense / Motor 内部 `static`** | **不持久化** |
| **历史数据** | 校准历史、累计出粮统计、失败记录 | ⚠️ **暂不实现** | 无既有 Storage 归属，需单独评审 |

### 9.2 新增 Config 模块的改动面（代码事实）

`config_manager.cpp:33` `#define CONFIG_MODULE_COUNT 8`，`kModuleNames[]`（`:65-73`）= `wifi / oled / valve / time / weight / mqtt / mi_thermo / rtc`。
`data/config/` 实际文件：`mi_thermo / mqtt / oled / rtc / time / valve / version / weight / wifi`（9 个，含 `version.json`）。

⇒ 新增一个配置模块需改 **4 处**（项目既有约定）：
1. `CONFIG_MODULE_COUNT` 8 → 9
2. `kModuleNames[]` 追加
3. `CONFIG_MODULE_<NAME>` 宏（`config_manager.h:50-57`）
4. `data/config/<name>.json`

参考 `data/config/valve.json` 的形状（Dispense 配置可直接类比）：
```json
{
    "gpio_pin": 12,
    "active_level": 1,
    "open_duration_ms": 0,
    "safety_timeout_sec": 300,
    "enable": true
}
```

### 9.3 ⚠️ 两个必须先踩过的坑

**坑 1：配置 getter 与实现脱节（项目内已有先例）**
```cpp
// config_manager.h:616
int config_get_weight_filter_samples();   // 存在
// weight.cpp:44
#define WEIGHT_FILTER_WINDOW 5            // 实现用的是硬编码 5
```
⇒ **不要复制这个模式**。Dispense 的每一项配置必须有唯一的真实来源。

**坑 2：校准参数禁止复制**
```cpp
// config_manager.h:611-616
int   config_get_weight_dt();
int   config_get_weight_sck();
float config_get_weight_scale();
long  config_get_weight_zero_offset();
```
⇒ 比例因子 / 零点偏移 **已在 Weight 模块 + `weight.json`**。Dispense **不要**再建一套（否则两套零点会漂移）。

### 9.4 不提前实现

- 校准历史：**不实现**（无 Storage 归属，需单独评审）
- 统计数据：**不实现**
- 出粮记录持久化：**不实现**（日志已覆盖）

---

## 10. 未决问题列表

> 编号规则新增 **`DSP-n`** 前缀（与既有 `VALVE-n` / `LOG-n` / `R-n` / `EVT-n` / `WF-n` 并列）。
> ⚠️ 本轮**未**写入 `未修复的问题.md`（本阶段只提交本报告），待用户确认后再登记。

| ID | 问题 | 影响 | 建议 |
|---|---|---|---|
| **DSP-1** | **命名**：Dispense / Motor / 两层分离？现有代码里 "Dispense" = 供水，"Motor" = 投喂 | 影响模块名、Action ID、Config 模块名、日志段 | **推荐两层分离**：`DispenseManager`（出粮编排）+ `Motor`（驱动，沿用 readme 既有名） |
| **DSP-2** | **EventId 段**：出粮用 `0x0501–0x0504`（零新增但撞 Water 语义）还是 `0x0Fxx` 新开（干净但需协议变更） | 影响 log_events.h（冻结） | 方案 A（沿用）最省；方案 B 需单独立项。**本轮不新增任何 ID** |
| **DSP-3** | **`EVT-1` 是否先修**：`SYSTEM_EVENT_COUNT=14` 越界 ⇒ 新增事件无法被 Workflow 订阅 | 阻塞"出粮事件驱动 Workflow"的设计 | 若 v1 不需要事件订阅 ⇒ 可不修；需要则**先修 EVT-1** |
| **DSP-4** | **Temp Action 队头阻塞**：出粮长时动作会堵死整个 temp action 队列（`workflow.cpp:4462`），默认超时 600 s | 阻塞所有云端即时命令 | `timeout_ms` **必填**并强制钳位（建议 ≤60 s）；出粮必须能自我终止 |
| **DSP-5** | **反馈精度**：重量更新 2 Hz + `R-8` 假跳变（未修）⇒ 过冲与误判如何补偿？是否在出粮期抑制跳变检测？ | 影响出粮精度与误报 | 建议 Dispense 侧做提前量补偿，**不动 Weight**；抑制跳变需单独评审 |
| **DSP-6** | **跨域误动作**：出粮振动 → 重量抖动 → `EVENT_WEIGHT_ERROR` → `dispense_guard` → **误关水阀** | 🔴 安全功能被无关业务污染 | 需评估"出粮期是否抑制水安全链"，**属安全逻辑，须单独评审** |
| **DSP-7** | **DispenseGuard 是否升级为通用安全层**（含停电机）？ | 影响 Guard 的单一性与可验证性 | **建议不升级**：保持水专用；出粮急停由 Dispense 终止分支 + Motor 硬件兜底承担 |
| **DSP-8** | **System State 是否加** `STATE_DISPENSE_ACTIVE`？加几个？ | 影响 `system_state.h/.cpp` state_map（冻结） | 建议**只加 1 个 bool**；三态与进度不进 |
| **DSP-9** | **Config 新增模块名**（`motor` / `dispense` / 两个都加） | 影响 `CONFIG_MODULE_COUNT`、Capability Registry stable_id **整体平移**（既有铁律：新增会让后续 stable_id 平移） | 建议跟随 DSP-1 的命名决策；**注意 stable_id 平移风险** |
| **DSP-10** | **开环 vs 闭环**：readme 的 `MOTOR_MOVE` payload 是 `{speed, position}`（开环），用户要求是 `target_g`（闭环）。是否要两个 Action？ | 影响 Action 数量与参数集 | 建议**先只做闭环一个 Action**（`DISPENSE`），开环 `MOTOR_MOVE` 留到有真实需求时再加 |

### 优先级建议

```
P0（开工前必须定）：DSP-1 命名 · DSP-2 日志段 · DSP-4 超时/队头阻塞
P1（设计阶段定）  ：DSP-5 精度 · DSP-8 System State · DSP-9 Config 模块名 · DSP-10 Action 拆分
P2（可延后）      ：DSP-3 EVT-1 · DSP-6 跨域误动作 · DSP-7 Guard 升级
```

---

## 附：本轮核查的源文件清单

| 文件 | 用途 |
|---|---|
| `src/dispense_guard.cpp/.h` | DispenseGuard 能力边界（全文读） |
| `src/valve.cpp/.h` | 执行机构 + Action Descriptor 范式（全文读） |
| `src/weight.cpp/.h` | 重量反馈 API 与更新策略（全文读） |
| `src/workflow.h` | Action/Trigger 契约、参数上限、Instance `runtime` 槽 |
| `src/workflow.cpp` | Step Action 引擎（:4180-4356）、Temp Action FIFO（:4340-4477） |
| `src/command_manager.cpp` | `command_execute_action()`（:746-799）、超时常量（:78-79） |
| `src/event_manager.h` | SystemEvent 枚举、`SYSTEM_EVENT_COUNT`、EventPolicy |
| `src/log_events.h` | EventId 段划分（:203-346）、ParamId 表（:140-201） |
| `src/system_state.h` | System State 全量条目与风格 |
| `src/config_manager.h/.cpp` | `CONFIG_MODULE_COUNT`、模块名表、valve/weight getter 范式 |
| `src/main.cpp` | 六层初始化顺序（:440-520）、loop 任务序列（:572-592） |
| `readme.md` | 分层架构图（:30-60）、Motor 定义（:99/:2737）、MOTOR_MOVE payload 示例（:1700-1726） |
| `未修复的问题.md` | Dispense 无宿主登记（:463）、路线图 B7（:550）、EVT-1 专节 |

---

**最后更新**：2026-09-20 · Phase 4 前置审查 · **零代码改动**（`src/` 与 `test/` 均未触碰）
**下一步**：用户裁决 DSP-1 / DSP-2 / DSP-4 后，进入 Dispense 详细设计。
