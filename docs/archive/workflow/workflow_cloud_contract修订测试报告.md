# Workflow Cloud Contract 修订 — 测试报告

**固件基线**：`5ea7664`（代码）+ `cfc8dbc`（测试资产）
**硬件**：ESP32-S3 N16R8，串口 COM8
**测试方式**：串口 `cm {...}` 透传（与 MQTT 走同一条 `command_manager_execute()`）
**执行日期**：2026-09-13

---

## 1. 总览

| 用例集 | 断言数 | 结果 |
|---|---|---|
| `test/wf_contract_tests.txt`（Test 1–19） | 60 | **60/60 PASS** |
| `test/wf_contract_reboot_tests.txt`（Test 6 / 20） | 12 | **12/12 PASS** |
| `test/wf_sync_tests.txt`（旧回归，已转 Slot 定位） | 34 | **34/34 PASS** |
| `test/wf_reboot_tests.txt` | 7 | **7/7 PASS** |
| `test/wf_final_tests.txt` | 49 | **49/49 PASS** |
| `test/wf_reboot_final_tests.txt` | 9 | **9/9 PASS** |
| **合计** | **171** | **171/171 PASS，0 崩溃** |

崩溃检查：`grep -cE "Guru Meditation|canary|abort\(\)|Stack smashing"` = **0**（全部日志）。

---

## 2. Test 1–20 逐项结果

### Test 1：重复 `workflow.id` —— **PASS**

```text
cm workflow.create  workflow.id="same"  →  slot=3  variant=1
cm workflow.create  workflow.id="same"  →  slot=4  variant=1
cm workflow.list                        →  count=5
workflow.get slot 3 / 4                →  两者 id 均为 "same"
```

### Test 2：内容完全相同的两个 Workflow —— **PASS**

```text
cm workflow.create（JSON A） → slot=5  variant=1
cm workflow.create（JSON A） → slot=6  variant=1
```
两个 Slot 内容逐字段相同，各自 `variant=1`，互不影响。

### Test 3：`set` 只改 `p.id` 指定的 Slot —— **PASS**

```text
前提：slot3.id = "same"，slot4.id = "same"

workflow.set p.id=3, workflow.id="changed"
  → slot 3: {"id":"changed", ..., "variant":2}
  → slot 4: {"id":"same",    ..., "variant":1}    ← 未被波及
```

### Test 4：`workflow.id` 可改成与他人重复 —— **PASS**

```text
workflow.set p.id=3, workflow.id="same"（与 slot4 同名）
  → slot 3: id="same" ✅
  → slot 4: id="same" ✅
```

### Test 5：`get` `valid=false` 对象（不 reboot）—— **PASS**

```text
workflow.delete p.id=3 → deleted:true, variant=4
workflow.get    p.id=3 →
  {"i":"t5b","o":{"id":"same","name":"same","variant":4,"enable":false,
    "timeout_ms":0,"steps":[]},"k":5,"valid":false,"slot":3,
   "command":"workflow.get"}
```
对象仍可读、内容正确，顶层明确带 `"valid": false`。

### Test 6：reboot 后 `get` 已删除 Slot —— **PASS**

```text
（reboot 后）
workflow.get p.id=3 → {"m":"workflow slot is empty","e":5}
workflow.delete p.id=3 → {"m":"workflow slot is empty","e":5}
```
启动日志确认 slot 3 未加载（`load wf=` 只有 0/1/2/4/5/6/7）。

### Test 7：`delete` 幂等 —— **PASS**

```text
workflow.delete p.id=3（第 1 次）→ deleted:true,  noop:false, variant 3→4
workflow.delete p.id=3（第 2 次）→ deleted:false, noop:true,  variant 仍为 4
workflow.get    p.id=3          → valid:false, variant 仍为 4
workflow.list                   → count 仍为 8（无变化）
```
第二次删除未增 variant、未产生新 Dirty、未触发额外 Registry 变化。

### Test 8：`count` 包含 `valid=false` 对象 —— **PASS**

```text
workflow.sync_info → "count":8
workflow.list      → "count":8   （其中 slot3 为 valid=false）
```

### Test 9：只改内容不改 `id` —— **PASS**

```text
workflow.set p.id=5（改 enable false→true）
  → changed:true, variant 1→2
  → 日志: [WF][DBG] compare wf=5 identical=0
```

### Test 10：改 `workflow.id` —— **PASS**

```text
workflow.set p.id=6, workflow.id="dupfull"→"zz_renamed"
  → changed:true, variant 1→2
```

### Test 11：新增导致 `stable_id` 重排 —— **PASS**

```text
workflow.create workflow.id="aaa_first"（字母序最前）
  → slot=7, stable_id=0
```
日志：`[CMD][WF] create: slot=7 id=aaa_first stable_id=0 variant=1`
—— 它顶到 0，原有对象整体平移，证明 Cloud 不能依赖 stable_id 永久不变。

### Test 12：Action/Trigger Registry 改变 —— **NOT TESTABLE**

**原因**：设备没有运行时注册 / 注销 Action 或 Trigger 的命令接口。
ACTION / TRIGGER 两张 Registry 表在 `capability_registry_init()` 时
由已编译进固件的能力集合决定，运行期恒为 `reuse version=1`
（ACTION count=7 / TRIGGER count=3）。要制造变化必须重新编译固件
（增删能力模块），这超出了本次"上板串口测试"的范围。

**替代验证**：`registry_version` 随 Workflow 变化的行为已在
Test 9/10/11 中验证（`WORKFLOW rebuild version=` 递增）；
且 CHANGE 流程（`sync_info → list`）本身与变化来源无关。

### Test 13：`type` 缺失 —— **PASS**

```text
workflow.set    steps:[{"id":"VALVE_OPEN"}] → e:11 "step.type is required at steps[0]"
workflow.create steps:[{"id":"VALVE_OPEN"}] → e:11 同上
workflow.get    p.id=4                      → 目标内容未被破坏
```
不再默认成 `action`。

### Test 14：`>8 params` —— **PASS**

```text
workflow.set 9 个 params → e:11 "step.params exceed 8 at steps[0]"
```
未截断。

### Test 15：`>16 steps` —— **PASS**

```text
workflow.set 17 个 steps → e:11 "workflow.steps exceed 16 (got more)"
```
未截断。

### Test 16：JSON 字段顺序变化不增 variant —— **PASS**

```text
workflow.set p.id=5（与当前内容语义一致，member 顺序与 params 顺序均不同）
  → changed:false
  → 日志: [WF][DBG] compare wf=5 identical=1 (cur=101 B, new=101 B)
workflow.get p.id=5 → variant 仍为 1
```

### Test 17：save 部分失败 —— **PASS**

```text
workflow.set p.id=5  → dirty
workflow.set p.id=6  → dirty
wfst failwf 6                       ← 注入 slot6 保存失败（一次性）

workflow.save {"restart":false}
  → e:10  "workflow save failed (dirty kept for retry)"

日志:
  [Workflow] save transaction: dirty=[5 6 ] critical_held=1
  [Workflow] save wf=5 id=dupfull    -> OK
  [WFStg]    TEST fail_wf injected: wf=6
  [Workflow] save wf=6 id=zz_renamed -> WRITE_FAILED
  [Workflow] save partial: dirty remain, critical_held=1
  [Workflow] save transaction: FAILED (dirty kept for retry)

workflow.list → dirty:true

（去掉注入后再 save）
  [Workflow] save transaction: dirty=[6 ] critical_held=1   ← 只剩 6
  [Workflow] save wf=6 id=zz_renamed -> OK
workflow.list → dirty:false
```

**关键证据**：第二轮 save 的 dirty 清单只剩 `[6 ]`，证明 slot5 在
**上一轮失败**的 save 中已经清掉了自己的 Dirty —— 正是 §13 要求的
"成功对象清自己、失败对象保留"语义。

### Test 18：save 无 Dirty —— **PASS**

```text
workflow.save {"restart":false} → saved:true
workflow.save {"restart":false} → restarting:false
```
无 Dirty 时不空转重启。

### Test 19：save + restart —— **PASS**

```text
workflow.set  p.id=4             → changed:true
workflow.list                    → dirty:true
workflow.save（restart 缺省 true）
  → {"saved":true,"dirty":false,"restarting":true,"registry_version":77}
```
随后设备断开并 reboot（下一轮测试的启动日志已确认）。

### Test 20：reboot 后完整同步 —— **PASS**

```text
workflow.list      → count:7（slot3 已消失）
workflow.sync_info → count:7, dirty:false
workflow.get p.id=7 → id="aaa_first"
workflow.get p.id=5 → id="dupfull", variant=3   ← 跨重启保持
workflow.get p.id=6 → id="zz_renamed"
workflow.get p.id=4 → id="same"
```

---

## 3. 未测试项 / 未验证边界

| 项 | 状态 | 原因 |
|---|---|---|
| Test 12 Action/Trigger Registry 变化 | **NOT TESTABLE** | 无运行时增删能力的命令接口，需重新编译固件 |
| 16 Workflow × 16 Step 满载 | 未测 | 需构造 16 条满配 Workflow，耗时大；`NO_FREE_SLOT` 路径（填满 16 槽）同样未覆盖 |
| RUNNING 中 `set` 被拒（错误码 13） | 未测 | 需要一条能被长时间触发的 Workflow；现有测试 Workflow 均未真正运行 |
| `variant` uint32 溢出回绕 | 未测 | 需要 2^32 次修改，不可行；代码层有保护（`0` 回退为 `1`） |
| MQTT 端到端 | 未测 | 需求 §29 明确不要求；串口 `cm` 已覆盖同一 `command_manager_execute()` 链路 |
| 掉电事务恢复 | 未测 | 需要真实掉电时序；`wfst abort1/abort2` 仅模拟 |

---

## 4. 与需求文字的一处已知差异

| 项 | 需求文字 | 实际实现 | 处理 |
|---|---|---|---|
| §8.1 delete 后 Dirty | "进入 Dirty 状态" | `workflow_storage_delete()` 只置 `meta.valid=0` 并**立即写盘**，随后清掉本对象 Dirty | **保持现状**。若保留 Dirty，后续 `save_transaction()` 会把对象重新写回并置 `valid=true` → 删除被撤销（源码注释已说明）。改动会破坏已验证的 Dirty/Critical 配对，违反 §31-6/7。Test 6 仍能通过（save 为 no-op 后 reboot） |

---

## 5. 回归记录

旧用例集（47+32 断言）在本次契约变更前使用 `p.id = workflow.id` 定位，
新契约下全部失效，已统一转换为 Slot 定位并**重新上板验证通过**
（34 / 7 / 49 / 9，见 §1）。
