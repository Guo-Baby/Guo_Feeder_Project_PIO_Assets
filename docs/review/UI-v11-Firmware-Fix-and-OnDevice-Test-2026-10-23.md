# UI v1.1 固件修正 + 烧录 + 真机综合测试报告

> 基线：`e110b96`（旧）→ 本轮提交（见 §1）
> 云端契约参考：`Cloudflare_Assets @ 78c6bec`（**未部署**，见 §9）
> 测试日期：2026-10-23 21:55 ~ 22:40（本机时间）
> 设备：ESP32-S3 N16R8 / COM8（CH343）/ device_id `288485896ce4`

---

## 1. 仓库基线

| 项 | 值 |
|---|---|
| 仓库 | `PIO_Assets/Guo_Feeder_Project` |
| 分支 | `wb`（远端 `origin/main`） |
| 起始 HEAD | `f628921`（报告补提交） / 代码基线 `e110b96` |
| 本轮提交 | 见文末「提交记录」（`src` + `test` + 本报告） |
| 工作区 | 除 3 个历史遗留未跟踪文档外无其它改动 |

> 3 个遗留未跟踪文档（**非本轮产物，未触碰**）：
> `docs/architecture/Stepper-Vibration-Plan-Revision-Report.md`、
> `docs/evidence/LogAckSplit-2026-10-23/legacy-topic-rows-backup.json`、
> `docs/evidence/Stepper-NoRotation-Diagnosis-2026-10-22.md`

---

## 2. 本轮修改（共 8 个文件，+367 / −5 行）

### 2.1 急停锁存覆盖全部硬件启动入口（任务 §2.1）

**根因**：`e110b96` 的目录锁只在 `command_execute_workflow()` 一处生效 ⇒
**单发 Action 仍能在急停之后重新启动执行器**，且 Trigger 自动启动的 Workflow 也未拦截。

| 文件 | 改动 | 说明 |
|---|---|---|
| `src/app/safety.h` | 新增 `safety_action_blocked_by_latch()` 声明 + `SafetyReport.temp_cancelled` | — |
| `src/app/safety.cpp` | 新增**白名单**判定 + `stop_all` 增加「取消排队任务」步骤 | 白名单 7 项：`VALVE_CLOSE` / `STEPPER_EMERGENCY_STOP` / `TEST_STEPPER_STOP` / `VIBRATION_STOP` / `MI_THERMO_STOP_SCAN` / `MI_THERMO_START_SCAN` / `WEIGHT_ZERO` |
| `src/automation/workflow.h` | 新增 `workflow_cancel_pending_temp_actions()` | — |
| `src/automation/workflow.cpp` | ① `workflow_start()` 加锁存拦截 ② `enqueue_temp_action()` 加锁存拦截 ③ `workflow_action_start()` 兜底 ④ 实现 `workflow_cancel_pending_temp_actions()` | 见下 |
| `src/services/command_manager.cpp` | `command_execute_action()` 加锁存拦截（给出可读原因） | — |

**为什么用白名单而不是黑名单**：黑名单在新增 Action 时默认放行 ⇒ 新 Action 会静默绕过急停；
白名单的失效方向是「多拦一个」，最坏结果是用户多点一次 `safety.clear`。

**三个拦截点的分工（缺一不可）**

| 位置 | 覆盖的路径 |
|---|---|
| `command_execute_action()` | MQTT / 串口 `cm {}` 的**单发 Action**，并能回一条**可读原因**给云端 |
| `enqueue_temp_action()` | WorkflowManager 的**唯一 Action 入队口**（兜住所有其它调用方） |
| `workflow_start()` | **Trigger 自动启动**（定时/条件触发），这是只拦 CommandManager 会漏掉的路径 |
| `workflow_action_start()` | Workflow **step 之间**发生锁存时的兜底（置 `ACTION_FAILED` 走既有失败终止） |

**`stop_all` 的新顺序（不得调整）**：置 `latched` → 停所有非 IDLE 的 Workflow →
**取消排队中的临时 Action** → 停阀 → 停步进 → 停振动 → 停测试步进。
取消必须**早于**停硬件，否则队列里的下一项会在函数返回后又把阀打开。

`workflow_cancel_pending_temp_actions()` 只用既有 `temp_action_complete(item, ACTION_FAILED)` 收口，
因此：释放 Critical Operation ✔、回调 CommandManager 上报**失败终态** ✔、环形队列不变式不变 ✔、幂等 ✔。

### 2.2 强制重启的启动标记回读（任务 §2.2）

| 文件 | 改动 |
|---|---|
| `src/services/config_manager.h` | 新增 `config_boot_flag_present()` 声明（只读） |
| `src/services/config_manager.cpp` | 实现回读：`json_storage_exists("/config/.bootok")` |
| `src/services/command_manager.cpp` | `boot_flag_written = 写入链路成功 && 独立回读存在`；新增 `boot_flag_write_ok` / `boot_flag_present` / `critical_at_request` / `critical_bypassed` / `latched` / `config_integrity_guaranteed=false` / `temp_cancelled` |

**核查结论（代码层）**

- `config_boot_validate()` 的返回值来自 `json_storage_write_atomic()`：写 tmp → 校验长度 → rename，
  **成功才返回 true** ⇒ 原实现并非「无条件 true」。
- 但「写函数说成功」与「文件真的在命名空间里」是两件事 ⇒ 本轮补一次**独立回读**，
  两者都为真才对外声称 `boot_flag_written=true`。
- 语义边界写进响应：`boot_flag_written` 只表示**启动标记写入成功**，
  **不**表示配置内容完整；`config_integrity_guaranteed` 恒为 `false`。
- 仍然是 **不看 critical 计数、300 ms 后复位**（已批准的风险决策，未改）。

### 2.3 未改动

F-3（帧字段）、F-4（只读查询）、F-7（boot_seq）**本轮未改任何协议**；
`system.restart` 与 `system.reboot` 的 status 语义差异**保持原样**（属既有契约，见 §8）。

---

## 3. 构建与烧录

### 3.1 构建

| 项 | 值 |
|---|---|
| 命令 | `PLATFORMIO_BUILD_DIR=.pio/build/tel PLATFORMIO_BUILD_FLAGS="-DGF_TELEMETRY_ENABLED=1" pio run` |
| 环境 | `esp32-s3-devkitc-1` · platform `espressif32@7.0.1` · `framework-arduinoespressif32@3.20017.241212`（Arduino-ESP32 2.0.17 / IDF 4.4.7） |
| 关键编译开关 | `-DGF_TELEMETRY_ENABLED=1`（**真机必需**）· `-DARDUINOJSON_USE_CBOR=1` · `-DARDUINO_LOOP_STACK_SIZE=16384` · `-DGF_LOOP_STATS_ENABLED=1` |
| 结果 | **成功**（`error:` 计数 0；`firmware.bin` 生成） |
| Flash | `.flash.text` 1 062 847 + `.flash.rodata` 278 408 = **1 341 255 B**；镜像 1 450 144 B / 2 097 152 = **69.1%**（< 85% 护栏 ✅） |
| RAM | `.dram0.data` 24 568 + `.dram0.bss` 111 312 = **135 880 B** / 327 680 = **41.5%**（+48 B vs 上一版） |
| 镜像 SHA-256 | **`9264fe3b21e7119db3544c4f03643f002906a6ffb42ecad34343f0564a5345aa`** |
| 镜像大小 | **1 450 144 B** |

> 构建尾部出现 `[safe-delete][SAFE_DELETE_BULK_CONFIRM_REQUIRED]` + `SystemExit: 1`，
> 这是**沙箱的临时文件清理保护**，发生在 `firmware.bin` 生成**之后**，与编译结果无关
> （判据：`error:` = 0 且产物存在）。

### 3.2 烧录

| 项 | 值 |
|---|---|
| 命令 | `esptool.py --chip esp32s3 --port COM8 --baud 921600 --before default_reset --after no_reset write_flash -z 0x10000 <firmware.bin>` |
| 只写 app 分区 | ✅ 不擦片、**不 uploadfs**、不碰 `nvs`（device_id / 凭据）与 LittleFS（设备端配置） |
| 结果 | `Wrote 1450144 bytes (926880 compressed) at 0x00010000 in 15.9 seconds` · **`Hash of data verified.`** · `Staying in bootloader.` |
| 新固件确认 | ① 串口出现 `[Safety] outputs init done`（**旧固件没有这一行**）② `c="safety"` 命令可用（旧固件无此命令） |

### 3.3 启动（T1）

```
[CFG][I] init done, loaded 10/10 modules           3.13 s
[Safety] valve: pin=12 safe_level=0                3.14 s
[Safety] stepper_en: pin=15 safe_level=1           3.14 s
[Safety] vibration: pin=16 safe_level=0            3.14 s
[Safety] outputs init done (network not started yet) 3.15 s
[BLE] init OK                                      3.19 s
WiFi init...                                       3.19 s
[Valve] Init OK (pin=12, active_level=1, safety=60 s)   ← T6 改后
[CFG][I] boot validated                            5.96 s
MQTT connected                                     6.80 s
[Time] SNTP sync OK (2026-10-23 22:19:00)          9.11 s
```

- `reset reason=power_on`（首次上电），后续重启为 `software`
- 无 `Guru Meditation` / `Stack canary` / `task_wdt` / `LoadProhibited` / `StoreProhibited`
- `[Safety] outputs init done` 在 `WiFi init` **之前**（3.15 s vs 3.19 s）✅

---

## 4. 真机测试结果总表

| 组 | 内容 | PASS | FAIL | BLOCKED | NOT VERIFIED |
|---|---|---:|---:|---:|---:|
| T1 | 启动与稳定性 | 6 | 0 | 0 | 0 |
| T3 | 只读命令与协议 | 54 | 0 | 0 | 0 |
| T4 | 急停锁存（不驱动硬件） | 14 | 0 | 0 | 0 |
| T4-live | 运行中急停（真实驱动执行器） | 9 | 0 | 0 | 0 |
| T4q | 排队任务取消 | 6 | 0 | 0 | 0 |
| T5 | 强制重启 | 10 | 0 | 0 | 1 |
| T6 | 阀门 300→60 s（读/改/存/重启/回读） | 9 | 0 | 0 | 0 |
| T7 | 振动定时停止 / 步进 | 6 | 0 | 2 | 0 |
| T2 | GPIO 万用表电气测量 | 0 | 0 | 1 | 0 |
| **合计** | | **114** | **0** | **3** | **1** |

主机侧：`firmware_contract_check.py` **82 / 82 PASS**（源码形态核对，见 §5.4 的局限声明）。

### 4.1 T3 只读命令（54/54 PASS）

逐条实测（全部走真实 MQTT 下行 + 设备回帧）：

`query_state`（valve_status / weight_valid）、`query_actions`、`query_triggers`、`query_workflows`、
`query_capabilities`、`query_action_registry`、`query_trigger_registry`、`query_workflow_registry`、
`registry k=0/1/2`、`system.memory/flash/stats/loop/memwatch/restart_status/time`、`config_query`、
`workflow.sync_info/list/get`。

**负向用例（4 条，全部按预期拒绝）**

| 用例 | 设备回帧 |
|---|---|
| `query_state` 缺 `o` | `s=4` `"Missing 'object' (state key)"` |
| `registry` 缺 `k` | `s=4` `"invalid registry type"`（**不发 ack**，协议层直接拒绝） |
| 未知 `system` 对象 | `s=4` `"Unknown system object: no_such_object"` |
| `action` 版本不符 `v=99` | `s=4` `"action registry version mismatch"` |

**协议形态实测（与云端契约直接相关，见 §8.1）**

```jsonc
// ack（真实）
{"c":"ack","i":"wbt3-00","o":"valve_status","p":{"result":"received"}}
// result（真实 —— 注意 status 是 **整数 s**，不是字符串 status）
{"s":0,"c":"result","i":"wbt3-00","o":"valve_status","t":1792765338,"value":"false","command":"query_state"}
// 失败 result（真实）
{"s":4,"c":"result","i":"wbt3-22","m":"Missing 'id' (workflow slot)","e":3,"type":"command"}
```

`system.memwatch` 实测（可用作基线）：`dram.total=246812 free=59792 used_pct=75.77%`，
`loopTask stack 16384 B，used 6708 B (40.94%)，hwm 9676 B`。

### 4.2 T4 急停锁存（14/14 PASS）

| 用例 | 结果 |
|---|---|
| 空闲 `stop_all` | `s=0 all_ok=true latched=true`，逐执行器 `verified=false`（无电气回读，**未谎称已物理停止**） |
| 锁存中 `VALVE_OPEN` | **被拒** `"Emergency stop latch is active; action rejected: VALVE_OPEN (send safety.clear to unlock)"` |
| 锁存中 `STEPPER_ROTATE_STEPS` | **被拒**（同上文案） |
| 锁存中 `VIBRATION_START` | **被拒** |
| 锁存中 `TEST_STEPPER_START` | **被拒** |
| 锁存中 `VALVE_CLOSE` | **允许**（`s=1` queued） |
| 锁存中 Workflow `wf_valve_20g` | **被拒** `"system is latched by emergency stop; send safety.clear first"` |
| `safety.clear` | `s=0`，`was_latched=true` → `latched=false` |
| clear 后 4 个 Workflow | **全部 IDLE**（未自动恢复） |
| 解锁后显式动作 | 恢复受理 |

### 4.3 T4-live 运行中急停（9/9 PASS）

| 步骤 | 证据 |
|---|---|
| 振动 `duration_ms=0` 启动 | `s=1` |
| 1~2 s 后查询 | `vibration_active = "true"` |
| `stop_all` | `s=0`，`vibration: {ok:true, was_active:true, note:"stopped", verified:false}`，`all_ok=true` |
| 开阀 | `s=1` |
| 2 s 后查询 | `valve_status = "true"`（未被 DispenseGuard 抢先关掉） |
| `stop_all` | `valve: {ok:true, was_active:true, note:"force_closed"}` |
| 1.5 s 后查询 | `valve_status = "false"` |

⇒ **「正在输出的振动马达」与「正在开着的电磁阀」都被急停确实关掉**，且回读证实。
（仍未做电气测量，故 `verified` 恒为 false —— 见 T2。）

### 4.4 T4q 排队任务取消（6/6 PASS）

场景：振动 8 s 占住队头 → 再入队一条 `VALVE_CLOSE` → 立即 `stop_all`。

- `temp_cancelled = 2`
- 振动 `note="stopped"`（队头正在执行的也被停）
- 被取消的排队命令收到 `{"s":3,...,"o":"VALVE_CLOSE","type":"action_result"}` ⇒ **云端能拿到失败终态，不是静默消失**

### 4.5 T5 强制重启（10 PASS / 1 NOT VERIFIED）

串口取证（`.pio/t5_serial2.log`）：

```
20.68 [Safety] stop_all: wf_seen=4 wf_stopped=0 temp_cancelled=0 running=0
      valve=1(already_closed) stepper=0(no_action_running) vib=1(already_off)
      tstep=1(not_running) all_ok=1 21 ms
20.76 [CFG][I] boot validated                     ← 启动标记在复位前落地
20.78 [SYSCMD][W] FORCE restart requested (critical=0 ignored, prev=idle, delay=300 ms)
21.08 [SYSCMD][W] FORCE restart window elapsed (critical NOT drained), restarting now
21.23 rst:0xc (RTC_SW_CPU_RST),boot:0x8 (SPI_FAST_FLASH_BOOT)
25.07 [CFG][I] init done, loaded 10/10 modules
25.08 [Safety] outputs init done (network not started yet)   ← 新启动仍执行安全电平
25.56 [SYSCMD][I] init done, reset reason=software
27.87 [CFG][I] boot validated
      [Valve] Init OK (... safety=60 s)          ← 配置未回滚
      MQTT connected
```

- **未出现** `recovering (boot_ok=0, ...)` ⇒ 没有因启动标记缺失触发 CFG-ROLLBACK ✅
- 复位原因 `software`（不是看门狗、不是上电）✅
- **NOT VERIFIED**：`force_restart` 的 result 帧**没有收到**。原因诚实记录：
  设备在 300 ms 后就复位，QoS1 的出站队列来不及把 result 帧发完。
  ⇒ 云端会落到 `unknown`（这是**正确**的语义，不是谎报成功），但排障体验差，见 §8.4。

### 4.6 T6 阀门安全超时 300 → 60（9/9 PASS）

| 步骤 | 证据 |
|---|---|
| 读现值 | `300`（**设备实值与代码默认值无关** —— 印证 §2 的铁律） |
| `config_set 60` | `s=1 "config set queued, restart required"` |
| `config_save` | `s=1 "config save queued"` |
| 串口 | `config saved` + 安全重启流程出现 |
| 重启后启动日志 | `[Valve] Init OK (pin=12, active_level=1, safety=60 s)` ✅ |
| 重启后回读 | `safety_timeout_sec = 60` ✅ |
| 开阀 / 关阀 | 均受理；关阀后 `valve_status=false` ✅ |

**未验证（如实记）**：60 s **超时自动关阀**本身没有测。
原因：HX711 未接 ⇒ `weight.enable=true` 下 DispenseGuard 会随机 `valve_force_close()`，
无法把「60 s 超时」与「重量异常强关」隔离开；且 60 s 连续放水存在溢出风险。
⇒ 归类 `NOT VERIFIED`，需接回 HX711 或临时关闭 weight 模块后重测。

### 4.7 T7 振动与步进（6 PASS / 2 BLOCKED）

| 用例 | 结果 |
|---|---|
| 定时振动 `duration_ms=3000` | 1 s 后 `vibration_active=true`；5 s 后 `false` ⇒ **定时自动停止已验证** ✅ |
| 持续振动 + `VIBRATION_STOP` | 停止后 `vibration_active=false` ✅ |
| 步进电机带载/转动 | **BLOCKED** — DRV8825 已损坏、替换件与接线未确认 |
| `TEST_STEPPER` 启停 | **BLOCKED** — 与步进共用 STEP/EN，驱动未确认前不执行 |

> 这一项解决了一个长期缺口：**振动定时停止此前从未验证**，本轮实测通过。

---

## 5. 测试方法与局限

### 5.1 新增测试资产（`test/ui_v11_contract/`）

| 文件 | 作用 |
|---|---|
| `firmware_contract_check.py` | 82 条**源码形态**核对（F-1/F-2/F-3/F-4/F-5/F-6 + 本轮新增 F1b-01~09、F2b-01~04） |
| `boot_probe.py` | T1 启动观测（只读串口 + 关键行判定，支持 `--reset` 脉冲复位） |
| `scenario.py` | T3/T4/T4-live/T4q/T6-read/T7 的 MQTT 场景执行器（先查 registry 再构造 `v`+`k`） |
| `t5_force_restart.py` | T5 串口 + MQTT 同会话取证 |
| `t6_valve_timeout.py` | T6 串口 + MQTT 同会话取证（读→改→存→重启→回读） |

### 5.2 为什么测试脚本必须"先查 registry"

压缩下行里 `c="action"` **只认 `v`(registry 版本) + `k`(stable_id)**，不认 runtime 名称。
本轮实测 registry：`action v=4 n=14`、`workflow v=3 n=4`。
脚本一律先 `{"c":"registry","k":0/2}` 取回映射再构造命令 ⇒ 顺带把 F-4 也验了。

### 5.3 测试脚本自身的两个坑（已修，记录以备复现）

1. `config_query` 是**异步**的：第一帧 `s=1 accepted`，第二帧才带 `data` ⇒ 断言必须等两帧。
2. 打开 COM8 **不一定**复位板子 ⇒ 不能写「至少 2 次 boot」这类绝对断言，
   必须以串口里的 `rst:0x` 为锚点做**相对**判断。

### 5.4 局限声明（重要）

- `firmware_contract_check.py` **只核对源码形态**，不能证明设备行为。
  本轮真实的判别力来自 T3~T7 的**设备回帧与串口日志**。
- 所有 `verified` 字段在固件里**恒为 false**：固件没有执行器电气回读通道，
  `valve_is_open()` / `s_busy` / `s_output_on` 都只是**意图镜像**。
- **未做电气测量**（T2 BLOCKED）⇒ 「GPIO 高阻窗口内继电器是否误导通」**没有证据**。

---

## 6. RTC / HX711 缺失相关的日志分类

| 日志 | 归类 | 说明 |
|---|---|---|
| `[Time] RTC probe failed (addr=0x51 err=2), chip not present?` | **预期缺失** | 板上未接 PCF8563T ⇒ 每次启动一条 WARN，**属正常** |
| `[Time] RTC init failed (wait SNTP)` | **预期缺失** | 同上，时间由 SNTP 兜底（实测 `SNTP sync OK`） |
| `[Weight] Init OK (dt=6 sck=7 scale=741.0 zero_offset=-800750)` | **需注意** | 模块初始化成功，但 HX711 未接 |
| `query_state weight_valid` → `"true"` | **假阳性** | HX711 未接却报"有效" ⇒ 固件**无法区分"没装"与"断线"**（已知缺陷，未在本轮修） |
| `[DispenseGuard] Weight error received` → `[Valve] FORCE CLOSE` ×41~64 / 会话 | **预期缺失的副作用** | 重量读数抖动 ⇒ 每 5 s 一次安全响应；**这是 HX711 未接的直接后果**，不是本轮回归 |

> ⚠️ 副作用风险（本轮新增观测）：HX711 未接且 `weight.enable=true` 时，
> DispenseGuard 会**无差别**强关阀门（不判断阀是否开着）。实测 10 s 内 41~64 次。
> 这会让**手动开阀几乎无法保持**，也是 10-23 那次「一开就关」的机制性解释。
> 本轮**没有**为此改动任何代码（属既有行为 + 缺硬件），只在报告里登记。

---

## 7. 风险分析

### 7.1 急停

| 风险 | 现状 | 处置 |
|---|---|---|
| 白名单漏掉新 Action | 新增 Action 默认被**拒绝**（失败方向安全） | 新增 Action 时必须回来决定是否放行 |
| 串口调试命令（`tstep_start` / `wf_start`）绕过锁存 | **确实绕过**（它们不走 CommandManager） | 属**已知例外**，只能在现场纪律上约束；`cm {}` 路径已被覆盖 |
| 跨任务访问临时队列 | `stop_all` 在 esp-mqtt 任务执行，`workflow_task` 在 loop 任务 | **既有**并发形态（`workflow_stop` 同样如此），本轮未引入新锁；风险窗口 = 一次队列指针推进 |
| `verified=false` | 无法证明物理已停 | 由 T2 万用表补 |

### 7.2 强制重启

| 风险 | 现状 |
|---|---|
| 300 ms 内打断 Flash 写入 | **未消除**（已批准的风险决策）。本轮实测是空闲态，**没有**在写入中强制重启 |
| 启动标记未落 ⇒ CFG-ROLLBACK | 本轮实测**未发生**（`boot validated` 在复位前落地；重启后无 `recovering(boot_ok=0)`） |
| result 帧丢失 ⇒ 云端 `unknown` | **实测发生**（诚实语义，但可观测性差） |
| 连续强制重启磨损 Flash | 固件**无**频次限制；建议云端限流（云端已有 3 次/5 min 桶，**未部署**） |

### 7.3 GPIO 安全电平

- 代码顺序已验证（`config_init` → `safety_outputs_init` → BLE/WiFi），
  实测时间差 **40 ms**（3.15 s vs 3.19 s）。
- 复位到该函数执行之间仍有高阻窗口 ⇒ **只有万用表能证明**（T2 BLOCKED）。

### 7.4 阀门超时

- 默认值改为 60 只影响**配置缺失时**；设备实值原先 300，本轮已显式改为 60 并回读确认。
- 钳位下限从 1 提到 10（与 `config_manager.h` 注释对齐）。**若设备实值曾在 1~9 会被抬到 10**，本轮实值为 300→60，不受影响。
- 60 s 与 21 g 实测 45.4 s 的余量只有 **14.6 s** ⇒ 真机供水时需有人值守（本轮未做长时间供水）。

---

## 8. 跨仓契约偏差（**云端侧缺陷，本轮不修**）

> 以下全部由**真实设备回帧**证实，不是推测。因任务边界（不得改云端 / 不得部署）只登记，等裁决。

### 8.1 【P0】云端读不到 status ⇒ 所有命令都会落到 `unknown`

- 设备上行是**压缩格式**（`cloud_manager.cpp::cloud_compress_uplink`，所有 `/up` 消息都过）：
  `status` → **整数 `s`**（0=OK / 1=ACCEPTED / 2=RUNNING / 3=FAILED / 4=ERROR），`cmd`→`c`、`id`→`i`。
- 云端 `applyDeviceFrameToCommand()` 读 `msg.status || msg.st`，**两者都不存在**（实测帧里只有 `s`）。
- 后果：`stateFromFrame({cmd:"result", status:""})` → `default → unknown`
  ⇒ **每一次设备执行结果都会写成 `unknown`**，包括成功。

**建议最小修法**（云端，1 处）：`status` 取值改为 `msg.status ?? msg.st ?? mapS(msg.s)`，
其中 `0→done / 1→accepted / 2→running / 3|4→failed`。

> 附带：上一轮 M1 修的 `cmd || c` 对真实流量**其实用不上**（线上永远是 `c`），
> 但保留无害；真正的缺口是 `s`。

### 8.2 【P1】`workflow.get` 的参数键是 `id`，云端目录写的是 `o`

实测 `{"c":"workflow.get","p":{"o":3}}` → `s=4 "Missing 'id' (workflow slot)"`；
`{"c":"workflow.get","p":{"id":3}}` → `s=0`。
⇒ 云端 M8 能力目录里 `workflow.get` 的参数名需从 `o` 改为 `id`。

### 8.3 【P1】`config_query` 是两帧，云端需取第二帧

实测第一帧 `{"s":1,"m":"config query queued"}`，第二帧才带 `data`（含 `safety_timeout_sec`）。
云端若只取第一帧，配置值永远是空的。

### 8.4 【P2】`force_restart` 的 result 帧赶不上复位

实测未收到。两个可选的后续动作（**需裁决，本轮未做**）：
① 固件在 300 ms 窗口内加**有界** outbox 排空（≤150 ms）——会推迟复位，与「不等待」的原则有轻微冲突；
② 云端以「上线 + `restart_status`」判定，不依赖 result 帧。

### 8.5 【信息】错误码字段

设备 result 帧发的是 `e`（压缩自 `error_code`），云端读 `msg.e` ✅ 一致。
但 `command_send_error()` 的原始 key 是 `error_code`，**只有经过压缩才变成 `e`**；
若将来有非 `/up` 通道复用该函数，需要留意。

---

## 9. 部署状态声明

| 项 | 状态 |
|---|---|
| 固件烧录 | ✅ **已执行**（app 分区 `0x10000`，hash 已验证） |
| 设备配置修改 | ✅ **仅 1 项**：`valve.safety_timeout_sec` 300 → 60（T6 已授权范围），其余未动 |
| 设备命令 | ✅ 已发送（全部为本任务 T3~T7 授权的测试命令） |
| 云端 Worker 部署 | ❌ **未部署** |
| 云端 Pages 部署 | ❌ **未部署** |
| D1 迁移 `0008` | ❌ **未应用** |
| 线上配置 / 密钥 / ACL | ❌ **未修改** |
| 设备文件系统（uploadfs） | ❌ **未执行** |
| 设备当前状态 | 阀关闭 · 振动停止 · **锁存已解除** · `safety=60 s` |

### 端到端受限的项

云端仍是 `78c6bec` 之前的线上版本 ⇒ 以下**无法**在真机端到端验证：
`/api/ui/command` 的 202/409/503 语义、`command_audit` 落库、回帧关联、
限流分桶、CSRF/会话吊销。**本轮的全部命令都是直连 MQTT 下发的。**

---

## 10. 仍未完成 / 需硬件 / 需裁决

### 10.1 需要硬件

| # | 项 | 前置 |
|---|---|---|
| H-1 | **T2 GPIO 万用表电气测量**（继电器输入脚启动期电平、是否出现误导通脉冲） | 万用表 + 人手 |
| H-2 | 60 s 超时自动关阀 | 接回 HX711，或临时 `weight.enable=false`（**需授权**） |
| H-3 | 步进电机带载 / `TEST_STEPPER` | DRV8825 替换件 + 接线确认 |
| H-4 | 强制重启**打断 Flash 写入**场景 | 需构造写入窗口，风险高，本轮未做 |

### 10.2 需裁决

| # | 项 | 选项 |
|---|---|---|
| D-1 | 云端 `stateFromFrame` 读 `s`（§8.1） | 建议立即修（否则 UI 的命令状态永远 unknown） |
| D-2 | 云端 `workflow.get` 参数名 `o`→`id`（§8.2） | 按固件实测改目录 |
| D-3 | `force_restart` result 帧丢失（§8.4） | 固件有界排空 / 云端改用上线判定 |
| D-4 | `system.restart` 报 `success`、`system.reboot` 报 `rebooting` | 是否统一（属既有契约，未改） |
| D-5 | 串口调试命令绕过急停锁存 | 是否也纳入（当前**故意**不拦，便于现场排障） |
| D-6 | 白名单里放行 `MI_THERMO_START_SCAN` / `WEIGHT_ZERO` | 若认为"传感器类也必须冻结"，删掉即可 |

### 10.3 已知缺陷（沿用，本轮未修）

- HX711 不校验 `scale.read()` 返回值 ⇒ 未接线也可能 `weight_valid=true`（假阳性）。
- 无命令级 `boot_seq` ⇒ 云端无法精确识别"重启后是新实例"。
- 新模块（stepper / vibration / test_stepper）**无结构化日志码**，云端日志里看不到。

---

## 11. 回滚方式

| 回滚点 | 操作 |
|---|---|
| 固件 | `git revert <本轮提交>` 后重新构建烧录；或直接烧回上一版 `firmware.bin` |
| 设备配置 | `config_set valve safety_timeout_sec 300` + `config_save`（会重启） |
| 急停锁存 | 发 `{"c":"safety","p":{"o":"clear"}}`；或重启设备（锁存是 RAM 态，复位即清） |
| 云端 | 本轮**未改云端**，无需回滚 |

---

## 12. 提交记录

| 提交 | 内容 |
|---|---|
| （本轮） | `src` 修正（8 文件）+ `test/ui_v11_contract/*` 4 个脚本 + 本报告 + `docs/evidence/UI-v11-OnDevice-2026-10-23/*.json` |

未提交：3 个历史遗留未跟踪文档（与本轮无关）。

> ⚠️ 原始串口日志（`t5_serial2.log` / `t6_serial.log` / `t1_boot2.log` 的 `.log` 形态）
> 被 `.gitignore:39` 的 `docs/evidence/**/*serial*.log` 规则排除（项目既有约定），
> **只留在本地工作区**。报告里已逐字引用决定性片段（§3.3 / §4.5 / §4.6），
> 结构化结果（`*_result.json`）已入库。
