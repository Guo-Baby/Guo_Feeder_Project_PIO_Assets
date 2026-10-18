// =====================================================
// `HD-3` · `A-1` —— 双槽元数据**纯逻辑**的真实执行探针
//
// 被测代码 = `src/log/log_meta_slot.h`（**与 `log_manager.cpp` 跑的是同一份**）
//   · `pack()`   : 序列化 32 B 槽（含 commit 标记与 CRC）
//   · `parse()`  : 四项校验（magic / fmt / **commit** / CRC）
//   · `choose()` : `slot_seq` 大者胜；相等取 A；都无效 ⇒ -1
//
// 编译（无开发板、无 LittleFS）：
//   clang++ --target=wasm32 -std=gnu++11 -nostdlib -fno-builtin -I src \
//           test/log_contract/probe_meta_slot.cpp \
//           -Wl,--no-entry -Wl,--export-all -o .pio/probe_meta_slot.wasm
//   node test/log_contract/run_wasm_probe.js .pio/probe_meta_slot.wasm
//
// ★ 本探针**不能**证明的事（必须明确，不得越界表述）：
//   · LittleFS 的写入原子性、掉电时"两次写之间"的真实 Flash 行为
//   · `meta_commit_write()` 中**文件 I/O 的返回值处理**是否正确
//   · `boot_seq` 从日志段恢复（`flash_find_max_seq` 依赖 LittleFS 扫描）
//   · fail-closed 在 `log_emit_internal()` 中的实际短路（依赖 `s_ready` 等运行时状态）
//   ⇒ 上述四项属**代码审查 + 真机**范围。
//
// 覆盖项（与授权要求逐条对应）：
//   M1  往返保真（boot/reserved/corrupt/slot_seq 四字段）
//   M2  `commit=0` ⇒ 无效（**两阶段提交的安全支点**）
//   M3  CRC 被破坏 ⇒ 无效
//   M4  `slot_seq` 受 CRC 保护（改它 ⇒ CRC 失配）
//   M5  legacy 格式（fmt=2）**不得**被当成槽
//   M6  magic 错 / 全 0（Flash 擦除态）⇒ 无效
//   M7  选槽：A>B / B>A / 相等取 A / 仅 A / 仅 B / 都无效
//   M8  **掉电模拟**：旧槽 A 完整，新槽 B 只写了 commit=0 ⇒ 仍选 A（不丢旧状态）
//   M9  **双槽损坏** ⇒ -1（调用方必须回退 legacy/段，或 fail-closed）
//   M10 连续提交 3 次 ⇒ slot_seq 严格递增且选最新
//   M11 端点值（0 / UINT32_MAX）往返不溢出
// =====================================================

#include <stdint.h>

#include "log/log_meta_slot.h"

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

// ---- 极简日志（node 侧按 UTF-8 读回） ----

static char   g_log[8192];
static int    g_log_len = 0;
static int    g_fail = 0;

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
    using namespace log_meta_slot;

    uint8_t a[LOG_META_SIZE];
    uint8_t b[LOG_META_SIZE];

    /* ---------- M1 往返保真 ---------- */
    pack(a, 104u, 110848u, 3u, 7u, LOG_META_COMMIT_MARK);

    uint32_t boot = 0, reserved = 0, corrupt = 0, slot_seq = 0;

    const bool ok1 = parse(a, boot, reserved, corrupt, slot_seq);
    checkb(ok1, "M1a pack->parse 往返有效");
    checkv(boot, 104u, "M1b boot_seq 保真");
    checkv(reserved, 110848u, "M1c seq_reserved 保真");
    checkv(corrupt, 3u, "M1d corrupt 保真");
    checkv(slot_seq, 7u, "M1e slot_seq 保真");

    /* ---------- M2 commit=0 ⇒ 无效 ---------- */
    pack(b, 105u, 111104u, 3u, 8u, 0u);
    checkb(!parse(b, boot, reserved, corrupt, slot_seq),
           "M2 commit=0 (半写槽) ⇒ 无效（两阶段提交支点）");

    /* ---------- M3 CRC 被破坏 ⇒ 无效 ---------- */
    pack(b, 105u, 111104u, 3u, 8u, LOG_META_COMMIT_MARK);
    b[LOG_META_OFF_BOOT_SEQ] ^= 0xFFu;
    checkb(!parse(b, boot, reserved, corrupt, slot_seq), "M3 boot_seq 被篡改 ⇒ CRC 失配 ⇒ 无效");

    /* ---------- M4 slot_seq 受 CRC 保护 ---------- */
    pack(b, 105u, 111104u, 3u, 8u, LOG_META_COMMIT_MARK);
    b[LOG_META_OFF_SLOT_SEQ] ^= 0x01u;
    checkb(!parse(b, boot, reserved, corrupt, slot_seq), "M4 slot_seq 被篡改 ⇒ CRC 失配（选槽依据受保护）");

    /* ---------- M5 legacy fmt=2 不得被当成槽 ---------- */
    pack(b, 105u, 111104u, 3u, 8u, LOG_META_COMMIT_MARK);
    put_u32(&b[LOG_META_OFF_FMT], 2u);                 // legacy 版本号
    put_u32(&b[LOG_META_OFF_CRC32], crc32(b, LOG_META_CRC_SPAN));   // CRC 重新自洽
    checkb(!parse(b, boot, reserved, corrupt, slot_seq),
           "M5 legacy fmt=2（CRC 自洽）仍判无效（版本隔离）");

    /* ---------- M6 magic 错 / 全 0 ---------- */
    pack(b, 105u, 111104u, 3u, 8u, LOG_META_COMMIT_MARK);
    put_u32(&b[LOG_META_OFF_MAGIC], 0xDEADBEEFu);
    put_u32(&b[LOG_META_OFF_CRC32], crc32(b, LOG_META_CRC_SPAN));
    checkb(!parse(b, boot, reserved, corrupt, slot_seq), "M6a magic 错（CRC 自洽）⇒ 无效");

    memset(b, 0xFF, LOG_META_SIZE);                    // Flash 擦除态
    checkb(!parse(b, boot, reserved, corrupt, slot_seq), "M6b 全 0xFF（擦除态）⇒ 无效");

    memset(b, 0x00, LOG_META_SIZE);
    checkb(!parse(b, boot, reserved, corrupt, slot_seq), "M6c 全 0x00 ⇒ 无效");

    /* ---------- M7 选槽 ---------- */
    checkv((uint32_t)choose(true, 9u, true, 8u), 0u, "M7a A>B ⇒ 选 A");
    checkv((uint32_t)choose(true, 8u, true, 9u), 1u, "M7b B>A ⇒ 选 B");
    checkv((uint32_t)choose(true, 8u, true, 8u), 0u, "M7c 相等 ⇒ 取 A（确定性）");
    checkv((uint32_t)choose(true, 1u, false, 0u), 0u, "M7d 仅 A 有效 ⇒ 选 A");
    checkv((uint32_t)choose(false, 0u, true, 1u), 1u, "M7e 仅 B 有效 ⇒ 选 B");
    checkv((uint32_t)choose(false, 0u, false, 0u), 0xFFFFFFFFu, "M7f 都无效 ⇒ -1");

    /* ---------- M8 掉电模拟：旧槽完整 + 新槽只写了 commit=0 ---------- */
    {
        uint8_t oldA[LOG_META_SIZE];
        uint8_t halfB[LOG_META_SIZE];

        pack(oldA, 104u, 110848u, 3u, 7u, LOG_META_COMMIT_MARK);   // 旧：已提交
        pack(halfB, 105u, 111104u, 3u, 8u, 0u);                    // 新：只到"失效标记"

        uint32_t bb = 0, br = 0, bc = 0, bq = 0;
        const bool okA = parse(oldA, bb, br, bc, bq);
        const bool okB = parse(halfB, bb, br, bc, bq);

        checkb(okA && !okB, "M8a 旧槽有效 / 半写槽无效");
        checkv((uint32_t)choose(okA, 7u, okB, 8u), 0u, "M8b 掉电在两次写之间 ⇒ 仍选旧槽（旧状态不丢）");
    }

    /* ---------- M9 双槽损坏 ⇒ -1 ---------- */
    {
        memset(a, 0xFF, LOG_META_SIZE);
        memset(b, 0x00, LOG_META_SIZE);

        uint32_t x1 = 0, x2 = 0, x3 = 0, x4 = 0;
        const bool okA = parse(a, x1, x2, x3, x4);
        const bool okB = parse(b, x1, x2, x3, x4);

        checkb(!okA && !okB, "M9a 双槽均无效");
        checkv((uint32_t)choose(okA, 0u, okB, 0u), 0xFFFFFFFFu,
               "M9b 双槽损坏 ⇒ -1（调用方须回退 legacy/段 或 fail-closed）");
    }

    /* ---------- M10 连续提交 3 次：slot_seq 严格递增 ---------- */
    {
        uint32_t last_seq = 0;
        uint32_t max_seq = 0;
        const char *names[3] = { "M10a 第 1 次提交 slot_seq=1",
                                 "M10b 第 2 次提交 slot_seq=2",
                                 "M10c 第 3 次提交 slot_seq=3" };

        for (uint32_t i = 1; i <= 3u; i++)
        {
            pack(a, 104u + i, 110848u + i * 256u, 3u, i, LOG_META_COMMIT_MARK);

            uint32_t qb = 0, qr = 0, qc = 0, qq = 0;
            const bool ok = parse(a, qb, qr, qc, qq);

            checkb(ok && qq > last_seq, names[i - 1]);
            last_seq = qq;

            if (qq > max_seq)
            {
                max_seq = qq;
            }
        }

        checkv(max_seq, 3u, "M10d 最新槽 slot_seq=3（选槽将取它）");
    }

    /* ---------- M11 端点值 ---------- */
    {
        pack(a, 0u, 0u, 0u, 0u, LOG_META_COMMIT_MARK);

        uint32_t eb = 1, er = 1, ec = 1, eq = 1;
        const bool ok = parse(a, eb, er, ec, eq);

        checkb(ok, "M11a 全 0 载荷仍可解析（0 是合法值，不等于无效）");
        checkv(eb, 0u, "M11b boot_seq=0 往返");
        checkv(eq, 0u, "M11c slot_seq=0 往返");

        pack(a, 0xFFFFFFFFu, 0xFFFFFF00u, 0xFFFFFFFFu, 0xFFFFFFFEu, LOG_META_COMMIT_MARK);
        const bool ok2 = parse(a, eb, er, ec, eq);

        checkb(ok2, "M11d UINT32_MAX 端点可解析");
        checkv(eb, 0xFFFFFFFFu, "M11e boot_seq=UINT32_MAX 往返不溢出");
        checkv(er, 0xFFFFFF00u, "M11f seq_reserved 大值往返");
    }

    /* ---------- M12 已迁出 ----------
       `decide_boot_source` 在 `HD-3`·`A-1` 修复中迁至
       `log/log_meta_evidence.h::decide_recovery()`（新增 NVS 证据线与
       跨重启隔离），其 7 条用例由 `probe_meta_evidence.cpp` 的 `E5` 组
       逐条承接并扩展 —— **未删减覆盖，只迁移位置**。 */

    pstr("== probe_meta_slot done, failures=");
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
