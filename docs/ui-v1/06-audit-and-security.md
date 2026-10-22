# 审计与安全计划（修订）

> 本轮**不实施**：不建迁移、不改代码、不部署。本文是待批准的实施规格。
> 唯一从原计划原样保留的硬要求：**先堵匿名命令入口，再谈界面。**

---

## 1. 现状核查结论（都是实测/读码，不是推测）

| # | 事实 | 证据 |
|---|---|---|
| 1 | `cloud_api` 命令通路**无身份校验** | `index.js:451` 早退，位于 `:545` 的 webhook key 校验之前 |
| 2 | 匿名 `POST /` 实测可下发命令 | 无任何 header，连续 200 |
| 3 | Worker 成功响应只有纯文本 `"OK"` | `index.js:905-910` |
| 4 | `/api/command` **不在** 中间件保护集 | `_middleware.js:47` |
| 5 | `/api/command` 转发**不带任何凭据** | `api/command.js:38-47` |
| 6 | 会话是**无状态 HMAC**，`sid` 为空占位，**无吊销表** | `session.js:108-125` |
| 7 | 登出只清 cookie，令牌 TTL 内仍有效 | `session.js:211` + `api/auth/logout.js` |
| 8 | 限流器是**单例、仅用于登录**，per-isolate | `session.js:332` |
| 9 | 全仓**无** Origin/Referer/Sec-Fetch-Site 校验 | grep 无命中 |
| 10 | 无任何端点能读 `mqtt_messages` 行 | 只有 `COUNT(*)` |
| 11 | `device_event.type` 有 CHECK 白名单，不含命令审计 | `0001_...sql` |
| 12 | 迁移已应用到 `0007` | `d1_migrations` 实测 |

---

## 2. 鉴权改造（三处，缺一不可）

### 2.1 Worker：`cloud_api` 分支加 Bearer

```
if (source === "cloud_api") {
    const denied = authorizeAdmin(request, env);   // ← 与 /api/history/* 同一个函数
    if (denied) return denied;                      //    缺失/错误/未配置 ⇒ 同一 401
    …（原有 isValidDeviceId 逻辑不变）
}
```

要点：
- **复用 `authorizeAdmin`**，不新写比较逻辑（它已是常量时间 + 统一 401，避免泄露"是缺令牌还是错令牌"）
- 位置：**必须在 `:451` 分支的入口处**，早于任何业务动作
- **EMQX 入站分支（`source != cloud_api`）完全不动** —— 它走 `X-Webhook-Key`，动了就断流

### 2.2 Pages：`/api/command` 纳入会话保护

`PROTECTED_PAGES` 增加 `"/api/command"`。

★ 但**不能**用现在的 302 逻辑 —— 那是给页面用的。API 路径应返回 **401 JSON**：
```
if (path === "/api/command") {
    const s = await readSession(request, env);
    if (!s) return jsonRes({ error: "unauthorized" }, 401);
    return next();
}
```
理由：`fetch` 拿到 302 到登录页会把 HTML 塞进 JSON 解析，产生"看不懂的错误"。
（现有 `[[path]]` 代理已按这个思路处理，保持一致。）

### 2.3 Pages：服务端注入 Bearer

`api/command.js` 转发时增加：
```
headers: {
  "Content-Type": "application/json",
  "Authorization": "Bearer " + env.ADMIN_API_TOKEN
}
```
`ADMIN_API_TOKEN` **只出现在服务端**，与 `[[path]].js` 保持一致（`session.js` 头注释第 1 条纪律）。

**部署顺序（否则一定断）**：
```
① 部署 Pages（会注入 Bearer + 加会话保护）
② 立刻部署 Worker（开始要求 Bearer）
③ 中间最多几十秒窗口：Worker 仍不要求令牌 ⇒ 不失败；Pages 已带令牌 ⇒ 也不会失败
```
若反过来先部署 Worker ⇒ 旧 Pages 不带令牌 ⇒ 命令全 401。

---

## 3. CSRF / 来源校验

### 3.1 判定规则（只作用于**写**入口）

适用：`POST /api/command`、`POST /api/auth/login`、`POST /api/auth/logout`。
**不适用**：`/api/webhook`、`/api/ingest`、`/api/history/ingest`、`/api/presence/event`
（它们是机器入口，用密钥而非 cookie，加了反而可能误伤）。

```
1. Origin 存在 ⇒ 必须与请求 host 的源完全一致（scheme+host+port）
2. Origin 缺失 ⇒ 看 Sec-Fetch-Site：
     "same-origin" 放行 / "none" 放行（地址栏直接访问）/ 其他 ⇒ 拒
3. 两者都缺失 ⇒ 看 Referer：必须同源
4. 三者都缺失 ⇒ 按"非浏览器客户端"处理：
     · /api/command 仍需会话 cookie，无 cookie 自然 401 ⇒ 无害
     · 因此缺失时**不额外拒绝**（避免误伤 curl 调试）
```

失败策略：**403 `bad_origin`**，不透露期望值。
⚠️ 注意 `SameSite=Lax` 已能挡住大部分跨站 POST；这道校验是纵深防御，
针对的是"同站但不同子域"与"浏览器扩展"这两类。

---

## 4. 会话吊销（当前完全缺失）

### 4.1 问题

无状态令牌 + 无吊销表 ⇒ **登出后 12 小时内旧 cookie 仍然有效**。
若 cookie 被复制（例如从浏览器导出），只有等 TTL 过期。

### 4.2 两个方案

| 方案 | 做法 | 代价 | 建议 |
|---|---|---|---|
| **A. 加会话表（推荐）** | 新表 `ui_session(sid, created_at, expires_at, revoked_at, ip_hash, ua_hash)`；登录写一行；`readSession` 额外查 `revoked_at IS NULL AND expires_at > now`；登出 `UPDATE revoked_at` | 每次请求多一次 D1 读（可加 KV 缓存，本轮不引入）、需迁移 `0008` | ✅ 推荐：可吊销 + 可审计"谁在什么时候登录" |
| B. 缩短 TTL + 轮换 | 把 12 h 降到 1 h，刷新时续期 | 改动小，但**仍然不能立刻吊销**，且用户频繁重登 | ❌ 不解决根本问题 |

**方案 A 的降级策略**：D1 查询失败时 **fail-open**（允许通过）还是 **fail-closed**（拒绝）？
⇒ 选 **fail-closed 但仅对写入口**：读页面允许降级（体验优先），
写命令必须拿到会话有效性的明确答复（安全优先）。这条要写进代码注释。

---

## 5. 限流（必须分桶）

| 桶 | 键 | 阈值（建议） | 触发后 |
|---|---|---|---|
| 登录失败 | `ipHash` | 5 次 / 10 min（沿用现值） | 429 + `LIMIT_DOC` |
| 登录尝试（含成功） | `ipHash` | 30 次 / 10 min | 429 |
| **命令下发** | `ipHash + kind` | 20 次 / min；**危险类 6 次 / min** | 429 `rate_limited` |
| 读接口 | `ipHash` | 120 次 / min | 429 |

★ **必须新建独立实例**（`createRateLimiter({...})`），不能复用 `loginLimiter`：
- 共用会导致"登录失败 5 次 ⇒ 连命令也发不出去"
- 危险类单独更严，是因为"开阀"的代价与"查询"不同

★ 现状局限必须如实标注：**per-isolate 内存**，冷启动归零，不是全局配额。
真正的跨实例配额需要 Durable Object / KV（本轮**不引入**）。

---

## 6. 命令审计

### 6.1 为什么不能写 `device_event`

`device_event.type` 的 CHECK 白名单固定 13 种（`register`…`retire`），**不含**命令类；
D1 不支持改 CHECK（需重建表，风险高）。

### 6.2 拟议迁移 `0008_command_audit.sql`（**本轮不创建、不应用**）

```sql
CREATE TABLE IF NOT EXISTS command_audit (
    id            INTEGER PRIMARY KEY AUTOINCREMENT,
    device_id     TEXT    NOT NULL,
    cmd_id        TEXT    NOT NULL,
    kind          TEXT    NOT NULL,      -- action | workflow | config | query | system
    object        TEXT,                  -- VALVE_OPEN / wf_valve_20g / weight.enable
    params_json   TEXT,                  -- 脱敏后的参数（见 §6.4）
    state         TEXT    NOT NULL,      -- 见 §6.3
    http_status   INTEGER,
    publish_ok    INTEGER,               -- 1/0/NULL
    device_s      INTEGER,               -- 设备回帧 s
    device_e      INTEGER,               -- 设备回帧 e
    device_m      TEXT,
    instance_id   INTEGER,
    actor         TEXT    NOT NULL,      -- 'ui' | 'system'
    actor_sid     TEXT,                  -- 会话 sid 前 8 位
    actor_ip_hash TEXT,                  -- session.js 的 ipHash
    created_at    INTEGER NOT NULL DEFAULT (unixepoch()),
    updated_at    INTEGER
);

CREATE UNIQUE INDEX IF NOT EXISTS idx_cmd_audit_cmdid ON command_audit(device_id, cmd_id);
CREATE INDEX IF NOT EXISTS idx_cmd_audit_time ON command_audit(device_id, created_at DESC);
CREATE INDEX IF NOT EXISTS idx_cmd_audit_state ON command_audit(state, created_at DESC);
```

**兼容策略**：纯新增表 + 新增索引，**不触碰任何既有表/索引**；
`CREATE ... IF NOT EXISTS` 可重复执行；回滚 = `DROP TABLE command_audit`（不影响其它功能）。

### 6.3 审计条目状态机（与 `05` 的九态对齐）

```
accepted → published → acked → queued → succeeded
                    ↘ failed
        ↘ rejected（HTTP 4xx/5xx）
        ↘ expired / unknown
```

**每次状态变更 `UPDATE` 同一行**（按 `(device_id, cmd_id)` 唯一索引），
`updated_at` 记录最后变更；**不做"每阶段插一行"**（查询会变复杂）。

### 6.4 敏感信息处理

| 字段 | 处理 |
|---|---|
| `p` 里的 `password` / `BLE_key` / `wifi.password` / `mqtt.password` | **整体替换为 `"***"`**，长度也不记 |
| `device_id` | 保留（是路由键） |
| 客户端 IP | **只存 `ipHash`**（8 字节 hex），永不存原文 |
| cookie / token | **绝不入库、绝不进日志** |
| 会话标识 | 只存 `sid` 前 8 位 |

★ 注意：`config_query` 的**上行**回帧里含 `wifi.password` 等明文
（`index.js` 已有 `redactSecrets()` 处理 console.log）。
审计只记录**下发**的参数，且同样脱敏；**不复制上行回帧**。

### 6.5 操作人身份

- `actor = "ui"`（经 `ui_session` 登录）+ `actor_sid` + `actor_ip_hash`
- 设备/系统自发动作（如 `weight_decrease` 触发）**不进 `command_audit`**
  （它们不是人发起的；要看请用 `log_history` 的 `WF_*` 事件）

### 6.6 审计查询与保留

| 项 | 设计 |
|---|---|
| 查询 | `GET /api/admin/device/:id/audit?from&to&state&kind&limit&before_id`（需新增，M2） |
| 排序 | `created_at DESC, id DESC` |
| 保留 | 与现有 retention 一致：**90 天**（与 `history` 同级），由既有 Cron 扩展（M2 评估） |
| 清理失败的审计 | **不允许删**（审计价值高于存储成本）；`rejected`/`unknown` 永久保留 |

---

## 7. M1 验收（必须分层，HTTP 200 不算数）

| 层 | 验收项 | 方法 | 通过标准 |
|---|---|---|---|
| **L1 HTTP** | 匿名 `POST {WORKER_BASE}/` + `source:cloud_api` | 无 header 请求 | **401**（不是 200） |
| L1 | 无会话 `POST /api/command` | 无 cookie | **401 JSON**（不是 302 HTML） |
| L1 | 有会话 + 正确 Bearer ⇒ 正常 | UI 发一条 `query_state` | 200 |
| L1 | 跨源 Origin | 伪造 `Origin: https://evil.example` | **403 `bad_origin`** |
| L1 | 限流 | 1 分钟内发 7 次开阀类 | 第 7 次 **429** |
| **L2 Worker** | 发布失败可见 | 造非法 topic（不可能，用不存在设备 id） | 502 `publish_failed` 原样回传 |
| **L3 MQTT** | 下发到正确 topic | `wrangler tail` 看 `[cloud_api] published dev=… topic=guo_feeder/<id>/down` | topic 与设备订阅一致 |
| **L4 设备** | 设备**真的收到** | 串口看 `[Cloud] MQTT RX {"c":"query",…}` | 有 RX 行 |
| L4 | 设备**真的回帧** | 串口看 `[Cloud] UP-RESULT` | 有回帧 |
| L4 | `cmd_audit` 有终态 | 查 `command_audit` | `state=succeeded`，`device_s=0` |

### 7.1 安全验收用哪些操作（**不得用开阀**）

```
✅ 第一顺位：query_state（valve_status）—— 完全无害，必然成功
✅ 第二顺位：VALVE_CLOSE —— 安全方向，即使真执行也只是关
❌ 禁止：用 VALVE_OPEN 做"鉴权是否通"的验收
```

理由：鉴权改造是**基础设施变更**，验收时不应同时引入"可能放水"的变量。
开阀只在前两步全绿、且人在设备旁时，作为**唯一一次**端到端确认。

### 7.2 三源交叉（因为 `G4-R5` 已知漏记 ~9%）

任何"设备执行成功"的结论，必须三条都对齐：
1. 串口 `[Cloud] UP-RESULT`（设备侧真实）
2. `command_audit.device_s/device_e`
3. `log_history` 里对应的 `WF_*` / `VALVE_*` 事件

**只用 `mqtt_messages` 一行做判据 = 不合格**（历史上已因此误判过）。

---

## 8. 待你裁决的问题（M1 前必须定）

| # | 问题 | 选项 |
|---|---|---|
| Q1 | 会话吊销方案 | **A. 加 `ui_session` 表（推荐）** / B. 只缩短 TTL |
| Q2 | D1 查询失败时写入口行为 | **A. fail-closed（拒绝）** / B. fail-open |
| Q3 | 限流阈值 | 采用 §5 建议值 / 你另定 |
| Q4 | `command_audit` 保留期 | **90 天（推荐）** / 永久 / 其它 |
| Q5 | 是否允许"多用户"（现在只有一个口令） | 单用户（现状）/ 多用户（要改登录体系，不在 M1） |
