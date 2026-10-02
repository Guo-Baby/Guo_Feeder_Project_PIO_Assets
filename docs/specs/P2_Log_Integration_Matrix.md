# P2 模块日志接入矩阵

> 版本：v1.0（P2 前置准备）
> 基线 commit：`65b4523 fix(log): complete P1.5 ack hole and replay cursor fixes`（其后 `1d0b47c` 为仓库整理，不含代码）
> 性质：**分析文档**。未修改任何 `src/` 生产代码、未新增 EventId、未新增 ParamId、未修改 LogManager API。
> 配套文档：`docs/archive/log/LogManager-P2接入准备审查0918.md`（EventId / ParamId / 高频风险 / 初始化顺序 / 风险清单）
> 接口细节见：`docs/specs/LogManager-Integration-Guide.md`

---

## 0. 阅读约定

### 0.1 事实来源

本矩阵所有 `文件:行号` 均来自当前工作区实读（HEAD `65b4523` + 未提交的 0 处 src 改动）。

### 0.2 Level 判定（冻结，无 EventId 级例外）

| Level | Flash | Cloud | 判定规则 |
|---|---|---|---|
| `LOG_LVL_DEBUG` | ✗ | ✗ | **不进 Log 系统**（`log_emit` 直接 return false）。仅侧信道计数 |
| `LOG_LVL_INFO` | ✗ | ✓ | 正常状态变化 / 动作完成 / 成功结果 |
| `LOG_LVL_WARN` | ✓ | ✓ | 可自愈的异常、重试、降级、超时 |
| `LOG_LVL_ERROR` | ✓ | ✓ | 功能失败但系统仍可运行 |
| `LOG_LVL_CRITICAL` | ✓ | ✓ | 安全相关 / 数据可能损坏 / 需要立刻 flush（IMM） |

> ⚠️ **INFO 不落盘但占 RAM 环与云队列**——"INFO 洪泛"同样会挤掉真正的 WARN+。见 §7 与审查报告 §5。

### 0.3 硬约束（写代码时必须遵守）

1. `log_emit()` 内部用 `portENTER_CRITICAL()`（**非 `_ISR` 变体**）⇒ **禁止在 ISR 调用**。NimBLE 回调（`MiAdvCallback::onResult`）虽非 ISR，但属 BLE host 任务上下文，**同样禁止**（见 §11 BLE）。★ **Phase 5-C 已按此执行**：BLE 的 4 处埋点**全部落在 `MiThermometer_task()`（loop）**，callback 内 `log_emit` 站点 **= 0**（objdump 证明）。
2. 参数 `param_count > LOG_MAX_PARAMS(8)` ⇒ **整体拒绝**，不截断。
3. `log_emit()` 返回值必须检查的场景：CRITICAL 级。返回 `false` = 未进环（环未就绪 / 参数超限 / DEBUG）。
4. `log_emit()` **线程安全**（临界区保护），多任务可调用；但仍建议集中在 `*_task()`（loop 上下文）调用。
5. **业务模块禁止调用** `log_flash_*` / `log_meta_*` / `log_seg_*` / `log_cloud_test_*`。

---

## 1. System / Boot

| 项 | 内容 |
|---|---|
| 源码文件 | `src/main.cpp`（1748 行）、`src/services/system_command.cpp`（624）、`src/services/system_state.cpp`（464）、`src/app/computer_reset.cpp`（475） |

### 1.1 当前状态

| 检查项 | 现状 |
|---|---|
| 已有日志 | ✅ **Phase 5-A 已接入 6 个业务埋点**（`6194e3b` + `9c32675`）：`LOG_SYS_RESET_ABNORMAL` / `BOOT_INCOMPLETE_PREV` / `RESTART_REQUESTED` / `RESTART_EXECUTED` / `CRITICAL_OP_UNDERFLOW` / `BOOT_COMPLETE`；另有 `main.cpp` 的串口测试钩子（`logt` 等） |
| 错误处理 | `system_command.cpp` 有完整 Safe Restart V2 状态机（`RESTART_IDLE/REQUESTED/PENDING/RESTARTING`）+ Critical Operation 计数（acquire/release/count） |
| 状态机 | ✅ 有：Safe Restart 状态机、ComputerReset 脉冲状态机 |
| 关键状态迁移点 | 重启请求 → 10s 安全窗口 → `ESP.restart()`（`system_command.cpp:312`，全系统唯一）；Critical Op 计数归零/泄漏 |

### 1.2 建议接入（**下方为 P2 阶段的设计建议；Phase 5-A 落地结果见 1.3，两者以 1.3 为准**）

| 位置（文件:行 / 函数） | 事件 | EventId | Level | Params | 原因 |
|---|---|---|---|---|---|
| `main.cpp:58` `setup()` LittleFS 挂载失败 | FS 挂载失败 | `LOG_SYS_FS_MOUNT_FAILED` | CRITICAL | — | **该点在 `log_init()`（:80）之前**，见风险 R-1 |
| `main.cpp:170` `config_boot_validate()` 失败 | 上次启动未完成 | `LOG_SYS_BOOT_INCOMPLETE_PREV` | CRITICAL | `LOG_P_BOOT_SEQ` | 反复重启的核心判据；当前只打串口 |
| `main.cpp:173` setup 走完 | 启动完成 | `LOG_SYS_BOOT_COMPLETE` | INFO | `LOG_P_BOOT_SEQ`, `LOG_P_INIT_MS`, `LOG_P_RESET_REASON` | 每次启动 1 条，建立日志时间轴原点 |
| 启动后（读 `system_command_reset_reason()`） | 复位来源 | `LOG_SYS_RESET_NORMAL` / `LOG_SYS_RESET_ABNORMAL` | INFO / CRITICAL | `LOG_P_RESET_REASON` | 区分正常重启与 panic；**当前 `reset_reason` 从不落盘** |
| `system_command_request_restart()` :326 | 请求重启 | `LOG_SYS_RESTART_REQUESTED` | INFO | `LOG_P_REASON`, `LOG_P_HOLD_MS` | 解释后续所有重启的发起方 |
| `system_command_task()` :312 `ESP.restart()` 前 | 执行重启 | `LOG_SYS_RESTART_EXECUTED` | INFO（IMM） | `LOG_P_HOLD_MS` | IMM = 立即 flush；重启前最后一条 |
| 重启取消路径 | 取消重启 | `LOG_SYS_RESTART_CANCELLED` | INFO | `LOG_P_REASON` | 与 REQUESTED 配对，便于对账 |
| `system_command_critical_operation_release()` :452 计数下溢 | Critical Op 泄漏/重复释放 | `LOG_SYS_CRITICAL_OP_UNDERFLOW` | CRITICAL | `LOG_P_COUNT` | **P0 类缺陷**：泄漏 = 永久无法重启 |
| 堆低水位检测（新增周期检查） | 堆不足 | `LOG_SYS_HEAP_LOW` | WARN | `LOG_P_FREE_BYTES`, `LOG_P_MIN_FREE` | 需模块侧限流（避免每 loop 一条） |
| PSRAM 分配失败点 | PSRAM 耗尽 | `LOG_SYS_PSRAM_ALLOC_FAILED` | CRITICAL | `LOG_P_NEED_BYTES`, `LOG_P_FREE_BYTES` | 项目铁律：PSRAM 失败回退 DRAM，需可见 |
| `computer_reset_trigger()` :441 | 电脑重启脉冲 | `LOG_CRESET_PULSE` | INFO | `LOG_P_HOLD_MS` | 动作完成 |
| `computer_reset_task()` 安全超时 | 脉冲回收超时 | `LOG_CRESET_SAFETY_TIMEOUT` | WARN | `LOG_P_HOLD_MS` | GPIO 卡在 active 有硬件风险 |
| `computer_reset_ctx_release()` / 池耗尽 | 上下文池耗尽 | `LOG_CRESET_POOL_EXHAUSTED` | ERROR | `LOG_P_ACTIVE`, `LOG_P_CAPACITY` | 需扩容依据 |

### 1.3 实际接入（Phase 5-A，2026-09-20 · `6194e3b` + `9c32675`）

| EventId | Level | 参数（实际） | 实际宿主（唯一语义定义点） | 与 1.2 建议的差异 |
|---|---|---|---|---|
| `LOG_SYS_BOOT_INCOMPLETE_PREV` 0x0102 | CRITICAL | `LOG_P_RESET_REASON` | **`config_manager.cpp` `config_init()`**：`bool last_boot_ok = json_storage_exists(CONFIG_BOOT_FLAG_FILE);` 之后、`json_storage_remove()` 之前 | ★ **宿主不是 `main.cpp config_boot_validate()` 失败** —— 事实由 **boot flag 机制**定义（`config_init()` 读、`config_boot_validate()` 写），按"埋定义点"归 ConfigManager；★ **参数不是 `LOG_P_BOOT_SEQ`**（boot_seq 已在每个批次头 `log_cbor.h:101`，再记一次是冗余） |
| `LOG_SYS_RESET_ABNORMAL` 0x0103 | CRITICAL | `LOG_P_RESET_REASON` | `system_command.cpp` `system_command_init()`：紧接 `s_reset_reason = (uint8_t)esp_reset_reason();` 之后 | 按建议接入；**只记异常**（`RESET_NORMAL` 未接入） |
| `LOG_SYS_RESTART_REQUESTED` 0x0109 | INFO | `LOG_P_COUNT` = critical 计数 | `system_command.cpp` `system_command_request_restart()` 的 `if (first_request)` 块首 | 参数改为 `LOG_P_COUNT`（请求时的实际计数，用于解释"为什么还要等"） |
| `LOG_SYS_RESTART_EXECUTED` 0x010A | INFO | **无参** | `system_command.cpp` `system_command_task()`：`Serial.flush()` 之后、`ESP.restart()` 之前 | ★ **`（IMM）` 未实现**：无生产 flush 消费链 ⇒ INFO 不落 Flash，**可能因重启丢失** ⇒ 登记 **`PROTO-2`（OPEN / DEFERRED）**，本阶段不修 |
| `LOG_SYS_CRITICAL_OP_UNDERFLOW` 0x010C | CRITICAL | **无参** | `system_command.cpp` `system_command_critical_operation_release()` 的 `if (!ok)` 下溢分支 | 按建议接入（保留既有 `syscmd_log("E",…)` 串口输出） |
| `LOG_SYS_BOOT_COMPLETE` 0x0101 | INFO | `LOG_P_INIT_MS` = setup 总耗时 | `main.cpp` `setup()`：`config_boot_validate()` **成功分支**；入口新增局部变量 `setup_begin_ms` | 参数只留 `LOG_P_INIT_MS`（`LOG_P_BOOT_SEQ` 冗余） |

**本阶段明确不接入 / 未实现**

| EventId | 原因 |
|---|---|
| `LOG_SYS_RESTART_CANCELLED` 0x010B | **架构无合法宿主**：`system_command.h` 明示 Restart **不可取消**（无 cancel 接口），状态机 `IDLE→REQUESTED→PENDING→RESTARTING` **单向不可逆**；`request_restart()` 返回 false 的分支语义是"**拒绝**"而非"**取消**" ⇒ 挂上去属**语义伪造** |
| `LOG_SYS_RESET_NORMAL` 0x0104 | 用户清单明示排除；`BOOT_COMPLETE` 已提供"启动发生"的锚点 |
| `LOG_SYS_INIT_FAILED` 0x0105 · `FS_MOUNT_FAILED` 0x0106 | `FS_MOUNT_FAILED` 与 `LOG_STG_FS_UNAVAILABLE`(0x0301，已有宿主) **语义重叠** |
| `LOG_SYS_PSRAM_ALLOC_FAILED` 0x0107 · `HEAP_LOW` 0x0108 | 运行时阈值定义不明确；`HEAP_LOW` 若接入必须先定门控（跨阈值边沿 + ≥60 s），否则内存枯竭时形成日志风暴 |
| `LOG_SYS_FS_MOUNT_FAILED` 0x0106 | 另见风险 `R-1`：该点在 `log_init()` **之前**，物理上无法记录 |

### 1.4 SystemEvent Registry 状态（Phase 5-B-2，2026-09-21 · `7fb9437`）

**状态：✅ Registry 已修复**（此前为"存在错位"）。

| 项 | 修复前 | 修复后 |
|---|---|---|
| `SYSTEM_EVENT_COUNT`（`event_manager.h:53`） | `(EVENT_ERROR + 1)` = **14**（偏小 3） | **`(EVENT_VALVE_ERROR + 1)` = 17** |
| `event_names[]`（`event_from_string`，`event_manager.cpp:143-159`） | 15 项；idx10 为幻影 `"EVENT_CLOUD_COMMAND"`；`EVENT_VALVE_*` 缺字符串 | **17 项**，与 enum 逐项对齐 |
| `event_names[]`（`event_to_string`，`event_manager.cpp:180-199`） | 同上 | **17 项**，与 enum 逐项对齐 |
| `SystemEvent` enum 数值 / 顺序 | — | **完全未变**（未重排、未插入、未改数值） |

修复前后果（已消除）：`event_to_string(EVENT_WEIGHT_ERROR)` 返回 `"EVENT_WEIGHT_READY"`；`event_from_string("weight_error")` 返回 `EVENT_ERROR`(13)；`"EVENT_ERROR"` 越界永不可解析；`event_to_string(EVENT_VALVE_*)` 返回 `"EVENT_UNKNOWN"`。

**职责边界（本阶段顺带明确）**

| 组件 | 职责 | 不在职责内 |
|---|---|---|
| **EventManager** | 负责**事件总线注册**与**字符串映射**（`SystemEvent` enum ↔ `event_names[]` ↔ `to/from`） | **不负责业务日志记录** —— 业务事件事实由各自**发布方模块**记录（本项目既定原则）；EventManager 自身只记自身运行异常（`P2-M`） |
| **Workflow Event Trigger** | 由 `workflow.cpp` 的 `event_` 闸门 + `event_from_string()` 解析实现 | **当前仍关闭**（`WORKFLOW_EVENT_ENABLED = 0`，`workflow.h:18`）⇒ **不属于本阶段范围**；启用前需先解决 `P0-1b`（`event_` / `event.` 前缀规范不一致，见 `未修复的问题.md`） |

> ⚠️ 因 `WORKFLOW_EVENT_ENABLED = 0`，`event_from_string` / `event_to_string` 及两张表**当前被 `--gc-sections` 从最终镜像剔除** ⇒ 本修复**不会立即改变运行行为**（对象文件层面已确认生效：表符号 `0x3c` → `0x44`）。


---

## 2. Config

| 项 | 内容 |
|---|---|
| 源码文件 | `src/services/config_manager.cpp`（3695）、`src/services/config_manager.h`（684） |

### 2.1 当前状态

| 检查项 | 现状 |
|---|---|
| 已有日志 | ❌ 无 LogManager 接入。有内部 `cfg_log(level, fmt, ...)`（`config_manager.cpp:207`）→ 走 `ConfigLogCallback`，**默认静默**，且**未在 main.cpp 注册**（已注册的只有 `bin_storage` 与 `command_manager`） |
| 错误处理 | ✅ 非常完整：Active/Backup 双版本、原子写、Boot Validation、Backup 恢复、提交回滚、版本重建、出厂重置 |
| 状态机 | ✅ `CONFIG_COMMIT_*` 提交状态机 + 重启倒计时状态机（`config_restart_pending/now/cancel`） |
| 关键状态迁移点 | 模块加载（Active→Backup 回退）、Save 事务（轮转→写→校验→提交/回滚）、Boot Validation（成功/失败→Backup 恢复）、重启倒计时（PENDING→执行/超时） |

### 2.2 建议接入

| 位置 | 事件 | EventId | Level | Params | 原因 |
|---|---|---|---|---|---|
| `config_init()` :1034 各模块加载完成 | 配置加载完成 | `LOG_CFG_LOAD_DONE` | INFO | `LOG_P_MODULE`, `LOG_P_VERSION`, `LOG_P_COUNT` | 每次启动 N 条（N = 模块数），说明用的是哪份配置 |
| `load_module()` :373 单模块加载失败 | 模块加载失败 | `LOG_CFG_MODULE_LOAD_FAILED` | ERROR | `LOG_P_MODULE`, `LOG_P_ERR_CODE` | 配置损坏的首发信号 |
| `load_module()` :394-401 回退 Backup | 从 Backup 恢复 | `LOG_CFG_RECOVERED_FROM_BACKUP` | WARN | `LOG_P_MODULE` | 说明 Active 已损坏 |
| `recover_module()` :524/537/547 恢复失败 | 恢复失败 | `LOG_CFG_MODULE_LOAD_FAILED` | ERROR | `LOG_P_MODULE`, `LOG_P_ERR_CODE` | 两份都坏 = 即将出厂重置 |
| `config_save()` :1213 成功 | 保存成功 | `LOG_CFG_SAVE_OK` | INFO | `LOG_P_MODULE`, `LOG_P_COUNT` | — |
| `config_save()` :1213 失败 | 保存失败 | **建议新增 `LOG_CFG_SAVE_FAILED`** | ERROR | `LOG_P_MODULE`, `LOG_P_ERR_CODE` | 现有只有 COMMIT_FAILED_ROLLBACK，覆盖不到"保存整体失败但没回滚" |
| 提交失败回滚 | 提交失败已回滚 | `LOG_CFG_COMMIT_FAILED_ROLLBACK` | ERROR | `LOG_P_MODULE`, `LOG_P_ERR_CODE` | 数据安全事件 |
| 版本文件重建 | 版本重建 | `LOG_CFG_VERSION_REBUILT` | WARN | `LOG_P_MODULE`, `LOG_P_VERSION` | 说明 version.json 丢失 |
| 出厂重置 | 恢复出厂 | `LOG_CFG_FACTORY_RESET` | WARN | `LOG_P_MODULE`, `LOG_P_REASON` | 不可逆变 |
| 写请求被拒 | 写被拒 | `LOG_CFG_WRITE_REJECTED` | WARN | `LOG_P_MODULE`, `LOG_P_REASON` | 常见于重启待决期间 |
| 配置变更生效 | 变更应用 | `LOG_CFG_CHANGE_APPLIED` | INFO | `LOG_P_MODULE`, `LOG_P_KEY` | ⚠️ `LOG_P_KEY` 语义为字符串，**当前不可达**（见审查报告 §4.2） |
| `config_restart_pending()` :3309 倒计时超时 | 重启超时 | `LOG_CFG_RESTART_TIMEOUT` | INFO | `LOG_P_REMAIN_MS` | — |

> **接入方式建议（不改 ConfigManager 内部结构）**：在 `main.cpp` 注册 `config_set_log_callback()` 的 bridge，把 `cfg_log("E"/"W"/"I")` 直接映射到 LogManager；再在 8 个关键语义点（上表）补显式 `log_emit()`。前者零侵入拿到全部既有错误，后者补足语义。

---

## 3. Storage

| 项 | 内容 |
|---|---|
| 源码文件 | `src/storage/file_storage.cpp`（782）、`src/storage/json_storage.cpp`（994）、`src/storage/bin_storage.cpp`（733）、`src/automation/workflow_storage.cpp`（1583） |

### 3.1 当前状态

| 检查项 | 现状 |
|---|---|
| 已有日志 | ⚠️ **有回调机制但从未注册，静默丢弃**：`json_storage` 有 **37 处 E/W**（`js_log("E")` 30 + `js_log("W")` 7）、`file_storage` 有 **30 处 E/W**（`fs_log("E")` 24 + `fs_log("W")` 6）⇒ 共 **67 条**错误/警告完全不可见；`bin_storage` 16 处 E/W 只打到串口（`main.cpp:95` 注册 `bin_log_serial`） |
| 错误处理 | ✅ 完整：原子写（tmp→rename）、CRC 校验、写后校验、事务恢复、目录创建 |
| 状态机 | 无显式状态机；函数级同步返回码 |
| 关键状态迁移点 | FS 可用/不可用、写入成功/失败、rename 成功/失败（= 原子写是否成立）、CRC 校验通过/失败 |

### 3.2 建议接入

| 位置 | 事件 | EventId | Level | Params | 原因 |
|---|---|---|---|---|---|
| `file_storage.cpp:129` "LittleFS unavailable" | FS 不可用 | `LOG_STG_FS_UNAVAILABLE` | CRITICAL | — | FS 挂了 = LogManager 自己也落不了盘 |
| rename 失败 `:277` | 原子写失败 | `LOG_STG_ATOMIC_WRITE_FAILED` | ERROR | `LOG_P_PATH`⚠️, `LOG_P_ERR_CODE` | 原子性被破坏，可能产生 tmp 残留 |
| 写校验失败 | 写校验失败 | `LOG_STG_WRITE_VERIFY_FAILED` | ERROR | `LOG_P_PATH`⚠️, `LOG_P_STORED_CRC`, `LOG_P_CALC_CRC` | 静默数据损坏 |
| CRC 失败 | CRC 校验失败 | `LOG_STG_CRC_FAILED` | CRITICAL | `LOG_P_STORED_CRC`, `LOG_P_CALC_CRC` | — |
| 读失败 `:390` | 读失败 | `LOG_STG_READ_FAILED` | WARN | `LOG_P_PATH`⚠️ | — |
| 事务恢复 | 事务已恢复 | `LOG_STG_TXN_RECOVERED` | WARN | `LOG_P_STAGE` | 上次写入被打断 |

> **最高性价比动作**：P2 第一步只需在 `main.cpp` 加两行注册 `json_storage_set_log_callback()` / `file_storage_set_log_callback()`（bridge 到 LogManager + 保留串口），即可让 **67 条静默错误**变为可观测，且**零业务代码改动**。
> ⚠️ `LOG_P_PATH` 是字符串语义，**当前 `LOG_PTYPE_STR` 不可达**（`LogParamIn` 无 blob 字段）⇒ 路径只能哈希化或省略，见审查报告 §4.2。

---

## 4. WiFi

| 项 | 内容 |
|---|---|
| 源码文件 | `src/services/wifi_module.cpp`（347）、`src/services/wifi_module.h`（19） |

### 4.1 当前状态

| 检查项 | 现状 |
|---|---|
| 已有日志 | ❌ 无 LogManager 接入，无 `log_` 引用；只有 `Serial.println` |
| 错误处理 | ⚠️ 弱：超时/断线只有状态迁移 + 串口打印，**无错误计数、无失败原因记录** |
| 状态机 | ✅ 完整 4 态：`WIFI_IDLE → CONNECTING → CONNECTED → DISCONNECTED → (重连) CONNECTING` |
| 关键状态迁移点 | `wifi_start_connect()`（:157，含 init 阶段 :151 提前一次）、CONNECTING→CONNECTED（:206）、CONNECTING→超时（:235）、CONNECTED→LOST（:262）、DISCONNECTED→重连（:294） |

### 4.2 建议接入

| 位置 | 事件 | EventId | Level | Params | 原因 |
|---|---|---|---|---|---|
| `wifi_start_connect()` :157 | 开始连接 | `LOG_WIFI_CONNECT_START` | INFO | `LOG_P_SSID_HASH`, `LOG_P_ATTEMPT_N` | ⚠️ 每次重连都会产生 ⇒ **需限流**，见 §7 |
| `wifi_task()` :206 WL_CONNECTED 边沿 | 已连接 | `LOG_WIFI_CONNECTED` | INFO | `LOG_P_CONNECT_MS`, `LOG_P_RSSI`, `LOG_P_SSID_HASH` | 必须在 `if(!wifi_connected)` 边沿内发 |
| `wifi_task()` :235 超时 | 连接超时 | `LOG_WIFI_CONNECT_TIMEOUT` | WARN | `LOG_P_TIMEOUT_MS`, `LOG_P_ATTEMPT_N` | — |
| `wifi_task()` :262 断线 | WiFi 丢失 | `LOG_WIFI_LOST` | WARN | `LOG_P_CONNECTED_MS`, `LOG_P_RSSI`, `LOG_P_CAUSE` | 现场断网首因判据 |
| `wifi_task()` :294 重连尝试 | 尝试重连 | `LOG_WIFI_RECONNECT_TRY` | WARN | `LOG_P_RETRY_N`, `LOG_P_ATTEMPT_N` | ⚠️ 默认 10s 一次 = 6 条/分钟 ⇒ **必须限流** |
| `wifi_update_signal()` :75 RSSI 过低 | 信号弱 | **建议新增 `LOG_WIFI_RSSI_LOW`** | WARN | `LOG_P_RSSI` | 预测性诊断（断线前兆） |
| — | Provisioning | `LOG_WIFI_PROVISION_ENTER/DONE` (0x0606/0x0607) | INFO | — | ⚠️ **当前代码无任何 provisioning 实现**，两个 EventId 暂无触发点，P2 可不实现 |

> ⚠️ 状态迁移必须写 `LOG_P_STATE`(0x44) + `LOG_P_WAS`(0x47) 成对，便于云端还原迁移图。

---

## 5. Cloud / MQTT

| 项 | 内容 |
|---|---|
| 源码文件 | `src/cloud/cloud_manager.cpp`（1656）、`src/cloud/cloud_manager.h`（125）、`src/test/test_mqtt.cpp`（319，**测试代码，待删**） |

### 5.1 当前状态

| 检查项 | 现状 |
|---|---|
| 已有日志 | ⚠️ 有 7 处 `LOG_*` 引用，但**全部是 P1.5 的 `log_ack` 协议常量与常量包含**（`LOG_ACK_COMMAND` / `LOG_ACK_P_*`），**不是业务埋点**。LogManager 自身的 6 个事件（`LOG_LOG_UPLOAD_FAIL`…`LOG_LOG_SELF_DEGRADED`）由 LogManager 内部产生，Cloud 侧无需再记 |
| 错误处理 | ✅ 完整：重连计数、sleep 退避、发布失败检测、分片失败、命令去重、协议错误 |
| 状态机 | ✅ `mqtt_connected` / `mqtt_sleep_mode` / `mqtt_retry_count`；`cloud_task()` 每 loop 推进 |
| 关键状态迁移点 | MQTT_EVENT_CONNECTED(:233) / DISCONNECTED(:258) / PUBLISHED(:266) / ERROR(:314)；`cloud_task()` WiFi 断开(:1435)、进入 sleep(:1490)、退出 sleep(:1470) |

### 5.2 建议接入

| 位置 | 事件 | EventId | Level | Params | 原因 |
|---|---|---|---|---|---|
| `mqtt_event_handler()` :233 `MQTT_EVENT_CONNECTED` | MQTT 已连接 | `LOG_MQTT_CONNECTED` | INFO | `LOG_P_OUTBOX`, `LOG_P_SERVER`⚠️ | 需在 `mqtt_connected` 由 false→true 的边沿发 |
| `mqtt_event_handler()` :258 DISCONNECTED | MQTT 断开 | `LOG_MQTT_DISCONNECTED` | WARN | `LOG_P_ERR_CODE`, `LOG_P_OUTBOX` | ⚠️ 该事件可能连续触发，**必须边沿去重** |
| `mqtt_event_handler()` :314 ERROR（CONNACK 非 0 / TLS） | 认证或连接被拒 | **建议新增 `LOG_MQTT_AUTH_FAILED`** | ERROR | `LOG_P_ERR_CODE` | 凭据错误的唯一可观测点；当前只有串口 |
| `cloud_task()` :1490 进入 sleep | 进入休眠退避 | `LOG_MQTT_SLEEP_ENTER` | ERROR（IMM） | `LOG_P_RETRY_N`, `LOG_P_SLEEP_MS` | 连续重连失败的终态 |
| `cloud_mqtt_publish_binary()` :363 发布失败 | 发布失败 | `LOG_MQTT_PUBLISH_FAIL` | WARN | `LOG_P_QUEUE_SIZE`, `LOG_P_ERR_CODE` | ⚠️ **必须限流**（离线时每 loop 可能失败多次） |
| `cloud_mqtt_publish_text()` :416 同上 | 发布失败 | `LOG_MQTT_PUBLISH_FAIL` | WARN | 同上 | 同上 |
| `cloud_publish_fragmented()` :440 分片失败 | 分片失败 | `LOG_CLOUD_FRAG_FAIL` | WARN | `LOG_P_COUNT`, `LOG_P_ERR_CODE` | — |
| 下行消息解析/协议错误 | 下行非法 | **建议新增 `LOG_CLOUD_RX_INVALID`** | WARN | `LOG_P_CMD`⚠️, `LOG_P_ERR_CODE` | 云端协议演进期的高价值信号 |
| 命令执行失败 | 命令执行失败 | `LOG_MQTT_CMD_EXEC_FAILED` | WARN | `LOG_P_CMD_ID`⚠️, `LOG_P_ERR_CODE` | — |

> `LOG_P_SERVER` / `LOG_P_CMD` / `LOG_P_CMD_ID` 均为字符串语义 ⇒ 见审查报告 §4.2。

---

## 6. Time / RTC

| 项 | 内容 |
|---|---|
| 源码文件 | `src/services/time_manager.cpp`（879）、`src/services/time_manager.h`（195） |

### 6.1 当前状态

| 检查项 | 现状 |
|---|---|
| 已有日志 | ❌ 无 LogManager 接入，无 `log_` 引用；约 20 处 `Serial.printf` |
| 错误处理 | ✅ 完整：SNTP 失败、RTC 探测失败、VL 标志、BCD 越界、写 NACK、年范围拒绝、校准跳过 |
| 状态机 | ✅ 时间有效性状态机（`STATE_TIME_VALID`）+ SNTP 状态机 + RTC 校准状态机 |
| 关键状态迁移点 | `time_update_valid_event()` :393（valid↔invalid）、SNTP 同步回调 :171、RTC 探测 :250、RTC 校准 :426、boot restore :44 |

### 6.2 建议接入

| 位置 | 事件 | EventId | Level | Params | 原因 |
|---|---|---|---|---|---|
| `time_on_sntp_sync()` :171 成功 | NTP 同步成功 | `LOG_TIME_NTP_OK` | INFO | `LOG_P_SERVER`⚠️, `LOG_P_UNIX`, `LOG_P_DRIFT_MS` | 时间可信的起点；**日志 timestamp 依赖它** |
| SNTP 失败/超时 | NTP 失败 | `LOG_TIME_NTP_FAIL` | WARN | `LOG_P_SERVER`⚠️, `LOG_P_ERR_CODE`, `LOG_P_ATTEMPT_N` | — |
| `time_update_valid_event()` :393 → valid | 进入时间有效 | `LOG_TIME_VALID_ENTER` | INFO | `LOG_P_UNIX`, `LOG_P_SOURCE` | ⚠️ P1 已撤销 Flash 例外 ⇒ **Flash NO**，仅上云 |
| `time_update_valid_event()` :393 → invalid | 进入时间无效 | `LOG_TIME_INVALID_ENTER` | WARN | `LOG_P_CAUSE` | — |
| `time_init()` :250 RTC 探测 | RTC 探测结果 | `LOG_TIME_RTC_PROBE` | INFO（成功）/ WARN（失败） | `LOG_P_ADDR`, `LOG_P_SDA`→用 `LOG_P_ERR_CODE` | ⚠️ **上板实测 `rtc_present=false`**，此事件是定位依据 |
| `time_init()` :44 boot restore 成功 | RTC 开机恢复 | `LOG_TIME_RTC_BOOT_RESTORE` | INFO | `LOG_P_UNIX`, `LOG_P_DRIFT_MS` | — |
| `time_init()` :301 VL 标志置位 | RTC 掉电 | `LOG_TIME_RTC_VL_FLAG` | WARN | — | 电池没电 |
| `time_init()` :321 BCD 越界 | RTC 数据非法 | `LOG_TIME_RTC_BCD_INVALID` | WARN | `LOG_P_RAW` | — |
| `time_rtc_calibrate()` :468 写成功 | RTC 已校准 | `LOG_TIME_RTC_CALIBRATED` | INFO | `LOG_P_DRIFT_MS` | — |
| `time_rtc_calibrate()` :470 写失败 | RTC 写失败 | `LOG_TIME_RTC_WRITE_FAILED` | WARN | `LOG_P_ERR_CODE`, `LOG_P_DRIFT_MS` | ⚠️ 该路径已接入 Critical Op，日志不得改变 release 语义 |

---

## 7. Workflow

| 项 | 内容 |
|---|---|
| 源码文件 | `src/automation/workflow.cpp`（4465）、`src/automation/workflow.h`（827）、`src/automation/workflow_storage.cpp`（1583） |

> ⚠️ **作用域更正（2026-09-20 / WF-4 审查结论）**：
> 本节的 Registry 相关条目（§7.2 末行）**已移出 Workflow 作用域**，归入 §12 的
> `capability_registry` 独立模块。原因：`workflow_storage.cpp` 与 Capability Registry
> **零耦合** —— 其 7 个 `#include` 不含 `capability_registry.h`，全仓库对 Registry API 的
> 引用仅出现在 `cloud_manager.cpp` / `command_manager.cpp` / `main.cpp`，
> `workflow_storage.cpp/.h` 中唯一提及 Registry 之处是 `workflow_storage.h:268`
> 的一条**注释**（描述下游后果，非调用关系）。两者是**平行关系**：
> Workflow 本体 → `/workflow/*.bin`；Registry → `/registry/*.bin`。
> 报告：`docs/archive/log/WF4-Registry范围审查0920.md`

### 7.1 当前状态

| 检查项 | 现状 |
|---|---|
| 已有日志 | ❌ **0 处** `log_`/`LOG_` 引用。全部为 `Serial.printf` |
| 错误处理 | ✅ 完整但**静默**：`workflow_terminate()` 六个终止路径零日志；save 事务 partial 只打串口 |
| 状态机 | ✅ 核心：`WORKFLOW_IDLE/RUNNING/WAITING/FINISHED/TIMEOUT/ERROR` + Step Trigger/Action 子状态机 |
| 关键状态迁移点 | `workflow_start()` :3497（含 Critical Op acquire 失败 :3539）、超时 :3981、Trigger 失败 :4050、Action 失败 :4098、临时 Action 超时 :4120、`workflow_terminate()` :3644、`workflow_save_transaction()` :1730 |
| **风险** | ⚠️ `workflow_terminate()` 是 Critical Op release 的收口点，日志接入**不得改变任何 return 路径**（记忆铁律：release 不能放在会中途 return 的函数里） |

### 7.2 建议接入

| 位置 | 事件 | EventId | Level | Params | 原因 |
|---|---|---|---|---|---|
| `workflow_start()` :3497 成功 | Workflow 启动 | `LOG_WF_START` | INFO | `LOG_P_SLOT`, `LOG_P_WF_ID`⚠️, `LOG_P_STEPS_DONE` | **以 `p.id`(slot) 为主键** |
| `workflow_start()` :3539 acquire 被拒 | 启动被拒 | **建议新增 `LOG_WF_STOPPED`** 或复用 `LOG_WF_FAILED` | WARN | `LOG_P_SLOT`, `LOG_P_CAUSE` | 安全窗口内拒绝启动，需可审计 |
| 正常完成 | 完成 | `LOG_WF_FINISHED` | INFO | `LOG_P_SLOT`, `LOG_P_DURATION_MS`, `LOG_P_STEPS_DONE` | — |
| `workflow_task()` :3981 超时 | 超时 | `LOG_WF_TIMEOUT` | WARN | `LOG_P_SLOT`, `LOG_P_TIMEOUT_MS`, `LOG_P_STUCK_STEP` | — |
| Trigger 失败 :4050 | Workflow 失败 | `LOG_WF_FAILED` | WARN | `LOG_P_SLOT`, `LOG_P_FAIL_STEP`, `LOG_P_CAUSE` | — |
| Action 失败 :4098 | Action 失败 | `LOG_WF_ACTION_FAILED` | WARN | `LOG_P_SLOT`, `LOG_P_FAIL_STEP`, `LOG_P_ACTION_ID`⚠️ | — |
| 临时 Action 超时 :4120 | 临时动作超时 | `LOG_WF_TEMP_ACTION_TIMEOUT` | WARN | `LOG_P_TIMEOUT_MS`, `LOG_P_ACTION_ID`⚠️ | — |
| `workflow_save_transaction()` :1745 def 分配失败 | 运行时分配失败 | `LOG_WF_RUNTIME_ALLOC_FAILED` | ERROR | `LOG_P_NEED_BYTES` | ⚠️ 堆/PSRAM 不足 |
| `workflow_save_transaction()` :1848 `all_ok=false` | 保存失败 | `LOG_WF_SAVE_FAILED` | WARN/ERROR | `LOG_P_SAVED`, `LOG_P_TOTAL`, `LOG_P_ERR_CODE` | — |
| 部分成功 | 部分保存 | `LOG_WF_SAVE_PARTIAL` | WARN | `LOG_P_SAVED`, `LOG_P_TOTAL` | — |
| 部分保存重试成功 | 重试成功 | `LOG_WF_SAVE_PARTIAL_RETRY_OK` | INFO | `LOG_P_RETRY_N` | ID 保留，P2 决定是否实现 |
| `workflow_create/update/delete` | CRUD | `LOG_WF_CRUD` | INFO | `LOG_P_SLOT`, `LOG_P_OP`, `LOG_P_VARIANT` | — |
| `workflow_migrate_to_storage()` :1968 | JSON→BIN 迁移 | `LOG_WF_MIGRATED` | INFO | `LOG_P_COUNT`, `LOG_P_STAGED_COUNT` | — |

> Registry 埋点**不属于本节**（见 §12）。`workflow_storage.cpp` 不产生 Registry 类日志。

---

## 8. Water / Dispense Guard

| 项 | 内容 |
|---|---|
| 源码文件 | `src/app/dispense_guard.cpp`（原 73 → 现 138）、`src/app/dispense_guard.h`（3） |
| 状态 | ✅ **已接入（Phase 4，2026-09-20，`94912bb`）** |

### 8.1 当前状态（接入后）

| 检查项 | 现状 |
|---|---|
| 已有日志 | ✅ **1 处 `log_emit`**（`LOG_DISPENSE_SAFETY_RESPONSE` 0x0511 WARN） |
| 错误处理 | ⚠️ 弱（原生行为，本次未改）：`valve_force_close()` 返回 false 时只打印，**无状态记录、无重试** ⇒ 其可观测性由 `valve.cpp:405` 的 `LOG_VALVE_FORCE_CLOSE_FAILED` 承担 |
| 状态机 | 无（纯事件回调）—— **本轮确认：`dispense_guard` 就是"Dispense 模块"本体，其唯一职责 = 安全响应**，不再规划出粮控制/电机/状态机 |
| 关键状态迁移点 | `EVENT_WEIGHT_ERROR` 到达 → **【决策埋点 0x0511】** → `valve_force_close()` → 成功/失败（由 Valve 侧记 0x0505/0x0506） |

### 8.2 实际接入（★ 结论：本模块**只记"决策"，不记"输入"也不记"输出"**）

| 位置 | 语义 | EventId | Level | Params | 是否重复 |
|---|---|---|---|---|---|
| `dispense_guard_event_callback()` 过滤后、`valve_force_close()` 前 | **安全响应决策**（"我响应了，并决定发起安全动作"） | **`LOG_DISPENSE_SAFETY_RESPONSE` 0x0511** | **WARN** | `LOG_P_CAUSE`(=1) —— 合并器再追加 `LOG_P_COUNT` | ✅ **不重复**（见下） |
| ~~收到 weight error~~ | ~~出水中断~~ | ~~`LOG_DISPENSE_FAILED` 0x0503~~ | — | — | ❌ **重复**：`weight.cpp:236` 已记 `LOG_WEIGHT_ERROR_ENTER` 0x050C（边沿 + 突发合并），且逐条转记会形成 **N:1 放大** |
| ~~`valve_force_close()` 调用后~~ | ~~关阀动作~~ | ~~`LOG_VALVE_FORCE_CLOSE` 0x0505~~ | — | — | ❌ **重复**：`valve.cpp:470` 已记（CRITICAL + 5 s 门控），且 `LOG_P_CAUSE=1` **已带出"来源 = dispense_guard"** |
| `dispense_guard_init()` 订阅失败 | 保护未生效 | ❌ **仍缺 ID** | — | — | 📌 登记为 **`LV-1`**（需新增 `0x0512` 之类；用户裁决本轮不新增 ⇒ 保持零改动） |
| ~~出水开始/完成/超时~~ | — | ~~0x0501-0x0504~~ | — | — | ⚠️ **本轮判定：四个 ID 语义均不对位**（描述"一次供水过程"的生命周期；安全响应是"终止供水"，复用 `START` 会伪造事实）⇒ **不占用，留给未来真正的 Dispense 业务流程** |

### 8.3 ★ 为何 0x0511 不是 0x0505 的重复（决定性论据）

| 维度 | Valve 侧 `0x0505` | Dispense 侧 `0x0511` |
|---|---|---|
| 记录的事实 | **执行器动作**（GPIO 写入尝试；`pin<0` 时走 0x0506） | **决策**（响应了哪个事件并决定动作） |
| 次数语义 | ⚠️ **5 s 日志门控**（`VALVE_FORCE_CLOSE_LOG_COOLDOWN_MS`）⇒ **只记"发生过"，丢失次数** | ✅ **突发合并**（5 s 窗口）⇒ **Σ `LOG_P_COUNT` = 真实响应次数** |
| Level | CRITICAL（触发 immediately-flush） | WARN（仅路由 Flash+Cloud，**不抢通道**） |
| 依赖执行结果 | 是（与 `result` 耦合） | 否（纯决策，与执行解耦） |

⇒ 二者**互补**：`0x0505` 回答"阀门关了没有"，`0x0511` 回答"响应了几次"。

### 8.4 限流（**只限制日志，不限制安全动作**）

`LOG_DISPENSE_SAFETY_RESPONSE` 已注册进 LogManager 的 `s_coalesce_targets[]`
（`LOG_COALESCE_WINDOW_MS = 5000`）。合并发生在 `log_emit()` 内、**回调的安全逻辑早已完成**，
且埋点块无 `return` / 无分支 / 不读不改 `result` ⇒ **短时间多个重量异常时安全动作次数不减少**。

| 项 | 值 |
|---|---|
| 实测最坏频率 | **6~20 次/s**（`valve.cpp:454`，R-3） |
| 不合并 5 s 内 | 30 ~ 100 条 WARN（会同时压满 Flash 段环 496 与云队列 128） |
| 合并后 5 s 内 | **2 条**（1 立即 + 1 汇总），ΣCOUNT = 30~100，**信息零丢失** |

### 8.5 已知缺口

- ⚠️ `dispense_guard` 无节流（**VALVE-5**）：日志侧已由合并白名单兜住；**动作侧仍无节流**（本轮不改安全逻辑）
- 📌 `LV-1`：订阅失败不可观测（需新增 EventId，未批准）
- 📌 **观测工具缺口**：`logt fver` 只打印 `params=<个数>`，**不打印参数值** ⇒ ΣCOUNT 的精确值只能在**云端批次解码**侧验证（用 `.pio/p15run/log_decode.py` + `p2i_report.py` 模式，仅改 2 处字面量）。已在 `test/p4_dispense_log_tests.txt` 中写明步骤。

---

## 9. Valve

> **⚠️ 本节已由 P2-H（2026-09-19）按真实代码模型重写**（依据 P2-H 决策 **D3=A**）。
> 原矩阵假定阀门有 6 个状态（OPEN / CLOSE / OPENING / CLOSING / ERROR / FORCE_CLOSE），
> **实测代码中 OPENING / CLOSING / ERROR / FORCE_CLOSE 四个状态并不存在**。
> 审查报告：`docs/archive/log/LogManager-P2H-Valve接入审查0919.md`。

| 项 | 内容 |
|---|---|
| 源码文件 | `src/app/valve.cpp`（475）、`src/app/valve.h`（42）、`src/app/dispense_guard.cpp`（73） |
| 本质 | 阀门是**二值设备**，且切换是**瞬时**的（`gpio_set_level()` 后即完成）⇒ **不存在 OPENING / CLOSING 过渡态** |

### 9.1 真实模型（取代原"6 状态"假设）

**状态（只有 1 个变量，二值）**

| 状态 | 载体 | 说明 |
|---|---|---|
| `OPEN` | `static bool current_state == true`（`valve.cpp:31`） | `STATE_VALVE_STATUS`（`system_state.h:31`）是其 SystemState 镜像 |
| `CLOSE` | `static bool current_state == false` | 初始值即 CLOSE |

辅助量（**不是状态**）：`open_start_time`（0 = 未开启）、`last_operation_time`（防风暴）、`initialized`。

**明确不存在**：`OPENING` / `CLOSING`（无过渡态）、`ERROR`（只有 `EVENT_VALVE_ERROR` **事件**，无状态变量）、
`FORCE_CLOSE`（`valve_force_close()` 把 `current_state` 置 false，**与普通关闭在状态上不可区分**）。

**事件（实际埋点，共 6 类）**

| 事件 | 宿主（行号） | EventId | Level | 真实触发条件 |
|---|---|---|---|---|
| 阀门开启 | `valve_set_gpio()` open 分支 | `LOG_VALVE_OPEN` 0x0509 | INFO | 真实切换（`open != current_state` 判等已通过）⇒ **天然边沿去重** |
| 阀门关闭 | `valve_set_gpio()` close 分支 | `LOG_VALVE_CLOSE` 0x050A | INFO | 同上；带 `VALVE_OPEN_MS` |
| 强制关阀 | `valve_force_close()` | `LOG_VALVE_FORCE_CLOSE` 0x0505 | CRITICAL | **纯事件**：每次调用都执行（无状态判等）⇒ **必须去重门控**，见 9.3 |
| 强制关阀失败 | `valve_force_close()` `valve_pin<0` | `LOG_VALVE_FORCE_CLOSE_FAILED` 0x0506 | CRITICAL | 引脚未配置。⚠️ **无门控**（见 9.4 风险） |
| 安全超时 | `valve_task()` 超时分支 | `LOG_VALVE_SAFETY_TIMEOUT` 0x0508 | WARN | 开启时长 ≥ `safety_timeout_sec`；**一次性报告锁** |
| 防风暴跳过 | `valve_set_gpio()` 50 ms 限流分支 | `LOG_VALVE_RATE_LIMITED` 0x050B | WARN | 50 ms 内发生二次**真实**切换 |

**无宿主（不埋，登记缺口）**：`LOG_VALVE_OVERFLOW_RISK` 0x0507（无检测实现，= 问题清单 `P0-4`）、
`LOG_VALVE_NOT_READY`（**`log_events.h` 中未定义**，禁止新增 EventId）。

### 9.2 原矩阵的 3 处错误（P2-H 已核实推翻）

| 原矩阵说法 | 实际 |
|---|---|
| `valve_force_close()` "永远返回 true（无失败分支）"⇒ `FORCE_CLOSE_FAILED` **无处可埋** | ❌ `valve.cpp:323` 确有 `valve_pin < 0` 分支 ⇒ **可埋，且 P2-H 已埋** |
| CRITICAL（IMM）"**必须立即 flush**" | ❌ 实现是"**置一次 `s_flush_requested` 标志**，由本轮 `log_task()` 阶段 2 落盘"（`log_manager.cpp:1683-1702`）；**不建独立通道、不绕过 RAM 环（64 槽）/云队列（128 槽）、不承诺秒级** ⇒ **标 CRITICAL 并不能防队列冲爆** |
| `LOG_VALVE_RATE_LIMITED` "高频调用源会重复触发，**需边沿/计数去重**" | ❌ 防风暴检查在 `open == current_state` 判等 return **之后** ⇒ 只在"真要切换"时才可能进入；实测 **11 个既有会话 `Operation too frequent` 全部为 0** ⇒ 无需去重 |

### 9.3 `valve_force_close()` 去重门控（P2-H 决策 **D1=A**）

**必要性（实测）**：`valve_force_close()` 无状态判等，`dispense_guard` 把**每个** `EVENT_WEIGHT_ERROR` 都转成一次调用。
实测 B2 会话 **50 次 / 175 s**、峰值 **6 次/s**；理论上限 **20 次/s**（`EVENT_STORM_MAX_PER_EVENT=5` / 500 ms × ×2 成对）。
而 `LOG_VALVE_FORCE_CLOSE` 是 CRITICAL ⇒ 落 Flash（496 条环）+ 进云队列（128 槽）
⇒ **峰值下约 83 s 就能冲光全部历史日志**。

**规则**（`valve.cpp`）：满足任一才 `log_emit`，否则**只执行 force_close、不写日志**：
1. `cause != last_force_close_cause`
2. `(now - last_force_close_log_ms) >= 5000`

**约束**：门控**只限制日志**，`valve_force_close()` 的"无条件强制同步 GPIO"语义**零改动**；
日志判断位置在"GPIO 动作 → 状态更新 → SystemState → `open_start_time` → 事件发布"**全部完成之后**（绝不 `if(!need_log) return;`）。

**`CAUSE` 冻结定义**：`1 = WEIGHT_ERROR`（本轮唯一来源）/ `2 = MANUAL_COMMAND`（预留）/ `3 = SAFETY_TIMEOUT`（预留）。

### 9.4 已知残余风险

- 🔴 **`LOG_VALVE_FORCE_CLOSE_FAILED` 未门控**：实测 `valve_pin = -1` 时 **54 次调用 → 52 条 CRITICAL**
  （约 0.6 条/s；理论上限 20 条/s）。该分支只在引脚未配置（模块不可用）时进入。
  **建议后续按 9.3 同款门控** —— 待拍板，本轮按 spec 未加。
- 🟠 `valve_force_close()` **不检查 `initialized`**（问题清单 **VALVE-1**）：模块被 config 禁用但引脚已配置时
  会写未 `gpio_config()` 的引脚并**返回 `true`（假成功）**。P2-H 决策 **D2=A**：本轮**只登记不修**。
- 🟠 `dispense_guard` 无节流（**VALVE-5**）；`valve_force_close()` 成对调用 ×2 成因未定位（**R-6**）。


---

## 10. Weight / HX711

| 项 | 内容 |
|---|---|
| 源码文件 | `src/app/weight.cpp`（645）、`src/app/weight.h`（41） |

### 10.1 当前状态

| 检查项 | 现状 |
|---|---|
| 已有日志 | ❌ 无，仅 `Serial.printf`（含一段已注释的 5s 调试打印 :379-400） |
| 错误处理 | ✅ 完整：三类异常（HX711 无数据 5s / raw 连续为 0 5s / 5s 内跳变 ≥5 次），**且已有边沿检测**（`if(err != error_state)` :204） |
| 状态机 | ✅ 零点校准状态机（`calibrating` / `calibrate_ok`）+ Trigger 状态机（`weight_active`） |
| 关键状态迁移点 | `weight_refresh_error_state()` :204（error 边沿）、`weight_task()` :281（校准完成）、`weight_trigger_start()` :463/:483（异常/参数非法）、`weight_trigger_poll()` :525（触发成功） |
| **高频源** | ⚠️ HX711 ≈10Hz，每 5 点出一次滤波值（≈0.5s）；`weight_record_jump()` :216 每次跳变都 push `EVENT_WEIGHT_ERROR` — **若在此埋点会造成洪泛** |

### 10.2 建议接入

| 位置 | 事件 | EventId | Level | Params | 原因 |
|---|---|---|---|---|---|
| `weight_refresh_error_state()` :204 `err=true` | 称重异常进入 | `LOG_WEIGHT_ERROR_ENTER` | WARN | `LOG_P_CAUSE`(1=not_ready / 2=raw_zero / 3=jump), `LOG_P_WEIGHT_G`, `LOG_P_RAW` | **唯一允许的称重错误埋点**（天然边沿） |
| `weight_refresh_error_state()` :204 `err=false` | 称重异常解除 | `LOG_WEIGHT_ERROR_EXIT` | INFO | `LOG_P_CAUSE`, `LOG_P_WEIGHT_G` | — |
| `weight_task()` :281 校准完成且保存成功 | 零点校准完成 | `LOG_WEIGHT_ZERO_DONE` | INFO | `LOG_P_OFFSET`, `LOG_P_SAMPLES`, `LOG_P_SAVED` | — |
| `weight_task()` :281 校准完成但保存失败 | 校准失败 | `LOG_WEIGHT_CALIB_FAILED` | ERROR | `LOG_P_OFFSET`, `LOG_P_ERR_CODE` | 校准值写不进配置 = 白校准 |
| `weight_zero_calibrate()` :634 | 校准开始 | **建议新增 `LOG_WEIGHT_CALIB_START`** | INFO | `LOG_P_SAMPLES` | 与 DONE 配对，判断"卡在中途" |
| `weight_trigger_poll()` :525 触发成功 | 重量触发成功 | `LOG_WEIGHT_TRIGGER_FIRED` | INFO | `LOG_P_TARGET_G`, `LOG_P_DELTA_G`, `LOG_P_WEIGHT_G` | ⚠️ 一次出水只触发一次，安全 |
| `weight_trigger_start()` :463 异常中止 | — | `LOG_WEIGHT_ERROR_ENTER` + `LOG_P_CAUSE=4` | WARN | 同上 | 复用已有 ID，不新增 |
| `weight_trigger_start()` :483 参数非法 | — | `LOG_WEIGHT_ERROR_ENTER` + `LOG_P_CAUSE=5` | WARN | `LOG_P_TARGET_G` | 同上 |

> **禁止埋点位置**：`weight_record_jump()` :216（每次跳变都调用，5s 内最多 5 次）与 `weight_task()` 的 not-ready 分支 :323（已有 5s 节流但仍是重复源）。二者都应通过 `error_state` 边沿间接体现。

---

## 11. BLE / MiThermometer

| 项 | 内容 |
|---|---|
| 源码文件 | `src/app/MiThermometer.cpp`（655 → **700**）、`src/app/MiThermometer.h`（26） |

### 11.1 当前状态

| 检查项 | 现状 |
|---|---|
| 已有日志 | ✅ **已于 Phase 5-C 接入 4 个事件（2026-09-21，`70b0c76`）** —— 见 **§11.3**；★ **`onResult()` 内仍为零埋点**（callback 禁埋点，静态证明）。<br>此前已知缺陷 **`P0-3`**：`MiAdvCallback::onResult()` :117-123 **逐字节 `Serial.printf` 打印整个 ADV payload**（每包数十次串口调用）⇒ ✅ **已在 Phase 2 完成**（`edf2566`，编译期开关 `MI_THERMO_DEBUG_VERBOSE` 默认 0，`onResult()` 中 `Print::printf`/`println` 计数 = 0） |
| 错误处理 | ✅ 三级降级：LEVEL0 → LEVEL1 → LEVEL2（禁用扫描，`MI_THERMO_MAX_FAIL=4`）；解码失败、bindkey/MAC 缺失、队列创建失败均有处理 |
| 状态机 | ✅ 7 态：`BOOT_DELAY / SLEEP / SCAN_WINDOW_RUN / SCAN_WINDOW_WAIT / SCAN_ON / SCAN_OFF / DISABLED` |
| 关键状态迁移点 | 扫描窗口结束判定 :316-343（成功/fail_count++/LEVEL 迁移/禁扫）、解码队列消费 :419-457、`mi_thermo_start_scan()` :531 / `stop_scan()` :564 |
| **高频源** | 🔴 **最高风险 —— 但须区分两个速率**（Phase 5 审查修正）：<br>① **`onResult()` 触发率**：所有在空中的广播设备 ⇒ 密集环境可达**数十/秒** ⇒ **无任何可记录事实**（MAC 不匹配在 `:124-126` 就 `return`）<br>② **通过 MAC + 长度过滤、真正 `xQueueSend` 入队的帧**：**≈ 1 包 / 3 s**（P2-BLE 实测：广播周期 1–2 s × 扫描占空"1 s 开 / 1.8–2.3 s 关"）⇒ **这才是"可记录事实"的速率**<br>③ **窗口级事件**：15 / 30 min<br>⚠️ 队列 16 槽，`xQueueSend` 返回值**被忽略**（`:165`，静默丢包） |

### 11.2 建议接入（**P2 阶段的设计建议；Phase 5-C 实际落地见 §11.3，两者以 §11.3 为准**）

| 位置 | 事件 | EventId | Level | Params | 原因 |
|---|---|---|---|---|---|
| `MiThermometer_task()` :419 解码成功 | 数据解码成功 | `LOG_BLE_DATA_DECODED` | INFO | `LOG_P_TEMP`, `LOG_P_HUMID`, `LOG_P_BATT_V`, `LOG_P_MAC_SUFFIX` | ⚠️ **必须每次扫描窗口最多 1 条**（收到 temp+humid 后置位 `s_got_*`，需加"本窗口已上报"标志） |
| `MiThermometer_task()` :424 解码失败 | 解码失败 | `LOG_BLE_DECODE_FAIL` | WARN | `LOG_P_FAIL_COUNT`, `LOG_P_FAIL_KIND` | 🔴 契约已标注"限流"（:318）⇒ **建议 ≤1 条 / 扫描窗口，其余只累加计数** |
| 扫描窗口失败 / LEVEL 迁移 | 传感器失联 | `LOG_BLE_SENSOR_LOST` | WARN | `LOG_P_FAIL_COUNT`, `LOG_P_BLE_LEVEL` | 窗口级事件（15/30 分钟一次），安全 |
| :337 进入 LEVEL2 禁扫 | 扫描已禁用 | `LOG_BLE_SCAN_DISABLED` | INFO | `LOG_P_BLE_LEVEL`, `LOG_P_FAIL_COUNT` | — |
| `MiThermometerInit()` :210 MAC/bindkey 缺失 | 初始化失败 | **建议新增 `LOG_BLE_INIT_FAILED`** | ERROR | `LOG_P_CAUSE` | 配置错误首发信号 |
| `MiThermometerInit()` :241 队列创建失败 | 初始化失败 | **建议新增 `LOG_BLE_INIT_FAILED`** | ERROR | `LOG_P_NEED_BYTES`, `LOG_P_FREE_BYTES` | — |
| `onResult()` :127 `xQueueSend` 失败 | 广播队列满 | **建议新增 `LOG_BLE_QUEUE_FULL`** | WARN | `LOG_P_QUEUE_SIZE`, `LOG_P_DROP_RING` | 🔴 当前静默丢包；**但不能在回调里 `log_emit`**，需在 `MiThermometer_task()` 侧用计数+标志上报 |

> 🔴 **硬禁令**：**不得在 `MiAdvCallback::onResult()` 中调用 `log_emit()`**。理由：① 运行在 NimBLE host 任务上下文，非 loop；② 该回调已被逐字节串口打印拖慢；③ 每包一条会瞬间填满 64 槽 RAM 环。正确做法：回调内只置计数/标志，日志在 `MiThermometer_task()` 中按窗口聚合上报。

> ### ✅ **Phase 5 审查补充（2026-09-20）：这条禁令在本设计中根本不会被触发**
>
> **代码事实**：`onResult()`（`:114-167`）的全部动作 = MAC 逐字节过滤 → payload 空检查 → 长度过滤（`<29`）→ 构造 `RawAdvItem`（memcpy）→ [`MI_THERMO_DEBUG_VERBOSE` 开关的 hex dump，默认关闭] → `xQueueSend`。
> ⇒ **callback 里没有"决策"，只有"搬运"** ⇒ 按铁律 25「总线只搬运，不代记」⇒ **callback 埋点数 = 0**。
>
> **上表 4 个已冻结 ID 的天然宿主全部在 `MiThermometer_task()`（loop 上下文）**：
> | EventId | 宿主位置 | 速率 | 需聚合？ |
> |---|---|---|---|
> | `DATA_DECODED` 0x0901 | `:468` `if(result){}` | **≤1 / 扫描窗口**（收到 temp+humid ⇒ `SCAN_WINDOW_WAIT` **停扫**，`:490-499`）⇒ 代码天然保证；埋点处需加"本窗口已上报"标志 | ❌ |
> | **`DECODE_FAIL` 0x0902** | `:468` **`else`** 分支 | **可达 1/3 s = 20/min**（**bindkey 配置错误** ⇒ CCM 对每包都失败） | ✅ **必须** |
> | `SENSOR_LOST` 0x0903 | `:362` `s_scan_fail_count++` 分支 | ≤1 / 15–30 min | ❌ |
> | `SCAN_DISABLED` 0x0904 | `:373-382` 进入 `LEVEL2`（`state_set_bool(ENABLE,false)` 前） | 1 次 / 生命周期（迁移后 `s_scan_state = DISABLED`） | ❌ |
>
> **★ Flash 风险定量**：`DECODE_FAIL` 是 **WARN ⇒ 落 Flash**；20 条/min ⇒ **496 条段环 ≈ 25 min 冲满**（对照 `R-3` 的 CRITICAL 83 s 冲光，量级低 3 个数量级，但**在"长期错误配置"下会持续消耗**）⇒ **必须聚合**。
> **聚合方式建议**：**模块内窗口聚合**（复用既有 `s_scan_fail_count`，窗口结束上报 1 条带 `LOG_P_FAIL_COUNT`）⇒ **零 LogManager 改动**；若改走 LogManager 突发合并白名单（5 s），须满足 **铁律 30**：被合并埋点**必须至少带 1 个参数**（`log_coalesce_emit_summary()` 在 `base_n == 0` 时静默丢弃折叠计数）。
>
> **技术补充（为什么"技术上可以但工程上不应该"）**：`log_emit_internal()` **无 I/O / 无 malloc / 无 delay**，临界区仅 128 B memcpy（µs 级），栈开销 ≈200 B vs **NimBLE host task 栈 4096 B**（`nimconfig.h:217`）⇒ 可行；但 NimBLE host task 优先级**高于 loopTask** ⇒ 在 `s_mux` 上引入**优先级反转**（唯一被拖慢的是 loop 的 `log_task()`，µs 级、可忽略，**但方向是错的**）。
>
> 📄 详见 `docs/archive/log/Phase5-BLE-OLED-Dispense边界与LogManager完整性审查0920.md` §1
>
> **尚未覆盖的 BLE 自身异常（候选，需新增 ID —— 未批准）**：
> | 候选 | 位置 | 为何是真实缺口 |
> |---|---|---|
> | `BLE_INIT_FAILED`（ERROR） | `MiThermometerInit()` `:247-251`（MAC/bindkey 空）、`:277-281`（`xQueueCreate` 失败） | `:247` 分支 **`return false` 但调用方 `main.cpp:508` 忽略返回值** ⇒ 模块永久不可用却**静默**（与 `LV-1` 同构） |
> | `BLE_QUEUE_FULL`（WARN） | `onResult()` `:165` `xQueueSend(...,0)` 返回值被忽略，队列 16 槽 | 静默丢帧；**不能在 callback 记录** ⇒ 需回调置计数、task 侧消费上报（同 `EVT_QUEUE_FULL` 做法） |

### 11.3 实际接入（Phase 5-C，2026-09-21 · `70b0c76`）

> 改动：**`src/app/MiThermometer.cpp` `+74/−0`（仅此 1 个文件）**。**零新增协议**（无新 EventId / ParamId / 配置项 / 开关）；`log_events.h` / `log_manager.*` / `event_manager.*` / `cloud_manager.cpp` / `system_state.*` / `workflow.*` / `main.cpp` / `test/` **全部零改动**。

| EventId | Level | 参数（实际） | 实际宿主 | 与 §11.2 建议的差异 |
|---|---|---|---|---|
| `LOG_BLE_DATA_DECODED` 0x0901 | INFO | 按 `data_type` 选 `LOG_P_TEMP`(0x3B) / `LOG_P_HUMID`(0x3C) / `LOG_P_BATT_V`(0x3D)，值 `log_arg_f32` | `MiThermometer_task()` → `if(result)` 内、`switch(data_type)` **之后**（三 case 汇合） | ★ **未用 `LOG_P_MAC_SUFFIX`**（MAC 由配置侧已知，无诊断增量）；★ **无需额外"本窗口已上报"标志** —— 收到温+湿即停扫（`SCAN_WINDOW_WAIT`），代码天然保证 ≤1 条/窗口；★ **未埋 `state_set_*` 处**（同一定义点的第二份副本） |
| `LOG_BLE_DECODE_FAIL` 0x0902 | **WARN** | `LOG_P_FAIL_COUNT`(0x3F) + `LOG_P_WINDOW_MS`(0x4A) | `mi_decode_fail_tick()`（内联进 task）的**窗口到期**分支 | ★ **改走"模块内 60 s 窗口聚合"而非 LogManager 突发合并**（§11.2 的 `LOG_P_FAIL_KIND` **未使用** —— 那需要把 `lywsd03_decrypt()` 的 `bool` 改成带原因返回，超范围）；★ **`else` 分支为本轮新增**（原代码 `if(result){}` **无 else**） |
| `LOG_BLE_SENSOR_LOST` 0x0903 | WARN | `LOG_P_FAIL_COUNT` + `LOG_P_BLE_LEVEL`(0x51) | task `SCAN_WINDOW_RUN` → `s_scan_fail_count++;` **之后** | 按建议接入（天然边沿：≤1 条 / 15–30 min） |
| `LOG_BLE_SCAN_DISABLED` 0x0904 | INFO | `LOG_P_FAIL_COUNT` + `LOG_P_BLE_LEVEL` | `LEVEL2` 分支 → **`state_set_bool(STATE_MI_THERMO_ENABLE,false)` 之前** | 按建议接入（1 次/生命周期） |

**聚合实现（`mi_decode_fail_tick()`，节拍器 = `MiThermometer_task()` 首行）**

```
count == 0         ⇒ 清零窗口起点并 return（**静默且不推进窗口** ⇒ 失败停止后不再产出）
window_start == 0  ⇒ 锚定 millis()（首个失败）
now - start < 60s  ⇒ return
到期               ⇒ log_emit(0x0902, WARN, {FAIL_COUNT, WINDOW_MS=60000}) → **count 与窗口同时清零**
```

⇒ **每 60 s 最多 1 条**；`millis()` 用**无符号差值** ⇒ 回绕安全；节拍器位于 task **第一句**（早于 `xRawAdvQueue == nullptr` 守卫）⇒ **不被早退吞掉**（objdump 已证：函数首条逻辑即 `count==0` 判断）。

**Flash 定量**

| 场景 | 逐帧记录（**未采纳**） | 窗口聚合（**实际**） |
|---|---|---|
| bindkey 配置错误（CCM 对每包都失败） | 20 条/min ⇒ **496 条段环 ≈ 25 min 冲满** | **1 条/min ⇒ 环撑 ≈ 8.3 h（降幅 20×）** |

**验证**

| 项 | 结果 |
|---|---|
| 编译 | ✅ **SUCCESS**；**RAM 130624 → 130632（+8 B = 2 个 `static uint32_t`）** / **Flash 1370873 → 1371129（+256 B）** |
| 栈 | `MiThermometer_task` `entry a1, 0x100` → **`0x110`（+16 B）**，远小于 loopTask 16 KB |
| **callback 零埋点** | ✅ objdump：`MiAdvCallback::onResult` 中 `log_emit` 站点 **= 0**；`MiThermometer_task` 内**恰 4 个** |
| EventId 编码 | ✅ `0x9xx` > 2047 超 `movi` 12 位 ⇒ **经字面量池加载**；逐值解析 `.flash.text`：`0x42000098=0x0902` / `0xc8=0x0903` / `0xd0=0x0904` / `0xe8=0x0901`，与 4 个站点**一一对应** |
| Level / 参数个数 | ✅ `a11` = **2/2/1/1**（WARN·WARN·INFO·INFO）；`param_count` = **2/2/2/1** |
| ParamId 编码 | ✅ `0x23f`(0x3F+U32)×3 · `0x551`(0x51+**ENUM**)×2 · `0x24a`(0x4A+U32)×1 · `s8i` **0x3B/0x3C/0x3D 各 1**（+type 3 = F32）；**params 全在栈**（SP+40…+52） |
| 资源约束 | ✅ 无新增静态大数组 · 无 `malloc` · 无 `String` · 新增行中 `Serial.` 命中 **0** · `lywsd03_decrypt()` **零改动** |
| **上板验证** | ⏳ **未执行**（无串口设备）。`DATA_DECODED` / `SCAN_DISABLED` 为 INFO ⇒ `logt ring` 可自证；`DECODE_FAIL` / `SENSOR_LOST` 为 WARN ⇒ `logt flash` 跨重启可查 |

**本阶段明确未接入**

| 候选 | 判定 |
|---|---|
| 扫描开始 / 设备发现 / 状态写入 | ❌ **不建议**：无业务事实 / callback 内 / 与 `DATA_DECODED` 重复（定义点唯一原则） |
| `onResult()` `xQueueSend` 失败（队列满） | ❌ **无对应 EventId** ⇒ 不接入，登记 **`LV-3`**（与 `LV-1` 同性质：有宿主候选但无 ID） |
| `BLE_INIT_FAILED`（`MiThermometerInit()` MAC/bindkey 空、`xQueueCreate` 失败） | ❌ **需新增 ID（未批准）**；`:247` 分支 `return false` 但调用方 `main.cpp:508` **忽略返回值** ⇒ 模块永久不可用却静默 |
| 失败原因分类（MIC vs 格式） | ⏸ **延期**：需改 `lywsd03_decrypt()` 签名（超出"只加观测"） |

> 📄 审查依据：`docs/archive/log/Phase5-BLE-OLED-Dispense边界与LogManager完整性审查0920.md` §1；进度记录：`docs/archive/log/LogManager-P2-Progress.md` §Next「Phase 5-C」。

---

## 12. Command / Event / OLED（收尾）

| 模块 | 源码 | 现状 | 建议接入 |
|---|---|---|---|
| Command | `command_manager.cpp`（3204） | 有 `command_manager_set_log_callback()` 且**已在 `main.cpp:135` 注册**（打到串口） | `LOG_CMD_APPLIED`(INFO) / `LOG_CMD_REJECTED`(WARN) / `LOG_CMD_RUNTIME_QUEUE_FULL`(WARN) / `LOG_CMD_RUNTIME_TIMEOUT`(WARN) + `LOG_P_CMD_ID`⚠️ / `LOG_P_QUEUE_SIZE`。建议把已注册的 bridge 改为"串口 + LogManager"双路 |
| Event | `event_manager.cpp` | ✅ **已接入（P2-M，2026-09-20）** —— 有风暴抑制（`EVENT_STORM_MAX_PER_EVENT=5` / `500ms`）+ `drop_count` | `LOG_EVT_QUEUE_FULL`(WARN) / `LOG_EVT_STORM_DROPPED`(WARN)。**按契约"走侧信道计数上报"实现**：**不为每条丢弃事件发日志**，改为**周期聚合 1 条**（窗口 60000 ms，节拍器 = `event_dispatch()` 首行）。★ **只记自身运行异常**：业务事件一律不记（17 个 `event_push` 中 13 个发布方已埋 `log_emit` ⇒ 转发即重复）。宿主 = **确定丢弃的分支**（`0x0C01` 埋返回 `EVENT_QUEUE_FULL` 的唯一出口；`0x0C02` 埋风暴抑制分支），**不在调用者、不在入口**；**Σ `LOG_P_COUNT` = 真实丢弃数量**。详见 `docs/archive/log/LogManager-P2M-Event接入审查0920.md` |
| OLED | `oled.cpp`（301）+ `oled_animation.h`（1367） | ⚠️ **判定已修正（Phase 5 审查，2026-09-20）**：**不是"无宿主"，而是"有异常、有 ID、但缺判据"** | `LOG_OLED_INIT_FAILED`(0x0E01, WARN)。★ **缺判据的根因**：`oled_init()` `:103` `oled.begin();` **丢弃了 `U8g2::begin()` 的 `bool` 返回值**（对照 `U8g2lib.h:144` `bool begin(void)`），并在 `:110` **无条件**打印 `"OLED init OK"` ⇒ **假成功**（OLED 未接 / I2C NACK / 地址错误时串口仍显示 OK）⇒ 登记 **`OLED-1`**。⇒ **当前维持"不埋点"**（埋点无可依附判据；修 `begin()` 检查属**生产代码改动**，须单独批准）。<br>★ **分层澄清**："占位"只适用于**事件响应层**（`oled_event_handler()` 仅 2 个有效 case 且都只 `Serial.println`；注册 4 个事件、3 个落 `default`）；**显示层已实现**。附带缺陷：`oled_restart()`（`:251`）**零调用者 = 死代码**；`oled.h:16` 的 `void oled_event_handler(SystemEvent)` **永不定义 = 死声明**；`oled_event_init()` **4 次 `event_subscribe()` 返回值全被忽略**（与 `LV-1` 同构） |
| ComputerReset | `computer_reset.cpp`（475） | 见 §1 | `LOG_CRESET_*` 三个 |
| Registry | `capability_registry.cpp`（1029） | ✅ **已接入（P2-L，2026-09-20）** | `LOG_REG_REBUILT`(INFO) / `LOG_REG_SAVE_FAILED`(ERROR)，均落在 `registry_sync()` 内（**2 处 `log_emit`**）。宿主 = 定义点而非触发点：重建 → `else` 分支（checksum 不一致/缺失/损坏）；保存失败 → `save_registry_file()` 返回值判定处。**不得**埋 `command_manager` 的 4 个 rescan 调用点（一次 create 连触发 3 次）与 `save_registry_file()` 内部 4 个失败出口 |

---

## 13. 全矩阵速览（接入优先级 × 工作量 × 风险）

| # | 模块 | 现有日志 | 建议埋点数 | 阻塞项 | 优先级 | 风险 |
|---|---|---|---|---|---|---|
| 1 | System/Boot | ✅ **已接入 6 个**（Phase 5-A，`6194e3b` + `9c32675`） | ✅ **6**（`0x0101/0x0102/0x0103/0x0109/0x010A/0x010C`）；剩余 5 个明示排除（`0x0104/0x0105/0x0106/0x0107/0x0108`）+ `0x010B` 无合法宿主 | `RESTART_EXECUTED`(INFO) 因重启可能丢失 ⇒ **`PROTO-2`（OPEN/DEFERRED）**；R-1（FS 失败在 log_init 前） | Phase 5-A | **低** |
| 2 | Storage | 回调存在但**静默**（json 37 + file 30） | 6 类（+67 条自动） | 字符串 `LOG_P_PATH` 不可达 | P0 | 低（零侵入） |
| 3 | Config | 静默 | ~12 | `LOG_P_KEY` 不可达；缺 SAVE_FAILED | P0 | 中（事务语义） |
| 4 | WiFi | 无 | ~6 | 重连洪泛需限流 | P1 | 低 |
| 5 | Cloud/MQTT | 仅 log_ack 常量 | ~9 | 发布失败洪泛；缺 AUTH_FAILED | P1 | 中（IMM 语义） |
| 6 | Time/RTC | 无 | ~10 | 无 | P1 | 低 |
| 7 | Workflow | **0** | ~13 | 🔴 Critical Op release 收口；缺 WF_STOPPED | P1 | **高** |
| 8 | Weight | 无 | ~8 | 🔴 高频源，必须边沿 | P1 | 中 |
| 9 | Valve | 无 | ~8 | 缺 NOT_READY；FORCE_CLOSE 无失败分支 | P1 | 中（安全） |
| 10 | Dispense Guard | 无 | ✅ **1**（`0x0511` WARN） | ✅ **已完成（Phase 4，`94912bb`）**：纯转发模块 ⇒ **只记"决策"**；`0x0511` 与 Valve 的 `0x0505` **互补非冗余**（后者 5 s 门控丢次数，前者 ΣCOUNT 守恒）；已注册进合并白名单 | Phase 4 | **低** |
| 11 | BLE | ✅ **已接入 4 个（Phase 5-C，2026-09-21，`70b0c76`）** | ✅ **4**（`0x0901/0x0902/0x0903/0x0904`，**零新增**） | ✅ **已完成**：4 处埋点**全在 `MiThermometer_task()`（loop）**；★ **callback（`onResult()`）内 `log_emit` 站点 = 0**（objdump 证明）—— 该回调只有 MAC 过滤 + memcpy + `xQueueSend`，无可记录事实；★ `DECODE_FAIL` 走**模块内 60 s 窗口聚合**（task 首行节拍器）⇒ bindkey 错误场景 **20 条/min → 1 条/min**（Flash 环 25 min → 8.3 h）；★ 未接入项：队列满丢包（**无 ID** ⇒ `LV-3`）/ `BLE_INIT_FAILED`+`BLE_QUEUE_FULL`（需新增 ID，未批准）/ 失败原因分类（需改 `lywsd03_decrypt()` 签名，延期）；**仅改 `MiThermometer.cpp` `+74/−0`**，RAM **+8 B** / Flash **+256 B**；**上板验证未执行**（无串口设备） | Phase 5-C | **低**（已完成，宿主全在 loop + 聚合已落地） |
| 12 | Command/Event/OLED/Registry | ✅ **Command=P2-J / Event=P2-M / Registry=P2-L 完成**；OLED=**判定修正为"缺判据"**（`OLED-1`） | ✅ | 侧信道聚合 | P2 / Phase 5 | 低 |
| — | **System/Boot `0x01xx`** | **12 个 ID 整段零宿主**（`grep LOG_SYS_` 仅 1 行注释） | ✅ **全部已冻结** | 🔴 **★ Phase 5 判定为最高优先级接入项**：`system_command.cpp:222` **已缓存 `esp_reset_reason()`**、`:97` **已有名称映射表** ⇒ **有 ID、有判据、有宿主，只差接线**；全部为**启动期一次性**记录 ⇒ **零频率风险、零 Flash 压力**。接入前需裁决：`0x0106 FS_MOUNT_FAILED` 与 `LOG_STG_FS_UNAVAILABLE`(0x0301) 去重；`HEAP_LOW` 门控策略 | **Phase 5** | **极低** |

---

## 14. 本矩阵未做的事

- ❌ 未新增任何 EventId（§2.2 / §4.2 / §5.2 / §7.2 / §9.2 / §10.2 / §11.2 中的"建议新增"仅为提案）
- ❌ 未新增任何 ParamId
- ❌ 未修改 `src/log/log_events.h`
- ❌ 未修改 LogManager API
- ❌ 未修改任何 `src/` 生产代码

---

## 15. P2 最终覆盖状态（**2026-09-22 冻结 · Phase 7-2**）

> **本节为权威最终状态**，取代 §13「全矩阵速览」的设计期视图（§13 保留作设计演进记录）。
> 本节**零代码改动**，仅整理已落地事实。`未修复的问题.md` 同步新增「LogManager 后续事项」。

### 15.1 已完成模块（16 个）

| # | 模块 | 接入方式 | 完成阶段 |
|---|---|---|---|
| 1 | **SystemCommand** | 显式 `log_emit`（System/Boot 段 `0x01xx`） | Phase 5-A |
| 2 | **Config Manager** | `cfg_log()` 回调桥接 + 显式埋点 | P2-B |
| 3 | **Storage — json_storage** | 回调桥接（`json_storage_log_bridge`） | P2-A |
| 4 | **Storage — file_storage** | 回调桥接（`file_storage_log_bridge`） | P2-A |
| 5 | **Storage — bin_storage** | 回调桥接（`bin_log_bridge`） | **Phase 6-A** |
| 6 | **Storage — workflow_storage** | 显式 `log_emit` | **Phase 6-B / 6-C / 7-1** |
| 7 | **WiFi** | 显式 `log_emit` | P2-C |
| 8 | **Cloud Manager** | 显式 `log_emit` | P2-D |
| 9 | **Time Manager** | 显式 `log_emit` | P2-E |
| 10 | **Workflow Manager** | 显式 `log_emit` | P2-F / **Phase 6-B / 7-1** |
| 11 | **Weight / HX711** | 显式 `log_emit`（边沿 + 突发合并） | P2-I |
| 12 | **Valve** | 显式 `log_emit`（含 5 s 门控） | P2-H |
| 13 | **Command Manager** | 显式 `log_emit`（4 个 ID） | P2-J |
| 14 | **ComputerReset** | 显式 `log_emit`（3 个 ID） | P2-K |
| 15 | **Capability Registry** | 显式 `log_emit`（2 个 ID） | P2-L |
| 16 | **Event Manager** | 周期聚合（60 s，走侧信道计数） | P2-M |
| — | **DispenseGuard** | 显式 `log_emit`（`0x0511`，只记"决策"） | Phase 4 |
| — | **BLE / MiThermometer** | 显式 `log_emit`（4 个 ID，callback 零埋点） | Phase 5-C |

> 上表 18 行：16 个"业务模块" + DispenseGuard + BLE（二者在 §13 中单列）。

### 15.2 Phase 6 / 7 新增明细

**Phase 6-A（`00314f2`）—— `bin_storage`**

| 项 | 内容 |
|---|---|
| 覆盖 | `bin_storage.cpp` **23 个 `bin_log` 站点**（E=17 / W=5 / I=1） |
| 机制 | 复用 P2-A 的 callback bridge（`bin_log_serial` → `bin_log_bridge`）⇒ 串口格式不变 + LogManager 双路 |
| 模块编号 | `STG_BRIDGE_MODULE_BIN` = **2**（json=0 / file=1 / bin=2 / workflow_storage=3，共用 `LOG_P_MODULE` 编号空间） |
| EventId | **全部复用 Storage 段既有 ID**（`0x0301` FS_UNAVAILABLE / `0x0302` ATOMIC_WRITE_FAILED / `0x0303` CRC_FAILED / `0x0305` WRITE_VERIFY_FAILED / `0x0306` READ_FAILED）⇒ **零新增 EventId** |
| 改动 | 仅 `src/main.cpp` +33/−10 |

**Phase 6-B / 6-C（`714d009` + `6d40944`）—— `workflow_storage` meta 加载链路**

| 位置 | 语义 | EventId | Level | ERR_CODE |
|---|---|---|---|---|
| `deserialize_meta()` 失败 | meta BIN 内容损坏 | **0x0303** `LOG_STG_CRC_FAILED` | **CRITICAL** | `r` 原值（10/11/12/13） |
| `file_size == 0 \|\| > MAX` | meta 文件尺寸非法 | **0x0306** `LOG_STG_READ_FAILED` | ERROR | **12** `FORMAT_INVALID` |
| `bin_storage_read()` 失败 | meta BIN 读取失败 | **0x0306** | ERROR | **6** `READ_FAILED` |
| `bytes_read != file_size` | 读取长度不足 | **0x0306** | ERROR | **6** `READ_FAILED` |
| `workflow.cpp` 加载 `def_buf == NULL` | WorkflowDefinition 分配失败 | **0x040B** `LOG_WF_RUNTIME_ALLOC_FAILED` | ERROR | —（用 `NEED_BYTES`） |

> ★ **跨域复用先例**：Workflow 域的 meta 损坏复用 **Storage 段 `0x0303`** —— 理由：BIN 持久化数据损坏属"存储完整性失败"，非 Workflow 业务错误。
> ★ **模块编号** `WF_STG_LOG_MODULE` = **3**；`WF_STG_LOG_PATH_META` = **1**（path 标识空间）。

**Phase 7-1（`bb2b3cc`）**

| 位置 | 语义 | EventId | Level | 参数 |
|---|---|---|---|---|
| `main.cpp` DEF-1 修复 | 注册前置 ⇒ 初始化期 ERROR 不再静默丢弃 | —（无新增埋点） | — | — |
| `workflow.cpp::workflow_load_from_storage()` 单 Slot 加载失败 | 单个 Workflow Slot 不可用 | **0x0306** `LOG_STG_READ_FAILED` | **WARN** | `MODULE` + `SLOT` + `ERR_CODE` |
| `workflow_storage.cpp::stage_process()` rename 失败 | 已提交事务未发布 ⇒ 内容停旧版本 | **0x0302** `LOG_STG_ATOMIC_WRITE_FAILED` | **ERROR** | `MODULE` + `PATH`(=2) + `ERR_CODE`(=9) + `STAGE` |

> ★ `stage_process()` **一个埋点覆盖两条路径**：`recover_internal()`（掉电恢复）**与** `workflow_storage_save()` 的发布/清理 ⇒ 避免重复埋点。
> ★ **DEF-1 二进制级证据**：基线 `4201bc19 call8 bin_storage_init` → `4201bc2b call8 set_log_callback`；修复后 `4201bc20 call8 set_log_callback` → `4201bc23 call8 bin_storage_init`。

### 15.3 P2 完成判定

| # | 完成条件 | 状态 |
|---|---|---|
| 1 | 主要业务模块均具备事件观测能力 | ✅ |
| 2 | Storage 持久化链路可观测 | ✅ |
| 3 | Workflow BIN 数据完整性失败可观测 | ✅ |
| 4 | 初始化阶段日志链路完整 | ✅（DEF-1 已修） |
| 5 | 静默失败路径完成第一轮覆盖 | ✅ |

**P2 明确不包含（转入独立 backlog）**

| 项 | 归口 |
|---|---|
| **OLED** | `OLED-1` —— 有异常、有 ID、但缺判据（`oled_init()` 丢弃 `U8g2::begin()` 返回值）⇒ 维持不埋 |
| **`test_mqtt` 清理** | Technical Debt —— 非 LogManager 范畴 |
| **Cloud credential 安全整改** | Security —— `cloud_manager.cpp` 明文打印 MQTT username/password |
| **Reliability 专项** | `R-7` / `R-8`（DEFERRED）· `LV-1` / `LV-2` / `LV-3` · `PROTO-1` / `PROTO-2` · `P0-1b` / `P0-4` |

### 15.4 未接入但仍为候选（**P3，本轮不处理**）

`workflow_storage.cpp:1525` 保存发布 rename 失败 · `:1500` meta commit 失败 · `:1450/:1464` save readback 失败 / crc mismatch · `main.cpp:447` LittleFS mount 失败（对应零宿主的 `LOG_SYS_FS_MOUNT_FAILED` `0x0106`）。

---

*文档结束 —— 待人工审核*
