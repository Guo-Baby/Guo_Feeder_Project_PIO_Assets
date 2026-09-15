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
#include <LittleFS.h>
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

// ---- P1.3 Flash Segment Ring 状态 ----
//
// 只由 loopTask 读写（log_init / log_task / 测试钩子都在 loop 上），
// 因此不需要 s_mux；但**不得**被其它任务直接访问（§35 单写者）。
static bool     s_flash_ready = false;

// 每 segment 元信息（16 个小整数 —— 属"小型控制状态"，留内部 RAM，§32）
static uint32_t s_seg_first_seq[LOG_SEGMENT_COUNT];   // 0 = 无有效首条
static uint8_t  s_seg_records[LOG_SEGMENT_COUNT];     // 已验证的有效 record 数
static uint8_t  s_seg_present[LOG_SEGMENT_COUNT];     // 文件存在且头合法

static uint32_t s_oldest_segment = 0;
static uint32_t s_newest_segment = 0;
static uint32_t s_append_segment = 0;
static uint8_t  s_append_index = 0;
static uint8_t  s_valid_segment_count = 0;
static uint16_t s_flash_total_records = 0;

// Flash 批量工作缓冲（LOG_FLUSH_RECORDS × 128 B = 1 KB）
//
// §31 / §33：批量工作区优先 PSRAM，失败回退 DRAM。
// §30：一次 log_task() 最多执行**一个** Flash append 单元（= 一个 batch）。
static LogRecord *s_flash_batch = nullptr;
static bool       s_flash_batch_in_psram = false;
static uint32_t   s_flash_batch_bytes = 0;

// ---- seq / boot_seq（§13 / §15 / §16）----
//
// Boot 时从 /log/meta.bin 预留一个区间（LOG_SEQ_RESERVE = 256 个号）：
//     seq_base = meta.seq_reserved + 1
//     meta.seq_reserved += 256        ← 立即落盘
// 本次开机使用 seq_base .. seq_base+255；用尽后再预留下一段。
// **允许空洞，不允许重复**（未用掉的号直接废弃，不回收）。
static uint32_t s_boot_seq = 1;           // 本次开机序号
static uint32_t s_meta_reserved = 0;      // meta.seq_reserved（已预留高水位）
static uint32_t s_meta_corrupt = 0;       // meta.corrupt_count
static uint32_t s_seq_base = 0;           // 本区间首号
static uint32_t s_seq_limit = 0;          // 本区间末号
static uint32_t s_seq_last = 0;           // 最近一次分配出去的 seq
static bool     s_seq_reliable = true;    // false = 可能重复（§15）
static bool     s_seq_reserving = false;  // 预留进行中（避免并发重复写 meta）

// 测试钩子：令下一次 meta_write 失败（一次性），用于验证 §15 / §41-4
static bool s_meta_fail_next = false;

// 本次扫描新增的损坏 segment 数（§18），由 seq_init() 累加进 meta.corrupt_count
static uint32_t s_scan_corrupt = 0;

static bool s_flush_requested = false;

static LogStats s_stats;

// 临界区：保护 s_wr / s_rd / s_ring 槽内容 / s_seq_last / s_stats / s_flush_requested
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
// P1.3 Flash Segment Ring（实现）
//
// 布局（§8 / §9）：
//   /log/meta.bin                32 B（Commit 3 接入）
//   /log/s0000000.log .. s0000015.log
//       16 B header + 31 × 128 B record = 3984 B
//
// 原则（§12 / §20 / §42）：
//   整段创建 / 整段追加 / 整段删除 —— 绝不修改已写入 Record 内的任何字节
//   （因此不存在 uploaded / persist 位，符合 LittleFS COW 特性）
//
// 并发（§35）：
//   全部由 loopTask 访问（log_init / log_task / 测试钩子），不额外加锁
// =====================================================

static void flash_seg_path(uint32_t seg, char *out, size_t out_size)
{
    snprintf(out, out_size, LOG_SEG_PATH_FMT, (unsigned)seg);
}

static void flash_put_u32(uint8_t *dst, uint32_t v)
{
    dst[0] = (uint8_t)(v & 0xFFu);
    dst[1] = (uint8_t)((v >> 8) & 0xFFu);
    dst[2] = (uint8_t)((v >> 16) & 0xFFu);
    dst[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static uint32_t flash_get_u32(const uint8_t *src)
{
    return (uint32_t)src[0]
         | ((uint32_t)src[1] << 8)
         | ((uint32_t)src[2] << 16)
         | ((uint32_t)src[3] << 24);
}

// 段头组装：magic / seg_index / first_seq / crc32(前 12 B)
static void flash_build_head(uint8_t *head, uint32_t seg, uint32_t first_seq)
{
    memset(head, 0, LOG_SEGMENT_HEADER_SIZE);
    flash_put_u32(&head[LOG_SEG_OFF_MAGIC], LOG_SEG_MAGIC);
    flash_put_u32(&head[LOG_SEG_OFF_INDEX], seg);
    flash_put_u32(&head[LOG_SEG_OFF_FIRST_SEQ], first_seq);
    flash_put_u32(&head[LOG_SEG_OFF_CRC32], log_crc32(head, LOG_SEG_CRC_SPAN));
}

// 段头校验（magic / seg_index / crc32）
static bool flash_check_head(const uint8_t *head, uint32_t expect_seg)
{
    if (flash_get_u32(&head[LOG_SEG_OFF_MAGIC]) != LOG_SEG_MAGIC)
    {
        return false;
    }

    if (flash_get_u32(&head[LOG_SEG_OFF_INDEX]) != expect_seg)
    {
        return false;
    }

    if (flash_get_u32(&head[LOG_SEG_OFF_CRC32]) != log_crc32(head, LOG_SEG_CRC_SPAN))
    {
        return false;
    }

    return true;
}

// Record 校验：crc32 覆盖 [0..LOG_OFF_CRC32)
static bool flash_check_record(const LogRecord &rec)
{
    return rec.crc32 == log_crc32((const uint8_t *)&rec, LOG_OFF_CRC32);
}

// 扫描段内 record（§19）：
//   遇到第一条 CRC 失败的 record 即停止 —— 其后的数据不能假定连续有效
//
// count_errors=false 用于"只读观测"（测试钩子 peek），避免反复扫描污染统计
static uint8_t flash_scan_records(File &f, bool count_errors)
{
    uint8_t count = 0;
    LogRecord rec;

    for (uint8_t i = 0; i < LOG_RECORDS_PER_SEGMENT; i++)
    {
        const uint32_t off =
            LOG_SEGMENT_HEADER_SIZE + (uint32_t)i * LOG_RECORD_SIZE;

        if (!f.seek(off))
        {
            break;
        }

        if (f.read((uint8_t *)&rec, LOG_RECORD_SIZE) != (size_t)LOG_RECORD_SIZE)
        {
            break;
        }

        if (!flash_check_record(rec))
        {
            // §19：该条及其后不再认为有效
            if (count_errors)
            {
                portENTER_CRITICAL(&s_mux);
                s_stats.flash_crc_error++;
                portEXIT_CRITICAL(&s_mux);
            }
            break;
        }

        count++;
    }

    return count;
}

// 把文件补零扩展到 target（修复被截断的 segment，§19）
static bool flash_extend_file(File &f, size_t target)
{
    const size_t sz = f.size();

    if (sz >= target)
    {
        return true;
    }

    if (!f.seek((uint32_t)sz))
    {
        return false;
    }

    static const uint8_t zeros[128] = { 0 };
    size_t remain = target - sz;

    while (remain > 0)
    {
        const size_t chunk = (remain >= sizeof(zeros)) ? sizeof(zeros) : remain;

        if (f.write(zeros, chunk) != chunk)
        {
            return false;
        }

        remain -= chunk;
    }

    return true;
}

// §18：Header 无效 / 尺寸异常 → 删除该 segment（下次作为空段复用）并计数
static void flash_drop_broken_segment(uint32_t seg, const char *path)
{
    LittleFS.remove(path);

    s_seg_present[seg] = 0;
    s_seg_records[seg] = 0;
    s_seg_first_seq[seg] = 0;

    s_scan_corrupt++;

    portENTER_CRITICAL(&s_mux);
    s_stats.flash_corrupt_segment++;
    portEXIT_CRITICAL(&s_mux);
}

// 依 first_seq 重算 oldest / newest
static void flash_recompute_extremes()
{
    uint32_t oldest = 0;
    uint32_t newest = 0;
    uint32_t oldest_seq = 0;
    uint32_t newest_seq = 0;
    bool have = false;

    for (uint32_t i = 0; i < LOG_SEGMENT_COUNT; i++)
    {
        if (!s_seg_present[i])
        {
            continue;
        }

        const uint32_t fs = s_seg_first_seq[i];

        if (!have || fs < oldest_seq)
        {
            oldest_seq = fs;
            oldest = i;
        }

        if (!have || fs > newest_seq)
        {
            newest_seq = fs;
            newest = i;
        }

        have = true;
    }

    s_oldest_segment = have ? oldest : 0;
    s_newest_segment = have ? newest : 0;
}

// §20：删除某个 segment（ring 淘汰 / 损坏重建）
static bool flash_drop_segment(uint32_t seg)
{
    char path[32];
    flash_seg_path(seg, path, sizeof(path));

    bool ok = true;

    if (LittleFS.exists(path))
    {
        ok = LittleFS.remove(path);
    }

    if (!ok)
    {
        return false;
    }

    if (s_seg_present[seg])
    {
        if (s_flash_total_records >= s_seg_records[seg])
        {
            s_flash_total_records =
                (uint16_t)(s_flash_total_records - s_seg_records[seg]);
        }

        s_seg_present[seg] = 0;
        s_seg_records[seg] = 0;
        s_seg_first_seq[seg] = 0;

        if (s_valid_segment_count > 0)
        {
            s_valid_segment_count--;
        }
    }

    portENTER_CRITICAL(&s_mux);
    s_stats.flash_segment_deleted++;
    portEXIT_CRITICAL(&s_mux);

    flash_recompute_extremes();
    return true;
}

// 全量扫描 16 个 segment（§17）
//
// ⚠️ Commit 2 范围：只做"识别与元信息建立"。
//    头/大小损坏时的"删除 + corrupt_count++"（§18）在 Commit 4 实现。
static void flash_scan_all()
{
    s_valid_segment_count = 0;
    s_flash_total_records = 0;
    s_oldest_segment = 0;
    s_newest_segment = 0;
    s_append_segment = 0;
    s_append_index = 0;

    uint32_t oldest_seq = 0;
    uint32_t newest_seq = 0;
    bool have_any = false;

    for (uint32_t i = 0; i < LOG_SEGMENT_COUNT; i++)
    {
        s_seg_present[i] = 0;
        s_seg_records[i] = 0;
        s_seg_first_seq[i] = 0;

        char path[32];
        flash_seg_path(i, path, sizeof(path));

        if (!LittleFS.exists(path))
        {
            continue;
        }

        // 以 "r+" 打开：截断修复（§19）需要写回
        File f = LittleFS.open(path, "r+");

        if (!f)
        {
            continue;
        }

        const size_t fsize = f.size();

        // §18：连 header 都不完整 → 损坏
        if (fsize < (size_t)LOG_SEGMENT_HEADER_SIZE)
        {
            f.close();
            flash_drop_broken_segment(i, path);
            continue;
        }

        // §18：尺寸异常膨胀 → 损坏
        if (fsize > (size_t)LOG_SEGMENT_SIZE)
        {
            f.close();
            flash_drop_broken_segment(i, path);
            continue;
        }

        uint8_t head[LOG_SEGMENT_HEADER_SIZE];

        if (f.read(head, sizeof(head)) != (size_t)sizeof(head))
        {
            f.close();
            flash_drop_broken_segment(i, path);
            continue;
        }

        // §17-3/4/5 magic / CRC / index 校验；§18：非法 → 删除 + 计数
        if (!flash_check_head(head, i))
        {
            f.close();
            flash_drop_broken_segment(i, path);
            continue;
        }

        const uint32_t first_seq = flash_get_u32(&head[LOG_SEG_OFF_FIRST_SEQ]);
        const uint8_t records = flash_scan_records(f, true);

        // §19：文件被截断（部分写入）→ 补零修复为定长；
        //       append 位置由 flash_scan_records 给出的"首条坏记录"决定
        if (fsize < (size_t)LOG_SEGMENT_SIZE)
        {
            flash_extend_file(f, LOG_SEGMENT_SIZE);
        }

        f.close();

        s_seg_present[i] = 1;
        s_seg_records[i] = records;
        s_seg_first_seq[i] = first_seq;
        s_valid_segment_count++;
        s_flash_total_records = (uint16_t)(s_flash_total_records + records);

        if (!have_any || first_seq < oldest_seq)
        {
            oldest_seq = first_seq;
            s_oldest_segment = i;
        }

        if (!have_any || first_seq > newest_seq)
        {
            newest_seq = first_seq;
            s_newest_segment = i;
        }

        have_any = true;
    }

    if (!have_any)
    {
        return;   // 空 ring：追加时从 segment 0 建起
    }

    // 追加目标：最新段的空闲槽
    s_append_segment = s_newest_segment;
    s_append_index = s_seg_records[s_newest_segment];
}

// 整段创建：预分配 LOG_SEGMENT_SIZE 字节并写入 16 B header
//
// §9：一个 segment 恒为 3984 B（16 B header + 31 × 128 B）。
// 之所以**预分配定长**而不是"按需增长"：
//   · 扫描时 size 校验恒定成立（否则部分填充的段会被 size 检查误判为损坏）
//   · 追加只是覆盖已有偏移，不触发文件扩展
// 未写入的 record 槽为全 0 —— 其 crc32 必然校验失败，因此天然表达"空槽"。
//
// 时间边界（§30）：仅在本段首次创建时发生，约 4 KB 写（几十 ms 量级），
// 属低频事件（每 31 条记录一次），因此可与本轮的 record 追加合为一个单元。
static bool flash_create_segment(uint32_t seg, uint32_t first_seq)
{
    char path[32];
    flash_seg_path(seg, path, sizeof(path));

    if (LittleFS.exists(path))
    {
        LittleFS.remove(path);
    }

    File f = LittleFS.open(path, "w");

    if (!f)
    {
        return false;
    }

    uint8_t head[LOG_SEGMENT_HEADER_SIZE];
    flash_build_head(head, seg, first_seq);

    bool ok = (f.write(head, sizeof(head)) == (size_t)sizeof(head));

    // 整段预分配：剩余空间补零，使文件恒为 LOG_SEGMENT_SIZE
    if (ok)
    {
        static const uint8_t zeros[128] = { 0 };
        uint32_t remain = LOG_SEGMENT_SIZE - LOG_SEGMENT_HEADER_SIZE;

        while (remain > 0)
        {
            const size_t chunk = (remain >= sizeof(zeros)) ? sizeof(zeros) : remain;

            if (f.write(zeros, chunk) != chunk)
            {
                ok = false;
                break;
            }

            remain -= (uint32_t)chunk;
        }
    }

    f.close();

    if (!ok)
    {
        return false;
    }

    s_seg_present[seg] = 1;
    s_seg_records[seg] = 0;
    s_seg_first_seq[seg] = first_seq;
    s_valid_segment_count++;
    s_append_segment = seg;
    s_append_index = 0;
    s_newest_segment = seg;   // 新段必为最新（first_seq 单调递增）

    portENTER_CRITICAL(&s_mux);
    s_stats.flash_segment_created++;
    portEXIT_CRITICAL(&s_mux);

    return true;
}

// 确保存在可写的追加目标；必要时新建 segment
//
// ⚠️ Commit 2 范围：16 段全满时**返回 false**（暂不可写）。
//    §20 的"删除最老段并复用"在 Commit 4 实现。
static bool flash_ensure_append_target(uint32_t next_seq)
{
    // ① 当前目标段可用（存在且未满）
    if (s_seg_present[s_append_segment] &&
        s_append_index < LOG_RECORDS_PER_SEGMENT)
    {
        return true;
    }

    // ② 选定目标段
    uint32_t target = LOG_SEGMENT_COUNT;

    if (s_valid_segment_count == 0)
    {
        target = 0;                                                  // 空 ring
    }
    else if (s_seg_records[s_newest_segment] < LOG_RECORDS_PER_SEGMENT)
    {
        target = s_newest_segment;                                   // 最新段有空位
    }
    else
    {
        // 从最新段之后按环形找一条未使用的段
        for (uint32_t k = 1; k <= LOG_SEGMENT_COUNT; k++)
        {
            const uint32_t idx = (s_newest_segment + k) % LOG_SEGMENT_COUNT;

            if (!s_seg_present[idx])
            {
                target = idx;
                break;
            }
        }

        if (target >= LOG_SEGMENT_COUNT)
        {
            // §20：段环已满 → 删除最老 segment 后复用该槽位
            const uint32_t victim = s_oldest_segment;

            if (!flash_drop_segment(victim))
            {
                return false;
            }

            target = victim;
        }
    }

    // ③ 目标段已有文件 → 直接指向其尾部
    if (s_seg_present[target])
    {
        s_append_segment = target;
        s_append_index = s_seg_records[target];
        return (s_append_index < LOG_RECORDS_PER_SEGMENT);
    }

    // ④ 新建段（first_seq = 即将写入的首条记录 seq）
    return flash_create_segment(target, next_seq);
}

// 追加一个 batch（§30：一次调用 = 一个 Flash append 单元）
//
// 返回成功写入的 record 条数（< count 表示中途失败）。
static uint8_t flash_append_batch(const LogRecord *recs, uint8_t count)
{
    uint8_t written = 0;

    for (uint8_t i = 0; i < count; i++)
    {
        if (!flash_ensure_append_target(recs[i].seq))
        {
            break;
        }

        char path[32];
        flash_seg_path(s_append_segment, path, sizeof(path));

        File f = LittleFS.open(path, "r+");

        if (!f)
        {
            break;
        }

        const uint32_t off = LOG_SEGMENT_HEADER_SIZE +
                             (uint32_t)s_append_index * LOG_RECORD_SIZE;

        bool ok = f.seek(off);

        if (ok)
        {
            ok = (f.write((const uint8_t *)&recs[i], LOG_RECORD_SIZE) ==
                  (size_t)LOG_RECORD_SIZE);
        }

        f.close();

        if (!ok)
        {
            break;
        }

        s_append_index++;
        s_seg_records[s_append_segment]++;
        s_flash_total_records++;
        written++;
    }

    return written;
}

// =====================================================
// P1.3 meta.bin / sequence reservation（§12 / §13 / §15 / §16）
// =====================================================

// 读 meta；false = 不存在 / 长度不符 / magic 或版本不符 / CRC 错
static bool meta_read(uint32_t &boot_seq, uint32_t &seq_reserved, uint32_t &corrupt)
{
    if (!LittleFS.exists(LOG_META_PATH))
    {
        return false;
    }

    File f = LittleFS.open(LOG_META_PATH, "r");

    if (!f)
    {
        return false;
    }

    uint8_t buf[LOG_META_SIZE];
    const bool got = (f.read(buf, sizeof(buf)) == (size_t)sizeof(buf));
    f.close();

    if (!got)
    {
        return false;
    }

    if (flash_get_u32(&buf[LOG_META_OFF_MAGIC]) != LOG_META_MAGIC)
    {
        return false;
    }

    if (flash_get_u32(&buf[LOG_META_OFF_FMT]) != LOG_META_VERSION)
    {
        return false;
    }

    if (flash_get_u32(&buf[LOG_META_OFF_CRC32]) != log_crc32(buf, LOG_META_CRC_SPAN))
    {
        return false;
    }

    boot_seq     = flash_get_u32(&buf[LOG_META_OFF_BOOT_SEQ]);
    seq_reserved = flash_get_u32(&buf[LOG_META_OFF_SEQ_RESERVED]);
    corrupt      = flash_get_u32(&buf[LOG_META_OFF_CORRUPT]);

    return true;
}

// 写 meta（整块 32 B 覆写，含 CRC）
static bool meta_write(uint32_t boot_seq, uint32_t seq_reserved, uint32_t corrupt)
{
    // 故障注入（仅测试用，一次性）
    if (s_meta_fail_next)
    {
        s_meta_fail_next = false;
        return false;
    }

    uint8_t buf[LOG_META_SIZE];
    memset(buf, 0, sizeof(buf));

    flash_put_u32(&buf[LOG_META_OFF_MAGIC], LOG_META_MAGIC);
    flash_put_u32(&buf[LOG_META_OFF_FMT], LOG_META_VERSION);
    flash_put_u32(&buf[LOG_META_OFF_BOOT_SEQ], boot_seq);
    flash_put_u32(&buf[LOG_META_OFF_SEQ_RESERVED], seq_reserved);
    flash_put_u32(&buf[LOG_META_OFF_CORRUPT], corrupt);
    flash_put_u32(&buf[LOG_META_OFF_CRC32], log_crc32(buf, LOG_META_CRC_SPAN));

    File f = LittleFS.open(LOG_META_PATH, "w");

    if (!f)
    {
        return false;
    }

    const bool ok = (f.write(buf, sizeof(buf)) == (size_t)sizeof(buf));
    f.close();

    return ok;
}

// 扫描全部有效 segment 求已使用的最大 seq（§16-1..4）
//
// 依赖 flash_scan_all() 建立的 s_seg_present / s_seg_records，
// 因此必须在扫描之后调用。
static uint32_t flash_find_max_seq()
{
    uint32_t max_seq = 0;
    LogRecord rec;

    for (uint32_t seg = 0; seg < LOG_SEGMENT_COUNT; seg++)
    {
        if (!s_seg_present[seg])
        {
            continue;
        }

        char path[32];
        flash_seg_path(seg, path, sizeof(path));

        File f = LittleFS.open(path, "r");

        if (!f)
        {
            continue;
        }

        for (uint8_t i = 0; i < s_seg_records[seg]; i++)
        {
            const uint32_t off =
                LOG_SEGMENT_HEADER_SIZE + (uint32_t)i * LOG_RECORD_SIZE;

            if (!f.seek(off))
            {
                break;
            }

            if (f.read((uint8_t *)&rec, LOG_RECORD_SIZE) != (size_t)LOG_RECORD_SIZE)
            {
                break;
            }

            if (rec.seq > max_seq)
            {
                max_seq = rec.seq;
            }
        }

        f.close();
    }

    return max_seq;
}

// 预留一个新 seq 区间（§13）
//
//   seq_base = meta.seq_reserved + 1
//   meta.seq_reserved += LOG_SEQ_RESERVE     ← 立即落盘
//
// 返回 false = meta 写入失败 → 调用方置 s_seq_reliable = false（§15）
static bool seq_reserve_next()
{
    const uint32_t base = s_meta_reserved + 1;
    const uint32_t next_reserved = s_meta_reserved + LOG_SEQ_RESERVE;

    if (!meta_write(s_boot_seq, next_reserved, s_meta_corrupt))
    {
        return false;
    }

    s_meta_reserved = next_reserved;
    s_seq_base = base;
    s_seq_limit = base + LOG_SEQ_RESERVE - 1;

    // 分配指针对齐到区间起点之前（不倒退已有进度）
    if (s_seq_last < base - 1)
    {
        s_seq_last = base - 1;
    }

    return true;
}

// Boot 时初始化 sequence（§13 / §16）
//
// 必须在 flash_init()（含 flash_scan_all）之后调用。
static void seq_init()
{
    uint32_t boot = 0;
    uint32_t reserved = 0;
    uint32_t corrupt = 0;

    const bool have_meta = meta_read(boot, reserved, corrupt);

    if (have_meta)
    {
        // §18：本次扫描发现的损坏段也累加进持久化计数
        corrupt = corrupt + s_scan_corrupt;
        s_meta_corrupt = corrupt;
    }
    else
    {
        // §16：meta 不存在或损坏 → 扫描现有 segment 取最大 seq，向上对齐重建
        const uint32_t max_seq = flash_find_max_seq();

        reserved = ((max_seq + LOG_SEQ_RESERVE - 1) / LOG_SEQ_RESERVE) * LOG_SEQ_RESERVE;
        corrupt = corrupt + 1 + s_scan_corrupt;      // §16-7 + §18

        s_meta_corrupt = corrupt;

        Serial.printf(
            "[Log] meta missing/corrupt -> rebuild: max_seq=%u reserved=%u corrupt=%u\n",
            (unsigned)max_seq, (unsigned)reserved, (unsigned)corrupt);
    }

    s_meta_reserved = reserved;
    s_boot_seq = boot + 1;                            // §13：boot_seq++

    if (!seq_reserve_next())
    {
        // §15：写失败 → 明确标记不可信，但**不阻塞、不死等**，继续以本地区间工作
        s_seq_reliable = false;
        s_seq_base = s_meta_reserved + 1;
        s_seq_limit = s_seq_base + LOG_SEQ_RESERVE - 1;

        if (s_seq_last < s_seq_base - 1)
        {
            s_seq_last = s_seq_base - 1;
        }

        Serial.println("[Log] seq reservation failed -> seq_reliable=false");
    }
}

// Flash 层初始化：建目录 + 批量缓冲（PSRAM 优先）+ 全量扫描
static bool flash_init()
{
    if (!LittleFS.exists(LOG_DIR_PATH))
    {
        if (!LittleFS.mkdir(LOG_DIR_PATH))
        {
            Serial.println("[Log] mkdir /log failed");
            return false;
        }
    }

    // 批量工作缓冲（§31/§33：优先 PSRAM）
    s_flash_batch_bytes = (uint32_t)LOG_FLUSH_RECORDS * LOG_RECORD_SIZE;

    void *mem = heap_caps_malloc(s_flash_batch_bytes, MALLOC_CAP_SPIRAM);

    if (mem != nullptr)
    {
        s_flash_batch_in_psram = true;
    }
    else
    {
        mem = heap_caps_malloc(s_flash_batch_bytes, MALLOC_CAP_8BIT);
        s_flash_batch_in_psram = false;
    }

    if (mem == nullptr)
    {
        Serial.printf("[Log] Flash batch alloc failed (%u B)\n",
                      (unsigned)s_flash_batch_bytes);
        return false;
    }

    s_flash_batch = (LogRecord *)mem;
    memset(s_flash_batch, 0, s_flash_batch_bytes);

    flash_scan_all();

    Serial.printf(
        "[Log] Flash ring ready: seg=%u x %u rec, seg_size=%u B, "
        "valid=%u, oldest=%u, newest=%u, append=%u@%u, total_rec=%u, batch=%u B in %s\n",
        (unsigned)LOG_SEGMENT_COUNT,
        (unsigned)LOG_RECORDS_PER_SEGMENT,
        (unsigned)LOG_SEGMENT_SIZE,
        (unsigned)s_valid_segment_count,
        (unsigned)s_oldest_segment,
        (unsigned)s_newest_segment,
        (unsigned)s_append_segment,
        (unsigned)s_append_index,
        (unsigned)s_flash_total_records,
        (unsigned)s_flash_batch_bytes,
        s_flash_batch_in_psram ? "PSRAM" : "DRAM");

    return true;
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
    // boot_seq / seq 区间由 seq_init() 在 Flash 扫描之后统一建立（§13）

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

    // ---- P1.3：Flash Segment Ring（建目录 + 批量缓冲 + 全量扫描）----
    s_flash_ready = flash_init();

    if (!s_flash_ready)
    {
        // Flash 不可用：RAM 环与 routing 决策继续工作（受控 degraded），
        // 不阻塞、不死等；具体失败计数由 log_task() 的 I/O 区累加。
        Serial.println("[Log] Flash ring unavailable (degraded)");
    }

    // ---- P1.3：meta.bin / sequence reservation（依赖 scan 结果）----
    seq_init();

    Serial.printf(
        "[Log] seq: boot_seq=%u base=%u limit=%u reserved=%u reliable=%u corrupt=%u\n",
        (unsigned)s_boot_seq,
        (unsigned)s_seq_base,
        (unsigned)s_seq_limit,
        (unsigned)s_meta_reserved,
        s_seq_reliable ? 1u : 0u,
        (unsigned)s_meta_corrupt);

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

    // ---- seq 区间用尽则先预留（§13）----
    //
    // 预留需要写 meta.bin（Flash I/O）→ **禁止放进临界区**（§3）。
    // 用 s_seq_reserving 门闩避免多任务重复预留；实际分配仍在线性临界区内。
    {
        bool need_reserve = false;

        portENTER_CRITICAL(&s_mux);

        if (s_seq_last + 1 > s_seq_limit && !s_seq_reserving)
        {
            s_seq_reserving = true;
            need_reserve = true;
        }

        portEXIT_CRITICAL(&s_mux);

        if (need_reserve)
        {
            const bool ok = seq_reserve_next();   // Flash I/O，位于临界区【外】

            portENTER_CRITICAL(&s_mux);
            s_seq_reserving = false;

            if (!ok)
            {
                // §15：meta 写失败 → sequence 可能重复，明确降级后继续工作
                s_seq_reliable = false;
                s_seq_base = s_meta_reserved + 1;
                s_seq_limit = s_seq_base + LOG_SEQ_RESERVE - 1;

                if (s_seq_last < s_seq_base - 1)
                {
                    s_seq_last = s_seq_base - 1;
                }
            }

            portEXIT_CRITICAL(&s_mux);
        }
    }

    portENTER_CRITICAL(&s_mux);

    // seq 在临界区内推进：唯一、单调（跨区间允许空洞）
    s_seq_last++;
    rec.seq = s_seq_last;
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
    s_stats.last_seq = s_seq_last;
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

    // 本轮收集待落盘记录；循环结束后**一次性**写入
    // （§30：一次 log_task() 最多执行一个 Flash append 单元）
    uint8_t batch_n = 0;

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

        // ---- I/O 区 ----
        //
        // P1.3：把需落 Flash 的记录收集进批量缓冲 —— 真正的写盘在循环外，
        //       以"一个 append 单元"为粒度（§30），避免长时间占用 loop。
        // P1.4：Cloud Log Queue 投递
        // ★ 严格要求：耗时 I/O 必须在临界区【外】执行 ——
        //   禁止把 Flash 写入或 MQTT publish 放进上面的 s_mux 段。
        if (to_flash)
        {
            if (s_flash_ready && s_flash_batch != nullptr &&
                batch_n < LOG_FLUSH_RECORDS)
            {
                memcpy(&s_flash_batch[batch_n], &rec, LOG_RECORD_SIZE);
                batch_n++;
            }
            else
            {
                // Flash 未就绪 / 缓冲异常：显式计失败，绝不静默丢弃
                // （Commit 5 会把此处改为"记录保留 + 下一轮重试"）
                portENTER_CRITICAL(&s_mux);
                s_stats.flash_append_fail++;
                portEXIT_CRITICAL(&s_mux);
            }
        }
    }

    // ---- 本轮唯一的 Flash append 单元 ----
    if (batch_n > 0)
    {
        const uint8_t written = flash_append_batch(s_flash_batch, batch_n);

        portENTER_CRITICAL(&s_mux);
        s_stats.flash_append_ok += written;
        s_stats.flash_append_fail += (uint32_t)(batch_n - written);
        portEXIT_CRITICAL(&s_mux);
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

// =====================================================
// P1.3 Flash 观测 / 测试钩子（仅上板自测，见头文件说明）
// =====================================================

void log_flash_get_info(LogFlashInfo &out)
{
    memset(&out, 0, sizeof(out));

    out.flash_ready    = s_flash_ready ? 1u : 0u;
    out.valid_segments = s_valid_segment_count;
    out.oldest_segment = s_oldest_segment;
    out.newest_segment = s_newest_segment;
    out.append_segment = s_append_segment;
    out.append_index   = s_append_index;
    out.total_records  = s_flash_total_records;

    out.first_seq_oldest = s_seg_present[s_oldest_segment]
                               ? s_seg_first_seq[s_oldest_segment] : 0u;
    out.first_seq_newest = s_seg_present[s_newest_segment]
                               ? s_seg_first_seq[s_newest_segment] : 0u;

    out.batch_bytes    = s_flash_batch_bytes;
    out.batch_in_psram = s_flash_batch_in_psram ? 1u : 0u;

    // seq / meta 状态（与 log_emit 的写入统一用 s_mux）
    portENTER_CRITICAL(&s_mux);
    out.seq_reliable  = s_seq_reliable ? 1u : 0u;
    out.boot_seq      = s_boot_seq;
    out.seq_reserved  = s_meta_reserved;
    out.corrupt_count = s_meta_corrupt;
    out.seq_base      = s_seq_base;
    out.seq_limit     = s_seq_limit;
    out.seq_last      = s_seq_last;
    portEXIT_CRITICAL(&s_mux);
}

bool log_flash_peek_segment(uint32_t seg, LogSegmentHead &out)
{
    memset(&out, 0, sizeof(out));

    if (seg >= LOG_SEGMENT_COUNT)
    {
        return false;
    }

    char path[32];
    flash_seg_path(seg, path, sizeof(path));

    if (!LittleFS.exists(path))
    {
        return false;
    }

    File f = LittleFS.open(path, "r");

    if (!f)
    {
        return false;
    }

    uint8_t head[LOG_SEGMENT_HEADER_SIZE];

    if (f.read(head, sizeof(head)) != (size_t)sizeof(head))
    {
        f.close();
        return false;
    }

    out.magic     = flash_get_u32(&head[LOG_SEG_OFF_MAGIC]);
    out.index     = flash_get_u32(&head[LOG_SEG_OFF_INDEX]);
    out.first_seq = flash_get_u32(&head[LOG_SEG_OFF_FIRST_SEQ]);
    out.crc32     = flash_get_u32(&head[LOG_SEG_OFF_CRC32]);
    out.ok        = flash_check_head(head, seg) ? 1u : 0u;
    out.records   = out.ok ? flash_scan_records(f, false) : 0u;

    f.close();
    return true;
}

bool log_flash_peek_record(uint32_t seg, uint8_t rec, LogRecord &out, uint8_t &crc_ok)
{
    crc_ok = 0;

    if (seg >= LOG_SEGMENT_COUNT || rec >= LOG_RECORDS_PER_SEGMENT)
    {
        return false;
    }

    char path[32];
    flash_seg_path(seg, path, sizeof(path));

    File f = LittleFS.open(path, "r");

    if (!f)
    {
        return false;
    }

    const uint32_t off =
        LOG_SEGMENT_HEADER_SIZE + (uint32_t)rec * LOG_RECORD_SIZE;

    bool ok = f.seek(off);

    if (ok)
    {
        ok = (f.read((uint8_t *)&out, LOG_RECORD_SIZE) == (size_t)LOG_RECORD_SIZE);
    }

    f.close();

    if (!ok)
    {
        return false;
    }

    crc_ok = flash_check_record(out) ? 1u : 0u;
    return true;
}

bool log_flash_wipe()
{
    bool ok = true;
    char path[32];

    for (uint32_t i = 0; i < LOG_SEGMENT_COUNT; i++)
    {
        flash_seg_path(i, path, sizeof(path));

        if (LittleFS.exists(path) && !LittleFS.remove(path))
        {
            ok = false;
        }
    }

    if (LittleFS.exists(LOG_META_PATH) && !LittleFS.remove(LOG_META_PATH))
    {
        ok = false;
    }

    flash_scan_all();   // 重建元信息（空 ring）

    return ok;
}

bool log_meta_wipe()
{
    if (LittleFS.exists(LOG_META_PATH))
    {
        return LittleFS.remove(LOG_META_PATH);
    }

    return true;
}

bool log_meta_corrupt()
{
    // 翻转文件最后一个字节（落在 crc32 字段内）→ CRC 校验必然失败
    File f = LittleFS.open(LOG_META_PATH, "r+");

    if (!f)
    {
        return false;
    }

    const size_t sz = f.size();

    if (sz != (size_t)LOG_META_SIZE)
    {
        f.close();
        return false;
    }

    uint8_t b = 0;

    if (!f.seek((uint32_t)(sz - 1)) || (f.read(&b, 1) != 1))
    {
        f.close();
        return false;
    }

    b ^= 0xFFu;

    if (!f.seek((uint32_t)(sz - 1)) || (f.write(&b, 1) != 1))
    {
        f.close();
        return false;
    }

    f.close();
    return true;
}

void log_meta_test_fail_next(bool enable)
{
    s_meta_fail_next = enable;
}

// ---- Commit 4：损坏注入钩子（仅上板自测）----

// 翻转文件某字节（读-改-写），用于构造损坏数据
static bool flash_flip_byte(const char *path, uint32_t off)
{
    File f = LittleFS.open(path, "r+");

    if (!f)
    {
        return false;
    }

    uint8_t b = 0;

    if (!f.seek(off) || (f.read(&b, 1) != 1))
    {
        f.close();
        return false;
    }

    b ^= 0xFFu;

    if (!f.seek(off) || (f.write(&b, 1) != 1))
    {
        f.close();
        return false;
    }

    f.close();
    return true;
}

bool log_seg_corrupt_head(uint32_t seg)
{
    if (seg >= LOG_SEGMENT_COUNT)
    {
        return false;
    }

    char path[32];
    flash_seg_path(seg, path, sizeof(path));

    if (!LittleFS.exists(path))
    {
        return false;
    }

    // 翻转 seg_index（offset 4）→ 头 CRC 必然不匹配（§18 场景）
    const bool ok = flash_flip_byte(path, LOG_SEG_OFF_INDEX);

    if (ok)
    {
        flash_scan_all();   // 模拟"重新初始化"后的扫描（§40-F7）
    }

    return ok;
}

bool log_seg_corrupt_record(uint32_t seg, uint8_t rec)
{
    if (seg >= LOG_SEGMENT_COUNT || rec >= LOG_RECORDS_PER_SEGMENT)
    {
        return false;
    }

    char path[32];
    flash_seg_path(seg, path, sizeof(path));

    if (!LittleFS.exists(path))
    {
        return false;
    }

    // 翻转该 record 的 byte 0（version，落在 CRC 覆盖区 [0..107]）
    const uint32_t off =
        LOG_SEGMENT_HEADER_SIZE + (uint32_t)rec * LOG_RECORD_SIZE;

    const bool ok = flash_flip_byte(path, off);

    if (ok)
    {
        flash_scan_all();   // 模拟"重新初始化"后的扫描（§40-F8）
    }

    return ok;
}

bool log_seg_truncate(uint32_t seg, uint32_t bytes)
{
    if (seg >= LOG_SEGMENT_COUNT)
    {
        return false;
    }

    if (bytes > LOG_SEGMENT_SIZE)
    {
        bytes = LOG_SEGMENT_SIZE;
    }

    char path[32];
    flash_seg_path(seg, path, sizeof(path));

    if (!LittleFS.exists(path))
    {
        return false;
    }

    // 读回原内容（≤ 3984 B，PSRAM 优先）
    uint8_t *buf = (uint8_t *)heap_caps_malloc(LOG_SEGMENT_SIZE, MALLOC_CAP_SPIRAM);

    if (buf == nullptr)
    {
        buf = (uint8_t *)heap_caps_malloc(LOG_SEGMENT_SIZE, MALLOC_CAP_8BIT);
    }

    if (buf == nullptr)
    {
        return false;
    }

    size_t n = 0;
    File f = LittleFS.open(path, "r");

    if (f)
    {
        n = f.read(buf, LOG_SEGMENT_SIZE);
        f.close();
    }

    bool ok = LittleFS.remove(path);

    if (ok)
    {
        File w = LittleFS.open(path, "w");

        if (!w)
        {
            ok = false;
        }
        else
        {
            const uint32_t take = (bytes < (uint32_t)n) ? bytes : (uint32_t)n;
            ok = (w.write(buf, take) == (size_t)take);
            w.close();
        }
    }

    heap_caps_free(buf);

    if (ok)
    {
        flash_scan_all();   // 模拟"重新初始化"后的扫描（§40-F9）
    }

    return ok;
}
