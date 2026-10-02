# 交接文档 · Guo Feeder Project

> **用途**：跨会话 / 跨人交接。新开会话时**先读本文件**，再按 §1 的清单读对应文档。
> **最后更新**：2026-10-02

---

## 1. 给下一个会话的启动指令（直接复制粘贴）

```
请先读下列文档了解本项目的当前状态，读完先复述你的理解，等我确认后再动手。

【必读·按顺序】
1. D:\Guo_Feeder_Project\PIO_Assets\Guo_Feeder_Project\HANDOFF.md            ← 本文件优先
2. D:\Guo_Feeder_Project\PIO_Assets\Guo_Feeder_Project\readme.md             ← 项目总说明（架构/模块/目录/规范）
3. D:\Guo_Feeder_Project\PIO_Assets\Guo_Feeder_Project\AGENTS.md             ← AI 工程速览（构建/烧录/调试/铁律）
4. D:\Guo_Feeder_Project\PIO_Assets\Guo_Feeder_Project\docs\README.md        ← 文档总索引
5. D:\Guo_Feeder_Project\PIO_Assets\Guo_Feeder_Project\docs\architecture\Cloud-APP-Platform-Plan.md
                                                                             ← 云端/APP 架构 v2 权威方案（P0–P6 依据）
6. D:\Guo_Feeder_Project\PIO_Assets\Guo_Feeder_Project\docs\issues\未修复的问题.md
                                                                             ← 全项目问题权威清单
7. D:\Guo_Feeder_Project\PIO_Assets\Guo_Feeder_Project\.workbuddy\memory\MEMORY.md
   D:\Guo_Feeder_Project\PIO_Assets\Guo_Feeder_Project\.workbuddy\memory\MEMORY-cloud.md
   D:\Guo_Feeder_Project\PIO_Assets\Guo_Feeder_Project\.workbuddy\memory\2026-10-02.md
                                                                             ← 长期笔记 + 最近工作日志

【按需查阅】★ P0 施工时必看第 8 项
8. D:\Guo_Feeder_Project\PIO_Assets\Guo_Feeder_Project\docs\architecture\P0-设备身份与Topic隔离设计.md
                                                                             ← ★ **P0 实施级设计**（D1/D2/D3/D5/D7 已定案 + 改造清单 + 上线顺序 + 验收）
9. D:\Guo_Feeder_Project\PIO_Assets\Guo_Feeder_Project\docs\interfaces\cloud_protocol.md      ← MQTT 协议唯一权威
10. D:\Guo_Feeder_Project\PIO_Assets\Guo_Feeder_Project\docs\interfaces\workflow_cloud_interface.md
11. D:\Guo_Feeder_Project\PIO_Assets\Guo_Feeder_Project\AI_CONTEXT.md / AI_RULES.md

【背景】当前处于「云端/APP 架构落地」阶段，下一步是 P0–P6（详见 HANDOFF.md §4）。
我（用户）是 ESP32 固件开发者，习惯先看文档再动手；改动前请先说明方案。
构建/烧录/串口调试的完整命令与已知坑见 AGENTS.md。
```

---

## 2. 项目是什么

**ESP32-S3 N16R8 全自动宠物投喂 / 供水设备**（当前主攻自动猫咪饮水）。

- 架构：模块化分层 + 非阻塞状态机 + 全局 System State 中心 + Event 机制
- 硬约束：禁止 `delay()` / `while` 死等；禁止跨模块直接读写内部变量；零全局裸变量
- 版本：`v0.7 + 云端架构 v2`（2026-10-02）

---

## 3. 三个独立仓库（全部已在 GitHub）

| 资产 | 本地路径 | GitHub 远程 | 可见性 | 分支 |
|---|---|---|---|---|
| **固件** | `D:\Guo_Feeder_Project\PIO_Assets\Guo_Feeder_Project\` | `Guo-Baby/Guo_Feeder_Project_PIO_Assets` | public | `wb` |
| **云端** | `D:\Guo_Feeder_Project\Cloudflare_Assets\` | `Guo-Baby/Guo_Feeder_Project_Cloudflare_Assets` | private | `main` |
| **消息** | `D:\Guo_Feeder_Project\EMQX_Assets\` | `Guo-Baby/Guo_Feeder_Project_EMQX_Assets` | private | `main` |
| 工作区 | `D:\Guo_Feeder_Project\GuoFeeder.code-workspace` | — | — | 多根工作区（不在任何仓库内） |

> ⚠️ 固件仓库的旧 URL `Guo-Baby/Guo_Feeder_Project` 是新名的**重定向**，仍可用。
> ⚠️ 固件仓库**默认分支是 `main`**（远程另有 `wb` / `emqx` 两个分支），本地工作在 `wb`。

---

## 4. 当前进度

### 4.1 固件侧（`wb` 分支）

| 项 | 状态 |
|---|---|
| `src/` 分层 | ✅ **7 个物理子目录**（`app/ automation/ services/ storage/ log/ cloud/ test/`），`main.cpp` 留根；121 处 include 已改显式路径 |
| 文档归位 | ✅ 根目录 33 → 10 条目；文档进 `docs/{requirements,architecture,interfaces,modules,specs,issues,scripts}`；历史进 `docs/archive/{log,workflow,legacy}` |
| LogManager | ✅ **P2 已完成**（16 模块接入，冻结）；细节见 `docs/specs/LogManager-Integration-Guide.md` |
| TimeManager V2 | ✅ 完成；⚠️ **板子未焊 PCF8563T，`rtc.json` 的 `enable=false`**（焊上后改 `true`） |
| Workflow / Config / Command / Cloud | ✅ 可用（含云端 Workflow 编辑协议） |
| **P0（身份 + Topic V3）** | ✅ **已完成、已上板、已端到端验证** —— `9825b09` DeviceIdentity · `5c4ffb6` TopicRenderer · `607b16d` client_id + 渲染接入 · `17e459c` ACL（`EMQX_Assets`，**已 apply 到线上**）· `9e07d6d` 用例集 + 工具适配。累计 **RAM +88 B / Flash +7,040 B**。**端到端实测**：经 MQTT 向 `guo_feeder/<device_id>/down` 下发 memory / flash / config_query 三条只读命令，设备均正确执行并回包 |

>
> **P0 验收结果（COM8 实机 · 2026-10-02）**
>
> | 用例 | 结果 |
> |---|---|
> | S-1 探活 · S-2 首次启动 · S-3 重复启动 | ✅ |
> | S-4 NVS 丢失 → **重建同一 device_id** | ✅ |
> | S-6 topic 渲染（client_id + 三 topic 全 V3，零 WARN） | ✅ |
> | S-7 回滚通路（legacy 模板 ⇒ 原样放行 + 一次 WARN，仅 dev/test） | ✅ |
> | S-8 client_id 实值（`dev_<id>`；配置回包**不含** device_id 字段） | ✅ |
> | S-10 命令链路回归 · S-11 升级（重烧 app 分区）后身份不变 | ✅ |
> | M-1 单设备上线 + MQTT 下发（memory / flash / config_query） | ✅ |
> | M-3 越权被拒（设备账号 SUBACK = `(1,128,1)`：自己 down ALLOW / 他人 down DENY） | ✅ |
> | **S-5 did 与 MAC 不一致 · S-9 client_id 超长** | ⛔ 实测**不可构造**（见下） |
> | M-2 双设备互踢 | ⏳ 无第二台设备 |
>
> **S-5 不可构造**：等长篡改 NVS 镜像中的 `did` 后写回 ⇒ 设备报 `nvs_get_str fail: did NOT_FOUND`
> （**NVS 自带完整性校验**）；且固件不提供任何改 `did` 的接口 ⇒ 该场景在正常运维路径下不可达。
> **S-9 不可构造**：配置命令链路**先行拒绝**超长值（`CONFIG_ENQUEUE_INVALID` → `e:6`，
> 随后 `config_save` 报 `no dirty module`）⇒ 该分支由编译期自检 `topic_renderer_selftest` 的
> `too_long` 用例覆盖。
>
> **仍未做**：**C-3**（`platformio.ini` 固定版本）· M-2（需第二台设备）·
> P1 前置实测（授权缓存生效延迟 `A-1`、`client_id` broker 长度上限 `A-4`、一机一凭据）

| Mijia BLE 温湿度计 | ⏳ 解码算法已分析，待正式集成 |
| Cloud Protocol CBOR | ⏳ 已验证，待正式整合进协议 |
| **编译状态** | ✅ 通过；**RAM 39.9%（130,824 B，P0 后 +88 B）/ Flash 65.8%（1,379,568 B，P0 后 +7,040 B）** |

> ✅ **2026-10-02 已推送**：`328d44f..84d8ff6  wb -> wb`（一次性推上 53 个提交，
> 含 10-01 目录重构与本轮 readme/HANDOFF 改动）。remote 已由旧 URL
> `Guo_Feeder_Project` 更名为 `Guo_Feeder_Project_PIO_Assets`（旧名仍可访问，自动重定向）。
>
> ✅ **同日 `wb` 已快进合并进 `main`**：`8c51a32..c6a68c4  wb -> main`。
> 现在 **`main` / `wb` / `origin/main` / `origin/wb` 四个引用全部指向 `c6a68c4`**，
> 远程默认分支 `main` 即最新代码。

### 4.2 云端侧（`Cloudflare_Assets`）

| 资源 | 值 |
|---|---|
| Worker | `guo-feeder-api` → `https://guo-feeder-api.guobaby.workers.dev/` |
| Pages | `guo-feeder-pagesdev` |
| D1 | `guofeeder`（`0d8385ba-a1a6-4fcc-8e43-ae61c014c43d`），绑定名 `guofeeder_DB`，现有表 `mqtt_messages` |
| CF 账号 | `9e72d201efec6925b3a468a0e3fb4928` |
| Secrets | `EMQX_APP_ID` / `EMQX_APP_SECRET`（**已同步为新 Key**）/ `WEBHOOK_SECRET`（代码里仍是注释） |
| 工具链 | `wrangler 4.145.0`（装在项目内 `node_modules`，未污染全局） |

**已知待改**（对照 P1/P3）：主题硬编码无 `device_id`、webhook 鉴权被注释、无登录/绑定/凭据签发、**每条消息一行 INSERT**（11 台即打满 D1）、一张 `mqtt_messages` 装所有消息。

### 4.3 消息侧（`EMQX_Assets`）

| 项 | 值 |
|---|---|
| 部署 | `n302933b.ala.cn-hangzhou.emqxsl.cn`（Serverless，仅 8883 mqtts / 8084 wss） |
| ACL | ✅ **已升级到 Topic V3**：新增 `config/acl-rules.v3.json` + `apply-acl.mjs --device-ids`（**已 apply**）；设备账号规则 3 → 6 条（3 条 V3 + 3 条 legacy 过渡）；兜底 `全部用户 → # → deny` 不变 |
| 脚本 | `scripts/{emqx-api,check-connection,diagnose-api,apply-acl}.mjs` |

**ACL 现状**：

| 账号 | 允许 |
|---|---|
| `GuoFeederDevice` · `GuoFeederDevice001` | sub/pub `guo_feeder/<device_id>/{down,up,log}`（**逐设备枚举，禁通配**）+ legacy 三条（过渡期，**仅 dev/test**） |
| `workbuddy` · `test001` · `shouji` | all `guo_feeder/#`（调试用，未来收窄） |

> ⚠️ EMQX Serverless **不支持外部 HTTP 认证 / 扩展授权 / 白名单开关** ⇒ P1 的凭据模型只能是
> **「预置账号 + Worker 用 EMQX API 改写该账号 ACL」的 slot 池**。详见 `MEMORY-cloud.md`。

---

## 5. 下一步：P0–P6

> 完整定义与验收标准见 **`docs/architecture/Cloud-APP-Platform-Plan.md`**。

| 阶段 | 内容 |
|---|---|
| **P0** ✅ | 设备身份（MAC 派生 `device_id` + NVS 持久化 + 禁漂移）· **Topic V3** `guo_feeder/<device_id>/...` · ACL 隔离 —— **已完成** |
| **P1** | EMQX 认证 + 用户/设备绑定 + 凭据签发（slot 池） |
| **P2** | **Homie Bridge**（与 CloudManager **并列**的投影适配层，写入单入口经 CommandManager） |
| **P3** | 历史/D1（多设备数据模型 + 遥测降频策略） |
| **P4** | Web UI（MQTT-Tiles + Homie Discovery Adapter） |
| **P5** | Cloudflare Pages 部署 + 安卓 WebView 壳 |
| **P6** | 原生 Android（**【后续可选】**，现在不装 Android Studio） |

### ★ P0 开工前的设计确认 —— **已定案（2026-10-02）**

> **★ 施工依据 = `docs/architecture/P0-设备身份与Topic隔离设计.md`**
> （含**精确到行号**的改造清单 · device_id 派生与 NVS 决策表 · 渲染契约与失败语义 ·
> EMQX ACL（V3 兼容版）规则集 · 上线顺序与回滚 · 验收标准 DoD · 风险登记）

| # | 决策 | 取值 | 状态 |
|---|---|---|---|
| **D1** | `device_id` 格式 | **`aabbccddeeff`**（12 位小写 hex，无分隔、无前缀） | ✅ 已定 |
| **D2** | 持久化位置 | **NVS：复用现有 `nvs` 分区 + 独立 namespace `gfid`**（⚠️ 偏离 Plan 建议的"独立分区"——实测 flash 已 100% 分配；且 `device_id` 是 MAC 的**幂等派生值**（**仅在未迁移、未绑定时**可重建）⇒ 无独立分区必要） | ✅ 已定 |
| **D3** | Topic 实现方式 | **方案 A：配置留模板 + `device_topic_render()` 渲染**（最大价值：**回滚只改配置，不回退固件**） | ✅ 已定 |
| **D5** | EMQX 按 `username` 改写 ACL | **可通**：`PUT /authorization/sources/built_in_database/rules/users/{username}` **实测 200**，`emqx-api.mjs` 已封装 `replaceUserAclRules()`；⚠️ **授权缓存生效延迟待实测**（P1 签发时序必须"先改 ACL 再让 APP 连"） | ✅ 已定 |
| **D7** | 遥测落库策略 | **分层策略表已冻结**（目标 <1200 行/设备/天）；**P0 不引入任何遥测配置项**（上报功能本身仍在 `readme.md` §六 待开发 ⇒ 随"Device State Heartbeat"一起落地，落点预登记为 `mqtt.json.telemetry`，**不新增 Config 模块**） | ✅ 已定 |
| — | `client_id` 改 `dev_<device_id>` | ✅ 已定 —— **性质是缺陷修复**：固定 `guo_feeder_001` ⇒ 第二台设备上电触发 **MQTT session takeover 把第一台踢下线**（表现为"莫名重连"，极难排查） | ✅ 已定 |
| C-3 | `platformio.ini` 固定版本（`DD-5` / `9.7`） | 建议并入 P0（4 行，独立 commit） | ⏳ **仍未做**（待点头） |
| C-4 | 测试工具适配 V3（`tools/mqtt_*.py` · `test/mqtt_log_probe.py` 等**硬编码 legacy topic**） | ✅ **已完成** —— 并入 **P0-5** `9e07d6d`（4 个工具加 `--device-id`，**不提供通配回退**） | ✅ 已完成 |
| 6 | APP 凭据槽位数 N 与租约模型 | P1 开工前定 | ⏳ P1 |
| 8 | legacy Topic 过渡期长度 | 以"P1 凭据 + P3 落库"完成度为终点（现在定天数无依据） | ⏳ 待定 |

> **⇒ P0 已完成**（P0-1..P0-5 全部提交，并完成上板与 MQTT 端到端验证）；
> 仅 **C-3（`platformio.ini` 固定版本）** 未做 —— 不影响主链路，可随时补。
>
> **★ 排障铁律（P0 期间实测）**：`[Cloud] MQTT subscribed` 日志 **≠** 订阅被授权。
> EMQX 白名单模式下，未被显式 allow 的 topic 会落到兜底 `# deny` ⇒ EMQX clients 里
> `subscriptions=0`（设备收不到任何下行），但固件照样打 `subscribed`。**必须以 `subscriptions` 为准**。
> 且 ACL 改完**不会**让已建立的连接自动重订阅 ⇒ 需重连（无串口时可用 `DELETE /clients/{clientid}` 踢连接）。

---

## 6. 环境要点（省得重新摸索）

### ★ 当前开发板硬件状态（2026-10-02）

| 硬件 | 状态 | 对应配置 |
|---|---|---|
| 电子秤 **HX711** | **未安装** | `data/config/weight.json` → `"enable": false` |
| **PCF8563T RTC** | **未焊接** | `data/config/rtc.json` → `"enable": false` |

**★ `weight.enable` 的由来（实际踩过的坑）**：
HX711 未接入时 DOUT 悬空 ⇒ `scale.is_ready()` 恒 false ⇒ `weight_task()` 每 5s 发一次
`event_push(EVENT_WEIGHT_ERROR, "HX711 not ready")` ⇒ `DispenseGuard` 反复
`valve_force_close()`，串口每 5s 刷三行日志、阀门被反复强关。

**装秤后**改回 `"enable": true` 并重做零点校准（`WEIGHT_ZERO` action / `config_set` + `config_save`）。

> **为什么不做"自动检测传感器是否存在"**：固件无法区分「压根没装 HX711」与
> 「装了但 DOUT 断线 / 芯片损坏」—— 两者都表现为"从未就绪"。对"从未就绪"一律静默
> = **秤坏了也不报警**（安全降级）。故必须由使用者显式声明。
>
> **置 false 的连带效果**：不注册 `weight_decrease` Trigger 与 `WEIGHT_ZERO` Action
> （与 `valve_init()` 的 `!enable` 处理同构）⇒ `registry_version` 会变化，
> 云端 / APP 应**重新拉取 registry 缓存**，切勿写死 `stable_id`。

> ⚠️ **`data/` 已被 gitignore** ⇒ 上述配置是**设备端 / 本机**配置，**不在版本控制内**。
> 改设备端需走 MQTT：`{"c":"system","i":"<唯一id>","p":{"o":"config_set","module":"weight","key":"enable","value":true}}`
> 然后 `{"c":"system","i":"<唯一id>","p":{"o":"config_save"}}`
> （见 `docs/interfaces/config_manager接口文档.md` §4.2 / §4.5）；
> 或 `pio run -t uploadfs`（**会整区重建 LittleFS**，设备端独有文件会丢，慎用）。

### GitHub / 代理（**最重要**）
```bash
# 本机 GitHub 出口必须是这个！沙箱注入的 3386 代理会覆盖系统代理并导致 502
export https_proxy=http://127.0.0.1:7890 http_proxy=http://127.0.0.1:7890
export HTTPS_PROXY=$https_proxy HTTP_PROXY=$http_proxy
```
- `gh` 已装在 **`D:\tools\gh\bin`**（已在用户 PATH），**已持久化登录**（`Guo-Baby`，token 存系统 keyring）
- `api.github.com` 是唯一可直连的例外

### 固件构建 / 烧录 / 调试
详见 **`AGENTS.md`**。要点：
- 沙箱会拦删 `.o`/`.elf` ⇒ 用**独立构建目录**：`PLATFORMIO_BUILD_DIR=.pio/build/xxx pio run`
- `pio run -t upload` 可能不烧录 ⇒ 改用 `esptool.py write_flash -z 0x10000 <firmware.bin>`
- 串口回归：`python test/serial_batch.py <COM> <log> <用例txt>`
- 全量重建索引：`pio run -t compiledb`（**全新 clone 后必须先跑**，否则 clangd 无索引）

### 常见坑（详见 `.workbuddy/memory/MEMORY.md`）
- `.gitignore` **不支持行尾注释**；且**只对未跟踪文件生效**（已跟踪的必须 `git rm --cached`）
- 提交前必须 `git diff --cached --name-only` **全量核对**（`git add` 路径范围会悄悄扩大）
- `.ps1` 脚本**必须纯 ASCII**（PowerShell 5.1 按 ANSI 解码）
- npm 安装中断会导致 optional 平台包永久缺失 ⇒ 只能删 `node_modules` + 锁文件干净重装

---

## 7. 待决事项（本轮结束时挂起）

1. ~~固件仓库的默认分支问题~~ ✅ **已解决（2026-10-02）** —— `wb` 已**快进**合并进 `main` 并推送
   （`8c51a32..c6a68c4`）。因 `origin/main` 本就是 `wb` 的祖先，为**纯快进**：
   无冲突、无 merge commit、历史线性。GitHub 首页（默认分支 `main`）现为最新代码。
   > 顺带确认：其余本地分支 `emqx` / `emqx_text` / `deepseek_dev` **也都是 `wb` 的祖先**
   > （独有 0 提交），即全部内容都已被 `wb` 包含。
2. **P0 的五项设计确认**（D1 / D2 / D3 / D5 / D7，见 §5）。
3. 固件仓库历史中的 **明文 MQTT 凭据**尚未轮换（计划在 P1「Topic V3 + 一机一密」时一并处理）。

---

## 8. 本轮（2026-10-02）完成清单

- ✅ 新建 **`EMQX_Assets`** 仓库骨架（REST 封装 + 自检 + 诊断 + ACL 套用脚本）
- ✅ 为 **`Cloudflare_Assets`** 建立 git 追踪并提交
- ✅ 创建 **`GuoFeeder.code-workspace`** 多根工作区
- ✅ 安装 **GitHub CLI**（`D:\tools\gh`）+ 完成持久化登录
- ✅ 创建两个 GitHub 私有仓库并推送
- ✅ **套用 EMQX ACL 修复**（5 用户 / 9 规则，白名单语义成立）
- ✅ 实测 EMQX Serverless API 边界（认证/ACL 路径可用，`/authentication` 列表 403）
- ✅ 修正 **`readme.md`** 9 处过时点 + 移出 1251 行重复协议章节（→ `docs/archive/legacy/`）
- ✅ 记忆维护：拆出 **`MEMORY-cloud.md`**，`MEMORY.md` 由 40 KB 降至 29 KB
- ✅ **固件仓库首次推送**（`wb` 分支 53 个提交 → GitHub），remote 更名为真实仓库名
- ⚠️ 踩坑：`GH_TOKEN=...` **未加 `export`** ⇒ git 内联凭据 helper 取不到变量 ⇒
  `Invalid username or token`。**必须 `export GH_TOKEN=...`**
- ✅ **`wb` 快进合并进 `main`**（`8c51a32..c6a68c4  wb -> main`），四个引用统一；
  采用「推送 + `git branch -f main wb`」而非切分支 merge，**避免工作区在 164 个提交间来回切换**

**关联提交**：
- `EMQX_Assets`：`3415211`（API 边界实测 + 修正探针 + ACL 套用脚本）
- `Cloudflare_Assets`：`1d6fe00`（凭据同步脚本）、`4751bc9`（建立 git 追踪）
- 固件仓库：`d9c8cdd`（文档归位）· `ca89176`（gitignore 治理）· `f5b5414`（src 分层）+ 本轮 readme/HANDOFF 提交
