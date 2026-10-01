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

【按需查阅】
8. D:\Guo_Feeder_Project\PIO_Assets\Guo_Feeder_Project\docs\interfaces\cloud_protocol.md      ← MQTT 协议唯一权威
9. D:\Guo_Feeder_Project\PIO_Assets\Guo_Feeder_Project\docs\interfaces\workflow_cloud_interface.md
10. D:\Guo_Feeder_Project\PIO_Assets\Guo_Feeder_Project\AI_CONTEXT.md / AI_RULES.md

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
| Mijia BLE 温湿度计 | ⏳ 解码算法已分析，待正式集成 |
| Cloud Protocol CBOR | ⏳ 已验证，待正式整合进协议 |
| **编译状态** | ✅ 通过；RAM 39.9%（130,736 B）/ Flash 65.4% |

> ⚠️ **本轮（10-02）实测：固件仓库本地领先远程 `wb` 分支 52 个提交**（含 10-01 目录重构）。**尚未推送**。

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
| ACL | ✅ **已修复**：5 用户 / 9 条规则 + `全部用户 → # → deny` 兜底 ⇒ **白名单语义成立** |
| 脚本 | `scripts/{emqx-api,check-connection,diagnose-api,apply-acl}.mjs` |

**ACL 现状**：

| 账号 | 允许 |
|---|---|
| `GuoFeederDevice` · `GuoFeederDevice001` | sub `guo_feeder/down` · pub `guo_feeder/up` · pub `guo_feeder/log` |
| `workbuddy` · `test001` · `shouji` | all `guo_feeder/#`（调试用，未来收窄） |

> ⚠️ EMQX Serverless **不支持外部 HTTP 认证 / 扩展授权 / 白名单开关** ⇒ P1 的凭据模型只能是
> **「预置账号 + Worker 用 EMQX API 改写该账号 ACL」的 slot 池**。详见 `MEMORY-cloud.md`。

---

## 5. 下一步：P0–P6

> 完整定义与验收标准见 **`docs/architecture/Cloud-APP-Platform-Plan.md`**。

| 阶段 | 内容 |
|---|---|
| **P0** | 设备身份（MAC 派生 `device_id` + NVS 持久化 + 禁漂移）· **Topic V3** `guo_feeder/<device_id>/...` · ACL 隔离设计 |
| **P1** | EMQX 认证 + 用户/设备绑定 + 凭据签发（slot 池） |
| **P2** | **Homie Bridge**（与 CloudManager **并列**的投影适配层，写入单入口经 CommandManager） |
| **P3** | 历史/D1（多设备数据模型 + 遥测降频策略） |
| **P4** | Web UI（MQTT-Tiles + Homie Discovery Adapter） |
| **P5** | Cloudflare Pages 部署 + 安卓 WebView 壳 |
| **P6** | 原生 Android（**【后续可选】**，现在不装 Android Studio） |

### ★ P0 开工前尚缺的设计确认（8 项）

| # | 待确认 | 影响 |
|---|---|---|
| **D1** | `device_id` 格式（`aabbccddeeff` 还是 `gf-...`） | 决定 Topic/ACL/D1 全部字符串，改了全线返工 |
| **D2** | 存储位置（NVS 独立分区 / Config 只读字段） | 决定实现路径 |
| 3 | `client_id` 是否同步改为 `dev_<device_id>` | 现为 `guo_feeder_001`，与 MAC 无关（身份漂移残留） |
| **D3** | Topic 实现方式（模板渲染 / 代码拼接） | 决定 `config_manager` 是否新增 `topic_render()` |
| **D5** | EMQX API 能否按 `username` 改写 ACL（**需实测**） | **P1 根本前提** |
| 6 | APP 凭据槽位数 N 与租约模型 | 决定 D1 `Credential` 表结构 |
| **D7** | 遥测落库频率策略 | 设备侧降频需 P0 一起设计配置项 |
| 8 | legacy Topic 过渡期长度 | 决定双发实现与 ACL 过渡规则 |

> **建议先定 D1 / D2 / D3 / D5 / D7 五项，即可开工 P0。**

---

## 6. 环境要点（省得重新摸索）

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

1. **固件仓库推送到哪个分支？** —— 本地 `wb` 领先远程 52 提交；远程默认分支是 `main`。
   本轮的处理见 §8「本轮完成清单」。
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

**关联提交**：
- `EMQX_Assets`：`3415211`（API 边界实测 + 修正探针 + ACL 套用脚本）
- `Cloudflare_Assets`：`1d6fe00`（凭据同步脚本）、`4751bc9`（建立 git 追踪）
- 固件仓库：`d9c8cdd`（文档归位）· `ca89176`（gitignore 治理）· `f5b5414`（src 分层）+ 本轮 readme/HANDOFF 提交
