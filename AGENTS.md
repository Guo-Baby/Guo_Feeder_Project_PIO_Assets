# AGENTS.md —— Guo Feeder（ESP32-S3 自动猫粮粉分装机固件）

> 面向 AI 编程助手的工程速览。完整架构与编码规范以仓库内 `AI_CONTEXT.md`、`AI_RULES.md` 为准（它们是为 AI 编写的权威文档，改动前务必先读）。

## 项目一句话

ESP32-S3（Arduino 框架 / PlatformIO）固件：自动称重、定量分配猫粮粉、控制阀门与步进电机、执行用户工作流、通过 MQTT 连云、OLED 显示状态。

## 关键事实（非显而易见，先读再改）

- 板型：`esp32-s3-devkitc-1`；Flash 16MB（QIO），OPI PSRAM 8MB （已在 `platformio.ini` 开全套 PSRAM 宏）。
- 平台 `espressif32`，`framework = arduino`，文件系统 `littlefs`。
- 第三方库（`lib_deps`）：`U8g2`(OLED)、`ArduinoJson`(JSON，已开 `ARDUINOJSON_USE_CBOR=1`)、`HX711`(称重)、`NimBLE-Arduino`(BLE/MQTT)。
- 构建：`pio run`；上传 `pio run -t upload`；串口监视 `pio device monitor`（115200）；生成/刷新编译数据库 `pio run -t compiledb`。
- **clangd 已配置**：仓库有 `.clangd`（指向编译数据库并过滤 ESP32 的 GCC 专有标志）。⚠️ `compile_commands.json`（约 5 MB）**自 2026-10-01 起已 gitignore、不再入库**（构建产物，可再生成）——**全新 clone 后先跑一次 `pio run -t compiledb`**，clangd 才有索引数据。clangd 二进制已装于 `C:\Program Files\LLVM\bin\clangd.exe`，需将其加入 PATH 并让 VS Code clangd 扩展指向该路径（见下「clangd 环境」）。编译数据库已通过 `fix_compiledb.py` 设置 `COMPILATIONDB_INCLUDE_TOOLCHAIN=True`，工具链 include 路径已内嵌，clangd 可直接解析 `<Arduino.h>`、`<vector>` 等头文件，**无需**把 Xtensa 工具链加入 PATH。

## clangd 环境（已安装 LLVM，还需下面两步）

1. **把 LLVM 的 bin 加入系统 PATH**（clangd 默认没进 PATH）：  
   - 图形界面：设置 → 系统 → 关于 → 高级系统设置 → 环境变量 → 编辑「用户变量」的 `Path` → 新增 `C:\Program Files\LLVM\bin` → 确定。  
   - 或管理员 PowerShell：`[Environment]::SetEnvironmentVariable("Path", $env:Path + ";C:\Program Files\LLVM\bin", "User")`  
   改完**重启终端 / VS Code**，验证：`clangd --version`。
2. **VS Code 安装 `clangd` 扩展**（ID：`llvm-vs-code-extensions.vscode-clangd`），并在设置里将 `clangd.path` 设为 `C:\Program Files\LLVM\bin\clangd.exe`（避免扩展重复下载 clangd）。
3. **避免两套引擎冲突**：若装了 Microsoft 的 C/C++ 扩展（`ms-vscode.cpptools`），建议把 `C_Cpp.intelliSenseEngine` 设为 `disabled`（或在 clangd 弹窗里选「Disable IntelliSense」），让 clangd 独占 C/C++ 语言服务。
4. 打开工程根 `Guo_Feeder_Project`，clangd 会自动读取 `.clangd` 与 `compile_commands.json` 开始索引（首次约几十秒）。**无需**把 Xtensa 工具链加入 PATH —— include 路径已内嵌在编译数据库，clangd 不依赖交叉编译器二进制。
5. **PlatformIO Core 目录在 `D:\platformIO`**（非默认 `~/.platformio`），故 Xtensa 工具链位于 `D:\platformIO\packages\toolchain-xtensa-esp32s3\bin`（你的板子是 ESP32-S3）。clangd 通常无需它；若仍有零星系统头文件报错，可在 `.clangd` 的 `CompileFlags.Add` 补一行：`--query-driver=D:/platformIO/packages/toolchain-xtensa-esp32s3/bin/xtensa-esp32s3-elf-g++.exe`。

## 源码地图（`src/`）

> **2026-10-01 起 `src/` 已按系统层级分层为 7 个子目录**。模块间 `#include` 一律写成
> **相对 `src/` 的显式路径**（如 `#include "services/config_manager.h"`）。
> PlatformIO 默认已含 `-Isrc`，**无需**在 `platformio.ini` 里为子目录加 `-I`。

- `main.cpp` —— 入口：模块初始化与主循环调度。
- `app/` —— **应用 / 能力层**：`valve`(电磁阀)、`weight`(HX711 称重)、`dispense_guard`(定量供水安全保护)、`MiThermometer`(米家蓝牙温湿度)、`oled`/`oled_animation`(显示)、`computer_reset`。
- `automation/` —— **自动化 / 中间层**：`workflow`(工作流引擎 / 动作调度)、`workflow_storage`(BIN 持久化)、`capability_registry`(能力登记 + Stable ID)。
- `services/` —— **服务层**：`system_state`(全局状态中心)、`config_manager`、`event_manager`、`time_manager`(SNTP+RTC)、`wifi_module`、`command_manager`(命令路由)、`system_command`。
- `storage/` —— **存储基础设施层**：`file_storage`(LittleFS 原子写抽象)、`json_storage`、`bin_storage`。
- `log/` —— **日志子系统**：`log_manager`、`log_events`(事件 ID 表)、`log_cbor`、`log_ack`。
- `cloud/` —— **云通信层**：`cloud_manager`(MQTT / 云协议 / JSON+CBOR / ACK)。
- `test/` —— 测试桩：`test_mqtt`。

## 架构与编码铁律（摘自 `AI_RULES.md`，改动前必读）

- 严格分层，命令流方向固定：`CloudManager → CommandManager → WorkflowManager → Action → Hardware Driver`；**禁止跨层调用**（如 CloudManager 不得直达 Action）。
- 主循环**绝不阻塞**：禁用 `delay()`、禁用同步等待硬件完成；一律用状态机 / 队列 / 回调（非阻塞模型）。
- 改动遵循「先理解架构 → 定位受影响模块 → 最小改动 → 保留接口」，不要另造工作流引擎/队列/动作系统。
- 内部模块不直接处理 JSON；JSON 仅在 `CloudManager` 与云之间转换，内部用 C++ 结构。

## 文档地图（完整索引见 `docs/README.md`）

**根目录（工程与 AI 约定）**

- `readme.md` —— 项目总说明（架构、目录结构、核心模块、协议概述、开发规范）。
- `AI_CONTEXT.md` —— 系统架构、数据流、配置系统、执行模型。
- `AI_RULES.md` —— AI 编码规则（架构边界、通信、非阻塞、错误处理）。⚠️ 已 gitignore（含凭据），本地保留。
- `AGENTS.md` —— 本文件。

**`docs/` 分类目录**

- `docs/requirements/需求文档.md` —— 产品功能需求。
- `docs/interfaces/` —— ★ **接口 / 协议契约**（接入前必读）：`cloud_protocol.md`(V2.0)、`workflow_cloud_interface.md`、`config_manager接口文档.md`、`json_storage接口文档.md`、`system_command接口文档.md`。
- `docs/architecture/` —— 架构设计：`Cloud-APP-Platform-Plan.md`（云端/APP 正式规划 v2）、`jsonstorage开发架构.md`。
- `docs/modules/` —— 各模块设计说明：`wifi开发笔记.md`、`bin_storage开发说明0910.md`。
- `docs/specs/` —— 规范 / 模板 / 集成指南：`critical_operation接入规范.md`、`新增动作模板.md`、`LogManager-Integration-Guide.md`、`P2_Log_Integration_Matrix.md`。
- `docs/issues/未修复的问题.md` —— ★ **全项目问题权威清单**（`LOG-n`/`VALVE-n`/`R-n`/…）。
- `docs/archive/` —— **历史归档**（`log/`、`workflow/`、`legacy/`），含各阶段报告与进度台账，**不代表当前实现**。

## 约定

- 设备配置以 JSON（`config.json` / `workflow.json`）存储，仅 `ConfigManager` 可写配置。
- 提交前建议跑 `pio run` 确保编译通过；长耗时操作需在状态机里保存运行时状态，不阻塞主循环。
