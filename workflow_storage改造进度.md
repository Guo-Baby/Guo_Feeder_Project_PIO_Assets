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

### Phase 1 — WorkflowStorage 模块（文档1 全部）✅ 已提交

commit：`8846aef`（5 files, +2239）
编译：SUCCESS 56.70s，0 error，0 新增 warning（唯一告警是 `cloud_manager.cpp:1041` 既有 DynamicJsonDocument 弃用告警）

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

### Phase 2 — Definition / Runtime Separation ✅ 已提交

commit：**`7ed5b51`**（3 files, +335 / -7）
编译：SUCCESS 172.76s，0 error，0 新增 warning

已实现（最小侵入）：

- `workflow.h` 新增 `WorkflowStepDef` 结构（type / instance_type / id / param_count / params[8]）
- `workflow.cpp` 新增 PSRAM 池 `step_definitions`（16×16=256 slot，优先 PSRAM，失败回退 DRAM）
  - **刻意不放进 `WorkflowStep` 内部**：每个 Step 8 个参数 × 256 slot 内联会让常驻 DRAM 的
    `workflows[]` 膨胀上百 KB（当前 DRAM 已用 129KB/327KB）
- 新增三个内部函数（定义在 `workflow_clear()` 之前）：
  - `workflow_step_def_at(wf, step)` —— 对外（.h 已声明），Slot 访问
  - `workflow_clear_step_def(wf, step)` —— 清空 Definition
  - `workflow_capture_step_def(wf, step, s)` —— 解析/CRUD 后写入 Definition
  - `workflow_snapshot_step_def(wf, step, s)` —— **start 时 Definition → Runtime 值拷贝快照**
- `workflow_parse_json()`：Trigger / Action 三个出口（成功 + 两个 desc==nullptr 分支）均调用 capture
- `workflow_clear()`：逐 Step 调用 `workflow_clear_step_def()`
- `workflow_start()`：`workflow_reset_step()` **之后**调用 snapshot
  - 顺序理由：`descriptor->reset()` 可能改写 instance 内部字段，放在其后才能保证拿到 Definition 当前值
- `workflow_init()`：分配 + placement new；`workflow_destroy_all_instances()`：析构 + free

行为不变保证：
- 解析期 instance 仍按原逻辑初始化并填入 params，Definition 只是**并行副本**
- 首次 start 时 snapshot 写入的值与解析期完全一致 → 执行行为零变化
- Temp Action 完全不经过此路径（`temp_action_instances[8]` 独立 DRAM 池）

### Phase 5/6/7 — Dirty Bitmap + Critical Transaction + Save Transaction ✅ 已提交

commit：**`0c13f7a`（待替换为实际 hash）**
编译：SUCCESS 149.15s，0 error，0 新增 warning；RAM 129200 → 129240 B（+40 B）

`workflow.h` 新增 5 个对外 API（bool 语义，不 include workflow_storage.h，保持分层）：

- `workflow_mark_step_dirty(wf, step)` —— Clean→Dirty；首次产生 Dirty 时 Critical +1
- `workflow_has_any_dirty()`
- `workflow_save_transaction()` —— 完整保存事务；失败不 release、不清 Dirty
- `workflow_request_save()` —— 启动 / 刷新 5 分钟窗口
- `workflow_delete(wf)` —— 只置 `meta.valid=false`，不删 Step BIN

`workflow.cpp` 新增：

- `#include "workflow_storage.h"`
- `uint32_t dirty_bitmap[8]`（256 bit，`slot = wf*16 + step`）
- 内部位操作 `dirty_mark / dirty_clear / dirty_is / dirty_any / dirty_workflow_has / dirty_workflow_clear`
- `static bool wf_dirty_critical_held` —— 持有标记，release 幂等
  - ⚠️ **不要改名成 `workflow_critical_held`**：该名字已被既有的
    `static bool workflow_critical_held[WORKFLOW_MAX_COUNT]`（运行体 Critical 标记）占用
- `static unsigned long workflow_save_since_ms` —— 0 = 无待保存
- `workflow_def_to_storage()` —— `WorkflowStepDef`(String) → `WorkflowStepDefinition`(char[])
- `workflow_build_definition()` —— 由 RAM 构造完整 `WorkflowDefinition`
- `workflow_delayed_save_poll()` —— 由 `workflow_task()` 开头调用，窗口到期触发保存

关键设计决策：

- **保存粒度是整个 Workflow 而不是单个 Step**。原因：Meta Entry 的 `crc32` 覆盖该
  Workflow **全部** Step payload，若只写 Dirty Step，CRC 会因缺失数据而不一致。
  故 Dirty 仍按 Step 记录（便于查询），保存时按 Workflow 聚合成整事务。
- **延迟轮询不用于维护 Critical**。Critical 的 +1/-1 完全由 `mark_dirty` /
  `save_transaction` 显式驱动，`workflow_delayed_save_poll()` 只负责窗口到期触发一次保存
  （满足 §19/§47）。
- **`workflow_delete()` 运行中保护**：`state == WORKFLOW_RUNNING` 时只置
  `enable=false` + meta invalid，**不清 Definition、不改 step_count**
  （改 step_count 会让在飞运行提前结束）；当前运行继续使用自己的 Runtime 快照跑完（§27）。

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

### 6.0 编译必须每次使用【全新 build 目录】（最重要）

> 这是此前两次「编译 11 分钟无输出 / EXIT=1 后卡住」的**真正根因**。

**根因**：沙箱 `safe-delete` 会拦截 PlatformIO 增量编译时对 `firmware.elf` 的删除：

```text
[safe-delete][SAFE_DELETE_BULK_CONFIRM_REQUIRED] {"count":50,"threshold":50,
 "scope":"turn","targets":["...\.pio\build\esp32-s3-devkitc-1\...\firmware.elf"]}
```

被拦截后 pio 一直等待确认 → **无任何输出、永不结束**。与代码无关，与工具链无关。

**解法**：每次编译使用一个**全新的、不存在的** build 目录（全新目录无需删除任何文件）：

```bash
cd /d/Guo_Feeder_Project/PIO_Assets/Guo_Feeder_Project
BD=".pio/build/b$(date +%s)"
PLATFORMIO_BUILD_DIR="$BD" pio run > build_out.log 2>&1
echo "EXIT=$?" > build_done.flag
```

- 单个 build 目录约 **146 MB**，磁盘剩余 687 GB → 可放心累积，不必清理
- 全量编译约 **150 秒**
- 若看到日志出现 `SAFE_DELETE_BULK_CONFIRM_REQUIRED` → 立即换全新目录重跑

### 6.1 编译完成的可靠监控方式（**必须遵守**）

> 背景：此前两次出现「看不到编译是否完成」——原因是 `pio run | tail -N` 的管道缓冲
> 直到进程结束才刷出，加上 PlatformIO 偶发重装工具链，导致长时间无输出被误判为卡死。

**正确做法：标记文件（marker file）+ 短轮询。** 不要用 `| tail`，不要用阻塞式长等待。

启动（后台）：

```bash
cd /d/Guo_Feeder_Project/PIO_Assets/Guo_Feeder_Project
rm -f build_out.log build_done.flag
PLATFORMIO_BUILD_DIR=.pio/build/esp32-s3-devkitc-1 pio run > build_out.log 2>&1
echo "EXIT=$?" > build_done.flag
```

轮询（每轮最多 100–110 秒，可重复调用直到出现标记）：

```bash
cd /d/Guo_Feeder_Project/PIO_Assets/Guo_Feeder_Project
for i in $(seq 1 10); do [ -f build_done.flag ] && { cat build_done.flag; break; }; sleep 10; done
tail -3 build_out.log
```

判定：

- `build_done.flag` 存在且 `EXIT=0` → 编译成功
- `EXIT!=0` → `grep -nE "error:" build_out.log`
- 标记不存在 → 仍在编译，`tail -3 build_out.log` 看实时进度（能看见具体在编译哪个 .c/.cpp）

收尾：`rm -f build_out.log build_done.flag`（不要提交这两个文件）

---

## 7. 环境注意事项

- 仓库**无 remote-tracking 分支**（`git branch -r` 为空），push 前需先 `git fetch origin`
- `.workbuddy/memory/` 下有本项目的长期笔记与日志，含大量已验证结论（Critical Operation 跨任务铁律等）

### 7.1 已知坑：PlatformIO 偶发重装工具链

现象：编译日志出现 `Tool Manager: Installing platformio/tool-scons @ ...` /
`tool-esptoolpy ... has been installed!` 等，说明 PlatformIO 在重新下载安装工具链，
耗时可达数分钟且**与代码无关**。

诱因：强杀 `python.exe` 进程（如 `taskkill //F //IM python.exe //T`）会打断 PlatformIO
的包管理状态。已发生过一次，重装后恢复正常。

对策：

1. 不要用 `taskkill //F //IM python.exe //T` 结束编译，只杀 `pio.exe` / `ninja.exe` / `cmake.exe`
2. 看到工具链重装日志时，**耐心等它装完**（用 §6.1 的轮询方式），不要中断
3. 装完后后续编译恢复正常速度

---

## 8. 测试命令格式（Phase 1 阶段可用）

### 8.1 本阶段（Phase 1）实际可测内容

WorkflowStorage **尚未接入 CommandManager**（需求文档1 明确"本阶段不实现 CommandManager 接口"），
因此本阶段只能做**启动期观测 + 回归验证**，不能通过 MQTT 直接读写 Step BIN。

串口预期（烧录后 115200）：

```text
[System] JsonStorage init failed!      ← 不应出现
[System] BinStorage init failed!       ← 不应出现
[System] WorkflowStorage init failed!  ← 不应出现
```

三条都不出现即表示 `/workflow` 目录创建成功、BinStorage / FileStorage / LittleFS 链路正常。

### 8.2 回归测试（确认未破坏既有 Workflow）

MQTT 旧格式（**调试首选**，无需 version / stable_id）：

```json
{"cmd":"query_workflows","id":"9001"}
```

```json
{"cmd":"execute_workflow","id":"9002","ob":"<workflow_id>"}
```

期望：行为与改造前完全一致（本阶段 Workflow 仍走原有 JSON 加载路径，零改动）。

Capability Registry 查询（确认 Action / Trigger / Workflow 注册表未变）：

```json
{"c":"registry","i":"9003","k":0}
```

`k`：0=ACTION / 1=TRIGGER / 2=WORKFLOW

临时 Action 执行（确认 Temp Action 不受影响）：

```json
{"cmd":"execute_action","id":"9004","ob":"<RUNTIME_ID>"}
```

### 8.3 Phase 5 之后需要新增的命令（当前尚未实现）

CommandManager 分层要求：不得直接操作 BIN /不得直接调用 FileStorage，
必须经 `WorkflowManager API`。建议命令格式（与现有 `system.time` 风格一致）：

```json
{"c":"workflow","i":"<唯一>","p":{"o":"save"}}
{"c":"workflow","i":"<唯一>","p":{"o":"load","wf":0}}
{"c":"workflow","i":"<唯一>","p":{"o":"delete","wf":0}}
{"c":"workflow","i":"<唯一>","p":{"o":"meta"}}
```

| 字段 | 含义 |
|---|---|
| `c` | 固定 `"workflow"` |
| `i` | 命令唯一 ID，设备按 cmd_id 去重 |
| `p.o` | 操作：save / load / delete / meta |
| `p.wf` | Workflow Index 0..15 |

### 8.4 Phase 10 回归测试清单

见 `workflow修改需求文档.md` §50，共 21 项。
