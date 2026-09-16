// =====================================================
// LogManager P1.5 —— ACK / Retry / Offline 的**纯决策逻辑**
//
// 为什么单独成头文件：
//   · 只依赖 <stdint.h> + log_events.h，**不依赖 Arduino / LittleFS / MQTT**
//     ⇒ 可用主机编译器编到 wasm32 并由 node 真实执行（见
//       test/log_contract/probe_ack.cpp）。
//   · 本设备在开发期长期没有可用的 MQTT 对端，ACK 语义若只写在
//     log_manager.cpp 里就完全无法验证。把"判定"与"执行"分开后，
//     判定部分可以被穷举测试，log_manager.cpp 只负责取数、调这里、落账。
//
// 冻结语义（LogManager-P1契约冻结0915.md §13）
//   · ACK = 云端已持久化该 seq 区间，**不是**"以后不会再收到"
//   · 全系统 at-least-once；云端按 (device_id, boot_seq, seq) 幂等去重
//   · 重复 ACK → 无变化；回退 ACK → 忽略；错误 boot_seq → 忽略
//   · 部分覆盖 → 只能推进被覆盖的前缀，**不得**删除仍含未确认 record 的段
//   · 重试耗尽 → GIVE_UP_NOT_ADVANCE：不推进 ack 水位、不删段
//   · 退避 2 / 4 / 8 / 16 / 32 s，上限 60 s
// =====================================================

#ifndef LOG_ACK_H
#define LOG_ACK_H

#include <stdint.h>

#include "log_events.h"

// ACK 判定结果
enum LogAckResult
{
    LOG_ACK_IGNORE    = 0,   // 与本设备/在途批次无关，或区间非法
    LOG_ACK_DUPLICATE = 1,   // 回退 / 重复确认：无变化（不是错误）
    LOG_ACK_ACCEPT    = 2,   // 完整覆盖在途批次
    LOG_ACK_PARTIAL   = 3    // 只覆盖前一部分（其余需重发）
};

// 退避基数（首次超时后等待 2 s）；序列 2/4/8/16/32 s，上限 60 s
#define LOG_ACK_BACKOFF_BASE_MS 2000u

// -----------------------------------------------------
// ACK 区间校验：from <= to 且都非 0 才算合法
// （seq 从 1 开始分配，0 表示"未分配"，因此 0 视为非法）
// -----------------------------------------------------
static inline bool log_ack_range_valid(uint32_t seq_from, uint32_t seq_to)
{
    if (seq_from == 0u || seq_to == 0u)
    {
        return false;
    }
    return seq_from <= seq_to;
}

// -----------------------------------------------------
// 分类一条 log_ack
//
//   ack_boot            云端回执里的 boot_seq
//   ack_from/ack_to     云端确认的 seq 区间（含两端）
//   tx_boot             在途批次的 boot_seq（无在途时传 0）
//   tx_from/tx_to       在途批次的 seq 区间（无在途时传 0/0）
//   acked_seq           本 boot 内已经推进到的确认高水位（0 = 尚无）
//
// 判定顺序（顺序本身是语义的一部分）：
//   ① 区间非法                → IGNORE
//   ② 无在途批次              → IGNORE
//   ③ boot_seq 不匹配          → IGNORE（重启前/其他设备的回执）
//   ④ ack_to <= acked_seq     → DUPLICATE（回退或重复，无变化）
//   ⑤ ack_to < tx_from        → IGNORE（确认的是更早的批次，与在途无关）
//   ⑥ 完整覆盖 [tx_from,tx_to] → ACCEPT
//   ⑦ 其余                    → PARTIAL
// -----------------------------------------------------
static inline LogAckResult log_ack_classify(
    uint32_t ack_boot, uint32_t ack_from, uint32_t ack_to,
    uint32_t tx_boot, uint32_t tx_from, uint32_t tx_to,
    uint32_t acked_seq)
{
    if (!log_ack_range_valid(ack_from, ack_to))
    {
        return LOG_ACK_IGNORE;
    }

    if (tx_boot == 0u || tx_to == 0u)
    {
        return LOG_ACK_IGNORE;
    }

    if (ack_boot != tx_boot)
    {
        return LOG_ACK_IGNORE;
    }

    if (ack_to <= acked_seq)
    {
        return LOG_ACK_DUPLICATE;
    }

    if (ack_to < tx_from)
    {
        return LOG_ACK_IGNORE;
    }

    if (ack_from <= tx_from && ack_to >= tx_to)
    {
        return LOG_ACK_ACCEPT;
    }

    return LOG_ACK_PARTIAL;
}

// -----------------------------------------------------
// 在途批次中被 ACK 覆盖的条数（前缀，含两端）
//
// records 必须按 seq 升序（cloud_collect_batch() 保证）。
// 返回 0 表示一条都没覆盖到。
// -----------------------------------------------------
static inline uint8_t log_ack_covered_count(
    const LogRecord *records, uint8_t n, uint32_t ack_to)
{
    uint8_t k = 0;

    while (k < n)
    {
        if (records[k].seq > ack_to)
        {
            break;
        }
        k++;
    }

    return k;
}

// -----------------------------------------------------
// ⚠️ DEPRECATED —— 段是否已可删除（**不得用于生产路径**）
//
// ****************************************************************
// WARNING:
//   deprecated.
//   Do NOT use for production reclaim.
//   It relies on dense seq assumption.
// ****************************************************************
//
// 仅当整段的 record 都被 ACK 覆盖（末条 seq <= acked_seq）才允许删除。
// 空段（records == 0）不可删 —— 它可能是当前追加目标。
//
// 【为什么被弃用】本函数按 `first_seq + records - 1` **推导**末条 seq，该推导
//    仅在 "seq→slot 映射稠密" 时成立。而 `seq` 在 `log_emit()` 中**每条非 DEBUG
//    记录都消耗一个**（含 INFO），INFO 又不落 Flash ⇒ Flash 段内的 seq
//    **必然稀疏** ⇒ 推导值系统性偏小（偏小量 = 段内 INFO 空洞数）⇒ 段会在
//    尚有未确认记录时被判"可回收" ⇒ 静默丢失 WARN+。
//
// 【生产路径请改用】`log_ack_segment_reclaimable()`
//    —— 它接收**真实末条 seq**（由 `s_seg_last_seq[]` 提供，来自扫描时读取
//       末槽记录的 `rec.seq`），并额外要求"不与任何空洞相交"。
//
// 【保留本函数的原因】仅供 `test/log_contract/probe_ack.cpp` 的离线回归，
//    以及"稠密场景下与 reclaimable 等价"的对照证明（两者在无 INFO 空洞时
//    结果必然一致）。**不得**在任何 `.cpp` 的生产路径中引用 —— R1 改名即为此。
// -----------------------------------------------------
static inline bool log_ack_segment_deletable_legacy(
    uint32_t first_seq, uint8_t records, uint32_t acked_seq)
{
    if (records == 0u)
    {
        return false;
    }

    const uint32_t last_seq = first_seq + (uint32_t)records - 1u;

    return last_seq <= acked_seq;
}

// -----------------------------------------------------
// 空洞（hole）区间登记 —— FIX-2
//
// 为什么需要：`acked_seq` 是 ACK 驱动的**单点高水位**，不是"该区间内每条
// 都已确认"的集合。以下两种情形会让水位**跨过**未确认记录：
//   · GIVE_UP_NOT_ADVANCE：批次移出云队列，但刻意不推进 acked_seq
//   · 云队列溢出淘汰（Flash-routed）：记录移出云队列，acked_seq 不动
// 若回收只看水位，这些记录的**唯一物理副本**会随段一起被删除。
//
// 因此显式登记"未确认且已放弃本次 Boot 投递"的区间；回收必须与之不相交。
// 区间可合并：give-up 与淘汰都按 seq 递增产生，相邻区间自动并成一条
// （常态下恒为 1 条）。表满且无法合并 ⇒ 由调用方置 overflow，停止回收。
// -----------------------------------------------------
#define LOG_HOLE_MAX 8u

struct LogHole
{
    uint32_t from;
    uint32_t to;
};

// 追加一个空洞区间（闭区间）。
// 返回 false = 表满且无法合并（调用方须置 overflow 并停止回收）。
static inline bool log_hole_add(LogHole *holes, uint8_t *count,
                                uint32_t from, uint32_t to)
{
    if (holes == nullptr || count == nullptr || from > to)
    {
        return false;
    }

    // ① 与已有条目重叠 / 相邻 → 合并
    //    重叠：from <= holes[i].to  且  to >= holes[i].from
    //    相邻：from == holes[i].to + 1  或  to + 1 == holes[i].from
    //    两者合并为一条判定：from <= holes[i].to + 1  且  to + 1 >= holes[i].from
    for (uint8_t i = 0; i < *count; i++)
    {
        if (from <= holes[i].to + 1u && to + 1u >= holes[i].from)
        {
            if (from < holes[i].from)
            {
                holes[i].from = from;
            }
            if (to > holes[i].to)
            {
                holes[i].to = to;
            }
            return true;
        }
    }

    // ② 新条目
    if (*count >= LOG_HOLE_MAX)
    {
        return false;
    }

    holes[*count].from = from;
    holes[*count].to = to;
    (*count)++;

    return true;
}

// 段 [first, last] 是否与任何空洞相交
static inline bool log_hole_overlaps(const LogHole *holes, uint8_t count,
                                     uint32_t first, uint32_t last)
{
    if (holes == nullptr || first > last)
    {
        return false;
    }

    for (uint8_t i = 0; i < count; i++)
    {
        if (!(holes[i].to < first || holes[i].from > last))
        {
            return true;   // 区间相交
        }
    }

    return false;
}

// -----------------------------------------------------
// FIX-H2：为"在途窗口内被淘汰、且未被本次 ACK 覆盖"的区间补齐空洞保护
//
// 背景（FIX-1 × FIX-2 的交界缺口）：
//   `cloud_queue_push()` 在 in-flight 窗口内淘汰时**不**登记空洞 —— 那本身是
//   对的（那些记录仍可能被本次 ACK 覆盖，归宿由 ACK / give-up 收口；提前登记
//   会留下永不复位的陈旧空洞、永久阻塞段回收）。
//   但若本次 ACK **只覆盖前缀 covered**，而 `evicted > covered`，则下标区间
//   [covered, evicted) 的记录同时具备三个坏性质：
//     · 已离开 RAM 云队列（rd 已越过）⇒ 本 Boot 不会再发送
//     · 未被本次 ACK 覆盖            ⇒ acked_seq 不会为它们停留
//     · 也没有任何空洞               ⇒ 段回收对它们不设防
//   后续批次把水位推过它们之后，其物理副本就可能被回收 ⇒ **重启也补不回**。
//
// 下标边界（与 push 的**非**在途分支互斥，不会重复登记）：
//   [0, covered)         已被 ACK                        ⇒ 无需保护
//   [covered, evicted)   被淘汰且未确认                    ⇒ ★本函数负责
//   [evicted, tx_count)  由 push 的 else 分支已登记空洞    ⇒ 跳过
//
// 参数
//   batch / tx_count   本次在途批次（本函数只读）
//   covered            本次 ACK 覆盖的**前缀条数**（0..tx_count）
//   evicted            在途期间从队首淘汰走的条数（可 > tx_count）
//   holes / count      空洞表（原地追加）
//   overflow           出参：表满且无法合并 ⇒ 置 true（调用方须停止段回收）
// 返回
//   实际登记保护成功的**记录条数**（merged 与 new 都计入；观测用）
//
// ⚠️ 只登记 Flash-routed 记录：INFO 不落 Flash，没有物理副本可保护。
// ⚠️ 此处**不**校验 boot_seq（与 push 的写法不同）：补发批次里含上一 Boot
//    写下的记录，它们的物理副本同样需要保护。
// ⚠️ 纯函数、无 I/O、无动态分配 ⇒ 可在主机上编译运行并断言（见 probe_ack.cpp）。
// -----------------------------------------------------
static inline uint8_t log_hole_protect_gap(
    const LogRecord *batch, uint8_t tx_count,
    uint8_t covered, uint32_t evicted,
    LogHole *holes, uint8_t *count, bool *overflow)
{
    if (overflow != nullptr)
    {
        *overflow = false;
    }

    if (batch == nullptr || holes == nullptr || count == nullptr)
    {
        return 0u;
    }

    if (evicted <= (uint32_t)covered)
    {
        return 0u;   // 没有"被淘汰且未确认"的区间
    }

    const uint32_t k_end = (evicted < (uint32_t)tx_count)
                               ? evicted
                               : (uint32_t)tx_count;

    uint8_t added = 0u;

    for (uint32_t k = (uint32_t)covered; k < k_end; k++)
    {
        const LogRecord &r = batch[k];

        if (!log_level_to_flash((LogLevel)r.level))
        {
            continue;   // INFO / DEBUG：无物理副本可保护
        }

        if (log_hole_add(holes, count, r.seq, r.seq))
        {
            added++;
        }
        else if (overflow != nullptr)
        {
            *overflow = true;   // 表满 ⇒ 保守：停止回收
        }
    }

    return added;
}

// -----------------------------------------------------
// 段是否可回收 —— **生产路径的唯一判定点**（FIX-2 + FIX-3b）
//
//   first_seq / last_seq  段的**真实** seq 范围（last_seq 来自
//                         `s_seg_last_seq[]`，**禁止**用 `first + n - 1` 推导）
//   acked_seq             已确认高水位
//   holes/count           空洞表
//   hole_overflow         空洞表溢出（true ⇒ 一律不可回收，保守）
//
// 四个条件：
//   ① 段非空（last_seq >= first_seq）
//   ② 整段 ≤ 水位：last_seq <= acked_seq
//   ③ 与任何空洞不相交
//   ④ 空洞表未溢出
// -----------------------------------------------------
static inline bool log_ack_segment_reclaimable(
    uint32_t first_seq, uint32_t last_seq, uint32_t acked_seq,
    const LogHole *holes, uint8_t hole_count, bool hole_overflow)
{
    if (hole_overflow)
    {
        return false;
    }

    if (last_seq < first_seq)
    {
        return false;   // 空段
    }

    if (last_seq > acked_seq)
    {
        return false;   // 尚有未确认记录
    }

    return !log_hole_overlaps(holes, hole_count, first_seq, last_seq);
}

// -----------------------------------------------------
// 退避时长：base / 2·base / 4·base ...，上限 LOG_ACK_BACKOFF_MAX_MS
//
// retry 为**已发生的超时次数**（1 = 第一次超时）。
// retry == 0 时返回 base。
//
// 正式固件用 base = LOG_ACK_BACKOFF_BASE_MS(2 s)
// ⇒ 2 / 4 / 8 / 16 / 32 s，随后被 60 s 上限截断。
//
// 为什么带 _ex 版本：上板自测需要把 6 次超时 + 退避压进秒级
// （默认参数下仅退避就累计 62 s，加上 6×15 s 超时共 ≈152 s，
//   超出本项目"单条命令 200 s 上限"的预算），否则
// "重试耗尽 → GIVE_UP_NOT_ADVANCE" 这条冻结语义无法被验证。
// 生产路径不经过 _ex（见下面的 log_ack_backoff_ms 包装）。
// -----------------------------------------------------
static inline uint32_t log_ack_backoff_ms_ex(uint32_t retry, uint32_t base)
{
    if (base == 0u)
    {
        base = LOG_ACK_BACKOFF_BASE_MS;
    }

    if (retry == 0u)
    {
        return (base > LOG_ACK_BACKOFF_MAX_MS) ? LOG_ACK_BACKOFF_MAX_MS : base;
    }

    if (retry > 16u)
    {
        retry = 16u;   // 防止移位溢出，结果必然已到上限（或按 base 截断）
    }

    const uint32_t ms = base << (retry - 1u);

    // 溢出 / 越过上限 ⇒ 取上限；结果不可能小于 base
    if (ms < base || ms > LOG_ACK_BACKOFF_MAX_MS)
    {
        return LOG_ACK_BACKOFF_MAX_MS;
    }

    return ms;
}

// 生产路径：固定 base = 2 s
static inline uint32_t log_ack_backoff_ms(uint32_t retry)
{
    return log_ack_backoff_ms_ex(retry, LOG_ACK_BACKOFF_BASE_MS);
}

// -----------------------------------------------------
// 是否应放弃本批次（GIVE_UP_NOT_ADVANCE）
//
// 语义：超时次数超过 LOG_ACK_MAX_RETRY(5) 即放弃**发送**该批次；
//       但 ack 水位与 Flash 段**都不推进**（记录留待下次开机重试）。
// -----------------------------------------------------
static inline bool log_ack_should_give_up(uint32_t retry)
{
    return retry > LOG_ACK_MAX_RETRY;
}

// -----------------------------------------------------
// 补发（replay）时是否应把这条从 Flash 取出重发
//
//   acked_seq     已确认高水位（重启后为 0 ⇒ 全部重放，属正常行为）
//   give_up_seq   本次开机已放弃重发的水位（避免与云端无 ACK 时死循环）
// -----------------------------------------------------
static inline bool log_ack_should_replay(
    uint32_t seq, uint32_t acked_seq, uint32_t give_up_seq)
{
    if (seq == 0u)
    {
        return false;
    }

    if (seq <= acked_seq)
    {
        return false;
    }

    if (give_up_seq != 0u && seq <= give_up_seq)
    {
        return false;
    }

    return true;
}

#endif // LOG_ACK_H
