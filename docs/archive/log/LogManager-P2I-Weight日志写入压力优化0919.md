# P2-I　Weight Error 日志写入压力优化（LogManager 侧突发合并）

- **日期**：2026-09-19
- **范围**：Log Integration（方案 A），**不涉及任何安全行为变更**
- **生产代码**：`src/log_manager.h` `+25 / −0`、`src/log_manager.cpp` `+248 / −5`
- **零改动**：`src/weight.cpp` · `src/dispense_guard.cpp` · `src/event_manager.cpp` · `src/event_manager.h`
  · `src/log_events.h` · `src/valve.cpp` · `test/`
- **未新增**：EventId / ParamId / System State / 测试模式 / 开关

---

## 1. 问题定义

`EVENT_WEIGHT_ERROR` 同时服务两条**互相独立**的链：

| 链 | 路径 | 要求 |
|---|---|---|
| **安全链** | `weight_record_jump()` → `event_push()` → EventManager → `dispense_guard` → `valve_force_close()` | **实时**，绝不允许延迟 / debounce / 屏蔽 |
| **记录链** | `weight_refresh_error_state()` 状态边沿 → `log_emit(LOG_WEIGHT_ERROR_ENTER, WARN)` | 允许合并（历史信息完整即可） |

`WARN` ⇒ 落 Flash（Level Policy），而 Flash 段环只有 **496 条**（16 段 × 31）⇒
重复记录会挤掉更早的历史。**优化目标仅限记录链。**

---

## 2. 现状核实（改造前）

### 2.1 `EVENT_WEIGHT_ERROR` 完整调用链

**生产者（4 个 push 点，全部 `EVENT_POLICY_STATE`）**

| # | 位置 | 条件 | 优先级 | 当前是否活跃 |
|---|---|---|---|---|
| P1 | `weight.cpp:268`（`weight_record_jump()`） | 相邻滤波窗口 `|Δgram| ≥ WEIGHT_JUMP_DELTA_G(50)` | CRITICAL | ✅ **主因** |
| P2 | `weight.cpp:384` | HX711 连续 not-ready ≥ 5 s | HIGH | ✅ 次要 |
| P3 | `weight.cpp:522` | `weight_trigger_start()` 中途异常 | CRITICAL | ⛔ 触发路径未启用 |
| P4 | `weight.cpp:543` | 非法 gram 参数 | NORMAL | ⛔ 同上 |

**消费者**：`dispense_guard_event_callback()`（`dispense_guard.cpp:23`）→ `valve_force_close()`。
全仓**唯一**订阅者；实测 `force_close` 调用数 == `guard_rx` 在全部 9 次运行中**恒等**。

### 2.2 日志记录链与 Flash 写入点

```
weight_refresh_error_state()            ← err != error_state 的天然边沿（weight.cpp:211）
        ↓ log_emit(LOG_WEIGHT_ERROR_ENTER, WARN, {CAUSE, WEIGHT_G, RAW})
log_emit()  → 纯 RAM 入环（64 槽，memcpy；唯一的 Flash I/O 是每 256 条一次的 meta.bin seq 预留）
        ↓ log_task()（在 loop() 内，非阻塞、无 delay/while）
   ┌────┴─────┐
   ↓          ↓
Flash 段环   云队列（128 槽）→ MQTT log Topic
```

**结论：安全事件回调路径中不存在耗时 Flash 操作** ⇒ 「方案 B（异步写入保护）」**已经满足，无需改动**。

### 2.3 触发形态（决定方案的现实性）

- `LOG_WEIGHT_ERROR_ENTER` 是**状态边沿**记录，不是每条采样、也不是每个事件。
- `jump_error = jump_count >= WEIGHT_MAX_JUMP_EVENTS(5)`（窗口 `WEIGHT_ERROR_WINDOW_MS = 5000`）
  ⇒ 单靠跳变路径，**天然 ≥ 5 s 才能再触发一次**。
- 但 `err = no_data || raw_zero || jump_error` 是**三因或**（`weight.cpp:204-209`）
  ⇒ 任一因先消失、再成立即可产生**同毫秒** `-> 1` / `-> 0` 闪断（实测多次，也是 `R-6` 的来源）。
- ⇒ 短时间内重复 ENTER **是真实形态**，折叠有意义；但量级是 **0~10 条/会话**，不是高频源。

---

## 3. 方案

### 3.1 设计（在 LogManager 内，白名单制）

白名单当前只有 `LOG_WEIGHT_ERROR_ENTER`（`log_manager.cpp` 的 `s_coalesce_targets[]`）。

```
log_emit(白名单事件)
  ├─ 距上次"被接受" < LOG_COALESCE_WINDOW_MS(5000) ⇒ 折叠
  │     folded++ ；缓存本条参数；不占 seq / 不进环 / 不落 Flash / 不上云，return true
  └─ 否则 ⇒ 接受
        ① 若上一窗口 folded > 0 ⇒ 先产出汇总记录（LOG_P_COUNT = folded），保证时间序
        ② 本条正常落地，并追加 LOG_P_COUNT = 1

log_task() 阶段 0（每轮 loop，非阻塞、无 Flash I/O）
  └─ 窗口到期且 folded > 0 ⇒ 产出汇总记录（LOG_P_COUNT = folded）
```

**不变量：`Σ LOG_P_COUNT` = 真实发生次数**（记录数下降，但没有任何一次异常被丢掉）。

### 3.2 为什么放在 LogManager 而不是 WeightManager

| 设计要求 | 本方案 |
|---|---|
| 不改重量异常检测 | `weight.cpp` **零改动**（阈值 / 采样 / 状态机 / 事件产生条件全部原样） |
| 不改 DispenseGuard | `dispense_guard.cpp` **零改动** |
| 不在 EventManager 过滤 | `event_manager.*` **零改动**，风暴参数未动 |
| 只影响 LogManager 存储行为 | 合并逻辑**只在** `log_emit()` / `log_task()` 内，状态是 `log_manager.cpp` 文件级 static |
| Capability 层不直接操作 LittleFS | 未新增任何 Flash API；汇总记录走原路径 |
| 不影响 System State | 合并状态**不进** System State |

### 3.3 参数与 ID

复用既有 `LOG_P_COUNT`(0x45) + `LOG_WEIGHT_ERROR_ENTER`(0x050C)。被接受 / 汇总的记录形状**确定**：
`[原参数…] + LOG_P_COUNT`（本事件为 4 个参数）。**未新增、未重编号任何 ID。**

### 3.4 方案 C 审查结论（EventManager 风暴保护）

`EVENT_WEIGHT_ERROR` 的 4 个 push 点都是 `EVENT_POLICY_STATE`（≠ `EVENT_POLICY_FORCE`）
⇒ **确实受** `EVENT_STORM_MAX_PER_EVENT = 5 / 500 ms` 限制，第 6 条起 `EVENT_DROPPED`。

**判定：不构成安全缺陷** —— `valve_force_close()` 是**无条件的幂等强制同步**，
同一窗口内的重复事件执行**同一个动作** ⇒ 丢重复副本不会减少保护。
⇒ **不修改任何风暴参数**；仅在 Log 层优化。（`EVENT_POLICY_STATE` 还会清掉队列里同事件旧消息，同理。）

---

## 4. 上板验证（COM8，固件 `.pio/build/p2i`；Flash 65.3% / RAM 39.9%）

### 4.1 ★ 关键手段：`logt fill <level> <n> <EventId(hex)>`

`src/main.cpp:1292`：`long ev = strtol(logt_arg(op, 3), nullptr, 16)`，缺省回落 `LOG_WF_START`
⇒ **既有控制台钩子即可定向注入任意 EventId**，无需新增任何测试代码。

### 4.2 确定性验证结果（MQTT 记录级；`qdrop=0` / `evict_inf=0` ⇒ 捕获完整）

| 注入 | 期望 | 实测 |
|---|---|---|
| `logt fill warn 1 50C` | 1 条 `COUNT=1` | ✅ `seq=16904 COUNT=1` |
| `logt fill warn 10 50C` | 1 条 `COUNT=1` + 到期 1 条 `COUNT=9` | ✅ `seq=16905 COUNT=1` / `seq=16906 COUNT=9` |
| `logt fill warn 6 50C` | 1 条 `COUNT=1` + 到期 1 条 `COUNT=5` | ✅ `seq=16907 COUNT=1` / `seq=16908 COUNT=5` |
| `logt fill info 4 50D`（`ERROR_EXIT`，非白名单） | 逐条 4 条 | ✅ 4 条，未合并 |
| `logt fill warn 4 401`（`LOG_WF_START`，非白名单） | 逐条 4 条 | ✅ 4 条，未合并 |

**三项不变量**

1. **ΣCOUNT 守恒**：`1+1+9+1+5 = 17` == 注入总量 `1+10+6 = 17` ✅
2. **记录数下降**：**17 → 5（−70.6%）**；10 连发 **10 → 2（−80%）**；6 连发 **6 → 2（−66.7%）**；
   单次发生 **1 → 1（不变）**——符合"首次立即记录、重复折叠"的设计
3. **被接受记录间隔 ≥ 窗口**：`8533 / 5000 / 6941 / 5000 ms` ✅（两条 5000 ms 的正是窗口到期汇总条）

### 4.3 场景对照

| 场景 | 预期 | 实测 |
|---|---|---|
| **1. 单次异常** | DispenseGuard 立即动作 + ENTER 正常产生 | ✅ 真实会话捕获 `LOG_WEIGHT_ERROR_ENTER(CAUSE=2 RAW=0 COUNT=1)`；同期 `guard_rx == FORCE_CLOSE`（6/6、4/4、2/2…） |
| **2. 连续 10 次异常** | Valve 调用数不减 / 事件数不减 / Flash 记录显著降低 | ✅ Valve 与事件计数**不受影响**（LogManager 只是旁路消费者，物理上不参与安全链）；记录 **10 → 2** |
| **3. 异常恢复** | EXIT 正常产生，状态恢复逻辑不变 | ✅ 捕获 `LOG_WEIGHT_ERROR_EXIT(CAUSE=2 DURATION_MS=91)`，**参数与语义完全未变**；对照组 4 条逐一落地 |

### 4.4 四次失败尝试（教训，避免重复）

| 尝试 | 结果 | 原因 |
|---|---|---|
| `fill warn 24 + flush`（稀疏） | ENTER=0 | 阻塞不够密集 |
| `fill warn 62 + flush ×12`（密集） | ENTER=3 ✅ 但 `qdrop=424` / `evict_inf=160` | 记录级捕获不完整 ⇒ 无法核对 ΣCOUNT |
| `fwipe` + `mwipe`（阻塞但不产记录） | ENTER=0 | 擦除阻塞弱于写入阻塞，凑不满"5 次跳变 / 5 s" |
| `fill warn 8 + flush`（薄填充） | ENTER=0 | 单次 append 的阻塞时长不够 |

⇒ **折叠 / 去重 / 门控类验证不要靠"诱发环境异常"**，用定向注入；
且**记录级核对必须先排空云队列**（ACK 前置）。已登记 `未修复的问题.md` 的 `T-5` / `T-6`。

---

## 5. 回归

**182 / 195**（A 53/56 · B 65/65 · C 20/21 · D 24/30 · E 23/23）＝ **13 MISS**，
全部属于 **`R-7` 环境干扰类**（`replay=` / `evict_inf=` / `qdrop=` / `holes=` / `hole_evict=` /
`tx_valid=` / `partial=`，均为"绝对值 / 拓扑派生"断言）。

- **夹具 0 改动，断言仍 195 条（未降）**
- 夹具**完全不涉及 Weight 事件**（仅注释提到）⇒ 合并逻辑不可能直接命中任何断言；
  影响只能通过"环境记录总量"间接发生
- 对照 P2-H 同环境 **168/195（26 MISS）**；本改动**减少**了环境记录量，但两次运行的
  环境异常强度不同 ⇒ **不声明因果**。结论仍为 `R-7` 所定义的"需要测试隔离"

---

## 6. 架构影响分析

| 维度 | 结论 |
|---|---|
| 分层（`AI_RULES`） | 无破坏。LogManager 仍是**唯一日志存储入口**；Capability 层未新增 Flash 访问 |
| 依赖方向 | 未新增任何 include / 回调 / 反向依赖 |
| 事件机制 | EventManager 未被触碰；事件数量、顺序、订阅者全部不变 |
| System State | 未新增、未修改；合并状态为文件级 static |
| 非阻塞 | 合并路径仅整数比较 + `memcpy`；`log_task()` 阶段 0 不产生 Flash I/O |
| 并发 | 合并状态的读改全部在 `s_mux` 内；**任何 `log_emit()` 调用都在临界区之外**（避免自锁） |
| 协议 / 冻结项 | EventId、ParamId、Record 128 B、CBOR 批次头、Level Policy **全部未动** |
| 栈 / 内存 | 新增栈用量每处 ≤ 64 B（`LogParamIn[8]`）；静态状态 ~80 B |

---

## 7. 是否改变安全语义

**否。** 三条论证：

1. **路径隔离**：安全动作 `EventManager → DispenseGuard → valve_force_close()` **完全不经过 LogManager**
   ⇒ 日志侧合并/丢弃在物理上无法延迟或削弱安全动作。
2. **上游零改动**：异常检测（阈值 / 采样 / `jump_error` 判定）、事件推送（数量、优先级、策略）
   **逐字未改** ⇒ 安全链看到的输入与改造前完全一致。
3. **实测恒等**：全部会话中 `guard_rx == valve_force_close 调用数` 保持恒等
   （6/6、4/4、2/2、3/3…），`FORCE_CLOSE` 记录也照常产生。

唯一的行为变化是**历史记录的条数**，且由 `LOG_P_COUNT` 保证信息量不减（ΣCOUNT 守恒）。

---

## 8. 编译与提交

- **编译**：`SUCCESS`，`Flash 65.3% (1369801 B)` / `RAM 39.9% (130600 B)` —— 与 P2-H 固件**完全一致**
- **改动文件**：`src/log_manager.h`（`+25/−0`）、`src/log_manager.cpp`（`+248/−5`）
  - `−5` 明细（全部为**机械替换**，无逻辑删除）：
    `log_emit` 签名两行（新增 `allow_coalesce` 形参）· `rec.param_count = use_n` ·
    `for (...; i < use_n; ...)` · `const LogParamIn &p = use_p[i]`
- **行尾**：`log_manager.h` = LF、`log_manager.cpp` = CRLF（**保持原样**，补丁脚本带行尾保留与自检）
