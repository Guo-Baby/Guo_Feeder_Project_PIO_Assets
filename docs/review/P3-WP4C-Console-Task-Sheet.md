# P3 · `WP4C` —— **EMQX 控制台施工任务单（`R3.1` · 照抄版）**

> ## 🔄 当前进度（`2026-10-12` 晚间更新）
>
> | 步骤 | 状态 |
> |---|---|
> | `1`–`5` 删 3 旧对象 + 建连接器 `CB` | ✅ **已做**（`CB = c-n302933b-0c1cb0` · URL 裸主机 · SNI 已填 · 4 头） |
> | `6`–`7` 建动作 ×2 | ✅ **已建 · 全链修复**（§7.5.7 第五轮终测实证）：secret 值修正后 history/ingest / webhook / presence/event **全部 200** |
> | `8` 建规则 A | ✅ **已建已启 = `r-n302933b-cf0cf2`**（`guo_feeder/+/up`） |
> | `9` 建规则 B | ✅ **已建已启 = `r-n302933b-f731cf`**（`guo_feeder/#`） |
> | `10` 验证 | ✅ **五轮探针 + tail 实证 + 冻结验证集 `V-1…V-7` 全部 `PASS`**（§7.5.7）⇒ **`CF-2` 判据满足 = `CLOSED`** |
> | `11` 回写 ID | 🟡 新 ID 已登入 `V2V4-Config-After`；**SQL 实际采用版本待你确认**（§11 登记表）· **presence 删旧建新后的新连接器/动作 ID 待你回传**（代理无 API 权限，看不到新对象） |
>
> **★ 本次修正（`R3.1`）= 只改 SQL 谓词写法**（`==` → 单 `=` 语义 / 改用官方 `is_null`&nbsp;·&nbsp;`is_not_null`）。
> **连接器、端点、请求体、TLS 的规格不变；`X-Webhook-Key` 已在「URL 路径 = `/api/history/ingest`」的动作 `a-n302933b-126ec6` 上（§7.5.6 tail 实证头已随请求到达），剩余 = 把**值**改成 `WEBHOOK_SECRET`（§7.5.6）。**
>
> ## 📋 一句话总览
>
> **删 3 个**（1 规则 + 1 动作 + 1 连接器）→ **建 4 个**（1 连接器 + 2 动作 + 2 规则）。
> **只影响"消息流"这一条链路**（本就 ≈90% 失败、best-effort）；**presence 的 4 个对象完全不动**。
> **预计 15 分钟**；**配额必须刚好吻合**（连接器 2/2 · 规则 4/4 · 动作 4/4）。
>
> | 项 | 值 |
> |---|---|
> | **执行人** | 你（EMQX 控制台人工）。代理**无**控制台 / 数据集成 API 权限（API 全 `403` · 无 CLI） |
> | **依据** | 冻结设计 `R3` + **`R3.1` SQL 修正**（`docs/review/P3-WP4C-EMQX-Rule-Design.md` §10/§11）+ 施工规范（`docs/review/P3-WP4C-Console-Build-Guide-R3.md`） |
> | **不要忘** | ① **`${payload}` 不加引号**（§5/§6）② **规则 SQL 用 `is_null` / `is_not_null`，不要 `==`**（§7.1）③ **保存前用页面调试功能试跑**（§7.0） |
> | **禁止** | ❌ 不动 presence 的任何对象 ❌ 不新建第 3 个连接器（配额满）❌ 不改 firmware / Worker / D1 ❌ 密钥值不写进任何文件 |
>
> **流水账（打勾用）**：
> `[ ] 1 抄旧统计` → `[ ] 2 删规则` → `[ ] 3 删动作` → `[ ] 4 删连接器` → `[ ] 5 建连接器 CB` →
> `[ ] 6 建动作 MF` → `[ ] 7 建动作 HI` → `[ ] 8 建规则 A 并启用` → `[ ] 9 建规则 B 并启用` →
> `[ ] 10 验证` → `[ ] 11 回写 ID`

---

## §0 本次要动的对象（一张表看清）

| 对象 | ID | 备注 | 处置 |
|---|---|---|---|
| 连接器（消息流） | **`c-n302933b-b12100`** | `GuoFeederToCloudflare` | 🔴 **删除**，由 **`CB`** 取代 |
| 动作（消息流） | **`a-n302933b-921e22`** | （空） | 🔴 **删除**，由 **`MF`** 取代 |
| 规则（消息流） | **`r-n302933b-a01720`** | （空） | 🔴 **删除**，由 **规则 B** 取代 |
| 连接器（presence） | `c-n302933b-291200` | `guo_feeder_presence_http` | ✅ **不动** |
| 动作（presence） | `a-n302933b-10fade` | `presence_connected` | ✅ **不动** |
| 动作（presence） | `a-n302933b-17ee9a` | `guo_feeder_presence_disconnected` | ✅ **不动** |
| 规则（presence） | `r-n302933b-3990d0` | `guo_feeder_presence_connected` | ✅ **不动** |
| 规则（presence） | `r-n302933b-18a660` | `guo_feeder_presence_disconnected` | ✅ **不动** |
| 连接器（新） | **`CB`** | `guo_feeder_shared_http` | 🟢 **新建** |
| 动作（新） | **`MF`** | `guo_feeder_message_ingest` | 🟢 **新建** |
| 动作（新） | **`HI`** | `guo_feeder_history_ingest` | 🟢 **新建** |
| 规则（新） | **规则 A** | `guo_feeder_history_ingest` | 🟢 **新建** |
| 规则（新） | **规则 B** | `guo_feeder_message_ingest` | 🟢 **新建** |

**依赖关系（决定了删除顺序）**：

```
c-n302933b-b12100  ←被引用←  a-n302933b-921e22  ←被引用←  r-n302933b-a01720
     （连接器）                    （动作）                     （规则）

★ 官方要求：「确保在删除连接器之前已清空该连接器下的所有规则」
⇒ 删除顺序**只能**是：规则 → 动作 → 连接器
```

**为什么要删掉重建**：EMQX Cloud 文档明写 **「编辑连接器 / 启用和禁用连接器 —— 该功能仅适用于专有版部署」** ⇒ Serverless 上连接器**建完就冻结**（不能改、不能停），而配额连接器 **2/2 已满** ⇒ 唯一办法是**删旧建新**。

---

## §1 【存档】旧对象原文（你提供过的，已整理 · 可直接核对）

### 1.1 要删的连接器 `c-n302933b-b12100`（= `GuoFeederToCloudflare`）

| 控制台字段 | 原值 |
|---|---|
| 连接器类型 | HTTP 服务 |
| 状态 | 已连接 |
| **URL** | **`https://guo-feeder-pagesdev.pages.dev/api/webhook`** |
| **请求头（JSON 原文）** | `{"accept":"application/json","cache-control":"no-cache","connection":"keep-alive","content-type":"application/json"}` |
| 含鉴权？ | ❌ **无**（没有 `X-Webhook-Key`） |
| 连接超时 | **`10s`** |
| HTTP 管道 | `50` |
| 连接池大小 | `1` |
| 连接池类型 | `random` |
| 健康检查间隔 | `30s` |
| 启动超时时间 | `5s` |
| 启用 TLS | **已启用** |
| SNI / 验证服务器证书 / CA | 🟠 **保存后平台不可回读**（这就是为什么要"保存前截图"） |
| 被引用规则数 | **1** |

### 1.2 要删的动作 `a-n302933b-921e22`

| 控制台字段 | 原值 |
|---|---|
| 备注 | （空） |
| 使用连接器 | `c-n302933b-b12100` |
| 动作类型 | 数据转发 - HTTP 服务 |
| 状态 | 可用 |
| 请求方法 | **`POST`** |
| **URL 路径** | **（空）** ← 端点路径写死在连接器 URL 里 |
| 请求头（动作层） | `content-type: application/json` |
| **请求体（逐字）** | 见下方代码块 |
| 高级设置 | 重试 `1` · 缓存池 `1` · 超时 `5s` · 健康检查 `30s` · 缓存队列 `4MB` · **异步** · 飞行窗口 `50` |

**旧请求体（逐字 · ★ `payload` 没有引号）**：

```json
{
  "topic": "${topic}",
  "clientid": "${clientid}",
  "payload": ${payload}
}
```

> ⚠️ 这段请求体**本次不动语义**（新动作 `MF` 照抄它），但要注意它**必须配合 `SELECT *`** ——
> `${topic}` / `${clientid}` / `${payload}` 这三个变量靠规则的 `SELECT *` 提供。

### 1.3 要删的规则 `r-n302933b-a01720`

| 控制台字段 | 原值 |
|---|---|
| 备注 | （空） |
| 启用 | ✅ 已启用 |
| 运行统计 | **触发 ≈ 19,000+ · 失败 ≈ 17,000+（≈90%）** ← ★ **删之前必须抄下来**（§2） |
| 关联动作 | `a-n302933b-921e22` |

**旧 SQL（逐字 · 无 WHERE）**：

```sql
SELECT * FROM "guo_feeder/#"
```

### 1.4 保留不动的 presence 三件（仅供对照，**不要动**）

| 对象 | 关键值 |
|---|---|
| 连接器 `c-n302933b-291200` | URL `https://guo-feeder-pagesdev.pages.dev/api/presence/event` · 请求头 `{"X-Webhook-Key":"<已配置>","content-type":"application/json"}` · 连接超时 **`5s`** · 其余同 §1.1 · 启用 TLS |
| 动作 `a-n302933b-10fade` / `a-…17ee9a` | `POST` · **URL 路径（空）** · 请求体 **裸 `${body}`** · 动作层头 5 条（`accept` / `cache-control` / `connection` / `content-type` / `keep-alive: timeout=5`） |
| 规则 `r-…3990d0` | `SELECT json_encode(map_put('connected_at', connected_at, map_put('peername', peername, map_put('username', username, map_put('clientid', clientid, map_put('event', 'client_connected', map_new())))))) AS body FROM "$events/client_connected"` |
| 规则 `r-…18a660` | `SELECT json_encode(map_put('disconnected_at', disconnected_at, map_put('reason', reason, map_put('peername', peername, map_put('username', username, map_put('clientid', clientid, map_put('event', 'client_disconnected', map_new()))))))) AS body FROM "$events/client_disconnected"` |

> ★ **对照要点**：presence 走的是「规则 `SELECT json_encode(…) AS body` + 动作体**裸 `${body}`**」；
> 消息流走的是「规则 `SELECT *` + 动作体**三键模板**」。**两种不能混用** —— 混用会让变量取空、投递必失败。

---

## §2 【施工前 · 唯一必做项】抄旧运行统计

> **为什么**：删除会**永久丢失**这些数字，它们是"这条链路到底有多不靠谱"的唯一量化证据（≈19,000 触发 / ≈17,000 失败 / ≈13% 成功）。

**在控制台**：规则列表 → 点 **`r-n302933b-a01720`** → **运行统计** → 抄下来（截图最好）：

| 项 | 值 |
|---|---|
| 触发次数 | `<抄这里>` |
| 成功次数 | `<抄这里>` |
| 失败次数 | `<抄这里>` |
| 规则创建时间 | `<抄这里>` |

顺手也可以对 **`a-n302933b-921e22`** 做同样的事（动作列表 → 点动作 → 运行统计）。

> 采集完之后，**下面就开始有中断窗口了**（消息流会短暂停止落库）—— 所以这一步要**先做完**。

---

## §3 【删除】按顺序删 3 个（🛑 中断窗口从这里开始）

> **顺序不能换**：规则 → 动作 → 连接器。
> **每步都必须输入对象 ID 确认**（EMQX 的删除确认方式）。

| 步 | 去哪 | 点 | 确认 |
|---|---|---|---|
| **3.1** | **数据集成 → 规则列表** | 找到 **`r-n302933b-a01720`** → 行末 **删除** | 输入 **`r-n302933b-a01720`** |
| **3.2** | **数据集成 → 动作列表**（或该规则设置页里的动作） | 找到 **`a-n302933b-921e22`** → **删除** | 弹窗确认 |
| **3.3** | **数据集成 → 连接器列表** | 找到 **`c-n302933b-b12100`** → 行末 **删除** | 输入 **`c-n302933b-b12100`** |

**如果 3.3 被拦住**（提示仍被引用）⇒ 说明 3.1/3.2 没删干净，回去检查该连接器的「关联规则数」是否为 **0**，为 0 再删。

**删完核对**：连接器列表应只剩 **1 个**（`c-…291200`）；规则列表剩 **2 条**；动作列表剩 **2 个**。

---

## §4 【新建】连接器 `CB`

**去哪**：数据集成 → **连接器列表** → 右上角 **新建连接器** → Web 服务分类下选 **HTTP 服务** → 填表 → **测试** → **新建**。

| 控制台字段 | **填这个** | 为什么 |
|---|---|---|
| **连接器名称** | `guo_feeder_shared_http` | 控制台若自动命名（`c-<…>`），就把这名字填进**备注** |
| **URL** | **`https://guo-feeder-pagesdev.pages.dev`** | ★ **裸主机**：无路径、**无尾部斜杠**（下面靠"动作的 URL 路径"拼出两个端点） |
| **请求头** | 加 4 行：<br>`accept` = `application/json`<br>`cache-control` = `no-cache`<br>`connection` = `keep-alive`<br>`content-type` = `application/json` | 就是旧连接器的 4 个头，**逐字照抄** |
| | ⚠️ **不要在这里填 `X-Webhook-Key`** | 放到 `HI` 动作层（§7），这样消息流链路的行为与今天**完全一致** |
| **启用 TLS** | **开** | 目标是 `https://` |
| **SNI** | **`guo-feeder-pagesdev.pages.dev`** | ★ **必须填**，不填会握手失败（`ssl/tls alert handshake failure`） |
| **验证服务器证书** | **关** | Cloudflare 会轮换签发 CA，开着要配 CA 文件 |
| **CA 证书 / 客户端证书 / 私钥** | **全部留空** | — |
| 连接超时 | `10s` | 照抄旧消息流连接器（presence 那个是 `5s`） |
| HTTP 管道 | `50` | 默认 |
| 连接池大小 | `1` | 默认 |
| 连接池类型 | `random` | 默认 |
| 健康检查间隔 | `30s` | 默认 |
| 启动超时时间 | `5s` | 默认 |
| OAuth2 客户端凭证 | **不开** | 我们用 `X-Webhook-Key` |

### ★ 两个动作不能漏

| # | 动作 | 怎么做 |
|---|---|---|
| **①** | **保存前截图 TLS 段** | 把 **启用 TLS / SNI / 验证服务器证书 / CA** 四项**同框截图** —— 因为**保存后平台不再显示这些字段**，这是唯一的留痕机会 |
| **②** | **保存后实测** | 点 **「测试」**（= 对根路径 `GET /`）⇒ 期望 **`200`**；连接器列表状态应为 **「已连接」**。<br>出现 `TLS` / `certificate` / `ssl` 字样 ⇒ **先回去查 SNI**。<br>（判定口径：只要能到 **HTTP 层**（200/401/400/405 都算），就说明握手已通） |

**★ 为什么 URL 必须写"裸主机"**：
官方文档原话 —— 动作的 **`URL 路径`**「**此路径将附加到连接器的 URL 配置中，以形成完整的 URL 地址**」。
所以：`https://guo-feeder-pagesdev.pages.dev` + `/api/webhook` = `…/api/webhook`（与旧链路的生效地址**一模一样**），
`+ /api/history/ingest` = `…/api/history/ingest`（新端点）。若 URL 写成带路径的，就拼不出这两个了。

**建完记下新 ID** → 填进 §11 登记表：

```
新连接器 CB 的 ID = c-n302933b-0c1cb0   ← ✅ 已建（2026-10-12，URL 裸主机 + SNI 已填 + 4 个头）
```

---

## §5 【新建】动作 `MF`（消息流 · 取代被删的 `a-…921e22`）

**去哪**：数据集成 → **动作列表** → **新建动作** → 连接器下拉选 **`CB`** → 填表 → **确认**。

| 控制台字段 | **填这个** |
|---|---|
| **动作名称 / 备注** | `guo_feeder_message_ingest` |
| **使用连接器** | **`CB`**（§4 新建的那个） |
| **请求方法** | **`POST`** |
| **URL 路径** | **`/api/webhook`** |
| **请求头** | **留空**（继承 `CB` 的 4 个头 ⇒ 与今天完全一致） |
| **请求体** | 见下方代码块 |
| 最大重试次数 | `1` |
| 缓存池大小 | `1` |
| 请求超时 | `5` 秒 |
| 健康检查间隔 | `30` 秒 |
| 缓存队列最大长度 | `4` MB |
| 请求模式 | **异步** |
| 请求飞行队列窗口 | `50` |

**请求体（一行版 · 直接粘贴）**：

```
{"topic":"${topic}","clientid":"${clientid}","payload": ${payload}}
```

> ★ **`${payload}` 前后不要加引号**。加了引号 ⇒ `payload` 变成字符串 ⇒ Cloudflare 侧 `400 unparsable_payload` ⇒ 投递必失败。
> ★ **生效 URL**：`https://guo-feeder-pagesdev.pages.dev` + `/api/webhook` = **与拆除前逐字相同**。

**建完记下 ID**：

```
新动作 MF 的 ID = a-______________________
```

---

## §6 【新建】动作 `HI`（history · 全新）

**去哪**：同 §5（动作列表 → 新建动作 → 选连接器 **`CB`**）。

| 控制台字段 | **填这个** |
|---|---|
| **动作名称 / 备注** | `guo_feeder_history_ingest` |
| **使用连接器** | **`CB`** |
| **请求方法** | **`POST`** |
| **URL 路径** | **`/api/history/ingest`** |
| **请求头** | 加 2 行：<br>`content-type` = `application/json`<br>`X-Webhook-Key` = **`<WEBHOOK_SECRET>`（填真实值，只填在控制台里）** |
| **请求体** | 与 `MF` **完全一样**（代码见上） |
| 高级设置（7 项） | 与 `MF` **完全一样**：重试 `1` · 缓存池 `1` · 超时 `5s` · 健康检查 `30s` · 缓存 `4MB` · **异步** · 窗口 `50` |

**请求体（一行版 · 直接粘贴）**：

```
{"topic":"${topic}","clientid":"${clientid}","payload": ${payload}}
```

> ★ **这里是唯一需要 `X-Webhook-Key` 的地方**（`/api/history/ingest` 是鉴权端点，没 key 必返 `401`）。
> 值 = 你在 Cloudflare Worker 里配的 `WEBHOOK_SECRET`（`wrangler secret put WEBHOOK_SECRET` 的那个值）。
> **只填在控制台，不要写进任何仓库文件或聊天外的文档。**
>
> **备用做法（万一动作页没有"请求头"字段）**：把 `X-Webhook-Key` 加回 **`CB` 连接器**的请求头里。
> 副作用：消息流请求也会带上这个头 —— 对**没有鉴权**的 `/api/webhook` **没有行为影响**，可以接受。
> ⚠️ **不要两处都配同名头**。

**建完记下 ID**：

```
新动作 HI 的 ID = a-______________________
```

---

## §7 【新建】规则 2 条（★ 顺序：先 A 后 B）

**去哪**：数据集成 → **规则列表** → **新建规则** → 在 **SQL 编辑器**粘贴 SQL →（可选：**启用调试** 先试跑）→ **下一步** → **使用连接器选 `CB`** → 选/填对应动作 → **确认** → 回规则列表 → 打开 **是否启用** 开关。

### 7.1 规则 A `guo_feeder_history_ingest`（**先建 · 先启用**）

| 项 | 值 |
|---|---|
| 名称 / 备注 | `guo_feeder_history_ingest` |
| **FROM** | `guo_feeder/+/up` |
| **SELECT** | `*` |
| **WHERE** | 冻结谓词（下方 SQL 已含） |
| 关联动作 | **`HI`** |
| 启用 | ✅ **是** |

> ### ⚠️ `R3.1` 修正（`2026-10-12` · **必读：上次报错是我 SQL 写错了**）
>
> 你上次保存失败与操作无关，是我提供的 SQL 有 **2 处错误**：
>
> | # | 错在哪 | 事实（官方） | 改成 |
> |---|---|---|---|
> | 1 | 我写了 **`==`** | EMQX 规则 SQL 的"相等"**只有单个 `=`**（`=` = "Check if the two operands are completely equal"）；**`==` 不存在** ⇒ 被拆成两个 `=` token ⇒ `syntax error before: '='`（正是你贴的报错）。`!=` 是**合法**的 | `c == 'undefined'` → **`is_null(payload.c)`** |
> | 2 | 我用 **`!= 'undefined'`** 判断"键是否存在" | 官方正确写法是内置函数 **`is_null()` / `is_not_null()`**：`is_null(Term)` = 判断 Term **未赋值 / undefined**；`is_not_null(Term)` = 反之。**路径中间字段缺失也返回 `true`**（EMQX 维护者原话：`is_null(payload.t.field)` 输入 `{"msg":"hello"}` ⇒ `true`） | 全部改用 `is_null` / `is_not_null` |
>
> ⇒ 下面 SQL 已全部改用 **`is_null` / `is_not_null`**。**保存前先用页面自带的调试功能试跑**（见 §7.0）。

### 7.0 【试跑】保存前先验证谓词（1 分钟）

规则编辑页打开 **启用调试**，用页面的"发送测试消息"，**每条规则各跑两条**（topic 用 `guo_feeder/ffffffffffff/up`）：

| 测试消息 | 规则 A 期望 | 规则 B 期望 |
|---|---|---|
| `{"msg": "hello"}`（**非** telemetry） | **不匹配** | **匹配** |
| `{"ts":1799999991,"type":"temperature","value":21.5,"seq":1,"schema_version":1}` | **匹配** | **不匹配** |

两条都符合期望 ⇒ 再点"下一步"选动作保存。

---

**SQL（首选写法 · 整段复制粘贴）**：

```sql
SELECT
  *
FROM
  "guo_feeder/+/up"
WHERE
      is_not_null(payload.schema_version)
  AND is_not_null(payload.seq)
  AND is_not_null(payload.ts)
  AND is_not_null(payload.type)
  AND is_not_null(payload.value)
  AND is_null(payload.c)
  AND is_null(payload.cmd)
```

> ★ `is_not_null(x)` = **x 存在**；`is_null(x)` = **x 不存在**。
> **不要**改写成 `x != 0` / `x != ''` 之类（否则 `value = 0`、`false` 会被漏掉 —— 冻结铁律 1）。

**建完记下 ID**：

```
规则 A 的 ID = r-______________________
```

### 7.2 规则 B `guo_feeder_message_ingest`（**后建 · 后启用**）

| 项 | 值 |
|---|---|
| 名称 / 备注 | `guo_feeder_message_ingest` |
| **FROM** | `guo_feeder/#` ← **与旧规则逐字相同** |
| **SELECT** | `*` ← **与旧规则逐字相同** |
| **WHERE** | **"非 telemetry"**（= 把 A 的条件整体取反）← ★ 一步到位，这样**不会双投** |
| 关联动作 | **`MF`** |
| 启用 | ✅ **是**（建完这一步，**中断窗口结束**） |

**SQL（首选写法 · 整段复制粘贴）**：

```sql
SELECT
  *
FROM
  "guo_feeder/#"
WHERE
      is_null(payload.schema_version)
   OR is_null(payload.seq)
   OR is_null(payload.ts)
   OR is_null(payload.type)
   OR is_null(payload.value)
   OR is_not_null(payload.c)
   OR is_not_null(payload.cmd)
```

> **语义**：只要"五个 telemetry 键**有任一缺失**"**或**"出现了命令键 `c` / `cmd`" ⇒ 就不是 telemetry ⇒ 交给消息流动作 `MF`。
> `AND` 优先级高于 `OR` ⇒ **不需要括号**（也正好避开括号子集风险）。**只用到 `OR` / `AND` / `is_null` / `is_not_null`，全是官方文档列出的写法。**

**SQL（备选写法 · 与上面完全等价 · 仅当你的版本不支持 `OR` 时再试）**：

```sql
SELECT
  *
FROM
  "guo_feeder/#"
WHERE
  NOT (
        is_not_null(payload.schema_version)
    AND is_not_null(payload.seq)
    AND is_not_null(payload.ts)
    AND is_not_null(payload.type)
    AND is_not_null(payload.value)
    AND is_null(payload.c)
    AND is_null(payload.cmd)
  )
```

**SQL（最后备选 · 若 `is_null` 报"未知函数"再用这版 · 注意 `=` 是单等号）**：

```sql
SELECT
  *
FROM
  "guo_feeder/#"
WHERE
      payload.schema_version = 'undefined'
   OR payload.seq            = 'undefined'
   OR payload.ts             = 'undefined'
   OR payload.type           = 'undefined'
   OR payload.value          = 'undefined'
   OR payload.c              != 'undefined'
   OR payload.cmd            != 'undefined'
```

**建完记下 ID**：

```
规则 B 的 ID = r-______________________
```

### 7.3 三条不能搞错的语义（冻结 · 别自行改动）

| # | 铁律 |
|---|---|
| 1 | `value` 判的是"**这个键存不存在**"，**不要**写成"值非空 / 值 ≠ 0"（否则 `value = 0`、`false` 会被漏掉） |
| 2 | 谓词里**不要**出现 `device_id`（设备身份只来自 topic） |
| 3 | **不要**只判 `type` / `value` / `topic` 中的一两个 —— 那五个正条件缺一不可 |

> ⚠️ 若规则保存仍报**语法错** ⇒ **停手**，把控制台报错原文发我。
> 若报的是 **`unknown function is_null`** ⇒ 改用上面的**最后备选版**（`= 'undefined'`，**单等号**），并把"用了哪一版"告诉我。
> 若报的是 **`NOT` / 括号** 不支持 ⇒ 用**首选版**（首选版不含 `NOT`、不含括号）。

### 7.4 ⚠️ 你在动作页填错了两处（截图已证实 · 必须改正）

你截图里那个动作 **备注 `guo_feeder_message_ingest` · URL 路径 `/api/webhook`**，但它是在**规则 A（`guo_feeder_history_ingest`）**下新建的 ⇒ **归属错了**：

| 字段 | 你填的 | 应该填 | 为什么 |
|---|---|---|---|
| **备注** | `guo_feeder_message_ingest` | **`guo_feeder_history_ingest`** | 这是 **`HI`** 动作（history 域），不是消息流那个 |
| **URL 路径** | `/api/webhook` | **`/api/history/ingest`** | 规则 A 的报文必须进 **`telemetry_history`**；填 `/api/webhook` 会全落进 `mqtt_messages`（**双投 + 分流失效**） |

**正确对应关系**：

```
规则 A  guo_feeder_history_ingest  →  动作 HI  路径 /api/history/ingest  备注 guo_feeder_history_ingest  （头带 X-Webhook-Key）
规则 B  guo_feeder_message_ingest  →  动作 MF  路径 /api/webhook         备注 guo_feeder_message_ingest  （头留空）
```

> **两个动作都在同一个新建流程里做**：先建规则 A → 选连接器 `CB` → 建动作 `HI`（路径 `/api/history/ingest`）→ 保存；
> 再单独新建规则 B → 选连接器 `CB` → 建动作 `MF`（路径 `/api/webhook`）→ 保存。
> **别把 `MF` 的路径填到 `HI` 上**（这是上次唯一容易混的地方）。

---

### 7.5 【实测诊断 · `2026-10-12` 18:30–19:05】代理侧端到端探针（**无需你操作 · 已完成的测试**）

> **结论先行**：**两条规则的 SQL 都工作正常**；**两个动作的 URL 路径相对规则交叉了**
> ⇒ telemetry 落进 `mqtt_messages`、非 telemetry 被 `/api/history/ingest` 静默拒绝。
> **修复 = 对调两个动作的 URL 路径**（§7.5.3）· **规则 SQL 一行不动**。

#### 7.5.1 探针方法（可复现 · 合成数据 · 零设备副作用）

| 手端 | 做法 |
|---|---|
| MQTT 合成发布 | 账号 `workbuddy`（ACL `all/allow guo_feeder/#`）· TLS `8883` · 合成 device **`ffffffffffff`**（非真实设备）· QoS1 + **严格 PUBACK 校验** |
| D1 只读回读 | `wrangler d1 execute guofeeder --remote --config guo-feeder-api/wrangler.toml`（wrangler OAuth） |
| 端点直击 | `curl` + `X-Webhook-Key`（值只从 `.secrets.local` 读入，**不打印**） |

#### 7.5.2 证据表（生产 D1 实测 · 测试行已全部清除）

| # | 合成报文（topic 均为 `guo_feeder/ffffffffffff/…`） | 应走 | 实际落点 | 判定 |
|---|---|---|---|---|
| T1 | `/up` `{"ts":…,"type":"wp4c_probe","value":42,"seq":91021,"schema_version":1}` | 规则 A → HI → `telemetry_history` | **`mqtt_messages` id=2460** | ❌ **规则 A 的动作 = `/api/webhook`**（seq 91031 二次复现 id=2462） |
| T2 | `/up` 同左但 `value=null`（seq 91022） | 规则 A → HI | **`mqtt_messages` id=2461** | 规则 A **命中**（JSON null 被判「键存在」）· 同样落错表 |
| T3 | `/up` `{"msg":"hello"}` | 规则 B → MF → `mqtt_messages` | **无** | ❌ **规则 B 的动作不是 `/api/webhook`** |
| T4 | `/up` `{"c":"system","i":"wp4cprobe","p":{"o":"config_query"}}`（命令回包形态） | 规则 B → MF | **无** | 同上 |
| T5 | `/down` `{"cmd":"wp4c_probe"}` | 规则 B → MF | **无** | 同上 |
| T6 | 合法 telemetry **直击** `/api/history/ingest`（带 key） | — | **`{"accepted":1}`** | ✅ **HI 端点 + key 完全健康** ⇒ 问题 100% 在 EMQX 动作 |
| T7 | `{"msg":"hello"}` 直击 `/api/history/ingest`（带 key） | — | **`{"accepted":0,"rejected":1}`（HTTP 200）** | 非 telemetry 到 HI = **静默拒绝、零落库** ⇒ 解释 T3–T5 为何失踪 |
| T8 | `/log` **二进制载荷**直击 `/api/webhook` | — | **`Invalid JSON` [400]** | ✅ **旧链路 ≈90% 失败主因实锤**（`R-WP4C-15` H-1/H-2）：CBOR ⇒ 非法 JSON ⇒ 400。修复后 `/log` 仍会 400（与旧链路同款，**已知 parity 风险，不归本包修**） |

> **附带发现 ①**：`test001` 账号探针期间**认证被拒**（`Bad user name or password`，v3.1.1/v5 均拒；同期 `workbuddy` 正常）⇒ 疑被改密 / 禁用 / 闪断封禁。**不影响本包结论**（全部实测已用 `workbuddy` 完成）；后续要用 `test001` 须人工核验重置。
> **附带发现 ②**：早前一轮探针的 `PUB OK` 曾为 **paho 假阳性**（`wait_for_publish` 超时会静默返回）⇒ 本轮已改用 `is_published()` 严格校验，并以 **D1 落库**为唯一准绳。
> **卫生**：全部测试行已清除 —— `mqtt_messages` 回到 **2458** · `telemetry_history` **0** · `log_history` **0**（与探针前基线逐项一致）。

#### 7.5.3 修复（**只对调两个动作的 URL 路径**）

**目标态**：

```
规则 A（guo_feeder/+/up · history）→ 它的动作 ⇒ URL 路径 /api/history/ingest + 头 X-Webhook-Key
规则 B（guo_feeder/#   · message）→ 它的动作 ⇒ URL 路径 /api/webhook
```

| 步 | 对象（打开它的编辑页） | 改成 |
|---|---|---|
| 1 | 动作 **`a-n302933b-126ec6`**（属规则 A · 备注 `guo_feeder_message_ingest`） | **URL 路径** `/api/webhook` → **`/api/history/ingest`**；**请求头加一行** `X-Webhook-Key` = `<WEBHOOK_SECRET>`（`content-type` 保留） |
| 2 | 动作 **`a-n302933b-7241e2`**（属规则 B · 备注 `guo_feeder_history_ingest`） | **URL 路径** `/api/history/ingest` → **`/api/webhook`**；请求头**去掉** `X-Webhook-Key`（留着无害，建议去掉） |
| 3 | 备注（可选） | 若可编辑，改成与规则一致（history 归 A 的动作 / message 归 B 的动作）；不可编辑就维持，**以本表为准** |
| 4 | 若动作页**不可编辑或保存报错** | **删除该动作 → 从对应规则页重建**（配额：动作 2/4 有富余；请求体三键模板逐字照抄 §5/§6） |

> ⚠️ **不要**为此改规则 SQL / FROM（已实测正确）· **不要**动连接器 `CB`（Serverless 不可编辑，删重建代价大）。
> **改完告诉我** ⇒ 我重跑 §7.5.1 探针复测，**期望：`telemetry_history` +1 · `mqtt_messages` +4**（T1–T5 全部归位）。

#### 7.5.4 【复测 · `2026-10-12` 19:30–19:50】对调后的第二轮探针

> **你已完成 §7.5.3 的 URL 路径对调**；复测结论 = **规则 B 侧修复 ✅ · 规则 A 侧仍不落库 🔴（差 `X-Webhook-Key` 头）**。

| # | 合成报文（topic 均为 `guo_feeder/ffffffffffff/…`） | 期望 | 实际 | 判定 |
|---|---|---|---|---|
| R1 | `/up` 合法 telemetry（seq 91101） | 规则 A → HI → `telemetry_history` +1 | **无**（`th=0` · `mqtt_messages` 亦无） | 🔴 **规则 A 的动作没带 `X-Webhook-Key`** ⇒ HI 返 `401` ⇒ 零落库 |
| R2 | `/up` `{"msg":"hello_swap"}` | 规则 B → `/api/webhook` → `mqtt_messages` +1 | **`mqtt_messages` id=2463** | ✅ **对调生效** |
| R3 | `/down` `{"cmd":"wp4c_swap_check"}` | 规则 B → `/api/webhook` → `mqtt_messages` +1 | **`mqtt_messages` id=2464** | ✅ **对调生效** |
| R4 | `/log` 二进制载荷 | 规则 B → `/api/webhook` → 400 | **无落库** | ✅ 符合预期（`Invalid JSON` · `CF-1` 已知，不归本包修） |
| R5 | 合法 telemetry **直击** HI（带 key） | `{"accepted":1}` | **`{"accepted":1,"duplicate":0,"rejected":0}`**（落 `telemetry_history` id=12，已清） | ✅ **端点 + Worker + D1 全链健康** ⇒ 问题 100% 在动作请求头 |
| R6 | 直击 HI **无 key** | `401` | **`401`** | ✅ 鉴权按预期拦截 ⇒ **反证 R1 失败原因 = 动作缺 key** |

**修复（只差一步）**：打开动作 **`a-n302933b-126ec6`**（属规则 A `guo_feeder/+/up`）→ 请求头**加一行** `X-Webhook-Key` = `<WEBHOOK_SECRET>`（与 `content-type` 并存）→ 保存。**规则 SQL / 连接器 `CB` / 另一动作都不用动。**
（`a-n302933b-7241e2` 上若残留 `X-Webhook-Key` 留着无害；想去掉也可。）
> 若加头后仍不落库 ⇒ 再查该动作是否被禁用 / 请求体三键模板是否完好（§5 逐字对照）。
**改完告诉我** ⇒ 再跑一轮探针，**期望：`telemetry_history` +1 · `mqtt_messages` +2**（R1 归位 + R2/R3 复现）。
**卫生**：本轮测试行已全部清除 —— `mqtt_messages` **2458** · `telemetry_history` **0** · `log_history` **0**（与探针前基线逐项一致）。
> 附注：`mqtt_messages` id 2460–2462 被消耗但无对应行（插入被忽略/失败仍推进行 id，与 `R-WP4C-17` 同机制）—— 不影响结论。

---

#### 7.5.5 【终测 · `2026-10-12` 21:20】补头后的第三轮探针

> **你已给两个动作「一个带 secret、一个不带」**；终测结论 = **规则 B 侧仍 ✅ · 规则 A 侧仍 🔴 零落库** ⇒ **secret 加错了动作**（极大概率）。

| # | 合成报文（topic 均为 `guo_feeder/ffffffffffff/…`） | 期望 | 实际 | 判定 |
|---|---|---|---|---|
| F1 | `/up` 合法 telemetry（seq 91201） | 规则 A → HI → `telemetry_history` +1 | **无**（`th=0` 持续 35 s+） | 🔴 **HI 动作仍没带上 key** |
| F2 | `/up` `{"msg":"hello_final"}` | 规则 B → `/api/webhook` → `mqtt_messages` +1 | **id 2465** | ✅ |
| F3 | `/down` `{"cmd":"wp4c_final_check"}` | 规则 B → `/api/webhook` → `mqtt_messages` +1 | **id 2466** | ✅ |

**诊断**：端点健康已两轮实锤（带 key `{"accepted":1}` · 无 key `401`）⇒ 问题 100% 在动作请求头。
**最可能原因 = secret 加到了错的动作上**：两个动作的**备注是交叉的**（`a-n302933b-126ec6` 备注 `guo_feeder_message_ingest` 却 serve history；`a-n302933b-7241e2` 备注 `guo_feeder_history_ingest` 却 serve message）—— 按备注找 "history" 就会加错。
**修复（★ 按 URL 路径认动作，别按备注）**：

| 动作 | 归属规则 | URL 路径（唯一准绳） | `X-Webhook-Key` 应为 |
|---|---|---|---|
| `a-n302933b-126ec6` | 规则 A `guo_feeder/+/up` | **`/api/history/ingest`** | **必须有** |
| `a-n302933b-7241e2` | 规则 B `guo_feeder/#` | **`/api/webhook`** | 不需要（残留无害） |

打开两个动作编辑页**逐一核对「URL 路径 + 请求头」**；把 key 挪到路径 = `/api/history/ingest` 的那个动作上。
**改完告诉我** ⇒ 再跑一轮（期望 `telemetry_history` +1 · `mqtt_messages` +2）。
**卫生**：本轮测试行已清除（`mqtt_messages` 2458 · `telemetry_history` 0 · `log_history` 0）。

#### 7.5.6 【第四轮终测 · `2026-10-12` 21:50】★ Worker 实时日志实证（`wrangler tail`）

> **★ 新诊断通道（本轮创立）**：`wrangler tail` **直连被墙**（DNS 投毒），但**走本机代理 `http://127.0.0.1:7890` 可用**：
> ```bash
> cd Cloudflare_Assets
> export https_proxy=http://127.0.0.1:7890 http_proxy=http://127.0.0.1:7890
> ./node_modules/.bin/wrangler tail guo-feeder-api --config guo-feeder-api/wrangler.toml --format json
> ```
> 输出**每个请求的 URL / 方法 / 内容长度 / 响应状态码**（`x-webhook-key` 显示 REDACTED）—— EMQX 动作到底发没发、发成什么样，**一眼看穿**。此后排障优先用此通道。

本轮探针（probe7）：MQTT 客户端 ID = **`dev_ffffffffffff`**（合成设备，`CLAIM_PENDING`，无真实硬件 ⇒ presence 事件可安全落库再清理）+ T1–T4 四向量 + 连接/断开（触发 presence 两条规则）。

**Worker 实收 5 个请求（tail 逐字证据）**：

| # | 请求 | 长度 | 状态 | 解读 |
|---|---|---|---|---|
| 1 | `POST /api/presence/event`（连接事件） | 144 B | **401** | 规则 P-1 ✓ 触发 · body ✓ 渲染 · `X-Webhook-Key` 头 ✓ 在 ⇒ **值不对** |
| 2 | `POST /api/history/ingest`（T1 telemetry） | 163 B | **401** | 规则 A ✓ 触发 · 信封 ✓ 完整 · key 头 ✓ 在 ⇒ **值不对** |
| 3 | `POST /`（T2 hello → webhook） | 101 B | **200** | 规则 B 全链 ✅ |
| 4 | `POST /`（T3 down → webhook） | 108 B | **200** | 规则 B 全链 ✅ |
| 5 | `POST /api/presence/event`（断开事件） | 168 B | **401** | 规则 P-2 ✓ 触发 · body ✓ 渲染 · key 头 ✓ 在 ⇒ **值不对** |

**结论（★ 推翻 §7.5.5 的「加错动作」判断）**：secret **已经在对的动作上**（key 头随请求到达 Worker）—— **唯一剩余问题 = `X-Webhook-Key` 的值与 Worker 密钥不匹配**。规则 SQL、URL 路径、动作 body 模板、presence 两条规则+动作**全部正确**（头在、body 长度符合预期信封）。

**旁证**：同一把正确值（`Cloudflare_Assets/.secrets.local` 的 `WEBHOOK_SECRET=`，40 字符）直击两端点：HI ⇒ `{"accepted":1,"duplicate":0,"rejected":0}` · presence ⇒ `200 {"ignored":"not_device_identity"}`（鉴权通过、按身份忽略，链路通的铁证）。`.secrets.local` 仅两把钥匙：`WEBHOOK_SECRET` / `ADMIN_API_TOKEN` —— **极可能又把 `ADMIN_API_TOKEN` 当成了 webhook 密钥**（本项目已犯过同款错误）。

**剩余修复（改两处值，无中断窗口）**：

| 位置 | 对象 | 改成 |
|---|---|---|
| 动作请求头 | `a-n302933b-126ec6`（规则 A · 路径 `/api/history/ingest`） | `X-Webhook-Key` = `WEBHOOK_SECRET` 的**值**（40 字符，无引号无空格） |
| 连接器/动作请求头 | presence 链（连接器 `c-n302933b-70f9da`，或动作层，看当时建在哪） | 同上 |

**卫生**：本轮测试行已全部清除，D1 基线恢复（`th=0` · `lh=0` · `mm=2458` · `device_event(ffffffffffff)=0` · `last_seen=1791811429`）。**改完告诉我 ⇒ 我跑第五轮（期望：T1 ⇒ `telemetry_history` +1 · presence 两条 ⇒ `device_event` online/offline 各 1 行）**。

---

## §8 【验证】4 项快检（做完就有结论）

| # | 检查 | 期望 |
|---|---|---|
| **8.1** | 规则列表 | 4 条规则全部启用；新两条的**失败率应该和 presence 一个量级**（**不应**是 90%） |
| **8.2** | 连接器列表 | `CB` 状态 **「已连接」**，且**没有** `resource down` / `alarm_is_activated` 告警反复出现 |
| **8.3** | 手工 POST 端点（浏览器/终端） | `POST https://guo-feeder-pagesdev.pages.dev/api/history/ingest` **不带 key** ⇒ **`401`**（= 链路通到 Worker 了） |
| **8.4** | 数据落库 | 让设备正常上报一会儿，然后在 `Cloudflare_Assets` 目录执行下面的命令，看两张表是否**开始增长** |

```bash
cd Cloudflare_Assets
unset http_proxy https_proxy HTTP_PROXY HTTPS_PROXY ALL_PROXY all_proxy
./node_modules/.bin/wrangler d1 execute guofeeder --remote --config guo-feeder-api/wrangler.toml \
  --command "SELECT (SELECT COUNT(*) FROM telemetry_history) AS th, (SELECT COUNT(*) FROM log_history) AS lh, (SELECT COUNT(*) FROM mqtt_messages) AS mm;"
```

**基线对照（变更前实测值）**：

| 表 | 变更前 | 变更后应 |
|---|---|---|
| `telemetry_history` | **0** | **开始 +1、+1 …**（telemetry 报文） |
| `log_history` | **0** | 视设备是否上报 log 域 |
| `mqtt_messages` | **2458** | 继续增长（近 24 h 只有 13 行 ⇒ 看**增量**，别看总量） |

> ★ **`mqtt_messages` 噪声提醒**：这条链路本就 ≈13% 成功率。若某次命令没 `+1`，**先对照既有成功率**再判断是不是我们引入的问题。

---

## §9 【回滚】（万一要退）

> ⚠️ **旧对象已删除、ID 不复用 ⇒ 无法"恢复原样"**，只能**用新 ID 重建等价物**。

| 步 | 做什么 |
|---|---|
| **1** | **停新链路**：停用/删除规则 A `guo_feeder_history_ingest` |
| **2** | **恢复消息流**：新建一个临时连接器，URL 直接写**全路径** `https://guo-feeder-pagesdev.pages.dev/api/webhook` → 新建动作（**URL 路径留空**，请求体用旧的三键模板）→ 新建规则（`SELECT * FROM "guo_feeder/#"`，**不加 WHERE**）→ 启用 |
| **3** | 校验：`mqtt_messages` 30 分钟内增量 ≥1；`telemetry_history` 停止增长 |
| **4** | 新对象**建议保留不删**（便于复查） |

---

## §10 【时间线】窗口在哪（心里有数）

```
[窗口外] §2 抄旧运行统计（慢慢做，不着急）
              ▼
🛑 窗口开始   §3 删除 3 个（规则 → 动作 → 连接器）
              §4 建连接器 CB（含截图 TLS + 测试）
              §5 建动作 MF
              §6 建动作 HI
              §7 建规则 A 并启用（telemetry 先通）
              §7 建规则 B 并启用
              ▲
✅ 窗口结束   §8 验证

窗口 ≈ 15 分钟；期间"消息流"不落库（该链路 ≈4–70 条/天 ⇒ 预计丢 0–1 条）；
presence / D1 / Worker / Pages / 设备 MQTT 连接 全部不受影响。
```

---

## §11 【回写】新 ID 登记表（施工后填，发我即可）

```
变更窗口：  从 ____:____  到 ____:____（GMT+8）
操作者：    ____________
实际窗口时长：____ 分钟

新连接器 CB   = c-______________________
新动作   MF   = a-______________________
新动作   HI   = a-______________________
规则 A (history) = r-______________________
规则 B (message) = r-______________________

规则 B 实际采用：  [ ] 首选(is_null + OR)   [ ] 备选(NOT + 括号)   [ ] 最后备选(= 'undefined' 单等号)
TLS 段截图存放：   ____________

被删对象（存证）：
  r-n302933b-a01720  触发 ____ / 成功 ____ / 失败 ____
  a-n302933b-921e22  触发 ____ / 成功 ____ / 失败 ____
```

---

## §12 常见卡点速查

| 现象 | 原因 / 怎么办 |
|---|---|
| 建连接器点「测试」报 `TLS handshake` / `certificate` | **SNI 没填或填错** ⇒ 填 `guo-feeder-pagesdev.pages.dev` |
| 删除连接器被拒 | 它下面还有规则引用 ⇒ 回到 §3 先删规则、再删动作 |
| 动作生效地址不对 | `URL 路径` 与连接器 URL **拼错** —— 连接器必须是裸主机 `https://guo-feeder-pagesdev.pages.dev` |
| 规则保存报 **`syntax error before: '='`** | ★ **上次就是这个** —— 原因 = SQL 里写了 **`==`**（EMQX 只有单 `=`）⇒ **改用 §7.1/§7.2 的 `is_null` / `is_not_null` 版** |
| 规则保存报 `unknown function is_null` | 用 §7.2 的**最后备选版**（`= 'undefined'`，**单等号**） |
| 规则保存报其它语法错 | 换 §7.2 **备选版**（`NOT` + 括号）；仍失败 ⇒ **停手**，把报错原文发我 |
| 规则保存成功但调试测试结果和 §7.0 期望不符 | **先别保存**，把"测试消息 + 实际匹配结果"发我 |
| `/api/history/ingest` 一直被打回 `401` | 动作 `HI` 的 `X-Webhook-Key` 没填 / 值与 Worker 的 `WEBHOOK_SECRET` 不一致 |
| `mqtt_messages` 增长异常偏少 | 该链路**本就 ≈13% 成功率**（既有问题，本次不修）；看**窗口内增量**并与既有成功率对照 |
| 全部成功但 `telemetry_history` 仍是 0 | 设备侧 `GF_TELEMETRY_ENABLED` 编译开关 + `telemetry.enable` 运行时开关是**两道门**，需都开才会发 telemetry —— 但**不影响本次规则验证**（可先手工 POST 造一条） |

---

## §13 边界声明

| 项 | 状态 |
|---|---|
| 本文件性质 | **人工作业任务单** —— 代理**不能**执行（无控制台 / 数据集成 API 权限） |
| 代理本次做了什么 | **仅写文档**：未创建/修改任何 EMQX 规则 · 动作 · 连接器；未部署；未改 firmware / Worker / D1 |
| 密钥 | 本文件**不含任何密钥值**（`X-Webhook-Key` 用占位符 `<WEBHOOK_SECRET>`） |
| 关联文档 | 施工规范与平台依据 → `docs/review/P3-WP4C-Console-Build-Guide-R3.md` · 设计 → `P3-WP4C-EMQX-Rule-Design.md` §10 · 变更前基线 → `EMQX_Assets/config/data-integration/P3-WP4C-V0-EMQX-Baseline.md` · 回写目标 → `…/P3-WP4C-V2V4-Config-After.md` |

---

### 7.5.7 【第五轮终测 + 冻结验证集】`2026-10-12` 22:00–22:20 · **全绿 ⇒ `CF-2` 判据满足**

**背景**：人工将 presence 连接器**删旧建新**（旧连接器无法修改请求头）并填入正确 secret；history 动作的 secret 值同步修正。

**第五轮终测**（probe7 · 合成设备 `dev_ffffffffffff` · tail 实收 5 请求 + D1 回读）：

| # | 请求 | 长度 | 状态 | 解读 |
|---|---|---|---|---|
| 1 | `POST /api/presence/event`（连接） | 144 B | **200** | presence 链全通 ✅ |
| 2 | `POST /api/history/ingest`（telemetry） | 163 B | **200** | 规则 A 历史链全通 ✅ |
| 3 | `POST /`（hello → webhook） | 101 B | **200** | 规则 B 消息链 ✅ |
| 4 | `POST /`（down → webhook） | 108 B | **200** | 规则 B 消息链 ✅ |
| 5 | `POST /api/presence/event`（断开） | 168 B | **200** | presence 链全通 ✅ |

D1 回读：`telemetry_history` **+1**（seq `91301`）· `device_event` **online/offline 各 1 行**（`event_id` = `emqx:dev_ffffffffffff:{connected,disconnected}:<ts>` · 含 `peername`/`reason`）· `last_seen` 刷新。⇒ **两条修复链全部实证恢复**。

**冻结验证集 `V-1…V-7` 正式执行**（probe8/probe9 · `P3-WP4C-EMQX-Rule-Design.md` §7 判据）：

| # | 用例 | 操作 | 实测 | 判定 |
|---|---|---|---|---|
| `V-1` | Rule syntax | 两规则保存+启用 | 均已保存启用且正常触发（tail 实证） | ✅ **`PASS`** |
| `V-2` | telemetry positive | A 发合法 telemetry（ts1001/seq92001） | `th(A)+1` · `mm+0` | ✅ **`PASS`** |
| `V-3` | command negative | A 发 `{"cmd":...}` | `th+0` · `mm(A)+1` | ✅ **`PASS`** |
| `V-4` | mixed payload | A 发含 `cmd` 的 telemetry 外形 | 判**非 telemetry** ⇒ `th+0` · `mm(A)+1` | ✅ **`PASS`** |
| `V-5` | multi device isolation | B（`eeeeeeeeeeee`）发 telemetry | `th(B)+1` · A 的行零增长（身份取自 topic 第 2 段） | ✅ **`PASS`** |
| `V-6` | reconnect | 同 tuple 重连前后各发一次 | `th(A)` **恰 1 行**（重连不重复写入） | ✅ **`PASS`** |
| `V-7` | duplicate | 同 tuple 同连接发两次 | `th(A)` **恰 1 行**（`ux_th_dedup` 吸收） | ✅ **`PASS`** |

> **`V-5` 首跑插曲**：probe8 误用 11 位十六进制设备 ID `ffffffffffe` ⇒ Worker `invalid_device_id` 400（身份格式校验生效的反向实证）⇒ 换合法 12 位 `eeeeeeeeeeee` 重跑即 PASS。**测试探针缺陷，非链路缺陷。**

> **⇒ `CF-2`（判据 = `V-1…V-7` 通过）= ✅ `CLOSED`。** 连同 §7.5.6 的端点健康实证，本工作包控制台侧目标全部达成。
>
> **卫生**：本轮全部测试行已清除，D1 基线恢复（`th=0` · `lh=0` · `mm=2458` · `device_event=0` · `last_seen=1791811429`）。

---

## §14 【追加任务】补回 presence 链路（**连接器 ×1 + 规则 ×2 + 动作 ×2**）

> **背景**：原计划 presence 4 对象「不动」，实际旧 presence 连接器 `c-n302933b-291200` 已被一并删除 ⇒ **presence 全链路消失**。
> **目标**：按 `V-0` 基线（`P3-WP4C-V0-EMQX-Baseline.md` §3.1–§3.3）**逐字重建等价物**（新 ID）。
> **配额核算（建成后）**：连接器 **2/2** · 规则 **4/4** · 动作 **4/4** —— **刚好用满；此后任何改动都必须先删后建**。
> **建议顺序**：先做 §7.5.3 的路径对调修复（避免和 presence 混在一起排查），再做本节。

> ### 🔄 施工进度（`2026-10-12` 22:20 · 第五轮终测后 = **全部完成**）
>
> | 对象 | 状态 |
> |---|---|
> | 连接器（`guo_feeder_presence_http`） | ✅ **已重建**（旧 `c-n302933b-70f9da` 无法修改请求头 ⇒ 人工删旧建新）—— **secret 值已填**；§7.5.7 tail 实证连接/断开事件均 **200** · **新 ID 待你回传** |
> | 规则 `guo_feeder_presence_disconnected` | ✅ **已建 · SQL 正确勿动**（括号平衡 8/8）—— §7.5.7 实证断开事件 168 B 正常渲染（含 `reason`） |
> | disconnected 规则的动作 | ✅ **已建 · body = 裸 `${body}` 已生效**（同上实证） |
> | 规则 `guo_feeder_presence_connected` | ✅ **已建**（§14.2 修正版 SQL）—— §7.5.7 实证连接事件 144 B 正常触发并渲染 |
> | connected 规则的动作 | ✅ **已建 · body 已填**（同上实证） |
>
> **★ presence 链路 = 全链恢复**（§7.5.7：`device_event` online/offline 各 1 行 · `last_seen` 刷新 · 重连 4 事件全部落库）。**剩余事项 = 人工回传新连接器/动作 ID**（回写 `V2V4-Config-After` 存档用）。

### 14.1 新建连接器（presence）

| 控制台字段 | **填这个** | 依据 |
|---|---|---|
| 连接器类型 | HTTP 服务 | 同旧 |
| 连接器名称 / 备注 | `guo_feeder_presence_http` | 同旧备注 |
| **URL** | **`https://guo-feeder-pagesdev.pages.dev/api/presence/event`** | ★ **全路径**（presence 动作的 URL 路径**留空**，与旧链路逐字一致；**勿**学 `CB` 用裸主机） |
| 请求头 | `X-Webhook-Key` = `<WEBHOOK_SECRET>` · `content-type` = `application/json` | 同旧（与 `CB` 的 4 头不同，**勿混**） |
| 连接超时 | **`5s`** | 同旧（`CB` 是 `10s`） |
| HTTP 管道 / 连接池大小 / 连接池类型 / 健康检查间隔 / 启动超时 | `50` / `1` / `random` / `30s` / `5s` | 同旧 |
| 启用 TLS | **开** | 同旧 |
| **SNI** | **`guo-feeder-pagesdev.pages.dev`** | 必填（不填握手失败） |
| 验证服务器证书 / CA 证书 / 客户端证书 / 私钥 | **关 / 空 / 空 / 空** | 同旧 |
| OAuth2 客户端凭证 | 不开 | 用 `X-Webhook-Key` |

★ **保存前截图 TLS 段**（`R-WP4C-16` guard ①）· 保存后点「测试」期望 **200**；出现 `TLS / certificate / ssl` 字样先查 SNI。

### 14.2 新建规则 ×2（Serverless：**先建规则、后配动作**）

> 🔴 **括号修正（`2026-10-12` 21:20 · 你实测 parse 错误后）**：**只有 connected（P-1）的 SQL 多了一个右括号**（7 开 8 闭）⇒ `1: Missing FROM or WHERE`（`V-0` 存档 §3.1 的 `R-1` 同错，为转录误差）。
> **P-1 已改为平衡版（7 开 7 闭）**；**P-2（disconnected）括号本就平衡（8 开 8 闭，多一个 `reason` 字段 ⇒ 多一个 `map_put`）—— 你截图里那条是对的，不要动。**
> **按 `R-WP4C-20` 纪律：保存前必须用规则页调试功能试跑。**

**规则 P-1**（备注 `guo_feeder_presence_connected` · 启用 ✅）：

```sql
SELECT
  json_encode(
    map_put('connected_at', connected_at,
    map_put('peername', peername,
    map_put('username', username,
    map_put('clientid', clientid,
    map_put('event', 'client_connected',
      map_new()
    )))))
  ) AS body
FROM
  "$events/client_connected"
```

**规则 P-2**（备注 `guo_feeder_presence_disconnected` · 启用 ✅）：

```sql
SELECT
  json_encode(
    map_put('disconnected_at', disconnected_at,
    map_put('reason', reason,
    map_put('peername', peername,
    map_put('username', username,
    map_put('clientid', clientid,
    map_put('event', 'client_disconnected',
      map_new()
    ))))))
  ) AS body
FROM
  "$events/client_disconnected"
```

> 两段 SQL **逐字取自 `V-0` 基线 §3.1（`R-1` / `R-2`）** · **无 WHERE** · 与已删旧规则逐字一致（存档已核验）。

### 14.3 每条规则各配 1 个动作（共 2 个）

| 控制台字段 | **填这个** |
|---|---|
| 动作类型 | 数据转发 - HTTP 服务 |
| 使用连接器 | **§14.1 新建的 presence 连接器** |
| 请求方法 | `POST` |
| **URL 路径** | **（空）** ← 端点路径已写死在连接器 URL 里 |
| **请求体** | **裸 `${body}`**（★ 与消息流 / history 的三键模板**完全不同，切勿混用**） |
| 动作层请求头（5 条） | `accept` = `application/json` · `cache-control` = `no-cache` · `connection` = `keep-alive` · `content-type` = `application/json` · `keep-alive` = `timeout=5` |
| 高级设置（7 项） | 重试 `1` · 缓存池 `1` · 超时 `5s` · 健康检查 `30s` · 缓存队列 `4MB` · **异步** · 飞行窗口 `50` |

> ⚠️ **`2026-10-12` 21:20 实测提醒**：你已建的动作 `a-n302933b-6981cc` 的**请求体在截图里似为空白** —— **必须是裸 `${body}`**（规则 SQL 已用 `json_encode` 产出完整 JSON；空 body ⇒ POST 空体 ⇒ presence 端点收到空数据）。
> 另：**高级设置 7 项**保存前逐项核对（截图里「高级设置」是折叠的，无法确认）。

### 14.4 做完验证 + 回写

- 连接器列表：presence 连接器状态**「已连接」**；
- 用**任意 MQTT 客户端连接/断开一次**（MQTTX，任意账号均可 —— `$events/client_*` 与身份无关）⇒ 规则 P-1 / P-2 **触发数各 +1**；
- 把 4 个新 ID 回写到 `EMQX_Assets/config/data-integration/P3-WP4C-V2V4-Config-After.md`（presence 存档节）。

---

## §15 是否还需要新建其他规则？—— **不需要**（presence 那 2 条是「恢复」不是「新建」）

| # | 判断 | 依据 |
|---|---|---|
| 1 | **history ingest 不需要更多规则** | 规则 A（`/+/up` + 冻结谓词）与规则 B（`/#` + 谓词之否定）**互斥且完整覆盖** `guo_feeder/#` 全族 ⇒ 无漏投、无双投（本轮探针 T1–T5 实证） |
| 2 | **presence 2 条 = 恢复旧物** | 见 §14；建成后规则 4/4 用满 |
| 3 | **不要为 `/log` 加规则 / 改 FROM** | `/log`（CBOR 二进制）修复后仍会经规则 B → `/api/webhook` → `Invalid JSON 400`（T8 实锤 · `R-WP4C-15`）。**修它靠固件侧 Cloud Log Envelope（`CF-1`，固件工作包，未开工）**，不靠新规则；且改 `FROM` 违反冻结设计（`SELECT *` + FROM 不动） |
| 4 | **未来固件 telemetry publisher 落地后** | 规则 A **自动开始接收**，零规则改动（`CF-2` 的修复在固件侧） |
| 5 | **未来固件 log 信封（JSON）落地后** | 信封字段（`device_id`/`boot_seq`/`seq`/`ts`/`level`/`payload_base64`）缺 `schema_version`/`type`/`value` ⇒ 会被规则 B 送去 `/api/webhook`（错端点）⇒ **届时需新增一条 log 域规则**（属未来工作包，**现在不建**） |

---

*本文档于 **`2026-10-12`** 建立，为 **`R3` 控制台施工任务单（照抄版）**；**同日升级 `R3.1`**（SQL 谓词修正：`==` 非法 ⇒ 改用官方 `is_null` / `is_not_null`），对应裁决 **`H-WP4C-09-B = 🟡 READY (R3.1)`**。
**旧对象原文逐字取自你提供的控制台实测**（转录于 `P3-WP4C-V0-EMQX-Baseline.md`），**新对象规格取自冻结设计 `R3`**，**SQL 谓词取自 `R3.1`**（依据 EMQX 官方 Rule SQL / Built-in Functions 文档）。
基线 = 固件仓 **`1fc9f5a`** · CF 仓 **`415cd0c`** · EMQX 仓 **`1178611`**；
**已建对象**：连接器 `c-n302933b-0c1cb0`（`2026-10-12`）。*
