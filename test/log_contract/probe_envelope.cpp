// =====================================================
// P3 · WP4-FW-LOG —— Cloud Log Envelope Producer **真实执行**的验证
//
// log_envelope.{h,cpp} 是 freestanding 的（只依赖 stdint/stddef +
// log_events.h / log_cbor.h），所以能在**没有开发板**的机器上真实跑起来：
//
//   clang++ --target=wasm32 -std=gnu++11 -nostdlib -fno-builtin -I src \
//           test/log_contract/probe_envelope.cpp src/log/log_envelope.cpp \
//           -Wl,--no-entry -Wl,--export-all -o probe_envelope.wasm
//   node test/log_contract/run_wasm_probe.js probe_envelope.wasm
//
// 验证点：
//   ① 字段完整性：envelope 恰好含 6 个冻结字段，取值正确
//   ② payload_base64 可逆：解码后与直接编码的 CBOR **逐字节相等**
//   ③ 原 log path regression：count=1 的 CBOR 批次仍走冻结编码器，
//      长度 = 188 上界公式，且**逐字节等于** log_cbor_encode_batch(count=1)
//   ④ device_id 与容量参数的负向保护（拒绝而非越界）
//   ⑤ envelope JSON 前缀 / 后缀 / 总长精确匹配
// =====================================================

#include <stdint.h>
#include <stddef.h>

// ---- freestanding libc 原语（避免编译器产生未定义 libcall）----

extern "C" void *memcpy(void *dst, const void *src, unsigned long n)
{
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    for (unsigned long i = 0; i < n; i++) { d[i] = s[i]; }
    return dst;
}

extern "C" void *memset(void *dst, int c, unsigned long n)
{
    unsigned char *d = (unsigned char *)dst;
    for (unsigned long i = 0; i < n; i++) { d[i] = (unsigned char)c; }
    return dst;
}

extern "C" int memcmp(const void *a, const void *b, unsigned long n)
{
    const unsigned char *x = (const unsigned char *)a;
    const unsigned char *y = (const unsigned char *)b;
    for (unsigned long i = 0; i < n; i++)
    {
        if (x[i] != y[i]) { return (int)x[i] - (int)y[i]; }
    }
    return 0;
}

#include "log_envelope.h"

// ---- 极简输出 ----

static char g_log[16384];
static int  g_log_len = 0;
static int  g_fail = 0;

static void pch(char c)
{
    if (g_log_len < (int)sizeof(g_log) - 1) { g_log[g_log_len++] = c; }
}

static void pstr(const char *s)
{
    while (*s != '\0') { pch(*s++); }
}

static void pdec(uint32_t v)
{
    char b[12];
    int i = 0;
    if (v == 0u) { pch('0'); return; }
    while (v > 0u) { b[i++] = (char)('0' + (int)(v % 10u)); v /= 10u; }
    while (i > 0) { pch(b[--i]); }
}

static void check(int cond, const char *what)
{
    pstr(cond ? "  [OK  ] " : "  [FAIL] ");
    pstr(what);
    pch('\n');
    if (cond == 0) { g_fail++; }
}

static unsigned slen(const char *s)
{
    unsigned n = 0;
    while (s[n] != '\0') { n++; }
    return n;
}

// 统计 hay 中 needle 出现次数
static int count_sub(const char *hay, const char *needle)
{
    int c = 0;
    const unsigned nl = slen(needle);
    for (unsigned i = 0; hay[i] != '\0'; i++)
    {
        unsigned j = 0;
        while (j < nl && hay[i + j] == needle[j]) { j++; }
        if (j == nl && nl > 0u) { c++; }
    }
    return c;
}

// ---- 探针自带的 Base64 解码（用于"可逆性"验证）----

static int b64_val(char c)
{
    if (c >= 'A' && c <= 'Z') { return (int)(c - 'A'); }
    if (c >= 'a' && c <= 'z') { return (int)(c - 'a') + 26; }
    if (c >= '0' && c <= '9') { return (int)(c - '0') + 52; }
    if (c == '+') { return 62; }
    if (c == '/') { return 63; }
    return -1;
}

// 返回解码字节数；非法字符 / 容量不足 → -1
static int b64_decode(const char *in, unsigned in_len, uint8_t *out, unsigned out_cap)
{
    if (in_len == 0u || (in_len % 4u) != 0u) { return -1; }

    unsigned o = 0;

    for (unsigned i = 0; i < in_len; i += 4u)
    {
        int v[4];
        for (unsigned k = 0; k < 4u; k++)
        {
            const char c = in[i + k];
            v[k] = (c == '=') ? -2 : b64_val(c);
            if (v[k] == -1) { return -1; }
        }

        const unsigned bytes = (v[2] == -2) ? 1u : ((v[3] == -2) ? 2u : 3u);

        if (o + bytes > out_cap) { return -1; }

        const int a = v[0] & 63;
        const int b = (v[1] == -2) ? 0 : (v[1] & 63);
        const int c = (v[2] == -2) ? 0 : (v[2] & 63);
        const int d = (v[3] == -2) ? 0 : (v[3] & 63);

        const uint32_t w = ((uint32_t)a << 18) | ((uint32_t)b << 12) |
                           ((uint32_t)c << 6) | (uint32_t)d;

        if (bytes >= 1u) { out[o++] = (uint8_t)(w >> 16); }
        if (bytes >= 2u) { out[o++] = (uint8_t)(w >> 8); }
        if (bytes >= 3u) { out[o++] = (uint8_t)w; }
    }

    return (int)o;
}

// 构造一个确定性的测试记录
static void make_rec(LogRecord &r, uint32_t seq, uint32_t ts)
{
    memset(&r, 0, sizeof(r));
    r.version = (uint8_t)LOG_RECORD_VERSION;
    r.level = (uint8_t)LOG_LVL_ERROR;      // 3
    r.flags = LOG_FLAG_TIMESTAMP_VALID;
    r.param_count = 0;
    r.seq = seq;
    r.boot_seq = 7u;
    r.event_id = LOG_MQTT_DISCONNECTED;    // 0x0702
    r.uptime_ms = 1000u;
    r.timestamp = ts;
}

extern "C" int probe_run()
{
    g_log_len = 0;
    g_fail = 0;

    pstr("---- P3 WP4-FW-LOG Cloud Log Envelope Producer (wasm32) ----\n");

    // ---- 契约一致性（编译期 + 常量）----
    static_assert(LOG_ENV_RECORDS_PER_MSG == 1u, "count must be 1 (frozen)");

    check(log_cbor_batch_max_size(LOG_ENV_RECORDS_PER_MSG) == LOG_ENV_CBOR_BYTES,
          "LOG_ENV_CBOR_BYTES == log_cbor_batch_max_size(1) == 188");

    const char *DEVID = "aabbccddeeff";   // 12 位小写 hex（设备身份契约）

    // ---------------------------------------------------------
    // [1] 单条 CBOR 批次（count = 1）：原 log path regression
    // ---------------------------------------------------------
    LogRecord rec;
    make_rec(rec, 0x11223344u, 1700000000u);   // 2023-11-14T22:13:20Z

    LogEnvelopeMeta meta;
    meta.drop_ring = 0;
    meta.drop_overflow = 0;
    meta.drop_unacked = 0;
    meta.self_degraded = 0;
    meta.flags = LOG_BATCH_FLAG_SEQ_RELIABLE;

    uint8_t cbor[LOG_ENV_CBOR_BYTES];
    memset(cbor, 0xAA, sizeof(cbor));

    const uint32_t cbor_len = log_envelope_build_cbor(rec, meta, cbor, sizeof(cbor));

    // 期望：array12(1) + 11 uints(1+1+1+5+5+1+1+1+1+1+1=19) + array1(1)
    //       + bstr 前缀(2) + 128 = 151
    pstr("  cbor_len=");
    pdec(cbor_len);
    pch('\n');

    check(cbor_len == 151u, "count=1 batch length == 151 (deterministic)");
    check(cbor[0] == 0x8Cu, "header = array(12) 0x8C (frozen)");
    check(cbor[1] == 0x02u, "fmt = LOG_BATCH_FMT(2)");
    check(cbor[2] == 0x02u, "event_dict_ver = LOG_RECORD_VERSION(2)");
    check(cbor[3] == 0x07u, "boot_seq = 7");
    check(cbor[4] == 0x1Au && cbor[5] == 0x11u && cbor[6] == 0x22u &&
          cbor[7] == 0x33u && cbor[8] == 0x44u, "seq_from = 0x11223344 (uint32)");
    check(cbor[9] == 0x1Au && cbor[13] == 0x44u, "seq_to = 0x11223344 (uint32)");
    check(cbor[14] == 0x01u, "count = 1");
    check(cbor[19] == 0x01u, "flags = seq_reliable");
    check(cbor[20] == 0x81u, "records = array(1)");
    check(cbor[21] == 0x58u && cbor[22] == 0x80u, "record[0] = bstr(128) prefix");
    check(memcmp(&cbor[23], (const uint8_t *)&rec, LOG_RECORD_SIZE) == 0,
          "records[0] keeps frozen 128 B layout (byte-for-byte)");
    check(cbor[cbor_len] == 0xAAu, "no overrun (sentinel intact)");

    // 与直接调用冻结编码器**逐字节一致** ⇒ 原 log path 未被改变
    {
        LogCborBatchHeader h;
        h.fmt = LOG_BATCH_FMT;
        h.event_dict_ver = LOG_RECORD_VERSION;
        h.boot_seq = rec.boot_seq;
        h.seq_from = rec.seq;
        h.seq_to = rec.seq;
        h.count = 1u;
        h.drop_ring = meta.drop_ring;
        h.drop_overflow = meta.drop_overflow;
        h.drop_unacked = meta.drop_unacked;
        h.self_degraded = meta.self_degraded;
        h.flags = meta.flags;

        uint8_t ref[LOG_ENV_CBOR_BYTES];
        const uint32_t ref_len = log_cbor_encode_batch(h, &rec, ref, sizeof(ref));

        check(ref_len == cbor_len && memcmp(ref, cbor, cbor_len) == 0,
              "envelope CBOR == frozen log_cbor_encode_batch(count=1)");
    }

    // ---------------------------------------------------------
    // [2] Envelope：字段完整性 / 前缀 / 后缀 / 长度
    // ---------------------------------------------------------
    char env[LOG_ENV_MAX_BYTES + 1];
    memset(env, 0, sizeof(env));

    const uint32_t env_len =
        log_envelope_encode_record(rec, meta, DEVID, env, sizeof(env));

    pstr("  env_len=");
    pdec(env_len);
    pch('\n');

    check(env_len > 0u && env_len < sizeof(env), "envelope encodes");
    check(slen(env) == env_len, "returned length == strlen (NUL-terminated)");
    check(env[env_len] == '\0', "NUL terminated at returned length");

    // 六个冻结字段各恰好出现一次
    check(count_sub(env, "\"device_id\":") == 1, "field device_id x1");
    check(count_sub(env, "\"boot_seq\":") == 1, "field boot_seq x1");
    check(count_sub(env, "\"seq\":") == 1, "field seq x1");
    check(count_sub(env, "\"ts\":") == 1, "field ts x1");
    check(count_sub(env, "\"level\":") == 1, "field level x1");
    check(count_sub(env, "\"payload_base64\":") == 1, "field payload_base64 x1");

    // 前缀（字段顺序与取值）
    {
        const char *pfx =
            "{\"device_id\":\"aabbccddeeff\",\"boot_seq\":7,\"seq\":287454020,"
            "\"ts\":1700000000,\"level\":3,\"payload_base64\":\"";
        check(env_len > slen(pfx) && memcmp(env, pfx, slen(pfx)) == 0,
              "prefix: field order + scalar values exact");
    }

    // 后缀
    check(env[env_len - 2u] == '"' && env[env_len - 1u] == '}',
          "suffix: '\"}' terminates payload_base64 + object");

    // ---------------------------------------------------------
    // [3] payload_base64 可逆（解码 == 原始 CBOR 字节）
    // ---------------------------------------------------------
    {
        const char *p = env;
        const char *key = "\"payload_base64\":\"";
        unsigned klen = slen(key);

        // 定位 key
        unsigned at = 0;
        for (; p[at] != '\0'; at++)
        {
            unsigned j = 0;
            while (j < klen && p[at + j] == key[j]) { j++; }
            if (j == klen) { break; }
        }

        const char *b64 = &p[at + klen];

        unsigned blen = 0;
        while (b64[blen] != '"' && b64[blen] != '\0') { blen++; }

        check(blen == LOG_ENV_B64_BYTES - 48u, "base64 length == 204 (ceil(151/3)*4)");
        check(b64[blen - 1u] == '=' && b64[blen - 2u] == '=',
              "single-byte remainder -> '==' padding");

        uint8_t dec[LOG_ENV_CBOR_BYTES];
        memset(dec, 0, sizeof(dec));
        const int dlen = b64_decode(b64, blen, dec, sizeof(dec));

        check(dlen == (int)cbor_len, "decoded length == cbor_len");
        check(dlen > 0 && memcmp(dec, cbor, (unsigned)dlen) == 0,
              "payload_base64 round-trips to the exact CBOR bytes");

        // 独立核对：base64 API 直接调用的输出与 envelope 内嵌片段一致
        char b64b[LOG_ENV_B64_BYTES];
        const uint32_t b64b_len = log_envelope_base64(cbor, cbor_len, b64b, sizeof(b64b));
        check(b64b_len == blen && memcmp(b64b, b64, blen) == 0,
              "standalone base64() matches embedded fragment");
    }

    // ---------------------------------------------------------
    // [4] 第二条记录（小 seq / 1 字节整数 / ts=0）——覆盖编码分支
    // ---------------------------------------------------------
    {
        LogRecord r2;
        make_rec(r2, 23u, 0u);
        r2.level = (uint8_t)LOG_LVL_INFO;     // 1
        r2.boot_seq = 1u;

        LogEnvelopeMeta m2;
        m2.drop_ring = 0;
        m2.drop_overflow = 0;
        m2.drop_unacked = 0;
        m2.self_degraded = 0;
        m2.flags = 0;

        // 直接 log_cbor_encode_batch(count=1) 与 envelope 内部构造一致
        LogCborBatchHeader h2;
        h2.fmt = LOG_BATCH_FMT;
        h2.event_dict_ver = LOG_RECORD_VERSION;
        h2.boot_seq = 1u;
        h2.seq_from = 23u;
        h2.seq_to = 23u;
        h2.count = 1u;
        h2.drop_ring = 0;
        h2.drop_overflow = 0;
        h2.drop_unacked = 0;
        h2.self_degraded = 0;
        h2.flags = 0;

        uint8_t ref2[LOG_ENV_CBOR_BYTES];
        const uint32_t ref2_len = log_cbor_encode_batch(h2, &r2, ref2, sizeof(ref2));

        uint8_t c2[LOG_ENV_CBOR_BYTES];
        const uint32_t c2_len = log_envelope_build_cbor(r2, m2, c2, sizeof(c2));

        check(ref2_len == c2_len && memcmp(ref2, c2, c2_len) == 0,
              "record#2: CBOR matches frozen encoder");
        check(c2[4] == 23u, "v=23 inline (1 B) for seq");

        char e2[LOG_ENV_MAX_BYTES + 1];
        const uint32_t e2_len = log_envelope_encode_record(r2, m2, DEVID, e2, sizeof(e2));
        check(e2_len > 0u, "record#2 envelope encodes");
        check(count_sub(e2, "\"ts\":0,") == 1, "record#2 ts=0 preserved (not dropped)");
        check(count_sub(e2, "\"seq\":23,") == 1, "record#2 seq=23 preserved");
        check(count_sub(e2, "\"level\":1,") == 1, "record#2 level=1 preserved");
    }

    // ---------------------------------------------------------
    // [5] 负向：device_id / 容量 / 空输入
    // ---------------------------------------------------------
    {
        char out[LOG_ENV_MAX_BYTES + 1];

        check(log_envelope_encode_record(rec, meta, "", out, sizeof(out)) == 0u,
              "empty device_id -> 0");
        check(log_envelope_encode_record(rec, meta, nullptr, out, sizeof(out)) == 0u,
              "null device_id -> 0");
        check(log_envelope_encode_record(rec, meta, "abc\"def", out, sizeof(out)) == 0u,
              "device_id with quote -> 0 (no escaping)");
        check(log_envelope_encode_record(rec, meta, "abc\\def", out, sizeof(out)) == 0u,
              "device_id with backslash -> 0");

        char tiny[16];
        check(log_envelope_encode_record(rec, meta, DEVID, tiny, sizeof(tiny)) == 0u,
              "output too small -> 0 (no overrun)");

        check(log_envelope_encode_cbor(nullptr, 0u, 0u, 0u, 0u, 0u, DEVID, out, sizeof(out)) == 0u,
              "null cbor -> 0");
        check(log_envelope_encode_cbor(cbor, 0u, 0u, 0u, 0u, 0u, DEVID, out, sizeof(out)) == 0u,
              "zero-length cbor -> 0");

        check(log_envelope_build_cbor(rec, meta, nullptr, 0u) == 0u,
              "null cbor out -> 0");

        uint8_t small[64];
        check(log_envelope_build_cbor(rec, meta, small, sizeof(small)) == 0u,
              "cbor cap too small -> 0 (encoder keeps record)");
    }

    // ---------------------------------------------------------
    // [6] Base64 单元测试（含 padding 边界）
    // ---------------------------------------------------------
    {
        char b[16];

        check(log_envelope_base64((const uint8_t *)"", 0u, b, sizeof(b)) == 0u,
              "base64 empty -> 0");

        // "f"    -> "Zg=="
        check(log_envelope_base64((const uint8_t *)"f", 1u, b, sizeof(b)) == 4u &&
              b[0] == 'Z' && b[1] == 'g' && b[2] == '=' && b[3] == '=',
              "base64('f') == 'Zg=='");

        // "fo"   -> "Zm8="
        check(log_envelope_base64((const uint8_t *)"fo", 2u, b, sizeof(b)) == 4u &&
              b[0] == 'Z' && b[1] == 'm' && b[2] == '8' && b[3] == '=',
              "base64('fo') == 'Zm8='");

        // "foo"  -> "Zm9v"
        check(log_envelope_base64((const uint8_t *)"foo", 3u, b, sizeof(b)) == 4u &&
              b[0] == 'Z' && b[1] == 'm' && b[2] == '9' && b[3] == 'v',
              "base64('foo') == 'Zm9v'");

        // 容量不足 -> 0
        char b2[3];
        check(log_envelope_base64((const uint8_t *)"foo", 3u, b2, sizeof(b2)) == 0u,
              "base64 cap too small -> 0");
    }

    pstr("-----------------------------------------\n");
    pstr(g_fail == 0 ? "ENVELOPE PROBE: ALL PASS\n" : "ENVELOPE PROBE: FAIL\n");
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
