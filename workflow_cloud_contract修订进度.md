# Workflow Cloud Interface 修订与代码一致性整改 — 进度台账

> 中断后**先读本文件**再动手。每完成一个阶段即 commit，commit 后回来更新本表。
> 需求来源：`workflow修改需求文档.md`（新版，41 节 Cloud Contract 修订）。

## 当前阶段

**Phase F — 接口文档 + README + 测试报告**（收尾中）
全部代码改动已提交并上板验证；剩余为文档提交与最终交付说明。

## 阶段总表

| 阶段 | 内容 | 状态 | Commit |
|---|---|---|---|
| A | `p.id` = 整数 Slot 定位；`workflow.id` 允许重复；list 按 Slot 输出 | ✅ 完成 | `5ea7664` |
| B | `delete` 幂等（valid=false → no-op）+ `get valid=false` 语义 | ✅ 完成 | `5ea7664` |
| C | `type` 必填 + `params>8` / `steps>16` reject（禁止静默截断） | ✅ 完成 | `5ea7664` |
| D | `save` per-workflow Dirty（成功者各自清，失败者保留） | ✅ 完成 | `5ea7664` |
| E | 编译 + 烧录 + 上板 Test 1–20 | ✅ 完成 | `cfc8dbc`（测试资产） |
| F | 接口文档 + README + 测试报告 | 🚧 收尾 | 待提交 |

## 基线信息

- 板子：ESP32-S3 N16R8，串口 **COM8**，波特率 921600 烧录
- 起始 commit：`b993c0b`
- 基线状态：3 条 Workflow（slot 0/1/2），`dirty=false`
- 串口透传：`cm {"cmd":"workflow.xxx","ob":"-","id":"<唯一>","p":{...}}`
  （走的是同一条 `command_manager_execute()`，与 MQTT 等价）

## 工作区保护（§7 / §31）

以下文件是**用户已有修改**，本次全程不得触碰 / 提交：

- `workflow修改需求文档.md`（M）
- `workflow架构适配commandmanager及version-修改报告.md`（M）
- `workflow代码分析0910.md`（??）
- `workflow架构设计补充0910.md`（??）
- `workflow架构适配commandmanager及version需求.md`（??）

禁止 `git add .`；只 `git add <明确文件>`。

## 代码改动清单（已提交 `5ea7664`）

### src/workflow.h

| 新增 | 说明 |
|---|---|
| `workflow_slot_occupied(uint8_t)` | Slot 是否被占用（id 非空，无论 valid） |
| `workflow_get_occupied_count()` | 占用 Slot 数（含 valid=false）—— list/sync_info 的 `count` |
| `workflow_validate_workflow_json(obj, err)` | 严格校验：id 非空 / type 必填 / steps≤16 / params≤8 |

`workflow_find_index_by_id()` 注释改为「仅供内部，不得用于云端定位」。

### src/workflow.cpp

1. 新增 `workflow_slot_occupied()` / `workflow_get_occupied_count()`
2. 新增 `workflow_validate_workflow_json()`（严格 Cloud Contract）
3. `workflow_apply_workflow_json()`：
   - `type` 缺失 / 非法 → `return false`（不再 `| "action"`）
   - `step_index >= 16` → `return false`（不再 `break` 截断）
   - `param_count >= 8` → `return false`（不再 `break` 截断）
4. `workflow_save_transaction()`：
   - 成功的 Workflow **立即清自己的 Dirty**
   - 失败的保留 Dirty 且 **循环不中断**（旧实现 `break`）
   - 仅当 `!dirty_any()` 时清窗口 + release Critical

### src/command_manager.cpp

1. `workflow_resolve_index()` → **`workflow_resolve_slot()`**
   - `p.id` 必须是整数（全数字字符串兼容）；非整数 → error 6
   - 越界 / Slot 未占用 → error 5
   - 删除了 `workflow.id` 搜索与 `cmd.object` 定位
   - `stable_id` 保留，最终解析回 Slot
2. `command_workflow_list()`：按 Slot 遍历，输出 `slot/id/variant/valid/stable_id`；
   `count` = `workflow_get_occupied_count()`
3. `command_workflow_sync_info()`：`count` 同上
4. `command_workflow_get()`：Slot 定位；valid=false 仍返回（顶层带 `valid`）；
   新增返回 `slot`
5. `command_workflow_create()`：先 `workflow_validate_workflow_json()`（分配 Slot 之前）
6. `command_workflow_set()`：先校验（幂等比较之前）→ Slot 定位 → 幂等比较 → apply
7. `command_workflow_delete()`：Slot 定位 → `!workflow_is_valid()` 时 no-op
   （`deleted:false, noop:true`）

## 上板测试结果（COM8，2026-09-13）

| 用例集 | 断言 | 结果 |
|---|---|---|
| `test/wf_contract_tests.txt`（Test 1–19） | 60 | **60/60 PASS** |
| `test/wf_contract_reboot_tests.txt`（Test 6 / 20） | 12 | **12/12 PASS** |
| `test/wf_sync_tests.txt` | 34 | **34/34 PASS** |
| `test/wf_reboot_tests.txt` | 7 | **7/7 PASS** |
| `test/wf_final_tests.txt` | 49 | **49/49 PASS** |
| `test/wf_reboot_final_tests.txt` | 9 | **9/9 PASS** |
| **合计** | **171** | **171/171，0 崩溃** |

详细逐项结果见 `workflow_cloud_contract修订测试报告.md`。

**NOT TESTABLE**：Test 12（Action/Trigger Registry 变化 —— 无运行时
增删能力的命令接口，需重新编译固件）。
未覆盖边界：16×16 满载 / `NO_FREE_SLOT` / RUNNING 中 set 被拒 /
uint32 溢出 / MQTT 端到端 / 掉电事务恢复。

## 关键设计决策（与需求文字的差异，需向用户确认）

| 项 | 需求文字 | 实际行为 | 处理 |
|---|---|---|---|
| §8.1 delete 后 Dirty | "进入 Dirty 状态" | `workflow_storage_delete()` 只把 meta.valid 置 0 并**立即写盘**，随后清 Dirty | **保持现状**。理由：注释明确"若留 Dirty，后续 save 会把 valid 置回 true → 删除被撤销"；改 RAM-only 会破坏已验证的 Dirty/Critical 配对（违反 §31-6/7）。Test 6 仍可 PASS（save 是 no-op 后 reboot）。**需在报告中显式标注** |
| §18 错误码 | "error 6 / INVALID_PAYLOAD" | 统一用 `CMD_ERROR_INVALID_PAYLOAD = 11`（名字即 INVALID_PAYLOAD） | 文档写明编号 11 |
| §21 `k` 字段 | 示例里 `k:3` 与 `p.id` 同值 | CommandManager 明文格式下用 `slot`；`k` 是 CloudManager 紧凑键（= stable_id） | 文档区分明文/紧凑两套 |

## 测试资产

- 批处理：`test/serial_batch.py`（参数：`COM口 输出日志 用例文件 [每条等待秒数]`）
- 用例格式：`cm {...} ||| 期望子串`；脚本输出 `EXPECT '...' : OK/MISS`
- 本次新增：`test/wf_contract_tests.txt`（Test 1–20）
- 打开串口会复位板子 → **必须批量跑**，单条命令会与 boot 日志竞争

## 常用命令

```bash
# 编译（沙箱 safe-delete 拦截增量编译，必须用全新 build dir）
BD=".pio/build/xx$(date +%s)"; echo "$BD" > .pio/last_bd.txt
PLATFORMIO_BUILD_DIR="$BD" pio run

# 烧录（pio run -t upload 在本环境只编译不烧录）
"C:/Users/wang/.workbuddy/binaries/python/envs/default/Scripts/python.exe" \
  "C:/Users/wang/.platformio/packages/tool-esptoolpy/esptool.py" \
  --chip esp32s3 --port COM8 --baud 921600 write_flash -z 0x10000 \
  "$(cat .pio/last_bd.txt)/esp32-s3-devkitc-1/firmware.bin"

# 回归
"C:/Users/wang/.workbuddy/binaries/python/envs/default/Scripts/python.exe" \
  .pio/serial_batch.py COM8 <输出日志> <用例文件> 4
```
