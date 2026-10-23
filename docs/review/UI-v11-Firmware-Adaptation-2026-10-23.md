# UI v1.1 固件适配 —— 实施与审查报告

> 日期：2026-10-23 · 仓库 `PIO_Assets/Guo_Feeder_Project` · 分支 `wb`
> 上游契约：`Cloudflare_Assets` @ `78c6bec`（`docs/UI-v1-Phase2-API-Contract.md` §12）
> **本阶段未烧录、未修改设备配置、未发送设备命令、未部署云端、未应用数据库迁移。**

---

## 1. 仓库基线

| 项 | 值 |
|---|---|
| 仓库 | `PIO_Assets/Guo_Feeder_Project` |
| 分支 | `wb`（推送目标 `origin/main`） |
| 起始 HEAD | `4771330` |
| 工作区（起始） | 3 个未跟踪文档（`docs/architecture/Stepper-Vibration-Plan-Revision-Report.md`、`docs/evidence/LogAckSplit-2026-10-23/legacy-topic-rows-backup.json`、`docs/evidence/Stepper-NoRotation-Diagnosis-2026-10-22.md`）—— 均为前几轮遗留，本轮**未触碰** |
| 结束 HEAD | 见 §8 |

---

## 2. 逐项修改

### F-1 · 紧急停止 `c="safety"`

| 文件 | 改动 | 根因 / 设计依据 |
|---|---|---|
| `src/app/safety.h`（新） | `SafetyReport` / `SafetyActuatorResult` / `safety_stop_all()` / `safety_clear_latch()` / `safety_is_latched()` | 需要一个知道各执行器物理停止 API 的**编排层** |
| `src/app/safety.cpp`（新） | 三层动作：`workflow_stop`（账本）→ `valve_force_close` / `stepper_emergency_stop` / `vibration_stop` / `test_stepper_stop`（物理）→ 汇总 | `workflow_stop()` 只清状态**不碰硬件**；`WorkflowActionDescriptor` 只有 reset/start/poll，**没有 stop** |
| `src/services/command_manager.cpp` | `command_is_safety()` + `safety_router()` + `command_safety_stop_all()` / `command_safety_clear()`；一级路由插入 | 独立一级命令，**不注册为 Action**（注册会重排 stable_id 并递增 registry 版本 ⇒ 云端缓存的 `k` 全失效） |
| 同上 | `command_execute_workflow()` 开头加 `safety_is_latched()` 拦截 | 急停后不允许 Workflow 自行重启；**不拦单发 Action**（人工操作，拦了会卡死） |

**报文**：`{"c":"safety","i":"<cmd_id>","p":{"o":"stop_all"|"clear"}}`
（`cloud_manager.cpp` 的透传分支已覆盖，**未改** cloud_manager）

**响应**（沿用既有 result 帧机制，由 CommandManager 统一上报）：

```json
{"cmd":"result","id":"<cmd_id>","type":"command","status":"success|error",
 "object":"stop_all","all_ok":true,
 "workflows_seen":3,"workflows_stopped":1,"any_was_running":true,
 "actuators":{
   "valve":{"attempted":true,"ok":true,"verified":false,"was_active":true,"note":"force_closed"},
   "stepper":{"attempted":true,"ok":false,"verified":false,"was_active":false,"note":"no_action_running"},
   "vibration":{"attempted":true,"ok":true,"verified":false,"was_active":false,"note":"already_off"},
   "test_stepper":{"attempted":true,"ok":true,"verified":false,"was_active":false,"note":"not_running"}},
 "latched":true,"elapsed_ms":12,
 "physical_verified":false,
 "hint":"固件无执行器电气回读：本结果只证明停止动作已执行且未报错，不证明阀门/电机物理上已停止。"}
```

**三层语义的区分（对照云端十态）**：

| 层 | 证据 | 云端态 |
|---|---|---|
| 命令已接收 | CommandManager 解析成功 | 无（命令帧不回 ack，见 §4.3） |
| 停止编排已处理 | `status=success/error` + `actuators[]` | `done` / `failed` |
| 硬件状态已验证 | **恒 false**（`physical_verified`） | — |

★ `status` 只用 `success`/`error`：云端 M2 只认这两值（+failed/timeout/rebooting），
自造 `partial` 会被映射成 `unknown`，反而更不诚实。逐项细节在 `actuators` 里。

### F-2 · 强制重启 `system.force_restart`

| 文件 | 改动 |
|---|---|
| `src/services/system_command.h` | 枚举加 `RESTART_FORCE_PENDING`（**加在末尾**，不动既有值）；`SYSTEM_FORCE_RESTART_DELAY_MS 300UL`；`system_command_force_restart()` / `..._pending()` |
| `src/services/system_command.cpp` | 状态机加 `RESTART_FORCE_PENDING` 分支（300 ms → `ESP.restart()`，**不看 critical 计数**）；`name` 映射加 `force_pending` |
| `src/services/command_manager.cpp` | `system_router` 加 `object == "force_restart"` → `command_system_force_restart()` |

**与普通重启的差异**：

| 项 | 普通重启 | 强制重启 |
|---|---|---|
| Critical Operation | 等归零（无限等） | **不等** |
| 倒计时 | 10 000 ms | 300 ms |
| 前置硬件停堆 | 无 | `safety_stop_all()`（尽力，失败也继续） |
| 前置 boot flag | 无 | `config_boot_validate()` |
| 状态名 | `requested`/`pending`/`restarting` | `force_pending` |

**为什么必须先落 boot flag**：强制重启跳过正常启动确认流程，若 flag 未落，
下次启动 `last_boot_ok=false` ⇒ ConfigManager 用 Backup 覆盖 Active（**CFG-ROLLBACK**，10-22 已实际踩过）。
`boot_flag_written=false` 会如实上报，不假装成功。

★ **不保证配置完整**：300 ms 内可能打断 Flash 写入；Active/Backup 校验和是唯一兜底。
响应里带固定 `warning` 文案，不得被 UI 省略。

### F-3 · 命令与结果帧兼容（核对，**未改任何字段**）

| 契约点 | 固件实现 | 位置 |
|---|---|---|
| ACK `c` / `i` | `ack["c"]="ack"`、`ack["i"]=cmd_id` | `cloud_manager.cpp:1240 附近 cloud_send_ack()` |
| result `cmd` / `id` | `doc["cmd"]="result"`、`doc["id"]=…cmd_id` | `command_manager.cpp`（7 处） |
| `status="success"` | ✓ | 同上 |
| `status="error"` | ✓（`command_send_error` 统一出口） | `command_manager.cpp:179` 等 |
| 重复命令 | `cmd_id` FIFO 10 / TTL 30 s 静默丢弃 | `cloud_manager.cpp` |
| 乱序 ACK/result | 固件不保证顺序；云端 M1/M6 已按"迟到回帧优先、终态不可被中间态覆盖"处理 | — |

**已知语义偏差（未修改，需裁决）**：
`system.restart` 返回 `status:"success"`（含义是"**受理**成功"），
而 `system.reboot` 返回 `status:"rebooting"`。云端把 `success` 映射为 `done`（终态）。
⇒ UI 会显示"已完成"，而设备随后会断连重启。

不修改的理由：属于**既有契约**，改动会影响所有现有调用方；且未在本次授权范围内。
建议（下一阶段）：统一为 `rebooting`，或在云端把 `system.restart` 的 `success` 特殊处理为 `accepted`。

### F-4 · 只读查询（核对 22 条，**未改固件**）

| 云端 id | 固件分派 | 参数 | 结论 |
|---|---|---|---|
| `query_state` | `command_manager.cpp` `query_router` → `command_query_state` | `p.o` 必填 | ✓ |
| `query_actions` / `query_triggers` / `query_workflows` / `query_capabilities` | 同上 | 无 | ✓ |
| `query_action_registry` / `query_trigger_registry` / `query_workflow_registry` | 同上 | 无 | ✓ |
| `registry_action` / `registry_trigger` / `registry_workflow` | `cloud_manager.cpp` `REGISTRY_ACTION/TRIGGER/WORKFLOW` | 顶层 `k`=0/1/2 | ✓（云端伪 id，组帧已映射为 `{"c":"registry","k":n}`） |
| `system.memory` / `flash` / `stats` / `loop` / `memwatch` / `restart_status` / `time` | `system_router` 对应 object | 无 | ✓ |
| `system.config_query` | `system_router` → `command_config_query` | `module`/`key`/`keys_only`（全可选） | ✓ |
| `workflow.sync_info` / `workflow.list` / `workflow.get` | `workflow_router` | `sync_info`/`list` 无；`get` 需 `p.o`=slot | ✓ |

**只读性核对**：对 10 个只读 handler 做了函数体静态扫描，
确认不含 `valve_open/valve_close/stepper_start/stepper_emergency_stop/vibration_start/config_save/config_set/ESP.restart`
（脚本 `F4-pure` 共 10 条断言，全部 PASS）。

★ `system.config_query` 会**占 CommandRuntime 槽位**并发异步（回两条帧），
这是既有实现，不是副作用；但它是 22 条里唯一"会写 runtime 表"的只读命令，已在报告标注。

### F-5 · 阀门安全超时默认值

| 文件 | 改动 |
|---|---|
| `src/services/config_manager.cpp` | `["safety_timeout_sec"] | 300` → `| 60` |
| `src/app/valve.cpp` | 钳位下限 `1` → `10`（与 `config_manager.h` 注释"合法值 10~300"对齐） |

**关键边界**：`| 60` 只在**键缺失**时生效。设备当前实值是 300（`data/config/valve.json` 被 gitignore）
⇒ **改代码不会改变设备行为**，必须真机阶段显式 `config_set` + `config_save`。

`valve_task()` 的超时检测逻辑未改（`now - open_start_time >= safety_timeout_ms`），
所以正常关阀、超时关阀、任务取消、急停、重启期间的状态不受影响。

### F-6 · 启动 GPIO 安全初始化

| 文件 | 改动 |
|---|---|
| `src/app/safety.cpp` | `safety_outputs_init()`：三个引脚 `gpio_config(OUTPUT)` → `gpio_set_level(安全电平)` |
| `src/main.cpp` | 在 `config_init()` 之后、`ble_init()`/`wifi_init()` **之前**调用 |

安全电平 = 有效电平取反（全部来自 config，**不硬编码**）：
阀 `active_level=1` ⇒ 安全 LOW；步进 `en_active_level=0` ⇒ 失能 HIGH；振动 `active_level=1` ⇒ 安全 LOW。

**为什么位置是硬约束**：复位后 GPIO 默认输入/高阻，继电器输入若浮空可能被拉向导通侧；
放到 WiFi/BLE 之后会留下 200~1500 ms 未定义窗口。

★ **只降低风险，不消除风险**：从复位到本函数执行仍有极短高阻窗口，
且继电器输入级是否有下拉取决于实际电路 ⇒ 必须万用表实测（见 §7）。

### F-7 · 启动序号与重启状态（核实，**未新增**）

| 项 | 现状 |
|---|---|
| `restart_status` 命令 | ✓ 存在，返回 `state` / `critical_operations` / `restart_pending` / `remain_ms` / `safe_delay_ms` |
| `force_pending` 状态值 | 本轮**新增的枚举值**，`restart_status` 会返回字符串 `"force_pending"`（值扩展，非新协议） |
| 命令级 `boot_seq` | **不存在**（`query_state` 里没有 `STATE_*_BOOT`） |
| 日志级 `boot_seq` | ✓ 存在：`log_ack` 的 `p.b`（`cloud_manager.cpp:1595`）、`log_history.boot_seq` 列 |
| `homie` 的 `s_boot_seq` | ✗ **不能当启动计数**：`homie_bridge.cpp:610` 用 `esp_random()` 生成，是随机值 |

⇒ **未新增 `boot_seq` 状态键**：用户明确要求"不得为 UI 增加未经批准的新事件类型、状态体系"。
云端若需识别"重启后新实例"，目前只能靠**日志通道的 boot_seq 变化**间接推断，
不能从命令链路直接读。这条已作为阻塞项写进报告。

---

## 3. 测试

| 测试 | 类型 | 结果 |
|---|---|---|
| `pio run` | 构建 | 见 §4 |
| `test/ui_v11_contract/firmware_contract_check.py`（新） | 源码级契约核对（69 条断言） | **69 / 69 PASS** |
| 既有 host 测试（`test/stepper_quantization_host` 等） | 未在本次变更路径上 | 未执行（与本轮改动无关） |

`firmware_contract_check.py` 覆盖：
- F-3 帧字段 6 条（`c`/`i`/`cmd`/`id`/status 五值/type）
- F-1 安全命令 11 条（分派、四个执行器调用、`physical_verified=false`、latched 顺序）
- F-2 强制重启 7 条（状态、300 ms、不查 critical、boot flag、`ESP.restart()` 次数）
- F-4 只读命令 33 条（22 条分派 + k 参数 + query_state 参数 + 10 条纯函数检查）
- F-5 阀门 5 条
- F-6 GPIO 5 条（含调用顺序的行号比较）

**明确不是**真机验证：这些测试只核对源码形态，不能证明设备行为。

---

## 4. 编译结果

| 项 | 值 |
|---|---|
| 命令 | `pio run`（env `esp32-s3-devkitc-1`） |
| 结果 | `[SUCCESS]` · 31.95 s（增量）／215.95 s（全量） |
| 平台/框架 | `espressif32@7.0.1` / `framework-arduinoespressif32@3.20017.241212`（Arduino-ESP32 2.0.17 / IDF 4.4.7） |
| 构建标志 | 默认（**未**加 `-DGF_TELEMETRY_ENABLED=1`，与烧录镜像不同 —— 见 §7） |
| **RAM** | **41.5%**（135 832 / 327 680 B） |
| **Flash** | **69.0%**（1 446 249 / 2 097 152 B）—— G-1 护栏 <85% ✅ |
| ELF 尺寸 | `text 1 160 434` / `data 302 080` / `bss 1 292 965` |
| 产物 | `.pio/build/esp32-s3-devkitc-1/firmware.bin` |
| 产物大小 | **1 446 608 B** |
| SHA-256 | `b1ba5c9c0806473964908b24936708cf197b51571be7482857c0348da64f9fc1` |

**构建产物归档说明**：本次 `.bin` 是**默认构建**（不含 `-DGF_TELEMETRY_ENABLED=1`），
仅用于编译验证与尺寸核算，**不可直接烧录**。真机镜像必须重新按项目纪律构建并归档。

---

## 5. 风险分析

### 5.1 紧急停止

| 风险 | 说明 | 缓解 |
|---|---|---|
| `stepper_emergency_stop()` 在无动作时返回 false | 会被记成 `ok=false, note=no_action_running`；`all_ok` 视为安全 | 语义已区分，不误报失败 |
| `vibration_stop()` 未初始化返回 false | 同上（`module_not_ready`） | 同上 |
| `valve_force_close()` **不检查 initialized**（VALVE-1 已知） | 模块禁用但引脚已配置时仍会写 GPIO | 对急停而言是**更安全**的行为，保持不变 |
| 急停与 config 写入并发 | `safety_stop_all()` **不写 Flash**，只做 GPIO/RMT 操作 | 无 Flash 竞争 |
| `latched` 只拦 Workflow | 单发 Action 仍可执行 | 刻意设计：拦掉会让用户卡死 |
| 急停本身阻塞 loop | 最坏约 10~100 ms（RMT 停 + 4 次 GPIO） | 可接受，急停优先 |

### 5.2 强制重启

| 风险 | 说明 |
|---|---|
| **打断 Flash 写入** | 300 ms 内可能中断 `config_save` / `workflow_storage` 写入 ⇒ 配置或文件损坏。用户已明确接受 |
| **boot flag 写失败** | `config_boot_validate()` 若被中断 ⇒ 下次启动 `last_boot_ok=false` ⇒ **CFG-ROLLBACK 用 Backup 覆盖 Active**。响应如实上报 `boot_flag_written` |
| `config_boot_validate()` 本身要写 Flash | 这是"为了避免回滚而主动写一次"，与"避免写 Flash"有冲突；选择前者（回滚的后果更严重） |
| 重启前停堆失败 | 不阻塞重启（否则设备卡死在故障态）⇒ 可能出现"带载复位" |
| `ESP.restart()` 调用点 | 现在有 2 处（普通 + 强制），都在 `system_command.cpp` 内 ⇒ 仍满足"重启实现唯一归属" |

### 5.3 阀门 60 秒

| 风险 | 说明 |
|---|---|
| **改代码不改变设备** | 设备实值 300；必须真机 `config_set` |
| 60 s 余量 | 21 g 闭环实测 45.4 s ⇒ 余量仅 **14.6 s**。水温低/水压小时长会变 ⇒ 有被截断风险 |
| 钳位收紧 1→10 | 若设备实值曾落到 1~9 会被钳到 10。当前实值 300，不受影响 |

### 5.4 GPIO 初始化

| 风险 | 说明 |
|---|---|
| 引脚误配 | 若 `valve.gpio_pin` / `stepper.en_pin` / `vibration.relay_pin` 被配成 4/5（I2C），会破坏 OLED/RTC。当前配置是 12/15/16，不冲突 |
| 高阻窗口 | 从复位到 `safety_outputs_init()` 仍有极短未定义期（无法用软件消除） |
| 继电器输入级 | 是否有下拉/光耦隔离取决于实际电路 ⇒ **必须万用表实测** |

---

## 6. M8 命令 ↔ 固件分派对照

见 §2 F-4 表格（22 条逐条列出分派函数与参数）。全部 **PASS**，无一条需要改固件。

---

## 7. 未实现 / 未验证 / 需硬件

### 未实现（有意不做）

| 项 | 原因 |
|---|---|
| 命令级 `boot_seq` | 属"新状态体系"，需单独批准 |
| `partial` 状态值 | 云端不认，会被映射成 `unknown`；改用 `all_ok` + 逐项 detail |
| `system.restart` 的 status 语义修正 | 属既有契约，不在授权范围（见 §2 F-3） |
| `valve.open_duration_ms`（死参数）清理 | schema 变更，可能影响未知消费者 |

### 需硬件才能验证（本阶段全部**未验证**）

1. `c="safety"` 的端到端：串口 `cm {}` 注入 + 观察阀门/步进/振动实际动作
2. 强制重启：真实复位、`boot_flag_written` 是否生效、配置是否被回滚
3. 阀门 60 s：必须先读取并备份当前配置，再 `config_set` 回读验证
4. **GPIO 高阻窗口**：万用表测复位瞬间阀 `IN` 脚电压（这是唯一无法靠软件证明的项）
5. 急停后 `latched` 与 Workflow 重启的交互

### 烧录后验证清单（供下一阶段）

```
① 万用表测 valve IN 脚：复位瞬间到 [Safety] 打印之间是否有导通脉冲
② 串口看启动顺序：[Safety] outputs init done 必须出现在 [WiFi] 之前
③ cm {"c":"safety","p":{"o":"stop_all"}} → 观察 result 帧与四执行器状态
④ cm {"c":"system","p":{"o":"force_restart"}} → 观察 boot_flag_written 与重启后 config
⑤ config_query valve → 确认 safety_timeout_sec 实值，再决定是否改成 60
```

---

## 8. 提交记录

| 项 | 值 |
|---|---|
| 提交 | **`e110b96`** |
| 分支 | `wb` → 已推送 `origin/main`（`4771330..e110b96`） |
| 变更 | 10 个文件（4 新增 / 6 修改） |
| 新增 | `src/app/safety.h`、`src/app/safety.cpp`、`test/ui_v11_contract/firmware_contract_check.py`、`docs/review/UI-v11-Firmware-Adaptation-2026-10-23.md` |
| 修改 | `src/main.cpp`、`src/services/command_manager.cpp`、`src/services/system_command.h`、`src/services/system_command.cpp`、`src/services/config_manager.cpp`、`src/app/valve.cpp` |
| **未提交** | 3 个前几轮遗留的未跟踪文档（`docs/architecture/Stepper-Vibration-Plan-Revision-Report.md`、`docs/evidence/LogAckSplit-2026-10-23/legacy-topic-rows-backup.json`、`docs/evidence/Stepper-NoRotation-Diagnosis-2026-10-22.md`）—— 不属于本轮，未触碰 |

---

## 9. 声明

**本阶段没有烧录、没有修改设备配置、没有发送设备命令、没有部署云端、没有应用数据库迁移。**
