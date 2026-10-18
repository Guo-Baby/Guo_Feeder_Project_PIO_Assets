// =====================================================
// `HD-3` · `A-1` 修复 —— **持久证据 / 隔离判据**的真实执行探针
//
// 被测代码 = `src/log/log_meta_evidence.h`（与 `log_manager.cpp` 同一份）
//   · `pack()` / `parse()`            : 32 B 证据记录（NVS 侧）
//   · `decide_recovery()`             : 唯一的恢复来源判据
//   · `align_reserve()`               : 高水位对齐（**A1-04 溢出回归**）
//   · `next_boot_seq()`               : `boot_seq` 饱和自增（**A1-07 回绕回归**）
//   · `seg_last_usable()`             : `0xFFFFFFFF` 哨兵过滤（A1-04 根因）
//
// ★ 本探针**不能**证明的事（不得越界表述）：
//   · NVS 键级原子性、断电时 NVS 的实际行为
//   · 证据写入失败后的降级路径（依赖真实 NVS）
//   · 「整片擦除」是否真的与全新设备不可分辨（这是**边界定义**，不是可测命题）
//   ⇒ 上述属**代码审查 + 真机**范围。
//
// 覆盖项（与 `A1-01`/`A1-02`/`A1-04`/`A1-07` 逐条对应）：
//   E1  证据记录往返保真（boot/reserved/flags/gen/corrupt）
//   E2  magic / fmt / CRC 任一被破坏 ⇒ 无效
//   E3  全 0 / 全 0xFF（擦除态）⇒ 无效
//   E4  flags 位语义（QUARANTINE / PROVISIONED 互不干扰）
//   E5  恢复判据优先级（含 M12 迁移来的 7 条）+ **新的** A1-01/A1-02 语义
//   E6  高水位取大（只升不降）
//   E7  `align_reserve` 溢出回归（旧实现在 0xFFFFFFFF 上会回绕成 0）
//   E8  `boot_seq` 饱和自增（不回绕到 0）
//   E9  `seg_last_usable` 哨兵过滤
//   E10 跨重启状态机（B1–B6，覆盖 A1-01 / A1-02 的真实链路）
//   E11 **损坏痕迹**（`A1-02` 残留修复）：往返 / CRC / 饱和 / 合并取大 /
//        「段被删后仍判 QUARANTINE」的跨重启断言
// =====================================================

#include <stdint.h>

#include "log/log_meta_evidence.h"

// ---- nostdlib 下最小运行时 ----

extern "C" void *memcpy(void *dst, const void *src, unsigned long n)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;

    for (unsigned long i = 0; i < n; i++)
    {
        d[i] = s[i];
    }

    return dst;
}

extern "C" void *memset(void *dst, int c, unsigned long n)
{
    uint8_t *d = (uint8_t *)dst;

    for (unsigned long i = 0; i < n; i++)
    {
        d[i] = (uint8_t)c;
    }

    return dst;
}

// ---- 极简输出 ----

static char g_log[8192];
static int  g_log_len = 0;
static int  g_fail = 0;

static void pch(char c)
{
    if (g_log_len < (int)sizeof(g_log) - 1)
    {
        g_log[g_log_len++] = c;
        g_log[g_log_len] = '\0';
    }
}

static void pstr(const char *s)
{
    while (*s)
    {
        pch(*s++);
    }
}

static void pdec(uint32_t v)
{
    char buf[12];
    int i = 0;

    if (v == 0)
    {
        pch('0');
        return;
    }

    while (v > 0 && i < 11)
    {
        buf[i++] = (char)('0' + (v % 10u));
        v /= 10u;
    }

    while (i > 0)
    {
        pch(buf[--i]);
    }
}

static void checkb(bool cond, const char *name)
{
    if (cond)
    {
        pstr("  PASS  ");
    }
    else
    {
        pstr("  FAIL  ");
        g_fail++;
    }

    pstr(name);
    pch('\n');
}

static void checkv(uint32_t got, uint32_t want, const char *name)
{
    if (got == want)
    {
        pstr("  PASS  ");
        pstr(name);
        pch('\n');
    }
    else
    {
        pstr("  FAIL  ");
        pstr(name);
        pstr(" (got=");
        pdec(got);
        pstr(" want=");
        pdec(want);
        pstr(")\n");
        g_fail++;
    }
}

extern "C" int probe_run()
{
    using namespace log_meta_evidence;

    uint8_t a[LOG_EV_SIZE];
    uint8_t b[LOG_EV_SIZE];

    /* ---------- E1 往返保真 ---------- */
    pack(a, 104u, 110848u, LOG_EV_FLAG_PROVISIONED, 7u, 3u);

    uint32_t boot = 0, res = 0, flags = 0, gen = 0, corrupt = 0;

    checkb(parse(a, boot, res, flags, gen, corrupt), "E1a pack->parse 有效");
    checkv(boot, 104u, "E1b boot_hw 保真");
    checkv(res, 110848u, "E1c reserved_hw 保真");
    checkv(flags, LOG_EV_FLAG_PROVISIONED, "E1d flags 保真");
    checkv(gen, 7u, "E1e gen 保真");
    checkv(corrupt, 3u, "E1f corrupt 保真");

    /* ---------- E2 破坏任一项 ⇒ 无效 ---------- */
    pack(b, 104u, 110848u, 0u, 7u, 3u);
    b[LOG_EV_OFF_BOOT_HW] ^= 0xFFu;
    checkb(!parse(b, boot, res, flags, gen, corrupt), "E2a boot_hw 篡改 ⇒ CRC 失配");

    pack(b, 104u, 110848u, 0u, 7u, 3u);
    put_u32(&b[LOG_EV_OFF_MAGIC], 0xDEADBEEFu);
    put_u32(&b[LOG_EV_OFF_CRC32], crc32(b, LOG_EV_CRC_SPAN));
    checkb(!parse(b, boot, res, flags, gen, corrupt), "E2b magic 错（CRC 自洽）⇒ 无效");

    pack(b, 104u, 110848u, 0u, 7u, 3u);
    put_u32(&b[LOG_EV_OFF_FMT], 2u);
    put_u32(&b[LOG_EV_OFF_CRC32], crc32(b, LOG_EV_CRC_SPAN));
    checkb(!parse(b, boot, res, flags, gen, corrupt), "E2c fmt 非 1（CRC 自洽）⇒ 无效");

    pack(b, 104u, 110848u, 0u, 7u, 3u);
    b[LOG_EV_OFF_CRC32] ^= 0x01u;
    checkb(!parse(b, boot, res, flags, gen, corrupt), "E2d CRC 位翻转 ⇒ 无效");

    /* ---------- E3 擦除态 ---------- */
    memset(b, 0xFF, LOG_EV_SIZE);
    checkb(!parse(b, boot, res, flags, gen, corrupt), "E3a 全 0xFF（擦除态）⇒ 无效");

    memset(b, 0x00, LOG_EV_SIZE);
    checkb(!parse(b, boot, res, flags, gen, corrupt), "E3b 全 0x00 ⇒ 无效");

    /* ---------- E4 flags 位语义 ---------- */
    pack(a, 5u, 256u, LOG_EV_FLAG_QUARANTINE | LOG_EV_FLAG_PROVISIONED, 1u, 0u);
    checkb(parse(a, boot, res, flags, gen, corrupt), "E4a 双标志并存可解析");
    checkb((flags & LOG_EV_FLAG_QUARANTINE) != 0u, "E4b QUARANTINE 位可读");
    checkb((flags & LOG_EV_FLAG_PROVISIONED) != 0u, "E4c PROVISIONED 位可读");

    /* ---------- E5 恢复判据（含 M12 迁移 + A1-01/A1-02 新语义）---------- */

    // ① 隔离优先于一切（含 NVS/槽/段全有）
    checkv((uint32_t)decide_recovery(true, true, true, true, 104u, 40u, 3u, true),
           (uint32_t)REC_QUARANTINE, "E5a 隔离标记存在 ⇒ 压过 NVS/槽/段（跨重启固化）");

    // ② NVS 证据优先于槽（格式化后槽全无，NVS 仍在）
    checkv((uint32_t)decide_recovery(false, true, false, false, 0u, 0u, 0u, true),
           (uint32_t)REC_FROM_NVS, "E5b 仅 NVS 有证据 ⇒ 用它（**格式化后仍不判全新**）");

    // ③ 槽 / legacy / 段 / 全无
    checkv((uint32_t)decide_recovery(false, false, true, true, 104u, 40u, 3u, true),
           (uint32_t)REC_FROM_SLOT, "E5c 槽与 legacy 并存 ⇒ 槽优先（M12g 迁移）");
    checkv((uint32_t)decide_recovery(false, false, false, true, 0u, 0u, 0u, true),
           (uint32_t)REC_FROM_LEGACY, "E5d 仅 legacy ⇒ legacy（升级路径，M12b 迁移）");
    checkv((uint32_t)decide_recovery(false, false, false, false, 104u, 40u, 0u, true),
           (uint32_t)REC_FROM_SEGMENT, "E5e 段内 max_boot=104 ⇒ 段恢复（M12c 迁移）");

    // ④ 「真·首启」与「无证据但身份已存在」**必须分开**（A1-01/A1-02 的核心）
    checkv((uint32_t)decide_recovery(false, false, false, false, 0u, 0u, 0u, false),
           (uint32_t)REC_PROVISION, "E5f 无证据 + **本次才建立身份** ⇒ 真·首启（允许 =1）");
    checkv((uint32_t)decide_recovery(false, false, false, false, 0u, 0u, 0u, true),
           (uint32_t)REC_QUARANTINE, "E5g 无证据 + 身份早已存在 ⇒ **隔离**（不得当全新设备）");

    // ⑤ 旧 M12e / M12f：有记录或有损坏痕迹 ⇒ 隔离
    checkv((uint32_t)decide_recovery(false, false, false, false, 0u, 5u, 0u, false),
           (uint32_t)REC_QUARANTINE, "E5h 有记录但无 boot_seq ⇒ 隔离（M12e 迁移）");
    checkv((uint32_t)decide_recovery(false, false, false, false, 0u, 0u, 1u, false),
           (uint32_t)REC_QUARANTINE, "E5i 有损坏痕迹但无 boot_seq ⇒ 隔离（M12f 迁移）");

    // ⑥ 允许分配与否
    checkb(recovery_allows_emit(REC_FROM_NVS), "E5j NVS 来源允许分配");
    checkb(recovery_allows_emit(REC_PROVISION), "E5k 首启允许分配");
    checkb(!recovery_allows_emit(REC_QUARANTINE), "E5l 隔离禁止分配");

    /* ---------- E6 高水位只升不降 ---------- */
    checkv(pick_hw(5u, 9u), 9u, "E6a pick_hw 取大");
    checkv(pick_hw(9u, 5u), 9u, "E6b pick_hw 顺序无关");
    checkv(pick_hw4(1u, 7u, 3u, 5u), 7u, "E6c pick_hw4 取四者最大");
    checkv(pick_hw4(0u, 0u, 0u, 0u), 0u, "E6d 全 0 ⇒ 0");

    /* ---------- E7 align_reserve 溢出回归（A1-04） ---------- */
    checkv(align_reserve(0u), 0u, "E7a 0 ⇒ 0");
    checkv(align_reserve(1u), 256u, "E7b 1 ⇒ 256（向上对齐）");
    checkv(align_reserve(256u), 256u, "E7c 256 ⇒ 256（已对齐不变）");
    checkv(align_reserve(257u), 512u, "E7d 257 ⇒ 512");
    checkv(align_reserve(110848u), 110848u, "E7e 110848 ⇒ 110848");
    checkv(align_reserve(0xFFFFFF00u), (uint32_t)LOG_SEQ_EXHAUSTED,
           "E7f 0xFFFFFF00 ⇒ 耗尽哨兵（**不回绕成 0**）");
    checkv(align_reserve(0xFFFFFFFFu), (uint32_t)LOG_SEQ_EXHAUSTED,
           "E7g 0xFFFFFFFF ⇒ 耗尽哨兵（旧实现会回绕成 0 ⇒ reserved 骤降）");

    /* ---------- E8 boot_seq 饱和自增（A1-07） ---------- */
    {
        bool ok = false;

        checkv(next_boot_seq(104u, ok), 105u, "E8a 104 ⇒ 105");
        checkb(ok, "E8b 104 递增成功");

        checkv(next_boot_seq(0u, ok), 1u, "E8c 0 ⇒ 1");
        checkb(ok, "E8d 0 递增成功");

        checkv(next_boot_seq(0xFFFFFFFEu, ok), 0xFFFFFFFFu, "E8e 0xFFFFFFFE ⇒ 饱和到 MAX");
        checkb(!ok, "E8f 0xFFFFFFFE 标记为不可继续");

        checkv(next_boot_seq(0xFFFFFFFFu, ok), 0xFFFFFFFFu, "E8g MAX ⇒ 保持 MAX（**不回绕到 0**）");
        checkb(!ok, "E8h MAX 标记为不可继续");
    }

    /* ---------- E9 段末 seq 哨兵过滤（A1-04 根因） ---------- */
    checkb(!seg_last_usable(0xFFFFFFFFu), "E9a 0xFFFFFFFF（末条未知）⇒ 不可用");
    checkb(!seg_last_usable(0u), "E9b 0（空段）⇒ 不可用");
    checkb(seg_last_usable(1u), "E9c 1 ⇒ 可用");
    checkb(seg_last_usable(0xFFFFFFFEu), "E9d 0xFFFFFFFE ⇒ 可用（真实的极大 seq）");

    /* ---------- E10 跨重启状态机（**多轮**，覆盖 A1-01 / A1-02 的真实链路） ----------
     *
     * 用内存中的 `evbuf` 模拟 NVS 记录（**不是** NVS 实现，只验证判据与推进规则）。
     * 每轮 = 一次开机：读证据 → 判定来源 → 算 boot/reserved → 回写证据。
     *
     * 场景链（★ 标出修复点）：
     *   B1 全新设备（本次才建立身份）      ⇒ PROVISION, boot=1
     *   B2 正常重启                        ⇒ NVS,       boot=2
     *   B3 **LittleFS 被格式化**（槽/段全无，NVS 仍在）⇒ NVS, boot=3  ★不再是 1
     *   B4 NVS 证据也丢了 + 身份早已存在    ⇒ QUARANTINE                ★不再当全新
     *   B5 隔离**跨重启保持**（只在 RAM 时这里会是 1）⇒ QUARANTINE      ★A1-02
     *   B6 人工解除隔离（保留 boot_hw）     ⇒ NVS,       boot=4
     */
    {
        uint8_t  evbuf[LOG_EV_SIZE];
        bool     ev_ok = false;
        uint32_t evb = 0, evr = 0, evf = 0, evg = 0, evc = 0;

        const uint32_t total_recs = 0;     // 模拟"日志区已被格式化"
        const uint32_t scan_corrupt = 0;   // 且没有损坏痕迹（旧实现据此判 FRESH）

        /* ---- B1：全新设备 ---- */
        {
            const int8_t src = decide_recovery(false, ev_ok, false, false, 0u,
                                               total_recs, scan_corrupt, false);
            checkv((uint32_t)src, (uint32_t)REC_PROVISION, "E10a B1 全新设备 ⇒ PROVISION");

            bool ok = false;
            const uint32_t b = next_boot_seq(0u, ok);   // 首启 ⇒ boot=1
            checkv(b, 1u, "E10b B1 boot_seq=1");

            pack(evbuf, b, 0u, LOG_EV_FLAG_PROVISIONED, 1u, 0u);
            ev_ok = parse(evbuf, evb, evr, evf, evg, evc);
            checkb(ev_ok, "E10c B1 证据已固化（PROVISIONED）");
        }

        /* ---- B2：正常重启 ---- */
        {
            const int8_t src = decide_recovery(false, ev_ok, false, false, 0u,
                                               total_recs, scan_corrupt, true);
            checkv((uint32_t)src, (uint32_t)REC_FROM_NVS, "E10d B2 正常重启 ⇒ NVS");

            bool ok = false;
            const uint32_t b = next_boot_seq(evb, ok);
            checkv(b, 2u, "E10e B2 boot_seq=2");

            pack(evbuf, b, 256u, LOG_EV_FLAG_PROVISIONED, 2u, 0u);
            ev_ok = parse(evbuf, evb, evr, evf, evg, evc);
        }

        /* ---- B3：LittleFS 被格式化（双槽 / legacy / 段全部消失）---- */
        {
            const int8_t src = decide_recovery(false, ev_ok, false, false, 0u,
                                               total_recs, scan_corrupt, true);
            checkv((uint32_t)src, (uint32_t)REC_FROM_NVS, "E10f B3 格式化后 ⇒ 仍走 NVS");

            bool ok = false;
            const uint32_t b = next_boot_seq(evb, ok);
            checkv(b, 3u, "E10g B3 boot_seq=3（**不是** 1 ⇒ A1-01 已修）");

            pack(evbuf, b, 512u, LOG_EV_FLAG_PROVISIONED, 3u, 0u);
            ev_ok = parse(evbuf, evb, evr, evf, evg, evc);
        }

        /* ---- B4：NVS 证据也丢失（但身份早已存在）---- */
        {
            ev_ok = false;   // 模拟 NVS 记录丢失

            const int8_t src = decide_recovery(false, ev_ok, false, false, 0u,
                                               total_recs, scan_corrupt, true);
            checkv((uint32_t)src, (uint32_t)REC_QUARANTINE,
                   "E10h B4 证据全无 + 身份已存在 ⇒ QUARANTINE（**不当全新**）");
            checkb(!recovery_allows_emit(src), "E10i B4 隔离期间禁止分配 seq");
        }

        /* ---- B5：隔离跨重启保持（隔离已固化进证据）---- */
        {
            pack(evbuf, evb, evr,
                 LOG_EV_FLAG_PROVISIONED | LOG_EV_FLAG_QUARANTINE, 4u, 0u);

            uint32_t b2 = 0, r2 = 0, f2 = 0, g2 = 0, c2 = 0;
            ev_ok = parse(evbuf, b2, r2, f2, g2, c2);

            const int8_t src = decide_recovery((f2 & LOG_EV_FLAG_QUARANTINE) != 0u, ev_ok,
                                               true, true, 104u, 40u, 0u, true);
            checkv((uint32_t)src, (uint32_t)REC_QUARANTINE,
                   "E10j B5 隔离跨重启保持（即使双槽/段都有效 ⇒ A1-02 已修）");
            checkb(!recovery_allows_emit(src), "E10k B5 仍禁止分配");
        }

        /* ---- B6：人工解除隔离（保留 boot_hw ⇒ 从 3 续到 4，不回 1）---- */
        {
            const uint32_t cleared = (evf & ~(uint32_t)LOG_EV_FLAG_QUARANTINE)
                                     | LOG_EV_FLAG_PROVISIONED;
            pack(evbuf, evb, evr, cleared, 5u, 0u);

            uint32_t b3 = 0, r3 = 0, f3 = 0, g3 = 0, c3 = 0;
            ev_ok = parse(evbuf, b3, r3, f3, g3, c3);

            const int8_t src = decide_recovery((f3 & LOG_EV_FLAG_QUARANTINE) != 0u, ev_ok,
                                               false, false, 0u, 0u, 0u, true);
            checkv((uint32_t)src, (uint32_t)REC_FROM_NVS, "E10l B6 解除隔离后 ⇒ 恢复正常");

            bool ok = false;
            const uint32_t b = next_boot_seq(b3, ok);
            checkv(b, 4u, "E10m B6 从记忆的 boot_hw 续推进（**不回 1**）");
        }
    }

    /* ---------- E11 损坏痕迹（`A1-02` 残留修复 · 跨重启"损坏证据"） ----------
     *
     * 背景：`flash_drop_broken_segment()` 会删掉损坏段 ⇒ 下一次启动
     * `scan_corrupt` 归 0、`total_records` 归 0 ⇒ 可能被判 `PROVISION`。
     * 本组验证：痕迹记录本身的往返/校验/饱和，以及"合并后绝不判 PROVISION"。
     */
    {
        uint8_t tb[LOG_EV_SIZE];
        uint32_t c = 0, sg = 0, bt = 0, ms = 0;

        pack_trace(tb, 3u, 7u, 12u, 123456u);
        checkb(parse_trace(tb, c, sg, bt, ms), "E11a 痕迹 pack/parse 往返有效");
        checkv(c,  3u,     "E11b count 保真");
        checkv(sg, 7u,     "E11c last_seg 保真");
        checkv(bt, 12u,    "E11d last_boot 保真");
        checkv(ms, 123456u, "E11e last_ms 保真");

        pack_trace(tb, 3u, 7u, 12u, 1u);
        tb[LOG_TRACE_OFF_CRC32] ^= 0xFFu;
        checkb(!parse_trace(tb, c, sg, bt, ms), "E11f CRC 破坏 ⇒ 无效（半写记录不可用）");

        pack_trace(tb, 3u, 7u, 12u, 1u);
        tb[LOG_TRACE_OFF_MAGIC] ^= 0xFFu;
        checkb(!parse_trace(tb, c, sg, bt, ms), "E11g magic 破坏 ⇒ 无效");

        checkv(trace_add(0xFFFFFFFEu, 1u), 0xFFFFFFFFu, "E11h 饱和到 MAX（不回绕）");
        checkv(trace_add(0xFFFFFFFFu, 5u), 0xFFFFFFFFu, "E11i 已满 ⇒ 保持 MAX");
        checkv(trace_add(2u, 3u), 5u, "E11j 正常累加");

        checkv(merge_corrupt(0u, 2u), 2u, "E11k 段已删(scan=0) + 痕迹=2 ⇒ 2");
        checkv(merge_corrupt(5u, 2u), 5u, "E11l 本次更多 ⇒ 取本次");
        checkv(merge_corrupt(0u, 0u), 0u, "E11m 无痕迹 ⇒ 0（真首启不受影响）");

        /* ★ 跨重启核心断言 */
        const uint32_t b1_trace    = trace_add(0u, 2u);   // B1 发现 2 个损坏段
        const uint32_t b2_scan_now = 0u;                  // B2：损坏段已被 remove
        const uint32_t b2_merged   = merge_corrupt(b2_scan_now, b1_trace);

        checkv(b2_merged, 2u, "E11n B2 合并后仍为 2（损坏证据不自毁）");
        checkv((uint32_t)decide_recovery(false, false, false, false, 0u, 0u, b2_merged, true),
               (uint32_t)REC_QUARANTINE,
               "E11o B2 scan=0 但痕迹=2 ⇒ QUARANTINE（旧实现会误判 PROVISION）");

        checkv((uint32_t)decide_recovery(false, false, false, false, 0u, 0u, 0u, false),
               (uint32_t)REC_PROVISION, "E11p 无痕迹 + 身份本次建立 ⇒ PROVISION（对照）");

        /* ★ 隔离双标记全失败 ⇒ bump(SEG_NONE) ⇒ 下次重启仍隔离 */
        const uint32_t q_trace = trace_add(0u, 1u);

        checkv(q_trace, 1u, "E11q 隔离标记无法固化 ⇒ 痕迹 +1");
        checkv((uint32_t)decide_recovery(false, false, false, false, 0u, 0u,
                                         merge_corrupt(0u, q_trace), true),
               (uint32_t)REC_QUARANTINE,
               "E11r 痕迹=1 ⇒ 下次重启仍 QUARANTINE（A1-02 残留闭环）");

        checkb(LOG_TRACE_SEG_NONE == 0xFFFFFFFFu, "E11s SEG_NONE 哨兵 = 0xFFFFFFFF");
        checkb(LOG_TRACE_SEG_NONE >= LOG_SEGMENT_COUNT, "E11t 哨兵不落在合法段号区间");
    }

    pstr("== probe_meta_evidence done, failures=");
    pdec((uint32_t)g_fail);
    pch('\n');

    return g_fail;
}

// ---- 供 node 侧读取的导出 ----

extern "C" const char *probe_log_ptr()
{
    return g_log;
}

extern "C" int probe_log_len()
{
    return g_log_len;
}
