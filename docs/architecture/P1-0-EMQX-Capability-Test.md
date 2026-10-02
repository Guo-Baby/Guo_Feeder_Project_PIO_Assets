# P1-0 EMQX 平台能力实测（Capability Test）

> **状态**：**已完成（2026-10-02）· 待人工审核**
> **上游**：`P1-Cloud-Device-Lifecycle-Plan.md`（§5.1 冻结的实测顺序）
> **顺序**：**A-4 → A-5 → A-1 → A-6**（A-4 决定 credential 格式 → A-5 决定账号模型 → A-1 决定授权生效流程 → A-6 只影响在线状态实现）
> **本文性质**：**实测记录**，不含任何实现代码。所有测试脚本位于 `.pio/p0run/`（不入库）。

---

## 0. 测试环境（四项共用）

| 项 | 值 |
|---|---|
| 平台 | **EMQX Cloud Serverless** |
| 部署 | `n302933b.ala.cn-hangzhou.emqxsl.cn`（mqtts `8883`） |
| 管理 API | `https://…:8443/api/v5`（**App ID/Secret**，Basic Auth） |
| MQTT 客户端 | `paho-mqtt`（Python），TLS + CA `data/emqxsl-ca.crt` |
| 凭据来源 | API 凭据来自 `EMQX_Assets/.env`（不入库）；MQTT 测试账号用 **`workbuddy`**（**非设备账号** ⇒ 不会踢掉在线设备） |
| 在线设备 | 1 台（`username=GuoFeederDevice`，`client_id=dev_<device_id>`）—— 测试全程未触碰其凭据 |

> **安全声明**：除 A-1 外均为只读或"连接尝试"；A-1 对 `workbuddy` 的规则做了**临时收紧并已在 finally 中恢复**（恢复亦经实测确认生效）。**未改动设备账号凭据、未改动兜底规则、未创建任何长期对象**。

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
- ⚠️ 登记待补：`client_id` 上限的**精确值**（二分定位，非 P1 阻塞项）。

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
- ⚠️ **单次测量，存在噪声** ⇒ 登记待补：多次重复测量 + 观察是否有长尾（P1 实施时可复测）。

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

## 汇总：对 P1 设计的影响

| # | 结论 | 对 P1 的影响 | 是否阻塞 |
|---|---|---|---|
| **A-4** | `client_id` 上限 8K~16K；字符集近乎不限；空 `client_id` 被接受 | P0 的 ≤128 + `[0-9a-zA-Z_-]` **保守安全，无需调整**；固件侧字符集校验是**唯一防线**，保留 | ✅ 不阻塞 |
| **A-5** | 账号可程序化创建/删除；上限约 2000 | **"一机一账号"可行** ⇒ 凭据模型成立；**P1 不需要 slot 池**（APP 授权已延后） | ✅ **支撑** |
| **A-1** | 授权变更**秒级生效**（≈1.3 s） | 签发流程只需**等 ≥2 s**（或连接失败重试）；**R-2 风险大幅降级** | ✅ **解除阻塞** |
| **A-6** | **不支持 `$SYS`**；**支持** `$events/client_connected/disconnected` 经数据集成 | 在线状态**零设备开销**方案确认；落地 = 数据集成规则 → HTTP Action → Worker | ✅ **支撑** |

**⇒ P1-0 的四项均未发现"设计不可行"结论；A-1 的秒级延迟反而解除了此前最大的时序顾虑。**

---

## 遗留 / 待补测（不阻塞 P1 启动）

| # | 事项 | 何时补 |
|---|---|---|
| 1 | `client_id` 上限的**精确值**（8192~16384 之间二分） | 可选 |
| 2 | **账号创建速率 / API 调用配额** | P1-2 实施前（若批量注册） |
| 3 | **A-1 多次重复测量**（本次为单次，观察长尾） | P1-2 实施时 |
| 4 | **实际创建数据集成规则**并验证端到端投递（含 `reason` 字段） | Phase F（Online State） |
| 5 | `$events/client_connected` vs `$events/client/connected` **主题名差异**（两版文档写法不同） | Phase F 实施时以控制台为准 |

---

## 附：实测脚本（不入库）

| 脚本 | 覆盖 |
|---|---|
| `.pio/p0run/p1_0_a4.py` | A-4（连接级边界） |
| `.pio/p0run/p1_0_a56.py` | A-5 + A-6（API 探测） |
| `.pio/p0run/p1_0_a1.py` | A-1（授权生效延迟，含规则恢复） |
