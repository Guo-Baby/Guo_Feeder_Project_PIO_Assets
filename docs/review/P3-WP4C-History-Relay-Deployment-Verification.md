# P3 · `WP4C` —— **History Relay 部署与端到端验证记录**（`H-WP4C-11` 收口）

> ## ✅ `PRODUCTION CHANGE EXECUTED` —— **Pages 中转已部署 + 端到端验证通过**
>
> | 项 | 值 |
> |---|---|
> | **本文档性质** | **部署后验证记录（Deployment Verification）** —— 留痕一次**已授权的生产部署**及其**端到端验证结果** |
> | **对应授权** | **`H-WP4C-11`**（Pages 中转函数新增 + 部署）—— 由 **`H-WP4C-10-A`** 裁决（方案 A = `Pages Proxy`）派生 |
> | **状态** | ✅ **`CLOSED`**（部署完成 · 中转层 + Worker 端点 + D1 落库**全部实测通过**） |
> | **执行人** | 代理（人工已于 `2026-10-12` 明示授权：*「授权你，开始部署，做完整测试确保可用」*） |
> | **基线** | 固件仓 **`ef49db5`** · CF 仓 **`9b25203`** · EMQX 仓 **`b6bdbe2`** |
> | **本次实际的生产变更** | **1 次 Pages 部署**（仅新增 Functions bundle；静态资产未变）· **D1 仅只读 + 定点清理**（详见 §5） |
> | **本次未做的** | ❌ 未创建 / 修改任何 EMQX Rule · Action · Connector · ❌ 未部署 Worker · ❌ 未跑 migration · ❌ 未改固件 |

---

## §1 执行摘要

| # | 步骤 | 结果 |
|---|---|---|
| 1 | 在 Pages 工程新增转发函数 `guo-feeder-pagesdev/functions/api/history/ingest.js` | ✅ 完成（136 行 · 语法校验通过） |
| 2 | `wrangler pages deploy` 部署到生产 | ✅ 完成 —— 新部署 = **`61b4938e-fa2e-42da-b950-f527781f79f9`** |
| 3 | 线上端点验证（中转层存活 + 鉴权边界） | ✅ **7 / 7 `PASS`** |
| 4 | 端到端写入验证（信封 → D1 落库） | ✅ **8 / 8 `PASS`**（另有人工前置 **9 / 9 `PASS`**） |
| 5 | D1 落库证据（行内容逐列核对） | ✅ **telemetry ×4 + log ×1** 已落库并**定点清理** |
| 6 | 既有端点回归（`/api/presence/event` · `/api/webhook` · `/api/command`） | ✅ 全部存活，与部署前一致 |
| 7 | 核查**旧连接器**是否被 CF 收到并存储（人工指定重点） | ✅ **是** —— `mqtt_messages` **2458 行**且**仍在增长**（详见 §6） |

**结论**：`EMQX Action → Pages 中转 → Worker `/api/history/ingest` → D1` 的**后半段（中转 → Worker → D1）已端到端打通并验证**。
**前半段（EMQX → 中转）由既存 presence 连接器在生产上持续验证**（同 host、同 TLS、同中转模式）。

---

## §2 部署事实

| 项 | 值 |
|---|---|
| 工程 | `guo-feeder-pagesdev`（Pages · account `9e72d201efec6925b3a468a0e3fb4928`） |
| **生产域名** | `https://guo-feeder-pagesdev.pages.dev` |
| **部署命令** | `wrangler pages deploy . --project-name guo-feeder-pagesdev --commit-dirty=true`（wrangler **4.145.0**） |
| **新部署 ID** | **`61b4938e-fa2e-42da-b950-f527781f79f9`**（branch `main` · Environment `Production`） |
| **部署预览域** | `https://61b4938e.guo-feeder-pagesdev.pages.dev` |
| **上一版本（回滚目标）** | **`5790b3c5-ff84-4539-bc03-0810401abd39`** |
| 变更内容 | **仅 Functions bundle** —— 新增 `/api/history/ingest`；静态资产**未变**（构建输出 `Uploaded 0 files (2 already uploaded)`） |
| 代码来源 | CF 仓 `9b25203` + 本次新增 `functions/api/history/ingest.js` ⇒ **部署后 CF 仓 HEAD（待提交）== 线上版本** |

**中转函数形态**（与 `functions/api/presence/event.js` 逐条同构）：

| 要点 | 实现 | 理由 |
|---|---|---|
| 转发方式 | `request.text()` **逐字转发**，**不解析 JSON** | 保住 Worker 侧 `invalid_envelope` / `unparsable_payload` / 三态计数等语义 |
| 鉴权头 | **`X-Webhook-Key` 原样透传**（**必须** —— 与 `/api/webhook` 的无鉴权根路径**不同**） | Worker `/api/history/ingest` 由 `requireWebhookKey()` 守护；不透传 ⇒ 一律 `401` |
| 上游 | `https://guo-feeder-api.guobaby.workers.dev/api/history/ingest`（可用 Pages 环境变量 `HISTORY_UPSTREAM` 覆盖） | 与既有中转同一上游主机 |
| 上游不可达 | **`502`** + `{"error":"history_upstream_unreachable"}` | 让 EMQX 视为失败并重投，**不得改成 `200`**（否则静默丢历史） |
| 非 `POST` | `200 "History Relay OK"` | 与同工程其余三个函数一致，便于在线存活探测（EMQX「测试连接」多为 GET） |

---

## §3 验证矩阵（**全部实测**）

### 3.1 A 段 —— 中转层与鉴权边界（只读，**不写任何数据**）

| ID | 用例 | 期望 | 实测 | 结果 |
|---|---|---|---|---|
| `A-1` | `GET /api/history/ingest` | `200 "History Relay OK"` | 同 | ✅ |
| `A-2` | `GET /api/presence/event` | `200 "Presence Relay OK"` | 同 | ✅ |
| `A-3` | `GET /api/webhook` | `200 "MQTT Router OK"` | 同 | ✅ |
| `A-4` | `GET /api/command` | `200 "Command API OK"` | 同 | ✅ |
| `A-5` | `POST /api/history/ingest` **无 key** | `401` | `401 {"error":"unauthorized"}` | ✅ |
| `A-6` | `POST /api/history/ingest` **错 key** | `401` | `401 {"error":"unauthorized"}` | ✅ |
| `A-7` | `POST /api/presence/event` **无 key** | `401` | `401 {"error":"unauthorized"}` | ✅ |

> **★ `A-5` 的证明力**：`401` **不是**中转层自己回的字面量 —— 它是 **Worker 的 `requireWebhookKey()`** 回包，
> 经中转**原样回传**。⇒ 一次性证明三件事：**① 路由已生效 ② 请求已送达 Worker ③ 鉴权确实生效**。

### 3.2 B 段 —— 端到端写入（**会写生产 D1**，用合成 `device_id`，事后定点清理）

| ID | 用例 | 期望 | 实测 | 结果 |
|---|---|---|---|---|
| `B-1` | 合法 telemetry 信封 | `200 {accepted:1,…}` | `{"accepted":1,"duplicate":0,"rejected":0}` | ✅ |
| `B-2` | **同信封重发**（幂等） | `200 {duplicate:1}` | `{"accepted":0,"duplicate":1,"rejected":0}` | ✅ |
| `B-3` | 非法 JSON | `400 invalid_envelope` | `400 {"error":"invalid_envelope"}` | ✅ |
| `B-4` | 缺 `topic` | `400 missing_topic` | `400 {"error":"missing_topic"}` | ✅ |
| `B-5` | 未知 topic 族 | `400 unknown_topic` | `400 {"error":"unknown_topic"}` | ✅ |
| `B-6` | **log 域**（`payload_base64`） | `200 {accepted:1}` | `{"accepted":1,"duplicate":0,"rejected":0}` | ✅ |
| `B-7` | 数组 `payload`（2 条） | `200 {accepted:2}` | `{"accepted":2,"duplicate":0,"rejected":0}` | ✅ |
| `B-8` | 部分非法（1 合法 + 1 缺 `seq`） | `200 {accepted:1,rejected:1}` | `{"accepted":1,"duplicate":0,"rejected":1}` | ✅ |

**合计 `PASS 15 / FAIL 0`**（A 7 + B 8）。`B-2` 证明**幂等键 `ux_th_dedup` 生效**；`B-8` 证明**记录级失败不污染整批 HTTP 状态**（与设计 §7.4 一致）。

### 3.3 可复现命令

```bash
cd Cloudflare_Assets
unset http_proxy https_proxy HTTP_PROXY HTTPS_PROXY ALL_PROXY all_proxy
node scripts/smoke-history-relay.mjs            # 只读（A 段）
node scripts/smoke-history-relay.mjs --write    # 含写入（A + B 段；末尾打印清理 SQL）
```

> 脚本 `secrets` 来源：环境变量 `WEBHOOK_SECRET`，否则读 `Cloudflare_Assets/.secrets.local`（**gitignored**）。
> **脚本全程不打印密钥值**；`--write` **不自动清理**（末尾打印定点 `DELETE`）。

---

## §4 D1 落库证据（**逐列核对**）

`B` 段写入的行（**清理前**实测，`device_id = ffffffffffff` 为合成值）：

```
telemetry_history
 id | device_id    | ts         | type    | value | source | seq    | schema_version | created_at(UTC)
  1 | ffffffffffff | 1799999991 | smoke_t | 21.5  | NULL   | 900001 | 1              | 2026-10-12 07:48:23
  3 | ffffffffffff | 1799999993 | smoke_t | 1     | NULL   | 900003 | 1              | 2026-10-12 07:48:37
  4 | ffffffffffff | 1799999993 | smoke_h | 55    | NULL   | 900004 | 1              | 2026-10-12 07:48:37
  5 | ffffffffffff | 1799999994 | smoke_t | 9     | NULL   | 900005 | 1              | 2026-10-12 07:48:39

log_history
 id | device_id    | ts         | level | code | boot_seq | seq    | message         | payload    | created_at(UTC)
  1 | ffffffffffff | 1799999992 | ERROR | 17   | 7        | 900002 | smoke test log  | AAECAwQ=   | 2026-10-12 07:48:36
```

**三条设计契约被现场证实**：

| 契约 | 证据 |
|---|---|
| `value` **物理列为 TEXT**（`DT:WP-2` §3.3） | `21.5` / `1` / `55` / `9` 落库为**字符串**（`"21.5"` …），非 `REAL` |
| `payload` **透明搬运**（`H-WP2-3`；**禁解码 CBOR**） | `payload_base64 = "AAECAwQ="` **原样**落 `payload` 列，**未被解码** |
| `created_at` **由 D1 生成**（Worker 不提供） | 列有值且与请求时刻一致 ⇒ `DEFAULT (unixepoch())` 生效 |

> **注**：`telemetry_history.id` 跳号（1 → 3）是 `INSERT OR IGNORE` 在**幂等冲突**时仍**消耗 `AUTOINCREMENT` 序列**所致（`B-2` 的重复投递）—— **非数据丢失**，行数正确（4 行 = `B-1` + `B-7`×2 + `B-8`×1）。

**清理**：两次写入（人工 9 例 + 脚本 8 例）共产生 **telemetry 8 行 + log 2 行**，已按 `device_id='ffffffffffff'` **定点 `DELETE`**（`changes` 合计 `4+1` + `4+1` = 10）。
清理后复核：`telemetry_history = 0` · `log_history = 0` · **`mqtt_messages = 2458`（未受影响）**。

---

## §5 本次生产变更的边界（**逐项核对**）

| 项 | 状态 |
|---|---|
| Pages 部署 | ⚠️ **1 次**（**已授权** —— `H-WP4C-11`；仅 Functions bundle） |
| D1 写入 | ⚠️ **仅测试行**（合成 `device_id`，**已定点清理**，`changes` 10 行） |
| D1 schema / migration | ✅ **未动**（`0001` / `0002` / `0003` 均未触碰） |
| Worker 代码 / 部署 | ✅ **未动**（CF 仓 `guo-feeder-api/` 零改动；线上仍 `99e6d937`） |
| **EMQX Rule / Action / Connector** | ✅ **零创建 · 零修改**（未调用任何 EMQX API；未登录控制台） |
| 固件源码 / 配置 | ✅ **未动** |
| 冻结设计（`P3-WP4C-EMQX-Rule-Design.md` / `DT:WP-4`） | ✅ **未改** |
| 密钥 | ✅ **未写入任何文件**；脚本/中转均不打印、不落盘（仅从 gitignored `.secrets.local` 读入变量） |
| `push` | ✅ **未 push** |

---

## §6 旧连接器「CF 是否收到并存储」核查（**人工指定重点**）

> 人工指示：*「关键是旧的连接器是转发每条消息给 cf 做存储，你需要检查 cf 是否收到了，并且做了存储。」*

**链路**：EMQX 规则 `r-n302933b-a01720`（`SELECT * FROM "guo_feeder/#"`）→ 动作 `a-n302933b-921e22` → 连接器 `c-n302933b-b12100`
（`https://guo-feeder-pagesdev.pages.dev/api/webhook`）→ Pages `webhook.js` → Worker **根路径**（`/`）→ **P0 旧 webhook 流程** → **`mqtt_messages`**。

### 6.1 结论：**是 —— 收到并已存储，且仍在增长**

| 指标 | 实测值 |
|---|---|
| **`mqtt_messages` 总行数** | **2458** |
| 方向分布 | `…/up` **1918** · `…/down` **531** · **`…/log` = 0** |
| 不同 topic 数 | **31** |
| 不同 `device_id` 数 | **61** |
| 最新一条（UTC） | **`2026-10-12 06:33:36`**（本地 **14:33**，≈1 小时前） |
| 最早一条（UTC） | `2026-08-23 15:10:41` |

**按日活跃度（UTC，最近 12 天）**：

| 日期 | 行数 | 日期 | 行数 |
|---|---|---|---|
| 2026-10-12 | 4 | 2026-10-06 | 3 |
| 2026-10-11 | 13 | 2026-10-05 | 21 |
| 2026-10-10 | 4 | 2026-10-04 | 7 |
| 2026-10-09 | 22 | 2026-10-03 | 90 |
| 2026-10-08 | 70 | 2026-10-02 | 78 |
| 2026-10-07 | 56 | 2026-10-01 | 28 |

⇒ **不是「失效」，而是「低成功率」**：19k 触发 / 17k 失败 ≈ **13% 成功率**，与库存 2458 行、每日仍有 ~4–90 行新增**吻合**。

### 6.2 生产设备实样（`288485896ce4`，最近 5 条）

```
guo_feeder/288485896ce4/up | up | {"s":1,"c":"result","i":"80110","o":"COMPUTER_RESET",… | 2026-10-12 06:33:36
guo_feeder/288485896ce4/up | up | {"c":"ack","i":"80110","o":"COMPUTER_RESET","p":{…}}   | 2026-10-12 06:33:35
guo_feeder/288485896ce4/up | up | {"s":0,"c":"result","i":"80110","o":"COMPUTER_RESET",… | 2026-10-12 06:33:34
guo_feeder/288485896ce4/up | up | {"s":0,"c":"result","i":"80110","o":"COMPUTER_RESET",… | 2026-10-11 16:57:02
guo_feeder/288485896ce4/up | up | {"s":1,"c":"result","i":"80110","o":"COMPUTER_RESET",… | 2026-10-11 16:57:02
```

⇒ 落库的是**真实业务报文**（`ack` / `result` 帧），**解析字段（`c` / `i` / `o` / `s` …）均已正确填充** ⇒ 存储路径**功能正常**。

### 6.3 ★ 关键新证据：**`…/log` 主题 0 行**

全表 `topic LIKE '%/log'` = **0 行**（31 个 topic 中**无一个** `/log`）。
⇒ 与 `R-WP4C-15` 的**候选机制一致**：**`/log` 载荷是二进制 CBOR**，而 Pages `webhook.js` 第一件事是 `await request.json()`
⇒ 非 JSON ⇒ `400 Invalid JSON` ⇒ EMQX 记失败并重投。**`/log` 高频（固件日志）** ⇒ 恰好解释 ~90% 失败面。
（**仍属候选未定论** —— 人工暂不提供失败明细；本工作包**不修**该链路，且**不得顺手改**。）

### 6.4 旁证：presence 链路（同一 host / 同一中转模式）**在生产持续成功**

| 指标 | 实测值 |
|---|---|
| `device_event` 总行数 | **148** |
| 近 7 天 | **114** |

⇒ 连接器 `c-n302933b-291200`（→ `https://guo-feeder-pagesdev.pages.dev/api/presence/event`，**同 host**）**在生产持续投递成功**。
这就是 **`V-1`（EMQX 侧可达性）** 的**现场证据**（见 §7）。

---

## §7 `V-1` 判定

| 项 | 内容 |
|---|---|
| **`V-1` 定义**（纪律 §6A.3） | **确认目标 Action 可达** —— **实测**可达；**不得**以"应该能通"代替 |
| **判定** | 🟢 **`PASS`（带说明）** |
| **证据 1（EMQX 侧）** | 既存连接器 **`c-n302933b-291200`** 已在生产**持续投递**到 **同一 host** `https://guo-feeder-pagesdev.pages.dev`（`device_event` 近 7 天 **114** 次）⇒ **EMQX → `*.pages.dev` 的可达性与 TLS 链路已被现场证实**（`D-2` / 方案 A 成立） |
| **证据 2（目标端点）** | 新端点 `GET/POST /api/history/ingest` 已在线且行为正确（§3.1） |
| **尚须补的** | **新连接器本身**的可达性仍须在 `V-2` **保存后实测**（`R-WP4C-16` guard：TLS 明细不可回读 ⇒ 保存后必须实测） |
| **不得省略** | `V-2` 后仍须用**一条测试消息**跑通 `EMQX → 中转 → Worker → D1` 全链，才算 `V-1` 完全闭合 |

---

## §8 下游影响

| 对象 | 变化 |
|---|---|
| **`H-WP4C-11`** | 🔴 `OPEN` → ✅ **`CLOSED`**（部署 + 验证 + 可复现脚本） |
| **`H-WP4C-09-B`**（EMQX Rule Implementation） | 🔴 `BLOCKED` → 🟡 **`READY`**（**两条依赖均已闭合**：`-09-A = CLOSED WITH GUARD` · `H-WP4C-11 = CLOSED`）—— **执行仍须人工控制台**（代理无 EMQX 控制台/API 权限） |
| **纪律 §6A.4 `STOP` 条件 1**（目标不可达） | ✅ **不再成立**（中转已部署 · 同 host 已被 EMQX 生产验证） |
| **`WP4C-IMPL:C-5`**（Rule deployment） | 🔴 `BLOCKED` → 仍 `BLOCKED`，但**唯一依赖收敛为「人工执行 `H-WP4C-09-B`」** |
| **`CF-2`** | 🔴 仍 `BLOCKED`（判据 = `V-1…V-7` **全项**；现仅 `V-1` 部分达成） |
| **`R-WP4C-15`** | 🟠 保持 —— **新增证据**：`…/log` 主题 **0 行**（§6.3），候选机制获支持 |
| **新增登记** | **`R-WP4C-17`**：`INSERT OR IGNORE` 的**幂等冲突仍消耗 `AUTOINCREMENT`**（§4 注）—— 影响 `id` 连续性，**不影响行数/语义**；若未来按 `id` 增量做对账须注意 |

---

## §9 修改文件

| 文件 | 仓 | 性质 |
|---|---|---|
| `guo-feeder-pagesdev/functions/api/history/ingest.js` | CF | **新增** —— Pages 中转函数（136 行） |
| `scripts/smoke-history-relay.mjs` | CF | **新增** —— 部署后冒烟测试（只读 / `--write` 两态） |
| `README.md` | CF | **更新** —— 记录新端点与冒烟脚本 |
| `docs/review/P3-WP4C-History-Relay-Deployment-Verification.md` | 固件 | **新增** —— 本文件 |
| `docs/review/P3-WP4C-Decisions.md` · `P3-WP4C-Implementation-Record.md` · `P3-WP4C-Implementation-Discipline.md` · `P3-WP4C-Action-Target-Decision.md` · `docs/README.md` | 固件 | **状态回填**（`H-WP4C-11` → `CLOSED`；`-09-B` → `READY`；`V-1` → `PASS`；`R-WP4C-15/17`） |
| `config/data-integration/P3-WP4C-V2V4-Config-After.md` · `README.md` | EMQX | **更新** —— 追加「中转已就绪」前置事实与冒烟结果 |

---

*本文档于 **2026-10-12** 建立，为 **`H-WP4C-11` 的部署与验证记录**；
**唯一的生产变更 = 1 次已授权的 Pages 部署**（另含合成测试行的写入与定点清理）。
**本工作包未创建 / 修改任何 EMQX Rule · Action · Connector，未改 Worker，未跑 migration，未改固件。**
基线 = 固件仓 **`ef49db5`** · CF 仓 **`9b25203`**（提交后 HEAD 前移）· EMQX 仓 **`b6bdbe2`**。所有事实均可逐条回查。*
