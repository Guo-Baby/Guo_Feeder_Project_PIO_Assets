// =====================================================
// LogManager —— P1.2：Log Core / RAM Queue（实现）
//
// 设计依据（冻结，不得重新设计）：
//   docs/archive/log/LogManager-P1契约冻结0915.md
//   docs/archive/log/LogManager详细设计规划0915.md
//   src/log_events.h（P1.1 契约头）
//
// 并发模型：
//   · 生产者：任意 Task 上下文（loopTask / esp-mqtt / BLE callback ...）
//   · 消费者：仅 loopTask（本文件不创建任何 Task）
//   · 环索引 + 槽拷贝在临界区内完成（拷贝 128 B，量级 ~1 us）
//   · **ISR 上下文不支持**（需要时另加 portENTER_CRITICAL_ISR 版本）
//
// 副作用：
//   · P1.2 不写 Flash、不发 MQTT，只在 log_task() 内做 routing **决策与计数**
// =====================================================

#include "log/log_manager.h"

#include <Arduino.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <string.h>
#include <esp_heap_caps.h>

// P1.4：LogManager 不得直接调用 MQTT —— 一律经 CloudManager 的 Log API。
// 依赖方向：LogManager → CloudManager（单向，CloudManager 不反向依赖）
#include "cloud/cloud_manager.h"

// G4-R4 · H-4：显式分配点统一经 `gf_alloc_psram_first()` 记账
//（**行为逐字等价**：同样 PSRAM 优先、同样 MALLOC_CAP_8BIT 回退）
#include "services/mem_watch.h"

// P1.4：CBOR 批次编码器（独立头文件，可主机编译验证）
#include "log/log_cbor.h"

// P1.5：ACK / Retry / Offline 的纯决策逻辑（独立头文件，可主机编译验证）
#include "log/log_ack.h"
#include "log/log_meta_slot.h"
#include "log/log_meta_evidence.h"

// P3 · WP4-FW-LOG：Cloud Log Envelope Producer（独立模块，可主机编译验证）
//   —— /log 上行载荷由「原始 CBOR 批次」改为「JSON envelope（逐条，count=1）」
#include "log/log_envelope.h"

// envelope 的 device_id 必须与 topic 同源 ⇒ 取设备身份（只读调用）
#include "services/device_identity.h"

#include "services/time_manager.h"

// =====================================================
// 内部状态
// =====================================================

static LogRecord *s_ring = nullptr;   // 64 槽 × 128 B（PSRAM 优先）
static uint32_t   s_wr = 0;           // 写计数（单调递增）
static uint32_t   s_rd = 0;           // 读计数（单调递增）
static bool       s_ready = false;
static bool       s_in_psram = false;
static uint32_t   s_ring_bytes = 0;

// ---- P1.3 Flash Segment Ring 状态 ----
//
// 只由 loopTask 读写（log_init / log_task / 测试钩子都在 loop 上），
// 因此不需要 s_mux；但**不得**被其它任务直接访问（§35 单写者）。
static bool     s_flash_ready = false;

// 每 segment 元信息（16 个小整数 —— 属"小型控制状态"，留内部 RAM，§32）
//
// ★ 单一真相源仍是**段文件内容**；下面三个数组是**同一份扫描派生缓存**，
//   必须由同一组函数整体重建 / 整体维护（禁止任何一处单独修改）：
//     重建：flash_scan_all()           维护：flash_append_batch()
//     失效：flash_create_segment() / flash_drop_segment() / flash_drop_broken_segment()
//
// ★ 为什么需要 s_seg_last_seq（FIX-3b）：
//   `seq` 在 log_emit() 中**每条非 DEBUG 记录都消耗一个**（含 INFO），而 INFO
//   不落 Flash ⇒ Flash 段内的 seq **必然稀疏** ⇒ `first_seq + records - 1`
//   系统性偏小（偏小量 = 段内 INFO 空洞数）⇒ 会**过早回收含未确认记录的段**。
//   故末条 seq 必须**实测**，不得推导。
static uint32_t s_seg_first_seq[LOG_SEGMENT_COUNT];   // 0 = 无有效首条
static uint32_t s_seg_last_seq[LOG_SEGMENT_COUNT];    // 末槽记录的 seq（records==0 时 0）
static uint8_t  s_seg_records[LOG_SEGMENT_COUNT];     // 已验证的有效 record 数
static uint8_t  s_seg_present[LOG_SEGMENT_COUNT];     // 文件存在且头合法

static uint32_t s_oldest_segment = 0;
static uint32_t s_newest_segment = 0;
static uint32_t s_append_segment = 0;
static uint8_t  s_append_index = 0;
static uint8_t  s_valid_segment_count = 0;
static uint16_t s_flash_total_records = 0;

// Flash 批量工作缓冲（LOG_FLUSH_RECORDS × 128 B = 1 KB）
//
// §31 / §33：批量工作区优先 PSRAM，失败回退 DRAM。
// §30：一次 log_task() 最多执行**一个** Flash append 单元（= 一个 batch）。
static LogRecord *s_flash_batch = nullptr;
static bool       s_flash_batch_in_psram = false;
static uint32_t   s_flash_batch_bytes = 0;

// ---- seq / boot_seq（§13 / §15 / §16）----
//
// Boot 时从 /log/meta.bin 预留一个区间（LOG_SEQ_RESERVE = 256 个号）：
//     seq_base = meta.seq_reserved + 1
//     meta.seq_reserved += 256        ← 立即落盘
// 本次开机使用 seq_base .. seq_base+255；用尽后再预留下一段。
// **允许空洞，不允许重复**（未用掉的号直接废弃，不回收）。
static uint32_t s_boot_seq = 1;           // 本次开机序号
static uint32_t s_meta_reserved = 0;      // meta.seq_reserved（已预留高水位）
static uint32_t s_meta_corrupt = 0;       // meta.corrupt_count
static uint32_t s_seq_base = 0;           // 本区间首号
static uint32_t s_seq_limit = 0;          // 本区间末号
static uint32_t s_seq_last = 0;           // 最近一次分配出去的 seq
static bool     s_seq_reliable = true;    // false = 可能重复（§15）
static bool     s_seq_reserving = false;  // 预留进行中（避免并发重复写 meta）

// ---- `HD-3`·`A-1`（`2026-10-18`）：双槽元数据状态 ----
static int8_t   s_meta_slot = -1;         // 当前有效槽：0=A / 1=B / -1=未知
static uint32_t s_meta_slot_seq = 0;      // 当前有效槽的 slot_seq（选槽依据）
// true = **无法证明 boot_seq 安全** ⇒ 停止分配新 seq（fail-closed）。
// `HD-3`·`A-1` 修复后：该状态**跨重启固化**（NVS 标志 + `/log/quarantine.bin`），
// 不再因"第一次启动删掉了损坏证据"而退化（原 `A1-02`）。
static bool     s_seq_failclosed = false;
static bool     s_failclosed_logged = false;

// ---- `HD-3`·`A-1` 修复：持久证据（NVS）与隔离（`2026-10-18`）----
//
// 为什么必须放在 NVS：`LittleFS.begin(true, …)` 的 `formatOnFail` 会在挂载失败时
// **主动格式化**整个 LittleFS ⇒ 日志区（双槽 / legacy / 段）全部消失。
// NVS 是**独立分区**，不随该格式化丢失 ⇒ 它是唯一能区分
// 「真·首启」与「日志区被清空」的本地证据（原 `A1-01`）。
static const char *kEvNamespace = "guolog";   // NVS namespace（≤15 字符）
static const char *kEvKey       = "ev";       // 32 B 证据记录

static bool     s_evidence_ok      = false;   // NVS 证据可读
static uint32_t s_ev_boot_hw       = 0;       // NVS 里的 boot 高水位
static uint32_t s_ev_reserved_hw   = 0;       // NVS 里的 seq 高水位
static uint32_t s_ev_flags         = 0;       // bit0 隔离 / bit1 曾 provision
static uint32_t s_ev_gen           = 0;       // 写入代数
static uint32_t s_ev_corrupt       = 0;       // corrupt_count 快照
static uint8_t  s_ev_source        = 0xFFu;   // 本次恢复来源（LogFlashInfo 上报）

// 「本次 boot 才建立设备身份」= 真·首启的**唯一**合法信号（由 `main.cpp` 注入）。
// 默认 `false`（保守）：未注入时任何"无证据"都会转入隔离，而不是当作全新设备。
static bool     s_first_boot       = false;
// 是否**真的**被注入过（仅用于诊断：漏调用 ⇒ 新设备会被误判隔离，必须可见）
static bool     s_first_boot_set   = false;
static bool     s_qfile_present    = false;   // /log/quarantine.bin 存在

// 测试钩子：令下一次 meta_write 失败（一次性），用于验证 §15 / §41-4
static bool s_meta_fail_next = false;

// 测试钩子（`A1-06`）：分阶段注入。0=关；1=写槽①失败；2=写槽②失败；3=回读校验失败
static uint8_t s_meta_fail_stage = 0;

// 测试钩子：令接下来 N 次 Flash append 整批失败，用于验证 §22/§23 / F10
static uint8_t s_flash_fail_budget = 0;

// 本次扫描新增的损坏 segment 数（§18），由 seq_init() 累加进 meta.corrupt_count
static uint32_t s_scan_corrupt = 0;

static bool s_flush_requested = false;

static LogStats s_stats;

// 临界区：保护 s_wr / s_rd / s_ring 槽内容 / s_seq_last / s_stats / s_flush_requested
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

// =====================================================
// ★ 突发合并（Burst Coalescing）—— 仅 LogManager
//
// 目的：对"高频但非安全关键响应"的记录做窗口内折叠，
//       降低 Flash 段环与云队列的写入压力。
// 不变式：Σ LOG_P_COUNT == 真实发生次数（可上板核对）
//
// 线程约定：合并状态只在"发出该事件的上下文"与 log_task()（loop）
//           之间共享，所有读改都在 s_mux 内完成；任何 log_emit() 调用
//           都在临界区**之外**发生（避免自锁）。
// =====================================================

struct LogCoalesceTarget
{
    LogEventId id;
    LogLevel   level;
};

static const LogCoalesceTarget s_coalesce_targets[] = {
    // 重量异常"进入"：由 weight_refresh_error_state() 的 error_state 边沿产生。
    // 安全响应走 EventManager → DispenseGuard → valve_force_close()，
    // **完全不经过本模块** ⇒ 折叠日志不可能影响安全时序。
    { LOG_WEIGHT_ERROR_ENTER, LOG_LVL_WARN },

    // Phase 4：DispenseGuard 安全响应决策。
    //
    // 为什么必须合并：本埋点的宿主在 weight 异常回调内，而实测权重跳变期
    // 该回调可达 6~20 次/s（valve.cpp:454 实测）⇒ 不合并时 5s 内可达
    // 30~100 条 WARN，会同时压满 Flash 段环(496 条)与云队列(128 槽)。
    //
    // 为什么合并是安全的：本 EventId 只承载"决策已作出"这一**观测事实**，
    // 安全动作（valve_force_close() 的 GPIO 写入）在 callback 内**先于**本
    // log_emit 之前就已由控制流决定，且 log_emit 不参与任何分支判断 ⇒
    // 折叠日志不可能改变安全动作的次数与时序。
    //
    // 与 valve 侧的互补关系（非冗余）：LOG_VALVE_FORCE_CLOSE(0x0505) 带
    // 5s 日志门控 ⇒ 只记"发生过"；本 ID 的 Σ LOG_P_COUNT 记"发生了几次"。
    { LOG_DISPENSE_SAFETY_RESPONSE, LOG_LVL_WARN }
};

static const uint8_t s_coalesce_target_count =
    (uint8_t)(sizeof(s_coalesce_targets) / sizeof(s_coalesce_targets[0]));

static bool       s_coalesce_active      = false;  // 合并窗口是否活动
static uint32_t   s_coalesce_last_ms     = 0;      // 最近一条"被接受"记录的时刻
static uint16_t   s_coalesce_folded      = 0;      // 窗口内被折叠的发生次数
static LogEventId s_coalesce_event       = LOG_EVT_NONE;
static LogLevel   s_coalesce_level       = LOG_LVL_WARN;
static LogParamIn s_coalesce_params[LOG_MAX_PARAMS];
static uint8_t    s_coalesce_param_count = 0;

// 前置声明：汇总记录必须走与普通记录**完全相同**的编码 / 入环路径
static bool log_emit_internal(LogEventId event_id, LogLevel level,
                              const LogParamIn *params, uint8_t param_count,
                              bool allow_coalesce);

static bool log_coalesce_is_target(LogEventId id)
{
    for (uint8_t i = 0; i < s_coalesce_target_count; i++)
    {
        if (s_coalesce_targets[i].id == id)
        {
            return true;
        }
    }
    return false;
}

// 汇总记录：把窗口内被折叠的次数合并为一条（LOG_P_COUNT = 折叠次数）
static void log_coalesce_emit_summary(LogEventId event_id, LogLevel level,
                                      const LogParamIn *base, uint8_t base_n,
                                      uint16_t folded)
{
    if (base == nullptr || base_n == 0 ||
        base_n >= LOG_MAX_PARAMS || folded == 0)
    {
        return;
    }

    LogParamIn p[LOG_MAX_PARAMS];

    for (uint8_t i = 0; i < base_n; i++)
    {
        p[i] = base[i];
    }

    p[base_n] = log_arg_u32(LOG_P_COUNT, (uint32_t)folded);

    // allow_coalesce = false：汇总记录自身绝不能再被折叠
    (void)log_emit_internal(event_id, level, p, (uint8_t)(base_n + 1), false);
}

// 窗口到期 ⇒ 结清悬挂的折叠计数（由 log_task() 每轮调用，非阻塞、无 Flash I/O）
static void log_coalesce_tick()
{
    const uint32_t now_ms = (uint32_t)millis();

    LogParamIn cached[LOG_MAX_PARAMS];
    LogEventId ev     = LOG_EVT_NONE;
    LogLevel   lv     = LOG_LVL_WARN;
    uint8_t    base_n = 0;
    uint16_t   folded = 0;

    portENTER_CRITICAL(&s_mux);

    if (s_coalesce_active && s_coalesce_folded > 0 &&
        (uint32_t)(now_ms - s_coalesce_last_ms) >= LOG_COALESCE_WINDOW_MS)
    {
        ev     = s_coalesce_event;
        lv     = s_coalesce_level;
        base_n = s_coalesce_param_count;
        folded = s_coalesce_folded;

        if (base_n > 0)
        {
            memcpy(cached, s_coalesce_params,
                   (uint32_t)base_n * sizeof(LogParamIn));
        }

        s_coalesce_active      = false;
        s_coalesce_folded      = 0;
        s_coalesce_param_count = 0;
    }

    portEXIT_CRITICAL(&s_mux);

    if (base_n > 0 && folded > 0)
    {
        log_coalesce_emit_summary(ev, lv, cached, base_n, folded);
    }
}

// 在编码之前调用：
//   返回 true  = 本条已被折叠（不得进入 RAM 环、不占用 seq）
//   返回 false = 本条正常落地；out_p / out_n 为"已追加 LOG_P_COUNT"的最终参数
static bool log_coalesce_filter(LogEventId event_id, LogLevel level,
                                const LogParamIn *params, uint8_t param_count,
                                LogParamIn *out_p, uint8_t &out_n)
{
    out_n = param_count;

    if (!log_coalesce_is_target(event_id) || param_count >= LOG_MAX_PARAMS)
    {
        return false;
    }

    const uint32_t now_ms = (uint32_t)millis();

    LogParamIn cached[LOG_MAX_PARAMS];
    LogEventId ev     = LOG_EVT_NONE;
    LogLevel   lv     = LOG_LVL_WARN;
    uint8_t    base_n = 0;
    uint16_t   folded = 0;

    portENTER_CRITICAL(&s_mux);

    // ---- ① 窗口内重复发生 ⇒ 折叠（只累计计数，不产生记录）----
    if (s_coalesce_active &&
        (uint32_t)(now_ms - s_coalesce_last_ms) < LOG_COALESCE_WINDOW_MS)
    {
        if (params != nullptr && param_count > 0)
        {
            memcpy(s_coalesce_params, params,
                   (uint32_t)param_count * sizeof(LogParamIn));
            s_coalesce_param_count = param_count;
        }

        if (s_coalesce_folded < 0xFFFFu)
        {
            s_coalesce_folded++;
        }

        s_coalesce_event = event_id;
        s_coalesce_level = level;

        portEXIT_CRITICAL(&s_mux);
        return true;
    }

    // ---- ② 接受本条：先结清上一窗口的折叠计数（保证记录时间序）----
    if (s_coalesce_active && s_coalesce_folded > 0)
    {
        ev     = s_coalesce_event;
        lv     = s_coalesce_level;
        base_n = s_coalesce_param_count;
        folded = s_coalesce_folded;

        if (base_n > 0)
        {
            memcpy(cached, s_coalesce_params,
                   (uint32_t)base_n * sizeof(LogParamIn));
        }
    }

    s_coalesce_active      = true;
    s_coalesce_last_ms     = now_ms;
    s_coalesce_folded      = 0;
    s_coalesce_param_count = 0;
    s_coalesce_event       = event_id;
    s_coalesce_level       = level;

    portEXIT_CRITICAL(&s_mux);

    if (base_n > 0 && folded > 0)
    {
        log_coalesce_emit_summary(ev, lv, cached, base_n, folded);
    }

    // ---- ③ 放行本条，标注 LOG_P_COUNT = 1（本条代表 1 次真实发生）----
    for (uint8_t i = 0; i < param_count; i++)
    {
        out_p[i] = params[i];
    }

    out_p[param_count] = log_arg_u32(LOG_P_COUNT, 1u);
    out_n = (uint8_t)(param_count + 1);

    return false;
}


// =====================================================
// P1.4：Cloud Log Topic 状态
//
//   P1.4 只负责"把记录送出去"；ACK / 超时 / 重试 / 退避属 P1.5。
//   依赖方向：LogManager → CloudManager（单向注入回调，无循环依赖）
// =====================================================

// 云待发队列（环形；PSRAM 优先，失败回退 DRAM）
static LogRecord *s_cloud_q = nullptr;
static uint32_t   s_cloud_q_wr = 0;
static uint32_t   s_cloud_q_rd = 0;
static bool       s_cloud_q_in_psram = false;
static uint32_t   s_cloud_q_bytes = 0;
static uint32_t   s_cloud_q_last_seq = 0;   // 去重守卫：最近一次入队的 seq
static uint32_t   s_cloud_q_last_boot = 0;  // 去重守卫：与之配对的 boot_seq

// 批次缓冲（LOG_BATCH_MAX_RECORDS × 128 B）与 CBOR 输出缓冲
static LogRecord *s_cloud_batch = nullptr;
static uint8_t   *s_cloud_cbor = nullptr;
static bool       s_cloud_buf_in_psram = false;

// 在途批次
static bool     s_cloud_inflight = false;
static uint32_t s_cloud_tx_boot = 0;
static uint32_t s_cloud_tx_from = 0;
static uint32_t s_cloud_tx_to = 0;
static uint8_t  s_cloud_tx_count = 0;
static uint32_t s_cloud_cbor_len = 0;
static uint32_t s_cloud_last_tx_ms = 0;
static uint32_t s_cloud_acked_seq = 0;      // 已确认高水位（RAM；重启归 0）
static uint8_t  s_cloud_flags = 0;          // 在途批次的 flags（bit0 seq_reliable）

// ---- FIX-BT9：水位**语义分离**（两个标量，分工不同）----
//
//   s_cloud_acked_seq  云端确认过的**最大** seq（真实最大值）。只用于观测
//                      （串口打印 / stats），**不参与任何放行判定**。
//   s_cloud_gc_seq     **连续**可回收水位：保证 "任何 seq <= gc_seq 的记录都
//                      已被云端确认"。所有放行判定都用它：
//                        · log_ack_classify()  的重复/回退检测
//                        · log_ack_should_replay()      （能否跳过补发）
//                        · log_ack_segment_reclaimable()（能否回收段）
//                        · flash_count_unacked()        （淘汰记账口径）
//
//   为什么不合并成一个（BT-9 根因）：ACK 只证明"刚发出的这一批被持久化"，
//   不证明"比它小的都已持久化"。Boot 期 live 记录（seq 更大）先被 ACK 时，
//   单点高水位会跨过仍在 Flash 等待补发的旧记录 ⇒ 补发被跳过 + 段被误回收
//   ⇒ 违反 at-least-once（详见 log_ack.h 的 log_ack_gc_watermark 说明）。
//
//   两者关系恒为  gc_seq = log_ack_gc_watermark(acked_seq, gc_floor) <= acked_seq
static uint32_t s_cloud_gc_seq = 0;

// 尚未确认的 Flash backlog 的**最低 seq 下界**（0 = 无钳制）
//
//   设置点：cloud_replay_arm()（= 每次 arm 时 Flash 里现存记录的最低 first_seq）
//   清除点：cloud_poll() —— 补发 sweep 已完成 **且** 队列里已无"上一 Boot 的
//           记录"（它们构成队列前缀，见 cloud_queue_push 的说明）⇒ backlog
//           已全部投递（被 ACK / 放弃(已登记空洞) / 淘汰(已登记空洞)）
//   ⚠️ 清除条件里不能用 "队列已空"：持续有 live 记录时队列可能长期非空，
//      会把钳制永久留在低位 ⇒ 段永不回收 ⇒ 环压力反而丢数据。用"前缀已消费"
//      既安全（旧记录走光才放行）又不会饥饿。
static uint32_t s_cloud_gc_floor = 0;

// FIX-1：批次**绝对基准** —— cloud_collect_batch() 取批时的 s_cloud_q_rd
//
// 为什么必须绝对：`covered`（被 ACK 覆盖的条数）是"从取批时的队首起算"的
// 相对量；而队列溢出淘汰会在批次在途期间推进 s_cloud_q_rd。
// 若把相对量加到**已漂移**的游标上（原 `rd += covered`），就会**跳过**
// 一条既未发送、也未计数的记录（INFO 永久丢失）。
// 正确写法：rd = rd_base + max(evicted_since_collect, covered)。
static uint32_t s_cloud_tx_rd_base = 0;

// FIX-5：批次描述符**有效期**（与"是否正在等待"解耦）
//
// 原实现以 s_cloud_inflight 作为"能否匹配 ACK"的依据，而超时后
// (`cloud_poll` 第 2 步) 会立刻 `inflight = false` 进入退避窗口 ⇒
// 此期间到达的 ACK 被 `log_ack_classify()` 规则②判为 IGNORE 并**永久丢弃**
// ⇒ 无谓重传、推进被推迟、give-up 概率上升（与 FIX-2 的空洞压力联动）。
// 语义修正：只要"最近一次发送的批次"尚未被 ACK / 放弃 / 被新批次覆盖，
// 它的描述符就仍然有效。
static bool     s_cloud_tx_valid = false;

// ---- P1.5：ACK / Retry / Offline ----
//
// 语义要点（冻结）：
//   · ACK = 云端已持久化该区间，**不是**"以后不会再收到" ⇒ at-least-once
//   · 重试耗尽 = GIVE_UP_NOT_ADVANCE：放弃**发送**，但 ack 水位与 Flash 段
//     都不推进（记录留待下次开机重试）
//   · 部分覆盖只推进被覆盖前缀；段删除必须整段被覆盖（log_ack.h 判定）
static uint32_t s_cloud_retry_n = 0;        // 当前在途批次的超时次数
static bool     s_cloud_gave_up = false;    // 本批次已放弃发送（仅标志，不影响 ack 水位）
static uint32_t s_cloud_give_up_seq = 0;    // 本 boot 已放弃重发的水位
static uint32_t s_cloud_deadline_ms = 0;    // 在途批次的 ACK 超时时刻
static uint32_t s_cloud_backoff_until_ms = 0;  // 下次允许发送的时刻（退避）

// ACK 注入（CloudManager 回调 / 串口测试钩子共用；仅做轻量赋值，
// 真正的判定与落账在 cloud_poll() 内完成 —— 回调上下文不碰 Flash）
static volatile bool s_cloud_ack_pending = false;
static uint32_t s_cloud_ack_boot = 0;
static uint32_t s_cloud_ack_from = 0;
static uint32_t s_cloud_ack_to = 0;

// ---- FIX-2：空洞（未确认且本 Boot 已放弃投递）区间表 ----
//
// 只在 loopTask 上下文读写（give-up / 队列淘汰 / 回收判定 / 观测全在 loop 上），
// 回调线程不碰它 ⇒ **无需进入 s_mux**。
static LogHole  s_cloud_holes[LOG_HOLE_MAX];
static uint8_t  s_cloud_hole_count = 0;
static bool     s_cloud_hole_overflow = false;

// ---- DIR-1：Flash → 云 补发游标（按 slot 遍历，不再按 seq 反算）----
//
// ★ 为什么必须按 slot 遍历：
//   `seq` 在 log_emit() 中每条非 DEBUG 记录都消耗（含 INFO），INFO 不落 Flash
//   ⇒ 段内的 seq **稀疏** ⇒ 旧实现的两处假设同时失效：
//     ① `idx = seq - first_seq` 反算 slot ⇒ 指向**错误的槽位**
//     ② 终止条件 `flash_find_segment_of_seq() == 0xFF` 会在
//        `replay_seq > first + records - 1` 时立刻触发 ⇒ **补发提前终止**，
//        其后所有段中从未确认的 WARN+ 全部静默丢失
//   改为遍历 slot 并使用**记录自身的 seq** 后，两个问题同时消失。
static uint8_t  s_replay_seg = 0xFF;   // 0xFF = 需按 s_replay_after_* 重新选取起始段
static uint8_t  s_replay_idx = 0;      // 段内下一个待考察的 slot 下标
static bool     s_replay_armed = false; // 是否有待补发工作（可由段删除重新 arm）
static uint32_t s_replay_diag_seq = 0; // 诊断：最近一次考察到的 record seq（**非游标**）

// ---- FIX-D1：本轮 arm 的"起始段约束"（纯 RAM，无持久化）----
//
//   after_valid = false ⇒ 从 first_seq 最小的段开始（= 全新一轮 sweep）
//   after_valid = true  ⇒ 从 first_seq **严格大于** after_seq 的段开始
//
// 为什么需要：补发 sweep 进行中，若 flash_drop_segment() 删掉的正是游标所在段，
// 旧实现把游标复位成 0xFF（= 约束清零）⇒ 下一轮从**最老段重新扫**，已扫描
// 区域被无谓地重复遍历。当前不变量下（游标只在 used==0 时前进 ⇒ 已扫描记录
// 必已被 ACK/放弃）不会造成重复投递，但属**无谓扫描**，且一旦将来不变量变化
// 就会升级为重复投递。改为记住被删段的 first_seq 并从其后继续 ⇒ 扫描严格单调。
static uint32_t s_replay_after_seq = 0;
static bool     s_replay_after_valid = false;

// 保留字段（观测兼容）：= !s_replay_armed
static bool     s_replay_done = true;

// 发送失败注入（模拟离线；一次性计数）
static uint8_t  s_cloud_fail_next = 0;

// 上板自测旁路（仅测试钩子可置位）
//
// 目的：本设备在开发期长期没有可用的 MQTT 对端，而 ACK 状态机必须存在
// "在途批次"才能进入 ACCEPT / PARTIAL / 超时重试分支。置位后：
//   · 在线判定恒为 true（不受 cloud_is_connected() 影响）
//   · cloud_send_log() 被跳过，直接视为发送成功
// 这样整条 ACK / 超时 / 退避 / 放弃 / 段回收链路都能在纯串口下跑通。
// **正式固件不得置位**（默认 false）。
static bool     s_cloud_force_online = false;

// 上板自测可调项（默认 = 冻结值，生产行为不受影响）
//
// 为什么需要：默认 ACK_TIMEOUT=15 s + 退避 2/4/8/16/32 s 下，走完
// "重试耗尽 → GIVE_UP_NOT_ADVANCE" 需要 ≈152 s 纯等待，超出本项目
// "单条命令 200 s 上限"的预算，那条冻结语义将无法被验证。
static uint32_t s_ack_timeout_ms = LOG_ACK_TIMEOUT_MS;
static uint32_t s_backoff_base_ms = LOG_ACK_BACKOFF_BASE_MS;

// 批次头"自上次上报"增量计数基线
static uint32_t s_cloud_rep_ring = 0;
static uint32_t s_cloud_rep_overflow = 0;
static uint32_t s_cloud_rep_unacked = 0;
static uint32_t s_cloud_rep_degraded = 0;

static void cloud_poll();
static void cloud_queue_push(const LogRecord &rec);
static bool cloud_init_buffers();
static void cloud_handle_ack();
static void cloud_delete_acked_segments();
static void cloud_replay_step();
static void cloud_replay_arm();
static bool cloud_try_send_batch(uint8_t n);
static uint32_t flash_lowest_first_seq();
static bool cloud_queue_head_is_current_boot();

// CloudManager 的 log_ack 回调（运行在 esp-mqtt 任务上下文）
//
// 只做一件事：把区间存进字段并置标志。
// **禁止**在这里做 LittleFS / Serial / 任何长耗时操作 —— 判定与落账
// 全部由 loopTask 的 cloud_poll() 完成（§26 回调限制 / §35 单写者）。
static void log_cloud_ack_callback(uint32_t boot_seq, uint32_t seq_from, uint32_t seq_to)
{
    portENTER_CRITICAL(&s_mux);
    s_cloud_ack_boot = boot_seq;
    s_cloud_ack_from = seq_from;
    s_cloud_ack_to = seq_to;
    s_cloud_ack_pending = true;
    portEXIT_CRITICAL(&s_mux);
}

// CRC32（与项目既有实现一致：init 0xFFFFFFFF，
//        poly 0xEDB88320 反射，final xor 0xFFFFFFFF）
//
// `HD-3`·`A-1`：实现已抽到 `log/log_meta_slot.h`（freestanding）以便
// `test/log_contract/probe_meta_slot.cpp` 真执行验证；此处保留同名薄封装，
// **既有调用点零改动**，算法仍只有**一份**。
static uint32_t log_crc32(const uint8_t *data, uint32_t len)
{
    return log_meta_slot::crc32(data, len);
}

// =====================================================
// P1.3 Flash Segment Ring（实现）
//
// 布局（§8 / §9）：
//   /log/meta.bin                32 B（Commit 3 接入）
//   /log/s0000000.log .. s0000015.log
//       16 B header + 31 × 128 B record = 3984 B
//
// 原则（§12 / §20 / §42）：
//   整段创建 / 整段追加 / 整段删除 —— 绝不修改已写入 Record 内的任何字节
//   （因此不存在 uploaded / persist 位，符合 LittleFS COW 特性）
//
// 并发（§35）：
//   全部由 loopTask 访问（log_init / log_task / 测试钩子），不额外加锁
// =====================================================

static void flash_seg_path(uint32_t seg, char *out, size_t out_size)
{
    snprintf(out, out_size, LOG_SEG_PATH_FMT, (unsigned)seg);
}

// 小端 u32 —— 实现见 `log/log_meta_slot.h`（`A-1` 抽取，单一来源）
static void flash_put_u32(uint8_t *dst, uint32_t v)
{
    log_meta_slot::put_u32(dst, v);
}

static uint32_t flash_get_u32(const uint8_t *src)
{
    return log_meta_slot::get_u32(src);
}

// 段头组装：magic / seg_index / first_seq / crc32(前 12 B)
static void flash_build_head(uint8_t *head, uint32_t seg, uint32_t first_seq)
{
    memset(head, 0, LOG_SEGMENT_HEADER_SIZE);
    flash_put_u32(&head[LOG_SEG_OFF_MAGIC], LOG_SEG_MAGIC);
    flash_put_u32(&head[LOG_SEG_OFF_INDEX], seg);
    flash_put_u32(&head[LOG_SEG_OFF_FIRST_SEQ], first_seq);
    flash_put_u32(&head[LOG_SEG_OFF_CRC32], log_crc32(head, LOG_SEG_CRC_SPAN));
}

// 段头校验（magic / seg_index / crc32）
static bool flash_check_head(const uint8_t *head, uint32_t expect_seg)
{
    if (flash_get_u32(&head[LOG_SEG_OFF_MAGIC]) != LOG_SEG_MAGIC)
    {
        return false;
    }

    if (flash_get_u32(&head[LOG_SEG_OFF_INDEX]) != expect_seg)
    {
        return false;
    }

    if (flash_get_u32(&head[LOG_SEG_OFF_CRC32]) != log_crc32(head, LOG_SEG_CRC_SPAN))
    {
        return false;
    }

    return true;
}

// Record 校验：crc32 覆盖 [0..LOG_OFF_CRC32)
static bool flash_check_record(const LogRecord &rec)
{
    return rec.crc32 == log_crc32((const uint8_t *)&rec, LOG_OFF_CRC32);
}

// 扫描段内 record（§19）：
//   遇到第一条 CRC 失败的 record 即停止 —— 其后的数据不能假定连续有效
//
// count_errors=false 用于"只读观测"（测试钩子 peek），避免反复扫描污染统计
static uint8_t flash_scan_records(File &f, bool count_errors)
{
    uint8_t count = 0;
    LogRecord rec;

    for (uint8_t i = 0; i < LOG_RECORDS_PER_SEGMENT; i++)
    {
        const uint32_t off =
            LOG_SEGMENT_HEADER_SIZE + (uint32_t)i * LOG_RECORD_SIZE;

        if (!f.seek(off))
        {
            break;
        }

        if (f.read((uint8_t *)&rec, LOG_RECORD_SIZE) != (size_t)LOG_RECORD_SIZE)
        {
            break;
        }

        if (!flash_check_record(rec))
        {
            // §19：该条及其后不再认为有效
            if (count_errors)
            {
                portENTER_CRITICAL(&s_mux);
                s_stats.flash_crc_error++;
                portEXIT_CRITICAL(&s_mux);
            }
            break;
        }

        count++;
    }

    return count;
}

// 把文件补零扩展到 target（修复被截断的 segment，§19）
static bool flash_extend_file(File &f, size_t target)
{
    const size_t sz = f.size();

    if (sz >= target)
    {
        return true;
    }

    if (!f.seek((uint32_t)sz))
    {
        return false;
    }

    static const uint8_t zeros[128] = { 0 };
    size_t remain = target - sz;

    while (remain > 0)
    {
        const size_t chunk = (remain >= sizeof(zeros)) ? sizeof(zeros) : remain;

        if (f.write(zeros, chunk) != chunk)
        {
            return false;
        }

        remain -= chunk;
    }

    return true;
}

// -----------------------------------------------------------------------------
// `A1-02` 残留修复（`2026-10-19`）：**损坏痕迹**（`/log/corrupt.trace`）
// -----------------------------------------------------------------------------
//
// 背景：`flash_drop_broken_segment()` 会 `LittleFS.remove()` 掉损坏段 ⇒ 同一份
// "损坏证据"在**下一次启动**就消失了。若隔离标记两份都写失败（NVS 异常 +
// LittleFS 只读），第二次启动就可能因 `scan_corrupt=0 + total_records=0`
// 被判 `REC_PROVISION` ⇒ `boot_seq=1` ⇒ 键重用。
//
// 本痕迹把"曾经损坏过"独立于段文件固化；`flash_scan_all()` 与本次扫描**取大**。
// ⚠️ 与日志区同生共死（同一 LittleFS），格式化后同样消失 —— 该情形由 NVS 证据兜底。

#define LOG_CORRUPT_TRACE_PATH "/log/corrupt.trace"
#define LOG_CORRUPT_TRACE_TMP  "/log/corrupt.tmp"

static bool corrupt_trace_read_path(const char *path, uint32_t &count,
                                    uint32_t &last_seg, uint32_t &last_boot)
{
    count     = 0;
    last_seg  = LOG_TRACE_SEG_NONE;
    last_boot = 0;

    if (!LittleFS.exists(path))
    {
        return false;
    }

    File f = LittleFS.open(path, "r");

    if (!f)
    {
        return false;
    }

    uint8_t buf[LOG_EV_SIZE];
    const bool got = (f.read(buf, sizeof(buf)) == (size_t)sizeof(buf));

    f.close();

    if (!got)
    {
        return false;
    }

    uint32_t ms = 0;

    return log_meta_evidence::parse_trace(buf, count, last_seg, last_boot, ms);
}

static bool corrupt_trace_read(uint32_t &count, uint32_t &last_seg, uint32_t &last_boot)
{
    return corrupt_trace_read_path(LOG_CORRUPT_TRACE_PATH, count, last_seg, last_boot);
}

static bool corrupt_trace_write_raw(const char *path, uint32_t count,
                                    uint32_t last_seg, uint32_t last_boot)
{
    uint8_t buf[LOG_EV_SIZE];

    log_meta_evidence::pack_trace(buf, count, last_seg, last_boot, (uint32_t)millis());

    File f = LittleFS.open(path, "w");

    if (!f)
    {
        return false;
    }

    const bool ok = (f.write(buf, sizeof(buf)) == (size_t)sizeof(buf));

    f.close();

    return ok;
}

/**
 * 损坏计数 +1 并落盘（**写后回读校验**）。
 *
 * @param last_seg  `LOG_TRACE_SEG_NONE` 表示非段来源（如隔离标记无法持久化）
 * @returns true = 痕迹**已确认落盘**；false = 无法持久化（调用方须保持停写）
 */
static bool corrupt_trace_bump(uint32_t last_seg, uint32_t last_boot)
{
    uint32_t c = 0, s = 0, b = 0;

    (void)corrupt_trace_read(c, s, b);

    const uint32_t next = log_meta_evidence::trace_add(c, 1u);

    if (!corrupt_trace_write_raw(LOG_CORRUPT_TRACE_TMP, next, last_seg, last_boot))
    {
        LittleFS.remove(LOG_CORRUPT_TRACE_TMP);
        return false;
    }

    /* 回读校验（与 `A1-05` 同风格）：`write()` 成功 ≠ 已真正落盘 */
    uint32_t c2 = 0, s2 = 0, b2 = 0;

    if (!corrupt_trace_read_path(LOG_CORRUPT_TRACE_TMP, c2, s2, b2) || (c2 != next))
    {
        LittleFS.remove(LOG_CORRUPT_TRACE_TMP);
        return false;
    }

    /* rename 覆盖（同文件系统、目录项级）；失败再退化为直接写 */
    if (LittleFS.exists(LOG_CORRUPT_TRACE_PATH))
    {
        (void)LittleFS.remove(LOG_CORRUPT_TRACE_PATH);
    }

    if (!LittleFS.rename(LOG_CORRUPT_TRACE_TMP, LOG_CORRUPT_TRACE_PATH))
    {
        LittleFS.remove(LOG_CORRUPT_TRACE_TMP);

        if (!corrupt_trace_write_raw(LOG_CORRUPT_TRACE_PATH, next, last_seg, last_boot))
        {
            return false;
        }
    }

    return true;
}

// §18：Header 无效 / 尺寸异常 → 删除该 segment（下次作为空段复用）并计数
static void flash_drop_broken_segment(uint32_t seg, const char *path)
{
    /* ★ `A1-02` 残留修复：**先**把损坏痕迹固化，**再**删除段。
       即使段被删，下一次启动仍能从 trace 读到 `count > 0` ⇒ 不会被判 `PROVISION`。 */
    (void)corrupt_trace_bump(seg, s_boot_seq);

    LittleFS.remove(path);

    s_seg_present[seg] = 0;
    s_seg_records[seg] = 0;
    s_seg_first_seq[seg] = 0;
    s_seg_last_seq[seg] = 0;

    s_scan_corrupt++;

    portENTER_CRITICAL(&s_mux);
    s_stats.flash_corrupt_segment++;
    portEXIT_CRITICAL(&s_mux);
}

// 依 first_seq 重算 oldest / newest
static void flash_recompute_extremes()
{
    uint32_t oldest = 0;
    uint32_t newest = 0;
    uint32_t oldest_seq = 0;
    uint32_t newest_seq = 0;
    bool have = false;

    for (uint32_t i = 0; i < LOG_SEGMENT_COUNT; i++)
    {
        if (!s_seg_present[i])
        {
            continue;
        }

        const uint32_t fs = s_seg_first_seq[i];

        if (!have || fs < oldest_seq)
        {
            oldest_seq = fs;
            oldest = i;
        }

        if (!have || fs > newest_seq)
        {
            newest_seq = fs;
            newest = i;
        }

        have = true;
    }

    s_oldest_segment = have ? oldest : 0;
    s_newest_segment = have ? newest : 0;
}

// §20：删除某个 segment（ring 淘汰 / 损坏重建）
static bool flash_drop_segment(uint32_t seg)
{
    // FIX-D1：必须在清空段元信息**之前**记下 first_seq —— 若被删的正是补发游标
    // 所在段，需要用它把下一轮扫描约束在"该 seq 之后"，而不是回到最老段重扫。
    const uint32_t dropped_first_seq = s_seg_first_seq[seg];
    const bool     dropped_present   = (s_seg_present[seg] != 0);

    char path[32];
    flash_seg_path(seg, path, sizeof(path));

    bool ok = true;

    if (LittleFS.exists(path))
    {
        ok = LittleFS.remove(path);
    }

    if (!ok)
    {
        return false;
    }

    if (s_seg_present[seg])
    {
        if (s_flash_total_records >= s_seg_records[seg])
        {
            s_flash_total_records =
                (uint16_t)(s_flash_total_records - s_seg_records[seg]);
        }

        s_seg_present[seg] = 0;
        s_seg_records[seg] = 0;
        s_seg_first_seq[seg] = 0;
        s_seg_last_seq[seg] = 0;

        if (s_valid_segment_count > 0)
        {
            s_valid_segment_count--;
        }
    }

    portENTER_CRITICAL(&s_mux);
    s_stats.flash_segment_deleted++;
    portEXIT_CRITICAL(&s_mux);

    // FIX-D1：补发游标可能正指向被删段 ⇒ 复位并重新 arm
    //（否则游标指向不存在的段，扫描会提前结束、漏掉其后所有未确认记录）
    //
    // ★ 关键：**不要**无条件回到最老段。把被删段的 first_seq 记为约束，
    //   下一轮从"first_seq > 该值"的段继续 ⇒ 扫描严格单调前进、不重复遍历。
    //   `dropped_present == false`（删的是空段槽位）时退化为"从最老段开始"，
    //   与旧行为一致且此时无可扫描内容，安全。
    if (s_replay_seg == (uint8_t)seg)
    {
        s_replay_after_seq = dropped_first_seq;
        s_replay_after_valid = (dropped_present && dropped_first_seq != 0u);

        s_replay_seg = 0xFF;
        s_replay_idx = 0;
        s_replay_armed = true;
        s_replay_done = false;
    }

    flash_recompute_extremes();
    return true;
}

// 全量扫描 16 个 segment（§17）
//
// ⚠️ Commit 2 范围：只做"识别与元信息建立"。
//    头/大小损坏时的"删除 + corrupt_count++"（§18）在 Commit 4 实现。
static void flash_scan_all()
{
    /* ★ `A1-02` 残留修复（`2026-10-19`）：把**历史**损坏痕迹并入本次计数（取大）。
       损坏段在上一次启动已被 `LittleFS.remove()` ⇒ 段文件不再是证据；
       trace 是唯一还能证明"这台设备曾发生过损坏"的东西。
       `merge_corrupt` 用 `max` 而非 `+`：多轮扫描不会重复累加，且
       "痕迹 > 0 ⇒ 合并值 > 0" ⇒ `decide_recovery()` 永不返回 `REC_PROVISION`。 */
    {
        uint32_t tc = 0, ts = 0, tb = 0;

        if (corrupt_trace_read(tc, ts, tb))
        {
            s_scan_corrupt = log_meta_evidence::merge_corrupt(s_scan_corrupt, tc);
        }
    }

    s_valid_segment_count = 0;
    s_flash_total_records = 0;
    s_oldest_segment = 0;
    s_newest_segment = 0;
    s_append_segment = 0;
    s_append_index = 0;

    uint32_t oldest_seq = 0;
    uint32_t newest_seq = 0;
    bool have_any = false;

    for (uint32_t i = 0; i < LOG_SEGMENT_COUNT; i++)
    {
        s_seg_present[i] = 0;
        s_seg_records[i] = 0;
        s_seg_first_seq[i] = 0;
        s_seg_last_seq[i] = 0;

        char path[32];
        flash_seg_path(i, path, sizeof(path));

        if (!LittleFS.exists(path))
        {
            continue;
        }

        // 以 "r+" 打开：截断修复（§19）需要写回
        File f = LittleFS.open(path, "r+");

        if (!f)
        {
            continue;
        }

        const size_t fsize = f.size();

        // §18：连 header 都不完整 → 损坏
        if (fsize < (size_t)LOG_SEGMENT_HEADER_SIZE)
        {
            f.close();
            flash_drop_broken_segment(i, path);
            continue;
        }

        // §18：尺寸异常膨胀 → 损坏
        if (fsize > (size_t)LOG_SEGMENT_SIZE)
        {
            f.close();
            flash_drop_broken_segment(i, path);
            continue;
        }

        uint8_t head[LOG_SEGMENT_HEADER_SIZE];

        if (f.read(head, sizeof(head)) != (size_t)sizeof(head))
        {
            f.close();
            flash_drop_broken_segment(i, path);
            continue;
        }

        // §17-3/4/5 magic / CRC / index 校验；§18：非法 → 删除 + 计数
        if (!flash_check_head(head, i))
        {
            f.close();
            flash_drop_broken_segment(i, path);
            continue;
        }

        const uint32_t first_seq = flash_get_u32(&head[LOG_SEG_OFF_FIRST_SEQ]);
        const uint8_t records = flash_scan_records(f, true);

        // FIX-3b：**实测**末条 seq（禁止用 `first_seq + records - 1` 推导）
        //
        // 追加严格按 seq 递增（Ring 顺序 = emit 顺序），且损坏恢复后的重写
        // 使用更大的新 seq ⇒ 末槽恒为该段最大 seq。
        // 读失败时取 UINT32_MAX：含义是"末条 seq 未知" ⇒ 该段在本次 Boot 内
        // 永不满足 `last_seq <= acked_seq` ⇒ 不回收（保守，宁可少回收）。
        uint32_t last_seq = 0;

        if (records > 0)
        {
            LogRecord last_rec;
            const uint32_t loff = LOG_SEGMENT_HEADER_SIZE +
                                  (uint32_t)(records - 1u) * LOG_RECORD_SIZE;

            if (f.seek(loff) &&
                (f.read((uint8_t *)&last_rec, LOG_RECORD_SIZE) ==
                 (size_t)LOG_RECORD_SIZE))
            {
                last_seq = last_rec.seq;
            }
            else
            {
                last_seq = 0xFFFFFFFFu;
            }
        }

        // §19：文件被截断（部分写入）→ 补零修复为定长；
        //       append 位置由 flash_scan_records 给出的"首条坏记录"决定
        if (fsize < (size_t)LOG_SEGMENT_SIZE)
        {
            flash_extend_file(f, LOG_SEGMENT_SIZE);
        }

        f.close();

        s_seg_present[i] = 1;
        s_seg_records[i] = records;
        s_seg_first_seq[i] = first_seq;
        s_seg_last_seq[i] = last_seq;
        s_valid_segment_count++;
        s_flash_total_records = (uint16_t)(s_flash_total_records + records);

        if (!have_any || first_seq < oldest_seq)
        {
            oldest_seq = first_seq;
            s_oldest_segment = i;
        }

        if (!have_any || first_seq > newest_seq)
        {
            newest_seq = first_seq;
            s_newest_segment = i;
        }

        have_any = true;
    }

    if (!have_any)
    {
        return;   // 空 ring：追加时从 segment 0 建起
    }

    // 追加目标：最新段的空闲槽
    s_append_segment = s_newest_segment;
    s_append_index = s_seg_records[s_newest_segment];
}

// 整段创建：预分配 LOG_SEGMENT_SIZE 字节并写入 16 B header
//
// §9：一个 segment 恒为 3984 B（16 B header + 31 × 128 B）。
// 之所以**预分配定长**而不是"按需增长"：
//   · 扫描时 size 校验恒定成立（否则部分填充的段会被 size 检查误判为损坏）
//   · 追加只是覆盖已有偏移，不触发文件扩展
// 未写入的 record 槽为全 0 —— 其 crc32 必然校验失败，因此天然表达"空槽"。
//
// 时间边界（§30）：仅在本段首次创建时发生，约 4 KB 写（几十 ms 量级），
// 属低频事件（每 31 条记录一次），因此可与本轮的 record 追加合为一个单元。
static bool flash_create_segment(uint32_t seg, uint32_t first_seq)
{
    char path[32];
    flash_seg_path(seg, path, sizeof(path));

    if (LittleFS.exists(path))
    {
        LittleFS.remove(path);
    }

    File f = LittleFS.open(path, "w");

    if (!f)
    {
        return false;
    }

    uint8_t head[LOG_SEGMENT_HEADER_SIZE];
    flash_build_head(head, seg, first_seq);

    bool ok = (f.write(head, sizeof(head)) == (size_t)sizeof(head));

    // 整段预分配：剩余空间补零，使文件恒为 LOG_SEGMENT_SIZE
    if (ok)
    {
        static const uint8_t zeros[128] = { 0 };
        uint32_t remain = LOG_SEGMENT_SIZE - LOG_SEGMENT_HEADER_SIZE;

        while (remain > 0)
        {
            const size_t chunk = (remain >= sizeof(zeros)) ? sizeof(zeros) : remain;

            if (f.write(zeros, chunk) != chunk)
            {
                ok = false;
                break;
            }

            remain -= (uint32_t)chunk;
        }
    }

    f.close();

    if (!ok)
    {
        return false;
    }

    s_seg_present[seg] = 1;
    s_seg_records[seg] = 0;
    s_seg_first_seq[seg] = first_seq;
    s_seg_last_seq[seg] = 0;
    s_valid_segment_count++;
    s_append_segment = seg;
    s_append_index = 0;
    s_newest_segment = seg;   // 新段必为最新（first_seq 单调递增）

    portENTER_CRITICAL(&s_mux);
    s_stats.flash_segment_created++;
    portEXIT_CRITICAL(&s_mux);

    return true;
}

// 确保存在可写的追加目标；必要时新建 segment
//
// ⚠️ Commit 2 范围：16 段全满时**返回 false**（暂不可写）。
//    §20 的"删除最老段并复用"在 Commit 4 实现。
// FIX-3：段环压力淘汰前的**未确认记录计数**
//
// 被淘汰段中 seq > acked_seq 的记录从未被云端确认，销毁它们必须**有账**
// （→ drop_unacked，随批次头侧信道上行），否则云端永远不知道丢过 WARN+。
//
// ★ 计数方式：逐 slot 读取并用**记录自身的 seq** 比较。
//   **不得**用 `records - (acked - first + 1)` 之类公式 —— 段内 seq 稀疏
//   （INFO 不落 Flash），任何"稠密"假设都会漏报。
// ★ 读失败时保守返回剩余条数（宁可多报，不可漏报）。
static uint32_t flash_count_unacked(uint32_t seg, uint32_t acked_seq)
{
    if (!s_seg_present[seg] || s_seg_records[seg] == 0)
    {
        return 0;
    }

    if (acked_seq == 0)
    {
        return s_seg_records[seg];   // 尚无任何确认 ⇒ 全部未确认（省 31 次读）
    }

    if (s_seg_last_seq[seg] <= acked_seq)
    {
        return 0;                    // 整段低于水位（last_seq 为实测值）
    }

    char path[32];
    flash_seg_path(seg, path, sizeof(path));

    File f = LittleFS.open(path, "r");

    if (!f)
    {
        return s_seg_records[seg];   // 读不到 → 保守全计
    }

    uint32_t unacked = 0;
    LogRecord rec;

    for (uint8_t i = 0; i < s_seg_records[seg]; i++)
    {
        const uint32_t off =
            LOG_SEGMENT_HEADER_SIZE + (uint32_t)i * LOG_RECORD_SIZE;

        if (!f.seek(off) ||
            f.read((uint8_t *)&rec, LOG_RECORD_SIZE) != (size_t)LOG_RECORD_SIZE)
        {
            unacked += (uint32_t)(s_seg_records[seg] - i);
            break;
        }

        if (rec.seq > acked_seq)
        {
            unacked++;
        }
    }

    f.close();
    return unacked;
}

static bool flash_ensure_append_target(uint32_t next_seq)
{
    // ① 当前目标段可用（存在且未满）
    if (s_seg_present[s_append_segment] &&
        s_append_index < LOG_RECORDS_PER_SEGMENT)
    {
        return true;
    }

    // ② 选定目标段
    uint32_t target = LOG_SEGMENT_COUNT;

    if (s_valid_segment_count == 0)
    {
        target = 0;                                                  // 空 ring
    }
    else if (s_seg_records[s_newest_segment] < LOG_RECORDS_PER_SEGMENT)
    {
        target = s_newest_segment;                                   // 最新段有空位
    }
    else
    {
        // 从最新段之后按环形找一条未使用的段
        for (uint32_t k = 1; k <= LOG_SEGMENT_COUNT; k++)
        {
            const uint32_t idx = (s_newest_segment + k) % LOG_SEGMENT_COUNT;

            if (!s_seg_present[idx])
            {
                target = idx;
                break;
            }
        }

        if (target >= LOG_SEGMENT_COUNT)
        {
            // §20：段环已满 → 淘汰一个 segment 后复用其槽位
            //
            // FIX-3：**优先淘汰已完全被 ACK 覆盖、且不与空洞相交的段**
            //        （删之无损：记录已确认送达）。仅当不存在这样的段时，
            //        才退化为最老段（与旧行为一致），并对其中未确认的记录记账。
            uint32_t victim = s_oldest_segment;
            bool victim_confirmed = false;

            for (uint32_t k = 0; k < LOG_SEGMENT_COUNT; k++)
            {
                const uint32_t idx = (s_oldest_segment + k) % LOG_SEGMENT_COUNT;

                if (!s_seg_present[idx] || s_seg_records[idx] == 0)
                {
                    continue;
                }

                if (idx == s_append_segment)
                {
                    continue;   // 追加目标不动
                }

                // FIX-BT9：用**连续**水位 —— 否则会把"尚未补发/确认的旧记录"
                // 所在段当作"可安全复用"，等价于在环压力下丢未投递数据。
                if (log_ack_segment_reclaimable(
                        s_seg_first_seq[idx], s_seg_last_seq[idx],
                        s_cloud_gc_seq,
                        s_cloud_holes, s_cloud_hole_count,
                        s_cloud_hole_overflow))
                {
                    victim = idx;
                    victim_confirmed = true;
                    break;
                }
            }

            if (!victim_confirmed)
            {
                // 只能牺牲未确认数据 → 必须记账（§20：drop_unacked 可见）
                // FIX-BT9：口径与"未确认"的判定保持一致（> 连续水位 ⇒ 未确认）
                const uint32_t unacked =
                    flash_count_unacked(victim, s_cloud_gc_seq);

                if (unacked > 0)
                {
                    portENTER_CRITICAL(&s_mux);
                    s_stats.cloud_flash_drop += unacked;
                    s_stats.flash_seg_evict_unacked += unacked;
                    portEXIT_CRITICAL(&s_mux);

                    Serial.printf(
                        "[Log] ring pressure: evict seg=%u unacked=%u acked=%u gc=%u\n",
                        (unsigned)victim, (unsigned)unacked,
                        (unsigned)s_cloud_acked_seq, (unsigned)s_cloud_gc_seq);
                }
            }

            if (!flash_drop_segment(victim))
            {
                return false;
            }

            target = victim;
        }
    }

    // ③ 目标段已有文件 → 直接指向其尾部
    if (s_seg_present[target])
    {
        s_append_segment = target;
        s_append_index = s_seg_records[target];
        return (s_append_index < LOG_RECORDS_PER_SEGMENT);
    }

    // ④ 新建段（first_seq = 即将写入的首条记录 seq）
    return flash_create_segment(target, next_seq);
}

// 追加一个 batch（§30：一次调用 = 一个 Flash append 单元）
//
// 返回成功写入的 record 条数（< count 表示中途失败）。
static uint8_t flash_append_batch(const LogRecord *recs, uint8_t count)
{
    uint8_t written = 0;

    // 故障注入（§22/§23 / F10）：整批失败，不写入任何 record。
    // 调用方会因 written==0 而**不推进 s_rd**，记录保留待重试。
    if (s_flash_fail_budget > 0)
    {
        s_flash_fail_budget--;
        return 0;
    }

    for (uint8_t i = 0; i < count; i++)
    {
        if (!flash_ensure_append_target(recs[i].seq))
        {
            break;
        }

        char path[32];
        flash_seg_path(s_append_segment, path, sizeof(path));

        File f = LittleFS.open(path, "r+");

        if (!f)
        {
            break;
        }

        const uint32_t off = LOG_SEGMENT_HEADER_SIZE +
                             (uint32_t)s_append_index * LOG_RECORD_SIZE;

        bool ok = f.seek(off);

        if (ok)
        {
            ok = (f.write((const uint8_t *)&recs[i], LOG_RECORD_SIZE) ==
                  (size_t)LOG_RECORD_SIZE);
        }

        f.close();

        if (!ok)
        {
            break;
        }

        s_append_index++;
        s_seg_records[s_append_segment]++;
        s_seg_last_seq[s_append_segment] = recs[i].seq;   // FIX-3b：与 records 同点维护
        s_flash_total_records++;
        written++;
    }

    return written;
}

// =====================================================
// P1.3 meta.bin / sequence reservation（§12 / §13 / §15 / §16）
// =====================================================

// 读 meta；false = 不存在 / 长度不符 / magic 或版本不符 / CRC 错
static bool meta_read(uint32_t &boot_seq, uint32_t &seq_reserved, uint32_t &corrupt)
{
    if (!LittleFS.exists(LOG_META_PATH))
    {
        return false;
    }

    File f = LittleFS.open(LOG_META_PATH, "r");

    if (!f)
    {
        return false;
    }

    uint8_t buf[LOG_META_SIZE];
    const bool got = (f.read(buf, sizeof(buf)) == (size_t)sizeof(buf));
    f.close();

    if (!got)
    {
        return false;
    }

    if (flash_get_u32(&buf[LOG_META_OFF_MAGIC]) != LOG_META_MAGIC)
    {
        return false;
    }

    if (flash_get_u32(&buf[LOG_META_OFF_FMT]) != LOG_META_VERSION)
    {
        return false;
    }

    if (flash_get_u32(&buf[LOG_META_OFF_CRC32]) != log_crc32(buf, LOG_META_CRC_SPAN))
    {
        return false;
    }

    boot_seq     = flash_get_u32(&buf[LOG_META_OFF_BOOT_SEQ]);
    seq_reserved = flash_get_u32(&buf[LOG_META_OFF_SEQ_RESERVED]);
    corrupt      = flash_get_u32(&buf[LOG_META_OFF_CORRUPT]);

    return true;
}

// -----------------------------------------------------------------------------
// `HD-3`·`A-1` 修复：持久证据（NVS）与隔离标记（`2026-10-18`）
// -----------------------------------------------------------------------------
//
// 三份证据的存活关系（**这是本修复的核心论点**）：
//   · NVS 记录（`guolog/ev`）      —— 存活于 LittleFS 格式化 / 日志区损坏
//   · `/log/quarantine.bin`        —— 与日志区同生共死（作为 NVS 的备份）
//   · 双槽 / legacy / 段内 boot_seq —— 原有三层（日志区内）
// ⇒ 只有"整片擦除（NVS + LittleFS 同时丢）"才会三者全无；
//   该情形等价于**受控恢复出厂设置**，见 `log_meta_evidence.h` 头部边界说明。

#define LOG_QUARANTINE_PATH "/log/quarantine.bin"

// 读 NVS 证据。失败 ⇒ 输出参数保持 0，返回 false（**不得**据此推断"从未运行"）
static bool evidence_read_nvs()
{
    Preferences prefs;

    if (!prefs.begin(kEvNamespace, true))   // 只读打开
    {
        return false;
    }

    uint8_t buf[LOG_EV_SIZE];

    const size_t n = prefs.getBytesLength(kEvKey);

    if (n != (size_t)LOG_EV_SIZE)
    {
        prefs.end();
        return false;
    }

    if (prefs.getBytes(kEvKey, buf, sizeof(buf)) != sizeof(buf))
    {
        prefs.end();
        return false;
    }

    prefs.end();

    uint32_t b = 0, r = 0, f = 0, g = 0, c = 0;

    if (!log_meta_evidence::parse(buf, b, r, f, g, c))
    {
        return false;
    }

    s_ev_boot_hw     = b;
    s_ev_reserved_hw = r;
    s_ev_flags       = f;
    s_ev_gen         = g;
    s_ev_corrupt     = c;

    return true;
}

// 写 NVS 证据（每次 boot + 每次 seq 预留 + 隔离置位）
static bool evidence_write_nvs(uint32_t boot_hw, uint32_t reserved_hw,
                               uint32_t flags, uint32_t corrupt)
{
    uint8_t buf[LOG_EV_SIZE];

    log_meta_evidence::pack(buf, boot_hw, reserved_hw, flags, s_ev_gen + 1u, corrupt);

    Preferences prefs;

    if (!prefs.begin(kEvNamespace, false))
    {
        return false;
    }

    const size_t n = prefs.putBytes(kEvKey, buf, sizeof(buf));

    prefs.end();

    if (n != sizeof(buf))
    {
        return false;
    }

    s_ev_gen         = s_ev_gen + 1u;
    s_ev_boot_hw     = boot_hw;
    s_ev_reserved_hw = reserved_hw;
    s_ev_flags       = flags;
    s_ev_corrupt     = corrupt;
    s_evidence_ok    = true;

    return true;
}

// 读 `/log/quarantine.bin`（NVS 的备份；两者任一存活即可）
static bool quarantine_read_file()
{
    if (!LittleFS.exists(LOG_QUARANTINE_PATH))
    {
        return false;
    }

    File f = LittleFS.open(LOG_QUARANTINE_PATH, "r");

    if (!f)
    {
        return false;
    }

    uint8_t buf[LOG_EV_SIZE];
    const bool got = (f.read(buf, sizeof(buf)) == (size_t)sizeof(buf));

    f.close();

    if (!got)
    {
        return false;
    }

    uint32_t b = 0, r = 0, f2 = 0, g = 0, c = 0;
    return log_meta_evidence::parse(buf, b, r, f2, g, c);
}

/**
 * 写隔离标记文件（`/log/quarantine.bin`）。
 *
 * ★ `A1-11` 修复（`2026-10-19`）：原实现 `open(path, "w")` **立即截断**目标 ——
 *   掉电在 32 B 半写之间 ⇒ 文件存在但 CRC 失败 ⇒ `quarantine_read_file()` 返回
 *   false ⇒ 隔离标记丢失（此时只剩 NVS 一份）。现改为：
 *     ① 写临时文件；② **回读逐字节校验**；③ `rename` 覆盖。
 *   ⇒ "要么旧标记、要么新标记"，不会产生半写的第三态。
 *
 * ⚠️ `LittleFS.rename` 在目标存在时可能是 unlink+rename（非严格原子）⇒
 *   仍存在极窄的"两者都不存在"窗口；因此 NVS 那一份**不可省略**。
 */
static bool quarantine_write_file(uint32_t boot_hw, uint32_t reserved_hw, uint32_t corrupt)
{
    uint8_t buf[LOG_EV_SIZE];

    log_meta_evidence::pack(buf, boot_hw, reserved_hw,
                            LOG_EV_FLAG_QUARANTINE, s_ev_gen + 1u, corrupt);

    const char *tmp = "/log/quarantine.tmp";

    File f = LittleFS.open(tmp, "w");

    if (!f)
    {
        return false;
    }

    const bool wrote = (f.write(buf, sizeof(buf)) == (size_t)sizeof(buf));

    f.close();

    if (!wrote)
    {
        LittleFS.remove(tmp);
        return false;
    }

    /* 回读校验：写成功 ≠ 已落盘 */
    uint32_t b = 0, r = 0, fl = 0, g = 0, c = 0;
    bool verified = false;

    File rf = LittleFS.open(tmp, "r");

    if (rf)
    {
        uint8_t chk[LOG_EV_SIZE];

        verified = (rf.read(chk, sizeof(chk)) == (size_t)sizeof(chk))
                && log_meta_evidence::parse(chk, b, r, fl, g, c);

        rf.close();
    }

    if (!verified)
    {
        LittleFS.remove(tmp);
        return false;
    }

    if (LittleFS.exists(LOG_QUARANTINE_PATH))
    {
        (void)LittleFS.remove(LOG_QUARANTINE_PATH);
    }

    if (LittleFS.rename(tmp, LOG_QUARANTINE_PATH))
    {
        return true;
    }

    /* 退化：rename 失败时直接写正式路径（旧行为），至少保证标记存在 */
    LittleFS.remove(tmp);

    File f2 = LittleFS.open(LOG_QUARANTINE_PATH, "w");

    if (!f2)
    {
        return false;
    }

    const bool ok2 = (f2.write(buf, sizeof(buf)) == (size_t)sizeof(buf));

    f2.close();

    return ok2;
}

/**
 * 进入隔离：**双份固化**（NVS + 文件），并把运行期闸门置位。
 *
 * 设计取舍：任一份写失败都**不放弃**另一份，且无论持久化是否成功，
 * 本次 boot 立即停写（`s_seq_failclosed`）—— 宁可少记日志，也不产生
 * 可能与历史记录撞键的 `(boot_seq, seq)`。
 */
static void quarantine_enter(const char *reason, uint32_t boot_hw, uint32_t reserved_hw)
{
    s_seq_failclosed = true;
    s_seq_reliable   = false;

    const uint32_t flags = (s_ev_flags | LOG_EV_FLAG_QUARANTINE | LOG_EV_FLAG_PROVISIONED);

    const bool nvs_ok  = evidence_write_nvs(boot_hw, reserved_hw, flags, s_meta_corrupt);
    const bool file_ok = quarantine_write_file(boot_hw, reserved_hw, s_meta_corrupt);

    s_qfile_present = file_ok ? true : s_qfile_present;

    Serial.printf(
        "[Log] QUARANTINE (%s): seq allocation stopped; nvs=%d file=%d\n",
        reason, nvs_ok ? 1 : 0, file_ok ? 1 : 0);

    if (!nvs_ok && !file_ok)
    {
        /* 两份标记都写不进 ⇒ 本次 boot 仍停写，但**下次重启无法复原该状态**。
           退而求其次（`A1-02` 残留修复）：把"无法固化隔离"记为一次**损坏痕迹**。
           若该痕迹能落盘 ⇒ 下次 `flash_scan_all()` 会把它并入 `scan_corrupt`
           ⇒ `decide_recovery()` 绝不返回 `REC_PROVISION`（探针 E11 已断言）。
           ⚠️ 若 trace 也写不进（LittleFS 整体只读），则**没有任何**跨重启证据 ——
              该边界无法在设备侧保证，必须显式告警并由服务侧兜底。 */
        const bool trace_ok = corrupt_trace_bump(LOG_TRACE_SEG_NONE, boot_hw);

        Serial.printf(
            "[Log] QUARANTINE: BOTH markers failed -> trace=%d (durable=%d) reason=%s\n",
            trace_ok ? 1 : 0, trace_ok ? 1 : 0, reason);

        if (!trace_ok)
        {
            Serial.println(
                "[Log] QUARANTINE: NOT durable across reboot (nvs+file+trace all failed)");
        }
    }
}


// 写 **legacy** `meta.bin`（32 B · `fmt = LOG_META_VERSION = 2`）
//
// ⚠️ `HD-3`·`A-1` 起本函数**不再**是权威写入路径：
//    权威路径 = 双槽 `meta_commit_write()`（`meta_a.bin` / `meta_b.bin`，fmt=3）。
//    本函数只用于**回滚镜像**——让旧固件（只认 `meta.bin`）仍能读到最近一次成功值。
// ⚠️ 故障注入**不在此处**（已上移到 `meta_commit_write()`），否则"槽提交失败"与
//    "镜像失败"会被混为一谈。
// ★ `A1-08` 修复：改为 **临时文件 + rename**。原实现 `open("w")` 会立即截断唯一的
//   回滚镜像；若随后掉电，旧固件将读到 0 字节 ⇒ 回滚本身成为 `R-2` 的触发器。
//   `rename` 在同一文件系统内是**目录项级原子替换** ⇒ 要么旧镜像、要么新镜像。
static bool meta_write(uint32_t boot_seq, uint32_t seq_reserved, uint32_t corrupt)
{
    uint8_t buf[LOG_META_SIZE];
    memset(buf, 0, sizeof(buf));

    flash_put_u32(&buf[LOG_META_OFF_MAGIC], LOG_META_MAGIC);
    flash_put_u32(&buf[LOG_META_OFF_FMT], LOG_META_VERSION);
    flash_put_u32(&buf[LOG_META_OFF_BOOT_SEQ], boot_seq);
    flash_put_u32(&buf[LOG_META_OFF_SEQ_RESERVED], seq_reserved);
    flash_put_u32(&buf[LOG_META_OFF_CORRUPT], corrupt);
    flash_put_u32(&buf[LOG_META_OFF_CRC32], log_crc32(buf, LOG_META_CRC_SPAN));

    const char *tmp_path = LOG_META_PATH ".tmp";

    File f = LittleFS.open(tmp_path, "w");

    if (!f)
    {
        return false;
    }

    const bool wrote = (f.write(buf, sizeof(buf)) == (size_t)sizeof(buf));
    f.close();

    if (!wrote)
    {
        LittleFS.remove(tmp_path);
        return false;
    }

    /* 原子替换：旧镜像在新镜像落盘前始终完好 */
    if (!LittleFS.rename(tmp_path, LOG_META_PATH))
    {
        LittleFS.remove(tmp_path);
        return false;
    }

    return true;
}

// -----------------------------------------------------------------------------
// `HD-3`·`A-1`：双槽写入原语（`2026-10-18`）
// -----------------------------------------------------------------------------

// 写单个槽（`commit` 由调用方指定）+ **写后回读逐字节校验**（`A1-05`）
//
// ★ 为什么必须回读：`File::write()` 只表示"数据交给了 FS 缓存"，
//   `File::close()` 在 Arduino-ESP32 中**无返回值** ⇒ 落盘失败无法被察觉。
//   原实现因此可能返回 `true` 而槽内仍是旧值，导致内存与 Flash 信念分叉。
//   回读 32 B 的成本可忽略（仅在每次 seq 预留 / 开机时发生）。
static bool meta_slot_write_raw(const char *path, uint32_t boot_seq,
                                uint32_t seq_reserved, uint32_t corrupt,
                                uint32_t slot_seq, uint32_t commit)
{
    uint8_t buf[LOG_META_SIZE];

    /* 序列化（magic/fmt/payload/slot_seq/commit/crc）**全部**由纯逻辑完成
       ⇒ 与 `probe_meta_slot.cpp` 跑的是同一份代码 */
    log_meta_slot::pack(buf, boot_seq, seq_reserved, corrupt, slot_seq, commit);

    File f = LittleFS.open(path, "w");

    if (!f)
    {
        return false;
    }

    const bool wrote = (f.write(buf, sizeof(buf)) == (size_t)sizeof(buf));
    f.close();

    if (!wrote)
    {
        return false;
    }

    /* ---- 回读校验（`A1-05`）：逐字节比对，覆盖 `commit=0` 的中间态 ---- */
    if (s_meta_fail_stage == 3u)
    {
        s_meta_fail_stage = 0;      // 一次性注入
        return false;
    }

    File g = LittleFS.open(path, "r");

    if (!g)
    {
        return false;
    }

    uint8_t back[LOG_META_SIZE];
    const bool got = (g.read(back, sizeof(back)) == (size_t)sizeof(back));

    g.close();

    if (!got)
    {
        return false;
    }

    for (uint32_t i = 0; i < LOG_META_SIZE; i++)
    {
        if (back[i] != buf[i])
        {
            return false;
        }
    }

    return true;
}

// 读单个槽：**只有 `commit=1` 且 CRC 通过才算有效**
//
// ★ 两阶段提交的关键：`commit=0` 的半写槽**一律无效** ⇒ 掉电不会产生"看似有效
//   但字段半新半旧"的状态。
static bool meta_slot_read(const char *path, uint32_t &boot_seq,
                           uint32_t &seq_reserved, uint32_t &corrupt,
                           uint32_t &slot_seq)
{
    if (!LittleFS.exists(path))
    {
        return false;
    }

    File f = LittleFS.open(path, "r");

    if (!f)
    {
        return false;
    }

    uint8_t buf[LOG_META_SIZE];
    const bool got = (f.read(buf, sizeof(buf)) == (size_t)sizeof(buf));
    f.close();

    if (!got)
    {
        return false;
    }

    /* 校验（magic / fmt / **commit** / CRC）由纯逻辑完成 ⇒ 与探针同一份代码 */
    return log_meta_slot::parse(buf, boot_seq, seq_reserved, corrupt, slot_seq);
}

// 选一个可信槽：`slot_seq` 大者胜；两槽都无效 ⇒ false
static bool meta_read_best(uint32_t &boot_seq, uint32_t &seq_reserved,
                           uint32_t &corrupt, int8_t &slot)
{
    uint32_t bA = 0, rA = 0, cA = 0, qA = 0;
    uint32_t bB = 0, rB = 0, cB = 0, qB = 0;

    const bool okA = meta_slot_read(LOG_META_SLOT_A, bA, rA, cA, qA);
    const bool okB = meta_slot_read(LOG_META_SLOT_B, bB, rB, cB, qB);

    /* 选槽规则（纯逻辑，见 `log_meta_slot::choose()`）：
       `slot_seq` 大者胜；相等取 A（确定性）；都无效 ⇒ -1 */
    const int8_t pick = log_meta_slot::choose(okA, qA, okB, qB);

    if (pick < 0)
    {
        slot = -1;
        s_meta_slot_seq = 0;
        return false;
    }

    const bool takeA = (pick == 0);

    boot_seq        = takeA ? bA : bB;
    seq_reserved    = takeA ? rA : rB;
    corrupt         = takeA ? cA : cB;
    slot            = pick;
    s_meta_slot_seq = takeA ? qA : qB;

    return true;
}

// 提交一次元数据：**双槽两阶段** + legacy 镜像（best-effort）
//
// 顺序（每一步掉电都安全）：
//   ① 写目标槽（= 非当前有效槽）`commit=0`  ⇒ 目标槽失效，**旧槽仍完好**
//   ② 写目标槽 `commit=1`                   ⇒ 原子生效点
//   ③ best-effort 镜像 legacy `meta.bin`
//
// 返回 false 只有两种情形：① 或 ② 失败。此时**旧槽仍是最后一次成功值**，
// 调用方必须按"序号可能重复"降级（`s_seq_reliable=false`）。
static bool meta_commit_write(uint32_t boot_seq, uint32_t seq_reserved, uint32_t corrupt)
{
    // 故障注入（仅测试用，一次性）—— 作用于**整个提交**
    if (s_meta_fail_next)
    {
        s_meta_fail_next = false;
        return false;
    }

    /* `A1-06`：阶段 1 = 写槽①之前失败（等价于"提交未开始"） */
    if (s_meta_fail_stage == 1u)
    {
        s_meta_fail_stage = 0;
        return false;
    }

    const int8_t target = (s_meta_slot == 0) ? 1 : 0;
    const char *path = (target == 0) ? LOG_META_SLOT_A : LOG_META_SLOT_B;
    const uint32_t next_seq = s_meta_slot_seq + 1;

    if (!meta_slot_write_raw(path, boot_seq, seq_reserved, corrupt, next_seq, 0))
    {
        return false;
    }

    /* `A1-06`：阶段 2 = 槽①已落盘（`commit=0`）但槽②失败
       ⇒ **半写槽留在盘上**，这是两阶段提交最关键的中间态 */
    if (s_meta_fail_stage == 2u)
    {
        s_meta_fail_stage = 0;
        return false;
    }

    if (!meta_slot_write_raw(path, boot_seq, seq_reserved, corrupt,
                             next_seq, LOG_META_COMMIT_MARK))
    {
        return false;
    }

    s_meta_slot = target;
    s_meta_slot_seq = next_seq;

    /* legacy 镜像：失败**不影响**本次提交（回滚场景的已知限制），
       但必须留痕（`A1-08`）—— 否则"回滚可用性"会在无声中退化 */
    if (!meta_write(boot_seq, seq_reserved, corrupt))
    {
        s_seq_reliable = false;
        Serial.println("[Log] legacy meta mirror failed (rollback image stale)");
    }

    /* NVS 证据：与槽**同频**刷新（每次预留 / 开机一次）
       ⇒ LittleFS 整体被格式化后仍有独立的高水位可依（`A1-01`）。
       失败不改变本次提交结果（槽是权威），但必须降级标记。 */
    if (!evidence_write_nvs(boot_seq, seq_reserved,
                            s_ev_flags | LOG_EV_FLAG_PROVISIONED, corrupt))
    {
        s_seq_reliable = false;
        Serial.println("[Log] NVS evidence write failed (independent copy stale)");
    }

    return true;
}

// 扫描全部有效 segment 求已使用的最大 seq **与最大 boot_seq**（§16-1..4）
//
// `A-1` 追加 `out_boot_seq`：`meta` 全失效时，`boot_seq` **必须**能从段内
// `LogRecord.boot_seq`（`LOG_OFF_BOOT_SEQ = 8`，固件记录里本就存在）恢复，
// 否则只能从 1 重建 ⇒ 与云端历史 `(device_id, 1, …)` 撞键（`R-2`）。
//
// 依赖 flash_scan_all() 建立的 s_seg_present / s_seg_records，
// 因此必须在扫描之后调用。
static uint32_t flash_find_max_seq(uint32_t *out_boot_seq)
{
    uint32_t max_seq = 0;
    uint32_t max_boot = 0;
    LogRecord rec;

    for (uint32_t seg = 0; seg < LOG_SEGMENT_COUNT; seg++)
    {
        if (!s_seg_present[seg])
        {
            continue;
        }

        char path[32];
        flash_seg_path(seg, path, sizeof(path));

        File f = LittleFS.open(path, "r");

        if (!f)
        {
            continue;
        }

        for (uint8_t i = 0; i < s_seg_records[seg]; i++)
        {
            const uint32_t off =
                LOG_SEGMENT_HEADER_SIZE + (uint32_t)i * LOG_RECORD_SIZE;

            if (!f.seek(off))
            {
                break;
            }

            if (f.read((uint8_t *)&rec, LOG_RECORD_SIZE) != (size_t)LOG_RECORD_SIZE)
            {
                break;
            }

            if (rec.seq > max_seq)
            {
                max_seq = rec.seq;
            }

            if (rec.boot_seq > max_boot)
            {
                max_boot = rec.boot_seq;
            }
        }

        f.close();
    }

    if (out_boot_seq != nullptr)
    {
        *out_boot_seq = max_boot;
    }

    return max_seq;
}

// 预留一个新 seq 区间（§13）
//
//   seq_base = meta.seq_reserved + 1
//   meta.seq_reserved += LOG_SEQ_RESERVE     ← 立即落盘
//
// 返回 false = meta 写入失败 → 调用方置 s_seq_reliable = false（§15）
static bool seq_reserve_next()
{
    /* `A1-07`：区间算术不得回绕。`reserved` 已到最后一个可对齐的倍数之后
       ⇒ 无可用区间 ⇒ 直接转入隔离（此后 `log_emit` 全部拒绝）。 */
    if (s_meta_reserved >= 0xFFFFFF00u)
    {
        s_seq_failclosed = true;
        s_seq_reliable   = false;
        Serial.println("[Log] seq reserve impossible (reserved near UINT32_MAX) -> quarantine");
        return false;
    }

    const uint32_t base = s_meta_reserved + 1;
    const uint32_t next_reserved = s_meta_reserved + LOG_SEQ_RESERVE;

    if (!meta_commit_write(s_boot_seq, next_reserved, s_meta_corrupt))
    {
        return false;
    }

    s_meta_reserved = next_reserved;
    s_seq_base = base;
    s_seq_limit = base + LOG_SEQ_RESERVE - 1;

    // 分配指针对齐到区间起点之前（不倒退已有进度）
    if (s_seq_last < base - 1)
    {
        s_seq_last = base - 1;
    }

    return true;
}

// ---- `HD-3`·`A-1` 修复：段内 seq 高水位（跳过"末条未知"哨兵，`A1-04`）----
//
// `flash_scan_all()` 在读不到某段末条记录时把 `s_seg_last_seq[seg]` 置为
// `0xFFFFFFFF`（"未知"）。旧实现把它当数值参与 `ceil` 对齐 ⇒ 溢出回绕成 0
// ⇒ `reserved` **骤降**（注释却声称"只升不降"）。
static uint32_t flash_seg_high_water()
{
    uint32_t hw = 0;

    for (uint32_t i = 0; i < LOG_SEGMENT_COUNT; i++)
    {
        if (log_meta_evidence::seg_last_usable(s_seg_last_seq[i]) &&
            s_seg_last_seq[i] > hw)
        {
            hw = s_seg_last_seq[i];
        }
    }

    return hw;
}

// Boot 时初始化 sequence（§13 / §16 · `HD-3`·`A-1` 修复）
//
// 必须在 flash_init()（含 flash_scan_all）之后调用。
//
// 恢复顺序（唯一判据 = `log_meta_evidence::decide_recovery`）：
//   ① 隔离标记（NVS / `/log/quarantine.bin`）⇒ 拒绝分配，**跨重启固化**
//   ② NVS 证据        —— 独立于 LittleFS（格式化后仍可依）
//   ③ 双槽            —— 掉电安全的权威值
//   ④ legacy `meta.bin` —— 升级 / 回滚路径
//   ⑤ 段内 `boot_seq` —— 日志区最后一道线索
//   ⑥ 真·首启（**仅当**"本次 boot 才建立身份"且无任何痕迹）
//   ⑦ 其余（含"无证据但身份早已存在"）⇒ 隔离
//
// ★ 与旧实现的**行为差异**（即 `A1-01` / `A1-02` 的修复点）：
//   · 旧：无证据 + 无损坏痕迹 ⇒ 一律 `BOOT_FRESH`（含被格式化的旧设备）
//   · 新：无证据 + 身份早已存在 ⇒ **隔离**；只有"身份也是本次才建立"才允许 =1
//   · 旧：隔离只在 RAM 里，第二次重启即被删证据"洗白"
//   · 新：隔离写入 NVS（+ 文件备份）⇒ 重启后仍然生效
static void seq_init()
{
    /* 诊断：漏调 `log_set_first_boot()` ⇒ 全新设备会被判隔离（安全但不可用）。
       本告警只打一次，且**不改变**任何判定。 */
    if (!s_first_boot_set)
    {
        Serial.println(
            "[Log] WARN log_set_first_boot() not called -> first boot will be quarantined");
    }

    /* ================= ① 读三条独立证据线 ================= */

    s_evidence_ok   = evidence_read_nvs();
    s_qfile_present = quarantine_read_file();

    const bool q_nvs      = s_evidence_ok && ((s_ev_flags & LOG_EV_FLAG_QUARANTINE) != 0u);
    const bool quarantine = q_nvs || s_qfile_present;

    /* 双槽**总是**读：即使由 NVS 主导恢复，也必须知道"下一次提交该写哪个槽"，
       否则可能覆盖唯一有效槽 ⇒ 下次开机选槽回退到更旧的 `slot_seq`。 */
    uint32_t slot_boot = 0, slot_res = 0, slot_corrupt = 0;
    int8_t   slot = -1;
    const bool have_slot = meta_read_best(slot_boot, slot_res, slot_corrupt, slot);

    if (have_slot)
    {
        s_meta_slot = slot;      // 目标槽 = 非当前槽（由 `meta_commit_write` 取反）
    }

    /* legacy 仅在双槽无效且未隔离时读（省一次 Flash 读） */
    uint32_t l_boot = 0, l_res = 0, l_corrupt = 0;
    const bool have_legacy = (!have_slot && !quarantine)
                                 ? meta_read(l_boot, l_res, l_corrupt)
                                 : false;

    /* 段扫描仅在 NVS / 槽 / legacy 全无效时做（省 I/O） */
    uint32_t max_boot = 0;

    if (!s_evidence_ok && !have_slot && !have_legacy && !quarantine)
    {
        (void)flash_find_max_seq(&max_boot);   // seq 高水位由 `flash_seg_high_water()` 统一取
    }

    /* ================= ② 来源判定（纯逻辑） ================= */

    const int src = (int)log_meta_evidence::decide_recovery(
        quarantine, s_evidence_ok, have_slot, have_legacy,
        max_boot, s_flash_total_records, s_scan_corrupt, !s_first_boot);

    s_ev_source = (uint8_t)src;

    /* ================= ③ 统一取高水位（只升不降） ================= */

    const uint32_t seg_hw      = flash_seg_high_water();
    const uint32_t seg_align   = log_meta_evidence::align_reserve(seg_hw);

    const uint32_t boot_used = log_meta_evidence::pick_hw4(
        s_evidence_ok ? s_ev_boot_hw : 0u, slot_boot, l_boot, max_boot);

    uint32_t reserved = log_meta_evidence::pick_hw4(
        s_evidence_ok ? s_ev_reserved_hw : 0u, slot_res, l_res,
        (seg_align == LOG_SEQ_EXHAUSTED) ? 0u : seg_align);

    /* corrupt 计数：取"主导来源"的快照 + 本次扫描新增（§18） */
    uint32_t stored_corrupt = 0;

    if (s_evidence_ok)
    {
        stored_corrupt = s_ev_corrupt;
    }
    else if (have_slot)
    {
        stored_corrupt = slot_corrupt;
    }
    else if (have_legacy)
    {
        stored_corrupt = l_corrupt;
    }

    const bool recovered = (src == (int)log_meta_evidence::REC_FROM_NVS)
                        || (src == (int)log_meta_evidence::REC_FROM_SLOT)
                        || (src == (int)log_meta_evidence::REC_FROM_LEGACY);

    uint32_t corrupt = (recovered ? stored_corrupt : 0u) + s_scan_corrupt
                     + (recovered ? 0u : 1u);

    /* ================= ④ 隔离：不分配、不写槽、固化标记 ================= */

    if (src == (int)log_meta_evidence::REC_QUARANTINE)
    {
        s_meta_reserved = reserved;
        s_meta_corrupt  = corrupt;
        s_boot_seq      = boot_used;      // 仅用于上报，**不参与任何分配**

        Serial.printf(
            "[Log] recovery=QUARANTINE (nvs=%d slot=%d legacy=%d seg_max=%u recs=%u corrupt=%u first_boot=%d)\n",
            s_evidence_ok ? 1 : 0, have_slot ? 1 : 0, have_legacy ? 1 : 0,
            (unsigned)max_boot, (unsigned)s_flash_total_records,
            (unsigned)s_scan_corrupt, s_first_boot ? 1 : 0);

        const char *why = q_nvs ? "nvs-flag"
                        : (s_qfile_present ? "file-flag" : "no-evidence-but-provisioned");

        quarantine_enter(why, boot_used, reserved);

        /* 区间信息置为"不可用"，避免上游把 0 当成合法区间 */
        s_seq_base  = 0;
        s_seq_limit = 0;
        s_seq_last  = 0;
        return;
    }

    /* ================= ⑤ 计算本次 boot_seq（饱和，不回绕） ================= */

    bool boot_ok = true;
    uint32_t next_boot;

    if (src == (int)log_meta_evidence::REC_PROVISION)
    {
        next_boot = 1u;
        Serial.println("[Log] recovery=PROVISION (first boot of a new unit) -> boot_seq=1");
    }
    else
    {
        next_boot = log_meta_evidence::next_boot_seq(boot_used, boot_ok);
    }

    /* 区间耗尽 ⇒ 同样无法安全分配 ⇒ 隔离（绝不把 0 当新区间） */
    if (!boot_ok || reserved == LOG_SEQ_EXHAUSTED || seg_align == LOG_SEQ_EXHAUSTED)
    {
        s_meta_reserved = reserved;
        s_meta_corrupt  = corrupt;
        s_boot_seq      = boot_used;

        Serial.printf(
            "[Log] seq space exhausted (boot_ok=%d reserved=%u seg=%u) -> quarantine\n",
            boot_ok ? 1 : 0, (unsigned)reserved, (unsigned)seg_align);

        quarantine_enter("seq-exhausted", boot_used, reserved);
        s_seq_base  = 0;
        s_seq_limit = 0;
        s_seq_last  = 0;
        return;
    }

    /* ================= ⑥ 段内高水位兜底（`A1-04`：已按 sentinel 过滤） ================= */

    if (seg_align != LOG_SEQ_EXHAUSTED && seg_align > reserved)
    {
        Serial.printf(
            "[Log] reserved raised by segment high-water: seg_hw=%u reserved=%u\n",
            (unsigned)seg_hw, (unsigned)seg_align);

        reserved = seg_align;
    }

    s_meta_reserved = reserved;
    s_meta_corrupt  = corrupt;
    s_boot_seq      = next_boot;

    /* ================= ⑦ 固化 NVS 证据（即使后续预留失败也已留证） ================= */

    if (!evidence_write_nvs(s_boot_seq, s_meta_reserved,
                            (s_ev_flags | LOG_EV_FLAG_PROVISIONED),
                            s_meta_corrupt))
    {
        s_seq_reliable = false;
        Serial.println("[Log] NVS evidence write failed at boot (independent copy stale)");
    }

    /* 恢复来源逐条留痕（**串口是唯一能区分 5 种来源的地方**） */
    Serial.printf(
        "[Log] recovery=%s boot_hw=%u -> boot_seq=%u reserved=%u corrupt=%u (nvs=%d slot=%d legacy=%d seg_max=%u)\n",
        (src == (int)log_meta_evidence::REC_FROM_NVS)     ? "NVS"
        : (src == (int)log_meta_evidence::REC_FROM_SLOT)  ? "SLOT"
        : (src == (int)log_meta_evidence::REC_FROM_LEGACY) ? "LEGACY"
        : (src == (int)log_meta_evidence::REC_FROM_SEGMENT) ? "SEGMENT" : "PROVISION",
        (unsigned)boot_used, (unsigned)s_boot_seq, (unsigned)reserved,
        (unsigned)corrupt, s_evidence_ok ? 1 : 0, have_slot ? 1 : 0,
        have_legacy ? 1 : 0, (unsigned)max_boot);

    /* ================= ⑧ 预留首个区间 ================= */

    if (!seq_reserve_next())
    {
        // §15：写失败 → 明确标记不可信，但**不阻塞、不死等**，继续以本地区间工作
        s_seq_reliable = false;
        s_seq_base = s_meta_reserved + 1;
        s_seq_limit = s_seq_base + LOG_SEQ_RESERVE - 1;

        if (s_seq_last < s_seq_base - 1)
        {
            s_seq_last = s_seq_base - 1;
        }

        Serial.println("[Log] seq reservation failed -> seq_reliable=false");
    }
}

// Flash 层初始化：建目录 + 批量缓冲（PSRAM 优先）+ 全量扫描
static bool flash_init()
{
    if (!LittleFS.exists(LOG_DIR_PATH))
    {
        if (!LittleFS.mkdir(LOG_DIR_PATH))
        {
            Serial.println("[Log] mkdir /log failed");
            return false;
        }
    }

    // 批量工作缓冲（§31/§33：优先 PSRAM）
    s_flash_batch_bytes = (uint32_t)LOG_FLUSH_RECORDS * LOG_RECORD_SIZE;

    uint8_t in_psram = 0;
    void *mem = gf_alloc_psram_first(s_flash_batch_bytes, MEM_SITE_LOG_FLASH_BATCH,
                                    &in_psram);
    s_flash_batch_in_psram = (in_psram != 0u);

    if (mem == nullptr)
    {
        Serial.printf("[Log] Flash batch alloc failed (%u B)\n",
                      (unsigned)s_flash_batch_bytes);
        return false;
    }

    s_flash_batch = (LogRecord *)mem;
    memset(s_flash_batch, 0, s_flash_batch_bytes);

    flash_scan_all();

    Serial.printf(
        "[Log] Flash ring ready: seg=%u x %u rec, seg_size=%u B, "
        "valid=%u, oldest=%u, newest=%u, append=%u@%u, total_rec=%u, batch=%u B in %s\n",
        (unsigned)LOG_SEGMENT_COUNT,
        (unsigned)LOG_RECORDS_PER_SEGMENT,
        (unsigned)LOG_SEGMENT_SIZE,
        (unsigned)s_valid_segment_count,
        (unsigned)s_oldest_segment,
        (unsigned)s_newest_segment,
        (unsigned)s_append_segment,
        (unsigned)s_append_index,
        (unsigned)s_flash_total_records,
        (unsigned)s_flash_batch_bytes,
        s_flash_batch_in_psram ? "PSRAM" : "DRAM");

    return true;
}

// =====================================================
// log_init
// =====================================================

bool log_init()
{
    if (s_ready)
    {
        return true;
    }

    const uint32_t bytes = (uint32_t)LOG_RAM_QUEUE_SLOTS * (uint32_t)LOG_RECORD_SIZE;

    // 项目铁律：PSRAM 优先，失败回退内部 DRAM
    uint8_t in_psram = 0;
    void *mem = gf_alloc_psram_first(bytes, MEM_SITE_LOG_RING, &in_psram);
    s_in_psram = (in_psram != 0u);

    if (mem == nullptr)
    {
        Serial.printf("[Log] RAM ring alloc failed (%u B)\n", (unsigned)bytes);
        s_ready = false;
        return false;
    }

    s_ring = (LogRecord *)mem;
    memset(s_ring, 0, bytes);

    s_wr = 0;
    s_rd = 0;
    // boot_seq / seq 区间由 seq_init() 在 Flash 扫描之后统一建立（§13）

    // 与运行期保持同一保护规则：stats / flush 状态一律经 s_mux 访问
    // （init 期虽无并发，但统一规则可避免将来出现"漏锁"的访问点）
    portENTER_CRITICAL(&s_mux);
    memset(&s_stats, 0, sizeof(s_stats));
    s_flush_requested = false;

    // 突发合并状态复位（重新 init 视为无活动窗口）
    s_coalesce_active      = false;
    s_coalesce_last_ms     = 0;
    s_coalesce_folded      = 0;
    s_coalesce_param_count = 0;
    portEXIT_CRITICAL(&s_mux);

    s_ring_bytes = bytes;
    s_ready = true;

    Serial.printf(
        "[Log] RAM ring ready: %u slots x %u B = %u B in %s (boot_seq=%u)\n",
        (unsigned)LOG_RAM_QUEUE_SLOTS,
        (unsigned)LOG_RECORD_SIZE,
        (unsigned)bytes,
        s_in_psram ? "PSRAM" : "DRAM",
        (unsigned)s_boot_seq
    );

    // ---- P1.3：Flash Segment Ring（建目录 + 批量缓冲 + 全量扫描）----
    s_flash_ready = flash_init();

    if (!s_flash_ready)
    {
        // Flash 不可用：RAM 环与 routing 决策继续工作（受控 degraded），
        // 不阻塞、不死等；具体失败计数由 log_task() 的 I/O 区累加。
        Serial.println("[Log] Flash ring unavailable (degraded)");
    }

    // ---- P1.3：meta.bin / sequence reservation（依赖 scan 结果）----
    seq_init();

    Serial.printf(
        "[Log] seq: boot_seq=%u base=%u limit=%u reserved=%u reliable=%u corrupt=%u src=%u failclosed=%u\n",
        (unsigned)s_boot_seq,
        (unsigned)s_seq_base,
        (unsigned)s_seq_limit,
        (unsigned)s_meta_reserved,
        s_seq_reliable ? 1u : 0u,
        (unsigned)s_meta_corrupt,
        (unsigned)s_ev_source,
        s_seq_failclosed ? 1u : 0u);

    // ---- P1.4：Cloud Log Topic 缓冲（PSRAM 优先 / DRAM 回退）----
    if (!cloud_init_buffers())
    {
        // 云侧缓冲不可用：RAM 环与 Flash 段环继续工作（受控 degraded，不阻塞）
        Serial.println("[Log] Cloud log unavailable (degraded)");

        portENTER_CRITICAL(&s_mux);
        s_stats.self_degraded++;
        portEXIT_CRITICAL(&s_mux);
    }
    else
    {
        // P1.5：ACK 回调注入。
        // 依赖方向 CloudManager <- LogManager（单向），CloudManager 不反向依赖。
        // 回调只置标志（MQTT 任务上下文禁止碰 Flash），判定与落账在 cloud_poll()。
        cloud_set_log_ack_callback(log_cloud_ack_callback);

        // P1.5 状态复位：
        //   · acked_seq = 0 ⇒ 重启后曾发过的记录会重发（at-least-once，§25）
        //   · replay 未完成 ⇒ 允许把离线期间落在 Flash 的 WARN+ 补送出去
        portENTER_CRITICAL(&s_mux);
        s_cloud_acked_seq = 0;
        s_cloud_gc_seq = 0;          // FIX-BT9：连续水位随 acked 一起归零
        s_cloud_ack_pending = false;
        s_cloud_ack_boot = 0;
        s_cloud_ack_from = 0;
        s_cloud_ack_to = 0;
        portEXIT_CRITICAL(&s_mux);

        s_cloud_inflight = false;
        s_cloud_retry_n = 0;
        s_cloud_gave_up = false;
        s_cloud_give_up_seq = 0;
        s_cloud_deadline_ms = 0;
        s_cloud_backoff_until_ms = 0;
        s_cloud_last_tx_ms = 0;

        s_cloud_q_last_seq = 0;
        s_cloud_q_last_boot = 0;

        // FIX-1 / FIX-5 / FIX-2
        s_cloud_tx_rd_base = 0;
        s_cloud_tx_valid = false;
        s_cloud_hole_count = 0;
        s_cloud_hole_overflow = false;

        // DIR-1：arm 补发扫描（重启后 acked=0 ⇒ 全部未确认记录都会被重新投递）
        cloud_replay_arm();

        s_cloud_fail_next = 0;
    }

    return true;
}

// =====================================================
// log_emit
// =====================================================

static bool log_emit_internal(LogEventId event_id, LogLevel level,
                              const LogParamIn *params, uint8_t param_count,
                              bool allow_coalesce)
{
    if (!s_ready)
    {
        return false;
    }

    /* ★ `HD-3`·`A-1` fail-closed：无法证明 `boot_seq` 安全时**停止分配任何新 seq**。
       非阻塞、无 Flash I/O、不进临界区 —— 只返回 `false`（调用方本就多以忽略返回值调用）。
       取舍：宁可少记一条日志，也不产生**可能与历史记录撞 `ux_lh_dedup` 键**的
       `(boot_seq, seq)` —— 后者会被云端静默丢弃，属**不可恢复**的数据损失。
       只在首次触发时打一行串口，避免刷屏。 */
    if (s_seq_failclosed)
    {
        if (!s_failclosed_logged)
        {
            s_failclosed_logged = true;
            Serial.println(
                "[Log] fail-closed active: log_emit rejected (seq not provably unique)");
        }

        return false;
    }

    // DEBUG 策略：不进入 Log 系统（不占 RAM 环、不落盘、不上云）
    // 只累加侧信道计数，便于上板验证"DEBUG 未产生任何持久化负担"。
    if (level == LOG_LVL_DEBUG)
    {
#if LOG_DEBUG_ENABLE
        portENTER_CRITICAL(&s_mux);
        s_stats.debug_dropped++;
        portEXIT_CRITICAL(&s_mux);
#endif
        return false;
    }

    // 契约：参数数量超限整体拒绝（不截断）
    if (param_count > LOG_MAX_PARAMS)
    {
        return false;
    }

    if (param_count > 0 && params == nullptr)
    {
        return false;
    }

    // ---- ★ 突发合并：白名单事件在窗口内的重复发生只折叠计数，不产生记录 ----
    // 折叠 ⇒ 不占 seq、不进 RAM 环、不落 Flash、不上云；
    // 被接受 ⇒ 追加 LOG_P_COUNT(=1) 后按原路径继续（行为与既有实现一致）。
    LogParamIn       coalesce_p[LOG_MAX_PARAMS];
    const LogParamIn *use_p = params;
    uint8_t           use_n = param_count;

    if (allow_coalesce &&
        log_coalesce_filter(event_id, level, params, param_count,
                            coalesce_p, use_n))
    {
        return true;    // 已折叠
    }

    if (use_n != param_count)
    {
        use_p = coalesce_p;
    }

    LogRecord rec;
    memset(&rec, 0, LOG_RECORD_SIZE);

    rec.version     = (uint8_t)LOG_RECORD_VERSION;
    rec.level       = (uint8_t)level;
    rec.param_count = use_n;
    rec.event_id    = (uint16_t)event_id;
    rec.packed      = 0;                 // context_kind = 0（P1.2 未实现 Context）
    rec.uptime_ms   = (uint32_t)millis();
    rec.timestamp   = 0;
    rec.blob_len    = 0;
    rec.reserved16  = 0;

    // timestamp：仅在 TimeManager 已同步时写入并置 valid 位
    {
        time_t t = time_get();

        if (t > 0)
        {
            rec.timestamp = (uint32_t)t;
            rec.flags |= LOG_FLAG_TIMESTAMP_VALID;
        }
    }

    // 参数编码：每项 6 B { id:u8, type:u8, value:u32le }
    for (uint8_t i = 0; i < use_n; i++)
    {
        const LogParamIn &p = use_p[i];
        uint8_t *dst = &rec.params[(uint32_t)i * LOG_PARAM_SIZE];

        uint32_t raw = p.v.u;

        // BOOL 只写了 union 的 1 字节，其余字节不可信 -> 显式归一化
        if (p.type == LOG_PTYPE_BOOL)
        {
            raw = p.v.b ? 1u : 0u;
        }

        dst[0] = p.id;
        dst[1] = p.type;
        dst[2] = (uint8_t)(raw & 0xFFu);
        dst[3] = (uint8_t)((raw >> 8) & 0xFFu);
        dst[4] = (uint8_t)((raw >> 16) & 0xFFu);
        dst[5] = (uint8_t)((raw >> 24) & 0xFFu);
    }

    // ---- seq 区间用尽则先预留（§13）----
    //
    // 预留需要写 meta.bin（Flash I/O）→ **禁止放进临界区**（§3）。
    // 用 s_seq_reserving 门闩避免多任务重复预留；实际分配仍在线性临界区内。
    {
        bool need_reserve = false;

        portENTER_CRITICAL(&s_mux);

        if (s_seq_last + 1 > s_seq_limit && !s_seq_reserving)
        {
            s_seq_reserving = true;
            need_reserve = true;
        }

        portEXIT_CRITICAL(&s_mux);

        if (need_reserve)
        {
            const bool ok = seq_reserve_next();   // Flash I/O，位于临界区【外】

            portENTER_CRITICAL(&s_mux);
            s_seq_reserving = false;

            if (!ok)
            {
                // §15：meta 写失败 → sequence 可能重复，明确降级后继续工作
                s_seq_reliable = false;
                s_seq_base = s_meta_reserved + 1;
                s_seq_limit = s_seq_base + LOG_SEQ_RESERVE - 1;

                if (s_seq_last < s_seq_base - 1)
                {
                    s_seq_last = s_seq_base - 1;
                }
            }

            portEXIT_CRITICAL(&s_mux);
        }
    }

    portENTER_CRITICAL(&s_mux);

    // seq 在临界区内推进：唯一、单调（跨区间允许空洞）
    s_seq_last++;
    rec.seq = s_seq_last;
    rec.boot_seq = s_boot_seq;

    // CRC32 覆盖 [0..LOG_OFF_CRC32)
    rec.crc32 = log_crc32((const uint8_t *)&rec, LOG_OFF_CRC32);

    // 环形写入：满则淘汰最旧（side-channel 计数 ring_drop）
    uint32_t used = s_wr - s_rd;

    if (used >= LOG_RAM_QUEUE_SLOTS)
    {
        s_rd++;
        s_stats.ring_drop++;
        used--;
    }

    const uint32_t slot = s_wr % LOG_RAM_QUEUE_SLOTS;
    memcpy(&s_ring[slot], &rec, LOG_RECORD_SIZE);
    s_wr++;

    const uint32_t now_used = s_wr - s_rd;

    s_stats.emit_total++;
    s_stats.last_seq = s_seq_last;
    s_stats.ring_used = (uint8_t)now_used;

    if (now_used > (uint32_t)s_stats.ring_high_water)
    {
        s_stats.ring_high_water = (uint8_t)now_used;
    }

    portEXIT_CRITICAL(&s_mux);

    return true;
}

// 公开 API：语义与调用方式完全不变（allow_coalesce = true）
bool log_emit(LogEventId event_id, LogLevel level,
              const LogParamIn *params, uint8_t param_count)
{
    return log_emit_internal(event_id, level, params, param_count, true);
}

// =====================================================
// log_task（仅在 loop() 内调用）
// =====================================================

void log_task()
{
    if (!s_ready)
    {
        return;
    }

    // ---- 阶段 0：突发合并窗口到期 ⇒ 结清折叠计数 ----
    // 只做 RAM 入环（与普通记录同一路径），不在此处做任何 Flash I/O。
    log_coalesce_tick();

    // =================================================
    // 阶段 1：窥视 + 分拣（**不推进 s_rd**）
    //
    // ★ §22 RAM → Flash 安全交接：
    //   "Flash persistence 成功" 是 "RAM record 完成消费" 的前提。
    //   所以这里只读不动游标；真正的推进在阶段 3 按"已提交前缀"完成。
    // =================================================
    uint8_t scanned = 0;       // 本轮考察并接收的记录数
    uint8_t batch_n = 0;       // 其中需落盘、已装入批量缓冲的条数
    uint8_t flash_flags = 0;   // bit i = 第 i 条需落 Flash
    uint8_t blocked = 0;       // 队首无法被接收的条数（0 或 1）

    while (scanned < LOG_DRAIN_MAX_PER_TASK)
    {
        LogRecord rec;
        bool got = false;

        // 窥视第 scanned 条：读内容但**不推进 s_rd**
        portENTER_CRITICAL(&s_mux);

        if ((uint32_t)scanned < (s_wr - s_rd))
        {
            const uint32_t slot = (s_rd + scanned) % LOG_RAM_QUEUE_SLOTS;
            memcpy(&rec, &s_ring[slot], LOG_RECORD_SIZE);
            got = true;
        }

        portEXIT_CRITICAL(&s_mux);

        if (!got)
        {
            break;                             // 环已空
        }

        // ---- routing 决策（纯计算，不涉及任何 I/O）----
        const LogLevel lv = (LogLevel)rec.level;
        const bool to_flash = log_level_to_flash(lv);
        const bool to_cloud = log_level_to_cloud(lv);
        const bool is_critical = (lv == LOG_LVL_CRITICAL);

        // ---- 短临界区 ①：统一更新 stats 与 flush 请求 ----
        //
        // 并发规则（P1.2 fix）：所有 s_stats / s_flush_requested 的读与改
        // 一律在 s_mux 内完成，与 log_emit() / log_get_stats() /
        // log_stats_reset() / log_flush_requested() 使用同一把锁。
        portENTER_CRITICAL(&s_mux);

        if (to_flash)
        {
            s_stats.flash_routed++;
        }

        if (to_cloud)
        {
            s_stats.cloud_routed++;
        }

        s_stats.consumed++;

        if (is_critical)
        {
            s_stats.critical_seen++;

            // 冻结语义（P1 修订）：CRITICAL = Flash 立即 + 进入
            // "最高优先级下一批次"，**不建独立通道、不承诺秒级**。
            // 边沿触发：只置一次，避免重复 flush。
            // 因 CRITICAL 在本轮阶段 2 就会落盘，"立即"由此天然满足（§28）。
            if (!s_flush_requested)
            {
                s_flush_requested = true;
                s_stats.flush_requests++;
            }
        }

        portEXIT_CRITICAL(&s_mux);

        // ---- I/O 区（准备）：只做内存拷贝，不写盘 ----
        //
        // P1.3：把需落盘的记录收集进批量缓冲 —— 真正的写盘在循环外，
        //       以"一个 append 单元"为粒度（§30），避免长时间占用 loop。
        // P1.4：Cloud Log Queue 投递
        // ★ 严格要求：耗时 I/O 必须在临界区【外】执行 ——
        //   禁止把 Flash 写入或 MQTT publish 放进上面的 s_mux 段。
        if (to_flash)
        {
            if (s_flash_ready && s_flash_batch != nullptr &&
                batch_n < LOG_FLUSH_RECORDS)
            {
                memcpy(&s_flash_batch[batch_n], &rec, LOG_RECORD_SIZE);
                flash_flags |= (uint8_t)(1u << scanned);
                batch_n++;
            }
            else
            {
                // Flash 未就绪 / 缓冲异常 → 本轮无法接收该条。
                // **不推进游标**：该条及其后全部保留在 RAM 环，下轮再试（§23）。
                blocked++;
                break;
            }
        }

        // ---- P1.4：云待发入队（INFO 与 WARN+ 都要上云）----
        //
        // seq 守卫（在 cloud_queue_push 内部）：落盘失败被保留的记录下一轮
        // 会以相同 seq 再次出现，靠 seq 去重，避免队列里出现两份。
        if (to_cloud)
        {
            cloud_queue_push(rec);
        }

        scanned++;
    }

    // =================================================
    // 阶段 2：本轮唯一的 Flash append 单元（§30）
    // =================================================
    uint8_t written = 0;

    if (batch_n > 0)
    {
        written = flash_append_batch(s_flash_batch, batch_n);
    }

    // =================================================
    // 阶段 3：按序提交 —— 只有"已落盘"的前缀才推进 s_rd
    //
    //   · INFO（Flash NO）不依赖落盘，天然可提交（§24）
    //   · WARN / ERROR / CRITICAL 必须等自己的 append 成功
    //   未提交的记录留在环内，下一轮 log_task() 重试（§23）。
    // =================================================
    uint8_t commit = 0;
    uint8_t flash_seen = 0;

    for (uint8_t i = 0; i < scanned; i++)
    {
        if (flash_flags & (uint8_t)(1u << i))
        {
            flash_seen++;

            if (flash_seen > written)
            {
                break;                         // 该条未落盘 → 从此处截断
            }
        }

        commit++;
    }

    // =================================================
    // 阶段 4：提交游标 + 统计（统一 s_mux）
    // =================================================
    portENTER_CRITICAL(&s_mux);

    if (commit > 0)
    {
        const uint32_t avail = s_wr - s_rd;
        s_rd += (commit > avail) ? avail : commit;   // 防御性钳位
    }

    s_stats.flash_append_ok += written;
    s_stats.flash_append_fail += (uint32_t)(batch_n - written);

    if (written < batch_n)
    {
        // 有记录被保留、下一轮重试（§23：绝不静默丢弃 WARN+）
        s_stats.flash_retry++;
    }

    if (blocked > 0)
    {
        s_stats.flash_blocked_rounds++;
    }

    // CRITICAL 的 flush 请求在本轮确有落盘时视为已兑现并消费（§27）
    if (written > 0 && s_flush_requested)
    {
        s_flush_requested = false;
        s_stats.flush_honored++;
    }

    s_stats.ring_used = (uint8_t)(s_wr - s_rd);

    portEXIT_CRITICAL(&s_mux);

    // P1.4：云上行（内部节流 1 批 / 500 ms；不阻塞、不创建 Task）
    cloud_poll();
}

// =====================================================
// P1.4：Cloud Log Topic（CBOR Batch 上行）
//
//   依赖方向：LogManager → CloudManager（单向）
//   本阶段只做"发出去"；ACK / 超时 / 重试 / 退避 / 段删除属 P1.5。
// =====================================================

// 说明：CBOR 写入器与批次编码已抽到 src/log_cbor.h ——
// 那里只依赖 <stdint.h>/<stddef.h>/<string.h> + log_events.h，
// 不依赖 Arduino，因此可用主机编译器直接编译运行做**字节级**验证
// （见 test/log_contract/probe_cbor.cpp）。

// ---- 云队列 ----

static uint32_t cloud_queue_used_locked()
{
    return s_cloud_q_wr - s_cloud_q_rd;
}

// FIX-BT9：队首记录是否属于**本 Boot**（无记录时返回 true）
//
// 为什么用它判断"补发前缀已消费完"：
//   补发只在 `used == 0` 时推进 ⇒ 每次 push 都是往**空队列**里放 ⇒ 补发的旧记录
//   必然构成队列的**前缀**（live 记录只会在其后追加）。因此"队首已是本 Boot 记录"
//   等价于"上一 Boot 的补发记录已全部离开队列"（被 ACK / 放弃 / 淘汰）。
//   ⚠️ 不能用"队列已空"作判据：持续有 live 记录时队列可能长期非空 ⇒ 水位钳制
//      永不解除 ⇒ 段永不回收（环压力反而丢数据）。
static bool cloud_queue_head_is_current_boot()
{
    bool cur = true;

    portENTER_CRITICAL(&s_mux);

    if (cloud_queue_used_locked() > 0)
    {
        const uint32_t slot = s_cloud_q_rd % LOG_CLOUD_QUEUE_SLOTS;
        cur = (s_cloud_q[slot].boot_seq == s_boot_seq);
    }

    portEXIT_CRITICAL(&s_mux);

    return cur;
}

static void cloud_queue_push(const LogRecord &rec)
{
    if (s_cloud_q == nullptr)
    {
        return;
    }

    portENTER_CRITICAL(&s_mux);

    // 去重守卫：同一条记录（Flash 落盘失败被保留后重新路由）不重复入队。
    //
    // P1.5 调整为 (boot_seq, seq) 相等判定，而**不是** seq 单调判定：
    // 单调判定会让 Flash 补发（replay）在 RAM 队列排空后永远被拒绝 ——
    // 补发的都是旧 seq，而 s_cloud_q_last_seq 已经是新水位。
    // 重复只可能发生在"相邻两次 push"，故相等判定已足够。
    if (rec.boot_seq == s_cloud_q_last_boot &&
        rec.seq == s_cloud_q_last_seq)
    {
        portEXIT_CRITICAL(&s_mux);
        return;
    }

    if (cloud_queue_used_locked() >= LOG_CLOUD_QUEUE_SLOTS)
    {
        // §27：队列满 → FIFO 淘汰最旧
        //
        // used == SLOTS ⇒ rd % SLOTS == wr % SLOTS，故"即将被覆盖的槽位"
        // 就是被淘汰记录所在槽位，可在覆盖前读到它的元信息。
        const uint32_t evict_slot = s_cloud_q_wr % LOG_CLOUD_QUEUE_SLOTS;
        const uint8_t  evict_level = s_cloud_q[evict_slot].level;
        const uint32_t evict_seq = s_cloud_q[evict_slot].seq;
        const uint32_t evict_boot = s_cloud_q[evict_slot].boot_seq;

        // FIX-1：淘汰落在**在途批次窗口**内的记录时不计 drop_overflow。
        // 该记录已在 s_cloud_batch 副本内、仍可能被 ACK ⇒ 记成"丢弃"是多计；
        // 它的最终归宿由 ACK / give-up 路径负责记账（每条只被计一次）。
        const bool in_flight_window =
            s_cloud_tx_valid &&
            ((s_cloud_q_rd - s_cloud_tx_rd_base) < (uint32_t)s_cloud_tx_count);

        s_cloud_q_rd++;

        if (in_flight_window)
        {
            s_stats.cloud_q_evict_inflight++;
        }
        else
        {
            s_stats.cloud_q_drop++;

            // FIX-2：Flash-routed 记录在本次 Boot 内已不可能被 replay 取回
            //（rd 只在 ACK / give-up / 淘汰时推进 ⇒ 队列真正排空必然伴随 ACK
            //  落账 ⇒ 水位必然已越过它 ⇒ log_ack_should_replay() 会跳过）
            // ⇒ 登记空洞，保护其物理副本不被段回收删除，留待下次开机重放。
            if (evict_boot == s_boot_seq &&
                log_level_to_flash((LogLevel)evict_level))
            {
                if (!log_hole_add(s_cloud_holes, &s_cloud_hole_count,
                                  evict_seq, evict_seq))
                {
                    s_cloud_hole_overflow = true;
                }
            }
        }
    }

    const uint32_t slot = s_cloud_q_wr % LOG_CLOUD_QUEUE_SLOTS;
    memcpy(&s_cloud_q[slot], &rec, LOG_RECORD_SIZE);
    s_cloud_q_wr++;
    s_cloud_q_last_seq = rec.seq;
    s_cloud_q_last_boot = rec.boot_seq;
    s_stats.cloud_q_used = (uint8_t)cloud_queue_used_locked();

    portEXIT_CRITICAL(&s_mux);
}

// ---- 组批 ----

// 取队首 **LOG_ENV_RECORDS_PER_MSG（= 1）** 条。
//
// P3 · WP4-FW-LOG（H-WP4-FW-02 / 04 冻结）：
//   1 个 MQTT log message = 1 条 log_history record ⇒ 组批上界由
//   LOG_BATCH_MAX_RECORDS(16) 收敛为 LOG_ENV_RECORDS_PER_MSG(1)。
//   ★ 仅改**上界**：boot_seq 分组语义、绝对基准 s_cloud_tx_rd_base、
//     ACK / 重试 / 水位 / 段回收逻辑**一字不动**。
static uint8_t cloud_collect_batch()
{
    uint8_t n = 0;

    portENTER_CRITICAL(&s_mux);

    const uint32_t used = cloud_queue_used_locked();

    // FIX-1：登记本批次的**绝对基准**（ACK / give-up 做绝对推进的依据）
    // 注意：只有真的取到记录（used > 0）时才更新，避免 n==0 的轮次污染基准。
    if (used > 0)
    {
        s_cloud_tx_rd_base = s_cloud_q_rd;
    }

    while (n < LOG_ENV_RECORDS_PER_MSG && n < used)
    {
        const uint32_t slot = (s_cloud_q_rd + n) % LOG_CLOUD_QUEUE_SLOTS;

        if (n > 0 && s_cloud_q[slot].boot_seq != s_cloud_batch[0].boot_seq)
        {
            break;
        }

        memcpy(&s_cloud_batch[n], &s_cloud_q[slot], LOG_RECORD_SIZE);
        n++;
    }

    portEXIT_CRITICAL(&s_mux);

    return n;
}

// 组装 Cloud Log Envelope（逐条 · count = 1），返回 JSON 字节数
//
// P3 · WP4-FW-LOG：把「原始 CBOR 批次」替换为「JSON envelope」。
//   · 载荷 = log_envelope_encode_record()（内部仍走冻结的 log_cbor_encode_batch，
//     count = 1 ⇒ payload_base64 = 188 B CBOR，格式零改动）
//   · 侧信道计数（drop_* / flags）继续留在 CBOR 批次头（键值不变），随载荷承载
//   · 事务描述符 s_cloud_tx_* 的推进语义与旧实现**逐字一致**（仅 first == last）
static uint32_t cloud_build_envelope(uint8_t n)
{
    if (n != LOG_ENV_RECORDS_PER_MSG)
    {
        // 冻结：每次仅 1 条；上界由 cloud_collect_batch() 保证
        return 0u;
    }

    const LogRecord &rec = s_cloud_batch[0];

    LogEnvelopeMeta meta;
    meta.drop_ring = 0;
    meta.drop_overflow = 0;
    meta.drop_unacked = 0;
    meta.self_degraded = 0;
    meta.flags = 0;

    portENTER_CRITICAL(&s_mux);
    // 增量：自上次上报以来新增的丢弃计数（§17 侧信道）
    meta.drop_ring = s_stats.ring_drop - s_cloud_rep_ring;
    meta.drop_overflow = s_stats.cloud_q_drop - s_cloud_rep_overflow;
    meta.drop_unacked = s_stats.cloud_flash_drop - s_cloud_rep_unacked;
    meta.self_degraded = s_stats.self_degraded - s_cloud_rep_degraded;

    if (s_seq_reliable)
    {
        meta.flags |= LOG_BATCH_FLAG_SEQ_RELIABLE;
    }

    s_cloud_tx_boot = rec.boot_seq;
    s_cloud_tx_from = rec.seq;
    s_cloud_tx_to = rec.seq;
    s_cloud_tx_count = LOG_ENV_RECORDS_PER_MSG;
    s_cloud_flags = (uint8_t)meta.flags;
    portEXIT_CRITICAL(&s_mux);

    // 信封 / Base64 编码交给 src/log/log_envelope.h（可在主机上字节级验证）。
    // device_id 必须与 /log topic 同源 ⇒ 只读调用 device_id()。
    return log_envelope_encode_record(
        rec,
        meta,
        device_id(),
        (char *)s_cloud_cbor,
        LOG_BATCH_MAX_PAYLOAD);
}

// ---- 发送调度（P1.5：ACK 驱动推进 + 超时重试退避 + 离线补发）----

// DIR-1：取"first_seq 严格大于 after_seq"的、first_seq 最小的段
//
// 为什么不再按 seq 反查段（原 flash_find_segment_of_seq）：
//   段内 seq **稀疏**（INFO 不落 Flash）⇒ `slots == seq 范围` 的假设不成立，
//   按 `seq - first_seq` 反算 slot 会指向错误槽位；而用
//   `first + records - 1` 判"该 seq 在不在本段"又会因派生值偏小**提前终止**补发。
//   改为"按 first_seq 升序遍历段 + 段内按 slot 遍历 + 用记录自身 seq 判定"后，
//   两个问题同时消失，且不再依赖任何稠密假设。
//
// have_after = false ⇒ 取全局 first_seq 最小者（补发起点）
static uint8_t flash_next_segment_after(uint32_t after_seq, bool have_after)
{
    uint8_t best = 0xFF;
    uint32_t best_first = 0;

    for (uint8_t seg = 0; seg < LOG_SEGMENT_COUNT; seg++)
    {
        if (s_seg_present[seg] == 0 || s_seg_records[seg] == 0)
        {
            continue;
        }

        const uint32_t first = s_seg_first_seq[seg];

        if (have_after && first <= after_seq)
        {
            continue;
        }

        if (best == 0xFF || first < best_first)
        {
            best = seg;
            best_first = first;
        }
    }

    return best;
}

// FIX-BT9：Flash 里现存记录的**最低 seq**（= 尚未补发/确认的 backlog 下界）
//
// 用途：`s_cloud_gc_floor` 的取值 —— 只要它非 0，可回收水位就被钳在
//       "backlog 下界之前"，从而保证补发不会被跳过的旧记录之外的高水位越过。
//
// 为什么取 `s_seg_first_seq[]` 的最小值（而不是逐条读记录）：
//   · 段首 seq 是段的**下确界** ⇒ 用它做钳制只会**更保守**（不会放行不该放的）
//   · 无需 Flash I/O、可在任意上下文调用（含临界区内）
//   · 空段 / 不存在段跳过；全无 ⇒ 返回 0（无 backlog ⇒ 不钳制）
static uint32_t flash_lowest_first_seq()
{
    uint32_t best = 0;

    for (uint8_t seg = 0; seg < LOG_SEGMENT_COUNT; seg++)
    {
        if (s_seg_present[seg] == 0 || s_seg_records[seg] == 0)
        {
            continue;
        }

        const uint32_t first = s_seg_first_seq[seg];

        if (first != 0 && (best == 0 || first < best))
        {
            best = first;
        }
    }

    return best;
}

// 回收"整段已被 ACK 覆盖"的 Flash 段
//
// §13-partial：部分覆盖**绝不**删段 —— 判定唯一入口是
// log_ack_segment_reclaimable()（整段 ≤ 水位 ∧ 不与空洞相交 ∧ 空洞表未溢出）。
// ⚠️ 禁止改用 log_ack_segment_deletable_legacy()（按稠密 seq 推导末条 seq，会被
//    INFO 造成的 seq 空洞骗到 ⇒ 段在尚有未确认记录时被判可回收）。
// 只由 loop 上下文调用（LittleFS I/O）。
static void cloud_delete_acked_segments()
{
    // FIX-BT9：用**连续**水位（不再是"确认最大值"）。gc_seq == 0 ⇒ 没有任何
    // 连续前缀被确认 ⇒ 一律不可回收（含"钳制到 0"的情形，保守且等价于早退）。
    if (!s_flash_ready || s_cloud_gc_seq == 0)
    {
        return;
    }

    for (uint8_t seg = 0; seg < LOG_SEGMENT_COUNT; seg++)
    {
        if (s_seg_present[seg] == 0)
        {
            continue;
        }

        if (seg == s_append_segment)
        {
            continue;   // 追加目标不动
        }

        if (s_seg_records[seg] == 0)
        {
            continue;   // 空段（可能是即将复用的槽位）
        }

        if (s_replay_armed && s_replay_seg == seg)
        {
            continue;   // 补发游标正在扫这一段，先留着（DIR-1）
        }

        // FIX-2 + FIX-3b：整段 ≤ 水位 **且** 与空洞不相交 **且** 末条 seq 为实测值
        // FIX-BT9：水位用 `s_cloud_gc_seq`（连续水位）—— 用确认最大值会把
        //          "尚未补发/确认的旧记录"所在段一并删掉。
        if (!log_ack_segment_reclaimable(
                s_seg_first_seq[seg], s_seg_last_seq[seg], s_cloud_gc_seq,
                s_cloud_holes, s_cloud_hole_count, s_cloud_hole_overflow))
        {
            continue;
        }

        if (flash_drop_segment(seg))
        {
            portENTER_CRITICAL(&s_mux);
            s_stats.cloud_seg_acked_del++;
            portEXIT_CRITICAL(&s_mux);

            Serial.printf("[Log Cloud] segment %u fully acked -> deleted\n",
                          (unsigned)seg);
        }
    }
}

// ACK 落账（只在 loop 上下文调用；回调只置标志，不在这里做 I/O）
static void cloud_handle_ack()
{
    if (!s_cloud_ack_pending)
    {
        return;
    }

    uint32_t ack_boot = 0;
    uint32_t ack_from = 0;
    uint32_t ack_to = 0;

    portENTER_CRITICAL(&s_mux);
    s_cloud_ack_pending = false;
    ack_boot = s_cloud_ack_boot;
    ack_from = s_cloud_ack_from;
    ack_to = s_cloud_ack_to;
    portEXIT_CRITICAL(&s_mux);

    const LogAckResult r = log_ack_classify(
        ack_boot, ack_from, ack_to,
        s_cloud_tx_valid ? s_cloud_tx_boot : 0u,
        s_cloud_tx_valid ? s_cloud_tx_from : 0u,
        s_cloud_tx_valid ? s_cloud_tx_to : 0u,
        // FIX-BT9：重复/回退检测必须用**连续**水位。用"确认最大值"会把
        // "晚到的、属于更旧批次的合法 ACK"误判成 DUPLICATE 而丢弃 ⇒
        // 那些记录永远留在云队列里（补发被自身阻塞 ⇒ 比 BT-9 更糟）。
        s_cloud_gc_seq);

    if (r != LOG_ACK_ACCEPT && r != LOG_ACK_PARTIAL)
    {
        portENTER_CRITICAL(&s_mux);
        s_stats.cloud_ack_ignored++;
        portEXIT_CRITICAL(&s_mux);

        Serial.printf(
            "[Log Cloud] ack ignored boot=%u from=%u to=%u r=%d\n",
            (unsigned)ack_boot, (unsigned)ack_from, (unsigned)ack_to, (int)r);
        return;
    }

    const uint8_t covered =
        log_ack_covered_count(s_cloud_batch, s_cloud_tx_count, ack_to);

    if (covered == 0)
    {
        portENTER_CRITICAL(&s_mux);
        s_stats.cloud_ack_ignored++;
        portEXIT_CRITICAL(&s_mux);
        return;
    }

    const uint32_t new_acked = s_cloud_batch[covered - 1u].seq;
    const uint8_t tx_count = s_cloud_tx_count;

    portENTER_CRITICAL(&s_mux);

    // FIX-1：**绝对基准**推进
    //
    //   evicted = 本批次在途期间被溢出淘汰走的条数
    //   rd      = rd_base + max(evicted, covered)
    //
    // 原写法 `rd += covered` 把"从取批时队首起算的相对量"加到**已漂移**的
    // 游标上 ⇒ 跳过一条既未发送也未计数的记录。
    const uint32_t evicted = s_cloud_q_rd - s_cloud_tx_rd_base;
    const uint32_t adv = ((uint32_t)covered > evicted) ? (uint32_t)covered : evicted;
    uint32_t new_rd = s_cloud_tx_rd_base + adv;

    if (new_rd > s_cloud_q_wr)
    {
        new_rd = s_cloud_q_wr;   // 防御性钳位（正常不可能触发）
    }

    s_cloud_q_rd = new_rd;

    // FIX-BT9：两个水位分别推进（语义分离，见顶部 s_cloud_gc_seq 注释）
    //   acked_seq = 真实最大值（观测用，单调）
    //   gc_seq    = 连续可回收水位 = min(acked_seq, gc_floor - 1)，**单调不降**
    if (new_acked > s_cloud_acked_seq)
    {
        s_cloud_acked_seq = new_acked;
    }

    {
        const uint32_t gc = log_ack_gc_watermark(s_cloud_acked_seq, s_cloud_gc_floor);

        if (gc > s_cloud_gc_seq)
        {
            s_cloud_gc_seq = gc;
        }
    }
    s_stats.cloud_q_used = (uint8_t)cloud_queue_used_locked();
    s_stats.cloud_ack_ok++;
    if (r == LOG_ACK_PARTIAL)
    {
        s_stats.cloud_ack_partial++;
    }

    // ---- FIX-H2：为"在途窗口内被淘汰、且**未被本次 ACK 覆盖**"的区间
    //      补齐空洞保护（FIX-1 × FIX-2 的交界缺口）----
    //
    // 判定与执行分离：区间/类型判定全部在 `log_hole_protect_gap()`
    // （`src/log_ack.h`，freestanding ⇒ 可主机编译并用 node 跑 wasm 断言，
    //   见 `test/log_contract/probe_ack.cpp`）；这里只做原地登记与记账。
    //
    // `evicted` 为在途期间从队首淘汰的条数（见上方 FIX-1 说明）：
    //   [0, covered)        已被 ACK                     ⇒ 无需保护
    //   [covered, evicted)  被淘汰且未确认                 ⇒ ★本段补齐
    //   [evicted, tx_count) 已由 push 的 else 分支登记     ⇒ 跳过（互斥）
    if (evicted > (uint32_t)covered)
    {
        bool gap_overflow = false;

        const uint8_t gap_protected = log_hole_protect_gap(
            s_cloud_batch, tx_count, covered, evicted,
            s_cloud_holes, &s_cloud_hole_count, &gap_overflow);

        s_stats.cloud_hole_from_evict += gap_protected;

        if (gap_overflow)
        {
            s_cloud_hole_overflow = true;   // 表满 ⇒ 停止回收（保守）
        }
    }

    portEXIT_CRITICAL(&s_mux);

    // 未被覆盖的尾部仍留在云队列里，下一轮重新组批发送（不丢、不重复记账）
    s_cloud_inflight = false;
    s_cloud_tx_valid = false;   // FIX-5：描述符已消费
    s_cloud_retry_n = 0;
    s_cloud_gave_up = false;
    s_cloud_backoff_until_ms = 0;

    Serial.printf(
        "[Log Cloud] ack ok boot=%u to=%u covered=%u/%u acked=%u gc=%u floor=%u r=%d\n",
        (unsigned)ack_boot, (unsigned)ack_to,
        (unsigned)covered, (unsigned)tx_count,
        (unsigned)s_cloud_acked_seq, (unsigned)s_cloud_gc_seq,
        (unsigned)s_cloud_gc_floor, (int)r);

    cloud_delete_acked_segments();
}

// Flash -> 云 补发一步
//
// 触发条件（cloud_poll 内）：在线、无在途批次、退避结束、RAM 云队列已空。
// 目的：离线期间 WARN+ 已落 Flash（INFO 不落盘，只走 RAM 队列），
//       重连后必须把这些记录补送出去，否则云端永久缺失。
// 重启后 s_cloud_acked_seq 归 0 ⇒ 曾发过的记录会重发，这属**正常行为**
// （§25），由云端按 (device_id, boot_seq, seq) 幂等去重消化。
// DIR-1：重新 arm 补发扫描（从 first_seq 最小的段重新开始）
static void cloud_replay_arm()
{
    s_replay_seg = 0xFF;
    s_replay_idx = 0;
    s_replay_armed = true;
    s_replay_done = false;

    // FIX-D1：全新一轮 sweep ⇒ 不携带"从某 seq 之后继续"的约束
    //（该约束只由 flash_drop_segment() 在"游标段被删"时设置，消费一次即失效）
    s_replay_after_seq = 0;
    s_replay_after_valid = false;

    // FIX-BT9：本轮补发的 backlog 下界 ⇒ 立即钳制可回收水位。
    //
    // 这是本修复的关键时序：**arm 发生在任何 ACK 之前**（log_init /
    // log_cloud_test_reset / 游标段被删），因此"用 live 记录的批量 ACK 把
    // 水位推高"这件事必然发生在钳制生效之后 ⇒ 旧记录不会被越过。
    //
    // 取"现存段最低 first_seq"而不是"本轮实际要补发的最低 seq"：
    // 后者需要读记录（I/O），前者是下确界 ⇒ 只会更保守，且零 I/O。
    s_cloud_gc_floor = flash_lowest_first_seq();
}

// Flash -> 云 补发一步（DIR-1：**按 slot 遍历**，不再按 seq 反算）
//
// 触发条件（cloud_poll 内）：在线、无在途批次、退避结束、RAM 云队列已空。
// 目的：离线期间 WARN+ 已落 Flash（INFO 不落盘，只走 RAM 队列），
//       重连后必须把这些记录补送出去，否则云端永久缺失。
// 重启后 s_cloud_acked_seq 归 0 ⇒ 曾发过的记录会重发，这属**正常行为**
// （§25），由云端按 (device_id, boot_seq, seq) 幂等去重消化。
//
// ★ 为什么不按 seq 遍历（旧实现的 CRITICAL 级缺陷）：
//   段内 seq **稀疏**（`seq` 每条非 DEBUG 记录都消耗，但 INFO 不落 Flash）⇒
//     ① `idx = seq - first_seq` 指向**错误槽位**
//     ② 终止判定 `first + records - 1 < seq` 会**提前成立** ⇒ 补发在本 Boot 内
//        **永久终止**，其后所有段中未确认的 WARN+ 静默丢失
//   改为按 slot 遍历后：读到的就是记录本身，判定用记录自身的 seq。
//
// ★ 每轮只扫**一个段**（I/O 有界，且只需 open 一次），段间切换放到下一次调用。
static void cloud_replay_step()
{
    if (!s_flash_ready || s_cloud_q == nullptr)
    {
        s_replay_armed = false;
        s_replay_done = true;
        return;
    }

    if (!s_replay_armed)
    {
        return;
    }

    // 首轮 / 段删除后重新选取起始段（FIX-D1）
    //
    //   after_valid=false ⇒ 从 first_seq 最小的段开始（全新一轮 sweep）
    //   after_valid=true  ⇒ 从 first_seq > after_seq 的段继续（游标段刚被删）
    if (s_replay_seg == 0xFF)
    {
        s_replay_seg = flash_next_segment_after(s_replay_after_seq,
                                                s_replay_after_valid);
        s_replay_idx = 0;

        // 约束只作用于"本轮的起始段选择"，消费后立即清除，
        // 避免后续 arm（正常情况下由 cloud_replay_arm 显式清零）误用陈旧值。
        s_replay_after_valid = false;

        if (s_replay_seg == 0xFF)
        {
            s_replay_armed = false;
            s_replay_done = true;
            return;
        }
    }

    // 本段已扫完 → 游标移到下一段并**返回**（下次调用再扫，I/O 有界）
    if (s_replay_idx >= s_seg_records[s_replay_seg])
    {
        const uint32_t cur_first = s_seg_first_seq[s_replay_seg];

        s_replay_seg = flash_next_segment_after(cur_first, true);
        s_replay_idx = 0;

        if (s_replay_seg == 0xFF)
        {
            s_replay_armed = false;
            s_replay_done = true;

            Serial.printf("[Log Cloud] replay sweep done (acked=%u gc=%u floor=%u giveup=%u)\n",
                          (unsigned)s_cloud_acked_seq,
                          (unsigned)s_cloud_gc_seq,
                          (unsigned)s_cloud_gc_floor,
                          (unsigned)s_cloud_give_up_seq);
        }
        return;
    }

    char path[32];
    flash_seg_path(s_replay_seg, path, sizeof(path));

    File f = LittleFS.open(path, "r");

    if (!f)
    {
        // 段不可读 → 视为本段已扫完（下次调用切下一段）
        s_replay_idx = s_seg_records[s_replay_seg];
        return;
    }

    uint8_t pushed = 0;
    uint32_t scanned = 0;

    while (s_replay_idx < s_seg_records[s_replay_seg] &&
           scanned < LOG_REPLAY_SCAN_MAX_PER_CALL &&
           pushed < LOG_BATCH_MAX_RECORDS)
    {
        const uint32_t off = LOG_SEGMENT_HEADER_SIZE +
                             (uint32_t)s_replay_idx * LOG_RECORD_SIZE;

        LogRecord rec;
        const bool ok =
            f.seek(off) &&
            (f.read((uint8_t *)&rec, LOG_RECORD_SIZE) == (size_t)LOG_RECORD_SIZE) &&
            flash_check_record(rec);

        s_replay_idx++;
        scanned++;

        if (!ok)
        {
            // 单条损坏：跳过该槽（损坏语义由 §19 的扫描阶段负责）
            continue;
        }

        s_replay_diag_seq = rec.seq;   // 诊断（非游标：游标是 seg/idx）

        // 水位判定用**记录自身的 seq**（不再依赖任何稠密假设）
        //
        // FIX-BT9：水位必须传**连续**水位 `s_cloud_gc_seq`，不能传
        // `s_cloud_acked_seq`。否则 Boot 期一条 live 记录被 ACK 后，水位会
        // 跨过本条（旧记录）⇒ 永久跳过补发（at-least-once 破坏）。
        if (!log_ack_should_replay(rec.seq, s_cloud_gc_seq, s_cloud_give_up_seq))
        {
            continue;
        }

        cloud_queue_push(rec);

        portENTER_CRITICAL(&s_mux);
        s_stats.cloud_replay_records++;
        portEXIT_CRITICAL(&s_mux);

        pushed++;
    }

    f.close();
}

// 编码 + 发送一批；成功才置在途
static bool cloud_try_send_batch(uint8_t n)
{
    const uint32_t len = cloud_build_envelope(n);

    if (len == 0 || len > LOG_BATCH_MAX_PAYLOAD)
    {
        portENTER_CRITICAL(&s_mux);
        s_stats.self_degraded++;
        portEXIT_CRITICAL(&s_mux);
        return false;
    }

    s_cloud_cbor_len = len;

    if (s_cloud_fail_next > 0)
    {
        s_cloud_fail_next--;      // 故障注入：模拟离线
        return false;
    }

    if (!s_cloud_force_online && !cloud_send_log(s_cloud_cbor, len))
    {
        return false;
    }

    s_cloud_inflight = true;

    // FIX-5：描述符有效期与"是否正在等待"解耦 —— 超时进入退避后仍保持有效，
    // 使晚到的 ACK 依然能被匹配（否则会被 log_ack_classify 规则②丢弃）。
    s_cloud_tx_valid = true;

    s_cloud_deadline_ms = millis() + s_ack_timeout_ms;

    portENTER_CRITICAL(&s_mux);
    s_stats.cloud_batch_sent++;

    // ★ `G4-R4`/对账修复（`2026-10-20`）：`cloud_records_sent` 此前**没有自增点**
    //   ⇒ `system.stats.counters.cloud_records_sent` 恒为 0（`S-1` 登记项）。
    //   后果：设备侧"发出多少条"不可测量 ⇒ §4.3 要求的
    //   「设备发出 / Broker 接收 / 云端接收 / 入库」四段对账**缺第一段**。
    //   语义（与字段名注释一致）：只计**首次发出**；ACK 超时后的重发由
    //   `cloud_retx` 单独计数（`s_cloud_retry_n > 0` ⇒ 本次是重发）。
    if (s_cloud_retry_n == 0)
    {
        s_stats.cloud_records_sent += n;
    }
    portEXIT_CRITICAL(&s_mux);

    return true;
}

// 放弃当前批次（GIVE_UP_NOT_ADVANCE）
//
// 只放弃**发送**：把该批从 RAM 云队列移出并计入 drop_unacked，
// 但 ack 水位与 Flash 段**都不推进** —— 记录仍在 Flash 中，
// 下次开机（acked_seq 归 0）会重新补发。
static void cloud_give_up_batch()
{
    const uint8_t n = s_cloud_tx_count;

    portENTER_CRITICAL(&s_mux);

    // FIX-1：与 ACK 落账同构的**绝对基准**推进（见 cloud_handle_ack 注释）
    const uint32_t evicted = s_cloud_q_rd - s_cloud_tx_rd_base;
    const uint32_t adv = ((uint32_t)n > evicted) ? (uint32_t)n : evicted;
    uint32_t new_rd = s_cloud_tx_rd_base + adv;

    if (new_rd > s_cloud_q_wr)
    {
        new_rd = s_cloud_q_wr;
    }

    s_cloud_q_rd = new_rd;

    s_stats.cloud_q_used = (uint8_t)cloud_queue_used_locked();
    s_stats.cloud_flash_drop += n;   // -> drop_unacked（每条只在此处记一次）
    s_stats.cloud_ack_lost++;
    s_stats.cloud_give_up++;
    portEXIT_CRITICAL(&s_mux);

    // FIX-2：登记空洞 —— 本批次不再于本 Boot 内重发，但**物理副本必须留住**
    //（GIVE_UP_NOT_ADVANCE 的语义是"留待下次开机重放"）。
    // 若不登记，后续更高的 ACK 水位会跨过它们，段回收会把整段删掉。
    if (!log_hole_add(s_cloud_holes, &s_cloud_hole_count,
                      s_cloud_tx_from, s_cloud_tx_to))
    {
        s_cloud_hole_overflow = true;   // 表满 ⇒ 停止回收（保守）
    }

    s_cloud_gave_up = true;
    s_cloud_give_up_seq = s_cloud_tx_to;
    s_cloud_inflight = false;
    s_cloud_tx_valid = false;   // FIX-5：描述符已作废
    s_cloud_retry_n = 0;

    Serial.printf(
        "[Log Cloud] give up (NOT advance) boot=%u from=%u to=%u n=%u\n",
        (unsigned)s_cloud_tx_boot, (unsigned)s_cloud_tx_from,
        (unsigned)s_cloud_tx_to, (unsigned)n);
}

static void cloud_poll()
{
    if (s_cloud_q == nullptr || s_cloud_batch == nullptr || s_cloud_cbor == nullptr)
    {
        return;
    }

    const uint32_t now = millis();

    // (1) ACK 落账 —— 回调只置标志，处理在这里做（可安全做 Flash I/O）
    cloud_handle_ack();

    // (2) 在途批次：等 ACK，或超时 → 重发 / 放弃
    if (s_cloud_inflight)
    {
        if ((int32_t)(now - s_cloud_deadline_ms) < 0)
        {
            return;
        }

        portENTER_CRITICAL(&s_mux);
        s_stats.cloud_ack_timeout++;
        portEXIT_CRITICAL(&s_mux);

        s_cloud_retry_n++;

        if (log_ack_should_give_up(s_cloud_retry_n))
        {
            cloud_give_up_batch();
            return;
        }

        const uint32_t backoff =
            log_ack_backoff_ms_ex(s_cloud_retry_n, s_backoff_base_ms);

        Serial.printf(
            "[Log Cloud] ack timeout retry=%u/%u backoff=%ums\n",
            (unsigned)s_cloud_retry_n, (unsigned)LOG_ACK_MAX_RETRY,
            (unsigned)backoff);

        portENTER_CRITICAL(&s_mux);
        s_stats.cloud_retx++;
        portEXIT_CRITICAL(&s_mux);

        s_cloud_backoff_until_ms = now + backoff;
        s_cloud_inflight = false;   // 退避结束后由 (3)(7) 重新发送
        return;
    }

    // (3) 退避中
    if (s_cloud_backoff_until_ms != 0 &&
        (int32_t)(now - s_cloud_backoff_until_ms) < 0)
    {
        return;
    }

    s_cloud_backoff_until_ms = 0;

    // (4) 离线：不消费任何记录（留在云队列 / Flash，等重连）
    if (!s_cloud_force_online && !cloud_is_connected())
    {
        portENTER_CRITICAL(&s_mux);
        s_stats.cloud_offline_skip++;
        portEXIT_CRITICAL(&s_mux);
        return;
    }

    // (5) 节流：最快 1 批 / LOG_TX_MIN_INTERVAL_MS
    if (s_cloud_last_tx_ms != 0 &&
        (uint32_t)(now - s_cloud_last_tx_ms) < LOG_TX_MIN_INTERVAL_MS)
    {
        return;
    }

    // (6) RAM 云队列已空 → 从 Flash 补发离线积压
    uint32_t used = 0;

    portENTER_CRITICAL(&s_mux);
    used = cloud_queue_used_locked();
    portEXIT_CRITICAL(&s_mux);

    if (used == 0)
    {
        cloud_replay_step();
    }

    // (6b) FIX-BT9：解除水位钳制
    //
    // 条件 = ①本轮补发已结束（否则还有旧记录没投递）
    //        ②队列里已无上一 Boot 的记录（队首即本 Boot ⇒ 旧记录构成的前缀已消费完）
    // 两个条件同时成立 ⇒ backlog 已全部投递（被 ACK / 放弃(已登记空洞) /
    // 淘汰(已登记空洞)）⇒ 可回收水位允许回到真实确认值。
    //
    // ⚠️ 与 `used` 无关地判定：队列可能因持续 live 记录而长期非空（永不为 0），
    //    若把"队列空"当必要条件会导致钳制永不解除 ⇒ 段永不回收。
    if (!s_replay_armed && s_cloud_gc_floor != 0 &&
        cloud_queue_head_is_current_boot())
    {
        s_cloud_gc_floor = 0;

        portENTER_CRITICAL(&s_mux);

        const uint32_t gc = log_ack_gc_watermark(s_cloud_acked_seq, 0u);

        if (gc > s_cloud_gc_seq)
        {
            s_cloud_gc_seq = gc;
        }

        portEXIT_CRITICAL(&s_mux);

        // 钳制期间被抑制的回收在此补做一次（此后由 ACK 路径继续驱动）
        cloud_delete_acked_segments();
    }

    // (7) 取一批
    const uint8_t n = cloud_collect_batch();

    if (n == 0)
    {
        return;
    }

    s_cloud_last_tx_ms = now;

    if (!cloud_try_send_batch(n))
    {
        // 发送失败：不推进、不淘汰；退避一小段再试（避免离线空转刷串口）
        s_cloud_backoff_until_ms = now + LOG_TX_OFFLINE_BACKOFF_MS;
    }
}
// ---- 云侧缓冲分配（PSRAM 优先 / DRAM 回退）----

static bool cloud_init_buffers()
{
    const size_t q_bytes = (size_t)LOG_CLOUD_QUEUE_SLOTS * LOG_RECORD_SIZE;
    const size_t b_bytes = (size_t)LOG_BATCH_MAX_RECORDS * LOG_RECORD_SIZE;

    bool psram = true;

    uint8_t q_in_psram = 0;
    void *mem = gf_alloc_psram_first(q_bytes, MEM_SITE_LOG_CLOUD_Q, &q_in_psram);
    psram = (q_in_psram != 0u);

    if (mem == nullptr)
    {
        Serial.printf("[Log] Cloud queue alloc failed (%u B)\n",
                      (unsigned)q_bytes);
        return false;
    }

    s_cloud_q = (LogRecord *)mem;
    s_cloud_q_in_psram = psram;
    s_cloud_q_bytes = (uint32_t)q_bytes;
    memset(s_cloud_q, 0, q_bytes);

    void *bmem = gf_alloc_psram_first(b_bytes, MEM_SITE_LOG_CLOUD_BATCH, nullptr);

    if (bmem == nullptr)
    {
        Serial.printf("[Log] Cloud batch alloc failed (%u B)\n",
                      (unsigned)b_bytes);
        return false;
    }

    s_cloud_batch = (LogRecord *)bmem;
    memset(s_cloud_batch, 0, b_bytes);

    void *cmem = gf_alloc_psram_first(LOG_BATCH_MAX_PAYLOAD, MEM_SITE_LOG_CBOR, nullptr);

    if (cmem == nullptr)
    {
        Serial.printf("[Log] Cloud CBOR alloc failed (%u B)\n",
                      (unsigned)LOG_BATCH_MAX_PAYLOAD);
        return false;
    }

    s_cloud_cbor = (uint8_t *)cmem;
    memset(s_cloud_cbor, 0, LOG_BATCH_MAX_PAYLOAD);

    s_cloud_buf_in_psram = psram;

    Serial.printf(
        "[Log] Cloud queue ready: %u slots x %u B = %u B (q=%s), "
        "batch=%u B, cbor=%u B in %s\n",
        (unsigned)LOG_CLOUD_QUEUE_SLOTS,
        (unsigned)LOG_RECORD_SIZE,
        (unsigned)q_bytes,
        psram ? "PSRAM" : "DRAM",
        (unsigned)b_bytes,
        (unsigned)LOG_BATCH_MAX_PAYLOAD,
        (s_cloud_buf_in_psram && bmem != nullptr) ? "PSRAM" : "mix");

    return true;
}

// =====================================================
// P1.4 / P1.5 观测与测试钩子
// =====================================================

void log_cloud_get_info(LogCloudInfo &out)
{
    memset(&out, 0, sizeof(out));

    portENTER_CRITICAL(&s_mux);
    out.queue_used = (uint8_t)cloud_queue_used_locked();
    out.acked_seq = s_cloud_acked_seq;
    out.inflight = s_cloud_inflight ? 1u : 0u;
    out.tx_boot_seq = s_cloud_tx_boot;
    out.tx_from = s_cloud_tx_from;
    out.tx_to = s_cloud_tx_to;
    out.tx_count = s_cloud_tx_count;
    portEXIT_CRITICAL(&s_mux);

    out.queue_ready = (s_cloud_q != nullptr) ? 1u : 0u;
    out.queue_in_psram = s_cloud_q_in_psram ? 1u : 0u;
    out.queue_bytes = s_cloud_q_bytes;
    out.last_batch_bytes = s_cloud_cbor_len;
    out.connected = cloud_is_connected() ? 1u : 0u;

    // ---- P1.5 ----
    out.retry = (uint8_t)((s_cloud_retry_n > LOG_ACK_MAX_RETRY)
                              ? LOG_ACK_MAX_RETRY
                              : s_cloud_retry_n);
    out.gave_up = s_cloud_gave_up ? 1u : 0u;
    out.give_up_seq = s_cloud_give_up_seq;
    out.replay_seq = s_replay_diag_seq;   // 公开观测名保持 replay_seq（诊断值）
    out.replay_done = s_replay_done ? 1u : 0u;

    const uint32_t now = millis();

    if (s_cloud_inflight)
    {
        out.next_tx_in_ms = ((int32_t)(s_cloud_deadline_ms - now) > 0)
                                ? (s_cloud_deadline_ms - now)
                                : 0u;
    }
    else if ((int32_t)(s_cloud_backoff_until_ms - now) > 0)
    {
        out.next_tx_in_ms = s_cloud_backoff_until_ms - now;
    }
    else
    {
        out.next_tx_in_ms = 0u;
    }

    out.force_online = s_cloud_force_online ? 1u : 0u;
    out.ack_timeout_ms = s_ack_timeout_ms;
    out.backoff_base_ms = s_backoff_base_ms;

    // ---- FIX-1 / FIX-2 / FIX-3 / DIR-1 观测 ----
    out.tx_valid = s_cloud_tx_valid ? 1u : 0u;
    out.tx_rd_base = s_cloud_tx_rd_base;
    out.hole_count = s_cloud_hole_count;
    out.hole_overflow = s_cloud_hole_overflow ? 1u : 0u;
    out.replay_seg = s_replay_seg;
    out.replay_idx = s_replay_idx;
    out.replay_armed = s_replay_armed ? 1u : 0u;

    // FIX-BT9：水位分离观测（gc_seq 是判定用值，acked_seq 是真实确认最大值）
    portENTER_CRITICAL(&s_mux);
    out.gc_seq = s_cloud_gc_seq;
    portEXIT_CRITICAL(&s_mux);

    out.gc_floor = s_cloud_gc_floor;
}

bool log_cloud_test_push(LogEventId event_id, LogLevel level)
{
    if (s_cloud_q == nullptr)
    {
        return false;
    }

    LogRecord rec;
    memset(&rec, 0, sizeof(rec));

    rec.version = (uint8_t)LOG_RECORD_VERSION;
    rec.level = (uint8_t)level;
    rec.flags = 0;
    rec.param_count = 0;
    rec.event_id = (uint16_t)event_id;
    rec.packed = 0;
    rec.uptime_ms = millis();
    rec.timestamp = 0;
    rec.blob_len = 0;
    rec.reserved16 = 0;

    portENTER_CRITICAL(&s_mux);
    s_seq_last++;
    rec.seq = s_seq_last;
    rec.boot_seq = s_boot_seq;
    portEXIT_CRITICAL(&s_mux);

    rec.crc32 = log_crc32((const uint8_t *)&rec, LOG_OFF_CRC32);

    cloud_queue_push(rec);
    return true;
}

void log_cloud_test_reset()
{
    portENTER_CRITICAL(&s_mux);
    s_cloud_q_rd = s_cloud_q_wr;         // 清空队列
    s_cloud_q_last_seq = 0;
    s_cloud_q_last_boot = 0;
    s_cloud_acked_seq = 0;
    s_cloud_gc_seq = 0;          // FIX-BT9：连续水位一并归零（floor 由 arm 重设）
    s_cloud_inflight = false;
    s_cloud_cbor_len = 0;
    s_cloud_last_tx_ms = 0;
    s_cloud_ack_pending = false;
    s_cloud_ack_boot = 0;
    s_cloud_ack_from = 0;
    s_cloud_ack_to = 0;
    s_stats.cloud_q_used = 0;
    s_cloud_rep_ring = s_stats.ring_drop;
    s_cloud_rep_overflow = s_stats.cloud_q_drop;
    s_cloud_rep_unacked = s_stats.cloud_flash_drop;
    s_cloud_rep_degraded = s_stats.self_degraded;
    portEXIT_CRITICAL(&s_mux);

    s_cloud_retry_n = 0;
    s_cloud_gave_up = false;
    s_cloud_give_up_seq = 0;
    s_cloud_deadline_ms = 0;
    s_cloud_backoff_until_ms = 0;
    s_cloud_fail_next = 0;
    s_cloud_tx_rd_base = 0;
    s_cloud_tx_valid = false;
    s_cloud_hole_count = 0;
    s_cloud_hole_overflow = false;
    cloud_replay_arm();
    s_cloud_force_online = false;
    s_ack_timeout_ms = LOG_ACK_TIMEOUT_MS;
    s_backoff_base_ms = LOG_ACK_BACKOFF_BASE_MS;
}

// 直接注入一条 log_ack（不经过 MQTT）——上板自测用。
//
// 与 CloudManager 回调走**同一条**路径（置标志 → cloud_poll() 处理），
// 因此它验证的是真实的 ACK 状态机，而不是测试专用的旁路。
void log_cloud_test_ack(uint32_t boot_seq, uint32_t seq_from, uint32_t seq_to)
{
    portENTER_CRITICAL(&s_mux);
    s_cloud_ack_boot = boot_seq;
    s_cloud_ack_from = seq_from;
    s_cloud_ack_to = seq_to;
    s_cloud_ack_pending = true;
    portEXIT_CRITICAL(&s_mux);
}

void log_cloud_test_fail_next(uint8_t count)
{
    s_cloud_fail_next = count;
}

// 上板自测：强制"在线"，并跳过真实 cloud_send_log()。
// 用于在没有 MQTT 对端的条件下驱动 ACK / 超时 / 退避 / 放弃全链路。
void log_cloud_test_set_online(bool on)
{
    s_cloud_force_online = on;
}

// 上板自测：临时缩短 ACK 超时 / 退避基数（0 = 恢复冻结默认值）。
// 用于把"超时重试 / 重试耗尽"压进秒级；正式固件不得调用。
void log_cloud_test_set_ack_timeout(uint32_t ms)
{
    s_ack_timeout_ms = (ms == 0u) ? LOG_ACK_TIMEOUT_MS : ms;
}

void log_cloud_test_set_backoff_base(uint32_t ms)
{
    s_backoff_base_ms = (ms == 0u) ? LOG_ACK_BACKOFF_BASE_MS : ms;
}

bool log_cloud_test_ack_inflight(uint8_t mode, uint8_t k)
{
    if (!s_cloud_inflight || s_cloud_batch == nullptr || s_cloud_tx_count == 0u)
    {
        return false;
    }

    uint32_t boot = s_cloud_tx_boot;
    uint32_t from = s_cloud_tx_from;
    uint32_t to = s_cloud_tx_to;

    if (mode == 1u)
    {
        uint8_t n = (k == 0u) ? 1u : k;

        if (n > s_cloud_tx_count)
        {
            n = s_cloud_tx_count;
        }

        to = s_cloud_batch[n - 1u].seq;      // 只覆盖前 n 条
    }
    else if (mode == 2u)
    {
        from = 1u;
        // FIX-BT9：与 classify 的比较对象保持一致 ⇒ "回退/重复 ACK" 语义不变
        to = s_cloud_gc_seq;                 // 回退到已确认（连续）水位
    }
    else if (mode == 3u)
    {
        boot = s_cloud_tx_boot + 1u;         // 错误 boot_seq
    }

    log_cloud_test_ack(boot, from, to);
    return true;
}

// =====================================================
// 观测
// =====================================================

void log_get_stats(LogStats &out)
{
    portENTER_CRITICAL(&s_mux);

    out = s_stats;
    out.ring_used = (uint8_t)(s_wr - s_rd);
    out.ring_ready = s_ready ? 1u : 0u;
    out.ring_in_psram = s_in_psram ? 1u : 0u;
    out.ring_bytes = s_ring_bytes;
    out.boot_seq = s_boot_seq;

    portEXIT_CRITICAL(&s_mux);
}

void log_stats_reset()
{
    portENTER_CRITICAL(&s_mux);
    memset(&s_stats, 0, sizeof(s_stats));
    s_stats.ring_used = (uint8_t)(s_wr - s_rd);
    portEXIT_CRITICAL(&s_mux);
}

// G-4：统计只读快照（单临界区整体拷贝，保证字段自洽）。
// 语义见 log_manager.h：false = 不可用 ≠ 全零。
bool log_stats_get(LogStats &out)
{
    memset(&out, 0, sizeof(out));

    if (!s_ready)
    {
        return false;
    }

    portENTER_CRITICAL(&s_mux);
    out = s_stats;          // POD 结构（纯 uint32/uint8），整体拷贝
    portEXIT_CRITICAL(&s_mux);

    return true;
}

bool log_flush_requested()
{
    // 与 log_task() / log_emit() 统一使用 s_mux（非阻塞自旋锁，不用 mutex）
    portENTER_CRITICAL(&s_mux);
    const bool requested = s_flush_requested;
    portEXIT_CRITICAL(&s_mux);

    return requested;
}

void log_clear_flush_request()
{
    portENTER_CRITICAL(&s_mux);
    s_flush_requested = false;
    portEXIT_CRITICAL(&s_mux);
}

// =====================================================
// P1.3 Flash 观测 / 测试钩子（仅上板自测，见头文件说明）
// =====================================================

void log_flash_get_info(LogFlashInfo &out)
{
    memset(&out, 0, sizeof(out));

    out.flash_ready    = s_flash_ready ? 1u : 0u;
    out.valid_segments = s_valid_segment_count;
    out.oldest_segment = s_oldest_segment;
    out.newest_segment = s_newest_segment;
    out.append_segment = s_append_segment;
    out.append_index   = s_append_index;
    out.total_records  = s_flash_total_records;

    out.first_seq_oldest = s_seg_present[s_oldest_segment]
                               ? s_seg_first_seq[s_oldest_segment] : 0u;
    out.first_seq_newest = s_seg_present[s_newest_segment]
                               ? s_seg_first_seq[s_newest_segment] : 0u;

    out.batch_bytes    = s_flash_batch_bytes;
    out.batch_in_psram = s_flash_batch_in_psram ? 1u : 0u;

    // seq / meta 状态（与 log_emit 的写入统一用 s_mux）
    portENTER_CRITICAL(&s_mux);
    out.seq_reliable  = s_seq_reliable ? 1u : 0u;
    out.boot_seq      = s_boot_seq;
    out.seq_reserved  = s_meta_reserved;
    out.corrupt_count = s_meta_corrupt;
    out.seq_base      = s_seq_base;
    out.seq_limit     = s_seq_limit;
    out.seq_last      = s_seq_last;

    /* `HD-3`·`A-1` 修复：恢复证据与隔离（追加字段，`A1-03`） */
    out.seq_failclosed  = s_seq_failclosed ? 1u : 0u;
    out.nvs_evidence_ok = s_evidence_ok ? 1u : 0u;
    out.nvs_quarantine  = (s_evidence_ok && ((s_ev_flags & LOG_EV_FLAG_QUARANTINE) != 0u)) ? 1u : 0u;
    out.quarantine_file = s_qfile_present ? 1u : 0u;
    out.evidence_source = s_ev_source;
    out.nvs_boot_hw     = s_ev_boot_hw;
    out.nvs_reserved_hw = s_ev_reserved_hw;
    out.evidence_gen    = s_ev_gen;
    portEXIT_CRITICAL(&s_mux);

    /* 损坏痕迹（只读一次小文件；失败 ⇒ 保持 0，不改变任何判定） */
    {
        uint32_t tc = 0, ts = 0, tb = 0;

        if (corrupt_trace_read(tc, ts, tb))
        {
            out.trace_corrupt  = tc;
            out.trace_last_seg = ts;
        }
    }
}

bool log_flash_peek_segment(uint32_t seg, LogSegmentHead &out)
{
    memset(&out, 0, sizeof(out));

    if (seg >= LOG_SEGMENT_COUNT)
    {
        return false;
    }

    char path[32];
    flash_seg_path(seg, path, sizeof(path));

    if (!LittleFS.exists(path))
    {
        return false;
    }

    File f = LittleFS.open(path, "r");

    if (!f)
    {
        return false;
    }

    uint8_t head[LOG_SEGMENT_HEADER_SIZE];

    if (f.read(head, sizeof(head)) != (size_t)sizeof(head))
    {
        f.close();
        return false;
    }

    out.magic     = flash_get_u32(&head[LOG_SEG_OFF_MAGIC]);
    out.index     = flash_get_u32(&head[LOG_SEG_OFF_INDEX]);
    out.first_seq = flash_get_u32(&head[LOG_SEG_OFF_FIRST_SEQ]);
    out.crc32     = flash_get_u32(&head[LOG_SEG_OFF_CRC32]);
    out.ok        = flash_check_head(head, seg) ? 1u : 0u;
    out.records   = out.ok ? flash_scan_records(f, false) : 0u;

    // FIX-3b 验证专用：**直接从 Flash** 读末槽记录的 seq（不读缓存），
    // 以便上板断言"扫描派生缓存 == Flash 真相"，并暴露 `first + records - 1`
    // 的偏小量（= 段内 INFO 空洞数）。
    if (out.records > 0)
    {
        LogRecord last_rec;
        const uint32_t loff = LOG_SEGMENT_HEADER_SIZE +
                              (uint32_t)(out.records - 1u) * LOG_RECORD_SIZE;

        if (f.seek(loff) &&
            (f.read((uint8_t *)&last_rec, LOG_RECORD_SIZE) == (size_t)LOG_RECORD_SIZE))
        {
            out.last_seq = last_rec.seq;
        }
    }

    f.close();
    return true;
}

bool log_flash_peek_record(uint32_t seg, uint8_t rec, LogRecord &out, uint8_t &crc_ok)
{
    crc_ok = 0;

    if (seg >= LOG_SEGMENT_COUNT || rec >= LOG_RECORDS_PER_SEGMENT)
    {
        return false;
    }

    char path[32];
    flash_seg_path(seg, path, sizeof(path));

    File f = LittleFS.open(path, "r");

    if (!f)
    {
        return false;
    }

    const uint32_t off =
        LOG_SEGMENT_HEADER_SIZE + (uint32_t)rec * LOG_RECORD_SIZE;

    bool ok = f.seek(off);

    if (ok)
    {
        ok = (f.read((uint8_t *)&out, LOG_RECORD_SIZE) == (size_t)LOG_RECORD_SIZE);
    }

    f.close();

    if (!ok)
    {
        return false;
    }

    crc_ok = flash_check_record(out) ? 1u : 0u;
    return true;
}

bool log_flash_wipe()
{
    bool ok = true;
    char path[32];

    for (uint32_t i = 0; i < LOG_SEGMENT_COUNT; i++)
    {
        flash_seg_path(i, path, sizeof(path));

        if (LittleFS.exists(path) && !LittleFS.remove(path))
        {
            ok = false;
        }
    }

    if (LittleFS.exists(LOG_META_PATH) && !LittleFS.remove(LOG_META_PATH))
    {
        ok = false;
    }

    flash_scan_all();   // 重建元信息（空 ring）

    return ok;
}

bool log_meta_wipe()
{
    /* `HD-3`·`A-1` 修复后：`meta.bin` 只是回滚镜像 ⇒ 仅删它**不再**等于
       "元数据丢失"。本函数表示 **legacy + 双槽全失**（段保留），
       以保持上板自测 `logt mwipe` 的原始语义（→ 走"段内重建"路径）。 */
    bool ok = true;

    if (LittleFS.exists(LOG_META_PATH) && !LittleFS.remove(LOG_META_PATH))
    {
        ok = false;
    }

    if (!log_meta_wipe_slots())
    {
        ok = false;
    }

    s_meta_slot     = -1;
    s_meta_slot_seq = 0;

    return ok;
}

bool log_meta_wipe_legacy()
{
    if (LittleFS.exists(LOG_META_PATH))
    {
        return LittleFS.remove(LOG_META_PATH);
    }

    return true;
}

bool log_meta_wipe_slots()
{
    bool ok = true;

    if (LittleFS.exists(LOG_META_SLOT_A) && !LittleFS.remove(LOG_META_SLOT_A))
    {
        ok = false;
    }

    if (LittleFS.exists(LOG_META_SLOT_B) && !LittleFS.remove(LOG_META_SLOT_B))
    {
        ok = false;
    }

    s_meta_slot     = -1;
    s_meta_slot_seq = 0;

    return ok;
}

void log_meta_test_fail_stage(uint8_t stage)
{
    s_meta_fail_stage = stage;
}

// ---- `HD-3`·`A-1` 修复：持久证据 / 隔离（`2026-10-18`）----

void log_set_first_boot(bool is_first_boot)
{
    s_first_boot     = is_first_boot;
    s_first_boot_set = true;
}

bool log_quarantine_active()
{
    return s_seq_failclosed;
}

/**
 * 受控解除隔离（**仅人工**：串口 `lqclear`）。
 *
 * ★ 语义边界：**保留** `boot_hw` / `reserved_hw`，只清隔离标志
 *   ⇒ 下次开机继续从"已知最大 `boot_seq`"往后走，**不是**回到 1。
 *   若当初隔离时 `boot_hw` 就是 0（什么都没恢复出来），则下次仍为 1
 *   —— 该情形无法在设备侧消除，须服务侧兜底（`HUMAN_REQUIRED`）。
 *
 * ⚠️ 不改变本次运行期闸门（`s_seq_failclosed`）：必须重启后重新判定，
 *    避免"运行中清标志 → 复用未固化的序号区间"。
 */
bool log_quarantine_clear()
{
    uint8_t buf[LOG_EV_SIZE];

    const uint32_t flags = (s_ev_flags & ~(uint32_t)LOG_EV_FLAG_QUARANTINE)
                           | LOG_EV_FLAG_PROVISIONED;

    log_meta_evidence::pack(buf, s_ev_boot_hw, s_ev_reserved_hw, flags,
                            s_ev_gen + 1u, s_meta_corrupt);

    bool ok_nvs = false;
    Preferences prefs;

    if (prefs.begin(kEvNamespace, false))
    {
        ok_nvs = (prefs.putBytes(kEvKey, buf, sizeof(buf)) == sizeof(buf));
        prefs.end();
    }

    bool ok_file = true;

    if (LittleFS.exists(LOG_QUARANTINE_PATH))
    {
        ok_file = LittleFS.remove(LOG_QUARANTINE_PATH);
    }

    if (ok_nvs && ok_file)
    {
        s_ev_flags      = flags;
        s_ev_gen        = s_ev_gen + 1u;
        s_evidence_ok   = true;
        s_qfile_present = false;
    }

    Serial.printf("[Log] quarantine clear: nvs=%d file=%d (takes effect next boot)\n",
                  ok_nvs ? 1 : 0, ok_file ? 1 : 0);

    return ok_nvs && ok_file;
}

/**
 * 删除持久证据（NVS 记录 + 隔离文件）。**仅测试 / 受控重置**用。
 *
 * 用于验证两条边界：
 *   · 清证后 + `first_boot=false` ⇒ 下次必判隔离（不得当作全新设备）
 *   · 清证后 + `first_boot=true`  ⇒ 下次判 `PROVISION`（真·首启）
 */
bool log_evidence_wipe()
{
    bool ok = true;

    Preferences prefs;

    if (prefs.begin(kEvNamespace, false))
    {
        prefs.remove(kEvKey);
        prefs.end();
    }
    else
    {
        ok = false;
    }

    if (LittleFS.exists(LOG_QUARANTINE_PATH) && !LittleFS.remove(LOG_QUARANTINE_PATH))
    {
        ok = false;
    }

    s_evidence_ok   = false;
    s_qfile_present = false;
    s_ev_flags      = 0;
    s_ev_boot_hw    = 0;
    s_ev_reserved_hw = 0;
    s_ev_gen        = 0;

    return ok;
}

bool log_meta_corrupt()
{
    // 翻转文件最后一个字节（落在 crc32 字段内）→ CRC 校验必然失败
    File f = LittleFS.open(LOG_META_PATH, "r+");

    if (!f)
    {
        return false;
    }

    const size_t sz = f.size();

    if (sz != (size_t)LOG_META_SIZE)
    {
        f.close();
        return false;
    }

    uint8_t b = 0;

    if (!f.seek((uint32_t)(sz - 1)) || (f.read(&b, 1) != 1))
    {
        f.close();
        return false;
    }

    b ^= 0xFFu;

    if (!f.seek((uint32_t)(sz - 1)) || (f.write(&b, 1) != 1))
    {
        f.close();
        return false;
    }

    f.close();
    return true;
}

void log_meta_test_fail_next(bool enable)
{
    s_meta_fail_next = enable;
}

// =====================================================
// Commit 5：Flash append 故障注入（F10）
// =====================================================

void log_flash_test_fail_next(uint8_t count)
{
    s_flash_fail_budget = count;
}

// ---- Commit 4：损坏注入钩子（仅上板自测）----

// 翻转文件某字节（读-改-写），用于构造损坏数据
static bool flash_flip_byte(const char *path, uint32_t off)
{
    File f = LittleFS.open(path, "r+");

    if (!f)
    {
        return false;
    }

    uint8_t b = 0;

    if (!f.seek(off) || (f.read(&b, 1) != 1))
    {
        f.close();
        return false;
    }

    b ^= 0xFFu;

    if (!f.seek(off) || (f.write(&b, 1) != 1))
    {
        f.close();
        return false;
    }

    f.close();
    return true;
}

bool log_seg_corrupt_head(uint32_t seg)
{
    if (seg >= LOG_SEGMENT_COUNT)
    {
        return false;
    }

    char path[32];
    flash_seg_path(seg, path, sizeof(path));

    if (!LittleFS.exists(path))
    {
        return false;
    }

    // 翻转 seg_index（offset 4）→ 头 CRC 必然不匹配（§18 场景）
    const bool ok = flash_flip_byte(path, LOG_SEG_OFF_INDEX);

    if (ok)
    {
        flash_scan_all();   // 模拟"重新初始化"后的扫描（§40-F7）
    }

    return ok;
}

bool log_seg_corrupt_record(uint32_t seg, uint8_t rec)
{
    if (seg >= LOG_SEGMENT_COUNT || rec >= LOG_RECORDS_PER_SEGMENT)
    {
        return false;
    }

    char path[32];
    flash_seg_path(seg, path, sizeof(path));

    if (!LittleFS.exists(path))
    {
        return false;
    }

    // 翻转该 record 的 byte 0（version，落在 CRC 覆盖区 [0..107]）
    const uint32_t off =
        LOG_SEGMENT_HEADER_SIZE + (uint32_t)rec * LOG_RECORD_SIZE;

    const bool ok = flash_flip_byte(path, off);

    if (ok)
    {
        flash_scan_all();   // 模拟"重新初始化"后的扫描（§40-F8）
    }

    return ok;
}

bool log_seg_truncate(uint32_t seg, uint32_t bytes)
{
    if (seg >= LOG_SEGMENT_COUNT)
    {
        return false;
    }

    if (bytes > LOG_SEGMENT_SIZE)
    {
        bytes = LOG_SEGMENT_SIZE;
    }

    char path[32];
    flash_seg_path(seg, path, sizeof(path));

    if (!LittleFS.exists(path))
    {
        return false;
    }

    // 读回原内容（≤ 3984 B，PSRAM 优先）
    uint8_t *buf = (uint8_t *)gf_alloc_psram_first(LOG_SEGMENT_SIZE,
                                                   MEM_SITE_LOG_SEG_BUF, nullptr);

    if (buf == nullptr)
    {
        return false;
    }

    size_t n = 0;
    File f = LittleFS.open(path, "r");

    if (f)
    {
        n = f.read(buf, LOG_SEGMENT_SIZE);
        f.close();
    }

    bool ok = LittleFS.remove(path);

    if (ok)
    {
        File w = LittleFS.open(path, "w");

        if (!w)
        {
            ok = false;
        }
        else
        {
            const uint32_t take = (bytes < (uint32_t)n) ? bytes : (uint32_t)n;
            ok = (w.write(buf, take) == (size_t)take);
            w.close();
        }
    }

    heap_caps_free(buf);

    if (ok)
    {
        flash_scan_all();   // 模拟"重新初始化"后的扫描（§40-F9）
    }

    return ok;
}
