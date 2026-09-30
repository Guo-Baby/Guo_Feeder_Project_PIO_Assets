# 云端 + 安卓端平台选型与本地工具部署方案

> 状态：**规划稿 v1（2026-09-30）**，未改动任何固件代码。
> 目标架构：设备侧 Homie 5 自描述（复用 Capability Registry） + 前端 MQTT-Tiles/Crouton 改版托管 Cloudflare Pages + EMQX WSS 8084 直连 + Worker 仅负责鉴权/绑定/临时凭据 + D1 经 EMQX 数据集成落历史。
> 本文回答两件事：**① 写服务端 / 写安卓端各需要什么平台；② 本地（优先 D 盘）要装哪些工具。**

---

## 0. 结论速览（TL;DR）

| 层 | 选型 | 是否需自建服务器 |
|---|---|---|
| 设备↔云 传输 | EMQX Cloud Serverless（**已有**） | 否（托管） |
| 设备侧能力描述 | **Homie 5** 映射层，复用 `capability_registry` | 否（固件内） |
| 前端（Web / 安卓壳） | **MQTT-Tiles**（MIT，Vue/Quasar）改一版 | 否（Pages 静态托管） |
| 前端托管 | Cloudflare Pages（**已有**基础页面） | 否 |
| 鉴权 / 绑定 / 临时凭据 | Cloudflare Worker（**已有**骨架） | 否 |
| 历史数据 | Cloudflare D1（**已有**）+ EMQX 数据集成（HTTP） | 否 |
| 安卓端 | WebView + MQTT.js 壳 → **或** 原生 Kotlin + HiveMQ Client | 否 |

**一句话**：你**不需要**租任何 VPS、不需要 Docker、不需要自建 broker。全部使用托管服务 + 本地只装开发工具。整条链路（EMQX → Worker → D1）你已经有可跑通的原型（`readme.md` §「已实现」）。

---

## 1. 先确认三个"物理约束"（决定架构，不能绕）

这三条是本次联网核实的硬事实，直接否决了若干备选方案：

### 1.1 EMQX Serverless 只开两个端口

| 协议 | 端口 | 可用性 |
|---|---|---|
| MQTT over TLS (`mqtts`) | **8883** | ✅ 设备用 |
| MQTT over WebSocket + TLS (`wss`) | **8084** | ✅ 浏览器/APP 用 |
| TCP 1883 / WS 8083 | — | ❌ **Serverless 不支持** |

- WSS **完整 URL 必须带路径**：`wss://<部署域名>:8084/mqtt`
- **必须提供 SNI**，否则被拒（错误码 `-5`）
- Serverless **强制 TLS**，无明文选项 → 本地调试也必须走 `mqtts://` 或 `wss://`
- CA 证书：EMQX 提供，已存在于仓库 `data/emqxsl-ca.crt`
- 引用：<https://docs.emqx.io/en/cloud/latest/deployments/port_guide_serverless.html>

> **影响**：浏览器/安卓端**不能**用 `ws://` 裸连接，必须 `wss://`，且端口是 **8084 不是 8083**。MQTT-Tiles 支持任意 WSS broker，所以可用。

### 1.2 Cloudflare Worker 免费档 10ms CPU / 请求

| 项 | 免费档 |
|---|---|
| 请求数 | 100,000 / 天（全部 Worker 合计，UTC 00:00 重置） |
| **CPU 时间** | **10 ms / 请求** |
| 内存 | 128 MB / isolate |
| 子请求 | 50 / 请求 |
| **外部网络等待** | **不计入 CPU 时间** ✅ |

关键点：**等待外部 HTTP 响应（`fetch()`）不计 CPU**。官方原文：*"Waiting on network requests (such as fetch() calls, KV reads, or database queries) does not count toward CPU time."*

> **影响**：Worker 里"转调 EMQX REST API / 查 D1"这类**以 IO 为主**的操作**完全可行**（IO 等待不烧 CPU）。但**绝不能在 Worker 里压缩/解析大 JSON**、不能做长时间循环——那会计入 10ms 并报 `Error 1102`。⇒ Worker 只做"薄"逻辑：鉴权、签发临时凭据、D1 简单读写。
> 引用：<https://developers.cloudflare.com/workers/platform/limits/>

### 1.3 D1 / KV 免费档写入额度（决定数据落点）

| 产品 | 读 | 写 | 存储 |
|---|---|---|---|
| D1 | 5M rows / 天 | **100,000 rows / 天** | 5 GB |
| KV | 100,000 / 天 | **1,000 / 天** ⚠️ | 1 GB |

> **影响（结论）**：**历史数据必须落 D1，绝不能落 KV**。KV 每天只允许 1000 次写入，用 KV 存设备状态/遥测会当天就爆。
> 引用：<https://developers.cloudflare.com/workers/platform/pricing/>

---

## 2. 目标架构（数据平面 / 控制平面分离）

```
┌──────────────── 控制平面（低频，Worker） ────────────────┐
│  安卓 APP / Web                                          │
│     │ ① 登录（拿到身份）                                 │
│     │ ② GET /api/device/<id>/credential                 │
│     ▼                                                     │
│  Cloudflare Worker  ──►  校验 JWT ──► 绑定关系(D1)       │
│     │  ③ 返回一次性 MQTT 临时凭据                        │
│     │     { url: wss://...:8084/mqtt,                    │
│     │       user: "u_<uid>_<dev>",                       │
│     │       pass: "<短期token>", exp: 3600 }             │
└───────────────────────────────────────────────────────────┘
                          │
                          ▼  ④ 用临时凭据直连（长连接，不经 Worker）
┌──────────────── 数据平面（高频，EMQX 直连） ──────────────┐
│  安卓 APP / Web  ◄═════ WSS 8084 ═════►  EMQX Serverless│
│                                            ▲    │         │
│                                            │    │ ⑤ 订阅  │
│                       Homie 5 主题          │    ▼         │
│   ESP32-S3 设备 ═══ mqtts 8883 ════════════┘  订阅/发布   │
└───────────────────────────────────────────────────────────┘
                          │
                          │ ⑥ EMQX 数据集成（规则引擎 → HTTP Action，不计设备流量）
                          ▼
              Worker  /ingest（或独立 ingest Worker）
                          │
                          ▼
                   Cloudflare D1（历史/日志/在线状态）
```

**为什么这样分**：

| 理由 | 说明 |
|---|---|
| 成本 | 高频遥测走 EMQX 直连，**完全不消耗 Worker 请求额度** |
| 延迟 | APP 到 broker 一跳，不经 Worker 中转 |
| 安全 | 永远不下发设备级长期凭据给 APP；临时凭据过期即废 |
| Worker 职责单一 | 只做无状态鉴权，10ms CPU 绰绰有余 |

**唯一一个 Worker 侧要注意的点**：EMQX 数据集成回写（⑥）**会计入 Worker 请求数**。按 100k/天额度估算：若设备每 10s 上报一次遥测 = 8640 次/天/设备，单设备只占 8.6%。**多设备要提前算账**，必要时把 ingest 拆到独立 Worker 并降低上报频率（或按变化上报）。

---

## 3. 设备侧：Homie 5 自描述映射（复用 Capability Registry）

### 3.1 为什么这次改造特别合适

Homie 5 的核心是 **"设备自己声明自己有什么"**（`$description` JSON），控制器订阅通配符自动发现。你现在的 `capability_registry` **已经是一份能力清单**——它本来就是"能力名称 → stable_id → 云端/UI"的中间层（`readme.md` §4.10）。

| 你的现状 | Homie 5 对应物 | 需要做的 |
|---|---|---|
| `capability_registry` 的 Action 列表 | Node + Property | **几乎直接映射** |
| `capability_export_action_registry()` | `$description` 的 JSON | 换一层序列化即可 |
| `registry.version` | `$description.version`（整数，变化即递增） | **语义天然吻合** |
| `object_version`（Workflow variant） | `$description` 版本 | 已具备 |
| `LOG_REG_REBUILT` 等事件 | 事件驱动重发 `$description` | 已有埋点，挂回调即可 |

> **注意版本语义差异**：Homie 的 `$description.version` 要求是**整数且递增**（规范明确"不需要连续，可以用时间戳"）。你的 `registry.version` 是 uint32 递增——**直接可用**，这正是 Homie 想要的。⚠️ 但 Homie 要求 `$description` **只在 `$state` 为 `init`/`disconnected`/`lost` 时变更**，所以重发时机要挂在这几个状态上（见 3.3）。

### 3.2 主题映射设计（Homie 5 规范）

规范根主题：`homie/5/<device-id>/...`。Homie 官方要求控制器订阅 `+/5/+/$state` 做自动发现。

> ⚠️ **规范域可自定义**：homie-domain **允许改**（"if it does not suit your needs... you can change the domain part"），**版本段不可改**。你的现有 Topic 是 `guo_feeder/up|down|log`（`data/config/mqtt.json`）。两个选择：
> - **A（保守）**：domain 用 `homie`，新增 `homie/5/...` 树，与旧 `guo_feeder/*` 并存 → **旧协议零破坏**，MQTT-Tiles 开箱即用。
> - **B（统一）**：domain 改 `guofeeder`，得到 `guofeeder/5/...` → 品牌一致，但 **MQTT-Tiles 默认按 `homie` 发现**，需改源码里的发现域，反而增加工作量。
>
> **建议选 A**。旧 `guo_feeder/down`、`guo_feeder/up`、`guo_feeder/log` 保留不动（Protocol V2.0 冻结），Homie 树作为**新增的、面向 UI 的投影**。这样即使 Homie 层出问题，设备主链路不受影响。

```
homie/5/<device-id>/
├── $state                    online / ready / init / disconnected / lost / sleeping
├── $description              ← capability_registry 导出的 JSON（retained）
├── $homie                    5.0
├── valve/
│   ├── state                 open / close          （读）
│   └── set                   open / close          （写 → 触发 VALVE_OPEN/CLOSE Action）
├── weight/
│   ├── value                 -800750 等（读）
│   └── tared                 true/false
└── workflow/
    └── <wf_id>/
        ├── enable            true/false（写 → workflow.enable）
        └── variant           12（读，对应 workflow.variant）
```

**关键规范细节（照抄，别记错）**：

| 项 | 规范要求 |
|---|---|
| 根主题 | `homie/5/`（domain 可自定义，`5` 不可改） |
| `$state` 发布时机 | **必须 retained**，LWT 设为 `lost` |
| 设备生命周期 | `init` → `ready`（`online` 是 Homie 4 的旧值，**5 用 `ready`**） |
| `$description` | **retained**；仅可在 `init`/`disconnected`/`lost` 时变更 |
| 空 payload on `$state` | = 设备注销（removal） |
| property 写入 | 控制器发布到 `<node>/<prop>/set` |
| 属性基本项 | `name` / `datatype`（`integer`/`float`/`boolean`/`enum`/`string`/`json`）/ `format` / `settable` / `unit` / `retained` |

### 3.3 落地到固件：只加"投影层"，不碰核心

**架构铁律对齐**（`AI_RULES.md`）：`CloudManager → CommandManager → Workflow → Action → Hardware`，禁止跨层。

建议新增一个 **`homie_bridge`（或直接扩 `capability_registry` + `cloud_manager` 的一个路由）**，位置在 CloudManager 内，理由：

- 它**只做"序列化/主题映射"**，不含业务逻辑
- 写入方向统一转成**现有命令**：`<node>/<prop>/set` → 构造一条内部命令 → 走 **CommandManager**（复用既有的 `cmd_id` 去重、action `v`/`k` 校验）
- **不新增 Action / Trigger / EventId / ParamId**（遵守项目冻结约定），只用现有 Registry 导出数据

改动清单（预估）：

| 文件 | 改动 | 量级 |
|---|---|---|
| `src/homie_bridge.h/.cpp`（新） | 主题拼装、`$description` 生成、set 路由 | ~250 行 |
| `src/cloud_manager.cpp` | 订阅 `homie/5/<id>/+/+/set`；连接成功后发 `$state=ready` + `$description`(retained) | ~40 行 |
| `src/main.cpp` | 初始化 + 注册"registry 重建"回调 → 重发 `$description` | ~10 行 |
| `data/config/mqtt.json` | 新增 `homie_root`（默认 `homie/5`）、`device_id` | 2 字段 |

> ⚠️ **`device_id` 是量产阻塞项**：现在 Topic 里**没有设备 ID**（见 §6.1），Homie 强制要求 `<device-id>` 唯一。必须先把 `device_id` 落到配置里。

---

## 4. 前端：MQTT-Tiles vs Crouton（建议 MQTT-Tiles）

### 4.1 事实对比（已联网核实）

| 维度 | **MQTT-Tiles**（flespi） | Crouton |
|---|---|---|
| 协议栈 | Vue 2 + Quasar 1.x + Vuex + Webpack | Vue 3 + Vuetify |
| MQTT | MQTT 5.0 / 3.1.x，**任意 WSS broker** | 支持 MQTT |
| 组件 | gauge / toggle / button / iframe / text / switch | dashboard 组件 |
| 授权 | **MIT** | MIT |
| 活跃度 | 提交稀疏（最新 2026-04 附近），~110 star | 更活跃 |
| 配置存储 | 浏览器 localStorage **或 broker retained 消息** | 后端/本地 |
| 改造成本 | **低**（加"发现 Homie 设备"逻辑即可） | 中（Vue 3 需重写一部分） |

### 4.2 建议：**MQTT-Tiles**

理由：

1. **它有"任意 WSS broker"能力**，正好吃 EMQX 8084
2. **tile 模型和 Homie 的 property 是同一个粒度** —— 一个 tile ↔ 一个 property，映射几乎不用设计
3. MIT + Vue/Quasar，改一版**完全合法**
4. 你已经有 Pages 基础调试页（`readme.md` 已实现项），MQTT-Tiles 可以直接落地在 Pages 上作为"正式 UI"

**必须自己补的一段代码**：MQTT-Tiles **没有"自动发现设备"**逻辑（它靠手工加 tile）。你需要加：

```
连接后 → subscribe('homie/5/+/$state')
       → 收到 ready → subscribe('homie/5/<id>/$description')
       → 解析 JSON → 动态生成 tile 列表
```

这正是 **Homie 自描述的价值所在**：设备加了新能力，UI 自动出现对应控件，**前端不用改代码**。

> ⚠️ **MQTT-Tiles 的 publish 局限（上次已记录）**：它的静态 tile 无法为每条命令生成唯一 `id`；而你的设备**对重复 `id` 会静默丢弃**（`MQTT_DUP_CACHE_SIZE=10` / `TTL 30s`）。⇒ 必须在改版里让 button tile 每次发布生成**唯一 id**（或用设备侧的 `c`/`i` 旁路）。**这是改版的必做项，不是可选项。**

### 4.3 安卓端

| 方案 | 做法 | 适用 |
|---|---|---|
| **A. WebView 壳（推荐先做）** | 直接套 MQTT-Tiles 页面 + 本地通知 | 最快，一套代码两端跑 |
| **B. 原生 Kotlin** | `HiveMQ MQTT Client`（`com.hivemq:hivemq-mqtt-client`） | 需要后台常驻/推送/蓝牙配网时 |

**Android MQTT 库选型结论**：

| 库 | 协议 | 状态 |
|---|---|---|
| **HiveMQ MQTT Client** | 3.1.1 / **5.0** | ✅ **推荐**，异步非阻塞、内置指数退避重连、不依赖旧 support 库 |
| Eclipse Paho Android | 3.1 / 3.1.1 | ⚠️ **2018 年后停更**，需 Jetifier，Android 10+ 有兼容问题 |

→ **新项目直接用 HiveMQ**。EMQX 也是 MQTT 5.0 broker，协议对齐。

---

## 5. 本地工具部署清单（优先 D 盘）

### 5.1 现状盘点（已核实）

| 工具 | 现状 | 位置 |
|---|---|---|
| Node.js | ✅ v22.22.2（受管） | `C:\Users\wang\.workbuddy\binaries\node\versions\22.22.2-3` |
| npm | ✅ 10.9.7 | 同上 |
| Git | ✅ 2.55.0 | PortableGit |
| Python | ✅ 3.13.12 + venv | `...\binaries\python\...` |
| PlatformIO Core | ✅ 6.2.0 | `D:\platformIO` |
| **MQTTX** | ✅ **已装** | `D:\program files\MQTTX` |
| VS Code | ✅ | `D:\program files\Microsoft VS Code` |
| Wireshark | ✅ | `D:\program files\Wireshark` |
| **Wrangler** | ❌ 未装 | → 待装 |
| **Android Studio** | ❌ 未装 | → 待装 |
| **JDK 17** | ⚠️ 需确认 | → 待装 |
| pnpm | ❌ 未装 | 可选 |

**D 盘余量：723 GB** —— 空间完全够（Android Studio + SDK + 模拟器约 25–30 GB）。

### 5.2 必装（服务端 / 前端）

| # | 工具 | 装到 | 安装方式 | 用途 |
|---|---|---|---|---|
| 1 | **Wrangler CLI** | D 盘 | `npm i -g wrangler`（配 npm prefix 到 D） | Workers/Pages/D1 本地开发与部署 |
| 2 | **pnpm** | D 盘 | `corepack enable pnpm` | MQTT-Tiles 依赖安装（比 npm 快、省盘） |
| 3 | **Node 全局包目录** | `D:\Guo_Feeder_Project\tools\node-global` | `npm config set prefix` | **防 C 盘膨胀**（关键） |

### 5.3 必装（安卓端）

| # | 工具 | 装到 | 说明 |
|---|---|---|---|
| 4 | **Android Studio** | `D:\Android\Android Studio` | 含 SDK Manager |
| 5 | **Android SDK** | `D:\Android\Sdk` | 通过 `ANDROID_HOME` 指向 |
| 6 | **JDK 17**（Temurin） | `D:\Android\jdk17` | Android Studio 内置或独立安装 |
| 7 | **Gradle 缓存** | `D:\Android\.gradle` | `GRADLE_USER_HOME`（**不设会吃 C 盘十几 GB**） |
| 8 | **AVD 模拟器** | `D:\Android\avd` | `ANDROID_AVD_HOME` |

### 5.4 建议装（调试 / 量产）

| # | 工具 | 用途 |
|---|---|---|
| 9 | **emqx cli / mqttx-cli** | 命令行压测、脚本化订阅（你已有 `tools/mqtt_send.py`，可互补） |
| 10 | **Wireshark + TLS keylog** | 抓 `wss://...:8084/mqtt` 与 `mqtts://...:8883` 握手，排查 SNI/证书问题 |
| 11 | **Cloudflare 账号** | 已具备（Worker/Pages/D1 已跑过） |
| 12 | **EMQX Cloud 控制台权限** | 需能配"数据集成/规则引擎" |
| 13 | **Android 真机** | 比模拟器更贴近实际（WSS/证书行为） |

### 5.5 环境变量（一次性设置，全部指向 D 盘）

```powershell
# 管理员 PowerShell 执行
[Environment]::SetEnvironmentVariable("JAVA_HOME",     "D:\Android\jdk17",        "User")
[Environment]::SetEnvironmentVariable("ANDROID_HOME",  "D:\Android\Sdk",          "User")
[Environment]::SetEnvironmentVariable("ANDROID_SDK_ROOT", "D:\Android\Sdk",      "User")
[Environment]::SetEnvironmentVariable("GRADLE_USER_HOME", "D:\Android\.gradle",   "User")
[Environment]::SetEnvironmentVariable("ANDROID_AVD_HOME","D:\Android\avd",        "User")
[Environment]::SetEnvironmentVariable("Path", $env:Path + ";D:\Android\Sdk\platform-tools;D:\Android\Sdk\cmdline-tools\latest\bin", "User")
```

### 5.6 一键部署脚本

见同目录 `scripts/setup-cloud-app-env.ps1`（**默认 dry-run，加 `-Apply` 才真正执行**）。

---

## 6. 量产阻塞项（必须在写 APP 之前解决）

### 6.1 ★ Topic 里没有 device_id —— 多设备不可能

现状（`cloud_protocol.md` §1.1 / §1.3，已核实）：

```
subscribe_topic : guo_feeder/down      ← 无设备维度
publish_topic   : guo_feeder/up        ← 无设备维度
client_id       : guo_feeder_001
```

`cloud_protocol.md` §1.1 明确写着：**"Topic 是完整字符串，不是模板。设备不会把 `{uid}` 替换成任何值。"**

**后果（严重）**：
- 两台设备同时上线 → **两台都订阅同一个 `guo_feeder/down`** → 一条命令**两台都执行**
- 上行无法区分来源，D1 历史数据无法归属设备
- **Homie 5 强制要求 `<device-id>` 唯一** → 这一步不通，Homie 树根本立不起来

**建议**：把 Topic 模板化，例如 `guo_feeder/<device_id>/down`，`device_id` 从配置读（或在首次开机用 MAC 派生并持久化）。这是**一次改动、长期受益**，且是 Homie + 多设备 + D1 归属的共同前置。

### 6.2 MQTT 凭据进 Topic 命名空间 = 越权风险

现在设备用**同一个** `GuoFeederDevice` 账号连 broker，APP 也要连。如果 APP 复用它，那么 APP 就能**发 `guo_feeder/down` 控制任意设备**、能**订阅所有设备的 `up`**。

**必须做**：EMQX **ACL** 按 `device_id` 隔离，APP 用临时凭据登录时只授权 `guo_feeder/<该用户绑定的device_id>/#`。这正是 §2 控制平面存在的意义。

### 6.3 已知的日志链路缺陷（来自项目既有记录）

| # | 问题 | 影响 |
|---|---|---|
| BT-1 | 云端**从不返回 `log_ack`** | 设备 Flash 日志段**永不回收**（`cloud_protocol.md` §8.2 已定义 ACK 格式，云端未实现） |
| DEF-2 | `cloud_manager.cpp` **明文打印 MQTT username/password 到串口** | 凭据泄露；日志通道上线后风险放大（`未修复的问题.md`「LogManager 后续事项 #2」） |
| 9.7 | `platformio.ini` 的 `platform` 与 `lib_deps` **未固定版本** | 构建漂移会改变 LittleFS/二进制格式行为 |

> **建议顺序**：写服务端之前先修 **6.1（device_id）** 和 **6.2（ACL）**，否则 APP 与 D1 都会返工。`BT-1`/`DEF-2` 可并行。

---

## 7. 分阶段落地路线

| 阶段 | 目标 | 产出 | 无阻塞 |
|---|---|---|---|
| **P0** | Topic 模板化 + `device_id` 落配置 | 固件 + `mqtt.json` | ✅ **先做** |
| **P1** | EMQX ACL 按 device_id 隔离 + APP 临时凭据签发接口 | Worker `/credential` | 依赖 P0 |
| **P2** | `homie_bridge` 投影层（`$state`/`$description`） | 固件新模块 | 依赖 P0 |
| **P3** | EMQX 数据集成 → Worker `/ingest` → D1 | 规则 + ingest Worker | 依赖 P0 |
| **P4** | MQTT-Tiles 改版（自动发现 + 唯一 `id` 发布） | 前端仓库 | 依赖 P2 |
| **P5** | Pages 部署 + 安卓 WebView 壳 | 可装 APK | 依赖 P1/P4 |
| **P6**（可选） | 原生 Kotlin + HiveMQ | 原生 APP | 依赖 P5 验证 |

---

## 8. 待你确认的决策点

1. **Homie domain**：用 `homie`（保守，MQTT-Tiles 开箱可用）还是 `guofeeder`（品牌统一，需改前端发现域）？—— **建议 `homie`**
2. **前端框架**：MQTT-Tiles（低改造成本）还是 Crouton（更新更活跃）？—— **建议 MQTT-Tiles**
3. **安卓端**：先 WebView 壳还是直接原生 Kotlin？—— **建议先壳**
4. **`device_id` 来源**：MAC 派生 / 手工配置 / EMQX 分配？
5. **是否现在就装 Android Studio**（约 25–30 GB）？还是等 P4 之后再装？

---

## 附录 A：核实过的外部事实来源

| 结论 | 来源 |
|---|---|
| EMQX Serverless 仅 8883/8084，强制 TLS，WSS 需 SNI + `/mqtt` 路径 | `docs.emqx.io/en/cloud/latest/deployments/port_guide_serverless.html` |
| EMQX 数据集成（规则引擎 → HTTP）Serverless 可用，Beta 期免费 | `docs.emqx.io/en/cloud/latest/data_integration/http_server.html`、`emqx.com/blog/data-integration-is-now-available-in-emqx-cloud-serverless` |
| Workers 免费 100k req/天、10ms CPU、**网络等待不计 CPU** | `developers.cloudflare.com/workers/platform/limits/` |
| D1 免费 5M read / 100k write 行/天；KV 仅 1000 write/天 | `developers.cloudflare.com/workers/platform/pricing/` |
| Homie 5 主题 `homie/5/device/node/property`；domain 可自定义、版本段不可改；`$state` 需 retained + LWT | `homieiot.github.io`、`github.com/homieiot/convention` |
| MQTT-Tiles = MIT、Vue/Quasar、支持任意 WSS broker | `github.com/flespi-software/MQTT-Tiles` |
| Paho Android 停更（2018）、HiveMQ Client 支持 MQTT 5.0 | HiveMQ 官方 / 社区对比资料 |

## 附录 B：引用的项目内既有资产

| 资产 | 位置 | 与本方案关系 |
|---|---|---|
| Cloud Protocol V2.0 | `cloud_protocol.md` | 旧链路契约，Homie 树**并存不替代** |
| Capability Registry | `src/capability_registry.{h,cpp}` | **直接复用**为 `$description` 数据源 |
| Workflow 云端契约 | `workflow_cloud_interface.md` | `variant` → `$description.version` |
| MQTT 配置 | `data/config/mqtt.json` | 需新增 `device_id` / `homie_root` |
| 已知问题 | `未修复的问题.md`、`cloud_protocol.md` §9 | BT-1 / DEF-2 / 9.7 |
