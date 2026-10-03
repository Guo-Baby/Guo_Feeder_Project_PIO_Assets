# P1-5 设备绑定设计（Device Binding Design）

> **状态**：**Phase P1-5-A 设计稿 —— 仅文档，不含任何代码**；文末 **Q14–Q20 待裁决**，裁决前不编码。
> **上位文档**：`P1-Cloud-Device-Lifecycle-Plan.md`（§2.4 绑定模型 / §2.5 表草案）· `PhaseD-Final-Review.md` §9（给 P1-5 的接口与前置）· `P1-实现清单.md`（§2 P1-5 工作包）
> **本文件边界**：**不改** `src/` · **不改** `Cloudflare_Assets/guo-feeder-api/src/`（Worker）· **不改** `migrations/`（`0001` 已 apply，不可动）
> **撰写日期**：2026-10-04
> **一句话**：把 `device_binding` 从"已建好但零引用"变成**唯一 owner 的可审计归属链**，且**不碰凭据、不碰身份、不碰在线状态**。

---

## 0. 范围与不变量

### 0.1 P1-5 v1 **必做**（已批准）

| # | 能力 | 落点 |
|---|---|---|
| 1 | **device binding** | `device_binding` 追加式记录 + `device.state` 摘要 |
| 2 | **claim** | 所有权声明入口（形态见 Q14） |
| 3 | **transfer** | 原子转让（关旧 + 开新） |
| 4 | **unbind** | 解除归属，**保留**凭据与 MQTT 能力 |
| 5 | **ownership event** | `device_event` 写 `bind` / `unbind` / `transfer` |

### 0.2 明确**暂缓**（不设计、不预留实现）

❌ 用户系统 · ❌ APP 登录 · ❌ OAuth · ❌ 多用户共享 · ❌ 权限模型（RBAC/ACL-on-binding）· ❌ 家庭/分组 · ❌ 设备转让的"接收入确认"流程

> 理由（用户已明确）：**目前没有 user auth**，D1 的 `user_id` 只是**占位身份**；**不要提前设计完整 IAM**。

### 0.3 继承的**已冻结**边界（不得违反）

| 来源 | 冻结内容 | 对 P1-5 的约束 |
|---|---|---|
| Phase C **Q1** | 未知 `device_id` **只能**进 `CLAIM_PENDING`，严禁自动升 `REGISTERED` | 绑定**不得**成为"绕过准入"的旁路；`CLAIM_PENDING` 设备**不可绑定** |
| Phase D **Q9** | `CredentialManager` 独占凭据状态机；CloudManager 不得感知 `ACTIVE/TESTING/generation` | 绑定流程**不得**读写凭据内部状态 |
| Phase D **Q10** | 只有 `result.object == "credential_set"` 且 `confirmed` 才可升 ACTIVE | **不得复用 `credential` 命名空间**（防确认语义混淆） |
| `P1` Freeze #5/#6 | **解绑 ≠ 吊销**；归属关系 ≠ MQTT 身份 | `BOUND→UNBOUND` **保持** registry / credential / ACL / MQTT 能力 |
| `P0` **DV-1..3** | `device_id` **不可变** | 绑定**不得**触碰 `device_id`；identity 与 binding 完全解耦 |
| `0001` DDL | `ux_binding_active`（`unbound_at IS NULL` 的部分唯一索引） | **同一设备至多一个 owner**，由 DB 强制 |
| `PhaseD-Final-Review` §9 | 分配/解除 owner 属**控制面** ⇒ 走 `/api/admin/**`（`ADMIN_API_TOKEN`） | 与 `credential.js` 同一鉴权面，直到 APP 鉴权就位 |

### 0.4 本文档**新冻结**的不变量（INV）

| # | 不变量 | 强制手段 |
|---|---|---|
| **INV-1** | `device.state` 是**摘要**，owner 唯一真源 = `device_binding WHERE unbound_at IS NULL` | 两者**必须**在同一事务内同步更新 |
| **INV-2** | 状态一致性：有 active binding ⇔ `state='BOUND'`；无 active binding 且有绑定历史 ⇔ `state='UNBOUND'`；无 active binding 且无历史 ⇔ `state='REGISTERED'` | 事务内 + 回归用例 D3-8 同构 |
| **INV-3** | `REVOKED` 是**终态**：任何 bind / unbind / transfer 一律拒绝 | 前置校验（与 `grantClaim` 的 `422 device_revoked` 一致） |
| **INV-4** | **退役（`REVOKED`）必须关闭 active binding**（否则 INV-2 破） | 实施时在 `revokeDevice()` 同一批次内补 `UPDATE device_binding … unbound_at=now` + `unbind` 事件（**见 §8**） |
| **INV-5** | 绑定流程**不改** `device_credential` 任何列；凭据流程**不改** `device_binding` 任何行（除 INV-4 的退役配对） | 代码审查 + 定向回归 |
| **INV-6** | 事件 `payload` **永不**含密码 / 凭据 / token / `ADMIN_API_TOKEN` | 负向 grep（与 D-5 同纪律） |
| **INV-7** | 绑定**不要求设备在线**（控制面与数据面分离，C-3） | 无需 MQTT 交互 |

---

## 1. Ownership 模型

### 1.1 关系基数（P1 冻结）

```
User ──(1..N)──► Binding ──(N..1)──► Device
      一个用户可有多个设备        一台设备**同一时刻至多一个 owner**（不做共享）
```

| 关系 | P1 结论 | 依据 |
|---|---|---|
| 1 用户 : N 设备 | ✅ **允许**（正常场景） | §2.4 |
| 1 设备 : 1 owner | ✅ **唯一**（部分唯一索引强制） | `ux_binding_active` |
| 1 设备 : N 绑定历史 | ✅ **追加式**，永不 UPDATE 旧行的有效期 | §2.4 |
| N 用户 : 1 设备（共享） | ❌ **禁止**（见 **Q19**） | 无权限模型 |
| 1 用户 : N 设备（**同一设备重复绑定**） | 幂等（见 F-6） | 本设计 |

### 1.2 owner 的唯一真源

- **owner 判定**：`SELECT user_id FROM device_binding WHERE device_id=? AND unbound_at IS NULL`
  - 命中 1 行 ⇒ 该 `user_id` 是 owner；0 行 ⇒ **无主（unowned）**
  - **不可能 >1 行**（`ux_binding_active` 保证）
- **禁止**从 `device.state` 反推 owner（`BOUND` 只说明"有 owner"，不说明是谁）
- **禁止**新建"current_owner"冗余列（双写 = 双真源，迟早不一致）

### 1.3 `user_id` 的身份语义（P1 占位）

| 项 | 冻结 |
|---|---|
| 语义 | **不透明外部标识**；P1 **不校验其存在性**（无用户体系） |
| 来源 | 由管理端调用方提供（Q14）；APP 阶段接真实鉴权后改为**服务端从 token 解析**，**不由前端传** |
| 格式 | 建议白名单 `^[A-Za-z0-9_.:@-]{1,64}$`（防注入 / 防超长 / 可读） |
| 大小写 | **区分大小写**（原样存储、原样比较） |
| 空值 | **拒绝**（`400 invalid_user_id`） |

> ⚠️ **已知风险（必须登记）**：P1 下**任意字符串都能成为 owner**（无用户校验）。缓解 = 管理端 token 门控 + `bound_by` 审计 + 事件流水可追溯。**P1 不做**"先到先得"自助绑定（= 抢注，与 Q1 反抢注冲突）。

### 1.4 与 `device.state` 的一致性不变量（INV-2 展开）

| `device.state` | 有 active binding？ | 有绑定历史？ | 含义 |
|---|---|---|---|
| `FACTORY` | 否 | 否 | 产线预置，云端未发现 |
| `CLAIM_PENDING` | 否 | 否 | 已发现，待准入（**不可绑定**） |
| `REGISTERED` | 否 | 否 | 已签发凭据，**从未绑定** |
| `BOUND` | **是** | 是 | 当前有 owner |
| `UNBOUND` | 否 | **是** | 曾被绑定，现已解除（保留凭据） |
| `REVOKED` | 否（INV-4） | 是 | 退役终态 |

---

## 2. Binding 状态机

### 2.1 状态图（设备视角）

```
FACTORY
   │  (云端发现，upsert)
   ▼
CLAIM_PENDING                    ← 绑定在此**不可用**（必须已准入 + 已签发）
   │  (Q1 准入 + P1-3 凭据 ACTIVE)
   ▼
REGISTERED ──bind──► BOUND ──unbind──► UNBOUND ──bind──┐
   ▲                   │                    │          │
   │                   │  transfer          │          │
   │                   └───────► BOUND ◄────┘          │
   │                                  ▲                │
   └──────────────────────────────────┴────────────────┘
                     （REGISTERED / BOUND / UNBOUND）
                              │  retire
                              ▼
                          REVOKED（终态，不可逆）
```

> **`REGISTERED` 不会因解绑而回到**：解绑落 `UNBOUND`（保留"曾绑定"事实）。
> **`transfer` 不改变 `state`**（BOUND → BOUND）。

### 2.2 迁移表

| 当前 | 动作 | 前置 | 结果 state | 事件 | 事务内容 |
|---|---|---|---|---|---|
| `REGISTERED` | **bind**(u) | 有 ACTIVE credential · 无 active binding | `BOUND` | `bind` | INSERT binding · UPDATE device · INSERT event |
| `UNBOUND` | **bind**(u) | 同上 | `BOUND` | `bind` | 同上 |
| `BOUND`(u) | **bind**(u) | 同一 owner | `BOUND` | **无**（幂等 noop） | 无写 |
| `BOUND`(x) | **bind**(y≠x) | — | — | — | **拒绝** `422 already_bound` ⇒ 改走 transfer |
| `BOUND`(x) | **unbind** | — | `UNBOUND` | `unbind` | UPDATE `unbound_at=now` · UPDATE device · INSERT event |
| `REGISTERED`/`UNBOUND` | **unbind** | 无 active binding | 不变 | **无**（幂等 noop） | 无写 |
| `BOUND`(x) | **transfer**(y) | y≠x · y 合法 | `BOUND` | `transfer` | UPDATE 关旧 · INSERT 开新 · UPDATE device · INSERT event（**同一批次**） |
| `BOUND`(x) | **transfer**(x) | 目标 = 现 owner | `BOUND` | **无**（幂等 noop） | 无写 |
| `REGISTERED`/`UNBOUND` | **transfer** | 无 active binding | — | — | **拒绝** `422 not_bound` ⇒ 改走 bind |
| `FACTORY`/`CLAIM_PENDING` | bind / transfer | — | — | — | **拒绝** `422 device_not_registered` |
| `REVOKED` | 任意 | — | — | — | **拒绝** `422 device_revoked`（INV-3） |
| 任意（有 ACTIVE credential 缺失） | bind / transfer | — | — | — | **拒绝** `422 device_not_provisioned` |

### 2.3 与凭据流程的交叉（**本设计的相容性基石**）

已核对的**既有实现事实**（只读确认，非推测）：

| 既有函数 | 对 `device.state` 的写法 | 对 bound/unbound 的影响 | 判定 |
|---|---|---|---|
| `upsertDeviceOnIngest()` | `ON CONFLICT DO UPDATE SET … state = CASE WHEN device.state='FACTORY' THEN 'CLAIM_PENDING' ELSE device.state END` | **显式守卫**：非 FACTORY 一律不动 | ✅ **相容**（绑定态不会被上行冲刷） |
| `activateCredential()` | `UPDATE device SET state='REGISTERED' … WHERE state IN ('CLAIM_PENDING','FACTORY')` | 只从两个"未注册"态升级 | ✅ **相容**（**轮换不会把 BOUND/UNBOUND 打回 REGISTERED**） |
| `revokeDevice()` | `setDeviceState(..., 'REVOKED')`（无条件） | **不碰 `device_binding`** | ⚠️ **必须补**（INV-4，见 §8） |
| `setDeviceState()` | 通用 `UPDATE device SET state=?` | 无守卫 | ⚠️ 实施 P1-5 时**只允许经专用 helper 调用**，避免绕过 INV-2 |

> **结论**：既有代码**已经**为绑定态让路；P1-5 只需（a）新增绑定模块，（b）补 `revokeDevice` 的配对关闭（§8）。

---

## 3. Claim flow（准入 → 绑定）

### 3.1 三个易混淆的 "claim"（**必须分清**）

| 概念 | 层级 | 现存实现 | 门控对象 | 事件类型 |
|---|---|---|---|---|
| **① 准入 claim**（P1-4） | 设备 → 云端"可信首次注册" | `grantClaim()` 写 `device_event(type='register', actor='cloud')`；`claimAuthorized()` 读取 | **能否签发凭据** | `register` |
| **② 所有权 claim**（P1-5） | 人 → 设备"归属声明" | **本设计** | **谁拥有设备** | `bind` |
| ③ 配对码 claim（未实现） | 设备屏显码 → APP 核销 | — | ② 的**自动化形态** | `bind` |

⇒ **冻结**：P1-5 **不得**复用 `register` 事件类型，**不得**读写 `claimAuthorized()` 的准入判定，**不得**引入 `credential` 命名空间。

### 3.2 P1-5 v1 的绑定入口（**Q14**）

```
① 前置：设备已 REGISTERED（P1-4 准入 ✅ + P1-3 凭据 ACTIVE ✅）
        ↓
② owner 声明：由「绑定入口」发起（Q14 裁决）
        ↓
③ 校验：state ∈ {REGISTERED, UNBOUND} · 无 active binding · user_id 合法 · 有 ACTIVE credential
        ↓
④ 事务（同一批次）：INSERT device_binding · UPDATE device.state='BOUND' · INSERT device_event('bind')
        ↓
⑤ owner 生效：此后"谁能控制该设备"由**权限层**决定（P1 仅记录，不下发命令）
```

### 3.3 未来配对码形态（**预留，不实现**）

若 Q14 裁决要做配对码，形态为：设备屏显 6–8 位**短时码**（`claim_code`）→ APP 提交 → 云端核销 → 执行 ②–④。
**本设计预留**：命名空间用 **`claim`**（**绝不用 `credential`**），并**不建表**（P1-5 v1 不建，避免越界）。
设备侧可行性**已具备**（OLED 已存在），但**属独立工作包**。

---

## 4. Transfer flow（原子转让）

### 4.1 事务步骤（D1 `batch()`）

```sql
-- 同一批次（隐式事务，任一失败则整体回滚）
UPDATE device_binding
   SET unbound_at = :now
 WHERE device_id = :id AND unbound_at IS NULL;            -- ① 关旧（0 或 1 行）

INSERT INTO device_binding (device_id, user_id, bound_at, bound_by, reason)
VALUES (:id, :to_user_id, :now, :actor, :reason);         -- ② 开新

UPDATE device
   SET state = 'BOUND', updated_at = :now
 WHERE device_id = :id AND state <> 'REVOKED';            -- ③ 摘要同步（REVOKED 守卫）

INSERT INTO device_event (device_id, type, actor, payload)
VALUES (:id, 'transfer', :actor, :payload_json);          -- ④ 审计（一条）
```

**顺序铁律**：**必须先关旧、再开新**（与 D-3 的"切主先降旧再升新"同源）。
若顺序颠倒，`ux_binding_active` 会**立即拒绝**（同一设备两个 `unbound_at IS NULL`）。

`payload_json` = `{"from":"<u1>","to":"<u2>","reason":"<...>"}`。

### 4.2 并发与竞争

- **两个并发 transfer** ⇒ `ux_binding_active` 使**恰好一个成功**；另一个因唯一索引冲突失败 ⇒ 返回 `422 binding_conflict`，**客户端可安全重试**（重试后走幂等 noop 或再次冲突）。
- **禁止**"先查后写"（check-then-act）⇒ 必须靠 DB 约束兜底（与 A-7 的"不做 GET 预检"同纪律）。
- 失败**不得**留下"双主"或"无主"：批次原子性保证（**Phase E 必须实测"批次中途失败 ⇒ 全回滚"**，见 §9 T-13）。

---

## 5. API contract

### 5.1 通用约定

| 项 | 约定 |
|---|---|
| 前缀 | `/api/admin/**`（Bearer `ADMIN_API_TOKEN`，常量时间比较，401 body 一致） |
| 方法 | 状态变更一律 `POST`；查询用 `GET` |
| 请求体 | `application/json`；`Content-Type` 缺失时按 `{}` 处理（沿用既有风格） |
| 响应 | 复用 `jsonResponse(obj, status)`；错误体 `{"error":"<snake_case>", ...上下文}` |
| 幂等 | **语义幂等**（重复调用安全）；P1-5 **不引入** `Idempotency-Key` 头 |
| 上游失败 | 与既有约定一致：`502`（EMQX 类）；P1-5 通常不触上游 |

### 5.2 端点清单

| # | 方法 | 路径 | 用途 | 备注 |
|---|---|---|---|---|
| 1 | POST | `/api/admin/device/:id/bind` | 绑定 owner | body `{user_id, reason?}` |
| 2 | POST | `/api/admin/device/:id/unbind` | 解除归属 | body `{reason?}` |
| 3 | POST | `/api/admin/device/:id/transfer` | 原子转让 | body `{to_user_id, reason?}` |
| 4 | GET | `/api/admin/device/:id/binding` | 当前 owner + 绑定历史 | 只读 |
| 5 | GET | `/api/admin/user/:user_id/devices` | 该用户当前拥有的设备 | 见 Q18 |
| 6 | GET | `/api/admin/device/:id` | **扩展现有**：追加 `binding` / `binding_history` 字段 | 向后兼容（只增字段） |

> **命名不得与既有冲突**：`claim` / `credential` / `credential/rotate` / `revoke` / `reconcile` 已占用；新增只用 `bind` / `unbind` / `transfer` / `binding`。

### 5.3 请求 / 响应示例

**bind（成功）**
```json
POST /api/admin/device/288485896ce4/bind
{"user_id":"u_demo_0001","reason":"admin_bind"}

200 {"device_id":"288485896ce4","action":"bind","ok":true,
     "owner":"u_demo_0001","state":"BOUND","binding_id":17}
```

**bind（幂等 noop）**
```json
200 {"device_id":"288485896ce4","action":"bind","ok":true,"idempotent":true,
     "owner":"u_demo_0001","state":"BOUND","note":"already_owned_by_same_user"}
```

**transfer（成功）**
```json
POST /api/admin/device/288485896ce4/transfer
{"to_user_id":"u_demo_0002","reason":"admin_transfer"}

200 {"device_id":"288485896ce4","action":"transfer","ok":true,
     "from":"u_demo_0001","to":"u_demo_0002","state":"BOUND"}
```

**binding（查询）**
```json
200 {"device_id":"288485896ce4","owner":"u_demo_0002",
     "state":"BOUND",
     "binding":{"id":18,"user_id":"u_demo_0002","bound_at":1791060000,"unbound_at":null,"bound_by":"cloud"},
     "history":[{"id":18,"user_id":"u_demo_0002","bound_at":1791060000,"unbound_at":null},
                {"id":17,"user_id":"u_demo_0001","bound_at":1791059000,"unbound_at":1791060000}]}
```

### 5.4 错误码表

| HTTP | `error` | 触发 | 语义 |
|---|---|---|---|
| 400 | `invalid_device_id` | 非 12 位小写 hex | 沿用既有 |
| 400 | `invalid_user_id` | 空 / 超 64 / 非法字符 | 新增 |
| 401 | `unauthorized` | token 缺失/错误/未配置（三种 body **逐字节相同**） | 沿用既有 |
| 404 | `device_not_found` | D1 无该设备 | 沿用既有 |
| 422 | `device_revoked` | `state='REVOKED'` | 沿用既有措辞 |
| 422 | `device_not_registered` | `FACTORY` / `CLAIM_PENDING` | 新增 |
| 422 | `device_not_provisioned` | 无 `ACTIVE` credential | 新增 |
| 422 | `already_bound` | bind 时已有**其他** owner | 新增（提示改 transfer） |
| 422 | `not_bound` | transfer/unbind 时无 active binding（transfer） | 新增 |
| 422 | `binding_conflict` | 并发竞争 / 批次冲突 | 新增（可重试） |
| 405 | `method_not_allowed` | GET 打状态变更端点 | 沿用既有 |
| 404 | `not_found` | 未知 action | 沿用既有 |

> **HTTP 语义选择说明**：既有代码用 **`422`** 表达"状态不允许"（`device_revoked`）。
> 本设计**沿用 422**（而非 409）以保持**同一 API 面的单一风格**；若裁决要求 REST 更严格，可整体改 409（**一次性替换，不混用**）。

---

## 6. Event model

### 6.1 事件类型与 payload schema

`device_event.type` 已由 `0001` 的 CHECK 允许 `bind` / `unbind` / `transfer`（**无需迁移**）。

| type | actor | payload（JSON 小对象） | 触发 |
|---|---|---|---|
| `bind` | `cloud`（P1）/ `user`（APP 期） | `{"user_id":"<u>","reason":"<...>","source":"admin"}` | 绑定成功 |
| `unbind` | `cloud` / `user` | `{"user_id":"<prev>","reason":"<...>","forced":false}` | 解除成功 |
| `unbind` | `cloud` | `{"user_id":"<prev>","reason":"retire","forced":true}` | **退役配对**（INV-4） |
| `transfer` | `cloud` / `user` | `{"from":"<u1>","to":"<u2>","reason":"<...>"}` | 转让成功 |

### 6.2 actor 语义

| actor | 含义 | P1-5 何时用 |
|---|---|---|
| `cloud` | 云端/管理员发起（`/api/admin/**`） | **P1-5 的默认值** |
| `user` | 终端用户发起（APP） | 预留（APP 鉴权就位后启用） |
| `device` | 设备自身上报 | **不用**（绑定不是设备行为） |
| `system` | 系统自动（对账/超时） | 预留（如未来的"自动回收孤儿绑定"） |

### 6.3 禁止项（INV-6）

- ❌ payload 里放密码 / 凭据 / `ADMIN_API_TOKEN` / 任何 MQTT 凭据
- ❌ 用 `credential` 命名空间的事件名
- ❌ UPDATE / DELETE 历史事件行（`device_event` **追加式**）
- ❌ 一次操作写多条同语义事件（转让 = **恰好 1 条** `transfer`，不再补 `unbind`+`bind`）

---

## 7. Failure matrix

| # | 场景 | 期望行为 | 结果 |
|---|---|---|---|
| **F-1** | `device_id` 非法 | `400 invalid_device_id` | 无副作用 |
| **F-2** | 设备不存在 | `404 device_not_found` | 无副作用 |
| **F-3** | 设备 `REVOKED` | `422 device_revoked` | 无副作用（INV-3） |
| **F-4** | 设备 `FACTORY` / `CLAIM_PENDING` | `422 device_not_registered` | **绑定不得绕过准入**（Q1） |
| **F-5** | 无 `ACTIVE` credential | `422 device_not_provisioned` | 无副作用 |
| **F-6** | bind：已有**同一** owner | `200 idempotent:true` | **不写**事件、不改 state |
| **F-7** | bind：已有**其他** owner | `422 already_bound`（含 `owner` 提示） | 无副作用 ⇒ 须走 transfer |
| **F-8** | transfer：无 active binding | `422 not_bound` | 无副作用 ⇒ 须走 bind |
| **F-9** | transfer：目标 = 现 owner | `200 idempotent:true` | 不写事件 |
| **F-10** | unbind：无 active binding | `200 idempotent:true` | 不写事件、不改 state |
| **F-11** | `user_id` 非法（空/超长/字符） | `400 invalid_user_id` | 无副作用 |
| **F-12** | **并发 transfer**（同设备） | **恰好一个成功**；另一个 `422 binding_conflict` | **禁止**双主/无主 |
| **F-13** | **批次中途失败** | **整体回滚** | 无半状态（**Phase E 必须实测**） |
| **F-14** | 设备**离线**时 bind/transfer/unbind | **允许**（INV-7） | 事件照写；`online/offline` 不受影响 |
| **F-15** | 绑定期间设备**正在轮换凭据** | **互不干扰**（INV-5） | 凭据流程不改 state；绑定流程不改凭据 |
| **F-16** | 解绑后设备继续上行 | **允许**（保留 MQTT 能力） | `last_seen` / `online` 照常刷新；**不得**因 `UNBOUND` 而拒收 |
| **F-17** | 退役（`REVOKED`）时存在 active binding | **同批次关闭**（INV-4） | 否则 INV-2 破 |
| **F-18** | 对 `BOUND` 设备重复 retire | 幂等（既有 `revokeDevice` 已容忍） | 无二次破坏 |
| **F-19** | `unbound_at < bound_at` | **DB CHECK 拒绝** | 时间单调性由 DDL 保证 |
| **F-20** | 管理端点 token 缺失/错误/未配置 | `401`（三 body 一致） | 不泄露状态 |

---

## 8. 兼容性与**必须修改的既有代码**（实施前须知）

| # | 文件 | 改动 | 理由 | 风险 |
|---|---|---|---|---|
| 1 | `device-registry.js` | 新增绑定读写（`bindDevice` / `unbindDevice` / `transferDevice` / `getBinding` / `listUserDevices`）；`DEVICE_STATES` 已含 BOUND/UNBOUND（**无需改**） | P1-5 主体 | 低 |
| 2 | `index.js` | `handleAdmin()` 增加 4–5 条 action 分支 + `/api/admin/user/:id/devices` 路由 | 暴露端点 | 低（只加不改） |
| 3 | `credential.js` | **`revokeDevice()` 补一行批次**：关闭 active binding + 写 `unbind(reason=retire, forced=true)` | INV-4 | **中**（改既有函数，须回归 D3-6） |
| 4 | （新）`binding.js` | 4 个动作的编排 + 校验 | 与 `credential.js` 对称 | 低 |
| 5 | `README.md`（Cloudflare） | 路由/不变量/幂等语义 | 文档同步 | 无 |

**不改**：`0001_*.sql`（**不新增迁移**）· `wrangler.toml` · `admin-auth.js` · `emqx-admin.js` · **固件 `src/` 全部**（P1-5 **零固件改动**）。

> **重要**：P1-5 **不需要**任何固件改动 —— 绑定是纯控制面能力。
> 设备侧唯一相关点是 `F-16`（`UNBOUND` 时仍必须能连、能收），而这**已由既有固件满足**（固件不感知绑定状态）。

---

## 9. 测试用例（C-7 展开，实施后落 `test/` 与云端脚本）

| # | 用例 | 判据 |
|---|---|---|
| T-1 | bind 正常 | `state=REGISTERED → BOUND`；`device_binding` 新增 1 行；`device_event` 新增 1 条 `bind` |
| T-2 | bind 幂等 | 重复同 owner ⇒ `200 idempotent`，**行数不变**、**事件数不变** |
| T-3 | bind 冲突 | 换 owner ⇒ `422 already_bound`，**D1 无任何变化** |
| T-4 | unbind 正常 | `BOUND → UNBOUND`；旧行 `unbound_at` 非空；事件 1 条 |
| T-5 | unbind 幂等 | 重复 ⇒ `200 idempotent`，无新行 |
| T-6 | transfer 正常 | 关旧 + 开新；`from/to` 正确；事件 **恰好 1 条** `transfer` |
| T-7 | transfer 后 owner 唯一 | `SELECT … WHERE unbound_at IS NULL` 恰 1 行，且 = 新 owner |
| T-8 | transfer 幂等 | 目标 = 现 owner ⇒ `200 idempotent` |
| T-9 | transfer 无主 | `422 not_bound` |
| T-10 | 准入不足 | `CLAIM_PENDING` 设备 bind ⇒ `422 device_not_registered` |
| T-11 | 退役设备 | `REVOKED` 设备 bind/unbind/transfer ⇒ `422 device_revoked` |
| T-12 | 用户多设备 | 1 user 绑 3 台 ⇒ `GET /api/admin/user/:u/devices` 返回 3 台 |
| T-13 | **批次原子性** | 注入一条必失败语句 ⇒ **全部回滚**（无 binding、无 state 变更、无事件） |
| T-14 | 并发 transfer | 两个并行请求 ⇒ **恰一个 200**，另一个 422；最终 owner 唯一 |
| T-15 | 退役配对 | `revokeDevice()`（BOUND 设备）⇒ `REVOKED` **且** active binding 已关闭（INV-2/INV-4） |
| T-16 | 凭据不受影响 | bind/unbind/transfer 前后 `device_credential` **逐列不变**（INV-5） |
| T-17 | 上行不受影响 | `UNBOUND` 后真机上行 ⇒ `last_seen` 刷新、`online` 正常（F-16） |
| T-18 | 明文负向 | 全部事件 payload + 全部响应体 **无** password/凭据/token（INV-6） |
| T-19 | 鉴权一致性 | 缺/错/未配置 token 三种 401 body **逐字节相同**（F-20） |

---

## 10. 待裁决问题（**裁决后才进入 P1-5 编码**）

| # | 问题 | 候选 | **建议** | 影响落点 |
|---|---|---|---|---|
| **Q14** | **绑定入口是谁？**（谁有权发起 owner 声明） | A 管理端直绑（`/api/admin/...`）· B 设备屏显配对码 + APP 核销 · C 先到先得自助 | **A**（P1 无 user auth，管理端是唯一可信入口）；**B 预留命名空间 `claim`、不建表**；**C 明确禁止**（= 抢注） | §3.2 ② / 端点 1 |
| **Q15** | **管理员能否强制解绑？** | A 能（写 `forced:true`）· B 不能 | **A，但须留痕**：`device_event(type='unbind', actor='cloud', payload.forced=true, reason=...)`；且**收窄时机** = APP 鉴权就位后，管理员仅保留"运营干预"权限（unbind/retire），**bind/transfer 交还用户** | `unbind` 实现 / §6.1 |
| **Q16** | **设备恢复出厂如何处理 binding？** | A 保留（不自动解绑）· B 恢复出厂即自动解绑 · C 恢复出厂即 REVOKED | **A**。**已实测确认**：`device_id` 由 **eFuse WiFi-STA MAC 纯函数派生**（`device_identity.cpp`），NVS 被擦后**重新派生为同一值** ⇒ **binding 不会变孤儿**；但 `gfcred` 被清 ⇒ 设备**失去凭据**，需走"重新签发"（P1-3）恢复 ⇒ **不应自动解绑**（否则用户重配后还要重新绑定）。**禁止 C**（误清 NVS 不该报废设备） | §3 / Q20 相关 |
| **Q17** | **用户删除账号如何处理设备？** | A 级联删除设备 · B 解绑但保设备 · C 不管 | **B**。关闭 active binding（`reason='owner_deleted'`）⇒ 设备落 `UNBOUND`（**保留** credential 与 MQTT 能力）；**保留**绑定历史（审计）；**禁止 A**（误删账号导致设备报废）。P1 无用户体系 ⇒ 仅登记为**策略预留** | §1.1 / INV-2 |
| **Q18** | **一个用户多个设备关系？** | A 允许 1:N · B 限制上限 · C 引入分组 | **A（允许，P1 不设上限）**；查询走 `ix_binding_user`；**不做**批量绑定 / 不做家庭分组（超 P1 范围） | 端点 5 |
| **Q19** | **一个设备多个用户共享？** | A 禁止（唯一 owner）· B 允许只读共享 · C 引入 roles | **A（禁止）**。无权限模型 ⇒ 共享 = 无法界定的权限；未来若需，走**独立阶段**（引入 roles/ACL-on-binding），**不复用** P1-5 结构 | §1.1 / `ux_binding_active` |
| **Q20** | **退役（`REVOKED`）是否必须关闭 active binding？**（**新增，建议一并裁决**） | A 必须关闭（同批次）· B 允许 `REVOKED` 保留 active binding（INV-2 豁免） | **A**。否则出现"已退役但仍有 owner"的矛盾态，**且** `GET binding` 会对外报一个不可用的 owner。代价 = **修改既有 `revokeDevice()`**（§8-3），需回归 D3-6 | INV-4 / §8 |

---

## 11. 后续动作（本次**未做**）

- [ ] 用户裁决 Q14–Q20
- [ ] 裁决后出 **P1-5-E 实施拆分**（对齐 Phase D 的 D-1/D-2/D-3 节奏）
- [ ] 实施时落 `docs/interfaces/cloud_protocol.md` 的**管理端绑定契约**（属实施阶段，不在本设计内）
- [ ] 把 §9 用例 T-1…T-19 落为受跟踪测试资产
- [ ] （若 Q14 选 B）另立 **配对码工作包**，命名空间 `claim`
