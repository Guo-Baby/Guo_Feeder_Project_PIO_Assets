// =====================================================
// LogManager —— P1.2：Log Core / RAM Queue
//
// 冻结契约见 log_events.h（P1.1，不得重新设计）。
//
// 本阶段（P1.2）实现：
//   log_emit()  → RAM Ring → log_task()（在 loop() 中消费）
//   Level → (Flash, Cloud) routing **决策与计数**
//
// 本阶段**不**实现（后续阶段）：
//   P1.3 Flash Segment Ring（真正落盘）+ meta.bin / seq reserve
//   P1.4 Cloud Log Topic（真正上传 MQTT）
//   P1.5 ACK / Retry / Offline
//
// 冻结铁律：
//   · 不创建独立 FreeRTOS Task（LittleFS 为单写者模型）
//   · 不要求调用方构造动态 String 作为正式 Log
//   · 不允许递归 Logging（自身故障只走侧信道计数）
//   · DEBUG 不入 Log 系统（不占 RAM 环、不落盘、不上云）
//   · Record 不含 uploaded / persist（零原地修改）
// =====================================================

#ifndef LOG_MANAGER_H
#define LOG_MANAGER_H

#include <stdint.h>
#include "log_events.h"

// =====================================================
// P1.2 常量
// =====================================================

// RAM 环槽数（冻结设计：64 槽 × 128 B = 8 KB，PSRAM 优先）
#define LOG_RAM_QUEUE_SLOTS       64u

// log_task() 每轮最多消费条数（对齐 LOG_FLUSH_RECORDS，避免长时间占用 loop）
#define LOG_DRAIN_MAX_PER_TASK    8u

// DEBUG 编译期总开关：0 时 log_emit() 对 DEBUG 立即返回
#define LOG_DEBUG_ENABLE          1

// =====================================================
// 统计（侧信道：LogManager 自身状态，不产生 Log，避免递归）
// =====================================================

struct LogStats
{
    uint32_t emit_total;      // 成功进入 RAM 环的记录数
    uint32_t debug_dropped;   // DEBUG 被策略丢弃数（不入环）
    uint32_t ring_drop;       // RAM 环满 → 淘汰最旧记录数
    uint32_t consumed;        // log_task() 已消费条数
    uint32_t flash_routed;    // routing 决策 = 需落 Flash 的条数
    uint32_t cloud_routed;    // routing 决策 = 需上云的条数
    uint32_t critical_seen;   // CRITICAL 记录数
    uint32_t flush_requests;  // CRITICAL 触发的立即 flush 请求数
    uint8_t  ring_used;       // 当前占用槽数
    uint8_t  ring_high_water; // 历史最高占用（自上次 reset）
    uint32_t boot_seq;        // 本次开机序号（P1.3 改为 meta.bin 持久化）
    uint32_t last_seq;        // 最近一条记录的 seq
    uint8_t  ring_ready;      // log_init() 是否成功（0/1）
    uint8_t  ring_in_psram;   // 环缓冲是否落在 PSRAM（0/1）
    uint32_t ring_bytes;      // 环缓冲字节数
};

// =====================================================
// 生命周期
// =====================================================

// 分配 RAM 环（PSRAM 优先，失败回退 DRAM）。幂等，可重复调用。
bool log_init();

// 在 loop() 中调用：消费 RAM 环并做 routing 决策。
// 不阻塞、不写 Flash、不发 MQTT（P1.2 范围）。
void log_task();

// =====================================================
// 正式 API（结构化：EventId + Level + Typed Params）
//
// 返回 true = 已进入 RAM 环；false = 未进入
//   （DEBUG 被策略丢弃 / 环未就绪 / 参数超限被拒以外的情况）
// param_count > LOG_MAX_PARAMS 时按契约整体拒绝（不截断）。
// =====================================================

bool log_emit(LogEventId event_id, LogLevel level,
              const LogParamIn *params, uint8_t param_count);

inline bool log_emit0(LogEventId event_id, LogLevel level)
{
    return log_emit(event_id, level, nullptr, 0);
}

// =====================================================
// 参数构造 helper（避免调用方误用 union 成员）
// =====================================================

inline LogParamIn log_arg_i32(uint8_t pid, int32_t v)
{
    LogParamIn p;
    p.id = pid;
    p.type = LOG_PTYPE_I32;
    p.v.i = v;
    return p;
}

inline LogParamIn log_arg_u32(uint8_t pid, uint32_t v)
{
    LogParamIn p;
    p.id = pid;
    p.type = LOG_PTYPE_U32;
    p.v.u = v;
    return p;
}

inline LogParamIn log_arg_f32(uint8_t pid, float v)
{
    LogParamIn p;
    p.id = pid;
    p.type = LOG_PTYPE_F32;
    p.v.f = v;
    return p;
}

inline LogParamIn log_arg_bool(uint8_t pid, bool v)
{
    LogParamIn p;
    p.id = pid;
    p.type = LOG_PTYPE_BOOL;
    p.v.b = v ? (uint8_t)1 : (uint8_t)0;
    return p;
}

inline LogParamIn log_arg_enum(uint8_t pid, uint32_t v)
{
    LogParamIn p;
    p.id = pid;
    p.type = LOG_PTYPE_ENUM;
    p.v.u = v;
    return p;
}

// =====================================================
// 观测（上板测试 / 后续阶段使用）
// =====================================================

void log_get_stats(LogStats &out);

// 仅清零统计计数，不动 RAM 环内容
void log_stats_reset();

// CRITICAL 触发的"立即 flush + 提升 Cloud 优先级"请求（P1.3 / P1.4 消费）
bool log_flush_requested();
void log_clear_flush_request();

#endif // LOG_MANAGER_H
