# 技术契约核查（阶段 1 · 只读）

> 日期：2026-10-23 · 对应提交基线 `92f34c1`
> 标注：**[代码确认]** 读代码得到 · **[实测]** 有运行证据 · **[待验证]** 未证实 · **[拟议]** 尚不存在
> 本文件**不修改任何生产代码**，只把「真实行为」写成可验收的契约。
> 上一轮的调查见 `01-investigation.md`；本文件是它的**增量核实 + 新增需求相关补充**，
> 两者冲突时以本文件为准（并注明冲突点）。

---

## 0. 本轮新增核实的 8 个结论（先看这个）

| # | 结论 | 影响的需求 |
|---|---|---|
| 1 | **`workflow_stop()` 全仓无 MQTT/HTTP 入口**，唯一调用点是串口调试命令 `stop N`（`main.cpp:2379`） | §3.1 强制停止**必须**新增命令入口 |
| 2 | **`workflow_stop()` 只改 Workflow 状态，不调用任何物理停止**（不关阀、不停步进、不停振动） | §3.1 不能靠它达成"安全状态" |
| 3 | **Action 描述符只有 `reset/start/poll` 三个回调，没有 `stop`**（`workflow.h`）；`workflow_action_reset()` 仅清 `result/running/runtime` | §3.1 需要新增编排层，不能"给 Action 加个 stop" |
| 4 | 三个执行器**各有现成的物理停止函数**：`valve_force_close()` / `stepper_emergency_stop()` / `vibration_stop()` | §3.1 最小方案成立 |
| 5 | **`system reboot` / `system restart` 都会无限等待 Critical Operation 归零**，不存在"拒绝"分支；也不存在强制重启 | §3.2 与用户预期不符，需新增语义 |
| 6 | **执行器初始化在 `wifi_init()` 之后**（`main.cpp:547` → 556/558/559） | §3.2 要求"网络前初始化安全电平" **当前不满足** |
| 7 | **`valve.safety_timeout_sec` 已经是 Config 参数**（默认 300，钳位 1~300），**不是硬编码常量** | §3.6 只需改默认值 + 加 UI，无需新机制 |
| 8 | Registry 响应含 `version` + `checksum`，**不含 `boot_seq`**；Workflow 侧另有 `workflow.sync_info` 提供 `registry_version/checksum/dirty` | §3.9 同步策略必须基于 version/checksum，不能基于 boot_id |

---

## 1. 命令链路（下行 / 上行 / 回读）

### 1.1 真实链路

```
[浏览器]
   │ POST /api/command        ← Pages Functions，★ 无 session 校验、无 CSRF、无 Bearer
   ▼
[Pages functions/api/command.js]
   │ body.source = "cloud_api"；转发到 Worker 根路径
   ▼
[Worker  src/index.js  (path === "/")]
   │ 校验：source=cloud_api ⇒ 必须 device_id（12 位小写 hex）   [代码确认]
   │ ★ 不校验 ADMIN_API_TOKEN
   ▼
[EMQX]  publish → guo_feeder/<device_id>/down
   ▼
[设备]  MQTT_EVENT_DATA → cloud_transport_dispatch_rx() → CommandManager
   │ 去重（FIFO 10 / TTL 30 s）
   │ 执行 → command_report_result() 发 cmd="result"
   ▼
[EMQX]  guo_feeder/<device_id>/up
   ▼
[Worker  /api/ingest] → mqtt_messages（D1，既有生产表，仓库内无 migration）
   ▼
[UI]  ✘ 无任何端点能读 mqtt_messages 的行        ← 全局缺口 G-1
```

### 1.2 三个缺环（P0）

| 编号 | 缺环 | 证据 | 后果 |
|---|---|---|---|
| **G-1** | 没有只读端点读 `mqtt_messages` 行 | `read-api.js` 白名单仅 4 条；`mqtt_messages` 只在 `handleAdminMetrics` 里被 `COUNT(*)` | UI 无法把「我发的命令」与「设备回帧」关联，只能显示"已下发" |
| **G-2** | `/api/command` 完全匿名 | 见上；`_middleware.js` 默认放行，只保护 `/`、`/app`、`/app.html` | 任何人知道 device_id 即可开阀 / 重启设备 |
| **G-3** | Worker 根路径 `cloud_api` 分支不校验 Bearer | `index.js` 该分支只做 `isValidDeviceId()` | 即使 Pages 加了鉴权，也能**绕过 Pages 直连 Worker** |

> G-2 与 G-3 必须**同时**修，只修一侧等于没修（攻击者选另一条路）。

### 1.3 命令语义分层（必须写进 UI 文案）

| 层 | 证据来源 | 能证明什么 |
|---|---|---|
| L1 请求已接受 | Pages/Worker HTTP 200 + body | 服务端**收到了**这次请求 |
| L2 已发布 | Worker 拿到 MQTT publish 的 QoS 结果 | 消息进了 broker，**不代表设备收到** |
| L3 设备已确认 | 设备 `cmd="ack"`（若该命令有 ack） | 设备解析了命令 |
| L4 已受理/执行终态 | 设备 `cmd="result"` + `status` | 设备**执行完成**（见 §2.3 的状态值） |
| L5 物理已发生 | 传感器/串口证据（如 `DELTA_G`、`VALVE_OPEN_MS`） | 真实世界发生了变化 |

**HTTP 200 只承诺 L1**。这条必须写死在 `05-command-lifecycle.md` 与 UI 文案里。

---

## 2. `cmd_id` 全生命周期

### 2.1 生成

- **[待验证]** 谁生成首个 `cmd_id`：Worker 还是前端。当前 `command.js` 原样透传 body，**不生成**；
  Worker 根路径也不生成（只在缺 `i` 时按设备侧默认处理）。
  ⇒ **[拟议]** 由 Pages 服务端在鉴权通过后生成，前端不参与 —— 这样 `cmd_id` 才可被审计信任。
- 格式：字符串，设备侧按 `String` 比较；长度上限未见显式校验 **[待验证]**。

### 2.2 设备端去重

| 项 | 值 | 依据 |
|---|---|---|
| 缓存 | FIFO **10 条** | `cloud_manager.cpp:59` `MQTT_DUP_CACHE_SIZE 10` |
| TTL | **30 s** | 同上 |
| 命中表现 | **静默丢弃**（无 ack、无 error） | [代码确认] |

⇒ **"重发同一 id"在 30 s 内 = 完全没反应**，与设备离线不可区分。
⇒ 安全重试只能"复用同一 id + 人工判断"，禁止自动重试（见 `05` §幂等）。

### 2.3 结果帧

`command_report_result()` 产出 `cmd="result"`，`status` 可能是
`success` / `failed` / `accepted` / `running` / `rebooting` / `timeout` / `rejected` **[代码确认，逐命令不同]**。
`action_result` / `workflow_result` 走独立 `type`，`command_manager.cpp` 内多处分支。

**关键**：`accepted` 与 `running` **不是终态**，UI 不得把 `accepted` 显示为"成功"。
这是一个已存在的真实陷阱（`05` §3 已列，本轮再确认）。

---

## 3. Registry / Action / Workflow / Config 读取能力

### 3.1 Registry 响应字段（[代码确认]）

```json
{ "registry":"action", "version":4, "checksum":<u32>, "count":14,
  "entries":[ { "stable_id":9, "runtime_id":"VALVE_CLOSE", "name":"...", "params":[...] } ] }
```

- 有 `version` + `checksum` ✅
- **无 `boot_seq` / `boot_id` / `config_version`** ❌
- **无结构化参数约束**（`min/max/default/unit` 均无，只有自由文本 `description`）❌

### 3.2 四类数据的失效条件（§3.9 的基础）

| 数据 | 来源 | 有版本号 | 什么时候会变 | 建议失效条件 |
|---|---|---|---|---|
| **静态参数元数据**（Action/Trigger 名称、参数、单位、范围） | 云端目录（拟议） | 云端自管 `meta_version` | 发版 | 随应用版本走，长期缓存 |
| **设备端能力映射**（stable_id ↔ runtime_id、version、checksum） | 设备 `registry` | ✅ `version`+`checksum` | 新增/删除 Action（如 `weight.enable` 切换）、改 Workflow 定义 | 版本或 checksum 变化 ⇒ 失效 |
| **设备当前配置值** | 设备 `config_query` | ❌ 无版本 | 任意 `config_set`+`config_save` | 每次进入详情页重读；本地不缓存写入结果 |
| **Workflow 内容** | 设备 `workflow.list/get/sync_info` | ✅ `registry_version`+`checksum`+`dirty` | 任何 CRUD + save | `sync_info` 比对后按需细分拉取 |

★ **不要用一个 TTL 解决四类数据**。这是 §3.9 的硬约束。

### 3.3 `workflow.*` 命令完整清单（[代码确认] `workflow_router`）

| 命令 | 作用 | 备注 |
|---|---|---|
| `workflow.sync_info` | 只返回 `registry_version`/`checksum`/`count`/`dirty` | **现成的轻量同步入口**，UI 应优先用它 |
| `workflow.list` | 摘要（slot/id/variant/valid/state/current_step/stable_id） | 按 **slot** 遍历，含逻辑删除对象 |
| `workflow.get` | 单条完整 JSON | |
| `workflow.create` | 新建（占 slot） | `running`/`dirty` 时拒绝 |
| `workflow.set` | **整体替换**（定位用 `p.id`=slot） | 同样受 running/dirty 限制 |
| `workflow.delete` | 逻辑删除（`valid=false`，不清 id） | |
| `workflow.save` | 落盘（`pl.restart` 默认 true） | **save 后必然重启** |
| ❌ `workflow.run` | **不存在** | 执行走 `c="workflow", v, k` |
| ❌ `workflow.stop` | **不存在** | ★ 本轮的 §3.1 缺口 |

### 3.4 Config 能力

| 命令 | 作用 | 证据 |
|---|---|---|
| `config_query` | 读整个模块 | **回两条帧，取第二条**（终帧 `type=config_result`，`data` 在顶层） |
| `config_set` | 改单键（可新增键） | 受 `module_ro` 只读键限制 |
| `config_save` | 落盘 | **成功后设备必然重启** |
| `config_reset` / `config_backup` / `config_delete` / `config_module` | 维护 | 详见 `12-device-detail-and-config.md` |

- 已注册模块 **10 个**：`wifi/oled/valve/time/weight/mqtt/mi_thermo/rtc/stepper/vibration`
- **`data/config/*.json` 在 gitignore 里 ⇒ 仓库值 ≠ 设备实值**（已实测：仓库 `weight.enable=false`，设备 `true`）
- 参数目录见 `12-device-detail-and-config.md` §3（从 `config_manager.h/.cpp` 逐项提取）

---

## 4. Workflow 执行与停止（§3.1 的事实基础）

### 4.1 执行入口与"跳过第 0 步"

- 云端执行：`{"c":"workflow","i":"<id>","v":<ver>,"k":<stable_id>}` → `workflow_start(wf, **true**, ...)`
  ⇒ **跳过第 0 步**（`command_manager.cpp:918`）
- 串口：`wf_start <slot> 0` 从第 0 步开始
- ⇒ 把 `action` 放第 0 步 = 该 action **永不自动执行**，只能手动调（当前 `wf_valve_20g` 就是这么设计的）

### 4.2 `workflow_stop()` 到底做了什么（[代码确认] `workflow.cpp:4657`）

```cpp
workflow_critical_release_by_index(i);   // 释放 Critical Operation
wf.state = WORKFLOW_IDLE;                // 状态归零
wf.current_step = 0; wf.start_time = 0; wf.cmd_id = ""; wf.finish_callback = nullptr;
```

**没有做的事**（全部缺失）：
1. ❌ 不调用 `action->descriptor->...`（也没得调，描述符无 stop）
2. ❌ 不调用 `valve_force_close()`
3. ❌ 不调用 `stepper_emergency_stop()`
4. ❌ 不调用 `vibration_stop()`
5. ❌ 不取消已在跑的 RMT / GPIO 输出
6. ❌ 不上报 `workflow_result`（`finish_callback` 被直接丢弃 ⇒ **云端永远收不到终态**）

⇒ **`workflow_stop()` 是"账本层面的停止"，不是"物理层面的停止"。**
把它直接接到 UI 上会造出"UI 显示已停止、阀门还在出水"的最坏情况。

### 4.3 执行器物理停止函数（都已存在，[代码确认]）

| 执行器 | 函数 | 语义 | 幂等 |
|---|---|---|---|
| 电磁阀 | `valve_force_close()` | 直接写 GPIO 到关闭电平，不经状态判等 | ✅（无状态判等，重复调用安全） |
| 步进 | `stepper_emergency_stop()` | 停 RMT + 禁 EN + 写结果 | ✅（`stepper_finish()` 幂等） |
| 振动 | `vibration_stop()` | 关 GPIO | ✅ |

⇒ **强制停止的物理能力已经齐备，缺的只是"编排 + 命令入口 + 结果核实"。**

### 4.4 运行中 Workflow 的身份识别

- `query_workflows` 返回每个 `wf` 的 `state`（IDLE/RUNNING/WAITING/FINISHED/TIMEOUT/ERROR）+ `current_step` + `stable_id` **[代码确认]**
- `workflow.list` 也返回 `state`/`current_step`/`valid`
- ⇒ "正在运行哪个 Workflow" **可查**，但**没有 `instance_id`** ⇒ 无法区分"同一条 Workflow 的第 1 次和第 2 次运行"

---

## 5. 重启机制（§3.2 的事实基础）

### 5.1 现有状态机（[代码确认] `system_command.cpp`）

```
RESTART_IDLE → (request) → RESTART_REQUESTED
                ↓ 每 loop 轮询 critical_count
         cnt == 0 → RESTART_PENDING（记 pending_since）
                ↓ 等 SYSTEM_RESTART_SAFE_DELAY_MS = 10000 ms
         RESTARTING → ESP.restart()
```

| 事实 | 值 / 行为 |
|---|---|
| `system reboot` 与 `system restart` | **机制完全相同**，只是上报文案不同 |
| Critical Operation 存在时 | **无限等待**（无超时、不拒绝、不告知剩余） |
| 重复请求 | 幂等（`accepted=true`，不重排倒计时） |
| `RESTARTING` 时再请求 | `accepted=false` |
| 全仓唯一 `ESP.restart()` | `system_command.cpp:345`（另有 `config_manager` 的 save 后重启走同一入口）**[待验证：`system_command.cpp:312` 旧记录已变]** |
| 强制重启 | **不存在** |

### 5.2 与用户预期不一致的两点

| 用户要求 | 现状 | 差额 |
|---|---|---|
| "存在 Critical Operation 时**拒绝**重启并说明原因" | 不拒绝，**挂起等待** | 需新增"预检 + 拒绝"语义（或 UI 先查 `restart_status` 再决定，但这不原子） |
| "提供**强制重启**" | 无此能力 | 需新增固件能力（见 `11-emergency-stop-and-restart.md` §4） |

### 5.3 启动顺序（§3.2 第 4 条要求的关键）

`main.cpp setup()` 实测顺序（节选）：

```
480 device_identity_init()
486 log_init()
503 json_storage_init()
524 bin_storage_init()
531 workflow_storage_init()
534 system_state_init()
535 config_init()          ← 配置可用
542 event_manager_init()
546 ble_init()
547 wifi_init()            ← ★ 网络开始
551 workflow_init()
555 weight_init()
556 valve_init()           ← ★ 执行器在此才被配置为输出
558 stepper_init()
559 vibration_init()
560 test_stepper_init()
```

**问题**：从复位到 `valve_init()`（约 200~1500 ms，取决于 WiFi/BLE 初始化耗时），
阀门 GPIO **处于复位默认态（输入/高阻）**，继电器模块的输入若浮空，可能被拉低/拉高到导通侧。
⇒ 用户要求的"**尽可能在网络初始化之前把执行器输出初始化为安全状态**" **当前不满足**。

**[拟议]** 在 `config_init()` 之后、`ble_init()`/`wifi_init()` 之前插入 `safety_outputs_init()`：
按 config 读取 `valve.gpio_pin/active_level`、`stepper.en_pin/en_active_level`、`vibration.relay_pin/active_level`，
立刻 `gpio_config(OUTPUT)` + 写安全电平（阀关、EN 失能、振动关）。**不注册 Action、不启动模块**。
可行性：`config_init()` 早于 `wifi_init()` ✅；这些键都是纯扁平键 ✅。

### 5.4 重启后的"上线"能证明什么

- EMQX `connected` → Worker `/api/presence/event` → `device_event(type='online')` + 刷 `device.last_seen`
- ⚠ presence 语义：**长连接 + 无周期上行时 `last_seen` 会合法老化**（`presence.js` 注释明写）
  ⇒ `online=true, stale=true` 是正常态
- ⇒ **上线事件只证明 MQTT 重连**，不证明执行器安全（用户明确要求，与实现一致）

---

## 6. 执行器在异常情况下的真实输出状态

| 场景 | 电磁阀 | 步进 | 振动 | 证据 |
|---|---|---|---|---|
| 上电/复位瞬间 | **未定义（高阻）** | **未定义（高阻）** | **未定义（高阻）** | [代码确认] §5.3 |
| 进入 `valve_init()` 后 | 关闭 | — | — | `valve_close()` 在 init 内调用 |
| 掉线（MQTT 断） | **保持当前电平** | 保持/跑完 | 保持/定时到时关 | [代码确认] 无掉线联动逻辑 |
| Workflow 超时 | **保持**（无人关） | 30 s 硬上限 | 30 s 硬上限 | 步进/振动有模块级硬上限 ✅；阀**没有**（只有 `safety_timeout_sec`） |
| `workflow_stop()` | **保持** | 保持 | 保持 | §4.2 |
| 普通重启 | 复位后高阻 → init 后关 | 同上 | 同上 | §5.3 |
| `EVENT_WEIGHT_ERROR` | `valve_force_close()`（DispenseGuard） | — | — | [实测] 10-23 强关事故 |

★ **阀是唯一"没有模块级物理兜底且不随 Workflow 结束时机关闭"的执行器**，但它有
`valve_task()` 的 `safety_timeout_ms` 兜底（默认 300 s）。这是 §3.6 要改的那个值。

---

## 7. D1 基线

| 项 | 值 |
|---|---|
| 已应用迁移 | **7/7**，最近 `0007_mqtt_messages_dedup.sql` |
| 下一个编号 | `0008`（本轮**不创建**） |
| `mqtt_messages` | 既有生产表，**仓库内无 migration**（`migrations/README.md` 明示"完全不动"） |
| `device_event.type` | 有 CHECK 白名单（13 种），**不含命令审计类** ⇒ 审计必须新表 |
| `log_history` | `id, device_id, ts, level, code, boot_seq, seq, message, payload, created_at`；`ts`/`created_at` 为**秒** |
| `mqtt_messages.created_at` | **毫秒**（与 log 表不同单位，UI 必须分别处理） |
| `telemetry_history` | 幂等键 `(device_id,type,ts,seq)` |

### 7.1 会话与限流（现状）

- Pages session：`gf_session`，HttpOnly/Secure/SameSite=Lax，**12 h**，无状态 HMAC，**无吊销表**
- 限流：`UI-RATELIMIT-ISOLATE` 未解决（多设备/多标签页会互相挤占）
- ⇒ 阶段 2 必须补：会话吊销、CSRF、限流分桶（见 `06-audit-and-security.md`）

---

## 8. 日志格式（可解码性确认）

- 上行 `payload` = base64( CBOR 12 项定序数组信封 ) ，`LogRecord v2` 128 字节
- `log_history.message` / `code` **100% NULL** ⇒ 可读性必须靠解码 `payload`
- `event_dict_ver` 是 `LOG_RECORD_VERSION` 复用，**不是**字典版本 ⇒ **不能**用来检测码表漂移
- 已有可复用解码器：`docs/scripts/decode_log_history.py`（实测可跑）

---

## 9. P0 缺口清单（阶段 2 的输入）

| ID | 缺口 | 层 | 阻塞什么 | 修法 |
|---|---|---|---|---|
| **P0-A** | `/api/command` 无鉴权 | Pages | 所有控制功能 | session 校验 + CSRF + 服务端生成 cmd_id |
| **P0-B** | Worker `cloud_api` 分支无 Bearer | Worker | 同上（可绕过 Pages） | 强制 Bearer + 拒绝跨源 |
| **P0-C** | 无回帧只读端点 | Worker | 命令状态机、审计、强制停止核实 | `GET /api/history/command?cmd_id=` |
| **P0-D** | 无 `command_audit` 表 | D1 | 审计、Unknown 追踪 | 新增 `0008`（单独评审） |
| **P0-E** | D1 写失败无明确语义 | Worker | 审计失败时是否谎报 | fail-closed，明确 5xx |
| **P0-F** | 服务端无并发去重 | Worker | 同一操作重复下发 | `cmd_id` 唯一约束 + 幂等返回 |
| **P0-G** | `workflow.stop` 不存在 | 固件 | §3.1 全部 | 新增命令 + 安全停堆编排 |
| **P0-H** | 强制重启不存在 | 固件 | §3.2 后半 | 新增能力（需评审） |
| **P0-I** | 启动期执行器引脚未定义 | 固件 | §3.2 第 4 条 | `safety_outputs_init()` |
| **P0-J** | Registry 无参数元数据 | 固件/云端 | Workflow 编辑器表单 | 云端元数据目录 + 漂移检测 |

> P0-A~P0-J 是**阶段 2 的全部范围**。UI 可以并行做原型，但**不得**在没有 A/B/C/F 的情况下
> 把"命令成功"作为可交付行为。

---

## 10. 仍需验证（本轮无法证实）

| # | 事项 | 为什么无法证实 | 怎么验证 |
|---|---|---|---|
| V-1 | `cmd_id` 的实际上限长度 | 设备侧未做显式校验 | 构造 64/128 字符 id 看是否被拒 |
| V-2 | `mqtt_messages` 的完整列 | 表在线上，仓库无 migration | `wrangler d1 execute --remote "PRAGMA table_info(mqtt_messages)"` |
| V-3 | `workflow_stop()` 后 `finish_callback` 丢失是否已被云端感知 | 无 stop 入口，从未触发过 | 新增入口后实测 |
| V-4 | 强制重启会不会破坏 `config` 的 Active/Backup | 无该能力 | 只读分析 + 空载测试 |
| V-5 | 继电器模块在 GPIO 高阻时的实际导通行为 | 需要万用表/示波器 | 硬件测量（**用户执行**） |
| V-6 | 步进/振动真机行为 | DRV8825 损坏 | 等替换件 |
| V-7 | `system_command.cpp:312` 与 `345` 的 `ESP.restart()` 归属 | 行号随提交漂移 | 当前基线：**唯一**在 `345` |
