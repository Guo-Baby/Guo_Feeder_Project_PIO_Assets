# BT-1 · ACK 成功判据与故障排查说明

> **版本**：**`rev.4`**（`2026-10-18` · **新增 `phase`/`ms` 观测字段与 §8.1**）· `rev.3`（真实 `200` 体已取样 + `unknown` 已实测）· 适用代码：`guo-feeder-api/src/log-ack.js`（`classifyPublish()`）
> **`rev.3` → `rev.4`**：日志新增 `phase`（`headers|body|preflight|throw`）与单次发布耗时 `ms`，
> 用于把 `unknown` 定位到**具体阶段**；**判据代码与超时值均未改变**（1500 / 3000 保持不变，理由见 §8.1 末）。
> **`rev.2` → `rev.3`**：把 `200` 成功体从 **`文档` 级** 升级为 **`实测` 级**（本实例真实返回 `{"id":"00065E0B81B012F211121036C4C30000"}`），
> 并补入一条 **`unknown/TIMEOUT` 但设备实际收到** 的生产实证。**判据代码本身未变**。
> **`rev.1` → `rev.2` 的变化**：`published` 从「`HTTP 200` + 响应体可解析为 JSON 对象」收紧为
> **「`200` + 文档化成功字段 `id` + 无任何否定标记」**；并**更正** `rev.1` 的一处**证据错误**
> （`200 {"message":"ok"}` 曾被写作"生产实测形态"，实际**从未取样**）。
> **依据**：`BT-1-Production-Publish-Contract.md`（`rev.2`）· EMQX **Cloud** v5 文档 `POST /publish`。

---

## 1. 三个状态的定义（严格，不得互换）

| 状态 | 定义 | 判据（全部条件同时成立） | 业务后果 |
|---|---|---|---|
| **`published`** | **API 层确认该次发布被受理** | ① `HTTP 200`；② 响应体是 **JSON 对象**；③ 含**文档化**成功字段 `id`（非空字符串）；④ **无**任何失败 / 矛盾标记 | 计入"已通知设备"；**仍不等于**设备已收到 |
| **`failed`** | **确定未送达，或请求被拒** | 非 2xx；或 `reason_code:16` / `no_matching_subscribers`（含 `202`）；或 2xx 但响应体给出**文档化错误码**（`code`）／失败标记；或根本没发出（`CONFIG_MISSING` / `INVALID_TOPIC` / `TIMEOUT_UNSUPPORTED`） | 不计成功；设备侧重试兜底 |
| **`unknown`** | **本进程得不到结论** | 无响应（超时/断网/体读失败）；其它 2xx（`201`/`204`/非 16 的 `202`…）；`200` 但体不可用 / 非对象；`200` 但**无成功标识**；成功与失败标记**并存**（矛盾） | **既不**断言已送达，**也不**断言没送达 |

> **红线 ①**：`unknown` **不是**"肯定没发出"，更**不是**"已撤销发布"。请求可能已被 Broker 处理，只是我们没等到可信回应。
>
> **★ 已由生产实测证实（`2026-10-18 00:09` · `dev_288485896ce4`）**：一次 `log_ack` 被本判据记为
> **`unknown` / `status=0` / `TIMEOUT`**（`ACK_PUBLISH_TIMEOUT_MS=1500` 到点），而**设备在同一时刻真实收到该 ACK
> 并推进了 `acked_seq`（`110858 → 110859`）**。⇒ 排查时**必须**先看 Broker 侧与设备侧，再判断"是否真的没送到"；
> 也说明 `1500 ms` 对 Worker 冷连接路径**偏紧**（同批直连 `/publish` 仅 165 ms）。见登记册 §8.14。
> **红线 ②**：`published` 是**窄门** —— 任何"看起来像成功"但**没有文档依据**的结构（`{"message":"ok"}`、`{"success":true}`、`{"status":"ok"}`…）**一律不进** `published`。

---

## 2. 判定表（逐形态 · 含证据等级）

证据等级：**`实测`** = 本生产实例实测（`2026-10-17` · `test/bt1/<run>/**`）· **`文档`** = 供应商文档，本实例**未取样** · **`—`** = 无契约，按保守方向处理。

| # | HTTP | 响应体 | 判定 | 诊断码 | 证据 |
|---|---|---|---|---|---|
| 1 | `200` | `{"id":"00065E0B…"}` | **`published`** | `ok` | **`实测`**（`2026-10-18 00:09` · 本实例真实返回；`2026-10-17` 时仅为`文档`级） |
| 2 | `200` | `{"id":"…","message":"ok"}` | `published` | `ok` | 同上（多余字段不改变成功契约） |
| 3 | `200` | `{"id":"…","reason_code":0}` | `published` | `ok` | `reason_code:0` = 成功码，不算否定 |
| 4 | `200` | `{}` | `unknown` | `NO_SUCCESS_MARKER` | `—`（无成功标识） |
| 5 | `200` | `{"message":"ok"}` | `unknown` | `NO_SUCCESS_MARKER` | `—`（**无证据**，不放宽） |
| 6 | `200` | `{"unexpected":"value"}` | `unknown` | `NO_SUCCESS_MARKER` | `—` |
| 7 | `200` | `{"code":"BAD_REQUEST"}` | **`failed`** | `BAD_REQUEST` | `实测`（`code` 为错误字段） |
| 8 | `200` | `{"message":"publish failed"}` | `unknown` | `NO_SUCCESS_MARKER` | `—`（`message` 无枚举契约） |
| 9 | `200` | `{"success":false}` / `{"ok":false}` / `{"failed":true}` | `failed` | `RESPONSE_*` | `—`（通用失败标记） |
| 10 | `200` | `{"error":…}` / `{"errors":[…]}` | `failed` | `RESPONSE_ERROR(S)_FIELD` | `—` |
| 11 | `200` | `{"publish_result":"no_subscriber"}` | `failed` | `PUBLISH_RESULT_…` | `文档`（EMQX 5 PR #9170 字段） |
| 12 | `200` | `{"id":"…","code":"BAD_REQUEST"}` | `unknown` | `CONTRADICTORY_RESPONSE` | `—`（自相矛盾） |
| 13 | `200` | `{"id":""}` / `{"id":123}` / `{"id":null}` | `unknown` | `NO_SUCCESS_MARKER` | `—`（`id` 非非空字符串） |
| 14 | `200` | 空体 / 非 JSON | `unknown` | `BODY_UNVERIFIABLE` | `—` |
| 15 | `200` | 数组 / 标量（如 `[{…}]`、`"ok"`） | `unknown` | `BODY_NOT_OBJECT` | `—`（数组是 `/publish/bulk` 形态） |
| 16 | `202` | `{"message":"no_matching_subscribers","reason_code":16}` | **`failed`** | `NO_SUBSCRIBER` | **`实测`** |
| 17 | `202` | 其它语义（如 `{"message":"queued"}`） | `unknown` | `UNEXPECTED_2XX_202` | `—` |
| 18 | `201` / `203` / `206` | 任意（**即使带 `id`**） | `unknown` | `UNEXPECTED_2XX_<code>` | `—`（非 200 不在成功契约内） |
| 19 | `204` | 空 | `unknown` | `UNEXPECTED_2XX_204` | `—` |
| 20 | `400` | `{"message":"topic_name_invalid","reason_code":144}` | `failed` | `topic_name_invalid` | **`实测`** |
| 21 | `400` | `{"code":"BAD_REQUEST",…}` | `failed` | `BAD_REQUEST` | **`实测`** |
| 22 | `400` | `decode_base64_payload_failed`（`131`） | `failed` | 该 `message` | **`实测`** |
| 23 | `403` | 空体（无凭据） | `failed` | `HTTP_403` | **`实测`** |
| 24 | `5xx` | 任意 | `failed` | 体 `code` → `message` → `HTTP_<status>` | 代码方向（5xx 无样本） |
| 25 | — | 超时 / 断网 / 体读失败（`status:0`） | `unknown` | `TIMEOUT` / `NETWORK_ERROR` | **`实测`**（workerd） |
| 26 | — | 缺凭据 / topic 非法 / 平台不支持中止 | `failed` | `CONFIG_MISSING` / `INVALID_TOPIC` / `TIMEOUT_UNSUPPORTED` | 代码（**根本没发出**） |

### 2.1 为什么"200 + 可解析 JSON"不够

`rev.1` 的判据会把 **`{"code":"BAD_REQUEST"}`（报文说失败）**、**`{"message":"publish failed"}`**、
**`{"unexpected":"value"}`**、**`{"success":false}`**、**`{}`** **全部**记成 `published`
（`2026-10-17` 复现：9 个样本中 8 个被误判为 `published`）。后果：
- 监控显示"ACK 全部成功"，而报文其实在说"失败"或什么都没说；
- 一旦出现新的错误结构（平台升级 / 网关改写），误判会**静默**发生。

⇒ 现判据要求**正向证据**（`id`）+ **无反向证据**（无失败标记）。

### 2.2 `{"message":"ok"}` 的处理（重要的证据更正）

`rev.1` 与生产契约报告 `rev.1` 的 §5.1 曾写「实测形态 `{"message":"ok"}`」。**该样本从未取得**
（同报告 §3.2 已明确"本次未观察到 `200`"）—— 属**文档缺陷**，已更正。
EMQX **Cloud** v5 文档给出的 `200` 体是 **`{"id": "<hex>"}`**，**不是** `{"message":"ok"}`。
⇒ `{"message":"ok"}` 现判 `unknown / NO_SUCCESS_MARKER`（**不猜测、不放宽**）。

---

## 3. 为什么 `202` 不能算成功

EMQX 5 在**没有任何匹配订阅者**时返回 `202` + `no_matching_subscribers`（`2026-10-17` 生产实测）。此时消息被**丢弃**——设备收不到，`acked_seq` 不推进。

若把它记成"成功"：
- 监控显示"ACK 全部成功"，实际设备一条都没收到；
- 故障（设备离线订阅丢失 / ACL 配错 / clientid 冲突）**被计数掩盖**。

⇒ 判据把它归 `failed`（**确定未送达**）：契约明确告诉我们"没有订阅者"，这是**已知结论**，不是"不知道"。

---

## 4. 有界执行与超时

| 常量 | 值 | 含义 |
|---|---|---|
| `ACK_PUBLISH_TIMEOUT_MS` | `1500` | **单条**发布等待上界（纯 I/O） |
| `ACK_TOTAL_BUDGET_MS` | `3000` | **ACK 阶段**总预算（**不含** `persist()`；不是整请求上界） |
| `ACK_MAX_PER_REQUEST` | `20` | 单请求最多尝试条数；超出记 `skipped` |
| 前瞻闸门 | `已用 + 1500 > 3000` 即停发 | 保证"启动的那次发布"也能在预算内收口 |

**为什么是 3000**：EMQX Action 请求超时 **5 s** ⇒ 3.0 s（ACK 阶段）+ 1.5 s（单条）− 余量，避免超时触发 Action 重投（会产生重复记录 + 重复 ACK）。
**跳过的记录**记 `skipped`，由设备重投收敛——**保守方向正确**（少发只导致重试）。

---

## 5. 三态对重试 / 幂等 / 日志保留的影响（本轮逐项核对）

| 问题 | 结论 | 依据 |
|---|---|---|
| `failed` 会导致日志被错误确认或删除吗？ | **不会**。ACK 的**发送**与**判定**是两件事：ACK 在判定**之前**就已发出；判定只写计数与日志，**不**回滚 D1、**不**删行、**不**改 ingest 的 HTTP 响应 | `S2/S3/S7*`：`attempted=1` 且 **D1 写语句 = 0** |
| `unknown` 会导致同样问题吗？ | **不会**，同上 | `S5*`/`S9*`：`attempted=1`、无 D1 写 |
| `unknown` 是否允许安全重试？ | **Worker 不重试**（模块内无重试逻辑）⇒ 不存在"因为 unknown 而重复发布"；重投来自**设备**（`MAX_RETRY=5`，`2/4/8/16/32 s`） | `S9`：`calls=1` |
| 重试造成重复发布，现有机制能容忍吗？ | **能**。重复 ACK 载荷**逐字相同**（`i="ack:<b>:<s>"`），设备侧 `log_ack_classify` 对已确认区间判 `DUPLICATE`（无副作用） | `S11c`：两次载荷字符串相等 |
| 收紧后会不会因结构不匹配而**持续** `unknown`？ | **可能**（若真实 200 体不含 `id`）。这是**已知残余风险**，见 §2.2 与契约报告 §7 `R-1`；表现为 `unknown` 计数上升 + `code=NO_SUCCESS_MARKER`，而 **ACK 仍正常发出**（功能不退化，仅计数与日志等级退化） | `C6`/`S4*` |
| 日志保留策略受影响吗？ | **无**。未触碰 `WP-7` retention / `log_history` 保留期 / Cron | 本轮 diff 仅 `log-ack.js` + 测试 |

> **不变式**：`rows = accepted + duplicate + unpersisted`；`attempted = published + failed + unknown`；`skipped ⊆ accepted`。
> 三态**互斥且完备** —— 任何一次尝试必落入且仅落入一个状态。

---

## 6. 设备删除日志的安全条件（为什么顺序不可改）

唯一合法顺序（`history-ingest.js`）：

```
persist() → 逐条"已持久化"判定 → ACK 发布 → 返回既有响应体
```

- 设备收到 ACK 会**推进 `acked_seq` 并删除 Flash 段**。
- 因此 **ACK 只能覆盖"已确认持久化"的具体记录**：
  - `written[k] === true`（`meta.changes > 0`）⇒ 可直接 ACK；
  - `written[k] === false` ⇒ **不等于"已存在"**（`INSERT OR IGNORE` 也吞其它约束冲突）⇒ 必须 `SELECT 1 … LIMIT 1` 命中才 ACK；
  - 查询异常 / 超时 ⇒ **不 ACK**；
  - `rejected` / `filtered` ⇒ **不 ACK**。
- ACK 载荷 `f = t = seq`（**逐条**），**绝不用区间**：区间 ACK 会把中间未落库的记录一起确认 ⇒ 数据永久丢失。

---

## 7. 故障排查

### 7.1 设备一直重投同一条日志

按顺序查：

1. **Worker 日志**（`wrangler tail`）搜 `[log-ack] summary`：
   - `accepted>0` 但 `published=0`，`failed` 高 ⇒ **看 `code`**：
     - `NO_SUBSCRIBER` ⇒ 设备**没有订阅** `guo_feeder/<dev>/down`（离线且无持久会话 / clientid 变更 / 被踢）。查 `GET /api/v5/subscriptions`。
     - `HTTP_403` ⇒ Worker 的 `EMQX_APP_ID/SECRET` 失效或被撤销。
     - `HTTP_400` ⇒ topic 或载荷非法（正常不该出现）。
     - `BAD_REQUEST` / `RESPONSE_*` ⇒ EMQX 明确拒绝（**含 2xx 带错误码的矛盾响应**）。
   - `unknown` 高 ⇒ 看 `code`：
     - `TIMEOUT` / `NETWORK_ERROR` ⇒ 网络 / 超时。
     - `NO_SUCCESS_MARKER` ⇒ **收到了 200，但响应体不是已文档化的成功形态** ⇒ 见 §7.3。
     - `BODY_UNVERIFIABLE` / `BODY_NOT_OBJECT` ⇒ 体为空 / 非 JSON / 非对象（可能被中间层改写）。
     - `CONTRADICTORY_RESPONSE` ⇒ 报文同时给了成功与失败标记 ⇒ **人工确认契约**。
   - `unpersisted` 高 ⇒ **D1 写入没成功**，此时**本来就不该 ACK**（这是正确行为），去查 `persist` 的 503。
2. **设备侧**：`stats` 看 `acked_seq` 是否前进、`queue_used` 是否满。
3. **订阅面**：`GET /api/v5/subscriptions` 是否仍有 `guo_feeder/<dev>/down`。

### 7.2 监控显示"ACK 全成功"但设备仍在重投

**这是最旧判据（`res.ok === true`，即"2xx 即成功"）针对的场景**（旧代码会把 `202` 记成 `published`）。确认部署版本已含 `classifyPublish()`：日志中应出现 `code=NO_SUBSCRIBER` 而不是 `published … status=202`。

### 7.3 `unknown` 突然升高（`NO_SUCCESS_MARKER`）

⇒ **此时不要放宽判据**。按以下顺序：

1. 取**一次**真实 `200` 响应体（授权范围内：可临时在 Worker 里打印 `status` 与 body 的**键名集合**，不打印值）；
2. 与 EMQX Cloud v5 文档对照：是否含 `id`？是否被网关改写？
3. 若真实形态**稳定且可解释**，走**正式变更**（新增白名单字段 + 同步本文件 §2 + 补测试），并记录证据来源；
4. 若形态不稳定 ⇒ 保持 `unknown`（**安全但观测退化**），不得临时放宽。

> ⚠️ 绝不允许的做法：把 `message`/`code` 的**任意取值**当作成功、或按"2xx 即成功"回退。

---

## 8. 日志字段速查

```
[log-ack] <state> dev=<12hex> b=<boot_seq> s=<seq> status=<http|?> code=<诊断码|-> \
          ms=<本次发布耗时> phase=<headers|body|preflight|throw|->
[log-ack] summary dev=<id> rows=<n> accepted=<n> duplicate=<n> unpersisted=<n> \
          skipped=<n> attempted=<n> published=<n> failed=<n> unknown=<n> ms=<ACK 阶段总耗时>
```

- 只记 `dev / b / s / status / code / ms / phase`：**不记 topic、不记凭据、不记载荷**（`S12c/S12d` 断言）。
- 恒等式：`rows = accepted + duplicate + unpersisted`；`attempted = published + failed + unknown`；`skipped ⊆ accepted`。
- `published` 走 `console.log`，其余走 `console.warn`（便于按级别筛选）。

### 8.1 ★ `phase`：`unknown` 到底卡在哪一段（`2026-10-18` 新增）

`unknown` 以前是**一个**笼统结果：`emqxRequest()` 把两种完全不同的失败压成
同一个 `{status:0, code:"TIMEOUT"}`。现在每次失败都带 `phase`：

| `phase` | 含义 | 排查方向 |
|---|---|---|
| `headers` | **还没拿到响应头**（请求未完成 / 对端未应答 / 连接未建立） | Worker→EMQX 冷连接、TLS、DNS、对端过载 |
| `body` | 头已到，`res.text()` 读超时/失败 | 响应体过大、传输中断（`M-1` 场景） |
| `preflight` | `CONFIG_MISSING` / `TIMEOUT_UNSUPPORTED` —— **根本没发出去** | 部署缺 secret、运行时无 `AbortSignal` |
| `throw` | 调用被上层异常打断（`PUBLISH_THROW`） | 代码异常 |
| `-`（成功） | 拿到 2xx 且体可解析 | 看 `status`/`code` |

配合 `ms` 可算分布：`phase=headers` 且 `ms≈1500` ⇒ 吃满超时；`phase=headers`
且 `ms` 很小 ⇒ 快速失败（连接被拒/鉴权）。

> **现状**：`2026-10-17` 7 次真机发布中 **3 次** `unknown/TIMEOUT`，当时**无法**
> 判断阶段（无该字段）⇒ 因此 `2026-10-18` **没有调整** `ACK_PUBLISH_TIMEOUT_MS`
> （仍是 1500）。下一次生产运行即可用 `phase`/`ms` 定位，再决定是否调参。

---

## 9. 版本与证据边界

| 项 | 状态 |
|---|---|
| `202` / `400` / `403` / 超时 契约 | **实测**（生产 · 隔离 topic） |
| `200` 成功体形态 | **实测（间接）**：`2026-10-17` 真机路径 4 次 `published status=200 code=ok`；因判据要求 `id` 非空字符串 ⇒ **真实 `200` 体确实含 `id`**（与 Cloud v5 文档一致）。**未保存原始 body 文本**（Worker 不打印响应体）|
| 第 ③ 层（Broker 投递） | **实测**：ACK 下行被 Rule B 捕获（`mqtt_messages`，`clientid=http_api`，每 seq 恰 1 条）⇒ Broker **已接受并路由**；仍**不等于**设备收到 |
| 第 ④ 层（独立订阅端收到） | **间接**：设备重投由 **6 次降至 1–2 次**（`log_history` rowid 步长 + tail 计数双证） |
| 第 ⑤ 层（设备推进 `acked_seq`） | **BLOCKED**：本机无 COM 串口（`acked_seq` 仅 RAM、不进遥测）⇒ **端到端闭环未成立** |
| 本轮代码级测试 | **全绿**，但**不证明**生产契约已完全验证 |

---

## 10. 生产运行观测（`2026-10-17` 深夜 · 真机路径）

| 观测 | 值 | 含义 |
|---|---|---|
| ingest 请求 | 7 | seq `110615…110619`（`boot_seq=103`） |
| `published` | 4 | 全部 `status=200 code=ok` |
| `unknown` | 3 | 全部 `status=0 code=TIMEOUT`（**非** 5xx、非 202） |
| `failed` | 0 | 设备在线、订阅正常 ⇒ 无 202 |
| 假成功 | **0** | 无任何 202 / 缺 `id` 被判 `published` |

**★ `TIMEOUT` 的真实成因（待优化项）**：直连 EMQX `/publish` 实测延迟仅 **39–231 ms**（8 次），
而 Worker 内 7 次发布有 **3 次**超过 `ACK_PUBLISH_TIMEOUT_MS=1500` ⇒ 瓶颈在
**Worker→EMQX 的冷连接/传输**（每次新 isolate 可能重新握手），不是判据、也不是 API。
- 语义上仍正确：超时 ⇒ `unknown`（不谎报成功）；其中一次 ACK **随后确实到达 Broker**。
- 代价：设备多重投 1 次（`6 → 1~2`），**无数据风险**。
- **建议（未实施）**：评估把单条上限提到 `2000~2500 ms`，并同批核算
  `ACK_TOTAL_BUDGET_MS + 单条 < 5 s`（EMQX Action 超时）与 P99（`Q-9` 阈值仍为空）。
- **判断纪律**：`unknown` 升高**不等于**回退信号；先看是否全部是 `TIMEOUT`（传输）而非
  `NO_SUCCESS_MARKER`（契约漂移）。
