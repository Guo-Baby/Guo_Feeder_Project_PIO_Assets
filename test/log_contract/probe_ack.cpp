// =====================================================
// P1.5 ACK / Retry / Offline 决策逻辑 —— **真实执行**的验证
//
// log_ack.h 是 freestanding 的（只依赖 <stdint.h> + log_events.h），因此可以
// 在**没有开发板、没有 MQTT 对端、没有 CRT** 的机器上真实跑起来：
//
//   clang++ --target=wasm32 -std=gnu++11 -nostdlib -fno-builtin -I src \
//           test/log_contract/probe_ack.cpp \
//           -Wl,--no-entry -Wl,--export-all -o .pio/probe_ack.wasm
//   node test/log_contract/run_wasm_probe.js .probe/probe_ack.wasm
//
// 为什么需要它：ACK 的正确性依赖"忽略 / 重复 / 部分覆盖 / 退避 / 放弃"这些
// 分支，而本设备在开发期长期没有可用的云端对端 —— 只靠上板串口无法覆盖。
// 把判定与执行分离后，判定部分在这里被穷举。
//
// 验证点（对应冻结语义）：
//   ① 区间非法 / 无在途 / boot_seq 不匹配 → IGNORE
//   ② ack_to <= acked_seq（回退或重复）   → DUPLICATE（无变化，不是错误）
//   ③ ack_to < tx_from（确认更早批次）     → IGNORE
//   ④ 完整覆盖 → ACCEPT；部分覆盖 → PARTIAL
//   ⑤ 部分覆盖只能推进被覆盖**前缀**（log_ack_covered_count）
//   ⑥ 「部分覆盖不得删段」→ 整段末条 seq 才决定可删性
//   ⑦ 退避 2/4/8/16/32 s，上限 60 s；超过 LOG_ACK_MAX_RETRY 才放弃
//   ⑧ GIVE_UP_NOT_ADVANCE：放弃后可重放的水位只前移到 give_up_seq
// =====================================================

#include <stdint.h>
#include <stddef.h>

// ---- freestanding 下的 libc 原语 ----

extern "C" void *memcpy(void *dst, const void *src, unsigned long n)
{
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    for (unsigned long i = 0; i < n; i++)
    {
        d[i] = s[i];
    }
    return dst;
}

extern "C" void *memset(void *dst, int c, unsigned long n)
{
    unsigned char *d = (unsigned char *)dst;
    for (unsigned long i = 0; i < n; i++)
    {
        d[i] = (unsigned char)c;
    }
    return dst;
}

extern "C" int memcmp(const void *a, const void *b, unsigned long n)
{
    const unsigned char *x = (const unsigned char *)a;
    const unsigned char *y = (const unsigned char *)b;
    for (unsigned long i = 0; i < n; i++)
    {
        if (x[i] != y[i])
        {
            return (int)x[i] - (int)y[i];
        }
    }
    return 0;
}

#include "log_ack.h"

// ---- 极简输出（写进静态缓冲，由 node 侧读出）----

static char g_log[8192];
static int  g_log_len = 0;
static int  g_fail = 0;

static void pch(char c)
{
    if (g_log_len < (int)sizeof(g_log) - 1)
    {
        g_log[g_log_len++] = c;
    }
}

static void pstr(const char *s)
{
    while (*s != '\0')
    {
        pch(*s++);
    }
}

static void pdec(uint32_t v)
{
    char b[12];
    int i = 0;

    if (v == 0u)
    {
        pch('0');
        return;
    }

    while (v > 0u)
    {
        b[i++] = (char)('0' + (int)(v % 10u));
        v /= 10u;
    }

    while (i > 0)
    {
        pch(b[--i]);
    }
}

static void check(int cond, const char *what)
{
    pstr(cond ? "  [OK  ] " : "  [FAIL] ");
    pstr(what);
    pch('\n');

    if (cond == 0)
    {
        g_fail++;
    }
}

// 带数值的检查（便于失败时定位）
static void checkv(int cond, const char *what, uint32_t got)
{
    if (cond != 0)
    {
        check(1, what);
        return;
    }

    pstr("  [FAIL] ");
    pstr(what);
    pstr(" (got=");
    pdec(got);
    pstr(")\n");
    g_fail++;
}

extern "C" int probe_run()
{
    g_log_len = 0;
    g_fail = 0;

    pstr("---- P1.5 ack/retry/offline logic (wasm32) ----\n");

    // 冻结常量自检（编译期）
    static_assert(LOG_ACK_TIMEOUT_MS == 15000u, "ack timeout must be 15 s");
    static_assert(LOG_ACK_MAX_RETRY == 5u, "max retry must be 5");
    static_assert(LOG_ACK_BACKOFF_MAX_MS == 60000u, "backoff cap must be 60 s");
    static_assert(LOG_ACK_BACKOFF_BASE_MS == 2000u, "backoff base must be 2 s");
    static_assert((LOG_ACK_BACKOFF_BASE_MS << 5u) > LOG_ACK_BACKOFF_MAX_MS,
                  "60 s cap must be reachable from 2 s base");

    // ---------- ① 区间合法性 ----------
    pstr("  区间合法性:\n");
    check(!log_ack_range_valid(0u, 0u), "from=0,to=0 -> invalid");
    check(!log_ack_range_valid(0u, 5u), "from=0,to=5 -> invalid");
    check(!log_ack_range_valid(5u, 0u), "from=5,to=0 -> invalid");
    check(!log_ack_range_valid(5u, 4u), "from=5,to=4 -> invalid");
    check(log_ack_range_valid(5u, 5u), "from=5,to=5 -> valid");
    check(log_ack_range_valid(4u, 5u), "from=4,to=5 -> valid");

    // ---------- ② 分类 ----------
    // 在途批次：boot=7, seq 100..109，acked 高水位 0
    const uint32_t tb = 7u, tf = 100u, tt = 109u, ac0 = 0u;

    pstr("  ACK 分类:\n");

    check((int)log_ack_classify(7u, 0u, 5u, tb, tf, tt, ac0) == (int)LOG_ACK_IGNORE,
          "invalid range -> IGNORE");

    check((int)log_ack_classify(7u, 100u, 109u, 0u, 0u, 0u, ac0) == (int)LOG_ACK_IGNORE,
          "no inflight batch -> IGNORE");

    check((int)log_ack_classify(6u, 100u, 109u, tb, tf, tt, ac0) == (int)LOG_ACK_IGNORE,
          "wrong boot_seq -> IGNORE");

    // acked 必须 < ack_to，否则规则 ④（回退/重复）会先命中
    check((int)log_ack_classify(7u, 1u, 99u, tb, tf, tt, ac0) == (int)LOG_ACK_IGNORE,
          "ack_to < tx_from (older batch) -> IGNORE");

    // 优先级固化：acked 已到 99 时，同一条 ack 应判为 DUPLICATE 而非 IGNORE
    // （两者都不提交，但只有 DUPLICATE 能说明"这是回退/重复"）
    check((int)log_ack_classify(7u, 1u, 99u, tb, tf, tt, 99u) == (int)LOG_ACK_DUPLICATE,
          "precedence: rollback wins over older-batch");

    check((int)log_ack_classify(7u, 100u, 109u, tb, tf, tt, 109u) == (int)LOG_ACK_DUPLICATE,
          "rollback ack_to <= acked_seq -> DUPLICATE");

    check((int)log_ack_classify(7u, 90u, 100u, tb, tf, tt, 100u) == (int)LOG_ACK_DUPLICATE,
          "duplicate (acked already 100) -> DUPLICATE");

    check((int)log_ack_classify(7u, 100u, 109u, tb, tf, tt, ac0) == (int)LOG_ACK_ACCEPT,
          "exact full cover -> ACCEPT");

    check((int)log_ack_classify(7u, 95u, 120u, tb, tf, tt, ac0) == (int)LOG_ACK_ACCEPT,
          "superset cover -> ACCEPT");

    check((int)log_ack_classify(7u, 100u, 104u, tb, tf, tt, ac0) == (int)LOG_ACK_PARTIAL,
          "prefix cover -> PARTIAL");

    check((int)log_ack_classify(7u, 104u, 109u, tb, tf, tt, ac0) == (int)LOG_ACK_PARTIAL,
          "interior cover -> PARTIAL");

    check((int)log_ack_classify(7u, 101u, 105u, tb, tf, tt, ac0) == (int)LOG_ACK_PARTIAL,
          "mid-range cover -> PARTIAL");

    // ---------- ③ 被覆盖前缀条数 ----------
    pstr("  覆盖前缀计数:\n");

    LogRecord batch[LOG_BATCH_MAX_RECORDS];
    memset(batch, 0, sizeof(batch));

    for (uint8_t i = 0; i < 5; i++)
    {
        batch[i].seq = 10u + (uint32_t)i;         // 10,11,12,13,14
        batch[i].boot_seq = 3u;
    }

    checkv(log_ack_covered_count(batch, 5u, 9u) == 0u,
           "ack_to below first seq -> 0", log_ack_covered_count(batch, 5u, 9u));
    checkv(log_ack_covered_count(batch, 5u, 10u) == 1u,
           "ack_to == first -> 1", log_ack_covered_count(batch, 5u, 10u));
    checkv(log_ack_covered_count(batch, 5u, 12u) == 3u,
           "ack_to mid -> 3", log_ack_covered_count(batch, 5u, 12u));
    checkv(log_ack_covered_count(batch, 5u, 14u) == 5u,
           "ack_to == last -> 5", log_ack_covered_count(batch, 5u, 14u));
    checkv(log_ack_covered_count(batch, 5u, 9999u) == 5u,
           "ack_to beyond last -> clamped 5", log_ack_covered_count(batch, 5u, 9999u));
    checkv(log_ack_covered_count(batch, 0u, 99u) == 0u,
           "empty batch -> 0", log_ack_covered_count(batch, 0u, 99u));

    // ---------- ④ 段可删性（"部分覆盖不得删段"）----------
    pstr("  段可删性:\n");

    check(!log_ack_segment_deletable_legacy(1u, 0u, 1000u),
          "empty segment -> not deletable");
    check(!log_ack_segment_deletable_legacy(1u, 10u, 9u),
          "last=10 > acked=9 -> not deletable");
    check(log_ack_segment_deletable_legacy(1u, 10u, 10u),
          "last=10 <= acked=10 -> deletable");
    check(log_ack_segment_deletable_legacy(1u, 10u, 11u),
          "last=10 <= acked=11 -> deletable");
    check(!log_ack_segment_deletable_legacy(31u, 31u, 60u),
          "first=31,n=31,acked=60 (last=61) -> not deletable");
    check(log_ack_segment_deletable_legacy(31u, 31u, 61u),
          "first=31,n=31,acked=61 -> deletable");
    check(log_ack_segment_deletable_legacy(1u, 1u, 1u),
          "single record fully acked -> deletable");

    // ---------- ④′ FIX-2：空洞登记（可合并 / 有界）----------
    pstr("  空洞表（FIX-2）:\n");

    LogHole hs[LOG_HOLE_MAX];
    uint8_t hn = 0;

    for (uint8_t i = 0; i < LOG_HOLE_MAX; i++)
    {
        hs[i].from = 0;
        hs[i].to = 0;
    }

    check(log_hole_add(hs, &hn, 10u, 20u) && hn == 1u, "add [10,20] -> count=1");
    check(log_hole_add(hs, &hn, 21u, 30u) && hn == 1u, "adjacent [21,30] merges");
    check(hs[0].from == 10u && hs[0].to == 30u, "merged range = [10,30]");
    check(log_hole_add(hs, &hn, 15u, 17u) && hn == 1u, "contained [15,17] merges");
    check(hs[0].from == 10u && hs[0].to == 30u, "contained merge keeps [10,30]");
    check(log_hole_add(hs, &hn, 5u, 9u) && hn == 1u, "left-adjacent [5,9] merges");
    check(hs[0].from == 5u && hs[0].to == 30u, "merged range = [5,30]");
    check(log_hole_add(hs, &hn, 100u, 110u) && hn == 2u, "disjoint [100,110] -> count=2");

    check(log_hole_add(hs, &hn, 200u, 201u) && hn == 3u, "add 3rd");
    check(log_hole_add(hs, &hn, 300u, 301u) && hn == 4u, "add 4th");
    check(log_hole_add(hs, &hn, 400u, 401u) && hn == 5u, "add 5th");
    check(log_hole_add(hs, &hn, 500u, 501u) && hn == 6u, "add 6th");
    check(log_hole_add(hs, &hn, 600u, 601u) && hn == 7u, "add 7th");
    check(log_hole_add(hs, &hn, 700u, 701u) && hn == 8u, "add 8th (table now full)");

    check(!log_hole_add(hs, &hn, 900u, 901u) && hn == 8u,
          "9th disjoint -> REJECTED, count stays 8");
    check(log_hole_add(hs, &hn, 700u, 720u) && hn == 8u,
          "merge into a full table still allowed");

    // 无法表示的区间（from > to）必须被拒
    check(!log_hole_add(hs, &hn, 50u, 49u), "from > to -> rejected");

    pstr("  空洞相交判定:\n");
    check(log_hole_overlaps(hs, hn, 5u, 5u), "single seq 5 inside [5,30] -> overlap");
    check(!log_hole_overlaps(hs, hn, 1u, 4u), "[1,4] before all holes -> no overlap");
    check(!log_hole_overlaps(hs, hn, 31u, 99u), "[31,99] in the gap -> no overlap");
    check(log_hole_overlaps(hs, hn, 99u, 100u), "touching [100,110] -> overlap");
    check(log_hole_overlaps(hs, hn, 30u, 31u), "touching left edge -> overlap");
    check(!log_hole_overlaps(nullptr, 0u, 1u, 9u), "null table -> no overlap");

    // ---------- ④″ FIX-2 + FIX-3b：空洞感知回收 ----------
    pstr("  空洞感知回收（FIX-2 + FIX-3b）:\n");

    LogHole g1[1];
    g1[0].from = 10u;
    g1[0].to = 20u;

    check(log_ack_segment_reclaimable(1u, 9u, 100u, g1, 1u, false),
          "[1,9] acked=100 -> reclaimable");
    check(!log_ack_segment_reclaimable(1u, 15u, 100u, g1, 1u, false),
          "[1,15] overlaps hole -> KEEP (CRITICAL-2 fix)");
    check(log_ack_segment_reclaimable(21u, 31u, 100u, g1, 1u, false),
          "[21,31] above hole -> reclaimable");
    check(!log_ack_segment_reclaimable(90u, 101u, 100u, g1, 1u, false),
          "last=101 > acked=100 -> KEEP");
    check(!log_ack_segment_reclaimable(5u, 4u, 100u, g1, 1u, false),
          "empty (last<first) -> KEEP");
    check(!log_ack_segment_reclaimable(1u, 9u, 100u, g1, 1u, true),
          "hole table overflow -> NEVER reclaim");
    check(log_ack_segment_reclaimable(1u, 9u, 100u, g1, 0u, false),
          "no holes -> reclaimable when fully acked");

    // ★ FIX-3b 的存在理由：段内 seq **稀疏**（INFO 不落 Flash）时，
    //   `first + records - 1` 会**小于**真实末条 seq。
    //   若用派生值判定，段 [first=1, n=3, derived=3] 在 acked=3 时会被误判可回收，
    //   而真实末条 seq=5 尚未确认 ⇒ 记录被静默删除。
    check(log_ack_segment_deletable_legacy(1u, 3u, 3u),
          "legacy derived (first=1,n=3,acked=3) -> would DELETE  [wrong]");
    check(!log_ack_segment_reclaimable(1u, 5u, 3u, nullptr, 0u, false),
          "sparse truth (last=5, acked=3) -> KEEP                    [correct]");

    // ---------- ④‴ FIX-H2：淘汰间隙补齐空洞（FIX-1 × FIX-2 交界缺口）----------
    //
    // 场景：send batch → ACK 只覆盖前缀 covered → 期间 overflow evicted > covered
    //   ⇒ 下标 [covered, evicted) 的记录：已离开 RAM 队列 / 未被 ACK / 无空洞
    //   ⇒ 必须在此补齐保护，且**只**保护 Flash-routed（INFO 无物理副本可保护）。
    pstr("  淘汰间隙补齐空洞（FIX-H2）:\n");

    LogRecord gap_batch[LOG_BATCH_MAX_RECORDS];
    memset(gap_batch, 0, sizeof(gap_batch));

    for (uint8_t i = 0; i < 12u; i++)
    {
        gap_batch[i].seq = 100u + (uint32_t)i;     // 100..111
        gap_batch[i].boot_seq = 7u;
        // 偶数下标 = WARN（落 Flash）；奇数下标 = INFO（不落 Flash）
        gap_batch[i].level = ((i % 2u) == 0u) ? (uint8_t)LOG_LVL_WARN
                                              : (uint8_t)LOG_LVL_INFO;
    }

    LogHole h2[LOG_HOLE_MAX];
    uint8_t h2n = 0u;
    bool h2ovf = false;

    // covered=4, evicted=12 ⇒ 保护下标 4..11 中的 WARN（seq 104/106/108/110）= 4 条
    checkv(log_hole_protect_gap(gap_batch, 12u, 4u, 12u, h2, &h2n, &h2ovf) == 4u,
           "covered=4 evicted=12 -> 4 Flash-routed protected",
           log_hole_protect_gap(gap_batch, 12u, 4u, 12u, h2, &h2n, &h2ovf));
    checkv(h2n == 4u, "4 separate holes (104/106/108/110 not adjacent)", h2n);
    check(!h2ovf, "no overflow");

    check(log_hole_overlaps(h2, h2n, 104u, 104u), "seq 104 protected");
    check(log_hole_overlaps(h2, h2n, 106u, 106u), "seq 106 protected");
    check(log_hole_overlaps(h2, h2n, 108u, 108u), "seq 108 protected");
    check(log_hole_overlaps(h2, h2n, 110u, 110u), "seq 110 protected");

    check(!log_hole_overlaps(h2, h2n, 105u, 105u), "seq 105 (INFO) NOT protected");
    check(!log_hole_overlaps(h2, h2n, 100u, 100u), "seq 100 (covered) NOT protected");
    check(!log_hole_overlaps(h2, h2n, 102u, 102u), "seq 102 (covered) NOT protected");

    // ★ 保护生效的实证：含 seq 104 的段必须被拒绝回收，直到水位越过它
    check(!log_ack_segment_reclaimable(100u, 111u, 111u, h2, h2n, false),
          "segment [100,111] fully acked BUT overlaps gap hole -> KEEP");

    // 边界 1：evicted == covered ⇒ 无间隙，不登记任何空洞
    h2n = 0u;
    h2ovf = false;
    checkv(log_hole_protect_gap(gap_batch, 12u, 12u, 12u, h2, &h2n, &h2ovf) == 0u,
           "evicted == covered -> gap is empty",
           log_hole_protect_gap(gap_batch, 12u, 12u, 12u, h2, &h2n, &h2ovf));
    checkv(h2n == 0u, "no hole added", h2n);

    // 边界 2：evicted < covered（正常 ACK 多于淘汰）⇒ 同样无间隙
    h2n = 0u;
    h2ovf = false;
    checkv(log_hole_protect_gap(gap_batch, 12u, 12u, 5u, h2, &h2n, &h2ovf) == 0u,
           "evicted < covered -> gap is empty",
           log_hole_protect_gap(gap_batch, 12u, 12u, 5u, h2, &h2n, &h2ovf));

    // 边界 3：covered == 0（本次 ACK 未覆盖任何内容）⇒ 保护 [0, evicted) 中的 WARN
    //   下标 0,2,4,6,8,10 = 6 条
    h2n = 0u;
    h2ovf = false;
    checkv(log_hole_protect_gap(gap_batch, 12u, 0u, 12u, h2, &h2n, &h2ovf) == 6u,
           "covered=0 evicted=12 -> 6 Flash-routed protected",
           log_hole_protect_gap(gap_batch, 12u, 0u, 12u, h2, &h2n, &h2ovf));
    check(log_hole_overlaps(h2, h2n, 100u, 100u), "seq 100 protected when covered=0");

    // 边界 4：evicted > tx_count ⇒ 只处理到 tx_count（其后由 push 的 else 分支负责）
    //   tx_count=8, covered=2, evicted=99 ⇒ 扫描下标 [2,8) 中的 WARN：2,4,6 = 3 条
    h2n = 0u;
    h2ovf = false;
    checkv(log_hole_protect_gap(gap_batch, 8u, 2u, 99u, h2, &h2n, &h2ovf) == 3u,
           "evicted > tx_count -> clamped to tx_count",
           log_hole_protect_gap(gap_batch, 8u, 2u, 99u, h2, &h2n, &h2ovf));
    check(!log_hole_overlaps(h2, h2n, 108u, 108u),
          "index 8 (>= tx_count) NOT handled here");

    // 边界 5：空指针 / 零批 ⇒ 安全返回 0
    h2n = 0u;
    h2ovf = false;
    checkv(log_hole_protect_gap(nullptr, 12u, 0u, 12u, h2, &h2n, &h2ovf) == 0u,
           "null batch -> 0", log_hole_protect_gap(nullptr, 12u, 0u, 12u, h2, &h2n, &h2ovf));
    checkv(log_hole_protect_gap(gap_batch, 12u, 0u, 12u, nullptr, &h2n, &h2ovf) == 0u,
           "null holes -> 0", log_hole_protect_gap(gap_batch, 12u, 0u, 12u, nullptr, &h2n, &h2ovf));

    // 边界 6：空洞表已满且无法合并 ⇒ overflow 置位（调用方须停止回收）
    h2n = LOG_HOLE_MAX;
    h2ovf = false;
    for (uint8_t i = 0; i < LOG_HOLE_MAX; i++)
    {
        h2[i].from = 1000u + (uint32_t)i * 10u;   // 1000,1010,1020,... 互不相邻
        h2[i].to   = h2[i].from;
    }
    checkv(log_hole_protect_gap(gap_batch, 12u, 0u, 12u, h2, &h2n, &h2ovf) == 0u,
           "full table -> 0 protected",
           log_hole_protect_gap(gap_batch, 12u, 0u, 12u, h2, &h2n, &h2ovf));
    check(h2ovf, "full table -> overflow flag set");

    // ---------- ⑤ 退避序列 ----------
    pstr("  退避序列（期望 2/4/8/16/32/60 s）:\n");

    checkv(log_ack_backoff_ms(0u) == 2000u, "retry=0 -> 2000", log_ack_backoff_ms(0u));
    checkv(log_ack_backoff_ms(1u) == 2000u, "retry=1 -> 2000", log_ack_backoff_ms(1u));
    checkv(log_ack_backoff_ms(2u) == 4000u, "retry=2 -> 4000", log_ack_backoff_ms(2u));
    checkv(log_ack_backoff_ms(3u) == 8000u, "retry=3 -> 8000", log_ack_backoff_ms(3u));
    checkv(log_ack_backoff_ms(4u) == 16000u, "retry=4 -> 16000", log_ack_backoff_ms(4u));
    checkv(log_ack_backoff_ms(5u) == 32000u, "retry=5 -> 32000", log_ack_backoff_ms(5u));
    checkv(log_ack_backoff_ms(6u) == 60000u, "retry=6 -> capped 60000", log_ack_backoff_ms(6u));
    checkv(log_ack_backoff_ms(10u) == 60000u, "retry=10 -> capped 60000", log_ack_backoff_ms(10u));
    checkv(log_ack_backoff_ms(16u) == 60000u, "retry=16 -> capped 60000", log_ack_backoff_ms(16u));
    checkv(log_ack_backoff_ms(100u) == 60000u, "retry=100 (clamped) -> 60000", log_ack_backoff_ms(100u));

    // 上板自测用的可调基数版本（默认基数下必须与 log_ack_backoff_ms 完全一致）
    checkv(log_ack_backoff_ms_ex(3u, 10u) == 40u,
           "ex: base=10,retry=3 -> 40", log_ack_backoff_ms_ex(3u, 10u));
    checkv(log_ack_backoff_ms_ex(1u, 10u) == 10u,
           "ex: base=10,retry=1 -> 10", log_ack_backoff_ms_ex(1u, 10u));
    checkv(log_ack_backoff_ms_ex(3u, LOG_ACK_BACKOFF_BASE_MS) == log_ack_backoff_ms(3u),
           "ex(base=default) == production path", log_ack_backoff_ms_ex(3u, LOG_ACK_BACKOFF_BASE_MS));
    checkv(log_ack_backoff_ms_ex(3u, 0u) == log_ack_backoff_ms(3u),
           "ex(base=0) falls back to default", log_ack_backoff_ms_ex(3u, 0u));

    // ---------- ⑥ 放弃门槛 ----------
    pstr("  放弃门槛（LOG_ACK_MAX_RETRY=5）:\n");

    check(!log_ack_should_give_up(0u), "retry=0 -> keep trying");
    check(!log_ack_should_give_up(5u), "retry=5 -> keep trying");
    check(log_ack_should_give_up(6u), "retry=6 -> give up");
    check(log_ack_should_give_up(99u), "retry=99 -> give up");

    // ---------- ⑦ GIVE_UP_NOT_ADVANCE 下的可重放水位 ----------
    pstr("  可重放水位:\n");

    check(!log_ack_should_replay(0u, 0u, 0u), "seq=0 -> never replay");
    check(!log_ack_should_replay(100u, 100u, 0u), "seq == acked -> no replay");
    check(!log_ack_should_replay(99u, 100u, 0u), "seq < acked -> no replay");
    check(log_ack_should_replay(101u, 100u, 0u), "seq > acked -> replay");
    check(!log_ack_should_replay(5u, 0u, 5u), "seq <= give_up_seq -> no replay");
    check(log_ack_should_replay(6u, 0u, 5u), "seq > give_up_seq -> replay");
    check(log_ack_should_replay(1u, 0u, 0u), "acked=0 & giveup=0 -> replay (reboot)");

    pstr("  合计失败: ");
    pdec((uint32_t)g_fail);
    pch('\n');

    return g_fail;
}

// ---- 供 node 侧读取的导出 ----

extern "C" const char *probe_log_ptr()
{
    return g_log;
}

extern "C" int probe_log_len()
{
    return g_log_len;
}
