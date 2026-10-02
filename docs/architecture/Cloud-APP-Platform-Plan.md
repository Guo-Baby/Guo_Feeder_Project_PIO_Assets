# 云端 + 安卓端架构规划（正式版 v2）

> 状态：**正式架构规划 v2（2026-09-30 修订）**，未改动任何固件代码。
> 规划基线：设备侧 Homie 5 自描述（复用 Capability Registry） + 前端 MQTT-Tiles 改版托管 Cloudflare Pages + EMQX Serverless WSS 8084 直连 + Worker 负责鉴权/绑定/凭据 + D1 经 EMQX 数据集成落历史。
> 本文是 **P0–P6 的实施依据**，并回答两个定量问题：**① Topic 改 device_id 的改造量；② Capability Registry 映射 Homie 的改造量。**
> 标记约定：**【已冻结】** 不再讨论的实现约束 · **【待设计】** 需产出设计文档 · **【待验证】** 需实测 · **【后续可选】** 可延后。

---

## 0. 结论速览（TL;DR）

| 层 | 选型 | 是否需自建服务器 | 状态 |
|---|---|---|---|
| 设备↔云 传输 | EMQX Cloud Serverless（**已有**） | 否（托管） | 【已冻结】 |
| 设备身份 | **MAC 派生 `device_id`**（首启持久化，不可配） | 否（固件内） | 【已冻结】 |
| Topic 规范 | **Topic V3：`guo_feeder/<device_id>/...`** | 否 | 【已冻结】 |
| 设备侧能力描述 | **Homie 5** 独立投影适配层，复用 `capability_registry` | 否（固件内） | 【已冻结】 |
| 前端（Web / 安卓壳） | **MQTT-Tiles**（MIT，Vue/Quasar）改一版 | 否（Pages 静态托管） | 【已冻结】 |
| 前端托管 | Cloudflare Pages（**已有**基础页面） | 否 | 【已冻结】 |
| 鉴权 / 绑定 / 凭据 | Cloudflare Worker（**已有**骨架） | 否 | 【待设计】 |
| 历史数据 | Cloudflare D1（**已有**）+ EMQX 数据集成（HTTP） | 否 | 【待设计】 |
| 安卓端 | **WebView 壳**（第一阶段） → 原生 Kotlin（按需） | 否 | 【后续可选】 |

**一句话**：**不需要**租 VPS、不需要 Docker、不需要自建 broker、**现在也不需要 Android Studio**。全部走托管服务；本地只装前端/服务端开发工具（见 §8）。

---

## 1. 三个"物理约束"（决定架构，不可绕）

这三条是联网核实的硬事实，直接否决了若干备选方案。**【已冻结】**

### 1.1 EMQX Serverless 只开两个端口

| 协议 | 端口 | 可用性 |
|---|---|---|
| MQTT over TLS (`mqtts`) | **8883** | ✅ 设备用 |
| MQTT over WebSocket + TLS (`wss`) | **8084** | ✅ 浏览器/APP 用 |
| TCP 1883 / WS 8083 | — | ❌ **Serverless 不支持** |

- WSS **完整 URL 必须带路径**：`wss://<部署域名>:8084/mqtt`
- **必须提供 SNI**，否则被拒（错误码 `-5`）
- Serverless **强制 TLS**（无明文选项）→ 本地调试也必须 `mqtts://` 或 `wss://`
- CA 证书：EMQX 提供，已存在于仓库 `data/emqxsl-ca.crt`
- 引用：<https://docs.emqx.io/en/cloud/latest/deployments/port_guide_serverless.html>

### 1.2 Cloudflare Worker 免费档 10ms CPU / 请求

| 项 | 免费档 |
|---|---|
| 请求数 | **100,000 / 天**（全部 Worker 合计，UTC 00:00 重置） |
| **CPU 时间** | **10 ms / 请求** |
| 内存 | 128 MB / isolate |
| 子请求 | 50 / 请求 |
| **外部网络等待** | **不计入 CPU 时间** ✅ |

官方原文：*"Waiting on network requests (such as fetch() calls, KV reads, or database queries) does not count toward CPU time."*

> **影响**：Worker 里"转调 EMQX API / 查 D1"这类**以 IO 为主**的操作**完全可行**（IO 等待不烧 CPU）。但**绝不能**在 Worker 里压缩/解析大 JSON、做长循环——那会计入 10ms 并报 `Error 1102`。
> 引用：<https://developers.cloudflare.com/workers/platform/limits/>

### 1.3 D1 / KV 免费档写入额度（决定数据落点）

| 产品 | 读 | 写 | 存储 |
|---|---|---|---|
| D1 | 5M rows / 天 | **100,000 rows / 天** | 5 GB |
| KV | 100,000 / 天 | **1,000 / 天** ⚠️ | 1 GB |

> **影响（结论）**：**历史数据必须落 D1，绝不落 KV**。KV 每天只允许 1000 次写入，存设备状态/遥测当天就爆。
> 引用：<https://developers.cloudflare.com/workers/platform/pricing/>

### 1.4 ★ EMQX Serverless 的认证/授权能力上限（本轮新增核实，**最关键**）

这一条是本轮最重要的发现，它**直接决定了 P1 临时凭据的实现方式**。

| 能力 | Serverless | Dedicated / BYOC |
|---|---|---|
| 内置数据库认证（username/password） | ✅ **支持** | ✅ |
| 内置数据库授权（ACL 规则） | ✅ **支持** | ✅ |
| **外部（HTTP）认证** | ❌ **不支持** | ✅ |
| **外部（HTTP/MySQL/Redis）授权** | ❌ **不支持** | ✅ |
| 授权模式"白名单"一键切换 | ❌ **不支持**（需用"拒绝 `#`"兜底实现） | ✅（v5.10+） |
| 认证/授权信息 **批量 CSV 导入** | ✅ 一次 1000 条，**最多 2000 条** | — |
| **API 管理认证 / ACL** | ✅ **支持**（AK/SK + HTTP Basic） | ✅ |
| 内置授权条目上限 | 部署连接数的 **2 倍**，封顶 100,000 | — |

- Serverless **支持**的 API 类别：**认证控制**（增删查改）、**访问控制**（增删查改）、客户端管理、主题订阅、消息发布。
- 引用：`emqx.com/zh/blog/mastering-iot-solution-with-emqx-cloud-serverless-addition`、`docs.emqx.com/en/cloud/latest/deployments/http_auth.html`（原文："HTTP authentication is not supported in EMQX Serverless deployments"）、`docs.emqx.com/en/cloud/latest/deployments/default_authz.html`

> **★★ 由此得出的关键架构结论（见 §5）**：
> Serverless **没有**"连接时动态鉴权"能力（HTTP auth 不支持），也**没有**"每条连接即时算权限"能力（HTTP authz 不支持）。
> ⇒ **"Worker 签发一次性临时凭据"不能是"凭空生成一个全新账号"**，只能是——
> **预置账号 + Worker 在签发时用 EMQX API 动态改写该账号的 ACL**。
> 这是本方案 P1 的**根本设计前提**，也是与 EMQX Dedicated 方案最大的差异。

---

## 2. 目标架构（数据平面 / 控制平面分离）

### 2.1 最终架构关系图

```
                         ┌──────────────────────────── 用户 / 运维 ────────────────────────────┐
                         │                                                                      │
                         │   ┌────────────────────────┐        ┌──────────────────────────┐     │
                         │   │  浏览器 / Web (P4)      │        │  安卓 (P5)                │     │
                         │   │  MQTT-Tiles +          │        │  WebView 壳 → 同 Web 页面  │     │
                         │   │  Homie Discovery Adapter│        │  (P6 可选: 原生 Kotlin)   │     │
                         │   └───────┬────────┬───────┘        └────────┬─────────────────┘     │
                         └───────────┼────────┼─────────────────────────┼──────────────────────┘
                                     │        │                         │
                    ① 控制面（低频） │        │ ② 数据面（高频，长连接，不经 Worker）
                                     │        │                         │
                                     ▼        └───────────┬─────────────┘
        ┌────────────────────────────────┐                │
        │  Cloudflare Worker（P1 鉴权面）  │                │  WSS :8084/mqtt
        │  · 用户登录 / JWT 校验           │                │  (SNI 必填)
        │  · 用户↔设备绑定查询 (D1)        │                │
        │  · 签发 MQTT 临时凭据            │                │
        │  · 设备注册 / 绑定 API           │                │
        │  · 小查询 / 管理 API             │                │
        └───────────┬────────────────────┘                │
                    │ ③ 用 EMQX API 改写 ACL              │
                    ▼                                     ▼
        ┌───────────────────────────┐        ┌═══════════════════════════════════┐
        │  EMQX Serverless          │◄═══════╡         EMQX Serverless           │
        │  · 内置认证 (预置账号)      │  ④ 鉴权 ║   Broker（8883 / 8084）           │
        │  · 内置 ACL (按 device_id) │         ║   · Topic V3 (业务)               │
        │  · 数据集成 (规则引擎)      │         ║   · Homie 5 树 (UI 投影)          │
        └───────────┬───────────────┘        └═══════════════┬═══════════════════┘
                    │                                        │
                    │ ⑤ 数据集成 (规则引擎 → HTTP)            │ mqtts :8883
                    ▼                                        │
        ┌───────────────────────────┐                        │
        │  Ingest Worker（P3）       │        ┌───────────────┴──────────────────┐
        │  · 按 device_id 落历史      │        │   ESP32-S3 设备（P0 / P2）        │
        │  · 批量写 / 限流            │        │   · Device Identity (MAC)         │
        └───────────┬───────────────┘        │   · CloudManager (Topic V3)       │
                    │                        │   · Homie Bridge (投影层, 独立)    │
                    ▼                        │   · Capability Registry (复用)     │
        ┌───────────────────────────┐        └───────────────────────────────────┘
        │  Cloudflare D1（P3）       │
        │  Device / User / Binding /  │
        │  Credential / Telemetry /   │
        │  Log（全部以 device_id 为键）│
        └───────────────────────────┘
```

**为什么这样分**：

| 理由 | 说明 |
|---|---|
| 成本 | 高频遥测走 EMQX 直连，**完全不消耗 Worker 请求额度** |
| 延迟 | APP 到 broker 一跳，不经 Worker 中转 |
| 安全 | 不下发设备级长期凭据给 APP；临时凭据过期即废 + ACL 按 device_id 收窄 |
| Worker 职责单一 | 只做无状态鉴权类 IO，10ms CPU 绰绰有余 |

### 2.2 设备内部：Homie Bridge 的位置（本轮重大修订）

**【已冻结】Homie Bridge 不是 CloudManager 的内部模块，而是与 CloudManager 并列的"协议/能力投影适配层"。**

```
                     ┌──────────────────────────────────────────────┐
                     │            Capability Registry               │
                     │  （Action / Trigger / Workflow 能力清单）      │
                     └───────────────────┬──────────────────────────┘
                                         │ 只读查询
              ┌──────────────────────────┼──────────────────────────┐
              │                          │                          │
              ▼                          ▼                          ▼
   ┌────────────────────┐   ┌────────────────────────┐   ┌────────────────────┐
   │  Homie Bridge      │   │   Cloud Manager        │   │  （未来其它协议）   │
   │  （协议投影适配层）  │   │   （业务云协议 V2.0）    │   │                    │
   │  · $state/$description│ │  · Topic V3 上下行     │   │                    │
   │  · 主题↔能力映射     │   │  · JSON/CBOR 编解码    │   │                    │
   └─────────┬──────────┘   └───────────┬────────────┘   └────────────────────┘
             │                          │
             │  set 方向转成"现有命令"   │
             └───────────┬──────────────┘
                         ▼
              ┌──────────────────────────┐
              │      CommandManager      │  ← 复用 cmd_id 去重 + v/k 校验
              └────────────┬─────────────┘
                           ▼
              ┌──────────────────────────┐
              │   Workflow / Action      │
              └────────────┬─────────────┘
                           ▼
                    【硬件 / 应用层】
```

**理由**：

1. Homie 与 Cloud Protocol **是两套并列的外部协议投影**，都消费同一个 Capability Registry，**互不依赖**。
2. 放在 CloudManager 内部会让 CloudManager 同时承担"业务云协议"和"UI 协议"，职责混淆，且 Homie 出问题会**污染主链路**。
3. 独立后：**Homie 层完全可关闭/可失败**，不影响 CloudManager 的 Topic V3 业务链路。

**铁律对齐**（`AI_RULES.md`）：写入方向**只有一个入口** ——
`<node>/<prop>/set` → **Homie Bridge** 转成现有命令 → **CommandManager**（复用 `cmd_id` 去重 + `v`/`k` 校验）→ Workflow/Action → 硬件。
**禁止** Homie Bridge 直接调用 Action / 直接操作硬件。

### 2.3 Capability → Homie 投影规则（本轮新增，**【待设计】细化**）

这是 Homie Bridge 的**唯一设计输入**，必须在 P2 前固化成一张表格。

| Capability 侧（已有） | Homie 侧 | 生成规则 |
|---|---|---|
| **Action**（`WorkflowActionDescriptor`） | **Node + settable Property** | Node id = `module`（如 `valve`）；Property id = Action 的语义名（如 `set`） |
| Action 的 `params[]`（`WorkflowParam`） | Property 的 /**独立子 Property** | `param_count == 0` ⇒ 单 Property（`enum`/`boolean`）；`param_count > 0` ⇒ 每个 param 一个可写 Property 或一个 `json` Property（【待设计】二选一，见 §11） |
| `WorkflowParam.name` | Property `$name`（或 id） | 直接映射 |
| `WorkflowParam.type`（`PARAM_INT/FLOAT/BOOL/STRING`） | `datatype` = `integer`/`float`/`boolean`/`string` | 一一对应 |
| `WorkflowParam.unit` | `unit` | 直接映射（空则不发该属性） |
| `WorkflowParam.description` | Property `$name` 的补充说明 | 直接映射（Homie 无独立 description 字段，并入 `$name`） |
| **Trigger**（`WorkflowTriggerDescriptor`） | **只读 Property**（状态/事件投影） | 事件类 Trigger **不映射**（Homie 无事件语义）；仅状态类映射为 `retained` 只读 Property |
| **Workflow**（含 `variant`） | **Node `workflow` 下的子 Node** | Node = workflow `runtime_id`；Property `enable`（settable，写 → `workflow.enable`）+ Property `variant`（只读，= `object_version`） |
| 设备全局状态（System State） | 根级 Property（如 `$state` 之外的自定义 `uptime`/`rssi`） | **【待设计】** 白名单映射，避免泄露内部状态 |
| `registry.version` / `object_version` | `$description.version` | 直接映射（uint32 递增，语义天然吻合） |

**datatype / format / settable / retained 生成规则（照抄 Homie 5）**：

| 场景 | `datatype` | `format` | `settable` | `retained` |
|---|---|---|---|---|
| 枚举动作（VALVE_OPEN/CLOSE） | `enum` | `open,close` | `true` | `false` |
| 布尔（enable） | `boolean` | — | `true` | `true` |
| 数值+单位（weight.value, unit=g） | `float` | — | `false` | `true` |
| 整数状态（rssi） | `integer` | — | `false` | `true` |
| 结构化参数集合 | `json` | — | `true` | `false` |
| 版本号（variant） | `integer` | — | `false` | `true` |

**必须遵守的 Homie 5 规范细节**：

| 项 | 规范要求 |
|---|---|
| 根主题 | `homie/5/`（domain 可自定义，**`5` 不可改**） |
| `$state` | **必须 retained**，**LWT = `lost`** |
| 生命周期 | `init` → **`ready`**（`online` 是 Homie 4 旧值，**5 用 `ready`**） |
| `$description` | **retained**；**仅可在 `$state` ∈ {`init`,`disconnected`,`lost`} 时变更** |
| `$description` 重发 | **不随每次状态变化重发**（见 §6.2） |
| 空 payload on `$state` | = 设备注销（removal） |
| property 写入 | 控制器发布到 `<node>/<prop>/set` |
| 禁止项 | 不能在 `ready` 之后随意重发 `$description` |

### 2.4 双协议并存（**【已冻结】**）

`homie/5/<device_id>/...`（UI 投影）与 `guo_feeder/<device_id>/...`（业务 V2.0）**永久并存**。

- Homie 树**只描述设备与能力**，不承载新业务协议；
- 旧 `guo_feeder/up|down|log`（无 device_id 版本）通过 §9 迁移路径过渡；
- **任何一方出问题不影响另一方**。

---

## 3. 【Q1】Topic 改 device_id 的改造量评估

### 3.1 结论：**改造量小且高度集中**

设备侧 Topic 的读写路径**只有 5 处**（已逐一核实）：

| # | 文件 / 行 | 内容 | 改动 |
|---|---|---|---|
| 1 | `src/services/config_manager.cpp:3686` | `config_get_mqtt_subscribe_topic()` | 返回值改为携带 `device_id` 的模板渲染结果 |
| 2 | `src/services/config_manager.cpp:3701` | `config_get_mqtt_publish_topic()`（有 fallback） | 同上 |
| 3 | `src/services/config_manager.cpp:3708` | `config_get_mqtt_log_topic()`（有 fallback） | 同上 |
| 4 | `src/services/config_manager.h:630/633/634` | 三个 getter 声明 | 可能新增一个"渲染后 Topic"接口 |
| 5 | `src/cloud/cloud_manager.cpp:65/66/68` | `static String mqtt_sub_topic / mqtt_pub_topic / mqtt_log_topic` | **不改**（仍缓存渲染结果） |

使用点（**无需改**，因为用的是缓存值）：

| 文件 / 行 | 用途 |
|---|---|
| `cloud_manager.cpp:333` | `subscribe(mqtt_sub_topic.c_str())` |
| `cloud_manager.cpp:550` | `topic = mqtt_pub_topic` |
| `cloud_manager.cpp:554` | `topic = mqtt_sub_topic` |
| `cloud_manager.cpp:1471-1479` | 从 Config 读入三个静态变量 |
| `cloud_manager.cpp:1804` | fallback `"guo_feeder/log"` → 应改为带 `device_id` |

**真正的实现选择**（【待设计】二选一，建议方案 A）：

- **方案 A（建议）：保留配置项为"模板"，运行时渲染。**
  `data/config/mqtt.json` 里仍写 `guo_feeder/<device_id>/down`，由新增的 `topic_render()` 把 `<device_id>` 替换成运行时 `device_id`。
  优点：配置可读、可回滚、可单测；旧 `data/config/mqtt.json` 迁移容易。
  > **★ 2026-10-02 补充（价值最高的一条）**：方案 A 让「回滚 Topic V3」变成
  > **改配置而不是回退固件** —— 渲染器对**不含 `<device_id>` 的模板原样放行**（并打一次 WARN）
  > ⇒ 把 topic 改回 `guo_feeder/down` 即立刻回到 V2.0 行为。
  > **⇒ 「是否启用 Topic V3」完全由配置决定，灰度与回滚都不碰固件。**
  > 且 P0 **刻意不做「双订阅 legacy + V3」**（双订阅 ⇒ 共享 `down` 仍能控制设备，违反 §9.4 第 4 条）。
  > 详见 `P0-设备身份与Topic隔离设计.md` §1.3 / §3.3 / §8.2。
- **方案 B：不复用模板，直接由代码拼接** `"guo_feeder/" + device_id + "/down"`。
  优点：零解析开销；缺点：失去可配置性，EMQX 侧改主题需改固件。

> **注意**：`cloud_protocol.md` §1.1 明确"In-MQTT Topic 是完整字符串，设备不会替换 `{uid}`"。**方案 A 的替换是由固件新增的 `topic_render()` 完成，与协议文档不冲突**（协议文档描述的是"设备不会自作主张替换"，我们这里是显式实现）。协议文档需补一句说明。

### 3.2 EMQX 侧改造量：**中等，且可用 API/CSV 批量化**

| 任务 | 手段 | 自动化程度 |
|---|---|---|
| 设备账号创建 | 控制台单条 / **CSV 批量导入**（一次 1000，最多 2000） | ✅ 可脚本生成 CSV |
| 设备 ACL 规则 | CSV 批量导入（字段 `clientid,username,topic,action,access`） | ✅ 可脚本生成 |
| APP 临时 ACL 改写 | **EMQX Serverless REST API**（AK/SK Basic）| ✅ **Worker 内 `fetch()` 调用**（IO 等待不烧 CPU） |
| 授权白名单化 | Serverless 不支持一键切换 ⇒ **添加"全部用户 → `#` → pubsub → 拒绝"兜底规则** | 控制台/API 一次性配置 |

**EMQX ACL 规则设计（Topic V3）**：

| 主体 | Topic | Action | 权限 |
|---|---|---|---|
| 设备（`username = dev_<device_id>`） | `guo_feeder/<device_id>/up` | pub | allow |
| 设备 | `guo_feeder/<device_id>/log` | pub | allow |
| 设备 | `guo_feeder/<device_id>/down` | sub | allow |
| 设备 | `homie/5/<device_id>/#` | pubsub | allow |
| 设备 | `homie/5/+/$state` | sub | allow（发现用，可选） |
| APP（`username = app_<uid>_<device_id>`） | `guo_feeder/<device_id>/down` | pub | allow |
| APP | `guo_feeder/<device_id>/up` | sub | allow |
| APP | `guo_feeder/<device_id>/log` | sub | allow（默认可关） |
| APP | `homie/5/<device_id>/#` | pubsub | allow |
| **兜底（全部用户）** | `#` | pubsub | **deny** ← 实现白名单效果 |

> ⚠️ Serverless **不支持控制台"白名单一键切换"**，必须靠最后一条"拒绝 `#`"实现白名单语义。
> ⚠️ 客户端**不能订阅裸 `#`**（broker 限制），但**授权规则里可以写 `#`** 作为兜底拒绝——官方明确说明。

### 3.3 Cloudflare 侧改造量：**小**

- EMQX 数据集成（规则引擎）的 SQL 里，事件主题已天然带 `<device_id>` → **规则里直接 `payload` + `topic` 落库即可，无需"规则映射表"**。
- Worker 只需在 D1 侧按 `device_id` 分表/分键（见 §7）。
- ⇒ **CF 侧没有"Topic 映射"改造工作量**，只有 D1 schema 与 ingest 逻辑（属 P3）。

### 3.4 CLI 工具可用性（本轮核实）

| 平台 | 官方 CLI | 能力 | 结论 |
|---|---|---|---|
| Cloudflare | **Wrangler**（`npm i -g wrangler`） | Workers / Pages / **D1（`wrangler d1 execute`）** / KV / R2 / secret / tail / 部署 | ✅ **全流程可 CLI**，CI 用 `CLOUDFLARE_API_TOKEN` + `wrangler deploy` |
| EMQX Serverless | **无官方"emqx cloud"CLI** | 但提供 **REST API**（AK/SK + HTTP Basic）：认证控制 / 访问控制 / 客户端管理 / 订阅信息 / 消息发布 | ⚠️ **无 CLI，但有 REST API** ⇒ 可用 `curl` / Node 脚本 / **Worker `fetch()`** 自动化 |
| MQTT 调试 | **MQTTX CLI**（`npm i -g mqttx-cli`） | `conn` / `sub` / `pub` / `bench`，支持 CA/自签/双向 SSL | ✅ 本地验证用 |

> **回答"你能否全部帮我配置"**：
> - **Cloudflare 侧**：**可以全自动**（Wrangler + API Token）。
> - **EMQX 侧**：**部分可自动**。API 覆盖"认证/ACL 增删改查"，所以"批量建 2000 个设备账号+ACL"可以写成脚本；但**部署创建、开启 AK/SK、数据集成规则、白名单兜底规则**这类"平台级一次性操作"**仍需你在控制台点几下**（我提供逐步操作清单）。
> ⇒ **分工**：我写**脚本（生成 CSV / 调 EMQX API / Wrangler 配置）**；你执行**控制台一次性配置**。

### 3.5 Q1 最终结论

| 维度 | 结论 |
|---|---|
| 固件改造量 | **小**（1 个新函数 + 3 个 getter + 1 个 JSON 字段 + 1 处 fallback） |
| EMQX 改造量 | **中**（批量 CSV + 白名单兜底 + APP ACL 走 API） |
| CF 改造量 | **小**（无需 Topic 映射，只有 D1 schema） |
| 阻塞关系 | **Topic V3 是 Homie / 多设备 / D1 归属的共同前置** ⇒ **P0 第一件事** |

---

## 4. 【Q2】Capability Registry 映射 Homie 的改造量评估

### 4.1 结论：**改造量小 —— 因为元数据已存在**

**关键发现（本轮核实）**：Homie 需要的全部元数据，**在你的现有结构体里已经有了**。

| Homie 需要的字段 | 现成来源 | 位置 |
|---|---|---|
| Node 名 | `WorkflowActionDescriptor.module` | `src/automation/workflow.h:241` |
| Property 名（人类可读） | `WorkflowActionDescriptor.name` | 同上 |
| Property 说明 | `WorkflowActionDescriptor.description` | 同上 |
| 参数列表 | `WorkflowActionDescriptor.params[]` + `param_count` | 同上 |
| 参数名 | `WorkflowParam.name` | `src/automation/workflow.h:157` |
| **参数类型 → `datatype`** | `WorkflowParam.type`（`PARAM_INT`/`PARAM_FLOAT`/`PARAM_BOOL`/`PARAM_STRING`，`workflow.h:91`） | 同上 |
| **单位 → `unit`** | `WorkflowParam.unit` | 同上 |
| 参数说明 | `WorkflowParam.description` | 同上 |
| 能力清单 + 稳定 ID | `capability_registry` 的 `action/trigger/workflow.bin` | `src/automation/capability_registry.h` |
| 版本号 | `registry.version`（uint32 递增） | 同上 |
| Workflow 内容版本 | `CapabilityMapping.object_version`（= `workflow.variant`） | `src/automation/capability_registry.h:127` |

> **⇒ Homie Bridge 的工作 = "序列化 + 主题映射"，不是"新建业务模型"。**
> `PARAM_INT/FLOAT/BOOL/STRING` → `integer/float/boolean/string` 是**一对一映射**，无需设计。

### 4.2 规模估算

| 项 | 现状 | Homie 侧规模 |
|---|---|---|
| Action 注册数 | **~8 个**（MiThermometer 2 · computer_reset 1 · test_mqtt 1 · valve 2 · weight 1 · workflow.cpp:961） | 去除重复 ⇒ **Node 约 4–5 个**，Property 约 8–10 个 |
| Trigger 注册数 | **4 个**（weight 1 · timer · delay · workflow.cpp:923） | 仅状态类映射为只读 Property ⇒ **约 1–2 个** |
| Workflow | ≤16（`WORKFLOW_MAX_COUNT`） | Node `workflow/<runtime_id>` ×2 Property |
| `$description` JSON 体积 | — | **【待验证】** 估计 2–6 KB（含全部 node/property） |

**⇒ `$description` 体积不大**，用 PSRAM 缓冲即可（见 §4.3）。

### 4.3 RAM / Flash 压力分析（本轮实测）

**实测固件占用**（`xtensa-esp32s3-elf-size`）：

```
text   = 1,108,098 B  (~1.06 MB)   → Flash (IRAM 拷贝)
data   =   278,896 B  (~272 KB)    → DRAM 初始化数据
bss    = 1,222,165 B  (~1.19 MB)   ← ★ 静态 RAM 占用（真正压力点）
```

> ⚠️ **`bss ≈ 1.19 MB` 是静态 RAM 占用**，用户"内置 RAM 快满"的判断**属实**。ESP32-S3 内部 SRAM 约 512 KB（DRAM），**注意这里是"逻辑大小"，包含大量已初始化为 0 的结构体数组**（如 `CapabilityRegistryTable` ×3、Workflow 表、LogManager 缓冲等）。

**关键事实（本轮核实）**：

1. **项目已有完善的 PSRAM 优先 / DRAM 回退模式**，可直接复用：
   ```cpp
   // capability_registry.cpp:510-513（既有模式）
   void *p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
   if (!p) p = heap_caps_malloc(size, MALLOC_CAP_8BIT);
   ```
   同样模式出现在 `config_manager.cpp:1889`、`workflow.cpp:783-784`、`log_manager.cpp` 多处。
2. **内存铁律（`AI_RULES.md`）**：**栈永远在内部 RAM；PSRAM 只能用于堆**；**>1KB 结构禁止上栈**；`MALLOC_CAP_SPIRAM` 优先、失败回退 `MALLOC_CAP_8BIT`。
3. **PSRAM 已确认可用**：`main.cpp:452-453` 已有 `ESP.getPsramSize()` / `getFreePsram()` 打印。

**⇒ 结论与建议**：

| 对象 | 放置 | 理由 |
|---|---|---|
| `$description` 生成缓冲（String/动态） | **PSRAM 堆** | 2–6 KB 级，一次性生成 |
| Registry 映射增量（Homie 专用索引表） | **PSRAM 堆**（**不新增静态数组**） | 避免继续推高 `bss` |
| Homie 主题字符串 | **PSRAM 堆**（若 >1KB） | 遵循铁律 |
| Capability Registry 本体（已有 3×`entries[32]`） | **DRAM（不动）** | 已存在，改位置风险大且无必要 |
| `$description` 静态模板 / 常量字符串 | **Flash（`const char[]`）** | 不占 RAM |

> ⚠️ **不要把 Homie Bridge 做成新的静态大数组** —— 那会继续推高 `bss`。
> ✅ **Homie Bridge 只持有"指针 + 长度"，缓冲全部走 PSRAM 动态分配。**

**外置 Flash 问题**：**本轮不需要外置 Flash**。
理由：`$description` 是**运行时生成**的（从 Registry 读），**不需要落盘**；Homie 无必须的持久化状态（`$state` 走 retained 存在 broker 侧）。⇒ **不引入外部 Flash 驱动，节省复杂度。**

### 4.4 Q2 最终结论

| 维度 | 结论 |
|---|---|
| 改造类型 | **序列化 + 主题映射**（非业务模型改造） |
| 新增代码量 | `src/homie_bridge.h/.cpp` **约 250–350 行**（不含投影表配置） |
| 元数据来源 | **全部现成**（`WorkflowActionDescriptor` / `WorkflowParam` / `capability_registry`） |
| RAM 影响 | **接近 0**（全部走 PSRAM 动态分配，不新增静态数组） |
| 外置 Flash | **不需要**（`$description` 运行时生成，无持久化需求） |
| 主要风险 | `$description` **生成时机**（Homie 规范限制，见 §6.2）；`$description` **重发策略** |

---

## 5. EMQX 认证 / 授权模型（P1 核心，**【待设计】**）

### 5.1 模型要素（必须在 P1 前定稿）

| 要素 | 定义 | 备注 |
|---|---|---|
| **User**（APP 用户） | 平台账号，`uid` | D1 `User` 表 |
| **Device**（设备） | `device_id`（MAC 派生） | D1 `Device` 表，唯一键 |
| **Binding**（绑定关系） | `uid ↔ device_id`，含权限级别/生效期 | D1 `UserDeviceBinding` 表 |
| **Credential**（凭据） | MQTT 凭据：`username` / `password` / `expires_at` / `scope` | D1 `Credential` 表 |
| **Authenticator** | EMQX 内置数据库（**Serverless 唯一选项**） | 预置账号 |
| **Authorizer** | EMQX 内置 ACL（**Serverless 唯一选项**） | 按 `device_id` 收窄 |

### 5.2 ★ 临时凭据的实现路径（Serverless 约束下的唯一解）

因为 **Serverless 不支持 HTTP 认证/授权**，临时凭据**不能**是"凭空生成新账号 + 连接时动态算权限"。**只能**是：

```
① 预置（一次性，运维）
   ├─ 为每台设备建账号：username = dev_<device_id>，password = <随机强密码>
   └─ 为"APP 凭据槽位"预置 N 个账号：username = app_slot_<n>，password = <平台统一密钥>
      （N 按并发 APP 数估算，可用 CSV 批量导入，上限 2000）

② 签发（每次 APP 请求，Worker）
   ├─ 校验用户 JWT
   ├─ 查 D1 UserDeviceBinding：该用户是否绑定 device_id？
   ├─ 选定一个空闲 slot（app_slot_<n>）
   ├─ 调用 EMQX API "访问控制 → 增删改查"：
   │     把 app_slot_<n> 的 ACL 改写为"仅本 device_id"
   │       - allow sub guo_feeder/<device_id>/up
   │       - allow sub guo_feeder/<device_id>/log
   │       - allow pub guo_feeder/<device_id>/down
   │       - allow pubsub homie/5/<device_id>/#
   ├─ 写入 D1 Credential（username=app_slot_<n>, scope=device_id, expires_at=now+3600）
   └─ 返回 { url: wss://<域名>:8084/mqtt, username, password, expires_at }

③ 使用（APP）
   └─ 直连 EMQX WSS 8084，EMQX 用内置 ACL 校验（已收窄到 device_id）

④ 回收（Worker / 定时任务）
   ├─ 到期后把 slot 的 ACL 改回"全部 deny"
   └─ 标记 Credential 失效（复用 slot 时先改 ACL）
```

**关键点（写进文档防遗忘）**：

| # | 约束 | 后果 |
|---|---|---|
| 1 | **不能**在连接时动态鉴权 | ⇒ **必须预置账号**，slot 数量是并发上限 |
| 2 | **不能**用 HTTP authz | ⇒ **ACL 改写必须通过 API 预写** |
| 3 | ACL 改写是**全局生效**的（按 username 维度） | ⇒ **同一 slot 不能被两个 APP 同时用**，需 slot 池 + 租约 |
| 4 | Serverless **不支持白名单一键切换** | ⇒ **必须加"拒绝 `#`"兜底规则**（否则黑名单模式下未拒绝即允许，等于无 ACL）|
| 5 | 客户端不能订阅裸 `#` | ⇒ APP 只能订阅具体前缀，无影响 |
| 6 | 内置 ACL 条目上限 = 连接数×2（封顶 10 万） | ⇒ 2000 设备 × 每设备 ~4 条 ≈ 8000 条，**够用** |

> **⚠️ 【待设计】必须回答的问题**（P1 开工前）：
> 1. APP 凭据槽位数 N 取多少？槽位与 APP 会话的生命周期怎么对齐？
> 2. 是"改 ACL"还是"给每个 user 建永久受限账号"？（前者省条目，后者省 API 调用）
> 3. Serverless 是否支持 EMQX API 的 "按 username 改 ACL"？（**需实测**，见 §11）
> 4. 退化为"更长有效期凭据 + 无 slot 池"是否可接受？（复杂度大幅下降，安全性下降）

### 5.3 设备侧账号（无临时性）

设备用**固定账号** `dev_<device_id>`（随机强密码，首启写入配置或独立 NVS）。
- ⚠️ **DEF-2 必须先修**：`cloud_manager.cpp` 目前**明文打印 MQTT username/password 到串口**，设备账号如果是"一机一密"，泄露面会扩大。**P0 必须处理**。

---

## 6. Worker 职责边界（**【待设计】**）

### 6.1 Worker 做什么 / 不做什么

| ✅ Worker 做 | ❌ Worker 不做 |
|---|---|
| 用户登录 / JWT 校验 | **不做 MQTT 数据转发/代理** |
| 用户↔设备绑定查询与增删（D1） | 不做高频遥测中转 |
| 签发 MQTT 临时凭据（走 EMQX API） | 不做大 JSON 压缩/解析（10ms CPU） |
| 设备注册 / 绑定 API | 不做长循环 / 批量计算 |
| 小查询（设备列表、在线状态、最近 N 条） | 不做 MQTT 长连接持有 |
| 管理 API（用户/绑定/凭据） | 不存设备侧业务状态 |
| **历史数据入口**（EMQX 数据集成 → ingest Worker → D1） | 不承载实时控制/状态 |

**判定原则**：**Worker 只处理"低频 + 以 IO 为主 + 单次即可完成"的请求。**

### 6.2 实时控制/状态的正确路径（**已冻结**）

```
实时控制：浏览器/APP ──publish──► EMQX ──► 设备        （不经 Worker）
实时状态：设备 ──publish──► EMQX ──► 浏览器/APP        （不经 Worker）
         ▲ 浏览器/APP 通过 WSS 直连，长连接保持在 EMQX 侧
```

**Worker 只在"进门之前"出现**（拿凭据），**进门之后不再参与**。

### 6.3 D1 写入的两条路径（区分清楚）

| 路径 | 触发 | 内容 | 是否计 Worker 请求 |
|---|---|---|---|
| **A. 用户 API 触发** | 用户点开某设备历史 | 同步查 D1 → 返回 | ✅ 计入（低频） |
| **B. 设备数据触发** | EMQX 数据集成规则命中 | **ingest Worker** 批量写 D1 | ✅ 计入（**需要算账**，见 §7.3） |

---

## 7. EMQX → Worker → D1 历史链路与容量核算（**【待设计】**）

### 7.1 容量核算（**本方案第一个真正的瓶颈**）

**假设**：设备每 **10s** 上报一次遥测（`guo_feeder/<device_id>/up` 经 EMQX 数据集成 → ingest Worker → D1）。

| 项目 | 单设备 | 11 台设备 | 免费额度 | 占用 |
|---|---|---|---|---|
| Worker 请求 / 天（ingest） | 8,640 | **95,040** | 100,000 | **95.0%** ⚠️ |
| D1 写行数 / 天 | 8,640 | **95,040** | 100,000 | **95.0%** ⚠️ |
| D1 存储 / 年（每行 ~200B） | ~630 MB | ~6.9 GB | 5 GB | ❌ **超** |

> ⚠️ **⚠️ 双 95% + 存储超限**。这是**必须解决**的问题，不是"以后再说"。

### 7.2 由此得出的强制设计约束（**【已冻结】**）

| # | 约束 | 说明 |
|---|---|---|
| 1 | **遥测不能按秒/固定 10s 全量落库** | 必须做**降频 / 阈值 / 聚合** |
| 2 | **实时 ≠ 历史** | 实时走 EMQX 直连（不落库）；**只有需要回溯的才落 D1** |
| 3 | **批量写** | ingest Worker 一次请求合并多条（D1 支持多值 INSERT） |
| 4 | **只存必要字段** | 不存原始 JSON 全量 payload |
| 5 | **日志与遥测分表** | 日志（`LOG-n`）与遥测（`VALVE-1`/`weight`）落点不同 |
| 6 | **告警事件 100% 落库，常规遥测按策略** | 事件全存，周期数据抽样 |

### 7.3 遥测 → 历史数据策略（**【待设计】，P3 必须定稿**）

| 数据类别 | 频率 | 落库策略 | 预估日行数/设备 |
|---|---|---|---|
| 故障/安全事件（`EVENT_*`, `VALVE_ERROR`） | 低频 | **100% 落库** | < 100 |
| 供水动作（`VALVE_OPEN/CLOSE`, `0x0505`） | 中低频 | **100% 落库** | < 500 |
| 重量（`weight.value`） | 高频 | **阈值 + 抽样**（变化 >Xg 才记，或每 N 分钟一条） | < 300 |
| 水位/温湿度 | 高频 | **每 5–15 分钟一条**（或变化超阈值） | < 300 |
| 在线状态（`$state`） | 低频 | **状态变化才落库** | < 50 |
| 心跳 | 高频 | **不落库**（仅内存/在线态） | 0 |

> ⇒ 目标：**单设备 < 1200 行/天**，11 台 ≈ 13,200 行/天（**13% 额度**），D1 存储 ~1 GB/年。**安全**。

**具体手段（【待设计】二选一或组合）**：
- **设备侧**：上报频率/阈值由配置控制（不新增 Action，走现有 `config`）。
- **EMQX 侧**：数据集成规则 SQL 里 `WHERE` 过滤（如只转发变化超过阈值的事件）。
- **ingest 侧**：Worker 内按 `device_id` 聚合 + 批量 INSERT（注意 CPU 预算，**只做拼接不做解析**）。

### 7.4 明确结论（写进文档）

> **本方案的第一个瓶颈不是 D1 存储，而是「Worker 请求/天 + D1 写行/天」的组合上限，以及 D1 的年存储增长。**
> 必须在 P3 之前把遥测降频/聚合策略定稿，否则 11 台设备就接近打满免费额度。

---

## 8. 本地工具部署清单（优先 D 盘）

### 8.1 现状盘点（已核实）

| 工具 | 现状 | 位置 |
|---|---|---|
| Node.js | ✅ v22.22.2（受管） | `C:\Users\wang\.workbuddy\binaries\node\versions\22.22.2-3` |
| npm | ✅ 10.9.7 | 同上 |
| Git | ✅ 2.55.0 | PortableGit |
| Python | ✅ 3.13.12 + venv | `...\binaries\python\...` |
| PlatformIO Core | ✅ 6.2.0 | `D:\platformIO` |
| **MQTTX（GUI）** | ✅ **已装** | `D:\program files\MQTTX` |
| VS Code | ✅ | `D:\program files\Microsoft VS Code` |
| Wireshark | ✅ | `D:\program files\Wireshark` |
| **Wrangler** | ❌ 未装 | → **必装**（P1 起） |
| **MQTTX CLI** | ❌ 未装 | → 建议装（验证用） |
| pnpm | ❌ 未装 | → 可选（P4 起） |
| **Android Studio** | ❌ 未装 | → **【后续可选】**，P5 之后再说 |
| JDK 17 | ⚠️ 未确认 | → 随 Android Studio |

**D 盘余量：723 GB**。

### 8.2 现在必装（服务端/前端，**P0–P3 就够用**）

| # | 工具 | 装到 | 命令 |
|---|---|---|---|
| 1 | **Node 全局包目录** | `D:\Guo_Feeder_Project\tools\node-global` | `npm config set prefix "D:\Guo_Feeder_Project\tools\node-global"` |
| 2 | **npm 缓存** | `D:\Guo_Feeder_Project\tools\npm-cache` | `npm config set cache "D:\Guo_Feeder_Project\tools\npm-cache"` |
| 3 | **Wrangler** | 同上 | `npm i -g wrangler` |
| 4 | **MQTTX CLI** | 同上 | `npm i -g mqttx-cli` |

### 8.3 后续装（P4/P5）

| # | 工具 | 时机 |
|---|---|---|
| 5 | pnpm（`corepack enable pnpm`） | P4 MQTT-Tiles 改版 |
| 6 | Android Studio + SDK + JDK17 + Gradle 缓存 + AVD | **P5 之后按需**（约 25–30 GB） |

**未来 Android 环境变量（一次性，全部指向 D 盘，**现在不要设**）**：

```powershell
[Environment]::SetEnvironmentVariable("JAVA_HOME",          "D:\Android\jdk17",  "User")
[Environment]::SetEnvironmentVariable("ANDROID_HOME",       "D:\Android\Sdk",    "User")
[Environment]::SetEnvironmentVariable("ANDROID_SDK_ROOT",   "D:\Android\Sdk",    "User")
[Environment]::SetEnvironmentVariable("GRADLE_USER_HOME",   "D:\Android\.gradle","User")
[Environment]::SetEnvironmentVariable("ANDROID_AVD_HOME",   "D:\Android\avd",    "User")
```

### 8.4 一键部署脚本

见同目录 `scripts/setup-cloud-app-env.ps1`（**默认 dry-run，加 `-Apply` 才执行**）。
> ⚠️ **脚本必须纯 ASCII**：PowerShell 5.1 读取无 BOM 的 `.ps1` 按 ANSI 解码，**中文注释会直接 `ParserError`**（首版实际踩到）。

---

## 9. 遗留协议兼容与迁移（**【已冻结】原则**）

### 9.1 现状

- `guo_feeder/down` / `guo_feeder/up` / `guo_feeder/log`（**无 device_id**）已存在且被固件使用（`data/config/mqtt.json`）。
- 旧 Topic **不能直接删除**（会打断现有验证链路）。

### 9.2 但不允许"共享 down"成为最终架构

**共享 `down` 的后果（严重）**：两台设备同时上线 → **两台都订阅同一 `guo_feeder/down`** → **一条命令两台都执行**。

### 9.3 迁移路径（**【已冻结】**）

```
Legacy Topic                 Compat 期                    Topic V3（生产）
guo_feeder/down      ──►   同时订阅 Legacy + V3   ──►   仅订阅 guo_feeder/<device_id>/down
guo_feeder/up        ──►   双发（可选）           ──►   仅发 guo_feeder/<device_id>/up
guo_feeder/log       ──►   双发（可选）           ──►   仅发 guo_feeder/<device_id>/log
```

### 9.4 生产环境硬要求（**【已冻结】**）

| # | 要求 |
|---|---|
| 1 | **生产 ACL 必须阻止**通过旧通配 Topic 的跨设备越权（**"拒绝 `#`"兜底 + 白名单化**） |
| 2 | **final 架构不允许**任何"共享 Topic"承载设备控制 |
| 3 | 设备**只订阅自己的** `guo_feeder/<device_id>/down` |
| 4 | Legacy Topic 可作为**只读兼容**保留（过渡期），**不允许作为控制入口** |
| 5 | **Topic V3 是量产前置，不是"安卓阶段再做的事"** |

---

## 10. 分阶段落地路线（P0–P6）

> 形态：**已冻结** 的项目不再改动；**待设计** 的必须先产出设计文档再开工。

### P0 · 设备身份 + Topic 隔离 + ACL 设计（**【已冻结】范围**）

| # | 任务 | 产出 | 状态 |
|---|---|---|---|
| 0.1 | **`device_id` 定义与生成**：MAC 派生，统一小写 hex，格式 `aabbccddeeff`（或 `gf-aabbccddeeff`） | 设计文档 | 【待设计】 |
| 0.2 | `device_id` **首启持久化**（独立于 ConfigManager 的 NVS 分区或 Config 只读字段） | 设计文档 | 【待设计】 |
| 0.3 | **`device_id` 不可被 `update_config` 修改**（写入时显式拒绝） | 设计文档 | 【待设计】 |
| 0.4 | **Topic V3**：`guo_feeder/<device_id>/down|up|log` + `topic_render()` | 固件 | 【待设计】 |
| 0.5 | `data/config/mqtt.json` 迁移（模板写法） | 配置 | 【待设计】 |
| 0.6 | **EMQX ACL 模型设计**（§3.2 表 + 白名单兜底） | 设计文档 | 【待设计】 |
| 0.7 | **DEF-2 修复**（凭据明文打印串口） | 固件 | 【待设计】 |

> ⚠️ **P0 完成前不得进入 P1**（ACL 依赖 `device_id`）。

### P1 · EMQX 认证 + 设备绑定 + 临时凭据

| # | 任务 | 状态 |
|---|---|---|
| 1.1 | EMQX 侧：预置设备账号（CSV 批量） | 【待设计】 |
| 1.2 | EMQX 侧：预置 APP 凭据 slot + 白名单兜底规则 | 【待设计】 |
| 1.3 | Worker：用户登录 / JWT / 绑定校验 | 【待设计】 |
| 1.4 | Worker：`GET /api/device/<id>/credential`（EMQX API 改 ACL + 返回凭据） | 【待设计】 |
| 1.5 | D1：`User` / `Device` / `UserDeviceBinding` / `Credential` 建表 | 【待设计】 |
| 1.6 | **实测验证**：EMQX API 能否按 username 改 ACL（§5.2 疑问 3） | 【待验证】 |

### P2 · Homie Bridge（独立投影适配层）

| # | 任务 | 状态 |
|---|---|---|
| 2.1 | **投影规则表**固化成文档（§2.3 细化） | 【待设计】 |
| 2.2 | `src/homie_bridge.h/.cpp`：`$description` 生成（PSRAM 缓冲） | 【待设计】 |
| 2.3 | `$state` retained + LWT=`lost` + `init→ready` 生命周期 | 【待设计】 |
| 2.4 | `<node>/<prop>/set` → 现有命令 → CommandManager | 【待设计】 |
| 2.5 | `LOG_REG_REBUILT` 回调 → 在合法时机重发 `$description` | 【待设计】 |
| 2.6 | **`$description` 体积与 PSRAM 占用实测** | 【待验证】 |

### P3 · 历史 / D1

| # | 任务 | 状态 |
|---|---|---|
| 3.1 | **遥测 → 历史策略定稿**（§7.3） | 【待设计】**前置** |
| 3.2 | D1 schema（`TelemetryHistory` / `LogHistory`，均含 `device_id` + `ts` 索引） | 【待设计】 |
| 3.3 | EMQX 数据集成规则（SQL 过滤 + HTTP Action → ingest Worker） | 【待设计】 |
| 3.4 | ingest Worker：批量 INSERT + 只拼接不解析 | 【待设计】 |
| 3.5 | 日志落库（依赖 BT-1 `log_ack` 修复） | 【待设计】 |
| 3.6 | **容量实测**：11 台设备实际行数 vs 预估 | 【待验证】 |
| 3.7 | 保留策略（TTL / 归档） | 【待设计】 |

### P4 · Web UI（MQTT-Tiles 改版）

| # | 任务 | 状态 |
|---|---|---|
| 4.1 | fork MQTT-Tiles（Vue2 + Quasar1） | 【待设计】 |
| 4.2 | **Homie Discovery Adapter**：`subscribe('homie/5/+/$state')` → `ready` → 取 `$description` → 动态生成 tile | 【待设计】 |
| 4.3 | **唯一 command id 生成**（button tile 每次发布新 `id`） | 【待设计】 |
| 4.4 | 登录 / 凭据获取（调 Worker） | 【待设计】 |
| 4.5 | WSS 8084 连接（SNI + `/mqtt` 路径 + CA） | 【待设计】 |
| 4.6 | 多设备切换 UI | 【待设计】 |
| 4.7 | 历史查询（调 Worker → D1） | 【待设计】 |
| 4.8 | 移动端自适应 | 【待设计】 |

### P5 · Cloudflare Pages + 安卓 WebView 壳

| # | 任务 | 状态 |
|---|---|---|
| 5.1 | Pages 部署（Wrangler） | 【待设计】 |
| 5.2 | WebView 壳（Android） | 【待设计】 |
| 5.3 | 本地通知 / 前台服务 | 【待设计】 |
| 5.4 | 证书 / WSS 真机验证 | 【待验证】 |
| 5.5 | 打包 APK | 【待设计】 |

### P6 · 原生安卓（**【后续可选】**）

仅当出现**明确需求**时才启动：后台常驻 MQTT、系统级推送、蓝牙配网、深度生命周期控制、本地能力、复杂账号/设备管理。
技术栈：Kotlin + **HiveMQ MQTT Client**（`com.hivemq:hivemq-mqtt-client`）。

---

## 11. 最小端到端验证路径（**【待验证】，P0–P2 完成后立即执行）**

这是**本方案的最低可交付验证**，用于在投入 P3–P6 前证明架构成立。

```
① ESP32 上电
    └─ 用 MAC 派生 device_id（如 a1b2c3d4e5f6）
    └─ 订阅 guo_feeder/a1b2c3d4e5f6/down
② EMQX 连接成功
    └─ 发布 $state = init → 发布 $description (retained) → 发布 $state = ready
    └─ LWT 设为 lost
③ 浏览器打开 MQTT-Tiles（WSS 8084）
    └─ 用临时凭据连接
④ Homie Discovery Adapter
    └─ subscribe('homie/5/+/$state') → 收到 ready
    └─ 取 homie/5/a1b2c3d4e5f6/$description
    └─ 解析 JSON → 渲染出 valve / weight / workflow 控件
⑤ 点击"开阀"控件
    └─ 发布 homie/5/a1b2c3d4e5f6/valve/set = "open"（带唯一 id）
⑥ 设备侧
    └─ Homie Bridge 收到 → 转为现有命令 → CommandManager → Workflow/Action → 电磁阀真开
⑦ 回传
    └─ 设备发布 homie/5/<id>/valve/state = "open" → 浏览器 UI 实时变化
```

**通过标准**：⑦ 完成 = **架构成立**（Homie 自描述 + 自动发现 + 控制闭环 + 设备身份 + Topic 隔离全部打通）。
**然后**再扩展：Worker 凭据（P1）→ D1 历史（P3）→ 多设备 → 安卓（P5）。

---

## 12. "暂不实施"清单（**【已冻结】，防止范围蔓延**）

| # | 不做 | 原因 |
|---|---|---|
| 1 | **不做完整原生安卓**（现在） | WebView 壳足够第一阶段 |
| 2 | **现在不装 Android Studio** | 除非进入 P5 |
| 3 | **不租 VPS** | 全托管够用 |
| 4 | **不用 Docker**（生产） | 无自建服务 |
| 5 | **不自建 broker** | EMQX Serverless 够用 |
| 6 | **不把 Worker 做成 MQTT 中转** | 违反控制/数据平面分离 |
| 7 | **不重构 CommandManager** | 复用即可 |
| 8 | **不重新设计 Action / Trigger / Workflow** | 复用即可 |
| 9 | **不重新设计 EventId / ParamId** | 冻结约定 |
| 10 | **不把 Homie 做成业务模型** | 它只是协议投影 |
| 11 | **不把 Homie Bridge 放进 CloudManager** | 职责分离（§2.2） |
| 12 | **不把 `device_id` 做成普通 ConfigManager 参数** | 防身份漂移（§13） |
| 13 | **不在 ACL 模型定稿前做临时凭据** | 否则返工 |
| 14 | **不假设 D1 免费额度能吃下全部历史** | 须先算账（§7） |
| 15 | **不把全部遥测原样写 D1** | 必先降频/聚合（§7.3） |
| 16 | **本轮不改 ESP32 源码 / 不 git commit** | 本轮只改架构文档 |

---

## 13. 设备身份（Device Identity）模型（**【已冻结】原则**）

### 13.1 定义

`device_id` **不是普通配置参数**，是**设备身份**：

| 要求 | 说明 |
|---|---|
| **稳定性** | 重启 / 升级 / 改配置后**不变** |
| **唯一性** | 全局唯一 |
| **来源** | **MAC 派生**（格式 `aabbccddeeff` 或 `gf-aabbccddeeff`，**统一小写**） |
| **持久化** | **首次开机生成并持久化** |
| **不可改性** | **禁止**通过 `update_config` 修改 |
| **可迁移** | 需单独的"设备身份迁移/恢复出厂"机制 |

### 13.2 作为全局公共键（**必须统一**）

同一个 `device_id` **必须**是下列全部系统的公共键：

```
device_id  ──┬──► Homie device-id
             ├──► MQTT Topic（guo_feeder/<device_id>/...）
             ├──► EMQX ACL（username = dev_<device_id>）
             ├──► D1 Device 表（唯一键）
             ├──► 用户↔设备绑定（UserDeviceBinding）
             ├──► APP 设备上下文
             ├──► 历史数据（TelemetryHistory.device_id / LogHistory.device_id）
             ├──► 日志
             └──► 云端注册
```

### 13.3 铁律

> **禁止出现"设备身份漂移"** —— 任何环节使用了**不同来源**或**不同格式**的设备标识（如 EMQX client_id、Topic 名、D1 主键不一致），都视为**架构缺陷**。

**已知风险点**：现有 `client_id = guo_feeder_001`（`data/config/mqtt.json`）与 MAC 无关。**P0 必须统一**：`client_id` 也应改为 `dev_<device_id>`（或至少保证与 `device_id` 一一对应）。

> ⚠️ **2026-10-02 严重度上调（不只是「漂移」）**：`client_id` 是 MQTT 的**全局会话标识** ——
> 两台设备用同一个 `client_id` 连同一 broker ⇒ **后连者触发 session takeover，把先连者踢下线**。
> 现象为「莫名重连 / 命令时好时坏」，**极难排查**。
> ⇒ 该项属 **P0 必需缺陷修复**（不是美化），见 `P0-设备身份与Topic隔离设计.md` §1.6。

---

## 14. 架构关键原则（10 条）

| # | 原则 | 说明 |
|---|---|---|
| **1** | **设备身份唯一且不可漂移** | `device_id` 是全局公共键，禁止多来源/多格式（§13） |
| **2** | **控制平面与数据平面分离** | Worker 只管"进门"（凭据/绑定），EMQX 承载"实时"（§2.1, §6） |
| **3** | **实时 ≠ 历史** | 实时走 EMQX 直连不落库；只有需回溯的才进 D1（§7.2） |
| **4** | **能力投影，不改业务模型** | Homie / Cloud 都是 Capability Registry 的**投影**，不新增业务概念（§2.2, §4） |
| **5** | **协议适配层与业务层并列** | Homie Bridge **不放进** CloudManager（§2.2） |
| **6** | **写入方向单入口** | Homie `set` / MQTT `down` → 统一走 CommandManager（§2.2） |
| **7** | **一切以 `device_id` 为隔离边界** | Topic / ACL / D1 / 绑定 / 历史，全部按 `device_id` 收窄（§3.2, §13.2） |
| **8** | **动态内存优先 PSRAM，不新增静态大数组** | 现有 RAM 压力已高（`bss ≈1.19MB`），Homie 缓冲全走 PSRAM（§4.3） |
| **9** | **免费额度是设计约束，不是事后优化** | 先算账再设计（§7.1 双 95% 已证实） |
| **10** | **前端演进：Web 先行，原生按需** | MQTT-Tiles → WebView 壳 → Kotlin（§10 P4–P6） |

---

## 15. 待确认决策点（**P0 开工前必须回答**）

> ### ✅ **本节 5 项已于 2026-10-02 定案** —— 详见 **`P0-设备身份与Topic隔离设计.md`**
>
> | 项 | 定案值 |
> |---|---|
> | **D1** `device_id` 格式 | **`aabbccddeeff`**（12 位小写 hex，无分隔、无前缀） |
> | **D2** 存储位置 | **NVS：复用现有 `nvs` 分区 + 独立 namespace `gfid`** ⚠️ **偏离下方"独立分区"建议** —— 实测 `partitions.csv` 的 flash **已 100% 分配**（无空闲空间），且 `device_id` 是 MAC 的**幂等派生值**（NVS 丢失**可重建**，**前提：未发生身份迁移、且云端绑定未变化** —— 见 P0 设计 §2.3 / §2.3.1）⇒ 独立分区无实际收益，代价却是"重排分区表 + 全片重烧" |
> | **D3** Topic 实现 | 方案 A（配置留模板 + `device_topic_render()`）；**回滚只改配置，不回退固件**（见 §3.1 补充） |
> | **D5** APP 凭据模型 | **slot 池 + API 改写 ACL**；`PUT /authorization/sources/built_in_database/rules/users/{username}` **已实测 200**（脚本 `emqx-api.mjs:211` 已封装）；⚠️ **授权缓存生效延迟仍待实测**（P1 时序须"先改 ACL 再让 APP 连"） |
> | **D7** 遥测策略 | **分层策略表已冻结**（§7.3）；**P0 不引入任何遥测配置项**（上报功能本身仍在 `readme.md` §六 待开发 ⇒ 随心跳功能落地，落点预登记 `mqtt.json.telemetry`，不新增 Config 模块） |
> | （附加）`client_id` | `dev_<device_id>` —— **性质为缺陷修复**（见 §13.3 的严重度上调注） |
>
> **下方原表保留**，作为决策时的**选项与权衡记录**（不再作为待办）。

| # | 决策项 | 选项 | 建议 |
|---|---|---|---|
| **D1** | **`device_id` 格式** | `aabbccddeeff` / `gf-aabbccddeeff` | 建议 `aabbccddeeff`（简洁、无分隔、便于主题匹配） |
| **D2** | **`device_id` 存储位置** | NVS 独立分区 / ConfigManager 只读字段 | 建议 **NVS 独立分区**（彻底与 ConfigManager 解耦，`update_config` 天然改不到） |
| **D3** | **Topic 实现方式** | 模板渲染（方案 A）/ 代码拼接（方案 B） | 建议 **方案 A**（§3.1） |
| **D4** | **Homie domain** | `homie` / `guofeeder` | 建议 **`homie`**（MQTT-Tiles 开箱发现，零改源码） |
| **D5** | **APP 凭据模型** | slot 池 + ACL 改写 / 每用户永久受限账号 / 长有效期凭据 | 建议 **slot 池**（安全性最好，需实测 EMQX API，§5.2） |
| **D6** | **Action 多参数映射** | 每个 param 独立 Property / 单个 `json` Property | 建议 **单个 `json` Property**（Property 数可控，UI 用表单） |
| **D7** | **遥测落库频率策略** | 设备侧控制 / EMQX 侧过滤 / ingest 侧聚合 | 建议 **EMQX 侧过滤 + 设备侧降频**（组合，§7.3） |
| **D8** | **legacy Topic 过渡期** | 立即切换 / 双发过渡 / 只读兼容 | 建议 **双发过渡**（§9.3） |
| **D9** | **前端框架确认** | MQTT-Tiles / Crouton | 建议 **MQTT-Tiles** |
| **D10** | **BT-1（`log_ack`）修复时机** | P0 / P3 | 建议 **P3**（落库前必须先修，否则 Flash 日志段永不回收） |

---

## 16. 六个核心问题自检

| # | 核心问题 | 本方案回答 | 章节 | 状态 |
|---|---|---|---|---|
| 1 | **多设备隔离** | Topic V3 带 `device_id` + ACL 按 `device_id` 收窄 + "拒绝 `#`"白名单兜底 | §3.2, §9, §13 | ✅ 已解决 |
| 2 | **设备身份** | MAC 派生 + 首启持久化 + `update_config` 拒绝修改 + 全局唯一公共键 + "禁止漂移"铁律 | §13, §10 P0.1–0.3 | ✅ 已解决（细节【待设计】） |
| 3 | **MQTT ACL** | Serverless 内置 ACL + `dev_<device_id>` / `app_slot_<n>` + placeholders `${username}` + API 改写 + 兜底拒绝 | §5, §3.2 | ✅ 已解决（**实现路径需实测**） |
| 4 | **Homie 与现有架构解耦** | Homie Bridge **独立并列**，只读 Registry，写入转 CommandManager，双协议并存，可随时关闭 | §2.2, §2.3, §2.4 | ✅ 已解决 |
| 5 | **Worker 请求量** | 实时不走 Worker；Worker 只做低频 IO；**已识别 ingest 是瓶颈**（11 台 95%）并给出降频方案 | §6, §7.1, §7.3 | ✅ 已解决（策略【待设计】） |
| 6 | **D1 历史数据** | 多设备 schema（6 表 + `device_id` 键）+ 容量核算 + 降频/聚合/批量写 + 保留策略 | §7, P3 | ✅ 已解决（**年存储需保留策略**） |
| 7 | **Web→Android 演进** | MQTT-Tiles → WebView 壳 → 原生 Kotlin（按需，HiveMQ Client） | §10 P4–P6 | ✅ 已解决（P6【后续可选】） |

**结论：7/7 均已给出明确架构回答**，其中 **3 项**（设备身份细节、ACL 实现路径、遥测策略）为 **【待设计】/【待验证】**，需在对应阶段开工前定稿。

---

## 附录 A：核实过的外部事实来源

| 结论 | 来源 |
|---|---|
| EMQX Serverless 仅 8883/8084，强制 TLS，WSS 需 SNI + `/mqtt` 路径 | `docs.emqx.io/en/cloud/latest/deployments/port_guide_serverless.html` |
| **EMQX Serverless 不支持 HTTP 认证** | `docs.emqx.com/en/cloud/latest/deployments/http_auth.html` |
| **EMQX Serverless 不支持 HTTP / 扩展授权** | `docs.emqx.com/en/cloud/latest/deployments/http_authz.html`、`custom_authz.html` |
| EMQX Serverless **支持内置认证 + 内置授权 + API + 批量 CSV** | `emqx.com/zh/blog/mastering-iot-solution-with-emqx-cloud-serverless-addition` |
| 默认授权三层级 + `${username}`/`${clientid}` 占位符 + 白名单须用"拒绝 `#`" | `docs.emqx.com/en/cloud/latest/deployments/default_authz.html` |
| 客户端不能订阅裸 `#`；授权规则可用 `#` 兜底 | 同上 |
| EMQX 数据集成（规则引擎 → HTTP）Serverless 可用 | `docs.emqx.io/en/cloud/latest/data_integration/http_server.html` |
| Workers 免费 100k req/天、10ms CPU、**网络等待不计 CPU** | `developers.cloudflare.com/workers/platform/limits/` |
| D1 免费 5M read / 100k write 行/天；KV 仅 1000 write/天 | `developers.cloudflare.com/workers/platform/pricing/` |
| Homie 5 主题 `homie/5/device/node/property`；domain 可自定义、版本段不可改；`$state` 需 retained + LWT | `homieiot.github.io`、`github.com/homieiot/convention` |
| MQTT-Tiles = MIT、Vue/Quasar、支持任意 WSS broker | `github.com/flespi-software/MQTT-Tiles` |
| Paho Android 停更（2018）、HiveMQ Client 支持 MQTT 5.0 | HiveMQ 官方 / 社区对比资料 |
| **Wrangler CLI** 覆盖 Workers/Pages/D1/KV/secret/tail | `developers.cloudflare.com/workers/wrangler/` |
| **MQTTX CLI**（`mqttx conn/sub/pub/bench`） | `mqttx.app/zh/cli` |

## 附录 B：引用的项目内既有资产

| 资产 | 位置 | 与本方案关系 |
|---|---|---|
| Cloud Protocol V2.0 | `cloud_protocol.md` | 旧链路契约，Homie 树**并存不替代**；§1.1 需补"模板渲染由固件实现"说明 |
| Capability Registry | `src/capability_registry.{h,cpp}` | **直接复用**为 `$description` 数据源；已有 PSRAM 优先分配模式（`:510-513`） |
| Action/Param 描述符 | `src/automation/workflow.h:91/145/157/241` | **Homie 元数据全部现成** |
| Workflow 云端契约 | `workflow_cloud_interface.md` | `variant` → `object_version` → `$description.version` |
| MQTT 配置 | `data/config/mqtt.json` | 需改 `subscribe/publish/log_topic` 为模板 + `client_id` 统一 |
| 已知问题 | `未修复的问题.md`、`cloud_protocol.md` §9 | BT-1（`log_ack`）/ DEF-2（凭据泄露）/ 9.7（版本未固定） |
| 内存铁律 | `AI_RULES.md` | 栈在内部 RAM、PSRAM 只做堆、>1KB 不上栈 |
| 部署脚本 | `docs/scripts/setup-cloud-app-env.ps1` | 默认 dry-run，**必须纯 ASCII** |

---

## 本次修订说明（v1 → v2）

### 修改的文件

| 文件 | 动作 |
|---|---|
| `docs/architecture/Cloud-APP-Platform-Plan.md` | **整体重写为 v2 正式架构规划**（本文） |
| （未改动任何 ESP32 源码；未执行 git commit） | — |

### 相比 v1 的主要变化

| # | 变化 | 依据 |
|---|---|---|
| 1 | **标题与定位**：从"平台选型与工具部署方案"升级为"正式架构规划 v2（P0–P6 实施依据）" | 用户要求"可作为 P0–P6 实施依据" |
| 2 | **新增 §1.4 ★ EMQX Serverless 认证/授权能力上限** | 本轮核实：Serverless **不支持 HTTP 认证/授权**（v1 未核实，属重大遗漏） |
| 3 | **新增 §2.1 最终架构关系图**（含 ingest Worker、双协议、Homie Bridge 并列） | 用户要求"最终架构关系图" |
| 4 | **§2.2 重大修订：Homie Bridge 移出 CloudManager**，改为**并列的独立投影适配层** + 架构图 | 用户【二】 |
| 5 | **新增 §2.3 Capability → Homie 投影规则表**（含 datatype/format/settable 生成规则） | 用户【二】要求"定义投影规则" |
| 6 | **新增 §3【Q1】Topic 改 device_id 改造量评估**（5 处读写点 + EMQX/CF 分工 + CLI 可用性） | 用户问题 1 |
| 7 | **新增 §4【Q2】Capability → Homie 改造量评估**（元数据已存在 + PSRAM 方案 + 实测 `bss≈1.19MB`） | 用户问题 2 |
| 8 | **新增 §5 EMQX 认证/授权模型**（User/Device/Binding/Credential）+ **slot 池 + API 改写 ACL** 实现路径 | 用户【三】 |
| 9 | **新增 §6 Worker 职责边界**（做/不做清单 + 实时正确路径 + D1 两条写入路径） | 用户【四】 |
| 10 | **新增 §7 容量核算**（11 台设备双 95% 告警 + 强制约束 + 遥测策略表） | 用户【五】 |
| 11 | **新增 §9 遗留协议兼容与迁移路径** | 用户【十二】 |
| 12 | **§10 P0–P6 全部细化**（P0 七个子项、P1 六个、P2 六个、P3 七个、P4 八个、P5 五个） | 用户【十】 |
| 13 | **新增 §11 最小端到端验证路径**（7 步） | 用户【十一】 |
| 14 | **新增 §12 "暂不实施"清单**（16 条） | 用户【十三】 |
| 15 | **新增 §13 设备身份模型**（MAC 派生 + 全局公共键 + "禁止漂移"） | 用户【一】 |
| 16 | **新增 §14 架构关键原则**（10 条） | 用户【十四】 |
| 17 | **新增 §15 待确认决策点**（D1–D10，含建议） | 用户要求"还缺哪些设计确认" |
| 18 | **新增 §16 六个核心问题自检**（7/7 已答） | 用户要求验证 |
| 19 | **§8 工具部署调整**：Android Studio 降级为**【后续可选】**；Wrangler / MQTTX CLI 升为**必装** | 用户【九】 |
| 20 | **全文增加状态标记**：**【已冻结】/【待设计】/【待验证】/【后续可选】** | 用户【十四】 |
| 21 | **§2.4 双协议并存**明确为**【已冻结】** | 用户【七】 |
| 22 | **§4.3 明确"不需要外置 Flash"** + PSRAM 放置表 | 用户问题 2 |
| 23 | **附录 A 新增**：EMQX HTTP auth/authz 不支持、默认授权占位符、Wrangler、MQTTX CLI | 本轮核实 |

### 当前已冻结的决策（不再讨论）

1. **设备身份**：MAC 派生 `device_id`，首启持久化，`update_config` 不得修改，全局唯一公共键，**禁止身份漂移**。
2. **Topic V3**：`guo_feeder/<device_id>/down|up|log`，设备只订阅自己的 `down`；**Topic V3 是量产前置**。
3. **Homie Bridge 位置**：**独立投影适配层，与 CloudManager 并列**，**不放进 CloudManager**。
4. **双协议并存**：`homie/5/<device_id>/...` 与 `guo_feeder/<device_id>/...` 永久并存，互不影响。
5. **Homie domain = `homie`**（非 `guofeeder`）。
6. **写入单入口**：Homie `set` / MQTT `down` → **统一经 CommandManager**。
7. **不新增 Action / Trigger / EventId / ParamId**；不重构 CommandManager / Workflow。
8. **控制/数据平面分离**：Worker 不做 MQTT 转发。
9. **实时 ≠ 历史**：实时走 EMQX 直连，历史才进 D1。
10. **内存策略**：Homie 相关缓冲全走 **PSRAM 动态分配**，**不新增静态大数组**；**不引入外置 Flash**。
11. **前端**：MQTT-Tiles 为第一阶段 UI 基座（非最终 App 架构），**必须**实现 Homie Discovery Adapter + 唯一 command id。
12. **安卓**：WebView 壳先行，原生 Kotlin 仅按需。
13. **ACL 必须白名单化**（Serverless 无开关 ⇒ 用"拒绝 `#`"兜底）。
14. **"暂不实施"清单**（§12）16 条，防范围蔓延。

### P0 开工前仍缺的设计确认（**8 项**）

| # | 待确认 | 为什么阻塞 P0 |
|---|---|---|
| 1 | **`device_id` 格式**（D1） | 直接决定 Topic / ACL / D1 全部字符串格式，**改了要全线返工** |
| 2 | **`device_id` 存储位置**（D2） | 决定"NVS 独立分区"还是"Config 只读字段"的实现路径 |
| 3 | **`client_id` 是否同步改为 `dev_<device_id>`** | 若不改，"身份漂移"风险残留 |
| 4 | **Topic 实现方式**（D3：模板渲染 / 代码拼接） | 决定 `config_manager` 是否新增渲染函数 |
| 5 | **EMQX API 能否按 `username` 改写 ACL**（§5.2 疑问 3） | **P1 的根本前提**，需实测；若不能，D5 必须改方案 |
| 6 | **APP 凭据槽位数 N 与租约模型**（D5） | 决定 D1 `Credential` 表结构与 Worker 逻辑 |
| 7 | **遥测落库频率策略**（D7） | 虽属 P3，但**设备侧降频需要 P0 一起设计配置项**，否则又要动固件 |
| 8 | **legacy Topic 过渡期长度**（D8） | 决定双发实现与 ACL 过渡规则 |

> **建议：D1/D2/D3/D5/D7 五项先定，即可开工 P0。**

---

*（本文件为架构规划文档，未涉及任何 ESP32 源码改动。下一步行动以 §10 P0 为准。）*
