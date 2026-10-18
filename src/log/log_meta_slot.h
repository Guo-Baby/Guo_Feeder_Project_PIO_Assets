// =====================================================
// `HD-3` · `A-1`：双槽元数据（`meta_a.bin` / `meta_b.bin`）的**纯逻辑**
//
// 为什么单独成头文件：
//   `log_manager.cpp` 里的双槽实现依赖 LittleFS ⇒ **无法在主机上执行**。
//   把"序列化 / 校验 / 选槽"抽成 freestanding 的 inline 函数后，
//   `test/log_contract/probe_meta_slot.cpp`（clang → wasm32 → node）
//   可以**真跑真实逻辑**，而不是只做代码审查。
//
// 明确**不**在此处实现（仍属 `log_manager.cpp` + 代码审查）：
//   · 文件读写、`LittleFS.open/close` 的返回值处理
//   · 两阶段提交的**时序**（先 commit=0 再 commit=1）
//   · 掉电发生在两次写之间时的实际 Flash 行为
//   ⇒ 探针通过**不得**表述为"掉电原子性已在真机验证"。
//
// 槽布局（32 B，**与 legacy `meta.bin` 同尺寸、同 CRC 跨度**）：
//   0   magic        0x474C4F47
//   4   fmt_version  3（槽专用；legacy = 2）
//   8   boot_seq
//   12  seq_reserved
//   16  corrupt_count
//   20  slot_seq     提交序号（选槽依据）
//   24  commit       1 = 有效；0 = 写入中 / 已失效
//   28  crc32        覆盖 [0..27]（**含** slot_seq 与 commit）
// =====================================================

#ifndef LOG_META_SLOT_H
#define LOG_META_SLOT_H

#include <stdint.h>

#include "log/log_manager.h"

namespace log_meta_slot
{

// CRC32 —— 与项目既有实现**逐字一致**
// （init 0xFFFFFFFF · poly 0xEDB88320 反射 · final xor 0xFFFFFFFF）
inline uint32_t crc32(const uint8_t *data, uint32_t len)
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

// 小端 u32
inline void put_u32(uint8_t *dst, uint32_t v)
{
    dst[0] = (uint8_t)(v & 0xFFu);
    dst[1] = (uint8_t)((v >> 8) & 0xFFu);
    dst[2] = (uint8_t)((v >> 16) & 0xFFu);
    dst[3] = (uint8_t)((v >> 24) & 0xFFu);
}

inline uint32_t get_u32(const uint8_t *src)
{
    return (uint32_t)src[0]
         | ((uint32_t)src[1] << 8)
         | ((uint32_t)src[2] << 16)
         | ((uint32_t)src[3] << 24);
}

/**
 * 序列化一个槽（固定 32 B）。
 *
 * @param commit `LOG_META_COMMIT_MARK` = 有效；`0` = 写入中 / 失效
 */
inline void pack(uint8_t out[LOG_META_SIZE],
                 uint32_t boot_seq, uint32_t seq_reserved, uint32_t corrupt,
                 uint32_t slot_seq, uint32_t commit)
{
    /* 手工清零（**不依赖 `<string.h>`**）：wasm32 `-nostdlib` 探针没有 CRT 头，
       本头必须完全 freestanding，才能在主机上真跑。 */
    for (uint32_t i = 0; i < LOG_META_SIZE; i++)
    {
        out[i] = 0;
    }

    put_u32(&out[LOG_META_OFF_MAGIC],        LOG_META_MAGIC);
    put_u32(&out[LOG_META_OFF_FMT],          LOG_META_SLOT_FMT);
    put_u32(&out[LOG_META_OFF_BOOT_SEQ],     boot_seq);
    put_u32(&out[LOG_META_OFF_SEQ_RESERVED], seq_reserved);
    put_u32(&out[LOG_META_OFF_CORRUPT],      corrupt);
    put_u32(&out[LOG_META_OFF_SLOT_SEQ],     slot_seq);
    put_u32(&out[LOG_META_OFF_COMMIT],       commit);
    put_u32(&out[LOG_META_OFF_CRC32],        crc32(out, LOG_META_CRC_SPAN));
}

/**
 * 解析并**校验**一个槽。
 *
 * 四项全过才算有效（顺序即为拒绝理由的优先级）：
 *   ① magic ② fmt=3 ③ **commit=1** ④ CRC32
 *
 * ★ ③ 是"两阶段提交"的安全支点：半写槽的 `commit` 仍是 0 ⇒ 一律无效。
 *
 * @returns true = 有效；false = 无效（**不得**使用任何输出参数）
 */
inline bool parse(const uint8_t in[LOG_META_SIZE],
                  uint32_t &boot_seq, uint32_t &seq_reserved,
                  uint32_t &corrupt, uint32_t &slot_seq)
{
    if (get_u32(&in[LOG_META_OFF_MAGIC]) != LOG_META_MAGIC)
    {
        return false;
    }

    if (get_u32(&in[LOG_META_OFF_FMT]) != LOG_META_SLOT_FMT)
    {
        return false;
    }

    if (get_u32(&in[LOG_META_OFF_COMMIT]) != LOG_META_COMMIT_MARK)
    {
        return false;
    }

    if (get_u32(&in[LOG_META_OFF_CRC32]) != crc32(in, LOG_META_CRC_SPAN))
    {
        return false;
    }

    boot_seq     = get_u32(&in[LOG_META_OFF_BOOT_SEQ]);
    seq_reserved = get_u32(&in[LOG_META_OFF_SEQ_RESERVED]);
    corrupt      = get_u32(&in[LOG_META_OFF_CORRUPT]);
    slot_seq     = get_u32(&in[LOG_META_OFF_SLOT_SEQ]);

    return true;
}

/**
 * 选槽：`slot_seq` 大者胜（相等时取 A，保证**确定性**）。
 *
 * @returns 0 = A · 1 = B · -1 = 两槽都无效
 */
inline int8_t choose(bool ok_a, uint32_t seq_a, bool ok_b, uint32_t seq_b)
{
    if (ok_a && ok_b)
    {
        return (seq_a >= seq_b) ? 0 : 1;
    }

    if (ok_a)
    {
        return 0;
    }

    if (ok_b)
    {
        return 1;
    }

    return -1;
}

// -----------------------------------------------------------------------------
// ⚠️ 恢复来源判定（原 `decide_boot_source`）**已迁出**
//
//   `HD-3`·`A-1` 修复（`2026-10-18`）后，判据必须同时考虑**独立于 LittleFS**
//   的持久证据（NVS）与**跨重启固化的隔离标记** ⇒ 现位于
//   `log/log_meta_evidence.h::log_meta_evidence::decide_recovery()`。
//
//   本文件**只保留**"槽格式 + 槽内数据校验 + 选槽"这一层纯逻辑；
//   迁移原因与 `A1-01`/`A1-02` 的缺陷对应关系见
//   `docs/architecture/HD-3-A1-B3-Independent-Review.md` §6。
// -----------------------------------------------------------------------------

}   // namespace log_meta_slot

#endif  // LOG_META_SLOT_H
