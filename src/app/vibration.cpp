#include <Arduino.h>
#include <driver/gpio.h>

#include "app/vibration.h"
#include "services/config_manager.h"
#include "services/system_state.h"
#include "services/event_manager.h"
#include "automation/workflow.h"

// =====================================================
// Vibration 模块实现（单 GPIO）
//
// 两种运行模式（同一个 Action，靠 duration_ms 区分）：
//   · duration_ms == 0 ：持续振动。开启成功后 Action【立即返回成功】，
//     输出保持开启，直到显式停止 / 30s 兜底超时。
//     ⇒ 不会阻塞 Workflow，后续可以继续调用步进出粮 Action（并发重叠）。
//   · duration_ms >  0 ：定时振动。开启后 Action 保持 RUNNING，
//     到时自动关闭输出后返回成功。
//
// 两个层次严格区分：
//   ① 定时振动的 duration_ms = 业务请求的正常结束时间
//   ② 30s safety_timeout    = 模块最长运行时间的安全兜底
//   duration_ms 不得超过 safety_timeout（超过 ⇒ 明确拒绝，不静默延长）。
// =====================================================

// =====================================================
// 代码层硬限制
// =====================================================
#define VIBRATION_TIMEOUT_MS_HARD      30000UL
#define VIBRATION_DEF_PIN              16
#define VIBRATION_DEF_ACTIVE_LEVEL     1
#define VIBRATION_DEF_MAX_DURATION_MS  30000UL
#define VIBRATION_DEF_MIN_INTERVAL_MS  200UL

// =====================================================
// Action 私有上下文
// =====================================================
struct VibrationCtx
{
    bool     used;
    uint32_t duration_ms;
};

#define VIBRATION_MAX_CTX  2
static VibrationCtx s_ctx_pool[VIBRATION_MAX_CTX];

// ---- 配置 ----
static int  s_pin          = -1;
static int  s_active_level = VIBRATION_DEF_ACTIVE_LEVEL;
static bool s_cfg_enable   = false;
static bool s_initialized  = false;
static bool s_pins_ready   = false;
static uint32_t s_max_duration_ms  = VIBRATION_DEF_MAX_DURATION_MS;
static uint32_t s_safety_timeout_ms = VIBRATION_TIMEOUT_MS_HARD;
static uint32_t s_min_interval_ms  = VIBRATION_DEF_MIN_INTERVAL_MS;

// ---- 运行态 ----
static bool s_output_on = false;
static unsigned long s_on_start_ms = 0;
static unsigned long s_last_off_ms = 0;
static WorkflowActionInstance *s_timed_action = nullptr;
static unsigned long s_deadline_ms = 0;
static VibrationResult s_last_result = VIB_RESULT_NONE;
static uint32_t s_last_duration_ms = 0;

// =====================================================
// 前向声明
// =====================================================
static void vibration_action_reset(WorkflowActionInstance *action);
static void vibration_start_action(WorkflowActionInstance *action);
static void vibration_stop_action(WorkflowActionInstance *action);
static void vibration_action_poll(WorkflowActionInstance *action);
static void vibration_output_off();

// =====================================================
// 参数表
// =====================================================
static WorkflowParam vibration_start_params[] = {
    {"duration_ms", PARAM_INT, "ms",
     "0 = 持续振动（Action 立即成功返回，需显式停止）；>0 = 定时振动（到时自动关闭）"}
};

static WorkflowParam vibration_no_params[] = {
    {"_unused", PARAM_INT, "", ""}
};

// =====================================================
// Action 描述符
// =====================================================
static WorkflowActionDescriptor vibration_start_desc = {
    .id          = "VIBRATION_START",
    .name        = "振动启动",
    .module      = "vibration",
    .description = "启动振动输出；duration_ms=0 持续，>0 定时后自动关闭",
    .params      = vibration_start_params,
    .param_count = 1,
    .reset       = vibration_action_reset,
    .start       = vibration_start_action,
    .poll        = vibration_action_poll
};

static WorkflowActionDescriptor vibration_stop_desc = {
    .id          = "VIBRATION_STOP",
    .name        = "振动停止",
    .module      = "vibration",
    .description = "关闭振动输出；若正在定时等待则该 Action 以失败结束",
    .params      = vibration_no_params,
    .param_count = 0,
    .reset       = vibration_action_reset,
    .start       = vibration_stop_action,
    .poll        = vibration_action_poll
};

// =====================================================
// 工具
// =====================================================
static void vib_log(const char *fmt, ...)
{
    Serial.print("[Vibration] ");
    va_list ap;
    va_start(ap, fmt);
    char buf[160];
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    Serial.println(buf);
}

static bool vibration_param_int(WorkflowActionInstance *action,
                                const char *name,
                                int *out)
{
    if (action == nullptr || name == nullptr || out == nullptr) {
        return false;
    }
    for (uint8_t i = 0; i < action->param_count && i < WORKFLOW_MAX_PARAM; i++) {
        if (action->params[i].type == PARAM_INT &&
            action->params[i].name == name)
        {
            *out = action->params[i].int_value;
            return true;
        }
    }
    return false;
}

static void vibration_sync_state()
{
    state_set_bool(STATE_VIBRATION_ENABLE, s_cfg_enable && s_pins_ready);
    state_set_bool(STATE_VIBRATION_ACTIVE, s_output_on);
    state_set_int(STATE_VIBRATION_LAST_RESULT, (int)s_last_result);
    state_set_int(STATE_VIBRATION_LAST_DURATION_MS, (int)s_last_duration_ms);
}

// =====================================================
// 硬件输出
// =====================================================
static void vibration_output_on()
{
    if (!s_pins_ready) {
        return;
    }
    int level = s_active_level ? 1 : 0;
    gpio_set_level((gpio_num_t)s_pin, level);
    s_output_on     = true;
    s_on_start_ms   = millis();
}

static void vibration_output_off()
{
    if (!s_pins_ready) {
        s_output_on = false;
        return;
    }
    int level = s_active_level ? 0 : 1;
    gpio_set_level((gpio_num_t)s_pin, level);
    if (s_output_on) {
        s_last_off_ms = millis();
    }
    s_output_on = false;
}

// =====================================================
// 定时动作收尾（幂等）
// =====================================================
static void vibration_finish(VibrationResult result, const char *reason)
{
    if (s_timed_action == nullptr) {
        // 无定时动作在等待：只同步状态（持续模式 / 超时关闭走这里）
        s_last_result = result;
        vibration_sync_state();
        return;
    }

    WorkflowActionInstance *action = s_timed_action;
    s_timed_action = nullptr;
    s_deadline_ms  = 0;

    vibration_output_off();

    if (action->result == ACTION_RUNNING) {
        action->result = (result == VIB_RESULT_SUCCESS) ? ACTION_SUCCESS : ACTION_FAILED;
    }
    s_last_result = result;

    vib_log("finish result=%d duration=%ums reason=%s",
            (int)result, (unsigned)s_last_duration_ms, reason ? reason : "-");

    vibration_sync_state();

    if (result == VIB_RESULT_SUCCESS) {
        event_push(EVENT_VIBRATION_STOP, "timed_done", "vibration",
                   EVENT_PRIORITY_NORMAL, EVENT_POLICY_STATE, 0);
    } else {
        event_push(EVENT_VIBRATION_ERROR,
                   String((int)result) + ":" + (reason ? reason : ""),
                   "vibration", EVENT_PRIORITY_HIGH, EVENT_POLICY_STATE, 0);
    }
}

// =====================================================
// 对外：启动（duration_ms = 0 持续 / >0 定时）
// =====================================================
bool vibration_start(uint32_t duration_ms)
{
    if (!s_initialized || !s_cfg_enable || !s_pins_ready) {
        vib_log("rejected: module disabled or not initialized");
        s_last_result = VIB_RESULT_REJECTED;
        vibration_sync_state();
        return false;
    }
    if (duration_ms > s_safety_timeout_ms) {
        vib_log("rejected: duration %ums > safety limit %ums",
                (unsigned)duration_ms, (unsigned)s_safety_timeout_ms);
        s_last_result = VIB_RESULT_REJECTED;
        vibration_sync_state();
        return false;
    }
    if (duration_ms > s_max_duration_ms) {
        vib_log("rejected: duration %ums > config max %ums",
                (unsigned)duration_ms, (unsigned)s_max_duration_ms);
        s_last_result = VIB_RESULT_REJECTED;
        vibration_sync_state();
        return false;
    }

    // 已经在定时等待中：拒绝（避免状态混乱）
    if (s_timed_action != nullptr) {
        vib_log("rejected: timed vibration already running");
        s_last_result = VIB_RESULT_REJECTED;
        vibration_sync_state();
        return false;
    }

    // 防风暴：关闭 → 再次开启的最小间隔（只对 off→on 生效）
    if (!s_output_on && s_last_off_ms != 0 &&
        (millis() - s_last_off_ms) < s_min_interval_ms)
    {
        vib_log("rejected: min interval %ums not elapsed", (unsigned)s_min_interval_ms);
        s_last_result = VIB_RESULT_REJECTED;
        vibration_sync_state();
        return false;
    }

    vibration_output_on();
    s_last_duration_ms = duration_ms;

    vib_log("start duration=%ums (pin=%d level=%d)", (unsigned)duration_ms,
            s_pin, s_active_level ? 1 : 0);
    s_last_result = VIB_RESULT_NONE;
    vibration_sync_state();
    event_push(EVENT_VIBRATION_START, String(duration_ms), "vibration",
               EVENT_PRIORITY_NORMAL, EVENT_POLICY_STATE, 0);
    return true;
}

// =====================================================
// 对外：停止
// =====================================================
bool vibration_stop()
{
    if (!s_initialized || !s_pins_ready) {
        vib_log("stop ignored: pins not ready");
        return false;
    }

    // 定时振动被主动停止 ⇒ 该 Action 判【失败】（不得误报正常完成）
    if (s_timed_action != nullptr) {
        vibration_finish(VIB_RESULT_STOPPED, "stopped_by_user");
        return true;
    }

    if (!s_output_on) {
        vib_log("stop ignored: output already off");
        return true;
    }

    vibration_output_off();
    s_last_result = VIB_RESULT_STOPPED;
    vib_log("stopped by user");
    vibration_sync_state();
    event_push(EVENT_VIBRATION_STOP, "user_stop", "vibration",
               EVENT_PRIORITY_NORMAL, EVENT_POLICY_STATE, 0);
    return true;
}

// =====================================================
// Action：启动
// =====================================================
static void vibration_start_action(WorkflowActionInstance *action)
{
    if (action == nullptr) {
        return;
    }

    int duration = 0;
    bool has_param = vibration_param_int(action, "duration_ms", &duration);
    if (!has_param) {
        duration = 0;   // 缺省 = 持续振动
    }
    if (duration < 0) {
        vib_log("rejected: duration_ms negative (%d)", duration);
        s_last_result = VIB_RESULT_REJECTED;
        vibration_sync_state();
        action->result = ACTION_FAILED;
        return;
    }

    VibrationCtx *ctx = nullptr;
    for (uint8_t i = 0; i < VIBRATION_MAX_CTX; i++) {
        if (!s_ctx_pool[i].used) {
            ctx = &s_ctx_pool[i];
            break;
        }
    }
    if (ctx == nullptr) {
        vib_log("rejected: context pool full");
        action->result = ACTION_FAILED;
        return;
    }

    if (!vibration_start((uint32_t)duration)) {
        action->result = ACTION_FAILED;
        return;
    }

    ctx->used        = true;
    ctx->duration_ms = (uint32_t)duration;
    action->runtime  = ctx;

    if (duration == 0) {
        // 持续模式：立即成功返回，Workflow 可继续执行下一步（步进出粮）
        action->result = ACTION_SUCCESS;
        s_last_result  = VIB_RESULT_NONE;
        vibration_sync_state();
    } else {
        // 定时模式：保持 RUNNING，由 vibration_task() 到时收尾
        action->result       = ACTION_RUNNING;
        s_timed_action       = action;
        s_deadline_ms        = millis() + (unsigned long)duration;
        vibration_sync_state();
    }
}

// =====================================================
// Action：停止
// =====================================================
static void vibration_stop_action(WorkflowActionInstance *action)
{
    bool ok = vibration_stop();
    if (action != nullptr) {
        action->result = ok ? ACTION_SUCCESS : ACTION_FAILED;
    }
}

// =====================================================
// Action reset / poll
// =====================================================
static void vibration_ctx_release(VibrationCtx *ctx)
{
    if (ctx == nullptr) {
        return;
    }
    ctx->used        = false;
    ctx->duration_ms = 0;
}

static void vibration_action_reset(WorkflowActionInstance *action)
{
    if (action == nullptr) {
        return;
    }
    VibrationCtx *ctx = (VibrationCtx *)action->runtime;
    if (ctx != nullptr) {
        vibration_ctx_release(ctx);
        action->runtime = nullptr;
    }
    action->result = ACTION_IDLE;
}

static void vibration_action_poll(WorkflowActionInstance *action)
{
    if (action == nullptr) {
        return;
    }
    VibrationCtx *ctx = (VibrationCtx *)action->runtime;

    if (ctx == nullptr) {
        if (action->result == ACTION_IDLE) {
            action->result = ACTION_FAILED;
        }
        return;
    }

    if (action->result != ACTION_RUNNING) {
        vibration_ctx_release(ctx);
        action->runtime = nullptr;
        return;
    }

    // 仍 RUNNING：由 vibration_task() 到期收尾；若已脱钩则判失败
    if (s_timed_action != action) {
        vib_log("poll: timed action detached -> failed");
        vibration_ctx_release(ctx);
        action->runtime = nullptr;
        action->result = ACTION_FAILED;
    }
}

// =====================================================
// 初始化
// =====================================================
void vibration_init()
{
    for (uint8_t i = 0; i < VIBRATION_MAX_CTX; i++) {
        s_ctx_pool[i].used        = false;
        s_ctx_pool[i].duration_ms = 0;
    }

    s_cfg_enable   = config_get_vibration_enable();
    s_pin          = config_get_vibration_relay_pin();
    s_active_level = config_get_vibration_active_level();

    uint32_t maxd = (uint32_t)config_get_vibration_max_duration_ms();
    if (maxd == 0 || maxd > VIBRATION_TIMEOUT_MS_HARD) {
        maxd = VIBRATION_TIMEOUT_MS_HARD;
    }
    s_max_duration_ms = maxd;

    uint32_t to = (uint32_t)config_get_vibration_safety_timeout_ms();
    if (to < 1000 || to > VIBRATION_TIMEOUT_MS_HARD) {
        to = VIBRATION_TIMEOUT_MS_HARD;
    }
    s_safety_timeout_ms = to;

    s_min_interval_ms = (uint32_t)config_get_vibration_min_interval_ms();

    s_initialized = true;

    // GPIO：先写安全电平（关闭），后 gpio_config
    s_pins_ready = false;
    s_output_on  = false;
    if (s_cfg_enable && s_pin >= 0) {
        gpio_set_level((gpio_num_t)s_pin, s_active_level ? 0 : 1);
        gpio_config_t io;
        io.intr_type    = GPIO_INTR_DISABLE;
        io.mode         = GPIO_MODE_OUTPUT;
        io.pin_bit_mask = (1ULL << s_pin);
        io.pull_down_en = GPIO_PULLDOWN_DISABLE;
        io.pull_up_en   = GPIO_PULLUP_DISABLE;
        if (gpio_config(&io) == ESP_OK) {
            s_pins_ready = true;
        } else {
            vib_log("gpio_config failed (pin=%d)", s_pin);
        }
    }

    // 注册 Action（无论是否启用都注册，避免 stable_id 漂移）
    if (!workflow_register_action(&vibration_start_desc)) {
        vib_log("VIBRATION_START register failed");
    }
    if (!workflow_register_action(&vibration_stop_desc)) {
        vib_log("VIBRATION_STOP register failed");
    }

    vibration_sync_state();

    vib_log("Init: enable=%u pins_ready=%u (pin=%d active_level=%d, "
            "max_duration=%ums safety_timeout=%ums)",
            (unsigned)s_cfg_enable, (unsigned)s_pins_ready, s_pin, s_active_level,
            (unsigned)s_max_duration_ms, (unsigned)s_safety_timeout_ms);
}

// =====================================================
// 任务（loop 中调用，非阻塞）
// =====================================================
void vibration_task()
{
    if (!s_initialized || !s_pins_ready) {
        return;
    }

    unsigned long now = millis();

    // ---- ① 定时振动到期：正常关闭并判成功 ----
    if (s_timed_action != nullptr && s_deadline_ms != 0) {
        if ((long)(now - s_deadline_ms) >= 0) {
            vibration_finish(VIB_RESULT_SUCCESS, "duration_elapsed");
            return;
        }
    }

    // ---- ② 30 秒最长运行时间兜底 ----
    if (s_output_on) {
        unsigned long elapsed = now - s_on_start_ms;
        if (elapsed >= s_safety_timeout_ms) {
            vib_log("safety timeout (%lu ms) -> forced off", s_safety_timeout_ms);
            if (s_timed_action != nullptr) {
                vibration_finish(VIB_RESULT_TIMEOUT, "safety_timeout");
            } else {
                vibration_output_off();
                s_last_result = VIB_RESULT_TIMEOUT;
                vib_log("continuous vibration stopped by 30s protection");
                vibration_sync_state();
                event_push(EVENT_VIBRATION_ERROR, String(VIB_RESULT_TIMEOUT) +
                           ":safety_timeout", "vibration",
                           EVENT_PRIORITY_HIGH, EVENT_POLICY_STATE, 0);
            }
        }
    }
}

// =====================================================
// 查询接口
// =====================================================
bool vibration_is_enabled() { return s_cfg_enable && s_pins_ready; }
bool vibration_is_active()  { return s_output_on; }
int  vibration_get_last_result() { return (int)s_last_result; }
uint32_t vibration_get_last_duration_ms() { return s_last_duration_ms; }
uint32_t vibration_get_active_ms()
{
    return s_output_on ? (uint32_t)(millis() - s_on_start_ms) : 0;
}
int  vibration_get_pin() { return s_pin; }
