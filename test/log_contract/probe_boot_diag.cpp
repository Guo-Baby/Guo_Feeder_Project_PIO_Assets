// =====================================================
// `HD-3` · `A1-03` —— 启动诊断参数（PID / 类型 / 上限）真实执行探针
//
// 被测代码 = `src/log/log_events.h`（与固件同一份，纯 constexpr/enum）
//
// ★ 本探针**能**证明：
//   · 4 个新 PID 的取值、互不冲突、且不与既有区间重叠；
//   · `LOG_P_MAX` 已同步抬升（否则越界 PID 会被契约拒绝）；
//   · 参数数量上限容得下 5 个参数；
//   · `LogParamIn` 的尺寸 ⇒ 5 个参数在栈上约多少字节；
//   · 类型映射（本格式**没有 U8**，故用 ENUM/BOOL 表达 u8 语义）；
//   · `boot_seq` 属**记录头部**字段，不需要额外 PID。
//
// ★ 本探针**不能**证明（不得越界表述）：
//   · "旧解码器会跳过未知 PID" —— **仓库内没有任何 TLV 解码器**
//     （`probe_envelope.cpp` 只置 `param_count = 0`，不解析参数）；
//   · 云端可见性 —— 云端把 `payload_base64` 当**不透明数据**，
//     且需要部署 + 真机启动才成立；
//   ⇒ 兼容性结论只能表述为"**当前仓库的云端路径不解析 payload**，
//     消费方契约要求跳过未知 PID"（见 `docs/interfaces/log_params_contract.md`）。
//
// 覆盖项：D1–D16（见下方 checkv/checkb 名称）
// =====================================================

#include <stdint.h>

#include "log/log_events.h"

// ---- nostdlib 下最小运行时 ----

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
    /* ---------- D1–D5：4 个新 PID 的取值 ---------- */
    checkv((uint32_t)LOG_P_RESERVED_HW,   0x5Au, "D1 LOG_P_RESERVED_HW = 0x5A");
    checkv((uint32_t)LOG_P_RECOVERY_SRC,  0x5Bu, "D2 LOG_P_RECOVERY_SRC = 0x5B");
    checkv((uint32_t)LOG_P_FAILDCLOSED,   0x5Cu, "D3 LOG_P_FAILDCLOSED = 0x5C");
    checkv((uint32_t)LOG_P_TRACE_CORRUPT, 0x5Du, "D4 LOG_P_TRACE_CORRUPT = 0x5D");
    checkv((uint32_t)LOG_P_MAX,           0x5Du, "D5 LOG_P_MAX 已抬到 0x5D");

    /* ---------- D6：全部 > 旧上限 0x59（不与既有 90 个 PID 重叠） ---------- */
    checkb((uint32_t)LOG_P_RESERVED_HW > 0x59u
           && (uint32_t)LOG_P_RECOVERY_SRC > 0x59u
           && (uint32_t)LOG_P_FAILDCLOSED > 0x59u
           && (uint32_t)LOG_P_TRACE_CORRUPT > 0x59u,
           "D6 4 个新 PID 均 > 旧上限 0x59");

    /* ---------- D7：两两不等 ---------- */
    checkb(LOG_P_RESERVED_HW != LOG_P_RECOVERY_SRC
           && LOG_P_RESERVED_HW != LOG_P_FAILDCLOSED
           && LOG_P_RESERVED_HW != LOG_P_TRACE_CORRUPT
           && LOG_P_RECOVERY_SRC != LOG_P_FAILDCLOSED
           && LOG_P_RECOVERY_SRC != LOG_P_TRACE_CORRUPT
           && LOG_P_FAILDCLOSED != LOG_P_TRACE_CORRUPT,
           "D7 4 个新 PID 两两不相等");

    /* ---------- D8：不与既有 BOOT_SEQ 复用 ---------- */
    checkb((uint32_t)LOG_P_BOOT_SEQ == 0x21u
           && LOG_P_BOOT_SEQ != LOG_P_RESERVED_HW
           && LOG_P_BOOT_SEQ != LOG_P_RECOVERY_SRC
           && LOG_P_BOOT_SEQ != LOG_P_FAILDCLOSED
           && LOG_P_BOOT_SEQ != LOG_P_TRACE_CORRUPT,
           "D8 未复用既有 LOG_P_BOOT_SEQ(0x21)");

    /* ---------- D9：参数数量上限容得下 5 个 ---------- */
    checkb(LOG_MAX_PARAMS >= 5u, "D9 LOG_MAX_PARAMS >= 5（启动事件用 5 个）");
    checkv((uint32_t)LOG_MAX_PARAMS, 8u, "D10 LOG_MAX_PARAMS 仍为 8（未被改动）");

    /* ---------- D11：栈开销 ---------- */
    checkb(sizeof(LogParamIn) <= 16u, "D11 sizeof(LogParamIn) <= 16 B");
    checkb(sizeof(LogParamIn) * 5u <= 80u, "D12 5 个参数 <= 80 B（栈开销可控）");

    /* ---------- D13：boot_seq 在记录头部，不需要 PID ---------- */
    checkv((uint32_t)LOG_OFF_BOOT_SEQ, 8u, "D13 LOG_OFF_BOOT_SEQ = 8（头部自带）");

    /* ---------- D14：事件 ID 未变 ---------- */
    checkv((uint32_t)LOG_SYS_BOOT_COMPLETE, 0x0101u, "D14 LOG_SYS_BOOT_COMPLETE 仍为 0x0101");

    /* ---------- D15：类型映射（无 U8 ⇒ ENUM/BOOL） ---------- */
    checkb((uint32_t)LOG_PTYPE_U32 == 2u
           && (uint32_t)LOG_PTYPE_BOOL == 4u
           && (uint32_t)LOG_PTYPE_ENUM == 5u,
           "D15 类型常量 U32=2 / BOOL=4 / ENUM=5");

    /* ---------- D16：结构体字段语义（id / type / value 联合体） ----------
     * ⚠️ `log_arg_*` helper 在 `log_manager.h`（依赖 Arduino），本探针无法包含；
     *    它们的存在性由 `pio run` 编译保证（main.cpp 直接调用）。
     *    这里验证的是**结构体本身**能否正确承载 5 个参数。 */
    {
        LogParamIn p[5];

        p[0].id = LOG_P_INIT_MS;
        p[0].type = LOG_PTYPE_U32;
        p[0].v.u = 1234u;

        p[1].id = LOG_P_RESERVED_HW;
        p[1].type = LOG_PTYPE_U32;
        p[1].v.u = 4096u;

        p[2].id = LOG_P_RECOVERY_SRC;
        p[2].type = LOG_PTYPE_ENUM;
        p[2].v.u = 0u;

        p[3].id = LOG_P_FAILDCLOSED;
        p[3].type = LOG_PTYPE_BOOL;
        p[3].v.u = 0u;

        p[4].id = LOG_P_TRACE_CORRUPT;
        p[4].type = LOG_PTYPE_U32;
        p[4].v.u = 2u;

        checkb(p[0].id == LOG_P_INIT_MS && p[0].type == LOG_PTYPE_U32 && p[0].v.u == 1234u,
               "D16a INIT_MS 承载（u32/值）");
        checkb(p[1].id == LOG_P_RESERVED_HW && p[1].type == LOG_PTYPE_U32 && p[1].v.u == 4096u,
               "D16b RESERVED_HW 承载（u32/值）");
        checkb(p[2].id == LOG_P_RECOVERY_SRC && p[2].type == LOG_PTYPE_ENUM && p[2].v.u == 0u,
               "D16c RECOVERY_SRC 承载（enum/NVS=0）");
        checkb(p[3].id == LOG_P_FAILDCLOSED && p[3].type == LOG_PTYPE_BOOL && p[3].v.u == 0u,
               "D16d FAILDCLOSED 承载（bool/0）");
        checkb(p[4].id == LOG_P_TRACE_CORRUPT && p[4].type == LOG_PTYPE_U32 && p[4].v.u == 2u,
               "D16e TRACE_CORRUPT 承载（u32/值）");

        /* 顺序即契约：消费方按 pid 取值，不依赖下标 */
        checkb(p[0].id == LOG_P_INIT_MS && p[1].id == LOG_P_RESERVED_HW
               && p[2].id == LOG_P_RECOVERY_SRC && p[3].id == LOG_P_FAILDCLOSED
               && p[4].id == LOG_P_TRACE_CORRUPT,
               "D16f 5 个参数顺序固定（pid 唯一标识）");

        /* 尺寸：id/type 各 1 字节 ⇒ 结构体头部偏移可预测 */
        checkb(offsetof(LogParamIn, id) == 0u && offsetof(LogParamIn, type) == 1u,
               "D16g LogParamIn 布局：id@0 / type@1");
    }

    pstr("\n== probe_boot_diag done, failures=");
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
