# Workflow Storage + Definition/Runtime Separation 改造进度

> **给后续 AI 节点：本文件是断点续做唯一入口。**
> 接手时请先读本文件，再读 `## 待办` 中标记 `[进行中]` / `[未开始]` 的章节，
> 对照 `## 已完成` 确认仓库实际状态（`git log`），不要重复已完成的工作。

版本：V1
基线 commit：`9a7fe2e`（feat(storage): 新增 FileStorage / BinStorage 通用二进制存储层）

---

## 0. 需求来源与交付范围

| 文档 | 内容 |
|---|---|
| `workflowstorage需求文档.md` | **WorkflowStorage 模块**（BIN 格式 / Meta / CRC / 原子写 / Lazy Load / API） |
| `workflow修改需求文档.md` | **Workflow.cpp Definition/Runtime 分离 + Dirty + Critical 事务 + CRUD**（50+ 节） |

用户要求：
1. 修改前确保无未提交修改 ✅（已确认 `git status` 干净，基线 `9a7fe2e`）
2. 完成后**编译通过**（不烧录）
3. 完成后 commit
4. 输出测试命令格式
5. token 不足中断时，换节点可继续 → 本文件

---

## 1. 两份需求文档的冲突与裁决

| # | 冲突点 | 文档1（storage） | 文档2（workflow） | **裁决** |
|---|---|---|---|---|
| 1 | 文件布局 | `/workflow/wf00/step00.bin`（目录式） | `/workflow/W00S00.bin`（扁平，标注"建议"） | **采用文档1**（storage 专项文档；文档2 §3 明确写"建议"） |
| 2 | step_count 归属 | 存于 `WorkflowMetaEntry.step_count`（§六 有完整论证） | 属于 Definition 不属于 Meta（§5） | **持久化放 Meta**（文档1 §六 论证：无 step_count 则加载时无法判断 Step 是否应读取）；RAM 中 `WorkflowDefinition.step_count` 同时存在，加载时以 Meta 为准并交叉校验 |
| 3 | 错误码 | 复用 `BinStorageResult` + 增加 Workflow 语义错误 | — | **定义 `WorkflowStorageResult`**（数值对齐 `BinStorageResult`，额外 4 个语义码）。理由：验收标准"不修改 BinStorage 原则上禁止"优先，故不污染底层枚举 |

---

## 2. 已完成

### Phase 1 — WorkflowStorage 模块（文档1 全部）`[代码已写入，待编译/提交]`

commit：**尚未提交**（代码已落盘，等待编译结果后提交）

新增文件：
- `src/workflow_storage.h`
- `src/workflow_storage.cpp`
- `main.cpp` 接入 `workflow_storage_init()`（第一层，位于 `bin_storage_init()` 之后）

**未触碰**：`workflow.cpp` / `workflow.h` / `command_manager.*` / `config_manager.*` / `json_storage.*` / `bin_storage.*` / `file_storage.*`

设计要点（后续改造必须遵守）：
- 路径：`/workflow/meta.bin`、`/workflow/wf%02u/step%02u.bin`
- Meta Header 12B：`magic(4) version(2) workflow_count(2) crc32(4)`
- Meta Entry 12B × 16：`valid(1) version(1) step_count(2) update_time(4) crc32(4)`
  - Entry.crc32 = 该 Workflow 全部 Step payload 的 CRC32（save 时计算，load 时校验）
- Step BIN：`header(16B) + payload`
  - Header：`magic(4) version(2) header_size(2) payload_size(4) crc32(4)`
  - Payload 首部含 `workflow_index` / `step_index`（满足文档2 §7"可验证属于正确 Workflow/Step"）
  - CRC32 只覆盖 payload
- **全部手工逐字节序列化**（put/get u8/u16/u32/float/bytes），**不 memcpy 结构体**，规避对齐与 padding 差异
- `WorkflowStorageResult` 错误码见 `workflow_storage.h`
- 提供 `workflow_storage_load/save/delete/load_step/save_step` + `*_meta` / `*_valid` 全套

---

## 3. 待办（按文档2 Phase 顺序）

### Phase 2 — Definition / Runtime Separation `[未开始]`
- 现状：`WorkflowStep.instance` 在 `workflow_parse_json()` 加载期从 PSRAM 池分配，params（定义）与 state（运行时）混在同一对象；`workflow_start()` 不新建、不拷贝
- 目标：`Workflow { Definition + Runtime }`，Runtime 持有 params **值拷贝快照**

### Phase 3 — 统一 Runtime Pool `[未开始]`
- 现有 `trigger_instances[256]` + `action_instances[256]`（PSRAM）→ 合并为单一 `StepRuntime Pool[256]`
- 与 `temp_action_instances[8]`（DRAM）**必须继续物理隔离**（文档2 §41）

### Phase 4 — Definition RAM `[未开始]`
- 建立独立 `WorkflowDefinition[]`，字段：id / name / enable / timeout_ms / step_count / steps[]

### Phase 5 — Step BIN + Meta 接入 `[未开始]`
- 用 Phase 1 的 `workflow_storage_*` 替换现有 `workflow_load_json_file` / `workflow_save_json_file`
- 启动加载：读 meta → 遍历 valid → 读 Step 0..step_count-1（文档2 §8）

### Phase 6 — Dirty Bitmap `[未开始]`
- `uint32_t dirty_bitmap[8]`（256 bit），`slot = wf*16 + step`
- `mark_dirty / clear_dirty / is_dirty / has_any_dirty`

### Phase 7 — Dirty Transaction + Critical `[未开始]`
- Clean→Dirty 且 `has_any_dirty()==false` 时 `critical_operation_acquire()`（仅 +1 一次）
- Save 全部成功（BIN + CRC + Meta + 清 Dirty）后 `release()`
- 失败：不 release、不清 Dirty（文档2 §18/§25）

### Phase 8 — 延迟保存 `[未开始]`
- 参考 ConfigManager 5 分钟窗口；显式 `workflow_save` 立即触发

### Phase 9 — Workflow CRUD `[未开始]`
- create / update / delete；delete 只置 `meta.valid=false`，**不删 Step BIN**（文档2 §26）

### Phase 10 — 回归验证 `[未开始]`
- 文档2 §50 列出 21 项测试

---

## 4. 硬性约束（改动前必读）

- 常量不得改变：`WORKFLOW_MAX_PARAM=8` / `WORKFLOW_MAX_STEP=16` / `WORKFLOW_MAX_COUNT=16`
- `workflow.cpp` 不得出现 `LittleFS.open/remove/rename`，不得 `#include <LittleFS.h>`（文档2 §37）
- 不得持久化：函数指针 / descriptor 指针 / runtime / callback / running / result（文档2 §6、文档1 §八）
- BIN 只存 Action/Trigger **ID**，加载时经 `find_descriptor()` 恢复（文档2 §44）
- Step 0 必须 Trigger；`current_step >= step_count` 判定结束（文档2 §33/§5）
- Critical Operation 用**现有** `system_command_critical_operation_acquire/release` + `system_command_request_restart()`，**不得重新设计状态机**（文档2 §24）
- Temp Action 行为不得改变（文档2 §42）
- 不得为了 Dirty/Critical 做持续 loop 轮询（文档2 §19/§47）

---

## 5. 关键代码位置（当前基线）

| 内容 | 位置 |
|---|---|
| PSRAM 实例池 `trigger_instances` / `action_instances` 各 256 | `workflow.cpp:751-755` |
| `workflow_parse_json()` 加载期分配并链接实例 | `workflow.cpp:1181` |
| `workflow_start()` 复用池实例，不新建 | `workflow.cpp:1522`（内部 reset） |
| `workflow_reload()` Running 守卫 | `workflow.cpp:1471-1478` |
| 唯一写盘 `workflow_save_json_file`（整体覆盖 `/workflow.json`，直接 LittleFS） | `workflow.cpp:1432` |
| `workflow_load_json_file`（不防运行中，会 `workflow_clear`） | `workflow.cpp:1450` 附近 |
| 临时 Action 池 `temp_action_instances[8]`（DRAM） | `workflow.cpp:162` |
| 全系统唯一 `ESP.restart()` | `system_command.cpp:312` |

---

## 6. 编译方式（必须遵守）

沙箱 `safe-delete` 会拦截增量编译删除 `.o`，**增量编译必然失败**（非代码问题）：

```bash
cd /d/Guo_Feeder_Project/PIO_Assets/Guo_Feeder_Project
PLATFORMIO_BUILD_DIR=.pio/build/esp32-s3-devkitc-1 pio run
```

全量约 70–130 秒。

---

## 7. 环境注意事项

- 仓库**无 remote-tracking 分支**（`git branch -r` 为空），push 前需先 `git fetch origin`
- `.workbuddy/memory/` 下有本项目的长期笔记与日志，含大量已验证结论（Critical Operation 跨任务铁律等）
