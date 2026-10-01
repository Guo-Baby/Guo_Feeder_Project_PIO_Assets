# LogManager P1.5 收尾审查报告

> 项目：Guo Feeder Project（ESP32-S3 N16R8）
> 日期：2026-09-17
> 代码基线：`5816b32`（已含 FIX-1 / FIX-2 / FIX-3 / FIX-5 + DIR-1 + FIX-4）
> 审查性质：**只读**。未修改任何 `.cpp` / `.h`；未产生任何 commit。
> 唯一写入物：`test/log_fix_tests.txt`（按审查要求生成的测试命令清单，非源码）

---

## 0. 审查方法与证据来源

全部结论来自**逐函数实读**，不依赖设计文档或此前的报告表述。实读清单：

| 文件 | 覆盖到的函数 / 区域 |
|---|---|
| `src/log_manager.cpp` | `flash_scan_all` / `flash_append_batch` / `flash_count_unacked` / `flash_ensure_append_target` / `flash_drop_segment` / `flash_next_segment_after` / `flash_find_max_seq` / `cloud_queue_push` / `cloud_collect_batch` / `cloud_build_cbor` / `cloud_try_send_batch` / `cloud_give_up_batch` / `cloud_handle_ack` / `cloud_delete_acked_segments` / `cloud_replay_arm` / `cloud_replay_step` / `cloud_poll` / `log_task` / `log_init` / `log_cloud_test_reset` |
| `src/log_ack.h` | `log_ack_range_valid` / `log_ack_classify` / `log_ack_covered_count` / `log_ack_segment_deletable`（legacy）/ `log_hole_add` / `log_hole_overlaps` / `log_ack_segment_reclaimable` / `log_ack_backoff_ms[_ex]` / `log_ack_should_give_up` / `log_ack_should_replay` |
| `src/cloud_manager.cpp` | `cloud_process_rx_message`（id 必需性 / 去重缓存 / `log_ack` 分支 / 字段解析）/ `cloud_check_duplicate_cmd` / `cloud_send_log` / `cloud_mqtt_publish_binary` / `cloud_is_connected` / `cloud_set_log_ack_callback` |
| `src/main.cpp` | `logt` 全部子命令与其**精确输出格式串**（用于校准断言） |
| `test/mqtt_log_probe.py` | CLI 子命令与 payload 形态 |
| 构建日志 | `.pio/build_fx1|fx2|fx3.log`（RAM/Flash 实测） |

---

## 1. Flash seq 稀疏模型确认（最高优先级）

### 1.1 结论：**生产路径已完全移除稠密推导**，无残留风险

旧假设 `first_seq + records - 1 == 段内真实末条 seq` 的全部出现点如下（`grep` 全量）：

| 位置 | 内容 | 是否生产路径 | 风险 |
|---|---|---|---|
| `log_manager.cpp:64` | 注释（解释为什么需要 `s_seg_last_seq`） | 否 | — |
| `log_manager.cpp:205` | 注释（解释 CRITICAL-3a） | 否 | — |
| `log_manager.cpp:625` | 注释（"禁止用推导"） | 否 | — |
| `log_manager.cpp:637` | `loff = HEADER + (records-1) * RECORD_SIZE` —— **末槽的文件偏移**，不是 seq | **是** | 无（正确：`records` 是有效条数，末槽下标 = records-1） |
| `log_manager.cpp:1962` `:2177` | 注释 | 否 | — |
| `log_manager.cpp:2890` `:2896` | 同 637，`peek_segment` 里取末槽偏移以读实测 `last_seq` | **是** | 无 |
| `main.cpp:528` | `fseg2` 的 `derived=` **诊断显示**（刻意保留，用于对比 `gap`） | 是（仅显示） | 无 |
| `log_ack.h:154` | `log_ack_segment_deletable()` 函数体 | **否** | 🟡 LOW（见 1.3） |
| `log_manager.h:308` | 注释 | 否 | — |

### 1.2 三项关键确认（逐项对照要求）

| 要求 | 实际 | 证据 |
|---|---|---|
| **replay 是否完全改为 slot 遍历** | ✅ 是 | `cloud_replay_step()` 游标为 `(s_replay_seg, s_replay_idx)`；读偏移 = `HEADER + idx*RECORD_SIZE`；判定用 `rec.seq`（记录自身）。**全文件已无 `seq - first_seq` 任何形式** |
| **reclaim 是否使用实测 last_seq** | ✅ 是 | `flash_scan_all()` 逐段读**末槽**记录取 `last_rec.seq` 存入 `s_seg_last_seq[]`（读失败置 `0xFFFFFFFF` = "永不满足 ≤ 水位" = 保守不回收）；`cloud_delete_acked_segments()` 与 `flash_ensure_append_target()` 的受害者选择都传 `s_seg_last_seq[]` |
| **unacked 统计是否基于 record.seq** | ✅ 是 | `flash_count_unacked()` 逐槽 `f.seek` + `read` 后比较 `rec.seq > acked_seq`；快速路径 `s_seg_last_seq[seg] <= acked_seq` 也是实测值；读失败保守计剩余条数 |
| `flash_find_segment_of_seq` 是否残留 | ✅ **已删除**（仅 2 处注释提及"为什么不再用它"） | `grep` 全项目无定义、无调用 |
| `flash_find_max_seq()`（meta 重建用）是否也依赖稠密假设 | ✅ 否 | 逐槽读记录取 `rec.seq` 最大值，与 `records` 只作循环上界 |

### 1.3 残留项与风险等级

| # | 位置 | 问题 | 等级 | 说明 |
|---|---|---|---|---|
| R1 | `log_ack.h:146 log_ack_segment_deletable()` | 仍在头文件中作为**公开 inline 函数**存在，函数体就是被否决的稠密推导。当前无生产调用点（仅 `probe_ack.cpp` 回归用），但名字与 `log_ack_segment_reclaimable()` 高度相近，**后来者极易误用** | 🟡 **LOW** | 建议（不实施）：改名为 `log_ack_segment_deletable_legacy()`，或加 `#ifdef LOG_ENABLE_LEGACY_TESTS` 包裹 + 文件头写死"生产路径禁止调用"。函数注释已有该警告，但名字仍是隐患 |
| R2 | `log_manager.cpp:211 s_replay_seq` | 现在是**纯诊断**字段（最后考察到的 record seq），但名字延续了旧实现的"游标"语义，易被误解为仍按 seq 遍历 | 🟡 **LOW** | 已有注释标注"诊断"，可接受 |
| R3 | `log_hole_add` 的 `holes[i].to + 1u` | `to == UINT32_MAX` 时回绕。实际 seq 远小于此（Boot 预留区间 +256/次） | 🟢 **可忽略** | 仅记录 |

**判定：稠密假设已在生产路径彻底清除，无 CRITICAL / HIGH 残留。**

---

## 2. DIR-1 replay 正确性验证

### 2.1 游标形态：✅ 已是 `(segment, slot)`

```
static uint8_t s_replay_seg = 0xFF;   // 0xFF = 需从"first_seq 最小"的段重新开始
static uint8_t s_replay_idx = 0;      // 段内下一个待考察的 slot 下标
static bool    s_replay_armed = false;
```

### 2.2 用户给定的场景验证（slot 0 seq=10 WARN / 1 INFO(不存在) / 2 seq=12 ERROR / 3 seq=15 WARN）

此处 `records = 3`（3 条有效记录）。新逻辑：

```
idx=0 → off=HEADER+0*128 → 读 seq=10 → should_replay → push
idx=1 → off=HEADER+1*128 → 读 seq=12 → push
idx=2 → off=HEADER+2*128 → 读 seq=15 → push
idx=3 ≥ records(3) → 切下一段（return，下轮再扫）
```

**不会因 seq 空洞提前结束** ✅。而旧逻辑 `idx = seq - first_seq` 会算出 `0 / 2 / 5`：slot 5 越界（`records=3`），并且 `flash_find_segment_of_seq()` 在 `seq > first+records-1` 时返回 `0xFF` ⇒ `s_replay_done=true`。**修正确认。**

### 2.3 段间推进与终止

| 机制 | 实现 | 正确性 |
|---|---|---|
| 段间选择 | `flash_next_segment_after(after_seq, have_after)`：按 `first_seq` **升序**选"严格大于"的段；只依赖 `first_seq`（一个直接来自段头的实测值），**不依赖任何稠密假设** | ✅ |
| 每轮 I/O 上界 | 只扫**一个段**；`scanned < LOG_REPLAY_SCAN_MAX_PER_CALL(32)`、`pushed < LOG_BATCH_MAX_RECORDS(16)` | ✅ 与一个 append 单元同量级 |
| 正常终止 | `s_replay_armed=false; s_replay_done=true`，打印 `replay sweep done (acked=… giveup=…)` | ✅ |
| 段不可读 | `s_replay_idx = s_seg_records[seg]` ⇒ 下轮切下一段（不误判整轮结束） | ✅ |
| 单条损坏 | `continue`（只跳过该槽） | ✅ |

### 2.4 re-arm 机制复核（用户重点项）

三个触发点，全部实读确认：

| 触发点 | 位置 | 覆盖的情形 |
|---|---|---|
| `log_init()` | `:1402 cloud_replay_arm()` | 重启（`acked_seq` 归 0 ⇒ 全部 Flash 记录重新可补发） |
| `log_cloud_test_reset()` | `:2691` | 测试基线复位 |
| **被删段正是游标所在段** | `flash_drop_segment():535-540`：`if (s_replay_seg == seg) { s_replay_seg = 0xFF; s_replay_idx = 0; s_replay_armed = true; }` | 防止游标指向不存在的段 ⇒ 扫描提前结束 ⇒ 漏掉其后所有未确认记录 |

**"一轮 replay 完成后是否还能重新启动"** —— 会，但**只在上述三处**。这是刻意的：正常运行时新记录直接进云队列（不需要 replay）；replay 的职责是"把 Flash 里**本 Boot 未确认**的旧记录送出去"。sweep 完成后本 Boot 内不再重扫，符合设计（下一次 Boot 由 `log_init` 重新 arm）。

### 2.5 DIR-1 发现的两个残留项

| # | 问题 | 等级 | 详述 |
|---|---|---|---|
| D1 | **被删段触发 re-arm 会从最老段重扫** ⇒ 可能把已推送过的记录**再次入队** | 🟡 **MEDIUM** | `flash_drop_segment()` 重置为 `s_replay_seg=0xFF` ⇒ 下轮 `flash_next_segment_after(0,false)` = **最老段**。若游标原本已在环的中途，会重扫已扫过的段并重复 push。后果是重复上云（云端按 `(device_id,boot_seq,seq)` 幂等消化，**不丢数据**），但会占用云队列配额（128 槽）并放大流量。<br>触发条件：replay sweep 进行中恰好发生段删除（需环压 + sweep 并发，罕见）。<br>建议（不实施）：改为"取当前段 `first_seq` 之后的下一个段"而非回到最老段；或在 arm 时保留 `s_replay_seg` 的 `first_seq` 作为 `after_seq` |
| D2 | sweep 完成后本 Boot 不再重扫 | 🟢 **非缺陷** | 与 `cloud_queue_push()` 中"在途窗口外淘汰 ⇒ 登记空洞"的注释一致（注释明写"本 Boot 内已不可能被 replay 取回，留待下次开机重放"）。逻辑自洽 |

---

## 3. FIX-2 hole reclaim 复核

### 3.1 合并条件：✅ 正确（含相邻）

```c
for (uint8_t i = 0; i < *count; i++) {
    if (from <= holes[i].to + 1u && to + 1u >= holes[i].from) {
        if (from < holes[i].from) holes[i].from = from;
        if (to   > holes[i].to)   holes[i].to   = to;
        return true;
    }
}
if (*count >= LOG_HOLE_MAX) return false;   // 表满且无法合并
```

| 情形 | 判定 | 结果 |
|---|---|---|
| `[10,20]` + `[21,30]`（相邻） | `21 <= 21` ✓ 且 `21 >= 10` ✓ | 合并为 `[10,30]` ✅ |
| `[10,20]` + `[15,25]`（重叠） | ✓ | `[10,25]` ✅ |
| `[10,20]` + `[5,8]`（左邻） | `5 <= 21` ✓ 且 `9 >= 10`？→ `to+1=9 >= from=10` ✗ | **不合并**，新增条目 ✅（正确：`[5,8]` 与 `[10,20]` 之间有 seq=9 的空隙，不是连续空洞） |
| `[10,20]` + `[22,30]`（隔 1） | `22 <= 21`？✗ | 新增条目 ✅（正确） |
| 表满且无法合并 | `return false` | 调用方置 `s_cloud_hole_overflow = true` ✅ |

> 注：`log_hole_add` 只合并**首个**匹配项，不级联检测新扩展后的区间是否会与后面的条目合并。因登记顺序递增（seq 单调），实际不会产生本可合并的残留条目。

### 3.2 `log_ack_segment_reclaimable()`：✅ 四条件全部在位

```c
if (hole_overflow) return false;              // ④ 表溢出 ⇒ 一律不可回收（保守）
if (last_seq < first_seq) return false;       // ① 空段
if (last_seq > acked_seq) return false;       // ② 整段 ≤ 水位
return !log_hole_overlaps(holes, count, first_seq, last_seq);  // ③ 与任何空洞不相交
```

`log_hole_overlaps` 的相交判定 `!(holes[i].to < first || holes[i].from > last)` 是标准闭区间相交，边界相触（`to == first`）判为**相交** ✅（保守方向正确）。

**"partial ACK 不会删除 segment"** ✅ —— 31 条/段，未覆盖尾部与已覆盖前缀同段 ⇒ 段末 seq 必然 > 水位 ⇒ 条件 ② 失败。

### 3.3 空洞登记点复核

| 登记点 | 是否登记 | 条件 | 正确性 |
|---|---|---|---|
| **give-up** | ✅ 登记 | `log_hole_add(holes, …, s_cloud_tx_from, s_cloud_tx_to)` —— 整个批次区间 | ✅ 与"记录仍在 Flash、留待下次开机"的语义一致 |
| **队列溢出淘汰** | ✅ 登记，但**仅在非在途窗口** | `if (in_flight_window) { evict_inf++ } else { qdrop++; if (Flash-routed && 同 boot) hole_add(evict_seq, evict_seq) }` | ✅ **这一分支位置很关键且正确**：在途窗口内被淘汰的记录**仍可能被 ACK**，其归宿由 ACK/give-up 路径收口；若在此处也登记空洞，一旦该批被 ACK 就会留下**永不复位的陈旧空洞**，永久阻塞段回收。代码把 `hole_add` 放在 `else` 分支，避免了这一点。**已逐行确认。** |
| 段压力淘汰 | ❌ 不登记 | 段被整段删除，保护对象已不存在 | ✅ 正确（改为在删除前用 `flash_count_unacked()` 记 `drop_unacked`） |

### 3.4 空洞生命周期

```
登记（两个来源：give-up 区间 / 非在途窗口的 Flash-routed 淘汰）
   │  相邻或重叠 ⇒ 合并（常态下 give-up 的整段区间会吸收后续小块）
   ↓
存续：纯 RAM 态。**不持久化、不需要任何 Flash 格式迁移**
   │  （重启后 acked_seq=0 ⇒ 由"全部未确认"重新推导，自愈）
   ↓
失效：无失效路径 —— 只由 `log_cloud_test_reset()` 与重启清空
   │  （这是刻意的：空洞保护的是"从未被确认"的物理副本，
   │    用阈值无法安全判定其是否已确认 ⇒ 见 3.5 的论证）
   ↓
作用：`log_ack_segment_reclaimable()` 条件 ③ ⇒ 阻止段被回收
   ↓
表满（> 8 条）⇒ `hole_overflow = true` ⇒ **停止一切段回收**（保守安全）
```

### 3.5 FIX-2 的两个残留风险

| # | 问题 | 等级 | 详述与建议 |
|---|---|---|---|
| H1 | **空洞表 8 项在长离线 + 持续槽压时会耗尽** ⇒ `hovf=1` ⇒ **整个 Boot 停止段回收** ⇒ 环压只能强制淘汰未确认段 ⇒ `drop_unacked` 显著虚增（云端误以为大量数据丢失） | 🟡 **MEDIUM** | 溢出淘汰的 **Flash-routed** 记录会各登记 1 条；INFO（云路由不落盘）穿插其中会**打断相邻合并**，使条目增长快于"1 条恒等式"的预期。<br>**建议（安全、零风险）**：增加空洞修剪 —— 删除**与所有现存段都不相交**的空洞（完全低于 `min(存活段 first_seq)`，或完全高于 `max(存活段 last_seq)`）。这类空洞保护不了任何物理副本，删除无损，但能显著缓解表压力。<br>次选：把 `LOG_HOLE_MAX` 提到 16（+128 B DRAM）。 |
| H2 | **部分 ACK 与在途窗口内溢出叠加时，`[covered, evicted)` 区间的记录既未确认、又无空洞保护** | 🟡 **MEDIUM** | 详见 §4.4。这是 FIX-1 与 FIX-2 交界处的**唯一覆盖缺口**，也是本次审查**新发现**的问题。 |

> 补充（**并非缺陷**）：FIX-2 的设计刻意**不允许**用 `acked_seq` 修剪空洞。论证：`acked_seq` 是**单点高水位**，只能证明"水位之下的某条曾进入某个被确认的批次"，**不能**证明某条具体记录被确认。被淘汰而未发送的记录同样位于水位之下 —— 若用阈值修剪，就会把它们（FIX-2 存在的唯一理由）的保护去掉。因此"空洞只增不减"是正确的保守选择，代价就是 H1。

---

## 4. FIX-1 / FIX-5 ACK 生命周期复核

### 4.1 `s_cloud_tx_valid` 生命周期

| 事件 | 位置 | `inflight` | `tx_valid` | 评价 |
|---|---|---|---|---|
| 发送成功 | `cloud_try_send_batch():2306-2310` | `true` | `true` | ✅ 两者同时置位 |
| **超时** | `cloud_poll()` 步骤 (2)：`s_cloud_inflight = false;` | **`false`** | **保持 `true`** | ✅ **FIX-5 核心**：描述符不失效 |
| ACK 落账（ACCEPT/PARTIAL） | `cloud_handle_ack()` 尾部 | `false` | `false` | ✅ 描述符已消费 |
| give-up | `cloud_give_up_batch()` 尾部 | `false` | `false` | ✅ 描述符已作废 |
| 重启 | `log_init():1397` | `false` | `false` | ✅ |
| 测试复位 | `log_cloud_test_reset():2688` | `false` | `false` | ✅ |

**确认不存在"timeout 直接 invalidate batch"** ✅ —— 已 `grep` 全部 `s_cloud_tx_valid` 赋值点（仅 4 处：try_send 置 true；handle_ack / give_up / log_init / test_reset 置 false）。

### 4.2 用户给定的三个流程

**流程 A：send → timeout → retry → late ACK**

```
t=0     try_send: inflight=1, tx_valid=1, tx={boot,from,to}, rd_base=rd
t=T1    超时: retx++, backoff=now+backoff_ms, inflight=0, **tx_valid=1**
t=T2    退避到期 → collect_batch: rd_base=rd（未推进）→ tx={同一区间} → try_send → tx_valid=1
t=T3    晚到 ACK（针对第一次传输，区间相同）到达
        → classify(tx_valid=1, 区间匹配) → ACCEPT → 正确落账
```
✅ **ACK 仍然有效。** 且注意：即使 ACK 在 `[T1, T2)` 的**退避窗口内**到达（`inflight=0` 期间），`cloud_handle_ack()` 用的是 `s_cloud_tx_valid ? tx_* : 0u`（**不是** `s_cloud_inflight`），因此同样 ACCEPT ⇒ **这正是 FIX-5 修掉的 HIGH-3**。

**流程 B：send → ACK → duplicate ACK**

```
第一次 ACK → ACCEPT → tx_valid=0, acked_seq=Sn
第二次 ACK → classify: tx_boot=0 → 规则② → IGNORE
             （即使假设 tx 仍有效：ack_to <= acked_seq → 规则④ DUPLICATE → IGNORE）
```
✅ **第二次 ACK 不推进**（双重保险，幂等）。

**流程 C：old ACK → new batch**

```
批 A = [100..115] 已 ACCEPT → acked_seq=115, tx_valid=0
批 B = [116..131] 发送中 → tx_valid=1, tx={...,116,131}
迟到的 A 的部分 ACK [100..110] 到达
  → 规则④: ack_to(110) <= acked(115) → DUPLICATE → IGNORE ✅
迟到的 A 的更早批次 ACK [84..99]
  → ack_to(99) < tx_from(116) → 规则⑤ IGNORE ✅
```
✅ **不会误确认新批次。**

### 4.3 FIX-1 绝对基准推进复核

```c
const uint32_t evicted = s_cloud_q_rd - s_cloud_tx_rd_base;
const uint32_t adv = (covered > evicted) ? covered : evicted;
uint32_t new_rd = s_cloud_tx_rd_base + adv;      // 钳位到 wr
```

- `cloud_handle_ack()` 与 `cloud_give_up_batch()` **同构**使用该公式 ✅
- `rd_base` 只在 `cloud_collect_batch()` 中、且 `used > 0` 时更新（避免 `n==0` 的轮次污染基准）✅
- **不存在 `rd < rd_base`**：`rd` 只增、`rd_base` 取自 `rd` 的快照 ⇒ `evicted ≥ 0` 恒成立 ✅
- 钳位 `if (new_rd > s_cloud_q_wr) new_rd = s_cloud_q_wr;` 是防御性的（正常不可达）✅

**不变量（已逐条核对）**：每条记录离开队列的原因**恰好**属于 {ACK 确认、溢出淘汰（in-flight 或非 in-flight）、give-up} 之一，不再有"既未发送也未计数"的静默跳过 —— **CRITICAL-1 已修复**。

### 4.4 ★ 新发现：`[covered, evicted)` 区间无保护（FIX-1 × FIX-2 交界缺口）

**场景**（`evicted > covered`）：

```
rd_base = R，批 = 位置 R..R+15（16 条）
在途期间发生 12 次溢出淘汰 → rd = R+12（全部计入 cloud_q_evict_inflight，无空洞）
云端只 ACK 前缀 4 条 → covered = 4
落账：adv = max(4, 12) = 12 → rd = R+12
```

结果：

| 位置 | 是否被 ACK | 是否已离开队列 | 是否登记空洞 | 归宿 |
|---|---|---|---|---|
| R..R+3 | ✅ 是 | ✅ | — | 正常确认 |
| **R+4..R+11** | ❌ **否** | ✅ **是** | ❌ **无** | ⚠️ **无保护** |

这 8 条：**曾经被发送过**（在批次里），因此云端大概率已收到。但设备侧**从未确认**它们，且因为 `rd` 已越过它们，**本 Boot 内不会再发送**。它们仍在 Flash 中，但**没有空洞保护** ⇒ 一旦后续批次把 `acked_seq` 推过它们的 seq，其所在段就可能被回收 ⇒ **物理副本消失 ⇒ 重启后也无法补发**。

- **触发条件**：部分 ACK + 在途窗口内溢出，两者同时发生（高负载 + 云端 ACK 粒度较粗）。
- **等级**：🟡 **MEDIUM**（数据丢失窗口窄、概率低；但后果是 WARN+ 静默丢失）
- **与 CRITICAL-1 的关系**：CRITICAL-1 修掉了"无计数跳过"；本条是同一处的**保护缺口**——计数已一致（记为 `evict_inf`），但**保护**没跟上。
- **建议修复（不实施，两种，择一）**：
  1. **最小**：在 `cloud_handle_ack()` 落账时，若 `evicted > covered`，为 `[s_cloud_batch[covered].seq, s_cloud_batch[evicted-1].seq]` 中 **Flash-routed** 的记录登记空洞（逐条或区间）。
  2. **更干净**：`cloud_queue_push()` 在在途窗口内淘汰时，**不**立即记 `evict_inf`，而是记住该位置；由 ACK 落账统一按 `covered` 判定 —— 被覆盖的算确认、未被覆盖的登记空洞并计 `drop_unacked`。
- **建议补充测试**：在 `test/log_fix_tests.txt` 增补一条 `ackauto 1 4`（部分覆盖 4 条）+ `evict_inf > 4` 的交叉用例（当前文件只有全量 ACK 的 F1，检测不到本缺口）。

### 4.5 阻塞与非阻塞

- `grep "delay(|while(|for(;;)" src/log_manager.cpp` ⇒ 无新增阻塞点 ✅
- 退避序列：`log_ack_backoff_ms_ex(retry, base)`；默认 base=2000 ⇒ **2 / 4 / 8 / 16 / 32 s**，上限 **60000 ms** ✅
- `log_ack_should_give_up(retry) = retry > 5` ⇒ 第 6 次超时放弃 ✅
- 无独立 Task；`cloud_poll()` 在 `log_task()` 内、`log_task()` 在 `loop()` 内 ✅（保持 LittleFS 单写者）

---

## 5. FIX-4 Cloud ACK 协议隔离复核

### 5.1 实现（`cloud_manager.cpp:1058-1084`）

```c
const bool is_log_ack = (strcmp(c, LOG_ACK_COMMAND) == 0);

String cmd_id = compact ? (doc["i"] | "") : (doc["id"] | "");
if(!is_log_ack && cmd_id.length() == 0)                 { /* Missing id */ return; }
if(!is_log_ack && cloud_check_duplicate_cmd(cmd_id))    { /* duplicate */ return; }

if(strcmp(c, "change_msg_limit") == 0) { ... return; }   // 保持原样
if(strcmp(c, LOG_ACK_COMMAND) == 0)    { ... return; }   // log_ack 旁路
```

| 要求 | 结论 |
|---|---|
| `log_ack` **不要求** `id` | ✅ `!is_log_ack &&` 短路 ⇒ 无 `i` 也放行 |
| `log_ack` **不进入**去重缓存 | ✅ `cloud_check_duplicate_cmd()` 完全不被调用 ⇒ 不读也不写 `cmd_id_cache[]` |
| 普通 command **仍检查** `id` | ✅ `is_log_ack == false` ⇒ 走原路径 |
| 普通 command **仍进入**去重缓存 | ✅ 同上 |
| 高频 `log_ack` **不会污染** `MQTT_DUP_CACHE_SIZE = 10` | ✅ **这正是 FIX-4 修掉的副作用**：ACK 最高 1 条/500 ms，10 项/30 s TTL 缓存数秒即被刷满，会挤掉真实命令 id ⇒ 命令去重失效 ⇒ 同一条命令可能被执行两次（比丢一条 ACK 更严重） |
| ACK 的 `id` 重复是否被拦截 | ✅ 不需要：`log_ack_classify()` 判 DUPLICATE（幂等无副作用），已由 §4.2 流程 B 验证 |

### 5.2 字段名一致性（设备期望 vs 云端发送）

| 项 | 设备（`cloud_manager.cpp:1101-1117`） | 常量 | PC 工具（`test/mqtt_log_probe.py cmd_ack`） | 一致 |
|---|---|---|---|---|
| 命令名 | `doc["c"]`（紧凑）/ `doc["cmd"]`（旧） | `LOG_ACK_COMMAND "log_ack"` | `{"c":"log_ack"}` | ✅ |
| boot_seq | `pl["b"]` | `LOG_ACK_P_BOOT_SEQ "b"` | `{"b":…}` | ✅ |
| seq_from | `pl["f"]` | `LOG_ACK_P_SEQ_FROM "f"` | `{"f":…}` | ✅ |
| seq_to | `pl["t"]` | `LOG_ACK_P_SEQ_TO "t"` | `{"t":…}` | ✅ |
| payload 容器 | `compact ? doc["p"] : doc["pl"]` | — | `p` | ✅ |
| 是否回 ACK | **不回**（明确注释） | — | — | ✅ |

**设备期望格式**：`{"c":"log_ack","i":"<可选>","p":{"b":<boot_seq>,"f":<seq_from>,"t":<seq_to>}}`
**云端发送格式**：同上（与 PC 工具一致）

### 5.3 FIX-4 的残留项

| # | 问题 | 等级 |
|---|---|---|
| F4-1 | PC 工具 `cmd_ack` 用 `"i": "ack%d" % int(time.time())` —— **秒级** id。若同一秒内调用两次会重号。因 `log_ack` 已不受去重约束 ⇒ **无影响**（反而说明 FIX-4 生效）。但若用 `raw` 手工给**真实命令**复用了同一 `i`，会被判重复 —— **测试时须注意** | 🟢 LOW（工具用法提示） |
| F4-2 | 旧格式 `{"cmd":"log_ack","id":…,"pl":{…}}` 同样被旁路（因为 `c` 取值兼容两套写法）⇒ 兼容性良好，无需额外处理 | 🟢 非缺陷 |

---

## 6. 新增设备测试计划 → 已生成 `test/log_fix_tests.txt`

按用户 §6 的 F0–F5 要求生成。**文件已写盘，但尚未在硬件上执行**（开发板未连接）。

| 段 | 覆盖 | 关键判别断言 | 旧行为（作对照） |
|---|---|---|---|
| **F0** | 稀疏 seq 模型 + DIR-1 跨段补发 | `fseg2 gap=15`；`fseg 0 recs=31` / `fseg 1 recs=9`；`ackst replay=` **16 → 31 → 40**；`cloud3 rarmed=0` | 旧实现 replay 停在 **31**（seg1 的 9 条永久丢失） |
| **F1** | 队列溢出 × 在途 ACK 不跳条 | `cstats2 evict_inf=12`；`cstats qdrop=0`；`cloud3 holes=0`；`ackauto 0 → qused=124` | 旧实现 `qused=112` 且静默跳过 12 条 |
| **F2** | give-up → 空洞保护 →（复位）→ 补发 → 确认后回收 | 本 Boot：`fdrop=8 / holes=1 / seg_del=0`；重启后：`rarmed=1 → replay=8 → segdel=1 / seg_del=1` | 旧实现无空洞 ⇒ 段可能被删 ⇒ 无法重放 |
| **F3-A** | 段环压力淘汰未确认段必须记账 | `seg_evict_unacked=31`；`fdrop=31`；`total=466`；`oldest=1` | 旧实现无任何记账 |
| **F3-B** | 已确认段释放不计 `drop_unacked` | `seg_del=1 / segdel=1` 且 `seg_evict_unacked=0` | — |
| **F4** | 超时后描述符仍有效（晚到 ACK 生效） | `retx=1` + **`tx_valid=1` 且 `inflight=0`**；随后 `ack_ok=1 / tx_valid=0` | 旧实现超时后 ACK 必被丢弃 ⇒ `ack_ok` 永远到不了 1 |
| **F5** | `log_ack` 不污染命令去重缓存（**需 MQTT**） | 15 条 ACK 之后，重复 `i` 的命令**仍**被判 duplicate | 旧实现会刷满 10 项缓存 ⇒ 命令去重失效 |
| **F6** | 既有回归（`log_core/flash/recovery/corrupt/handoff`） | 66 / 92 / 34 / 43 / 56 | — |

### 6.1 设计要点（为什么这样设计）

1. **F0 只用 3 条命令就造出 seq 空洞** —— 依托 `logt fill info N`（INFO 消耗 seq 但**不落 Flash**），无需逐条 `emit`。
2. **F0 的中间值 16 / 31 / 40 是确定性的**（已按代码推导）：批 1 = 16 条；批 2 = 15 条（每轮只扫一个段，到段尾停）；空转一轮（游标切段后**直接 return**，不 push）；批 3 = 9 条。
3. **F4 用 `abackoff 20000` 造出约 20 s 的稳定观察窗口**，使"`inflight=0` 且 `tx_valid=1`"这一状态**可稳定断言**（否则状态会随重发漂移）；随后用 `atimeout 60000` 冻结超时，让重发后的批次稳定在途。
4. **F2 跨复位**，因此文件内以 `★ 复位开发板 ★` 显式标出边界，需分两次运行。
5. **F3-B 的诚实说明**：`flash_ensure_append_target()` 里"压力时优先挑已确认段"的分支在板上**难以直接命中** —— 正常回收路径（ACK 时触发的 `cloud_delete_acked_segments`）会先把已确认段清掉。因此 F3-B 验证的是它的**等价保证**（已确认数据的释放无需付 `drop_unacked` 代价），该分支本身由代码审查（§3.3）覆盖。**已在用例文件内以注释写明，不伪装成已覆盖。**
6. **F5 明确标注需要 MQTT**：`logt ack` / `ackauto` 直接注入 LogManager，**绕过** `cloud_process_rx_message`，因此无法用串口验证 FIX-4。

---

## 7. 输出（按要求格式）

### A. 当前状态总结

| 项 | 状态 |
|---|---|
| 代码基线 | `5816b32`（FIX-1/2/3/5 + DIR-1 + FIX-4 全部落地） |
| **Flash seq 稀疏模型** | ✅ **生产路径已彻底移除稠密推导**；`flash_find_segment_of_seq` 已删除；reclaim / unacked 统计 / replay 全部改用实测值或记录自身 seq |
| **DIR-1 replay** | ✅ 游标为 `(seg, idx)` slot 遍历；判定用 `rec.seq`；跨段推进依赖 `first_seq` 升序；re-arm 三处齐备 |
| **FIX-2 空洞回收** | ✅ 相邻/重叠合并正确；表溢出即停回收；登记点（give-up 区间 / 非在途窗口淘汰）**位置正确** |
| **FIX-1 游标** | ✅ 绝对基准 `rd = rd_base + max(evicted, covered)`；每条离队原因唯一归属，无静默跳过 |
| **FIX-5 描述符** | ✅ 有效期与 `inflight` 解耦；超时不 invalidate；晚到 ACK 仍 ACCEPT |
| **FIX-4 协议隔离** | ✅ `log_ack` 不需 `id`、不进去重缓存；普通命令不受影响；字段名与 PC 工具完全一致 |
| **构建** | ✅ PASS（`build_fx3`，106.07 s） |
| **静态 RAM** | 130 048 → **130 192 B（+144 B）**；FIX-4 **+0 B**。无新增 PSRAM 占用 |
| **契约测试（离线）** | ✅ ALL PASS 4/4（正向 / 负向 / CBOR 16 项 / ACK 逻辑 **83 项**，后两步为 wasm32 真实执行） |
| **设备测试** | ⛔ **未执行** —— 开发板未连接（`list_ports` 仅 3 个蓝牙虚拟串口） |

**本次审查结论集中为：2 个 MEDIUM（H2 / D1）+ 1 个 MEDIUM（H1）、3 个 LOW。无 CRITICAL、无 HIGH。**

### B. 是否可以进入设备测试

**可以，条件已具备**（但有两项前置）

| 前置项 | 说明 |
|---|---|
| ① 接入开发板 | 唯一硬前置。`test/log_fix_tests.txt` 已就绪，接上后按 §D 分段执行 |
| ② MQTT 侧测试（仅 F5 需要） | EMQX 凭据见 `AI_RULES.md` 末尾（`n302933b.ala.cn-hangzhou.emqxsl.cn:8883`，CA 在 `data/emqxsl-ca.crt`）。**`guo_feeder/log` 的 ACL 已于 09-16 实测通过**；F5 只需 `down` Topic（无需 log Topic），因此**无阻塞** |

**建议顺序**：先跑 **F0 + F1 + F4**（≈110 s，最快暴露 slot 遍历与游标问题）→ 再跑 **F2**（含复位，两段）→ 再跑 **F3-A / F3-B**（各 ≈70 s）→ 最后 **F5**（MQTT）→ 结束后复跑 **F6** 全套回归。

> ⚠️ 提醒：`test/log_fix_tests.txt` 的数值全部为**代码推导**，尚未在硬件上校准。若某条不符，**先看设备实际输出**再判定是断言错还是代码错（历史经验：多数是断言写错）。

### C. 剩余风险列表

| # | 等级 | 问题 | 位置 | 建议 |
|---|---|---|---|---|
| **H2** | 🟡 **MEDIUM** | **部分 ACK × 在途窗口溢出 ⇒ `[covered, evicted)` 区间既未确认又无空洞保护** ⇒ 段被回收后该区间 WARN+ 无法在重启后补发 | `cloud_handle_ack()` 的 `adv = max(covered, evicted)` | 落账时若 `evicted > covered`，为该区间 Flash-routed 记录登记空洞（详见 §4.4） |
| **H1** | 🟡 **MEDIUM** | **空洞表 8 项耗尽可能**（INFO 穿插打断合并）⇒ `hovf=1` ⇒ 整个 Boot 停止段回收 ⇒ `drop_unacked` 虚增 | `LOG_HOLE_MAX 8` / `log_hole_add()` | 增加安全修剪：删除"与所有现存段都不相交"的空洞（无损）；或把上限提到 16（+128 B） |
| **D1** | 🟡 **MEDIUM** | **游标所在段被删触发 re-arm 时从最老段重扫** ⇒ 可能重复推送已推送记录（云端幂等消化，不丢数据；但占队列配额、放大流量） | `flash_drop_segment():535-540` | re-arm 时保留当前段的 `first_seq` 作为 `after_seq`，而非回到 0 |
| R1 | 🟢 LOW | `log_ack_segment_deletable()` 仍作为公开 inline 函数存在（函数体即被否决的稠密推导），与 `log_ack_segment_reclaimable()` 名字相近，易误用 | `log_ack.h:146` | 改名 `..._legacy` 或加编译期门控 |
| R2 | 🟢 LOW | `s_replay_seq` 名为"游标"但实为诊断字段 | `log_manager.cpp:211` | 改名 `s_replay_diag_seq` |
| R3 | 🟢 LOW | `log_hole_add` 的 `to + 1u` 在 `to == UINT32_MAX` 时回绕 | `log_ack.h:198` | 实际不可达，仅记录 |
| F4-1 | 🟢 LOW | PC 工具 `cmd_ack` 用秒级时间戳作 `i`，同秒两次会重号（对 `log_ack` 无影响；对真实命令有影响） | `test/mqtt_log_probe.py:191` | 测试时注意不要给真实命令复用同一 `i` |

**仍未覆盖的测试项（诚实清单）**：

1. `[covered, evicted)` 缺口（H2）—— 当前用例只有全量 ACK，**检测不到**；需补 `ackauto 1 4` × `evict_inf > 4` 的交叉用例
2. 空洞表 overflow（H1）与相邻合并在真机上的表现
3. 段删除 × replay sweep 并发（D1）
4. PSRAM 分配失败 → DRAM 回退路径（`cloud_init_buffers` 的落点统计仍只用一个 bool，**CBOR 缓冲落点未单独记录**）
5. MCU 硬掉电事务恢复（Flash 段半写）
6. `cloud_q_evict_inflight` 与非在途淘汰交界的边界计数
7. 16 Workflow × 16 Step 满载、`NO_FREE_SLOT`（与 Log 无关，历史遗留）
8. **`test/log_fix_tests.txt` 本身未执行** ⇒ 上述全部设备侧结论仍属"代码推导"

### D. 测试命令清单

**生成物**：`test/log_fix_tests.txt`（F0–F6，含每段的推导注释与旧行为对照）

```bash
# ---------- 0) 构建 + 烧录 ----------
BD=".pio/build/fixfinal"
mkdir -p "$BD/esp32-s3-devkitc-1"
PLATFORMIO_BUILD_DIR="$BD" timeout 190 pio run
python "D:/platformIO/packages/tool-esptoolpy/esptool.py" --chip esp32s3 \
  --port COM8 --baud 921600 write_flash -z 0x10000 \
  "$BD/esp32-s3-devkitc-1/firmware.bin"

# ---------- 1) 离线契约测试（无需硬件，先跑） ----------
python test/log_contract/run_contract_test.py

# ---------- 2) 设备侧分段回归（QUIET=3.0 / HIT_GRACE=0.35 必需） ----------
#    用 sed 抽取各段到临时文件，或人工拆成 3 个文件
python test/serial_batch.py COM8 .pio/fix_f0_f1.log <F0+F1 段> 3.0 0.35
python test/serial_batch.py COM8 .pio/fix_f4.log    <F4 段>    3.0 0.35
python test/serial_batch.py COM8 .pio/fix_f3a.log   <F3-A 段>  3.0 0.35
python test/serial_batch.py COM8 .pio/fix_f3b.log   <F3-B 段>  3.0 0.35

# ---------- 3) F2（跨复位，必须两段） ----------
python test/serial_batch.py COM8 .pio/fix_f2b_a.log <F2 段>   3.0 0.35
#     ★ 复位开发板（重新打开串口即复位）★
python test/serial_batch.py COM8 .pio/fix_f2b_b.log <F2-B 段> 3.0 0.35

# ---------- 4) F5（需 MQTT；设备在线） ----------
python test/mqtt_log_probe.py raw guo_feeder/down '{"c":"system","i":"DUPX","p":{"o":"memory"}}'
python test/mqtt_log_probe.py raw guo_feeder/down '{"c":"system","i":"DUPX","p":{"o":"memory"}}'
#    ↑ 第 2 次串口必须出现 "[Cloud] duplicate command"
for i in $(seq 1 15); do
  python test/mqtt_log_probe.py raw guo_feeder/down \
    '{"c":"log_ack","i":"AK'$i'","p":{"b":1,"f":1,"t":1}}'
done
python test/mqtt_log_probe.py raw guo_feeder/down '{"c":"system","i":"DUPX","p":{"o":"memory"}}'
#    ↑ 仍须出现 "[Cloud] duplicate command"（否则 ACK 污染了 10 项去重缓存）
#    另测：不带 i 的 log_ack 不得打印 "Missing id"
python test/mqtt_log_probe.py raw guo_feeder/down '{"c":"log_ack","p":{"b":1,"f":1,"t":1}}'

# ---------- 5) 结果判定 ----------
for f in f0_f1 f4 f3a f3b f2b_a f2b_b; do
  printf "%-8s OK=%s MISS=%s\n" "$f" \
    "$(grep -c ': OK' .pio/fix_$f.log)" "$(grep -c ': MISS' .pio/fix_$f.log)"
done
grep -n ': MISS' .pio/fix_*.log
grep -cE 'Guru Meditation|canary|abort\(\)|Stack smashing' .pio/fix_*.log   # 必须为 0

# ---------- 6) 既有回归（F6） ----------
python test/serial_batch.py COM8 .pio/fix_reg_core.log     test/log_core_tests.txt 5.0 0.35
python test/serial_batch.py COM8 .pio/fix_reg_flash.log    test/log_flash_tests.txt 3.0 0.35
python test/serial_batch.py COM8 .pio/fix_reg_recovery.log test/log_flash_recovery_tests.txt 3.0 0.35
python test/serial_batch.py COM8 .pio/fix_reg_corrupt.log  test/log_flash_corrupt_tests.txt 5.0 0.35
python test/serial_batch.py COM8 .pio/fix_reg_handoff.log  test/log_flash_handoff_tests.txt 5.0 0.35
```

### E. 不需要修改的部分（本次审查确认无需改动）

| 模块 / 机制 | 结论 |
|---|---|
| **`log_events.h`（冻结契约）** | ✅ 零改动必要。Record 128 B v2 / 段头 16 B / Meta v2 / CBOR 12 键 / `LOG_ACK_*` 常量 / Level Policy 全部与实现一致，**ABI 未变** |
| **Flash 段格式与 `meta.bin`** | ✅ 无格式迁移需求。本次修复的全部新增状态（`s_seg_last_seq` / 空洞表 / `rd_base` / `tx_valid` / replay 游标）均为**纯 RAM 派生缓存**，重启后由扫描 + `acked=0` 自愈 |
| **`cloud_queue_push()` 的空洞登记位置** | ✅ 把 `hole_add` 放在 `else`（非在途窗口）分支是**正确且关键**的 —— 从源头杜绝了"陈旧空洞永久阻塞回收"。已逐行确认，**不要改动** |
| **`log_ack_classify()` 规则与顺序** | ✅ 5 条规则（区间合法 / 无在途 / boot 匹配 / 回退重复 / 更早批次）顺序合理，被 83 项探针覆盖。**不要调整顺序** |
| **`log_ack_segment_reclaimable()` 四条件** | ✅ 条件②用实测 `last_seq`、条件③用区间相交、条件④表溢出即停 —— 全部正确 |
| **ABI / 现有函数签名** | ✅ `log_init` / `log_task` / `log_emit` / `LogCloudInfo` / `CloudManager` 的 6 个新接口均无破坏性变更 |
| **`log_cbor.h`** | ✅ freestanding、无 malloc/new/String/ArduinoJson、三重越界预检；与冻结协议逐字节一致（16 项探针） |
| **PSRAM 传 MQTT 的安全性** | ✅ 安全，且**依赖 `CLOUD_MQTT_STORE=true`**（入队即拷贝）。**不要把这个开关改成 false** —— 否则 `s_cloud_cbor` 会在下次组包时被覆写，发出脏数据 |
| **依赖方向** | ✅ `LogManager → CloudManager` 单向；`cloud_manager.*` 不 include `log_manager.h`（回调注入） |
| **`log_ack` 旁路的三个分支位置** | ✅ 在 `change_msg_limit` 之后、`cloud_translate_command` 之前，与既有协议级消息处理风格一致；**不要重排** |
| **无独立 Task / 单写者约束** | ✅ `log_task()` 在 `loop()` 内，LittleFS 单写者不变 |
| **`test/log_core_tests.txt` / `log_flash_tests.txt` 等既有用例** | ✅ 无需因 FIX-1/2/3 改写：这些用例全程不发 ACK ⇒ `acked_seq = 0` ⇒ 无"已确认段" ⇒ 受害者选择仍取最老段，行为与修复前一致（`oldest=` / `total=466` 等期望值仍成立）。**但必须在最终固件上复跑确认**（见 F6） |

---

## 附：本次审查未做的事（声明）

- 未修改任何 `.cpp` / `.h` / `.json` / 协议文档
- 未产生任何 git commit（`HEAD` 仍为 `5816b32`）
- 未执行任何设备测试（开发板未连接）
- `test/log_fix_tests.txt` 为**唯一写入物**，且**尚未执行** —— 其中所有数值均为代码推导，需上板复核
