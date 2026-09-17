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
| **阶段 2** | **P2-A Storage bridge 接入**（json_storage 37 + file_storage 30 = 67 处 E/W 由静默变为可观测） | `feat(log): connect storage callbacks to LogManager` | ✅ 已提交 |
| **阶段 3** | 建立本进度文档 | 本文件 | ✅ |

---

## Current baseline

```
HEAD:           见 `git log --oneline -1`（每次提交后更新本行）
分支:           wb
P1.5 设备侧:    ✅ 不再 BLOCKED（A–E 全部可跑段落已完成）
```

### 已接入 LogManager 的模块

| 模块 | 接入方式 | 事件覆盖 | 提交 |
|---|---|---|---|
| **Storage（json_storage / file_storage）** | `main.cpp` 回调桥接（`json_storage_log_bridge` / `file_storage_log_bridge`） | `LOG_STG_FS_UNAVAILABLE` / `ATOMIC_WRITE_FAILED` / `CRC_FAILED` / `TXN_RECOVERED` / `WRITE_VERIFY_FAILED` / `READ_FAILED` | P2-A |
| 其余 10 个模块 | **未接入** | — | — |

未接入清单（按约定顺序）：**Config → WiFi → Cloud → Time → Workflow → Weight → Valve → Dispense → BLE → Command/Event/OLED/Registry**

---

## Next

### 下一步：**Config 接入**（P2-B）

依据 `docs/P2_Log_Integration_Matrix.md` §2 与 `log模块历史/LogManager-P2接入准备审查0918.md`：

1. **第一步仍用桥接**：`config_set_log_callback()` 已在 `main.cpp` 之外从未注册
   （当前 `main.cpp` 只注册了 `bin_storage` / `command_manager` / 新增的两个 Storage）
   ⇒ 先在 `setup()` 注册 `config_log_bridge`，零侵入拿到 ConfigManager 既有 E/W。
2. **第二步补语义埋点**（约 12 处，全部用已冻结的 10 个 Config EventId）：
   `LOG_CFG_LOAD_DONE` / `MODULE_LOAD_FAILED` / `RECOVERED_FROM_BACKUP` / `SAVE_OK` /
   `COMMIT_FAILED_ROLLBACK` / `VERSION_REBUILT` / `FACTORY_RESET` / `WRITE_REJECTED` /
   `CHANGE_APPLIED` / `RESTART_TIMEOUT`。
3. ⚠️ `LOG_P_KEY`（0x1E）为字符串语义、**当前不可达** ⇒ 按 P2 定版走"哈希/枚举化"
   （沿用 `LOG_P_SSID_HASH` 先例）或省略，不得为此扩 API。
4. 提交主题建议：`feat(log): integrate config manager logging`

### 之后（严格一次一个模块）

`WiFi → Cloud → Time → Workflow → Weight → Valve → Dispense → BLE → Command/Event/OLED/Registry`

⚠️ **Workflow 接入前必须先评审** `workflow_terminate()` 的 Critical Op release 收口路径
（项目铁律：release 不能放在会中途 return 的函数里）。详见
`log模块历史/LogManager-P2接入准备审查0918.md` §7 R-7。

---

## 未决 / 阻塞事项

| ID | 事项 | 阻塞什么 | 状态 |
|---|---|---|---|
| **BT-1** | **云端从未回送 `log_ack`**（`guo_feeder/down`）⇒ `acked_seq` 不前进 ⇒ Flash 段永不回收 | 线上回收闭环；P2 接入后日志量上升会加速暴露（496 条写满后开始淘汰未确认记录） | 🔴 **未解决**（外部依赖，需云端 Worker 实现，且必须幂等） |
| BT-4~BT-8 | `test/log_fix_tests.txt` 中 20 条 MISS 的预期值修正（算术错 / 设计语义 / 时序不可观测 / 用例用错工具 / `holes=2` 未归因） | P1.5 回归的"全绿"基线 | ⏳ 待用例作者确认 |
| P2-A 未验证项 | Storage bridge 的 **ERROR / CRITICAL 分支**与 **10 s 抑制窗口**无法用现有钩子触发 | 桥接层的完整性 | ⏳ 待"结构化回调 + 故障注入"一并解决 |
| F2 决策 | 字符串参数是否改结构化回调（现为哈希/枚举化） | Storage 路径、Config key、Workflow id/action id 等 7 个 ParamId 的可用性 | ⏳ 已按"暂不扩 API"执行，未来可评审 |
| F6 基线 | P1.5 之前的既有回归集（P1.2 核心 66 / Flash 92 / F5 恢复 34 / F7-F9 43 / 交接 56）**未复跑** | P2 首个模块接入前的回归基线 | ⏳ 建议 P2-B 之前补跑 |

---

## 本阶段对文档的产出

| 文档 | 内容 |
|---|---|
| `docs/LogManager-Integration-Guide.md` §9 | **新增**：Storage 回调桥接（注册方式 / 语义还原 / 参数映射 / 高频抑制 / 生命周期 / 上板结果） |
| `docs/LogManager-P1.5-Board-Test-Report0918.md` | **新增**：P1.5 上板验证报告（环境 / 初始化 / MQTT / ACK / replay / F0–F8 / 已知问题） |
| `docs/P2_Log_Integration_Matrix.md` · `log模块历史/LogManager-P2接入准备审查0918.md` | 前置审查（**尚未入库**，见下） |

### 尚未入库的文件（待决定）

| 文件 | 说明 |
|---|---|
| `docs/P2_Log_Integration_Matrix.md` | P2 前置产物，未提交 |
| `log模块历史/LogManager-P2接入准备审查0918.md` | P2 前置产物，未提交 |
| `AI_RULES.md` | 已加「§9 Build/Test/Cleanup Rules」，但该文件**已被 `.gitignore` 忽略**（见 commit `1d0b47c`）⇒ 改动不会入库 |

---

*最后更新：2026-09-17（阶段 3）*
