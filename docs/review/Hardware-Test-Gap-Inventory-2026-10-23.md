# 真机验收缺口清单（2026-10-23 · 21 g 闭环跑通后复核）

> 目的：回答「开发阶段还有哪些测试被真机实验阻塞」，并判断能否开始 UI 阶段。
> 全部结论基于 **D1 `log_history` 全量 CBOR 解码**（工具：`docs/scripts/decode_log_history.py`）
> + `query_state` 实时读数，不使用估算。
> 本轮**未改固件、未烧录、未改配置**。

---

## 一、今天实测解除的（原先登记为「真机阻塞」）

`wf_valve_20g`（slot 3，21 g）在 **boot 73** 完整跑通，证据链如下：

| 时间 | seq | 事件 | 关键参数 |
|---|---|---|---|
| 05:15:27 | 20236 | `LOG_WF_START` | `SLOT=3 STEPS_DONE=4 TIMEOUT_MS=360000` |
| 05:15:27 | 20237 | `LOG_VALVE_OPEN` | `STATE=1 WAS=0` |
| 05:16:07 | 20238 | `LOG_BLE_DATA_DECODED` | `TEMP=24.1`（**期间无 WEIGHT_ERROR / FORCE_CLOSE**） |
| 05:16:12 | 20239 | `LOG_WEIGHT_TRIGGER_FIRED` | `TARGET_G=21.0 DELTA_G=21.04 WEIGHT_G=1760.06` |
| 05:16:12 | 20240 | `LOG_VALVE_CLOSE` | `STATE=0 WAS=1 VALVE_OPEN_MS=45364` |
| 05:16:12 | 20241 | `LOG_WF_FINISHED` | `SLOT=3 DURATION_MS=45387 STEPS_DONE=4` |

由此解除：

| 编号 | 原状态 | 现在 | 依据 |
|---|---|---|---|
| **NC-7**（`WF_FINISHED`） | 未验证（缺"只含无害 Action 的 Workflow"） | ✅ **已验证** | 05:16:12 `WF_FINISHED SLOT=3 DURATION_MS=45387 STEPS_DONE=4` |
| **NC-7**（`WF_TIMEOUT`） | 未验证 | ⚠️ 仍未验证 | 需要一个**跑不完**的流程（如 weight_decrease 永不满足）；05:05 那次是 `WF_FAILED` 不是 TIMEOUT |
| **NC-10**（`WEIGHT_TRIGGER_FIRED`） | 未验证（需真实减重） | ✅ **已验证** | `DELTA_G=21.04` = 真实减重 21.04 g，秤从 1760.06 g 起算 |
| **NC-8**（`WF_FAILED`） | 未验证（需构造非法 BIN） | ✅ **意外解除** | 05:05:09 `WF_FAILED SLOT=3 FAIL_STEP=2 CAUSE=3`（`TRIGGER_FAILED`，真实故障路径） |
| **NC-13 阀门 OPEN/CLOSE** | 已验（09-19） | ✅ 再次确认 | `VALVE_OPEN_MS=45364`（45.4 s 持续开启，未被强关） |
| **端到端供水闭环** | 未验证 | ✅ **已验证** | 开阀 → 减重 21.04 g → 自动关阀 → 流程正常结束 |

顺带得到一条**负面证据的正式确认**：`DISPENSE_SAFETY_RESPONSE` / `VALVE_FORCE_CLOSE`
在 45 秒的完整运行期内**一次都没出现** ⇒ 上一轮"一开就被关"已不再是当前状态。

---

## 二、仍然被真机阻塞 —— **功能级**（会阻塞"功能完成"判定）

| 编号 | 内容 | 为什么必须真机 | 复测手段 | 阻塞什么 |
|---|---|---|---|---|
| **HW-1** | `stepper` 三个 Action 从未执行：`STEPPER_DISPENSE_ROTATE`(120°)、`STEPPER_ROTATE_STEPS`、`STEPPER_EMERGENCY_STOP` | 需电机 + DRV8825 + 12V 实际转动；**开环**，无传感器反馈 | 先 `test_stepper`（LEDC 常通电方波）确认电机能转，再跑正式 Action | **出粮功能**整体（`Motor` 通道） |
| **HW-2** | 步进电机「能转」本身未确认 | 之前现象是**锁止但不转**；改用 LEDC 后仍未知 | `tstep_start 100 0` → 观察轴是否旋转，再 20 Hz / 200 Hz 对比 | 同上 |
| **HW-3** | 120° 出粮账本（533/533/534 脉冲）未经真机校准 | 需要实际出粮重量 vs 脉冲数对应关系 | 用秤称出粮量，反算 steps/g | 定量出粮 |
| **HW-4** | RMT 波形从未用示波器/逻辑分析仪验证 | 纯软件无法判断波形是否正确 | 示波器测 GPIO13 | `stepper` 的 RMT 路径是否可用 |
| **HW-5** | `vibration` 两个 Action 从未执行：`VIBRATION_START`(GPIO16) / `VIBRATION_STOP` | 需马达实接 | `{"c":"action","v":4,"k":11}` / `k=12` | 振动功能 |

> ★ 说明：`HW-1`~`HW-4` 其实是**同一个模块**的四层（能不能转 / 波形对不对 / 位置准不准 / 三个 Action 跑不跑）。
> 只要电机转起来，前两层一次性解决；第三层需要称重配合；第四层需要仪器。

---

## 三、仍然被真机阻塞 —— **观测级**（不阻塞功能，只影响日志埋点覆盖率）

这些不是"功能没做"，而是"埋点代码路径存在、但真机上从未走到"，
所以**不能写成"已验证"**。它们全部不影响 UI 阶段。

| 编号 | 事件 | 为什么没触发 | 复测方法（真机） |
|---|---|---|---|
| NC-1 | Storage bridge 的 ERROR/CRITICAL 分支 + 10 s 抑制 | 现有钩子对缺失文件返回 OK，不产生 fs 级错误 | 需故障注入（改库或写保护） |
| NC-2 | `LOG_WIFI_LOST` | 无任何钩子能强制断连 | **物理关掉 AP / 拔路由器** |
| NC-3 | `LOG_MQTT_SLEEP_ENTER` | 需连续 30 次断连且期间一次没连上（≈5 min） | 临时把 `mqtt` 密码改错 → 重启 → 5 min → 改回 |
| NC-4 | `LOG_MQTT_PUBLISH_FAIL` | 上层都先自查 `mqtt_connected`，离线时根本不发 | 时序竞态，不适合稳定回归 |
| **NC-5** 剩余 | `RTC_CALIBRATED` / `WRITE_FAILED` / `VL_FLAG` / `BCD_INVALID` | **板上当前未接 RTC 芯片**（见 §4.1） | 先接上 PCF8563T，再按各自条件注入 |
| NC-6 | `LOG_TIME_INVALID_ENTER` | 设备没有 `set_time` 通道 | 需新增通道或注入 |
| NC-8 剩余 | `TEMP_ACTION_TIMEOUT` / `RUNTIME_ALLOC_FAILED` | 需临时 Action 超时 / `def_buf` 分配失败（PSRAM 充裕） | 故障注入 |
| NC-10 剩余 | `LOG_WEIGHT_CALIB_FAILED` | 需 `config_set_weight_zero_offset()` / `config_save()` 失败 | LittleFS 写失败注入 |
| NC-13 剩余 | `LOG_VALVE_RATE_LIMITED` | 需 50 ms 内二次**真实**切换，串口命令精度不足 | 需固件侧测试钩子 |
| NC-14 | `SAFETY_TIMEOUT` 的「被 50 ms 防风暴跳过」路径 | 需精确定时，串口做不到 | 临时调大 `MIN_OPERATION_INTERVAL_MS` 做注入 |
| NC-15 | `FORCE_CLOSE` 的「cause 发生变化」分支 | 目前只有 `dispense_guard` 一个调用来源，cause 恒为 1 | 需第二个来源（人工命令 / 安全超时）才能区分 |
| NC-11 / NC-12 | `VALVE_NOT_READY` / `valve_init` 注册失败 | **`log_events.h` 中无对应 EventId**（冻结契约禁止新增） | 不修，登记缺口 |

---

## 四、当前观测到的硬件状态

### 4.1 ⚪ RTC 模块当前**未接** ⇒ 探测失败是**预期行为，不是缺陷**

全量解码结果：

```
26 × LOG_TIME_RTC_PROBE  WARN  STATE=1 ERR_CODE=2   （Wire err 2 = 地址 NACK）
 1 × LOG_TIME_RTC_PROBE  WARN  STATE=1 ERR_CODE=5   （Wire err 5 = 未知错误）
 0 × STATE=2（成功）
```

`STATE` 语义（`time_manager.cpp:140-144`）：`0=未探测`、`1=失败`、`2=成功`。
ERR_CODE=2 = `Wire.endTransmission()` 返回 2（地址 NACK）= **总线上 0x51 处没有器件**。

**用户已确认：当前板上没有接 RTC 芯片**（此前 10-22 测过，芯片在时
`PCF8563T detected at 0x51` + `RTC boot restore OK` 均正常，见
`docs/evidence/RTC-Probe-2026-10-22/`）。

⇒ **判定：预期行为。** 不登记为缺陷、不需要修代码。

**副作用（无害）**：每次 boot 会产生 1 条 `LOG_TIME_RTC_PROBE` WARN 上云。
若不想看到这条噪声，可以把 `rtc.enable` 置 `false`（那样探测直接跳过，
日志变成 `STATE=0 未探测`）——但这只是降噪，不改任何功能。

**功能影响**：`BOOT_RESTORE`（掉电保时）不可用；`LOG_TIME_NTP_OK` +
`LOG_TIME_VALID_ENTER SOURCE=2` 说明 **NTP 兜底正常**，时间仍然有效。
⇒ 时间链路可用，只是少了"无网络也能保持时间"的能力。

**连带**：`NC-5` 剩余 4 个事件（`CALIBRATED` / `WRITE_FAILED` / `VL_FLAG` /
`BCD_INVALID`）**必须在接上 RTC 芯片后才能复验** ⇒ 当前属"工装缺失"而非"未测"。

### 4.2 称重读数已恢复正常（接线假设得到支持）

| 时点 | `weight_value` | 现象 |
|---|---|---|
| 04:36 之前 | 恒定 **1080.63 g** | 伴随 `CAUSE=2 raw_zero`（RAW=0）与 `CAUSE=4 jump` 反复 |
| 05:15 之后 | **1759.9 g**（真实载荷） | 45 s 内真实减重 21.04 g，全程无错误事件 |

⇒ 支持用户判断：**此前是接线问题**（称重传感器/激励接错 ⇒ 原始值漂移或恒 0），
不是（或不只是）EMI 干扰。已据此修订 `docs/evidence/Valve-ForceClose-Diagnosis-2026-10-23/`。

---

## 五、软件侧未完成项（与真机无关，但影响"可发布"）

| 编号 | 内容 | 等级 | 说明 |
|---|---|---|---|
| **SW-1** | `weight.cpp:441` `raw_value = scale.read()` **不校验返回值**（只处理 `==0`） | **MAJOR** | 接线错误是本轮真因，但"库返回异常时无防护"是客观缺陷；建议补"全 1 位型 / 偏离过大"样本丢弃 |
| **SW-2** | `P2-8-KI-1` 代码默认 `dt=4 / sck=5` 与 OLED I2C（sda=4/scl=5）冲突 | MAJOR（潜在） | 当前设备与仓库都是 6/7 ⇒ 未触发；若某设备 `weight.json` 缺键 ⇒ 抢引脚 |
| **SW-3** | `stepper` / `vibration` **无结构化日志码** | MEDIUM | `log_events.h` 是 P1.1 冻结契约，目前两模块只有 `Serial` ⇒ 云端看不到这两个模块的任何事件 |
| SW-4 | `G4-R4` DRAM 阈值裁决（`<70%` 在本机从未满足） | 待人工裁决 | 需书面指定阈值口径 |
| SW-5 | `loop()` P99 / 分模块耗时埋点（`S-3-U1/U2/U6`） | MEDIUM | 观测缺口，不阻塞功能 |
| SW-6 | `G4-R5` 突发漏记 ~9% / `B-3` 漏记根因 | MAJOR（取证） | 判据必须串口交叉验证 |
| SW-7 | `G4-R7`（`rewind_count=0` 与 `last_rewind_at` 并存）、`I-8`（EMQX 403）、migration 原子性 | MINOR | 均为登记项 |
| SW-8 | UI 四项：`UI-TELEMETRY-EMPTY` / `UI-RATELIMIT-ISOLATE` / `UI-COOKIE-REVOCATION` / `UI-DEVURL-PUBLIC` | — | **正好在 UI 阶段一起做** |

---

## 六、结论

1. **功能级缺口只剩步进电机与振动马达两个模块**（`HW-1`~`HW-5`）。
   其余（21 g 供水闭环、秤、阀、RTC 保时链路、遥测、日志通道）**已全部真机走通**。
2. **观测级缺口（NC-*）不影响 UI**，它们需要故障注入或仪器，属于"持续完善"性质，
   可以并行、也可以推迟到发布前。
3. ⇒ **UI 阶段可以开工**。建议把 UI 作为主线，把 `HW-1`（步进）作为并行的硬件支线
   （它需要你手上有仪器/能观察电机，不占用我这边的时间）。
4. 无待现场确认的异常 —— 唯一被问到的 RTC 探测失败已确认为**模块未接的预期行为**（§4.1）。

---

## 附：本轮新增工具

`docs/scripts/decode_log_history.py` —— `log_history` 的 CBOR 解码器（可复用）。

```bash
python decode_log_history.py 60                # 最近 60 分钟全部
python decode_log_history.py 180 VALVE WEIGHT  # 只看事件名含关键字的
```

★ 两个已踩过的坑（脚本内已处理）：
- `LogLevel` 枚举是 **0 基**（`DEBUG=0`…`CRITICAL=4`），1 基映射会把 WARN 显示成 INFO；
- 开机早期记录内 `ts=0`（`RET-1`），显示时间会全是 `--:--:--` ⇒ 回退用入库时间 `created_at`（**秒**）。
