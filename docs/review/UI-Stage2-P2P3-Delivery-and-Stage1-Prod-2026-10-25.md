# Guo Feeder · 阶段一生产验收 + 阶段二（P2 + P3）交付报告

> 日期：2026-10-25 · 云仓 `Cloudflare_Assets`（提交见 §7）· 文档仓 `PIO_Assets/Guo_Feeder_Project`
> 上游：《阶段一部署收尾 + 阶段二实施任务书》（D-1～D-11 + 五阶段）
> 计划基线：`docs/review/UI-V11-Gap-Audit-and-Implementation-Plan-2026-10-25.md`（§8 唯一进度表）
> **阶段二代码未部署**；阶段三未开始。

---

## 0. 结论摘要

| 部分 | 结果 |
|---|---|
| 阶段一部署 | ✅ Worker `6bf731d4` + Pages `497f4672`；差异仅阶段一 10 个文件；零 migration / 零 EMQX 变更 |
| 阶段一生产只读验收 | ✅ Worker 侧 **20/20 PASS**；日志 `code` 派生**已生效**（新 2 行 code=2305/中文 message）；存量过滤有效；⚠️ 会话级页面 **BLOCKED**（Turnstile 真人验证，不绕过） |
| 阶段二实施 | ✅ P2-1/2/3/4/5 + P3-3/4/5/6 全部落地；四态/未知态/日志/同步等阶段一能力保持不变 |
| 测试 | ✅ 中央 **23 套件 1414 断言 0 FAIL**；真实 Chromium **4 套件 130 断言 0 FAIL**（stage1 47 / stage2 38 / stage5 22 / mqtt 23）；api 仓 run_all 25/26（既有 `S3-10-1b` 保留） |
| 部署状态 | 阶段二 **未部署**；等待单独授权 |

---

## 1. 部署前检查（实际执行）

| 项 | 结果 |
|---|---|
| 云仓 | `main` @ `24f87f2`，工作区干净 |
| 文档仓 | `wb` @ `724888f`（工作区有**历史遗留**的格式/证据文件未提交，非本轮产生，未纳入本次提交） |
| Pages 目录/构建 | `guo-feeder-pagesdev/`（`wrangler pages deploy .`，含 Functions 构建） |
| Worker 目录 | `guo-feeder-api/`（`wrangler.toml`：`guo-feeder-api` / D1 `guofeeder-v2` / 双 cron） |
| 部署差异 | `23d4034..24f87f2`：Pages 5 文件（app.html、gf-app、gf-mqtt、gf-ui-core、read/[[path]]）+ Worker 5 文件（log-i18n/log-filter/log-decode/read-api/history-ingest）；**migrations 零改动** |
| 回滚目标（已核实） | Worker 上一版本 `2690a54b`（部署 `8b1c2335`，2026-10-25T09:14Z）· Pages `8841f9ca`（源 `23d4034`） |
| 部署前测试 | 中央 21 套件 1355 PASS / api run_all 25/26（既有失败保留） |

**部署顺序**：Worker 先（后端先支持新过滤参数）→ Pages 后（前端 + 白名单）。

---

## 2. 部署执行

| 步骤 | 命令/结果 |
|---|---|
| Worker | `wrangler deploy --config guo-feeder-api/wrangler.toml` ⇒ `Current Version ID: 6bf731d4-2140-44cf-917a-4f81ba77f683`；cron `0 3 * * *` / `5 * * * *` 保留 |
| Pages | `cd guo-feeder-pagesdev && wrangler pages deploy . --project-name guo-feeder-pagesdev` ⇒ `Compiled Worker successfully`，部署 `497f4672` |
| 产物核验 | 线上 `gf-app.js` 含阶段一新符号（`upsertCommandCard` 等 17 处匹配） |
| 未执行 | ❌ D1 migration、❌ EMQX 变更、❌ 向设备发命令 |

---

## 3. 生产只读验收

### 3.1 Worker 侧（`scripts/verify_stage1_prod.mjs` · 20/20 PASS）

| 组 | 断言要点 | 结果 |
|---|---|---|
| P-1 | 无过滤参数 ⇒ `filter:null`、`items/has_more` 照旧 | PASS |
| P-2 | `include_noise=0` ⇒ 返回行**逐条解码后无噪声**；`scanned=84`（命中 3 条） | PASS |
| P-3 | `code=0x0508` ⇒ 命中行确为 0x0508（存量 `code` 仍 NULL 也能筛） | PASS |
| P-4 | `module=water` ⇒ 全为 0x05xx | PASS |
| P-5 | `category=alert` ⇒ 全 alert（0x0511 归噪声被排除） | PASS |
| P-6 | 非法 `code/module/include_noise` ⇒ 400 | PASS |
| P-7 | 扫描口径：`scanned≤3000`、`capped/exhausted` 有明确值（本次 scanned=84、capped=false、919ms） | PASS |
| P-8 | 鉴权未削弱：Worker 无 token ⇒ 401；Pages 只读代理无会话 ⇒ 401；`admin/revoke` ⇒ 401/403 | PASS |

### 3.2 日志 `code` 派生（生产写侧修复）

- 部署前：`log_history` 84/84 `code IS NULL`。
- 部署后（22:10–22:30 观察窗口，**未人为制造事件**）：表内 86 行，其中 **2 行 `code` 非空**：

| id | level | code | message | created_at |
|---|---|---|---|---|
| 107 | INFO | **2305**（0x0901） | 温湿度计数据解析成功 | 1792938554 |
| 105 | INFO | **2305**（0x0901） | 温湿度计数据解析成功 | 1792937954 |

- `payload` 仍是原始 base64（`jAICGGIZx2wZ...`）⇒ 溯源与 `log-ack` 语义未变。
- **判定：写侧修复在生产真实生效**（不是本地推断）。

### 3.3 R1 审计页（会话级）

| 检查 | 结果 |
|---|---|
| 部署已含白名单修复 | ✅（部署差异含 `read/[[path]].js`，`/^history\/command$/`） |
| 未鉴权访问 `/api/read/history/command` | 401（**鉴权未被削弱**，符合"会话 → 白名单"顺序） |
| 真实登录后点「记录 → 审计」 | ⚠️ **BLOCKED**：生产登录启用 Turnstile（`captcha_failed:true`，无 token 401），自动化环境无法完成真人验证；**我没有也没有尝试绕过** |

> **需要人工 60 秒确认**：浏览器登录 → 「记录 → 审计」→ 应出现表格（含命令 ID/风险/状态列）。

### 3.4 阶段一 UI 生产冒烟

- BLOCKED（同上：页面需会话）。**已用真实 Chromium + 真实前端资源覆盖**（`stage1_ui_playwright` 47 断言，含在线状态/connected_at/离线时长标注/自动化卡降级/四态与来源/未知态两出口/日志中文与去噪/同步重试）。

---

## 4. 阶段二实施明细

### 4.1 P2-1 快捷操作（R4）

- **自由 1..12**（新增 `normalizeQuickIds`）：不再有 `0/1/2/4/8/12` 档位；**保留用户顺序**（顺序是配置的一部分）。
- 编辑器改为「已选列表（↑↓ 调整 / 移除）+ 可添加区」，上限 12 时禁止再加并提示。
- 首页**超过 6 个折叠**：`还有 N 个 · 展开全部` / `收起`；折叠只影响显示，配置不丢。
- 按设备隔离：`gf.quick_<deviceId>`（换设备不串用）。
- 测试：`ui_stage2_core` Q1 组 14 条 + `stage2_ui_playwright` S2-1/S2-2/S2-3(布局)。

### 4.2 P2-2 首页布局（R6）

- `HOME_CARDS` 4 区块；**锁定项 = 设备状态卡 + 最近操作**（不可隐藏/不可移动）。
  - 原型列 4 个锁定项（状态头/重量/阀态/进行中命令）；本实现把重量与阀态**收敛进主状态卡**，故锁定项为 2 个 —— 是收敛不是缩水（见 core 注释）。
- 布局编辑器：↑↓ 排序、显示/隐藏、保存、恢复默认（二次确认）。
- 存储按设备：`gf.layout_<deviceId>`；`sanitizeHomeLayout` 保证 order 覆盖全部区块、锁定项永不被隐藏。
- 测试：`ui_stage2_core` L1 组 6 条 + 浏览器 S2-3a..d。

### 4.3 P2-3 指定脉冲弹窗（R8）

- 从页面内联改为**独立弹窗**：脉冲数（min/max 取自能力目录 1–6400）+ 方向枚举（目录 `enumLabels`）。
- 校验即时生效：非法值 ⇒ **确认按钮保持 disabled** 并给出具体错误；合法 + 过 800 ms 防误触才可点。
- 文案明说：设备端还会按 `max_pulses` 再钳位；该动作未真机验证。
- 测试：浏览器 S2-4a..e（含 pulses=1600/direction 进帧）。

### 4.4 P2-4 危险确认弹层（R9）

- 新增统一 `askSheet()`（替代原生 `confirm/prompt`）：标题/结构化后果/影响范围/**防误触延迟**/`requireText` 输入闸/Esc 取消/背景点击取消。
- 已接入：开阀、出粮 120°、秤清零、急停、普通重启、强制重启（需输入 `RESTART`）、自动化运行/删除/提交/落盘、未知态「重新下发」、布局恢复默认。
- **刻意保留原生 confirm 的 2 处**（非危险）：放弃未保存草稿、查询全部状态键（只读）；已在代码注释说明。
- 测试：浏览器 S2-5a..e（确认按钮初始 disabled、取消零发布、确认后真发）。

### 4.5 P2-5 审计扩展筛选（R25）

- Worker `history/command` 新增 `family` / `from` / `to`（`state`/`cmd_id` 原有），均有参数校验；`from>to` ⇒ 400。
- UI：命令族 / 状态 / 时间范围三个下拉 + **风险列**；筛选变化即从第一页重查（保留 cmd_id 检索词）。
- 索引：`ix_ca_dev_time`、`ix_ca_state` 已具备，无新增迁移。
- 测试：`tests/api/history_command_filters` 17 条 + 浏览器 S2-8。

### 4.6 P3-3 自动化执行详情（R17）

- 独立路由 `#wfdetail/<slot>`：名称/状态/`第 N / M 步` + **步骤时间线**（done 实线、active 高亮、pending 虚线）。
- 数据只用真实字段：定义 ← `workflow.get`，运行态 ← `query_workflows`；**耗时/完成步数不显示**（设备不提供）。
- 第 0 步在云端运行时标注「云端运行跳过」（固件契约 §4.1）。
- 缺定义时降级：仍显示运行态与停止入口，不编造步骤。
- 测试：`workflowStepView` 8 条 + 浏览器 S2-6a..d。

### 4.7 P3-4 强制停止（R18）

- 已核实固件**仍无单 Workflow 停止命令**（`workflow_stop` 只清账本、不关执行器）。
- 详情页只提供 `safety.stop_all`，按钮与弹层**三处明写**影响范围（全部执行器 + 置锁存 + 设备无单流程停止）。
- 停止结果以设备回帧为准；无回帧不显示"已停止"。
- 测试：浏览器 S2-6e（发出的是 `safety.stop_all`）+ S2-6f（未发任何 workflow 停止命令）。

### 4.8 P3-5 自动化独立编辑器（R19）

- 独立路由 `#wfedit/<slot|new>`；**复用**既有 `openWfEditor/submitWfEditor`（校验、草稿、`workflow.create/set` 语义完全不变）。
- 列表页「编辑」「新建」改为导航到该页；提交/落盘前走确认弹层。
- 没有可靠协议支持的编辑项：保持"暂不支持"（未新增任何字段）。
- 测试：浏览器 S2-7a..c。

### 4.9 P3-6 重启恢复六段（R27）

- 六段：停止相关操作 → 下发重启请求 → 等待设备离线 → 等待设备重新上线 → 核查设备状态 → 恢复完成。
- 诚实性：
  - 「离线」由 EMQX 会话消失 **或** `last_seen` 停滞 ≥15 s（连续 ≥9 s 判定）观察；
  - 「重启完成」必须有 `boot_seq` 递增，否则只说「已上线，但重启未确认」；
  - 逐项核查：EMQX 连接/最近上行/boot_seq/阀门/称重/MQTT；**阀门的 last_result/enable 标注"不可核"**。
  - 失败定位到具体段（`failedAt`），不糊成"整体失败"。
- 离开设备页即停止监视（省 D1 读）；180 s 超时置失败。
- 测试：`restartView` 11 条 + 浏览器 S2-9a..c。

### 4.10 多设备与异步隔离（任务书 §4.6）

- `refreshStatePair` / `refreshHomeWorkflow` / `renderWfDetail` / `renderWfEditPage` 全部在**回写前校验设备身份**（`dev0 !== S.deviceId` ⇒ 丢弃），编辑页切换设备会清空草稿并提示。
- 所有命令都带当前 `device_id`（浏览器 S2-10 断言）。
- 未引入新的鉴权旁路：仍走同一会话与既有命令端点。

---

## 5. 测试结果（分层，不合并且不美化）

### 5.1 单元 / 契约（Node）

| 套件 | 断言 | 结果 |
|---|---|---|
| `ui_stage2_core`（新） | 39 | PASS |
| `history_command_filters`（新） | 17 | PASS |
| `ui_stage1_core` | 59 | PASS |
| `ui_app` | 194 | PASS |
| `ui_app_runtime` | 130 | PASS |
| `log_i18n_filter` | 57 | PASS |
| `log_scan_filter` | 35 | PASS |
| `read_proxy_allowlist` | 21 | PASS |
| `ui_stage5_remaining` | 58 | PASS |
| 其余 14 套件（api/pages/ui） | 704 | PASS |
| **合计** | **1414 PASS / 0 FAIL（23 套件）** | ✅ |

### 5.2 真实 Chromium

| 套件 | 断言 | 结果 |
|---|---|---|
| `stage2_ui_playwright`（新） | 38 | PASS（快捷折叠/排序/布局/脉冲弹窗/确认弹层/详情时间线/停全部/独立编辑/审计筛选/六段/设备隔离） |
| `stage1_ui_playwright` | 47 | PASS |
| `stage5_ui_playwright` | 22 | PASS（脉冲弹窗化已同步断言） |
| `mqtt_browser_playwright` | 23 | PASS |
| **合计** | **130 PASS / 0 FAIL** | ✅ |

### 5.3 API 仓自带套件

`guo-feeder-api/test/run_all.mjs` ⇒ **25/26**；唯一失败 = 既有 `S3-10-1b`（`cloud_api` 写路径在 mock 下 502/CONFIG_MISSING），与本次改动无关，**保留记录、未改断言**。

### 5.4 生产只读

见 §3：Worker 20/20 PASS；日志 code 派生已生效；R1 会话级 BLOCKED。

### 5.5 真机

❌ 未做。步进（DRV8825）/振动/称重（HX711）硬件未修复，相关动作仍带"未真机验证"提示。

---

## 6. 未完成 / 风险 / 阻塞

| 项 | 状态 | 说明 |
|---|---|---|
| 阶段二部署 | ⬜ 未部署 | 任务书不自动授权；部署后需重跑生产只读冒烟 |
| R1 会话级验收 | 🚫 BLOCKED | Turnstile 真人验证；需人工点一次「记录 → 审计」 |
| 真机动作 | ⚠️ 未验证 | 硬件问题，非 UI 可解 |
| `device_event` 上线事件 | 🚫 缺数据 | 生产 0 行（EMQX webhook 未接）⇒ 离线时长仍按最近上行估算 |
| 自动化耗时/完成步数 | 🚫 设备不提供 | 详情页只显示 `current_step` 推导；不编造 |
| 多设备切换 UI | ⬜ 阶段四 | 本阶段只做异步隔离；设备清单端点仍缺（D-5） |
| 审计 `family` 无独立索引 | ⚠️ 性能 | 单设备量级可接受；跨设备大量数据前需评估 |

---

## 7. 变更清单

### 7.1 云仓（阶段二）

| 文件 | 变更 |
|---|---|
| `guo-feeder-api/src/read-api.js` | 审计端点新增 `family/from/to` 校验与 WHERE |
| `guo-feeder-pagesdev/assets/js/gf-ui-core.js` | 新增 `QUICK_ACTION_MAX/normalizeQuickIds/quickCollapseSplit/moveItem`、`HOME_CARDS/sanitizeHomeLayout/homeRenderOrder`、`RESTART_STAGES/restartView`、`workflowStepView` |
| `guo-feeder-pagesdev/assets/js/gf-app.js` | `askSheet` 统一弹层；快捷操作自由排序/折叠；布局编辑器；脉冲弹窗；六段重启；`#wfdetail`/`#wfedit` 页面与路由；审计筛选+风险列；设备隔离守卫；`cacheOnly` 首页重绘 |
| `guo-feeder-pagesdev/app.html` | 弹层容器、两个新页面容器、排序/步骤/阶段样式 |
| `scripts/verify_stage1_prod.mjs` | 新增（生产只读验收脚本） |
| `tests/ui/ui_stage2_core.test.mjs` | 新增 39 |
| `tests/api/history_command_filters.test.mjs` | 新增 17 |
| `tests/e2e/stage2_ui_playwright.mjs` | 新增 38 |
| `tests/pages/ui_app.test.mjs` / `ui_app_runtime.test.mjs` / `tests/e2e/stage1_ui_playwright.mjs` / `stage5_ui_playwright.mjs` | 同步新交互（档位→自由排序、confirm→弹层、内联脉冲→弹窗、重启六段） |

### 7.2 协议 / 数据库 / 权限

- **协议零改动**（Topic/帧族/EventId 均未变）。
- **数据库零迁移**（`family` 复用既有索引）。
- **权限边界**：未新增端点、未放宽鉴权；Pages 只读代理仍只放行枚举路径（阶段一已加 `history/command`）。

---

## 8. 文档更新清单

| 文件 | 变更 |
|---|---|
| `docs/review/UI-V11-Gap-Audit-and-Implementation-Plan-2026-10-25.md` | §8 进度表更新到阶段二（含生产状态列）、§8.1 新增 X4–X7、§8.2 部署记录、§8.3 未验证项 |
| `docs/review/UI-Stage2-P2P3-Delivery-and-Stage1-Prod-2026-10-25.md` | 本报告（新增） |
| `.workbuddy/memory/MEMORY.md` / `2026-10-25.md` | 阶段二状态 + 新踩坑 |

---

## 9. 下一步（需授权）

1. **人工 1 分钟**：登录生产 → 「记录 → 审计」确认表格出现（补 R1 会话级验收）。
2. **授权部署阶段二**（Pages 5 文件；Worker 仅审计端点 1 文件）→ 部署后我重跑 `verify_stage1_prod` + 生产 UI 冒烟。
3. 阶段三起点：配置规格表（P4-0）→ 只读 UI → 写侧 → **失败演练门禁**。
