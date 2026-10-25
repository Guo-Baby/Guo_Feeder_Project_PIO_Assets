# Guo Feeder · 阶段一（P0 + P1）交付报告 —— 生产阻断、设备状态与操作记录

> 日期：2026-10-25 · 云仓 `Cloudflare_Assets`（本次提交见 §11）· 固件仓零固件改动
> 上游：用户《阶段五 UI 完善与五阶段实施任务书》（决策 D-1～D-11）
> 计划基线：`docs/review/UI-V11-Gap-Audit-and-Implementation-Plan-2026-10-25.md`（§1.1 决策 · §8 唯一进度表）
> **本轮只实施阶段一**；阶段二～五未开始，也未标记完成。

---

## 0. 一句话结论

**两个生产阻断问题都已定位到真实根因并修复（含回归测试），首页/状态/四态卡/日志/能力同步按决议重做完毕；21 个自动化套件 1355 断言 + 3 个真实 Chromium 套件 92 断言全绿。但按任务书禁令未部署生产 —— 因此 R1（审计页 403）的修复目前只在仓库里，生产仍 403。**

---

## 1. 两个生产阻断（P0）——根因与证据

### P0-1 · 审计/命令历史页 403

| 项 | 内容 |
|---|---|
| 现象 | 生产打开「记录 → 审计」必然 403；阶段五的 S5-04/05 检索与导出**实际不可用** |
| 根因 | `guo-feeder-pagesdev/functions/api/read/[[path]].js` 的 `ALLOWED` 白名单**漏了 `history/command`**（前端 `gf-app.js` 在调，403 错误体的 `allowed` 清单里还宣称支持它 ⇒ 契约自相矛盾） |
| 修复 | 只加 **1 条精确正则** `/^history\/command$/`；**不放行** `admin/*` 其他路径、不改鉴权、不去掉会话校验 |
| 证据 | 修复：`read/[[path]].js:44-66`；回归：`tests/pages/read_proxy_allowlist.test.mjs`（21 断言，含"宣称可用 ⇒ 必须放行"与 8 条越权拒绝） |
| 状态 | 代码 ✅ / 自动化 ✅ / 生产 ⚠️ **未部署**（本轮禁止部署） |

> 该测试是**契约测试**而非 handler 级测试：`onRequest` 的顺序是「方法 → 会话 → 白名单」，无真实 D1 会话时请求先 401，走不到白名单分支。故测试做双向源码契约：抽 `ALLOWED` 逐条 eval + 抽 403 宣称清单 + 断言二者一致。**这正是当初逃逸的那一类缺陷。**

### P0-2 · `log_history.code` 全 NULL ⇒ 事件过滤失效

| 项 | 内容 |
|---|---|
| 现象 | 任何"按事件码/模块/类别"过滤都筛不出东西 |
| 实测 | 生产 `log_history` **82/82 行 code IS NULL**（100%） |
| 根因（固件侧实证） | 日志信封**只有** `{device_id,boot_seq,seq,ts,level,payload_base64}`（`src/log/log_envelope.cpp:230-268`）—— 根本没有 `code` 字段；而 `history-ingest.js` 按设计把 payload 当不透明数据（"❌ CBOR 解码"）⇒ EventId 从未落库 |
| 修复（写侧） | `history-ingest.js` 入库前解码**一次**，派生 `code` + 中文 `message`；`payload` 仍**原样**存 base64（log-ack 语义不变）；解码失败**不拒收、不编造** |
| 修复（读侧） | `read-api.js` 新增有界解码扫描：带 `code/module/category/include_noise` 时按块 keyset 扫描（`SCAN_CAP=3000`）解码后匹配，如实回 `scanned/scan_capped/scan_exhausted`；不带这些参数时**走原 SQL 路径逐字节不变** |
| 公共内核 | `log-filter.js::codeFromPayload()` 被写侧与读侧共用 ⇒ 两侧不可能漂移 |
| 证据 | `tests/api/log_i18n_filter.test.mjs`（57）用**12 条真实生产样本**（`tests/fixtures/log_samples/`）验证"能从 payload 派生 EventId"；`tests/api/log_scan_filter.test.mjs`（35）验证过滤/分页/上限/400 |
| 存量数据 | 仍 NULL（不迁库、不回填）。UI 走解码派生，**过滤对存量同样有效**（这就是扫描路径存在的理由） |

---

## 2. 在线/离线时长（D-1 执行口径）

| 场景 | 数据来源 | 计算 | 失败处理 |
|---|---|---|---|
| 在线 | EMQX `/api/v5/clients` 的 `connected_at` | `now - connected_at` | `connected_at` 缺失/非法 ⇒ **「不可用」**，不估算；连接时刻在未来（时钟不同步）⇒ 不计算 |
| 离线 | `device.last_seen`（最近一次**上行**） | `now - last_seen` | 无记录 ⇒ 「不可用」 |
| 状态未知 | EMQX 不可达/限流/过滤不可信 | 时长一律「不可用」 | UI 明示**「不判为离线」** |

**字段实证**（生产只读）：`connected_at = "2026-10-24T09:44:00.615+00:00"`（RFC3339 带偏移，UTC）。前端 `parseRfc3339Sec()` 按瞬时解析（`+08:00` 与 `+00:00` 等价，已测）。

**⚠️ 事实缺口（必须知道）**：
- 生产 `device_event` **0 行**（EMQX webhook 未接入）⇒ 无"下线时刻"记录。因此离线时长标签写的是**「最近一次上行」**，并在 UI 注明"按最近上行估算，不是精确下线时刻"。
- 若要让离线时长精确化，需要接 EMQX `client.connected/disconnected` webhook 到 `device_event`（**本轮未做**，属阶段二/运维项）。

`tests/ui/ui_stage1_core.test.mjs` U1 组 18 条断言锁住以上全部分支。

---

## 3. 首页整改（原型屏 1）

| 要求 | 落地 |
|---|---|
| 主状态卡（重量/温湿度/在线-离线时长/最近一次上行/阀态/称重有效） | `.statuscard` 单卡：重量大字号 + 温湿度 + 时长 + 时刻 + 4 个状态 chip + 来源/时刻说明 |
| 删掉顶部 WiFi dBm / MQTT 状态 / RTC | 首页**已无**这些指标（浏览器断言 S1-1f） |
| 数据来源/更新时间/未知状态 | 每块标注：遥测（含取回时刻）/ EMQX 查询时刻 / 设备查询时刻；缺失写「未取到」而不是 0 |
| 不新增频繁查询 | 阀态/称重**只查 2 键**、结果缓存 60 s（浏览器断言 S1-3a：进首页恰好 2 条 `query_state`） |
| 今日统计 | 仍不存在（R5 回归断言） |

## 4. 当前自动化执行卡（R3）

- 数据源 `query_workflows`；**已核实固件真实字段**：`command_manager.cpp:1417-1454` 回 `{workflows:[{id,name,enable,state,step_count,current_step,stable_id}]}`，`cloud_compress_uplink` 对未知字段原样保留 ⇒ **设备端名称可用（D-3 成立，无需新增存储）**。
- UI：只列 `RUNNING/WAITING`；有 `current_step+step_count` 才显示「第 N / M 步」，缺字段显示「设备未提供步骤进度」；名称为空时用 ID 并**明示不是用户命名**。
- 浏览器断言：S1-4a..d（含"空闲自动化不出现"）。

## 5. 四态命令卡（R20/R21）

### 5.1 状态 ↔ 协议对应（**唯一口径**，来自 `gf-mqtt.js::stateFromFrame` / Worker `command-audit.js`）

| UI 阶段 | 协议证据 | 含义 |
|---|---|---|
| 1 已下发 | `published`（202 / PUBACK） | 帧已发出，**≠ 已执行** |
| 2 已确认 | `acked`（设备 ack 帧）/ `accepted`（受理） | 设备收到，**≠ 动作成功** |
| 3 执行中 | `running`（压缩 `s=2`） | 设备正在执行 |
| 4 最终结果 | `done`(s=0) / `failed`(s=3,4) / `rejected` / `timeout`,`unknown` | 只有 `done` 显示「已完成」 |

### 5.2 来源标注（不混淆两条链路）

| 通道 | 标注 | 说明 |
|---|---|---|
| HTTP 代理 | 已记入云端审计（`command_audit`） | 命令经 Pages → Worker → EMQX |
| MQTT 直连 | **无云端审计记录（直连不经 Worker）** | 与 Worker 无审计写入的事实一致 |

### 5.3 只给**用户操作**建卡（本轮新修的 X1）

初版把**所有**命令都建成卡 ⇒ boot 期的 registry / 状态 / 自动化 / 配置读取把"最近操作"刷满，真正的阀门动作被挤掉（真实浏览器测试 S1-5a 抓到）。现规则：`family === "query"` 静默，其余（action / safety / system 重启 / wfadmin / workflow 运行）建卡。

### 5.4 未知结果与迟到回帧

- 未知态卡提供两个**互不相同**的出口：
  - 「重新查询设备状态」= 只读（`query_state`），**绝不**重发原命令；
  - 「重新下发（新命令）」= 新 `cmd_id` + 二次确认，文案明说"会再次真实驱动设备"。
- 迟到回帧：`gf-mqtt.js` 新增 `_settled` 窗口（5 min）+ `setLateFrameHook`；命令窗口结束后到达的 result 帧**改判**卡片并标注「迟到回帧改判：X → Y」，原判断作废（不静默覆盖）。
- 断言：S1-6a..f（含"重新查询只发查询类、无 VALVE_*"与"新命令 cmd_id 不复用"）。

## 6. System State 面板（R10）

- 位置：移到「**设备信息之下**」（浏览器断言用 `compareDocumentPosition` 证明，不是靠人眼）。
- 入口：「刷新常用状态（8 项）」/「查询全部状态（27 项，带二次确认）」。
- 分组改为易懂中文：**阀门 / 出粮电机 / 振动马达 / 称重 / 温湿度 / 网络与连接**（不再用笼统的"执行器"）。
- 状态语义四分开：`未查询（点上方按钮向设备读取）` / `查询中…` / `查询失败（原因）` / `未取到（设备未回该键）`，并在面板顶部明确写「未查询**不是故障**」。
- 回帧解析保持压缩形态 `{s,c,i,o,value}`（`stateValueFromFrame` 兼容 `key` 与 `o`）。

## 7. 日志：中文化 + 去噪 + 筛选（R22/R23/R24）

### 7.1 中文映射（102 事件全覆盖 + 参数）

- 新增 `guo-feeder-api/src/log-i18n.js`：`EVENT_ZH`（**102/102**，与码表一一对应，不多不少）、`MODULE_ZH`（15 模块，`workflow→自动化`）、`PARAM_ZH`（68 个常用参数）。
- 未知事件**不编造**：显示「未知事件 + 原始码」，`translated:false` 让 UI 额外显示 `0xNNNN`。
- 生成物 `log-codes.generated.js` **零改动**（它仍是唯一码表来源）。

### 7.2 去噪口径（D-9 + 生产实测）

| 默认折叠（`noise`） | 依据 |
|---|---|
| `0x0A04` 命令已应用（回执） | 每条命令一条回执，无用户信息量 |
| `0x0511` 供水安全响应 | HX711 噪声期 5 s 一条 |
| `0x0901` 温湿度计数据解析成功 | **生产实测 84 条样本占 56.0%**（本轮新增结论，原审计未识别） |

- `0x0505` 强制关阀（CRITICAL）**不折叠**：用「重复聚合 ×N」压缩刷屏（`groupRepeatedLogRows`，60 s 窗口，行守恒）。
- 折叠**不删数据**：UI 顶部显示「已折叠 N 条通信噪声（勾选上方开关可看）」；勾选后可见并带「噪声」标记。
- 原始记录一条不删：所有折叠都在查询/展示层。

### 7.3 筛选

- 「级别 / 模块 / 类别 / 时间范围」四维 + 「显示通信噪声」开关；服务端是权威口径（`code/module/category/include_noise/from`），本地缓存只做即时切换。
- 自动化事件摘要（R24）：由 `0x0401/0x0402/0x0403/0x0404` + 参数 `0x01 slot / 0x06 用时 / 0x07 完成步数 / 0x0A 失败步骤 / 0x11 停止原因` 拼句；名称来自设备端 `query_workflows`，取不到就**明说 "slot N"**，不伪装成用户命名。

### 7.4 本轮修的 X2（大小写不敏）

码表键是**大写 hex**（`0x0A04`），而所有入口把 id `toLowerCase()` ⇒ `EVENTS[id]` / `EVENT_ZH[id]` / 码匹配直接比较**全部命中不了**：含字母的事件码显示「未知事件」、按码筛不出、参数中文缺失。生产样本里 `0x0A04` 占 40.5% 却显示"未知事件"，据此发现。
→ 建三张小写索引表（事件 / 中文名 / 参数）+ 码匹配大小写归一；新增 10 条回归断言（L-4i..o、F-6f..k）。

## 8. 能力同步三态（R28）

- 三态：`syncing`（动作按钮禁用 + 说明）/ `failed`（状态条 + 原因 + **重试按钮** + 动作禁用）/ `ok`（隐藏状态条，版本冲突时给说明）。
- 入口：全局 `#syncbar`（在命令条下方），失败时 `#sync-retry` 强制重同步。
- 版本冲突：对比缓存版本与设备版本给出「缓存 vX ⇒ 设备 vY」提示。
- 浏览器断言：S1-9a..e（首屏失败 → 禁用 → 重试成功 → 恢复）。

---

## 9. 测试结果（分层，不合并）

### 9.1 自动化（Node，无浏览器）

| 套件 | 断言 | 内容 |
|---|---|---|
| `tests/api/log_i18n_filter` | **57** | 102 事件中文覆盖 / 参数中文 / 分类规则 / **12 条真实样本派生** / 参数校验 / 大小写回归 |
| `tests/api/log_scan_filter` | **35** | 解码扫描 / 去噪 / 按码按模块按类别 / 扫描上限 / 分页 / 400 |
| `tests/pages/read_proxy_allowlist` | **21** | 白名单契约（P0-1 回归） |
| `tests/ui/ui_stage1_core` | **59** | 时长 6 分支 / 四态映射 10 态 / 来源标注 / 日志过滤 / 重复聚合 / 自动化卡 / 同步三态 |
| 既有 17 套件 | 1183 | 全部保持绿（含 `ui_app` 194、`ui_security` 273、`mqtt_pool` 68 …） |
| **合计** | **1355 PASS / 0 FAIL（21 套件）** | |

### 9.2 真实 Chromium（`tests/e2e/stage1_ui_playwright.mjs`）

**47 断言 / 0 FAIL**，3 个独立 context（A 直连 · B 同步失败 · C HTTP 未知态）：

- 首页 8 项事实齐备、无 WiFi/MQTT/RTC、在线时长 = 1 小时 2 分（来自 `connected_at`）、进首页只查 2 键、自动化卡含设备端名称与「第 2 / 4 步」；
- 四态卡：读命令不建卡（0 张）→ 关阀后 1 张 → 四阶段齐 → 来源「MQTT 直连 · 无云端审计」→ 终态「已完成」；
- System State 面板在设备信息**之后** + 两个入口 + 中文分组 + "不是故障"；
- 日志：`include_noise=0` 生效、默认不见噪声、中文名、模块中文、自动化摘要、详情展开显示 `0xNNNN`、勾选后噪声可见并标记、模块筛选进请求；
- 未知态：结果未知 → 两个出口 → **重新查询只发查询类（无 VALVE_*）** → 重新下发发新命令且 `cmd_id` 不复用；
- 能力同步：失败状态条 + 动作禁用 + 查询仍可用 → 重试成功 → 状态条消失、按钮恢复；
- 375px 无横向滚动；三个流程零未捕获异常。

### 9.3 其它浏览器套件回归

| 套件 | 结果 |
|---|---|
| `stage5_ui_playwright` | 22/22（改 1 条断言：面板文案由「未查询」变「未查询（…）」） |
| `mqtt_browser_playwright` | 23/23 |
| `mqtt_browser_e2e`（真 EMQX） | 未跑：脚本有"生产池已存在则拒绝触碰"的守卫，属**设计行为**（阶段四已验证 33/33） |

### 9.4 API 仓自带套件（`guo-feeder-api/test/run_all.mjs`）

25/26 套件通过；唯一失败 = 既有 `S3-10-1b`（cloud_api 写路径，mock 下 EMQX publish 报 CONFIG_MISSING，**与本次改动无关**，阶段四时同状态）。

### 9.5 生产只读验证（未做任何写操作）

| 核对项 | 结果 |
|---|---|
| `log_history.code` | **84/84 抽样全 NULL**（P0-2 根因确认） |
| `device_event` | **0 行**（EMQX webhook 未接入 ⇒ 离线时长只能按上行近似） |
| EMQX `/clients.connected_at` | `2026-10-24T09:44:00.615+00:00`（RFC3339，UTC） |
| 日志真实分布（84 条解码） | 0x0901 **56.0%** / 0x0A04 **40.5%** / 其余 3.5% ⇒ 去噪口径据此调整 |

---

## 10. 未验证 / 剩余风险（不得当作已完成）

| 项 | 状态 | 说明 |
|---|---|---|
| R1 生产生效 | **未部署** | 本轮任务书禁止部署 Pages/Worker ⇒ 生产审计页仍 403 |
| 生产日志新数据 `code` | 未验证 | 写侧修复要等 Worker 部署后才有新行 |
| 离线时长精确性 | 受限 | 无下线事件（`device_event` 空）⇒ 按最近上行估算，已在 UI 明示 |
| 真机动作 | 未验证 | DRV8825/HX711 未修复；步进/振动相关仍带"未真机验证"提示 |
| 自动化结束摘要 | 部分 | 无 `0x0402` 真实样本；仅在有真实事件时显示，缺字段降级 |
| 能力同步失败的重试 | 浏览器已验证本地 | 生产未验证（需部署后再看） |
| 日志扫描成本 | 有界但需观察 | `SCAN_CAP=3000` 行/请求；生产日志量增长后应观察 D1 读行数 |

---

## 11. 变更清单

### 11.1 代码

| 文件 | 变更 |
|---|---|
| `guo-feeder-api/src/log-i18n.js` | **新增**：102 事件中文 + 15 模块中文 + 68 参数中文 + 分类（D-9）+ 大小写不敏感索引 |
| `guo-feeder-api/src/log-filter.js` | **新增**：过滤参数解析/匹配/派生内核 `codeFromPayload`/扫描常量 |
| `guo-feeder-api/src/log-decode.js` | 增量：`event.name_zh/translated/module_zh/category`、参数 `name_zh`；未知事件不编造 |
| `guo-feeder-api/src/read-api.js` | 日志端点：事件级过滤 + 有界解码扫描 + 命中行 `event`/`code` 补齐；响应新增 `scanned/scan_capped/scan_exhausted/filter` |
| `guo-feeder-api/src/history-ingest.js` | 写侧派生 `code` + 中文 `message`（payload 原样） |
| `guo-feeder-pagesdev/functions/api/read/[[path]].js` | **P0-1**：白名单 +`history/command` |
| `guo-feeder-pagesdev/assets/js/gf-ui-core.js` | 新增：时长/连接生命周期/四态阶段/来源标注/日志过滤与聚合/自动化卡/同步三态；分组中文；状态行失败态 |
| `guo-feeder-pagesdev/assets/js/gf-app.js` | 首页重构（主状态卡 + 自动化卡 + 最近操作）、四态卡与未知出口、状态面板重排、日志 tab 重做、能力同步条、`query` 族静默、文案统一「自动化」 |
| `guo-feeder-pagesdev/assets/js/gf-mqtt.js` | 迟到回帧窗口 `_settled` + `setLateFrameHook` |
| `guo-feeder-pagesdev/app.html` | 新增 `#syncbar`；状态卡/四阶段/详情/标题层级样式 |

### 11.2 测试

| 文件 | 变更 |
|---|---|
| `tests/api/log_i18n_filter.test.mjs` | 新增 57 |
| `tests/api/log_scan_filter.test.mjs` | 新增 35 |
| `tests/pages/read_proxy_allowlist.test.mjs` | 新增 21 |
| `tests/ui/ui_stage1_core.test.mjs` | 新增 59 |
| `tests/e2e/stage1_ui_playwright.mjs` | 新增 47 |
| `tests/pages/ui_app.test.mjs` | 改 1 条（命令条→四态卡，去重语义不变） |
| `tests/ui/ui_stage5_remaining.test.mjs` | 改 2 条（分组中文、未查询文案） |
| `tests/e2e/stage5_ui_playwright.mjs` | 改 1 条（未查询文案） |

### 11.3 文档

| 文件 | 变更 |
|---|---|
| `docs/review/UI-V11-Gap-Audit-and-Implementation-Plan-2026-10-25.md` | 写入 D-1～D-11 决策、五阶段合并、需求追踪验证列、**§8 唯一进度表**（含 X1/X2/X3 与本轮未验证项）、文档维护规则 |
| `docs/review/UI-Stage1-P0P1-Delivery-2026-10-25.md` | **本报告（新增）** |
| `.workbuddy/memory/MEMORY.md` / `2026-10-25.md` | 更新阶段状态与新踩坑 |

### 11.4 数据库 / 协议 / 固件

- **无迁移**（未新增、未修改任何已应用迁移）。
- **无协议改动**（未改 Topic、帧格式、命令族；固件零改动）。
- **无生产写操作**（对生产仅做 `SELECT` 与 EMQX `GET`）。

---

## 12. 下一步（阶段二前置，未开始）

1. **部署本阶段**（需授权）：Pages（白名单修复 + 前端）+ Worker（日志派生/过滤）⇒ 之后才能做「生产只读验证」R1 与"新数据 code"核对。
2. 观察生产日志扫描的 D1 读行数（`SCAN_CAP` 是否够用）。
3. 阶段二起点：快捷操作 1..12 + 排序/折叠、布局管理、指定脉冲弹窗、危险确认弹层、自动化详情/编辑器、重启恢复 6 段。
4. 若要精确离线时长：接 EMQX `client_disconnected` webhook → `device_event`（运维项，非 UI）。
