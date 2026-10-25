# 阶段四（P5 多设备）交付与验收 —— 2026-10-25

> 对应任务书 **C 段**。A/B 段见 `docs/review/UI-Stage3-Deploy-and-RealDevice-Write-Acceptance-2026-10-25.md`。
>
> 一句话范围：**可做、已做的** = 有权设备**清单可见**、可切换、设备级数据/请求/响应/缓存**隔离**、会话鉴权边界不变。
> **没做、也没假装做了的** = 跨设备**数据权限隔离**、别名跨端同步（见 §6，这是本项目当前部署形态的客观限制，不是偷工）。

---

## 0. 结论速览

| 层 | 结论 | 数字 |
|---|---|---|
| 实现 | ✅ | Worker 新增 `/api/ui/devices`；Pages 设备清单页 + 切换 + 隔离 |
| 自动化测试 | ✅ | `tests/api/ui_devices.test.mjs` **22** · `tests/ui/ui_stage4_devices.test.mjs` **55** · `read_proxy_allowlist` **28** |
| 真实浏览器（模拟多设备） | ✅ | `tests/e2e/stage4_devices_playwright.mjs` **19 PASS / 0 FAIL** |
| 生产只读验收 | ✅ | `scripts/verify_stage4_prod.mjs` **33 PASS / 0 FAIL / 4 NOTE** |
| 阶段三生产回归 | ✅ | 阶段四部署后复跑 **`49/49`** 仍全绿 |
| **生产多设备实测** | 🚫 **未做** | 生产**只有 1 台**真实设备 ⇒ 切换/隔离在生产**没有真实验收**，只有模拟 e2e。**不得**据此声称"生产多设备已验收" |

---

## 1. 范围核对（按任务书要求：先看架构文档 + 唯一进度表 + 实际代码）

| 来源 | 关于多设备的现状 | 本阶段处置 |
|---|---|---|
| 架构文档 `P2-0-...md` §20 **D-7** | 「多设备发现（一个用户多台）」目标阶段 P4/P5，理由：需按用户绑定下发**多组** ACL | UI 侧清单/切换**已交付**；**多组 ACL** 一条没动（见 §6 限制 3） |
| 唯一进度表 §8 **R26** | 🟡 阶段二只做异步隔离；列表/切换/跨端别名未做 | → 更新为 ✅ 列表/切换/本地别名已交付；**跨端别名未做** |
| 实际代码 | 原清单只有 `/api/admin/user/:userId/devices`（管理令牌、无在线状态、UI 会话里根本没有 `user_id` 维度） | 新增会话侧 `/api/ui/devices` |

---

## 2. 做了什么

### 2.1 后端（Worker）

- 新增 `src/ui-devices.js`：`GET /api/ui/devices`
  - **可见范围由后端决定并自陈**：响应带 `scope` / `binding_based`，前端**不**自己推断"我该看到哪些设备"；
  - **在线状态三态**：`online` / `offline` / **`unknown`**。`unknown` 映射为 `online: null`，**绝不降级成 `offline`**（未知 ≠ 离线，本项目一贯纪律）；
  - **字段白名单**：只下发 7 个设备字段（`project()`），无任何凭据/令牌/内部列；
  - **别名存储方式如实标注**：`alias_storage: "local_only"` —— 不让人以为后端已经存了；
  - 无 D1 迁移、无 EMQX 变更、无 topic/协议/设备身份改动。
- `index.js`：挂在 `/api/ui/**` Bearer 组，**仅 GET**。

### 2.2 前端（Pages）

- 只读代理白名单加 `/^ui\/devices$/`，**且只这一个精确路径**：不放行 `ui/command`（写出口）、`ui/session`、`ui/mqtt/credentials`（凭据出口）。
- `gf-ui-core.js`：`deviceListView()`（含"在线/离线/**在线状态未知**"三态渲染）、`deviceListEmptyView()`、`DEVICE_SWITCH_CLEAR`（32 个设备级键；`catalog` **不**在其中 —— 它不随设备变）。
- `gf-app.js`：`loadDevices()` / `switchDevice()` / `renderDevices()` / `aliasOfDevice()`、`#devices` 路由、设备栏「切换」入口。
- 切换动作 = **停轮询 + 增代际 + 清缓存**三步（缺一会出现"切了设备但页面还是上一台的自动化/配置/日志"）。
- `runCommand()` 记下"命令发给哪台设备"（`dev0`），回包时若已切走 ⇒ **整份丢弃**（陈旧响应不写回）。

---

## 3. ★ 真实浏览器测试抓到的**真缺陷**：X16 boot 顺序

这是本轮**最有价值**的一条 —— 它不是测试桩问题，是代码 bug。

| 项 | 内容 |
|---|---|
| 现象 | e2e **D-6b** 失败：对"已不在可见清单"的设备发出了 **4 条命令**（`registry_action`、`query_state`、`query_workflows`、`query_state`） |
| 根因 | boot 里 `loadDevices()` 是 **fire-and-forget**。它判定"当前设备不可见"后只是把 `location.hash` 改成 `#devices`，而 **boot 不等它** ⇒ 后面的 `loadRegistry()` / 状态 / workflow 早已按**旧设备**发出去了。**跳到清单页只是换了个页面，命令照样落地。** |
| 为什么危险 | 页面明明已经知道"这台设备对当前账号不可见"，却仍然对它下发了命令 —— 与"不对无权设备发命令"的目标正好相反 |
| 修复 | ① boot 改为 `await Promise.race([loadDevices(false), 4s 超时])`（限时是为了清单端点挂起时不把启动卡死）；② 设备级加载受 `deviceVisible()` 门禁；③ **`runCommand()` 唯一出口**再拦一次（页面里还有定时刷新/用户点击/路由渲染多条路径，与其每处加 if，不如拦在出口） |
| 为什么"清单未加载"算放行 | `deviceVisible()` 里 `!devicesLoaded` / 空清单返回 **true**。这是**"不知道"**，沿用既有"端点不可用退化"行为；写成 `false` 会让清单端点一抖动整个页面就瘫，是更糟的失败模式 |
| 回归锁 | `tests/ui/ui_stage4_devices.test.mjs` M-13a..M-13g（7 条源码契约）+ e2e D-6b |

---

## 4. 验收明细

### 4.1 真实 Chromium（模拟多设备）—— 19 PASS / 0 FAIL

`tests/e2e/stage4_devices_playwright.mjs`，3 台模拟设备（在线 / 离线 / **未知**各一）+ 1 台"不在清单里"的幽灵设备：

- D-1 清单页 3 台 + 三态各就各位；**未知那台没被显示成离线**；如实标注"单用户部署""别名不会跨端同步"
- D-2 别名只出现在该设备卡片上（按 `device_id` 隔离）
- D-3 切换后 `localStorage` 与设备栏同步；不再显示上一台的别名
- D-4 ★ 切换后**上一台的配置缓存被清空**（回到"尚未读取"）
- D-5 ★ 切换后发出的命令 `device_id` = 新设备
- D-6 ★ 当前设备不在清单 ⇒ 提示并跳清单页；**全程零条命令发给无权设备**
- D-7 清单走 `/api/read/ui/devices`；**浏览器全程零次直连 Worker**
- D-8 零未捕获异常

### 4.2 生产只读 —— 33 PASS / 0 FAIL / 4 NOTE

`scripts/verify_stage4_prod.mjs`：

- ★ `/api/ui/devices` 200 且形状合规；`scope` 自陈、`binding_based` 布尔、`alias_storage=local_only`
- ★ 不带令牌 ⇒ **401**（鉴权边界未因阶段四放松）
- ★ 只下发白名单字段；**不含任何凭据类字段名**，不含出厂凭据字面量
- ★ presence 三态严格（`online` 布尔与 `state` **一一对应**；`unknown` ⇒ `online: null`）
- ★ 清单里的在线状态与**权威端点** `/api/admin/presence/emqx/<id>` **一致**（清单不许说谎）
- ★ 生产 JS 产物含 `loadDevices` / `deviceVisible` / `switchDevice` / `deviceListView` / `DEVICE_SWITCH_CLEAR`，且 **hash 与本地一致**（证明部署的就是这份代码）
- ★ 写通道门禁**仍关闭**：目录 `enabled=false`；写命令 **403** 且**未产生 `cmd_id`**
- ★ 未新增 D1 迁移（最新仍 `0010`）；D1 绑定未变
- 阶段一/二/三回归：命令/日志端点 200、目录含配置读/写项、规格表 10 模块 + 66 键

**4 条 NOTE（如实记录，不算 PASS）**：

1. 在线状态分布 `["online"]`（只有 1 台）。
2. ★ **生产仅 1 台真实设备** ⇒ 多设备切换 / 跨设备隔离**生产未实测**。
3. `app.html` 在会话之后（无会话取到空体）⇒ 沿用阶段三处理，前端以 JS 产物 hash + 符号为据。
4. **白名单不可在生产自证**：Pages 代理的**会话门禁早于白名单**，无会话时无法区分"命中白名单"与"被白名单拒绝" ⇒ 以单测（28 PASS）+ 部署 commit 为据。

### 4.3 全量回归

| 层 | 结果 |
|---|---|
| 单测 31 套件 | **全 0 FAIL**（`ui_security` 275、`pages_auth` 107、`ui_app` 196、`wfadmin` 65 …） |
| Chromium e2e | stage1 **47** / stage2 **38** / stage3 **30** / **stage4 19** / stage5 **22** / mqtt_browser_playwright **27** = **183 PASS / 0 FAIL** |
| `mqtt_browser_e2e` | **守门触发退出**（G-0c：生产池 `gf_web_00..04` 已存在即主动退出）。这是一次性 bootstrap 脚本的**设计行为**（轮换会让 D1 密文与线上不一致），**不是回归**，也不应靠删生产池账号来"修绿" |

---

## 5. 部署记录

| 项 | 内容 |
|---|---|
| 部署前 | Worker `fdfddd54`（阶段三 + 门禁关闭）／ Pages `fa8e6f9d`（阶段三） |
| **阶段四 Worker** | `5a5513f2-d386-434a-910b-3f0fe08de4b2`（回滚目标 `fdfddd54`） |
| **阶段四 Pages** | `79199104-d6c1-42f3-a541-210aa7134b12`（回滚目标 `fa8e6f9d`） |
| 差异范围 | Worker 2 文件（`ui-devices.js` 新增、`index.js`）+ Pages 4 文件；**零 migration、零 EMQX 变更、零新密钥** |
| 提交 | 云仓 `3f7fe90`（代码）· `6acf00c`（真机证据文档） |
| 部署后回归 | 阶段四生产只读 33/33；**阶段三生产只读复跑 49/49** |

---

## 6. 限制（如实说明，不做过度承诺）

| # | 限制 | 原因 / 后果 |
|---|---|---|
| 1 | **单用户部署**：`UI_USER_ID` 未配 ⇒ 返回全部 `REGISTERED` 设备，响应里用 `scope:"single_user_all_registered"` + `binding_based:false` **明说**这是降级 | 这不是"按用户过滤过的有权列表"。配了 `UI_USER_ID` 才会走 `device_binding` 严格归属 |
| 2 | **别名仅浏览器本地**（`alias_storage:"local_only"`，键 `gf.alias_<device_id>`） | **不跨端、不跨浏览器**。要跨端需后端存储 ⇒ 需新增 D1 迁移，本阶段**未获批**任何 D1 变更，故未做 |
| 3 | **共享 MQTT 设备账号**（`GuoFeederDevice`） | 架构文档 D-7 要求的"按设备下发多组 ACL"**未做**。当前所有设备共用同一账号 ⇒ **按设备隔离 ACL 事实上不成立** |
| 4 | **生产只有 1 台真机** | 多设备切换 / 跨设备隔离**没有生产实测**，只有模拟 e2e（19/19）。**不得**把模拟结果写成生产多设备实测 |
| 5 | 会话级页面（设备清单页本身要登录后才能看） | 生产有 Turnstile ⇒ **自动化拿不到会话**，页面级人工验收未做（同阶段三 R1 的处理） |
| 6 | 在线状态依赖 EMQX | 取不到时给 `unknown`（**不给"离线"假值**）；单台上限 20，超出给 `presence:null` + `over_cap` |

> ★ 因此本阶段**不得**被描述为「跨设备数据与权限隔离」。它交付的是：**清单可见 / 可切换 / 前端层面的设备级隔离 / 不对不可见设备发命令**。权限隔离需要限制 1 与 3 一并解决。

---

## 7. 未做（不得当作已完成）

- 跨设备别名同步（需 D1 迁移，未获批）
- 按用户绑定（需 `UI_USER_ID` + 绑定数据）
- 按设备隔离 ACL（架构 D-7，需 EMQX 变更，本任务书明令禁止）
- 生产多设备真实验收（需 ≥2 台真机）
- 设备清单页的**会话级人工验收**（Turnstile）

---

## 8. 证据索引

| 主题 | 位置 |
|---|---|
| 后端 | `Cloudflare_Assets/guo-feeder-api/src/ui-devices.js` · `src/index.js` |
| 前端 | `guo-feeder-pagesdev/assets/js/gf-app.js`（`loadDevices` / `switchDevice` / `deviceVisible` / `renderDevices`）· `gf-ui-core.js`（`deviceListView` / `DEVICE_SWITCH_CLEAR`）· `app.html`（`#page-devices`） |
| 代理白名单 | `guo-feeder-pagesdev/functions/api/read/[[path]].js`（`ALLOWED`） |
| 测试 | `tests/api/ui_devices.test.mjs`(22) · `tests/ui/ui_stage4_devices.test.mjs`(55) · `tests/e2e/stage4_devices_playwright.mjs`(19) · `tests/pages/read_proxy_allowlist.test.mjs`(28) |
| 生产验收 | `scripts/verify_stage4_prod.mjs`(33) · `scripts/verify_stage3_prod.mjs`(49 回归) |
| 三态来源 | `guo-feeder-api/src/presence-emqx.js`（`state: online\|offline\|unknown`） |
