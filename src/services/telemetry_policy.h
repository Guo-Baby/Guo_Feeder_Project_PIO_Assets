// =====================================================
// WP4-FW-TELEMETRY —— Telemetry Policy（纯决策逻辑）
//
// 设计依据：
//   docs/review/P3-WP4-Firmware-Integration-Design-Review.md §3.2（两级拆分）
//   docs/architecture/P3-WP1-Telemetry-Policy-Final.md（遥测策略冻结）
//
// ★ **freestanding**：只依赖 <stdint.h> ⇒ 可用主机 clang++ 编到 wasm32
//   **真跑**做穷举验证（见 test/telemetry_host/probe_telemetry.cpp）。
//   **无 MQTT 依赖 · 无文件系统依赖 · 无 Arduino 依赖 · 无动态分配。**
//
// ⚠️⚠️ **PROVISIONAL（草案，未冻结）** ⚠️⚠️
//   下面的**阈值 / 周期 / 词表**是**可运行的默认草案**，**不是**冻结协议：
//     · `H-WP4-FW-05`（type 封闭词表 + value 编码 + schema_version 初值）= **OPEN**
//     · `H-WP4-FW-06`（`mqtt.json.telemetry` 字段集与默认值）        = **OPEN**
//     · `H-WP4-FW-07`（telemetry `seq` 语义：是否跨 boot 持久化）    = **OPEN**
//   ⇒ 本文件的所有取值在人工裁决前**不得**被当作云端契约。
//   ⇒ 本模块只做**设备侧策略**（WP-1 §1：唯一权威在设备侧）。
// =====================================================

#ifndef TELEMETRY_POLICY_H
#define TELEMETRY_POLICY_H

#include <stdint.h>

// =====================================================
// 1. 草案参数（⚠️ PROVISIONAL · 待 H-WP4-FW-05/06 裁决）
// =====================================================

// 抽样兜底周期
//   WP1:§5 H-W1-2（weight）= N = 15 min；WP1:§3.5（climate）= 5–15 min
#define TP_WEIGHT_PERIOD_MS        (15UL * 60UL * 1000UL)

// ★★ [P4-T1 · 2026-11-02] climate 通道**关闭时间兜底**（0 = 只按变化发）。
//
// ── 线上实测的缺陷（2026-11-02） ──
//   旧值 5 min ⇒ `tp_cadence_due()` 每 5 min 到期一次 ⇒ 设备把**内存里的旧读数**
//   原样再发一次（信封的 ts 是发布时刻、seq 也递增，看起来像一条新遥测）。
//   D1 `telemetry_history` 因此每台设备每 5 min 多一行：
//     实测 climate.temperature 1366 行 / climate.humidity 1353 行，
//     相邻行 ts 严格相差 300 s、value 全为 23.5；
//   而 LYWSD03MMC 的实际读数 70–80 min 才变一次
//   ⇒ 首页「历史曲线」1 小时内出现 12 个同值点，看起来像"重复采样"。
//
// ── 为什么这是根因，而不是前端/云端的问题 ──
//   ① 信封 v1 已冻结为 {ts,type,value,seq,schema_version} —— **没有采样标识**
//      （`mi_thermo_temp_ts` 是"最后一次成功解密广播帧的时刻"，每帧都会更新，
//       不是采样时间）⇒ 云端**无法**在不使用"数值相同"这一被禁止的判据下
//       区分"同一采样的重复传输"与"不同采样恰好同值"；
//   ② 因此去重只能发生在**拥有真实采样信息的那一侧**，也就是设备侧：
//      不再凭空产生"新样本"，数值没跨过阈值就不发。
//   ⇒ 取 0（关闭时间兜底）。重量通道仍保留 15 min 兜底（水位是缓变量，
//      且本轮未收到相关反馈，不做扩大改动）。
#define TP_CLIMATE_CADENCE_MS      (0UL)
/** @deprecated 旧名，保留以便历史文档/评审记录按原名检索；语义同 CADENCE。 */
#define TP_CLIMATE_PERIOD_MS       TP_CLIMATE_CADENCE_MS

// 事件类边沿去重的最小间隔（防止抖动刷屏；非采样周期）
#define TP_EDGE_MIN_GAP_MS         1000UL

// 变化阈值（⚠️ WP1 明令**不冻结**阈值 X ⇒ 这里是草案默认值，不是契约）
#define TP_WEIGHT_DELTA_G          20.0f    // 重量变化 ≥ 20 g
#define TP_CLIMATE_DELTA_TEMP_C    0.5f     // 温度变化 ≥ 0.5 °C
#define TP_CLIMATE_DELTA_HUMID_PCT 2.0f     // 湿度变化 ≥ 2 %

// 每轮 `telemetry_task()` 最多发出的报文数（保证不阻塞主循环）
#define TP_MAX_EMIT_PER_TASK       1u

// =====================================================
// 2. 在线状态（$state 词表）
//
// `lost` = broker LWT 判定 ⇒ **设备侧不可自报告**（F-WP4-FW-7 / WP1 H-W1-4）
// ⇒ `tp_online_should_emit()` 永不因 `lost` 触发。
// =====================================================

enum TpOnlineState
{
    TP_ONLINE_INIT         = 0,
    TP_ONLINE_READY        = 1,
    TP_ONLINE_DISCONNECTED = 2,
    TP_ONLINE_LOST         = 3
};

// =====================================================
// 3. 纯决策函数（无副作用 · 可穷举验证）
// =====================================================

// 变化阈值判定（含边界：d == thr 视为**已超过**）
static inline bool tp_delta_exceeded(float now_v, float last_v, float thr)
{
    const float d = (now_v >= last_v) ? (now_v - last_v) : (last_v - now_v);
    return d >= thr;
}

// 抽样兜底到期判定（无符号回绕安全）
static inline bool tp_cadence_due(uint32_t now_ms, uint32_t last_ms, uint32_t period_ms)
{
    if (period_ms == 0u)
    {
        return false;
    }
    return (uint32_t)(now_ms - last_ms) >= period_ms;
}

// ★★ [P4-T1 · 2026-11-02] 取值通道「该不该发」的**唯一**决策点（纯函数）。
//
//   = 变化阈值已越过  ∨  抽样兜底到期
//
// ★ `period_ms == 0` ⇒ **没有**时间兜底 ⇒ 只在数值变化时发。
//   这条语义是 T1 修复的核心：周期兜底会把**旧读数**包装成一条新遥测
//   （ts/seq 都是新的）⇒ 云端逐条落库 ⇒ 图表上凭空多出同值点。
//   没有真实新采样的"心跳"不应伪装成采样点。
static inline bool tp_should_emit(float now_v, float last_v, float thr,
                                  uint32_t now_ms, uint32_t last_ms, uint32_t period_ms)
{
    if (tp_delta_exceeded(now_v, last_v, thr))
    {
        return true;
    }
    return tp_cadence_due(now_ms, last_ms, period_ms);
}

// 边沿去重：同一事件在 min_gap 内只接受一次。
// last_ms == 0（从未触发过）⇒ 首次必接受。
static inline bool tp_edge_accept(uint32_t now_ms, uint32_t last_ms, uint32_t min_gap_ms)
{
    if (last_ms == 0u)
    {
        return true;
    }
    return (uint32_t)(now_ms - last_ms) >= min_gap_ms;
}

// $state 迁移：仅 init / ready / disconnected 需要上报；lost 不可自报告。
static inline bool tp_online_should_emit(int prev_state, int cur_state)
{
    if (prev_state == cur_state)
    {
        return false;
    }

    if (cur_state == (int)TP_ONLINE_LOST)
    {
        return false;   // broker LWT 才判定为 lost
    }

    return (cur_state == (int)TP_ONLINE_INIT ||
            cur_state == (int)TP_ONLINE_READY ||
            cur_state == (int)TP_ONLINE_DISCONNECTED);
}

// `$state` 词表（草案；与 P2 Homie 投影的 $state 取值保持一致）
static inline const char *tp_online_state_name(int state)
{
    switch (state)
    {
        case (int)TP_ONLINE_INIT:         return "init";
        case (int)TP_ONLINE_READY:        return "ready";
        case (int)TP_ONLINE_DISCONNECTED: return "disconnected";
        case (int)TP_ONLINE_LOST:         return "lost";
        default:                          return "unknown";
    }
}

// 健康异常判定（仅异常上报；WP1:§3.9）
//   `value >= threshold` ⇒ 异常（如 free heap 低于阈值 ⇒ 用 floor 语义调用方翻转）
static inline bool tp_health_abnormal(long value, long threshold)
{
    return value >= threshold;
}

#endif // TELEMETRY_POLICY_H
