# 设备能力目录（2026-10-23）

> 三态定义（必须分开看，不可合并）：
> **F = 固件支持**（代码里已注册，可被调用）·
> **C = 云端链路支持**（能从云端下发并到达设备）·
> **H = 真机验证通过**（有实测证据，不是"代码看起来对"）
>
> 状态：`✔` 是 · `✘` 否 · `△` 部分 · `?` 待验证

---

## 0. 一张表看懂"能做什么、能验证到什么程度"

| 类别 | 数量 | F | C | H |
|---|---|---|---|---|
| Action | 14 | ✔ | ✔（下发）／**✘（回读）** | **3/14** |
| Trigger | 3 | ✔ | ✔（只能嵌在 Workflow 里） | 1/3 |
| Workflow 管理 | 7 | ✔ | ✔ | 4/7 |
| 查询 | 8 + `query_state` | ✔ | ✔（下发）／**✘（回读）** | 3/9 |
| System 对象 | 20+ | ✔ | ✔ | 5/20+ |

**"✘（回读）"是全局性的**：云端**没有任何**端点能读到 `mqtt_messages` 里的 ack/result 回帧
（见 `01-investigation.md` §4）。所以下表"链路支持"一列写 ✔ 仅指**能发出去**，
**不代表能知道执行结果**。这不是逐项缺陷，是**一个统一的缺失**。

---

## 1. Action（14 个，`registry v=4 / n=14`）

`k` = stable_id（按 `runtime_id` 字符串升序，**会随注册集合变化重排，禁止硬编码**）。
参数列取自固件描述符；**注意约束是自由文本**，固件不提供 min/max/default 结构字段。

| k | Action | 中文 | 参数 | 约束（原文） | F | C | H |
|---|---|---|---|---|---|---|---|
| 0 | `COMPUTER_RESET` | 电脑重启 | 无 | — | ✔ | ✔ | ? |
| 1 | `MI_THERMO_START_SCAN` | 米家温度计开始扫描 | 无 | — | ✔ | ✔ | ? |
| 2 | `MI_THERMO_STOP_SCAN` | 米家温度计停止扫描 | 无 | — | ✔ | ✔ | ? |
| 3 | `MQTT_TEST` | MQTT 压测 | `length` int (`byte`) | "MQTT测试消息长度"（无范围） | ✔ | ✔ | ? |
| 4 | `STEPPER_DISPENSE_ROTATE` | 步进出粮旋转（120°） | 无 | 脉冲由量化账本分配 | ✔ | ✔ | **✘** |
| 5 | `STEPPER_EMERGENCY_STOP` | 步进急停 | 无 | — | ✔ | ✔ | **✘** |
| 6 | `STEPPER_ROTATE_STEPS` | 步进指定脉冲旋转 | `pulses` int(step)<br>`direction` int(enum) | "1 ~ max_pulses"<br>"0=出粮向，1=反向；**必须显式提供**" | ✔ | ✔ | **✘** |
| 7 | `TEST_STEPPER_START` | 步进测试-启动 | `hz` int(Hz)<br>`direction` int(enum) | "10~2000，默认 100"<br>"默认 0" | ✔ | ✔ | △（LEDC 建波 ✔，**电机是否转 ✘**） |
| 8 | `TEST_STEPPER_STOP` | 步进测试-停止 | 无 | 幂等 | ✔ | ✔ | △（脚本跑过，未观察电机） |
| 9 | `VALVE_CLOSE` | 电磁阀关 | 无 | — | ✔ | ✔ | **✔**（05:16:12 `VALVE_OPEN_MS=45364` 结束） |
| 10 | `VALVE_OPEN` | 电磁阀开 | 无 | 受 `safety_timeout_sec` 钳制（1~300） | ✔ | ✔ | **✔**（05:15:27→05:16:12 连续 45.4 s） |
| 11 | `VIBRATION_START` | 振动启动 | `duration_ms` int(ms) | "0 = 持续振动（立即返回成功，需显式停止）；>0 = 定时" | ✔ | ✔ | **✘** |
| 12 | `VIBRATION_STOP` | 振动停止 | 无 | 幂等 | ✔ | ✔ | **✘** |
| 13 | `WEIGHT_ZERO` | 电子秤零点校准 | 无 | "保证电子秤空载时才可执行此命令" | ✔ | ✔ | **✘**（未在真机执行过） |

### 1.1 ★ Action 集合会随配置变化（影响编辑器与 UI）

| 模块 | `enable=false` 时是否注册 | 依据 |
|---|---|---|
| `valve` | **不注册**（`return` 在注册之前） | `valve.cpp:256-259` |
| `weight` | **不注册**（`WEIGHT_ZERO` + `weight_decrease` 一起消失） | `weight.cpp:716` 起 |
| `stepper` | **仍注册**（注释明写"避免 stable_id 漂移"） | `stepper.cpp:737` |
| `test_stepper` | **仍注册** | `test_stepper.cpp:400` |
| `vibration` | **仍注册** | `vibration.cpp:476` |

⇒ 关阀门/关秤会**改变 registry 版本**，UI 必须重新拉取；
关掉阀门后 `VALVE_OPEN` 按钮必须**消失或禁用**，不能"点了没反应"。

### 1.2 硬件验证状态（诚实标注）

- **已验证**：`VALVE_OPEN` / `VALVE_CLOSE`（21 g 闭环）
- **未验证，且不能从规划中删除**：`STEPPER_*`(3) / `VIBRATION_*`(2) / `WEIGHT_ZERO`
  —— 它们在固件里可用、云端可下发，只是**真机没跑通过**。
  UI 应把它们标记为 **"未验证（beta）"** 而不是隐藏，否则等于永久放弃功能。
- **待验证**：`COMPUTER_RESET` / `MI_THERMO_*` / `MQTT_TEST`（无实测证据）

---

## 2. Trigger（3 个，`registry v=2 / n=3`）

Trigger **不能单独下发执行**，只能作为 Workflow 的一个步骤。云端"执行 trigger"= 执行整个 Workflow。

| k | Trigger | 中文 | 参数 | 约束（原文） | F | C | H |
|---|---|---|---|---|---|---|---|
| 0 | `delay` | 延时 | `seconds` int | "1-3600" | ✔ | ✔（作为步骤） | △（间接） |
| 1 | `timer` | 定时 | `type` string；`hour`；`minute`；`year`；`month`；`day`；`day_of_week` | "once/daily/weekly"；"0-23"；"0-59"；"2026"；"1-12"；"1-31"；"0-6 (0=Sunday)" | ✔ | ✔（作为步骤） | ? |
| 2 | `weight_decrease` | 重量减少 | `gram` float (克) | "0 < gram <= 300" | ✔ | ✔ | **✔**（05:16:12 `DELTA_G=21.04`） |

### 2.1 ★ `gram` 非法时是**静默回落**，不是报错

非法 `gram` ⇒ 回落到 20 g 并发 `EVENT_WEIGHT_ERROR`（`weight.cpp` 注释与实测日志一致）。
⇒ **编辑器必须前置校验**，否则用户以为设了 300 g，实际跑的是 20 g。
⇒ 这是"必须有云端参数约束表"的最强理由（见 `04-workflow-editor.md` §2）。

### 2.2 `weight.enable=false` 时 `weight_decrease` 不注册

⇒ 引用该 trigger 的 Workflow 在设备上会 **`WF_FAILED / CAUSE=TRIGGER_FAILED`**（实测 `CAUSE=3`）。
⇒ UI 打开 Workflow 详情时必须先比对 registry，缺 trigger 就红字提示。

---

## 3. Workflow 管理（7 个命令，全部 F ✔ C ✔）

| 命令 | 用途 | 前置 | 副作用 | H |
|---|---|---|---|---|
| `workflow.list` | 摘要（stable_id + variant） | — | 无 | ✔ |
| `workflow.get` | 取单个完整 JSON | 需 `p.id` | 无 | ✔ |
| `workflow.create` | 新建 | `p.workflow` 完整定义 | 内存态，`dirty=true` | ✔ |
| `workflow.set` | 整体替换 | `p.id` + `p.workflow` | `variant`+1、`dirty=true`、**registry v +1** | ✔ |
| `workflow.delete` | 删除 | `p.id` | **只置 `valid=false`**，非物理删除 | ? |
| `workflow.save` | 落盘 | — | **触发设备安全重启**、`registry_version` +1 | ✔（3 次） |
| `workflow.sync_info` | 同步元信息 | — | 无 | ? |

**[代码确认] 没有 `workflow.reorder` / `workflow.rename`**：
- 步骤顺序 = 定义里 `steps[]` 数组顺序 ⇒ "排序"只能靠 `workflow.set` 重写整份
- "改名" = `name` 字段，同样走 `set`

**[实测] 每次 `save` 都会重启设备**（约 40 s）⇒ 编辑器不能"每改一步就保存"，
必须是**显式保存 + 明确的"将重启设备"提示**。

**[实测] 执行 Workflow 时跳过第 0 步**（`workflow_start(wf, true, …)`）⇒
第 0 步只在串口 `wfc run <slot> 0` 时执行。**编辑器必须显示这个语义**，
否则用户把"开阀"放在第 0 步会以为会执行。

---

## 4. 查询（8 个 + `query_state`）

| 命令 | 用途 | 需要 | H |
|---|---|---|---|
| `query_state` | 读单个状态键（`p.o` 必填） | `p.o` | ✔ |
| `query_actions` | Action 列表（名字 + 参数？**实为 id/name**） | — | ? |
| `query_triggers` | Trigger 列表 | — | ? |
| `query_workflows` | Workflow 摘要（含 `state` / `current_step` / `step_count`） | — | ✔ |
| `query_capabilities` | **三段 `{version, checksum, count, entries[]}`** | — | ? |
| `query_action_registry` | 等价 `registry k=0` | — | ✔ |
| `query_trigger_registry` | 等价 `registry k=1` | — | ✔ |
| `query_workflow_registry` | 等价 `registry k=2` | — | ✔ |

### 4.1 `query_state` 可用键（**这是状态页的数据源**）

```
wifi_status · wifi_rssi · wifi_signal · wifi_state · time_valid
valve_status
weight_value · weight_error · weight_valid
mqtt_status · mqtt_retry_count · mqtt_last_connect_time · mqtt_last_error
mi_thermo_temp · mi_thermo_humid · mi_thermo_bat_v
mi_thermo_temp_ts · mi_thermo_humid_ts · mi_thermo_bat_ts
mi_thermo_valid · mi_thermo_enable
stepper_enable · stepper_busy · stepper_last_result · stepper_last_pulses
```
（`system_state.cpp` 的 `state_map[]`；`vibration` 无状态键 —— **这是能力缺口**）

★ **每个键要一次请求**（`p.o` 单键）⇒ 状态页要 8 个键就是 8 条 MQTT 下行 + 8 条上行。
⇒ UI 不应"逐键轮询"，而应**优先用 `snapshot`（D1 聚合）**，只在必要时才下发 `query_state`。
这条直接决定 §3 的轮询设计。

---

## 5. System 对象（`c:"system"`, `p:{o:…}`）

| 分类 | 对象 | 用途 | H |
|---|---|---|---|
| 运维 | `reboot` | 重启 | ✔（今日用过） |
| 运维 | `restart` / `restart_status` | 安全重启 / 查询重启状态 | ? |
| 时间 | `time` / `set_time` | 读时间 / 设时间 | ? |
| 秤 | `weight_zero` | 零点（与 `WEIGHT_ZERO` Action 重复） | ? |
| 网络 | `wifi_config` / `wifi_ap` | 配网 / 开 AP | ? |
| 配置 | `config_query` / `config_set` / `config_save` | **读写配置（含模块 enable）** | ✔ |
| 配置 | `config_restart` / `config_delete` / `config_module` / `config_reset` / `config_backup` | 高级配置操作 | ? |
| 诊断 | `memory` / `flash` / `stats` / `loop` / `memwatch` | 资源与耗时 | ✔（`stats`） |

★ `config_set` **只改内存**（回 `restart required`），必须配 `config_save` 才落盘并重启。
⇒ UI 的"开关模块"必须做成**两步 + 明确提示"将重启"**，不能一个 toggle 直接发。

---

## 6. 数据展示类（非设备命令，来自 D1）

| 数据 | 来源 | 真实量 | 说明 |
|---|---|---|---|
| `weight.value` | `telemetry_history` | 369 行（24h 343） | 曲线可用 |
| `climate.temperature` | 同上 | 173 行（24h 97） | 曲线可用 |
| `climate.humidity` | 同上 | 179 行（24h 99） | 曲线可用 |
| 日志 | `log_history` | 3970 行 | **必须解码 `payload`**（`message` 全 NULL） |
| 事件 | `device_event` | 少量 | 13 种 type（在线/离线/凭据/ACL…） |
| 在线 | `device.last_seen` + 300 s 阈值 | — | 派生值，不是字段 |

---

## 7. 缺口清单（能力层面，供裁决）

| # | 缺口 | 影响 | 建议 |
|---|---|---|---|
| G-1 | **无命令回帧回读端点** | UI 无法显示"设备已确认/执行成功" | 新增只读端点（M1） |
| G-2 | **Registry 无参数元数据** | 无法自动生成参数表单 | 云端元数据目录（M2）+ 长期提"固件暴露参数 schema" |
| G-3 | **无 `vibration` 状态键** | 首页无法显示振动是否在跑 | 记为缺口；或 UI 只显示"最近一次操作结果" |
| G-4 | **无 workflow 重排/改名专用接口** | 编辑器只能整份替换 | 接受（用 `set` 实现） |
| G-5 | **无"停 workflow"的 MQTT 命令** | 卡住的流程只能等超时（最长 600 s）或串口 | 记为高风险；UI 只能显示"无法中断" |
| G-6 | **无 `workflow.get` 的"草稿"概念** | 改动即 `dirty`，误操作会污染设备态 | UI 侧做本地草稿（不落设备） |
| G-7 | `stepper`/`vibration` **无结构化日志码** | 云端看不到这两个模块的事件 | 专项立项（不在 UI 范围） |
