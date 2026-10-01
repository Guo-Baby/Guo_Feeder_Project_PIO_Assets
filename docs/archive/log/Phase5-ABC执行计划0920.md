# Phase 5-A / 5-B / 5-C 执行计划（2026-09-20）

> **性质**：实施前计划（本轮**不修改任何代码**，`git diff --stat src/ test/` 必须为空）。
> **前置**：Phase 5 前置审查已确认（`5b5b529` `docs(review): phase 5 boundary review` + `9d5eae4` `docs(progress): record phase 5 review findings`）。
> **冻结约束**：不修改已有 EventId 编号 / 不修改 ParamId / 不增加配置项 / 不增加临时 enable 开关 / 不破坏 System State 与 EventManager 架构。
> **已确认范围**：OLED 维持"不埋点"（仅登记 `OLED-1`）；Dispense 未来使用 `0x0Fxx` 域，不提前新增日志；`HEAP_LOW` / `PSRAM_ALLOC_FAILED` / `FS_MOUNT_FAILED` 本阶段不接入。

---

## 0. 结论速览

| 阶段 | 可执行性 | 协议改动 | 主要阻塞 |
|---|---|---|---|
| **5-A** System/Boot（7 ID） | ⚠️ **6 个可直接接入**；**1 个（`RESTART_CANCELLED`）当前架构下无宿主** | **零** —— 7 个 EventId 与所需 6 个 ParamId **全部已冻结**，⇒ **`log_events.h` 零改动**（`LOG_SYS_BOOT_COMPLETE` 甚至已有 `static_assert`，`:608`） | E1 / E2 / E3 |
| **5-B** EVT-1 | ✅ 缺陷链已精确定位（**结论与先前表述不同，见 §3**）；★ **且必须连带修 `P0-1`**（幻影条目导致 index ≥10 全部错位） | 零 EventId 改动（1 个宏 + 2 张字符串表，**删 1 个幻影条目**） | E6 |
| **5-C** BLE（4 ID） | ✅ 3 个有现成宿主；**`DECODE_FAIL` 当前无 `else` 分支，需新增** | **零** | E7 / E8 |

**可在你确认 E1–E8 后立即开工**；若只批准"无争议项"，5-A 也可先做 5-A-1 / 5-A-3 两组（共 5 个 ID），把 `RESTART_CANCELLED` / `RESTART_EXECUTED` 单独留待裁决。

---

## 1. 事实核查（本轮新增 / 修正）

### 1.1 5-A 七个 ID 的宿主定位（唯一语义宿主）

| EventId | Level | **宿主（= 事实定义点）** | 参数（全部复用） | 备注 |
|---|---|---|---|---|
| `LOG_SYS_RESET_ABNORMAL` | 0x0103 | CRITICAL | `src/system_command.cpp:222` `s_reset_reason = (uint8_t)esp_reset_reason();` **之后**（同函数内） | `LOG_P_RESET_REASON`(0x20) | 判据函数需新增（见 §2.1）；`:95` 已有"复位原因→名称"映射可复用 |
| `LOG_SYS_BOOT_INCOMPLETE_PREV` | 0x0102 | CRITICAL | `src/config_manager.cpp:1108` `bool last_boot_ok = json_storage_exists(CONFIG_BOOT_FLAG_FILE);` **之后** | `LOG_P_RESET_REASON`(0x20) | ★ **机制已存在**：`:1098-1114` 注释原文「标记缺失 = 上一次启动未成功（panic / 反复重启）」 |
| `LOG_SYS_BOOT_COMPLETE` | 0x0101 | INFO | `src/main.cpp:553` `config_boot_validate()` **返回 true 之后**（setup `:414` 末尾） | `LOG_P_INIT_MS`(0x22) | 与 boot flag **严格对齐**：只有走到这里 flag 才被写入 |
| `LOG_SYS_RESTART_REQUESTED` | 0x0109 | INFO | `src/system_command.cpp:356` `if (first_request)` 分支内 | `LOG_P_COUNT`(0x45) = 请求时的 critical 计数 | 定义点 = "首次请求被接受"，**不在** `system_command_request_restart()` 入口、**不在** 4 个调用方 |
| `LOG_SYS_RESTART_EXECUTED` | 0x010A | INFO(IMM) | `src/system_command.cpp:309` 之后、`:312` `ESP.restart();` **之前** | 无参 | ⚠️ **送达风险**，见 §1.3 |
| `LOG_SYS_RESTART_CANCELLED` | 0x010B | INFO | **⚠️ 无宿主** | — | ⚠️ 架构明示不可取消，见 §1.2 |
| `LOG_SYS_CRITICAL_OP_UNDERFLOW` | 0x010C | CRITICAL | `src/system_command.cpp:464` `if (!ok)` 下溢分支内 | 无参 | 唯一出口（`:469` 已有 `syscmd_log("E", ...)`） |

**调用顺序已核验**（埋点必须晚于 `log_init()`）：

```
main.cpp:444  log_init()            ← RAM 环就绪
main.cpp:483  config_init()         ← BOOT_INCOMPLETE_PREV 宿主（:1108）
main.cpp:518  command_manager_init()
   └ command_manager.cpp:534  system_command_init()   ← RESET_ABNORMAL / RESTART_* 宿主
main.cpp:553  config_boot_validate() ← BOOT_COMPLETE 宿主
main.cpp:567  loop(){ system_command_task(); ... log_task(); }
```

⇒ 全部宿主**晚于** `log_init()`，记录不会因环未就绪而丢失。

### 1.2 ★ 新发现①：`LOG_SYS_RESTART_CANCELLED` **当前架构下无宿主**

`src/system_command.h` 三条硬性规则原文：

> 1. Restart 一旦被请求，**不可取消**（不提供 cancel 接口）。
> 2. Restart 可以被延迟（Critical Count > 0），但不能被撤销。
> 3. 进入 RESTART_PENDING 后禁止新的 Critical Operation。

代码核验：`system_command.cpp` 全文 **无 `cancel` 字样**；`system_command_task()` 的状态机 `IDLE → REQUESTED → PENDING → RESTARTING` **单向不可逆**（`:254-320` 无回退分支）；`system_command_request_restart()` 返回 `false` 的唯一分支是 `RESTARTING`（`:348-350`），语义为"**拒绝**"而非"**取消**"。

⇒ **`0x010B` 在本架构下是永不触发的 ID**。若要接入，只能挂到"被拒绝"分支 —— 那是**语义伪造**（把"拒绝"写成"取消"）。详见决策点 **E1**。

### 1.3 ★ 新发现②：`(IMM)` 注解**没有实现** ⇒ `RESTART_EXECUTED` 存在真实丢失风险

| 证据 | 内容 |
|---|---|
| `log_manager.cpp:1962-1975` | `s_flush_requested` **只在 `is_critical`（CRITICAL）时置位** |
| `log_manager.cpp:3290-3303` | `log_flush_requested()` / `log_clear_flush_request()` —— **生产代码零消费方**（全仓库唯一引用是 `main.cpp:1516` 的测试台 `logt reset`） |
| `log_manager.cpp:2743-2781` | 发送由 `s_cloud_inflight` + `s_cloud_deadline_ms = millis() + s_ack_timeout_ms` 驱动 |
| `log_manager.cpp:2883-2886` / `2900-2905` | 发送闸门：① 等 ACK 在途 ② 退避中 ③ **节流 `LOG_TX_MIN_INTERVAL_MS`** |
| 实测（`BT-1`） | 上行实际频次 **≈ 1 批 / 15 s**（受 `LOG_ACK_TIMEOUT_MS` 支配） |

标注 `(IMM)` 的 5 个 ID 中：`0x0505/0x0506/0x0507/0x0703` 都是 **WARN+/ERROR** ⇒ "立即"由"**立即落 Flash**"天然满足（重启后可补发）；**只有 `LOG_SYS_RESTART_EXECUTED = 0x010A` 是 INFO** ⇒ 不落 Flash（`log_level_to_flash`）⇒ **记完 10 s 后 `ESP.restart()` 会清空 RAM 环，记录永久丢失**，除非恰好在 10 s 内触发一次上行。

`SYSTEM_RESTART_SAFE_DELAY_MS = 10000`（`system_command.h:59`）**< 15 s** ⇒ 风险成立。

> ✅ 附带修正：**日志云端上行链路本身是通的**（`log_manager.cpp:2763` 直连 `cloud_manager.cpp:1795` `cloud_send_log()`；`CloudManager` 不引用 `log_cbor_*` 是因为 CBOR 编码在 LogManager 侧，**不是缺口**）。

### 1.4 ★ 新发现③：`BOOT_INCOMPLETE_PREV` 的判据**已存在**，但需消解"首次启动"歧义

`CONFIG_BOOT_FLAG_FILE = "/config/.bootok"`（`config_manager.h:81`）：
- `config_init()` `:1108` **读**（`last_boot_ok`）→ `:1116` **删** → 本次启动必须重新证明能走完 setup
- `config_boot_validate()` `:1377` **写**

⇒ **不需要新增任何持久化机制**（这是 Phase 5 审查时担心的点，现已排除）。

但 `!last_boot_ok` 有两种成因：① 上次启动崩溃/中断；② **首次烧录 / `uploadfs` 后首次启动**（flag 文件从不存在）。详见决策点 **E3**。

### 1.5 ★ 新发现④：BLE `DECODE_FAIL` **当前没有 `else` 分支**

`MiThermometer.cpp:468-501`：

```cpp
bool result = lywsd03_decrypt(...);
if(result)
{
    ...                        // ← 只有成功分支
}                              // ← :501 循环体结束，**没有 else**
```

⇒ `LOG_BLE_DECODE_FAIL`(0x0902) **需要新增 `else` 分支**（纯增量：只累加计数，无 `return` / 无行为变化）。

而 `lywsd03_decrypt()`（`:507-512` 等）有 **6 个 `return false` 出口**（长度不足 / 头部不符 / cipher 长度非法 / setkey 失败 / CCM mic 失败 …），回调侧已做 MAC + 长度过滤（`:124-126` / `:135`）⇒ 实际失败 = **bindkey 错误或格式异常**，这正是 `DECODE_FAIL` 要记的事实。

### 1.6 5-B 事实修正：`EVT-1` 的真正断点是 **`event_from_string()`**，不是发布路径

| 环节 | 边界宏 | 阀门事件（14/15/16）是否通过 |
|---|---|---|
| `event_push()` | **`MAX_EVENT_TYPE`(32)** | ✅ **通过** ⇒ 发布、入队、订阅回调**全部正常** |
| `event_subscribe()` | **`MAX_EVENT_TYPE`(32)** | ✅ **通过** ⇒ 可订阅 |
| `event_manager_init()` | **`MAX_EVENT_TYPE`(32)** | ✅ 正常清理 |
| **`event_from_string()`** | `SYSTEM_EVENT_COUNT`(14) + `event_names[]` **仅 14 项**（`event_manager.cpp:180-199`，**无阀门条目**） | ❌ **永远解析失败 ⇒ 返回 `EVENT_NONE`** |
| **`event_to_string()`** | `SYSTEM_EVENT_COUNT`(14)（`:176`） | ❌ **返回 `"EVENT_UNKNOWN"`** |

**真实后果**（比先前表述更精确）：
1. `workflow.cpp:4658/4687` `event_from_string(trigger->id)` ⇒ **Workflow 无法用字符串注册/响应任何阀门事件，且静默失败**；
2. `valve.cpp:158/171/374` 发布的阀门事件**能被订阅者收到**，但一旦需要转字符串（上报/日志/诊断）就变成 `"EVENT_UNKNOWN"` ⇒ **事件身份丢失**；
3. `dispense_guard` 等既有安全链**不受影响**（它订阅的是 `EVENT_WEIGHT_ERROR`=12）。

⇒ 修复面比先前判断**更窄、更安全**：只需 1 个宏 + 1 张字符串表，**不触碰发布/订阅/派发任何逻辑**。

---

## 2. Phase 5-A 实施计划（System/Boot 证据链）

**协议改动：零。** `src/log_events.h` **不修改**（7 个 ID 与所需 ParamId 全部已冻结）。
预计改动：`system_command.cpp`（约 +70 行，含注释）、`config_manager.cpp`（约 +20 行）、`main.cpp`（约 +12 行）。

### 2.1 5-A-1 复位证据链（2 个 ID）

| 步骤 | 文件 | 内容 |
|---|---|---|
| A1-1 | `system_command.cpp` | 新增 `static bool syscmd_reset_reason_is_abnormal(uint8_t r)`：仅把 **已知异常** 判为异常 —— `ESP_RST_PANIC / INT_WDT / TASK_WDT / WDT / BROWNOUT / SDIO` ⇒ `true`；`POWERON / EXT / SW / DEEPSLEEP / UNKNOWN` ⇒ `false`；`default` ⇒ `false`（**fail-safe：宁可漏报不误报**） |
| A1-2 | `system_command.cpp` | `system_command_init()` `:222` 之后：`if (异常) log_emit(LOG_SYS_RESET_ABNORMAL, LOG_LVL_CRITICAL, {LOG_P_RESET_REASON}, 1)` |
| A1-3 | `config_manager.cpp` | `:1108` 之后：`if (!last_boot_ok) log_emit(LOG_SYS_BOOT_INCOMPLETE_PREV, LOG_LVL_CRITICAL, {LOG_P_RESET_REASON}, 1)` |

**为何 `BOOT_INCOMPLETE_PREV` 放在 `config_manager.cpp`**：该事实由 **boot flag 机制**定义（`ConfigManager` 拥有读写两侧），按**铁律 22（埋定义点）** 归 ConfigManager；`0x01xx` 仅表示"系统生命周期语义"，不要求发布方属于 System 模块。
**替代方案**：ConfigManager 只置一个 RAM 标志，由 `system_command_task()` 记录 —— 引入跨模块胶水，**不推荐**（决策点 **E4**）。

### 2.2 5-A-2 重启生命周期（2 个 ID，含 1 个待裁决）

| 步骤 | 文件 | 内容 |
|---|---|---|
| A2-1 | `system_command.cpp` | `:356` `if (first_request)` 分支内：`log_emit(LOG_SYS_RESTART_REQUESTED, LOG_LVL_INFO, {LOG_P_COUNT = cnt}, 1)` |
| A2-2 | `system_command.cpp` | `:309` 之后、`:312` `ESP.restart()` **之前**：`log_emit(LOG_SYS_RESTART_EXECUTED, LOG_LVL_INFO, nullptr, 0)` ⚠️ **待 E2 裁决** |
| A2-3 | `system_command.cpp` | `LOG_SYS_RESTART_CANCELLED` ⚠️ **无宿主，待 E1 裁决**（预计"不接入 + 登记"） |

**唯一执行路径已确认**：`ESP.restart()` 全系统只出现在 `system_command.cpp:312` 一处，且 `RESTARTING` 状态在 `:306` 先置位、`:310` `Serial.flush()` 之后才调用 ⇒ **宿主唯一、无重复可能**。

### 2.3 5-A-3 溢出保护 + 启动锚点（2 个 ID）

| 步骤 | 文件 | 内容 |
|---|---|---|
| A3-1 | `system_command.cpp` | `:464` `if (!ok)` 内：`log_emit(LOG_SYS_CRITICAL_OP_UNDERFLOW, LOG_LVL_CRITICAL, nullptr, 0)`（与既有 `syscmd_log("E", ...)` 并存，后者是串口兜底） |
| A3-2 | `main.cpp` | setup `:553` 之后：`log_emit(LOG_SYS_BOOT_COMPLETE, LOG_LVL_INFO, {LOG_P_INIT_MS}, 1)`，`LOG_P_INIT_MS` = `millis()` − setup 起点（`millis()` 在 `setup()` 开头采样，**不新增全局状态**，用函数内局部即可） |

**为何 `BOOT_COMPLETE` 不带 `LOG_P_BOOT_SEQ`**（虽然 `0x21` 已冻结）：
① LogManager **没有 `boot_seq` 公开 getter**（`log_manager.h:149/316` 是结构字段，无访问器）；
② `boot_seq` **已经在每个批次的批次头里**（`log_cbor.h:101`）⇒ 再作为参数记一次是**冗余**（铁律 31：先问"现有机制缺哪一维"——这里一维都不缺）。
⇒ 结论：**只带 `INIT_MS`**，零新增、零冗余。

**为何 `INSERT` 在 `config_boot_validate()` 成功之后**：只有到这一行 boot flag 才真的被写入 ⇒ `BOOT_COMPLETE` 出现 **⇔** 下次启动不会报 `BOOT_INCOMPLETE_PREV`，两条记录构成**严格互斥的证据对**。若 validate 失败则**不记**（此时串口已有 `[WARN] Config boot validate failed`，且下次启动必然报 `BOOT_INCOMPLETE_PREV` ⇒ 不缺证据）。

---

## 3. Phase 5-B：`EVT-1` 修复方案（**待确认后实施，不与日志混合**）

### 3.1 方案对比（均**不改任何 EventId 数值**）

| 方案 | 改动 | 效果 | 风险 |
|---|---|---|---|
| **A（推荐）** | `event_manager.h:53` `#define SYSTEM_EVENT_COUNT (EVENT_VALVE_ERROR + 1)`（14 → **17**）+ `event_manager.cpp` `event_names[]` **补 3 项**（`"EVENT_VALVE_OPEN"/"CLOSE"/"ERROR"`） | ✅ `event_from_string` / `event_to_string` 双双修复 ⇒ **Workflow 可响应阀门事件** + 上报不再 `"EVENT_UNKNOWN"` | `SYSTEM_EVENT_COUNT` 仅被 `:161`/`:176` 两处使用（已 grep 确认），扩大上界=**修正行为**；发布/订阅用 `MAX_EVENT_TYPE` ⇒ 不受影响 |
| **B（保守）** | 不动 `SYSTEM_EVENT_COUNT`，只把 `event_names[]` 补到 17 项、`event_to_string()` 上界改为 `MAX_EVENT_TYPE` | 只修"上报可读性"，**不修**"Workflow 响应能力" | `event_from_string` 上界仍是 14 ⇒ 阀门 Trigger 仍静默失效（只解决一半） |
| **C** | 新增独立的"可触发事件计数"宏 | 语义最清 | 属架构改动（新增宏 + 改两处判定），代价最大 |

### 3.2 推荐 A 的论证

`event_names[]` 本质是**全枚举的字符串表**（0..13 共 14 项，恰好等于当时的 `SYSTEM_EVENT_COUNT`）。阀门 3 个事件被加入枚举时**字符串表与计数宏都没同步** ⇒ 这是**一致性缺陷**，不是"有意排除"。而 `event_subscribe()` / `event_push()` 一律以 `MAX_EVENT_TYPE` 为界 ⇒ **架构上阀门事件本来就是"可发布、可订阅"的**，只是字符串桥漏了 ⇒ 补齐 = **修复不一致**，而非扩权。

⚠️ **必须同时说明的副作用**（诚实标注）：修复后 `"EVENT_VALVE_OPEN"` 等字符串变成**可注册的 Trigger** ⇒ 若云端/Workflow 里已存在此类字符串，它们会**从"静默无效"变为"真正生效"**（行为变化）。实施前需 grep `data/` 与云端配置确认（本轮已确认 `src/` 内无此类 Trigger 字面量）。

**验收要求**：编译 + 一段"字符串 ↔ 枚举"往返自测（可用 `logt` 之外的主机侧单元思路：在 `test/` 里加 `event_roundtrip` 断言；或在 `cm` 控制台加一次性回显）。**不改事件机制、不改风暴策略、不改队列**。

### 3.3 ★ 5-B 必须连带修复 `P0-1`（否则修不干净）

`event_manager.cpp` 的**两张字符串表都多出一个 enum 里不存在的幻影条目** `"EVENT_CLOUD_COMMAND"`（`event_from_string` `:143-159` 与 `event_to_string` `:180-199`，**都插在 index 10**）：

| index | 枚举真值 | 字符串表 | **错位结果** |
|---|---|---|---|
| 10 | `EVENT_COMMAND_RESULT` | `"EVENT_CLOUD_COMMAND"` ❌ | `event_to_string(10)` 输出错误名称 |
| 11 | `EVENT_WEIGHT_READY` | `"EVENT_COMMAND_RESULT"` ❌ | — |
| 12 | `EVENT_WEIGHT_ERROR` | `"EVENT_WEIGHT_READY"` ❌ | **`event_to_string(EVENT_WEIGHT_ERROR)` 显示成 `WEIGHT_READY`** ⇒ 诊断/上报误导 |
| 13 | `EVENT_ERROR` | `"EVENT_WEIGHT_ERROR"` ❌ | — |
| 14 | — | `"EVENT_ERROR"` | 超出 `SYSTEM_EVENT_COUNT` ⇒ `"EVENT_ERROR"` **永远解析不到** |

**真实后果（比 `P0-1` 原登记"串口事件名错位、误导诊断"严重）**：
`workflow.cpp:4657-4662` 订阅侧与 `:4687` 比较侧**都**走 `event_from_string()` ⇒ 错位在校验时是"自洽"的，但**发布侧用的是编译期枚举**（`weight.cpp` 推的是 `EVENT_WEIGHT_ERROR = 12`，永不推 13）⇒
> 一个声明 `event_weight_error` 的 Workflow Trigger，会被解析成 `EVENT_ERROR`(13)，于是**永远不会被真正的重量异常唤醒**，且**静默无报错**。

⇒ 与 `EVT-1` **同源同形**（都是"字符串桥与枚举不同步"），必须**同批修**，否则把 `SYSTEM_EVENT_COUNT` 扩到 17 只会让错位延伸到阀门事件。

**影响面已核验 = 零**：`event_*` 触发器字面量在全仓库（`src/` / `data/` / `test/`）**零使用者** ⇒ 修复**无现存配置回归风险**，属"修好一个潜伏缺陷"。

**修复内容（并入 5-B，不改任何 EventId 数值）**：删除两张表中的 `"EVENT_CLOUD_COMMAND"` 一行，使表与枚举**逐项对齐**，再追加 3 个阀门条目 ⇒ 两张表各 **17 项**，与 `SYSTEM_EVENT_COUNT = (EVENT_VALVE_ERROR + 1) = 17` 严格一致。

**回归要求（5-B 专属）**：① 跑 Workflow 小回归（`test/wf_contract_tests.txt` / `wf_final_tests.txt` / `wf_sync_tests.txt`）确认 `variant` / `changed` 类断言不受影响；② 新增 1 组**往返断言**（`event_to_string(event_from_string(x)) == x` 覆盖全部 17 项）—— 这是本缺陷的**唯一有效判据**（不能用"某动作产生几条日志"间接验证）。

**`P0-1` 的状态需同步升级**：`未修复的问题.md` 当前把它列在"🟡 低优先级 / 清理类"并描述为"误导诊断"，应升级为**功能性缺陷**（`EVT-1` 同族）并按本计划一并关闭。

---

## 4. Phase 5-C 实施计划（BLE，4 个已冻结 ID）

**协议改动：零。** 不新增 BLE EventId；`callback(onResult)` **零 `log_emit`**；全部日志在 `MiThermometer_task()`（loop 上下文）。

| EventId | Level | 宿主 | 参数（全部复用） | 聚合 |
|---|---|---|---|---|
| `LOG_BLE_DATA_DECODED` | 0x0901 | INFO | `MiThermometer.cpp:468` `if(result)` 内（`switch(data_type)` 之后） | 按 `data_type` 取 **`LOG_P_TEMP`(0x3B) / `LOG_P_HUMID`(0x3C) / `LOG_P_BATT_V`(0x3D)** 三者之一 | ❌ 无需（≤3 条/窗口；收到温+湿即停扫） |
| **`LOG_BLE_DECODE_FAIL`** | 0x0902 | WARN | **需新增 `else` 分支**（`:501` 前） | **`LOG_P_FAIL_COUNT`(0x3F) + `LOG_P_WINDOW_MS`(0x4A)** | ✅ **模块内窗口聚合** |
| `LOG_BLE_SENSOR_LOST` | 0x0903 | WARN | `:363` `s_scan_fail_count++;` 之后 | `LOG_P_FAIL_COUNT`(0x3F) + `LOG_P_BLE_LEVEL`(0x51) | ❌ 无需（≤1/窗口，15/30 min） |
| `LOG_BLE_SCAN_DISABLED` | 0x0904 | INFO | `:372-381` 进入 LEVEL2 分支内 | `LOG_P_FAIL_COUNT`(0x3F) + `LOG_P_BLE_LEVEL`(0x51) | ❌ 无需（1 次/生命周期） |

### 4.1 `DECODE_FAIL` 模块内聚合设计（对齐 Phase 4 的 `evt_report_faults()` 模板）

```cpp
// 模块级 static（MiThermometer.cpp 已有 s_scan_fail_count 等同类量）
static uint16_t   s_decode_fail_count = 0;
static uint32_t   s_decode_fail_win_ms = 0;

// 节拍器：放在 MiThermometer_task() **最开头**（在 xRawAdvQueue 空守卫之前）
//   理由同 P2-M：不能被后面的早退/switch 吞掉节拍
static void mi_ble_fault_tick();
```

- 窗口：**60000 ms**（与 P2-M / P2-D 一致），**无异常完全静默且不推进窗口**
- 累加在 `else` 分支内：`if (s_decode_fail_count < 0xFFFF) s_decode_fail_count++;` —— **只累加，不 emit**
- 上报：窗口到期且计数 > 0 ⇒ 1 条 WARN，`LOG_P_FAIL_COUNT = s_decode_fail_count`，随后清零 ⇒ **Σ COUNT = 真实解码失败帧数**
- **绝不改 `if(result)` 的成功分支、绝不 return、绝不改任何判定**（限流只限日志）

### 4.2 Flash 压力定量（用户要求）

| 场景 | 无聚合（逐帧记） | 本方案（60 s 窗口） |
|---|---|---|
| bindkey 错误，持续失败 | 入队 ≈ 1 帧/3 s（`onResult` 已被 MAC+长度过滤）⇒ **20 条/min** ⇒ 496 条环 **≈ 25 min 冲满** | **1 条/min** ⇒ 496 条环 **≈ 8.3 h**；**1440 条/天** |
| Flash 擦写寿命 | — | 16 段 × 4 KB；≈ 87 次整环旋转/月 ⇒ **单段擦写 ≈ 1044 次/年 ≪ 100 k 次** ✅ |

⇒ **降幅 20×**，且 `Σ COUNT` 零丢失。若你希望更保守，可把窗口提到 **300 s**（⇒ 288 条/天，环可撑 41 h）—— 见决策点 **E7**。

### 4.3 附带登记建议（不改代码）

`:165` `xQueueSend(xRawAdvQueue, &item, 0);` —— **返回值被忽略**（0 超时，队列满即静默丢包，容量 `:277` = 16）。这是 `0x09xx` 无对应 ID 的"有宿主候选但无 EventId"缺口，性质与 `LV-1` 相同 ⇒ 建议登记 **`LV-3`**（见决策点 **E8**）。**本阶段不修**。

---

## 5. 验证矩阵

### 5.1 ★ 关键约束：INFO 与 WARN+ 的**可观测路径不同**

| 记录 | Level | 落 Flash？ | 上板可读路径 |
|---|---|---|---|
| `BOOT_COMPLETE` / `RESTART_REQUESTED` / `RESTART_EXECUTED` / `SCAN_DISABLED` / `DATA_DECODED` | **INFO** | ❌ | **`logt ring`**（RAM 环，**本会话内**）+ MQTT `[Cloud LOG] OK` |
| `RESET_ABNORMAL` / `BOOT_INCOMPLETE_PREV` / `CRITICAL_OP_UNDERFLOW` / `DECODE_FAIL` / `SENSOR_LOST` | WARN+ | ✅ | `logt flash` / `fver` / `frec`（**跨重启可查**）+ `logt ring` |

> **已确认 `logt` 支持 `ring` 操作**（op 全量：`atimeout help stats ring flush flash fseg fver fwipe meta mwipe mcorrupt mfail ffail cloud cpush creset ack cfail sonline ackauto fcorrupt fbrec ftrunc reset policy emit fill mix overlimit`）⇒ **INFO 记录不必依赖云端即可自证**，这解决了 `LV-2` 对本次验证的阻碍（`LV-2` 只影响"参数值"的读取，不影响"记录存在性"）。

### 5.2 用例

| 项 | 方法 | 期望 |
|---|---|---|
| **V1 编译** | `pio run`（增量必失败 ⇒ 用 `PLATFORMIO_BUILD_DIR=.pio/build/p5a …`） | SUCCESS + RAM/Flash 增量 |
| **V2 启动锚点** | 上电 → `logt ring` | 出现 `0x0101`，且 `INIT_MS` 有值；`logt flash total` **不含** `0x0101`（INFO 不落盘） |
| **V3 复位证据（正常）** | 正常上电 → `logt flash` | **不出现** `0x0103`（POWERON 非异常） |
| **V4 复位证据（异常，静态兜底）** | 无法在板上制造 panic ⇒ **objdump 静态验证**判据函数的分支编码 + `LOG_P_RESET_REASON` 参数编码 | 与 `LOG_SYS_RESET_ABNORMAL` 分支对应；**诚实标注"运行时未触发"** |
| **V5 重启三态** | `cm {"cmd":"system.restart"}` → 10 s 窗口内 `logt ring` | `0x0109`（带 critical 计数）→ `0x010A`；重启后 `logt flash` **查不到 `0x010A`**（INFO）⇒ 正好验证 §1.3 的丢失风险（**该风险由 E2 裁决**） |
| **V6 `BOOT_INCOMPLETE_PREV`** | ① 上电后 3 s 内**硬断电** → 重新上电 → `logt flash` | 出现 `0x0102`（CRITICAL ⇒ 落 Flash ⇒ 跨重启可查）✅ 可实测 |
| **V7 `CRITICAL_OP_UNDERFLOW`** | 无控制台入口 ⇒ **objdump 静态验证** + 人工审阅配对错误路径 | 如实标注"未触发" |
| **V8 BLE `DECODE_FAIL` 聚合** | `logt fill warn 10 902`（验证 LogManager 侧行为不变）+ **objdump 静态验证** `else` 分支累加与窗口常量 | 记录条数 1/窗口；ΣCOUNT 因 `logt fver` 不打印参数值 ⇒ 走 `.pio/p15run/log_decode.py` 路径（`LV-2`） |
| **V9 BLE 正常链** | 与温湿度计同场 → `logt ring` | `0x0901` 出现；`0x0902` 仅在失败时出现；**callback 内零日志**（objdump 确认 `onResult` 无 `log_emit` 调用） |

**验证纪律**：沿用既有惯例 —— 不可达路径**诚实标注"未触发"**，以 **objdump 静态证据**兜底，**不降断言、不改判据**。查 Flash 前先 `logt fwipe` + `mwipe` + `creset`（铁律 12）。

---

## 6. 提交计划（小步、不 squash）

| # | commit message | 内容 |
|---|---|---|
| 1 | `docs(plan): phase 5-a/b/c execution plan` | 本文件 + `未修复的问题.md` 登记 `LV-3` / `PROTO-2`（纯事实缺口，与决策无关） |
| 2 | `feat(log): log reset reason and boot completeness evidence` | 5-A-1（`system_command.cpp` + `config_manager.cpp`） |
| 3 | `docs(progress): record system reset logging completion` | Progress / Matrix / 未修复问题 |
| 4 | `feat(log): log restart lifecycle and boot completion` | 5-A-2 + 5-A-3（`system_command.cpp` + `main.cpp`） |
| 5 | `docs(progress): record system boot logging completion` | 同步文档 |
| 6 | `fix(event): correct system event count, valve names and string table alignment` | **5-B**（`EVT-1` + `P0-1` 同批，**待你确认 E6 后**） |
| 7 | `docs(progress): record evt-1 fix` | 同步文档 + `EVT-1` 关闭 |
| 8 | `feat(log): integrate ble thermometer logging` | **5-C** |
| 9 | `docs(progress): record ble logging completion` | 同步文档 |

**纪律**：每次 commit 前 `git diff --stat src/`（或 `test/`）自查；**5-B 与日志改动严格分离**（用户明令）；不 `git add -A`；每个文件独立 `git add`。

---

## 7. 需要确认的决策点

| # | 决策点 | 建议 |
|---|---|---|
| **E1** | `LOG_SYS_RESTART_CANCELLED`：① 不接入 + 登记为"架构上不可取消的零宿主 ID"；② 挂到 `request_restart()` 的"被拒绝"分支（**语义伪造，不推荐**） | **①** |
| **E2** | `LOG_SYS_RESTART_EXECUTED` 的送达：① 接受"尽力而为"（可能丢）+ 登记 `PROTO-2`；② 本阶段**暂缓接入**该 ID（其余 6 个先做）；③ 新增"重启前强制上行"能力（**超出"只增加观测能力"范围**） | **①**（保守：接入 + 诚实登记；`logt ring` 可覆盖上板验证） |
| **E3** | `BOOT_INCOMPLETE_PREV` 的"首次启动"歧义：① 接受首次烧录后首启产生 1 条（带 `RESET_REASON` 供判读）；② 引入 `boot_seq` getter 消歧（+1 内部 API） | **①**（零新增；首启 1 条是一次性、可接受） |
| **E4** | `BOOT_INCOMPLETE_PREV` 宿主归属：① `config_manager.cpp`（**定义点**，推荐）；② `system_command.cpp`（段归属，需胶水） | **①** |
| **E5** | 5-A 是否按 5-A-1 / 5-A-2 / 5-A-3 三组拆分提交 | **是** |
| **E6** | 5-B 方案（A：扩 `SYSTEM_EVENT_COUNT` 到 17 + 补齐字符串表 + **删幻影条目修 `P0-1`** / B：只补字符串表 / C：新增独立宏） | **A**（并接受"阀门 Trigger 由静默无效变为生效"；`P0-1` 修复经核验**零现存使用者**、零回归风险） |
| **E7** | `DECODE_FAIL` 聚合窗口：**60 s**（1 条/min，环撑 8.3 h）或 **300 s**（1 条/5 min，环撑 41 h） | **60 s**（与其他聚合器一致） |
| **E8** | 是否登记 `LV-3`（BLE 队列 `xQueueSend` 静默丢包）与 `PROTO-2`（`(IMM)` 无实现） | **是**（纯登记，不改代码） |

---

## 8. 边界声明（本阶段**不做**）

- ❌ 不改任何已有 EventId 数值、ParamId、配置项、enable 开关、System State 结构、EventManager 策略/优先级/风暴参数/队列大小/dispatch 流程
- ❌ 不接入 `HEAP_LOW` / `PSRAM_ALLOC_FAILED` / `FS_MOUNT_FAILED`（用户明示）
- ❌ 不接入 `LOG_SYS_RESET_NORMAL`（不在用户清单内；`BOOT_COMPLETE` 已提供"启动发生"的锚点）—— 如认为需要请告知
- ❌ 不接入 OLED（维持"不埋点"，仅登记 `OLED-1`）
- ❌ 不新增 `0x0Fxx` Motor/Dispense 日志（未来域，等业务模块本体）
- ❌ 不修 `R-7` / `R-8`（维持 `DEFERRED`，本阶段不重新升级普通采样间隔问题）
- ❌ 不引入任何"重启前强制上行"的新能力（见 E2）

---

## 附：本计划引用的关键代码位置

| 事实 | 位置 |
|---|---|
| 复位原因采样 | `system_command.cpp:222`（`esp_reset_reason()` 缓存于 `:48`） |
| 复位原因→名称映射 | `system_command.cpp:95` |
| 唯一 `ESP.restart()` | `system_command.cpp:312`（`RESTARTING` 置位 `:306`，`Serial.flush()` `:310`） |
| Restart 首次请求分支 | `system_command.cpp:333-338` / `:356-363` |
| Critical Op 下溢分支 | `system_command.cpp:452-477`（`if(!ok)` 在 `:464`） |
| boot flag 读/删/写 | `config_manager.cpp:1108` / `:1116` / `:1377-1395`；`config_manager.h:81` |
| setup 调用序 | `main.cpp:444`（`log_init`）/ `:483`（`config_init`）/ `:518`+`command_manager.cpp:534`（`system_command_init`）/ `:553`（`config_boot_validate`） |
| `SYSTEM_EVENT_COUNT` | `event_manager.h:53`（`= EVENT_ERROR + 1 = 14`）；使用点仅 `event_manager.cpp:161` / `:176` |
| `event_names[]` 缺阀门 | `event_manager.cpp:180-199`（14 项） |
| 阀门事件发布方 | `valve.cpp:158` / `:171` / `:374` |
| Trigger 字符串解析 | `workflow.cpp:4658` / `:4687` |
| flush 仅 CRITICAL 置位 | `log_manager.cpp:1962-1975`；API `:3290-3303`（生产零消费方） |
| 发送闸门 / 节流 | `log_manager.cpp:2883-2886` / `:2900-2905`；上行 `:2763` → `cloud_manager.cpp:1795` |
| BLE 回调（禁埋点） | `MiThermometer.cpp:114-167`（`xQueueSend` `:165` 返回值忽略） |
| BLE 解码点（无 `else`） | `MiThermometer.cpp:462-501`（`if(result)` `:468`） |
| BLE 失败计数 / LEVEL 迁移 | `MiThermometer.cpp:363` / `:367-382` |
