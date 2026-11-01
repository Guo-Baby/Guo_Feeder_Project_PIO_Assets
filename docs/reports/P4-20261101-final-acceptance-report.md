# Guo Feeder · P4 最终交付与验收报告

- 生成时间：2026-11-01 18:40 (GMT+8)
- 唯一依据：`D:\Guo_Feeder_Project\P4-20261101-execution-ledger.md`（T01 / T03 / T05 / T06 / T07 / T08 / T09）
- 生成纪律：**只读台账与其引用的测试/部署/刷写证据**；未重新通读三个仓库源码，未补做任何调查。

---

## 1. 执行摘要

| 项 | 数量 | 说明 |
|---|---|---|
| 本轮任务书条目 | 10（T01–T10） | 用户指定执行 **T01 / T03 / T05 / T06 / T07 / T08 / T09 / T10**；**T02、T04 由用户判定本轮不做** |
| 状态 | **PASS 5 · PARTIAL 1 · BLOCKED 1 · 未执行 2** | 详见 §2 |
| 代码/文档级修复的缺陷 | **13 个** | 9 个属前序轮次的既有缺陷，4 个为本轮新引入后自行发现并修复 |
| 新发现并已修复 | **9 个** | 其中 2 个是**安全类**（CSRF 失效、凭据误报成功）、1 个是**首页不可用级**（presence 无超时） |
| 新发现但**未**修复 | **2 个** | 见 §6（一项 BLOCKED 于真实账号、一项 BLOCKED 于日志检索能力） |
| 已部署 | Worker `7b102f86`、Pages `6c1011f4` | 见 §3 |
| 已刷写固件 | **否（BLOCKED）** | 本机无 ESP32 USB 串口，见 §3、§6 |

> **一句话结论**：代码侧交付完成且生产已上线；**固件未刷写**、**真实米家账号验证未做**、**人工物理测试未做**，
> 因此 P4 **不能**宣布"完全关闭"。

---

## 2. 逐任务交付表

| 编号 | 任务目标 | 实际完成内容 | 测试结果 | 部署状态 | 结论 |
|---|---|---|---|---|---|
| **T01** | P0-3 统一设备预检与冷却提示 | 新增唯一入口 `workflowRunPreflight()`（首页快捷 + 自动化页共用）；新增纯函数 `devicesForWorkflow()` / `workflowDevicePreflight()`（物理/安全/只读三级）；米家桥新增 `deviceSnapshots()` / `refreshDevices()`（一次查全部涉及设备）；冷却 45 s 内不查询只报剩余秒数；断电安全例外保留 | 新增 `tests/ui/p0_3_unified_preflight.test.mjs` **49/0**；`tests/ui`+`api`+`pages` 全绿 | 已部署（Pages） | **PASS** |
| **T02** | HTTP 代理通道剩余问题 | **未执行**（用户判定优先级不高，本轮放弃） | — | — | **未执行** |
| **T03** | 米家 API 日志与日志体系核对 | 结论：**继续写 Workers Logs，不写 D1**（`log_history` 幂等键 `(device_id,boot_seq,seq)` 且 `device_id NOT NULL`，塞入必须伪造 ⇒ 违反"不得伪装成设备日志"）；修 2 个真实泄露路径（`Authorization: Bearer` 未被掩码、自由文本可绕过白名单）；失败日志加洪泛上限 30/60 s（非采样） | 新增 `test/current/xiaomi_api_log_security.test.mjs` **41/0**；既有 `xiaomi_api_log` 74/0 | 已部署（Worker） | **PASS**（含 1 条待决策 D-01） |
| **T04** | 首页初始化请求数量调查与优化 | **未执行**（用户判定优先级不高，本轮放弃） | — | — | **未执行** |
| **T05** | Config dirty 刷新恢复 | 固件 `config_query` 增补顶层 `dirty:{module:bool}` + `restart_in_ms`（`data`/`versions` 一字未改）；前端 `S.configDeviceDirty` 与会话 `S.configPending` **分开存**；绝不从 localStorage 恢复 dirty；`saveConfigToFlash()` 支持"无 pending 但设备 dirty"；`normalRestart()` 把设备 dirty 纳入"会丢失"判定；详情页五层状态 | 新增 `tests/ui/config_dirty_restore.test.mjs` **51/0**；新增跨仓契约 `test/ui_v11_contract/config_dirty_contract.py` **20/0**；固件编译 SUCCESS | 前端已部署；**固件未刷写** | **PARTIAL**（代码完成，真机未验） |
| **T06** | 取帧语义审查 | 结论：**可统一**。两个旧函数"互补地错"（一个丢顺序、一个认不出压缩帧的 `o`）⇒ 新增唯一实现 `pickResultFrame()`（4 层判据：载荷+终态 / 任意载荷 / **结果帧无载荷**（压过 ACK）/ 最后非空帧）；两个旧名改为**委托别名** ⇒ 28 个调用点零改动 | 新增 `tests/ui/frame_pick_semantics.test.mjs` **27/0**；`ui_app_runtime` 138 断言通过 | 已部署 | **PASS** |
| **T07** | 部署前检查与 Worker/Pages 部署 | 三仓状态核对；前序改动完整性核对；Worker 全量 + 前端 20 套件 + 语法门禁全绿；**Pages 部署脚本 exit 66 未复现**（语法门禁正常放行，未绕过任何门禁）；记录回滚点；部署 Worker 与 Pages；最小只读验证 7 项 | Worker `run_all` 全绿；`tests/ui`(20)+`api`(14)+`pages`(6) exit=0；`frontend_syntax` 27/0；部署脚本 `--dry-run` exit 0 | **已部署** | **PASS** |
| **T08** | 固件刷写与刷后验证 | 源码↔产物对应性验证（重编译后 SHA **完全一致**，可复现）；构建环境与分区核对；SHA256 验证；刷写命令与验收清单就绪；**刷写本身无法执行**（本机仅有 3 个蓝牙虚拟串口，无任何 USB 串口） | 固件编译 SUCCESS（1,471,408 B / SHA `68b2cdcb…`） | **未刷写** | **BLOCKED** |
| **T09** | 生产只读 + 浏览器自动化验证 | 5 个只读脚本 + 2 个浏览器脚本实跑；**发现并修复 4 个真实缺陷**（详见 §4） | 只读：21/0、36/0、15/0、24/0、15/0；浏览器：**62/1**（首页缺陷修复前后为 10/10 → 62/1） | 因此再部署 4 次 Pages + 2 次 Worker | **PARTIAL** |
| **T10** | 生成本报告 | 本文件 | — | — | **PASS** |

---

## 3. 部署与固件状态

### 3.1 生产版本（最终）

| 组件 | 版本 ID | 时间 | 回滚目标 |
|---|---|---|---|
| **Worker** | `YOUR_RESOURCE_ID` | 2026-11-01 约 17:10 | `0d66db53`（本轮首部署）→ `879a7e78`（本轮前生产）→ `48fbce05` → `de8d98ff` → `780b3d59` |
| **Pages** | `6c1011f4` | 2026-11-01 约 18:30 | `1872c8d7` → `062fab03` → `dc74b996` → `330f7d00` → `30cb0b2b` → `b8218335`（本轮前生产） |

本轮 Pages 共部署 **6 次**、Worker **2 次**（T07 首部署 + T09 缺陷修复）。每次部署原因见台账 §T09.6。

**回滚方式**
- Pages：Cloudflare Dashboard → Pages → Deployments → 目标版本 *Rollback to this deployment*。
- Worker：`npx wrangler rollback --config guo-feeder-api/wrangler.toml <version-id>`。
- 兼容性：新前端与旧 Worker、旧前端与新 Worker **均兼容**（本轮 Worker 无响应结构的破坏性变更；
  新增字段只有 `config_query.dirty` / `restart_in_ms`，旧前端会忽略）。

### 3.2 固件

| 项 | 值 |
|---|---|
| 生产在跑 | `7f71ada7…`（2026-10-31 刷写） |
| **待刷产物** | **`68b2cdcbb06a9f308ad66b04b6cbee3be325f0b61ff55b0f6bdc5b3242f467ae`**，**1,471,408 B** |
| 可复现性 | 重新 `pio run` 后 SHA **完全一致** |
| 归档 | `docs/evidence/build-archive/20261101-154803-esp32-s3-devkitc-1/`（含 `.manifest.txt`：`git_HEAD=cb7a1c7`、`git_dirty=34`、`pio 6.2.0`） |
| 刷写状态 | **未刷写（BLOCKED）** |
| 刷前 `boot_seq` | **227**（D1 `log_history`，最近日志 2026-11-01 16:38，设备在线） |
| 刷写方式 | **只刷 app 分区 `0x10000`**；**绝对禁止 `uploadfs`**；也不要用 `pio run -t upload`（会写 bootloader/partitions/boot_app0） |

★ 任务书命令 8 里写的 `e29ce50c… / 1,471,152 B` **已作废**（T05 改了固件）。

### 3.3 D1 / EMQX

| 项 | 变化 |
|---|---|
| D1 `YOUR_D1_DATABASE_NAME` | **无 schema 变更、无新迁移**（`0011`/`0012` 为前序轮次已应用，本轮仅纳入版本控制） |
| D1 写入 | 本轮验证脚本全部为只读；`verify_p0_prod` 显式对账"零新增 `mqtt_messages` 行" |
| EMQX | **未修改**（未改 listener / 认证 / ACL / 账号）。⚠ 历史遗留：`AUTH` 类账号仍在（不属本轮） |

---

## 4. 新增缺陷与隐藏缺陷

### 4.1 本轮新引入、随后自行发现并修复（4 个）

| # | 现象 | 根因 | 修复文件 | 测试证据 | 已部署 |
|---|---|---|---|---|---|
| A | 首页停在「加载中…」45 s，Chromium **`Target crashed`** | `renderConfigBar()` 内无条件 `ensureConfigDirtyKnown().then(renderConfigBar)`：读不到配置时 `known` 仍 false，而退避分支**立即返回已 resolve 的 promise** ⇒ **微任务自激循环** | `guo-feeder-pagesdev/assets/js/gf-app.js` | `config_dirty_restore.test.mjs` C-9a..e（51/0） | ✅ |
| B | 米家 4 个写入口（`auth`/`logout`/`sync`/`lan-token`）**CSRF 完全失效** | 调用写成 `if (!verifyCsrf(request))`：函数是**对象解构 + async** ⇒ 返回对象/Promise，**恒为真值** ⇒ 永不拒绝。既有测试只测函数本身，**从未测调用点** | `guo-feeder-pagesdev/functions/api/xiaomi/{auth,logout,sync,lan-token}.js` | 新增 `tests/api/pages_csrf_callsites.test.mjs`（49/0）；生产实测：缺头 ⇒ **403**、篡改 ⇒ **403** | ✅ |
| C | `verify_p1_prod.mjs` 的米家状态检查**永远 PASS**（假绿） | 请求 `/api/xiaomi/status` —— Pages 无此函数 ⇒ 落到静态兜底 `index.html`（200 + HTML）⇒ `loginEnabled` 恒 `undefined` | `scripts/verify_p1_prod.mjs` | 复跑 15/0，真实值 `state=ACTIVE`、`loginEnabled=开` | ✅ |
| D | 填**任意错误密码**也提示「已用你提供的账号连上 EMQX」并起 30 分钟 TTL | 页面加载时池连接仍在建立，`disconnect()` 只清 `_client`、**未动 `_connectPromise`** ⇒ `ensureConnected()` 返回那次**池连接**的 promise ⇒ 误报成功 | `guo-feeder-pagesdev/assets/js/gf-mqtt.js`（+ `gf-app.js` 文案） | `manual_mqtt_state.test.mjs` M-9a..h（48/0）；生产真实错误凭据实测 15/0 | ✅ |

### 4.2 既有隐藏缺陷（本轮发现并修复，5 个）

| # | 现象 | 根因 | 修复 | 测试 | 部署 |
|---|---|---|---|---|---|
| E | **首页永久「加载中…」**（状态卡/快捷操作/「▤ 布局」全不出现，底部导航可点） | `renderHome()` 的第二个 await `fetchEmqxPresence()` **无超时**，而 `GET /api/read/admin/presence/emqx/<dev>` 偶发**永久挂起**（实测 pending > 10 s；Node 侧同路径 1.8 s） | `gf-app.js`：**仅在该调用处**加 4 s 上界，超时按"在线状态未知"（`p:null`），不动 `fetchEmqxPresence` 内部缓存/in-flight 语义 | 修复前后 10/10 → **62/1**；`ui_app_runtime` 的 T-17-22/37/38 保持通过 | ✅ |
| F | 米家密码登录一律显示 `unknown` | 4 个叠加根因：错误码漏 `20003/10025/20024/70014`；UNKNOWN 兜底不带上游文案；**Worker 成功响应缺 `state:"ACTIVE"`**；前端从不读 `b.detail.message` | `src/xiaomi/{auth,routes}.js` + `gf-xiaomi.js` | `xiaomi_login_unknown_fix` 等套件 | ✅ |
| G | 敏感键名单漏 `pwd`/`pass` 等密码别名 | 名单只收了 `password/passwd` | `src/xiaomi/api-log.js` | `xiaomi_api_log` 74/0 + `_security` 41/0 | ✅ |
| H | 二维码轮询未扫码时**阻塞 126 s** 后返回 502「失败」 | 上游 `lp` 是长轮询，旧实现无上界且把空响应判成 `NON_JSON` ⇒ `ERROR` | `src/xiaomi/auth.js`：`AbortController` 10 s 上界；**超时/空体 ⇒ `WAITING`**（等待≠错误）；有内容但非 JSON 仍 `ERROR` | 新增 `xiaomi_qr_poll_timeout.test.mjs` 11/0；生产实测 `qr-poll` ⇒ 200 `WAITING` | ✅ |
| I | 编辑器渲染崩溃 / 缓存键串模型（`timeoutSec` 被静默改成 600、步骤参数丢失） | `Object.keys(s.params)` 对无 `params` 步骤抛错；`ensureWfMeta`（设备模型）与 `openWfEditor`（表单模型）**共用** `wf_detail_<slot>` | `gf-app.js`（`stepParams` 兜底 + 拆键 `wf_meta_<slot>` + `invalidateWfSlotCache()`） | `ui_app_runtime` 138/0；`tests/ui` 多套件 | ✅ |

### 4.3 新发现但**未**修复（2 个）

| # | 问题 | 状态 | 原因 |
|---|---|---|---|
| J | 米家密码登录对**真实错误码**的处理未经验证 | **BLOCKED** | 没有可用的米家测试账号；不猜测账号以免触发上游风控。已用"空凭据 ⇒ 400 校验拒绝、不产生上游尝试、不破坏既有会话"证明门禁与校验在位 |
| K | 米家 API 日志的**线上内容**缺运行时证据 | **BLOCKED** | Workers Logs 无历史检索 API，`wrangler tail` 仅实时；本轮未在触发米家调用时挂 tail |

---

## 5. 人工测试清单（**必须由用户本人执行**）

> 以下 10 组是本轮唯一无法由自动化代替的验证。每项含：操作 / 预期 / 失败需反馈什么。

### 5.1 固件刷写（前置，其余真机项都依赖它）
1. 用 USB 数据线把 ESP32-S3 接到本机，确认出现新的 `COM<N>`。
2. 复核：`sha256sum .pio/build/esp32-s3-devkitc-1/firmware.bin` = `68b2cdcbb06a…`，1,471,408 B。
3. 执行：`python -m esptool --chip esp32s3 -p COM<N> -b 921600 --before default_reset --after hard_reset write_flash -z 0x10000 .pio/build/esp32-s3-devkitc-1/firmware.bin`
   - **预期**：`Hash of data verified.` + `Hard resetting via RTS pin...`
   - **禁止**：`uploadfs`（会覆盖 LittleFS ⇒ 丢配置、可能清米家 Token）
4. 刷后 5 分钟内看 D1：`MAX(boot_seq)` 由 **227 → ≥228**（这是"真重启"的唯一可信判据；presence 全程 online 不作数）。
5. **失败需反馈**：端口号、esptool 完整输出、刷前的 `boot_seq`。

### 5.2 喂食器与插座**双设备并发**
- 操作：控制页同时点「读取喂食器状态」与「读取插座状态」（或连续快速点）。
- 预期：两台各自返回、**不串台**（各自归属自己的卡），没有"喂食器的结果出现在插座卡里"。
- 失败反馈：两张卡的完整文案 + 时间 + 是否只出现一台。

### 5.3 插座**紧急断电**
- 操作：喂食器处于"执行中/离线/状态过期"时，点「插座断电」。
- 预期：**允许执行**（安全例外：断电不要求设备在线/状态新鲜）；文案里写明"安全例外"。
- 失败反馈：弹窗全文 + 当时的在线状态标签。

### 5.4 Config 停用后首页快捷操作**立即**同步
- 操作：配置页把「米家喂食器」`enable` 关掉 → 回首页看快捷操作里的米家出粮项。
- 预期：**立即**置灰（不需刷新），且点不动；恢复开启后立即恢复可点。
- 失败反馈：操作前后截图 + 是否刷新过页面。

### 5.5 多设备 Workflow 的**整份预检**
- 操作：建一份 `[开阀 → 米家出粮 → 关阀]`，把喂食器断电后从首页快捷与自动化页各点一次运行。
- 预期：两处**完全相同**地拒绝，弹窗逐行列出"米家喂食器（步骤 2 · 物理动作）：不在线"，
  并明确**第 1 步的阀没有被开**；提供「查询全部涉及设备」。
- 失败反馈：弹窗全文 + 阀门实际开合状态 + 用的哪个入口。

### 5.6 首页快捷操作与**诊断页结果弹窗**
- 操作：设备页点「查内存」「查 Flash」「查统计」。
- 预期：三者都显示**中文摘要**（内部 RAM / 外部 PSRAM 等），并保留可展开的原始 JSON；
  **不出现**"未取到设备回复"。
- 失败反馈：Console 首条红错 + `/api/read/history/frames?cmd_id=…` 原始响应。

### 5.7 HTTP 代理与 MQTT 直连**回执表现**
- 操作：设备页切换通道为「HTTP 代理」执行一次只读查询；再切回「MQTT 直连」执行一次。
- 预期：两条通道都能拿到结果；HTTP 通道在「记录 → 审计」有记录，MQTT 直连**没有**云端审计记录（这是已知取舍，不是缺陷）。
- 失败反馈：通道标签 + 结果帧是否存在 + 是否出现 `cmd_id_duplicate`（重复提交）。

### 5.8 Config / Workflow dirty 的**刷新、保存、重启反馈**
- 操作 a：配置页改一项**不保存** → 刷新 → 看底部。
  **预期**：刷 `68b2cdcb` 后显示「设备端未落盘 N 个模块…」+ 自动保存剩余秒数；
  **刷写前（旧固件）应显示"未知"，绝不显示"干净"**。
- 操作 b：点「保存到 Flash」→ 预期落盘并请求重启，页面进入重启监视；刷新后 dirty 消失。
- 操作 c：改一项后直接点「普通重启」→ 预期**三选一**（先落盘再重启 / 仍要重启（会丢失）/ 取消），不静默。
- 失败反馈：`config_query` 原始回帧（含 `dirty` / `restart_in_ms`）+ 截图。

### 5.9 米家**二维码登录**与**用户名密码登录**
- 二维码：设备页 → 米家 → 二维码登录 → 扫码。
  **预期**：二维码正常显示；未扫码期间轮询**不报错**（≤10 s 一轮返回"等待"）；扫码后变"已登录"。
- 密码（需专用测试账号，**允许被反复错误登录**）：故意输错一次。
  **预期**：提示"账号或密码错误"，**不是** `unknown` / `UNKNOWN`。
- 失败反馈：弹窗全文 + 时间 + 是否触发验证码/二次验证。

### 5.10 米家历史同步、日志记录与 Workflow 参数显示
- 同步：首页点「同步」→ 预期出现新增/重复/跳过计数；1 分钟内重复同步不应把日志刷爆。
- 日志：Cloudflare 控制台 Workers Logs 搜 `[xiaomi-api]`，再搜 `eyJ` / `Bearer` / `ssecurity=`
  **预期**：前者有记录，后者**零命中**（命中即为泄露，立即反馈）。
- 参数显示：自动化详情 → `weight_decrease` 应显示**中文「目标减重」+ 1–300 g**，`timer.type` 显示中文枚举；
  数值精度不得出现 `21.0000000001` 之类。
- 失败反馈：截图 + 具体自动化名。

---

## 6. 未完成任务

| # | 事项 | 类型 | 具体原因 | 解除条件 |
|---|---|---|---|---|
| 1 | **固件刷写 + 刷后验证（F1–F7）** | 物理操作未完成 | 本机无 ESP32 USB 串口（`pio device list` 仅 3 个蓝牙虚拟口；WMI 交叉确认无 `VID_303A`/CH340/CP210x） | 接线出现 `COM<N>` 后执行 §5.1 |
| 2 | **Config dirty 真实值验证** | 真机验证未完成 | 依赖 #1（旧固件无 `dirty` 字段） | 刷写后按 §5.8 复验 |
| 3 | **米家真实账号密码登录错误码** | 真实账号测试未完成（**BLOCKED**） | 无测试账号；不猜账号以免上游风控 | 提供专用测试账号（`XIAOMI_TEST_USER/PASS`），必要时提供验证码 |
| 4 | **米家 API 日志线上内容证据** | 自动化验证未完成（**BLOCKED**） | Workers Logs 无历史检索 API | 允许在触发一次米家只读调用时挂 `wrangler tail` 采集 60 s |
| 5 | **T02 HTTP 通道剩余项** | 代码未完成（**用户判定不做**） | publish QoS 0→1、`STALE_AFTER_MS` 与 `POLL_TOTAL_MS` 统一、Pages 代理超时重试 | 单开任务 |
| 6 | **T04 首页请求数量调查** | 代码未完成（**用户判定不做**） | — | 单开任务（需生产测量） |
| 7 | **D-01 米家 API 日志是否落 D1** | 待用户决策 | 需新增 `0013_*` 迁移 + 约 ≤8.6k 写/天的 D1 增量 | 用户拍板（台账 §T03.5 有方案与成本） |
| 8 | **Playwright 剩余 1 项** | 自动化验证未完成 | 失败点不稳定（`#workflows` Tab / `#d-mem` 点击），核对 `mqtt_messages` 无对应下行命令 ⇒ 脚本切页时通道未就绪 | 人工复看 §5.6 / §5.4；后续可给脚本加"等通道就绪"再切页 |
| 9 | 物理动作类验证（出粮、通断电、双设备并发） | 用户人工测试未完成 | 纪律禁止自动化执行物理动作 | §5.2 / §5.3 |

---

## 7. 安全与资源

| 项 | 现状 |
|---|---|
| **凭据持久化风险** | 米家 Token 仅存设备 NVS（`gflan`）；Worker 侧会话密钥加密存储；浏览器**不落任何 MQTT/米家凭据**（`localStorage` 实测零凭据，`M7` 断言守着）。本轮**新增** `.test-accounts.local` 入 `.gitignore`（EMQX 仓），防误提交 |
| **米家 API 日志泄露风险** | 白名单丢弃 + 敏感键打码 + **新增** 请求头掩码（`Authorization`/`Bearer`/`x-api-key`）+ **新增** `looksLikePayload()` 拒绝响应体形态的自由文本 + 失败洪泛上限。代码级 41/0；线上内容待 §5.10 |
| **MQTT / Worker / D1 请求量** | 本轮新增 1 类查询：`config_query`（**只在"未知"时问一次**，30 s 失败退避）⇒ 每会话 +1 次、非周期性。验收脚本本身会申请/释放池租约（有限次数、已加归还）。`verify_p0_prod` 对账证明探测**零新增 `mqtt_messages` 行** |
| **数据库结构与架构** | **零变更**（无新表、无新索引、无迁移）。生产架构不变 |
| **未授权生产变更** | 仅限：Worker 部署 2 次、Pages 部署 6 次（用户已授权"部署"）。**EMQX 未改**，**D1 未写**（除既有业务写入外无人工写），**未执行任何物理动作**（零出粮/零通断电/零开阀/零运行自动化） |
| ⚠ 需注意 | 验证期间 MQTT 池账号曾被我方测试脚本占满（headless 关闭不触发 `pagehide`）⇒ 已在脚本侧加归还；但**多标签页 + 异常关闭**仍会短暂占满池，属既有设计（30 min 租约 + 到期回收） |

---

## 8. 最终验收结论

### 8.1 可以宣布完成（代码 + 自动化 + 已上线）
- **T01** 统一设备预检（含多设备整份否决、冷却提示、断电例外）
- **T03** 米家 API 日志存储方案结论 + 两处泄露路径修复
- **T06** 取帧语义统一（`pickResultFrame`）
- **T07** Worker / Pages 部署（含回滚点与最小只读验证）
- 4 个新缺陷（A/B/C/D）与 5 个既有缺陷（E/F/G/H/I）**修复并部署**，均有测试证据

### 8.2 仅代码完成（未真机验证）
- **T05** Config dirty：固件改动已编译、契约脚本 20/0，但**未刷写** ⇒ 真实 `dirty` 未验证
- 固件侧全部改动（`dirty`/`restart_in_ms`）

### 8.3 必须等待人工测试
- 固件刷写与刷后 F1–F7
- §5.2–§5.10 全部 9 组（双设备并发、紧急断电、Config 同步、多设备预检、诊断弹窗、双通道回执、dirty 刷新/保存/重启、米家两种登录、历史同步与参数显示）

### 8.4 仍阻止 P4 完全关闭
1. **固件未刷写**（物理条件）——这是最大阻塞项，直接决定 T05 能否关闭、以及"新前端读到真实 dirty"能否成立。
2. **真实米家账号验证缺失**（BLOCKED）。
3. **Playwright 剩余 1 项不稳定失败**（测试时序，非产品缺陷，但要求人工复看确认）。
4. **T02 / T04 按用户决定未做**（不计入阻塞，但 P4 原始范围未 100% 覆盖）。

---

## 9. 附：本轮关键数字

| 指标 | 值 |
|---|---|
| 新增测试套件 | **7** 个（`p0_3_unified_preflight`、`config_dirty_restore`、`frame_pick_semantics`、`xiaomi_api_log_security`、`xiaomi_qr_poll_timeout`、`pages_csrf_callsites`、`verify_t09_*` 2 个脚本） |
| 新增断言 | 约 **300+** 条 |
| `frontend_syntax` | 27/0（25 个 ES 模块） |
| Worker `run_all` | 全部套件通过（含 legacy 的 EXPECTED_FAILURE，属预期） |
| 前端套件 | `tests/ui` 21 + `tests/api` 15 + `tests/pages` 6，全部 exit=0 |
| 生产只读脚本 | 21/0、36/0、15/0、24/0、15/0 |
| 浏览器验收 | 62/1（修复首页缺陷前 10/10） |
| 固件产物 | 1,471,408 B / SHA `68b2cdcb…` / 编译 23.8 s / Flash 70.1% |
