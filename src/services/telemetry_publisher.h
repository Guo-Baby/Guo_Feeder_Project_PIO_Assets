// =====================================================
// WP4-FW-TELEMETRY —— Telemetry Publisher（glue）
//
// 职责（**只做这些**）：
//   1. 只读 System State
//   2. 用 telemetry_policy.h 的纯函数做「该不该发」决策
//   3. 用 telemetry_envelope.h 组 JSON 信封
//   4. 交 CloudManager transport（cloud_send_telemetry）
//
// 禁止：
//   · 直接调用 MQTT（一律经 CloudManager）
//   · 复用 command uplink（不经 cloud_compress_uplink，不包成 {"cmd":"result"}）
//   · 阻塞（无 delay / while；每轮最多发 TP_MAX_EMIT_PER_TASK 条）
//
// 依据：docs/review/P3-WP4-Firmware-Integration-Design-Review.md §3.2/§3.3
//       docs/architecture/P3-WP1-Telemetry-Policy-Final.md
//
// ✅ Envelope v1 = **已冻结**（`H-WP4-FW-05` / `07` = `CLOSED`）
//    权威：docs/review/P3-WP4-Firmware-Telemetry-Decisions.md
//      · 信封 5 字段 {ts,type,value,seq,schema_version}（无 device_id / source）
//      · value 三态编码（number | string | boolean；**禁**数字编码 bool）
//      · seq = boot lifetime 计数（不持久化）
// ⚠️ 仍 `OPEN`：`H-WP4-FW-06`（`mqtt.json.telemetry` 字段集与默认值，本轮未使用配置）
// =====================================================

#ifndef TELEMETRY_PUBLISHER_H
#define TELEMETRY_PUBLISHER_H

#include <Arduino.h>

// =====================================================
// 编译期门禁（与 GF_HOMIE_ENABLED 同一模式）
//
//   0（默认）= 编译为空实现 ⇒ **零 Flash / 零 RAM 代价**，主循环行为不变
//   1        = 启用（`-DGF_TELEMETRY_ENABLED=1`）
//
// ⚠️ 默认 0 的原因见 `H-WP4-FW-08`（`CLOSED WITH GUARD`）：
//    P3 的 Flash 预算仅剩 636 B，而 telemetry 实测增量远超该值 ⇒
//    **启用须先经人工重裁 ceiling**（详见 WP4-FW 实施报告）。
// =====================================================

#ifndef GF_TELEMETRY_ENABLED
#define GF_TELEMETRY_ENABLED 0
#endif

// 诊断计数
struct TelemetryStats
{
    uint32_t emitted;      // 成功交给 transport 的报文数
    uint32_t transport_fail;
    uint32_t build_fail;
    uint32_t skipped_offline;
};

// 初始化（幂等；只置就绪标志与基线，不阻塞）
void telemetry_publisher_init();

// 主循环任务（非阻塞；每轮最多一条）
void telemetry_task();

// 诊断读取（只读快照）
TelemetryStats telemetry_get_stats();

// 当前是否启用（编译期门禁的运行时视图）
bool telemetry_is_enabled();

#endif // TELEMETRY_PUBLISHER_H
