# P1-0 EMQX 平台能力实测（Capability Test）

> **状态**：**已完成 · 待人工审核**（A-4/A-5/A-1/A-6 = 2026-10-02；**A-7 = 2026-10-03**）
> **上游**：`P1-Cloud-Device-Lifecycle-Plan.md`（§5.1 冻结的实测顺序）
> **顺序**：**A-4 → A-5 → A-1 → A-6**（A-4 决定 credential 格式 → A-5 决定账号模型 → A-1 决定授权生效流程 → A-6 只影响在线状态实现）；**A-7（create user 幂等性）为 P1-2 编码前的补测，不改变上述顺序**
> **本文性质**：**实测记录**，不含任何实现代码。所有测试脚本位于 `.pio/p0run/`（不入库）。

---

## 0. 测试环境（各项共用）

| 项 | 值 |
|---|---|
| 平台 | **EMQX Cloud Serverless** |
| 部署 | `n302933b.ala.cn-hangzhou.emqxsl.cn`（mqtts `8883`） |
| 管理 API | `https://…:8443/api/v5`（**App ID/Secret**，Basic Auth） |
| MQTT 客户端 | `paho-mqtt`（Python），TLS + CA `data/emqxsl-ca.crt` |
| 凭据来源 | API 凭据来自 `EMQX_Assets/.env`（不入库）；MQTT 测试账号用 **`workbuddy`**（**非设备账号** ⇒ 不会踢掉在线设备） |
| 在线设备 | 1 台（`username=GuoFeederDevice`，`client_id=dev_<device_id>`）—— 测试全程未触碰其凭据 |

> **安全声明**：除 A-1 与 A-7 外均为只读或"连接尝试"。A-1 对 `workbuddy` 的规则做了**临时收紧并已在 finally 中恢复**（恢复亦经实测确认生效）。
> **A-7** 创建了一个**临时测试账号**（`a7test_*`，非设备账号），全程结束**删用户 + 删其 ACL 规则**，
> 并**前后对比全量用户级 ACL 快照**确认生产规则**逐条零改动**（快照已写入报告 JSON）。
> 全程**未改动设备账号凭据、未改动兜底规则、未创建任何长期对象、未写 D1**。

---

## A-4 `username` / `password` / `client_id` 限制

### 测试目的
确定 credential 与 `client_id` 的**格式边界**（长度、字符集），以决定 P1 中设备凭据与 APP `client_id` 的生成规则。

### 测试步骤
1. **基线**：用 `workbuddy` 账号以正常 `client_id` 连接 ⇒ 应成功。
2. **`client_id` 长度**：`23 / 64 / 128 / 256 / 512 / 1024 / 4096 / 8192 / 16384 / 65535` 字符逐一连接，记录 `CONNACK rc`。
3. **`client_id` 字符集**：`:` · `/` · 空格 · 中文 · `-#+@` · **空串**。
4. **`username` 长度**：`64 / 128 / 256 / 1024 / 4096`（密码固定为错误值，观察 broker 的拒绝形态）。
5. **`password` 长度**：`64 / 128 / 256 / 1024 / 4096`（用户名固定，密码为随机长串）。

### 实测结果

| 测试项 | 实测 |
|---|---|
| 基线（10 字符 `client_id`） | ✅ `rc=0` |
| `client_id` 长度 **23 → 8192** | ✅ **全部 `rc=0`** |
| `client_id` 长度 **16384 / 65535** | ❌ **无 CONNACK**（本地或 broker 拒绝） |
| `client_id` 含 **`:`** / **`/`** / **空格** / **中文** / **`-#+@`** | ✅ **全部 `rc=0`** |
| **空 `client_id`** | ⚠️ **`rc=0`（被接受）** |
| `username` 长度 64 → 4096 | 均 `rc=5`（NOT_AUTHORIZED，因密码不匹配）⇒ **未见长度拒绝** |
| `password` 长度 64 → 4096 | 均 `rc=4`（BAD_CREDENTIALS）⇒ **未见长度拒绝** |

### 结论
1. **`client_id` 的实际上限落在 8192 ~ 16384 之间**（本次未二分精确定位；**8192 可用、16384 不可用**）。
2. **`client_id` 字符集无限制**（连空格与中文都被接受）。
3. **`username` / `password` 在 4096 字符内无长度限制**。
4. ⚠️ **空 `client_id` 被 broker 接受** —— 这意味着"不设 `client_id`"不会报错，但会让多台设备落到同一默认会话 ⇒ **必须靠规范禁止**（P0 §1.4.1 已要求唯一）。

### 对 P1 设计影响
- **A-4 不构成阻塞**：P0 自定的「`client_id` ≤ 128、字符集 `[0-9a-zA-Z_-]`」是**保守且安全**的子集，**无需调整**。
- `client_id = dev_<device_id>`（15 字符）与 APP 的 UUID（36 字符）**均远低于上限**。
- 建议 P1 生成 **password 长度 32~64 字符**（高熵且远低于限制）。
- **收紧而非放宽**：正因为 broker 几乎不限制字符集，固件侧的字符集校验（P0-3 已实现）才是**唯一防线** ⇒ 保留。
- ✅ **已补测**：上限精确值见 **§A-4b**（最大可用 **15323** / **15324** 起失败），结论不变。

## A-4b 补测：`client_id` 长度上限精确值（✅ 已补测）

### 测试目的

二分定位 broker 对 `client_id` 的**长度上限精确值**，并判定失败形态
（broker 返回 CONNACK 错误码 vs 连接未完成）。

### 测试环境

同 §0；账号 `workbuddy`（非设备账号）；**只做连接尝试，不改任何服务端配置**。

### 测试步骤

1. 复验已知边界：8192（预期成功）/ 16384（预期失败）。
2. 在 (8192, 16384) 区间二分，共 13 次连接尝试。

### 实测结果

```
  len=8192   -> OK
  len=16384  -> 失败（未收到 CONNACK）
  二分：12288 OK / 14336 OK / 15360 失败 / 14848 OK / 15104 OK / 15232 OK
        / 15296 OK / 15328 失败 / 15312 OK / 15320 OK / 15324 失败 / 15322 OK / 15323 OK

  ⇒ 最大可用长度 = 15323
  ⇒ 最小失败长度 = 15324
```

失败形态：4 个失败样本**全部为"未收到 CONNACK"**（无 CONNACK 错误码返回）。

### 结论

- 实测边界：**15323 可用 / 15324 失败**。
- ⚠️ 失败形态是"连接未完成"，**本测试未能区分**"broker 静默断连"与"客户端库 / 协议编码限制"
  （后续可用 `on_disconnect` 回调补判）。
- **对 P1 无影响**：P0 采用的 `client_id ≤ 128` 比实测边界低**两个数量级**，安全裕度充足。

### 对 P1 设计影响

- **无影响**。P1 的 `dev_<device_id>`（15 字符）与 APP UUID（36 字符）远低于边界。
- 该边界**不应**被当作设计依据（失败形态未完全判定）。

---

## A-5 账号数量 / API 限制

### 测试目的
判断"**每台设备一个账号**"是否可行，还是必须退化为 slot 池。

### 测试步骤
1. `GET /authentication/password_based%3Abuilt_in_database/users?limit=200` ⇒ 统计账号数与清单。
2. `POST …/users` 创建**临时账号** `p1_probe_tmp`。
3. `DELETE …/users/p1_probe_tmp` 删除（**立即清理，不留痕**）。

### 实测结果

| 测试项 | 实测 |
|---|---|
| 账号总数 | **5 个**：`workbuddy` · `test001` · `shouji` · `GuoFeederDevice` · `GuoFeederDevice001` |
| **创建账号** | ✅ **`POST → 201`** |
| **删除账号** | ✅ **`DELETE → 204`** |
| 平台文档记载上限 | 控制台批量导入「一次 1000 条、**累计最多 2000 条**」 |
| API 调用配额 | **未观测到限流**（本次未做压力测试） |

### 结论
1. **账号可完全程序化创建 / 删除** ⇒ **"一机一账号"可行**，不必强依赖 slot 池。
2. 账号数上限约 **2000** ⇒ 对**当前 11 台（乃至数百台）远有余量**。

### 对 P1 设计影响
- **P1 凭据模型（`username = dev_<device_id>`）成立**：注册时**直接创建该设备专属账号**，无需预留 slot。
- "slot 池"方案的**唯一剩余价值在 APP 侧**（避免用户账号数膨胀）；而 **APP 授权已裁决延后**（Freeze Decision #3）⇒ **P1 不需要 slot 池**。
- ⚠️ 登记待补：**账号创建速率限制**与 **API 调用配额**（未测；若 P1 批量注册需评估）。
- ⚠️ 账号数上限 2000 ⇒ 一机一账号下**设备规模上限 ≈ 2000**（当前规模无压力，但应写入容量台账）。

---

## A-1 ACL 修改生效延迟

### 测试目的
**决定授权生效流程**：规则 `PUT` 之后能否"签发即用"，还是必须在签发流程中插入等待 / 重试。

### 测试步骤
1. 读 `workbuddy` 当前规则（原：`allow all guo_feeder/#`）。
2. **基线**：用 `workbuddy` 订阅 `guo_feeder/p1test/down` ⇒ 应 `SUBACK granted=1`（ALLOW）。
3. **收紧**：`PUT` 一条仅允许 `guo_feeder/p1probe-nonexistent/#` 的规则（等效取消上式授权），**记录 PUT 完成时刻**。
4. **轮询**：每 ~1 秒重新连接并订阅 `guo_feeder/p1test/down`，直到 `granted=128`（DENY）⇒ 该时间差即**收紧生效延迟**。
5. **恢复**：`PUT` 回原规则，同样轮询直到恢复 ALLOW ⇒ **恢复生效延迟**。

### 实测结果

| 阶段 | 结果 |
|---|---|
| 基线订阅 | ✅ `SUBACK granted=1`（ALLOW） |
| 收紧 `PUT` | ✅ **`204`** |
| **收紧生效延迟** | ✅ **1.3 秒** |
| 恢复 `PUT` | ✅ **`204`** |
| **恢复生效延迟** | ✅ **1.2 秒** |

> **附带发现（重要）**：规则 `PUT` 的 **body 必须是对象 `{"username": …, "rules": [ … ]}`**，
> **不是**裸数组；且规则项**只传 `{topic, permission, action}`**（**传 `qos` / `retain` 会 400
> `bad_value_for_struct`**）。—— 本项实测前两次尝试均因格式错误返回 `400`，第三次修正后成功。
> 读回时 EMQX 会**自动回填** `retain:"all"` 与 `qos:[0,1,2]`。

### 结论
1. **授权变更延迟为秒级（≈1.2~1.3 s），并非此前假设的"分钟级"**。
2. ⇒ **"签发即用"基本成立**，只需在流程中留 **≥2 秒**的等待余量（或首次连接失败重试 1~2 次）。

### 对 P1 设计影响
- **§2.3 的授权时序仍然成立**，且实现代价很低：
  ```
  建账号 → 写 ACL → 【等待 ≥2s（或连接失败重试）】 → 下发凭据/允许连接
  ```
- **消除 R-2 的主要风险**（"授权缓存延迟导致签发即连失败"）：
  从"必须设计等待/重试机制"降级为"**加 2 秒等待即可**"。
- ✅ **已补测**：3 次重复测量见 **§A-1c**（收紧 1.35~1.36 s / 恢复 1.32~1.40 s），**无长尾**。
- ⚠️ **新增关键发现（比延迟本身更重要）**：**已连接客户端不立即生效**（§A-1c Part 2）
  ⇒ **吊销权限必须配合断开连接**，详见 §A-1c「对 P1 设计影响」。

## A-1c 补测：延迟可重复性 + 已连接客户端行为（✅ 已补测）

### 测试目的

1. 对 A-1 的延迟做 **3 次重复测量**，确认可重复性与是否存在长尾。
2. 回答"**已连接客户端是否立即生效**" —— A-1 未覆盖，但它对 **P1 的吊销语义是决定性的**。

> ⚠️ **中间版本（A-1b）的结论已作废**：`paho` 的 `granted` 是**元组** `(1,)`，
> 直接 `== 128` 永不成立 ⇒ 曾误报"收紧延迟 = None"、"既有订阅未撤销"。
> 本版统一取 `granted[0]` 为 int，并在 Part 2 中**同时对照新连接**，**结论以本版为准**。

### 测试环境

同 §0；账号 `workbuddy`；不发布消息；规则在 `finally` 中恢复并复验。

### 测试步骤

- **Part 1**（3 轮）：恢复原规则 → 基线确认允许 → `PUT` 收紧 →
  每 0.15 s 用**新连接**探测直到被拒 → 恢复 → 探测直到恢复。
- **Part 2**：建立**长连接**并订阅 → `PUT` 收紧 →
  用**新连接**确认"变更已全局生效" → 在**同一长连接**上再发起一次订阅，观察是否被拒。

### 实测结果

```
Part 1（新连接，3 轮）
  收紧生效延迟 = 1.36 / 1.35 / 1.35 s    (min 1.35  max 1.36  avg 1.35)
  恢复生效延迟 = 1.40 / 1.32 / 1.38 s    (min 1.32  max 1.40  avg 1.37)

Part 2（已连接客户端）
  长连接首次订阅 granted = 1（允许）
  PUT 收紧后，新连接首次被拒时刻 = 1.23 s   ← 证明 ACL 变更已**全局生效**
  同一长连接上的**新订阅** granted = 1（仍被允许）← 既有会话未被重新鉴权
```

### 结论

1. **延迟高度可重复**：收紧 ≈ **1.35 s**、恢复 ≈ **1.37 s**，**无长尾**（3 轮极差 ≤ 0.08 s）。
2. **★ 已连接客户端不立即生效 —— 存在按会话的授权缓存**：
   ACL 变更对**新连接**约 1.2~1.4 s 生效；但**既有会话**仍按旧授权继续放行，
   **连新的 `subscribe` 都不再重新校验**。

### 对 P1 设计影响（**重要，直接影响吊销语义**）

- **P1-2 签发时序**：等待余量 **≥2 s 足够**（实测 max 1.40 s）；保守可取 **3 s**。
- **P1-5 解绑 / 凭据吊销 / P1-6 在线状态**：
  **仅改 ACL 不足以让已连接设备失效** ⇒ 必须**同时断开其会话**
  （`DELETE /clients/{clientid}`，P0 期间已实测返回 **204**）。
  ⇒ **"吊销 = 改 ACL + 踢连接"** 应写入 P1 的凭据生命周期定义。
- **P1-3 轮换**：换凭据后若设备仍用旧会话在线，不会被立即切断 ⇒ 轮换流程需**显式重连 / 踢线**。

---

## A-6 `client.connected` / `disconnected` 事件能力

### 测试目的
判断能否**零设备开销**地获知设备上下线（决定 §2.6 在线状态方案）。

### 测试步骤
1. **API 探测**（只读）：`GET /rule_engine/rules` · `/rule_engine/sources` · `/bridges` · `/actions` · `/connectors` · `/authorization/sources`。
2. **官方文档核实**：EMQX Cloud 系统主题 / 数据集成 / 事件主题文档，以及 EMQX 仓库维护者答复。

### 实测结果

| 探测项 | 结果 |
|---|---|
| API `/rule_engine/rules` · `/rule_engine/sources` · `/bridges` · `/actions` · `/connectors` · `/authorization/sources` | ❌ **全部 `403`**（App 凭据权限不足）<br>（注：`/authorization/sources/built_in_database/rules/users` **可用** ⇒ 权限**按端点细分**） |

**文档 / 维护者结论（权威）**

| 结论 | 依据 |
|---|---|
| **Serverless 不支持订阅 `$SYS`**（**付费版同样不支持**） | EMQX 仓库讨论 #15559（维护者明确答复） |
| ✅ **支持经「数据集成（规则引擎）」捕获 `$events/client_connected` / `$events/client_disconnected`** | 官方最佳实践页《Capture Client Connection and Disconnection Event Topic Messages via Data Integration》 |
| **推荐做法**：Republish 到业务主题（如 `iot/events/client/+`），**业务端不要直接依赖 `$SYS` 结构** | 同上 |
| **关键字端**：`clientid` · `username` · `peername` · `connected_at` / `disconnected_at` · **`reason`** | 同上 |
| **`reason` 取值**：`normal` · `kicked` · `keepalive_timeout` · `not_authorized` · `tcp_closed` · `internal_error` · `discarded` · **`takenover`** | 同上 |
| Serverless 数据集成目标支持 **Kafka 与 HTTP**；Beta 期免费，之后**按 Action 执行次数计费** | EMQX 官方博客《Data Integration is Now Available in EMQX Cloud Serverless》 |

### 结论
1. **可以零设备开销地获得上下线事件** ✅ ⇒ §2.6 的"EMQX 连接事件优先"方案**成立**，无需退化为纯心跳推断。
2. 落地路径：**数据集成规则**（`FROM "$events/client_connected"` / `"$events/client_disconnected"`）→ **HTTP Action** → ingest Worker → D1。
3. ⚠️ **不能**用"订阅 `$SYS`"这条常见路径（Serverless 不支持）。

### 对 P1 设计影响
- **§2.6 在线判定确认为"事件优先 + `last_seen` 兜底"**，设备侧**无需**为此增加任何上报。
- **`reason` 字段可直接支撑诊断**：
  - `takenover` ⇒ **`client_id` 冲突**（正是 P0 修复的"互相踢线"，可作运行期告警）；
  - `kicked` ⇒ 被平台/API 主动断开（P0 实测中我们用 `DELETE /clients/{id}` 踢过设备）；
  - `keepalive_timeout` ⇒ 心跳/网络问题。
- ⚠️ **计费约束**：Action 按执行次数计费 ⇒ **只接低频率事件**（上下线各 1 次/次），
  **不要**把 `message_delivered` 等高频事件接入（会放大成本）。
- ⚠️ 登记待补：**实际创建**一条数据集成规则并验证端到端投递（本阶段仅核实能力，**未创建规则**）。

---

## A-7 EMQX User Creation Idempotency

### 测试目的

P1-2 的签发链路第一步是"建账号"。若该接口**不幂等**，Worker 任何一次重试/崩溃恢复都可能造成
"账号被改坏"或"重复建号"，进而让 `device_credential` 与 EMQX 真实状态**不一致**。
本项用于确定：**create user 的幂等语义**、**是否需要先 GET 再 CREATE**、**重试策略**，
以及能否采用 **at-least-once provisioning** 模型。

### 测试步骤

- **A-7.1 首次创建**：`POST /authentication/password_based:built_in_database/users`
  （body `{user_id, password, is_superuser:false}`）⇒ 记录 HTTP code / response body；
  再 `GET .../users/{user_id}` 确认存在性；**并用该凭据真实 MQTT CONNECT** 证明可用。
- **A-7.2 重复创建同名账号**：
  - **(a) 同名 + 同密码**：原样重发 ⇒ 记录 code/body；回读账号是否仍在、条数是否仍为 1；
    并在**重复创建前后各取一次全量 ACL 快照**，判定是否被改动。
  - **(b) 同名 + 不同密码**：改密码重发 ⇒ 记录 code/body；
    再用 **MQTT 建连反证**密码到底哪一个生效（无法从响应体判断）。
- **A-7.3 异常恢复**：模拟"create 请求已发出、Worker 崩溃/未消费响应"，随后**用同一 payload 原样重试**，
  检查：账号条数、凭据是否仍可用。
- **安全纪律**：只用**临时账号**（`a7test_*`），结束时**删用户 + 删其 ACL 规则**，
  并在**前后对比全量用户级 ACL 快照**，证明生产规则零改动。

### 实测结果

测试账号 `a7test_5n7w6cxz`（临时，已删除）；`AUTH_ID = password_based:built_in_database`；
ACL 基线 = 5 个用户（`GuoFeederDevice` 6 条 / `GuoFeederDevice001` 6 条 / `workbuddy` 1 / `test001` 1 / `shouji` 1）。

```
A-7.1  首次创建
  POST  users                     -> 201  {"is_superuser": false, "user_id": "a7test_5n7w6cxz"}
  GET   users/{u}                 -> 200  {"is_superuser": false, "user_id": "a7test_5n7w6cxz"}
  MQTT  CONNECT(密码 P1)           -> 成功（rc = Success）        ← 新账号立即可用
  PUT   rules/users/{u}           -> 204                        ← 写 1 条测试规则（预备 A-7.2）

A-7.2a 重复创建（同名 + 同密码）
  POST  users                     -> 409  {"code": "ALREADY_EXISTS", "message": "User already exists"}
  GET   users/{u}                 -> 200  （账号仍在）
  同名账号条数                     = 1                          ← 未重复建号
  ACL 快照                        = 与重复创建前**逐条一致**      ← 未改动 ACL

A-7.2b 重复创建（同名 + **不同**密码）
  POST  users                     -> 409  {"code": "ALREADY_EXISTS", "message": "User already exists"}
  MQTT  CONNECT(密码 P2, 新)       -> **失败**（Bad user name or password）
  MQTT  CONNECT(密码 P1, 原)       -> 成功
  ⇒ **密码未被覆盖**（创建接口对已存在账号是"整体拒绝"，不是 upsert）

A-7.3 异常恢复（模拟 Worker 崩溃后重试）
  POST  users（响应被丢弃）         -> 409 ALREADY_EXISTS
  POST  users（原样重试）           -> 409 ALREADY_EXISTS
  同名账号条数（重试后）             = 1
  MQTT  CONNECT(当前生效的 P1)      -> 成功                      ← 重试**未破坏**已有凭据
  ⇒ at-least-once provisioning **可行**

附加（供 P1-2 回收后重签参考）
  DELETE rules/users/{u}          -> 204
  DELETE users/{u}                -> 204
  GET    users/{u}                -> 404  {"code": "NOT_FOUND", "message": "User not found"}
  POST   users（删除后同名重建）    -> 201                        ← 回收后可重签
  MQTT   CONNECT(重建后的新密码)    -> 成功
  ⇒ 生产 ACL 与基线**逐条一致**（5 用户，规则数不变）
```

### 结论

1. **create user 不是 upsert，而是"首次成功 / 之后一律 409"**：
   `201` + body `{user_id, is_superuser}`；已存在 ⇒ `409` + `{"code":"ALREADY_EXISTS"}`。
2. **已存在账号不受任何影响**：密码**不覆盖**、`is_superuser` 不变、**ACL 规则不动**、**不重复建号**。
   ⇒ 对 Worker 而言，`409 ALREADY_EXISTS` 是**可安全当成功处理**的确定性信号。
3. **无需 "先 GET 再 CREATE"**：`409` 本身就是判别号；多一次 `GET` 只增加往返与"check-then-act"竞态窗口。
4. **回收后可同名重建**（`DELETE` 后 `POST` 仍 `201`）⇒ 轮换/退役后的重签路径成立。
5. `DELETE .../rules/users/{username}` **存在且返回 204**（此前 `emqx-api.mjs` 未封装）⇒ 回收路径可直接删规则。

### A-7.4 输出 Design Impact

见紧随其后的 **§A-7 Design Impact**。

---

## A-7 Design Impact

> **本节与 §Design Impact Freeze 同为冻结区**，专门约束 P1-2 的"建账号"一步。

| # | 问题 | 冻结结论 |
|---|---|---|
| 1 | **create user 是否天然幂等？** | **是（幂等且不可变）** —— 重复创建返回 `409 ALREADY_EXISTS`，且**不覆盖密码 / 不改 ACL / 不重复建号**。语义上等价于 "INSERT ... ON CONFLICT DO NOTHING"。 |
| 2 | **Worker 是否需要先 GET 再 CREATE？** | **不需要**。直接 `POST`；把 **`409 ALREADY_EXISTS` 视为成功**（`201` 同样视为成功）。避免 check-then-act 竞态。 |
| 3 | **重试策略** | **可盲重试（blinded retry）**：`201` / `409` 均判成功；网络异常（超时、连接中断）**按可重试处理**。建议上限 **5 次**、指数退避（1s/2s/4s/8s/16s）；超出后落 `provision_error`（`emqx_create_user_timeout`）并进入 §对账。 |
| 4 | **对 credential generation 的影响** | **无影响** —— 账号名 `dev_<device_id>` 与 generation **解耦**：回收旧凭据走 `DELETE`，重建同名账号仍 `201`（本项附加实测已验证）。⇒ **同一 device 的轮换可以复用同一个 `username`**，`generation` 只用于**设备侧与 D1 的对账/防重放**，不需要把 generation 编进账号名。 |
| 5 | **对 `PROVISIONING` 状态的影响** | "建账号"这一步**不再是不可恢复的失败点**（可盲重试）⇒ `PROVISIONING` 的失败原因将主要来自 **ACL 侧**（§Design Impact Freeze 的传播窗口）与**设备未自证**，而不是账号创建。 |
| 6 | **是否可采用 at-least-once provisioning？** | **可以采用**（A-7.3 实测：崩溃 + 原样重试后账号条数仍为 1、凭据仍可用）。⚠️ 前提：**ACL 写入（`PUT`）本身也是幂等全量覆盖**（P0 已验证），且**下发命令每次换新 `i`**（避免被 30 s 去重缓存丢弃）。 |

**⚠️ 一条必须写清的边界**：A-7 只证明 **authn（账号）** 侧幂等。
**authz（ACL）** 侧是另一条链路（`PUT` 全量覆盖 + **约 1.3 s 传播窗口**），
两者**不可互推** —— 账号已建 ≠ 凭据可用。完整判据见 §Design Impact Freeze 与
`P1-2-P1-4-注册与凭据签发设计.md` §6.3。

---

## Design Impact Freeze

> **本节为 P1-0 结论的冻结区。** 数据来源：§A-1c（3 轮复测 + 已连接客户端行为；**以该节为准**）。
> **本节内容是 P1-2 / P1-3 / P1-5 的设计约束，不可被后续实现覆盖** —— 若要偏离，须先在本文更新本节并说明理由。

### ACL propagation behavior

**实测**（`workbuddy` 账号，规则改回并在 `finally` 复验）：

- ACL 修改后，**新连接**约 **1.2~1.4 秒**后受到新规则影响
  （收紧 **1.36 / 1.35 / 1.35 s**，avg 1.35；恢复 **1.40 / 1.32 / 1.38 s**，avg 1.37；极差 ≤ 0.08 s，**无长尾**）。
- **已建立 MQTT session 不立即重新评估 ACL。**
- **已连接客户端继续拥有旧 session 权限** —— 实测中收紧后，同一长连接即使发起**新的 `subscribe`** 仍 `granted=1`，
  而同时刻的新连接已在 1.23 s 被拒 ⇒ **变更确已全局生效，只是不作用于既有会话**。

**因此：**

> **ACL 修改不是完整 revoke。**

完整 revoke **必须**：

```
update ACL
+
terminate existing session        # DELETE /clients/{clientid}，P0 期间实测返回 204
```

两者缺一不可：只改 ACL ⇒ 已连接设备继续可用（最长直到自然断线）；
只踢连接 ⇒ 设备会带原凭据立即重连并成功。

**⇒ 本条写入 P1 凭据生命周期定义：revoke = ACL change + session termination（两个动作均需可观测、可重试、可对账）。**

### 对 P1-2 影响

credential issuance 流程**不得只依赖**：

```
EMQX API success
```

`POST /users` 与 `PUT .../rules/users/{u}` 返回 2xx **只代表规则已写入配置**，
**不代表规则已生效** —— 中间存在 §A-1c 实测的传播窗口。

必须考虑 **ACL propagation window**。签发流程必须具备：

| 要求 | 说明 |
|---|---|
| **ACL ready check** | 不能以"API 返回成功"作为 ready 判据；需有独立的**就绪判定**（判定方式在 P1-2 实施时定，**不在本文冻结**） |
| **retry / wait** | 未 ready ⇒ 等待后重试，而非直接判定失败。**等待余量实测 ≥2 s 足够（max 1.40 s），保守取 3 s** |
| **可检测失败状态** | 必须能区分 `OK` / `PENDING` / `FAILED`，且状态**可被查询与告警**；禁止出现"签发成功却静默不可用"的中间态 |

> ⚠️ **半成品状态必须可检测**：账号创建成功但 ACL 未就绪（或写入失败）的设备不得被标记为可用。
> P1-2 的 D1 凭据记录需能表达该中间态（字段设计见 P1-1 的 `device_credential`）。

### 对 P1-3 影响

credential rotation（固件侧双 slot 模型 `ACTIVE / TESTING / INVALID`）：

**不得直接：**

```
new credential active
old credential revoke
```

⇒ 这条路径在新凭据**尚未验证连通**时就废弃了旧的，一旦失败设备即失联。

**必须：**

```
new credential TESTING
        ↓
connection verification      # CONNACK 成功才算通过；网络失败（超时/DNS/TLS）不得误判为凭据失败
        ↓
ACTIVE
        ↓
old credential revoke        # 改 ACL
        ↓
terminate old sessions       # ← 本节点引入：否则旧会话按 §ACL propagation behavior 继续存活
```

**⇒ "terminate old sessions" 是本次 A-1c 新增的强制步骤**，原 P1-3 定义中缺失。
轮换完成后必须显式重连 / 踢线，不能假定"改了 ACL 就等于旧凭据失效"。

### 对 P1-5 影响

owner transfer / revoke（绑定与解绑语义）：

必须**同时**考虑三个层面（缺一即出现"数据已解绑但设备仍被控"或反之）：

| 层面 | 位置 | 说明 |
|---|---|---|
| **database binding state** | D1 `device_binding` | 归属关系的**唯一权威**（active 绑定唯一约束） |
| **MQTT authorization** | EMQX ACL | 决定能否收发 `guo_feeder/<device_id>/{down,up,log}` |
| **existing MQTT sessions** | EMQX 连接 | **本小节新增**：已连接会话不随 ACL 变更重鉴权 |

转让完成后需要：

```
update binding
+
update ACL
+
terminate old control sessions     # ← 本节点引入
```

> **不变的既有冻结项**：`BOUND → UNBOUND`（解绑）**不吊销 credential** —— 归属关系 ≠ MQTT 身份；
> 仅 `UNBOUND → REVOKED`（退役）才触发上述三步组合。

---

## 汇总：对 P1 设计的影响

| # | 结论 | 对 P1 的影响 | 是否阻塞 |
|---|---|---|---|
| **A-4** | `client_id` 上限 8K~16K；字符集近乎不限；空 `client_id` 被接受 | P0 的 ≤128 + `[0-9a-zA-Z_-]` **保守安全，无需调整**；固件侧字符集校验是**唯一防线**，保留 | ✅ 不阻塞 |
| **A-5** | 账号可程序化创建/删除；上限约 2000 | **"一机一账号"可行** ⇒ 凭据模型成立；**P1 不需要 slot 池**（APP 授权已延后） | ✅ **支撑** |
| **A-1** | 授权变更**秒级生效**（≈1.3 s）；**但仅作用于新连接**（§A-1c：既有会话不重鉴权） | 签发流程需**等 ≥2 s**（或连接失败重试）；**R-2 风险大幅降级**；⚠️ **吊销类操作另见 §Design Impact Freeze** | ✅ **解除阻塞**（但**不覆盖** revoke 语义） |
| **A-6** | **不支持 `$SYS`**；**支持** `$events/client_connected/disconnected` 经数据集成 | 在线状态**零设备开销**方案确认；落地 = 数据集成规则 → HTTP Action → Worker | ✅ **支撑** |
| **A-7** | create user **幂等且不可变**（重复 ⇒ `409 ALREADY_EXISTS`，不覆盖密码 / 不改 ACL / 不重复建号）；删除后可同名重建 | P1-2 **无需 GET+CREATE**，`409` 直接当成功；**可盲重试**（上限 5 次）⇒ **at-least-once provisioning 成立**；`username` 不必带 generation | ✅ **支撑** |

**⇒ P1-0 的五项均未发现"设计不可行"结论；A-1 的秒级延迟与 A-7 的幂等性共同解除了签发链路的时序与重试顾虑。**

> ⚠️ **但 A-1c 限定了该结论的适用范围**：秒级生效是对**新连接**而言。
> 凡涉及**让某个已在线身份失效**的操作（P1-2 失败回滚 / P1-3 轮换 / P1-5 转让与退役），
> **统一受 §Design Impact Freeze 约束**：`ACL change` 必须与 `session termination` 成对执行。

---

## 遗留 / 待补测（不阻塞 P1 启动）

| # | 事项 | 何时补 |
|---|---|---|
| ~~1~~ | ~~`client_id` 上限精确值~~ ✅ **已补测**（§A-4b = 15323 / 15324） | — |
| 2 | **账号创建速率 / API 调用配额** | P1-2 实施前（若批量注册） |
| ~~3~~ | ~~A-1 多次重复测量~~ ✅ **已补测**（§A-1c 3 轮，无长尾） | — |
| 4 | **实际创建数据集成规则**并验证端到端投递（含 `reason` 字段） | Phase F（Online State） |
| 5 | `$events/client_connected` vs `$events/client/connected` **主题名差异**（两版文档写法不同） | Phase F 实施时以控制台为准 |
| ~~6~~ | ~~create user 的幂等语义~~ ✅ **已补测**（§A-7） | — |
| 7 | **`POST /users` 是否有速率限制**（连续创建时的 `429` 行为） | P1-2 实施前（若批量注册） |

---

## 附：实测脚本（不入库）

| 脚本 | 覆盖 |
|---|---|
| `.pio/p0run/p1_0_a4.py` | A-4（连接级边界） |
| `.pio/p0run/p1_0_a4b.py` | A-4b（上限二分 + 失败形态判定） |
| `.pio/p0run/p1_0_a56.py` | A-5 / A-6（账号与事件 API 探测） |
| `.pio/p0run/p1_0_a1.py` | A-1（授权生效延迟，含规则恢复） |
| `.pio/p0run/p1_0_a1c.py` | A-1c（3 轮复测 + 已连接客户端；**结论以此为准**） |
| `.pio/p0run/p1_0_a7.py` | **A-7**（create user 幂等性 / 密码是否被覆盖 / 崩溃重试 / 回收后重签）—— 报告落 `.pio/p0run/p1_0_a7_report.json` |
| `.pio/p0run/p1_0_a1b.py` | ⚠️ **已作废**（`granted` 元组比较错误，结论不可采信） |
