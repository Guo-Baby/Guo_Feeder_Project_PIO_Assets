# `F-C2-8` / `F-C2-9` 收尾与 `V-3.3` 端到端验收 · 验证记录

> ## 📋 验证记录（`READ-ONLY` 观察 + **已授权的最小生产部署** · 不含固件/Worker 之外的实现变更）
>
> | 项 | 值 |
> |---|---|
> | **执行时间** | `2026-10-15`（GMT+8 约 `21:20 – 22:45`） |
> | **本轮结论** | **文档 ✅ · 实现/测试复核 ✅ · Worker 部署 ✅ · EMQX 线上路由核验 🔴 `BLOCKED` · `V-3.3` 真实设备 E2E 🔴 `BLOCKED`** |
> | **是否可关闭任务** | **否** —— `V-3.3` 需 EMQX `/log` 分流 + 设备在线，二者本轮均不满足 |
> | **生产变更** | **仅 1 项**：Worker `guo-feeder-api` 部署 `e328a11` ⇒ 版本 `dd5ab824`（回滚 `99e6d937`） |
> | **未做** | 未改固件 / 未改 EMQX 配置 / 未改 D1 schema 或 migration / 未写生产 D1 测试数据 / 未 push |

> ## ★ 后续收口（`2026-10-16` 追加 · 本记录结论**不改**）
>
> 本记录的两项 `BLOCKED` 均已于 `2026-10-16` 解除：**EMQX `/log` 分流**由**人工在控制台完成并核实**（规则 A/B · `HR-C2-7a` 黑名单形态）；
> **`V-3.3` 真实设备 E2E** 已在真机完成 —— `log_history` `0 → 13`（`level` 全 `INFO` · 幂等吸收重投）⇒ **`V-3.3` `BLOCKED` → `PASS`**。
> ⇒ **`Phase C-C2 = CLOSED`** · `V-3.1…V-3.9` 全 `PASS` → **`docs/review/Phase-C-C2-Final-Closure-Report.md`**。

---

## §1 目标与范围

1. 澄清「设备 MQTT 日志信封」与「EMQX → Worker HTTP 接收信封」的契约（`F-C2-9` 根因）。
2. 复核 `F-C2-8`（数字 `level`）与 `F-C2-9`（嵌套 `payload`）实现与测试。
3. 核实并执行获准范围内的 Worker 部署。
4. 核实 EMQX 当前真实 `/log` 路由。
5. 完成 `V-3` 路由验证与真实设备 E2E。
6. 证据归档与验收结论。

---

## §2 三仓 Git 状态

| 仓 | 分支 | 起始 HEAD | 结束 HEAD | 工作区 | 远端 |
|---|---|---|---|---|---|
| 固件 `Guo_Feeder_Project_PIO_Assets` | `wb` | `4bb9206` | **`76c4cc5`** | 4 份 `REV-3` 未提交文档**原样保留**（哈希未变 · §3.3） | 未 push |
| 云端 `Guo_Feeder_Project_Cloudflare_Assets` | `main` | `e328a11` | `e328a11`（**本轮无新提交**） | **clean** | 未 push（ahead 7） |
| EMQX `Guo_Feeder_Project_EMQX_Assets` | `main` | `3165aab` | `3165aab`（未动） | **clean** | 未 push |

> **★ `F-C2-8` 四笔提交分属两仓**（已核实）：`fed18be` / `b1c1f83` 在**云端仓**；`53ada3b` / `4bb9206` 在**固件仓**。

---

## §3 本轮变更与提交

### 3.1 新增文档（固件仓 · `docs-only`）

| 提交 | 文件 | 说明 |
|---|---|---|
| **`76c4cc5`** | `docs/interfaces/log_envelope_contract.md`（209 行） | **两层信封契约澄清** —— §2 设备 MQTT 信封（6 字段 + `level` 数值契约）· §3 EMQX→Worker 接收信封（**嵌套为规范**、扁平仅兼容、冲突拒收、`device_id` 一致性覆盖双层）· §5 Worker 实际行为（以代码为准）· §7 常见误判 · §8 依据 |

### 3.2 本轮新增/修改的其他文件

- 本文件（`docs/review/F-C2-8-F-C2-9-V33-Closure-Verification-Record.md`）—— 归档记录。
- **未修改**任何固件源码 / Worker 源码 / EMQX 配置 / migration。
- **未新增 `docs/README.md` 索引行** —— 该文件处于 `REV-3` 未提交状态（用户已有改动），为避免把本任务混入用户未提交工作，**刻意不改**（索引待 `REV-3` 提交后补）。

### 3.3 `REV-3` 未提交文档保护（哈希证据）

| 文件 | 会话开始 `hash-object` | 结束 `hash-object` |
|---|---|---|
| `HANDOFF.md` | `9e68a38e…` | `9e68a38e…` ✅ |
| `docs/README.md` | `93407e17…` | `93407e17…` ✅ |
| `docs/review/Phase-C-C2-Log-History-End-to-End-Review.md` | `0151cae5…` | `0151cae5…` ✅ |
| `docs/review/Phase-C-C2-Log-Routing-Console-Task-Sheet.md` | `21850477…` | `21850477…` ✅ |

---

## §4 实现复核与测试结果（**本轮实跑**）

### 4.1 复核要点（对照代码取证）

| # | 检查项 | 结论 | 依据 |
|---|---|---|---|
| 1 | 嵌套 `payload` 对象能提取日志信封 | ✅ | `history-ingest.js:383` `return [nestedEnv]` |
| 2 | 兼容输入范围与文档一致 | ✅ | 数组 / 对象 / 顶层 `payload_base64` / 字符串 `payload` 四分支 |
| 3 | 嵌套与顶层字段冲突被拒 | ✅ | `logEnvelopesAgree()`（`:103` · 调用点 `:374`） |
| 4 | `device_id` 不一致被拒 | ✅ | `ID-6` 段同时校验嵌套与顶层 |
| 5 | 数字 `level` 严格类型/范围 | ✅ | `asLevelCode()`（严格 number + 整数 + `0..255`） |
| 6 | `DEBUG` ⇒ `filtered`，不误计 `rejected`、不落库 | ✅ | `LOG_FILTERED`（`:125` / `:572` / `:260`） |
| 7 | `1..4` → `INFO/WARN/ERROR/CRITICAL` | ✅ | `LOG_LEVEL_BY_CODE`（`:76-81`） |
| 8 | D1 `log_history.level` 仍 `TEXT NOT NULL` · **无 schema 变更** | ✅ | `0003_p3_history_tables.sql:82`；migration 工作区**未改**（最后提交 `9b25203`） |
| 9 | 响应 `{accepted,duplicate,rejected,filtered}` 语义一致 | ✅ | `:294-301` |
| 10 | 重复/非法/时间戳/Base64/去重无回归 | ✅ | 真实 D1 `§[4]`/`§[16]`/`§[17]` 全绿 |
| 11 | telemetry `/up` 行为不变 | ✅ | `history_ingest` 32/32 · 新套件 `C-4` 显式断言 |

### 4.2 测试实跑（**本轮命令与结果**）

| 套件 / 脚本 | 命令 | 结果 |
|---|---|---|
| `log_envelope_shape.test.mjs`（`F-C2-9`） | `node test/current/log_envelope_shape.test.mjs` | **57 / 57 PASS** · FAIL 0 |
| `log_level_contract.test.mjs`（`F-C2-8`） | `node test/current/log_level_contract.test.mjs` | **95 / 95 PASS** · FAIL 0 |
| `history_ingest.test.mjs` | `node test/current/history_ingest.test.mjs` | **32 / 32 PASS** · FAIL 0 |
| 全量 runner | `node test/run_all.mjs` | **8 / 8 套件 OK**（exit 0 · legacy 7 `EXPECTED_FAILURE` 属既有设计） |
| 真实 D1 引擎 | `bash test/real_d1/history_ingest_real_d1.sh`（`wrangler dev --local` + 本地 D1 引擎 · scratch 目录） | **4 次运行：`123/0` · `122/1` · `123/0` · `123/0`** |

> ⚠️ **如实登记**：4 次真实 D1 运行中 **1 次出现单个 `FAIL`**（`122/1`）。该次完整日志未留存；**相邻 3 次均 `123/0` 且未复现**，且该次 `§[17]` 的确定性断言在尾部输出中均为 PASS。**判定 = 本地瞬时抖动**（`wrangler dev`/localhost curl 类），**非本任务引入的代码缺陷**；建议验收时复跑一次确认。
> **HTTP 200 不作为入库证据** —— 所有写入类断言均以 **D1 实际行数 / 列值** 为准。

---

## §5 Worker 部署（生产 · 已授权最小变更）

### 5.1 部署前检查记录

| 项 | 值 |
|---|---|
| **目标 Worker / 环境** | `guo-feeder-api` · 账号 `9e72d201efec6925b3a468a0e3fb4928`（`wyl94@foxmail.com`）· 生产 `https://guo-feeder-api.guobaby.workers.dev` |
| **待部署提交** | 云端仓 `main` @ **`e328a11`**（工作区 clean） |
| **实际部署文件范围** | `guo-feeder-api/src/**` 打包（`src/index.js` 入口） |
| **代码变更面** | 相对上一生产版本，**仅 `src/history-ingest.js`**（198+/25−）；`/api/webhook` · `/api/presence/event` · `/api/command` · `/api/admin/**` 逻辑**未变**（`415cd0c` 仅改 Pages/README） |
| **D1 绑定** | `guofeeder_DB` → D1 `guofeeder`（`0d8385ba-…`）· **无 schema / migration 变更** |
| **回滚基线** | 版本 **`99e6d937-4408-47b7-8170-4bfa71c9dfec`**（`2026-10-11T19:59Z`） |
| **`/up` telemetry 兼容性** | ✅ 本机 + 真实 D1 引擎均证 telemetry 路径零变化 |
| **部署前测试** | ✅ 上述 4.2 全绿 |
| **回滚方案** | `wrangler rollback` / `wrangler versions deploy 99e6d937…`（**触发条件**：health ≠ 401、telemetry 摄取异常、`/api/webhook` 回归） |

### 5.2 部署结果

| 项 | 值 |
|---|---|
| 命令 | `wrangler deploy --config wrangler.toml`（`--dry-run` 先行校验通过：87.56 KiB / gzip 20.54 KiB） |
| **新版本 ID** | **`dd5ab824-ec39-41f3-9990-a37903aaf61f`**（`2026-10-15T14:25:11Z` = 本地 `22:25` · 100% 流量） |
| 部署消息 | `P3 F-C2-8/F-C2-9 fix @ e328a11 (numeric level + nested log envelope) [WB 2026-10-15]` |
| **部署后健康** | `POST https://guo-feeder-pagesdev.pages.dev/api/history/ingest`（无 key）⇒ **`401 {"error":"unauthorized"}`** ⇒ Pages 中转 → Worker 全链可达、鉴权生效 |
| Pages 工程 | **未改动、未重部署**（中转层逐字转发，无需变更） |
| **回滚** | **未触发** |

> **遗留缺口（如实登记）**：**无法在线上用 `X-Webhook-Key` 发一次真实请求验证 `F-C2-9` 行为** —— 密钥仅存于 EMQX 控制台与 Worker Secret（本地副本受安全门禁保护，未读取）；且设备离线（§7）。⇒ 线上行为验证 = `NOT RUN`。

---

## §6 EMQX 当前真实路由（任务 D）

### 6.1 直读配置：**`BLOCKED`**（平台限制 · **本轮实测**）

| 探测 | 结果 |
|---|---|
| `GET /api/v5/rule_engine/rules` | **`403`** |
| `GET /api/v5/connectors` | **`403`** |
| `GET /api/v5/actions` | **`403`** |
| `GET /api/v5/bridges` | **`403`** |
| `GET /api/v5/clients` | `200` ✅ |
| `GET /api/v5/subscriptions` | `200` ✅ |

⇒ **EMQX Cloud Serverless 的数据集成配置只能经控制台读改（API 403）**，与 EMQX 仓 `README.md` §「数据集成…全 403」一致。**代理无控制台权限 ⇒ 规则/动作/连接器的「当前实时配置」无法直接核验**（`LIVE CONFIG NOT VERIFIED`）。

### 6.2 只读旁证（当前生产事态）

| 证据 | 值 | 来源 |
|---|---|---|
| `/clients?clientid=dev_288485896ce4` | **`[]`（设备**不在**在线列表）** | EMQX API（本轮） |
| `/subscriptions` | `dev_288485896ce4` → `guo_feeder/288485896ce4/down`（QoS1） | EMQX API（本轮） |
| D1 `mqtt_messages` 最近 6 行 | **全部** `guo_feeder/288485896ce4/log`，时间 `21:33:32 – 21:35:49` | D1 `--remote`（本轮只读） |
| D1 `mqtt_messages` 中 `/log` 累计 | **782** 行；**近 24 h 147** 行 | D1 `--remote` |
| D1 `log_history` | **`0`** | D1 `--remote` |

⇒ **当前生产事态（最近一轮设备活动 = `21:35`）**：`/log` 仍全部进入**旧 webhook 路径**（`mqtt_messages`），`log_history` 恒 0 ⇒ **EMQX 侧 `/log` → `/api/history/ingest` 的分流仍未生效**。
> ⚠️ 这是**运行时旁证**，**不是**配置直读 ⇒ 只可表述为「按最近流量判定未分流」，**不可**据此断言控制台当前 SQL 内容。

---

## §7 真实设备端到端（任务 E）

| 前置 | 状态 |
|---|---|
| Worker 已部署 | ✅（§5） |
| EMQX `/log` 分流就绪 | 🔴 **未满足**（§6.1 `BLOCKED` + §6.2 旁证未分流） |
| 设备在线并可发 `/log` | 🔴 **未满足** —— `/clients` 返回 `[]`；最后一条 `/log` 落库于 `21:35:49`（约 50 min 前） |

⇒ **E1 预检**：未执行（前置不满足）。**E2 实际发送与链路观测**：**`NOT RUN`**。**E3 关键验收场景**：**`NOT RUN`**。
**未使用**手工构造 JSON 冒充真实设备测试，**未**为测试创建/删除生产数据。

---

## §8 D1 生产读数（只读 `SELECT`）

| 时刻（本地） | `telemetry_history` | `log_history` | `mqtt_messages` |
|---|---|---|---|
| `22:25`（部署后） | **15** | **0** | **3376** |
| `22:28`（观察窗口后） | 15 | 0 | 3376（`mm_last` = `21:35:49`） |

> **本轮对生产 D1 仅执行 `SELECT`**；`changes: 0` / `rows_written: 0`。**未插入 / 未删除 / 未伪造**任何生产数据。

---

## §9 未解决问题 · 权限限制 · 风险

1. 🔴 **EMQX 数据集成 API 全 `403`** ⇒ `/log` 路由配置**无法**由本环境核验或修改；须**人工控制台**（承接 `HR-C2-4/5/6/7`）。
2. 🔴 **设备当前离线** ⇒ 真实 E2E 无法进行；须**设备上线**。
3. 🟠 **线上 `F-C2-9` 行为未验证**（缺 `X-Webhook-Key` 访问路径 + 设备离线）。
4. 🟠 **`F-C2-9` 的设计层歧义已由本轮文档收敛**（`76c4cc5`），但 `P3-DT-WP4-History-Ingestion-Design.md` §2.4b 原文仍可读作「顶层」—— 建议**另开固件仓 docs-only 任务**做一次措辞校正。
5. 🟡 **`0003_p3_history_tables.sql` 注释**仍写「冻结枚举 WARN / ERROR / CRITICAL」（`F-C2-8` 后已含 `INFO`）—— 该 migration **已上线不得编辑**，属**已登记的文档滞后**。
6. 🟡 **真实 D1 引擎 1/4 次瞬时 `FAIL`**（未复现 · 详见 §4.2）。
7. 🟡 `docs/README.md` 索引**未加**本记录与契约文档条目（该文件处 `REV-3` 未提交状态）。
8. ⚪ 未 push（三仓均未推送；云端仓 ahead 7、固件仓 ahead 8）。

---

## §10 回滚

| 项 | 值 |
|---|---|
| 是否回滚 | **否** |
| 配置回滚 | Worker ⇒ `wrangler rollback`（回 `99e6d937…`）；EMQX/Pages/固件**本轮未改**，无需回滚 |
| **数据副作用** | **无** —— Worker 部署未写入任何生产数据；本轮生产 D1 仅 `SELECT` |
| 触发条件 | health ≠ `401` · `telemetry_history` 摄取异常 · `/api/webhook` 或 presence 回归 |

---

## §11 验收矩阵

| # | 验收项 | 状态 | 依据 |
|---|---|---|---|
| 1 | 文档契约澄清完成 | ✅ **`PASS`** | `76c4cc5` · `docs/interfaces/log_envelope_contract.md` |
| 2 | `F-C2-8` 本轮复核通过 | ✅ **`PASS`**（代码 + 逻辑 95/95 + 真实 D1 引擎） | §4 |
| 3 | `F-C2-9` 本轮复核通过 | ✅ **`PASS`**（代码 + 逻辑 57/57 + 真实 D1 引擎 `§[17]`） | §4 |
| 4 | Worker 部署到明确目标环境 | ✅ **`PASS`** | `dd5ab824` @ `2026-10-15T14:25Z` · health 401 |
| 5 | EMQX `/log` 路由由**当前线上证据**证明 | 🔴 **`BLOCKED`** | 配置 API `403`；旁证显示**未分流**（§6） |
| 6 | `V-3.3` 真实设备 E2E | 🔴 **`BLOCKED`**（且 `NOT RUN`） | 路由未就绪 + 设备离线（§7） |
| 7 | `/up` telemetry 无回归 | 🟡 **`CONDITIONAL PASS`** | 本机 + 真实 D1 引擎全绿；**线上**因设备离线**无新遥测流量**可对照（`th` 恒 15） |
| 8 | 可正式关闭 `F-C2-8` / `F-C2-9` / `V-3.3` | `F-C2-8`/`F-C2-9` = **代码与引擎层已闭合**（生产端到端仍缺）· `V-3.3` = **不可关闭** | §11 |
| 9 | 仍需人工授权事项 | **是**（§12） | — |

---

## §12 最小后续动作（**需人工授权 / 设备侧**）

1. **（人工 · EMQX 控制台）** 按 `docs/review/Phase-C-C2-Log-Routing-Console-Task-Sheet.md`（`REV-3`）执行 `/log` 分流（方案 B），并回填 `HR-C2-*` 裁决。
2. **（设备侧）** 让 `288485896ce4` 上线并产生**可识别的真实 `/log`**（普通 `INFO`/`ERROR` 即可）。
3. **（代理 · 只读）** 分流后跑 `V-3.1…V-3.9`：MQTT 抓包 + `wrangler tail` + D1 `log_history` 增量，并核对 `/up` telemetry 无回归。
4. **（可选 · docs-only）** 另开固件仓任务校正 `P3-DT-WP4-History-Ingestion-Design.md` §2.4b 措辞；`REV-3` 提交后补 `docs/README.md` 索引。
5. **（人工 · 推送授权）** 三仓共 15 笔未推送提交（固件 8 · 云端 7）待裁决是否 push。

---

*本文件于 **`2026-10-15`** 建立，为 `F-C2-8` / `F-C2-9` 收尾与 `V-3.3` 端到端验收的**验证记录**；唯一生产变更 = Worker 部署 `dd5ab824`（可一键回滚）；**未改固件 / EMQX / D1 schema · 未写生产数据 · 未 push**。*
