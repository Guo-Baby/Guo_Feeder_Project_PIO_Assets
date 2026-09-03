# Computer Reset Action 功能需求文档

## 1. 功能目标

新增 `Computer Reset` 硬件 Action，用于通过 ESP32-S3 的 GPIO8 控制外部继电器，从而模拟电脑主板 Reset Switch 的一次按键操作。

执行 `computer_reset` Action 时：

1. GPIO8 输出 HIGH。
2. 保持 HIGH **800 ms**。
3. 800 ms 到达后 GPIO8 恢复 LOW。
4. Action 完成。

该功能必须采用现有 WorkflowManager / Temporary Action 架构执行，不新增独立的 Command 执行机制。

---

## 2. 系统架构

必须遵循现有项目架构：

```text
MQTT
  ↓
CloudManager
  ↓
CommandManager
  ↓
WorkflowManager
  ↓
Temporary Action
  ↓
Computer Reset Action
  ↓
GPIO8
```

`CommandManager` 不直接调用 `computer_reset()` 硬件函数。

`computer_reset` 必须作为一个标准 Action 注册到 WorkflowManager / Capability Registry 所使用的现有 Action 体系中。

这样既可以支持：

* 云端直接执行 `computer_reset` Action
* Workflow 中调用 `computer_reset` Action
* 未来其他 Trigger 调用 `computer_reset`

所有执行路径最终都必须进入同一个 Computer Reset Action 实现。

---

## 3. 新增文件

建议新增：

```text
src/computer_reset.h
src/computer_reset.cpp
```

具体接口名称应遵循项目现有 Action 的接口规范，不要为了本功能重新设计一套 Action API。

如果现有项目已经存在类似硬件 Action 的实现，应优先按照现有实现模式编写。

---

## 4. GPIO 定义

Computer Reset 使用：

```text
GPIO8
```

建议定义为：

```cpp
constexpr uint8_t COMPUTER_RESET_PIN = 8;
```

不要在多个位置直接硬编码 `8`。

---

## 5. 初始化要求

Computer Reset 模块初始化时必须将 GPIO8 设置为安全状态 LOW。

要求：

```cpp
pinMode(COMPUTER_RESET_PIN, OUTPUT);
digitalWrite(COMPUTER_RESET_PIN, LOW);
```

初始化完成后，GPIO8 必须保持 LOW。

### 安全原则

GPIO8 的 HIGH 状态只能由 Computer Reset Action 的执行流程产生。

除 Computer Reset 模块之外，不应存在其他模块主动控制 GPIO8。

---

## 6. Action 执行行为

`computer_reset` Action 的执行逻辑：

```text
START
  ↓
GPIO8 = HIGH
  ↓
记录开始时间
  ↓
进入 RUNNING / WAIT 状态
  ↓
持续非阻塞检查 elapsed time
  ↓
elapsed >= 800 ms
  ↓
GPIO8 = LOW
  ↓
ACTION COMPLETE
```

严禁使用：

```cpp
delay(800);
```

不得阻塞主循环、WorkflowManager、CommandManager 或其他系统任务。

必须使用现有项目的非阻塞 Action / Workflow 状态机机制，例如基于 `millis()` 的时间判断。

---

## 7. HIGH 持续时间

固定持续：

```text
800 ms
```

本版本不需要从 Command Payload 或 ConfigManager 获取该时间。

不要增加可配置参数。

不要修改 ConfigManager。

---

## 8. Action Reset / 完成 / 异常安全要求

无论 Action 因为什么原因离开执行状态，都必须确保 GPIO8 最终为 LOW。

至少包括：

```text
正常完成
Action reset
Workflow reset
Action 被取消
异常退出
```

所有这些路径都必须确保：

```cpp
digitalWrite(COMPUTER_RESET_PIN, LOW);
```

不得出现 Action 已经结束但 GPIO8 仍保持 HIGH 的情况。

---

## 9. Critical Operation 要求

本 Action **不需要使用 System Command 的 Critical Operation 机制**。

不要调用：

```cpp
system_command_critical_operation_acquire();
```

也不要调用：

```cpp
system_command_critical_operation_release();
```

原因：

Computer Reset 只是一个持续 800 ms 的 GPIO 脉冲。

它不像以下操作：

* Flash 写入
* Config 持久化
* RTC 写入
* 阀门保持开启
* 电机持续运行
* 其他可能因 ESP32 Restart 而造成危险状态或数据损坏的操作

Computer Reset 本身不属于需要阻止 ESP32 Restart 的 Critical Operation。

因此：

```text
Computer Reset Action
        │
        └── 不占用 Critical Operation
```

---

## 10. Restart 场景

ESP32 Restart 与 Computer Reset 是两个完全不同的概念。

本模块不得修改 System Command 的 Restart 机制。

不得调用：

```cpp
ESP.restart();
```

Computer Reset Action 只负责：

```text
GPIO8 HIGH → 800 ms → GPIO8 LOW
```

---

## 11. Action 注册

必须将 Computer Reset 注册为现有 Action。

Action 的 Stable ID / Runtime ID / capability mapping 必须遵循项目现有 Capability Registry 机制。

不要为 Computer Reset 创建独立的 ID 系统。

不要绕过 Capability Registry。

Action 名称使用：

```text
computer_reset
```

具体注册 API 和 ID 分配方式按照当前项目已有 Action 注册模式实现。

---

## 12. Temporary Action

云端收到：

```text
computer_reset
```

后，应按照当前 CommandManager 已有的 Action 执行流程进入 WorkflowManager 的 Temporary Action 机制。

不要为 `computer_reset` 新增：

```text
CommandManager → computer_reset()
```

这种独立执行路径。

也不要修改现有 CommandManager 的整体架构。

---

## 13. 非阻塞要求

Computer Reset Action 必须完全非阻塞。

执行过程中：

```text
GPIO8 = HIGH
```

但 ESP32 仍然必须能够正常执行：

* MQTT
* WiFi
* WorkflowManager
* EventManager
* OLED
* 其他后台任务

不得使用 800 ms 的 `delay()`。

---

## 14. 并发 / 重复执行

应遵循现有 WorkflowManager / Temporary Action 对 Action 并发和重复执行的既有规则。

不要在 Computer Reset 模块内部重新设计一套队列、线程或任务系统。

如果现有 Temporary Action 机制已经负责 Action 生命周期，则 Computer Reset 只负责正确实现 Action 本身。

如果现有框架规定 Action Start 时必须先检查 Busy 状态，则遵循现有框架，不重复实现。

---

## 15. GPIO 安全边界

GPIO8 的控制权只属于 Computer Reset 模块。

初始化：

```text
LOW
```

Action 开始：

```text
HIGH
```

800 ms 后：

```text
LOW
```

Action Reset / Cancel / Error：

```text
LOW
```

最终必须满足：

```text
正常空闲状态 = LOW
```

---

## 16. 不允许的修改

本任务是新增 Computer Reset 功能。

除非为了接入现有 Action 注册机制确实需要，否则不要修改以下模块的核心逻辑：

```text
CloudManager
CommandManager
WorkflowManager
SystemCommand
ConfigManager
EventManager
SystemState
```

尤其不要修改：

* CommandManager 的 Command 架构
* Temporary Action 的执行机制
* WorkflowManager 的状态机
* System Command Restart 机制
* Critical Operation 机制

应该最大限度复用现有接口。

---

## 17. 代码质量要求

要求：

1. GPIO8 使用统一常量定义。
2. 不使用 `delay()`。
3. HIGH 持续时间固定为 800 ms。
4. 所有退出路径确保 GPIO8 为 LOW。
5. 初始化时明确设置 GPIO8 为 LOW。
6. 不新增独立 Command 执行体系。
7. 不使用 Critical Operation。
8. 不修改现有 Restart 机制。
9. 遵循现有 Action / Temporary Action / Capability Registry 的代码风格。
10. 不改变已有公开 API，除非现有 Action 注册机制本身要求增加注册代码。

---

## 18. 最终设计原则

Computer Reset 是一个标准 Hardware Action，而不是一个特殊 Command。

因此最终结构必须保持：

```text
CloudManager
    ↓
CommandManager
    ↓
WorkflowManager
    ↓
Temporary Action
    ↓
ComputerReset Action
    ↓
GPIO8
```

ComputerReset 模块只负责：

```text
GPIO8 初始化为 LOW
        ↓
Action Start
        ↓
GPIO8 HIGH
        ↓
非阻塞等待 800 ms
        ↓
GPIO8 LOW
        ↓
Action Complete
```

不要因为这个功能简单而绕过现有 Workflow / Action 架构。
