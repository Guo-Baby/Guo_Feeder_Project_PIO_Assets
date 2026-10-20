// =====================================================
// MemWatch —— `G4-R4` 内存观测（H-1…H-5）
//
// 存在理由（一句话）：`G-4` 的 DRAM 判据长期只有一个"占用率百分比"，
// 而该百分比**口径不统一且空载即 77%**（见 `G4-R4-R5-Joint-Investigation-Report.md` §3.1）。
// 本模块不做判分，只提供**可直接测量**的量：
//
//   H-1  内部 RAM `largest_free_block`（最大连续块）—— 真实风险是**碎片**，不是百分比
//   H-2  当前 boot 的 `minimum_free`（启动以来低水位）
//   H-3  固定采样窗内的**稳态漂移**（一阶最小二乘斜率，B/h）
//   H-4  项目**显式分配点**的失败计数，分三级：
//          psram 尝试失败 / DRAM 回退失败 / 逻辑分配最终失败
//   H-5  `loopTask` 栈高水位（含**单位自证**：bytes vs words）
//
// ★ 门禁纪律（`G4-R4` 未裁决前的硬约束）：
//   本模块**不**产生 PASS/FAIL，**不**在代码里把任何候选阈值当正式门禁。
//   所有阈值字段一律标注 `candidate`，且仅用于"窗口是否建立"这类**结构性**判断。
//
// ★ 覆盖边界（**不得**被表述为"系统无分配失败"）：
//   H-4 只统计**显式**调用 `gf_alloc_psram_first()` / `gf_alloc_psram_only()` 的点。
//   以下路径**不在**覆盖范围内，且不打算在本阶段覆盖：
//     · Arduino `String`（`malloc`/`realloc`，全仓 498 处）
//     · 第三方库内部（含 mbedTLS、LittleFS 缓存、ArduinoJson）
//     · FreeRTOS / IDF 自身分配
//   ⇒ 报告与文档只能写"**已登记显式分配点**内无失败"。
//
// ★ 实时性：`memwatch_tick()` 每 `MEMWATCH_SAMPLE_INTERVAL_MS` 才真正采样一次，
//   其余轮次仅一次 32 位取模比较；采样本身是两次 `heap_caps_get_*`（只读）。
//   无动态分配、无阻塞、无串口输出。
// =====================================================

#ifndef MEM_WATCH_H
#define MEM_WATCH_H

#include <stdint.h>
#include <stddef.h>
#include "services/mem_watch_calc.h"

// ---- 采样参数（**候选配置**，非门禁）----
#define MEMWATCH_SAMPLE_INTERVAL_MS   60000u   // 60 s 一个采样点
// `MEMWATCH_RING_SAMPLES`（= 32）定义在 `mem_watch_calc.h`（单一事实来源）
#define MEMWATCH_WARMUP_MS            300000u  // 5 min 预热（跳过启动期 TLS/分配尖峰）
#define MEMWATCH_MIN_SPAN_MS          1800000u // 30 min ⇒ 早于此不判"稳态漂移"

// ---- loopTask 栈大小（H-5 换算基准）----
//
// ★ 不硬编码：直接取 `ARDUINO_LOOP_STACK_SIZE`（`platformio.ini` 已设为 16384）。
//   历史留痕：本项目 loopTask 默认 8 KB 会溢出（Stack canary watchpoint），
//   现已显式调到 16 KB ⇒ 若此处写死 8192，`used_bytes` 会被**低估一半**。
//   这正是"单位/基准错误导致不欺骗失败"的典型点，故用宏对齐而非字面量。
#ifndef MEMWATCH_LOOPTASK_STACK_BYTES
#  ifdef ARDUINO_LOOP_STACK_SIZE
#    define MEMWATCH_LOOPTASK_STACK_BYTES ((uint32_t)ARDUINO_LOOP_STACK_SIZE)
#  else
#    warning "ARDUINO_LOOP_STACK_SIZE 未定义，H-5 按 8192 B 假设（可能与实际不符）"
#    define MEMWATCH_LOOPTASK_STACK_BYTES 8192u
#  endif
#endif

// ---- 显式分配点登记（H-4 的覆盖清单；新增点**必须**在此登记）----
enum MemAllocSite : uint8_t
{
    MEM_SITE_NONE = 0,
    MEM_SITE_LOG_RING = 1,          // log_manager: RAM 环（64 槽）
    MEM_SITE_LOG_FLASH_BATCH = 2,   // log_manager: Flash 批量写缓冲
    MEM_SITE_LOG_CLOUD_Q = 3,       // log_manager: 云待发队列（128 槽）
    MEM_SITE_LOG_CLOUD_BATCH = 4,   // log_manager: 组批副本
    MEM_SITE_LOG_CBOR = 5,          // log_manager: CBOR 编码缓冲
    MEM_SITE_LOG_SEG_BUF = 6,       // log_manager: 段扫描缓冲
    MEM_SITE_WF_INSTANCES = 7,      // workflow: trigger/action 实例数组
    MEM_SITE_WF_STEPS = 8,          // workflow: step 定义数组
    MEM_SITE_WF_STORAGE = 9,        // workflow_storage: 定义正文
    MEM_SITE_CAP_REGISTRY = 10,     // capability_registry: 描述符表
    MEM_SITE_HOMIE_DESC = 11,       // homie_bridge: 描述符文本
    MEM_SITE_CONFIG_BUF = 12,       // config_manager: JSON 读缓冲
    MEM_SITE_COUNT
};

struct MemWatchAllocStats
{
    uint32_t calls;              // 显式分配调用总次数
    uint32_t psram_ok;           // PSRAM 尝试成功
    uint32_t psram_fail;         // PSRAM 尝试失败（**不等于**最终失败）
    uint32_t dram_ok;            // DRAM 回退成功
    uint32_t dram_fail;          // DRAM 回退失败（⇒ 该次最终失败）
    uint32_t final_fail;         // 逻辑分配最终失败（psram_only 失败 + dram_fail）
    uint8_t  last_fail_site;     // 最近一次最终失败的登记点（MemAllocSite）
    uint32_t last_fail_size;     // 该次请求字节数
    uint32_t last_fail_ms;       // 发生时刻（millis）
    uint16_t site_final_fail[MEM_SITE_COUNT];   // 按登记点分解的最终失败数
};

struct MemWatchStackInfo
{
    uint8_t  available;          // 0 = 无法取得（任务不存在 / API 失败）
    uint8_t  consistent;         // 1 = `hwm_raw <= stack_bytes`（与字节口径自洽）
    uint8_t  unit_verified;      // 1 = 已由**深栈探测**确认口径（否则"未验证"）
    uint8_t  unit_is_bytes;      // 仅 `unit_verified=1` 时有意义
    uint32_t stack_bytes;        // 该任务栈总大小（B）
    uint32_t hwm_raw;            // API 原始返回值（**始终**给出，便于复核）
    uint32_t hwm_bytes;          // 剩余字节（**仅** `unit_verified && unit_is_bytes` 时填）
    uint32_t used_bytes;         // stack_bytes - hwm_bytes（同上，否则 0 = 未知）
    uint32_t probe_delta;        // 最近一次深栈探测的下降量（原始单位；0 = 未探测）
    uint32_t probe_depth;        // 该次探测的递归深度（B）
    uint8_t  name_len;           // 任务名长度（0 = 未知）
    char     name[16];           // 任务名（截断到 15 字符，便于核对"确实是 loopTask"）
};

struct MemWatchInfo
{
    uint8_t  available;          // 1 = 已初始化

    // ---- H-1 / H-2：即时 + 低水位 ----
    uint32_t dram_total;
    uint32_t dram_free;
    uint32_t dram_largest;       // H-1
    uint32_t dram_min_free;      // H-2（**启动以来**低水位，非窗口）
    uint32_t psram_total;
    uint32_t psram_free;
    uint32_t psram_largest;
    uint32_t psram_min_free;

    // ---- H-3：稳态漂移 ----
    uint8_t  window_samples;     // 已入环的有效样本数
    uint32_t window_span_ms;     // 首末样本跨度
    uint32_t window_target_ms;   // 目标窗口（候选配置）
    MemDriftResult dram_drift;   // 内部 RAM 漂移（含 valid / reason）
    MemDriftResult psram_drift;  // PSRAM 漂移

    // ---- H-4 ----
    MemWatchAllocStats alloc;

    // ---- H-5 ----
    MemWatchStackInfo  stack;

    uint32_t uptime_ms;
};

/** 初始化（setup 阶段调用一次；幂等）。不分配内存。 */
void memwatch_init();

/** 由 `loop()` 每轮调用（非阻塞；内部按 60 s 节流采样）。 */
void memwatch_tick();

/** 只读快照（单临界区整体拷贝，字段间自洽）。`false` = 未初始化。 */
bool memwatch_get(MemWatchInfo &out);

/** 登记点 → 稳定短名（用于 JSON 输出；未知点返回 "unknown"） */
const char *memwatch_site_name(uint8_t site);

// =====================================================
// H-5：栈单位**实测**验证（串口自测命令 `mw probe` 调用；**不**暴露给云端）
//
// 做法：在 loopTask 上下文里做一次**受控深度**递归（每帧 256 B volatile 缓冲），
//   读探测前后的 `uxTaskGetStackHighWaterMark()`，用下降量反推口径：
//     下降量 ≈ depth       ⇒ 字节口径
//     下降量 ≈ depth / 4   ⇒ 字数口径
//   结果缓存在模块内，`memwatch_get()` 以 `unit_verified` 反映。
//
// 安全约束（硬性）：
//   · `depth_bytes` 被钳到 `[1024, 栈大小/2]` ⇒ 绝不可能击穿栈
//   · 只在串口控制台调用，**不**在 loop()/命令路径调用
//   · 不分配堆、不阻塞、无输出（输出由调用方负责）
//
// @returns 1 = 字节口径；0 = 字数口径；2 = 无法判定；3 = 拒绝执行（参数越界/无任务）
// =====================================================
uint8_t memwatch_stack_probe(uint32_t depth_bytes);

// =====================================================
// H-4：显式分配节流
//
// ★ 行为与原写法**逐字等价**（同样的 caps 顺序、同样的返回指针），
//   只在失败/成功路径上多加一次计数（短临界区）。
//   ⇒ 接入本函数**不改变**任何模块的分配语义。
// =====================================================

/** PSRAM 优先、失败回退 DRAM（`MALLOC_CAP_8BIT`）。返回 nullptr = 最终失败。
 *
 * @param used_psram 可选出参：1 = 实际落在 PSRAM；0 = 回退到 DRAM。
 *                   （既有调用方用它填 `*_in_psram` 观测字段，语义与原先一致）
 */
void *gf_alloc_psram_first(size_t bytes, uint8_t site, uint8_t *used_psram = nullptr);

/** 仅 PSRAM（不允许回退）。返回 nullptr = 最终失败。 */
void *gf_alloc_psram_only(size_t bytes, uint8_t site);

#endif // MEM_WATCH_H
