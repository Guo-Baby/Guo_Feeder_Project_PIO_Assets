# 固件修改清单（含模块、改动、测试依赖、回滚）

> 日期：2026-10-23 · 行号基线：提交 `92f34c1`
> **本文件是清单，不是实现。** 每一项在实施前必须单独评审；
> 涉及执行器安全语义（F-2/F-4）、持久化（F-5）的项，用户明确要求"先给依据、风险和最小方案"。

---

## 0. 总览

| # | 改动 | 需改模块 | 风险 | 破坏 registry 稳定 | 需真机 | 前置 |
|---|---|---|---|---|---|---|
| F-1 | 新增 `safety.cpp/.h`（启动安全电平 + 安全停堆编排） | 新文件 | 低 | 否 | 否 | — |
| F-2 | `main.cpp` 在 WiFi 前调 `safety_outputs_init()` | main | **中** | 否 | 是（电气验证） | F-1 |
| F-3 | `safety_router`（`stop_all` / `clear`） | command_manager | 低 | **否**（故意不做成 Action） | 是 | F-1 |
| F-4 | 强制重启 `system.force_restart` | system_command | **高** | 否 | 是 | F-5 |
| F-5 | Flash 写保护闸门 `flash_write_busy` | config_manager / workflow_storage / json_storage | 中 | 否 | 串口 | — |
| F-6 | 新增 `STATE_BOOT_SEQ` | system_state + command_manager | 低 | 否 | 串口 | F-7 |
| F-7 | `boot_seq` 自增并持久化 | device_identity（或 version.json） | 低 | 否 | 串口 | — |
| F-8 | 阀 `safety_timeout_sec` 默认 300 → 60，范围下限 1 → 10 | config_manager + valve | 低 | 否 | 是 | — |
| F-9 | 普通重启"Critical 存在时拒绝"语义 | system_command + command_manager | 中 | 否 | 串口 | — |

**Flash/RAM 影响预估**：F-1 + F-3 + F-4 + F-9 ≈ **+3~5 KB Flash / +0.5 KB RAM**（粗估，需实测）。
按 G-1 护栏（Flash <85%）当前余量充足，但**每次合并后必须跑构建后整体余量检查**。

---

## 1. F-1 · `src/app/safety.{h,cpp}`（新文件）

### 改动

```cpp
// safety.h
void safety_outputs_init();          // 启动期：按 config 把三个执行器 GPIO 置安全电平
bool safety_stop_all(SafetyReport &out);   // 停堆：workflow_stop_all + 三个物理停止 + 读回
bool safety_clear_latch();           // 解除 latched
bool safety_is_latched();
void safety_task();                  // 非阻塞状态机推进（若采用分步）
```

### 职责边界（不得越界）

| 可以 | 不可以 |
|---|---|
| 调 `workflow_stop()`（公开 API） | 直接改 `workflows[i].state` |
| 调 `valve_force_close()` / `stepper_emergency_stop()` / `vibration_stop()` | 直接写 GPIO（**除** `safety_outputs_init` 的启动期） |
| 读 `valve_is_open()` 等只读接口 | 注册新的 Action / Trigger |

### 为什么不注册成 Action

注册 Action 会触发 **stable_id 全量重排 + `v` 递增**（`v:4 → v:5`），
导致云端所有缓存的 `k` 全部失效（MEMORY 铁律）。
⇒ **必须**走独立一级命令 `c="safety"`，保持 registry 不变。

### 测试依赖

| 测试 | 层 |
|---|---|
| 无 workflow 时 `stop_all` 返回 `success` + `terminated=[]` | 串口 |
| `latched` 期间 `workflow_action_start` 被拒 | 串口 |
| `safety_clear_latch()` 后可正常跑 | 串口 |

### 回滚

删文件 + 撤 F-2/F-3 的调用。无持久化副作用。

---

## 2. F-2 · 启动期安全电平（`main.cpp`）

### 现状与问题

| 位置 | 代码 | 说明 |
|---|---|---|
| `main.cpp:535` | `config_init()` | 配置可用 |
| `main.cpp:546` | `ble_init()` | |
| `main.cpp:547` | `wifi_init()` | ★ 网络从这里开始，可能阻塞数百 ms |
| `main.cpp:556` | `valve_init()` | ★ 阀门到这里才被配成输出 |

⇒ 复位后 ~200~1500 ms 内，`valve.gpio_pin` / `stepper.en_pin` / `vibration.relay_pin`
**保持复位默认（输入/高阻）**。继电器是否导通取决于模块输入级设计（多为光耦 + 下拉，通常不导通，
但**本机硬件未实测** ⇒ 见 `V-5`）。

### 改动（最小）

在 `main.cpp` 的 `config_init()`（535）之后、`event_manager_init()`（542）之前插入一行：

```cpp
safety_outputs_init();   // ★ 必须早于 ble_init()/wifi_init()
```

函数体（`safety.cpp`）：

```cpp
void safety_outputs_init()
{
    // 阀：安全态 = 关闭
    int vp = config_get_valve_gpio_pin();
    if (vp >= 0) {
        pinMode(vp, OUTPUT);
        digitalWrite(vp, config_get_valve_active_level() ? LOW : HIGH);
    }
    // 步进 EN：安全态 = 失能
    int en = config_get_stepper_en_pin();
    if (en >= 0) {
        pinMode(en, OUTPUT);
        digitalWrite(en, config_get_stepper_en_active_level() ? LOW : HIGH);
    }
    // 振动：安全态 = 关
    int rp = config_get_vibration_relay_pin();
    if (rp >= 0) {
        pinMode(rp, OUTPUT);
        digitalWrite(rp, config_get_vibration_active_level() ? LOW : HIGH);
    }
    Serial.printf("[Safety] outputs init: valve=%d en=%d vib=%d\n", vp, en, rp);
}
```

### 风险

| 风险 | 缓解 |
|---|---|
| 若某 pin 与其他外设冲突（如 `en_pin` 误配为 I2C 脚），会**提前**把它拉成输出 | 三个 pin 都来自 config；`*-1` 即 skip；`safety_outputs_init` 只写这 3 个脚 |
| `digitalWrite` 前必须 `pinMode(OUTPUT)`，否则写不进 | 代码已保证顺序 |
| 与 `valve_init()` 重复（后者也 `valve_close()`） | 幂等，重复写同电平无副作用 |
| 错误地在 `config_init()` 之前调用 ⇒ 读到默认值 | 位置硬约束，加注释 + `Serial` 打印实际值便于核对 |

### 测试依赖

- 串口：`[Safety] outputs init:` 行必须出现在 `[WiFi]` 之前
- 电气：万用表测 IN 脚在复位瞬间的电压（**用户执行**）

### 回滚

注释该行 → 行为回到现状（`valve_init` 时才配置）。**保留函数不删**，便于再次启用。

---

## 3. F-3 · `safety_router`（`command_manager.cpp`）

### 改动

1. 一级路由加 `c == "safety"`
2. 新增 `static bool safety_router(const CommandMessage&, JsonDocument&)`
3. 两个对象：`stop_all` / `clear`

```json
{"c":"safety","i":"<cmd_id>","p":{"o":"stop_all","reason":"user_estop"}}
{"c":"safety","i":"<cmd_id>","p":{"o":"clear"}}
```

### 响应语义

| status | 条件 |
|---|---|
| `success` | 三个执行器读回均为安全态 |
| `partial` | 有执行器读回超时/不支持 |
| `failed` | `stop_all` 本身未能执行（如模块未初始化） |

★ **不复用 `accepted`** —— 强停不允许"已受理"这种中间态上报，否则 UI 会被诱导弹成功。

### 注意

- 该命令**不经过** `CommandRuntime` 的异步表（同步完成），避免占用 10 条 FIFO
- `cmd_id` 去重仍由上层 `cloud_manager` 负责（30 s 内重复静默丢弃）
- **`reason` 仅记录，不影响行为**（防止有人以为 `reason=auto` 会跳过动作）

### 测试依赖

见 `11` §6.2 ES-01~ES-10。

### 回滚

删分支（`safety_router` + 一级路由判断）。

---

## 4. F-5 · Flash 写保护闸门（**必须先于 F-4**）

### 为什么必须有

强制重启会在**任意时刻**打断 Flash 写入：

| 写入者 | 被打断的后果 | 现有保护 |
|---|---|---|
| `config_save` | Active 半写 | ✅ Backup + `config_boot_validate()` + `last_boot_ok` 回滚 |
| `workflow_storage` 保存 | 文件半写 | △ 有事务/镜像机制（**强度待验证**） |
| `json_storage` | JSON 损坏 | ❌ **无** |
| log Flash 队列 | 丢尾部记录 | ✅ 可接受 |

⇒ 没有闸门 = "随机损坏配置/JSON"的赌局。

### 改动

```cpp
// system_command.h
bool system_command_flash_write_acquire();   // 返回 false = 不允许写? 否 —— 见下
bool system_command_flash_write_release();
bool system_command_flash_write_busy();
uint32_t system_command_flash_write_count();
```

- 语义与 `critical_operation` **相似但独立计数**（不能合并：强重启要绕过 critical，但不能绕过 flash）
- 三个写入路径在**真正落盘的那一段** acquire/release（不是整个 `config_set` 期间）
- `system_command_force_restart()` 检查 `flash_write_busy()` ⇒ `true` 则拒绝

### 风险

| 风险 | 缓解 |
|---|---|
| 漏掉某个写入路径 ⇒ 闸门形同虚设 | 全仓 grep `File.write` / `fwrite` / `json_storage_save` 后逐一登记 |
| acquire 失败时怎么办 | 写入路径**不因 acquire 失败而放弃写入**（闸门只读不阻塞写入），避免引入新故障 |
| 计数泄漏（acquire 后异常未 release） | 参照 `critical_operation` 现有写法（RAII 风格的手动配对 + 断言日志） |

### 测试依赖

- 串口：`flash_write_busy=1` 期间发 `force_restart` ⇒ 被拒
- 串口：`config_save` 期间 `flash_write_busy` 为 1，结束后为 0

### 回滚

独立 commit，`git revert` 即恢复。**不要**与其他改动混在同一提交。

---

## 5. F-4 · 强制重启（`system_command.cpp`）

### 改动

```cpp
bool system_command_force_restart(const char *reason);
```

行为：

```
1. if (system_command_flash_write_busy()) → 返回 false（拒绝）
2. 记录 event / log（WARN，含未完成的 critical 计数）
3. s_restart_state = RESTARTING
4. 延时 300 ms（非阻塞：用状态机 + 到期时间戳，不 delay()）
5. ESP.restart()
```

- **必须非阻塞**（项目禁 `delay()`）：新增 `RESTART_FORCE_PENDING` 状态 + 到期时间戳
- **必须走 `system_command.cpp` 的既有状态机**，不新增 `ESP.restart()` 调用点
  （MEMORY 铁律："唯一 `ESP.restart()` 在 `system_command.cpp`"）
- 开关宏 `GF_ENABLE_FORCE_RESTART`，**默认 0**，便于现场关闭

### 命令入口

```json
{"c":"system","i":"<id>","p":{"o":"force_restart","reason":"user_confirmed"}}
```

**同一命令内是否顺带执行 `stop_all`？**
→ **否**。编排放在云端（UI 状态机先发 `safety.stop_all` 再发 `force_restart`）。
理由：固件内耦合两个高风险动作会让"只重启不停堆"变成不可能，且违反单一职责。
但**启动期 `safety_outputs_init()`（F-2）必须存在**，它是最终兜底。

### 风险（最高）

| 风险 | 说明 | 缓解 |
|---|---|---|
| 中断 OTA/写入 | 已由 F-5 拦 | F-5 |
| 与 `config` 的 boot_ok 回滚叠加 | 强制重启本身**不产生** `!last_boot_ok`（因为 `config_boot_validate` 未走到 ⇒ 会！） | ★ **必须验证**：强制重启后 `last_boot_ok` 是否为 false。若是，会触发 CFG-ROLLBACK 把 Backup 覆盖 Active（**MEMORY 有踩坑记录**）⇒ 需要在 `safety_outputs_init()` 之后**提前调用** `config_boot_validate()`，或为强制重启写一个"已确认启动"标记 |
| 执行器带电瞬间断电 | 阀可能卡在中途 | `stop_all` 在前；`safety_outputs_init()` 在后 |

★ **第三条是 F-4 的最大隐藏风险**，实施前必须做一次专门实验：
`system.force_restart` → 重启 → `config_query` 对比是否被回滚。

### 测试依赖

见 `11` §6.3 FR-01~FR-06。

### 回滚

宏置 0（命令返回"不支持"）。

---

## 6. F-6 / F-7 · `boot_seq`

### 为什么需要

`11` §4.4 的"重启后识别设备实例"、`13` §3.5 的"启动标识变化时失效"，
都依赖一个**重启会变**的标识。现状：registry 的 `version`/`checksum` 重启不变，
presence 只有 `online/offline`。

### 改动（最小）

| 项 | 位置 |
|---|---|
| 新增 `STATE_BOOT_SEQ`（int） | `system_state.h` 枚举 + `system_state.cpp` 的 `state_map[]` |
| 启动时自增并持久化 | 复用 `version.json` 或 `device_identity`，**一次 Flash 写** |
| `query_state o=boot_seq` 可读 | 自动（走通用 state 查询） |

### 风险

| 风险 | 缓解 |
|---|---|
| 每次启动写 Flash ⇒ 磨损 | 一天重启 3 次 × 10 年 ≈ 1 万次写，远低于寿命 |
| 写失败导致 boot_seq 不递增 | 退化为 RAM 内自增（重启归零）⇒ 也可用，只是跨重启不唯一；记录 WARN |
| 与 CFG-ROLLBACK 交互 | `version.json` 不在 config 的 Active/Backup 体系内 ⇒ 不受回滚影响（**待验证**）|

### 测试依赖

串口：重启两次，`boot_seq` 递增；`config_save` 触发的重启也递增。

### 回滚

删枚举项 + 撤写入（保留读，返回 0）。UI 侧按"不支持"降级。

---

## 7. F-8 · 阀安全超时默认值

| 改点 | 现在 | 改为 |
|---|---|---|
| `config_manager.cpp:3601` | `["safety_timeout_sec"] | 300` | `| 60` |
| `valve.cpp:252-253` | `< 1 → 1` / `> 300 → 300` | `< 10 → 10` / `> 300 → 300` |
| `config_manager.h:602` 注释 | "合法值 10~300"（与代码 1~300 矛盾） | 改代码使其与注释一致 |

### 关键提醒

1. **改默认值只影响"从未写过该键"的模块**。设备实值当前是 **300**
   ⇒ 改完代码后设备仍是 300，**必须**通过 `config_set` + `config_save` 显式改。
2. **不要把 `open_duration_ms` 当成第二个开关** —— 它是死参数（无消费者）。
3. **`workflow` 超时（360 s）不关阀**，所以 60 s 的 M1 会先截断 21 g 供水（实测需 45 s）。
   ⇒ 60 s 对当前流程够用（45 s < 60 s），但**余量只有 15 s**。若水温低/水压小时长会变，
   建议 UI 在 `safety_timeout_sec < 该流程历史最长用时 × 1.5` 时给红警。

### 测试依赖

| 测试 | 层 |
|---|---|
| 改 60 后跑 `wf_valve_20g`，实测 `VALVE_OPEN_MS` ≈ 45 s 且正常结束 | 真机 |
| 把 60 改成 20 再跑，验证 M1 在 20 s 截断并产生 `LOG_P_LIMIT_MS` 事件 | 真机（这是**验证保护真的生效**的正面用例） |
| 把范围下限改 10 后，`config_set safety=5` 被钳到 10 | 串口 |

### 回滚

改回 300 / 1。**设备实值不随之改变**，需另行 `config_set`。

---

## 8. F-9 · 普通重启遇到 Critical Operation 的行为

### 现状 vs 需求

| | 现状 | 需求 |
|---|---|---|
| 有 Critical Operation 时 `system.restart` | **无限等待**（不拒绝、不告知剩余） | **拒绝**并说明原因 |

### 两个方案

| 方案 | 做法 | 优点 | 缺点 |
|---|---|---|---|
| **A（推荐）** | 新增 `p.precheck=true`：先查 `critical_operations`，>0 直接回 `e=<新码> "critical operation in progress (n)"`，**不改变默认行为** | 向后兼容；现有 `config_save` 触发的重启路径不受影响 | UI 需多一条命令 |
| B | 改 `system.restart` 默认拒绝 | 语义更符合需求 | **会打断 `config_save` → 自动重启的既有链路**（critical 未归零时必然拒绝 ⇒ 配置改完不重启） |

⇒ **推荐 A**。理由：`config_save` 的"改完自动重启"依赖"等待 drain"，
把默认改成"拒绝"会把这条链路弄坏（`config_manager.cpp:3357` 的 `CONFIG_CMD_RESTART` 走的就是它）。

### 改动（方案 A）

- `system.restart` 支持 `p.precheck`（bool，默认 false）
- `precheck=true` 且 `critical_operations > 0` ⇒ `command_send_error(cmd, CMD_ERROR_SYSTEM, "critical operation in progress (n)")`
- UI 的「普通重启」按钮固定带 `precheck=true`

### 测试

- 无 critical 时 `precheck=true` → 正常重启
- 有 critical（如正在 `config_save`）时 → 明确错误 + 计数
- `config_save` 自身触发的重启 → 行为不变（仍是等待）

### 回滚

忽略 `p.precheck` 参数即回到现状。

---

## 9. 实施顺序建议（依赖安全）

```
① F-8（独立、低风险、可先做）      ← 只改默认值 + 钳位，无新逻辑
② F-5（Flash 闸门）               ← 必须先于 F-4
③ F-1（safety.cpp 骨架）
④ F-2（启动安全电平）             ← 需 F-1；需串口验证顺序 + 电气测量
⑤ F-6/F-7（boot_seq）             ← 独立；为 UI 提供实例识别
⑥ F-3（safety_router）            ← 需 F-1；强停在此可用
⑦ F-9（重启 precheck 方案 A）      ← 独立
⑧ F-4（强制重启）                 ← 需 F-5 已合入并验证
```

每项**单独 commit**，代码与文档分离（项目提交纪律）。
每项合并后**必须**跑：`pio run` + 构建后整体余量检查（G-1 Flash <85% / G-2 RAM 不变）。

---

## 10. 明确**不做**的固件改动

| 想法 | 为什么不做 |
|---|---|
| 给 `WorkflowActionDescriptor` 加 `stop` 回调 | 3 个模块 + 14 个 Action 全要改；现有物理停止函数已够用；改动面与收益不成比例 |
| 把 `stop_all` 做成 Action | 会重排全部 stable_id ⇒ 云端缓存全失效 |
| 在 `workflow_stop()` 里直接关阀 | 违反分层（Workflow 不该知道阀的存在）；且会让"停 workflow 顺带停一切"变得不可预期 |
| 删掉 `open_duration_ms` | 属于 schema 变更，可能影响未知消费者；本轮只标注"预留" |
| 为 UI 加"按克直接供水"命令 | 用户明确说不新增；流程是"改 Workflow → 运行" |
