# 设备详情、Config 参数目录、布局自定义与设备别名

> 日期：2026-10-23 · 对应需求 §3.4 / §3.5 / §3.6 / §3.7
> 参数目录**从 `config_manager.h/.cpp` 与 `data/config/*.json` 逐项提取**，
> 不凭文档名称猜测。凡范围来自"代码钳位"的都已注明，**没有声明的不臆造上下限**。

---

## 0. 三个必须先纠正的事实

| # | 事实 | 影响 |
|---|---|---|
| 1 | **`valve.open_duration_ms` 是死参数**：只有 getter，全仓**无任何消费者** | 不能做成 UI 开关；做成开关就是骗人 |
| 2 | **`valve.safety_timeout_sec` 的合法范围文档与代码不一致**：注释写 `10~300`，`valve.cpp:252-253` 实际钳位 `1~300` | UI 必须按**代码**（1~300），并把文档矛盾登记为待修 |
| 3 | **`config_save` 后有 5 分钟重启窗口**（`CONFIG_RESTART_TIMEOUT_MS`），窗口内可择机重启，超时自动重启 | §3.6 的"是否需要重启"有现成答案：**需要**，但由窗口机制接管 |

---

## 1. 布局自定义（§3.4）

### 1.1 可自定义的三类对象

| 对象 | 粒度 | 约束 |
|---|---|---|
| **首页模块**（重量卡、状态行、今日统计、近期命令…） | 显示/隐藏 + 拖动排序 | **安全状态模块不可隐藏**（见 §1.3） |
| **快捷操作** | 增 / 删 / 排序，数量 **0 ~ N** | 硬上限 12（UI 侧），下限 0 |
| **控制页分组** | 显示/隐藏 + 排序 | 分组内按钮由能力目录决定，用户不能自造 |

### 1.2 快捷操作数量与布局适应性

需求明确"不得固定为四个"。设计成**流式网格**而非固定 2×2：

| 数量 | 布局 | 说明 |
|---|---|---|
| 0 | 显示空态卡「还没有快捷操作 · 去添加」 | 不留空洞 |
| 1 | 1 列满宽 | |
| 2 | 2 列 | |
| 3 | 2 列（最后一行 1 个占满） | |
| 4 | 2 列 × 2 行 | 上一轮原型的样子 |
| 5~6 | 2 列，最多 3 行 | 第 4 行起折叠为「更多」 |
| 7~12 | 2 列 3 行 + 「更多（N）」入口 | **必须折叠**，否则把安全状态挤到屏幕外 |

**硬规则**：首屏（不用滚动）必须始终可见
`设备别名 + 在线状态 + 重量读数 + 阀门状态`。
快捷操作区**不得**挤压这四者 —— 实现方式：快捷区自己滚动/折叠，不推动上方卡片。

### 1.3 不可隐藏的模块（安全底线）

| 模块 | 理由 |
|---|---|
| 设备状态头（在线/离线 + 在线时长） | 离线时用户必须立刻知道 |
| 重量读数卡 | 供水/出粮判断依据 |
| 阀门状态徽标 | 唯一"正在出水"的提示 |
| 命令进行中条 | 有命令在飞时必须可见 |

⇒ 这几个不出现在"模块管理"列表里，或显示为**锁定（不可拖拽/不可隐藏）**。

### 1.4 持久化策略（需求要求明确回答）

| 方案 | 优点 | 缺点 | 结论 |
|---|---|---|---|
| A 仅浏览器 `localStorage` | 零后端改动，立即能用 | 换设备/清缓存即丢；多标签页不同步 | **M1 采用**（单用户单设备阶段） |
| B 后端用户配置（D1 表） | 跨设备同步 | 需新表 + 迁移 + 接口（阶段 2 之外） | **M3 再评估** |
| C URL 参数 | 可分享 | 不适合日常 | 不采用 |

**本轮决定：方案 A**。
- key：`gf.ui.layout.v1.<device_id>`（按设备隔离，为未来多设备留位）
- 值：`{ "modules":[...], "quick":[...], "v":1 }`
- **写入时机**：拖拽结束 / 开关切换后 300 ms 防抖
- **读取失败**（JSON 坏 / 版本不符）⇒ 静默回落默认值 + 记一条前端 warn，**不弹错**
- **恢复默认**：设置页「恢复默认布局」+ 二次确认；只清当前设备的 key

### 1.5 布局与业务逻辑解耦（需求原文要求）

- 布局只描述**顺序与可见性**，不描述业务：`{id:"card.weight", visible:true}`
- 每个模块 id 必须在**模块注册表**里存在；不认识的 id **丢弃而不是崩溃**（未来降级兼容）
- 业务动作（开阀/出粮）来自能力目录，与布局无关 ⇒ 桌面/平板重排时复用同一份数据

---

## 2. 设备详情（§3.5）

### 2.1 入口结构

```
设备 Tab
├─ 设备列表（多设备时）           ← 别名优先显示，副标题是 device_id 后 4 位
├─ <设备> 详情
│   ├─ 概述（固件版本 / 最后在线 / Critical Operation / 重启状态）
│   ├─ 模块列表（10 个，按是否启用排序）
│   │   ├─ 电子秤 HX711
│   │   ├─ 电磁阀
│   │   ├─ 步进电机
│   │   ├─ 振动马达
│   │   ├─ OLED
│   │   ├─ RTC
│   │   ├─ 米家温湿度计
│   │   ├─ 系统时间
│   │   ├─ WiFi
│   │   └─ MQTT
│   ├─ 操作审计
│   └─ ⚠ 保护区（在页面底部，需滚动）
│       ├─ 普通重启
│       └─ 强制重启（危险）
```

### 2.2 每个详情页的统一骨架（5 段）

1. **状态**（只读）：当前值 + 是否可观测 + 最后更新时间
2. **启用开关**（仅对可执行器/可选模块）：`enable` 键，切换后提示"需保存并重启"
3. **操作**（仅对有能力者）：来自 Action 目录，标注「已验证 / 未验证 / 不可用」
4. **配置参数**：按 §3 目录渲染，逐个含 单位/默认值/范围/说明
5. **危险区**（仅阀、步进、振动）：立即停止

### 2.3 各详情页的具体内容

| 模块 | 状态来源 | 可开关 | 操作 | 参数 |
|---|---|---|---|---|
| **电子秤** | `weight_value` / `weight_valid` / `weight_error` | ✅ `weight.enable` | 清零（`WEIGHT_ZERO`，**从常用控制移到这里**）| `dt/sck/sample_interval/scale/zero_offset/filter_samples`（只读展示，仅 `zero_offset` 可通过清零更新） |
| **电磁阀** | `valve_status` | ✅ `valve.enable` | 开/关/立即强制关闭 | `gpio_pin/active_level/safety_timeout_sec`；`open_duration_ms` **标注为"预留，当前无效果"** |
| **步进** | `stepper_enable/busy/last_result/last_pulses` | ✅ `stepper.enable` | 出粮 120° / 指定脉冲 / 急停 / **测试启动-停止（未验证）** | 12 个参数（见 §3.4） |
| **振动** | `vibration_enable/active/last_result/last_duration_ms` | ✅ `vibration.enable` | 启动（可填时长）/ 停止 | 5 个参数 |
| **OLED** | 无状态键（不可观测） | ❌ 无 enable 键 | 无 | `sda/scl/i2c_speed/frame_delay/rotation` |
| **RTC** | 每 boot 一条 WARN（未接芯片） | ✅ `rtc.enable` | 无 | `sda/scl/i2c_addr/calibrate_threshold_sec` |
| **米家温湿度** | `mi_thermo_temp/humid/bat_v/*_ts/valid/enable` | ✅ `allow_collect` | 开始/停止扫描 | `BLE_key`（**敏感**，脱敏显示）、`MAC` |
| **时间** | `time_valid` + `system.time` | ❌ | 立即 NTP 校时（**当前为预留空实现**） | `timezone/ntp_server1/ntp_server2/sync_interval_sec` |
| **WiFi** | `wifi_status/rssi/signal/state` | ❌ | 修改 SSID/密码（会重启） | `ssid/password/connect_timeout/reconnect_interval` |
| **MQTT** | `mqtt_status/retry_count/last_error` | ❌ | 无（`MQTT_TEST` 仅供排障） | `server/port/username/password/ca_path/retry_*` + `telemetry_enable` |

★ **`mqtt.password` / `mi_thermo.BLE_key` / `wifi.password` 三类敏感值**：
前端只显示 `••••`，编辑时留空表示"不修改"；服务端**不回传**明文（需 Worker 侧脱敏）。

---

## 3. Config 参数目录（权威）

图例：**D** = 默认值 · **R** = 范围（`代码` = 代码钳位 / `无` = 无校验）· **RS** = 修改后是否需要重启

### 3.1 `wifi`
| 键 | 类型 | D | R | RS | 说明 |
|---|---|---|---|---|---|
| `ssid` | str | — | 无 | ✅ | WiFi 名称 |
| `password` | str | — | 无 | ✅ | ★敏感 |
| `connect_timeout` | int | 见 json | 无 | ✅ | 连接超时（ms） |
| `reconnect_interval` | int | 见 json | 无 | ✅ | 重连间隔（ms） |

### 3.2 `oled`
| 键 | 类型 | D | R | RS | 说明 |
|---|---|---|---|---|---|
| `sda` / `scl` | int | 4 / 5 | 无 | ✅ | I2C 引脚 |
| `i2c_speed` | int | 100000 | 无 | ✅ | 总线频率 Hz |
| `frame_delay` | int | 50 | 无 | ✅ | 帧延迟 ms（★实测每 50 ms 阻塞 loop 6.4 ms） |
| `rotation` | int | 2 | `代码` 0~3 | ✅ | 0/90/180/270 |

### 3.3 `valve`
| 键 | 类型 | D | R | RS | 说明 |
|---|---|---|---|---|---|
| `gpio_pin` | int | 12 | 无（<0 视为未配置） | ✅ | 继电器 IN |
| `active_level` | int | 1 | 0/1 | ✅ | 1=高电平开阀 |
| `open_duration_ms` | int | 0 | — | — | **★死参数：无消费者，UI 只读展示并标注"预留"** |
| `safety_timeout_sec` | int | **300** | `代码` **1~300**（注释误写 10~300） | ✅ | **§4 的对象** |
| `enable` | bool | true | — | ✅ | 关闭后 `VALVE_*` **不注册** ⇒ registry 版本变化 |

### 3.4 `stepper`
| 键 | 类型 | D | R | RS | 说明 |
|---|---|---|---|---|---|
| `enable` | bool | false | — | ✅ | 安全默认关 |
| `step_pin` / `dir_pin` / `en_pin` | int | 13/14/15 | 无 | ✅ | |
| `en_active_level` | int | 0 | 0/1 | ✅ | DRV8825 默认低有效 |
| `dir_invert` | bool | false | — | ✅ | 机械方向相反时用 |
| `steps_per_rev` | int | 1600 | 无 | ✅ | 1.8°+1/8 微步 |
| `dispense_angle_deg` | int | 120 | 无 | ✅ | 单次出粮角 |
| `pulse_period_us` | int | 10000 | 无（≈100 Hz） | ✅ | 越小越快，**过快会失步** |
| `enable_delay_ms` | int | 50 | 无 | ✅ | 应用层保守值 |
| `max_pulses` | int | 6400 | `代码` 上限 6400 | ✅ | **只能收紧不能放大** |
| `action_timeout_ms` | int | 30000 | `代码` 上限 30000 | ✅ | **只能收紧不能放大** |
| `disable_after_action` | bool | true | — | ✅ | 动作后是否失能 |

### 3.5 `vibration`
| 键 | 类型 | D | R | RS | 说明 |
|---|---|---|---|---|---|
| `enable` | bool | false | — | ✅ | 安全默认关 |
| `relay_pin` | int | 16 | 无 | ✅ | 单 GPIO，两马达共路 |
| `active_level` | int | 1 | 0/1 | ✅ | 按驱动电路确认 |
| `max_duration_ms` | int | 30000 | `代码` 上限 30000 | ✅ | 定时振动上限 |
| `safety_timeout_ms` | int | 30000 | 同上 | ✅ | 模块兜底 |
| `min_interval_ms` | int | 200 | 无 | ✅ | 防启停风暴 |

### 3.6 `weight`
| 键 | 类型 | D | R | RS | 说明 |
|---|---|---|---|---|---|
| `enable` | bool | **false（仓库）/ true（设备）** | — | ✅ | ★关闭后 `WEIGHT_ZERO`+`weight_decrease` 不注册 ⇒ registry 变化 |
| `dt` / `sck` | int | 6 / 7 | 无 | ✅ | HX711 |
| `sample_interval` | int | 50 | 无 | ✅ | ms |
| `scale` | float | 741 | 无 | ✅ | 比例因子（校准产物） |
| `zero_offset` | long | -800750 | 无 | ✅ | 零点（`WEIGHT_ZERO` 修改它） |
| `filter_samples` | int | 10 | 无 | ✅ | 滤波窗口 |

### 3.7 `rtc`
| 键 | 类型 | D | R | RS | 说明 |
|---|---|---|---|---|---|
| `enable` | bool | false（仓库）/ true（设备） | — | ✅ | ★板上未接芯片 ⇒ 每 boot WARN 是预期 |
| `sda` / `scl` | int | 4 / 5 | 无 | ✅ | 与 OLED 共用 I2C |
| `i2c_addr` | int | 81 (0x51) | 无 | ✅ | PCF8563T |
| `calibrate_threshold_sec` | int | 2 | 无 | ✅ | 差值超阈值才写 RTC |

### 3.8 `time`
| 键 | 类型 | D | R | RS | 说明 |
|---|---|---|---|---|---|
| `timezone` | int | 8 | 无 | ✅ | UTC+8 |
| `ntp_server1` / `ntp_server2` | str | aliyun / cn.pool | 无 | ✅ | |
| `sync_interval_sec` | int | 86400 | 无 | ✅ | 24 h |

### 3.9 `mqtt`
| 键 | 类型 | D | R | RS | 说明 |
|---|---|---|---|---|---|
| `mqtt_server` / `mqtt_port` | str/int | emqxsl / 8883 | 无 | ✅ | |
| `client_id` / `subscribe_topic` / `publish_topic` / `log_topic` | str | **模板**（含 `<device_id>`） | — | ✅ | ★**只读**：渲染点在 `topic_renderer.h`，UI 不得直接改（改了就断链） |
| `username` / `password` | str | GuoFeederDevice / ★ | — | ✅ | ★敏感；★**ACL 挂 username** |
| `ca_path` | str | /emqxsl-ca.crt | 无 | ✅ | |
| `retry_interval` / `retry_max` / `sleep_retry_interval` | num | 10000/30/1800000 | 无 | ✅ | |
| `mqtt_keep_alive` | int | 60 | 无 | ✅ | |
| `telemetry_enable` | bool | — | fail-closed（缺键=false） | ✅ | 遥测总开关 |

### 3.10 `mi_thermo`
| 键 | 类型 | D | R | RS | 说明 |
|---|---|---|---|---|---|
| `BLE_key` | str | — | 无 | ✅ | ★敏感（32 hex） |
| `MAC` | str | — | 无 | ✅ | |
| `mi_thermo_allow_collect` | bool | true | — | — | **状态键，不需要重启**（运行时读） |

### 3.11 模块清单常量

- `CONFIG_MODULE_COUNT = 10`，`kModuleNames[]` 10 项
- **无 `cloud` 模块**（不在 config 体系里）
- 新增模块必须同时改：`CONFIG_MODULE_COUNT` + `kModuleNames[]` + `data/config/<m>.json` + getter，
  且**首启要写 `{}` 并置 loaded**（否则 `config_set` 永远 `module not found`）

---

## 4. 电磁阀自动关闭时间（§3.6 专项）

### 4.1 它到底在哪一层

| 候选 | 结论 | 证据 |
|---|---|---|
| 固件硬编码常量 | ❌ 不是 | `safety_timeout_ms` 由 config 赋值（`valve.cpp:251-254`） |
| **固件 Config** | ✅ **就是这个** | `valve.safety_timeout_sec`，默认 **300** |
| Workflow 参数 | ❌ 不是 | Workflow 的 `timeout_ms` 只结束账本，**不关阀** |
| 其它安全计时 | 部分 | `DispenseGuard`（事件驱动）、`workflow.timeout_ms`（不关阀） |

### 4.2 四个关阀/停水机制的关系（关键，别改错）

| # | 机制 | 触发条件 | 效果 | 是否随本次修改变化 |
|---|---|---|---|---|
| **M1** | `valve.safety_timeout_sec` → `valve_task()` | 开阀持续超过 N 秒 | `valve_set_gpio(false)` + 事件 `LOG_P_LIMIT_MS` | ✅ **就是它** |
| **M2** | `DispenseGuard` → `EVENT_WEIGHT_ERROR` | 秤异常（`|Δ|≥50 g` × 5 次/5 s） | `valve_force_close()` | ❌ 无关 |
| **M3** | `VALVE_CLOSE` Action（Workflow 步骤） | 业务正常结束 | GPIO 关 | ❌ 无关 |
| **M4** | `workflow.timeout_ms`（当前 360 s） | Workflow 超时 | **只置 TIMEOUT，不关阀** | ❌ 无关（**但这是隐患**：超时后阀仍开，只能等 M1） |
| **M5** | `valve.open_duration_ms` | —— | **无消费者，无效果** | ❌ 死参数 |

⇒ **只改 M1 不会遗漏任何东西**，但必须知道 **M4 不兜底**：
Workflow 超时（360 s）> M1（300 s）⇒ 实际上 M1 先兜。**若把 M1 改到 60 s，
Workflow 超时仍是 360 s，两者不会冲突，但语义上"流程 6 分钟、阀 1 分钟"意味着流程必然在 60 s 被 M1 打断。**
这一点必须写进 UI 提示。

### 4.3 最小修改方案

**目标**：正常使用默认 1 分钟，可由 UI 修改，设备侧生效，不依赖手机在线。

| 步骤 | 改动 | 位置 | 影响 |
|---|---|---|---|
| 1 | 默认值 300 → **60** | `config_manager.cpp:3601` 的 `| 300` → `| 60` | 仅影响**从未写过该键**的模块（已有设备实值 300 不变） |
| 2 | 合法范围明确为 **10~300** | `valve.cpp:252-253` 的 `<1` → `<10` | ⚠ 会把现存 `1~9` 的配置**钳到 10**；需先确认设备实值 |
| 3 | UI 在阀详情页提供滑杆/输入（10~300 s，步进 10） | 前端 | — |
| 4 | 保存走 `config_set` + `config_save` | 既有链路 | 触发 5 分钟重启窗口 |

**风险与对策**：

| 风险 | 对策 |
|---|---|
| 设备已有 `safety_timeout_sec=300`，改默认值**对它无效**（只影响新键） | UI 必须显示实际值，并提供"改成 60"的显式操作；**不能假设改代码就生效** |
| 改成 10 s 会让 21 g 供水（实测 45 s）**中途被截断** | 参数页必须显示当前 Workflow 的典型用时，并在 `safety_timeout_sec` < 该用时 **1.5 倍**时给红色警告 |
| 改范围下限 1→10 可能钳改现有值 | 落地前先 `config_query module=valve` 读实测值（**当前 300，安全**） |
| 需要重启才生效 | 走既有 5 分钟窗口；UI 提供"立即重启"按钮（`config.restart`） |

### 4.4 明确回答需求的 7 条

1. 默认 1 分钟 → **改 `| 300` 为 `| 60`**（步骤 1）
2. UI 可改 → 阀详情页参数（步骤 3）
3. 单位 秒 / 最小 **10** / 最大 **300** / 默认 **60** / 持久化 `data/config/valve.json`（步骤 1~4）
4. 需要重启 → **是**，由 `CONFIG_RESTART_TIMEOUT_MS`（5 min）窗口接管，可立即重启
5. 沿用 ConfigManager 的 Active/Backup + `boot_ok` 回滚 → **自动满足**（走既有 `config_set/save`）
6. 多个保护机制的关系 → §4.2 五条；**M4 不兜底**这一点必须写进 UI
7. 设备端生效、不依赖手机 → **满足**（`valve_task()` 在设备 loop 里）

---

## 5. 设备别名（§3.7）

| 项 | 决定 |
|---|---|
| **存储位置** | **D1 `device_alias` 表**（拟议，随 `0008` 迁移）—— 别名是"用户资产"，不能只存浏览器 |
| 备选 | `device` 表加 `alias TEXT` 列（更简单，但改既有表结构；D1 可 `ALTER TABLE ADD COLUMN`）→ **优先选这个**，避免新表 |
| 唯一性 | 不要求全局唯一；同一用户下的显示冲突时追加 `·<id 后 4 位>` |
| 长度 | 1~24 字符（前后空白 trim；全空白视为未设置） |
| 默认显示 | 未设置别名 ⇒ 显示 `设备 <id 后 4 位>` |
| 显示优先级 | 首页标题 > 设备列表 > 控制确认框 > 命令记录 > 审计 |
| 真实 ID | **设备详情页必须可查看完整 12-hex**；`cmd_id` 详情面板同时显示 device_id |
| 路由影响 | **零**。别名**不参与** MQTT topic、`cmd_id`、审计主键 |
| 多设备 | `device_id` 是主键，别名是它的属性 ⇒ 天然支持 |

**修改权限**：仅已登录会话（阶段 2 的鉴权完成后）。

---

## 6. 校验分层（不得只靠前端）

| 层 | 校验内容 | 现状 |
|---|---|---|
| 前端 | 类型、范围、必填、依赖（如 `enable=true` 才能操作） | 需实现 |
| Worker | 同上 + 敏感字段脱敏 + 白名单键 | 部分（`ingest-validate.js` 只管入库） |
| 设备 | `config_set` 的 `module_ro` 只读键 + 各模块 init 时的钳位 | ✅ 已有 |

★ **设备侧钳位是最后防线**：如 `max_pulses` / `action_timeout_ms` 代码硬上限 6400/30000，
前端就算显示可输入 99999，设备也会收紧。UI 必须显示**设备接受后的实际值**，不能显示用户输入值。
