// =====================================================
// WP4-FW-TELEMETRY —— Telemetry Publisher（实现）
//
// 数据流：System State → telemetry_policy（决策）→ telemetry_envelope（组包）
//         → CloudManager transport（cloud_send_telemetry）→ MQTT /up
//
// ★ 非阻塞：本文件**无 delay / 无 while 死等 / 无动态分配**
// ★ 不创建 Task：由 main.cpp 的 loop() 每轮调用 telemetry_task()
// ★ 不复用 command uplink：绝不经 cloud_compress_uplink()
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

bool telemetry_is_enabled()
{
    return false;
}

#else   // ---------------- GF_TELEMETRY_ENABLED == 1 ----------------

#include <Arduino.h>

#include "services/system_state.h"
#include "services/time_manager.h"
#include "services/device_identity.h"
#include "services/telemetry_policy.h"
#include "services/telemetry_envelope.h"

#include "cloud/cloud_manager.h"

namespace
{

bool            s_ready  = false;
uint32_t        s_seq    = 0u;
uint32_t        s_last_edge_ms = 0u;
TelemetryStats  s_stats  = { 0u, 0u, 0u, 0u };

// ---- 边沿基线 ----
bool s_valve     = false;
bool s_wifi      = false;
bool s_timevalid = false;
bool s_werror    = false;
int  s_online    = (int)TP_ONLINE_INIT;

// ---- 取值基线（含上次采样时刻）----
float    s_temp     = 0.0f;
float    s_humid    = 0.0f;
float    s_weight   = 0.0f;
uint32_t s_temp_ms  = 0u;
uint32_t s_humid_ms = 0u;
uint32_t s_weight_ms = 0u;

// 时间有效性由 State 决定；未同步 ⇒ ts = 0（按原值上报，不伪造）
inline uint32_t tele_ts()
{
    return state_get_bool(STATE_TIME_VALID) ? (uint32_t)time_get() : 0u;
}

// 组包 + 交运输；成功返回 true
bool tele_emit(const char *type, float num, const char *str, const char *source)
{
    TelemetryEnvelope e;
    e.device_id      = device_id();
    e.ts             = tele_ts();
    e.type           = type;
    e.value_num      = num;
    e.value_str      = str;
    e.source         = source;
    e.seq            = ++s_seq;
    e.schema_version = TELEM_SCHEMA_VERSION_PROVISIONAL;

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

inline bool tele_emit_bool(const char *type, bool v, const char *source)
{
    return tele_emit(type, v ? 1.0f : 0.0f, nullptr, source);
}

} // namespace

void telemetry_publisher_init()
{
    // 只记录基线，**不发**（避免上电即产生无意义样本）
    s_seq           = 0u;
    s_last_edge_ms  = 0u;
    s_stats.emitted = 0u;
    s_stats.transport_fail = 0u;
    s_stats.build_fail = 0u;
    s_stats.skipped_offline = 0u;

    s_valve     = state_get_bool(STATE_VALVE_STATUS);
    s_wifi      = state_get_bool(STATE_WIFI_STATUS);
    s_timevalid = state_get_bool(STATE_TIME_VALID);
    s_werror    = state_get_bool(STATE_WEIGHT_ERROR);

    s_online = state_get_bool(STATE_MQTT_STATUS)
                   ? (int)TP_ONLINE_READY
                   : (int)TP_ONLINE_DISCONNECTED;

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

    const bool tf_valid = state_get_bool(STATE_TIME_VALID);
    const bool werror   = state_get_bool(STATE_WEIGHT_ERROR);
    const bool valve    = state_get_bool(STATE_VALVE_STATUS);
    const bool wifi     = state_get_bool(STATE_WIFI_STATUS);
    const int  online   = state_get_bool(STATE_MQTT_STATUS)
                              ? (int)TP_ONLINE_READY
                              : (int)TP_ONLINE_DISCONNECTED;

    // ---- (1) 边沿类：状态迁移 / 布尔翻转（每轮最多一条）----
    // 共用 s_last_edge_ms 做去抖（边沿是稀有事件，1 s 粒度足够）

    if ((online != s_online) &&
        tp_online_should_emit(s_online, online) &&
        tp_edge_accept(now, s_last_edge_ms, TP_EDGE_MIN_GAP_MS))
    {
        if (tele_emit("online.state", 0.0f, tp_online_state_name(online), "mqtt"))
        {
            s_online = online;
            s_last_edge_ms = now;
            return;
        }
    }

    if ((tf_valid != s_timevalid) &&
        tp_edge_accept(now, s_last_edge_ms, TP_EDGE_MIN_GAP_MS))
    {
        if (tele_emit_bool("time.valid", tf_valid, "time"))
        {
            s_timevalid = tf_valid;
            s_last_edge_ms = now;
            return;
        }
    }

    if ((werror != s_werror) &&
        tp_edge_accept(now, s_last_edge_ms, TP_EDGE_MIN_GAP_MS))
    {
        if (tele_emit_bool("weight.error", werror, "weight"))
        {
            s_werror = werror;
            s_last_edge_ms = now;
            return;
        }
    }

    if ((valve != s_valve) &&
        tp_edge_accept(now, s_last_edge_ms, TP_EDGE_MIN_GAP_MS))
    {
        if (tele_emit_bool("valve.state", valve, "valve"))
        {
            s_valve = valve;
            s_last_edge_ms = now;
            return;
        }
    }

    if ((wifi != s_wifi) &&
        tp_edge_accept(now, s_last_edge_ms, TP_EDGE_MIN_GAP_MS))
    {
        if (tele_emit_bool("wifi.state", wifi, "wifi"))
        {
            s_wifi = wifi;
            s_last_edge_ms = now;
            return;
        }
    }

    // ---- (2) 取值类：变化阈值优先，其次抽样兜底 ----

    if (state_is_valid(STATE_MI_THERMO_TEMP))
    {
        const float temp = state_get_float(STATE_MI_THERMO_TEMP);

        if (tp_delta_exceeded(temp, s_temp, TP_CLIMATE_DELTA_TEMP_C) ||
            tp_cadence_due(now, s_temp_ms, TP_CLIMATE_PERIOD_MS))
        {
            if (tele_emit("climate.temperature", temp, nullptr, "mi_thermo"))
            {
                s_temp = temp;
                s_temp_ms = now;
                return;
            }
        }
    }

    if (state_is_valid(STATE_MI_THERMO_HUMID))
    {
        const float humid = state_get_float(STATE_MI_THERMO_HUMID);

        if (tp_delta_exceeded(humid, s_humid, TP_CLIMATE_DELTA_HUMID_PCT) ||
            tp_cadence_due(now, s_humid_ms, TP_CLIMATE_PERIOD_MS))
        {
            if (tele_emit("climate.humidity", humid, nullptr, "mi_thermo"))
            {
                s_humid = humid;
                s_humid_ms = now;
                return;
            }
        }
    }

    if (state_is_valid(STATE_WEIGHT_VALUE))
    {
        const float weight = state_get_float(STATE_WEIGHT_VALUE);

        if (tp_delta_exceeded(weight, s_weight, TP_WEIGHT_DELTA_G) ||
            tp_cadence_due(now, s_weight_ms, TP_WEIGHT_PERIOD_MS))
        {
            if (tele_emit("weight.value", weight, nullptr, "weight"))
            {
                s_weight = weight;
                s_weight_ms = now;
                return;
            }
        }
    }
}

TelemetryStats telemetry_get_stats()
{
    return s_stats;
}

bool telemetry_is_enabled()
{
    return true;
}

#endif  // GF_TELEMETRY_ENABLED
