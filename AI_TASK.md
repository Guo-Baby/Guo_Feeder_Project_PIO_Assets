# System Command — Safe Restart 需求文档

## 1. 本次开发范围

本次仅修改：

* `System Command` 模块
* `system_command.cpp`
* `system_command.h`
* 以及该模块内部为实现 Safe Restart 所必需的代码

**禁止修改其他业务模块。**

本次核心目标：

1. 将 Restart 能力从 Config Manager 收回。
2. 在 System Command 中建立统一的 Safe Restart 机制。
3. Restart 不区分来源：

   * 用户下发 Restart
   * Config Manager 自动请求 Restart
   * 未来其他模块请求 Restart
4. 建立全局 `Critical Operation` 计数机制。
5. 输出后续其他模块接入 `Critical Operation` 的规范，但本阶段不要修改其他模块。

---

# 2. Safe Restart 核心模型

系统只允许存在一个统一的 Restart 执行机制。

Restart 的最终执行条件：

```text
Restart 已被请求
        ↓
Critical Operation Count == 0
        ↓
进入 Restart Pending
        ↓
10 秒安全倒计时
        ↓
执行 ESP Restart
```

Restart 一旦被请求，**不可被普通操作取消**。

普通操作不能撤销 Restart Request。

---

# 3. Critical Operation Counter

System Command 内部维护一个全局的 Critical Operation 计数器：

```cpp
uint32_t critical_operation_count;
```

语义：

```text
0
    → 当前没有 Critical Operation
    → 允许进入 Restart

> 0
    → 当前存在 Critical Operation
    → 禁止开始 Restart
```

## 3.1 开始 Critical Operation

其他模块开始执行不可安全中断的操作时：

```cpp
system_command_critical_operation_acquire();
```

内部：

```text
critical_operation_count++
```

## 3.2 Critical Operation 完成

操作安全完成后：

```cpp
system_command_critical_operation_release();
```

内部：

```text
critical_operation_count--
```

必须保证：

```text
acquire() 与 release() 成对出现
```

禁止出现：

```text
acquire()
...
忘记 release()
```

否则系统可能永久无法 Restart。

---

# 4. Critical Operation 的定义

Critical Operation 指：

> 如果此操作在执行过程中被 ESP32 强制重启，可能导致系统状态、持久化数据、外部设备状态或硬件状态处于不可预期状态。

当前确认属于 Critical Operation 的操作：

### Config

Config Manager 修改配置并进入延迟保存阶段时：

```text
Critical Operation Count +1
```

直到：

```text
Config Save 完成
```

才：

```text
Critical Operation Count -1
```

注意：

**5 分钟 Config 延迟计时属于 Config Manager 自己的业务逻辑，不属于 Restart 模块。**

System Command 不知道也不关心这个 5 分钟倒计时。

---

### Workflow JSON

Workflow 执行过程中涉及必须完整完成的 JSON 文件写操作时，由 Workflow/相关业务模块负责声明 Critical Operation。

JSON Storage 本身不是 Critical Operation。

也就是说：

```text
JSON Storage
    ↓
只是底层工具
    ↓
不自行 acquire/release
```

真正决定是否 Critical 的是调用它的业务模块。

---

### WOF

WOF 的开/关动作属于 Critical Operation。

例如：

```text
WOF 开启
    acquire
    ↓
执行硬件动作
    ↓
操作完成
    ↓
release
```

开发状态下，如果 WOF 正在执行不可中断的物理动作，则 Restart 必须等待。

---

### RTC 写操作

未来 Time Manager 对 RTC 芯片执行写操作时：

```text
RTC Write
    acquire
    ↓
RTC 写入
    ↓
release
```

RTC Read 不属于 Critical Operation。

---

# 5. 非 Critical Operation

以下操作默认不需要 Critical Operation：

* 普通 RAM 读取
* Flash 容量查询
* Flash 使用量查询
* LittleFS 文件列表
* LittleFS 文件读取
* System State 查询
* Wi-Fi 状态查询
* MQTT 状态查询
* 时间读取
* JSON Storage 普通读写接口本身
* 普通 Command 查询
* 普通 MQTT 操作

原则：

**只有业务层明确认为“被 Restart 中断会产生危险状态”的操作，才声明 Critical Operation。**

---

# 6. Restart 状态机

System Command 内部实现以下状态：

```cpp
enum RestartState
{
    RESTART_IDLE,
    RESTART_REQUESTED,
    RESTART_PENDING,
    RESTARTING
};
```

## RESTART_IDLE

系统正常运行。

此状态下：

* 可以接受 Critical Operation
* 可以接受 Restart Request

如果没有 Restart Request：

```text
Critical Operation Count
可以自由变化
```

---

## RESTART_REQUESTED

收到 Restart Request。

此状态下：

* 不执行立即 Restart
* 继续等待 Critical Operation 完成
* 允许新的 Critical Operation
* 持续检查：

```text
critical_operation_count == 0
```

只有 Count 归零：

```text
RESTART_REQUESTED
        ↓
RESTART_PENDING
```

注意：

**Restart Request 不能因为 Critical Operation 出现而取消。**

如果新的 Critical Operation 在等待期间开始：

```text
Count > 0
```

则继续等待。

---

## RESTART_PENDING

进入最终 Restart 安全窗口。

进入条件：

```text
Restart Requested
AND
Critical Operation Count == 0
```

此时：

```text
启动 10 秒倒计时
```

从这一刻开始：

**禁止新的 Critical Operation 开始。**

也就是说：

```text
RESTART_PENDING
    ↓
任何新的 Critical Operation 请求
    ↓
必须被拒绝 / 不允许开始
```

原因：

系统已经确认当前没有 Critical Operation，并准备执行 Restart。

---

## RESTARTING

10 秒倒计时结束：

```cpp
ESP.restart();
```

进入：

```text
RESTARTING
```

之后不再执行其他业务逻辑。

---

# 7. 为什么 RESTART_REQUESTED 允许 Critical Operation

这是设计中的重要规则。

例如：

```text
Config Set A
    ↓
Critical Count = 1

Config Set B
    ↓
Critical Count = 2

Config Set C
    ↓
Critical Count = 3
```

此时即使用户已经发送：

```text
Restart
```

也不能阻止 Config Manager 继续修改参数。

Restart 只需要等待：

```text
3 → 2 → 1 → 0
```

当：

```text
Critical Count == 0
```

才进入：

```text
RESTART_PENDING
```

因此用户可以连续修改多个 Config 参数。

---

# 8. 5 分钟 Config Save 与 Restart 的关系

Restart 模块**不负责** Config 的 5 分钟计时。

Config Manager 自己负责：

```text
Config Set
    ↓
修改 RAM
    ↓
Critical Operation Count++
    ↓
5 分钟计时
    ↓
Config Save
    ↓
Critical Operation Count--
```

如果此时用户发送 Restart：

```text
Restart Request
    ↓
RESTART_REQUESTED
    ↓
等待 Config Manager
    ↓
Config Save
    ↓
Critical Count = 0
    ↓
RESTART_PENDING
    ↓
10 秒
    ↓
ESP.restart()
```

因此 Config Manager 不再拥有自己的 Restart 实现。

---

# 9. Restart Request API

System Command 应提供统一的 Restart Request 接口，例如：

```cpp
void system_command_request_restart();
```

具体命名可根据当前项目代码风格调整，但必须满足：

* 所有 Restart 来源统一调用该接口
* 不允许其他模块直接调用 `ESP.restart()`
* 不允许其他模块实现自己的 Restart Timer
* 不允许 Config Manager 自己 Restart

---

# 10. Critical Operation API

System Command 对外提供两个基础接口：

```cpp
bool system_command_critical_operation_acquire();
bool system_command_critical_operation_release();
```

建议：

### acquire

返回：

```text
true
    → Critical Operation 成功获得

false
    → 当前已经进入 RESTART_PENDING / 不允许开始
```

### release

只有此前成功 acquire 的操作才能 release。

必须防止：

```text
Count == 0
release()
```

造成无符号整数下溢。

因此 release 必须进行保护。

---

# 11. Critical Operation 的并发安全

Critical Operation Counter 是全局共享资源。

必须考虑：

* MQTT Command
* Workflow
* Config Manager
* Timer
* 其他业务模块

可能在不同执行路径中调用。

因此：

**Counter 的读写必须保证原子性/并发安全。**

不要让业务模块直接访问：

```cpp
critical_operation_count
```

业务模块只能通过 System Command 提供的 API：

```cpp
acquire()
release()
```

访问。

---

# 12. Critical Operation 风暴

系统不需要在 Restart 模块内部解决 Critical Operation 无限增加的问题。

理论上：

```text
Restart Request
    ↓
Critical Count 一直 > 0
    ↓
Restart 永远等待
```

这是允许存在的。

Restart 模块只负责正确执行：

```text
Request
    ↓
等待 Count == 0
    ↓
10 秒安全窗口
    ↓
Restart
```

Critical Operation 的正确 acquire/release 由各业务模块负责。

---

# 13. Restart 不可取消

一旦：

```cpp
system_command_request_restart();
```

成功接受：

禁止提供：

```cpp
cancel_restart()
```

之类的普通取消接口。

Restart 可以被延迟：

```text
Critical Count > 0
```

但不能被取消。

---

# 14. Restart 安全窗口

进入：

```text
RESTART_PENDING
```

以后：

```text
critical_operation_count == 0
```

并启动：

```text
10 秒倒计时
```

此时必须禁止新的 Critical Operation。

如果某个模块尝试：

```cpp
system_command_critical_operation_acquire();
```

必须失败。

该模块不能开始自己的 Critical Operation。

---

# 15. ESP Reset Reason

本阶段不要求 System Command 自己实现完整 Log 模块。

Restart 后如果需要判断：

```text
为什么发生 Reset
```

优先使用 ESP32/ESP-IDF 提供的 Reset Reason 机制。

System Command 可以预留接口，但：

**不要在本阶段为了 Reset Reason 引入新的 Log 系统。**

未来 Log Module 建立后，再决定是否将 Restart Request / Restart 执行过程纳入 Log。

---

# 16. Shutdown Handler

本阶段不需要单独实现 Shutdown Handler。

原因：

ESP32 的 Restart 并不是传统意义上的操作系统 Shutdown。

当前目标只是：

```text
安全等待 Critical Operation
        ↓
10 秒安全窗口
        ↓
ESP.restart()
```

不增加额外 Shutdown 抽象层。

如果未来出现真正需要统一执行的：

```text
停止电机
关闭 WOF
Flush Log
保存状态
关闭外设
```

再单独评估是否需要 Shutdown/Pre-reset Handler。

---

# 17. 本阶段禁止事项

AI 实现本需求时：

### 禁止

```text
❌ 修改 Config Manager 的业务逻辑
❌ 保留 Config Manager 自己的 Restart 实现
❌ 让 System Command 调用 Config Manager Restart
❌ 其他模块直接调用 ESP.restart()
❌ System Command 实现 Config 的 5 分钟计时
❌ System Command 判断“Restart 是谁发起的”
❌ 区分 User Restart / Config Restart / Automatic Restart
❌ 引入完整 Log Module
❌ 修改 Workflow / WOF / Time Manager / JSON Storage
```

本阶段只建立：

```text
System Command
    │
    ├── Restart State Machine
    ├── Critical Operation Counter
    ├── Restart Request API
    └── Critical Operation Acquire/Release API
```

---

# 18. 后续其他模块接入规范

System Command 完成并测试通过后，其他模块按照以下模式接入。

### 开始 Critical Operation

```cpp
if (!system_command_critical_operation_acquire())
{
    // Restart 已进入 RESTART_PENDING
    // 当前操作不得开始
    return;
}
```

### 执行 Critical Operation

```cpp
// critical operation
```

### 完成

```cpp
system_command_critical_operation_release();
```

完整生命周期：

```text
Acquire
   ↓
Critical Operation
   ↓
Release
```

必须保证任何成功的：

```cpp
acquire()
```

最终一定对应：

```cpp
release()
```

即使执行过程中出现异常/错误路径，也必须保证 release。

---

# 19. 最终架构

最终系统形成：

```text
                 ┌──────────────────────┐
                 │    System Command    │
                 │                      │
                 │ Restart State       │
                 │ Critical Counter    │
                 │ Restart Request     │
                 │ Acquire / Release   │
                 └──────────┬───────────┘
                            │
             ┌──────────────┼──────────────┐
             │              │              │
             ▼              ▼              ▼
       Config Manager     Workflow        WOF
             │              │              │
       acquire/release acquire/release acquire/release
             │              │              │
             └──────────────┼──────────────┘
                            │
                            ▼
                  Critical Count == 0
                            │
                            ▼
                   Restart Pending
                            │
                          10 sec
                            │
                            ▼
                       ESP.restart()
```

核心原则只有一句话：

> **业务模块负责声明“什么时候不能重启”，System Command 只负责判断“什么时候可以重启”，并执行唯一的 Restart。**
