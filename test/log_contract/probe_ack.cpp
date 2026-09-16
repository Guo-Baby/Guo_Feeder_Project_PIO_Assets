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

    check(!log_ack_segment_deletable(1u, 0u, 1000u),
          "empty segment -> not deletable");
    check(!log_ack_segment_deletable(1u, 10u, 9u),
          "last=10 > acked=9 -> not deletable");
    check(log_ack_segment_deletable(1u, 10u, 10u),
          "last=10 <= acked=10 -> deletable");
    check(log_ack_segment_deletable(1u, 10u, 11u),
          "last=10 <= acked=11 -> deletable");
    check(!log_ack_segment_deletable(31u, 31u, 60u),
          "first=31,n=31,acked=60 (last=61) -> not deletable");
    check(log_ack_segment_deletable(31u, 31u, 61u),
          "first=31,n=31,acked=61 -> deletable");
    check(log_ack_segment_deletable(1u, 1u, 1u),
          "single record fully acked -> deletable");

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
