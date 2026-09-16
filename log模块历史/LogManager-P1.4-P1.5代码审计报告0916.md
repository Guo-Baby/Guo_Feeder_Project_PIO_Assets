# LogManager P1.4/P1.5 Code Audit Report

> 审计对象：`src/log_manager.cpp` / `src/log_manager.h` / `src/log_ack.h` /
> `src/log_cbor.h` / `src/cloud_manager.cpp` / `src/cloud_manager.h` / `src/log_events.h`
> 代码基线：`git 4fdc1b2`（P1.5 提交）
> 审计方式：**逐段实读源码**，不依赖设计文档或提交说明
> 本次审计**未修改任何代码、未提交任何 commit**

---

## 1. 总体结论

**PASS WITH RISKS**

核心模型成立：**「发送成功」确实不等于「推进」** —— `s_cloud_q_rd` 的推进点全集为
`cloud_handle_ack()`（ACK 落账）、`cloud_give_up_batch()`（放弃）、
`cloud_queue_push()`（队列溢出淘汰）、测试复位。**没有任何一条路径是
`cloud_send_log() == true` 就删除记录或推进水位。**

但存在 **2 个 CRITICAL 级缺陷**，都会造成**未确认日志的静默丢失**，
且都发生在需求明确要求覆盖的高负载 / 云无响应场景：

| 编号 | 一句话 |
|---|---|
| **CRITICAL-1** | 云队列溢出时淘汰指针先动，随后 ACK 落账再按"本批条数"推进 → **游标越过一条从未发送的记录** |
| **CRITICAL-2** | Flash 段回收用**单一水位**判断"整段已确认" → 段内存在从未 ACK 的空洞（give-up / 队列溢出）时被整段删除，**违反 `GIVE_UP_NOT_ADVANCE` 的"留待下次开机重放"承诺** |

另有 3 个 HIGH、4 个 MEDIUM、8 个 LOW。

---

## 2. CRITICAL 问题

### CRITICAL-1 —— 队列淘汰与 ACK 落账叠加 → 游标跳过未发送记录

```cpp
// src/log_manager.cpp:1595-1605  cloud_queue_push()
if (cloud_queue_used_locked() >= LOG_CLOUD_QUEUE_SLOTS)
{
    s_cloud_q_rd++;              // ← 无条件推进，不看是否有在途批次
    s_stats.cloud_q_drop++;
}
```

```cpp
// src/log_manager.cpp:1815-1825  cloud_handle_ack()
s_cloud_q_rd += covered;         // covered = 本批被覆盖条数（≤ tx_count）
```

**问题**：在途批次是在 `rd = R` 时收集的（记录位于逻辑下标 `R..R+15`）。
批次在途期间若新记录入队导致队列满，淘汰把 `rd` 推成 `R+1`；
ACK 到达后再 `rd += 16` → `rd = R+17`。
**逻辑下标 `R+16` 的那条记录既没被发送，也没被计数，被直接跳过。**

**后果**：
- `INFO`（Flash NO）→ **永久丢失**，无任何副本、无任何计数；
- `WARN/ERROR/CRITICAL` → Flash 有副本，理论上可由 replay 补回，
  但受 CRITICAL-2 影响，段可能已被删除。

**触发条件**：云队列达到 128 条上限且此时存在在途批次。
即需求 §“高负载：连续 WARN / 连续 ERROR” 明确要求覆盖的场景；
离线时间较长时（队列持续积压）同样成立。

**严重原因**：这是**未计数**的丢失 —— 违反本项目的核心不变量思想
（"已落盘 + 环内保留 == 已路由"），也违反 §27 的 `drop_overflow` 记账纪律
（`cloud_q_drop` 只记了淘汰 1 条，实际语义上丢了 2 条的发送机会）。

---

### CRITICAL-2 —— Flash 段回收的水位近似 → 删除含未确认记录的段

```cpp
// src/log_ack.h  log_ack_segment_deletable()
if (records == 0u) return false;
const uint32_t last_seq = first_seq + (uint32_t)records - 1u;
return last_seq <= acked_seq;      // ← 只看"末条"，不校验"每条都被 ACK 过"
```

```cpp
// src/log_manager.cpp  cloud_delete_acked_segments()
if (!log_ack_segment_deletable(s_seg_first_seq[seg], s_seg_records[seg],
                               s_cloud_acked_seq)) continue;
if (flash_drop_segment(seg)) { ... }
```

**问题**：`s_cloud_acked_seq` 是**单点高水位**（= 被 ACK 前缀的末条 seq），
不是"每条都已确认"的集合。当 seq 序列出现**空洞**时，水位会**跨过**未确认记录：

```
时间线：
  S1 .. S16   一个批次，重试耗尽 → GIVE_UP_NOT_ADVANCE
              （ack 水位不动、段保留 —— 这是承诺）
  S17 .. S32  后续批次
  ...
  S31 被 ACK  →  acked_seq = 31
  段 0 = S1..S31  →  last_seq(31) <= acked_seq(31)  →  整段被删除
  ⇒ S1..S16 这些**从未被 ACK** 的记录从 Flash 永久消失
```

**空洞的两种真实来源**（都不是假设）：
1. **`GIVE_UP_NOT_ADVANCE`**（重试耗尽）—— 记录被移出 RAM 云队列但仍在 Flash，
   设计意图正是"下次开机重放"；
2. **云队列溢出淘汰** `drop_overflow`（CRITICAL-1 的同源场景）—— 记录未发送过。

**后果**：`GIVE_UP_NOT_ADVANCE` 的"记录留待下次开机重放"承诺**不成立**；
重启后 `acked_seq` 归 0 也救不回来 —— **段文件已经没了**。

⚠️ 同时说明：**P1.5 的提交说明与交付报告里写的
"GIVE_UP_NOT_ADVANCE：ack 水位与 Flash 段都不推进（记录留待下次开机重放）"
在实现层面只对了前半句**（当时确实不删），后半句被本缺陷推翻。

**附注（部分覆盖是安全的）**：用户关心的"partial ACK 不得删段"这一条**成立**——
段容量 31 条，未被覆盖的尾部与已覆盖前缀同段时，段末 seq 必然 > 水位 → 不删。
风险只来自**空洞**，而非部分覆盖本身。

---

## 3. HIGH 风险

### HIGH-1 —— 段环满 FIFO 淘汰未计 `drop_unacked`

```cpp
// src/log_manager.cpp  flash_ensure_append_target()
if (target >= LOG_SEGMENT_COUNT)
{
    const uint32_t victim = s_oldest_segment;
    if (!flash_drop_segment(victim)) return false;   // ← 只删，不计 drop_unacked
    target = victim;
}
```

`flash_drop_segment()` 只 `flash_segment_deleted++`，**不动 `s_stats.cloud_flash_drop`**。

**后果**：段环满（496 条）时被淘汰的最老段里的**未确认**记录，
在**从未尝试发送**的情况下被销毁，且**云端永远不会知道**（`drop_unacked`
是批次头里唯一能表达"我丢过未确认数据"的通道）。
需求 §20 明确要求"环满 FIFO 淘汰并累计 `drop_overflow` / `drop_unacked`"。
**这是与冻结契约的直接冲突。**

---

### HIGH-2 —— `log_ack` 会被 `id` 校验与去重过滤器拦截（旁路不完整）

```cpp
// src/cloud_manager.cpp:1054-1073
bool compact = !doc["c"].isNull();
const char* c = compact ? doc["c"] : doc["cmd"];
if(c == nullptr) { ... return; }
String cmd_id = compact ? (doc["i"] | "") : (doc["id"] | "");
if(cmd_id.length() == 0) { Serial.println("[Cloud] Missing id"); return; }   // ← ①
if(cloud_check_duplicate_cmd(cmd_id))     { ... return; }                    // ← ②
...
if(strcmp(c, LOG_ACK_COMMAND) == 0)       { ... }        // log_ack 分支在这之后
```

**问题**：`log_ack` 虽然"旁路 CommandManager"，但**没有旁路 `id` 必需性与去重缓存**：

- ① 云端若发 `{"c":"log_ack","p":{...}}`（无 `i`）→ 被丢弃，ACK 静默失效；
- ② 云端若用**相同 id** 重发 ACK（很自然的实现方式）→ 第二次被当重复命令丢弃。

**后果**：ACK 收不到 → 15 s 超时 → 5 次重试 → give-up → 记录被移出 RAM 云队列；
对 `INFO` 而言即永久丢失。链条很长但每一环都不罕见。

**对比**：`change_msg_limit` 同样走旁路，但它同样受这两个过滤器约束，
所以这是**既有模式**，不是 P1.5 新引入 —— 但对 log_ack 是致命的。

---

### HIGH-3 —— 非在途时刻到达的 ACK 被无条件丢弃

```cpp
// src/log_manager.cpp  cloud_handle_ack()
const LogAckResult r = log_ack_classify(
    ack_boot, ack_from, ack_to,
    s_cloud_inflight ? s_cloud_tx_boot : 0u,
    s_cloud_inflight ? s_cloud_tx_from : 0u,
    s_cloud_inflight ? s_cloud_tx_to   : 0u,     // ← 非在途时传 0
    s_cloud_acked_seq);
```

`log_ack_classify()` 对 `tx_boot == 0 || tx_to == 0` 直接返回 `IGNORE`。

**问题**：超时后代码会 `s_cloud_inflight = false` 并进入**退避窗口**
（2/4/8/16/32 s，上限 60 s）。这个窗口内到达的 ACK **被丢弃且不留下任何痕迹**，
随后重新发送同一批次。若云端对"重复批次"不再回 ACK（幂等实现常见做法），
设备将重试 5 次后 give-up → `INFO` 永久丢失。

**根因**：把"是否在途"当成了"是否存在待确认批次"。
`cloud_try_send_batch()` 一旦成功就会设置 `s_cloud_tx_*`，
但超时路径把它当作"无在途"而丢弃了上下文。

---

## 4. MEDIUM 风险

### MEDIUM-1 —— 未来 ACK 被误判为 PARTIAL 并提交整批

```cpp
// src/log_ack.h  log_ack_classify()
if (ack_to <= acked_seq) return LOG_ACK_DUPLICATE;
if (ack_to < tx_from)    return LOG_ACK_IGNORE;      // 只挡了"完全在过去"
if (ack_from <= tx_from && ack_to >= tx_to) return LOG_ACK_ACCEPT;
return LOG_ACK_PARTIAL;                              // ← ack_from > tx_to 落到这里
```

若收到 `ack_from > tx_to`（区间完全在在途批次**之后**，例如别的来源、
云端 bug、被拼接的报文），判定为 PARTIAL，而
`log_ack_covered_count()` 会数出**全部 16 条**（因为每条的 seq 都 ≤ ack_to）
→ 整批被提交、水位被推进。**缺少 `ack_from > tx_to → IGNORE` 规则。**

测试覆盖缺口：`probe_ack.cpp` 测了 `ack_to < tx_from`，**没测 `ack_from > tx_to`**。

### MEDIUM-2 —— 云队列入队发生在提交判定之前 → 重复入队

```cpp
// src/log_manager.cpp  log_task() 阶段 1 的 I/O 区
if (to_flash) { ...若不能接收则 blocked++; break... }
if (to_cloud) { cloud_queue_push(rec); }   // ← 此时尚不知 Flash 是否成功
```

保留待重试的记录下一轮会被再次 `cloud_queue_push()`，而去重守卫
只比较"**上一次**入队的那一条"：

```cpp
if (rec.boot_seq == s_cloud_q_last_boot && rec.seq == s_cloud_q_last_seq) return;
```

若一轮里被保留的是**多条**（`flash_append_batch` 部分成功即可造成），
则下一轮重新入队时，第一条已 ≠ "上一次入队的那条"（那条是上一轮的末条）
→ **逐条重新入队，形成两份副本**。

**后果**：云端收到重复日志（协议层可容忍 —— 幂等键 `(device_id,boot_seq,seq)`
正是为此设计），但云队列被无效占满，加剧 CRITICAL-1 的触发概率。

### MEDIUM-3 —— PSRAM/DRAM 回退统计不正确（审计 §9 明确问项）

```cpp
// src/log_manager.cpp  cloud_init_buffers()
void *mem  = heap_caps_malloc(q_bytes, MALLOC_CAP_SPIRAM);   // 队列
if (mem != nullptr) psram = true; else { mem = ..._8BIT; psram = false; }
...
void *bmem = heap_caps_malloc(b_bytes, MALLOC_CAP_SPIRAM);   // 批次：不回填标记
if (bmem == nullptr) bmem = heap_caps_malloc(b_bytes, MALLOC_CAP_8BIT);
...
void *cmem = heap_caps_malloc(LOG_BATCH_MAX_PAYLOAD, MALLOC_CAP_SPIRAM); // CBOR：同样不回填
...
s_cloud_buf_in_psram = psram;                                // ← 只反映"队列"
Serial.printf(... (s_cloud_buf_in_psram && bmem != nullptr) ? "PSRAM" : "mix");
```

**问题**：
1. `s_cloud_buf_in_psram` 只表示**队列**的落点，却被用于打印**批次**缓冲的落点；
   只要队列在 PSRAM，打印就是 `PSRAM`，即使 `bmem` 回退到了 DRAM；
2. **CBOR 缓冲（4 KB）的落点完全没有被记录**，`LogCloudInfo` 也无法观测；
3. 三个分配是**独立**的，可能出现"队列 PSRAM + 批次 DRAM + CBOR PSRAM"的混合态，
   而对外只报一个字符串。

**影响**：验收时无法判断是否真的全部命中 PSRAM（而项目内存铁律恰恰要求可观测）。

### MEDIUM-4 —— 离线期间队列溢出会使 INFO 静默消失，且云端不可知

离线时 `cloud_poll()` 第 ④ 步直接返回、不消费记录 → 云队列（128 槽）持续增长
→ 溢出后按 FIFO 淘汰。`INFO` 无 Flash 副本 ⇒ **离线超过 128 条 INFO 即永久丢失**。
`drop_overflow` 只能搭"下一批"顺风车上报，而离线期间根本发不出批次，
⇒ **云端在恢复后才可能知道，且无法补回**。（§14 已把"有意限流"写进设计，
但"何时告诉云端"这一环在长离线场景下实际失效。）

---

## 5. 具体代码位置

### CRITICAL-1

```
文件:   src/log_manager.cpp
函数:   cloud_queue_push()  (≈1595-1605)
        cloud_handle_ack()  (≈1815-1825)
        cloud_give_up_batch() (≈1994-2000)
问题:   淘汰推进 rd 与 ACK 落账 rd += covered 相互独立，叠加后越过未发送记录
原因:   在途批次的起始逻辑下标没有被记录，也没有"在途期间禁止淘汰"的约束
建议:   ① 记录在途批次的起始下标 s_cloud_inflight_rd；
        ② 最简方案：在途期间禁止淘汰（满则拒绝入队并计 drop），
           即 cloud_queue_push() 中 `if (inflight) { drop; return; }`；
        ③ 或 ACK 落账改为"绝对定位"：`rd = inflight_rd + covered`（而非 `rd += covered`）
```

### CRITICAL-2

```
文件:   src/log_manager.cpp
函数:   cloud_delete_acked_segments()
        src/log_ack.h  log_ack_segment_deletable()
问题:   用单一水位 acked_seq 判断"整段已确认"，无法识别段内空洞
原因:   水位只能表达"最高连续确认点"这一近似；give-up / 队列溢出会产生空洞，
        且这两类记录**故意**留在 Flash 中待重放
建议:   ① 增加"段内未确认水位"约束：删除条件追加
           `s_cloud_give_up_seq < s_seg_first_seq[seg]`
           （即该段起点晚于已放弃水位）；
        ② 更严格：为每段维护"已确认条数"，仅当 == records 才删；
        ③ 或把 give-up 的区间记入一张小的"未确认区间表"，删除前逐个排除
```

### HIGH-1

```
文件:   src/log_manager.cpp
函数:   flash_ensure_append_target()（环满分支）
问题:   淘汰最老段时未累计 drop_unacked
原因:   flash_drop_segment() 是"纯删除"原语，不含业务记账；调用方漏记
建议:   环满淘汰前计算 victim 的 records，并按"未确认条数"累加
        s_stats.cloud_flash_drop；或提供 flash_drop_segment_evict()
        由它统一记账
```

### HIGH-2

```
文件:   src/cloud_manager.cpp
函数:   cloud_process_rx_message()  (≈1054-1105)
问题:   log_ack 仍被 "Missing id" 与 cloud_check_duplicate_cmd() 约束
原因:   旁路只绕过了 CommandManager，未绕过 id 校验与去重缓存
建议:   把 log_ack 分支**前移**到 cmd_id 校验之前（照 c == "log_ack" 先判），
        或在文档中把"log_ack 必须携带全局唯一 i"写成硬契约并让云端遵守
```

### HIGH-3

```
文件:   src/log_manager.cpp
函数:   cloud_handle_ack()
问题:   非在途时刻（退避窗口内）到达的 ACK 被判 IGNORE 并丢弃
原因:   classify 的入参用 s_cloud_inflight 三元表达式，把"无在途"与
        "无待确认批次"混为一谈
建议:   只要 s_cloud_tx_count != 0（存在已发出待确认批次）就传入 tx_*，
        与 inflight 解耦；或退避期间保持 inflight 为真
```

### MEDIUM-1

```
文件:   src/log_ack.h
函数:   log_ack_classify()
问题:   缺少 ack_from > tx_to → IGNORE
原因:   只处理了"完全在过去"（ack_to < tx_from），未处理"完全在未来"
建议:   在 ⑤ 之后追加 `if (ack_from > tx_to) return LOG_ACK_IGNORE;`
        并在 probe_ack.cpp 补一条断言（当前为测试缺口）
```

### MEDIUM-2

```
文件:   src/log_manager.cpp / cloud_queue_push()
函数:   log_task() 阶段 1 I/O 区；cloud_queue_push() 去重守卫
问题:   入队早于提交判定；去重只比对"上一条"
原因:   守卫用"相邻相等"近似"本轮已入队集合"
建议:   把 cloud_queue_push() 移到阶段 4，仅对 commit 前缀内的记录入队
        （一次入队、零重复，且天然保证"只发已消费记录"）
```

### MEDIUM-3

```
文件:   src/log_manager.cpp
函数:   cloud_init_buffers()
问题:   PSRAM 落点统计张冠李戴；CBOR 缓冲落点未记录
原因:   单一 bool 复用于三个独立分配
建议:   三个分配各自记标记（q_psram / b_psram / cbor_psram），
        LogCloudInfo 增加对应字段，打印按各分配分别输出
```

### MEDIUM-4

```
文件:   src/log_manager.cpp
函数:   cloud_poll() 第 ④ 步（离线跳过）
问题:   离线期间队列溢出淘汰 INFO，云端不可知、不可补
原因:   drop 计数只随批次头上报，离线时无批次
建议:   契约层明确"INFO 在离线期间不保证"；或给 INFO 开一个很小的
        Flash 窗口（需先改契约，不宜擅自实现）
```

### LOW（汇总）

```
LOW-1  src/cloud_manager.cpp: cloud_mqtt_publish_binary() 的 qos 形参被忽略
       （实际使用编译期 CLOUD_MQTT_QOS）；cloud_send_log(..., 1) 具误导性。
       另：P1.4 的提交说明/文档写 "store=0"，实际 CLOUD_MQTT_STORE=true —— 
       文档与实现不符（但对 PSRAM 安全是**有利**的，见 §9 结论）。
LOW-2  src/cloud_manager.h: enum CloudRoute 定义后**从未被引用**（死代码）。
LOW-3  src/log_manager.cpp: flash_flags 为 uint8_t 且用 1u << scanned；
       当前 LOG_DRAIN_MAX_PER_TASK == LOG_FLUSH_RECORDS == 8 恰好安全，
       但两者无 static_assert 绑定，任一被调大会立即越界/UB。
LOW-4  src/cloud_manager.cpp: mqtt_connected 为普通 bool，由 esp-mqtt 任务写、
       loop 任务经 cloud_is_connected() 读；单字节读写实践中安全，形式上为数据竞争。
LOW-5  src/log_manager.cpp: log_cloud_get_info() 读 s_cloud_retry_n / s_cloud_gave_up
       等未加 s_mux（仅观测用途）。
LOW-6  src/log_cbor.h: log_cbor_put_* 无逐次边界检查，安全性完全依赖
       log_cbor_encode_batch() 的预检；后续若被直接调用即可能越界。
LOW-7  src/log_manager.cpp: s_cloud_gave_up 只写不读（除观测），语义冗余。
LOW-8  test/mqtt_log_probe.py: ack 的 id 用 "ack<秒>"，同一秒内连发两次会被
       设备判为重复命令丢弃 —— 自测时会表现为"ACK 无效"。
```

---

## 6. 状态机图（按实际代码绘制）

```
                        log_emit()
                            │  入 RAM 环（64×128B, PSRAM）
                            ▼
                    ┌───────────────┐
                    │  RAM Ring     │  log_task() 四阶段
                    └───────┬───────┘
        阶段1 窥视(不推进 s_rd)│  阶段2 append 单元 → 阶段3 按"已落盘前缀"提交
                            │  阶段4 s_rd += commit
                            ├──────────────► Flash 段环
                            │                /log/s%07u.log  16×31 = 496 条
                            │                     ▲        │
                            │                     │        │ ① ring 满 → 淘汰最老段
                            │                     │        │    ⚠ HIGH-1 未计 drop_unacked
                            │                     │        │
                            │                     │        │ ② 整段 last_seq <= acked → 删
                            │                     │        │    ⚠ CRITICAL-2 空洞会误删
                            │                     │        │
                            │              replay  │        │
                            │        (仅 used==0 时)│        │
                            │                     │        ▼
                            └──to_cloud──►┌──────────────────┐
                                          │ Cloud Pending Q  │ 128×128B (PSRAM)
                                          │ rd / wr          │
                                          └────────┬─────────┘
                                    满则 rd++ ⚠CRITICAL-1
                                                   │ cloud_collect_batch()
                                                   │ （≤16 条，同一 boot_seq）
                                                   ▼
                                          ┌──────────────────┐
                                          │  inflight batch  │  tx_boot/from/to/count
                                          │  + deadline      │  ⚠ HIGH-3 退避期间上下文丢失
                                          └────────┬─────────┘
                                                   │ cloud_build_cbor() 12 项定序数组
                                                   │ cloud_send_log() → log_topic
                                                   │ （esp_mqtt enqueue, store=true **拷贝**）
                                                   ▼
                                              ┌─────────┐
                                              │  MQTT   │  guo_feeder/log
                                              └─────────┘
                                                   │
                        guo_feeder/down ◄──────────┘
                        {"c":"log_ack","i":u,"p":{"b":..,"f":..,"t":..}}
                                │
                                │ ⚠ HIGH-2 需唯一 i，且过"重复命令"过滤器
                                ▼
                   cloud_process_rx_message() → s_log_ack_cb()
                                │（仅置标志，MQTT 任务上下文）
                                ▼
                        cloud_handle_ack()  ← 只在 loop 上下文落账
                                │
                ┌───────────────┼────────────────┬───────────────────┐
                ▼               ▼                ▼                   ▼
          ACCEPT/PARTIAL    DUPLICATE        IGNORE             （超时 → 退避）
          rd += covered     无变化       boot 不符/区间非法      2/4/8/16/32→60s
          acked = 覆盖前缀                                       5 次后 give-up
                │                                              rd += n
                ▼                                              drop_unacked += n
        cloud_delete_acked_segments()                          ⚠ CRITICAL-2 之后段仍可能被删
```

---

## 7. 审计项逐条结论

| 审计项 | 结论 |
|---|---|
| §1 发送成功 == 推进？ | ✅ **否**，rd 只在 ACK / give-up / 淘汰 / 复位 时推进（见 §1 推进点全集） |
| §1 inflight 独立生命周期 | ✅ 有（flag + tx_* + deadline） |
| §1 ACK 前禁止重建同一批次 | ✅ 在途时第 ② 步直接返回；超时后重取同一批（rd 未动）——⚠ 但有淘汰漂移 |
| §1 ACK 前保留发送数据 | ✅ RAM 队列保留（rd 未推进）——⚠ 被淘汰的那些不保留 |
| §1 reboot 后可重新发送 | 🟡 WARN+ 可以（Flash replay）；**INFO 不行**（无 Flash 副本，RAM 队列随重启丢失）——设计使然 |
| §2 partial ACK 只推进前缀 | ✅ 正确（`acked = records[covered-1].seq`，不会跳到 104） |
| §2 duplicate / rollback | ✅ 无状态变化（记 `cloud_ack_ignored`） |
| §2 wrong boot_seq | ✅ IGNORE |
| §2 invalid range | ✅ IGNORE |
| §2 **future ACK** | ❌ **MEDIUM-1**：落进 PARTIAL 并提交整批 |
| §3 删段三条件 | ❌ **CRITICAL-2**：条件 1 用**水位近似**，空洞场景不成立 |
| §3 partial ACK 不删段 | ✅ 成立（31 条/段，未覆盖尾部与前缀同段 ⇒ 段末 seq > 水位） |
| §4 give-up 只移出 RAM + drop_unacked++ | ✅ 就本函数而言正确（不动 acked、不删 Flash）——⚠ 但事后可能被 CRITICAL-2 删段 |
| §4 重启后可 replay | 🟡 WARN+ 取决于 CRITICAL-2 是否已删段；INFO 不可 replay |
| §5 retry / timeout / backoff 存在 | ✅ 15 s / 5 次 / 2-4-8-16-32 上限 60 s（`log_ack.h` 已由探针穷举 14 项） |
| §5 无 while-retry / delay | ✅ grep 确认新增路径无 `delay()`、无无界 `while` |
| §6 依赖方向 LogManager → CloudManager | ✅ 单向，`cloud_manager.*` 不 include `log_manager.h`；`CloudRoute` 为死代码（LOW-2） |
| §6 cloud_send_log 只发送 | ✅ 不删日志、不推进状态 |
| §7 字段名一致性 | ✅ 设备 `b/f/t` == PC 工具 `b/f/t`；⚠ 但**必须有唯一 `i`**（HIGH-2） |
| §8 CBOR 无 malloc/new/String/ArduinoJson | ✅ 全部满足（仅字节循环拷贝） |
| §8 越界保护 | ✅ 空指针 / count 超限 / 容量不足 三重预检；实际最大 2101 B < 4096 B 缓冲 |
| §9 PSRAM 分配成功检测 + DRAM 回退 | ✅ 三处都有回退 —— 但**回退统计不正确**（MEDIUM-3） |
| §9 MQTT publish 引用 PSRAM 是否安全 | ✅ **安全，但仅因 `CLOUD_MQTT_STORE=true` 会在入队时拷贝**（`esp_mqtt_client_enqueue`）。⚠ 若改为 `store=false`，CBOR 缓冲会在下一次组包时被覆写 → 发送脏数据（HIGH 隐患，当前不成立） |
| §10 静态 RAM | +240 B（129,808 → 130,048），来自 P1.4/P1.5 的 ~20 个静态标量 |
| §10 PSRAM | +22,528 B（队列 16,384 + 批次 2,048 + CBOR 4,096），一次性分配、无运行期 churn |
| §10 大局部数组 / 栈风险 | ✅ 新增路径最大局部为 `LogRecord rec`（128 B）；无 `uint8_t x[4096]` 类栈数组 |
| §10 heap 碎片 | ✅ 三个缓冲只在 `log_init()` 分配一次，运行期不 free/alloc |

---

## 8. 测试覆盖审查（不看 PASS，只看覆盖）

### 已覆盖（wasm32 真实执行，50 项）

| 项 | 覆盖 |
|---|---|
| partial ACK | ✅ `log_ack_covered_count` 6 项 + classify PARTIAL 3 项 |
| duplicate ACK | ✅ classify 2 项（含"回退优先于更早批次"的优先级固化） |
| invalid range | ✅ 6 项 |
| boot_seq 不匹配 | ✅ 1 项 + `log_ack_backoff_ms_ex` 一致性 4 项 |
| 退避序列 / 上限 | ✅ 10 项（2/4/8/16/32/60 s、clamp、溢出） |
| 放弃门槛 | ✅ 4 项 |
| 可重放水位（含 give_up） | ✅ 7 项 |
| 段可删性 | ✅ 7 项 |

### 未覆盖（**本次审计新发现，全部与 CRITICAL/HIGH 对应**）

| # | 未覆盖项 | 对应缺陷 |
|---|---|---|
| 1 | **淘汰 + ACK 叠加时的游标漂移** | CRITICAL-1 |
| 2 | **give-up 后 acked 水位越过该段 → 段是否被删** | CRITICAL-2 |
| 3 | **future ACK（`ack_from > tx_to`）** | MEDIUM-1 |
| 4 | **环满淘汰的 drop_unacked 记账** | HIGH-1 |
| 5 | **log_ack 无 `i` / 重复 `i` 的下行路径** | HIGH-2 |
| 6 | **退避窗口内到达 ACK** | HIGH-3 |
| 7 | reboot after send before ACK（设备侧） | ⛔ 无开发板 |
| 8 | offline retry exhausted（设备侧） | ⛔ 无开发板 |
| 9 | Flash full + ACK reclaim（设备侧端到端） | ⛔ 无开发板 |
| 10 | cloud queue overflow（设备侧端到端） | ⛔ 无开发板 |
| 11 | PSRAM 分配失败路径（强制回退） | 无注入钩子 |

> 注：已有 50 项**全部是纯函数判定**，它们全部为真 —— 缺陷都出在
> **判定与执行之间的簿记**（游标、记账、上下文传递），
> 这正是"纯逻辑探针"覆盖不到的地方。

---

## 9. 建议下一步（仅建议，未修改任何代码）

**按修复优先级**：

1. **CRITICAL-1** —— 在途期间禁止淘汰（或 ACK 落账改为绝对定位 `rd = inflight_rd + covered`）。
   这是唯一一条会造成"**未发送即丢失且不计数**"的路径，且触发条件是需求明确点名的高负载场景。
2. **CRITICAL-2** —— 删段条件追加"段起点 > `give_up_seq`"（最小改动），
   或为每段维护"已确认条数"（更严格）。**在修掉之前，
   `GIVE_UP_NOT_ADVANCE` 的"留待下次开机重放"承诺不得对外宣称。**
3. **HIGH-2** —— 把 log_ack 分支前移到 `id` 校验之前；或把"`i` 必须全局唯一"
   写进 `cloud_protocol.md` 成为对云端的硬契约。
4. **HIGH-1 / HIGH-3** —— 记账补全与"待确认批次"上下文解耦（各一处小改）。
5. **MEDIUM-1** —— 补 `ack_from > tx_to → IGNORE` + 一条探针断言（同时补上测试缺口）。
6. **MEDIUM-3** —— 三个缓冲各自记录落点，恢复 PSRAM 验收的可观测性。
7. **文档纠偏** —— P1.4 提交说明中的 "store=0" 与实际 `CLOUD_MQTT_STORE=true` 不符；
   P1.5 报告中"GIVE_UP 后记录留待下次开机重放"在 CRITICAL-2 修复前不成立。

**流程建议**：
- 本次审计发现的 1–5 项都属于"**簿记类**"缺陷，
  建议在补测试时**同时**加上"设备侧端到端"用例（GC 与淘汰的交互只有真机能暴露）；
- 建议为 `log_cbor.h` 那样把**队列/段回收的簿记逻辑**也抽成可主机测试的纯函数
  （例如"给定 wr/rd/inflight_rd/covered/evictions，返回新的 rd"），
  这样 CRITICAL-1 这类缺陷就能在无硬件时被穷举。
