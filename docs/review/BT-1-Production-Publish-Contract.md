# BT-1 · 生产发布契约与 ACK 判据验证报告

> **版本**：**`rev.4`**（`2026-10-18` 凌晨 · **真实 `200` 原始体已取得**）· `rev.3`（部署后真机路径）· `rev.2`（严格成功判据）· `rev.1`（契约实测）
> **`rev.4` 补记**：串口恢复后主动下发 1 条只读命令 ⇒ 取得**真实 `200` 原始响应体** `{"id":"00065E0B…"}`（`165 ms`），
> `R-1` 由"间接"升为**直接实测**；同一窗口内 `log_ack` 被判 **`unknown/TIMEOUT`**，**设备却真实收到并推进 `acked_seq`**
> （见登记册 §8.14）。**`T04` = `PASS`**；**`T05` = `NOT OBSERVED`**（单段且为 append 段，无回收候选）⇒ **端到端"发布→设备处理"已闭环，"段回收"仍未验证**。
> **`rev.3` 补记**：判据修复已**部署**（版本 `72023596-657a-4a65-aa62-81848540f324`）并在真机路径取到
> **4 `published` / 3 `unknown` / 0 `failed` / 0 假成功**；`200` 体含 `id` 由判据行为**间接证实**（见 `R-1`）；
> 新增 `R-1b`（Worker→EMQX 冷连接导致 `1500 ms` 超时）。
> **授权**：负责人本轮明确授权在**生产 EMQX** 上、以**隔离测试 topic**验证 `/api/v5/publish` 契约，并据此修复 ACK 成功判据（`rev.1`）；`rev.2` 只做**代码/测试/文档**收紧，**未再发起任何生产请求**。
> **结论摘要**：契约**已实测**（`202 / 400 / 403` + 超时）；据此发现并修复了两个**真实判据缺陷**：
> ① `202 no_matching_subscribers` 曾被当作 `published`（`rev.1` 已修）；
> ② **`200` + 任意可解析 JSON 仍被当作 `published`**（`rev.2` 已修 —— `{"code":"BAD_REQUEST"}` 等 8 类误判）。
> **`200` 的成功体形态 = `DOCUMENTED`（EMQX Cloud v5 文档）/ 本实例未取样** ⇒ 判据按"只认文档化字段 `id`"实现。
> **状态纪律**：本报告**不**构成 `BT-1` 关闭、`G-4` 解除或 `WP-5` 解锁。

---

## 1. 背景与问题定义

`BT-1` 的修复（`src/log-ack.js`）在"日志已落 D1"之后向设备回 `log_ack`。发布结果的判定直接影响设备行为：

- **误判为成功**（`published`）⇒ Worker 认为已通知设备；但若消息**实际未投递**，设备会一直重试，而监控显示"一切正常"——**故障被计数掩盖**。
- **误判为失败** ⇒ 只是不 ACK，设备重试，**安全但低效**。

修复前判据是 `res.ok === true`（**任何 HTTP 2xx**）。EMQX 5 在**无匹配订阅者**时返回 **`202`**（仍是 2xx）⇒ 旧判据会把"没投递"记成"已发布"。本报告用生产实测确认该形态是否存在，并据此收紧判据。

---

## 2. 环境与授权

| 项 | 值 |
|---|---|
| 实例 | `https://n302***.ala.cn-hangzhou.emqxsl.cn:8443/api/v5`（**生产**，EMQX Cloud **Serverless**） |
| 版本 | **未取证**：`GET /api/v5/nodes` → **403**（API Key 权限受限）；仅能观察到 `server: Cowboy`。⇒ 一切结论**以行为实测为准**，不以版本文档推断 |
| 凭据 | 仓库 `guo-feeder-api/.dev.vars` 中的部署 API Key（**本报告不记录任何明文值**） |
| 测试 topic | `test/bt1/<run_id>/**`（`run_id = r` + 12 hex，本次 `ra92c71a66e00`） |
| 测试时间 | `2026-10-17` 21:2x（GMT+8） |

### 2.1 发送前的副作用核查（硬性前置）

| 检查 | 结果 | 依据 |
|---|---|---|
| 是否有生产规则会处理 `test/bt1/**` | **否** | 4 条规则 `FROM` 分别为 `guo_feeder/+/up` + `guo_feeder/+/log`、`guo_feeder/#`、`$events/client_connected`、`$events/client_disconnected` ⇒ 均不匹配 `test/` 前缀（`EMQX-Live-Rules-Snapshot-2026-10-17.md` §1） |
| 是否有客户端订阅了 `test/bt1/**`、`#` 或 `$SYS/#` | **否** | `GET /subscriptions` 返回 **1 条**：`guo_feeder/288485896ce4/down` |
| 是否会写入 D1 | **否** | 无规则命中 ⇒ 不会触发 Action ⇒ 不会到 Pages/Worker ⇒ 不写 D1 |
| 是否可能到达真实设备 | **否** | 无匹配订阅；且**未**向 `guo_feeder/<id>/down` 发布过任何消息 |
| 当前在线客户端 | 1 个（`dev_2***`，即真实设备） | `GET /clients`：4 个 clientid，其中仅 1 个 `connected=true` |

> ⇒ **测试消息的副作用 = 零**（不进规则、不进 D1、不到设备）。这一结论建立在上述**实测订阅面**上，而非"应该不会"。

---

## 3. 实测结果

证据文件：`Cloudflare_Assets/.wrangler/bt1_prod/contract_result.json`（`git check-ignore` 覆盖，不入库）
复现脚本：`Cloudflare_Assets/.wrangler/bt1_prod/contract.mjs`（含**硬编码护栏**：topic 必须落在 `test/bt1/<run>/` 内，且不得含 `guo_feeder`/`down`/`log_ack`）

| # | 场景 | 请求 | **HTTP** | 响应体（截断） | 判定 |
|---|---|---|---|---|---|
| S1 | 合法 topic · 无订阅者 · QoS1 | `test/bt1/<run>/publish` | **202** | `{"message":"no_matching_subscribers","reason_code":16}` | 未投递 |
| S2 | QoS0 | 同上 | **202** | 同上 | 未投递 |
| S3 | QoS2 | 同上 | **202** | 同上 | 未投递 |
| S4 | topic 含 `#` | `test/bt1/<run>/#` | **400** | `{"message":"topic_name_invalid","reason_code":144}` | 被拒 |
| S5 | topic 含 `+` | `test/bt1/<run>/+` | **400** | 同上 | 被拒 |
| S6 | `retain=true` 且无订阅者 | — | **202** | `no_matching_subscribers` | 未投递 |
| S7 | QoS=3（非法） | — | **400** | `{"code":"BAD_REQUEST","message":"{\"value\":\"3\",…\"expected\":\"qos()\"…}"}` | 被拒 |
| S8 | **无 `Authorization` 头** | — | **403** | （空体） | 鉴权失败 |
| S9 | 非法 base64 载荷 | `payload_encoding:"base64"` | **400** | `{"message":"{decode_base64_payload_failed,…}","reason_code":131}` | 被拒 |
| S10 | 客户端 5 ms 超时 | — | **抛 `TimeoutError`** | — | 结果未知 |
| S11 | 客户端 50 ms 超时 | — | **抛 `TimeoutError`** | — | 结果未知 |
| S12 | 并发 5 条（不同 topic） | — | **202 ×5** | 全部一致 | 无串线 |

**观测到的其它量**：`content-type: application/json`（403 除外，为空体）· 最大延迟 **297 ms** · 无 5xx。

### 3.1 契约结论（可断言）

1. **`202` = 无匹配订阅者**，响应体显式给出 `reason_code: 16`。**这是 2xx，但语义是"未投递"。**
2. **`400` = 参数/topic 非法**：`topic_name_invalid`（`144`）、`BAD_REQUEST`（结构化校验）、`decode_base64_payload_failed`（`131`）。
3. **`403` = 鉴权失败**（本项目环境下"无凭据"返回 403 而非 401）。
4. **无 5xx 样本**：未构造出"Broker 内部故障"（不能为测试而破坏生产），此列 **NOT RUN**。
5. **超时在客户端侧表现为异常**（`TimeoutError`），此时**无法判断服务端是否已处理**。

### 3.2 未取得样本：`200`

本次**未观察到 `200`**。原因是：本实例上**唯一**的订阅是真实设备的 `guo_feeder/288485896ce4/down`，而向该 topic 发布**被明令禁止**；要造出"有订阅者"的场景，必须**新建 MQTT 身份 + 放开 `test/bt1/#` 的 ACL**——那是新增生产身份与授权面，超出"最小修改"授权。

⇒ **`200` 的语义目前是契约推断**（"至少一个匹配订阅者"），标注为 **`INFERRED`**，不是实测。判据对此的处理见 §5 的"保守方向"。

> **★ `rev.2` 更正（重要）**：`rev.1` 的 §5.1 判定表把 `200 {"message":"ok"}` 写成「**实测形态**」，与本节
> 「本次**未观察到 `200`**」**直接矛盾** —— 该样本从未取得，属**文档缺陷**（同时被写进了
> `log-ack.js` 的头部注释与 `BT-1-ACK-Predicate-and-Troubleshooting.md` `rev.1`，`rev.2` 已一并修正）。
> ⇒ 正确的表述：**`200` 的响应体形态 = `DOCUMENTED`（见 §3.3）/ 本实例未取样**，
> 且**供应商文档给出的形态并不是** `{"message":"ok"}`。

### 3.3 `200` 形态的文档依据（`DOCUMENTED` · 非实测）

来源：EMQX **Cloud** v5 文档 · `POST /publish`（`https://docs.emqx.com/en/cloud/v5/api/publish_v5.html`，`2026-10-17` 取阅）：

| 项 | 文档原文（要点） |
|---|---|
| Response Message | **`id`** · `String` · "Identifier" —— **单条发布只有这一个字段** |
| `200` 示例 | `{"id": "000600D09A099053F445000014C30000"}` |
| `202` 示例 | `{"message": "no_matching_subscribers", "reason_code": 16}` |

**该文档对本实例的适用性有间接支持**：文档的 `202` 示例与本实例**实测 `202` 逐字一致**
（`{"message":"no_matching_subscribers","reason_code":16}`，含 `reason_code` 数值类型）；
但 `200` 体**仍未在本实例取样** ⇒ 证据等级只能记为 **`DOCUMENTED`**，**不得**写成 `CONFIRMED`。

> 另一个**不作为依据**但已登记的旁证：EMQX 开源仓 PR #9170（`release-v50`）描述了
> `200` 体含 `message_id` + `publish_result`（`"OK"`）的形态。**本判据不采纳该形态作为成功白名单**
> （见 §5.2），仅把它用于**失败标记**识别（`publish_result != "OK"` ⇒ 非成功）。

---

## 4. 证据分层（不得互相替代）

| 层 | 含义 | 本次结论 |
|---|---|---|
| ① Worker 发起发布 | 调用是否发出 | ✅ 代码级 + 实测（`calls` 计数） |
| ② API 返回 | HTTP 状态 + 响应体 | ✅ **实测**（§3） |
| ③ Broker 接受并投递 | 至少一个订阅者收到 / 入队 | ⚠️ **仅间接**：`202` 可**否定**投递；`200` **未取得样本** |
| ④ 独立订阅端收到 | 用非设备订阅者取证 | **NOT RUN**（无可用非设备身份；见 §3.2） |
| ⑤ 设备收到并推进 `acked_seq` | 真机 | **NOT RUN**（`BT1-T04`，需 `Q-10` 授权） |

> **禁止表述**：不得把 ② 的成功写成"设备已收到 ACK"，也不得把本次实测写成"端到端已验证"。
>
> **`rev.2` 补充一层（不得与 ② 并列）**：`200` 的成功体形态来自**供应商文档**（§3.3），
> 属"**有依据、但未在本实例取样**"的独立一层。它**不能**替代 ③④⑤，也**不能**被写成实测。

---

## 5. ACK 判据修复

### 5.0 两轮修复的分界

| 轮次 | 缺陷 | 判据 |
|---|---|---|
| `rev.1` | `res.ok === true`（**2xx 即成功**）⇒ `202` 未投递被记 `published` | `200` + **可解析 JSON 对象** |
| **`rev.2`（本轮）** | 上一条仍过宽 ⇒ `{"code":"BAD_REQUEST"}` / `{"message":"publish failed"}` / `{"unexpected":"value"}` / `{"success":false}` / `{}` **全部**被记 `published`（`2026-10-17` 复现：9 个样本 **8 个误判**） | `200` + JSON 对象 + **文档化成功字段 `id`（非空字符串）** + **无任何否定标记** |

### 5.1 判定表（`classifyPublish()` · `src/log-ack.js` · `rev.2`）

| HTTP / 形态 | 判定 | 诊断码 | 证据 |
|---|---|---|---|
| `200` + `{"id":"<非空>"}`（可含无关字段） | **`published`** | `ok` | **`DOCUMENTED`**（§3.3） |
| `200` + `{"id":"…","reason_code":0}` | `published` | `ok` | `reason_code:0` = 成功码 |
| `200` + `{"code":"…"}`（任意 `code`） | **`failed`** | 该 `code` | `code` 为**实测错误字段** |
| `200` + `{"success":false}` / `{"ok":false}` / `{"failed":true}` | `failed` | `RESPONSE_*` | 通用失败标记 |
| `200` + `{"error":…}` / `{"errors":[…]}` | `failed` | `RESPONSE_ERROR(S)_FIELD` | 同上 |
| `200` + `{"publish_result":"<非 OK>"}` | `failed` | `PUBLISH_RESULT_…` | PR #9170 字段（仅用于否定） |
| `200` + 成功标记 **与** 失败标记并存 | `unknown` | `CONTRADICTORY_RESPONSE` | 自相矛盾 |
| `200` + `{}` / `{"message":"ok"}` / `{"unexpected":…}` / `{"id":""}` | `unknown` | `NO_SUCCESS_MARKER` | **无成功依据** |
| `200` 但体为空 / 非 JSON | `unknown` | `BODY_UNVERIFIABLE` | — |
| `200` 但体是数组 / 标量 | `unknown` | `BODY_NOT_OBJECT` | 数组属 `/publish/bulk` |
| `202` + `reason_code:16` / `no_matching_subscribers` | **`failed`** | `NO_SUBSCRIBER` | **实测** |
| `202` 其它语义 | `unknown` | `UNEXPECTED_2XX_202` | — |
| `201` / `203` / `204` / `206`（**即使带 `id`**） | `unknown` | `UNEXPECTED_2XX_<code>` | 非 200 不在成功契约 |
| `4xx` / `5xx` | `failed` | 体 `code` → `message` → `HTTP_<status>` | 实测（4xx）/ 方向（5xx） |
| `status: 0`（超时 / 断网 / 体读失败） | `unknown` | `TIMEOUT` / `NETWORK_ERROR` | 实测（workerd） |
| `CONFIG_MISSING` / `INVALID_TOPIC` / `TIMEOUT_UNSUPPORTED` | `failed` | 同名 | **根本没发出** |

### 5.2 为什么这才是"最低可辩护"的成功判据

1. **只有一个成功入口**：`return { state: "published" }` 在 `classifyPublish()` 内**仅一处**（`log-ack.js:429`）；
   `summary.published` 的唯一自增点是 `summary[verdict.state]++`，`verdict` 只来自该函数 ⇒ **不存在绕过路径**（`§6` 已核对）。
2. **正向证据 + 无反向证据**：既要求文档化字段 `id`，又要求不存在任何否定标记；两者缺一即不进 `published`。
3. **不发明白名单**：`message:"ok"`、`success:true`、`status:"ok"`、`code:"SUCCESS"` 等**一律不认**
   —— 没有任何证据支持它们表示"已投递"。`code` 出现时按**否定**处理（`code` 的唯一实测用法是错误码）。
4. **矛盾即未知**：`id` 与 `code` 同时出现时判 `unknown`，不偏向任何一方。
5. **保守方向的代价是可见的**：若真实 `200` 体不含 `id`，全部 ACK 会落 `unknown/NO_SUCCESS_MARKER`
   —— **ACK 仍会发出**（功能不退化），只是**计数与日志等级退化**；这是"宁可少宣称成功"的必然代价（§7 `R-1`）。

### 5.3 代码与测试差异（本轮）

| 文件 | 改动 |
|---|---|
| `guo-feeder-api/src/log-ack.js` | **新增** `SUCCESS_RESPONSE_FIELD` / `REASON_CODE_NO_SUBSCRIBER` 与 6 个内部判据函数；`classifyPublish()` 重写为"否定标记 → 状态码 → 正向标识"三段式；头部三态定义与证据分层同步。**未改**：持久化顺序、存在性校验、有界预算、载荷、topic、`log_ack` 调用点 |
| `guo-feeder-api/test/current/log_ack_contract.test.mjs` | 判据矩阵 **18 → 46** 条；**修正 2 条编码了缺陷契约的旧断言**（`200 {}` 与 `200 {"message":"ok"}` 曾断言为 `published`）；新增 `C6`（未识别形态仍发 ACK） |
| `guo-feeder-api/test/current/log_ack_publish_strict.test.mjs` | **新增** 套件（**76** 条）：端到端（`ackPersistedLogs`）覆盖 10 类场景 + 三态恒等式 + "无 D1 写语句" + 幂等/重复发布 + 日志不含凭据 |
| `guo-feeder-api/test/current/log_ack.test.mjs` / `log_ack_edge.test.mjs` | **仅改夹具**（成功桩由 `{}` 改为 `{"id":…}`）；**断言数量与内容未变**（`60` / `48`） |
| `guo-feeder-api/test/run_all.mjs` | 登记新套件（`+1` 行） |
| `guo-feeder-api/src/emqx-admin.js` · `history-ingest.js` | **本轮未改**（其未提交差异属上一轮 `M-1` 修复与 `persist()` 逐条化） |
| `.wrangler/bt1_iso_r6/**`（gitignored） | 隔离副本刷新为**本轮** `log-ack.js` + stub 成功体改为文档形态；**独立目录运行，未覆盖 `bt1_iso` 既有证据** |

---

## 6. 验证结果

| 项 | 结果 |
|---|---|
| `log_ack.test.mjs`（既有契约套件） | **60 PASS / 0 FAIL**（断言未改，仅成功桩改为文档形态） |
| `log_ack_edge.test.mjs`（边界套件） | **48 PASS / 0 FAIL**（同上） |
| `log_ack_contract.test.mjs`（判据矩阵） | **57 PASS / 0 FAIL**（`rev.1` 为 26） |
| `log_ack_publish_strict.test.mjs`（**新增**） | **76 PASS / 0 FAIL** |
| 全量 L1（`test/run_all.mjs`） | **13/13 套件 `exit=0`**；`PASS 965 / FAIL 0`（legacy 7 项 `EXPECTED_FAILURE` 属既有预期） |
| 隔离集成（`.wrangler/bt1_iso_r6` · 真实 D1 引擎 + 本地 stub） | **11 PASS / 0 FAIL**；`[log-ack] published … code=ok`（首插 / 重插）· `failed … INTERNAL_ERROR`（stub 500）· 生产域名命中 **0** |
| 构建（`wrangler deploy --dry-run --outdir`） | **105.76 KiB / gzip 26.12 KiB**（`rev.1` 为 103.17 / 25.49；**未部署**） |

> 全部为**逻辑替身 + 本地真实 D1 引擎**测试：不证明真实 EMQX 的 `200` 语义、不证明设备收到。四类证据**不得混用**。

---

## 7. 未验证项与残余风险

| # | 项 | 状态 | 风险 | 建议 |
|---|---|---|---|---|
| R-1 | `200` 的响应体形态 | **已实测（直接 · `2026-10-18 00:09`）** | 主动下发只读命令后，`POST /api/v5/publish` 返回 **`200 {"id":"00065E0B81B012F211121036C4C30000"}`**（`165 ms`）⇒ **原始 body 已取得**；喂给 `classifyPublish()` 实测 **`published/ok`**。**残余**：未按 QoS/retain 组合分别取样 | 仍**不要**把 `message:"ok"` 之类加进白名单；如需多组合样本，另行授权 |
| R-1b | **Worker 侧发布延迟** | **已实测（`2026-10-17` 深夜）** | 7 次 ACK 发布中 **3 次超 `1500 ms`**（`unknown/TIMEOUT`），而直连 API 仅 39–231 ms ⇒ 瓶颈在 Worker→EMQX 冷连接；**不影响数据安全**，只多一次设备重投（`6 → 1~2`） | 评估 `ACK_PUBLISH_TIMEOUT_MS` 提到 `2000~2500`，同批核算 `ACK_TOTAL_BUDGET_MS` 与 Action `5 s`；`Q-9` 阈值仍为空 |
| R-2 | 第 ④ 层（独立订阅端收到） | **NOT RUN** | 无法排除"200 但未真正投递" | 需非设备 MQTT 身份 + `test/bt1/#` ACL（**属生产授权变更，需单独批准**） |
| R-3 | 第 ⑤ 层（设备推进 `acked_seq`） | **NOT RUN** | 端到端未闭环 | `Q-10` 授权后跑 `BT1-T04` |
| R-4 | Broker 内部故障（5xx） | **NOT RUN** | 未验证 5xx 判定（代码按 `failed` 处理，方向安全） | 可在非生产实例补偿 |
| R-5 | 实例版本 | **未取证**（403） | 未来平台升级可能改契约 | 每次变更后重跑本报告 §3 矩阵 |
| R-6 | 白名单字段的**完备性** | **单一字段**（`id`） | 若同一平台在不同 QoS / retain 下返回不同成功体 ⇒ 部分 ACK 落 `unknown` | 采样时按 QoS 0/1/2 × retain 组合各取一次；本判据**不**预设"多种成功体都接受" |

---

## 8. 门禁状态

| 对象 | 状态 | 依据 |
|---|---|---|
| `BT-1` | **`OPEN`** | `T02`/`T04`/`T05` 未 `PASS`；**`T04`/`T05` 于 `2026-10-17` 深夜实测为 `BLOCKED`（本机无 COM 串口）**；第 ⑤ 层 `NOT RUN` |
| **部署** | **已部署** `72023596-657a-4a65-aa62-81848540f324`（`2026-10-17T15:24:52Z`） | 负责人「受控部署 + 真机验证」授权；**未**改 EMQX/固件/D1 migration |
| `G-4` | **`BLOCKED`** | 无人工显式改写记录（**部署 ≠ 解除**） |
| `WP-5` | **未解锁** | `GATE-WP5` 七条：①②③ `NOT RUN`、④ **已裁决未落实**（生产 Rule B 未改）、⑤ `T13` `BLOCKED`、⑥ 未定性、⑦ `BLOCKED` |
| `STOP-R1` | **未书面解除** | 触发前提（"坚持字面"）已由 `Q-2=A-2` 消除；解除程序原文未规定 ⇒ 须负责人书面确认 |
| `GATE-ENC` | 7/7（文档层面） | 不变 |

---

## 9. 附录

- **证据 JSON**：`Cloudflare_Assets/.wrangler/bt1_prod/contract_result.json`（含每条请求的 topic / 状态 / 响应体 / 延迟；**不含**任何凭据）
- **复现**：`cd Cloudflare_Assets/.wrangler/bt1_prod && node contract.mjs`
- **只读盘点**：`node probe2.mjs`（clients / subscriptions）
- **注意**：脚本从 `guo-feeder-api/.dev.vars` 读凭据，**仅在本机进程内使用**，不写入任何输出。
- **`rev.2` 新增证据（隔离 · 非生产）**：`Cloudflare_Assets/.wrangler/bt1_iso_r6/`
  —— 由 `bt1_iso` **复制**而来（**未覆盖**原目录及其日志）；`src/log-ack.js` 刷新为本轮版本，
  `stub.mjs` 的成功体改为文档形态 `{"id":"…"}`；`run_iso.sh` 结果 `PASS=11 FAIL=0`，
  `wrangler.log` 中 `[log-ack] published … code=ok` / `failed … INTERNAL_ERROR` 可查，
  生产域名命中 **0**。该目录 `.wrangler/**` 已 gitignore，**不入库**。
- **本轮（`rev.2`）未发起任何生产请求**：无 `wrangler deploy`、无 EMQX 规则/动作/ACL/用户变更、
  无生产 MQTT 发布、无设备操作、无 `commit`/`push`。
