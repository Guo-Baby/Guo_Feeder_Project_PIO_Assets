# EMQX 账号池 · 用户与 ACL 生命周期 —— 调查结论与可复用操作文档

> 日期：2026-10-25 · **本轮严格只读**：对生产 EMQX 只发 **HTTP GET** 探测；**未执行任何 POST/PUT/DELETE**；未触碰 `gf_web_00..04` / `GuoFeederDevice` / `dev_288485896ce4` / `workbuddy` / `test001` / `shouji`；未轮换任何密码；未改任何 ACL；未创建/删除任何测试账号；未部署；未迁移。
> 配套：`UI-V11-Gap-Audit-and-Implementation-Plan-2026-10-25.md`（§3.10 摘要 + §6 D-8 决策项）
> 目的：把此前"**ACL 不能通过 API 控制**"的错误结论彻底纠正，形成**后续 AI 可直接复用**的账号/ACL/连接操作手册，并给出固定池 vs 动态池的能力对比。

---

## 0. 结论速览

| 问题 | 结论 |
|---|---|
| EMQX 能不能用 API 管用户？ | **能**：建/查/改密/删全部实测可用（阶段四实测 + 本次 GET 复验） |
| EMQX 能不能用 API 管 ACL？ | **能**：整表 PUT + 清空 DELETE 实测可用且**真实生效**（闭环实验证明）。**不能**做"单条规则增删"（API 不存在，只能整表覆盖） |
| 「ACL 不能通过 API 控制」还成立吗？ | ❌ **已被 4 条独立证据链推翻**（§3） |
| 当前 App ID 权限范围 | **受限（scoped）角色**：仅 `…/users`、`…/rules/users`、`/clients` 三组可访问；管理面全 403；**平台配额不可读** |
| 固定池会不会长期累积资源 | **不会**（删号/删 ACL/踢连接/轮换能力齐备，且已在生产跑通） |
| 动态池的真正阻碍 | ① DB `CHECK (username GLOB 'gf_web_[0-9][0-9]')` + `POOL_USERNAME_RE` 双闸；② 平台配额**不可探测**（`/limits` 403）；③ ACL 只能整表覆盖 ⇒ 并发写互相覆盖 |
| 最大的踩坑点 | ① `?clientid=` 精确过滤**静默失效**（200 但 0 条）；② ACL 变更对**已建立会话不即时生效**（授权按 (连接,topic) 缓存）⇒ 必须"改 ACL + 踢连接"成对；③ `GET …/rules` 根路径 **405**、`…/rules/groups` **404** |

---

## 1. 认证、基址与权限边界

| 项 | 事实 | 证据 |
|---|---|---|
| 基址 | `EMQX_API_BASE`（Secret）或默认 `https://n302933b.ala.cn-hangzhou.emqxsl.cn:8443/api/v5` | `emqx-admin.js:45` |
| 认证 | HTTP Basic：`username = EMQX_APP_ID`，`password = EMQX_APP_SECRET` | `emqx-admin.js:19, 54-64` |
| 角色 | **受限（scoped）**，非超级管理员；10 个用户全部 `is_superuser:false` | 本次实测 |
| 可访问面（实测 200） | `GET /authentication/password_based:built_in_database/users`（列表）、`…/users/{id}`（单个）、`GET /authorization/sources/built_in_database/rules/users`（ACL 整表）、`…/rules/users/{username}`（按用户）、`GET /clients`（+`username` / `like_clientid` / `conn_state` / `page` / `limit` 参数） | 本次实测 |
| 不可访问（实测） | `GET /authentication`（403，代码已注明）、`GET /authorization`（403）、`GET /authorization/sources`（403）、`GET /zones` `/listeners` `/api-key` `/limits` `/dashboard` `/node/info` `/metrics`（全 403）、`GET …/rules`（**405**，该路径不接受 GET）、`GET …/rules/groups`（**404**，Serverless 未启用 group 授权源） | 本次实测 |
| 平台配额 | **不可探测**（`/limits` 403）⇒ 账号数/规则数上限**无 API 可查**，只能撞错误码 | 本次实测 |

---

## 2. 操作能力矩阵（逐项：方法/路径/请求体/响应/生产可用性/证据）

### 2.1 用户

| 操作 | 方法 + 路径 | 请求体 | 成功响应 | 生产可用 | 证据 |
|---|---|---|---|---|---|
| 创建 | `POST /authentication/password_based:built_in_database/users` | `{user_id, password, is_superuser:false}` | `201`；已存在 `409 + code:"ALREADY_EXISTS"`（**不覆盖密码**） | ✅ | `emqx-admin.js:175-183`；`p4r_emqx_probe.py:327-330` PASS；`p4r_emqx_acl_probe.py:199-202` PASS |
| 读取（单个） | `GET …/users/{user_id}` | — | `200 {user_id, is_superuser}`（**不含密码**） | ✅ | `emqx-admin.js:225-232`；本次实测 200 |
| 读取（列表） | `GET …/users` | — | `200 {data:[…], meta{count,page,limit,hasnext}}` | ✅ | `emqx-admin.js:238-245`；本次实测 200 / count=10 |
| 改密 | `PUT …/users/{user_id}` | `{password?, is_superuser?}`（**不含 user_id**） | `200 {user_id, is_superuser}` | ✅ | `emqx-admin.js:197-217`；`p4r_emqx_probe.py:366-368` PASS；旧密码立即失效已实测 |
| 删除 | `DELETE …/users/{user_id}` | — | `204` | ✅ | `emqx-admin.js:256-263`；`p4r_emqx_probe.py:420-421` PASS；`p4r_emqx_acl_probe.py:257-258` PASS |

**幂等要点**：`ensureUserPassword()`（`emqx-admin.js:276-305`）的策略是 **POST → 201 新建；POST → 409 则 PUT 强制对齐密码**（因为 POST 409 不覆盖密码）。批量建号时必须走这个顺序，否则会出现"用户存在但密码不是我们生成的那个"。

### 2.2 ACL

| 操作 | 方法 + 路径 | 请求体 | 成功响应 | 生产可用 | 证据 |
|---|---|---|---|---|---|
| 整表覆盖 | `PUT /authorization/sources/built_in_database/rules/users/{username}` | **`{username, rules:[{topic,permission,action}]}`**（必须是**对象**；裸数组 400） | `200/204` | ✅ **已推翻早期 403 结论** | `emqx-admin.js:318-327`；`p4r_emqx_acl_probe.py:211-220` PASS（200/204）；`mqtt_browser_e2e.mjs:264` E-1c PASS |
| 读取（按用户） | `GET …/rules/users/{username}` | — | `200 {username, rules:[…]}`（**顶层 rules**） | ✅ | 本次实测 200；`gf_web_00` 返回恰好 3 条 |
| 读取（整表） | `GET …/rules/users` | — | `200 {data:[{username,rules:[…]}], meta}`（**data[].rules**） | ✅ | 本次实测 200（10 个用户全可见） |
| 清空 | `DELETE …/rules/users/{username}` | — | `204` | ✅ | `emqx-admin.js:336-343`；`p4r_emqx_acl_probe.py:246-247` PASS |
| **单条规则增删** | — | — | — | ❌ **API 不存在** | EMQX v5 用户级 ACL 只有 PUT 整表 / DELETE 清空；`rules[]` 无 rule_id 寻址 |
| 规则组 | `…/rules/groups` | — | — | ❌ 404（Serverless 未启用） | 本次实测 |

⚠️ **两种响应形状并存**：单用户 = 顶层 `rules`；整表 = `data[].rules` ⇒ 解析代码必须分别处理（**本次实测踩过：查 `data.rules` 得到 0 条，误以为"ACL 未配置"**）。

⚠️ **写入形状与读取形状不对称**：`replaceUserAclRules()` 只提交 3 个字段（`topic/permission/action`，`mqtt-pool.js:248-255`），而 EMQX 读回时补齐 `qos:[0,1,2]` 与 `retain:"all"` ⇒ **GET 回来的规则对象不能原样 PUT 回去**。

### 2.3 连接

| 操作 | 方法 + 路径 | 生产可用 | 实测要点 |
|---|---|---|---|
| 列表（全量/分页） | `GET /clients?page=&limit=` | ✅ | `{data:[…], meta{count,page,limit,hasnext}}` |
| 按 username 过滤 | `GET /clients?username=X` | ✅ **过滤真实生效** | `?username=GuoFeederDevice` → 1 条（`dev_288485896ce4`）；`?username=gf_web_00` → 0 条；`?username=NONEXISTENT_zzz` → 0 条（排除"静默返回整页"） |
| 前缀过滤 | `GET /clients?like_clientid=X` | ✅ | `?like_clientid=dev_` → 1 条 |
| 连接态过滤 | `GET /clients?conn_state=connected` | ✅ | 代码兜底路径（`mqtt-pool.js:781`） |
| ⚠️ **clientid 精确过滤** | `GET /clients?clientid=X` | ⚠️ **静默失效** | HTTP **200 但 n=0**，而同一时刻全量列表里该 client 在线 ⇒ **不可用于在线判定** |
| 断开 | `DELETE /clients/{clientid}` | ✅ 204/404 | `emqx-admin.js:361-370`；`p4r_emqx_probe.py:382-385` PASS（踢后 10s 内消失） |

**⇒ `?clientid=` 静默失效是"假离线"的头号来源**。代码已用双段式防御兜住（`presence-emqx.js:24-28`、`emqx-admin.js:394-405`、`mqtt-pool.js:728-736`）：先按 `username` 精确查 → 空则全量 `conn_state=connected` 再本地过滤 ⇒ **不要退回 `?clientid=`**。

---

## 3. ★ 关键结论翻转：「ACL 不能通过 API 控制」已被推翻

| 轮次 | 证据 | 结论 |
|---|---|---|
| 早期存档 | `scripts/p4_emqx_probe.py:7`、`p4r_emqx_probe.py:14`：「本环境 ACL API 全 403，也改不了」 | ❌ 错误结论 |
| 第一次反转 | `p4r_emqx_probe.py:410-413`：PUT 返回 **204**（不是 403） | 与存档冲突，存疑 |
| 第二次反转（闭环） | `p4r_emqx_acl_probe.py`：① PUT 200/204 → ② 授予后同连接订阅 rc≠0x80 → ③ **发布确达 broker** → ④ DELETE 204 → ⑤ **撤销后新连接订阅被拒 rc=0x80** ⇒ **204 不是静默忽略，规则真实生效** | ✅ 推翻 |
| 第三次反转（生产 E2E） | `mqtt_browser_e2e.mjs`：E-1c「ACL 三规则重放 2xx」PASS；C-2a/b「SUBACK /up、/log 被授予」PASS；C-4a/b「越权 subscribe 被拒 / 越权 publish ⇒ PUBACK rc=135」PASS | ✅ 推翻 |
| 第四次反转（**本次独立只读实测**） | `GET …/rules/users/gf_web_00` → 200 且**恰好 3 条**规则 = 代码 `browserAclRules()` 的 down/publish + up/subscribe + log/subscribe；`gf_web_01..04` 完全同构 | ✅ 推翻 |

**推理闭环**：`browserAclRules()` 的三条规则**只可能由 `replaceUserAclRules()` 的 PUT 写入**（代码里没有控制台操作路径）⇒ **生产 ACL 是 API 写进去的** ⇒ 早期结论完全错误。用户控制台目视「5 个账号 + 至少一个新用户 ACL 已配置」与本次实测**完全吻合**。

**代码内已固化新事实**：`emqx-admin.js:31-33` 的端点清单注释写明「PUT …/rules/users/{username}（**P0 实测 200/204**）」「DELETE …（**A-8 实测 204**）」。

**唯一真正的限制**：ACL 变更**对已建立的会话不即时生效**（授权按 (连接,topic) 缓存，`emqx-admin.js:352-354`、`mqtt-pool.js:26-28`）⇒ 必须「改 ACL + 踢连接」成对。

---

## 4. 生产现状（只读实测快照，2026-10-25）

| 用户 | 规则数 | 内容 | 归属 |
|---|---|---|---|
| `gf_web_00` … `gf_web_04` | **3** | `publish guo_feeder/+/down`｜`subscribe guo_feeder/+/up`｜`subscribe guo_feeder/+/log` | **浏览器池**（API 写入，与 `browserAclRules()` 逐字一致） |
| `dev_288485896ce4` | 4 | down/sub、log/ack/sub、up/pub、log/all | 设备账号（测试机） |
| `GuoFeederDevice` | 4 | 同上，但 `log` 为 publish | 设备账号（生产） |
| `workbuddy` / `test001` / `shouji` | 1 | `all` + `guo_feeder/#` | 历史账号（控制台风格，未动） |

**在线会话**：仅 `dev_288485896ce4`（username `GuoFeederDevice`）在线；池账号 0 条在线（与"当前无浏览器直连"一致）。

**状态一致性**：EMQX 侧 5 个池账号 + D1 侧 `mqtt_browser_account` 5 行（`0010` 迁移**已于本日 17:05 应用**）⇒ 一致 ✅。⚠️ `migrations/README.md` 状态表仍写"0010 未应用" ⇒ **文档滞后需更新**（记入审计报告 B-3.10 第 10.6 项）。

---

## 5. 长期资源累积评估（用户核心关切）

### 5.1 固定池（当前实现）——**不累积**

| 生命周期事件 | 谁负责 | 证据 |
|---|---|---|
| 建号 | `bootstrapPool()` 批量建（≤ `MQTT_POOL_SIZE`，默认 5，硬上限 20） | `mqtt-pool.js:305-336`；`MAX_POOL_SIZE=20` `:68` |
| 密码定期轮换（默认 24h） | cron `5 * * * *` → `rotateDueMqttPoolAccounts()`（每次至多 1 个） | `mqtt-pool.js:600-700`；阶段四 D-2 七步实测 PASS |
| 改密后**踢连接** | 两段式（按 username 查 + 全量兜底）→ 逐个 `DELETE /clients/{id}` → ≤10s 复验 | `mqtt-pool.js:728-790` |
| ACL 重放 | 轮换第③步 `replaceUserAclRules()` 整表覆盖（幂等） | `mqtt-pool.js:657` |
| 租约到期归还 | `releaseExpiredLeases()`（cron 维护） | `mqtt-pool.js` 释放逻辑 |
| 账号删除 | 落库失败/半成品时**回收 EMQX 孤儿**（删 ACL + 删用户） | `mqtt-pool.js:357-358, 373-374` |
| 命名空间 | `gf_web_00..19`（DB CHECK `GLOB 'gf_web_[0-9][0-9]'`） | `0010:80`；`POOL_USERNAME_RE` `mqtt-pool.js:58` |

⇒ **固定池的账号集合恒定 ≤20，密码定期轮换，连接按需踢除 ⇒ EMQX 侧不会随时间累积。**

### 5.2 动态池的**新增**能力需求与阻碍

| # | 需求 | 是否具备 | 阻碍 |
|---|---|---|---|
| D1 | 运行时按需建号 | ✅ F1 已具备 | — |
| D2 | 运行时按需删号 | ✅ F3 已具备 | — |
| D3 | 租约到期自动归还 + 回收 | ✅ 代码已有骨架（`release`/`recover`/`cleanup_orphan` 事件白名单） | — |
| D4 | 命名空间扩展（会话哈希等） | ❌ | **DB CHECK + `POOL_USERNAME_RE` 双闸**，需改迁移与代码 |
| D5 | 运行时改池规模 | ⚠️ | `MQTT_POOL_SIZE` 是 Worker Secret，**改 Secret 需重新部署**，非真动态 |
| D6 | 配额感知（还能建多少） | ❌ | **`/limits` 403 ⇒ 平台上限不可观测** |
| D7 | ACL 差异化下发 | ⚠️ 受限 | 只能整表覆盖 ⇒ 并发改同一用户 ACL **会互相覆盖**（无 ETag/版本号） |
| D8 | 池内账号 ACL 互不影响 | ✅ | ACL 按 username 独立存储（实测） |

⇒ **建议维持固定池**（能力已足、不累积、轮换已实测），动态池列为"并发规模超 20 时再评估"（决策项 D-8）。

---

## 6. 后续调查/验证计划（需授权时执行）

> 目标：把"写操作"的真实行为与失败恢复路径补全，形成可复用规程。**所有写操作仅允许对专用临时账号（`gf_probe_*` 前缀）执行**，且每次操作前后都记录读快照。

| # | 待验证项 | 方式 | 需要的权限 | 判据 |
|---|---|---|---|---|
| V1 | **删号后 ACL 是否自动清理** | 建临时账号 + 3 条 ACL → 删号 → 再查该用户 ACL 与用户列表 | 现有 App ID（写权限已具备） | 记录 ACL 是否残留（决定要不要"先删 ACL 再删号"的固定顺序） |
| V2 | **publish-only → subscribe-only** 的整表覆盖 | PUT 只含 subscribe 规则 → 重新连接实测订阅/发布 | 同上 | 订阅成功 + 发布被拒（rc=135）⇒ 证明整表覆盖等价于改权限 |
| V3 | **ACL 变更对已建连会话的即时性** | 同一连接：授予后再改 ACL → 观察是否需重连 | 同上 | 明确"必须踢连接"的定量结论（现有结论来自测试账号） |
| V4 | **账号数/规则数平台上限** | 只能撞错误码（`/limits` 403）⇒ 用临时账号试探 | 同上 | 记录错误码形态（建议在**非生产**环境试） |
| V5 | **改密是否踢连接** | 生产已实测：**不踢**（`p4r_emqx_probe.py:371-374` 明确记录） | — | 已有结论 |
| V6 | **PUT body 形状容错** | 对象 vs 裸数组 | 同上 | 裸数组 400（已知），记录错误体形态 |
| V7 | **ACL 并发写** | 两个请求同时 PUT 同一用户 | 同上 | 记录是否出现"后者覆盖前者"的确定性问题（影响并发安全设计） |

**验证纪律**（沿用 p4r 探针铁律）：① 账号名必须带 `gf_probe_` 前缀，脚本内 `assert name.startswith("gf_probe_")`；② 保护名单硬编码断言（`GuoFeederDevice`/`test001`/`shouji`/`workbuddy`/`dev_288485896ce4`/`gf_web_`）禁止出现在任何写请求 URL；③ `finally` 二次清理（幂等）；④ 结束断言"零残留：探针连接数 = 0 且探针账号数 = 0"。

---

## 7. 可复用接口调用示例（脱敏；供后续 AI 直接使用）

> ⚠️ 凭据来自 Cloudflare Secret（`EMQX_APP_ID`/`EMQX_APP_SECRET`）或本地 `.dev.vars`；**不得写入文档或 Git**。示例中的 `gf_web_00` 为池账号，**只读示例**；写操作请用 `gf_probe_*` 临时账号。

### 7.1 只读：列出池账号（判断池是否就绪）

```bash
BASE=https://<emqx-host>:8443/api/v5
curl -s -u "$EMQX_APP_ID:$EMQX_APP_SECRET" \
  "$BASE/authentication/password_based:built_in_database/users" | \
  python -c "import sys,json; d=json.load(sys.stdin); print([u['user_id'] for u in d['data']])"
```

### 7.2 只读：查某账号的 ACL（★ 顶层 `rules`，不是 `data.rules`）

```bash
curl -s -u "$EMQX_APP_ID:$EMQX_APP_SECRET" \
  "$BASE/authorization/sources/built_in_database/rules/users/gf_web_00"
# → {"username":"gf_web_00","rules":[{"action":"publish","permission":"allow","topic":"guo_feeder/+/down","qos":[0,1,2],"retain":"all"}, ...]}
```

### 7.3 只读：查某账号的在线连接（★ 不用 `?clientid=`）

```bash
curl -s -u "$EMQX_APP_ID:$EMQX_APP_SECRET" "$BASE/clients?username=gf_web_00"
# 空数组 = 无连接；再退一步用 ?conn_state=connected 全量 + 本地过滤
```

### 7.4 写（仅临时账号）：建号 + 授权 + 验真 + 回收

```bash
U=gf_probe_$(date +%s)
# ① 建号（409 表示已存在 ⇒ 改密码对齐）
curl -s -o /dev/null -w "%{http_code}\n" -u "$A" -X POST -H 'content-type: application/json' \
  -d "{\"user_id\":\"$U\",\"password\":\"$(openssl rand -hex 12)\",\"is_superuser\":false}" \
  "$BASE/authentication/password_based:built_in_database/users"
# ② 整表授权（必须是对象；裸数组 400）
curl -s -o /dev/null -w "%{http_code}\n" -u "$A" -X PUT -H 'content-type: application/json' \
  -d "{\"username\":\"$U\",\"rules\":[{\"topic\":\"guo_feeder/+/up\",\"permission\":\"allow\",\"action\":\"subscribe\"}]}" \
  "$BASE/authorization/sources/built_in_database/rules/users/$U"
# ③ 验真：新连接订阅 guo_feeder/<fake>/up ⇒ SUBACK rc≠0x80（授予前应 rc=0x80）
# ④ 回收：先清 ACL 再删用户，再断言零残留
curl -s -o /dev/null -w "%{http_code}\n" -u "$A" -X DELETE "$BASE/authorization/sources/built_in_database/rules/users/$U"
curl -s -o /dev/null -w "%{http_code}\n" -u "$A" -X DELETE "$BASE/authentication/password_based:built_in_database/users/$U"
```

### 7.5 写（仅临时账号）：踢连接

```bash
curl -s -o /dev/null -w "%{http_code}\n" -u "$A" -X DELETE "$BASE/clients/<clientid>"
```

---

## 8. 运维规程（池生命周期）

| 场景 | 正确动作 | 禁止 |
|---|---|---|
| 池为空 / 账号不足 | `POST /api/admin/mqtt/pool/bootstrap`（幂等；`force:true` = **重铺全部密码**，仅在 `MQTT_POOL_KEY` 更换后使用） | 手工在控制台建账号（绕过 AES 密文 ⇒ D1 与 EMQX 密码不一致） |
| 密码轮换 | `POST /api/admin/mqtt/pool/rotate {"username":"gf_web_00"}`（七步：改密→ACL 重放→踢连接→复验→加密落库） | 只改密码不踢连接（老会话仍持旧授权窗口） |
| 改 ACL | **PUT 整表 + 踢连接**（成对） | 只改 ACL 不踢（已建连会话按 (连接,topic) 缓存，不重新鉴权） |
| 设备账号退役 | `credential.js:502` 收紧为 DENY_ALL → 踢连接 → `:644-645` 删账号+ACL | 直接删账号（会话可能仍在线） |
| 排查"假离线" | 用 `?username=` 过滤，不用 `?clientid=` | 依据 `?clientid=` 的 0 条判离线 |
| 平台配额 | **无 API 可查**（`/limits` 403）⇒ 靠固定池上限（20）自约束 | 假设可以无限建号 |
| 换 `MQTT_POOL_KEY` | 密文全部作废 ⇒ 只能 `bootstrap {force:true}` 重铺 + 踢全部池连接 | 直接改 Secret（= 全池不可用） |

---

## 9. 证据与可复现命令

| 证据 | 位置 / 命令 |
|---|---|
| 代码封装（唯一入口） | `guo-feeder-api/src/emqx-admin.js`（11 个端点，`:19-64` 认证，`:70-161` 统一请求，非 2xx 不抛异常） |
| 池逻辑 | `guo-feeder-api/src/mqtt-pool.js`（`browserAclRules():248`、`bootstrapPool():305`、`rotateMqttPoolAccount` 七步 `:600-700`、`poolClientIds()` 两段式 `:767-790`、`kickClient():748`） |
| 设备账号侧调用 | `credential.js:261/277/318/502/520/530/537/644-645/712`；`command-api.js:567`；`log-ack.js:79`；`index.js:1529`；`presence-emqx.js:183/201` |
| 阶段四 ACL 闭环实验 | `scripts/p4r_emqx_acl_probe.py`（授→订阅→发布→撤→被拒，5 步全 PASS） |
| 阶段四综合探针 | `scripts/p4r_emqx_probe.py`（用户 CRUD/改密不踢/踢连接/新密码可连/旧密码被拒） |
| 生产 e2e | `tests/e2e/mqtt_browser_e2e.mjs`（33 断言：CONNACK/SUBACK/PUBACK 135/轮换七步/零残留） |
| 生产验证脚本 | `scripts/verify_mqtt_pool_prod.mjs`（17 断言：登录面/凭据 fail-closed/D1 密文解密/真实 WSS/池状态） |
| 本次只读实测 | 21 条 GET 探测（仅 GET；写操作零执行）；ACL 单用户/整表响应形状；`?clientid=` 静默失效复现；用户清单 10 个 |
