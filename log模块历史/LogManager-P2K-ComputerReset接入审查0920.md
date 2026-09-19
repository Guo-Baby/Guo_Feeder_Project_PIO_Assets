# LogManager P2-K —— ComputerReset 模块埋点接入审查

> **状态**：✅ 完成（代码 + 编译 + 上板验证）
> **日期**：2026-09-20
> **范围**：`src/computer_reset.cpp` 一个文件，3 个 EventId / 4 处 `log_emit`
> **前置**：P2 八模块、P2-I（突发合并）、P2-J（Command）
> **路线依据**：Phase 3「接入顺序：① Command ② OLED ③ ComputerReset ④ Registry ⑤ Event」
> **权威依据**：`docs/P2_Log_Integration_Matrix.md:68-70`（宿主 / 参数白名单）

---

## 一、接入清单

| # | EventId | 语义 | 级别 | 位置 | 节流策略 |
|---|---------|------|------|------|----------|
| ① | `LOG_CRESET_PULSE` `0x0D01` | 电脑重启脉冲产生 | INFO | **`computer_reset_set_output()`** 的 `LOW → HIGH` 上升沿 | **无（天然边沿）** |
| ② | `LOG_CRESET_SAFETY_TIMEOUT` `0x0D02` | GPIO 卡在 HIGH 超时兜底 | WARN | `computer_reset_task()` 的 `>= HOLD+MARGIN` 分支 | **无（天然边沿）** |
| ③ | `LOG_CRESET_POOL_EXHAUSTED` `0x0D03` | 实例上下文池耗尽 | ERROR | `computer_reset_action_start()` + `computer_reset_trigger()`（**2 处**） | **无（天然边沿）** |

**参数复用**（未新增任何 ParamId）：

| ParamId | 值 | 用于 |
|---------|-----|------|
| `LOG_P_HOLD_MS` | `0x43` | ① 设计保持时长 / ② 设计上限 |
| `LOG_P_DURATION_MS` | `0x06` | ② 实际卡住时长 |
| `LOG_P_ACTIVE` | `0x41` | ③ 当前活跃脉冲数 |
| `LOG_P_CAPACITY` | `0x42` | ③ 池容量（`COMPUTER_RESET_MAX_INSTANCE` = 4） |

`log_events.h` **零改动**。

---

## 二、关键决策：① 为何挂 `set_output()` 而非 `trigger()`

**矩阵原文**（`P2_Log_Integration_Matrix.md:68`）：

| `computer_reset_trigger()` :441 | 电脑重启脉冲 | `LOG_CRESET_PULSE` | INFO | `LOG_P_HOLD_MS` |
|---|---|---|---|---|

**代码事实**：脉冲有**两条产生路径**，二者**都**最终经 `computer_reset_set_output(true)` 拉高 GPIO：

| 路径 | 入口 | 调用链 |
|------|------|--------|
| ① 手动 | `main.cpp:635` 控制台 `computer_reset` | `computer_reset_trigger()` :459 → `set_output(true)` |
| ② Workflow Action | 云端 `execute_action` ob=`COMPUTER_RESET` | `computer_reset_action_start()` :271 → `set_output(true)` |

**若严格照矩阵只挂 `trigger()`，则路径 ② 产生的脉冲完全没有日志（漏记）。**

⇒ **决策：挂在 `set_output()` 的上升沿**，一次覆盖两条路径。这也符合矩阵的**真实意图**（"电脑重启脉冲"这一语义，而非"某函数被调用"）。

**免门控论证（天然边沿）**：`set_output()` 开头即有

```c
// 相同电平不重复写
if(active == output_active) return;
```

⇒ 能走到 `log_emit` 处**必然是一次真实的 LOW→HIGH 跳变**，结构上不可能被连续进入。

---

## 三、② 的取值时机（LOG-13 铁律落地）

`computer_reset_task()` 安全超时块：

```c
if((unsigned long)(now - pulse_start_ms) >= (HOLD_MS + MARGIN_MS))
{
    // ★ 埋点必须在 force_idle() 之前
    {
        LogParamIn p[2];
        p[0] = log_arg_u32(LOG_P_HOLD_MS,     (uint32_t)COMPUTER_RESET_HOLD_MS);
        p[1] = log_arg_u32(LOG_P_DURATION_MS, (uint32_t)(now - pulse_start_ms));
        log_emit(LOG_CRESET_SAFETY_TIMEOUT, LOG_LVL_WARN, p, 2);
    }

    Serial.printf("[ComputerReset] Safety timeout! ...");
    computer_reset_force_idle();     // ← 这里会把 pulse_start_ms 清零
}
```

**为什么顺序关键**：紧随其后的 `computer_reset_force_idle()` 会调 `set_output(false)`，而 `set_output(false)` 内部执行 `pulse_start_ms = 0`。

⇒ 若把埋点写在 `force_idle()` **之后**，`DURATION_MS` 会算成 `now - 0` = **错误的天文数字**。

这正是 **LOG-13 铁律**「**取值用于日志的变量须在源头清零前取**」（`OPEN_MS` 已踩过 3 次）。本处刻意把埋点放在 `force_idle()` **之前**，并在注释中写明原因。

**免门控论证（天然边沿）**：`force_idle()` 置 `output_active = false`，而本函数上方有 `if(!output_active || pulse_start_ms == 0) return;` ⇒ **不可能连续两次进入本块**。

---

## 四、③ 与矩阵措辞的偏差（按代码事实修正）

**矩阵原文**（`:70`）：

| `computer_reset_ctx_release()` / 池耗尽 | 上下文池耗尽 | `LOG_CRESET_POOL_EXHAUSTED` | ERROR | `LOG_P_ACTIVE`, `LOG_P_CAPACITY` |
|---|---|---|---|---|

**代码事实**：`computer_reset_ctx_release()` **只做释放，不做分配** ⇒ 它**不可能**导致"池耗尽"。真正的两个宿主是 `computer_reset_ctx_alloc()` 返回 `nullptr` 的两处：

| 位置 | 函数 | 上下文 |
|------|------|--------|
| `:288-298` | `computer_reset_action_start()` | Workflow Action 路径 |
| `:510-520` | `computer_reset_trigger()` | 手动路径 |

⇒ **按代码事实实现**（矩阵的 `LOG_P_ACTIVE`/`LOG_P_CAPACITY` 参数指定照采纳），并把偏差记录在案。

---

## 五、验证记录

### V1 编译

| 项 | 值 |
|----|-----|
| 结果 | **SUCCESS** |
| RAM | 130616 B（39.9%）—— **零增长** |
| Flash | 1370281 B（65.3%）—— 较 P2-J 的 1370125 **+156 B** |

### V2 零回归（静置）

`.pio/p15run/p2k_idle.log`：

- `emit` **5 → 5**（**零增长**）⇒ 3 个埋点**无虚假触发** ✅
- 唯一的 `[ComputerReset]` 行是启动日志 `Init OK (pin=8, hold=800 ms)`（**非** `log_emit` 记录）
- loop 存活正常

### V3 `LOG_CRESET_PULSE` 实测 —— **手动路径**（`p2k_pulse.log`）

```
[ComputerReset] GPIO8 = HIGH
[ComputerReset] Manual pulse started (active=1)
[ComputerReset] GPIO8 = LOW          ← 800ms 后正常回落
ComputerReset active state: 0        ← 回到空闲
```

| 指标 | 前 | 后 | 增量 |
|------|-----|-----|------|
| `emit` | 3 | 4 | **+1** |
| `flash` | 0 | 0 | **0**（INFO ⇒ 只上云不落 Flash ✅ 符合规格） |

### V3b `LOG_CRESET_PULSE` 实测 —— **Workflow Action 路径**（`p2k_action.log`）★ **决定性**

```
[CMD][RESULT] {"status":"accepted","message":"Action queued","action":"COMPUTER_RESET",...}
[ComputerReset] GPIO8 = HIGH
[ComputerReset] Pulse started (hold=800 ms, active=1)   ← 注意：非 "Manual pulse started"
[ComputerReset] GPIO8 = LOW
[ComputerReset] Pulse finished (elapsed=800 ms, active=0)
```

| 指标 | 前 | 后 | 增量 |
|------|-----|-----|------|
| `emit` | 3 | 4 | **+1** ✅ |

⇒ **本项直接证明 §二 决策的正确性**：Action 路径的脉冲**也被记录**。若按矩阵字面只挂 `trigger()`，这条记录**将不存在**。

### V4 二进制级确认（补偿 ②③ 不可实测）

`objdump -dr` 反汇编 `computer_reset.cpp.o`，4 处 `log_emit` 调用**逐一映射**到所属函数：

| `log_emit` 偏移 | 所属函数 | 对应埋点 |
|---|---|---|
| `0x46` | `computer_reset_set_output(bool)` | ① PULSE |
| `0x5a` | `computer_reset_action_start(...)` | ③ POOL_EXHAUSTED |
| `0x73` | `computer_reset_task()` | ② SAFETY_TIMEOUT |
| `0x48` | `computer_reset_trigger()` | ③ POOL_EXHAUSTED |

⇒ **4 处 = 源码 4 处**，且**全部落在预期的函数内**，无死代码、无错位。

---

## 六、未覆盖项（如实登记）

| 埋点 | 未验证原因 | 结论 |
|------|-----------|------|
| ② `SAFETY_TIMEOUT` | **正常路径结构性不可达**：手动路径在 800 ms 就由 `task()` 步骤 1 回收（`active_pulse_count` 归零 → `set_output(false)` → `pulse_start_ms=0`）；Action 路径在 800 ms 由 `poll()` 释放。二者**都到不了** `HOLD+MARGIN = 2000 ms`。要触发需"实例泄漏 / `poll()` 不再被调用"这类**异常条件**，无法用现有控制台构造；而新增测试钩子**违反"禁止添加测试模式开关"** | **二进制已验证**（V4）；属**安全兜底路径**，正常时不应触发 —— **不触发即为正确行为** |
| ③ `POOL_EXHAUSTED` | 需 **4 个实例同时活跃**（`COMPUTER_RESET_MAX_INSTANCE = 4`），而每个脉冲 800 ms 即结束 ⇒ 自然条件下极难占满 | **二进制已验证**（V4） |

> **★ 与 Command 段（P2-J）的对比**：P2-J 的未覆盖项是**工具限制**（探针挂不住 60 s）；
> 本项的未覆盖是**语义上不该正常触发**（安全兜底 / 资源耗尽）⇒ **性质更良性**。
> 三处埋点中**唯一有正常触发路径的 ① 已双路径实测通过**。

---

## 七、本轮沉淀的教训

1. **★ 矩阵给的是"语义宿主"，落地要按"代码事实"找"物理宿点"。**
   矩阵写 `trigger()`，但脉冲有两条产生路径 ⇒ 挂 `trigger()` 会漏掉 Action 路径。
   **正解是上移到两条路径的公共汇合点**（`set_output()` 的上升沿）。
   V3b 实测证明：这个上移**把一条本会静默的路径纳入了观测**。

2. **★ 埋点顺序在"取值依赖会被清零的变量"时是正确性问题，不是风格问题。**
   `DURATION_MS` 依赖 `pulse_start_ms`，而紧随的 `force_idle()` 会把它清零 ⇒
   埋点**必须**写在前面。这是 LOG-13 的又一次具体化（`OPEN_MS` 已踩 3 次）。

3. **"天然边沿"的论证可以直接引用**既有的**防御性代码**。
   `set_output()` 的 `if(active == output_active) return;` 本来是为"避免重复写 GPIO"，
   但它**同时**构成了天然边沿的**结构性证明**。 ⇒ 现有防御性守卫常可复用为门控依据，
   无需新增任何去重逻辑。

4. **"正常时不应触发"的埋点，其"不触发"本身就是验收项。**
   ② 是安全兜底：**能实测触发反而说明有异常**。⇒ 这类埋点的验收标准是
   **"二进制存在 + 正常路径零增长"**，而非"实测能触发"。

---

## 八、改动文件

| 文件 | 改动 |
|------|------|
| `src/computer_reset.cpp` | +include `log_manager.h`；+4 处 `log_emit`；+注释（双路径论证 / 免门控论证 / 取值时机 / 矩阵偏差） |

**未改动**：`log_events.h`（零改动）、ParamId、日志协议、System State、EventManager 风暴策略、任何测试断言、GPIO 时序、池容量、所有业务逻辑。
