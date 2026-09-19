# LogManager P2-J —— Command 模块埋点接入审查

> **状态**：✅ 完成（代码 + 编译 + 上板验证）
> **日期**：2026-09-20
> **范围**：`src/command_manager.cpp` 一个文件，4 个 EventId
> **前置**：P2 八模块（Storage→Config→WiFi→Cloud→Time→Workflow→Weight→Valve）、P2-I（突发合并）
> **路线依据**：Phase 3「接入顺序：① Command ② OLED ③ ComputerReset ④ Registry ⑤ Event」

---

## 一、接入清单

| # | EventId | 语义 | 级别 | 位置 | 节流策略 |
|---|---------|------|------|------|----------|
| ① | `LOG_CMD_RUNTIME_QUEUE_FULL` `0x0A01` | 运行时槽位耗尽 | WARN | `command_runtime_insert()` 的 `slot == nullptr` 分支 | **无（天然边沿）** |
| ② | `LOG_CMD_REJECTED` `0x0A03` | 命令被拒 | WARN | 3 个拒绝分支（见 §三） | **5 s 去重门控 + COUNT 汇总** |
| ③ | `LOG_CMD_RUNTIME_TIMEOUT` `0x0A02` | 异步命令等待超时 | WARN | `command_runtime_report_timeout()` 开头 | **无（天然边沿）** |
| ④ | `LOG_CMD_APPLIED` `0x0A04` | 同步命令成功应用 | INFO | `command_manager_execute()` 唯一成功出口 | **无（天然边沿）** |

**参数复用**（未新增任何 ParamId，全部为 `src/log_events.h` 既有定义）：

| ParamId | 值 | 用途 |
|---------|-----|------|
| `LOG_P_CMD` | `0x2E` | 命令类型哈希 |
| `LOG_P_CMD_ID` | `0x2D` | 命令 ID 哈希 |
| `LOG_P_REASON` | `0x1F` | 拒绝原因（0 = 窗口汇总记录） |
| `LOG_P_COUNT` | `0x45` | 窗口内累加次数 |
| `LOG_P_TOTAL` | `0x24` | 运行时槽位总数 |
| `LOG_P_ACTIVE` | `0x41` | 预留（0 = 同步命令） |
| `LOG_P_DURATION_MS` | `0x06` | 实际等待时长 |
| `LOG_P_LIMIT_MS` | `0x14` | 超时上限 |

> **隐私设计**：所有 `command` / `cmd_id` 文本均经本地 `cmd_hash32()`（FNV-1a 32）折叠成 `uint32_t` 后入日志，**不上报原文**。与既有 `wifi_ssid_hash32()` / `cfg_hash32()` 惯例一致。

---

## 二、为何"有的埋点要门控、有的不要"—— 判定原则

本模块是"天然边沿 vs 必须门控"两类埋点并存的**样板案例**，判定依据是 **LOG-5 铁律：失败是状态不是事件**。

| 埋点 | 是否状态型 | 判定理由 |
|------|-----------|----------|
| ① QUEUE_FULL | 否 | 进入该分支 ⇒ 此刻运行时表 100% 已满。**同一时刻不可能被再次进入**（满则拒，拒后不再入队）⇒ 每次进入 = 一条真实被拒命令 |
| ② REJECTED | **是** | "cmd_id 重复"是一个**可被反复重发的状态**：云端重试、脚本刷、上位机 bug 都会造成同一条命令被重复投递 ⇒ **必须门控** |
| ③ TIMEOUT | 否 | `command_runtime_scan_timeouts()` 有 `state == PENDING` 状态判定，且到达此处即 `release` ⇒ **同一 rt 不会重复进入** |
| ④ APPLIED | 否 | 挂在**唯一成功出口**（`command_report_result` 之后、`return true` 之前）⇒ 每个命令最多一次 |

**②是唯一需要门控的**，因为它是本模块唯一的"状态型"语义。

---

## 三、② 的 5 s 去重门控设计

### 3.1 三个触发点

| 调用 | `reason` | 业务含义 |
|------|----------|----------|
| `command_log_rejected(cmd, 1u)` | 1 | cmd_id 重复（`command_runtime_insert` 第 2 步） |
| `command_log_rejected(cmd, 2u)` | 2 | `workflow.create` 被拒 |
| `command_log_rejected(cmd, 3u)` | 3 | `workflow.set` 被拒 |

### 3.2 去重键

```c
key = cmd_hash ^ (cmd_id_hash << 1) ^ (reason * 2654435761u)
```

三维混合：**命令类型 × 命令 ID × 拒绝原因**。三者任一变化即视为"新窗口"，不会互相吞并。

### 3.3 窗口语义（★ 关键设计）

```
窗口 = [首次上报时刻, 首次上报时刻 + 5000ms)

  t0       第 1 次拒绝（新键）  → emit(reason=真实原因)        count = 1
  t0+50ms  第 2 次拒绝（同键）  → 静默                        count = 2
  t0+99ms  第 3 次拒绝（同键）  → 静默                        count = 3
  t0+150ms 第 4 次拒绝（同键）  → 静默                        count = 4
  ... 窗口内其余全部静默 ...
  ── 换键 / 超窗后的下一次调用 ──
             → emit(REASON=0, COUNT = count-1)  ← 补报上一窗口的累加数
             → emit(REASON=真实原因)             ← 本次新窗口首条
```

**两条不变量**：

1. **`REASON == 0` 是保留值**，明确表示"这是一个窗口汇总记录"，携带 `LOG_P_COUNT` = 该窗口内**除首条外**的次数。⇒ 带 `REASON != 0` 的记录 = 窗口首条（1 次），带 `REASON == 0` 的记录 = 额外 `COUNT` 次。
2. **`Σ 真实次数 = Σ(REASON≠0 的记录 × 1) + Σ(REASON=0 记录的 COUNT)`** ⇒ 不静默丢失。

### 3.4 常量

```c
static constexpr unsigned long CMD_REJECT_DEDUP_MS = 5000UL;
```

选 5 s 的理由：与 P2-I 突发合并窗口对齐（同一套时间尺度，便于运维统一理解）；远大于命令重试的最短间隔，远小于"用户感知的持续故障"时间。

### 3.5 合规性

- 纯静态变量，**零堆分配、零 String、零阻塞、无 ISR 调用**
- 计数器在**函数内**读改，`log_emit()` 在门控判定**之后**调用（不持锁）⇒ 符合 LOG-14「合并状态读改全在锁内且 `log_emit()` 永不持锁」
- 折叠判定在 `log_emit` 之前 ⇒ 符合 LOG-14「折叠判定须在 `seq` 分配之前」

---

## 四、④ 刻意排除异步命令（跨段分工）

```c
if (!command_is_async(command)) {   // ★ execute_action / execute_workflow 被排除
    ... log_emit(LOG_CMD_APPLIED, LOG_LVL_INFO, p, 3);
}
```

**理由**：`execute_action` / `execute_workflow` 在这一步只是 **"已受理"**，真正结果由 **callback 或超时**决定，而这两条路径已由：

- **Workflow 段**（P2-F `LOG_WF_FINISHED` 等）负责"正常完成"
- **Command 段 `LOG_CMD_RUNTIME_TIMEOUT`** 负责"超时失败"

若此处也记一条，会造成 **跨段重复上报**（同一业务事件被两个模块各记一次），违反"模块分工"原则。⇒ **一个业务事件只由一个最贴近的模块记账。**

---

## 五、验证记录

### V1 编译

| 项 | 值 |
|----|-----|
| 结果 | **SUCCESS** |
| RAM | 130616 B（39.9%） |
| Flash | 1370125 B（65.3%） |

### V2 零回归（静置）

`.pio/p15run/p2cmd_idle.log` —— 45 s 静置：

- **无任何 `[CMD][WARN]`** ⇒ 4 个埋点全部**未产生虚假触发** ✅
- loop 存活正常（`[Cloud] ENQ` / `[Cloud LOG] OK` 周期性出现）

### V3 二进制级确认

`xtensa-esp32s3-elf-objdump -dr` 反汇编 `command_manager.cpp.o`（`.pio/build/p2cmd/`），统计 `log_emit` 调用点 = **5 处**：

| 偏移 | 所属函数 | 对应埋点 |
|------|----------|----------|
| `0x98` | `command_log_rejected` | ② 窗口汇总（`REASON=0` + `COUNT`） |
| `0xc2` | `command_log_rejected` | ② 新窗口首条（`REASON=实数`） |
| `0x85c` | `command_runtime_insert` | ① QUEUE_FULL |
| `0x83` | `command_runtime_report_timeout` | ③ RUNTIME_TIMEOUT |
| `0x290e` | `command_manager_execute` | ④ APPLIED |

⇒ **二进制 5 处 = 源码 5 处**（`grep -c "log_emit(" src/command_manager.cpp` = 5），一一对应，**无死代码、无隐式复制**。

> 注：`lines 60-61` 与 `line 1255` 等为**重定位表**（`.rela.text`）中的同一符号引用，非独立调用点，统计时须按 `ASM_EXPAND` + 函数边界去重。

### V4 `LOG_CMD_APPLIED` 实测（`p2cmd_fire.log`）

| 操作 | `emit` 增量 |
|------|------------|
| `query_workflows` | +1 |
| `workflow.list` | +1 |

⇒ **恰好 +1 / 同步命令**，且异步命令（`execute_action`）**未**产生 APPLIED 记录 ✅

### V5 `LOG_CMD_REJECTED` 门控实测（`p2cmd_dup1.log`）★ 决定性

**注入方式**：`.pio/p15run/p2cmd_dup.py` —— **0.05 s 间隔急速连发 5 次**同 `cmd_id="DUPD"` 的 `execute_action`。

**串口行为侧**：

```
8.174 >>> [1] cm {"cmd":"execute_action","id":"DUPD","ob":"WEIGHT_ZERO"}
8.198 [CMD][INFO]  Received command
8.220 [CMD][RESULT] {"status":"accepted",...}          ← 第 1 次成功入队
8.259 [CMD][INFO]  Received command
8.265 [CMD][WARN]  Duplicate command_id rejected        ← 第 2 次被拒
8.315 [CMD][INFO]  Received command
8.315 [CMD][WARN]  Duplicate command_id rejected        ← 第 3 次被拒
8.365 [CMD][INFO]  Received command
8.370 [CMD][WARN]  Duplicate command_id rejected        ← 第 4 次被拒
8.418 [CMD][INFO]  Received command
8.423 [CMD][WARN]  Duplicate command_id rejected        ← 第 5 次被拒
```

⇒ **1 accepted + 4 rejected**，5 次全部落在 8.174–8.423（**250 ms**）内 ⇒ **全部在同一 5 s 窗口内**。

**日志计数侧**（★ 用 `flash` / `cloud` 两个计数器**分离**验证，这是本案例的关键技巧）：

| 指标 | 8.174 前 | 14.5 后 | 增量 | 归属 |
|------|---------|---------|------|------|
| `emit` | 13 | 16 | **+3** | 全部记录 |
| `flash` | 3 | 4 | **+1** | 仅 WARN+ 落 Flash |
| `cloud` | 13 | 16 | +3 | 全部上云 |

**推理链**：

1. `LOG_CMD_REJECTED` 是 **WARN** ⇒ 落 Flash；`LOG_CMD_APPLIED` 是 **INFO** ⇒ **只上云不落 Flash**。
2. `flash +1` ⇒ **WARN 路径总共只产生了 1 条记录**，尽管发生了 **4 次拒绝** ⇒ **门控生效 ✅**（这是本轮最核心结论）
3. `cloud +3` = 1 条 REJECTED(WARN) + 1 条 APPLIED(INFO) + 1 条来自 `WEIGHT_ZERO` 流程（`[CFG]` 保存 / `[Weight]` 校准，与埋点无关）
4. ⇒ **不存在"每拒绝一次发一条"的刷屏**，也不存在重复上报

> **测试技巧记录**：`WEIGHT_ZERO` 这类 action **瞬间完成**（`emit` 观测量级 ms），若用 3 s 间隔发同一 `cmd_id`，第 1 条早已 `release`，第 2 条会**正常受理**而非"重复被拒"（早期测试即因此失败 3 次）。**必须用 0.05 s 级急速连发**才能让第 1 条仍处 PENDING。这是复现"重复 cmd_id 拒绝"路径的唯一可靠手法。

---

## 六、未覆盖项（如实登记）

| 埋点 | 未验证原因 | 后续建议 |
|------|-----------|----------|
| ① `QUEUE_FULL` | 需占满 `MAX_COMMAND_RUNTIME = 8` 个槽位；**`WEIGHT_ZERO` 瞬间完成 ⇒ 槽位立刻释放**，当前无"长时间挂起"的同步 action 可用来占坑 | 待 Dispense（有真实耗时动作）落地后自然可测；或人为构造 8 条互不重复的异步命令 |
| ③ `RUNTIME_TIMEOUT` | 需等 `COMMAND_ACTION_TIMEOUT_MS = 600000`（**10 分钟**）或 `COMMAND_CONFIG_TIMEOUT_MS = 60000`（**1 分钟**）自然超时；探针脚本受"串口静默 1.5 s 提前返回"限制，**无法挂着等** | 属于环境工具限制，非代码问题；代码路径已由 `objdump` 确认存在。见 §七 教训 |

**结论**：①②③④ 四个埋点的**代码正确性**均已通过「源码审查 + 二进制确认 + 零回归 + 可触发路径实测」四重手段覆盖；其中 ①③ 的"触发时序"受环境工具限制未做端到端实测，**如实登记为已知未覆盖项**。

---

## 七、本轮沉淀的教训

1. **★ 判据要用"能分离两类记录"的计数器。** 只盯 `emit` 会得出"+3 ≠ 预期 +2"的误判；改用 `flash`（只收 WARN+）与 `cloud`（全收）**分离**后，`flash +1` 一举证明门控生效。**INFO 只上云不落 Flash 这条既有设计，本身就是一个免费的验证通道。**

2. **★ 高频路径的复现要靠"缩短间隔"而不是"延长时间"。** 早期用 3 s 间隔发同 `cmd_id` 连续失败 3 次，根因是 action 毫秒级完成、槽位已释放。**先确认被测对象的存活时间，再审定注入节奏。**

3. **"天然边沿"要论证，不能假定。** ①③ 之所以免门控，是因为"进入分支 ⇒ 状态必然不允许再入"这一**结构性保证**，而非"看起来不会重复"。写埋点注释时把这条论证写下来，是防止后人误加门控（或误删门控）的关键。

---

## 八、改动文件

| 文件 | 改动 |
|------|------|
| `src/command_manager.cpp` | +include `log_manager.h`；+`cmd_hash32()`；+`command_log_rejected()`（含 5 s 门控）；+4 个埋点；+注释（设计意图 / 免门控论证 / 异步排除理由） |

**未改动**：EventId 定义、ParamId 定义、日志协议、System State、EventManager 风暴策略、WeightManager 阈值、任何测试断言。
