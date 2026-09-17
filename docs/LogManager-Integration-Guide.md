# LogManager 接入接口文档（P2 前置阶段）

> **文档性质**：设计 / 规范文档。**本阶段不修改任何生产代码、不进入模块接入开发。**
> **代码基线**：`65b4523`（P1.5 修复）+ `1d0b47c`（仓库整理）
> **依据**：`src/log_manager.h`、`src/log_manager.cpp`、`src/log_events.h`、`src/log_ack.h`、`src/log_cbor.h` 的**实读结果**
> **冻结来源**：`log模块历史/LogManager-P1契约冻结0915.md`（P1.1 已冻结，不得重新设计）

---

## 0. 阅读前必读（本文档的三条硬约束）

**① 本文档所有接口均来自实读代码，不是设计假设。**
已核对文件：`src/log_manager.h`(437 行) / `src/log_manager.cpp`(3260 行) / `src/log_events.h`(642 行) / `src/log_ack.h`(461 行) / `src/log_cbor.h`(171 行)。

**② ⚠️ 当前代码里不存在 `LOG_INFO()` / `LOG_WARN()` 这类宏。**
grep 结果：`src/` 下只有 `#define LOG_DEBUG_ENABLE 1`，**没有任何 `LOG_INFO` / `LOG_WARN` / `LOG_ERROR` 宏定义**。
真实入口是 `log_emit(event_id, level, params, count)`。本文 §4 按**真实接口**给出写法。

**③ ⚠️ Event ID 段分配与你提案不完全一致，且已冻结不可改。**
你在任务中提出的映射为 `0x01 System / 0x02 WiFi / 0x03 Workflow / 0x04 Valve / 0x05 Weight / 0x06 BLE / 0x07 Cloud`；
**实际冻结表**是 `0x01 System / 0x02 Config / 0x03 Storage / 0x04 Workflow / 0x05 Water / 0x06 WiFi / 0x07 Cloud / 0x08 Time / 0x09 BLE …`（见 §3.2、§3.3）。
按约束「不修改 P1.1~P1.5 已冻结 ABI」，**沿用实际冻结表**，你的提案记为差异说明。

---

## 1. LogManager 当前架构说明

### 1.1 分层视图

```
业务模块 (System / Workflow / Valve / Weight / WiFi / BLE / Cloud / …)
        │  只调用 log_emit() / log_emit0() + log_arg_*()
        ▼
┌──────────────────────────────────────────────────────┐
│ LogManager (Services 层)                              │
│                                                       │
│  RAM Ring ──► log_task()(loop 内) ──► routing 决策     │
│                                        │              │
│                        ┌───────────────┴────────┐    │
│                        ▼                        ▼    │
│              Flash Segment Ring        Cloud 待发队列  │
│              (WARN+ 才落盘)            (INFO+ 才入队)  │
│                        │                        │     │
│              离线保命 / 补发源            CBOR 批次     │
│                        └────────► MQTT log Topic      │
│                                          │            │
│                                     log_ack (down)    │
│                                          │            │
│                                   ACK 落账 → 段回收    │
└──────────────────────────────────────────────────────┘
```

### 1.2 RAM Queue（P1.2）

- **64 槽 × 128 B = 8192 B**，`heap_caps_malloc(..., MALLOC_CAP_SPIRAM)` **PSRAM 优先、失败回退 DRAM**（`log_manager.h:34`）。
- 生产（`log_emit`）与消费（`log_task`）都在 `portENTER_CRITICAL(&s_mux)` 内拷贝 128 B；**无动态内存、无 String**。
- 环满 → **淘汰最旧**并累加 `LogStats.ring_drop`。
- `log_task()` 每轮最多消费 `LOG_DRAIN_MAX_PER_TASK = 8` 条。

### 1.3 Flash Segment Ring（P1.3）

- 目录 `/log`，文件 `s0000000.log … s0000015.log`（`LOG_SEG_PATH_FMT`）。
- **每段恒为 3984 B**：段头 16 B（magic / seg_index / first_seq / crc32）+ 31 × 128 B（`LOG_RECORDS_PER_SEGMENT = 31`）。
  > 3984 < 4096 是**设计目标（适配 4 KB block）**，不对 LittleFS 物理层做严格映射假设。
- **16 段 ⇒ 容量 496 条 ≈ 63.7 KB**。
- 原则：**整段创建 / 整段追加 / 整段删除**；**禁止** ACK 后回写记录内的 uploaded 位（LittleFS 是 COW，回写会重写整个 block）。
- 只有 `log_level_to_flash(level) == true` 的记录才落盘 ⇒ **DEBUG / INFO 永不进 Flash**。

### 1.4 Cloud Sync（P1.4）

- 独立云待发队列：**128 槽 × 128 B = 16 KB**，PSRAM 优先。
  > 为什么需要它：INFO 是 Cloud YES / Flash NO，不落盘 ⇒ **无法从 Flash 补发**，只能靠这条队列。
- 组批：单批 **≤ 16 条**、**payload ≤ 4096 B**、**必须同一 boot_seq**（批次头只有 1 个 boot_seq 字段）。
- Wire 格式：**CBOR 定序数组，12 项**（`LOG_BKEY` 0..11），records 以 `bstr(128)` 原样携带冻结 Record 布局。
- 节流：**最快 1 批 / 500 ms**（`LOG_TX_MIN_INTERVAL_MS`）。
- 传输：`cloud_send_log()` → MQTT **独立 `log` Topic**（`guo_feeder/log`），**与业务 `up` Topic 完全分离**。

### 1.5 ACK / Retry（P1.5）

| 参数 | 值 |
|---|---|
| ACK 超时 | `LOG_ACK_TIMEOUT_MS` = 15000 |
| 最大重试 | `LOG_ACK_MAX_RETRY` = 5 |
| 退避 | 2 / 4 / 8 / 16 / 32 s，上限 `LOG_ACK_BACKOFF_MAX_MS` = 60000（基数 `LOG_ACK_BACKOFF_BASE_MS` = 2000，定义于 `log_ack.h:38`） |
| ACK 报文 | `{"c":"log_ack","i":..,"p":{"b":<boot_seq>,"f":<seq_from>,"t":<seq_to>}}`，走 **down** Topic，**旁路 CommandManager** |
| 传输语义 | **Device = at-least-once；Cloud 必须幂等去重**（键：`device_id + boot_seq + seq`） |
| 重试耗尽 | **GIVE_UP_NOT_ADVANCE**：只把该批移出云队列 + `drop_unacked++`；**不推进 ack 水位、不删 Flash 段** |

- **推进依据 = 收到 ACK，不是发送成功**。
- 部分 ACK（如发 100–104 收到 ACK 100–102）→ **只推进被覆盖的前缀**（`covered`），尾部 103/104 保留在队列下轮重发；**不会错误推进到 104**。

### 1.6 Offline Replay

- MQTT 离线时：**不消费任何记录**（留在云队列 / Flash），仅 `cloud_offline_skip++`。
- 云队列排空后，从 Flash 段环**按 slot 遍历**补发未确认记录（`cloud_replay_step()`）。
  > ★ 现为 **(segment, slot) 游标**（DIR-1），**不是** `seq - first_seq` 反算 —— 后者在 seq 稀疏时会提前终止补发（P1.5 已修）。
- 补发扫描上限 `LOG_REPLAY_SCAN_MAX_PER_CALL = 32` 条 / 次调用，保证 loop 时间预算。
- **重启后 `acked_seq` 归 0 ⇒ 历史记录可能重发**。这是**设计内行为**，由云端幂等消化。

### 1.7 Hole Protection（FIX-2，P1.5 收尾新增）

- **问题**：`acked_seq` 是**单点高水位**，不能证明某条具体记录被确认。若存在"被淘汰 / give-up 但未确认"的记录，水位跨过它们后，其物理副本可能被整段回收 ⇒ **重启也补不回**。
- **机制**：`LogHole[8]` 记录这些"未确认区间"，可合并；登记点：
  1. `cloud_give_up_batch()` 放弃的 `[tx_from, tx_to]`；
  2. `cloud_queue_push()` **非在途窗口**溢出淘汰的 Flash-routed 记录；
  3. `cloud_handle_ack()` 中 `evicted > covered` 时，为 `[covered, evicted)` 的 Flash-routed 记录补齐（FIX-H2）。
- 表满（`hole_overflow`）⇒ **保守停止一切段回收**。
- 纯 RAM 态，重启后由 `acked = 0` 自愈。

### 1.8 Segment Reclaim

删除一个 Flash 段的**全部条件**（`log_ack_segment_reclaimable()`，生产路径唯一判定点）：

1. 整段 record 的**真实末条 seq ≤ ack 水位**（用 `s_seg_last_seq[]` 实测值，**禁止**用 `first_seq + records - 1` 推导）；
2. 与任何 Hole **不相交**；
3. 不是当前追加目标段（`s_append_segment`）；
4. 补发游标已越过该段；
5. 且 `hole_overflow == false`。

### 1.9 ★ 业务模块**不应关心**的东西（明确清单）

| 不应关心 | 原因 |
|---|---|
| **Flash** | 是否落盘由 Level Policy 唯一决定；模块不感知段/文件/CRC |
| **ACK** | 由 LogManager 内部状态机处理；模块不感知批次/水位 |
| **MQTT** | 独立 log Topic、QoS、重连全由 LogManager + CloudManager 负责 |
| **CBOR** | Wire 编码由 `log_cbor.h` 负责；模块只给结构化参数 |
| **Retry / 退避 / 离线** | 重试次数、退避时长、离线补发全部内部自治 |
| **seq / boot_seq** | 由 `log_emit()` 在临界区内分配；模块不得自选 |
| **Record 布局 / 段格式** | 冻结的 128 B / 3984 B，模块不得依赖或构造 |
| **丢弃统计** | `ring_drop` / `drop_unacked` 等是 LogManager 侧信道，模块不得写入 |

### 1.10 ★ 业务模块**只需关心**的东西

1. 选一个**已注册的 `LogEventId`**；
2. 判断正确的 **`LogLevel`**；
3. 挑 ≤ 8 个 **`LogParamId`** + 用 `log_arg_*()` 填值；
4. 调用 `log_emit()` / `log_emit0()`；
5. （可选）对高频事件做**模块侧节流**（见 §7 F-4）。

---

## 2. 模块日志调用规范

### 2.1 三类日志

| 类别 | 定义 | 例子 |
|---|---|---|
| **① 普通事件** | 一次已发生的业务事实（完成 / 开始 / 被拒绝） | `LOG_VALVE_OPEN`、`LOG_WF_START`、`LOG_WEIGHT_ZERO_DONE` |
| **② 状态变化** | 状态机迁移**边**（A→B），不是周期性状态快照 | WiFi `DISCONNECTED→CONNECTING→CONNECTED`；`LOG_WEIGHT_ERROR_ENTER/EXIT` |
| **③ 错误 / 异常** | 失败、降级、超限、硬件不可用 | `LOG_WEIGHT_CALIB_FAILED`、`LOG_VALVE_FORCE_CLOSE`、`LOG_MQTT_PUBLISH_FAIL` |

> **反模式**：把"当前状态"周期性地打进 Log（那是 System State 的职责）。Log 记**历史事实**，不记"现在是什么"。

### 2.2 Level 判断规则（决策树）

```
设备/功能是否仍可继续正常工作？
├─ 是，且只是常规业务事实（成功完成、正常切换）
│     └─ INFO        （Cloud YES / Flash NO —— 占带宽不占 Flash）
│
├─ 是，但走了降级/重试/被限流/接近阈值
│     └─ WARN        （Flash YES —— 落盘保命）
│
├─ 否，某个功能已不可用 / 操作确定失败
│     └─ ERROR       （Flash YES）
│
└─ 否，且涉及安全/数据完整性/必须立即留证
      └─ CRITICAL    （Flash 立即 + 最高优先级下一批次）
```

**调试信息（原始采样、逐字节 hex、内部循环跟踪）→ `DEBUG`。**
`DEBUG` 在 `log_emit()` 处**直接早退**：不入 RAM 环、不落盘、不上云，仅 `debug_dropped++`。

### 2.3 Level Policy 表（冻结，唯一策略）

| Level | 值 | Flash | Cloud | 典型用途 |
|---|---|---|---|---|
| `LOG_LVL_DEBUG` | 0 | ✗ | ✗ | 开发期原始输出 |
| `LOG_LVL_INFO` | 1 | ✗ | ✓ | 业务完成 / 状态迁移成功 |
| `LOG_LVL_WARN` | 2 | ✓ | ✓ | 重试、降级、限流、超时 |
| `LOG_LVL_ERROR` | 3 | ✓ | ✓ | 功能失败、硬件不可用 |
| `LOG_LVL_CRITICAL` | 4 | ✓ | ✓ | 安全兜底、数据损坏、异常复位 |

> **★ 无任何 EventId 级例外**。禁止 `persist` / `force_flash` / `flash_override`
> （"配网掉电要留存"这类需求应另做**独立的状态持久化机制**，不给 Level 开洞）。

### 2.4 各模块典型事件与建议 Level

| 模块 | 事件 | 建议 Level | 说明 |
|---|---|---|---|
| **Valve** | 阀门开启 / 关闭 | INFO | 普通完成 |
| | 安全超时关阀 | WARN | 降级路径 |
| | 开启被限流 | WARN | 限流 |
| | **强制关阀** | **CRITICAL** | 安全兜底，必须立即留证 |
| **Weight** | 去皮完成 | INFO | |
| | 标定失败 | ERROR | 功能不可用 |
| | 进入 / 退出异常态 | WARN / INFO | 状态变化**边** |
| | 称重触发 | INFO | |
| **WiFi** | 开始连接 / 连接成功 | INFO | 状态迁移 |
| | 连接超时 / 掉线 / 重连尝试 | WARN | 降级 |
| | 进入 / 完成配网 | INFO | |
| **Workflow** | 启动 / 完成 | INFO | |
| | 超时 / 失败 / 动作失败 / 保存失败 | WARN | |
| | 运行时分配失败 | ERROR | |
| **Time** | NTP 成功 | INFO | |
| | NTP 失败 / 时间失效 | WARN | |
| | RTC 探测：成功 INFO / **失败 WARN** | INFO·WARN | 同一 EventId 两种 Level（`LOG_TIME_RTC_PROBE`） |
| **BLE** | 数据解码成功 | INFO | |
| | 解码失败（**必须限流**） | WARN | 高频源，见 §7 F-4 |
| | 传感器失联 | WARN | |

---

## 3. 统一 Event ID 规范（Event Registry）

### 3.1 编号规则（与实现一致）

```
  0x MM EE
     │  │
     │  └── event 序号：0x01 .. 0xFF（每模块最多 255 个）
     └───── module 段：0x01 .. 0x0F（高字节）
```

- 类型：`enum LogEventId : uint16_t`（`log_events.h:212`）。
- 新增事件 = **在所属段内取下一个未用序号**，不得跨段、不得复用。
- `LOG_EVT_NONE = 0x0000` 保留。

### 3.2 已冻结分配表（**实际代码，权威**）

| 段 | 模块 | 现有事件（节选） | 已用末号 |
|---|---|---|---|
| `0x01xx` | **System / Boot / Restart** | BOOT_COMPLETE, BOOT_INCOMPLETE_PREV, RESET_ABNORMAL, RESET_NORMAL, INIT_FAILED, FS_MOUNT_FAILED, PSRAM_ALLOC_FAILED, HEAP_LOW, RESTART_*, CRITICAL_OP_UNDERFLOW | 0x010C |
| `0x02xx` | **Config** | LOAD_DONE, MODULE_LOAD_FAILED, CHANGE_APPLIED, SAVE_OK, COMMIT_FAILED_ROLLBACK, RECOVERED_FROM_BACKUP, VERSION_REBUILT, FACTORY_RESET, WRITE_REJECTED, RESTART_TIMEOUT | 0x020A |
| `0x03xx` | **Storage** | FS_UNAVAILABLE, ATOMIC_WRITE_FAILED, CRC_FAILED, TXN_RECOVERED, WRITE_VERIFY_FAILED, READ_FAILED | 0x0306 |
| `0x04xx` | **Workflow** | START, FINISHED, TIMEOUT, FAILED, ACTION_FAILED, SAVE_FAILED, SAVE_PARTIAL, CRUD, MIGRATED, TEMP_ACTION_TIMEOUT, RUNTIME_ALLOC_FAILED, SAVE_PARTIAL_RETRY_OK | 0x040C |
| `0x05xx` | **Water（Dispense / Valve / Weight）** | DISPENSE_*, VALVE_FORCE_CLOSE(_FAILED), VALVE_OVERFLOW_RISK, VALVE_SAFETY_TIMEOUT, VALVE_OPEN/CLOSE, VALVE_RATE_LIMITED, WEIGHT_ERROR_ENTER/EXIT, WEIGHT_ZERO_DONE, WEIGHT_TRIGGER_FIRED, WEIGHT_CALIB_FAILED | 0x0510 |
| `0x06xx` | **WiFi** | CONNECT_START, CONNECTED, CONNECT_TIMEOUT, LOST, RECONNECT_TRY, PROVISION_ENTER, PROVISION_DONE | 0x0607 |
| `0x07xx` | **MQTT / Cloud / Log 自身** | MQTT_CONNECTED/DISCONNECTED/SLEEP_ENTER/PUBLISH_FAIL/CMD_EXEC_FAILED, CLOUD_FRAG_FAIL, LOG_UPLOAD_FAIL, LOG_ACK_TIMEOUT, LOG_RING_OVERFLOW, LOG_ACK_LOST, LOG_SELF_DEGRADED | 0x070B |
| `0x08xx` | **Time / RTC** | NTP_OK/FAIL, TIME_VALID_ENTER/INVALID_ENTER, RTC_PROBE, RTC_BOOT_RESTORE, RTC_CALIBRATED, RTC_WRITE_FAILED, RTC_VL_FLAG, RTC_BCD_INVALID | 0x080A |
| `0x09xx` | **Mijia BLE** | DATA_DECODED, DECODE_FAIL, SENSOR_LOST, SCAN_DISABLED | 0x0904 |
| `0x0Axx` | **Command** | RUNTIME_QUEUE_FULL, RUNTIME_TIMEOUT, REJECTED, APPLIED | 0x0A04 |
| `0x0Bxx` | **Capability Registry** | REBUILT, SAVE_FAILED | 0x0B02 |
| `0x0Cxx` | **Event Manager** | EVT_QUEUE_FULL, EVT_STORM_DROPPED | 0x0C02 |
| `0x0Dxx` | **ComputerReset** | PULSE, SAFETY_TIMEOUT, POOL_EXHAUSTED | 0x0D03 |
| `0x0Exx` | **OLED** | OLED_INIT_FAILED | 0x0E01 |
| `0x0Fxx` | **Motor（未来模块，已预留）** | `LOG_MOTOR_RESERVED_BASE = 0x0F00` | — |

### 3.3 ⚠️ 与任务提案的差异（**必须知悉**）

| 你提案 | 实际冻结 | 结论 |
|---|---|---|
| `0x02xx` WiFi | `0x02xx` **Config**（WiFi 在 `0x06xx`） | **沿用实际** |
| `0x03xx` Workflow | `0x03xx` **Storage**（Workflow 在 `0x04xx`） | **沿用实际** |
| `0x04xx` Valve | `0x04xx` **Workflow**（Valve 在 `0x05xx`，与 Water/Weight 同段） | **沿用实际** |
| `0x05xx` Weight | `0x05xx` **Water 段**（Dispense+Valve+Weight 合并） | **沿用实际** |
| `0x06xx` BLE | `0x06xx` **WiFi**（BLE 在 `0x09xx`） | **沿用实际** |
| `0x07xx` Cloud | `0x07xx` **Cloud/MQTT/Log 自身** ✅ 一致 | 一致 |

**为何不能改成你的提案**：EventId 已写入 128 B Record 的 `event_id` 字段并随 CBOR 上云；改动会使**已落盘的历史段与云端解析全部错位**，且违反「不修改已冻结 ABI」约束。
⇒ 若确实需要"按模块重新分段"，只能在**下一次契约修订**中作为**破坏性变更**处理（需 Flash 格式版本 + 云端双解析过渡）。**P2 前置阶段不做。**

### 3.4 如何新增一个 Event

1. 确认所属**模块段**（§3.2）；Valve/Weight 属 `0x05xx` Water 段。
2. 在段内取**下一个未用序号**（例：新增"阀门驱动无响应" → `0x05xx` 段下一个是 `0x0511`）。
3. 命名：`LOG_<MODULE>_<ACTION>`，全大写下划线。
4. 在 `enum LogEventId` 中**追加在所属段末尾**（不得插入中间，避免语义漂移）。
5. 注释里标注**建议 Level**（`// INFO` / `// WARN` …），与 §2.2 规则一致。
6. 若需要新参数 → 同步在 `enum LogParamId` 追加（见 §5.4）。
7. **不得**：跨段编号、复用已废弃号、使用 `0x0000`、使用 `0x0F00` 以下非本段号。

### 3.5 如何避免冲突

- 单一真相源：`src/log_events.h` 的 `enum LogEventId`。**模块不得自带私有事件号**。
- `log_events.h` 内含 `static_assert` 与契约测试（`test/log_contract/probe_ok.cpp`）**编译期**校验 ID 值；新增后须跑：
  ```bash
  python test/log_contract/run_contract_test.py
  ```
- 命名前缀即模块归属（`LOG_VALVE_*` 必须落在 `0x05xx`），**前缀与段不符即视为错误**。

### 3.6 如何保持 Cloud 可解析

- Cloud 侧持有一份 **`EventId → (名称, Level, 参数字典)` 映射表**，随固件契约版本发布。
- `event_id` 是 `uint16` 随记录原样上云 ⇒ **新增事件不会破坏旧解析**（旧云端遇到未知 ID 应记录为 `unknown:<hex>` 而非报错）。
- **改语义（同 ID 换含义）是破坏性变更，禁止**；要改就新开 ID。

---

## 4. Log API 使用方式（**按实际代码**）

### 4.1 实际公开 API（`src/log_manager.h`）

**正式记录接口（模块唯一可用）：**

```cpp
// 带参数
bool log_emit(LogEventId event_id,
              LogLevel level,
              const LogParamIn *params,
              uint8_t param_count);

// 无参数（inline 包装）
inline bool log_emit0(LogEventId event_id, LogLevel level);
```

**参数构造 helper（避免误用 union 成员）：**

```cpp
inline LogParamIn log_arg_i32 (uint8_t pid, int32_t  v);
inline LogParamIn log_arg_u32 (uint8_t pid, uint32_t v);
inline LogParamIn log_arg_f32 (uint8_t pid, float    v);
inline LogParamIn log_arg_bool(uint8_t pid, bool     v);
inline LogParamIn log_arg_enum(uint8_t pid, uint32_t v);
```

**生命周期（仅 `main.cpp` 调用，模块不得调用）：**

```cpp
bool log_init();   // main.cpp:80 —— 必须在 loop() 之前
void log_task();   // main.cpp:213（loop 内）
```

**观测 / 测试钩子（`log_flash_*` / `log_meta_*` / `log_seg_*` / `log_cloud_test_*`）：**
仅上板自测与串口控制台使用，**业务模块禁止调用**。

### 4.2 ★ 不存在 `LOG_INFO()` 宏 —— 不要写成那样

任务描述中的示例：

```cpp
LOG_INFO(EVENT_VALVE_OPEN);   // ❌ 当前代码没有这个宏
```

**正确写法**（本项目实际形态）：

```cpp
log_emit0(LOG_VALVE_OPEN, LOG_LVL_INFO);
```

是否提供宏包装属于 **Future Improvement**（§7 F-1），本阶段**不新增**。

### 4.3 调用示例（可直接照抄）

**Valve —— 无参数**

```cpp
#include "log_manager.h"

void valve_open()
{
    // ... 硬件动作 ...
    log_emit0(LOG_VALVE_OPEN, LOG_LVL_INFO);
}

void valve_close()
{
    // ... 硬件动作 ...
    log_emit0(LOG_VALVE_CLOSE, LOG_LVL_INFO);
}
```

**Valve —— 带参数（安全超时）**

```cpp
void valve_safety_timeout(uint32_t open_ms, uint32_t limit_ms)
{
    LogParamIn p[2];
    p[0] = log_arg_u32(LOG_P_OPEN_MS,  open_ms);
    p[1] = log_arg_u32(LOG_P_LIMIT_MS, limit_ms);

    log_emit(LOG_VALVE_SAFETY_TIMEOUT, LOG_LVL_WARN, p, 2);
}
```

**Valve —— CRITICAL（强制关阀，必须留证）**

```cpp
void valve_force_close(const char *cause_enum)
{
    // cause 建议做成枚举常量，避免字符串
    LogParamIn p[1];
    p[0] = log_arg_enum(LOG_P_CAUSE, (uint32_t)VALVE_CAUSE_SAFETY);

    log_emit(LOG_VALVE_FORCE_CLOSE, LOG_LVL_CRITICAL, p, 1);
}
```

**Weight —— 标定失败（ERROR + 浮点参数）**

```cpp
void weight_calibrate_done(bool ok, float factor)
{
    if (!ok) {
        LogParamIn p[1];
        p[0] = log_arg_f32(LOG_P_OFFSET, factor);
        log_emit(LOG_WEIGHT_CALIB_FAILED, LOG_LVL_ERROR, p, 1);
        return;
    }
    log_emit0(LOG_WEIGHT_ZERO_DONE, LOG_LVL_INFO);
}
```

**Weight —— 状态变化（进/出异常态）**

```cpp
// 只在"边"上打，不在每次循环里打
static bool s_w_err = false;

void weight_refresh_error_state(bool now_err, int32_t raw, int32_t filtered)
{
    if (now_err == s_w_err) {
        return;                       // ★ 去重：状态没变就不记
    }
    s_w_err = now_err;

    LogParamIn p[2];
    p[0] = log_arg_i32(LOG_P_RAW,      raw);
    p[1] = log_arg_i32(LOG_P_FILTERED, filtered);

    log_emit(now_err ? LOG_WEIGHT_ERROR_ENTER : LOG_WEIGHT_ERROR_EXIT,
             now_err ? LOG_LVL_WARN : LOG_LVL_INFO,
             p, 2);
}
```

**WiFi —— 状态迁移**

```cpp
void wifi_on_state_changed(wifi_state_t from, wifi_state_t to)
{
    LogParamIn p[2];
    p[0] = log_arg_enum(LOG_P_WAS,   (uint32_t)from);
    p[1] = log_arg_enum(LOG_P_STATE, (uint32_t)to);

    LogEventId ev;
    LogLevel   lv;

    switch (to) {
    case WIFI_CONNECTED:    ev = LOG_WIFI_CONNECTED;      lv = LOG_LVL_INFO; break;
    case WIFI_CONNECTING:   ev = LOG_WIFI_CONNECT_START;  lv = LOG_LVL_INFO; break;
    case WIFI_DISCONNECTED: ev = LOG_WIFI_LOST;           lv = LOG_LVL_WARN; break;
    default: return;
    }

    log_emit(ev, lv, p, 2);
}
```

**Workflow —— 启动 / 完成**

```cpp
void workflow_start(uint8_t slot)
{
    LogParamIn p[1];
    p[0] = log_arg_u32(LOG_P_SLOT, slot);
    log_emit(LOG_WF_START, LOG_LVL_INFO, p, 1);
}

void workflow_finish(uint8_t slot, uint32_t duration_ms, bool ok)
{
    LogParamIn p[3];
    p[0] = log_arg_u32(LOG_P_SLOT,        slot);
    p[1] = log_arg_u32(LOG_P_DURATION_MS, duration_ms);
    p[2] = log_arg_enum(LOG_P_STOP_REASON, ok ? WF_STOP_DONE : WF_STOP_FAILED);

    log_emit(ok ? LOG_WF_FINISHED : LOG_WF_FAILED,
             ok ? LOG_LVL_INFO : LOG_LVL_WARN,
             p, 3);
}
```

**System —— 启动完成（带启动耗时）**

```cpp
void system_boot_complete(uint32_t init_ms, uint8_t loaded, uint8_t total)
{
    LogParamIn p[3];
    p[0] = log_arg_u32(LOG_P_INIT_MS, init_ms);
    p[1] = log_arg_u32(LOG_P_LOADED,  loaded);
    p[2] = log_arg_u32(LOG_P_TOTAL,   total);

    log_emit(LOG_SYS_BOOT_COMPLETE, LOG_LVL_INFO, p, 3);
}
```

### 4.4 调用上下文约束（**必须遵守**）

| 约束 | 说明 |
|---|---|
| **必须在 `log_init()` 之后** | `log_init()` 位于 `main.cpp:80`（`LittleFS.begin()` 之后、`json_storage_init()` 之前）。之前调用 `log_emit()` 会安全返回 `false`。 |
| **任务上下文，禁止 ISR** | `log_emit()` 使用 `portENTER_CRITICAL()`（**非 `..._ISR` 变体**），**不得在中断服务程序 / GPIO ISR 中调用**。ISR 中请置标志，由任务侧记录。 |
| **非阻塞** | `log_emit()` 只做 128 B 拷贝 + 临界区计数，无 Flash I/O、无 MQTT、无 `delay`。可在任意任务中调用。 |
| **可被并发调用** | 多任务安全（`s_mux` 保护）；但**单条记录的参数数组必须在调用方栈上、调用期间有效**。 |
| **返回值可忽略** | 返回 `false` 表示未入环（DEBUG 丢弃 / 环未就绪 / 参数超限）。**业务不得因返回值改变控制流**。 |
| **禁止构造 String** | 需要标识一律用 `LogParamId` + 标量 / 枚举；**不得传字符串**（当前 API 也不支持，见 §7 F-2）。 |

---

## 5. 参数 payload 规范

### 5.1 参数结构

```cpp
struct LogParamIn          // 调用方栈上，sizeof == 8（含 2 B padding）
{
    uint8_t id;            // LogParamId
    uint8_t type;          // LogParamType
    union { int32_t i; uint32_t u; float f; uint8_t b; } v;
};
```

落盘 / 上云时压缩为 **6 B/项**：`{ id:u8, type:u8, value:u32le }`。

### 5.2 数量限制

- **最多 8 个**（`LOG_MAX_PARAMS = 8`，Record 内 `params[8]` = 48 B）。
- **`param_count > 8` → 整条拒绝**（`log_manager.cpp:1461`），**不截断**。
  > 这是刻意的：静默截断会造成 silent data loss。

### 5.3 类型限制

| `LogParamType` | 值 | helper | 说明 |
|---|---|---|---|
| `LOG_PTYPE_I32` | 1 | `log_arg_i32` | 有符号 32 位 |
| `LOG_PTYPE_U32` | 2 | `log_arg_u32` | 无符号 32 位 |
| `LOG_PTYPE_F32` | 3 | `log_arg_f32` | 浮点（如重量、系数） |
| `LOG_PTYPE_BOOL` | 4 | `log_arg_bool` | 布尔 |
| `LOG_PTYPE_ENUM` | 5 | `log_arg_enum` | **枚举 / 状态码，推荐替代字符串** |
| `LOG_PTYPE_STR` | 6 | **❌ 无 helper** | 值 = `(blob_off << 16) | blob_len`，需 blob——当前公开 API **不可达**（§7 F-2） |
| `LOG_PTYPE_I8` | 7 | **❌ 无 helper** | 需手动构造 |
| `LOG_PTYPE_U16` | 8 | **❌ 无 helper** | 需手动构造 |

> **实际可用（有 helper）**：`i32 / u32 / f32 / bool / enum` 五种。

### 5.4 ParamId 复用原则

- ParamId 是**全局共享字典**（`0x00..0x58`），**跨事件复用**，不要为同一语义重复造 ID。
- 已有可直接复用的常用项：

| 语义 | ParamId |
|---|---|
| 槽位 / 编号 | `LOG_P_SLOT` (0x01) |
| 耗时 | `LOG_P_DURATION_MS` (0x06) |
| 超时阈值 | `LOG_P_TIMEOUT_MS` (0x08) |
| 错误码 | `LOG_P_ERR_CODE` (0x0C) |
| 重量（克） | `LOG_P_WEIGHT_G` (0x15) |
| 原因 / 枚举 | `LOG_P_CAUSE` (0x16)、`LOG_P_REASON` (0x1F) |
| 模块名 | `LOG_P_MODULE` (0x1D) |
| 状态 / 旧值 | `LOG_P_STATE` (0x44)、`LOG_P_WAS` (0x47) |
| 重试次数 | `LOG_P_RETRY_N` (0x29) |
| RSSI | `LOG_P_RSSI` (0x31) |
| 温度 / 湿度 / 电量 | `LOG_P_TEMP` (0x3B) / `LOG_P_HUMID` (0x3C) / `LOG_P_BATT_V` (0x3D) |

- **新增 ParamId**：已分配至 `0x58`（`LOG_P_REMAIN_MS`）；`LOG_P_MAX = 0x59` 为边界哨兵。
  ⇒ 下一个可用号**需在 P2 契约评审中确认**（`0x59` 是否可作普通 ID，或同步上调 `LOG_P_MAX`）。**本阶段不自行分配。**

### 5.5 Flash 空间考虑

- 每条 Record **恒 128 B**，与参数数量无关 ⇒ **不填参数不省空间**。
- 真正影响 Flash 的是 **Level**：`INFO` 不落盘 ⇒ INFO 参数再多也不占 Flash（只占 Cloud 带宽）。
- `WARN+` 才落盘；段容量 496 条。按"WARN+ ≤ 数十条/日"估算，无需为单条参数做极致压缩。

### 5.6 Cloud 查询需求

- 参数应该是**可过滤、可聚合的标量**，不要打包成位域或字符串。
- 同一语义在不同事件里**必须用同一个 ParamId**（否则云端无法跨事件查询，如"所有带 `LOG_P_SLOT` 的事件"）。
- 枚举值建议在项目内维护**常量表**（`WF_STOP_DONE` 等），并在契约文档中同步给云端。

### 5.7 正例 / 反例

**❌ 反例**（字符串化、语义丢失）

```
"Valve error"                        // 无 EventId、无参数、不可查询
```

**✅ 正例**

```
event_id : LOG_VALVE_SAFETY_TIMEOUT  (0x0508)
level    : WARN
params   : { LOG_P_OPEN_MS  = 5230 (u32) }
           { LOG_P_LIMIT_MS = 5000 (u32) }
```

---

## 6. 模块接入顺序建议（**只建议，不改代码**）

### 6.1 先纠正一处依赖认知

你在任务中给出的链：

```
System State → Event Manager → LogManager → …
```

**实际依赖方向不是这样**，而且这是**刻意设计**：

- `LogManager` **不依赖** `System State`（State Context 机制已定义但**未通过公开 API 暴露**，见 §7 F-3）；
- `LogManager` **不依赖** `Event Manager`（否则 Event 打 Log、Log 又依赖 Event ⇒ 递归风险）；
- 真实方向是**反向**：`System State / Event Manager → LogManager`（它们向 LogManager 记录自己的异常，如 `LOG_EVT_QUEUE_FULL`）。

⇒ 因此 **LogManager 已在 P1.2 完成初始化（`main.cpp:80`），不阻塞任何模块**。接入链从"谁产生日志"开始即可。

### 6.2 建议顺序与理由

| # | 模块 | 理由 |
|---|---|---|
| **1** | **System / Boot**（`0x01xx`） | 零依赖；先建立"设备为什么起来"这条最关键的诊断主线（`BOOT_COMPLETE` / `RESET_ABNORMAL` / `PSRAM_ALLOC_FAILED`）。**先做它，之后所有其它模块的排障都有锚点。** |
| **2** | **Config + Storage**（`0x02xx` / `0x03xx`） | 二者是"配置改错了 / 写坏了"的根因来源；且它们的失败往往是其它模块异常的**上游原因**。依赖：无（LogManager 已完成）。 |
| **3** | **WiFi**（`0x06xx`） | 决定 Cloud 是否可用。先有连通性日志，Cloud 日志才有解释上下文。依赖：无。 |
| **4** | **MQTT / Cloud**（`0x07xx`） | 依赖 WiFi。此时可验证"链路通 → Log 能上云"闭环。 |
| **5** | **Time / RTC**（`0x08xx`） | 影响所有日志的 `timestamp_valid`。建议**早于**业务模块，但可并行；放在 WiFi 之后是因为 NTP 依赖网络。 |
| **6** | **Workflow**（`0x04xx`） | 依赖 Config（配置加载）+ 各 Action。业务主线，日志量大，需先确认前 5 步稳定。 |
| **7** | **Water 段：Dispense / Valve**（`0x05xx`） | 依赖 Workflow（被其调度）+ Weight（反馈）。**安全相关（CRITICAL 事件最多）**，放在核心链路验证之后。 |
| **8** | **Weight**（`0x05xx` 同段） | 与 Valve 同属 Water 段；建议在 Valve 之前或同时，因为 Dispense 需要重量反馈。**注意状态变化要去重**（§4.3 示例）。 |
| **9** | **BLE / Mijia**（`0x09xx`） | 高频源（解码失败可能每秒数十次）。**最后做**，且**必须自带限流**（见 §7 F-4），否则会淹没 RAM 环、挤出真正的 WARN+。 |
| **10** | **Command / Registry / Event / ComputerReset / OLED** | 剩余低风险模块，独立无依赖，可并行收尾。 |

> **与你的提案差异**：你给的是 `…→ Valve → Weight → BLE`；建议**先 Weight 后 Valve**（或同批），因为 Dispense 的正确性依赖重量反馈，先有 Weight 日志才能解释 Valve 的超时/强制关阀。

### 6.3 每步验收（建议）

1. 编译通过（不新增 warning）；
2. `python test/log_contract/run_contract_test.py` → `CONTRACT TEST: ALL PASS`；
3. 上板：触发该模块的正常 / 异常 / 边界三条路径，用 `logt stats` 确认 `emit_total` 增长、`ring_drop == 0`；
4. 高频模块额外确认：`ring_high_water` 未打满 64。

---

## 7. 接入能力评估与 Future Improvement（**只列不修**）

### 7.1 现有能力检查表

| 检查项 | 现状 | 判定 |
|---|---|---|
| **现有 API 是否足够支持业务模块？** | `log_emit` + `log_emit0` + 5 个 `log_arg_*` 覆盖"事件 + Level + ≤8 个标量参数" | ✅ **基本够用**，可立即开始 P2 接入 |
| **module id** | 无独立字段；可用 `LOG_P_MODULE` (0x1D) 作为参数传递 | ⚠️ 非强制，靠 `event_id` 高字节已隐含模块 |
| **event id** | 已冻结 15 个模块段 + `static_assert` 校验 | ✅ 完备 |
| **payload helper** | `i32/u32/f32/bool/enum` 五个；**无 str / i8 / u16** | ⚠️ 见 F-2 |
| **macro wrapper**（`LOG_INFO()` 等） | **不存在** | ❌ 见 F-1 |
| **rate limit** | **无任何限流机制** | ❌ 见 F-4 |
| **log enable 控制** | 仅编译期 `LOG_DEBUG_ENABLE`；**无运行时开关** | ⚠️ 见 F-5 |
| **ISR 安全** | 无 `FromISR` 变体 | ⚠️ 见 F-6（文档已约束禁止） |

### 7.2 Future Improvement 清单

| 编号 | 项 | 说明 | 建议优先级 |
|---|---|---|---|
| **F-1** | **宏包装缺失** | 现状调用较啰嗦：`log_emit(LOG_X, LOG_LVL_WARN, p, 2)`。建议后续提供 `LOG_W(evt)` / `LOG_W1(evt,pid,v)` 等宏（**注意**：不能命名为 `LOG_LEVEL_*`，与 NimBLE 宏冲突；也不能用 `LOG_INFO` 这类需评估冲突） | 中 |
| **F-2** | **字符串 / blob 参数不可达** | `LOG_PTYPE_STR` 已定义（值 = `blob_off<<16|len`，字符存 blob），但 `LogParamIn` **无 blob 字段**、`log_emit()` **无 blob 形参** ⇒ STR/I8/U16 三种类型**当前无法使用**。要么补 API，要么从契约中移除这三种类型以免误导 | 中 |
| **F-3** | **State Context 未暴露** | `LogContextKind`（SYSTEM/WORKFLOW/DISPENSE/WEIGHT/MQTT/STRING）与 `LOG_FLAG_CONTEXT_PRESENT` 已在 Record 中定义，但**公开 API 无设置入口**。若要自动附带上下文（如 boot_seq / wifi 状态），需新增 API | 低（可先靠参数手工携带） |
| **F-4** | **无限流（rate limit）** | 高频源（BLE 解码失败、HX711 采样异常、MQTT 重连）可能每秒数十条，会挤占 64 槽 RAM 环、挤出真正的 WARN+。当前只能靠**模块侧自己去重/计数**（如 §4.3 的 `s_w_err` 边沿判定）。建议在 LogManager 侧提供 per-EventId 的最小间隔 | **高**（BLE/Weight 接入前最好有） |
| **F-5** | **无运行时开关** | 只有编译期 `LOG_DEBUG_ENABLE`。现场排障若想临时关某类日志需重编固件。建议后续接 `log.json` Config 模块（注意：受「Config 改动需重启生效」既有约定约束，且 Q5 未决） | 低 |
| **F-6** | **无 ISR 变体** | `log_emit()` 用 `portENTER_CRITICAL()`，非 ISR 安全。当前以文档约束"禁止 ISR 调用"规避；若将来确实需要 ISR 记录（如紧急关阀中断），需另设计 `log_emit_from_isr()` | 低 |
| **F-7** | **ParamId 边界未定** | `LOG_P_MAX = 0x59` 与"下一个可用号"关系未明确（见 §5.4） | 中（P2 开工前需定） |
| **F-8** | **无"模块已接入"自证机制** | 没有类似 `capability registry` 的能力登记，云端无法知道设备哪个模块已具备日志。可随 `LOG_REG_*` 扩展 | 低 |

### 7.3 不建议现在做的

- ❌ 修改 `log_events.h` 的 ABI / EventId 分段（会破坏 Flash 与云端解析）；
- ❌ 新增 Macro wrapper（属 API 变更，应单独评审；且需先解决 NimBLE 命名冲突评估）；
- ❌ 让 LogManager 依赖 System State / Event Manager（引入循环依赖与递归风险）；
- ❌ 引入独立 Log Task 或阻塞等待（破坏 LittleFS 单写者模型）。

---

## 8. 需要冻结的接口清单（P2 开工前必须锁定）

以下接口在 **P2 模块接入期间不得变更**；任何变更需走契约修订流程：

| # | 冻结项 | 来源 |
|---|---|---|
| 1 | `bool log_emit(LogEventId, LogLevel, const LogParamIn*, uint8_t)` | `log_manager.h:198` |
| 2 | `inline bool log_emit0(LogEventId, LogLevel)` | `log_manager.h:201` |
| 3 | `log_arg_i32 / u32 / f32 / bool / enum`（签名与语义） | `log_manager.h:210-253` |
| 4 | `enum LogLevel`（`LOG_LVL_*`，值 0..4） | `log_events.h:33` |
| 5 | **Level Policy 表**（Flash/Cloud 五档，无 EventId 例外） | `log_events.h:64` |
| 6 | `enum LogEventId` 全部已分配值 + 模块段划分 | `log_events.h:212-346` |
| 7 | `enum LogParamId` 全部已分配值（`0x00..0x58`） | `log_events.h:109-201` |
| 8 | `LogRecord` v2 / 128 B 布局与偏移常量 | `log_events.h:408-429` |
| 9 | `LOG_MAX_PARAMS = 8`、超限**整体拒绝**语义 | `log_events.h:410` |
| 10 | Flash Segment：3984 B / 31 条 / 16 段 / 段头 16 B | `log_events.h:479-483` |
| 11 | Cloud Batch：`fmt=2` / 12 键 / ≤16 条 / ≤4096 B / 500 ms | `log_events.h:512-515` |
| 12 | ACK：`log_ack` 命令名 + `b/f/t` 字段名 + 15 s / 5 次 / 60 s 上限 | `log_events.h:520-546` |
| 13 | `at-least-once` + 云端 `(device_id, boot_seq, seq)` 幂等义务 | 冻结契约 |
| 14 | **GIVE_UP_NOT_ADVANCE** 语义（不推进水位、不删段） | P1.5 冻结 |
| 15 | `log_init()` / `log_task()` 的调用位置与"无独立 Task"约束 | `main.cpp:80/213` |
| 16 | **业务模块禁止调用的清单**：所有 `log_flash_*` / `log_meta_*` / `log_seg_*` / `log_cloud_test_*` | `log_manager.h:299-435` |

---

## 9. Storage 回调桥接（**P2-A 已落地，首个接入模块**）

> 状态：已实现并上板验证（`src/main.cpp` 桥接段 + `setup()` 注册）。
> 验证记录：`docs/LogManager-P1.5-Board-Test-Report0918.md` §Storage bridge
> 提交：`feat(log): connect storage callbacks to LogManager`

### 9.1 背景：为什么需要"桥接"而不是"直接改模块"

`json_storage` 与 `file_storage` **本来就带日志回调接口**，但**从未被注册**：

| 模块 | 回调 setter | E/W 调用点 | 接入前的实际行为 |
|---|---|---|---|
| `json_storage` | `json_storage_set_log_callback()` | **37** 处（`js_log("E")` 30 + `js_log("W")` 7） | `s_log_cb == nullptr` ⇒ 直接 `return`，**完全静默** |
| `file_storage` | `file_storage_set_log_callback()` | **30** 处（`fs_log("E")` 24 + `fs_log("W")` 6） | 同上 ⇒ **完全静默** |

⇒ 合计 **67 条错误/警告从未被人看到**。因此 P2 的第一个接入动作**不是改模块**，
而是在 `main.cpp` 注册一个"桥接回调"，把既有回调转成 `log_emit()`。

**这样做的收益**：零业务代码改动、零接口变更、零冻结契约影响。

### 9.2 注册方式（**位置很关键**）

```cpp
// src/main.cpp  setup()
log_init();                                        // ← 必须在前（RAM 环就绪）
json_storage_set_log_callback(json_storage_log_bridge);   // ← 本桥接注册
file_storage_set_log_callback(file_storage_log_bridge);
if (!json_storage_init()) { ... }                  // ← 各 Storage 自己的 init
if (!bin_storage_init())  { ... }                  //   （file_storage_init 在其中被级联调用）
```

**为什么必须注册在 `log_init()` 之后、各 Storage `init()` 之前**：

1. 在 `log_init()` 之前 ⇒ `log_emit()` 会因 `s_ready == false` 直接丢弃；
2. 在各 Storage `init()` 之前 ⇒ 才能捕获**初始化期**的 Storage 错误（否则前几条必然漏掉）。
3. 两个 setter 都只是 `s_log_cb = callback;`（纯指针赋值），**不依赖模块已 init**，可安全前置。

> ⚠️ `bin_storage` 的既有回调（`bin_log_serial`）**保持只走串口**，本次未改动。
> 如需三路（串口 + LogManager）另开一次提交，避免混主题。

### 9.3 语义还原：回调只给自由文本，EventId 靠 op 前缀分类

回调签名是 `void (*)(const char *level, const char *message)` ——
**只有等级字符和一句自由文本**，没有结构化的 op/路径/错误码。因此桥接层做三件事：

| 步骤 | 做法 |
|---|---|
| ① 取等级 | `'E'` → `LOG_LVL_ERROR`；`'W'` → `LOG_LVL_WARN`；**`'I'` 不上报**（Storage 只有一条 `"ready"`，无诊断价值） |
| ② 分类 op | 对 `message` 做**定长前缀匹配**（`strncmp`/`strstr`，不构造 `String`），得到 `StgBridgeOp` |
| ③ 映射 EventId | `op → Storage 段（0x03）已冻结 EventId`；`op` 明细写入 `LOG_P_ERR_CODE` |

**op → EventId 映射表（全部为已冻结 ID，未新增）**

| op（message 前缀） | EventId | Level |
|---|---|---|
| `LittleFS unavailable` | `LOG_STG_FS_UNAVAILABLE` (0x0301) | **CRITICAL**（按冻结契约，触发立即 flush） |
| `…not initialized` | `LOG_STG_FS_UNAVAILABLE` (0x0301) | 跟随回调（`E` → ERROR）；用 `LOG_P_ERR_CODE` 区分 |
| `crc32` | `LOG_STG_CRC_FAILED` (0x0303) | 跟随回调 |
| `verify` | `LOG_STG_WRITE_VERIFY_FAILED` (0x0305) | 跟随回调 |
| `recover` | `LOG_STG_TXN_RECOVERED` (0x0304) | 跟随回调 |
| `rename` / `write` / `mkdir` / `remove` | `LOG_STG_ATOMIC_WRITE_FAILED` (0x0302) | 跟随回调 |
| `read` / `foreach` / `exists` / `size` / `seek` / `truncate` | `LOG_STG_READ_FAILED` (0x0306) | 跟随回调 |
| 未识别（兜底） | `E` → `LOG_STG_ATOMIC_WRITE_FAILED`；`W` → `LOG_STG_READ_FAILED` | 跟随回调 |

**参数（只用现有 ParamId，未新增）**

| ParamId | 取值 | 说明 |
|---|---|---|
| `LOG_P_MODULE` (0x1D) | `0` = json_storage，`1` = file_storage | 复用为"存储层"标识 |
| `LOG_P_ERR_CODE` (0x0C) | `StgBridgeOp` 枚举值（0=unknown, 1=fs_unavailable, 2=not_initialized, 3=mkdir, 4=remove, 5=rename, 6=read, 7=write, 8=crc32, 9=verify, 10=foreach, 11=exists, 12=size, 13=recover, 14=seek/truncate） | **真实 op 语义靠它还原** |
| `LOG_P_COUNT` (0x45) | 被抑制的重复条数（仅当 > 0 时携带） | 见 §9.4 |

> ⚠️ **字符串参数约束**：`LOG_P_PATH` / `LOG_P_KEY` 等字符串语义 ParamId **不可用**
> （`LogParamIn` 无 blob 字段，`log_param_type` 的 `STR/I8/U16` 无法构造）。
> 因此**文件路径没有进日志** —— 这是本桥接最大的信息损失。
> 若需要路径，应把回调改为结构化（`level + op + path_hash + err_code`），
> 属 **API 变更**，需单独评审（对应 §7.2 的 F-2）。

### 9.4 高频抑制（防止"FS 不可用"按调用次数刷屏）

**风险**：FS 未挂载时，每次 `read/write` 都会产生一条 `"...: not initialized"`。
若不过滤，一次业务调用链就能灌满 64 槽 RAM 环，把真正的 WARN+ 挤掉。

**方案**：同一 `(module, op)` 在 **10 s 窗口**内只上报 **1** 条，其余累加计数；
下个窗口首次上报时用 `LOG_P_COUNT` 携带被抑制的条数。

```cpp
static unsigned long s_stg_last_ms [2][STG_OP_COUNT];   // 204 B 静态，零堆分配
static uint16_t      s_stg_suppressed[2][STG_OP_COUNT];
```

**语义边界（必须知悉）**：
- 抑制只作用于 **LogManager 路径**；**串口输出不做抑制**（保留完整原始信息，便于现场抓包）；
- 若错误在窗口结束前就停止，则累计数**不会**被上报（计数丢失可接受，因为事件本身已上报过一次）；
- 首次出现（`last == 0`）**一定上报**，保证"第一次"永不丢失。

### 9.5 生命周期

| 阶段 | 行为 |
|---|---|
| `setup()` 注册后 ~ 首次错误 | 计数器全 0 ⇒ **首次即刻上报** |
| 同一 `(module, op)` 10 s 内再次出现 | 只累加 `s_stg_suppressed`，不产生新记录 |
| 窗口到期后再次出现 | 上报 1 条，携带 `LOG_P_COUNT = 被抑制条数`，并清零 |
| **进程生命周期** | 抑制表为 `static`，**随重启清零**（不做持久化）。这是刻意选择：抑制是"限流"，不是"状态"，重启后应重新可见 |
| `log_init()` 未调用时 | 桥接会被注册但 `log_emit()` 返回 false（安全丢空），串口输出不受影响 |

### 9.6 上板验证结果（P2-A）

真实 Storage 错误（`/config/.commit` 不存在）在 **4 次启动**中稳定复现，全链路打通：

```
[JStg][I] ready                            ← INFO：桥接正确"不上报"
[FStg][I] ready                            ← INFO：同上
[JStg][W] read: open failed: /config/.commit   ← WARN：串口双路输出
[LogT] stats  emit=1 ... flash=1 cloud=1   ← 进入 RAM 环 → Flash + Cloud 两路
[LogT] fstats ok=1                         ← 成功落盘
[LogT] cstats batch=1 ... qused=1          ← 进入 Cloud 队列
[Cloud LOG] OK topic=guo_feeder/log len=147 ← ★ 真实发到 MQTT 日志主题
[LogT] cloud  boot=N from=N to=N n=1       ← 批次含该条记录
```

**未验证项（如实记录）**：
- `ERROR` / `CRITICAL` 分支：现有串口钩子（`wfst del` 等）对缺失文件返回 OK，**不产生 fs 级错误**，
  无法在不给 storage 模块加测试钩子的前提下触发 ⇒ 本次未验证（E 分支与 W 分支代码对称，风险低）。
- 10 s 抑制窗口：无法在 10 s 内制造同一 `(module, op)` 的重复错误 ⇒ 本次未验证。
- 以上两项留待后续用"结构化回调 + 故障注入"一并解决（见 §7.2）。

---

## 附录 A：EventId 全表（按段，来自 `src/log_events.h:212-346`）

```
0x0101 SYS_BOOT_COMPLETE        INFO      0x0102 SYS_BOOT_INCOMPLETE_PREV  CRITICAL
0x0103 SYS_RESET_ABNORMAL       CRITICAL  0x0104 SYS_RESET_NORMAL          INFO
0x0105 SYS_INIT_FAILED          ERROR     0x0106 SYS_FS_MOUNT_FAILED       CRITICAL
0x0107 SYS_PSRAM_ALLOC_FAILED   CRITICAL  0x0108 SYS_HEAP_LOW              WARN
0x0109 SYS_RESTART_REQUESTED    INFO      0x010A SYS_RESTART_EXECUTED      INFO
0x010B SYS_RESTART_CANCELLED    INFO      0x010C SYS_CRITICAL_OP_UNDERFLOW CRITICAL

0x0201 CFG_LOAD_DONE            INFO      0x0202 CFG_MODULE_LOAD_FAILED    ERROR
0x0203 CFG_CHANGE_APPLIED       INFO      0x0204 CFG_SAVE_OK               INFO
0x0205 CFG_COMMIT_FAILED_ROLLBACK ERROR   0x0206 CFG_RECOVERED_FROM_BACKUP WARN
0x0207 CFG_VERSION_REBUILT      WARN      0x0208 CFG_FACTORY_RESET         WARN
0x0209 CFG_WRITE_REJECTED       WARN      0x020A CFG_RESTART_TIMEOUT       INFO

0x0301 STG_FS_UNAVAILABLE       CRITICAL  0x0302 STG_ATOMIC_WRITE_FAILED   ERROR
0x0303 STG_CRC_FAILED           CRITICAL  0x0304 STG_TXN_RECOVERED         WARN
0x0305 STG_WRITE_VERIFY_FAILED  ERROR     0x0306 STG_READ_FAILED           WARN

0x0401 WF_START                 INFO      0x0402 WF_FINISHED               INFO
0x0403 WF_TIMEOUT               WARN      0x0404 WF_FAILED                 WARN
0x0405 WF_ACTION_FAILED         WARN      0x0406 WF_SAVE_FAILED            WARN
0x0407 WF_SAVE_PARTIAL          WARN      0x0408 WF_CRUD                   INFO
0x0409 WF_MIGRATED              INFO      0x040A WF_TEMP_ACTION_TIMEOUT    WARN
0x040B WF_RUNTIME_ALLOC_FAILED  ERROR     0x040C WF_SAVE_PARTIAL_RETRY_OK  INFO(可选)

0x0501 DISPENSE_START           INFO      0x0502 DISPENSE_DONE             INFO
0x0503 DISPENSE_FAILED          WARN      0x0504 DISPENSE_TIMEOUT          WARN
0x0505 VALVE_FORCE_CLOSE        CRITICAL  0x0506 VALVE_FORCE_CLOSE_FAILED  CRITICAL
0x0507 VALVE_OVERFLOW_RISK      CRITICAL  0x0508 VALVE_SAFETY_TIMEOUT      WARN
0x0509 VALVE_OPEN               INFO      0x050A VALVE_CLOSE               INFO
0x050B VALVE_RATE_LIMITED       WARN      0x050C WEIGHT_ERROR_ENTER        WARN
0x050D WEIGHT_ERROR_EXIT        INFO      0x050E WEIGHT_ZERO_DONE          INFO
0x050F WEIGHT_TRIGGER_FIRED     INFO      0x0510 WEIGHT_CALIB_FAILED       ERROR

0x0601 WIFI_CONNECT_START       INFO      0x0602 WIFI_CONNECTED            INFO
0x0603 WIFI_CONNECT_TIMEOUT     WARN      0x0604 WIFI_LOST                 WARN
0x0605 WIFI_RECONNECT_TRY       WARN      0x0606 WIFI_PROVISION_ENTER      INFO
0x0607 WIFI_PROVISION_DONE      INFO

0x0701 MQTT_CONNECTED           INFO      0x0702 MQTT_DISCONNECTED         WARN
0x0703 MQTT_SLEEP_ENTER         ERROR     0x0704 MQTT_PUBLISH_FAIL         WARN
0x0705 MQTT_CMD_EXEC_FAILED     WARN      0x0706 CLOUD_FRAG_FAIL           WARN
0x0707 LOG_UPLOAD_FAIL          WARN      0x0708 LOG_ACK_TIMEOUT           WARN
0x0709 LOG_RING_OVERFLOW        WARN      0x070A LOG_ACK_LOST              WARN
0x070B LOG_SELF_DEGRADED        ERROR

0x0801 TIME_NTP_OK              INFO      0x0802 TIME_NTP_FAIL             WARN
0x0803 TIME_VALID_ENTER         INFO      0x0804 TIME_INVALID_ENTER        WARN
0x0805 TIME_RTC_PROBE           INFO/WARN 0x0806 TIME_RTC_BOOT_RESTORE     INFO
0x0807 TIME_RTC_CALIBRATED      INFO      0x0808 TIME_RTC_WRITE_FAILED     WARN
0x0809 TIME_RTC_VL_FLAG         WARN      0x080A TIME_RTC_BCD_INVALID      WARN

0x0901 BLE_DATA_DECODED         INFO      0x0902 BLE_DECODE_FAIL           WARN(限流)
0x0903 BLE_SENSOR_LOST          WARN      0x0904 BLE_SCAN_DISABLED         INFO

0x0A01 CMD_RUNTIME_QUEUE_FULL   WARN      0x0A02 CMD_RUNTIME_TIMEOUT       WARN
0x0A03 CMD_REJECTED             WARN      0x0A04 CMD_APPLIED               INFO

0x0B01 REG_REBUILT              INFO      0x0B02 REG_SAVE_FAILED           ERROR
0x0C01 EVT_QUEUE_FULL           WARN      0x0C02 EVT_STORM_DROPPED         WARN
0x0D01 CRESET_PULSE             INFO      0x0D02 CRESET_SAFETY_TIMEOUT     WARN
0x0D03 CRESET_POOL_EXHAUSTED    ERROR
0x0E01 OLED_INIT_FAILED         WARN
0x0F00 MOTOR_RESERVED_BASE      （未来模块）
```

## 附录 B：调用速查卡

```cpp
#include "log_manager.h"

// 无参数
log_emit0(LOG_EVT_ID, LOG_LVL_INFO);

// 1~8 个参数
LogParamIn p[2];
p[0] = log_arg_u32(LOG_P_OPEN_MS,  open_ms);
p[1] = log_arg_enum(LOG_P_CAUSE,   cause);
log_emit(LOG_EVT_ID, LOG_LVL_WARN, p, 2);
```

**四不要**：
1. 不要写 `LOG_INFO(...)`（不存在）；
2. 不要在 ISR 中调用；
3. 不要传字符串（当前 API 不支持）；
4. 不要把周期性状态当事件打（只在**边**上打）。

---

## 10. ConfigManager 接入（**P2-B 已落地**）

> 状态：已实现并上板验证。提交：`feat(log): integrate config manager logging`
> 前置阅读：§9（Storage bridge）—— 本节的桥接与之同构，只看差异即可。

### 10.1 与 P2-A（Storage）的三点关键差异

| # | 差异 | 影响 |
|---|---|---|
| ① | **`cfg_log()` 在未注册回调时会兜底打串口**（`config_manager.cpp:220`）。一旦注册，兜底分支被跳过 | 桥接**必须自己补串口输出**，否则原有日志会突然消失（行为回退）⇒ 本桥接是"串口 + LogManager"双路 |
| ② | **允许 INFO 上报**（Storage 桥接丢弃 INFO） | Config 的 INFO 承载真实诊断信息（`init done` / `boot validated`），且频率极低 |
| ③ | **两个显式语义埋点必须在 ConfigManager 内部** | 因为对应事件在 `cfg_log` 里**没有文本**（见 §10.3） |

### 10.2 桥接的语义映射（关键词分类）

`cfg_log` 只给自由文本 ⇒ EventId 靠**关键词**分类，op 明细写入 `LOG_P_ERR_CODE`。

| 关键词 | EventId | 说明 |
|---|---|---|
| `from backup` | `LOG_CFG_RECOVERED_FROM_BACKUP` (0x0206) | `loaded from backup` / `recovered from backup` |
| `restart timeout` | `LOG_CFG_RESTART_TIMEOUT` (0x020A) | 倒计时超时后保存并重启 |
| `init done` / `boot validated` | `LOG_CFG_LOAD_DONE` (0x0201) | 二者同 op ⇒ 被 10 s 去重合并为**每 Boot 1 条**（正是想要的效果） |
| `version` | `LOG_CFG_VERSION_REBUILT` (0x0207) | 缺失/损坏/重建 |
| `rejected` / `set failed` | `LOG_CFG_WRITE_REJECTED` (0x0209) | 写入被拒 |
| `rotate to backup` / `write active` / `commit` / `rollback` / `save failed` / `save:` | `LOG_CFG_COMMIT_FAILED_ROLLBACK` (0x0205) | 提交/回滚/原子写失败（含"保存失败"；Config 段无独立 SAVE_FAILED ID） |
| 其余 `E` | `LOG_CFG_MODULE_LOAD_FAILED` (0x0202) | **兜底桶**：真实语义看串口原文 + `LOG_P_ERR_CODE`，不要依赖事件名 |
| 其余 `W` | `LOG_CFG_WRITE_REJECTED` (0x0209) | 同上 |
| 其余 `I` | **不上报** | ⚠️ INFO 的兜底分类不可靠（`critical op acquired` / `restart timer refreshed` 无对应事件）⇒ 错标比不报更糟，只在串口保留 |

**明确跳过（`CFG_OP_SKIP`，避免与其它机制 / 其它段重复）**

| 文本 | 为什么跳过 |
|---|---|
| `enqueue accepted, pending=%d` | **入队 ≠ 生效**；"生效"由 §10.3 的显式埋点负责 |
| `restart requested by caller, delegating to SystemCommand` | 重启请求归 **System 段** `LOG_SYS_RESTART_REQUESTED`，跨段会重复 |

**参数（只用现有 ParamId）**：`LOG_P_ERR_CODE`(0x0C) = 桥接私有 op 枚举；
`LOG_P_COUNT`(0x45) = 10 s 窗口内被抑制的条数（仅当 > 0）。

### 10.3 两个显式语义埋点（桥接覆盖不到的部分）

| 事件 | 落点 | 参数 | 为什么必须显式 |
|---|---|---|---|
| `LOG_CFG_CHANGE_APPLIED` (0x0203) | `exec_set_field()` 成功尾部（**单键**路径，`config_set` 走这里）**和** `exec_set_module()` 成功尾部（**整模块**路径） | `LOG_P_MODULE`(哈希) + `LOG_P_KEY`(哈希) + `LOG_P_COUNT`(生效字段数) | `cfg_log` 里没有"变更生效"的文本 |
| `LOG_CFG_SAVE_OK` (0x0204) | `exec_save()` 成功尾部（**命令通道**）**和** `config_save()` 成功尾部（**内部 API 通道**） | 无 | `cfg_log` 里没有"保存成功"的文本 |

> ⚠️ **`exec_save()` 与 `config_save()` 是保存事务的两份实现**，前者**不经过**后者。
> 只在一处埋点会漏掉另一条通道 ⇒ 两处都要。二者互斥执行，**不会重复上报**。
> 同理 `exec_set_field()` 与 `exec_set_module()` 是两个互斥入口。

**`LOG_CFG_SAVE_OK` 只在"真实写入"路径发射**：`config_save()` / `exec_save()` 的
"无 dirty → 幂等返回"是 no-op，**不发射**（否则每次空调用都会留一条 INFO）。

### 10.4 字符串哈希（`LOG_P_MODULE` / `LOG_P_KEY`）

`LogParamIn` 无 blob 字段、`LOG_PTYPE_STR` 不可构造 ⇒ 按 P2 定版做 **FNV-1a 32 位**哈希
（`config_manager.cpp` 内的 `cfg_hash32()`，8 行、无表、无堆）。

```
uint32_t h = 2166136261u;            // FNV offset basis
while (*s) { h ^= (uint8_t)*s++; h *= 16777619u; }
```

**已知限制**：① 不可逆 ⇒ 云端需维护"模块名 / 键名 → 哈希"字典；② 理论碰撞 ⇒ 哈希只用于
**聚合与筛选**，精确定位以串口原文为准；③ 该 helper 目前是 ConfigManager 的局部实现，
多模块共用时应上移为共享工具（Future Improvement）。

### 10.5 上板验证结果（P2-B，2026-09-17）

```
[CFG][I] init done, loaded 8/8 modules      ← 双路串口（若未补串口则本行消失）
[CFG][I] boot validated
[Cloud LOG] OK topic=guo_feeder/log len=147 ← 1 条记录（LOAD_DONE；INFO 只上云）
[LogT] stats emit=1 ... flash=0 cloud=1     ← flash=0 证明 INFO 不走 Flash（符合 Level Policy）

-- config_set（valve.open_duration_ms，同值写入）--
{"m":"field updated, restart required","e":0}
[LogT] stats emit=2 ... flash=0 cloud=2     ← +1 = LOG_CFG_CHANGE_APPLIED ✅
[CFG][I] enqueue accepted, pending=1        ← 被 SKIP ⇒ 未产生第二条（无重复上报 ✅）

-- config_save --
{"m":"config saved","e":0,"data":{"saved":true,"dirty":false}}
[Cloud LOG] OK topic=guo_feeder/log len=407 ← ★ 147 → 407 = 1 → 3 条
                                               （+CHANGE_APPLIED +SAVE_OK = LOG_CFG_SAVE_OK ✅）
rst:0xc (RTC_SW_CPU_RST)                    ← 安全重启正常（boot_seq 15 → 16），Critical Op 未被破坏
```

**验证要点**：双路串口 ✅ · LOAD_DONE 每 Boot 1 条 ✅ · SKIP 无重复 ✅ ·
CHANGE_APPLIED ✅ · SAVE_OK ✅ · INFO 只上云（`flash=0`）✅ · 安全重启语义未受影响 ✅

**未验证**：桥接的 `E`/`W` 分支（需真实的配置加载失败 / 写被拒；现有钩子无法在不破坏设备配置的前提下触发）。

### 10.6 命令通道备忘（写用例/联调时容易踩）

- `config_*` 命令**不在顶层分发**，而在 `system_router()` 内 ⇒ 报文必须是
  `{"cmd":"system","ob":"config_query|config_set|config_save|...","id":"<唯一>","p":{...}}`。
  用 `{"cmd":"config_query",...}` 会得到 `Unknown command: config_query`。
- `config_set` 需要 `module` **和** `key`；省略 `expect_version` 表示**不做乐观锁校验**；
  显式传入过期版本会得到 `version mismatch`（**这是正确行为，不是缺陷**）。
- 配置写入会走 **Safe Restart**（Critical Op + 5 分钟倒计时）⇒ 测试后设备会自行重启，
  `logt stats` 的计数会被重置，**不要**把重启后的计数当作同一会话的延续。

---

## 11. watermark 语义分离（**FIX-BT9，已落地**）

> 状态：已实现并上板验证。提交：`fix(log): prevent ack watermark bypass replay records`
> 前置阅读：§1 架构。**本节改变了"水位"的语义，凡涉及补发/段回收/ACK 匹配的理解都必须以本节为准。**

### 11.1 问题：一个标量承担了两种互不相容的语义

修复前只有 `s_cloud_acked_seq` 一个标量，被同时用于：

| 用途 | 需要的语义 |
|---|---|
| ACK 的"重复/回退"检测（`log_ack_classify`） | 云端确认过的**最大** seq |
| 能否跳过补发（`log_ack_should_replay`） | **连续**已确认水位（比它小的都已确认） |
| 能否回收段（`log_ack_segment_reclaimable`） | **连续**已确认水位 |
| 淘汰记账（`flash_count_unacked`） | 同上 |

而 **ACK 只证明"我刚发出的这一批被云端持久化"，不证明"比它小的都已持久化"**。
Boot 期一条 live 记录（seq 更大）先被 ACK 时，单点水位就会**跨过**仍躺在 Flash 里、
本 Boot 还没补发过的旧记录：

```
Boot: [JStg][W] read: open failed: /config/.commit   ← live WARN（seq 较大）
      → 进云队列 → 被 ACK ⇒ acked_seq 跳到较大值
      → 队列排空 ⇒ 补发 sweep 开始扫 Flash 里的旧记录（seq 更小）
      → should_replay(seq <= acked) == false  ⇒ 永久不再补发（静默丢失）
      → segment_reclaimable(last_seq <= acked) == true ⇒ 段被删（重启也补不回）
```

⇒ 违反 at-least-once。记为 **BT-9**。

**放大因素（不是根因）**：补发 sweep 只在 `used == 0` 时推进（§11.5），
live 记录会把补发挡住；而 P2-A 之后每个 Boot 必有 live 记录 ⇒ P2-A 让 BT-9 由"偶发"变"必现"。

### 11.2 方案：两个标量 + 一个钳制规则（状态语义分离）

```c
s_cloud_acked_seq   // 云端确认过的**最大** seq（真实值）。仅用于观测，不参与判定
s_cloud_gc_seq      // **连续**可回收水位。所有放行判定都用它
s_cloud_gc_floor    // 尚未确认的 Flash backlog 最低 seq 下界（0 = 无钳制）

关系：gc_seq = (gc_floor == 0) ? acked_seq : min(acked_seq, gc_floor - 1)
不变量：gc_seq <= acked_seq   （钳制只会更保守，永不抬高）
```

判定函数（纯逻辑，在 `src/log_ack.h`，可主机穷举）：

```c
static inline uint32_t log_ack_gc_watermark(uint32_t acked_max, uint32_t gc_floor);
```

**`s_cloud_gc_seq` 现在用于**（全部替换掉原来的 `s_cloud_acked_seq`）：

1. `log_ack_classify()` 的重复/回退检测
2. `log_ack_should_replay()`（能否跳过补发）
3. `log_ack_segment_reclaimable()`（能否回收段 / 能否当作环压力受害者）
4. `flash_count_unacked()`（淘汰记账口径）

`s_cloud_acked_seq` 仅出现在串口打印与 `LogCloudInfo.acked_seq`。

### 11.3 为什么必须把 `log_ack_classify` 也换成连续水位

只改补发与段回收是**不够**的。旧批次（boot_seq 更小）晚到的 ACK 若与"确认最大值"
比较，会被规则④判成 `DUPLICATE` 而丢弃 ⇒ `rd` 不推进 ⇒ 那 8 条永远留在云队列里 ⇒
`used != 0` 恒成立 ⇒ **补发被自身永久阻塞**（比 BT-9 更糟）。

换成 gc 水位后：旧批次 ACK 的 `ack_to` 恒 > gc（因为它本来就没被确认过）⇒ 正常 `ACCEPT`；
而**真正的重复 ACK**（同一批已处理）在与 gc 比较时仍满足 `ack_to <= gc` ⇒ 仍判 `DUPLICATE`
⇒ 幂等性没有被放掉（合约测试 ⑧-3 有正/负向两组断言钉住）。

### 11.4 `gc_floor` 的生命周期（唯一容易写错的地方）

| 事件 | 动作 |
|---|---|
| `cloud_replay_arm()`（log_init / `creset` / 游标段被删） | `gc_floor = flash_lowest_first_seq()` —— **发生在任何 ACK 之前**，这是修复的关键时序 |
| `cloud_poll()` 每轮 | 仅在 `!s_replay_armed && gc_floor != 0 && cloud_queue_head_is_current_boot()` 时清零，并让 `gc_seq` 追平 `acked_seq`，随后补做一次段回收 |

清除条件的两个分支缺一不可：

- `!s_replay_armed` —— 还有旧记录没投递时不能放行；
- `cloud_queue_head_is_current_boot()` —— 补发只在 `used == 0` 时推进 ⇒ 每次 push 都是
  往**空队列**里放 ⇒ 补发的旧记录必然构成**队列前缀**；队首回到本 Boot ⇒ 那段前缀已全部离开
  队列（被 ACK / 放弃（已登记空洞）/ 淘汰（已登记空洞））。

⚠️ **不要用"队列已空"当清除条件**：持续有 live 记录时队列可能长期非空 ⇒ 钳制永不解除
⇒ 段永不回收 ⇒ 环压力反而丢数据（把静默丢失换成另一种丢失）。

### 11.5 未改动的部分（刻意保留）

| 项 | 现状 | 说明 |
|---|---|---|
| 补发 sweep 的 `used == 0` 门槛 | **未改** | 它使 live 记录阻塞补发（延迟，非丢失）。云端正常工作时队列会周期性排空 ⇒ 补发总能推进；云端不 ACK 时补发出来的记录同样送不出去，改门槛无收益。属**延迟**问题，不属 at-least-once 破坏 |
| 空洞表（`LOG_HOLE_MAX = 8`） | **未改** | give-up / 队列淘汰的语义不变；段回收仍要求"与任何空洞不相交" |
| 队列淘汰登记空洞的 `evict_boot == s_boot_seq` 条件 | **未改** | 见 §11.6 残余项 BT-10 |
| `log_ack_*` 各纯函数签名 | **未改** | 只改变了"传进去的水位是哪个"，未改变任何判定规则本身 |
| `LogRecord` / Flash 段格式 / CBOR 批次格式 | **未改** | 纯 RAM 态变更，无持久化格式迁移 |

### 11.6 残余项（已记录，不在本次修复范围）

**BT-10（低概率）**：云队列**溢出**淘汰时，只有当被淘汰记录属于本 Boot 才登记空洞
（`cloud_queue_push()` 的 `evict_boot == s_boot_seq` 条件）。若淘汰的是刚补发进来的
**上一 Boot** 记录，则不登记空洞 ⇒ 该记录在钳制解除后有被回收的风险。
触发条件是复合的：① 队列满（128）且队首补发记录还没发出去 ② 之后有更高的 ACK 把水位推过它
③ 恰好触发段回收。
**建议修法**：把该条件去掉（补发记录同样需要空洞保护），代价是空洞表压力上升
（补发记录 seq 稀疏、难以合并，`LOG_HOLE_MAX=8` 可能溢出 ⇒ 退化为"本 Boot 停止回收"）。
因 `test/log_fix_tests.txt` F1 的 `holes=0` 断言正是钉住当前行为，改动需单独评审。

### 11.7 观测（串口）

```
[LogT] cloud4 acked=%u gc=%u gcfloor=%u
[Log Cloud] ack ok boot=%u to=%u covered=%u/%u acked=%u gc=%u floor=%u r=%d
[Log Cloud] replay sweep done (acked=%u gc=%u floor=%u giveup=%u)
```

判别：**`gc < acked` ⟺ 存在"已被更晚的 ACK 越过、但仍未确认"的旧记录（钳制生效中）**。
`LogCloudInfo` 新增 `gc_seq` / `gc_floor` 两个字段（只增不改，既有字段含义不变）。

### 11.8 上板验证（F2-B，BT-9 验收场景）

场景构造（用注入 ACK 把偶发变成确定性）：

```
复位 → rarmed=1
logt atimeout 60000              # 防 live 批次中途 give-up
logt stats ||| boot=             # 刷新 <BOOT>
logt ack <BOOT> 1 4294967295     # ★ 注入 live 批次 ACK ⇒ 水位跳到旧记录之上
logt cloud ||| gc=               # 观测：gc < acked（钳制生效）
logt stats ||| replay=8          # ★ 验收 1：旧记录仍被补发（修复前恒 0）
logt stats ||| segdel=0          # ★ 验收 2：旧段未被提前回收
logt cloud   ||| qused=8         # ★ 验收 3：旧记录已回到云队列
logt ackauto 0                   # 旧批次 ACK 不得被判 DUPLICATE 吞掉
logt stats ||| qused=0           # 队列排空
logt cloud ||| rarmed=0
```

离线侧的等价证据在 `test/log_contract/probe_ack.cpp` 第 ⑧ 组：
**23 条断言全部通过，含 3 条负向探针**（用修复前的单点水位时结论必须相反），
证明这组断言具备可证伪性、且修复确实改变了行为。

---

## 12. WiFi 接入（**P2-C 已落地**）

> 状态：已实现并上板验证。提交：`feat(log): integrate wifi module logging`
> 源码：`src/wifi_module.cpp`（**唯一改动文件**）

### 12.1 与 Storage / Config 的关键差异：**没有回调接口，只能显式埋点**

Storage / Config 都有 `*_set_log_callback()` ⇒ 可用 `main.cpp` 里的桥接（§9 / §10）零侵入接入。
**WiFi 没有** —— 全模块只有 5 处 `Serial.println` ⇒ 只能在状态机分支里直接调 `log_emit()`。

本节的接入形态：**在既有分支里加一行 `log_emit()`，不改任何判定、不新增等待**。

### 12.2 埋点表

| 位置 | 事件 | EventId | Level | 参数 |
|---|---|---|---|---|
| `wifi_start_connect()`（**仅非重连**） | 发起连接 | `LOG_WIFI_CONNECT_START` (0x0601) | INFO | `SSID_HASH`(0x33)、`ATTEMPT_N`(0x30)、`WAS`(0x47)、`STATE`(0x44) |
| `wifi_task()` WIFI_CONNECTING，`if(!wifi_connected)` 边沿内 | 已连接 | `LOG_WIFI_CONNECTED` (0x0602) | INFO | `CONNECT_MS`(0x32)、`RSSI`(0x31)、`SSID_HASH`、`WAS`、`STATE` |
| `wifi_task()` WIFI_CONNECTING 超时分支 | 连接超时 | `LOG_WIFI_CONNECT_TIMEOUT` (0x0603) | WARN | `TIMEOUT_MS`(0x08)、`ATTEMPT_N`、`WAS`、`STATE` |
| `wifi_task()` WIFI_CONNECTED 断线分支 | WiFi 丢失 | `LOG_WIFI_LOST` (0x0604) | WARN | `CONNECTED_MS`(0x2C)、`RSSI`、`CAUSE`(0x16)、`WAS`、`STATE` |
| `wifi_task()` WIFI_DISCONNECTED 重连分支 | 尝试重连 | `LOG_WIFI_RECONNECT_TRY` (0x0605) | WARN | `RETRY_N`(0x29)、`ATTEMPT_N`、`WAS`、`STATE` |

参数数量 4–5，均 ≤ `LOG_MAX_PARAMS`(8)。

### 12.3 ★ 限流：失败循环（超时 + 重连）两个事件都要节流

实测发现：**只节流 `RECONNECT_TRY` 是不够的**。本状态机里超时与重连是**同一个失败循环的两半**
（超时 → `DISCONNECTED` → 重连 → 再超时…），默认 `connect_timeout=30s` ⇒ 循环每 ~30s 一轮 ⇒
超时事件本身就有 **2 条/分钟**（WARN，**落 Flash**）。

因此两个事件共用同一个节流门（各自独立计数 / 独立计时）：

```c
#define WIFI_FAIL_LOG_EVERY_N   5u      // 每 5 次记 1 次
#define WIFI_FAIL_LOG_MIN_MS    60000u  // 最小间隔下限

static bool wifi_fail_should_log(uint32_t n, unsigned long *last_ms)
{
    if ((n % WIFI_FAIL_LOG_EVERY_N) != 1u)                      return false;
    if (*last_ms != 0 && (millis() - *last_ms) < WIFI_FAIL_LOG_MIN_MS) return false;
    return true;
}
```

⚠️ **用 AND 而不是 OR**（这是实测踩出来的）：
- `MIN_MS` 的定位是"**最小间隔下限**"，用于防止把 `reconnect_interval` 配得过小（如 1s）时
  "N 次"也很快 ⇒ 又变洪泛。
- 若写成 **OR**，60s 会反过来变成**上限**：当尝试间隔本来就 > 12s 时，它会架空 N 次规则、
  让日志**变多**（实测 30s 一轮时：OR 每 60s 一条、AND 每 150s 一条）。

**效果（实测，SSID 指向不存在的 AP）**：4 轮失败循环（每轮 = 1 次超时 + 1 次重连）
⇒ 只有第 1 轮产生了 2 条 WARN，第 2/3/4 轮**全部被抑制**（`flash` 计数不再增长）。
全量应为 8 条，实际 2 条 ⇒ **4× 削减**。

被抑制的轮次**不丢信息**：发出的记录带 `LOG_P_RETRY_N`（本次掉线第几次）与
`LOG_P_ATTEMPT_N`（累计第几次连接尝试），云端可据此还原重试节奏。

### 12.4 ★ 为什么重连时**不发** `CONNECT_START`

`wifi_start_connect()` 同时承担"首次连接"和"每次重连"两种调用（后者在 `WIFI_DISCONNECTED`
分支里）。若两个事件都按全量发，会在**同一个 tick 上重复**。

| 场景 | 发哪个 |
|---|---|
| 非重连（`wifi_retry_n == 0`，开机 / IDLE 重入） | `LOG_WIFI_CONNECT_START`（INFO） |
| 重连（`wifi_retry_n >= 1`） | `LOG_WIFI_RECONNECT_TRY`（WARN，节流） |

判据 `if (wifi_retry_n == 0)` 放在 `wifi_start_connect()` 里，而 `wifi_retry_n++` 在**调用它之前**
完成 —— 顺序本身是语义的一部分。⇒ `CONNECT_START` 天然低频（每 Boot 约 1 条），无需节流。

### 12.5 状态迁移的 `WAS` / `STATE` 编码

按 §2，状态迁移日志一律携带成对 `LOG_P_WAS` + `LOG_P_STATE`（便于云端还原迁移图）。
取值 = `enum WifiState`（`wifi_module.cpp` 文件内枚举）：

| 值 | 状态 |
|---|---|
| 0 | `WIFI_IDLE` |
| 1 | `WIFI_CONNECTING` |
| 2 | `WIFI_CONNECTED` |
| 3 | `WIFI_DISCONNECTED` |

> ⚠️ 该枚举是 `wifi_module.cpp` 的**文件内**枚举，不对外暴露。若将来有第二个模块需要表达
> WiFi 状态，应先把它上移到头文件并冻结取值，避免出现第二套编码。

### 12.6 参数语义（三个容易写错的点）

| 参数 | 取值 | 为什么 |
|---|---|---|
| `LOG_P_SSID_HASH` | FNV-1a 32（`wifi_ssid_hash32()`） | `LogParamIn` 无 blob 字段、`LOG_PTYPE_STR` 不可构造 ⇒ 按 P2 定版走哈希（沿用 §10.4 的 `cfg_hash32()` 先例）。**不传明文 SSID**（隐私 + 定长） |
| `LOG_P_CAUSE`（LOST） | 断连瞬间 `WiFi.status()`（`WL_CONNECTION_LOST` / `WL_DISCONNECTED` / `WL_CONNECT_FAILED` …） | 现场断网的**首因**判据 |
| `LOG_P_RSSI`（LOST） | `state_get_int(STATE_WIFI_RSSI)` = **最后一次采样值** | 此刻已断连，`WiFi.RSSI()` 只会返回 −100；该状态量由 `wifi_update_signal()` 每 10 s 维护 |

`LOG_P_CONNECTED_MS`（LOST）＝ `millis() - wifi_connected_since`（新增的**纯日志用**静态量，
不参与任何判定）。

### 12.7 刻意未做的事

| 项 | 原因 |
|---|---|
| 新增 `LOG_WIFI_RSSI_LOW` | 需**新增 EventId**，违反 P2 定版"暂不新增 EventId" |
| 实现 `LOG_WIFI_PROVISION_ENTER/DONE` (0x0606/0x0607) | 代码中**不存在任何 provisioning 实现**，无宿主 |
| 把 `wifi_ssid_hash32()` 上移为共享工具 | 本次约束"只在 `wifi_module.cpp` 改动"。⚠️ 它现在与 `config_manager.cpp` 的 `cfg_hash32()` **重复**，多模块共用时应上移 |
| 改状态机 / 连接流程 / 加等待 | 明令禁止；埋点全在既有分支内，且全部是非阻塞调用 |
| 修 `give_up_seq` 越过旧记录（见 §12.9 新发现 BT-11） | 属 LogManager 基线（replay 语义），P2-C 明令"不改 ACK/replay/GC 逻辑" |

### 12.8 上板验证（2026-09-17，COM8，AP `wqs1`）

**① 正常连接（开机）**

```
WiFi init... / Connecting to:wqs1 / WiFi connected
[LogT] stats emit=3 ... flash=0 cloud=3     ← 两条 WiFi 事件都是 INFO ⇒ 不落 Flash ✅
```

MQTT 侧解码（同批 3 条，`boot_seq=2`，seq 769..771）：

```
pc=4  SSID_HASH=0x7c7a4fc9  ATTEMPT_N=1  WAS=0(IDLE)        STATE=1(CONNECTING)   ⇒ CONNECT_START ✅
pc=5  CONNECT_MS=1807  RSSI=-56  SSID_HASH=0x7c7a4fc9
      WAS=1(CONNECTING) STATE=2(CONNECTED)                                        ⇒ CONNECTED     ✅
```

**② 失败循环（SSID 指向不存在的 AP）**

```
[00:16:28] WiFi connect timeout / Try reconnect     ← 第 1 轮
[00:16:58] WiFi connect timeout / Try reconnect     ← 第 2 轮（未记日志）
[00:17:28] WiFi connect timeout / Try reconnect     ← 第 3 轮（未记日志）
[00:17:58] WiFi connect timeout / Try reconnect     ← 第 4 轮（未记日志）
[LogT] stats ... flash: 0 → 2（仅第 1 轮）⇒ 之后三轮 flash 计数**不再增长** ✅ 节流生效
```

恢复 SSID 并重新联网后，这两个 WARN 由补发路径送到云端，实测解码：

```
pc=4  level=WARN  TIMEOUT_MS=30000  ATTEMPT_N=1  WAS=1(CONNECTING)   STATE=3(DISCONNECTED)
                                                    ⇒ LOG_WIFI_CONNECT_TIMEOUT ✅
pc=4  level=WARN  RETRY_N=1         ATTEMPT_N=1  WAS=3(DISCONNECTED) STATE=1(CONNECTING)
                                                    ⇒ LOG_WIFI_RECONNECT_TRY   ✅
```

**③ 回归**：埋点后全量重跑 **195/195 = 100%，0 MISS**，未破坏任何既有断言。
（P2-D 期间夹具加固后 B 段断言数 65 → 66 ⇒ 当前基线为 **196/196**，见 §13.7）

### 12.9 已知限制 / 新发现

| 项 | 说明 |
|---|---|
| **`LOG_WIFI_LOST` 未做硬件触发验证** | 当前**没有任何钩子**可以强制断连（`WiFi.disconnect()` 无调用点；改 SSID 只会影响下次 `WiFi.begin()`，不会让已建立的连接掉线）。本环境唯一办法是**物理关掉 AP**。参数语义已做代码审查（§12.6）。建议后续加一个测试钩子（如 `wifi force-lost`），或由人工关 AP 复测 |
| **新发现 BT-11（与 LogManager 基线有关，本次不改）** | `s_cloud_give_up_seq` 是**单点水位**：若**较新**的批次先 give-up（实测 INFO 批次 seq 3073-3075 放弃 ⇒ `give_up_seq=3075`），那么 seq 更小、仍在 Flash 等待补发的旧记录（seq≈1800）会被 `log_ack_should_replay(seq <= give_up_seq)` **跳过** ⇒ 本 Boot 补发不到（下次 Boot `give_up_seq` 归 0 后恢复）。**与 BT-9 同源**（单点水位越过旧记录），但属 replay 语义，需在 LogManager 侧单独评审。规避：让补发先于 live 批次发生，或把 give-up 也改成按区间/连续水位记账 |
| **`LOG_WIFI_LOST` 验证状态（三项分开看）** | ① **代码路径审查 ✅ 通过**（埋点在 `case WIFI_CONNECTED` 的 `WiFi.status() != WL_CONNECTED` 分支内，未改动任何分支/return 路径，与既有 `Serial.println("WiFi lost")` 同处一个边沿块）；② **参数语义 ✅ 已确认**（见 §12.6：`CONNECTED_MS` / `RSSI`=最后采样值 / `CAUSE`=断连瞬间 `WiFi.status()` / `WAS=2` `STATE=3`）；③ **真机断 AP 验证 ❌ 未完成 —— 列为后续测试项**（无强制断连钩子：`WiFi.disconnect()` 无调用点，改 SSID 只影响下次 `WiFi.begin()`。建议加 `wifi force-lost` 钩子或人工关 AP 复测）。详见 Progress 文档「P2-C 验证记录」 |
| SSID 哈希不可逆 | 与 §10.4 同：云端需维护"SSID → 哈希"字典；哈希只用于聚合/筛选 |

---

## 13. Cloud / MQTT 接入（**P2-D 已落地**）

> 状态：已实现并上板验证。提交：`feat(log): integrate cloud manager logging`
> 源码：`src/cloud_manager.cpp`（**唯一改动文件**，纯增量 +233 / −0 —— 没有任何既有行被修改）

### 13.1 与 Storage / Config / WiFi 的差异

Cloud **既没有**日志回调接口（不像 Storage/Config 可桥接），**也不该**在 MQTT 回调里埋点
（回调运行在 esp-mqtt 任务上下文）。它已经有现成的**"回调置标志 → `cloud_task()` 处理"**
同步机制 ⇒ 埋点应当**挂在标志被消费的地方**（loop 上下文），边沿性由标志本身保证。

### 13.2 埋点表

| 位置 | 事件 | EventId | Level | 参数 | 限流 |
|---|---|---|---|---|---|
| `cloud_process_mqtt_events()` `mqtt_connect_pending` 分支 | MQTT 已连接 | `LOG_MQTT_CONNECTED` (0x0701) | INFO | `OUTBOX`(0x2B)、`WAS`(0x47)、`STATE`(0x44) | 边沿即天然限流 |
| `cloud_process_mqtt_events()` `mqtt_disconnect_pending` 分支 | MQTT 断开 | `LOG_MQTT_DISCONNECTED` (0x0702) | WARN | `OUTBOX`、`RETRY_N`(0x29)、`WAS`、`STATE` | 同上 |
| `cloud_task()` 进入 sleep 退避分支 | 进入休眠退避 | `LOG_MQTT_SLEEP_ENTER` (0x0703) | ERROR | `RETRY_N`、`SLEEP_MS`(0x2A) | 同上 |
| `cloud_task()` 顶部（周期窗口） | 发布失败（聚合） | `LOG_MQTT_PUBLISH_FAIL` (0x0704) | WARN | `FAIL_COUNT`(0x3F) | **计数聚合：60 s 窗口 1 条** |
| `cloud_process_rx_message()` 命令执行失败 | 命令执行失败 | `LOG_MQTT_CMD_EXEC_FAILED` (0x0705) | WARN | `CMD_ID`(0x2D, **哈希**) | 无需（频率由云端下发速率决定） |

### 13.3 ★ 边沿触发是**制度性**保证，不靠"记得去重"

| 事件 | 边沿来源 |
|---|---|
| `LOG_MQTT_CONNECTED` | `mqtt_connect_pending` 是**单槽标志**，回调置位、`cloud_process_mqtt_events()` **进入即清零** ⇒ 一次连接只记一条；底层 `MQTT_EVENT_CONNECTED` 重复到达时，标志已被消费，下一次才会有新记录 |
| `LOG_MQTT_DISCONNECTED` | 同上（`mqtt_disconnect_pending`）。MQTT 抖动时 DISCONNECTED 可能连续上报，但单槽标志使得**每个 loop 至多一条**，不会与重连计数一起放大 |
| `LOG_MQTT_SLEEP_ENTER` | 该分支之前有 `if(mqtt_sleep_mode) { ... return; }` ⇒ **"已在休眠"的状态提前返回**，只有 `false→true` 的迁移能走到这里 |

**为什么不直接在 `mqtt_event_handler()` 里埋点**：① 那是 esp-mqtt 任务上下文，与"集中在
`*_task()` 调用"的约定不符；② 直接在回调里发日志会失去上述标志提供的**天然去重**，
反而需要额外写一套边沿判断。**复用既有标志 = 零新增机制**。

### 13.4 ★ 发布失败：计数聚合（**绝不逐条记**）

离线时发布失败可能**每 loop 发生多次**。采用审查报告 §5.3「**计数聚合**」：

```c
static uint32_t      cloud_publish_fail_count     = 0;   // 只累加
static unsigned long cloud_publish_fail_report_ms = 0;

static void cloud_note_publish_fail();     // 失败点调用：只 ++
static void cloud_report_publish_fail();   // cloud_task() 顶部调用：窗口到才发射 1 条
```

- **累加点（4 处，全在底层发布函数）**：`cloud_mqtt_publish_binary()` 的
  "client 为空 / 未连接" 与 `msg_id < 0`；`cloud_mqtt_publish_text()` 的同一对。
  只在这两个函数里计，避免同一失败被上层重复计数。
- **发射点（1 处）**：`cloud_task()` **最顶部**，`cloud_report_publish_fail()`。
  ⚠️ 必须在任何早期 `return` **之前** —— 离线正是 `if(!wifi_connected) return;` 的情形，
  放后面就永远发不出来。
- **窗口**：`CLOUD_PUBLISH_FAIL_REPORT_MS = 60000`。窗口内无失败 ⇒ **完全静默**。
- **不丢信息**：被抑制的次数放在 `LOG_P_FAIL_COUNT` 里（例如 `fail_count=37` 表示
  这 60 s 内失败了 37 次）。

> 语义边界：`cloud_send_up()` / `cloud_send_log()` 在离线时会**先行返回**，不会调用底层
> 发布函数 ⇒ 这类"根本没尝试发"**不计入** PUBLISH_FAIL（离线状态由
> `LOG_MQTT_DISCONNECTED` 单独表达）。PUBLISH_FAIL 表达的是"**尝试了但失败**"。

### 13.5 `WAS` / `STATE` 编码

底层只有一个 `bool mqtt_connected` ⇒ 用最小两值枚举（`cloud_manager.cpp` 内 `#define`）：

| 值 | 状态 |
|---|---|
| 0 | `CLOUD_MQTT_STATE_OFFLINE` |
| 1 | `CLOUD_MQTT_STATE_ONLINE` |

> ⚠️ 这是 CloudManager 的局部约定；若将来出现第三态（如"认证被拒"）必须先冻结取值再扩展。

### 13.6 刻意未做的事

| 项 | 原因 |
|---|---|
| `LOG_CLOUD_FRAG_FAIL` (0x0706) 埋点 | **`cloud_publish_fragmented()` 全仓库无调用者（死代码）** ⇒ 埋点不可达。不记，也不删（删函数属另一主题，超出"只加日志"的范围） |
| `LOG_MQTT_AUTH_FAILED` / `LOG_CLOUD_RX_INVALID` | 矩阵列为"**建议新增** EventId" ⇒ 违反 P2 定版"暂不新增 EventId"，本次不做 |
| `LOG_P_SERVER` / `LOG_P_CMD` / `LOG_P_CMD_ID` 传明文 | 均为字符串语义、当前不可达 ⇒ 按 P2 定版走哈希（`CMD_ID` 传 `cloud_cmd_id_hash32()`）；`SERVER` 直接省略 |
| WiFi 掉线时的 `cloud_task()` 分支（:1435 附近） | **不重复埋点**：该场景已由 WiFi 模块的 `LOG_WIFI_LOST` 表达，Cloud 侧再记会变成同一事件两条 |
| 改 MQTT 协议 / `log_ack` / replay / GC | 明令禁止；本次改动 **0 行删除**，纯新增 |
| 给 `LOG_MQTT_SLEEP_ENTER` 强制 flush | 事件注释里的 "IMM" 是设计意图；`log_flush()` 并不存在（只有 `log_flush_requested()`/`log_clear_flush_request()`，且 flush 由 CRITICAL 驱动）⇒ 想强制落盘必须改 LogManager，超出本次范围 |

### 13.7 上板验证（2026-09-18，COM8，AP `wqs1`，broker `emqxsl.cn:8883`）

**验证方法（可复用）**：把收到的 CBOR 批次用 `.pio/p15run/log_decode.py` 解到
**记录级**（`LogRecord` v2 的 `event_id` 在偏移 12，u16 LE；参数 `{id,type,u32le}` 从偏移 28 起，
每项 6 B），直接打印 `event_id` 与每个 `ParamId` 的**真值**。比"只比对计数"强得多。

**断连的确定性触发（关键技巧）**：用**设备自己的 `client_id`** 再连一次 broker
⇒ EMQX 做 **session takeover**，把设备踢下线 ⇒ 设备侧收到**真实的**
`MQTT_EVENT_DISCONNECTED`。这条路不需要改固件、不需要加钩子、不涉及协议改动。

```
设备串口（真实现场）：
  01:02:01.957 [Cloud] MQTT connected
  01:02:11.702 [Cloud] MQTT disconnected outbox=0        ← 被踢
  01:02:22.075 [Cloud] MQTT connected                    ← 自动重连成功

MQTT 侧解码（同一批，boot_seq=3）：
  seq=1027 INFO LOG_WIFI_CONNECTED      CONNECT_MS(u32)=1757 RSSI(i32)=-57
                                        SSID_HASH(u32)=2088390601 WAS(enum)=1 STATE(enum)=2   （P2-C 复现 ✅）
  seq=1028 INFO LOG_MQTT_CONNECTED      OUTBOX(u32)=450  WAS(enum)=0 STATE(enum)=1   ✅
  seq=1029 WARN LOG_MQTT_DISCONNECTED   OUTBOX(u32)=0    RETRY_N(u32)=1
                                        WAS(enum)=1 STATE(enum)=0                    ✅
  seq=1030 INFO LOG_MQTT_CONNECTED      OUTBOX(u32)=710  WAS(enum)=0 STATE(enum)=1   ✅（重连再次边沿触发）
  seq=1031 WARN LOG_MQTT_CMD_EXEC_FAILED  CMD_ID(u32)=2977285702                    ✅
  seq=1032 WARN LOG_MQTT_CMD_EXEC_FAILED  CMD_ID(u32)=2960508083                    ✅
```

- 两次 `LOG_MQTT_CMD_EXEC_FAILED` 对应向 `guo_feeder/down` 发的两条**不存在**的 action
  （`{"cmd":"execute_action","id":"p2dx1|p2dx2","ob":"NO_SUCH_ACTION_P2D…"}`）
  ⇒ 2 条命令 2 条记录，**未误触发**其他事件 ✅
- `LOG_MQTT_CONNECTED` 出现两次（首连 + 重连）⇒ 边沿语义正确：**一次连接一条**，不多不少 ✅
- `WAS/STATE` 成对且方向正确（0→1 连、1→0 断）✅

| EventId | 结论 |
|---|---|
| `LOG_MQTT_CONNECTED` | ✅ 上板通过 |
| `LOG_MQTT_DISCONNECTED` | ✅ 上板通过（真实被踢） |
| `LOG_MQTT_CMD_EXEC_FAILED` | ✅ 上板通过 |
| `LOG_MQTT_SLEEP_ENTER` | ⚠️ **代码路径审查通过，真机未触发**（原因见下） |
| `LOG_MQTT_PUBLISH_FAIL` | ⚠️ **代码路径审查通过，真机未触发**（原因见下） |

**回归**：埋点后全量重跑 **196/196 = 100%，0 MISS**（B 段断言数 65 → 66，见 §13.9）。

### 13.9 ★ 回归夹具加固（本次踩到，**后续每个模块都会遇到**）

**现象**：P2-D 埋点后 B 段（F2-B = BT-9 验收场景）**稳定** 4 条 MISS：
`replay=8` / `qused=8` / `qused=0` / `rarmed=0`。

**根因**：F2-B 的逻辑是"注入 live 批次 ACK ⇒ 队列排空 ⇒ 补发 sweep 推进 ⇒ 旧记录 replay"。
但注入的 ACK **只覆盖当时的在途批次**。P2-D 新增的 `LOG_MQTT_CONNECTED` 比 WiFi 记录
**晚约 4 s 落地**（要等 TLS 握手完成）⇒ 队列里**必然**多出一条尾巴（实测 `qused=4`，
批次只有 3 条）⇒ ACK 后 `used != 0` ⇒ **补发被挡住**。
（不是"没有补发"，是**观察不到**：再过一次 ACK 后 `replay=8` 立刻出现。）

**加固做法（把时序依赖改成顺序无关）**：在注入 ACK **之前**用 `logt cpush 1` 显式造一条
确定性的 live 尾巴，再在 ACK **之后**补一次 `logt ackauto 0` 排空它。
于是"Boot 期记录落在哪一批"不再影响结果 —— 两种情形收敛到同一条路径。
**断言强度未削弱**（`replay=8` 仍是精确值），只是夹具变确定性。

**⚠️ 给后续模块（Time / Workflow / Weight …）的规矩**：

1. **新模块的埋点会往同一条日志流里加记录**，`log_fix_tests.txt` 中的**绝对计数**断言
   （`evict_inf` / `qtotal` / `replay` / `fdrop` / `total` / `recs` / `qused` …）都可能被扰动。
   接入前先算清"我的埋点会在 Boot 期产生几条"，必要时在夹具里显式构造或排空。
2. **不要让用例依赖"某一批恰好装下全部记录"**：只要有一条记录在
   `cloud_collect_batch()` **之后**入队，它就会成为下一批。
   （这属 P1.5 既有语义，不是缺陷；夹具必须显式处理。）
3. **网络抖动会污染回归**：A 段首轮曾因 MQTT 写入超时
   （`MQTT_CLIENT: Writing didn't complete in specified timeout: errno=119`）
   阻塞 loop 约 8 s ⇒ 串口静默 ⇒ 命令未被及时处理 ⇒ **11 条假 MISS**。
   复跑（无抖动）后 A 段 56/56 全绿。**判据：串口里出现 `MQTT error event` /
   `Writing didn't complete`。** 遇到大面积 MISS 先查这个，再怀疑固件。


### 13.8 已知限制（两项未做真机触发，原因与复测方法）

**① `LOG_MQTT_SLEEP_ENTER` 真机未触发**

代码路径审查：埋点在 `cloud_task()` 的 `mqtt_retry_count >= max_retries` 分支，
**进入前已有 `if(mqtt_sleep_mode) { … return; }`** 保证只有 `false→true` 迁移能走到 ⇒ 天然边沿。

为什么难触发：`mqtt_retry_count` 只在 `MQTT_EVENT_DISCONNECTED` 分支自增，
而**每次成功连接都会把它清零**（CONNECTED 分支 `mqtt_retry_count = 0`）。
所以要凑满 `retry_max`（配置值 **30**）次，必须"**连续 30 次断连且期间一次都没连上**"。
在 broker 正常接受连接的情况下不可能出现（每次重连都会清零）。

真正能命中它的场景：**broker 接受 TCP/TLS 但拒绝或关闭 MQTT 会话**
（凭据错误、ACL 拒绝、client_id 被策略封禁）⇒ 不会有 CONNECTED，DISCONNECTED 持续到达 ⇒ 计数累积。

复测方法（二选一，均需改配置或人工介入，本次未做）：
- 把 `data/config/mqtt.json` 的 `password` 临时改成错值 → 重启 → 观察 30 次失败后进入休眠
  （`retry_interval` 10 s × 30 ≈ 5 min）；**测完必须改回**（与 P2-C 的 SSID 复测同一套路）。
- 或在 EMQX 侧临时禁用该 client_id。

**② `LOG_MQTT_PUBLISH_FAIL` 真机未触发**

代码路径审查：4 个累加点都在底层发布函数的"未连接"与 `msg_id < 0` 分支；发射点唯一且在
`cloud_task()` 最顶部（任何早期 return 之前）。聚合窗口 60 s、`FAIL_COUNT` 承载被抑制次数。

为什么难触发：**上层调用者都会先自查 `mqtt_connected`**
（`cloud_send_up` / `cloud_send_set` / `cloud_send_log` 都如此）⇒ 离线时"根本不尝试发"，
不会走到 `msg_id < 0`。真正会命中它的只有：
- **竞态窗口**：连接刚断、`mqtt_connected` 还是 true 的那几毫秒内恰好有发布；
- **`esp_mqtt_client_enqueue` 返回 -1**（outbox 满、载荷超限）。

复测方法：把"踢会话"与"立即发命令/制造命令结果"压缩到同一瞬间（竞态窗口内），
或在 outbox 打满时发布；都属于时序敏感场景，**不适合做稳定回归**，故本次仅记录。

---

## 14. TimeManager 接入（**P2-E 已落地**）

> 状态：已实现并上板验证。提交：`feat(log): integrate time manager logging`
> 前置阅读：§9（Storage bridge）与 §12（WiFi 显式埋点）—— Time 与 **WiFi 同类**：
> **无日志回调接口** ⇒ 只能加显式埋点。

### 14.1 冻结 EventId 的最终处置（10 个）

| EventId | 名称 | Level | 宿主 | 状态 |
|---|---|---|---|---|
| `0x0801` | `TIME_NTP_OK` | INFO | `time_task()` 的 **settled 分支** | ✅ 已埋 / 上板验证 |
| `0x0802` | `TIME_NTP_FAIL` | WARN | —— | ⛔ **无宿主，未实现**（见 §14.3） |
| `0x0803` | `TIME_VALID_ENTER` | INFO | `time_update_valid_event()` 的有效边沿 | ✅ 已埋 / 上板验证 |
| `0x0804` | `TIME_INVALID_ENTER` | WARN | 同上（无效边沿） | ✅ 已埋 / 代码审查（不可运行时触发，见 §14.8） |
| `0x0805` | `TIME_RTC_PROBE` | INFO / WARN | `rtc_init()` 失败·成功 + `time_init()` 禁用分支 | ✅ 已埋 / **三个分支均上板验证** |
| `0x0806` | `TIME_RTC_BOOT_RESTORE` | INFO | `time_init()` 的 RTC 硬同步成功 | ✅ 已埋 / 需 RTC 芯片 |
| `0x0807` | `TIME_RTC_CALIBRATED` | INFO | `time_rtc_calibrate()` 写成功 | ✅ 已埋 / 需 RTC 芯片 |
| `0x0808` | `TIME_RTC_WRITE_FAILED` | WARN | `time_rtc_calibrate()` 写失败 | ✅ 已埋 / 需 RTC 芯片 |
| `0x0809` | `TIME_RTC_VL_FLAG` | WARN | `rtc_read_time()` VL 置位 | ✅ 已埋 / 需 RTC 芯片 + 电池耗尽 |
| `0x080A` | `TIME_RTC_BCD_INVALID` | WARN | `rtc_read_time()` BCD 越界 | ✅ 已埋 / 需 RTC 芯片 |

代码改动性质：**纯增量 `+210 / −0`**（`git diff` 无任何既有行被修改/删除），
唯一例外是把 `|System − RTC|` 多存了一份到函数作用域（`rtc_drift_s`），
供写成功/失败埋点读取 —— **不改变任何既有判定与分支走向**。

### 14.2 埋点表（位置 / 参数）

| # | 位置（既有分支） | 事件 | Level | 参数 |
|---|---|---|---|---|
| 1 | `time_task()` **settled 分支** | `NTP_OK` | INFO | `UNIX`(服务器时间) · `DURATION_MS`(callback→确认耗时) · `ATTEMPT_N`(本 Boot 第几次同步) |
| 2 | `time_update_valid_event()` → valid | `VALID_ENTER` | INFO | `UNIX` · `SOURCE` |
| 3 | `time_update_valid_event()` → invalid | `INVALID_ENTER` | WARN | `UNIX`(被判无效的那个值) · `CAUSE`(=1，低于有效阈值) |
| 4 | `rtc_init()` 探测失败 | `RTC_PROBE` | WARN | `ADDR` · `STATE`=1 · `ERR_CODE`(Wire err) |
| 5 | `rtc_init()` 探测成功 | `RTC_PROBE` | INFO | `ADDR` · `STATE`=2 |
| 6 | `time_init()` **配置禁用** | `RTC_PROBE` | INFO | `STATE`=0（无 `ADDR`：未探测，地址无意义） |
| 7 | `time_init()` RTC 硬同步成功 | `RTC_BOOT_RESTORE` | INFO | `UNIX` |
| 8 | `time_rtc_calibrate()` 写成功 | `RTC_CALIBRATED` | INFO | `UNIX` · `DRIFT_MS` |
| 9 | `time_rtc_calibrate()` 写失败 | `RTC_WRITE_FAILED` | WARN | `UNIX` · `DRIFT_MS` |
| 10 | `rtc_read_time()` VL 置位 | `RTC_VL_FLAG` | WARN | （无参数） |
| 11 | `rtc_read_time()` BCD 越界 | `RTC_BCD_INVALID` | WARN | `RAW`(寄存器打包) · `ERR_CODE`(越界位掩码) |

**参数零新增**，全部复用已冻结 ParamId：`UNIX`(0x35) · `DRIFT_MS`(0x36) · `ADDR`(0x37) ·
`RAW`(0x18) · `ERR_CODE`(0x0C) · `SOURCE`(0x12) · `CAUSE`(0x16) · `STATE`(0x44) ·
`DURATION_MS`(0x06) · `ATTEMPT_N`(0x30)。单条最多 **3** 个参数（≤ `LOG_MAX_PARAMS`=8）。

**`SOURCE` 编码**（`time_source_code()`；`time_source` 是 `const char*`，
而 `LogParamIn` 无 blob 字段且 `LOG_PTYPE_STR` 不可构造 ⇒ 按 P2 定版走**枚举化**）：

| 值 | 含义 |
|---|---|
| 0 | `INVALID`（含"有效边沿早于来源确认"，见 §14.6） |
| 1 | `RTC` |
| 2 | `SNTP` |
| 3 | `MANUAL` |

**`RTC_PROBE.STATE` 编码**：`0`=未探测（配置禁用）· `1`=探测失败 · `2`=探测成功。

**`RTC_BCD_INVALID` 的两个参数**（取证用）：

```
RAW      = (raw[0]<<24)|(raw[1]<<16)|(raw[2]<<8)|raw[3]
           = seconds/minutes/hours/days 的**原始寄存器字节**（BCD，含标志位）
ERR_CODE = 越界位掩码  bit0=sec bit1=min bit2=hour bit3=day bit4=month
```

**`DRIFT_MS` 语义**：`time_rtc_calibrate()` 里**写入之前**的 `|System − RTC|` 毫秒值
（即晶振累计误差）。`-1` = **RTC 读失败**（属"给芯片写初值"场景，无漂移可比）。
秒→毫秒换算带钳制（上限 `INT32_MAX`，约 24.8 天）。

**`RTC_WRITE_FAILED` 不带 `LOG_P_ERR_CODE`**：`rtc_write_time()` 只返回 `bool`，
不暴露子原因（年份越界 / Wire NACK）—— **不为此改函数签名**。

**`RTC_BOOT_RESTORE` 不带 `LOG_P_DRIFT_MS`**（矩阵 §6.2 原列了该参数，**有意偏离**）：
此处的语义是"用 RTC 设 System Time"，而此前 System Time 未设置（≈0），
差值无意义。

> **未埋点的既有错误分支**：`time_set_manual()` 的 RTC 写失败路径（原 `:756`）。
> 该函数**全仓库无调用者**（`time_set_manual_string()` 亦无调用者）
> ⇒ 埋点不可达，与 P2-D 的 `LOG_CLOUD_FRAG_FAIL` 同处理：**不记，也不删函数**。

### 14.3 SNTP 特殊处理（本节是 Time 接入的核心）

**① 成功点必须挂在"确认"处，不能挂在 SNTP callback 里。**

`time_init()` 结束前会 `esp_sntp_set_time_sync_notification_cb(time_on_sntp_sync)`，
该 callback 运行在 **lwip 上下文**，只允许置标记（`sntp_sync_seq++` /
`sntp_sync_notify_ms`），严禁 I2C / Serial / 长耗时操作。
真正的"确认"发生在 `time_task()` 的 **settled 分支**：

```
callback 通知（seq 变化） → loop 侧：
    st == IN_PROGRESS  ⇒ 等 adjtime 收敛，超时 SNTP_SMOOTH_TIMEOUT_MS(30s) 兜底
    st == COMPLETED    ⇒ 瞬时窗口内采到，明确完成
    st == RESET        ⇒ 再等 SNTP_SMOOTH_SETTLE_MS(3s) 稳定窗口
  ↓ settled
  rtc_last_calibrate_seq = seq; last_ntp_sync_time = sntp_sync_tv_sec;
  synced_once = true; time_source = "SNTP"      ← 埋 NTP_OK 就在这里
```

⇒ **`NTP_OK` 是"callback 通知 + loop 延迟确认"的产物**，与项目铁律一致。

**② `LOG_TIME_NTP_FAIL`（0x0802）＝ 冻结 EventId / 当前无宿主 / 未实现。**

本 SDK 只提供成功通知，**没有失败回调，状态枚举也没有失败态**：

| 证据 | 事实 |
|---|---|
| `esp_sntp.h:83` | `typedef void (*sntp_sync_time_cb_t)(struct timeval *tv);` —— **只此一个**回调，且是成功通知 |
| `esp_sntp.h:67-70` | `sntp_sync_status_t` = `RESET / COMPLETED / IN_PROGRESS`，**无失败态** |
| `ESP-IDF 头注释 :118-121` | `COMPLETED` 后**自动回落 `RESET`**；"尚未更新"**也是** `RESET` |
| 全仓库 | 除 `esp_sntp_set_time_sync_notification_cb` 外没有任何失败判定 |

⇒ 设备**现在无法知道 NTP 是否失败**（这是 Time 模块最大的诊断盲区）。
要产出该事件必须**新增**"已启动 N 秒仍无 `sntp_sync_seq` ⇒ 告警"的看门狗，
即**扩展 TimeManager 状态机**，超出 P2「只加观测」的范围
⇒ 编号保留、**不埋点**（与 `LOG_CLOUD_FRAG_FAIL` 同处理）。

**③ `SNTP_SMOOTH_TIMEOUT_MS` 分支绝不能当作 NTP 失败。**

```c
if (st == SNTP_SYNC_STATUS_IN_PROGRESS) {
    settled = (waited_ms >= SNTP_SMOOTH_TIMEOUT_MS);   // 30s 兜底
}
```
走到这里说明**本次同步已经成功**（callback 已通知），只是 adjtime 未收敛、
不再等它。把它标成 `NTP_FAIL` 会**污染云端「NTP 是否可用」的判断**。
⇒ 改为在 `NTP_OK` 的 `DURATION_MS` 上暴露该征兆：
`DURATION_MS ≈ 30000` 即表示走了超时兜底。

**④ 不用 `status != IN_PROGRESS` 判成功**（项目铁律）：
`RESET` 同时表示"从未同步"与"已同步后回落"，用它会**把没同步过当成同步成功**。
`NTP_OK` 的前置条件是 `sntp_sync_seq` 已变化（callback 确实通知过），
不存在这个误判。

### 14.4 ★ RTC 探测三分支与**一处不可达陷阱**（实测发现）

```
time_init():
    rtc_enabled = config_get_rtc_enable();
    if (rtc_enabled && rtc_init()) { ... }      ← 短路求值！
    else {
        if (rtc_enabled) { /* init 失败：rtc_init() 内已埋 WARN 版 */ }
        else            { /* 配置禁用：在此埋 INFO 版 STATE=0 */ }
    }
```

⚠️ **`rtc_init()` 内部的 `if (!rtc_enabled)` 分支是死代码**：`time_init()` 是它**唯一**的
调用者（全仓库确认），而调用条件是 `rtc_enabled && rtc_init()` —— 短路求值使得
`!rtc_enabled` 时 `rtc_init()` 根本不会被调用。
⇒ 最初把"配置禁用"的埋点写在那里，**上板实测该记录根本不出现**（`rtc.enable=false`
启动时队列里没有 `RTC_PROBE`）。
⇒ 已把它移到 `time_init()` 中**可达**的 `else { if (!rtc_enabled) }` 分支，并在
`rtc_init()` 内留下注释说明为何不在此埋点。

**这是"埋点必须验证可达性"的一个实例**：分支存在 ≠ 分支可达。

三分支的区分方式（`rtc_present=false` 时尤其有用）：

| 场景 | 记录 | 额外字段 |
|---|---|---|
| 配置禁用 | `RTC_PROBE` **INFO** `STATE=0` | 无（未探测） |
| 芯片不在 | `RTC_PROBE` **WARN** `STATE=1` | `ADDR=81` `ERR_CODE=2`(Wire NACK) |
| 探测成功 | `RTC_PROBE` **INFO** `STATE=2` | `ADDR=81` |

### 14.5 边沿去重（`VL_FLAG` / `BCD_INVALID`）—— 必须

`rtc_read_time()` 有**三条**调用路径：

| 调用者 | 频率 |
|---|---|
| `time_init()` | 每 Boot 1 次 |
| `time_rtc_calibrate()` | 每 SNTP 同步 1 次（默认 24h） |
| **`time_query()`**（`system.get_time` 命令，`command_manager.cpp`） | **每次状态查询** |

VL 置位与 BCD 越界都是**会持续存在的条件**（VL 只有写秒寄存器才清）
⇒ 若按调用上报，一次 `system.get_time` 轮询就重复写一条。

处置：各自一把 `static bool` 边沿锁，
**解锁点选在"条件确实消失"处**：

| 锁 | 置位 | 解锁 |
|---|---|---|
| `rtc_vl_reported` | `rtc_read_time()` 见 VL | `rtc_write_time()` **成功**（写秒寄存器清 VL） |
| `rtc_bcd_reported` | `rtc_read_time()` BCD 越界 | `rtc_read_time()` **成功**（数据合法） |

这样"异常再次出现"（电池再次耗尽 / 再次读到坏数据）能重新上报。

### 14.6 `VALID_ENTER.SOURCE` 的语义边界（**实测竞态**）

`SOURCE` 取的是**边沿发生瞬间**的 `time_source`。SMOOTH 同步下，系统时间会在
SNTP **确认（settled）之前**就越过有效阈值（`TIME_VALID_START_TIMESTAMP`），
而 `time_task()` 第 3 步（valid 维护）与第 2 步（SNTP 确认）是**同一轮内的先后判断**。

两次上板实测拿到**两种顺序**：

```
（首次验证）  NTP_OK(DURATION_MS=454)  →  VALID_ENTER(SOURCE=2 SNTP)
（配置恢复后）VALID_ENTER(SOURCE=0)    →  NTP_OK(DURATION_MS=484)
```

⇒ **`VALID_ENTER.SOURCE=0` 不是错值**：此刻模块确实还没归属来源。
但它**不能**单独用来判断"时间从哪来"。云端应把
`NTP_OK` / `RTC_BOOT_RESTORE` 作为**来源的权威记录**，`VALID_ENTER` 只负责
"何时开始有效 / 有效时刻的值是多少"。

（要让 `SOURCE` 永远准确，需要把有效边沿推迟到来源确认之后 —— 那属于**改状态机**，不做。）

### 14.7 上板验证（2026-09-18，COM8，broker `guo_feeder/log`）

方法：先用 `test/mqtt_log_probe.py` 类探针订阅（本阶段用 `.pio/p15run/p2e_probe.py`），
**再烧录/复位板子** ⇒ 才能抓到 Boot 头几秒产生的记录（RTC 探测在 WiFi 之前）；
随后用 `.pio/p15run/log_decode.py` **解到记录级**（`event_id` + 每个 ParamId 的真值）。

**① 正常启动（`rtc.enable=true`，板上无 RTC 芯片）**

```
[batch] fmt=2 boot=2 seq=1537..1543 count=7 flags=1
  seq=1537 INFO LOG_CFG_LOAD_DONE        ERR_CODE(u32)=1
  seq=1538 INFO LOG_WIFI_CONNECT_START   SSID_HASH=2088390601 ATTEMPT_N=1 WAS=0 STATE=1
  seq=1539 WARN LOG_TIME_RTC_PROBE       ADDR(u32)=81 STATE(u32)=1 ERR_CODE(u32)=2   ← ★
  seq=1540 INFO LOG_WIFI_CONNECTED       CONNECT_MS=1966 RSSI=-60 SSID_HASH=… WAS=1 STATE=2
  seq=1541 INFO LOG_TIME_NTP_OK          UNIX=1789666817 DURATION_MS=454 ATTEMPT_N=1  ← ★
  seq=1542 INFO LOG_TIME_VALID_ENTER     UNIX=1789666817 SOURCE(u32)=2               ← ★
  seq=1543 INFO LOG_MQTT_CONNECTED       OUTBOX=22 WAS=0 STATE=1
```

- **`LOG_TIME_RTC_PROBE` 终于解释了 `rtc_present=false`**：
  `ADDR=0x51`、`STATE=1`（探测失败）、`ERR_CODE=2`（Wire `endTransmission` 收到地址 NACK
  ⇒ 从机无应答 ⇒ 芯片不存在）。**这条记录正是本模块此前最大的观测空白。**
- `NTP_OK` 的 `DURATION_MS=454` ⇒ adjtime 很快收敛（**没有**走 30s 兜底）。
- `VALID_ENTER.SOURCE=2` ⇒ 来源正确记为 SNTP。
- 记录顺序 `NTP_OK → VALID_ENTER` 与设计一致。
- `NTP_FAIL` **未出现** ✅（预期：未实现）。
- RTC 相关 INFO（`BOOT_RESTORE`/`CALIBRATED`）与 WARN（`WRITE_FAILED`/`VL_FLAG`/
  `BCD_INVALID`）**均未出现** ✅（板上无 RTC ⇒ 这些分支不可达）。

**② 配置禁用（`rtc.enable=false`）—— 验证 §14.4 的修复**

```
[batch] fmt=2 boot=4 seq=1281..1287 count=7 flags=1
  seq=1283 INFO LOG_TIME_RTC_PROBE       STATE(u32)=0            ← ★ 修复后才出现
  seq=1286 INFO LOG_TIME_NTP_OK          UNIX=1789671663 DURATION_MS=84 ATTEMPT_N=1
  seq=1287 INFO LOG_TIME_VALID_ENTER     UNIX=1789671663 SOURCE(u32)=0   ← 见 §14.6
```

⇒ `STATE=0` INFO、**无** `ADDR`/`ERR_CODE`、**无** WARN ⇒ 与"芯片不在"可清晰区分。
（修复前这一整个 Boot **没有任何** `RTC_PROBE` 记录。）

**③ 配置恢复（`rtc.enable=true`）**

```
boot=5: seq=1539 INFO LOG_TIME_RTC_PROBE  STATE=0            ← 恢复前的最后一次启动
boot=6: seq=1795 WARN LOG_TIME_RTC_PROBE  ADDR=81 STATE=1 ERR_CODE=2   ← ★ WARN 版回归
```

现场已恢复 `rtc.enable=true`（`config_query` 确认）。

**④ 回归**：全量 **196/196 = 100%，0 MISS**（A 56 / B 66 / C 21 / D 30 / E 23）。

### 14.8 未验证项（如实记录）

| 项 | 原因 | 复测方法 |
|---|---|---|
| `RTC_BOOT_RESTORE` / `RTC_CALIBRATED` / `RTC_WRITE_FAILED` / `RTC_VL_FLAG` / `RTC_BCD_INVALID` | **板上 `rtc_present=false`**（无 PCF8563T 芯片）⇒ 这些分支全部不可达 | 焊上 RTC 芯片（或接模块）后正常启动即可；`VL_FLAG` 还需**电池耗尽**（或先 `rtc_write_time` 清 VL 再断电） |
| `INVALID_ENTER` | 需要**有效→无效**的迁移，即系统时钟被设到 `2026-07-01` 之前；而设备**没有**任何 `set_time` 通道（`time_set_manual*` 无调用者，`time_manager_set_time()` 是预留空实现） | 需先给设备加"设时间"入口，或在 RTC 芯片存在时用**倒退的 RTC 值**触发。当前判据 = 代码路径审查 + 同函数的**有效边沿已上板验证**（互为镜像分支，共用同一 `last_reported` 门控） |
| `NTP_FAIL` | ⛔ 设计上无宿主（§14.3） | 不适用（除非评审后新增看门狗） |
| `time_set_manual()` 的写失败埋点 | 该函数**无调用者**（不可达）⇒ 本次**未埋点** | 不适用（同上，属死代码） |

### 14.9 ★★ 回归夹具加固（本节对后续所有模块都适用）

P2-E 首次全量回归出现 **A 48/56 + B 62/66**，两个**不同**的原因，必须分开处理：

**① A 段 8 条 = MQTT 抖动假 MISS（网络问题，不是缺陷）**

判据：`grep -ac "Writing didn't complete"` = 1、`MQTT error event` = 1。
TLS 写入超时会让 loop 阻塞约 8s ⇒ 串口静默 ⇒ 命令未被及时处理。
**复跑即 56/56 全绿**（实测）。

**② B 段 4 条 = 真问题，但根因在**夹具**而不是实现**

失败断言：`replay=8`（实测 9）、`qused=8`（实测 9）、`qused=0`（实测 1）、`rarmed=0`（实测 1）。

**根因（比 P2-D 那次更根本）**：`cloud_collect_batch()` 里有

```c
if (n > 0 && s_cloud_q[slot].boot_seq != s_cloud_batch[0].boot_seq) break;
```

⇒ **批次在 `boot_seq` 变化处截断**。于是"一批装下全部补发记录 ⇒ `used` 归 0
⇒ 补发 sweep 走完 ⇒ `rarmed=0`"这个前提，**只在积压恰好同属一个 Boot 且不超批次上限时成立**。

而 P2-E 让每个 Boot **多了一条 Flash 记录**：`LOG_TIME_RTC_PROBE` 是 **WARN**
（探测失败也照样落 Flash），在无 RTC 的板上**必现**。
⇒ 积压 = 上一 Boot 的 **8** 条 + 本 Boot 的 **1** 条（**两个 boot_seq**）
⇒ 单批永远排不空 ⇒ 那 4 条断言**必然失败**。

（**这不是实现缺陷**：`cloud_poll()` 会把余下记录作为**下一批**继续发，只是夹具没有
足够多的 ACK 轮次；所需轮次数 = 积压里不同 `boot_seq` 的个数，同样与历史相关。）

**夹具修法**（按 P2-D 的路线：不迁就实现，只消除对"日志批次偶然性"的依赖）：

1. **`test/serial_batch.py` 新增内联正则期望**：期望串里成对的 `/正则/` 片段按正则处理，
   其余部分按字面量。**向后完全兼容**（无 `/` ⇒ 仍是子串匹配）。
   自检脚本：`.pio/p15run/expect_hit_selftest.py`（13 例，含 4 条**负向探针**）。
2. **F2-B 的判据改为"非零"**：`replay` 修复前在该场景**恒为 0**，
   修复后 > 0 ⇒ `replay=/[1-9][0-9]*/` 精确且与积压历史无关。
   同时把 `qused=8/qused=0/rarmed=0` 换成结构性判据：
   `gcfloor=/[1-9][0-9]*/`（钳制生效）、`replay_seq=/[1-9][0-9]*/`（游标已推进）、
   `ack ok boot=`（旧批次 ACK **被接受**而非被判 DUPLICATE）、`giveup=0`。
   断言总数 **196 条不变**。

> ⚠️⚠️ **写内联正则时必须把字段名写进字面量**：`replay=/[1-9][0-9]*/`
> 而**不是** `/[1-9][0-9]*/`。纯正则会 `search` **整行**，`offskip=1`、`ack_ok=2`
> 之类的其它数字会造成**假命中** —— 实测 `replay=0` 也被判 OK。
