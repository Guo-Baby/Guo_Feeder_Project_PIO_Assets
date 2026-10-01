# LogManager P1.5 收尾修复报告

> 项目：Guo Feeder Project（ESP32-S3 N16R8）
> 日期：2026-09-17
> 基线：commit `5816b32`
> 范围：审查报告确认的风险项（H2 / D1 / R1 / R2）
> **状态：已改、已编译、已跑契约测试；按用户要求「先审 diff」⇒ 未 commit**

---

## 1. 修改文件列表

| 文件 | 增/删 | 内容 |
|---|---|---|
| `src/log_ack.h` | **+110 / -13** | ① R1：`log_ack_segment_deletable` → **`_legacy`**，加加粗 WARNING 弃用横幅；② 新增 **`log_hole_protect_gap()`**（FIX-H2 判定，freestanding） |
| `src/log_manager.cpp` | **+87 / -22** | ① H2：ACK 落账补齐空洞（调用纯函数）；② D1：`s_replay_after_seq/valid` 约束 + 三处接入；③ R2：`s_replay_seq` → `s_replay_diag_seq`（3 处）；④ 陈旧注释修正 2 处 |
| `src/log_manager.h` | **+3 / -0** | `LogStats.cloud_hole_from_evict`（观测计数） |
| `src/main.cpp` | **+7 / -5** | `cstats2` 增加 `hole_evict=` |
| `test/log_contract/probe_ack.cpp` | **+114 / -8** | **22 项 H2 断言** + 8 处 R1 改名同步 |
| `test/log_fix_tests.txt` | **+430 / -88** | 新增 **F7（H2）** 与 **F8（D1）** 两段；断言 132 → **185** |

**未触碰（按要求）**：`src/log_events.h`、`src/cloud_manager.cpp`、`src/cloud_manager.h`、`platformio.ini`、`partitions.csv`、`data/`。
（`git diff --stat` 对以上均为空，已逐项校验。）

---

## 2. 每个修改的原因

### 2.1 H2 —— FIX-1 × FIX-2 交界缺口（必须修复）

**问题**：`cloud_queue_push()` 在 **in-flight 窗口内**淘汰记录时**不**登记空洞。
这个设计本身是**对的**（那些记录仍在批次副本内、仍可能被本次 ACK 覆盖；提前登记会留下永不复位的陈旧空洞、永久阻塞段回收）。但若本次 ACK **只覆盖前缀 `covered`**，而 `evicted > covered`，则下标区间 `[covered, evicted)` 的记录同时具备三个坏性质：

```
· 已离开 RAM 云队列（rd 已越过）  ⇒ 本 Boot 不会再发送
· 未被本次 ACK 覆盖              ⇒ acked_seq 不会为它们停留
· 也没有任何空洞                 ⇒ 段回收对它们不设防
```

后续批次把水位推过它们之后，其物理副本就可能被回收 ⇒ **重启也补不回**。

**修法**（严格只补这一小段）：

```
下标边界（与 push 的非在途分支互斥，不会重复登记）：
  [0, covered)         已被 ACK                       ⇒ 无需保护
  [covered, evicted)   被淘汰且未确认                  ⇒ ★本次补齐（新代码）
  [evicted, tx_count)  由 push 的 else 分支已登记空洞  ⇒ 跳过
```

在 `cloud_handle_ack()` 落账阶段调用新函数 `log_hole_protect_gap()`：

```c
if (evicted > (uint32_t)covered)
{
    bool gap_overflow = false;
    const uint8_t gap_protected = log_hole_protect_gap(
        s_cloud_batch, tx_count, covered, evicted,
        s_cloud_holes, &s_cloud_hole_count, &gap_overflow);

    s_stats.cloud_hole_from_evict += gap_protected;
    if (gap_overflow) { s_cloud_hole_overflow = true; }   // 表满 ⇒ 停回收（保守）
}
```

**为什么把判定抽到 `log_ack.h` 而不是内联**：沿用项目既有的「判定与执行分离」范式
（`log_ack_classify` / `log_ack_covered_count` / `log_ack_segment_reclaimable` 都是这么做的）。
收益是**这个簿记缺陷现在能在没有开发板时被穷举** —— 本项目的 0916 审计早已指出：
"两个 CRITICAL 都出在判定与执行之间的簿记，纯逻辑探针覆盖不到"。把 H2 的簿记也放进纯函数后，
它就有了 22 项主机端断言。

**只保护 Flash-routed**：INFO / DEBUG 不落 Flash，没有物理副本可保护，登记空洞纯属浪费表项。

**刻意不校验 `boot_seq`**（与 `push` 的写法不同）：补发批次里含**上一 Boot** 写下的记录，
它们的物理副本同样需要保护。（`push` 的 `evict_boot == s_boot_seq` 守卫存在不对称，见 §6 后续项。）

### 2.2 D1 —— sweep 中删除"游标所在段"（必须修复）

**原行为**：`flash_drop_segment()` 中

```c
if (s_replay_seg == (uint8_t)seg) { s_replay_seg = 0xFF; s_replay_idx = 0; ... }
```

`0xFF` 的语义是"约束清零" ⇒ 下一轮 `flash_next_segment_after(0, false)` = **最老段** ⇒
已扫描区域被重复遍历。

**修法**（用户选项 A）：在**清空段元信息之前**记下 `first_seq`，作为"从其后继续"的约束。

```c
static bool flash_drop_segment(uint32_t seg)
{
    const uint32_t dropped_first_seq = s_seg_first_seq[seg];   // ★ 必须在清零前取
    const bool     dropped_present   = (s_seg_present[seg] != 0);
    ...
    if (s_replay_seg == (uint8_t)seg)
    {
        s_replay_after_seq   = dropped_first_seq;
        s_replay_after_valid = (dropped_present && dropped_first_seq != 0u);
        s_replay_seg = 0xFF; s_replay_idx = 0; s_replay_armed = true; s_replay_done = false;
    }
}
```

`cloud_replay_step()` 的起始段选取改为：

```c
s_replay_seg = flash_next_segment_after(s_replay_after_seq, s_replay_after_valid);
s_replay_after_valid = false;   // 约束只作用于"本轮起始段选择"，消费一次即失效
```

`cloud_replay_arm()` 显式清零约束（全新一轮 sweep 不该携带陈旧值）。

**正确性论证**：
- **不丢记录**：被删段里未 push 的记录随文件一起消失，但那属于"段被删除"的既有语义
  （删除只发生在"整段已确认"或"环压强制淘汰"两种情形，后者已计入 `drop_unacked`），
  D1 不改变这一点。
- **不无限重复**：游标严格单调前进（`after_seq` 单调递增，且正常推进路径本就是升序选取）。
- **云端幂等仍保留**：at-least-once 语义与 `(device_id, boot_seq, seq)` 去重键完全未动。

> ⚠️ **诚实结论**：D1 的行为差异**在板上不可观测**。因为游标只在 `used == 0` 时前进
> ⇒ 已扫描记录必已被 ACK/放弃 ⇒ 即便回到最老段重扫，也会被
> `log_ack_should_replay()`（`seq <= acked_seq`）跳过。故 D1 属**预防性**修改：
> 消除无谓重扫，并防止将来不变量变化后退化为重复投递。**F8 验证的是"删除发生在 sweep 中"
> 这条路径本身不丢、不空转、不死循环**；新旧差异由代码审查覆盖。已在用例文件内注明，
> 不伪装成"已由测试证明有差异"。

### 2.3 R1 —— 易误用的 legacy 函数（低优先级，顺手）

`log_ack_segment_deletable()`（按 `first_seq + records - 1` **稠密推导**末条 seq）与
`log_ack_segment_reclaimable()`（接收**真实**末条 seq + 空洞感知）名字高度相近，
而前者是**错误**的。已改名并要求测试同步：

```c
// ****************************************************************
// WARNING:
//   deprecated.
//   Do NOT use for production reclaim.
//   It relies on dense seq assumption.
// ****************************************************************
static inline bool log_ack_segment_deletable_legacy(...)
```

同步：`test/log_contract/probe_ack.cpp` 8 处引用；`log_manager.cpp` 1 处旧注释。

### 2.4 R2 —— 诊断字段改名（低优先级，顺手）

`s_replay_seq` 实为**诊断**字段（最后考察到的 record seq），名字却像游标（真正游标是 `seg/idx`）。
改名 `s_replay_diag_seq`（3 处）。公开观测结构字段 `LogCloudInfo::replay_seq` **保留原名**
（`logt cloud2` 的 `replay_seq=` 输出未变，避免影响既有断言与工具）。

---

## 3. RAM / Flash 变化

| 项 | 修复前 | 修复后 | 变化 |
|---|---|---|---|
| **静态 RAM** | 130192 B（39.7%） | **130200 B（39.7%）** | **+8 B** |
| **Flash** | 1363633 B（65.0%） | **1363633 B（65.0%）** | **±0 B** |
| PSRAM | — | — | **±0**（新增状态全在静态 DRAM） |
| 新增堆分配 | — | — | **无** |
| 新增局部大数组 | — | — | **无**（最大仍是 `LogRecord rec` 128 B） |

**+8 B 明细**：

| 符号 | 类型 | 字节 | 用途 |
|---|---|---|---|
| `s_replay_after_seq` | `uint32_t` | 4 | D1 起始段约束 |
| `s_replay_after_valid` | `bool` | 1 | D1 约束是否有效 |
| `LogStats::cloud_hole_from_evict` | `uint32_t` | 4 | H2 观测（补保护的记录条数） |
| 对齐填充 | — | −1 | 结构体对齐吸收 |

**Flash 无变化**：H2 只新增一个函数调用（既有分支内），D1 只改起始段选取表达式，
未引入新常量或新表。**未新增任何持久化状态** —— `s_replay_after_*` 是纯 RAM 约束，
重启后由 `cloud_replay_arm()` 归零（`log_init()` 调用），**Flash 格式与 `meta.bin` 完全未动**。

---

## 4. 测试结果

### 4.1 编译

```
PLATFORMIO_BUILD_DIR=.pio/build/final2... timeout 185 pio run
RAM:   [====      ]  39.7% (used 130200 bytes from 327680 bytes)
Flash: [=======   ]  65.0% (used 1363633 bytes from 2097152 bytes)
======================== [SUCCESS] Took 106.50 seconds ========================
```

### 4.2 契约测试（离线，无需硬件）

```
python test/log_contract/run_contract_test.py

[1/4 positive] PASS : 编译通过（期望通过）
[2/4 negative] PASS : 编译失败且命中负向探针断言（期望失败）
[3/4 cbor    ] PASS : 21 B 头逐字节匹配 + records 130 B/条 + 越界/上界保护
[4/4 ack     ] PASS : IGNORE/DUPLICATE/ACCEPT/PARTIAL 分类 + 覆盖前缀 + 段可删性 + 退避 + 放弃门槛
     合计失败: 0
CONTRACT TEST: ALL PASS
```

ACK 探针断言数：**83 → 105**（新增 H2 的 22 项）。

### 4.3 新增 Case H2（主机端真实执行 —— wasm32 + node）

```
淘汰间隙补齐空洞（FIX-H2）:
[OK] covered=4 evicted=12 -> 4 Flash-routed protected
[OK] 4 separate holes (104/106/108/110 not adjacent)
[OK] no overflow
[OK] seq 104 protected / 106 protected / 108 protected / 110 protected
[OK] seq 105 (INFO) NOT protected
[OK] seq 100 (covered) NOT protected / seq 102 (covered) NOT protected
[OK] segment [100,111] fully acked BUT overlaps gap hole -> KEEP   ← ★保护生效
[OK] evicted == covered -> gap is empty
[OK] no hole added
[OK] evicted < covered -> gap is empty
[OK] covered=0 evicted=12 -> 6 Flash-routed protected
[OK] seq 100 protected when covered=0
[OK] evicted > tx_count -> clamped to tx_count
[OK] index 8 (>= tx_count) NOT handled here
[OK] null batch -> 0 / null holes -> 0
[OK] full table -> 0 protected
[OK] full table -> overflow flag set
```

覆盖了：**边界等于/小于/大于**、`covered=0`、`evicted > tx_count` 钳位、
空指针、满表 overflow，以及**"含间隙空洞的段即使整段被 ACK 也不得回收"**这一效果断言。

### 4.4 新增设备侧用例（F7 / F8）—— **未执行**

| 段 | 关键断言 | 说明 |
|---|---|---|
| **F7（H2）** | `cstats2 hole_evict=8`（**修复前恒为 0**）+ `cloud3 holes=1`；随后 `cstats segdel=0` / `fstats seg_del=0` | 精确构造：`fill warn 20 / 62 / 58` ⇒ 队列 140 条 ⇒ **12 次在途淘汰**；`ackauto 1 4` 造 partial ⇒ 补齐 8 条 WARN（合并成 1 个空洞表项）；再把水位推过 seg0 ⇒ seg0 因空洞**不得**回收（`segdel=0`；无修复则为 1） |
| **F8（D1）** | `fcorrupt 0` → `rarmed=1` → `ackauto 0` → `cloud3 rseg=1` / `ackst replay=25` → `rarmed=0` | `creset + sonline 0` 让 replay 推 16 条后暂停（游标 = seg0@16），删掉 seg0，再上线排空 ⇒ 补发**从 seg0 之后**继续、命中 seg1、正常收尾 |

> ⚠️ **F7 / F8 均为代码推导值，尚未在硬件上执行**（开发板未连接，`list_ports` 无 USB-Serial）。
> 上板若某条不符，请先看设备实际输出再判定是断言错还是代码错。

---

## 5. `git diff` summary

```
 .workbuddy/memory/2026-09-16.md |  62 ++++++         （记忆，非本次代码）
 .workbuddy/memory/MEMORY.md     |  73 +++----        （记忆，非本次代码）
 AI_RULES.md                     |   7 +             （用户自己的修改，未触碰）
 src/log_ack.h                   | 110 +++++++++-      ← H2 纯函数 + R1 改名
 src/log_manager.cpp             |  87 +++++++-        ← H2 落账 + D1 约束 + R2 改名
 src/log_manager.h               |   3 +             ← H2 观测字段
 src/main.cpp                    |   7 +-             ← cstats2 hole_evict
 test/log_contract/probe_ack.cpp | 114 ++++++++++-     ← 22 项 H2 断言 + R1 同步
 test/log_fix_tests.txt          | 430 ++++++++++++++++++++++++++++++++++------   ← F7/F8
 9 files changed, 765 insertions(+), 128 deletions(-)
```

**禁止项逐项校验**（均已通过）：

| 要求 | 校验命令 | 结果 |
|---|---|---|
| 不改 `log_events.h` | `git diff --stat -- src/log_events.h` | 空 ✅ |
| 不改 Flash record/segment 格式 | 同上（格式常量全在 `log_events.h`）+ 未动 `flash_serialize_*` | ✅ |
| 不引入新持久化状态 | `s_replay_after_*` 仅 RAM；`meta.bin` / 段头未动 | ✅ |
| 不加 Task | 未新增 `xTaskCreate` | ✅ |
| 不使用 `delay`/`while` 阻塞 | `grep` 新增路径无命中 | ✅ |
| 不改 CloudManager 依赖方向 | `git diff --stat -- src/cloud_manager.*` | 空 ✅ |
| 不改 ACK classify 顺序 | `git diff -U0 -- src/log_ack.h \| grep log_ack_classify` | 无 ✅ |
| 不改既有测试语义 | 仅函数改名同步（8 处）；断言串未改 | ✅ |

---

## 6. 后续项（本次**未改**，按"不重构、不扩范围"）

`cloud_queue_push()` 的 **`evict_boot == s_boot_seq` 守卫与 H2 写法不对称**：

- H2 刻意**不**校验 `boot_seq`（理由见 §2.1：补发批次含上一 Boot 记录，物理副本同样需保护）。
- 但 `push` 的守卫会让 **"上一 Boot 写入 → 本 Boot 被补发 → 又被淘汰（非在途窗口）"** 的记录
  **漏登记空洞**。其后果与 H2 同源：水位推过后段可能被回收 ⇒ 下次开机补不回。
- 由于 `acked_seq` 每次开机归 0，该记录在**再下一次**开机本可重放 —— 唯一丢失路径是
  "本 Boot 的段回收把物理副本删掉"。
- **建议**：把 `push` 的守卫一并去掉（空洞保护的是**物理副本**，与写入时的 Boot 无关）。
  一行改动，但属既有逻辑，故本次未动，列入待批。

其余未修项：**H1**（空洞表 8 项可能耗尽 ⇒ 建议"删除与所有现存段都不相交的空洞"这一无损修剪）。

---

## 7. 交付状态声明

- ✅ 已改代码、已编译（RAM +8 B / Flash ±0）、已跑离线契约测试（4/4 ALL PASS，含 22 项新 H2 断言）
- ⛔ **未 commit**（按要求先审 diff）
- ⛔ **未上板**（开发板未连接）⇒ F7/F8 与"旧行为对照值"均为代码推导
- ⛔ **F5 仍需 MQTT**（`logt ack` 绕过 `cloud_process_rx_message`，串口无法验证 FIX-4 的隔离效果）
