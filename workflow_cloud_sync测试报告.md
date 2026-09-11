# Workflow 云端同步 —— 测试报告

**任务**：Workflow 云端同步架构完善（需求文档 `workflow修改需求文档.md`）
**固件基线**：commit `56b3d55` + `workflow_pick_object` 保护性修复
**测试日期**：2026-09-12
**测试方式**：脚本化回归（真实设备，走 `command_manager_execute()` 完整链路）

---

## 1. 测试环境

| 项目 | 值 |
|---|---|
| 芯片 | ESP32-S3 N16R8 |
| PSRAM | 8386007 B（约 8 MB），已启用 |
| 端口 | COM8，921600 baud |
| 栈 | loopTask 16384 B（`ARDUINO_LOOP_STACK_SIZE`） |
| 上报链路 | 串口 `cm` 直通 → `command_manager_execute()`（与 MQTT 同一条链路） |

> 需求 §32 明确"不要求 MQTT 实机测试"，但必须走
> `command_manager_execute()` 的**实际完整命令链**，而不是只测底层函数。
> 本报告使用串口 `cm` 入口，它与云协议入口进入的是**同一个**
> `command_manager_execute()`，满足要求。

### 1.1 测试工具

| 文件 | 作用 |
|---|---|
| `test/serial_batch.py` | 批处理执行器：逐条下发、收集日志、按期望子串判定 |
| `test/wf_final_tests.txt` | 主回归集（§33–§38，47 条断言） |
| `test/wf_reboot_final_tests.txt` | 重启持久化验证（§35，9 条断言） |
| `test/wf_sync_tests.txt` | 早期版本主回归集（32 条断言，保留备查） |
| `test/wf_reboot_tests.txt` | 早期版本重启验证（7 条断言，保留备查） |

用法：

```bash
python test/serial_batch.py COM8 <日志文件> test/wf_final_tests.txt 4
```

> 打开串口会复位开发板，逐条手敲命令会与 boot 日志竞争，必须用批处理。

---

## 2. 总体结果

| 用例集 | 断言数 | 通过 | 失败 | 崩溃 |
|---|---|---|---|---|
| `wf_final_tests.txt`（§33–§38） | 47 | **47** | 0 | 0 |
| `wf_reboot_final_tests.txt`（§35） | 9 | **9** | 0 | 0 |
| **合计** | **56** | **56** | **0** | **0** |

崩溃检查（`Guru Meditation` / `Stack canary` / `abort()` / `Stack smashing`）：**0 次**

日志：`.pio/wf_final_run2.log`、`.pio/wf_reboot_final.log`

---

## 3. 核心场景逐项对照（§33）

| 需求条目 | 用例 | 判定 | 证据 |
|---|---|---|---|
| §33 基础 `sync_info` | a1 / a2 | ✅ | `"command":"workflow.sync_info"`、`registry_checksum` 命中 |
| §33 基础 `list`（含 valid） | a3 | ✅ | `"valid":true` |
| §33 基础 `get`（含 valid） | a4 | ✅ | `"valid":true` |
| §33 创建 → list 立即可见 | b1 / b2 | ✅ | `create` 返回 `"variant":1`；`list` 立刻含 `"id":"cm_a","variant":1,"valid":true` |
| §33 创建 → get 可读 | b3 | ✅ | `get` 返回 steps 内容（含 `timer`） |
| §33 修改 → variant++ | c1 | ✅ | `"variant":2` |
| §33 无变化 set 不增 variant | c2 | ✅ | `"changed":false` 且 `variant` 仍为 2 |
| §33 修改后 get 校验 | c3 / c4 | ✅ | `"A2"`、`"variant":2` |
| §33 删除 → variant++ + valid=false | f1 / f2 | ✅ | `list` 返回 `"id":"cm_g","variant":2,"valid":false` |
| §33 多对象 save | g1–g5 | ✅ | 见 §4 |

### 3.1 §13 整体替换语义（补充验证）

| 用例 | 判定 | 证据 |
|---|---|---|
| d1：2 个 step（OPEN + CLOSE） | ✅ | `"variant":3` |
| d2：确认 CLOSE 在 | ✅ | `"VALVE_CLOSE"` |
| d3：改为只留 1 个 step | ✅ | `"variant":4` |
| d4：**尾部 step 真的消失** | ✅ | `"steps":[{"type":"action","id":"VALVE_OPEN"}]` —— 序列化后仅 1 个 step |

### 3.2 §11 JSON 兼容 + 参数类型往返（补充验证）

| 用例 | 判定 | 证据 |
|---|---|---|
| e1：float 参数 `gram:12.5` + int 参数 `length:512` | ✅ | `"variant":1` |
| e2：float 往返 | ✅ | `"gram":12.5` |
| e3：int 往返 | ✅ | `"length":512` |

### 3.3 §28 错误码（补充验证）

| 用例 | 期望 | 判定 |
|---|---|---|
| k1：`get` 不存在的 `stable_id:99` | `5` | ✅ |
| k2：`get` 不存在的 `id` | `5` | ✅ |
| k3：`workflow.foo` 未知命令 | `1` | ✅ |
| k4：`create` 空 payload | `6` | ✅ |
| k5：`set` 只给 `id`（缺 `workflow`） | `6` | ✅ |
| k6：`delete` 无任何定位字段 | `3` | ✅ |

---

## 4. §15 / §34 save 全 Dirty 语义（本次重点）

### 4.1 §33 Save：多对象一次落盘

命令序列：修改 `cm_a`(slot 3) + 修改 `cm_h`(slot 5) → `workflow.save {"restart":false}`

```text
[Workflow] save transaction: dirty=[3 5 ] critical_held=1
[Workflow] save wf=3 id=cm_a -> OK
[Workflow] save wf=5 id=cm_h -> OK
→ {"saved":true,"dirty":false,"restarting":false}
```

✅ 一次事务处理**全部** Dirty 对象；全部成功后 dirty 清零；
`restart:false` 时不重启。

### 4.2 §16.1 Dirty=false 不得空转重启

```text
cm workflow.save {"restart":true}   （此时 dirty=false）
→ {"saved":true,"dirty":false,"restarting":false}
```

✅ 无 Dirty 时不请求重启。

### 4.3 §34 Save 部分失败 → Dirty 必须保留

故障注入 `wfst failwf 5`（让 slot 5 的整次 save 失败），然后：

```text
[Workflow] save transaction: dirty=[3 5 ] critical_held=1
[Workflow] save wf=3 id=cm_a -> OK
[WFStg]    TEST fail_wf injected: wf=5
[Workflow] save wf=5 id=cm_h -> WRITE_FAILED
→ 错误码 10，message = "workflow save failed (dirty kept for retry)"
```

随后：

```text
workflow.list          → "dirty":true          ← Dirty 被完整保留
workflow.save（重试）  → "saved":true, "dirty":false
```

✅ **部分成功未清空全部 Dirty**；重试能够完成剩余保存。
✅ 失败时 `Critical` 未释放（`critical_held=1` 在重试日志中仍可见）。

---

## 5. §35 重启持久化

前一轮结束状态：`cm_a`(slot 3, `name=A4`, `variant=6`, 有效已落盘)、
`cm_g`/`cm_h`/`cm_z` 已 delete 并落盘。重启后：

```text
[WF][DBG] load wf=0 id=daily_valve_test    steps=4 variant=1
[WF][DBG] load wf=1 id=daily_valve_test1   steps=4 variant=1
[WF][DBG] load wf=2 id=queue_test          steps=4 variant=1
[WF][DBG] load wf=3 id=cm_a                steps=1 variant=6     ← variant 未回退
```

```json
{"count":4,"dirty":false,
 "workflows":[{"stable_id":0,"id":"cm_a","variant":6,"valid":true},
              {"stable_id":1,"id":"daily_valve_test","variant":1,"valid":true},
              {"stable_id":2,"id":"daily_valve_test1","variant":1,"valid":true},
              {"stable_id":3,"id":"queue_test","variant":1,"valid":true}]}
```

| 验证项 | 判定 | 证据 |
|---|---|---|
| `variant` 跨重启不回退 | ✅ | `variant=6` 保持不变 |
| 已删对象不得复活 | ✅ | `cm_g`/`cm_h`/`cm_z` 查询均返回错误码 `5` |
| 删除后不残留 Dirty | ✅ | `"dirty":false` |

> 附带验证了 §7.2 的 Stable ID 行为：`cm_a` 成为 `stable_id 0` 后，
> 原有三个对象的 `stable_id` 整体 +1 平移。这与接口文档描述一致。

---

## 6. §36 Capability Registry

| 验证项 | 判定 | 证据 |
|---|---|---|
| `object_version` 随 variant 变化 | ✅ | 每次 create / set / delete 后 `WORKFLOW rebuild version=NN` 递增 |
| Workflow 内容变化 → registry 版本变化 | ✅ | `rebuild version=34…39` 连续递增，checksum 同步变化 |
| 无变化时不误增 | ✅ | 幂等 `set` 后无 `rebuild`；重启后 `reuse version=65`（版本稳定） |
| Registry 与 Variant 不混淆 | ✅ | `registry_version`（整体映射）与单个 `variant` 在返回中分列 |

---

## 7. §37 PSRAM / 栈安全

```text
[Workflow]    pools: trigger=PSRAM(114688 B) action=PSRAM(108544 B)
                      step_def=PSRAM(105472 B) | PSRAM total=8386007 B
[CapRegistry] sync buf 2328 B in PSRAM (hwm=9876)
```

| 验证项 | 判定 | 说明 |
|---|---|---|
| 三大实例池命中 PSRAM | ✅ | trigger / action / step_def 均为 PSRAM |
| Registry 临时缓冲命中 PSRAM | ✅ | 3 次分配全部 `in PSRAM` |
| `WorkflowDefinition`（8.6KB）不在栈上 | ✅ | 由 `workflow_storage_alloc_definition()` PSRAM 优先分配 |
| 栈余量 | ✅ | hwm 9876 / 16384（约 6.5KB 余量），全程 0 崩溃 |
| DRAM fallback 存在 | ✅ | PSRAM 分配失败时回落 `MALLOC_CAP_8BIT` |

---

## 8. 本次测试中发现并修复的问题

### 8.1 `workflow.set` / `create` 只给定位字段 → 静默清空目标对象

**严重度**：高（数据破坏）

**现象**：`workflow_pick_object()` 原有逻辑为

```cpp
JsonVariant wf = pl["workflow"];
if (!wf.isNull() && wf.is<JsonObject>()) { out = wf; return true; }
if (!pl["id"].isNull()) { out = pl.as<JsonObject>(); return true; }   // ← 危险
```

当云端只发送定位信息 `{"id":"WF1"}`（没有 `workflow` 对象）时，
`pl` 本身会被当作"一个只有 id、没有 steps 的完整 Workflow"，
走整体替换后把 `WF1` 的 steps **全部清空**。

**修复**：内容必须嵌套在 `p.workflow` 下；平铺形态一律拒绝（错误码 `6`）。

**验证**：`k5` 用例 —— `set` 只给 `id` → 返回 `error_code 6`，
且 `cm_a` 未被破坏（后续 `save wf=3 id=cm_a -> OK`）。

**文档同步**：`workflow_cloud_interface.md` §9.4 / §9.5 已更新形态约定。

### 8.2 测试断言自身的两处笔误

`"save transaction: ...` 与 `"WORKFLOW rebuild version=` 多写了一个引号，
而日志行本身不含引号 → 首轮 2 个 MISS。修正后 47/47。
（非固件缺陷。）

---

## 9. 未验证的边界条件

| 项 | 原因 |
|---|---|
| 16 Workflow × 16 Step 满载 | 需构造 16 条 Workflow，未做；`step` 超 16 的**截断**路径已由代码审阅确认，未实测 |
| 参数个数超 8（截断） | 同上 |
| `workflow.create` 返回 `12 NO_FREE_SLOT` | 需先填满 16 个 Slot；`workflow_find_free_slot` 第二轮（回收已删 Slot）**代码路径已实现但未在满载下实测** |
| RUNNING 中 `set` 被拒（错误码 13） | 需构造一条运行中的长时 Workflow 并发下发；未做 |
| `variant` uint32 溢出回绕 | 需 2³² 次修改；仅代码审阅（溢出回绕到 1，不归 0） |
| MQTT 端到端（CloudManager → Cloud） | 需求 §32 明确不要求；串口 `cm` 已覆盖同一 `command_manager_execute()` 链路 |
| 掉电中断事务恢复 | 依赖已有的 `wfst abort1/abort2` 故障注入，属上一阶段范围，本次未复跑 |

---

## 10. 复现步骤

```bash
# 1. 编译（沙箱内增量编译会被 safe-delete 拦截，必须换 build 目录全量编译）
PLATFORMIO_BUILD_DIR=.pio/build/regr pio run

# 2. 烧录
python .platformio/packages/tool-esptoolpy/esptool.py --chip esp32s3 \
    --port COM8 --baud 921600 write_flash -z 0x10000 \
    .pio/build/regr/esp32-s3-devkitc-1/firmware.bin

# 3. 前置基线：设备应有 3 个有效 Workflow（slot 0/1/2，variant=1）
python test/serial_batch.py COM8 .pio/base.log test/wf_reboot_tests.txt 5

# 4. 主回归
python test/serial_batch.py COM8 .pio/final.log test/wf_final_tests.txt 4
grep -c ': OK' .pio/final.log      # 期望 47

# 5. 重启持久化（复位后执行）
python test/serial_batch.py COM8 .pio/reboot.log test/wf_reboot_final_tests.txt 5
grep -c ': OK' .pio/reboot.log     # 期望 9
```

**注意**：主回归集不是幂等的（会创建固定 id 的临时 Workflow）。
复跑前必须恢复到"3 个有效 Workflow"基线，否则断言中的
Slot 编号（如 `dirty=[3 5 ]`）会失配。

---

## 11. 结论

- §33–§37 全部核心场景**实测通过**，56/56 断言命中，0 崩溃。
- §15.1 / §34 的 save 全 Dirty 语义、失败保留 Dirty、重试续存
  —— 三项均有**故障注入下的实测证据**。
- §35 重启持久化（variant 不回退、删除不复活）**实测通过**。
- §37 PSRAM 策略**全部命中**，栈余量约 6.5KB。
- 测试过程中发现并修复 1 处**高严重度**接口缺陷（§8.1）。
- 尚有 6 项边界未实测，已在 §9 显式列出。
