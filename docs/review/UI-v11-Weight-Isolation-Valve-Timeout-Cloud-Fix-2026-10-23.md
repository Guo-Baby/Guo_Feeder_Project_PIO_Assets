# UI v1.1 第三轮：Weight 隔离 · 阀门 60 s 超时 · 云端协议修复 · 联合测试

> 固件：`PIO_Assets/Guo_Feeder_Project` @ `wb`
> 云端：`Cloudflare_Assets` @ `main`
> 设备：ESP32-S3 N16R8 / COM8 / device_id `288485896ce4`
> 时间：2026-10-23 22:50 ~ 23:20

---

## 0. 提交与仓库状态

| 仓库 | 起始 | 结束 | 分支 | 工作区 |
|---|---|---|---|---|
| 固件 | `74a6c36` | **`b23b7a7`** | `wb` | 仅 3 个历史遗留未跟踪文档 |
| 云端 | `78c6bec` | **`106eb1b`** | `main` | 仅历史遗留 `scripts/enforce_watch.sh` |

---

## 1. Weight 模块隔离（任务 §1）

### 1.1 变更过程（全部走 ConfigManager，未碰文件系统、未改源码默认值）

| 步骤 | 结果 |
|---|---|
| 读取现值 | `weight.enable = true` |
| `config_set weight enable=false` | `s=1 "config set queued, restart required"` |
| `config_save` | `s=1 "config save queued"` → 串口 `config saved` + `rst:0xc` |
| 重启后回读 | **`weight.enable = false`** ✅ |
| 启动日志 | `[Weight] Disabled by config (HX711 not installed) -> weight_decrease / WEIGHT_ZERO NOT registered, no weight events emitted` |

### 1.2 隔离有效性（同一会话内前后对照）

| 窗口 | `[DispenseGuard] Weight error received` | `[Valve] FORCE CLOSE` |
|---|---:|---:|
| 改配置**之前**那次启动（0~36.9 s） | 4 | 4 |
| 改配置**之后**那次启动（41.0 s 起，约 40 s 观察） | **0** | **0** |

⇒ 关闭 Weight 后，DispenseGuard 不再因缺失 HX711 反复强关阀门，阀门保持关闭，MQTT 正常。

### 1.3 侧证：registry 版本随模块启停变化

| 状态 | action registry |
|---|---|
| weight 关闭 | `v=5 n=13`（`WEIGHT_ZERO` / `weight_decrease` 未注册） |
| weight 恢复 | `v=6 n=14` |

⇒ 印证"**不能写死 stable_id**"。本轮所有测试脚本都是先查 registry 再构造 `v`+`k`。

### 1.4 恢复（用户追加要求）

测试全部通过后恢复：`weight.enable=true` → 重启 → **回读 `true`**，串口重新出现
`[Weight] Init OK (dt=6 sck=7 scale=741.0 …)`；`FORCE CLOSE=6` 随之回到原状态
（HX711 仍未接，属既有行为，不是本轮引入）。

**注意**：关闭 Weight 只是**隔离测试条件**，不代表 HX711 或重量闭环通过验收。

---

## 2. 阀门 60 秒安全超时（任务 §2）

### 2.1 前置条件（脚本自动检查，不满足则拒绝开阀）

`weight.enable=false` ✔ · `safety_timeout_sec=60` ✔ · 未锁存 ✔ · 阀门关闭 ✔ ·
无 RUNNING/WAITING 的 Workflow ✔

### 2.2 实测

| 项 | 值 |
|---|---|
| 开阀命令 | `VALVE_OPEN`，`s=1 "Action queued"` |
| 超时阈值（设备自报） | `[Valve] Safety timeout! Forced close (open > **60000** ms)` |
| **实际开启时长** | **60.00 s**（串口时间戳 `[Valve] OPEN` → `Safety timeout` 差值） |
| 关阀触发源 | **`Safety timeout` 分支**（`valve_task()` 内的超时判定） |
| 是否被 DispenseGuard 抢先 | **否**（超时前 400 字符内无 `FORCE CLOSE`） |
| 超时后 `valve_status` | `false` |
| 关阀后是否被重新打开 | **否**（全程 `[Valve] OPEN` 仅 1 次） |
| 结果 | **13 / 13 PASS** |

### 2.3 结论

**PASS** — 能证明是 60 秒安全超时逻辑本身触发关阀且关阀成功，不是其他保护路径。

### 2.4 附带发现（可观测性缺口）

`VALVE_OPEN` 这个 Action **即时完成**（`s=0` 在开阀瞬间就返回），
60 s 后的超时关阀**没有与该命令关联的回帧**，只有：
- 串口 `[Valve] Safety timeout! …`
- 日志通道 `LOG_VALVE_SAFETY_TIMEOUT`（WARN，含 `LOG_P_OPEN_MS` / `LOG_P_LIMIT_MS`）

⇒ 云端/UI 要显示"这次开阀是被超时关掉的"，必须读 **日志**，不能只看命令审计。
这是设计现状，未在本轮修改（改动会触及 valve 与 CommandManager 的职责边界）。

---

## 3. 云端 P0 修复：`stateFromFrame` 读取 `s`（任务 §3）

### 3.1 缺陷（真机帧证实）

设备 `/up` 全量经过 `cloud_compress_uplink()`：`status` → **整数 `s`**。
线上帧**没有 `status` 字段**，而旧实现只读 `msg.status || msg.st`
⇒ **每一次执行结果（含成功）都落 `unknown`**。

真机原文（`docs/evidence/UI-v11-OnDevice-2026-10-23/`）：

```json
{"s":0,"c":"result","i":"wbt3-00","o":"valve_status","t":1792765338,"value":"false","command":"query_state"}
{"s":4,"c":"result","i":"wbt3-22","t":...,"m":"Missing 'id' (workflow slot)","e":3,"type":"command"}
```

### 3.2 修复

- `COMPACT_STATUS_CODE`：`0→done 1→accepted 2→running 3|4→failed`
- 优先级 `s` > `status` > `st`；`c` / `cmd`、`i` / `id` 两种写法都认
- 未知整数（如 `9`）⇒ `unknown`，**不伪装成功**
- ACK 分支最高优先（ack 帧无 `s`）⇒ 仍 `acked`
- 调用点由 `stateFromFrame({c, cmd, status})` 改为 `stateFromFrame(msg)`
  （这样 `s` 与 `o` 都能拿到）

### 3.3 测试

| 层 | 用例 | 结果 |
|---|---|---|
| 单元映射 | `H1-1`~`H1-13`（s=0/1/2/3/4/9、ack、legacy status/st、重启族） | 全 PASS |
| 状态机 | `H2-1`~`H2-5`（accepted→done 允许，done→running/acked 禁止） | 全 PASS |
| **完整 Worker 路径** | `H3-1`~`H3-7`：`POST /api/ingest` → 解析 → `stateFromFrame` → `markCommandState` | 全 PASS |
| **真实设备端到端** | `cloud_e2e_local.py`：真实回帧 → `wrangler dev` → **真实本地 D1** | **11 / 11 PASS** |

真实端到端证据（`docs/evidence/UI-v11-Round3-2026-10-23/cloud_e2e.json`）：

```
E1-1 提交只读查询 → 202 published，executed=false verified=false
E2-1 设备真实回帧 {"s":0,"c":"result","i":"e2e92768205",...}
E3-1 ingest 200 OK
E3-2 审计状态 → done（terminal=true）
E3-3 first_frame_at 已落
```

> 生产 D1 没有 `command_audit` 表，这条链只能在本地区 D1 上跑（见 §6）。

---

## 4. 其余三项修复（任务 §4 / §5 / §6）

### 4.1 `workflow.get` 参数键 `o` → `id`

| 项 | 结果 |
|---|---|
| 真机实测 | `p.o` ⇒ `s=4 "Missing 'id' (workflow slot)"`；`p.id=3` ⇒ `s=0` |
| 目录 params | 改为 `{name:"id", type:int, min:0, max:15, required:true}` |
| 组帧 | `buildQueryFrame` workflow 分支输出 `p.id` |
| 缺 id / 传旧 o / 越界 99 | 一律 400 `invalid_params`，**不做 slot 兜底**（H4-3~H4-5） |
| 前端调用点 | `guo-feeder-pagesdev` 与 `app.html` **无任何 workflow.get 调用**（N/A） |

### 4.2 `config_query` 两帧 + 有界等待

| 用例 | 结果 |
|---|---|
| 第一帧 `s=1` → `accepted`（非终态，不当作结果） | H5-1 PASS |
| 窗口内 `effective_state=accepted`，不误报超时 | H5-2 PASS |
| 第二帧 `s=0`（带 data）→ `done` | H5-3 PASS |
| 重复第二帧不回退 | H5-4 PASS |
| **第二帧缺失** → `effective_state=timeout` + `stale=true`，且**不改库** | H5b-1~3 PASS |

窗口按 family：`query 20 s` / `safety 60 s` / `config|system 90 s` / `action|workflow 630 s`
（对齐固件 600 s 超时，不早于设备喊超时）。

前端 `gf-api.js::getCommand()` 把 `state` 归一为 `effective_state`，原值留在 `raw_state`
⇒ UI 不会把"第一帧 accepted"当成"配置为空"。

### 4.3 重启命令状态语义

| 命令 | 设备实际 | 云端映射 |
|---|---|---|
| `system.restart` | `status="success"`（线上 `s=0`） | **`accepted`**（不是 done） |
| `system.reboot` | `status="rebooting"`（`s=2`） | `accepted` |
| `system.force_restart` | 常常无回帧（300 ms 后复位） | 保持非终态 |

- `isRestartObject()` 同时认 `restart` 与 `system.restart`（前缀兼容）
- `GET /api/ui/command/:id` 对重启族返回 `rebooting: true` + 说明文案
- 设备重新上线**不**被当作"命令成功"（H6-4 守住）

---

## 5. 急停锁存白名单收窄（任务 §7）

### 5.1 变更

白名单 7 项 → **5 项**（只保留停止类）：

```
保留：VALVE_CLOSE / STEPPER_EMERGENCY_STOP / TEST_STEPPER_STOP /
      VIBRATION_STOP / MI_THERMO_STOP_SCAN
移除：WEIGHT_ZERO（置皮重会让"秤是否被改动"不可判定）
      MI_THERMO_START_SCAN（启动持续扫描不是停止动作）
```

判据只有一条：**这个 Action 会不会让系统离开"更停"的状态**。会 ⇒ 拒绝。

### 5.2 真机复验（镜像 `014c1f3e…56965e`，1 450 128 B）

| 用例 | 结果 |
|---|---|
| 锁存中 `VALVE_OPEN` | 拒绝（`s=4`，文案含 latch） |
| 锁存中 `STEPPER_ROTATE_STEPS` | 拒绝 |
| 锁存中 `VIBRATION_START` | 拒绝 |
| 锁存中 `TEST_STEPPER_START` | 拒绝 |
| 锁存中 **`MI_THERMO_START_SCAN`** | **拒绝**（本轮新增，原来是放行的） |
| 锁存中 **`WEIGHT_ZERO`** | **拒绝**（本轮新增；weight 恢复后复验） |
| 锁存中 `VALVE_CLOSE` | 允许 |
| 锁存中 Workflow 启动 | 拒绝 |
| `safety.clear` | `was_latched=true → latched=false` |
| clear 后 Workflow 状态 | 全部 `IDLE`（无自动恢复） |
| 解锁后显式动作 | 恢复受理 |

**16 / 16 PASS**。

### 5.3 串口调试例外（必须在文档与风险里写明）

`tstep_start` / `wf_start` 等**串口调试命令绕过锁存**（不经过 CommandManager）。
- 本轮**故意不改**（现场排障需要）
- 但它们**不得**通过 UI / MQTT 暴露成正常操作入口
- 走 `cm {json}` 的串口命令**已被覆盖**（它进 CommandManager → `command_execute_action`）

---

## 6. 部署（任务 §8）

### 6.1 部署前检查

| 项 | 结果 |
|---|---|
| 云端 HEAD / 分支 / 工作区 | `106eb1b` / `main` / 仅遗留未跟踪文件 |
| 三项修复已提交并通过测试 | ✅ 244/244 |
| Worker dry-run | 242.87 KiB（gzip 61.96 KiB） |
| Pages functions build | `Compiled Worker successfully` |
| 同一组候选代码 | ✅ 两仓同一次提交内 |

### 6.2 D1 迁移前置条件检查（**关键**）

生产 D1 现有表：`device` `device_binding` `device_credential` `device_event`
`log_boot_watermark` `log_boot_watermark_audit` `log_conflict` `log_history`
`mqtt_messages` `telemetry_history`。

**缺失**（`0008` 的全部内容）：

| 对象 | 用途 | 缺了会怎样 |
|---|---|---|
| `ui_session` | 可撤销会话 | `/api/ui/session` → 503 |
| `command_audit` | 命令审计 | `/api/ui/command` → 503（fail-closed） |
| `safety_event` | 急停/强重启证据链 | safety 审计缺失 |
| 8 个索引 | 查询效率 | — |

⇒ **按任务 §8.2，未应用迁移**。已用 `--local` 在**本地 D1** 应用并验证：
3 表 + 8 索引 + `UNIQUE(cmd_id, device_id)` 全部创建成功（**不碰生产**）。

> 另发现：`0008` **不含** `device.alias` 列（此前设计稿里有）。
> 设备别名若要用，需要新增迁移或单独表。

### 6.3 部署结果

| 项 | 值 |
|---|---|
| Worker | `https://guo-feeder-api.guobaby.workers.dev` |
| **Worker Version ID** | **`0cf8b297-d9cc-4abe-85c7-43e5d0fd3023`** |
| Worker 体积 | 242.87 KiB（gzip 61.96 KiB），Startup 3 ms |
| Pages | `https://a21294de.guo-feeder-pagesdev.pages.dev`（项目 `guo-feeder-pagesdev`，分支 main） |
| Pages 构建 | `Compiled Worker successfully` + `Uploading Functions bundle` ✔ |
| 部署时间 | 2026-10-23 23:0x（GMT+8） |

### 6.4 部署后验证（16 / 16 PASS）

| 用例 | 结果 |
|---|---|
| 无 Bearer / 错 Bearer ⇒ 401 | PASS |
| 正确 Bearer ⇒ 200，能力目录 22 query + 14 action | PASS |
| `/api/ui/session` issue ⇒ **503 `session_store_unavailable`**（fail-closed，不降级放行） | PASS |
| `/api/ui/command` ⇒ **503 `audit_unavailable`** | PASS |
| **确认没有下发到设备**（8 s 内无该 cmd_id 回帧） | PASS |
| 真实压缩帧 ingest ⇒ 200；错 webhook key ⇒ 401 | PASS |
| 只读端点（telemetry / snapshot / metrics）200；无 Bearer 401 | PASS |
| 无会话访问 `/app.html` ⇒ 302；静态资源 200 | PASS |
| 无会话提交命令 ⇒ 401 | PASS |

`/api/auth/session` 返回 `revocable:false`（当前 store 为本地模式，见 §8）。

### 6.5 因为缺 `0008` 而 BLOCKED 的项目

- 生产端 `stateFromFrame` 的**审计落库**（本地已验，生产不可验）
- `workflow.get` / `config_query` / 重启语义的**生产端**状态推进
- 可撤销会话 / 登出即失效
- 限流的真实分布（per-isolate 内存）

---

## 7. 结果汇总（PASS / FAIL / BLOCKED / NOT VERIFIED）

| 组 | PASS | FAIL | BLOCKED | NOT VERIFIED |
|---|---:|---:|---:|---:|
| 固件：Weight 关闭 + 隔离 | 7 | 0 | 0 | 0 |
| 固件：阀门 60 s 超时 | 13 | 0 | 0 | 0 |
| 固件：锁存白名单（weight 关） | 15 | 0 | 1 | 0 |
| 固件：锁存白名单（weight 恢复后复验） | 16 | 0 | 0 | 0 |
| 固件：Weight 恢复 | 7 | 0 | 0 | 0 |
| 云端：单元/集成测试 | 244 | 0 | 0 | 0 |
| 云端：既有回归（pages_auth / webhook） | 111 | 0 | 0 | 0 |
| 云端：真实帧本地端到端 | 11 | 0 | 0 | 0 |
| 生产部署后验证 | 16 | 0 | 0 | 0 |

**合计 440 PASS / 0 FAIL。**
`BLOCKED` 1 条（weight 关闭时 `WEIGHT_ZERO` 未注册），已在恢复 weight 后补测为 PASS。

---

## 8. 仍未验证 / 风险

| # | 项 | 状态 |
|---|---|---|
| 1 | 生产端命令审计（缺 `0008`） | **BLOCKED**，需迁移授权 |
| 2 | 可撤销会话（缺 `0008`） | **BLOCKED**，当前 `revocable:false`（登录可用但登出不能即时失效） |
| 3 | GPIO 电气测量（万用表） | **NOT VERIFIED**（本轮未做） |
| 4 | 步进电机（DRV8825 损坏） | **BLOCKED** |
| 5 | `GET /api/ui/command/:id` 不带 `device_id` | 多设备歧义（沿用，未修） |
| 6 | `purgeOldCommands` 删 `requesting` | 未结束态保留策略未定（沿用） |
| 7 | `ix_ca_retain(state, request_at)` | 索引效率未由真实查询计划证实 |
| 8 | 串口调试命令绕过锁存 | 已知例外，见 §5.3 |
| 9 | 超时关阀无命令回帧 | 设计现状，见 §2.4 |
| 10 | HX711 `weight_valid=true` 假阳性 | 已知缺陷（`scale.read()` 返回值未校验） |

---

## 9. 回滚

| 对象 | 方式 |
|---|---|
| 固件 | `git revert b23b7a7` 后重新构建烧录；或烧回上一版 `firmware.bin`（sha256 `9264fe3b…`） |
| 设备配置 | `config_set weight enable true` + `config_save`（当前已是 true）；`valve.safety_timeout_sec` 若要回 300 同理 |
| Worker | `wrangler rollback --config guo-feeder-api/wrangler.toml`（回到上一 Version） |
| Pages | 在 Cloudflare 控制台回退到上一 Deployment |
| D1 | 本轮**未应用任何迁移**，无需回滚 |

---

## 10. 设备最终状态

| 项 | 值 |
|---|---|
| 固件 | `b23b7a7` 构建（sha256 `014c1f3e…56965e`） |
| `weight.enable` | **true**（已恢复，回读确认） |
| `valve.safety_timeout_sec` | 60 |
| 阀门 / 振动 | 关闭 / 停止 |
| 急停锁存 | 已解除 |
| MQTT | 在线 |
