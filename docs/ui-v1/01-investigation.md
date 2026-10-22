# 只读调查报告（UI v1 修订依据）

> 本轮**只读**：未改生产代码、未加迁移、未部署、未烧录、未向设备下发任何命令。
> 分级标记：
> **[代码确认]** = 已在源码/配置中逐行核实 ·
> **[实测]** = 有真实运行数据支持 ·
> **[待验证]** = 无法从现有代码证实 ·
> **[拟议]** = 本轮设计提议，尚不存在。
> 所有行号基于 2026-10-23 05:50 的工作区。

---

## 1. Pages 前端结构与构建方式

| 项 | 结论 | 依据 |
|---|---|---|
| 构建方式 | **纯静态，无构建步骤**。`web:dev` = `wrangler pages dev guo-feeder-pagesdev`，`web:deploy` 直接上传目录 | **[代码确认]** `package.json` scripts |
| Pages 配置 | 仅 `{name, compatibility_date}`，**无 build/无 bindings 声明** | **[代码确认]** `guo-feeder-pagesdev/wrangler.jsonc` |
| 页面 | `index.html`(58) / `login.html`(196) / `app.html`(274) | **[代码确认]** |
| 前端依赖 | **零依赖**（无 `package.json`、无 import、无 CDN） | **[代码确认]** |
| 移动端适配 | 有 `viewport` meta + `grid-template-columns:auto-fill minmax(150px,1fr)` 自适应；**无触控尺寸规范、无底部导航、无 PWA manifest** | **[代码确认]** `app.html:5,33` |
| 现有能力 | 表规模 / 入站校验 / 设备快照 / 最近日志（级别过滤 + 分页） | **[代码确认]** `app.html:68-107` |
| 控制能力 | **零**。全文无 `POST`（仅登出走 POST） | **[代码确认]** `app.html:265` |

**差异（原计划与代码不符）**：原计划写"只读看板 274 行"，实测一致；
但原计划未指出 **`app.html` 的日志列表渲染的是 `message` 字段，而该字段 100% 为 NULL**（见 §9.2）
⇒ 现有日志区"看起来有行但内容为空"，这不是加载失败。

---

## 2. Middleware / 会话 / CSRF / `/api/command` 全链路

| 项 | 结论 | 依据 |
|---|---|---|
| 保护范围 | 精确集合 `{"/app.html","/app","/"}`，其余**一律放行** | **[代码确认]** `_middleware.js:47` |
| 未登录行为 | 302 到 `/login.html?next=…`（不是 401） | **[代码确认]** `_middleware.js:76-82` |
| 会话机制 | **无状态 HMAC-SHA256**，payload 只有 `{exp, sid}`；`sid` 注释写明"便于将来做撤销表" | **[代码确认]** `_lib/session.js:108-125` |
| 会话吊销 | **不存在**。登出只清 cookie（`Max-Age=0`），令牌在 TTL 内仍有效 | **[代码确认]** `session.js:211`、`api/auth/logout.js` |
| 登录限流 | **per-isolate 内存**（`loginLimiter` 全局单例），冷启动归零；自身文档用 `LIMIT_DOC` 声明不是全局配额 | **[代码确认]** `session.js:22-26,58,332` |
| 限流分桶 | **单例、单一用途**（登录失败计数）。命令入口无独立桶 | **[代码确认]** `session.js:266-332` |
| CSRF | **无任何 Origin / Referer / Sec-Fetch-Site 校验** | **[代码确认]** 全仓 grep 无命中 |
| `/api/command` 会话保护 | **无**（不在保护集）→ **未登录可调用** | **[代码确认]** `_middleware.js:47` |
| `/api/command` 转发 | 注入 `source:"cloud_api"` 后 `fetch(WORKER_BASE)`，**只带 Content-Type，不带任何凭据** | **[代码确认]** `api/command.js:29-47` |
| 客户端 IP | `CF-Connecting-IP` → `X-Forwarded-For` → `"unknown"`；`ipHash` = SHA-256 前 8 字节 hex（**不落原文**） | **[代码确认]** `session.js:238-251` |

⇒ 三条结论：
1. **登出后旧 cookie 仍可用**（无吊销）——`UI-COOKIE-REVOCATION` 属实。
2. **限流无法复用**：现有 `loginLimiter` 是单例、按 key 计数，可复用其结构，但必须**新建独立实例**，否则登录与命令互相挤占。
3. **CSRF 完全空白**，任何同浏览器第三方页面都能触发 `/api/command`（配合 P0-1 更危险）。

---

## 3. Worker cloud_api 入口与鉴权（P0）

| 项 | 结论 | 依据 |
|---|---|---|
| 受保护路径 | 仅 `/api/history/telemetry|log`（`authorizeAdmin`）与 `/api/admin/**` | **[代码确认]** `index.js:233-247, 255-262` |
| 根 `POST /` | 先判 `source`；`cloud_api` 分支在 `:451` 早退，**位于** webhook key 校验 `:545` **之前** | **[代码确认]** |
| `cloud_api` 的校验 | **只有** `isValidDeviceId(target)`（12 位小写 hex），**无身份校验** | **[代码确认]** `index.js:466-482` |
| 成功响应 | **纯文本 `"OK"`，HTTP 200**；**不返回 message_id / cmd_id / 时间戳** | **[代码确认]** `index.js:905-910` |
| 失败响应 | `502 {error:"publish_failed", device_id, topic, status, code}` | **[代码确认]** `index.js:885-894` |
| 匿名可用性 | **[实测]** 无任何 header，`POST {WORKER_BASE}/` + `{"device_id":…,"source":"cloud_api"}` 连续返回 200 并真实下发 | 本轮调查复核 |

⇒ **P0-1 成立**。且比原计划更严重的一点：**成功响应不含任何关联 ID**，
即使加了鉴权，UI 也无法从响应里拿到"这次下发的标识"⇒ 必须同时改响应体（见 §4 结论）。

---

## 4. 命令请求 → 设备响应 → 云端落库 完整链路

```
[UI] --POST /api/command--> [Pages function]
      → 注入 source=cloud_api
      → POST {WORKER_BASE}/  (无凭据)
        → isValidDeviceId
        → topic = guo_feeder/<id>/down
        → publishToDevice (EMQX HTTP API, QoS?)
        → 200 "OK"                      ← ★ 只到这里为止
                                        （设备尚未执行，甚至可能尚未收到）
[设备] MQTT down 收到
      → cloud_check_duplicate_cmd(id)   ← 命中即静默返回（无回帧）
      → 回 ACK: {c:"ack", i, o, p:{result:"received"}, t}
      → 回 RESULT(受理): {s:1, c:"result", i, o, m:"Action queued", instance_id, command}
      → 真正执行（异步）
      → 回 RESULT(终态): {s, c:"result", i, o, type:"action_result", instance_id}
[EMQX] /up 主题 → 规则 → [Worker /api/ingest] → mqtt_messages
[UI] ???                              ← ★ 无任何只读端点能读到这些回帧
```

**[代码确认] 关键缺口：目前没有任何端点能读 `mqtt_messages` 的行。**

- `read-api.js` 白名单只有 4 条：`history/telemetry`、`history/log`、`admin/metrics`、`admin/device/<id>/snapshot`
- `handleAdmin` 的 device 子路由是 claim/credential/revoke/bind/... 全为管理动作
- `mqtt_messages` 仅在 `handleAdminMetrics` 里被 `COUNT(*)`
- `snapshot` 返回 `device / telemetry_latest / telemetry_recent / log_recent / event_recent / counts`，**不含 mqtt**

⇒ **UI 无法把"我发的命令"与"设备的回帧"关联起来**。这不是设计偏好问题，
而是**能力缺失**：M2 的状态机必须先补一个按 `cmd_id` 查回帧的只读端点（**[拟议]**，见 `05-command-lifecycle.md` §5）。

---

## 5. 设备端 cmd_id 去重与超时

| 项 | 值 | 依据 |
|---|---|---|
| 去重缓存 | **FIFO 10 条** | **[代码确认]** `cloud_manager.cpp:59` `MQTT_DUP_CACHE_SIZE 10` |
| TTL | **30 000 ms** | **[代码确认]** `:60` |
| 命中行为 | 打印 `[Cloud] Command duplicate` 后 **直接 return —— 不回 ACK、不回 result** | **[代码确认]** `:1572-1576` |
| `i` 缺失 | `[Cloud] Missing id` → **直接丢弃**，无回帧 | **[代码确认]** `:1567-1571` |
| `log_ack` | **旁路去重**（幂等，重复由云端消化） | **[代码确认]** `:1563,1590+` |
| Action 运行时超时 | `COMMAND_ACTION_TIMEOUT_MS = 600000`（10 min） | **[代码确认]** `command_manager.cpp:84` |
| Workflow 运行时超时 | `COMMAND_WORKFLOW_TIMEOUT_MS = 600000` | **[代码确认]** `command_manager.cpp:85` |
| Workflow 自身超时 | 定义内的 `timeout_ms`（`wf_valve_20g` = 360000） | **[实测]** `workflow.get` 回读 |

⇒ **对 UI 最重要的三条**：
1. **重复 `i` 的表现是"彻底没有回应"**，与"设备离线"不可区分 ⇒
   绝不能用同一 `i` 重试来"探活"；重试必须换新 `i`，且必须由**人**决定。
2. 去重窗口只有 30 s，**超过 30 s 后同一 `i` 会被当成新命令执行** ⇒
   UI 侧必须保证幂等键长期唯一（不能靠"30 s 内别点两次"这种约定）。
3. 10 条 FIFO 很小：并发连点 10 次即可把真实命令挤出缓存 ⇒ 服务端必须阻止连点（见 §7）。

---

## 6. Registry 与能力发现

| 项 | 结论 | 依据 |
|---|---|---|
| `{"c":"registry","k":0/1/2}` | 返回 `{v, n, d:[[k, NAME], …]}` —— **只有序号与名字** | **[实测]** 05:1x 查询 |
| `k` 必须放**顶层** | 放进 `p` 里被忽略且**无回帧** | **[实测]** 本轮踩过 |
| `query_capabilities` | 返回 `action/trigger/workflow` 三段，每段 `{version, checksum, count, entries:[{stable_id, runtime_id}]}` ⇒ **仍然只有 id ↔ 名字** | **[代码确认]** `command_manager.cpp:1313-1366` |
| 参数元数据 | **不在任何命令响应中**。固件内部 `WorkflowParam{name,type,unit,description}` 仅存在于 `.rodata` | **[代码确认]** `workflow.h:157-163` |
| 参数约束表达 | **自由文本字符串**（如 `"0 < gram <= 300"`、`"1-3600"`、`"0-23"`），**无 min/max/default/required 结构字段** | **[代码确认]** 各 `*_params[]` |
| checksum 语义 | **[待验证]** 未找到固件端 checksum 的计算算法与是否覆盖参数集合的说明 | — |
| 版本递增规则 | `v` 在 Action/Trigger/Workflow 集合变化时递增（新增/移除注册会重排 stable_id） | **[实测]** 今日经历 2→3→4 |
| `boot_seq` 来源 | 设备侧 NVS/配置持久计数，每次 boot +1 | **[实测]** 日志 `boot=73` |

⇒ **§6 的结论可以定死**：**当前 Registry 不足以生成编辑表单**，
因为它连"这个 Action 有几个参数"都不告诉云端。详见 `04-workflow-editor.md` §2。

---

## 7. 现有 Action / Trigger / 查询 / Workflow 能力

完整清单见 `02-capability-catalog.md`（含参数、约束、三态标记）。此处只记总量与关键约束：

| 类别 | 数量 | 说明 |
|---|---|---|
| Action | **14**（`v=4`，`n=14`） | 其中 2 个属 `test_stepper`（排障用） |
| Trigger | **3**（`v=2`，`n=3`） | `delay` / `timer` / `weight_decrease` |
| Workflow | **4**（`v=3`，`n=4`） | `wf_valve_20g` 为当前业务流 |
| 查询命令 | **8 + `query_state`** | `query_state` 支持 25+ 个状态键 |
| System 对象 | **20+** | 含 `config_*` 9 个、资源诊断 5 个 |
| Workflow 管理 | **7** | `list/get/create/set/delete/save/sync_info` |

**[代码确认] Workflow 无独立"排序"接口**：步骤顺序 = 定义 JSON 里 `steps` 数组顺序；
调整顺序等价于 `workflow.set` 整体替换。**没有** `workflow.reorder` / `workflow.rename`。
**[实测] `workflow.save` 会触发设备安全重启**（`restarting:true`），且 `workflow registry v` +1。

**[代码确认] 删除语义**：`workflow.delete` 只置 `valid=false`，不是物理删除。

---

## 8. D1 迁移与表结构

| 项 | 结论 |
|---|---|
| 迁移文件 | `0001`…`0007`（`guo-feeder-api/migrations/`） |
| 已应用 | **7/7**，最近 `0007_mqtt_messages_dedup.sql`（10-20 17:23）**[实测]** |
| 下一个编号 | **`0008`**（本轮**不创建**，仅在设计里提案） |
| `device_event.type` | 有 **CHECK 白名单**（13 种），**不含**命令审计类 **[代码确认]** `0001_...sql` |
| `log_history` | 列：`id, device_id, ts, level, code, boot_seq, seq, message, payload, created_at`；`ts`/`created_at` 均为**秒** |
| `mqtt_messages` | `created_at` 为**毫秒**（与 log 表不同单位） |
| `telemetry_history` | 三分量幂等键 `(device_id,type,ts,seq)` 由唯一索引吸收重投 |

⇒ 命令审计**不能**塞 `device_event`（CHECK 会拒），必须新表。D1 无法改 CHECK。

---

## 9. 现有可用数据接口与真实数据

### 9.1 可用只读接口（4 条，均须 Bearer，经 Pages 同源代理）

| 接口 | 参数 | 返回 |
|---|---|---|
| `GET /api/history/telemetry` | `device_id, type, from, to, before_id, limit` | `{items,count,limit,has_more,next_before_id,order}` |
| `GET /api/history/log` | 同上 + `level` | 同上，`items` 为**全列**（含 `payload`） |
| `GET /api/admin/metrics` | — | 表规模 + 入站校验计数 + warnings |
| `GET /api/admin/device/:id/snapshot` | `recent` | `device / telemetry_latest / telemetry_recent / log_recent / event_recent / counts` |

**[实测] 数据量**：`telemetry_history` 721 行（`weight.value` 369 / `climate.humidity` 179 / `climate.temperature` 173，近 24h 分别为 343/99/97）；
`log_history` 3970 行；在线判定阈值 `DEFAULT_PRESENCE_TIMEOUT_SECONDS = 300`。

### 9.2 `log_history` 字段实况（**这是原计划的一处误判**）

**[实测]** 全表 3970 行：

| 字段 | 实况 |
|---|---|
| `message` | **3970/3970 全为 NULL** |
| `code` | **3970/3970 全为 NULL** |
| `payload` | **0 行为空**，长度分布 196/200/204 字符（base64） |
| `level` | 有值：INFO 1840 / WARN 1491 / CRITICAL 635 / ERROR 4 |

⇒ **现有 UI 的"消息"列永远空白**，因为 envelope 里本来就没有 `message` 字段
（envelope 只有 `device_id / boot_seq / seq / ts / level / payload_base64`）。
**"日志区能读"必须先解码 `payload`，没有第二条路。**

---

## 10. 测试与部署验证现状

| 项 | 结论 |
|---|---|
| 前端测试 | `tests/pages/pages_auth.test.mjs`(28 KB) + `pages_webhook.test.mjs`(14 KB) |
| 测试方式 | **直接 import `functions/**` 处理函数并构造 context**，不启 dev server；上游 `fetch` 用 stub 替身 **[代码确认]** 文件头注释 |
| 运行脚本 | **`package.json` 无 `test` 脚本** ⇒ 目前靠手工 `node --test` / `node <file>` **[代码确认]** |
| 真实网络端到端 | 记录在证据目录，非自动化 |
| 固件侧契约测试 | 有 `test/log_contract/`（主机 clang++ 编 wasm32 跑字节级验证）——见 `log_envelope.h` 头注释 **[代码确认]** |

⇒ UI 阶段应补：`package.json` 的 `test` 脚本、以及**日志解码的黄金样本测试**（见 `07-log-decode.md` §6）。

---

## 11. 与原计划的差异汇总（以代码为准）

| # | 原计划表述 | 实际 | 影响 |
|---|---|---|---|
| 1 | "新增 registry 只读端点，返回 `{v, actions:{NAME:k}}`" | 可行，但需**新建** Worker 端点 + Pages 白名单；`query_capabilities` 也不含参数 | 保留方向，补充"参数元数据不在固件协议内" |
| 2 | "`cloud_api` 分支加 Bearer" | 成立；但**响应体只有 `"OK"`**，不含关联 ID | 必须同批改响应体，否则 UI 无法关联 |
| 3 | "UI 轮询设备状态" | **没有**任何能读到 `mqtt_messages` 回帧的端点 | **新增前置依赖**（M1 补端点） |
| 4 | "日志解码后 UI 可读" | 成立；`message` 100% NULL，`payload` 必在 | 方向正确，但要说明 envelope 无 message |
| 5 | "registry 缓存按 boot_seq 失效" | registry 响应**不含 boot_seq** | 改为「TTL + 收到 version mismatch 主动失效」 |
| 6 | "参数校验在编辑器里做" | 固件**无结构化约束**，非法值多为**静默回落**（如 `gram` 非法→20 + 事件） | 约束必须由**云端元数据目录**给出，且标注"与固件漂移风险" |
| 7 | "Workflow 支持排序" | 无 reorder 接口，排序 = `workflow.set` 重写数组 | 编辑器用"重写整份定义"实现，不提 reorder |
| 8 | "会话吊销" | 无状态 HMAC，无吊销表 | 需新增会话表或改令牌策略（M1 提案见 `06-audit-and-security.md`） |

---

## 12. 尚待验证（不得当作已有能力）

| # | 项 | 为什么现在还证不了 |
|---|---|---|
| V-1 | registry `checksum` 的算法与覆盖范围 | 固件侧未见文档/实现说明，无法判断能否用它检测"参数集变化" |
| V-2 | EMQX `publishToDevice` 的 QoS 与是否持久化 | 未读 `emqx-admin.js` 细节；影响"下发丢失"的判定 |
| V-3 | `mqtt_messages` 的 `direction/cmd/object` 是否稳定填充 | 需要真实回帧样本逐条核对（可用于 cmd_id 关联，但需验证 `object` 是否等于 `o`） |
| V-4 | 设备重启后 `instance_id` 是否复用于新命令 | 影响"迟到回帧"的判定（可能是旧命令的回帧） |
| V-5 | 10 条 FIFO 去重在"命令风暴"下的实际挤占行为 | 需真机压测（**本轮不执行**） |
| V-6 | `workflow.set` 对 `variant` 的影响与 UI 是否需要展示修订号 | 需读 `workflow.cpp` set 分支细节 |
