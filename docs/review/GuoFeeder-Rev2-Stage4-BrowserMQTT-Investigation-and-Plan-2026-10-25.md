# 阶段四修订 · 浏览器 MQTT 直连 —— 调查报告与实施方案（2026-10-25）

- 依据：项目负责人确认的产品决策（浏览器 WSS 直连 EMQX · Worker 签发/轮换凭据 · 账号池 · 定期轮换）
- 本文 = 阶段 1（调查）+ 阶段 2（方案）。代码实施与测试结果见后续报告。
- 复验脚本：`Cloudflare_Assets/scripts/p4r_emqx_probe.py`（轮换全流程）、`p4r_emqx_acl_probe.py`（ACL 授予有效性）
- ★★ 本阶段**推翻了一条阶段四存档结论**，它直接改变了架构（见 §1.2）

---

## 一、执行摘要

### 1.1 可以做什么（已实测）

| 能力 | 结论 | 证据 |
|---|---|---|
| 浏览器 WSS 连 EMQX | ✅ 8084 开放 · 101 握手 · 子协议协商 mqtt | p4r 探针 §1 |
| Worker 建/改/删 MQTT 用户 | ✅ 201/200/204 | p4r 探针 §2；`emqx-admin.js` |
| **Worker 授予/撤销用户级 ACL** | ✅ **PUT/DELETE 204，且授予后订阅/发布实际放行、撤销后实际拒绝** | p4r_acl_probe §3–§6 |
| 按 username 找会话并踢掉 | ✅ `?username=` 可用 + 本地复核兜底；逐 `DELETE /clients/{id}` | p4r 探针 §4–§5 |
| 轮换后旧连接确实断开 | ✅ 改密 4.1 s 内旧连接消失（HTTP 列表 + 客户端 EOF 双证据） | p4r 探针 §5e/§5f |
| 轮换后旧密码失效 | ✅ rc=4；新密码 rc=0 | p4r 探针 §5g/§5h |

### 1.2 ★ 对阶段四存档结论的修正（必读）

阶段四报告记载「ACL 管理 API 全 403 ⇒ 浏览器 MQTT 无法自动化签发 ⇒ 选型维持 Worker 代理」。
**该结论不准确**：403 的是 `/authorization/settings`、`/authorization/sources`（列表）、
`/authorization/cache`、旧 `/acl` 等**管理面**端点；而**用户级规则端点**
`PUT/DELETE /authorization/sources/built_in_database/rules/users/{username}`
**可用（204）且真实生效**——这也与生产事实自洽：设备账号 `GuoFeederDevice` 的 6 条 ACL
正是 Worker 经该端点写入的（`credential.js::buildAclRules` + `replaceUserAclRules`，
P0 期即实测 200/204）。

⇒ **账号池可以全自动管理（建号 + 最小权限 ACL + 改密 + 踢连接），无需项目负责人在
EMQX 控制台手工授权。** 控制台操作清单从「必需」降级为「核对项」（§3.7）。

### 1.3 轮换的两个已实测行为（设计输入）

1. **改密不踢连接**（复验仍成立）：PUT 新密码后，旧连接照常在線（PINGRESP 正常）。
   ⇒ 轮换流程**必须**显式踢连接，且必须验证踢成功（§3.3）。
2. **ACL 授权按 (连接, topic) 缓存**：同一连接先被拒的 topic，授予后仍拒（需重连）；
   同一连接订阅**新** topic 立即享受新授权。⇒ 授予 ACL 后应主动踢一次该账号连接，
   让浏览器立刻拿到新权限（与「改完 ACL 必须重启设备」的既有结论同源）。

---

## 二、阶段 1：现状调查

### 2.1 三仓基线（2026-10-25）

| 仓 | 路径 | HEAD | 状态 |
|---|---|---|---|
| 云 | `Cloudflare_Assets` | `31a2b59`（阶段四：零产品代码） | clean（仅 `scripts/enforce_watch.sh` 未跟踪） |
| 固件/文档 | `PIO_Assets/Guo_Feeder_Project` | `f31c5e7` | clean |
| 生产 Worker | `guo-feeder-api` | **`c6720d7f`** @100%（10-24T18:12Z，阶段三） | 与本地基线一致（阶段四无产品改动）；回滚目标 `66d45848` |
| 生产 Pages | `guo-feeder-pagesdev` | `897f151e`（阶段二） | 未变 |
| 生产 D1 | `guofeeder-v2`（14 表，0000–0009 全应用） | — | 旧库 `0d8385ba` 只读保留 |

### 2.2 Pages 登录机制（代码事实，非设计文档）

- **登录**：`POST /api/auth/login` `{password, turnstile_token}`。
  口令 = 环境变量 `UI_PASSWORD`，**常量时间比较**；失败统一 350 ms 延迟 +
  per-isolate 限流（5 次/10 min，`LIMIT_DOC` 明示非全局配额）；429/401 **不区分**
  口令错与验证码错的文案（只在 `captcha_failed` 提示验证码状态）。
- **Turnstile**：**服务端** siteverify（secret 模式、hostname 白名单、action=`login`、
  fail-closed、10 s 超时）——代码层面确实服务端验证。
  ⚠ **生产当前未配置 `TURNSTILE_SECRET`**（`prod_verify.py` 仅用口令即可登录生产），
  即**生产登录此刻没有人机验证**。属既有部署缺口，列为控制台待办（§5-E），
  但不阻塞本任务（凭据接口的安全不依赖 Turnstile，见 §3.5）。
- **会话**：cookie `gf_session`（HttpOnly/Secure/SameSite=Lax，12 h）。
  生产 `SESSION_STORE=d1` ⇒ 形态 A（`sid.secret`，D1 可撤销，`ui_session` 表）；
  远端不可用且未设 `SESSION_REQUIRE_REMOTE=1` 时降级形态 B（HMAC，不可撤销，响应可见）。
- **CSRF**：`gf_csrf` cookie + `X-CSRF-Token` 头，HMAC 绑定 sid；**写操作强制**
  （`/api/command` 对形态 B 明确拒绝）。
- **中间件**：只保护 `/app.html`、`/app`、`/`（含 clean-URL 双形态）；其余全部放行
  （含所有 `/api/**`）—— 机器入口不因会话中断。
- **命令代理（回退路径）**：`POST /api/command` = 会话(fail-closed) + CSRF +
  服务端生成 cmd_id + 拒旧协议字段(`c/k/v/ob`) → Worker `/api/ui/command`（Bearer）。
- **只读代理**：`GET /api/read/<精确白名单>` → Worker（Bearer）；零 CORS，同源。
- **未登录能否绕过取凭据**：当前**不存在** MQTT 凭据接口（本任务新建）。既有 `/api/command`
  已按上述链路保护；`/api/read/*` 需会话。**不存在「知道接口地址即可调用」的面**。

### 2.3 Worker 认证与 EMQX API 能力

- Worker 自身零 CORS；浏览器永不直连（Pages 注入 `ADMIN_API_TOKEN` Bearer）。
- EMQX Admin API：Basic（`EMQX_APP_ID`/`EMQX_APP_SECRET`，均为 Secret，不进仓）；
  base = `https://n302933b.ala.cn-hangzhou.emqxsl.cn:8443/api/v5`（可用 `EMQX_API_BASE` 覆盖）。
- 可用：用户 CRUD、`ensureUserPassword`（409→PUT 对齐）、用户级 ACL PUT/DELETE、
  `DELETE /clients/{id}`、`/clients?like_clientid|username|conn_state`、`POST /publish`。
  不可用：`/authorization/settings|sources(列表)|cache`、`GET /status`（403/405）。
- 既有防泄漏设施：`redact.js`（`redactSecrets` 纯函数脱敏，ingest 全路径使用）；
  `emqx-admin.js` 不打印任何请求/响应内容。
- 限流：`rate-limit.js` 分桶（read/command/danger/estop/forceRestart），key=`bucket|actor|device`，
  per-isolate 内存，语义随 429 回传。
- 审计：`command_audit`（HTTP 命令路径）、`mqtt_messages`（帧级，up/down）、`device_event`。
- Cron：`0 3 * * *`（retention + bootseq guard + UI 清理，后两者默认关）。

### 2.4 MQTT Topic 清单（逐项来自代码，禁止猜测）

来源：固件 `src/services/topic_renderer.h`（模板常量）、`cloud_manager.cpp`（订阅/发布）、
Worker `command-api.js`（下行 topic）、`credential.js::buildAclRules`（设备 ACL）。

| Topic | 生产者 | 消费者 | 用途 |
|---|---|---|---|
| `guo_feeder/<device_id>/down` | 浏览器（新）/ Worker（回退） | 设备 | **唯一命令下行通道**（action/workflow/safety/system/query/wfadmin 压缩帧） |
| `guo_feeder/<device_id>/up` | 设备 | Worker（ingest）/ 浏览器（新） | 状态/结果/ACK 帧上行 |
| `guo_feeder/<device_id>/log` | 设备 | Worker（log_history）/ 浏览器（新） | 日志帧上行（停等协议） |
| `guo_feeder/<device_id>/log/ack` | Worker（log-ack.js） | 设备 | 日志回执；**浏览器不需要** |
| `homie/5/<device_id>/#`、`homie/5/<device_id>/+/+/set` | 设备 | Homie 桥 | 设备既有 Homie 命名空间；**浏览器不需要** |
| `$tenants/<t>/guo_feeder/<device>/{up,down,log}` | broker 内部 | Serverless 桥/规则 | 平台副本站内可见；**浏览器不直接订** |

### 2.5 设备账号 vs 浏览器账号

| | 设备账号 | 浏览器账号池（新） |
|---|---|---|
| 用户名 | `GuoFeederDevice`（共享，P1 遗留） | `gf_web_01`…`gf_web_05`（每账号独立） |
| 管理方 | 项目负责人（不在本任务改动） | **Worker 全自动**（建/授权/轮换/回收） |
| 权限 | 6 条设备级规则（§2.4 前 4 条 + Homie 2 条） | 3 条通配规则（§3.7） |
| 轮换 | 不在本任务范围（**明确禁止**触碰） | 24 h 逐账号轮换 |

### 2.6 测试覆盖（现状）

`tests/api/` 8 套件 + `tests/pages/` 6 项（含 `prod_verify.py` 生产只读验证）+
Worker `test/current` + `test/real_d1`（必须串行）。根套件 943 PASS/0 FAIL（阶段四）。
**无任何 MQTT 客户端连通性测试**（阶段四仅原生 socket 探针）——本任务补齐。

### 2.7 限制与风险（如实列出）

1. **改密不踢连接**（实测）⇒ 轮换必须显式踢 + 验证，否则旧会话在 keepalive 内存活。
2. **ACL 按 (连接,topic) 缓存** ⇒ 授权变更后需踢连接才全量生效。
3. **command_audit 不覆盖 MQTT 路径命令**：浏览器直连发布的命令不落 `command_audit`；
   帧级痕迹仍在 `mqtt_messages`（down echo + up result）。UI 本地跟踪状态。
   （后续可选：浏览器向 Worker 补报 cmd_id 供审计——v1 不做，避免「又当又立」。）
4. **MQTT 3.1.1 无法观测发布被拒**（无负向 PUBACK）⇒ 测试与实现统一用 **MQTT 5**。
5. **生产无人机验证**（§2.2）——凭据接口不依赖它，但登录面整体强度受此影响。
6. **共享设备账号**（P1）：按设备隔离 ACL 对 `GuoFeederDevice` 无效——不在本任务改。
7. **session minutes**：EMQX Serverless 按连接分钟计费 ⇒ 浏览器必须「按需连接、隐藏即断」。
8. **凭据进入浏览器 = 可被恶意扩展/XSS 读取**（TLS 只保护传输）——已批准的设计取舍，
   在最终报告中明示。

### 2.8 需项目负责人人工完成的事项（重评后）

| # | 事项 | 性质 |
|---|---|---|
| 1 | **核对** EMQX 控制台 → 授权 → `gf_web_*` 用户级规则是否为 §3.7 三条 | 核对（API 已自动授予） |
| 2 | **配置** Pages `TURNSTILE_SECRET` + `TURNSTILE_SITE_KEY`（生产登录人机验证） | 控制台配置 |
| 3 | **配置** Worker Secret `MQTT_POOL_KEY`（32 随机字节 base64） | 控制台配置 |
| 4 | （可选）真机只读查询验证设备 topic 发布授权 | 需授权的真机操作 |

**不再需要**：为浏览器账号手工建号/配 ACL（API 全自动）。

---

## 三、阶段 2：实施方案

### 3.1 账号池规模与状态模型

- **规模 5**（`MQTT_POOL_SIZE` 可配 3–5 之外的值亦可，代码不写死）。
  依据：① 单口令应用，常态 1–3 个并发标签页（桌面+手机+备用）；② EMQX session-minutes
  预算要求控制并发连接数；③ 5 个账号 + 24 h 周期 ⇒ 轮换错峰 ≈ 4.8 h/个，
  单次踢连接只影响一个账号的会话；④ 超过池大小的并发 ⇒ 明确 503 + UI 提示走回退路径，
  **不静默共享**。
- 用户名 `gf_web_01`…（`MQTT_POOL_USER_PREFIX` 可配），**创建后不变**（只轮换密码）。
- **状态机**（D1 `mqtt_browser_account.state`）：

| 状态 | 含义 | 可分配 | 进入条件 |
|---|---|---|---|
| `available` | 可分配 | ✅ | 轮换完成 / 租约过期被回收 |
| `leased` | 已分配使用中 | ❌（租约过期可接管） | 凭据接口成功分配 |
| `rotating` | 轮换中 | ❌ | 轮换开始（CAS） |
| `failed` | 失效，需人工 | ❌ | 连续失败达上限 |

「状态不明禁止分配」由两条规则保证：① 分配**只认** `state='available'`；
② 恢复扫描把停留超时的 `rotating` 重新驱动到完成或改判 `failed`——任何中间态都不可分配。

### 3.2 密码存储与加密

- **AES-256-GCM**（WebCrypto， Workers 原生）。每次加密生成**全新 12 字节随机 nonce**，
  `ciphertext || nonce` 以 BLOB 存 D1；同密钥下 nonce 不重复（随机 96-bit，冲突概率可忽略）。
- 密钥 = Worker Secret **`MQTT_POOL_KEY`**（base64 的 32 字节；仅存在于 Worker 运行时，
  **不进 Git、不进日志、不进测试报告**）。
- 明文密码只在「生成→PUT 到 EMQX→(可选)返回给浏览器」的内存窗口存在；
  **不写日志、不写 localStorage、不出现在任何响应（除凭据接口本身的 password 字段）**。
- 密钥轮换/恢复/备份策略：换钥 = 管理端点用新钥重加密全部行（原子批处理）；
  密钥丢失 = 全部账号不可解密 ⇒ 批量置 `failed` + 人工重建（删除 EMQX 用户后重新 bootstrap）。
  D1 备份导出只含密文，无钥即无用（这是特性，文档写明）。

### 3.3 轮换周期与流程

- **周期 24 h**（`MQTT_POOL_ROTATE_HOURS` 可配）。理由：浏览器会话本身短命
  （UI 会话 12 h、标签页级 MQTT），24 h 把泄露窗口压到一天内；5 账号错峰后
  平均 4.8 h 才一次踢连接，用户无感；相比「每次登录轮换」，24 h 的 EMQX 写/踢
  频率低两个数量级，且不给「多标签页互相踢」提供放大器。
  「每次会话轮换」已被任务单否决（成本=每次登录一次改密+踢全账号会话；风险=会话中途被踢）。
- **触发**：Cron 每小时第 5 分钟检查（`5 * * * *`，与 retention 的 `0 3 * * *` 分开，
  按 `controller.cron` 分派，retention 行为零变化）；另提供人工触发端点（管理员 Bearer）。
- **逐账号流程**（一次 cron 至多轮换 1 个账号；单账号周期 ≥ 周期/池大小，防 bootstrap 后齐爆）：
  1. 选 `password_updated_at` 最旧且状态∈{available,leased} 且距上次轮换 ≥ 周期的账号；
  2. CAS `→ rotating`（`rotation_started_at=now`）；冲突（已被别人转走）⇒ 跳过；
  3. 生成新密码 → `ensureUserPassword`（PUT；失败进重试，不改状态以外的任何东西）；
  4. **重放 ACL**（PUT 同 3 条规则；幂等，防有人手工改过）；
  5. 按 username 找会话（`?username=` + 本地复核兜底）→ 逐 `DELETE /clients/{id}`（404 算成功）；
  6. **验证断开**：轮询 `/clients?username=…&conn_state=connected` 至空（≤10 s，实测 4.1 s）；
     仍非空 ⇒ 重试踢一次；仍非空 ⇒ 判 `failed`（密码已改但旧会话存活，**禁止分配**）；
  7. 加密存储新密码 → `→ available` + `last_allocated_at` 保持 → 写 `mqtt_browser_rotation` 日志行。
- **硬安全边界**：轮换只接受 `^gf_web_\d{2}$` 形状的用户名（与设备账号 `GuoFeederDevice`、
  `dev_*` 形态互斥）；踢连接**只按 username 精确匹配**，绝不用 clientid 前缀扫射
  （设备 clientid `dev_<id>` 永不被误伤）；不改设备账号任何属性。

### 3.4 并发与失败恢复

- **分配并发安全**：D1 单行 CAS（`UPDATE … SET state='leased' WHERE username=? AND state='available'`，
  受影响行数=0 ⇒ 重选下一个）。两个请求不可能拿到同一个 `rotating`/`leased` 账号。
- **租约**：`lease_expires_at`（默认 30 min，`MQTT_POOL_LEASE_MINUTES` 可配）。
  每次凭据请求续租；过期后 `available` 可被他人接管（旧浏览器连接仍在也没关系——
  下次它重连会重新取凭据；被踢是预期行为）。
- **中断恢复**：cron 启动时先跑恢复扫描——`rotating` 且 `rotation_started_at` 早于
  2×周期 ⇒ 视为上次中断：直接**重新驱动**（再生成新密码 PUT——PUT 是覆盖语义，
  天然修复「改了密没存密」的半成品状态），走完 §3.3 全流程；连续 3 次失败 ⇒ `failed` + 告警日志。
- **API 超时**：所有 EMQX 调用带 `timeoutMs`（`emqxRequest` 已支持）；超时=结果未知 ⇒
  不标记成功，进入重试/恢复链，**绝不把超时当成功**。

### 3.5 登录与凭据接口设计

```
浏览器 ──GET /api/mqtt/credentials (cookie 会话)──► Pages Function
  会话校验 readSessionFull（fail-closed）· per-isolate 限流 20/min/ip_hash
  ──Bearer ADMIN_API_TOKEN──► Worker GET /api/ui/mqtt/credentials
  账号池分配（§3.4）· 解密 · 返回
```

返回：`{username, password, broker_url:"wss://…:8084/mqtt", client_id, topics:{down,up,log}, lease_expires_at}`。
`broker_url` 由 `EMQX_API_BASE` 推导（可用 `MQTT_WSS_URL` 覆盖）；`client_id` 服务端生成
（`gfwb_<12hex>`，唯一可辨识）。

- **为什么 GET 也要防**：SameSite=Lax + 同源 + 无 CORS 头 ⇒ 跨站读不到响应；
  限流防会话内滥用；接口**不**依赖 Turnstile（生产未配也不失守——失守的只是登录面，见 §2.7-5）。
- **不泄露**：响应除密码本身外无秘密；日志只记 `username`/`sid`/结果，**绝不记 password**；
  错误信息不区分「池空」与「内部错误」以外的细节。
- **生命周期**：凭据只在浏览器内存（模块级变量）；刷新/关标签页/登出即消失；
  证书过期（被踢）→ 前端自动重新获取一次 → 仍失败则提示并建议回退 HTTP 路径。

### 3.6 浏览器 MQTT 生命周期

- 库：**MQTT.js v5**（vendored 到 `assets/js/vendor/mqtt.min.js`，无 CDN 依赖、无构建）。
  用 v5 的原因：发布被拒可观测（PUBACK rc=0x87）。
- **命令通道开关**：UI 设置项 `gf.cmd_path` ∈ {`http`（默认，回退路径）, `mqtt`}；
  一个命令只走一条路（`runCommand` 按开关分派，切换原子，在途命令按原路径完成）。
  **验收完成前默认 `http`，现有代理路径一行不删。**
- 连接策略：仅当 `gf.cmd_path=mqtt` 且页面可见时连接；隐藏 >60 s 主动断开；
  `beforeunload`/登出断开；断线重连带退避；被踢（rc=0x87/not authorized）→ 自动重取凭据 1 次。
- 多标签页：上限=池大小（5）；第 6 个请求 ⇒ 503 ⇒ UI 提示「MQTT 通道已满，请关闭其他
  标签页或改用 HTTP 通道」。不用 localStorage 选主（选主会让非主标签页的命令延迟/丢失，
  违背「命令直达」的产品决策）。
- 帧构造：端口 `buildDeviceFrame`/`buildQueryFrame` 到 `gf-ui-core.js`，
  **与 Worker 实现逐字节一致**（有对等测试，§3.8-E）；target（v/k）来源不变（设备 registry）。
- 状态跟踪：订阅 `guo_feeder/<id>/up`，按 `i`(=cmd_id) 匹配，`s` 码映射四态
  （0 done / 1 accepted / 2 running / 3 failed / 4 error），与 `stateFromFrame` 同源语义。

### 3.7 EMQX 授权清单

**A. 浏览器账号池（Worker 自动授予，每条规则都可从代码指出处）**

| 账号 | Topic | 操作 | 权限 | 用途 |
|---|---|---|---|---|
| `gf_web_01`…`gf_web_05` | `guo_feeder/+/down` | Publish | Allow | 向任意（含未来）设备下发命令 |
| `gf_web_01`…`gf_web_05` | `guo_feeder/+/up` | Subscribe | Allow | 接收设备状态/结果帧 |
| `gf_web_01`…`gf_web_05` | `guo_feeder/+/log` | Subscribe | Allow | 接收设备日志帧 |

- 通配 `+` 的理由：任务要求「所有已配置设备」且「不为第二台设备重建账号池」——
  `guo_feeder/+/` 正是本项目设备 topic 命名空间（§2.4），不触碰 `$SYS/#` 与任意 topic。
- 不授予：`/log/ack`（Worker 专属）、`homie/…`、`$tenants/…`、`$SYS/#`。
- 测试账号（`gf_bmqtt_*`，仅自动化测试用）额外授予 `gf_bmqtt_selftest/#`（all）
  ——生产池**不**含此条（最小权限）。

**B. 设备账号 `GuoFeederDevice`：零改动**（6 条既有规则原样保留，本任务禁入）。

**C. 控制台核对项**（API 已做，人工只需核对）：授权 → 用户级规则 → 上述 3 条 ×5 账号存在。

### 3.8 自动化测试方案（对应任务 §6.1 A–E）

| 组 | 内容 | 方式 |
|---|---|---|
| A 登录 | 对/错口令、未登录取凭据 401、Turnstile 服务端验证（现有+新增断言）、凭据端点限流 | Pages 单测 + 本地 pages dev 集成 |
| B 凭据 | 授权会话可取、响应格式、无明文泄露（日志捕获断言）、匿名不可绕过、并发分配不重号 | Worker 单测 + Pages 集成 |
| C 真实 EMQX | MQTT.js v5 经 **真实 WSS**：CONNACK/SUBACK/发布确达（跨连接收到）/未授权发布被拒（0x87）/唯一 clientId/零残留 | Node + vendored mqtt.min.js + 真实 broker |
| D 轮换 | 旧密码拒连、旧连接被踢、新密码可连、rotating 期不分配、API 失败恢复、设备账号零影响（前后只读比对） | 本地 Worker（wrangler dev + 本地 D1）+ 真实 EMQX |
| E 路径确认 | MQTT 发布期间 **fetch 间谍**断言零次 `/api/command`；帧构造与 Worker 对等（同一用例表比对） | Node 测试 |

- 真机：**不做任何有副作用命令**（禁喂食/放水/重启/改配置）。设备 topic 发布授权用
  「不存在的设备 id（模式匹配、零订阅者）+ MQTT5 PUBACK rc」证明；真机只读验证列为
  控制台核对项（§2.8-4）。
- Playwright（安装中）用于本地 pages dev 全栈：登录→取凭据→真 WSS 连接→发布/订阅断言。
  若不可用则降级为 Node 直测 + 既有 DOM 桩测试，并在报告中如实标注。

### 3.9 回退策略

1. 代码层：`gf.cmd_path` 默认 `http`；MQTT 模块加载失败不影响页面其余功能。
2. 部署层：Worker 出问题 ⇒ 回滚到 `c6720d7f`（现网版本，零新表依赖——
   新端点/新表对旧代码不可见，旧版本根本不读新表）；Pages 出问题 ⇒ 回滚 `897f151e`。
3. 数据层：`0010` 迁移只新增两张空表 + 索引；回退 = 停用相关端点（表留着无害）。
   账号池在 EMQX 侧的清理：管理端点删 `gf_web_*`（密码作废后本就连不上）。
4. 密钥丢失：§3.2 的人工重建路径。

### 3.10 新增配置 / Secret / D1 / 调度

| 项 | 类型 | 默认 | 说明 |
|---|---|---|---|
| `0010_mqtt_browser_pool.sql` | D1 迁移 | — | 2 张新表（账号、轮换日志）+ 2 索引 |
| `MQTT_POOL_KEY` | Worker Secret | 无（必配） | AES-256-GCM 密钥，base64 32 字节 |
| `MQTT_POOL_SIZE` | var | `5` | 池大小 |
| `MQTT_POOL_USER_PREFIX` | var | `gf_web_` | 用户名前缀 |
| `MQTT_POOL_ROTATE_HOURS` | var | `24` | 轮换周期 |
| `MQTT_POOL_LEASE_MINUTES` | var | `30` | 租约 |
| `MQTT_WSS_URL` | var | 由 `EMQX_API_BASE` 推导 | 浏览器用 broker 地址 |
| crons | wrangler.toml | `["0 3 * * *","5 * * * *"]` | retention 不变；新增轮换检查 |

### 3.11 阻塞项

**无硬阻塞。** 阶段四的 ACL 403 阻塞经复验已消除（§1.2）。
软依赖：`MQTT_POOL_KEY` 需项目负责人在 Cloudflare 控制台配置后才能部署 Worker
（配置前凭据接口返回 503 `pool_unconfigured`，不伪造成功）。
