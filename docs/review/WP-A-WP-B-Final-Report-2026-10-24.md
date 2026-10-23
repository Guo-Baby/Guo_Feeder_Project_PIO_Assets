# WP-A 阀门超时告警 → WP-B UI 开发 · 最终分类报告（2026-10-24）

> 范围：任务书两个工作包（WP-A 阀门安全超时告警 / WP-B 直接继续 UI 开发）。
> 纪律：以仓库实际代码与生产实测为准；本地通过 ≠ 生产验证；设备在线 ≠ 下行正常。
> 本文所有"PASS"均指明验证方式（单测 / 生产只读实测 / 代码路径审查 / 第三轮真机证据）。

---

## 1. 阀门告警实现状态（WP-A）

### 1.1 结论：零固件改动、零 DDL、零协议扩展

A1 只读检查结论（以仓库代码为准）：**固件早已具备阀门安全超时告警能力**——
`valve_task()` 超时后**先 `valve_set_gpio(false)` 关阀**，再 `event_push` →
`log_emit(LOG_VALVE_SAFETY_TIMEOUT = 0x0508, WARN, {OPEN_MS=0x13, LIMIT_MS=0x14})`，
带一次性报告锁 `safety_timeout_reported`。真正缺口是**云端/UI 无人解码展示**
（`log_history.code/message` 恒 NULL，信封不含事件语义）。

⇒ 实现全部落在云端与 UI 侧，**未引入任何新 `device_event.type` / `EventId` /
协议扩展**（无此需求；既有表达方式完全够用）。

### 1.2 交付物

| 层 | 文件 | 内容 |
|---|---|---|
| 码表 | `scripts/gen-log-codes.mjs` → `guo-feeder-api/src/log-codes.generated.js` | 单一来源 `src/log/log/log_events.h` → 102 事件 / 93 参数；`--check` 校验同步；多等级注释记 null 不猜 |
| 解码 | `guo-feeder-api/src/log-decode.js` | Worker **唯一** CBOR 解码点；`decodeLogPayload` 永不抛；降级 reason ∈ `cbor_error/short_record/unsupported_version/unknown_event`；crc 不匹配仅 warnings；`time_source` ∈ record/created_at；输出毫秒 |
| 出口 | `read-api.js handleHistoryLog` | `?decode=1` 附加 `decoded`；**缺省/0 老行为逐字节不变**；非法值 400；payload 原文始终返回 |
| UI | `gf-ui-core.js alertsFromLogItems` + `gf-app.js renderAlertCard/alertExplainer` | 告警卡字段：设备 / 时间（含时间来源标注）/ 实际开启时长 / 安全阈值 / **超出量（formatExcessMs，精示亚秒差）** / 事件标识（boot+seq）/ 事件名+ID；`alertExplainer()` 明示"一条告警不代表任何命令失败" |
| 文档 | 固件仓 `docs/review/WP-A-Valve-Timeout-Alert-Design-2026-10-24.md` | A1 结论、协议兼容性判定、最小设计、失败行为表、验收口径 V1–V7 |

### 1.3 验收结果（A3 · V1–V7）

| # | 验收项 | 结果 | 证据 |
|---|---|---|---|
| V1 | 本地关阀不依赖云端（告警构造/发送/重试/云端处理不阻塞关阀） | **PASS** | 代码路径审查：`valve_set_gpio(false)` 先于 `log_emit`；第三轮真机 440 项含关阀证据 |
| V2 | 正常开阀不误报 | **PASS** | 一次性锁语义（仅超时触发）；生产扫描窗口内唯一 0x0508 即黄金样本 |
| V3 | 告警不覆盖原命令终态 | **PASS** | 单测机器可检断言：告警输出**不含任何命令终态字段**（state/terminal/result/result_msg/result_code/cmd_id/action_id）；UI 双数据源分区 |
| V4 | 重复上报幂等 | **PASS** | 固件一次性锁 + `log_history (device_id,boot_seq,seq)` 幂等（`ux_lh_dedup`） |
| V5 | 云端拒绝/断连不影响关阀 | **PASS** | 关阀是设备本地 GPIO 写，与云端状态无关（设计文档化） |
| V6 | 既有回归不破坏 | **PASS** | 全量 554/554（见 §3）；较上轮 413 全绿且增量 141 项 |
| V7 | 黄金样本解码正确 | **PASS** | 生产 id=8682：boot=83/seq=23323，OPEN_MS=60006 > LIMIT_MS=60000，超出 6 ms；12 真实样本 JS/Python 双实现交叉验证 |

### 1.4 告警数据路径深度验证（本轮新增）

`scripts/verify_alert_history_prod.py`（只读）：用与 UI `loadAlerts()` **同参数同链路**
（`decode=1&level=WARN&limit=200` + `before_id` 游标）往前挖 6 页 / 1200 条：
- 游标契约 PASS（`next_before_id` 单调递减、`has_more` 一致）；
- **挖到黄金样本 id=8682**（boot=83/seq=23323，60006 > 60000，超出 6 ms）PASS；
- 告警卡片字段齐备 PASS。

---

## 2. UI 开发进度（WP-B）

### 2.1 路线图位置

阶段 2（安全后端，上轮 418 PASS）✅ → **阶段 3（首页与基础控制）本轮主体完成** →
阶段 4（Workflow 编辑器，未做）→ 阶段 5（遥测/日志解码/审计，部分雏形）。

### 2.2 本轮完成（SPA 五页）

`guo-feeder-pagesdev/app.html` + `assets/js/gf-app.js` + `assets/js/gf-ui-core.js`
（纯逻辑与 DOM 分离，逻辑层可 Node 单测）：

| 页面 | 内容 | 关键纪律落实 |
|---|---|---|
| 首页 | 设备状态（在线启发式）/ 重量气候遥测 / 快捷操作 | 遥测缺失时**明说**"重量数据不可信"（HX711 #6），不静默省略 |
| 控制 | 阀门 / 振动 / 出粮 / 秤 / 安全（急停+解除锁存） | 开阀二次确认含设备+重量+超时阈值；DRV8825/HX711 未验证动作 toast 警示；振动 enable=false 预期被拒 |
| 流程 | workflow.list 只读 + 运行 + 停止（=急停） | **运行契约修正**（见 2.3）；明示"云端运行跳过第 0 步"；明示 workflow_stop 只改账本不关阀 |
| 记录 | **告警**（0x0508，游标翻页）/ 日志（解码后，游标翻页） | 告警与命令结果分区说明；降级行不丢 |
| 设备 | 信息 / 配置只读 / 诊断 | 配置修改未开放（会重启设备） |

### 2.3 本轮发现并修复的实现缺陷（均有测试/生产证据）

1. **workflow 运行协议错误**（原实现）：发 `action_id` 而 Worker 要求 `workflow_id`
   （400 invalid_workflow_id），且误用 action registry 作 target。
   修正：`workflow_id` 字符串 + `target{v,k}` 取自 **workflow.list 帧自身**
   （`registry_version` = `capability_get_workflow_version()` 同源，`stable_id` 逐项），
   不用 action registry；valid=false / 无 stable_id 的流程禁止运行。
2. **告警页窗口太浅**：0x0511 噪声（实测 1795 条）占据 WARN ⇒ 单页 100 条翻不到
   历史告警。修正：游标翻页 + 自陈扫描口径 + "加载更早"按钮（§1.4 生产验证）。
3. **分钟格式吞掉亚秒超出量**：60006 ms → "1 分 0 秒" 与阈值无视觉差别。
   修正：新增 `formatExcessMs` 在告警卡精示"超出 6 ms"。
4. **registry 漂移无恢复**：固件 registry 随模块启停重排（v 5↔6）。
   修正：动作命令终态 failed 且 `e=3`/version mismatch ⇒ 自动重拉 registry 并提示。

### 2.4 数据出口新增（WP-A/WP-B 共用）

`GET /api/history/frames?cmd_id=`（Worker + Pages 代理白名单双落地）：
`command_audit` 有意不存 result payload ⇒ 查询类命令的**结果值**只能从
`mqtt_messages` 读。返回 `raw_payload`（整帧原文，写入侧已脱敏；压缩 result 帧的
业务数据在顶层）+ `payload_json`（仅当帧带 `p` 时非空）+ status/object 等专用列。
UI `latestFramePayload` 已改为 **raw_payload 优先**（否则 registry/config_query
取值全为空——这是本轮对 D1 实际数据形状的实测发现）。

---

## 3. 测试分类（通过 / 失败 / 未验证）

### 3.1 通过（全量回归 6 套件 · Node · 假值无网络）

| 套件 | 结果 | 覆盖 |
|---|---|---|
| `tests/api/ui_security.test.mjs` | **273 PASS / 0 FAIL** | 会话/审计状态机/限流/目录/命令提交/Pages CSRF |
| `tests/pages/pages_auth.test.mjs` | **89 PASS / 0 FAIL** | 登录/限流/代理白名单/中间件 |
| `tests/pages/pages_webhook.test.mjs` | **22 PASS / 0 FAIL** | webhook 密钥/校验 |
| `tests/api/log_decode.test.mjs` | **29 PASS / 0 FAIL** | 码表生成幂等/12 真实样本交叉验证/构造边界/API 集成 |
| `tests/api/history_frames.test.mjs`（新） | **33 PASS / 0 FAIL** | frames 参数校验/SQL 形状/响应形状/Worker 路由/空结果 |
| `tests/pages/ui_app.test.mjs`（新） | **108 PASS / 0 FAIL** | gf-ui-core 12 组纯函数 + 页面结构 20 项 |
| **合计** | **554 PASS / 0 FAIL** | |

其中机器可检的核心断言：告警输出**不含命令终态字段**；UI 无令牌字面量；
workflow 发 `workflow_id`；raw_payload 优先取值；跳过第 0 步明示。

### 3.2 生产只读实测（通过）

| 脚本 | 结果 |
|---|---|
| `scripts/verify_frames_prod.py` | **13 PASS / 0 FAIL / 1 INFO**（Worker 直连 401/200/空 items；Pages 代理无 cookie 401 / 有 cookie 200 且与直连一致 / 非白名单 403；decode=1 告警路径 INFO） |
| `scripts/verify_alert_history_prod.py` | **4 PASS**（游标契约/挖到黄金样本/数值/字段） |
| 静态与访问保护 | 无会话 `/app.html` 302→login；有会话 308→`/app` 200；三个 JS 资产 200 |

### 3.3 未验证 / 受阻（如实报告）

| 项 | 状态 | 原因 |
|---|---|---|
| 真实浏览器 + 真机的 UI 端到端操作 | **NOT VERIFIED** | 生产 Pages 登录启用了 Turnstile（实测 `captcha_failed:true`），程序化无法登录；自动化只到代理层。**未绕过任何人机验证** |
| workflow 运行真机执行 | **NOT VERIFIED** | 协议按固件契约实现+单测，但未向真机发过真实运行命令（危险操作，需人工在场） |
| registry 漂移自恢复真机触发 | **NOT VERIFIED** | 单测覆盖逻辑，未制造真实漂移 |
| 告警页当前窗口（最近 100 条）无 0x0508 | **INFO** | 黄金样本在更早 id；翻页后可展示（§1.4 已验证可达） |

---

## 4. 提交与部署基线

### 4.1 提交

| 仓库 | 提交 | 内容 |
|---|---|---|
| Cloudflare_Assets | `de01284` | feat：frames 端点 + decode=1 + SPA 控制台 + 告警页 |
| Cloudflare_Assets | `9cf951b` | test：frames 33 项 + UI 108 项 + 生产验证脚本 |
| Cloudflare_Assets | `2f5071a` | feat：告警页游标翻页（已推送 `7faba04..2f5071a` → main） |
| 固件仓 | 本轮 | docs：WP-A 设计文档 + 本报告（**无任何固件代码改动**） |

> 未提交：`Cloudflare_Assets/scripts/enforce_watch.sh`（早前受控试验的监视脚本，
> 非本工作包产物，按"禁 `git add -A`"纪律留在工作区未动）。

### 4.2 部署（实际版本）

| 目标 | 版本 | 验证证据 |
|---|---|---|
| Worker `guo-feeder-api` | **`bae88be7-46c7-4644-abc2-bb50939be8a8`** | 含 `/api/history/frames` + `?decode=1`；生产 401/200 实测 |
| Pages `guo-feeder-pagesdev` | **`8615a81d`**（前序 `a6da1707`/`7ee2cac6`） | `Compiled Worker successfully` + `Uploading Functions bundle` 均在；SPA 资产 200 |

**运维注记**：`wrangler pages deploy` 必须 **在 `guo-feeder-pagesdev/` 目录内以 `.`
为参数**执行——从父目录用目录名参数时实测**静默跳过 Functions 编译**
（只上传静态文件，表现为 `/api/read/*` 全部回落到 index.html）。本轮已按正确方式部署。

### 4.3 回滚

- Worker：Cloudflare 后台回滚到上一 Version（frames/decode 为纯增量只读端点，回滚无数据影响）。
- Pages：回滚到上一 deployment。
- D1：**无任何迁移**（0008 之后未加 DDL）⇒ 无回滚动作。
- 固件：**无改动** ⇒ 无回滚动作。

---

## 5. 延期硬件清单（维持 BLOCKED）

| # | 项 | 影响 | 状态 |
|---|---|---|---|
| 1 | HX711 未连接 | `weight_valid` 假阳性（遗留 #6）；重量遥测不可信；WEIGHT_ZERO 不可用 | BLOCKED（需硬件） |
| 2 | DRV8825 损坏 | 步进/出粮未真机验证；相关动作 UI 带警示 | BLOCKED（需硬件） |
| 3 | GPIO 电气测量 | 需万用表 | BLOCKED（需仪表） |
| 4 | Pages 登录 Turnstile 真实验证 | 需真实浏览器人工过验证 | BLOCKED（需人工） |
| 5 | 生产 D1 不可用真实故障路径 | fail-closed 行为未在生产故障态实测 | BLOCKED |
| 6 | config_query 第二帧超时生产验证 | UI 显示"结果未知"路径未在生产触发 | BLOCKED |

---

## 6. 尚未达到整机验收的风险

1. **UI 渲染层零真机验证**：命令链路（UI→Worker→EMQX→设备→回帧→D1）有第三轮
   e2e 与本轮生产实测背书，但**浏览器内的实际渲染/交互未验**（Turnstile 阻挡自动化）。
   整机验收前必须人工登录走一遍：开阀（含二次确认）/ 关阀 / 急停 / 解除锁存 /
   告警页翻页 / 日志页翻页 / 设备页配置只读。
2. **workflow 运行未发过真实命令**：协议正确性靠契约+单测，运行行为（跳过第 0 步、
   终态回报）未实测；首跑须人工在场并确认机械安全。
3. **告警展示依赖 WARN 窗口**：0x0511 噪声量级下，很深的历史告警需要多页翻挖；
   若未来需要"告警订阅/推送"，须另立需求（本版明确不做）。
4. **registry 漂移自恢复未实测**：weight 模块启停会改变 registry 版本，
   自恢复路径（自动重拉）未在真机触发过。
5. **固件侧零改动的另一面**：WP-A 完全依赖既有固件行为（关阀先行 + 一次性锁），
   若未来固件重构 `valve_task`，需回归 V1/V2/V4 三项。

---

## 7. 结论

- WP-A：**完成**。零固件/零 DDL/零协议扩展；V1–V7 全 PASS；生产黄金样本可经
  UI 同链路复现。
- WP-B：**阶段 3 主体完成**（登录保护/命令交互/状态反馈/告警展示/错误处理均有
  测试与生产实测）；阶段 4（Workflow 编辑器）未做，按路线图留待下轮。
- 测试：554 单测 + 17 项生产只读实测全绿；3 类 NOT VERIFIED / 6 项 BLOCKED 如实列名。
- 下一步建议：① 人工登录完成 UI 真机走查（§6-1 清单）；② workflow 首跑人工在场；
  ③ 硬件恢复后补 HX711/DRV8825 验证；④ 阶段 4 开工前先立 workflow CRUD 的
  安全边界（save 必重启、running/dirty 拒绝）设计文档。
