// =====================================================
// LoopStats —— loop() 周期耗时统计（S-3 实现）
//
// ★ 本文件的三条硬约束（评审时逐条核对，违反即缺陷）：
//   1. **零输出**：不出现 Serial / printf / log_emit / ESP_LOGx / MQTT 调用
//      —— 一行 100 B 的串口打印在 115200 8N1 下 ≈ 8.7 ms，会直接制造假长尾。
//   2. **零动态分配**：不出现 new / malloc / String / std::vector。
//   3. **热路径无锁**：`loop_stats_tick()` 内不出现 portENTER_CRITICAL；
//      只有读路径 `loop_stats_get()` 用单临界区整体拷贝。
//
// 计时源：esp_timer_get_time()（int64 µs）。**禁止 micros()**（uint32 µs，
// 71.58 min 回绕，2 h 门禁窗口必然溢出）。
// =====================================================

#include "app/loop_stats.h"

#if defined(LOOP_STATS_HOST_TEST)
// ---------------- 宿主机（无硬件、无 Arduino、无 libc）----------------
// ★ 故意**不** include <string.h>：宿主机探针跑在 wasm32 freestanding
//   （-nostdlib），由探针自行提供 memcpy/memset（见 test/loop_stats_host）。
#include <stddef.h>
extern "C" void *memcpy(void *dst, const void *src, size_t n);
extern "C" void *memset(void *dst, int c, size_t n);
extern "C" int64_t loop_stats_host_time_us(void);
static inline int64_t ls_now_us(void) { return loop_stats_host_time_us(); }
#define LS_CRIT_ENTER() ((void)0)
#define LS_CRIT_EXIT()  ((void)0)
#else
// ---------------- 目标板 ----------------
#include <Arduino.h>
#include <string.h>
#include "esp_timer.h"

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static inline int64_t ls_now_us(void) { return (int64_t)esp_timer_get_time(); }
#define LS_CRIT_ENTER() portENTER_CRITICAL(&s_mux)
#define LS_CRIT_EXIT()  portEXIT_CRITICAL(&s_mux)
#endif

// =====================================================
// 内部状态（全部 static，编译期定容）
//   hist 512 B + 饱和位图 16 B + 标量 ≈ 40 B ⇒ 合计 ≈ 570 B 内部 RAM
//
// ★ 整段被 GF_LOOP_STATS_ENABLED 包裹：宏=0 时**连静态数组都不分配**
//   ⇒ "纯净对照构建"才是真正的零 RAM 零开销（不是"只关逻辑、留着数组"）。
// =====================================================
#if GF_LOOP_STATS_ENABLED
static uint32_t s_hist[loopstat::BUCKET_COUNT];      // 累积桶计数
static uint32_t s_sat_mask[loopstat::BUCKET_COUNT / 32]; // 已饱和桶的位图（去重计数用）
static uint32_t s_over;          // dt >= RANGE_US 的样本数
static uint32_t s_anomaly;       // dt <= 0 或 dt > 60 s
static uint32_t s_n;             // 有效样本数（含 over）
static uint32_t s_min_us;
static uint32_t s_max_us;
static uint32_t s_sat_buckets;   // 发生过饱和的**不同**桶数
static uint8_t  s_n_saturated;
static uint8_t  s_available;     // 1 = 首样本已建立
static uint64_t s_sum_us;
static int64_t  s_prev_us;       // 上一次 loop() 入口时刻
static int64_t  s_start_us;      // 首样本时刻（elapsed 起点）
static uint32_t s_crit_max_us;   // 读路径观测到的最大耗时
static uint32_t s_snap_count;
#endif // GF_LOOP_STATS_ENABLED

// 结构体积（1 KB 栈铁律：快照会被放在命令处理栈帧上）
static_assert(sizeof(LoopStatsSnapshot) <= 1024, "LoopStatsSnapshot 超 1 KB");
static_assert(sizeof(LoopStatsQuantiles) <= 128, "LoopStatsQuantiles 意外过大");
// 20 ms 必须落在桶边界上，否则 P99 判定会产生桶内插值歧义
static_assert((loopstat::GATE_P99_US % loopstat::BUCKET_WIDTH_US) == 0,
              "门禁阈值必须与桶宽整除");
static_assert((loopstat::GATE_P99_US / loopstat::BUCKET_WIDTH_US) < loopstat::BUCKET_COUNT,
              "门禁阈值必须落在直方图量程内");

// =====================================================
// 初始化
// =====================================================
void loop_stats_init()
{
#if GF_LOOP_STATS_ENABLED
    memset(s_hist, 0, sizeof(s_hist));
    memset(s_sat_mask, 0, sizeof(s_sat_mask));
    s_over = 0;
    s_anomaly = 0;
    s_n = 0;
    s_min_us = 0;
    s_max_us = 0;
    s_sat_buckets = 0;
    s_n_saturated = 0;
    s_available = 0;
    s_sum_us = 0;
    s_prev_us = 0;
    s_start_us = 0;
    s_crit_max_us = 0;
    s_snap_count = 0;
#endif
}

// =====================================================
// 采样（loop() 第一行）
//
// 开销预算：esp_timer_get_time ≈0.5–1.5 µs + 减法/索引/自增 ≈0.1 µs ⇒ 1.0–1.8 µs/轮
// （目标板实测值见实施报告；本函数内**没有任何**可能阻塞的调用）
// =====================================================
void loop_stats_tick()
{
#if GF_LOOP_STATS_ENABLED
    const int64_t now = ls_now_us();

    // 首样本：只建立基线，丢弃（否则首个 dt 是不完整周期）
    if (!s_available)
    {
        s_prev_us = now;
        s_start_us = now;
        s_available = 1;
        return;
    }

    const int64_t dt_signed = now - s_prev_us;
    s_prev_us = now;

    // ---- 异常样本：dt <= 0（时钟回退/并发重复进入）或 dt > 60 s（非物理）----
    // ★ 计入 anomaly 后**直接返回**：既不入 n，也不入 sum/min/max，
    //   因此 elapsed 与 sum 的差额即可反映"未被统计覆盖的时间"。
    if (dt_signed <= 0 || dt_signed > (int64_t)loopstat::ANOMALY_MAX_US)
    {
        s_anomaly++;
        return;
    }

    const uint32_t dt_us = (uint32_t)dt_signed;

    if (dt_us >= loopstat::RANGE_US)
    {
        // ---- 溢出桶：dt >= 25.6 ms ----
        if (s_over != 0xFFFFFFFFu) { s_over++; }
    }
    else
    {
        uint32_t b = dt_us / loopstat::BUCKET_WIDTH_US;
        if (b >= loopstat::BUCKET_COUNT) { b = loopstat::BUCKET_COUNT - 1; } // 防御（不可达）

        if (s_hist[b] != 0xFFFFFFFFu)
        {
            s_hist[b]++;
        }
        else
        {
            // 饱和：不再自增，按**不同桶**去重计数（位图保证只计一次）
            const uint32_t w = b >> 5;
            const uint32_t bit = 1u << (b & 31u);
            if ((s_sat_mask[w] & bit) == 0u)
            {
                s_sat_mask[w] |= bit;
                s_sat_buckets++;
            }
        }
    }

    // ---- 计数饱和（uint32 @1 kHz ≈ 49.7 天；2.5 h 窗口无风险）----
    if (s_n != 0xFFFFFFFFu) { s_n++; }
    else { s_n_saturated = 1; }

    s_sum_us += (uint64_t)dt_us;

    if (s_min_us == 0u || dt_us < s_min_us) { s_min_us = dt_us; }
    if (dt_us > s_max_us) { s_max_us = dt_us; }
#endif
}

// =====================================================
// 读取快照（任意任务上下文；单临界区整体拷贝）
// =====================================================
bool loop_stats_get(LoopStatsSnapshot &out)
{
    memset(&out, 0, sizeof(out));
    out.magic = loopstat::MAGIC;
    out.bucket_count = loopstat::BUCKET_COUNT;
    out.bucket_width_us = loopstat::BUCKET_WIDTH_US;
    out.enabled = (GF_LOOP_STATS_ENABLED) ? 1u : 0u;

#if GF_LOOP_STATS_ENABLED
    const int64_t t0 = ls_now_us();

    LS_CRIT_ENTER();
    out.available    = s_available;
    out.over_cnt     = s_over;
    out.anomaly_cnt  = s_anomaly;
    out.n            = s_n;
    out.min_us       = s_min_us;
    out.max_us       = s_max_us;
    out.sat_buckets  = s_sat_buckets;
    out.n_saturated  = s_n_saturated;
    out.sum_us       = s_sum_us;
    if (s_available)
    {
        memcpy(out.hist, s_hist, sizeof(s_hist));   // 512 B
        out.elapsed_us = (uint64_t)(t0 - s_start_us);
        s_snap_count++;
    }
    out.snap_count = s_snap_count;
    LS_CRIT_EXIT();

    // 读路径开销自测（目标板取证项）：含临界区进出 + 512 B 拷贝
    {
        const uint32_t dur = (uint32_t)(ls_now_us() - t0);
        if (dur > s_crit_max_us) { s_crit_max_us = dur; }
        out.snap_crit_max_us = s_crit_max_us;
    }

    return out.available != 0u;
#else
    return false;
#endif
}

// =====================================================
// 分位 / 差分（纯函数）
// =====================================================
bool loop_stats_compute(const LoopStatsSnapshot &base,
                        const LoopStatsSnapshot &top,
                        LoopStatsQuantiles &out)
{
    memset(&out, 0, sizeof(out));

    // ---- 1. 几何 / 版本校验 ----
    const bool base_empty = (base.magic == loopstat::MAGIC_EMPTY);
    if (top.magic != loopstat::MAGIC ||
        top.bucket_count != loopstat::BUCKET_COUNT ||
        top.bucket_width_us != loopstat::BUCKET_WIDTH_US)
    {
        out.invalid_bits = loopstat::INVALID_GEOMETRY;
        return false;
    }
    if (!base_empty &&
        (base.magic != loopstat::MAGIC ||
         base.bucket_count != top.bucket_count ||
         base.bucket_width_us != top.bucket_width_us))
    {
        out.invalid_bits = loopstat::INVALID_GEOMETRY;
        return false;
    }
    if (!top.available)
    {
        out.invalid_bits = loopstat::INVALID_NOT_AVAILABLE;
        return false;
    }

    // ---- 2. 差分（含回退检测）----
    uint16_t bits = 0;
    uint32_t d_n = 0, d_over = 0, d_anom = 0, d_sat = 0;
    uint64_t d_sum = 0;

    if (base_empty)
    {
        d_n = top.n;
        d_over = top.over_cnt;
        d_anom = top.anomaly_cnt;
        d_sat = top.sat_buckets;
        d_sum = top.sum_us;
    }
    else
    {
        if (top.n < base.n || top.over_cnt < base.over_cnt ||
            top.anomaly_cnt < base.anomaly_cnt || top.sum_us < base.sum_us ||
            top.sat_buckets < base.sat_buckets)
        {
            bits |= loopstat::INVALID_REGRESSION;
        }
        d_n = (top.n >= base.n) ? (top.n - base.n) : 0u;
        d_over = (top.over_cnt >= base.over_cnt) ? (top.over_cnt - base.over_cnt) : 0u;
        d_anom = (top.anomaly_cnt >= base.anomaly_cnt) ? (top.anomaly_cnt - base.anomaly_cnt) : 0u;
        d_sat = (top.sat_buckets >= base.sat_buckets) ? (top.sat_buckets - base.sat_buckets) : 0u;
        d_sum = (top.sum_us >= base.sum_us) ? (top.sum_us - base.sum_us) : 0ull;
    }

    out.n = d_n;
    out.over = d_over;
    out.anomaly = d_anom;
    out.sum_us = d_sum;
    out.min_us_uptime = top.min_us;
    out.max_us_uptime = top.max_us;
    out.mean_us = (d_n != 0u) ? (uint32_t)(d_sum / (uint64_t)d_n) : 0u;

    // ---- 3. 饱和（窗口内新增即判 P99 不可信）----
    if (d_sat != 0u || top.n_saturated != 0u)
    {
        bits |= loopstat::INVALID_SATURATION;
        if (top.n_saturated != 0u) { bits |= loopstat::INVALID_N_SATURATED; }
    }

    if (d_n < loopstat::MIN_SAMPLES) { bits |= loopstat::INVALID_INSUFFICIENT; }
    if (d_anom != 0u && (uint64_t)d_anom > (uint64_t)d_n) { bits |= loopstat::INVALID_ANOMALY_DOMINANT; }

    // ---- 4. 分位（单趟累加，不分配临时数组）----
    // rank = ceil(p * N)，用整数运算避免浮点
    const uint64_t rank50 = ((uint64_t)d_n * 50ull + 99ull) / 100ull;
    const uint64_t rank99 = ((uint64_t)d_n * 99ull + 99ull) / 100ull;

    uint64_t cum = 0;
    bool got50 = false, got99 = false;
    uint32_t b50 = 0, b99 = 0;

    // ★ 必须**遍历全部桶**：`cum` 的终值要用于 "Δn == ΣΔhist + Δover" 恒等式校验，
    //   提前 break 会让恒等式误判为"计数回退"（曾被 T9 端到端用例捕获）。
    for (uint32_t i = 0; i < loopstat::BUCKET_COUNT; ++i)
    {
        const uint32_t v = top.hist[i];
        const uint32_t b = base_empty ? 0u : base.hist[i];
        if (!base_empty && v < b)
        {
            bits |= loopstat::INVALID_REGRESSION;
            continue;
        }
        cum += (uint64_t)(v - b);

        if (d_n == 0u) { continue; }
        if (!got50 && cum >= rank50) { b50 = i; got50 = true; }
        if (!got99 && cum >= rank99) { b99 = i; got99 = true; }
    }

    // ---- 5. 恒等式：Δn == ΣΔhist + Δover（仅在无饱和时成立）----
    if ((bits & loopstat::INVALID_SATURATION) == 0u && top.n_saturated == 0u)
    {
        if ((uint64_t)d_n != cum + (uint64_t)d_over)
        {
            bits |= loopstat::INVALID_REGRESSION;
        }
    }

    out.invalid_bits = bits;

    // d_n == 0 时不做分位判定（否则会被误读为"P99 落在 over 区间"），
    // 可信性已由 INSUFFICIENT 位否决
    if (d_n != 0u)
    {
        if (!got50) { out.p50_in_overflow = 1; }
        else
        {
            out.p50_bucket = b50;
            out.p50_low_us = b50 * loopstat::BUCKET_WIDTH_US;
            out.p50_high_us = (b50 + 1u) * loopstat::BUCKET_WIDTH_US;
        }

        if (!got99) { out.p99_in_overflow = 1; }
        else
        {
            out.p99_bucket = b99;
            out.p99_low_us = b99 * loopstat::BUCKET_WIDTH_US;
            out.p99_high_us = (b99 + 1u) * loopstat::BUCKET_WIDTH_US;
            // ★ 20 ms = 100 × 200 µs 恰在桶边界 ⇒ 该判定**精确**
            out.p99_lt_gate = (out.p99_low_us < loopstat::GATE_P99_US) ? 1u : 0u;
        }
    }

    out.valid = (bits == 0u && out.p99_in_overflow == 0u && out.p50_in_overflow == 0u) ? 1u : 0u;
    return true;
}

// =====================================================
// 宿主机单测注入钩子（生产构建中不编译）
// =====================================================
#if defined(LOOP_STATS_HOST_TEST) && GF_LOOP_STATS_ENABLED
void loop_stats_host_force_bucket_full(uint32_t bucket)
{
    if (bucket < loopstat::BUCKET_COUNT) { s_hist[bucket] = 0xFFFFFFFFu; }
}
void loop_stats_host_force_n_saturated()
{
    s_n = 0xFFFFFFFFu;
    s_n_saturated = 1;
}
uint32_t loop_stats_host_n() { return s_n; }
void loop_stats_host_reset() { loop_stats_init(); }
#endif
