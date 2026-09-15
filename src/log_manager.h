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
// P1.3 常量（Flash Segment Ring）
//
// 尺寸常量在 log_events.h 已冻结，此处**不重复定义**，只补路径与段头约定：
//   LOG_SEGMENT_HEADER_SIZE / LOG_RECORDS_PER_SEGMENT / LOG_SEGMENT_SIZE
//   LOG_SEGMENT_COUNT / LOG_SEGMENT_CAPACITY
// =====================================================

#define LOG_DIR_PATH        "/log"
#define LOG_META_PATH       "/log/meta.bin"
#define LOG_SEG_PATH_FMT    "/log/s%07u.log"   // s0000000.log .. s0000015.log

// 段头 magic（uint32，"SEGS"）
#define LOG_SEG_MAGIC       0x53454753u

// 段头布局（固定 16 B，不得增删字段）
//   0   magic      uint32
//   4   seg_index  uint32
//   8   first_seq  uint32
//   12  crc32      uint32（覆盖 [0..11]）
#define LOG_SEG_OFF_MAGIC      0u
#define LOG_SEG_OFF_INDEX      4u
#define LOG_SEG_OFF_FIRST_SEQ  8u
#define LOG_SEG_OFF_CRC32      12u
#define LOG_SEG_CRC_SPAN       12u   // 头 CRC 覆盖字节数

// meta.bin 布局（固定 32 B，不得增删字段；§12）
//   0   magic         uint32  0x474C4F47 ("GLOG")
//   4   fmt_version   uint32  = 2
//   8   boot_seq      uint32  每次 log_init() +1
//   12  seq_reserved  uint32  已预留出去的 seq 高水位
//   16  corrupt_count uint32  损坏段 / meta 丢失累计
//   20  reserved      8 B
//   28  crc32         uint32（覆盖 [0..27]）
//
// 注：log_events.h 中同一区域写作 "fmt_version(1) + reserved(1) +
//     reserved16(2)"，产生的字节序列与 uint32 fmt_version 完全一致
//     （均为 02 00 00 00），故两者不冲突。
#define LOG_META_OFF_MAGIC          0u
#define LOG_META_OFF_FMT            4u
#define LOG_META_OFF_BOOT_SEQ       8u
#define LOG_META_OFF_SEQ_RESERVED   12u
#define LOG_META_OFF_CORRUPT        16u
#define LOG_META_OFF_CRC32          28u
#define LOG_META_CRC_SPAN           28u

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

    // ---- P1.3 Flash Segment Ring（有界计数，无动态字符串）----
    uint32_t flash_append_ok;       // 成功写入 Flash 的 record 条数
    uint32_t flash_append_fail;     // 写入失败的 record 条数
    uint32_t flash_segment_created; // 新建 segment 次数
    uint32_t flash_segment_deleted; // 删除 segment 次数（ring 淘汰 / 损坏重建）
    uint32_t flash_crc_error;       // 扫描时发现的 record CRC 错误数
    uint32_t flash_corrupt_segment; // Header 损坏被废弃的 segment 数
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

// =====================================================
// P1.3 Flash 观测 / 测试钩子（仅上板自测，不属于正式 API）
//
// 与 workflow_storage_test_* 同一惯例：只由串口控制台调用。
// P1.4 / P1.5 若确需正式 API，再单独提升。
// =====================================================

struct LogFlashInfo
{
    uint8_t  flash_ready;       // /log 初始化是否成功（0/1）
    uint8_t  valid_segments;    // 当前有效 segment 数
    uint32_t oldest_segment;    // 最老 segment 索引
    uint32_t newest_segment;    // 最新 segment 索引
    uint32_t append_segment;    // 当前追加目标 segment
    uint8_t  append_index;      // 该 segment 内下一条 record 槽位（0..31）
    uint16_t total_records;     // 全部有效 record 条数
    uint32_t first_seq_oldest;  // 最老 segment 的 first_seq
    uint32_t first_seq_newest;  // 最新 segment 的 first_seq
    uint32_t batch_bytes;       // Flash 批量工作缓冲字节数
    uint8_t  batch_in_psram;    // 该缓冲是否落在 PSRAM（0/1）

    // ---- meta.bin / sequence（§12 / §13 / §15 / §16）----
    uint8_t  seq_reliable;      // 0 = sequence 可能重复（meta 写入失败，§15）
    uint32_t boot_seq;          // 本次开机序号（每次 log_init() +1）
    uint32_t seq_reserved;      // meta.seq_reserved 高水位
    uint32_t corrupt_count;     // meta.corrupt_count
    uint32_t seq_base;          // 本区间首号
    uint32_t seq_limit;         // 本区间末号（seq_base + LOG_SEQ_RESERVE - 1）
    uint32_t seq_last;          // 最近一次分配出去的 seq
};

void log_flash_get_info(LogFlashInfo &out);

// 读某 segment 头部（ok=0 表示不存在 / magic 或 CRC 非法）
struct LogSegmentHead
{
    uint8_t  ok;
    uint8_t  records;      // 该段已验证的有效 record 数
    uint32_t magic;
    uint32_t index;
    uint32_t first_seq;
    uint32_t crc32;
};

bool log_flash_peek_segment(uint32_t seg, LogSegmentHead &out);

// 读某 segment 的第 rec 条 record（crc_ok=0 表示内容存在但校验失败）
bool log_flash_peek_record(uint32_t seg, uint8_t rec, LogRecord &out, uint8_t &crc_ok);

// 删除 /log 下全部文件（测试基线复位）
bool log_flash_wipe();

// 删除 /log/meta.bin（测试 §16 的"meta 丢失 → 扫描重建"路径）
bool log_meta_wipe();

// 破坏 /log/meta.bin 的 CRC（测试 §16 的"meta 损坏 → 扫描重建"路径）
bool log_meta_corrupt();

// 一次性故障注入：令下一次 meta 写入失败（测试 §15 / §41-4 的
// "reservation 写失败 → s_seq_reliable = false"）
void log_meta_test_fail_next(bool enable);

#endif // LOG_MANAGER_H
