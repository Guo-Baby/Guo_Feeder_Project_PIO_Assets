// =====================================================
// P3 · WP4-FW-TELEMETRY —— Telemetry Policy + Envelope **真实执行**验证
//
// telemetry_policy.h / telemetry_envelope.h 都是 freestanding（只依赖 <stdint.h>），
// 因此能在**没有开发板**的机器上真实跑起来：
//
//   clang++ --target=wasm32 -std=gnu++11 -nostdlib -fno-builtin -I src \
//           test/telemetry_host/probe_telemetry.cpp \
//           -Wl,--no-entry -Wl,--export-all -o probe_telemetry.wasm
//   node run_wasm_probe.js probe_telemetry.wasm
//
// 验证点：
//   [1] policy 阈值判定（含边界 = / < / >）
//   [2] policy 抽样周期（含回绕 / period=0）
//   [3] policy 边沿去抖（首次 / 间隔内 / 间隔外）
//   [4] policy $state 迁移（lost 不可自报告）
//   [5] envelope 字段完整性（7 个冻结字段，各恰好一次）
//   [6] envelope JSON 结构合法性（大括号 / 引号配对 / 无 NaN）
//   [7] envelope 数值定点输出（正/负/零/两位小数/去尾零）
//   [8] envelope 字符串值模式
//   [9] envelope seq 行为（透传；不同 seq ⇒ 输出不同）
//   [10] envelope 负向保护（device_id / type / source / 容量）
//   [11] regression：telemetry 信封**不得**具备 command uplink 形态
// =====================================================

#include <stdint.h>
#include <stddef.h>

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

#include "services/telemetry_policy.h"
#include "services/telemetry_envelope.h"

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

static int count_sub(const char *hay, const char *needle)
{
    int c = 0;
    const unsigned nl = slen(needle);
    if (nl == 0u) { return 0; }
    for (unsigned i = 0; hay[i] != '\0'; i++)
    {
        unsigned j = 0;
        while (j < nl && hay[i + j] == needle[j]) { j++; }
        if (j == nl) { c++; }
    }
    return c;
}

static bool has_sub(const char *hay, const char *needle)
{
    return count_sub(hay, needle) > 0;
}

// JSON 合法性（本信封只允许平铺对象 + 数字 / 字符串，无嵌套 / 无转义）
static bool json_flat_object(const char *s)
{
    const unsigned n = slen(s);
    if (n < 2u || s[0] != '{' || s[n - 1u] != '}') { return false; }

    int depth = 0;
    bool in_str = false;
    unsigned quotes = 0u;

    for (unsigned i = 0; i < n; i++)
    {
        const char c = s[i];

        if (c == '"')
        {
            in_str = !in_str;
            quotes++;
            continue;
        }

        if (!in_str)
        {
            if (c == '{') { depth++; }
            else if (c == '}') { depth--; }
            else if (c == '[' || c == ']') { return false; }   // 无数组
            else if (c == '\\') { return false; }              // 无转义
        }
    }

    return depth == 0 && !in_str && (quotes % 2u) == 0u;
}

static void make_env(TelemetryEnvelope &e)
{
    e.device_id      = "aabbccddeeff";
    e.ts             = 1700000000u;
    e.type           = "climate.temperature";
    e.value_num      = 25.3f;
    e.value_str      = nullptr;
    e.source         = "mi_thermo";
    e.seq            = 1u;
    e.schema_version = TELEM_SCHEMA_VERSION_PROVISIONAL;
}

extern "C" int probe_run()
{
    g_log_len = 0;
    g_fail = 0;

    pstr("---- P3 WP4-FW-TELEMETRY policy + envelope (wasm32) ----\n");

    // ---------------------------------------------------------
    // [1] policy 阈值判定
    // ---------------------------------------------------------
    {
        check(tp_delta_exceeded(30.0f, 10.0f, 20.0f) == true,
              "delta: exactly at threshold -> exceeded (boundary)");
        check(tp_delta_exceeded(29.9f, 10.0f, 20.0f) == false,
              "delta: just below threshold -> not exceeded");
        check(tp_delta_exceeded(10.1f, 30.0f, 20.0f) == false,
              "delta: absolute value (decrease below threshold)");
        check(tp_delta_exceeded(-5.0f, -30.0f, 20.0f) == true,
              "delta: negative values handled");
    }

    // ---------------------------------------------------------
    // [2] policy 抽样周期
    // ---------------------------------------------------------
    {
        check(tp_cadence_due(1000u, 0u, 1000u) == true,  "cadence: exactly due");
        check(tp_cadence_due(999u, 0u, 1000u) == false,  "cadence: 1 ms early");
        // 回绕：(99 - 0xFFFFFFF0) mod 2^32 = 115 ⇒ 已到期
        check(tp_cadence_due(99u, 0xFFFFFFF0u, 100u) == true,
              "cadence: uint32 wraparound safe (elapsed 115 >= 100)");
        // 回绕但未到期：(50 - 0xFFFFFFF0) mod 2^32 = 66 < 100
        check(tp_cadence_due(50u, 0xFFFFFFF0u, 100u) == false,
              "cadence: wraparound but not yet due");
        check(tp_cadence_due(1000u, 0u, 0u) == false,    "cadence: period 0 -> never");
    }

    // ---------------------------------------------------------
    // [3] policy 边沿去抖
    // ---------------------------------------------------------
    {
        check(tp_edge_accept(1000u, 0u, 1000u) == true,   "edge: first ever -> accept");
        check(tp_edge_accept(1500u, 1000u, 1000u) == false, "edge: within gap -> reject");
        check(tp_edge_accept(2000u, 1000u, 1000u) == true,  "edge: exactly gap -> accept");
    }

    // ---------------------------------------------------------
    // [4] policy $state 迁移
    // ---------------------------------------------------------
    {
        check(tp_online_should_emit(TP_ONLINE_INIT, TP_ONLINE_READY) == true,
              "online: init -> ready emits");
        check(tp_online_should_emit(TP_ONLINE_READY, TP_ONLINE_READY) == false,
              "online: no change -> no emit");
        check(tp_online_should_emit(TP_ONLINE_READY, TP_ONLINE_DISCONNECTED) == true,
              "online: ready -> disconnected emits");
        check(tp_online_should_emit(TP_ONLINE_READY, TP_ONLINE_LOST) == false,
              "online: -> lost NEVER emitted (broker LWT only)");
        check(tp_online_should_emit(TP_ONLINE_DISCONNECTED, TP_ONLINE_LOST) == false,
              "online: disconnected -> lost NEVER emitted");
        check(tp_online_state_name(TP_ONLINE_READY)[0] == 'r' &&
              tp_online_state_name(TP_ONLINE_DISCONNECTED)[0] == 'd' &&
              tp_online_state_name(TP_ONLINE_INIT)[0] == 'i',
              "online: $state word list init/ready/disconnected");
    }

    // ---------------------------------------------------------
    // [5] envelope 字段完整性
    // ---------------------------------------------------------
    TelemetryEnvelope e;
    make_env(e);

    char buf[TELEM_ENV_MAX_BYTES + 1];
    memset(buf, 0, sizeof(buf));

    const uint32_t n = telemetry_envelope_build(e, buf, sizeof(buf));

    pstr("  len=");
    pdec(n);
    pch('\n');

    check(n > 0u && n < sizeof(buf), "envelope encodes");
    check(slen(buf) == n, "returned length == strlen");
    check(buf[n] == '\0', "NUL terminated");

    check(count_sub(buf, "\"device_id\":") == 1,      "field device_id x1");
    check(count_sub(buf, "\"ts\":") == 1,             "field ts x1");
    check(count_sub(buf, "\"type\":") == 1,           "field type x1");
    check(count_sub(buf, "\"value\":") == 1,          "field value x1");
    check(count_sub(buf, "\"source\":") == 1,         "field source x1");
    check(count_sub(buf, "\"seq\":") == 1,            "field seq x1");
    check(count_sub(buf, "\"schema_version\":") == 1, "field schema_version x1");

    // ---------------------------------------------------------
    // [6] JSON 结构合法性 + 精确内容
    // ---------------------------------------------------------
    {
        check(json_flat_object(buf), "json: flat object, braces/quotes balanced");
        check(!has_sub(buf, "nan") && !has_sub(buf, "inf"), "json: no nan/inf tokens");

        const char *pfx = "{\"device_id\":\"aabbccddeeff\",\"ts\":1700000000,"
                          "\"type\":\"climate.temperature\",\"value\":25.3,";
        check(memcmp(buf, pfx, slen(pfx)) == 0, "json: exact prefix (order + values)");

        check(has_sub(buf, ",\"source\":\"mi_thermo\",\"seq\":1,\"schema_version\":1}"),
              "json: exact suffix (source/seq/schema_version)");
    }

    // ---------------------------------------------------------
    // [7] 数值定点输出
    // ---------------------------------------------------------
    {
        TelemetryEnvelope t = e;
        t.source = nullptr;

        struct Case { float v; const char *want; };
        const Case cases[] = {
            {  25.3f,   "\"value\":25.3," },
            { -1.25f,   "\"value\":-1.25," },
            {   0.0f,   "\"value\":0," },
            { 100.0f,   "\"value\":100," },
            {   0.05f,  "\"value\":0.05," },
            {  -0.0f,   "\"value\":0," },
        };

        for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
        {
            t.value_num = cases[i].v;
            char b2[TELEM_ENV_MAX_BYTES];
            const uint32_t m = telemetry_envelope_build(t, b2, sizeof(b2));
            const bool ok = (m > 0u) && has_sub(b2, cases[i].want);
            check(ok, cases[i].want);
        }
    }

    // ---------------------------------------------------------
    // [8] 字符串值模式
    // ---------------------------------------------------------
    {
        TelemetryEnvelope t = e;
        t.type = "online.state";
        t.value_str = "ready";
        t.source = "mqtt";

        char b3[TELEM_ENV_MAX_BYTES];
        const uint32_t m = telemetry_envelope_build(t, b3, sizeof(b3));

        check(m > 0u && has_sub(b3, "\"value\":\"ready\""),
              "string value: \\\"value\\\":\\\"ready\\\"");
        check(m > 0u && json_flat_object(b3), "string value: json still valid");
    }

    // ---------------------------------------------------------
    // [9] seq 行为（透传；不同 seq ⇒ 不同输出）
    // ---------------------------------------------------------
    {
        TelemetryEnvelope a = e;
        TelemetryEnvelope b = e;
        a.seq = 7u;
        b.seq = 8u;

        char ba[TELEM_ENV_MAX_BYTES];
        char bb[TELEM_ENV_MAX_BYTES];
        const uint32_t la = telemetry_envelope_build(a, ba, sizeof(ba));
        const uint32_t lb = telemetry_envelope_build(b, bb, sizeof(bb));

        check(la > 0u && has_sub(ba, "\"seq\":7,"), "seq: 7 emitted verbatim");
        check(lb > 0u && has_sub(bb, "\"seq\":8,"), "seq: 8 emitted verbatim");
        check(la != lb || memcmp(ba, bb, la) != 0, "seq: different seq -> different bytes");

        a.seq = 4294967295u;
        char bc[TELEM_ENV_MAX_BYTES];
        check(telemetry_envelope_build(a, bc, sizeof(bc)) > 0u &&
              has_sub(bc, "\"seq\":4294967295,"), "seq: uint32 max verbatim");
    }

    // ---------------------------------------------------------
    // [10] 负向保护
    // ---------------------------------------------------------
    {
        TelemetryEnvelope t = e;
        char o[TELEM_ENV_MAX_BYTES];

        t.device_id = nullptr;
        check(telemetry_envelope_build(t, o, sizeof(o)) == 0u, "null device_id -> 0");
        t.device_id = "";
        check(telemetry_envelope_build(t, o, sizeof(o)) == 0u, "empty device_id -> 0");
        t.device_id = "aa\"bb";
        check(telemetry_envelope_build(t, o, sizeof(o)) == 0u, "device_id with quote -> 0");

        t = e;
        t.type = nullptr;
        check(telemetry_envelope_build(t, o, sizeof(o)) == 0u, "null type -> 0");
        t.type = "bad type";
        check(telemetry_envelope_build(t, o, sizeof(o)) == 0u, "type with space -> 0");

        t = e;
        t.source = "bad\"source";
        check(telemetry_envelope_build(t, o, sizeof(o)) == 0u, "source with quote -> 0");

        t = e;
        char tiny[16];
        check(telemetry_envelope_build(t, tiny, sizeof(tiny)) == 0u,
              "output too small -> 0 (no overrun)");

        check(telemetry_envelope_build(e, nullptr, 0u) == 0u, "null out -> 0");
    }

    // ---------------------------------------------------------
    // [11] regression：**不是** command uplink 形态
    // ---------------------------------------------------------
    {
        // 命令结果走 cloud_compress_uplink() ⇒ 短键 {"c":"result","i":…,"s":…}
        check(!has_sub(buf, "\"c\":"),   "regression: no command key \\\"c\\\"");
        check(!has_sub(buf, "\"cmd\":"), "regression: no \\\"cmd\\\"");
        check(!has_sub(buf, "\"status\":"), "regression: no \\\"status\\\"");
        check(!has_sub(buf, "\"i\":"),   "regression: no compressed id key \\\"i\\\"");

        // DT:WP-4 ingest 判定 telemetry 的必需字段（缺失即 rejected）
        check(has_sub(buf, "\"ts\":") && has_sub(buf, "\"type\":") &&
              has_sub(buf, "\"value\":") && has_sub(buf, "\"seq\":"),
              "regression: has ts/type/value/seq (ingest-acceptable shape)");

        // 冻结常量透传（本轮不得改动 command / log 侧契约）
        check(TELEM_SCHEMA_VERSION_PROVISIONAL == 1u,
              "provisional: schema_version draft == 1 (H-WP4-FW-05 OPEN)");
        check(TP_MAX_EMIT_PER_TASK == 1u, "budget: <= 1 emit per task iteration");
    }

    pstr("-----------------------------------------\n");
    pstr(g_fail == 0 ? "TELEMETRY PROBE: ALL PASS\n" : "TELEMETRY PROBE: FAIL\n");
    return g_fail;
}

extern "C" const char *probe_log_ptr() { return g_log; }
extern "C" int probe_log_len() { return g_log_len; }
