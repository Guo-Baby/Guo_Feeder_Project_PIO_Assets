# Event 模块日志接入前置设计审查（Phase 3 第 ⑤ 项 / P2-M）

> **性质：只分析、不修改代码。** 本轮未改动 `event_manager.cpp` / `event_manager.h` /
> `log_events.h` / EventId / ParamId / System State / 测试代码中的任何一个。
> 审查日期：2026-09-20 ｜ 代码基线：`4b2e04b`（P2-L Registry 接入后）

---

## 0. 结论速览（TL;DR）

| 问题 | 结论 |
|---|---|
| **EventManager 是否应该成为日志宿主？** | **是，但仅限"自身运行异常"**（队列满 / 风暴丢弃 / 其它内部异常）。**绝不**作为"业务事件转发器"逐条记录 `EVENT_*`。 |
| 业务事件（`EVENT_WIFI_CONNECTED` 等）该谁记？ | **发布方模块**（wifi / time / valve / weight / cloud）。实测 **17 个 `event_push` 中 13 个的发布方已埋 `log_emit`** ⇒ EventManager 再记即为**重复**。 |
| 为什么丢包必须由 EventManager 自己记？ | **★ 决定性证据**：17 个调用点**全部忽略 `event_push()` 返回值**，且 `event_get_drop_count()` / `duplicate_count()` / `queue_count()` 三个 getter **全仓库零消费方** ⇒ **事件丢弃当前 100% 不可观测**，发布方物理上看不到，只能由 EventManager 内部记录。 |
| `0x0C01` / `0x0C02` 是否可逐条记？ | **均不可**。定义上就是"异常流量"事件：风暴丢弃**天然高频**（超过 5 次/500ms 才触发）⇒ **必须计数聚合 + 周期窗口上报**。 |
| 是否需要新增接口？ | **否**。`event_dispatch()` 每 loop 无条件调用 ⇒ 天然周期节拍器；ParamId 全部复用既有（`LOG_P_COUNT` / `LOG_P_FAIL_COUNT` / `LOG_P_QUEUE_SIZE` / `LOG_P_CAUSE`）。 |
| 是否新增 EventId / ParamId / System State / 配置项 / 开关？ | **否，全部为零**。 |

> **★ 与用户判断完全一致**：事件由功能模块发布/监听，业务事实已在模块侧布点；
> EventManager 只需记录**自身运行中的异常**（队列满、风暴触发、其它异常）。

---

## 1. EventManager 当前职责分析

### 1.1 `event_push()` 调用位置（全仓库 17 处 / 5 个模块）

| # | 位置 | 事件 | 策略 | 同函数是否已有 `log_emit` | 判定 |
|---|---|---|---|---|---|
| 1 | `cloud_manager.cpp:1318` | `EVENT_CLOUD_DISCONNECTED` | STATE | ✅ 有 `LOG_MQTT_DISCONNECTED`（P2-D） | **重复** |
| 2 | `cloud_manager.cpp:1564` | `EVENT_CLOUD_CONNECTED` | DEDUP | ✅ 紧邻上方 `:1556` `LOG_MQTT_CONNECTED` | **重复** |
| 3 | `cloud_manager.cpp:1580` | `EVENT_CLOUD_DISCONNECTED` | STATE | ✅ 同模块已有 | **重复** |
| 4 | `cloud_manager.cpp:1646` | `EVENT_CLOUD_DISCONNECTED` | STATE | ✅ 同模块已有 | **重复** |
| 5 | `time_manager.cpp:546` | `EVENT_TIME_VALID` | NORMAL | ✅ 紧邻 `LOG_TIME_VALID_ENTER`（P2-E） | **重复** |
| 6 | `time_manager.cpp:567` | `EVENT_TIME_INVALID` | NORMAL | ✅ 紧邻 `LOG_TIME_INVALID_ENTER` | **重复** |
| 7 | `time_manager.cpp:857` | `EVENT_NTP_SYNC_OK` | NORMAL | ✅ 紧邻 `LOG_TIME_NTP_OK` | **重复** |
| 8 | `valve.cpp:158` | `EVENT_VALVE_OPEN` | STATE | ✅ 紧邻 `LOG_VALVE_OPEN`（P2-H） | **重复** |
| 9 | `valve.cpp:171` | `EVENT_VALVE_CLOSE` | STATE | ✅ 紧邻 `LOG_VALVE_CLOSE` | **重复** |
| 10 | `valve.cpp:374` | `EVENT_VALVE_ERROR`（安全超时） | STATE | ✅ `LOG_VALVE_SAFETY_TIMEOUT`（一次性锁） | **重复** |
| 11 | `valve.cpp:427` | `EVENT_VALVE_CLOSE`（force close） | STATE | ✅ 同函数 `:470` `LOG_VALVE_FORCE_CLOSE` | **重复** |
| 12 | `weight.cpp:268` | `EVENT_WEIGHT_ERROR`（jump） | STATE | ⚠️ 无（P2-G 决策：jump 不转 `ERROR_ENTER`） | **唯一源** |
| 13 | `weight.cpp:384` | `EVENT_WEIGHT_ERROR`（HX711 not ready） | STATE | ✅ **紧随其后** `:397` `weight_refresh_error_state()` → `LOG_WEIGHT_ERROR_ENTER` 边沿 | **重复** |
| 14 | `weight.cpp:557` | `EVENT_WEIGHT_ERROR`（trigger aborted） | STATE | ✅ 由**进入 `STATE_WEIGHT_ERROR` 时**已发的 `ERROR_ENTER` 覆盖（本分支只在"已处于错误态"时触发，属**结果**而非新事实） | **重复** |
| 15 | `weight.cpp:578` | `EVENT_WEIGHT_ERROR`（invalid gram） | STATE | ❌ 仅 `Serial.println` | **唯一源** |
| 16 | `wifi_module.cpp:417` | `EVENT_WIFI_CONNECTED` | NORMAL | ✅ `LOG_WIFI_CONNECTED`（P2-C） | **重复** |
| 17 | `wifi_module.cpp:577` | `EVENT_WIFI_DISCONNECTED` | NORMAL | ✅ `LOG_WIFI_DISCONNECTED` | **重复** |

**统计**：17 处中 **13 处（76%）的发布方已埋** `log_emit`；仅 2 处（#12 `#15`）是唯一事实源，
且这 2 处都**不应由 EventManager 代记**（见 §3.3）。

**策略分布**：`STATE` 11 / `NORMAL` 5 / `DEDUP` 1 / **`FORCE` 0**。
⇒ `EVENT_POLICY_FORCE` 分支（`:210` / `:250-262`）**当前无调用方使用**，属死路径。

### 1.2 `event_dispatch()` 流程

```
main.cpp:576  event_dispatch();          // 每 loop 无条件调用一次
    ↓
while (queue_count > 0 && processed < 4)      // 单次最多 4 条，防阻塞主循环
    ↓
index = find_highest_priority()               // 按 priority 取最高
    ↓
is_expired = (expire>0 && millis()-timestamp > expire)
    ↓
dispatch_msg = msg;  remove_event(index)      // ★ 先出队，后回调
    ↓
if (is_expired) { drop_count++; continue; }   // 过期丢弃
    ↓
for (i < sub_count[eid]) cb(dispatch_msg)     // 通知订阅者
```

**要点**：
- **无独立 Task**：`event_dispatch()` 挂在 `loop()`（`main.cpp:576`），与 `log_task()` 同处 loop 上下文 ⇒ **埋点无跨任务风险**。
- **每 loop 必然执行一次** ⇒ 天然具备"周期节拍器"能力（见 §4 方案 A）。
- **先出队再回调**（`:340-342`）：回调内再次 `event_push` 同事件**不会**被 DEDUP 拦 ⇒ 无自锁风险。
- 单次上限 4 条 ⇒ 队列持续积压时 `dispatch` 追不上 `push`，是队列满的前置条件。

### 1.3 Subscriber 数量

| 注册点 | 事件 | 回调 | 类型 |
|---|---|---|---|
| `cloud_manager.cpp:1511` | `EVENT_WIFI_CONNECTED` | `cloud_event_callback` | 静态 |
| `cloud_manager.cpp:1512` | `EVENT_WIFI_DISCONNECTED` | `cloud_event_callback` | 静态 |
| `dispense_guard.cpp:58` | `EVENT_WEIGHT_ERROR` | `dispense_guard_event_callback` | 静态 |
| `oled.cpp:272` | `EVENT_CONFIG_CHANGED` | `oled_event_handler` | 静态 |
| `oled.cpp:278` | `EVENT_NTP_SYNC_OK` | `oled_event_handler` | 静态 |
| `oled.cpp:284` | `EVENT_WIFI_CONNECTED` | `oled_event_handler` | 静态 |
| `oled.cpp:290` | `EVENT_WIFI_DISCONNECTED` | `oled_event_handler` | 静态 |
| `workflow.cpp:4661` | 由 `event_from_string(trigger->id)` 决定 | `workflow_event_callback` | **动态**（遍历所有 workflow 的 step0 trigger） |

- **去重后 4 个回调函数**：`cloud_event_callback` / `dispense_guard_event_callback` / `oled_event_handler` / `workflow_event_callback`。
- 上限：`MAX_EVENT_SUBSCRIBER 8`（每事件类型）/ `MAX_EVENT_TYPE 32`。当前每事件最多 2 个订阅者，远未饱和。
- `event_subscribe()` 满载时**静默 `return false`**（`:308-311`），仅 `dispense_guard_init()` 检查了返回值并打串口。

### 1.4 是否存在"消费者已记录同一事实"

**存在，且是普遍现象。** 以安全链为例（详见 §3）：`EVENT_WEIGHT_ERROR` 的**发布方**（`weight.cpp`）
与**消费方动作**（`valve_force_close()`）**都已**有 `log_emit`，EventManager 若再记 ⇒ 一条物理事实三条记录。

### 1.5 ★ 本节结论：EventManager 是否应该成为日志宿主？

> **应该 —— 但宿主范围严格限定为「EventManager 自身运行异常」，且这是唯一可行的宿主。**

**理由（两条，缺一不可）**：

1. **业务事件：不需要，且会造成重复。** 76% 的 `event_push` 发布方已埋点（§1.1），
   EventManager 是**传输层**，逐条转发业务事件 = 把"模块已记录的事实"再记一遍（架构污染）。

2. **运行异常：必须，且只能由 EventManager 记。** 实证：
   - **17 个调用点 100% 忽略 `event_push()` 返回值**（全仓库无 `EventPushResult` 接收者）
   - **3 个侧信道 getter 零消费方**（`event_get_drop_count()` / `event_get_duplicate_count()` / `event_get_queue_count()` 在 `src/` 内除定义外无任何引用）

   ⇒ 队列满 / 风暴丢弃 / 过期丢弃 **当前完全不可观测**：发布方看不到（不查返回值），
   外部查不到（没人调 getter）。**只有 EventManager 内部能观测这些事实。**

这正好支持用户判断：**业务事件不记，只记自身异常。**

---

## 2. EventId `0x0C01` / `0x0C02` 语义分析

### 2.1 现有定义（`log_events.h:333-334`）

```c
LOG_EVT_QUEUE_FULL    = 0x0C01,   // WARN（策略 Flash + Cloud；走侧信道计数上报）
LOG_EVT_STORM_DROPPED = 0x0C02,   // WARN（策略 Flash + Cloud；走侧信道计数上报）
```

**Event 段只有这 2 个 EventId** —— 与"只记自身异常"的职责边界**完全吻合**，无需新增。

### 2.2 逐项分析

| EventId | 名称 | 定义（结构性） | 触发位置 | 频率 | 是否适合逐条日志 |
|---|---|---|---|---|---|
| **0x0C01** | `LOG_EVT_QUEUE_FULL` | 队列已达 `EVENT_QUEUE_SIZE=32` 且新事件无法入队 | `event_push()` **`:266`**（`queue_count>=32` 且 `policy != FORCE`） | **低基线 / 高危峰**：稳态 ≈0；一旦 dispatch 跟不上 push（`dispatch` 每 loop 仅 4 条），**每次 push 都会触发** ⇒ 可持续高频 | ❌ **不适合**。需 ≥60s 窗口聚合（与 P2-D 同构）。瞬时多发无意义：队列满本身是**状态**，不是事件（铁律 5） |
| **0x0C02** | `LOG_EVT_STORM_DROPPED` | 单事件类型在 500ms 内被 push 超过 5 次，超出部分被抑 | `event_push()` **`:220-224`**（`storm_counter[eid] > EVENT_STORM_MAX_PER_EVENT`） | **★ 天然高频**：定义上就是"超过 5 次/500ms"的**持续流**。真风暴下可达**数十~数百次/秒** | ❌❌ **绝对不可逐条**。一条消息一条 log 会**瞬间填满 64 槽 RAM 环**并打爆 Flash（WARN 落 Flash）。**必须计数聚合** |

### 2.3 ★ 关键陷阱：返回值不可作为判据

`EVENT_DROPPED` 这个**返回值**在三个不同分支被返回：

| 分支 | 行 | 语义 |
|---|---|---|
| eid 越界 | `:206` | 参数非法（**且未计入 `drop_count`**） |
| 风暴抑制 | `:223` | **= `0x0C02`** |
| 队列满 + FORCE 但优先级不够 | `:260` | 队列满的 FORCE 变体 |

⇒ **不能**靠"返回值 == `EVENT_DROPPED`"来埋 `0x0C02`，会误捕 `:206` / `:260`。
**必须在分支内部埋点**（`:220-224` 块内），而非在 `event_push()` 出口统一判断。

同理，`0x0C01` 必须埋在 `:266`（`return EVENT_QUEUE_FULL;` 处），
**不能**用"返回值 == `EVENT_QUEUE_FULL`"在函数出口判断 —— 虽然当前等价，但耦合了返回值语义，脆弱。

### 2.4 不适合逐条 ⇒ 替代方式

**采用 P2-D 已验证的「计数聚合 + 周期窗口」模板**（`cloud_manager.cpp:100-156`，可逐行照搬）：

```
累加器（只 ++，不发射）  →  周期窗口到  →  发射 1 条，携带 LOG_P_FAIL_COUNT/COUNT  = 窗口内被抑制的次数
                                      →  窗口内为 0 ⇒ 完全静默
```

- **信息零丢失**：`ΣCOUNT = 真实发生次数`（P2-I 已沉淀，铁律 6 / 15）。
- **条目数恒定**：无论窗口内发生 1 次还是 10000 次，都只 1 条。
- 窗口建议 **60000 ms**（与 `CLOUD_PUBLISH_FAIL_REPORT_MS` 对齐，保持全局一致）。

---

## 3. 重点检查：重复记录风险

### 3.1 示例链路（用户点名）

```
weight.cpp（HX711 异常）
   ├─ log_emit(LOG_WEIGHT_ERROR_ENTER)          ← P2-G 已埋（ERROR 边沿锁）
   └─ event_push(EVENT_WEIGHT_ERROR)             :384
        ↓
   EventManager（入队 / 风暴限流 / dispatch）
        ↓  [若在此逐条 log_emit → +1 条]  ❌
   dispense_guard_event_callback()               dispense_guard.cpp:23
        ↓  [若在此 log_emit → +1 条]  ❌（当前仅 Serial.println，未埋）
   valve_force_close()                           valve.cpp:398
        ├─ log_emit(LOG_VALVE_FORCE_CLOSE, CRITICAL)   ← P2-H 已埋 :470
        └─ event_push(EVENT_VALVE_CLOSE, "force close") :427
             ↓  [若 EventManager 再记 → 又 +1 条]  ❌
        └─ log_emit(LOG_VALVE_FORCE_CLOSE_FAILED, CRITICAL)（失败分支 :405）
```

### 3.2 一条物理事实会产生几条日志？

| 方案 | 记录数 | 说明 |
|---|---|---|
| **现状**（不接 Event 埋点） | **2~3 条** | `WEIGHT_ERROR_ENTER`(WARN) + `VALVE_FORCE_CLOSE`(CRITICAL)［+ 失败时 `FORCE_CLOSE_FAILED`］—— 已是完整、无冗余的事实链 |
| **若 EventManager 逐条记业务事件** | **4~6 条** | 上述 + `EVENT_WEIGHT_ERROR` 入队 + dispatch + `EVENT_VALVE_CLOSE` 入队 ⇒ **同一物理事实重复 2~3 倍** |
| **推荐方案**（只记自身异常 + 聚合） | **2~3 条**（稳态）+ **偶尔 1 条**聚合异常 | 异常仅在真正发生时追加，且被聚合成 1 条 |

### 3.3 唯一事实宿主判定表

| 事实 | 唯一宿主 | 依据 |
|---|---|---|
| "重量进入异常态" | **`weight.cpp`**（P2-G `LOG_WEIGHT_ERROR_ENTER`） | 挂在 `error_state` 边沿上，是**状态跃变的定义点** |
| "重量跳变 jump" | **`weight.cpp`** 的 `event_push` 侧（`:268`）——**若未来要记，应埋在 weight.cpp，不是 EventManager** | P2-G 已决策 jump 不转 `ERROR_ENTER`（它是**事件**非**状态**）；当前无日志，属**已知缺口**而非 EventManager 责任 |
| "gram 参数非法回退 20" | **`weight.cpp:578`** | 同上，唯一源在发布方 |
| "阀门被强制关闭" | **`valve.cpp:470`**（`LOG_VALVE_FORCE_CLOSE`） | 安全动作的定义点，P2-H 已埋 |
| "强制关闭失败" | **`valve.cpp:405`**（CRITICAL） | 唯一失败分支 |
| "事件入队 / 出队 / 派发" | **⛔ 无宿主（不应记录）** | 传输层内部行为，业务事实已由发布方记录 |
| **"事件队列满"** | **★ `event_manager.cpp:266`** | **只有它能观测**（返回值被忽略 + getter 零消费） |
| **"事件风暴触发"** | **★ `event_manager.cpp:220-224`** | 同上 |
| "事件过期被丢弃"（`:347`） | ⚠️ **无 EventId**（缺口） | 建议**不新增** ID，可并入 `0x0C02` 的 `CAUSE` 语义或维持不记录 |

---

## 4. 设计方案

### 方案 A：EventManager 周期统计（计数聚合 + 周期上报）★ 推荐

在 `event_manager.cpp` 内新增**两个累加器** + **一个周期上报函数**，
以 **`event_dispatch()` 作为节拍器**（它每 loop 必然执行一次，无需新增任务、无需改 `main.cpp`）：

```
event_push():220  风暴丢弃分支  → evt_note_storm_drop()   // 只 ++
event_push():266  队列满分支    → evt_note_queue_full()   // 只 ++
event_dispatch()  开头          → evt_report_periodic()   // 窗口到 & 计数>0 ⇒ 各发 1 条
```

| 项 | 内容 |
|---|---|
| 上报内容 | `LOG_EVT_QUEUE_FULL`：参数 `LOG_P_COUNT`(被抑制次数) + `LOG_P_QUEUE_SIZE`(32)；`LOG_EVT_STORM_DROPPED`：参数 `LOG_P_COUNT` + `LOG_P_CAUSE`(1=风暴) |
| 窗口 | 60000 ms（对齐 `CLOUD_PUBLISH_FAIL_REPORT_MS`） |
| 静默性 | 窗口内计数为 0 ⇒ **完全静默**（零噪声） |
| 新增状态 | 4 个 `static` 局部累加器（模块内 `static`，非全局裸变量；**不进 System State**） |
| 新增接口 | **无**（`event_dispatch()` 已存在且已被调用） |
| 是否改风暴策略 | **否**（只观测，不干预 `EVENT_STORM_MAX_PER_EVENT` / 窗口） |

### 方案 B：仅记录关键事件（CRITICAL / FORCE 类）

按"事件重要性"筛选，只对 `EVENT_PRIORITY_CRITICAL` 与 `EVENT_POLICY_FORCE` 的事件逐条记。

| 项 | 内容 |
|---|---|
| 判定 | `msg.priority == EVENT_PRIORITY_CRITICAL` 或 `policy == EVENT_POLICY_FORCE` |
| 命中范围 | 实测：`EVENT_PRIORITY_CRITICAL` 用于 `weight.cpp:268/557`（WEIGHT_ERROR）、`valve.cpp:427`（VALVE_CLOSE force）；`FORCE` 策略 **0 处使用** |
| 问题 | ① 这三个事件**发布方均已埋点**（见 §1.1 #11/#12/#14）⇒ **仍然重复**；② CRITICAL 事件在真风暴下同样高频 ⇒ **仍会打爆环** |

### 4.3 两方案比较

| 维度 | 方案 A（周期统计）★ | 方案 B（仅关键事件） |
|---|---|---|
| **Flash 压力** | ✅ **恒定**：窗口内无论发生 1 次还是 10000 次都只 1 条（WARN 落 Flash）。最坏 2 条/60s | ❌ **风暴下失控**：CRITICAL 事件在 HX711 掉线时可 0.2 次/秒持续，WARN/CRITICAL 落 Flash ⇒ 直接冲击 496 条环与 Flash 寿命 |
| **信息完整性** | ✅ **零丢失**：`ΣCOUNT = 真实次数`（P2-I 铁律 6/15）。能回答"发生了多少次" | ⚠️ **有损**：只知"发生了"，不知"多少次"；且**遗漏非 CRITICAL 的丢包**（队列满多为 NORMAL/STATE 事件） |
| **与现有 LogManager 架构兼容性** | ✅ **完全一致**：逐行复用 P2-D 已上板验证的聚合模板（`cloud_manager.cpp:104-156`）；符合矩阵 §12 契约"走侧信道计数上报、周期聚合 1 条"；符合铁律 6（高频路径 N 次记 1 且 ≥60s） | ❌ **违背契约**：矩阵 §12 明确"**不要为每条丢弃事件发日志**"；方案 B 本质是"按优先级筛选后逐条发"，与契约冲突 |
| **重复记录风险** | ✅ **零**：只记 EventManager 自身异常，与发布方埋点**完全不重叠** | ❌ **高**：CRITICAL 事件发布方已埋（§1.1）⇒ 同一事实 2 条 |
| **改动面** | 小：`event_manager.cpp` 内 ~40 行（含注释），无接口变更 | 小，但语义错误 |
| **风险** | 低 | **中高**（Flash 压力 + 架构污染） |

> **结论：采用方案 A。方案 B 不建议采用。**

---

## 5. 最终建议

### 5.1 哪些需要 `log_emit`（共 2 个 EventId / 2 处埋点）

| EventId | Level | 埋点位置 | 方式 | 参数（全部复用既有） |
|---|---|---|---|---|
| `LOG_EVT_QUEUE_FULL` (0x0C01) | WARN | `event_push()` **`:266`** 分支内 | **计数聚合**（不直接 emit） | `LOG_P_COUNT`(0x45) + `LOG_P_QUEUE_SIZE`(0x53) |
| `LOG_EVT_STORM_DROPPED` (0x0C02) | WARN | `event_push()` **`:220-224`** 分支内 | **计数聚合**（不直接 emit） | `LOG_P_COUNT`(0x45) + `LOG_P_CAUSE`(0x16) |

- 两处均**只累加**，实际 `log_emit` 由 `event_dispatch()` 内的周期上报器发出（**共 2 个 `log_emit` 调用点**）。
- 参数全部复用既有 ParamId，**不新增任何 ParamId**。

### 5.2 哪些 Event **永远不应该** `log_emit`

| 类别 | 事件 | 理由 |
|---|---|---|
| **全部业务事件**（逐条） | `EVENT_WIFI_CONNECTED/DISCONNECTED`、`EVENT_NTP_SYNC_OK`、`EVENT_TIME_VALID/INVALID`、`EVENT_CLOUD_CONNECTED/DISCONNECTED`、`EVENT_CONFIG_CHANGED`、`EVENT_CLOUD_UPLOAD`、`EVENT_COMMAND_RESULT`、`EVENT_WEIGHT_READY`、`EVENT_WEIGHT_ERROR`、`EVENT_VALVE_OPEN/CLOSE/ERROR` | 发布方 76% 已埋点 ⇒ 重复记录。EventManager 是**传输层**，不是事实源 |
| **传输层内部行为** | 入队成功、出队、dispatch 派发、订阅注册成功/失败 | 无业务语义；订阅失败仅影响可观测性，且 `dispense_guard_init()` 已打串口 |
| **`EVENT_DUPLICATE`（DEDUP 命中 `:243`）** | — | **无对应 EventId**，且是**正常设计行为**（DEDUP 策略的目的就是去重），不是异常 ⇒ **不新增 ID、不记录** |
| **事件过期丢弃（`:347`）** | — | 同上，**无 EventId**；建议**不新增**，维持不记录（或后续评估并入 `0x0C02` 的 `CAUSE`） |
| **eid 越界（`:206`）** | — | 编程错误，当前 `FORCE`/动态字符串路径下不可达；**不新增 ID** |

### 5.3 是否需要新增接口

| 项 | 结论 |
|---|---|
| 是否只增加 `log_emit` 调用即可？ | ✅ **是**（+ `#include "log_manager.h"`）。2 处累加 + 1 个周期上报器 |
| 是否需要新增 callback / 转发到 EventManager？ | ❌ **否**。EventManager 是同步模块，直接埋点，保持单向依赖 `event_manager → log_manager` |
| 是否需要新增任务 / 改 `main.cpp`？ | ❌ **否**。`event_dispatch()` 已每 loop 调用，直接在其内部做周期判定 |
| 是否需要新增 EventId / ParamId / System State / 配置项 / enable 开关？ | ❌ **全部否** |
| 是否修改 EventManager 风暴策略？ | ❌ **否**。只观测，不干预 `EVENT_STORM_MAX_PER_EVENT=5` / `500ms` / 队列策略 |
| 是否新增测试模式 / 过滤开关？ | ❌ **否** |

### 5.4 实施约束（沿用既有铁律）

- **累加器用模块内 `static`**：`event_push()` / `event_dispatch()` 均在 loop 上下文（无 MQTT 任务并发）⇒ 与 Registry（P2-L）不同，**此处 `static` 是安全的**（Registry 需栈上局部是因其可能被 esp-mqtt 任务调用）。
- **不得提前 return**：周期上报器应放在 `event_dispatch()` 的**最开头**，在 `while` 循环之前 —— 否则队列非空时会因 `processed` 计数而延迟上报（铁律 13）。
- **`log_emit()` 永不持锁**：累加器读写均在同步路径，无需 mutex。

---

## 6. 本轮附带的发现（**只报告，不修复**）

| 编号 | 发现 | 证据 | 影响 | 建议 |
|---|---|---|---|---|
| **EVT-1** | **`SYSTEM_EVENT_COUNT` 越界**：`#define SYSTEM_EVENT_COUNT (EVENT_ERROR + 1)` = **14**，但 `EVENT_VALVE_OPEN=14` / `CLOSE=15` / `ERROR=16` **超出上界** | 实测枚举展开（见 §7 证据）。`event_from_string()` 循环 `i < SYSTEM_EVENT_COUNT` ⇒ 三个阀门事件**永远解析不到**；`event_to_string()` 对 `idx>=14` 返回 `"EVENT_UNKNOWN"` | **功能性**：`workflow.cpp:4659` 用 `event_from_string(trigger->id)` 注册事件触发器 ⇒ **Workflow 无法订阅任何阀门事件**（静默失败，无报错） | 登记为独立缺陷，不在本次埋点范围内修复 |
| **EVT-2** | **`drop_count` 语义混淆**：单一计数器被 4 种不同原因共用（`:222` 风暴 / `:259` 队列满+FORCE 优先级不足 / `:265` 队列满 / `:346` 派发时过期） | `event_manager.cpp:222/259/265/346` 四处均执行 `drop_count++`，无原因区分 | `event_get_drop_count()` 无法区分原因。⇒ **因此本设计必须在分支内埋点，不能读 `drop_count`** | 设计已规避（§2.3）；是否拆分计数器另行评审 |
| **EVT-3** | `:206` 越界 `return EVENT_DROPPED` **未计入** `drop_count` | 对比 `:222/:259/:265/:346` 均有 `drop_count++`，唯 `:206` 无 | 统计遗漏（当前该路径不可达，影响为零） | 仅登记 |
| **EVT-4** | **侧信道 getter 零消费方**：`event_get_drop_count()` / `event_get_duplicate_count()` / `event_get_queue_count()` 在 `src/` 内除定义外**无任何引用** | 全仓库 grep 结果为空 | 矩阵 §12 所称"走侧信道计数上报"的**消费侧根本不存在** ⇒ 印证"丢包 100% 不可观测"，也说明**必须由 EventManager 内部主动上报** | 设计已采用内部主动上报（方案 A） |
| **EVT-5** | `MAX_EVENT_TYPE=32` 但实际事件枚举只到 16；`storm_window_tick[32]` / `storm_counter[32]` / `callback_table[32][8]` 按 32 分配，实际仅用前 17 | 静态容量冗余 | 无功能影响；`callback_table` = 32×8×4B = 1KB 静态占用（可接受） | 仅登记 |

---

## 7. 证据清单（可复现）

| 结论 | 命令 / 方法 |
|---|---|
| 17 个 `event_push` 调用点 | `grep -rn "event_push(" src/*.cpp`（排除 `event_manager.cpp`） |
| 返回值 100% 被忽略 | `grep -rnE "=\s*event_push\(\|EventPushResult" src/` ⇒ 除定义外**无匹配** |
| 侧信道 getter 零消费方 | `grep -rn "event_get_drop_count\|event_get_duplicate_count\|event_get_queue_count" src/` ⇒ 除 `event_manager.*` 外**无匹配** |
| 发布方埋点覆盖 13/17 | 逐一 `sed` 查看 17 处上下文，比对同函数内 `log_emit` |
| 策略分布 STATE 11 / NORMAL 5 / DEDUP 1 / **FORCE 0** | `grep -A6 "event_push(" src/*.cpp \| grep -o "EVENT_POLICY_[A-Z]*" \| sort \| uniq -c` |
| `SYSTEM_EVENT_COUNT=14`、阀门事件 14/15/16 越界 | 枚举展开实测：`EVENT_ERROR=13` ⇒ `COUNT=14`；`VALVE_OPEN=14 / CLOSE=15 / ERROR=16` 均 `>= 14` |
| `event_dispatch()` 每 loop 调用 | `main.cpp:576`，位于 `void loop()` 内，无条件调用 |
| 订阅方 4 个回调 | `grep -rn "event_subscribe(" src/` ⇒ cloud×2 / dispense_guard×1 / oled×4 / workflow 动态 |
| P2-D 聚合模板 | `cloud_manager.cpp:100-156`（`CLOUD_PUBLISH_FAIL_REPORT_MS` / `cloud_note_publish_fail()` / `cloud_report_publish_fail()`） |

---

## 8. 诚实标注：未覆盖 / 待定

1. **未实测事件风暴的真实发生频率**：稳态事件率极低（22s boot log 中业务事件 ≈0 条），
   风暴只在异常态（如 HX711 持续掉线、模块死循环 push）出现。本报告的"高频"结论来自
   **结构性论证**（风暴定义 = 超过 5 次/500ms），**非实测**。若需实测，需构造异常注入场景
   （且按约束**不得新增测试代码** ⇒ 建议用既有串口命令观察 HX711 掉线时的表现）。
2. **`0x0C01` 在 FORCE 策略下不触发**（`:250-262` 返回 `EVENT_DROPPED` 而非 `QUEUE_FULL`）。
   当前 `FORCE` 使用数为 0 ⇒ 无影响，但未来若启用 FORCE 需重新评估 `0x0C01` 语义完整性。
3. **过期丢弃（`:347`）与 DEDUP（`:243`）当前无 EventId**，按"不新增 ID"约束维持不记录。
4. **EVT-1（枚举越界）** 已证实影响 Workflow 事件触发器，但**不在本次埋点范围**，仅登记。

---

## 9. 下一步（**等待确认**）

待确认项 **E1–E4**：

| 编号 | 待确认 | 建议 |
|---|---|---|
| **E1** | 是否采用**方案 A**（周期统计聚合，2 个 EventId / 2 处 `log_emit` 调用点） | ✅ 建议采纳 |
| **E2** | 周期窗口取 **60000 ms**（对齐 P2-D `CLOUD_PUBLISH_FAIL_REPORT_MS`） | ✅ 建议采纳 |
| **E3** | 是否**不新增** EventId（过期丢弃 / DEDUP / eid 越界均维持不记录） | ✅ 建议不新增 |
| **E4** | `EVT-1`（`SYSTEM_EVENT_COUNT` 越界致阀门事件不可订阅）是否**另行开缺陷单** | ✅ 建议单独登记，不混入 P2-M |

**确认后实施范围**（预计）：
`src/event_manager.cpp` 唯一改动文件（+include + 2 处累加 + 1 个周期上报器，约 +40 行含注释）
→ V1 编译 → V2 上板零回归（稳态应完全静默）→ V3 构造风暴确认聚合计数正确
→ 更新 `docs/LogManager-P2-Progress.md` / `未修复的问题.md` → 独立 commit。

**未经确认不修改**：`event_manager.cpp` / `event_manager.h` / `log_events.h` / EventId / ParamId / System State / 测试代码。
