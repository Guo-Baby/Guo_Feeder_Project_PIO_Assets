// =====================================================
// `HD-3` · `A-1` 修复（`2026-10-18`）—— **持久化证据**（`NVS` 侧）的纯逻辑
//
// 为什么需要这一层（`A1-01` / `A1-02` 的根因）：
//   `log_meta_slot.h` 的恢复判据只看 **日志区自身**（双槽 / legacy / 日志段）。
//   一旦日志区被整体毁掉（`LittleFS.begin(true, …)` 的 `formatOnFail` 会**主动**
//   格式化），设备就被判成「全新设备」⇒ `boot_seq=1` ⇒ 与云端历史撞
//   `(device_id, boot_seq, seq)` —— 而 `device_id` 由 eFuse MAC 派生且存在 NVS，
//   格式化后**身份不变**，故云端历史仍在。
//
// 本层的职责：
//   ① 提供一条**独立于 LittleFS** 的持久证据（NVS 分区不随 LittleFS 格式化丢失）；
//   ② 把「无法证明 `boot_seq` 唯一」这一状态**跨重启固化**（隔离标记）；
//   ③ 把「真·首启」与「存储被清空」在判据上**分开**（不再等价）。
//
// 持久化边界（必须原样理解，不得扩大表述）：
//   · **NVS 分区**：`LittleFS` 格式化 / 日志区损坏 / 双槽损坏 ⇒ 证据**仍在**
//   · **整片擦除（`erase_flash`）+ 重烧**：NVS 与 LittleFS **同时**丢失 ⇒ 证据消失
//     ⇒ 该情形**无法在设备侧与全新设备区分**；按 §3 的定义它等价于
//     「受控恢复出厂设置」，必须由服务侧的 `boot_seq` 回退检测兜底
//     （属协议/服务变更 ⇒ `HUMAN_REQUIRED`，不在本次授权内）
//   · 证据自身的**双份**：NVS 记录 + `/log/quarantine.bin`；任一存活即可
//     ⇒ 两者同时丢失才退化为「无证据」
//
// -----------------------------------------------------------------------------
// 场景判定矩阵（**唯一权威表**；`decide_recovery()` 即此表的实现）
// -----------------------------------------------------------------------------
//  场景                          NVS证据  日志区(槽/legacy/段)  首启标记   判定            boot_seq
//  ----------------------------  -------  --------------------  ---------  --------------  --------------
//  ① 真·首启（新机/新烧录）        无        空                   true      PROVISION       1
//  ② 普通重启                     有        有                   false     FROM_NVS        高水位 + 1
//  ③ 老固件升级到本固件            无        有(legacy/段)        false     LEGACY / SEG    高水位 + 1
//  ④ LittleFS 格式化              有        空                   false     FROM_NVS        高水位 + 1
//  ⑤ NVS 证据丢失（日志区健在）     无        有                   false     SLOT / LEGACY / SEG  高水位 + 1
//  ⑥ NVS 证据丢失 + 日志区为空      无        空                   false     **QUARANTINE**  不分配
//  ⑦ **两类存储同时丢失（整片擦除）** 无       空                   true      PROVISION       1  ← **未关闭风险**
//  ⑧ 受控恢复出厂（`lqclear`）      有(清隔离位) 任意               任意      FROM_NVS        保留 hw + 1
//
// ★ ⑦ 是本设计**唯一**无法在设备侧消除的情形：整片擦除后 NVS 与 LittleFS 同时为空，
//   与"新机"在本地**不可区分**。本文件**不**把它认定为"用户主动恢复出厂"，
//   只承认"设备侧判据在此失效" ⇒ 风险**保持未关闭**，须由服务侧 `boot_seq`
//   回退检测兜底（`docs/architecture/HD-3-A1-Recovery-Scenarios-and-RealDevice-Matrix.md`）。
//
// -----------------------------------------------------------------------------
// 第五条持久线索：`/log/corrupt.trace`（`2026-10-19` 追加 · `A1-02` 残留修复）
// -----------------------------------------------------------------------------
//   `flash_drop_broken_segment()` 会 `LittleFS.remove()` 掉损坏段 ⇒ **段文件本身**
//   不再是跨重启证据。trace 记录"累计损坏次数"，`flash_scan_all()` 与本次扫描**取大**：
//     · B1 发现 2 个坏段 ⇒ `scan_corrupt=2` + `trace=2` ⇒ 判 QUARANTINE
//     · 若隔离标记两份都写失败（NVS 分区异常 + LittleFS 只读）⇒ 隔离只剩 RAM
//     · B2 坏段已被删（`scan=0`）但 `trace=2` ⇒ 合并后仍 2 ⇒ **仍 QUARANTINE**
//       （旧实现在这里会退化为 `scan=0 + total_records=0` ⇒ 判 `PROVISION` ⇒
//         `boot_seq=1` ⇒ 与云端历史撞键）
//
//   ⚠️ trace 与日志区**同生共死**（同一 LittleFS）；格式化后它也会消失 ——
//      该情形由 NVS 证据兜底（`REC_FROM_NVS` 或隔离），不依赖本记录。
//   ⚠️ 若 NVS 证据、`/log/quarantine.bin`、`corrupt.trace` **三者全部**写失败，
//      设备侧**没有**任何跨重启证据 ⇒ 只能由服务侧 `boot_seq` 水位兜底
//      （`Cloudflare_Assets/guo-feeder-api/src/log-boot-watermark.js`，
//        默认关闭、未部署）。该边界**不得**声称已闭环。
//
// 明确**不**在此处实现（仍属 `log_manager.cpp` + 代码审查）：
//   · NVS / LittleFS 的实际读写与返回值处理
//   · 证据写入的时序与失败降级
// =====================================================

#ifndef LOG_META_EVIDENCE_H
#define LOG_META_EVIDENCE_H

#include <stdint.h>

#include "log/log_manager.h"

namespace log_meta_evidence
{

// 证据记录布局（32 B，与 meta 槽同尺寸，便于复用同一套序列化风格）
//   0   magic        0x454E4C47 ("GLNE")
//   4   fmt_version  1
//   8   boot_hw      最近一次已知 `boot_seq`（**正在使用**的值，非 next）
//   12  reserved_hw  `seq` 预留高水位
//   16  flags        bit0 = 隔离中；bit1 = 曾经完成过 provision
//   20  gen          写入代数（诊断用，单调 +1）
//   24  corrupt      `corrupt_count` 快照
//   28  crc32        覆盖 [0..27]
#define LOG_EV_MAGIC        0x454E4C47u
#define LOG_EV_FMT          1u
#define LOG_EV_SIZE         LOG_META_SIZE

#define LOG_EV_OFF_MAGIC        0u
#define LOG_EV_OFF_FMT          4u
#define LOG_EV_OFF_BOOT_HW      8u
#define LOG_EV_OFF_RESERVED_HW  12u
#define LOG_EV_OFF_FLAGS        16u
#define LOG_EV_OFF_GEN          20u
#define LOG_EV_OFF_CORRUPT      24u
#define LOG_EV_OFF_CRC32        28u
#define LOG_EV_CRC_SPAN         28u

/** 隔离中：**拒绝分配任何新 seq**，直到人工清除（受控恢复出厂） */
#define LOG_EV_FLAG_QUARANTINE  0x0001u
/** 曾经完成过 provision（用于区分「真·首启」与「NVS 被重建」） */
#define LOG_EV_FLAG_PROVISIONED 0x0002u

/** `seq` 空间耗尽的哨兵（`align_reserve` 返回它表示无可安全区间） */
#define LOG_SEQ_EXHAUSTED       0xFFFFFFFFu

/** CRC32 —— 与项目既有实现逐字一致（单一来源仍在 `log_meta_slot.h`） */
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

/** 序列化一条证据记录（固定 32 B，手工清零以保持 freestanding） */
inline void pack(uint8_t out[LOG_EV_SIZE], uint32_t boot_hw, uint32_t reserved_hw,
                 uint32_t flags, uint32_t gen, uint32_t corrupt)
{
    for (uint32_t i = 0; i < LOG_EV_SIZE; i++)
    {
        out[i] = 0;
    }

    put_u32(&out[LOG_EV_OFF_MAGIC],       LOG_EV_MAGIC);
    put_u32(&out[LOG_EV_OFF_FMT],         LOG_EV_FMT);
    put_u32(&out[LOG_EV_OFF_BOOT_HW],     boot_hw);
    put_u32(&out[LOG_EV_OFF_RESERVED_HW], reserved_hw);
    put_u32(&out[LOG_EV_OFF_FLAGS],       flags);
    put_u32(&out[LOG_EV_OFF_GEN],         gen);
    put_u32(&out[LOG_EV_OFF_CORRUPT],     corrupt);
    put_u32(&out[LOG_EV_OFF_CRC32],       crc32(out, LOG_EV_CRC_SPAN));
}

/**
 * 解析并校验（magic / fmt / CRC 三项）。
 *
 * ★ **无 `commit` 字段**：与元数据槽不同，本记录是「单条 32 B 覆写 + NVS 原子键」，
 *   由 NVS 自身的键值原子性保证；CRC 用于识别截断 / 半写 / 位翻转。
 *   ⚠️ NVS 的**键级**原子性不得外推为「Flash 块级原子性」——见头部边界说明。
 *
 * @returns true = 有效；false = 无效（**不得**使用任何输出参数）
 */
inline bool parse(const uint8_t in[LOG_EV_SIZE], uint32_t &boot_hw, uint32_t &reserved_hw,
                  uint32_t &flags, uint32_t &gen, uint32_t &corrupt)
{
    if (get_u32(&in[LOG_EV_OFF_MAGIC]) != LOG_EV_MAGIC)
    {
        return false;
    }

    if (get_u32(&in[LOG_EV_OFF_FMT]) != LOG_EV_FMT)
    {
        return false;
    }

    if (get_u32(&in[LOG_EV_OFF_CRC32]) != crc32(in, LOG_EV_CRC_SPAN))
    {
        return false;
    }

    boot_hw     = get_u32(&in[LOG_EV_OFF_BOOT_HW]);
    reserved_hw = get_u32(&in[LOG_EV_OFF_RESERVED_HW]);
    flags       = get_u32(&in[LOG_EV_OFF_FLAGS]);
    gen         = get_u32(&in[LOG_EV_OFF_GEN]);
    corrupt     = get_u32(&in[LOG_EV_OFF_CORRUPT]);

    return true;
}

/**
 * 恢复来源判定（`HD-3`·`A-1` 修复后的**唯一**判据）。
 *
 * 优先级（高 → 低）：
 *   ① `quarantine`（NVS 标志 或 `/log/quarantine.bin`）⇒ 拒绝分配（跨重启固化）
 *   ② NVS 证据有效                     ⇒ 用它（**独立于 LittleFS**）
 *   ③ 双槽有效                         ⇒ 用它
 *   ④ legacy `meta.bin` 有效           ⇒ 用它（升级路径）
 *   ⑤ 段内存在 `boot_seq > 0`          ⇒ 用它（段内记录自带）
 *   ⑥ 无任何记录 + 无损坏痕迹 + **本次 boot 才建立身份** ⇒ 真·首启（`boot_seq=1`）
 *   ⑦ 其余（**含「无证据但身份早已存在」**）⇒ 隔离（`QUARANTINE`）
 *
 * ★ ⑥ 与 ⑦ 的分界是本设计**唯一**允许"从 1 开始"的场景，且要求
 *   **NVS 里的设备身份也是本次 boot 才写入的**（`pre_provisioned == false`）。
 *   只要身份早于本次 boot 存在，就证明"这台设备曾经运行过" ⇒ 不允许从 1 重放。
 *
 * ⚠️ 已知不可分辨情形（须记录，不得声称已覆盖）：
 *   整片擦除（NVS + LittleFS 同时丢失）⇒ `pre_provisioned == false` ⇒ 判 ⑥。
 *   该情形等价于"受控恢复出厂设置"，需服务侧 `boot_seq` 回退检测兜底
 *   （`HUMAN_REQUIRED`）。
 */
enum Recovery
{
    REC_FROM_NVS       = 0,
    REC_FROM_SLOT      = 1,
    REC_FROM_LEGACY    = 2,
    REC_FROM_SEGMENT   = 3,
    REC_PROVISION      = 4,
    REC_QUARANTINE     = 5
};

inline int8_t decide_recovery(bool quarantine,
                              bool have_nvs,
                              bool have_slot,
                              bool have_legacy,
                              uint32_t max_boot,
                              uint32_t total_records,
                              uint32_t scan_corrupt,
                              bool pre_provisioned)
{
    if (quarantine)
    {
        return REC_QUARANTINE;
    }

    if (have_nvs)
    {
        return REC_FROM_NVS;
    }

    if (have_slot)
    {
        return REC_FROM_SLOT;
    }

    if (have_legacy)
    {
        return REC_FROM_LEGACY;
    }

    if (max_boot > 0)
    {
        return REC_FROM_SEGMENT;
    }

    if (total_records == 0 && scan_corrupt == 0 && !pre_provisioned)
    {
        return REC_PROVISION;
    }

    return REC_QUARANTINE;
}

/** 该判据是否允许分配 `seq`（`QUARANTINE` 之外全部允许） */
inline bool recovery_allows_emit(int8_t recovery)
{
    return recovery != REC_QUARANTINE;
}

/** 取两个高水位的较大者（**只升不降**） */
inline uint32_t pick_hw(uint32_t a, uint32_t b)
{
    return (a > b) ? a : b;
}

/** 取四个高水位的较大者 */
inline uint32_t pick_hw4(uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
    return pick_hw(pick_hw(a, b), pick_hw(c, d));
}

/**
 * `s_seg_last_seq[]` 的值是否可作为高水位。
 *
 * `0xFFFFFFFF` 是 `flash_scan_all()` 的「末条未知」哨兵（读失败），
 * **不得**参与算术 —— 否则 `ceil` 会溢出回绕成 0（`A1-04` 的根因）。
 */
inline bool seg_last_usable(uint32_t last_seq)
{
    return last_seq != 0u && last_seq != 0xFFFFFFFFu;
}

/**
 * 把 `max_seq` 向上对齐到 `LOG_SEQ_RESERVE` 的整数倍（`reserved` 高水位语义）。
 *
 * @returns 对齐值；`max_seq` 已接近 `UINT32_MAX` ⇒ `LOG_SEQ_EXHAUSTED`（**不得**回绕）
 */
inline uint32_t align_reserve(uint32_t max_seq)
{
    if (max_seq == 0u)
    {
        return 0u;
    }

    /* 最后一个可安全对齐的 256 的倍数 = 0xFFFFFF00。
       超过它 ⇒ 无可用区间（返回哨兵，由调用方转入隔离，绝不回绕到 0）。 */
    if (max_seq >= 0xFFFFFF00u)
    {
        return LOG_SEQ_EXHAUSTED;
    }

    return ((max_seq + (LOG_SEQ_RESERVE - 1u)) / LOG_SEQ_RESERVE) * LOG_SEQ_RESERVE;
}

/**
 * `boot_seq` 饱和自增（`A1-07`）。
 *
 * @param ok 输出：false = 已无法再安全递增（调用方须转入隔离）
 * @returns 新值（饱和到 `0xFFFFFFFF`，**不回绕到 0**）
 */
inline uint32_t next_boot_seq(uint32_t boot_hw, bool &ok)
{
    if (boot_hw >= 0xFFFFFFFEu)
    {
        ok = false;
        return 0xFFFFFFFFu;
    }

    ok = true;
    return boot_hw + 1u;
}

// =============================================================================
// 损坏痕迹（`/log/corrupt.trace`）—— **跨重启的损坏证据**（`A1-02` 残留修复）
// =============================================================================
//
// 为什么需要它：
//   `flash_scan_all()` 发现损坏段时会 `LittleFS.remove()` 掉它 ⇒ 同一份"损坏
//   证据"在**下一次启动**就消失了。于是存在这条退化链：
//     ① B1：`scan_corrupt=2` ⇒ 判 `QUARANTINE`；
//     ② 若隔离标记**两份都写失败**（NVS 分区异常 + LittleFS 只读），隔离只剩 RAM；
//     ③ B2：损坏段已被删 ⇒ `scan_corrupt=0`、`total_records=0`
//        ⇒ 若身份也是本次才建立（NVS 被擦）⇒ 判 `PROVISION` ⇒ `boot_seq=1` ⇒ 键重用。
//
//   本记录把"曾经发生过段损坏"这一点**独立于段文件**固化下来：
//   `flash_scan_all()` 把 trace 的累计值与本次扫描值**取大**，因此
//   `scan_corrupt` 永远不会因为"坏段被删除"而回落到 0。
//
// 布局（32 B，与 meta 槽同尺寸）：
//   0   magic      0x44525443 ("CTRD")
//   4   fmt        1
//   8   count      累计损坏次数（**只增不减**）
//   12  last_seg   最近损坏的段号（`0xFFFFFFFF` = 非段来源，如"隔离无法固化"）
//   16  last_boot  记录时的 `boot_hw`（证据，可能为 0）
//   20  last_ms    记录时间 ms（诊断用；0 = 未知）
//   24  rsv        保留（0）
//   28  crc32      覆盖 [0..27]
//
// ⚠️ 边界：trace 与日志区**同生共死**（同一 LittleFS）。格式化后它也会消失 ——
//   那种情形由 NVS 证据兜底（`REC_FROM_NVS` 或隔离），不依赖本记录。

#define LOG_TRACE_MAGIC     0x44525443u
#define LOG_TRACE_FMT       1u
#define LOG_TRACE_OFF_MAGIC      0u
#define LOG_TRACE_OFF_FMT        4u
#define LOG_TRACE_OFF_COUNT      8u
#define LOG_TRACE_OFF_LAST_SEG   12u
#define LOG_TRACE_OFF_LAST_BOOT  16u
#define LOG_TRACE_OFF_LAST_MS    20u
#define LOG_TRACE_OFF_RSV        24u
#define LOG_TRACE_OFF_CRC32      28u
#define LOG_TRACE_CRC_SPAN       28u

/** "非段来源"的段号（如隔离标记无法持久化时记的痕迹） */
#define LOG_TRACE_SEG_NONE       0xFFFFFFFFu

/** 序列化一条损坏痕迹（固定 `LOG_EV_SIZE` = 32 B） */
inline void pack_trace(uint8_t out[LOG_EV_SIZE], uint32_t count, uint32_t last_seg,
                       uint32_t last_boot, uint32_t last_ms)
{
    for (uint32_t i = 0; i < LOG_EV_SIZE; i++)
    {
        out[i] = 0;
    }

    put_u32(&out[LOG_TRACE_OFF_MAGIC],     LOG_TRACE_MAGIC);
    put_u32(&out[LOG_TRACE_OFF_FMT],       LOG_TRACE_FMT);
    put_u32(&out[LOG_TRACE_OFF_COUNT],     count);
    put_u32(&out[LOG_TRACE_OFF_LAST_SEG],  last_seg);
    put_u32(&out[LOG_TRACE_OFF_LAST_BOOT], last_boot);
    put_u32(&out[LOG_TRACE_OFF_LAST_MS],   last_ms);
    put_u32(&out[LOG_TRACE_OFF_CRC32],     crc32(out, LOG_TRACE_CRC_SPAN));
}

/** 解析并校验（magic / fmt / CRC）；失败时**不得**使用任何输出参数 */
inline bool parse_trace(const uint8_t in[LOG_EV_SIZE], uint32_t &count, uint32_t &last_seg,
                        uint32_t &last_boot, uint32_t &last_ms)
{
    if (get_u32(&in[LOG_TRACE_OFF_MAGIC]) != LOG_TRACE_MAGIC)
    {
        return false;
    }

    if (get_u32(&in[LOG_TRACE_OFF_FMT]) != LOG_TRACE_FMT)
    {
        return false;
    }

    if (get_u32(&in[LOG_TRACE_OFF_CRC32]) != crc32(in, LOG_TRACE_CRC_SPAN))
    {
        return false;
    }

    count     = get_u32(&in[LOG_TRACE_OFF_COUNT]);
    last_seg  = get_u32(&in[LOG_TRACE_OFF_LAST_SEG]);
    last_boot = get_u32(&in[LOG_TRACE_OFF_LAST_BOOT]);
    last_ms    = get_u32(&in[LOG_TRACE_OFF_LAST_MS]);

    return true;
}

/**
 * 累计计数饱和加法（`A1-04` 同风格：**绝不回绕**）。
 *
 * @returns `prev + add`；溢出 ⇒ `0xFFFFFFFF`（饱和）
 */
inline uint32_t trace_add(uint32_t prev, uint32_t add)
{
    if (add > 0xFFFFFFFFu - prev)
    {
        return 0xFFFFFFFFu;
    }

    return prev + add;
}

/**
 * 合并"本次扫描计数"与"历史痕迹计数" ⇒ **取大**。
 *
 * ★ 语义是 `max` 而不是 `+`：避免同一批损坏在多轮扫描里被重复累加，
 *   同时保证"痕迹 > 0 ⇒ 合并值 > 0"（这正是阻止误判 `REC_PROVISION` 的关键）。
 */
inline uint32_t merge_corrupt(uint32_t scan_now, uint32_t trace_count)
{
    return (scan_now > trace_count) ? scan_now : trace_count;
}

}   // namespace log_meta_evidence

#endif  // LOG_META_EVIDENCE_H
