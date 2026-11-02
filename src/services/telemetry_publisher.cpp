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
#include "services/config_manager.h"   // Phase C-C1 · HD-1：运行时开关（配置唯一入口）

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

// ★★ [P4-T2 · 2026-11-02] 每个取值通道「上次发布时**看到的采样时刻戳**」。
//   与 `s_*_ms`（上次**发布**时刻，millis）是**两个不同的东西**：
//     · `s_*_ms`        = 何时发的（时间基线，给周期兜底用）
//     · `s_*_sample_ts` = 发的那条数据来自哪个采样（事件基线，给"新样本"用）
//   只有后者能回答"这次是**真的**采到新读数，还是内存里的旧值"。
uint32_t s_temp_sample_ts  = 0u;
uint32_t s_humid_sample_ts = 0u;

// (B · 表驱动) v1 取值通道描述子 —— 3 条通道逻辑同构，收敛为一张静态表
struct TeleChannel
{
    SystemStateKey key;         // System State 取值键
    const char    *type;        // telemetry `type`（T1 点分命名空间）
    float         *last;        // 上次上报值（变化阈值基线）
    uint32_t      *last_ms;     // 上次上报时刻（抽样兜底基线）
    float          thr;         // 变化阈值
    uint32_t       period_ms;   // 抽样兜底周期；**0 = 无时间兜底**（只按变化发，climate 现为此）

    // ★ [P4-T2] 「新样本」触发通道（**事件驱动**，非时间驱动）
    //   `sample_ts_key == (SystemStateKey)0xFF` ⇒ 该通道无采样时刻概念（weight）。
    SystemStateKey sample_ts_key;
    uint32_t      *last_sample_ts;   // 上次发布时的采样时刻戳
    uint32_t       sample_min_gap_ms;// 同一窗口内多帧的最小发布间隔
};

// 「该通道没有采样时刻戳」哨兵（`STATE_MAX` 之外的非法键，绝不会与真实键相等）
constexpr SystemStateKey TELE_NO_SAMPLE_TS = (SystemStateKey)0xFFFF;
static_assert((int)STATE_MAX < 0xFFFF, "TELE_NO_SAMPLE_TS must never collide with a real state key");

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

    // ★ [P4-T2] 采样时刻基线从 0 起（"尚未看过任何样本"）。
    //   不在这里读 `mi_thermo_*_ts`：上电时它们就是 0，读与不读等价，
    //   但**读一次**会让"重启前最后一个样本"被误当成"本 boot 已消费"⇒ 丢点。
    s_temp_sample_ts  = 0u;
    s_humid_sample_ts = 0u;

    // ★ [P4-T2] 此处 `s_temp` / `s_humid` **就是** `NAN`（`MiThermometerInit()`
    //   在 setup 更早的位置无条件写入，函数顺序见 main.cpp）——不修正它，
    //   而是把它如实当作"基线未建立"传给 `tp_should_emit()`（→ 首个真实样本必发）。
    //   历史缺陷正是把 NAN 当成一个可比较的数值：NaN 与任何数比较恒 false
    //   ⇒ cadence 归零后该通道再无任何放行路径 ⇒ 线上 climate 永久静默。

    s_ready = true;
}

void telemetry_task()
{
    // ★ Phase C-C1 · HD-1：运行时 telemetry 开关（**最前置** · fail-closed）
    //
    //   有效发送 = 编译门 GF_TELEMETRY_ENABLED==1 ∧ 运行时开关 true
    //   关闭时**零副作用**：直接返回 —— 不计入 emitted / transport_fail /
    //   build_fail / skipped_offline，也不进入通道遍历。
    //   读取经 ConfigManager（配置唯一入口）；本文件不含键名以外的配置知识、
    //   不解析 JSON、不持有 JsonDocument（见规格 §6.2 / §9.3 L-2）。
    if (!config_get_telemetry_enable())
    {
        return;
    }

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
          TP_CLIMATE_DELTA_TEMP_C,    TP_CLIMATE_CADENCE_MS,
          STATE_MI_THERMO_TEMP_TS,  &s_temp_sample_ts,  TP_CLIMATE_SAMPLE_MIN_GAP_MS },

        { STATE_MI_THERMO_HUMID, "climate.humidity",    &s_humid,  &s_humid_ms,
          TP_CLIMATE_DELTA_HUMID_PCT, TP_CLIMATE_CADENCE_MS,
          STATE_MI_THERMO_HUMID_TS, &s_humid_sample_ts, TP_CLIMATE_SAMPLE_MIN_GAP_MS },

        /* weight：HX711 连续采样，15 min 一次**就是**对当前重量的真实抽样
           （不是"重发缓存值"）⇒ 保留周期兜底，且无采样时刻戳通道。 */
        { STATE_WEIGHT_VALUE,    "weight.value",        &s_weight, &s_weight_ms,
          TP_WEIGHT_DELTA_G,          TP_WEIGHT_PERIOD_MS,
          TELE_NO_SAMPLE_TS,        nullptr,            0u },
    };

    for (uint8_t i = 0u; i < (uint8_t)(sizeof(kChannels) / sizeof(kChannels[0])); i++)
    {
        const TeleChannel &c = kChannels[i];

        if (!state_is_valid(c.key))
        {
            continue;
        }

        const float v = state_get_float(c.key);

        /* ★ [P4-T2] 非有限读数（NAN / Inf）一律不发：
           `state_is_valid()` 在 `state_set_float()` 里被无条件置 true
           ⇒ 它**不**能证明"有读数"。把 NAN 发出去只会变成信封里的
           `"value":null`（见 telemetry_envelope.h `te_fixed2`）⇒ Worker
           按形状校验拒收，白占一条 transport 配额。 */
        if (!tp_finite(v))
        {
            continue;
        }

        /* 该通道当前的采样时刻戳（0 = 从未采到；重量通道无此概念，恒 0） */
        const uint32_t sample_ts = (c.sample_ts_key != TELE_NO_SAMPLE_TS)
            ? (uint32_t)state_get_long(c.sample_ts_key)
            : 0u;

        /*
         * ★★ [P4-T1 · 2026-11-02] 统一走 `tp_should_emit()`（策略层纯函数）。
         *   climate 的 `period_ms` 现为 0 ⇒ 只有**数值真的变化**才发，
         *   不再每 5 min 把缓存旧值当新样本重发（那是图表同值点的唯一来源）。
         *
         * ★★ [P4-T2 · 2026-11-02] 追加「真实新样本」通道（**事件驱动**）：
         *   采样时刻戳推进 ⇒ 传感器确实报了新读数 ⇒ 即使数值没跨过阈值也要发。
         *   没有新样本时戳不变 ⇒ 永不因"时间到了"重发缓存值（与 T1 不冲突）。
         */
        bool emit = tp_should_emit(v, *c.last, c.thr, now, *c.last_ms, c.period_ms);

        if (!emit && c.sample_ts_key != TELE_NO_SAMPLE_TS)
        {
            emit = tp_sample_advanced(sample_ts, *c.last_sample_ts)
                && tp_gap_elapsed(now, *c.last_ms, c.sample_min_gap_ms);
        }

        if (!emit)
        {
            continue;
        }

        if (tele_emit_num(c.type, v))
        {
            *c.last    = v;
            *c.last_ms = now;
            if (c.sample_ts_key != TELE_NO_SAMPLE_TS)
            {
                /* 记录"这条数据来自哪个采样" ⇒ 同一采样的重复帧不会再触发 */
                *c.last_sample_ts = sample_ts;
            }
            return;   // 每轮最多一条（TP_MAX_EMIT_PER_TASK）
        }
    }
}

TelemetryStats telemetry_get_stats()
{
    return s_stats;
}

#endif  // GF_TELEMETRY_ENABLED
