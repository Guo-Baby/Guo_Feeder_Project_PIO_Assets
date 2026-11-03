# 浏览器 MQTT 动态账号池实施计划 **v2**

- 状态：**待人工审批**（本轮只做计划修订，未改任何代码、库结构、EMQX 配置，未部署）
- 取代：`MQTT-Pool-Device-Lease-Plan-2026-11-03.md`（v1，本文档发布后 v1 作废，仅留作对比）
- 适用仓库：`Cloudflare_Assets`（Worker `guo-feeder-api` + Pages `guo-feeder-pagesdev`）；`Android_Assets` 只做真机验证，不改代码
- 证据基线：生产 D1 `YOUR_D1_DATABASE_NAME`、Worker `YOUR_RESOURCE_ID`、Pages `af077daf`、代码 commit `89b3c38`
- 生产库迁移基线：`d1_migrations` **MAX(id)=13 / count=13 / 最后一条 = `0012_xiaomi_auth_pending.sql`** ⇒ 新迁移编号 **`0013`**（本轮只读核对，不是沿用假设）

---

## 0. 修订总览（v1 → v2）

| # | v1 的做法 | 问题 | v2 的做法 |
|---|---|---|---|
| 1 | 用 `lease_sid LIKE '<sid>\|%'` 收敛同会话的多个租约 | `sid` 是**登录会话**，不是设备；同会话可能被多台设备使用 ⇒ 误释放、误踢 | **只收敛 `lease_device = did` 的行**；`lease_device IS NULL` 的历史行一律**冻结**，等自然过期（≤30 min），另设人工确认端点 |
| 2 | 释放/收敛时只改 D1 字段 | EMQX 不认 D1 的 epoch，旧客户端仍能连 | 所有"退出 leased"的路径统一进 **`quarantined`**，必须走 **改密 → 踢连接 → 验证断开** 才能回 `available`；建立全局不变量 I-1 |
| 3 | `kick_pending=1` 后仍完成交接 | 等于把可能仍被旧客户端占用的账号发给新设备 | **任何一步失败/未知 ⇒ 账号停在隔离态，绝不交接**；`kick_pending` 只表示"待继续恢复" |
| 4 | 心跳只更新 `last_seen_at`；靠 240 s 节流 | 活跃设备 30 min 后租约到期被回收，反而被踢 | 心跳**同时**续 `lease_expires_at`，节流窗口 45 s ≪ 租约 1/4，并给"临近到期强制续"兜底；给出可执行 SQL |
| 5 | 声称 5 min 恢复 | cron 实为**每小时 :05**，无证据 | 双通道：**请求触发**（有流量即秒级推进）+ **cron 改 `*/5`**（配置变更须单独审批）；不承诺做不到的时间，明确写出"不改 cron 则上界 60 min" |
| 6 | 旧版 release 缺 epoch 时按 `sid + username` 放行 | 旧页面延迟 release 会释放已分配给新设备的账号 | `lease_device IS NOT NULL` 的租约**必须** `did + epoch` 同时匹配，否则拒绝；仅 `lease_device IS NULL` 的 legacy 行允许旧逻辑 |
| 7 | 沿用"页面关闭即释放"的思路 | 页面级事件释放设备级租约，且 `pagehide` 从未挂载 | **明确区分页面级断开 / 设备级释放**；`pagehide` 明确**不得**挂载；切 HTTP 只断连不释放 |

---

## 1. 事实基线（全部有代码或生产证据，非推断）

### 1.1 代码事实

| 项 | 证据 |
|---|---|
| 池容量 / 命名 | `DEFAULT_POOL_SIZE=5`（钳 1..20）、`gf_web_00…NN`、`POOL_USERNAME_RE=/^gf_web_\d{2}$/`、DB `CHECK username GLOB 'gf_web_[0-9][0-9]'` |
| 租期 / 轮换 | `MQTT_POOL_LEASE_MIN` 缺省 **30 min**（钳 5..240）；`MQTT_POOL_ROTATE_HOURS` 缺省 24 |
| 租约归属键 | `lease_sid` 单列存 **`<sid>\|<cid>`**（`mqtt-pool.js:173 leaseOwnerKey`） |
| 复用条件 | `WHERE lease_sid = ? AND state='leased' AND lease_expires_at > now`（`mqtt-pool.js:555`） |
| 池满行为 | 无候选 ⇒ `{http:503, reason:"pool_exhausted"}`（`mqtt-pool.js:651`），**无驱逐** |
| 过期自愈 | 候选查询含 `state='leased' AND lease_expires_at <= now`（P0-2 修复，`mqtt-pool.js:641`） |
| 轮换原语 | `rotateAccount()` 七步：CAS→改密→重放 ACL→按 username 踢→验证断开(≤10 s)→落密文→返回（`mqtt-pool.js:978`） |
| 踢连接 | `poolClientIds()`：`?username=` 精确过滤 + 本地复核 + 为空时 `conn_state=connected&limit=500` 兜底（`mqtt-pool.js:1125`） |
| 验证断开 | `verifyDisconnected()` 轮询 `POOL` 直到 0 个连接或 10 s 超时（`mqtt-pool.js:1170`） |
| 恢复扫描 | `recoverStuckRotations()`：`rotating` 卡死阈值 **2×周期=48 h**，`failed` 退避 `max(1h, 周期/4)`（`mqtt-pool.js:1217`） |
| cron | `crons=["0 3 * * *","5 * * * *"]`；`isPoolCron()` **精确匹配** `"5 * * * *"` ⇒ **每小时 :05**，不是每 5 分钟 |
| 前端库 | `assets/vendor/mqtt.min.js` = **mqtt.js v5.16.1** |
| 前端 cid | `tabId()` 存 **sessionStorage**（`gf-mqtt.js:370`）⇒ WebView 重建即换值 |
| 前端释放钩子 | `installPoolReleaseHooks()` **被 import（`gf-app.js:40`）但全项目零调用点** ⇒ `pagehide` 释放是**死代码** |
| 前端自动重连 | `reconnectPeriod: 0`（`gf-mqtt.js:593`）⇒ 无内建重连；重连只由 `ensureConnected()` 触发 |
| 凭据缓存 | `CRED_MAX_AGE_S=300`、`CRED_REFRESH_MARGIN_S=30`（`gf-mqtt.js:59-60`） |
| Android | `domStorageEnabled=true`、`databaseEnabled=true`、**无任何 clearCache/deleteAllData**；`CookieManager.flush()` 在 `onStop` |

### 1.2 生产只读证据（2026-11-03 02:3x–03:1x UTC+8）

**(A) 池 5/5 全 leased，同一 sid、5 个不同 cid**

```
gf_web_00 leased <sid 7079b843…>|tossjr5izmvzl9afc  alloc 02:24:44
gf_web_01 leased <sid 7079b843…>|tjs6l1imkmvzl9zuj  alloc 02:25:17
gf_web_02 leased <sid 7079b843…>|twijdofzcmvzl91ec  alloc 02:24:32
gf_web_03 leased <sid 7079b843…>|teyed6tflmvzkv5qi  alloc 02:13:45
gf_web_04 leased <sid 7079b843…>|t03h5ok5amvzko2zf  alloc 02:08:15
```

**(B) 近 24 h 流水**：`allocate` 22、`release` 13 —— **13 条 release 全是 `{"via":"lease_expired"}`**，即客户端主动归还**一次都没成功过**。`rotate_*` 各 4 次（cron 正常）。

**(C) 迁移**：13 条已应用，最后 `0012` ⇒ 候选新迁移 `0013` 成立。

> 结论（沿用 v1 的根因，但修正第 1 条的处置方式）：**同设备重复占号 = cid 随页面重建而变 + 无驱逐**；而 `sid` 只是"这些请求恰好来自同一登录会话"的**伴随现象**，**不能**当作设备身份的证据。

---

## 2. 身份模型（统一规则，消除 v1 的自相矛盾）

### 2.1 四层标识，职责不可互换

| 层 | 名称 | 载体 | 生命周期 | 唯一用途 | **禁止**用于 |
|---|---|---|---|---|---|
| L1 | `did`（install id） | localStorage `gf.install_id`，144 bit | 到清数据/卸载 | 租约归属、复用键的一半、驱逐对象 | 授权（可伪造） |
| L2 | `sid`（会话） | Cookie `gf_session`（HttpOnly，12 h），Pages `readSessionFull` 校验 | 12 h | **授权**：决定"能不能拿/能不能还" | 设备身份（同会话可多设备） |
| L3 | `cid`（页面实例） | sessionStorage `gf.mqtt.tab` | 一次页面会话 | 诊断、日志、页面级观测 | 复用键、释放作用域 |
| L4 | `clientId` | `gfweb_<12hex>`，每次 `connect` 新生成 | 一个连接 | EMQX 侧精确踢连接 | 任何身份判定 |

### 2.2 复用规则（唯一口径，查询与复用**同一条件**）

```sql
-- 复用查询：did 与 sid 必须同时匹配（这就是"复用条件"，不存在第二个版本）
SELECT username, password_ct, password_nonce, lease_expires_at, lease_epoch
  FROM mqtt_browser_account
 WHERE lease_device = ?did
   AND lease_sid    = ?sid          -- 新写入为**纯 sid**（不再拼 cid）
   AND state        = 'leased'
   AND lease_expires_at > ?now
 LIMIT 1;
```

论证（为什么必须 `did + sid`，而不是只要 `did`）：
- 只要 `did` ⇒ 客户端伪造 `did` 即可拿到别人的密码 ⇒ 不可接受。
- 只要 `sid` ⇒ 同会话内多设备互相串号 ⇒ 就是 v1 的 B1 问题。
- `did + sid` ⇒ 必须**同时**是"同一安装"且"同一有效登录会话"才复用；两者都是攻击者要同时伪造才有价值，而 `sid` 由服务端签发、浏览器读不到（HttpOnly），伪造不了 ⇒ 只剩"同一登录者"这一个真实主体，与"同一人重复打开 App"完全对应。

**同一设备重新登录（sid 变化）⇒ 不复用**，走新分配；旧租约随登出进入隔离回收，最迟 30 min 自然过期。明确写出这个代价：**一次"登出→登录"最多多占 1 个池位，最长 30 min**。

**同一账号多设备（两台手机同一登录）⇒ 两个 did、一个 sid ⇒ 不复用**，各拿一个账号。这是正确行为（它们是两台设备），不做任何合并。

### 2.3 为什么 `did` 不需要（也不能）做鉴权

- `did` 由客户端生成并放进请求头，服务端**无法证明**它属于哪台物理设备。
- 但它能做的事恰好够用：**把"复用"限制在同一登录会话内**（`did + sid`），伪造 `did` 只能抢到"自己的"复用资格，而 `sid` 决定了范围。
- 真正的安全边界仍在：① `sid` 决定能不能取凭据；② `epoch` 决定能不能改/还租约；③ EMQX 密码 + 踢连接决定旧连接能不能活。

---

## 3. 账号状态机（v2 核心）

### 3.1 状态定义

| 状态 | 可被分配 | 语义 |
|---|---|---|
| `available` | ✅ | 密码"从未下发给任何未失效客户端"（见 I-1），可直接分配 |
| `leased` | ❌ | 已租给某 `(did, sid)`，`lease_epoch` 有效 |
| `quarantined` | ❌ | **已退出上一任，但凭据尚未确认作废**。当前 `quarantine_step` 指出下一步 |
| `failed` | ❌ | 隔离流程中有不可恢复错误（如密钥解不开），需人工/自愈端点处理 |

> **取消 v1 的 `rotating` 语义歧义**：v1 用它同时表示"到期轮换"和"驱逐中间态"。v2 统一为 `quarantined` + `quarantine_step`（`password` / `acl` / `kick` / `verify`），轮换 = `available → quarantined(reason=rotate) → available`。**同一套流程、同一套安全判据**，不再有两条路径。

### 3.2 转换表（每一行都是一条 CAS，条件全列）

| 从 | 到 | 触发 | CAS 条件（`WHERE`） | 失败即 |
|---|---|---|---|---|
| `available` | `leased` | 新设备分配 | `username=? AND state='available' AND lease_epoch=?` | 重选候选（最多 6 次） |
| `leased` | `quarantined(password)` | ①主动释放 ②登出 ③过期回收 ④驱逐 | `username=? AND state='leased' AND lease_epoch=? AND lease_device=?`（过期回收可省 `lease_device`，但必须带 `state`+`lease_epoch`） | `changes=0` ⇒ 已被别人处理，**不重试** |
| `quarantined(password)` | `quarantined(acl)` | 改密成功 | `username=? AND state='quarantined' AND quarantine_step='password' AND lease_epoch=?` | 停在原步，下次恢复重试 |
| `quarantined(acl)` | `quarantined(kick)` | ACL 重放成功 | 同上（`step='acl'`） | 同上 |
| `quarantined(kick)` | `quarantined(verify)` | 全部旧 client 已 `DELETE` 204/404 | 同上（`step='kick'`） | 同上 |
| `quarantined(verify)` | `available` **或** `leased`(新主) | EMQX 确认该 username 下 0 个 connected | `username=? AND state='quarantined' AND quarantine_step='verify' AND lease_epoch=?` | **不交付**，继续轮询/下次恢复 |
| 任意 | `failed` | 密钥解不开 / ACL 永久失败 | `username=? AND lease_epoch=?` | 只记录，不入池 |

### 3.3 全局不变量（实施时必须用测试锁住）

**I-1（核心）**：处于 `available` 的行，其 `password_ct` 对应的密码**从未被任何仍然有效的客户端持有**。
> 保证方式：**任何 `leased → 其它` 的转换都必须经过 `quarantined` 且必须完成改密**。于是"密码被下发"与"密码被作废"在同一生命周期内一一对应，`available` 的行天然是"干净密码"。
> 这条不变量是"新设备直接拿 available 账号即可信"的**唯一依据**，也是 v2 拒绝"只改 D1 不改 EMQX"的根本原因。

**I-2**：`state='leased'` ⇒ `lease_device` 非空且**全局唯一**（部分唯一索引保证）。
**I-3**：`state='quarantined'` ⇒ `lease_device IS NULL AND lease_sid IS NULL AND lease_expires_at = 0`（不可被任何复用查询命中）。
**I-4**：任何改变归属的写都带 `lease_epoch`；`changes≠1` ⇒ 本请求视为失效，**不得**继续用旧结论。
**I-5**：`failed` / `quarantined` 永不被分配（宁可 503，也不发可能有效的旧密码）。

### 3.4 状态图

```
                     ┌──────────────── 分配(CAS) ────────────────┐
                     ▼                                          │
   ┌──────────┐   new/resume   ┌────────┐  release/expire/evict │
   │ available│───────────────►│ leased │───────────────────────┤
   └──────────┘                └────────┘                        │
        ▲                                                        ▼
        │                                          ┌───────────────────────┐
        │        verify 通过(0 连接)                │      quarantined      │
        └──────────────────────────────────────────│ step: password→acl    │
        │                                          │       →kick→verify    │
        │                                          └───────────────────────┘
        │                                                     │ 任一步失败/未知
        │                                                     ▼
        │                                              ┌────────────┐
        └────── 仅人工/自愈端点 ───────────────────────│  (停留原步) │
                                                       └────────────┘
        驱逐路径：verify 通过后**直接** CAS 成 leased(新 did)，不经 available（避免被第三方抢走）
```

---

## 4. 生命周期全路径（每条路径都必须使旧凭据失效）

### 4.1 路径清单与处置（对应任务书 §2 的"逐一覆盖"）

| 路径 | 归属变化 | 必须做 | 谁执行 | 失败后果 |
|---|---|---|---|---|
| 新设备分配 | `available → leased(new)` | 无（I-1 保证密码干净） | allocate | 重选 |
| 同设备复用 | 不变 | 续 `lease_expires_at`/`last_seen_at` | allocate / heartbeat | 409 |
| 同设备多租约收敛 | 仅**可确认同 did** 的行 `leased → quarantined(release)` | 改密+踢+验证 | 请求触发（1 个/次） | 停在隔离，不影响其他设备 |
| 主动释放（用户点"归还"） | `leased → quarantined(release)` | 同上 | 标记即时返回，回收异步 | 账号立即不可分配（安全方向） |
| 登出 | 同上（设备级） | 同上 | 同上 | 同上 |
| 切到 HTTP 代理 | **不释放**（页面级动作，见 §9） | 仅断开本页连接 | 前端 | — |
| 租约过期 | `leased → quarantined(expire)` | 同上 | cron（有界）/ 请求触发 | 停在隔离 |
| 新设备驱逐 | `leased → quarantined(evict) → leased(new)` | 同上 + verify 通过才交付 | allocate | **不交付**，503 |
| 定期轮换 | `available → quarantined(rotate) → available` | 同上 | cron（1 个/次） | 停在隔离 |

**关键点（v2 与 v1 的最大差别）**：v1 把"释放/过期"当成"改一行字段"；v2 把它们全部并入 `quarantined` 流水。**代价是容量**（隔离中的账号不可用），**收益是任何时刻都不会把"可能还有旧连接的账号"交给新设备**。任务书明确要求"不得为提高账号池可用率而牺牲账号隔离安全"，本计划照此取舍。

### 4.2 容量影响的量化（必须写进运维手册）

池 5，最坏情形：5 个账号同时处于 `quarantined`（例如 EMQX 管理 API 全面故障）⇒ **可用 0**，全部请求 `503 pool_unavailable`，`hint` 明确写"账号正在安全回收，EMQX 管理接口不可用"。
这是**有意设计**：宁可全池不可用，也不发一份可能仍有效的旧密码。

### 4.3 历史数据（`lease_device IS NULL`）的过渡策略

| 阶段 | 处置 |
|---|---|
| T0 迁移完成 | 所有存量行 `lease_device IS NULL` ⇒ **冻结**：不收敛、不踢、不改密、不驱逐 |
| T0 → T0+30 min | 存量行靠 `lease_expires_at` **自然过期**；`releaseExpiredLeases` 把它们送入 `quarantined(expire)` 完成改密+踢+验证 |
| 过渡期内的新请求 | 无 `available` 且无可驱逐对象（legacy 行**不可驱逐**）⇒ `503 {reason:"pool_legacy_held", retry_after:≤1800}`，前端提示"账号正在换手，请稍等" |
| 过渡期结束 | 全池进入新模型，驱逐与复用按 §5/§6 工作 |
| 兜底 | 管理端点 `POST /api/admin/mqtt/pool/legacy/release {username}`（逐条、需 `?confirm=1`、写审计），仅供人工处理卡死的 legacy 行；**默认不使用** |

> 这个策略**完全放弃了 v1 的"按 sid 收敛"**：因为 `sid` 只证明"同一登录会话"，不证明"同一台设备"；而按会话释放/踢会伤到同会话的其它真实设备。宁可让用户等 ≤30 min。

---

## 5. 分配与驱逐算法

### 5.1 分配优先级（严格顺序，命中即返回）

```
① did/sid 归一化；任一非法 ⇒ 走 legacy 分支（§11.3），不参与新模型
② 复用：lease_device=did AND lease_sid=sid AND leased AND 未过期     → renew（§7）
③ 收敛：仅 lease_device=did 的多余行（理论上不可能，索引兜底）      → quarantine（≤1 个/请求）
④ 空闲：state='available' AND state_epoch 有效，按 last_allocated_at ASC  → CAS 抢占
⑤ 驱逐：见 5.2
⑥ 回收推进：池里若有 quarantined 且可推进 ⇒ 推进 1 个（预算 ≤6 s），成功后**重新从 ④ 试一次**
⑦ 仍失败 ⇒ 503，reason 精确区分（pool_exhausted / pool_legacy_held / pool_quarantined / emqx_unknown / pool_store_unavailable）
```

> ⑥ 是 v1 没有的一步：**让隔离中的账号在"有人来申请"时被及时救回**，这是"不依赖常驻进程"的关键手段。

### 5.2 驱逐：三重判据（时间、在线、保护期）

```sql
-- 候选：最久未活跃，且不在保护期
SELECT username, lease_device, lease_epoch, last_seen_at, last_allocated_at
  FROM mqtt_browser_account
 WHERE state = 'leased'
   AND lease_device IS NOT NULL          -- ★ legacy 行永不驱逐（B1）
   AND (lease_expires_at <= ?now
        OR (last_seen_at <= ?now - 900 AND last_allocated_at <= ?now - 180))
 ORDER BY last_seen_at ASC
 LIMIT 3;
```

判定顺序（**在线否决权优先于时间**）：

1. 取 EMQX 在线集合：`GET /clients?conn_state=connected&limit=200`（`poolClientIds` 已有同类封装）。
2. **结果不完整 ⇒ 一律不驱逐**：`ok!==true`、HTTP 非 2xx、或返回条数**打满 limit** ⇒ 视为"无法确定" ⇒ 整个驱逐流程放弃，返回 `503 {reason:"emqx_unknown"}`。
   （判据与 `presence-emqx.js:59-67` 完全一致：`QUERY_LIMIT=50` / `FALLBACK_LIMIT=200`，"打满就不能下结论"。）
3. 本地按 `c.username` 复核（不信任服务端过滤）。
4. 候选的 username 在在线集合里 ⇒ **跳过**（它在用，不算闲置），换下一个候选（最多 3 个）。
5. 全部候选都在线 ⇒ `503 pool_exhausted`（诚实：池真的不够用）。

保护期与阈值的取值理由：

| 参数 | 值 | 理由 |
|---|---|---|
| `EVICT_IDLE_S` | 900（15 min） | 心跳 60 s ⇒ 活跃设备 `last_seen_at` 恒在 90 s 内；15 min 是"连断 15 次心跳"才判闲置，容忍弱网抖动 |
| `EVICT_GRACE_S` | 180（3 min） | 新分配的账号 3 min 内不可被抢 ⇒ 消除"刚拿到就被抢"的抖动与乒乓 |
| 候选上限 | 3 | 单次驱逐最多 3 次 EMQX 在线查询，wall time 可控 |
| 分页/none | 打满即 unknown | 沿用 presence 的既有纪律，不猜 |

**"正在连接/正在重连"的设备如何算活跃**：前端心跳**不要求 `online`**，只要页面可见且未 revoked 就发（§10.3 给出状态机）。因此"connecting / reconnecting / 刚断开准备重连"都会刷新 `last_seen_at`，不会被 15 min 判闲置。

**HTTP fallback 的影响**：切到 HTTP 后该页面不再心跳 ⇒ 该设备 15 min 后成为可驱逐候选。这是**有意**的（它确实不占 MQTT 连接了）；用户切回 MQTT 时重新申请即可。

### 5.3 驱逐交接（**verify 通过才交付**，v2 相对 v1 的硬修正）

```
① 选候选 V（5.2 通过）
② CAS 夺取：leased → quarantined(step=password, reason=evict, reserved_device=did, reserved_sid=sid)
     WHERE username=V AND state='leased' AND lease_epoch=V.epoch
   失败 ⇒ 换下一个候选
③ 改密（ensureUserPassword）          失败/超时 ⇒ 停在 quarantined，**返回 503，不交付**
④ 重放 ACL（replaceUserAclRules）      同上
⑤ 踢全部连接（按 username 枚举 + 逐个 DELETE 204/404）  失败 ⇒ 停在 quarantined(kick)，不交付
⑥ 验证断开（≤10 s 轮询，0 个 connected）  失败/未知 ⇒ 停在 quarantined(verify)，不交付
⑦ 落库：state='leased', lease_device=did, lease_sid=sid, lease_epoch=epoch+1,
        lease_expires_at=now+ttl, last_seen_at=now, password_ct=新密文, reserved_*=NULL
⑧ 只在 ⑦ 成功后回包
```

任何一步失败：**新设备拿不到账号**，账号停在 `quarantined`，由 §8 的恢复通道继续。`kick_pending` 在 v2 里**只表示"这一行需要恢复"**（等价于 `quarantine_step ∈ {kick, verify}`），**绝不表示"可以交付"**。

### 5.4 同设备并发申请（结构性防重）

- 部分唯一索引：`CREATE UNIQUE INDEX ux_mba_lease_device ON mqtt_browser_account(lease_device) WHERE lease_device IS NOT NULL AND state='leased'`。
- 两个并发请求同时给同一 `did` 抢不同账号 ⇒ 第二个 `UPDATE` 撞约束 ⇒ 捕获后**回查 `lease_device=did`** ⇒ 拿到赢家的行 ⇒ 直接复用并返回同一 username（**不**再分配、**不**报错）。
- 这条路径必须有测试（§12 T-01/T-09），且负向探针要断言"没有第二行被置为 leased"。

---

## 6. 历史重复租约收敛（v2：只认 did）

### 6.1 允许收敛的唯一条件

```sql
SELECT username, lease_epoch, lease_expires_at, last_allocated_at
  FROM mqtt_browser_account
 WHERE state='leased' AND lease_device = ?did      -- ★ 唯一的收敛依据
 ORDER BY lease_expires_at DESC, last_allocated_at DESC;
```

- 命中 1 行 ⇒ 正常复用。
- 命中 >1 行（只可能来自 bug 或索引上线前的窗口）⇒ 保留第 1 行，其余逐行 CAS 进 `quarantined(reason=duplicate)`，**每请求最多推进 1 个**，其余留待下次。
- 命中 0 行 ⇒ 不收敛任何东西。

### 6.2 明确禁止（写成代码级断言 + 测试）

- ❌ 不得用 `lease_sid LIKE` 反查"同设备"。
- ❌ 不得因为"多行共享同一个 sid"就释放/踢其中任何一行。
- ❌ 不得对 `lease_device IS NULL` 的行执行改密或踢连接（除 §4.3 的人工端点）。

> 理由记录在案：生产当前 5 行共享同一 sid 但这一事实**不能**证明它们是同一台设备（同一登录会话完全可以是两台手机）。v1 据此收敛 = 可能把另一台设备正在用的账号踢掉。

### 6.3 迁移期"设备短暂占两个号"的诚实说明

Worker v2 上线、Pages v1 未更新时，前端仍不带 `did` ⇒ 新建租约为 legacy 形态（`lease_sid='<sid>|<cid>'`, `lease_device=NULL`）。Pages v2 上线后，同一设备会先按新模型拿一个 `did` 租约，而它的 legacy 租约要等 ≤30 min 自然过期。
⇒ **过渡窗口内单设备最多短暂占用 2 个账号，最长 30 min**。池 5 / 单设备场景无影响；多设备场景应**在低峰期**按 §11.1 顺序发布，或临时把 `MQTT_POOL_SIZE` 视为容量预留。

---

## 7. 心跳与续租（含 SQL，v2 修正 B4）

### 7.1 语义

心跳 = **"我还在用这把租约"**，因此必须同时做两件事：① 记活动（供驱逐排序）；② 续期（否则活跃设备会被自己的 TTL 回收掉）。

### 7.2 唯一 SQL（条件更新，天然幂等与并发安全）

```sql
/* 入参：now, newExpires = now + ttl, username, did, sid, epoch, clientId */
UPDATE mqtt_browser_account
   SET lease_expires_at = ?newExpires,
       last_seen_at     = ?now,
       lease_client     = ?clientId,     -- 仅诊断，不作为清理依据
       updated_at       = ?now
 WHERE username     = ?username
   AND state        = 'leased'
   AND lease_device = ?did
   AND lease_sid    = ?sid
   AND lease_epoch  = ?epoch
   AND lease_expires_at < ?newExpires;   -- ★ 只延长，绝不缩短（防旧请求把新租约改短）
```

返回判定：

| `changes` | 后续动作 |
|---|---|
| 1 | `200 {ok:true, lease_expires_at:newExpires}` |
| 0 | 再 `SELECT state, lease_epoch, lease_device FROM … WHERE username=?`：<br>· 行不存在 ⇒ `404 lease_gone`<br>· `state!=='leased'` 或 `lease_epoch≠epoch` ⇒ **`409 lease_revoked`**<br>· `lease_device≠did` ⇒ `409 lease_revoked`<br>· 仅 `lease_expires_at >= newExpires`（别人刚续过）⇒ **仍返回 200**（并发心跳，语义等价） |

### 7.3 写入节流（并证明不会提前过期）

| 参数 | 值 | 说明 |
|---|---|---|
| 前端心跳间隔 | **60 s**（页面可见、未 revoked 即发） | 与连接状态解耦，覆盖 connecting/reconnecting |
| 服务端最小续租间隔 | **45 s** | `WHERE last_seen_at <= now - 45` 附加条件；命中 0 行且原因只是"刚续过" ⇒ 返回 200 + 当前 `lease_expires_at` |
| 租约 TTL | 1800 s | — |
| 强制续期余量 | 剩余 < 600 s 时**忽略节流**直接续 | 兜底，保证任何情况下剩余 ≥ 300 s |

**不会提前过期的论证**：最坏情况是"每 45 s 最多写一次"，而心跳每 60 s 一次 ⇒ 每次心跳都会落一次续租（节流只在重复请求时生效，例如两个页面同时心跳）。续租把 `lease_expires_at` 推到 `now+1800`，与下一次心跳间隔 60 s ⇒ 剩余恒 > 1700 s。✅

**写入量**：5 设备 × 60 次/h × 24 h = **7200 行/天**（D1 免费额度 100k 行/天），可接受；且 `lease_expires_at < newExpires` 条件保证重复请求写 0 行。

### 7.4 并发矩阵（心跳 / 释放 / 驱逐 / 过期回收）

| 并发对 | 结果 | 机制 |
|---|---|---|
| 心跳 × 心跳（两页面） | 都 200，只有一次写 | `lease_expires_at < newExpires` + 节流 |
| 心跳 × 释放 | 释放先成功 ⇒ 心跳 `changes=0` ⇒ `state='quarantined'` ⇒ **409**；心跳先 ⇒ 释放仍按 `epoch` 成功（epoch 未变） | 两者都带 `epoch`，`state` 不同步切换 |
| 心跳 × 驱逐 | 驱逐已把行改成 `quarantined` 且 `epoch+1` ⇒ 心跳 `changes=0` + epoch 不匹配 ⇒ **409**（旧设备立刻知道） | epoch 栅栏 |
| 心跳 × 过期回收 | 回收把行改 `quarantined` ⇒ 心跳 409；若心跳先续期，回收的 `WHERE lease_expires_at<=now` 不再命中 ⇒ 不回收 | 条件写 |
| 释放 × 驱逐 | 一方 `leased→quarantined` 成功，另一方 `state='leased'` 条件失败 ⇒ 换候选/放弃 | CAS |
| 恢复者 × 恢复者 | 只有拿到 `resume_lease_until` 的那个推进 | §8.3 |

### 7.5 过期回收不能误伤活跃租约

`releaseExpiredLeases()` 判据保持 `state='leased' AND lease_expires_at <= now`，**且**追加：
- 若 `last_seen_at > now - 120`（2 分钟内还活跃）⇒ **跳过本轮**，等下一轮（防止"续租请求正在路上"的边界误杀）。
- 每轮最多处理 **2 个**账号（每个要走 4 步 EMQX），避免 cron 超时。

---

## 8. 恢复机制（v2 修正 B5：不虚构实时性）

### 8.1 现实约束

- Worker 无常驻进程，唯一后台入口是 cron。
- 当前 cron `"5 * * * *"` = **每小时 :05**（`isPoolCron` 精确匹配，已核对 `cron-jobs.js:42-48`）。
- 因此 **v1 的"300 s 恢复"没有任何调度依据**，本文档撤回该承诺。

### 8.2 双通道设计（明确各自上界）

| 通道 | 触发 | 每次工作量上限 | 恢复时延上界 |
|---|---|---|---|
| **请求触发** | `GET /credentials`、`POST /heartbeat` 内部顺带推进 | 1 个账号 / 6 s 预算 | 有流量时 ≈ 下一次请求（秒级）；无流量时不推进 |
| **定时** | cron | 2 个账号 / 每次 | 取决于 cron 频率：`*/5` ⇒ **≤5 min + 处理时间**；保持 `5 * * * *` ⇒ **≤60 min** |

**结论（必须写进承诺）**：
- 若批准把 pool cron 改为 **`*/5 * * * *`**：恢复上界 **≤6 min**（5 min 调度 + ≤1 min 处理），可对外承诺"分钟级"。
- 若**不**批准改 cron：恢复上界 **≤61 min**，且只能在"有用户访问"时靠请求触发提前完成。**不得**对外声称 5 分钟。

> cron 变更属于生产配置变更，列入 §11.1 的实施步骤 5，**单独审批**。Cloudflare Cron Triggers 的最短粒度为 1 分钟，`*/5` 合法；代价是每天多 288 次调用（免费额度内）。

### 8.3 恢复的执行者互斥（`resume_lease_until`）

多个请求 + cron 可能同时想推进同一个隔离账号。用一次性短期租约做互斥：

```sql
/* 抢占恢复权：30 s 内只有我能推进这一步 */
UPDATE mqtt_browser_account
   SET resume_lease_until = ?now + 30
 WHERE username = ?username
   AND state    = 'quarantined'
   AND quarantine_step = ?step
   AND (resume_lease_until IS NULL OR resume_lease_until <= ?now);
-- changes=1 ⇒ 我负责推进；0 ⇒ 别人在做，直接返回
```

推进成功后清 `resume_lease_until`；崩溃则 30 s 后自然释放，不影响正确性（下一步的 CAS 仍带 `step`）。

### 8.4 恢复流程（幂等，逐步推进，每步独立提交）

| `quarantine_step` | 动作 | 成功 ⇒ | 失败 ⇒ |
|---|---|---|---|
| `password` | `ensureUserPassword(新随机密码)` | `step='acl'` | 停留 + `fail_reason`（**不入池**） |
| `acl` | `replaceUserAclRules(browserAclRules())` | `step='kick'` | 停留 |
| `kick` | `poolClientIds()` → 逐个 `kickClient()`（204/404 视为成功）；查询失败 ⇒ 失败 | `step='verify'` | 停留（**不得**跳过验证） |
| `verify` | `verifyDisconnected()`（≤10 s 轮询，0 个 connected） | `available`（或 `leased(新主)`） | 停留 |

**时长上界**：单账号单次恢复 ≤ 改密 10 s + ACL 10 s + 踢 10 s + 验证 10 s = **40 s 最坏**，但实际各步 0.3–1.5 s；因此"6 s 预算"必须按"**每步一个预算**"实现（进请求触发路径时，只推进一步就返回，不追求一次走完）。cron 路径可以一次走完一个账号。

> 这条"一次只推进一步"是 v2 的另一个关键修正：v1 试图在一次请求里跑完整个交接，导致 `kick_pending` 之后放弃验证。v2 允许跨请求/跨 cron 逐步推进，**但绝不允许跳过任何一步**。

### 8.5 长时间无法恢复的处理

- `quarantined` 超过 **24 h** 仍是同一步 ⇒ `poolStatus()` 标 `quarantined_stuck`，管理端告警；**仍不自动回 available**（可能旧密码仍有效）。
- `failed` ⇒ 沿用既有退避 `max(1h, 周期/4)`，由 `recoverStuckRotations` 重驱动；v2 把 `rotating` 的卡死阈值从 **48 h 改为 300 s**（因为正常轮换只需秒级，48 h 是 v1 的过时保守值）。

---

## 9. 页面级断开 vs 设备级释放（v2 修正 B7）

### 9.1 定义

| 概念 | 范围 | 触发 | 是否动服务端租约 |
|---|---|---|---|
| **页面级断开** | 当前页面 | 切 HTTP、页面隐藏 60 s、路由离开、手动连接切换 | ❌ 不动 |
| **设备级释放** | 整台安装 | 用户点"登出"（显式） | ✅ 进 `quarantined(release)` |
| **WebView 重建 / App 被杀** | — | 无事件可用 | ❌ 不动（靠 TTL + 心跳缺失 + 驱逐） |

### 9.2 具体规则

1. **`pagehide` 明确不得挂载释放**。当前 `installPoolReleaseHooks()` 从未被调用（事实），v2 要求：删除 `gf-app.js:40` 的死 import，并在 `gf-mqtt.js` 的该函数注释里写明"**常量禁用**：设备级租约不得由页面事件释放"，函数体保留为空实现或直接删除。
2. **切到 HTTP**：只 `disconnect("switched_to_http")`，**删掉** `releasePoolLease()` 调用（`gf-mqtt.js:99` 现有一行）。理由：同一设备可能还有另一个页面在用 MQTT，释放会误伤。
3. **登出**：保留 `releasePoolLease()`，但语义升级为"设备级"：带 `did + epoch`，服务端 CAS 进 `quarantined`。
4. **同一设备多页面**：共享同一 `(did, sid, epoch)`；每页一个 MQTT client；**任一页面的断开/隐藏/切通道都不影响其他页面**。
5. **防延迟 Release 误伤**：所有 release 必带 `epoch`；`pagehide`/切通道根本不发 release；因此不存在"页面延迟 release"这条路径（从源头消除，而不是靠服务端拒绝）。

### 9.3 多 Client 的记录方式（v2 修正 B3）

- **D1 只记录"最近一次心跳上报的 clientId"**（`lease_client`），用于诊断，**绝不作为清理依据**。
- 清理时一律**按 username 向 EMQX 枚举**（`poolClientIds`：精确过滤 + 本地复核 + 打满兜底全量）。
- 因此"只踢掉最后记录的那个 client"这个缺陷在结构上不可能发生。
- 单设备允许的 client 数：技术上不限（每页面 1 个），前端保留既有软上限 `tabs > pool_size ⇒ too_many_tabs`（提示性，不参与池容量计算）。

---

## 10. 前端设计

### 10.1 错误分类表（v2 修正"MQTT 客户端认证错误处理"）

已核对 vendored **mqtt.js v5.16.1**：CONNACK reason code ≥ 0x80 时
`_handleConnack` 会 `emit("error", new ErrorWithReasonCode(msg, rc))`，`ErrorWithReasonCode` 的 `.code === rc`（源码中 `ErrorWithReasonCode=ea`，`ea` 继承 `Error` 并赋 `code`）。

**MQTT 5 下的真实码**（必须修正现有实现）：

| rc | 含义 | 现有代码 `{4,5,135}` 是否覆盖 |
|---|---|---|
| **134** (0x86) | Bad User Name or Password | ❌ **漏了**（当前会被当成普通错误，不触发 `invalidateCredentials`） |
| 135 (0x87) | Not authorized | ✅ |
| 4 / 5 | MQTT 3.1.1 的 bad user/pass / not authorized | ✅（保留兼容，`protocolVersion:5` 下不出现） |

⇒ v2 要求把判定集合改为 **`{4, 5, 134, 135}`**，并加单元测试锁住（用桩 client 触发 `error` 事件）。

### 10.2 五类状态必须互不混淆

| 状态 | 判定信号 | UI 表现 | 是否允许自动重试 |
|---|---|---|---|
| 普通断线 | `close` 无 CONNACK | 「未连接」+ 现有通道提示 | 否（沿用 `reconnectPeriod:0`） |
| 池租约被撤销 | heartbeat **409**，或 CONNACK **134/135** 且 `_cred.source==="pool"` | 🔴 Modal「MQTT 连接已失效」 | **否** |
| 手动凭据错误 | CONNACK 134/135 且 `_cred.source==="manual"` | 现有「手动连接失败」红条 + 重试按钮，**绝不弹撤销弹窗** | 用户点才重试 |
| 账号申请失败 | HTTP 503/429，`reason` ∈ {pool_exhausted, pool_legacy_held, emqx_unknown, pool_store_unavailable} | toast + 设备页入口，**不**进 revoked | 否 |
| 从未连接 | `idle` | 「尚未连接」 | 否 |

判别 `_cred.source` 是硬要求：**同一组 rc 在手动模式下绝不解释为"账号被回收"**。

### 10.3 心跳状态机（前端）

```
状态：idle / connecting / online / reconnecting / offline / revoked
心跳发送条件：document.visibilityState==='visible' && !_leaseRevoked && _client !== null
             （不要求 connState==='online' —— connecting/reconnecting 也要证明"我还活着"）
间隔：60 s（setInterval，single instance；每次 ensureConnected 成功后确保只启动一次）
停止：disconnect() / 切 HTTP / revoked / 页面隐藏（沿用现有 60 s 隐藏断开逻辑）
409 处理：onLeaseRevoked("heartbeat")
```

> 前端**不需要**自己判断"租约快过期了要续"——续期是服务端的职责，心跳只是触发器。

### 10.4 撤销后的交互（严格按任务书 §4）

1. `onLeaseRevoked()`：置 `_leaseRevoked=true`、停心跳、`_cred=null`、`disconnect("lease_revoked")`、`setConnState("revoked")`、派发 `gf:lease-revoked`。
2. **Modal**（复用 `gf-ui.js::openSheet`，可关闭、唯一实例）：
   - 标题「MQTT 连接已失效」
   - 正文：账号已被其他设备回收 / 当前无法经 MQTT 下发命令 / 可手动重新申请
   - 按钮：`[使用账号池连接（primary）] [关闭]`
   - Esc / 点背景 / 关闭 都可关闭；关闭后 `_leaseRevoked` **保持 true**
3. **持久入口**：设备页「MQTT 手动连接（备用方式）」区已有的 `#man-pool`「使用账号池连接」按钮，`revoked` 时文案改为「**重新申请账号并连接**」；这是关闭 Modal 后的唯一入口。
4. 点击 ⇒ `requestPoolReconnect()`：`_leaseRevoked=false` → 清 `_cred` → `ensureConnected()`（**恰好一次**申请）。
5. **门禁**：`fetchCredentials()` 与 `ensureConnected()` 开头都判 `_leaseRevoked` ⇒ 任何间接调用（路由切换、命令下发、回前台）都不会自动申请。
6. **不静默切 HTTP**：命中 `lease_revoked` 时命令直接失败并提示，**不**改 `cmdPath`（沿用项目"不静默切通道"铁律）。
7. 不把 "HTTP 已发送" 显示成 "MQTT 已连接"：`renderChannel`/`renderManualMqtt` 的 `revoked` 态文案必须写明"直连不可用"，不得复用"已连接"绿标。

### 10.5 心跳响应的"假成功"防护（新增，必须实现）

心跳是撤销判定的**唯一主动通道**，因此它的响应必须被**严格校验形状**，否则回滚/故障期会出现两类假结论：

| 响应 | 含义 | 处理 |
|---|---|---|
| `200 {ok:true, lease_expires_at:<number>}` | 正常续租 | 继续 |
| `409 {error:"lease_revoked"}` | **唯一**的撤销信号 | `onLeaseRevoked("heartbeat")` |
| `404 not_found` | 服务端**没有**该端点（旧 Worker / 回滚后） | **停用心跳**（`_hbSupported=false`），**不**弹撤销窗、**不**断连；退化为"无心跳模式"（等同改造前行为） |
| `5xx` / 网络错误 | 服务端/网络故障 | 计一次失败并退避（30 s→60 s→120 s，上限 5 min）；**不**弹撤销窗 |
| `200` 但无 `lease_expires_at`（数字） | 形状不对（代理/中间件篡改） | 视为**失败**，按上一行退避；**绝不**当成续租成功 |

> 依据：`index.js:2303` 对未注册路径返回 `404 {"error":"not_found"}`（不是兜底 200）。即便如此也不可依赖"非 200 即撤销"——必须**只认 409 + `error==="lease_revoked"`** 这一个组合。

### 10.6 前端新增/修改点（文件级）

| 文件 | 改动 |
|---|---|
| `gf-mqtt.js` | `installId()`（localStorage）；`x-gf-device` 头；`_cred.epoch/.source`；心跳帧；`_leaseRevoked` 门禁；`onLeaseRevoked()`；`requestPoolReconnect()`；rc 集合改 `{4,5,134,135}`；切 HTTP 不再 release；`installPoolReleaseHooks` 停用并注明 |
| `gf-app.js` | 删死 import；`gf:lease-revoked` 弹窗（`openSheet`）；`#man-pool` 文案与行为切到 `requestPoolReconnect()`；`renderChannel` 增 `revoked` 文案；`onConnState` 重渲染 |
| `functions/api/mqtt/credentials.js` | 新增 `x-gf-device` 校验（复用 `clientInstanceId` 同款），畸形 ⇒ 400（**绝不静默丢弃**），转发 `x-ui-device` |
| `functions/api/mqtt/heartbeat.js`（新） | 会话 + 限流（60/min/IP）+ 转发 + 透传 409 |
| `functions/api/mqtt/release.js` | body 接受 `did`/`epoch`（`sendBeacon` 不能设头），同款校验；透传上游 409 |

---

## 11. Worker / D1 / 路由

### 11.1 数据库迁移 `0013_mqtt_pool_device_lease.sql`（新增，纯加法）

```sql
-- ① 身份与租约
ALTER TABLE mqtt_browser_account ADD COLUMN lease_device      TEXT;                      -- did
ALTER TABLE mqtt_browser_account ADD COLUMN lease_cid         TEXT;                      -- 页面实例（诊断）
ALTER TABLE mqtt_browser_account ADD COLUMN lease_epoch       INTEGER NOT NULL DEFAULT 0; -- fencing
ALTER TABLE mqtt_browser_account ADD COLUMN lease_client      TEXT;                      -- 最近心跳上报的 clientId（诊断）
-- ② 活动与隔离
ALTER TABLE mqtt_browser_account ADD COLUMN last_seen_at      INTEGER NOT NULL DEFAULT 0;
ALTER TABLE mqtt_browser_account ADD COLUMN quarantine_step   TEXT;                      -- password|acl|kick|verify
ALTER TABLE mqtt_browser_account ADD COLUMN quarantine_reason TEXT;                       -- release|logout|expire|evict|duplicate|rotate|legacy
ALTER TABLE mqtt_browser_account ADD COLUMN quarantine_started_at INTEGER NOT NULL DEFAULT 0;
ALTER TABLE mqtt_browser_account ADD COLUMN reserved_device   TEXT;                       -- 驱逐目标（verify 通过后转正）
ALTER TABLE mqtt_browser_account ADD COLUMN reserved_sid      TEXT;
ALTER TABLE mqtt_browser_account ADD COLUMN resume_lease_until INTEGER NOT NULL DEFAULT 0; -- 恢复互斥
ALTER TABLE mqtt_browser_account ADD COLUMN kick_pending      INTEGER NOT NULL DEFAULT 0;  -- 仅诊断/恢复标记，**不代表可用**

-- ③ 结构性约束
CREATE UNIQUE INDEX IF NOT EXISTS ux_mba_lease_device
    ON mqtt_browser_account(lease_device)
    WHERE lease_device IS NOT NULL AND state = 'leased';
CREATE INDEX IF NOT EXISTS ix_mba_state_seen ON mqtt_browser_account(state, last_seen_at);
CREATE INDEX IF NOT EXISTS ix_mba_quarantine ON mqtt_browser_account(quarantine_step, resume_lease_until)
    WHERE state = 'quarantined';

-- ④ 回填（仅活动时间的近似，不动归属）
UPDATE mqtt_browser_account SET last_seen_at = last_allocated_at WHERE last_seen_at = 0;
```

设计说明：
- **不新增 CHECK**（枚举演进风险，与 0008/0010 同律）；`did` 格式在应用层 `normDeviceId()`。
- **不改既有列语义**：`lease_sid` 继续存在，新写入改为**纯 sid**；旧行仍是 `<sid>|<cid>` ⇒ 结构上天然不命中新复用查询（这正是 B1 想要的"不认 legacy"）。
- 回填 `last_seen_at` 只影响驱逐排序，不改变任何归属与可用性。

迁移失败/回滚：SQLite `ALTER TABLE ADD COLUMN` 逐条执行，失败只影响该条；回滚 = 不删除列（D1 不支持 DROP COLUMN 的稳妥路径），仅回滚 Worker/Pages 代码；多余列对旧代码完全无害（v1 已证明：旧 Worker 不 `SELECT *` 的列不受影响，实际 `SELECT *` 取到多余列也不影响使用）。

### 11.2 Worker 模块改动

| 文件/函数 | 改动 |
|---|---|
| `mqtt-pool.js::normDeviceId` | 新增 `/^[A-Za-z0-9_-]{16,40}$/` |
| `allocate(env,{sid,cid,did})` | 按 §5.1 重排；`did=""` ⇒ **完全走 legacy 分支**（与当前行为逐字一致） |
| `quarantineLease(env,{username,epoch,reason,reserved?})` | 新增：`leased→quarantined` CAS（§3.2） |
| `advanceQuarantine(env,{username,maxStepBudgetMs})` | 新增：按 `quarantine_step` 推进**一步**（§8.4） |
| `touchLease` → **`renewLease`** | 改名并改语义：续期 + 活动（§7.2 SQL） |
| `releaseLease` | 新签名 `{sid,cid,did,epoch,username}`；`lease_device IS NOT NULL` ⇒ 必须 `did+epoch` 匹配否则 `409 lease_guard_failed`；legacy 行保留旧逻辑 |
| `releaseExpiredLeases` | 改为"送入 `quarantined(expire)`"，每轮 ≤2 个，跳过 120 s 内活跃者 |
| `rotateDueAccounts` | 复用 `quarantine(rotate)` 流水 |
| `recoverStuckRotations` | `rotating` 阈值 48 h → **300 s**；新增 `quarantined` 卡死统计 |
| `poolStatus` | 增 `lease_device`(前 8 位)、`last_seen_at`、`quarantine_step`、`resume_lease_until`、`by_state` 含 quarantined |
| `ROTATION_EVENTS` | 增 `quarantine_start`、`quarantine_step_ok`、`quarantine_done`、`evict`、`evict_rejected_online`、`renew`、`revoke`、`legacy_release_admin` |

### 11.3 路由与兼容（v2 修正"路由前缀"）

**结论：改为族前缀，但必须配 fail-closed 分派 + 守卫测试。**

```js
/* index.js 顶部路由段 */
if (path.startsWith("/api/ui/mqtt/")) {          // 族前缀：新增端点默认受保护
    return await handleUi(request, env, path, url);   // handleUi 内未注册 ⇒ 404（已鉴权后才 404）
}
```

安全性论证（回应"不会让未知路由进入错误分支"）：
1. 前缀匹配只扩大**鉴权覆盖**，不扩大**能力**：未知路径先过 `authorizeAdmin`（Bearer），再由 `handleUi` 返回 `404 unknown_ui_path`。
2. 若沿用逐条枚举，新增 `/api/ui/mqtt/heartbeat` 忘了登记就会掉进兜底 ingest 分支返回 **200**（`index.js:322` 的注释里已经记着同类事故），这是**更危险**的失败方向。
3. 守卫测试：扫描 `index.js` 中所有 `path === "/api/ui/..."` 字面量，断言每个都被某个前缀规则覆盖；并断言 `GET /api/ui/mqtt/__nonexistent` 返回 **401（无 Bearer）/ 404（有 Bearer）**，**不得**返回 200。

**新旧版本共存矩阵**

| D1 | Worker | Pages | 行为 | 安全 |
|---|---|---|---|---|
| 0013 | v1 旧 | v1 旧 | 新列被忽略；租约仍为 legacy 形态 | ✅ 与现状一致 |
| 0013 | **v2** | v1 旧 | `did=""` ⇒ legacy 分支；新请求仍建 legacy 租约 | ✅ 无回归 |
| 0013 | v2 | **v2** | 新模型生效；legacy 行冻结至过期 | ✅ 过渡期 ≤30 min 双占用 |
| 0013 | v1 旧 | v2 新 | ⚠️ **禁止**：v2 前端会发 `did`/`epoch`，旧 Worker 不认 ⇒ heartbeat 命中未注册路径返回 **404 `not_found`**，release 缺 epoch 校验退回旧语义。前端按 §10.5 会停用心跳而非误报撤销，但**账号回收能力缺失**（旧 Worker 只会改 D1，不会改密/踢连接）⇒ 安全语义不成立 | 回滚必须**先 Pages 后 Worker** |

⇒ **部署顺序：0013 → Worker v2 → 观察 → Pages v2**；**回滚顺序：Pages → Worker**（迁移不回滚）。

### 11.4 故障行为

| 故障 | 行为 |
|---|---|
| D1 写不可用 | `pool_store_unavailable` + `storage` 分类（沿用 `d1-guard.js`），**不得**与池满混淆 |
| D1 读失败 | 503，不猜 |
| EMQX 管理 API 全挂 | 无 `available` ⇒ 全池 `503 pool_unavailable`；隔离账号停在原步；**绝不**降级成"直接发旧密码" |
| EMQX 查询打满/超时 | 判 `unknown`，不驱逐、不交接、不回收 |
| cron 未触发 | 请求触发兜底推进；最坏 60 min（或改 `*/5` 后 6 min） |

---

## 12. 测试矩阵（22 项 = 任务书要求的 21 项全覆盖 + 1 项补充，分层标注）

层次：**U**=单元（纯函数/模块，Node）｜**D**=D1 集成（真 SQLite + 真 migration，`test/lib/d1_write_probe.mjs`）｜**E**=EMQX API 模拟（stub `emqx-admin`）｜**R**=真机/生产（人工）

| # | 用例 | 层 | 断言要点 |
|---|---|---|---|
| T-01 | 同一安装连续重启 5 次（同 did 同 sid，5 个不同 cid） | D | 5 次返回**同一 username**；`leased` 行数=1；写入次数 ≤2（节流） |
| T-02 | 同设备历史多租约（全部 `lease_device=did`）能收敛 | D | 保留 `lease_expires_at` 最大者；其余进 `quarantined(duplicate)`；**每请求最多推进 1 个** |
| T-03 | 同一 sid 下多个不同 did | D | 三行都**原样保留**；无任何 `quarantined`/踢连接；断言"legacy 冻结" |
| T-04 | 两新设备同时申请最后 1 个 available | D | 恰好 1 个 `changes=1`；另一个走驱逐或 503；无两行同 did |
| T-05 | 池满驱逐最久未活跃 | D+E | 被驱逐者 = `min(last_seen_at)` 且不在 EMQX 在线集 |
| T-06 | 活跃设备不被误驱逐 | D+E | `last_seen_at` 新鲜 ⇒ 不进候选；即使候选也被在线否决 |
| T-07 | EMQX 查询失败/打满 | E | **不驱逐**；返回 `emqx_unknown`；无任何状态变更 |
| T-08 | 改密失败 | E | 停 `quarantined(password)`；**新设备拿不到账号**；无 `lease_device` 变更 |
| T-09 | 踢下线失败 | E | 停 `quarantined(kick)`；**不交付**；`kick_pending=1` 仅作恢复标记 |
| T-10 | 踢完仍有旧连接 | E | `verifyDisconnected` 返回 remaining>0 ⇒ 停 `quarantined(verify)`；**不交付**；下一轮重试 |
| T-11 | 查询结果不完整（打满 limit） | E | 判 unknown ⇒ 停/不驱；断言没有"乐观放行"分支 |
| T-12 | Worker 在轮换中途崩溃 | D+E | 行停原步；`resume_lease_until` 30 s 后失效；下一次推进从该步继续；**全程无 available 提前出现** |
| T-13 | Heartbeat **确实**延长 `lease_expires_at` | D | 连续 3 次心跳后 `lease_expires_at` 单调递增；`last_seen_at` 同步推进；`changes` 符合节流预期 |
| T-14 | 旧页面缺 epoch 的 Release | D | `lease_device IS NOT NULL` 的行**拒绝**（`released:0` + 409），行不变 |
| T-15 | 旧 epoch 的延迟 Heartbeat/Release | D | 409；目标行 `updated_at`/`lease_expires_at`/`lease_device` 全部不变 |
| T-16 | 一个页面切 HTTP 不影响其他页面 | U | 切 HTTP **不发** release 请求（fetch 计数=0）；连接仅本页断开；其他页心跳仍 200 |
| T-17 | 同设备多 Client 全部被清理 | D+E | 3 个 clientId 时 3 次 DELETE；`verify` 需 0 个才通过；不因 `lease_client` 只记 1 个而漏踢 |
| T-18 | 手动密码错误不误报"账号回收" | U | `source==='manual'` + rc 134 ⇒ 走手动失败分支；`leaseRevoked()===false`；不弹撤销 Modal |
| T-19 | 撤销 Modal 可关闭、入口仍在 | U | 关闭后 `_leaseRevoked===true` 且 `#man-pool` 仍渲染；无新增 fetch |
| T-20 | cron 与请求触发并发恢复 | D+E | `resume_lease_until` 互斥；只推进一次；状态终态正确；无双重改密 |
| T-21 | 新旧 Pages/Worker 兼容与回滚 | D | `did=""` 走 legacy 且行为与 v1 逐字一致；v2 前端 + v1 Worker 明确拒绝（版本探测返回 `unsupported`） |
| **T-22** | 心跳响应形状防护（§10.5） | U | `404`/`5xx`/无 `lease_expires_at` 三种响应均**不**触发撤销、**不**断连；仅 `409 + error:"lease_revoked"` 触发；退避序列 30→60→120→300 s |

强制纪律：
- **EMQX 全程 stub，禁止对生产发任何写操作**（改密/踢连接/ACL）。
- 模拟通过 ≠ 真实 EMQX 通过：T-05/T-07/T-09/T-10/T-11/T-17 标注"仅 stub 验证"，真实验证需 §13 的受控窗口。
- 新增套件必须登记进 `guo-feeder-api/test/run_all.mjs`（Worker）与 `tests/run_all.mjs`（Pages 自动扫描）。
- 负向探针要求：T-03（不得收敛）、T-07（不得驱逐）、T-09（不得交付）、T-14（不得释放）必须"把正确代码改坏 ⇒ 断言变红"。

---

## 13. 部署、回滚与验证

### 13.1 顺序（每一步都可独立回滚）

| 步 | 动作 | 前置 | 回滚 |
|---|---|---|---|
| 1 | 应用迁移 `0013` | 备份 D1（`wrangler d1 export`） | 不回滚（加法） |
| 2 | 部署 Worker v2 | 步 1 完成 | 重新部署上一版本 |
| 3 | 只读验收（旧 Pages）：`/credentials` 仍 200、`release` 仍工作 | 步 2 | — |
| 4 | 部署 Pages v2 | 步 3 通过 | 重新部署上一版本（**先于** Worker 回滚） |
| 5 | （单独审批）cron 改 `*/5 * * * *` | 步 4 稳定 24 h | 改回 `5 * * * *` |
| 6 | 观察 30 min：`allocate` 复用率、`quarantined` 数量、`pool_exhausted` 次数 | — | — |

> **步 5 执行记录（2026-11-03 04:50–05:20 · 已单独授权）**
>
> - 已执行：池 cron 由 `5 * * * *` 改为 `*/5 * * * *`（每 5 分钟），**仅改 Worker**，未动 Pages / D1 / Turnstile / EMQX。
> - 同步点：`wrangler.toml [triggers].crons`、`cron-jobs.js::POOL_CRON`、`isPoolCron()` 精确匹配、`index.js::scheduled()` 分派注释。
>   ★ **`POOL_CRON` 与 `wrangler.toml` 必须逐字一致** —— 不一致时 `isPoolCron()` 不匹配，
>   池维护会**静默**落到每日分支（不报错、无日志）。已由 `test/current/pool_cron_schedule.test.mjs` 静态锁死。
> - 每日保留清理 cron `0 3 * * *` **未改动**；crons 数组仍**恰好 2 条**（未新增重复 trigger）。
> - 恢复上界由 **≤60 min** 降为 **≤6 min**（5 min 调度 + 单批 ≤20 s）。
>   单批工作量不变（`advanceQuarantineBatch` 仍 ≤2 账号 / 20 s 预算）⇒ 频率提升不放大写压力。
> - 请求触发通道 `advanceQuarantineSidecar()` **保留不变** ⇒ 双通道互补。
> - 回滚：把两处改回 `5 * * * *` 即可（`wrangler.toml` + `cron-jobs.js::POOL_CRON`）。

### 13.2 上线后必须采集的证据（不做就等于没验证）

1. 同一设备启动 5 次后：`SELECT COUNT(*) FROM mqtt_browser_account WHERE state='leased' AND lease_device=?` = 1。
2. `mqtt_browser_rotation` 中 `quarantine_*` 流水完整（每账号 4 步齐全，无跳步）。
3. `lease_expires_at - last_seen_at` 恒 ≈ 1800（证明心跳在续期）。
4. `poolStatus().by_state` 与 EMQX 在线集合人工交叉核对一次。

### 13.3 完整回滚判据

出现以下任一情况立即回滚 Pages → Worker：
- 池中 `quarantined` 占比 > 60% 持续 15 min（说明 EMQX 侧有问题）；
- `pool_exhausted` 频率高于改造前；
- 前端出现"已连接"但实际无 MQTT 的错配。

---

## 14. 尚未解决 / 需外部条件的事项（诚实标注）

| 项 | 原因 | 需什么才能定 |
|---|---|---|
| Cron 最小间隔能否设 `*/5` | Cloudflare 计划类型未在本轮核对（免费/付费配额不同） | 查账户计划 + 一次 `wrangler` 配置预演；**不改也不影响正确性**，只影响恢复时延（60 min vs 6 min） |
| `AbortSignal.timeout` 在 workerd 的实际生效情况 | `emqx-admin.js:105` 自述"未实测" | 保持既有 `Promise.race` 兜底（`presence-emqx.js` 已用此模式），不依赖 signal |
| EMQX `/clients` 一次查询的真实上限 | 已知 presence 用 50/200，池账号连接数个位数 | 受控窗口跑一次 `limit=200` 观察 `meta`；本计划已用"打满即 unknown"规避 |
| Android WebView 是否触发 `pagehide` | 未实测 | **本设计不依赖**（设备级租约不由页面事件释放），故不阻塞 |
| localStorage 跨进程持久性 | 未真机验证 | 真机验证一次即可；失效则退化为每会话一个新 did（旧行为，不更糟） |
| 是否允许客户端自助"设备级释放" | 可能被滥用（反复释放/申请） | 沿用 Pages 限流（release 60/min/IP）+ 只允许 `did+epoch` 匹配；若被滥用再加冷却 |

---

## 15. 实施任务分解（可直接派工）

| # | 任务 | 产出 | 依赖 |
|---|---|---|---|
| 1 | 写 `0013` 迁移 + 迁移测试（空库跑 0000→0013，断言列/索引/约束） | SQL + 测试 | — |
| 2 | `mqtt-pool.js`：`normDeviceId`、`allocate` 重排（含 legacy 分支）、`quarantineLease`、`advanceQuarantine`、`renewLease`、`releaseLease` 新签名 | 模块 + 单测 | 1 |
| 3 | `releaseExpiredLeases` / `rotateDueAccounts` / `recoverStuckRotations` 改造（有界、走隔离流水） | 模块 + 测试 | 2 |
| 4 | `index.js`：`/api/ui/mqtt/` 前缀化 + `heartbeat` 端点 + credentials/release 参数 + 守卫测试 | 路由 + 测试 | 2 |
| 5 | Pages：`credentials`/`heartbeat`/`release` 三个函数（`x-gf-device` 校验、限流、透传 409） | 函数 + 测试 | 4 |
| 6 | 前端 `gf-mqtt.js`：installId、心跳、epoch、revoked、rc 集合、切 HTTP 不再 release | 模块 + 单测 | 5 |
| 7 | 前端 `gf-app.js`：Modal、`#man-pool` 改造、`renderChannel` revoked 态、删死 import | 模块 + 测试 | 6 |
| 8 | 测试套件：T-01…T-21 落地，登记两个 runner | 测试 | 2–7 |
| 9 | 部署与验证（另需授权） | 迁移/部署记录 | 8 |

---

## 16. 一页速查（供审批人 30 秒判断）

1. **身份**：`did`（localStorage）+ `sid`（cookie）双条件复用；`sid` 单独绝不收敛/释放/踢。
2. **安全铁律**：任何"退出 leased"都必须经过 **改密 → 踢连接 → 验证断开** 才能再分配（不变量 I-1）。
3. **失败即隔离**：改密/踢/查询/验证任一失败或未知 ⇒ 账号停在 `quarantined`，**绝不给新设备**。
4. **续租**：心跳 60 s，服务端条件更新同时推 `lease_expires_at` 与 `last_seen_at`，节流 45 s，剩余 <600 s 强制续。
5. **驱逐**：`last_seen_at` 最旧 + EMQX 在线否决 + 3 min 保护期；查询不完整 ⇒ 不驱逐。
6. **恢复**：请求触发（1 步/请求）+ cron（2 个/次）；**改 `*/5` 才谈 6 min，否则上界 60 min**。
7. **旧版兼容**：`did` 缺失 ⇒ 完全走旧逻辑；`lease_device` 非空的租约，缺 epoch 的 release **必拒**。
8. **页面 vs 设备**：切 HTTP/关页面**不释放**；只有登出做设备级释放；`pagehide` 明确禁用。
9. **前端**：只在"心跳 409 或 池凭据 CONNACK 134/135"时才报"账号已回收"；可关 Modal，设备页入口常驻，**无自动重试**。
