# Guo Feeder 项目交接书（HANDOVER）

> **最后更新**：2026-10-30 15:38（GMT+8）
> **权威性**：本文件是**当前状态快照**，优先于 `../../HANDOFF.md`（那份停在 2026-10-21，不含阶段 N/O）。
> **配套文件**（同目录）：`OPEN_ISSUES.md`（问题清单）· `SOURCE_INDEX.md`（资料索引）· `NEXT_SESSION.md`（开场任务书）。
> **路径约定**：`WS` = 工作区根 `D:\Guo_Feeder_Project`；正文路径均相对 `WS`。

---

## 1. 项目背景与总体目标

**Guo Feeder（果喂）** 是一套自建的宠物自动喂食 / 饮水设备系统，由三部分组成：

- **嵌入式固件**：ESP32-S3 N16R8（16 MB Flash / 8 MB PSRAM），PlatformIO + Arduino 框架，当前版本线 **v0.7**。
- **云端**：Cloudflare Worker（api）+ Pages（前端 + BFF）+ D1（SQLite），MQTT broker 用 EMQX Cloud。
- **外部设备**：小米米家喂食器、米家智能插座（局域网 miIO/UDP 直连）、米家温湿度计（BLE）。

**目标**：设备能本地自主运行（断网可用），联网时可远程查看状态、下发命令、记录历史；所有"成功/失败/未知"必须如实区分，不允许把"发出了"当"执行了"、把"测试通过"当"真机通过。

### 1.1 使用场景与硬约束

- **离线自治优先**：路由器断网或云端不可用时，本地定时喂食、按键操作、安全保护必须继续工作；云只负责"看"和"远程下发"，不参与本地闭环。
- **网络环境**：家庭 2.4 GHz Wi-Fi。设备与米家喂食器/插座必须处于**同一局域网**——miIO 走 UDP 直连，不跨网段、不跨 VLAN。
- **单设备上下文**：账号可绑定多台设备，但 UI 同一时刻只操作"当前设备"；设备身份由 MAC 派生（`device_id` = 12 位 hex）。
- **存储约束**：NVS 分区容量有限。配置写入必须"改 RAM → 校验 → save 落盘"三步；不落盘则重启丢失，且 `version` 不变（很多"配置没生效"由此而来）。
- **安全底线**：任何可能造成物理动作的命令（出粮、插座通断电）都要二次确认；结果未知时宁可让用户核对，也不自动重试。
- **可观测性**：设备端与云端都必须能回答"最后一次成功是什么时候、失败原因是什么"，而不是只给一个布尔值。

---

## 2. 系统架构与仓库职责

| 仓库 | 路径 | 职责 | 分支 / 远端 |
|---|---|---|---|
| 固件 + 文档 | `PIO_Assets/Guo_Feeder_Project` | ESP32 固件、构建脚本、架构与接口文档、证据归档 | `wb` → origin/wb |
| 云端 | `Cloudflare_Assets` | `guo-feeder-api`（Worker）、`guo-feeder-pagesdev`（Pages + Functions）、测试 | `main` → origin/main |
| EMQX 配置 | `EMQX_Assets` | ACL 规则、账号池、运维脚本 | `main` → origin/main（领先上游 3 个文档提交） |

**数据流（上行）**：设备 → MQTT `.../up`（遥测）/ `.../log`（日志）→ Worker（EMQX webhook / HTTP 兜底）→ D1 → Pages `/api/read/*` → 浏览器。

**数据流（下行）**：浏览器 → MQTT WSS 直连（默认）或 Pages `/api/command`（经 Worker）→ EMQX `.../down` → 设备 → `.../up` 回 result 帧 → 前端轮询/订阅判定终态。

**鉴权链**：浏览器拿到 Pages 的 `gf_session`（HttpOnly）→ Pages 服务端注入 `Bearer ADMIN_API_TOKEN` → Worker 只读端点 → D1。**Worker 零 CORS**，前端只能走 Pages 同源代理。

**MQTT 鉴权**：设备与浏览器各用独立账号池；ACL 按 topic 前缀（`gf_dev_*` / `gf_web_*`）授予 pub/sub。**路由鉴权必须按族前缀 + fail-closed**，逐条枚举会静默绕过。

### 2.1 Topic 结构与链路细节

- **设备侧**：上行 `guo_feeder/<device_id>/up`（遥测）与 `.../log`（日志）；下行 `.../down`（命令）。
- **Web 侧**：浏览器用 `gf_web_*` 账号，ACL 只允许 pub 到 `+/down`、sub `+/up` 与 `+/log`——即"能发命令、能看回执"，不能越权读其他设备。
- **QoS**：命令与回执用 QoS 1（至少一次），前端按 `cmd_id` 去重；遥测/日志允许 QoS 0/1，落库端做幂等。
- **断线重连**：WSS 断开后指数退避重连；重连成功必须重新订阅，否则会出现"显示在线但收不到回执"。
- **双通道**：默认 WSS 直连（不经 Worker）；HTTP 代理经 Worker 有审计。二者在 UI 上必须分别标注，不能都写成"已连接"。

---

## 3. 固件核心模块与架构约束

### 3.1 模块划分（`src/`）

| 层 | 目录 / 文件 | 职责 |
|---|---|---|
| 服务 | `services/config_manager.*` | NVS 键值配置，**只写 RAM 不落盘则 version 不变** |
| 服务 | `services/command_manager.*` | 命令解析/分发、result 帧、非幂等保护 |
| 服务 | `services/system_state.*` | 全局状态中心（含米家诊断键） |
| 服务 | `services/telemetry_publisher.*` | 遥测生成/发布（受**编译门 + 运行时门**双重控制） |
| 服务 | `services/cred_store.*`、`device_identity.*` | 凭据与设备身份（`device_id` = MAC 12-hex） |
| 自动化 | `automation/workflow.*` | 非阻塞状态机、定时/条件触发 |
| 应用 | `app/xiaomi_lan.*` | 米家局域网 miIO（喂食器 / 插座） |
| 应用 | `app/MiThermometer.*` | BLE 温湿度采集 |
| 应用 | `app/stepper.*`、`valve.*`、`vibration.*`、`weight.*`、`safety.*` | 执行器与安全 |
| 云 | `cloud/*`、`log/*` | MQTT 连接、日志 |
| 存储 | `storage/*` | 分区 / JSON 存储 |

### 3.2 硬性架构约束（违反会出事故）

1. **职责单一**：ConfigManager 只管配置、CommandManager 只管命令、WorkflowManager 只管调度、CloudManager 只管链路。不得越界互调。
2. **非幂等动作禁止自动重试**：出粮、插座通断电一旦"结果未知"，只上报未知，绝不自动重发。
3. **result 帧终态**：`accepted / running / pending / queued` 都是**非终态**，只有 `s=0`（或明确的失败码）才是终态。
4. **重启唯一证据 = `boot_seq` 递增**；`presence` 全程 online，不能据此判断重启。
5. **开串口会复位设备**（DTR 拉低，`rst:0x1`）⇒ 打开串口后立刻读 state 是"观测手段破坏被观测对象"。
6. **遥测双重门禁**：编译门 `GF_TELEMETRY_ENABLED`（默认 0）+ 运行时 `mqtt.telemetry_enable`（默认键不存在）。只改运行时零效果。
7. **判空写法**：`x ? Number(x.y) : NaN`，不能用 `Number(null)`（`===0` 会误判）。
8. **在线状态查询禁止串行 await**：必须并发 + 总预算，超时标 unknown。
9. **`pio run` 会自动删 `.pio/build`**：`project.checksum` 变化（改 ini / 增删源文件）即 `rmtree`。改配置或加文件后，构建前必须先归档产物（`tools/archive_build_artifacts.sh` 已挂进 `build_telemetry.sh`）。
10. **UI 三态分离**：通道状态 / 设备在线 / 查询结果必须分开表达；`undefined` 不得静默；离线不得渲染成"已断电"。

### 3.3 启动顺序与调度模型

1. **启动**：NVS 初始化 → 配置加载（`device_id`、Wi-Fi、MQTT 凭据）→ 网络连接 → BLE/米家模块初始化 → 遥测任务（仅当编译门开启）→ 进入主循环。
2. **非阻塞**：主循环不允许长时间 `delay()`；BLE 扫描、UDP 请求、MQTT 重连都拆成状态机 + 时间片。
3. **BLE 节奏**：启动延迟约 15 s；扫描窗口 15/30 min；窗口内 1 s 开、1.8–2.3 s 关；休息 48–60 min；连续 4 个窗口失败则降级并最终停用。
4. **资源**：DRAM 阈值以 `< 85%` 为准（分母 `internal.total`、per-boot 语义）；大缓冲放 PSRAM，不放栈。
5. **复位语义**：`boot_seq` 每次启动自增并上报；`reset reason=power_on` 在打开串口（DTR）时也会出现，不能当作设备自发重启。

---

## 4. Cloudflare / D1 / EMQX / UI 基本架构

### 4.1 Worker（`Cloudflare_Assets/guo-feeder-api`）

- 入口 `src/index.js`；只读 API `src/read-api.js`；命令 `src/command-api.js`；设备注册 `device-registry.js`；历史入库 `history-ingest.js`；米家同步 `src/xiaomi/`。
- 关键迁移：`migrations/0003_p3_history_tables.sql`（遥测/日志历史）、`0008_ui_command_audit.sql`（命令审计）、`0011_xiaomi_history.sql`、`0012_xiaomi_auth_pending.sql`。
- D1 库名 `YOUR_D1_DATABASE_NAME`（id `632b1224…`）。
- 入站校验 `ingest-validate.js` 当前为 `observe`（只计数不拒绝）。

### 4.2 Pages（`Cloudflare_Assets/guo-feeder-pagesdev`）

- 静态页 `app.html` + `assets/js/`：`gf-app.js`（主应用/路由/命令）、`gf-xiaomi.js`（米家）、`gf-ui-core.js`（配置卡/System State）、`gf-mqtt.js`（WSS 直连）、`gf-api.js`、`gf-cache.js`。
- Functions：`functions/api/**`（auth / catalog / command / history / mqtt / presence / read 代理 / webhook / xiaomi）。
- **部署必须用 `tools/deploy_pages.sh`**：`wrangler pages deploy .` **不读** `.assetsignore`，曾把 `/.dev.vars`、`/.wrangler/`、`/test/` 一起上传造成泄露。白名单暂存部署已验证 `/.dev.vars` 不再可匿名下载。
- `ADMIN_API_TOKEN` 绝不能进前端；中间件默认放行，只保护 `/`、`/app`、`/app.html`。

### 4.3 EMQX（`EMQX_Assets`）

- `config/acl-rules.v3.json`、`acl-rules.proposed.json`；脚本 `scripts/apply-web-acl.mjs`、`audit-web-acl.mjs` 等。
- **API 封装 `request()` 返回 `{ ok, status, data }`**，不是 `.body`；按 `.body` 读会得出"0 条规则"的**假阴性**（已踩）。
- 账号池：设备侧与 Web 侧分离；`gf_web_*` 每账号 3 条规则（pub `+/down`、sub `+/up`、sub `+/log`）。

### 4.4 米家局域网（miIO）

- 纯函数解析在 `src/app/xiaomi_lan_parse.h`；动作在 `xiaomi_lan.cpp`。
- NVS 键**按设备隔离**：喂食器 `f_ip / f_tok`，插座 `p_ip / p_tok`（真独立，写入即生效）。
- 只读动作：`XIAOMI_FEEDER_STATUS`、`XIAOMI_PLUG_STATUS`；控制：`XIAOMI_FEED`、`XIAOMI_PLUG_ON`、`XIAOMI_PLUG_OFF`、`XIAOMI_PLUG_ON_60S`。
- 设备端 60 秒普通通电保护**不依赖浏览器**；强制通电无自动断电；断电始终可用。
- 小米云响应带 `&&&&START&&&&` 前缀，`.json()` 必抛，需先剥离。

---

## 5. 开发路线图与阶段完成情况

| 阶段 | 时间 | 主题 | 状态 |
|---|---|---|---|
| A–I | 10-27 ~ 10-28 | 云端骨架、设备注册/绑定、登录、米家同步、LAN 固件、UI 控制 | 已交付，多为历史基线 |
| J–M | 10-29 上午 | 增量同步、任务台账、BLE/遥测取证、部署、人工验收 | 已交付（报告在 `docs/reports/`） |
| N | 10-29 10:36–12:45 | BLE 长周期验证、遥测断点定位、UI 响应式、部署冒烟 | 已交付；纠正了"M 阶段 BLE 从未成功"的误判 |
| **O** | 10-29 16:19–18:20 | **WP-A 遥测恢复**、WP-B 米家双设备、WP-C 状态查询 | **已交付（真机烧录 + D1 证据）** |
| O 收尾 | 10-29 20:21–21:05 | WP-D 时间线更正、WP-H auto-clean 根因、WP-F/G 三态分离与部署 | 已交付 |
| O 续接 | 10-30 14:44–15:04 | 浏览器测试 `ui_responsive_n.mjs` 修复至全绿 | **已交付（仅测试与 1 处反模式修复）** |

### 5.1 几条必须记住的阶段结论

- **遥测断更的真因**不是"MQTT 坏了"：`telemetry_task()` 被编译门 `GF_TELEMETRY_ENABLED` 包住且默认 0，`platformio.ini` 从未定义该宏，运行时键也不存在。二者叠加 ⇒ 设备照常上报 `log`，但 `up` 遥测零条。
- **"BLE 从未解密成功"是误判**：打开串口会复位设备并清空状态，造成"读到 nan"的假象；云端 `log_history` 的 2305/2307 事件证明采集实际成功。
- **`.pio/build` 丢失**是 PlatformIO auto-clean（checksum 变化即 `rmtree`），不是人为误删；已用归档脚本兜底。
- **状态查询失败**的根因在前端静默吞错，不在设备/协议/broker；真机帧 `s=0` 已排除链路问题。
- **测试数量漂移**（121/134/146/178）是用例持续新增，不是测试失效；引用必须注明"哪一轮"。

---

## 6. 用户已明确批准的设计与决定

1. **遥测可复现构建**：新增 `[env:esp32-s3-telemetry]`（extend 默认 env + `-DGF_TELEMETRY_ENABLED=1`），默认 env 保持零开销。这是已批准的方案。
2. **运行时开关必须落盘**：`mqtt.telemetry_enable` 需 `set → save → 复位后仍为 true`。
3. **构建产物必须立即归档**：`tools/archive_build_artifacts.sh` 已固化。
4. **Pages 只走白名单部署**：`tools/deploy_pages.sh`。
5. **米家双设备各自独立** IP/Token/启用开关；UI 分两张配置卡。
6. **三态分离**：查询通道 / 设备在线 / 查询结果分列；插座离线不显示"已断电"。
7. **非幂等动作二次确认 + 结果未知不重试**。
8. **断电始终可点**（即使状态未知），普通通电 60 秒自动断电由设备端计时。

---

## 7. 用户明确禁止的架构改动与操作

1. **不得伪造**心跳、遥测、D1 数据、命令结果。
2. **不得把**"本地测试通过"当"生产验证"，"设备在线"当"下行正常"，"发布成功"当"已执行"，"已实现"当"已部署"。
3. **不得破坏** ConfigManager / CommandManager / WorkflowManager / CloudManager 的职责边界。
4. **未经明确授权，不得** `commit`、`push`、烧录固件、部署 Worker/Pages、修改 EMQX 生产配置。
5. **未经逐次授权，不得**执行真实出粮、插座通断电。
6. **不得读** `MiLAN_Test/secrets` 等凭据文件。
7. **不得因普通代码问题包装成用户裁决**（能自己修的 bug 直接修）。
8. **客数事项已彻底放弃**，不再实现。
9. `xiaomi_plug_stat` 表虽存在但 Worker 零引用；单位语义未定前**不擅自实现**。

---

## 8. 当前三仓代码 / 部署 / 固件基线

> 采集时间：2026-10-30 15:38。所有数字为本轮实测，非历史引用。

| 项 | 值 |
|---|---|
| PIO 仓库 | `wb`，HEAD `cb7a1c7`，**dirty 26** |
| CF 仓库 | `main`，HEAD `3db8f56`，**dirty 55**（本轮新增 2 个文件改动：`gf-xiaomi.js`、`ui_responsive_n.mjs`） |
| EMQX 仓库 | `main`，HEAD `05434ad`，**ahead 3 / dirty 2**（2 个未跟踪 ACL 脚本） |
| 固件 | **`71556b9c315c9f06…`**，**1,467,008 bytes**，含遥测；`verify_flash` = "verify OK (digest matched)" |
| 运行时遥测开关 | `mqtt.telemetry_enable = true`，**已落盘**（复位后仍 true） |
| 线上 Pages | **`778a6768`** |
| 线上 Worker | **`YOUR_RESOURCE_ID`** |
| D1 | `YOUR_D1_DATABASE_NAME`，无待应用迁移 |
| 本轮 git 操作 | **未 commit · 未 push** |
| 本轮设备操作 | **未烧录 · 未部署 · 未执行真实出粮/通断电** |

### 8.1 测试基线（2026-10-30 实跑，非历史引用）

| 套件 | 结果 |
|---|---|
| `tests/pages/ui_responsive_n.mjs` | **142 PASS / 0 FAIL** |
| `tests/pages/xiaomi_ui.test.mjs` | **178 PASS / 0 FAIL** |
| `tests/pages/ui_app_runtime.test.mjs` | **134 PASS / 0 FAIL** |
| `guo-feeder-api/test/run_all.mjs` | **全部套件通过**（含 legacy `EXPECTED_FAILURE`，属预期） |

> ⚠️ 以上是**自动化 / 浏览器层**结果，**不等于**真实米家设备查询与控制通过。验收标准见 `OPEN_ISSUES.md` 的 GF-MI-01/02/05/06/07。

⚠️ **线上 Pages `778a6768` 不包含**：阶段 O 后续的 UI 改动（控制动作、9 键状态、三态分离细节）与本轮 `gf-xiaomi.js` 修复。这些**都在工作区、未部署**。

---

## 9. 阶段 O 的真实完成情况

### 9.1 WP-A 遥测端到端打通（唯一"真机验证"的成果）

- 构建：`tools/build_telemetry.sh` → 1,467,008 B，sha256 `71556b9c…`，构建后**断言**二进制含 `climate.temperature`。
- 运行时：`tools/gf_telemetry_enable.py` set → save → 回读 True → **复位后仍 True**。
- 烧录：COM8，MAC `28:84:85:89:6c:e4`（设备标识，非凭据），`write_flash 0x10000` + `verify_flash` 通过。
- 落库：16:35:27 窗口开始 → 16:38:46 `temp + humidity received` → **16:39:59 落库 22.3 °C / 51.1 %**；节拍 **climate 5 min / weight 15 min**，seq 连续。
- 证据目录：`PIO_Assets/Guo_Feeder_Project/docs/evidence/Telemetry-Flash-2026-10-29/`。

### 9.2 WP-B 米家双设备

- NVS 键真独立：`f_ip/f_tok` vs `p_ip/p_tok`；写入即生效。
- UI：两张配置卡（喂食器 / 插座），各自 enable、IP、Token、保存、显隐；Token 为 `password` 且不落 localStorage。
- 每台设备各自"最近读取"时间，刷新一台不清另一台。

### 9.3 WP-C 状态查询

- 根因：**前端静默吞错**（`queryState` 在 `r.error` 时返回 undefined，UI 空 catch）⇒ 用户只看到"点了没反应"。
- 踩坑：EMQX API 封装返回 `{ok,status,data}`，按 `.body` 读得出"0 条规则"的假阴性。
- 真机帧验证：经 EMQX API 下发 `query_state`，设备回 `{"s":0,"c":"result",...}` ⇒ 设备/协议/broker 正常。
- 修复：`queryState` 失败抛错；控制区**持久显示**失败原因 + 上次查询时间。

### 9.4 真机只读查询结果（阶段 O 收尾）

| 设备 | 结果 |
|---|---|
| 米家喂食器 | `xiaomi_feeder_online = true`，`xiaomi_feeder_out_status = 0` |
| 米家智能插座 | `xiaomi_plug_online = false`，`xiaomi_plug_power = false` |

⚠️ 插座 `false` 的**物理原因未确证**（可能是断电、断网、Token 错、IP 变）⇒ 不猜，列 OPEN。

### 9.5 事故与修复

- `.pio/build/` 历史产物被清理（根因 = PlatformIO auto-clean，源码 `clean_build_dir()` 证实），7 个 env 产物不可恢复。
- 已新增归档脚本并挂进构建流程；`docs/evidence/build-archive/` 有 2026-10-29 的归档。

---

## 10. 当前最重要的问题（按优先级）

1. **插座局域网 offline 根因未定**（GF-HW-01 / GF-MI-02）：需在设备侧做只读探测，区分"物理断电 / 不在同一网段 / IP 变 / Token 错 / 设备停用"。
2. **新 UI 未部署**（GF-DEP-01）：线上 `778a6768` 缺少阶段 O 后续改动，需用户授权部署 + 登录后目视验收。
3. **生产登录后遥测目视**（GF-UI-03）：Turnstile 无法自动化，需人工。
4. **V-1~V-8 真机定时项**（GF-V-01）：需烧录 + 逐包批准，BLOCKED。
5. **真实出粮 / 插座通断电**：从未真机执行，非幂等风险高，需逐次授权。

---

## 11. 下一阶段建议执行顺序

1. **交接核实**：读本目录 4 份文件 → 核对三仓 `git status` 与线上 `deployments status` → 确认基线未漂移。
2. **（需授权）部署 Pages** → 线上登录后目视：首页温湿度 ≈ 22.x / 52.x、控制页三态、米家两卡。
3. **（只读）插座 offline 调查**：设备侧 `xiaomi_lan` 诊断键（`*_last_err`、`recv/badsum/timeout`）、同网段连通性、IP/Token 回读；**不改生产配置**。
4. **（需逐次授权）真实出粮 / 通断电**：每次先确认二确认弹层、限时通电、结果未知不重试。
5. **（需烧录 + 逐包批准）V-1~V-8**。
6. 文档与测试补齐（`OPEN_ISSUES.md` 逐项推进）。

---

## 12. 新对话必须遵守的工作方式

1. **先核实，再动手**：第一轮只做状态确认，不写代码。
2. **真实证据优先**：结论必须能指向 D1 行、串口行、HTTP 响应、`verify_flash` 输出或文件哈希。
3. **六态标注**：`IMPLEMENTED` / `AUTOMATED_TEST_PASS` / `DEVICE_VERIFIED` / `UI_VERIFIED` / `NOT_RUN` / `BLOCKED`，不得混用。
4. **测试通过 ≠ 真机通过**：浏览器自动化只证明 DOM/交互层；米家查询/控制必须真机复验。
5. **不重复已完成**：先读 `OPEN_ISSUES.md`，已有证据的项不要重跑。
6. **保护工作区**：PIO dirty 26 / CF dirty 55 / EMQX ahead 3 dirty 2 是**有价值未提交改动**，禁止 `reset / clean / checkout`。
7. **阻塞不终止**：一项 BLOCKED 时继续推进其他独立、已授权的工作。
8. **最终报告逐项 PASS / FAIL / BLOCKED + 证据**，不允许只给"已完成"三个字。
