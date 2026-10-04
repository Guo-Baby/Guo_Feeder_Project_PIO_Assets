# 交接文档 · Guo Feeder Project

> **用途**：跨会话 / 跨人交接。新开会话时**先读本文件**，再按 §1 的清单读对应文档。
> **最后更新**：2026-10-04

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
> **仍未做**：M-2（需第二台设备）。
> ✅ **C-3 已完成**（`9ec321d`）：`platformio.ini` 锁定 `platform=espressif32@7.0.1` + 8 个
> `platform_packages` + 4 个库精确版本，`firmware.bin` 逐字节等长（零漂移）。
> ✅ **P1 前置实测已完成** —— 见下方「★ P1 进度」。

| Mijia BLE 温湿度计 | ⏳ 解码算法已分析，待正式集成 |
| Cloud Protocol CBOR | ⏳ 已验证，待正式整合进协议 |
| **编译状态** | ✅ 通过；**RAM 39.9%（130,824 B，P0 后 +88 B）/ Flash 65.8%（1,379,568 B，P0 后 +7,040 B）** |

### ★ P1 进度（2026-10-04 更新）

| 工作包 | 状态 |
|---|---|
| **P1-0** 平台能力实测 | ✅ **完成** —— **A-4 / A-4b / A-5 / A-1 / A-1c / A-6 / A-7 / A-8 全部实测完毕**（`288cd95` · `a0e7164` · **`fece474`**） |
| **P1-0** 结论冻结 | ✅ `0255791` —— `P1-0-EMQX-Capability-Test.md` 新增 **§Design Impact Freeze** · **§A-7** · **§A-8（PUT 改密 + body schema）** |
| **P1-1** D1 Schema | ✅ **完成**（`75f84ab`），**已 apply 到远端 D1**（`--local` + `--remote` 各 `15 commands` 成功）——`device` / `device_credential` / `device_binding` / `device_event` |
| **Phase C**（P1-2 + P1-4） | ✅ **完成：设计冻结**（`01f27bf` · **`10b6a57`（Q1–Q8）** · `4a5449a`（Q9–Q13））—— `P1-2-P1-4-注册与凭据签发设计.md` 文末 **§Architecture Freeze Decision** |
| **Phase D-1** 云端基础能力 | ✅ 完成（Cloudflare 仓 `21de1c8`）—— `emqx-admin` / `admin-auth` / `credential-gen` / `device-registry` |
| **Phase D-2** 固件凭据管理 | ✅ 完成（`004ebf0`）—— `cred_store` + `credential_manager` + `credential_set` + Q13 source 门控 |
| **Phase D-3** 真机联调 | ✅ **完成（2026-10-04）** —— **51/51 全绿**；云端签发闭环 + 管理端点 + ingest 建档/确认；固件修 **2 处真实缺陷**（见下） |
| **P1-5** 设备绑定（Binding） | ✅ **实现完成（2026-10-04）** —— **P1-5-1** `binding.js` · **P1-5-2** Admin API 5 端点 · **P1-5-3** **INV-4：退役必关 binding** · **P1-5-4** 文档同步。离线自测 **276 PASS / 0 FAIL**。收口评审 → `docs/architecture/P1-5-Final-Review.md`。**P1-5-5 正式验收（真实 D1 + 部署版 Worker）待做** |
| P1-6 / P1-7 | ⏳ 未开始（见 `P1-实现清单.md`） |

#### ★ P1 关键冻结事项（改代码前必读）

#### ★ D-3 联调暴露并修复的两处**真实缺陷**（改代码前必读）

| # | 层 | 缺陷 | 现象 | 修法 |
|---|---|---|---|---|
| **1** | 固件 `cloud_manager.cpp` | **换连前未等 outbox 排空** | promote 后立即 `stop+destroy` 重建客户端 ⇒ 刚 `enqueue` 的 `credential_confirm`（QoS1）**被一起销毁**；串口有 `[Cloud UP] OK`，云端**永远收不到** ⇒ 凭据停在 `PROVISIONING`、300 s 后 `timeout:confirm` | 身份变化时先进入「等 outbox 排空」状态（**非阻塞**：`outbox==0 && ≥400ms` 或 `≥2500ms` 上界）再重建；串口标志 `[Cloud] credential switch: outbox=N -> rebuild` |
| **2** | 云端 `credential.js` | **把「信息帧」误判为失败帧** | 设备的 `ack` / 受理帧也带 `o:"credential_set"` 但**无 `confirmed`**；旧实现一律写 `provision_error='device:unknown'` ⇒ 真正确认帧到达时被**硬失败守卫**拒绝（`acl_ready_at` 永远为 NULL） | 新增 `classifyCredentialFrame()`：`confirmed:true` ⇒ confirm；`confirmed:false` / `s>=3` / `status∈{failed,error,timeout}` ⇒ failure；**其余一律 info（不动状态）** |

**① EMQX `create user`（P1-0 §A-7 实测）**

| 行为 | 值 |
|---|---|
| 首次创建 | **`201`** + `{user_id, is_superuser}` |
| 已存在 | **`409`** `{"code":"ALREADY_EXISTS"}` ——**不覆盖密码**、不改 ACL、不重复建号 |
| 结论 | **无需"先 GET 再 CREATE"**（`409` 即判别号）；**可 at-least-once retry**（崩溃 + 原样重试安全，上限 5 次指数退避） |
| 回收重签 | `DELETE` 用户后可**同名重建**（`201`）；`DELETE .../rules/users/{u}` 返回 `204` |

**② ACL propagation（P1-0 §A-1 / §A-1c 实测）**

- **新连接**约 **1.35 s** 后受新规则影响（收紧 1.35 / 恢复 1.37 s，无长尾）。
- **已连接 session 不自动撤销** —— 既有会话既不重鉴权、连新的 `subscribe` 也仍放行。
- ⇒ **revoke 必须 = `update ACL` + `session termination`（`DELETE /clients/{clientid}`）**，缺一不成立。

**③ credential provisioning（Phase C Q1–Q8 冻结）**

- **ACL readiness 的唯一硬证据 = 设备真实 CONNACK 成功**（设备用该凭据建连成功）。
- **`HTTP 2xx` / `GET` 查规则 / 固定等待时间 均不能作为 `ACTIVE` 条件**；
  3 s 等待只能用作 retry/backoff 节奏，**不能改变状态**。
- 附带：`system.credential_set` 只允许 cloud provisioning path；password 只能 `cloud → device`；
  `confirm timeout = 300 s` / `retry = 5`，四态 `PENDING / CONFIRMED / FAILED / TIMEOUT`。

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
| D1 | `guofeeder`（`0d8385ba-a1a6-4fcc-8e43-ae61c014c43d`），绑定名 `guofeeder_DB`。表：`mqtt_messages`（P0 遗留）+ **`device` / `device_credential` / `device_binding` / `device_event`（P1-1，2026-10-03 已 apply 到远端）** |
| CF 账号 | `9e72d201efec6925b3a468a0e3fb4928` |
| Secrets | `EMQX_APP_ID` / `EMQX_APP_SECRET`（**已同步为新 Key**）/ `WEBHOOK_SECRET`（代码里仍是注释） |
| 工具链 | `wrangler 4.145.0`（装在项目内 `node_modules`，未污染全局） |

**已知待改**（对照 P1/P3）：~~无绑定~~ ✅ **P1-5 已实现**（bind / unbind / transfer / 查询 + **INV-4**；见 `P1-5-Final-Review.md`）；**仍缺 APP 用户体系**（`user_id` 为占位身份）与**在线状态**（Phase F）；主题硬编码无 `device_id`、webhook 鉴权被注释、**每条消息一行 INSERT**（11 台即打满 D1）、一张 `mqtt_messages` 装所有消息。

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
> **「程序化建账号 + Worker 用 EMQX API 改写该账号 ACL」**。
> ✅ **P1-0 A-5 实测已推翻旧的「slot 池」方案**：账号可程序化创建/删除（上限 ≈2000）
> ⇒ 采用 **「一机一账号」**（`dev_<device_id>`），**不需要 slot 池**。详见 `MEMORY-cloud.md`。

---

## 5. 下一步：P0–P6

> 完整定义与验收标准见 **`docs/architecture/Cloud-APP-Platform-Plan.md`**。

| 阶段 | 内容 |
|---|---|
| **P0** ✅ | 设备身份（MAC 派生 `device_id` + NVS 持久化 + 禁漂移）· **Topic V3** `guo_feeder/<device_id>/...` · ACL 隔离 —— **已完成** |
| **P1** | **设备生命周期管理**：注册（`CLAIM_PENDING` 防抢注）· 一机一凭据（建账号 + ACL + 轮换/回收）· 绑定 / 在线状态 · 生产模式检查 —— **Phase A（P1-0）✅ · B（P1-1）✅ · C（P1-2/P1-4）✅ · D（P1-3 凭据）✅ · E（P1-5 绑定）✅（2026-10-04）；剩 Phase F（P1-6 在线状态）· G（P1-7 生产检查）· P1-8 验收** |
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
2. ✅ ~~**P0 的五项设计确认**（D1 / D2 / D3 / D5 / D7）~~ —— 已定案，见 §5。
3. ✅ ~~**P1 Phase C 的 7 项待裁决**~~ —— **已全部裁决并冻结（2026-10-03，Q1–Q8）**，
   见 `docs/architecture/P1-2-P1-4-注册与凭据签发设计.md` 文末 **§Architecture Freeze Decision**
   （配套结论摘要见 §4.1「★ P1 关键冻结事项」）。
   ▶ **当前下一步**：Phase D（P1-3 设备侧 `gfcred`）—— 先出实施计划，**人工审核通过后才编码**。
4. 固件仓库历史中的 **明文 MQTT 凭据**尚未轮换（计划在 P1「Topic V3 + 一机一密」时一并处理）。
5. ✅ ~~未跟踪的 `docs/review/`~~ —— **已移出仓库**（2026-10-04 移至仓库外 `D:\Guo_Feeder_Project\review-materials\`，不再受 git 管理）。
6. ⚠️ **P1-5 的代码与文档改动尚未 commit** —— Cloudflare 仓 4 处（`binding.js` **新增** · `device-registry.js` / `index.js` / `credential.js` **修改**）+ 固件仓 6 份文档（`P1-5-Final-Review.md` 新增 · `cloud_protocol.md` / 设计稿 / `docs/README.md` / `P1-实现清单.md` / `HANDOFF.md` 修改）。**提交粒度待定**（建议按 P1-5-1 / P1-5-2 / P1-5-3 / P1-5-4 分别提交，代码与文档分离）。
7. **P1-5-5 正式验收未做** —— 当前绑定链路的全部结论均来自**离线 mock 自测**；真实 D1（`--local`/`--remote`）与部署版 Worker 上的 T-1…T-19 待跑。

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

---

## 9. 本轮（2026-10-03）完成清单

- ✅ **P1-0 结论冻结**（固件仓 `0255791`）—— `P1-0-EMQX-Capability-Test.md` 新增 **§Design Impact Freeze**：
  ACL 修改**不等于**完整 revoke（已连接 session 不重鉴权）⇒ revoke = `update ACL` + `terminate session`
- ✅ **P1-1 D1 Schema**（`Cloudflare_Assets` `75f84ab`）—— 4 张表 + 11 个索引，**已 apply 到远端 D1**
  （local + remote 均 ✅，`15 commands` 成功；`mqtt_messages` 未受影响；远端实测 CHECK 约束生效）
- ✅ **Cloudflare `main` 已推送**（`1d6fe00..75f84ab`）
- ✅ **Phase C 设计**（固件仓 `01f27bf`）—— `P1-2-P1-4-注册与凭据签发设计.md`：
  **ACL 就绪的唯一硬证据 = 设备 CONNACK 自证**（⇒ 这正是 P1-3 双 slot 存在的理由）
- ✅ **P1-0 A-7 实测**（固件仓 `fece474`）—— create user **幂等且不可变**（`201` / `409 ALREADY_EXISTS`，
  不覆盖密码 · 不改 ACL · 不重复建号；MQTT 建连反证）⇒ **无需 GET+CREATE**、**可 at-least-once retry**
- ✅ **Phase C 架构冻结 Q1–Q8**（固件仓 `10b6a57`）—— 文末 **§Architecture Freeze Decision**；
  **Phase C 文档闭环完成**（含 §4.2 三态→四态、§5.4/§6.3/§6.4/§10.1 冻结标记、§15 改"已全部裁决"）
- ✅ 文档索引同步：`docs/README.md`、`P1-实现清单.md`（含状态名 `NEW` → `CLAIM_PENDING` 修正）
- ✅ **weight 模块暂停**：`weight.enable=false` 已落盘生效（设备 `288485896ce4`，v:13），
  离线刷屏 `STATE_WEIGHT_ERROR` 已消除（根因是**固件旧版不读该字段**，非命令失败）
- ⚠️ 踩坑（已入 `MEMORY.md`）：**SQLite `GLOB '[0-9a-f]*'` 挡不住大写** ⇒ 必须补 `= lower(x)`；
  **table-constraint 必须写在所有 column-def 之后**
- ⚠️ 踩坑：`git push` 走 GCM 会**静默挂起**（沙箱内 120 s 超时）⇒ 用
  `export GH_TOKEN=$(gh auth token)` + 内联 credential helper 推送

**关联提交**：
- 固件仓库：`0255791`（P1-0 冻结）· `01f27bf`（Phase C 设计）· **`fece474`（A-7 实测）** ·
  **`10b6a57`（Phase C 冻结 Q1–Q8）** · `23cd731`（weight.enable）
- `Cloudflare_Assets`：`75f84ab`（P1-1 D1 Schema，**已推送**）

---

## 10. 本轮（2026-10-04）完成清单 —— **P1-5 Device Binding**

### 10.1 状态

> ✅ **P1-5 Device Binding completed** —— 管理端绑定 API 已完成，**INV-4 已满足**。
> **绑定链路的正式验收（P1-5-5）与代码提交尚未进行**。

| 子阶段 | 内容 | 状态 |
|---|---|---|
| **P1-5-A** | 绑定设计（Q14–Q20 裁决） | ✅ 设计冻结 |
| **P1-5-1** | Binding Core —— 新建 `binding.js`（5 导出）+ `device-registry.js` 只增 5 项能力 | ✅ Review Passed |
| **P1-5-2** | Admin API —— `index.js` 接入 5 条路由（bind / unbind / transfer / GET binding / GET user devices） | ✅ 完成 + 全链路回归 |
| **P1-5-3** | Revoke Binding Closure —— `revokeDevice()` 同批关 binding（**INV-4**） | ✅ 完成 |
| **P1-5-4** | Documentation Synchronization | ✅ 完成 |
| **P1-5-5** | 正式验收（真实 D1 + 部署版 Worker） | ⏳ **待做** |

### 10.2 关键结论

- **已完成**：`POST /api/admin/device/:id/{bind,unbind,transfer}` · `GET /api/admin/device/:id/binding` · `GET /api/admin/user/:userId/devices`；`revoke` 增强（`binding_closed`）。
- **INV-4 已满足**：退役（`REVOKED`）与 active binding 互斥 —— 由 `revokeDevice()` **同一 D1 事务**内的"关 binding + 写 `unbind` 事件"保证。
- **owner 唯一来源 = `device_binding`**；`device.state` 仅作摘要，**禁止**由其反推 owner。
- **在线状态仍属于 Phase F（P1-6）**；**APP 用户体系尚未开始**（P1 的 `user_id` 是占位身份）。
- **零固件改动**；**未新增迁移**（复用 `0001`）；`device_credential` 零触碰。

### 10.3 交付物与差异登记

- 代码：`Cloudflare_Assets/guo-feeder-api/src/` —— `binding.js`（新）· `device-registry.js`（+196/−7）· `index.js`（+136/−1）· `credential.js`（+77/−13）。
- 文档：`docs/architecture/P1-5-Final-Review.md`（新 · 审计桥接）· `docs/interfaces/cloud_protocol.md` **§11 Control Plane API** · `P1-5-Device-Binding-Design.md`（加指针）· `docs/README.md` · `P1-实现清单.md` · `Cloudflare_Assets/README.md`。
- **设计与实现差异 11 项（Δ-A…Δ-K）** 全部登记在 `P1-5-Final-Review.md` §2；**该文与代码冲突时以代码为准**。
