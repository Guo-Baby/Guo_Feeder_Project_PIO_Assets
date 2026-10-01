# docs/ 文档索引

> 本目录存放 Guo Feeder 项目的**说明文档 / 接口文档 / 规范 / 归档**。
> 源码在 `../src/`，工程总览见 `../readme.md`。
> 整理日期：**2026-10-01**（根目录文档分类归位 + 历史归档）。

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
| `Cloud-APP-Platform-Plan.md` | **云端 + 安卓端正式架构规划 v2** —— EMQX Serverless + Cloudflare Worker/Pages/D1 + Homie 5 + MQTT-Tiles；含 P0–P6 实施路线、容量核算、设备身份与 ACL 模型。**P0–P6 的实施依据** |
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
| `archive/legacy/` | 其他一次性文档：`AI_TASK.md`（TimeManager V2 需求，已完成）、`computer_reset接入报告.md`、`config_version与workflow初始bin-进度.md`、`bin_storage开发需求.md`、`整理方案-文档与源码目录重构.md`（本次整理方案） | 5 |

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
4. 根目录只保留：构建配置、AI 工程文档（`readme.md` / `AGENTS.md` / `AI_CONTEXT.md` / `AI_RULES.md`）。
