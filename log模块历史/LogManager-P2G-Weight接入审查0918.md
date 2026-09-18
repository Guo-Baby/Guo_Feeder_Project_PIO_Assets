# LogManager P2-G · Weight / HX711 接入前审查

> 日期：2026-09-18 · 基线：`3b68a3f`（P2-F 完成，回归 196/196）
> **只审查、未改任何生产代码**（`src/` 零改动）。实现待确认后再开始。
> 结论先行：**发现 3 处"矩阵与代码实际能力不一致" ⇒ 按约定立即停止并报告**（见 §5）。

---

## 1. 审查范围

| 项 | 内容 |
|---|---|
| 源码 | `src/weight.cpp`（645 行）、`src/weight.h`（41 行）、`src/dispense_guard.cpp`（73 行，`EVENT_WEIGHT_ERROR` 的**唯一消费者**） |
| 冻结 EventId | Weight 相关 **5 个**：`LOG_WEIGHT_ERROR_ENTER`(0x050C,WARN) · `ERROR_EXIT`(0x050D,INFO) · `ZERO_DONE`(0x050E,INFO) · `TRIGGER_FIRED`(0x050F,INFO) · `CALIB_FAILED`(0x0510,ERROR)；**注意 0x0511/0x0512 尚未定义**（只在 P2 前置审查里作为*提案*出现） |
| ParamId | 全部已存在，**无需新增**：`CAUSE` `WEIGHT_G` `RAW` `OFFSET` `SAMPLES` `SAVED` `TARGET_G` `DELTA_G` `ERR_CODE` `DURATION_MS` |
| 现有埋点 | **0 处**（`grep -c log_emit src/weight.cpp` = 0） |

---

## 2. HX711 采样链路与频率分析（★ 本模块的核心约束）

```
main.cpp loop()  ──每轮调用──▶  weight_task()                      ← 调用频率 ≫ 采样频率
                                   │
                                   ├─ if(!scale.is_ready()) ──▶ 提前返回（**没有等待**）
                                   │
                                   └─ is_ready()==true ──▶ scale.read()   ← 每次就绪只读 1 次
                                                              │
                        filter_samples[5] 滑动窗口（去最大/去最小/取平均 3 点）
                                                              │
                                        每 5 次有效采样 ⇒ 1 个滤波值 ⇒ current_weight 更新
```

| 量 | 值 | 出处 |
|---|---|---|
| HX711 采样率 | **≈10 Hz**（数据手册默认档） | 代码注释 `weight.cpp:41` |
| **`current_weight` 更新率** | **≈2 Hz**（10 Hz ÷ 5 点窗口） | `WEIGHT_FILTER_WINDOW=5` |
| `weight_task()` 进入率 | 每个 loop 迭代（**每秒数千次**） | `main.cpp:585` |
| `state_set_float(STATE_WEIGHT_VALUE)` | active 时 2 Hz；空闲时 **1/30 s**（`WEIGHT_IDLE_STATE_UPDATE_MS`） | `weight.cpp:248-257` |
| `EVENT_WEIGHT_ERROR` 推送 | ① 跳变：**≤2 Hz**（每窗口最多 1 次）② not-ready：**每 5 s 最多 1 次** ③ trigger start 失败 | `weight.cpp:216 / 323 / 465 / 486` |
| 跳变计数窗口 | 5 s 内 ≥5 次 ⇒ 异常（`WEIGHT_JUMP_DELTA_G=50 g`） | `weight.cpp:65-67` |

> **⇒ 铁律：`weight_task()` 的采样/滤波路径、`weight_filter()`、`weight_record_jump()` 一律不可埋点。**
> 前三者的量级是 10 Hz / 2 Hz / 2 Hz，任何一条都会在数分钟内写满 496 条 Flash 环。

### 2.1 ★ 板上实测（不是估算）

回归日志（`.pio/p15run/*.log`，P2-F 最终固件）：

| 段 | `STATE_WEIGHT_ERROR` 迁移 | `DispenseGuard` 收到事件 | 跳变事件 |
|---|---|---|---|
| A | 0 | 0 | 0 |
| **B** | **4**（2 组 enter/exit） | 0 | 0 |
| **C** | 0 | **4** | 0 |
| D / E | 0 | 0 | 0 |

两条重要事实：

1. **B 段 2 组迁移的时间戳是同毫秒**（`22:05:56.987 → 1` 与 `→ 0`）⇒ 该板的 HX711 处于**"值有效/无效快速交替"**状态（`raw_zero` 或 `no_data` 判定反复跨阈）。⇒ **`ERROR_ENTER` 的边沿在本板会真实触发，回归必须重跑验证**（同 P2-E 的教训：Boot 期多一条 WARN 就会扰动绝对计数断言）。
2. **C 段 4 次 `EVENT_WEIGHT_ERROR` 却没有状态迁移** ⇒ 这些来自 **`weight_record_jump()`**（跳变不需要进 error_state）⇒ **跳变源在本板真实存在**，且每次都让 `dispense_guard` 调 `valve_force_close()`（实测同一秒内 2 次强制关阀）。⇒ 验证了"**绝不能在 `weight_record_jump()` 埋点**"。

---

## 3. Weight 的实际状态清单（**用户列的 9 个状态 vs 代码**）

代码里**没有集中式状态机**；"状态"由 3 个独立布尔量 + 1 个校准标志表达：

| 用户要求的状态 | 代码中的实际载体 | 能否作为独立事件存在 |
|---|---|---|
| 初始化 | `initialized`（`weight_init()` 末尾置位） | ❌ **无冻结 EventId**（无 `LOG_WEIGHT_INIT`）⇒ 缺口 |
| ready | **不存在**（隐含在 `initialized` + `is_ready()` 通过） | ❌ 无状态 ⇒ 不新增虚假检测 |
| calibration | `calibrating`（`weight_zero_calibrate()` 置位，`weight_task()` 采样 20 点后清） | ⚠️ **只有"完成"有宿主**（`ZERO_DONE`/`CALIB_FAILED`），**"开始"无宿主** |
| zero / tare | 校准完成的产物：`zero_offset` + `config_set_weight_zero_offset()` + `config_save()` | ✅ `ZERO_DONE` |
| abnormal | `error_state`（`STATE_WEIGHT_ERROR` 的 RAM 镜像，`:204` 边沿） | ✅ `ERROR_ENTER` / `ERROR_EXIT` |
| timeout | **不是独立状态** —— 是 `error_state` 的 3 个成因之一（`no_data`：HX711 ≥5 s 无数据） | ✅ 由 `CAUSE` 位掩码区分（见 §4.2） |
| sensor error | 同上：`no_data`（无数据）/ `raw_zero`（raw 连续 5 s 为 0） | ✅ 由 `CAUSE` 区分 |
| overload | **代码中完全不存在**（无超载检测） | ❌ **无宿主、无检测** ⇒ 不新增虚假逻辑 |
| unstable | **不存在独立状态**；最接近的是 **跳变检测**（5 s 内 ≥50 g 跳变 ≥5 次 ⇒ 进 abnormal） | ❌ 不能独立成事件；由 `ERROR_ENTER` 的 `CAUSE.bit2` 间接体现 |

**⇒ 结论：用户列的 9 个状态中，只有 4 个（calibration 完成 / abnormal 进 / abnormal 出 / zero）在代码里有真实、可观测的状态迁移。其余 5 个要么被 `CAUSE` 覆盖（timeout / sensor error / unstable），要么根本不存在（ready / overload），要么缺 EventId（初始化 / 校准开始）。**

---

## 4. EventId ↔ 代码宿主 对应表（含频率/边沿/聚合判定）

### 4.1 建议埋点（5 处，全部复用冻结 ID）

| # | EventId | Level | 宿主（文件:行 + 函数） | 参数 | 频率 | 判定 |
|---|---|---|---|---|---|---|
| 1 | `LOG_WEIGHT_ERROR_ENTER` (0x050C) | WARN | `weight.cpp:204` `weight_refresh_error_state()` 的 `err != error_state && err` 分支 | `CAUSE`(位掩码) `WEIGHT_G` `RAW` | **状态边沿**，最短周期 ≈5 s（进入条件需连续 5 s 异常） | **必须边沿**（天然边沿，代码已有 `if(err != error_state)`） |
| 2 | `LOG_WEIGHT_ERROR_EXIT` (0x050D) | INFO | 同函数 `err != error_state && !err` 分支 | `CAUSE`(锁存的进入原因) `DURATION_MS` `WEIGHT_G` | 同上（与 ① 一一配对） | **必须边沿** |
| 3 | `LOG_WEIGHT_ZERO_DONE` (0x050E) | INFO | `weight.cpp:288-308` 校准完成且 `calibrate_ok==true` | `OFFSET` `SAMPLES`(=20) `SAVED`(=1) | **每次校准 1 条**（云端命令 / `WEIGHT_ZERO` action 触发） | 事件驱动，无需限流 |
| 4 | `LOG_WEIGHT_CALIB_FAILED` (0x0510) | ERROR | 同处 `calibrate_ok==false`（`config_set_weight_zero_offset()` 失败 **或** `config_save()` 失败） | `OFFSET` `ERR_CODE` | 同上 | 事件驱动 |
| 5 | `LOG_WEIGHT_TRIGGER_FIRED` (0x050F) | INFO | `weight.cpp:525` `weight_trigger_poll()` 的 `weight_loss >= trigger_gram` 分支 | `TARGET_G` `DELTA_G` `WEIGHT_G` | **一次出水 1 条**（2 Hz 轮询但只在跨阈那一刻成功 1 次） | 事件驱动（成功即退出运行态 ⇒ 天然一次性） |

**3/4 互斥**（同一处 `if/else`），**1/2 互斥**（同一 `if(err != error_state)` 的两半）⇒ **不会重复上报**。

### 4.2 `CAUSE` 编码（与矩阵有一处偏差，见 §5）

矩阵 §10.2 写的是 `1=not_ready / 2=raw_zero / 3=jump`（**枚举语义**）。但代码里 `no_data`、`raw_zero`、`jump_error` **可以同时为真**（实测 B 段就是快速交替）。

⇒ 建议改为 **位掩码**：`bit0=no_data(1)` · `bit1=raw_zero(2)` · `bit2=jump(4)`。这样"三种同时成立"= `7`，信息不丢。
（这是**观测编码**的选择，不涉及新增检测逻辑；但确实偏离矩阵字面，故列入 §5 待确认。）

### 4.3 必须**聚合**的事件

**本模块无需要求**。原因：唯一的高频源（10 Hz 采样 / 2 Hz 滤波 / 2 Hz 跳变）**全部被排除在埋点之外**；剩下的 5 个埋点中，最高的也是"≥5 s 一次的状态边沿"。⇒ **不需要计数聚合、不需要 N 次记 1 次**。

### 4.4 不可记录（**禁止埋点**）清单

| 位置 | 频率 | 为什么禁止 |
|---|---|---|
| `weight_task()` 采样/滤波主体（`:267-421`） | 进入率 = 每 loop；`current_weight` 2 Hz | 10 次/秒级 ⇒ 数分钟写满 496 条 Flash 环 |
| `weight_filter()`（`:150`） | 2 Hz | 同上 |
| **`weight_record_jump()`（`:216`）** | **≤2 Hz 突发，实测本板真实发生** | 每次跳变都 push 事件；**且 5 s 内可能连发 5 次**。已确认"由 `error_state` 边沿间接体现" |
| `weight_task()` not-ready 分支的 `event_push`（`:327`） | 每 5 s ≤1 次 | 与 `ERROR_ENTER`（同因、同 5 s 阈值）**语义重复** ⇒ 只保留边沿 |
| `weight_update_state_value()`（`:248`） | 2 Hz / 1/30 s | 纯数值刷新，非状态迁移 |
| `weight_trigger_poll()` 的 `TRIGGER_RUNNING` 分支（`:535`） | 2 Hz | 轮询未满足=无事发生 |
| `weight_init()` 的 Serial 打印 | 每 Boot 1 次 | 无 EventId（见 §5） |

---

## 5. ★★ 三处"矩阵与代码实际能力不一致"（**按约定立即停止并报告**）

### 【不一致 ①】`weight_trigger_start()` 的两个失败路径**不能**复用 `LOG_WEIGHT_ERROR_ENTER`

矩阵 §10.2 末两行建议：

| 矩阵原文 | 问题 |
|---|---|
| `weight_trigger_start()` :463 异常中止 → `ERROR_ENTER` + `CAUSE=4` | **语义重复**：该分支的前提就是 `state_get_bool(STATE_WEIGHT_ERROR)==true`（`:463`）⇒ 这个 `ERROR_ENTER` **早已在进错那一次报过了**（边沿）。在此再报一条，会得到"1 个 EXIT 配 2 个 ENTER"的不配对序列。而且此处**不产生任何状态迁移**。 |
| `weight_trigger_start()` :483 参数非法（`gram` 越界 ⇒ 回落 20） → `ERROR_ENTER` + `CAUSE=5` | **语义失真**：该分支**不会**置 `STATE_WEIGHT_ERROR`（代码只 `event_push` + Serial），⇒ 若发 `ERROR_ENTER` 就**永远没有配对的 `ERROR_EXIT`**，云端会看到"秤一直异常"。这正是"**为了满足矩阵而制造虚假状态记录**"。 |

**⇒ 建议：这两处都不埋点。** 它们的可观测性已由 `EventManager`（`EVENT_WEIGHT_ERROR` ⇒ `dispense_guard`）承担；若确实需要，应**单独评审新增** `LOG_WEIGHT_TRIGGER_ABORTED`（INFO/WARN）——**不在 P2-G 夹带**。

### 【不一致 ②】"校准开始"无宿主（矩阵自己建议新增 `LOG_WEIGHT_CALIB_START`）

矩阵 §10.2 第 5 行：`weight_zero_calibrate()` :634 → **建议新增 `LOG_WEIGHT_CALIB_START`**（P2 前置审查给它编号 `0x0512`，标"低（可选）"）。

**事实**：`0x0511` / `0x0512` **在 `log_events.h` 中并不存在**（冻结表止于 `0x0510`）⇒ 要用就得**改冻结契约**（需同步 4 处：`log_events.h`、Guide 附录 A、矩阵、云端 EventId 字典）。

**P2 已拍板"暂不新增 EventId"** ⇒ 建议**不埋**，把"校准开始"记为缺口。
代价（如实说明）：**无法区分"校准卡在中途"与"从未开始"** —— 只有 `ZERO_DONE`/`CALIB_FAILED`，没有 START。缓解：校准是**云端命令 / Workflow action 触发**的（低频率、有命令回执），可从命令侧侧证。

### 【不一致 ③】"初始化"无宿主

`weight_init()` 失败/成功都只有 `Serial.printf`，**没有 `LOG_WEIGHT_INIT` 之类的冻结 ID**。
⇒ 建议**不埋**（P2-D 的 `CLOUD_FRAG_FAIL`、P2-E 的 `NTP_FAIL` 同例：**冻结 ID 无宿主 ⇒ 不埋、只记录**）。
可接受的替代：`weight_init()` 里的两条 `workflow_register_*` 失败其实已可由 **Workflow 段**的 `LOG_WF_*` 与 Registry 侧覆盖（注册失败会体现在 Workflow 命令可用性上）。

> **另注（不属本阶段，但必须提前告知 P2-H）**：`LOG_VALVE_OVERFLOW_RISK`(0x0507) 的注释写着"**需新增检测**"，而这正好对应已知 P0 项"`valve_force_close()` 后无残余增重检测"。它属 **Valve/Dispense 段**，**不在 Weight 阶段新增**。

---

## 6. 需要新增的**只读**状态（纯观测，不改控制流）

为使 `ERROR_EXIT` 能带出"进入原因 + 故障持续时长"，需要 2 个模块级静态量：

```c
static uint8_t       weight_err_cause_latched = 0;   // ENTER 时的 CAUSE 位掩码
static unsigned long weight_err_enter_ms      = 0;   // ENTER 时刻（算 DURATION_MS）
```

**边界声明**：这两个变量**只被 `weight_refresh_error_state()` 的边沿分支读写**，不参与 `err` 判定、不影响 `error_state`、不接触 HX711 算法 / 采样周期 / 滤波参数 / 重量控制逻辑。
**替代方案（若你希望"零新增状态"）**：`ERROR_EXIT` 只带 `WEIGHT_G`，不带 `CAUSE`/`DURATION_MS` ⇒ 丢失"故障持续了多久"，而这是最有诊断价值的一项。**建议采用前者**（与矩阵 §10.2 的要求一致）。

---

## 7. 回归影响评估（基于实测，同 P2-E 的教训）

| 影响 | 实测依据 | 处置 |
|---|---|---|
| **Boot/会话期会多出 WARN 记录**（`ERROR_ENTER` 是 WARN ⇒ **落 Flash**） | B 段实测 **2 组迁移**（≈2 条 WARN）；C 段另有 4 次事件（若在 `weight_record_jump()` 埋点就会是 4 条，但我们不埋） | 新增 WARN 会进入同一条日志流 ⇒ **`test/log_fix_tests.txt` 的绝对计数断言（`evict_inf`/`qtotal`/`replay`/`fdrop`/`total`/`recs`/`qused`）可能被扰动**。实现后**必须全量重跑 196 条**；若失败，按既定规矩**改夹具（顺序无关 / 非零判据）而不是削断言语义** |
| `ERROR_EXIT` 是 INFO ⇒ 不落 Flash | Level Policy | 只影响云端记录数，不影响 Flash 环 |
| `TRIGGER_FIRED` / `ZERO_DONE` 是 INFO | 需真机触发 Workflow / 校准 | 不在回归集内（回归不跑 Workflow/校准）⇒ **对 196 条无影响** |
| `CALIB_FAILED` 是 ERROR ⇒ 落 Flash | 只在保存失败时 | 回归不触发 |

---

## 8. 风险清单

| ID | 风险 | 影响 | 建议 |
|---|---|---|---|
| **R-1** | **边沿抖动**：本板实测 `-> 1` 与 `-> 0` **同毫秒**发生 ⇒ 若持续抖，最坏 ≈**12 ENTER + 12 EXIT / 分钟** | 12 条 WARN/min 落 Flash ⇒ 496 槽约 **6–7 分钟**填满并开始淘汰其它模块的 WARN+ | **接受**（这是真实硬件故障，本身就属 WARN+；且进入条件需要"连续 5 s 异常"，抖动本身就是病灶）。**不为此新增迟滞逻辑**（那属改行为）。记入风险，若将来抖动频繁再单独评审 |
| **R-2** | `weight_record_jump()` 的 2 Hz 跳变源在本板**真实存在**（C 段 4 次） | 若误埋点 ⇒ 洪水 | 已列入 §4.4 禁止清单 |
| **R-3** | **跳变源会让 `dispense_guard` 反复强制关阀**（实测同一秒 2 次） | **安全相关**；`LOG_VALVE_FORCE_CLOSE` 是 **CRITICAL + IMM**，**P2-H 若直接埋点会得到 2 条/s CRITICAL** | ⚠️ **P2-H 开工前必须先决定"事件去重/边沿"**；本阶段只记录（`dispense_guard.cpp` 不在 P2-G 范围） |
| **R-4** | `weight_is_active()` **在 `weight.h:41` 声明，全仓库无定义、无调用者**（死声明） | 无功能影响；潜在链接陷阱 | 记录（不属日志范围，不改） |
| **R-5** | `weight_trigger_start()` 的 `EVENT_PRIORITY_NORMAL`（参数非法）与 `CRITICAL`（异常中止）**都会触发 `dispense_guard` 强制关阀** —— 即"参数写错"也会关阀 | 行为问题（非日志） | 记录；建议单独评审 |

---

## 9. 推荐实施方案（**待你确认后执行**）

```
src/weight.cpp 纯增量追加（目标 +80/−0 量级）：
  + #include "log_manager.h"
  + static uint8_t weight_err_cause_latched / weight_err_enter_ms   （§6）
  + :204 边沿两半 → ERROR_ENTER / ERROR_EXIT
  + :288-308 校准完成 if/else → ZERO_DONE / CALIB_FAILED
  + :525 触发成功 → TRIGGER_FIRED

不碰：HX711 算法、采样周期、滤波参数、重量控制逻辑、状态机、
      event_push 行为、Serial 打印（保留原样，与 Config 桥接的"双路"不同 ——
      这里 Serial 与 LogManager 是**不同粒度**，不重复）
```

**预期回归**：Boot 期新增 1–2 条 WARN（实测 B 段 2 组迁移）⇒ 可能扰动绝对计数断言 ⇒ 按规矩改夹具。

---

## 10. 待你拍板（4 项）

| # | 事项 | 选项 |
|---|---|---|
| 1 | **`weight_trigger_start()` 两个失败路径**（不一致 ①） | **A. 不埋**（推荐） / B. 单独评审新增 `LOG_WEIGHT_TRIGGER_ABORTED` |
| 2 | **"校准开始"**（不一致 ②） | **A. 不埋**（推荐，守"暂不新增 EventId"） / B. 本次新增 `0x0512`（需同步 4 处文档 + 云端字典） |
| 3 | **`CAUSE` 编码**（§4.2） | **A. 位掩码**（推荐，`1/2/4`，可表达多因同时） / B. 严格按矩阵枚举 `1/2/3`（单因，多因时按优先级取一个） |
| 4 | **`ERROR_EXIT` 是否带 `CAUSE`+`DURATION_MS`**（§6） | **A. 带**（需 2 个只读静态量，推荐） / B. 不带（零新增状态，但丢失故障时长） |

---

## 11. 附：本阶段**不**做的清单（明确边界）

- ❌ 不改 HX711 算法 / 采样周期 / 滤波参数 / 重量控制逻辑
- ❌ 不新增 EventId / ParamId（0x0511、0x0512 保持未定义）
- ❌ 不为"ready / overload / unstable"新增检测逻辑（代码里没有这些状态）
- ❌ 不动 `dispense_guard.cpp`（R-3 只记录，留 P2-H）
- ❌ 不动 `event_push` 的次数 / 优先级 / 策略
- ❌ 不改 `weight_record_jump()`、not-ready 分支的既有节流
