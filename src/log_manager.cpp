// =====================================================
// LogManager —— P1.2：Log Core / RAM Queue（实现）
//
// 设计依据（冻结，不得重新设计）：
//   log模块历史/LogManager-P1契约冻结0915.md
//   log模块历史/LogManager详细设计规划0915.md
//   src/log_events.h（P1.1 契约头）
//
// 并发模型：
//   · 生产者：任意 Task 上下文（loopTask / esp-mqtt / BLE callback ...）
//   · 消费者：仅 loopTask（本文件不创建任何 Task）
//   · 环索引 + 槽拷贝在临界区内完成（拷贝 128 B，量级 ~1 us）
//   · **ISR 上下文不支持**（需要时另加 portENTER_CRITICAL_ISR 版本）
//
// 副作用：
//   · P1.2 不写 Flash、不发 MQTT，只在 log_task() 内做 routing **决策与计数**
// =====================================================

#include "log_manager.h"

#include <Arduino.h>
#include <string.h>
#include <esp_heap_caps.h>

#include "time_manager.h"

// =====================================================
// 内部状态
// =====================================================

static LogRecord *s_ring = nullptr;   // 64 槽 × 128 B（PSRAM 优先）
static uint32_t   s_wr = 0;           // 写计数（单调递增）
static uint32_t   s_rd = 0;           // 读计数（单调递增）
static bool       s_ready = false;
static bool       s_in_psram = false;
static uint32_t   s_ring_bytes = 0;

// seq / boot_seq
//
// ⚠️ P1.2 为 **RAM 态** 占位实现：
//    - boot_seq 固定为 1（P1.3 改为 log_init() 时从 /log/meta.bin 读取并 +1 落盘）
//    - seq 由 0 单调递增（P1.3 改为 Boot 预留区间 seq_base..seq_base+255，
//      允许空洞、不允许重复）
//    本阶段只保证"本次开机内单调递增且不重复"。
static uint32_t s_boot_seq = 1;
static uint32_t s_seq = 0;

static bool s_flush_requested = false;

static LogStats s_stats;

// 临界区：保护 s_wr / s_rd / s_ring 槽内容 / s_seq / s_stats
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

// =====================================================
// CRC32（与项目既有实现一致：init 0xFFFFFFFF，
//        poly 0xEDB88320 反射，final xor 0xFFFFFFFF）
// =====================================================

static uint32_t log_crc32(const uint8_t *data, uint32_t len)
{
    uint32_t crc = 0xFFFFFFFFu;

    for (uint32_t i = 0; i < len; i++)
    {
        crc ^= (uint32_t)data[i];

        for (uint8_t bit = 0; bit < 8; bit++)
        {
            crc = (crc >> 1) ^ ((crc & 1u) ? 0xEDB88320u : 0u);
        }
    }

    return ~crc;
}

// =====================================================
// log_init
// =====================================================

bool log_init()
{
    if (s_ready)
    {
        return true;
    }

    const uint32_t bytes = (uint32_t)LOG_RAM_QUEUE_SLOTS * (uint32_t)LOG_RECORD_SIZE;

    // 项目铁律：PSRAM 优先，失败回退内部 DRAM
    void *mem = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
    if (mem != nullptr)
    {
        s_in_psram = true;
    }
    else
    {
        mem = heap_caps_malloc(bytes, MALLOC_CAP_8BIT);
        s_in_psram = false;
    }

    if (mem == nullptr)
    {
        Serial.printf("[Log] RAM ring alloc failed (%u B)\n", (unsigned)bytes);
        s_ready = false;
        return false;
    }

    s_ring = (LogRecord *)mem;
    memset(s_ring, 0, bytes);

    s_wr = 0;
    s_rd = 0;
    s_seq = 0;
    s_boot_seq = 1;          // P1.3：改为 meta.bin 读取 + 1

    // 与运行期保持同一保护规则：stats / flush 状态一律经 s_mux 访问
    // （init 期虽无并发，但统一规则可避免将来出现"漏锁"的访问点）
    portENTER_CRITICAL(&s_mux);
    memset(&s_stats, 0, sizeof(s_stats));
    s_flush_requested = false;
    portEXIT_CRITICAL(&s_mux);

    s_ring_bytes = bytes;
    s_ready = true;

    Serial.printf(
        "[Log] RAM ring ready: %u slots x %u B = %u B in %s (boot_seq=%u)\n",
        (unsigned)LOG_RAM_QUEUE_SLOTS,
        (unsigned)LOG_RECORD_SIZE,
        (unsigned)bytes,
        s_in_psram ? "PSRAM" : "DRAM",
        (unsigned)s_boot_seq
    );

    return true;
}

// =====================================================
// log_emit
// =====================================================

bool log_emit(LogEventId event_id, LogLevel level,
              const LogParamIn *params, uint8_t param_count)
{
    if (!s_ready)
    {
        return false;
    }

    // DEBUG 策略：不进入 Log 系统（不占 RAM 环、不落盘、不上云）
    // 只累加侧信道计数，便于上板验证"DEBUG 未产生任何持久化负担"。
    if (level == LOG_LVL_DEBUG)
    {
#if LOG_DEBUG_ENABLE
        portENTER_CRITICAL(&s_mux);
        s_stats.debug_dropped++;
        portEXIT_CRITICAL(&s_mux);
#endif
        return false;
    }

    // 契约：参数数量超限整体拒绝（不截断）
    if (param_count > LOG_MAX_PARAMS)
    {
        return false;
    }

    if (param_count > 0 && params == nullptr)
    {
        return false;
    }

    LogRecord rec;
    memset(&rec, 0, LOG_RECORD_SIZE);

    rec.version     = (uint8_t)LOG_RECORD_VERSION;
    rec.level       = (uint8_t)level;
    rec.param_count = param_count;
    rec.event_id    = (uint16_t)event_id;
    rec.packed      = 0;                 // context_kind = 0（P1.2 未实现 Context）
    rec.uptime_ms   = (uint32_t)millis();
    rec.timestamp   = 0;
    rec.blob_len    = 0;
    rec.reserved16  = 0;

    // timestamp：仅在 TimeManager 已同步时写入并置 valid 位
    {
        time_t t = time_get();

        if (t > 0)
        {
            rec.timestamp = (uint32_t)t;
            rec.flags |= LOG_FLAG_TIMESTAMP_VALID;
        }
    }

    // 参数编码：每项 6 B { id:u8, type:u8, value:u32le }
    for (uint8_t i = 0; i < param_count; i++)
    {
        const LogParamIn &p = params[i];
        uint8_t *dst = &rec.params[(uint32_t)i * LOG_PARAM_SIZE];

        uint32_t raw = p.v.u;

        // BOOL 只写了 union 的 1 字节，其余字节不可信 -> 显式归一化
        if (p.type == LOG_PTYPE_BOOL)
        {
            raw = p.v.b ? 1u : 0u;
        }

        dst[0] = p.id;
        dst[1] = p.type;
        dst[2] = (uint8_t)(raw & 0xFFu);
        dst[3] = (uint8_t)((raw >> 8) & 0xFFu);
        dst[4] = (uint8_t)((raw >> 16) & 0xFFu);
        dst[5] = (uint8_t)((raw >> 24) & 0xFFu);
    }

    portENTER_CRITICAL(&s_mux);

    // seq / boot_seq 在临界区内推进，保证唯一且单调
    s_seq++;
    rec.seq = s_seq;
    rec.boot_seq = s_boot_seq;

    // CRC32 覆盖 [0..LOG_OFF_CRC32)
    rec.crc32 = log_crc32((const uint8_t *)&rec, LOG_OFF_CRC32);

    // 环形写入：满则淘汰最旧（side-channel 计数 ring_drop）
    uint32_t used = s_wr - s_rd;

    if (used >= LOG_RAM_QUEUE_SLOTS)
    {
        s_rd++;
        s_stats.ring_drop++;
        used--;
    }

    const uint32_t slot = s_wr % LOG_RAM_QUEUE_SLOTS;
    memcpy(&s_ring[slot], &rec, LOG_RECORD_SIZE);
    s_wr++;

    const uint32_t now_used = s_wr - s_rd;

    s_stats.emit_total++;
    s_stats.last_seq = s_seq;
    s_stats.ring_used = (uint8_t)now_used;

    if (now_used > (uint32_t)s_stats.ring_high_water)
    {
        s_stats.ring_high_water = (uint8_t)now_used;
    }

    portEXIT_CRITICAL(&s_mux);

    return true;
}

// =====================================================
// log_task（仅在 loop() 内调用）
// =====================================================

void log_task()
{
    if (!s_ready)
    {
        return;
    }

    uint8_t budget = LOG_DRAIN_MAX_PER_TASK;

    while (budget > 0)
    {
        LogRecord rec;
        bool got = false;

        portENTER_CRITICAL(&s_mux);

        if (s_rd != s_wr)
        {
            const uint32_t slot = s_rd % LOG_RAM_QUEUE_SLOTS;
            memcpy(&rec, &s_ring[slot], LOG_RECORD_SIZE);
            s_rd++;
            got = true;
        }

        portEXIT_CRITICAL(&s_mux);

        if (!got)
        {
            break;
        }

        budget--;

        // ---- routing 决策（纯计算，不涉及任何 I/O）----
        const LogLevel lv = (LogLevel)rec.level;
        const bool to_flash = log_level_to_flash(lv);
        const bool to_cloud = log_level_to_cloud(lv);
        const bool is_critical = (lv == LOG_LVL_CRITICAL);

        // ---- 短临界区 ①：统一更新 stats 与 flush 请求 ----
        //
        // 并发规则（P1.2 fix）：所有 s_stats / s_flush_requested 的读与改
        // 一律在 s_mux 内完成，与 log_emit() / log_get_stats() /
        // log_stats_reset() / log_flush_requested() 使用同一把锁。
        portENTER_CRITICAL(&s_mux);

        if (to_flash)
        {
            s_stats.flash_routed++;
        }

        if (to_cloud)
        {
            s_stats.cloud_routed++;
        }

        s_stats.consumed++;

        if (is_critical)
        {
            s_stats.critical_seen++;

            // 冻结语义（P1 修订）：CRITICAL = Flash 立即 + 进入
            // "最高优先级下一批次"，**不建独立通道、不承诺秒级**。
            // 边沿触发：只置一次，避免重复 flush。
            if (!s_flush_requested)
            {
                s_flush_requested = true;
                s_stats.flush_requests++;
            }
        }

        portEXIT_CRITICAL(&s_mux);

        // ---- I/O 区（P1.2 为空，P1.3/P1.4 在此落地）----
        //
        // P1.3：Flash Segment append
        // P1.4：Cloud Log Queue 投递
        // ★ 严格要求：耗时 I/O 必须在临界区【外】执行 ——
        //   禁止把 Flash 写入或 MQTT publish 放进上面的 s_mux 段。
    }

    portENTER_CRITICAL(&s_mux);
    s_stats.ring_used = (uint8_t)(s_wr - s_rd);
    portEXIT_CRITICAL(&s_mux);
}

// =====================================================
// 观测
// =====================================================

void log_get_stats(LogStats &out)
{
    portENTER_CRITICAL(&s_mux);

    out = s_stats;
    out.ring_used = (uint8_t)(s_wr - s_rd);
    out.ring_ready = s_ready ? 1u : 0u;
    out.ring_in_psram = s_in_psram ? 1u : 0u;
    out.ring_bytes = s_ring_bytes;
    out.boot_seq = s_boot_seq;

    portEXIT_CRITICAL(&s_mux);
}

void log_stats_reset()
{
    portENTER_CRITICAL(&s_mux);
    memset(&s_stats, 0, sizeof(s_stats));
    s_stats.ring_used = (uint8_t)(s_wr - s_rd);
    portEXIT_CRITICAL(&s_mux);
}

bool log_flush_requested()
{
    // 与 log_task() / log_emit() 统一使用 s_mux（非阻塞自旋锁，不用 mutex）
    portENTER_CRITICAL(&s_mux);
    const bool requested = s_flush_requested;
    portEXIT_CRITICAL(&s_mux);

    return requested;
}

void log_clear_flush_request()
{
    portENTER_CRITICAL(&s_mux);
    s_flush_requested = false;
    portEXIT_CRITICAL(&s_mux);
}
