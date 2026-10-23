# UI v1 · 阶段 4 尾项完成报告：重启与恢复 + 急停物理核实

> 日期：2026-10-25 · 前序：`UI-v1-Stage4-WorkflowEditor-and-SelfReview-2026-10-24.md`
> 本轮把路线图阶段 4 的最后三项（强制停止核实、普通重启、强制重启）收口。

---

## 1. 固件前置盘点（只读核查，未改固件）

对照 `15-firmware-change-list.md` 逐项核实现状：

| # | 改动 | 状态 |
|---|---|---|
| F-1/F-3 | safety 模块 + safety_router(stop_all/clear) | ✅ 已实现（`src/app/safety.cpp`） |
| F-2 | 启动安全电平 `safety_outputs_init()` | ✅ 已挂（`main.cpp:554`） |
| F-4 | 强制重启 `system.force_restart` | ✅ 已实现（stop_all→落 boot flag→300ms 复位） |
| F-5 | Flash 写保护闸门 | ❌ 未实现 —— **风险已被固件注释显式接受**（"最后手段"定位，boot flag+Backup 兜底） |
| F-8/F-9 | 阀超时 60s / 普通重启等 Critical | ✅ 已实现 |
| 命令名 | `c=system, p.o=restart / force_restart` | ✅ 与 Worker 目录一致 |

**结论：阶段 4 尾项是纯 UI 接线** —— Worker 目录与帧构造早已支持两条重启命令，本轮零 Worker 改动。

## 2. 本轮实现（云仓 `97fe27a` · Pages `dcd274cc`）

### 2.1 设备页「重启与恢复」危险区
- **普通重启**：confirm → `system.restart(precheck=true)`；文案明示"等关键操作归零、约 10s 窗口"
- **强制重启**：prompt 输入 **RESTART**（双确认闸）→ `system.force_restart(reason=user_forced)`；文案明示 300ms 复位 / 打断 Flash 写入 / Backup+启动标记兜底
- **回归监视 `watchRestart`**：不依赖命令终态（回帧后设备随即消失）——独立轮询 snapshot.last_seen，观察**断裂→重新上线** ⇒ "设备已回归"，并引导核对设备页配置与告警页（0x0508）；150s 超时如实报人工确认
- 强制重启回帧明细如实展示：`boot_flag_written` / `critical_bypassed` / `safety_all_ok`（后者为 false 时红色警示）

### 2.2 急停物理核实（技术契约 §3.3）
`safety.stop_all` 成功后自动独立 `query_state(o=valve_status)` 复核：
- 阀已关 ⇒ "物理核实：阀已关闭 ✓（步进/振动无只读状态，以急停回执为准）"
- 未核实 ⇒ "⚠ 阀位未核实成功 —— 请人工确认阀门状态"
- 落实"不得仅凭命令 success 宣称已停"的验收要求

### 2.3 Worker 无改动说明
`system.restart` / `system.force_restart` 此前已在 `SAFETY_COMMANDS` 目录（risk DANGER/CRITICAL）且 `buildDeviceFrame` 有专用分支；`family:"system"` 在 UI `runCommand` 允许列表内（P0 修复时已归一）。本轮只补了 system 族的取回帧条件（重启回帧明细在 mqtt_messages）。

## 3. 测试

- 全量回归 **739 PASS / 0 FAIL**（ui_security 273 / pages_auth 104 / pages_webhook 22 / log_decode 29 / history_frames 33 / wfadmin 65 / ui_app 143 / **ui_app_runtime 70**）
- 新增：T-13 六项（重启按钮存在/风险文案/system.restart 载荷/**回归监视序列**（基线→断裂→回归）/RESTART 确认/reason 参数）+ S-13 七项结构断言 + 急停物理核实断言
- 测试桩升级：`insertAdjacentHTML`、元素级 `querySelector`、真实短周期 `setInterval`（回归监视需要多次轮询观察状态迁移）

## 4. 真机验证（`verify_restart_prod.py` v2，7 PASS + 1 INFO）

**判据修正（重要口径）**：空闲设备只发 `/log`（不刷 last_seen）⇒ last_seen 停更是正常口径、不能当离线判据。改用 **log_history 持续增长**（在线）与 **boot_seq 递增**（重启完成，信封自带）。

| 项 | 结果 |
|---|---|
| R-1 在线（log 增长） | PASS |
| R-2 普通重启 202 | PASS |
| R-3 普通重启 boot_seq 92→93 | PASS（完成复位并重新上报） |
| R-4 回归后 query_actions 正常（无 CFG-ROLLBACK 迹象） | PASS |
| R-5 强制重启 202 | PASS |
| R-6 回帧 boot_flag_written | **INFO：result 帧丢失，复现 2/2**（300ms 窗口内 MQTT publish 未完成即复位——11 文档 §2.5 预判的场景；ack 帧正常） |
| R-7 强制重启 boot_seq 95→96 | PASS |
| R-8 强制重启后 query_actions 正常 | PASS（配置/registry 行为证据健康） |

两轮验证设备共完成 4 次真机重启（boot_seq 92→96），全部健康回归。

## 5. 已知限制（如实）

1. **强制重启回帧丢失（复现 2/2）**：F-4 的 300ms 窗口内 publish 竞态 ⇒ `boot_flag_written` 等明细不可靠。UI 已容忍（frame 为 null 不展示明细），重启效果由回归监视独立确认。若要修需固件侧改（先 publish 确认再复位）——超出本阶段范围，已记入待办。
2. **普通重启 audit 终态 = accepted 而非 done**：服务端防假-done 设计（MEMORY 铁律：重启族 done⇒accepted）。UI 的回归监视不依赖命令终态，行为诚实。
3. **F-5 Flash 写闸门未实现**：强制重启时若恰有 Flash 写入，靠 Backup+boot_ok 兜底（可能丢一次未落盘修改）。风险文案已在 UI 明示。

## 6. 阶段 4 验收矩阵收口（S4-01~S4-14）

| 组 | 项 | 状态 |
|---|---|---|
| 列表/详情/编辑器/运行/删除/落盘 | S4-01~09 | ✅（前两轮完成，真机验证过） |
| 正常停止 | S4-10 | ✅ 急停 + 本轮物理核实 |
| 强制停止 | S4-11 | ✅ safety.stop_all（与急停同一命令，固件 latched 语义） |
| 普通重启 | S4-12 | ✅ 本轮（真机 2 次） |
| 强制重启 | S4-13 | ✅ 本轮（真机 2 次；回帧丢失记已知限制） |
| 重启后安全核查 | S4-14 | ✅ 回归监视 + query_actions 行为核查（真机 4 次） |

**阶段 4 全部完成。** 下一步：阶段 5（遥测曲线、审计筛选/导出页、异常态完善）。
