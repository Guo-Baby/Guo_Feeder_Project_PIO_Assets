# Guo Feeder —— 阶段 2：UI 改进、EMQX 权威在线状态与请求优化（实施报告）

- 日期：2026-10-24（UTC 15:34–16:25 / 本地 23:34–00:25）
- 仓库：`Cloudflare_Assets`（`main`，HEAD `0e8bc7a`；切换前 `75b22b8`）
- 生产：Worker **`66d45848-5617-4853-ac6f-c5e3ccda2614`** @100% ·
  Pages **`897f151e`**（前一个 `22ed76c2`）
- 结论：**完成**（UI 浏览器实操验证缺失，见 §9.1）

---

## 1. 结果总览

| 阶段 2 条目 | 状态 |
|---|---|
| 移除无用冗余计数（遥测/日志/事件条数、表规模） | ✅ 已完成（阶段 1 收口，本阶段并入生产） |
| 传感器改手动刷新 + 加载态 + 失败反馈 | ✅ |
| 单位 / 时间戳 / 含义自陈（不把旧数据当刚采集） | ✅ 新增"数据新鲜度"卡 + 陈旧标注 |
| 设备页展示固件版本（缺失显式"未上报"） | ✅ |
| 快捷操作 0/1/2/4/8/12 可配置 | ✅ |
| 在线状态以 EMQX 权威状态为准 + 与新鲜度分离 | ✅（**探针推翻文档写法**，见 §2.1） |
| 减少命令轮询 / 页面切走即停 / 代际守卫 | ✅ |
| 帧去重按 `frame.id`（不按 cmd_id） | ✅ |
| 不因减查询丢终态/错误/超时 | ✅（终态落 `command_audit`，命令条明示） |
| 部署 Worker + Pages 并验证 | ✅（**UI 浏览器实操未验**，§9.1） |

---

## 2. 实际改动

### 2.1 API / Worker（3 文件 + 1 新增）

| 文件 | 变更 |
|---|---|
| `src/presence-emqx.js` | **新增**：`GET /api/admin/presence/emqx/:id`，只读、不落库、30 s isolate 缓存 + in-flight 合并 |
| `src/emqx-admin.js` | 新增 `queryClients({likeClientid, connState, limit, timeoutMs})` |
| `src/index.js` | `handleAdmin` 内新增 presence 路由（在 `/api/admin/device/` 前缀之前） |
| `functions/api/read/[[path]].js` | 只读代理白名单加 `^admin/presence/emqx/[0-9a-f]{12}$` |
| `src/read-api.js` | （阶段 1 前置）snapshot `counts` 显式 opt-in，默认不算 COUNT |

**★★ 最关键的一条：文档写法在本环境会把在线设备报成离线**

Rev.2 原计划写 `GET /clients?clientid=<精确值>`。部署前按计划做只读探针，
结果是：

```
GET /clients?limit=50                        → 200，2 条：['phone','dev_288485896ce4']   ← 设备在线
GET /clients?clientid=dev_288485896ce4       → 200，0 条                              ← 精确过滤不生效
GET /clients?like_clientid=dev_288485896ce4  → 200，1 条：['dev_288485896ce4']          ← 可用
GET /clients?username=GuoFeederDevice        → 200，1 条                               ← 可用但共享账号无区分力
```

⇒ 照文档写会**假离线**。最终实现为两段式：

1. `like_clientid=dev_<id>` 取前缀候选 → **本地精确比对 `clientid`**
   （前缀命中 ≠ 主会话在线：轮换残留 `dev_<id>_t<gen>` 不算）；
2. 未命中 → `conn_state=connected&limit=200` 无过滤交叉确认：
   未打满且无目标 ⇒ **offline**；打满 200 仍无目标 ⇒ **`unknown/filter_unverified`**（不猜）。

`unknown` 语义：EMQX 未配置 / 不可达 / 429 / 过滤无效 —— 一律 `state:"unknown"` + `reason`，
**永不**降级成 `offline`。

### 2.2 UI（`gf-app.js` / `gf-ui-core.js`）

- **两张正交卡**：`连接状态`（EMQX）+ `数据新鲜度`（快照时刻，`dataFreshness()`：
  2 分钟内 / N 分钟前 / 陈旧）。`presenceFromLastSeen()` 在 `gf-app` 里**零调用**。
- **快捷操作可配置**：档位 `0/1/2/4/8/12`（`QUICK_ACTION_COUNTS`），池子 12 项
  （开/关阀、急停、解除锁存、出粮、清零、查阀位、振动×2、诊断×3，全部复用既有处理函数）；
  选择按设备存 `localStorage: gf.quick_<deviceId>`；就地编辑器先选档位再勾选，
  勾满锁其余项、未勾满禁用保存并提示差几个；0 个显示引导不空塌；急停不在栏内会提示。
- **固件版本**：`d.fw_version || "未上报"` + 解释文案（"上行尚未携带 fw_version"）。
- **命令条同一 cmd_id 只占一行**（原地更新），不再每 1.5 s 插一条刷屏。
- **修复阶段 1 遗留回归**：`onValveOpen()` 原读 `S.lastSnapshot`，而首页改缓存驱动后
  该字段不再填充 ⇒ 开阀二次确认永远拿不到重量。改 `cachedSnapshot()`。

### 2.3 请求与生命周期

| 机制 | 实现 |
|---|---|
| 轮询退避 | `pollDelaySeq()` 纯函数：1.5 s → ×1.4 → 上限 3.5 s |
| 切页即停 | `onRoute()` 首行 `cancelPolling()`（`pollGen` 代际 + `activePolls` 集合） |
| 真离开 | `pagehide` → `cancelPolling()` |
| 后台标签页 | 隐藏时间隔 ×2（**不停掉**，保证切回来终态已就绪） |
| 帧请求合并 | `framesInflight` + 2 s 微缓存；帧按 **`frame.id`** 合并（`mergeFramesById`） |
| 连接状态合并 | `presenceInflight` + 30 s 缓存（`gf-cache`） |

**终态不丢的三重保障**：① 云端 `stateFromFrame` 单调推进并把结果写 `command_audit`；
② 取消时命令条明示"已停止 N 条轮询（结果见「记录 → 审计」）"；③ 审计页可用 cmd_id 精确检索。

---

## 3. 测试结果

### 3.1 根套件（`tests/`，11 套件）

| 套件 | PASS / FAIL |
|---|---|
| `presence_emqx`（**新增 E-1…E-11**） | 43 / 0 |
| `ui_app`（S-17 **新增 25 项**） | 194 / 0 |
| `ui_app_runtime`（T-17 **新增 38 项**） | 127 / 0 |
| `pages_auth`（P-2-9 / P-3-3 / P-3-4） | 107 / 0 |
| `ui_security` | 273 / 0 |
| `wfadmin` | 65 / 0 |
| `history_frames` | 33 / 0 |
| `log_decode` | 29 / 0 |
| `pages_webhook` | 22 / 0 |
| `history_command` | 18 / 0 |
| `snapshot_counts` | 11 / 0 |
| **合计** | **922 / 0** |

阶段 2 前基线为 813（10 套件）⇒ 净增 109 条断言，全部新增，零删除。

### 3.2 Worker 自有套件（`guo-feeder-api/test/run_all.mjs`，26 套件）

**1707 total / 1677 PASS / 21 FAIL / 9 EXPECTED_FAILURE** —— 与阶段 1 记录的基线
**逐套件完全一致**：

| 套件 | 失败数 | 定性 |
|---|---|---|
| `log_ack` | 2 | `T01f`/`T02e` 仍断言 ACK 发 `/down`（`LOG-ACK-SPLIT` 后已改 `/log/ack`） |
| `cloud_api_downlink_topic` | 17 | 测试未带令牌 ⇒ 401 |
| `retention_policy` | 1 | `scheduled()` 体 21 行 > 任务书 10 行（风格断言） |
| `s3_ingest_validate` | 1 | `S3-10-1b`，套件自述"既有行为" |

⇒ **本次零新增失败**。已用纯净 HEAD 工作树对同一 4 个套件做过对照（阶段 1 报告 §5.2）。

### 3.3 真实 EMQX 端到端（非桩）

本沙箱无法访问 `*.workers.dev`，改用**与生产相同的凭据**直接驱动
`presence-emqx.js` 的判定逻辑（同一份代码、同一个 EMQX）：

| 用例 | 结果 |
|---|---|
| 真实设备 `288485896ce4` | `online` + `connected_at=2026-10-24T09:44:00.615+00:00`（247 ms） |
| 二次调用 | `cached: true`（未重复打 EMQX） |
| 不存在的 `0badc0de0000` | `offline` |
| 无凭据 env | `unknown / config_missing` |

---

## 4. 生产部署

| 组件 | 部署前 | 部署后 | 时间 |
|---|---|---|---|
| Worker | `81ceda94-48cd-4651-94af-c0b5a6748635` | **`66d45848-5617-4853-ac6f-c5e3ccda2614`** | 2026-10-24T16:05:48Z |
| Pages | `b4aa1257-c39c-4013-845a-0d4e6157ba78` | **`22ed76c2`** → **`897f151e`** | 16:06Z / 16:20Z |

- 顺序：**先 Worker（纯新增端点）→ 再 Pages（才开始调用）** ⇒ 无新旧不兼容窗口。
- Worker 用 `versions upload`（0 流量）→ `versions view` 核对绑定（`guofeeder-v2`）→
  `versions deploy @100%`；生效版本从 Cloudflare 侧核实，不看本地 toml。
- Pages 在 `guo-feeder-pagesdev/` 内以 `.` 为参数部署，输出含
  **`Compiled Worker successfully` + `Uploading Functions bundle`** 双判据（避开已知坑）。
- 部署后 `gf-app.js` 生产哈希 `9978b708c7c2` = 本地哈希。

**生产验证**

| 项 | 结果 |
|---|---|
| `/api/read/admin/presence/emqx/<12hex>`（无会话） | **401**（鉴权优先，非 403/404/500） |
| `/api/read/admin/device/<12hex>/snapshot`（无会话） | 401（既有行为不变） |
| `/login.html` | 308（未登录正常跳转） |
| 设备遥测是否继续落 D1 | ✅ 34 行，最后一条 75 s 前；`log_history` 2 行 |
| D1 24h 用量（新库） | read_queries 101 / write_queries 117 · rows_read 1,241 / rows_written 402 |

---

## 5. 请求优化证据（可复现）

| # | 优化 | 证据（可复跑） |
|---|---|---|
| 1 | 命令轮询退避 | `pollDelaySeq(75000)` **长度 = 22**，原固定 1500 ms = **50 次** ⇒ **−56%**（T-17-1/2/3/4 断言长度、区间、单调不减） |
| 2 | 切页即停 | T-17-34：轮询进行中切页后 `pollCount ≤ 3`（不取消会跑到 50+），命令条明示"已离开页面…见审计" |
| 3 | 连接状态缓存 + 合并 | T-17-37：首页→设备页→首页→设备页（4 次渲染，含双触发）⇒ presence 端点 **1 次**；T-17-38：点「刷新连接状态」⇒ 恰好 **+1 次** |
| 4 | 帧请求合并 | 结构断言 S-17-22（`framesInflight` + `FRAMES_TTL_MS`）；行为断言 T-17-5/6/7（同 cmd_id 的 ACK+result 都保留、按 frame.id 去重） |
| 5 | 快照不再 COUNT | （阶段 1）S-16-1/2/3 + T-16-1/2；本阶段未改动该路径 |

> 未测量项不宣称收益：帧合并的**次数下降**没有独立计量（当前每条命令只读一次帧，
> 合并是为"并发/重复读"场景兜底），此处只声明"不会丢帧"这一正确性结论。

---

## 6. Git 状态

| 项 | 值 |
|---|---|
| 分支 | `main` |
| 本次提交 | **`e5bd26f`**（阶段 2 主体 + 阶段 1 前置整合，16 文件 +2685 −122）<br>**`0e8bc7a`**（presence in-flight 合并 + 2 条实测断言，3 文件 +66 −8） |
| 提交前 HEAD | `75b22b8` |
| 是否 push | **否**（未推送；`origin/main` 落后 2 个提交 ⇒ 现落后 4 个） |
| 工作区剩余 | 仅 `?? scripts/enforce_watch.sh`（与本阶段无关的本地守护脚本，**有意保持未跟踪**） |

> 阶段 1 的改动为何并入本提交：`gf-app.js` 是同一个文件，且该文件已随本次 Pages 部署上线
> ⇒ 不入库会造成"生产跑的代码不在仓库里"。评审记录见
> `GuoFeeder-Optimization-Phase1-PreDeploy-Review-2026-10-24.md`。

---

## 7. 数据库与安全

| 项 | 状态 |
|---|---|
| 旧库 `guofeeder`（`0d8385ba…`） | **未改动**：未删、未清空、未写入 |
| Worker D1 绑定 | `guofeeder_DB` → `632b1224…`（`guofeeder-v2`），部署后 `versions view` 核实 |
| 新增迁移 | **无**（本阶段零 DDL，`0001`–`0009` 一字未动） |
| 密钥 / Turnstile / 安全设置 | **零变更**（presentse 复用既有 `EMQX_APP_ID/SECRET`，未新增凭据） |
| 新增写路径 | **无**：presence 只读 EMQX，不写 D1（S-17-6 断言源码无 INSERT/UPDATE/DELETE） |
| 权限模型 | 沿用 `ADMIN_API_TOKEN`；浏览器不直连 Worker（Pages 注入 Bearer）；代理白名单精确到 12-hex |

---

## 8. 回滚方案

**Worker**（秒级，回到阶段 1 版本）

```bash
node node_modules/wrangler/bin/wrangler.js versions deploy \
  81ceda94-48cd-4651-94af-c0b5a6748635@100% \
  --config guo-feeder-api/wrangler.toml --yes
```

**Pages**（回到阶段 1 部署 `b4aa1257`）

```bash
# 方式 A：Dashboard → Pages → guo-feeder-pagesdev → Deployments → b4aa1257 → Rollback
# 方式 B：在 checkout 到 75b22b8 的树里重新部署
cd guo-feeder-pagesdev && node ../node_modules/wrangler/bin/wrangler.js pages deploy . \
  --project-name guo-feeder-pagesdev
```

> ⚠️ 回滚 Pages 到 `b4aa1257` 会同时**回退阶段 1 的 UI 改造**（缓存 + 手动刷新 +
> 条数移除）⇒ D1 读会回升。若只想撤掉阶段 2 的某个点，优先用 Worker 侧回滚
> （0 影响 UI）或直接改具体分支后重新部署 Pages。

**已验证 / 未验证**

- ✅ `versions deploy <旧版本>@100%` 机制本身在本会话中已多次使用成功。
- ⚠️ 回滚到 `81ceda94` / `b4aa1257` 的**具体执行未实跑**（避免无谓抖动）。

---

## 9. 遗留风险

### 9.1 【未验证，需你过一眼】浏览器内 UI 实操

本沙箱无法登录生产 UI（`*.workers.dev` 不可达；`pages.dev` 可达但登录需 Turnstile
浏览器令牌；Python 直连 `/api/auth/login` 被 Cloudflare 边缘拦 `403 code 1010`）。
因此**以下只做了代码级 + 桩级验证，没有真人点击过**：

- 首页两张卡的真实排版（375 px 是否挤压）
- 快捷操作编辑器的勾选/禁用交互手感
- 命令条"已离开页面"提示与审计页跳转的实际观感

建议你登录后按 30 秒清单过一遍：① 首页看"连接状态=在线"与"数据新鲜度"是否分开显示；
② 点「编辑快捷操作」改成 2 个 → 保存 → 刷新页面确认持久化；③ 点「刷新连接状态」看是否秒回；
④ 发一条关阀命令后立刻切到别的页 → 命令条应出现"已离开页面…见审计"。

### 9.2 EMQX 过滤是"两段确认"，离线路径多一次请求

为使"离线"可信，离线时要打 2 次 EMQX（前缀 + 无过滤兜底）。在线只打 1 次。
30 s 缓存把总体频率压到 ≤2 次/30 s。若将来 EMQX 支持 `clientid=` 精确过滤，
可去掉兜底（`presence-emqx.js::probe()` 已把这条写成可删的第二段）。

### 9.3 单设备假设

`ensureDeviceId()` 仍取"最近日志里的第一个 device_id"，页面无设备切换器。
`presenceInflight` / 缓存键都按 deviceId 隔离，切设备逻辑本身是对的，但入口尚未做。

### 9.4 未做（明确排除，不在本阶段）

- 首页模块排序（需求里有，但本次任务书 §3 未列入 ⇒ 未做，避免扩大范围）
- 浏览器直连 MQTT（Rev.2 阶段 4）
- 日志写入治理 / watermark 降频（阶段 3）
- `retention_policy` 等 4 个既有失败套件的修复（与本阶段无关）

---

## 附录：本次改动的 12 条自检

1. 未改 `0001`–`0009` 任何迁移 ✅
2. 未新增迁移 ✅
3. 未动 EMQX 认证/ACL/规则/Action/Connector ✅
4. 未改设备端协议 / 未刷固件 ✅
5. 未削弱 ACK / 重试 / 去重 / `log_conflict` / rewind ✅（Worker 仅新增只读端点）
6. 未把控制命令迁到浏览器直发 MQTT ✅
7. 未用"HTTP 200"代替业务成功判断 ✅
8. 未新增写路径（无 D1 写入） ✅
9. 未覆盖用户既有未提交改动（阶段 1 是**整合**不是覆盖） ✅
10. 未 `git add -A`；`enforce_watch.sh` 保持未跟踪 ✅
11. 未 push、未强推 ✅
12. 历史失败与新增失败已分开陈述（§3.2） ✅
