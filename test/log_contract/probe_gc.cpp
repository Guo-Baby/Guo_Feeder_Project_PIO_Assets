// =====================================================
// 问题 B —— 段回收（GC）状态机：**真实固件逻辑**的穷举验证
//
// log_ack.h 是 freestanding 的（只依赖 <stdint.h> + log_events.h），因此可在
// **没有开发板、没有 LittleFS** 的机器上真实跑起来：
//
//   clang++ --target=wasm32 -std=gnu++11 -nostdlib -fno-builtin -I src \
//           test/log_contract/probe_gc.cpp \
//           -Wl,--no-entry -Wl,--export-all -o .pio/probe_gc.wasm
//   node test/log_contract/run_wasm_probe.js .pio/probe_gc.wasm
//
// 与 probe_ack.cpp 的分工：
//   probe_ack  = ACK 分类 / 覆盖前缀 / 退避 / 放弃（**协议**侧）
//   本探针     = **"哪一段可以被删"** 的判定（**GC** 侧），以及它与
//                `log_ack_gc_watermark()`、空洞表、溢出标志的交互。
//
// ★ 本探针**只验证纯判定**（`log_ack_segment_reclaimable` / `log_ack_gc_watermark`）：
//   真正的删除动作在 `log_manager.cpp::cloud_delete_acked_segments()` 里，依赖
//   LittleFS，无法在主机上跑。调用方守卫（append 段 / 补发游标 / 空段）属**代码审查**
//   结论，见 `docs/review/BT-1-...` 与 `test/diag/README`。**不得**把本探针的通过
//   表述为"真机段回收已验证"（那需要 `BT1-T05` 的串口证据）。
//
// 覆盖的判据（对应冻结语义 §13 / FIX-2 / FIX-3b / FIX-BT9）：
//   ① 整段已确认（last ≤ gc）⇒ 可回收
//   ② 尚有未确认（last > gc）⇒ 不可回收
//   ③ 边界：last == gc 可回收；last == gc+1 不可
//   ④ 空段（last < first）⇒ 不可
//   ⑤ 与空洞相交 ⇒ 不可；相邻不相交 ⇒ 可
//   ⑥ 空洞表溢出 ⇒ 一律不可（保守）
//   ⑦ seq 从 1 开始 / UINT32_MAX 端点不溢出
//   ⑧ **必须用 gc 水位**：用 acked_max 会误删仍含未补发记录的段（BT-9 复现）
//   ⑨ 重复 / 回退 ACK ⇒ DUPLICATE（无变化），且不推进水位
//   ⑩ 谓词本身**不认识 append 段** ⇒ append 保护必须由调用方提供（断言其语义）
// =====================================================

#include <stdint.h>
#include <stddef.h>

#include "log/log_ack.h"

// ---- freestanding libc ----

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

// ---- 极简输出缓冲 ----

static char g_log[16384];
static int g_log_len = 0;
static int g_fail = 0;

static void pch(char c)
{
    if (g_log_len < (int)sizeof(g_log) - 1)
    {
        g_log[g_log_len++] = c;
        g_log[g_log_len] = 0;
    }
}

static void pstr(const char *s)
{
    while (s && *s)
    {
        pch(*s++);
    }
}

static void pdec(uint32_t v)
{
    char b[12];
    int n = 0;
    if (v == 0u)
    {
        pch('0');
        return;
    }
    while (v > 0u)
    {
        b[n++] = (char)('0' + (v % 10u));
        v /= 10u;
    }
    while (n > 0)
    {
        pch(b[--n]);
    }
}

static void line(const char *name, bool pass, uint32_t got)
{
    pstr(pass ? "  ok   " : "  FAIL ");
    pstr(name);
    if (!pass)
    {
        pstr("   got=");
        pdec(got);
        g_fail++;
    }
    pch('\n');
}

static void check(bool cond, const char *name)
{
    line(name, cond, cond ? 0u : 1u);
}

static void checkv(uint32_t got, uint32_t want, const char *name)
{
    const bool ok = (got == want);
    if (!ok)
    {
        pstr("  FAIL ");
        pstr(name);
        pstr("   want=");
        pdec(want);
        pstr(" got=");
        pdec(got);
        pch('\n');
        g_fail++;
        return;
    }
    line(name, true, 0u);
}

static void checkb(bool got, bool want, const char *name)
{
    const bool ok = (got == want);
    if (!ok)
    {
        pstr("  FAIL ");
        pstr(name);
        pstr("   want=");
        pstr(want ? "true" : "false");
        pstr(" got=");
        pstr(got ? "true" : "false");
        pch('\n');
        g_fail++;
        return;
    }
    line(name, true, 0u);
}

// =====================================================

extern "C" int probe_run()
{
    g_log_len = 0;
    g_fail = 0;
    pch(0);

    pstr("== B · segment GC predicate \n");

    // ---- ① 整段已确认 ----
    checkb(log_ack_segment_reclaimable(100u, 130u, 130u, nullptr, 0u, false),
           true, "G1 whole segment acked (last == gc) -> reclaimable");

    // ---- ② 尚有未确认 ----
    checkb(log_ack_segment_reclaimable(100u, 130u, 129u, nullptr, 0u, false),
           false, "G2 last > gc (one unacked) -> NOT reclaimable");

    // ---- ③ 边界 ----
    checkb(log_ack_segment_reclaimable(100u, 130u, 131u, nullptr, 0u, false),
           true, "G3 gc beyond last -> reclaimable");
    checkb(log_ack_segment_reclaimable(130u, 130u, 130u, nullptr, 0u, false),
           true, "G3b single-record segment, acked -> reclaimable");
    checkb(log_ack_segment_reclaimable(130u, 130u, 129u, nullptr, 0u, false),
           false, "G3c single-record segment, unacked -> NOT reclaimable");

    // ---- ④ 空段 / 非法范围 ----
    checkb(log_ack_segment_reclaimable(130u, 100u, 999u, nullptr, 0u, false),
           false, "G4 empty segment (last < first) -> NOT reclaimable");

    // ★ 谓词只把 `last < first` 当空段；(0,0,0) 满足 last<=gc 且无洞 ⇒ 判 true。
    //   生产路径**不会**把这种参数送进来：调用方先判 `s_seg_present[seg]` 与
    //   `s_seg_records[seg] == 0`（log_manager.cpp:2403/2413）。
    //   本用例把该前提**显式化**，防止将来误以为谓词自带"空段"保护。
    checkb(log_ack_segment_reclaimable(0u, 0u, 0u, nullptr, 0u, false),
           true, "G4b degenerate (0,0,0) is reclaimable at predicate level -> caller must guard empty seg");

    // ---- ⑤ 空洞相交 ----
    {
        LogHole h[2];
        h[0].from = 118u; h[0].to = 120u;      // 段内
        h[1].from = 200u; h[1].to = 210u;      // 段外

        checkb(log_ack_segment_reclaimable(100u, 130u, 200u, h, 1u, false),
               false, "G5 hole inside segment -> NOT reclaimable");

        /* 相邻但不相交：hole = [131,140] */
        LogHole a[1];
        a[0].from = 131u; a[0].to = 140u;
        checkb(log_ack_segment_reclaimable(100u, 130u, 200u, a, 1u, false),
               true, "G5b adjacent hole (no overlap) -> reclaimable");

        /* 段被空洞完整包住 */
        LogHole w[1];
        w[0].from = 1u; w[0].to = 5000u;
        checkb(log_ack_segment_reclaimable(100u, 130u, 5000u, w, 1u, false),
               false, "G5c hole covering segment -> NOT reclaimable");

        /* 第二个洞命中（不得只看第一个） */
        LogHole two[2];
        two[0].from = 1u;   two[0].to = 50u;
        two[1].from = 125u; two[1].to = 126u;
        checkb(log_ack_segment_reclaimable(100u, 130u, 500u, two, 2u, false),
               false, "G5d second hole hits -> NOT reclaimable");
    }

    // ---- ⑥ 空洞表溢出 ----
    {
        LogHole h[1];
        h[0].from = 1u; h[0].to = 1u;
        checkb(log_ack_segment_reclaimable(100u, 130u, 999u, h, 1u, true),
               false, "G6 hole table overflow -> NOT reclaimable (conservative)");
        /* 溢出时即便无洞也必须保守 */
        checkb(log_ack_segment_reclaimable(100u, 130u, 999u, nullptr, 0u, true),
               false, "G6b overflow with empty table -> NOT reclaimable");
    }

    // ---- ⑦ 边界 seq ----
    checkb(log_ack_segment_reclaimable(1u, 31u, 31u, nullptr, 0u, false),
           true, "G7 segment starting at seq=1, acked -> reclaimable");
    checkb(log_ack_segment_reclaimable(1u, 31u, 0u, nullptr, 0u, false),
           false, "G7b gc=0 (fresh boot) -> NOT reclaimable");

    {
        const uint32_t mx = 0xFFFFFFFFu;
        checkb(log_ack_segment_reclaimable(mx - 5u, mx, mx, nullptr, 0u, false),
               true, "G8 UINT32_MAX endpoint, acked -> reclaimable (no overflow)");
        checkb(log_ack_segment_reclaimable(mx - 5u, mx, mx - 1u, nullptr, 0u, false),
               false, "G8b UINT32_MAX last, gc one short -> NOT reclaimable");
    }

    // ---- ⑧ ★ 必须用 gc 水位，不能用 acked_max（BT-9）----
    //
    // 场景：云端确认最大值 acked_max = 3000，但 seq 2500 之前有未补发记录
    //       ⇒ gc_floor = 2500 ⇒ gc 水位 = 2499。
    //   段 [2000,2499]：整段在 gc 以下 ⇒ 可回收
    //   段 [2000,2500]：末条 2500 仍在 floor 之上 ⇒ **不可**回收
    //   若误用 acked_max(=3000) 判，两段都会被删 ⇒ 2500 这条未补发记录**永久丢失**。
    {
        const uint32_t acked_max = 3000u;
        const uint32_t floor_seq = 2500u;
        const uint32_t gc = log_ack_gc_watermark(acked_max, floor_seq);

        checkv(gc, 2499u, "G9 gc watermark with floor=2500 -> 2499");

        checkb(log_ack_segment_reclaimable(2000u, 2499u, gc, nullptr, 0u, false),
               true, "G9b segment below gc -> reclaimable");

        checkb(log_ack_segment_reclaimable(2000u, 2500u, gc, nullptr, 0u, false),
               false, "G9c segment containing floor seq -> NOT reclaimable (gc)");

        /* 反证：同一个段若误用 acked_max ⇒ 判定为可删（这就是 BT-9 的数据丢失路径） */
        checkb(log_ack_segment_reclaimable(2000u, 2500u, acked_max, nullptr, 0u, false),
               true, "G9d NEG: with raw acked_max the same segment WOULD be deleted");
    }

    // ---- ⑨ 重复 / 回退 ACK：DUPLICATE，且水位不变 ----
    {
        const uint32_t acked = 2817u;

        checkv((uint32_t)log_ack_classify(5u, 1u, 2817u, 5u, 2800u, 2817u, acked),
               (uint32_t)LOG_ACK_DUPLICATE, "G10 exact duplicate ack -> DUPLICATE");
        checkv((uint32_t)log_ack_classify(5u, 1u, 2700u, 5u, 2800u, 2817u, acked),
               (uint32_t)LOG_ACK_DUPLICATE, "G10b rollback ack -> DUPLICATE");

        /* 重复 ACK 不推进水位 ⇒ 段的可回收性不变 */
        checkb(log_ack_segment_reclaimable(2800u, 2900u, acked, nullptr, 0u, false),
               false, "G10c segment above acked stays NOT reclaimable");

        /* boot_seq 不匹配 ⇒ IGNORE（不得影响回收） */
        checkv((uint32_t)log_ack_classify(4u, 1u, 2817u, 5u, 2800u, 2817u, acked),
               (uint32_t)LOG_ACK_IGNORE, "G10d wrong boot_seq -> IGNORE");

        /* 非法区间 ⇒ IGNORE */
        checkv((uint32_t)log_ack_classify(5u, 0u, 2817u, 5u, 2800u, 2817u, acked),
               (uint32_t)LOG_ACK_IGNORE, "G10e from=0 -> IGNORE");
        checkv((uint32_t)log_ack_classify(5u, 2818u, 2817u, 5u, 2800u, 2817u, acked),
               (uint32_t)LOG_ACK_IGNORE, "G10f from>to -> IGNORE");
    }

    // ---- ⑩ 谓词不认识 append 段 ----
    //
    // ★ 这是**语义断言**，不是缺陷：`log_ack_segment_reclaimable()` 只看
    //   (first,last,gc,holes,overflow)，**没有** segment id / append 概念。
    //   因此 append 保护**必须**由调用方提供 —— 事实核对：
    //     `log_manager.cpp::cloud_delete_acked_segments()` 在调用谓词**之前**
    //     已有 `if (seg == s_append_segment) continue;`
    //   ⇒ 本用例锁定"谓词侧不提供该保护"，防止将来误以为谓词已含 append 判断。
    checkb(log_ack_segment_reclaimable(100u, 130u, 999u, nullptr, 0u, false),
           true, "G11 predicate has NO append awareness (caller must skip append seg)");

    // ---- ⑪ 部分覆盖：只能推进前缀 ----
    {
        LogRecord recs[4];
        recs[0].seq = 100u;
        recs[1].seq = 101u;
        recs[2].seq = 102u;
        recs[3].seq = 103u;

        checkv((uint32_t)log_ack_covered_count(recs, 4u, 101u), 2u,
               "G12 partial ack covers prefix only (2/4)");
        checkv((uint32_t)log_ack_covered_count(recs, 4u, 99u), 0u,
               "G12b ack before first -> 0 covered");
        checkv((uint32_t)log_ack_covered_count(recs, 4u, 103u), 4u,
               "G12c ack at last -> all covered");
        checkv((uint32_t)log_ack_covered_count(nullptr, 0u, 10u), 0u,
               "G12d n=0 -> 0 covered (no crash)");
    }

    // ---- ⑫ 放弃（GIVE_UP）：不推进水位 ⇒ 段不回收 ----
    {
        checkb(log_ack_should_give_up(LOG_ACK_MAX_RETRY), false,
               "G13 give-up not triggered at MAX_RETRY");
        checkb(log_ack_should_give_up(LOG_ACK_MAX_RETRY + 1u), true,
               "G13b give-up triggered beyond MAX_RETRY");

        /* 放弃的区间被登记为空洞 ⇒ 段虽整段 ≤ acked_max 仍不可回收 */
        LogHole h[1];
        h[0].from = 2900u;
        h[0].to = 2950u;
        const uint32_t gc = log_ack_gc_watermark(3000u, 2900u);   // = 2899
        checkv(gc, 2899u, "G13c give-up floor lowers gc");
        checkb(log_ack_segment_reclaimable(2900u, 2950u, gc, h, 1u, false),
               false, "G13d segment over give-up hole -> NOT reclaimable");
    }

    pstr("== B done, failures=");
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
