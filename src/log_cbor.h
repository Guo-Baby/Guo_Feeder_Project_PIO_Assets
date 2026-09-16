// =====================================================
// LogManager —— CBOR 批次编码器（P1.4）
//
// 为什么单独成头文件：
//   · 只依赖 <stdint.h> / <stddef.h> + log_events.h，**完全 freestanding**
//     （连 <string.h> 都不用，拷贝用本文件的字节循环）
//     ⇒ 可以用主机编译器编译成 wasm32 并**真的跑起来**做字节级验证，
//       也可用项目交叉编译器 -fsyntax-only 秒级检查，
//       见 test/log_contract/probe_cbor.cpp
//   · log_manager.cpp 只负责"取数据 + 调这里"，编码逻辑可独立回归。
//
// 编码形态：CBOR **定序数组**，下标即 log_events.h 冻结的 LOG_BKEY_*（0..11）
//
//   [ fmt, event_dict_ver, boot_seq, seq_from, seq_to, count,
//     drop_ring, drop_overflow, drop_unacked, self_degraded, flags,
//     records[] ]                      ← 共 12 项
//
//   records 每项 = bstr(128 B)：**原样保留冻结的 LogRecord 布局**，
//   不做逐字段 CBOR 化 —— 既省带宽，也避免云端与固件产生第二套字段定义。
// =====================================================

#ifndef LOG_CBOR_H
#define LOG_CBOR_H

#include <stdint.h>
#include <stddef.h>

#include "log_events.h"

// 批次头固定 12 项（= LOG_BKEY 0..11 的个数）
#define LOG_CBOR_BATCH_ITEMS 12u

// 字节拷贝：刻意不用 <string.h>，保持本头文件 freestanding
// （128 B 的循环在 ESP32 上会被编译器展开，开销可忽略）
static inline void log_cbor_copy(uint8_t *dst, const uint8_t *src, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++)
    {
        dst[i] = src[i];
    }
}

// ---- 极简 CBOR 写入器（只覆盖本批次需要的类型）----

static inline size_t log_cbor_put_raw(uint8_t *out, size_t pos,
                                      uint8_t major, uint32_t v)
{
    const uint8_t m = (uint8_t)(major << 5u);

    if (v < 24u)
    {
        out[pos++] = (uint8_t)(m | (uint8_t)v);
    }
    else if (v <= 0xFFu)
    {
        out[pos++] = (uint8_t)(m | 24u);
        out[pos++] = (uint8_t)v;
    }
    else if (v <= 0xFFFFu)
    {
        out[pos++] = (uint8_t)(m | 25u);
        out[pos++] = (uint8_t)(v >> 8);
        out[pos++] = (uint8_t)v;
    }
    else
    {
        out[pos++] = (uint8_t)(m | 26u);
        out[pos++] = (uint8_t)(v >> 24);
        out[pos++] = (uint8_t)(v >> 16);
        out[pos++] = (uint8_t)(v >> 8);
        out[pos++] = (uint8_t)v;
    }

    return pos;
}

static inline size_t log_cbor_put_uint(uint8_t *out, size_t pos, uint32_t v)
{
    return log_cbor_put_raw(out, pos, 0u, v);       // major 0 = unsigned int
}

static inline size_t log_cbor_put_array(uint8_t *out, size_t pos, uint32_t n)
{
    return log_cbor_put_raw(out, pos, 4u, n);       // major 4 = array
}

static inline size_t log_cbor_put_bstr(uint8_t *out, size_t pos,
                                       const uint8_t *data, uint32_t len)
{
    pos = log_cbor_put_raw(out, pos, 2u, len);      // major 2 = byte string
    log_cbor_copy(&out[pos], data, len);
    return pos + len;
}

// ---- 批次头 ----

struct LogCborBatchHeader
{
    uint32_t fmt;              // LOG_BATCH_FMT
    uint32_t event_dict_ver;   // 事件字典版本
    uint32_t boot_seq;
    uint32_t seq_from;
    uint32_t seq_to;
    uint32_t count;            // = records 条数
    uint32_t drop_ring;
    uint32_t drop_overflow;
    uint32_t drop_unacked;
    uint32_t self_degraded;
    uint32_t flags;            // bit0 = seq_reliable
};

// 编码后的**最大**字节数（用于发送前容量预检）
static inline size_t log_cbor_batch_max_size(uint32_t count)
{
    // array(12) : 1
    // 11 个 uint: 最坏 5 B 各 = 55
    // array(count): count < 24 → 1
    // count × bstr(128): 3 + 128 = 131
    return 1u + 55u + 1u + (size_t)count * (3u + LOG_RECORD_SIZE);
}

// 编码整个批次；返回写入字节数。
// out_cap 不足 / 参数非法 → 返回 0（调用方保留记录、下轮再试）
static inline uint32_t log_cbor_encode_batch(
    const LogCborBatchHeader &h,
    const LogRecord *records,
    uint8_t *out,
    size_t out_cap)
{
    if (out == nullptr || records == nullptr || h.count == 0u)
    {
        return 0u;
    }

    if (h.count > LOG_BATCH_MAX_RECORDS)
    {
        return 0u;
    }

    if (out_cap < log_cbor_batch_max_size(h.count))
    {
        return 0u;
    }

    size_t pos = 0;

    pos = log_cbor_put_array(out, pos, LOG_CBOR_BATCH_ITEMS);
    pos = log_cbor_put_uint(out, pos, h.fmt);              //  0
    pos = log_cbor_put_uint(out, pos, h.event_dict_ver);   //  1
    pos = log_cbor_put_uint(out, pos, h.boot_seq);         //  2
    pos = log_cbor_put_uint(out, pos, h.seq_from);         //  3
    pos = log_cbor_put_uint(out, pos, h.seq_to);           //  4
    pos = log_cbor_put_uint(out, pos, h.count);            //  5
    pos = log_cbor_put_uint(out, pos, h.drop_ring);        //  6
    pos = log_cbor_put_uint(out, pos, h.drop_overflow);    //  7
    pos = log_cbor_put_uint(out, pos, h.drop_unacked);     //  8
    pos = log_cbor_put_uint(out, pos, h.self_degraded);    //  9
    pos = log_cbor_put_uint(out, pos, h.flags);            // 10
    pos = log_cbor_put_array(out, pos, h.count);           // 11 records

    for (uint32_t i = 0; i < h.count; i++)
    {
        pos = log_cbor_put_bstr(out, pos,
                                (const uint8_t *)&records[i],
                                LOG_RECORD_SIZE);
    }

    return (uint32_t)pos;
}

#endif // LOG_CBOR_H
