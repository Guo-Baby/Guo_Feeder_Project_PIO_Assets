# 用户真机走查需求清单（Backlog）· 2026-10-25

> 来源：用户两轮真机走查反馈（05:47 / 05:54）。用户指示：
> **先全部记录为本轮重点修改项 → 完成阶段 5 → 用户下命令后反查 UI 与计划差异并补齐。**
> 本文是那份"差异反查"的基准清单之一（另一基准 = `14-roadmap-and-acceptance.md`）。

## A. 走查发现的功能缺失（对照原型）

| # | 需求 | 核实/归属 |
|---|---|---|
| A1 | 首页**快捷操作栏可编辑**（原型：可配置数量 0/1/2/4/8/12，安全四要素常驻首屏） | 阶段 3 遗留（S3-04/S3-05）。**未做** |
| A2 | 首页**展示模块可调顺序** + 布局持久化 + 恢复默认 | 阶段 3 遗留（S3-05）。**未做** |
| A3 | 控制页缺**指定步数出粮**（1/8 步、指定脉冲数） | 固件动作已有（STEPPER_ROTATE_STEPS/步进指定脉冲旋转）；S3-09 要求输入校验上下限来自设备 `max_pulses`。**UI 未暴露** |
| A4 | 流程页已逻辑删除（valid=false）的流程**默认不展示**，需要时再展开 | 本轮实现时未过滤。待改 |
| A5 | **设备别名（自定义命名）** | 阶段 3 范围（S3-01 别名+在线时长）。**未做**，用户问是否下阶段 |
| A6 | 震动/出粮**状态查询**（用户以为固件没支持） | **核实：固件 System State 已支持**（`query_state` 走 `system_state_query`，键含 valve_status/stepper_busy/stepper_last_pulses/vibration_active/vibration_last_duration_ms/weight_value/mi_thermo_temp 等 20+ 键）⇒ 纯 UI 暴露即可，零固件改动 |
| A7 | **System State 全量状态查询入口**（用户认为有必要） | 同 A6：固件 query_state 即 System State 查询，设备页可做只读状态面板 |

## B. 性能与体验（用户称"太慢受不了"）

| # | 需求 | 方案 |
|---|---|---|
| B1 | **★ 本地缓存 + 版本对应**：切页不再重发设备查询（流程页查动作/触发器清单、点编辑查详情、模块配置逐个查）——版本一致直接用浏览器缓存 | localStorage 缓存（带 registry_version/workflow variant/config 版本戳）；设备版本变化时自动失效重拉 |
| B2 | **在线状态不走 log 推断**：首选 EMQX（网页无法直连 MQTT）→ **Worker 代理 EMQX Management API 查 clients**（emqx-admin.js 已有同源 API 凭据，P0 实测过 /clients 端点） | 待实现；若做不了留给未来 APP（APP 可直连 MQTT） |
| B3 | **模块配置全量读取**（用户印象固件有"读全部配置"命令，待核实）；逐模块 config_query×7 太慢 | 核实 config_query 是否支持 module=all / config_module 命令语义；配合 B1 缓存 |
| B4 | 日志翻译（解码展示） | 已有 `?decode=1`（WP-A）；用户指的可能是记录页日志 tab 的展示完善，阶段 5 收口 |

## C. 模块配置 UI 化（用户重点，问哪版做）

需求原文要点：
1. **中文界面**：每个配置键有中文名（GPIO Pin=引脚、Active Level=有效电平、Open Duration MS=开启时长、Safety Timeout Second=安全超时、Enable=启用）
2. 用户可修改每个模块的配置键值
3. **输入校验**：数值型→数字框（禁止字母等非法输入）；布尔型→true/false 选择
4. **GPIO pin 允许改但必须警示风险**
5. 关联：config_set/config_save 固件命令已存在（system_router object 分发）；⚠ CFG-ROLLBACK 铁律——改配置后遇非计划重启必须复核；config 修改涉及重启语义需谨慎设计

## D. 用户已认可 / 暂不做

- 阀门超时保护强制关阀 = 预期行为（HX711 缺席），非 bug
- 阶段 4 交付"暂未发现问题，但不能标 100% 通过"（用户未走查全部功能）
- 强制重启回帧丢失（2/2 复现）= 固件已知限制，接受
- 震动/出粮状态查询：用户本以为固件没支持说"暂时不用"；核实后固件已支持（A6），纳入 backlog

## E. 用户指示的执行顺序

1. ✅ 本清单记录
2. ✅ 完成阶段 5（S5-01~S5-08）
3. ⏸ 等用户命令 → **反查 UI 与计划的差异**（对照 `14-roadmap-and-acceptance.md` 验收矩阵 + 本清单）→ 补齐缺失
