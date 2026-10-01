# LogManager P2 接入准备审查 0918

> 阶段：P2 前置（**只分析，不开发**）
> Git 基线：`65b4523 fix(log): complete P1.5 ack hole and replay cursor fixes`
> （其后 `1d0b47c chore(repo): stop tracking local credentials and archive P1.5 reports` 为仓库整理，不含代码变更）
> 性质：**审查报告**。未修改任何 `src/` 生产代码、未新增 EventId / ParamId、未修改 `log_events.h`、未修改 LogManager API、未执行 `git add` / `commit`。
> 关联文档：
> - 详细接入矩阵：**`docs/P2_Log_Integration_Matrix.md`**（每模块源码/现状/埋点/EventId/Level/Params）
> - 接口规范：**`docs/LogManager-Integration-Guide.md`**
> - 历史埋点表（**参考来源，不直接采用**）：`log模块历史/LogManager全系统审计报告0914.md` §5

---

## 1. 当前基线

### 1.1 代码基线

| 项 | 值 |
|---|---|
| HEAD | `65b4523` |
| 工作区 | `src/`、`test/` **零改动**（`git diff --stat -- src/ test/` 为空）；仅有 `docs/` 未跟踪 + 2 个 `.workbuddy/memory/*` |
| LogManager 状态 | P1.1–P1.5 全部完成；设备侧测试仍 **BLOCKED**（开发板未连接，非代码问题） |
| 契约测试 | 4/4 ALL PASS（ACK 探针 105 + 22 项主机端断言，无需开发板） |

### 1.2 LogManager 冻结能力（P2 必须按此使用，不得假设其他接口）

| 能力 | 实测值 | 出处 |
|---|---|---|
| 记录入口 | `log_emit(LogEventId, LogLevel, const LogParamIn*, uint8_t)` / `log_emit0(id, level)` | `log_manager.h:198/201` |
| 参数 helper | `log_arg_i32 / u32 / f32 / bool / enum` —— **仅此 5 个** | `log_manager.h:210-253` |
| `LogParamIn` | `{ uint8_t id; uint8_t type; union { int32_t i; uint32_t u; float f; uint8_t b; } v; }` —— **无 blob 字段** | `log_events.h:432-443` |
| 参数上限 | `LOG_MAX_PARAMS = 8`，**超限整体拒绝（不截断）** | `log_events.h:410` |
| Level 策略 | DEBUG(flash0,cloud0) / INFO(0,1) / WARN(1,1) / ERROR(1,1) / CRITICAL(1,1)，**无 EventId 级例外** | `log_events.h:64-69` |
| RAM 环 | `LOG_RAM_QUEUE_SLOTS = 64` × `LOG_RECORD_SIZE = 128B` = 8192B（PSRAM 优先，失败回退 DRAM） | `log_manager.h:34`、`log_manager.cpp:1313-1326` |
| 每轮消费 | `LOG_DRAIN_MAX_PER_TASK = 8`（`log_task()` 在 `loop()` 内，无独立 Task） | `log_manager.h:37` |
| Flash 段环 | 16 段 × 31 条 = 496 条；段大小 3984B | `log_events.h:479-483` |
| Cloud | 队列 `LOG_CLOUD_QUEUE_SLOTS = 128`；批次 fmt=2、≤16 条、≤4096B；TX 最小间隔 500ms | `log_manager.h:61`、`log_events.h:512-515` |
| ACK | 超时 15s、最多 5 次、退避 base 2s / max 60s；**at-least-once，云端必须幂等** | `log_events.h:520-522`、`log_ack.h:38` |
| 未就绪安全 | `log_emit()` 在 `s_ready == false` 时 **直接 return false**（不崩溃） | `log_manager.cpp:1443-1446` |
| 调用上下文 | 内部 `portENTER_CRITICAL()`（**非 `_ISR` 变体**）⇒ **禁止 ISR 调用** | `log_manager.cpp:1453` |

### 1.3 业务模块接入现状（实读结论）

```
main.cpp                41 → 全部是串口测试钩子（logtest/logfill），非业务埋点
cloud_manager.cpp        7 → 全部是 P1.5 log_ack 协议常量，非业务埋点
config_manager.cpp       2 → 常量/注释
json_storage.cpp         2 → 回调机制
file_storage.cpp         2 → 回调机制
bin_storage.cpp          2 → 回调机制
system_state / system_command / computer_reset / wifi_module /
time_manager / workflow / workflow_storage / capability_registry /
dispense_guard / valve / weight / MiThermometer /
event_manager / command_manager / oled                    0
```

⇒ **结论：生产代码的 LogManager 业务接入率为 0。** P2 是"从零开始"，不是"补全"。

---

## 2. 接入原则

### 2.1 六条铁律

1. **只调 `log_emit()` 家族**。业务模块**禁止**调用 `log_flash_*` / `log_meta_*` / `log_seg_*` / `log_cloud_test_*`，也禁止关心 Flash / ACK / MQTT / CBOR / Retry / 补发 / 段回收。
2. **Level 由语义决定，不由情绪决定**。看 §2.2 决策树。**不存在 EventId 级例外**（P1 已撤销全部 3 条 INFO Flash 例外）。
3. **状态迁移成对记录**：`LOG_P_WAS`(0x47) + `LOG_P_STATE`(0x44)。单值日志无法还原迁移图。
4. **边沿优先于周期**。凡是"持续为真"的条件（错误、失联、队列满），**只在 true↔false 边沿发日志**，周期性的用计数聚合。
5. **禁止在 ISR 与 NimBLE 回调中 `log_emit()`**（`portENTER_CRITICAL` 非 ISR 变体 + BLE 回调已过重）。
6. **不追求全**。宁可少几条可解释的，不要多几条解释不了的。INFO 也占 RAM 环与云队列。

### 2.2 Level 决策树

```
这次事件之后，设备还能正常完成它的核心功能吗？
├─ 不能，且有安全/数据损坏风险（阀门失控、强制关阀、配置双份损坏、FS 挂了）
│    └─ CRITICAL（触发立即 flush）
├─ 不能，但能自愈或降级继续（模块加载失败、保存失败、校准失败）
│    └─ ERROR
├─ 能，但走了异常路径 / 需要重试 / 已降级（超时、断线、重连、回退 Backup、被限流）
│    └─ WARN
├─ 能，且这是正常的、有诊断价值的结果或状态变化
│    └─ INFO
└─ 只是开发期排障信息（HX711 raw、BLE raw packet、MQTT polling、timer tick、state polling）
     └─ DEBUG（不进 Log 系统，只累加侧信道计数）—— 或干脆用串口
```

**判断口诀**：
- **能不能回答"发生了什么 + 为什么"** → 不能就加 `LOG_P_CAUSE` / `LOG_P_ERR_CODE`，而不是加日志条数。
- **这条日志会不会在 1 分钟内重复 10 次以上** → 会，就必须边沿化 / 限流 / 聚合。
- **这条日志错了会不会要重启设备才能恢复** → 会，至少 WARN。

### 2.3 反模式（P2 明确禁止）

| 反模式 | 为什么错 | 正确做法 |
|---|---|---|
| `log_emit0(LOG_VALVE_OPEN, LOG_LVL_INFO)` 放在 `valve_task()` 里每 loop 调 | 每秒数十条 INFO，挤掉 WARN+ | 放在 `valve_set_gpio()` 的状态切换分支（天然边沿，:97 有重复调用 return） |
| 在 `weight_record_jump()` 里记录每次跳变 | 5s 内最多 5 次 + 恢复时再来一轮 | 走 `weight_refresh_error_state()` 的 `err != error_state` 边沿 |
| 在 `MiAdvCallback::onResult()` 里记录每个 ADV | BLE host 任务上下文 + 数十包/秒 | 回调只置标志，`MiThermometer_task()` 按扫描窗口聚合 |
| MQTT 每次 publish 失败都记 | 离线时每 loop 失败 | 计数 + 周期聚合 1 条 |
| 用 `LOG_P_PATH` 传文件路径 | `LOG_PTYPE_STR` **当前不可达**（见 §4.2） | 哈希化 / 改为 `LOG_P_MODULE` + `LOG_P_KEY` 枚举 |
| 忽视 `log_emit()` 返回值 | CRITICAL 未进环 = 静默丢失 | CRITICAL 必须检查返回值，失败时保留串口兜底 |

---

## 3. 模块接入顺序（建议）

### 3.1 推荐顺序

```
① System/Boot  →  ② Storage  →  ③ Config  →  ④ WiFi  →  ⑤ Cloud/MQTT
      →  ⑥ Time/RTC  →  ⑦ Workflow  →  ⑧ Weight  →  ⑨ Valve
      →  ⑩ Dispense Guard  →  ⑪ BLE  →  ⑫ Command/Event/OLED/Registry
```

### 3.2 顺序理由

| # | 模块 | 理由 |
|---|---|---|
| ① | **System/Boot** | `boot_seq` 是日志主键的一部分（`log_ack` 的 `b` 字段），没有启动基线，后续所有日志缺少上下文。且 `reset_reason` **当前从不落盘** —— 现场反复重启无法归因。工作量小、依赖零。 |
| ② | **Storage** | **性价比最高、风险最低**：回调已存在但从未注册，`json_storage` 37 处（30E+7W）+ `file_storage` 30 处（24E+6W）E/W **完全静默**，共 67 条。只需在 `main.cpp` 加两行注册 bridge，**零业务代码改动**即可全部变为可观测。且 Storage 是 LogManager 自己的落盘底层（都依赖 LittleFS），先让它可见才能判断"日志落不下去"是 Log 的问题还是 FS 的问题。 |
| ③ | **Config** | 依赖 Storage（②已完成）。配置错误是现场故障首因，且 ConfigManager 已有非常完整的错误语义（Backup 恢复 / 提交回滚 / 版本重建 / 出厂重置），只需映射到已冻结的 10 个 EventId。 |
| ④ | **WiFi** | 云路上游。WARN 类（LOST / TIMEOUT）诊断价值最高，且状态机清晰、埋点位置无歧义。 |
| ⑤ | **Cloud/MQTT** | 依赖 WiFi。注意：LogManager 自身的 6 个事件（`LOG_LOG_*`）由 LogManager 内部产生，**Cloud 侧不需要也不应该重复记**。Cloud 只需补 CONNECTED/DISCONNECTED/AUTH/SLEEP/PUBLISH_FAIL/FRAG_FAIL。 |
| ⑥ | **Time/RTC** | 日志的 `timestamp` 依赖它（`log_emit()` 在时间无效时写 0 且不置 valid 位）。RTC 路径已接入 Critical Op（仅 RTC 写），接入时不得改变 release 语义。上板实测 `rtc_present=false`，此模块日志是定位依据。 |
| ⑦ | **Workflow** | 事件最多（12 个已冻结 ID）、代码最大（4465 行），且**与 Critical Op 强耦合**。`workflow_terminate()` 是 release 收口点 —— 项目铁律：**release 不能放在会中途 return 的函数里**，日志接入**不得新增任何 return 路径**。故必须放在前面各模块稳定之后。 |
| ⑧ | **Weight** | 高频源，必须先定好边沿方案再动手（§5）。且它是 Dispense 正确性判据。 |
| ⑨ | **Valve** | **放在 Weight 之后**：Dispense 链路是 `Weight error → force close valve`，先有 Weight 日志才能解释 Valve 的强制关阀/安全超时。Valve 属安全相关（CRITICAL/IMM）。 |
| ⑩ | **Dispense Guard** | 依赖 ⑧+⑨，且当前代码中"Dispense 模块"并不存在（`LOG_DISPENSE_*` 四个 ID 无宿主），需 P2 先定义宿主。 |
| ⑪ | **BLE** | **最高频 + 回调上下文禁令**，最难做对，放最后。 |
| ⑫ | **其余** | Command / Event / OLED / Registry 收尾，低风险。 |

### 3.3 与用户提案顺序的两处偏差（及理由）

| 提案顺序 | 本建议 | 理由 |
|---|---|---|
| Config → Storage | **Storage → Config** | ConfigManager 完全构建在 JsonStorage 之上（`ConfigManager → JsonStorage → LittleFS`）。先让 Storage 的 67 条静默错误可见，Config 出问题时才能一眼区分是"文件层坏了"还是"配置语义错了"。且 Storage 是零侵入改动，可先落地验证 bridge 模式。 |
| Workflow → Water → Weight | **Weight → Valve → Dispense** | `dispense_guard` 的触发源是 `EVENT_WEIGHT_ERROR`，先有 Weight 日志才能解释为什么关阀；Valve 的 `LOG_VALVE_FORCE_CLOSE` 是 CRITICAL，其 `LOG_P_CAUSE` 需要引用 Weight 侧的值。 |

### 3.4 一个重要认知纠正（沿用 0917 结论）

用户在前序任务中给出的依赖链是 `System State → Event Manager → LogManager`。**实际方向相反，且是刻意设计**：

- LogManager **不依赖** System State（State Context 未通过公开 API 暴露）
- LogManager **不依赖** Event Manager（否则 Event 打 Log、Log 又依赖 Event ⇒ 递归风险）

真实方向是 **它们 → LogManager**。且 **LogManager 已在 `main.cpp:80` 完成初始化**，位于所有业务模块 init 之前 ⇒ **不阻塞任何模块接入**。

---

## 4. EventId 与 ParamId 覆盖分析

> ⚠️ 本节**只提出建议，未修改 `log_events.h`，未新增任何 ID**。

### 4.1 EventId 覆盖情况

#### 已冻结的段划分（15 段 + 1 预留）

| 段 | 模块 | 已用 ID 数 | 覆盖评价 |
|---|---|---|---|
| `0x01` | System | 12（0x0101–0x010C） | ✅ 完整（启动/复位/重启/堆/PSRAM/FS/Critical 下溢） |
| `0x02` | Config | 10（0x0201–0x020A） | ⚠️ 缺"保存失败"（只有 COMMIT_FAILED_ROLLBACK） |
| `0x03` | Storage | 6（0x0301–0x0306） | ✅ 完整 |
| `0x04` | Workflow | 12（0x0401–0x040C） | ⚠️ 缺"被强制停止 / 启动被拒" |
| `0x05` | Water（Dispense+Valve+Weight） | 16（0x0501–0x0510） | ⚠️ 缺"阀门不可用"；缺"校准开始" |
| `0x06` | WiFi | 7（0x0601–0x0607） | ⚠️ 缺 RSSI 过低；`PROVISION_*` 无实现宿主 |
| `0x07` | Cloud/MQTT/Log 自身 | 11（0x0701–0x070B） | ⚠️ 缺 MQTT 认证失败、下行非法 |
| `0x08` | Time | 10（0x0801–0x080A） | ✅ 完整 |
| `0x09` | BLE | 4（0x0901–0x0904） | ⚠️ 缺初始化失败、队列满 |
| `0x0A` | Command | 4 | ✅ 完整 |
| `0x0B` | Registry | 2 | ✅ 完整 |
| `0x0C` | EventMgr | 2 | ✅ 完整（且已注明走侧信道计数） |
| `0x0D` | ComputerReset | 3 | ✅ 完整 |
| `0x0E` | OLED | 1 | ✅ 够用 |
| `0x0F` | Motor | 预留 base `0x0F00` | — |

#### 建议新增清单（**提案，需单独评审 + 单独提交**）

| 建议 ID | 名称 | Level | 触发位置 | 紧迫度 |
|---|---|---|---|---|
| `0x020B` | `LOG_CFG_SAVE_FAILED` | ERROR | `config_save()` 失败且未回滚 | 高 |
| `0x040D` | `LOG_WF_STOPPED` | WARN | `workflow_stop/disable/clear`、`workflow_start()` acquire 被拒 | 高 |
| `0x0511` | `LOG_VALVE_NOT_READY` | WARN | `valve_init()` 禁用 / 引脚未配置 / 注册失败 | 高 |
| `0x0905` | `LOG_BLE_INIT_FAILED` | ERROR | `MiThermometerInit()` MAC/bindkey 缺失、队列创建失败 | 中 |
| `0x0906` | `LOG_BLE_QUEUE_FULL` | WARN | `onResult()` `xQueueSend` 失败（侧信道聚合） | 中 |
| `0x0608` | `LOG_WIFI_RSSI_LOW` | WARN | `wifi_update_signal()` | 中 |
| `0x070C` | `LOG_MQTT_AUTH_FAILED` | ERROR | `MQTT_EVENT_ERROR` / CONNACK 非 0 | 中 |
| `0x070D` | `LOG_CLOUD_RX_INVALID` | WARN | 下行消息解析失败 / 协议错误 | 中 |
| `0x0512` | `LOG_WEIGHT_CALIB_START` | INFO | `weight_zero_calibrate()` | 低（可选） |

> **处理方式建议**：新增 EventId 属于**冻结契约变更**，必须**单独一次 commit**、**单独评审**，且必须同步更新：① `log_events.h`；② `docs/LogManager-Integration-Guide.md` 附录 A；③ `docs/P2_Log_Integration_Matrix.md`；④ 云端 EventId 字典。**P2 接入阶段不要夹带新增 ID** —— 先用现有 104 个 ID 完成接入，缺口用 `LOG_P_CAUSE` / `LOG_P_ERR_CODE` 区分（已验证可行：Weight 的 5 类异常可全部复用 `LOG_WEIGHT_ERROR_ENTER` + `LOG_P_CAUSE`）。

#### 冲突避免规则

1. 段前缀 = 模块，段内序号递增，**永不复用已废弃的号**。
2. 新增前先查 `log_events.h:214-345` 全表 + `docs/LogManager-Integration-Guide.md` 附录 A。
3. **Cloud 可解析性**：EventId 是 uint16，云端靠 `(段 << 8)` 路由到模块字典；**段号一旦分配永久固定**，即使模块重构也不变。

### 4.2 ParamId 使用情况分析

#### 现状

- 已定义 `0x00`–`0x58`（89 个可用），`LOG_P_MAX = 0x59`。
- 类型枚举有 8 种：`I32/U32/F32/BOOL/ENUM/STR/I8/U16`（`log_events.h:86-96`）。
- **但 `log_arg_*` 只有 5 个**（i32/u32/f32/bool/enum），且 `LogParamIn` 的 union 只有 `{i,u,f,b}`、**没有 blob 字段**，`log_emit()` 也没有 blob 形参。

#### 🔴 P0 发现：三种参数类型不可达

```
LOG_PTYPE_STR  = 6   // 值 = (blob_off << 16) | blob_len，字符存于 blob
LOG_PTYPE_I8   = 7
LOG_PTYPE_U16  = 8
```

**这三种类型当前无法构造任何参数。** 而语义上应为字符串的 ParamId 至少有 7 个：

| ParamId | 语义 | 影响的事件 |
|---|---|---|
| `LOG_P_WF_ID` (0x02) | Workflow id（String） | `LOG_WF_START` / `LOG_WF_CRUD` |
| `LOG_P_ACTION_ID` (0x0B) | Action id（"VALVE_OPEN"） | `LOG_WF_ACTION_FAILED` / `LOG_WF_TEMP_ACTION_TIMEOUT` |
| `LOG_P_KEY` (0x1E) | Config key | `LOG_CFG_CHANGE_APPLIED` |
| `LOG_P_PATH` (0x25) | 文件路径 | 全部 `LOG_STG_*` |
| `LOG_P_CMD` (0x2E) | 命令名 | `LOG_MQTT_CMD_EXEC_FAILED` / `LOG_CLOUD_RX_INVALID` |
| `LOG_P_OBJ` (0x2F) | 对象 runtime_id | Command / Registry |
| `LOG_P_SERVER` (0x34) | NTP/MQTT 服务器 | `LOG_TIME_NTP_*` / `LOG_MQTT_CONNECTED` |

**两个解决方向（需人工拍板，都各有代价）**：

| 方案 | 做法 | 代价 |
|---|---|---|
| **A. 哈希/枚举化（推荐）** | 沿用 `LOG_P_SSID_HASH`(0x33) 的先例，字符串一律转 `u32` 哈希或枚举编号；云端维护字典 | 哈希不可逆，云端需设备侧字典；路径/命令名的可读性下降。但**不改冻结 API**，符合当前约束 |
| **B. 扩展 API** | 给 `LogParamIn` 加 `const void* blob; uint8_t blob_len;` 并提供 `log_arg_str()` | 属 **API 变更 + Record 布局影响**，需单独评审；且违背"不改 LogManager API"的当前阶段约束 |

> **本阶段建议：采用方案 A，先把 P2 接入做完**；把方案 B 列入 Future Improvement（与 0917 审查的 F-2 同一条）。

#### 其余 ParamId 充足性评估

| 模块 | 需要的参数 | 现有 ParamId | 结论 |
|---|---|---|---|
| System | boot_seq / init_ms / reset_reason / free_bytes / need_bytes / hold_ms / count | `0x21 / 0x22 / 0x20 / 0x3A+0x56 / 0x39 / 0x43 / 0x45` | ✅ 全部有 |
| Config | module / key / version / reason / remain_ms / err_code | `0x1D / 0x1E⚠️ / 0x4B / 0x1F / 0x58 / 0x0C` | ⚠️ 仅 `key` 受 STR 影响 |
| Storage | path⚠️ / stored_crc / calc_crc / stage / module | `0x25⚠️ / 0x27 / 0x28 / 0x26 / 0x1D` | ⚠️ 仅 `path` 受 STR 影响 |
| WiFi | ssid_hash / rssi / connect_ms / connected_ms / timeout_ms / attempt_n / retry_n / state / was | `0x33 / 0x31 / 0x32 / 0x2C / 0x08 / 0x30 / 0x29 / 0x44 / 0x47` | ✅ 全部有 |
| Cloud | outbox / queue_size / server⚠️ / err_code / sleep_ms / cmd_id⚠️ | `0x2B / 0x53 / 0x34⚠️ / 0x0C / 0x2A / 0x2D⚠️` | ⚠️ 2 个受 STR 影响 |
| Time | unix / drift_ms / server⚠️ / addr / raw / err_code / attempt_n | `0x35 / 0x36 / 0x34⚠️ / 0x37 / 0x18 / 0x0C / 0x30` | ⚠️ 1 个受 STR 影响 |
| Workflow | slot / wf_id⚠️ / variant / steps_done / stuck_step / fail_step / duration_ms / timeout_ms / saved / total / op / action_id⚠️ | `0x01 / 0x02⚠️ / 0x03 / 0x07 / 0x09 / 0x0A / 0x06 / 0x08 / 0x1C / 0x24 / 0x46 / 0x0B⚠️` | ✅ 覆盖极好，2 个受 STR 影响 |
| Valve | open_ms / limit_ms / cause / state / was / valve_open_ms / gain_after_close_g | `0x13 / 0x14 / 0x16 / 0x44 / 0x47 / 0x48 / 0x49` | ✅ 全部有 |
| Weight | weight_g / delta_g / target_g / start_g / final_g / raw / filtered / offset / samples / saved / cause | `0x15 / 0x10 / 0x0D / 0x0E / 0x0F / 0x18 / 0x19 / 0x1A / 0x1B / 0x1C / 0x16` | ✅ 全部有（覆盖最完整） |
| BLE | temp / humid / batt_v / mac_suffix / fail_count / ble_level / fail_kind / queue_size | `0x3B / 0x3C / 0x3D / 0x3E / 0x3F / 0x51 / 0x52 / 0x53` | ✅ 全部有 |

⇒ **结论：除"字符串不可达"外，现有 89 个 ParamId 完全满足 P2 需求，无需新增。**

#### 两个使用规范建议

1. **`LOG_P_MAX = 0x59` 是否可作普通 ParamId 需在 P2 开工前定**。当前 89 个已分配 ID 全在 `0x00`–`0x58`，`0x59` 是哨兵。建议**明确禁止把 `0x59` 用作业务参数**，把它当"下一个可用号"的起点从 `0x5A` 开始更安全。
2. **状态迁移必须用 `LOG_P_WAS` + `LOG_P_STATE` 成对**（WiFi / Workflow / Valve / Weight 全部适用），否则云端无法还原迁移图。

---

## 5. 高频日志风险分析

### 5.1 容量数学模型

| 环节 | 容量 | 约束 |
|---|---|---|
| RAM 环 | 64 槽 | 所有 Level（含 INFO）共享 |
| 每 loop 消费 | ≤ 8 条 | `LOG_DRAIN_MAX_PER_TASK` |
| Flash 段环 | 496 条（仅 WARN+） | 满了滚动淘汰，未 ACK 的记录 → `drop_unacked` |
| Cloud 队列 | 128 槽 | — |
| Cloud TX | 每 ≥500ms 一批，≤16 条 | 理论上限约 32 条/秒，实际受 MQTT 与 Flash 制约 |

**关键推论**：
- 若某源以 >8 条/loop 的速度持续产生，RAM 环会**单调增长直至满**，此后新记录被丢弃（含 WARN+）。
- **INFO 不落盘，但占 RAM 环 + Cloud 队列 + 上行流量**。"只记 INFO 没关系"是错误认知。
- 64 槽在 setup 阶段尤其危险：setup 里各模块 init 若各发数条 INFO，**可能在第一次 `log_task()` 之前就溢出**。

### 5.2 风险源逐项

| # | 源 | 频率 | 现有保护 | 若直接埋点的后果 | 模块侧方案（不改代码，仅方案） |
|---|---|---|---|---|---|
| H-1 | **BLE ADV 包** | 1s 扫描窗口内可能数十包；队列 16 槽 | MAC 过滤 + 长度过滤；`xQueueSend` 返回值**被忽略**（`MiThermometer.cpp:127`，静默丢包） | 🔴 每包 1 条 ⇒ 瞬间填满 64 槽，WARN+ 全丢 | **禁止在 `onResult()` 埋点**。回调只置 `s_adv_total/s_adv_dropped` 计数；`MiThermometer_task()` 每次扫描窗口最多 1 条 `LOG_BLE_DATA_DECODED`（用"本窗口已上报"标志） |
| H-2 | **BLE 解码失败** | 失败包数 ∝ ADV 数 | 契约已标注"限流"（`log_events.h:318`） | 🔴 同 H-1 | 同上：**每扫描窗口最多 1 条**，其余只累加 `LOG_P_FAIL_COUNT`；`LOG_P_FAIL_KIND` 区分 CRC/长度/类型 |
| H-3 | **HX711 采样** | ≈10Hz 采样，≈0.5s 出一次滤波值 | 5 点窗口滤波 | 🟠 每个滤波值 1 条 ⇒ 2 条/秒 INFO | **只在 `weight_refresh_error_state()` 的 `err != error_state` 边沿埋点**（`weight.cpp:204`）。周期性重量上报走 SystemState，不走日志 |
| H-4 | **HX711 无数据** | 持续失联时每 5s 一次 | 已有 5s 节流（`weight.cpp:323-325`） | 🟠 12 条/分钟 WARN | 由 H-3 的 `error_state` 边沿覆盖；**单独埋点会重复**，禁止 |
| H-5 | **重量跳变** | 5s 内最多 5 次 | `WEIGHT_MAX_JUMP_EVENTS=5` | 🟠 5 条/5s | 禁止在 `weight_record_jump()` 埋点；由 `error_state` 边沿间接体现 |
| H-6 | **MQTT 发布失败** | 离线时每 loop 可能失败（WiFi/MQTT 断连期间） | 无 | 🟠 loop 频率 × 失败次数 | 计数 + **周期聚合**（如每 60s 最多 1 条，带 `LOG_P_DROPPED_TOTAL`）；或仅在"首次失败"和"恢复成功"两条边沿埋点 |
| H-7 | **MQTT 断开事件** | WiFi 抖动时 CONNECTED/DISCONNECTED 反复 | 无 | 🟠 配对洪泛 | 只在 `mqtt_connected` 布尔量**边沿**发（`cloud_manager.cpp:47` 已有该变量），事件重复到达时不重复发 |
| H-8 | **WiFi 重连** | 默认 `reconnect_interval=10s` ⇒ 6 次/分钟 | 状态机 | 🟠 12 条/分钟（START + RECONNECT_TRY） | `LOG_WIFI_RECONNECT_TRY` **限流**：每 5 次尝试记 1 次（带 `LOG_P_RETRY_N`），或 ≥60s 记 1 次。`LOG_WIFI_CONNECT_START` 同理 |
| H-9 | **Workflow 轮询** | 每 loop 遍历所有 step | 无 | 🟠 每个 RUNNING step 每 loop 1 条 | **只在状态迁移埋点**（TIMEOUT / FAILED / FINISHED / ACTION_FAILED），`ACTION_RUNNING` 与 `TRIGGER_RUNNING` 一律不记 |
| H-10 | **Event 风暴丢弃** | ≤5 条/500ms/event type 判定 | `EVENT_STORM_MAX_PER_EVENT=5` / `500ms`（`event_manager.cpp:9-10`） | 🟠 风暴时每 500ms 1 条 | 契约已注明"`LOG_EVT_STORM_DROPPED` 走侧信道计数上报" ⇒ **不为每条丢弃发日志**，改为在批次头侧信道携带计数 |
| H-11 | **阀门防风暴跳过** | 调用频率决定 | 50ms 冷却（`MIN_OPERATION_INTERVAL_MS`） | 🟡 频繁调用时大量 WARN | `LOG_VALVE_RATE_LIMITED` 加**边沿/计数去重**（如同一 `valve_open()` 会话只记 1 次）；注意 `valve_set_gpio()` 在 `open == current_state` 时直接 return ⇒ 天然边沿，可直接埋 |
| H-12 | **setup 阶段 INFO** | 一次性 | 无 | 🟡 各模块 init 各发数条，可能>64 | 控制 setup 期 INFO ≤ 约 20 条（每模块 1 条 init 完成即可），WARN+ 不限 |

### 5.3 通用去重/边沿模式（P2 统一使用，仅方案，不含代码）

| 模式 | 适用 | 做法 |
|---|---|---|
| **状态边沿** | 有 bool 状态量的（error / connected / valid / enabled） | `if (new != old) { old = new; log_emit(...); }` —— 项目里 `weight.cpp:204`、`wifi_module.cpp:206` 已有现成骨架 |
| **计数聚合** | 高频失败（BLE decode / MQTT publish / event drop） | 模块内 `static uint32_t fail_count; static uint32_t last_report_ms;`，周期窗口到才发 1 条并把计数放进 `LOG_P_FAIL_COUNT` |
| **N 次记 1 次** | 周期性重试（WiFi reconnect / NTP retry） | `if (++attempt_n % N == 1) log_emit(... LOG_P_ATTEMPT_N, attempt_n ...)` |
| **首末两条** | 长持续故障（离线、失联） | 只在"进入故障"与"恢复"各记 1 条，中间的持续时长放进 `LOG_P_CONNECTED_MS` / `LOG_P_LAST_OK_AGE_MS` |
| **会话去重** | 同一动作的重复拒绝 | 用 `static bool reported_in_this_session`，动作开始/结束时复位 |

---

## 6. 初始化顺序风险分析

### 6.1 `main.cpp setup()` 实际顺序（实读 `main.cpp:50-173`）

```
 52  Serial.begin(115200) + delay(1000)
 58  LittleFS.begin(true, "/littlefs", 10, "littlefs")   ← ⚠️ log_init() 之前
 64  PSRAM size / free 打印
 67  log contract self check 打印
 80  log_init()                                          ← ✅ LogManager 就绪
 82  json_storage_init()
 91  bin_storage_init()
 95  bin_storage_set_log_callback(bin_log_serial)
100  workflow_storage_init()
103  system_state_init()
104  config_init()
105  event_manager_init()
109  ble_init()
110  wifi_init()          ← 内部立即调用 wifi_start_connect()
114  workflow_init()
118  weight_init()
119  valve_init()
120  computer_reset_init()
121  dispense_guard_init()
122  oled_init() / 123 oled_event_init()
127  time_init()          ← 必须在 oled_init() 之后（RTC 复用 OLED 的 Wire）
128  test_mqtt_init()     ← 测试代码，待删
129  MiThermometerInit()
133  command_manager_init()
136  cloud_init()
142  workflow_load_from_storage() / workflow_load_json_file()
162  capability_registry_init()
170  config_boot_validate()
```

`loop()`（`main.cpp:188-215`）：`system_command_task → wifi_task → event_dispatch → time_task → cloud_task → command_manager_task → config_task → workflow_task → oled_task → weight_task → valve_task → computer_reset_task → MiThermometer_task → test_mqtt_task → **log_task()** → serial_debug_command_process`

### 6.2 逐项风险

| # | 模块 | 函数 | 原因 | 影响 | 建议 |
|---|---|---|---|---|---|
| **R-1** | System | `main.cpp:58` `LittleFS.begin()` | **发生在 `log_init()`（:80）之前**。`log_emit()` 在未就绪时直接 return false（`log_manager.cpp:1443`）⇒ `LOG_SYS_FS_MOUNT_FAILED`（CRITICAL）**必然丢失**，而这一条恰恰是最需要被看见的 | 现场 FS 损坏时，设备"知道 FS 挂了"却无法记录 | 在 :58 失败分支置一个 `static bool s_fs_mount_failed`（**不调 log_emit**）；`log_init()` 之后立即补发。注意此时 Flash 也不可用 ⇒ 该条只能走 Cloud（Level CRITICAL 的 cloud=1），**上线后仍可上报** |
| **R-2** | System | `main.cpp:58` ↓ `log_init()` :1361 `flash_init()` | `log_init()` 内部会调用 `flash_init()` 做段扫描；若 LittleFS 未挂载 ⇒ `s_flash_ready = false`，打印 "[Log] Flash ring unavailable (degraded)" | 🔴 **LogManager 进入永久 degraded**：WARN+ 永远落不了盘，只能堵在 RAM 环（64 槽）里，环满后新记录全丢 | 同 R-1 处置；并在 `log_task()` 观测 `flash_blocked_rounds` 持续增长时**必须降级为"少记但记关键"**（只保留 CRITICAL） |
| **R-3** | System | `main.cpp:64-75` PSRAM 打印 | PSRAM 分配失败点在 `log_init()` 之前 ⇒ `LOG_SYS_PSRAM_ALLOC_FAILED` 丢失 | 内存问题归因困难 | 同 R-1：置标志，`log_init()` 之后补发 |
| **R-4** | 全部 | setup 期间产生的日志 | `log_task()` 第一次执行在**首次 `loop()`**（`main.cpp:213`），而 setup 中所有模块的 init 日志都先堆在 RAM 环（64 槽） | 若 setup 期 INFO 过多（每模块数条），**可能在首次 drain 前就溢出**，WARN+ 被 INFO 挤掉 | 见 H-12：控制 setup 期 INFO ≤ ~20 条；或考虑在 setup 中后段（如 :136 cloud_init 之后）插入一次 `log_task()` 提前排空（**需评审，属代码改动**） |
| **R-5** | Time | `time_init()` :127 | 在 `log_init()` 之后 ✅，但**远晚于前 6 个模块**（config/wifi/workflow/weight/valve 等） | 这些模块的日志 `timestamp = 0` 且 valid 位为 0（`log_manager.cpp:1480`） | **云端必须容忍 `timestamp` 缺失**，以 `uptime_ms` + `boot_seq` 排序；这是已知设计，不是缺陷，但需在云端协议里写明 |
| **R-6** | Cloud | `cloud_init()` :136 | `log_init()` 的 `cloud_init_buffers()` 已成功且注入了 `log_cloud_ack_callback`（`log_manager.cpp:1397`）⇒ 依赖方向 `CloudManager ← LogManager` 单向 | 若 `cloud_init()` 失败，LogManager 侧已就绪但云不可用 ⇒ 记录只落盘、不上传，重启后靠 replay 补发 | 无风险，属预期行为（at-least-once） |
| **R-7** | 全部 | `log_task()` 在 `loop()` **最后**（:213） | 排在 `serial_debug_command_process()`（:214）之前，但**排在所有 `*_task()` 之后** ⇒ 本轮 loop 中前面模块产生的记录会在同轮末尾被消费（✅ 正确）；但 :214 产生的记录要等下一轮 | 延迟一轮，可接受 | 无需处理 |
| **R-8** | WiFi | `wifi_init()` :110 内部立即 `wifi_start_connect()` :151 | 在 `log_init()` 之后 ✅，但此时 `log_task()` 尚未运行过；若连接很快成功，日志仍安全进环 | 无实质风险 | 无 |
| **R-9** | BLE | `MiThermometerInit()` :129 | 在 `log_init()` 之后 ✅ | 无 | 无 |

### 6.3 结论

**不存在"模块先于 LogManager 启动并产生日志"的问题** —— `log_init()` 位于 `main.cpp:80`，早于所有业务模块 init。这一点比 0914 审计时的假设乐观。

**真正的问题只有三个**：
1. **R-1/R-2**：LittleFS 挂载失败发生在 `log_init()` 之前，且会同时让 LogManager 永久 degraded ⇒ 最严重的故障恰恰记不下来。
2. **R-4**：setup 期 INFO 可能挤爆 64 槽 RAM 环。
3. **R-5**：前 6 个模块的日志无有效时间戳（云端需容忍）。

---

## 7. 风险列表（汇总）

| ID | 类别 | 风险 | 严重度 | 处置 |
|---|---|---|---|---|
| **R-1** | 初始化 | LittleFS 挂载失败发生在 `log_init()` 之前，CRITICAL 日志必然丢失 | 🔴 高 | 置标志，`log_init()` 后补发（走 Cloud） |
| **R-2** | 初始化 | FS 不可用 ⇒ LogManager 永久 degraded，WARN+ 堵死 RAM 环 | 🔴 高 | 同 R-1 + 观测 `flash_blocked_rounds`，必要时降级为只记 CRITICAL |
| **R-3** | 初始化 | PSRAM 分配失败丢失 | 🟡 中 | 同 R-1 |
| **R-4** | 容量 | setup 期 INFO 挤爆 64 槽 RAM 环 | 🟡 中 | 控制 setup 期 INFO ≤ ~20 条 |
| **R-5** | 时间 | 前 6 个模块日志无有效 timestamp | 🟡 中 | 云端以 `uptime_ms` + `boot_seq` 排序（需写入云端协议文档） |
| **R-6** | 接入 | 字符串类 ParamId 不可达（`LOG_PTYPE_STR/I8/U16`），影响 7 个 ParamId | 🔴 高 | 见 §4.2，需人工拍板方案 A/B |
| **R-7** | 接入 | Workflow 与 Critical Op 强耦合，日志接入可能引入新的 return 路径导致 release 泄漏 | 🔴 高 | 项目铁律：release 不能放在会中途 return 的函数里；接入后必须回归 `wf` 相关串口用例 |
| **R-8** | 接入 | BLE 回调上下文误用 `log_emit()` | 🔴 高 | 硬禁令（见 §5.2 H-1） |
| **R-9** | 容量 | INFO 洪泛挤占 RAM 环与云队列 | 🟡 中 | 边沿/限流/聚合（§5.3） |
| **R-10** | 契约 | `LOG_P_MAX = 0x59` 是否可作普通 ParamId 未定 | 🟡 中 | P2 开工前定：建议禁止，新号从 `0x5A` 起 |
| **R-11** | 覆盖 | 67 条 Storage E/W 静默（json 37 + file 30，回调未注册） | 🟡 中 | P2 第一步注册 bridge（零侵入） |
| **R-12** | 覆盖 | `LOG_DISPENSE_*` 四个 EventId 无宿主（Dispense 模块不存在） | 🟢 低 | P2 定义宿主或标记为未实现 |
| **R-13** | 覆盖 | `LOG_WIFI_PROVISION_*` 两个 EventId 无实现 | 🟢 低 | P2 可不实现 |
| **R-14** | 覆盖 | `LOG_VALVE_FORCE_CLOSE_FAILED` 无处可埋（`valve_force_close()` 恒返回 true） | 🟢 低 | 先补失败检测，或标记未实现 |
| **R-15** | 覆盖 | `LOG_VALVE_OVERFLOW_RISK`（`log_events.h:271` 标注"需新增检测"）无实现 | 🟢 低 | 同上 |
| **R-16** | 设备验证 | **P1.5 设备侧测试仍 BLOCKED**（开发板未连接） | 🟡 中 | P2 接入前建议先跑一轮 `test/log_fix_tests.txt`（F0–F8 / 186 断言），确认 P1.5 上板无回归 |

---

## 8. 建议实施顺序

### 阶段 A：零侵入冒烟（不改业务代码）

1. 在 `main.cpp` 注册 `json_storage_set_log_callback()` / `file_storage_set_log_callback()` 的 bridge（串口 + LogManager 双路）⇒ 立即获得 **67 条静默 E/W**。
2. 把 `bin_storage` / `command_manager` 已注册的串口 bridge 改为双路。
3. 跑一次上板，观察：`logt stats` 的 `drop_ring` / `drop_overflow` / `flash_blocked_rounds` 是否正常；确认 Storage 错误可见。

### 阶段 B：基线模块（低风险）

4. **System/Boot**：11 个埋点（含 R-1 的补发机制）。
5. **Time/RTC**：10 个埋点（顺带定位 `rtc_present=false`）。
6. **WiFi**：6 个埋点 + RECONNECT_TRY 限流。

### 阶段 C：语义模块（中风险）

7. **Config**：12 个埋点（映射已冻结的 10 个 EventId）。
8. **Cloud/MQTT**：9 个埋点 + publish fail 聚合。
9. **Weight**：8 个埋点（严格边沿）。
10. **Valve**：8 个埋点（安全相关，CRITICAL/IMM）。

### 阶段 D：高风险模块（需评审后动手）

11. **Workflow**：13 个埋点，**接入前必须评审 `workflow_terminate()` 的 release 收口路径**。
12. **Dispense Guard**：3 个埋点（先定义宿主）。
13. **BLE**：6 个埋点（严格遵守回调禁令 + 窗口聚合）。

### 阶段 E：收尾

14. **Command / Event / OLED / Registry**：8 个埋点。
15. 全量回归：跑 `test/log_fix_tests.txt`（F0–F8 / 186 断言）+ `wf` 相关串口用例 + Critical Op 回归。

### 每个阶段的提交规范

- 一次 commit **只含一个模块**。
- **禁止 `git add -A` / `.`**（`.workbuddy/memory/` 与 `data/config/` 下均有 force-add 过的 tracked 文件）。
- 新增 EventId 必须**单独 commit + 单独评审**，不与接入混在一起。

---

## 9. 需要人工拍板的事项（阻塞 P2 开工）

| # | 事项 | 选项 | 影响 |
|---|---|---|---|
| 1 | 字符串参数方案 | A 哈希/枚举化（推荐，不改 API） / B 扩展 `LogParamIn` 加 blob | 影响 7 个 ParamId、4+ 个模块 |
| 2 | `LOG_P_MAX = 0x59` 能否作普通 ParamId | 禁止（新号从 0x5A 起） / 允许 | 影响后续新增空间 |
| 3 | 是否接受 §4.1 的 9 个建议新增 EventId | 全部 / 部分 / 暂不新增（先用 `LOG_P_CAUSE` 区分） | 建议：**先不新增**，用 CAUSE 区分 |
| 4 | 是否在接入前先补 P1.5 上板回归（R-16） | 是 / 否（继续 BLOCKED） | 影响 P2 结果的置信度 |
| 5 | 是否接受 §3.1 的接入顺序（两处偏差） | 接受 / 调整 | 影响 Storage↔Config、Weight↔Valve 的先后 |
| 6 | setup 期是否插入一次 `log_task()`（R-4 的备选方案） | 插 / 不插（改为控制 INFO 条数） | 涉及 `main.cpp` 改动，需评审 |

---

## 10. 本报告的边界

- ✅ 已完成：模块矩阵、EventId 覆盖分析、ParamId 分析、埋点位置（文件/函数/事件/Level/原因）、高频风险、初始化顺序风险、接入顺序、实施计划
- ❌ 未做（按约束）：
  - 未修改任何 `src/` 生产代码
  - 未新增 EventId / ParamId
  - 未修改 `log_events.h` / LogManager API
  - 未执行 `git add` / `commit`
  - 未进行任何设备侧验证（开发板未连接）

---

*报告结束 —— 等待人工审核，未进入代码修改阶段*
