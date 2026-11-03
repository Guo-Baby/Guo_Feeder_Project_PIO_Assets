# 浏览器 MQTT 动态账号池改造计划：设备实例租约 + 满池驱逐 + 手动重连

> ⚠️ **本文档（v1）已被 `MQTT-Pool-Device-Lease-Plan-v2-2026-11-03.md` 取代，不得据此实施。**
> v1 的七处阻断问题（sid 当设备身份、释放不改密、kick 失败仍交接、心跳不续租、恢复时间与 cron 不符、
> 旧 release 缺 epoch 放行、页面级释放设备级租约）在 v2 中已逐一修正；本文档仅保留作对比基线。

- 文档版本：v1（2026-11-03 03:0x，**只调查与设计，未改任何代码/库/EMQX/部署**）
- 适用仓库：`Cloudflare_Assets`（Worker `guo-feeder-api` + Pages `guo-feeder-pagesdev`）为主，`Android_Assets` 只做验证不改代码
- 目标读者：下一阶段的实施者（AI 编码执行）
- 证据基线：生产 D1 `YOUR_D1_DATABASE_NAME`、生产 Worker `151b683b`、Pages `af077daf`、代码 commit `89b3c38`

---

## 0. 结论先行

| 问题 | 结论 |
|---|---|
| 为什么反复启动会占多个账号 | 租约归属键是 `<sid>\|<cid>`，`cid` 存在 **sessionStorage**，App/WebView 每次重建都换新值；`allocate()` 的复用查询要求 `lease_sid` **完全相等** ⇒ 每次都 miss ⇒ 每次重新分配 |
| 为什么旧租约不释放 | `installPoolReleaseHooks()` **被 import 但从未被调用**（`gf-app.js:40` 只有 import，无调用点）⇒ `pagehide` 释放是**死代码**。生产 `mqtt_browser_rotation` 里 14 条 `release` 全部是 `{"via":"lease_expired"}`（cron 回收），没有一条来自客户端 |
| 为什么池满后彻底卡死 | `allocate()` 只有「available 或已过期」两路候选，**没有驱逐**；池 5/5 活跃租约 ⇒ `503 pool_exhausted` |
| 改造主线 | ① 引入 **install id（`did`，localStorage）** 做设备身份；② 租约**按设备唯一**（D1 唯一部分索引 + CAS）；③ 满池时**驱逐最久未活动且 EMQX 侧确无连接**的租约，并**改密 + 踢连接**完成交接；④ 加**心跳**维持 `last_seen_at` 并提供失效检测通道；⑤ 前端加 `lease_revoked` 态与手动重连入口，**禁止自动循环申请** |
| 是否改 Envelope / 设备账号 / EMQX 配置 | 否。设备账号（`dev_*` / `YOUR_MQTT_USERNAME`）与池账号（`gf_web_NN`）物理隔离不变；不改 EMQX 配置文件，只用既有管理 API（改密 / 列连接 / 踢连接） |

---

## 1. 调查范围与取证

### 1.1 读过的代码（依据，不是架构文档）

| 文件 | 关注点 |
|---|---|
| `guo-feeder-api/src/mqtt-pool.js`（1317 行） | `allocate()` L535、`releaseLease()` L763、`releaseExpiredLeases()` L898、`rotateAccount()` L978、`kickPoolClients()` L1095、`verifyDisconnected()` L1170、`recoverStuckRotations()` L1217、`poolStatus()` L1283 |
| `guo-feeder-api/src/index.js` | `/api/ui/**` 鉴权枚举 L322、credentials L1915、release L2010 |
| `guo-feeder-api/src/emqx-admin.js` | `ensureUserPassword` L276、`replaceUserAclRules` L318、`kickClient` L361、`queryClients` L418（`username=` 可用、`clientid=` 静默失效） |
| `guo-feeder-api/src/cron-jobs.js` | `"5 * * * *"` → `runMqttPoolMaintenance()`：`releaseExpiredLeases` / `rotateDueAccounts(max 1)` / `recoverStuckRotations(max 2)` |
| `guo-feeder-api/migrations/0010_mqtt_browser_pool.sql` | 表结构、状态机、`CHECK username GLOB 'gf_web_[0-9][0-9]'` |
| `guo-feeder-pagesdev/functions/api/mqtt/credentials.js` / `release.js` | Pages 侧代理、`x-ui-cid` 校验、`readSessionFull` |
| `guo-feeder-pagesdev/assets/js/gf-mqtt.js` | `tabId()` L370、`fetchCredentials()` L409、`newClientId()` L510、`ensureConnected()` L527、`releasePoolLease()` L768、`installPoolReleaseHooks()` L833 |
| `guo-feeder-pagesdev/assets/js/gf-app.js` | `renderChannel()` L7378、`renderManualMqtt()` L7543、`man-pool` 按钮 L7647、登出释放 L3798/L11712 |
| `Mobile`/`Android_Assets/app/.../MainActivity.kt` | `domStorageEnabled=true`、`databaseEnabled=true`、`cacheMode=LOAD_DEFAULT`、**无任何 clear/delete** |
| 测试 | `guo-feeder-api/test/current/p0_pool_lifecycle.test.mjs`、`p0_pool_multitab.test.mjs`、`p0_ghost_lease.test.mjs`；夹具 `test/lib/d1_write_probe.mjs`（**真 SQLite + 真 migration**） |

### 1.2 生产只读取证（2026-11-03 02:3x UTC+8）

**（A）账号池当前状态 —— 缺陷的直接复现**

```
gf_web_00 leased <sid 7079b843…>|tossjr5izmvzl9afc  alloc 02:24:44  exp 02:54:44
gf_web_01 leased <sid 7079b843…>|tjs6l1imkmvzl9zuj  alloc 02:25:17  exp 02:55:17
gf_web_02 leased <sid 7079b843…>|twijdofzcmvzl91ec  alloc 02:24:32  exp 02:54:32
gf_web_03 leased <sid 7079b843…>|teyed6tflmvzkv5qi  alloc 02:13:45  exp 02:43:45
gf_web_04 leased <sid 7079b843…>|t03h5ok5amvzko2zf  alloc 02:08:15  exp 02:38:15
```

- **同一个 sid（同一登录会话）**、**5 个不同 cid**、17 分钟内 5 次分配 ⇒ 5/5 全占满。
- 结论：这不是"用户开了 5 个标签页"，而是**同一设备反复重建页面**把池吃光。

**（B）释放流水 —— 客户端从未释放过**

```
release 13 条，detail 全部 {"via":"lease_expired"}
```

⇒ `pagehide` 释放路径在生产上**一次都没生效**（与 1.1 的"死 import"互相印证）。

**（C）分配流水**：近 24h `allocate` 22 次，`rotate_*` 各 4 次（cron 正常）。

**（D）迁移基线**：`d1_migrations` 已应用到 **0012**，共 13 条 ⇒ 新迁移编号 = `0013`。

**（E）租期默认值**：`MQTT_POOL_LEASE_MIN` 缺省 30 min（`leaseSeconds()` 钳 5..240）；`MQTT_POOL_SIZE` 缺省 5（钳 1..20）；`MQTT_POOL_ROTATE_HOURS` 缺省 24。

---

## 2. 当前实现架构（**已由代码证实**的事实）

```
浏览器/WebView
  │  GET /api/mqtt/credentials   (cookie: gf_session)
  │    headers: x-ui-cid = tabId()          ← sessionStorage，每次新建页面都变
  ▼
Pages  functions/api/mqtt/credentials.js
  │  readSessionFull() → sid（服务端可信）
  │  per-isolate 限流 20/min/IP
  ▼
Worker GET /api/ui/mqtt/credentials      (Bearer ADMIN_API_TOKEN + x-ui-sid + x-ui-cid)
  ▼
mqtt-pool.js::allocate(env,{sid,cid})
  │  ① 复用：SELECT … WHERE lease_sid = '<sid>|<cid>' AND state='leased' AND expires>now
  │  ② 抢占：SELECT … WHERE state='available' OR (state='leased' AND expires<=now)
  │           ORDER BY last_allocated_at ASC LIMIT 1
  │           UPDATE … WHERE username=? AND (同样条件)   ← CAS
  │  ③ 都没有 ⇒ 503 pool_exhausted                        ← **没有驱逐**
  ▼
D1 mqtt_browser_account(username PK, state, password_ct, password_nonce,
                        password_updated_at, last_allocated_at, lease_sid,
                        lease_expires_at, rotation_started_at, rotation_attempts,
                        fail_reason, created_at, updated_at)
```

关键结构事实：

1. `lease_sid` **一列存两个东西**：`"<sid>|<cid>"`（P0 · 2026-10-31 改造）。没有独立的 `cid` 列。
2. 表里**没有** `device_id`、`last_seen_at`、`lease_epoch`、`kick_pending` 任何一列。
3. `allocate()` 的复用只看 `lease_sid` 字符串相等，**不理解"同一设备"**。
4. 池满 = 立即 503，**没有任何回收/驱逐路径**（快路径只有"过期租约可复用"这一种自愈）。
5. 轮换（`rotateAccount`，7 步）**已具备**我们需要的全部副作用原语：改密 → 重放 ACL → 按 username 踢连接 → 验证断开 → 落密文。**驱逐交接可以直接复用这条流水**，只是入口状态从 `available` 改成 `leased`。
6. `cron "5 * * * *"` 提供每小时一次的兜底维护位（已有），无需新增触发器。
7. Worker 是请求驱动的：没有任何常驻状态；`allocate` 的并发安全完全靠 **SQL CAS + `changes===1`**。

### 2.1 代码与运行配置不一致之处（必须指出）

| 现象 | 证据 | 影响 |
|---|---|---|
| `installPoolReleaseHooks()` 导出且被 import，但**无调用点** | `grep -rn "installPoolReleaseHooks"` 只命中定义与 import | `pagehide` 释放是死代码；"池会被主动归还"的假设不成立，这是本次缺陷的放大器 |
| 注释宣称"多标签页各持一把租约"，但真实主因是**同一页面反复重建** | 生产 5 条租约同 sid 不同 cid，17 分钟内 | 之前把"同会话多页"当成本，实际成本是"同设备多代" |
| `p0_pool_multitab.test.mjs` 断言的语义是"每 cid 一把租约" | 套件名与注释 | 本次改造会**改变该契约**，必须重写该套件而不是"调绿" |

---

## 3. 根因（三条，逐条对应证据）

### R1（主因）设备身份缺失 ⇒ 复用键随页面重建而失效

`cid = tabId()` 存在 `sessionStorage`（`gf-mqtt.js:370`）。Android WebView 的 `sessionStorage` 生命周期 = 页面会话；App 被杀 / WebView 重建 / 进程重启都会换新 `cid`。
复用查询 `WHERE lease_sid = '<sid>|<cid>'` 因此 **必然 miss**，即使 `sid`（cookie，`gf_session` Max-Age 12h）完全没变。

> 生产反证：5 条租约 sid 全部相同 ⇒ 会话一直没变，变的只有 cid。

### R2（放大器）旧租约没有回收路径

`pagehide` 释放从未挂载（§2.1），`releaseExpiredLeases` 只在 cron（每小时 :05）跑，租期 30 min。于是每次启动都会**净增**一把租约，直到 5/5。

### R3（致命）满池没有驱逐

`allocate()` 两条候选路都不含"活跃但闲置"的租约 ⇒ 5/5 之后**永久 503**，直到 cron 回收。用户感受就是"App 突然连不上直连通道"。

---

## 4. 目标语义（改造后的状态机）

### 4.1 身份三层（不要混用）

| 层 | 名称 | 载体 | 生命周期 | 用途 |
|---|---|---|---|---|
| L1 | `did`（install id） | **localStorage** `gf.install_id` | 直到清数据/卸载 | **设备身份**：租约归属、复用、驱逐对象 |
| L2 | `sid`（会话） | Cookie `gf_session`（HttpOnly，12h） | 12h | **授权**：谁能释放、Pages 透传可信 |
| L3 | `cid`（页面实例） | sessionStorage `gf.mqtt.tab` | 一次页面会话 | **观测/精确释放**：不再作为复用键 |

> 判定原则：**复用看 `did`，鉴权看 `sid`，隔离连接看 MQTT `clientId`（`gfweb_<rand>`）**。

### 4.2 租约状态机（在 0010 基础上只增列，不改既有语义）

```
                       allocate(did)                evict(victim)
available ──────────────────────► leased ──────────────────────► rotating
    ▲                                 │                              │
    │        release(did) / TTL 到期   │                              │ 改密+踢连接+落密文
    └─────────────────────────────────┘                              ▼
                                                              leased(新 did)
rotating ──失败──► failed ──recover(300s 后 / cron)──► rotating ──► available
```

新增不变式（**结构性**，不靠注释）：

1. `state='leased'` 的行，`lease_device` **唯一**（部分唯一索引）。
2. 任何改变归属的写入都必须带 `lease_epoch`（fencing），`changes!==1` ⇒ 本次操作失效。
3. `rotating` 期间 `lease_device` 必须为 `NULL`（否则索引会挡住交接）。

---

## 5. 同设备身份识别方案

### 5.1 生成与持久化

`gf-mqtt.js` 新增：

```js
const LS_INSTALL = "gf.install_id";      // ★ localStorage（不是 sessionStorage）

export function installId() {
  try {
    let v = localStorage.getItem(LS_INSTALL);
    if (v && /^[A-Za-z0-9_-]{16,40}$/.test(v)) return v;
    const b = crypto.getRandomValues(new Uint8Array(18));   // 144 bit
    v = btoa(String.fromCharCode(...b)).replace(/\+/g,"-").replace(/\//g,"_").replace(/=+$/,"");
    localStorage.setItem(LS_INSTALL, v);
    return v;
  } catch (e) {
    // 隐私模式/存储被禁 ⇒ 退化为一次性内存值（等价旧行为，不抛错）
    return _ephemeralDid || (_ephemeralDid = "e" + Math.random().toString(36).slice(2, 18));
  }
}
```

为什么是 localStorage：
- **跨 WebView 重建 / App 重启 / 页面刷新稳定**（WebView 把 localStorage 落在 `/data/data/com.guofeeder.app/app_webview/Local Storage/`，Android 侧代码 `MainActivity.kt` **没有任何 clear/delete 调用**，已在 §1.1 核实）。
- 同源同 App 安装唯一 ⇒ **不会出现两台设备共享同一 did**。
- 清数据/卸载 ⇒ 新 did，旧租约靠 TTL/驱逐自愈（可接受，且比"无身份"更好）。

### 5.2 传输与校验（信任边界）

```
浏览器  → Pages :  x-gf-device: <did>        （新增）
Pages   → Worker:  x-ui-device: <did>        （新增；Pages 是唯一注入方）
Worker          :  normDeviceId() 校验 /^[A-Za-z0-9_-]{16,40}$/，非法 ⇒ 400
```

- `did` 是**客户端声明**，因此**永远不能当授权凭据**：它只用于"找自己的租约"。真正的授权仍是 `sid`（Pages 从会话读出，客户端无法伪造）。
- 副作用上界：伪造他人 `did` 最多做到"抢复用同一把池账号"（他本来也能申请到一把），**不能**读写设备、不能发布命令、不能释放别人的租约（释放仍钉 `sid` + `epoch`）。
- 采用（adopt）条件建议收紧为 `did 匹配 且 sid 匹配`：跨会话不继承（租期 30 min < 会话 12 h，正常场景不受影响；登出会释放）。

### 5.3 同一设备的不同 MQTT 连接怎么区分

| 维度 | 值 | 说明 |
|---|---|---|
| 租约 | `(username, lease_epoch)` | 一台设备同一时刻**最多一个**有效租约 |
| 页面实例 | `cid` | 仅观测/审计（`lease_cid` 记录"最后一次拿到租约的页面"） |
| 连接 | MQTT `clientId = gfweb_<12hex>` | 每次 `connect` 新建；用于 EMQX `DELETE /clients/{id}` 精确踢 |
| 心跳 | `POST /api/mqtt/heartbeat` | 带 `epoch` + 可选 `clientId`，服务端据此更新 `last_seen_at` 与 `lease_client` |

同一设备两个页面实例 = 同一租约 + 两个 MQTT 连接。这是**允许**的（EMQX 允许多客户端同 username；`clean:true` + 唯一 clientId ⇒ 互不踢），代价是"谁都不能单独把租约还掉" —— 见 §12。

---

## 6. 同设备租约复用与重复租约收敛

### 6.1 复用（每台设备的唯一快路径）

```js
// 伪代码，改写 mqtt-pool.js::allocate()
if (did) {
  const held = await db.prepare(
    "SELECT * FROM mqtt_browser_account WHERE lease_device=? AND state='leased'"
    + " AND lease_expires_at > ? ORDER BY last_allocated_at DESC").bind(did, now).first();
  if (held) return await renewAndReturn(held);   // 下面 §6.3
}
```

### 6.2 历史多租约收敛（"已占用 5 个账号"的存量清理）

每次 `allocate` 前执行一次**有界**收敛（最多扫 6 行，全表只有 5~20 行）：

1. 收集候选：`lease_device = did` **或**（`lease_device IS NULL` 且 `lease_sid LIKE '<sid>|%'`）。
   —— 后者专门覆盖 0013 之前的历史行（生产当前 5 条全是这种形态）。
2. 选**赢家**：`lease_expires_at` 最大者；并列取 `last_allocated_at` 最大者。
3. 先释放其余（逐个 CAS，避免索引冲突）：
   ```sql
   UPDATE mqtt_browser_account
      SET state='available', lease_sid=NULL, lease_device=NULL, lease_cid=NULL,
          lease_expires_at=0, lease_epoch=lease_epoch+1,
          password_updated_at=?,      -- 回拨到"已过期"，让 cron 尽快轮换（§9.3）
          updated_at=?
    WHERE username=? AND state='leased' AND lease_epoch=?
   ```
   并对该 username 执行 `kickPoolClients()`（尽力，失败只记流水，不阻塞）。
4. 再收编赢家：
   ```sql
   UPDATE mqtt_browser_account
      SET lease_device=?, lease_cid=?, lease_sid=?, last_seen_at=?, updated_at=?
    WHERE username=? AND state='leased' AND lease_epoch=?
   ```
5. 记 `journal(username,"cleanup_orphan",{released:n,kept:winner})`（**事件名已在白名单里**）。

> 幂等 + 有界：任何一步 `changes!==1` 就跳过该行继续；不重试、不回退。

### 6.3 续租节流（沿用既有结论，不放大 D1 写入）

保留 0010 现状的"剩余 > 50% 不写库"判据，**但新增 `last_seen_at` 单独节流窗口 240 s**：

```sql
UPDATE mqtt_browser_account
   SET lease_expires_at=?, last_allocated_at=?, last_seen_at=?, lease_cid=?, updated_at=?
 WHERE username=? AND state='leased' AND lease_epoch=? AND last_seen_at <= ?
```

即：同一个 4 分钟窗口内无论调用多少次，**只有第一次真的写行**（其余 `changes=0`，D1 计费按 0 行）。这满足"反复启动不产生写放大"。

---

## 7. 空闲分配与满池驱逐算法

### 7.1 分配优先级（严格按序，任何一步命中即返回）

```
① 归一 did（非法 ⇒ 走 legacy 分支，见 §11.1）
② 复用：lease_device = did 且 leased 且未过期           → renew（节流写）
③ 收敛：同 did / 同 sid 前缀的多余租约                  → 释放 + 踢，保留 1 把
④ 收编历史行：lease_device IS NULL 且 lease_sid LIKE sid|%  → adopt 后复用
⑤ 空闲：state='available'（或已过期 leased），按 last_allocated_at ASC
        CAS 抢占（同时写入 lease_device = did）
⑥ 满池驱逐：见 7.2
⑦ 仍失败 ⇒ 503 pool_exhausted（附 retry_after）
```

### 7.2 驱逐算法（一次 EMQX 调用定位活跃集，`O(池大小)` 内存判定）

```js
// 只调 1 次 EMQX：拉全部 connected 连接，本地按 username 聚合
const live = await queryClients(env, { connState:"connected", limit:500, timeoutMs:4000 });
const liveUsers = new Set(localFilter(live).map(c => c.username));   // 本地复核，不信任过滤

const EVICT_IDLE_S  = 900;   // 15 min 无心跳
const EVICT_GRACE_S = 180;   // 新租约保护期，防抖动/乒乓

const cands = await db.prepare(
  "SELECT username, lease_device, lease_epoch, last_seen_at, last_allocated_at"
+ " FROM mqtt_browser_account"
+ " WHERE state='leased' AND lease_device IS NOT NULL"
+ "   AND (lease_expires_at <= ? OR (last_seen_at <= ? AND last_allocated_at <= ?))"
+ " ORDER BY last_seen_at ASC LIMIT 3"
).bind(now, now - EVICT_IDLE_S, now - EVICT_GRACE_S).all();

for (const c of cands.results) {
  if (liveUsers.has(c.username)) continue;   // ★ 真有连接 ⇒ 判为活跃，跳过（不误驱逐）
  const done = await evictAndHandOver(env, c, { did, cid, sid });
  if (done.ok) return done.value;
  if (done.fatal) break;                     // 改密失败等 ⇒ 不再换候选，明确失败
}
return { ok:false, http:503, reason:"pool_exhausted", retry_after: 60 };
```

三条判据一起用，才能同时满足"不误驱逐活跃设备"和"能回收真闲置"：

1. **时间**：`last_seen_at` 最旧（不是 `created_at`、也不是 `last_allocated_at`）。
2. **在线**：EMQX `conn_state=connected` 里没有该 username（**否决权**，比时间更强）。
3. **保护**：`last_allocated_at` / `last_seen_at` 都在 180 s 之外的才可被驱逐（防"刚分配就被抢"的抖动）。

> 为什么不按 `lease_expires_at` 排：租期是**分配时**算的固定值，与"设备是否还在用"无关；用它排序等于按创建时间排，正是任务书禁止的做法。

### 7.3 "最久未活动"的定义与更新点（唯一口径）

`last_seen_at` = **该租约最后一次被证明"设备还在用它"的秒级时间戳**。更新点只有三处：

| 更新点 | 触发 | 节流 | 说明 |
|---|---|---|---|
| `allocate()` 复用/新分配 | 页面申请凭据 | 复用路径 240 s | 每次启动至少刷新一次 |
| `POST /api/ui/mqtt/heartbeat` | 前端 60 s 心跳（仅 MQTT `online` 时） | 240 s | **连接期唯一活性来源** |
| 收编/交接 | adopt / evict | — | 写入新归属时一并写 |

**不更新**的：`releaseLease`（释放即无租约）、`queryClients`（只读，不写库）。

可靠性说明（必须写进代码注释）：
- 心跳断 = 页面关了 / 断网 / 通道切 HTTP ⇒ 15 min 后判定闲置，合理。
- 心跳在但连接已断（`_connState!=='online'`）⇒ 前端**不心跳**，因此不会误报活跃。
- **最坏情况**：设备真在用，但心跳请求连续 15 min 全失败（弱网）⇒ 可能被判闲置。兜底是 EMQX 在线否决权（真有连接就不会被驱逐），因此该风险实际被消解。

---

## 8. 租约失效、凭据轮换与旧客户端隔离

### 8.1 为什么"只踢连接"不够

任务书已指出：多个客户端可共用同一用户名；踢掉连接后，旧客户端**仍持有缓存密码**，下次 `mqtt.connect()` 依然能认证成功。所以交接必须**同时**做两件事：

1. **改密**（`ensureUserPassword`，EMQX 侧旧密码立即失效）⇒ 旧客户端**再连不上**；
2. **踢连接**（`kickPoolClients`，按 username 枚举 + 本地复核 + 逐 clientId `DELETE`）⇒ 旧客户端**当前连接立即断**。

二者缺一：只改密 = 旧连接继续跑（EMQX 不重新鉴权已建立会话）；只踢 = 旧客户端 3 秒后连回来。

### 8.2 fencing token（`lease_epoch`）

- 每次归属变更 `lease_epoch += 1`。
- 客户端把 `epoch` 存进 `_cred`，在 **心跳 / 释放**时回传。
- 服务端所有"能改变租约的写"都带 `AND lease_epoch=?`；不匹配 ⇒ `409 lease_revoked`，且**不修改任何行**。
- 解决两类时序问题：① 旧页面的心跳不能给新设备的租约续命；② 旧页面的释放不能把新设备的账号还回池里。

`epoch` 不是密码学令牌（客户端可见/可伪造），它的作用是**让"上一代页面"的请求在服务端必然失效**，不是防越权（越权由 `sid` 兜）。

### 8.3 隔离强度矩阵（诚实标注）

| 动作 | 旧客户端立即 | 旧客户端重连 | 说明 |
|---|---|---|---|
| 只 release（归还） | 连接仍在 | **能**（密码没变，且账号可能再被分配出去） | 现有收敛路径，仅用于"同设备多租约"清理 |
| 只 kick | 断 | 能 | 不足 |
| **改密 + kick（驱逐交接采用）** | 断 | **不能**（CONNACK 4/5） | 目标态 |
| 再加 `epoch` 校验 | 心跳/释放被拒 ⇒ UI 转「已失效」 | 不能 | 至此闭环 |

---

## 9. Worker + D1 的原子性、并发与异常恢复

### 9.1 D1 能力边界（决定设计，不要假设）

- 无长事务、无 `SELECT … FOR UPDATE`；单条语句原子；`batch()` 是**整批原子**。
- 并发写由 D1 串行化，但**读-改-写之间仍会被插队** ⇒ 一切"抢占"必须写成 **条件 UPDATE + 判 `changes`**（项目既有铁律）。
- `rows_written` 按受影响行计费 ⇒ 条件不命中的 `UPDATE` 几乎零成本（这是心跳节流可行的依据）。

### 9.2 并发安全设计（四条，缺一不可）

| 场景 | 机制 |
|---|---|
| 同设备并发申请 | **部分唯一索引** `ux_mba_lease_device`（`WHERE lease_device IS NOT NULL AND state='leased'`）⇒ 第二个请求 INSERT/UPDATE 撞约束 ⇒ 捕获后**重查并复用赢家** |
| 两台设备抢同一个空闲号 | CAS `WHERE username=? AND (state='available' OR expired-leased)` + `changes===1`（既有，保留） |
| 两台设备抢同一个驱逐对象 | CAS `WHERE username=? AND state='leased' AND lease_epoch=?` + `changes===1` |
| 旧请求（epoch 过期）迟到 | 所有写带 `AND lease_epoch=?` ⇒ 命中 0 行 ⇒ 返回 `lease_revoked`，不改库 |

### 9.3 驱逐交接的失败分类（**不谎报成功**）

| 步 | 失败处理 | 用户可见结果 |
|---|---|---|
| ① CAS 抢占 | `changes=0` ⇒ 换下一个候选；候选耗尽 ⇒ `503 pool_exhausted` | 稍后重试 |
| ② 改密 | `state='failed'`（永不分配），`fail_reason='emqx_password_failed'` | `503 pool_evict_failed`，**旧设备不受影响** |
| ③ 重放 ACL | 同上（`emqx_acl_failed`） | 同上 |
| ④ 踢连接 | **不失败**：置 `kick_pending=1`，继续交接（新密码已生效），流水记 `rotate_kick_unverified` | 新设备正常；旧连接最多苟活到下次断开 |
| ⑤ 落密文 | 先写 D1 再回包；写失败 ⇒ `failed` + `503 pool_store_unavailable` | 明确存储故障（与池满区分） |
| ⑥ 回包 | 只在 ⑤ 成功之后 | — |

> `failed` 的恢复：把 `recoverStuckRotations()` 的 `rotating` 卡死阈值从 `2×周期(=48h)` 改成 **300 s**（正常轮换几秒完成，5 min 是安全上界），否则驱逐失败要等 48 h 才能自愈。`failed` 重试退避沿用 `max(1h, 周期/4)`。

### 9.4 异常恢复清单

1. **Worker 在改密后、落库前崩溃** ⇒ 行停在 `rotating`（密码在 EMQX 已换、D1 还是旧密文）⇒ 5 min 后 `recoverStuckRotations` 用 `rotateAccount()` 重新走一遍（幂等：`ensureUserPassword` 直接对齐新密码）。
2. **Worker 在踢连接后崩溃** ⇒ `kick_pending=1` ⇒ 下次 cron `retryPendingKicks(max=2)` 重试；同时任何 `allocate()` 顺带重试（有界 1 次）。
3. **D1 写入额度耗尽** ⇒ 沿用 `d1-guard.js` 的 `pool_store_unavailable` 分类（既有 P0-5 语义，**不得**与 `pool_exhausted` 混淆）。
4. **EMQX 管理 API 超时** ⇒ 单次 10 s 有界（既有 `EMQX_TIMEOUT_MS`）；驱逐路径把总预算压到 ≤12 s（改密 10 s 上限 + 踢 10 s 上限，但实测各 <1.5 s），超过即按上表失败，不回退到"假成功"。

---

## 10. EMQX 账号交接时序（四者状态）

```
时刻   新设备            Worker / D1                         EMQX                    旧设备
────────────────────────────────────────────────────────────────────────────────────────────
T0     请求凭据 ─────────►
T1                       did 无租约；无空闲；选 victim V
                         (LRU 且 EMQX 无连接)
T2                       CAS: leased→rotating, epoch+1
                         lease_device=NULL, lease_sid=NULL
                         ── 此后 V 的 heartbeat/release 全部命中 0 行
T3                       ──────────────────────────────►  PUT users/gf_web_NN
                                                           password = newPw
                                                          ★ 旧密码对新连接失效
T4                       ──────────────────────────────►  DELETE /clients/{cid} ×N
                                                          ★ 旧设备的在线连接被断
T5                       D1: 新密文 + state='leased'
                             + lease_device=did + epoch 保持
                             + lease_expires_at=now+ttl
T6     收到 {username,   ◄────
       newPw, epoch, exp}
       CONNACK ok
T7                                                                               收到 close
                                                                                前端**不自动重连**
T8                                                                               心跳 → 409 lease_revoked
                                                                                弹窗「MQTT 已失效」
                                                                                租约/凭据本地作废
```

**必须遵守的顺序约束**
- 改密（T3）**先于**踢连接（T4）：反过来会出现"旧连接被踢→旧客户端立刻用旧密码连回"的窗口。
- 落库（T5）**后于**改密：先落库再改密，若改密失败就会把"与 EMQX 不一致的密码"发给新设备（最坏的假成功）。
- 回包（T6）**最后**：新设备拿到密码时，EMQX 侧必须已经是这个密码。

---

## 11. 异常场景逐条处理（任务书 §五 全部 11 项）

| # | 场景 | 处理 | 可验证判据 |
|---|---|---|---|
| 1 | 同一 App 15 min 内启动 5 次 | 每次 `allocate(did)` 命中 ② 复用同一 `username`；`epoch` 不变或 +0；只按 240 s 节流写 1 次 | `SELECT COUNT(*) FROM mqtt_browser_account WHERE state='leased' AND lease_device=?` = 1；`allocate` 流水里 `reused:true` |
| 2 | 同设备已占 5 个账号（存量） | §6.2 收敛：保留 `lease_expires_at` 最大者，其余 `available` + 踢连接；`cleanup_orphan` 流水 n=4 | 收敛后该 did 名下 leased 恰好 1 条；池 `available` = 4 |
| 3 | 同设备两个 WebView/MQTT 客户端 | **共享同一租约**（同 username/password），各自独立 `clientId`；都不会单独释放；任一方心跳都能续 `last_seen_at`；`lease_cid` 只记录最后持有者 | 两个 clientId 同时在线；`lease_device` 唯一；关闭其中一个不影响另一个 |
| 4 | 两设备同时请求，池只剩 1 空闲 | CAS 只有一个 `changes===1`；输的那台转 ⑥ 驱逐（若无可驱逐 ⇒ 503） | 只有 1 台拿到账号；另 1 台 `pool_exhausted`（或成功驱逐） |
| 5 | 两设备同时请求，池已满 | 都进 ⑥；抢同一个 victim 时靠 `epoch` CAS 只有一个赢；输的重选候选（最多 3 个） | victim 只被交接一次；被驱逐方只有 1 台 |
| 6 | 被驱逐设备仍持旧用户名密码并尝试自动重连 | 密码已换 ⇒ CONNACK 4/5；前端 `_leaseRevoked` ⇒ **不再自动申请**；心跳 409 | 旧设备 `mqtt.connect` 失败；D1 不产生新 `allocate` 流水 |
| 7 | 被驱逐设备关弹窗，过一阵点设备页重连入口 | `requestPoolReconnect()` 清 `_leaseRevoked` → 走一次 `allocate`（可能再驱逐别人，这是用户显式动作，允许） | 点击后恰好 1 次 `allocate` 请求；之前 0 次 |
| 8 | 新设备已拿到账号，Worker 中途异常/超时 | 行停在 `rotating`；5 min 后 cron 重建；期间该账号**不可分配**（不会发出坏密码）；新设备本次收到 503/超时 ⇒ 前端提示重试 | `state='rotating'` 且 `rotation_started_at` 有界；`recover_done` 流水 |
| 9 | 同设备连续多个并发申请 | 唯一索引 + CAS ⇒ 只有 1 把租约；败者**重查 `lease_device=did`** 后直接复用并返回同一 username | 并发 N 次后 leased 数 = 1；返回的 username 全部相同 |
| 10 | 旧设备的延迟请求在账号已分配给新设备后才到 | 其 `epoch` 已过期 ⇒ 心跳/释放命中 0 行 ⇒ 409；**不修改**新租约 | 新租约行 `updated_at` 不被旧请求改动 |
| 11 | Worker/D1 与 EMQX 实际状态不一致 | ① 每次驱逐前用 EMQX 在线集做事实校正（不依赖 D1 的"以为"）；② `POST /api/admin/mqtt/pool/bootstrap {force:true}` 重铺密码；③ cron 每 5 min 级恢复 `rotating` | 事务性探针：`poolStatus()` 的 `by_state` 与 EMQX 在线 username 集合交叉核对 |

---

## 12. 前端：自动重连控制与手动重连 UI

### 12.1 状态与门禁（`gf-mqtt.js`）

新增模块态：

```js
let _leaseRevoked = false;         // ★ 一旦为真，禁止一切自动申请
let _hbTimer = null;
let _hbClientId = null;
```

三处**必须**加门禁（否则"自动循环申请"必然发生）：

1. `fetchCredentials(force)` 开头：`if (_leaseRevoked) throw new MqttChannelError("lease_revoked", "MQTT 账号已被回收，请手动重新申请");`
2. `ensureConnected(deviceId)` 开头：`if (_leaseRevoked) return { ok:false, reason:"lease_revoked" };`
3. `visibilitychange` 回前台 / 路由切换 / 命令下发 等所有间接调用点：由上面两处统一兜住（不要在各调用点分散判断）。

### 12.2 检测通道（两条）

| 通道 | 触发条件 | 处理 |
|---|---|---|
| 主动（主） | 心跳 `409 lease_revoked` | `onLeaseRevoked("heartbeat")` |
| 被动 | `client.on("error")` 或 `ready` reject 且 `code ∈ {4,5,135}` 且**非手动模式** | `onLeaseRevoked("connack")` —— **改掉现有的"静默 `invalidateCredentials()` 后下次再取"**（那正是循环申请的源头） |

```js
function onLeaseRevoked(source) {
  if (_leaseRevoked) return;
  _leaseRevoked = true;
  stopLeaseHeartbeat();
  _cred = null;                       // 本地作废，不发给任何后续请求
  disconnect("lease_revoked");
  setConnState("revoked", "账号已被回收");
  document.dispatchEvent(new CustomEvent("gf:lease-revoked", { detail:{ source }}));
}
```

### 12.3 手动重连（唯一出口）

```js
export function requestPoolReconnect() {
  _leaseRevoked = false;
  _cred = null;
  return ensureConnected(_deviceId);   // 真去申请（可能是"驱逐别人"，属用户显式动作）
}
export function leaseRevoked() { return _leaseRevoked; }
```

- 心跳：`_connState==="online" && !_leaseRevoked && !_isManual` 时每 **60 s** 一次；`disconnect()`/隐藏 60 s 断开时 `stopLeaseHeartbeat()`。

### 12.4 UI（`gf-app.js`）

| 位置 | 元素 | 行为 |
|---|---|---|
| 全局 | `openSheet()` 弹层「MQTT 连接已失效」 | 三行说明（账号已被其他设备回收 / 当前无法下发命令 / 可手动重新申请）；按钮 `[使用账号池连接(primary), 关闭]`；`closable:true` ⇒ 背景/Esc/关闭 皆可手动关闭；**同一时刻最多一个**（用 `_revokeSheet.isOpen()` 去重） |
| 弹层按钮 | 使用账号池连接 | `requestPoolReconnect()` → 成功则 `toast` + 关闭；失败按 `manualFailText()` 同款原因表提示（含 `pool_exhausted`） |
| 设备页「MQTT 手动连接（备用方式）」区 | **复用已有 `#man-pool`「使用账号池连接」按钮**（`gf-app.js:7647`），`leaseRevoked` 时文案改「重新申请账号并连接」 | 点击 → `requestPoolReconnect()` |
| 设备页「命令通道」区 | `renderChannel()` 的 chip 增加 `revoked` 态：「凭据已失效（需重新申请）」 | 只读展示 |
| 其它 | 命令失败且 `reason==="lease_revoked"` | `toast` 指向设备页入口；**不自动切 HTTP**（沿用"不静默切通道"铁律） |

> 复用的理由：`#man-pool` 已经存在、已经在设备页、已经承载"切回账号池"的语义 ⇒ 不新增第二套入口（避免"两套预检"那类重复实现）。弹窗只是**提醒 + 快捷方式**，关闭后入口仍在。

### 12.5 释放语义的调整（重要，且是"减行"改动）

**建议：不要启用 `installPoolReleaseHooks()`（保持死代码并删除该死 import），`pagehide` 只 `disconnect()` 不归还租约。**

理由：
1. 租约现在是**设备级**的，`pagehide` 触发时机是"页面"级；一旦归还，App 重启的复用就断了（回到 R1 的坑）。
2. 容量由"TTL + 驱逐 + 心跳闲置判定"接管，不再依赖"关闭即归还"。
3. 生产证据表明该路径从未生效（§1.2 B），启用它属于**扩大改动面**且与目标相反。

保留的显式释放（都是用户明确动作，且**设备作用域**）：
- 登出（`gf-app.js:3798` / `:11712`）：`releasePoolLease({silent:true})`；
- 切到 HTTP 代理（`setCmdPath("http")`）。

释放请求体升级为 `{ username, cid, did, epoch }`；Worker 校验 `epoch` 后才释放（防旧页面把新设备的号还掉）。若客户端没带 `epoch`（老页面）⇒ 退化为 `sid + username` 作用域（仍不会跨 sid）。

---

## 13. WebView / 多页面 / 多客户端行为

| 情形 | 行为 | 依据 |
|---|---|---|
| App 冷启动（新 WebView，localStorage 保留） | `did` 不变 ⇒ 复用同一账号 | §5.1；Android 侧无清理调用 |
| App 被杀后重启 | 同上（`sessionStorage` 丢失只影响 `cid`，不影响 `did`） | `cid` 仅观测用 |
| 页面刷新（同 WebView） | `did`/`cid` 都不变 ⇒ 复用；`_cred` 内存丢失 ⇒ 重新 `GET credentials`（命中复用，只可能节流写一次） | §6.1 |
| 同设备两个 WebView（分屏/多窗口） | 共享一个租约、两个 MQTT 连接；`lease_device` 唯一约束保证不会各拿一把 | §9.2 |
| 用户清 App 数据 | 新 `did` ⇒ 视为新设备；旧租约 15 min 后进入可驱逐，30 min 后 TTL 归还 | 可接受 |
| 会话过期（12h） | `sid` 变 ⇒ 登录后重新分配（旧租约那时早过期） | 租期 30 min ≪ 12 h |
| 桌面浏览器多标签 | 同 `did` ⇒ **共享一个账号**（原来是各占一个）。这是容量改善，不是缺陷 | 本计划的有意取舍 |

**需要在真机验证的唯一未知**：Android WebView 是否在 Activity 销毁时触发 `pagehide`。本设计**不依赖**它（心跳/ TTL / 驱逐都不依赖页面事件），因此不影响正确性。

---

## 14. 改动清单（文件级，供直接实施）

### 14.1 D1：新迁移 `guo-feeder-api/migrations/0013_mqtt_pool_device_lease.sql`

```sql
-- 只做加法（不 DROP、不改既有列、不动 0000~0012）
ALTER TABLE mqtt_browser_account ADD COLUMN lease_device  TEXT;                       -- did
ALTER TABLE mqtt_browser_account ADD COLUMN lease_cid     TEXT;                       -- 页面实例（观测）
ALTER TABLE mqtt_browser_account ADD COLUMN lease_epoch   INTEGER NOT NULL DEFAULT 0;  -- fencing
ALTER TABLE mqtt_browser_account ADD COLUMN last_seen_at  INTEGER NOT NULL DEFAULT 0;  -- 最后活动
ALTER TABLE mqtt_browser_account ADD COLUMN lease_client  TEXT;                       -- 最近上报的 MQTT clientId
ALTER TABLE mqtt_browser_account ADD COLUMN kick_pending  INTEGER NOT NULL DEFAULT 0;  -- 踢连接待重试

-- ★ 结构性保证：一台设备最多一把 leased 租约（rotating 期间必须为 NULL）
CREATE UNIQUE INDEX IF NOT EXISTS ux_mba_lease_device
    ON mqtt_browser_account(lease_device)
    WHERE lease_device IS NOT NULL AND state = 'leased';

-- 驱逐候选排序（last_seen_at 最旧优先）
CREATE INDEX IF NOT EXISTS ix_mba_last_seen
    ON mqtt_browser_account(state, last_seen_at);

-- 一次性回填：历史 leased 行用 last_allocated_at 近似"最后活动"
UPDATE mqtt_browser_account SET last_seen_at = last_allocated_at WHERE last_seen_at = 0;
```

> 不新增 `CHECK`（枚举演进风险，与 0008/0010 同律）；`did` 格式校验放在应用层 `normDeviceId()`。

### 14.2 Worker `guo-feeder-api/src/mqtt-pool.js`

| 新增/修改 | 签名 | 说明 |
|---|---|---|
| 新增 | `normDeviceId(v)` | `/^[A-Za-z0-9_-]{16,40}$/`，非法返回 `""` |
| 修改 | `allocate(env, {sid, cid, did})` | 按 §7.1 重排；`did` 空 ⇒ 完全保持旧行为（兼容滚动发布） |
| 新增 | `findDeviceLease(env, did, now)` | 复用快路径 |
| 新增 | `convergeDeviceLeases(env, {did, sid, cid})` | §6.2 |
| 新增 | `touchLease(env, {username, epoch, did, clientId, throttleS=240})` | 心跳落 `last_seen_at`（条件写） |
| 新增 | `evictLeastIdleAndHandOver(env, {did, cid, sid, key})` | §7.2 + §10 |
| 修改 | `releaseLease(env, {sid, cid, did, epoch, username, scope})` | `scope="device"` 默认；`epoch` 不匹配 ⇒ `released:0, revoked:true` |
| 修改 | `recoverStuckRotations()` | `rotating` 卡死阈值 `2×周期` → `300 s` |
| 新增 | `retryPendingKicks(env, {max:2})` | cron 用；`kick_pending=1` 的账号重踢 |
| 修改 | `poolStatus()` | 增列 `lease_device`（截断前 8 位）、`last_seen_at`、`lease_epoch`、`kick_pending` |
| 修改 | `ROTATION_EVENTS` | 增 `evict`、`evict_done`、`evict_failed`、`heartbeat_revoke`、`adopt_legacy` |

### 14.3 Worker `guo-feeder-api/src/index.js`

| 位置 | 改动 |
|---|---|
| L322 鉴权枚举 | **改成族前缀** `path.startsWith("/api/ui/mqtt/")`（当前是逐条枚举，本文件注释里已经踩过一次同类事故；新增端点漏登记会掉进 ingest 兜底 200） |
| credentials 端点 | 读 `x-ui-device`；透传 `{sid,cid,did}`；响应增 `lease_epoch`、`device_bound:true`、`lease_device_hint` |
| 新增 `POST /api/ui/mqtt/heartbeat` | body `{epoch, clientId?}`；`touchLease`；409 `{error:"lease_revoked"}`（epoch 不匹配 / 行已非该设备 / `rotating`） |
| release 端点 | 接受 `did`/`epoch`/`scope`；保留"sid 必填"的 fail-closed |
| `POST /api/admin/mqtt/pool/evict`（可选） | 运维手工驱逐指定 did（幂等、写审计），用于线上应急 |

### 14.4 Pages `guo-feeder-pagesdev`

| 文件 | 改动 |
|---|---|
| `functions/api/mqtt/credentials.js` | 读 `x-gf-device`（→ `?did=`），`clientInstanceId` 同款校验函数复用（新增 `clientDeviceId`）；畸形 ⇒ 400（**绝不静默丢弃**）；转发 `x-ui-device` |
| `functions/api/mqtt/heartbeat.js`（新） | `readSessionFull` → 限流（60/min/IP）→ 转发 `POST /api/ui/mqtt/heartbeat`；透传 409 |
| `functions/api/mqtt/release.js` | body 里接受 `did`/`epoch`（`sendBeacon` 不能设头），与 `cid` 同样校验 |
| `assets/js/gf-mqtt.js` | §5.1 / §12.1 / §12.2 / §12.3；`fetchCredentials` 带 `x-gf-device`；`_cred.epoch`；心跳；`_leaseRevoked` 门禁；CONNACK 4/5/135 转 revoked；`releasePoolLease` 体加 `did`/`epoch`；`installPoolReleaseHooks` 保持不挂（并删除 `gf-app.js:40` 的死 import） |
| `assets/js/gf-app.js` | §12.4：弹层、`#man-pool` 文案与行为、`renderChannel` 增 `revoked` 态、`onConnState` 重渲染 |

### 14.5 不需要改的（明确边界）

- `mqtt_browser_account` 的既有列语义、池命名 `gf_web_NN`、容量默认 5、租期 30 min、轮换 24 h。
- 设备账号体系（`dev_*` / `YOUR_MQTT_USERNAME`）、ACL 规则（`browserAclRules()`）、EMQX 生产配置、`wrangler.toml` 触发器。
- Envelope、遥测、命令帧、米家、Turnstile、`Android_Assets` 源码。

---

## 15. 测试方案与验收标准

### 15.1 Worker（新增 `guo-feeder-api/test/current/p0_pool_device_lease.test.mjs`，**必须登记进 `test/run_all.mjs`**）

用 `test/lib/d1_write_probe.mjs`（**真 SQLite + 真 migration 0000→0013**），EMQX 侧 stub 到 `emqx-admin.js` 的导出。

| 组 | 用例 | 断言 |
|---|---|---|
| A 复用 | A1 同 `did` + 5 个不同 `cid` 连续 allocate | 5 次返回同一 `username`；`leased` 行数=1；`allocate` 写入次数 ≤2（节流） |
| | A2 `did` 为空（老客户端） | 行为与改前完全一致（回归） |
| B 收敛 | B1 预置 5 行 `lease_sid='<sid>\|tN'` 同 sid | 收敛后 1 行 leased、4 行 available；有 `cleanup_orphan` 流水；被释放的 username 触发踢连接 |
| | B2 收敛时某行 `changes=0`（模拟并发） | 该行跳过，其余继续；不抛错 |
| C 驱逐 | C1 池满、victim `last_seen_at` 最旧 | victim 换成新 `did`，`epoch+1`，密文变化（解密后 ≠ 旧值），旧 `did` 名下 0 行 leased |
| | C2 victim 在 EMQX 有连接 | **不被驱逐**，转下一个候选；全活跃 ⇒ `pool_exhausted` |
| | C3 victim 刚分配（`last_allocated_at > now-180`） | 不可被驱逐 |
| | C4 改密失败（stub 返回 500） | `state='failed'`，返回 `pool_evict_failed`，**不**回包假密码 |
| | C5 踢连接失败 | `kick_pending=1`，仍完成交接，流水 `rotate_kick_unverified` |
| D 并发 | D1 `Promise.all` 同 `did` 两请求 | 最终 leased=1；两个响应 username 相同（或一个 409 后重查复用） |
| | D2 两 `did` 抢最后 1 个空闲 | 恰好 1 个 ok；另一个走驱逐或 503 |
| | D3 两 `did` 抢同一 victim | victim 只交接一次（`epoch` +1 仅一次） |
| E 失效 | E1 旧 `epoch` 心跳 | 409，且目标行 `updated_at`/`last_seen_at` 不变 |
| | E2 旧 `epoch` 释放 | `released:0, revoked:true`；行不变 |
| F 恢复 | F1 `rotating` 卡 6 min | `recoverStuckRotations` 能恢复（阈值 300 s 生效） |
| | F2 `kick_pending=1` | `retryPendingKicks` 重踢并清标记 |
| G 迁移 | G1 空库跑 0000→0013 | 列齐、`ux_mba_lease_device` 生效（插两行同 did leased ⇒ 第二行报约束） |

### 15.2 Pages（新增 `tests/ui/pool_device_lease.test.mjs`，自动被 `tests/run_all.mjs` 扫描）

| 用例 | 断言 |
|---|---|
| `installId()` 稳定 | 连续两次调用同值；`localStorage.clear()` 后不同；格式匹配 |
| 凭据请求带 `x-gf-device` | fetch stub 捕获头部 |
| 409 ⇒ `_leaseRevoked` | `leaseRevoked()===true`；之后 `ensureConnected()` **不产生 fetch**（负向探针：计数器=0） |
| `requestPoolReconnect()` | 清标记并恰好 1 次 fetch |
| CONNACK 5 | 触发 `gf:lease-revoked` 事件、`connState().state==="revoked"`；不再 `invalidateCredentials` 后静默重取 |
| 心跳生命周期 | online 后启动、`disconnect()` 后停止、`_leaseRevoked` 后不发 |
| 弹层去重 | 连续两次事件只弹一个 sheet；关闭后 `leaseRevoked()` 仍为 true |

### 15.3 需要**改写**的既有套件（不是调绿）

- `p0_pool_multitab.test.mjs`：原断言"每 cid 一把租约"，与新契约冲突 ⇒ 改为"同 did 共享一把 + `lease_cid` 记录最后持有者 + 释放按设备作用域"。
- `p0_pool_lifecycle.test.mjs`：`allocate` 增加 `did` 参数（缺省兼容），断言池满从"503"变为"驱逐或 503"。
- `p0_ghost_lease.test.mjs`：保持不变（仍应全绿，过期可复用语义未动）。

### 15.4 验收标准（对应任务书 §六）

| # | 标准 | 判定方法 |
|---|---|---|
| 1 | 同一设备连续启动 5 次不占 5 个账号 | 生产：`SELECT COUNT(*) … WHERE state='leased' AND lease_device=?` = 1；`allocate` 流水 `reused` 计数 ≥4 |
| 2 | 重复申请优先复用原账号 | 5 次返回的 `username` 完全相同 |
| 3 | 同设备重复租约安全收敛 | 存量场景 B1 + 生产 `cleanup_orphan` 流水 |
| 4 | 创建早但活跃的账号不被误驱逐 | C2（EMQX 在线否决）+ C3（保护期） |
| 5 | 满池驱逐最久未活动 | C1：被驱逐者 = `min(last_seen_at)` 且非在线 |
| 6 | 被驱逐设备不能继续用旧凭据 | 生产手工验证：旧页 `mqtt.connect` 返回 CONNACK 4/5 |
| 7 | 被驱逐设备不会自动反复申请 | 24h 内 `allocate` 流水不出现同一 did 的高频重复 |
| 8 | 弹窗/设备页可手动重连 | 真机 UI 走一遍 |
| 9 | 并发不重复分配 | D1/D2/D3 |
| 10 | 不依赖常驻进程 | 所有逻辑在请求内或既有 cron；`grep` 无 setInterval 常驻 |
| 11 | 设备 MQTT / 浏览器 MQTT / HTTP 备用通道不受影响 | 既有关键套件 + 生产冒烟（`scripts/verify_p4_readonly.mjs`） |

---

## 16. 部署顺序、回滚与风险

### 16.1 顺序（严格，且每一步都可独立回滚）

1. **D1 迁移 0013**（纯加法）。旧 Worker 忽略新列 ⇒ 无影响。
   `npx wrangler d1 migrations apply YOUR_D1_DATABASE_NAME --remote`
2. **部署 Worker**（`allocate` 支持 `did` 但**必须兼容无 did**；`heartbeat` 端点先上线但前端还没调）。
3. **部署 Pages**（前端开始发 `x-gf-device`；新弹窗生效）。
4. **观察 1 小时**：`allocate` 流水应大幅减少（复用命中）；`cleanup_orphan` 应只出现一次（存量 5→1 收敛）。
5. **可选**：等池恢复后手工验证一次"被驱逐"路径（两个设备 + 临时把 `MQTT_POOL_SIZE` 调小不推荐；改用 admin `evict` 端点）。

> 兼容性是硬要求：**新 Worker 必须能被旧 Pages 完整使用**（旧 Pages 不发 `x-ui-device` ⇒ `did=""` ⇒ 走 legacy 分支）。否则滚动部署期间会 503。

### 16.2 回滚

| 层 | 回滚动作 | 数据影响 |
|---|---|---|
| Pages | 重新部署上一版（`git revert` 或 `wrangler pages deploy` 旧 commit） | 无 |
| Worker | 同上 | 无（新列被忽略） |
| D1 | **不回滚**（加法迁移，留列无害）；必要时把 `lease_device/lease_cid/epoch` 置 NULL/0 | 无 |

### 16.3 风险表（区分"设计内"与"需验证"）

| 风险 | 类型 | 缓解 |
|---|---|---|
| Android WebView 的 localStorage 是否真的跨重启保留 | **需真机验证** | 已有代码事实支持（无清理）；若失效 ⇒ 退化为旧行为，不会更糟 |
| 心跳带来 D1 写放大 | 设计内 | 240 s 节流 ⇒ ≤15 行写/小时/设备（5 设备 ≈ 75/h，远低于日额度） |
| 驱逐路径耗时 3–10 s 可能超过前端 15 s 连接超时 | 设计内 | 预算 ≤12 s；前端 `connectTimeout` 保持 10 s 但页面显示"正在回收账号…"；超时后用户重试即命中复用 |
| 15 min 闲置阈值对"弱网但真在用"的设备误判 | 设计内（已消解） | EMQX 在线否决权；心跳失败 ≠ 无连接 |
| `conn_state=connected&limit=500` 在 Serverless 上的准确性 | **需实测**（已有 `presence-emqx.js` 同类结论支持可用） | 本地复核 + 失败即判"未知"⇒ 不驱逐（保守） |
| 同设备两页共享一个租约后，登出会断另一个页 | 设计内（可接受） | 登出本来就是"整设备退出" |
| 存量 5 条租约收敛时把 4 个账号的密码留到下次轮换（≤5 h） | 设计内（记录） | 被释放账号回池后立即可能被别的设备用旧密码复用，而旧页面早已失效；靠 `epoch` + TTL 兜底。若要更强：收敛时同步 `rotateAccount`（代价 4×10 s，不推荐放请求路径） |

---

## 17. 需要实施阶段验证的清单（诚实标注"未证实"）

1. `localStorage` 在三星真机 WebView 上的跨进程持久性（清后台/重启后 `gf.install_id` 是否一致）。
2. EMQX Serverless 的 `conn_state=connected&limit=500` 是否能一次覆盖全部池账号连接（若不能，按 `username` 逐个查，最多 5 次）。
3. 驱逐交接在真实 D1 + 真实 EMQX 上的端到端耗时（目标 P95 < 5 s）。
4. 改密后旧连接的"僵尸窗口"实际时长（`keepalive=30` ⇒ 预期 <45 s）。
5. Android WebView 是否触发 `pagehide`（本设计不依赖，仅记录）。

---

## 18. 一句话交接

> 把"`<sid>|<cid>` 精确匹配"换成"**`did` 设备身份 + 唯一索引 + epoch 栅栏**"，把"池满就 503"换成"**驱逐最久未活动且 EMQX 确认无连接者，改密+踢连接后交接**"，前端把"静默作废再取"换成"**置 revoked + 弹窗 + 手动重连**"——三处改完，任务书 §6 的 11 条验收即可全部量化验证。
