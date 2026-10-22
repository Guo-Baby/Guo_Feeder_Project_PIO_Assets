# 开阀 → 减重 20 g → 关阀：Workflow 下发与接线判定

- 日期：2026-10-23
- 设备：`288485896ce4`
- 固件：`463765f`（+ `d2e1aa5` 文档）
- 结论：Workflow 已创建 → 落盘 → 重启 → **重启后完整保留**（已实测）

---

## 一、Workflow 定义（设备实测回读，slot=3）

```json
{
  "id": "wf_valve_20g",
  "name": "开阀-减重20g-关阀",
  "variant": 1,
  "enable": true,
  "timeout_ms": 360000,
  "steps": [
    { "type": "action",  "id": "VALVE_CLOSE" },
    { "type": "action",  "id": "VALVE_OPEN" },
    { "type": "trigger", "id": "weight_decrease", "params": { "gram": 20 } },
    { "type": "action",  "id": "VALVE_CLOSE" }
  ]
}
```

### 1.1 为什么第 0 步用 `VALVE_CLOSE`

需求是「第 0 步随便一个 action，使流程只能手动触发」。要让这个成立，机制上有两道闸门：

| 闸门 | 代码位置 | 条件 |
|---|---|---|
| 事件自动启动 | `workflow.cpp:4717-4734` `workflow_event_callback()` | `steps[0].type == WORKFLOW_STEP_TRIGGER` **且** `trigger->id` 以 `event_` 开头 |
| 轮询执行 | `workflow.cpp:4215-4221` `workflow_task()` | 只处理 `state == RUNNING`，不主动启动 |

⇒ 第 0 步是 **action** 时，没有任何代码会去"监视"它，流程只能由外部命令启动。**需求成立。**

为什么选 `VALVE_CLOSE` 而不是别的 action：

| 候选 | 否决原因 |
|---|---|
| `MQTT_TEST` | 要 `length` 参数，且会发 MQTT 报文，污染排障期的流量判据 |
| `WEIGHT_ZERO` | 会把秤零点重置 ⇒ 破坏 `weight_decrease` 的基线，绝对不行 |
| `COMPUTER_RESET` | 直接重启设备 |
| `VALVE_CLOSE` | **参数为 0、瞬时成功、幂等**（阀本来就是关的），且"关"是安全方向 |

⚠️ 同一个 `VALVE_CLOSE` 在第 0 步和第 3 步各出现一次：**允许**。每步各自持有一个
独立实例（`workflow.cpp:3521`），apply 阶段无重复 id 校验，且两步顺序执行不并发。

### 1.2 手动执行会**跳过第 0 步**（重要）

`command_execute_workflow()` → `workflow_start(wf, true, ...)`（`command_manager.cpp:918`），
第二个参数 `skip_first_step = true` ⇒ `current_step` 直接置 1。

所以实际执行序列是：**VALVE_OPEN → 等减重 20 g → VALVE_CLOSE**，第 0 步不会真的去关一次阀。

### 1.3 `weight_decrease` 的基线语义

`weight_trigger_start()`（`weight.cpp:591`）：`start_weight = current_weight`，
即**走到第 3 步那一刻的读数**作为基线，`poll` 里 `weight_loss = start_weight - current_weight`，
`weight_loss >= gram` 判成功。⇒ 基线是实时的，不是陈旧值。

参数保护：`gram` 合法范围 `0 < gram <= 300`，非法值会**静默回落成 20** 并发 `EVENT_WEIGHT_ERROR`。

### 1.4 ⚠️ 5 分钟硬顶

`valve.safety_timeout_sec = 300`（设备实测值，且 `valve.cpp:253` 上限就是 300，改不大）。
阀开启超过 300 s 会被 `valve_force_close()` 强制关闭并推 `EVENT_VALVE_ERROR`。

⇒ **20 g 必须在开阀后 300 s 内流完**，否则阀先自动关，流程还在等减重，直到 360 s 超时终止。
若出水偏慢，应把 `gram` 调小（如 10），而不是指望延长超时。

---

## 二、Registry 权威值（2026-10-23 02:20 重启后实测）

**Action `v=4` / `n=14`**

| k | Action | | k | Action |
|---|---|---|---|---|
| 0 | COMPUTER_RESET | | 8 | TEST_STEPPER_STOP |
| 1 | MI_THERMO_START_SCAN | | **9** | **VALVE_CLOSE** |
| 2 | MI_THERMO_STOP_SCAN | | **10** | **VALVE_OPEN** |
| 3 | MQTT_TEST | | 11 | VIBRATION_START |
| 4 | STEPPER_DISPENSE_ROTATE | | 12 | VIBRATION_STOP |
| 5 | STEPPER_EMERGENCY_STOP | | 13 | WEIGHT_ZERO |
| 6 | STEPPER_ROTATE_STEPS | | | |
| 7 | TEST_STEPPER_START | | | |

**Workflow `v=2` / `n=4`**：`0=daily_valve_test 1=daily_valve_test1 2=queue_test **3=wf_valve_20g**`

**Trigger `v=2` / `n=3`**：`0=delay 1=timer 2=weight_decrease`

---

## 三、常用命令（MQTT 直发到 `guo_feeder/288485896ce4/down`）

> MQTT 直发**不带** `device_id`、不带 `source`；走 Worker `cloud_api` 时**必须**补这两个字段。
> `i` 在 30 s 内重复会被静默丢弃 ⇒ 每次都换新值。

### 3.1 开阀 / 关阀（`v=4`）

```json
{"c":"action","i":"vo1","v":4,"k":10}
```
```json
{"c":"action","i":"vc1","v":4,"k":9}
```

### 3.2 触发这个 Workflow（`v=2`）

```json
{"c":"workflow","i":"wf1","v":2,"k":3}
```

- 内部会跳过第 0 步，直接开阀
- `v` 不符 ⇒ `e=3 s=4 "workflow registry version mismatch"`（新增/删除 Workflow 会递增）
- 不能写成 `p.o` 形式：`c="execute_workflow"` 同样要过 `v`/`k` 校验
  （`cloud_manager.cpp:1374-1394`），只有旧的非紧凑格式才用 `ob`

### 3.3 查当前秤读数

```json
{"c":"query","i":"qs1","p":{"q":"query_state","o":"weight_value"}}
```

回帧：`{"s":0,"o":"weight_value","value":"3.4","command":"query_state"}`（单位 g，字符串形式）

其它可用键：

| 键 | 含义 | 实测值 |
|---|---|---|
| `weight_value` | 当前重量（g） | `3.4` |
| `weight_valid` | 秤可用 | `true` |
| `weight_error` | 称重异常 | — |
| `valve_status` | 阀状态 | `false`（关） |

⚠️ `query_state` **必须**带 `p.o`（状态键名），缺了报 `m:"Missing 'object' (state key)" e=3`。

### 3.4 查 Workflow 是否还在

```json
{"c":"query","i":"qw1","p":{"q":"query_workflows"}}
```
```json
{"c":"workflow.get","i":"wg1","p":{"id":3}}
```

`p.id` 是 **Slot 索引（整数）**，不是业务 id —— 业务 id 允许重复，故不做主键。

---

## 四、接线判定

### 4.1 判定依据（设备实值，非仓库值）

| 项 | 设备配置 | 与接线是否一致 |
|---|---|---|
| `valve.gpio_pin` | **12** | ✓ IN 接 GPIO12 |
| `valve.active_level` | **1** | ✓ 高电平通电开阀 |
| `valve.enable` | true | ✓ |
| `valve.safety_timeout_sec` | 300 | 上限 300，改不大 |
| `weight.dt` | **6** | ✓ DT → GPIO6 |
| `weight.sck` | **7** | ✓ SCK → GPIO7 |
| `weight.enable` | true | ✓ 已于 02:03 恢复 |

### 4.2 正确项

1. **GPIO12 → 继电器 IN** ✓ 与 `valve.gpio_pin=12` 一致；符合项目规范「禁止 GPIO 直驱电磁阀」。
2. **全部共地** ✓ 继电器 DC−、电磁阀负极、12V 负极、ESP32 GND 连通。3.3V 触发必须同参考地。
3. **COM 接电源、NO 接电磁阀正极** ✓ 高边开关用 **NO** 是正确选择：断电/未吸合时负载断开，安全失效方向。
4. **HX711 引脚** ✓ DT=6 / SCK=7 与配置一致；且 `weight_valid=true` + 读数随负载变化，证明**信号链通**。
5. **HX711 供 3.3V** ✓ 安全：DOUT 电平 3.3V，不会打坏 ESP32 的 3.3V IO。

### 4.3 ⚠️ 三个必须处理的问题

#### P1（阻断级）：电磁阀用 5V 驱动，阀的额定电压必须核对

`MP1584 OUT+`（5V）→ COM → NO → 电磁阀正极。**如果阀是 12V 型号，5V 打不开**：
能听到吸合声或轻微"嗒"，但阀芯不动、不出水，且长期低压堵转发热。

判断方法：
- 看阀体丝印（`DC12V` / `DC5V`）
- 万用表量线圈电阻：**12V 阀约 20–40 Ω，5V 阀约 10–20 Ω**（`R = V²/P`，12V/3W ≈ 48 Ω）

若为 12V 阀 ⇒ 把 **COM 那根线从 MP1584 OUT+ 改到 12V 正极**，电磁阀负极仍回共地。
继电器逻辑供电（DC+）保持 5V 不变。

#### P2（阻断级）：缺续流二极管

电磁阀是感性负载。继电器触点断开的瞬间，线圈自感会产生**几十到几百伏的反向尖峰**。
当前没有二极管，后果有三：

1. 触点拉弧 ⇒ 继电器寿命快速衰减
2. 尖峰沿共地/5V 轨道回灌 MP1584 与 ESP32 ⇒ **偶发复位**（现象是"关阀瞬间设备重启"）
3. 尖峰耦合进 HX711 模拟前端 ⇒ 称重跳变

文档已明确要求「共地 **+ 续流二极管**就绪」（`P2-7-Hardware-Deferral-Record.md` R-1 条）。

**加装方式**：1N4007（或 1N5819 肖特基）**反并联在线圈两端**：
- **阴极（有环那端）→ 线圈正极**（NO 侧）
- **阳极 → 线圈负极**（GND 侧）

⚠️ 接反会**直接短路电源**，上电前务必用万用表二极管档确认方向。

#### P3（观察级）：MP1584 带电磁阀的浪涌余量

MP1584 是 3 A 降压模块。电磁阀吸合瞬间浪涌 1–2 A，加继电器线圈 70 mA 左右，接近上限。
若 5V 掉压到 4.5V 以下，会出现继电器抖动、ESP32 brownout 复位。

建议：5V 输出并 **470 µF / 16V 电解电容**做储能；或电磁阀单独供电（只共地）。
实测手段：开阀瞬间量 5V 轨，压降 >0.5 V 就要处理。

### 4.4 另外两点（非阻断）

1. **继电器 5V 供电 + 3.3V IN 触发**：多数模块可用但临界（光耦 LED 电流偏小）。
   若继电器不吸合，先把继电器 VCC 改接 3.3V 试，或换 3.3V 兼容模块。
2. **上电瞬间"哒"一声**：ESP32 复位期间 GPIO12 为高阻，若模块输入端有上拉会短暂吸合。
   属正常，频繁发生则加 10 kΩ 下拉到 GND。

### 4.5 零点偏置

空载（托盘清空）时 `weight_value = 3.4 g` 而非 0 ⇒ 零点未校准。
在 20 g 阈值下这是 **17% 的相对误差**。建议空载执行一次 `WEIGHT_ZERO`：

```json
{"c":"action","i":"wz1","v":4,"k":13}
```

---

## 五、安全提醒

1. **本 Workflow 没有任何"超时关阀"步骤**。若在等减重期间流程异常终止（超时/被取消），
   阀可能保持开启 —— 唯一的兜底是 `valve.safety_timeout_sec = 300`。**不要移除这道保护**。
2. 首次实测建议：**先把托盘放好、确认排水通路通畅**，再触发。20 g ≈ 20 ml。
3. 触发前先看 `valve_status=false`、`weight_valid=true`。
