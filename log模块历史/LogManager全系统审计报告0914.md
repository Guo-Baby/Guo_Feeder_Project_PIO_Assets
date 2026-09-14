# Log Manager 全系统审计报告

> 项目：Guo Feeder Project（ESP32-S3 N16R8）
> 审计对象：全系统（应用层 / 自动化层 / 服务层 / 云通信层）
> 审计日期：2026-09-14
> 依据：`log模块历史/代码审计需求0914.md`
> 代码基线：git `ce9ba37`（工作区含用户未提交的文档重组，本报告未触碰）
> 性质：**纯只读审计**。本次未修改任何 `.cpp` / `.h` / JSON / README / 测试，未执行 git commit。

---

## 1. Executive Summary

### 这个系统到底需要记录什么？

**需要记录的只有三类**：

1. **"设备做了什么"** —— 与用户直接相关的业务事实：
   Workflow 启停成败、阀门开关、供水目标达成/超时、配置生效、重启。
2. **"设备哪里坏了"** —— 需要事后归因的异常：
   重量传感器异常、强制关阀、存储/配置提交失败、MQTT 反复失败、时间源失效。
3. **"设备这次为什么起来"** —— 每次启动的身份信息：
   重启原因、Boot 是否完成、Recovery 是否发生。

### 明确不需要记录的

**高频物理量、内部状态机轮询、协议细节、逐条成功回复**。

具体来说，本项目当前存在 **≈580 处日志调用点**（Serials + 各级 log 回调），
其中**绝大多数应当停留在 Serial/Debug 层，永远不进入 Log Manager**：

| 类别 | 例子 | 理由 |
|---|---|---|
| 高频采样 | HX711 raw、BLE 广播原始 payload | 每秒/每毫秒量级，写 Flash 会烧毁介质 |
| 状态机轮询 | workflow poll、weight_task 每轮 | 无信息增量，只反映"还在跑" |
| 逐条成功回复 | `[Cloud] enqueue OK`、`UP-RESULT`、`[CMD][RESULT]` | 正常流量，量随业务线性增长 |
| 协议细节 | `[Cloud FRAG]`、JSON parse 中间态 | 只在调试期有价值 |
| 纯状态读取 | `state_get_xxx()` | 状态不是事件 |

### 核心结论（一句话）

> **本项目的 Log 需求远小于"现有日志量"，但当前缺失的信息恰恰是最关键的那几条**：
> 重启原因、Workflow 结果、供水结果、传感器异常、存储恢复。
> 现有的 580 处日志大部分是给开发者看的 printf，而设备真正需要的是
> **约 60–80 条结构化事件**（见 §5 分类表）。

### 三个必须先解决的架构前提

1. **JsonStorage / FileStorage 的日志回调从未注册**（见 §11.1），
   它们的 E/W 级错误**当前 100% 静默丢弃** —— 存储故障诊断链是断的。
2. **时间无效时（`time_get()` 返回 0）没有任何排序依据** ——
   没有 boot_id、没有 uptime 字段；见 §11.3 与 §12。
3. **Event Manager 的字符串映射表已损坏**（见 §16.2），
   若 Log Manager 依赖 `event_to_string()` 会产出错误的事件名。

---

## 2. Current Architecture Audit

### 2.1 分层与 Log Manager 的应处位置

```
┌─────────────────────────────────────────────┐
│ 应用 / 能力层                                │
│ Valve  Weight  DispenseGuard  Mijia  OLED    │
│ ComputerReset  (Motor 未实现)                │
└────────────────────┬────────────────────────┘
                     │ 产生"业务事实"
┌────────────────────┴────────────────────────┐
│ 自动化 / 中间层                              │
│ WorkflowManager（Trigger/Action/Workflow）   │
│ CapabilityRegistry（Stable ID / checksum）   │
└────────────────────┬────────────────────────┘
                     │ 产生"任务结果"
┌────────────────────┴────────────────────────┐
│ 服务层                                       │
│ SystemState  ConfigManager  EventManager     │
│ TimeManager  WiFiModule     CommandManager   │
│ ★ LogManager ←应在此层，与 ConfigManager 平级 │
└────────────────────┬────────────────────────┘
                     │
┌────────────────────┴────────────────────────┐
│ 云通信层 CloudManager（MQTT/JSON/CBOR/ACK）  │
└─────────────────────────────────────────────┘
```

**建议位置：服务层，与 ConfigManager / EventManager 同级。**
理由：

- 它需要**存储能力**（Flash 持久化）→ 必须能用 JsonStorage/BinStorage；
- 它需要**时间能力**（打时间戳）→ 依赖 TimeManager；
- 它需要**上行能力**（Cloud）→ 但**不能**直接依赖 CloudManager（会形成
  `LogManager → CloudManager → CommandManager → ...` 的反向依赖，违反 §2.2 分层）；
- 它应由 **CommandManager 之外的独立通道**上行（见 §11.4）。

### 2.2 现有模块间关系（与 Log 相关的部分）

| 模块 | 对外产出 | 是否已有 Log 钩子 |
|---|---|---|
| SystemState | 19 个状态键（bool/int/long/float/string） | ❌ 无任何日志 |
| EventManager | 17 个事件 + 风暴抑制 + drop/dup 计数器 | ❌ 仅 1 行 init 日志 |
| ConfigManager | 配置读写/提交/回滚 | ✅ `config_set_log_callback`（已生效，含 Serial 兜底） |
| CommandManager | 命令路由/结果/超时 | ✅ `command_manager_set_log_callback`（main.cpp 已注册） |
| WorkflowManager | Workflow/Action/Trigger 执行 | ❌ 无回调，仅 Serial |
| CapabilityRegistry | stable_id / version / checksum | ❌ 仅 Serial |
| JsonStorage | 文件原子读写 | ⚠️ `json_storage_set_log_callback`**从未注册 → 静默丢弃** |
| FileStorage | 文件原语 | ⚠️ `file_storage_set_log_callback`**从未注册 → 静默丢弃** |
| BinStorage | BIN 读写/原子替换 | ✅ `bin_storage_set_log_callback`（main.cpp 已注册） |
| WorkflowStorage | Workflow BIN 事务 | ❌ 仅 Serial |
| TimeManager | SNTP / RTC / time_get | ❌ 仅 Serial |
| WiFiModule | 连接状态机 | ❌ 仅 Serial |
| CloudManager | MQTT 收发/分片/去重 | ❌ 仅 Serial |
| Weight / Valve / DispenseGuard / Mijia / OLED / ComputerReset | 硬件能力 | ❌ 仅 Serial |

### 2.3 三类概念的现状区分（需求 §三 强调的重点）

| 概念 | 现状 | Log Manager 应如何使用 |
|---|---|---|
| **System State**（现在是什么） | 19 键，`state_set/get_xxx` | **不作为 Log 源**。仅在 Snapshot 时按需读取（§7） |
| **Event**（刚发生了什么） | 17 事件 + 优先级 + 风暴抑制 + drop/dup 计数 | **是一个可用的候选源，但不应全量转 Log**（见 §11.2） |
| **Log**（值得留存的） | **尚不存在** | 新模块，按本报告 §5 分类表定义 |

**关键判断：不要把 System State 或 Event 自动转成 Log。**

- SystemState 是"当前值"，把它做成 Log 会产生持续噪声且没有历史语义；
- Event 中有一半是**内部信号**（`EVENT_WEIGHT_READY`、`EVENT_CLOUD_UPLOAD`），
  这些是给 Workflow Trigger 用的，不是给人看的。

---

## 3. Log Level Definition

结合本项目实际情况定义（非通用教科书解释）：

### DEBUG
> **只有开发者在接串口时才需要的信息。线上设备应可编译期整体关闭。**

本项目判定标准：
- 每 `loop()` 都会产生的（`weight_task` 每轮、`workflow_task` 每轮）
- 逐条硬件数据（HX711 raw、BLE payload hex）
- 协议中间态（分片序号、outbox 计数、`ENQ`/`enqueue OK`）
- 状态机内部迁移（`TRIGGER_RUNNING`→`TRIGGER_SUCCESS`）
- 敏感内容（MQTT 密码、WiFi 密码、BLE bindkey）
- 开发者自测代码的输出（`wfst`/`wfc`/`cm` 控制台、`test_mqtt`）

**去向：Serial，`#ifdef` 编译期开关。不上云、不落盘。**

### INFO
> **一次正常业务的完成事实。云端能据此还原"设备今天做过什么"。**

本项目判定标准（"用户会问起"的事件）：
- 设备启动完成 / 重启被请求
- Workflow 开始 / 成功完成
- 阀门打开 / 关闭（含来源：workflow / 命令 / 保护）
- 供水目标达成
- 配置修改生效 / 配置落盘
- WiFi 连接成功、MQTT 连接成功、SNTP 同步成功
- RTC 校准成功
- 米家温湿度首次拿到数据

**去向：Cloud，不写 Flash。**（长期联网设备，正常运行不需消耗 Flash 寿命）

### WARN
> **异常但已自愈，或异常但暂不影响功能。需要事后能查到"发生了但恢复了"。**

- 供水超时 / 供水被保护中断
- HX711 连续无数据 / raw 恒零 / 重量跳变超阈值
- 阀门安全超时强制关闭、阀门操作被限流丢弃
- Workflow 执行失败 / 超时
- Workflow 保存失败（Dirty 保留）
- 配置提交失败并回滚
- 存储恢复（Backup / factory / boot-flag 恢复）
- MQTT 断开、WiFi 断开、WiFi 连接超时
- RTC 探测失败 / RTC 写失败 / RTC VL 置位
- 时间无效（`time_valid = false`）
- Event 队列满 / 事件风暴丢弃

**去向：Cloud + Flash。**

### ERROR
> **功能已经受损，需要人工介入或至少需要明确归因。**

- 配置 I/O 失败（Active 写失败、Backup 轮转失败）
- Workflow 存储事务失败（CRC / 格式 / 写入）
- Capability Registry 落盘失败
- MQTT 连续重试达上限（进入 1 小时休眠）
- 命令运行时队列满 / 回调 cmd_id 不匹配
- 存储层文件系统不可用

**去向：Cloud + Flash（与 WARN 同策略，分级用于告警优先级）。**

### CRITICAL
> **设备可能无法继续正常工作，或数据/硬件处于不可预期状态。**

- 系统启动未完成（`.bootok` 缺失 → 上次启动是 panic / 反复重启）
- LittleFS 挂载失败（`totalBytes()==0`）
- 核心存储不可用（JsonStorage / BinStorage init 失败）
- Workflow BIN 数据损坏（CRC 失败）
- 掉电事务恢复（PENDING 提交、暂存文件残留、abort 注入）
- Critical Operation 计数异常（release underflow）
- 强制关阀保护触发（`DispenseGuard` → `valve_force_close`）
- 内存耗尽（PSRAM 池分配失败、`sync oom`）
- 复位原因 = panic / watchdog / brownout

**去向：Cloud（最高优先级）+ Flash（立即）。**

> **为什么"强制关阀"是 CRITICAL 而不是 WARN**：
> 这是唯一由**安全模块**主动介入、越过 Workflow 直接操作硬件的路径。
> 它的触发意味着"系统判定当前供水不可控"。用户看到"今天只出了 12g 水"时，
> 这条记录是第一现场证据。

---

## 4. Module-by-Module Audit

### 4.1 System / Boot

**现状**：`main.cpp` 有 8 层显式初始化，每层都有 Serial 里程碑。但**全部无结构**：

```
[System] LittleFS mounted
PSRAM size: 8386279 / PSRAM free: 8386035
[CFG][I] init done, loaded 8/8 modules
Event manager init OK / [BLE] init OK
[Workflow] pools: trigger=PSRAM(114688 B) ...
[SYSCMD][I] init done, reset reason=..., safe delay=10000 ms
[CFG][I] boot validated
```

| 信息 | 现状 | 建议 Level | Flash | Cloud | 实时 |
|---|---|---|---|---|---|
| 设备启动完成（含 boot 序号、复位原因、耗时） | ❌ 无 | INFO | NO | YES | NORMAL |
| 复位原因 = 正常（poweron/sw） | ✅ 仅 Serial | INFO | NO | YES | NORMAL |
| 复位原因 = panic / wdt / brownout | ⚠️ 仅 Serial，**且下次启动后丢失** | CRITICAL | **YES** | YES | IMMEDIATE |
| 上次启动未完成（`.bootok` 缺失） | ⚠️ 仅内部判定，**从不外报** | CRITICAL | YES | YES | IMMEDIATE |
| Recovery 发生（从 Backup 恢复） | ✅ `[CFG][W] recovered from backup` | WARN | YES | YES | NORMAL |
| 初始化失败（各模块 init 返回 false） | ⚠️ 仅 Serial，且不中断 | ERROR | YES | YES | NORMAL |
| PSRAM 池分配失败 | ✅ `[Workflow] PSRAM + RAM allocate fail` | CRITICAL | YES | YES | IMMEDIATE |
| LittleFS 挂载失败 | ✅ 仅 Serial 一行 | CRITICAL | YES | YES | IMMEDIATE |
| 各层初始化耗时 | ❌ 无 | DEBUG | NO | NO | NO |

**关键缺口**：`system_command.cpp:222` 读取了 `esp_reset_reason()` 并缓存在
`static uint8_t s_reset_reason`，**只打一次 Serial，既不落盘也不上云**。
这是**整个系统最有价值却最缺失的一条记录** —— "设备为什么重启"目前不可追溯。
（`system_command_reset_reason_name()` 已提供可读名，API 现成。）

### 4.2 Workflow

**现状**（`workflow.cpp`，47 处 Serial）：

- 已有：`critical op acquired/released`、`save transaction: dirty=[...]`、
  `save wf=N -> OK/FAILED`、`bin load failed`、`apply_json rejected: ...`、
  `Temp action timeout:%s`、`[WF][DBG]` 系列（load/delete/apply/export，**无编译开关**）
- **完全缺失**：Workflow 开始 / 成功完成 / 失败 / 超时 ——
  `workflow_terminate()`（处理 `WORKFLOW_TIMEOUT` / `WORKFLOW_FINISHED` / `WORKFLOW_ERROR`）
  **一行日志都没有**。

| 事件 | 现状 | 建议 Level | Flash | Cloud | 实时 |
|---|---|---|---|---|---|
| Workflow 开始（slot / id / variant / 触发源） | ❌ 无 | INFO | NO | YES | NORMAL |
| Workflow 成功完成（slot / 总耗时） | ❌ 无 | INFO | NO | YES | NORMAL |
| Workflow 超时（slot / timeout_ms / 卡在第几步） | ❌ 无 | WARN | YES | YES | NORMAL |
| Workflow 执行失败（slot / 失败 step / 原因） | ❌ 无 | WARN | YES | YES | NORMAL |
| Action 失败（action id / 错误） | ❌ 无（只有 descriptor 内部） | WARN | YES | YES | NORMAL |
| Action 成功 | ❌ 无 | **不记录**（噪声） | — | — | — |
| Trigger 触发成功 | ✅ `[Weight] Trigger success: loss=..` | INFO | NO | YES | NORMAL |
| Workflow 保存成功 | ✅ `save transaction: OK (saved=1)` | INFO（可降 DEBUG） | NO | NO | NO |
| Workflow 保存失败 | ✅ `save failed, keep dirty` | WARN | YES | YES | NORMAL |
| Workflow 部分失败 | ✅ `save partial: dirty remain` | WARN | YES | YES | NORMAL |
| 存储恢复 / 迁移 | ✅ `Workflow migrated JSON -> Flash BIN` | INFO | NO | YES | NORMAL |
| Workflow 创建/修改/删除（云端） | ✅ `[CMD][WF] create/set/delete` | INFO | NO | YES | NORMAL |
| Dirty 状态变化 | ✅ `save transaction: dirty=[..]` | DEBUG | NO | NO | NO |
| Critical Operation 获取/释放 | ✅ `critical op acquired/released` | DEBUG | NO | NO | NO |
| Runtime 实例分配失败 / 池耗尽 | ⚠️ 部分（`def alloc failed`） | ERROR | YES | YES | NORMAL |
| `[WF][DBG]` 全部 | ⚠️ **无编译期开关** | DEBUG | NO | NO | NO |

**重要判断：不要为每个 Step / Action 记录**。
一条 4-step 的供水 Workflow 一次执行会产生 10+ 条 Step 级日志，
而诊断价值集中在**第一个失败点**和**总耗时**上。

### 4.3 Valve / Water Dispensing（重点）

**现状**（`valve.cpp` 10 处、`dispense_guard.cpp` 5 处）：

```
[Valve] OPEN (pin=12, level=1)          ← 每次开
[Valve] CLOSE (pin=12, level=0)         ← 每次关
[Valve] Operation too frequent, skipped ← 被限流（MIN_OPERATION_INTERVAL_MS）
[Valve] Safety timeout! Forced close (open > %lu ms)
[Valve] FORCE CLOSE (pin=.., level=..)
[DispenseGuard] Weight error received / Valve force closed / force close failed
```

**已有的优点**：`valve_force_close()` 会发布 `EVENT_VALVE_CLOSE` 且
payload = `"force close"`、`priority = EVENT_PRIORITY_CRITICAL` ——
**"保护的关阀"与"正常的关阀"在事件层已经可区分**，这是很好的设计基础。

**"用户反馈：今天为什么只出了 12g 水？"反推 Log 设计**：

要回答这个问题，服务器需要能重建下面这条链：

```
① 本次供水是被谁发起的？        → workflow slot / id / 触发源（定时 or 命令）
② 目标重量是多少？              → target_weight_g
③ 起始重量是多少？              → start_weight_g
④ 实际出了多少？                → 结束时 weight_delta_g
⑤ 为什么停的？                  → 原因枚举：达标 / 超时 / 保护关阀 / 手动 / Workflow 失败
⑥ 中途有没有异常？              → 重量跳变次数、HX711 无数据、阀门安全超时
⑦ 一共跑了多久？                → duration_ms
⑧ 关阀命令发出后重量还涨了吗？   → 关阀后 residual_gain_g（防溢水证据）
```

| 事件 | 现状 | 建议 Level | Flash | Cloud | 实时 |
|---|---|---|---|---|---|
| 供水开始（workflow slot / target_g / start_g） | ❌ 无 | INFO | NO | YES | NORMAL |
| 阀门打开（含来源 source） | ⚠️ 仅 Serial，无来源字段 | INFO | NO | YES | NORMAL |
| 阀门关闭（含来源 source） | ⚠️ 仅 Serial，无来源字段 | INFO | NO | YES | NORMAL |
| **供水结束汇总**（含 ①–⑧ 全部字段） | ❌ 无 | INFO（达标）/ WARN（未达标） | 未达标 YES | YES | NORMAL |
| 供水超时 | ❌ 无（只有 Workflow 级 timeout） | WARN | YES | YES | NORMAL |
| **强制关阀保护触发** | ⚠️ 仅 Serial + 事件 | **CRITICAL** | YES | YES | IMMEDIATE |
| 强制关阀失败 | ⚠️ 仅 Serial | CRITICAL | YES | YES | IMMEDIATE |
| 阀门安全超时（open 超过 safety_timeout_ms） | ✅ Serial + 事件 | WARN | YES | YES | NORMAL |
| 阀门操作被限流丢弃 | ✅ Serial | WARN | NO（频率可控） | YES | NORMAL |
| 异常增重 / 异常减重（非本次供水期间） | ⚠️ 仅 `STATE_WEIGHT_ERROR` | WARN | YES | YES | NORMAL |
| 溢水风险（关阀后仍持续增重） | ❌ 无 | CRITICAL | YES | YES | IMMEDIATE |

> **"关阀后仍持续增重"目前完全没有检测**（`valve_force_close` 后不存在任何
> 后验检查）。这是防水淹的最后一道防线，建议后续补充（本次不改代码）。

### 4.4 Weight / HX711

**现状**（`weight.cpp` 11 处，其中 1 处已注释）：

- ✅ 已注释掉的高频 debug：`/* ... [Weight] raw=%ld filtered=%d gram=%.1f ... */`
  —— **这是正确的做法**，应保持注释状态并最终改为编译期宏。
- 异常判定已经**边沿触发**：`if(err != error_state)` 才打印，很好。
- 风暴抑制已存在：`not_ready` 事件每 5s 最多一次。

| 事件 | 现状 | 建议 Level | Flash | Cloud | 实时 |
|---|---|---|---|---|---|
| 重量异常状态 0→1（含原因：无数据/raw零/跳变） | ✅ 仅打印 0/1，**无原因** | WARN | YES | YES | NORMAL |
| 重量异常状态 1→0（恢复） | ✅ 仅打印 0/1 | INFO | NO | YES | NORMAL |
| 跳变次数达阈值（5 次 / 5s，Δ>50g） | ⚠️ 只体现在 error 布尔 | WARN | YES | YES | NORMAL |
| 调零成功（offset / 采样数） | ✅ `Zero calibrated, offset=.., save=..` | INFO | NO | YES | NORMAL |
| 调零失败 / 保存失败 | ⚠️ 部分（save=false） | ERROR | YES | YES | NORMAL |
| 校准（scale 修改） | ❌ 无（无校准命令） | INFO | NO | YES | NORMAL |
| Tare / Weight Zero 动作 | ✅ `Zero calibration started` | INFO | NO | YES | NORMAL |
| Weight Trigger 触发（gram / start_weight / loss） | ✅ Serial | INFO | NO | YES | NORMAL |
| **HX711 raw 高频值** | ⚠️ 已注释 | **DEBUG 永不入库** | NO | NO | NO |
| 采样统计（采样数 / 丢弃数 / 最长无数据时长） | ❌ 无 | DEBUG | NO | NO | NO |

**关于 `STATE_WEIGHT_ERROR` 的改进建议（不改代码，仅建议）**：
现在只记 `-> 0/1`，缺"为什么"。建议 Log 参数带原因枚举
（`NO_DATA` / `RAW_ZERO` / `JUMP`）+ 触发时的 `jump_count`。

### 4.5 WiFi

**现状**（`wifi_module.cpp` 9 处）：

```
WiFi init...            Connecting to:<SSID>      WiFi connected
WiFi connect timeout    WiFi lost                 Try reconnect...
```

| 事件 | 现状 | 建议 Level | Flash | Cloud | 实时 |
|---|---|---|---|---|---|
| 开始连接（SSID） | ⚠️ 打印 SSID | INFO | NO | YES | NORMAL |
| 连接成功（含耗时、RSSI） | ⚠️ 无耗时/RSSI | INFO | NO | YES | NORMAL |
| 连接失败 / 超时 | ✅ `WiFi connect timeout` | WARN | YES | YES | NORMAL |
| 断开（含原因） | ✅ `WiFi lost`，**无原因** | WARN | YES | YES | NORMAL |
| 重连尝试（第 N 次） | ✅ `Try reconnect...`，**无计数** | WARN | NO | YES | NORMAL |
| RSSI 周期上报 | ❌ 无（`STATE_WIFI_RSSI` 存在但不记录） | **不记录**（用 Snapshot） | — | — | — |
| AP 配网 / NVS 兜底 | ❌ **代码中不存在**（见 §16.3） | — | — | — | — |

> **隐私提示**：`Connecting to:<SSID>` 会把 SSID 打串口。SSID 属弱隐私信息，
> 若 Log 要上云，建议**不记录完整 SSID**（可用哈希或只记长度）。

### 4.6 MQTT / Cloud

**现状**（`cloud_manager.cpp` 84 处，全系统最多）：

```
[Cloud] MQTT connected / disconnected / subscribed / published-ack
[Cloud] ENQ id=%d qos=%d store=%d outbox=%d->%d len=%u t=%lu
[Cloud] enqueue OK id=%d qos=%d store=%d len=%u
[Cloud] publish FAIL %s len=%u
[Cloud] UP-ACK id=%s obj=%s / UP-RESULT len=%u
[Cloud FRAG] fragment len=%u n=%u limit=%u type=%u
[Cloud] MQTT RX / Empty MQTT message / JSON parse failed
[Cloud] Missing command / Missing id / Command duplicate
[Cloud] translate fail: %s / Command execution failed
[Cloud] enter sleep retry          ← 连续失败 10 次后进入 1 小时休眠
```

| 事件 | 现状 | 建议 Level | Flash | Cloud | 实时 |
|---|---|---|---|---|---|
| MQTT 连接成功（含重连次数） | ✅ 有 connected，无计数 | INFO | NO | YES | NORMAL |
| MQTT 断开（含原因） | ✅ `disconnected outbox=%d` | WARN | YES | YES | NORMAL |
| MQTT 重连尝试 / 重试计数 | ⚠️ 只在 State | WARN | NO | YES | NORMAL |
| **进入 1 小时休眠**（retry ≥ 10） | ✅ `enter sleep retry` | **ERROR** | YES | YES | IMMEDIATE |
| 发布失败 | ✅ `publish FAIL` | WARN | YES | YES | NORMAL |
| ACK 超时 / 未收到 | ⚠️ `ERROR_TIMEOUT` 枚举存在但无对应日志 | WARN | NO | YES | NORMAL |
| 收到 malformed 消息 / JSON 解析失败 | ✅ `JSON parse failed` | WARN | NO | NO | NO |
| 协议错误（缺 command / 缺 id / 重复） | ✅ 三条 | WARN | NO | NO | NO |
| 云端命令执行失败 | ✅ `Command execution failed` | WARN | YES | YES | NORMAL |
| 云端连接恢复 | ⚠️ 只有 `connected` | INFO | NO | YES | NORMAL |
| 分片收发 | ✅ `fragment len=..` | DEBUG | NO | NO | NO |
| 每条消息 `ENQ` / `enqueue OK` / `UP-*` | ✅ **逐条** | **DEBUG** | NO | NO | NO |
| **MQTT DEBUG banner（含密码）** | ⚠️ 打印 host/port/user/**password** | **删除** | NO | NO | NO |

> **`ENQ` / `enqueue OK` / `UP-RESULT` 是明确的"不应进入 Log Manager"项**：
> 它们随业务量线性增长，且成功路径无诊断价值。
> 只有**失败**（`publish FAIL`）才值得 WARN。

### 4.7 Time / RTC / NTP

**现状**（`time_manager.cpp` 32 处，覆盖度很好）：

```
[Time] SNTP started / restarted / SNTP sync OK (%s)
[Time] SNTP not configured, skip start
[Time] RTC probe failed (addr=0x51 err=%u)
[Time] PCF8563T detected at 0x51 / RTC disabled by config
[Time] RTC VL flag set / RTC BCD decode out of range / RTC write NACK
[Time] Event: TIME_VALID / TIME_INVALID
[Time] RTC boot restore OK: %s / RTC present but time invalid
[Time] RTC drift %llds <= %ds, skip write
[Time] RTC calibrated to System Time / RTC write failed
[Time] RTC calibrate skip: restart pending
[Time] NTP skip: no wifi
```

| 事件 | 现状 | 建议 Level | Flash | Cloud | 实时 |
|---|---|---|---|---|---|
| SNTP 启动 / 重启 | ✅ | DEBUG | NO | NO | NO |
| SNTP 同步成功（含服务器与偏差） | ✅ `SNTP sync OK (%s)` | INFO | NO | YES | NORMAL |
| SNTP 失败 / 长时间未同步 | ⚠️ 无显式失败日志 | WARN | YES | YES | NORMAL |
| 时间变为有效（TIME_VALID） | ✅ + 事件 | INFO | NO | YES | NORMAL |
| 时间变为无效（TIME_INVALID） | ✅ + 事件 | WARN | YES | YES | NORMAL |
| RTC 探测成功 / 失败 | ✅ | INFO / WARN | 失败 YES | YES | NORMAL |
| RTC 上电恢复（RTC → System Time） | ✅ `RTC boot restore OK` | INFO | NO | YES | NORMAL |
| RTC 校准写成功 | ✅ | INFO | NO | YES | NORMAL |
| RTC 写失败（保留 System Time） | ✅ | WARN | YES | YES | NORMAL |
| RTC VL 置位 / BCD 非法 | ✅ | WARN | YES | YES | NORMAL |
| `NTP skip: no wifi` | ✅ **每次调用都打** | DEBUG | NO | NO | NO |

> **`[Time] NTP skip: no wifi` 是潜在高频点**：`time_sync_ntp()` 由云端命令或
> 重连逻辑触发，若被周期性调用会持续刷屏。应降为 DEBUG 或改为边沿触发。

### 4.8 Config

**现状**（`config_manager.cpp` 70 处，**全系统最规范**）：
已有的 `cfg_log("E"/"W"/"I", ...)` 覆盖了
`init done` / `boot validated` / `version file created` /
`module not loaded` / `recovered from backup` / `commit failed, rolling back` /
`save: cannot mark commit pending` / `recover: ...` 等 38 类消息。

| 事件 | 现状 | 建议 Level | Flash | Cloud | 实时 |
|---|---|---|---|---|---|
| 配置加载完成（N/8 模块） | ✅ `init done, loaded N/8` | INFO | NO | YES | NORMAL |
| 模块加载失败（走默认值） | ✅ `module not loaded, defaults will be used` | ERROR | YES | YES | NORMAL |
| 配置修改入队 | ✅ `enqueue accepted, pending=%d` | **DEBUG** | NO | NO | NO |
| **配置修改生效**（module/key/old/new） | ⚠️ 无（只有入队与 commit） | INFO | NO | YES | NORMAL |
| 配置保存成功 | ✅ `commit ... saved`（间接） | INFO | NO | YES | NORMAL |
| 配置提交失败并回滚 | ✅ `commit failed at %s, rolling back` | ERROR | YES | YES | NORMAL |
| 版本文件缺失/重建 | ✅ `version file missing or corrupt, will rebuild` | WARN | YES | YES | NORMAL |
| 从 Backup 恢复 | ✅ `recovered from backup, module=%s` | WARN | YES | YES | NORMAL |
| factory 恢复 | ✅ | WARN | YES | YES | NORMAL |
| 静态校验失败（字段不存在） | ✅ `module write rejected, unknown field` | WARN | NO | YES | NORMAL |
| 5 分钟倒计时到期自动重启 | ✅ `restart timeout reached` | INFO | NO | YES | NORMAL |

**关键判断（需求 §六 强调）**：
**普通 Config 修改不应因为"有变化"就写 Flash Log。**
理由：`config_set` 可能被 UI 高频调用（调参、滑条），若不限流，
一个 UI 会话就能写几十条 Flash。
**建议**：`config_set` 只记 DEBUG；只有**落盘成功/失败**与**恢复**才进 Log。

### 4.9 Storage

**现状**：三层存储（JsonStorage / FileStorage / BinStorage）+ WorkflowStorage，
错误码体系完整（`BinStorageResult` 10 项、`JsonStorage` E/W）。

| 事件 | 现状 | 建议 Level | Flash | Cloud | 实时 |
|---|---|---|---|---|---|
| 文件系统不可用（LittleFS 未挂载） | ✅ json/bin/bin 都有，但 **js/fs 静默丢弃** | CRITICAL | YES | YES | IMMEDIATE |
| 原子写失败（tmp 写 / 校验 / rename） | ✅ 三层都有，js/fs **静默丢弃** | ERROR | YES | YES | NORMAL |
| 写入字节数不符 / 落盘校验失败 | ✅ | ERROR | YES | YES | NORMAL |
| CRC 失败（BIN 内容损坏） | ✅ `WF_STG_ERR_CRC_FAILED` | CRITICAL | YES | YES | IMMEDIATE |
| 事务恢复（暂存文件发布 / 清理） | ✅ `recover publish rename failed` | WARN | YES | YES | NORMAL |
| Backup 恢复 | ✅（Config 层） | WARN | YES | YES | NORMAL |
| Workflow 持久化失败 | ✅ | ERROR | YES | YES | NORMAL |
| 读越界 / buffer 太小 | ✅ 错误码存在 | WARN | NO | NO | NO |

> **必须写 Flash 的存储类事件**：CRC 失败、文件系统不可用、事务恢复。
> 这些是"存储曾经处于不一致状态"的直接证据，掉电后必须还能看到。

### 4.10 Mijia BLE

**现状**（`MiThermometer.cpp` 27 处）：

```
[MiThermo] ADV len=%d : XX XX XX ...      ← ★ 逐字节 hex，在 BLE 回调里
[MiThermo] enter sleep %u sec / enter scan window / scan window finished
[MiThermo] scan success / scan failed count=%d level=%d
[MiThermo] enter LEVEL1 / LEVEL2: scanning disabled
[MiThermo] decrypt OK / temp + humidity received, BLE scan stopped
[MiThermo] decode type=%d value=%.2f
[MiThermo] start scan / stop scan / already enabled/disabled
```

| 事件 | 现状 | 建议 Level | Flash | Cloud | 实时 |
|---|---|---|---|---|---|
| BLE 初始化 / 采集被配置禁用 | ✅ | DEBUG | NO | NO | NO |
| 扫描窗口开始 / 结束 | ✅ | DEBUG | NO | NO | NO |
| 解密成功（拿到温湿度/电量） | ✅ `decrypt OK` | **INFO**（首次/状态变化） | NO | YES | NORMAL |
| 解码失败 / MIC 失败 / 解密失败 | ⚠️ 无显式 | WARN | NO（高频） | YES | NORMAL |
| 连续扫描失败达阈值 → LEVEL2 禁用 | ✅ `enter LEVEL2` | WARN | YES | YES | NORMAL |
| 传感器长时间失联 | ✅ 间接（LEVEL2） | WARN | YES | YES | NORMAL |
| 数据异常（温湿度越界） | ❌ 无 | WARN | NO | YES | NORMAL |
| **原始 BLE payload hex** | ⚠️ **在扫描回调内逐字节 printf** | **删除 / DEBUG 宏** | NO | NO | NO |
| `decode type/value` 每次解码 | ⚠️ 每次 | DEBUG | NO | NO | NO |

> **`decrypt OK` 属于"状态变化才记录"**：温湿度每 10 分钟才上报一次，
> 但如果未来把扫描窗口调密，它会变成高频源。建议改为
> **"仅当数值变化超过阈值或距上次记录超过 N 分钟"** 才产生 INFO Log。

### 4.11 Device Initialization / Provisioning

**现状：几乎不存在**。README §8.2 描述的三级联网兜底
（`config.json` → NVS 历史账号 → AP 配网）中，
**只有第一级实现**；`wifi_module.cpp` 中**没有任何 `softAP` / NVS 代码**
（已用全项目检索确认）。

| 事件 | 现状 | 建议 Level | Flash | Cloud | 实时 |
|---|---|---|---|---|---|
| 首次上电（无任何配置） | ❌ 无区分 | INFO | NO | YES | NORMAL |
| 进入 AP 配网模式 | ❌ 未实现 | INFO | NO | 否（无网） | — |
| 配网成功 / 失败 | ❌ 未实现 | INFO / WARN | YES | YES（联网后补发） | NORMAL |
| WiFi 凭据被更新 | ❌ 未实现（ConfigManager 有 `update_wifi`） | INFO | NO | YES | NORMAL |
| 初始化整体完成（含各层耗时） | ⚠️ 仅散落里程碑 | INFO | NO | YES | NORMAL |

> 配网相关 Log 的特殊性：**配网期间设备无网**，无法上云。
> 必须在联网后**补发**（这也是 Flash 需要保存的一类 INFO 例外，见 §8）。

### 4.12 OLED / ComputerReset（补充）

| 事件 | 现状 | 建议 Level | Flash | Cloud | 实时 |
|---|---|---|---|---|---|
| OLED 初始化成功/失败 | ✅ 仅 Serial | DEBUG | NO | NO | NO |
| OLED 订阅事件 | ✅ `OLED event registered` | DEBUG | NO | NO | NO |
| ComputerReset 脉冲开始/结束 | ✅ Serial | INFO（若为业务动作） | NO | YES | NORMAL |
| ComputerReset 安全超时强制拉低 | ✅ | WARN | YES | YES | NORMAL |
| ComputerReset 实例池耗尽 / 未初始化被拒 | ✅ | ERROR | YES | YES | NORMAL |

---

## 5. Complete Log Classification Table

> 说明：
> - **Flash**：NO = 不落盘；YES = 落盘（仅 WARN/ERROR/CRITICAL 及少数例外）
> - **Cloud**：NO = 不上云；YES = 上云
> - **Real-time**：NO = 可批量延迟；NORMAL = 尽快；IMMEDIATE = 最高优先级
> - "建议产生位置"指**代码中应当发 Log 的位置**（模块名 + 函数）

| Module | Event | Level | Flash | Cloud | Real-time | Parameters | Trigger（产生位置） |
|---|---|---|---|---|---|---|---|
| System | boot_complete | INFO | NO | YES | NORMAL | boot_seq, reset_reason, init_ms, cfg_version, wf_count | `main.cpp setup()` 末尾（`config_boot_validate()` 后） |
| System | boot_incomplete_prev | CRITICAL | YES | YES | IMMEDIATE | reset_reason_prev, missing_bootok | `config_manager.cpp config_init()`（`last_boot_ok==false`） |
| System | reset_reason=panic/wdt/brownout | CRITICAL | YES | YES | IMMEDIATE | reason_code, reason_name | `system_command.cpp system_command_init()` |
| System | reset_reason=normal(sw/poweron) | INFO | NO | YES | NORMAL | reason_code, reason_name | 同上 |
| System | init_failed | ERROR | YES | YES | NORMAL | module, layer, err | `main.cpp setup()` 各层 init 返回失败处 |
| System | littlefs_mount_failed | CRITICAL | NO* | YES | IMMEDIATE | err | `main.cpp`（挂载失败；*FS 不可用时无法落盘，只能上云/串口） |
| System | psram_pool_alloc_failed | CRITICAL | YES | YES | IMMEDIATE | which_pool, need_bytes, free_bytes | `workflow.cpp workflow_init()` / `capability_registry.cpp` |
| Config | cfg_load_done | INFO | NO | YES | NORMAL | loaded, total, ms | `config_manager.cpp config_init()` |
| Config | cfg_module_load_failed | ERROR | YES | YES | NORMAL | module | `config_manager.cpp config_init()` |
| Config | cfg_change_applied | INFO | NO | YES | NORMAL | module, key, old, new, source | `config_manager.cpp exec_set_field()`（落盘后） |
| Config | cfg_save_ok | INFO | NO | YES | NORMAL | modules[], version | `config_manager.cpp config_save()` |
| Config | cfg_commit_failed_rollback | ERROR | YES | YES | NORMAL | stage, module | `config_manager.cpp commit_fail_and_rollback()` |
| Config | cfg_recovered_from_backup | WARN | YES | YES | NORMAL | module, reason | `config_manager.cpp recover_module()` |
| Config | cfg_version_rebuilt | WARN | YES | YES | NORMAL | modules | `config_manager.cpp bootstrap_version_file()` |
| Config | cfg_factory_reset | WARN | YES | YES | NORMAL | module | `config_manager.cpp exec_reset()` |
| Config | cfg_enqueue_accepted | DEBUG | NO | NO | NO | module, pending | `config_manager.cpp config_cmd_enqueue()` |
| Storage | fs_unavailable | CRITICAL | NO* | YES | IMMEDIATE | layer(json/bin/file), err | `json_storage.cpp` / `bin_storage.cpp` init |
| Storage | atomic_write_failed | ERROR | YES | YES | NORMAL | path, stage(tmp/write/verify/rename), err | `json_storage.cpp` / `bin_storage.cpp` |
| Storage | bin_crc_failed | CRITICAL | YES | YES | IMMEDIATE | path, stored_crc, calc_crc | `workflow_storage.cpp workflow_storage_load()` |
| Storage | workflow_txn_recovered | WARN | YES | YES | NORMAL | wf_slot, staged_count, action(publish/discard) | `workflow_storage.cpp recover` |
| Workflow | wf_start | INFO | NO | YES | NORMAL | slot, id, variant, trigger_source | `workflow.cpp workflow_start()` |
| Workflow | wf_finished | INFO | NO | YES | NORMAL | slot, id, duration_ms, steps_done | `workflow.cpp workflow_terminate(WORKFLOW_FINISHED)` |
| Workflow | wf_timeout | WARN | YES | YES | NORMAL | slot, id, timeout_ms, stuck_step | `workflow.cpp workflow_terminate(WORKFLOW_TIMEOUT)` |
| Workflow | wf_failed | WARN | YES | YES | NORMAL | slot, id, fail_step, action_id, err | `workflow.cpp workflow_terminate(WORKFLOW_ERROR)` |
| Workflow | wf_action_failed | WARN | YES | YES | NORMAL | slot, step, action_id, err | `workflow.cpp` Action poll 返回 FAILED 处 |
| Workflow | wf_save_failed | WARN | YES | YES | NORMAL | slot, id, err | `workflow.cpp workflow_save_transaction()` |
| Workflow | wf_save_partial | WARN | YES | YES | NORMAL | ok_slots[], failed_slots[] | 同上 |
| Workflow | wf_crud | INFO | NO | YES | NORMAL | op(create/set/delete), slot, variant, source | `command_manager.cpp command_workflow_*` |
| Workflow | wf_migrated_json_to_bin | INFO | NO | YES | NORMAL | count | `main.cpp`/`workflow.cpp workflow_migrate_to_storage()` |
| Workflow | wf_dirty_changed | DEBUG | NO | NO | NO | dirty_slots[] | `workflow.cpp workflow_save_transaction()` |
| Workflow | wf_critical_op | DEBUG | NO | NO | NO | slot/temp_id, count | `workflow.cpp workflow_critical_acquire/release` |
| Dispense | dispense_start | INFO | NO | YES | NORMAL | wf_slot, target_g, start_g, source | `weight.cpp weight_trigger_start()` |
| Dispense | dispense_done | INFO | NO | YES | NORMAL | target_g, start_g, final_g, delta_g, duration_ms, stop_reason | `weight.cpp weight_trigger_poll()`（达标分支） |
| Dispense | dispense_failed | WARN | YES | YES | NORMAL | target_g, actual_g, delta_g, reason | `workflow.cpp wf_failed` 或 trigger 超时 |
| Dispense | valve_force_close | CRITICAL | YES | YES | IMMEDIATE | reason, weight_g, valve_was_open_ms | `dispense_guard.cpp dispense_guard_event_callback()` |
| Dispense | valve_force_close_failed | CRITICAL | YES | YES | IMMEDIATE | err | 同上（else 分支） |
| Dispense | valve_safety_timeout | WARN | YES | YES | NORMAL | open_ms, limit_ms | `valve.cpp valve_task()` 安全超时 |
| Dispense | valve_overflow_risk | CRITICAL | YES | YES | IMMEDIATE | gain_after_close_g, window_ms | **需新增**（关阀后仍增重检测，本报告 §16.5） |
| Valve | valve_open / valve_close | INFO | NO | YES | NORMAL | source(workflow/cmd/protect), pin, level | `valve.cpp valve_set_gpio()` |
| Valve | valve_op_rate_limited | WARN | NO | YES | NORMAL | interval_ms | `valve.cpp valve_set_gpio()` |
| Weight | weight_error_enter | WARN | YES | YES | NORMAL | cause(NO_DATA/RAW_ZERO/JUMP), jump_count, raw, gram | `weight.cpp weight_refresh_error_state()` |
| Weight | weight_error_exit | INFO | NO | YES | NORMAL | duration_ms | 同上 |
| Weight | weight_zero_done | INFO | NO | YES | NORMAL | offset, samples, saved | `weight.cpp weight_task()` 校准完成 |
| Weight | weight_trigger_start | INFO | NO | YES | NORMAL | gram, start_weight_g | `weight.cpp weight_trigger_start()` |
| Weight | weight_trigger_success | INFO | NO | YES | NORMAL | loss_g, target_g, duration_ms | `weight.cpp weight_trigger_poll()` |
| Weight | hx711_raw | DEBUG | NO | NO | NO | raw, filtered, gram | `weight.cpp`（**保持注释 / 编译期宏**） |
| WiFi | wifi_connect_start | INFO | NO | YES | NORMAL | ssid_hash（**非明文**） | `wifi_module.cpp` |
| WiFi | wifi_connected | INFO | NO | YES | NORMAL | connect_ms, rssi | `wifi_module.cpp` |
| WiFi | wifi_connect_timeout | WARN | YES | YES | NORMAL | timeout_ms, retry_n | `wifi_module.cpp` |
| WiFi | wifi_lost | WARN | YES | YES | NORMAL | reason, was_connected_ms | `wifi_module.cpp` |
| WiFi | wifi_reconnect_try | WARN | NO | YES | NORMAL | attempt_n | `wifi_module.cpp` |
| WiFi | provisioning_enter / exit | INFO | **YES（例外）** | YES（联网后补发） | NORMAL | mode, reason | 未来配网模块 |
| MQTT | mqtt_connected | INFO | NO | YES | NORMAL | retry_n, session_uptime_ms | `cloud_manager.cpp cloud_process_mqtt_events()` |
| MQTT | mqtt_disconnected | WARN | YES | YES | NORMAL | reason, outbox, connected_ms | 同上 |
| MQTT | mqtt_sleep_enter | **ERROR** | YES | YES | IMMEDIATE | retry_count, sleep_ms | `cloud_manager.cpp cloud_task()` |
| MQTT | mqtt_publish_fail | WARN | YES | YES | NORMAL | qos, len, store, topic | `cloud_manager.cpp cloud_mqtt_publish_*` |
| MQTT | mqtt_msg_malformed | WARN | NO | NO | NO | stage(parse/missing_cmd/missing_id), len | `cloud_manager.cpp cloud_process_rx_queue()` |
| MQTT | mqtt_cmd_exec_failed | WARN | YES | YES | NORMAL | cmd_id, cmd, obj, err | `cloud_manager.cpp on_command_result()` |
| MQTT | mqtt_enqueue / up_result | DEBUG | NO | NO | NO | id, qos, len | `cloud_manager.cpp`（**不上云不落盘**） |
| Time | ntp_sync_ok | INFO | NO | YES | NORMAL | server, unix, drift_ms | `time_manager.cpp`（loop 确认后，**非 callback**） |
| Time | ntp_sync_fail | WARN | YES | YES | NORMAL | last_ok_age_ms | `time_manager.cpp time_task()` |
| Time | time_valid_enter / leave | INFO / WARN | 离开 YES | YES | NORMAL | source(RTC/SNTP/INVALID), unix | `time_manager.cpp time_update_valid_state()` |
| Time | rtc_probe_ok / fail | INFO / WARN | 失败 YES | YES | NORMAL | addr, err | `time_manager.cpp time_rtc_probe()` |
| Time | rtc_boot_restore | INFO | NO | YES | NORMAL | rtc_unix, delta_s | `time_manager.cpp` |
| Time | rtc_calibrated | INFO | NO | YES | NORMAL | rtc_before, system_now, delta_s | `time_manager.cpp time_rtc_calibrate()` |
| Time | rtc_write_failed | WARN | YES | YES | NORMAL | err, keep_system_time=true | 同上 |
| Time | rtc_vl_flag / rtc_bcd_invalid | WARN | YES | YES | NORMAL | raw_bytes | `time_manager.cpp rtc_read_time()` |
| Time | ntp_skip_no_wifi | DEBUG | NO | NO | NO | — | `time_manager.cpp time_sync_ntp()` |
| Mijia | ble_data_decoded | INFO | NO | YES | NORMAL | temp, humid, batt_v, mac_suffix | `MiThermometer.cpp`（**值变化或间隔≥N分钟才记**） |
| Mijia | ble_decode_fail | WARN | NO（限流） | YES | NORMAL | fail_kind(decrypt/mic/decode), count | `MiThermometer.cpp` |
| Mijia | ble_sensor_lost_level2 | WARN | YES | YES | NORMAL | fail_count | `MiThermometer.cpp`（进入 LEVEL2） |
| Mijia | ble_raw_adv | DEBUG | NO | NO | NO | payload_hex | `MiThermometer.cpp onResult()`（**应删除**） |
| Event | event_queue_full | WARN | NO（计数） | YES | NORMAL | dropped_total, queue_size | `event_manager.cpp event_push()` |
| Event | event_storm_dropped | WARN | NO（计数） | YES | NORMAL | event_id, dropped_total | 同上 |
| CapRegistry | registry_rebuilt | INFO | NO | YES | NORMAL | type, version, count, checksum | `capability_registry.cpp registry_sync()` |
| CapRegistry | registry_flash_save_failed | ERROR | YES | YES | NORMAL | type, version | 同上 |
| Command | cmd_runtime_queue_full | WARN | YES | YES | NORMAL | active, capacity | `command_manager.cpp command_runtime_insert()` |
| Command | cmd_dup_cmd_id | WARN | NO | NO | NO | cmd_id | 同上 |
| Command | cmd_runtime_timeout | WARN | YES | YES | NORMAL | cmd_id, cmd, obj, timeout_ms | `command_manager.cpp command_runtime_scan_timeouts()` |
| Command | cmd_result | DEBUG | NO | NO | NO | full JSON | `command_manager.cpp command_report_result()` |
| SysCmd | critical_op_underflow | CRITICAL | YES | YES | IMMEDIATE | where, count | `system_command.cpp critical_operation_release()` |
| SysCmd | restart_requested / pending / executed | INFO | NO | YES | IMMEDIATE(executed) | state, critical_count, remain_ms | `system_command.cpp system_command_task()` |
| ComputerReset | pulse_start / finish | INFO | NO | YES | NORMAL | hold_ms, source | `computer_reset.cpp` |
| ComputerReset | safety_timeout_forced_low | WARN | YES | YES | NORMAL | high_ms | `computer_reset.cpp computer_reset_task()` |
| OLED | oled_init_ok | DEBUG | NO | NO | NO | — | `oled.cpp oled_init()` |

---

## 6. High-Frequency Sources

> **以下位置禁止直接调用普通 Log Manager 持久化接口。**

| # | 位置 | 频率估算 | 现状 | 处置建议 |
|---|---|---|---|---|
| 1 | `weight.cpp weight_task()` —— HX711 读取与滤波 | 每 `loop()`（`is_ready` 门控，10SPS；滤波后 ≈2 Hz） | 已注释掉 raw 打印 ✅ | 保持注释；改用统计量（采样数/无数据时长） |
| 2 | `MiThermometer.cpp MiAdvCallback::onResult()` —— BLE 广播 | 目标设备广播 ≈0.3–1 Hz；**未过滤的设备不打印** | **逐字节 `Serial.printf("%02X ")` 在 BLE 回调内** ❌ | **必须移除或 `#ifdef DEBUG`**；回调内不得做串口输出 |
| 3 | `workflow.cpp workflow_task()` —— 每轮扫描全部 Workflow × Step | 每 `loop()` | 无日志 ✅（也正因此缺失 wf 结果日志） | 只在**状态迁移点**发 Log，不在 poll 里 |
| 4 | `workflow.cpp workflow_trigger_poll/action_poll` | 每 `loop()` | 无日志 ✅ | 同上 |
| 5 | `cloud_manager.cpp cloud_task()` —— MQTT 事件/重连 | 每 `loop()`；`cloud_connect()` 在 `mqtt_client==nullptr` 时会**每轮调用** | `MQTT connecting...` + **含密码的 DEBUG banner** ⚠️ | 加失败计数与退避；banner 必须在 `#ifdef` 内 |
| 6 | `cloud_manager.cpp cloud_mqtt_publish_*` —— 每条上行 | 每条消息 | `ENQ` / `enqueue OK` / `UP-*` ✅ 有 | 全部降 DEBUG，不入 Log |
| 7 | `time_manager.cpp time_sync_ntp()` —— `NTP skip: no wifi` | 调用方决定 | 每次调用打印 ⚠️ | 降 DEBUG 或边沿触发 |
| 8 | `event_manager.cpp event_dispatch()` | 每 `loop()`（空队列时立即返回） | 无日志 ✅ | 已在 push 侧做风暴抑制（500ms/5 次），保持 |
| 9 | `weight.cpp` `not_ready` 超时事件 | 每 5s 最多 1 次（已限流 ✅） | 有 | 已有 `last_not_ready_event_ms` 限流，可作范式 |
| 10 | `weight_refresh_error_state()` | 每 `loop()` 调用，仅**边沿**打印 ✅ | 有 | 保持边沿触发 |
| 11 | `capability_registry.cpp registry_sync()` | 每次 CRUD/rescan | `reuse version=` / `rebuild version=` ⚠️ | `reuse` 降 DEBUG |
| 12 | `command_manager.cpp command_manager_task()` | 每 `loop()` | 仅超时/异常时打印 ✅ | 保持 |
| 13 | `oled_task()` / `oled_animation` | 每 `loop()` | 无日志 ✅ | 保持 |
| 14 | `test_mqtt.cpp` —— 自测发送 | 由命令触发 | 6 处 Serial ⚠️ | 属测试代码，应移入 `backup/`（README §九 规则） |

### 通用处置范式（本项目已验证有效的三种）

1. **边沿触发**（已有范例）：`if(err != error_state) { log(); }`
   —— `weight.cpp weight_refresh_error_state()`
2. **时间窗限流**（已有范例）：`if(now - last_x_ms >= INTERVAL) { log(); }`
   —— `weight.cpp` 的 `WEIGHT_NOT_READY_TIMEOUT_MS` 限流
3. **聚合计数**（已有基础设施）：`event_get_drop_count()` /
   `event_get_duplicate_count()` / `mqtt_retry_count` / `STATE_MQTT_RETRY_COUNT`
   —— 定期把这些**计数器**作为一条 Log 上报，而不是上报每次发生

> **明确禁止**：在 ISR / 回调（NimBLE `onResult`、esp-mqtt event handler）中
> 调用任何可能阻塞的 Log 接口。`Serial.printf` 在 115200 波特率下
> 每 100 字符约耗时 **8.7 ms**，在 BLE 回调中出现会直接拖垮 BLE 主机任务。

---

## 7. State Snapshot Recommendations

**原则（需求 §八）**：不要把整个 System State 每次复制进 Log；
只在**特定异常**发生时快照。

### 7.1 触发时机

| # | 时机 | 理由 |
|---|---|---|
| S1 | **Boot 完成**（`config_boot_validate()` 之后） | 建立"本次开机的基线"，后续异常可对比 |
| S2 | **CRITICAL 事件发生**（强制关阀 / 存储损坏 / 内存耗尽 / panic 恢复） | 第一现场，唯一机会 |
| S3 | **供水未达标结束**（dispense_failed） | 直接服务"为什么只出了 12g 水" |
| S4 | **Workflow 失败 / 超时** | 需要当时的资源与网络上下文 |
| S5 | **进入 MQTT 1 小时休眠** | 设备即将长期离线，必须留下证据 |
| S6 | **Storage recovery** | 需要知道恢复前的状态 |

### 7.2 Snapshot 应包含的字段（分层，按需裁剪）

| 组 | 字段 | 来源 | 是否总是包含 |
|---|---|---|---|
| 身份 | `boot_seq`、`uptime_ms`、`reset_reason` | 新增 / `esp_reset_reason()` | ✅ |
| 时间 | `time_valid`、`unix`、`last_ntp_sync`、`time_source` | `STATE_TIME_VALID` / `time_get()` / `time_get_last_ntp_sync()` | ✅ |
| 内存 | `heap_free`、`heap_min_free`、`heap_largest`、`psram_free` | `ESP.getFreeHeap()` 等 | ✅ |
| 网络 | `wifi_status`、`rssi`、`wifi_state`、`mqtt_status`、`mqtt_retry` | `STATE_WIFI_*` / `STATE_MQTT_*` | ✅ |
| 存储 | `cfg_versions[8]`、`registry_version`、`registry_checksum`、`fs_free` | `config_get_version()` / `capability_get_workflow_*()` | ✅ |
| 执行 | `wf_slot`、`wf_id`、`wf_variant`、`wf_state`、`wf_step`、`wf_elapsed_ms` | WorkflowManager（**需新增只读 getter**） | 仅 S3/S4 |
| 供水 | `target_g`、`current_g`、`start_g`、`delta_g`、`valve_open`、`valve_open_ms` | `STATE_VALVE_STATUS` / `STATE_WEIGHT_VALUE` + 新增 | 仅 S2/S3 |
| 传感器 | `weight_error`、`weight_error_cause` | `STATE_WEIGHT_ERROR` + 新增 | 仅 S2/S3 |
| 命令 | `last_cmd_id`、`last_cmd`、`last_cmd_result`、`pending_cmds` | CommandManager（需新增 getter） | 仅 S2/S4 |
| 环境 | `mi_temp`、`mi_humid`、`mi_valid` | `STATE_MI_THERMO_*` | 可选 |

### 7.3 明确不需要进 Snapshot 的

| 字段 | 理由 |
|---|---|
| `STATE_WEIGHT_VALUE` 的完整滤波窗口 | 只保留**当前值**即可，历史窗口无意义 |
| `STATE_EVENT_QUEUE_COUNT` | 瞬时值，只在异常时才有意义（且已有 drop 计数） |
| 全部 19 个 System State 键 | 复制整表既费 RAM 又无诊断价值；上表已按场景裁剪 |
| Config 全部内容 | 太大；只记 `version` |
| Workflow 全部 Step 内容 | 太大；只记 `slot/id/variant` |

### 7.4 体积与实现建议

- 一次 Snapshot 序列化后预估 **300–600 字节**（JSON）/ **150–250 字节**（CBOR）。
- **不要**在异常路径里做复杂的 JSON 构造（可能内存不足）。
  建议：**定长二进制结构**（固定字段序 + 紧凑类型），
  仅在**上传时**转 CBOR/JSON。
- Snapshot 应与触发它的 Log **同一条记录**，避免产生"两条独立记录要对齐时间"的额外复杂度。

---

## 8. Flash Storage Recommendations

### 8.1 写入频率估算（用于定容量）

| Level | 预估频率 | 每日条数 | 是否落盘 |
|---|---|---|---|
| DEBUG | 每 loop / 每条消息 | 10⁵–10⁶ | ❌ |
| INFO | 每次业务动作 + 网络事件 | **50–300** | ❌（除配网例外） |
| WARN | 传感器异常 / 断开 / 超时 | **5–50** | ✅ |
| ERROR | 存储/配置失败、重试上限 | **0–5**（异常日） | ✅ |
| CRITICAL | 保护关阀、存储损坏、panic | **0–2**（多数日子为 0） | ✅ |

**结论：真正写 Flash 的只有 WARN 及以上，正常日约 5–50 条。**
按 250 字节/条计算，**每日 ≤ 12 KB**。

### 8.2 关键设计决策建议

| 项目 | 建议 | 理由 |
|---|---|---|
| 是否需要 Ring Buffer | **需要** | 断网时无上限增长会写满分区 |
| 记录形式 | **定长二进制 Record** | 便于原地覆盖、CRC、无解析开销 |
| 记录大小 | **固定 128 字节**（含头部） | 对齐 Flash 页，便于按槽定位；超出部分字段裁剪 |
| CRC | **需要**（每条记录 CRC32 或 CRC16） | 掉电半写会产生残缺记录 |
| `sequence` | **需要**（uint32 单调递增） | 上传去重 + 排序 |
| `boot_id` / `boot_seq` | **需要**（uint16 或 uint32） | 跨重启排序的唯一依据（见 §11.3） |
| `timestamp` | 需要但**可为 0** | 时间无效时以 `uptime_ms` + `boot_seq` 排序 |
| 写入方式 | **批量提交**（累积 N 条或 M 秒） | 减少擦写次数；但 CRITICAL 必须**立即单条提交** |
| 掉电一致性 | 追加写 + CRC 校验 + 尾部哨兵字节 | 重启时丢弃最后一条不完整记录 |
| 逻辑删除 | 记录带 `uploaded` 位，**不物理删除** | 避免频繁擦除；Ring Buffer 覆盖最旧 |
| 本地条数上限 | **≤ 512 条**（128 B × 512 = 64 KB） | 留足余量，分区 12.4 MB |
| 单独分区 | **不需要** | 用独立文件即可（如 `/log/ring.bin`），避免改 `partitions.csv` |
| 文件位置 | `/log/ring.bin` + `/log/head.bin`（写指针） | 与 `/config`、`/workflow` 并列 |
| 是否复用 BinStorage | **建议复用** | 已有 `bin_storage_write_atomic` / `crc32`，且日志回调已注册在 main.cpp |

### 8.3 Flash wear 分析

- LittleFS 分区 12.4 MB；ESP32-S3 内置 SPI Flash 擦写寿命约 10 万次/扇区。
- 每日 ≤ 12 KB、批量提交（例如每 10 条一次），
  按扇区 4 KB 计：**每天约 1–3 次扇区擦写**。
- LittleFS 自带磨损均衡；**即使每天 10 次擦写，10 年也只有 3.6 万次** —— 寿命充足。
- **真正的风险是 INFO 落盘**：若把 INFO 也写 Flash，
  按每日 300 条 × 250 B = 75 KB/日 ≈ 每天 20 次扇区擦写，
  虽仍可接受，但**完全没有必要**，且会挤占 WARN 的环形空间。
  → **坚持"INFO 不落 Flash"。**

### 8.4 例外（必须落 Flash 的 INFO）

| 事件 | 为什么必须落盘 |
|---|---|
| 配网成功 / WiFi 凭据更新 | 配网期间无网，只能联网后补发；若期间掉电则必须还能看到 |
| 时间首次变为有效（`time_valid` 0→1） | 这是"设备从无时间到有时间"的转折，且往往是重启后的关键上下文 |

> 建议：此类 INFO 记录标记为 `persist=true`，与 WARN 走同一环形但优先级更低。

---

## 9. Cloud Upload Recommendations

### 9.1 现状约束

| 项 | 值 | 来源 |
|---|---|---|
| 协议 | MQTT 3.1.1，QoS 1 | `cloud_manager.cpp:21` |
| 单条上限 | `CLOUD_MSG_LIMIT_MAX = 8128` B（默认 8000） | `cloud_manager.cpp:33-35` |
| MQTT 缓冲 | 8192 B | `CLOUD_MQTT_BUFFER_SIZE` |
| 接收槽 | 4 × 8192 B | `MQTT_RX_SLOT_COUNT/SIZE` |
| 分片 | 已有（`CLOUD_FRAG_UP` / `CLOUD_FRAG_SET`） | `cloud_manager.cpp:38-39,465` |
| 重试 | `MQTT_RETRY_MAX=10` → 进入 **1 小时休眠** | `cloud_manager.cpp:27-28` |
| 去重缓存 | 10 条 / 30 s TTL（**仅下行命令**） | `MQTT_DUP_CACHE_SIZE/TTL_MS` |
| 上行 topic | `{uid}/up`；广播 `{uid}/set` | `cloud_protocol.md §1.2` |
| 协议中的 `log_upload` | **协议文档已列出，代码未实现** | `cloud_protocol.md §4.2` |

### 9.2 分级上传策略建议

| Level | 策略 | 批次 | 延迟容忍 |
|---|---|---|---|
| INFO | RAM 队列 + **批量**（N 条或 T 秒） | 10–20 条/批 | ≤ 60 s |
| WARN | 落盘后**尽快**上传；离线则留在 Flash | 5–10 条/批 | ≤ 5 s |
| ERROR | 同 WARN，但不受批量延迟 | 单条或小批 | ≤ 2 s |
| CRITICAL | **最高优先级，单条立即上传**；若离线则先落盘再等重连 | 单条 | **IMMEDIATE** |

### 9.3 单条 Log 报文体积

- 定长二进制 Record 128 B → CBOR 编码后约 **100–140 B**（含字段名压缩）。
- 20 条批量 ≈ **2.0–2.8 KB**，远低于 8000 B 上限，**无需分片**。
- **JSON 仅用于调试**；正式上行必须 CBOR（项目已开 `ARDUINOJSON_USE_CBOR=1`，
  但 README 记录 "CBOR 方案已完成验证，待正式整合" —— 即**当前上行仍是 JSON 文本**）。

### 9.4 关键机制建议

| 机制 | 建议 | 理由 |
|---|---|---|
| `sequence` | **必须**（每条 Log 唯一、单调） | 去重、断点续传、乱序检测 |
| ACK | **需要**。上行 Log 批次带 `seq_range`，云端回 ACK | 否则无法确认删除时机 |
| retry | 需要，但要有**上限 + 退避** | 避免与 MQTT 自身的重连风暴叠加 |
| offline queue | **需要**（就是 Flash Ring Buffer） | 设备长期离线场景 |
| 重复上传 | 靠 `sequence` 幂等 + 云端去重 | 网络重传必然产生重复 |
| ACK 后删除 | **标记 `uploaded=1`，不立即物理删除** | 环形覆盖自然回收；避免频繁擦写 |
| 上传中掉线 | 保持 `uploaded=0`，重连后续传 | 不做"上传中"中间态 |
| 上传阻塞 | **严禁**在 `loop()` 里同步等待 ACK | 违反非阻塞铁律；用状态机 |
| 上行通道 | **独立 `log_upload` 消息类型**，不进 `result` | 见 §11.4 |

### 9.5 需要后续决定的问题

1. 云端是否需要 Log 的**独立表**（`Device Log` 数据库已在 README 第四阶段规划）？
2. ACK 粒度：**批次级** 还是 **单条级**？（建议批次级，省带宽）
3. 时钟无效时上传的 Log，云端用什么字段做**主排序键**？（见 §11.3）
4. 是否允许云端**下发日志级别阈值**（动态调整 INFO 是否上传）？
5. Log 与现有 `state_report` / `device_online` 心跳的关系（是否合并）？

---

## 10. LogManager API Recommendations

### 10.1 API 形态建议

**不要**设计成 `log_info(String)` 这种自由文本接口。

**建议：Event ID + typed parameters 的结构化接口。**

```cpp
// 事件 ID（枚举，编译期固定，与 §5 分类表一一对应）
enum LogEventId : uint16_t {
    LOG_BOOT_COMPLETE = 1,
    LOG_WF_START,
    LOG_WF_FINISHED,
    LOG_DISPENSE_DONE,
    LOG_VALVE_FORCE_CLOSE,
    // ...
};

// 参数值（联合体，避免动态内存）
struct LogParam {
    LogParamType type;     // INT32 / UINT32 / FLOAT / BOOL / ENUM / STR8
    union { int32_t i; uint32_t u; float f; uint8_t b; };
    char s[16];            // 短字符串（模块名 / id 片段），定长
};

// 核心接口（非阻塞，只入队）
bool log_emit(LogEventId id, const LogParam *params, uint8_t count);
bool log_emit_critical(LogEventId id, const LogParam *params, uint8_t count);

// 便捷宏（编译期裁剪参数）
#define LOG_INFO(id, ...)  log_emit(id, MAKE_PARAMS(__VA_ARGS__), N)
#define LOG_DEBUG(...)     /* 编译期关闭时展开为空 */

// 主循环驱动（在 loop() 中调用，与 config_task 同级）
void log_task();

// 诊断
uint32_t log_get_dropped_count();
uint32_t log_get_pending_count();
```

### 10.2 为什么是 Event ID + typed parameters

| 维度 | String 自由文本 | Event ID + typed params |
|---|---|---|
| RAM 占用 | 每条构造 String（堆碎片） | 定长联合体，零动态分配 |
| Flash 体积 | 每条日志文本不同，无法压缩 | 事件 ID 后是紧凑二进制 |
| 云端解析 | 需正则/AI 解析 | 直接按 schema 解 |
| 多语言 | 无法本地化 | ID → 云端渲染文案 |
| 拼写/格式漂移 | 每处调用都可能不一致 | 编译期固定 |
| 与本项目既有范式一致性 | ❌ | ✅ 与 `CloudStatus`/`ErrorCode`/`LogEventId` 同类 |

**本项目已经在用这套范式**：`cloud_protocol.md` 的 `CloudStatus` / `ErrorCode` /
`RegistryType` / `FragmentType` 都是 C 枚举 + 缩写；Log 应延续。

### 10.3 参数类型要求

| 参数 | 类型 | 反例 |
|---|---|---|
| 重量 / 目标 | `float`（克） | ❌ 不要传 `"12.5g"` 字符串 |
| 时长 / 时间戳 | `uint32`（ms / unix） | ❌ 不要传 `"3.2s"` |
| slot / step / version | `uint8` / `uint32` | ❌ 不要传十进制字符串 |
| 原因 / 来源 | `enum` | ❌ 不要传 `"force close"` 文本 |
| 模块名 / id | 定长 `char[16]`（可截断） | ⚠️ 避免 String |
| 错误码 | `enum`（复用 `BinStorageResult` / `ConfigCommandError` / `CloudStatus`） | ❌ 不要传错误描述文本 |

### 10.4 明确禁止的 API 形态

- ❌ `log_info(const String &msg)` —— 动态 String，堆碎片
- ❌ `log_printf(fmt, ...)` —— 可变参数在嵌入式下易出错且无法裁剪
- ❌ `log(const char *json)` —— 调用方构造 JSON（违反"内部模块不处理 JSON"铁律，
  见 `AGENTS.md`）
- ❌ 任何**同步写 Flash** 的接口（必须在 `log_task()` 内异步）

---

## 11. Coupling / Dependency Analysis

### 11.1 现状发现的耦合问题（本次审计实测）

**问题 A：JsonStorage / FileStorage 日志回调从未注册**

```
src/json_storage.cpp:42:  static JsonStorageLogCallback s_log_cb = nullptr;
src/json_storage.cpp js_log():  if (s_log_cb == nullptr) { return; }   ← 静默丢弃
src/file_storage.cpp:32:  static FileStorageLogCallback s_log_cb = nullptr;
src/file_storage.cpp fs_log():  if (s_log_cb == nullptr) { return; }   ← 静默丢弃
```

`main.cpp` 只注册了 `bin_storage_set_log_callback`（:74）与
`command_manager_set_log_callback`（:114）。

**后果**：JsonStorage / FileStorage 的全部 E/W 级日志（约 88 处调用点）
**当前 100% 不可见**。ConfigManager 的原子写若在底层失败，
上层只看到 `bool false` 并打印 `write active failed, module=%s`，
**底层真实原因（tmp 写失败 / 大小不符 / rename 失败）完全丢失**。

> 这是本次审计发现的**最高优先级 Log 相关缺陷**。
> 修复成本极低（两行注册），但诊断价值极大。

**问题 B：System State 无任何日志**
19 个状态键，0 处日志。这**本身是正确设计**（状态不是事件），
但意味着任何状态变化都不会留下痕迹。→ 通过 Snapshot（§7）解决，不要改成逐状态日志。

**问题 C：Workflow / WorkflowStorage 无 log 回调**
两者都只有裸 `Serial.printf`（47 + 10 处），无法被 Log Manager 以回调方式接管。
→ 建议为它们各加一个与 `config_set_log_callback` 同形态的回调，
而不是让 LogManager 直接 include `workflow.h`。

### 11.2 需求 §九 十个问题的回答

| # | 问题 | 回答 |
|---|---|---|
| 1 | Module 是否直接调用 LogManager？ | **不应该**。应沿用项目已有的 **`set_log_callback` 模式**（5 个模块已有先例）。模块只声明语义事件，由 LogManager 注册回调或订阅 EventManager。 |
| 2 | Event Manager 是否应自动生成 Log？ | **不应该**。① 17 个事件中约一半是内部信号（`EVENT_WEIGHT_READY`、`EVENT_CLOUD_UPLOAD`）；② Event 有风暴抑制但语义是"给 Workflow 用"，自动转 Log 会把 Trigger 的内部噪声写进历史；③ `event_to_string()` 当前有 bug（§16.2）。**改为：LogManager 显式订阅它关心的少数事件**（`EVENT_VALVE_*`、`EVENT_WEIGHT_ERROR`、`EVENT_TIME_INVALID`）。 |
| 3 | System State 是否应自动生成 Log？ | **不应该**。状态是"当前值"，逐变化记 Log 会产生持续噪声（`STATE_WEIGHT_VALUE` 每秒变）。只在 Snapshot 时读取。 |
| 4 | Cloud Manager 负责 Log 上传，还是 LogManager 自己管理 Cloud Queue？ | **LogManager 管理自己的 RAM 队列与 Flash 环形；CloudManager 只做传输**。但**不能让 LogManager 直接 include `cloud_manager.h`**（会造成服务层反向依赖云通信层，违反 `AI_RULES.md §1`）。建议：LogManager 注册一个 `log_upload_callback`（与 `command_manager_set_result_callback` 同形态），由 `main.cpp` 在 setup 中把 CloudManager 的上行函数接进去。**依赖由 main.cpp 注入，不由 LogManager 硬编码。** |
| 5 | Log Manager 是否应知道 WorkflowManager 内部结构？ | **不应该**。LogManager 只知道 `LogEventId` 与参数值。`wf_slot/id/variant` 由 WorkflowManager 在调用点取出并作为 **typed 参数**传入；LogManager 不 include `workflow.h`。 |
| 6 | Log Manager 是否应知道 Weight/Valve 内部结构？ | **不应该**。同上。`target_g` / `current_g` 由 Weight 模块取出后传入。 |
| 7 | 如何避免与所有模块严重耦合？ | 三层隔离：① **接口层**：`log.h` 只暴露 `log_emit(id, params, count)` + `log_task()`；② **采集层**：每个模块用自己已有的 `set_log_callback` 或直接调用 `log_emit`（单向依赖，模块 → LogManager）；③ **输出层**：`main.cpp` 注入 Cloud 上传回调。**LogManager 不 include 任何业务模块头文件。** |
| 8 | 是否应采用 Event ID + 参数的结构化 Log？ | **是**，理由见 §10.2。 |
| 9 | 哪些参数应是整数/浮点/枚举而非 String？ | 见 §10.3 表。原则：**能枚举就枚举，能定长就定长**。 |
| 10 | 是否应避免在 Log API 中传递动态 String？ | **是**。`String` 会产生堆分配与碎片（本项目内部模块已明确禁止在内部传 JSON/String，见 `AGENTS.md`）。用定长 `char[16]` 截断存放 id/name。 |

### 11.3 Time 无效时的排序问题（需求 §六 重点）

**现状**：

```cpp
time_t time_get() {
    if (!state_get_bool(STATE_TIME_VALID)) return 0;   // ← 无效直接返回 0
    ...
}
// command_manager.cpp:
time_t ts = get_unix_timestamp();
if (ts > 0) doc["timestamp"] = ts;      // ← ts==0 时【字段直接省略】
```

**已有的时间来源**：`STATE_TIME_VALID`(bool)、`time_get()`、`time_get_last_ntp_sync()`、
`source`(RTC/SNTP/INVALID，仅 `system.time` 命令返回)、RTC 硬件持久化。

**缺失的**：`boot_id` / `boot_seq`（全项目检索为空）、任何形式的 `uptime` 字段。

**问题场景**：设备冷启动（RTC 无电池 / 无效），在 SNTP 成功前的 0–30 秒内
可能已经产生若干 Log（如 `wifi_connect_start`、`cfg_version_rebuilt`）。
这些记录**没有任何时间戳**，上传后云端无法排序，也无法与"上一次开机的记录"区分。

**建议的分析结论（不决定最终 Schema）**：

| 字段 | 作用 | 建议 |
|---|---|---|
| `timestamp_valid` | 标记本条时间戳是否可信 | **必须**（1 bit） |
| `unix` | 可信时间 | 需要（可为 0） |
| **`boot_seq`** | 跨重启排序的唯一依据 | **必须**（否则两次开机的无时间戳记录无法区分） |
| **`uptime_ms`** | 本次开机内的单调序 | **必须**（`millis()` 现成；用来在同一次开机内排序） |
| `seq` | 全局单调序号 | **必须**（去重 + 上传断点） |

**排序规则建议（两级）**：
```
① 若 timestamp_valid == 1 → 用 unix 排序
② 否则 → 用 (boot_seq 升序, uptime_ms 升序) 排序
③ 同一批内再用 seq 兜底去重
```

**`boot_seq` 的获取**：可在 `/log/head.bin`（或复用 `/config/`）中持久化一个
自增计数器，每次 Boot 完成时 +1。**这是整个 Log Schema 里唯一必须新增的持久字段。**

> 注意：RTC 有效时（`source=RTC`），`time_valid` 在 boot 早期即为 true，
> 此时可直接用 unix —— 这也解释了为什么 RTC 是有价值的：
> **它让"无网期间的日志"也能有真实时间戳。**

### 11.4 循环依赖风险的结论

```
❌ 不可接受：LogManager → CloudManager →（可能有回调回 Log）→ LogManager
❌ 不可接受：LogManager → WorkflowManager（服务层依赖自动化层，方向倒置）
✅ 可接受：  各业务模块 → LogManager（单向，模块只调用 log_emit）
✅ 可接受：  LogManager → EventManager（订阅少数事件；两者同为服务层）
✅ 可接受：  LogManager → TimeManager（取时间；服务层内部）
✅ 可接受：  LogManager → BinStorage（落盘；向下依赖存储层）
✅ 可接受：  main.cpp → 把 CloudManager 上行函数注入 LogManager 的 upload_callback
```

---

## 12. Missing Diagnostics

> 回答需求 §十二：如果设备以后出现下面这些问题，**目前系统缺什么诊断信息**？

### 12.1 "没有出够水"（只出了 12g）

**缺少**：
| 缺什么 | 应该由谁产生 | Level | Flash |
|---|---|---|---|
| 供水开始记录（target_g / start_g / 发起源） | `weight.cpp weight_trigger_start()` | INFO | NO |
| **供水结束汇总**（final_g / delta_g / duration_ms / **stop_reason**） | `weight.cpp weight_trigger_poll()` 或 `workflow_terminate()` | INFO/WARN | 未达标 YES |
| Workflow 失败时的失败 Step 与 Action id | `workflow.cpp workflow_terminate(WORKFLOW_ERROR)` | WARN | YES |
| 强制关阀事件及其原因 | `dispense_guard.cpp` | CRITICAL | YES |
| 供水期间的重量异常（跳变/无数据） | `weight.cpp weight_refresh_error_state()` 带 cause | WARN | YES |
| **关阀后残余增重**（溢水证据） | 需新增检测 | CRITICAL | YES |

**现状**：上述 6 项**全部缺失**。目前唯一线索是
`[Weight] Trigger success: loss=..`（成功时才有）与
`[Valve] OPEN/CLOSE` 两条无上下文的时间戳行。**无法回答用户的问题。**

### 12.2 "Workflow 偶尔失败"

**缺少**：
- Workflow 开始 / 结束 / 失败的**结构化记录**（`workflow_terminate()` 零日志）
- 失败时的**哪个 Step、哪个 Action、哪个错误码**
- 当时的内存 / 网络 / 时间是否有效（Snapshot）

**现状**：只有云端下发 `execute_workflow` 时才会有 `result` 上行；
**定时/事件触发的 Workflow 失败完全不可见**。

### 12.3 "设备偶尔离线"

**缺少**：
- 断开**原因**（`[Cloud] MQTT disconnected outbox=%d` 只有 outbox 计数）
- 断开**持续时长**与**累计离线时长**
- 重连**尝试次数序列**（`mqtt_retry_count` 只在 State，不记录）
- **进入 1 小时休眠**这一关键事件的持久记录（当前只有一行 Serial）
- WiFi 断开与 MQTT 断开的**因果关系**（谁先断）

**现状**：`STATE_MQTT_RETRY_COUNT` / `STATE_MQTT_LAST_ERROR` 是**瞬时状态**，
重启即丢；`STATE_MQTT_LAST_CONNECT_TIME` 是字符串且不落盘。

### 12.4 "重启后配置丢失"

**缺少**：
- **本次启动的复位原因**（panic / wdt / brownout / sw）—— 已有 API，未持久化
- **上次启动是否完成**（`.bootok` 缺失）—— 已判定，从未外报
- Recovery 的**完整链条**（哪些模块从 Backup 恢复、恢复前后版本）
- 配置提交的**阶段失败点**（`commit_failed at %s` 有，但 stage 语义未上传）

**现状**：`cfg_log` 的 Serial 输出**重启后消失**，云端看不到任何历史。

### 12.5 "必须补充的 Log 清单"（按优先级）

| 优先级 | Log | 位置 |
|---|---|---|
| **P0** | 注册 JsonStorage / FileStorage 日志回调 | `main.cpp setup()` |
| **P0** | `reset_reason` + `boot_incomplete` 落盘并上云 | `system_command.cpp` / `config_manager.cpp` |
| **P0** | Workflow 开始/完成/失败/超时 | `workflow.cpp workflow_terminate()` |
| **P0** | 供水结果汇总 + 强制关阀 | `weight.cpp` / `dispense_guard.cpp` |
| **P1** | MQTT 进入 1 小时休眠 | `cloud_manager.cpp` |
| **P1** | WiFi/MQTT 断开原因与持续时长 | `wifi_module.cpp` / `cloud_manager.cpp` |
| **P1** | 重量异常原因（NO_DATA / RAW_ZERO / JUMP） | `weight.cpp` |
| **P2** | boot_seq 持久化计数器 | 新增 |
| **P2** | 配置变更生效（module/key/old/new） | `config_manager.cpp` |
| **P2** | Capability Registry 重建 | `capability_registry.cpp` |

---

## 13. Recommended Implementation Order

> 只给顺序与依赖，不写代码。

```
阶段 0：前置修复（不属 LogManager，但必须先做）
  0.1 注册 JsonStorage / FileStorage 日志回调（2 行，直接恢复 ~88 处日志）
  0.2 修复 event_from_string / event_to_string（§16.2）
  0.3 移除/宏隔离 MiThermometer 的逐字节 BLE hex 打印
  0.4 移除 MQTT DEBUG banner 中的密码输出
  0.5 决定 test_mqtt.cpp 的归属（移入 backup/）
      ↓
阶段 1：Log Event Taxonomy（最先行，因为它决定一切）
  1.1 收敛 §5 分类表为 LogEventId 枚举（预计 60–80 个 ID）
  1.2 为每个 ID 冻结参数 schema（名称/类型/单位/取值范围）
  1.3 冻结 Level → (Flash, Cloud, Real-time) 的默认策略映射表
      ↓
阶段 2：Log Record Schema + 序列化
  2.1 定长 Record 结构（128 B，含 header）
  2.2 boot_seq / seq / timestamp / timestamp_valid / uptime_ms 语义冻结
  2.3 序列化 + 反序列化 + CRC 自校验（可复用 BinStorage 的 crc32）
      ↓
阶段 3：RAM Queue + 编译期级别裁剪
  3.1 定长环形 RAM 队列（禁止动态分配）
  3.2 `LOG_DEBUG` 等宏的编译期开关（`-DLOG_LEVEL_MAX=`）
  3.3 队列满策略：丢弃 DEBUG → 保留 CRITICAL（含丢弃计数）
      ↓
阶段 4：Flash 持久化（Ring Buffer）
  4.1 `/log/ring.bin` 环形 + `/log/head.bin` 写指针
  4.2 批量提交策略（WARN/ERROR 批量，CRITICAL 立即单条）
  4.3 启动时扫描并丢弃不完整尾记录
  4.4 boot_seq 持久化计数器
      ↓
阶段 5：Cloud Queue + 上传通道
  5.1 复用 cloud_protocol 新增 `log_upload` 消息类型
  5.2 upload_callback 注入（main.cpp，避免 LogManager 直接依赖 CloudManager）
  5.3 批次组包（≤20 条）+ CBOR 编码
      ↓
阶段 6：ACK / retry / 去重
  6.1 批次级 ACK（seq_range）
  6.2 ACK 后标记 uploaded，不物理删除
  6.3 重传幂等靠 seq
  6.4 上传状态机（非阻塞，禁止 loop 内等待）
      ↓
阶段 7：模块接入（按价值排序，一次接一个模块并上板验证）
  7.1 System/Boot（reset_reason、boot_complete）
  7.2 Workflow（terminate 四点）
  7.3 Dispense + Valve + Weight（供水全链路）
  7.4 Config + Storage（提交/恢复/CRC）
  7.5 WiFi + MQTT
  7.6 Time/RTC
  7.7 Mijia / OLED / ComputerReset（低优先级）
      ↓
阶段 8：State Snapshot
  8.1 Snapshot 结构（复用 §7.2 字段组）
  8.2 挂接 6 个触发点
  8.3 与 Log 记录合并为一条
      ↓
阶段 9：测试
  9.1 单元：Record 序列化往返、CRC、Ring 覆盖、掉电半写
  9.2 集成：队列满、Flash 满、离线累积、重连补发
  9.3 上板：Mock 掉电（复用 wfst 的 abort 注入范式）、断网演练
  9.4 长稳：72 小时运行，观察 Flash 擦写与 RAM 占用
```

**依赖关系**：阶段 1 是阶段 2–8 的前置；阶段 3 是 4/5 的前置；
**阶段 0 与阶段 1 可并行**（0 不依赖任何新架构）。

---

## 14. Open Questions

> 真正开始写 Log Manager 之前，**必须由产品/架构层决定**的问题。

| # | 问题 | 影响 | 建议倾向 |
|---|---|---|---|
| Q1 | **INFO 是否真的完全不落 Flash？** 若用户希望"断网 3 天后仍能看到做过什么"，INFO 必须落盘 | 决定 §8 容量与 §9 策略 | 折中：INFO 落盘但**窗口更短**（如只保留 50 条） |
| Q2 | Log 是否需要**本地可读**（OLED / 串口查询命令）？ | 决定是否要 JSON 渲染层 | 建议：串口提供 `log dump`，OLED 不做 |
| Q3 | 云端是否需要**动态调整 Log 级别阈值**（下发 `log_level`）？ | 决定 Log 是否需要 Config 集成 | 建议：进 Config（`log` 模块），但**默认级别编译期固定** |
| Q4 | **本地 Ring 保留多少条 / 多少天**？ | 决定分区占用与覆盖策略 | 建议：512 条或 7 天，取先到者 |
| Q5 | ACK 粒度：**批次级还是单条级**？ | 带宽 vs 可靠性 | 建议批次级 + `seq_range` |
| Q6 | 时间无效期间的 Log 是否**延迟上传**（等 SNTP 成功后补时间戳）？ | 决定是否需要在 RAM 中暂存 pending | 建议：不延迟；依靠 `boot_seq + uptime_ms` 排序 |
| Q7 | Log 是否需要**与 command result 关联**（同一次云端命令的日志串起来）？ | 决定是否要带 `cmd_id` | 建议：CRITICAL/ERROR 带 `cmd_id` |
| Q8 | **CRITICAL 时是否阻塞等待 ACK**（最长 N 秒）？ | 需求 §五 提出但未定 | 建议：**不阻塞**。先落盘，再尽力上传；设备安全性优先 |
| Q9 | 生产固件是否允许保留 `Serial` 输出（115200）？ | 若保留，需评估 BLE 回调内的串口开销 | 建议：生产固件默认关闭 DEBUG 级串口 |
| Q10 | Log 与 README 第四阶段规划的 **Device Log 数据库 / 心跳**如何分工？ | 避免重复建设 | 建议：Log 负责"异常与业务事实"，心跳负责"在线状态与会话时长" |
| Q11 | 是否允许把 **WiFi SSID / MQTT client_id** 写入 Log？ | 隐私 | 建议：SSID 只存哈希；MQTT 凭据一律不记 |
| Q12 | 是否需要 **Log 的端到端加密 / 签名**？ | 安全 | 建议：依赖 MQTT TLS（已用 8883），Log 层不额外加密 |

---

## 15. 明确不应该进入 Log Manager 的信息

> 需求 §十三 要求单独输出。以下是**逐条**列出并说明理由。

| 类别 | 具体内容 | 位置 | 为什么不能进 Log |
|---|---|---|---|
| 高频传感器原始值 | HX711 `raw`、`filtered`、`gram` | `weight.cpp:390`（**已注释**） | 10 SPS × 每轮循环；写 Flash 会快速耗尽擦写寿命 |
| 高频 BLE 广播 | `ADV len=..` + 逐字节 hex | `MiThermometer.cpp:117` | 在 NimBLE 回调内，串口输出会阻塞 BLE 主机任务 |
| 高频 Workflow poll | 每次 `workflow_task()` 的 Step 推进 | `workflow.cpp workflow_task()` | 每 loop 执行；无信息增量 |
| 高频 Trigger/Action poll | `timer_poll` / `delay_poll` / `action_poll` | `workflow.cpp` | 同上 |
| MQTT 消息循环 | `MQTT RX`、`ENQ`、`enqueue OK`、`UP-ACK`、`UP-RESULT`、`fragment` | `cloud_manager.cpp`（84 处中约一半） | 随业务量线性增长；成功路径无诊断价值 |
| 普通状态读取 | `state_get_xxx()` 调用 | 全项目 | 状态读取不是事件 |
| 每次函数调用/执行细节 | `Serial.print("Recv cmd: [")`、`Execute ret=` | `main.cpp serial_debug_command_process()` | 纯调试，且属自测控制台 |
| 普通 System State getter | 19 个状态键的读取 | 全项目 | 同上 |
| 正常 timer tick | — | — | 无 tick 日志（正确） |
| 命令结果全量 JSON | `[CMD][RESULT] {...}` | `command_manager.cpp command_report_result()` | 每条命令一次；已在 Cloud 通道，Log 不应重复 |
| 实例池/临界区内部轨迹 | `critical op acquired/released`、`save transaction: dirty=[..]` | `workflow.cpp` | 内部实现细节；仅调试期需要 |
| `[WF][DBG]` 全部 | load/delete/apply/export/compare | `workflow.cpp`（**无编译开关**） | 应改为编译期宏，永不进 Log |
| `[CapRegistry] reuse version=` | 每次 rescan 一条 | `capability_registry.cpp:662` | 只在 `rebuild` 时有价值；`reuse` 是常态 |
| 容量/资源快照类查询 | `system.memory` / `system.flash` 的返回 | `system_command.cpp` | 是"按需查询"，不是"应记录事件" |
| 敏感信息 | MQTT password、WiFi password、BLE bindkey | `cloud_manager.cpp:1205+`、`MiThermometer.cpp:212` | 安全；且日志会外发 |
| 初始化逐层细节 | `WiFi init...`、`OLED init...`、`[BLE] init OK` | `main.cpp` / 各模块 | 只在未完成时才需要（`init_failed` 已覆盖） |
| 调试控制台输出 | `wfst` / `wfc` / `cm` 的输出 | `main.cpp` | 只在上板自测时存在 |
| `test_mqtt` 全部 | `[TestMQTT] ...` | `test_mqtt.cpp` | 测试代码（README §九 要求移入 backup） |

---

## 16. 发现的问题，本次不修改

> 需求 §十六 要求单独列出。**以下均为只读审计发现，本次未做任何修改。**

### 16.1 【高】JsonStorage / FileStorage 日志回调从未注册 → 日志静默丢弃

- **位置**：`src/json_storage.cpp:42,102`、`src/file_storage.cpp:32,99`、
  `src/main.cpp:74,114`（只注册了 bin_storage 与 command_manager）
- **证据**：`js_log()` / `fs_log()` 首行 `if (s_log_cb == nullptr) { return; }`
- **后果**：约 88 处存储层 E/W 日志不可见；ConfigManager 原子写失败的**底层原因丢失**
- **建议**：`main.cpp setup()` 中补两行 `json_storage_set_log_callback(...)` /
  `file_storage_set_log_callback(...)`

### 16.2 【高】Event Manager 字符串映射表错位 + 阀门事件越界

- **位置**：`src/event_manager.cpp:37`（`event_from_string`）、`:82`（`event_to_string`）
- **证据 1**：`event_names[]` 中第 10 项是 `"EVENT_CLOUD_COMMAND"`，
  而枚举 `SystemEvent` 中**没有** `EVENT_CLOUD_COMMAND`，
  第 10 项实际是 `EVENT_COMMAND_RESULT` → **从索引 10 起整体错位一位**
- **证据 2**：`SYSTEM_EVENT_COUNT` 定义为 `(EVENT_ERROR + 1) = 15`，
  而 `EVENT_VALVE_OPEN/CLOSE/ERROR` 的枚举值是 14/15/16
  （定义在 `EVENT_ERROR` **之后**）→
  `event_to_string(EVENT_VALVE_OPEN)` 返回 `"EVENT_ERROR"`，
  `EVENT_VALVE_CLOSE/ERROR` 返回 `"EVENT_UNKNOWN"`，
  `event_from_string("EVENT_VALVE_OPEN")` 返回 `EVENT_NONE`
- **后果**：① 若 Log Manager 用 `event_to_string()` 记录事件名会产出**错误名称**；
  ② Workflow 的 Event Trigger **无法通过字符串绑定阀门事件**
  （而 `event_push`/`event_subscribe` 用的是 `MAX_EVENT_TYPE=32`，**所以事件本身是通的**，
  只有字符串转换层是坏的）
- **建议**：统一 `event_names[]` 与枚举顺序（把 `EVENT_CLOUD_COMMAND` 去掉或补进枚举），
  并把 `SYSTEM_EVENT_COUNT` 改为 `EVENT_VALVE_ERROR + 1`（或与 `MAX_EVENT_TYPE` 对齐）

### 16.3 【中】README 描述的 WiFi 三级兜底只有一级实现

- **位置**：`src/wifi_module.cpp`（全项目检索 `softAP` / `Preferences` / `nvs` **均无结果**）
- **README 描述**（§4.3 / §8.2）：`config.json` → **NVS 历史账号** → **AP 配网兜底**
- **实际**：只有 `config.json` 一级；无 NVS 缓存、无 softAP
- **影响**：README 与代码不一致；未来 Log 的 provisioning 相关条目暂无落点

### 16.4 【中】MQTT 调试横幅打印密码

- **位置**：`src/cloud_manager.cpp:1205` 起（`===== MQTT DEBUG =====` 区块）
- **证据**：实测串口输出 `password=GuoBaby`
- **影响**：凭据明文出现在串口日志中；若未来 Log 接入串口/云端会导致凭据外泄
- **建议**：整块移入 `#ifdef` 编译期开关，且**永不输出 password**

### 16.5 【中】关阀后"残余增重"无检测（溢水防护缺口）

- **位置**：`src/valve.cpp valve_force_close()` / `src/dispense_guard.cpp`
- **现状**：`valve_force_close()` 之后**没有任何后验检查**
- **风险**：若阀门卡滞或管路虹吸，关阀后重量仍增长 → **溢水**，且系统无感知
- **建议**：新增"关阀后 N 秒内重量仍在增长超过阈值"的检测，
  触发 CRITICAL（这是本次审计发现的**唯一功能性安全缺口**）

### 16.6 【中】`test_mqtt.cpp` 仍活在 setup/loop 中

- **位置**：`src/main.cpp:107`（`test_mqtt_init()`）、`:186`（`test_mqtt_task()//测试代码，需要删除`）
- **规则冲突**：README §九-2 "测试代码进入 backup"、§七-6 "src 只保留有效工程代码"
- **影响**：生产固件含测试代码；且它有 6 处 Serial 输出

### 16.7 【低】BLE 扫描回调内逐字节 `Serial.printf`

- **位置**：`src/MiThermometer.cpp:117-124`
- **具体**：`Serial.printf("[MiThermo] ADV len=%d : ")` +
  循环内每个字节一次 `Serial.printf("%02X ")`（29–62 次调用/包）
- **影响**：虽已被 MAC 过滤（只打目标设备），但在 **NimBLE 回调上下文**中
  执行 30+ 次串口写，每次可能阻塞（115200 波特率下整行 ≈ 9.5 ms）
- **建议**：整块移入 `#ifdef`；或改为一次性构造后输出

### 16.8 【低】Config 字段未被消费

- **`weight.json` 的 `sample_interval`（50）**：`src/weight.cpp` 中**无任何引用**
- **`weight.json` 的 `filter_samples`（10）**：实际用的是硬编码
  `WEIGHT_FILTER_WINDOW = 5`（`weight.cpp:43`）
- **影响**：云端/UI 改这两个字段**完全无效**，属"死配置"，
  会误导调参（并且未来 Log 若引用它们会记录假数据）

### 16.9 【低】`WEIGHT_DEBUG_PRINT_MS` 定义但未使用

- **位置**：`src/weight.cpp:35`（`#define WEIGHT_DEBUG_PRINT_MS 5000UL`）
- 对应的打印块已注释；宏成为残留

### 16.10【低】`[Time] NTP skip: no wifi` 每次调用打印

- **位置**：`src/time_manager.cpp:809`
- **风险**：若被周期调用会成为高频源；建议降 DEBUG 或边沿触发

### 16.11【提示】MCP C++ 工具在本项目不可用

- `mcp__mcp-cpp__get_project_details` 返回 `components: []`，
  `search_symbols` 报 "No build directories found"
- **原因**：本项目是 **PlatformIO**（非 CMake/Meson），MCP 需要
  `compile_commands.json` 所在的标准 build 目录
- **实际做法**：本次审计改用 **Grep + Read 逐文件通读**，
  并已覆盖 `src/` 全部 41 个 `.cpp`/`.h` 与 19 个模块文档
- **建议**：若希望 MCP 可用，可让 MCP 指向仓库根目录的
  `compile_commands.json`（`fix_compiledb.py` 已生成），或等待 MCP 支持 PlatformIO

---

## 附录 A：本次审计的阅读覆盖清单

**Markdown 文档（19 个，全部阅读）**

| 文档 | 用途 |
|---|---|
| `AI_RULES.md` | 架构边界、通信规则、错误处理（**已遵守：未改任何代码**） |
| `AI_CONTEXT.md` | 分层、数据流、配置系统、执行模型 |
| `AI_TASK.md` | TimeManager V2 需求（RTC/SNTP 语义、Critical Operation 边界） |
| `AGENTS.md` | 工程速览、源码地图、原文引用 |
| `readme.md`（2567 行） | 四层架构、19 状态键、17 事件、Workflow、Cloud 协议、开发规范 |
| `config_manager接口文档.md` | 8 模块、命令队列、commit 三态、Boot Validation |
| `system_command接口文档.md` | memory/flash/restart、Restart 状态机、8128 B 上限 |
| `critical_operation接入规范.md` | acquire/release 配对、各模块接入点、测试方法 |
| `cloud_protocol.md` | 紧凑字段、枚举、QoS、分片、`log_upload`（未实现） |
| `bin_storage开发说明0910.md` | BinStorage/FileStorage 分层、错误码、日志回调 |
| `json_storage接口文档.md` / `jsonstorage开发架构.md` | JsonStorage 原子写与错误语义 |
| `wifi开发笔记.md` / `需求文档.md` / `新增动作模板.md` | WiFi / 功能需求 / Action 模板 |
| `workflow_cloud_interface.md` | Workflow 云端契约（15 章） |
| `workflow修改历史需求/`（17 个） | Workflow 存储/CRUD/variant 的历史需求与报告 |

**源代码（`src/` 全部 41 个 `.cpp` / `.h`，重点通读）**

`main.cpp`(838) · `cloud_manager.cpp`(84 日志点) · `config_manager.cpp`(70) ·
`workflow.cpp`(47) · `command_manager.cpp`(39) · `time_manager.cpp`(32) ·
`MiThermometer.cpp`(27) · `capability_registry.cpp`(16) · `computer_reset.cpp`(12) ·
`weight.cpp`(11) · `workflow_storage.cpp`(10) · `valve.cpp`(10) · `system_command.cpp`(10) ·
`wifi_module.cpp`(9) · `test_mqtt.cpp`(6) · `oled.cpp`(5) · `dispense_guard.cpp`(5) ·
`event_manager.cpp`(1) · `system_state.cpp`(0) · `json_storage.cpp`(51) ·
`file_storage.cpp`(37) · `bin_storage.cpp`(24) · 及各 `.h`

**其他**：`data/`（9 个 config JSON + workflow.json + BIN 基线）、
`test/`（6 个用例集 + 批量脚本）、`tools/`（5 个脚本）、`partitions.csv`、`platformio.ini`

---

## 附录 B：本报告的关键数据

| 项目 | 值 |
|---|---|
| 全项目日志调用点 | **≈582 处**（cloud 84 / main 76 / config 70 / json_storage 51 / workflow 47 / …） |
| 已注册的日志回调 | **2 个**（bin_storage、command_manager） |
| **未注册且静默丢弃**的回调 | **2 个（json_storage、file_storage）** —— 约 88 处 E/W 日志不可见 |
| 未注册但有 Serial 兜底 | **1 个**（config_manager，`cfg_log` 在无回调时 `Serial.printf`） |
| System State 键 | **19 个**（0 处日志） |
| Event 类型 | **17 个**（1 处日志；风暴抑制 500ms/5 次） |
| Workflow 上限 | 16 个 × 16 Step × 8 Param |
| Config 模块 | 8 个 |
| Action / Trigger | 7 个 Action / 3 个 Trigger |
| 建议的 Log Event 数 | **约 60–80 个**（见 §5 表） |
| 建议 Flash Record | 128 B 定长 × ≤512 条 = 64 KB |
| 预估 WARN+ 日写入 | ≤ 12 KB/日（≈1–3 次扇区擦写） |
| MQTT 单条上限 | 8128 B（默认 8000） |
| MQTT 重试上限 | 10 次 → **1 小时休眠** |

---

*本报告为纯审计产出。全部结论基于代码与文档实测通读，未修改任何项目文件。*
