# LogManager P2 开发进度

> 用途：**会话中断后的续接基线**。每次完成一个模块接入必须更新本文件。
> 上游文档：`docs/LogManager-Integration-Guide.md`（接口规范）· `docs/P2_Log_Integration_Matrix.md`（模块矩阵）· `log模块历史/LogManager-P2接入准备审查0918.md`（前置审查）
> 更新规则：只记「已完成 / 当前基线 / 下一步 / 未决事项」，不记过程细节。

---

## Completed

| 阶段 | 内容 | commit | 状态 |
|---|---|---|---|
| P1.1–P1.5 | LogManager 核心（RAM 环 / Flash 段环 / Cloud / ACK·Retry / Replay / H2 / D1） | `5816b32`、`c29d9d5`、`d1ccdbe`、`65b4523` | ✅ 已提交 |
| 仓库整理 | 凭据文件取消跟踪 + 0917 报告入库 | `1d0b47c` | ✅ 已提交 |
| P2 前置 | 接入矩阵 + P2 接入准备审查（只出文档） | 见下方「待入库」 | ✅ 文档已生成 |
| **阶段 0** | 工作区确认（HEAD、`src/`/`test/` 无未提交改动） | — | ✅ |
| **阶段 1** | **P1.5 上板验证**（187 断言 → 167 OK / 20 MISS，全部 MISS 归因为用例预期缺陷） | `6a82fe1`（用例 op 名修正）、`789f53b`（验证报告） | ✅ 已提交 |
| **阶段 2** | **P2-A Storage bridge 接入**（json_storage 37 + file_storage 30 = 67 处 E/W 由静默变为可观测） | `930ed45` | ✅ 已提交 |
| **阶段 3** | 建立本进度文档 | `6485e25` | ✅ |
| **测试资产修正** | 按首轮归因修正 14 处预期值 + 回归器加 `<BOOT>` 宏 + 重录基线 | `test(log): fix P1.5 case expectations and re-baseline` | ✅ 已提交 |
| **收尾 · BT-1** | 云端 `log_ack`：**Worker 不在本仓库** ⇒ 仅记录接口要求，未改设备协议 | — | 📝 已记录 |
| **收尾 · holes=2** | 代码审查结论：**设计行为**（两个独立来源、相邻才合并）⇒ 更新用例而不改实现 | 见上方提交 | ✅ |
| **阶段 2-B** | **P2-B ConfigManager 接入**：`config_log_bridge` 桥接（关键词分类 + 10 s 去重 + SKIP 防重复）+ 2 个显式语义埋点（`CHANGE_APPLIED` / `SAVE_OK`）+ `cfg_hash32()` | `feat(log): integrate config manager logging` | ✅ 已提交 |
| **BT-9 修复** | **ACK 水位越过未确认补发记录**（at-least-once 破坏）：拆成 `s_cloud_acked_seq`（真实最大，仅观测）+ `s_cloud_gc_seq`（连续可回收，全部判定）+ `s_cloud_gc_floor`（backlog 下界钳制）；新增 `log_ack_gc_watermark()` 纯函数；补发/段回收/淘汰记账/**ACK 重复检测**全部改用连续水位 | `fix(log): prevent ack watermark bypass replay records` | ✅ 已提交 |
| **BT-9 回归** | 全量重跑 **195/195 = 100%，0 MISS**（首轮 167/187 → R1 187/189 → 195/195；**此后 P2-D 夹具加固 ⇒ 现基线 196/196**）；F2-B 两条原 KNOWN-FAIL 断言（`replay=8` / `qused=8`）**由 FAIL 转真实 PASS** | 同上 | ✅ |
| **阶段 2-C** | **P2-C WiFi 接入**（`src/wifi_module.cpp`，唯一改动文件）：5 个埋点（`CONNECT_START` / `CONNECTED` / `CONNECT_TIMEOUT` / `LOST` / `RECONNECT_TRY`）+ 失败循环节流（N 次记 1 次 **AND** ≥60s）+ `wifi_ssid_hash32()` | `feat(log): integrate wifi module logging` | ✅ 已提交 |
| **阶段 2-D** | **P2-D Cloud/MQTT 接入**（`src/cloud_manager.cpp`，唯一改动文件，**纯增量 +233/−0**）：5 个埋点（`CONNECTED` / `DISCONNECTED` / `SLEEP_ENTER` / `PUBLISH_FAIL` 聚合 / `CMD_EXEC_FAILED`）+ 发布失败计数聚合（60s 窗口） | `feat(log): integrate cloud manager logging` | ✅ 已提交 |
| **阶段 2-E** | **P2-E TimeManager 接入**（`src/time_manager.cpp`，唯一生产代码改动文件，**纯增量 +210/−0**）：9 个埋点覆盖 `NTP_OK` / `VALID_ENTER` / `INVALID_ENTER` / `RTC_PROBE`（**三分支**）/ `RTC_BOOT_RESTORE` / `RTC_CALIBRATED` / `RTC_WRITE_FAILED` / `RTC_VL_FLAG` / `RTC_BCD_INVALID`；RTC 异常**边沿锁**去重；`time_source_code()` 枚举化 | `feat(log): integrate time manager logging` | ✅ 已提交 |
| **阶段 2-E · 夹具** | P2-E 引发的 F2-B 4 条真失败：根因＝`cloud_collect_batch()` **在 `boot_seq` 变化处截断批次**，而每个 Boot 多 1 条 Flash 记录（`RTC_PROBE` 是 WARN）⇒ 积压跨 2 个 boot_seq ⇒ 单批永远排不空 ⇒ `qused=0`/`rarmed=0` 不可能达成。**只改夹具**：回归器新增**内联正则期望**（`replay=/[1-9][0-9]*/`），F2-B 判据改为"非零"+结构性事实，**断言总数 196 条不变** | 同上 | ✅ |
| **阶段 2-E · 发现** | ① `LOG_TIME_NTP_FAIL`(0x0802) **确认无宿主**（SDK 只提供成功通知、状态枚举无失败态）⇒ 记为**未实现**，不新增看门狗；② `rtc_init()` 内的"配置禁用"分支是**死代码**（`rtc_enabled && rtc_init()` 短路求值）⇒ 埋点移到 `time_init()` 可达分支；③ `VALID_ENTER.SOURCE` 存在**实测竞态**（边沿可能早于来源确认，实测两种顺序都出现过） | 同上 | ✅ 已处理并记录 |
| **阶段 2-F · 审查** | **Workflow 接入前审查**（只审查、未改生产代码）：Critical Op 生命周期审计（3 acquire 全配对 / 两个释放函数幂等 / `workflow_terminate()` **零提前 return** / 6 个调用点全覆盖 / **无可达绕过路径、无永久锁死风险**）、生命周期状态流转、6 个 terminate 退出路径、矩阵与代码 **4 项不一致**、14 个推荐埋点位置（含频率与限流策略） | `docs(log): review workflow logging integration plan` | ✅ 已提交 |
| **阶段 2-F · 发现 WF-1** | **保存失败后重试风暴**：保存失败的**两条路径都不重置** `workflow_save_since_ms` ⇒ `workflow_delayed_save_poll()` 的 5 分钟窗口**一旦过期就永远过期** ⇒ Dirty 未清期间**每个 loop** 跑一次完整保存事务（8.6 KB `def_buf` + LittleFS 落盘尝试 + 串口 2 行）。**已实测**：151 s 内 **5828 次**事务（≈**38.6 次/秒**）、串口 ≈77 行/秒。**与 Critical Op 无关**，按规则**只报告不修复**；P2-F 埋点对它免疫（三个边沿锁）。详见 `log模块历史/LogManager-P2F-Workflow接入审查0918.md` §3 | — | ⏳ **未修**（待单独评审） |
| **阶段 2-F** | **P2-F Workflow 接入**（`src/workflow.cpp`，唯一生产代码改动文件，**纯增量 +261/−0，0 删除行**）：12 个冻结 EventId / **17 个发射点**全落地（**Workflow 段无"无宿主 EventId"**）；保存类事件用**三个边沿锁**（`wf_save_fail_reported[]` 逐 slot / `wf_save_partial_reported` 整事务 / `wf_alloc_fail_reported`）+ `wf_save_partial_retries` 计数；主键 `LOG_P_SLOT`（非哈希）；`OP`/`CAUSE` 枚举化 | `feat(log): integrate workflow manager logging` | ✅ 已提交 |
| **阶段 2-F · 发现 WF-2** | **保存成功路径不记账是"正确设计"的确认**：Workflow 段**没有** `SAVE_OK` ID，且不加成功记录 ⇒ 自动后台保存（每 loop 可能）**零噪声**；唯一的成功侧记录是 `0x040C`，语义是"**从 partial 恢复**"这一**迁移**而非"成功" | 同上 | ✅ 已记录 |

---

## Current baseline

```
HEAD:           见 `git log --oneline -1`（每次提交后更新本行）
分支:           wb
P1.5 设备侧:    ✅ 不再 BLOCKED（A–E 全部可跑段落已完成）
回归基线:       ✅ 196/196 = 100%，0 MISS（test/log_fix_tests.txt，P2-E 最终固件 + 恢复配置后全量重跑）
                   首轮 167/187 → R1 187/189（2 MISS=BT-9）→ BT-9 修复后 195/195
                   → P2-D 夹具加固 196/196 → P2-E 夹具加固（正则期望）后仍 **196/196**
                   （断言总数保持不变：F2-B 去掉 3 条与积压历史相关的绝对计数，
                     换成 3 条结构性判据 + 补 1 条 `giveup=0`）
合约测试:       ✅ 4/4 ALL PASS（含新增第 ⑧ 组 23 条 BT-9 断言，其中 3 条负向探针）
```

### 已接入 LogManager 的模块

| 模块 | 接入方式 | 事件覆盖 | 提交 |
|---|---|---|---|
| **Storage（json_storage / file_storage）** | `main.cpp` 回调桥接（`json_storage_log_bridge` / `file_storage_log_bridge`） | `LOG_STG_FS_UNAVAILABLE` / `ATOMIC_WRITE_FAILED` / `CRC_FAILED` / `TXN_RECOVERED` / `WRITE_VERIFY_FAILED` / `READ_FAILED` | P2-A |
| **Config（ConfigManager）** | `main.cpp` 桥接（`config_log_bridge`，**双路串口**）+ `config_manager.cpp` 内 2 处显式埋点 | `LOG_CFG_LOAD_DONE` / `MODULE_LOAD_FAILED` / `RECOVERED_FROM_BACKUP` / `COMMIT_FAILED_ROLLBACK` / `VERSION_REBUILT` / `WRITE_REJECTED` / `RESTART_TIMEOUT` + **`CHANGE_APPLIED`** / **`SAVE_OK`** | P2-B |
| **WiFi（`wifi_module.cpp`）** | 无回调接口 ⇒ **直接显式埋点**（5 处，全在既有分支内） | `LOG_WIFI_CONNECT_START` / `CONNECTED` / `CONNECT_TIMEOUT` / `LOST` / `RECONNECT_TRY` | P2-C |
| **Cloud（`cloud_manager.cpp`）** | 复用既有"回调置标志 → `cloud_task()` 消费"机制 ⇒ 埋点挂在**标志被消费处**（loop 上下文，边沿由单槽标志保证） | `LOG_MQTT_CONNECTED` / `DISCONNECTED` / `SLEEP_ENTER` / `PUBLISH_FAIL`（聚合）/ `CMD_EXEC_FAILED` | P2-D |
| **Time（`time_manager.cpp`）** | 无回调接口 ⇒ **直接显式埋点**（9 个事件 / 11 个埋点，全在既有分支内）；RTC 异常用**边沿锁**；`SOURCE` 走**枚举化**（0=INVALID 1=RTC 2=SNTP 3=MANUAL） | `LOG_TIME_NTP_OK` / `VALID_ENTER` / `INVALID_ENTER` / `RTC_PROBE`(**INFO·WARN 三分支**) / `RTC_BOOT_RESTORE` / `RTC_CALIBRATED` / `RTC_WRITE_FAILED` / `RTC_VL_FLAG` / `RTC_BCD_INVALID` | P2-E |
| **Workflow（`workflow.cpp`）** | 无回调接口 ⇒ **直接显式埋点**（12 个事件 / **17 个发射点**，全在既有分支内）；**纯增量 `+261/−0`（0 删除行）**；保存类事件用**三个边沿锁**（逐 slot / 整事务 / 分配失败）+ `RETRY_N` 计数；主键用 **`LOG_P_SLOT`**（不用 id 哈希） | `LOG_WF_START` / `FINISHED` / `TIMEOUT` / `FAILED`(4 处) / `ACTION_FAILED`(2 处) / `SAVE_FAILED` / `SAVE_PARTIAL` / **`SAVE_PARTIAL_RETRY_OK`** / `CRUD`(3 处) / `MIGRATED` / `TEMP_ACTION_TIMEOUT` / `RUNTIME_ALLOC_FAILED` | P2-F |
| 其余 5 个模块 | **未接入** | — | — |

未接入清单（按约定顺序）：**Weight → Valve → Dispense → BLE → Command/Event/OLED/Registry**

（未实现的冻结 EventId，均**无宿主** ⇒ 不埋点、编号保留：）

| EventId | 原因 |
|---|---|
| `LOG_CFG_FACTORY_RESET`(0x0208) | 代码中**不存在** factory reset 函数 |
| `LOG_CLOUD_FRAG_FAIL`(0x0706) | `cloud_publish_fragmented()` 全仓库无调用者（死代码） |
| **`LOG_TIME_NTP_FAIL`(0x0802)** | **SDK 只提供成功通知（`sntp_sync_time_cb_t`），状态枚举无失败态（`RESET`/`COMPLETED`/`IN_PROGRESS`）⇒ 设备无法观测 NTP 失败。要产出它必须新增 SNTP 超时看门狗（＝扩展状态机），超出 P2「只加观测」范围。详见 Guide §14.3** |

### P2-C 验证记录（2026-09-17，COM8，AP `wqs1`）

| EventId | 验证方式 | 结论 |
|---|---|---|
| `LOG_WIFI_CONNECT_START` | 真机开机 + MQTT 解码 | ✅ 通过（解码到 `SSID_HASH=0x7c7a4fc9 ATTEMPT_N=1 WAS=0(IDLE) STATE=1(CONNECTING)`） |
| `LOG_WIFI_CONNECTED` | 真机开机 + MQTT 解码 | ✅ 通过（解码到 `CONNECT_MS=1807 RSSI=-56 WAS=1 STATE=2`；`flash=0` 证明 INFO 不落盘） |
| `LOG_WIFI_CONNECT_TIMEOUT` | 真机（SSID 指向不存在的 AP）+ 补发回传解码 | ✅ 通过（解码到 `TIMEOUT_MS=30000 ATTEMPT_N=1 WAS=1 STATE=3`，WARN 落 Flash） |
| `LOG_WIFI_RECONNECT_TRY` | 同上 | ✅ 通过（解码到 `RETRY_N=1 ATTEMPT_N=1 WAS=3 STATE=1`；实测 4 轮循环只记 1 轮 ⇒ 节流生效） |
| **`LOG_WIFI_LOST`** | **见下方专项说明** | ⚠️ **部分通过**（代码路径审查通过 / 参数语义已确认 / **真机断 AP 验证未完成**） |

#### ⚠️ `LOG_WIFI_LOST` 专项说明

| 项 | 状态 |
|---|---|
| **代码路径审查** | ✅ **通过**。埋点位于 `wifi_task()` 的 `case WIFI_CONNECTED` 内、紧邻 `WiFi.status() != WL_CONNECTED` 判定之后，**未改变任何分支与 return 路径**；与既有 `Serial.println("WiFi lost")` / `event_push(EVENT_WIFI_DISCONNECTED)` 同处一个边沿块，语义一致 |
| **参数语义确认** | ✅ **已确认**：`CONNECTED_MS` = `millis() - wifi_connected_since`；`RSSI` = `state_get_int(STATE_WIFI_RSSI)`（**最后一次采样值**，因断连后 `WiFi.RSSI()` 只会返回 −100）；`CAUSE` = 断连瞬间 `WiFi.status()`（`WL_CONNECTION_LOST` / `WL_DISCONNECTED` / `WL_CONNECT_FAILED`…）；`WAS=2(CONNECTED)` / `STATE=3(DISCONNECTED)` |
| **真机断 AP 验证** | ❌ **未完成 —— 列为后续测试项** |
| 未完成原因 | 当前**没有任何钩子**可以强制断连：`WiFi.disconnect()` 在代码中无调用点；改 SSID 只影响下一次 `WiFi.begin()`，**不会**让已建立的连接掉线。本环境唯一办法是**物理关闭 AP** |
| 后续测试项 | ①（推荐）后续加一个测试钩子（如 `wifi force-lost`）；或 ② 人工关闭 AP 一次后复测。验收标准：MQTT 侧能解码到一条 level=WARN、带 `CONNECTED_MS`/`RSSI`/`CAUSE`/`WAS=2`/`STATE=3` 的记录 |

> 说明：本次**未**为 `LOST` 修改任何生产代码；`BT-10` / `BT-11` 按约定**不处理**，
> 仅在「未决 / 阻塞事项」中记录。

### P2-D 验证记录（2026-09-18，COM8，broker `emqxsl.cn:8883`）

**验证手段（可复用，已落 `.pio/p15run/`，不入库）**：
- `log_decode.py`：把 CBOR 批次解到**记录级**（`event_id` 在偏移 12 / u16 LE；参数从偏移 28 起，
  每项 `{id,type,u32le}` 6 B）⇒ 能直接读到 `event_id` 与每个 `ParamId` 的**真值**。
- `p2d_probe.py`：订阅 `guo_feeder/log` + **用设备自己的 `client_id` 再连一次 broker**（EMQX
  **session takeover** 会把设备踢下线，产生**真实的** `MQTT_EVENT_DISCONNECTED`）
  + 向 `guo_feeder/down` 发**不存在**的 action。**不改固件、不加钩子、不改协议。**

| EventId | 验证方式 | 结论 |
|---|---|---|
| `LOG_MQTT_CONNECTED` | 真机首连 + 重连（各 1 条） | ✅ 通过（`OUTBOX=450/710 WAS=0 STATE=1`；**一次连接一条**） |
| `LOG_MQTT_DISCONNECTED` | session takeover 真实踢下线 | ✅ 通过（`OUTBOX=0 RETRY_N=1 WAS=1 STATE=0`；设备串口同步出现 `MQTT disconnected outbox=0`） |
| `LOG_MQTT_CMD_EXEC_FAILED` | 下发 2 条不存在的 action | ✅ 通过（2 条命令 = 2 条记录，`CMD_ID` 为哈希；未误触发其他事件） |
| `LOG_MQTT_SLEEP_ENTER` | 代码路径审查 | ⚠️ **审查通过，真机未触发** —— 需"连续 30 次断连且期间一次都没连上"（每次成功连接会把 `mqtt_retry_count` 清零，配置 `retry_max=30`）。复测需临时改坏 MQTT `password` 或 EMQX 侧禁 client_id |
| `LOG_MQTT_PUBLISH_FAIL` | 代码路径审查 | ⚠️ **审查通过，真机未触发** —— 上层调用者都先自查 `mqtt_connected` ⇒ 离线时"根本不尝试发"；只有**竞态窗口**（刚断连但标志未更新）或 `enqueue` 返回 −1 才命中，时序敏感、不适合稳定回归 |

> 两项未触发的原因与复测方法详见 `docs/LogManager-Integration-Guide.md` §13.8。
> `LOG_CLOUD_FRAG_FAIL`(0x0706) **未埋点**：`cloud_publish_fragmented()` 全仓库无调用者（死代码），埋点不可达。
> 回归：埋点后全量重跑 **196/196 = 100%，0 MISS**（含夹具加固，见下）。

### P2-E 验证记录（2026-09-18，COM8，broker `guo_feeder/log`）

**验证手段**：`.pio/p15run/p2e_probe.py`（先订阅再烧录/复位 ⇒ 才能抓到 Boot 头几秒的记录）
+ `log_decode.py` 解到记录级。**注意本阶段同时修好了 `log_decode.py` 的批次头字段错位**：
CBOR 批次的 `array(12)` 顺序是
`[fmt, dict_ver, **boot_seq**, seq_from, seq_to, count, drop_ring, drop_overflow, drop_unacked, self_degraded, flags, [records]]`
（原实现漏了 `boot_seq`，把 `v[2]` 当 `seq_from`，导致 `boot=` 打印成 seq 值）。

| EventId | 验证方式 | 结论 |
|---|---|---|
| `LOG_TIME_NTP_OK` | 真机开机 + MQTT 解码 | ✅ 通过（`UNIX=1789666817 DURATION_MS=454 ATTEMPT_N=1`；**挂在 settled 确认点**，非 callback） |
| `LOG_TIME_VALID_ENTER` | 真机开机 | ✅ 通过（`UNIX=<同步时刻> SOURCE=2(SNTP)`） |
| `LOG_TIME_RTC_PROBE`（**失败**） | 真机开机（板上无 RTC 芯片） | ✅ 通过（**WARN** `ADDR=81 STATE=1 ERR_CODE=2`）—— **首次用日志解释了 `rtc_present=false`** |
| `LOG_TIME_RTC_PROBE`（**配置禁用**） | `config_set rtc.enable=false` + `config_save` ⇒ 重启 | ✅ 通过（**INFO** `STATE=0`，无 ADDR/ERR_CODE）。⚠️ **修复后才可达**，见下 |
| `LOG_TIME_RTC_PROBE`（**探测成功**） | —— | ❌ 需真实 PCF8563T 芯片 |
| `LOG_TIME_RTC_BOOT_RESTORE` | —— | ❌ 需 RTC 芯片 |
| `LOG_TIME_RTC_CALIBRATED` | —— | ❌ 需 RTC 芯片 |
| `LOG_TIME_RTC_WRITE_FAILED` | —— | ❌ 需 RTC 芯片 |
| `LOG_TIME_RTC_VL_FLAG` | —— | ❌ 需 RTC 芯片 + **电池耗尽** |
| `LOG_TIME_RTC_BCD_INVALID` | —— | ❌ 需 RTC 芯片 |
| `LOG_TIME_INVALID_ENTER` | 代码路径审查 | ⚠️ **审查通过，运行时不可触发** —— 需"有效→无效"迁移（把时钟设到 2026-07-01 之前），而设备**没有** `set_time` 通道（`time_set_manual*` 无调用者）。同函数的**有效边沿已上板验证**（互为镜像分支，共用同一 `last_reported` 门控） |
| **`LOG_TIME_NTP_FAIL`** | —— | ⛔ **冻结 EventId / 无宿主 / 未实现**（见 `Current baseline` 的说明表与 Guide §14.3） |

**★ P2-E 期间发现并修复的三件事**（详见 Guide §14.4 / §14.6 / §14.9）：

1. **`rtc_init()` 里的"配置禁用"分支是死代码**：`time_init()` 是它唯一调用者，而调用条件是
   `rtc_enabled && rtc_init()`（**短路求值**）⇒ `!rtc_enabled` 时它根本不会被调用。
   埋点曾写在那里，**实测该记录完全不出现**。已移到 `time_init()` 中可达的 `else` 分支。
   ⇒ **教训：分支存在 ≠ 分支可达，埋点必须验证可达性。**
2. **`VALID_ENTER.SOURCE` 存在实测竞态**：SMOOTH 同步下系统时间会在 SNTP **确认之前**
   就越过有效阈值 ⇒ 两次实测拿到两种顺序（`NTP_OK → VALID_ENTER(SOURCE=2)` 与
   `VALID_ENTER(SOURCE=0) → NTP_OK`）。`SOURCE=0` 不是错值，但**来源应以
   `NTP_OK` / `RTC_BOOT_RESTORE` 为准**。
3. **夹具根因比 P2-D 那次更根本**（见下节）。

**现场状态**：`rtc.enable` 已恢复 `true`（`config_query` 确认，且启动日志回到 WARN 版）。

### P2-F 验证记录（2026-09-18，COM8；固件 `.pio/build/p2f2`）

**固件核对**：`firmware.bin` mtime **晚于** `src/workflow.cpp`，且大小比含 `0x040C` 之前的版本 +48 B ⇒ 烧录的就是最终代码。

```text
boot=6  seq=1797 INFO LOG_WF_CRUD            SLOT=2 OP=3(delete) VARIANT=2
        seq=1802 INFO LOG_WF_CRUD            SLOT=3 OP=1(create) VARIANT=1
        seq=1803 INFO LOG_WF_CRUD            SLOT=3 OP=3(delete) VARIANT=2
        seq=1804 INFO LOG_WF_START           SLOT=0 STEPS_DONE=2 TIMEOUT_MS=10000
        seq=1805 WARN LOG_WF_ACTION_FAILED   SLOT=0 FAIL_STEP=0 CAUSE=4

boot=9  seq=2568 INFO LOG_WF_CRUD            SLOT=0 OP=2(update) VARIANT=1
        seq=2569 WARN LOG_WF_SAVE_FAILED     SLOT=0 ERR_CODE=7    ← WRITE_FAILED（注入）
        seq=2570 WARN LOG_WF_SAVE_PARTIAL    SAVED=0 TOTAL=1
        seq=2571 INFO LOG_WF_SAVE_PARTIAL_RETRY_OK RETRY_N=1      ← ★ 0x040C 落地
```

**`emit` 轨迹逐条对齐**：`meta` +1（CRUD）→ 首次 `save` 失败 +2（SAVE_FAILED + SAVE_PARTIAL）→ 恢复 `save` 成功 +1（0x040C）→ 第三次 `save`（已无 Dirty）**+0**（幂等 no-op 零噪声）。
**`flash` 只随 WARN 增长**（`1 → 3`）⇒ INFO 不落 Flash 的 Level Policy 在 Workflow 段同样成立。

| EventId | 验证方式 | 结论 |
|---|---|---|
| `LOG_WF_CRUD`（create/update/delete 三向） | 记录级解码 | ✅ 上板通过（OP 枚举值正确） |
| `LOG_WF_START` | 记录级解码 | ✅ 上板通过 |
| `LOG_WF_ACTION_FAILED` | 记录级解码 | ✅ 上板通过（`CAUSE=4`） |
| `LOG_WF_SAVE_FAILED` / `SAVE_PARTIAL` | 注入 `wfst failwf 0` | ✅ 上板通过（各 1 条，边沿锁生效） |
| **`LOG_WF_SAVE_PARTIAL_RETRY_OK`(0x040C)** | 失败后再 `wfc save` 成功 | ✅ 上板通过（`RETRY_N=1`） |
| `LOG_WF_MIGRATED` | 记录级解码 | ✅ 上板通过（`wfc migrate`） |
| `LOG_WF_FINISHED` / `TIMEOUT` / `FAILED` / `TEMP_ACTION_TIMEOUT` / `RUNTIME_ALLOC_FAILED` | 代码路径审查 | ⚠️ **未做真机触发**（原因与复测方法见 Guide §15.8；其中 `FINISHED`/`TIMEOUT` 需要"无害 Action 的 Workflow"，板上现存工作流会**真的驱动执行机构** ⇒ 有意不跑） |

**风暴下的边沿锁实测**（WF-1 场景，单会话探针 `.pio/p15run/storm_probe.py`；**重开串口会复位清掉 RAM Dirty，必须单会话**）：

```text
第一次失败（永久 INVALID_ARGUMENT）：emit +2 = SAVE_FAILED + SAVE_PARTIAL（锁置位）
T1（风暴已跑 36s）: emit=12 flash=3
T2（再跑 48s）    : emit=12 flash=3   ← 84s 内 3399 次失败事务，新增记录 0 条
```
⇒ **边沿锁把 3399 条潜在 WARN 压成 0 条**（≈3400× 削减）。若不做边沿锁，同等时长会写约 3400×128 B 到 Flash。

**回归**：全量 **196/196 = 100%，0 MISS**（P2-F 改动未触碰任何断言）。

**设备状态复原**：测试期间新增/删除的都是控制台临时 Workflow；结束时 `wfc del` 已清掉无效定义，`dirty=0`。
⚠️ **遗留**：`wfc create`（控制台新建的**不完整定义**）会产生**永久 `INVALID_ARGUMENT`** ⇒ 用它做失败注入很方便，但**测完必须删掉**，否则会持续触发 WF-1 风暴（P2-F 期间已遇到一次）。

### ★ 回归夹具加固（P2-D 期间发现，**P2-E 又加深了一层**）

**现象**：P2-D 埋点后 B 段稳定 4 条 MISS（`replay=8` / `qused=8` / `qused=0` / `rarmed=0`）。

**根因**：F2-B 的 BT-9 场景靠"注入 live 批次 ACK ⇒ 队列排空 ⇒ 补发 sweep 推进"。
但注入的 ACK **只覆盖当时的在途批次**；P2-D 新增的 `LOG_MQTT_CONNECTED` 比 WiFi 记录
**晚约 4s 落地**（要等 TLS 握手）⇒ 队列里**必然**多出一条尾巴 ⇒ ACK 后 `used != 0`
⇒ 补发被挡住（**不是没补发，是观察不到**）。此前该尾巴"通常不存在"，故用例看起来是通的。

**加固做法（把时序依赖变成顺序无关）**：在注入 ACK 之前显式 `logt cpush 1` 造一条
确定性的 live 尾巴，再在 ACK 之后补一次 `logt ackauto 0` 排空它。这样
"Boot 期记录落在哪一批"不再影响结果，两种情形收敛到同一路径。
**断言强度未削弱**（`replay=8` 仍为精确值），只是把夹具变成确定性的。

**⚠️ 给后续模块的规矩**：

1. **任何新模块的埋点都会往同一条日志流里加记录**，`log_fix_tests.txt` 里的**绝对计数**
   断言（`evict_inf` / `qtotal` / `replay` / `fdrop` / `total` / `recs` …）都可能被扰动。
   接入前先想清楚"我的埋点会在 Boot 期产生几条"，并在回归里预留/显式构造状态。
2. **不要让用例依赖"某一批恰好包含全部记录"**：只要有一条记录在 `cloud_collect_batch()`
   **之后**入队，它就会成为下一批。夹具要么显式制造尾巴（推荐 `logt cpush`），
   要么显式排空。
3. **MQTT/网络抖动会污染回归**：实测 A 段首轮因 MQTT 写入超时（`Writing didn't complete:
   errno=119`）导致 loop 阻塞 ~8s、串口静默，命令未被及时处理 ⇒ 11 条假 MISS。
   **判据是串口出现 `MQTT error event` / `Writing didn't complete`**；遇到大面积 MISS
   先查这个，再怀疑固件（见 PIO 技能 §10 的排查顺序）。

#### P2-E 加深的一层：**`boot_seq` 截断使"排空队列"不再可达**

P2-E 又出现同样的 4 条 MISS（`replay=8` / `qused=8` / `qused=0` / `rarmed=0`），
但**根因不同、更根本**：

```c
// cloud_collect_batch()
if (n > 0 && s_cloud_q[slot].boot_seq != s_cloud_batch[0].boot_seq) break;
```

⇒ 批次**在 `boot_seq` 变化处截断**。于是"一批装下全部补发记录 ⇒ `used` 归 0
⇒ sweep 走完 ⇒ `rarmed=0`"这个前提，**只在积压同属一个 Boot 且不超批次上限时成立**。

P2-E 让每个 Boot **多一条 Flash 记录**（`LOG_TIME_RTC_PROBE` 是 **WARN**，在无 RTC 的板上
**必现**）⇒ 积压 = 上一 Boot 8 条 + 本 Boot 1 条（**两个 boot_seq**）⇒ 单批永远排不空。
（**实现无缺陷**：`cloud_poll()` 会把余下记录作为下一批继续发，只是夹具没有足够 ACK 轮次；
所需轮次数 = 积压里不同 `boot_seq` 的个数，同样与历史相关。）

**加固做法（P2-E）**：

| 动作 | 内容 |
|---|---|
| **回归器新增内联正则期望** | 期望串里成对的 `/正则/` 片段按正则处理，其余按字面量。**向后完全兼容**（无 `/` ⇒ 仍子串匹配）。自检：`.pio/p15run/expect_hit_selftest.py`（13 例含 4 条**负向探针**） |
| **F2-B 判据改为"非零"** | `replay` 修复前在该场景**恒为 0** ⇒ `replay=/[1-9][0-9]*/` 精确且与积压历史无关 |
| **换成结构性事实** | `gcfloor=/[1-9][0-9]*/`（钳制生效）· `replay_seq=/[1-9][0-9]*/`（游标已推进）· `ack ok boot=`（旧批次 ACK **被接受**而非 DUPLICATE）· `giveup=0` |
| **断言总数不变** | 去掉 3 条与积压历史相关的绝对计数，换成 3 条结构性判据 + 补 1 条 ⇒ 仍 **196 条** |

> ⚠️⚠️ **写内联正则必须把字段名写进字面量**：`replay=/[1-9][0-9]*/`，
> **不是** `/[1-9][0-9]*/`。纯正则会 `search` **整行**，`offskip=1`、`ack_ok=2`
> 等其它数字会造成**假命中** —— 实测 `replay=0` 也被判 OK。

**⇒ 给后续模块（Workflow 起）的结论**：Boot 期**新增任何 Flash 记录**（即 WARN+）
都会改变补发积压的 boot_seq 构成。**凡是与"补发条数 / 队列排空"相关的断言，
一律不要写绝对值**，改用"非零"或结构性判据。



---

## Next

### 下一步：**Weight / HX711 接入**（P2-G）

Workflow 已接入完成（见上），回归 **196/196 全绿**。下一个按约定顺序是 **Weight**
（`src/weight.cpp`，`dispense_guard.cpp` 的触发源）：

1. ⚠️⚠️ **10 Hz 采样 ⇒ 原始值绝对不能进日志**。已有现成骨架：`weight_refresh_error_state()`
   里 `err != error_state` 的**状态边沿**（`weight.cpp:204`）—— 异常进入/退出各一条。
2. ⚠️ **5 类异常建议复用 `LOG_WEIGHT_ERROR_ENTER` + `LOG_P_CAUSE`**，暂不新增 EventId
   （P2 定版：先用已有 EventId + `CAUSE`/`ERR_CODE` 区分）。若发现确实无法区分，**先停下报告**。
3. ⚠️ **`weight_trigger_*` 类事件只记"触发成功/失败"**，不要记每次采样；如需"重量稳定/超重"，
   必须用**阈值 + 持续时间**的边沿（不是每帧比较）。
4. ⚠️ 标定类事件（`WEIGHT_CALIB_*`）当前**是否有宿主**要先 grep 确认（P2-E 的
   `NTP_FAIL` / P2-D 的 `FRAG_FAIL` 都是"冻结 ID 无宿主"的先例 ⇒ **无宿主就不埋、只记录**）。
5. ⚠️ `dispense_guard` 触发**强制关阀**：这条路径涉及执行机构安全 ⇒ 埋点只加观测、
   **不得改变任何时序**（P2-F 的"纯增量 0 删除行"做法可复用）。
6. 提交主题建议：`feat(log): integrate weight manager logging`

### 之后（严格一次一个模块）

`Valve → Dispense → BLE → Command/Event/OLED/Registry`

⚠️ **Valve 在 Dispense 之前**：`dispense_guard` 的判定依赖阀门开度反馈；
而 **Weight 已先行接入**（其异常事件是 guard 的触发源）。
⚠️ **Weight 提到 Valve 之前**：`dispense_guard` 的触发源是 `EVENT_WEIGHT_ERROR`，
先有 Weight 日志才能解释强制关阀。

---

## 未决 / 阻塞事项

| ID | 事项 | 阻塞什么 | 状态 |
|---|---|---|---|
| **BT-1** | **云端从未回送 `log_ack`**（`guo_feeder/down`）⇒ `acked_seq` 不前进 ⇒ Flash 段永不回收 | 线上回收闭环；P2 接入后日志量上升会加速暴露（496 条写满后开始淘汰未确认记录） | 🔴 **未解决**（外部依赖）。**Cloud Worker 代码不在本仓库**（本仓库只有 `cloud_protocol.md`）⇒ 按约定**只记录接口要求，不改设备侧协议**。接口要求见 §"BT-1 接口要求" |
| ~~**BT-9**~~ | ~~live ACK 越过旧未确认补发记录 ⇒ 永久跳过~~ | — | ✅ **已修复**（`fix(log): prevent ack watermark bypass replay records`）。方案＝**状态语义分离**：`acked_seq`（真实最大，仅观测）/ `gc_seq`（连续可回收，全部判定）/ `gc_floor`（钳制下界）。详见 Guide §11 与 Board-Test-Report 附录 R2 |
| **BT-10**（新） | 云队列溢出淘汰时**只给本 Boot 记录**登记空洞（`evict_boot == s_boot_seq`）⇒ 补发进来的上一 Boot 记录被淘汰后无空洞保护 | 复合低概率残余项（需同时：队列满且队首补发记录未发出 + 之后有更高 ACK + 触发段回收） | ⏳ **未修**（超出本次范围）。修法＝去掉该条件；代价＝空洞表压力上升（补发记录 seq 稀疏难合并，`LOG_HOLE_MAX=8` 可能溢出⇒退化为本 Boot 停止回收）。**F1 的 `holes=0` 断言正钉住当前行为** ⇒ 需单独评审 |
| **BT-11**（新，P2-C 期间发现） | `s_cloud_give_up_seq` 是**单点水位**：较新的批次先 give-up（实测 INFO 批次 seq 3073-3075 ⇒ `give_up_seq=3075`）后，seq 更小、仍在 Flash 等待补发的旧记录（seq≈1800）被 `log_ack_should_replay(seq <= give_up_seq)` 跳过 ⇒ **本 Boot 补发不到**（下次 Boot 归 0 后恢复，故**不丢数据**，属延迟） | 长离线后恢复时的补发**及时性** | ⏳ **未修**（P2-C 明令"不改 ACK/replay/GC 逻辑"）。与 BT-9 同源（单点水位越过旧记录），需在 LogManager 侧单独评审 |
| BT-4~BT-8 | ~~`test/log_fix_tests.txt` 中 20 条 MISS 的预期值修正~~ | P1.5 回归"全绿"基线 | ✅ **已修正 14 处**；回归现为 **196/196 = 100%，0 MISS** |
| P2-A 未验证项 | Storage bridge 的 **ERROR / CRITICAL 分支**与 **10 s 抑制窗口**无法用现有钩子触发 | 桥接层完整性 | ⏳ 待"结构化回调 + 故障注入"一并解决 |
| F2 决策 | 字符串参数是否改结构化回调（现为哈希/枚举化） | Storage 路径、Config key、Workflow id/action id 等 7 个 ParamId 的可用性 | ⏳ 已按"暂不扩 API"执行，未来可评审 |
| F6 基线 | P1.5 之前的既有回归集（P1.2 核心 66 / Flash 92 / F5 恢复 34 / F7-F9 43 / 交接 56）**未复跑** | P2 首个模块接入前的回归基线 | ⏳ 建议尽快补跑 |
| **WF-1**（新，P2-F 发现并**实测确认**） | **保存失败后重试风暴**：保存失败的两条路径**不重置** `workflow_save_since_ms` ⇒ 5 分钟延迟窗口**一旦过期就永远过期** ⇒ Dirty 未清期间 `workflow_delayed_save_poll()` **每个 loop** 跑一次完整保存事务（8.6 KB `def_buf` + LittleFS 落盘尝试 + 串口 2 行）。**实测 39–40 次/秒**（两次独立观测：151 s/5828 次、84 s/3399 次）；串口 ≈77 行/秒 | **loop 负载、LittleFS 擦写压力、串口洪泛**；也解释了"为什么它会掩盖串口命令"。**P2-F 埋点已对它免疫**（三个边沿锁 ⇒ 风暴期新增记录 0 条） | ⏳ **未修**（属**行为变更**，超出"只加观测"范围 ⇒ 按规则只报告）。最小修法＝两条失败路径补 `workflow_save_since_ms = millis();`，但**必须单独评审 + 独立提交 + 独立回归**（会把故障持续期的重试频率从 39/s 降到 1/5min）。详见审查报告 §3 与 Guide §15.5 |
| **P2-F 未验证项** | `FINISHED` / `TIMEOUT` / `FAILED` / `TEMP_ACTION_TIMEOUT` / `RUNTIME_ALLOC_FAILED` 五个事件**未做真机触发**（代码路径审查通过） | Workflow 运行期观测的完整性 | ⏳ `FINISHED`/`TIMEOUT` 需要"只含无害 Action 的 Workflow"（板上现存工作流会**真的驱动执行机构** ⇒ 有意不跑）；其余需故障注入。复测方法见 Guide §15.8 |
| **NTP 失败不可观测**（P2-E 发现） | **设备无法知道 SNTP 是否失败**：SDK 只提供成功通知，状态枚举无失败态。⇒ "设备联网正常但 NTP 一直没同步"这种故障**完全静默**（板上若 UDP 123 被墙就是这种状态） | Time 模块的诊断能力；云端的"NTP 是否可用"判断 | ⏳ **未解决**（P2-E 按约定不新增状态机）。若要做 = 加"已启动 N 秒仍无 `sntp_sync_seq` ⇒ 发 `LOG_TIME_NTP_FAIL`"的**纯观测看门狗**（约 5 行 + 1 个阈值常量 + 1 个 latch）。**建议单独评审后在 P2 收尾或 P3 处理** |
| **RTC 芯片缺失**（P2-E 发现） | 板上 `rtc_present=false`（`ERR_CODE=2` = Wire 地址 NACK ⇒ 芯片不在）⇒ 5 个 RTC 事件（`BOOT_RESTORE` / `CALIBRATED` / `WRITE_FAILED` / `VL_FLAG` / `BCD_INVALID`）与 `RTC_PROBE` 的**成功分支**无法上板验证 | RTC 路径的观测完整性 | ⏳ **待硬件**。焊上 PCF8563T（0x51，与 OLED 共用 I2C）即可验证前 4 个；`VL_FLAG` 还需**电池耗尽** |
| **无 `set_time` 通道**（P2-E 发现） | `time_set_manual()` / `time_set_manual_string()` **全仓库无调用者**（`time_manager_set_time()` 是预留空实现）⇒ ① `INVALID_ENTER` 无法运行时触发（需把时钟设到 2026-07-01 之前）；② `time_set_manual()` 的 RTC 写失败埋点不可达（本次**未埋点**） | `INVALID_ENTER` 的真机验证；手动校时功能本身 | ⏳ **未解决**（属功能缺失，不是日志问题）。P2-E 已按"埋点不放在不可达分支"处理并在 Guide §14.8 记录复测方法 |

---

## BT-1 接口要求（Worker 不在本仓库，仅记录要求）

云端需要在 `guo_feeder/down` 上实现 `log_ack` 回执。设备侧**已具备**接收能力并已在真机验证
（`[Cloud LOG] ack rx` 可被接收；**不带 `i` 也须被处理**；协议级消息必须旁路命令去重缓存）。

| 项 | 要求 |
|---|---|
| 命令名 | `"c": "log_ack"`（常量 `LOG_ACK_COMMAND`） |
| 参数键 | `"p": { "b": <boot_seq>, "f": <seq_from>, "t": <seq_to> }` |
| `i` 字段 | **可选**。不带 `i` 必须被正常处理（不得回 "Missing id"） |
| `b` | 必须回**设备上报的 boot_seq**（`b` 不匹配 ⇒ 设备 IGNORE） |
| `f` / `t` | 确认的 seq 闭区间；`f<=t` 且都非 0。覆盖整个批次 ⇒ ACCEPT；只覆盖前缀 ⇒ PARTIAL |
| **幂等义务** | 设备侧是 **at-least-once**（重启后 `acked_seq` 归 0，会重发）⇒ **云端必须按 `(device_id, boot_seq, seq)` 幂等**，重复 ACK 无副作用 |
| 触发时机 | 收到 `guo_feeder/log` 批次并**成功入库**后回一次 ACK（建议按批次回 `f/t` = 该批次的 seq 区间） |
| 不 ACK 的后果 | 设备走满 6 次超时 + 2/4/8/16/32 s 退避（≈152 s）后 give-up，**不推进水位、不删段** ⇒ 存储泄漏 + 重复补发 |

> ⚠️ 注意：**BT-9 未修之前，即使云端实现了 `log_ack`，旧未确认记录仍可能被越过**。
> 建议 BT-1 与 BT-9 一起解决（先修 BT-9 语义，再让云端闭环）。

---

## 本阶段对文档的产出

| 文档 | 内容 |
|---|---|
| `docs/LogManager-Integration-Guide.md` §9 | **新增**：Storage 回调桥接（注册方式 / 语义还原 / 参数映射 / 高频抑制 / 生命周期 / 上板结果） |
| `docs/LogManager-Integration-Guide.md` §10 | **新增**：ConfigManager 接入（与 Storage 的 3 点差异 / 关键词映射 / 2 个显式埋点 / 哈希 / 上板结果） |
| `docs/LogManager-Integration-Guide.md` **§11** | **新增**：**watermark 语义分离（FIX-BT9）** —— 根因 / 两个标量 / 为何 classify 也要改 / `gc_floor` 生命周期 / 刻意未改的部分 / 残余 BT-10 / 观测 / 验收 |
| `docs/LogManager-Integration-Guide.md` **§12** | **新增**：**WiFi 接入** —— 无回调接口⇒显式埋点 / 5 个埋点表 / 失败循环节流（N 次记 1 **AND** ≥60s）/ 为何重连不发 CONNECT_START / WAS·STATE 编码 / 参数语义 / 上板验证 / 已知限制 |
| `docs/LogManager-Integration-Guide.md` **§13** | **新增**：**Cloud/MQTT 接入** —— 复用既有单槽标志做边沿 / 5 个埋点表 / 发布失败计数聚合 / WAS·STATE 编码 / 刻意未做 / 上板验证（含 session takeover 触发技巧）/ 两项未触发的复测方法 |
| `docs/LogManager-Integration-Guide.md` **§14** | **新增**：**TimeManager 接入** —— 10 个冻结 EventId 的最终处置（含 `NTP_FAIL` **无宿主**的 SDK 证据）/ 11 个埋点表 / **SNTP 特殊处理**（为何挂在 settled 确认点、为何禁用 `status != IN_PROGRESS`、为何不拿平滑超时当失败）/ RTC 探测三分支 + **一处不可达陷阱**（`rtc_enabled && rtc_init()` 短路）/ 边沿锁去重 / `VALID_ENTER.SOURCE` 竞态 / 上板验证 / 未验证项 / **回归夹具加固（boot_seq 截断 + 内联正则期望）** |
| `docs/LogManager-Integration-Guide.md` **§15** | **新增**：**Workflow 接入** —— 与前面模块的 4 处差异 / 12 事件·17 发射点全表 / **三个边沿锁**（为何不用"降频"）/ 为何不记保存成功 / **WF-1 重试风暴实测**（39–40 次/秒、边沿锁 3400× 削减）/ Critical Op 审查结论表 / 上板验证 / 未验证项与 P2-G 注意事项 |
| `log模块历史/LogManager-P2F-Workflow接入审查0918.md` | **新增**：Workflow 接入前审查（Critical Op 生命周期审计 / 6 个 terminate 退出路径 / 矩阵与代码 4 项不一致 / 14 个推荐埋点位置 / WF-1 设计问题） |
| `docs/LogManager-P1.5-Board-Test-Report0918.md` | **新增**：P1.5 上板验证报告（环境 / 初始化 / MQTT / ACK / replay / F0–F8 / 已知问题）；**附录 R1**（用例修正与基线重录）；**附录 R2**（BT-9 分析·修复·验证 + 新基线 195/195） |
| `docs/P2_Log_Integration_Matrix.md` · `log模块历史/LogManager-P2接入准备审查0918.md` | 前置审查 —— 已随 `85c88b5` 入库 |

### 入库状态（已核实 `git ls-files`）

| 文件 | 说明 |
|---|---|
| `docs/P2_Log_Integration_Matrix.md` | ✅ 已入库（`85c88b5`） |
| `log模块历史/LogManager-P2接入准备审查0918.md` | ✅ 已入库（`85c88b5`） |
| `AI_RULES.md` | 已加「§9 Build/Test/Cleanup Rules」，但该文件**已被 `.gitignore` 忽略**（见 commit `1d0b47c`）⇒ 改动不会入库 |

### 未入库的过程工具（落 `.pio/`，被 ignore；供后续会话复用）

| 文件 | 用途 |
|---|---|
| `.pio/p15run/log_decode.py` | CBOR 批次 → **记录级**解码（`event_id` + 每个 ParamId 真值）。P2-E 期间修正了批次头**字段错位**（`v[2]` 是 `boot_seq`，不是 `seq_from`） |
| `.pio/p15run/p2e_probe.py` | 订阅 `guo_feeder/log` 跨一次复位抓 Boot 记录（**必须"先订阅、后复位"**，否则 Boot 头几秒的记录抓不到） |
| `.pio/p15run/p2d_probe.py` | 含 **session takeover 踢会话**技巧（用设备自己的 `client_id` 再连一次 ⇒ 产生真实 `DISCONNECTED`） |
| `.pio/p15run/expect_hit_selftest.py` | `serial_batch.py` 的 **`expect_hit` 真值表自检**（13 例含 4 条负向探针）——改匹配逻辑后必须跑 |
| `.pio/p15run/p2e_patch.py` | P2-E 的 14 处精确插入式补丁脚本（每处断言恰好命中 1 次） |
| `.pio/p15run/p2f_patch.py` | P2-F 的 24 处精确插入式补丁脚本（同样每处断言恰好命中 1 次） |
| `.pio/p15run/storm_probe.py` | **长静默观测探针**：单会话保持串口打开（重开串口会复位、清掉 RAM Dirty）⇒ 用来实测 WF-1 的 5 分钟延迟窗口与风暴。⚠️ `serial_batch.py` **串口静默 1.5 s 即提前返回**，做不了这种实验 |

---

*最后更新：2026-09-18（**P2-F Workflow 接入完成**，回归 196/196；下次：**P2-G Weight/HX711 接入**）*
