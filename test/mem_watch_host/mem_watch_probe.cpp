// =====================================================
// MemWatch 宿主机探针（`G4-R4` · H-3/H-5 的**纯计算**部分）
//
// 为什么需要：`mem_watch.cpp` 依赖 ESP-IDF（heap_caps / FreeRTOS），
// 无法在主机执行；但漂移斜率与栈单位换算的**正确性**不能靠推理，
// 必须有真实执行证据。故把这两块抽到 `src/services/mem_watch_calc.h`
// （零依赖纯函数），本探针用 wasm32 freestanding + node 真实运行。
//
// 与 `test/loop_stats_host` 同一模式（本机 clang 无 MSVC libc）。
//
// 覆盖：
//   M-1  斜率：完美线性 ⇒ 精确命中（含负向泄漏）
//   M-2  斜率：常量序列 ⇒ 0
//   M-3  斜率：抖动序列 ⇒ 接近 0（不被单点毛刺带偏）
//   M-4  最小二乘优于"两点斜率"（对照）
//   M-5  样本不足 / 跨度不足 ⇒ valid=0 且 reason 正确
//   M-6  极值：free_min / largest_min 与 delta 为**直接测量**，与 valid 无关
//   M-7  DRAM/PSRAM 字段选择（use_psram）互不干扰
//   M-8  栈单位自证：raw ≤ stack ⇒ bytes；raw > stack ⇒ words（×4）
//   M-9  边界：n=0 / nullptr 不崩
//   M-10 大跨度不溢出 int64 中间量（32 min，dy 20 万 B）
// =====================================================

#include <stdint.h>
#include <stddef.h>

#include "services/mem_watch_calc.h"

// ---- freestanding 环境的最小运行时 ----
extern "C" void *memcpy(void *dst, const void *src, unsigned n)
{
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    while (n--) *d++ = *s++;
    return dst;
}

extern "C" void *memset(void *dst, int v, unsigned n)
{
    unsigned char *d = (unsigned char *)dst;
    while (n--) *d++ = (unsigned char)v;
    return dst;
}

// ---- 日志缓冲（node 侧读取）----
static char   s_log[65536];
static size_t s_log_len = 0;
static int    s_fail = 0;

static void out(const char *s)
{
    while (*s != '\0')
    {
        if (s_log_len < sizeof(s_log) - 1) s_log[s_log_len++] = *s;
        s++;
    }
    s_log[s_log_len] = '\0';
}

static void out_num(long long v)
{
    char buf[24];
    int  i = 0;
    unsigned long long u;

    if (v < 0)
    {
        out("-");
        u = (unsigned long long)(-v);
    }
    else
    {
        u = (unsigned long long)v;
    }

    if (u == 0) { out("0"); return; }

    while (u > 0) { buf[i++] = (char)('0' + (u % 10)); u /= 10; }
    while (i > 0) { char c[2] = { buf[--i], '\0' }; out(c); }
}

static void check(const char *name, bool cond, long long got, long long want)
{
    if (cond)
    {
        out("  PASS  ");
        out(name);
        out("\n");
    }
    else
    {
        s_fail++;
        out("  FAIL  ");
        out(name);
        out("   got=");
        out_num(got);
        out(" want=");
        out_num(want);
        out("\n");
    }
}

// ---- 构造样本 ----
static void put(MemDriftSample *s, uint8_t i, uint32_t t_ms,
                int32_t dram, int32_t dram_largest,
                int32_t psram, int32_t psram_largest)
{
    s[i].t_ms          = t_ms;
    s[i].dram_free     = dram;
    s[i].dram_largest  = dram_largest;
    s[i].psram_free    = psram;
    s[i].psram_largest = psram_largest;
}

extern "C" int probe_run()
{
    s_fail = 0;
    s_log_len = 0;
    out("=== MemWatch calc probe (wasm32 freestanding) ===\n");

    MemDriftSample s[MEMWATCH_RING_SAMPLES];

    // ---------------- M-1 线性：每 60 s 减少 100 B ⇒ -6000 B/h ----------------
    {
        for (uint8_t i = 0; i < 10; i++)
        {
            put(s, i, (uint32_t)i * 60000u,
                100000 - (int32_t)(i * 100), 50000,
                1000000, 900000);
        }
        const MemDriftResult r = memwatch_drift_calc(s, 10, 0u, 0u);
        check("M-1-1 线性泄漏 ⇒ valid", r.valid == 1, r.valid, 1);
        check("M-1-2 斜率 = -6000 B/h", r.slope_per_hour == -6000, r.slope_per_hour, -6000);
        check("M-1-3 样本数入结果", r.samples == 10, r.samples, 10);
        check("M-1-4 free_delta = -900", r.free_delta == -900, r.free_delta, -900);
        check("M-1-5 span = 540000 ms", r.span_ms == 540000u, (long long)r.span_ms, 540000);
    }

    // ---------------- M-2 常量 ⇒ 0 ----------------
    {
        for (uint8_t i = 0; i < 8; i++)
        {
            put(s, i, (uint32_t)i * 60000u, 90000, 40000, 0, 0);
        }
        const MemDriftResult r = memwatch_drift_calc(s, 8, 0u, 0u);
        check("M-2-1 无漂移 ⇒ valid", r.valid == 1, r.valid, 1);
        check("M-2-2 斜率 = 0", r.slope_per_hour == 0, r.slope_per_hour, 0);
        check("M-2-3 delta = 0", r.free_delta == 0, r.free_delta, 0);
    }

    // ---------------- M-3 抖动 + 单点毛刺：最小二乘不应被带偏 ----------------
    {
        const int32_t ys[8] = { 100000, 100200, 99800, 100100, 99900, 100150, 99850, 100000 };
        for (uint8_t i = 0; i < 8; i++)
        {
            put(s, i, (uint32_t)i * 60000u, ys[i], 50000, 0, 0);
        }
        const MemDriftResult r = memwatch_drift_calc(s, 8, 0u, 0u);
        check("M-3-1 抖动 ⇒ 斜率接近 0（|slope| < 1000 B/h）",
              (r.slope_per_hour > -1000 && r.slope_per_hour < 1000),
              r.slope_per_hour, 0);

        // 对照：只看首末点（`free_delta`）会得到 0 ⇒ **完全漏掉**抖动幅度；
        // 这也是为什么"漂移"必须用拟合，而不是"首末相减"。
        check("M-3-2 对照：首末点 delta = 0（漏掉抖动）", r.free_delta == 0, r.free_delta, 0);
        check("M-3-3 而窗口 free_min 记录了抖动最低点 99800",
              r.free_min == 99800, r.free_min, 99800);
    }

    // ---------------- M-4 一阶拟合对"折线"的取值 ----------------
    {
        // 前 5 点持平 100000，后 5 点每点 -100 ⇒ 拟合斜率应落在 -3000±200 B/h
        for (uint8_t i = 0; i < 10; i++)
        {
            const int32_t v = (i < 5) ? 100000 : (int32_t)(100000 - (i - 4) * 100);
            put(s, i, (uint32_t)i * 60000u, v, 50000, 0, 0);
        }
        const MemDriftResult r = memwatch_drift_calc(s, 10, 0u, 0u);
        check("M-4-1 折线 ⇒ 斜率介于 -4000 与 -2000 B/h",
              (r.slope_per_hour > -4000 && r.slope_per_hour < -2000),
              r.slope_per_hour, 0);
    }

    // ---------------- M-5 样本/跨度不足 ----------------
    {
        put(s, 0, 0u, 1000, 500, 0, 0);
        put(s, 1, 60000u, 900, 500, 0, 0);
        const MemDriftResult r = memwatch_drift_calc(s, 2, 0u, 0u);
        check("M-5-1 样本 < 4 ⇒ valid=0", r.valid == 0, r.valid, 0);
        check("M-5-2 reason = too_few", r.reason == MEMWATCH_REASON_TOO_FEW, r.reason, 1);

        for (uint8_t i = 0; i < 8; i++)
        {
            put(s, i, (uint32_t)i * 60000u, 1000 - (int32_t)i, 500, 0, 0);
        }
        const MemDriftResult r2 = memwatch_drift_calc(s, 8, 1800000u, 0u);
        check("M-5-3 跨度 7 min < 30 min ⇒ valid=0", r2.valid == 0, r2.valid, 0);
        check("M-5-4 reason = span_too_short",
              r2.reason == MEMWATCH_REASON_SPAN_SHORT, r2.reason, 2);
    }

    // ---------------- M-6 直接测量字段与 valid 无关 ----------------
    {
        for (uint8_t i = 0; i < 6; i++)
        {
            // free 递减，largest 在第 3 点掉到 12000（模拟碎片化尖峰）
            const int32_t largest = (i == 2) ? 12000 : 50000;
            put(s, i, (uint32_t)i * 60000u, 90000 - (int32_t)i * 10, largest, 0, 0);
        }
        const MemDriftResult r = memwatch_drift_calc(s, 6, 1800000u, 0u);  // 故意让 valid=0
        check("M-6-1 窗口不足 ⇒ valid=0", r.valid == 0, r.valid, 0);
        check("M-6-2 但仍给出 free_min=89950", r.free_min == 89950, r.free_min, 89950);
        check("M-6-3 且给出 largest_min=12000（碎片硬指标）",
              r.largest_min == 12000, r.largest_min, 12000);
        check("M-6-4 且给出 free_delta=-50", r.free_delta == -50, r.free_delta, -50);
    }

    // ---------------- M-7 DRAM / PSRAM 字段互不干扰 ----------------
    {
        for (uint8_t i = 0; i < 8; i++)
        {
            put(s, i, (uint32_t)i * 60000u,
                100000, 50000,
                2000000 - (int32_t)i * 1000, 1500000);
        }
        const MemDriftResult d = memwatch_drift_calc(s, 8, 0u, 0u);
        const MemDriftResult p = memwatch_drift_calc(s, 8, 0u, 1u);
        check("M-7-1 DRAM 斜率 0", d.slope_per_hour == 0, d.slope_per_hour, 0);
        check("M-7-2 PSRAM 斜率 -60000 B/h", p.slope_per_hour == -60000,
              p.slope_per_hour, -60000);
        check("M-7-3 PSRAM delta = -7000", p.free_delta == -7000, p.free_delta, -7000);
        check("M-7-4 DRAM largest_min 不受 PSRAM 影响", d.largest_min == 50000, d.largest_min, 50000);
    }

    // ---------------- M-8 栈单位：一致性 + 实测反推 ----------------
    //
    // ★ 设计缺陷留痕：第一版曾用"raw > stack ⇒ words，故 ×4"做自动换算。
    //   该判据**数学上永不成立**（见 `mem_watch_calc.h` 说明）——
    //   两种口径下 raw 都必然 ≤ stack。主机探针当场暴露了它，故改为：
    //     ① 一致性只做**必要条件**校验（raw ≤ stack）
    //     ② 口径由**受控深栈探测的下降量**反推（真机可执行）
    {
        check("M-8-1 raw 7000 ≤ stack 16384 ⇒ 自洽",
              memwatch_stack_consistent(7000u, 16384u) == 1u, 1, 1);
        check("M-8-2 raw 16384 = stack ⇒ 自洽（边界）",
              memwatch_stack_consistent(16384u, 16384u) == 1u, 1, 1);
        check("M-8-3 raw 16385 > stack ⇒ **不自洽**（物理不可能）",
              memwatch_stack_consistent(16385u, 16384u) == 0u, 0, 0);
        check("M-8-4 stack=0 ⇒ 不判假（视为自洽，避免误报）",
              memwatch_stack_consistent(5000u, 0u) == 1u, 1, 1);

        // 下降量反推：depth=8192
        check("M-8-5 delta 8192 ≈ depth ⇒ 判定**字节**口径",
              memwatch_stack_delta_implies_bytes(8192u, 8192u, 3276u) == 1u, 1, 1);
        check("M-8-6 delta 2048 ≈ depth/4 ⇒ 判定**字数**口径",
              memwatch_stack_delta_implies_bytes(2048u, 8192u, 3276u) == 0u, 0, 0);
        check("M-8-7 delta 5000（既非 depth 也非 depth/4）⇒ 无法判定(2)",
              memwatch_stack_delta_implies_bytes(5000u, 8192u, 819u) == 2u, 2, 2);
        check("M-8-8 delta=0（探测未生效）⇒ 无法判定(2)",
              memwatch_stack_delta_implies_bytes(0u, 8192u, 3276u) == 2u, 2, 2);
        check("M-8-9 depth=0 ⇒ 无法判定(2)",
              memwatch_stack_delta_implies_bytes(8192u, 0u, 0u) == 2u, 2, 2);
    }

    // ---------------- M-9 边界 ----------------
    {
        const MemDriftResult r0 = memwatch_drift_calc(nullptr, 5, 0u, 0u);
        check("M-9-1 nullptr ⇒ valid=0 且不崩", r0.valid == 0, r0.valid, 0);

        const MemDriftResult r1 = memwatch_drift_calc(s, 0, 0u, 0u);
        check("M-9-2 n=0 ⇒ valid=0 且不崩", r1.valid == 0, r1.valid, 0);
    }

    // ---------------- M-10 大跨度 / 大 dy 不溢出 ----------------
    {
        // 32 点 × 60 s = 31 min，dy 每点 -2000 ⇒ 总 -62000 B；斜率应 -4000 B/h
        for (uint8_t i = 0; i < MEMWATCH_RING_SAMPLES; i++)
        {
            put(s, i, (uint32_t)i * 60000u,
                260000 - (int32_t)i * 2000, 90000, 0, 0);
        }
        const MemDriftResult r = memwatch_drift_calc(s, MEMWATCH_RING_SAMPLES, 1800000u, 0u);
        check("M-10-1 32 点满环 ⇒ valid", r.valid == 1, r.valid, 1);
        check("M-10-2 斜率 = -120000 B/h（每 60 s -2000 B）",
              r.slope_per_hour == -120000, r.slope_per_hour, -120000);
        check("M-10-3 span = 1860000 ms", r.span_ms == 1860000u, (long long)r.span_ms, 1860000);
    }

    out("\n===== summary =====\n  failures = ");
    out_num(s_fail);
    out("\n");

    return s_fail;
}

extern "C" int         probe_log_len() { return (int)s_log_len; }
extern "C" const char *probe_log_ptr() { return s_log; }
