# P3 · `WP4C` —— **EMQX 控制台施工任务单（`R3` · 照抄版）**

> ## 📋 一句话总览
>
> **删 3 个**（1 规则 + 1 动作 + 1 连接器）→ **建 4 个**（1 连接器 + 2 动作 + 2 规则）。
> **只影响"消息流"这一条链路**（本就 ≈90% 失败、best-effort）；**presence 的 4 个对象完全不动**。
> **预计 15 分钟**；**配额必须刚好吻合**（连接器 2/2 · 规则 4/4 · 动作 4/4）。
>
> | 项 | 值 |
> |---|---|
> | **执行人** | 你（EMQX 控制台人工）。代理**无**控制台 / 数据集成 API 权限（API 全 `403` · 无 CLI） |
> | **依据** | 冻结设计 `R3`（`docs/review/P3-WP4C-EMQX-Rule-Design.md` §10）+ 施工规范（`docs/review/P3-WP4C-Console-Build-Guide-R3.md`） |
> | **不要忘** | ① **删之前先抄旧运行统计**（§2）② **建连接器保存前先截图 TLS 段**（§4）③ **`${payload}` 不加引号**（§5/§6） |
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
新连接器 CB 的 ID = c-______________________
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

**SQL（整段复制粘贴）**：

```sql
SELECT
  *
FROM
  "guo_feeder/+/up"
WHERE
      payload.schema_version != 'undefined'
  AND payload.seq            != 'undefined'
  AND payload.ts             != 'undefined'
  AND payload.type           != 'undefined'
  AND payload.value          != 'undefined'
  AND payload.c              == 'undefined'
  AND payload.cmd            == 'undefined'
```

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
| **WHERE** | **`NOT`（上面那段谓词）** ← ★ 一步到位，这样**不会双投** |
| 关联动作 | **`MF`** |
| 启用 | ✅ **是**（建完这一步，**中断窗口结束**） |

**SQL（首选写法 · 整段复制粘贴）**：

```sql
SELECT
  *
FROM
  "guo_feeder/#"
WHERE
  NOT (
        payload.schema_version != 'undefined'
    AND payload.seq            != 'undefined'
    AND payload.ts             != 'undefined'
    AND payload.type           != 'undefined'
    AND payload.value          != 'undefined'
    AND payload.c              == 'undefined'
    AND payload.cmd            == 'undefined'
  )
```

**SQL（备用写法 · 万上上面那段报语法错，用这段"德摩根展开"版，含义完全等价）**：

```sql
SELECT
  *
FROM
  "guo_feeder/#"
WHERE
      payload.schema_version == 'undefined'
   OR payload.seq            == 'undefined'
   OR payload.ts             == 'undefined'
   OR payload.type           == 'undefined'
   OR payload.value          == 'undefined'
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

> ⚠️ 若两条规则都保存失败（提示语法不支持）⇒ **停手**，把控制台的报错原文发我（不同版本 SQL 子集不同，可能需要改用 `json_decode(payload)` 写法）。
> 若规则 A 成功、规则 B 只在 `NOT` 上报错 ⇒ 直接用上面的**德摩根备用版**（**记得告诉我实际用了哪一版**）。

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

规则 B 实际采用：  [ ] NOT 写法   [ ] 德摩根备用写法
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
| 规则保存报语法错 | 先换 §7.2 的**德摩根备用写法**；两版都失败 ⇒ **停手**，把报错原文发我 |
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

*本文档于 **`2026-10-12`** 建立，为 **`R3` 控制台施工任务单（照抄版）**；对应裁决 **`H-WP4C-09-B = 🟡 READY (R3)`**。
**旧对象原文逐字取自你提供的控制台实测**（转录于 `P3-WP4C-V0-EMQX-Baseline.md`），**新对象规格取自冻结设计 `R3`**。
基线 = 固件仓 **`1fc9f5a`** · CF 仓 **`415cd0c`** · EMQX 仓 **`1178611`**。*
