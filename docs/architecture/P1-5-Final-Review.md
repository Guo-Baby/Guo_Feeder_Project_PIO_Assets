# P1-5 Final Review（设备绑定收口评审）

> **状态**：**实施完成（2026-10-04）** —— P1-5-1 Binding Core · P1-5-2 Admin API · P1-5-3 Revoke Binding Closure · P1-5-4 文档同步 全部完成并自测通过；**P1-5-5 正式验收（真实 D1 + 部署版 Worker）待做**。
> **上位文档**：`P1-5-Device-Binding-Design.md`（P1-5-A 设计稿）· `P1-5E-Implementation-Plan.md`（P1-5-E 实施计划）· 裁决 **Q14–Q20**（2026-10-04）。
> **本文定位**：**设计稿 ↔ 实现之间的审计桥接**。
> 设计稿（P1-5-A）与实施计划（P1-5-E）**保持原样不改**（史料留存），所有"设计怎么写 / 代码实际怎么做"的差异集中登记在本文件。
> **代码真源**：`Cloudflare_Assets/guo-feeder-api/src/` 下的 `binding.js` · `device-registry.js` · `credential.js` · `index.js`。**本文与代码冲突时，以代码为准。**
> **撰写日期**：2026-10-04

---

## 0. 一句话结论

`device_binding`（`0001` 建好但 Worker 零引用）已成为**唯一 owner 的可审计归属链**：管理端 bind / unbind / transfer / 查询全链路可用，`REVOKED` 必关 active binding（**INV-4**）已在 `revokeDevice()` 内落地，且**未新增迁移、未碰凭据状态机、未碰固件**。

---

## 1. 范围与交付物

### 1.1 交付物

**云端 `Cloudflare_Assets`（`guo-feeder-api/src/`）**

| 文件 | 变更 | 规模 | 阶段 |
|---|---|---|---|
| `binding.js` | **新建** —— 业务编排 + 参数校验 + D1 事务组合 | ~522 行 | P1-5-1 |
| `device-registry.js` | **只增** —— `eventStatement` / `isValidUserId` / `getActiveBinding` / `listBindingHistory` / `listBindingsByUser` + 2 个 binding 语句工厂 | +196 / −7 | P1-5-1 · P1-5-3 |
| `index.js` | **只增** —— 5 条路由 + import + 文件头路由注释 | +136 / −1 | P1-5-2 |
| `credential.js` | **仅 `revokeDevice()`** —— D1 写入 batch 化 + INV-4 关 binding + `binding_closed` | +77 / −13 | P1-5-3 |

**文档（固件仓 `PIO_Assets/Guo_Feeder_Project`）**

| 文件 | 变更 |
|---|---|
| `docs/architecture/P1-5-Final-Review.md` | **本文件（新建）** |
| `docs/interfaces/cloud_protocol.md` | 新增 **§11 Control Plane API**（管理端 HTTP 契约） |
| `docs/architecture/P1-5-Device-Binding-Design.md` | 顶部加**最终实现指针**（正文不改） |
| `docs/README.md` | 索引补本文件 |
| `docs/architecture/P1-实现清单.md` | P1-5 状态 → 已完成 |
| `HANDOFF.md` | 当前状态 → P1-5 completed |

**文档（云端仓 `Cloudflare_Assets`）**：`README.md`（路由 / 模块 / 不变量 / 待办）

### 1.2 冻结边界遵守情况（逐条核对）

| 冻结项 | 结果 |
|---|---|
| **不新增迁移** | ✅ `migrations/0001_*` 未改、未新增（`device_binding` / `device_event` 均已存在于 `0001`） |
| `device_credential` 零触碰 | ✅ `binding.js` 全文无 `device_credential` / 无 credential 调用（负向 grep 0 命中） |
| `revokeDevice` 不重构 | ✅ 仅替换 D1 写入段为单 `batch`，EMQX 段（ACL 收紧 / 踢会话）与 credential 状态机**逐字未改** |
| 固件 `src/` | ✅ 零改动（P1-5 全阶段无固件改动） |
| `admin-auth.js` / `emqx-admin.js` / `wrangler.toml` | ✅ 哈希全程未变 |
| 不引入用户系统 / RBAC / 共享 owner | ✅ 单 owner，`user_id` 为占位身份 |

---

## 2. 设计与实现差异（Δ-A … Δ-K）

> 左列为**设计稿 / 实施计划原值**（现已废弃或修正），右列为**代码实际行为**（唯一真源）。
> 后续 APP / 运维对接**只看右列**。

| # | 项 | 设计 / 计划预设 | **实际实现（代码真源）** |
|---|---|---|---|
| **Δ-A** | 成功响应体 | `{ok:true, action:"bind", owner, state:"BOUND", binding_id}` | `{http, device_id, user_id, bound:true, idempotent:false}` —— **无** `ok` / `action` / `state` / `binding_id` |
| **Δ-B** | transfer CAS 失败错误码 | `owner_mismatch` | **`binding_conflict`**（`owner_mismatch` **不存在**）；bind 的"已有其他 owner" = `already_bound` |
| **Δ-C** | `hint` 字段 | 多数错误带 `hint` | **一律不带**（唯二例外：`index.js` 前置 `device_id` 校验、`reason` 超限 `invalid_reason`） |
| **Δ-D** | `reason` 上限 | ≤64 + 白名单建议 | **≤200**（在 `index.js` 路由层收口），**无白名单**，超限 `400 invalid_reason`（不截断） |
| **Δ-E** | `GET …/binding` 返回 | `{owner, state, binding, history}` | `{http, device_id, current_owner, user_id, history}` —— **`state` 已移除**（P1-5-1 Review M-1：`device.state` 与 `device_binding` 是两个状态机，暴露会诱导反推 owner） |
| **Δ-F** | `GET /user/:id/devices` | 含 device 详情 + limit 默认 50 / 上限 200 | **只返 binding 投影**；limit **默认 200 / 钳位 1..500**（由 `binding.js` 负责） |
| **Δ-G** | `GET /device/:id` 扩展 | 追加 `binding` + `binding_history` | **未扩展**（决策 Q-A：保持 device 端点职责单一，binding 走独立端点） |
| **Δ-H** | revoke 语句顺序 | close → credential → device → **unbind** → retire | **方案 B**：close → **unbind** → credential → device → retire |
| **Δ-I** | revoke 返回 | （计划未写） | **新增 `binding_closed: true\|false`** |
| **Δ-J** | unbind 事件入批 | （计划未写） | **仅"读到 active"时才入批** —— 修正"同秒内重复 revoke 误写第二条 `unbind` 审计"的真实缺陷 |
| **Δ-K** | `not_bound` | 422 + hint | 422，**无 hint** |

**其他实现细节（非契约层，仅供排查）**：实施计划里设想的 `runBindingTxn()` / `classifyD1Error()` 未落地；实际 `binding.js` 采用 **guarded statement（SQL 守卫）** 方案 —— 用 `WHERE <guard>` 让冲突路径整批 0 效果，配合 `precheckDevice()` / `changesOf()` / `guardedEventStatement()` 三个私有 helper 完成判定。功能等价（"检测到冲突"与"零写入"同时成立），且**无 check-then-act**。

---

## 3. 最终 API 契约（以代码为唯一真源）

### 3.1 通用约定

| 项 | 值 |
|---|---|
| 前缀 / 鉴权 | `/api/admin/**` + `Authorization: Bearer <ADMIN_API_TOKEN>`（`authorizeAdmin`，常量时间比较；缺失 / 错误 / 未配置 ⇒ **同一 401 响应体**） |
| 方法 | 变更一律 `POST`；查询 `GET` |
| 响应 | `jsonResponse(obj, status)`；服务层返回 `{http, ...}` ⇒ `jsonResponse(out, out.http \|\| 200)` |
| `user_id` 规则 | `^[A-Za-z0-9_.:@-]{1,64}$`，区分大小写；**唯一校验点 = `binding.js::isValidUserId`** |
| `device_id` 规则 | `^[0-9a-f]{12}$`（`index.js` 前置校验，返回 `{error, hint}`，**无 `http` 字段**、沿用既有行为） |
| `reason` 规则 | 可选；`typeof string` 且长度 ≤200（`index.js` 层）；超限 `400 invalid_reason`（**不截断**）；缺省传 `null` |
| 幂等 | **语义幂等**（同 owner 重复 bind / 无 active 重复 unbind ⇒ `200 idempotent:true` 且**零写入**）；**不引入** `Idempotency-Key` |
| 错误体 | `{"error":"<snake_case>", …}`；统一由 `binding.js` 构造 |

### 3.2 端点契约

#### ① `POST /api/admin/device/:id/bind`

- **请求**：`{ "user_id": "<必填>", "reason": "<可选>" }`
- **状态门槛**：`device.state ∈ {REGISTERED, BOUND, UNBOUND}` 才可进入；`FACTORY` / `CLAIM_PENDING` ⇒ `device_not_registered`；`REVOKED` ⇒ `device_revoked`
- **幂等**：已是同一 owner ⇒ `200 idempotent:true`，**零写入零事件**

| 情形 | HTTP | body |
|---|---|---|
| 首次绑定 | 200 | `{http, device_id, user_id, bound:true, idempotent:false}` |
| 同一 owner 重复 | 200 | `{http, device_id, user_id, idempotent:true}` |
| 已有**其他** owner | 422 | `{error:"already_bound", device_id, owner}` |
| `device_id` 非法 | 400 | `{error:"invalid_device_id"}`（index.js 前置为 `{error, hint}`） |
| `user_id` 非法 / 缺失 | 400 | `{error:"invalid_user_id"}` |
| 设备不存在 | 404 | `{error:"device_not_found"}` |
| 未注册（`FACTORY`/`CLAIM_PENDING`） | 422 | `{error:"device_not_registered"}` |
| 已退役 | 422 | `{error:"device_revoked"}` |
| 事务失败 | 500 | `{error:"batch_failed", device_id}` |

#### ② `POST /api/admin/device/:id/unbind`

- **请求**：`{ "reason": "<可选>" }`
- **`forced` 规则（Q15）**：**服务端固定 `true`，禁止客户端控制** —— `index.js` 不读取 `body.forced`；`binding.js` 调用签名默认 `forced = true`。客户端即便传 `forced:false` 也被忽略，事件 `payload.forced` 恒为 `true`
- **副作用**：**不吊销凭据**（归属 ≠ MQTT 身份）；关闭 binding + `device.state→UNBOUND` + `unbind` 事件**同批**
- **幂等**：无 active binding ⇒ `200 idempotent:true`，**零写入零事件**

| 情形 | HTTP | body |
|---|---|---|
| 正常解绑 | 200 | `{http, device_id, user_id:"<prev>", unbound:true, idempotent:false}` |
| 无 active binding | 200 | `{http, device_id, user_id:null, idempotent:true}` |
| `device_id` 非法 | 400 | `{error:"invalid_device_id"}` |
| 设备不存在 | 404 | `{error:"device_not_found"}` |
| 未注册 | 422 | `{error:"device_not_registered"}` |
| 已退役 | 422 | `{error:"device_revoked"}`（**禁止用普通 unbind 修复 REVOKED 绑定** —— 唯一路径是 `revokeDevice()`） |
| 事务失败 | 500 | `{error:"batch_failed", device_id}` |

#### ③ `POST /api/admin/device/:id/transfer`

- **请求**：`{ "to_user_id": "<必填>", "from_user_id": "<可选 CAS>", "reason": "<可选>" }`
- **CAS 语义**：`from_user_id` 提供且与当前 owner 不符 ⇒ `422 binding_conflict` 且**零写入**；未提供 ⇒ 直接转让当前 owner
- **不降级**：无 active binding ⇒ `422 not_bound`（**不会**退化成 bind）

| 情形 | HTTP | body |
|---|---|---|
| 成功 | 200 | `{http, device_id, from, to, transferred:true, idempotent:false}` |
| `to_user_id` / `from_user_id` 非法 | 400 | `{error:"invalid_user_id"}` |
| 设备不存在 | 404 | `{error:"device_not_found"}` |
| 未注册 | 422 | `{error:"device_not_registered"}` |
| 已退役 | 422 | `{error:"device_revoked"}` |
| 无 active binding | 422 | `{error:"not_bound", device_id}` |
| **CAS 不符** | 422 | `{error:"binding_conflict", device_id, owner}` |
| 事务失败 | 500 | `{error:"batch_failed", device_id}` |

#### ④ `GET /api/admin/device/:id/binding`

- **请求**：无 body；`history` 深度由内部 `historyLimit`（默认 50）决定
- **返回**：`{http, device_id, current_owner, user_id, history}`
  - `current_owner` = active binding 行 **或 `null`**（**owner 唯一来源**）
  - `user_id` = 便捷字段（等价于 `current_owner.user_id`）
  - `history` = **含已关闭的历史绑定**（追加式，不删行）
- **不含 `device_state`**（Δ-E）；投影仅 `id / device_id / user_id / bound_at / unbound_at / bound_by / reason / created_at`，**不含任何凭据字段**

| 情形 | HTTP | body |
|---|---|---|
| 有 / 无 owner | 200 | 上述结构（无 owner 时 `current_owner:null`、`user_id:null`、`history:[]`） |
| `device_id` 非法 | 400 | `{error:"invalid_device_id"}` |
| 设备不存在 | 404 | `{error:"device_not_found"}` |

#### ⑤ `GET /api/admin/user/:userId/devices`

- **请求**：路径 `userId` 需 `decodeURIComponent`（允许字符含 `@ : . - _`）；query `limit`（可选，默认 200，内部钳位 1..500）
- **返回**：`{http, user_id, devices:[…], count}` —— `devices` 为 **active binding 投影**（1:N，P1 不设上限）

| 情形 | HTTP | body |
|---|---|---|
| 正常 | 200 | 上述结构（`count` = active 绑定数） |
| `userId` 非法（空格 / 超长 / 空） | 400 | `{error:"invalid_user_id"}` |
| 路径形状不符（`/user/:id`、`/user/:id/other`） | 404 | `{error:"not_found"}` |
| 非 GET 方法 | 405 | `{error:"method_not_allowed"}` |

#### ⑥ `POST /api/admin/device/:id/revoke`（P1-5-3 增强）

- **请求**：`{ "reason": "<可选，默认 'retire'>" }`
- **返回**：`{http:200, device_id, revoked:true, acl_tightened:true, kicked, binding_closed:<bool>, detail}` —— **新增 `binding_closed`**（Δ-I）；既有字段与错误码**不变**
- **INV-4 落地**：D1 写段单 `batch`，顺序 close-binding → unbind 事件 → credential → device → retire；无 active binding 时 `binding_closed:false` 且**不写** `unbind` 事件

---

## 4. 不变量落地（INV-1 … INV-7）

| # | 不变量 | 落地手段 | 状态 |
|---|---|---|---|
| **INV-1** | owner 唯一真源 = `device_binding WHERE unbound_at IS NULL`；`device.state` 只是摘要 | 所有写路径同批更新两者；`getBinding()` **不返回** `device.state` | ✅ |
| **INV-2** | 状态一致性（有 active ⇔ BOUND 等） | 同批 + `WHERE state IN (...)` 守卫 | ✅ |
| **INV-3** | `REVOKED` 是终态，bind/unbind/transfer 一律拒绝 | `precheckDevice()` 前置 `422 device_revoked` | ✅ |
| **INV-4** | **退役必须关闭 active binding** | `credential.js::revokeDevice()` 同批：`UPDATE device_binding SET unbound_at=…` + `INSERT device_event('unbind')` | ✅ **本阶段新落地** |
| **INV-5** | 绑定流程不改 `device_credential`；凭据流程不改 `device_binding`（除 INV-4 配对） | 负向 grep 0 命中 + `revokeDevice` 是唯一跨写点 | ✅ |
| **INV-6** | 事件 payload 永不含密码 / 凭据 / token | `binding.js` payload 仅 `{user_id \| from/to, reason, forced, source}` | ✅ |
| **INV-7** | 绑定不要求设备在线 | 无 MQTT 交互（控制面 / 数据面分离） | ✅ |

**单 owner 强制**：`ux_binding_active`（`device_id WHERE unbound_at IS NULL` 部分唯一索引）+ "先关旧→再开新、同事务"共同保证；**同设备至多一条 active binding**。

---

## 5. 事务设计落地

所有多步状态变更走 **`env.guofeeder_DB.batch()`**（D1 单隐式事务，后句可见前句写入）。冲突检测**全部由 SQL guard 承担**，无 check-then-act。

| 操作 | batch 内语句顺序 |
|---|---|
| **bind** | ① `INSERT device_binding`（`WHERE NOT EXISTS(active)`） → ② `UPDATE device → BOUND`（`state IN (…)` + `EXISTS(active AND user_id=?)`） → ③ `INSERT device_event('bind')`（同 guard） |
| **unbind** | ① `UPDATE device_binding SET unbound_at=…`（`AND unbound_at IS NULL`） → ② `UPDATE device → UNBOUND`（`state IN (…)` + `NOT EXISTS(active)`） → ③ `INSERT device_event('unbind')`（`NOT EXISTS(active)`） |
| **transfer** | ① 关旧（`AND user_id = ?` ← **即 CAS guard**） → ② `INSERT` 新（`WHERE NOT EXISTS(active)`） → ③ `UPDATE device → BOUND` → ④ `INSERT device_event('transfer')` |
| **revoke** | ① 关 binding → ② `INSERT event('unbind')`（仅读到 active 时入批） → ③ `UPDATE device_credential → REVOKED` → ④ `UPDATE device → REVOKED` → ⑤ `INSERT event('retire')` |

**关键性质**：冲突路径的 guard 令后续语句全部 0 行 ⇒ **"检测到冲突"与"零写入"同时成立**（非抛错回滚）；`ux_binding_active` 从不被撞（只做 active→inactive 的单向变更）。

---

## 6. 验收结果（自测）

| 套件 | 覆盖 | 结果 |
|---|---|---|
| **P1-5-1** Binding Core | 5 个导出全分支 · payload 字段 · batch 语句顺序 · 零写入/零事件路径 · CAS · guard · C-3/C-4 裁决 · `appendEvent` 回归 | **89 PASS / 0 FAIL** |
| **P1-5-2** Admin API | 路由可达性 · 幂等 · 冲突零写入 · 非法入参 · 未授权 401 一致性 · `forced` 不可控 · 旧端点回归 · 跨端点语义联动 | **139 PASS / 0 FAIL** |
| **P1-5-3** Revoke Closure | 正常 revoke / 无 binding / 重复 revoke / **batch 失败全量回滚** / 历史保留 / 回归 / 静态边界 | **48 PASS / 0 FAIL** |

**验证手法**：`node --check` 语法检查 + **内存 mock D1**（实现 `prepare/bind/first/all/run/batch`，含快照回滚与故障注入）+ 直接 `import` Worker default export 调 `fetch(Request, env)` 走完整链路（鉴权 → 路由 → 服务 → 响应），并对 SQL 语句轨迹断言"零写入"。脚本位于 `D:/Guo_Feeder_Project/.tmp_p15/`（**仓库外，不入 git**）。

**静态边界**：`binding.js` 全文负向 grep `device_credential` / `getActiveCredential` / MQTT / token / `Authorization` / `fetch(` / `Request(` / `Response(` ⇒ **0 命中**（含注释）。

> ⚠️ **自测的局限**：mock 只**重述 SQL 语义**，SQL 文本正确性、D1 真实约束（部分唯一索引 / CHECK）、部署版 Worker 行为**均待 P1-5-5 在真实环境验证**。

---

## 7. 遗留问题

| # | 问题 | 影响 | 处置 |
|---|---|---|---|
| **1** | **EMQX 外呼与 D1 事务无法原子化** —— ACL 收紧 / 踢会话在 batch **之前**执行，batch 失败时 ACL / 会话已生效（不可逆） | 偏向保守方向（凭据仍 REVOKED，设备无法接入）；**与改动前同性质，非本阶段引入** | 登记为**已知限制**；P1-5-5 复核 |
| **2** | `unbind` 事件的 guard 依赖**秒级时间戳** —— 极端情形（读到无 active、同时刻并发绑定并被本批关闭）可能**漏记一条 `unbind` 审计** | 审计缺口，**不影响状态一致性** | 登记；如需强审计可在 P1-5-5 评估改用 `id` 判据 |
| **3** | **P1-5-5 正式验收未做** —— 当前全部为离线 mock 自测；未在**真实 D1**（`--local`/`--remote`）与**部署版 Worker**（带 `ADMIN_API_TOKEN`）上跑 T-1…T-19 | 契约与 SQL 的真实正确性未证 | **下一阶段（P1-5-5）** |
| **4** | P1 无用户体系 ⇒ **任意合法 `user_id` 字符串都能成为 owner** | 缓解 = `/api/admin/**` token + `bound_by='admin'` 审计；`user_id` 来源须在 APP 阶段改为**服务端解析** | 设计已登记（`P1-5-Device-Binding-Design.md` §1.3） |
| **5** | **代码与文档尚未 commit** —— P1-5-1/2/3 的 4 处代码改动 + 本阶段文档改动均在工作树 | — | 待人工裁决提交粒度 |
| **6** | `credential.js` 已不再使用 `setDeviceState`（导出保留给其他模块/未来使用） | 无 | 无需处理 |

---

## 8. 给 P1-5-5（正式验收）的前置

1. **真实 D1 验证**：`wrangler d1 execute guofeeder --local/--remote` 跑 `bind` / `unbind` / `transfer` / `revoke`，回读 `device_binding` / `device` / `device_event`，断言：单 active binding、事件序列、`INV-4` 关闭。
2. **部署版 Worker 验证**：`wrangler deploy` 后用 `ADMIN_API_TOKEN` 打通 6 个端点（含 `?limit=` 与 `a%40b.com` 编码路径）。
3. **回归 D3-6**：确认 `revoke` 的既有行为（ACL 收紧 + 踢会话 + 凭据 REVOKED）未被 batch 化改变。
4. **负向检查**：`payload` 无凭据 / 无 token；401 响应体一致性。
5. **固件真机**：`288485896ce4` 上验证"绑定与凭据解耦"（绑定/解绑**不**影响 MQTT 连通）。

---

*本文档由实施结果回写，遵循"代码为唯一真源"。若后续修改 `binding.js` / `credential.js` / `device-registry.js` / `index.js` 的相关契约，**必须同步更新本文件与 `cloud_protocol.md` §11**。*
