// =====================================================
// WP4-FW-TELEMETRY —— Telemetry Envelope（JSON 组包）
//
// ★ **freestanding**：只依赖 <stdint.h> ⇒ 可在主机 wasm32 真跑验证
//   （见 test/telemetry_host/probe_telemetry.cpp）。
//   刻意**不用** snprintf / ArduinoJson / String ⇒ 代码小、无动态分配。
//
// Envelope（字段顺序即契约 · 逐字见任务冻结清单）：
//   { "device_id":…, "ts":…, "type":…, "value":…,
//     ["source":…,] "seq":…, "schema_version":… }
//
// ⚠️ **PROVISIONAL（草案，未冻结）** —— `type` 词表 / `value` 编码 /
//    `schema_version` 初值 = `H-WP4-FW-05`（**OPEN**）；`seq` 语义 = `H-WP4-FW-07`（**OPEN**）。
//    本文件只提供**可运行草案**，不构成云端契约。
//
// ★ 不复用 command uplink：本信封**绝不**经过 `cloud_compress_uplink()`，
//   也**不得**被包成 `{"cmd":"result",…}`（H-WP4C-02）。
// =====================================================

#ifndef TELEMETRY_ENVELOPE_H
#define TELEMETRY_ENVELOPE_H

#include <stdint.h>

// envelope JSON 上界（实测最大约 216 B，留余量）
#define TELEM_ENV_MAX_BYTES 320u

// ⚠️ PROVISIONAL：`schema_version` 初值（H-WP4-FW-05 = OPEN）
#define TELEM_SCHEMA_VERSION_PROVISIONAL 1u

// 一条观测
struct TelemetryEnvelope
{
    const char *device_id;      // 必须与 /up topic 的 <device_id> 同源
    uint32_t    ts;             // Unix 秒；0 = 时间未同步（按原值上报）
    const char *type;           // ⚠️ PROVISIONAL 词表
    float       value_num;      // 数值形态（value_str == nullptr 时使用）
    const char *value_str;      // 非 nullptr ⇒ `value` 以 JSON string 输出
    const char *source;         // 可 nullptr（省略该字段）
    uint32_t    seq;            // ⚠️ PROVISIONAL 语义（H-WP4-FW-07 = OPEN）
    uint32_t    schema_version;
};

// ---- 有界 JSON 写出器（无动态分配）----

struct TeJsonOut
{
    char    *p;
    uint32_t cap;
    uint32_t n;
    bool     ok;
};

static inline void te_ch(TeJsonOut &o, char c)
{
    if (!o.ok)
    {
        return;
    }
    if (o.n + 1u >= o.cap)   // 始终留结尾 '\0'
    {
        o.ok = false;
        return;
    }
    o.p[o.n++] = c;
}

static inline void te_str(TeJsonOut &o, const char *s)
{
    while (*s != '\0')
    {
        te_ch(o, *s++);
    }
}

static inline void te_u32(TeJsonOut &o, uint32_t v)
{
    char b[10];
    uint32_t i = 0u;

    if (v == 0u)
    {
        te_ch(o, '0');
        return;
    }
    while (v > 0u)
    {
        b[i++] = (char)('0' + (int)(v % 10u));
        v /= 10u;
    }
    while (i > 0u)
    {
        te_ch(o, b[--i]);
    }
}

// 定点小数输出（2 位小数 · 去尾零）—— 不使用 snprintf
static inline void te_fixed2(TeJsonOut &o, float v)
{
    const bool neg = (v < 0.0f);

    if (neg)
    {
        v = -v;
    }

    if (!(v < 100000.0f))   // NaN / 过大 ⇒ 退化为 null（不产生非法 JSON）
    {
        te_str(o, "null");
        return;
    }

    const int32_t scaled = (int32_t)(v * 100.0f + 0.5f);
    const int32_t ip = scaled / 100;
    const int32_t fp = scaled % 100;

    if (neg && (ip != 0 || fp != 0))
    {
        te_ch(o, '-');
    }

    te_u32(o, (uint32_t)ip);

    if (fp != 0)
    {
        te_ch(o, '.');
        te_ch(o, (char)('0' + (fp / 10)));
        if ((fp % 10) != 0)
        {
            te_ch(o, (char)('0' + (fp % 10)));
        }
    }
}

// JSON 安全字符集（不实现转义 ⇒ 非法即拒绝）
static inline bool te_id_safe(const char *s)
{
    if (s == nullptr)
    {
        return false;
    }

    uint32_t n = 0u;

    while (s[n] != '\0')
    {
        const char c = s[n];
        const bool ok =
            (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
            (c >= 'A' && c <= 'Z') || c == '.' || c == '_' || c == '-';

        if (!ok)
        {
            return false;
        }

        n++;
        if (n > 32u)
        {
            return false;
        }
    }

    return n > 0u;
}

static inline bool te_word_safe(const char *s)
{
    return te_id_safe(s);   // 词表 / source 使用同一安全字符集
}

// 组包：成功返回 JSON 长度（不含结尾 '\0'），失败返回 0。
static inline uint32_t telemetry_envelope_build(const TelemetryEnvelope &e,
                                                char *out, uint32_t out_cap)
{
    if (out == nullptr || out_cap == 0u)
    {
        return 0u;
    }

    if (!te_id_safe(e.device_id) || !te_word_safe(e.type))
    {
        return 0u;
    }

    if (e.value_str != nullptr && !te_word_safe(e.value_str))
    {
        return 0u;
    }

    if (e.source != nullptr && !te_word_safe(e.source))
    {
        return 0u;
    }

    TeJsonOut o;
    o.p = out;
    o.cap = out_cap;
    o.n = 0u;
    o.ok = true;

    te_str(o, "{\"device_id\":\"");
    te_str(o, e.device_id);

    te_str(o, "\",\"ts\":");
    te_u32(o, e.ts);

    te_str(o, ",\"type\":\"");
    te_str(o, e.type);

    te_str(o, "\",\"value\":");
    if (e.value_str != nullptr)
    {
        te_ch(o, '"');
        te_str(o, e.value_str);
        te_ch(o, '"');
    }
    else
    {
        te_fixed2(o, e.value_num);
    }

    if (e.source != nullptr)
    {
        te_str(o, ",\"source\":\"");
        te_str(o, e.source);
        te_ch(o, '"');
    }

    te_str(o, ",\"seq\":");
    te_u32(o, e.seq);

    te_str(o, ",\"schema_version\":");
    te_u32(o, e.schema_version);

    te_ch(o, '}');

    if (!o.ok)
    {
        return 0u;
    }

    out[o.n] = '\0';
    return o.n;
}

#endif // TELEMETRY_ENVELOPE_H
