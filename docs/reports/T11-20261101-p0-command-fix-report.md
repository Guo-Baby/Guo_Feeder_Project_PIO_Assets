# T11 修复报告：MQTT 命令队列阻塞 / 重复执行 / 插座误通电

- 生成时间：2026-11-01 22:05 (GMT+8)
- 任务级别：**P0 / 严重安全故障**
- 执行边界遵守情况：**未刷写、未部署、未 commit/push、未执行任何物理动作、未改 EMQX 生产配置、未改 D1**（任务书全部红线遵守）。
- 权威台账：`D:\Guo_Feeder_Project\P4-20261101-execution-ledger.md` §T11（本报告是其摘要视图，两者数据一致）。

---

## 1. 根因结论

三个现象对应 **四条相互独立的根因**（先分型、后修复）：

| 现象 | 根因 | 结论 |
|---|---|---|
| A 确认弹窗点不出来（刷新才恢复） | **R1 纯前端** | 与 MQTT / 固件无关 |
| B 断电命令确认后设备不执行、约 1 min 超时、之后普通重启也不灵 | **R2 固件（下行吞吐 + 发送背压）** | 与 A 无关 |
| C 强制重启前插座被再次通电、重启后保持通电 | **R3 固件（重启窗口无门禁 + 安全停堆不覆盖米家）** | 与 A、B 均独立 |
| B 的一个子现象：断电失败却显示成功 | **R4 固件（结果关联错位）** | 独立小缺陷 |

### R1 · 前端两层 busy 语义矛盾（现象 A）
- 按钮层：`if (devBusy("plug") && !isOff) return;` —— **断电被豁免**；
- 下发层 `runXiaomiAction()`：`if (devKey && busyByDev[devKey]) return;` —— **无条件拦截且静默**（连 toast 都没有）。
- ⇒ 点断电时被第二层吞掉。busy 窗口最长 `POLL_TOTAL_MS` = **75 s**，期间按钮永远无反应；刷新重置 `ctrlUI` 才恢复。与"确认弹窗单实例"无关（弹窗本身每次调用都重建 DOM）。

### R2 · 下行环"每轮 1 条"+ 环满静默丢弃 + outbox 无背压（现象 B）
- `cloud_process_rx_queue()` 旧实现**每轮只出 1 条**，接收环仅 4 槽 ⇒ 云端突发数条（或断线重连成批到达）时，**第 5 条起静默丢弃**。esp-mqtt 已先回 PUBACK ⇒ **broker 不会重投，命令永久丢失**。这正是"确认了、设备没执行、一分钟超时"。
- 日志批与命令结果**共用同一 outbox**、同为 `qos1/store=true`、FIFO ⇒ 结果帧排在日志批之后；真机实测 outbox 0→**5997 B** 期间**零 `publish FAIL`** ⇒ 本 IDF（4.4）无 `outbox_limit`，outbox 只进不出；`network_timeout_ms` 默认 10 s，mqtt 任务持 API 锁做阻塞写 ⇒ 主循环被拖长，串口命令随之超时。

### R3 · 重启窗口无任何门禁 + `safety_stop_all()` 完全不覆盖米家（现象 C）
- grep 全仓：`restart_state` 门禁**一处都没有**（唯一命中是一句注释）。`RESTART_PENDING` 10 000 ms、`RESTART_FORCE_PENDING` 300 ms 期间，主循环照常 `cloud_process_rx_queue()` → `command_manager_execute()` → `workflow_task()` ⇒ 排队/重投的旧命令在窗口内被执行。
- `safety_stop_all()` 只停本地执行器（阀/步进/振动/测试步进），**完全没碰米家**；米家请求两阶段（握手 800 ms → 发命令 → 等回包 3000 ms），排队中的那条会在窗口内真的发出；插座是**外部设备**，主机重启断不掉它 ⇒ "重启后一直通电"。用户观察到的"约 300 ms"与 `RESTART_FORCE_PENDING` 的 300 ms 窗口吻合。

### R4 · 结果关联错位（断电失败显示成功）
- `xiaomi_lan.cpp::actionPoll()` 按 `descriptor` 认领结果（`r.owner == desc && r.fin`），而 `fin/ok` 是**槽级**状态、`finishRequest()` 后不清 ⇒ 本次 `startRequest` 早退失败（BUSY/未配置）时，poll 用**上一轮的 SUCCESS** 覆盖本次 ACTION_FAILED。

## 2. Outbox 结论（约 5997 B 与本次故障的关系）

**有关，且是现象 B 的重要一环，已在本任务内修复**（不是记录为未来优化项）：

1. **因果链**：日志批与结果帧共用 outbox 且同优先级 ⇒ 积压时结果帧排不上队 ⇒ 前端 75 s 超时；mqtt 任务持锁阻塞写 ⇒ 主循环被拖长 ⇒ 串口命令连带超时。
2. **修复**（见 §3）：
   - 日志批加**高水位让路**（outbox 超过 4096 B 时本轮不发日志，LogManager 稍后重发，**不丢数据**）；
   - **命令结果路径零门禁**（结果不可延后）；
   - 下行环**每轮出 4 条**（见下），从源头减少积压窗口。
3. **刻意不做**：不扩容 outbox（队列容量不是处理能力）；不放宽 `network_timeout_ms`（掩盖问题）。
4. **对"5997 B 是否单独导致整个故障"的回答**：它解释了"结果帧慢、主循环卡"，但**不能单独解释**"断电命令设备根本没收到"——那需要 R2 的下行环丢弃。两者共同构成现象 B。

## 3. 修复内容

### 固件（PIO_Assets/Guo_Feeder_Project，9 文件 +532/−10）
| 文件 | 改动 |
|---|---|
| `src/cloud/cloud_manager.cpp` | ① 下行环每轮最多出 4 条（`MQTT_RX_DRAIN_PER_LOOP`，与槽数 4 解耦）；② 环满/非法长度**计数 + 5 s 节流上报**（`s_rx_drop_count`/`s_rx_bad_len`）；③ `cloud_send_log()` 加高水位让路（`CLOUD_OUTBOX_HIGH_WATER_BYTES=4096`）；④ 结果路径 `cloud_send_up()` 不加任何门禁 |
| `src/services/command_manager.cpp` | ⑤ 重启窗口门禁：`system_command_is_restarting()` 时拒 `execute_*`/`workflow.*` 两族，**放行 safety / query / 停止类动作**（刻意不整族封杀 `system`，保住 restart_status 与诊断）；⑥ **已完成 cmd_id 记忆环**（16 槽 / TTL 5 min）：异步命令重复投递回 `status:"duplicate"` 且**不再执行**；⑦ 超时/完成一律释放槽位 |
| `src/services/system_command.{h,cpp}` | ⑧ 新增 `system_command_is_restarting()`（`state != RESTART_IDLE`，覆盖 FORCE_PENDING） |
| `src/app/safety.{h,cpp}` | ⑨ 锁存白名单补 **`XIAOMI_PLUG_OFF`**（旧白名单全是本地执行器，锁存期间连断电都被拒）；⑩ 抽出 `safety_action_is_stop_class()`，"锁存白名单"与"重启门禁"**共用一份数据**；⑪ `safety_stop_all()` 开头调用 `xiaomi_lan_abort_noncritical()`；⑫ `SafetyReport` 增加 `xlan_cancelled` 字段并在结果帧上报 |
| `src/app/xiaomi_lan.{h,cpp}` | ⑬ 新增 `xiaomi_lan_abort_noncritical()`：作废在途的**非安全**米家请求（`REQ_PLUG_OFF` 保留——断电是安全方向）；⑭ `startRequest()` 槽空闲时**先清 `fin/ok` 并认领 owner**（位置在所有早退之前）——修 R4 |
| `src/automation/workflow.cpp` | ⑮ `workflow_action_start()` 同判据重启门禁（挡住"倒计时里推进下一步"） |

### 前端（Cloudflare_Assets，`gf-xiaomi.js` +118/−23）
| 改动 | 说明 |
|---|---|
| busy 收口为 `devBusy()/setDevBusy()/devBusyLeftSec()` | 唯一读写入口，杜绝两层语义矛盾 |
| **安全动作豁免**：`XIAOMI_PLUG_OFF` 不看 busy | 断电永不被普通命令阻塞（与固件白名单同语义） |
| 被挡必有 toast（含剩余秒数） | 消灭静默丢弃 |
| `BUSY_MAX_MS = 90 s` 泄漏自愈 | 超时自动释放，**刷新不再是恢复的必要条件** |
| 删除按钮层两处静默早退 | busy 只在一个地方判定 |

### 明确不做（对应任务书 §四）
未重写 MQTT 架构 / CommandManager / WorkflowManager；未引入 RTOS 动态任务或大栈对象；未新增 OTA/EventId 体系；未改 D1 migration。

## 4. 测试结果（逐项）

| 套件 | 组 | 结果 | 说明 |
|---|---|---|---|
| `tests/ui/p0_t11_ui_command_lifecycle.test.mjs`（**新增**） | A | **33 / 0** | 断电豁免 busy、失败/超时后可重发、连续点击不重复、busy 90 s 自愈、无静默早退、弹窗每次重建 |
| `test/p0_t11_command_queue_and_safety_test.py`（**新增**） | B+C | **54 / 0** | 含 **6 条负向探针**（删掉门禁/去重/清槽逻辑后必须报错）；覆盖命令入队出队、队列满有明确结果、超时不复活、QoS1 重投不重复执行、断线重连不重放、失败不阻塞后续、断电可执行、强制重启取消在途通电、断电不被旧命令阻塞 |
| `test/p0_t11_outbox_backpressure_test.py`（**新增**） | D | **30 / 0** | 含 **3 条负向探针**；覆盖正常/接近上限/满载、断网恢复、满载期间主循环仍转、发送失败不重发非幂等动作 |
| `test/p4_wire_envelope_contract_test.py`（回归） | — | 8 / 0 | T05 契约守卫未破坏 |
| Cloudflare `tests/{ui,api,pages}` 全量 47 套 | — | **全部 rc=0** | 排除 `ui_responsive_n`（既有环境问题，见下） |
| `tests/pages/xiaomi_ui.test.mjs` | — | 196 / 0 | 3 条断言契约漂移 → 精确化为更强契约（非掩盖） |
| `tests/ui/p4_config_sync_layout.test.mjs` | — | 56 / 0 | 同上 |

**说明**：
- `tests/pages/ui_responsive_n.mjs` 失败：**用 HEAD 基线版本对照同样失败/挂住** ⇒ 既有环境问题（Playwright + 本地 server），与 T11 无关，登记不修。
- 两处契约漂移按"先分型再定性"处理：旧断言盯着按钮层两行原文，T11 恰好删除了这两行（消灭静默早退）⇒ 改为钉**更强契约**（按设备 busy + 安全豁免 + 无静默早退）。
- 全部为**确定性模拟/契约测试**，无"伪造设备执行成功"。

## 5. 固件编译

| 项 | 值 |
|---|---|
| 环境 | `esp32-s3-devkitc-1`（生产 env）· PlatformIO 6.2.0 · Arduino 2.0.17（IDF 4.4） |
| 结果 | **SUCCESS**（31.8 s） |
| 产物 | **1,473,600 B** · SHA-256 **`eed32fd8702e6e2bd59ae218c330ed8e637fd871548da67048243c6b8068c2ec`** |
| 对比在跑固件 | `4c100537…` / 1,471,696 B ⇒ **+1,904 B** |
| RAM | **42.0%**（137,728 B，仅 +296 B）· Flash 70.2% |
| 已回退方案 | 接收环 4→8 槽（+32 KB 常驻 RAM，RAM 42%→52%）——**收益配不上成本，已回退**，改为"每轮多出 + 可观测" |
| 状态 | **仅编译通过，未刷写** |

## 6. 安全审查

- **强制重启能否阻止旧通电命令继续执行**：
  - 入口门禁：`RESTART_PENDING`（10 s）与 `RESTART_FORCE_PENDING`（300 ms）窗口内，`execute_*`/`workflow.*` 被固件层拒绝并回错误帧（**不静默**）；`workflow_action_start()` 同判据兜底；
  - `safety_stop_all()` 在物理停止**之前**先 `xiaomi_lan_abort_noncritical()` 作废在途的非安全米家请求（断电除外）；结果帧可观测（`xlan_cancelled` 计数）；
  - 后果：重启窗口内"插座通电"要么被拒、要么其 in-flight 请求被作废 ⇒ **不会产生新的通电动作**。
- **断电的安全优先级**：三层全部放行——① 前端 busy 豁免；② 固件重启门禁放行停止类；③ 锁存白名单补 `XIAOMI_PLUG_OFF`；且 `xiaomi_lan_abort_noncritical()` **保留** `REQ_PLUG_OFF` 在途请求。断电不会被普通队列旧命令阻塞。
- **职责边界**：CommandManager 只做门禁与去重（入口），WorkflowManager 只在 action 启动处加同判据（推进），safety 只做停止编排——未越界重写。
- **"已发送 ≠ 已执行"**：重复投递命中记忆环时回 `status:"duplicate"`，措辞明确"可能此前已失败，请换新 cmd_id 重发"；UI 的"未知"仍按未知渲染。

## 7. 人工验收步骤（用户本人执行，最少 6 项；刷写仍用 `4c100537` 时代的流程：**只刷 0x10000，绝不 `uploadfs`**）

> 新固件：`.pio/build/t11/esp32-s3-devkitc-1/firmware.bin`（`eed32fd8…` / 1,473,600 B）

1. **现象 A 复验**：通电 → 立刻点「插座断电」⇒ 弹窗**必须**出现（不需要刷新）。
2. **busy 自愈**：让一条命令走到超时（约 75 s）后再点任意插座动作 ⇒ **必须有 toast 说明**（含剩余秒数），不得无反应。
3. **重复投递**：连续点两次「插座通电」⇒ 只应产生**一次**物理动作（第二次回 duplicate 或被 busy 拦）。
4. **重启门禁**：点「普通重启」→ 在倒计时窗口内点「插座通电」⇒ 应被拒（`system is restarting; only safety / stop-class commands are accepted`）。
5. **强制重启安全**：点「强制重启」⇒ 重启后插座**不得**处于通电态；若结果里 `xlan_cancelled > 0`，说明确实拦下了一条在途米家请求。
6. **R4 复验**：点「插座断电」⇒ 真断电；若失败，UI 必须显示**失败**（不得再显示成功）。

## 8. 未解决问题（仅本任务直接相关且确实无法解决）

- **真机验证未做**：任务书禁止刷写与物理动作 ⇒ R1–R4 的修复全部处于"代码已修复 / 自动化测试通过 / 固件编译通过 / **真机尚未验证**"状态，需按 §7 由用户验证后才可写"用户人工验证通过"。
- `ui_responsive_n` 套件在 HEAD 基线同样失败/挂住：既有环境问题，非本任务范围，登记不修。
- IDF 4.4 的 outbox 无容量上限属平台约束：本次用"高水位让路 + 结果路径零门禁"从**使用侧**消除积压后果，未能（也不应）在库层面加上限。

---

### 状态汇总（严格区分）

| 项 | 状态 |
|---|---|
| R1 前端 busy 生命周期 | **代码已修复 · 自动化测试通过（33/0）** · 真机未验证 |
| R2 下行吞吐 + outbox 背压 | **代码已修复 · 测试通过（30/0）** · 真机未验证 |
| R3 重启门禁 + 安全停堆覆盖米家 | **代码已修复 · 测试通过（54/0）** · 真机未验证 |
| R4 结果关联错位 | **代码已修复 · 测试通过** · 真机未验证 |
| 固件编译 | **通过**（`eed32fd8…` / 1,473,600 B） |
| 部署 / 刷写 / commit | **未执行**（任务书禁止，等待用户审阅） |
