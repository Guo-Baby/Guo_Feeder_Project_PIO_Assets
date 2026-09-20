// =====================================================
// SystemCommand 模块（V2 实现）
//
// 仅负责设备自身基础系统控制与资源查询，
// 并作为全系统唯一的 Restart 执行点。
//
// 详见 system_command.h 顶部说明与 AI_TASK.md。
// =====================================================
#include "system_command.h"

#include "log_manager.h"   // LOG_SYS_RESET_ABNORMAL（log_emit / log_arg_*）

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
