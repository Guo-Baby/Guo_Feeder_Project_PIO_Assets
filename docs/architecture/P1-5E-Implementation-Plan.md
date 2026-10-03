# P1-5-E Implementation Plan

> **状态**：**实施计划 · 仅规划不编码** —— 待人工审核后才进入编码
> **上游**：`P1-5-Device-Binding-Design.md`（P1-5-A，已审核）+ **Q14–Q20 裁决（2026-10-04）**
> **撰写日期**：2026-10-04
> **本文边界**：不含任何代码。**不改** `migrations/` · **不改** 固件 `src/` · **不改** `admin-auth.js` / `emqx-admin.js` / `wrangler.toml`

---

## 1. Scope

### 1.1 目标

把 `device_binding`（`0001` 已建、**Worker 中零引用**）变成**唯一 owner 的可审计归属链**，并让它与既有的凭据状态机**零耦合**。

### 1.2 裁决基线（Q14–Q20 已定，实施不得偏离）

| # | 裁决 | 实施含义 |
|---|---|---|
| **Q14** | 绑定入口 = **A 管理端直绑**；`/api/admin/**` 是 P1 唯一 owner 变更入口；配对码预留但 **P1-5 不实现**；**禁止**先到先得自助 | 只做 admin 端点；`claim` 命名空间不落地 |
| **Q15** | 管理员**可**强制解绑；**必须**写 `device_event`；`forced=true`；保留完整审计；P1 管理员拥有完整 bind/unbind/transfer | `unbind` payload 带 `forced` |
| **Q16** | 恢复出厂**保留** binding、**不自动解绑**、**不修改** `device_binding`；`device_id` 不变；`gfcred` 丢失后重走 credential provisioning | **绑定侧零动作**（等价于"什么都不做"） |
| **Q17** | 用户删除 ⇒ **解绑但保留设备**，`reason=owner_deleted`，保留历史；P1 无用户系统 ⇒ **仅记录未来策略** | 仅登记 `reason` 取值；**不实现**用户删除入口 |
| **Q18** | 一用户多设备：**允许 1:N、不设上限**、**保留查询接口** | 落地 `GET /api/admin/user/:id/devices` |
| **Q19** | 一设备多用户：**禁止**；保持单 owner；**不引入** roles / shared binding | 不加任何共享结构 |
| **Q20** | `REVOKED` **必须关闭** active binding；`revokeDevice()` **同一 D1 事务**内：① `unbound_at=now` ② 写 `unbind` 事件 ③ `device.state='REVOKED'`；**不允许** `REVOKED + active binding` | 唯一需改既有函数（见 §3.3） |

### 1.3 只读代码结构基线（已核对，`Cloudflare_Assets/guo-feeder-api/src/`）

| 项 | 实情（**只读确认**） |
|---|---|
| 文件规模 | `index.js` 837 · `credential.js` 631 · `device-registry.js` 576 · `emqx-admin.js` 362 · `admin-auth.js` 95 · `credential-gen.js` 59 |
| `device-registry.js` 导出 | `DEVICE_STATES` · `isValidDeviceId` · `getDevice` · `countDevicesByState` · `deviceIdFromTopic` · `upsertDeviceOnIngest` · `appendEvent` · `listEvents` · `claimAuthorized` · `getCredentialByGeneration` · `getRawCredential` · `listLiveCredentials` · `deriveProvisionState` · `getLatestCredential` · `getActiveCredential` · `listCredentials` · `nextGeneration` · `insertProvisioningRow` · `recordProvisionError` · `activateCredential` · `revokeCredentialRow` · `setDeviceState`；**顶部无 import（自包含）** |
| `credential.js` 导出 | `usernameFor` · `buildAclRules` · `grantClaim` · `provisionCredential` · `classifyCredentialFrame` · `handleCredentialConfirm` · `revokeDevice` · `reclaimCredential` · `reconcileTimeouts` · `reconcileView`；依赖 `credential-gen.js` / `emqx-admin.js` / `device-registry.js` |
| `index.js` 路由 | ① `path.startsWith("/api/admin/")` ⇒ `handleAdmin(request, env, path)` ② `POST /api/ingest` ⇒ `requireWebhookKey` ③ 非 POST ⇒ 探活 ④ 其余 POST ⇒ 既有 webhook / `cloud_api` 通路 |
| `handleAdmin` 结构 | 先 `authorizeAdmin(request, env)`（返回 `Response` 或 `null`）⇒ `/api/admin/reconcile` 特判 ⇒ 前缀 `/api/admin/device/` ⇒ `rest = path.slice(PREFIX).split("/")`，`action = rest.slice(1).join("/")` ⇒ `GET + action===""` 走 `handleAdminGetDevice` ⇒ 非 POST ⇒ 405 ⇒ `action === "claim" \| "credential" \| "credential/rotate" \| "revoke"` ⇒ 兜底 `404 {error:"not_found", action}` |
| 服务层返回约定 | 服务函数返回 `{http, ...}`，`index.js` 统一 `jsonResponse(out, out.http \|\| 200)`（`grantClaim` 额外做了一次 `http!==200` 判定） |
| 响应工具 | `jsonResponse(obj, status)`（`JSON.stringify` + `content-type: application/json`）；`requireWebhookKey` 对缺失/错误**返回同一个 401 body** |
| 鉴权工具 | `admin-auth.js`：`timingSafeEqual` · `unauthorizedResponse` · `extractBearerToken` · `authorizeAdmin` |
| **D1 事务用法** | **全仓仅 1 处** `env.guofeeder_DB.batch([...])` —— `device-registry.js:483` 的 `activateCredential()`（D-3 切主），代码注释明写「**D1 `batch` = 单事务**」；**无任何显式 `BEGIN/COMMIT/ROLLBACK`** |
| ⚠️ 既有 batch 的边界 | `activateCredential()` 的 batch **只含 2 条 credential UPDATE**；随后的 `UPDATE device … state='REGISTERED'` 与 `appendEvent(credential_activate)` **在 batch 之外**（两次独立 `await`）⇒ **既有代码并未把"状态 + 事件"放进同一事务**。P1-5 要求更严（§5） |
| `device_event` 写入 | `appendEvent(env, {deviceId, type, actor, payload})` ⇒ **立即执行**的单条 `INSERT`（`payload` 经 `JSON.stringify`）；**代码层不校验 `type`**（靠 DDL 的 CHECK 兜底） |
| `device_id` 校验 | `isValidDeviceId()` = `/^[0-9a-f]{12}$/` |
| `user_id` 现状 | **Worker `src/` 中零出现** ⇒ P1-5 首次引入（DDL 已备 `device_binding.user_id TEXT NOT NULL`，**无格式 CHECK**） |
| `device_binding` 现状 | **Worker `src/` 中零引用**；DDL 已有 `ux_binding_active`（`device_id WHERE unbound_at IS NULL` 部分唯一索引）· `ix_binding_user` · `ix_binding_device` · `CHECK(unbound_at IS NULL OR unbound_at >= bound_at)` |

### 1.4 相对 P1-5-A 的**设计增量**（须在 §3.4 同步回设计文档）

| # | 增量 | 理由 |
|---|---|---|
| Δ-1 | **取消 `device_not_provisioned` 判定**，改用 `device.state ∈ {REGISTERED, BOUND, UNBOUND}` 作为"已签发"的**代理判据** | 冻结边界 **#5「binding 不允许读写 `device_credential`」** ⇒ 绑定侧**不得**调 `getActiveCredential()`。而 `REGISTERED` 只由 `activateCredential()` 在激活成功后写入 ⇒ state 是等价且更省的判据 |
| Δ-2 | `transfer` 增加**可选 CAS 断言** `from_user_id` | 让调用方可声明"我以为的当前 owner"⇒ 与期望不符时**零写入**返回 `422`（消除"误转让"） |
| Δ-3 | 判定"本次是否真的写了"的手段 = **D1 `batch()` 的逐语句 `meta.changes`** | 既有 `activateCredential()` 已在用 `results[last].meta.changes` ⇒ 同一手法 |
| Δ-4 | 所有"冲突"路径改为 **guarded statement 使批次整体成为 0 效果**（而不是抛错回滚） | 保证"检测到冲突"与"没有写入"同时成立（§5.3） |

---

## 2. File Impact

### 2.1 新增

| 文件 | 内容 |
|---|---|
| `Cloudflare_Assets/guo-feeder-api/src/binding.js` | **P1-5 唯一新模块**：`isValidUserId` · 事务封装 `runBindingTxn` · D1 错误分类 `classifyD1Error` · 五个动作 `bindDevice` / `unbindDevice` / `transferDevice` / `getBinding` / `listDevicesByUser` · guarded statement 构造 |
| `PIO_Assets/Guo_Feeder_Project/test/`（**受跟踪**） | P1-5 用例集（T-1…T-19 中可离线/可脚本化的部分），与既有 `test/` 同纪律 |
| `.pio/p0run/p1_5_*.py`（**不入库**） | 联调脚本（沿 D-3 约定） |

### 2.2 修改

| 文件 | 改动 | 行数量级 |
|---|---|---|
| `guo-feeder-api/src/device-registry.js` | ① 新增 `eventStatement(env, {...})`（**只构造不执行**的 prepared statement）并把 `appendEvent()` 重构为「`eventStatement` + `run()`」② 新增 `isValidUserId()` ③ 新增只读查询 `getActiveBinding()` / `listBindingHistory()` / `listBindingsByUser()` | +80 / −6 |
| `guo-feeder-api/src/credential.js` | **仅 `revokeDevice()` 的 D1 段**：把原「`UPDATE device_credential` + `setDeviceState(REVOKED)` + `appendEvent(retire)`」改为**同一 `batch`**，并在**最前**插入 `UPDATE device_binding … unbound_at`，其后**条件插入** `unbind` 事件（Q20 / INV-4） | 函数内 ±25 |
| `guo-feeder-api/src/index.js` | ① 顶部 import 增加 `binding.js` 的 5 个符号 ② `handleAdmin()` 增加 5 条分支（`bind` / `unbind` / `transfer` / `GET binding`）+ `/api/admin/user/:userId/devices` 路由 ③ `handleAdminGetDevice()` 返回值追加 `binding` / `binding_history` | +60 |
| `Cloudflare_Assets/README.md` | 路由表 · 模块表 · 不变量（补 INV-1…INV-7）· 待办 | +25 |
| `PIO_Assets/.../docs/interfaces/cloud_protocol.md` | 新增「**管理端绑定契约**」章节（P1-5 的控制面协议，与设备侧协议分节） | +60 |
| `PIO_Assets/.../docs/architecture/P1-5-Device-Binding-Design.md` | 同步 Δ-1…Δ-4（§1.4）；F-5 改为 state 判据 | +15 / −5 |
| `PIO_Assets/.../docs/architecture/P1-实现清单.md` · `HANDOFF.md` · `docs/README.md` | 状态推进（P1-5 → 实施中/完成） | +20 |

### 2.3 禁止修改（**硬边界，提交前逐条核对改动数 = 0**）

```
guo-feeder-api/migrations/**            （含 0001；且**不新增任何迁移**）
guo-feeder-api/src/admin-auth.js
guo-feeder-api/src/emqx-admin.js
guo-feeder-api/src/credential-gen.js
guo-feeder-api/wrangler.toml
guo-feeder-api/src/credential.js        除 revokeDevice() 外的**任何**函数
                                        （尤其 provisionCredential / handleCredentialConfirm /
                                          classifyCredentialFrame / reclaimCredential / reconcile*）
PIO_Assets/.../src/**                   全部（device_identity / topic_renderer / system_command /
                                          credential_manager / cred_store / command_manager /
                                          cloud_manager / main.cpp / app / automation / storage / log）
PIO_Assets/.../data/**  ·  platformio.ini
EMQX_Assets/**                          全部
```

> **为什么 `device-registry.js` 可以改而 `admin-auth.js` 不能改**：前者是**数据访问层**（P1-5 需要新增绑定读写与可组合的事件语句构造），后者是**鉴权层**（P1-5 完全复用 `authorizeAdmin`，无新增需求）。

---

## 3. Implementation Steps

### 3.1 P1-5-1 Binding Core（`binding.js`）

**模块职责（与 `credential.js` 对称：编排 + 校验 + 事务组合）**

| 类别 | 内容 |
|---|---|
| **校验** | `isValidUserId(id)`（`^[A-Za-z0-9_.:@-]{1,64}$`）· `device_id` 复用 `isValidDeviceId` · 前置态判定（`state ∈ {REGISTERED, BOUND, UNBOUND}`） |
| **事务封装** | `runBindingTxn(env, statements)` ⇒ 包 `env.guofeeder_DB.batch(stmts)`，`try/catch` 内调 `classifyD1Error()`；返回 `{ok, results, conflict}` |
| **D1 错误分类** | `classifyD1Error(e)` ⇒ 命中 `UNIQUE`/`constraint` 关键词 ⇒ `{kind:"unique_violation"}`；否则 `{kind:"db_error"}`（**实现时须先打印一次真实错误文本以确定匹配串**——登记为待实测项） |
| **guarded statement 构造** | `stmtInsertBinding` · `stmtCloseBinding` · `stmtSyncDeviceBound` · `stmtEvent`（均返回 **prepared statement**，不执行） |
| **helper API** | `bindDevice` · `unbindDevice` · `transferDevice` · `getBinding` · `listDevicesByUser`（**均返回 `{http, ...}`**，与 `credential.js` 同约定） |

**数据库操作边界（本计划的核心纪律）**

| 表 | binding.js 允许的操作 |
|---|---|
| `device_binding` | **SELECT / INSERT / UPDATE（仅 `unbound_at`）** —— 本模块**独占写权** |
| `device` | 仅 `UPDATE state`（**仅** `→BOUND` / `→UNBOUND`，且**必带守卫**） |
| `device_event` | 仅 `INSERT`（`bind` / `unbind` / `transfer`） |
| `device_credential` | **🚫 禁止任何读写**（冻结 #5） |
| 其他表 | 禁止 |

**复用（只读 import，不复制逻辑）**：`getDevice` · `isValidDeviceId` · `eventStatement` · `getActiveBinding` · `listBindingHistory` · `listBindingsByUser`（均来自 `device-registry.js`）。

### 3.2 P1-5-2 Admin API（`index.js`）

| # | 方法 + 路径 | 服务调用 | 备注 |
|---|---|---|---|
| 1 | `POST /api/admin/device/:id/bind` | `bindDevice(env, {deviceId, userId: body.user_id, reason: body.reason})` | `user_id` 必填 |
| 2 | `POST /api/admin/device/:id/unbind` | `unbindDevice(env, {deviceId, reason, forced: true, actor:"cloud"})` | Q15：`forced=true` 写入 payload |
| 3 | `POST /api/admin/device/:id/transfer` | `transferDevice(env, {deviceId, toUserId: body.to_user_id, fromUserId: body.from_user_id ?? null, reason})` | `from_user_id` = **可选 CAS**（Δ-2） |
| 4 | `GET  /api/admin/device/:id/binding` | `getBinding(env, {deviceId, historyLimit: 50})` | 只读 |
| 5 | `GET  /api/admin/user/:userId/devices` | `listDevicesByUser(env, {userId, limit})` | Q18；**新路由前缀** |
| 6 | `GET  /api/admin/device/:id` | `handleAdminGetDevice()` **扩展** | 追加 `binding` / `binding_history`（**只增字段**） |

**分派顺序（必须明确，避免与既有分支冲突）**

1. `/api/admin/reconcile` 特判（既有，位置不变）
2. **`/api/admin/user/` 前缀特判（新增，须在 device 前缀之前）**
3. `/api/admin/device/` 前缀 ⇒ `rest` 解析 ⇒
   - `GET` + `action===""` ⇒ 既有设备查询（扩展返回）
   - **`GET` + `action==="binding"` ⇒ 新增（必须在下面的 405 之前）**
   - 非 `POST` ⇒ 405（既有）
   - `POST` + `action === "bind" | "unbind" | "transfer"` ⇒ 新增
   - 既有 `claim` / `credential` / `credential/rotate` / `revoke` ⇒ **位置与语义不变**

### 3.3 P1-5-3 revoke integration（`credential.js::revokeDevice` 最小修改）

**只改 D1 段（原"步骤 3"）**；EMQX 段（① ACL 收紧 ② 踢会话）**一个字不动**（它天然无法进 D1 事务）。

| 原实现 | 新实现 | 语义 |
|---|---|---|
| 逐个 `await`：`UPDATE device_credential` → `setDeviceState(REVOKED)` → `appendEvent(retire)` | **一个 `batch`**：<br>S1 `UPDATE device_binding SET unbound_at=:now WHERE device_id=? AND unbound_at IS NULL`<br>S2 `UPDATE device_credential SET state='REVOKED', revoked_at=unixepoch(), updated_at=unixepoch() WHERE device_id=? AND state<>'REVOKED'`（**原样照抄**）<br>S3 `UPDATE device SET state='REVOKED', updated_at=unixepoch() WHERE device_id=?`<br>S4 `INSERT device_event('unbind') … SELECT … WHERE EXISTS(unbound_at = :now)`（**条件**）<br>S5 `INSERT device_event('retire')`（**原样**） | ① 保持 `credential` 状态机语义**逐字不变**（冻结 #4）② 满足 Q20 三件套同事务 ③ **未绑定的设备** ⇒ S1 改 0 行 ⇒ S4 不写 ⇒ **事件序列与旧行为完全一致**（回归安全） |

**INV-4 保证**：`REVOKED` 与 active binding 的互斥由 **S1（先关）+ S3（后置终态）在同一事务**保证；事务中途失败 ⇒ 整体回滚 ⇒ 不会出现"已退役但有 owner"。
**`setDeviceState()` 不再在本函数中调用**（避免绕过守卫；该函数保持原样供他处使用）。

**回归风险（详见 §7-5）**：D3-6 的设备若曾绑定，`device_event` 会多一条 `unbind(forced, reason=retire)` ⇒ 若 D3-6 断言枚举了事件全集，**须同步更新断言**。

### 3.4 P1-5-4 Documentation

| 文件 | 内容 |
|---|---|
| `docs/interfaces/cloud_protocol.md` | 新章节「管理端绑定契约」：6 端点 · 请求/响应 · 错误码 · **幂等语义** · `forced` / `reason` 取值表（含 `owner_deleted`）· **明确标注"控制面，非设备协议"** |
| `Cloudflare_Assets/README.md` | 路由表补 5 条 · 模块表补 `binding.js` · 不变量补 INV-1…INV-7 · 待办更新 |
| `docs/architecture/P1-5-Device-Binding-Design.md` | 同步 Δ-1…Δ-4（§1.4） |
| `docs/README.md` · `P1-实现清单.md` · `HANDOFF.md` | 索引与进度 |

### 3.5 P1-5-5 Verification

三段式（沿用 D-3 节奏）：

| 段 | 手段 | 覆盖 |
|---|---|---|
| **A. 离线/本地** | `wrangler dev`（**随机端口**，见技能 §19.3）+ **本地 D1** | T-1…T-16 · T-18 · T-19 |
| **B. D1 直查** | `wrangler d1 execute --local/--remote --json` 回读（行数/唯一性/事件序列） | 全部用例的**落库证据** |
| **C. 部署版 + 真机** | `wrangler deploy` + 真机 `288485896ce4` | T-17 · T-18 · T-19 · **D3-6 回归** |

⚠️ 部署版部署后**必须重新注入/确认 secrets**（`ADMIN_API_TOKEN` 已存在；本阶段**不需要**新 secret）。

---

## 4. API Contract Finalization

### 4.1 通用

| 项 | 冻结 |
|---|---|
| 前缀 / 鉴权 | `/api/admin/**` + `Authorization: Bearer <ADMIN_API_TOKEN>`（`authorizeAdmin`，常量时间比较） |
| 方法 | 变更一律 `POST`；查询 `GET` |
| 响应 | `jsonResponse(obj, status)`；错误体 `{"error":"<snake_case>", …}` |
| 服务返回 | `{http, ...}` ⇒ `jsonResponse(out, out.http \|\| 200)` |
| 幂等 | **语义幂等**；**不引入** `Idempotency-Key` |
| `user_id` | `^[A-Za-z0-9_.:@-]{1,64}$`，**区分大小写**，非空 |
| `reason` | 自由字符串（≤64，建议白名单：`admin_bind` / `admin_unbind` / `admin_transfer` / `owner_deleted` / `retire` / `service`）；**未知值不报错**，原样入 `payload` |

### 4.2 端点契约

**① `POST /api/admin/device/:id/bind`** — body `{user_id, reason?}`

| 情形 | HTTP | body |
|---|---|---|
| 成功 | 200 | `{device_id, action:"bind", ok:true, owner, state:"BOUND", binding_id}` |
| **同一 owner 重复** | 200 | `{…, ok:true, idempotent:true, note:"already_owned_by_same_user"}` |
| 已有**其他** owner | 422 | `{error:"already_bound", owner:"<current>", hint:"use /transfer"}` |
| 未注册（`FACTORY`/`CLAIM_PENDING`） | 422 | `{error:"device_not_registered", state}` |
| 已退役 | 422 | `{error:"device_revoked"}` |
| `user_id` 非法 | 400 | `{error:"invalid_user_id", hint}` |
| 设备不存在 | 404 | `{error:"device_not_found", device_id}` |
| 竞态（唯一索引） | 422 | `{error:"binding_conflict", hint:"retry"}` |

**② `POST /api/admin/device/:id/unbind`** — body `{reason?}`（**`forced` 由服务端固定为 `true`**，Q15）

| 情形 | HTTP | body |
|---|---|---|
| 成功 | 200 | `{device_id, action:"unbind", ok:true, owner:"<prev>", state:"UNBOUND"}` |
| 无 active binding | 200 | `{…, ok:true, idempotent:true, state:"<unchanged>"}` |
| 已退役 | 422 | `{error:"device_revoked"}` |

**③ `POST /api/admin/device/:id/transfer`** — body `{to_user_id, from_user_id?, reason?}`

| 情形 | HTTP | body |
|---|---|---|
| 成功 | 200 | `{device_id, action:"transfer", ok:true, from, to, state:"BOUND"}` |
| 目标 == 现 owner | 200 | `{…, ok:true, idempotent:true}` |
| 无 active binding | 422 | `{error:"not_bound", hint:"use /bind"}` |
| **CAS 不符**（`from_user_id` ≠ 实际 owner） | 422 | `{error:"owner_mismatch", owner:"<actual>", expected:"<given>"}` |
| 竞态 | 422 | `{error:"binding_conflict"}` |
| `to_user_id` 非法 | 400 | `{error:"invalid_user_id"}` |

**④ `GET /api/admin/device/:id/binding`** ⇒ `{device_id, owner|null, state, binding|null, history:[{id,user_id,bound_at,unbound_at,bound_by,reason}]}`

**⑤ `GET /api/admin/user/:userId/devices?limit=50`** ⇒ `{user_id, count, devices:[{device_id, state, model, fw_version, last_seen, bound_at}]}`（`limit` 默认 50、上限 200）

**⑥ `GET /api/admin/device/:id`**（**扩展**，向后兼容）⇒ 追加 `binding`（同 ④ 的 `binding` 字段）与 `binding_history`

### 4.3 与既有端点的**命名隔离**（冻结）

已占用：`claim` · `credential` · `credential/rotate` · `revoke` · `reconcile`。
P1-5 只新增：`bind` · `unbind` · `transfer` · `binding` · `/api/admin/user/**`。**不复用 `credential` 命名空间**（Q10 语义保护）。

---

## 5. Transaction Design

### 5.1 机制与前提

- 机制 = **`env.guofeeder_DB.batch([...])`**（既有唯一先例：`activateCredential()`）。
- **前提（必须被 T-13 证实后才可依赖）**：D1 `batch()` 在**单一隐式事务**中按序执行，任一语句失败 ⇒ 整体回滚。
  依据 = ① D1 官方语义 ② `activateCredential()` 的注释与 D-3 实测（先降旧再升新依赖原子性）。
  ⇒ **T-13 是本设计的前置验证项**，不是可选项。
- **统一 `:now`**：每个请求**只计算一次** `now = Math.floor(Date.now()/1000)`，绑定到该批次所有语句（保证 `unbound_at >= bound_at` 与守卫判据一致）。

### 5.2 四类事务的**精确语句表**

**T-A `bind`**（3 条，一条批次）

```sql
-- S1 插入（guard：此刻无 active binding ⇒ 冲突则 0 行，而不是靠抛异常）
INSERT INTO device_binding (device_id, user_id, bound_at, bound_by, reason)
SELECT :id, :u, :now, :actor, :reason
 WHERE NOT EXISTS (SELECT 1 FROM device_binding WHERE device_id = :id AND unbound_at IS NULL);

-- S2 摘要同步（guard：本次插入确实成立）
UPDATE device SET state = 'BOUND', updated_at = :now
 WHERE device_id = :id
   AND state IN ('REGISTERED','UNBOUND')                       -- ★ 永不覆盖 REVOKED，也不覆盖 CLAIM_PENDING
   AND EXISTS (SELECT 1 FROM device_binding
                WHERE device_id = :id AND user_id = :u AND unbound_at IS NULL AND bound_at = :now);

-- S3 审计（guard：同上）
INSERT INTO device_event (device_id, type, actor, payload)
SELECT :id, 'bind', :actor, :json
 WHERE EXISTS (SELECT 1 FROM device_binding
                WHERE device_id = :id AND user_id = :u AND unbound_at IS NULL AND bound_at = :now);
```

**T-B `unbind`**（3 条）

```sql
-- S1 关旧
UPDATE device_binding SET unbound_at = :now
 WHERE device_id = :id AND unbound_at IS NULL;

-- S2 摘要（guard：此刻确实已无 active binding ⇒ 与"是谁关的"无关，是**不变量判据**）
UPDATE device SET state = 'UNBOUND', updated_at = :now
 WHERE device_id = :id AND state = 'BOUND'
   AND NOT EXISTS (SELECT 1 FROM device_binding WHERE device_id = :id AND unbound_at IS NULL);

-- S3 审计（guard：本次关的行）
INSERT INTO device_event (device_id, type, actor, payload)
SELECT :id, 'unbind', :actor, :json
 WHERE EXISTS (SELECT 1 FROM device_binding WHERE device_id = :id AND unbound_at = :now);
```

**T-C `transfer`**（4 条，**顺序严格**）

```sql
-- S1 关旧（★ 乐观守卫：必须是"调用方以为的那位 owner"）
UPDATE device_binding SET unbound_at = :now
 WHERE device_id = :id AND unbound_at IS NULL AND user_id = :expected_from;

-- S2 开新（guard：此刻必须已无 active binding；否则 0 行）
INSERT INTO device_binding (device_id, user_id, bound_at, bound_by, reason)
SELECT :id, :to, :now, :actor, :reason
 WHERE NOT EXISTS (SELECT 1 FROM device_binding WHERE device_id = :id AND unbound_at IS NULL);

-- S3 摘要同步（guard：新行确为本次所插）
UPDATE device SET state = 'BOUND', updated_at = :now
 WHERE device_id = :id AND state <> 'REVOKED'
   AND EXISTS (SELECT 1 FROM device_binding
                WHERE device_id = :id AND user_id = :to AND unbound_at IS NULL AND bound_at = :now);

-- S4 审计（恰好 1 条 transfer；guard：同上）
INSERT INTO device_event (device_id, type, actor, payload)
SELECT :id, 'transfer', :actor, :json
 WHERE EXISTS (SELECT 1 FROM device_binding
                WHERE device_id = :id AND user_id = :to AND unbound_at IS NULL AND bound_at = :now);
```

**T-D `revokeDevice` 的 D1 段**（5 条，见 §3.3）

> **顺序铁律**：`transfer` 必须 **先关旧（S1）→ 再开新（S2）→ 再同步 state（S3）→ 最后写事件（S4）**。
> 若颠倒 S1/S2，`ux_binding_active` 会因为"两条 `unbound_at IS NULL`"而立刻拒绝（与 D-3「切主先降旧再升新」同源）。

### 5.3 冲突检测（**消除 check-then-act**）

| 动作 | 竞态来源 | 处理 |
|---|---|---|
| `bind` | 两次并发 bind | S1 的 `NOT EXISTS` ⇒ 第二个**插 0 行**；批次整体 0 效果 ⇒ **读 S1 的 `meta.changes`，为 0 ⇒ 422 `already_bound` / `binding_conflict`**。`ux_binding_active` 作为**兜底**（真并发下 SQLite 写锁 + 唯一索引）；抛错则 `classifyD1Error` ⇒ 422 |
| `unbind` | 并发 unbind | S1 是**单条原子 UPDATE** ⇒ 第二个改 0 行 ⇒ S2/S3 守卫失效（0 效果）⇒ **`changes===0` ⇒ 200 idempotent** |
| `transfer` | 并发 transfer / 中途被 unbind | S1 带 `user_id=:expected_from` ⇒ 只有"当前 owner 仍是调用方以为的那位"才关得动；S2 的 `NOT EXISTS` ⇒ 若仍有 active binding 则插 0 行 ⇒ **S1..S4 全 0 效果** ⇒ 读 `changes` ⇒ 422 `binding_conflict` |

**判定实现**：`batch()` 返回**逐语句结果数组** ⇒ 取 S1 的 `results[0].meta.changes`（`activateCredential()` 已用同一字段，`?? res.changes` 兼容写法照抄）。
**不采用**"先 SELECT 再决定"的路径来决定写不写 —— 预读**仅**用于① 幂等 fast path 的提示 ② CAS 报错文案。

**fast path（幂等 200）与 correctness 的分工**

| 路径 | 依据 | 允许的残差 |
|---|---|---|
| 幂等 fast path（同 owner 重复 bind / 目标即现 owner 的 transfer / 无 owner 的 unbind） | **预读**（`getActiveBinding`） | 预读可能略陈旧 ⇒ 极端情况下返回 `idempotent` 而实际已被他方变更；**后果有界**（管理员操作、可重读、不授予任何访问权） |
| **正确性**（不得双主 / 不得丢更新 / 不得半状态） | **批次守卫 + 唯一索引** | **零容忍** |

### 5.4 `unbound_at = :now` 守卫的**残差风险**（登记）

S2/S3（unbind）与 S4（revoke）用"`unbound_at = :now`"作为"本次真的关了行"的判据。
理论上若**同一秒内**同一设备先有另一次关闭产生相同 `unbound_at`，会误判为"本次所为"。
**评估**：需同秒 + 同设备 + 前一次刚关闭 ⇒ 后果仅为**多写一条 `unbind` 事件**（状态与绑定行均正确）。
**处置**：v1 接受（并加"预读无 owner ⇒ 直接返回 idempotent"的 fast path 把该窗口压到几乎为零）；
**备选（需实测）**：在批次内用 `SELECT changes()` 取上一条语句的影响行数作精确守卫，若 T-13/T-14 证实可行再升级。

---

## 6. Test Mapping

| # | 用例 | 段 | 手段 | 判据 / D1 证据 |
|---|---|---|---|---|
| **T-1** | bind 正常 | A | `POST /bind` | `state:REGISTERED→BOUND`；`SELECT COUNT(*) FROM device_binding WHERE device_id=? AND unbound_at IS NULL` = 1；`device_event` 新增 1 条 `bind` |
| **T-2** | bind 幂等 | A | 重复 `POST /bind` | 200 `idempotent`；**行数不变 + 事件数不变** |
| **T-3** | bind 冲突 | A | 换 owner `POST /bind` | 422 `already_bound`；**D1 前后逐表无变化** |
| **T-4** | unbind 正常 | A | `POST /unbind` | `BOUND→UNBOUND`；`unbound_at` 非空；事件 1 条 |
| **T-5** | unbind 幂等 | A | 重复 | 200 `idempotent`；`device_binding` 行数不变 |
| **T-6** | transfer 正常 | A | `POST /transfer` | 关旧 + 开新；`from/to` 正确；事件 **恰好 1 条** `transfer` |
| **T-7** | transfer 后 owner 唯一 | B | SQL | `…WHERE unbound_at IS NULL` **恰 1 行且 = 新 owner**；`ux_binding_active` 未被违反 |
| **T-8** | transfer 幂等 | A | 目标 = 现 owner | 200 `idempotent`；无新行/无新事件 |
| **T-9** | transfer 无主 | A | 对未绑定设备 | 422 `not_bound` |
| **T-9b** | **CAS 不符**（Δ-2） | A | `from_user_id` 给错 | 422 `owner_mismatch`；**零写入** |
| **T-10** | 准入不足 | A | `CLAIM_PENDING` 设备 bind | 422 `device_not_registered` |
| **T-11** | 退役设备 | A | `REVOKED` 设备三动作 | 均 422 `device_revoked` |
| **T-12** | 用户多设备 | A+B | 1 user 绑 3 台 ⇒ `GET /api/admin/user/:u/devices` | `count=3`；SQL 交叉核对 |
| **T-13** | **批次原子性（前置验证）** | A | 构造必失败批次（临时探针） | **全部回滚**：无 binding 行、`device.state` 不变、无事件 |
| **T-14** | 并发 transfer | A | 同时发 2 个请求 | **恰 1 个 200**，另一个 422；最终 active binding **恰 1 行** |
| **T-15** | **退役配对（Q20/INV-4）** | A | 对 **BOUND** 设备 `POST /revoke` | `state=REVOKED` **且** active binding = **0 行**；事件含 `unbind(forced,reason=retire)` + `retire` 成对 |
| **T-16** | 凭据不受影响（INV-5） | A+B | bind/unbind/transfer 前后 `SELECT * FROM device_credential` 快照比对 | **逐列完全一致** |
| **T-17** | 上行不受影响（F-16） | **C** | 真机 `288485896ce4` 在 `UNBOUND` 态持续上行 | `last_seen` 刷新；EMQX 会话在线；**无拒收** |
| **T-18** | 明文负向（INV-6） | A+C | 响应体 + `device_event.payload` 全量 grep | 无 `password` / 凭据 / `ADMIN_API_TOKEN` |
| **T-19** | 鉴权一致性（F-20） | A+C | 缺/错/未配置 token | 三种 401 body **逐字节相同** |
| **T-20** | **D3-6 回归** | **C** | 重跑 D-3 的 revoke 用例（未绑定设备） | 事件序列与旧行为**一致**（S4 不触发）；ACL 收紧 + 踢会话仍成对 |

**分类汇总**

| 类别 | 用例 |
|---|---|
| **自动化（HTTP 端点级，本地）** | T-1…T-6、T-8…T-12、T-14、T-15、T-16、T-18、T-19 |
| **D1 查询验证（`--local` / `--remote`）** | T-1、T-3（无变化）、T-4、T-7、T-12、T-13、T-15、T-16 |
| **需真实部署版 / 真机** | **T-13**（batch 语义判定，须在真 D1 上确认一次）、**T-17**、T-18（响应）、T-19（401 body）、**T-20（D3-6 回归）** |

---

## 7. Risk Review

### 7-1 `device.state` 是否可能被 bind/unbind 错误覆盖？

| 风险点 | 分析 | 处置 |
|---|---|---|
| 上行冲刷 | `upsertDeviceOnIngest()` 有 `CASE WHEN device.state='FACTORY' THEN 'CLAIM_PENDING' ELSE device.state END` | ✅ 已安全（只读确认） |
| 轮换/激活冲刷 | `activateCredential()` 的 `WHERE state IN ('CLAIM_PENDING','FACTORY')` | ✅ 已安全（只读确认） |
| **绑定侧自伤** | 若 `UPDATE device` 不带守卫，可能与并发凭据流程互相覆盖 | ✅ **S2/S3 均带 `state IN (...)` 或 `NOT EXISTS` / `EXISTS` 守卫**；**永不写 `REVOKED`**、**永不写 `CLAIM_PENDING`** |
| 通用写口 | `setDeviceState()` 无守卫，任何调用方都能写任意态 | ⚠️ **纪律**：binding.js **禁止**调用 `setDeviceState()`；`revokeDevice()` 改用内联 `UPDATE`（§3.3） |
| 双重写入 | 绑定与凭据可能"同时"改 state（不同事务） | 两者都在**自己的批次内**完成 ⇒ 后提交者按**守卫**决定是否生效；**不会出现"半态"**。极端交错下可能最后一次生效，但 state 始终 ∈ 合法集合 |

### 7-2 `transfer` 是否严格同事务、顺序正确？

✅ **是**。T-C 的四条语句在**同一个 `batch()`** 内，顺序固定为
**关旧（S1）→ 开新（S2）→ `device` 摘要（S3）→ 事件（S4）**。
若把 S1/S2 颠倒，`ux_binding_active` 会在"两条 active"的瞬间拒绝 ⇒ 正是 D-3 已验证过的失效模式。
**前置依赖**：`batch()` 的原子性必须由 **T-13** 证实（§5.1）。

### 7-3 是否存在 check-then-act 竞态？

| 位置 | 结论 |
|---|---|
| `bind` | 预读**仅**决定幂等 fast path；**写入正确性由 S1 的 `NOT EXISTS` + `ux_binding_active` 保证** ⇒ 无 TOCTOU |
| `unbind` | S1 单条原子 `UPDATE … WHERE unbound_at IS NULL` ⇒ **天然无竞态** |
| `transfer` | S1 带 `user_id=:expected_from`**乐观守卫** + S2 `NOT EXISTS` ⇒ 冲突时**整批 0 效果** ⇒ 无 TOCTOU、无"丢更新" |
| 幂等 fast path 的预读 | 可能陈旧，但**只影响提示**，不授予任何能力；残差已登记（§5.3） |

⇒ **结论：无 check-then-act 依赖**（预读不承担正确性职责）。

### 7-4 是否存在 credential / binding 状态污染？

| 方向 | 结论 |
|---|---|
| binding → credential | **🚫 零读写**（冻结 #5；Δ-1 用 `device.state` 替代 credential 查询） |
| credential → binding | **仅** `revokeDevice()` 的 INV-4 配对关闭（冻结 #6 明确允许）——**无其他触碰** |
| 共享可变状态 | 无（无模块级缓存；`revokeDevice` 与 binding 函数不共享变量） |
| `provisionCredential` / `handleCredentialConfirm` | **完全不动**；`activateCredential()` 的 `WHERE state IN ('CLAIM_PENDING','FACTORY')` 不改 |
| 事件命名空间 | `bind`/`unbind`/`transfer` ≠ `register`/`credential_*`/`acl_update`/`session_terminate` ⇒ 无混淆 |

### 7-5 `revokeDevice()` 修改是否影响 D3-6？

| 维度 | 影响 | 处置 |
|---|---|---|
| credential 语义 | **零改动**（S2 逐字照抄原 SQL，仅位置移入批次） | ✅ |
| 未绑定设备的**事件序列** | S1 改 0 行 ⇒ S4 条件不成立 ⇒ **与旧行为逐条一致** | ✅ 向后兼容 |
| **已绑定设备**的事件序列 | 新增 1 条 `unbind(forced, reason=retire)` | ⚠️ 若 D3-6 断言**枚举全集**须更新；已被 **T-15 / T-20** 覆盖 |
| 返回值 | 保持 `{http:200, revoked:true, acl_tightened:true, kicked, detail}`，可追加 `binding_closed: bool`（**只增字段**） | ✅ 向后兼容 |
| `setDeviceState()` 不再被调用 | 该函数仍导出、其他调用方不变 | ✅ |
| 失败语义 | 原为 3 次独立 `await`（中途失败会留半状态）；现为单批次 ⇒ **更安全**（原实现是一次隐性改进的机会） | ✅ |
| EMQX 段 | **一字不动**（ACL 收紧 + 踢会话仍在批次外，符合"外部调用不能进 D1 事务"） | ✅ |

### 7-6 是否违反"同设备最多一个 active binding"？

| 路径 | 保证 |
|---|---|
| DB 层 | `ux_binding_active`（`UNIQUE(device_id) WHERE unbound_at IS NULL`）—— **最终强制** |
| `bind` | S1 `NOT EXISTS` 前置守卫 + 唯一索引兜底 |
| `transfer` | S1 先关（0/1 行）→ S2 才插 ⇒ **不存在"两条 active 同时存在"的可见瞬间**（同一事务内） |
| `unbind` | 只关不插 |
| `revokeDevice` | 只关不插 |
| 历史行 | 全部 `unbound_at` 非空 ⇒ 不参与唯一索引 ⇒ 追加式历史不受限 |

⇒ **不可能违反**。另：`CHECK(unbound_at IS NULL OR unbound_at >= bound_at)` 由统一 `:now` 保证单调。

---

## 8. Ready-to-Code Checklist

**编码前**
- [ ] 人工审核通过本计划（尤其 §5.2 的语句顺序、§3.3 的 revoke 改法、§7-5 的 D3-6 影响）
- [ ] 确认 **Q14–Q20 裁决不再变动**
- [ ] 确认 **Δ-1…Δ-4** 可接受（尤其 Δ-1：绑定侧不读 `device_credential`）

**编码中（每步独立 commit，代码/文档分离）**
- [ ] P1-5-1 `binding.js`（含 `runBindingTxn` / `classifyD1Error` / 5 个服务函数）
- [ ] P1-5-1b `device-registry.js`：`eventStatement()` 抽取 + `appendEvent()` 重构（**行为等价**）+ `isValidUserId` + 3 个只读查询
- [ ] P1-5-2 `index.js`：路由分支 + `handleAdminGetDevice` 扩展
- [ ] P1-5-3 `credential.js`：`revokeDevice()` D1 段改批次 + 关闭 binding
- [ ] P1-5-4 文档（`cloud_protocol.md` / README / 设计文档 Δ 同步 / 索引与进度）
- [ ] P1-5-5 验证（T-1…T-20 三段式）

**提交前（**每条都必须执行**）**
- [ ] `git diff --cached --name-only` **全量核对**（禁 `git add -A` / `.`）
- [ ] §2.3 禁止清单**逐条核对改动数 = 0**（尤其 `migrations/**`、固件 `src/**`）
- [ ] 确认**未新增迁移**（`migrations/` 目录文件数不变）
- [ ] 负向 grep：无明文凭据 / token 落入响应体与事件 payload（T-18）
- [ ] 401 一致性复核（T-19）
- [ ] **D3-6 回归通过**（T-20）
- [ ] `node --check` 全部改动文件 + import/export 一致性脚本核对

**验证证据（交付时必须附）**
- [ ] 端点级 PASS 计数（本地段）
- [ ] D1 回读证据（`binding` 行数 / 唯一 active / 事件序列 / `device_credential` 逐列不变）
- [ ] 部署版 + 真机段证据（T-17 / T-18 / T-19 / T-20）
- [ ] 每个子阶段的 commit hash

**完成后**：**停止，等待人工审核**；不进入 P1-6。

---

## 附：实施顺序与 commit 规划（建议）

| 序 | commit subject（建议） | 内容 | 前置 |
|---|---|---|---|
| 1 | `feat(p1-5): binding core 模块与数据访问 helper` | `binding.js` + `device-registry.js` 增量 | 本计划审核通过 |
| 2 | `feat(p1-5): admin 绑定端点（bind/unbind/transfer/查询）` | `index.js` | 1 |
| 3 | `fix(p1-5): revokeDevice 同事务关闭 active binding（Q20/INV-4）` | `credential.js` | 1 |
| 4 | `docs(p1-5): 绑定契约与 P1-5 设计增量同步` | 4 份文档 | 1–3 |
| 5 | `test(p1-5): T-1–T-20 验证与 D3-6 回归` | `test/` + `.pio/p0run/` | 1–4 |

> **每步完成后回报**：改动清单 + 验证证据 + commit hash，再进入下一步（沿用 Phase D 节奏）。
