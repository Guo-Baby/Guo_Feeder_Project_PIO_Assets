// =====================================================
// P1.4 CBOR 批次编码器 —— **真实执行**的字节级验证
//
// log_cbor.h 是 freestanding 的（只依赖 <stdint.h>/<stddef.h> + log_events.h），
// 所以可以在**没有开发板、没有 CRT** 的机器上真实跑起来：
//
//   clang++ --target=wasm32 -std=gnu++11 -nostdlib -fno-builtin \
//           -I src test/log_contract/probe_cbor.cpp \
//           -Wl,--no-entry -Wl,--export-all -o probe_cbor.wasm
//   node test/log_contract/run_cbor_probe.js probe_cbor.wasm
//
// 为什么走 wasm32：本机 clang 没有 MSVC/CRT 库（libcmt.lib 等缺失），
// 无法链接原生控制台程序；而 wasm32 只需要编译器自带的内建头，零依赖。
//
// 验证点：
//   ① 批次头 12 项的 CBOR 字节序列**逐字节**匹配
//   ② 定序数组下标 == log_events.h 冻结的 LOG_BKEY_*（0..11）
//   ③ records 以 bstr(128) 原样保留冻结的 LogRecord 布局
//   ④ 容量预检：不够 / count 超上界 / count==0 / nullptr → 返回 0 且不越界
//   ⑤ uint 编码的 23 / 24 / 65535 边界（内联 vs 0x18 vs 0x19）
// =====================================================

#include <stdint.h>
#include <stddef.h>

// ---- freestanding 下的 libc 原语（避免编译器产生未定义 libcall）----

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

#include "log_cbor.h"

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

// ---- 主体验证 ----

extern "C" int probe_run()
{
    g_log_len = 0;
    g_fail = 0;

    pstr("---- P1.4 CBOR batch encoder (wasm32) ----\n");

    // 契约一致性（编译期）
    static_assert(LOG_CBOR_BATCH_ITEMS == 12u, "batch must have 12 items");
    static_assert(LOG_BKEY_RECORDS == 11u, "records must be key 11");
    static_assert(LOG_CBOR_BATCH_ITEMS == LOG_BKEY_RECORDS + 1u,
                  "item count must equal key range 0..11");

    LogRecord recs[2];
    memset(recs, 0, sizeof(recs));

    recs[0].version = (uint8_t)LOG_RECORD_VERSION;
    recs[0].level = 2u;
    recs[0].seq = 0x12345678u;      // 强制 uint32 的 4 字节分支
    recs[0].boot_seq = 1u;
    recs[0].event_id = 0x0702u;
    recs[0].uptime_ms = 1000u;

    recs[1] = recs[0];
    recs[1].seq = 0x12345679u;
    recs[1].uptime_ms = 1100u;

    static uint8_t out[LOG_BATCH_MAX_PAYLOAD];
    memset(out, 0xAA, sizeof(out));

    LogCborBatchHeader h;
    memset(&h, 0, sizeof(h));
    h.fmt = LOG_BATCH_FMT;
    h.event_dict_ver = LOG_RECORD_VERSION;
    h.boot_seq = 1u;
    h.seq_from = 0x12345678u;
    h.seq_to = 0x12345679u;
    h.count = 2u;
    h.drop_ring = 3u;
    h.drop_overflow = 0u;
    h.drop_unacked = 0u;
    h.self_degraded = 1u;
    h.flags = LOG_BATCH_FLAG_SEQ_RELIABLE;

    const uint32_t n = log_cbor_encode_batch(h, recs, out, sizeof(out));

    // ---- 逐字节期望 ----
    // 批次头 = 12 项，共 **21 B**：
    //  0        array(12)                    0x8C
    //  1        fmt = 2                      0x02
    //  2        event_dict_ver = 2           0x02
    //  3        boot_seq = 1                 0x01
    //  4..8     seq_from = 0x12345678        0x1A 12 34 56 78
    //  9..13    seq_to   = 0x12345679        0x1A 12 34 56 79
    //  14       count = 2                    0x02
    //  15       drop_ring = 3                0x03
    //  16       drop_overflow = 0            0x00
    //  17       drop_unacked = 0             0x00
    //  18       self_degraded = 1            0x01
    //  19       flags = 1                    0x01
    //  20       array(2)                     0x82
    // 之后每条 record = bstr 前缀(0x58 0x80) + 128 B = **130 B**
    //   总长度 = 21 + 2 × 130 = 281
    static const uint8_t expect_head[21] = {
        0x8C,
        0x02, 0x02, 0x01,
        0x1A, 0x12, 0x34, 0x56, 0x78,
        0x1A, 0x12, 0x34, 0x56, 0x79,
        0x02, 0x03, 0x00, 0x00, 0x01, 0x01,
        0x82,
    };
    const uint32_t HEAD = 21u;
    const uint32_t STRIDE = 2u + (uint32_t)LOG_RECORD_SIZE;   // 130

    {
        pstr("  len=");
        pdec(n);
        pstr(" (head=");
        pdec(HEAD);
        pstr(" stride=");
        pdec(STRIDE);
        pstr(")\n");
    }

    check(n == HEAD + 2u * STRIDE, "total == 21 B head + 2 x 130 B record");
    check(memcmp(out, expect_head, sizeof(expect_head)) == 0,
          "header 21 bytes match byte-for-byte");
    check(out[HEAD] == 0x58u && out[HEAD + 1u] == 0x80u,
          "record[0] bstr prefix = 0x58 0x80 (len 128)");
    check(memcmp(&out[HEAD + 2u], (const uint8_t *)&recs[0], LOG_RECORD_SIZE) == 0,
          "records[0] keeps frozen 128 B layout");
    check(out[HEAD + STRIDE] == 0x58u && out[HEAD + STRIDE + 1u] == 0x80u,
          "record[1] bstr prefix = 0x58 0x80");
    check(memcmp(&out[HEAD + STRIDE + 2u], (const uint8_t *)&recs[1],
                 LOG_RECORD_SIZE) == 0,
          "records[1] keeps frozen 128 B layout");
    check(out[n] == 0xAAu, "no overrun (sentinel intact)");

    // ---- 容量 / 上界 ----
    check(log_cbor_batch_max_size(LOG_BATCH_MAX_RECORDS) <= LOG_BATCH_MAX_PAYLOAD,
          "16-record bound <= LOG_BATCH_MAX_PAYLOAD");

    {
        uint8_t small[16];
        check(log_cbor_encode_batch(h, recs, small, sizeof(small)) == 0u,
              "cap too small -> returns 0 (no overrun)");
    }
    {
        LogCborBatchHeader bad = h;
        bad.count = LOG_BATCH_MAX_RECORDS + 1u;
        check(log_cbor_encode_batch(bad, recs, out, sizeof(out)) == 0u,
              "count > max -> returns 0");
    }
    {
        LogCborBatchHeader zero = h;
        zero.count = 0u;
        check(log_cbor_encode_batch(zero, recs, out, sizeof(out)) == 0u,
              "count == 0 -> returns 0");
    }
    {
        check(log_cbor_encode_batch(h, 0, out, sizeof(out)) == 0u,
              "records == null -> returns 0");
    }

    // ---- uint 编码边界 ----
    {
        LogCborBatchHeader h2 = h;
        h2.count = 1u;
        h2.seq_from = 23u;
        h2.seq_to = 23u;
        h2.drop_ring = 23u;
        uint32_t n2 = log_cbor_encode_batch(h2, recs, out, sizeof(out));
        check(n2 > 0u, "single-record batch encodes");
        check(out[4] == 23u, "v=23 inline (1 B)");

        h2.drop_ring = 24u;
        n2 = log_cbor_encode_batch(h2, recs, out, sizeof(out));
        check(n2 > 0u && out[7] == 0x18u && out[8] == 24u,
              "v=24 -> 0x18 (2 B)");

        h2.drop_ring = 0xFFFFu;
        n2 = log_cbor_encode_batch(h2, recs, out, sizeof(out));
        check(n2 > 0u && out[7] == 0x19u && out[8] == 0xFFu && out[9] == 0xFFu,
              "v=65535 -> 0x19 (3 B)");
    }

    pstr("-----------------------------------------\n");
    pstr(g_fail == 0 ? "CBOR PROBE: ALL PASS\n" : "CBOR PROBE: FAIL\n");
    return g_fail;
}

extern "C" const char *probe_log_ptr()
{
    return g_log;
}

extern "C" int probe_log_len()
{
    return g_log_len;
}
