# P3 · `WP4C` —— **EMQX 控制台施工说明（`R3` + `R3.1` SQL 修正 · 删除重建版）**

> ## 🛑 本文件 = **人工控制台施工手册（`R3` / `R3.1`）** —— **代理不能执行**（无控制台 / 无数据集成 API 权限）
>
> | 项 | 值 |
> |---|---|
> | **本文档性质** | **施工说明 + `R3`/`R3.1` 修订记录** —— 回答三个问题：**如何建连接器 · 如何建动作 · 如何建规则** |
> | **执行方** | **人工**（EMQX Console，`H-WP4C-09-B`） |
> | **修订成因** | ① **`R3`**：**连接器创建后不可编辑**（官方文档：编辑 / 启停连接器**仅限专有版**）⇒ `R2` 的"就地改写 URL"**不可行** ⇒ 改为 **删除重建**。② **`R3.1`（`2026-10-12`）**：`R3` 的 SQL 谓词**语法错误**（`==` 不被支持 · 应用官方 `is_null` / `is_not_null`）⇒ **仅改谓词写法**，其余全部不变 |
> | **不变的部分（核心语义逐字保留）** | ✅ 方案 A（EMQX Rule payload predicate routing）· ✅ 冻结谓词语义 `( schema_version ∧ seq ∧ ts ∧ type ∧ value ) ∧ ¬c ∧ ¬cmd` · ✅ `Action Target = Pages Proxy`（不碰 `*.workers.dev`）· ✅ 端点 `/api/history/ingest` · ✅ 验证集 `V-1…V-7` · ✅ 不改 firmware / 不改 topic / Worker 不解析 |
> | **变更的部分** | ① **`R3`**：**「编辑连接器」→「删除旧连接器 + 新建」**；⇒ **`-S2` 双投窗口彻底消失**（新规则直接建成已收窄态）。② **`R3.1`**：**谓词写法** = `payload.x != 'undefined'` → **`is_not_null(payload.x)`** / **`is_null(payload.x)`**；**`==` → 禁用**（用 `is_null` 或单 `=`） |
> | **本文件不做的** | ❌ 未创建 / 修改任何 EMQX Rule · Action · Connector · ❌ 未部署 · ❌ 未改 firmware / Worker / migration · ❌ 不 push |
> | **基线** | 固件仓 **`4c74d3a`** · CF 仓 **`415cd0c`** · EMQX 仓 **`a73968a`**；**已建对象**：连接器 **`c-n302933b-0c1cb0`**（`2026-10-12`） |
>
> > ### 📋 **要"照着点"的话，请用 → `docs/review/P3-WP4C-Console-Task-Sheet.md`（施工任务单 · 照抄版）**
> > 本文件是**施工规范与依据**（平台事实 / 设计裁量 / 窗口量化 / 回滚 / 风险）；
> **任务单**把"旧对象原文 + 删除清单 + 新建逐字段值 + 完整 SQL"集中到一份，**打开即可照着操作**。两者内容**一致**（任务单为本文件的照抄化改写）。

---

## §1 为什么必须改成「删除重建」（**平台事实 · 逐条有据**）

| # | 平台事实 | 出处 | 对施工的影响 |
|---|---|---|---|
| **1** | **连接器创建后不可编辑** —— 官方原文：**「编辑连接器 注意 该功能仅适用于专有版部署」** | EMQX Cloud 文档《连接器》 | 🔴 **`R2` 的「改 `c-…b12100` 的 URL」不可执行** ⇒ 只能**删除 + 新建** |
| **2** | **连接器不可启用 / 停用** —— 官方原文：**「启用和禁用连接器 注意 该功能仅适用于专有版部署」** | 同上 | ⇒ 连接器**没有"先停后改"的退路**；一旦建成就固定 |
| **3** | **删除连接器有前置** —— 官方原文：**「确保在删除连接器之前已清空该连接器下的所有规则」**；**「必须输入连接器的 ID 来确认删除」** | 同上 | ⇒ **拆除顺序必须是 规则 → 动作 → 连接器**（依赖倒序，见 §4） |
| **4** | **规则可编辑**（无版本限制）—— 官方原文：「您可以直接点击规则列表上的编辑图标来编辑该规则……**在设置页面，你可以编辑规则的 SQL 模板，同时可以编辑、添加动作**」 | EMQX Cloud 文档《规则》 | ✅ ⇒ **收窄旧规则 WHERE**（`V-4`）仍可"就地编辑"；`R3` 只是把它并入重建 |
| **5** | **动作可编辑、可删除**（无版本限制）—— 官方原文：「可以在动作列表点击**动作名称**或编辑按钮，重新编辑动作。点击**删除**按钮删除动作」 | 同上 | ✅ ⇒ 动作**不必**跟着连接器一起删（但见 §4.1 顺序说明） |
| **6** | **动作有独立的「URL 路径」** —— 官方原文：「**URL 路径**：可以设置动作单独的请求路径，**此路径将附加到连接器的 URL 配置中，以形成完整的 URL 地址**」 | EMQX Cloud 文档《将 MQTT 数据发送到 HTTP 服务》 | ✅ ⇒ **单连接器 + 动作级路径**机制成立（`R2` 的核心机制不变） |
| **7** | **动作可单独设请求头** —— 官方原文：「**请求头**：对这个动作可以单独设置请求头，也可以直接使用连接器配置的请求头」 | 同上 | ⇒ `X-Webhook-Key` **两种落位都可行**（`R3` 选动作级，见 §6.2） |
| **8** | **Serverless 配额：连接器 ≤2 · 规则 ≤4 · 单规则动作 =1**（⇒ 动作实际上限 4） | EMQX Cloud 文档《数据集成简介》 + 人工控制台实测（**不可调整**） | 🔴 连接器 **2/2 已满** ⇒ 必须先删才能建（`D-7`） |

> **⇒ 结论**：`R2` 的**分流语义**（单连接器 + 动作级路径 + 冻结谓词）**完全保留**；
> 只是**实现手段**从"改一个"变成"删一个、建一个"。**这不是退让，反而更干净**（见 §2.3）。

---

## §2 变更总览

### 2.1 对象清单（**谁动、谁不动**）

| 对象 | ID | 处置 | 说明 |
|---|---|---|---|
| 连接器（presence） | `c-n302933b-291200` | ✅ **完全不动** | URL `…/api/presence/event`；已带 `X-Webhook-Key` |
| 连接器（消息流） | `c-n302933b-b12100` | 🔴 **删除** → 由 **`CB`（新）** 取代 | URL 含路径 ⇒ 无法承载第二个端点（不可编辑） |
| 规则（presence-connect） | `r-n302933b-3990d0` | ✅ **完全不动** | |
| 规则（presence-disconnect） | `r-n302933b-18a660` | ✅ **完全不动** | |
| 规则（消息流 `guo_feeder/#`） | `r-n302933b-a01720` | 🔴 **删除** → 由 **`guo_feeder_message_ingest`（新）** 取代 | `FROM` / `SELECT *` 逐字不变；**新规则直接带 `WHERE = NOT 谓词`** |
| 动作（presence ×2） | `a-…10fade` / `a-…17ee9a` | ✅ **完全不动** | 请求体 = 裸 `${body}` |
| 动作（消息流） | `a-n302933b-921e22` | 🔴 **删除** → 由 **`MF`（新）** 取代 | 请求体三键模板**逐字不变**；**新增 `URL 路径 = /api/webhook`** |
| 动作（history） | — | 🟢 **新建 `HI`** | `URL 路径 = /api/history/ingest` |

**依赖审计（拆除安全性）**：

```
c-n302933b-b12100  ←被引用 by←  a-n302933b-921e22  ←被引用 by←  r-n302933b-a01720
        ↑ 仅此一条引用链            ↑ 仅此一条引用链            ↑ 无上游
⇒ 删除这条链【不触及任何其他对象】；presence 两连接器/两动作/两规则【零接触】。
```

### 2.2 配额账（**变更后**）

| 配额 | 上限 | 变更前 | **变更后** | 判定 |
|---|---|---|---|---|
| 连接器 | **2** | 2 | **2** = presence + `CB` | ✅ 用满 |
| 规则 | **4** | 3 | **4** = 2×presence + MF + HI | ✅ 用满 |
| 动作 | **4**（= 4 规则 × 1） | 3 | **4** = 2×presence + MF + HI | ✅ 用满 |
| 单规则动作 | **1** | 1 | **1** | ✅ 天然满足 |

> ⚠️ **后果（已登记 `D-7`）**：变更后三项配额**全部用满** ⇒
> **后续任何新的数据集成需求只能"替换"，不能"新增"**。

### 2.3 ★ `R3` 相对 `R2` 的**收益**（不只是"没办法"）

| # | 收益 | 依据 |
|---|---|---|
| 1 | **`-S2` 双投窗口彻底消失** | `R2` 是"先启用新规则 → 再收窄旧规则"（存在 ≤30 s 双投）；`R3` 中旧规则被**删除后重建**，**新规则可直接写成已收窄态** ⇒ 不存在"两条规则同时吃 telemetry"的时刻 |
| 2 | **连接器内容回归"已知良好"** | `CB` 是**全新创建**的，URL / 头 / TLS 全由本次施工指定并留痕 ⇒ 不再依赖"保存后不可回读"的历史值（`R-WP4C-16` 的不可审计区缩小到仅 presence 连接器） |
| 3 | **消息流链路配置显式化** | 端点从"连接器 URL 里隐含"改为"动作 `URL 路径` 显式声明" ⇒ 与冻结设计的"端点即路径"语义一致 |

### 2.4 与 `R2` 的差异对照

| 项 | `R2`（作废） | **`R3`（本文件）** |
|---|---|---|
| 连接器 | **编辑** `c-…b12100`（URL → base、加头） | **删除** `c-…b12100` + **新建** `CB`（URL = base、头 = 旧 4 头） |
| 消息流动作 | **编辑** `a-…921e22`（补 `URL 路径`） | **删除** + **新建** `MF`（`URL 路径 = /api/webhook`） |
| 消息流规则 | **编辑** `r-…a01720`（补 `WHERE`） | **删除** + **新建**（`WHERE = NOT 谓词`，**一步到位**） |
| `X-Webhook-Key` 落位 | 连接器层 | **`HI` 动作层**（消息流链路**不带**该头 ⇒ 与今日行为逐字一致） |
| 中断窗口 | 两次保存之间（≤60 s） | **拆除起点 → 重建完成**（目标 ≤15 min） |
| 双投窗口 | ≤30 s（`-S2`） | **无** |

---

## §3 施工前采集（**只读 · 必做** · 全部做完再动手）

| # | 采集项 | 怎么取 | 为什么 |
|---|---|---|---|
| **0-1** | **★ 抄录 `r-…a01720` / `a-…921e22` 的运行统计**（触发次数 / 成功 / 失败） | 规则列表 → 点规则 ID → 运行统计 | **删除会永久丢失这些指标** —— 它正是 `R-WP4C-15`（≈90% 失败）的唯一量化证据 |
| **0-2** | **★ 截图 `c-…b12100` 的连接器详情页**（URL / 请求头 / 「已启用 TLS」） | 连接器列表 → 点连接器 | 拆除前留痕；并作为 `CB` 的**逐字模板** |
| **0-3** | **截图 `a-…921e22` 的动作配置**（请求方法 / 请求体 / 高级设置全展开） | 动作列表 → 点动作名 | `MF` 要**逐字照抄** |
| **0-4** | **抄录 `r-…a01720` 的 SQL 原文** | 规则 → 设置页 | 已知为 `SELECT * FROM "guo_feeder/#"`（`V-0` §3.1 R-3）—— 再核一次 |
| **0-5** | **采 D1 计数基线**（只读） | 见 §9.1 命令 | 变更前后差分判据 |
| **0-6** | （可选）截图 presence 连接器详情页作 TLS 参考 | 连接器列表 | 仅供比对，**不要动它** |

> **纪律**：**只看不存** —— 打开配置页抄完直接关闭，**绝不点「保存」**（`P3-WP4C-Implementation-Discipline.md` §6A）。
> 截图归档 → `EMQX_Assets/config/data-integration/assets/wp4c-v2v4/`（**密钥先打码**）。

---

## §4 Step 1 —— **拆除旧消息流链路**（🛑 **中断窗口从这里开始**）

### 4.1 删除顺序（**必须按此序 · 依赖倒序**）

| 序 | 对象 | 操作路径 | 确认方式 |
|---|---|---|---|
| **1** | 规则 `r-n302933b-a01720` | **数据集成 → 规则列表** → 该行「删除」 | **输入规则 ID** 确认 |
| **2** | 动作 `a-n302933b-921e22` | **动作列表**（或规则设置页内的动作）→「删除」 | 弹窗确认 |
| **3** | 连接器 `c-n302933b-b12100` | **数据集成 → 连接器列表** → 该行「删除」 | **输入连接器 ID** 确认 |

> **为什么是"规则 → 动作 → 连接器"**：官方文档明示**删除连接器前必须先清空其下所有规则**。
> 若第 3 步被拦（提示仍有引用）⇒ 回到第 1/2 步确认该连接器已**零引用**再删。

### 4.2 窗口与影响面（**已量化**）

| 项 | 内容 |
|---|---|
| **窗口起** | 第 1 步「删除规则」确认的瞬间 |
| **窗口止** | §7 的**两条新规则均启用**（消息流重新落库） |
| **目标时长** | **≤15 min**（人工操作；采集工作已在 §3 全部做完 ⇒ 只需连点） |
| **受影响** | **仅**消息流链路（`guo_feeder/#` → `/api/webhook` → `mqtt_messages`） |
| **不受影响** | ✅ presence 链路（另一连接器，**零接触**）· ✅ D1 / Worker / Pages · ✅ 设备侧（MQTT 连接与上下行**不经过**本规则） |
| **影响量化** | 生产设备实测 **≈4–70 条/天**（≈0.003–0.05 条/min）⇒ **期望丢失 0–1 条**；且该链路本就 **≈13% 成功率**、已判定 **best-effort**（`R-WP4C-15`，本工作包不修） |
| **回滚** | 见 §10 |

> **★ 想压到零中断怎么办**：唯一办法是不删连接器 —— 代价是让新动作拼出 `…/api/webhook/api/history/ingest` 这类怪路径（`R2` 的备选 `R-E`），**需改 Pages 与端点契约（违反冻结设计）** ⇒ **不采用**。

---

## §5 Step 2 —— **新建连接器 `CB`**（配额槽位已由 §4 释放）

### 5.1 操作路径

**数据集成 → 连接器列表 → 右上角「新建连接器」→ Web 服务分类下选「HTTP 服务」→ 填表 → 点「测试」→ 点「新建」**

### 5.2 逐字段填写（**照此填，勿自由发挥**）

| 控制台字段 | **填写值** | 说明 |
|---|---|---|
| **连接器名称** | `guo_feeder_shared_http` | 控制台若强制自动命名（`c-<tenant>-<hash>`）⇒ 把此名填入**备注** |
| 备注 | `GuoFeeder shared base (message + history)` | 便于日后辨认"这是共用 base 连接器" |
| **URL** | **`https://guo-feeder-pagesdev.pages.dev`** | **★ 裸主机：无路径、无尾斜杠** —— 见 §5.3 |
| **请求头** | 照抄旧连接器 4 条：<br>`accept: application/json`<br>`cache-control: no-cache`<br>`connection: keep-alive`<br>`content-type: application/json` | **★ 此处不填 `X-Webhook-Key`**（放到 `HI` 动作层，见 §6.2） |
| **启用 TLS** | **开** | 目标为 `https://` |
| **SNI** | **`guo-feeder-pagesdev.pages.dev`** | **★ 必填** —— 不填 ⇒ 握手直接失败（`ssl/tls alert handshake failure`） |
| **验证服务器证书** | **关**（`verify_none`） | `verify_peer` 强制要求 CA 文件，且 Cloudflare 会轮换签发 CA ⇒ 关 |
| **CA 证书 / TLS 证书 / 密钥** | **留空** | |
| 连接超时 | `10s` | 照抄旧消息流连接器 |
| HTTP 管道 | `50` | 同上（默认） |
| 连接池大小 | `1` | 同上 |
| 连接池类型 | `random` | 同上 |
| 健康检查间隔 | `30s` | 同上 |
| 启动超时时间 | `5s` | 同上 |
| OAuth2 客户端凭证 | **不开** | 本场景用 `X-Webhook-Key` |

### 5.3 ★ 为什么 URL 必须取「裸主机」

官方文档明确：**动作的 `URL 路径` 会「附加到连接器的 URL 配置中」**。

```
CB.URL = https://guo-feeder-pagesdev.pages.dev        ← 裸主机（无路径）
   + MF.Path = /api/webhook            ⇒ https://…pages.dev/api/webhook          ✅ 与今日逐字相同
   + HI.Path = /api/history/ingest     ⇒ https://…pages.dev/api/history/ingest   ✅ 冻结端点
```

⇒ 裸主机是**唯一**对"追加"与"替换路径"两种拼接语义都成立的选择：**消除唯一的不确定性**。

### 5.4 ★ 双 guard（**强制**）

| # | Guard | 判据 |
|---|---|---|
| **① 保存前截图 TLS 段** | `R-WP4C-16`：**TLS 明细保存后永久不可回读** ⇒ **保存前必须截图**（SNI / 验证服务器证书 / CA / 启用 TLS 四项同框） | 截图归档 `assets/wp4c-v2v4/` |
| **② 保存后必须实测** | 连接器列表状态 = **「已连接」**；且**必须**用「测试」或动作实测打到 **HTTP 层** | 见到 `200` / `401` / `400` / `405` 皆算**通**；见 `TLS` / `certificate` / `ssl` 字样 ⇒ **先查 SNI** |

> **「测试连接」的期望**：目标是**根路径 `GET /`** ⇒ 命中 Pages 静态站 ⇒ 期望 **`200`**（返回 `index.html`）。

### 5.5 登记新 ID

创建成功后**立刻**把新连接器 ID（`c-<tenant>-<hash>`）填入 **§11 登记表** —— 回滚与回写都要用它。

---

## §6 Step 3 —— **新建两个动作**

### 6.1 操作路径

**数据集成 → 动作列表 → 「新建动作」**（或建规则时在「下一步」页内创建）
→ 从 **「使用连接器」下拉框**选 **`CB`**（§5 新建的那个）
→ 填下表 → **确认**。

> ⚠️ **顺序**：动作必须先于规则存在（规则创建时"下一步"要选连接器与动作）。
> ⚠️ **两个动作都选同一个 `CB`** —— 这正是"单连接器 + 动作级路径分流"。

### 6.2 动作 A：`MF`（消息流 · 取代被删的 `a-…921e22`）

| 控制台字段 | **填写值** |
|---|---|
| **动作名称** | `guo_feeder_message_ingest`（若自动命名 ⇒ 备注填此名） |
| **使用连接器** | **`CB`** |
| **请求方法** | **`POST`** |
| **URL 路径** | **`/api/webhook`** ← ★ **本次新增的显式声明**（旧动作此字段为空，端点隐含在连接器 URL 里） |
| **请求头** | **留空**（继承 `CB` 的 4 条 ⇒ 与今日行为**逐字一致**） |
| **请求体** | `{"topic":"${topic}","clientid":"${clientid}","payload": ${payload}}` ← ★ **`${payload}` 前后不要加引号** |
| 高级设置：最大重试次数 | `1` |
| 高级设置：缓存池大小 | `1` |
| 高级设置：请求超时 | `5s` |
| 高级设置：健康检查间隔 | `30s` |
| 高级设置：缓存队列最大长度 | `4MB` |
| 高级设置：请求模式 | **`异步`** |
| 高级设置：请求飞行队列窗口 | `50` |

> **★ 生效 URL 校验**：`CB.URL` + `/api/webhook` = `https://guo-feeder-pagesdev.pages.dev/api/webhook` —— **与拆除前逐字相同**。
> **★ 请求体为什么"不加引号"**：`payload` 必须是**对象/数组**才能被 Worker 解析；加引号 ⇒ 变字符串 ⇒ `400`（`R2` §6.2 已定案）。
> **★ 高级设置为什么照抄**：`R2` §4.5 已定 —— **不修 `R-WP4C-15`**，保持与现状同构（超出 WP-4 范围）。

### 6.3 动作 B：`HI`（history · 全新）

| 控制台字段 | **填写值** |
|---|---|
| **动作名称** | `guo_feeder_history_ingest`（若自动命名 ⇒ 备注填此名） |
| **使用连接器** | **`CB`** |
| **请求方法** | **`POST`** |
| **URL 路径** | **`/api/history/ingest`** ⇒ 生效 URL = `https://guo-feeder-pagesdev.pages.dev/api/history/ingest` |
| **请求头** | ★ **两行**：<br>`content-type: application/json`<br>`X-Webhook-Key: <WEBHOOK_SECRET>` |
| **请求体** | `{"topic":"${topic}","clientid":"${clientid}","payload": ${payload}}` ← 与 `MF` **同一形态**（`${payload}` **不加引号**） |
| 高级设置 | **与 `MF` 逐字相同**（重试 `1` · 缓存池 `1` · 超时 `5s` · 健康检查 `30s` · 缓存队列 `4MB` · 请求模式 **异步** · 飞行窗口 `50`） |

> **★ 鉴权头为什么放这里（而不是连接器）**：官方文档确认**动作可单独设请求头**。
> 放动作层 ⇒ **消息流链路完全不带 `X-Webhook-Key`**，与今日行为**逐字一致**（零连带影响）。
> **`content-type` 同时写在动作层**：以防平台对动作头采用"替换"而非"合并"语义（两种语义下 `HI` 都拿到所需头）。
> ⚠️ **密钥纪律**：`<WEBHOOK_SECRET>` **只填进控制台，永不写入仓库 / 文档 / 记忆**（本文件用占位符）。
> **若动作页不提供请求头字段**（与文档不符）⇒ **回退**：把 `X-Webhook-Key` 加到 **`CB` 连接器**的请求头（副作用：消息流也会带上该头，对无鉴权的 `/api/webhook` **无行为变化**）。

---

## §7 Step 4 —— **新建两条规则**

### 7.1 操作路径

**数据集成 → 规则列表 → 「新建规则」** → 在 **SQL 编辑器**粘贴 SQL（可用 **「启用调试」** 先试跑）
→ 点 **「下一步」** → **「使用连接器」下拉选 `CB`** → 选/填对应动作 → **确认** → 返回规则列表 → 打开 **「是否启用」** 开关。

> ⚠️ **两条规则都建、都启用之后**，§4.2 的**中断窗口才结束**（消息流恢复落库）。

### 7.2 规则 A（**先建**）：`guo_feeder_history_ingest`

| 项 | 值 |
|---|---|
| **名称 / 备注** | `guo_feeder_history_ingest` |
| **FROM** | `guo_feeder/+/up` |
| **WHERE** | 冻结谓词（见 §7.4） |
| **SELECT** | **`*`** |
| **动作** | **`HI`**（§6.3） |
| **启用** | ✅ **是** |

> ### ⚠️ `R3.1` SQL 修正（`2026-10-12` · **BINDING**）
> `R3` 的 SQL 谓词有 **2 处错误**，已在 `R3.1` 修正：
> | # | 错 | 事实（EMQX 官方） | 修正 |
> |---|---|---|---|
> | 1 | `payload.c == 'undefined'` | 规则 SQL 的"相等"**只有单个 `=`**；**`==` 不存在** ⇒ `syntax error before: '='`（控制台实测报错） | 改用 **`is_null(payload.c)`** |
> | 2 | 用 `!= 'undefined'` 判断"键存在" | 官方内置函数 **`is_null(Term)`** = 判 **undefined（未赋值）**；**`is_not_null(Term)`** = 反之。中间字段缺失也返回 `true` | 全部改用 `is_null` / `is_not_null` |
>
> 来源：EMQX 官方 *Rule SQL Reference*（`=` 说明）与 *Built-in SQL Functions*（`is_null` / `is_not_null` 说明）。

**SQL（首选写法 · 控制台粘贴用）**：

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

**SQL（最后备选 · 仅当 `is_null` 报"未知函数"才用 · 注意 `=` 单等号）**：

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
  AND payload.c              = 'undefined'
  AND payload.cmd            = 'undefined'
```

> **★ `SELECT *` 是必须的**：动作体引用 `${topic}` / `${clientid}` / `${payload}`，
> 而 `SELECT json_encode(…) AS body` 式**只输出 `body` 一个字段** ⇒ 变量取空 ⇒ **`V-2` 必失败**（`R2` §6.1 已定案）。

### 7.3 规则 B（**后建**）：`guo_feeder_message_ingest`

| 项 | 值 |
|---|---|
| **名称 / 备注** | `guo_feeder_message_ingest` |
| **FROM** | `guo_feeder/#` ← **与被删规则逐字相同** |
| **WHERE** | **把规则 A 的条件整体取反**（= "非 telemetry"）← ★ **一步到位**（`R3` 的核心收益：**无双投窗口**） |
| **SELECT** | **`*`** ← 与原文逐字相同 |
| **动作** | **`MF`**（§6.2） |
| **启用** | ✅ **是** |

**SQL（首选写法 · 只含 `OR` / `AND` / `is_null` / `is_not_null` · 无 `NOT`、无括号）**：

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

**SQL（等价写法 · 若你的版本支持 `NOT` + 括号，可读性更好）**：

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

**SQL（最后备选 · 仅当 `is_null` 报"未知函数"才用 · 注意 `=` 单等号）**：

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

> ★ **三种写法语义完全等价**：只要"五个 telemetry 键有任一缺失"**或**"出现命令键 `c` / `cmd`" ⇒ 不是 telemetry ⇒ 交 `MF`。
> `AND` 优先级高于 `OR` ⇒ 首选版**无需括号**。**用哪种都行，但必须在回写时记明**（§11 / `V2V4-Config-After` §3）。
> ★ **若全部被 SQL 子集拒绝** ⇒ **`STOP`**，把控制台报错原文发我（属 `R-WP4C-7` / `R-WP4C-20`）。

### 7.4 谓词语义铁律（**冻结 · 不得自行改动**）

| # | 铁律 |
|---|---|
| 1 | `value` 用「**键存在**」语义 —— **禁**写成"值非空 / ≠ 0"（否则 `value = 0` / `false` 会被漏判）；`is_not_null(payload.value)` 即此语义 |
| 2 | 谓词**不涉及** `device_id`（设备身份**只**来自 topic） |
| 3 | **禁只判** `type` / `value` / `topic` —— 五个正条件缺一不可 |
| 4 | **`==` 禁用**（`syntax error before: '='` · `R3.1` 实测）；**相等一律单 `=`**，或用 `is_null` / `is_not_null` |
| 5 | **保存前必须用规则页的「启用调试 + 发送测试消息」试跑**（`{"msg":"hello"}` ⇒ 规则 A **不匹配** / 规则 B **匹配**；telemetry 信封 ⇒ 反之） |

---

## §8 全流程时间线（**含窗口标注**）

```
【窗口外】§3 采集（只读，可慢慢做）
  0-1 抄录旧规则/动作运行统计  0-2 截图旧连接器  0-3 截图旧动作
  0-4 抄录旧规则 SQL           0-5 采 D1 计数    0-6（可选）截图 presence 连接器

▼▼▼ 🛑 中断窗口开始（仅影响消息流一条 best-effort 链路 · 期望丢 0–1 条）▼▼▼

【§4 拆除】≤3 min
  4-1 删除规则 r-…a01720（输入规则 ID）
  4-2 删除动作 a-…921e22
  4-3 删除连接器 c-…b12100（输入连接器 ID）

【§5 建连接器】≤5 min
  5-1 新建 CB（URL = 裸主机 · 4 头 · TLS 开 + SNI · 其余照抄）
  5-2 ★ 保存前截图 TLS 段
  5-3 测试连接 ⇒ 期望 200 / 状态「已连接」
  5-4 登记新连接器 ID

【§6 建动作】≤3 min
  6-1 建 MF（CB · POST · Path /api/webhook · 体三键不加引号 · 高级设置照抄）
  6-2 建 HI（CB · POST · Path /api/history/ingest · 头含 X-Webhook-Key · 同上高级设置）

【§7 建规则并启用】≤4 min
  7-1 建 + 启用 guo_feeder_history_ingest（FROM guo_feeder/+/up · WHERE 谓词 · → HI）
  7-2 建 + 启用 guo_feeder_message_ingest（FROM guo_feeder/# · WHERE NOT 谓词 · → MF）

▲▲▲ ✅ 中断窗口结束（消息流恢复落库）▲▲▲

【§9 验证】§9.1 计数差分 → §9.2 curl → §9.3 控制台指标 → V-1…V-7
【§11 回写】V2V4-Config-After（§1 元数据 / §2 / §3 / §4）+ 本文件 §11 登记表
```

> **★ 唯一可优化点**：`§7-1` 建议**先于** `§7-2` 建并启用 ——
> 使 telemetry 在消息流规则尚未就绪时**也已**落入 `telemetry_history`（减少 telemetry 的盲窗）。
> 两者之间**不会双投**（因为消息流规则一出生就带 `NOT 谓词`）。

---

## §9 变更后验证

### 9.1 D1 计数差分（只读）

```bash
cd Cloudflare_Assets
unset http_proxy https_proxy HTTP_PROXY HTTPS_PROXY ALL_PROXY all_proxy
./node_modules/.bin/wrangler d1 execute guofeeder --remote --config guo-feeder-api/wrangler.toml \
  --command "SELECT (SELECT COUNT(*) FROM telemetry_history) AS th, (SELECT COUNT(*) FROM log_history) AS lh, (SELECT COUNT(*) FROM mqtt_messages) AS mm;"
```

| 表 | `V-0` 基线（`2026-10-12` 实测） | 变更后 | 判据 |
|---|---|---|---|
| `telemetry_history` | **0** | `<待填>` | `V-2` 后应 **+1**（telemetry positive） |
| `log_history` | **0** | `<待填>` | log 域同样走 `/up` 之外的 `…/log` ⇒ 仍归消息流规则 |
| `mqtt_messages` | **2458** | `<待填>` | `V-3` 期望 **+1**（command negative）；★ **该表噪声大（近 24h 仅 13 行）** ⇒ 必须看**窗口内增量**，不能用总量 |

### 9.2 端点 curl 复验（可选 · 控制台外旁证）

```bash
BASE=https://guo-feeder-pagesdev.pages.dev
curl -s --noproxy '*' -o /dev/null -w "ingest GET  -> %{http_code}\n" "$BASE/api/history/ingest"   # 期望 200
curl -s --noproxy '*' -X POST -H "Content-Type: application/json" -d '{}' \
     -w "ingest POST(no key) -> %{http_code}\n" "$BASE/api/history/ingest"                          # 期望 401
curl -s --noproxy '*' -o /dev/null -w "webhook GET -> %{http_code}\n" "$BASE/api/webhook"           # 期望非 405
```

### 9.3 控制台侧观测

| 观测 | 期望 |
|---|---|
| `CB` 状态 | **「已连接」** · 「已启用 TLS」 |
| 新连接器资源告警（`resource down` / `alarm_is_activated`） | **不应出现**（若出现 ⇒ 查 SNI / 网络；见 `R-WP4C-16`） |
| 两条新规则的运行统计 | 触发次数随设备上报增长；**失败率应与 presence 链路同量级**（不是 90%） |
| `telemetry_history` 增长 | 应与 telemetry 报文数一致（含幂等去重后的有效数） |

### 9.4 冻结验证集 `V-1…V-7`

| # | 用例 | 期望 | 实际 | 判定 |
|---|---|---|---|---|
| `V-1` | Rule syntax（保存 + 启用） | 可保存并启用 | `<待填>` | |
| `V-2` | telemetry positive | `telemetry_history` **+1** · `mqtt_messages` **+0** | `<待填>` | |
| `V-3` | command negative | `telemetry_history` **+0** · `mqtt_messages` **+1** | `<待填>` | |
| `V-4` | mixed payload（含 `cmd`） | **不是** telemetry | `<待填>` | |
| `V-5` | multi device isolation | 不串 device | `<待填>` | |
| `V-6` | reconnect | 不重复写入 | `<待填>` | |
| `V-7` | duplicate `(device_id,type,ts,seq)` | 落库**恰 1 行** | `<待填>` | |

---

## §10 回滚（**触发即执行 · 顺序不能乱**）

| 步 | 动作 | 说明 |
|---|---|---|
| **1** | **关停新链路**：停用 / 删除规则 `guo_feeder_history_ingest`（+ 必要时 `guo_feeder_message_ingest`） | 先止血 |
| **2** | **恢复消息流**：新建临时连接器（URL **直接写全路径** `https://guo-feeder-pagesdev.pages.dev/api/webhook`）+ 新动作（**Path 留空**）+ 新规则（`SELECT * FROM "guo_feeder/#"`，**无 WHERE**） | ⚠️ **旧的 `c-…b12100` / `a-…921e22` / `r-…a01720` 已不可恢复**（ID 不复用）⇒ 回滚 = **用新 ID 重建等价物** |
| **3** | 校验 | `mqtt_messages` 30 min 增量 ≥1 · `telemetry_history` 停止增长 · 无新告警 |
| **4** | 再决定连接器/动作去留（**建议保留不删**） | 保留可复查 |

> ★ **`R3` 的一个重要代价（须知晓）**：**删除不可逆** —— 旧连接器 / 动作 / 规则的 **ID 消失**，
> 回滚只能"**重建等价物**"（新 ID），**无法把旧对象还原回来**。
> ⇒ 这也是 §3 采集（尤其 **0-1 运行统计** 与 **0-2/0-3 配置截图**）**必须做在前面的原因**。
> 触发条件（对照 `V-0` §5.2）：① telemetry 双投 ② 新规则持续非 2xx 或出现 `resource down` 告警 ③ D1 写异常 ④ `mqtt_messages` 增量显著偏离基线。

---

## §11 施工记录 / 登记表（**执行时填写**）

### 11.1 新对象 ID 登记

| 对象 | ID | 备注值 | 创建时间 |
|---|---|---|---|
| 连接器 `CB` | **`c-n302933b-0c1cb0`** ✅ | `guo_feeder_shared_http` | `2026-10-12` |
| 动作 `MF` | `a-<待填>` | `guo_feeder_message_ingest` | `<待填>` |
| 动作 `HI` | `a-<待填>` | `guo_feeder_history_ingest` | `<待填>` |
| 规则（history） | `r-<待填>` | `guo_feeder_history_ingest` | `<待填>` |
| 规则（message） | `r-<待填>` | `guo_feeder_message_ingest` | `<待填>` |

> ⚠️ **易混提醒**：`HI` 的 **URL 路径 = `/api/history/ingest`**（配规则 A）；`MF` 的 **URL 路径 = `/api/webhook`**（配规则 B）。
> 两个动作都挂同一个连接器 `CB`。**上次截图里"动作备注 `guo_feeder_message_ingest` + 路径 `/api/webhook`"被建到了规则 A 下面 —— 那是错的**（会双投 + 分流失效）。

### 11.2 被删对象（**仅存证，不可恢复**）

| 对象 | ID | 删除前运行统计（§3 0-1） |
|---|---|---|
| 规则（消息流） | `r-n302933b-a01720` | ✅ **已删除**（`2026-10-12`）；统计值 `<待回填>` |
| 动作（消息流） | `a-n302933b-921e22` | ✅ **已删除**（`2026-10-12`） |
| 连接器（消息流） | `c-n302933b-b12100` | ✅ **已删除**（`2026-10-12`） |

### 11.3 本文件施工结果

| 项 | 值 |
|---|---|
| 窗口（起 → 止） | `<待填>` |
| 实际窗口时长 | `<待填（目标 ≤15 min）>` |
| 窗口内丢失消息数（差分推算） | `<待填（期望 0–1）>` |
| 规则 B 实际采用的 SQL 写法 | `<待填：首选(is_null+OR) / 备选(NOT) / 最后备选(= 'undefined')>` |
| 规则 A 实际采用的 SQL 写法 | `<待填：is_null 版 / 'undefined' 版>` |
| 规则页调试试跑结果（§7.0 两组） | `<待填：符合 / 不符合>` |
| TLS 段截图归档路径 | `assets/wp4c-v2v4/<待填>` |
| 是否发生回滚 | `<是 / 否>` |

---

## §12 边界与状态

| 禁止项 | 状态 |
|---|---|
| 创建 / 修改 EMQX Rule · Action · Connector | ✅ **未做**（本文件 = 施工说明；执行由人工完成） |
| 部署（Worker / Pages） | ✅ **未做** |
| 修改冻结设计**核心语义**（方案 A / 谓词 / 端点 / `V-1…V-7`） | ✅ **未改** |
| 修改 firmware / topic / Worker / migration | ✅ **未改** |
| 写入任何密钥值 | ✅ **无**（全占位符 `<WEBHOOK_SECRET>`） |
| `push` | ✅ **未 push** |

**状态变更**：

| 项 | 变化 |
|---|---|
| `P3-WP4C-Connector-Reuse-Amendment.md` | `R2` → **`R3`**（`R2` 的 §4.2 / §5 施工清单**作废**，由本文件取代；**分流语义不变**） |
| `P3-WP4C-EMQX-Rule-Design.md` | `FROZEN DESIGN R2` → **`R3`** → **`R3.1`**（`R3`：施工形态注释；`R3.1`：**SQL 谓词写法修正** —— `==` 禁用 / 改用 `is_null`·`is_not_null`。**语义逐字不变**） |
| `P3-WP4C-Implementation-Plan.md` §8 | 规格 `v2` → **`v3`** → **`v3.1`**（SQL 谓词写法） |
| `H-WP4C-09-B` | 🟡 `READY (R2)` → `READY (R3)` → 🟡 **`READY (R3.1)`** —— **施工规格以本文件为准** |
| `CF-2` | 🔴 仍 `BLOCKED`（判据 = **冻结验证集** `V-1…V-7`） |

> 📌 **Historical snapshot at time T（`R3.1` 施工说明时点 · `2026-10-12`）**。**Final status is recorded in Implementation Record §5 / §6 / §8**：
> `H-WP4C-09-B` = ✅ `CLOSED` · **`CF-2` = ✅ `CLOSED`**（`2026-10-12` 22:20）· 冻结验证集 `V-1…V-7` = **全 `PASS`**。
> 本说明的施工规格**已被执行完毕**（实证 → 任务单 §7.5.7）。本表为历史快照，**不代表终态**。

---

## §13 关联文档

| 文档 | 关系 |
|---|---|
| `docs/review/P3-WP4C-Connector-Reuse-Amendment.md` | `R2` 修订记录（**施工清单已被本文件取代**；机制论证仍有效） |
| `docs/review/P3-WP4C-EMQX-Rule-Design.md` | 🔒 冻结设计（`R3`：仅施工形态注释） |
| `docs/review/P3-WP4C-Implementation-Plan.md` | §8 规格 **v3** |
| `docs/review/P3-WP4C-Implementation-Discipline.md` | **§6A `EMQX Console Boundary Rule`**（本手册的操作纪律来源） |
| `docs/review/P3-WP4C-History-Relay-Deployment-Verification.md` | 端点侧已验证（`PASS 15 / FAIL 0`） |
| `EMQX_Assets/config/data-integration/P3-WP4C-V0-EMQX-Baseline.md` | **变更前基线**（被删对象原文） |
| `EMQX_Assets/config/data-integration/P3-WP4C-V2V4-Config-After.md` | **变更后回写目标** |

---

*本文档于 **`2026-10-12`** 建立，为 **`R3` 施工说明**（成因：**连接器创建后不可编辑**，官方文档明示"编辑连接器仅适用专有版部署"）；
**同日升级 `R3.1`**（成因：**控制台实测 `syntax error before: '='`** ⇒ `R3` 的 SQL 谓词写法有误，改用官方 `is_null` / `is_not_null`，**仅改写法、语义不变**）。
**核心语义（方案 A / 冻结谓词 / `-S2` 目标 / Pages Proxy / 端点 / 验证集）全部保留**；**仅变更施工形态与谓词写法**。
**本文件零生产变更** —— 未创建 / 修改任何 EMQX 配置、未部署、未改 firmware 与 Worker，**不含任何密钥值**。
基线 = 固件仓 **`4c74d3a`** · CF 仓 **`415cd0c`** · EMQX 仓 **`a73968a`**；**已建对象**：`c-n302933b-0c1cb0`。所有平台事实均可按 §1 出处逐条回查。*
**平台依据（`R3.1`）**：EMQX *Rule SQL Reference*（`=` = 完全相等；无 `==`）· EMQX *Built-in SQL Functions*（`is_null(Term)` 判 `undefined`；`is_not_null(Term)` 反之）。*
