# test_stepper 测试模块 —— 烧录与测试说明

> **日期**：2026-10-22
> **镜像**：`flash-target-270534be.bin`（sha256 `270534bebdfe912666141375533319f14ecbdf1221a17fe8c65624767ad492ef`）
> **构建**：`PLATFORMIO_BUILD_DIR=.pio/build/tstep_on PLATFORMIO_BUILD_FLAGS=-DGF_TELEMETRY_ENABLED=1`
> **资源**：Flash 68.9%（1,443,905 / 2,097,152）· RAM 41.5%（135,864 / 327,680）

---

## 一、为什么要有这个模块

正式 `stepper` 模块有三条约束，妨碍排障期的逐点测量：

| 约束 | 对测量的影响 |
|---|---|
| 单次动作 30 s 上限 | 到点自动停，来不及逐个测点 |
| 动作前 50 ms 使能等待 + 动作后按配置禁用 | 想"常通电测 EN/DIR/STEP 电平时序"做不到 |
| 脉冲数有限（533 或指定值） | 无法持续输出方波供万用表观察 |

`test_stepper` 专为排障设计：

1. **无 30 秒限制** —— 启动后持续输出方波，直到收到 STOP。唯一守护是 **10 分钟**硬上限（`TEST_STEPPER_MAX_RUN_MS`），防忘停烧电机。
2. **频率可调** —— `hz` 参数 10~2000，默认 100。用于验证「频率过高导致失步」。
3. **脉冲源换成 LEDC**（不是 RMT）—— 这是刻意设计：
   - 与 `stepper` 的 `RMT_CHANNEL_0` 不冲突，LEDC 是独立外设
   - **若 LEDC 能转、RMT 不能转 ⇒ 问题定位到 RMT 输出侧**
   - **若两者都不能转 ⇒ 问题在驱动器/电机/电源侧**
4. **复用 stepper 的引脚配置**，不新增配置文件（避免 LittleFS 缺文件导致 `config_set` 失败）。

---

## 二、新增与修改的文件

| 文件 | 变更 | 说明 |
|---|---|---|
| `src/app/test_stepper.h` | **新增** | 接口声明 + 常量（`TEST_STEPPER_MAX_RUN_MS` / `HZ_MIN/MAX/DEFAULT`） |
| `src/app/test_stepper.cpp` | **新增** | LEDC 方波输出、启停、Action 注册、硬上限守护 |
| `src/main.cpp` | 修改 | include + `test_stepper_init()`（第四层）+ `test_stepper_task()`（loop）+ 3 条串口命令 |

**未修改**：`stepper.cpp/h`、`config_manager.*`、`system_state.*`、`event_manager.h`。
⇒ 不新增 State / Event / Config 模块，**不动生产代码路径**。

---

## 三、Action 定义

| Action ID | 参数 | 行为 |
|---|---|---|
| `TEST_STEPPER_START` | `hz` (int, 可选, 默认 100, 范围 10~2000)<br>`direction` (int, 可选, 默认 0) | 使能驱动器 → DIR 就位 → LEDC 输出 50% 方波。**立即返回成功**，输出保持 |
| `TEST_STEPPER_STOP` | 无 | 停 LEDC → 释放引脚 → 禁用驱动器。**幂等**（未运行时也返回成功） |

**校验规则**：
- `hz` 超范围 ⇒ 拒绝，`ACTION_FAILED`，**输出不变**
- `direction` ∉ {0,1} ⇒ 拒绝，`ACTION_FAILED`
- `stepper` 正在执行正式动作（`stepper_is_busy()`）⇒ 拒绝（避免抢引脚打断正式动作）
- 已在运行再发 START ⇒ 先停再按新参数重启（方便边测边改频率）

---

## 四、MQTT 命令

> ⚠️ **新增 2 个 Action 会使 Capability Registry 版本递增、全部 stable_id 重排。**
> 烧录后必须重新查 registry 确认 `v` 和 `k`，不要沿用旧值。

### 4.1 先查 registry（必做）

```json
{"c":"registry","i":"reg1","k":0}
```

预期（烧录后实测填写）：

| k | Action |
|---|---|
| 7 | `TEST_STEPPER_START` |
| 8 | `TEST_STEPPER_STOP` |

同时 **`VALVE_CLOSE` 9、`VALVE_OPEN` 10、`VIBRATION_START` 11、`VIBRATION_STOP` 12**（原 7/8/9/10）—— 云端/客户端硬编码需同步。

### 4.2 启动 / 停止

发到 **`guo_feeder/288485896ce4/down`**（QoS 1），`i` 每次必须不同：

```json
{"c":"action","i":"ts1","v":3,"k":7,"p":{"hz":100,"direction":0}}
```

```json
{"c":"action","i":"ts2","v":3,"k":8}
```

参数组合示例：

| 目的 | 报文 |
|---|---|
| 100 Hz 正向（默认） | `{"c":"action","i":"a1","v":3,"k":7}` |
| 200 Hz 正向 | `{"c":"action","i":"a2","v":3,"k":7,"p":{"hz":200,"direction":0}}` |
| 20 Hz 反向 | `{"c":"action","i":"a3","v":3,"k":7,"p":{"hz":20,"direction":1}}` |
| 停止 | `{"c":"action","i":"a4","v":3,"k":8}` |

---

## 五、串口命令（更省事）

```
tstep_start            → 100 Hz 正向
tstep_start 200 0      → 200 Hz 正向
tstep_start 20 1       → 20 Hz 反向
tstep_stop             → 停止
tstep_status           → 打印 running / hz / dir / elapsed / 引脚 / 上限
```

串口输出示例：

```
[TStep] START: 100 Hz (actual 100.0 Hz, res=16 bit, duty=50%), dir=0, step=13 dir=14 en=15; 无 30s 限制，最长 600000 ms
[TStep] running: 100 Hz dir=0 elapsed=10000 ms        ← 每 10 秒心跳
[TStep] STOP (user_stop): ran 23500 ms at 100 Hz, dir=0; EN disabled
```

---

## 六、用它定位「锁死不转」（核心用法）

按顺序做，每一步都可能直接定死根因：

### 步骤 1：常通电，测驱动器端电平

```
tstep_start 100 0
```

保持运行，万用表逐点测（**测 DRV8825 模块的引脚，不是 ESP32 排针**）：

| 测点 | 预期 | 异常含义 |
|---|---|---|
| 模块 `STP` 对 GND | ≈1.65 V（100 Hz/50%） | 0 V 或恒定 ⇒ **信号没到模块，线接错/虚接** |
| 模块 `DIR` 对 GND | 0 V 或 3.3 V **恒定不抖** | 抖动 ⇒ **STP/DIR 接反** |
| 模块 `EN` 对 GND | 0 V（低使能） | 3.3 V ⇒ 使能信号没到 |
| 模块 `VMOT` 对 GND | ≥ 11 V | <9 V ⇒ 电源不足 |
| 模块 `SLP` / `RST` | 3.3 V 或 5 V（高） | 0 V ⇒ 芯片休眠/复位（不输出） |

> 这一步能一次排除「相线以外」的全部接线问题。

### 步骤 2：听 / 摸 / 感受

- **手轻触电机轴**：是否有「一顿一顿」的步进感（微小抖动）？
  - 有 ⇒ 换向在发生，只是转不动（力矩/相线/细分问题）
  - 完全恒定无抖动 ⇒ **STEP 没到驱动器**（回到步骤 1）
- **听声音**：`嗡嗡` 高频声 = 斩波正常；`哒哒` = 换向但失步；完全静音 = 无换向
- **摸电机外壳**：30 秒内明显发热 ⇒ 相线接错（短路/反相），**立即 STOP**

### 步骤 3：改频率验证失步

| 频率 | 现象 | 推论 |
|---|---|---|
| 20 Hz 能转、100 Hz 不能 | 高速失步 | 力矩不足或加速度过大 ⇒ 降压/降流/加细分 |
| 20 Hz 也不能转 | 非速度问题 | 回到步骤 1/2 |

```
tstep_start 20 0
```

### 步骤 4：与正式模块对照

```
tstep_stop
```
然后用 MQTT 发 `STEPPER_ROTATE_STEPS`（RMT 脉冲源）。

| LEDC（test） | RMT（正式） | 结论 |
|---|---|---|
| 转 | 不转 | **问题在 RMT 输出**（软件可修） |
| 不转 | 不转 | **问题在硬件**（驱动器/电机/电源/相线） |

---

## 七、安全与副作用

| 项 | 说明 |
|---|---|
| **10 分钟硬上限** | `TEST_STEPPER_MAX_RUN_MS = 600000`。到点强制停止并打日志 `HARD LIMIT` |
| **不持 Critical Operation** | 允许系统正常重启（重启即恢复一切） |
| **⚠️ 会接管 STEP 引脚** | `ledcAttachPin` 覆盖 `stepper` 的 GPIO 矩阵路由 ⇒ **测试结束后 `stepper` 的 RMT 输出失效，必须重启设备才能恢复**（串口会打 WARN 提示） |
| **不新增配置** | 复用 `stepper` 的 `step_pin/dir_pin/en_pin/en_active_level/dir_invert` |
| **不新增 State/Event** | 避免污染生产状态机与事件计数 |
| **不新增依赖** | 只用 Arduino 内置 `ledcSetup/ledcAttachPin/ledcWrite` |

**为什么不用 RMT**：`stepper` 已占用 `RMT_CHANNEL_0` 并与 GPIO13 建立矩阵路由；若再装一个 RMT channel 到同一引脚，需要卸载前者，会让 `stepper` 模块状态不一致（静默失效）。LEDC 与 RMT 是完全独立的外设，接管同一引脚时行为确定，且反而不影响 `stepper` 的模块状态字段（只是物理输出被顶替，重启即恢复）。

---

## 八、未验证项

- ❌ **LEDC 输出的实际波形未经示波器测量**（万用表只能读直流均值）
- ❌ **`TEST_STEPPER_START/STOP` 的 MQTT 链路尚未实测**（烧录后验证）
- ❌ **`v` / `k` 值为推算，需烧录后 `registry` 查询确认**
- ❌ 电机是否因此转动 —— 这正是本模块要排查的目标，不作为已知结论
