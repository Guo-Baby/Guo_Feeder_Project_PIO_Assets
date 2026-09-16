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

// =====================================================
// P1.4 常量（Cloud Log Topic）
// =====================================================

// 云待发队列槽数（128 × 128 B = 16 KB，PSRAM 优先 / DRAM 回退）
//
// 为什么需要这条独立队列：
//   · INFO 是 Cloud YES / Flash NO —— 不落盘，无法从 Flash 补发
//   · WARN+ 是 Cloud YES / Flash YES —— 离线时靠 Flash 保命，
//     重启后可从段环回填（见 log_cloud_seed_from_flash()）
// 队列满 → FIFO 淘汰最旧并累计 drop_overflow（§27）
#define LOG_CLOUD_QUEUE_SLOTS     128u

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

    // ---- Commit 5：RAM→Flash 安全交接（§22 / §23 / §39）----
    uint32_t flash_retry;           // 因落盘失败而"保留记录 + 下一轮重试"的轮数
    uint32_t flash_blocked_rounds;  // 队首记录无法被接收（Flash 未就绪）的轮数
    uint32_t flush_honored;         // 被真正兑现的 CRITICAL flush 请求数

    // ---- P1.4 Cloud Log Topic ----
    uint32_t cloud_batch_sent;      // 批次发送次数（含重发）
    uint32_t cloud_records_sent;    // 累计首次发出的 record 条数
    uint32_t cloud_retx;            // ACK 超时后的重发次数
    uint32_t cloud_ack_ok;          // 成功确认的批次数
    uint32_t cloud_ack_timeout;     // ACK 超时次数
    uint32_t cloud_ack_lost;        // 重试耗尽（放弃推进）次数
    uint32_t cloud_q_drop;          // 云队列溢出淘汰条数（→ drop_overflow）
    uint32_t cloud_flash_drop;      // Flash 段淘汰中"未被 ACK"条数（→ drop_unacked）
    uint32_t self_degraded;         // LogManager 自降级次数（§29 / §39）
    uint8_t  cloud_q_used;          // 当前云队列占用（≤ LOG_CLOUD_QUEUE_SLOTS）
};

// =====================================================
// 生命周期
// =====================================================

// 分配 RAM 环（PSRAM 优先，失败回退 DRAM）。幂等，可重复调用。
bool log_init();

// 在 loop() 中调用：消费 RAM 环 → routing 决策 → 一个 Flash append 单元。
//
// 不阻塞、无 delay / while 等待、不创建 FreeRTOS Task（§29）。
// **安全交接（§22/§23）**：只有 Flash 持久化成功，对应 RAM record 才
// 被认为已消费；失败则保留在环内、下一轮重试，绝不静默丢弃 WARN+。
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

// ---- Commit 4：损坏注入（仅上板自测）----

// 破坏某 segment 头部（翻转 seg_index → CRC 失败），用于 §18
bool log_seg_corrupt_head(uint32_t seg);

// 破坏某 segment 的第 rec 条 record（翻转首字节 → CRC 失败），用于 §19
bool log_seg_corrupt_record(uint32_t seg, uint8_t rec);

// 把某 segment 截断到 bytes 字节（模拟部分写入）并立即重扫描，用于 §19
bool log_seg_truncate(uint32_t seg, uint32_t bytes);

// ---- Commit 5：落盘失败注入（仅上板自测）----

// 令接下来 count 次 Flash append **整批失败**（不写入任何 record），
// 用于验证 §22/§23 的 RAM→Flash 安全交接与 F10 重试路径。
void log_flash_test_fail_next(uint8_t count);

// =====================================================
// P1.4 / P1.5 观测与测试钩子（仅上板自测）
// =====================================================

struct LogCloudInfo
{
    uint8_t  queue_ready;       // 云队列是否分配成功（0/1）
    uint8_t  queue_in_psram;    // 是否落在 PSRAM（0/1）
    uint32_t queue_bytes;       // 队列字节数
    uint8_t  queue_used;        // 当前占用条数
    uint32_t acked_seq;         // 已确认 seq 高水位（RAM；重启归 0 ⇒ 重放）
    uint8_t  inflight;          // 是否有在途未确认批次（0/1）
    uint32_t tx_boot_seq;       // 在途批次的 boot_seq
    uint32_t tx_from;           // 在途批次 seq_from
    uint32_t tx_to;             // 在途批次 seq_to
    uint8_t  tx_count;          // 在途批次条数
    uint8_t  retry;             // 当前重试计数（0..LOG_ACK_MAX_RETRY）
    uint8_t  gave_up;           // 重试耗尽（GIVE_UP_NOT_ADVANCE）标志
    uint8_t  connected;         // CloudManager 报告的 MQTT 在线状态
    uint32_t last_batch_bytes;  // 最近一次 CBOR 批次字节数
};

void log_cloud_get_info(LogCloudInfo &out);

// 直接向云队列注入一条记录（仅上板自测：不经过 RAM 环，
// 便于在没有 MQTT 的条件下验证队列/CBOR/ACK 逻辑）
bool log_cloud_test_push(LogEventId event_id, LogLevel level);

// 复位云侧状态（acked 高水位 / 在途批次 / 重试计数 / gave_up），
// 并清空云队列 —— 测试基线复位用
void log_cloud_test_reset();

#endif // LOG_MANAGER_H
