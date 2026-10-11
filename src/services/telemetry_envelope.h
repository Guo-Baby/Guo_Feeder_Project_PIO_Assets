// =====================================================
// WP4-FW-TELEMETRY —— Telemetry Envelope（JSON 组包）
//
// ★ **freestanding**：只依赖 <stdint.h> ⇒ 可在主机 wasm32 真跑验证
//   （见 test/telemetry_host/probe_telemetry.cpp）。
//   刻意**不用** snprintf / ArduinoJson / String ⇒ 代码小、无动态分配。
//
// Envelope **v1（已冻结）** —— 权威：`docs/review/P3-WP4-Firmware-Telemetry-Decisions.md` §1
//
//   { "ts":…, "type":"…", "value":…, "seq":…, "schema_version":… }
//
//   ★ 字段顺序即契约；**5 字段**。
//   ★ **无 `device_id`** —— 设备身份由 MQTT topic 承载（`guo_feeder/<device_id>/up` 第 2 段）。
//   ★ **无 `source`** —— 来源由 `type` 的域名前缀隐含。
//
// value 编码（已冻结 · 同文档 §2）：
//   number  -> JSON **number**   例 `25.3`
//   state   -> JSON **string**   例 `"ready"`（词表由 type 绑定）
//   boolean -> JSON **boolean**  例 `true` / `false`
//   🔴 **禁止**用 `0` / `1` 数字编码 boolean。
//
// ★ 不复用 command uplink：本信封**绝不**经过 `cloud_compress_uplink()`，
//   也**不得**被包成 `{"cmd":"result",…}`（H-WP4C-02）。
// =====================================================

#ifndef TELEMETRY_ENVELOPE_H
#define TELEMETRY_ENVELOPE_H

#include <stdint.h>

// envelope JSON 上界（v1 最坏情形 ≈ 147 B：uint32 满值 + type/value 各 32 字符）
#define TELEM_ENV_MAX_BYTES 192u

// ★ 冻结：schema_version v1 恒为 1
//   （云端 `telemetry_history.schema_version INTEGER NOT NULL DEFAULT 1`；
//    `WP2:§3.3` 把 `value` 的逻辑类型解释权交给它 ⇒ 前向兼容锚点）
#define TELEM_SCHEMA_VERSION 1u

// value 的逻辑类型（决定 JSON 形态 · 冻结三态）
enum TeValueKind
{
    TE_VAL_NUM  = 0,   // JSON number
    TE_VAL_STR  = 1,   // JSON string
    TE_VAL_BOOL = 2    // JSON boolean（true / false —— 禁止 0/1）
};

// 一条观测（v1 信封 · 5 字段）
struct TelemetryEnvelope
{
    uint32_t     ts;              // Unix 秒；0 = 时间未同步（按原值上报）
    const char  *type;            // 类型标识符（T1 点分命名空间；追加式演进）
    TeValueKind  kind;            // value 的逻辑类型（决定 JSON 形态）
    float        value_num;       // kind == TE_VAL_NUM 时使用
    const char  *value_str;       // kind == TE_VAL_STR 时使用（须 JSON 安全）
    bool         value_bool;      // kind == TE_VAL_BOOL 时使用
    uint32_t     seq;             // boot lifetime 计数（不持久化）
    uint32_t     schema_version;  // v1 = 1
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

// JSON 安全标识符（type / value 字符串共用 · 不实现转义 ⇒ 非法即拒绝）
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

// JSON boolean 字面量（**不是** 数字 0/1）
static inline void te_bool(TeJsonOut &o, bool v)
{
    te_str(o, v ? "true" : "false");
}

// 组包：成功返回 JSON 长度（不含结尾 '\0'），失败返回 0。
static inline uint32_t telemetry_envelope_build(const TelemetryEnvelope &e,
                                                char *out, uint32_t out_cap)
{
    if (out == nullptr || out_cap == 0u)
    {
        return 0u;
    }

    if (!te_id_safe(e.type))
    {
        return 0u;
    }

    if (e.kind == TE_VAL_STR && !te_id_safe(e.value_str))
    {
        return 0u;
    }

    TeJsonOut o;
    o.p = out;
    o.cap = out_cap;
    o.n = 0u;
    o.ok = true;

    te_str(o, "{\"ts\":");
    te_u32(o, e.ts);

    te_str(o, ",\"type\":\"");
    te_str(o, e.type);

    te_str(o, "\",\"value\":");
    if (e.kind == TE_VAL_STR)
    {
        te_ch(o, '"');
        te_str(o, e.value_str);
        te_ch(o, '"');
    }
    else if (e.kind == TE_VAL_BOOL)
    {
        te_bool(o, e.value_bool);
    }
    else
    {
        te_fixed2(o, e.value_num);
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
