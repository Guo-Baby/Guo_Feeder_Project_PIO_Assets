# P1 —— 云端 · 设备生命周期管理（Cloud-Device Lifecycle Plan）

> **状态**：**规划稿 · 待人工审核**（本文只做架构规划，**不含任何实现**）
> **上游依据**：`docs/architecture/P0-Final-Review.md`（P0 基线与遗留 L-1..L-7）·
> `Cloud-APP-Platform-Plan.md` v2（P0–P6 路线）· `P0-设备身份与Topic隔离设计.md` §1.4.1 / §4
> **撰写日期**：2026-10-02
> **本文不产出**：`src/` 代码 · 数据库迁移 · API 实现

---

## 0. 定位

| | P0（已完成） | **P1（本文）** |
|---|---|---|
| 解决的问题 | **设备是谁** + **消息去哪** | **设备如何被注册、授权、绑定、监控** |
| 核心产物 | `device_id` 身份 + Topic V3 + 静态 ACL | **设备注册表 + 凭据模型 + 动态授权 + 心跳基础** |
| 遗留输入 | L-1 共享凭据 · L-2 无生产模式 · L-4 无身份迁移 | 本文 §2.2 / §2.3 直接消除 L-1；L-2/L-4 见 §4 |

**一句话目标**：把设备从"**只知道自己的名字**"推进到"**云端知道它是谁、属于谁、现在活着吗、该给它什么权限**"。

---

## 1. P1 目标：从「设备身份隔离」升级到「设备生命周期管理」

```
P0  ──►  一机一 device_id · 一机一 topic · 越权被拒
         （设备能独立、互不干扰地通信 —— 但云端不认识它、也管不了它的权限）

P1  ──►  设备注册 · 凭据签发/轮换/回收 · 动态 ACL · 绑定归属 · 在线状态
         （云端成为"设备的权威登记处"，权限随设备生命周期而变化）
```

**P1 完成的判据（供审核）**
1. 新增一台设备 ⇒ **无需人工改配置/改 ACL** 即可安全上线（注册 → 签发 → 授权全自动）。
2. 两台设备的**凭据互不相同**，且任一台的设备端凭据泄露**不影响**另一台（消除 L-1）。
3. 设备"被解绑/退役" ⇒ 其凭据**可被回收**，回收后**连不上**。
4. 云端能回答：这台设备**是否在线**、**最后活跃时间**、**当前固件版本**。

---

## 2. P1 必须回答的六个问题

### 2.1 Device Registration（设备注册）

**设计目标**：设备首次上线即被云端**自动建档**，不需要人手登记。

**流程（控制面 / 数据面分离，P0 已确立）**

```
设备            EMQX                      Worker（控制/落库）          D1
 │               │                            │                       │
 │─ CONNECT ────►│  (用 P0 阶段的过渡凭据)     │                       │
 │─ PUB up ─────►│─ 规则引擎(异步) ──────────►│                       │
 │  (上线通告)    │   device_id 取自 topic      │─ upsert device ──────►│
 │               │   的 2 级路径              │  (state=CLAIM_PENDING)│
 │               │                            │─ 是否可信注册 ────────►│
 │               │                            │─ 否 ⇒ 停在 CLAIM_PENDING（等待可信首次注册）
 │               │                            │─ 是 ⇒ 走 §2.2 签发凭据 │
 │               │                            │─ 签发完成 ⇒ state=REGISTERED
```

**需要定义的三件事**

| 项 | 设计要点 |
|---|---|
| **device 记录** | 以 `device_id` 为主键（P0 已保证全局唯一、MAC 派生、不可变） |
| **registration state** | 见下表状态机；**注册 ≠ 绑定**（注册是"云端认识它"，绑定是"某个用户拥有它"） |
| **ownership（归属）** | 谁能成为设备所有者？**推荐**：首次绑定即成为 owner（"先绑先得"），并保留管理员强制解绑通道 |

**registration state 状态机（草案）**

| 状态 | 含义 | 进入条件 | 出口 |
|---|---|---|---|
| `FACTORY` | **设备生产完成，已有 `device_id`，但尚未完成云端注册** | 出厂（`device_id` 由 MAC 派生，见 P0） | 首次上行被云端发现 → `CLAIM_PENDING` |
| `CLAIM_PENDING` | **云端已发现设备，等待「首次可信注册」完成** | 收到该 `device_id` 的上行，但**尚未通过可信注册流程** | 可信注册完成 → 凭据签发 + ACL 写入 → `REGISTERED` |
| `REGISTERED` | **已完成 credential 签发，并拥有独立 MQTT 身份** | 凭据签发 + ACL 写入完成 | 被用户绑定 → `BOUND` |
| `BOUND` | 用户绑定 | 绑定操作成功 | 解绑 → `UNBOUND` |
| `UNBOUND` | **解除用户归属，但设备仍保持云端身份**（保留 credential 与 MQTT 连接能力） | 解绑 | 重新绑定 → `BOUND`；退役 → `REVOKED` |
| `REVOKED` | **设备退役，禁止接入** | `UNBOUND → REVOKED` | （终态，人工恢复） |

> ★ **冻结：P1 必须引入「可信首次注册机制」**（本阶段**只冻结需求，不实现 claim token**）。
> **为什么不能「未知 `device_id` 上行即自动 `REGISTERED`」**：P0 阶段设备**无法自证身份**
> （`device_id` 只能从 topic 路径读出，且用的是共享凭据）⇒ 任何拿到共享凭据的人都可以
> **抢先注册任意 `device_id`**（抢注 / 伪造）。
> ⇒ 因此 `CLAIM_PENDING` **不会自动**变成 `REGISTERED`，必须经过**可信流程**
> （形态待定：预置 claim token / 产线一次性写入 / 人工审批 —— **P1 不选型**）。
> ⚠️ **具体生产激活流程属于后续「生产流程设计」**，P1 不定义其实现。

### 2.2 Device Credential（设备凭据）—— 消除 P0 遗留 L-1

**设计目标**：**一台设备一套凭据**（`username` + `password/token`），替换 P0 的共享账号。

| 项 | 建议设计 | 理由 |
|---|---|---|
| `username` | **`dev_<device_id>`** | 与 P0 的 `client_id` 同源、天然一对一、便于 ACL 按 username 精确授权 |
| `password` | 云端生成的**高熵随机串**（长度满足 broker 上限，`A-4` 待实测） | 不用设备 MAC 派生（MAC 可预测，不是秘密） |
| **存储（设备侧）** | **NVS 专用 namespace**（建议 `gfcred`），与身份 `gfid` 分离 | 身份不可变、凭据可变可轮换；分离便于"丢凭据但不丢身份" |
| **存储（云端）** | EMQX 内置数据库（密码由 EMQX 哈希）+ D1 只存 `username` 与状态（**不存明文密码**） | P0 已验证 `POST /users` 可用；明文只在"一次性下发"瞬间存在于内存 |
| **下发方式** | 经 MQTT 下行（P0 已有 `config_set` 类通道）→ 设备写入 NVS → 重启生效 | 复用既有可靠通道，不新增协议 |

**★ 三个身份维度不可混用（冻结）**

| 维度 | 是什么 | **不是什么** |
|---|---|---|
| `device_id` | **设备逻辑身份**（MAC 派生，P0 冻结，不可变） | 不是凭据；不是会话标识 |
| `username` | **MQTT authorization principal**（Broker 的授权主体；ACL 按它精确授权） | **不是用户身份**、**不是设备展示身份**（用户身份在 `device_binding.user_id`；设备展示名属 APP 层） |
| `client_id` | **MQTT 连接会话身份**（必须每次连接唯一） | **不得**用 `username` 充当、**不得**多端复用（P0 §1.4.1 ID-1..ID-4） |

**生命周期**

| 阶段 | 触发 | 关键要求 |
|---|---|---|
| **create** | 设备首次注册（`NEW → REGISTERED`） | 必须先"建账号 + 写 ACL"，**再**下发凭据（顺序不可颠倒） |
| **rotate** | 疑似泄露 / 定期轮换 / 重新配网 | ⚠️ **鸡生蛋**：新凭据必须**经旧连接**下发；切换窗口内**新旧都可用**，确认设备已切换后再吊销旧的 |
| **revoke** | 退役 / 丢失 / 转让完成 | 吊销 = 禁用账号（EMQX API）⇒ 立即断开其在连会话；**凭据不可再用于接入** |

> ★ **冻结：设备侧凭据采用 slot 模型**（**取代**早期的 `pending` / `active` 简单两态）。
>
> 设备 NVS **namespace `gfcred`** 内含 **两个 slot**：`slot0` / `slot1`，每个 slot 保存：
>
> ```
> username · password · version · state          state ∈ { ACTIVE, TESTING, INVALID }
> ```
>
> **轮换流程（冻结）**
>
> ```
> 旧凭据 = ACTIVE
>     ↓  把新凭据写入**另一个** slot，其 state = TESTING
>     ↓  设备用 TESTING 凭据尝试 MQTT 连接
>     ↓  CONNACK 成功  ⇒ 新 slot 置 ACTIVE，旧 slot 置 INVALID
>     ↓  CONNACK 失败  ⇒ 新 slot 退回 INVALID（**但必须区分失败原因**）
> ```
>
> ⚠️ **关键约束：不得因网络失败误判 credential 失败。**
> 只有 Broker **明确拒绝认证**（未授权 / 账号密码错误）才算凭据无效；
> **超时、DNS/网络不可达、TLS 握手失败等一律视为网络问题** ⇒ 保持原 slot 仍为 `ACTIVE`、
> 新 slot 保留待下次重试（不得把测试中的 slot 判死，也不得把旧凭据作废）。
> 该策略属 **P1 设计项**，P0 不涉及。

### 2.3 ACL 动态授权

**目标**：把 P0 的**静态 ACL**（人工 `apply-acl.mjs --device-ids`）升级为**由 device_id 驱动的动态授权**。

```
device_id  ──►  credential(username)  ──►  ACL 规则（该设备专属的三条 topic）
```

**必须定义的授权时序**（顺序是硬约束）

```
① 设备注册（得到 device_id）                       [§2.1]
② 建账号：POST /authentication/.../users            ← username = dev_<device_id>
③ 写 ACL：PUT /authorization/.../rules/users/<username>
        sub guo_feeder/<device_id>/down
        pub guo_feeder/<device_id>/up
        pub guo_feeder/<device_id>/log
④ 落库：D1 记录 username / acl_version / state
⑤ 下发凭据给设备（经旧通道，见 §2.2）
⑥ 设备以新凭据连接 ⇒ 授权生效
```

⚠️ **为什么 ②③ 必须早于 ⑥**：EMQX 是**白名单模式**（兜底 `全部用户 → # → deny`，P0 已确立）
⇒ 未写 ACL 就连接，会表现为"**能连上但收不到/发不出**"（P0 期间实测踩过：EMQX `subscriptions=0`）。
⚠️ **授权缓存生效延迟（`A-1`，待实测）**：规则 `PUT` 后**不是瞬时生效** ⇒ 时序里要留等待或改为
"先建 ACL，再做任何连接/订阅尝试"。

**APP 侧授权（两种方案，P1 需择一）**

| 方案 | 做法 | 优点 | 代价 |
|---|---|---|---|
| **A. slot 池**（P0 设计倾向） | 预置固定数量的 `app_slot_<n>` 账号；登录时 Worker 用 API **改写该 slot 的 ACL** 指向被授权设备 | 账号数恒定、易回收、不暴露"账号创建"接口 | slot 数 = 并发上限；需**回收/超时**机制 |
| **B. 每用户一账号** | 用户首次登录时 `POST /users` 建账号 | 语义直白、无 slot 管理 | 账号数随用户增长（Serverless 上限需核算）；回收逻辑更重 |

> **P0 已确立的平台硬约束**：EMQX Serverless **不支持外部 HTTP 认证 / 扩展授权 / 白名单开关**
> ⇒ 授权只能靠"**预置/创建的账号 + 用 API 改写其 ACL**"。两种方案都满足该约束，差别在**账号生命周期管理**。

**⚠️ 维度纪律（P0 §1.4.1 已冻结）**：`username` = **授权维度**，`client_id` = **会话维度**。
APP 的 `client_id` 必须**每次连接唯一**（UUID v4 / 平台生成），**不得**用 `username` 充当、**不得**多端复用。

### 2.4 APP 绑定模型（User ↔ Device）

**关系定义**

```
User ──(1..N)──► Binding ──(N..1)──► Device
      一个用户可有多个设备        一台设备在不同时间可属于不同用户（保留历史）
```

| 语义 | 设计 |
|---|---|
| 一个用户多个设备 | 是（正常场景） |
| 一个设备同一时刻的 owner | **唯一**（P1 不做共享/多主，简化权限模型） |
| 一个设备多个绑定历史 | 是 ⇒ 绑定记录**追加式**（含 `bound_at` / `unbound_at`），不覆盖删除 |
| 转让 | = 旧 owner 解绑 + 新 owner 绑定；**需原子**（失败不得留下"无主"或"双主"） |
| 解绑后果 | 设备进入 `UNBOUND`：**保留注册与凭据**（避免误解绑导致设备失联），但**无人可控制** |
| 退役 | `UNBOUND` → `REVOKED`：**回收凭据**，设备无法再接 |

> ★ **冻结：解绑不吊销 MQTT credential。**
> 理由：**设备归属关系 ≠ MQTT 身份**。`BOUND → UNBOUND` 时**保持**：
> `device registry` 记录 · `credential`（账号 / ACL）· **MQTT 连接能力**。
> 只有 `UNBOUND → REVOKED`（**退役**）才执行 **`credential revoke`**。
>
> ⇒ "解绑后无人可控制"由**权限层**实现（APP 无授权即不再下发命令），
> **不**通过吊销凭据达成 —— 避免因误操作 / 临时解绑造成设备永久失联。

### 2.5 Cloud 数据库模型（**表结构草案，不实现**）

> 位置：Cloudflare D1（P0 已有实例 `guofeeder`，当前仅 1 张 `mqtt_messages` 表）。
> 下列为**字段清单草案**，不含 DDL、不含迁移。

**`device`** —— 设备主档

| 字段 | 说明 |
|---|---|
| `device_id` (PK) | P0 的 12-hex，MAC 派生，**不可变** |
| `state` | `NEW` / `REGISTERED` / `BOUND` / `UNBOUND` / `REVOKED` |
| `model` / `hw_rev` | 型号与硬件版本（P1 仅登记，不做 OTA 分型） |
| `first_seen` / `last_seen` | 首次/最近上行时间 |
| `fw_version` | 最近上报的固件版本 |
| `created_at` / `updated_at` | 审计 |

**`device_credential`** —— 凭据状态（**不存明文密码**）

| 字段 | 说明 |
|---|---|
| `device_id` (FK) | 指向 `device` |
| `username` | `dev_<device_id>` |
| `state` | `active` / `rotating` / `revoked` |
| `acl_version` | 已写入 EMQX 的 ACL 版本号（用于对账/重放） |
| `created_at` / `rotated_at` / `revoked_at` | 生命周期时间点 |

**`device_binding`** —— 绑定历史（**追加式**）

| 字段 | 说明 |
|---|---|
| `id` (PK) | 自增 |
| `device_id` (FK) | 设备 |
| `user_id` | 用户（P1 不建用户体系，仅存外部标识） |
| `bound_at` / `unbound_at` | `unbound_at IS NULL` ⇒ **当前有效绑定**（唯一约束：同一 device 至多一条有效） |
| `reason` | 绑定/解绑/转让原因（审计） |

**`device_event`** —— 生命周期事件流水（也用于 §2.6 的在线状态）

| 字段 | 说明 |
|---|---|
| `id` (PK) | 自增 |
| `device_id` (FK) | 设备 |
| `type` | `register` / `credential_issue` / `credential_rotate` / `credential_revoke` / `bind` / `unbind` / `online` / `offline` / `retire` |
| `payload` | 事件细节（CBOR/JSON 小对象） |
| `created_at` | 时间 |

**容量核算（P1 级）**：11 台设备 ×（上线/离线各 ~2 次/天 + 少量生命周期事件）
⇒ **每天 < 100 行**，D1 完全无压力 ✅（真正的容量压力在 **P3 的遥测**，P1 不涉及）

### 2.6 Device 状态同步（heartbeat / online / last_seen / fw_version）

| 项 | 设计 | 关键点 |
|---|---|---|
| **online / offline** | **首选 EMQX 生命周期事件**（`client.connected` / `client.disconnected`）→ 规则引擎 → Worker → D1 | **比心跳推断更可靠、且零设备开销**；断网/掉电也能判定 |
| **last_seen** | 每次上行（任意 topic）刷新 | 用于"在线但静默"的兜底判定 |
| **heartbeat** | 设备定期上行一条轻量状态（含 `fw_version`、`uptime`、`rssi` 等） | ⚠️ **必须降频**（P0 D7 已冻结目标 < 1200 行/设备/天）；落点预登记 `mqtt.json.telemetry` |
| **fw_version** | 随上线通告/heartbeat 上报 | 与 `readme.md` §六 Device State Heartbeat 同批落地 |
| **离线兜底** | `last_seen` 超过 N 个心跳周期 ⇒ 判离线 | 防止 EMQX 事件丢失导致"永远在线" |

> ⚠️ **P0 已冻结的容量约束（必须继承）**：11 台 × 10s 上报 ⇒ Worker / D1 双 95% + 年存储 > 5 GB。
> ⇒ **遥测必须降频/阈值/聚合**（目标 < 1200 行/设备/天）。P1 只定义 heartbeat **策略落点**，
> **不实现大规模遥测存储**（属 P3）。

---

## 3. 关键约束（继承 P0，不可违背）

| # | 约束 | 对 P1 的影响 |
|---|---|---|
| C-1 | EMQX Serverless **不支持外部 HTTP 认证 / 扩展授权 / 白名单开关** | 授权只能"预置/创建账号 + API 改写 ACL"（§2.3） |
| C-2 | **白名单模式**（兜底 `全部用户 → # → deny`） | 任何凭据**必须先有 ACL**才能用（顺序硬约束） |
| C-3 | **控制面 / 数据面分离** | 高频数据走 APP 直连 EMQX；Worker 只做控制与落库 |
| C-4 | Worker / D1 容量（P0 实测已近 95%） | 生命周期事件量小（可忽略）；**遥测**必须降频 |
| C-5 | 设备 `device_id` **不可变**（P0 冻结 DV-1..DV-3） | 凭据/绑定变更**不得**触碰身份 |
| C-6 | 设备侧身份在 **NVS `gfid`** | 凭据建议另立 namespace（§2.2），**两者不可混存** |

---

## 4. P1 范围冻结

### 4.1 **P1 包含**

| # | 范围 | 交付形态 |
|---|---|---|
| 1 | **Device registry** | `device` 表 + 注册流程（云端自动建档 + 状态机） |
| 2 | **Credential model** | 一机一凭据（签发 / 轮换 / 回收）+ 设备侧 NVS 存储方案 |
| 3 | **ACL dynamic authorization** | 由 device_id 驱动的自动授权（建账号 → 写 ACL → 落库 → 下发） |
| 4 | **Heartbeat 基础** | 上线通告（含 `fw_version`）+ 在线/离线判定 + `last_seen`（**不含**大规模遥测存储） |
| 5 | （支撑项）**绑定模型** | `device_binding` 表 + 绑定/解绑/转让的**语义**（APP UI 不在 P1） |
| 6 | **生产模式检查**（P0 遗留 L-2） | 固件启动检查：**production 下 topic 模板缺 `<device_id>` ⇒ 输出配置错误并阻止 MQTT 上线**（复用 P0 的"渲染失败 ⇒ 判配置错误"路径，不新增配置项） |

### 4.2 **P1 不包含**（明确排除，防蔓延）

| 排除项 | 归属 |
|---|---|
| APP UI / 交互界面 | P5 |
| Workflow 云端编辑（含既有协议改动） | 后续 |
| **Telemetry 大规模存储** | **P3** |
| **OTA 系统**（含 S-12 用例） | 后续独立阶段 |
| 支付 / 用户账号体系（注册登录、实名等） | 不在本路线（P1 只存 `user_id` 外部标识） |
| 身份迁移流程（P0 L-4） | 独立阶段 |

---

## 5. 前置实测与开放问题

### 5.1 P1-0 实测顺序（**冻结**）

```
A-4  broker 对 username / password / client_id 的限制
 ↓   理由：**A-4 决定 credential 格式**（凭据怎么生成、多长、哪些字符合法）
A-5  账号数量 / API 配额
 ↓   理由：**A-5 决定账号模型**（slot 池 vs 每用户一账号）
A-1  ACL 修改生效延迟
 ↓   理由：**A-1 决定授权生效流程**（能否"签发即用"，是否必须等待 / 重试）
A-6  client.connected / disconnected 事件能力
     理由：**A-6 只影响在线状态的实现方式**，不影响凭据与授权主链路
```

| 序 | # | 事项 | 为什么阻塞 | 实测做法 |
|---|---|---|---|---|
| **1** | **A-4** | `username` / `password` / `client_id` 的 **broker 长度与字符集上限** | **决定 credential 格式** | 用超长 / 特殊字符实测连接与认证 |
| **2** | **A-5** | Serverless **账号数上限**与 API 配额 | **决定账号模型**（A slot 池 / B 每用户一账号） | 查文档 + 试探 API |
| **3** | **A-1** | EMQX **授权缓存生效延迟**（规则 PUT 后多久生效） | **决定授权生效流程**（能否"签发即用"） | PUT 后按秒轮询订阅 / 发布，记录生效时间 |
| **4** | **A-6** | 规则引擎是否**原生支持** `client.connected / disconnected` → HTTP | **只影响在线状态实现** | 控制台 / 文档核实 |

> **实测产物**：`docs/architecture/P1-0-EMQX-Capability-Test.md`（每项含目的 / 环境 / 步骤 / **实测结果** / 结论 / 对 P1 设计影响）。
>
> ✅ **P1-0 已完成（2026-10-02）· 四项均未发现"设计不可行"** ：
> · **A-4 不阻塞** —— `client_id` 上限约 8K~16K（8K 可用 / 16K 失败），字符集近乎不限，
>   **空 `client_id` 竟也被接受** ⇒ P0 的「≤128 + `[0-9a-zA-Z_-]`」是必要且安全的收紧，**保留**；
> · **A-5 支撑一机一账号** —— 账号可**程序化创建/删除**（201/204），上限约 2000
>   ⇒ **P1 不需要 slot 池**；
> · **A-1 解除最大时序顾虑** —— 授权变更**秒级生效（≈1.3 s）**，并非假设的分钟级
>   ⇒ 签发流程只需**等待 ≥2 秒**（或连接失败重试），R-2 风险大幅降级；
> · **A-6 支撑在线状态零设备开销** —— Serverless **不支持订阅 `$SYS`**，但**支持**经
>   **数据集成**捕获 `$events/client_connected` / `$events/client_disconnected`
>   （含 `reason` 字段，可识别 `takenover` 抢线）。

### 5.2 已裁决（见文末 **P1 Architecture Freeze Decision**）

| # | 原问题 | 裁决 |
|---|---|---|
| **Q-1** | 解绑是否立即断开会话？ | **不吊销 credential**；归属关系 ≠ MQTT 身份（§2.4） |
| **Q-2** | 轮换的断电中断策略？ | **slot 模型**（`gfcred` 双 slot + `ACTIVE/TESTING/INVALID`，§2.2） |
| **Q-3** | 生产模式检查是否并入 P1？ | ✅ **纳入 P1**（成本低；属 CloudManager 上线安全边界检查；不涉及 OTA） |

### 5.3 仍开放

| # | 事项 | 说明 |
|---|---|---|
| **Q-4** | legacy ACL 三条何时收窄（P0 L-3）？ | 以"P1 凭据 + P3 落库"完成为终点 |

---

## 6. 风险登记（P1 级）

| # | 风险 | 影响 | 缓解 |
|---|---|---|---|
| R-1 | **凭据下发的"鸡生蛋"** | 轮换/首签需要设备已在线的旧通道 | 首签走过渡凭据；轮换走双凭据窗口 |
| R-2 | **授权缓存延迟**导致"签发即连失败" | 新增设备上线失败率 | 依赖 A-1 实测；必要时加等待/重试 |
| R-3 | **共享凭据过渡期**（P0 遗留）与一机一凭据并存 | ACL/注册逻辑需要兼容两种凭据 | 过渡期明确"哪些设备已切换"，按 `device_id` 白名单灰度 |
| R-4 | 设备侧**凭据写坏/丢失** | 设备永久失联 | 双份策略 + 保留"恢复通道"（本地串口重配） |
| R-5 | 绑定**双主/无主** | 权限错乱 | 绑定表加唯一约束（同一 device 至多一条有效绑定）+ 事务化转让 |
| R-6 | 生命周期事件**漏记** | 审计不可信 | 事件追加式写入；关键状态变更以 `device.state` 为准（事件为流水） |

---

## 7. 交付物（**仅规划，均不含代码**）

| # | 交付物 | 说明 |
|---|---|---|
| 1 | 本文 `P1-Cloud-Device-Lifecycle-Plan.md` | 架构规划（待审核） |
| 2 | `docs/architecture/P1-实现清单.md` | 工作包拆解（P1-1..P1-n + 用例表 + 验收 DoD） |
| 3 | `docs/interfaces/`（新增或扩充） | 注册/凭据/绑定的**接口契约**（与 `cloud_protocol.md` 对接） |
| 4 | 实测报告 | A-1 / A-4 / A-5 / A-6 结论 |
| 5 | D1 **表结构设计**（字段级，非迁移脚本） | §2.5 的定稿 |

---

## 8. 审核问题清单（**已裁决** —— 见文末「P1 Architecture Freeze Decision」）

| # | 问题 | 裁决 |
|---|---|---|
| 1 | **§2.2 凭据格式**：`username = dev_<device_id>`？ | ✅ **确认** |
| 2 | **§2.2 存储**：设备侧凭据放哪？ | ✅ **NVS namespace `gfcred`**（双 slot 模型，见 §2.2） |
| 3 | **§2.3 APP 授权**：A（slot 池）/ B（每用户一账号）？ | ⏭ **P1 暂不实现**，延后到 APP 阶段 |
| 4 | **§2.1 ownership**？ | ✅ **首次绑定获得 owner**，但**必须先经过可信注册流程** |
| 5 | **§2.4 解绑语义**：是否断会话 + 吊销凭据？ | ✅ **不吊销 credential**（归属关系 ≠ MQTT 身份） |
| 6 | **§5 Q-3**：生产模式检查并入 P1？ | ✅ **纳入 P1** |
| 7 | **§4.2 排除项**？ | ✅ 同意（**OTA 排除**、**Telemetry 归 P3**） |

---

## P1 Architecture Freeze Decision

> **状态**：**已冻结（2026-10-02，人工审核通过）**。以下 9 条是 P1 **基线决策**，实施阶段不得擅自变更；
> 如需变更必须显式修订本节并升级文档版号。

| # | 决策项 | **冻结值** |
|---|---|---|
| 1 | **`username`** | **`dev_<device_id>`** |
| 2 | **credential 存储** | **NVS namespace `gfcred`**（双 slot + `ACTIVE/TESTING/INVALID`，见 §2.2） |
| 3 | **APP 授权模型** | **P1 暂不实现**，延后到 APP 阶段；P1 只冻结「`username` 是授权主体」这一维度定义 |
| 4 | **ownership** | **首次绑定获得 owner**，但**必须先经过可信注册流程**（`CLAIM_PENDING → REGISTERED`，见 §2.1） |
| 5 | **解绑** | **不吊销 credential**（归属关系 ≠ MQTT 身份，见 §2.4） |
| 6 | **退役** | **吊销 credential**（`UNBOUND → REVOKED` 才执行 revoke） |
| 7 | **production check** | ✅ **纳入 P1**（成本低；属 CloudManager 上线安全边界检查；不涉及 OTA） |
| 8 | **OTA** | **排除**（不在 P1；S-12 用例仅登记不执行） |
| 9 | **Telemetry** | **排除**，归 **P3** |

**另有两条随本节一并冻结**

| 项 | 冻结内容 |
|---|---|
| **可信首次注册** | P1 **必须引入**「可信首次注册机制」；`CLAIM_PENDING` **不得自动**变为 `REGISTERED`。**本阶段不实现 claim token**；具体生产激活流程属后续「生产流程设计」 |
| **P1-0 顺序** | **A-4 → A-5 → A-1 → A-6**（理由见 §5.1） |

---

## P1 实施阶段（Phase A–G，冻结）

| Phase | 内容 | 对应工作包 | 前置 |
|---|---|---|---|
| **A** | **EMQX 平台能力实测** | P1-0 | — |
| **B** | **D1 Schema** | P1-1 | A |
| **C** | **云端注册 + credential 签发闭环** | **P1-2 + P1-4（合并设计）** | B |
| **D** | **ESP32 `gfcred` 接入** | P1-3 | C |
| **E** | **Binding** | P1-5 | B |
| **F** | **Online State** | P1-6 | B |
| **G** | **Production check** | P1-7 | C（固件侧，可与 E / F 并行） |

> **为什么 Phase C 把 P1-2 与 P1-4 合并**：注册（发现设备）与签发（给身份）是**同一闭环的两半** ——
> 分开做只会得到"能发现但不能授权"或"能授权但无触发点"的半成品。合并设计一次打通。
