# Critical Operation 接入规范

> 适用范围：所有需要「不可被重启中断」操作的业务模块（Config / Workflow / WOF / RTC 等）。
> 本规范依据 `AI_TASK.md`（System Command — Safe Restart 需求文档）§10 / §11 / §18 编写。
> 代码基线：`src/services/system_command.h` / `src/services/system_command.cpp`（V2）。

---

## 1. 一句话原则

> **业务模块负责声明「什么时候不能重启」，System Command 只负责判断「什么时候可以重启」，并执行唯一的 Restart。**

业务模块在开始一个不可安全中断的操作前调用 `acquire()`，操作完成（无论成败）后必须调用 `release()`。
System Command 收到 Restart 请求后，会一直等待所有 Critical Operation 归零，再进入 10 秒安全窗口，最后才执行 `ESP.restart()`。

---

## 2. 对外 API（全部定义在 `src/services/system_command.h`）

| 接口 | 签名 | 说明 |
|---|---|---|
| 请求重启 | `bool system_command_request_restart()` | 全系统唯一 Restart 入口，**幂等**、**不可取消**。返回 `true`=已接受 |
| 开始 Critical Op | `bool system_command_critical_operation_acquire()` | 返回 `true`=获得许可；`false`=系统已进入 PENDING，**不得开始操作** |
| 结束 Critical Op | `bool system_command_critical_operation_release()` | 与 acquire 配对。计数为 0 时调用返回 `false`（下溢保护），并打错误日志 |
| 查询计数 | `uint32_t system_command_critical_operation_count()` | 只读诊断 |
| 查询状态 | `RestartState system_command_restart_state()` | `RESTART_IDLE / RESTART_REQUESTED / RESTART_PENDING / RESTARTING` |
| 查询剩余时间 | `bool system_command_restart_pending(unsigned long &remain_ms)` | PENDING 时返回剩余 ms |
| 状态名 | `const char *system_command_restart_state_name(RestartState)` | 日志 / JSON 用 |
| Reset Reason | `uint8_t system_command_reset_reason()` / `const char *system_command_reset_reason_name()` | 本次启动的复位原因 |

头文件：业务模块 `#include "system_command.h"` 即可，**无需**包含 `system_command.cpp` 的实现细节。

---

## 3. 标准接入模式（必须照抄）

```cpp
#include "system_command.h"

// 1) 开始不可中断操作前：
if (!system_command_critical_operation_acquire())
{
    // 系统已进入 RESTART_PENDING（10 秒倒计时中），
    // 本次操作【不得】开始。这里直接放弃并返回。
    return;   // 或 return false / continue，视调用方而定
}

// 2) 执行 Critical Operation（原业务逻辑）
bool ok = do_something_unsafe_to_interrupt();

// 3) 无论成败，必须 release() —— 包括异常/错误分支
system_command_critical_operation_release();
```

### 3.1 错误路径也必须 release

```cpp
if (!system_command_critical_operation_acquire())
{
    return;
}

// ---- 业务 ----
JsonStorageWriteResult r = json_storage_write_atomic(...);
if (r != OK)
{
    // 出错也要先 release 再 return
    system_command_critical_operation_release();
    return FAILED;
}

system_command_critical_operation_release();
return OK;
```

> 铁律：**一次成功的 acquire() 最终一定对应一次 release()。**
> 忘记 release 的后果：计数永远 > 0，系统将**永久无法重启**（这是安全的失败方向，但会让定时重启等机制失效，属于严重 bug）。

---

## 4. 并发安全说明（为什么不用普通变量）

- `cloud_manager` 使用 esp-mqtt，云端命令在 `mqtt_event_handler()` 中**同步**执行，
  即 acquire/release/request_restart 可能运行在 **esp-mqtt 任务**上下文；
- `system_command_task()` 由 **Arduino loop 任务**每轮驱动；
- 两者可能在不同核心上并发执行，因此计数器与状态机内部全部由 **SMP 自旋锁**
  （`portMUX_TYPE` + `portENTER_CRITICAL/portEXIT_CRITICAL`）保护。

**业务模块不要直接访问 `critical_operation_count`**（它没有对外暴露），只能通过 API 访问。

---

## 5. 各模块接入点（本阶段【未】修改这些模块，未来接入时按此执行）

| 模块 | 判定标准 | 接入位置 |
|---|---|---|
| **ConfigManager** | 配置修改后处于「延迟保存」期间（RAM 已改、尚未落盘） | `config_set_*` 成功修改后 acquire → `config_save()` 完成后 release。**5 分钟倒计时本身不属于 Critical Operation**，System Command 不关心 |
| **Workflow** | 执行过程中涉及「必须完整完成的 JSON 文件写操作」时 | 由 Workflow 业务层在写文件前 acquire、写完后 release。**JsonStorage 本身不是 Critical Operation**，只是底层工具，不自行 acquire/release |
| **WOF** | 开 / 关的物理动作执行期间 | 动作开始前 acquire → 动作完成后 release |
| **TimeManager（已接入）** | RTC 芯片写操作（SNTP 成功后校准、手动设置时间写 RTC） | `time_rtc_calibrate()` / `time_set_manual()`：写入前 acquire → 写入完成后 release，成功与失败路径都 release；acquire 被拒直接 return（不 release）。**RTC 读不属于 Critical Operation** |

### 判断是否 Critical 的通用标准

> 如果此操作在执行过程中被强制重启，可能导致系统状态、持久化数据、外部设备状态或硬件状态处于不可预期状态 → 就是 Critical Operation。

反例（默认**不**需要）：普通 RAM 读取、Flash 容量查询、LittleFS 文件列表、文件读取、System State 查询、WiFi/MQTT 状态查询、时间读取、普通命令查询、普通 MQTT 操作。

---

## 6. Restart 请求规范

所有来源统一调用：

```cpp
system_command_request_restart();
```

- ✅ 用户下发 `system.restart` / `system.reboot` 命令 → CommandManager → 本接口
- ✅ ConfigManager 5 分钟倒计时到期 → 本接口
- ✅ 未来定时重启 / 异常自恢复 → 本接口
- ❌ **禁止**任何其他模块直接调用 `ESP.restart()`
- ❌ **禁止**任何其他模块实现自己的重启定时器
- ❌ **禁止**提供 cancel 接口：Restart 一旦请求**不可取消**，只能被 Critical Operation 延迟

---

## 7. 测试方法（接入完成后验证）

### 7.1 观察状态机

```json
{"c":"system","i":"9001","p":{"o":"restart_status"}}
```

预期返回 `data`：

```json
{
  "state": "idle",
  "critical_operations": 0,
  "restart_pending": false,
  "remain_ms": 0,
  "safe_delay_ms": 10000
}
```

### 7.2 验证「PENDING 后 acquire 被拒绝」

1. 下发 `system.restart` → 等状态变为 `pending`（`restart_status` 观察，约在 critical 归零后进入）；
2. 在 PENDING 窗口内由业务模块调用 `acquire()` → 必须返回 `false`，串口打印
   `[SYSCMD][W] critical op acquire rejected (state=pending)`；
3. 10 秒后设备重启。

### 7.3 验证「等待归零」

1. 业务模块 acquire（critical=1）→ 下发 `system.restart` → `restart_status` 显示
   `state=requested, critical_operations=1`，设备**不**重启；
2. 业务模块 release（critical=0）→ 状态机进入 `pending`，10 秒后重启。
3. 重启后 `restart_status` / 串口日志应显示 `reset_reason=software`。

### 7.4 串口日志关键字

```
[SYSCMD][I] init done, reset reason=..., safe delay=10000 ms
[SYSCMD][W] restart requested (critical=N), waiting for drain
[SYSCMD][W] critical operations drained, restart pending 10000 ms
[SYSCMD][W] safe window elapsed, restarting now
[SYSCMD][E] critical op release underflow: ...   ← 出现即表示配对 bug
```

---

## 8. 附录：Restart 状态机速览

```
RESTART_IDLE ──request_restart()──▶ RESTART_REQUESTED
RESTART_REQUESTED ──critical==0──▶ RESTART_PENDING（启动 10s 倒计时）
RESTART_PENDING ──10s 到点──▶ RESTARTING → ESP.restart()
```

- `RESTART_REQUESTED`：**允许**新的 Critical Operation 开始（用户可能连续改参数，重启只需等待归零）
- `RESTART_PENDING`：**禁止**新的 Critical Operation 开始（acquire 返回 false）
- Restart 不可取消、不可区分来源（用户 / Config / 自动统一对待）
