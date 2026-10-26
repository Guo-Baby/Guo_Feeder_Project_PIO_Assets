# Web UI 最终修复与 P6 收口 —— 交付报告（2026-10-26）

> ⚠ **本报告之后还有一轮**（同日）：《最终体验优化、验收与结项评估》——
> `docs/review/UI-Final-Optimization-and-Exit-Review-2026-10-26.md`
> （审计结果列、系统状态布局、滑动开关、配置卡片、顶栏迁移、悬浮保存区、
>   首页曲线与重启区；并给出**Web UI 结项评估**）。
> 本报告的 §0/§8 与线上版本信息**已随优化轮继续生效**：优化轮（commit `3db8f56`）已于 2026-10-26 部署，
> 当前生产 = Pages `655bac65-55e4-4c20-97ca-c611315abddb` / Worker `ad33173f-cb5e-4b36-b804-d9bc3f37505b`；
> "当前实现状态"与"四类状态区分"以**后一份**报告为准。

> 对应任务书《Guo Feeder Web UI 最终修复、P4-3 故障验收与文档收口任务书》。
> 贯穿全文的纪律：**部署成功 ≠ 生产验收成功；自动化测试通过 ≠ 真机验收通过；
> 模拟浏览器测试 ≠ 生产多设备验收。**

---

## 0. 结论速览

| 维度 | 结论 |
|---|---|
| 用户实测缺陷 | **8 项全部修复**（F-01…F-07 + F-10），含 2 项**既有真缺陷** |
| 单元测试 | **29 套件 0 FAIL**（含 2 个新语义断言更新） |
| 浏览器 e2e | **7 套件 222 PASS / 0 FAIL**（stage1 47 / stage2 38 / stage3 30 / stage4 19 / stage5 22 / **stage6 39（新增）** / mqtt_browser 27） |
| 生产部署 | ✅ **已部署**（用户授权）：Pages `1e425c10`（前端 4 文件）· Worker `6b57b014`（仅加写门禁变量） |
| 生产只读验收 | ✅ **21/21**（`verify_stage6_prod.mjs`，部署后；**部署前基线 9 PASS / 12 FAIL** ⇒ 机械证明"部署前确实没上"）。<br>阶段四回归 **33/33** · 阶段三回归 **49/49** |
| 真机配置写入 | ✅ **P4-3 演练实跑 6 PASS / 0 FAIL / 0 UNKNOWN / 4 NOT RUN**（设备 `288485896ce4`，键 `time.sync_interval_sec`）；演练后独立复核 **86400 / version 5 / boot_seq 103**（原值已还原） |
| P4-3 | **6/10 可自动化场景全部 PASS**；**4/10 破坏性 ⇒ NOT RUN**（附人工方案 + 风险 + 恢复） |
| 写入门禁 | ✅ **已按授权开放（D 档）**：开关开、**键范围不限制**；**鉴权 / 设备范围 / 类型·范围·枚举校验 / 乐观锁 / 审计 / 无故障注入入口 全部保留** |
| 生产变更 | ① Pages 前端 4 文件；② Worker **仅**新增 3 个写门禁环境变量。**零 migration、零 EMQX 变更、零新密钥** |

---

## 1. 本轮修复的问题编号、根因与变更文件

| 编号 | 现象 | 根因（带证据） | 变更文件 |
|---|---|---|---|
| **F-01** | 命令通道默认走 HTTP 代理，自动化运行期收不到设备实时反馈 | `gf-mqtt.js::cmdPath()` 旧实现：`localStorage` 不是 `"mqtt"` 就返回 `"http"` ⇒ **未设置时默认 http** | `assets/js/gf-mqtt.js`、`assets/js/gf-app.js`（通道显示文案/横幅） |
| **F-02** | 首页「当前自动化」不实时（要手动刷新） | ① 固件**不推逐步消息**（步进点 `workflow.cpp:4431/4494` 纯内存赋值零上报；终态帧 `command_manager.cpp:1144-1185` **无 current_step**；`timeout`/`failed` 同映射 `s=3`）；② 前端只做**一次性**读取 + 15 s 缓存，无轮询、无事件驱动 | `assets/js/gf-ui-core.js`（`wfRunPanelView`/`wfLastTerminal`/`workflowStateMap`）、`assets/js/gf-app.js`（`refreshHomeWorkflow`/`scheduleWfRunPoll`/`renderWfDetailRun`）、`assets/js/gf-mqtt.js`（`setFeedbackHook`） |
| **F-03** | 自动化编辑页一直显示「加载中」；从详情页返回列表后出现旧版 `编辑 Slot` 界面 | ★ **同源单因**：列表页（`gf-app.js:2388`）与编辑页（`:4518`）**都有 `id="wf-editor"`**（还有 `id="wf-edit-meta"`），而 `el()` = `getElementById` ⇒ 命中**文档里第一个**（`page-workflows` 在 `page-wfedit` 之前）⇒ 编辑器被画进列表页容器，编辑页自己的永远停在「加载中」；且 `openWfEditor` 抛异常时无人 catch | `assets/js/gf-app.js`（删列表页遗留容器；编辑页改专属 id `wfedit-editor`/`wfedit-meta`；加载失败显示错误 + 重试） |
| **F-04** | 状态查询只有「刷新常用状态 / 查询全部状态」两个入口 | `renderStatePanel` 只有两个按钮；`SYSTEM_STATE_KEYS` 本来就带 `group` 但未用 | `assets/js/gf-ui-core.js`（`systemStateGroups` + `stateStale`）、`assets/js/gf-app.js`（模块级按钮 + 失败不冒充新结果） |
| **F-05** | 配置字段控件与配置语义不符；`-1` 的禁用语义看不见 | 服务端规格表**本来就是对的**（`config-spec.js:181-206`：bool/int/enum/dead/min-max/建议范围分离），**UI 侧**没体现：数值输入没有 min/max 属性、弹层不展示 `spec.note`、枚举范围文案不带中文标签 | `assets/js/gf-ui-core.js`（`configRangeText` 带标签）、`assets/js/gf-app.js`（min/max + note 展示） |
| **F-06** | 手机端自动化列表挤压错位 | 整表 `.tscroll` 横滚（min-width 520px）⇒ 375px 屏上名称/状态/按钮全挤在一行 | `app.html`（`@media (max-width:700px)` 卡片化 + `button.wf-run` 主操作样式）、`assets/js/gf-app.js`（行结构重排） |
| **F-07** | 设备清单不可用时仍会向设备发命令 | ★ 旧 `deviceVisible()`：**清单未加载/为空 ⇒ 放行**（把"不知道"当成"有权"） | `assets/js/gf-app.js`（三态 `deviceVisibility()` + `renderVisibilityBanner` + `runCommand` 出口拦截）、`app.html`（`#visbar`） |
| **F-10** | （本轮顺带发现）状态查询失败时首页卡死/刷屏 | ★ **既有真缺陷**：`refreshStatePair` 查询失败 ⇒ 不写 `state_pair` 缓存 ⇒ `renderHome({cacheOnly:true})` ⇒ 首页发现没缓存 ⇒ 又调 `refreshStatePair` ⇒ **无限递归**（每轮 2 次设备往返，烧 D1 配额）。HTTP 通道下同样成立 | `assets/js/gf-app.js`（在途标志；boot 的 registry 加载改由"清单加载完成"驱动） |

### 1.1 固件侧**没有**改动的证明

本轮**未改一行固件**（`PIO_Assets/.../src/**` 无 diff）。F-02 的"实时"是在**现有协议**下做到的最强形态：
MQTT 帧事件（终态）+ `query_workflows` 轮询（步骤粒度），**没有**新增协议字段、没有新增命令。

---

## 2. 需求实现状态（本轮相关项）

| 需求 | 状态 | 证据 |
|---|---|---|
| §4.1 命令通道默认 MQTT 直连 | ✅ | `cmdPath()`：未设置 ⇒ mqtt；显式 `"http"` ⇒ http（`mqtt_path_confirm` E-8e…i） |
| §4.1 复用长连接 / 手动切 HTTP / 异常可切 / 不静默回退 | ✅ | `gf-mqtt.js` 单连接复用；`#chan-sel` 手动切换；`mqttErrorText` 给出可切提示；**代码里没有任何"HTTP 可用就自动切回"的分支** |
| §4.2 首页当前自动化实时反馈 | ✅（协议允许的最强形态） | 首页/详情页共用 `S.wfRun`；终态帧事件 + 轮询；切设备不串状态；断线回前台补查 |
| §4.3 编辑页加载 + 新旧 UI 混用 | ✅ | 专属 id + 错误/重试出口 |
| §4.4 模块级状态查询 | ✅ | 每模块一入口；只写回本次查到的键；失败标「上次成功值」 |
| §4.5 配置控件类型与校验 | ✅ | 电磁阀 5 键逐条断言（bool/int/enum/dead/min-max）；服务端 `validateConfigParams` 未动 |
| §4.6 移动端布局 | ✅ | 375px 实测：名称+状态同行、运行按钮独占行且 >50% 宽、无横向溢出、状态未隐藏 |
| §5 缓存/版本/请求代际 | ✅ | 配置缓存按设备；切换设备清空（含 `wfStates`/`stateStale`）；写入/保存后失效；版本冲突重读；执行状态不由静态版本判断 |
| §6 P4-3 | 🟡 6/10 可自动化（脚本就绪）、4/10 破坏性 NOT RUN（有人工方案） | §5 |
| §7 多设备隔离 | ✅（隔离面） / 🚫（生产多设备实测） | §6 |
| §8 测试 | ✅ | §3、§4 |
| §9 生产部署 | ⏸ **未执行**（需授权） | §8 发布计划 |
| §10 P6 与文档收口 | ✅ | §9 文档清单 |

---

## 3. 单元测试结果（29 套件 · 0 FAIL）

| 套件 | 结果 | 说明 |
|---|---|---|
| `api/config_spec_parity` | 30 / 0 | 配置规格表 ↔ 固件对齐（未改） |
| `api/config_write_command` | 66 / 0 | 配置写命令 + 服务端门禁（未改） |
| `api/ui_devices` | 22 / 0 | 设备清单（未改） |
| `api/presence_emqx` | 43 / 0 | 三态 presence |
| `api/ui_security` | 275 / 0 | 会话/鉴权 |
| `api/wfadmin` · `api/history_*` · `api/log_*` · `api/mqtt_pool` · `api/emqx_credential_rotation` · `api/snapshot_counts` | 18/17/29/57/35/68/21/11/33 | 未改 |
| `pages/*`（auth / webhook / allowlist / ui_app / ui_app_runtime） | 107/22/28/196/**131** | `ui_app_runtime` 因 F-10 修复后恢复 **131/0**（此前会挂死） |
| `ui/*` | 55/31/59/47/51/**58**/58 | `mqtt_path_confirm` 31（+2 新断言）、`ui_stage4_devices` 58（+7 可见性新语义） |

> ★ **测试更新的说明**（不是放宽）：`mqtt_path_confirm` 的 E-8e/f、`mqtt_frame_parity` 的 F-4b、
> `mqtt_browser_playwright` 的 P-2、`ui_stage4_devices` 的 M-13d…j 断言的是**旧默认值/旧可见性语义**，
> 而任务书**明确要求改掉这两处语义** ⇒ 断言改为断言**新契约**（默认 mqtt；未知也拦），
> 断言数量只增不减（31←29、58←51、27←27 中改的是 1 条 + 新增 2 条）。

---

## 4. 浏览器 e2e 结果（6 套件 · 222 PASS / 0 FAIL）

| 套件 | 结果 | 本轮相关 |
|---|---|---|
| `stage6_final_playwright.mjs`（**新增**） | **39 / 0** | 默认直连、实时反馈、编辑页、模块查询、配置类型、**375px 移动端**、桌面布局、可见性拦截 + 恢复出口 |
| `stage1_ui_playwright` | 47 / 0 | 桩补设备清单（可见性门禁的数据源） |
| `stage2_ui_playwright` | 38 / 0 | 同上 |
| `stage3_config_playwright` | 30 / 0 | 同上 |
| `stage4_devices_playwright` | 19 / 0 | 同上（清单桩已有） |
| `stage5_ui_playwright` | 22 / 0 | 改按"单一路由 + 路径分派"（Playwright 多 glob 优先级不可靠） |
| `mqtt_browser_playwright` | 27 / 0 | P-2 改为"启动期**零**代理调用"（证明默认直连） |

---

## 5. P4-3 故障场景逐项状态（§6.1 的 10 项）

| # | 场景 | 结论 | 依据 |
|---|---|---|---|
| S1 | Flash 写入失败 | **NOT RUN** | 破坏性；人工方案见 P4-3 决策文档 §4.4 |
| S2 | 配置保存完成但设备启动失败 | **NOT RUN** | 破坏性；同上 |
| S3 | 非计划重启触发配置回滚 | **NOT RUN** | 破坏性（需物理断电）；同上 |
| S4 | 配置提交长期停留 PENDING | **NOT RUN** | 破坏性（需在 commit 窗口断电）；同上 |
| S5 | 版本冲突 | **PASS** | 阶段三真机：`expect_version=51`（实际 1）⇒ `s=3 e=7 version mismatch`，**RAM 未污染** |
| S6 | 取消自动重启 | **PASS** | 阶段三真机：cancel ⇒ `s=0`；等满 330 s，boot_seq 不动、值不变 |
| S7 | 设备离线 / 回执超时 | **PASS** | 实跑：沙箱设备 ⇒ 服务端 **403 `config_write_device_not_allowed`** + **零下行帧** ⇒ 判「未执行」而非成功 |
| S8 | 云端收到请求但没有设备最终结果 | **PASS** | 实跑：**无任何设备 result 帧** ⇒ 结果 UNKNOWN，界面不得显示成功 |
| S9 | 配置已生效但 UI/网络未能及时确认 | **PASS** | 实跑：写入侧 `s=0`（已生效）；回读侧**无设备 result 帧**（发布记录 1 条 / result 帧 0 条 ⇒ 设备不存在、无回执）⇒ UI 必须显示未知 |
| S10 | 恢复原始配置后版本与启动状态一致 | **PASS** | 实跑：还原后回读 **86400**（原值）· version 5 · boot_seq 103；**独立复核**一致 |

**脚本**：`scripts/hw_config_fault_drill.mjs`
（只跑指定设备 · 双重确认 · 前置逐项检查 · 不知原值不许开始 · 分项记录 · 只有 `s=0` 算成功 ·
无回帧=UNKNOWN · 恢复后复核 · 失败即停 · 30 min 防重入锁 · JSON + MD 双输出 · `--dry-run` 只读）。

**实跑结果（2026-10-26，写门禁开放后）**：**6 PASS / 0 FAIL / 0 UNKNOWN / 4 NOT RUN**
（证据 `Cloudflare_Assets/docs/evidence/p4-3-fault-drill-2026-10-26/{result.json,report.md}`）。
演练前置：值 86400 / version 4 / boot_seq 101（第二次演练 version 4→5、boot_seq 102→103）。
演练后**独立复核**（另发一条 config_query 回读）：`time.sync_interval_sec = 86400`（原值）·
`versions.time = 5` · 最新日志 `boot_seq = 103` ⇒ **原值已还原，设备状态正常**。

> ★ **为什么 S1–S4 只能人工**：它们分别需要介质级 Flash 故障、应用启动失败、物理断电、
> Flash 事务窗口断电。固件**没有**对应的故障注入接口，且**不得**在生产固件里加默认开启的
> 危险入口；本项目也**只有一台**设备可用作实验品、没有可安全丢弃的第二台。
> ⇒ 这是**硬件约束**，记为**未执行 · 有理由的风险接受项**，不是"测试通过"。

---

## 6. 配置写入门禁：最终状态、依据与授权记录

| 项 | **变更前** | **变更后（已授权开放 · D 档）** |
|---|---|---|
| `CONFIG_WRITE_ENABLED_BY_DEFAULT` | false | **false（仍不变）** —— 关闭是靠 env，不是靠改默认值 |
| 生产目录 `config_write.enabled` | false | **true**（实测 `source: env.CONFIG_WRITE_ENABLED`） |
| 设备范围 | — | `["288485896ce4"]`（**保留设备范围控制**） |
| 键范围 | — | `[]` = **不限制**（D 档：任何配置键都能改） |
| 生产写命令 | 403 `config_write_disabled` + 零下行帧 | **202 受理**（演练实测） |
| 服务端类型/范围/枚举/占位符校验 | 保留 | **保留**（`validateConfigParams`） |
| `expect_version` 乐观锁 | 保留 | **保留**（S5 实测 `s=3 e=7`，RAM 未污染） |
| 命令审计 | 保留 | **保留**（`command_audit` 全量） |
| 故障注入入口 | 无 | **无**（破坏性注入与普通编辑**严格分离**） |
| 鉴权 | Bearer / 会话 | **不变** |

> ★ **授权记录**：2026-10-26 用户在"门禁策略"三选一中明确选定 **D 档**（"不加键白名单，
> 任何配置键都能改"）。本轮据此开放，**但没有**把安全校验一并关掉 ——
> 任务书 §6.4 明令"不得直接实现为关闭所有安全校验"，故 **鉴权 / 设备范围 / 类型·范围·枚举校验 /
> 版本冲突保护 / 失败状态处理 / 恢复能力全部保留**，且**不提供任何故障注入入口**。
> ★ **风险如实记录**：键不再受白名单限制 ⇒ 可改 `valve.gpio_pin`（继电器引脚）、
> `valve.active_level`（开阀电平）、`stepper.*` 等**直接驱动物理执行器**的参数，
> 改错可能**失控 / 常开 / 电机异常动作**。缓解：UI 对 `risk=critical` 的键用红色危险确认
> + 展示规格表 note；固件对部分键有硬钳位；写完必须回读确认；审计全量留痕。
> ★ **一键回滚**：`wrangler.toml` 把 `CONFIG_WRITE_ENABLED` 改回 `"0"` 后重新 deploy ⇒ 立即回到只读。

**三方案已提交待授权**（详见 `docs/review/P4-3-Fault-Drill-and-Config-Gate-Decision-2026-10-26.md` §4.2）：

| 方案 | 内容 | 风险 | 结论 |
|---|---|---|---|
| A 保持关闭 | 现状 | R30 永久 🚫 | 未选 |
| B 受限开放 | 常开 + 键白名单（只放 `time.*`） | 中低 | 未选（**保留为可回退的中间档**） |
| **D 全开** | 常开 + **键范围不限制**（设备范围与其它校验仍保留） | 高：可改物理执行器参数 | ✅ **用户选定并已实施** |

**实施结果**：Worker `6b57b014`（`CONFIG_WRITE_ENABLED=1` / `CONFIG_WRITE_DEVICES=288485896ce4` /
`CONFIG_WRITE_KEYS=""`），回滚目标 `5a5513f2`（门禁关闭版）。

---

## 7. 多设备隔离：已验证范围与未验证部分

| 面 | 状态 | 证据 |
|---|---|---|
| 清单可见 + 切换 | ✅ | `stage4` 19/19、`stage6` N-2/N-3 |
| 切换后清空设备级状态（32 键 + 本轮新增 `wfStates`/`stateStale`） | ✅ | `DEVICE_SWITCH_CLEAR` 单一口径 + 断言 |
| 命令目标 = 当前设备 | ✅ | `runCommand` 出口校验 + `staleDeviceResponse` 丢弃 |
| 过期响应不更新当前页 | ✅ | 代际守卫（`wfRunGen`/`stateGen`/`switchGen`） |
| 不可见设备不收命令 | ✅（本轮**收紧**） | `deviceVisibility()` 三态；**未知也拦** + 横幅 + 重试 |
| 自动化/配置/日志/状态按设备隔离 | ✅（前端层） | 缓存键含 device_id |
| **生产多设备实测** | 🚫 **未做** | 生产**只有 1 台**真机 |
| **跨设备数据/权限隔离** | 🚫 **不成立** | 所有设备共享 MQTT 账号；ACL 不按设备区分 |
| 跨端别名同步 | 🚫 未做 | 需 D1 迁移（未获批） |

---

## 8. 生产发布计划（**待授权，本轮未执行**）

### 8.0 ★ 执行结果（2026-10-26，用户授权后已执行）

| 项 | 结果 |
|---|---|
| **Pages** | ✅ 部署 `1e425c10-d950-4efa-9829-16d63b1de5b3`（前端 4 文件） |
| **Worker** | ✅ 部署 `6b57b014-7a94-44f0-b8b9-30508f8fd8d4`（**仅**新增 3 个写门禁环境变量，代码 0 改动） |
| 部署后验收 | `verify_stage6_prod` **21/21** · `verify_stage4_prod` **33/33** · `verify_stage3_prod` **49/49** |
| 部署前基线 | `verify_stage6_prod` **9 PASS / 12 FAIL**（12 项 = 线上还没本轮代码）⇒ 部署前后对照清晰 |
| 写门禁 | 开放（D 档），实测目录 `enabled=true` / `devices=[288485896ce4]` / `keys=[]`（不限制） |
| 真机演练 | P4-3 **6 PASS / 0 FAIL / 0 UNKNOWN / 4 NOT RUN**；设备已还原（86400 / v5 / boot_seq 103，独立复核） |

### 8.1 变更清单

| 项 | 内容 |
|---|---|
| Worker | `guo-feeder-api/src/**`：**0 处代码改动**；`wrangler.toml` **新增 3 个写门禁变量**（授权变更） |
| Pages | `guo-feeder-pagesdev/assets/js/gf-app.js`、`gf-ui-core.js`、`gf-mqtt.js`、`app.html`（4 个文件） |
| 数据库 | **无**（零 migration） |
| EMQX | **无**（账号池/ACL 不动） |
| 鉴权 | **不变** |
| 写入门禁 | **变更**（用户授权 D 档；其它校验全部保留，见 §6） |
| 新密钥/环境变量 | **无 Secret**；3 个普通 env var |

### 8.2 版本与回滚

| 项 | 值 |
|---|---|
| 部署前生产 Worker | `5a5513f2-d386-434a-910b-3f0fe08de4b2` |
| **Worker 回滚目标** | `5a5513f2`（门禁关闭版；改 `wrangler.toml` 后重新 deploy） |
| 部署前生产 Pages | `79199104-d6c1-42f3-a541-210aa7134b12` |
| **Pages 回滚目标** | `fa8e6f9d-2f92-4754-9216-80d57241ee93`（阶段三）→ 必要时 `0d960fb4`（阶段二） |
| 代码 commit | 云仓 `cfa8fa4`（前端）· `8f652c3`（门禁 + 验收脚本） |

### 8.3 只读 / 写生产区分

| 步骤 | 类型 | 说明 |
|---|---|---|
| 1. `wrangler pages deploy .`（`guo-feeder-pagesdev/` 内） | **写生产**（前端产物） | 判据：输出含 `Compiled Worker successfully` |
| 2. `scripts/verify_stage4_prod.mjs` | 只读 | 33/33 + 阶段三回归 49/49 |
| 3. 新增：`verify_stage6_prod.mjs` | 只读 | **部署前基线 9/21**（12 项失败 = 线上还没本轮代码）→ 部署后应 **21/21** |
| 4. 人工验收（Turnstile 会话级） | 人工 | 设备清单页、自动化编辑页、配置弹层 |

### 8.4 部署后必须重新验证（§9 要求）

默认 MQTT 直连 · HTTP 备用通道 · 自动化编辑与运行 · 首页实时执行状态 · 模块级状态查询 ·
配置读取与写入门禁（仍 403）· 多设备只读检查 · 日志/审计/鉴权/旧功能回归。

### 8.5 风险与恢复

| 风险 | 恢复 |
|---|---|
| 新前端在生产报 JS 错误 | Pages 回滚到 `fa8e6f9d`（一次部署，秒级） |
| 默认直连导致某浏览器环境连不上 EMQX | 用户可在通道下拉**手动切 HTTP**（已实现，无需回滚） |
| 可见性收紧导致命令被误拦 | 横幅提供「重试加载设备清单」；必要时回滚 Pages |

---

## 9. 文档更新清单（本轮）

| 文档 | 变更 |
|---|---|
| `docs/review/UI-Final-Fixes-and-Stage6-Closure-2026-10-26.md` | **新增**（本文） |
| `docs/review/P4-3-Fault-Drill-and-Config-Gate-Decision-2026-10-26.md` | **新增**：10 场景逐项结论 + 演练脚本说明 + 门禁三方案 + 人工方案 + 变更前后状态表 |
| `docs/user-guide/User-Guide-and-Config-Write-Risk-2026-10-26.md` | **新增**：用户使用说明 + **配置写入风险说明** |
| `docs/review/UI-V11-Gap-Audit-and-Implementation-Plan-2026-10-25.md` | §8 进度表：新增 §8.6（收口轮状态）、§8.5 细化、顶部变更说明补 2026-10-26 条目 |
| `docs/architecture/Cloud-APP-Platform-Plan.md` | **新增 §13 P6 收口**：8 项设计确认的现状、**已决策不做**的 4 项、WebView 套壳的下一步 |
| `docs/review/EMQX-Account-Pool-and-ACL-Lifecycle-2026-10-25.md` | §5.2 由"动态池需求"改为**"决策记录 = 不做"**；新增 §5.3 运维速查 |
| `docs/review/UI-Stage3-Deploy-and-RealDevice-Write-Acceptance-2026-10-25.md` | 阶段三报告（补 boot_seq 采样坑） |
| `docs/review/UI-Stage4-P5-MultiDevice-Delivery-2026-10-25.md` | 阶段四报告 |
| `Cloudflare_Assets/docs/evidence/config-write-realdevice-2026-10-25/` | 真机证据（result.json / report.md / final-recheck.json） |
| `Cloudflare_Assets/docs/evidence/p4-3-dryrun/dry-run.json` | **新增**：P4-3 只读演练计划与前置检查结果 |

---

## 10. 唯一进度表的最终状态

见 `docs/review/UI-V11-Gap-Audit-and-Implementation-Plan-2026-10-25.md` §8（本轮新增 §8.6）。
要点：**R26 仍 🟡**（跨端别名未做、生产多设备未实测）；**R30 仍 🚫**（门禁未开 + 4 个破坏性场景未执行）；
R7/R11/R12/R13/R14/R15 的生产状态与真机列**已按阶段三真机结果更新**。

---

## 11. 仍未解决的问题、风险与下一步

| # | 问题 | 原因 | 下一步 |
|---|---|---|---|
| 1 | ~~本轮修复未部署~~ | — | ✅ **已部署并验收**（21/33/49） |
| 2 | ~~P4-3 的 S7/S8/S9 未实跑~~ | — | ✅ **已实跑全 PASS** |
| 3 | ~~写入门禁仍关闭~~ | — | ✅ **已按授权开放（D 档）** |
| 3b | **写通道现已开放且键不限制** ⇒ 可改物理执行器参数 | 用户选 D 档 | ★ 建议：① 改前先看 UI 的红色危险确认 + 规格表 note；② 改完**回读确认**；③ 若要收敛，改 `CONFIG_WRITE_KEYS` 为 `"time.sync_interval_sec,time.timezone"` 后重新 deploy（无需改代码） |
| 4 | P4-3 的 S1–S4 破坏性场景未执行 | 需物理接触设备 + 可恢复 bootloader；只有一台设备 | 拿到第二台设备后按人工方案执行；或接受风险并记录 |
| 5 | **生产多设备未实测** | 只有 1 台真机 | 拿到第二台后按 `stage4_devices` 流程实测；在此之前**不得**宣称已验收 |
| 6 | 跨端别名未做 | 需 D1 迁移（未获批） | 若要跨端，单独申请 migration 授权 |
| 7 | 跨设备数据/权限隔离不成立 | 共享 MQTT 账号 + 不做动态 ACL（已决策） | 需要多用户/多租户时重新评估账号模型 |
| 8 | 会话级页面无法自动化验收 | 生产 Turnstile | 请用户在浏览器里人工点一次（清单页 / 编辑页 / 配置弹层 / 移动端 / 通道指示灯） |
| 8b | **D 档下"写入≠生效"仍依赖回读** | 固件对部分键**静默钳位**、不回执 | UI 已标「上次成功值 · 本次查询失败」；**用户写完务必回读** |
| 9 | 步进/振动/称重真机未验 | DRV8825 坏、HX711 未接 | 硬件修复后单独验 |
| 10 | 直连路径无云端审计 | 产品取舍（任务书已接受） | 需要审计时切 HTTP；或后续补"浏览器侧审计上报"（已列为可选，不阻塞） |
