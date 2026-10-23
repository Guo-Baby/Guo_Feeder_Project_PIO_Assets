# UI v1.1 第四轮报告：生产 D1 迁移、生产验收与遗留风险收尾

> 时间：2026-10-23 23:34 ~ 00:25（GMT+8）
> 固件 `PIO_Assets/Guo_Feeder_Project` @ `wb`
> 云端 `Cloudflare_Assets` @ `main`
> 设备：ESP32-S3 N16R8 / COM8 / `288485896ce4`

---

## 0. 先说一件必须记录的事故（我造成的，不是产品缺陷）

在复核"串口调试命令是否绕过急停锁存"时，我向串口发了
`cm {"c":"safety","i":"…","p":{"o":"stop_all"}}`（**压缩键**）。
串口控制台用的是**未压缩键** `{"cmd":…,"ob":…}`，于是固件回
`s=4 "Missing 'command' field"` ⇒ **锁存根本没置上**。

我随后发的 `wfc run 3 1` 因此合法执行 —— slot 3 是 `wf_valve_20g`（供水流程），
**阀门被打开了**。发现后立刻 `safety.stop_all` ⇒ `valve.note = force_closed`，
复核 `valve_status=false`、4 个 Workflow 全部 `IDLE`。

- 实际放水时长：脚本内 3 s + 我处置前的往返（约十余秒），按 21 g/45.4 s 的实测流速估算 **< 10 g**，由现场容器承接。
- 违反的是我自己给自己定的边界（"不执行与本轮验收无关的危险设备命令"）。
- 修复：把"先断言 `latched=true` 才继续发命令"写进脚本，并且**不再发任何 `wfc run`**。

⇒ 这条教训同时产出了一条通用结论：**串口 `cm` 与 MQTT 用不同的键**，混用会静默失败。

---

## 1. 执行前检查

| 项 | 值 |
|---|---|
| 云端分支 / HEAD | `main` / `e6c05b8`（= 远端 `origin/main`） |
| 固件分支 / HEAD | `wb` / `6888e7d`（= 远端） |
| 工作区 | 云端仅遗留 `scripts/enforce_watch.sh`；固件仅 3 个历史遗留文档 |
| 生产 Worker | `guo-feeder-api.guobaby.workers.dev`（上一版 `0cf8b297…`） |
| 生产 Pages | `guo-feeder-pagesdev.pages.dev` |
| **生产 D1** | `guofeeder` / uuid `0d8385ba-a1a6-4fcc-8e43-ae61c014c43d` —— **与 `wrangler.toml` 的 `database_id` 一致** ✅ |
| 迁移账本 | `d1_migrations` 已有 0001~0007 ⇒ `migrations apply` 只会跑 0008 |
| `0008` 内容核对 | 3 张表 + 8 个索引 + `UNIQUE(cmd_id, device_id)`，全部 `IF NOT EXISTS`，**无 DROP / 无 ALTER / 无 device.alias** —— 与评审稿一致 |
| 备份 | `Cloudflare_Assets/backups/guofeeder-pre0008-20261023.sql`，8 203 768 B，sha256 `5154a753…40d537b`（含 11 个 CREATE TABLE + 16 573 条 INSERT） |
| 既有用例可复现 | 是（后续实测 273 / 89 / 22 全通过） |

---

## 2. 生产 D1 迁移 `0008`

### 2.1 执行

```
cd guo-feeder-api && npx wrangler d1 migrations apply guofeeder --remote
→ Migrations to be applied: 0008_ui_command_audit.sql
→ Executed 12 commands in 2.32ms
→ 0008_ui_command_audit.sql  ✅
```

12 = 3 × CREATE TABLE + 8 × CREATE INDEX + 1 × 账本写入。

### 2.2 核验

| 项 | 结果 |
|---|---|
| `ui_session` / `command_audit` / `safety_event` | ✅ 三张表存在 |
| 8 个索引（`ix_us_exp` `ix_us_actor` `ix_ca_dev_time` `ix_ca_state` `ix_ca_actor` `ix_ca_retain` `ix_se_dev_time` `ix_se_kind`） | ✅ 8/8 |
| `UNIQUE(cmd_id, device_id)` | ✅ 实测：第二次插入返回 `UNIQUE constraint failed: command_audit.cmd_id, command_audit.device_id`（测试行已删除，残留 0 行） |
| `d1_migrations` | 7 → **8** 行 |

### 2.3 幂等性

- 再跑 `migrations apply --remote` ⇒ **`No migrations to apply!`**（账本生效）
- 把 DDL 文件直接 `--file` 再跑一次 ⇒ 全部 `success`，表数仍 3、索引仍 8（结构不变）

### 2.4 数据完整性（迁移前 → 后）

| 表 | 迁移前 | 迁移后 | 判定 |
|---|---:|---:|---|
| device | 10 | 10 | ✅ 不变 |
| device_binding | 1 | 1 | ✅ 不变 |
| device_credential | 16 | 16 | ✅ 不变 |
| device_event | 385 | 385 | ✅ 不变 |
| log_boot_watermark | 1 | 1 | ✅ 不变 |
| log_boot_watermark_audit | 1 | 1 | ✅ 不变 |
| log_conflict | 1661 | 1730 | 追加型，+69（设备在线） |
| log_history | 5408 | 5546 | 追加型，+138 |
| mqtt_messages | 7844 | 7844 | ✅ 不变 |
| telemetry_history | 1217 | 1219 | 追加型，+2 |

**无任何表行数下降** ⇒ 迁移是纯加法，没有触碰既有业务数据。

---

## 3. 生产端到端验收（28 / 28 PASS）

链路：`POST /api/ui/command` → 生产 Worker → 真实 EMQX → 真机 → 设备回帧
→ EMQX 数据集成 → 生产 Worker `/api/ingest` → **生产 D1**。

| 用例 | 输入 | 实际结果 | 结论 |
|---|---|---|---|
| A1-1/2 | 只读 `workflow.sync_info` | `202`，`published=true`，`executed=false verified=false` | PASS（不谎称已执行） |
| A1-3/4 | 设备真实回帧 | `state=done`，`first_frame_at`/`finished_at`/`duration_ms=14000` 已落 | PASS |
| A2-1/2 | 注入迟到 `s=2` 帧 | 终态 `done` **未被覆盖** | PASS（状态机单调） |
| A3-1/2 | `workflow.get` `params.id=3` | 真机 `done` | PASS |
| A3-3 | `params.o=3`（旧字段） | **400 `invalid_params`**，不做 slot 兜底 | PASS |
| A4-1/2/3 | `config_query module=valve` | 早期观察到 `published`→`acked`，第二帧后 `done`；两帧关联同一条审计 | PASS |
| A5-1/2 | 状态查询 | 带 `device_id` 精确命中；不带时单设备 200 / 多设备 409 | PASS |
| **A6-1/2/3/4** | **真实 `system.restart`** | 设备**已重新上线**，命令仍 `state=accepted`、`terminal=false`、`rebooting=true` | **PASS（受理 ≠ 完成）** |
| A7-1/2 | `safety.stop_all` | 命令 `done`；`safety_event` 落 `kind=estop, outcome=published` | PASS |
| B-1 | `session issue` | 200，D1 落 `ui_session` 行 | PASS |
| B-2 | `verify`（新会话） | 200 `ok:true` | PASS |
| B-3 | `revoke` | 200 `revoked:true` | PASS |
| **B-4** | `verify`（撤销后） | **401 `ok:false`** | **PASS（登出即失效的真实判据）** |
| B2-1/2/3 | 无 Bearer / 错 Bearer / 无效会话 | 401 / 401 / 401 | PASS（不降级放行） |
| C-1 | `safety.clear` | 202，锁存解除 | PASS |

### 3.1 生产 D1 里的真实审计行（抽样）

```
cmd_id=prd772360913 family=safety action_id=safety.stop_all      state=done
cmd_id=prd671853506 family=system action_id=system.restart       state=accepted   ← 关键
cmd_id=prd472306551 family=query  action_id=system.config_query  state=done
cmd_id=prd371835097 family=query  action_id=workflow.get         state=done
```

终态分布：`done 19` / `accepted 2`（两条都是 `system.restart`）。
`safety_event`：**1 行**，`outcome=published`（**不是 success**）。
`ui_session`：3 行，**全部 `revoked_at` 非空**（撤销生效）。

---

## 4. 本轮新发现并修复的问题（不在原清单里）

| # | 现象 | 根因 | 修复 |
|---|---|---|---|
| N1 | **急停后只能靠重启解锁** | 目录缺 `safety.clear`，且 `buildDeviceFrame` 的 safety 分支把 `p.o` 写死 `stop_all` | 目录补 `safety.clear`；`p.o` 由 actionId 推出；未知 safety 动作返回 `null`（不回落 stop_all） |
| N2 | **重启完成就被标 done** | 固件 `command_system_restart()` 只设 `status`，**不设 object** ⇒ 压缩帧没有 `o` ⇒ `isRestartObject(frame.o)` 为 false | 改用**库里 action_id** 判定 |
| N3 | **`safety_event` 表建了但没有任何写入方** | 从来没有代码引用这张表 | 新增 `recordSafetyEvent()`，只记 `published`；失败只记日志，**不阻塞急停** |

---

## 5. 遗留风险逐项结果

| # | 问题 | 结果 | 说明 |
|---|---|---|---|
| 1 | 状态查询跨设备歧义 | **已修复 PASS** | 409 `ambiguous_command` + 候选列表；H8-1~H8-5 |
| 2 | `purgeOldCommands` 删 `requesting` | **已修复 PASS** | 白名单只删 `done/failed/rejected`；H9-1~H9-4 |
| 3 | `ix_ca_retain` 效率 | **已修复 PASS（生产实证）** | `SEARCH … USING INDEX ix_ca_retain` vs 旧版 `SCAN` |
| 4 | 串口调试命令绕过锁存 | **结论被推翻 → 已覆盖 PASS** | 见 §5.4 |
| 5 | 阀门超时关阀缺关联回帧 | **BLOCKED（需设计/授权）** | 见 §5.5 |
| 6 | HX711 `weight_valid` 假阳性 | **BLOCKED（需硬件）** | 见 §5.6 |

### 5.4 串口调试命令（真机证明，6/6 PASS）

| 用例 | 结果 |
|---|---|
| 串口 `cm` 置锁存 | `latched=true` |
| 锁存中 `execute_action(VIBRATION_START)` | **拒**：`[Safety] action blocked by latch: VIBRATION_START` |
| 锁存中 `execute_action(TEST_STEPPER_START)` | **拒**（`tstep_start` 走同一闸门，main.cpp:909） |
| 锁存中 `VALVE_CLOSE` | 允许（停止类） |
| `clear` | `was_latched=true → latched=false` |

`wfc run` → `workflow_start()`（main.cpp:2437），其拦截点已由
**MQTT 侧 T4-07（锁存中 Workflow 被拒）** 与本次代码定位共同证明。
（本轮**没有**再发 `wfc run`，原因见 §0。）

### 5.5 阀门超时关阀缺关联回帧 —— BLOCKED

**根因**：`VALVE_OPEN` 这个 Action 是**即时完成**的（开阀瞬间就返回 `s=0`），
`valve_task()` 60 s 后的超时关阀只产生串口日志 + `LOG_VALVE_SAFETY_TIMEOUT`，
**不产生与该命令关联的回帧**。

**为什么不能"顺手改"**：`WorkflowActionInstance` **不含 cmd_id**
（只有 descriptor / id / params / result / running / runtime / callback），
所以要把 cmd_id 送到 valve 模块必须改这个结构或新增 API —— 属于架构改动，不在本轮授权内。

**两个候选方案（待裁决）**

- **方案 A（改结构）**：给 `WorkflowActionInstance` 增加 `cmd_id`，
  `valve_open_action` 存下它；超时关阀时补发一条 `{"c":"result","i":<cmd_id>,…}`
  并带 `reason=safety_timeout`。需新增字段语义。
- **方案 B（零协议变更，推荐）**：超时关阀时发一条**独立**的新 cmd_id 告警帧
  （`type="alert"`），云端按 `type=alert` 归类为"阀门告警"，
  **不**去覆盖原命令的终态（避免把"被超时关掉"伪装成"原命令成功"）。

### 5.6 HX711 `weight_valid` 假阳性 —— BLOCKED

**现状**：HX711 未接（DOUT 浮空）时仍会产出"看起来有效"的读数，
固件 `weight_task()` 因此把 `STATE_WEIGHT_VALID` 置 `true`。

**最小方案**：要求连续 N 次（如 3 次）`is_ready() && read()` 成功才置 valid，
并对异常跳变判为错误。

**为什么不做**：**没有 HX711 就无法验证"不破坏正常称重"** ——
一个未经验证的判定阈值可能把真实重量误判为无效，那比现在的假阳性更糟。
⇒ 必须等硬件到位后验证。再次强调：**关闭 Weight 只是隔离手段，不是这个问题的修复**。

---

## 6. 代码变更与提交

### 6.1 云端（`Cloudflare_Assets` @ `main`）

| 提交 | 内容 |
|---|---|
| `769e038` | 状态查询禁止跨设备误归属 + 清理只删终态（顺带让 `ix_ca_retain` 可用） |
| `fa0b21a` | 补 `safety.clear`（目录 + 组帧） |
| `995e9c7` | 重启族判据改用库里 action_id |
| `b1396eb` | `safety_event` 写入方 |
| 文档 | `docs/UI-v1-Phase2-API-Contract.md` §14 |

### 6.2 固件（`Guo_Feeder_Project` @ `wb`）

| 提交 | 内容 |
|---|---|
| （本轮） | `t_serial_latch.py`（串口锁存复核）+ `prod_e2e.py`（生产端到端）+ 证据归档 |

固件**源码本轮未改**（第三轮 `6888e7d` 的镜像仍在设备上运行）。

### 6.3 测试总数

| 套件 | 结果 |
|---|---|
| `tests/api/ui_security.test.mjs` | **273 / 273 PASS** |
| `tests/pages/pages_auth.test.mjs` | **89 / 89 PASS** |
| `tests/pages/pages_webhook.test.mjs` | **22 / 22 PASS** |
| 生产端到端 | **28 / 28 PASS** |
| 串口锁存复核 | **6 / 6 PASS** |

**合计 418 PASS / 0 FAIL。**

---

## 7. 部署与生效版本

| 项 | 值 |
|---|---|
| **Worker 生效 Version ID** | **`f3dc2360-2fee-4951-9852-007cbcbbea03`** |
| Worker 源码提交 | `b1396eb` |
| Pages 部署 | `https://a5843fd5.guo-feeder-pagesdev.pages.dev` |
| Pages Functions | `Compiled Worker successfully` + `Uploading Functions bundle` ✅ |
| Pages 新增变量 | `SESSION_STORE=d1`（可撤销会话） |
| 迁移失败 / 部署失败 | **无**，未执行任何恢复操作 |

版本演进（本轮内）：`0cf8b297` → `d6d7867c` → `633664a7` → `a5d14bdd` → **`f3dc2360`**（最终）。

---

## 8. 设备最终状态

| 项 | 值 |
|---|---|
| 固件 | `6888e7d`（sha256 `014c1f3e…56965e`） |
| `weight.enable` | **true**（未被当作规避手段长期关闭） |
| `valve.safety_timeout_sec` | 60 |
| 阀门 / 振动 | 关闭 / 停止 |
| 急停锁存 | 已解除 |
| MQTT | 在线 |
| `weight_valid` | true —— **HX711 未接的已知假阳性，未修** |

---

## 9. 最终验收结论

| 项目 | 结论 |
|---|---|
| 生产 D1 迁移 `0008` | **PASS**（三表 + 八索引 + UNIQUE + 幂等 + 数据零损失） |
| 生产命令审计链 | **PASS**（创建 / 发布 / 真实回帧 / 终态 / 迟到帧防回退） |
| 生产状态推进（`s` 映射、`workflow.get`、`config_query`） | **PASS** |
| 重启族不确定语义 | **PASS**（设备重新上线后仍是 `accepted`） |
| 会话签发 / 撤销 / 登出即失效 | **PASS**（撤销后 verify 401） |
| 鉴权闸门（无/错/无效） | **PASS** |
| 急停证据链 `safety_event` | **PASS**（`outcome=published`，不写 success） |
| 遗留 #1 / #2 / #3 | **PASS（已修复）** |
| 遗留 #4 串口绕过锁存 | **PASS（结论推翻，本就已被覆盖）** |
| 遗留 #5 阀超时关联回帧 | **BLOCKED（需设计/授权）** |
| 遗留 #6 HX711 假阳性 | **BLOCKED（需硬件）** |
| Pages 登录（含 Turnstile） | **NOT VERIFIED**（需浏览器解出真实 token） |
| 生产端 config_query 第二帧缺失超时 | **NOT VERIFIED**（生产无法制造缺失，逻辑由 H5b 覆盖） |
| D1 不可用时的真实 fail-closed | **NOT VERIFIED**（无法在不停机的前提下模拟） |
| GPIO 电气测量 | **NOT VERIFIED（未完成）** |
| 步进电机 | **BLOCKED（DRV8825 损坏）** |
| 重量闭环 / HX711 功能 | **BLOCKED（硬件未接）** |

**软件测试全部通过（418/418），但电气测量与硬件验收仍未完成，UI v1.1 不能据此宣布整体通过。**

---

## 10. 回滚

| 对象 | 方式 |
|---|---|
| D1 `0008` | `DROP TABLE ui_session / command_audit / safety_event`（会丢审计数据）+ 删除 `d1_migrations` 第 8 行；或用备份 `backups/guofeeder-pre0008-20261023.sql` 重建 |
| Worker | `wrangler rollback --config guo-feeder-api/wrangler.toml` |
| Pages | 控制台回退到上一 Deployment；或删除 `SESSION_STORE` 后重新部署（回到不可撤销会话） |
| 固件 | 本轮未改固件，无需回滚 |
| 设备配置 | 未被本轮改动 |

---

## 11. 证据归档

- `docs/evidence/UI-v11-Round4-2026-10-23/prod_e2e.json` —— 生产端到端 28 项结果
- `docs/evidence/UI-v11-Round4-2026-10-23/serial_latch.json` —— 串口锁存复核 6 项
- `Cloudflare_Assets/backups/guofeeder-pre0008-20261023.sql` —— 迁移前全量备份
