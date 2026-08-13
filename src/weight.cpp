#include <Arduino.h>
#include <math.h>
#include <HX711.h>

#include "weight.h"
#include "config_manager.h"
#include "system_state.h"
#include "event_manager.h"
#include "workflow.h"

// =====================================================
// Weight 模块 - HX711 称重
//
// 架构:
//   HX711 采样(is_ready 触发) -> 5 点窗口滤波 -> 重量计算
//     -> SystemState 同步 / 跳变检测 / Event 发布
//
// 非阻塞设计:
//   - 所有采样 / 校准均在 weight_task()（loop 周期调用）中推进
//   - 禁止 while 等待 HX711、禁止长 delay、禁止阻塞 workflow
// =====================================================

// =====================================================
// HX711 对象与模块参数（来自 config_manager）
// =====================================================
static HX711 scale;

static int weight_dt = -1;
static int weight_sck = -1;
static float weight_scale_factor = 741.0f;
static long zero_offset = 0;

static unsigned long last_weight_debug_print_ms = 0;

#define WEIGHT_DEBUG_PRINT_MS 5000UL

// =====================================================
// 滤波参数
//
// 固定 5 点窗口（去最大 / 去最小 / 剩余平均）。
// HX711 约 10Hz，每 5 次有效采样计算一次重量，约 0.5s 更新一次。
// =====================================================
#define WEIGHT_FILTER_WINDOW 5
static int filter_samples[WEIGHT_FILTER_WINDOW];
static uint8_t sample_index = 0;

// =====================================================
// 运行时数据
// =====================================================
static bool initialized = false;
static long raw_value = 0;
static float current_weight = 0;

// 跳变检测：连续两个窗口的滤波值
static float last_window_value = 0;
static bool have_last_window = false;

// 空闲状态（weight_active=false）时 STATE_WEIGHT_VALUE 的更新周期
#define WEIGHT_IDLE_STATE_UPDATE_MS 30000UL
static unsigned long last_state_update_ms = 0;

// =====================================================
// 异常检测参数
// =====================================================
#define WEIGHT_JUMP_DELTA_G        50.0f   // 相邻窗口重量跳变阈值（克）
#define WEIGHT_ERROR_WINDOW_MS     5000UL  // 5s 内跳变计数窗口
#define WEIGHT_MAX_JUMP_EVENTS     5       // 5s 内达到 5 次跳变 -> 异常
#define WEIGHT_RAW_ZERO_TIMEOUT_MS 5000UL  // raw 连续为零超时
#define WEIGHT_NOT_READY_TIMEOUT_MS 5000UL // HX711 连续无数据超时

static bool error_state = false;            // 当前 STATE_WEIGHT_ERROR 值
static unsigned long not_ready_start = 0;
static unsigned long last_not_ready_event_ms = 0;
static unsigned long raw_zero_start = 0;
static unsigned long jump_times[WEIGHT_MAX_JUMP_EVENTS];
static uint8_t jump_count = 0;

// =====================================================
// Workflow Trigger 运行状态（weight_decrease）
//
// 注意：WorkflowTriggerInstance 未提供模块私有 runtime 指针，
// 因此触发运行上下文使用模块级静态存储（同一时刻单触发使用）。
// =====================================================
static bool weight_active = false;
static float trigger_gram = 20.0f;
static float start_weight = 0.0f;

// =====================================================
// 零点校准状态机（非阻塞）
// =====================================================
#define WEIGHT_ZERO_SAMPLES 20
static bool calibrating = false;
static bool calibrate_ok = false;
static long cal_sum = 0;
static uint8_t cal_sample_count = 0;

// =====================================================
// 前向声明
// =====================================================
static void weight_trigger_reset(WorkflowTriggerInstance *trigger);
static void weight_trigger_start(WorkflowTriggerInstance *trigger);
static void weight_trigger_poll(WorkflowTriggerInstance *trigger);

static void weight_zero_action_reset(WorkflowActionInstance *action);
static void weight_zero_action_start(WorkflowActionInstance *action);
static void weight_zero_action_poll(WorkflowActionInstance *action);

// =====================================================
// Workflow Trigger Descriptor
// =====================================================
static WorkflowParam weight_trigger_params[] = {
    {"gram", PARAM_FLOAT, "克", "重量减少目标（克），合法范围 0 < gram <= 300"}
};

static WorkflowTriggerDescriptor weight_trigger_desc = {
    .id          = "weight_decrease",
    .name        = "重量减少",
    .module      = "weight",
    .description = "当重量减少*克时执行下一步",
    .params      = weight_trigger_params,
    .param_count = sizeof(weight_trigger_params) / sizeof(weight_trigger_params[0]),
    .reset       = weight_trigger_reset,
    .start       = weight_trigger_start,
    .poll        = weight_trigger_poll
};

// =====================================================
// Workflow Action Descriptor
// =====================================================
static WorkflowParam weight_zero_params[] = {
    // 无参数
};

static WorkflowActionDescriptor weight_zero_desc = {
    .id          = "WEIGHT_ZERO",
    .name        = "电子秤零点校准",
    .module      = "weight",
    .description = "保证电子秤空载时才可执行此命令",
    .params      = weight_zero_params,
    .param_count = 0,
    .reset       = weight_zero_action_reset,
    .start       = weight_zero_action_start,
    .poll        = weight_zero_action_poll
};

// =====================================================
// 滤波计算
// 5 点窗口：去最大值、去最小值，剩余 3 点取平均
// =====================================================
static int weight_filter()
{
    int max_value = filter_samples[0];
    int min_value = filter_samples[0];
    int sum = 0;

    for(uint8_t i = 0; i < WEIGHT_FILTER_WINDOW; i++)
    {
        if(filter_samples[i] > max_value)
            max_value = filter_samples[i];
        if(filter_samples[i] < min_value)
            min_value = filter_samples[i];

        sum += filter_samples[i];
    }

    sum -= max_value;
    sum -= min_value;

    return sum / (WEIGHT_FILTER_WINDOW - 2);
}

// =====================================================
// 异常状态维护
//
// STATE_WEIGHT_ERROR = true 的条件（任一）:
//   1. HX711 连续 5s 无数据（not ready）
//   2. raw 连续 5s 为零
//   3. 5s 内重量跳变（EVENT_WEIGHT_ERROR）达到 5 次
//
// 条件恢复后自动清除。
// =====================================================
static void weight_refresh_error_state()
{
    unsigned long now = millis();

    // 清理超过 5s 的跳变记录
    while(jump_count > 0 &&
          (now - jump_times[0]) > WEIGHT_ERROR_WINDOW_MS)
    {
        for(uint8_t i = 1; i < jump_count; i++)
        {
            jump_times[i - 1] = jump_times[i];
        }
        jump_count--;
    }

    bool no_data = (not_ready_start != 0) &&
                   (now - not_ready_start >= WEIGHT_NOT_READY_TIMEOUT_MS);
    bool raw_zero = (raw_zero_start != 0) &&
                    (now - raw_zero_start >= WEIGHT_RAW_ZERO_TIMEOUT_MS);
    bool jump_error = (jump_count >= WEIGHT_MAX_JUMP_EVENTS);
    bool err = no_data || raw_zero || jump_error;

    if(err != error_state)
    {
        error_state = err;
        state_set_bool(STATE_WEIGHT_ERROR, error_state);
        Serial.printf("[Weight] STATE_WEIGHT_ERROR -> %d\n",
                      error_state ? 1 : 0);
    }
}

// =====================================================
// 记录一次重量跳变事件
// =====================================================
static void weight_record_jump()
{
    if(jump_count < WEIGHT_MAX_JUMP_EVENTS)
    {
        jump_times[jump_count++] = millis();
    }
    else
    {
        // 记录池满：滚动丢弃最旧记录
        for(uint8_t i = 1; i < jump_count; i++)
        {
            jump_times[i - 1] = jump_times[i];
        }
        jump_times[jump_count - 1] = millis();
    }

    event_push(
        EVENT_WEIGHT_ERROR,
        "Weight jump detected",
        "weight",
        EVENT_PRIORITY_CRITICAL,
        EVENT_POLICY_STATE,
        0
    );
}

// =====================================================
// STATE_WEIGHT_VALUE 更新策略
//
// weight_active = true : 每次重量值更新都同步
// weight_active = false: 每 30 秒更新一次
// =====================================================
static void weight_update_state_value()
{
    unsigned long now = millis();
    if(weight_active ||
       (now - last_state_update_ms >= WEIGHT_IDLE_STATE_UPDATE_MS))
    {
        state_set_float(STATE_WEIGHT_VALUE, current_weight);
        last_state_update_ms = now;
    }
}

// =====================================================
// 任务（main.cpp loop 调用，非阻塞）
//
// 采样逻辑:
//   if(scale.is_ready()) { read() } else { return; }
// 只有成功读取新的 HX711 数据才进入滤波 / 重量计算 / 状态更新。
// 每次 is_ready() 为 true 只调用一次 read()，禁止重复读取同一数据。
// =====================================================
void weight_task()
{
    if(!initialized)
        return;
    // =================================================
    // 零点校准模式：优先消费 HX711 数据
    // =================================================
    if(calibrating)
    {
        if(scale.is_ready())
        {
            cal_sum += scale.read();
            cal_sample_count++;

            if(cal_sample_count >= WEIGHT_ZERO_SAMPLES)
            {
                zero_offset = cal_sum / WEIGHT_ZERO_SAMPLES;

                // config_manager 负责修改配置并保存 config.json
                bool ret = config_set_weight_zero_offset(zero_offset);

                if(ret){
                    calibrate_ok = config_save();
                }else{
                    calibrate_ok=false;
                }

                calibrating = false;

                // 校准后重置滤波窗口与重量
                sample_index = 0;
                have_last_window = false;
                current_weight = 0.0f;
                raw_zero_start = 0;
                state_set_float(STATE_WEIGHT_VALUE, current_weight);

                Serial.printf(
                    "[Weight] Zero calibrated, offset=%ld, save=%d\n",
                    zero_offset,
                    calibrate_ok ? 1 : 0
                );
            }
        }
        return;
    }

    // =================================================
    // HX711 采样：is_ready() 触发，每次只读一次
    // =================================================
    if(!scale.is_ready())
    {
        if(not_ready_start == 0)
            not_ready_start = millis();

        // 连续无数据超过 5s：发布错误事件（每 5s 最多一次）
        unsigned long now = millis();
        if((now - not_ready_start >= WEIGHT_NOT_READY_TIMEOUT_MS) &&
           (now - last_not_ready_event_ms >= WEIGHT_NOT_READY_TIMEOUT_MS))
        {
            last_not_ready_event_ms = now;
            event_push(
                EVENT_WEIGHT_ERROR,
                "HX711 not ready",
                "weight",
                EVENT_PRIORITY_HIGH,
                EVENT_POLICY_STATE,
                0
            );
        }

        // 同步异常状态（无数据超时 -> STATE_WEIGHT_ERROR）
        weight_refresh_error_state();
        return;
    }

    if(not_ready_start != 0)
    {
        not_ready_start = 0;
        last_not_ready_event_ms = 0;
    }

    // 成功读取新的 HX711 数据
    raw_value = scale.read();

    // raw 连续为零检测计时
    if(raw_value == 0)
    {
        if(raw_zero_start == 0)
            raw_zero_start = millis();
    }
    else
    {
        raw_zero_start = 0;
    }

    // 填入滤波窗口
    filter_samples[sample_index] = (int)raw_value;
    sample_index++;

    if(sample_index < WEIGHT_FILTER_WINDOW)
    {
        // 窗口未满：仅刷新异常状态
        weight_refresh_error_state();
        return;
    }
    sample_index = 0;

    // 滤波 + 重量计算（滤波后的重量，非 HX711 raw）
    int filtered = weight_filter();
    float gram = (float)(filtered - zero_offset) / weight_scale_factor;
    current_weight = gram;

/*    // =================================================
    // 调试代码Debug: 每5秒打印一次重量
    // =================================================
    static unsigned long last_weight_debug_print_ms = 0;

    unsigned long now = millis();

    if(now - last_weight_debug_print_ms >= 5000)
    {
        last_weight_debug_print_ms = now;

        Serial.printf(
            "[Weight] raw=%ld filtered=%d gram=%.1fg zero=%ld error=%d active=%d\n",
            raw_value,
            filtered,
            current_weight,
            zero_offset,
            error_state ? 1 : 0,
            weight_active ? 1 : 0
        );
    }
*/

    // 相邻窗口跳变检测：
    // delta = windowB - windowA，使用 fabs(float)，阈值 50g
    if(have_last_window)
    {
        float delta = gram - last_window_value;
        if(fabs(delta) >= WEIGHT_JUMP_DELTA_G)
        {
            weight_record_jump();
        }
    }
    
    last_window_value = gram;
    have_last_window = true;

    // 重量值同步到 SystemState
    weight_update_state_value();

    // 刷新异常状态
    weight_refresh_error_state();
}

// =====================================================
// Workflow Trigger: weight_decrease
// =====================================================
static void weight_trigger_reset(WorkflowTriggerInstance *trigger)
{
    if(trigger == nullptr)
        return;
    trigger->state = TRIGGER_IDLE;
    trigger->running = false;
    weight_active = false;
    trigger_gram = 0.0f;
    start_weight = current_weight;
}

// 读取 gram 参数（兼容 JSON int / float 参数类型）
static float weight_trigger_get_gram(WorkflowTriggerInstance *trigger)
{
    if(trigger == nullptr || trigger->param_count == 0)
        return 0.0f;

    for(uint8_t i = 0; i < trigger->param_count; i++)
    {
        if(trigger->params[i].name == "gram")
        {
            if(trigger->params[i].type == PARAM_FLOAT)
                return trigger->params[i].float_value;
            if(trigger->params[i].type == PARAM_INT)
                return (float)trigger->params[i].int_value;
            return 0.0f;   // 非数值类型视为非法
        }
    }
    return 0.0f;   // 参数不存在
}

static void weight_trigger_start(WorkflowTriggerInstance *trigger)
{
    if(trigger == nullptr)
        return;

    // 1. 称重异常：发布错误事件并返回 TRIGGER_FAILED
    if(state_get_bool(STATE_WEIGHT_ERROR))
    {
        event_push(
            EVENT_WEIGHT_ERROR,
            "Weight error, trigger aborted",
            "weight",
            EVENT_PRIORITY_CRITICAL,
            EVENT_POLICY_STATE,
            0
        );
        trigger->state = TRIGGER_FAILED;
        trigger->running = false;
        weight_active = false;
        return;
    }

    // 2. 参数保护：合法范围 0 < gram <= 300
    //    参数不存在 / params 为空 / gram<=0 / gram>300 均视为非法
    //    非法自动替换为 gram = 20 并发布 EVENT_WEIGHT_ERROR
    float gram = weight_trigger_get_gram(trigger);
    if(gram <= 0.0f || gram > 300.0f)
    {
        gram = 20.0f;
        event_push(
            EVENT_WEIGHT_ERROR,
            "Invalid gram param, use 20",
            "weight",
            EVENT_PRIORITY_NORMAL,
            EVENT_POLICY_STATE,
            0
        );
        Serial.println("[Weight] Invalid gram param, fallback to 20");
    }
    trigger_gram = gram;

    // 3. 记录起始重量，进入运行态
    start_weight = current_weight;
    weight_active = true;
    trigger->state = TRIGGER_RUNNING;
    trigger->running = true;

    Serial.printf("[Weight] Trigger start: gram=%.1f start_weight=%.1f\n",
                  trigger_gram, start_weight);
}

static void weight_trigger_poll(WorkflowTriggerInstance *trigger)
{
    if(trigger == nullptr)
        return;

    // 未 start 而直接 poll（异常路径）：直接失败
    if(trigger->state == TRIGGER_IDLE)
    {
        trigger->state = TRIGGER_FAILED;
        trigger->running = false;
        weight_active = false;
        return;
    }

    // weight_loss = start_weight - current_weight
    // 使用滤波后的重量数据，当 weight_loss >= gram 时触发成功
    float weight_loss = start_weight - current_weight;
    if(weight_loss >= trigger_gram)
    {
        trigger->state = TRIGGER_SUCCESS;
        trigger->running = false;
        weight_active = false;
        Serial.printf("[Weight] Trigger success: loss=%.1f >= %.1f\n",
                      weight_loss, trigger_gram);
    }
    else
    {
        trigger->state = TRIGGER_RUNNING;
    }
}

// =====================================================
// Workflow Action: WEIGHT_ZERO
// =====================================================
static void weight_zero_action_reset(WorkflowActionInstance *action)
{
    if(action == nullptr)
        return;
    action->result = ACTION_IDLE;
    action->running = false;
    action->runtime = nullptr;
}

static void weight_zero_action_start(WorkflowActionInstance *action)
{
    if(action == nullptr)
        return;
    // 启动非阻塞零点校准；实际采样在 weight_task() 中推进
    weight_zero_calibrate();
    action->result = ACTION_RUNNING;
}

static void weight_zero_action_poll(WorkflowActionInstance *action)
{
    if(action == nullptr)
        return;

    // 未 start 而直接 poll（异常路径）：直接失败
    if(action->result == ACTION_IDLE)
    {
        action->result = ACTION_FAILED;
        return;
    }

    // 校准进行中：继续等待；完成后按保存结果判定成功/失败
    if(calibrating)
    {
        action->result = ACTION_RUNNING;
    }
    else
    {
        action->result = calibrate_ok ? ACTION_SUCCESS : ACTION_FAILED;
    }
}

// =====================================================
// 初始化
// =====================================================
void weight_init()
{
    Serial.println("[Weight] Init...");

    // 1. 读取配置
    weight_dt = config_get_weight_dt();
    weight_sck = config_get_weight_sck();
    weight_scale_factor = config_get_weight_scale();
    zero_offset = config_get_weight_zero_offset();

    // 2. 初始化 HX711
    scale.begin(weight_dt, weight_sck);

    // 3. 注册 Workflow Trigger
    if(!workflow_register_trigger(&weight_trigger_desc))
    {
        Serial.println("[Weight] weight_decrease trigger register failed");
    }

    // 4. 注册 Workflow Action
    if(!workflow_register_action(&weight_zero_desc))
    {
        Serial.println("[Weight] WEIGHT_ZERO action register failed");
    }

    // 5. 初始状态同步
    state_set_float(STATE_WEIGHT_VALUE, current_weight);
    state_set_bool(STATE_WEIGHT_ERROR, false);

    initialized = true;
    Serial.printf("[Weight] Init OK (dt=%d sck=%d scale=%.1f zero_offset=%ld)\n",
                  weight_dt, weight_sck, weight_scale_factor, zero_offset);
}

// =====================================================
// 对外接口
// =====================================================
int weight_get_raw()
{
    return (int)raw_value;
}

float weight_get_gram()
{
    return current_weight;
}

// 启动零点校准（非阻塞）
void weight_zero_calibrate()
{
    if(!initialized || calibrating)
        return;

    calibrating = true;
    calibrate_ok = false;
    cal_sum = 0;
    cal_sample_count = 0;
    Serial.println("[Weight] Zero calibration started");
}

bool weight_is_active()
{
    return weight_active;
}
