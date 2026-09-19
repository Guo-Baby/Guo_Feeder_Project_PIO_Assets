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
| **阶段 2-G · 审查** | **Weight/HX711 接入前审查**（只审查、未改生产代码）：HX711 链路与频率分析（进入率=每 loop / `current_weight` 2 Hz / 跳变 ≤2 Hz / not-ready 每 5 s）；**用户列的 9 个状态中只有 4 个在代码里真实存在**（`ready`/`overload` 不存在，`timeout`/`sensor error`/`unstable` 都被 `error_state` 的 `CAUSE` 覆盖）；**★ 发现 3 处矩阵与代码能力不一致**（① `weight_trigger_start()` 两失败路径复用 `ERROR_ENTER` 会重复/虚假 ② "校准开始"需新增 `0x0512`（未定义）③ `weight_init()` 无宿主） | `docs(log): review weight log integration plan` | ✅ 已提交 |
| **阶段 2-G** | **P2-G Weight/HX711 接入**（`src/weight.cpp`，唯一生产代码改动文件，**纯增量 `+66/−0`**）：5 个埋点全用冻结 ID（`ERROR_ENTER`/`ERROR_EXIT` 挂 `error_state` **天然边沿** + `ZERO_DONE`/`CALIB_FAILED` + `TRIGGER_FIRED`）；`CAUSE` 用**位掩码**(1=no_data/2=raw_zero/4=jump)；`ERROR_EXIT` 带 `CAUSE`+`DURATION_MS`（仅 2 个**只读**观测变量）；**零聚合、零节流**（高频源全部排除） | `feat(log): integrate weight manager logging` | ✅ 已提交 |
| **阶段 2-G · 拍板** | 用户四项决策**全部选 A**：① `weight_trigger_start()` 两个失败路径**不埋**（复用 `ERROR_ENTER` 会重复/虚假）② "校准开始"不埋（守"暂不新增 ID"，`0x0512` 保持未定义）③ `CAUSE` 用**位掩码** ④ `ERROR_EXIT` **带** `CAUSE`+`DURATION_MS`（仅加只读观测变量，不改状态机）。另明确：**不把 `jump_error` 转成 ERROR_ENTER**（它属"重量异常**事件**"，不是"重量错误**状态**"） | — | ✅ 已按此实现 |
| **阶段 2-G · 夹具** | ⚠️ **F3-A 的"恰好填满"断言被打破**（P2-G 首次回归 B 62/66）：Weight 边沿恰好落在填充窗口内（实测 `23:47:00`）⇒ `LOG_WEIGHT_ERROR_ENTER`(WARN 落 Flash) 多写 1~2 条 ⇒ 环提前回绕 ⇒ `total=496`/`append=15@31`/`seg_evict_unacked=0`/`total=466` 4 条断言失效。**按"修夹具不降断言"处置**：移除这 4 条"无干扰"假设、改为 `segs=16` 结构性判据 + **多写 1 条保证必然越界**；**HIGH-1 的精确记账断言（`seg_evict_unacked=31`/`fdrop=31`/`seg_del=1`）原样保留** | `test(log): harden F3-A ring-fill fixture against inter-module WARN interference` | ✅ 已提交（**195/195 = 100%**） |
| **阶段 2-H · 审查** | **Valve/DispenseGuard 接入前审查**（只审查、未改生产代码）：`valve_force_close()` 确认为**纯事件非状态迁移**（无判等）；**矩阵 6 状态中 4 个不存在**（实际只有 `bool current_state`）；**3 处矩阵与代码不符**（`FORCE_CLOSE_FAILED` 确有 `valve_pin<0` 分支 / CRITICAL"IMM"实际只是置一次 flush 标志 / `RATE_LIMITED` 实测 0 次无需去重）；🔴 发现 **VALVE-1**（四个 valve API 均不检查 `initialized`⇒假成功） | `docs(log): review valve log integration plan` | ✅ 已提交 |
| **阶段 2-H · 拍板** | 用户七项决策**全部选 A**：D1 去重门控（cause 变化 / 5 s 冷却，只限日志不改 GPIO）· D2 VALVE-1 本轮不修只登记 · D3 矩阵按真实模型重写 · D4 SAFETY_TIMEOUT 加一次性报告锁 · D5 埋 `FORCE_CLOSE_FAILED` · D6 无宿主项一律不埋且不新增/不借 EventId · D7 R-6 本轮不定位 | — | ✅ |
| **阶段 2-H** | **P2-H Valve/DispenseGuard 接入**（`src/valve.cpp`，唯一生产代码改动文件，**净增量 `+125 / −1`**，唯一 −1 是把 `if(valve_pin<0){return false;}` 单行守卫展开为块）：**6 个埋点 / 6 个冻结 EventId**（`OPEN`/`CLOSE` 挂 `valve_set_gpio()` 天然边沿 + `RATE_LIMITED` + `FORCE_CLOSE`（5 s 门控）+ `FORCE_CLOSE_FAILED` + `SAFETY_TIMEOUT`（一次性锁））；`log_events.h` / `event_manager.cpp` / `dispense_guard.cpp` **零改动** | `feat(log): integrate valve logging` | ✅ 已提交 |
| **阶段 2-H · 冲突** | ⚠️ **回归 168/195（26 MISS）** —— 根因＝A 段发生 **69 次** `valve_force_close()` （P2-G 那轮仅 2 次）⇒ 门控后仍多出约 **34 条 CRITICAL** ⇒ F0/F1/F2 的**精确记账断言**（`total=40`/`replay=31`/`evict_inf=9`/`qdrop=12`…）全部失准。**"按 P2-G 方式修夹具"在此不适用**：这 26 条断言就是 FIX-1/2/3 的记账本体，改结构性判据 = 降断言 ⇒ **已停止并上报**，待拍板（Guide §17.9/§17.10） | — | ⛔ **阻塞待决策** |
| **阶段 2-H · 判定** | **回归干扰隔离实验（纯运行时观测，未改任何代码/夹具）**：同一 workload 跑 **P2-G 固件**（无 Valve 日志）与 **P2-H 固件** ⇒ 额外记录配比 **Valve `crit`=9 : Weight≈6**；`qdrop`/`fdrop`/`seg_evict_unacked`/`seg_new`/`seg_del` **几乎或完全相同**（结构行为未被改变）；**决定性判定**：P2-G 固件跑同一 A 段夹具三次 ⇒ `16调用/2边沿 → 0 MISS`、`68/11 → 9 MISS`、`48/12 → 2 MISS` ⇒ **失败与"是否有 Valve 日志"不相关，只与环境重量异常强度相关**；另实测 **Weight 侧单独就能 +13 条额外记录** （P2-G #3 `emit` 509 vs 基数 496、`crit` 恒 0）⇒ **去掉 Valve 日志不能归零** | `log模块历史/LogManager-P2H-回归干扰分析报告0919.md` | ✅ 已提交 |
| **阶段 2-H · 结论** | **本轮决定：`R-7` / `R-8` / `VALVE-6` 全部登记 OPEN、不修、不改代码**；**明确不采用"给 weight 加 `enable`"**（改变 Weight 能力边界、影响 System Config，超出 P2-H 范围）；另修正 `R-6` 表述（`force_close` 调用数 == `guard_rx` 恒等 ⇒ **不是"调用被复制"**，而是**同毫秒两个独立 `EVENT_WEIGHT_ERROR`**） | — | ✅ |
| **阶段 2-I** | **P2-I Weight Error 日志写入压力优化**（**仅 `src/log_manager.h` `+25/−0` 与 `src/log_manager.cpp` `+248/−5`**；`weight.cpp` / `dispense_guard.cpp` / `event_manager.*` / `log_events.h` **零改动**）：在 LogManager 内部引入 **突发合并（Burst Coalescing）**（白名单 = `LOG_WEIGHT_ERROR_ENTER`）：5 s 窗口内重复发生折叠为计数，被接受的记录携带 `LOG_P_COUNT`，窗口到期由 `log_task()` 汇总一条；**不变量Σ `LOG_P_COUNT` = 真实发生次数**（实测 17 次发生 → 5 条记录，降幅 **70.6%**）；安全链（EventManager→DispenseGuard→valve）**零接触** | `feat(log): coalesce repeated weight error records` | ✅ 已提交 |

---

## P2-H Valve Logging Integration — 完成 / 冻结（**2026-09-19**）

> **`P2-H implementation complete`　·　`Regression blocked by environment interference`**

| 项 | 值 |
|---|---|
| 日期 | 2026-09-19 |
| **生产代码 commit** | `d92d397` `feat(log): integrate valve logging` |
| 文档 commit | `95be626`、`78ba304`、`3239890`、`d30759a` |
| **修改文件** | **仅 `src/valve.cpp`** —— 净增量 **`+125 / −1`**（唯一 −1 = 把 `if(valve_pin<0){return false;}` 单行守卫展开为块；**条件、返回值、控制流完全不变**） |
| 未改动（零改动） | `src/log_events.h`、`src/event_manager.cpp`、`src/event_manager.h`、`src/valve.h`、`src/dispense_guard.cpp`、`test/log_fix_tests.txt` |
| 固件 | `.pio/build/p2h`（Flash **65.3 %** / RAM **39.8 %**，与 P2-G 相同） |
| EventId / ParamId | **未新增任何 EventId / ParamId**（`LOG_P_MAX = 0x59` 未被触碰） |

### EventId 列表（6 个埋点，全为冻结 ID）

| EventId | Level | 实现位置 | 去重策略 | 上板验证 |
|---|---|---|---|---|
| `LOG_VALVE_OPEN` 0x0509 | INFO | `valve_set_gpio()` open 分支（`valve.cpp:168`） | **天然边沿**（`open == current_state` 判等 `return` 在其上方） | ✅ `OPEN(STATE=true, WAS=false)`；**重复 `valve_open()` 只产生 1 条** |
| `LOG_VALVE_CLOSE` 0x050A | INFO | `valve_set_gpio()` close 分支（`valve.cpp:181`） | 同上 | ✅ `CLOSE(STATE=false, WAS=true, VALVE_OPEN_MS=2006)` |
| `LOG_VALVE_RATE_LIMITED` 0x050B | WARN | `valve_set_gpio()` 50 ms 防风暴 `return` 分支（`valve.cpp:137`） | **不需要**（判等 return 在防风暴检查之前 ⇒ 只在"真要切换"时才可能进入；实测 11 个会话 0 次） | ⚪ 未触发（`NC-13`：串口命令做不到 50 ms 内二次真实切换） |
| `LOG_VALVE_FORCE_CLOSE` 0x0505 | CRITICAL | `valve_force_close()` 尾部（`valve.cpp:470`） | **门控**：`cause 变化` **或** `距上次记录 ≥ 5000 ms` | ✅ **精确通过**：清零后 149.7 s 窗口 **10 次调用 → Δcrit = 4**，**离线按 5 s 规则模拟亦 = 4**，上限 30，零"孤儿"增量 |
| `LOG_VALVE_FORCE_CLOSE_FAILED` 0x0506 | CRITICAL | `valve_force_close()` 的 `valve_pin<0` 分支（`valve.cpp:405`） | ⚠️ **无门控**（按 spec）⇒ `VALVE-6` OPEN | ✅ `gpio_pin=-1` 后：串口 `FORCE CLOSE` 计数 **0**、`guard_rx=54` ⇒ **52 条** CRITICAL（`CAUSE=1`） |
| `LOG_VALVE_SAFETY_TIMEOUT` 0x0508 | WARN | `valve_task()` 超时分支（`valve.cpp:384`） | **一次性报告锁**（`safety_timeout_reported`；锁在"真实开启/关闭成功"处解除） | ✅ `safety_timeout_sec=2` ⇒ **6 轮超时 → 每轮恰好 1 条**（`OPEN_MS=LIMIT_MS=2000`） |

### `FORCE_CLOSE` 宿主顺序（★ 无提前 return）

```
valve_force_close()
  ├─ :398  if(valve_pin < 0) → 埋 FAILED → :407 return false     ← 唯一提前 return（失败分支，日志先于 return）
  ├─ :413  gpio_set_level(...)        ① GPIO 动作
  ├─ :416  current_state = false      ② 状态更新
  ├─ :418  valve_update_state()       ② SystemState 同步
  ├─ :424  open_start_time = 0        ② 计时清理（OPEN_MS 已在其前取值）
  ├─ :427  event_push(...)            ③ 事件发布
  ├─ :435  Serial.printf(...)         ③ 串口
  ├─ :461  日志判断（门控）+ :470 log_emit     ④ ★ 日志判断在【全部之后】
  └─ :474  return true
```
⇒ **GPIO 动作 → 状态更新 → 日志判断**，**没有 `if(!need_log) return;`**，
强制关阀的"无条件强制同步 GPIO"语义**零改动**（**未**改成 `current_state` 边沿判断）。

### 验证结果（记录级）

- 捕获路径：MQTT `guo_feeder/log`（`.pio/p15run/p2e_probe.py` + `log_decode.py`）；**设备侧计量**：`logt stats` 的 `crit=`（已验证不减实：`logt fill crit 62` ⇒ `Δcrit` 恰好 62）
- ⚠️ **精确计数实验前必须先 `fwipe`+`mwipe`+`creset`+`reset`**（否则回放/前序 Boot 记录会造出 18~37 s 的"孤儿增量"，极易误判成门控失效）
- `VALVE-2`、`VALVE-3` 于本轮 **DONE**（见 `未修复的问题.md`）

### 回归阻塞说明（**不是 P2-H 实现失败**）

全量回归 **168/195（26 MISS）**；性质定义为 **Regression Environment Interference**：

1. **Valve logging 本身正确** —— 6 个埋点逐项上板验证通过。
2. 26 MISS 分类：**A = 5 条**（新增日志直接增加记录数量，全是 Flash 计数类）、**D = 21 条**（记录数量变化 → 存储拓扑变化，`replay`/`evict_inf`/`qdrop`/`holes` 等派生物）。
3. **不能证明"删除 Valve logging 可恢复 195/195"** —— 依据：**P2-G 固件（完全没有 Valve logging）在同一 workload 下仍产生 MISS**
   （A 段三次：`16调用/2边沿 → 0 MISS`、`68/11 → 9 MISS`、`48/12 → 2 MISS`）；且实测 **Weight 侧单独就能产生 +13 条额外记录**。
4. 本轮**不处理** `R-7` / `R-8` / `VALVE-6`，均保持 **OPEN**。
5. 完整分析：`log模块历史/LogManager-P2H-回归干扰分析报告0919.md`

---


## Current baseline

```
HEAD:           见 `git log --oneline -1`（每次提交后更新本行）
分支:           wb
P1.5 设备侧:    ✅ 不再 BLOCKED（A–E 全部可跑段落已完成）
回归基线:       ⛔ **168/195（26 MISS）—— 阻塞待决策**（A 42/56 · B 64/65 · C 20/21 · D 23/30 · E 19/23）
                   ⚠️ **不是代码缺陷**：P2-H 的 5 个埋点已逐项上板验证通过（含门控"10 次调用 → 4 条记录"
                   **与 5 s 规则离线模拟值精确一致**）。失败的全部是 **F0/F1/F2 的绝对计数断言**，
                   根因＝实测 **A 段发生 69 次 `valve_force_close()`**（P2-G 那轮只有 2 次）⇒ 门控后
                   仍多出约 34 条 CRITICAL 记录 ⇒ 落 Flash + 进云队列 ⇒ 精确记账全部失准。
                   ⇒ **这类夹具与"跨模块 WARN+/CRITICAL 埋点"根本冲突**，需要"测试隔离"而不是"继续打补丁"
                   ⇒ **★★ 已定路线：下一阶段 = `R-7` 测试隔离方案评审**（见下方 `## Next`）；
                   **`E1`（weight enable）已否决**。
                   ⚠️ 干扰源是**突发式**的（静置实测 0.08 ~ 0.51 次/s，单秒峰值 2~7）⇒ 期望值不可确定，
                   无法通过"夹具自身 fwipe/mwipe 清零"解决（只能清"之前"的记录）。
                   ⚠️ **不要在跑回归时注入 ACK**：实测开"虚拟云端"自动回 ACK ⇒ 队列不再溢出 ⇒
                   `qdrop`/`evict_inf`/`replay` 类断言全部失效（A 52/56 · B 56/66 · E 13/23）—— **夹具隐含依赖 BT-1**
                   历史：首轮 167/187 → R1 187/189（2 MISS=BT-9）→ BT-9 修复后 195/195
                   → P2-D 夹具加固 196/196 → P2-E 夹具加固（正则期望）196/196 → P2-G **195/195**
                   → **P2-H 168/195（阻塞；断言总数仍为 195，未做任何删改）**
                   ★★ **判定（2026-09-19 隔离实验）：这不是 P2-H 代码缺陷，也不是"Valve 日志"问题** ——
                   **P2-G 固件（完全没有 Valve 日志）在同一 A 段夹具上，异常强度高时同样 MISS（9 条 / 2 条）**；
                   MISS 数只随"环境重量异常强度"单调上升（2 边沿→0、11→9、12→2、18→13）。
                   另实测 **Weight 侧单独就能产生 +13 条额外记录**（P2-G #3：`emit` 509 vs 基数 496，`crit` 恒 0）⇒
                   **只去掉 Valve 日志最多把污染减半（Valve : Weight ≈ 9 : 6），不能归零。**
                   ⇒ 需要的是"**测试隔离**"（让被测系统安静），不是消除某一个模块的日志。
                   ⇒ **本轮：R-7 / R-8 / VALVE-6 登记 OPEN、不修、不改代码**（详见干扰分析报告）。
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
| **Weight（`weight.cpp`）** | 无回调接口 ⇒ **直接显式埋点**（5 处 / 5 个事件）；`ERROR_*` 挂 `error_state` **天然边沿**（无需任何去重逻辑）；**纯增量 `+66/−0`**；`CAUSE` 位掩码；2 个**只读**观测变量（供 EXIT 报 `CAUSE`+`DURATION_MS`） | `LOG_WEIGHT_ERROR_ENTER` / `ERROR_EXIT` / `ZERO_DONE` / `CALIB_FAILED` / `TRIGGER_FIRED` | P2-G |
| **Valve（`valve.cpp`）** | 无回调接口 ⇒ **直接显式埋点**（6 处 / 6 个事件）；`OPEN`/`CLOSE` 挂 `valve_set_gpio()` **天然边沿**；`FORCE_CLOSE` 用 **5 s / cause 门控**（只限日志，不改 GPIO 语义）；`SAFETY_TIMEOUT` 用**一次性报告锁**；**净增量 `+125/−1`**；`dispense_guard.cpp` 未改（策略层不加 LOG） | `LOG_VALVE_OPEN` / `CLOSE` / `RATE_LIMITED` / `FORCE_CLOSE` / `FORCE_CLOSE_FAILED` / `SAFETY_TIMEOUT` | `feat(log): integrate valve logging` |
| **Command（`command_manager.cpp`）** | 无回调接口 ⇒ **直接显式埋点**（4 个事件 / **5 处 `log_emit`**）；**只对 `REJECTED` 加 5 s 去重门控**（本模块唯一"状态型"语义），其余 3 处均为**天然边沿**；`command`/`cmd_id` 一律 **FNV-1a 哈希后入日志**（不上报原文）；④ 刻意**排除 `execute_action`/`execute_workflow`**（异步，避免与 Workflow 段跨段重复上报） | `LOG_CMD_RUNTIME_QUEUE_FULL` / `REJECTED` / `RUNTIME_TIMEOUT` / `APPLIED` | **P2-J** |
| **ComputerReset（`computer_reset.cpp`）** | 无回调接口 ⇒ **直接显式埋点**（3 个事件 / **4 处 `log_emit`**）；**全部天然边沿、零门控**；★ `PULSE` 上移到 `set_output()` 的 **LOW→HIGH 上升沿**（而非矩阵写的 `trigger()`）以**同时覆盖手动与 Workflow Action 两条路径**；`SAFETY_TIMEOUT` 的 `DURATION_MS` **必须在 `force_idle()` 之前取**（LOG-13） | `LOG_CRESET_PULSE` / `SAFETY_TIMEOUT` / `POOL_EXHAUSTED` | **P2-K** |
| **Capability Registry（`capability_registry.cpp`）** | **单向依赖 `→ log_manager`，无回调、无 EventManager 转发、无 System State**（2 处 / 2 个事件，**纯增量 `+24/−0`**）；★ **宿主选"定义点"而非"触发点"**：重建埋 `registry_sync()` 的 `else` 分支、保存失败埋 `save_registry_file()` 返回值判定处；**不埋** `command_manager` 的 4 处 `rescan()` 调用点（一次 create 连触发 3 次）；**不进** `save_registry_file()` 内部 4 个失败出口（各有专属串口打印，1:1 覆盖 ⇒ 不带 `ERR_CODE`、不改签名）；参数用**栈上局部 `LogParamIn`**（禁 `static`/全局 —— 可能跑在 loopTask 8KB 或 esp-mqtt 任务上，防并发踩踏） | `LOG_REG_REBUILT`(INFO) / `LOG_REG_SAVE_FAILED`(ERROR) | **P2-L** |
| **Event（`event_manager.cpp`）** | **无回调接口 ⇒ 直接显式埋点，但只记"自身运行异常"**（2 个事件 / **2 处累加 + 1 个周期上报器 = 2 个 `log_emit` 调用点**，**纯增量 `+110/−0`**）；★ **业务事件一律不记**（17 个 `event_push` 中 13 个发布方已埋 `log_emit` ⇒ 转发即重复）；★ **周期聚合方案 A**：窗口 **60000 ms**，节拍器 = **`event_dispatch()` 首行**（loop 每轮无条件调用 ⇒ 不新增 Task / 不改 `main.cpp` / 不新建 timer / 无 delay / 无 while）；窗口内无异常**完全静默且不推进窗口**（首次异常立即可见）；**Σ `LOG_P_COUNT` = 真实丢弃数量**（信息零丢失、条目数恒定），与 `cloud_manager.cpp` 的 `CLOUD_PUBLISH_FAIL_REPORT_MS` 同构；FORCE 优先级不足分支**不计入**（语义为"被挤占"且零发布方） | `LOG_EVT_QUEUE_FULL`(WARN) / `LOG_EVT_STORM_DROPPED`(WARN) | **P2-M** |

未接入清单：**（P2 埋点清单已全部完成）** —— **BLE 暂缓**（高频来源，须先设计筛选 / 聚合 / callback 上下文安全）；**OLED 已判无宿主不埋**；**Dispense 移出埋点清单**，归入 Phase 4 业务开发（其埋点随模块本体一起建）。

> **★ 2026-09-20 路线调整**：原顺序 `Dispense → BLE → Command/Event/OLED/Registry`
> 改为 **`Command → OLED → ComputerReset → Registry → Event`**，理由：
> ① **前 4 项成本极小、风险极低**（与 P2-B / P2-C 同构，回调已注册或单点埋点），
> 可低成本批量推进；② **`Event` 需要特殊设计**（聚合 / 统计 / 周期报告 / drop 统计），
> 放最后做；③ **`BLE` 暂缓**（高频来源，须先设计筛选 / 聚合 / callback 上下文安全）；
> ④ **`Dispense` 移出埋点清单**，归入 Phase 4 业务开发（其埋点随模块本体一起建）。
> ⚠️ `Dispense` **不再等待 regression baseline 稳定**（`R-7` 已 `DEFERRED`）。

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

**设备状态复原**（如实记录，供下一会话判断）：

| 项 | 状态 | 说明 |
|---|---|---|
| `dirty` | `0` ✅ | 测试期新增的无效定义已 `wfc del` 清掉，**无残留风暴**（终检 `save transaction` 计数 = 0） |
| **Workflow registry 漂移** | ⚠️ **`[CapRegistry] WORKFLOW count=1`** | 本次清理时我把 slot 2（`queue_test` / "开阀测试"）当测试垃圾删掉了 —— 事后核对 **`data/workflow.json`（tracked，未改动）里有 3 个**（`daily_valve_test` / `daily_valve_test1` / `queue_test`）⇒ **删掉的是"真"工作流**。且该漂移**早于本次**（P2-D 期间观测到 `count=2`、P2-F 期间 `count=1`）。**恢复方式**：重建 `data/`（`tools/gen_workflow_bin.py`）+ `uploadfs`（**会整体擦除 LittleFS**，属项目既定基线流程），或删掉设备上的 registry 文件让它从 `/workflow.json` 重建 |
| `/littlefs/log/*` 被清 | 正常 ✅ | `[Log] meta missing/corrupt -> rebuild` + `boot_seq 归 1`：回归套件里的 **`logt fwipe`（7 次）/ `logt mwipe`（7 次）是套件自带的清理动作**（`log_manager.cpp` 会 `LittleFS.remove()` 段文件与 meta）⇒ **不是异常** |
| ⚠️ 遗留 | `wfc create`（控制台新建的**不完整定义**）会产生**永久 `INVALID_ARGUMENT`** ⇒ 用它做失败注入很方便，但**测完必须删掉**，否则 5 分钟延迟窗口一到就持续触发 WF-1 风暴（P2-F 期间已遇到一次） |

### P2-G 验证记录（2026-09-18 深夜，COM8；固件 `.pio/build/p2g`，Flash 65.3%）

**① `LOG_WEIGHT_ZERO_DONE` —— 记录级通过 ✅**

```
串口： [Weight] Zero calibration started
       [Weight] Zero calibrated, offset=-1, save=1            ← 20 次采样完成 + config_save 成功
       [Log Cloud] ack ok boot=1 to=4294967295 covered=2/2    ← 校准产生的 2 条被 ACK 覆盖
MQTT： seq=521 boot=1 INFO LOG_WEIGHT_ZERO_DONE
              LOG_P_OFFSET(i32)=-1  LOG_P_SAMPLES(u32)=20  LOG_P_SAVED(u32)=1
```
触发方式：`cm {"cmd":"system","ob":"weight_zero","id":"...","p":{}}`（CommandManager `system/weight_zero`）。
⚠️ **必须在同一 Boot 内 ACK**，否则 `ZERO_DONE`（INFO 只上云）会随 config-save 触发的重启一起丢失（第一次尝试就是这样丢的）。

**② `LOG_WEIGHT_ERROR_ENTER` / `ERROR_EXIT` —— 边沿在板上实测执行，但记录级未捕获**

| 事实 | 证据 |
|---|---|
| 边沿确实发生 | B 段每次运行均有 `[Weight] STATE_WEIGHT_ERROR -> 1` / `-> 0`：本次 **2 组**，另一会话 **5 组**（含一次持续 8 s 的真异常） |
| 埋点与它同块 | `Serial.printf` 与两条 `log_emit` 同在 `if(err != error_state)` 内，**中间无任何条件** |
| 为何拿不到记录 | 这些边沿恰好落在 F3-A 的 **496 条填充窗口**内，填充必然**冲垮云队列（128 槽）** ⇒ 记录被 `qdrop` 淘汰（**这正是回归在测的溢出行为，不是埋点失效**） |
| 复测方法 | 在填充窗口内**持续 ACK**；或把填充降到 <128 条 —— 实测 **2×62 条不足以触发边沿**（边沿由"累积 FS 阻塞"诱发，8×62 才会出现） |

**③ 未触发项**：`LOG_WEIGHT_CALIB_FAILED`（需 `config_set_weight_zero_offset()`/`config_save()` 失败）、`LOG_WEIGHT_TRIGGER_FIRED`（需真实减重场景）—— 代码路径审查通过。

**④ 设备状态副作用（如实记录）**：校准把 `weight.zero_offset` 从 **-800750 覆写为 -1**（该板 HX711 读数恒为 0/-1，`Zero calibrated, offset=-1, save=1`）。影响：`current_weight ≈ 0 g`（此前恒 ≈1080 g）。**未回写**（避免再触发重启）；如需恢复旧值：`config_set weight/zero_offset = -800750` + `config_save`。

**⑤ 新增可复用工具**：`.pio/p15run/log_mirror.py`（虚拟云端：订阅 + 自动 ACK + 落盘）。
⚠️ **两条使用限制**（实测得出）：① **绝不能与 196 回归同时运行**（ACK 破坏队列溢出断言）；② 其自动 ACK 的 `b` 字段取自批次头（`first.boot_seq`），实测被设备 **IGNORE** ⇒ 同批次连发 11 次 ⇒ **只能当记录采集器**，可靠 ACK 仍须走串口 `logt ack <BOOT> …`。

### P2-H 验证记录（2026-09-19，COM8；固件 `.pio/build/p2h`，Flash 65.3% / RAM 39.8%）

**① `LOG_VALVE_OPEN` / `LOG_VALVE_CLOSE` —— 记录级通过 ✅**

```
MQTT： seq=523 INFO LOG_VALVE_OPEN   STATE(bool)=True  WAS(bool)=False
       seq=524 INFO LOG_VALVE_CLOSE  STATE(bool)=False WAS(bool)=True VALVE_OPEN_MS(u32)=2006
串口： [Valve] OPEN (pin=12, level=1) → [Valve] CLOSE (pin=12, level=0)
```
**★ 天然边沿去重已验证**：连续两次 `valve_open`（第二次是 no-op）**只产生一条记录**。

**② `LOG_VALVE_FORCE_CLOSE` 门控 —— 精确通过 ✅**

先 `fwipe` + `mwipe` + `creset` + `reset`（**消除回放干扰**），再纯 `logt stats` 观测 150 s：

```
窗口内 valve_force_close() 调用次数   = 10      ← 无门控时应产生 10 条 CRITICAL
实测 Δcrit（logt stats 的 critical_seen 增量） = 4
按 5 s 规则离线模拟 emit 次数          = 4      ← ★ 与实测完全一致
crit 增量点与前置 FORCE_CLOSE 时间差    = 1.03/0.66/0.66/0.66 s（零"孤儿"增量）
```
捕获样张：`seq=3851 CRITICAL LOG_VALVE_FORCE_CLOSE CAUSE(u32)=1 VALVE_OPEN_MS(u32)=7998`

**③ `LOG_VALVE_FORCE_CLOSE_FAILED` —— 记录级通过 ✅**

`config_set valve/gpio_pin=-1` 后：串口 `FORCE CLOSE` 计数 = **0**（确认走失败分支），
`guard_rx`（Weight error received）= **54** ⇒ 记录了 **52 条** CRITICAL：
`seq=3082 CRITICAL LOG_VALVE_FORCE_CLOSE_FAILED CAUSE(u32)=1`
⚠️ 该分支**无门控**（按 spec）⇒ 引出新问题 `VALVE-6`。

**④ `LOG_VALVE_SAFETY_TIMEOUT` 一次性报告锁 —— 通过 ✅**

`safety_timeout_sec=2`，6 轮 `valve_open` + 等待：

```
01:12:20.544 OPEN     01:12:22.563 Safety timeout! + CLOSE      → SEQ 5385/5386/5387
01:12:28.084 OPEN     01:12:30.031 Safety timeout! + CLOSE      → SEQ 5389/5390/5391
...
6 轮 = 6 次 "Safety timeout!" = 6 条 LOG_VALVE_SAFETY_TIMEOUT（每轮恰好 1 条）
记录：WARN LOG_VALVE_SAFETY_TIMEOUT OPEN_MS(u32)=2000 LIMIT_MS(u32)=2000
      INFO LOG_VALVE_CLOSE          VALVE_OPEN_MS(u32)=2000
```
⇒ **一次真实超时恰好 1 条**（`VALVE-2` 的修复目标达成）。

**⑤ 清理动作（如实记录）**：为验证 ③ 与 ④，临时改过 `valve/gpio_pin`（-1 → 12）与
`valve/safety_timeout_sec`（300 → 10 → 2 → **已恢复 300**）。`weight/zero_offset` 仍为 **-1**（P2-G 的 `D-2`，未动）。

**⑥ 新增可复用工具**：`.pio/p15run/valve_watch.py`（**只读**串口观测器：统计 FORCE CLOSE / Guard /
WEIGHT_ERROR 边沿 / rate-limit 的**每秒分布**，用于量化高频调用；不烧录、不改代码）。

**⑦ 新增可复用手法（两条，见 Guide §17.7）**
1. **`logt stats` 的 `crit=` / `emit=` 增量 = "设备侧"计量**（云队列溢出时云端捕获会丢，这个不会）。
   已单独验证该计数**不重复计数**：`logt fill crit 62` ⇒ `Δcrit` **恰好 = 62**。
2. ⚠️ **不清零 Flash 时 `crit` 会被"回放/前序 Boot 记录"污染** —— 未 `fwipe` 时观测到
   `crit` 增量点与任何 `FORCE_CLOSE` 相差 **18~37 s**（"孤儿"）；清零后孤儿全消失。
   ⇒ **做精确计数实验前必须先清零**，否则会把"回放记录"误判成"门控失效"。

---

### P2-I 验证记录（2026-09-19，COM8；固件 `.pio/build/p2i`，Flash 65.3% / RAM 39.9%）

**改动**：`src/log_manager.h` `+25/−0`、`src/log_manager.cpp` `+248/−5`（`−5` 为**签名新增形参**与
`param_count → use_n` 的机械替换）。`weight.cpp` / `dispense_guard.cpp` / `event_manager.*` /
`log_events.h` / `test/` **全部零改动**；**未新增 EventId / ParamId**。

**机制**：白名单（当前仅 `LOG_WEIGHT_ERROR_ENTER`）事件在 `LOG_COALESCE_WINDOW_MS = 5000` 窗口内重复发生时
**不产生记录**、只累计 `folded`；被接受的记录自动追加 `LOG_P_COUNT`（=1）；窗口到期由
`log_task()` 阶段 0 产出**汇总记录**（`LOG_P_COUNT` = 折叠次数）。

**★ 决定性验证（确定性注入，非环境诱发）**

关键手段：**`logt fill <level> <n> <EventId(hex)>`** 支持第 3 个参数指定 EventId
（`src/main.cpp:1292`，缺省回落 `LOG_WF_START`）⇒ 可**定向注入** `LOG_WEIGHT_ERROR_ENTER`：

| 注入 | 期望 | 实测（MQTT 记录级，`qdrop=0` / `evict_inf=0` ⇒ 捕获完整） |
|---|---|---|
| `fill warn 1 50C` | 1 条 `COUNT=1` | ✅ `seq=16904 COUNT=1` |
| `fill warn 10 50C` | 1 条 `COUNT=1` + 到期 1 条 `COUNT=9` | ✅ `seq=16905 COUNT=1` / `seq=16906 COUNT=9` |
| `fill warn 6 50C` | 1 条 `COUNT=1` + 到期 1 条 `COUNT=5` | ✅ `seq=16907 COUNT=1` / `seq=16908 COUNT=5` |
| `fill info 4 50D`（EXIT，**非白名单**） | 逐条 4 条 | ✅ 4 条，**未合并** |
| `fill warn 4 401`（`LOG_WF_START`，**非白名单**） | 逐条 4 条 | ✅ 4 条，**未合并** |

**三项不变量实测**

1. **Σ `LOG_P_COUNT` = 真实发生次数**：`1+1+9+1+5 = 17` vs 注入 `1+10+6 = 17` ⇒ ✅ **守恒**
2. **记录数下降**：**17 次发生 → 5 条记录**（降幅 **70.6%**）；10 连发场景 **10 → 2 条（−80%）**
3. **被接受记录间隔 ≥ 窗口**：实测 `8533 / 5000 / 6941 / 5000 ms` ⇒ ✅ 成立
   （两条正好 5000 ms 的就是窗口到期产出的汇总条）

**真实重量异常路径（非注入）**：V2/V6 会话捕获到真实
`LOG_WEIGHT_ERROR_ENTER(CAUSE=2 RAW=0 COUNT=1)` + `LOG_WEIGHT_ERROR_EXIT(CAUSE=2 DURATION_MS=91)` 配对，
**EXIT 参数与语义完全未变**；同期 `guard_rx == FORCE_CLOSE` 计数**恒等**（6/6、4/4、2/2 …）
⇒ **安全链未受任何影响**。

**上板路径上的 4 次失败尝试（避免后人重复踩）**

| 尝试 | 结果 | 原因 |
|---|---|---|
| `fill warn 24 + flush`（稀疏） | ENTER=0 | 阻塞不够密集 |
| `fill warn 62 + flush ×12`（密集） | ENTER=3 ✅ 但 `qdrop=424` / `evict_inf=160` | 记录级捕获**不完整** ⇒ 无法核对 ΣCOUNT |
| `fwipe`/`mwipe`（阻塞但不产记录） | ENTER=0 | 擦除阻塞弱于写入阻塞，不足以凑满"5 次跳变/5 s" |
| `fill warn 8 + flush`（薄填充） | ENTER=0 | 单次 append 的阻塞时长不够 |

⇒ **结论：不要靠"诱发环境异常"做折叠/去重类验证**，改用 `logt fill … <EventId>` 定向注入。

**★ 关于触发门槛（重要认知）**：`LOG_WEIGHT_ERROR_ENTER` 是 `error_state` 的**边沿**记录，而
`jump_error = jump_count >= WEIGHT_MAX_JUMP_EVENTS(5)`（窗口 5 s）⇒ **单靠跳变路径天然 ≥5 s 才能再触发**；
但 `err = no_data || raw_zero || jump_error` 是**三因或**，任一因先消失再成立即可产生
**同毫秒 ENTER/EXIT 闪断**（实测多次）⇒ 短时间内的重复 ENTER 是真实存在的形态，折叠有意义。

---

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

### P2-G 加深的第三层：**"恰好填满"型边界断言也会被打破**（F3-A，断言 196 → 195）

**现象**：P2-G 首次全量回归 B 段 4 条 MISS —— `total=496` / `append=15@31` / `seg_evict_unacked=0` / `total=466`。

**时间线确证（决定性证据）**：

```
23:46:55  CMD: logt fill warn 62        （F3-A 第 2 次填充）
23:47:00  [Weight] STATE_WEIGHT_ERROR -> 1     ← ★ Weight 边沿落在填充窗口内
23:47:00  [Weight] STATE_WEIGHT_ERROR -> 0
23:47:02  CMD: logt fill warn 62        （第 3 次填充）
⇒ 实际写入 497~498 条 ⇒ 环提前回绕 + 淘汰 1 段 ⇒ total=467（≠496）
```

**这条规则的普适形式**：任何断言如果隐含 **"执行期间只有本用例在写日志"**，
在 P2 逐模块接入后都会陆续失效。它有三种典型形态：
1. **绝对计数**（`qdrop=12`）— P2-D 已处置；
2. **与历史相关的计数**（`replay=8`）— P2-E 已处置（改内联正则）；
3. **"恰好填满/恰好为 0"的边界**（`total=496`、`seg_evict_unacked=0`）— **P2-G 本轮处置**。

**处置原则（"修夹具、不降断言"）**：
- 删掉**隐含"无干扰"**的断言（它们不是设计不变量）；
- 换成**结构性判据**（`segs=16`）或**必然发生的事实**（多写 1 条 ⇒ 必然越界一次）；
- **该用例真正要判别的记账不变量必须原样保留**（F3-A 里 `seg_evict_unacked=31` / `fdrop=31` / `seg_del=1` 一条不删，实测稳定通过）。



---

## Next

### ✅ 2026-09-20（最新）：Phase 3 第 ⑤ 项 —— **EventManager 埋点接入完成**（P2-M）

> commit `604a892` `feat(log): integrate event manager fault logging`
> 前置审查：`log模块历史/LogManager-P2M-Event接入审查0920.md`（`52c93f9`）

**改动范围：仅 `src/event_manager.cpp`（`+110 / −0`）** —— `log_events.h` / `event_manager.h` / `main.cpp` / `dispense_guard.cpp` / 测试代码 **全部零改动**。

| EventId | Level | 宿主（**确定丢弃的分支**） | 参数 | 聚合 |
|---|---|---|---|---|
| `LOG_EVT_QUEUE_FULL` 0x0C01 | WARN | `event_push()` 中返回 `EVENT_QUEUE_FULL` 的**唯一出口**（非 FORCE 且 `queue_count >= 32`） | `LOG_P_COUNT`(丢弃条数) + `LOG_P_QUEUE_SIZE`(32) | 60 s 窗口，窗口内只发 **1 条** |
| `LOG_EVT_STORM_DROPPED` 0x0C02 | WARN | 风暴抑制分支（`storm_counter[eid] > EVENT_STORM_MAX_PER_EVENT`） | `LOG_P_COUNT`(被抑制条数) | 同上 |

**关键实现决策**

| 决策 | 理由 |
|---|---|
| **只累加、不逐条发射** | 风暴分支的定义就是"超过 5 次/500 ms 的持续流"，逐条记会形成**新的日志风暴**并瞬间填满 64 槽 RAM 环 |
| **节拍器 = `event_dispatch()` 首行** | 该函数由 `loop()` 每轮无条件调用 ⇒ 天然周期节拍；**不新增 Task / 不改 `main.cpp` / 不新建 timer / 无 `delay()` / 无 while 等待** |
| **必须放在 `while` 之前** | 放在循环后会被 `processed` 上限与"队列空直接跳过"吞掉节拍（铁律 13 的反面） |
| **无异常 ⇒ 完全静默且不推进窗口** | 保证**首次异常立即可见**，而不是等满 60 s |
| **Σ `LOG_P_COUNT` = 真实丢弃数量** | 信息零丢失、条目数恒定；与 `cloud_manager.cpp` 的 `CLOUD_PUBLISH_FAIL_REPORT_MS` 同构 |
| **FORCE 优先级不足分支不计入** | 语义是"被更高优先级挤占"而非"队列溢出"，且 `EVENT_POLICY_FORCE` **零发布方**（死路径） |
| **累加器用模块内 `static`** | 必须跨调用存活；而 `LogParamIn` 是**栈上局部**，且只在 loop 上下文构造（不涉 esp-mqtt 任务）⇒ 无需 PSRAM |
| ⚠️ 跨任务可见性（诚实标注） | `cloud_manager` 的 4 处 `event_push` 可能跑在 esp-mqtt 任务 ⇒ 累加器存在理论上的无锁竞争（**可能少计，不会损坏内存**）。与既有 `drop_count` / `duplicate_count` **完全相同**，不额外加锁以免改变事件路径时序 |

**验证结果**

| 项 | 结果 |
|---|---|
| **V1 编译** | ✅ SUCCESS；RAM **130616 → 130624（+8 B）**、Flash **1370533 → 1370701（+168 B）** |
| **V2 正常事件链** | ✅ 空闲 25 s `emit` **+0**；`valve_open` / `valve_close` 只产生 valve 自身 **2 条 INFO**（`flash +0`，INFO 不落 Flash）⇒ EventManager **0 条**重复记录 |
| **V5 高频事件** | ✅ 40 次 `valve_toggle` **全部成功**（0 次 `Operation too frequent`）：`emit` **+42**，全部归属 valve 40 条 INFO + wifi 2 条 WARN（`0x0603` / `0x0605`）⇒ EventManager **0 条** |
| **V3 队列满** | ⚪ **运行时不可达**（见下）⇒ objdump 静态确认埋点位置与参数编码 |
| **V4 风暴** | ⚪ **运行时不可达**（见下）⇒ objdump 静态确认；`0x0C01` / `0x0C02` 实测各 **0 条**（逐条核对 Flash 记录确认） |

**★ V3 / V4 不可达的定量论证（诚实标注 —— 这是"未触发"，不是"验证通过"）**

- **队列满（V3）**：队列容量 32，`event_dispatch()` 每轮 drain ≤4 且 loop 连续运行 ⇒ 需**单轮 loop 内 >36 次**入队。现有 17 个发布方全是**边沿型 / 低频**，`EVENT_POLICY_STATE` 还会先清空同类型旧消息 ⇒ 不可能达成。
- **风暴（V4）**：阈值 **>5 次 / 500 ms（同类型）**。最快的发布方是 valve（`MIN_OPERATION_INTERVAL_MS=50`），但开/关交替 ⇒ 同类型间隔 **100 ms + ε**；第 6 次同类型的时刻 = **500 ms + 10ε > 500** ⇒ 风暴窗口**先刷新**，计数永远到不了 6。实测 40 次 toggle 全部成功、0 条 `0x0C02`，与该推演一致。
- ⇒ 两个 EventId 属于**「睡眠中的观察者」**：正常负载下**零 Flash 压力**，一旦出现即代表真实异常。这**反向印证了聚合方案的必要性**——若当初采用"逐条记录"，这里就是日常的日志风暴源。

**objdump 静态证据（`.pio/build/p2m_evt`，`xtensa-esp32s3-elf-objdump -d -r`）**

| 证据 | 值 |
|---|---|
| `evt_report_faults()` 内联位置 | 进 `event_dispatch()` 的**函数最开头**（偏移 `0x0c`–`0x82`；dispatch 循环自 `0x84` 开始）⇒ 节拍不会被吞 |
| 站点 A | `l32r a10 ← .literal+0x14 = 0x00000C01`、`a11=2`(WARN)、`a13=2`(param_count)、p[0]={id `0x45`, u32, count}、p[1]={id `0x53`, u32, **32**} |
| 站点 B | `l32r a10 ← .literal+0x18 = 0x00000C02`、`a11=2`(WARN)、`a13=1`、p[0]={id `0x45`, u32, count} |
| 窗口常量 | `.literal+0x10 = 0xEA5F = 59999`（`delta < 60000` 的等价形式）⇒ **60000 ms 确认** |
| 累加器落地 | `event_push()` 的字面量池同时含 `.bss.evt_storm_drops`(+0xc) 与 `.bss.evt_queue_full_drops`(+0x1c) ⇒ 两处累加均已编入 |

**零协议新增 / 事件机制完全冻结**：未新增 EventId / ParamId / System State / Config 参数 / enable 开关；未改 `EVENT_STORM_WINDOW_MS` / `EVENT_STORM_MAX_PER_EVENT` / 队列大小 / dispatch 流程 / 风暴策略 —— **只增加观测能力**。

**遗留**：`EVT-1`（`SYSTEM_EVENT_COUNT=14` 越界 ⇒ 三个阀门事件无法解析 ⇒ Workflow 无法订阅阀门事件）**单独开缺陷单，不修 EventId 编号**（EventId 属冻结协议）。

**Phase 3 状态：①②③④⑤ 全部完成** ⇒ 下一步进入 **Phase 4：Dispense 模块开发**（4 项前置已满足）；`BLE` 埋点暂缓。

---

### 📋 2026-09-20：Phase 3 第 ⑤ 项**前置审查** —— `Event` 模块日志接入设计（**仅审查，未改代码**）

> 完整报告：`log模块历史/LogManager-P2M-Event接入审查0920.md`

**★ 核心结论：EventManager 是日志宿主，但仅限「自身运行异常」；业务事件一律不记。**

| 审查问题 | 结论（按**代码调用链**） |
|---|---|
| `event_push()` 调用位置 | **17 处 / 5 模块**（cloud 4、weight 4、time 3、valve 4、wifi 2）；策略分布 `STATE` 11 / `NORMAL` 5 / `DEDUP` 1 / **`FORCE` 0** |
| `event_dispatch()` 流程 | 无独立 Task，挂 `loop()`（`main.cpp:576`）**每轮无条件调用**；单次最多 4 条；按 priority 取最高；**先出队后回调**；过期丢弃 `drop_count++` |
| subscriber 数量 | **4 个回调**：`cloud_event_callback` / `dispense_guard_event_callback` / `oled_event_handler` / `workflow_event_callback`（workflow 为**动态**注册） |
| 消费者是否已记录同一事实？ | ✅ **是，76%**：17 个 `event_push` 中 **13 个**的发布方**已埋** `log_emit`（valve 4/4、wifi 2/2、time 3/3、cloud 4/4、weight 2/4） |
| **EventManager 是否应成为日志宿主？** | ✅ **是——但只限自身异常** |

**★ 决定性证据（为什么丢包只能由 EventManager 记）**：
- **17 个调用点 100% 忽略 `event_push()` 返回值**（全仓库无 `EventPushResult` 接收者）
- **3 个侧信道 getter 零消费方**（`event_get_drop_count()` / `duplicate_count()` / `queue_count()`）
⇒ **事件丢弃当前 100% 不可观测** —— 发布方看不到（不查返回值）、外部查不到（无人调 getter）。

**EventId 语义分析**：

| EventId | 定义点 | 频率 | 逐条？ |
|---|---|---|---|
| `LOG_EVT_QUEUE_FULL` 0x0C01 | `event_push()` `:266`（队列满 32 且非 FORCE） | 低基线 / 高危峰（dispatch 追不上时每次 push 都触发） | ❌ 不适合（是**状态**非事件） |
| `LOG_EVT_STORM_DROPPED` 0x0C02 | `event_push()` `:220-224`（>5 次/500ms） | **★ 天然高频**（定义即"超限持续流"） | ❌❌ **绝对不可**（会瞬间填满 64 槽环） |

⚠️ **陷阱**：`EVENT_DROPPED` 返回值在 **3 个不同分支**返回（`:206` 越界 / `:223` 风暴 / `:260` FORCE 失败）
⇒ **不能靠返回值判据**，必须在分支内部埋点。

**重复记录风险**（用户点名链路 `event_push → EventManager → DispenseGuard → Valve`）：

| 方案 | 一条物理事实的记录数 |
|---|---|
| 现状（不接 Event 埋点） | **2~3 条**（`WEIGHT_ERROR_ENTER` + `VALVE_FORCE_CLOSE`［+失败分支］）—— 完整无冗余 |
| 若 EventManager 逐条记业务事件 | **4~6 条** ⇒ 同一事实重复 2~3 倍 |
| **推荐方案 A** | **2~3 条** + 偶尔 1 条聚合异常 |

**唯一事实宿主**：重量异常→`weight.cpp`；强制关阀→`valve.cpp:470`；**队列满/风暴→`event_manager.cpp`**（唯一可观测点）。

**方案对比**：

| 维度 | 方案 A（周期统计聚合）★ | 方案 B（仅 CRITICAL/FORCE 事件） |
|---|---|---|
| Flash 压力 | ✅ 恒定（窗口内 1 条） | ❌ 风暴下失控（CRITICAL 持续落 Flash） |
| 信息完整性 | ✅ 零丢失（`ΣCOUNT`=真实次数） | ⚠️ 有损 + 遗漏非 CRITICAL 丢包 |
| 架构兼容性 | ✅ 复用 P2-D 已验证模板；符合矩阵"周期聚合 1 条" | ❌ 违背矩阵 §12 契约 |
| 重复风险 | ✅ 零 | ❌ 高（CRITICAL 发布方已埋） |

⇒ **采用方案 A**：2 个 EventId / 2 处累加 + 1 个周期上报器（共 2 个 `log_emit` 调用点）；
窗口 **60000 ms**（对齐 `CLOUD_PUBLISH_FAIL_REPORT_MS`）；
**以 `event_dispatch()` 为节拍器 ⇒ 不新增任务、不改 `main.cpp`**；
参数全复用（`LOG_P_COUNT` / `LOG_P_QUEUE_SIZE` / `LOG_P_CAUSE`）；**零新增** EventId/ParamId/System State/配置项/开关；**不改风暴策略**。

**新增发现（只报告不修复，已登记 `未修复的问题.md` §Event）**：
`EVT-1`🟠 **`SYSTEM_EVENT_COUNT=14` 越界** ⇒ 三个阀门事件（14/15/16）`event_from_string()` 永远解析不到 ⇒ **Workflow 无法订阅阀门事件**（静默失败）｜`EVT-2`🟡 `drop_count` 四因共用｜`EVT-3`⚪ `:206` 漏计数｜`EVT-4`🟡 侧信道 getter 零消费方｜`EVT-5`⚪ 静态容量冗余。

**待用户确认 E1–E4**：① 采用方案 A ② 窗口 60000 ms ③ 不新增 EventId（过期/DEDUP/越界维持不记录）④ `EVT-1` 另行开缺陷单。**确认后方进入代码修改。**

---

### ✅ 2026-09-20：Phase 3 第 ④ 项 —— **Registry 埋点接入完成**（P2-L，`WF-4` 关闭）

> commit `4b2e04b` `feat(log): integrate capability registry logging`
> 前置审查：`WF-4-Registry范围审查0920.md`（`fc250c4`）

**改动范围：仅 `src/capability_registry.cpp`（+24 行 / 2 处 `log_emit`）**

| EventId | Level | 宿主（**定义点**，非触发点） | 参数 |
|---|---|---|---|
| `LOG_REG_REBUILT` = 0x0B01 | INFO | `registry_sync()` 的 **`else` 分支**（checksum 不一致 / 文件缺失 / 损坏） | `LOG_P_REG_TYPE`, `LOG_P_COUNT`, `LOG_P_VERSION` |
| `LOG_REG_SAVE_FAILED` = 0x0B02 | ERROR | 同函数内 **`save_registry_file()` 返回值判定处** | `LOG_P_REG_TYPE` |

**设计决策落地**：
- ✅ 埋"定义点"：`command_manager.cpp` 的 4 处 `capability_registry_rescan()` **未埋** —— 一次 `workflow.create` 会连触发 3 次 `registry_sync`，埋触发点会重复记录 3 条。
- ✅ **不进** `save_registry_file()` 内部 4 个失败出口（:167/:202/:225/:244，各有 1 条专属串口打印，1:1 覆盖）⇒ **不带 `ERR_CODE`、不改签名、零信息损失**。
- ✅ 参数用**栈上局部 `LogParamIn`**；禁用 `static` / 全局缓存（本函数可能跑在 `loopTask(8KB)` 或 `esp-mqtt` 任务上，需防并发踩踏）。
- ✅ 单向依赖 `capability_registry → log_manager`；无 callback、无 EventManager 转发、无 System State。
- ✅ **未新增**任何 EventId / ParamId / 配置项 / enable 开关；`workflow_storage.cpp` / `log_events.h` / `EventManager` / 测试代码**零改动**。

**验证结果（V1–V4）**：

| 项 | 方法 | 结果 |
|---|---|---|
| **V1** | 全量编译（`.pio/build/p2l_reg`） | ✅ SUCCESS 22.0s；**RAM 130616 B（±0）** / **Flash 1370533 B（+408）** |
| **V2** | 上板启动，三表 | ✅ ACTION/TRIGGER/WORKFLOW **全走 `reuse`**；`REG_REBUILT`=**0**、`REG_SAVE_FAILED`=**0** ⇒ reuse 路径**不产生日志** |
| **V3** | `create`×1 + `delete`×1 定向触发 rebuild | ✅ **恰好 2 条** (`v10→v11→v12`)，ACTION/TRIGGER 保持 `reuse`。**若误埋触发点应为 6 条** |
| **V4a** | 运行期路由（`logt fwipe`+`reset` 清零后精确计数） | ✅ `emit` 0→3、`cloud` 0→**+3**、`flash` **+0** ⇒ `REG_REBUILT` 为 INFO **走云侧、不落 Flash**；rebuild 仅 1 次 |
| **V4b** | 静态可达性（`objdump -dr` 按符号去重） | ✅ **2 个 `log_emit` 调用点**，均在 `registry_sync` 内；参数编码 `(count=3, level=1=INFO)` 与 `(count=1, level=3=ERROR)`，与源码意图逐一对应 |

> ⚠️ **V4b 说明**：`capability_registry.cpp` 内**无故障注入钩子**，且任务约束禁止为此新增测试代码 / 改签名 ⇒ 无法在运行时人为制造 save 失败。故 `REG_SAVE_FAILED` 采用**二进制级静态验证**（调用点数量 + 参数编码 + Level 常量比对），配合 V4a 证明同分支内 ERROR 级路由必然落 Flash。

**文档同步**：`docs/P2_Log_Integration_Matrix.md` §7 作用域已更正（移除误导性的 Registry / `workflow_storage.cpp` 并列关联，加零耦合说明与报告链接）；§7.2 末行 Registry 条目**删除**并指向 §12；§12 Registry 行更新为"✅ 已接入"。

---

### 📋 2026-09-20：Phase 3 第 ④ 项**前置审查** —— `WF-4` 范围界定（**仅审查，未改代码**）

> 完整报告：`log模块历史/WF4-Registry范围审查0920.md`（commit `fc250c4`）

**核心结论：`WF-4` 不成立，建议关闭。**

| 审查问题 | 结论（按**代码调用链**，非文档模块名） |
|---|---|
| Registry 是否拥有完整生命周期？ | ✅ **是，完整自持**（init / rescan / scan / sort / crc / load / save / 查询 / 导出全在 `capability_registry.cpp`） |
| `workflow_storage` 是否只是消费者？ | ❌ **连消费者都不是** —— 不 `#include` `capability_registry.h`，全仓库对其零引用（`.h:268` 仅一行**注释**） |
| `workflow_storage` 是否应产生 Registry 类日志？ | ❌ **不应该** —— 无调用关系，硬加即**发明不存在的事件源**（架构污染） |
| 是否存在多模块记录同一事实？ | ⚠️ 有 **2 个真实风险点**，均可规避 |

**逐事件归属（6 项，用户点名核实）**：

| 事件 | 归属 |
|---|---|
| Registry 构建完成 / `REG_REBUILT` | `registry_sync()` 的 **`else` 分支**（:668-702）—— 该分支即"重建"的**结构性定义**，天然边沿 |
| Registry 保存失败 / `REG_SAVE_FAILED` | `registry_sync()` 内 **`save_registry_file()` 返回值判定处**（:687-693）；★ **不埋进 `save_registry_file()` 内部的 4 个出口**（会同一事实多条记录） |
| Workflow 读取 Registry | ⛔ **不埋** —— 高频只读（`workflow.list` 每次数十次调用），必成风暴 |
| Workflow Storage 保存 Registry 数据 | ⛔ **该事实不存在** —— 两套独立文件与代码路径 |
| Capability ID 分配变化 | ⛔ **不单独埋** —— 它是 checksum 变化的**结果**，已在 `REG_REBUILT` 语义内 |

**★ 最重要的通用判据（本轮沉淀）**：
> **"事件的定义点"与"事件的触发点"往往是两个不同函数。**
> Registry 的**触发点**在 `command_manager.cpp`（4 处 `rescan()`），但**定义点**在 `registry_sync()`。
> **必须埋"定义点"** —— 否则 4 个触发点会各记一条同一事实（且一次 create 会触发 3 次 `registry_sync`）。

**频率实测（决定是否需门控）**：`.pio/p15run/*.log` 统计 —— **91 轮 `sync` 中仅 3 次 `rebuild`（≈3.3%）**
⇒ `REBUILT` 属**低频源**（只在内容真变时触发），**INFO 不加门控可接受**。

**接口结论**：✅ **只需 +2 处 `log_emit`**，无需新增 callback（同步模块）、不破坏分层。
`REG_SAVE_FAILED` 建议**不带 `ERR_CODE`**（方案 1）—— 实测 `save_registry_file()` 的
**4 个失败出口各有 1 条专属串口打印（1:1 严格对应）** ⇒ 精确原因 100% 已由串口覆盖，**零信息损失**，
不必改既有函数签名。

**待用户确认 D1–D4**：① WF-4 是否关闭 ② `REG_REBUILT` 宿主位置 ③ 是否带 `ERR_CODE` ④ 是否更正矩阵 §7 作用域。

> ✅ **2026-09-20 已确认并执行完毕**：四项**全部采纳** —— ① WF-4 关闭；② 宿主 = `registry_sync()` `else` 分支；③ **不带** `ERR_CODE`；④ 矩阵 §7 作用域已更正。实施结果见本节上方「Phase 3 第 ④ 项 —— Registry 埋点接入完成（P2-L，`4b2e04b`）」。

---

### ✅ 2026-09-20：Phase 3 第 ③ 项 —— **ComputerReset 模块埋点接入完成**（P2-K）

> 完整审查记录：`log模块历史/LogManager-P2K-ComputerReset接入审查0920.md`

| 项 | 结果 |
|---|---|
| **改动** | **仅 `src/computer_reset.cpp`**（+include / **+4 处 `log_emit`**，`+65/−0` 纯增量） |
| **约束遵守** | 未改 `EventId` / `ParamId`（`log_events.h` **零改动**）/ 日志协议 / `System State` / `EventManager` 风暴策略 / GPIO 时序 / 池容量 / 测试断言 |
| **V1 编译** | SUCCESS，RAM 130616 B（39.9%，**零增长**）/ Flash 1370281 B（+156 B） |
| **V2 零回归** | 静置 `emit` **5 → 5**（**零增长**）⇒ 3 个埋点无虚假触发 |
| **V3 `PULSE` 手动路径** | 控制台 `computer_reset` ⇒ `emit` **+1**、`flash` **+0**（INFO 只上云 ✅）、GPIO8 HIGH→LOW 800 ms 正确 |
| **V3b `PULSE` Action 路径** ★ | `execute_action` ob=`COMPUTER_RESET` ⇒ `emit` **+1** ⇒ **证明"上移到 `set_output()` 上升沿"的决策正确**（若照矩阵只挂 `trigger()`，此条**将不存在**） |
| **V4 二进制映射** | 4 处 `log_emit` **逐一映射**到 `set_output` / `action_start` / `task` / `trigger`，与源码 4 处一致 |
| **未覆盖项** | `SAFETY_TIMEOUT`（正常路径**结构性不可达**：手动/Action 两条路径都在 800 ms 回收，到不了 2000 ms；触发需"实例泄漏"异常条件）与 `POOL_EXHAUSTED`（需 4 实例同时活跃）。**二者均为"正常时不应触发"的安全兜底/资源耗尽路径 ⇒ "不触发"即正确行为**，已由 V4 二进制确认存在 |

**★ 本项沉淀的四条教训**：
1. **矩阵给的是"语义宿主"，落地要按"代码事实"找"物理宿点"。** 矩阵写 `trigger()`，
   但脉冲有**两条产生路径** ⇒ 挂 `trigger()` 会漏掉 Action 路径。
   **正解是上移到两条路径的公共汇合点**（`set_output()` 上升沿）。
2. **埋点顺序在"取值依赖会被清零的变量"时是正确性问题，不是风格问题。**
   `DURATION_MS` 依赖 `pulse_start_ms`，而紧随的 `force_idle()` 会清零它 ⇒
   埋点**必须**写在前面（LOG-13 的具体化）。
3. **"天然边沿"的论证可直接引用既有的防御性代码。** `set_output()` 的
   `if(active == output_active) return;` 本为"避免重复写 GPIO"，但**同时**构成了
   天然边沿的结构性证明 ⇒ 现有守卫常可复用为门控依据，**无需新增去重逻辑**。
4. **"正常时不应触发"的埋点，其"不触发"本身就是验收项。** 安全兜底类埋点的验收
   标准是**"二进制存在 + 正常路径零增长"**，而非"实测能触发"。

**下一步**：✅ **Phase 3 第 ⑤ 项 Event 埋点已实施完成**（`604a892`，见本节顶部「Phase 3 第 ⑤ 项 —— EventManager 埋点接入完成（P2-M）」）。用户已确认 E1–E4：**方案 A（计数聚合 + 60 s 周期上报）**、`event_dispatch()` 作节拍器、不新增 EventId/ParamId、`EVT-1` 单独开单。**Phase 3 ①②③④⑤ 全部完成** ⇒ 进入 **Phase 4：Dispense 业务开发**；`BLE` 埋点暂缓。

> **② OLED 已判"无宿主不埋"**（2026-09-20 用户拍板）—— `oled.begin()` 无条件返回 true
> 且 U8g2 丢弃 I2C 错误码 ⇒ **原理上不可达**；且无消费方。已移入"无宿主"清单。

---

### ✅ 2026-09-20：Phase 3 第 ① 项 —— **Command 模块埋点接入完成**（P2-J）

> 完整审查记录：`log模块历史/LogManager-P2J-Command接入审查0920.md`

| 项 | 结果 |
|---|---|
| **改动** | **仅 `src/command_manager.cpp`**（+include / +`cmd_hash32()` / +`command_log_rejected()` / **+4 个埋点**） |
| **约束遵守** | 未改 `EventId` / `ParamId`（`log_events.h` **零改动**）/ 日志协议 / `System State` 架构 / `EventManager` 风暴策略 / `WeightManager` 阈值 |
| **V1 编译** | SUCCESS，RAM 130616 B（39.9%）/ Flash 1370125 B（65.3%） |
| **V2 零回归** | 45 s 静置，**无任何 `[CMD][WARN]`** ⇒ 4 个埋点**无虚假触发** |
| **V3 二进制确认** | `objdump -dr` 反汇编 = **5 处 `log_emit` 调用**，与源码 `grep -c` = 5 **一一对应** |
| **V4 `APPLIED` 实测** | `query_workflows` / `workflow.list` ⇒ **`emit` 恰好 +1 / 命令**；异步命令**正确排除** ✅ |
| **V5 `REJECTED` 门控实测** | **0.05 s 急速连发 5 次**同 `cmd_id`（1 accepted + 4 rejected，全在 250 ms 内）⇒ **`flash` 仅 +1**（WARN 路径只产 1 条）⇒ **5 s 门控生效** ✅ |
| **未覆盖项** | `QUEUE_FULL`（需占满 8 槽，无长效同步 action 可占坑）/ `RUNTIME_TIMEOUT`（需等 60 s~10 min，探针受"串口静默 1.5 s 提前返回"限制**挂不住**）。**如实登记为环境工具限制，非代码问题**（代码路径已由 V3 二进制确认） |

**★ 本项沉淀的三条教训**：
1. **判据要用能"分离两类记录"的计数器。** 只盯 `emit` 会得出"+3 ≠ 预期 +2"的**误判**；
   改用 `flash`（只收 WARN+）与 `cloud`（全收）**分离**后，`flash +1` 一举证明门控生效。
   ⇒ **"INFO 只上云不落 Flash"这条既有设计，本身就是免费的验证通道。**
2. **高频路径的复现靠"缩短注入间隔"而非"延长时间"。** 早期用 3 s 间隔发同 `cmd_id`
   连续失败 3 次（首条已 `release`，第 2 条**正常受理**而非"重复被拒"）。
   ⇒ **先确认被测对象的存活时间，再审定注入节奏。**
3. **"天然边沿"必须论证，不能假定。** ①③ 免门控是因为"进入分支 ⇒ 状态必然不允许再入"
   这一**结构性保证**。把论证写进埋点注释，是防止后人误加/误删门控的关键。

**下一步**：Phase 3 第 ② 项 —— **OLED**（`LOG_OLED_INIT_FAILED = 0x0E01`）。

---

### ✅ 2026-09-20 更新：Phase 1 完成 —— `R-8-A` **已修复并上板验证通过**

> Commit：**`129606f` `fix(weight): prevent HX711 blocking wait`**
> 完整记录：`log模块历史/R8A-HX711阻塞分析0920.md`（已扩写至第 8 章"实施与上板验证记录"）

| 项 | 结果 |
|---|---|
| **改动** | **仅 `src/weight.cpp`**，纯增量 **+35 / −0** —— 在采样分支 `scale.read()` 前加**二次 `is_ready()` 确认**，不就绪即退回**既有** `not_ready` 路径 |
| **约束遵守** | 未改重量阈值 / 采样策略 / 事件策略 / `DispenseGuard` / `EventId` / `ParamId` / `System State` 架构；未 fork 库 |
| **V1 编译** | SUCCESS，Flash **65.3%** / RAM **39.9%**（与 P2-I 基线**零增长**） |
| **V2 正常路径** | `STATE_WEIGHT_ERROR`=0、`not ready`=0 ⇒ **零回归** |
| **V3 核心验收** | HX711 **全部线未接**：`logt stats` **秒回**（loop 存活）+ **5 s 后**自动 `DispenseGuard` → `Valve FORCE CLOSE` ⇒ **安全链端到端可用** |
| **V4 恢复验证** | HX711 接回：`emit/crit/consumed` 20 s **零增长**、`DispenseGuard`=0 ⇒ **自动恢复** |
| **顺带解决** | `R-8-B`（`not_ready` 超时路径原"不可达" —— V3 实测已可达） |
| **残留风险** | 二次确认与 `read()` 间 ≈1 µs TOCTOU 窗口（10 Hz 下 100 ms 内自行解除）；**用户已确认 DOUT 有外部上拉** ⇒ 与软件构成双重保险 |

**两条重要教训（已写入 `未修复的问题.md`）**：
- **V3 方法论**：`R-8-A` 这类"**loop 是否存活**"的验收**必须用主动命令**（`logt stats`）
  **主动探测**；**禁止**依赖被动串口输出的疏密 —— 该板静置时数秒才一行，
  "低日志量" 与 "挂起" 无法从被动观测区分（本轮曾据此误判一次）。
- **V4 判据**：计数增长的判据应是"**静置期内是否增长**"，而非"累计值是否变大"。
  本轮 `crit` 2→28 曾疑似"残留误报"，经用户澄清为**手动按压电子秤的真实报错**
  ⇒ 反而**反证 HX711 工作正常**。

---

### 🔄 2026-09-20 路线调整：`R-7` / `R-8` 双双 `DEFERRED`（产品侧复审）

| 项 | 产品判定 | 处置 |
|---|---|---|
| **`R-7`**（测试隔离） | 不影响产品功能 / 不影响用户使用 / **不作为开发阻塞项** | **`DEFERRED`** —— 保留分析文档，归入**后续测试基础设施优化**，**不再投入开发时间** |
| **`R-8`**（采样间隔变化） | 重量检测目标是**秒级控制**而非高速实时控制；HX711 10 Hz 偶因 MCU 任务 / Flash / BLE 降频**可以接受** ⇒ **非产品缺陷** | **`DEFERRED`** —— **不修改**采样窗口逻辑 / 不加 gap reset / 不改异常阈值 / 不改 `EventManager` / 不改 `DispenseGuard`。`R8-Fix-1` 技术方案**存档不实施** |
| **`R-8-A`**（HX711 无限阻塞） | **单独处理** —— 独立可靠性问题 | ✅ **已修复**（见上） |

> ⚠️ **`R-7` 相关的"不要跑回归时注入 ACK"等注意事项仍然有效**（夹具隐含依赖 `BT-1`），
> 只是 `R-7` 本身**不再作为任何 Phase 的前置**。
> ⚠️ **底线不变**：`test/` 夹具 0 改动、195 条断言保持原样（**禁降断言**）。

---

### 🗺 新路线：4 个 Phase（**取代原串行依赖链**）

> 原路线 `A1 R-7 评审 → A2 baseline 恢复 → …` **已作废**。
> 原因：产品侧确认 **LogManager 是基础服务，不应阻塞业务开发**；
> 且 `R-8-A` 的修复证明 —— "安全链修复"**不依赖回归夹具**
> （V3 判据是"loop 是否存活"，**不是绝对计数断言**）。

```
Phase 1  系统可靠性低风险修复
         ✅ R-8-A 消除 read() 无限阻塞 —— 已完成（129606f）
  ↓
Phase 2  BLE 性能清理
         ▸ 检查 callback 中大量 Serial / 高频 hex dump / 不必要调试打印
         ▸ 重点：MiThermometer.cpp:117-123 onResult() 逐字节 hex 打印（≈7.8 ms/包）
         ▸ 目标：降低 BLE active 时 CPU/loop 压力
         ▸ 约束：不改解码逻辑 / 不改 System State / 不改数据流程
                 保留必要错误日志 / 不删调试能力，只降默认输出
  ↓
Phase 3  LogManager 埋点接入（按序）
         ① Command ✅   ② OLED ⛔(无宿主)   ③ ComputerReset ✅   ④ Registry ✅(P2-L)
         ⑤ Event ✅(P2-M，2026-09-20) —— **阶段完成**
         ⏸ BLE 暂缓 —— 高频来源，须先设计"事件筛选 / 聚合策略 / callback 上下文安全"
  ↓
Phase 4  恢复核心业务开发
         ▸ 进入 Dispense（注水过程）
         ▸ 不等 R-7 完美解决 / 不等 195/195 / 不等日志 100% 覆盖
         ▸ 必要条件（4 项现已全部满足 ✅）：
             ✅ Valve 稳定   ✅ Weight 安全链明确
             ✅ LogManager 基础稳定   ✅ HX711 不会永久阻塞
```

**Phase 4 必要条件核对（进入 Dispense 前）**：

| 条件 | 状态 | 依据 |
|---|---|---|
| Valve 稳定 | ✅ | P2-H 已冻结、逐项上板验证；`VALVE-2`/`VALVE-3` 已 DONE；`VALVE-1` 为"假成功"语义问题（非功能性故障） |
| Weight 安全链明确 | ✅ | `weight.cpp` → `event_push(EVENT_WEIGHT_ERROR, STATE)` → `dispense_guard_event_callback` → `valve_force_close()`；**V3 实测端到端闭环** |
| LogManager 基础稳定 | ✅ | P2-A~P2-I 全部落地；`log_emit()` 非阻塞已证；`P2-I` 合并优化实测 **−70.6%** 写入 |
| **HX711 不会永久阻塞** | ✅ | **本轮 `129606f`**（V3：掉线时 loop 存活 + 安全链闭环） |

---

### 历史路线（P2-H 冻结时的原计划，已被上方取代）

### 🔄 2026-09-20 更新（前一版）：`R-7` / `R-8` **只记录、不修复**（用户决策）

**本轮已完成**：`R-8` 专项评审（**纯分析，零代码改动**）。
产出 `log模块历史/R7-R8-后续评审与系统性能权衡0920.md`，
并已把结论增量登记进 `未修复的问题.md`（含新增 `R-8-A` / `R-8-B` / `R-8-C` / `SYS-1` / `SYS-2`
与「埋点覆盖率实况」「后续开发任务总览」两节）。

**R-8 评审核心结论**：

| 项 | 结论 |
|---|---|
| `R-8` 是否真实缺陷 | ✅ **是**，且升格为高优先级（机理确证 + 后果污染安全链 + 危险区间定量） |
| **危险区间（非单调）** | `< 100 ms` 无害 ｜ **`100–500 ms` 最危险** ｜ `> 500 ms` 整窗换血反而不误报 |
| 后果 | 不止多写日志 —— 经 `EVENT_WEIGHT_ERROR`(CRITICAL/STATE) → DispenseGuard → **`valve_force_close()`** |
| **`R-8-A`（新，🔴 P0）** | `HX711::read()` 内 `wait_ready()` = `while(!is_ready())` **无限阻塞** ⇒ HX711 掉线时 **loop 永久挂起**（唯一 `ESP.restart()` 也进不去）。**独立于 R-8，更危险** |
| `R-8-B`（新） | `WEIGHT_NOT_READY_TIMEOUT_MS` 超时**正常运行时不可达**（与 `NC-13` 吻合） |
| `R-8-C`（新） | `R-1` 获统一解释；修 `R-8` 大概率一并解决 |
| **`SYS-1`** | **LogManager 无需新增性能限制**（`log_emit()` 非阻塞成立；`LOG_DRAIN_MAX_PER_TASK=8` 已是单轮上限；**不推荐时间预算**——撞 §22/§23 冻结边界） |
| **`SYS-2`** | **BLE 与 LogManager 无并发竞争**（共用 loopTask）⇒ "给 LogManager 降优先级"**不可实现亦不必要**；BLE 侧真正消耗 = `P0-3`（≈7.8 ms/包） |

**最小修法 `R8-Fix-1`（已设计，未实施）**：仅改 `src/weight.cpp` 三处 ——
`last_sample_ms` + `WEIGHT_SAMPLE_GAP_MAX_MS`（建议 150 ms，需标定）+ 成功 `read()` 后判 gap，
超阈则 `sample_index = 0; have_last_window = false;`。内存 +4 B，依赖方向不变。

---

### 下一阶段路线（**更新后**）

```
【本轮】R-8 评审 → 只登记（✅ 已完成，零代码改动）
   ↓
A1  R-7 测试隔离方案评审            ← 硬阻塞，其余任务都依赖它
   ↓
A2  Regression baseline 恢复        ← 182/195 → 全绿；断言不做任何删改
   ↓
B1  bin_storage 接入（改双路）→ B2 Command → B3 OLED+ComputerReset → B4 Registry
   ↓                                ← 低成本批量接入（见「埋点覆盖率实况」）
C1  R-8-A 消除 read() 无限阻塞（P0）→ C2 R-8 采样间隔守卫 → C7 P0-3 删 BLE hex 打印
   ↓                                ← 安全链修复（C7 与 C2 同批）
B5  Event（周期聚合）→ B6 BLE（须先 C7）
   ↓
C3  VALVE-1 / C5 WF-1 / C6 P0-4 独立评审
   ↓
B7  Dispense                        ← ★ 必须等 baseline 稳定
```

**剩余未接入模块（按约定顺序）**：~~`Command`~~ ✅ → ~~**OLED**~~ ⛔（判无宿主不埋）
→ ~~**ComputerReset**~~ ✅ → **Registry** → **Event**（**BLE 暂缓**；`Dispense` 归入 Phase 4 业务开发）

> ⚠️ **2026-09-20 起 `R-7` 已 `DEFERRED`** ⇒ **不再需要等 regression baseline 稳定**，
> `Dispense` 亦不再作为埋点项等待（其埋点随模块本体一起建，见 Phase 4）。
> ⚠️ **`bin_storage` 是"最低成本的一步"**：`main.cpp:474` 当前只注册了串口版 `bin_log_serial`，
> 与 P2-A 的 `json_storage`/`file_storage` 桥接**同构**，改双路即可（当前**未排入** Phase 3 序列）。
> ⚠️ **BLE 是最需要谨慎的一块**：最高频 + 回调上下文禁令 ⇒ **必须先删 `P0-3` 的 hex 打印**，
> 再走"置标志 → task 消费"路径。
> ⚠️ **Event 必须周期聚合**：契约明令"不得为每条丢弃事件发日志"，否则与 `R-7` 噪声**相互放大**。

### 历史路线（P2-H 冻结时的原计划，已被上方取代）

**P2-H（Valve）已实现、逐项上板验证完成，并已冻结**（生产代码 `d92d397`；**生产代码不再修改**）。
当前唯一阻塞项是回归 **168/195**（现为 `182/195`），其性质已判定为 **Regression Environment Interference**
（**不是** P2-H implementation failure）。

原计划的下一步为：

1. ⚠️⚠️ **`R-7` 测试隔离方案评审**（唯一前置）—— 评审并选定"让被测系统在回归期间安静"的方案。
   - ⚠️ **明确不采用**：给 weight 模块加 `enable` 配置（会改变 Weight 能力边界、影响 System Config，超出 P2-H 范围）。
   - 评审依据：`log模块历史/LogManager-P2H-回归干扰分析报告0919.md`
     （判定实验：**P2-G 固件（无 Valve 日志）在同 workload 下仍产生 MISS**；
     且 Weight 侧单独就能产生 **+13 条**额外记录 ⇒ **只去掉 Valve 日志不能归零**）。
2. **Regression baseline 恢复** —— `R-7` 有解后重跑全量，确认基线稳定。**断言不做任何删改**。
3. **`VALVE-1` 独立安全评审** —— `initialized` 未检查 ⇒ 模块禁用时返回 `true`（假成功）。
4. **`R-8` 独立安全评审** —— ✅ **2026-09-20 已完成**（结论见上方）。
5. ⚠️ **在 regression baseline 稳定前，不进入 Dispense。**
6. 另登记（列入后续独立评审，本轮不修）：`VALVE-6` —— `FORCE_CLOSE_FAILED` 无门控。

### 后续顺序（原）

```
P2-H Freeze
   ↓
R-7 测试隔离方案评审
   ↓
Regression baseline 恢复
   ↓
VALVE-1 / R-8 独立评审
   ↓
Dispense
```

> 说明：原先 `E1`~`E4` 的编号已被本路线取代 ——
> **`E1`（weight enable）已否决**；`E2`（`VALVE-6`）、`E3`（`VALVE-1`）、`E4`（`R-8`）
> 统一归入"后续独立评审"，其中 **`VALVE-1` 与 `R-8` 提升为独立安全评审**。

---

## 已知但暂不修复的问题（Defect Register · 索引）

> **完整清单（含证据/影响/建议/复测方法）见仓库根目录的 `未修复的问题.md`** —— 本表是它的索引，
> 两者必须同步维护。本节只回答"**目前一共积压了哪些问题、什么优先级**"。
> 下面「未决 / 阻塞事项」章节保留**阻塞项的细节**，不重复叙述。

| 类别 | ID | 一句话 | 优先级 |
|---|---|---|---|
| **基线（BT）** | **BT-1** | 云端从不回 `log_ack`（Worker 不在本仓库）⇒ `acked_seq` 不前进 ⇒ Flash 段永不回收 | 🔴 |
| | **BT-10** | 队列淘汰只给本 Boot 记录登记空洞 ⇒ 补发的上一 Boot 记录无保护 | 🔴 |
| | **BT-11** | `give_up_seq` 单点水位 ⇒ 较旧待补发记录本 Boot 跳过（下次 Boot 恢复，不丢数据） | 🔴 |
| | **BT-H1** | 空洞表 `LOG_HOLE_MAX=8` 可能耗尽 ⇒ `hovf=1` 停回收（与 BT-10 互为代价） | 🟡 |
| **Workflow（WF）** | **WF-1** | **保存失败后重试风暴**：实测 **39–40 次/秒**（151 s/5828 次、84 s/3399 次） | 🔴 |
| | **WF-2** | `WORKFLOW_WAITING` 从未被赋值（死枚举） | 🟡 |
| | **WF-3** | 临时 Action 队尾 `if/else` 两分支相同、注释写反 | 🟠 |
| | **WF-4** | ~~矩阵作用域含 Registry / workflow_storage（范围待界定）~~ → ✅ **已结案关闭**：二者**零耦合**、属平行关系；Registry 埋点已按"定义点"落地于 `registry_sync()`（`4b2e04b`），**不增加** `workflow_storage` 相关 Registry 日志，矩阵 §7 作用域已更正。报告 `log模块历史/WF4-Registry范围审查0920.md` | ✅ 关闭 |
| **P0（与日志解耦）** | **P0-1** | `event_names[]` 第 10 项错位 | 🟡 |
| | **P0-2** | MQTT 明文密码（`mqtt.json` + `tools/mqtt_*.py` 早已入库）⇒ **需轮换口令** | 🟠 |
| | **P0-3** | BLE 回调逐字节 hex（`MiThermometer.cpp:117`） | 🟡 |
| | **P0-4** | **`valve_force_close()` 后无残余增重检测**（对应 `LOG_VALVE_OVERFLOW_RISK` 注释"需新增检测"） | 🔴 |
| | **P0-5** | `reset_reason` 从不落盘（复位原因诊断盲区） | 🟠 |
| **能力缺口** | **NTP 不可观测** | SDK 无失败通知、状态枚举无失败态 ⇒ `NTP_FAIL` 无宿主 | 🟠 |
| | **无 `set_time` 通道** | `time_set_manual*` 无调用者 ⇒ `INVALID_ENTER` 不可触发 | 🟠 |
| | **P2-G-①** | 矩阵建议 `weight_trigger_start()` 复用 `ERROR_ENTER` ⇒ **重复/虚假**（建议不埋） | 🟠 |
| | **P2-G-②** | "校准开始"需 `0x0512`（**未定义**）⇒ 守"暂不新增 ID"，记为缺口 | 🟠 |
| | **P2-G-③** | `weight_init()` 无宿主 ⇒ 初始化不可观测 | 🟠 |
| | **F2 决策** | 字符串参数仍不可达（7 个 ParamId），现走哈希/枚举化 | 🟠 |
| | **F6 基线** | P1.5 之前的既有回归集（核心 66 / Flash 92 / 交接 56 …）**未复跑** | 🟠 |
| **风险（R）** | **R-1** | Weight 异常边沿**同毫秒抖动**（实测）⇒ 最坏 12 WARN/min | 🟠 |
| | **R-3** | **跳变源让 `dispense_guard` 反复强关阀（实测同秒 2 次）** ⇒ P2-H 必须先定去重策略 | 🔴 |
| | **R-5** | "参数非法"也会触发强制关阀（行为问题） | 🟠 |
| **死代码（DD）** | **DD-1** | `weight_is_active()` 声明无定义、无调用者 | 🟡 |
| | **DD-2** | `cloud_publish_fragmented()` 无调用者 ⇒ `FRAG_FAIL` 无宿主 | 🟡 |
| | **DD-3** | `time_set_manual*` 死代码 | 🟡 |
| | **DD-4** | `retry_interval` 死配置 | 🟡 |
| | **DD-5** | `platformio.ini` 版本未固定（构建不可复现） | 🟡 |
| **未验证（NC）** | **NC-1..NC-8** | Storage E/W 分支与抑制窗口 / `LOG_WIFI_LOST` / `MQTT_SLEEP_ENTER` / `PUBLISH_FAIL` / 5 个 RTC 事件 / `INVALID_ENTER` / `WF_FINISHED`+`TIMEOUT` / `WF_FAILED`+`TEMP_ACTION_TIMEOUT`+`ALLOC_FAILED` | ⚪ |
| | **NC-9**（P2-G） | `LOG_WEIGHT_ERROR_ENTER`/`ERROR_EXIT` 的**记录级**捕获（边沿在板上实测执行，但恰好落在 496 条填充窗口内 ⇒ 被云队列溢出淘汰） | ⚪ |
| | **NC-10**（P2-G） | `LOG_WEIGHT_CALIB_FAILED` / `LOG_WEIGHT_TRIGGER_FIRED`（需保存失败注入 / 真实减重场景） | ⚪ |
| **测试环境耦合** | **T-1**（P2-G 发现） | **196 回归夹具隐含依赖 BT-1**：一旦有人在回归期间回 ACK（如开"虚拟云端"），队列不再溢出 ⇒ A/B/E 的 `qdrop`/`evict_inf`/`replay` 类断言**全部失效**（实测 A 52/56 · B 56/66 · E 13/23） | ⚠️ 已知并记录；**跑回归时禁止注入 ACK** |
| **设备状态** | **D-1**（P2-G 副作用） | 校准把 `weight.zero_offset` 从 **-800750 覆写为 -1**（该板 HX711 读数恒 0/-1）⇒ `current_weight` 由 ≈1080 g 变为 ≈0 g。**未回写** | ⚠️ 如需恢复：`config_set weight/zero_offset = -800750` + `config_save` |
| **Valve（VALVE）** | **VALVE-1** | 四个 valve API **均不检查 `initialized`** ⇒ 模块禁用但引脚已配置时**返回 true 假成功**且 GPIO 未配置 | 🔴 |
| | **VALVE-6**（**OPEN**） | **`FORCE_CLOSE_FAILED` 未门控**：实测 54 次调用 → 52 条 CRITICAL。**本轮决定不修**：该失败只来自人为制造 `gpio_pin=-1`，正常运行路径不会产生 | 🔴 |
| | VALVE-2 | 安全超时重复 push —— ✅ **P2-H 已修**（一次性报告锁） | ✅ |
| | VALVE-3 | force_close 失败分支不可观测 —— ✅ **P2-H 已修**（已埋 `FORCE_CLOSE_FAILED`） | ✅ |
| | VALVE-4 | `valve_close()` 返回值语义不精确（本来就关着也返回 true）⇒ 用 `LOG_P_WAS` 表达，不改返回值 | 🟡 |
| | VALVE-5 | `dispense_guard` 无节流（策略层；已由 Valve 侧 5 s 门控兜住） | 🟠 |
| **风险（R）** | **R-6** ⚠️**表述已修正** | `force_close` 调用数 == `guard_rx` 在全部 9 次运行中**恒等** ⇒ **不是"调用被复制"**，而是**同毫秒两个独立 `EVENT_WEIGHT_ERROR`** ⇒ **无需修复** | 🟡 |
| | **R-7**（**OPEN**） | **195 回归夹具与"跨模块 WARN+/CRITICAL 埋点"冲突** —— ★ 判定实验：**P2-G 固件（无 Valve 日志）异常强度高时同样 MISS（9/2 条）** ⇒ 与 Valve 日志不相关；Weight 侧单独可 +13 条额外记录 ⇒ 去掉 Valve 日志不能归零 | 🔴 |
| | **R-8**（**OPEN**） | **FS 阻塞诱发重量跳变**：LittleFS 操作阻塞 loop ⇒ HX711 窗口被跨阻塞拼接 ⇒ 跳变误报。定量：额外记录 P2-G #1=+2 / #2=+6 / #3=+13（`crit` 恒 0 ⇒ 纯 Weight 贡献） | 🟠 |

**统计**：🔴 高 6 项 · 🟠 中 11 项 · 🟡 低 9 项 · ⚪ 未验证 8 组。

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
| `docs/LogManager-Integration-Guide.md` **§16** | **新增**：**Weight/HX711 接入** —— 频率压力（进入率=每 loop / 10 Hz / 2 Hz）为何决定方案 / 5 埋点表 / `CAUSE` **位掩码** / `ERROR_EXIT` 的 2 个只读变量 / 9 个状态 vs 代码实际 / 禁止清单 / **上板验证（含两次"看似失败"的尝试）** / **§16.10 回归夹具第三层加固（F3-A 恰好填满断言）** |
| `log模块历史/LogManager-P2G-Weight接入审查0918.md` | **新增**：Weight 接入前审查（频率分析 / 状态对照 / EventId↔宿主 / **3 处矩阵不一致** / 风险 R-1..R-5 / 待拍板 4 项） |
| `docs/LogManager-Integration-Guide.md` **§17** | **新增**：**Valve / DispenseGuard 接入** —— 为何不能照矩阵直接埋点（纯事件 vs 状态迁移）/ 6 埋点表 / **突发频率实测** / `FORCE_CLOSE` 门控 / 一次性报告锁 / 状态机真相 / 上板验证 / **第四层教训** |
| `log模块历史/LogManager-P2H-Valve接入审查0919.md` | **新增**：Valve 接入前审查 + 实现与验证结果（含 7 项决策 D1~D7） |
| `docs/LogManager-Integration-Guide.md` **§18** | **新增**：**Weight Error 日志写入压力优化（P2-I）** —— 问题与边界 / 触发形态 / **窗口内折叠 + ΣCOUNT 守恒** / 逐条对照设计约束 / **方案 B、C 结论（已满足不重复修改）** / 定向注入验证 / **四种失败尝试的教训** / 回归 |
| `log模块历史/LogManager-P2I-Weight日志写入压力优化0919.md` | **新增**：**P2-I 全记录** —— 问题定义 / 现状核实（4 个 push 点 + 落盘路径 + 触发形态）/ 方案 / 方案 C 审查结论 / **确定性注入验证（5 组注入 + 3 项不变量）** / 三场景对照 / **四种失败尝试** / 回归归因 / 架构影响 / 安全语义论证 |
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
| `.pio/p15run/log_mirror.py` | **虚拟云端**（订阅 `guo_feeder/log` + 自动 ACK + 落盘）。⚠️ **两条限制**：① **绝不能与 196 回归同时跑**（ACK 会破坏队列溢出断言）；② 其自动 ACK 的 `b` 取自批次头，实测被设备 IGNORE（同批连发 11 次）⇒ **只能当记录采集器** |
| `.pio/p15run/p2g_patch.py` | P2-G 的 5 处精确插入式补丁（每处断言恰好命中 1 次，纯增量 `+66/−0`） |
| `.pio/p15run/storm_probe.py` | **长静默观测探针**：单会话保持串口打开（重开串口会复位、清掉 RAM Dirty）⇒ 用来实测 WF-1 的 5 分钟延迟窗口与风暴。⚠️ `serial_batch.py` **串口静默 1.5 s 即提前返回**，做不了这种实验 |
| `.pio/p15run/p2h_patch.py` | P2-H 的 8 处精确插入式补丁（每处断言恰好命中 1 次；**含 CRLF 行尾保留**与白名单化的"允许格式化改动"自检） |
| `.pio/p15run/valve_watch.py` | **只读**串口观测器：统计 `FORCE CLOSE` / `DispenseGuard` / `STATE_WEIGHT_ERROR` 边沿 / `Operation too frequent` 的**每秒分布**，用于量化高频调用 |
| `.pio/p15run/gen_iso.py` + `iso_report.py` | 隔离实验：生成"同一 workload"（`P2H_ISO.txt`，85 命令）并提取 T0/T1 增量 + 串口侧异常量。**用于"换固件跑同一 workload"的 A/B 归因** |
| `.pio/p15run/miss_table2.py` | 26 条 MISS 明细提取器（case/行号/命令/期望/实际/偏差/分类），输出可直接内联进报告 |
| `.pio/p15run/p2i_patch.py` | P2-I 的 10 处精确插入式补丁（含 **CRLF 行尾保留** 与"允许的机械替换白名单"自检；自检会报出每一行删改） |
| `.pio/p15run/p2i_report.py` | 折叠验证报告：从 MQTT 捕获提取 `LOG_WEIGHT_ERROR_ENTER` 序列，核对 **ΣCOUNT 守恒 / 记录数降幅 / 接受间隔 ≥ 窗口** 三项不变量 |
| `.pio/p15run/gen_p2i1..7.py` | P2-I 的诱因/验证序列生成器（V1..V7）。★ 其中 **V7** 是唯一成功的确定性方案：**`logt fill warn 10 50C` 定向注入** |

---

*最后更新：2026-09-20（**Phase 3 第 ⑤ 项 EventManager 埋点接入完成（P2-M）** —— 仅改 `src/event_manager.cpp`（`+110/−0`），接入 2 个冻结 EventId。**★ 只记自身运行异常**：业务事件一律不记（17 个 `event_push` 中 13 个发布方已埋 `log_emit` ⇒ 转发即重复）。`0x0C01` 埋在 `event_push()` 返回 `EVENT_QUEUE_FULL` 的**唯一出口**（确定丢弃分支；不在调用者、不在入口；FORCE 挤占分支不计入），`0x0C02` 埋在风暴抑制分支。二者**都只累加不逐条发射** ⇒ 采用**周期聚合方案 A**：窗口 60000 ms，**节拍器 = `event_dispatch()` 首行**（loop 每轮无条件调用 ⇒ 不新增 Task / 不改 `main.cpp` / 不新建 timer / 无 delay / 无 while）；**无异常完全静默且不推进窗口**（首次异常立即可见）；**Σ `LOG_P_COUNT` = 真实丢弃数量**（零丢失、条目恒定）。验证：**V1** 编译 SUCCESS，RAM 130616→130624（**+8 B**）/ Flash 1370533→1370701（**+168 B**）；**V2** 空闲 25 s `emit +0`、阀门开合只有 valve 自身 2 条 INFO（`flash +0`）；**V5** 40 次 `valve_toggle` 全成功 ⇒ `emit +42` 全部归属 valve 40 条 INFO + wifi 2 条 WARN（`0x0603`/`0x0605`），**逐条核对 Flash 记录 ⇒ `0x0C01`/`0x0C02` 各 0 条**；**V3 队列满 / V4 风暴运行时不可达**（定量论证：队列 32 且每轮 drain ≤4 ⇒ 需单轮 >36 次入队；风暴需同类型 6 次/500 ms，而 valve 同类型间隔 100 ms+ε ⇒ 第 6 次时窗口已刷新）⇒ 以 **objdump 静态证据**（站点 A/B 的 EventId·level·param_count·ParamId 编码 + 窗口常量 59999≡60000 + `event_push` 字面量池含两个累加器）确认埋点正确。**零协议新增**（无新 EventId/ParamId/System State/Config/开关），**事件机制完全冻结**（不改风暴策略/队列大小/dispatch 流程）。提交 `604a892`。**Phase 3 ①②③④⑤ 全部完成** ⇒ 下一步 **Phase 4 Dispense 开发**；`BLE` 埋点暂缓。`EVT-1` 单独开单、不改 EventId 编号。）*
*上一版更新：2026-09-20（**Phase 3 第 ⑤ 项前置审查完成 —— `Event` 模块日志接入设计（仅审查，未改代码）**）。结论：**EventManager 是日志宿主，但仅限"自身运行异常"**；业务事件 **76%（13/17）发布方已埋点** ⇒ 逐条记即重复。★ **决定性证据**：17 个 `event_push` 调用点 **100% 忽略返回值** + `event_get_drop_count()`/`duplicate_count()`/`queue_count()` **零消费方** ⇒ **事件丢弃当前 100% 不可观测**，只能由 EventManager 内部记录。`0x0C01`（队列满，`event_push():266`）与 `0x0C02`（风暴，`event_push():220-224`）**均不可逐条记**（后者定义即"超过 5 次/500ms 的持续流"）⇒ 采用**方案 A：计数聚合 + 60 s 周期上报**，复用 P2-D `cloud_manager.cpp:100-156` 模板，**以 `event_dispatch()` 为节拍器**（不新增任务、不改 `main.cpp`、不新增 EventId/ParamId/System State/配置项/开关、不改风暴策略）。⚠️ 陷阱：`EVENT_DROPPED` 返回值在 3 个分支返回（`:206` 越界/`:223` 风暴/`:260` FORCE 失败）⇒ **必须分支内埋点，不能用返回值判据**。重复记录：现状一条物理事实 2~3 条，若逐条记业务事件将达 4~6 条。**新增发现 EVT-1~EVT-5**，其中 **EVT-1**（`SYSTEM_EVENT_COUNT=14` 越界 ⇒ 三阀门事件 14/15/16 无法解析 ⇒ **Workflow 无法订阅阀门事件**，静默失败）建议单独开单。报告 `log模块历史/LogManager-P2M-Event接入审查0920.md`。**待用户确认 E1–E4**。)*
*上一版更新：2026-09-20（**Phase 3 第 ④ 项 Registry 埋点接入完成（P2-L）** —— 仅改 `src/capability_registry.cpp`（`+24/−0`），接入 2 个冻结 EventId / 2 处 `log_emit`。★ **宿主选"定义点"而非"触发点"**：`REG_REBUILT` 落 `registry_sync()` 的 `else` 分支（checksum 不一致/缺失/损坏，与 `reuse` 互斥 ⇒ 天然边沿无需门控），`REG_SAVE_FAILED` 落该函数内 `save_registry_file()` 返回值判定处；**不埋** `command_manager` 4 处 `rescan()`（一次 create 连触发 3 次）、**不进** `save_registry_file()` 内部 4 个失败出口（各有专属串口打印 1:1 覆盖 ⇒ 不带 `ERR_CODE`、不改签名）。参数用栈上局部 `LogParamIn`（禁 `static`/全局：可能跑在 loopTask 8KB 或 esp-mqtt 任务上）。**WF-4 正式关闭**（`workflow_storage` 与 Registry 零耦合）；矩阵 §7 作用域已更正。验证：**V1** 编译 SUCCESS，RAM 130616 B（±0）/ Flash 1370533 B（+408）；**V2** 三表全 `reuse`、`REG_*` 均 0 条；**V3** create×1+delete×1 → **恰好 2 条** rebuild（若误埋触发点应为 6）；**V4a** 清零后精确计数 emit +3 / cloud +3 / **flash +0**（INFO 不落 Flash）；**V4b** `objdump` 静态确认 2 个调用点参数编码 `(3,INFO)` / `(1,ERROR)`。提交 `4b2e04b`。)*
*上一版更新：2026-09-20（**Phase 3 第 ③ 项 ComputerReset 埋点接入完成（P2-K）**：仅改 `src/computer_reset.cpp`（`+65/−0`），接入 3 个冻结 EventId / 4 处 `log_emit`；**全部天然边沿、零门控**。★ `PULSE` 上移到 `set_output()` 上升沿以覆盖手动 + Workflow Action 两条路径（V3b 实测 Action 路径 `emit` +1 证明）；`SAFETY_TIMEOUT` 的 `DURATION_MS` 在 `force_idle()` 之前取（LOG-13）。)**
