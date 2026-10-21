# 步进电机（DRV8825）与振动马达模块接入实施计划

> **阶段**：规划 + 实施（v3 裁决已落地为代码）
> **基线**：固件仓 `wb` 分支 @ `99c0401`（工作区干净，`git status --porcelain` 无输出）
> **日期**：2026-10-22
>
> 任务书中出现的「VOLT 模块」经用户确认为笔误，本文统一以实际模块 **`valve`**
> （`src/app/valve.cpp/.h`，供水电磁阀，GPIO12）作为参考范例。

---

## ★ v3 裁决修订（2026-10-22 实施版 —— 本节优先于前文任何冲突处）

> 本节记录本轮人工裁决并**已经落地为代码**的决定。凡与 §8 / §10 / §11 / §12 等
> 早期章节冲突之处，**一律以本节为准**；早期章节保留作为设计推演过程。

| 编号 | 议题 | v3 裁决 | 落地位置 |
|---|---|---|---|
| V3-1 | STEP 脉冲生成 | **RMT 外设**（循环模式 + `loop_count`）产生脉冲；不再把 `esp_timer` 周期回调当作硬件脉冲方案 | `src/app/stepper.cpp`（`rmt_config` / `rmt_set_tx_loop_count` / `rmt_write_items` / `rmt_tx_stop`） |
| V3-2 | 框架事实 | 实际 SDK 为 **IDF 4.4.7（Arduino-ESP32 2.0.17）**，只能使用**旧版 RMT API**（`driver/rmt.h`）；`SOC_RMT_SUPPORT_TX_LOOP_COUNT=1`，每通道 48 字 | `platformio.ini` + SDK 头文件核实 |
| V3-3 | 脉冲参数 | 1.8° 整步 + 1/8 微步 = **1600 有效上升沿/圈**；STEP 周期 **10 ms ≈ 100 Hz**；使能后**保守等待 50 ms**（应用层值，**不是**数据手册最小值） | `stepper.json` / `stepper.cpp` 常量 |
| V3-4 | 出粮脉冲 | 累计余数分配：533 / 533 / 534；**只有成功完成才更新账本**；人工调整不进账本 | `stepper_plan_dispense_pulses()` |
| V3-5 | 步进 Action | 保留两个业务 Action + **新增独立急停 Action**（本轮明确要求，覆盖前版「不新增第三个」） | `STEPPER_DISPENSE_ROTATE` / `STEPPER_ROTATE_STEPS` / `STEPPER_EMERGENCY_STOP` |
| V3-6 | 收尾 | 所有退出路径（成功/失败/超时/急停）**统一汇聚 `stepper_finish()`**，幂等；**异步资源清理完成后才释放 Critical Operation** | `stepper_finish()` |
| V3-7 | 最长运行时间 | 步进与振动**均为 30 秒**，模块层执行；正常 120° 约 5.33 s 但**不得**当作超时值 | `STEPPER_ACTION_TIMEOUT_MS_HARD` / `VIBRATION_TIMEOUT_MS_HARD` |
| V3-8 | 振动硬件 | **单 GPIO**：两个物理马达共路、同时启停，**不**做成两个独立软件通道（推翻前版双通道设计） | `src/app/vibration.cpp` |
| V3-9 | 振动 Action | 单一 `VIBRATION_START`：`duration_ms=0` 持续（立即返回成功、不阻塞 Workflow）、`>0` 定时（到时自动关）；复用 `VIBRATION_STOP` | `vibration_start_action()` |
| V3-10 | 未来 UI | 30 秒最长运行时间限制必须在 UI 向用户展示（本次不实现 UI） | 本文 §V3-D |

### V3-A. 真机接线（GPIO 逻辑控制线）

| ESP32-S3 GPIO | 模块 / 信号 | 方向 | 说明 |
|---|---|---|---|
| GPIO13 | DRV8825 `STEP` | 输出（**RMT 驱动**） | 不得另作普通 GPIO 输出 |
| GPIO14 | DRV8825 `DIR` | 输出 | 方向建立后再使能驱动器 |
| GPIO15 | DRV8825 `EN`（nENBL） | 输出 | **低电平使能**；`en_active_level=0` |
| GPIO16 | 振动驱动电路控制端 | 输出 | 逻辑信号，**不**直接给马达供电；`active_level` 需按实际电路确认 |

### V3-B. Workflow 示例（出粮）

1. `VIBRATION_START {"duration_ms":0}` → 持续振动，**Action 立即成功返回**
2. `STEPPER_DISPENSE_ROTATE` → 120° 出粮（约 5.33 s），**期间振动仍运行（并发重叠）**
3. `STEPPER_DISPENSE_ROTATE` 成功 → `VIBRATION_STOP`
4. 步进失败 / 被急停 → 该步骤失败，Workflow 按现有失败传播终止；
   振动靠**自身 30 秒超时**兜底关闭（不依赖 Workflow 一定执行停止步骤）
5. 定时等待（如需）用 **Timer Trigger** 表达，`WorkflowStepType` 只有 `ACTION`/`TRIGGER`，
   **不得**新增 `delay` Action 或新步骤类型

### V3-C. 不可声称的边界（开环）

量化误差账本只解决**整数脉冲的数学量化误差**。软件**不得**声称：电机每次真实转过 120°、
无失步/卡步、拨片实际到位、RMT 波形已经示波器验证、急停已在真实机械运动中验证。

### V3-D. 未来 UI 必须展示

- 步进单次动作最长 30 秒（超时判失败并停止脉冲）
- 振动最长 30 秒（超时强制关闭）
- `stepper.enable` / `vibration.enable` 为 false 时所有相关 Action 被拒绝

### V3-E. 实施落地清单（2026-10-22 已完成编码 + 编译 + 烧录）

| 文件 | 变更 | 用途 |
|---|---|---|
| `src/app/stepper.h/.cpp` | 新增 | DRV8825 驱动 + RMT 脉冲 + 量化误差账本 + 3 个 Action + 统一收尾 |
| `src/app/vibration.h/.cpp` | 新增 | 单 GPIO 振动输出 + 持续/定时两模式 + 2 个 Action + 30 s 兜底 |
| `src/services/config_manager.h/.cpp` | 修改 | 新增 `stepper` / `vibration` 模块（`CONFIG_MODULE_COUNT` 8→10）+ 20 个 getter |
| `src/services/config_manager.cpp` | 修改 | **新模块首次上线引导**：Active 与 Backup 均缺失时写 `{}` 并置 loaded（否则 `config_set` 永远返回 module not found） |
| `src/services/system_state.h/.cpp` | 修改 | 8 个新 State 键 + `state_map` 映射 |
| `src/services/event_manager.h` | 修改 | 6 个新事件 + `SYSTEM_EVENT_COUNT` 改为 `EVENT_VIBRATION_ERROR + 1` |
| `src/main.cpp` | 修改 | `stepper_init/vibration_init`（第四层）、`stepper_task/vibration_task`（loop）、8 条串口调试命令 |
| `data/config/stepper.json`<br>`data/config/vibration.json` | 新增 | 配置文件模板（**本次未 uploadfs**，设备上靠 V3-E 引导生成空对象 + getter 默认值） |
| `test/stepper_quantization_host/stepper_quantization_test.py` | 新增 | 量化误差算法主机侧验证（7 项，纯数学） |

> ⚠️ `SYSTEM_EVENT_COUNT` 的原有注释声称「自动更新，无需维护」，但实现是手写的
> `EVENT_VALVE_ERROR + 1` —— 若不改，新事件会被队列边界检查丢弃。已修正。
>
> ⚠️ **禁止对当前设备执行 `uploadfs`**：设备上的 `mqtt.telemetry_enable=true`、
> `rtc.enable=true` 是本次实测写入的，而 `data/config` 的仓库值与之不同，
> uploadfs 会用仓库值覆盖设备实值（已发生过同类配置丢失事故）。

---

## 0. 阅读约定

| 标记 | 含义 |
|---|---|
| 【代码】 | 本次实际读到的源码事实（附文件路径 / 符号名） |
| 【文档】 | 仓库内正式文档的记载 |
| 【推断】 | 基于代码与惯例的设计建议，**尚未实现** |
| `NEEDS_CONFIRMATION` | 无法从当前仓库确认，需人工/实测给出证据，**不得臆测** |

---

## 1. 执行摘要与当前基线

### 1.1 结论速览

1. 建议新增**两个独立模块** `stepper`（DRV8825 步进）与 `vibration`（双振动马达），
   各自独立文件、独立 Config 模块、独立 `enable` 开关（见 §4.1 拆分依据）。
2. STEP 脉冲**不能**用主循环软件定时生成——实测 OLED `sendBuffer()` 每 50 ms 同步阻塞
   loop 约 6.4 ms（见 §10.2），会破坏脉冲间隔。推荐 **RMT 外设**，备选 `esp_timer`（§10）。
3. 振动马达定时启停**可直接复用** `computer_reset` 的 `reset/start/poll` 异步 Action
   模型（已上板验证的现成范式，§11）。
4. 新增 Action 会导致 **Capability Registry 的 `stable_id` 整体重排 + `version` 递增**，
   云端 `v`/`k` 全部失效，必须同步（§8.4，高风险项）。
5. 发现 valve 模块一处架构问题：**禁用时不注册 Action** ⇒ registry 随设备配置变化（§2.7）。

### 1.2 当前基线

| 项 | 值 | 来源 |
|---|---|---|
| 分支 / HEAD | `wb` @ `99c0401` | 【代码】`git rev-parse` |
| 工作区 | 干净 | 【代码】`git status --porcelain` |
| 主控 / 框架 | ESP32-S3 N16R8，`espressif32` + `arduino`，LittleFS | 【文档】`AGENTS.md` |
| 分层 | `app/` `automation/` `services/` `storage/` `log/` `cloud/` `test/` | 【代码】`src/` 目录 |
| 包含路径 | 一律写相对 `src/` 的显式路径（如 `"services/config_manager.h"`） | 【文档】`AGENTS.md` |
| 主循环 | 非阻塞，禁用 `delay()` | 【文档】`AI_RULES.md` |

---

## 2. valve 模块现有完整调用链（参考范例）

> 本节为下一阶段编码的事实依据，全部来自实际读取。

### 2.1 文件与职责 【代码】

`src/app/valve.cpp`（约 400 行）/ `src/app/valve.h`：

```
valve_init()          // 第四层初始化（main.cpp:553）
  ├ config_get_valve_gpio_pin() / active_level / enable / safety_timeout_sec
  ├ 安全超时钳位 1~300 s
  ├ enable=false 或 pin<0 ⇒ 直接 return（不配置 GPIO、不注册 Action）
  ├ gpio_config(OUTPUT, no pull)
  ├ valve_close()                       // 初始安全态
  ├ workflow_register_action(valve_open_desc / valve_close_desc)
  └ valve_update_state()                // state_set_bool(STATE_VALVE_STATUS, false)

valve_open() / valve_close() / valve_toggle()   // 同步，即时完成
  └ valve_set_gpio(bool open)
       ├ 判等去重（同状态直接 return）
       ├ 防风暴：MIN_OPERATION_INTERVAL_MS = 50
       ├ gpio_set_level(pin, open ? active_level : 1-active_level)
       ├ valve_update_state()            // System State
       ├ event_push(EVENT_VALVE_OPEN/CLOSE, "", "valve", 0, EVENT_POLICY_STATE, 0)
       └ log_emit(LOG_VALVE_OPEN/CLOSE/RATE_LIMITED/SAFETY_TIMEOUT, ...)

valve_task()          // loop 中（main.cpp:730）：仅做安全超时检测
valve_force_close()   // 异常强制关闭（dispense_guard 调用）
```

### 2.2 Action 描述符（即时完成型）【代码】

```c
// src/app/valve.cpp:82
static WorkflowActionDescriptor valve_open_desc = {
    .id          = "VALVE_OPEN",      // runtime id：大写下划线
    .name        = "电磁阀开",
    .module      = "valve",
    .description = "打开电磁阀",
    .params      = valve_params,      // 空数组
    .param_count = 0,
    .reset       = valve_action_reset,
    .start       = valve_open_start,  // start 内同步完成 ⇒ ACTION_SUCCESS/FAILED
    .poll        = valve_action_poll  // 仅 IDLE 兜底失败
};
```

`valve_action_poll()` 仅处理「未 start 直接 poll」的异常路径（`ACTION_IDLE ⇒ ACTION_FAILED`）。
**这类 Action 不适用于需要时长的执行**（步进、振动定时）——那是 `computer_reset` 的模型。

### 2.3 System State 【代码】

- 枚举：`STATE_VALVE_STATUS`（`src/services/system_state.h:31`）
- 映射表：`{"valve_status", STATE_VALVE_STATUS, STATE_TYPE_BOOL, true}`
  （`src/services/system_state.cpp:25`，`state_map[]`）
- 更新点：**唯一**——`valve_update_state()`，被 `valve_set_gpio()` 与 `valve_init()` 调用。

### 2.4 Event 【代码】

- `EVENT_VALVE_OPEN` / `EVENT_VALVE_CLOSE` / `EVENT_VALVE_ERROR`
  （`src/services/event_manager.h:45-47`）
- 位置：在 `EVENT_ERROR` **之后**「无需 workflow 响应的事件」区
- `event_push(EVENT_..., "", "valve", 0, EVENT_POLICY_STATE, 0)`（`valve.cpp:158/171`）
- 事件总数：`#define SYSTEM_EVENT_COUNT (EVENT_VALVE_ERROR + 1)`（`event_manager.h:53`）

### 2.5 ConfigManager 【代码】

- 模块名宏：`#define CONFIG_MODULE_VALVE "valve"`（`config_manager.h:52`）
- 模块表：`kModuleNames[CONFIG_MODULE_COUNT]`（`config_manager.cpp:70`，现 8 项）
- Getter 范式（`config_manager.cpp:3540-3562`）：
  ```c
  bool config_get_valve_enable(){ return module_ro(CONFIG_MODULE_VALVE)["enable"] | true; }
  int  config_get_valve_gpio_pin(){ return module_ro(CONFIG_MODULE_VALVE)["gpio_pin"] | -1; }
  ```
  即「键缺失 ⇒ 返回代码默认值」。
- `data/config/valve.json`：`{gpio_pin:12, active_level:1, open_duration_ms:0,
  safety_timeout_sec:300, enable:true}`
- 设备实值（2026-10-22 实测）：`enable=true`、`gpio_pin=12`

### 2.6 配置保存与重启策略 【代码】

- `config_save` 落盘成功 ⇒ `pending_save_finalize()`（config_manager.cpp:996）
  ⇒ Release + `request_restart` ⇒ **设备必然重启**（约 10 s 安全窗口后）
- 另有「修改配置后 5 分钟自动重启倒计时」（`config_task()`，config_manager.cpp:29 注释）
- 因此：**引脚 / enable 类配置必须走 init 读取，改配置 = 需要重启生效**（与 valve 一致）

### 2.7 发现的架构问题（仅记录，本阶段不修）

| ID | 问题 | 影响 |
|---|---|---|
| **V-1** | `valve_init()` 在 `enable=false` 或 `pin<0` 时**直接 return，不注册 Action**（valve.cpp:257-291） | Capability Registry 的 action 条目数随**设备配置**变化 ⇒ `stable_id` 重排、`version` 变更；同一份云端 `v/k` 在不同设备上语义不同 |
| **V-2** | `valve_init()` 先 `gpio_config()` 再 `valve_close()`（valve.cpp:276-279） | `gpio_config` 到写安全电平之间存在极短的不确定电平窗口（当前 active_level=1、默认低 ⇒ 恰好安全；但对「低电平使能」的 EN 类引脚是**真实风险**） |
| **V-3** | valve 无参数 Action 却保留空 `valve_params[]` 与注释中的 `hold_ms` | 设计遗留，非缺陷 |

> 【推断】新模块建议 **始终注册 Action、在 `start()` 内检查 enable/initialized**，
> 以规避 V-1（见 §12.2），并在 §15 记录为对 valve 的后续改进建议（本阶段不改）。

---

## 3. GPIO 资源盘点与候选分配

### 3.1 已占用 / 不可用 【代码 + 任务书】

| GPIO | 状态 | 依据 |
|---|---|---|
| 4 / 5 | OLED I²C SDA/SCL，RTC 共用 | `data/config/oled.json`、`rtc.json` |
| 6 / 7 | HX711 DT / SCK（weight，保留） | `data/config/weight.json` |
| 8 | computer_reset 脉冲（**全项目唯一硬编码点**） | `src/app/computer_reset.h:25` |
| 12 | 供水电磁阀继电器 | `data/config/valve.json`；**任务书禁止改动** |
| 36 / 37 | N16R8 PSRAM 相关 | 任务书 |
| 0 / 3 / 45 / 46 | ESP32-S3 strapping pin | 任务书（45 明确排除） |
| 19 / 20 | USB Serial/JTAG（当前烧录与串口监控使用 COM8） | 【推断】DevKitC-1 默认；若改用 USB-OTG 需重新评估 |
| 26–35 | 模块内 flash/PSRAM 可能占用 | 【推断】`NEEDS_CONFIRMATION`：需以实际模组原理图为准 |

### 3.2 推荐候选（4 个新引脚）

| 用途 | 建议 GPIO | 备选 | 依据 |
|---|---|---|---|
| 步进 `STEP` | **13** | 14 / 21 / 38 | 非 strapping、非 PSRAM、非 USB、未被占用 |
| 步进 `DIR` | **14** | 15 / 39 / 40 | 同上 |
| 步进 `EN`（低使能） | **15** | 16 / 41 / 42 | 同上；S3 上 15 **不是** strapping（与 ESP32 不同） |
| 振动马达 A（**储粮舱**）继电器 `IN` | **16** | 17 / 18 / 43 | 同上 |
| 振动马达 B（**出粮坡道**）继电器 `IN` | **17** | 18 / 21 / 44 | 同上（**R2 修订新增**：两路必须独立） |

- 【推断】选 13–17 连续号便于排线与后续扩展；全部为 config 驱动（不硬编码，遵循 valve 范式）。
- **R2 修订依据**：任务书 §6 要求「两个振动马达必须能够独立控制，不能因为关闭一个马达而意外
  关闭或启动另一个马达」⇒ 单引脚方案（原 §3.2 仅 GPIO16 一路）**无法满足**，故新增第二路。
  ⚠️ 冲突提示：任务书 §8.1 只列出 GPIO16 一路，与 §6 的双马达独立要求存在冲突；
  本计划按 §6（更强的功能约束）采纳两路，GPIO17 为候选值，实际接线留待人工确认。
- `NEEDS_CONFIRMATION`：**以上候选必须经「实物接线 + 上电实测」确认可用**。
  需要证据：① 该 GPIO 上电默认态；② 是否与板载外设冲突；③ 实际模组原理图。
  （按任务书 §8.1，接线确认**不作为编码阶段阻塞条件**，但需在上板前完成。）
- `NEEDS_CONFIRMATION`：DRV8825 模块的 **SLP / RST / M0 / M1 / M2 / FLT** 是否需要接 GPIO。
  任务书倾向「SLP/RST 保持有效电平、M0-M2 固定接线、FLT 可不接」⇒ 【推断】**先不占 GPIO**，
  但应在 §14 列为预留项（FLT 未来接入会再占 1 个 GPIO）。

---

## 4. 模块职责与文件布局

### 4.1 拆分 vs 合并（设计结论：拆分为两个模块）

| 维度 | stepper | vibration | 结论 |
|---|---|---|---|
| GPIO 数 | 3（STEP/DIR/EN） | 1（继电器 IN） | 差异大 |
| 时序模型 | 硬件脉冲序列（µs~ms 级，需外设） | 软件定时（ms~s 级，`millis()` 足够） | 机制不同 |
| 执行模型 | 异步 + 步数计数 + 可选加减速 | 异步 + 单一定时 + 单实例互斥 | 不同 |
| 独立 enable | 任务书强制要求 | 任务书强制要求 | 各自一份 |
| 工程惯例 | `valve` / `weight` / `computer_reset` 各自独立文件 | 同左 | 一致 |

⇒ **拆分为两个模块**，命名沿用现有风格（短小写）：

### 4.2 文件布局（新增，不修改既有文件）【推断】

```
src/app/stepper.h            // 对外接口（init/task + 查询）
src/app/stepper.cpp          // DRV8825 驱动 + Action 注册 + State/Event 维护
src/app/vibration.h
src/app/vibration.cpp
data/config/stepper.json     // 新增配置文件（设备端需同步下发）
data/config/vibration.json
```

- 放 `src/app/`（硬件能力层），与 `valve.cpp` / `computer_reset.cpp` 同级 ✓
- 包含写法：`#include "app/stepper.h"`、`#include "services/config_manager.h"` 等（相对 `src/`）✓

### 4.3 职责边界（严格遵守分层）

| 层 | 允许 | 禁止 |
|---|---|---|
| `stepper.cpp` / `vibration.cpp` | 读写自身 GPIO、维护自身 State、push 自身 Event、注册 Action、提供 `*_task()` | 不订阅/解析云端命令；不直接操作 MQTT；不自行改持久化配置；不跨模块写他人 State |
| `CommandManager` | 命令唯一入口 | 新模块不得绕过（本方案沿用现有 `action` 通道，无需改 CommandManager） |
| `WorkflowManager` | Action 唯一执行入口 | 同左 |
| `CloudManager` | 通信/编解码/ACK | 新模块不得新建 MQTT 客户端 |

---

## 5. ConfigManager 设计

### 5.1 模块名与注册点（**必须修改的 4 个位置**）

| # | 文件 | 修改 |
|---|---|---|
| C-1 | `src/services/config_manager.h` | 新增 `#define CONFIG_MODULE_STEPPER "stepper"`、`#define CONFIG_MODULE_VIBRATION "vibration"`（现有 8 个宏之后） |
| C-2 | `src/services/config_manager.cpp` | `#define CONFIG_MODULE_COUNT 8` → **10**（`config_manager.cpp:38`） |
| C-3 | `src/services/config_manager.cpp` | `kModuleNames[CONFIG_MODULE_COUNT]`（`config_manager.cpp:70`）新增两项 |
| C-4 | `data/config/stepper.json` / `vibration.json` | 新增两个配置文件（内容见下） |

> ⚠️ `CONFIG_MODULE_COUNT` 是**硬编码**；`s_modules[]`、备份/恢复遍历
> （`config_manager.cpp:284/303/629/667/699/753` 等多处循环）均依赖它 ⇒ 改一处即可覆盖，
> 但**必须同步**，否则新模块不参与 load/backup/recover。

### 5.2 字段设计

**`data/config/stepper.json`**（初版建议）

```json
{
  "enable": false,
  "step_pin": 13,
  "dir_pin": 14,
  "en_pin": 15,
  "en_active_level": 0,
  "dir_invert": false,
  "steps_per_rev": 1600,
  "default_speed_hz": 400,
  "max_speed_hz": 2000,
  "max_steps_per_action": 100000,
  "safety_timeout_ms": 10000,
  "disable_after_move": true
}
```

**`data/config/vibration.json`**

```json
{
  "enable": false,
  "hopper_pin": 16,
  "hopper_active_level": 1,
  "ramp_pin": 17,
  "ramp_active_level": 1,
  "default_duration_ms": 3000,
  "max_duration_ms": 30000,
  "min_interval_ms": 200,
  "safety_timeout_ms": 30000
}
```

> **R2 修订**：① 拆为 `hopper_`（储粮舱马达）与 `ramp_`（出粮坡道马达）两组独立引脚与
> 触发电平，保证互不影响；② `safety_timeout_ms` 由 60000 改为 **30000**（任务书 §6）。

字段说明：

| 字段 | 语义 | 默认值策略 |
|---|---|---|
| `enable` | **配置是否允许使用该模块** | `false`（安全默认） |
| `*_pin` | GPIO；`-1` 表示未配置 ⇒ 模块禁用 | 见 §3.2 |
| `en_active_level` | EN 的有效电平（DRV8825 EN 低使能 ⇒ 0） | `0` |
| `disable_after_move` | 动作结束后是否禁用驱动器 | `true`（省电/防发热） |
| `active_level`（vibration） | 继电器触发电平 | **`NEEDS_CONFIRMATION`**（见 §14） |

### 5.3 Getter 范式（新增到 `config_manager.cpp/.h`）

```c
bool config_get_stepper_enable(){ return module_ro(CONFIG_MODULE_STEPPER)["enable"] | false; }
int  config_get_stepper_step_pin(){ return module_ro(CONFIG_MODULE_STEPPER)["step_pin"] | -1; }
// … 其余同构
```

> 与 valve 的 `| true` 不同，这里**刻意用 `| false`**：键缺失时保持禁用（安全策略）。
> 该差异需在代码注释中写明理由。

### 5.4 概念分离（任务书强制要求，四个布尔不得混用）

| 概念 | 载体 | 说明 |
|---|---|---|
| 配置允许使用模块 | `config.stepper.enable` + `STATE_STEPPER_ENABLE` | 需 save + 重启生效 |
| 驱动器是否使能 | `STATE_STEPPER_EN_STATE` | EN 引脚状态，运行时可变 |
| 当前动作是否运行 | `STATE_STEPPER_RUNNING` | Action 生命周期 |
| 最近一次动作结果 | `STATE_STEPPER_LAST_RESULT`（枚举 int） | 不复用 RUNNING |

---

## 6. System State 字段与**全部维护位置**

### 6.1 新增字段（写在 `STATE_MAX` 之上）【推断】

```c
// src/services/system_state.h —— enum SystemStateKey
STATE_STEPPER_ENABLE,        // bool：配置允许
STATE_STEPPER_EN_STATE,      // bool：驱动器使能（EN 引脚语义）
STATE_STEPPER_RUNNING,       // bool：动作运行中
STATE_STEPPER_LAST_RESULT,   // int ：0=none 1=success 2=failed 3=rejected 4=timeout
STATE_STEPPER_LAST_STEPS,    // int ：最近一次实际步数（诊断用）

STATE_VIBRATION_ENABLE,      // bool：配置允许
STATE_VIBRATION_RUNNING,     // bool：振动中
STATE_VIBRATION_LAST_RESULT, // int ：同上枚举
```

### 6.2 **必须修改的位置（任务书要求逐项列出）**

| # | 文件 | 位置 | 内容 |
|---|---|---|---|
| S-1 | `src/services/system_state.h` | `enum SystemStateKey`（`STATE_MAX` 之前，约 :62 注释处） | 新增 8 个枚举项 |
| S-2 | `src/services/system_state.cpp` | `state_map[]`（:14 起，参照 :25 的 valve_status 行） | 新增 8 行 `{name, key, type, readable}` |

> 依据 `system_state.h:62` 注释：「新增 state 需要写在上方，同时维护 cpp 内的 state_map 映射表，
> `state_get_field_count()` / `state_get_field_name()` **会自动更新，无需维护**」。
> ⇒ 仅需 S-1 + S-2 两处。

### 6.3 **状态更新的代码位置（模块内）**

| 字段 | stepper 更新点 | vibration 更新点 |
|---|---|---|
| `*_ENABLE` | `stepper_init()`：读 config 写入（1 处） | `vibration_init()`（1 处） |
| `*_EN_STATE` | 使能/禁用驱动器的封装函数（2 处：enable / disable） | —（继电器无需独立使能态，由 RUNNING 表达） |
| `*_RUNNING` | `action_start()` 置 true；`action_poll()` 完成/失败置 false；`stepper_task()` 超时兜底置 false | 同左（start / poll / task 兜底） |
| `*_LAST_RESULT` | poll 成功⇒1、失败⇒2、被拒⇒3、超时⇒4；`action_reset()` 不清除（保留最近结果） | 同左 |
| `*_LAST_STEPS` | poll 完成时写入实际步数 | — |

> 【推断】建议两个模块各自提供 `static void xxx_update_state()` 集中封装，
> 参照 `valve_update_state()`（`valve.cpp:109`）的写法，避免散落。

---

## 7. Event 设计与发布时机

### 7.1 现有规则【代码】

`src/services/event_manager.h:38-53`：

- 「所有 **workflow event trigger** 对应的新增事件放在 `EVENT_ERROR` **之前**」，
  且**必须同步** `event_manager.cpp` 的 `event_from_string()` / `event_to_string()`；
- 「所有**无需 workflow 响应**的事件放在**下方**」（valve 三个事件即在此区）；
- `#define SYSTEM_EVENT_COUNT (EVENT_VALVE_ERROR + 1)`。

### 7.2 建议新增事件（放 `EVENT_VALVE_ERROR` 之后，即「无需 workflow 响应」区）

| 事件 | 时机 |
|---|---|
| `EVENT_STEPPER_START` | Action start 成功、脉冲开始（含步数/方向信息放 `data`） |
| `EVENT_STEPPER_DONE` | 脉冲发送完成（成功） |
| `EVENT_STEPPER_ERROR` | 失败 / 参数非法 / 并发被拒 / 超时（原因放 `data`，**不新增「被拒」独立事件**） |
| `EVENT_VIBRATION_START` | 继电器吸合、振动开始 |
| `EVENT_VIBRATION_STOP` | 定时到期或主动停止 |
| `EVENT_VIBRATION_ERROR` | 失败 / 被拒 / 超时（同上，合并） |

**不新增**的事件及理由：

- 「步进驱动器使能状态变化」⇒ 用 **`STATE_STEPPER_EN_STATE`** 表达（它是**状态**而非事件，
  且任务书明确禁止机械凑数）；
- 「失败」与「被拒」合并为单一 `*_ERROR` 事件（用 `data` 字段区分），避免 6 个事件变 10 个。

### 7.3 必须修改的位置

| # | 文件 | 修改 |
|---|---|---|
| E-1 | `src/services/event_manager.h` | 枚举新增 6 项（置于 `EVENT_VALVE_ERROR` 之后） |
| E-2 | `src/services/event_manager.h` | `SYSTEM_EVENT_COUNT` 宏改为 `(EVENT_VIBRATION_ERROR + 1)` |
| E-3 | `src/services/event_manager.cpp` | **若**放在 `EVENT_ERROR` 之后 ⇒ 按注释不需改字符串表；**但需实测确认** `event_to_string()` / `event_from_string()` 的实现是否按枚举遍历（`NEEDS_CONFIRMATION`，见 §14） |

> `NEEDS_CONFIRMATION`：`event_manager.cpp` 中两个字符串转换函数的实现范围。
> 若它们为覆盖全枚举的数组/switch，则 E-3 必须改。

---

## 8. Action 定义与 Workflow 注册

### 8.1 Action 设计

> **R1 修订**：步进模块对外**只有两个业务 Action**，不再注册 `STEPPER_STOP` /
> `STEPPER_ENABLE` 作为业务 Action（原计划 3 个）。停止与禁用改为**模块内部能力**，
> 由超时/异常路径与 `disable_after_move` 配置驱动（见 §10.6、§12）。
> 依据任务书 §2.1：「该 Action 成功结束后，步进电机模块应正确结束本次动作，
> **不需要 Workflow 再单独发送关闭步进电机的 Action**」。

| Action ID（runtime id） | module | 参数 | 语义 | 执行模型 | 复用范式 |
|---|---|---|---|---|---|
| **`STEPPER_DISPENSE_ROTATE`** | `stepper` | **无** | 出粮：三瓣拨片旋转固定 **120°**，脉冲数由模块按 §8.3 的误差分配算法决定 | 异步（RUNNING → poll 完成） | 结构同 `computer_reset`，脉冲由硬件外设 |
| **`STEPPER_ROTATE_STEPS`** | `stepper` | `pulses`(int，**必填**) | 人工调整：输出指定数量的**有效 STEP 上升沿** | 异步 | 同上 |
| `VIBRATION_START` | `vibration` | `motor`(int)、`duration_ms`(int，可选) | 启动指定马达并在 `duration_ms` 后自动停止 | 异步（定时） | **直接复用 `computer_reset` 模型** |
| `VIBRATION_STOP` | `vibration` | `motor`(int) | 停止指定马达 | 即时 | 同 valve |

参数语义约定：

| 参数 | 取值 | 说明 |
|---|---|---|
| `pulses` | 1 ~ `max_steps_per_action` | **有效 STEP 上升沿个数**；20 ⇒ 20 个上升沿（不是 GPIO 翻转次数，翻转次数为其 2 倍）。方向沿用 `dir_invert` 配置 |
| `motor` | `1`=储粮舱(hopper) / `2`=出粮坡道(ramp) / `3`=两者 | 两路独立，互不影响；`VIBRATION_STOP` 仅停指定马达 |

> 【推断】若后续实测发现带 `motor` 参数的 Action 在 Workflow 中组合不便，可**备选**拆为
> 4 个 Action（`VIBRATION_HOPPER_START/STOP`、`VIBRATION_RAMP_START/STOP`）——
> 但这会增加 registry 条目数，加剧 §8.5 的 stable_id 重排，**不推荐**，仅列为备选。

参数声明用 `WorkflowParam[]`：`{name, type, unit, description}`
（`workflow.h:157`），类型 `PARAM_INT/PARAM_FLOAT/PARAM_BOOL/PARAM_STRING`（:93-96），
上限 `WORKFLOW_MAX_PARAM = 8`（:50）。

### 8.2 参数如何传入【代码】

- 云端/命令 payload 的 `p` 对象经 `workflow_parse_params()`（`workflow.cpp:3308`）
  解析为 `WorkflowParamValue[]`（key = 参数名，最多 8 个）；
- 临时 Action：`command_execute_action()` 把 `rt->message.payload` 原样交给
  `workflow_enqueue_action()`（`command_manager.cpp:836`）⇒ `enqueue_temp_action()`
  （`workflow.cpp:3972`）解析。

⇒ 云端命令示例（**设计**，未实现）：
```json
{"c":"action","i":"s1","v":<新版本>,"k":<STEPPER_ROTATE 的 stable id>,"p":{"steps":1600,"speed_hz":400,"direction":1}}
```

### 8.3 120° 出粮旋转的脉冲计算与量化误差补偿（**R2 修订新增，重点**）

#### 8.3.1 基础计算（核对结果：任务书 §3.1 的计算正确）

| 项 | 值 | 推导 |
|---|---|---|
| 整步角 | 1.8° | 电机参数 |
| 每圈整步数 | 200 | 360 / 1.8 |
| 微步细分 | 1/8 | 配置 |
| **每圈 STEP 脉冲数** | **1600** | 200 × 8 ✓ |
| 每脉冲对应角度 | **0.225°** | 360 / 1600 |
| 单次出粮目标角 | 120° | 固定，不提供可配置角度参数 |
| **单次理想脉冲数** | **533⅓**（533.333…） | 1600 × 120 / 360 = 1600/3 ✓ |

⇒ **核对结论**：单次 120° **无法**用同一个整数脉冲数精确表示——533 对应 119.925°
（−0.075°），534 对应 120.15°（+0.075°）。必须处理量化误差。

#### 8.3.2 不补偿时的累计误差（为什么必须处理）

若每次固定取 533：

- 单次欠发 = 533⅓ − 533 = **1/3 个脉冲**
- n 次后累计欠发 = **n/3 个脉冲** = n × 0.075°
- 例：n=3 欠 1 脉冲；n=30 欠 10 脉冲（2.25°）；n=100 欠 33.3 脉冲（**7.5°**）

⇒ 误差随次数**线性无界增长**，长期必然导致拨片相位显著偏移。

#### 8.3.3 两个方案的计算与比较

**方案 1：累计余数 / 误差分配（推荐 ✅）**

维护整数账本（避免浮点累积误差）：

```
ideal_3n        = n × 1600                       // 累计理想脉冲 × 3（整数域）
target_total(n) = floor(n × 1600 / 3)            // 第 n 次后应达到的累计脉冲数
pulses(n)       = target_total(n) − issued_total // 本次实际应发
issued_total   += pulses(n)
```

逐次展开（`issued_total` 初值 0）：

| n | floor(n×1600/3) | 本次发 | 累计 | 本次偏差 | 累计误差 |
|---|---|---|---|---|---|
| 1 | 533 | **533** | 533 | −0.333 脉冲 | 0.333 |
| 2 | 1066 | **533** | 1066 | −0.333 | 0.667 |
| 3 | 1600 | **534** | 1600 | +0.667 | **0** |
| 4 | 2133 | **533** | 2133 | −0.333 | 0.333 |
| 5 | 2666 | **533** | 2666 | −0.333 | 0.667 |
| 6 | 3200 | **534** | 3200 | +0.667 | **0** |

⇒ 输出序列为 **533, 533, 534, 533, 533, 534, …**（每 3 次出现一次 534）。

**数学验证（不能只看示例）**：

- 实际累计 = `floor(n × 1600/3)`；理论累计 = `n × 1600/3`
- 误差 = 理论 − 实际 = `frac(n × 1600/3)` ∈ **[0, 1) 个脉冲**
  ⇒ **累计误差上界 = 1 个脉冲 = 0.225°，且该上界与 n 无关（有界，不累积）**
- 闭合性验证：3 次累计 = 1600 脉冲 = 360°，而 3 × 120° = 360° ⇒ **完全吻合，无残余**
- 每 3 次为一个完整周期，周期末误差精确归零

**方案 2：事后反向补偿（不采用 ❌）**

| 维度 | 分析 |
|---|---|
| 是否解决根本问题 | **否**。反向脉冲同样只能取整 ⇒ 只是把量化误差搬到反方向，未消除 |
| 额外机械动作 | 正转后立刻反转 ⇒ 增加拨片/齿轮往复磨损、噪声与时间开销 |
| 对本机构的意义 | 三瓣拨片位置由机械结构决定，0.225° 级的反向微步无实际校正价值 |
| 收益 vs 代价 | 方案 1 已将误差压到 ≤1 脉冲（0.225°）；反向补偿最多再改善同一量级，收益远小于代价 |
| 任务书要求 | §3.3 明确：采用误差分配后**不得**再额外加入反向补偿 |

⇒ **决定：采用方案 1（累计余数/误差分配），不加入任何反向补偿动作。**

#### 8.3.4 两个 Action 的独立执行语义（任务书 §3.2）

| Action | 是否更新误差账本 | 脉冲数来源 | 是否受 `max_steps_per_action` 约束 |
|---|---|---|---|
| `STEPPER_DISPENSE_ROTATE` | **是**（更新 `dispense_count` 与 `issued_total`） | 由 §8.3.3 算法算出（533/534） | 是（533/534 远低于上限） |
| `STEPPER_ROTATE_STEPS` | **否**（不触碰 120° 专用账本） | 用户参数 `pulses` 原样输出 | 是（校验 `1..max`） |

⇒ 人工调整的脉冲数**不进入**出粮的累计补偿计算；两个 Action 语义完全独立。

#### 8.3.5 补偿能力的边界（任务书 §3.4，必须如实表述）

补偿机制**只**解决「整数 STEP 脉冲带来的数学量化误差」，使累计误差**有界**
（≤ 1 脉冲 = 0.225°，且每 3 次归零）。

它**不能**检测或纠正：

- 电机失步
- 机械滑动 / 传动间隙
- 拨片实际位置与理论位置不一致
- 断电后机械位置变化

本项目仍为**开环**设计，**不新增**编码器或任何位置反馈硬件。

> ⚠️ 表述纪律：文档中**不得**把「累计量化误差有界」写成「保证真实机械位置永不偏移」
> 或「保证出粮角度精确」。正确表述为：**在电机未失步的前提下，累计指令脉冲与理论
> 目标角的偏差不超过 1 个微步（0.225°）**。

### 8.4 注册位置【代码】

- `workflow_register_action(&desc)`（现有 5 个注册点：valve×2、computer_reset、
  MiThermometer×2、weight）
- 【推断】在 `stepper_init()` / `vibration_init()` 内注册，位置同
  `valve.cpp:286`（第四层，必须在 `workflow_init()` 之后）

### 8.5 ⚠️ Stable ID 重排风险（高风险，必须处理）【代码】

`src/automation/capability_registry.h:67`：

> `runtime_id` **按字符串升序排序，排序后的 index 即 `stable_id`**；与注册顺序无关。

当前（实测 `v=1, n=6`）：
```
0 COMPUTER_RESET · 1 MI_THERMO_START_SCAN · 2 MI_THERMO_STOP_SCAN
3 MQTT_TEST · 4 VALVE_CLOSE · 5 VALVE_OPEN
```

新增 5 个 Action 后（按升序重排，`S`/`V` 开头会插入不同位置）⇒

1. **所有 `stable_id` 重新编号**，`valve` 的 `k` 从 4/5 变为新值；
2. `capability_get_action_version()` 递增（`v` 变化）；
3. 云端/APP 缓存的 `v/k` **全部失效**，旧命令会得到
   `action registry version mismatch`（本项目已实测该错误，见 §16 依据）。

**必须动作**：
- 新增后**重新下发** `{"c":"registry","i":..,"k":0}` 取新 `v` 与全部 `k`；
- 同步更新云端（Cloudflare Worker / Pages）与 APP 侧的 registry 缓存；
- 在文档中记录「新增 Action ⇒ 云端协议兼容动作」为**发布检查项**。

---

## 9. CommandManager 命令入口与参数

### 9.1 云端 MQTT 路径（无需改 CommandManager）【代码】

- 一级路由 `command_is_execute()` = `startsWith("execute_")`（`command_manager.cpp:629`）
- `execute_router()` 匹配 `execute_action`（:2217）⇒ `command_execute_action()`（:810）
- 报文形态由 `cloud_manager.cpp:1348-1370` 转换：
  - `c` 取 `"action"` 或 `"execute_action"` 均可；
  - **必须带顶层 `v`（registry 版本）与 `k`（stable id）**；
  - `v` 不符 ⇒ `ERROR_VERSION_MISMATCH`（`e=3`）；
  - `k` 无效 ⇒ `ERROR_INVALID_OBJECT`。

⇒ **新模块不需要新增 CommandManager 分支**，注册 Action 后自动获得该通道。

### 9.2 串口调试命令（建议新增，与 valve 对齐）【推断】

`main.cpp:764-781` 现有 `valve_open` / `valve_close` / `valve_toggle` / `valve_status`；
建议在同处 `serial_debug_command_process()` 新增：

```
stepper_rotate <steps> [speed_hz] [dir]
stepper_stop
stepper_enable / stepper_disable
stepper_status

vibration_on [duration_ms]
vibration_off
vibration_status
```

### 9.3 参数校验与错误返回【推断】

- **参数存在性 / 有效性 / 范围**（任务书 §2.1 强制）：
  - `STEPPER_ROTATE_STEPS` 的 `pulses` **必填**：缺失 ⇒ 拒绝；
    非正整数、或 > `max_steps_per_action` ⇒ 拒绝；
  - `VIBRATION_START` 的 `duration_ms` 缺省取 `default_duration_ms`，
    显式传值时**必须** ≤ `max_duration_ms`（30000）⇒ 超出拒绝；
  - `motor` 取值不在 {1,2,3} ⇒ 拒绝；
  ⇒ 拒绝路径：零 GPIO 动作，`ACTION_FAILED` + 日志写明原因 + `LAST_RESULT=rejected`；
- 模块未初始化 / 配置禁用 ⇒ 同上（**不执行任何 GPIO 动作**）；
- 并发（该路已有动作在跑）⇒ 拒绝，`LAST_RESULT=rejected`（busy），
  **不改变**正在运行动作的任何状态；
  - 步进：单脉冲通道 ⇒ 全局互斥；
  - 振动：**按路独立**互斥（储粮舱与坡道可同时运行，见 §11.1）。

### 9.4 急停与取消机制（**R1b 修订新增**，任务书 §2.2）

**决定：本次不新增第三个业务 Action**（无 `STEPPER_ESTOP`）。依据任务书 §2.2：
「对外业务 Action 为上述两个……不要自行增加第三个业务 Action」。

运行中的步进动作受**既有**机制约束（不发明新机制）：

| 机制 | 来源 | 作用 |
|---|---|---|
| **模块 30 s 超时** | `stepper_task()`（§12.5） | 最长 30 s 后**必然**停止脉冲并结束 Action |
| **Action `reset()` 收尾** | `WorkflowActionDescriptor.reset` | Workflow Stop / Clear / 重置时，引擎调用 `reset()` ⇒ 模块**必须**在 `reset()` 内停止脉冲、释放上下文、关闭输出 |
| Action/Workflow 超时 | `COMMAND_ACTION_TIMEOUT_MS`（10 min，`command_manager.cpp:84`）、Workflow `timeout_ms` | 引擎层兜底（**远大于** 30 s，不能替代模块限制） |
| **critical operation** | `system_command_critical_operation_acquire/release`（§12.6） | 动作期间推迟系统重启；系统级释放由 `system_command` 统一管理，本模块不复制 |

> **关键事实**【代码】：`computer_reset_action_reset()`（computer_reset.cpp:246）已示范该模式——
> 「若该实例上一次执行尚未收尾（被 Stop / Clear / 重置打断），这里必须释放上下文，
> 否则 GPIO 会一直保持 HIGH」。⇒ **取消的正确实现位置是 `reset()`**，
> 而不是假设 Workflow 能「直接销毁」一个运行中的 Action。

**待确认事项（不在本次扩大功能范围）**：

- 现有机制能保证的是「**最长 30 s 内必停**」；
- 若业务要求「**立即**停止（显著早于 30 s）」，现有架构**未提供**独立急停接口
  ⇒ 列为 §F 未解决事项，**不在本次新增第三个 Action**。
  （注：可通过 `reset()` 路径由引擎取消，但那依赖 Workflow 的取消动作，
  不是模块主动的急停。）

---

## 10. 步进电机脉冲生成与非阻塞执行方案（重点）

### 10.0 DRV8825 初始化时序（**R3 修订新增**，依据 TI DRV8825 数据手册）

> 依据：**TI DRV8825 数据手册**（SLVSCF4 系列）的引脚定义与时序参数。
> 以下「数据手册值」为手册标称值；「应用层值」为本计划为留裕量而取的保守值。

#### 10.0.1 控制引脚的有效电平（核对任务书 §4.1 预期）

| 引脚 | 有效电平 | 数据手册依据 | 任务书预期 | 核对 |
|---|---|---|---|---|
| `nENBL`（EN） | **低电平使能**，高电平禁用 | ENBL 为低有效（内部下拉） | 「EN 高 ⇒ 禁用，拉低 ⇒ 使能」 | ✅ **正确** |
| `nSLEEP` | **高电平工作**，低电平睡眠 | nSLEEP 低有效（内部下拉） | 「满足工作要求」 | ✅ 需外部确保为高 |
| `nRESET` | **高电平正常工作**，低电平复位 | nRESET 低有效 | 同上 | ✅ 需外部确保为高 |
| `DIR` | 电平决定方向 | — | 先设 DIR 再发 STEP | ✅ 正确 |
| `STEP` | **上升沿**触发一个微步 | — | — | ✅ |

> ⚠️ `nSLEEP` / `nRESET` 在 DRV8825 上均为**内部下拉**（低有效）：
> 若载板未将其上拉到逻辑高，**驱动器不会工作**。本项目初期按固定接线处理（不占 GPIO），
> ⇒ 该接法列为硬件验证项（`NEEDS_CONFIRMATION`，但按任务书 §8.2 **不作为编码阻塞**）。

#### 10.0.2 关键时序参数

| 参数 | 数据手册值 | 本计划取值 | 说明 |
|---|---|---|---|
| `nSLEEP` 唤醒 → 可接受 STEP | **1 ms**（t_wake，最大） | **50 ms** | 任务书预期 50 ms；手册仅需 1 ms ⇒ 50 ms 属**应用层保守等待**，安全裕量 50 倍 |
| DIR 建立 → STEP 上升沿 | **200 ns**（最小） | ≥ 数 µs（固件自然间隔） | 写 DIR 后再启动脉冲引擎，间隔远超 200 ns ✅ |
| STEP 脉宽（高 / 低） | **1.9 µs**（最小，各） | 5 ms（100 Hz 半周期） | 裕量 >2000 倍 ✅ |
| VMOT 上电稳定 | 手册未给固定值，要求 VMOT 稳定后再操作 | 随系统 boot 完成 | — |

#### 10.0.3 采纳的初始化顺序（核对结论：任务书预期顺序基本正确，仅补充细节）

1. **EN 保持高电平（禁用）**，且 STEP 置 **LOW** —— *顺序要点：先写电平，再配置 GPIO 为输出
   （见 §12.1），避免配置瞬间产生非预期上升沿*
2. 设置 **DIR**、**nSLEEP**、**nRESET** 及细分（M0/M1/M2 固定接线）的目标电平
   —— *nSLEEP / nRESET 必须确保为逻辑高*
3. 确认 SLEEP/RESET/细分满足工作要求（若受 GPIO 控制则读取确认；固定接线时由硬件保证）
4. 将 **EN 拉低**（使能驱动器）
5. **等待 50 ms**（数据手册仅需 1 ms；本值为应用层保守等待，含电荷泵与电流稳定裕量）
6. 设置 / 确认 **DIR**（DIR 建立时间 ≥200 ns 已自然满足），然后输出 STEP 脉冲
7. 动作完成后，按 `disable_after_move`（默认 `true`）**禁用驱动器**（EN 拉高）

**额外 STEP 上升沿风险与对策**：

| 风险 | 对策 |
|---|---|
| GPIO 配置为输出时若默认高 ⇒ 产生一个非预期上升沿 | STEP 初始电平固定为 **LOW**，且**先写电平再 `gpio_config()`** |
| EN 使能瞬间 DIR 未稳定 ⇒ 方向错误 | DIR 在步骤 2 设置（EN 使能**之前**），步骤 6 再确认一次 |
| 定时器/RMT 启动即发第一个脉冲，DIR 未稳 | 脉冲引擎启动前已写 DIR，且间隔 ≥ 数 µs ≫ 200 ns |

⇒ 结论：任务书 §4.1 的预期顺序**成立**，本计划按上述 7 步执行并补充了「先写电平后配置
输出」与「DIR 双重确认」两个细节。

### 10.1 需求参数（R3 修订：频率定为 100 Hz）

- 1/8 细分，两相步进电机 200 整步/圈 ⇒ **1600 微步/圈**（每脉冲 0.225°）
- **目标脉冲频率 100 Hz**（周期 10 ms）——任务书 §4.2 指定为当前默认设计目标
- 每个有效 **STEP 上升沿 = 一个微步指令**（上升沿计数，非 GPIO 翻转计数）
- 单次出粮脉冲数：**533 或 534**（按 §8.3.3 误差分配）
- 单次出粮脉冲输出时间：533 × 10 ms = **5.33 s**；534 × 10 ms = **5.34 s**
  （不含驱动器初始化 50 ms 与动作前后等待）
- DRV8825 STEP 最小脉宽 1.9 µs（高/低各）⇒ 100 Hz 下脉宽 5 ms，**裕量 >2000 倍** ✓
- ⚠️ 5.33 s 只是**预计耗时**，**不是超时值**；超时统一 30 s（见 §12.5）

### 10.2 为什么不能用主循环软件定时（关键实测依据）

2026-10-21 G-4 窗口实测（`docs/review/G4-Window-Verdict-2026-10-21.md`）：

- **OLED `sendBuffer()` 每 50 ms 同步阻塞 loop 约 6.4 ms**
  （`src/app/oled.cpp`，`oled.frame_delay=50`）；
- 该阻塞位于 P99.9 分位，稳定出现（18 次/秒）。

#### 对 100 Hz（周期 10 ms）的具体影响——**不能因为频率低就认为软件定时可靠**

任务书 §4.2 明确要求：不得因 100 Hz 频率低就认定任何软件定时实现一定可靠。
按实测阻塞值定量分析：

| 频率 | 周期 | OLED 6.4 ms 阻塞的影响 |
|---|---|---|
| 320–533 Hz | 1.9–3.1 ms | 阻塞 6.4 ms > 周期 ⇒ **必然漏 2–3 个脉冲**（丢步） |
| **100 Hz** | **10 ms** | 不会漏脉冲（10 > 6.4），但该周期被拉长至 **10 → 16.4 ms** ⇒ **抖动达 64%** |

即使不丢步，100 Hz 下的 64% 抖动仍会导致：

- 脉冲间隔严重不均 ⇒ 转速波动、电机振动与噪声明显增大；
- 在电流裕量不足或带载时，**瞬时失步风险上升**（步进电机对加速度突变敏感）；
- 叠加 loop 中其它任务（`cloud_task` / `log_task` / MQTT 发布）的耗时，实际抖动更大。

⇒ **结论不变**：即便目标是 100 Hz，软件 loop 定时仍不可靠；必须采用硬件外设方案
（**自主产生脉冲、不依赖主循环时序**，同时满足任务书 §4.2 的优先评估要求）。

### 10.3 候选方案对比

| 方案 | CPU 占用 | 精度 | 复杂度 | 现有依赖 | 结论 |
|---|---|---|---|---|---|
| **A. loop 软件定时** | 低 | ✗ 受 OLED 6.4 ms 阻塞破坏 | 低 | — | ❌ 否决 |
| **B. `esp_timer` 周期回调** | 每 2 ms 一次回调（~500 Hz），每次数 µs ⇒ <0.5% | 良（任务调度抖动 ~数十 µs） | 低 | 工程已用 `esp_timer.h`（`loop_stats.cpp`） | ✅ **推荐首选** |
| **C. `gptimer` + ISR** | 近 0 | 最优 | 中 | IDF 自带，工程未用 | 备选（若 B 抖动不可接受） |
| **D. RMT 脉冲序列** | ≈0（硬件发完 + 完成中断） | 优 | 中高 | IDF `driver/rmt.h`，**工程未用** | 备选（最优性能，需先验证 API 可用性） |

- 三者均为 **ESP-IDF 自带组件**，不引入第三方库 ✓（任务书禁止新增依赖）
- B/C/D 的回调/中断**不受 loopTask 的 OLED 阻塞影响**（独立任务/硬件）✓

### 10.4 推荐方案（分阶段）

1. **第一步实现 B（`esp_timer`）**：
   - 周期 = `1_000_000 / (2 * speed_hz)` µs（半个周期翻转一次，形成方波）；
   - 回调内：`gpio_set_level(STEP, level)` + `level = !level` + 步数计数；
   - 达目标步数 ⇒ 停止定时器 + 回调通知 Action 完成；
   - 回调**禁止** `Serial` / 动态分配 / 阻塞调用。
2. **若实测抖动导致失步**（NEEDS_CONFIRMATION）⇒ 升级 C（`gptimer`）或 D（RMT）。
3. **加减速**：初期建议**固定低速启动**（如 200 Hz），预留
   `ramp` 参数位；梯形加减速可在 B 的回调内按步数调整周期实现。

### 10.5 与 Critical Operation 的交互（重要约束）【代码】

`enqueue_temp_action()`（`workflow.cpp:4009`）在入队前调用
`system_command_critical_operation_acquire()`：

- 临时 Action 执行期间**系统不可重启**；
- ⇒ 步进/振动 Action **必须有可靠的超时与强制停止**（否则阻塞重启），
  且 `safety_timeout_ms` 应显著小于 10 s 安全窗口的量级（建议 ≤ 10 s，见 §5.2）。

### 10.6 动作结束后何时禁用驱动器

- 由 `disable_after_move`（默认 `true`）配置；
- 动作完成/失败/超时后，若该配置为 true ⇒ EN 置禁用电平；
- 该行为发生在 `action_poll()` 完成分支与 `stepper_task()` 兜底分支**两处**（见 §12）。

---

## 11. 振动马达定时启停方案

直接复用 `computer_reset.cpp` 已验证的异步 Action 模型：

```
reset(): 若实例仍持有上下文 ⇒ 释放 + 强制关闭（防 Stop 后马达长转）
start(): 检查 initialized/enable/互斥 ⇒ 置 GPIO active，记录 start_ms，result=ACTION_RUNNING
poll() : millis()-start_ms >= duration_ms ⇒ 置 GPIO inactive，result=ACTION_SUCCESS
task() : 安全兜底（Action 被丢弃/异常时强制关闭）+ 超时检测
```

差异（相对 computer_reset）：

| 项 | computer_reset | vibration（R2 修订） |
|---|---|---|
| 通道数 | 1（GPIO8） | **2 路独立**（`hopper_pin` / `ramp_pin`） |
| 并发 | 多实例上下文池（4 槽） | **每路单实例互斥**；两路之间**互不阻塞**（可同时运行） |
| 时长 | 固定 800 ms | `duration_ms` 参数（默认 3000，钳位 `max_duration_ms`=30000） |
| 超时 | — | **每路独立 30 s**（`safety_timeout_ms`=30000，见 §12.5） |
| 防风暴 | 无 | `min_interval_ms`（默认 200），**按路独立**计时 |

#### 11.1 双马达独立性（任务书 §6 强制要求）

- 每路维护**独立**的状态：`{running, start_ms, duration_ms, active_level, pin}`；
- `VIBRATION_STOP(motor=1)` **只**停储粮舱马达，**不得**触碰坡道马达的输出与状态；
  反之亦然；
- 停止一路时**不重新初始化**另一路的 GPIO，也不改写另一路的 `last_operation_time`；
- 每个马达对应独立的 State 与事件标记（见 §6.1 的 `STATE_VIBRATION_*`
  —— 实现时按路区分，或用 `data` 字段携带 `motor` 标识）。

- 【推断】`vibration` 需提供 `vibration_force_stop(motor)`（参照 `valve_force_close()`），
  供超时与异常路径使用；超时由 `vibration_task()` 在 loop 中检测，**不依赖 Workflow 调用停止**。
- 继电器触发电平 `active_level` 必须经实测确认（`NEEDS_CONFIRMATION`；按 §8.2 不作为编码阻塞）。

#### 11.2 三类超时/等待的区分（任务书 §6 要求明确区分）

| 类别 | 位置 | 作用 |
|---|---|---|
| **模块自身超时保护** | `vibration_task()`（loop 内） | 每路 30 s 硬上限；到期强制关闭该路输出。Workflow 中断也能生效 |
| Workflow 定时等待 | Workflow 的 **Timer Trigger** | 业务流程中的「等待 N 秒」（§11.5 步骤 3/6） |
| Workflow 步骤顺序 | Workflow Step 编排 | 决定 Action 的先后与成败依赖 |

> ⚠️ **Workflow 的定时等待不能替代振动模块自身的 30 s 最长运行时间保护**。
> 前者是业务流程编排，后者是安全兜底；两者必须同时存在。

### 11.5 Workflow 业务流程（出粮）（**R6 修订新增**）

> 依据：`workflow.h:58` `enum WorkflowStepType` 仅有 `WORKFLOW_STEP_ACTION` 与
> `WORKFLOW_STEP_TRIGGER` —— **没有 delay/timer step**
> ⇒ **定时等待必须用 Timer Trigger 表达**，不能当作 Action Step。

| # | 步骤 | 类型 | 说明 |
|---|---|---|---|
| 1 | 接收触发（人工命令 / 定时触发） | Trigger | 现有机制 |
| 2 | 启动**储粮舱**振动马达 | **Action** `VIBRATION_START(motor=1, duration_ms=…)` | 粉末下落 |
| 3 | 等待预设时间 | **Timer Trigger** | 不是 Action；Workflow 原生定时等待 |
| 4 | 启动步进 120° 出粮旋转 | **Action** `STEPPER_DISPENSE_ROTATE` | 与振动**并行** |
| 5 | 旋转期间振动继续运行 | （无新步骤） | 由步骤 2 的 `duration_ms` 覆盖该时段 |
| 6 | 旋转成功后继续等待预设时间 | **Timer Trigger** | 坡道粉末继续下落 |
| 7 | 停止本次使用的振动输出 | **Action** `VIBRATION_STOP(motor=…)` | 可停单路或双路 |
| 8 | 全部成功 ⇒ Workflow 成功 | — | 引擎既有语义 |

**并发要求（任务书 §7.1）**：

- `VIBRATION_START` 的 Action 实例与 `STEPPER_DISPENSE_ROTATE` 的 Action 实例
  在 Workflow 引擎中**各自独立**，可同时处于 RUNNING；
- 两模块的 `*_task()` 均在 loop 中非阻塞推进，**不存在**「步进等待振动完全停止」的耦合；
- 硬件层：步进脉冲由**硬件外设**产生（§10.3），振动由 GPIO + `millis()` 判定，
  二者不共享任何阻塞资源。

**失败处理（任务书 §7.2）**：

- 任一步骤失败 ⇒ Workflow 不得继续执行依赖其成功的后续业务步骤（引擎既有语义）；
- 失败原因通过既有 Action 结果与日志上报；**并发拒绝与执行失败必须可区分**（§9.3）；
- **不得假定**步骤 7（振动停止）一定会被执行；
- ⇒ 振动模块**必须**依靠自身 30 s 超时关闭输出（§11.2），这是 Workflow 中断时的兜底；
- 优先使用 Workflow 引擎**既有**的取消/失败清理机制，**不自行发明**全局清理系统。

---

## 12. 初始化、禁用、错误与重启时的安全行为

### 12.1 GPIO 安全默认值与上电顺序（关键）

DRV8825 的 **EN 为低电平使能**；ESP32 GPIO 配置为 OUTPUT 后的默认电平不可依赖。
⇒ **必须**：

1. `gpio_set_level(EN, 禁用电平)` **先于** `gpio_config()` 设置输出
   （规避 §2.7 的 V-2 窗口问题）；
2. STEP 初始 LOW；DIR 初始为配置方向；
3. 继电器 IN 初始为**非触发电平**。

> 【推断】实现顺序建议：`gpio_set_level(pin, safe)` → `gpio_config()` → 再写一次 `safe`
> （双保险）。此顺序优于 valve 现有实现，本阶段**不改 valve**。

### 12.2 禁用 / 初始化失败时的行为

| 场景 | 行为 |
|---|---|
| `enable=false` | 不配置 GPIO（或配置为安全态后不再操作）；**仍注册 Action**（规避 V-1） |
| `pin < 0` | 同上 |
| Action 被调用但模块禁用/未初始化 | `ACTION_FAILED` + `*_ERROR` 事件 + `LAST_RESULT=rejected`，**零 GPIO 动作** |
| Workflow 中调用被禁用的 Action | Step 返回 FAILED ⇒ Workflow 按既有失败处理（沿用 `ACTION_FAILED` 语义，不改 WorkflowManager） |

> 【推断】「仍注册 Action」是对 valve 的改进：保证 registry 条目数恒定，
> 使云端 `v/k` 不随设备配置漂移。需在代码注释写明理由。

### 12.3 重启 / 崩溃

- 每次 boot：`*_init()` 都会把 GPIO 重置为安全态 ⇒ 重启后不会意外启动 ✓
- 若使用 RMT/定时器：`*_init()` 中必须先 `stop` 再配置，避免残留；
- 掉电/复位瞬间电机不会自转（STEP 无脉冲）✓

### 12.4 错误与超时

| 错误 | 处理 | 与并发拒绝的区分 |
|---|---|---|
| 参数缺失/非法 | 拒绝；`ACTION_FAILED` + 日志记录 | `LAST_RESULT=rejected`（原因：param） |
| 模块未启用 / 未初始化 | 拒绝；零 GPIO 动作 | `LAST_RESULT=rejected`（原因：disabled） |
| **并发冲突** | 拒绝；**不改变**已在运行动作的任何状态 | `LAST_RESULT=rejected`（原因：busy） |
| **动作执行失败** | 停止输出；`ACTION_FAILED` | `LAST_RESULT=failed`（原因：hw/exec） |
| **动作超时** | 强制停止输出；`ACTION_FAILED` | `LAST_RESULT=timeout` |
| 外设初始化失败（定时器/RMT 创建失败等，**仅限软件可检测项**） | 不开始动作；`ACTION_FAILED` | `LAST_RESULT=failed`（原因：init） |
| 队列满 | 遵循既有「拒绝并告警」机制（不新增策略） | — |

> **并发拒绝 与 执行失败必须可区分**（任务书 §8.4）：二者返回不同的
> `LAST_RESULT` 枚举值，并在日志/事件的 `data` 中写明具体原因。
> 软件**只报告自己能验证的控制结果**，不虚构机械动作成功或设备存在。

### 12.5 单次动作 30 秒上限（**R4 修订**，任务书 §5.1）

| 项 | 值 |
|---|---|
| 适用范围 | **统一 30 s**：120° 出粮旋转 **和** 指定脉冲数旋转 |
| 预计耗时（**不是**超时值） | 120° 出粮 @100 Hz ≈ **5.33 s**（533 脉冲）；不含初始化与前后等待 |
| 执行层级 | **模块层**（`stepper_task()`，loop 内非阻塞检测） |
| 代码硬限制 | `STEP_HARD_MAX_ACTION_MS = 30000`；配置 `safety_timeout_ms` 读取后**钳位**到 ≤ 30000 |

**为什么必须由模块层执行（关键依据）**【代码】：

- `command_manager.cpp:84` `COMMAND_ACTION_TIMEOUT_MS = 600000UL`
  ⇒ 临时 Action 的**引擎默认超时是 10 分钟**，远大于 30 s，**不能**替代模块限制；
- 任务书 §8.3：最大动作时长受**代码层硬限制**约束，不能只依赖配置值。

超时后的处理顺序（**防重复完成**）：

1. **停止 STEP 脉冲输出**（停定时器/RMT，先停硬件）
2. 置 `action_active = false`
3. 由**唯一**的一次性提交函数 `stepper_finish(RESULT_TIMEOUT)` 提交
   `ACTION_FAILED` + `LAST_RESULT=timeout` + 事件/日志
4. 释放 critical operation（见 §12.6）

**防重复机制**：正常完成、失败、超时**三条路径都调用同一个** `stepper_finish()`；
该函数**首行检查** `if (!action_active) return;` ⇒ 只有第一个到达者能提交结果，
杜绝重复完成、重复释放资源、重复通知 Workflow。

### 12.6 critical operation（**R5 修订**，任务书 §5.2）

**正式接口**【代码】：`src/services/system_command.h`

```c
bool system_command_critical_operation_acquire();   // :130
bool system_command_critical_operation_release();   // :139
```

- 现有用法：临时 Action 入队前由引擎 acquire（`workflow.cpp:4009`），
  并由引擎在 Action 结束时 release（`workflow.cpp:1969/2069`）；
- **本模块职责**（不重新设计系统级机制）：
  在**真正驱动硬件的脉冲输出期间**登记；在动作结束时释放。
- **三条路径都必须正确释放**：正常完成 / 执行失败 / 超时
  ⇒ 统一放在 `stepper_finish()` 内（该函数只会执行一次，见 §12.5）
  ⇒ 保证不漏释放、不重复释放。
- **不得**复制一套系统级管理或全局释放策略（任务书 §5.2 明令）。

### 12.7 定时器回调与轮询任务的竞争（**R5 修订**，任务书 §5.3）

即使整体是单核轮询模型，硬件定时器回调（esp_timer 任务 / ISR / RMT 完成中断）
与 `stepper_task()`（loop 上下文）仍可能在不同执行上下文交互。

**设计约定**：

1. **回调只置标志，不做复杂处理**：
   脉冲完成由回调置 `volatile bool step_done = true`，**不**在回调内释放上下文、
   **不**调用 `Serial`、**不**提交 Action 结果；
2. **状态提交与资源释放统一在 `stepper_task()`（loop 上下文）执行**；
3. **顺序保证**（防止回调访问已释放资源）：
   ```
   停止定时器/RMT  →  （此后回调不再进入）  →  置 done 标志
                    →  stepper_task() 提交结果  →  释放上下文 / 资源
   ```
   ⇒ 释放在「硬件已停 + loop 侧」发生，回调不可能访问已释放内存；
4. `step_done` 与 `action_active` 声明为 `volatile`；
5. 完成状态只提交一次（§12.5 的 `stepper_finish()` 幂等保护）；
6. **不要求**为开环电机实现失步检测（任务书 §5.3）。

---

## 13. 测试计划

### 13.1 现状【代码】

- `test/` 为 PlatformIO Test Runner 目录，但 `platformio.ini` **只有**
  `[env:esp32-s3-devkitc-1]`（**无 native 环境**）；
- 现有 host 测试以自定义脚本驱动：`test/device_model_host/{run.sh,run_probe.js,dm_probe.cpp}`、
  `test/loop_stats_host/`、`test/homie_bridge_host/`。

### 13.2 单元测试（不依赖硬件）

| 项 | 方法 |
|---|---|
| 参数校验（steps/duration 边界、非法值） | 抽纯函数，host 脚本测试（参照 `device_model_host` 模式） |
| 步数 ↔ 时间换算 | 同上 |
| State/Event 枚举新增后的映射一致性 | 断言 `state_map` 项数 == 枚举项数；`SYSTEM_EVENT_COUNT` 与枚举末项一致 |

### 13.3 集成测试（上板、不接电机/马达）

| 项 | 方法 |
|---|---|
| registry 查询 | `{"c":"registry","i":..,"k":0}` ⇒ 校验 `v`、`n`、新增 action 的 `k` |
| Action 调用（禁用态） | 发 `STEPPER_ROTATE` ⇒ 期望 `s!=0`（rejected），零 GPIO 动作 |
| Action 调用（启用态） | 发 `STEPPER_ROTATE steps=10` ⇒ 期望 `s=0`、`EVENT_STEPPER_START/DONE` |
| 串口命令 | `stepper_rotate 10`、`vibration_on 500` ⇒ 期望 `ret=1` + 状态可查 |
| 并发互斥 | 连续两次 `VIBRATION_RUN` ⇒ 第二次被拒 |

### 13.4 真机测试（**必须人工执行，不在本阶段**）

| 步骤 | 内容 | 安全前提 |
|---|---|---|
| H-1 | 12V 供电**断开**，仅逻辑侧；用万用表/LED 确认 EN/STEP/DIR 电平与安全默认 | 电机不动 |
| H-2 | 接 DRV8825 但**不接电机**，实测 STEP 脉冲频率/占空比（示波器或逻辑分析仪） | 无机械运动 |
| H-3 | 电位器调至**最小电流**，接电机空载试转 10 步 | 低电流 |
| H-4 | 逐步提高电流至能带动负载，记录最终电位器位置 | 每次 ≤5 s |
| H-5 | 振动马达：先不接 12V 验证继电器吸合声，再供 12V 测 3 s | 短时 |
| H-6 | 全程监测 OLED 阻塞（6.4 ms）是否影响脉冲 | 逻辑分析仪 |

> ⚠️ **不得**在未完成 H-1/H-2 的情况下直接带载试转。

### 13.5 最终真机验收清单（**R7 修订新增**，任务书 §10）

> 串口调试**不能替代**系统集成测试；最终验收**必须**通过现有 MQTT 命令链路下发
> （`{"c":"action","i":..,"v":<新版本>,"k":<stable id>,"p":{...}}`，见 §9.1）。

| # | 验收项 | 判据 |
|---|---|---|
| A1 | 120° 出粮 Action | MQTT 下发 `STEPPER_DISPENSE_ROTATE` ⇒ 成功返回，脉冲数 533 或 534 |
| A2 | 指定脉冲数 Action | 下发 `pulses=20` ⇒ **20 个有效 STEP 上升沿**（示波器/逻辑分析仪计数，非翻转数） |
| A3 | 脉冲数量、方向与完成报告 | 实测数量 == 报告值；DIR 电平符合 `dir_invert` 配置；完成事件/状态一致 |
| A4 | 多次出粮的量化误差分配 | 连续执行 ≥6 次 ⇒ 脉冲序列为 533/533/534 循环；累计误差 ≤1 脉冲 |
| A5 | 两个振动马达**独立**启停 | 停 A 不影响 B；启 B 不影响 A |
| A6 | 步进与振动**并行** | 振动运行期间下发 120° 旋转 ⇒ 二者同时工作，互不阻塞 |
| A7 | 30 s 超时保护 | 构造超时场景（如超大 `pulses` 或强制阻塞）⇒ 30 s 后停止输出并返回失败 |
| A8 | Workflow 正常完成与中途失败 | 正常流程成功；中途失败 ⇒ 后续依赖步骤不执行；振动仍由自身超时停止 |
| A9 | 资源与状态一致性 | 超时/失败/正常完成三条路径下：critical op 已释放、定时器已停、State 与事件一致、无重复完成 |

- 以上均**不在本规划阶段执行**；A2/A3/A4 需要示波器或逻辑分析仪配合。
- `NEEDS_CONFIRMATION`：A7 的超时场景构造方式（是否需临时放宽 `max_steps_per_action`）。

---

## 14. 风险清单与 `NEEDS_CONFIRMATION`

### 14.1 硬件（`NEEDS_CONFIRMATION`，不得臆测）

| ID | 事项 | 需要的证据 |
|---|---|---|
| **NC-H1** | DRV8825 模块**无独立 VDD 引脚**时的逻辑供电实现方式 | 模块原理图 / 万用表测量；不得套用标准载板 VDD 方案 |
| **NC-H2** | 电机标称「5V 2A」的准确性；12V VMOT 驱动该电机的适配性 | 电机铭牌/商家规格 + 实测相电阻；若确为 5V 电机，12V 供电需靠驱动器限流 |
| **NC-H3** | SLP / RST 的**有效逻辑电平**与所需接法 | 模块资料或实测 |
| **NC-H4** | M0/M1/M2 固定 1/8 细分的具体接线（高低电平组合） | DRV8825 数据表（M0/M1/M2 编码表）+ 实测 |
| **NC-H5** | 振动继电器**触发电平**（高/低有效） | 实测：先用串口 `vibration_on` 听吸合声 |
| **NC-H6** | 续流二极管型号（依实际电流与元件资料） | 马达实测电流 + 二极管规格书 |
| **NC-H7** | 推荐的 GPIO 13/14/15/16 在实物板上确实可用 | 板级原理图 + 上电实测默认态 |
| **NC-H8** | FLT 是否预留 GPIO | 未来故障监测需求（当前建议不占 GPIO） |
| **NC-H9** | 12V 电源容量是否满足电机 2A 峰值 + 继电器 + 电磁阀 | 电源规格 + 实测压降 |

### 14.2 软件风险

| ID | 风险 | 缓解 |
|---|---|---|
| **R-1** | 新增 Action ⇒ `stable_id` 重排 + `version` 变更（§8.4） | 发布前重查 registry 并同步云端/APP |
| **R-2** | `CONFIG_MODULE_COUNT` 8→10 遗漏导致新模块不参与备份/恢复 | §5.1 的 C-2/C-3 必须同步；增加自检 |
| **R-3** | `SYSTEM_EVENT_COUNT` 未更新 ⇒ 事件遍历越界/漏处理 | §7.3 E-2 |
| **R-4** | `state_map` 与枚举不一致 | §6.2 S-2 |
| **R-5** | 步进 Action 阻塞 Critical Operation（无法重启） | 超时 ≤10 s + 强制停止（§10.5） |
| **R-6** | OLED 6.4 ms 阻塞（若用软件定时） | 采用硬件外设（§10.2/10.4） |
| **R-7** | EN 低使能 + GPIO 默认低 ⇒ 上电即使能（发热） | §12.1 先写安全电平再配置输出 |
| **R-8** | 电机堵转/失步无反馈（无编码器） | 仅靠超时；建议 `NEEDS_CONFIRMATION`：是否需要堵转检测（电流采样） |
| **R-9** | `event_manager.cpp` 字符串表是否需同步（§7.3） | 实施阶段先读实现确认 |

---

## 15. 分阶段实施顺序与验收标准

> 每阶段都可独立验证；**前置不满足不得进入下一阶段**。

| 阶段 | 目标 | 主要修改 | 前置 | 验收标准 |
|---|---|---|---|---|
| **P0** | 硬件基线确认 | 无（只出结论） | 用户完成 NC-H1~H9 | 全部 `NEEDS_CONFIRMATION` 有证据；GPIO 候选定稿 |
| **P1** | Config 骨架 | C-1~C-4（§5.1）+ Getter | P0 | `config_query module=stepper/vibration` 返回预期默认值（`enable=false`） |
| **P2** | System State 骨架 | S-1、S-2（§6.2） | P1 | 新 state 可通过 `system.*` 查询；`state_map` 项数与枚举一致 |
| **P3** | Event 骨架 | E-1、E-2（+E-3 若需要） | P2 | 事件可被 push 且不越界；`SYSTEM_EVENT_COUNT` 正确 |
| **P4** | vibration 模块（较简单，先做） | `vibration.{h,cpp}` + init/task 挂载 + 3 个 Action | P1-P3 | 禁用态调用被拒；启用态 `VIBRATION_RUN duration_ms=500` 返回成功，500 ms 后自动停止 |
| **P5** | stepper 模块（GPIO + 使能/方向） | `stepper.{h,cpp}` 骨架、`STEPPER_STOP/ENABLE` | P4 | EN/DIR 电平正确；安全默认态经万用表确认 |
| **P6** | stepper 脉冲（esp_timer） | 脉冲引擎 + `STEPPER_ROTATE` | P5 + H-2 | 示波器/逻辑分析仪测得 STEP 频率 = 配置值 ±5%；步数准确 |
| **P7** | 安全与异常 | 超时、强制停止、互斥、阻塞检测 | P6 | 超时强制停；并发被拒；执行中发起重启能完成 |
| **P8** | 真机 | 无代码改动 | P7 + H-1~H6 | 电机带动负载转动（≤5 s/次）；马达按定时启停 |
| **P9** | 云端同步 | 更新 registry `v/k` 到云端/APP | P8 | 云端用新 `v/k` 能成功触发两个模块 |

**失败处理**：任一步骤验收不通过 ⇒ 回退到上一已验收阶段；
若涉及 GPIO 电平异常，立即断电并检查接线，不得反复上电试探。

---

## 16. 代码与文档依据索引

| 主题 | 路径 | 关键符号 |
|---|---|---|
| 参考模块 | `src/app/valve.cpp` / `.h` | `valve_init` `valve_open` `valve_set_gpio` `valve_task` `valve_force_close` `valve_update_state` |
| 异步 Action 范例 | `src/app/computer_reset.cpp` | `computer_reset_action_reset/start/poll`、`ACTION_RUNNING`、`ComputerResetCtx` 池、`COMPUTER_RESET_HOLD_MS` |
| Action 描述符/结果 | `src/automation/workflow.h` | `WorkflowActionDescriptor`、`WorkflowParam`、`WorkflowParamValue`、`ACTION_IDLE/RUNNING/SUCCESS/FAILED`、`WORKFLOW_MAX_PARAM=8` |
| Action 注册/入队 | `src/automation/workflow.cpp` | `workflow_register_action`(:969)、`workflow_enqueue_action`(:4123)、`enqueue_temp_action`(:3972)、`workflow_parse_params`(:3308)、Critical Operation acquire(:4009) |
| Capability / Stable ID | `src/automation/capability_registry.h` | `stable_id`(:123)、排序规则注释(:67)、`capability_get_action_version`(:172)、`capability_get_action_by_stable_id`(:187) |
| System State | `src/services/system_state.h` / `.cpp` | `enum SystemStateKey`(:20)、新增规则注释(:62)、`state_map[]`(:14)、`STATE_VALVE_STATUS` 行(:25)、`state_set_bool`(:101) |
| Event | `src/services/event_manager.h` | 枚举(:20-52)、分区注释(:38-44)、`SYSTEM_EVENT_COUNT`(:53)、`event_push`(:121)、`EVENT_POLICY_STATE`(:78) |
| Config | `src/services/config_manager.h` / `.cpp` | `CONFIG_MODULE_*`(:50-57)、`CONFIG_MODULE_COUNT=8`(:38)、`kModuleNames[]`(:70)、valve getter 范式(:3540-3562)、`pending_save_finalize`(:996) |
| 命令路由 | `src/services/command_manager.cpp` | `command_is_execute`(:629)、`execute_router`(:2209)、`command_execute_action`(:810)、`system_router`(:2436) |
| 报文转换 | `src/cloud/cloud_manager.cpp` | action 分支 `v/k` 校验(:1348-1370)、registry 分支(:1396)、system 分支(:1425) |
| 主题定义 | `src/services/topic_renderer.h` | `GF_TOPIC_TPL_DOWN="guo_feeder/<device_id>/down"`(:28)、`GF_TOPIC_TPL_UP`(:29) |
| 初始化/主循环 | `src/main.cpp` | 分层 init(:540-575)、loop 任务序(:689-742)、串口 valve 命令(:764-781) |
| OLED 阻塞实测 | `docs/review/G4-Window-Verdict-2026-10-21.md` | §3.1「OLED 同步 I2C 6.4 ms / 50 ms」 |
| 本轮 MQTT 语法 | `.workbuddy/memory/2026-10-22.md` | `{"c":"action","v":..,"k":..}`；registry 查询 `k=0` |
| 架构约定 | `AGENTS.md` / `AI_RULES.md` | 分层、非阻塞、包含路径 |

---

## 17. 本阶段合规性声明

- ✅ 仅只读调查（Read / Grep / Bash 查询类命令），**未修改任何 `.cpp` / `.h` /
  配置文件 / 构建文件 / 依赖清单 / 现有测试代码**；
- ✅ 仅新增本文件（`docs/architecture/Stepper-Vibration-Module-Implementation-Plan.md`）；
- ✅ 未执行编译、烧录、部署或真机动作；
- ✅ 未执行 `git commit`；
- ✅ 未把设计建议描述为已实现功能，未把未执行测试标记为 PASS；
- ✅ 所有无法从仓库确认的事项已标记 `NEEDS_CONFIRMATION`；
- ✅ 发现的 valve 架构问题（V-1/V-2/V-3）仅记录，未顺手修改。
