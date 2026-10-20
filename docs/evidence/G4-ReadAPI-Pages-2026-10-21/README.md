# 读端点 + 同源代理 + 访问保护 + 指标端点（2026-10-21 04:30–04:47）

> 用户授权：「现在开工 读端点 + 同源代理，UI 的访问保护做个极简登录，限制短时间内重试次数，
> 也可以接入免费的验证码平台，以防止机器人访问。顺手给 Worker 加一个只读指标端点。」

## 1. 一句话结果

**UI 的硬前置已解除**：Worker 首次有了只读数据出口，Pages 有了登录/会话/代理三层。
生产已部署（Worker `9cdcdcef-…`、Pages `302edc35…`），**EMQX 写入链路实测未受影响**。
但**还差 4 个环境变量**，配好后 UI 才可用（详见 §6）。

## 2. 文件索引

| 文件 | 内容 |
|---|---|
| `tests-all-suites.txt` | Worker **26 套件全绿**（含新增 `read_api` 43 例），`exit=0` |
| `s4_e2e.py` / `s4-e2e-run.txt` | **端到端 40/40**：Pages → Worker → 本地 D1 真实链路 |
| `deploy1-pre.txt` / `deploy1.txt` / `deploy1-post.txt` | Worker 部署：门禁 / 记录 / 生效验证 |
| `deploy2.txt` / `deploy2-verify.txt` | Pages 部署：记录 / **生产只读验证（含 EMQX 回归）** |

> Pages 的 67 例单元测试在 `Cloudflare_Assets/tests/pages/pages_auth.test.mjs`
> （**不在 Pages 目录内**，否则会被当作静态资源发布）。

## 3. 做了什么

### 3.1 Worker（云端仓 `guo-feeder-api`）

新增 `src/read-api.js`（**全文只有 SELECT**），4 个端点：

| 端点 | 用途 | 关键设计 |
|---|---|---|
| `GET /api/history/telemetry` | 逐条遥测 | keyset 分页（`id < before_id`），**不用 OFFSET**（并发写入会错位） |
| `GET /api/history/log` | 逐条日志 | 同 keyset；支持 `device_id`/`level`/`from`/`to` 过滤 |
| `GET /api/admin/device/:id/snapshot` | 单设备聚合 | 用 `ROW_NUMBER() PARTITION BY type` 取每类最新（`GROUP BY+MAX` 取不到对应行） |
| `GET /api/admin/metrics` | **`invalid_total` 唯一出口** | 响应**强制自带** `scope:"isolate"` / `persistent:false` |

参数纪律：`limit` 默认 200 上限 1000（钳位不报错）；**非法一律 400，不静默降级**；
`from`/`to` 单位是**秒**（实测 `created_at - ts` 仅 0~24 秒，写毫秒会得到空集）。

路由用**精确路径白名单**（`path === "/api/history/telemetry"`），不用 `startsWith`
⇒ 不会把既有的 `POST /api/history/ingest`（走 X-Webhook-Key）吞掉。测试 `R-8-7` 专门防这个。

### 3.2 Pages（云端仓 `guo-feeder-pagesdev`）

| 文件 | 作用 |
|---|---|
| `functions/_lib/session.js` | HMAC-SHA256 无状态会话、常量时间比较、限流器、IP 哈希 |
| `functions/api/auth/login.js` | 口令登录 + 可选 Turnstile + 失败限流 + 恒定延迟 |
| `functions/api/auth/session.js` | 会话状态 + 下发 Turnstile **site key**（公开值） |
| `functions/api/auth/logout.js` | 清 cookie |
| `functions/api/read/[[path]].js` | **同源代理**：服务端注入 Bearer，浏览器永不见令牌 |
| `functions/_middleware.js` | 只保护页面，**默认放行**（见 §5） |
| `login.html` / `app.html` | 登录页 / 最小只读看板 |

**代理白名单**（精确正则，防把 Pages 变成开放代理）：
`history/telemetry` · `history/log` · `admin/metrics` · `admin/device/<12hex>/snapshot`
实测 `admin/reconcile`、`device/<id>/revoke`、`history/ingest` 等**全部 403**。

## 4. 端到端验证（40/40，真实链路，非 mock）

```
Pages dev 127.0.0.1:8793  →  Worker dev 127.0.0.1:8794  →  本地 D1（--local）
```

关键实测：
- 未登录：`/app.html` → 302 登录页；`/api/read/*` → 401
- 登录：错口令 401（带剩余次数）· 正确 200 + `gf_session`（HttpOnly/Secure/SameSite=Lax）
- 代理 → 真实 SQL：`telemetry_history=5`、`counts.telemetry=5`、按 `level=WARN` 过滤得 1 条、
  快照 3 类遥测各取最新、`ingest.scope=isolate`、`mode=observe`
- 白名单：5 条危险路径全 403
- 限流：连续失败 `codes=[401,401,401,401,429,429]`，**封禁后即使口令正确也 429**
- 登出：`Max-Age=0`

## 5. ★ 端到端抓到的一个真漏洞（已修）

`B-8` 初始失败：`/app.html` 返回 **308**。原因不是 bug，是 **Pages 的 clean-URL**：
`/app.html` 先 308 到 `/app`，而 `/app` **不在保护名单**里 ⇒ **未登录可直接访问，绕过登录**。

单元测试没抓到它（构造 Request 不触发 dev server 的重定向），**只有真实 dev server 才暴露**。
修复：`PROTECTED_PAGES` 同时含 `/app.html` 与 `/app`，并补 `B-9`/`B-10` 与单测 `M-2-4`/`M-2-5` 防回退。

另一处同类陷阱：`[[path]].js` 里 `params.path` **不含** `api/read`（曾因此白名单全线 403）。

## 5.5 ★ 密钥配置 + 生产验证（`05:16–05:20`，已解除阻塞）

| 步骤 | 结果 |
|---|---|
| 上传 3 枚 Pages secret | `secret-put.txt`：**3 secrets successfully uploaded**；`secret-list.txt` 确认三键存在（Value Encrypted） |
| 重新部署 Pages | `deploy3.txt`：`05:17:33`、`exit=0` |
| **生产验证** | `prod-verify.txt`：**22/22** |

生产实测（真实数据，非 mock）：

```
表规模   mqtt_messages=5772 · telemetry_history=20 · log_history=834 · device=10 · device_event=266
设备     288485896ce4  state=REGISTERED  counts={telemetry:20, log:834, event:179}
ingest   scope=isolate · persistent=false · mode=observe
         requests_observed=0 · invalid_total=0 · unauth_write_attempts=0 · unresolved_total=0
★ EMQX   /api/webhook 仍 200 "MQTT Router OK"
```

★ **`invalid_total=0` 不能当作「0 次非法投递」** —— 同一份响应里 `requests_observed=0`，
说明这个 isolate 冷启动后**还没处理过任何入站报文**。0 是「没观测到」，不是「观测到 0 次非法」。
切 `enforce` 前必须多 isolate 采样。

**密钥保管**：值**仅**存 `D:\Guo_Feeder_Project\Cloudflare_Assets\.secrets.local`
（gitignore 第 15 行覆盖，`git ls-files` 确认从未被追踪）。文件已按
`[WORKER]` / `[PAGES]` / `[TURNSTILE]` 三段归类。上传走 `pages secret bulk <file>`，
值**不出现在命令行**，因此也不进 shell 历史。本证据目录**不含任何密钥**。

## 6. ~~还差 4 个环境变量~~ → 只剩 Turnstile（可选）

Pages 项目设置 → Settings → Environment variables（production），配完需**重新部署 Pages** 才生效：

| 变量 | 必需 | 状态 |
|---|---|---|
| `UI_PASSWORD` | ✅ | **已配置**（用户指定） |
| `SESSION_SECRET` | ✅ | **已配置**（`secrets.token_hex(32)` 随机生成，64 hex） |
| `ADMIN_API_TOKEN` | ✅ | **已配置**（与 Worker 同值） |
| `TURNSTILE_SECRET` | ⬜ | **未创建** —— 本机无 `CLOUDFLARE_API_TOKEN`，Turnstile 只能经 API/Dashboard 创建，wrangler 无此能力（见 `UI-TURNSTILE-PENDING`） |
| `TURNSTILE_SITE_KEY` | ⬜ | 同上 |

未配置时的行为（**已实测**）：登录返回 401（**不泄露「未配置」**）、代理返回 500 或 401。
⇒ **现有 EMQX 链路完全不受影响**，只是 UI 暂时不可用。

## 7. 生产验证（`04:45:44`，只读）

```
GET /api/webhook         = 200 "MQTT Router OK"   ★ EMQX 入口未受影响
GET /api/history/ingest  = 200
GET /api/presence/event  = 200
GET /app.html  无 cookie = 302 → /login.html?next=%2Fapp.html
GET /app       无 cookie = 302 → /login.html?next=%2Fapp
GET /login.html          = 308（clean-URL 转 /login，浏览器会跟随）
GET /api/auth/session    = {"authenticated":false,"turnstile_site_key":null,"ttl_s":43200}
GET /api/read/admin/metrics = 401
POST /api/auth/login（错口令）= 401 {"error":"unauthorized","remaining_attempts":4}
```

## 8. 版本

- Worker：`4eb8d91b-…` → **`9cdcdcef-4550-4e14-9975-69f9a0c985b5`**（100%，`04:43:06`）
  绑定：`INGEST_VALIDATION ("observe")` · `LOG_BOOT_WATERMARK ("1")` · D1 `0d8385ba-…` **均未变**
- Pages：部署 `04:45:16`，`302edc35.guo-feeder-pagesdev.pages.dev`；生产域名 `guo-feeder-pagesdev.pages.dev`
- 回滚 Worker：`wrangler rollback 4eb8d91b-bdf7-42cb-9e22-81afd3670096`

## 9. 声明

- **未**切 `enforce` · **未**改写入路径 · **未**动 EMQX · **未**烧录 · **未**重启设备 · **未**执行 C-3
- **未**改 D1 schema（零 migration）· **未**写生产数据（全部只读 SELECT）
- 门禁四态未变：`G-4 = BLOCKED` · `WP-5` 未解锁 · `BT-1 = CLOSED` · `STOP-R1` 无解除
- 未验证：生产 `metrics` 的 HTTP 层（`workers.dev` 在本沙箱 DNS 不可达，拿不到 `ADMIN_API_TOKEN` 明文）；
  真实 Turnstile 校验（需用户配 key）
