// =====================================================
// MemWatch 实现（`G4-R4`）
//
// 线程约定：
//   · 采样只在 `memwatch_tick()`（loop 上下文）里做
//   · 分配计数可能来自任意上下文（setup / loop / 回调）⇒ **短临界区**保护
//   · `memwatch_get()` 在**单个**临界区内整体拷贝，保证字段自洽（不撕裂）
//
// 铁律：
//   · 不产生 Log（避免递归）、不分配堆、不阻塞、不串口输出
//   · 不改变任何模块的分配语义（见头文件说明）
// =====================================================

#include "services/mem_watch.h"

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <string.h>

// =====================================================
// 内部状态（全部静态定长；**零堆分配**）
// =====================================================

struct MemWatchState
{
    uint8_t  inited;
    uint32_t boot_ms;
    uint32_t last_sample_ms;
    uint8_t  n;                     // 已入环样本数（≤ MEMWATCH_RING_SAMPLES）
    uint8_t  head;                  // 下一个写入下标
    uint32_t warmup_done_ms;        // 预热结束时刻（0 = 尚未结束）
    MemDriftSample ring[MEMWATCH_RING_SAMPLES];
    MemWatchAllocStats alloc;

    // ---- H-5：深栈探测结果（0 = 未验证；1 = 已确认字节；2 = 已确认字数）----
    uint8_t  probe_unit;
    uint32_t probe_delta;
    uint32_t probe_depth;
};

static MemWatchState s_mw;
static portMUX_TYPE  s_mw_mux = portMUX_INITIALIZER_UNLOCKED;

// 采样环的**线性副本**（`memwatch_get()` 在临界区内 memcpy 到栈上，
// 再在锁外做纯计算 ⇒ 临界区时长与样本数无关）
static MemDriftSample s_snap[MEMWATCH_RING_SAMPLES];

// =====================================================
// H-4：分配节流
// =====================================================

// 记录一次"最终失败"。调用方必须**已**在临界区内或自行保证互斥。
static void alloc_note_fail_locked(uint8_t site, size_t bytes)
{
    const uint8_t s = (site < MEM_SITE_COUNT) ? site : (uint8_t)MEM_SITE_NONE;

    s_mw.alloc.final_fail++;
    s_mw.alloc.last_fail_site = s;
    s_mw.alloc.last_fail_size = (uint32_t)bytes;
    s_mw.alloc.last_fail_ms   = (uint32_t)millis();

    if (s != (uint8_t)MEM_SITE_NONE)
    {
        s_mw.alloc.site_final_fail[s]++;
    }
}

void *gf_alloc_psram_first(size_t bytes, uint8_t site, uint8_t *used_psram)
{
    // ---- PSRAM 尝试 ----
    void *p = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);

    if (p != nullptr)
    {
        portENTER_CRITICAL(&s_mw_mux);
        s_mw.alloc.calls++;
        s_mw.alloc.psram_ok++;
        portEXIT_CRITICAL(&s_mw_mux);

        if (used_psram != nullptr) *used_psram = 1u;
        return p;
    }

    // ---- 回退 DRAM ----
    void *q = heap_caps_malloc(bytes, MALLOC_CAP_8BIT);

    portENTER_CRITICAL(&s_mw_mux);
    s_mw.alloc.calls++;
    s_mw.alloc.psram_fail++;

    if (q != nullptr)
    {
        s_mw.alloc.dram_ok++;
    }
    else
    {
        s_mw.alloc.dram_fail++;
        alloc_note_fail_locked(site, bytes);
    }
    portEXIT_CRITICAL(&s_mw_mux);

    if (used_psram != nullptr) *used_psram = 0u;
    return q;
}

void *gf_alloc_psram_only(size_t bytes, uint8_t site)
{
    void *p = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);

    portENTER_CRITICAL(&s_mw_mux);
    s_mw.alloc.calls++;

    if (p != nullptr)
    {
        s_mw.alloc.psram_ok++;
    }
    else
    {
        s_mw.alloc.psram_fail++;
        alloc_note_fail_locked(site, bytes);
    }
    portEXIT_CRITICAL(&s_mw_mux);

    return p;
}

// =====================================================
// 生命周期
// =====================================================

void memwatch_init()
{
    portENTER_CRITICAL(&s_mw_mux);

    // 只清**本模块**状态；不动任何其他模块（避免"观测改变被观测对象"）
    memset(&s_mw.ring, 0, sizeof(s_mw.ring));
    s_mw.n = 0;
    s_mw.head = 0;
    s_mw.last_sample_ms = 0;
    s_mw.warmup_done_ms = 0;
    s_mw.boot_ms = (uint32_t)millis();
    s_mw.inited = 1;
    memset(&s_mw.alloc, 0, sizeof(s_mw.alloc));

    // H-5：探测结果**不跨 boot 重用**（口径验证属"本次固件/本次 boot"的证据）
    s_mw.probe_unit = 0;
    s_mw.probe_delta = 0;
    s_mw.probe_depth = 0;

    portEXIT_CRITICAL(&s_mw_mux);
}

// 采一个点（在 `memwatch_tick()` 内调用，已持锁）
static void sample_locked(uint32_t now)
{
    MemDriftSample &s = s_mw.ring[s_mw.head];

    s.t_ms          = now;
    s.dram_free     = (int32_t)ESP.getFreeHeap();
    s.dram_largest  = (int32_t)ESP.getMaxAllocHeap();
    s.psram_free    = (int32_t)ESP.getFreePsram();
    s.psram_largest = (int32_t)ESP.getMaxAllocPsram();

    s_mw.head = (uint8_t)((s_mw.head + 1u) % MEMWATCH_RING_SAMPLES);
    if (s_mw.n < MEMWATCH_RING_SAMPLES)
    {
        s_mw.n++;
    }
}

void memwatch_tick()
{
    const uint32_t now = (uint32_t)millis();

    // ---- 快速路径：未到采样时刻 ⇒ 一次比较即返回 ----
    //
    // 注意：`s_mw.last_sample_ms == 0` 表示"从未采样"，必须立即采首个点
    //（否则要等满 60 s 才有第一个样本）。
    if (s_mw.inited != 0 && s_mw.last_sample_ms != 0 &&
        (uint32_t)(now - s_mw.last_sample_ms) < MEMWATCH_SAMPLE_INTERVAL_MS)
    {
        return;
    }

    portENTER_CRITICAL(&s_mw_mux);

    if (s_mw.inited == 0)
    {
        portEXIT_CRITICAL(&s_mw_mux);
        return;
    }

    s_mw.last_sample_ms = (now == 0) ? 1u : now;   // 0 是"未采样"哨兵

    // 预热期内**不**入环（避免把启动期 TLS/首次分配尖峰当"基线"）
    if (s_mw.warmup_done_ms == 0)
    {
        if ((uint32_t)(now - s_mw.boot_ms) < MEMWATCH_WARMUP_MS)
        {
            portEXIT_CRITICAL(&s_mw_mux);
            return;
        }
        s_mw.warmup_done_ms = now;
    }

    sample_locked(now);

    portEXIT_CRITICAL(&s_mw_mux);
}

// =====================================================
// 只读快照
// =====================================================

// 把环形缓冲按时间顺序展开到 dst，返回有效样本数。
// 调用方已持锁。
static uint8_t flatten_locked(MemDriftSample *dst)
{
    const uint8_t n = s_mw.n;

    if (n < MEMWATCH_RING_SAMPLES)
    {
        // 未满：数据从 0 开始线性存放
        for (uint8_t i = 0u; i < n; i++)
        {
            dst[i] = s_mw.ring[i];
        }
        return n;
    }

    // 已满：从 head 开始回绕
    const uint8_t start = s_mw.head;

    for (uint8_t i = 0u; i < MEMWATCH_RING_SAMPLES; i++)
    {
        dst[i] = s_mw.ring[(uint8_t)((start + i) % MEMWATCH_RING_SAMPLES)];
    }
    return MEMWATCH_RING_SAMPLES;
}

bool memwatch_get(MemWatchInfo &out)
{
    memset(&out, 0, sizeof(out));

    MemWatchAllocStats alloc = {};
    uint8_t n = 0;
    uint8_t probe_unit = 0;        // 0 = 未验证 / 1 = bytes / 2 = words
    uint32_t probe_delta = 0;
    uint32_t probe_depth = 0;

    portENTER_CRITICAL(&s_mw_mux);
    const uint8_t inited = s_mw.inited;
    if (inited != 0)
    {
        n = flatten_locked(s_snap);
        alloc = s_mw.alloc;
        probe_unit  = s_mw.probe_unit;
        probe_delta = s_mw.probe_delta;
        probe_depth = s_mw.probe_depth;
    }
    portEXIT_CRITICAL(&s_mw_mux);

    if (inited == 0)
    {
        return false;
    }

    out.available = 1;
    out.uptime_ms = (uint32_t)millis();

    // ---- H-1 / H-2 ----
    out.dram_total     = (uint32_t)ESP.getHeapSize();
    out.dram_free      = (uint32_t)ESP.getFreeHeap();
    out.dram_largest   = (uint32_t)ESP.getMaxAllocHeap();
    out.dram_min_free  = (uint32_t)ESP.getMinFreeHeap();   // **启动以来**低水位
    out.psram_total    = (uint32_t)ESP.getPsramSize();
    out.psram_free     = (uint32_t)ESP.getFreePsram();
    out.psram_largest  = (uint32_t)ESP.getMaxAllocPsram();
    out.psram_min_free = (uint32_t)ESP.getMinFreePsram();

    // ---- H-3：漂移（锁外做纯计算）----
    out.window_samples = n;
    out.window_target_ms = MEMWATCH_MIN_SPAN_MS;

    if (n >= 2u)
    {
        out.window_span_ms = s_snap[n - 1u].t_ms - s_snap[0].t_ms;
    }

    out.dram_drift = memwatch_drift_calc(s_snap, n, MEMWATCH_MIN_SPAN_MS, 0u);

    // PSRAM 漂移：同一算法换字段（`use_psram=1`）⇒ **零额外栈/静态内存**
    if (out.psram_total > 0u && n >= 1u)
    {
        out.psram_drift = memwatch_drift_calc(s_snap, n, MEMWATCH_MIN_SPAN_MS, 1u);
    }

    // ---- H-4 ----
    out.alloc = alloc;

    // ---- H-5：loopTask 栈高水位 ----
    //
    // `uxTaskGetStackHighWaterMark(nullptr)` 取**当前任务** ⇒ loopTask（loop() 上下文）。
    // ★ 只报**原始值** + 一致性判定；"剩余字节"仅在**深栈探测**确认口径后给出
    //   （理由见 `mem_watch_calc.h`：`raw > stack ⇒ words` 的自动判据数学上不成立）。
    {
        const TaskHandle_t th = xTaskGetCurrentTaskHandle();
        const char *nm = (th != nullptr) ? pcTaskGetName(th) : nullptr;

        out.stack.available = (th != nullptr) ? 1u : 0u;

        if (nm != nullptr)
        {
            size_t len = strlen(nm);
            if (len > sizeof(out.stack.name) - 1u) len = sizeof(out.stack.name) - 1u;
            memcpy(out.stack.name, nm, len);
            out.stack.name[len] = '\0';
            out.stack.name_len = (uint8_t)len;
        }

        const uint32_t hwm_raw = (uint32_t)uxTaskGetStackHighWaterMark(nullptr);
        const uint32_t stack_bytes = MEMWATCH_LOOPTASK_STACK_BYTES;

        out.stack.stack_bytes = stack_bytes;
        out.stack.hwm_raw     = hwm_raw;
        out.stack.consistent  = memwatch_stack_consistent(hwm_raw, stack_bytes);
        out.stack.probe_delta = probe_delta;
        out.stack.probe_depth = probe_depth;

        // `probe_unit`：0 = 未验证；1 = 已确认为字节；2 = 已确认为字数
        out.stack.unit_verified = (probe_unit != 0u) ? 1u : 0u;
        out.stack.unit_is_bytes = (probe_unit == 1u) ? 1u : 0u;

        // 只有"口径已确认 + 长度自洽"才给出派生值；否则保持 0（= 未知，不是"用满了"）
        if (out.stack.unit_verified != 0u && out.stack.consistent != 0u)
        {
            if (out.stack.unit_is_bytes != 0u)
            {
                out.stack.hwm_bytes  = hwm_raw;
                out.stack.used_bytes = (hwm_raw <= stack_bytes) ? (stack_bytes - hwm_raw) : 0u;
            }
            else
            {
                // 字数口径：剩余字节 = raw × 4
                const uint32_t bytes = hwm_raw * 4u;
                out.stack.hwm_bytes  = bytes;
                out.stack.used_bytes = (bytes <= stack_bytes) ? (stack_bytes - bytes) : 0u;
            }
        }
    }

    return true;
}

// =====================================================
// H-5：栈单位实测（受控深栈探测）
//
// 每层吃掉 `PROBE_FRAME_BYTES` 字节的 volatile 缓冲；递归到目标深度后
// 用"最后一层写入 + 逐层读回"防止优化器把缓冲消除。
// =====================================================

#define PROBE_FRAME_BYTES 256u

static uint32_t probe_sink = 0;   // 静态接收点：确保写入不被优化掉

static void probe_recurse(uint32_t levels)
{
    volatile uint8_t buf[PROBE_FRAME_BYTES];

    // 写入首/末两个字节（跨页），既占实栈又阻止消除
    buf[0] = (uint8_t)(levels & 0xFFu);
    buf[PROBE_FRAME_BYTES - 1u] = (uint8_t)((levels >> 8) & 0xFFu);

    if (levels > 1u)
    {
        probe_recurse(levels - 1u);
    }

    // 读回并汇聚到静态变量（避免整段被删）
    probe_sink += (uint32_t)buf[0] + (uint32_t)buf[PROBE_FRAME_BYTES - 1u];
}

uint8_t memwatch_stack_probe(uint32_t depth_bytes)
{
    const TaskHandle_t th = xTaskGetCurrentTaskHandle();
    if (th == nullptr)
    {
        return 3u;
    }

    const uint32_t stack_bytes = MEMWATCH_LOOPTASK_STACK_BYTES;

    // ---- 安全钳位：最少 1 KB，最多 栈的一半（留足余量给中断与调用链）----
    uint32_t depth = depth_bytes;

    if (depth < 1024u)              depth = 1024u;
    if (depth > stack_bytes / 2u)   depth = stack_bytes / 2u;

    uint32_t levels = depth / PROBE_FRAME_BYTES;
    if (levels == 0u) levels = 1u;

    const uint32_t used_bytes = levels * PROBE_FRAME_BYTES;

    const uint32_t before = (uint32_t)uxTaskGetStackHighWaterMark(nullptr);

    probe_recurse(levels);

    const uint32_t after = (uint32_t)uxTaskGetStackHighWaterMark(nullptr);

    // 高水位是"历史最低"，探测只会让它**变小**（或不变）
    const uint32_t delta = (before > after) ? (before - after) : 0u;

    // 容差：取深度的 40%（递归帧对齐、中断栈叠加、编译器的额外寄存器保存）
    const uint32_t tol = used_bytes * 4u / 10u;

    const uint8_t verdict = memwatch_stack_delta_implies_bytes(delta, used_bytes, tol);

    portENTER_CRITICAL(&s_mw_mux);
    s_mw.probe_delta = delta;
    s_mw.probe_depth = used_bytes;
    s_mw.probe_unit  = (verdict == 1u) ? 1u : ((verdict == 0u) ? 2u : 0u);
    portEXIT_CRITICAL(&s_mw_mux);

    return verdict;
}

const char *memwatch_site_name(uint8_t site)
{
    switch (site)
    {
        case MEM_SITE_LOG_RING:         return "log.ring";
        case MEM_SITE_LOG_FLASH_BATCH:  return "log.flash_batch";
        case MEM_SITE_LOG_CLOUD_Q:      return "log.cloud_q";
        case MEM_SITE_LOG_CLOUD_BATCH:  return "log.cloud_batch";
        case MEM_SITE_LOG_CBOR:         return "log.cbor";
        case MEM_SITE_LOG_SEG_BUF:      return "log.seg_buf";
        case MEM_SITE_WF_INSTANCES:     return "workflow.instances";
        case MEM_SITE_WF_STEPS:         return "workflow.steps";
        case MEM_SITE_WF_STORAGE:       return "workflow.storage";
        case MEM_SITE_CAP_REGISTRY:     return "capability.registry";
        case MEM_SITE_HOMIE_DESC:       return "homie.desc";
        case MEM_SITE_CONFIG_BUF:       return "config.buf";
        default:                        return "unknown";
    }
}
