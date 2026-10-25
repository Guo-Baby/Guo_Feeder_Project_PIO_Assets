# UI 性能与 D1 配额全面审计（只读 · 2026-10-25）

> 性质：**只读审计**，未修改任何产品代码/数据库/生产环境/设备。
> 基线：固件仓 `wb@3cdd216`；云仓 `main@8e8d3e0`（工作区除本审计新增文档外干净）。
> 配套交付：`18-UI-Completion-and-Performance-Plan.md`（差距矩阵 + 实施计划）。

**证据等级标注**：`[代码]`=当前源码确认；`[测试]`=现有测试确认；`[生产]`=历史生产证据；`[推断]`=合理推断；`[未验]`=尚未验证。

---

## 1. 全链路请求图

浏览器 → Pages Function（会话校验 + 白名单代理）→ Worker（业务处理）→ D1 / EMQX / 设备。

```
[浏览器 app.html + gf-app.js]
   │
   ├─ HTML 页面访问 ──→ Pages _middleware.js（只拦 /app.html /app / 三条路径，
   │                      readSessionFull；API 路径不过中间件）[代码]
   │
   ├─ /api/read/*（只读）──→ functions/api/read/[[path]].js
   │      ① readSessionFull() → store.verify() → POST Worker /api/ui/session
   │           → verifySession(touch=true) → D1: SELECT ui_session + UPDATE last_seen_at
   │      ② 携 ADMIN_API_TOKEN 转发 Worker 只读端点 → 业务 D1 SELECT
   │      [代码] 白名单精确匹配：telemetry/log/frames/metrics/snapshot
   │
   ├─ POST /api/command ──→ functions/api/command.js
   │      ① readSessionFull()（同上 1 SELECT + 1 UPDATE）
   │      ② 转发 Worker /api/ui/command → 校验/目录/audit INSERT → MQTT 发布
   │
   ├─ GET /api/command/:id（轮询）──→ functions/api/command/[id].js
   │      ① readSessionFull()（同上）
   │      ② 转发 Worker getCommandStatus → D1: SELECT command_audit
   │
   └─ 设备/EMQX 侧（非 UI）──→ Worker /api/webhook、/api/history/ingest
          INSERT telemetry_history/log_history/mqtt_messages
          + markCommandState（getCommand SELECT + audit UPDATE）
```

**关键结构事实 [代码]**：
1. Pages Functions 与 Worker 是**两次独立调用**：每个浏览器请求 = 2 次 Worker invocation + Pages Function 1 次。
2. `readSessionFull`（形态 A 可撤销会话）在 **每个** `/api/read/*`、`POST /api/command`、`GET /api/command/:id` 上都执行；中间件只拦 HTML 页面，不构成双重校验 [代码]。
3. `verifySession` 的 `touch` 分支执行 `UPDATE ui_session SET last_seen_at=?`，**无任何节流**；而 Pages 的 `store.verify` 提交 `{action:"verify",sid,secret}` 不带 touch 字段，Worker 侧 `touch: body.touch !== false` ⇒ **恒为 true** [代码，ui-session.js:243-261、index.js:1757-1760、session-store.js:85-99]。
4. `verifySession` 源码注释明示：`last_seen_at 只做观测，不参与任何鉴权判据` [代码]。

---

## 2. 请求与 D1 操作清单

### 2.1 逐项清单（UI 侧）

| # | 页面/功能 | 触发 | 浏览器入口 [代码] | HTTP | Worker 处理 | D1 影响 | 非 D1 | 现有缓存 | 重复风险 | 配额影响（估算） | 证据 |
|---|---|---|---|---|---|---|---|---|---|---|---|
| R1 | 全部 API 请求 | 每请求 | gf-api.js `request()` | 各 | /api/ui/session | **1 SELECT + 1 UPDATE**（touch 无节流） | — | 无 | 无重复 | **每请求 2 ops**；读小、**写恒定 1** | 代码 |
| R2 | 页面访问（app 页） | 每次导航 | _middleware.js | GET HTML | — | 1 SELECT + 1 UPDATE（同 R1） | — | 浏览器缓存 HTML | 低 | 每次导航 2 ops | 代码 |
| R3 | 首页快照 | 会话首次 + 手动刷新 | renderHome → apiGet | GET /api/read/admin/device/:id/snapshot?recent=5 | handleDeviceSnapshot | device SELECT + 3 per-device COUNT(*) + 3 recent SELECT | — | 内存（仅会话） | 低 | **COUNT 随表增长线性涨**（telemetry 694+、log 数千行 ⇒ 每次数千行 read） | 代码 |
| R4 | 设备 ID 冷启动推断 | 仅 localStorage 无设备时 | ensureDeviceId | GET /api/read/history/log?limit=50 | handleHistoryLog | ~50 行 read | — | LS_DEVICE 持久 | 低 | 一次性 ~50 行 | 代码 |
| R5 | registry 拉取 | 首次控制操作；e=3 漂移重拉 | loadRegistry → runCommand | POST /api/command + 轮询 + frames | query registry_action | 见 R11 命令通用成本 | 1 MQTT 下行 | **registry 版本戳缓存**（命中 0 请求） | e=3 自动重拉（正确性兜底） | 命中时 0；未命中 ≈ 1 条命令 | 代码+测试 |
| R6 | 控制页阀门状态查询 | 点击 | onControl → runCommand(query query_state) | 同 R11 | query_state | 同 R11 | 1 MQTT 下行 | 无（状态实时性要求） | — | 1 条命令成本 | 代码 |
| R7 | 流程列表 | 进流程页 | renderWorkflows → runCommand(workflow.list) | 同 R11 | workflow.list | 同 R11 | 1 MQTT 下行 | **wf_list：registry_version + dirty=false 才缓存** | 写命令后失效（正确） | 命中 0；未命中 1 条命令 | 代码+测试 |
| R8 | 流程详情（编辑） | 点击编辑 | openWfEditor → runCommand(workflow.get) | 同 R11 | workflow.get | 同 R11 | 1 MQTT 下行 | **wf_detail_<slot>：slot+variant 键** | wfadmin 失效 | 命中 0 | 代码+测试 |
| R9 | 动作/触发器描述符 | 编辑器打开 | loadWfDescriptors → runCommand(query_actions/triggers) | 同 R11 ×2 | query_actions/query_triggers | 同 R11 ×2 | 2 MQTT 下行 | **wf_descriptors：registry 版本键** | e=3 失效 | 命中 0；未命中 2 条命令 | 代码+测试 |
| R10 | 模块配置读取 | 进设备页（缓存未命中） | loadDeviceConfig → 7× runCommand(config_query keys_only) | 同 R11 ×7（+轮询×7） | config_query | 同 R11 ×7 | **7 次 MQTT 下行** | **config_all：仅时间戳 + 手动刷新/写失效** | 无 | 未命中 ≈ 7 条命令 ≈ 7×~30 ops ≈ 200+ ops | 代码 |
| R11 | **命令通用成本** | 每条 runCommand | runCommand | POST + **轮询 GET×N**（1.5s 间隔，最长 75s ⇒ **最多 ~50 次**）+ query/wfadmin/system 族再取 frames×1 | audit INSERT + getCommandStatus SELECT×N + frames SELECT | INSERT 1 写；每轮询 1 读；回帧另触发 ingest 写（见 R15） | 1 次 MQTT 发布 + 回帧 | 无（状态不可缓存——正确） | 轮询失败 continue 重试（有 75s 上限） | **一条慢命令 ≈ 50×(1 读) + 52×(1 读+1 写) ≈ 150+ ops**；快命令（~3s 终态）≈ 2 轮询 ≈ 10 ops | 代码 |
| R12 | 日志页加载/翻页 | 进记录页 + 点更多 | loadLogs | GET /api/read/history/log?decode=1&limit=20 | handleHistoryLog | ~21 行 read/次 | — | **log_recent：id 去重增量，7 天 TTL，400 条上限** | 翻页游标推进 | 每次 ~21 行 + R1 | 代码+测试 |
| R13 | 告警页扫描 | 进告警 tab + 翻页 | loadAlerts | GET /api/read/history/log?decode=1&limit=… | 同上 | ~N 行 read/次 | — | 无（游标翻页本身即增量） | — | 同 R12 | 代码 |
| R14 | 遥测曲线 | 进设备页 + 手动刷新 | refreshTelCharts ×3 | GET /api/read/history/telemetry?type=…&limit=200 | handleHistoryTelemetry | ~201 行 read ×3 | — | 无（本会话内存） | 低 | 600 行/次刷新 | 代码 |
| R15 | **admin/metrics（诊断）** | **每次进设备页，无条件** | renderDevice → apiGet("/api/read/admin/metrics") | GET /api/read/admin/metrics | handleMetrics | **5 个全表 COUNT(*)**（mqtt_messages / telemetry_history / log_history / device / device_event） | — | **无** | 无 | **⚠ 最大单点：log_history 若 5 万行 ⇒ 一次请求 ~5 万+行 read** | 代码 |
| R16 | 审计检索 | 记录页审计 tab | loadAudit | GET /api/read/history/command?cmd_id=…&state=… | handleHistoryCommand | ~101 行 read | — | 无（按需查询） | — | 每次检索 ~100 行 | 代码 |
| R17 | 登录/登出/续期 | 显式 | login.html / logout | POST /api/auth/* | issue/revoke | 1 INSERT 或 1 SELECT+1 UPDATE | — | — | — | 低频 | 代码 |
| R18 | watchRestart 回归监视 | 重启命令后 | 设备页 setInterval 1.5s? [见 2.3] | snapshot 轮询 | handleDeviceSnapshot | 每轮 = R3 成本 | — | 有超时终止 | 已切页即停 | 重启窗口内 ~100s × R3 | 代码 |

### 2.2 非 UI 消耗源（不受 B1 影响）

| 源 | D1 影响 | 证据 |
|---|---|---|
| 设备日志上行 + ACK（停等协议） | 1 msg = 1 log_history INSERT（写）+ 1 log_ack 下行；46.7% 放大是设计代价 | 生产+代码 |
| 设备遥测上行 | telemetry_history INSERT/行（写） | 代码 |
| 设备命令回帧（ack/result） | mqtt_messages INSERT（写）+ markCommandState：getCommand SELECT（读）+ audit UPDATE（写）；restart 族 done 映射再多 1 次 SELECT | 代码 |
| history/ingest 批量 | INSERT OR IGNORE；唯一索引冲突检查消耗 rows_read [推断，口径待核实] | 代码 |
| retention cron（每日 3 点） | 批量 DELETE（写）；telemetry 90 天 / log 180 天 | 代码 |
| 本轮之前的验证脚本 | 曾高频轮询（Q2 待治理） | 生产 |

### 2.3 待核实项

- [未验] R18 watchRestart 的轮询间隔与单轮成本（代码在设备页 setInterval，具体间隔与是否复用 snapshot 未逐一核对本轮；列为本审计未覆盖点，实施 Q 阶段前须先读码确认）。
- [未验] D1 计费口径：`rows_read` 对 COUNT(*)/索引冲突检查的精确计数规则（以 Cloudflare 官方计费文档为准）；本报告按"扫描行数=读"口径估算。
- [未验] 多标签页同时打开时 R1 的并发倍增（无共享去重）。

---

## 3. D1 成本模型（按典型会话估算，**标注为估算**）

> ⚠️ **2026-10-24 官方数据修正（必读）**：本节为**代码推断的估算**，已被 Cloudflare GraphQL 官方数据部分推翻。
> 10-23 UTC 实测：`UPDATE ui_session` 仅 **871 次/871 行**（占当日写 1%），`SELECT ui_session` 789 次；
> 真实写主因是**设备日志链路**（`log_history` INSERT 38,046 行 + `log_boot_watermark` upsert 32,142 行 = 92%）；
> 读主因是**分页查询缺覆盖索引**（4 条查询占 84% 读）。
> 权威结论见 `docs/review/GuoFeeder-Full-Optimization-Investigation-and-Plan-2026-10-24.md` §3。
> 本节保留原估算以留痕，**不得作为决策依据**。

假设一轮"测试会话"= 进首页 + 设备页 ×3 + 控制 5 条命令（平均 10 轮询）+ 流程页浏览 ×2 + 日志翻页 ×5。

| 消耗源 | 次数 | 读（行） | 写（行） |
|---|---|---|---|
| R1 会话 SELECT+UPDATE | ~120 请求 | ~120 | **~120** |
| R15 metrics 全表 COUNT | 3 | **3×全表行数（若 log 5 万 ⇒ ~15 万）** | 0 |
| R11 命令轮询 audit SELECT | ~50 | ~50 | 0 |
| R11 命令 audit INSERT + 回帧写 | 5 | ~10 | ~15 |
| R3/R18 snapshot COUNT | ~5 | 数千~1 万 | 0 |
| R12/R14 业务读 | ~15 | ~1500 | 0 |
| **合计（不含 metrics 全表）** | | ~2000 | ~140 |
| **合计（含 metrics，log 5 万行假设）** | | **~17 万** | ~140 |

**结论（按影响排序）**：
1. **R15 metrics 全表 COUNT —— 最大读消耗源** [代码+推断]。与请求次数无关，与表规模成正比；这是"随便点点就 50 万读"的主要嫌疑。
2. **R1 会话 touch UPDATE —— 最大写消耗源** [代码]。每个请求恒定 1 写，且**纯观测用途**（源码自证不参与鉴权）——零安全代价可治。
3. **R11 命令轮询（1.5s×75s）** —— 慢命令（如 config_query 7 连发、重启族）把"会话成本 ×50"。
4. **R10 config 7 连查** —— B1 缓存已挡住重复进页，但首次/失效后仍 7 条命令。
5. R3 snapshot 的 per-device COUNT —— 随表增长。
6. 设备侧上行写（遥测+日志）—— 固有成本，独立议题（Q3）。

**B1 缓存实际减少了什么 / 没减少什么** [代码]：
- ✅ 减少：registry/wf_list/wf_detail/wf_descriptors/config_all 的重复**命令**（= MQTT 下行 + 轮询 + audit 读）；日志翻页回源量（20/页 + 本地增量）。
- ❌ 没减少：**每个 API 请求仍 1 SELECT + 1 UPDATE 的会话成本**（B1 只减少请求条数）；**R15 metrics 全表 COUNT**（无缓存且每次进设备页必发）；snapshot COUNT；设备侧固有写。

---

## 4. 缓存实现评估（gf-cache.js + 调用点）

| 项 | 键/版本 | 评估 [代码] |
|---|---|---|
| registry | `gf_cache_<dev>_registry`，v=registry 版本 | ✅ 正确；e=3 漂移自动失效重拉（正确性兜底） |
| wf_list | v=registry_version 且仅 dirty=false | ✅ 正确；写命令失效钩子到位 |
| wf_detail_<slot> | v=variant | ✅ 正确 |
| wf_descriptors | v=registry 版本 | ✅ 正确 |
| config_all | v=null 仅时间戳 + 手动刷新 + 写失效 | ⚠ 可用但**首次/失效后仍 7 连查**（B3 优化空间：固件支持省略 module=全量单查 [代码]） |
| log_recent | 独立键，id 去重，7 天 TTL，400 上限 | ✅ 设计正确（日志不可变历史） |
| 快照/遥测 | 仅内存（S.lastSnapshot / telData） | ⚠ 刷新页面即回源；可低 TTL 持久化（低优先级） |

**跨设备隔离**：所有键含 device_id ✅ [代码]。**敏感信息**：缓存仅业务数据，无 token/凭据 ✅ [代码]。**多标签页**：无跨 tab 失效广播——A 标签发写命令，B 标签缓存不失效 ⇒ **存在陈旧读风险**（低危：业务数据非安全状态；列计划）。**存储损坏/满**：静默降级 no-op ✅。**版本戳获取失败**：cacheSet(v=null) 退化为时间戳语义，不会缓存错误版本 ✅。**失效风暴**：重启族全失效后，下次进各页各自回源（非同时），可接受 [推断]。

---

## 5. Q1（Worker isolate 会话缓存 60s）专项评估

**结论：不按原样实施。推荐替代方案 Q1'（touch 节流 + 轮询退避），Q1 本身列为待用户决策。**

技术分析：

1. **isolate 语义**：Workers 每个请求可能落到不同 isolate；无全局内存。60s isolate 内缓存的实际命中率 = 同 isolate 连续请求比例，**不可控、不可观测**（无内置指标）[推断，官方模型]。
2. **撤销窗口**：注销/封禁后，其他 isolate 的缓存副本最长存活 TTL（60s）。撤销语义从"立即"退化为"≤60s 延迟"——**安全语义弱化**。当前系统对撤销的要求：admin-auth 管理端点 + UI 全部端点 fail-closed（store_error ⇒ 拒绝）[代码]。
3. **fail-closed 风险**：若缓存实现为"命中即放行"，则 D1 故障期间命中者放行（D1 fail 时恰好绕过 fail-closed）⇒ 必须实现为"D1 错误时不写入缓存且命中项也缩短 TTL"——复杂度高、难测试。
4. **收益上限**：只省 R1 的 1 SELECT/请求；**不省 1 UPDATE**（touch 另算）。R15 metrics 不受益。
5. **替代方案对比**：

| 方案 | 读节省 | 写节省 | 安全影响 | 复杂度 |
|---|---|---|---|---|
| Q1 isolate 60s 会话缓存 | 中（命中部分） | 0 | **撤销延迟 ≤60s，fail-closed 弱化** | 高 |
| **Q1' a：touch 节流**（SELECT 顺带取 last_seen_at，>60s 才 UPDATE） | 0 | **写 -95%+**（1 写/请求 → ≤1 写/分钟/会话） | **零**（源码自证 touch 纯观测） | **极低** |
| **Q1' b：轮询退避**（1.5s→2s/3s/4s 递增，75s 上限不变） | 轮询请求数 -40~60% | 同比例 | 无（终态判据不变） | 低 |
| Q1' c：Pages Function 层会话短缓存（内存 5s） | 每请求组去重 | 少量 | 撤销延迟 ≤5s（可接受） | 中 |

6. **推荐组合**：Q1'a + Q1'b 先行（零安全代价、合计削减最大两块成本）；Q1 本体在配额仍紧张时再议，且必须先补撤销语义的形式化说明与测试。

---

## 6. 其他低成本优化盘点

| 项 | 现状 [代码] | 可行性 |
|---|---|---|
| B3 全量配置单查 | 固件 `config_query` **省略 module = 查全部模块**（command_manager.cpp:3103） | ✅ 真实存在，UI 改 1 次调用 |
| metrics COUNT 优化 | 改 `MAX(id)` 近似 / 加 5s isolate 缓存 / 移出设备页改为运维端点 | ✅ 三选一 |
| snapshot COUNT 优化 | per-device COUNT 改 MAX(id) 或缓存 | ✅ |
| 轮询终止条件 | 75s 上限 + terminal/timeout 已有 ✅；缺退避 | ✅ 补退避 |
| EMQX 在线状态（B2/P4） | emqx-admin.js 已持 Management API 凭据（建用户/ACL 在用）[代码] | ✅ 加 GET /clients 代理 + 限频缓存即可 |
| UI 在线口径 | presenceFromLastSeen（日志/上报推断）——用户已认可这不是唯一依据，P4 补 EMQX 权威口径 | ✅ |
| CDN/ETag | 动态业务数据不适合；静态资源已由 Pages CDN 覆盖 | 不需要 |
| D1 索引 | cmd_id/device_id 查询均有唯一索引/索引（0008 迁移）[代码] | 无明显低效；COUNT 类是模式问题非索引问题 |
| 生产脚本控频（Q2） | verify_*.py 曾按秒级轮询 | 列入计划：预算制 + 单次合并 |
| 设备日志放大（Q3） | 1 msg = 1 record + 1 ack（46.7% 设计代价）| 固件议题，独立决策 |

---

## 7. 风险与待验证事项汇总

1. [未验] R18 watchRestart 轮询参数（实施前读码确认）。
2. [未验] D1 对 COUNT(*)/唯一索引冲突检查的 rows_read 计费口径（官方文档核实后修订 §3 估算）。
3. [未验] 以下优化实施后需生产验证（等配额恢复）：Q1' 组合的实际削减比、B3 单查、metrics 改造——**最小验证方案见计划文档 §验收**。
4. [推断] 多标签页缓存陈旧（低危）。
5. 无重大安全问题发现；R1 touch 写放大为资源问题而非安全漏洞（touch 不参与鉴权 [代码]）。
