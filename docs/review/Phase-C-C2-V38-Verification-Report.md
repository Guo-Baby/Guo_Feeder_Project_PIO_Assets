# Phase C-C2 · `V-3.8` Presence 回归 —— 独立正式验收报告

> **日期**：`2026-10-16`
> **判据来源**：`docs/review/Phase-C-C2-Log-Routing-Console-Task-Sheet.md` §4.1 `V-3.8` = 「`$events/client_connected` / `disconnected` 仍 **`200`** 落 **`device_event`**」（通道 = tail + D1）
> **范围**：**仅 `V-3.8`**。未重查 `V-3.7`，未重查 `O-1`。
> **性质**：**一次真实的设备 offline → online 状态变化**（EMQX 会话被终止 → 固件自动重连），全程只读生产链路，唯一写操作为「踢掉设备 MQTT 会话」。

---

## §0 判定：**`V-3.8` = `PASS`**

| 验收要件 | 结果 |
|---|---|
| 真实设备 **offline** presence 被 Worker 正确接收 | ✅ tail：`POST /api/presence/event` `200`（`07:12:35 UTC`） |
| 对应 `device_event` 正确新增 **offline** 记录 | ✅ **`id 217`**：`device_id=288485896ce4` · `type=offline` · `created_at=07:12:31` · `reason:"kicked"` |
| 真实设备 **online** presence 被 Worker 正确接收 | ✅ tail：`POST /api/presence/event` `200`（`07:12:51 UTC`） |
| 对应 `device_event` 正确新增 **online** 记录 | ✅ **`id 218`**：`device_id=288485896ce4` · `type=online` · `created_at=07:12:47` |
| **身份 / 状态 / 时间**关联正确 | ✅ 三条独立时间源在同秒级吻合（见 §4） |
| **非设备** presence 不污染 `device_event` | ✅ 非设备客户端 presence 请求 `200` 且 **零新增行**（§6） |
| **没有发现数据异常** | ✅ 新增行恰好 2 行，无额外行、无遗漏、无错写 |

> **纪律声明**：本判定**不以 `HTTP 200` 为据** —— `200` 仅作**佐证**。判定基于 **D1 行的实际新增** + **身份/状态/时间的逐条关联** + **负向对照的零新增**。

---

## §1 测试环境与观测手段

| 项 | 值 |
|---|---|
| 设备 | `device_id = 288485896ce4` · EMQX `clientid = username = dev_288485896ce4` · MQTT/4（3.1.1）· keepalive 120 · `clean_start=true` |
| 生产链路 | 设备 → **EMQX** `$events/client_connected` / `client_disconnected` → EMQX HTTP Action → **Pages 中转** `guo-feeder-pagesdev.pages.dev` → **Worker** `guo-feeder-api.guobaby.workers.dev` → **D1** `guofeeder` |
| 触发手段 | **EMQX Admin API** `DELETE /clients/dev_288485896ce4`（生产代码 `src/emqx-admin.js:305–312` 同端点 · 注释载明 P0 实测 204） |
| 观测 1 | `wrangler tail --format json`（走代理 `127.0.0.1:7890`）→ `.pio/v38_tail.jsonl` / `.pio/v38_tail2.jsonl` |
| 观测 2 | MQTT **只读订阅** `guo_feeder/288485896ce4/#`（`.pio/hd1_run.py` · plan 仅 `WAIT 330` · **未发布任何报文**）→ `.pio/v38_mqtt.log` |
| 观测 3 | 生产 D1 **`SELECT`**（`.pio/v38_tool.sh` · `meta.changes` 恒 0 / `rows_written` 恒 0） |
| 观测 4 | EMQX Admin API `GET /clients`（只读 · 上下线前后各一次） |

> **两条 tail 的说明**：首条 tail（`07:10:06` 创建）经代理的 WebSocket 在建立后不久报 `502`，我随后**重建**了第二条（`v38_tail2`）。**实测两条最终都正常收流**（分别 15 / 14 事件，去重后 12 事件）—— 关键事件在 `v38_tail` 中**全部留存**（见 §2 时间线）。此波折已如实记录，不影响证据完整性。

---

## §2 测试时间线（UTC；括号内为本地 `GMT+8`）

| # | 时刻 | 事件 | 证据源 |
|---|---|---|---|
| 1 | `07:09:29–07:09:35`（15:09） | **测试前基线快照** | D1 `SELECT`（§3） |
| 2 | `07:10:06` | 启动 tail（第 1 条） | `.pio/v38_tail.jsonl` |
| 3 | `07:10:08.154` | 仅订阅客户端 `hd1_run_1792134606`（username `workbuddy`）连接（**非设备身份 · 负向对照用**） | `GET /clients` |
| 4 | **`07:12:30.763`**（15:12:30） | **★ 触发：`DELETE /clients/dev_288485896ce4` → HTTP `204`** | `.pio/v38_kick1.txt` |
| 5 | `07:12:31`（15:12:31） | **设备真实断线**：EMQX `$events/client_disconnected` | — |
| 6 | `07:12:35` | Worker 收到 presence 请求（**offline**）→ `POST /api/presence/event` **`200`** | `.pio/v38_tail.jsonl` |
| 7 | **`07:12:31`** | **D1 新增 `device_event id 217`（offline）** | D1 `SELECT` |
| 8 | `07:12:46`（15:12:46） | **设备自发重连公告**：MQTT `/up` `{"cmd":"system","id":"online","src":"device"}`（`qos=1` · `dup=False`） | `.pio/v38_mqtt.log` |
| 9 | `07:12:47.520`（15:12:47） | **设备重新上线**（新会话）：EMQX `connected_at` **已变更**、`recv_msg` 归零 | `GET /clients`（前后对比） |
| 10 | `07:12:47` | EMQX `$events/client_connected` | — |
| 11 | `07:12:50` | `/up` 帧经济规则 B → `POST /` `200`（→ `mqtt_messages id 3513`） | tail + D1 |
| 12 | `07:12:51` | Worker 收到 presence 请求（**online**）→ `POST /api/presence/event` **`200`** | tail |
| 13 | **`07:12:47`** | **D1 新增 `device_event id 218`（online）** | D1 `SELECT` |
| 14 | `07:13:04 … 07:15:37` | 设备 `/log` 帧持续 → `POST /api/history/ingest` `200`（`len=387`，与本项**无关**） | tail |
| 15 | `07:15:40` | 仅订阅客户端断开（**非设备**） | — |
| 16 | **`07:15:41`** | Worker 收到 presence 请求（**非设备**）→ `POST /api/presence/event` **`200`** · **D1 零新增** | tail + D1 |
| 17 | `07:15:57–07:16:04`（15:16） | **测试后快照** | D1 `SELECT` |
| 18 | `07:16:0x` | 设备终态：`connected=true` · `connected_at=07:12:47.520Z` · `recv_msg=11`（**健康 · 未再中断**） | `GET /clients` |

---

## §3 测试前后 D1 基线（单条 SQL 同时取 `COUNT(*)` + `MAX(id)`）

### 3.1 测试**前**（查询时刻 `2026-10-16 07:09:35 UTC`）

```sql
SELECT COUNT(*) AS de_n, COALESCE(MAX(id),0) AS de_mx, datetime('now') AS q_utc FROM device_event;
```
| de_n | de_mx | q_utc |
|---|---|---|
| **187** | **216** | `2026-10-16 07:09:35` |

| 辅助快照 | 值 |
|---|---|
| `device` `288485896ce4` | `state = REGISTERED` · `last_seen = 1792133827`（`2026-10-16 06:57:07 UTC`） |
| 最近 presence 事件 | `id 216 offline @1792086616` · `id 215 online @1792086616`（`2026-10-15 17:50:16`） |
| 防串扰水印 | `mqtt_messages n=3494 / max=3512` · `log_history n=63 / max=365` |

### 3.2 测试**后**（查询时刻 `2026-10-16 07:16:04 UTC`）

| de_n | de_mx |
|---|---|
| **189** | **218** |

| 辅助快照 | 值 | 说明 |
|---|---|---|
| `device.last_seen` | `1792134770`（`2026-10-16 07:12:50 UTC`） | **已推进**（原 `06:57:07`）—— 见 §5.3 |
| `mqtt_messages` | `n=3495 / max=3513` | **+1** = `/up` 在线公告帧（§5.4） |
| `log_history` | `n=65 / max=379` | +2（设备 `/log` 续发 · 与本项**无关**） |

### 3.3 净增 ≠ 事件数（逐条关联，见 §4/§5）

`189 − 187 = 2` 与「1 offline + 1 online = 2 条事件」一致 —— **但这不作为判据**。判据是 `id > 216` 的**逐行内容**与事件的**逐条关联**（§5）。

---

## §4 三源时间关联表（身份 / 状态 / 时间的独立互证）

| 环节 | offline | online | 一致性 |
|---|---|---|---|
| **① 触发时刻**（我方操作） | `07:12:30.763` | — | — |
| **② EMQX 事件时刻**（信封 `event_at`） | `1792134751` = `07:12:31` | `1792134767` = `07:12:47` | — |
| **③ D1 `created_at`**（= 事件时刻，非写入时刻） | `1792134751` = `07:12:31` | `1792134767` = `07:12:47` | **② ≡ ③**（逐秒相同） |
| **④ Worker 投递时刻**（tail） | `07:12:35`（+4 s） | `07:12:51`（+4 s） | 与 ②③ 同分钟、投递延迟一致（4 s） |
| **⑤ EMQX 会话（`GET /clients`）** | 会话终止 ⇒ `connected_at` 由 `2026-10-15T17:50:16.585Z` 变为 `2026-10-16T07:12:47.520Z`；`recv_msg` 归零 | 同左（新会话） | **⑤ 的 `connected_at` ≡ ② online**（`07:12:47.520` ↔ `07:12:47`，亚秒级） |
| **⑥ 设备自证**（MQTT 只读订阅） | — | `07:12:46` 设备自发 `/up` `{"cmd":"system","id":"online","src":"device"}` | 与 ② online 相差 **1 s**（设备先自报、broker 后定事件） |

> **结论**：**三条互相独立的证据链**（EMQX 会话时间戳 / D1 行 / MQTT 抓包自证）在**秒级**内收敛到同一对事件。身份来源唯一（`device_id = 288485896ce4`，由 `dev_288485896ce4` 经 `deviceIdFromClientIdentity` 解析）。

---

## §5 逐条证据链

### 5.1 ★ offline 事件

| 项 | 内容 |
|---|---|
| **D1 行** | **`id = 217`** |
| `device_id` | `288485896ce4` |
| `type` | **`offline`** |
| `actor` | `cloud`（基础设施事实，`presence.js:48`） |
| `created_at` | `1792134751` → `2026-10-16 07:12:31 UTC`（= 事件时刻） |
| `payload` | `{"event_id":"emqx:dev_288485896ce4:disconnected:1792134751","event_at":1792134751,"clientid":"dev_288485896ce4","reason":"kicked","peername":"110.180.51.142:35514","source":"emqx"}` |
| **Worker HTTP** | `POST /api/presence/event` → **`200`**（`content-length: 175`，`cf-worker: guo-feeder-pagesdev.pages.dev`，`eventTimestamp 07:12:35`） |
| **关联** | `event_id` 内含 `clientid` + `disconnected` + 事件秒 ⇒ 与 `event_at` / `created_at` **逐秒自洽**；`reason:"kicked"` **直接记录触发方式**（可审计性最强的一条） |

### 5.2 ★ online 事件

| 项 | 内容 |
|---|---|
| **D1 行** | **`id = 218`** |
| `device_id` | `288485896ce4` |
| `type` | **`online`** |
| `actor` | `cloud` |
| `created_at` | `1792134767` → `2026-10-16 07:12:47 UTC` |
| `payload` | `{"event_id":"emqx:dev_288485896ce4:connected:1792134767","event_at":1792134767,"clientid":"dev_288485896ce4","peername":"110.180.51.142:34313","source":"emqx"}` |
| **Worker HTTP** | `POST /api/presence/event` → **`200`**（`content-length: 151`，`eventTimestamp 07:12:51`） |
| **关联** | `peername` 端口 `:34313` ≠ offline 的 `:35514` ⇒ **确为新会话**（非同会话幻影）；EMQX `connected_at` `07:12:47.520` 与之同秒 |

### 5.3 online 的**设计内副作用**（同事务）

| 项 | 值 | 判据 |
|---|---|---|
| `device.last_seen` | `1792133827`（06:57:07）→ **`1792134770`**（07:12:50） | `presence.js:311–316`：**仅 online** 刷新 `last_seen`；offline **不刷**（§5.1 之后 `last_seen` 未变，佐证该分支正确） |
| 终值 `07:12:50` 而非 `07:12:47` | `last_seen = MAX(证据)`；`07:12:50` 的 `/up` 帧（见 §5.4）比 presence 事件（`07:12:47`）更晚 | **符合设计**（单调前进），非异常 |

### 5.4 online 的**旁证**：设备自证 + 规则 B 通路

| 证据 | 内容 |
|---|---|
| MQTT（只读订阅） | `15:12:46`（=`07:12:46Z`）`topic=guo_feeder/288485896ce4/up` `qos=1 dup=False` `{"cmd":"system","id":"online","src":"device"}` |
| D1 `mqtt_messages` | **`id 3513`** · `topic=.../up` · `direction=up` · `created_at` → `07:12:50` · `raw_payload` 同上 |
| 防串扰 | `mqtt_messages` 窗口内新增 **1 行**，`topic LIKE '%/log'` 的**新增 = 0** → 与 `V-3.4` 的隔离结论一致，**无串扰** |

> ⇒ **设备自己在重连后主动上报 `online`** —— 这是「设备**真的**重新上线」的**设备侧自证**，独立于 broker 与 Worker。

---

## §6 正向 / 负向对照

### 6.1 正向对照（已验证）

| 组 | 请求 | HTTP | D1 结果 |
|---|---|---|---|
| **真设备 offline** | `POST /api/presence/event`（`07:12:35` · len 175） | `200` | ✅ **新增 1 行**（`217 offline`） |
| **真设备 online** | `POST /api/presence/event`（`07:12:51` · len 151） | `200` | ✅ **新增 1 行**（`218 online`） |
| — no `device_event` 之外的写入 | — | ✅ 仅 2 行 | 无额外行 |

### 6.2 ★ 负向对照（非设备客户端不污染）

| 项 | 内容 |
|---|---|
| 非设备客户端 | 我的**只读订阅**客户端：`clientid = hd1_run_1792134606` · `username = workbuddy`（**不以 `dev_` 开头**） |
| 其连接 | `07:10:08.154Z` |
| 其断开 | `07:15:40`（订阅窗口自然结束） |
| **Worker 请求** | `07:15:41` `POST /api/presence/event` → **`200`**（`content-length: 170`） |
| **D1 结果** | **零新增** —— 该请求之后 `device_event` 仍为 `n=189 / max=218`，**没有出现任何非设备身份的 `device_id` 行** |
| 历史旁证 | `device_event` 的 `device_id` 分布**仅含真实设备 id**（`288485896ce4` / `a5a0000000a1` / `d163d0000001` / …），**从无** `workbuddy` / `shouji` 等非设备身份 |
| `phone`（`shouji`） | 窗口内**全程保持连接**（`connected_at 06:17:59.973` 未变），未产生任何 presence 请求 ⇒ 无干扰 |

> **证据边界（如实说明）**：`wrangler tail` **不携带响应体**（本仓库既有结论：`event.response` 键仅 `['status']`）。因此「返回 `ignored:"not_device_identity"`」这**一字段级结论**由三者**联合**确立，而非直接读取：
> ① `presence.js:273–274` 的确定性判定（`!deviceId ⇒ {http:200, ignored:"not_device_identity"}`）；
> ② **零新增行**（若走了写入分支必然落行）；
> ③ **同窗口内真设备的两条同类请求确实落了行**（正/负向在同一窗口、同一端点、同一时段形成**对照**）。
> ⇒ 字段级措辞标 **`INFERRED`**，**行级（不污染）结论为 `CONFIRMED`**。

---

## §7 最终 `V-3.8` 判定

> ### ✅ **`V-3.8` = `PASS`**

| 判据（任务书） | 结果 |
|---|---|
| 真实设备 offline presence 被 Worker 正确接收 | ✅ `POST /api/presence/event` `200` @ `07:12:35` |
| 对应 `device_event` 正确新增 offline 记录 | ✅ `id 217` · `288485896ce4` · `offline` · `07:12:31` · `reason:"kicked"` |
| 真实设备 online presence 被 Worker 正确接收 | ✅ `POST /api/presence/event` `200` @ `07:12:51` |
| 对应 `device_event` 正确新增 online 记录 | ✅ `id 218` · `288485896ce4` · `online` · `07:12:47` |
| 身份 / 状态 / 时间关联正确 | ✅ §4 三源秒级收敛 |
| 非设备 presence 不污染 `device_event` | ✅ 零新增（§6.2） |
| 无数据异常 | ✅ 新增恰好 2 行；无重复、无遗漏、无错写、无串扰 |

**未发现任何 `FAIL` 条件**（无「已到达 Worker 但未落库」、无「身份/状态/关联错误」）。
**置信度：高**（EMQX 会话时间戳 / D1 行 / MQTT 设备自证 —— 三条独立链互证）。
**触发方式的透明度**：offline 行的 `payload.reason = "kicked"` **完整记录了本次触发是「broker 侧终止会话」**；该事件的**事件类型、身份、投递与落库路径与设备自然掉线完全同构**（同一 `$events/client_disconnected` 规则与同一 Worker 端点）。**设备随后由自身固件逻辑重连**（其自发 `/up` 帧为证），**非我方代劳**。

---

## §8 工作树变化清单

### 8.1 新增 / 修改的文件

| # | 文件 | 变更 | 层级 |
|---|---|---|---|
| 1 | `PIO_Assets/Guo_Feeder_Project/docs/review/Phase-C-C2-V38-Verification-Report.md` | **新增**（本报告） | 审阅记录（**未提交**） |
| 2 | `.pio/v38_tool.sh` | 新增（证据采集小工具 · **含唯一写操作 `kick` 入口**） | `.pio/` = **gitignored**，不入库 |
| 3 | `.pio/v38_tail.jsonl` · `.pio/v38_tail2.jsonl` · `.pio/v38_mqtt.log` · `.pio/v38_baseline.txt` · `.pio/v38_after1.txt` · `.pio/v38_final.txt` · `.pio/v38_kick1.txt` · `.pio/plan_v38_sub.txt` | 新增（原始产物） | `.pio/` = **gitignored**，不入库 |

**未修改**：Worker `src/**` · 固件 `src/**` · EMQX 规则 / 连接器 / Action / ACL / 账号 · D1 schema · 任何正式验收矩阵 / 任务单 / 历史 migration。

### 8.2 三仓工作树（变更前 → 变更后）

| 仓 | 变更前 | 变更后 | 判定 |
|---|---|---|---|
| 固件 `PIO_Assets/Guo_Feeder_Project`（`wb`） | `1ebae81` · dirty **7**（4 M 受保护文档 + 3 ?? 报告） | `1ebae81` · dirty **8**（4 M **逐字同前** + **4 ??**：+本报告） | ⚠️ 新增仅 1 份报告；**HEAD 未变**；4 份 REV-3 受保护文档**未被触碰** |
| 云端 `Cloudflare_Assets`（`main`） | `e328a11` · 1 ??（上轮夹具） | `e328a11` · 1 ??（**同前**） | ✅ 本轮**零变化** |
| 消息 `EMQX_Assets`（`main`） | `3165aab` · clean | `3165aab` · clean | ✅ 本轮**零变化** |

### 8.3 生产侧写入审计（唯一写操作 + 只读证据）

| 动作 | 目标 | 结果 |
|---|---|---|
| `DELETE /clients/dev_288485896ce4` | EMQX 会话（**唯一对生产状态的操作**） | **`204`** @ `07:12:30.763Z` —— 设备 **17 s 后由固件自行重连**，终态 `connected=true`（§2 #18） |
| D1 `SELECT`（全部查询） | `device_event` / `device` / `mqtt_messages` / `log_history` | `meta.changes = 0` · `rows_written = 0`（**可自证**） |
| EMQX `GET /clients` | 只读 | `200` |
| MQTT 订阅 | 只读（计划仅 `WAIT 330`） | **未发布任何报文** |
| 未做 | 未部署 · 未改 Worker / 固件 / 规则 / ACL / schema · 未 `POST /api/presence/event` 伪造 · 未清理历史数据 · 未 commit / push / reset / checkout | — |

---

## §9 本轮边界声明

**已做**：只读读取 `presence.js` / `index.js` / `device-registry.js`；取 D1 前后基线（单条 SQL 同时取 `COUNT` 与 `MAX(id)`，并记录查询时刻）；**一次** EMQX 会话终止（经人工裁决授权的触发方式）触发真实 offline→online；`wrangler tail`、MQTT 只读订阅、EMQX `GET /clients` 三路观测；逐条关联；生成本报告。

**未做**：未重查 `V-3.7` · 未重查 `O-1` · 未修改 `BT-1` / `V-3.2` / 正式验收矩阵 · 未部署 · 未改 schema · 未清理任何历史数据 · 未 commit / push / reset / checkout。

---

*本报告为 `2026-10-16` `V-3.8` 独立正式验收产物，**等待人工裁决**。未获授权前不启动任何回填、修复、部署或其它生产操作。*
