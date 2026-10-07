# P1-8 Acceptance Decision Closure + N-3 Remediation Design

> **性质**：人工裁决落地 + 修复设计审查。**本轮未修改任何业务代码、未执行任何生产写操作、未 commit / 未 push。**
> **状态**：`N-1 = RULED（DEFERRED TO P3）` · `N-3 = RULED（DoD-D6 BLOCKER，修复方向已定；设计待审）` · `Fixture = FROZEN` · `N-3 Remediation = DESIGN ONLY（未编码）`
> **基线**：Firmware `e36d5ac`(wb) · EMQX `080cb3c`(main) · Cloudflare `97ca496`(main) · Worker `f10e29a4-f5dc-4f5e-b3ec-890cf59369b2`

---

## 1. N-1 落地 —— `fw_version` = `DEFERRED TO P3`

### 1.1 裁决（已执行）

P1 不实现 `fw_version` 上报；`online / offline / last_seen` 仍为 **P1 acceptance**；`fw_version` 判据 **DEFERRED TO P3**。

### 1.2 已修改的正式文档（**逐处，未删除历史要求**）

| 文件 | 位置 | 改法 |
|---|---|---|
| `P1-实现清单.md` | §0.1 末尾 | **新增裁决注**：说明 `fw_version` 物理来源是心跳（P3）、P1 允许 `device.fw_version = NULL`、云端入口已就绪 |
| `P1-实现清单.md` | §P1-6「目标」（:141） | `online/offline/last_seen` 标 **P1 acceptance**；`~~fw_version~~ **DEFERRED TO P3**` |
| `P1-实现清单.md` | §P1-6「验证」（:145） | `~~fw_version 可见~~ **DEFERRED TO P3**`（P1 允许 `NULL`） |
| `P1-实现清单.md` | §3.1 **C-8**（:176） | 追加「**`fw_version` 判据 = DEFERRED TO P3（不参与 P1 验收）**」 |
| `P1-实现清单.md` | §4 **DoD-D4**（:209） | 判据收窄为「在线否 / 最后活跃（**P1**）」+「固件版本 = **DEFERRED TO P3**」 |
| `P1-8-Test-Matrix.md` | 矩阵 A **C-8** | 同上口径 + 状态改为「已裁决延至 P3」 |
| `P1-8-Test-Matrix.md` | 矩阵 D **D4** | 同上口径 + 「已裁决延至 P3」 |
| `P1-8-Test-Matrix.md` | §9 N-1 | 状态改为 **✅ 已裁决** |
| `P1-8-Test-Matrix.md` | §10 结论 | 同步 |

> **历史要求保留**：原文用删除线保留，未删除；未伪装成"P1 已实现"；P3 的既有架构归属（`P0-实现清单.md` §0.3、`P1-Cloud-Device-Lifecycle-Plan.md` §2.6）**未改动**；未顺带修改其他 P1/P3 边界；未改固件代码。

### 1.3 冲突引用复查（全文搜索）

对 `P1-实现清单.md` / `P1-8-Test-Matrix.md` 全文搜索 `fw_version`：**所有 P1 判据条目均已带 `DEFERRED TO P3` 说明**；
`P1-Cloud-Device-Lifecycle-Plan.md:263`（`fw_version` 随心跳上报、**与 §六 Device State Heartbeat 同批落地**）本即属 **P3 语境**，**无需修改**（它是 P3 归属的**依据**，不是 P1 判据）。
⇒ **未发现"P1 必须证明 `fw_version` 正确"而又无 `DEFERRED TO P3` 说明的矛盾条目。**

---

## 2. N-3 Remediation Design（**设计稿，未编码**）

### 2.0 链路精确取证（只读代码 + 只读 D1）

```
credential.js::provisionCredential()          ← 生成 password（CSPRNG）
   └─ publishToDevice(env, "guo_feeder/<id>/down", downlink, {qos:1})   [emqx-admin.js:344]
        └─ EMQX REST  POST /api/v5/publish  {topic, payload, qos, retain}
             └─ EMQX Rule：`guo_feeder/#` 消息 → HTTP Action → Worker webhook
                  └─ index.js::fetch  (ingest)
                       ├─ source   = body.source || "mqtt"                      [:355]
                       ├─ topic    = body.topic                                 [:402]
                       ├─ payload  = body.payload || ""                         [:410]
                       ├─ direction= /down ⇒ "down"                             [:452-460]
                       ├─ msg      = JSON.parse(payload) | payload              [:471]
                       ├─ payload_json = JSON.stringify(msg.p)                  [:507-514]  ← 含 password
                       └─ INSERT mqtt_messages ... raw_payload = JSON.stringify(payload)  [:529 / :574]  ← 含 password
```

| 问题 | 结论（取证） |
|---|---|
| ① `credential_set` 真实下发 payload | `{c:"system", i:"cred_<id>_g<n>_<rand>", t:<sec>, p:{o:"credential_set", action:"install", username:"dev_<id>", password:"<32 字符密钥>", generation:N, slot:N}}`（实测 `raw_payload` 头 `{"c":"system","i":"cred_a1b2..."}`，`password` 位于第 ~140 字符处） |
| ② EMQX 回投为何进 Worker | EMQX Rule 订阅 `guo_feeder/#` 并 Action → Worker webhook；**该 Rule 同时涵盖上/下行**（E-3 §11.B 已登记为既有系统行为） |
| ③ Worker 如何判 `direction="down"` | **纯按 topic 后缀**：`topic.endsWith("/down")` ⇒ `"down"`（`:460`），**与消息来源无关** |
| ④ `raw_payload` 如何生成 | `JSON.stringify(payload)`（`payload = body.payload`）——**原样**（`:574`）；另有 `payload_json = JSON.stringify(msg.p)`（`:514`） |
| ⑤ 最小脱敏点 | **`index.js` ingest 落库前**（见 §2.A） |
| ⑥ 脱敏是否影响真实下发 | **不影响**（见 §2.A 末） |
| ⑦ 其他明文路径 | 见 §2.1 |

### 2.1 ★ 追加发现：明文路径**不止一条**（仍需裁决）

生产 D1 只读实测：

| 路径 | 载体 | 实测 | 密钥性质 |
|---|---|---|---|
| **① 下行 `credential_set` 回显** | `mqtt_messages.raw_payload` **与** `payload_json` | `direction='down'` **493** 行；`device_id='http_api'` **56** 行含 `password`；`payload_json` 含 `password` **22** 行 | **每设备 MQTT 凭据**（P1 核心密钥） |
| **② 上行 `config_query` 响应** | `mqtt_messages.raw_payload` | **32** 行含 `password` | **`wifi.password`（WiFi WPA）+ `mqtt.password`（P0 遗留共享凭据）** |
| **③ Worker 运行日志** | Cloudflare 日志 | `index.js:348` `console.log(body)` 打印**整个 ingest body**（含 payload） | 同上 |
| ④ `device_event.payload` | — | 含 `password` = **0** 行 | 干净 |
| ⑤ D1 其他表 | `device` / `device_credential` / `device_binding` | **DDL 无 password/secret 列**（已逐表核对） | 干净 |
| ⑥ 固件串口 | 本机日志 | 当前固件**已脱敏**（`(hidden)`）；**108 个历史 `.pio` 日志**含旧共享凭据（N-2，已 gitignore） | 旧共享凭据 |
| ⑦ 仓库文件 | git 跟踪文件 | 10 个受跟踪文件含明文（P0-2 既有待办，含 `docs/interfaces/cloud_protocol.md` / `tools/mqtt_*.py`） | 共享凭据 |

> ⚠️ **路径 ②③ 的密钥性质与 ① 不同**（① = 每设备凭据；② = WiFi 口令 + 遗留共享凭据）。**是否一并脱敏需人工裁决**（见 §2.E）。这属于"凭据密码以外的额外明文路径"，**本轮只报告，不自行决定范围**。

### 2.A 推荐最小修复点

| 项 | 内容 |
|---|---|
| **文件** | `Cloudflare_Assets/guo-feeder-api/src/index.js` |
| **函数/位置** | ingest 处理函数（`export default { async fetch }` 内）——`msg` 解析之后（`:471`）、构建 `payload_json`（`:507`）与 INSERT（`:529`）**之前** |
| **当前行为** | `raw_payload = JSON.stringify(payload)`（`:574`）与 `payload_json = JSON.stringify(msg.p)`（`:514`）**原样**保存含 `password` 的帧 |
| **建议行为** | 新增**纯函数** `redactSecrets(v)`（深拷贝并替换敏感字段值），落库时改用 `JSON.stringify(redactSecrets(payload))` 与 `JSON.stringify(redactSecrets(msg.p))`；**其余列不变** |
| **为什么最小** | 仅 1 个文件、2 处绑定值 + 1 个纯函数；**不动** `credential.js` / `emqx-admin.js` / 协议 / schema / 其他通路 |
| **为什么不动真实下发** | 真实 MQTT 下发由 `credential.js → publishToDevice() → EMQX /api/v5/publish` **独立完成**；ingest 只是 **EMQX 回投的镜像**。**改写落库副本，绝不影响发往设备的报文**；`cloud_api` 调试通路的 `publishToEMQX()`（`:160`）亦不受影响 |
| **附带建议（最小）** | 将 `index.js:348` 的 `console.log(body)` 改为**脱敏摘要**（打印 topic/direction/object/msg_id 等，**不含 payload**），消除路径 ③ |

**安全性核对（脱敏后仍满足）**：

| 断言 | 依据 |
|---|---|
| MQTT 实际发给设备的 password 不变 | 下发与落库是**两条独立代码路径**；脱敏只改落库副本 |
| device credential install 不受影响 | install 由设备侧解析下行帧完成，与 Worker 落库无关 |
| ACK 不受影响 | ACK 走设备 `…/up` ⇒ ingest（`type=credential_confirm`），payload 无 password；脱敏不触及上行确认分支逻辑 |
| credential rotation 不受影响 | 轮换复用同一 `provisionCredential()` 下发路径 |
| revoke 不受影响 | `revokeDevice()` / `hardRetireOne()` 不写 `mqtt_messages` payload |
| 普通 MQTT message audit 仍存在 | 所有行**照常落库**，只有 `password` **值**被替换为固定占位（帧结构、`cmd_id`、`object`、时间等**全保留**） |

### 2.B 脱敏规则（**依据实际代码/数据，非假设**）

对 `credential_set` 下行帧：

| 字段 | 处置 | 理由 |
|---|---|---|
| `p.password` | **必须脱敏**（值 → 固定占位，如 `"<redacted>"`） | 唯一密钥字段 |
| `p.username` | **保留** | 非密钥（`dev_<device_id>`，可由 `device_id` 派生，且已存在于 `device_credential.username`） |
| `p.o` / `p.action` | **保留** | 审计必需（标识这是 credential_set / install） |
| `p.generation` / `p.slot` | **保留** | 审计必需（对应凭据世代 / slot） |
| `c` / `i`（cmd_id） / `t` | **保留** | 审计必需 |
| 列 `device_id` / `topic` / `direction` | **保留** | 既有列，不含密钥 |

对 `config_query` **上行**帧（若裁决纳入）：仅脱敏 `data.*.password`（`wifi.password` / `mqtt.password`），其余保留。见 §2.E。

**规则实现建议**：递归替换**任何名为 `password` 的键**的值 → 占位符。**理由**：① 覆盖上下行两条路径；② 未来新增字段若叫 `password` 自动受保护；③ 当前代码库中除上述两处外**无**其他合法的 `password` 字段（已核）。**不改键名、不改结构**（保持审计可读）。

### 2.C 历史数据处理（**本轮严禁执行**）

| 问题 | 结论 / 建议 |
|---|---|
| 1. 是否必须清理 | **建议处理**：生产 D1 现存明文（① ≥56 + ② 32 行）属真实密钥暴露。 |
| 2. 最小安全方式 | **首选「阻止未来 + 凭据轮换」** —— 轮换后历史明文**即失效**（P0 遗留共享凭据与各 device 旧世代密码作废），**无需改动 append-only 历史行**。次选：对含 `password` 的行做**定向 UPDATE** 替换为占位（**但违背 `mqtt_messages` append-only 语义，且属生产数据写，须单独授权**）。 |
| 3. 是否可仅阻止未来 | **技术上可以**（§2.A 修复即达）；但**残留明文仍可读**，故建议与**凭据轮换**（既有待办「MQTT 密码轮换」）绑定执行，使历史明文失去价值。 |
| 4. 是否应作为独立人工授权数据操作 | **是**（任何 `UPDATE`/`DELETE` 生产行必须单独授权）。 |
| 5. 清理后如何证明无残留 | `SELECT COUNT(*) FROM mqtt_messages WHERE raw_payload LIKE '%password%' OR payload_json LIKE '%password%'` ⇒ 期望 **0**（或仅占位符）；配合 `grep` 负向断言与 `wrangler tail` 观察窗口。 |

> **推荐**：**先做 §2.A（阻止未来）→ 再凭据轮换（使历史明文失效）→ 视裁决决定是否 UPDATE 历史**。避免把"改历史 append-only 数据"作为默认动作。

### 2.D 回归测试设计

> 全部为**设计**；执行需另行授权。`需生产` = 是否必须在部署版 Worker + 生产 D1 上跑；`destructive` = 是否改动生产数据/设备。

| # | 测试 | 前置条件 | 操作 | Expected | 需生产 | destructive |
|---|---|---|---|---|---|---|
| R-1 | 新凭据签发不落明文 | 修复已部署；一台**一次性测试设备** | 走 `/api/ingest → claim → credential` | `mqtt_messages` 新增下行行 **不含非空 `password`**（帧结构/`cmd_id`/`username` 仍在）；设备仍收到**真实可用**凭据 | 是 | 否（用一次性设备） |
| R-2 | 凭据轮换不落明文 | R-1 通过 | 触发轮换（新世代） | 同上；旧世代按策略失效 | 是 | **是**（轮换 = 生产凭据写） |
| R-3 | 凭据 revoke 不落明文 | R-1 通过 | 对**一次性设备** revoke | 无新明文；ACL 收紧 + 踢会话成对 | 是 | **是**（revoke 不可逆） |
| R-4 | device credential install 正常 | 一次性设备 + 真机/仿真客户端 | 设备接收下行并安装 | 设备侧 NVS `gfcred` 得新凭据并连通 | 是 | 否 |
| R-5 | MQTT downlink 正确性 | mqttx 订阅 `guo_feeder/<id>/down` | 触发签发 | mqttx **收到的报文含真实 password**（**证明脱敏只作用于落库副本**） | 是 | 否 |
| R-6 | `mqtt_messages` 无明文 password | R-1/R-2/R-3 后 | `SELECT COUNT(*) ... LIKE '%password%'` | 新增行 = **0**（历史行按 §2.C 处置） | 是 | 否（只读） |
| R-7 | `device_event` 无 password | 同上 | `SELECT COUNT(*) FROM device_event WHERE payload LIKE '%password%'` | **0** | 是 | 否（只读） |
| R-8 | Worker / 串口日志无 password | `wrangler tail` 窗口 + 串口监视 | 走一遍签发 | 日志仅见**脱敏摘要**；串口 `(hidden)` | 是 | 否 |
| R-9 | ACK 正常 | 真机或 mqttx | 签发后观察 `…/up` | `credential_confirm` ACK 正常，凭据升 `ACTIVE` | 是 | 否 |
| R-10 | device reconnect 正常 | 真机 | 签发后重启设备 | 用新凭据重连成功，`/clients` `connected=true` | 是 | 否（重启设备 = 窗口操作） |
| R-11 | 静态负向（离线） | 无需生产 | `grep` 源码 + 契约套件 | 无新增 `console.log(payload)`；Cloudflare 契约套件全绿（0 FAIL / 0 UNEXPECTED_PASS） | **否** | 否 |

### 2.E ★ 需人工裁决的范围问题

1. **路径 ②（`config_query` 上行）是否纳入本次脱敏？** —— 其密钥是 `wifi.password` 与遗留共享 `mqtt.password`（**不是**每设备凭据）。
   - **纳入**：一条通用规则覆盖全部；但扩大了 N-3 的原始范围（N-3 原指 `credential_set`）。
   - **不纳入**：N-3 聚焦每设备凭据；`config_query` 另行立项（且设备侧 `config_query` 有 `keys_only:true` 安全模式可改默认）。
2. **路径 ③（`console.log(body)`）是否一并清理？** —— 建议**一并**（成本极低、且属"落日志"违反 D6）。
3. **路径 ⑦（10 个受跟踪文件明文）** 属既有待办 **P0-2**，**不建议**并入 N-3（涉及 git 历史，无法靠单次提交清除）。

> **本轮不自行决定 ①②③ 的范围。**

---

## 3. L-9 表名勘正（已完成）

**问题**：`P1-6-3-E-closure-review.md` 的 **L-9** 原文写作「**`device_event`** 明文持久化凭据密码（下行回显行）」。

**只读核对结果**：
- `device_event` 含 `password` = **0 行**（生产 D1 实测）；
- 真正承载明文的是 **`mqtt_messages.raw_payload`**（`E-3 §11.C` 正文**本就正确**写明 `mqtt_messages`，仅 L-9 的**表名**表述不精确）。

**已做修正**（仅 L-9 一行，**未改其他历史审计内容**）：
- 表名改为 **`mqtt_messages.raw_payload` / `payload_json`**；
- 明确写「**安全结论不变，仅承载表名此前不精确**」；
- 追加「P1-8 已将其升级为 `DoD-D6` BLOCKER 并进入修复设计」（状态更新，不改 E 的历史结论）。

---

## 4. P1-5-5 Fixture Freeze（已批准 · 快照）

| fixture | 范围 | 终态 |
|---|---|---|
| **P5-A**（新建一次性，`REGISTERED`） | T-1·T-2·T-3·T-4·T-5·T-6·T-7·T-8·T-9·T-9b·T-12·T-13·T-14·T-16 | **不 REVOKE**；可控状态 |
| **P5-B**（新建一次性，`REGISTERED`） | **仅 T-15** | `BOUND → REVOKED`（**独立牺牲，不可逆**） |
| **P5-C**（新建一次性，`REGISTERED`） | **仅 T-20** | `REGISTERED + 无 binding → REVOKED`（**独立牺牲，不可逆**） |
| `3cfb56c8a5fc`（`CLAIM_PENDING`） | **仅 T-10**（只读；不得 claim/bind/rotation/revoke） | 不变 |
| `d163d0000001`（`REVOKED`） | **仅 T-11**（只读；不得改状态） | 不变 |
| `288485896ce4`（生产真机） | **仅 T-17 只读观测**；**禁止**为 T-17 主动 `bind → unbind` | 不变 |

**执行顺序**：先 T-12（绑 A/B/C）→ 解绑 P5-C → T-15（revoke P5-B）→ T-20（revoke P5-C）。

**★ T-17 前置（待裁决）**：只读实测 `288485896ce4` = `state=REGISTERED` + **0 active binding** + **online**（`dev_288485896ce4 conn=true ka=120 pv=4`）。
⇒ 功能上等价"无主"，但**字面不是 `UNBOUND`**。若测试代码要求字面 `UNBOUND` ⇒ 前置不满足：

```
T-17 fixture prerequisite not satisfied
```

**不得自行修改生产设备状态**，等人工裁决（① 接受等价前置 / ② 授权生产 bind→unbind【写】 / ③ 延后 T-17）。

**本轮不创建 P5-A/B/C**（创建即生产写）。

---

## 5. 本轮禁止执行（保持不变）

`T-15` · `T-20` · `C-2`/`C-3`/`C-5` · `D-1`/`D-2`/`D-2b`/`D-3`/`D-4` · `X-1`/`X-2` · 任意 credential rotation / revoke / bind / unbind / transfer · 任意生产 D1 写 · 任意生产凭据写入 · 任意生产设备破坏性操作 · 创建 P5-A/B/C。

---

## 6. Safety / Validation / Git

| 项 | 结果 |
|---|---|
| 生产写操作 | **NO** |
| credential 操作 | **NO** |
| revoke | **NO** |
| 业务代码修改 | **NO**（仅改**文档**：`P1-实现清单.md` · `P1-8-Test-Matrix.md` · `P1-8-Acceptance-Decision.md` · EMQX `P1-6-3-E-closure-review.md`） |
| 烧录 / 重启生产设备 | **NO** |
| 测试代码修改 | **NO** |
| 敏感文件（`.env` / token / password）变更 | **NO** |
| 临时脚本 / dump | **NO**（沿用既有 `.tmp_p16/` 只读工具，未新增敏感产物） |

**Git**：三仓 HEAD 未变（`e36d5ac` / `080cb3c` / `97ca496`）；**未 commit / 未 push**。

---

## 7. Blockers

| # | 级别 | 内容 |
|---|---|---|
| **N-3** | **BLOCKER（`DoD-D6`）** | 明文密码经 3 条路径进入生产 D1 / Worker 日志；**修复方向已定、设计待审、未实施** ⇒ 在修复 + 回归 + 重新验收前，`DoD-D6` **不得 PASS**。 |
| **T-17 前置** | **QUESTION** | 需裁决 T-17 的 `UNBOUND` 口径（当前 `REGISTERED + 0 binding`）。 |
| **N-3 范围** | **QUESTION** | 路径 ②（`config_query` 明文）与 ③（`console.log`）是否纳入本次修复（§2.E）。 |
| N-2 | MINOR | `.pio/` 历史日志含旧共享凭据（已 gitignore）。 |
| P0-2 | MINOR（既有） | 10 个受跟踪文件含明文凭据（git 历史不可清除）。 |

**无测试执行的 BLOCKER**；唯一 BLOCKER 是 **N-3 → D6**。

---

## 8. Recommendation（**下一轮顺序，本轮不执行**）

1. **人工审核本 N-3 Remediation Design**（尤其 §2.A 最小点、§2.E 范围）；
2. 批准后实施**最小脱敏修复**（`index.js`）；
3. 修复后跑**离线/部署版回归**（§2.D，先 R-11/R-6/R-7 等非破坏项）；
4. 处理**历史生产明文**（§2.C：优先"阻止未来 + 凭据轮换"）；
5. **`DoD-D6` 重新验收**；
6. 最后才进入 **credential LIVE / 破坏性 fixture 测试**（`T-15`/`T-20` 等）。

**报告结束 —— 等待人工审核 N-3 修复设计；本轮未编码、未 commit、未 push。**
