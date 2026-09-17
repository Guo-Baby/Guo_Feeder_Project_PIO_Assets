# LogManager P2 开发进度

> 用途：**会话中断后的续接基线**。每次完成一个模块接入必须更新本文件。
> 上游文档：`docs/LogManager-Integration-Guide.md`（接口规范）· `docs/P2_Log_Integration_Matrix.md`（模块矩阵）· `log模块历史/LogManager-P2接入准备审查0918.md`（前置审查）
> 更新规则：只记「已完成 / 当前基线 / 下一步 / 未决事项」，不记过程细节。

---

## Completed

| 阶段 | 内容 | commit | 状态 |
|---|---|---|---|
| P1.1–P1.5 | LogManager 核心（RAM 环 / Flash 段环 / Cloud / ACK·Retry / Replay / H2 / D1） | `5816b32`、`c29d9d5`、`d1ccdbe`、`65b4523` | ✅ 已提交 |
| 仓库整理 | 凭据文件取消跟踪 + 0917 报告入库 | `1d0b47c` | ✅ 已提交 |
| P2 前置 | 接入矩阵 + P2 接入准备审查（只出文档） | 见下方「待入库」 | ✅ 文档已生成 |
| **阶段 0** | 工作区确认（HEAD、`src/`/`test/` 无未提交改动） | — | ✅ |
| **阶段 1** | **P1.5 上板验证**（187 断言 → 167 OK / 20 MISS，全部 MISS 归因为用例预期缺陷） | `6a82fe1`（用例 op 名修正）、`789f53b`（验证报告） | ✅ 已提交 |
| **阶段 2** | **P2-A Storage bridge 接入**（json_storage 37 + file_storage 30 = 67 处 E/W 由静默变为可观测） | `930ed45` | ✅ 已提交 |
| **阶段 3** | 建立本进度文档 | `6485e25` | ✅ |
| **测试资产修正** | 按首轮归因修正 14 处预期值 + 回归器加 `<BOOT>` 宏 + 重录基线 | `test(log): fix P1.5 case expectations and re-baseline` | ✅ 已提交 |
| **收尾 · BT-1** | 云端 `log_ack`：**Worker 不在本仓库** ⇒ 仅记录接口要求，未改设备协议 | — | 📝 已记录 |
| **收尾 · holes=2** | 代码审查结论：**设计行为**（两个独立来源、相邻才合并）⇒ 更新用例而不改实现 | 见上方提交 | ✅ |
| **阶段 2-B** | **P2-B ConfigManager 接入**：`config_log_bridge` 桥接（关键词分类 + 10 s 去重 + SKIP 防重复）+ 2 个显式语义埋点（`CHANGE_APPLIED` / `SAVE_OK`）+ `cfg_hash32()` | `feat(log): integrate config manager logging` | ✅ 已提交 |
| **BT-9 修复** | **ACK 水位越过未确认补发记录**（at-least-once 破坏）：拆成 `s_cloud_acked_seq`（真实最大，仅观测）+ `s_cloud_gc_seq`（连续可回收，全部判定）+ `s_cloud_gc_floor`（backlog 下界钳制）；新增 `log_ack_gc_watermark()` 纯函数；补发/段回收/淘汰记账/**ACK 重复检测**全部改用连续水位 | `fix(log): prevent ack watermark bypass replay records` | ✅ 已提交 |
| **BT-9 回归** | 全量重跑 **195/195 = 100%，0 MISS**（首轮 167/187 → R1 187/189 → 195/195）；F2-B 两条原 KNOWN-FAIL 断言（`replay=8` / `qused=8`）**由 FAIL 转真实 PASS** | 同上 | ✅ |
| **阶段 2-C** | **P2-C WiFi 接入**（`src/wifi_module.cpp`，唯一改动文件）：5 个埋点（`CONNECT_START` / `CONNECTED` / `CONNECT_TIMEOUT` / `LOST` / `RECONNECT_TRY`）+ 失败循环节流（N 次记 1 次 **AND** ≥60s）+ `wifi_ssid_hash32()` | `feat(log): integrate wifi module logging` | ✅ 已提交 |

---

## Current baseline

```
HEAD:           见 `git log --oneline -1`（每次提交后更新本行）
分支:           wb
P1.5 设备侧:    ✅ 不再 BLOCKED（A–E 全部可跑段落已完成）
回归基线:       ✅ 195/195 = 100%，0 MISS（test/log_fix_tests.txt，BT-9 修复后全量重跑）
                   首轮 167/187 → R1 187/189（2 MISS=BT-9）→ 本轮 195/195
合约测试:       ✅ 4/4 ALL PASS（含新增第 ⑧ 组 23 条 BT-9 断言，其中 3 条负向探针）
```

### 已接入 LogManager 的模块

| 模块 | 接入方式 | 事件覆盖 | 提交 |
|---|---|---|---|
| **Storage（json_storage / file_storage）** | `main.cpp` 回调桥接（`json_storage_log_bridge` / `file_storage_log_bridge`） | `LOG_STG_FS_UNAVAILABLE` / `ATOMIC_WRITE_FAILED` / `CRC_FAILED` / `TXN_RECOVERED` / `WRITE_VERIFY_FAILED` / `READ_FAILED` | P2-A |
| **Config（ConfigManager）** | `main.cpp` 桥接（`config_log_bridge`，**双路串口**）+ `config_manager.cpp` 内 2 处显式埋点 | `LOG_CFG_LOAD_DONE` / `MODULE_LOAD_FAILED` / `RECOVERED_FROM_BACKUP` / `COMMIT_FAILED_ROLLBACK` / `VERSION_REBUILT` / `WRITE_REJECTED` / `RESTART_TIMEOUT` + **`CHANGE_APPLIED`** / **`SAVE_OK`** | P2-B |
| **WiFi（`wifi_module.cpp`）** | 无回调接口 ⇒ **直接显式埋点**（5 处，全在既有分支内） | `LOG_WIFI_CONNECT_START` / `CONNECTED` / `CONNECT_TIMEOUT` / `LOST` / `RECONNECT_TRY` | P2-C |
| 其余 8 个模块 | **未接入** | — | — |

未接入清单（按约定顺序）：**Cloud → Time → Workflow → Weight → Valve → Dispense → BLE → Command/Event/OLED/Registry**

（未实现的冻结 EventId：`LOG_CFG_FACTORY_RESET`(0x0208) —— 代码中**不存在** factory reset 函数，无宿主，记为未实现。）

---

## Next

### 下一步：**Cloud / MQTT 接入**（P2-D）

WiFi 已接入完成（见上），回归 **195/195 全绿**。下一个按约定顺序是 **Cloud**（`src/cloud_manager.cpp`，1656 行）：

1. 关键状态迁移点（矩阵 §5.2）：`MQTT_EVENT_CONNECTED`(:233) / `DISCONNECTED`(:258) /
   `PUBLISHED`(:266) / `ERROR`(:314)；`cloud_task()` 的 WiFi 断开(:1435)、进入 sleep(:1490)、
   退出 sleep(:1470)。
2. ⚠️ **`mqtt_connected` 布尔量已有**（`cloud_manager.cpp:47`）⇒ 连接/断连必须用**边沿**发，
   事件重复到达时不重复发（H-7）。
3. ⚠️ **MQTT 发布失败**（H-6）离线时每 loop 可能失败 ⇒ **计数聚合**（每 60s 最多 1 条，
   带 `LOG_P_DROPPED_TOTAL`），不要每条失败都发。
4. ⚠️ 云端 `log_ack` 相关常量（`LOG_ACK_COMMAND` 等）已在 P1.5 引入，属**协议级**，
   不要与业务埋点混淆。
5. 保持非阻塞：埋点只能放在已有分支里，**不得**新增等待。
6. 提交主题建议：`feat(log): integrate cloud manager logging`

### 之后（严格一次一个模块）

`Time → Workflow → Weight → Valve → Dispense → BLE → Command/Event/OLED/Registry`

⚠️ **Workflow 接入前必须先评审** `workflow_terminate()` 的 Critical Op release 收口路径
（项目铁律：release 不能放在会中途 return 的函数里）。详见
`log模块历史/LogManager-P2接入准备审查0918.md` §7 R-7。

---

## 未决 / 阻塞事项

| ID | 事项 | 阻塞什么 | 状态 |
|---|---|---|---|
| **BT-1** | **云端从未回送 `log_ack`**（`guo_feeder/down`）⇒ `acked_seq` 不前进 ⇒ Flash 段永不回收 | 线上回收闭环；P2 接入后日志量上升会加速暴露（496 条写满后开始淘汰未确认记录） | 🔴 **未解决**（外部依赖）。**Cloud Worker 代码不在本仓库**（本仓库只有 `cloud_protocol.md`）⇒ 按约定**只记录接口要求，不改设备侧协议**。接口要求见 §"BT-1 接口要求" |
| ~~**BT-9**~~ | ~~live ACK 越过旧未确认补发记录 ⇒ 永久跳过~~ | — | ✅ **已修复**（`fix(log): prevent ack watermark bypass replay records`）。方案＝**状态语义分离**：`acked_seq`（真实最大，仅观测）/ `gc_seq`（连续可回收，全部判定）/ `gc_floor`（钳制下界）。详见 Guide §11 与 Board-Test-Report 附录 R2 |
| **BT-10**（新） | 云队列溢出淘汰时**只给本 Boot 记录**登记空洞（`evict_boot == s_boot_seq`）⇒ 补发进来的上一 Boot 记录被淘汰后无空洞保护 | 复合低概率残余项（需同时：队列满且队首补发记录未发出 + 之后有更高 ACK + 触发段回收） | ⏳ **未修**（超出本次范围）。修法＝去掉该条件；代价＝空洞表压力上升（补发记录 seq 稀疏难合并，`LOG_HOLE_MAX=8` 可能溢出⇒退化为本 Boot 停止回收）。**F1 的 `holes=0` 断言正钉住当前行为** ⇒ 需单独评审 |
| **BT-11**（新，P2-C 期间发现） | `s_cloud_give_up_seq` 是**单点水位**：较新的批次先 give-up（实测 INFO 批次 seq 3073-3075 ⇒ `give_up_seq=3075`）后，seq 更小、仍在 Flash 等待补发的旧记录（seq≈1800）被 `log_ack_should_replay(seq <= give_up_seq)` 跳过 ⇒ **本 Boot 补发不到**（下次 Boot 归 0 后恢复，故**不丢数据**，属延迟） | 长离线后恢复时的补发**及时性** | ⏳ **未修**（P2-C 明令"不改 ACK/replay/GC 逻辑"）。与 BT-9 同源（单点水位越过旧记录），需在 LogManager 侧单独评审 |
| BT-4~BT-8 | ~~`test/log_fix_tests.txt` 中 20 条 MISS 的预期值修正~~ | P1.5 回归"全绿"基线 | ✅ **已修正 14 处**；回归现为 **195/195 = 100%，0 MISS** |
| P2-A 未验证项 | Storage bridge 的 **ERROR / CRITICAL 分支**与 **10 s 抑制窗口**无法用现有钩子触发 | 桥接层完整性 | ⏳ 待"结构化回调 + 故障注入"一并解决 |
| F2 决策 | 字符串参数是否改结构化回调（现为哈希/枚举化） | Storage 路径、Config key、Workflow id/action id 等 7 个 ParamId 的可用性 | ⏳ 已按"暂不扩 API"执行，未来可评审 |
| F6 基线 | P1.5 之前的既有回归集（P1.2 核心 66 / Flash 92 / F5 恢复 34 / F7-F9 43 / 交接 56）**未复跑** | P2 首个模块接入前的回归基线 | ⏳ 建议尽快补跑 |

---

## BT-1 接口要求（Worker 不在本仓库，仅记录要求）

云端需要在 `guo_feeder/down` 上实现 `log_ack` 回执。设备侧**已具备**接收能力并已在真机验证
（`[Cloud LOG] ack rx` 可被接收；**不带 `i` 也须被处理**；协议级消息必须旁路命令去重缓存）。

| 项 | 要求 |
|---|---|
| 命令名 | `"c": "log_ack"`（常量 `LOG_ACK_COMMAND`） |
| 参数键 | `"p": { "b": <boot_seq>, "f": <seq_from>, "t": <seq_to> }` |
| `i` 字段 | **可选**。不带 `i` 必须被正常处理（不得回 "Missing id"） |
| `b` | 必须回**设备上报的 boot_seq**（`b` 不匹配 ⇒ 设备 IGNORE） |
| `f` / `t` | 确认的 seq 闭区间；`f<=t` 且都非 0。覆盖整个批次 ⇒ ACCEPT；只覆盖前缀 ⇒ PARTIAL |
| **幂等义务** | 设备侧是 **at-least-once**（重启后 `acked_seq` 归 0，会重发）⇒ **云端必须按 `(device_id, boot_seq, seq)` 幂等**，重复 ACK 无副作用 |
| 触发时机 | 收到 `guo_feeder/log` 批次并**成功入库**后回一次 ACK（建议按批次回 `f/t` = 该批次的 seq 区间） |
| 不 ACK 的后果 | 设备走满 6 次超时 + 2/4/8/16/32 s 退避（≈152 s）后 give-up，**不推进水位、不删段** ⇒ 存储泄漏 + 重复补发 |

> ⚠️ 注意：**BT-9 未修之前，即使云端实现了 `log_ack`，旧未确认记录仍可能被越过**。
> 建议 BT-1 与 BT-9 一起解决（先修 BT-9 语义，再让云端闭环）。

---

## 本阶段对文档的产出

| 文档 | 内容 |
|---|---|
| `docs/LogManager-Integration-Guide.md` §9 | **新增**：Storage 回调桥接（注册方式 / 语义还原 / 参数映射 / 高频抑制 / 生命周期 / 上板结果） |
| `docs/LogManager-Integration-Guide.md` §10 | **新增**：ConfigManager 接入（与 Storage 的 3 点差异 / 关键词映射 / 2 个显式埋点 / 哈希 / 上板结果） |
| `docs/LogManager-Integration-Guide.md` **§11** | **新增**：**watermark 语义分离（FIX-BT9）** —— 根因 / 两个标量 / 为何 classify 也要改 / `gc_floor` 生命周期 / 刻意未改的部分 / 残余 BT-10 / 观测 / 验收 |
| `docs/LogManager-Integration-Guide.md` **§12** | **新增**：**WiFi 接入** —— 无回调接口⇒显式埋点 / 5 个埋点表 / 失败循环节流（N 次记 1 **AND** ≥60s）/ 为何重连不发 CONNECT_START / WAS·STATE 编码 / 参数语义 / 上板验证 / 已知限制 |
| `docs/LogManager-P1.5-Board-Test-Report0918.md` | **新增**：P1.5 上板验证报告（环境 / 初始化 / MQTT / ACK / replay / F0–F8 / 已知问题）；**附录 R1**（用例修正与基线重录）；**附录 R2**（BT-9 分析·修复·验证 + 新基线 195/195） |
| `docs/P2_Log_Integration_Matrix.md` · `log模块历史/LogManager-P2接入准备审查0918.md` | 前置审查 —— 已随 `85c88b5` 入库 |

### 入库状态（已核实 `git ls-files`）

| 文件 | 说明 |
|---|---|
| `docs/P2_Log_Integration_Matrix.md` | ✅ 已入库（`85c88b5`） |
| `log模块历史/LogManager-P2接入准备审查0918.md` | ✅ 已入库（`85c88b5`） |
| `AI_RULES.md` | 已加「§9 Build/Test/Cleanup Rules」，但该文件**已被 `.gitignore` 忽略**（见 commit `1d0b47c`）⇒ 改动不会入库 |

---

*最后更新：2026-09-17（P2-C WiFi 接入完成，回归 195/195；下一步 P2-D Cloud）*
