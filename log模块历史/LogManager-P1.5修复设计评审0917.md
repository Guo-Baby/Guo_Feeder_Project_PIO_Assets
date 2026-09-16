# P1.5 修复设计评审报告

> 项目：Guo Feeder Project（ESP32-S3 N16R8）
> 日期：2026-09-17
> 审计对象：P1.4 / P1.5（`src/log_manager.cpp` / `log_manager.h` / `log_ack.h` / `log_cbor.h` / `cloud_manager.cpp`）
> 代码基线：`892e228`（工作区 `src/` 零改动）
> 上游输入：`log模块历史/LogManager-P1.4-P1.5代码审计报告0916.md`
>
> **本报告性质：纯设计评审。未修改任何代码、未产生任何 commit。**
> 所有结论均来自对当前代码的实读，并给出**精确行号**。

---

## 0. 复核方法

对 5 个问题各自做了三件事：

1. **代码级确认**：读取实际函数体（不依赖设计文档、不依赖上一轮结论）；
2. **可达性确认**：给出触发所需的运行时条件，并核实该条件在冻结语义下是否被要求覆盖；
3. **影响面确认**：区分"未计数丢失"与"已计数丢失"，因为冻结契约（§27）允许有界但有账的丢失，**不允许静默丢失**。

关键常量（实读）：

| 常量 | 值 | 位置 |
|---|---|---|
| `LOG_CLOUD_QUEUE_SLOTS` | 128 | `log_manager.h:61` |
| `LOG_BATCH_MAX_RECORDS` | 16 | `log_events.h:513` |
| `LOG_ACK_TIMEOUT_MS` | 15000 | `log_events.h:520` |
| `LOG_ACK_MAX_RETRY` | 5 | `log_events.h:521` |
| `LOG_RECORD_SIZE` / `LOG_RECORDS_PER_SEGMENT` / `LOG_SEGMENT_COUNT` | 128 / 31 / 16 | `log_events.h` |
| `MQTT_DUP_CACHE_SIZE` / `MQTT_DUP_CACHE_TTL_MS` | 10 / 30000 | `cloud_manager.cpp:30-31` |
| `CLOUD_MQTT_QOS` / `CLOUD_MQTT_STORE` | 1 / **true** | `cloud_manager.cpp:22-23` |

静态 RAM 基线：**130,048 B（39.7%）**（`.pio/build_p15d.log`）。

---

## 1. 结论总表

| 编号 | 审计结论 | 本轮复核 | 定级（复核后） | 一句话修正 |
|---|---|---|---|---|
| CRITICAL-1 | 队列淘汰 + ACK 落账叠加 → 游标跳过未发送记录 | ✅ **真实存在** | **CRITICAL**（成立） | 计数表述需订正（见 §7.1） |
| CRITICAL-2 | 段回收用单一水位 → 删掉含未确认记录的段 | ✅ **真实存在** | **CRITICAL**（成立） | 举例偏保守，实际**更早**触发（§7.3） |
| HIGH-1 | 段环压力淘汰不累计 `drop_unacked` | ✅ **真实存在** | **HIGH**（成立） | 另需补"受害者选择"与"游标失效"两处（§4） |
| HIGH-2 | `log_ack` 仍受 `id` 必需性与去重缓存约束 | ✅ **真实存在** | **HIGH**（成立，且**危害比审计描述的更大**） | 见 §5.3 补充发现 |
| HIGH-3 | 退避窗口内到达的 ACK 被无条件忽略 | ✅ **真实存在** | **HIGH**（成立，但**非数据丢失路径**） | 定级保留，性质需注明（§7.4） |

**无一条误报。** 但有 1 处**后果描述不完整**、1 处**举例不精确**、1 处**性质未注明**、1 处**计数表述有误**，另有 **3 个同源问题审计未列出**，一并见 §7 / §8。

---

## 2. CRITICAL-1：队列淘汰与 ACK 落账叠加 → 游标跳过未发送记录

### 2.1 代码级确认

`src/log_manager.cpp` 中 `s_cloud_q_rd` 的**全部修改点**（实读 grep 结果）：

| 行号 | 所在函数 | 语句 | 语义 |
|---|---|---|---|
| 1600 | `cloud_queue_push()` | `s_cloud_q_rd++` | 队列满 → FIFO 淘汰最旧 |
| 1821 | `cloud_handle_ack()` | `s_cloud_q_rd += covered` | ACK 落账 → 推进被覆盖前缀 |
| 2000 | `cloud_give_up_batch()` | `s_cloud_q_rd += n` | 放弃批次 → 整体移出 |
| 2294 | `log_cloud_test_reset()` | `s_cloud_q_rd = s_cloud_q_wr` | 测试复位 |

而 `cloud_collect_batch()`（:1628）用**当时的** `s_cloud_q_rd` 计算槽位：

```cpp
const uint32_t slot = (s_cloud_q_rd + n) % LOG_CLOUD_QUEUE_SLOTS;
```

**矛盾点**：行 1821 的 `covered` 是"批次收集那一刻，从 `rd` 起算的第 k 条"，而 `rd` 本身可能在批次在途期间被行 1600 移动过。两者相加 = **相对量 + 已漂移的基准**。

### 2.2 触发条件（可达性）

在途批次存在 ⇔ 已成功 `cloud_send_log()` 且未收 ACK。
批次在途期间队列满 ⇔ `(s_cloud_q_wr - s_cloud_q_rd) >= 128`。

- 采集：`cloud_collect_batch()` 取队首 ≤16 条（:1628）；
- 入队：`log_task()` 每轮最多 8 条（`LOG_DRAIN_MAX_PER_TASK`）；
- 排空：只在 ACK 落账时（§2.1），且发送被 `LOG_TX_MIN_INTERVAL_MS=500` 节流。

⇒ 只要**产生速率 > 确认速率**（云端慢 / ACK 超时 / 高负载 burst），队列必然填满，`clean` 场景即可复现。**这正是需求 §“高负载：连续 WARN/ERROR” 要求覆盖的场景。**

### 2.3 修复前状态变化（实读推演）

```
队列容量 128。记 rd = 队首游标，wr = 写入游标，批次 b0 = 队列[R .. R+15]（16 条）

时刻  事件                               rd      wr      used   说明
────────────────────────────────────────────────────────────────────────────
t0    collect b0                          R      R+128   128    b0 覆盖 [R..R+15]
t1    push 1 条 → used=128 → 淘汰 1 条    R+1    R+129   128    淘汰下标 = R
                                                              ← 恰好是 b0[0]（在途！）
                                                              cloud_q_drop++（多计）
t2    ACK(b0, covered=16)              R+17    R+129   112    s_cloud_q_rd += 16
────────────────────────────────────────────────────────────────────────────
★ 结果：逻辑下标 R+16 的记录
     · 未被任何批次发送（t1 之后队首是 R+1，t2 之后跳到 R+17）
     · 未被任何计数覆盖（既非 cloud_q_drop，也非 cloud_flash_drop）
     · INFO(Flash NO) → 永久丢失，无副本
     · WARN+ → 物理副本在 Flash，但**本次 Boot 内补不回**（见 §7.2 订正）
```

### 2.4 修复方案 FIX-1：批次绝对基准（Absolute Base Accounting）

**核心思想**：把"从 rd 起算的相对量"改成"从**采集时刻的 rd** 起算的绝对量"，并让"淘汰"与"落账"共同作用于同一个基准。

```
新增状态：s_cloud_tx_rd_base —— cloud_collect_batch() 成功取批时的 rd

规则：任何一次"从队首移除 k 条"的记账，都写成
        rd = rd_base + max(evicted_since_collect, k)
      其中 evicted_since_collect = rd_current - rd_base（在途期间只可能由淘汰产生）
```

**修复后状态变化**：

```
t0    collect b0   → s_cloud_tx_rd_base = R ; tx_valid = true
t1    push 1 条 → 满 → 淘汰
        淘汰下标(=rd=R) 落在在途窗口 [rd_base, rd_base + tx_count) 内
        ⇒ rd = R+1，但**不计** drop_overflow（改计 cloud_q_evict_inflight，纯观测）
          理由：该条已在 b0 内、仍可能被 ACK —— 记成"丢弃"是**多计**
t2    ACK(b0, covered=16)
        evicted = rd - rd_base = 1
        rd = rd_base + max(1, 16) = R + 16
────────────────────────────────────────────────────────────────────────────
★ 队列[R+16] 仍在队首 ⇒ 下一批从它开始，不跳过任何一条
```

**不变量（修复后）**：

> 队列中每一条记录离开队首的原因**恰好**属于：
> ① 被 ACK 确认（`covered`）② 被放弃（`cloud_give_up_batch`）③ 被溢出淘汰（计数）
> **不存在"未发送且未计数"的第四条路径。**

**边界情况处理**：

| 情况 | 处理 |
|---|---|
| `evicted > covered`（淘汰吃掉了整个在途前缀） | `rd` 保持 `rd_current`（`max` 取 evicted），不额外推进 —— 正确：那些条已不在队列里 |
| `covered == 0` | 现在已有早退（`cloud_handle_ack` 内的 `if (covered == 0)`），保持 |
| 发送失败（未置 `tx_valid`） | `rd_base` 无人读取；下次 collect 覆盖之 ⇒ 无副作用 |
| 计数器 uint32 回绕 | `rd - rd_base` 恒为极小差值，无符号减法天然正确 |
| 防御性钳位 | 保留 `if (rd > wr) rd = wr;` 的现有风格（`log_task()` 已有同类钳位） |

**交付信息**

| 项 | 内容 |
|---|---|
| 修改文件 | `src/log_manager.cpp` |
| 修改函数 | `cloud_collect_batch()`（登记基准）、`cloud_queue_push()`（区分淘汰对象）、`cloud_handle_ack()`（绝对推进）、`cloud_give_up_batch()`（绝对推进） |
| 新增状态变量 | `uint32_t s_cloud_tx_rd_base`；`LogStats::cloud_q_evict_inflight`（观测，非必需） |
| RAM 影响 | **+8 B DRAM**（4 + 4），PSRAM 0 |
| Flash 格式迁移 | **不需要**（纯 RAM 态） |
| 约束 | 不引入 mutex（沿用 `s_mux` 短临界区）、无阻塞、不改 `log_events.h` |

**依赖**：淘汰窗口的判定门控建议用 FIX-5 引入的 `s_cloud_tx_valid`；若单独提交，可先退化为 `s_cloud_inflight`（仅在"超时→退避"窗口内多计一次 `cloud_q_drop`，后果轻微）。

---

## 3. CRITICAL-2：段回收用单一水位 → 删除含未确认记录的段

### 3.1 代码级确认

`src/log_ack.h:139-150`：

```cpp
static inline bool log_ack_segment_deletable(
    uint32_t first_seq, uint8_t records, uint32_t acked_seq)
{
    if (records == 0u) return false;
    const uint32_t last_seq = first_seq + (uint32_t)records - 1u;
    return last_seq <= acked_seq;        // ← 行 149：只看"段末 ≤ 水位"
}
```

`src/log_manager.cpp:1750`（`cloud_delete_acked_segments()`）：

```cpp
if (!log_ack_segment_deletable(
        s_seg_first_seq[seg], s_seg_records[seg], s_cloud_acked_seq))
{ continue; }
if (flash_drop_segment(seg)) { ... }     // 行 715 附近同类调用
```

**根因**：`s_cloud_acked_seq` 是 ACK 驱动推进的**单点高水位**（"云已确认到这个 seq"），它**不是**"该区间内每条都已确认"的集合。而水位的推进可以被**空洞**跨越：

- 空洞来源 A：`cloud_give_up_batch()`（:2000）—— 批次移出队列，但 `acked_seq` **不推进**（GIVE_UP_NOT_ADVANCE 的刻意语义）；
- 空洞来源 B：`cloud_queue_push()` 溢出淘汰 —— 记录移出队列，`acked_seq` 不推进，后续更高 seq 的批次被 ACK 后水位**越过**它。

⇒ `last_seq <= acked_seq` 在空洞存在时**语义不成立**（"段末被水位覆盖" ≠ "段内每条都已确认"）。

### 3.2 修复前状态变化

```
BEFORE（acked 是"到达点"，空洞被跨过 ⇒ 整段删除）

t0  give-up b0 = [S1..S16]
      rd 前移 16、give_up_seq = 16、cloud_flash_drop += 16
      acked_seq 仍 = 0（GIVE_UP_NOT_ADVANCE）
t1  replay 起点 = max(acked+1, give_up+1) = 17 → 组批 b1 = [S17..S32] → 发送
t2  ACK(b1, to = S32) → covered = 16 → acked_seq = 32
t3  cloud_delete_acked_segments()
      段 0：first_seq = S1, records = 31 → last_seq = S31
      S31 <= acked_seq(32) ⇒ log_ack_segment_deletable() = true
      ⇒ flash_drop_segment(0) —— **物理删除**
────────────────────────────────────────────────────────────────────────────
★ S1..S16 从未被云端确认，其唯一物理副本随段一起消失
★ 且删除不计入 drop_unacked（flash_drop_segment 只 ++flash_segment_deleted）
⇒ 静默永久丢失，同时**推翻了 P1.5 交付报告里"GIVE_UP_NOT_ADVANCE 记录留待下次开机重放"的承诺**
```

### 3.3 修复方案 FIX-2：空洞登记 + 空洞感知回收

**核心思想**：**不让水位去表达空洞**，而是显式登记空洞区间；回收条件改为"整段 ≤ 水位 **且** 与任何空洞不相交"。

**新增纯判定（放 `log_ack.h`，保持 freestanding ⇒ 可在 wasm32 上穷举）**：

```cpp
struct LogHole { uint32_t from; uint32_t to; };   // 闭区间
#define LOG_HOLE_MAX 8u

// 追加（自动与末项合并：相邻/重叠 ⇒ 合并；表满且无法合并 ⇒ 返回 false）
static inline bool log_hole_add(LogHole *h, uint8_t *n, uint32_t from, uint32_t to);

// 段是否与任何空洞相交
static inline bool log_hole_overlaps(const LogHole *h, uint8_t n,
                                     uint32_t first, uint32_t last);

// 回收判定 = 原条件 AND 无空洞相交 AND 空洞表未溢出
static inline bool log_ack_segment_reclaimable(
    uint32_t first_seq, uint8_t records, uint32_t acked_seq,
    const LogHole *h, uint8_t n, bool hole_overflow);
```

`log_ack_segment_deletable()` **保留不动**（既有 wasm 探针的断言继续有效），新函数在其基础上 `AND` 空洞条件。

**空洞登记点（三处）**：

| 来源 | 登记内容 | 理由 |
|---|---|---|
| `cloud_give_up_batch()` | `[s_cloud_tx_from, s_cloud_tx_to]` | 本 Boot 不再投递 → 段必须留到下次 Boot 重放 |
| `cloud_queue_push()` 淘汰（**仅 Flash-routed**） | `[rec.seq, rec.seq]` | 见 §3.5 论证：该条本 Boot 内已不可能被 replay 取回 |
| `flash_ensure_append_target()` 段淘汰 | **不登记**（改为计入 `drop_unacked`，见 FIX-3） | 记录即将被物理销毁，登记空洞无意义 |

> INFO 的淘汰**不登记空洞** —— INFO 无 Flash 副本，空洞无从保护（它只计 `drop_overflow`）。

**空洞表容量与溢出策略**：

- 表大小 `LOG_HOLE_MAX = 8`；
- **合并**使实际占用极小：give-up 与淘汰都是**递增且连续**的 seq，相邻区间自动并成一条（正常情况下恒为 1 条）；
- 表满且无法合并 ⇒ `s_cloud_hole_overflow = true` ⇒ **停止一切段回收**（保守、安全），重启后自然恢复。

### 3.4 修复后状态变化

```
AFTER

t0  give-up b0 = [S1..S16] → log_hole_add(1, 16) ; give_up_seq = 16
t1  replay 从 17 起 → b1 = [S17..S32] → ACK(to=S32) → acked_seq = 32
t2  cloud_delete_acked_segments()
      段 0：S31 <= 32 ✔ 但 [S1..S31] 与 hole[1..16] 相交 ✘
      ⇒ 不可回收 —— 物理副本保留
t3  reboot → acked_seq = 0, give_up_seq = 0, 空洞表为空（纯 RAM 态）
      replay 从 1 起 → S1..S16 被重新投递 → 云 ACK → 水位越过 16
t4  后续 ACK 使 acked_seq ≥ S31，且空洞已被"清除/覆盖"⇒ 段 0 正常回收 ✔
────────────────────────────────────────────────────────────────────────────
★ 自愈：空洞只在"本次 Boot 内"保护物理副本；跨 Boot 由 replay 消化
```

**空洞的清除**：`acked_seq` 越过某空洞的 `to` **并不**代表该空洞已确认（区间 ACK 无法证明区间内每条都到达）。因此采用保守策略：

- 空洞条目**不在本次 Boot 内自动移除**（除非 `hole_overflow` 复位）；
- 代价：本 Boot 内该段之后（更高 seq）的段也无法回收 ⇒ Flash 压力上升；
- **压力释放路径**是 FIX-3（优先淘汰"已确认段"，退化时对未确认段计 `drop_unacked`）—— 与冻结设计 §20 一致。

> 若希望降低保守代价，可选优化：在 `cloud_handle_ack()` 中，把"本批被确认的记录"从空洞表中剔除（这些 seq 已确证送达）。属可选增强，建议 v2 再做。

**交付信息**

| 项 | 内容 |
|---|---|
| 修改文件 | `src/log_ack.h`（新增类型与 3 个纯函数）、`src/log_manager.cpp`（登记 + 调用）、`test/log_contract/probe_ack.cpp`（新增断言） |
| 修改函数 | `log_ack.h` 新增；`cloud_give_up_batch()`、`cloud_queue_push()`、`cloud_delete_acked_segments()` |
| 新增状态变量 | `LogHole s_cloud_holes[8]`、`uint8_t s_cloud_hole_count`、`uint8_t s_cloud_hole_overflow` |
| RAM 影响 | **+66 B DRAM**（64 + 1 + 1，含对齐约 68 B），PSRAM 0 |
| Flash 格式迁移 | **不需要** —— 空洞表是纯 RAM 态，重启后由 `acked=0` 重新推导。这也是它优于"持久化确认位图"的关键 |
| 约束 | 空洞表**仅由 loopTask 读写**（give-up / 淘汰 / 回收 / 观测全在 loop 上下文），**无需进入临界区**；无 mutex；无阻塞；不改 `log_events.h` |

### 3.5 为什么"淘汰记录"也必须登记空洞（关键论证）

审计把"队列溢出"列为空洞来源之一，但**未说明它在本次 Boot 内已经不可恢复**。补充论证：

1. `rd` 的推进只发生在 ACK 落账 / give-up / 淘汰（§2.1 全表）；
2. ⇒ **队列排空必然伴随 ACK 落账**，而每次 ACK 都让 `acked_seq` 前进；
3. 被淘汰的记录位于队首（seq 最小），其后的所有批次 seq 都更大；
4. ⇒ 队列真正排空（replay 唯一被允许的时刻，`cloud_poll` 第 (6) 步 `used == 0`）时，`acked_seq` **必然已经越过**被淘汰记录的 seq；
5. ⇒ `log_ack_should_replay()`（`log_ack.h:226` `seq <= acked_seq → false`）会**跳过**它。

**结论**：队列溢出淘汰的 WARN+，在本次 Boot 内**不可能**被 replay 取回；唯一出路是重启后（`acked=0`）重放。因此必须登记空洞以保护其物理副本。

---

## 4. HIGH-1：段环压力淘汰不累计 `drop_unacked`

### 4.1 代码级确认

`flash_ensure_append_target()`（段环已满分枝，`src/log_manager.cpp:715`）：

```cpp
if (target >= LOG_SEGMENT_COUNT)
{
    const uint32_t victim = s_oldest_segment;
    if (!flash_drop_segment(victim)) { return false; }
    target = victim;
}
```

`flash_drop_segment()`（:474）：

```cpp
portENTER_CRITICAL(&s_mux);
s_stats.flash_segment_deleted++;      // ← 只有这一个计数
portEXIT_CRITICAL(&s_mux);
flash_recompute_extremes();
```

**确认**：删除一个最多含 31 条记录的段，**完全没有**触碰 `s_stats.cloud_flash_drop`（→ `drop_unacked`）。云端永远不知道有未确认数据被销毁。

**附加缺陷（审计未列）**：

- ① **受害者选择盲目**：无条件选 `s_oldest_segment`。当 FIX-2 使回收变保守后，环内可能同时存在"已确认段"与"未确认段"，此时删最老段可能牺牲未确认数据，而本可以删已确认段（删之无损）。
- ② **replay 游标失效**：`flash_drop_segment()` 既不更新 `s_replay_seq`，也不更新 `s_replay_done`。若 `s_replay_seq` 落在被删段内，`flash_find_segment_of_seq()` 返回 `0xFF` ⇒ `cloud_replay_step()` 置 `s_replay_done = true`（:1906）⇒ **整轮补发被静默终止**，后面段里还在的记录再也不会被补发。

### 4.2 修复方案 FIX-3

```
BEFORE
  flash_ensure_append_target() 段环满
      victim = s_oldest_segment            ← 无条件
      flash_drop_segment(victim)
          · 不累计 drop_unacked            ← 云端不可知
          · 不处理 replay 游标             ← 可能令补发静默终止

AFTER
  flash_ensure_append_target() 段环满
  ① 受害者优先：按 first_seq 升序遍历，
       取第一个 (last_seq <= acked_seq) 的段      ← 已确认，删除无损
       若不存在 → 退化为 s_oldest_segment（与现状一致）
  ② 删前记账：unacked = 该段中 seq > acked_seq 的条数
       s_stats.cloud_flash_drop      += unacked   (→ drop_unacked，云端可见)
       s_stats.flash_seg_evict_unacked += unacked (观测)
  ③ 删后：若 replay 游标落在该段 ⇒ 复位游标
       （最小变体：s_replay_seq = 0 强制重算；推荐变体：清 s_replay_seg/idx 并重扫）
```

**`unacked` 的精确计数**（避免引入连续假设）：新增 `uint32_t s_seg_last_seq[16]`（64 B），在 `flash_scan_all()` 与每次 append 成功后维护"该段真实末条 seq"。则：

```
unacked = (segment 完全在空洞/水位之上)
        ? records
        : (last_seq <= acked_seq ? 0 : records - clamp(acked_seq - first_seq + 1, 0, records))
```

> 为什么值得多花 64 B：`flash_find_segment_of_seq()`（:605 附近）与 `log_ack_segment_deletable()` **都**假设段内 seq 连续（`last = first + records - 1`）。而 `flash_scan_records()`（:389）遇坏记录即停、追加又从该槽位继续 ⇒ **段内可产生 seq 空洞**（旧记录在槽 0..k，新记录在槽 k+1.. 且 seq 有跳变）。此时 `last_seq` 计算偏小 ⇒ 可能**提前判定可回收**（与 CRITICAL-2 同类）。`s_seg_last_seq[16]` 一次性消除该假设，同时让 `unacked` 计数精确。

**交付信息**

| 项 | 内容 |
|---|---|
| 修改文件 | `src/log_manager.cpp`、`src/log_manager.h`（新增统计字段） |
| 修改函数 | `flash_ensure_append_target()`、`flash_drop_segment()`（或新增包装 `flash_evict_segment()`）、`flash_scan_all()`（维护 `s_seg_last_seq`）、`log_task()`（append 成功后更新末条 seq） |
| 新增状态变量 | `uint32_t s_seg_last_seq[16]`；`LogStats::flash_seg_evict_unacked` |
| RAM 影响 | **+68 B DRAM**（64 + 4）。最小变体（不引入 `s_seg_last_seq`，用连续假设近似）为 **+4 B** |
| Flash 格式迁移 | **不需要**（`s_seg_last_seq` 由扫描重建，不落盘） |
| 约束 | 无 mutex、无阻塞、不改 `log_events.h` |

**⚠️ 连带影响**：受害者选择改变后，`test/log_flash_tests.txt` 中"环满"段落的断言（`oldest=1` / `total=466`）**必须重新校准**。

---

## 5. HIGH-2：`log_ack` 仍受 `id` 必需性与去重缓存约束

### 5.1 代码级确认

`src/cloud_manager.cpp`（`cloud_process_rx_message()`）：

| 行号 | 语句 |
|---|---|
| 1060 | `String cmd_id = compact ? (doc["i"] \| "") : (doc["id"] \| "");` |
| **1061** | `if(cmd_id.length() == 0) { Serial.println("[Cloud] Missing id"); return; }` |
| **1066** | `if(cloud_check_duplicate_cmd(cmd_id)) { ... return; }` |
| **1086** | `if(strcmp(c, LOG_ACK_COMMAND) == 0) { ... }` ← 旁路分支在**两者之后** |

`cloud_check_duplicate_cmd()`（`cloud_manager.cpp:1615` 附近）：以 `id` 为键、`MQTT_DUP_CACHE_SIZE=10` 项 FIFO、`TTL=30 s`；**未命中时会写入缓存**（消费一个槽位）。

### 5.2 两个后果

1. **`i` 缺失 ⇒ 直接丢弃**：`log_ack` 本可自描述（`p.b` / `p.f` / `p.t` 已足够），却因不满足命令报文要求被拒。
2. **同一 `i` 重发 ⇒ 被去重丢弃**：**MQTT QoS1 的重复投递（DUP=1）复用完全相同的 payload**，因此 `i` 也完全相同 ⇒ 命中 30 s TTL 缓存 ⇒ **放弃**。设备丢 ACK → 重传 → 直至 give-up。

### 5.3 审计未指出的第三、四个后果（本报告补充）

3. **`log_ack` 污染命令去重缓存**：行 1066 对 `log_ack` 也执行"未命中则写入"。云上 ACK 频率最高可达 **1 条 / 500 ms**（`LOG_TX_MIN_INTERVAL_MS`），而缓存只有 **10 项 / 30 s TTL** ⇒ 持续日志流量会在 5 s 内把 10 个槽位全部刷成 `log_ack` 的 id。
   **后果**：真实命令（如 `workflow.save`、`VALVE_OPEN`）的 id 被挤出缓存 ⇒ **命令去重失效** ⇒ MQTT QoS1 重投或云端重试时，**同一条命令会被执行两次**。
   → 这是比"丢一条 ACK"更严重的副作用，且**被 FIX-4 顺带修复**。

4. **ACK 速率受下行带宽挤占**：由于 `log_ack` 也参与"命令"处理路径的串口静态打印（`Serial.println("[Cloud] MQTT RX")` 等），高频 ACK 还会带来可观的串口 I/O（每 500 ms 一次完整报文 dump）。

### 5.4 修复方案 FIX-4

**最小改动（推荐）**：在取得 `c` 之后立刻计算"是否协议级消息"，让两项检查对 `log_ack` **短路**，**不重排现有分支顺序**（降低回归风险）：

```cpp
const bool is_log_ack = (strcmp(c, LOG_ACK_COMMAND) == 0);

if (!is_log_ack && cmd_id.length() == 0) { ... return; }        // 行 1061
if (!is_log_ack && cloud_check_duplicate_cmd(cmd_id)) { ... }   // 行 1066
```

**语义后果（全部为期望行为）**：

| 情况 | 修复前 | 修复后 |
|---|---|---|
| `log_ack` 无 `i` | 丢弃 | 正常处理 |
| `log_ack` 重复投递（同 `i`） | 丢弃 | 正常处理 → `log_ack_classify()` 判为 `DUPLICATE`（幂等，无副作用） |
| `log_ack` 是否占用命令去重缓存 | **占用** | **不占用** ⇒ 命令去重恢复有效 |
| 非 `log_ack` 消息 | 不变 | 不变 |

**交付信息**

| 项 | 内容 |
|---|---|
| 修改文件 | `src/cloud_manager.cpp`、`cloud_protocol.md`（§8.2 注明 `log_ack` 的 `i` 可选、重复投递幂等） |
| 修改函数 | `cloud_process_rx_message()` |
| 新增状态变量 | **无** |
| RAM 影响 | **0** |
| Flash 格式迁移 | 不需要 |
| 约束 | 无 mutex / 无阻塞 / 不改 `log_events.h` / 不改现有 Up·Down 协议 |

---

## 6. HIGH-3：退避窗口内到达的 ACK 被无条件忽略

### 6.1 代码级确认

`cloud_handle_ack()`（:1815 附近）：

```cpp
const LogAckResult r = log_ack_classify(
    ack_boot, ack_from, ack_to,
    s_cloud_inflight ? s_cloud_tx_boot : 0u,     // ← 无在途 ⇒ 传 0
    s_cloud_inflight ? s_cloud_tx_from : 0u,
    s_cloud_inflight ? s_cloud_tx_to   : 0u,
    s_cloud_acked_seq);
```

`log_ack_classify()`（`log_ack.h:81`）：

```cpp
if (tx_boot == 0u || tx_to == 0u) { return LOG_ACK_IGNORE; }   // 规则②
```

而 `cloud_poll()` 第 (2) 步在超时时把在途标志清掉：

```cpp
s_cloud_backoff_until_ms = now + backoff;
s_cloud_inflight = false;      // ← 退避期间"无在途"
```

### 6.2 修复前状态变化

```
t0        cloud_send_log(b0) 成功 → inflight = true, deadline = T+15s
t1 = T+15  超时 → retry = 1, backoff = 2s, inflight = **false**
t2 = T+15.1  云端 ACK(b0) 到达
             cloud_poll(1) → cloud_handle_ack()
               inflight == false ⇒ tx_boot/tx_from/tx_to 一律传 0
               ⇒ log_ack_classify 命中规则② ⇒ LOG_ACK_IGNORE
               ⇒ s_cloud_ack_pending 已清空 ⇒ **该 ACK 永久丢弃**
t3 = T+17  退避结束 → 重新发送同一批次（云端需再幂等去重一次）
────────────────────────────────────────────────────────────────────────────
★ 后果：① 无谓重传（占用带宽与云端算力）；② 推进被推迟；
        ③ 增加 give-up 概率 → 增加空洞 → 与 CRITICAL-2 联动放大 Flash 压力
★ 但**不直接丢失数据**（记录始终留在云队列/Flash）—— 性质与审计定级需注明
```

### 6.3 修复方案 FIX-5：批次描述符生命周期与 `inflight` 解耦

**核心思想**：区分"是否有批次**待确认**"与"是否**正在等待**（未超时）"。前者才是 ACK 匹配的依据。

```
新增状态：bool s_cloud_tx_valid     // "最近一次成功发送的批次描述符仍然有效"
                                      （b0 的 tx_boot/tx_from/tx_to/tx_count）

置 true ：cloud_try_send_batch() 发送成功时（与 tx_* 同批设置）
置 false：① cloud_handle_ack() 提交成功（ACCEPT/PARTIAL）之后
          ② cloud_give_up_batch() 之后
          ③ 新批次被 cloud_build_cbor() 覆盖描述符时（天然转移，无需显式清）

判定依据替换：
  s_cloud_inflight ? s_cloud_tx_boot : 0u      →  s_cloud_tx_valid ? s_cloud_tx_boot : 0u
  （cloud_handle_ack 内 3 处，行 1818-1820 附近）
```

**修复后状态变化**：

```
t0        send(b0) 成功 → inflight = true, tx_valid = true, deadline = T+15s
t1 = T+15  超时 → retry = 1, backoff = 2s, inflight = false, **tx_valid 保持 true**
t2 = T+15.1  ACK(b0) 到达
             cloud_handle_ack() → tx_valid == true ⇒ 按 b0 分类 ⇒ ACCEPT
             ⇒ rd 按 covered 推进；retry = 0；backoff = 0；inflight = false；
               tx_valid = false  ⇒ 不会重传 ✔
t3 = T+17  队列已推进 ⇒ 收集的是**后续**记录，不会重复发送 b0 ✔
```

**幂等性验证（防重复推进）**：

| 交错时序 | 结果 |
|---|---|
| 超时 → 重发 b0（tx_valid 再次 true）→ 第一封 ACK 到达 | 提交一次：`rd` 推进、`tx_valid = false` ⇒ 第二封 ACK 到达时 `tx_valid == false` ⇒ IGNORE ⇒ **不重复推进** ✔ |
| 提交后重发新批次 b1 → 旧 ACK 到达 | 旧 ACK 的 `ack_to < b1.tx_from`（b1 从推进后的队首开始）⇒ 规则⑤ IGNORE ✔ |
| 部分覆盖（PARTIAL）后重发尾部 | 尾部重新组成新批次，`tx_from` = 尾部首条 ⇒ 前封 ACK 的 `ack_to < tx_from` ⇒ IGNORE ✔ |

**交付信息**

| 项 | 内容 |
|---|---|
| 修改文件 | `src/log_manager.cpp`、`src/log_manager.h`（观测可选） |
| 修改函数 | `cloud_try_send_batch()`、`cloud_handle_ack()`、`cloud_give_up_batch()`、`log_cloud_get_info()`（新增 `tx_valid` 字段便于上板断言） |
| 新增状态变量 | `bool s_cloud_tx_valid` |
| RAM 影响 | **+1 B**（`LogCloudInfo` 观测字段 +1 B，结构体对齐后 +4 B） |
| Flash 格式迁移 | **不需要** |
| 约束 | 无锁变更（`tx_valid` 与 `tx_*` 同处 loopTask，仅需与回调保持"只置标志"的现有约定）；无阻塞；不改 `log_events.h` |

---

## 7. 审计报告的误报、订正与精化

> 结论：**没有一条误报**（5 条问题全部真实存在）。但有 4 处需要订正/精化，其中 1 处**低估了影响**。

### 7.1 【表述有误】CRITICAL-1 的丢失计数

审计原文：

> `cloud_q_drop` 只记了淘汰 1 条，实际语义上丢了 2 条的发送机会。

**订正**：被淘汰的那 1 条（下标 R）**在 b0 内、仍可能被 ACK**，把它计为 `cloud_q_drop` 是**多计**（若 ACK 成功到达，它会被送达，却仍被报成丢弃）。真正**漏计**的是被跳过的那 1 条（下标 R+16）。
⇒ 应为：**1 条多计（可能仍送达）+ 1 条漏计（真实丢失）**。

### 7.2 【后果被低估】CRITICAL-1 中 WARN+ 的"可由 replay 补回"

审计原文：

> `WARN/ERROR/CRITICAL` → Flash 有副本，理论上可由 replay 补回。

**订正**：`s_replay_done` 在 `cloud_replay_step()` 内有 3 处置 `true`（`log_manager.cpp` 内 `cloud_replay_step` 的 3 处），而**只有** `log_init()`（:1188）与 `log_cloud_test_reset()`（:2319）置 `false` ⇒ **本次 Boot 内 replay 是一次性的、不会二次启动**。
因此被跳过的 WARN+ 在本次 Boot 内**也补不回**，必须重启（`acked_seq` 归 0）后由 replay 收取。
⇒ 应改为：**须重启后由 replay 补回**。
（注：FIX-1 从根因上消除了"跳过"，该隐患随之消失；但"replay 一次性"这一事实仍需在文档中写明，且是 FIX-3 必须处理 replay 游标的原因。）

### 7.3 【举例偏保守】CRITICAL-2 的段满前提

审计举例用 `段 0（S1..S31）last=31`，隐含"段恰好写满 31 条"。

**精化**：判定只看 `last_seq <= acked_seq`，**与段是否写满无关**。若该段只写了 16 条（`records=16`，`last=16`），则 `acked_seq >= 16` 即可触发删除，**比审计举例更早**发生。
⇒ 触发面比审计描述更大，结论不变。

### 7.4 【性质未注明】HIGH-3 不属数据丢失路径

HIGH-3 的直接后果是**无谓重传 + 推进延迟 + give-up 概率上升**，记录本身始终保留在云队列/Flash，**不丢数据**。定级 HIGH 可以保留（它会放大 CRITICAL-2 的空洞压力），但应注明"非数据丢失路径"，以免后续排障时误判为丢日志。

### 7.5 【经核对，非误报】审计 §9 关于 PSRAM 传 MQTT 的判断

审计称"PSRAM 传 MQTT 安全，但依赖 `store=true`"。实读确认：

- `cloud_manager.cpp:23` `#define CLOUD_MQTT_STORE true`
- `cloud_mqtt_publish_binary()` 在 `CLOUD_MQTT_QOS != 0` 分支调用 `esp_mqtt_client_enqueue(..., CLOUD_MQTT_QOS, 0, CLOUD_MQTT_STORE)`

`store=true` 时 esp-mqtt 会在入队时**复制** payload ⇒ 传 PSRAM 指针安全。**审计结论正确，保留。**

---

## 8. 审计未列出的同源问题（本报告补充）

| 编号 | 级别 | 问题 | 与本轮修复的关系 |
|---|---|---|---|
| **C-1** | HIGH | `log_ack` 会**写入并挤占** 10 项命令去重缓存（`cloud_manager.cpp:1066`）⇒ 高频 ACK 下真实命令去重失效 ⇒ 同一条命令可能被执行两次 | **FIX-4 顺带修复**（§5.3） |
| **C-2** | MEDIUM | `log_ack` 走完整命令路径的串口 dump（每 500 ms 一次完整报文打印）⇒ 稳定的串口 I/O 开销 | 可随 FIX-4 一并精简（可选） |
| **C-3** | MEDIUM | `flash_find_segment_of_seq()`（`log_manager.cpp` 内）与 `log_ack_segment_deletable()`（`log_ack.h:147`）**都假设段内 seq 连续**（`last = first + records - 1`），但 `flash_scan_records()`（:389）遇坏记录即停 + 追加从该槽位继续 ⇒ 段内可有 seq 空洞 ⇒ `last_seq` 偏小 ⇒ 可能**提前判定可回收**；`flash_find_segment_of_seq()` 亦会**漏命中** ⇒ replay 误判"补发完成" | **FIX-3 的 `s_seg_last_seq[16]` 一并消除**（§4.2） |
| **C-4** | MEDIUM | `cloud_queue_push()` 在"提交判定"之前执行（`log_task()` 阶段 1 入队，阶段 3 才决定是否提交）+ 去重守卫只比对**上一条**（`s_cloud_q_last_seq/_boot`）⇒ 一轮内保留多条时，下一轮会**重复入队** | 可选 FIX-6（§13.2） |

---

## 9. 六个重点审查项逐一回答

### 9.1 inflight batch 生命周期

| 项目 | 现状 | 修复后 |
|---|---|---|
| 描述符位置 | `s_cloud_tx_boot/from/to/count` + `s_cloud_flags`（`log_manager.cpp:130-137`） | 不变，**新增 `s_cloud_tx_rd_base`（队列基准）与 `s_cloud_tx_valid`（有效性）** |
| 有效期 | 隐式绑定 `s_cloud_inflight`（超时即失效） | 显式绑定 `s_cloud_tx_valid`（超时**不失效**，直至 ACK 或 give-up） |
| 数据保留 | 批次内容留在云队列（`rd` 未推进）⇒ 数据不丢 | 不变；且 `rd` 不再被"相对推进"越过 |
| ACK 前是否禁止重复建批 | 是（`inflight` 为真时 `cloud_poll` 直接返回） | 不变 |
| reboot 后 | 全部重新 replay（`acked_seq` 归 0） | 不变，且空洞机制保证未确认内容**物理上仍存在** |

### 9.2 cloud queue overflow

| 项目 | 现状 | 修复后 |
|---|---|---|
| 淘汰策略 | FIFO 淘汰最旧，`cloud_q_drop++` | 不变；**在途窗口内不重复计 drop**（改计 `cloud_q_evict_inflight`） |
| 游标一致性 | ❌ 与 ACK 落账叠加会跳条（CRITICAL-1） | ✅ 绝对基准，不跳条 |
| WARN+ 的命运 | 移出队列、计 `drop_overflow`；本 Boot 不可恢复、跨 Boot 可恢复（§3.5） | 同左，**并登记空洞保护物理副本** |
| INFO 的命运 | 移出队列 ⇒ 永久丢失（有计数） | 不变（有计数，符合冻结契约） |

### 9.3 ACK 返回晚到

| 场景 | 现状 | 修复后 |
|---|---|---|
| 超时后、退避窗口内到达 | **IGNORE、永久丢弃**（HIGH-3） | 正常提交（依赖 `tx_valid`），并复位 retry/backoff |
| 重传后第一封 ACK 到达 | 正常提交 | 正常提交，且第二封 ACK 因 `tx_valid=false` 被忽略（**不重复推进**） |
| 提交后旧 ACK 到达 | IGNORE | IGNORE（不变） |
| 回退 / 重复 ACK | `DUPLICATE`，无副作用 | 不变 |
| 错误 `boot_seq` | IGNORE | 不变 |
| 无 `i` / 重复 `i` | ❌ 报文被拒（HIGH-2） | ✅ 正常进入分类器，由 `DUPLICATE` 语义消化 |

### 9.4 Flash segment reclaim

| 条件 | 现状 | 修复后 |
|---|---|---|
| 整段 ≤ ACK 水位 | ✅ `log_ack_segment_deletable()` | ✅ 保留 |
| **段内无未确认空洞** | ❌ **缺失**（CRITICAL-2） | ✅ `log_ack_segment_reclaimable()` 增加空洞相交判定 |
| 非当前追加目标 | ✅ `seg == s_append_segment` 跳过 | 不变 |
| replay 游标已越过 | ✅ 已有近似实现 | 不变（FIX-3 保证游标在段删除后不失效） |
| 段环压力淘汰的受害者 | ❌ 无条件最老段 | ✅ 优先已确认段；未确认段删除时计 `drop_unacked` |

### 9.5 give-up replay

| 项目 | 现状 | 修复后 |
|---|---|---|
| give-up 是否推进 ack 水位 | ✅ 否（GIVE_UP_NOT_ADVANCE 正确） | 不变 |
| `drop_unacked` 记账 | ✅ 计入 `n` | 不变 |
| 记录是否仍可 replay | ✅ 设计上可以 | ✅ **物理上也可以**（空洞阻止段回收） |
| 同 Boot 内是否重放 | ❌ 否（`give_up_seq` 排除，**刻意**防死循环） | 不变（刻意） |
| 跨 Boot 是否重放 | ✅ 是（`acked=0`、`give_up_seq=0`） | ✅ 是，且此前 CRITICAL-2 会先把它删掉 —— **本轮修复的核心价值** |

### 9.6 reboot recovery

| 项目 | 现状 | 修复后 |
|---|---|---|
| `acked_seq` / `give_up_seq` | RAM 态，重启归 0 | 不变 |
| 段内容 | Flash 持久 | 不变 |
| 未确认内容 | 理论上全部 replay | ✅ 实际全部 replay（不再被误删） |
| 空洞表 | 不存在 | **不持久化**（纯 RAM）—— 重启后由 `acked=0` 自然推导 |
| 是否需要 Flash 格式迁移 | — | **不需要**（段头 / `meta.bin` / `Record` 布局 / CBOR 键全部不变） |

---

## 10. 约束符合性核对

| 约束 | 状态 | 说明 |
|---|---|---|
| ACK 前不丢任何 WARN+ | ✅ 消除**静默**丢失 | 修复后 WARN+ 离开投递路径只剩三条**有账**通道：溢出淘汰（`drop_overflow` + 空洞保护）、give-up（`drop_unacked`）、段环压力（`drop_unacked`）。**有界队列下"零丢失"不可承诺**，但"零静默丢失"可承诺 |
| `GIVE_UP_NOT_ADVANCE` 后仍可重放 | ✅ | FIX-2 空洞阻止段回收 ⇒ 物理副本留存至下次 Boot |
| `cloud_send_log` 成功不能推进 | ✅ 复核后仍成立 | `s_cloud_q_rd` 修改点全集仍为 4 处（§2.1），**无一由发送成功触发**；`cloud_send_log()` 本身只做 `esp_mqtt_client_enqueue` 并返回 bool |
| 不增加大块 DRAM | ✅ **+143 B**（推荐变体）/ +13 B（最小变体） | 见 §11 |
| 不引入 mutex | ✅ | 沿用 `portMUX` 短临界区；空洞表与 `s_seg_last_seq` **仅 loopTask 访问**，无需加锁 |
| 不允许阻塞等待 | ✅ | 新增路径无 `delay` / `while` 等待 / 无重试死循环；FIX-3 的 `unacked` 计数为 O(1)（依赖 `s_seg_last_seq`），不引入 4 KB 读 |
| 保持 `log_events.h` ABI 不变 | ✅ **完全不触碰** | 无新增 EventId / ParamId / 常量；`LogRecord` 128 B 布局、CBOR 12 键、段头 16 B、`meta.bin` fmt v2 **全部不变** |

---

## 11. 资源与迁移影响汇总

| 修复 | 新增状态 | DRAM | PSRAM | 需迁移旧 Flash 格式？ |
|---|---|---|---|---|
| FIX-1 | `s_cloud_tx_rd_base` + 观测计数 | +8 B | 0 | **否** |
| FIX-2 | `s_cloud_holes[8]` + count + overflow | +68 B | 0 | **否**（纯 RAM，重启后重新推导） |
| FIX-3 | `s_seg_last_seq[16]` + 观测计数 | +68 B | 0 | **否**（由扫描重建） |
| FIX-4 | — | 0 | 0 | **否** |
| FIX-5 | `s_cloud_tx_valid` (+ 观测) | +5 B | 0 | **否** |
| **合计（推荐）** | | **+149 B** | **0** | **全部不需要** |
| 最小变体（FIX-3 用连续假设、FIX-2 表减到 4 项） | | **+45 B** | 0 | 全部不需要 |

预计静态 RAM：130,048 B → **≈130,200 B（仍为 39.7%）**。

**明确不变的部分**（对云端与既有设备数据零影响）：
`LogRecord` 128 B 布局 · 段头 16 B · `meta.bin` 32 B / fmt v2 · CBOR 批次 12 键 · MQTT 主题 `guo_feeder/{down,up,log}` · 命令/结果报文外壳 · Up/Down 协议语义。

---

## 12. 状态机图（修复后）

```
                    ┌─────────────── log_emit() ───────────────┐
                    ▼                                          │
            RAM 环 (64 × 128 B, PSRAM)                          │
                    │ log_task() 阶段1：routing（**不推进 s_rd**）│
        ┌───────────┴────────────┐                             │
        │ to_flash               │ to_cloud                    │
        ▼                        ▼                             │
   阶段2 append 单元        云待发队列 (128 × 128 B, PSRAM)     │
        │                        │                             │
   成功 │ 失败 → 保留环内，下轮重试    │ cloud_collect_batch()      │
        └──────────► 阶段3/4 只提交"已落盘前缀"                   │
                                 ▼                             │
                      在途批次 (≤16 条)                          │
                      ┌ FIX-1: 记录 s_cloud_tx_rd_base          │
                      └ FIX-5: s_cloud_tx_valid = true          │
                                 │                             │
                        cloud_send_log()  ← **只表示"已交给 MQTT"**
                        ★ 不推进 rd / 不推进 acked / 不删段 ★     │
                                 ▼
                     MQTT  guo_feeder/log
                                 │
                        云 → {c:"log_ack", p:{b,f,t}}  (down)
                                 │
                 cloud_process_rx_message()  ┌ FIX-4: 免 id、免去重缓存
                                 ▼
                 log_cloud_ack_callback()  （esp-mqtt 任务：**只置标志**）
                                 │
                    cloud_poll (1) cloud_handle_ack()
                    ┌ FIX-5: 用 s_cloud_tx_valid 匹配（而非 inflight）
                    ▼
              log_ack_classify() → ACCEPT / PARTIAL / DUPLICATE / IGNORE
                    │ ACCEPT | PARTIAL
                    ▼
         rd = rd_base + max(evicted, covered)      【FIX-1：绝对基准】
         acked_seq 前移 ; tx_valid = false ; retry = 0 ; backoff = 0
                    ▼
         cloud_delete_acked_segments()
           条件 = 整段 ≤ acked_seq
                ∧ **与空洞表不相交**                 【FIX-2 新增】
                ∧ 非当前追加目标
                ∧ replay 游标已越过
                    ▼
             flash_drop_segment()  ⇒ 回收

   旁路（均**不**推进 acked_seq）
   ├─ give-up            → rd = rd_base + max(evicted, n)
   │                       drop_unacked += n
   │                       **log_hole_add(tx_from, tx_to)**   【FIX-2】
   ├─ 队列溢出淘汰        → rd++（在途窗口内**不**计 drop_overflow）
   │                       Flash-routed ⇒ **log_hole_add(seq, seq)** 【FIX-2】
   └─ 段环压力（环满）    → 优先淘汰"已确认段"
                           否则 drop_unacked += unacked          【FIX-3】

   reboot ⇒ acked_seq=0 / give_up_seq=0 / 空洞表空
         ⇒ 全部未确认内容重新 replay（**自愈**）
```

---

## 13. 实施与测试计划

### 13.1 建议提交顺序（4 个独立可回退 commit）

| # | 内容 | 文件 | 备注 |
|---|---|---|---|
| **C1** | FIX-4：`log_ack` 短路线 `id` 与去重缓存 | `cloud_manager.cpp`、`cloud_protocol.md` | 独立、零 RAM、零风险，可先上 |
| **C2** | FIX-1 + FIX-5：队列绝对基准 + 描述符有效期 | `log_manager.cpp`、`log_manager.h` | 两者共享 `tx_valid` 门控，建议同批 |
| **C3** | FIX-2 + FIX-3 + D-1(minimal)：空洞感知回收 + 淘汰记账 + 游标复位 | `log_ack.h`、`log_manager.cpp`、`log_manager.h`、`probe_ack.cpp` | CRITICAL-2 修复主体 |
| **C4** | FIX-6 + D-1(推荐)：入队时机 + 段迭代式 replay（可选） | `log_manager.cpp` | 需重校既有用例 |

### 13.2 可选 FIX-6：入队时机（修 C-4）

把 `cloud_queue_push()` 从 `log_task()` 阶段 1 移到**阶段 3/4（提交之后）**，只推送已提交前缀 ⇒ 落盘失败被保留的记录不会被重复入队。
**代价**：Flash 长期不可用时 WARN+ 也不再上云（当前会入队但重复入队）。**若不愿承担该退化，替代方案**是在 `cloud_queue_push()` 内改为"扫描队列判重"（128 次 `seq` 比较，无 memcpy）。
**建议**：本批不做，单独评估（它不是 CRITICAL/HIGH 的直接修复项）。

### 13.3 测试补充

**离线（wasm32，无需硬件）** —— `test/log_contract/probe_ack.cpp` 新增：

| 用例 | 断言要点 |
|---|---|
| 空洞合并 | 相邻/重叠/被包含区间合并为一条；计数不增长 |
| 空洞表溢出 | 第 9 条无法合并 ⇒ 返回 false ⇒ `reclaimable()` 全 false |
| 回收判定（4 组合） | 段在空洞之前 / 之内 / 之后 / 相交 —— 只有"完全在空洞之后且 ≤ 水位"可回收 |
| 绝对基准推进 | 给定 `(rd_base, evicted, covered)` → 新 `rd` 精确匹配（含 `evicted > covered` 退化） |
| 重复 ACK 不重复推进 | 提交后 `tx_valid=false` ⇒ 第二封 ACK 判 IGNORE |

**上板（串口）**：

| 用例 | 期望 |
|---|---|
| T-OVF | 造满队列 + 在途批次 + ACK ⇒ `qused` 与 `sequence` 连续，无整条跳过 |
| T-HOLE | give-up → 后续 ACK → `fstats seg_del` **不增长**；重启后该段记录被 replay（`stats replay>0`） |
| T-EVICT | 写满 16 段后追加 ⇒ `fstats fail`/`drop_unacked` 增量 == 被删段中未确认条数 |
| T-LATE-ACK | `atimeout 300` + `sonline 1` → 超时后补 `ackauto 0` ⇒ `ack_ok` 增长、`retx` 不增长 |
| T-ACK-NOID | PC 端发**无 `i`** 的 `log_ack` ⇒ 被处理（`cstats ack_ok` 增长）；连发两条同 `i` ⇒ 第二条不报 duplicate |
| T-DEDUP | 高频 `log_ack` 后，用同一 `id` 重发一条真实命令 ⇒ **仍被去重拦截**（验证 C-1 修复） |

**既有用例必须重校**（⚠️ 实施者注意）：

| 文件 | 原因 |
|---|---|
| `test/log_flash_tests.txt` | FIX-3 改变受害者选择 ⇒ 环满段落的 `oldest=` / `total=466` 期望值可能变化 |
| `test/log_core_tests.txt` | 若采用 FIX-6，`cloud=` 类计数断言需重校 |
| `test/log_p15_ack_tests.txt` / `log_p15_giveup_tests.txt` | 当前为"校准草稿"，且 HIGH-1 修复后 `giveup` 段落需补 `seg_del` 不增长的断言 |

---

## 14. 未决问题（需人工确认后才进入编码）

| # | 问题 | 影响 |
|---|---|---|
| Q1 | 空洞表容量取 **8**（+64 B）还是 **4**（+32 B）？ | 影响 DRAM 与"溢出后停止回收"的出现概率 |
| Q2 | 是否接受"本 Boot 内空洞阻止其后所有段回收"这一保守代价？ | 若不可接受，需引入"ACK 时剔除已确认空洞"的增强（复杂度上升） |
| Q3 | FIX-3 受害者优先"已确认段"是否会破坏现有 Flash 压力测试的语义？ | 影响 `log_flash_tests.txt` 的期望值重校范围 |
| Q4 | `s_cloud_q_drop` 是否接受"不再统计在途淘汰"（改为 `cloud_q_evict_inflight`）？ | 这是对 `drop_overflow` 语义的一处**收紧/纠正**，需与云端契约定稿同步 |
| Q5 | FIX-6 是否纳入本批？（会改变入队时机与其退化行为） | 决定是否新增一次用例重校 |
| Q6 | `s_seg_last_seq[16]`（+64 B）是否采纳？还是先用连续假设近似？ | 决定 FIX-3 能否顺带消除 C-3 的提前回收风险 |

---

## 15. 一句话总结

5 个问题**全部真实、无需撤销**；但审计对 CRITICAL-1 的**后果低估了一档**（WARN+ 在本次 Boot 内也补不回），并遗漏了 **`log_ack` 挤占命令去重缓存（HIGH）** 与 **段内 seq 空洞假设（MEDIUM）** 两个同源问题。
推荐以 **5 个修复 + 1 个可选修复**、**+149 B DRAM / 0 PSRAM / 零 Flash 格式迁移** 的代价收口；其中 **FIX-2（空洞登记）是与冻结契约冲突最小、收益最大的一个** —— 它把"GIVE_UP_NOT_ADVANCE 可重放"从**文档承诺**变成**物理保证**，且不引入任何持久化格式变更。
