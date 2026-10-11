// =====================================================
// WP4-FW-TELEMETRY —— Telemetry Publisher（实现）
//
// 数据流：System State → telemetry_policy（决策）→ telemetry_envelope（组包）
//         → CloudManager transport（cloud_send_telemetry）→ MQTT /up
//
// ★ 非阻塞：本文件**无 delay / 无 while 死等 / 无动态分配**
// ★ 不创建 Task：由 main.cpp 的 loop() 每轮调用 telemetry_task()
// ★ 不复用 command uplink：绝不经 cloud_compress_uplink()
//
// ★ v1 首版 capability 范围（E · 冻结）：只发取值类
//     KEEP  = temperature / humidity / weight
//     DEFER = valve / wifi / online / time.valid / weight.error（**不删除能力**）
// =====================================================

#include "services/telemetry_publisher.h"

// =====================================================
// 编译期门禁：默认 0 ⇒ 零 Flash / 零 RAM（主循环行为完全不变）
// =====================================================

#if !GF_TELEMETRY_ENABLED

void telemetry_publisher_init() {}

void telemetry_task() {}

TelemetryStats telemetry_get_stats()
{
    TelemetryStats s;
    s.emitted = 0u;
    s.transport_fail = 0u;
    s.build_fail = 0u;
    s.skipped_offline = 0u;
    return s;
}

#else   // ---------------- GF_TELEMETRY_ENABLED == 1 ----------------

#include <Arduino.h>

#include "services/system_state.h"
#include "services/time_manager.h"
#include "services/telemetry_policy.h"
#include "services/telemetry_envelope.h"

#include "cloud/cloud_manager.h"

namespace
{

bool            s_ready  = false;
uint32_t        s_seq    = 0u;
TelemetryStats  s_stats  = { 0u, 0u, 0u, 0u };

// ---- 取值基线（含上次采样时刻）----
//   ★ v1 首版范围（E · 已冻结）：**只保留取值类** temperature / humidity / weight。
//   ★ DEFER（**不删除能力，仅首版不产生**）：valve / wifi / online / time.valid / weight.error。
//     其 `value` 编码（state / boolean）与词表已在 Envelope（commit 1）冻结，
//     后续启用**无需重新冻结协议**，只需恢复对应分支。
float    s_temp     = 0.0f;
float    s_humid    = 0.0f;
float    s_weight   = 0.0f;
uint32_t s_temp_ms  = 0u;
uint32_t s_humid_ms = 0u;
uint32_t s_weight_ms = 0u;

// (B · 表驱动) v1 取值通道描述子 —— 3 条通道逻辑同构，收敛为一张静态表
struct TeleChannel
{
    SystemStateKey key;         // System State 取值键
    const char    *type;        // telemetry `type`（T1 点分命名空间）
    float         *last;        // 上次上报值（变化阈值基线）
    uint32_t      *last_ms;     // 上次上报时刻（抽样兜底基线）
    float          thr;         // 变化阈值
    uint32_t       period_ms;   // 抽样兜底周期
};

// 时间有效性由 State 决定；未同步 ⇒ ts = 0（按原值上报，不伪造）
inline uint32_t tele_ts()
{
    return state_get_bool(STATE_TIME_VALID) ? (uint32_t)time_get() : 0u;
}

// 组包 + 交运输；成功返回 true
//   ★ Envelope v1 已冻结（docs/review/P3-WP4-Firmware-Telemetry-Decisions.md §1/§2）：
//     5 字段 {ts,type,value,seq,schema_version} —— **不再携带 device_id / source**
//     （device_id 由 MQTT topic identity 承载）
bool tele_send(const char *type, TeValueKind kind, float num, const char *str, bool b)
{
    TelemetryEnvelope e;
    e.ts             = tele_ts();
    e.type           = type;
    e.kind           = kind;
    e.value_num      = num;
    e.value_str      = str;
    e.value_bool     = b;
    e.seq            = ++s_seq;
    e.schema_version = TELEM_SCHEMA_VERSION;

    char buf[TELEM_ENV_MAX_BYTES];
    const uint32_t n = telemetry_envelope_build(e, buf, (uint32_t)sizeof(buf));

    if (n == 0u)
    {
        s_stats.build_fail++;
        return false;
    }

    if (!cloud_send_telemetry(buf))
    {
        s_stats.transport_fail++;
        return false;
    }

    s_stats.emitted++;
    return true;
}

// number -> JSON number
inline bool tele_emit_num(const char *type, float v)
{
    return tele_send(type, TE_VAL_NUM, v, nullptr, false);
}

// state -> JSON string
inline bool tele_emit_str(const char *type, const char *v)
{
    return tele_send(type, TE_VAL_STR, 0.0f, v, false);
}

// boolean -> JSON boolean（**禁止** 0/1 数字编码）
inline bool tele_emit_bool(const char *type, bool v)
{
    return tele_send(type, TE_VAL_BOOL, 0.0f, nullptr, v);
}

} // namespace

void telemetry_publisher_init()
{
    // 只记录基线，**不发**（避免上电即产生无意义样本）
    s_seq           = 0u;
    s_stats.emitted = 0u;
    s_stats.transport_fail = 0u;
    s_stats.build_fail = 0u;
    s_stats.skipped_offline = 0u;

    s_temp   = state_get_float(STATE_MI_THERMO_TEMP);
    s_humid  = state_get_float(STATE_MI_THERMO_HUMID);
    s_weight = state_get_float(STATE_WEIGHT_VALUE);

    const uint32_t now = millis();
    s_temp_ms   = now;
    s_humid_ms  = now;
    s_weight_ms = now;

    s_ready = true;
}

void telemetry_task()
{
    if (!s_ready)
    {
        return;
    }

    if (!cloud_is_connected())
    {
        s_stats.skipped_offline++;
        return;
    }

    const uint32_t now = millis();

    // ★ v1 首版范围（E · 已冻结）：**只发取值类**。
    //   原「(1) 边沿类」5 条（online / time.valid / weight.error / valve / wifi）
    //   已按裁决 **DEFER**（首版不产生这些 telemetry）；
    //   ⚠️ **不是删除能力** —— System State 侧读取与 Envelope 编码（state/boolean）均保留，
    //   后续启用只需恢复分支（`tele_emit_str` / `tele_emit_bool` 仍在下方可用）。
    //
    // ★ (B · 表驱动去重) 取值类：变化阈值优先，其次抽样兜底。
    //   3 条通道逻辑同构 ⇒ 收敛为**一张静态表 + 一次遍历**，消除重复代码与重复字面量。

    static const TeleChannel kChannels[] =
    {
        { STATE_MI_THERMO_TEMP,  "climate.temperature", &s_temp,   &s_temp_ms,
          TP_CLIMATE_DELTA_TEMP_C,    TP_CLIMATE_PERIOD_MS },
        { STATE_MI_THERMO_HUMID, "climate.humidity",    &s_humid,  &s_humid_ms,
          TP_CLIMATE_DELTA_HUMID_PCT, TP_CLIMATE_PERIOD_MS },
        { STATE_WEIGHT_VALUE,    "weight.value",        &s_weight, &s_weight_ms,
          TP_WEIGHT_DELTA_G,          TP_WEIGHT_PERIOD_MS  },
    };

    for (uint8_t i = 0u; i < (uint8_t)(sizeof(kChannels) / sizeof(kChannels[0])); i++)
    {
        const TeleChannel &c = kChannels[i];

        if (!state_is_valid(c.key))
        {
            continue;
        }

        const float v = state_get_float(c.key);

        if (!tp_delta_exceeded(v, *c.last, c.thr) &&
            !tp_cadence_due(now, *c.last_ms, c.period_ms))
        {
            continue;
        }

        if (tele_emit_num(c.type, v))
        {
            *c.last    = v;
            *c.last_ms = now;
            return;   // 每轮最多一条（TP_MAX_EMIT_PER_TASK）
        }
    }
}

TelemetryStats telemetry_get_stats()
{
    return s_stats;
}

#endif  // GF_TELEMETRY_ENABLED
