# MQTT 命令参考手册（设备协议速查）

> 用途：一页查清所有下行/上行帧格式，避免每次翻源码。
> 依据：`src/cloud/cloud_manager.cpp`（帧解析）、`src/services/command_manager.cpp`（路由分发）、
> `Cloudflare_Assets/guo-feeder-api/src/capability-catalog.js`（生产验证过的参数契约）。
> 本文所有帧形态均以当前代码为准（2026-10-25 核对）。

---

## 0. 连接与 Topic

| 项 | 值 |
|---|---|
| Broker | EMQX（`*.emqxsl.cn:8443`，TLS） |
| 设备 MQTT 账号 | `GuoFeederDevice`（ACL 挂 username，≠ client_id `dev_<mac>`） |
| 下行（发命令） | `guo_feeder/<device_id>/down` |
| 上行（收结果） | `guo_feeder/<device_id>/up` |
| 日志上行 | `guo_feeder/<device_id>/log` |
| 日志回执下行 | `guo_feeder/<device_id>/log/ack` |

- `<device_id>` 例：`288485896ce4`。
- **ACL 缺失 = 静默拒绝**（SUBACK 照常回）⇒ SUBSCRIBED 不是成功证据；改 ACL 必须重启设备。
- 新增下行 topic = 三件事同次发布：固件模板+订阅 / Worker `buildAclRules()` / EMQX 规则 WHERE 排除。

---

## 1. 帧格式总则（三套键，勿混）

| 通道 | 格式 | 命令键 | 对象键 | 参数键 | id 键 |
|---|---|---|---|---|---|
| **MQTT 直发**（本文） | 压缩 | `c` | `p.o`（或 `ob`） | `p` | `i` |
| 串口 `cm` | 未压缩 | `cmd` | `ob` | `pl` | `id` |
| Worker→设备 | 压缩 | `c` | `p.o` | `p` | `i` |

- ★ **串口探针用未压缩键**：`{"cmd":...,"ob":...,"pl":{...}}`。MQTT 压缩键发串口 ⇒ `s=4 Missing 'command' field` 静默失败（曾误判锁存、误开阀）。
- ★ **MQTT 直发不带 `device_id`/`source`**；走 Worker 云 API 才带。
- ★ **`i`（cmd_id）30 秒内重复 ⇒ 静默丢弃**。每次命令必须换新 id。
- 上行全压缩：`s`(状态) `c`(cmd) `i`(id) `o` `p`(数据) `m`(消息) `t` `e`(错误码) `v`(版本) `k`(stable_id)。**上行无 `status` 字段** ⇒ 按 `msg.status` 解析必落 unknown；认整数 `s`。

### `s` 状态枚举

| s | 含义 |
|---|---|
| 0 | OK / done（终态） |
| 1 | accepted（已受理，非终态） |
| 2 | running（非终态） |
| 3 | FAIL（终态，参数/业务失败） |
| 4 | ERR（终态，协议/路由错误，如 `Missing 'command' field`、`registry version mismatch`） |

### 下行 `c` 类型分派（cloud_manager.cpp）

| `c` | 语义 | 需要字段 |
|---|---|---|
| `action`（别名 `execute_action`） | 执行动作 | `v`=registry 版本，`k`=动作 stable_id |
| `workflow`（别名 `execute_workflow`） | 执行 Workflow | `v`=workflow registry 版本，`k`=流程 stable_id |
| `registry` | 查 registry | `k`=0 动作 / 1 触发器 / 2 流程 |
| `system` | system 族 | `p.o`=子命令，其余参数平铺在 `p` |
| `query` | 查询族 | `p.q`=查询名（如 `query_state`），`p.o`=state 键（query_state 用） |
| `safety` | 安全停堆 | `p.o`=`stop_all`/`clear` |
| 其他任意字符串 | **透传**（`c` 即一级命令名） | `p.o`/`ob`=object；workflow.list 等以 `c` 直接命中 |

- `v` 不匹配 ⇒ `e=3 s=4`（registry version mismatch）。
- ★ registry 随模块启停变化（weight off ⇒ v=5 n=13；on ⇒ v=6 n=14）⇒ **禁写死 `k`**，先查 `registry`。

---

## 2. 执行动作（Action）

### 帧形态

```json
{"c":"action","i":"<唯一id>","v":<registry版本>,"k":<stable_id>,"p":{<参数>}}
```

### 动作 stable_id 速查（hint，**以 `registry` 实查为准**）

| k(hint) | 动作 | 参数 |
|---|---|---|
| 0 | COMPUTER_RESET 电脑重启 | — |
| 1 | MI_THERMO_START_SCAN 开始温湿度扫描 | — |
| 2 | MI_THERMO_STOP_SCAN 停止扫描 | — |
| 3 | MQTT_TEST 压测 | `length`(1..1500, 默认64) |
| 4 | STEPPER_DISPENSE_ROTATE 出粮 120° | — |
| 5 | STEPPER_EMERGENCY_STOP 步进急停 | — |
| 6 | STEPPER_ROTATE_STEPS 指定脉冲旋转 | `pulses`(1..6400, 默认1600) `direction`(0出粮向/1反向) |
| 7 | TEST_STEPPER_START 步进测试启动 | `hz`(10..2000) `direction` |
| 8 | TEST_STEPPER_STOP 步进测试停止 | — |
| 9 | VALVE_CLOSE 关阀 | — |
| 10 | VALVE_OPEN 开阀 | — |
| 11 | VIBRATION_START 振动启动 | `duration_ms`(0=持续, 上限30000) |
| 12 | VIBRATION_STOP 振动停止 | — |
| 13 | WEIGHT_ZERO 秤清零 | — |

示例（振动 3 秒）：

```json
{"c":"action","i":"act-20261025-001","v":6,"k":11,"p":{"duration_ms":3000}}
```

---

## 3. 执行 Workflow

```json
{"c":"workflow","i":"<唯一id>","v":<workflow registry版本>,"k":<流程stable_id>}
```

- ★ **执行会跳过第 0 步**（步骤索引从 1 开始跑）；串口 `wf_start <slot> 0` 才从 0 跑。
- 触发器类型（delay/timer/weight_decrease 的参数表见 catalog，参考 `query_triggers` 回帧的 params 元数据）。

---

## 4. 查询族（只读）

### 帧形态

```json
{"c":"query","i":"<唯一id>","p":{"q":"<查询名>","o":"<query_state 用:状态键>"}}
```

| p.q | 作用 | 额外参数 |
|---|---|---|
| `query_state` | 查单个 System State 键 | `p.o` = 键名（必填） |
| `query_actions` | 动作清单（含中文名 + params 元数据） | — |
| `query_triggers` | 触发器清单 | — |
| `query_workflows` | 流程清单 | — |
| `query_capabilities` | 能力总览 | — |
| `query_action_registry` | 动作 registry（v+k 表） | — |
| `query_trigger_registry` | 触发器 registry | — |
| `query_workflow_registry` | 流程 registry | — |

### System State 键表（`query_state` 的 `p.o`，共 29 个）

```
wifi_status  wifi_rssi  wifi_signal  wifi_state  time_valid
valve_status
weight_value  weight_error  weight_valid
mqtt_status  mqtt_retry_count  mqtt_last_connect_time  mqtt_last_error
mi_thermo_temp  mi_thermo_humid  mi_thermo_bat_v
mi_thermo_temp_ts  mi_thermo_humid_ts  mi_thermo_bat_ts
mi_thermo_valid  mi_thermo_enable
stepper_enable  stepper_busy  stepper_last_result  stepper_last_pulses
vibration_enable  vibration_active  vibration_last_result  vibration_last_duration_ms
```

示例（步进是否在忙）：

```json
{"c":"query","i":"q-20261025-001","p":{"q":"query_state","o":"stepper_busy"}}
```

### registry 查询

```json
{"c":"registry","i":"<唯一id>","k":0}
```

`k`：0=动作 / 1=触发器 / 2=流程。★ `k` 必须顶层（不在 p 里）。

---

## 5. system 族（`c:"system"` + `p.o`）

### 帧形态

```json
{"c":"system","i":"<唯一id>","p":{"o":"<子命令>", ...子命令参数}}
```

### 子命令清单

| p.o | 作用 | 参数 |
|---|---|---|
| `memory` | 内存信息 | — |
| `flash` | Flash 信息/文件列表 | — |
| `stats` | 统计 | — |
| `loop` | 主循环诊断（只读，不 reset） | — |
| `memwatch` | 内存监视（只读，不 reset） | — |
| `time` | 读时间 | — |
| `restart_status` | 查重启状态 | — |
| `restart` | 普通重启 | — |
| `force_restart` | 强制重启（先停硬件→落 boot flag→300ms 后复位） | — |
| `set_time` | 设置时间（占位） | — |
| `weight_zero` | 秤清零（等价 WEIGHT_ZERO） | — |
| `wifi_config` / `wifi_ap` | WiFi 配置 | 见源码 |
| `reboot` | legacy 重启别名 | — |
| `config_query` | 查配置 | 见 §7 |
| `config_set` | 改配置（RAM，需 save） | 见 §7 |
| `config_save` | 落盘 | `{}` |
| `config_restart` | 经 ConfigManager 重启 | `{}` 或 `{"cancel":true}` |
| `config_delete` / `config_module` / `config_reset` / `config_backup` | 配置管理 | 见源码 |

### 重启两兄弟（★ 行为差异必读）

```json
{"c":"system","i":"r-001","p":{"o":"restart"}}
{"c":"system","i":"r-002","p":{"o":"force_restart"}}
```

- `restart`（普通）：安全窗口重启；**不发 object 回帧** ⇒ 云端 audit **accepted 永不 done**（防假-done 设计，正常现象）。
- `force_restart`：顺序=①先 `safety_stop_all`（失败也继续）②落 boot flag（防 CFG-ROLLBACK）③300ms 后复位。**result 回帧丢失复现 2/2**（300ms 窗口 publish 竞态，ack 正常）——固件已知限制；重启效果用 boot_seq 递增 + query_actions 行为证据判定。
- ★ **重启完成判据**：空闲设备只发 `/log` 不刷 last_seen ⇒ 用 log_history 增长 + boot_seq 递增判定，**不要用 last_seen**。

---

## 6. safety 族（急停/解锁）

```json
{"c":"safety","i":"s-001","p":{"o":"stop_all"}}
{"c":"safety","i":"s-002","p":{"o":"clear"}}
```

- `stop_all`：停全部执行器（急停）。`clear`：解除停机锁存。
- ★ 独立一级命令，**不是 Action**（避免 registry 重排）。
- ★ 急停白名单：workflow/action 内只放行 5 项停止类（VALVE_CLOSE / STEPPER_EMERGENCY_STOP / TEST_STEPPER_STOP / VIBRATION_STOP / MI_THERMO_STOP_SCAN）；四个拦截点（command_execute_action / enqueue_temp_action / workflow_start / workflow_action_start）。
- 急停后物理核实：另发 `query_state p.o=valve_status` 独立确认，**不要只凭命令回执**。

---

## 7. Workflow 管理族（`c` = 命令名本身）

### 帧形态（透传：`c` 直接是一级命令名）

```json
{"c":"workflow.list","i":"w-001","p":{}}
{"c":"workflow.get","i":"w-002","p":{"id":3}}
{"c":"workflow.create","i":"w-003","p":{"workflow":{...完整Workflow对象...}}}
{"c":"workflow.set","i":"w-004","p":{"id":3,"workflow":{...完整Workflow对象...}}}
{"c":"workflow.delete","i":"w-005","p":{"id":3}}
{"c":"workflow.save","i":"w-006","p":{"restart":false}}
```

### 参数契约（真机实测确认）

| 命令 | 定位键 | 语义 | 坑 |
|---|---|---|---|
| `workflow.sync_info` | — | 同步信息（版本/dirty） | — |
| `workflow.list` | — | 列表（回帧自带 registry_version + dirty） | — |
| `workflow.get` | **`p.id`**（slot 序号 0..15） | 详情；workflow 对象在回帧 **`o` 字段** | ★ 传 `p.o` ⇒ `s=4 Missing 'id'`（10-23 真机实测） |
| `workflow.create` | — | 自动找空闲 slot（满 16 ⇒ `no_free_slot`） | ★ 只收嵌套形态 `p.workflow`，平铺一律拒 |
| `workflow.set` | `p.id` + `p.workflow` | **整体替换** | ★ `workflow.steps` 缺省 = 清空该 slot 全部步骤 |
| `workflow.delete` | `p.id` | 逻辑删除（valid=false），幂等 | 已无效仍返回 success |
| `workflow.save` | `p.restart` | 落盘 Flash | ★ 固件默认 `restart=true` ⇒ **云端强制覆写 `restart:false`**；要重启走 system.restart 由人决定 |
| `workflow` (执行) | `v`+`k` | 见 §3 | 执行跳过第 0 步 |

- Workflow JSON 校验：`id` 必填非空；steps ≤16；单 step params ≤8；`step.type` ∈ {trigger, action}。
- CRUD 在 running/dirty 事务 acquire 失败时被固件拒（s=4）——云端不预判，如实透传。

---

## 8. Config 完整流程（查 → 改 → 存 → 重启生效）

### 8.1 查配置 `config_query`（异步，回**两帧**）

```json
// 全部模块
{"c":"system","i":"cq-001","p":{"o":"config_query"}}
// 整个模块
{"c":"system","i":"cq-002","p":{"o":"config_query","module":"valve"}}
// 单个字段
{"c":"system","i":"cq-003","p":{"o":"config_query","module":"valve","key":"open_duration_ms"}}
// 只回键名不要值（省带宽）
{"c":"system","i":"cq-004","p":{"o":"config_query","module":"valve","keys_only":true}}
```

- ★ **回两帧取第二条**（第一帧 accepted、第二帧才带 data）。
- 合法模块名（10 个）：`wifi` `oled` `valve` `time` `weight` `mqtt` `mi_thermo` `rtc` `stepper` `vibration`。

### 8.2 改配置 `config_set`（只改 RAM，**不落盘、不生效于已初始化模块**）

```json
{"c":"system","i":"cs-001","p":{"o":"config_set","module":"valve","key":"open_duration_ms","value":8000}}
```

- `value` 只支持标量（int/bool/float/string）；null/object/array 入队即拒。
- 可选 `expect_version`（乐观锁）：传配置版本号，版本不符则拒；缺省不校验。
- 回帧 message：`config set queued, restart required` ⇒ **"命令已接受" ≠ "已持久化生效"**。

### 8.3 落盘 `config_save`（无参数）

```json
{"c":"system","i":"csv-001","p":{"o":"config_save"}}
```

### 8.4 生效：重启

```json
{"c":"system","i":"cr-001","p":{"o":"config_restart"}}
// 或取消 5 分钟自动重启倒计时：
{"c":"system","i":"cr-002","p":{"o":"config_restart","cancel":true}}
// 或直接普通重启（推荐，重启由人决定）：
{"c":"system","i":"cr-003","p":{"o":"restart"}}
```

- `config_restart`（不带 cancel）：ConfigManager 落盘后转交 SystemCommand 安全重启；**落盘后 5 分钟未重启会自动重启**。
- ★ CFG-ROLLBACK 铁律：短时间两次复位且首次未到 `config_boot_validate` ⇒ Backup 覆盖 Active。改配置后遇非计划重启**必须复核配置**；抓 boot 只复位一次。
- ★ `data/` gitignore ⇒ **仓库值 ≠ 设备值**；新模块首启 Active/Backup 都无 ⇒ 写 `{}` 置 loaded。

### 完整示例序列（valve 开阀时长改 8 秒）

```
1. {"c":"system","i":"x1","p":{"o":"config_query","module":"valve"}}        // 查（取第二帧）
2. {"c":"system","i":"x2","p":{"o":"config_set","module":"valve","key":"open_duration_ms","value":8000}}
3. {"c":"system","i":"x3","p":{"o":"config_query","module":"valve"}}        // 复核 RAM 已变
4. {"c":"system","i":"x4","p":{"o":"config_save"}}                          // 落盘
5. {"c":"system","i":"x5","p":{"o":"restart"}}                              // 重启生效
6. 重启后：{"c":"system","i":"x6","p":{"o":"config_query","module":"valve"}} // 复核落盘生效
```

---

## 9. 凭据类（特殊门控）

```json
{"c":"system","i":"c-001","p":{"o":"credential_set","username":"...","password":"..."}}
```

- ★ **仅 `source=="cloud"`（Worker 下发）接受**；MQTT 直发（source=cloud）同样视为 cloud。串口/空来源一律拒。
- 受理帧不含 `confirmed`；结果帧**不回显任何凭据内容**。

---

## 10. 上行帧速查

```json
{"s":0,"c":"result","i":"<回显 cmd_id>","o":"<object>","p":{...},"m":"<消息>","t":<时间戳>,"e":<错误码>,"v":<版本>,"k":<stable_id>}
```

- `s=0` done / 1 accepted / 2 running / 3 FAIL / 4 ERR。
- **config_query 回两帧**：第 1 帧 accepted、第 2 帧 data —— 取第二条。
- `system.restart` / `force_restart`：ack 正常，但 result 常丢失（300ms 复位竞态）⇒ 用 boot_seq 判定。
- ack 帧与 result 帧都在 `/up`：匹配回帧时注意别把 ack 当 result（ack 无业务字段）。

---

## 11. 高频坑清单（违反必得假结论）

1. `i` 30 秒重复 ⇒ 静默丢弃。
2. `v` 不匹配 ⇒ `e=3 s=4`；registry 随模块启停变 ⇒ 禁写死 stable_id。
3. `workflow.get` 参数键是 `id` 不是 `o`。
4. 执行 Workflow 跳过第 0 步。
5. 串口用未压缩键 `cmd/ob/pl`；混用 ⇒ `s=4 Missing 'command' field`。
6. 上行无 `status` 字段 ⇒ 解析必须认 `s`。
7. MQTT 直发不带 `device_id`/`source`；Worker 云 API 必须带。
8. ACL 改完必须重启设备才补建订阅；SUBSCRIBED ≠ 成功。
9. 空闲设备只发 `/log` 不刷 last_seen ⇒ 在线/重启判据用 log_history 增长 + boot_seq。
10. `config_set` ≠ 生效 ≠ 落盘；save 默认会触发重启（云端强制 restart:false 除外——那是 workflow.save）。
