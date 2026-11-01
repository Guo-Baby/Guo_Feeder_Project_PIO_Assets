# Guo Feeder 资料索引（SOURCE INDEX）

> **最后更新**：2026-10-30 15:38（GMT+8）
> **路径约定**：`WS` = `D:\Guo_Feeder_Project`；下列路径均相对 `WS`，**全部经过本机存在性核对**。
> **配套**：`HANDOVER.md` · `OPEN_ISSUES.md` · `NEXT_SESSION.md`

---

## 0. 阅读优先级规则（先看这条）

1. **当前实际代码 + 真实运行证据** > 设计文档里尚未实现的描述。
2. **用户实际验收结果** > AI 自动化测试结论。
3. **最新执行报告（10-29）** > `HANDOFF.md`（停在 2026-10-21）。
4. **线上部署版本与本地工作区版本必须分开记录**：线上 Pages `778a6768` / Worker `eea20436`，而工作区 dirty（PIO 26 / CF 55 / EMQX 2）含未部署改动。
5. 报告里的 `PASS` 只代表**当时那一轮**；引用时必须注明轮次，禁止跨轮混用（见 `OPEN_ISSUES.md` GF-TEST-01）。

---

## 1. 交接层（本目录，必读）

| 路径 | 用途 | 必读 | 权威度 | 过时风险 | 何时读 |
|---|---|---|---|---|---|
| `PIO_Assets/Guo_Feeder_Project/docs/handover/HANDOVER.md` | 当前状态快照、架构、基线、阶段 O 结论 | ★★★ 必读 | 高（本文件为准） | 低（每次交接更新） | 新对话第一份 |
| `PIO_Assets/Guo_Feeder_Project/docs/handover/OPEN_ISSUES.md` | 累积问题清单与验收标准 | ★★★ 必读 | 高 | 低 | 决定做什么之前 |
| `PIO_Assets/Guo_Feeder_Project/docs/handover/SOURCE_INDEX.md` | 本索引 | ★★ 必读 | 高 | 低 | 需要深挖时 |
| `PIO_Assets/Guo_Feeder_Project/docs/handover/NEXT_SESSION.md` | 开场任务书（可直接作第一条指令） | ★★★ 必读 | 高 | 低 | 启动新对话时 |

---

## 2. 当前状态与执行报告（高优先）

| 路径 | 用途 | 必读 | 权威度 | 过时风险 | 何时读 |
|---|---|---|---|---|---|
| `PIO_Assets/Guo_Feeder_Project/.workbuddy/memory/2026-10-29.md` | 阶段 M/N/O 全过程日志（最详细的事实源） | ★★★ | 高（一手记录） | 低 | 需要还原阶段 N/O 细节 |
| `PIO_Assets/Guo_Feeder_Project/.workbuddy/memory/2026-10-30.md` | 本轮 ui_responsive_n 修复与回归记录 | ★★ | 高 | 低 | 核对测试数字来源 |
| `docs/reports/2026-10-29-phaseO-telemetry-restore-and-status-query.md` | 阶段 O 主报告（遥测恢复/双设备/状态查询） | ★★★ | 高 | 低 | 核对阶段 O 结论 |
| `docs/reports/2026-10-29-phaseO-wrapup-verification.md` | 阶段 O 收尾（时间线更正、auto-clean 根因、三态分离） | ★★★ | 高 | 低 | 核对 WP-D/H/F/G |
| `docs/reports/2026-10-29-telemetry-stop-forensic-investigation.md` | 遥测停更只读取证（10-27 06:11 起） | ★★ | 高 | 低 | 追溯遥测历史 |
| `docs/reports/2026-10-29-phaseN-report.md` | 阶段 N（BLE 长周期/部署/人工验收） | ★★ | 高 | 中（部分结论已被 O 修正） | 了解 N 阶段 |
| `docs/reports/2026-10-29-phaseM-final-report.md` | 阶段 M 收官 | ★ | 中 | 中（BLE 结论已被 N 纠正） | 回溯 M |
| `docs/reports/2026-10-29-phaseL-execution-report.md` | 阶段 L | ☆ | 中 | 中 | 需要时 |
| `docs/reports/2026-10-29-phaseK-task-ledger.md` | 任务台账（含 V-1~V-8 定义） | ★★ | 高 | 低 | **做 GF-V-01 前必读** |
| `docs/reports/2026-10-29-phaseJ-incremental-sync-and-audit.md` | 增量同步与审计 | ☆ | 中 | 中 | 米家同步相关 |
| `docs/user-guide/2026-10-29-phaseN-manual-acceptance.md` | 人工验收指南 T-01~T-13 | ★★ | 高 | 中 | 做人工验收时 |
| `PIO_Assets/Guo_Feeder_Project/docs/user-guide/User-Guide-and-Config-Write-Risk-2026-10-26.md` | 配置写入风险说明 | ★ | 中 | 中 | 改配置前 |

> ⚠️ `PIO_Assets/Guo_Feeder_Project/docs/reports/` **是空目录**；阶段报告实际在**项目根** `docs/reports/`。不要找错。

---

## 3. 固件架构与接口

| 路径 | 用途 | 必读 | 权威度 | 过时风险 | 何时读 |
|---|---|---|---|---|---|
| `PIO_Assets/Guo_Feeder_Project/AGENTS.md` | 仓库级工作规则 | ★★ | 高 | 低 | 开始改固件前 |
| `PIO_Assets/Guo_Feeder_Project/AI_RULES.md` | AI 行为约束 | ★★ | 高 | 低 | 同上 |
| `PIO_Assets/Guo_Feeder_Project/AI_CONTEXT.md` | 项目上下文 | ★ | 中 | 中 | 快速了解 |
| `PIO_Assets/Guo_Feeder_Project/readme.md` | 固件总说明（较长，59 KB） | ☆ | 中 | 中 | 需要时检索 |
| `PIO_Assets/Guo_Feeder_Project/docs/architecture/Cloud-APP-Platform-Plan.md` | 云-端平台总体方案 | ★★ | 中高（设计意图） | **中高**（部分未实现） | 理解设计意图时 |
| `PIO_Assets/Guo_Feeder_Project/docs/architecture/MQTT-Command-Reference.md` | MQTT 命令/帧格式参考 | ★★★ | 高 | 低 | 改命令/协议前 |
| `PIO_Assets/Guo_Feeder_Project/docs/interfaces/cloud_protocol.md` | 云端协议 | ★★ | 高 | 低 | 协议对齐 |
| `PIO_Assets/Guo_Feeder_Project/docs/interfaces/system_command接口文档.md` | 系统命令接口 | ★★ | 高 | 低 | 改 CommandManager |
| `PIO_Assets/Guo_Feeder_Project/docs/interfaces/config_manager接口文档.md` | 配置管理器接口 | ★★ | 高 | 低 | 改 ConfigManager |
| `PIO_Assets/Guo_Feeder_Project/docs/interfaces/workflow_cloud_interface.md` | 工作流云端接口 | ★★ | 高 | 低 | 改自动化 |
| `PIO_Assets/Guo_Feeder_Project/docs/interfaces/log_envelope_contract.md` | 日志信封契约（`seq` 语义权威） | ★★ | 高 | 低 | 改日志/入库 |
| `PIO_Assets/Guo_Feeder_Project/docs/interfaces/log_params_contract.md` | 日志参数契约 | ★ | 高 | 低 | 同上 |
| `PIO_Assets/Guo_Feeder_Project/docs/interfaces/json_storage接口文档.md` | JSON 存储接口 | ☆ | 中 | 中 | 存储相关 |

**代码即文档（优先级高于上述设计文档）**：

| 路径 | 用途 |
|---|---|
| `PIO_Assets/Guo_Feeder_Project/src/services/config_manager.h/.cpp` | 配置读写/落盘/version |
| `PIO_Assets/Guo_Feeder_Project/src/services/command_manager.h/.cpp` | 命令分发/result 终态 |
| `PIO_Assets/Guo_Feeder_Project/src/services/system_state.h/.cpp` | 全局状态键（含米家诊断） |
| `PIO_Assets/Guo_Feeder_Project/src/services/telemetry_publisher.h/.cpp` | 遥测双重门禁 |
| `PIO_Assets/Guo_Feeder_Project/src/automation/workflow.cpp` | 非阻塞状态机 |
| `PIO_Assets/Guo_Feeder_Project/src/automation/capability_registry.cpp` | 动作注册表（**GF-MI-08 必查**） |
| `PIO_Assets/Guo_Feeder_Project/src/app/xiaomi_lan.cpp` | 米家 LAN 动作/60 s 保护 |
| `PIO_Assets/Guo_Feeder_Project/src/app/xiaomi_lan_parse.h` | miIO 纯函数解析（可宿主测试） |
| `PIO_Assets/Guo_Feeder_Project/src/app/MiThermometer.cpp` | BLE 温湿度 |

---

## 4. 云端：Worker / Pages / D1

| 路径 | 用途 | 必读 | 权威度 | 过时风险 | 何时读 |
|---|---|---|---|---|---|
| `Cloudflare_Assets/guo-feeder-api/src/index.js` | Worker 入口/路由 | ★★★ | 高 | 低 | 改 Worker 前 |
| `Cloudflare_Assets/guo-feeder-api/src/read-api.js` | 只读 API | ★★ | 高 | 低 | 查接口 |
| `Cloudflare_Assets/guo-feeder-api/src/command-api.js` | 命令 API | ★★ | 高 | 低 | 改命令 |
| `Cloudflare_Assets/guo-feeder-api/src/capability-catalog.js` | 动作目录（`cards` 透传） | ★★ | 高 | 低 | 加动作/配置卡 |
| `Cloudflare_Assets/guo-feeder-api/src/config-spec.js` | 配置规格（含 `xiaomi_lan` 两张卡） | ★★ | 高 | 低 | 改配置 UI |
| `Cloudflare_Assets/guo-feeder-api/src/history-ingest.js` | 遥测/日志入库 | ★★ | 高 | 低 | 查落库 |
| `Cloudflare_Assets/guo-feeder-api/src/ingest-validate.js` | 入站校验（当前 `observe`） | ★★ | 高 | 低 | GF-DEP-02 |
| `Cloudflare_Assets/guo-feeder-api/src/xiaomi/` | 米家云同步/登录 | ★★ | 高 | 中 | 米家云相关 |
| `Cloudflare_Assets/guo-feeder-api/migrations/0011_xiaomi_history.sql` | 米家历史表 | ★ | 高 | 低 | 查表结构 |
| `Cloudflare_Assets/guo-feeder-api/migrations/0012_xiaomi_auth_pending.sql` | 米家登录待决表 | ★ | 高 | 低 | 2FA 相关 |
| `Cloudflare_Assets/guo-feeder-api/test/run_all.mjs` | Worker 全量测试入口 | ★★ | 高 | 低 | 回归时 |
| `Cloudflare_Assets/guo-feeder-api/tools/d1q.sh` | D1 只读查询工具 | ★★ | 高 | 低 | 查生产数据 |

| 路径 | 用途 | 必读 | 权威度 | 过时风险 | 何时读 |
|---|---|---|---|---|---|
| `Cloudflare_Assets/guo-feeder-pagesdev/app.html` | UI 骨架（底栏 5 路由） | ★★ | 高 | 低 | 改 UI |
| `Cloudflare_Assets/guo-feeder-pagesdev/assets/js/gf-app.js` | 主应用：路由/命令/首页（约 8 K 行） | ★★★ | 高 | 低 | 改主流程 |
| `Cloudflare_Assets/guo-feeder-pagesdev/assets/js/gf-xiaomi.js` | 米家 UI（MOUNTS/控制/配置） | ★★★ | 高 | 低 | 改米家 UI |
| `Cloudflare_Assets/guo-feeder-pagesdev/assets/js/gf-ui-core.js` | 配置卡/System State | ★★ | 高 | 低 | 改状态面板 |
| `Cloudflare_Assets/guo-feeder-pagesdev/assets/js/gf-mqtt.js` | MQTT WSS 直连 | ★★ | 高 | 低 | 查下行链路 |
| `Cloudflare_Assets/guo-feeder-pagesdev/functions/api/**` | Pages Functions（BFF） | ★★ | 高 | 低 | 改代理/鉴权 |
| `Cloudflare_Assets/guo-feeder-pagesdev/functions/api/read/[[path]].js` | 只读代理（**透传不套壳**） | ★★ | 高 | 低 | 查读接口 |
| `Cloudflare_Assets/guo-feeder-pagesdev/tools/deploy_pages.sh` | **白名单部署**（唯一正确方式） | ★★★ | 高 | 低 | 部署前必读 |
| `Cloudflare_Assets/docs/UI-v1-Phase2-API-Contract.md` | UI API 契约 | ★ | 中 | 中 | 接口对齐 |
| `Cloudflare_Assets/docs/turnstile-integration.md` | Turnstile 集成 | ★ | 中 | 低 | 登录/人机验证 |

---

## 5. EMQX

| 路径 | 用途 | 必读 | 权威度 | 过时风险 | 何时读 |
|---|---|---|---|---|---|
| `EMQX_Assets/README.md` | EMQX 运维说明 | ★★ | 高 | 低 | 动 ACL 前 |
| `EMQX_Assets/config/acl-rules.v3.json` | 现行 ACL 规则 | ★★ | 高（生产配置） | 低 | 查权限 |
| `EMQX_Assets/config/acl-rules.proposed.json` | 候选 ACL | ☆ | 中 | 中 | 提案时 |
| `EMQX_Assets/scripts/apply-web-acl.mjs` | 应用 Web ACL | ★★ | 高 | 低 | 改 ACL |
| `EMQX_Assets/scripts/audit-web-acl.mjs` | 审计 Web ACL（返回 `{ok,status,data}`） | ★★ | 高 | 低 | 审计时 |

---

## 6. UI 测试入口（如何跑）

| 路径 | 用途 | 必读 | 权威度 | 过时风险 | 何时读 |
|---|---|---|---|---|---|
| `Cloudflare_Assets/tests/pages/ui_responsive_n.mjs` | 响应式 + 控制区三动作 + 三态分离（**本轮 142/0**） | ★★★ | 高 | 低 | 改 UI 后必跑 |
| `Cloudflare_Assets/tests/pages/xiaomi_ui.test.mjs` | 米家 UI 单元/结构（**178/0**） | ★★★ | 高 | 低 | 改米家 UI |
| `Cloudflare_Assets/tests/pages/ui_app_runtime.test.mjs` | 主应用运行时（**134/0**） | ★★ | 高 | 低 | 改主流程 |
| `Cloudflare_Assets/tests/pages/ui_mobile_375.mjs` | 手机竖屏真浏览器验收 | ★ | 高 | 中 | 移动端 |
| `Cloudflare_Assets/tests/e2e/post_deploy_smoke.mjs` | 线上 16 项冒烟 | ★★ | 高 | 低 | 部署后必跑 |

**运行方式**（工作目录 `Cloudflare_Assets`）：

```
node tests/pages/ui_responsive_n.mjs
node tests/pages/xiaomi_ui.test.mjs
node tests/pages/ui_app_runtime.test.mjs
node guo-feeder-api/test/run_all.mjs
```

> 测试通过 = DOM/交互层通过，**不等于**真机米家查询/控制通过。

---

## 7. 米家接入（云 + 局域网）

| 路径 | 用途 | 必读 | 权威度 | 过时风险 | 何时读 |
|---|---|---|---|---|---|
| `docs/plans/2026-10-28-xiaomi-integration-plan.md` | 米家接入总体计划 | ★★ | 中高 | 中 | 理解全貌 |
| `docs/testing/xiaomi-device-live-readonly-results.md` | 真机只读实测结果 | ★★ | 高 | 中 | 核对真机行为 |
| `docs/testing/xiaomi-device-python-acceptance-test.md` | Python 验收脚本说明 | ★ | 中 | 中 | 复现 |
| `docs/testing/pi2001-miot-spec-verification.md` | miIO 规格核对 | ★ | 中 | 中 | 协议细节 |
| `PIO_Assets/Guo_Feeder_Project/src/app/xiaomi_lan_parse.h` | LAN 解析纯函数 | ★★★ | 高 | 低 | 改解析 |
| `PIO_Assets/Guo_Feeder_Project/test/xiaomi_lan_parse_host/` | 宿主解析测试（33/0） | ★★ | 高 | 低 | 改解析后 |

---

## 8. 构建、归档与证据

| 路径 | 用途 | 必读 | 权威度 | 过时风险 | 何时读 |
|---|---|---|---|---|---|
| `PIO_Assets/Guo_Feeder_Project/tools/build_telemetry.sh` | 遥测构建（含产物断言） | ★★★ | 高 | 低 | 构建遥测固件 |
| `PIO_Assets/Guo_Feeder_Project/tools/archive_build_artifacts.sh` | 构建产物归档（防 auto-clean 丢失） | ★★★ | 高 | 低 | 每次构建后 |
| `PIO_Assets/Guo_Feeder_Project/tools/gf_telemetry_enable.py` | 运行时开关 set/save/验证 | ★★ | 高 | 低 | 开遥测 |
| `PIO_Assets/Guo_Feeder_Project/tools/gf_cmd.py` | 串口命令工具（已修残行/非终态） | ★★ | 高 | 低 | 串口调试 |
| `PIO_Assets/Guo_Feeder_Project/tools/gf_ble_watch.py` | 长周期 BLE 监控 | ★ | 高 | 低 | BLE 取证 |
| `PIO_Assets/Guo_Feeder_Project/docs/evidence/Telemetry-Flash-2026-10-29/` | 遥测固件证据（bin + build-record + pre-flash-config） | ★★★ | 高（物证） | 无 | 核对固件 |
| `PIO_Assets/Guo_Feeder_Project/docs/evidence/build-archive/` | 归档产物 + `INDEX.tsv` | ★★ | 高 | 无 | 找回历史构建 |

---

## 9. 历史文档（仅沿革，禁止当现状）

| 路径 | 说明 | 过时风险 |
|---|---|---|
| `PIO_Assets/Guo_Feeder_Project/HANDOFF.md` | 旧交接，**最后更新 2026-10-21 04:06**，不含阶段 N/O | **高**：不得作为最新状态依据 |
| `PIO_Assets/Guo_Feeder_Project/docs/architecture/G-4-Gate-Revision-Record-2026-10-20.md` | DRAM 阈值权威（85%） | 中（门禁历史） |
| `PIO_Assets/Guo_Feeder_Project/docs/architecture/G4-S1-S6-Execution-Report-2026-10-21.md` | S-1~S-6 | 中 |
| `PIO_Assets/Guo_Feeder_Project/docs/issues/未修复的问题.md` | 旧问题册（可能早于本清单） | **高**：以 `OPEN_ISSUES.md` 为准 |
| `PIO_Assets/Guo_Feeder_Project/docs/architecture/UI-Plan-v1-2026-10-23.md` | UI 计划 | 中 |

---

## 10. 明确不存在 / 易找错的路径（避免浪费时间）

| 路径 | 事实 |
|---|---|
| `PIO_Assets/Guo_Feeder_Project/HANDOVER.md` | **不存在**（在 `docs/handover/`） |
| `PIO_Assets/Guo_Feeder_Project/OPEN_ISSUES.md` | **不存在**（在 `docs/handover/`） |
| `PIO_Assets/Guo_Feeder_Project/SOURCE_INDEX.md` | **不存在**（在 `docs/handover/`） |
| `PIO_Assets/Guo_Feeder_Project/NEXT_SESSION.md` | **不存在**（在 `docs/handover/`） |
| `PIO_Assets/Guo_Feeder_Project/docs/reports/` | **空目录**；报告在项目根 `docs/reports/` |
| `docs/architecture/Cloud-APP-Platform-Plan.md`（根） | **不存在**；在 `PIO_Assets/Guo_Feeder_Project/docs/architecture/` |

---

## 11. 建议阅读顺序（按角色）

- **只想接手继续干活**：本目录 4 份 → `2026-10-29.md` → 对应源码 → 跑一次 `ui_responsive_n` 确认基线。
- **要改米家**：`HANDOVER` §4.4 → `xiaomi_lan.cpp` / `gf-xiaomi.js` → `docs/testing/xiaomi-device-live-readonly-results.md`。
- **要改云端**：`HANDOVER` §4 → `guo-feeder-api/src/index.js` → `run_all.mjs`。
- **要部署**：`HANDOVER` §7/§8 → `tools/deploy_pages.sh` → `post_deploy_smoke.mjs`。
