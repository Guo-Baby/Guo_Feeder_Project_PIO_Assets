// =====================================================
// LoopStats —— 宿主机探针（wasm32 freestanding + node）
//
// 用途：在**没有硬件**的环境里**真实执行** loop_stats 的全部纯逻辑，
//       覆盖：桶边界 / 首样本丢弃 / 异常样本 / 溢出桶 / 饱和 /
//             P50-P99 分位 / 快照差分 / 计数回退 / 几何校验 / 门禁边界。
//
// 编译（见 run.sh）：
//   clang++ --target=wasm32 -std=gnu++17 -nostdlib -fno-builtin -O1 \
//           -DLOOP_STATS_HOST_TEST -I src \
//           loop_stats_probe.cpp ../../src/app/loop_stats.cpp \
//           -Wl,--no-entry -Wl,--export-all -o probe.wasm
//
// ★ 本探针只证明"纯逻辑正确"，**不**证明目标板时序/开销 ——
//   后者必须在真实 ESP32-S3 上由 `system.loop` 的 snap_crit_max_us 取证。
// =====================================================

#include <stddef.h>
#include "app/loop_stats.h"

// ---- freestanding 支撑（避免 libcall） ----
extern "C" void *memcpy(void *dst, const void *src, size_t n)
{
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    for (size_t i = 0; i < n; ++i) { d[i] = s[i]; }
    return dst;
}
extern "C" void *memset(void *dst, int c, size_t n)
{
    unsigned char *d = (unsigned char *)dst;
    for (size_t i = 0; i < n; ++i) { d[i] = (unsigned char)c; }
    return dst;
}

// ---- 时间源（由探针完全控制） ----
static long long g_now_us = 0;
extern "C" long long loop_stats_host_time_us(void) { return g_now_us; }

// ---- 结果记录 ----
static char g_log[32768];
static int  g_log_len = 0;
static int  g_pass = 0;
static int  g_fail = 0;

static void put(const char *s)
{
    while (*s != '\0' && g_log_len < (int)sizeof(g_log) - 1) { g_log[g_log_len++] = *s++; }
}
static void put_u(long long v)
{
    char b[24];
    int n = 0;
    if (v < 0) { put("-"); v = -v; }
    if (v == 0) { b[n++] = '0'; }
    while (v > 0) { b[n++] = (char)('0' + (int)(v % 10)); v /= 10; }
    while (n > 0) { char c = b[--n]; if (g_log_len < (int)sizeof(g_log) - 1) { g_log[g_log_len++] = c; } }
}

static void check(bool ok, const char *name, long long got, long long want)
{
    if (ok) { g_pass++; put("  PASS  "); }
    else    { g_fail++; put("  FAIL  "); }
    put(name);
    put("  got="); put_u(got);
    put(" want="); put_u(want);
    put("\n");
}
#define CHECK_EQ(name, got, want) check(((long long)(got)) == ((long long)(want)), name, (long long)(got), (long long)(want))
#define CHECK_TRUE(name, cond)    check((cond) ? true : false, name, (cond) ? 1 : 0, 1)

// =====================================================
// 快照构造辅助（直接构造 POD，绕开采样器 ⇒ 单测"纯计算"部分）
// =====================================================
static void snap_reset(LoopStatsSnapshot &s)
{
    memset(&s, 0, sizeof(s));
    s.magic = loopstat::MAGIC;
    s.bucket_count = loopstat::BUCKET_COUNT;
    s.bucket_width_us = loopstat::BUCKET_WIDTH_US;
    s.available = 1;
}
static void snap_add_bucket(LoopStatsSnapshot &s, uint32_t b, uint32_t cnt)
{
    s.hist[b] += cnt;
    s.n += cnt;
    const uint32_t rep = b * loopstat::BUCKET_WIDTH_US + 100; // 桶内代表值
    s.sum_us += (uint64_t)cnt * (uint64_t)rep;
    if (s.min_us == 0u || rep < s.min_us) { s.min_us = rep; }
    if (rep > s.max_us) { s.max_us = rep; }
}
static void snap_add_over(LoopStatsSnapshot &s, uint32_t cnt, uint32_t us)
{
    s.over_cnt += cnt;
    s.n += cnt;
    s.sum_us += (uint64_t)cnt * (uint64_t)us;
    if (s.min_us == 0u || us < s.min_us) { s.min_us = us; }
    if (us > s.max_us) { s.max_us = us; }
}

// =====================================================
// T1 可用性 / 首样本
// =====================================================
static void t1_first_sample()
{
    put("\n[T1] 可用性与首样本丢弃\n");
    loop_stats_host_reset();
    g_now_us = 5000000;                     // 5 s

    LoopStatsSnapshot s;
    CHECK_TRUE("未进入 loop() 前 available=false", loop_stats_get(s) == false);
    CHECK_EQ("未进入 loop() 前 available 字段", s.available, 0);

    loop_stats_tick();                      // 首样本：只建基线
    CHECK_TRUE("首样本后 available=true", loop_stats_get(s) == true);
    CHECK_EQ("首样本**不**计入 n", s.n, 0);
    CHECK_EQ("首样本后 elapsed=0", (long long)(s.elapsed_us / 1000ull), 0);

    g_now_us += 1000;                       // 第二样本 dt=1000 µs
    loop_stats_tick();
    CHECK_TRUE("第二样本后可读", loop_stats_get(s) == true);
    CHECK_EQ("第二样本计入 n", s.n, 1);
    CHECK_EQ("min_us", s.min_us, 1000);
    CHECK_EQ("max_us", s.max_us, 1000);
    CHECK_EQ("bit0 命中（1000/200=5）", s.hist[5], 1);
    CHECK_EQ("sum_us", (long long)s.sum_us, 1000);
}

// =====================================================
// T2 桶边界 / 溢出桶
// =====================================================
static void t2_bucket_edges()
{
    put("\n[T2] 桶边界与溢出桶（桶宽 200 µs，量程 25.6 ms）\n");
    loop_stats_host_reset();
    g_now_us = 1000000;
    loop_stats_tick();                       // 基线

    struct { uint32_t dt; int want_bucket; } cases[] = {
        {1,      0},      // [0,200)    -> 0
        {199,    0},      // 上边界-1
        {200,    1},      // 恰好边界
        {25599,  127},    // 最后一个桶
        {25600,  -1},     // 恰为量程 => over
        {30000,  -1},     // over
    };
    uint32_t expect_hist[loopstat::BUCKET_COUNT];
    for (uint32_t i = 0; i < loopstat::BUCKET_COUNT; ++i) { expect_hist[i] = 0; }
    uint32_t expect_over = 0;

    for (uint32_t k = 0; k < sizeof(cases) / sizeof(cases[0]); ++k)
    {
        g_now_us += (long long)cases[k].dt;
        loop_stats_tick();
        if (cases[k].want_bucket >= 0) { expect_hist[cases[k].want_bucket]++; }
        else { expect_over++; }
    }

    LoopStatsSnapshot s;
    loop_stats_get(s);
    CHECK_EQ("n = 6 个样本", s.n, 6);
    CHECK_EQ("over_cnt = 2（≥25.6 ms）", s.over_cnt, expect_over);
    CHECK_EQ("桶 0 计数（dt=1,199）", s.hist[0], expect_hist[0]);
    CHECK_EQ("桶 1 计数（dt=200）", s.hist[1], expect_hist[1]);
    CHECK_EQ("桶 127 计数（dt=25599）", s.hist[127], expect_hist[127]);
    CHECK_EQ("max_us 精确（30000）", s.max_us, 30000);
    CHECK_EQ("min_us 精确（1）", s.min_us, 1);

    uint64_t sum_hist = 0;
    for (uint32_t i = 0; i < loopstat::BUCKET_COUNT; ++i) { sum_hist += s.hist[i]; }
    CHECK_EQ("恒等式 Σhist + over = n", (long long)(sum_hist + s.over_cnt), s.n);
}

// =====================================================
// T3 异常样本
// =====================================================
static void t3_anomaly()
{
    put("\n[T3] 异常样本（dt<=0 / dt>60 s）不计入统计\n");
    loop_stats_host_reset();
    g_now_us = 1000000;
    loop_stats_tick();

    g_now_us += 0;                 // dt = 0  => anomaly
    loop_stats_tick();
    g_now_us -= 500;               // dt = -500 => anomaly
    loop_stats_tick();
    g_now_us += 60000001;          // dt = 60000001 > 60 s => anomaly
    loop_stats_tick();
    g_now_us += 60000000;          // dt = 60 s 恰好 => 非异常，进 over
    loop_stats_tick();
    g_now_us += 1500;              // 正常样本
    loop_stats_tick();

    LoopStatsSnapshot s;
    loop_stats_get(s);
    CHECK_EQ("anomaly_cnt = 3", s.anomaly_cnt, 3);
    CHECK_EQ("n 只含 2 个有效样本", s.n, 2);
    CHECK_EQ("60 s 样本进 over（不判异常）", s.over_cnt, 1);
    CHECK_EQ("max_us = 60 000 000", s.max_us, 60000000u);
}

// =====================================================
// T4 分位（P50 / P99）
// =====================================================
static void t4_quantiles()
{
    put("\n[T4] 分位计算（rank = ceil(p*N)）\n");
    LoopStatsSnapshot base, top;
    LoopStatsQuantiles q;

    // --- 9000 @ b0 / 900 @ b10 / 100 @ b60，N=10000 ---
    snap_reset(base); base.magic = loopstat::MAGIC_EMPTY;
    snap_reset(top);
    snap_add_bucket(top, 0, 9000);
    snap_add_bucket(top, 10, 900);
    snap_add_bucket(top, 60, 100);

    loop_stats_compute(base, top, q);
    CHECK_EQ("N", q.n, 10000);
    CHECK_TRUE("valid", q.valid == 1);
    CHECK_EQ("P50 桶 = 0（累计 9000 ≥ 5000）", q.p50_bucket, 0);
    CHECK_EQ("P99 桶 = 10（累计 9900 ≥ 9900）", q.p99_bucket, 10);
    CHECK_EQ("P99 下界", q.p99_low_us, 2000);
    CHECK_EQ("P99 上界", q.p99_high_us, 2200);
    CHECK_EQ("mean = sum/n（非 P50）", q.mean_us, (uint32_t)(top.sum_us / 10000ull));

    // --- 恰好 99% 落在桶 0（rank 边界）---
    snap_reset(top);
    snap_add_bucket(top, 0, 9900);
    snap_add_bucket(top, 5, 100);
    loop_stats_compute(base, top, q);
    CHECK_EQ("P99 桶 = 0（9900/10000 恰为 99%）", q.p99_bucket, 0);

    // --- 门禁边界：P99 落在桶 99 => <20 ms；桶 100 => ≥20 ms ---
    snap_reset(top);
    snap_add_bucket(top, 0, 100);
    snap_add_bucket(top, 99, 9900);
    loop_stats_compute(base, top, q);
    CHECK_EQ("P99 桶 = 99", q.p99_bucket, 99);
    CHECK_EQ("P99 上界 = 20 000（桶 99 = [19800,20000)）", q.p99_high_us, 20000);
    CHECK_EQ("p99_lt_gate = 1（全部 <20 ms）", q.p99_lt_gate, 1);

    snap_reset(top);
    snap_add_bucket(top, 0, 100);
    snap_add_bucket(top, 100, 9900);
    loop_stats_compute(base, top, q);
    CHECK_EQ("P99 桶 = 100", q.p99_bucket, 100);
    CHECK_EQ("P99 下界 = 20 000（≥20 ms）", q.p99_low_us, 20000);
    CHECK_EQ("p99_lt_gate = 0（≥20 ms）", q.p99_lt_gate, 0);
}

// =====================================================
// T5 快照差分
// =====================================================
static void t5_diff()
{
    put("\n[T5] 两次快照差分（窗口统计）\n");
    LoopStatsSnapshot base, top;
    LoopStatsQuantiles q;

    snap_reset(base);
    snap_add_bucket(base, 0, 50000);          // 窗口前的历史样本
    snap_add_bucket(base, 3, 50000);

    snap_reset(top);
    snap_add_bucket(top, 0, 50000);
    snap_add_bucket(top, 3, 50000);
    snap_add_bucket(top, 7, 9000);            // 窗口内新增
    snap_add_bucket(top, 7, 0);
    snap_add_bucket(top, 8, 900);
    snap_add_bucket(top, 40, 100);

    loop_stats_compute(base, top, q);
    CHECK_EQ("Δn 只含窗口增量", q.n, 10000);
    CHECK_TRUE("valid", q.valid == 1);
    CHECK_EQ("P50 桶 = 7（9000 ≥ 5000）", q.p50_bucket, 7);
    // 累计到桶 8 时恰为 9900 = rank99 ⇒ P99 落在桶 8（**不是**桶 40）
    CHECK_EQ("P99 桶 = 8（累计 9900 恰达 rank99）", q.p99_bucket, 8);
    CHECK_TRUE("差分不重复计入窗口前样本", q.n == 10000);
}

// =====================================================
// T6 饱和
// =====================================================
static void t6_saturation()
{
    put("\n[T6] 桶饱和 / n 饱和 ⇒ 拒绝输出有效 P99\n");
    loop_stats_host_reset();
    g_now_us = 100000;
    loop_stats_tick();

    loop_stats_host_force_bucket_full(3);     // 桶 3 预置到 uint32 上限（未置 sat 位）
    g_now_us += 700;                          // dt=700 => 桶 3
    loop_stats_tick();
    g_now_us += 700;
    loop_stats_tick();                        // 第二次命中饱和桶

    LoopStatsSnapshot s;
    loop_stats_get(s);
    CHECK_EQ("饱和桶计数不再增长", s.hist[3], 0xFFFFFFFFu);
    CHECK_EQ("sat_buckets 按不同桶只计 1 次", s.sat_buckets, 1);

    LoopStatsSnapshot zero;
    memset(&zero, 0, sizeof(zero));
    zero.magic = loopstat::MAGIC_EMPTY;
    LoopStatsQuantiles q;
    loop_stats_compute(zero, s, q);
    CHECK_TRUE("饱和 ⇒ valid=0", q.valid == 0);
    CHECK_TRUE("置 INVALID_SATURATION 位",
               (q.invalid_bits & loopstat::INVALID_SATURATION) != 0);

    // n 饱和
    loop_stats_host_reset();
    g_now_us += 1000;
    loop_stats_tick();
    g_now_us += 5000;
    loop_stats_tick();
    loop_stats_host_force_n_saturated();
    LoopStatsSnapshot s2;
    loop_stats_get(s2);
    loop_stats_compute(zero, s2, q);
    CHECK_TRUE("n 饱和 ⇒ 置 INVALID_N_SATURATED 位",
               (q.invalid_bits & loopstat::INVALID_N_SATURATED) != 0);
    CHECK_TRUE("n 饱和 ⇒ valid=0", q.valid == 0);
}

// =====================================================
// T7 样本不足 / 溢出区间 / 异常主导
// =====================================================
static void t7_invalid_paths()
{
    put("\n[T7] 样本不足 / P99 落在溢出区间 / 异常主导 / 空窗口\n");
    LoopStatsSnapshot base, top;
    LoopStatsQuantiles q;

    // --- 样本不足 ---
    snap_reset(base); base.magic = loopstat::MAGIC_EMPTY;
    snap_reset(top);
    snap_add_bucket(top, 0, 9999);
    loop_stats_compute(base, top, q);
    CHECK_TRUE("n=9999 ⇒ INVALID_INSUFFICIENT",
               (q.invalid_bits & loopstat::INVALID_INSUFFICIENT) != 0);
    CHECK_TRUE("n=9999 ⇒ valid=0", q.valid == 0);

    // --- P99 落在溢出区间（Δover=1000 占 10%）---
    snap_reset(top);
    snap_add_bucket(top, 0, 9000);
    snap_add_over(top, 1000, 30000);
    loop_stats_compute(base, top, q);
    CHECK_EQ("N 含 over 样本", q.n, 10000);
    CHECK_TRUE("P99 落在 overflow 区间", q.p99_in_overflow == 1);
    CHECK_TRUE("overflow ⇒ valid=0", q.valid == 0);
    CHECK_EQ("overflow 时仍保留精确 max", q.max_us_uptime, 30000);
    CHECK_TRUE("overflow 时不产生伪造的 P99 桶", q.p99_high_us == 0);

    // --- 异常主导 ---
    snap_reset(top);
    snap_add_bucket(top, 0, 100);
    top.anomaly_cnt = 500;
    loop_stats_compute(base, top, q);
    CHECK_TRUE("anomaly > n ⇒ INVALID_ANOMALY_DOMINANT",
               (q.invalid_bits & loopstat::INVALID_ANOMALY_DOMINANT) != 0);

    // --- Δn = 0（空窗口）不得被误判成"P99 在 overflow"---
    snap_reset(base);
    snap_reset(top);
    snap_add_bucket(top, 0, 50000);
    snap_add_bucket(base, 0, 50000);
    loop_stats_compute(base, top, q);
    CHECK_EQ("空窗口 N=0", q.n, 0);
    CHECK_TRUE("空窗口不置 p99_in_overflow", q.p99_in_overflow == 0);
    CHECK_TRUE("空窗口 ⇒ INVALID_INSUFFICIENT",
               (q.invalid_bits & loopstat::INVALID_INSUFFICIENT) != 0);
}

// =====================================================
// T8 结构 / 几何 / 回退
// =====================================================
static void t8_geometry_regression()
{
    put("\n[T8] 几何校验 / 计数回退 / 恒等式破坏\n");
    LoopStatsSnapshot base, top;
    LoopStatsQuantiles q;

    snap_reset(top);
    top.magic = 0xDEADBEEF;
    CHECK_TRUE("magic 错 ⇒ 返回 false", loop_stats_compute(base, top, q) == false);
    CHECK_EQ("置 INVALID_GEOMETRY", q.invalid_bits, loopstat::INVALID_GEOMETRY);

    snap_reset(top);
    top.bucket_width_us = 100;
    CHECK_TRUE("桶宽不符 ⇒ 返回 false", loop_stats_compute(base, top, q) == false);

    snap_reset(top);
    top.available = 0;
    CHECK_TRUE("available=0 ⇒ 返回 false", loop_stats_compute(base, top, q) == false);
    CHECK_EQ("置 INVALID_NOT_AVAILABLE", q.invalid_bits, loopstat::INVALID_NOT_AVAILABLE);

    // 计数回退
    snap_reset(base);
    snap_reset(top);
    snap_add_bucket(base, 2, 20000);
    snap_add_bucket(top, 2, 10000);
    loop_stats_compute(base, top, q);
    CHECK_TRUE("桶计数回退 ⇒ INVALID_REGRESSION",
               (q.invalid_bits & loopstat::INVALID_REGRESSION) != 0);

    // 恒等式破坏（n 与 hist+over 不等）
    snap_reset(base); base.magic = loopstat::MAGIC_EMPTY;
    snap_reset(top);
    snap_add_bucket(top, 0, 20000);
    top.n += 5;                              // 人为破坏
    loop_stats_compute(base, top, q);
    CHECK_TRUE("恒等式破坏 ⇒ INVALID_REGRESSION",
               (q.invalid_bits & loopstat::INVALID_REGRESSION) != 0);

    // 结构体积
    CHECK_TRUE("sizeof(LoopStatsSnapshot) <= 1024", sizeof(LoopStatsSnapshot) <= 1024);
    CHECK_EQ("sizeof(LoopStatsSnapshot)", (long long)sizeof(LoopStatsSnapshot), 576);
    CHECK_TRUE("sizeof(LoopStatsQuantiles) <= 128", sizeof(LoopStatsQuantiles) <= 128);
    CHECK_EQ("门禁阈值是桶宽整数倍",
             loopstat::GATE_P99_US % loopstat::BUCKET_WIDTH_US, 0);
}

// =====================================================
// T9 端到端：采样器 → 快照 → 分位（真实调用链）
// =====================================================
static void t9_end_to_end()
{
    put("\n[T9] 端到端（tick → snapshot → compute）\n");
    loop_stats_host_reset();
    g_now_us = 0;
    loop_stats_tick();

    // 12000 个 1000 µs 样本（1 kHz）+ 100 个 3000 µs 样本 ⇒ P99 应落在 1000 所在桶
    for (int i = 0; i < 12000; ++i) { g_now_us += 1000; loop_stats_tick(); }
    for (int i = 0; i < 100; ++i)   { g_now_us += 3000; loop_stats_tick(); }

    LoopStatsSnapshot s;
    CHECK_TRUE("快照可读", loop_stats_get(s) == true);
    CHECK_EQ("样本数", s.n, 12100);
    CHECK_EQ("无溢出", s.over_cnt, 0);
    CHECK_EQ("无异常", s.anomaly_cnt, 0);
    CHECK_EQ("elapsed ≈ 12.3 s", (long long)(s.elapsed_us / 1000ull), 12300);
    CHECK_EQ("sum_us ≈ elapsed（采样覆盖完整）", (long long)(s.sum_us / 1000ull), 12300);

    LoopStatsSnapshot zero;
    memset(&zero, 0, sizeof(zero));
    zero.magic = loopstat::MAGIC_EMPTY;
    LoopStatsQuantiles q;
    loop_stats_compute(zero, s, q);
    CHECK_TRUE("valid", q.valid == 1);
    CHECK_EQ("P50 桶 = 5（1000 µs）", q.p50_bucket, 5);
    CHECK_EQ("P99 桶 = 5（3000 µs 仅占 0.8%）", q.p99_bucket, 5);
    CHECK_EQ("p99_lt_gate", q.p99_lt_gate, 1);
    CHECK_EQ("mean ≈ 1016 µs", q.mean_us, (uint32_t)((12000ull * 1000ull + 100ull * 3000ull) / 12100ull));

    // 采样率自洽：n / elapsed ≈ 1 / mean（**不用 P50 代替平均周期**）
    //   rate_from_counts  = n / elapsed(s) = n * 1000 / elapsed_ms
    //   rate_from_mean    = 1e6 / mean_us
    const long long rate_counts = (long long)s.n * 1000ll / (long long)(s.elapsed_us / 1000ull);
    const long long rate_mean = (long long)q.mean_us > 0 ? (1000000ll / (long long)q.mean_us) : 0;
    put("        rate_from_counts="); put_u(rate_counts);
    put(" rate_from_mean="); put_u(rate_mean); put("\n");
    const long long drift = (rate_counts > rate_mean) ? (rate_counts - rate_mean) : (rate_mean - rate_counts);
    CHECK_TRUE("n/elapsed 与 1/mean 偏差 <=1%（采样率自洽）",
               (drift * 100ll) <= (rate_mean * 1ll));
}

// =====================================================
// 入口
// =====================================================
extern "C" int probe_run()
{
    g_log_len = 0;
    g_pass = 0;
    g_fail = 0;

    put("=== LoopStats host probe（wasm32 freestanding，无硬件） ===\n");
    put("bucket_width_us="); put_u(loopstat::BUCKET_WIDTH_US);
    put(" bucket_count="); put_u(loopstat::BUCKET_COUNT);
    put(" range_us="); put_u(loopstat::RANGE_US);
    put(" gate_p99_us="); put_u(loopstat::GATE_P99_US);
    put("\n");

    t1_first_sample();
    t2_bucket_edges();
    t3_anomaly();
    t4_quantiles();
    t5_diff();
    t6_saturation();
    t7_invalid_paths();
    t8_geometry_regression();
    t9_end_to_end();

    put("\n=== SUMMARY: pass="); put_u(g_pass);
    put(" fail="); put_u(g_fail);
    put(" ===\n");
    return g_fail;
}

extern "C" const char *probe_log_ptr() { return g_log; }
extern "C" int probe_log_len() { return g_log_len; }
