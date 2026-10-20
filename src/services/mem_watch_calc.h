// =====================================================
// MemWatch · 纯计算层（freestanding · 可主机编译）
//
// `G4-R4`（`2026-10-20`）—— 内存观测的**唯一判定逻辑**都放在这里：
//   · 漂移斜率（一阶最小二乘）
//   · 采样窗是否"封闭"
//   · 栈高水位的**单位**判定（bytes vs words）
//
// 为什么单独成头（与 `loop_stats_compute()` 同一惯例）：
//   本机 clang 无 MSVC libc ⇒ 无法原生链接 ESP-IDF 代码。
//   把这些函数写成**零依赖纯函数**，即可用
//   `test/mem_watch_host`（clang → wasm32 freestanding → node）真实执行断言，
//   而不是在报告里"推理"正确性。
//
// 纪律：
//   · 本文件**不**包含任何 Arduino / ESP-IDF 头 ⇒ 不引入硬件依赖
//   · 所有函数**无状态**、**无分配**、**无 I/O**、**可重入**
//   · 整数运算（无 float），避免 FPU 状态与舍入不确定性
// =====================================================

#ifndef MEM_WATCH_CALC_H
#define MEM_WATCH_CALC_H

#include <stdint.h>

// 采样点（每个采样只存这 5 个标量 ⇒ 32 点 = 96 B，可放栈上）
struct MemDriftSample
{
    uint32_t t_ms;        // 采样时刻（`millis()` 口径；回绕安全由调用方保证 32 位减法）
    int32_t  dram_free;   // 内部 RAM 当前空闲（B）
    int32_t  dram_largest;// 内部 RAM 最大连续块（B）
    int32_t  psram_free;  // PSRAM 当前空闲（B；无 PSRAM 恒 0）
    int32_t  psram_largest;
};

// 最少样本数：少于它不计算斜率（两点确定一条直线，但无抗噪能力）
#define MEMWATCH_MIN_SAMPLES      4u

// 采样环容量（32 点 × 20 B = 640 B）。定义在**本头**而非 mem_watch.h，
// 使主机探针无需引入 ESP-IDF 头即可使用同一常量（单一事实来源）。
#define MEMWATCH_RING_SAMPLES     32u

// 漂移计算的输出
struct MemDriftResult
{
    uint8_t valid;            // 0 = 样本不足 / 时间跨度不足 ⇒ 其余字段无意义
    uint8_t reason;           // 不可用原因码（见下方 MEMWATCH_REASON_*）
    uint32_t span_ms;         // 窗口实际跨度（末样本 - 首样本）
    uint8_t  samples;         // 参与计算的样本数
    int32_t  free_delta;      // 末样本 - 首样本（B）；负 = 流失
    int32_t  slope_per_hour;  // 一阶拟合斜率（B/h）；负 = 持续流失
    int32_t  largest_min;     // 窗口内最大连续块的最小值（B）—— 碎片硬指标
    int32_t  free_min;        // 窗口内空闲的最小值（B）
};

#define MEMWATCH_REASON_OK          0u
#define MEMWATCH_REASON_TOO_FEW     1u   // 样本数 < MEMWATCH_MIN_SAMPLES
#define MEMWATCH_REASON_SPAN_SHORT  2u   // 跨度 < 最小窗口（噪声主导，不判漂移）

/**
 * 计算漂移。
 *
 * 语义（**刻意保守**，避免把噪声读成"泄漏"）：
 *   · 样本数 < `MEMWATCH_MIN_SAMPLES`           ⇒ `valid=0, reason=TOO_FEW`
 *   · 跨度 < `min_span_ms`                       ⇒ `valid=0, reason=SPAN_SHORT`
 *   · 以上皆满足                                 ⇒ 一阶最小二乘拟合全部样本
 *
 * 为什么用**全部**样本而不是首末两点：单点抖动会让"两点斜率"剧烈摆动；
 * 最小二乘对噪声不敏感，且**样本数进结果**（`samples`），可审计。
 *
 * @param s          采样数组（时间**必须单调不减**）
 * @param n          样本数
 * @param min_span_ms 判定"窗口已建立"的最小跨度
 * @param use_psram  0 = 统计 `dram_*` 字段；1 = 统计 `psram_*` 字段
 *
 * ★ 同一函数服务 DRAM 与 PSRAM（用 `use_psram` 选字段，**不**复制数组）
 *   ⇒ 调用方零额外栈开销，也避免"两套算法漂移"。
 */
static inline MemDriftResult memwatch_drift_calc(const MemDriftSample *s,
                                                 uint8_t n,
                                                 uint32_t min_span_ms,
                                                 uint8_t use_psram)
{
    MemDriftResult r = { 0, MEMWATCH_REASON_TOO_FEW, 0, 0, 0, 0, 0, 0 };

    if (s == nullptr || n == 0)
    {
        return r;
    }

    const uint32_t span = s[n - 1u].t_ms - s[0].t_ms;
    r.span_ms = span;
    r.samples = n;

    // ---- 极值（无论斜率是否可用都给出：它们是**直接测量**，不是推断）----
    int32_t y0 = use_psram ? s[0].psram_free : s[0].dram_free;
    int32_t l0 = use_psram ? s[0].psram_largest : s[0].dram_largest;

    r.free_min    = y0;
    r.largest_min = l0;

    for (uint8_t i = 1u; i < n; i++)
    {
        const int32_t y = use_psram ? s[i].psram_free : s[i].dram_free;
        const int32_t l = use_psram ? s[i].psram_largest : s[i].dram_largest;

        if (y < r.free_min)    r.free_min    = y;
        if (l < r.largest_min) r.largest_min = l;
    }

    const int32_t y_last = use_psram ? s[n - 1u].psram_free : s[n - 1u].dram_free;

    r.free_delta = y_last - y0;

    if (n < MEMWATCH_MIN_SAMPLES)
    {
        r.reason = MEMWATCH_REASON_TOO_FEW;
        return r;
    }

    if (span < min_span_ms)
    {
        r.reason = MEMWATCH_REASON_SPAN_SHORT;
        return r;
    }

    // ---- 一阶最小二乘：slope = Σ(dt·dy) / Σ(dt²) ----
    //
    // dt 用**秒**（t_ms/1000）以压小量级；dy 用字节。
    // 上界：dt ≤ 32 min = 1920 s，dy ≤ 2e5 B，n ≤ 32
    //   ⇒ Σdt·dy ≤ 32·1920·2e5 ≈ 1.2e10 ⇒ **必须 int64**。
    int64_t sum_t = 0;
    int64_t sum_y = 0;

    for (uint8_t i = 0u; i < n; i++)
    {
        sum_t += (int64_t)(s[i].t_ms / 1000u);
        sum_y += (int64_t)(use_psram ? s[i].psram_free : s[i].dram_free);
    }

    const int64_t mean_t = sum_t / (int64_t)n;
    const int64_t mean_y = sum_y / (int64_t)n;

    int64_t num = 0;
    int64_t den = 0;

    for (uint8_t i = 0u; i < n; i++)
    {
        const int64_t dt = (int64_t)(s[i].t_ms / 1000u) - mean_t;
        const int64_t dy = (int64_t)(use_psram ? s[i].psram_free : s[i].dram_free) - mean_y;

        num += dt * dy;
        den += dt * dt;
    }

    if (den == 0)
    {
        // 全部样本同一时刻（理论上不可能）⇒ 不给斜率，但仍报极值
        r.reason = MEMWATCH_REASON_SPAN_SHORT;
        return r;
    }

    // B/s → B/h
    const int64_t slope_h = (num * 3600) / den;

    // 钳位（防异常值溢出 int32；真实值远小于此）
    if (slope_h > 1000000000LL)       r.slope_per_hour = 1000000000;
    else if (slope_h < -1000000000LL) r.slope_per_hour = -1000000000;
    else                              r.slope_per_hour = (int32_t)slope_h;

    r.valid = 1u;
    r.reason = MEMWATCH_REASON_OK;
    return r;
}

/**
 * 栈高水位**一致性**判定。
 *
 * 背景（必须留痕）：原生 FreeRTOS 的 `uxTaskGetStackHighWaterMark()` 返回
 *   **字数（StackType_t）**；ESP-IDF 按其"栈大小以字节计"的约定返回**字节**。
 *   两者相差 4 倍（ESP32 的 `StackType_t` = 4 B）。
 *   把 word 当 byte 读会**低估 4 倍** ⇒ 误判"栈充足"。
 *
 * ★ 为什么**不**做 `raw > stack ⇒ words，故 ×4` 的自动换算：
 *   该判据**在数学上永不成立**。若 API 返回字节，则 `raw ≤ stack`；
 *   若返回字数，则 `raw ≤ stack/4 < stack`。两种口径下 `raw` 都必然 ≤ `stack`
 *   ⇒ 无法用它区分口径。（这是本模块第一版设计缺陷，已在主机探针中暴露。）
 *
 * 因此本函数只做**必要条件**校验，把"单位"交给**实测**：
 *   · 返回 1 ⇒ `raw` 与"字节口径"自洽（不能反证，但也不矛盾）
 *   · 返回 0 ⇒ `raw > stack`，物理上不可能 ⇒ 口径/基准必有错 ⇒ **拒绝给出派生值**
 *
 * 真正的口径验证见 `memwatch_stack_delta_implies_bytes()`：
 *   在真机上让一个**已知深度**的递归吃掉约 `depth` 字节后再读高水位，
 *   看下降量是"约 depth"（字节口径）还是"约 depth/4"（字数口径）。
 *   ⇒ 这是**可执行**的证据，不是文档推断（见串口自测命令 `mw probe`）。
 */
static inline uint8_t memwatch_stack_consistent(uint32_t raw, uint32_t stack_bytes)
{
    return (stack_bytes == 0u) ? 1u : ((raw <= stack_bytes) ? 1u : 0u);
}

/**
 * "字数口径"是否**物理可能**。
 *
 * 若 `raw` 是字数，则剩余字节 = `raw × 4`；而任何时刻剩余字节 ≤ 栈总大小 ⇒
 *   `raw × 4 ≤ stack_bytes` 是"字数口径"的**必要条件**。
 *
 * ★ 这条判据是**有效的**（与上面那条不同）：
 *   · 返回 0 ⇒ 字数口径**不可能** ⇒ **必然是字节口径**（结论确定，无需实测）
 *   · 返回 1 ⇒ 两种口径都可能 ⇒ **有歧义** ⇒ 交给 `mw probe` 实测
 *
 * 真机样例（`2026-10-20` · `loopTask`）：`raw=9724`，`stack=16384`
 *   ⇒ `9724 × 4 = 38 896 > 16 384` ⇒ 字数口径不可能 ⇒ **字节口径确定**。
 */
static inline uint8_t memwatch_stack_words_possible(uint32_t raw, uint32_t stack_bytes)
{
    if (stack_bytes == 0u) return 1u;                 // 无基准 ⇒ 不排除任何可能
    return (raw <= (stack_bytes / 4u)) ? 1u : 0u;
}

/**
 * 由"**双步**深栈探测"的高水位差反推口径。
 *
 * ★ 为什么必须"双步"（第一版单步模型因此失败，真机实测暴露）：
 *   高水位记的是"历史最深点"。单步探测的下降量 =
 *     `depth − (当前深度 − 历史最深深度)`，只有当探测确实刷新了历史最深点时
 *     才等于 `depth` —— 否则为 0 或部分值。
 *   实测：`mw probe 4096` ⇒ delta=0（未刷新历史低点）；`mw probe 8192` ⇒ delta=3840
 *   （既非 8192 也非 2048）⇒ 单步模型**不可用**。
 *
 *   双步则恒定：先探 `d1`（刷新低点），再探 `d2 = d1 + step`。
 *   第二探必然比第一探再深 `step` ⇒ `hwm(d1) − hwm(d2) == step`（字节口径）
 *   或 `step/4`（字数口径），**与当前深度、历史低点都无关**。
 *
 * @param delta_raw 两次探测高水位之差（`hwm(d1) − hwm(d2)`，原始单位）
 * @param step_bytes 两次探测的深度增量（B）
 * @param tol_bytes  容差（建议 = step 的 25%~40%）
 *
 * @returns 1 = 字节口径；0 = 字数口径；2 = 无法判定
 */
static inline uint8_t memwatch_stack_delta_implies_bytes(uint32_t delta_raw,
                                                         uint32_t step_bytes,
                                                         uint32_t tol_bytes)
{
    if (step_bytes == 0u) return 2u;
    if (delta_raw == 0u)  return 2u;

    const uint32_t lo = (step_bytes > tol_bytes) ? (step_bytes - tol_bytes) : 0u;
    const uint32_t hi = step_bytes + tol_bytes;

    if (delta_raw >= lo && delta_raw <= hi) return 1u;          // 字节口径

    const uint32_t q = step_bytes / 4u;
    const uint32_t tol_q = tol_bytes / 4u;
    const uint32_t qlo = (q > tol_q) ? (q - tol_q) : 0u;
    const uint32_t qhi = q + tol_q;

    if (q > 0u && delta_raw >= qlo && delta_raw <= qhi) return 0u;   // 字数口径

    return 2u;
}

#endif // MEM_WATCH_CALC_H
