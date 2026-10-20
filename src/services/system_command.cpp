// =====================================================
// SystemCommand 模块（V2 实现）
//
// 仅负责设备自身基础系统控制与资源查询，
// 并作为全系统唯一的 Restart 执行点。
//
// 详见 system_command.h 顶部说明与 docs/archive/legacy/AI_TASK.md。
// =====================================================
#include "services/system_command.h"

#include "log/log_manager.h"   // LOG_SYS_RESET_ABNORMAL（log_emit / log_arg_*）
#include "app/loop_stats.h"    // S-3：loop() 周期耗时统计（system.loop 只读查询）
#include "services/mem_watch.h" // G4-R4：内存观测（system.memwatch 只读查询）

#include <LittleFS.h>
#include <ESP.h>        // ESP.getHeapSize() / getFlashChipSize() / ESP.restart()
#include <stdarg.h>
#include <stdio.h>      // vsnprintf

// FreeRTOS：提供 portMUX_TYPE / portMUX_INITIALIZE / portENTER_CRITICAL。
//
// 为什么必须用真正的自旋锁:
//   cloud_manager 使用 esp-mqtt，命令在 mqtt_event_handler() 中同步执行
//   （cloud_manager.cpp:1095 调用 command_manager_execute），
//   该回调运行在 esp-mqtt 任务上下文，而不是 Arduino loop 任务。
//   因此 acquire()/release()/request_restart() 可能与 system_command_task()
//   在不同核心上并发执行，普通变量自增自减并不安全（需求文档 §11）。
//
// portmacro.h 内部已包含 soc/spinlock.h 与 esp_system.h，
// 因此 Reset Reason 相关 API 也随之可用。
#include <freertos/FreeRTOS.h>
#include <esp_system.h>

// =====================================================
// 内部状态
// =====================================================

// 保护下面全部共享状态的 SMP 自旋锁。
//
// 不使用 portMUX_INITIALIZER_UNLOCKED 静态初始化：
// 该宏展开为 C99 指示式初始化器，而本项目以 gnu++11 编译，
// 这里改为在 system_command_init() 中显式调用 portMUX_INITIALIZE()，
// 避免依赖 GNU 扩展行为。
static portMUX_TYPE s_lock;

static volatile RestartState s_restart_state = RESTART_IDLE;
static volatile uint32_t s_critical_count = 0;
static volatile unsigned long s_pending_since_ms = 0;

// 本次启动的 Reset Reason 缓存（esp_reset_reason() 结果随运行可能变化，
// 启动时取一次即可代表"本次为什么重启"）
static uint8_t s_reset_reason = 0;

// =====================================================
// 日志
//
// 本阶段不引入 Log 模块（需求文档 §15），仅做串口输出。
// Restart 是低频事件，输出量很小。
//
// 严禁在自旋锁临界区内调用本函数：串口输出可能阻塞。
// =====================================================
static void syscmd_log(const char *level, const char *fmt, ...)
{
    char buf[160];

    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    if (n < 0)
    {
        return;
    }

    Serial.printf("[SYSCMD][%s] %s\n", level, buf);
}

// =====================================================
// 状态名 / Reset Reason 名
// =====================================================
const char *system_command_restart_state_name(RestartState st)
{
    switch (st)
    {
        case RESTART_IDLE:
            return "idle";
        case RESTART_REQUESTED:
            return "requested";
        case RESTART_PENDING:
            return "pending";
        case RESTARTING:
            return "restarting";
        default:
            return "unknown";
    }
}

const char *system_command_reset_reason_name()
{
    switch ((esp_reset_reason_t)s_reset_reason)
    {
        case ESP_RST_UNKNOWN:
            return "unknown";
        case ESP_RST_POWERON:
            return "power_on";
        case ESP_RST_EXT:
            return "external_pin";
        case ESP_RST_SW:
            return "software";
        case ESP_RST_PANIC:
            return "panic";
        case ESP_RST_INT_WDT:
            return "interrupt_watchdog";
        case ESP_RST_TASK_WDT:
            return "task_watchdog";
        case ESP_RST_WDT:
            return "other_watchdog";
        case ESP_RST_DEEPSLEEP:
            return "deep_sleep";
        case ESP_RST_BROWNOUT:
            return "brownout";
        case ESP_RST_SDIO:
            return "sdio";
        default:
            return "unknown";
    }
}

// 复位原因是否属于异常。未列举的原因一律返回 false（宁可漏报不误报）。
static bool syscmd_reset_reason_is_abnormal(uint8_t reason)
{
    switch ((esp_reset_reason_t)reason)
    {
        case ESP_RST_PANIC:
        case ESP_RST_INT_WDT:
        case ESP_RST_TASK_WDT:
        case ESP_RST_WDT:
        case ESP_RST_BROWNOUT:
        case ESP_RST_SDIO:
            return true;

        default:
            return false;
    }
}

// =====================================================
// 内部辅助（资源查询部分）
// =====================================================

// 填充单个 RAM 区块的 5 个指标
static void fill_ram_block(
    JsonObject block,
    size_t total,
    size_t free_,
    size_t used,
    size_t largest_free,
    size_t min_free)
{
    block["total"] = total;
    block["free"] = free_;
    block["used"] = used;
    block["largest_free_block"] = largest_free;
    block["minimum_free"] = min_free;
}

// 递归收集 LittleFS 的目录与文件，写入 files 数组。
//
// 只读约束（需求文档 §8）:
//   - 仅 open 取元数据（name/isDirectory/size），不读写文件内容
//   - 不创建 / 删除 / 重命名任何文件
//   - 不修改 ConfigManager 的 Active / Backup / Factory 文件
//   - 直接返回扁平 path 列表，不构建嵌套 Tree 结构
//
// 部分核心版本 openNextFile() 返回的 name 不带前导 '/'，
// 这里统一规范化为 LittleFS 绝对路径。
static void files_collect(const String &dir_path, JsonArray files)
{
    File dir = LittleFS.open(dir_path);
    if (!dir || !dir.isDirectory())
    {
        if (dir)
        {
            dir.close();
        }
        return;
    }

    File f = dir.openNextFile();
    while (f)
    {
        String name = f.name();
        if (!name.startsWith("/"))
        {
            String full = dir_path;
            if (!dir_path.endsWith("/"))
            {
                full += "/";
            }
            full += name;
            name = full;
        }

        bool isDir = f.isDirectory();
        JsonObject entry = files.add<JsonObject>();
        if (!entry.isNull())
        {
            entry["path"] = name;
            entry["type"] = isDir ? "directory" : "file";
            entry["size"] = isDir ? 0 : f.size();
        }

        // 递归进入子目录（FS 深度很浅，栈开销可控）
        if (isDir)
        {
            files_collect(name, files);
        }

        f.close();
        f = dir.openNextFile();
    }
    dir.close();
}

// =====================================================
// 生命周期
// =====================================================
void system_command_init()
{
    // 显式把自旋锁置为 unlocked。
    //
    // 必须在任何 acquire()/release() 之前完成。
    // system_command_init() 由 setup() 阶段调用，早于一切业务。
    portMUX_INITIALIZE(&s_lock);

    portENTER_CRITICAL(&s_lock);
    s_restart_state = RESTART_IDLE;
    s_critical_count = 0;
    s_pending_since_ms = 0;
    portEXIT_CRITICAL(&s_lock);

    // Reset Reason 必须在早期读取：代表"本次为什么启动"。
    s_reset_reason = (uint8_t)esp_reset_reason();

    // 只记录异常复位（正常上电 / 软复位 / 深睡唤醒不记录）。
    if (syscmd_reset_reason_is_abnormal(s_reset_reason))
    {
        LogParamIn p[1];
        p[0] = log_arg_u32(LOG_P_RESET_REASON, (uint32_t)s_reset_reason);
        log_emit(LOG_SYS_RESET_ABNORMAL, LOG_LVL_CRITICAL, p, 1);
    }

    syscmd_log(
        "I",
        "init done, reset reason=%s, safe delay=%lu ms",
        system_command_reset_reason_name(),
        (unsigned long)SYSTEM_RESTART_SAFE_DELAY_MS
    );
}

// =====================================================
// Restart 状态机（由主循环驱动）
//
// 唯一允许调用 ESP.restart() 的地方。
//
// 状态推进:
//   RESTART_IDLE       -> 无事可做
//   RESTART_REQUESTED  -> 当 critical_count == 0 时进入 RESTART_PENDING
//   RESTART_PENDING    -> 倒计时满 SYSTEM_RESTART_SAFE_DELAY_MS 后真正重启
//   RESTARTING         -> 不可达（ESP.restart() 不会返回）
// =====================================================
void system_command_task()
{
    unsigned long now = millis();

    // 先在锁内取快照，之后所有判断与日志都在锁外进行。
    portENTER_CRITICAL(&s_lock);
    RestartState st = s_restart_state;
    uint32_t cnt = s_critical_count;
    unsigned long since = s_pending_since_ms;
    portEXIT_CRITICAL(&s_lock);

    switch (st)
    {
        case RESTART_REQUESTED:
        {
            // 等待所有 Critical Operation 完成。
            //
            // 注意（需求文档 §7）: 等待期间【允许】新的 Critical Operation 开始，
            // 这是刻意设计 —— 例如用户连续改多个 Config 参数时，
            // 已经发出的 Restart 不应该阻止后续参数修改，
            // 只需要等计数自然归零。
            if (cnt != 0)
            {
                return;
            }

            // 二次确认：cnt 是锁外快照，真正切换前必须重新判断，
            // 避免与并发的 acquire() 产生竞态。
            bool entered = false;

            portENTER_CRITICAL(&s_lock);
            if (s_restart_state == RESTART_REQUESTED && s_critical_count == 0)
            {
                s_restart_state = RESTART_PENDING;
                s_pending_since_ms = now;
                entered = true;
            }
            portEXIT_CRITICAL(&s_lock);

            if (entered)
            {
                syscmd_log(
                    "W",
                    "critical operations drained, restart pending %lu ms",
                    (unsigned long)SYSTEM_RESTART_SAFE_DELAY_MS
                );
            }
            return;
        }

        case RESTART_PENDING:
        {
            // 用无符号差值计算，天然兼容 millis() 溢出回绕
            unsigned long elapsed = now - since;

            if (elapsed < SYSTEM_RESTART_SAFE_DELAY_MS)
            {
                return;
            }

            // 进入 RESTARTING 之后再重启，保证状态可见。
            // ESP.restart() 必须在临界区外调用。
            portENTER_CRITICAL(&s_lock);
            s_restart_state = RESTARTING;
            portEXIT_CRITICAL(&s_lock);

            syscmd_log("W", "safe window elapsed, restarting now");
            Serial.flush();

            // 重启前记录（INFO，不落 Flash）
            log_emit0(LOG_SYS_RESTART_EXECUTED, LOG_LVL_INFO);

            ESP.restart();
            return;
        }

        case RESTART_IDLE:
        case RESTARTING:
        default:
            return;
    }
}

// =====================================================
// Restart Request API
// =====================================================
bool system_command_request_restart()
{
    bool accepted = false;
    bool first_request = false;

    portENTER_CRITICAL(&s_lock);

    if (s_restart_state == RESTART_IDLE)
    {
        s_restart_state = RESTART_REQUESTED;
        accepted = true;
        first_request = true;
    }
    else if (s_restart_state == RESTART_REQUESTED ||
             s_restart_state == RESTART_PENDING)
    {
        // 幂等（需求文档 §13）: 已经在等待 / 倒计时中。
        // 不取消、不重置倒计时、不重复触发。
        accepted = true;
    }
    else
    {
        // RESTARTING：系统正在关闭，无法再接受请求。
        accepted = false;
    }

    RestartState st = s_restart_state;
    uint32_t cnt = s_critical_count;
    portEXIT_CRITICAL(&s_lock);

    if (first_request)
    {
        LogParamIn p[1];
        p[0] = log_arg_u32(LOG_P_COUNT, cnt);
        log_emit(LOG_SYS_RESTART_REQUESTED, LOG_LVL_INFO, p, 1);

        syscmd_log(
            "W",
            "restart requested (critical=%lu), waiting for drain",
            (unsigned long)cnt
        );
    }
    else if (accepted)
    {
        syscmd_log(
            "I",
            "restart already requested (state=%s), ignored",
            system_command_restart_state_name(st)
        );
    }
    else
    {
        syscmd_log("W", "restart request rejected, system is restarting");
    }

    return accepted;
}

RestartState system_command_restart_state()
{
    portENTER_CRITICAL(&s_lock);
    RestartState st = s_restart_state;
    portEXIT_CRITICAL(&s_lock);

    return st;
}

bool system_command_restart_pending(unsigned long &remain_ms)
{
    remain_ms = 0;

    portENTER_CRITICAL(&s_lock);
    RestartState st = s_restart_state;
    unsigned long since = s_pending_since_ms;
    portEXIT_CRITICAL(&s_lock);

    if (st == RESTART_PENDING || st == RESTARTING)
    {
        unsigned long elapsed = millis() - since;
        remain_ms = (elapsed >= SYSTEM_RESTART_SAFE_DELAY_MS)
                  ? 0
                  : (SYSTEM_RESTART_SAFE_DELAY_MS - elapsed);
        return true;
    }

    if (st == RESTART_REQUESTED)
    {
        // 还在等 Critical Operation 归零，剩余时间未知，不能用 0 理解。
        remain_ms = 0;
        return true;
    }

    return false;
}

// =====================================================
// Critical Operation API
// =====================================================
bool system_command_critical_operation_acquire()
{
    bool ok = false;

    portENTER_CRITICAL(&s_lock);

    // 进入 RESTART_PENDING 后禁止新的 Critical Operation（需求文档 §14）。
    // 原因: 系统已经确认当时没有 Critical 操作并准备重启，
    // 若此刻允许新的不可中断操作开始，重启就永远无法收敛。
    if (s_restart_state != RESTART_PENDING &&
        s_restart_state != RESTARTING &&
        s_critical_count < 0xFFFFFFFFUL)
    {
        s_critical_count++;
        ok = true;
    }

    RestartState st = s_restart_state;
    portEXIT_CRITICAL(&s_lock);

    if (!ok)
    {
        syscmd_log(
            "W",
            "critical op acquire rejected (state=%s)",
            system_command_restart_state_name(st)
        );
    }

    return ok;
}

bool system_command_critical_operation_release()
{
    bool ok = false;

    portENTER_CRITICAL(&s_lock);
    if (s_critical_count > 0)
    {
        s_critical_count--;
        ok = true;
    }
    portEXIT_CRITICAL(&s_lock);

    if (!ok)
    {
        // 下溢保护（需求文档 §10）。
        // 计数已经是 0 还调用 release() 属于调用方配对错误，
        // 这里绝不递减，避免 uint32 回绕成巨大数值导致系统永久无法重启。
        syscmd_log(
            "E",
            "critical op release underflow: count already 0, "
            "acquire/release not paired"
        );

        log_emit0(LOG_SYS_CRITICAL_OP_UNDERFLOW, LOG_LVL_CRITICAL);
    }

    return ok;
}

uint32_t system_command_critical_operation_count()
{
    portENTER_CRITICAL(&s_lock);
    uint32_t cnt = s_critical_count;
    portEXIT_CRITICAL(&s_lock);

    return cnt;
}

uint8_t system_command_reset_reason()
{
    return s_reset_reason;
}

// =====================================================
// 指令实现
// =====================================================

bool syscmd_restart_status(JsonDocument &out)
{
    JsonObject data = out["data"].to<JsonObject>();
    if (data.isNull())
    {
        return false;
    }

    portENTER_CRITICAL(&s_lock);
    RestartState st = s_restart_state;
    uint32_t cnt = s_critical_count;
    unsigned long since = s_pending_since_ms;
    portEXIT_CRITICAL(&s_lock);

    unsigned long remain_ms = 0;
    if (st == RESTART_PENDING || st == RESTARTING)
    {
        unsigned long elapsed = millis() - since;
        remain_ms = (elapsed >= SYSTEM_RESTART_SAFE_DELAY_MS)
                  ? 0
                  : (SYSTEM_RESTART_SAFE_DELAY_MS - elapsed);
    }

    data["state"] = system_command_restart_state_name(st);
    data["critical_operations"] = cnt;
    data["restart_pending"] = (st != RESTART_IDLE);
    data["remain_ms"] = remain_ms;
    data["safe_delay_ms"] = (unsigned long)SYSTEM_RESTART_SAFE_DELAY_MS;

    return true;
}

bool syscmd_memory(JsonDocument &out)
{
    JsonObject data = out["data"].to<JsonObject>();
    if (data.isNull())
    {
        return false;
    }

    // ---- Internal RAM（片上 heap）----
    size_t internal_total = ESP.getHeapSize();
    size_t internal_free = ESP.getFreeHeap();
    size_t internal_used =
        (internal_total > internal_free)
            ? (internal_total - internal_free)
            : 0;
    JsonObject internal = data["internal"].to<JsonObject>();
    fill_ram_block(
        internal,
        internal_total,
        internal_free,
        internal_used,
        ESP.getMaxAllocHeap(),   // 当前最大可分配连续块
        ESP.getMinFreeHeap());   // 启动以来最小空闲（碎片参考）

    // ---- External PSRAM ----
    size_t psram_total = ESP.getPsramSize();
    JsonObject external = data["external"].to<JsonObject>();
    if (psram_total > 0)
    {
        size_t psram_free = ESP.getFreePsram();
        size_t psram_used =
            (psram_total > psram_free)
                ? (psram_total - psram_free)
                : 0;
        fill_ram_block(
            external,
            psram_total,
            psram_free,
            psram_used,
            ESP.getMaxAllocPsram(),
            ESP.getMinFreePsram());
    }
    else
    {
        // 无 PSRAM：整体填 0（属硬件差异，不报错）
        fill_ram_block(external, 0, 0, 0, 0, 0);
    }

    return true;
}

bool syscmd_flash(JsonDocument &out)
{
    JsonObject data = out["data"].to<JsonObject>();
    if (data.isNull())
    {
        return false;
    }

    // ---- External Flash = LittleFS 数据分区 ----
    size_t ext_total = LittleFS.totalBytes();
    size_t ext_used = LittleFS.usedBytes();
    if (ext_total == 0)
    {
        // 文件系统未挂载 / 不可用：明确失败，禁止返回 success
        return false;
    }
    size_t ext_free = (ext_total > ext_used) ? (ext_total - ext_used) : 0;
    JsonObject external = data["external"].to<JsonObject>();
    external["total"] = ext_total;
    external["used"] = ext_used;
    external["free"] = ext_free;

    // ---- Internal Flash = 固件区（Flash 总容量 - LittleFS 分区）----
    // 近似：固件区 = 整片 Flash - 数据分区。
    // 精确分区布局见 partitions.csv（nvs/otadata/app0/app1 位于 LittleFS 之前）。
    // used 以当前运行 app 大小（ESP.getSketchSize()）作为固件占用代理值。
    size_t chip = ESP.getFlashChipSize();
    size_t int_total = (chip > ext_total) ? (chip - ext_total) : 0;
    size_t int_used = ESP.getSketchSize();
    size_t int_free = (int_total > int_used) ? (int_total - int_used) : 0;
    JsonObject internal = data["internal"].to<JsonObject>();
    internal["total"] = int_total;
    internal["used"] = int_used;
    internal["free"] = int_free;

    // ---- 递归文件列表（只读）----
    JsonArray files = data["files"].to<JsonArray>();
    if (files.isNull())
    {
        return false;
    }
    files_collect("/", files);

    return true;
}

// =====================================================
// system.stats（G-4 · 只读统计快照）
//
// 目的：补齐 `GATE-WP5` ② 的观测缺口 ——
//       `drop_unacked` / `drop_overflow` / `hovf` / 段删除频率 等
//       此前**只有串口**可读，云端完全不可见。
//
// 语义约束：
//   · `log_stats_get()` 返回 false ⇒ `available:false`，**不写**计数器；
//     绝不把"统计不可用"渲染成"全部为 0"（那会被误读为"没有丢弃"）；
//   · 只读：不修改配置、不触发硬件、不重启、不分配堆内存；
//   · 栈开销 ≈ `LogStats(≈160 B) + LogCloudInfo(≈60 B) + LogFlashInfo(≈50 B)`。
// =====================================================
bool syscmd_stats(JsonDocument &out)
{
    JsonObject data = out["data"].to<JsonObject>();
    if (data.isNull())
    {
        return false;
    }

    LogStats st;
    const bool available = log_stats_get(st);

    data["available"] = available;
    data["uptime_ms"] = (uint32_t)millis();

    // ---- 队列 / ACK 运行时状态 ----
    LogCloudInfo ci;
    log_cloud_get_info(ci);

    JsonObject q = data["queue"].to<JsonObject>();
    q["used"] = ci.queue_used;
    q["capacity"] = (uint16_t)LOG_CLOUD_QUEUE_SLOTS;
    q["inflight"] = ci.inflight;
    q["retry"] = ci.retry;
    q["gave_up"] = ci.gave_up;
    q["acked_seq"] = ci.acked_seq;
    q["hole_overflow"] = ci.hole_overflow;   // `hovf`：空洞表溢出 ⇒ 本 boot 停止回收
    q["connected"] = ci.connected;

    // ---- Flash 段状态 ----
    LogFlashInfo fi;
    log_flash_get_info(fi);

    data["boot_seq"] = fi.boot_seq;

    JsonObject f = data["flash"].to<JsonObject>();
    f["ready"] = fi.flash_ready;
    f["valid_segments"] = fi.valid_segments;
    f["total_records"] = fi.total_records;
    f["corrupt_count"] = fi.corrupt_count;
    f["seq_reliable"] = fi.seq_reliable;

    // ---- 累计计数器（仅在可用时写入）----
    if (!available)
    {
        return true;    // 保留 available:false，字段缺省 ⇒ 调用方不得当 0 解读
    }

    JsonObject c = data["counters"].to<JsonObject>();

    c["emit_total"] = st.emit_total;
    c["ring_drop"] = st.ring_drop;

    // 丢弃三类（GATE-WP5 ② 的核心指标）
    c["drop_overflow"] = st.cloud_q_drop;          // 云队列溢出淘汰
    c["drop_unacked"] = st.cloud_flash_drop;       // give-up / 段淘汰中未 ACK 的条数
    c["seg_evicted_unacked"] = st.flash_seg_evict_unacked;   // 段环压力淘汰的未确认数

    c["cloud_batch_sent"] = st.cloud_batch_sent;
    c["cloud_records_sent"] = st.cloud_records_sent;
    c["cloud_retx"] = st.cloud_retx;               // ACK 超时后的重发次数
    c["cloud_ack_ok"] = st.cloud_ack_ok;
    c["cloud_ack_timeout"] = st.cloud_ack_timeout;
    c["cloud_ack_lost"] = st.cloud_ack_lost;       // 重试耗尽
    c["ack_ignored"] = st.cloud_ack_ignored;
    c["ack_partial"] = st.cloud_ack_partial;

    c["replay_records"] = st.cloud_replay_records;
    c["offline_skip"] = st.cloud_offline_skip;
    c["give_up"] = st.cloud_give_up;

    c["seg_deleted"] = st.flash_segment_deleted;   // 段删除频率 = Δ/时间
    c["self_degraded"] = st.self_degraded;

    return true;
}

// =====================================================
// system.loop（S-3：loop() 周期耗时统计 · 只读）
//
// 三条不变量：
//   1. 只读：不 reset、不改任何计数、不产生副作用（快照读取除外）
//   2. 不欺骗：统计不可信时**抑制** p50/p99 数值，只给 valid:false + 原因位
//   3. 不改既有契约：与 system.stats 完全独立（新 object，不影响既有解析方）
// =====================================================
static const char *loop_invalid_reason_name(uint16_t bit)
{
    switch (bit)
    {
    case loopstat::INVALID_NOT_AVAILABLE:    return "not_available";     // 尚无首样本
    case loopstat::INVALID_INSUFFICIENT:     return "insufficient_samples";
    case loopstat::INVALID_SATURATION:       return "bucket_saturated";
    case loopstat::INVALID_N_SATURATED:      return "sample_counter_saturated";
    case loopstat::INVALID_GEOMETRY:         return "snapshot_geometry_mismatch";
    case loopstat::INVALID_REGRESSION:       return "counter_regression";
    case loopstat::INVALID_ANOMALY_DOMINANT: return "anomaly_dominant";
    default:                                 return "unknown";
    }
}

bool syscmd_loop(JsonDocument &out)
{
    JsonObject data = out["data"].to<JsonObject>();
    if (data.isNull())
    {
        return false;
    }

    LoopStatsSnapshot snap;
    const bool available = loop_stats_get(snap);

    data["available"] = available;
    data["enabled"] = (GF_LOOP_STATS_ENABLED != 0);
    data["scope"] = "uptime";            // ★ 累积口径（自首样本起），不是窗口值
    data["uptime_ms"] = (uint32_t)millis();

    // 几何参数始终输出：即使不可用，调用方也能知道量程与桶宽
    data["bucket_width_us"] = (uint32_t)loopstat::BUCKET_WIDTH_US;
    data["bucket_count"] = (uint32_t)loopstat::BUCKET_COUNT;
    data["over_limit_us"] = (uint32_t)loopstat::RANGE_US;
    data["min_samples"] = (uint32_t)loopstat::MIN_SAMPLES;
    data["gate_p99_us"] = (uint32_t)loopstat::GATE_P99_US;

    if (!available)
    {
        // ★ 不写任何计数字段 ⇒ 调用方不得把缺省当成 0 解读
        data["quantiles_suppressed"] = true;
        JsonArray r = data["invalid_reasons"].to<JsonArray>();
        r.add("not_available");
        return true;
    }

    // ---- 原始计数（可用于 L2 有效性校验）----
    data["elapsed_ms"] = (uint64_t)(snap.elapsed_us / 1000ull);
    data["samples"] = snap.n;
    data["over"] = snap.over_cnt;
    data["anomaly"] = snap.anomaly_cnt;
    data["sum_us"] = snap.sum_us;
    data["min_us"] = snap.min_us;
    data["max_us"] = snap.max_us;
    data["sat_buckets"] = snap.sat_buckets;
    data["n_saturated"] = (snap.n_saturated != 0);
    data["snap_crit_max_us"] = snap.snap_crit_max_us;
    data["snap_count"] = snap.snap_count;

    // 精确均值（用 sum/n，**不得**用 P50 代替平均周期）
    if (snap.n != 0u)
    {
        data["mean_us"] = (uint32_t)(snap.sum_us / (uint64_t)snap.n);
    }

    // ---- 分位（以"空基线"做累积口径，等同自 boot 起）----
    LoopStatsSnapshot zero;
    memset(&zero, 0, sizeof(zero));
    zero.magic = loopstat::MAGIC_EMPTY;

    LoopStatsQuantiles q;
    const bool parsed = loop_stats_compute(zero, snap, q);
    if (!parsed)
    {
        data["valid"] = false;
        data["quantiles_suppressed"] = true;
        JsonArray r = data["invalid_reasons"].to<JsonArray>();
        r.add(loop_invalid_reason_name(q.invalid_bits));
        return true;
    }

    data["valid"] = (q.valid != 0);
    data["window_samples"] = q.n;
    data["window_over"] = q.over;
    data["window_anomaly"] = q.anomaly;

    // ---- 不可信：**拒绝输出** p50/p99 数值，只给原因 ----
    if (q.valid == 0u)
    {
        data["quantiles_suppressed"] = true;

        JsonArray r = data["invalid_reasons"].to<JsonArray>();
        for (uint16_t bit = 1; bit != 0; bit = (uint16_t)(bit << 1))
        {
            if ((q.invalid_bits & bit) != 0u)
            {
                r.add(loop_invalid_reason_name(bit));
            }
        }

        // 溢出区间仍必须给出**下界 + 精确最大值**（不得只报 25.6 ms）
        if (q.p99_in_overflow != 0u)
        {
            data["p99_overflow"] = true;
            data["p99_lower_bound_us"] = (uint32_t)loopstat::RANGE_US;
            data["p99_max_us"] = q.max_us_uptime;
        }
    }
    else
    {
        // ---- 可信：输出桶边界（20 ms 恰在桶边界 ⇒ 判定精确）----
        data["p50_bucket"] = q.p50_bucket;
        data["p50_low_us"] = q.p50_low_us;
        data["p50_high_us"] = q.p50_high_us;
        data["p50_us"] = q.p50_high_us;      // 保守取桶上界（不含）

        if (q.p99_in_overflow != 0u)
        {
            // P99 落在 over 区间：只给下界 + 精确 max，不给伪装成数值的 P99
            data["p99_overflow"] = true;
            data["p99_lower_bound_us"] = (uint32_t)loopstat::RANGE_US;
            data["p99_max_us"] = q.max_us_uptime;
            data["p99_lt_gate"] = false;
        }
        else
        {
            data["p99_bucket"] = q.p99_bucket;
            data["p99_low_us"] = q.p99_low_us;
            data["p99_high_us"] = q.p99_high_us;
            data["p99_us"] = q.p99_high_us;
            data["p99_lt_gate"] = (q.p99_lt_gate != 0u);
        }
    }

    // ---- 直方图（非零稀疏对；供"两次快照差分"复核 P99）----
    JsonArray hist = data["hist"].to<JsonArray>();
    uint32_t pairs = 0;
    for (uint32_t i = 0; i < loopstat::BUCKET_COUNT; ++i)
    {
        if (snap.hist[i] != 0u)
        {
            hist.add(i);
            hist.add(snap.hist[i]);
            pairs++;
        }
    }
    data["hist_pairs"] = pairs;

    return true;
}

// =====================================================
// system.memwatch（`G4-R4` · H-1…H-5 · 只读）
//
// 设计纪律（与 `G4-R4` 门禁修订草案一致）：
//   · 只**报告**可直接测量的量，**不**判 PASS/FAIL；
//   · 任何候选阈值只出现在 `thresholds` 段并标注 `candidate:true`，
//     **不参与**本命令的任何判定分支；
//   · `available:false` ⇒ 整段缺省（调用方不得当 0）；
//   · `drift.*.valid:false` ⇒ 不给 `slope_per_hour`（窗口未建立时该值无意义）。
//
// 栈开销：`MemWatchInfo` 约 170 B + 一次 memcpy 快照（32×20 B，静态缓冲，不在栈上）。
// =====================================================
bool syscmd_memwatch(JsonDocument &out)
{
    JsonObject data = out["data"].to<JsonObject>();
    if (data.isNull())
    {
        return false;
    }

    MemWatchInfo mw;
    const bool available = memwatch_get(mw);

    data["available"] = available;
    if (!available)
    {
        return true;    // 明确"不可用"，绝不渲染成 0
    }

    data["uptime_ms"] = mw.uptime_ms;

    // ---- H-1 / H-2 ----
    JsonObject dram = data["dram"].to<JsonObject>();
    dram["total"] = mw.dram_total;
    dram["free"] = mw.dram_free;
    dram["used"] = (mw.dram_total > mw.dram_free) ? (mw.dram_total - mw.dram_free) : 0u;
    dram["largest_free_block"] = mw.dram_largest;          // H-1
    dram["minimum_free_since_boot"] = mw.dram_min_free;    // H-2（**启动以来**低水位）
    dram["used_pct_x100"] = (mw.dram_total > 0u)
        ? (uint32_t)(((uint64_t)(mw.dram_total - mw.dram_free) * 10000u) / mw.dram_total)
        : 0u;   // 仅作参考（分母 = internal.total，与 PlatformIO 的 SRAM 总量口径不同）

    JsonObject psram = data["psram"].to<JsonObject>();
    psram["total"] = mw.psram_total;
    psram["free"] = mw.psram_free;
    psram["largest_free_block"] = mw.psram_largest;
    psram["minimum_free_since_boot"] = mw.psram_min_free;

    // ---- H-3：稳态漂移 ----
    JsonObject drift = data["drift"].to<JsonObject>();

    auto emit_drift = [](JsonObject dst, const MemDriftResult &d)
    {
        dst["valid"] = (d.valid != 0u);
        dst["reason"] = (d.reason == MEMWATCH_REASON_OK) ? "ok"
                      : (d.reason == MEMWATCH_REASON_TOO_FEW) ? "too_few_samples"
                      : "span_too_short";
        dst["samples"] = d.samples;
        dst["span_ms"] = d.span_ms;

        // 直接测量：无论 valid 与否都必须给出
        dst["free_min"] = d.free_min;
        dst["largest_min"] = d.largest_min;   // 碎片硬指标（窗口内最差连续块）
        dst["free_delta"] = d.free_delta;

        // 推断量：仅在窗口建立后给出（否则**不写**，避免被当 0）
        if (d.valid != 0u)
        {
            dst["slope_per_hour"] = d.slope_per_hour;
        }
    };

    emit_drift(drift["dram"].to<JsonObject>(), mw.dram_drift);
    emit_drift(drift["psram"].to<JsonObject>(), mw.psram_drift);

    JsonObject win = drift["window"].to<JsonObject>();
    win["samples"] = mw.window_samples;
    win["span_ms"] = mw.window_span_ms;
    win["target_ms"] = mw.window_target_ms;      // 候选配置（30 min）
    win["capacity"] = (uint8_t)MEMWATCH_RING_SAMPLES;
    win["sample_interval_ms"] = (uint32_t)MEMWATCH_SAMPLE_INTERVAL_MS;
    win["warmup_ms"] = (uint32_t)MEMWATCH_WARMUP_MS;
    win["closed"] = (mw.window_span_ms >= mw.window_target_ms);

    // ---- H-4：显式分配点失败计数 ----
    JsonObject alloc = data["alloc"].to<JsonObject>();
    alloc["calls"] = mw.alloc.calls;
    alloc["psram_ok"] = mw.alloc.psram_ok;
    alloc["psram_fail"] = mw.alloc.psram_fail;
    alloc["dram_fallback_ok"] = mw.alloc.dram_ok;
    alloc["dram_fallback_fail"] = mw.alloc.dram_fail;
    alloc["final_fail"] = mw.alloc.final_fail;
    alloc["last_fail_site"] = memwatch_site_name(mw.alloc.last_fail_site);
    alloc["last_fail_size"] = mw.alloc.last_fail_size;
    alloc["last_fail_ms"] = mw.alloc.last_fail_ms;

    JsonObject sites = alloc["sites"].to<JsonObject>();
    for (uint8_t i = 1; i < (uint8_t)MEM_SITE_COUNT; ++i)
    {
        // 只输出**非零**项（省 JSON 体积）；全零 ⇒ 该段为空 ⇒ "已登记点无失败"
        if (mw.alloc.site_final_fail[i] != 0u)
        {
            sites[memwatch_site_name(i)] = mw.alloc.site_final_fail[i];
        }
    }
    alloc["sites_reported"] = (uint8_t)(sites.size());

    // ★ 覆盖边界必须随数据一起输出，防止下游把"0"读成"系统无分配失败"
    JsonObject cov = alloc["coverage"].to<JsonObject>();
    cov["registered_sites"] = (uint8_t)(MEM_SITE_COUNT - 1);
    cov["scope"] = "explicit_alloc_sites_only";
    cov["excluded_note"] =
        "不含 Arduino String / 第三方库(含 mbedTLS) / IDF-Freertos 自身分配";
    cov["claim_limit"] =
        "final_fail=0 只表示**已登记显式分配点**未失败，不代表系统无任何分配失败";

    // ---- H-5：loopTask 栈高水位 ----
    JsonObject st = data["stack"].to<JsonObject>();
    st["available"] = (mw.stack.available != 0u);
    st["task"] = String(mw.stack.name);
    st["stack_bytes"] = mw.stack.stack_bytes;
    st["hwm_raw"] = mw.stack.hwm_raw;          // **始终**给出原始值（可复核）
    st["consistent"] = (mw.stack.consistent != 0u);
    st["unit_verified"] = (mw.stack.unit_verified != 0u);
    st["unit"] = (mw.stack.unit_verified == 0u) ? "unverified"
               : ((mw.stack.unit_is_bytes != 0u) ? "bytes" : "words");
    st["probe_depth_bytes"] = mw.stack.probe_depth;

    // ★ 派生值只在"口径已验证 + 长度自洽"时出现；
    //   否则**不写** `hwm_bytes` / `used_bytes`，避免把"未知"读成 0 或读成"用满了"。
    if (mw.stack.unit_verified != 0u && mw.stack.consistent != 0u)
    {
        st["hwm_bytes"] = mw.stack.hwm_bytes;
        st["used_bytes"] = mw.stack.used_bytes;
        if (mw.stack.stack_bytes > 0u)
        {
            st["used_pct_x100"] = (mw.stack.used_bytes * 10000u) / mw.stack.stack_bytes;
        }
    }
    else
    {
        st["derived_suppressed"] = true;
        st["derived_note"] =
            "hwm_bytes/used_bytes 未给出：需先用串口命令 `mw probe` 实测栈口径（bytes|words）";
    }

    // ---- 候选阈值（**未批准**；不参与任何判定）----
    JsonObject th = data["thresholds"].to<JsonObject>();
    th["candidate"] = true;
    th["approved"] = false;
    th["note"] = "全部为候选值，未经人工批准；本命令不据此判 PASS/FAIL";
    th["dram_min_free_candidate"] = 32768u;
    th["dram_largest_free_candidate"] = 16384u;
    th["dram_drift_bytes_per_hour_candidate"] = -1024;
    th["stack_used_candidate"] = (uint32_t)(MEMWATCH_LOOPTASK_STACK_BYTES * 3u / 4u);

    return true;
}
