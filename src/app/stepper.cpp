#include <Arduino.h>
#include <driver/gpio.h>
#include <driver/rmt.h>

#include "app/stepper.h"
#include "services/config_manager.h"
#include "services/system_state.h"
#include "services/event_manager.h"
#include "services/system_command.h"
#include "automation/workflow.h"

// =====================================================
// Stepper 模块实现（DRV8825 + RMT）
//
// 设计要点（对应 docs/architecture/Stepper-Vibration-Module-Implementation-Plan.md）：
//   · 硬件基线：1.8° 整步 / 1/8 微步 ⇒ 1600 有效 STEP 上升沿 / 圈
//   · 标准出粮 120° 理想脉冲 = 1600/3 = 533.333…（不能固定整数）
//   · 量化误差账本：累计余数分配 target(N)=floor(N*steps_per_rev*angle/360)
//     ⇒ 序列 533 / 533 / 534，误差上界 1 个脉冲（0.225°），与次数无关
//   · 人工调整(STEPPER_ROTATE_STEPS)【不】更新该账本
//   · STEP 脉冲由 RMT 循环模式产生（loop_count），不依赖 loop() 软件翻转
//   · 所有退出路径（成功 / 失败 / 超时 / 急停）统一汇聚到 stepper_finish()
// =====================================================

// =====================================================
// 代码层硬限制（配置值只能在此范围内，不能反向放大）
// =====================================================
// 30 秒最长运行时间（任务书 §五 / §2.6）：正常 120° 约 5.33 s，
// 但绝不能把 5.33 s 当超时值；30 s 是异常兜底。
#define STEPPER_ACTION_TIMEOUT_MS_HARD   30000UL
// RMT 单次 loop_count 上限（IDF 4.4 文档：1 ~ 1023）
#define STEPPER_RMT_MAX_LOOP             1023UL
// 单次动作脉冲数上限（防御异常输入，防止超长运行）
#define STEPPER_PULSES_CAP               6400UL
// 参数缺省值（配置缺失时的安全值，与 data/config/stepper.json 默认值一致）
#define STEPPER_DEF_STEP_PIN             13
#define STEPPER_DEF_DIR_PIN              14
#define STEPPER_DEF_EN_PIN               15
#define STEPPER_DEF_STEPS_PER_REV        1600
#define STEPPER_DEF_DISPENSE_ANGLE_DEG   120
#define STEPPER_DEF_PULSE_PERIOD_US      10000   // 10 ms ⇒ 约 100 Hz
#define STEPPER_DEF_ENABLE_DELAY_MS      50      // 应用层保守等待（非数据手册最小值）
#define STEPPER_DEF_MAX_PULSES           6400

// =====================================================
// 内部状态
// =====================================================
enum StepperPhase
{
    STEPPER_PHASE_IDLE = 0,
    STEPPER_PHASE_WAIT_ENABLE,   // EN 已使能，等待 enable_delay_ms
    STEPPER_PHASE_PULSING        // RMT 正在输出脉冲
};

// Action 私有上下文（仅保存"本次动作语义"，硬件状态在模块级）
struct StepperCtx
{
    bool     used;
    bool     dispense_accounting;   // true = 标准出粮动作（成功后才更新账本）
    uint32_t planned_pulses;
};

#define STEPPER_MAX_CTX   2
static StepperCtx s_ctx_pool[STEPPER_MAX_CTX];

// ---- 引脚与配置 ----
static int  s_step_pin      = -1;
static int  s_dir_pin       = -1;
static int  s_en_pin        = -1;
static int  s_en_active_lvl = 0;    // 0 = 低电平使能（DRV8825 EN 默认）
static bool s_dir_invert    = false;
static bool s_cfg_enable    = false;
static bool s_initialized   = false;
static bool s_pins_ready    = false;

static uint32_t s_steps_per_rev      = STEPPER_DEF_STEPS_PER_REV;
static uint32_t s_dispense_angle     = STEPPER_DEF_DISPENSE_ANGLE_DEG;
static uint32_t s_pulse_period_us    = STEPPER_DEF_PULSE_PERIOD_US;
static uint32_t s_enable_delay_ms    = STEPPER_DEF_ENABLE_DELAY_MS;
static uint32_t s_max_pulses         = STEPPER_DEF_MAX_PULSES;
static uint32_t s_action_timeout_ms  = STEPPER_ACTION_TIMEOUT_MS_HARD;
static bool     s_disable_after_done = true;

// ---- 硬件运行态 ----
static bool s_driver_enabled = false;   // EN 是否处于使能电平
static bool s_busy           = false;
static StepperPhase s_phase  = STEPPER_PHASE_IDLE;
static unsigned long s_op_start_ms     = 0;
static unsigned long s_phase_start_ms  = 0;
static uint32_t s_total_pulses  = 0;
static uint32_t s_emitted_pulses = 0;
static uint32_t s_burst_pulses  = 0;
static bool s_dispense_accounting = false;
static bool s_critical_held     = false;
static WorkflowActionInstance *s_active_action = nullptr;

// ---- 账本 ----
static uint32_t s_dispense_count = 0;   // 成功完成的出粮次数 N
static uint32_t s_issued_total   = 0;   // floor(N * steps_per_rev * angle / 360)

// ---- 结果 ----
static StepperResult s_last_result = STEPPER_RESULT_NONE;
static uint32_t s_last_pulses = 0;

// ---- RMT ----
static rmt_channel_t s_rmt_channel = RMT_CHANNEL_0;
static bool          s_rmt_ready    = false;
static rmt_item32_t  s_step_item;
static volatile bool s_tx_done      = false;

// =====================================================
// 前向声明
// =====================================================
static void stepper_action_reset(WorkflowActionInstance *action);
static void stepper_dispense_start(WorkflowActionInstance *action);
static void stepper_rotate_steps_start(WorkflowActionInstance *action);
static void stepper_estop_start(WorkflowActionInstance *action);
static void stepper_action_poll(WorkflowActionInstance *action);
static void stepper_finish(StepperResult result, const char *reason);
static bool stepper_start_burst(uint32_t n);

// =====================================================
// 参数表
// =====================================================
static WorkflowParam stepper_rotate_params[] = {
    {"pulses", PARAM_INT, "step", "计划输出的有效 STEP 上升沿数量（1 ~ max_pulses）"},
    {"direction", PARAM_INT, "enum", "旋转方向：0 = 出粮方向，1 = 反向；必须显式提供"}
};

static WorkflowParam stepper_no_params[] = {
    {"_unused", PARAM_INT, "", ""}
};

// =====================================================
// Action 描述符
// =====================================================
static WorkflowActionDescriptor stepper_dispense_desc = {
    .id          = "STEPPER_DISPENSE_ROTATE",
    .name        = "步进出粮旋转",
    .module      = "stepper",
    .description = "标准出粮：一次 120° 旋转，脉冲数由量化误差账本分配",
    .params      = stepper_no_params,
    .param_count = 0,
    .reset       = stepper_action_reset,
    .start       = stepper_dispense_start,
    .poll        = stepper_action_poll
};

static WorkflowActionDescriptor stepper_rotate_desc = {
    .id          = "STEPPER_ROTATE_STEPS",
    .name        = "步进指定脉冲旋转",
    .module      = "stepper",
    .description = "人工位置调整：按指定有效 STEP 上升沿数量与方向旋转",
    .params      = stepper_rotate_params,
    .param_count = 2,
    .reset       = stepper_action_reset,
    .start       = stepper_rotate_steps_start,
    .poll        = stepper_action_poll
};

static WorkflowActionDescriptor stepper_estop_desc = {
    .id          = "STEPPER_EMERGENCY_STOP",
    .name        = "步进急停",
    .module      = "stepper",
    .description = "立即中止正在进行的步进动作并停止 STEP 脉冲输出",
    .params      = stepper_no_params,
    .param_count = 0,
    .reset       = stepper_action_reset,
    .start       = stepper_estop_start,
    .poll        = stepper_action_poll
};

// =====================================================
// 工具：读 int 参数（严格：类型必须是 PARAM_INT，缺失返回 false）
// =====================================================
static bool stepper_param_int(WorkflowActionInstance *action,
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

// =====================================================
// 工具：日志（结构化日志码为 P1.1 冻结契约，本模块仅用串口输出，
//        详见报告 §未完成项）
// =====================================================
static void stepper_log(const char *fmt, ...)
{
    Serial.print("[Stepper] ");
    va_list ap;
    va_start(ap, fmt);
    char buf[160];
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    Serial.println(buf);
}

// =====================================================
// 工具：状态同步
// =====================================================
static void stepper_sync_state()
{
    state_set_bool(STATE_STEPPER_ENABLE, s_cfg_enable && s_pins_ready);
    state_set_bool(STATE_STEPPER_BUSY, s_busy);
    state_set_int(STATE_STEPPER_LAST_RESULT, (int)s_last_result);
    state_set_int(STATE_STEPPER_LAST_PULSES, (int)s_last_pulses);
}

// =====================================================
// 硬件：EN / DIR
// =====================================================
static void stepper_write_en(bool enable)
{
    if (s_en_pin < 0) {
        return;
    }
    int level = (s_en_active_lvl != 0) ? (enable ? 1 : 0) : (enable ? 0 : 1);
    gpio_set_level((gpio_num_t)s_en_pin, level);
    s_driver_enabled = enable;
}

static void stepper_write_dir(int direction)
{
    if (s_dir_pin < 0) {
        return;
    }
    int level = (direction != 0) ? 1 : 0;
    if (s_dir_invert) {
        level = level ? 0 : 1;
    }
    gpio_set_level((gpio_num_t)s_dir_pin, level);
}

// =====================================================
// 硬件：立即停止脉冲输出
//
// ★ 顺序不可颠倒：只关 EN 而让 RMT 继续输出是错误实现。
//   本函数只负责"停止 RMT"；是否禁用驱动器由 stepper_finish() 按
//   disable_after_action 配置与动作结果决定。
// =====================================================
static void stepper_stop_pulse_hw()
{
    if (s_rmt_ready) {
        rmt_tx_stop(s_rmt_channel);
    }
    s_tx_done = false;
}

// =====================================================
// RMT TX 结束回调（ISR 上下文：只置标志，不做任何其它事）
// =====================================================
static void stepper_rmt_tx_end_cb(rmt_channel_t channel, void *arg)
{
    (void)channel;
    (void)arg;
    s_tx_done = true;
}

// =====================================================
// 硬件：启动一段脉冲
// =====================================================
static bool stepper_start_burst(uint32_t n)
{
    if (!s_rmt_ready || n == 0) {
        return false;
    }
    s_tx_done = false;
    if (rmt_set_tx_loop_count(s_rmt_channel, n) != ESP_OK) {
        return false;
    }
    if (rmt_write_items(s_rmt_channel, &s_step_item, 1, false) != ESP_OK) {
        return false;
    }
    s_burst_pulses = n;
    return true;
}

// =====================================================
// 账本：下一次出粮动作应输出的脉冲数
//
//   target(N) = floor(N * steps_per_rev * angle / 360)
//   本次 = target(N+1) - target(N)
//
//   1600 / 120° ⇒ 533 / 533 / 534 循环，误差上界 1 脉冲 = 0.225°
// =====================================================
static uint64_t stepper_ideal_pulses(uint64_t n)
{
    return (n * (uint64_t)s_steps_per_rev * (uint64_t)s_dispense_angle) / 360ULL;
}

uint32_t stepper_plan_dispense_pulses()
{
    uint64_t next   = stepper_ideal_pulses((uint64_t)s_dispense_count + 1ULL);
    uint64_t issued = stepper_ideal_pulses((uint64_t)s_dispense_count);
    if (next <= issued) {
        return 0;
    }
    return (uint32_t)(next - issued);
}

// =====================================================
// 统一收尾：所有退出路径的唯一出口（幂等）
//
// 顺序（严格）：
//   ① 停 RMT（不再产生脉冲）
//   ② 禁用驱动器 EN
//   ③ 清运行态 / 阶段
//   ④ 写 Action 结果（只写一次）
//   ⑤ 同步 SystemState + 发布事件
//   ⑥ 【最后】释放 Critical Operation
// =====================================================
static void stepper_finish(StepperResult result, const char *reason)
{
    if (!s_busy) {
        return;   // 幂等：重复调用直接返回
    }

    // ①② 先停 RMT（不再产生脉冲），再按结果决定是否禁用驱动器
    stepper_stop_pulse_hw();
    //
    // 只有"正常成功 + 配置允许保持使能"时才让驱动器保持使能；
    // 失败 / 超时 / 急停一律禁用，避免异常路径下驱动器长期带电。
    if (!(result == STEPPER_RESULT_SUCCESS && !s_disable_after_done)) {
        stepper_write_en(false);
    }

    // ③ 清运行态
    s_busy          = false;
    s_phase         = STEPPER_PHASE_IDLE;
    s_burst_pulses  = 0;

    // 账本：只有【完整成功】的标准出粮动作才更新
    if (result == STEPPER_RESULT_SUCCESS && s_dispense_accounting) {
        s_dispense_count++;
        s_issued_total = (uint32_t)stepper_ideal_pulses((uint64_t)s_dispense_count);
    }
    s_dispense_accounting = false;

    s_last_result = result;
    s_last_pulses = s_total_pulses;

    // ④ 写 Action 结果
    if (s_active_action != nullptr) {
        if (s_active_action->result == ACTION_RUNNING) {
            s_active_action->result =
                (result == STEPPER_RESULT_SUCCESS) ? ACTION_SUCCESS : ACTION_FAILED;
        }
        s_active_action = nullptr;
    }

    stepper_log("finish result=%d pulses=%u emitted=%u reason=%s",
                (int)result, (unsigned)s_total_pulses,
                (unsigned)s_emitted_pulses, reason ? reason : "-");

    // ⑤ 状态 + 事件
    stepper_sync_state();
    switch (result) {
        case STEPPER_RESULT_SUCCESS:
            event_push(EVENT_STEPPER_DONE, String(s_total_pulses), "stepper",
                       EVENT_PRIORITY_NORMAL, EVENT_POLICY_STATE, 0);
            break;
        case STEPPER_RESULT_TIMEOUT:
        case STEPPER_RESULT_ESTOP:
        case STEPPER_RESULT_FAILED:
        case STEPPER_RESULT_REJECTED:
        default:
            event_push(EVENT_STEPPER_ERROR,
                       String((int)result) + ":" + (reason ? reason : ""),
                       "stepper", EVENT_PRIORITY_HIGH, EVENT_POLICY_STATE, 0);
            break;
    }

    // ⑥ 最后释放 Critical Operation
    if (s_critical_held) {
        system_command_critical_operation_release();
        s_critical_held = false;
    }
}

// =====================================================
// 启动一次旋转（公共内部路径）
//
// 返回 true = 已进入异步执行（ACTION_RUNNING）
// =====================================================
static bool stepper_begin(WorkflowActionInstance *action,
                          uint32_t pulses,
                          int direction,
                          bool dispense_accounting)
{
    if (!s_initialized || !s_cfg_enable || !s_pins_ready) {
        stepper_log("rejected: module disabled or not initialized");
        s_last_result = STEPPER_RESULT_REJECTED;
        s_last_pulses = 0;
        stepper_sync_state();
        return false;
    }
    if (s_busy) {
        stepper_log("rejected: another action is running");
        s_last_result = STEPPER_RESULT_REJECTED;
        stepper_sync_state();
        return false;
    }
    if (pulses == 0 || pulses > s_max_pulses || pulses > STEPPER_PULSES_CAP) {
        stepper_log("rejected: pulses out of range (%u)", (unsigned)pulses);
        s_last_result = STEPPER_RESULT_REJECTED;
        s_last_pulses = 0;
        stepper_sync_state();
        return false;
    }
    if (!s_rmt_ready) {
        stepper_log("rejected: RMT not ready");
        s_last_result = STEPPER_RESULT_FAILED;
        stepper_sync_state();
        return false;
    }

    // Critical Operation：动作期间不可被重启打断
    if (!system_command_critical_operation_acquire()) {
        stepper_log("rejected: critical operation acquire failed (restart pending)");
        s_last_result = STEPPER_RESULT_REJECTED;
        stepper_sync_state();
        return false;
    }
    s_critical_held = true;

    // 上下文
    StepperCtx *ctx = nullptr;
    for (uint8_t i = 0; i < STEPPER_MAX_CTX; i++) {
        if (!s_ctx_pool[i].used) {
            ctx = &s_ctx_pool[i];
            break;
        }
    }
    if (ctx == nullptr) {
        system_command_critical_operation_release();
        s_critical_held = false;
        s_last_result = STEPPER_RESULT_REJECTED;
        stepper_sync_state();
        return false;
    }
    ctx->used                = true;
    ctx->dispense_accounting = dispense_accounting;
    ctx->planned_pulses      = pulses;

    action->runtime       = ctx;
    action->result        = ACTION_RUNNING;

    s_active_action       = action;
    s_total_pulses        = pulses;
    s_emitted_pulses      = 0;
    s_dispense_accounting = dispense_accounting;
    s_last_pulses         = pulses;
    s_last_result         = STEPPER_RESULT_NONE;
    s_busy                = true;
    s_op_start_ms         = millis();
    s_phase               = STEPPER_PHASE_WAIT_ENABLE;
    s_phase_start_ms      = s_op_start_ms;

    // 方向先建立，再使能（避免方向未定时的任何脉冲）
    stepper_write_dir(direction);
    stepper_write_en(true);

    stepper_log("start pulses=%u dir=%d dispense=%u period=%uus",
                (unsigned)pulses, direction, (unsigned)dispense_accounting,
                (unsigned)s_pulse_period_us);

    event_push(EVENT_STEPPER_START, String(pulses), "stepper",
               EVENT_PRIORITY_NORMAL, EVENT_POLICY_STATE, 0);
    stepper_sync_state();
    return true;
}

// =====================================================
// Action: 标准出粮旋转（120°）
// =====================================================
static void stepper_dispense_start(WorkflowActionInstance *action)
{
    if (action == nullptr) {
        return;
    }
    uint32_t pulses = stepper_plan_dispense_pulses();
    if (pulses == 0) {
        stepper_log("rejected: dispense pulse plan invalid");
        s_last_result = STEPPER_RESULT_REJECTED;
        s_last_pulses = 0;
        stepper_sync_state();
        action->result = ACTION_FAILED;
        return;
    }
    if (!stepper_begin(action, pulses, 0, true)) {
        action->result = ACTION_FAILED;
    }
}

// =====================================================
// Action: 指定脉冲数旋转（人工调整，不进出粮账本）
// =====================================================
static void stepper_rotate_steps_start(WorkflowActionInstance *action)
{
    if (action == nullptr) {
        return;
    }
    int pulses = 0;
    int direction = -1;

    if (!stepper_param_int(action, "pulses", &pulses)) {
        stepper_log("rejected: param 'pulses' missing or not int");
        s_last_result = STEPPER_RESULT_REJECTED;
        s_last_pulses = 0;
        stepper_sync_state();
        action->result = ACTION_FAILED;
        return;
    }
    if (!stepper_param_int(action, "direction", &direction)) {
        stepper_log("rejected: param 'direction' missing or not int");
        s_last_result = STEPPER_RESULT_REJECTED;
        s_last_pulses = 0;
        stepper_sync_state();
        action->result = ACTION_FAILED;
        return;
    }
    if (direction != 0 && direction != 1) {
        stepper_log("rejected: direction invalid (%d), expect 0 or 1", direction);
        s_last_result = STEPPER_RESULT_REJECTED;
        s_last_pulses = 0;
        stepper_sync_state();
        action->result = ACTION_FAILED;
        return;
    }
    if (pulses <= 0) {
        stepper_log("rejected: pulses must be > 0 (%d)", pulses);
        s_last_result = STEPPER_RESULT_REJECTED;
        s_last_pulses = 0;
        stepper_sync_state();
        action->result = ACTION_FAILED;
        return;
    }

    // 人工调整【不】更新出粮账本
    if (!stepper_begin(action, (uint32_t)pulses, direction, false)) {
        action->result = ACTION_FAILED;
    }
}

// =====================================================
// Action: 急停
// =====================================================
static void stepper_estop_start(WorkflowActionInstance *action)
{
    bool stopped = stepper_emergency_stop();
    stepper_log("emergency_stop requested, stopped=%u", (unsigned)stopped);
    if (action != nullptr) {
        action->result = ACTION_SUCCESS;
    }
}

// =====================================================
// Action reset / poll
// =====================================================
static void stepper_ctx_release(StepperCtx *ctx)
{
    if (ctx == nullptr) {
        return;
    }
    ctx->used                = false;
    ctx->dispense_accounting = false;
    ctx->planned_pulses      = 0;
}

static void stepper_action_reset(WorkflowActionInstance *action)
{
    if (action == nullptr) {
        return;
    }
    StepperCtx *ctx = (StepperCtx *)action->runtime;
    if (ctx != nullptr) {
        stepper_ctx_release(ctx);
        action->runtime = nullptr;
    }
    action->result = ACTION_IDLE;
}

static void stepper_action_poll(WorkflowActionInstance *action)
{
    if (action == nullptr) {
        return;
    }
    StepperCtx *ctx = (StepperCtx *)action->runtime;

    // 未 start 而直接 poll（异常路径）
    if (ctx == nullptr) {
        if (action->result == ACTION_IDLE) {
            action->result = ACTION_FAILED;
        }
        return;
    }

    // 已结束（成功 / 失败 / 超时 / 急停）：回收上下文，保持结果不变
    if (action->result != ACTION_RUNNING) {
        stepper_ctx_release(ctx);
        action->runtime = nullptr;
        return;
    }

    // 仍 RUNNING：由 stepper_task() 驱动；若模块已不忙（异常脱钩）则判失败
    if (!s_busy && s_active_action != action) {
        stepper_log("poll: action detached while running -> failed");
        stepper_ctx_release(ctx);
        action->runtime = nullptr;
        action->result = ACTION_FAILED;
    }
}

// =====================================================
// 对外：急停
// =====================================================
bool stepper_emergency_stop()
{
    if (!s_busy) {
        stepper_log("emergency stop ignored: no action running");
        return false;
    }
    stepper_finish(STEPPER_RESULT_ESTOP, "estop");
    return true;
}

// =====================================================
// 初始化
// =====================================================
void stepper_init()
{
    for (uint8_t i = 0; i < STEPPER_MAX_CTX; i++) {
        s_ctx_pool[i].used                = false;
        s_ctx_pool[i].dispense_accounting = false;
        s_ctx_pool[i].planned_pulses      = 0;
    }

    // ---- 1) 读配置 ----
    s_cfg_enable     = config_get_stepper_enable();
    s_step_pin       = config_get_stepper_step_pin();
    s_dir_pin        = config_get_stepper_dir_pin();
    s_en_pin         = config_get_stepper_en_pin();
    s_en_active_lvl  = config_get_stepper_en_active_level();
    s_dir_invert     = config_get_stepper_dir_invert();

    s_steps_per_rev     = (uint32_t)config_get_stepper_steps_per_rev();
    s_dispense_angle    = (uint32_t)config_get_stepper_dispense_angle_deg();
    s_pulse_period_us   = (uint32_t)config_get_stepper_pulse_period_us();
    s_enable_delay_ms   = (uint32_t)config_get_stepper_enable_delay_ms();
    s_max_pulses        = (uint32_t)config_get_stepper_max_pulses();
    s_disable_after_done = config_get_stepper_disable_after_action();

    // 代码层硬限制钳位（配置不能放大）
    if (s_steps_per_rev < 1)      s_steps_per_rev = STEPPER_DEF_STEPS_PER_REV;
    if (s_dispense_angle < 1 ||
        s_dispense_angle > 360)   s_dispense_angle = STEPPER_DEF_DISPENSE_ANGLE_DEG;
    if (s_pulse_period_us < 20)   s_pulse_period_us = 20;
    if (s_pulse_period_us > 60000) s_pulse_period_us = 60000;
    if (s_enable_delay_ms > 2000) s_enable_delay_ms = STEPPER_DEF_ENABLE_DELAY_MS;
    if (s_max_pulses < 1 || s_max_pulses > STEPPER_PULSES_CAP) {
        s_max_pulses = STEPPER_PULSES_CAP;
    }
    uint32_t t = (uint32_t)config_get_stepper_action_timeout_ms();
    if (t < 1000 || t > STEPPER_ACTION_TIMEOUT_MS_HARD) {
        t = STEPPER_ACTION_TIMEOUT_MS_HARD;
    }
    s_action_timeout_ms = t;

    s_initialized = true;

    // ---- 2) GPIO：先写安全电平，后 gpio_config ----
    //
    // ★ V-2：valve 现实现是先 gpio_config 再写电平，存在不确定窗口。
    //   EN 为"低电平使能"时该窗口会让驱动器短暂使能，故本模块：
    //   gpio_set_level(禁用电平) → gpio_config(OUTPUT)
    s_pins_ready = false;
    if (s_cfg_enable && s_dir_pin >= 0 && s_en_pin >= 0 && s_step_pin >= 0) {
        gpio_set_level((gpio_num_t)s_en_pin, (s_en_active_lvl != 0) ? 0 : 1); // 禁用
        gpio_set_level((gpio_num_t)s_dir_pin, 0);
        s_driver_enabled = false;

        gpio_config_t io;
        io.intr_type    = GPIO_INTR_DISABLE;
        io.mode         = GPIO_MODE_OUTPUT;
        io.pin_bit_mask = (1ULL << s_dir_pin) | (1ULL << s_en_pin);
        io.pull_down_en = GPIO_PULLDOWN_DISABLE;
        io.pull_up_en   = GPIO_PULLUP_DISABLE;
        if (gpio_config(&io) == ESP_OK) {
            s_pins_ready = true;
        } else {
            stepper_log("gpio_config failed (dir=%d en=%d)", s_dir_pin, s_en_pin);
        }
    }

    // ---- 3) RMT（只在模块启用且引脚齐全时占用）----
    s_rmt_ready = false;
    if (s_pins_ready) {
        // 脉冲符号：半周期高 / 半周期低 ⇒ 每个 item = 1 个有效上升沿
        uint32_t half = s_pulse_period_us / 2;
        if (half < 1)       half = 1;
        if (half > 32767)   half = 32767;   // RMT duration 字段 15 bit

        s_step_item.level0    = 1;
        s_step_item.duration0 = half;
        s_step_item.level1    = 0;
        s_step_item.duration1 = half;

        rmt_config_t cfg = RMT_DEFAULT_CONFIG_TX((gpio_num_t)s_step_pin,
                                                 s_rmt_channel);
        cfg.clk_div       = 80;    // 80 MHz / 80 = 1 MHz ⇒ 1 µs / tick
        cfg.mem_block_num = 1;
        cfg.tx_config.carrier_en      = false;
        cfg.tx_config.idle_output_en  = true;
        cfg.tx_config.idle_level      = RMT_IDLE_LEVEL_LOW;

        if (rmt_config(&cfg) != ESP_OK) {
            stepper_log("rmt_config failed (step pin=%d)", s_step_pin);
        } else if (rmt_driver_install(s_rmt_channel, 0, 0) != ESP_OK) {
            stepper_log("rmt_driver_install failed");
        } else {
            rmt_set_idle_level(s_rmt_channel, true, RMT_IDLE_LEVEL_LOW);
            rmt_set_tx_loop_mode(s_rmt_channel, true);
            rmt_enable_tx_loop_autostop(s_rmt_channel, true);
            rmt_register_tx_end_callback(stepper_rmt_tx_end_cb, nullptr);
            s_rmt_ready = true;
        }
    }

    // ---- 4) 注册 Action（无论是否启用都注册，避免 stable_id 漂移）----
    if (!workflow_register_action(&stepper_dispense_desc)) {
        stepper_log("STEPPER_DISPENSE_ROTATE register failed");
    }
    if (!workflow_register_action(&stepper_rotate_desc)) {
        stepper_log("STEPPER_ROTATE_STEPS register failed");
    }
    if (!workflow_register_action(&stepper_estop_desc)) {
        stepper_log("STEPPER_EMERGENCY_STOP register failed");
    }

    // ---- 5) 初始状态同步 ----
    stepper_sync_state();

    stepper_log("Init: enable=%u pins_ready=%u rmt=%u (step=%d dir=%d en=%d, "
                "steps/rev=%u angle=%u period=%uus delay=%ums max=%u timeout=%ums)",
                (unsigned)s_cfg_enable, (unsigned)s_pins_ready, (unsigned)s_rmt_ready,
                s_step_pin, s_dir_pin, s_en_pin,
                (unsigned)s_steps_per_rev, (unsigned)s_dispense_angle,
                (unsigned)s_pulse_period_us, (unsigned)s_enable_delay_ms,
                (unsigned)s_max_pulses, (unsigned)s_action_timeout_ms);
}

// =====================================================
// 任务（loop 中调用，非阻塞）
// =====================================================
void stepper_task()
{
    if (!s_initialized || !s_busy) {
        return;
    }

    unsigned long now = millis();

    // ---- 30 秒兜底超时（异常路径，统一走 finish）----
    if ((unsigned long)(now - s_op_start_ms) >= s_action_timeout_ms) {
        stepper_log("action timeout (%lu ms) -> forced stop", s_action_timeout_ms);
        stepper_finish(STEPPER_RESULT_TIMEOUT, "timeout");
        return;
    }

    switch (s_phase) {
        case STEPPER_PHASE_WAIT_ENABLE:
            // 驱动器使能后的保守等待（50 ms，应用层选定值，非数据手册最小值）
            if ((unsigned long)(now - s_phase_start_ms) >= s_enable_delay_ms) {
                uint32_t remain = s_total_pulses - s_emitted_pulses;
                uint32_t burst  = remain > STEPPER_RMT_MAX_LOOP
                                    ? STEPPER_RMT_MAX_LOOP : remain;
                if (!stepper_start_burst(burst)) {
                    stepper_finish(STEPPER_RESULT_FAILED, "rmt_start_failed");
                    return;
                }
                s_phase          = STEPPER_PHASE_PULSING;
                s_phase_start_ms = now;
            }
            break;

        case STEPPER_PHASE_PULSING:
            if (s_tx_done) {
                s_tx_done = false;
                s_emitted_pulses += s_burst_pulses;
                if (s_emitted_pulses >= s_total_pulses) {
                    // 正常完成（是否禁用驱动器由 stepper_finish 统一决定）
                    stepper_finish(STEPPER_RESULT_SUCCESS, "pulse_done");
                } else {
                    // 剩余继续（>1023 脉冲时分段；段间有毫秒级间隙，不影响开环计数）
                    uint32_t remain = s_total_pulses - s_emitted_pulses;
                    uint32_t burst  = remain > STEPPER_RMT_MAX_LOOP
                                        ? STEPPER_RMT_MAX_LOOP : remain;
                    if (!stepper_start_burst(burst)) {
                        stepper_finish(STEPPER_RESULT_FAILED, "rmt_continue_failed");
                    }
                }
            }
            break;

        default:
            break;
    }
}

// =====================================================
// 查询接口
// =====================================================
bool stepper_is_enabled()         { return s_cfg_enable && s_pins_ready; }
bool stepper_is_driver_enabled()  { return s_driver_enabled; }
bool stepper_is_busy()            { return s_busy; }
int  stepper_get_last_result()    { return (int)s_last_result; }
uint32_t stepper_get_last_pulses(){ return s_last_pulses; }
uint32_t stepper_get_dispense_count() { return s_dispense_count; }
uint32_t stepper_get_issued_total()   { return s_issued_total; }
int  stepper_get_step_pin()       { return s_step_pin; }
int  stepper_get_dir_pin()        { return s_dir_pin; }
int  stepper_get_en_pin()         { return s_en_pin; }
