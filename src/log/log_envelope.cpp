// =====================================================
// WP4-FW-LOG —— Cloud Log Envelope Producer（实现）
//
// ★ freestanding：只依赖 <stdint.h> / <stddef.h> + log_events.h / log_cbor.h。
//   刻意不用 <string.h> / ArduinoJson —— 保证可在 wasm32 主机上真跑验证，
//   也避免在固件里为几十字节的拼接引入额外代码体积。
//
// 依据见 src/log/log_envelope.h 顶部注释与
// docs/review/P3-WP4-Firmware-Log-Implementation.md
// =====================================================

#include "log/log_envelope.h"

// ---- Base64 ----

uint32_t log_envelope_base64(const uint8_t *in, uint32_t in_len,
                             char *out, uint32_t out_cap)
{
    static const char TBL[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    if (in == nullptr || out == nullptr)
    {
        return 0u;
    }

    const uint32_t need = ((in_len + 2u) / 3u) * 4u;

    if (need == 0u || out_cap < need)
    {
        return 0u;
    }

    uint32_t o = 0u;
    uint32_t i = 0u;

    while (i + 3u <= in_len)
    {
        const uint32_t v = ((uint32_t)in[i] << 16) |
                           ((uint32_t)in[i + 1u] << 8) |
                           (uint32_t)in[i + 2u];

        out[o++] = TBL[(v >> 18) & 0x3Fu];
        out[o++] = TBL[(v >> 12) & 0x3Fu];
        out[o++] = TBL[(v >> 6) & 0x3Fu];
        out[o++] = TBL[v & 0x3Fu];

        i += 3u;
    }

    const uint32_t rem = in_len - i;

    if (rem == 1u)
    {
        const uint32_t v = (uint32_t)in[i] << 16;

        out[o++] = TBL[(v >> 18) & 0x3Fu];
        out[o++] = TBL[(v >> 12) & 0x3Fu];
        out[o++] = '=';
        out[o++] = '=';
    }
    else if (rem == 2u)
    {
        const uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1u] << 8);

        out[o++] = TBL[(v >> 18) & 0x3Fu];
        out[o++] = TBL[(v >> 12) & 0x3Fu];
        out[o++] = TBL[(v >> 6) & 0x3Fu];
        out[o++] = '=';
    }

    return o;
}

// ---- CBOR（count = 1）----

uint32_t log_envelope_build_cbor(const LogRecord &rec,
                                 const LogEnvelopeMeta &meta,
                                 uint8_t *out, uint32_t out_cap)
{
    if (out == nullptr)
    {
        return 0u;
    }

    // 复用冻结的 CBOR 批次编码器：**只把 count 定为 1**，其余键值/布局一字不动。
    // （单条记录批次 = 12 项定序数组 + 1 × bstr(128) = 188 B）
    LogCborBatchHeader h;
    h.fmt = LOG_BATCH_FMT;
    h.event_dict_ver = LOG_RECORD_VERSION;
    h.boot_seq = rec.boot_seq;
    h.seq_from = rec.seq;
    h.seq_to = rec.seq;
    h.count = LOG_ENV_RECORDS_PER_MSG;
    h.drop_ring = meta.drop_ring;
    h.drop_overflow = meta.drop_overflow;
    h.drop_unacked = meta.drop_unacked;
    h.self_degraded = meta.self_degraded;
    h.flags = meta.flags;

    return log_cbor_encode_batch(h, &rec, out, (size_t)out_cap);
}

// ---- JSON 写出器（有界，无动态分配）----

namespace
{

struct JsonOut
{
    char *p;
    uint32_t cap;
    uint32_t n;
    bool ok;
};

void jo_ch(JsonOut &o, char c)
{
    if (!o.ok)
    {
        return;
    }

    // 始终为结尾 '\0' 留一个字节
    if (o.n + 1u >= o.cap)
    {
        o.ok = false;
        return;
    }

    o.p[o.n++] = c;
}

void jo_str(JsonOut &o, const char *s)
{
    while (*s != '\0')
    {
        jo_ch(o, *s++);
    }
}

void jo_u32(JsonOut &o, uint32_t v)
{
    char b[10];
    uint32_t i = 0u;

    if (v == 0u)
    {
        jo_ch(o, '0');
        return;
    }

    while (v > 0u)
    {
        b[i++] = (char)('0' + (int)(v % 10u));
        v /= 10u;
    }

    while (i > 0u)
    {
        jo_ch(o, b[--i]);
    }
}

// device_id 的 JSON 安全性：非空、长度 ≤ 32、字符集 [0-9A-Za-z._-]。
// 设备契约（services/device_identity.h）产出 12 位小写 hex，必然满足；
// 这里刻意保守 —— 不实现转义，遇到非法字符直接拒绝（记录留在队列，下轮再试）。
bool device_id_json_safe(const char *s)
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
            (c >= '0' && c <= '9') ||
            (c >= 'a' && c <= 'z') ||
            (c >= 'A' && c <= 'Z') ||
            c == '.' || c == '_' || c == '-';

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

} // namespace

// ---- 核心：CBOR bytes → JSON envelope ----

uint32_t log_envelope_encode_cbor(const uint8_t *cbor, uint32_t cbor_len,
                                  uint32_t boot_seq, uint32_t seq, uint32_t ts,
                                  uint8_t level, const char *device_id,
                                  char *out, uint32_t out_cap)
{
    if (cbor == nullptr || cbor_len == 0u || out == nullptr || out_cap == 0u)
    {
        return 0u;
    }

    if (!device_id_json_safe(device_id))
    {
        return 0u;
    }

    char b64[LOG_ENV_B64_BYTES];
    const uint32_t b64_len =
        log_envelope_base64(cbor, cbor_len, b64, (uint32_t)sizeof(b64));

    if (b64_len == 0u)
    {
        return 0u;
    }

    JsonOut o;
    o.p = out;
    o.cap = out_cap;
    o.n = 0u;
    o.ok = true;

    jo_ch(o, '{');

    jo_str(o, "\"device_id\":\"");
    jo_str(o, device_id);
    jo_str(o, "\",");

    jo_str(o, "\"boot_seq\":");
    jo_u32(o, boot_seq);
    jo_ch(o, ',');

    jo_str(o, "\"seq\":");
    jo_u32(o, seq);
    jo_ch(o, ',');

    jo_str(o, "\"ts\":");
    jo_u32(o, ts);
    jo_ch(o, ',');

    jo_str(o, "\"level\":");
    jo_u32(o, (uint32_t)level);
    jo_ch(o, ',');

    jo_str(o, "\"payload_base64\":\"");

    for (uint32_t i = 0u; i < b64_len; i++)
    {
        jo_ch(o, b64[i]);
    }

    jo_str(o, "\"}");

    if (!o.ok)
    {
        return 0u;
    }

    out[o.n] = '\0';

    return o.n;
}

// ---- 便捷封装 ----

uint32_t log_envelope_encode_record(const LogRecord &rec,
                                    const LogEnvelopeMeta &meta,
                                    const char *device_id,
                                    char *out, uint32_t out_cap)
{
    uint8_t cbor[LOG_ENV_CBOR_BYTES];

    const uint32_t cbor_len =
        log_envelope_build_cbor(rec, meta, cbor, (uint32_t)sizeof(cbor));

    if (cbor_len == 0u)
    {
        return 0u;
    }

    return log_envelope_encode_cbor(cbor, cbor_len,
                                    rec.boot_seq, rec.seq, rec.timestamp,
                                    rec.level, device_id, out, out_cap);
}
