#pragma once
// =====================================================
// LoopStats —— loop() 周期耗时统计（S-3）
//
// 目的：为正式 G-4 门禁提供 `loop()` 单轮耗时 P50/P99 的**观测能力**。
//
// -----------------------------------------------------
// 设计要点（详见 docs/architecture/S-3-Loop-Timing-...md 与
//            docs/architecture/S-3-Implementation-and-Local-Acceptance-Report.md）
// -----------------------------------------------------
// 1. 采样点：**只有 loop() 入口一处**。样本 = 相邻两次进入 loop() 的时间差，
//    即"上一轮完整周期"，**包含调度与抢占影响**（无节流器时执行时间≈周期）。
// 2. 计时源：`esp_timer_get_time()`（int64 µs，**不回绕**）。
//    ★ 禁用 `micros()`：其底层为 uint32 µs，**71.58 min 回绕**，
//      2 h 门禁窗口必然溢出 ⇒ 差分变成负值/巨值。
// 3. 存储：**编译期定容、纯静态、内部 RAM**。
//    ★ 故意不用 PSRAM：PSRAM 经 cache，可能产生数十 µs 抖动，
//      会污染被测对象本身。
// 4. 热路径：**无锁**（单写者 = loopTask）、**零输出**（不 Serial / 不 log_emit /
//    不发 MQTT）、**无动态分配**。这是硬约束 —— 一行 100 B 的串口打印在
//    115200 8N1 下 ≈ 8.7 ms，足以单独制造"假 P99 长尾"。
// 5. 读路径：**单临界区整体拷贝** POD（576 B —— 经编译器核实 `sizeof(LoopStatsSnapshot)==576`），
//    预算 ≤ **15 µs**；
//    ★ 该预算原为 10 µs，`2026-10-20 03:05` 经 `R3` 裁决修订为 **15 µs**
//      （目标板实测临界区 **12 µs** ⇒ 原预算下"超标 20%"，新预算下"预算内"）。
//      ★ 措辞纪律：12 µs 只能写"**在 15 µs 预算内**"，**不得**写"达标 / PASS"。
//      依据：`docs/architecture/S-3-Spec-Revision-R3.md` · `S-3-Flash-Verification-and-Design-Goal-Assessment.md` §5.1
//    并在同一次调用内自测该临界区耗时（`snap_crit_max_us`，供目标板实测取证）。
// 6. 无写命令：**不提供 reset**。窗口统计由调用方做"两次快照差分"
//    （`loop_stats_compute(base, top, out)`）；单次查询返回的是
//    **自首样本起的累积口径**（`scope = "uptime"`）。
//
// -----------------------------------------------------
// 直方图参数（为什么是 128 × 200 µs）
// -----------------------------------------------------
//   量程 = 128 × 200 µs = **25.6 ms**，`over` 桶 = dt ≥ 25.6 ms。
//   ★ 门禁阈值 **20 ms = 100 × 200 µs，恰好落在桶边界上**
//     ⇒ bucket ≤ 99 ⟺ P99 < 20 ms（**判定精确，无桶内插值歧义**）。
//   ★ 桶类型用 **uint32** 而非 uint16：1 kHz 下 2 h ≈ 7.2e6 样本，
//     单桶（如 P50 所在桶）远超 65535 ⇒ uint16 会在正式窗口内**提前饱和**
//     并使 P99 失真。uint32 上限 4.29e9 ≈ 49.7 天 @1 kHz，覆盖 2.5 h 窗口有余。
//     仍保留饱和检测（`sat_buckets` / `n_saturated`）：一旦发生，**拒绝输出 P99**。
//   ★ ≥ 25.6 ms 的样本进 `over_cnt`，但 `max_us` 始终记录**精确最大值**
//     ⇒ 溢出区间也永不只报 "≥25.6 ms"。
// =====================================================

#include <stdint.h>

// 编译期门控：=0 时 tick/get 为空实现（零开销），用于纯净对照构建（S-3 §R1）
#ifndef GF_LOOP_STATS_ENABLED
#define GF_LOOP_STATS_ENABLED 1
#endif

namespace loopstat
{
    // ---- 直方图几何（改这些常量必须同步复核 20 ms 是否仍在桶边界）----
    static constexpr uint32_t BUCKET_COUNT = 128;
    static constexpr uint32_t BUCKET_WIDTH_US = 200;
    static constexpr uint32_t RANGE_US = BUCKET_COUNT * BUCKET_WIDTH_US; // 25 600 µs
    static constexpr uint32_t OVER_US = RANGE_US;                        // ≥ 此值进 over
    // 异常样本上界：dt ≤ 0 或 dt > 60 s ⇒ 非物理样本，计入 anomaly 且**不**入统计
    static constexpr uint32_t ANOMALY_MAX_US = 60000000u;
    // 分位可判读的最小样本数（P99 尾部 ≥100 个样本）
    static constexpr uint32_t MIN_SAMPLES = 10000;
    // 结构版本（快照兼容性校验）
    static constexpr uint32_t MAGIC = 0x4C535431u; // 'LST1'
    // 门禁绝对阈值（BT-1 Plan §6.2）
    static constexpr uint32_t GATE_P99_US = 20000;
    // 空基线的 magic：`loop_stats_compute()` 允许 base.magic==0 表示"自 boot 起"
    static constexpr uint32_t MAGIC_EMPTY = 0u;
}

// =====================================================
// 快照（POD，可整体拷贝；576 B ⇒ 可上栈，< 1 KB 铁律）
// =====================================================
struct LoopStatsSnapshot
{
    uint32_t magic;                          // loopstat::MAGIC / MAGIC_EMPTY
    uint32_t bucket_count;                   // 拷贝时校验几何一致性
    uint32_t bucket_width_us;
    uint32_t hist[loopstat::BUCKET_COUNT];   // 512 B：累积桶计数（uint32，见文件头）
    uint32_t over_cnt;                       // dt ≥ RANGE_US 的样本数（已含在 n 内）
    uint32_t anomaly_cnt;                    // dt ≤ 0 或 dt > 60 s 的样本数（**不**含在 n 内）
    uint32_t n;                              // 有效样本数（含 over）
    uint32_t min_us;                         // 累积最小值（未饱和前精确）
    uint32_t max_us;                         // 累积最大值（**精确**，不受直方图分辨率限制）
    uint32_t sat_buckets;                    // 发生过饱和的**不同**桶数量
    uint8_t  available;                      // 1 = 已建立首个样本，统计可读
    uint8_t  enabled;                        // 编译期开关回显
    uint8_t  n_saturated;                    // 1 = n 已达 uint32 上限（此后 n 不再增长）
    uint8_t  reserved0;
    uint64_t sum_us;                         // 累积样本和（用于精确均值，**不用 P50 代替**）
    uint64_t elapsed_us;                     // 自首个样本起的墙钟时间（**uint64**，2 h 不溢出）
    uint32_t snap_crit_max_us;               // 观测到的读路径临界区最大耗时（目标板取证项）
    uint32_t snap_count;                     // 读路径被调用次数
};

// =====================================================
// 分位结果（由 `loop_stats_compute()` 产出；纯计算，不碰硬件）
// =====================================================
struct LoopStatsQuantiles
{
    uint8_t  valid;             // 1 ⇒ p50/p99 数值可信（invalid_bits==0 且非 p99_in_overflow）
    uint8_t  p99_in_overflow;   // 1 ⇒ P99 落在 over 区间（只能给**下界** + 精确 max）
    uint8_t  p50_in_overflow;   // 1 ⇒ P50 落在 over 区间（病态）
    uint8_t  p99_lt_gate;       // 1 ⇒ P99 < 20 ms（门禁绝对口径）；无桶结果时为 0
    uint16_t invalid_bits;      // 见 loopstat::INVALID_* 位定义
    uint16_t reserved1;
    uint32_t n;                 // 窗口内有效样本数（Δn，含 over）
    uint32_t over;              // 窗口内 over 样本数（Δover）
    uint32_t anomaly;           // 窗口内异常样本数（Δanomaly）
    uint32_t mean_us;           // **精确均值** = Δsum / Δn（不是 P50）
    uint32_t min_us_uptime;     // 累积最小（**非窗口值**，见 min_max_scope）
    uint32_t max_us_uptime;     // 累积最大（**非窗口值**）
    uint32_t p50_bucket;        // 分位所在桶下标
    uint32_t p99_bucket;
    uint32_t p50_low_us;        // 桶下界（含）
    uint32_t p50_high_us;       // 桶上界（不含）
    uint32_t p99_low_us;
    uint32_t p99_high_us;
    uint64_t sum_us;            // 窗口内样本和（Δsum）
};

namespace loopstat
{
    // invalid_bits 定义（可叠加）
    static constexpr uint16_t INVALID_NOT_AVAILABLE   = 1u << 0;  // 尚无首样本 / 未启用
    static constexpr uint16_t INVALID_INSUFFICIENT    = 1u << 1;  // Δn < MIN_SAMPLES
    static constexpr uint16_t INVALID_SATURATION      = 1u << 2;  // 窗口内出现桶饱和 ⇒ P99 不可信
    static constexpr uint16_t INVALID_N_SATURATED     = 1u << 3;  // n 已饱和
    static constexpr uint16_t INVALID_GEOMETRY        = 1u << 4;  // 快照几何/版本不匹配
    static constexpr uint16_t INVALID_REGRESSION      = 1u << 5;  // 计数回退或恒等式不成立
    static constexpr uint16_t INVALID_ANOMALY_DOMINANT = 1u << 6; // 异常样本多于有效样本
}

// =====================================================
// API
// =====================================================

// 初始化（setup 末尾调用一次）。幂等：清空全部统计并丢弃首样本状态。
void loop_stats_init();

// 采样（**必须是 loop() 的第一行**）。无锁、零输出、O(1)。
void loop_stats_tick();

// 读取快照（可在任意任务上下文调用；内部单临界区整体拷贝）。
// 返回 true = 统计可用；false = 尚无首样本或编译期关闭（此时 out 已清零，
// 调用方**不得**把 false 当作"全部为 0"解读）。
bool loop_stats_get(LoopStatsSnapshot &out);

// 分位/差分计算（纯函数，无副作用、无硬件依赖，可在宿主机单测）。
//   base = 窗口起点快照（magic 可为 MAGIC_EMPTY 表示"自 boot 起"）
//   top  = 窗口终点快照
// 返回 true = 结构可解析（不代表分位可信，可信性看 out.valid / invalid_bits）。
bool loop_stats_compute(const LoopStatsSnapshot &base,
                        const LoopStatsSnapshot &top,
                        LoopStatsQuantiles &out);

// -----------------------------------------------------
// 仅宿主机单测可用的注入钩子（生产构建中不存在）
// -----------------------------------------------------
#if defined(LOOP_STATS_HOST_TEST)
// 把指定桶预置为饱和值（不置 sat 位）——用于验证"饱和只计一次"的逻辑
void loop_stats_host_force_bucket_full(uint32_t bucket);
// 把 n 预置为 uint32 上限
void loop_stats_host_force_n_saturated();
// 直接读内部 n（便于断言）
uint32_t loop_stats_host_n();
// 清空内部状态但**不**重置 n_saturated/几何（等价 init 的严格版）
void loop_stats_host_reset();
#endif

// 结构体积预算（static_assert 在 .cpp 中另有一条针对 1 KB 栈铁律的断言）
static_assert(sizeof(LoopStatsSnapshot) <= 1024,
              "LoopStatsSnapshot 必须 <=1024B：它会被放在命令处理栈帧上（1 KB 栈铁律）");
