# Workflow 架构适配 CommandManager 及 Version —— 改造进度

> 需求文档：`workflow架构适配commandmanager及version需求.md`
> 本文件是断点续做入口：中断后先读本文件，再对照 git log 定位进度。

---

## 0. 目标与边界

| 项 | 内容 |
|---|---|
| 目标 | 云端能查询 / 比对 / 增量同步 / 远程 CRUD Workflow，并主动立即保存 |
| 核心机制 | Workflow 新增 `variant`（内容版本）→ 接入 CapabilityRegistry `object_version` → 参与 checksum → registry version++ → 云端 `workflow.list` 发现不同 → `workflow.get` 拉取 |
| 硬边界 | CommandManager 不解析 BIN、不直接操作 LittleFS；CloudManager 不理解 variant / stable_id；BIN 格式不暴露给云端（云端只见 JSON） |

数据流：

```
Cloud ──MQTT JSON──> CloudManager ──> CommandManager ──> workflow.cpp API ──> workflow BIN
                                            │
                                            └──> CapabilityRegistry (stable_id ↔ runtime_id, object_version)
```

---

## 1. 提交记录

| commit | 内容 |
|---|---|
| `ca168cf` | feat: variant 版本机制 + CommandManager workflow.* 云端同步命令（主体） |
| `d3835cf` | fix: apply_json 漏推进 workflow_count + 补串口 cm 分发 |
| `82afacb` | fix: 补回 workflow.* 一级路由分支 |
| `beff2d3` | fix: registry rescan 栈上 2.4KB 改堆分配（修 loopTask 栈溢出崩溃） |
| `3b0ba11` | fix: storage load 未回填 variant → 重启后版本倒退为 1 |

## 1.1 上板实测暴露的 3 个缺陷（均已修）

| # | 现象 | 根因 | 修复 |
|---|---|---|---|
| 1 | `workflow.create` 后 Guru Meditation / Stack canary (loopTask) | `registry_sync()` 栈上放 3 个大对象（~2.4KB），命令路径栈已用掉大半 | 改 `heap_caps_malloc` + placement new 就地构造；并加 `-DARDUINO_LOOP_STACK_SIZE=16384` |
| 2 | 新建的 Workflow 重启前对云端不可见 | `workflow_apply_workflow_json()` 未推进 `workflow_count`，Registry 以它为扫描上限 | 在 variant 推进前补 `workflow_count` |
| 3 | 改到 variant=2 并落盘，重启后变回 1 | `workflow_storage_load()` 拷贝 Meta 字段时漏掉 v3 新增的 `variant` | 补 `definition->variant = e.variant` |

### Step 6 — 编译烧录 + 上板验证 `[完成]`

- 编译：SUCCESS 82.03s，0 error；RAM 129320 → 129472 B（+152B）
- 烧录：esptool `write_flash -z 0x10000`（本环境 `pio run -t upload` 只编译不烧）
- 回归：16/16 期望命中，0 崩溃（日志 `.pio/cm_run5.log`）
- 持久化：create→set(variant 2)→save→重启→variant=2 ✅
- 删除持久化：delete→重启→条目消失 ✅
- Registry 稳定性：无变更重启 `reuse version=15`（不误 bump）✅
- 栈余量：`stack_hwm=9140`（16KB 栈，约 9KB 可用）

---

## 2. 分步进度

### Step 1 — workflow_storage 增加 variant（Meta v3）`[完成]`

- `WorkflowDefinition` 新增 `uint32_t variant`
- `WorkflowMetaEntry` 新增 `uint32_t variant`
- `WF_STG_META_VERSION` 2 → **3**（entry 85B → 89B）
- 反序列化兼容 v1 / v2（layout 由文件长度判定，缺失字段读 0）
- 新增 `workflow_storage_get_variant()` / `workflow_storage_set_variant()`
- save 事务提交时 `e.variant = definition->variant`

> 注意 `WF_STG_META_BIN_MAX` = 1536，v3 实际 12 + 89×16 = 1436，仍有余量。

### Step 2 — workflow.cpp/h variant 维护 + 单 Workflow JSON 导入导出 `[完成]`

- `struct Workflow` 新增 `uint32_t variant`
- 规则：create = 1；set = +1；delete = +1（删除不是物理删除）；JSON 缺失时视为 1
- BIN 加载：v1/v2 旧 BIN 读到 0 → 统一提升为 1（避免"版本 0"被云端误判）
- `workflow_export_json()`（全量）新增 `"variant"` 字段
- 新增 API：
  - `workflow_get_variant(index)` / `workflow_set_variant(index, v)`
  - `workflow_find_index_by_id(id)` → 槽位或 -1
  - `workflow_export_workflow_json(index, json)` —— **从 Definition 导出**（不依赖 Runtime 是否已实例化），供 `workflow.get`
  - `workflow_apply_workflow_json(index, obj, is_create)` —— 整体替换；先 `set_step_count(0)` 清 Definition，再逐个 `set_step`（复用 Instance，不消耗新的 256 槽）；最后才推进 variant

### Step 3 — CapabilityRegistry 接入 variant + 持久化 `[完成]`

- `scan_workflow_ids()`：`object_version = wf->variant`（原 TODO 已消除）
- 文件格式 **v2**：条目新增 `object_version(uint32)`，魔数整体变更
  （'CAPA/CAPT/CAPW' → 'AP2A/AP2T/AP2W'）→ 旧 v1 文件判定为"不存在"并自动重建，无需兼容解析分支
- `registry_sync()`：object_version 在 **current_table 构建 / reuse / rebuild 三条路径**都要回填
  （漏掉任何一条都会导致"内容变了但 checksum 没变"，云端永远同步不到）
- `registry_checksum()` 早已包含 object_version，v2 只是把它真正落盘
- 新增 `capability_get_workflow_object_version(stable_id, &v)`
- dump / export 输出带 ` v=N`

### Step 4 — CommandManager 6 个 workflow 命令 `[完成]`

新增二级路由 `workflow_router`（一级判定 `command.startsWith("workflow.")`）：

| 命令 | payload | 行为 |
|---|---|---|
| `workflow.list` | — | registry_version / checksum / count / dirty + `workflows[{stable_id,id,variant}]` |
| `workflow.get` | `stable_id` 或 `id` | 返回 `stable_id` + 完整 `workflow` 对象（含 variant / steps / params） |
| `workflow.create` | `workflow` 对象 | 找空槽 → `apply_json(create)` → variant=1 → rescan |
| `workflow.set` | `stable_id`/`id` + `workflow` 对象 | 整体替换 → variant++ → rescan |
| `workflow.delete` | `stable_id` 或 `id` | 只置 valid=false → variant++ → rescan |
| `workflow.save` | 可选 `restart`（默认 true） | `workflow_save_transaction()` → rescan → 有 Dirty 才请求安全重启 |

新增错误码：`CMD_ERROR_INVALID_PAYLOAD 11` / `CMD_ERROR_NO_FREE_SLOT 12` / `CMD_ERROR_REJECTED 13`

调试能力：`command_manager_set_result_echo(bool)` —— 打开后结果 JSON 额外经
`CommandLogCallback` 以 `RESULT` 级别输出一份（生产路径关闭，避免刷屏）。

### Step 5 — main.cpp 串口 cm 直通 + 日志 `[完成]`

- 注册 `command_manager_set_log_callback(command_log_serial)` → `[CMD][level] msg`
- 新增串口命令 `cm {"cmd":...,"ob":...,"id":...,"p":{...}}`：
  与云端下行 JSON 同格式，喂给 `command_manager_execute()`，走完整路由链；
  执行期间临时打开结果回显，串口可直接看到 `[CMD][RESULT] {...}`

### Step 6 — 编译烧录 + 上板验证 `[待]`

---

## 3. 关键设计决策（改动前必读）

1. **variant 不用时间戳**：启动期时间不可信，且时间戳无法表达"改了几次"。
2. **Meta 用版本号兼容而非魔数变更**：BIN 里是真数据，丢不起；v1/v2 按文件长度判定 layout。
   Registry 是**可重建缓存**，所以用魔数变更强制重建最省心。
3. **checksum 必须包含 variant**，且 object_version 必须在三条路径都回填 —— 否则版本机制形同虚设。
4. **apply_json 先清 0 再逐个 set_step**：保证"新 JSON 步骤更少"时尾部真的消失；
   Instance 复用保证不消耗新的 256 槽（池索引单调递增，反复改会耗尽）。
5. **variant 在全部 Step 写成功后才推进**：避免"版本涨了但内容没改完"。
6. **save 后重启沿用 ConfigManager 语义**：由 SystemCommand 统一执行安全重启
   （等 Critical 归零 → 10s 安全窗口）。无 Dirty 时不请求重启。

---

## 4. 测试方式

见文末「修改报告」中的测试章节；串口批量脚本：

```bash
python .pio/serial_batch.py COM8 .pio/cm_run1.log .pio/cm_tests.txt 2.5
```

`cm_tests.txt` 每行格式：`命令 ||| 期望子串`。
