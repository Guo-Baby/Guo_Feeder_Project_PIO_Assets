# docs/ 文档索引

> 本目录存放 Guo Feeder 项目的**说明文档 / 接口文档 / 规范 / 归档**。
> 源码在 `../src/`，工程总览见 `../readme.md`。
>
> ★ **新会话请先读 `../HANDOFF.md`** —— 项目当前状态 / 必读文档清单 / 下一步计划。
>
> 整理日期：**2026-10-01**（根目录文档分类归位 + 历史归档）；
> **2026-10-02** 复核并修正过时内容、移出重复章节。

---

## 目录导航

| 目录 | 放什么 | 是否现行 |
|---|---|---|
| [`requirements/`](#requirements-需求) | 产品级需求 | ✅ 现行 |
| [`architecture/`](#architecture-架构) | 架构设计 | ✅ 现行 |
| [`interfaces/`](#interfaces-接口--协议) | **接口 / 协议契约**（最重要） | ✅ 现行 |
| [`modules/`](#modules-模块设计说明) | 各模块设计说明 | ✅ 现行 |
| [`specs/`](#specs-规范--模板--集成指南) | 规范 / 模板 / 集成指南 | ✅ 现行 |
| [`issues/`](#issues-问题清单) | 问题跟踪清单 | ✅ 现行 |
| [`scripts/`](#scripts-脚本) | 部署 / 工具脚本 | ✅ 现行 |
| [`archive/`](#archive-历史归档) | 历史进度、阶段报告、旧需求 | 📦 仅存档 |

---

## requirements/ 需求

| 文档 | 说明 |
|---|---|
| `需求文档.md` | **自动猫咪饮水系统软件需求设计文档 V2.1** —— 产品级需求（水箱 / 重力供水 / 电磁阀 / HX711 按重量控水） |

## architecture/ 架构

| 文档 | 说明 |
|---|---|
| `docs/architecture/Cloud-APP-Platform-Plan.md` | **云端 + 安卓端正式架构规划 v2** —— EMQX Serverless + Cloudflare Worker/Pages/D1 + Homie 5 + MQTT-Tiles；含 P0–P6 实施路线、容量核算、设备身份与 ACL 模型。**P0–P6 的实施依据** |
| `P0-设备身份与Topic隔离设计.md` | **★ P0 实施级设计（FROZEN · v1.2）** —— D1/D2/D3/D5/D7 定案 · `device_id` **三层身份来源**与生命周期（5 场景）· `dver` 迁移硬约束 · 身份边界 · Topic V3 + `topic_render()` · **legacy 回退的生产环境边界** · EMQX ACL 模型 · APP `client_id` 约束 · DEF-2 修复 · 上线顺序与回滚 · 验收 DoD |
| `P0-实现清单.md` | **★ P0 施工工单** —— 冻结基线（含 **P0 不包含**范围清单）· **P0-1…P0-5** 逐项（目标 / 修改文件 / 修改函数 / 修改内容 / 禁止 / 风险 / 验证方法）· 串口用例 S-1…S-10 · MQTT 用例 M-1…M-3 · 静态验证 A-1…A-4 |
| `P0-Final-Review.md` | **★ P0 验收快照（Final Baseline）** —— 冻结基线（Identity / MQTT / ACL / 构建环境）· 已知限制 **L-1…L-7（有意保留，非缺陷）** · P0-1…P0-5 提交与资源增量 · 上板验收（S-1…S-11 / M-1…M-3）· **排障铁律**（`subscribed` ≠ 授权等）· 遗留事项 |
| `P1-Cloud-Device-Lifecycle-Plan.md` | **★ P1 架构规划（已冻结 · 2026-10-02 审核通过）** —— 从「设备身份隔离」升级到「**设备生命周期管理**」：注册状态机（**FACTORY / CLAIM_PENDING / REGISTERED / BOUND / UNBOUND / REVOKED**）· **一机一凭据**（create/rotate/revoke）· **ACL 动态授权时序** · APP 绑定模型 · D1 表结构草案 · 在线状态同步 · **P1 范围冻结** + 前置实测项 + **P1 Architecture Freeze Decision（9 条）** |
| `P1-实现清单.md` | **★ P1 施工工单（仅规划，不编码）** —— **P1-0 前置实测（A-1/A-4/A-5/A-6，阻塞项）** · P1-1…P1-8 逐项（目标 / 预期修改 / 依赖 / 禁止 / 风险 / 验证方法）· 云端 / 设备侧 / 越权用例表 · 验收 DoD · 实施顺序 · 审核清单 |
| `P1-0-EMQX-Capability-Test.md` | **★ P1-0 平台能力实测（已完成）** —— A-4/A-4b（`client_id` 上限 15323）· A-5（账号可程序化增删，上限≈2000）· A-1/A-1c（**ACL 生效 ≈1.3 s，且只作用于新连接**）· A-6（无 `$SYS`，有 `$events/*`）· **A-7（create user 幂等：`201` / `409 ALREADY_EXISTS`、不覆盖密码、可 at-least-once retry）** + **§Design Impact Freeze**（revoke = ACL change + session termination）与 **§A-7 Design Impact** |
| `P1-2-P1-4-注册与凭据签发设计.md` | **★ Phase C 设计（已冻结 · 2026-10-03）** —— P1-2+P1-4 **合并闭环**：发现建档 → 准入 → 建账号 → 写 ACL → **就绪判定（设备 CONNACK 自证）** → 落库 → 下发 → 升级 ACTIVE；含状态/四态映射、失败矩阵、对账、API 契约草案、给 Phase D 的接口冻结，**文末 §Architecture Freeze Decision（Q1–Q8）为不可擅改基线** |
| `jsonstorage开发架构.md` | JsonStorage 存储层架构设计 |

## interfaces/ 接口 / 协议

> **接入新功能前必读。** 这是对外/对云契约的唯一权威来源。

| 文档 | 说明 |
|---|---|
| `cloud_protocol.md` | **Cloud Protocol V2.0** —— MQTT 连接参数、报文外壳、字段缩写、命令翻译、ACK、分片、错误码、Log Topic 预留 |
| `workflow_cloud_interface.md` | Workflow 云端接口契约（Workflow / Variant / 云端同步命令） |
| `config_manager接口文档.md` | ConfigManager 对外接口（模块配置读写、版本、原子写） |
| `json_storage接口文档.md` | JsonStorage 对外接口 |
| `system_command接口文档.md` | SystemCommand 对外接口（restart 等系统级命令） |

## modules/ 模块设计说明

| 文档 | 对应源码 |
|---|---|
| `bin_storage开发说明0910.md` | `src/storage/bin_storage.*` / `file_storage.*` |
| `wifi开发笔记.md` | `src/services/wifi_module.*`（开发笔记与踩坑） |

> ⚠️ **2026-10-01 更正确认**：仓库根目录原有 4 个 `.d` 文件
> （`cloud_manager.d` / `command_manager.d` / `valve.d` / `workflow.d`）
> **不是设计文档**，而是 GCC 的**依赖文件**（首行形如 `cloud_manager.o: src\cloud_manager.cpp \`，
> 生成于 2026-08-03，还引用着分层前的旧路径）⇒ 已删除，并在 `.gitignore` 加入 `*.d`
> 防止再次误入库。**PlatformIO 的依赖文件正常位于 `.pio/build/<env>/`（已被忽略）。**

## specs/ 规范 / 模板 / 集成指南

| 文档 | 说明 |
|---|---|
| `critical_operation接入规范.md` | **Critical Operation 接入规范** —— 关键操作（Flash 写等）的统一接入要求 |
| `新增动作模板.md` | **新增 Workflow Action 的标准模板与编码规范** |
| `LogManager-Integration-Guide.md` | LogManager 集成指南 |
| `P2_Log_Integration_Matrix.md` | **LogManager P2 埋点覆盖权威矩阵**（§15 为最终覆盖状态） |

## issues/ 问题清单

| 文档 | 说明 |
|---|---|
| `未修复的问题.md` | **全项目问题权威清单** —— `LOG-n` / `VALVE-n` / `R-n` / `T-n` / `EVT-n` / `LV-n` / `P0-n` / `DSP-n` / `PROTO-n` / `OLED-n` |

## scripts/ 脚本

| 文件 | 说明 |
|---|---|
| `setup-cloud-app-env.ps1` | 云端 / 安卓端本地工具链部署脚本（**默认 dry-run**，加 `-Apply` 执行）。⚠️ 必须保持纯 ASCII（PowerShell 5.1 按 ANSI 解码无 BOM 的 `.ps1`，中文注释会直接 `ParserError`） |

---

## archive/ 历史归档

> **仅作史料留存，不代表当前实现。** 排查问题时如果发现文档与代码不一致，**以代码为准**。

| 子目录 | 内容 | 数量 |
|---|---|---|
| `archive/log/` | LogManager 开发全过程：审计报告、详细设计规划、P1.1–P1.5 / P2F–P2N 各阶段报告与审查、Phase4/Phase5 边界审查、R7/R8 性能分析、板级测试报告 | 37 |
| `archive/workflow/` | Workflow 改造历程：需求文档、架构适配 CommandManager 报告、storage 改造进度、cloud_sync 进度与测试报告、critical_operation 接入与自查 | 17 |
| `archive/legacy/` | 其他一次性文档：`AI_TASK.md`（TimeManager V2 需求，已完成）、`computer_reset接入报告.md`、`config_version与workflow初始bin-进度.md`、`bin_storage开发需求.md`、`整理方案-文档与源码目录重构.md`（2026-10-01 整理方案）、`readme-旧版云端协议速查.md`（2026-10-02 自 readme 移出的重复章节） | 6 |

> **★ 历史路径映射（2026-10-02 补）**：归位时**未改写归档文档内部**的相互引用（史料保持原样），
> 因此在 `archive/` 里读到旧路径属正常，按下表换算：

| 文档内写的旧路径 | 现在的真实位置 |
|---|---|
| `log模块历史/<file>.md` | `docs/archive/log/<file>.md` |
| `workflow修改历史需求/<file>.md` | `docs/archive/workflow/<file>.md` |
| `docs/LogManager-P2-Progress.md` | `docs/archive/log/LogManager-P2-Progress.md` |
| `docs/LogManager-Integration-Guide.md` | `docs/specs/LogManager-Integration-Guide.md` |
| `docs/P2_Log_Integration_Matrix.md` | `docs/specs/P2_Log_Integration_Matrix.md` |

> **现行文档**（`requirements/` `architecture/` `interfaces/` `modules/` `specs/` `issues/`）中的同类引用
> 已于 **2026-10-02 全部更正为真实路径**（42 处）；只有 `archive/` 内部保留旧写法。

---

## ⚠️ 已知待办：文档中的明文凭据

以下**仍被 git 跟踪**的文档里含真实 MQTT 凭据（broker 域名 / 用户名 / 密码），已列入待办：

| 文件 | 处理建议 |
|---|---|
| `interfaces/cloud_protocol.md` | 替换为占位符 |
| `archive/log/LogManager-P1.5-Board-Test-Report0918.md` | 替换为占位符 |

> `data/config/mqtt.json` 与 `AI_RULES.md` 已通过 `.gitignore` 移出版本控制。
> **注意**：凭据已在 git **历史**中，`gitignore` 无法清除历史 —— 彻底修复需**轮换 MQTT 密码**，
> 或重写 git 历史。计划在 P0/P1「Topic V3 + 一机一密」改造时一并轮换
> （见 `architecture/Cloud-APP-Platform-Plan.md`）。

---

## 维护约定

1. **新增模块说明 / 接口文档** → 放入对应分类目录，并回来更新本索引。
2. **阶段报告 / 进度台账 / 一次性需求** → 完成后移入 `archive/<模块>/`，**不要留在根目录或分类目录**。
3. **接口 / 协议变更** → 必须同步更新 `interfaces/` 下的权威文档。
4. 根目录只保留：构建配置、AI 工程文档（`readme.md` / `AGENTS.md` / `AI_CONTEXT.md` / `AI_RULES.md`）
   与**会话交接文档 `HANDOFF.md`**。
5. **`readme.md` 只做「总说明」**：协议细节、示例、错误码表一律放 `interfaces/`，
   避免同一内容两处维护后产生分歧（2026-10-02 已据此移出 1251 行重复内容）。
