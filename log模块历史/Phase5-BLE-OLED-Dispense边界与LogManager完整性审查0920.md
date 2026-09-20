# 下一阶段前置审查：BLE / OLED / Dispense 业务边界 + LogManager 完整性

> 日期：2026-09-20
> 阶段：Phase 4 之后 · **纯代码事实审查（零代码改动）**
> 约束：不新增 EventId / 不改 ParamId / 不加配置项 / 不加临时 enable / 不动 System State 与 EventManager 架构
> 已完成基线：**Phase 3 ①②③④⑤ + Phase 4（`94912bb` + `397d953`）**

---

## 0. 结论速览

| 议题 | 结论 |
|---|---|
| **★ 四、最重要的发现** | **`0x01xx` System/Boot 段 12 个 EventId 整段零宿主** —— 含 `RESET_ABNORMAL` / `BOOT_INCOMPLETE_PREV` / `RESTART_REQUESTED/EXECUTED/CANCELLED` / `HEAP_LOW` / `PSRAM_ALLOC_FAILED`。而 `system_command.cpp` **已经算好了 `esp_reset_reason()`**（`:97` 有名表、`:222` 有缓存）⇒ **ID 与宿主候选同时存在，只差接线** ⇒ 这是全项目**性价比最高**的埋点缺口 |
| **★ 四、第二个发现** | **协议缺口**：`log_manager.cpp:2282` `h.event_dict_ver = LOG_RECORD_VERSION` ⇒ 批次头里"**事件字典版本**"字段被填成了"**记录格式版本**"（=2）⇒ **新增任何 EventId 都不会反映到该字段**，云端无法从批次头判断事件集合是否变化 |
| **一、BLE** | **4 个 ID 的天然宿主全部在 `MiThermometer_task()`（loop 上下文）** —— 解密/窗口判定/LEVEL 迁移都在 task 里，**callback 内没有任何可记录的事实** ⇒ **"callback 禁令"根本不需要特殊规避**，正常接入即可 |
| **一、BLE 高频辨析** | 需区分两个速率：**callback 触发率**（所有广播设备，可能数十/秒）vs **通过 MAC 过滤的有效帧**（P2-BLE 实测 **≈1 包 / 3 s**）。后者才是"可记录事实"的速率 |
| **一、BLE Flash 风险** | **存在，且已可定量**：`BLE_DECODE_FAIL`(WARN) 若按帧记录，在 **bindkey 配置错误**场景下 = 20 条/min ⇒ **496 条 Flash 环 ≈ 25 min 冲满** ⇒ **必须聚合** |
| **二、OLED** | ⚠️ 用户所说"占位模块"需**分两层**看：**显示层已实现**（动画 + 配置化旋转/速度/帧间隔）；**事件响应层是占位**（只有 2 个 `Serial.println` 分支 + 1 个死函数 + 1 个永不定义的声明）。**`oled_init()` 忽略 `U2G2::begin()` 的 bool 返回值并无条件打印 "OLED init OK"** ⇒ 异常**被掩盖** ⇒ 结论：**存在真实可观测异常，但当前代码不检测** |
| **三、Dispense 业务** | 电机/出粮流程/卡料/超时/重试 **全部无代码**；日志边界可按"**定义点**"预分配，**段空间充足（0x0Fxx 剩 254 个）**；但 **DSP-1/2/4 未裁决 ⇒ 不得开工** |
| **五、性能** | 按用户指定的 4 项逐一核对：**系统卡死**（无新增无界阻塞）/ **安全链断裂**（无新增）/ **Flash 异常消耗**（BLE 1 项、已定量）/ **数据不可恢复**（`0x01xx` 空缺 = 核心缺口）。**`R-7`/`R-8` 未重新升级**（见 §5.5 显式声明） |
| **本轮提交** | 仅本报告 + 文档同步；`src/` 与 `test/` **零改动** |

---

## 一、BLE 模块日志边界设计

### 1.0 先厘清一个前提：BLE 的"高频"到底指什么

矩阵 §11.1 写「🔴 **最高风险**：1s 扫描窗口内可能收到数十个 ADV」。这句正确但**容易被误读**，因为存在**两个完全不同的速率**：

| 速率 | 量级 | 由谁看到 | 可记录的事实 |
|---|---|---|---|
| **A. `onResult()` 被调用的次数** | 所有在空中的广播设备 ⇒ 密集环境可达**数十/秒** | callback | ❌ **无** —— MAC 不匹配的设备在 `:124-126` 就 `return` 了，没有任何语义 |
| **B. 通过 MAC 过滤 + 长度过滤、真正 `xQueueSend` 入队的帧** | **≈ 1 包 / 3 s**（P2-BLE 实测：目标设备广播周期 1–2 s × 扫描占空"1 s 开 / 1.8–2.3 s 关"） | task | ✅ 温度 / 湿度 / 电量 / 解码失败 |
| **C. 扫描窗口级事件** | 15 / 30 min | task | ✅ 窗口成功 / 窗口失败 / LEVEL 迁移 / 禁扫 |

**⇒ 结论**：**"逐广播包记录"要禁止的，是速率 A**（无意义且高频）；**速率 B 只有 1/3 s，本身完全在预算内**。这决定了下面的设计：**日志一律挂在 task（速率 B/C），绝不挂 callback（速率 A）**。

### 1.1 哪些异常属于 BLE 模块自身生命周期异常？

按"**定义点在哪**"（铁律 22/29）逐条核对，`0x09xx` 段 4 个已冻结 ID 与**实际代码位置**一一对位：

| # | EventId | Level | 消息 | **代码位置（loop 上下文）** | 性质 |
|---|---|---|---|---|---|
| 1 | `LOG_BLE_DATA_DECODED` 0x0901 | INFO | 数据解码成功 | `MiThermometer_task()` :468 `if(result){}` 内 | 业务事实（**正常**） |
| 2 | `LOG_BLE_DECODE_FAIL` 0x0902 | **WARN** | 解码失败 | `MiThermometer_task()` :468 **`else`**（即 `result == false`） | 🔴 **自身运行异常**（本模块唯一真正的"设备异常"） |
| 3 | `LOG_BLE_SENSOR_LOST` 0x0903 | **WARN** | 传感器失联 | `MiThermometer_task()` :362 `else { s_scan_fail_count++; ... }` | **生命周期异常**（窗口级） |
| 4 | `LOG_BLE_SCAN_DISABLED` 0x0904 | INFO | 扫描已禁用 | `MiThermometer_task()` :373-382 进入 `MI_THERMO_LEVEL2` | **生命周期终结**（永久停用） |

**★ 关键代码事实**：**这 4 个位置全部在 `MiThermometer_task()` 里**。`lywsd03_decrypt()` 在 `:467` 被 task 调用（**不在 callback 内**）；窗口判定、LEVEL 迁移、`state_set_bool(STATE_MI_THERMO_ENABLE,false)` 全在 task 的 `switch(s_scan_state)` 或队列消费循环里。

⇒ **没有任何一个需要记录的异常发生在 `MiAdvCallback::onResult()` 中。**

**代码事实：`onResult()` 的全部动作（`:114-167`）**
```
① xRawAdvQueue 空检查 → return
② 取 MAC 指针 → 空检查 → return
③ MAC 逐字节比对（不匹配 ⇒ return）          ← 速率 A 在此被丢弃
④ 取 payload → 空检查 → return
⑤ 长度过滤 (payload.size() < 29 ⇒ return)
⑥ 构造 RawAdvItem（memcpy，无 malloc）
⑦ [编译期开关] hex dump（默认关闭，P2-BLE 已处置）
⑧ xQueueSend(xRawAdvQueue, &item, 0)        ← 返回值被忽略
```
⇒ **callback 里没有"决策"，只有"搬运"** ⇒ 按铁律 25「总线只搬运，不代记」⇒ **callback 埋点数 = 0**。

**尚未覆盖的 BLE 自身异常（候选，需新增 ID）**：

| 候选 | 位置 | 性质 | 是否属"自身运行异常" |
|---|---|---|---|
| `BLE_INIT_FAILED` | `MiThermometerInit()` :247-251（MAC/bindkey 为空）、:277-281（`xQueueCreate` 失败） | 初始化失败 ⇒ **模块永久不可用** | ✅ **是**（且 `:247` 分支 **`return false` 但调用方 `main.cpp:508` 忽略返回值** ⇒ 不可观测，与 `LV-1` 同构） |
| `BLE_QUEUE_FULL` | `onResult()` :165 `xQueueSend(...,0)` **返回值被忽略**；队列 16 槽 | 静默丢帧 | ✅ **是**，但**不能在 callback 记录** ⇒ 需在 `onResult` 置计数、task 侧消费上报（同 `EVT_QUEUE_FULL` 的做法） |

### 1.2 哪些状态变化已由其他模块覆盖，不应重复记录？

逐条核对（`grep` 全仓库）：

| 状态 / 事实 | 是否有其它模块已记录 | 判定 |
|---|---|---|
| `STATE_MI_THERMO_TEMP` / `HUMID` / `BAT_V` / `*_TS` | ❌ 无其它模块引用（**BLE 独占**） | ⚠️ 但**温度/湿度是"数据"不是"事件"**，且**已进 System State** ⇒ **不需要也不应该进日志**（否则退化成"每包一条"） |
| `STATE_MI_THERMO_ENABLE` / `VALID` | ❌ 无其它模块写入 | 同上 —— 是**状态**，不是**事实** |
| 温度/湿度**数值** | `cloud_manager` 会上报 `state_report` | ❌ **不进日志**（云端已有独立 state 通道） |
| BLE 设备离线 | ❌ 无 | ✅ **应由 BLE 自己记**（→ `BLE_SENSOR_LOST`） |
| WiFi 断开 / 重连 | ✅ `wifi_module.cpp` 已记 `0x0604`/`0x0605` | ❌ **不重复** |
| BLE 扫描与 WiFi 的**时间复用冲突** | ❌ 无 | 📌 属"设计约束"而非"事实"，**不记** |
| 时间无效（影响 `*_TS` 可信度） | ✅ `time_manager.cpp` 已记 `0x0804` | ❌ **不重复** |

⇒ **BLE 侧不存在与其它模块重叠的埋点**（BLE 的数据与状态是独占的），**风险不是"重复"而是"把数据当事件记"**。

### 1.3 callback 上下文是否允许 `log_emit()`？

**技术层面（逐项核对 `log_emit()` 的实际开销）**：

| 检查项 | 事实 | 判定 |
|---|---|---|
| 有 I/O 吗？ | `log_emit_internal()`（`log_manager.cpp:1703-1870`）**无 `Serial` / 无 `LittleFS` / 无 `File`** | ✅ 无 |
| 有动态分配吗？ | 无 `malloc` / `free` / `new` | ✅ 无 |
| 有阻塞等待吗？ | 无 `delay()` / 无 `while` 等待 | ✅ 无 |
| 临界区多长？ | `portENTER_CRITICAL(&s_mux)` 包住 **128 B `memcpy` + 计数**⇒ **µs 级** | ✅ 短 |
| 栈开销？ | `LogRecord rec`(128 B) + `LogParamIn coalesce_p[8]`(≈64 B) ≈ **200 B** | ⚠️ 对照 **NimBLE host task 栈 = 4096 B**（`nimconfig.h:217`）⇒ 约 **5%** —— **可行但未实测余量** |
| 与 loop 的交互？ | `log_task()` 只在 loop 持同一把 `s_mux`，且 **Flash I/O 在临界区之外**（§22/§3 冻结设计） | ⚠️ **优先级反转**：NimBLE host task **优先级高于 loopTask** ⇒ 若 loop 正持锁（µs 级），host task 会自旋等待 |

⇒ **技术上"可以"，但工程上"不应该"**，三条理由：
1. **既有硬契约**：矩阵 §11 明文 🔴「**不得在 `MiAdvCallback::onResult()` 中调用 `log_emit()`**」
2. **优先级反转**：唯一可能被拖慢的是 loopTask 的 `log_task()`（µs 级持锁）⇒ 影响可忽略，但**方向是错的**（高优先级任务等低优先级任务）
3. **完全没必要**：**callback 里没有任何可记录的事实**（§1.1）⇒ 这条禁令在本轮**根本不会被触发**

⇒ **本轮设计结论：不需要在 callback 内埋点，因此"callback 禁令"不构成任何约束。**（这比"如何规避禁令"更彻底的答案）

### 1.4 是否需要缓存聚合后周期上报？

| 位置 | 速率 | 需要聚合吗 | 理由 |
|---|---|---|---|
| `DATA_DECODED` | **≤1 / 扫描窗口**（收到 temp+humid 后 `s_scan_state = SCAN_WINDOW_WAIT` ⇒ **停止扫描**，`:490-499`） | ❌ **不需要** | 代码天然保证"每窗口最多一次" —— 需在埋点处加**"本窗口已上报"标志**（矩阵建议，正确） |
| `SENSOR_LOST` | **≤1 / 扫描窗口**（15/30 min） | ❌ **不需要** | 天然低速率 |
| `SCAN_DISABLED` | **1 次 / LEVEL2 迁移**，且迁移后 `s_scan_state = DISABLED` ⇒ 不再进入 | ❌ **不需要** | 天然边沿（`state_set_bool(ENABLE,false)` 前） |
| **`DECODE_FAIL`** | **可达 1 / 3 s**（bindkey 错误场景） | ✅ **需要** | 见 §1.5 |

⇒ **4 个 ID 中只有 `DECODE_FAIL` 需要聚合**。且**已有两种现成机制可选**（零新机制）：
- **A. LogManager 突发合并白名单**（`s_coalesce_targets[]`，5 s 窗口，`Σ LOG_P_COUNT` 守恒）—— 与 Phase 4 的 `0x0511` 同法
- **B. 窗口级聚合**（模块自己在 task 内累加，窗口结束时上报 1 条带 `LOG_P_FAIL_COUNT`）—— 与既有 `s_scan_fail_count` 语义天然对齐

⇒ 推荐 **B**（模块内已有 `s_scan_fail_count` 计数器与"窗口结束"节拍点，**无需改 LogManager**）；若选 A 需注意 **铁律 30**（被合并埋点**必须至少带 1 个参数**）。

### 1.5 是否存在 Flash 风险？

**逐 ID 定量核算**（Level Policy：`INFO → (Flash=0, Cloud=1)`；`WARN+ → (Flash=1, Cloud=1)`）：

| EventId | Level | 落 Flash？ | 最坏速率 | 496 条段环耗尽时间 | 判定 |
|---|---|---|---|---|---|
| `DATA_DECODED` 0x0901 | INFO | ❌ | ≤1 / 15 min | — | ✅ 无 Flash 风险（仅压云队列 `128 槽`，速率 1/15min ⇒ 可忽略） |
| **`DECODE_FAIL` 0x0902** | **WARN** | ✅ | **1 / 3 s = 20 / min**（**bindkey 配置错误**场景：CCM 校验对每包都失败） | **≈ 25 min** | 🔴 **真实风险** ⇒ **必须聚合**（聚合后 5 s 窗口 ⇒ ≈12 条/小时 ⇒ 环寿命 ≈41 h） |
| `SENSOR_LOST` 0x0903 | WARN | ✅ | ≤1 / 15 min | ≈ 124 h | ✅ 安全 |
| `SCAN_DISABLED` 0x0904 | INFO | ❌ | 1 次/生命周期 | — | ✅ 安全（仅云队列 +1） |

**★ 与已有先例的一致性核对**：
- 这与 `R-3`（`valve_force_close()` 6~20 次/s ⇒ CRITICAL 83 s 冲光）**是同一类问题但量级低 3 个数量级**（20/min vs 1200/min）
- Phase 4 的 `0x0511`（WARN）在 6~20 次/s 下不合并会让 5 s 内产生 30~100 条 ⇒ 必须合并；BLE 是 5 s 内 ≈1.7 条 ⇒ **不合并也能活**，但**在 bindkey 错误这种"长期错误配置"下会持续消耗**，所以仍建议聚合

⇒ **结论：`DECODE_FAIL` 有 Flash 风险，必须聚合；其余 3 个无风险。**

---

## 二、OLED 模块状态

### 2.1 先纠正一个前提：OLED 不是"整体占位"

| 层 | 状态 | 证据 |
|---|---|---|
| **显示层** | ✅ **已实现** | `oled.cpp` 301 行 + `oled_animation.h` **1367 行**（动画帧表）；`oled_task()` 按 `frame_delay` 播放；`oled_init()` 读取 4 项配置（`sda`/`scl`/`i2c_speed`/`frame_delay`/`rotation`）—— `data/config/oled.json` 齐全 |
| **事件响应层** | ⚠️ **占位** | `oled_event_handler()`（`:213-243`）**只有 2 个有效 case**，且都只 `Serial.println`；`oled_event_init()` 注册了 **4 个事件**（`CONFIG_CHANGED` / `NTP_SYNC_OK` / `WIFI_CONNECTED` / `WIFI_DISCONNECTED`），其中 **3 个落到 `default: break;`** |
| **恢复能力** | ❌ **死代码** | `oled_restart()`（`:251-263`，含 `oled_init()` 重入）**全仓库零调用者**（只被前向声明 `:14` 与定义 `:251`） |

⇒ **"占位"适用于事件/交互层，不适用于显示层。**

### 2.2 是否存在真实可观测异常？

**★ 有，且是本轮第二个"ID 与宿主同时存在却未接线"的发现：**

```cpp
// src/oled.cpp:75-112
void oled_init()
{
    ...
    oled.begin();                    // ← ★ 返回值被忽略
    oled.setDisplayRotation(oled_rotation);
    Serial.println("OLED init OK");  // ← ★ 无条件打印"成功"
}
```

**代码事实**：
- `U8g2::begin()` 的签名是 **`bool begin(void)`**（`.pio/libdeps/*/U8g2/src/U8g2lib.h:144`）⇒ **它确实返回成功/失败**
- `oled_init()` **丢弃该返回值**，并**无条件**打印 `"OLED init OK"`
- ⇒ **OLED 未接 / I2C 无应答 / 地址错误**时：`begin()` 返回 `false`，但串口**仍然显示 "OLED init OK"** ⇒ **假成功**

**这与已登记的同类问题完全同构**：

| 案例 | 现象 | 登记 |
|---|---|---|
| `valve_force_close()` 不检查 `initialized` ⇒ 返回假成功 | 阀门实际未驱动 | **`VALVE-1`**（OPEN） |
| `dispense_guard_init()` 忽略 `event_subscribe()` 返回值 | 安全链永久失效静默 | **`LV-1`**（OPEN） |
| **`oled_init()` 忽略 `oled.begin()` 返回值** | **显示失效显示为"OK"** | ⚠️ **本轮新发现 ⇒ 候选 `OLED-1`** |

**⇒ 对用户问题「1. 是否存在真实可观测异常？」的答案：**
> **存在**（I2C NACK、OLED 未接、地址错、总线被 RTC 占用冲突），**但当前代码不检测它** ⇒ `LOG_OLED_INIT_FAILED`(0x0E01) 之所以"无宿主"，**不是因为没有异常，而是因为代码没有在产生判据**。
> ⇒ 这与先前"**判为无宿主不埋**"的结论**需要修正为**：「**有异常、有 ID，但缺判据**」。

**附加发现（不影响本轮结论，仅登记）**：
- `oled.h:16-18` 声明 `void oled_event_handler(SystemEvent event);` —— **该重载永不定义**（`oled.cpp:213` 定义的是 `const EventMessage&` 版本）⇒ **死声明**（与 `DD-1` `weight_is_active` 同类）
- `oled_event_init()` 的 **4 次 `event_subscribe()` 返回值全部被忽略** ⇒ 与 `LV-1` 同构
- `oled_task()` 内 `oled.sendBuffer()` 返回值被忽略，**无任何失败路径**

### 2.3 是否应该保持"不埋点"状态？

**分两种情形给结论（取决于用户对 §七-D4 的裁决）**：

| 情形 | 建议 | 理由 |
|---|---|---|
| **情形 A：不修 `oled.begin()` 返回值检查** | ✅ **维持"不埋点"** | **埋点没有可依附的判据** —— 按铁律"门控/边沿只能加在已有的判定点上"，凭空造一个 `if(!ok)` 属于**改生产逻辑**（用户已禁） |
| **情形 B：批准修 `begin()` 返回值检查**（1 处 `if`，约 3 行） | ✅ **接入 `0x0E01`**（已有 ID、已有 Level=WARN、**无需新增**） | 修好后判据自然存在 ⇒ 埋点变成"纯观测" |

⇒ **两者都成立，但都不是"现在就改代码"** —— 属**待裁决项**（§七-D4）。**本轮维持"不埋点"**（现状不变）。

---

## 三、Dispense 模块后续业务开发边界

> 前提：**当前只有 `dispense_guard.cpp`（138 行，含 Phase 4 埋点）**，**电机控制 / 出粮流程 / 卡料检测 / 超时保护 / 重试机制 全部无代码**（`grep` 全仓库 `motor`/`stepper`/`auger`/`servo` 零实现命中）。
> **本轮只做日志边界设计，不启动任何实际功能。** 且 **`DSP-1`（命名）/ `DSP-2`（日志段）/ `DSP-4`（超时队头阻塞）未裁决 ⇒ 不得开工**。

### 3.1 五个子项的日志边界设计（预分配，不落地）

**总原则**（沿用铁律 22/25/29）：
- 每个事实**只由它的"定义点"记录**
- 编排层（`DispenseManager`）**不替执行层（Motor 驱动）记硬件事实**
- 安全响应（`DispenseGuard`）**已定：只记"决策"（`0x0511`）**，不再扩展

| # | 子项 | 应记录的**事实**（定义点） | 宿主 | EventId 归属（**建议，未新增**） | Level |
|---|---|---|---|---|---|
| 1 | **电机控制** | ① 驱动级失败（GPIO 未初始化 / PWM 通道申请失败 / 通道冲突）<br>② 指令被拒（未使能、参数越界） | **Motor 驱动** | `0x0Fxx` 段（`MOTOR_*`） | ①ERROR ②WARN |
| 2 | **出粮流程** | ① 出粮开始<br>② 出粮完成（携 `TARGET_G`/`FINAL_G`/`DURATION_MS`）<br>③ 出粮失败（携 `CAUSE`） | **DispenseManager**（编排） | **复用 `0x0501–0x0504`？** ⚠️ **待 `DSP-2` 裁决** —— 见 §3.2 | START/DONE=INFO；FAILED=WARN |
| 3 | **卡料检测** | 卡料**判据成立**（如"N ms 内重量增量 < 阈值"） | **DispenseManager**（判据在业务） | 新增（`0x0Fxx` 或 `0x05xx`） | WARN + **边沿**（进入/解除配对，同 `WEIGHT_ERROR_ENTER/EXIT`） |
| 4 | **超时保护** | 出粮过程超时（携 `TIMEOUT_MS` + `ELAPSED_MS`） | **DispenseManager** | 复用 `LOG_DISPENSE_TIMEOUT`(0x0504)? 或新增 | WARN |
| 5 | **重试机制** | ① 重试**发起**（携 `ATTEMPT_N`/`MAX_N`）<br>② 重试**耗尽**（携 `ATTEMPT_N`） | **DispenseManager** | 新增 2 个 | ①WARN ②ERROR |

**明确的"不记"清单**：

| 不记 | 理由 |
|---|---|
| 电机的每一步 / 每一相 | 高频、无诊断价值（等价于"逐广播包记录"） |
| 重量采样值本身 | **`weight.cpp` 已记**（`LOG_P_WEIGHT_G` 随 `0x050C/0x050D/0x050F`）⇒ 重复 |
| 重量异常的检测 | **`weight.cpp` 已记**（`0x050C`）⇒ 重复 |
| 关阀动作 | **`valve.cpp` 已记**（`0x0505`/`0x0506`）⇒ 重复 |
| 安全响应决策 | **`dispense_guard.cpp` 已记**（`0x0511`，Phase 4）⇒ 重复 |
| Workflow 编排结果 | **`workflow.cpp` 已记 12 个 `LOG_WF_*`**（`0x0401`–`0x040C`）⇒ 重复 |
| 云端命令结果 | **`command_manager.cpp` 已记**（`0x0A01`–`0x0A04`）⇒ 重复 |

### 3.2 与前置审查结论的衔接（`DSP-2` 已被 Phase 4 部分回答）

Phase 4 的决策**隐式回答了一半的 `DSP-2`**：
- `0x0500` 段（Water/Dispense）**已用于"安全响应决策"（`0x0511`）**
- `0x0Fxx` 段（Motor）**仍完整保留给"出粮驱动"**（254 个空闲）

⇒ 建议 `DSP-2` 收敛为：
> **"出粮业务过程"（START/DONE/FAILED/TIMEOUT）放 `0x0Fxx` 段**（与 readme 的 **Motor** 命名、`LOG_MOTOR_RESERVED_BASE` 预留意图一致），
> 而 **`0x0501–0x0504` 继续保留不占用**（其语义是"供水过程"，与出粮不同域）。

⇒ 这样 `0x0Fxx` 段的语义变为"**出粮（Motor + Dispense 编排）**"，与 `0x05xx` 的"**供水（Valve + Weight + Guard）**"**形成干净的两域划分**。

### 3.3 三个必须写进设计的硬约束（来自已确认的代码事实）

| # | 约束 | 依据 |
|---|---|---|
| 1 | **`timeout_ms` 必须必填并钳位 ≤60 s** | `workflow.cpp:4462` Temp Action **严格 FIFO 队头阻塞**；`command_manager.cpp:78` 默认 `COMMAND_ACTION_TIMEOUT_MS = 600000` ⇒ 不钳位会**堵死队列 10 分钟** |
| 2 | **重量反馈必须用 `weight_get_gram()`，不能读 System State** | `weight.cpp:284-293` 在 `weight_active==false` 时 `STATE_WEIGHT_VALUE` **每 30 s 才更新** |
| 3 | **卡料判据需边沿配对**（进入 1 条 + 解除 1 条） | 参照 `LOG_WEIGHT_ERROR_ENTER/EXIT` 的既有模式；否则持续卡料会持续刷日志 |

---

## 四、LogManager 完整性统计

> 方法：脚本化审计 `src/log_events.h` 的 102 个非零 `LOG_*` EventId，在 `src/*.cpp` + `src/*.h`（**剥除注释**）中检索三类宿主证据：
> ① 作 `log_emit(` / `log_emit0(` 的**字面实参**；② 作**映射函数的 `return` 值**（`stg_bridge_event` / `cfg_bridge_event`，这两处让 Storage/Config 段的字面扫描产生**假阴性**）。
> 工具落 `.pio/p5run/evtid_audit.py`（可复用）。

### 4.1 已接入 / 未接入模块

| 项 | 数量 | 清单 |
|---|---|---|
| **已接入** | **13** | Storage(A) · Config(B) · WiFi(C) · Cloud(D) · Time(E) · Workflow(F) · Weight(G) · Valve(H) · Command(J) · ComputerReset(K) · Registry(L) · Event(M) · **DispenseGuard(Phase 4)** |
| **未接入（有待接入模块）** | **2** | **BLE**（4 个 ID 待接）· **OLED**（1 个 ID，缺判据 ⇒ 维持不埋） |
| **未接入（存储完整性缺口）** | **2** | `bin_storage`（有回调 `bin_storage_set_log_callback()` 但 `main.cpp:474` **只注册了串口版** `bin_log_serial`）· `workflow_storage`（**无回调接口**，需先加接口） |
| **判定为"零埋点"（性质不同）** | 2 | `oled.cpp` = **有异常有 ID 但缺判据**；`dispense_guard.cpp` = **已接入 1 条**（Phase 4）⇒ 不再是零埋点 |

### 4.2 已冻结 EventId 使用情况（★ 权威统计）

| 项 | 数值 |
|---|---|
| 定义总数（非零） | **102** |
| **有宿主** | **69** |
| **零宿主** | **33** |

**零宿主 33 个，按性质分五类：**

| 类别 | 数量 | 清单 | 性质 |
|---|---|---|---|
| **① 待接入模块** | **5** | `0x0901`–`0x0904`（BLE 4）· `0x0E01`（OLED 1） | 🟡 **本轮审查对象** |
| **② 整段无宿主：System/Boot** | **12** | `0x0101`–`0x010C` **全段** | 🔴 **★ 本轮最重要的完整性缺口**（详见 §4.5） |
| **③ 无产生点（无检测代码）** | **6** | `0x0208 CFG_FACTORY_RESET` · `0x0507 VALVE_OVERFLOW_RISK`（`P0-4` 需新增检测）· `0x0606/0x0607 WIFI_PROVISION_*`（配网未实现）· `0x0706 CLOUD_FRAG_FAIL`（`DD-2` 死代码）· `0x0802 TIME_NTP_FAIL` | 🟡 已登记，非本轮范围 |
| **④ 刻意走侧信道（设计，非缺口）** | **5** | `0x0707`–`0x070B`（LogManager 自身 5 个）⇒ 改用批次头 `drop_ring`/`drop_overflow`/`drop_unacked`/`self_degraded` | ✅ **设计** |
| **⑤ 保留不用 / 预留占位** | **5** | `0x0501`–`0x0504`（Dispense 生命周期，**Phase 4 判语义不对位而未占用**）· `0x0F00 MOTOR_RESERVED_BASE`（**占位常量，非事件**） | ⚪ 保留 |

**段级分布（定义数 / 有宿主 / 零宿主）：**

| 段 | 定义 | 有宿主 | 零宿主 | 备注 |
|---|---|---|---|---|
| `0x01xx` System/Boot | 12 | **0** | **12** | 🔴 **整段零宿主** |
| `0x02xx` Config | 10 | 9 | 1 | 经 `cfg_bridge_event()` 映射 + `log_emit0` |
| `0x03xx` Storage | 6 | **6** | 0 | 经 `stg_bridge_event()` 映射（**字面扫描会假阴性**） |
| `0x04xx` Workflow | 12 | 12 | 0 | ✅ |
| `0x05xx` Water | 17 | 12 | 5 | +Phase 4 的 `0x0511` |
| `0x06xx` WiFi | 7 | 5 | 2 | |
| `0x07xx` MQTT/Cloud/Log | 11 | 5 | 6 | 5 个走侧信道（设计） |
| `0x08xx` Time/RTC | 10 | 9 | 1 | |
| `0x09xx` BLE | 4 | **0** | **4** | 🟡 本轮对象 |
| `0x0Axx` Command | 4 | 4 | 0 | ✅ |
| `0x0Bxx` Registry | 2 | 2 | 0 | ✅ |
| `0x0Cxx` Event | 2 | 2 | 0 | ✅ |
| `0x0Dxx` ComputerReset | 3 | 3 | 0 | ✅ |
| `0x0Exx` OLED | 1 | **0** | **1** | 🟡 本轮对象 |
| `0x0Fxx` Motor | 1（占位） | 0 | 1 | ⚪ 预留 |

### 4.3 剩余可用编号空间

| 项 | 数值 |
|---|---|
| 段容量（`0x01`–`0xFF`） | 255 / 段 |
| 已启用段 | **15**（`0x01`–`0x0F`） |
| 理论总容量 | **3825** |
| 已定义 | **102** |
| **剩余** | **3723（97.3% 可用）** |
| 各段剩余（最小） | `0x05xx` 238 · `0x01xx` 243 · `0x04xx` 243 · 其余 ≥244 |
| 全新段可用 | `0x10`–`0xFF` **共 240 个段 × 255 = 61200** 完全未启用 |

⇒ **编号空间完全不构成约束**（这也是 Phase 4 能为 `0x0511` 做"零成本决策"的前提）。

### 4.4 是否存在日志协议设计缺口？

**★ 缺口 1（本轮新发现，属真实协议缺陷）：`event_dict_ver` 字段被填成了记录格式版本**

```cpp
// src/log_cbor.h:98-103  （批次头结构）
struct LogCborBatchHeader {
    uint32_t fmt;              // LOG_BATCH_FMT
    uint32_t event_dict_ver;   // ★ 注释写的是"事件字典版本"
    uint32_t boot_seq;
    ...
};

// src/log_manager.cpp:2282   （唯一的赋值点）
    h.event_dict_ver = LOG_RECORD_VERSION;      // ← ★ 填的是"记录格式版本"(=2u)

// src/log_events.h:417
#define LOG_RECORD_VERSION        2u

// src/log_events.h:535
#define LOG_BKEY_EVENT_DICT_VER   1u    // ★ 这只是 CBOR map 的"整数键索引"（键值 1），不是版本值
```

**后果（对云端解码的影响）**：
- `LOG_RECORD_VERSION` 只在 `rec.version`（记录自身格式）变化时递增 ⇒ **新增/删除 EventId 不会改变它**
- ⇒ **本设备新增 `0x0511`、未来新增 BLE/Motor 的 EventId，云端从批次头看不到任何版本变化**
- ⇒ 若云端持有一份"EventId → 名称/单位"字典（这是 `event_dict_ver` 的本意），它**无法判断是否需要更新字典** ⇒ 只能靠"遇到未知 EventId 就跳过"，或靠人工/带外同步
- ⇒ **`LOG_BKEY_EVENT_DICT_VER`(1u) 与 `h.event_dict_ver` 是两套概念，当前代码把两者混成了一件事**

**处置选项（需裁决，见 §七-D5）**：

| 选项 | 做法 | 代价 |
|---|---|---|
| **A. 仅登记不改**（推荐本轮） | 写入 `未修复的问题.md`（新编号 **`PROTO-1`**），云端侧以"未知 EventId 容忍"策略兜底 | 零代码改动，但协议语义继续含混 |
| **B. 引入真正的事件字典版本** | 新增一个编译期常量（如 `LOG_EVENT_DICT_VER`），**新增/删除 EventId 时手动 +1**；`h.event_dict_ver` 改填它 | ⚠️ **属冻结协议变更**（`log_events.h` + `log_manager.cpp`）⇒ **须独立评审**，且**会与"不新增协议元素"冲突** |
| **C. 云端按 EventId 值自适应** | 不改固件，云端遇到未知 `0xYYZZ` 时以 `"unknown_0xYYZZ"` 记录 | 零固件改动，但云端失去"单位/语义"信息 |

**★ 缺口 2：`LOG_P_COUNT` 的"隐式追加"缺少调用方约定文档化**
- 合并白名单内的 EventId，其记录会被**自动追加** `LOG_P_COUNT`（`log_coalesce_filter` 的 `out_p[param_count] = log_arg_u32(LOG_P_COUNT, 1u)`）
- 若调用方**自己也传** `LOG_P_COUNT` ⇒ 记录里会出现**两个** `LOG_P_COUNT`（云端解析歧义）
- 当前 `log_manager.h` 的契约只写了"调用方无需感知"，**未写"禁止自行传 `LOG_P_COUNT`"** ⇒ 建议补文档（**纯注释，不算协议变更**）

**★ 缺口 3：`0x01xx` 段整段空缺 ⇒ 缺少"设备自身生命史"证据链**（详见 §4.5）

### 4.5 ★★ 最重要的发现：`0x01xx` System/Boot 段 12 个 ID **全部零宿主**

**代码事实**：
```
$ grep -rn "LOG_SYS_" src/ --include=*.cpp --include=*.h | grep -v log_events.h
src/main.cpp:258: // （重启请求归 System 段 `LOG_SYS_RESTART_REQUESTED`，避免跨段重复）
（仅一行注释 —— 零个 log_emit 调用点）
```

**但是，判据与宿主候选都已在代码中现成存在**：

```cpp
// src/system_command.cpp:46
// 本次启动的 Reset Reason 缓存（esp_reset_reason() 结果随运行可能变化…
// src/system_command.cpp:97
    switch ((esp_reset_reason_t)s_reset_reason) { ... }   // ← ★ 已有"复位原因 → 名称"的完整映射
// src/system_command.cpp:222
    s_reset_reason = (uint8_t)esp_reset_reason();         // ← ★ 已在启动时采样并缓存
```

⇒ **`LOG_SYS_RESET_ABNORMAL` / `LOG_SYS_RESET_NORMAL` / `LOG_SYS_BOOT_INCOMPLETE_PREV` 的判据（`esp_reset_reason()`）已经算好了，只是从未写进日志。**
这与 `LV-1`（"有宿主候选但无 EventId"）**恰好相反** —— 这里是「**有 ID、有判据、有宿主，只差一行 `log_emit`**」。

**为什么这是最高优先级缺口（对照用户指定的 4 个性能关注点）**：

| 缺失的 ID | 语义 | 它本该回答的问题 | 与用户关注点的关系 |
|---|---|---|---|
| `LOG_SYS_RESET_ABNORMAL` 0x0103 | 异常复位 | **设备是不是在崩溃重启循环？** | 🎯 **数据不可恢复** + **系统卡死** |
| `LOG_SYS_BOOT_INCOMPLETE_PREV` 0x0102 | 上次启动未完成 | 上次是不是没跑完就死了？ | 🎯 **数据不可恢复** |
| `LOG_SYS_RESET_NORMAL` 0x0104 | 正常复位 | 正常重启 / OTA / 手动 的基线 | 对比基线 |
| `LOG_SYS_RESTART_REQUESTED/EXECUTED/CANCELLED` 0x0109–0x010B | Safe Restart V2 三态 | **一次安全重启的完整证据链**（谁请求的 / 是否真正执行 / 是否被取消） | 🎯 **数据不可恢复**（重启是唯一会丢 RAM 状态的动作） |
| `LOG_SYS_HEAP_LOW` 0x0108 | 堆不足 | 内存枯竭预警 | 🎯 **系统卡死**（OOM 前兆） |
| `LOG_SYS_PSRAM_ALLOC_FAILED` 0x0107 | PSRAM 分配失败 | 大缓冲（LogManager 环 / Workflow 池）降级到 DRAM | 🎯 **系统卡死** + 性能退化 |
| `LOG_SYS_FS_MOUNT_FAILED` 0x0106 | LittleFS 挂载失败 | 文件系统不可用 | ⚠️ **与 `LOG_STG_FS_UNAVAILABLE`(0x0301，已有宿主) 语义重叠** ⇒ 需先定边界 |
| `LOG_SYS_INIT_FAILED` 0x0105 | 模块初始化失败 | 某个 init 返回 false | ⚠️ 部分被各模块自有 ID 覆盖（如 `CFG_MODULE_LOAD_FAILED`）⇒ 需定边界 |
| `LOG_SYS_CRITICAL_OP_UNDERFLOW` 0x010C | Critical Op 计数下溢 | **Safe Restart 计数被多释放** ⇒ 永久无法重启 | 🎯 **数据不可恢复**（P0 级 bug 的探测器） |
| `LOG_SYS_BOOT_COMPLETE` 0x0101 | 启动完成 | **日志时间轴的锚点**（云端按 boot_seq 分组时的"起点"标记） | 🎯 可观测性基础 |

**★ 一个具体后果**：当前 Flash 段环里**没有任何"本次启动的开始"标记**，重启后只能靠 `boot_seq` 字段（记录级）区分 —— 而 `boot_seq` 是**每条记录都带**的，缺少**一次性的启动标记**会让云端无法快速定位"每次启动产出多少条记录"。

⇒ **建议：把 `0x01xx` 段接入作为下一阶段的第一优先项**（理由：判据现成、宿主现成、价值最高、风险最低 —— 全部是**启动期一次性**记录，零频率风险）。

---

## 五、性能重点评估（按用户指定的 4 项）

> ⚠️ **前置声明**：本节**只**评估用户指定的 4 项。**`R-7`（回归夹具环境干扰）与 `R-8`（采样窗口无时间信息）均为 `DEFERRED`，本轮不重新升级**（见 §5.5）。

### 5.1 系统卡死（无界阻塞 / 死锁）

| 检查项 | 结论 | 证据 |
|---|---|---|
| 新增无界阻塞？ | ✅ **无** —— 本轮零代码改动 | — |
| `R-8-A`（`HX711::read()` 无超时 ⇒ loop 永久挂起） | ✅ **已修**（`129606f`） | 见 `未修复的问题.md` |
| BLE `pBLEScan->start(0, true)` 是否阻塞？ | ⚠️ **需上板确认**（`start(duration=0, is_continue=true)` 语义为"持续扫描立即返回"，但未实测） | `MiThermometer.cpp:392` |
| OLED I2C 阻塞量级（**新评估**） | ⚠️ **可估算，非无界** | 见下 |

**OLED 阻塞量级估算（诚实标注为估算，未上板测量）**：
```
帧缓冲 = 128 × 64 / 8 = 1024 B
I2C 速率 = 100 kHz（data/config/oled.json: "i2c_speed": 100000）
理论传输时间 ≈ 1024 × 9 bit ÷ 100 kHz ≈ 92 ms
帧间隔 = frame_delay = 50 ms（data/config/oled.json）
```
- **若** u8g2 的 HW I2C `sendBuffer()` 是阻塞式（Arduino `Wire` 语义），则 **loop 每轮约 92 ms 被 I2C 占用**，且 **92 ms > frame_delay 50 ms** ⇒ 实际帧率由 I2C 决定（≈11 Hz）
- **影响面**：loopTask 节奏（`log_task()` / `event_dispatch()` / `weight_task()` / `valve_task()` 的调用间隔）
- **不构成"卡死"**：有确定上界（~92 ms），且 `valve_task()` 的安全超时是 **300 s** 量级 ⇒ **不受影响**；HX711 是 10 Hz（100 ms 周期）⇒ **92 ms 仍小于一个采样周期**，理论上不丢样
- **⇒ 结论：属"可接受的少量阻塞"，但建议列为上板实测项**（用 `millis()` 打点测 `sendBuffer()` 实际耗时）。可选缓解是 `i2c_speed` 100k→400k（**属配置项改动，需批准**；且 `oled.json` 已是可配置项，非新增）

### 5.2 安全链断裂

| 检查项 | 结论 |
|---|---|
| 新增断裂点？ | ✅ **无**（零代码改动） |
| 既有断裂点 | 📌 **`LV-1`**（`dispense_guard_init()` 忽略 `event_subscribe()` 返回值 ⇒ 安全链永久失效静默）**仍 OPEN** |
| 安全链的完整路径 | `weight.cpp:event_push` → `event_manager` → `dispense_guard` → `valve_force_close()` → GPIO —— **全链路无阻塞点**（`event_dispatch()` 每 loop 无条件调用；`valve_force_close()` 绕过 50 ms 防风暴、无条件写 GPIO） |
| 与 OLED 阻塞的关系 | `valve_force_close()` 由 `event_dispatch()` 触发，若 loop 被 OLED I2C 占 92 ms ⇒ **安全响应最坏延迟 +92 ms**。对照安全超时 300 s ⇒ **可忽略** |
| 与 BLE 的关系 | BLE 扫描在 `MiThermometer_task()`（loop）中启停，**不在中断/回调里** ⇒ 不阻塞安全链 |

### 5.3 Flash 异常消耗

| 消耗源 | Level | 速率 | 496 条环寿命 | 判定 |
|---|---|---|---|---|
| **`LOG_BLE_DECODE_FAIL`（若接入且不聚合）** | WARN | **20 / min**（bindkey 错误场景） | **≈ 25 min** | 🔴 **必须聚合** |
| `LOG_BLE_SENSOR_LOST` | WARN | ≤1 / 15 min | ≈124 h | ✅ |
| `LOG_DISPENSE_SAFETY_RESPONSE`（Phase 4 已落地） | WARN | 已合并（5 s 窗口） | ✅ | ✅ 已处置 |
| `LOG_VALVE_FORCE_CLOSE` | CRITICAL | 已 5 s 门控 | ✅ | ✅ P2-H 已处置 |
| `LOG_WEIGHT_ERROR_ENTER` | WARN | 已合并（突发） | ✅ | ✅ P2-I 已处置 |
| `LOG_SYS_HEAP_LOW`（若接入） | WARN | **若按轮记录 ⇒ 严重** | ⚠️ | **必须边沿/门控**（如"首次跨阈值记 1 条 + 每 ≥60 s 刷新"） |
| `LOG_SYS_PSRAM_ALLOC_FAILED`（若接入） | CRITICAL | 分配点数量有限（`grep` 命中 ~10 处） | ✅ | 天然低频 |

⇒ **Flash 风险结论**：**唯一新增的真实风险是 BLE `DECODE_FAIL`**（§1.5 已定量）；接入 System 段时需对 `HEAP_LOW` **预先定好门控策略**。

### 5.4 数据不可恢复

| 检查项 | 结论 |
|---|---|
| LogManager 的 INFO 只上云不落 Flash | ⚠️ **设计如此**（`LOG_LEVEL_POLICY` INFO = `{0,1}`）⇒ 重启后 INFO 无法补发。**非缺陷**，但意味着**"重启前的最后状态"只能靠 WARN+ 记录还原** |
| **`0x01xx` 整段空缺** | 🔴 **核心缺口** —— **没有任何"重启/复位"证据链**，无法区分"崩溃重启"与"正常重启"，也无法判断重启是否发生在某次操作中途（⇒ `BOOT_INCOMPLETE_PREV` 缺失） |
| `LOG_SYS_CRITICAL_OP_UNDERFLOW` 缺失 | 🔴 若 Critical Op 计数出现**下溢**（多 release），系统会**永久无法重启** —— 而当前**没有任何记录能发现它** |
| `RESTART_REQUESTED/EXECUTED/CANCELLED` 缺失 | 🔴 `system_command.cpp` 是全系统唯一 `ESP.restart()` 处（`:312`），但其"请求 → 等待计数归零 → 执行"三态**零记录** ⇒ 重启行为**不可审计** |
| 存储完整性缺口 | 🟡 `bin_storage` 未接 LogManager 桥接（`main.cpp:474` 只注册串口版）⇒ **BIN 存储的错误不进日志** ⇒ 若 Workflow BIN 损坏，云端无记录 |

### 5.5 显式声明：未重新升级 `R-7` / `R-8`

按用户指令「不要将 R-7/R-8 普通采样间隔问题重新升级」，本轮核查后**确认不动它们的优先级、不改其状态**：

| ID | 当前状态 | 本轮动作 |
|---|---|---|
| `R-7` | `DEFERRED`（回归夹具 vs 跨模块 WARN+ 冲突；夹具 0 改动、断言仍 195、168/195 通过，26 MISS 全属环境干扰） | **无动作**（未重新评估、未提优先级） |
| `R-8` | `DEFERRED`（采样窗口无时间信息 ⇒ 混合窗口假跳变；危险区间 100–500 ms） | **无动作**（§5.1 的 OLED 阻塞是**独立的另一件事**，且结论是"可接受"；**不构成对 `R-8` 的重新升级**） |

> 注：§5.1 提到"OLED 每次帧传输 ≈92 ms，与 HX711 的 100 ms 采样周期同量级" —— 这**只是量级对比**，用于说明"仍小于一个采样周期"，**不作为 `R-8` 升级依据**，也**不要求任何改动**。

---

## 六、推荐下一步开发顺序

**排序原则**（按"价值 ÷ 成本 ÷ 风险"，并尊重"不新增冻结元素"的约束）：

| 序 | 任务 | 成本 | 风险 | 价值 | 新增 ID | 推荐理由 |
|---|---|---|---|---|---|---|
| **1** | **`0x01xx` System/Boot 段接入**（SystemCommand） | ★★ 小 | **极低** | ★★★ **最高** | **0**（12 个 ID **已冻结**） | ① 判据**现成**（`esp_reset_reason()` 已缓存 + 已有名称映射）② **全部启动期一次性** ⇒ 零频率风险、零 Flash 压力 ③ 直接补齐"**数据不可恢复**"的核心证据链（§5.4）④ 已有同类先例（`system_command.cpp:312` 是唯一 `esp_reset_reason` 携带者，工作量为"接线"） |
| **2** | **BLE 接入**（4 个已冻结 ID） | ★★ 中 | **低** | ★★ 高 | **0**（若只接 4 个） | ① **4 个宿主全在 `MiThermometer_task()`（loop）** ⇒ **不需要动 callback**（§1.1/§1.3）② 只有 `DECODE_FAIL` 需聚合，模块内 `s_scan_fail_count` 可复用 ③ 唯一的 Flash 风险已定量并可闭合 |
| **3** | **`bin_storage` 接入 LogManager 桥接** | ★ 极小 | 无 | ★★ 中 | **0**（复用 Storage 段 6 个 ID） | 与 P2-A **严格同构**（`main.cpp:474` 已有 `bin_storage_set_log_callback()`，只是注册了串口版）⇒ 改 1~2 行；补齐**存储完整性缺口** |
| **4** | `LV-2` 观测工具增强（`logt fver` 打印参数值） | ★ 小 | 极低 | ★★ 中 | 0 | 让**折叠类埋点**（`0x050C` / `0x0511` / 未来的 BLE）的 **ΣCOUNT 守恒可在纯串口侧自证** ⇒ 一次性投入长期受益（仅测试控制台，不影响产品逻辑） |
| **5** | OLED（**维持不埋**） | — | — | ★ 低 | 0 | 缺判据（`oled.begin()` 返回值未检查）⇒ 埋点无依附点；若用户批准修那 1 处检查，再接 `0x0E01` |
| **6** | `workflow_storage` 回调接口 | ★★ 中 | 中 | ★ 低 | 0 | 需**先加接口**（与 P2-A 不同构）⇒ 成本最高、价值最低 |
| **7** | **Dispense 业务开发**（电机/出粮/卡料/超时/重试） | ★★★ 高 | **高** | ★★★ 高 | 需新增 | 🔴 **阻塞于 `DSP-1`/`DSP-2`/`DSP-4` 裁决**（§3.2 已给出建议）⇒ **不得先于 1–6 开工** |
| — | `LV-1` / `PROTO-1`（协议缺口） | ★ 小 | 低 | ★★ 中 | 需裁决 | 见 §七 |

**⇒ 一句话建议**：
> **先做 #1（System/Boot 段，判据与 ID 都现成、零新增、直接补上"不可恢复"的证据链），再做 #2（BLE，宿主全在 loop、风险已定量），然后 #3/#4 收尾；#7 Dispense 业务等裁决。**

---

## 七、需要确认的决策点列表

| # | 决策点 | 选项 | 影响 | 建议 |
|---|---|---|---|---|
| **D1** | **是否启动 `0x01xx` System/Boot 段接入？接入哪些？** | (a) 全部 12 个<br>(b) **仅"复位证据链"子集**：`BOOT_COMPLETE` / `BOOT_INCOMPLETE_PREV` / `RESET_ABNORMAL` / `RESET_NORMAL` / `RESTART_REQUESTED` / `RESTART_EXECUTED` / `RESTART_CANCELLED`（7 个）<br>(c) 不做 | 决定"崩溃重启循环"能否被诊断 | **推荐 (b)** —— 聚焦"数据不可恢复"，避开与 `LOG_STG_FS_UNAVAILABLE` 语义重叠的 `FS_MOUNT_FAILED`；`HEAP_LOW` 单独评审（需门控策略） |
| **D2** | **`LOG_SYS_FS_MOUNT_FAILED`(0x0106) 与 `LOG_STG_FS_UNAVAILABLE`(0x0301，已有宿主) 的边界？** | (a) 各记各的（分层）<br>(b) **只保留 STG 一个**（SYS 的登记为"语义重复、不落地"） | 决定是否产生重复记录 | **推荐 (b)** —— 二者都在描述"文件系统不可用"，而 STG 侧已由 `stg_bridge_event()` 的 `STG_OP_FS_UNAVAILABLE` 覆盖 |
| **D3** | **`LOG_SYS_HEAP_LOW`(0x0108) 的门控策略？** | (a) 每次检测到就记<br>(b) **跨阈值边沿 + ≥60 s 刷新**<br>(c) 不接 | 决定是否引入新的 Flash 消耗 | **推荐 (b)** —— 按轮记录会在内存枯竭时形成**日志风暴**（正是 `R-3` 的同类问题） |
| **D4** | **BLE 是否本轮接入？只接已冻结的 4 个，还是要新增 `BLE_INIT_FAILED` / `BLE_QUEUE_FULL`？** | (a) **只接 4 个**（零新增）<br>(b) 4 + 2 个新增（`INIT_FAILED` ERROR、`QUEUE_FULL` WARN） | 新增 ID 数量 | **推荐 (a) 先做** —— 与"不随意新增 EventId"一致；两个新增项先登记，后续单独立项（它们都是**真实缺口**：`:247` 的 `return false` 被 `main.cpp:508` 忽略 ⇒ 与 `LV-1` 同构） |
| **D5** | **`BLE_DECODE_FAIL` 的聚合方式？** | (a) **模块内窗口聚合**（复用 `s_scan_fail_count`，窗口结束上报 1 条带 `LOG_P_FAIL_COUNT`）<br>(b) **挂进 LogManager 突发合并白名单**（5 s，ΣCOUNT 守恒） | 改不改 `log_manager.cpp` | **推荐 (a)** —— **零 LogManager 改动**，且与模块既有的"窗口语义"天然对齐（注意若选 (b) 必须满足**铁律 30**：至少带 1 个参数） |
| **D6** | **OLED 是否维持"不埋点"？是否登记 `oled_init()` 忽略 `begin()` 返回值？** | (a) **维持不埋 + 登记缺陷**（新编号 **`OLED-1`**）<br>(b) 批准修 `begin()` 检查（~3 行）后接入 `0x0E01`<br>(c) 维持现状、不登记 | 决定 OLED 是否算"已覆盖" | **推荐 (a)** —— 保持零代码改动；把"假成功"作为**生产代码缺陷**独立登记（与 `VALVE-1` / `LV-1` 同类），是否修由用户另批 |
| **D7** | **`PROTO-1`：`event_dict_ver = LOG_RECORD_VERSION` 的协议缺口如何处置？** | (a) **仅登记**（云端以"未知 EventId 容忍"兜底）<br>(b) 引入真正的 `LOG_EVENT_DICT_VER`（**属冻结协议变更**）<br>(c) 云端自适应 | 决定是否需要动冻结协议 | **推荐 (a)** —— 与"不修改冻结协议"一致；**(b) 必须单独立项评审** |
| **D8** | **`LV-2`（`logt fver` 不打印参数值）是否本轮做？** | (a) **做**（测试控制台 +~20 行，不影响产品逻辑）<br>(b) 不做 | 折叠类埋点的 ΣCOUNT 能否纯串口自证 | **推荐 (a)** —— 收益纵贯后续所有折叠埋点（BLE `DECODE_FAIL` 也要用） |
| **D9** | **`DSP-1`（命名）与 `DSP-2`（日志段）是否现在裁决？** | (a) `DispenseManager`(编排) + `Motor`(驱动)，日志段 = **`0x0Fxx`**（`0x0501–0x0504` 继续不用）<br>(b) 其他 | 决定 §3 的边界设计能否定稿 | **推荐 (a)** —— 与 `readme.md` 既有命名 + `LOG_MOTOR_RESERVED_BASE` 预留意图一致，且与 `0x05xx`（供水）形成干净两域划分 |
| **D10** | **`LV-1`（DispenseGuard 订阅失败不可观测）是否随 #1/#2 一并批准？** | (a) 批准（+1 ID）<br>(b) 继续挂起 | 安全链可观测性 | **推荐 (a)** —— 与 #1 的"补齐不可观测点"主题一致，一次评审解决 |

---

## 附录 A：本轮核查的源文件与工具

| 文件 / 工具 | 用途 |
|---|---|
| `src/MiThermometer.cpp`（**700 行**） | BLE 全文结构：callback（`:114-167`）· 7 态状态机（`:315-456`）· 解码队列消费（`:462-501`）· `lywsd03_decrypt()`（`:507-570`）· 初始化（`:222-304`） |
| `src/MiThermometer.h`（26 行） | 对外接口 |
| `src/oled.cpp`（301）+ `src/oled.h`（19）+ `src/oled_animation.h`（1367） | OLED：`oled_init()`（`:75-112`）· `oled_task()`（`:120`）· `oled_show_frame()`（`:154`）· `oled_event_handler()`（`:213`）· `oled_restart()`（`:251`，**死代码**）· `oled_event_init()`（`:269`） |
| `src/system_command.cpp` | ★ `esp_reset_reason()` 缓存（`:222`）与名称映射（`:97`） |
| `src/log_events.h` | 102 个 EventId 定义 / Level Policy / `LOG_RECORD_VERSION=2` / `LOG_BKEY_EVENT_DICT_VER` |
| `src/log_manager.cpp` | `log_emit_internal()`（`:1703-1870`，**无 I/O/malloc/delay**）· 突发合并（`:131-322`）· ★ `h.event_dict_ver = LOG_RECORD_VERSION`（`:2282`） |
| `src/log_cbor.h`（95-103） | 批次头结构（`event_dict_ver` 的注释语义） |
| `.pio/libdeps/*/U8g2/src/U8g2lib.h:144` | ★ `bool begin(void)` —— 证明返回值存在且被忽略 |
| `.pio/libdeps/*/NimBLE-Arduino/src/nimconfig.h:217` | NimBLE host task 栈 = **4096 B** |
| `data/config/oled.json` | `i2c_speed=100000`（100 kHz）· `frame_delay=50` |
| `docs/P2_Log_Integration_Matrix.md` §11 | 既有 BLE 契约与 🔴 callback 硬禁令 |
| `log模块历史/P2-BLE性能清理0920.md` | 速率实测（≈1 包 / 3 s）· callback 上下文分析 · `P0-3` 处置记录 |
| `.pio/p5run/evtid_audit.py`（本轮新建，可复用） | EventId 宿主审计脚本（含 `log_emit0` 与映射函数 `return` 两类证据，并剥除注释以避免假阳性） |

## 附录 B：方法学提醒（本轮踩到并已修正的坑）

**首版审计脚本只匹配 `log_emit(` + 字面实参 ⇒ 产生两类假阴性：**
1. **`log_emit0(...)`** 未被匹配 ⇒ 误判 `LOG_CFG_SAVE_OK` 无宿主（实际 `config_manager.cpp:1347/2827` 有）
2. **映射函数 `return LOG_XXX;`** 未被匹配 ⇒ 误判 `0x02xx`/`0x03xx` 两段大面积无宿主（实际由 `cfg_bridge_event()` / `stg_bridge_event()` 驱动）

**⇒ 新铁律补充：统计"埋点宿主覆盖率"时，必须同时检索「字面实参」+「`log_emit0` 变体」+「映射函数 return 值」三类证据，并剥除注释；否则 Storage/Config 这类"桥接 + 映射"的模块会被系统性误判为"零宿主"。**

## 附录 C：本轮**未**做的事（明确边界）

- ❌ 未修改任何 `src/` / `test/` 文件（`git diff --stat src/` 为空）
- ❌ 未新增任何 EventId / ParamId / 配置项 / enable 开关
- ❌ 未修改 System State / EventManager / LogManager 的任何代码
- ❌ 未启动 Dispense 业务（电机 / 出粮 / 卡料 / 超时 / 重试）
- ❌ 未重新评估或升级 `R-7` / `R-8`
- ❌ 未上板（无串口设备）⇒ 本报告中的速率/耗时均为**代码推导 + 既有实测引用**，凡属估算处均已标注

---

**最后更新**：2026-09-20 · 下一阶段（Phase 5 候选）前置审查
**状态**：**审查完成，等待 §七 的 D1–D10 裁决后再动代码**
