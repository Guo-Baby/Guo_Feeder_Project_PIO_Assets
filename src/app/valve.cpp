#include <Arduino.h>
#include <driver/gpio.h>

#include "app/valve.h"
#include "services/config_manager.h"
#include "services/system_state.h"
#include "services/event_manager.h"
#include "automation/workflow.h"
#include "log/log_manager.h"   // P2-H：观测埋点（EventId / ParamId + log_emit）

// =====================================================
// Valve 模块 - GPIO 控制（应用模块 + 底层驱动模块）
//
// 职责:
//   - 阀门硬件控制（open/close/toggle）
//   - Workflow Action 注册（VALVE_OPEN / VALVE_CLOSE）
//   - SystemState 维护（STATE_VALVE_STATUS）
//   - EventManager 事件发布（EVENT_VALVE_OPEN / EVENT_VALVE_CLOSE / EVENT_VALVE_ERROR）
//
// 边界:
//   - 不实现 CommandManager 逻辑，不生成命令结果
//   - 不主动处理命令回调（由 Workflow 框架 / CommandManager 契约管理）
//   - 同一份 Action 描述符同时支持 Workflow Step 调用与 Command 临时 Action 调用
//     （模块只实现 descriptor 的 reset/start/poll，不感知调用来源）
// =====================================================

// =====================================================
// 硬件状态（模块私有）
// =====================================================
static int valve_pin = -1;
static int active_level = 1;         // 高电平有效
static bool current_state = false;   // false=关闭, true=打开
static bool initialized = false;

// 安全超时配置
static unsigned long safety_timeout_ms = 300000;  // 默认 300 秒
static unsigned long open_start_time = 0;

// 防风暴保护（只在真正切换状态时刷新）
static unsigned long last_operation_time = 0;
static const unsigned long MIN_OPERATION_INTERVAL_MS = 50;

// =====================================================
// P2-H：日志去重门控 / 一次性报告锁（纯观测状态，不参与控制流）
// =====================================================
//
// FORCE_CLOSE 去重门控（P2-H 决策 D1=A）：
//   cause 变化 或 距上次记录 >= 5s 才写日志。
//   门控**只限制日志**，绝不影响 valve_force_close() 的 GPIO 强制同步语义。
static uint32_t last_force_close_log_ms = 0;
static uint8_t  last_force_close_cause  = 0;
static const uint32_t VALVE_FORCE_CLOSE_LOG_COOLDOWN_MS = 5000UL;

// 强制关阀 CAUSE 冻结定义（本轮实际来源只有 WEIGHT_ERROR；2/3 为预留，勿改值）
enum : uint8_t {
    VALVE_CAUSE_WEIGHT_ERROR   = 1,   // dispense_guard 收到 EVENT_WEIGHT_ERROR
    VALVE_CAUSE_MANUAL_COMMAND = 2,   // 预留：人工命令
    VALVE_CAUSE_SAFETY_TIMEOUT = 3    // 预留：安全超时（当前走 valve_set_gpio，不经此路径）
};

// 安全超时一次性报告锁（P2-H 决策 D4=A）：
//   open_start_time 只在 valve_set_gpio(false) **成功后**清零；若本次被 50ms
//   防风暴跳过，则下一轮 loop 会再次进入超时分支 => 无锁时同一超时会产生几十条
//   重复记录。锁在"真正开启/关闭成功"时解除。
static bool safety_timeout_reported = false;

// =====================================================
// Workflow Action 参数（本模块无参数）
// =====================================================
static WorkflowParam valve_params[] = {
    // 如需参数: {"hold_ms", PARAM_INT, "ms", "保持时间"}
};

// =====================================================
// Workflow Action Descriptor（文件级 static，生命周期贯穿程序）
// =====================================================
static void valve_action_reset(WorkflowActionInstance *action);
static void valve_open_start(WorkflowActionInstance *action);
static void valve_close_start(WorkflowActionInstance *action);
static void valve_action_poll(WorkflowActionInstance *action);

static WorkflowActionDescriptor valve_open_desc = {
    .id          = "VALVE_OPEN",
    .name        = "电磁阀开",
    .module      = "valve",
    .description = "打开电磁阀",
    .params      = valve_params,
    .param_count = 0,
    .reset       = valve_action_reset,
    .start       = valve_open_start,
    .poll        = valve_action_poll
};

static WorkflowActionDescriptor valve_close_desc = {
    .id          = "VALVE_CLOSE",
    .name        = "电磁阀关",
    .module      = "valve",
    .description = "关闭电磁阀",
    .params      = valve_params,
    .param_count = 0,
    .reset       = valve_action_reset,
    .start       = valve_close_start,
    .poll        = valve_action_poll
};

// =====================================================
// 内部函数：更新 System State
// =====================================================
static void valve_update_state()
{
    state_set_bool(STATE_VALVE_STATUS, current_state);
}

// =====================================================
// 内部函数：设置 GPIO 电平（真正执行硬件操作）
// =====================================================
static void valve_set_gpio(bool open)
{
    if (valve_pin < 0) return;

    // 相同状态直接返回，不重复执行
    if (open == current_state) {
        return;
    }

    // 防风暴：只有真正发生切换才检查冷却
    unsigned long now = millis();
    if (now - last_operation_time < MIN_OPERATION_INTERVAL_MS) {
        Serial.println("[Valve] Operation too frequent, skipped");

        // ---- P2-H 埋点：防风暴跳过（RATE_LIMITED，WARN）----
        // 该分支只在"真的要切换状态"时才可能进入（判等 return 在防风暴检查之前），
        // 实测 11 个既有会话 0 次 => 无需去重，直接记录。
        {
            LogParamIn p[1];
            p[0] = log_arg_u32(LOG_P_LIMIT_MS, (uint32_t)MIN_OPERATION_INTERVAL_MS);
            log_emit(LOG_VALVE_RATE_LIMITED, LOG_LVL_WARN, p, 1);
        }
        return;
    }
    last_operation_time = now;

    // ---- P2-H：埋点取值准备（必须在 open_start_time 被刷新/清零之前）----
    // was 此处必然 != open（上方判等已 return）；open_ms = 本次开启时长。
    const bool was = current_state;
    const unsigned long open_ms = (open_start_time != 0) ? (now - open_start_time) : 0;

    // 执行硬件操作
    int level = open ? active_level : (1 - active_level);
    gpio_set_level((gpio_num_t)valve_pin, level);
    current_state = open;

    // 硬件状态 → SystemState 同步
    valve_update_state();

    // 硬件状态变化 → Event 发布
    if (open) {
        event_push(EVENT_VALVE_OPEN, "", "valve", 0, EVENT_POLICY_STATE, 0);
        open_start_time = millis();

        // ---- P2-H 埋点：阀门开启（INFO）----
        // 判等 return 在上方 => 天然边沿去重，重复调用 valve_open() 不会产生第二条。
        safety_timeout_reported = false;   // 新一轮开启 => 解除超时报告锁
        {
            LogParamIn p[2];
            p[0] = log_arg_bool(LOG_P_STATE, true);
            p[1] = log_arg_bool(LOG_P_WAS,   was);
            log_emit(LOG_VALVE_OPEN, LOG_LVL_INFO, p, 2);
        }
    } else {
        event_push(EVENT_VALVE_CLOSE, "", "valve", 0, EVENT_POLICY_STATE, 0);
        open_start_time = 0;   // 关闭时清零

        // ---- P2-H 埋点：阀门关闭（INFO）----
        safety_timeout_reported = false;   // 已真正关闭 => 解除超时报告锁
        {
            LogParamIn p[3];
            p[0] = log_arg_bool(LOG_P_STATE, false);
            p[1] = log_arg_bool(LOG_P_WAS,   was);
            p[2] = log_arg_u32(LOG_P_VALVE_OPEN_MS, (uint32_t)open_ms);
            log_emit(LOG_VALVE_CLOSE, LOG_LVL_INFO, p, 3);
        }
    }

    Serial.printf("[Valve] %s (pin=%d, level=%d)\n",
        open ? "OPEN" : "CLOSE",
        valve_pin,
        level
    );
}

// =====================================================
// Workflow Action 实现（即时完成）
//
// reset: 每次执行前复位
// start: 第一次进入 step 时调用；同步执行并立即给出结果
// poll : workflow_task 周期调用；IDLE 兜底失败
//
// 支持两种调用方式（模块无需感知）：
//   - Workflow Step 调用：workflow_task 驱动 start/poll
//   - Command 临时 Action 调用：CommandManager → workflow_enqueue_action → 引擎驱动 start/poll
// =====================================================
static void valve_action_reset(WorkflowActionInstance *action)
{
    if(action == nullptr)
        return;
    action->result  = ACTION_IDLE;
    action->running = false;
    action->runtime = nullptr;
}

// 打开阀门（即时完成）
static void valve_open_start(WorkflowActionInstance *action)
{
    if(action == nullptr)
        return;
    action->result = valve_open() ? ACTION_SUCCESS : ACTION_FAILED;
}

// 关闭阀门（即时完成）
static void valve_close_start(WorkflowActionInstance *action)
{
    if(action == nullptr)
        return;
    action->result = valve_close() ? ACTION_SUCCESS : ACTION_FAILED;
}

// 当前阀门动作无需异步等待
static void valve_action_poll(WorkflowActionInstance *action)
{
    if(action == nullptr)
        return;
    // 未 start 而直接 poll（异常路径）：直接失败
    if(action->result == ACTION_IDLE)
    {
        action->result = ACTION_FAILED;
    }
}

// =====================================================
// 初始化
// =====================================================
void valve_init()
{
    // 1. 读取配置
    valve_pin = config_get_valve_gpio_pin();
    active_level = config_get_valve_active_level();
    bool enable = config_get_valve_enable();

    // 2. 安全超时钳位（1~300 秒）
    int timeout_sec = config_get_valve_safety_timeout_sec();
    if (timeout_sec < 1) timeout_sec = 1;
    if (timeout_sec > 300) timeout_sec = 300;
    safety_timeout_ms = (unsigned long)timeout_sec * 1000UL;

    // 3. 检查是否启用
    if (!enable) {
        Serial.println("[Valve] Disabled by config");
        return;
    }

    // 4. 检查引脚是否配置
    if (valve_pin < 0) {
        Serial.println("[Valve] GPIO pin not configured, module disabled");
        return;
    }

    // 5. 配置 GPIO
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << valve_pin),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);

    // 6. 初始状态：关闭阀门
    valve_close();

    initialized = true;
    Serial.printf("[Valve] Init OK (pin=%d, active_level=%d, safety=%lu s)\n",
        valve_pin, active_level, safety_timeout_ms / 1000UL);

    // 7. 注册 Workflow Action（id 全局唯一，重复注册返回 false）
    if(!workflow_register_action(&valve_open_desc)) {
        Serial.println("[Valve] VALVE_OPEN register failed");
    }
    if(!workflow_register_action(&valve_close_desc)) {
        Serial.println("[Valve] VALVE_CLOSE register failed");
    }

    // 8. 初始状态同步（SystemState）
    valve_update_state();
    Serial.println("[Valve] System state registered: STATE_VALVE_STATUS");
}

// =====================================================
// 对外接口：打开阀门
// =====================================================
bool valve_open()
{
    if (valve_pin < 0)
        return false;
    valve_set_gpio(true);
    return current_state;
}
// =====================================================
// 对外接口：关闭阀门
// =====================================================
bool valve_close()
{
    if (valve_pin < 0)
        return false;
    valve_set_gpio(false);
    return !current_state;
}

// =====================================================
// 对外接口：切换阀门状态
// =====================================================
bool valve_toggle()
{
    if (valve_pin < 0)
        return false;
    bool target = !current_state;
    valve_set_gpio(target);
    return (current_state == target);
}
// =====================================================
// 获取当前状态
// =====================================================
bool valve_is_open()
{
    return current_state;
}

// =====================================================
// 获取引脚号（调试用）
// =====================================================
int valve_get_pin()
{
    return valve_pin;
}

// =====================================================
// Task 函数（loop 中调用）
//
// 仅做安全超时检测。
// 旧命令队列已移除：阀门命令统一走 CommandManager → WorkflowManager → Action 链路。
// =====================================================
void valve_task()
{
    if (!initialized || valve_pin < 0) {
        return;
    }

    // 关闭状态不检测
    if (!current_state) {
        return;
    }

    unsigned long now = millis();
    if (now - open_start_time >= safety_timeout_ms) {
        Serial.printf("[Valve] Safety timeout! Forced close (open > %lu ms)\n", safety_timeout_ms);

        // ---- P2-H：埋点取值准备（valve_set_gpio 会清零 open_start_time）----
        const unsigned long timed_out_open_ms =
            (open_start_time != 0) ? (now - open_start_time) : 0;

        // 强制关闭
        valve_set_gpio(false);
        // 推送异常事件
        event_push(EVENT_VALVE_ERROR, "Safety timeout", "valve", 0, EVENT_POLICY_STATE, 0);

        // ---- P2-H 埋点：安全超时（WARN，一次性报告锁）----
        // 原有 event_push 保留不动；只有本 log_emit 受锁保护。
        // 锁在"真正开启/关闭成功"时解除 => 一次真实超时只产生一条记录。
        if (!safety_timeout_reported) {
            safety_timeout_reported = true;
            LogParamIn p[2];
            p[0] = log_arg_u32(LOG_P_OPEN_MS,  (uint32_t)timed_out_open_ms);
            p[1] = log_arg_u32(LOG_P_LIMIT_MS, (uint32_t)safety_timeout_ms);
            log_emit(LOG_VALVE_SAFETY_TIMEOUT, LOG_LVL_WARN, p, 2);
        }
    }
}


// =====================================================
// 强制关闭阀门
// 用于安全异常场景://   - Weight error  - 外部保护模块
// 特点://   - 绕过50ms防风暴限制//   - 不判断当前状态//   - 强制同步GPIO，current_state，SystemState，Event
// 不用于普通workflow动作
// =====================================================
bool valve_force_close()
{
    if(valve_pin < 0)    {
        // ---- P2-H 埋点：强制关阀失败（CRITICAL）----
        // 该失败分支此前完全不可观测（调用方只会打一行串口）。
        // 注意：模块被 config 禁用但引脚已配置时 pin>=0 => 不走这里（= VALVE-1，本轮不修，只登记）。
        {
            LogParamIn p[1];
            p[0] = log_arg_u32(LOG_P_CAUSE, (uint32_t)VALVE_CAUSE_WEIGHT_ERROR);
            log_emit(LOG_VALVE_FORCE_CLOSE_FAILED, LOG_LVL_CRITICAL, p, 1);
        }
        return false;
    }
    // ==============================
    // 直接关闭GPIO
    // ==============================
    int level =        active_level ? 0 : 1;
    gpio_set_level(        (gpio_num_t)valve_pin,        level    );
 
    // 更新内部状态
    current_state = false;
    // 更新SystemState
    valve_update_state();
    // ---- P2-H：埋点取值准备（必须在 open_start_time 被清零之前）----
    const unsigned long forced_open_ms =
        (open_start_time != 0) ? (millis() - open_start_time) : 0;

    // 清理安全计时
    open_start_time = 0;
    safety_timeout_reported = false;   // P2-H：强制关闭成功 => 解除超时报告锁
    // 发布关闭事件
    event_push(
        EVENT_VALVE_CLOSE,
        "force close",
        "valve",
        EVENT_PRIORITY_CRITICAL,
        EVENT_POLICY_STATE,
        0
    );
    Serial.printf(
        "[Valve] FORCE CLOSE (pin=%d, level=%d)\n",
        valve_pin,
        level
    );

    // =====================================================
    // P2-H 埋点：强制关阀（CRITICAL，去重门控 —— 决策 D1=A）
    // =====================================================
    //
    // ★ 位置在"GPIO 动作 -> 状态更新 -> SystemState -> open_start_time -> 事件发布"
    //   全部完成之后：日志判断**永远不参与控制流**，force_close 的
    //   "无条件强制同步 GPIO" 语义零改动（不做 current_state 边沿判断）。
    //
    // 门控规则（满足任一才 emit）：
    //   ① cause 发生变化
    //   ② 距上次记录 >= VALVE_FORCE_CLOSE_LOG_COOLDOWN_MS(5s)
    // 否则只执行 force_close、不写日志。
    //
    // 为何必须门控：valve_force_close() 无状态判等，实测权重跳变期可达 6~20 次/s，
    // 而 LOG_VALVE_FORCE_CLOSE 是 CRITICAL => 落 Flash(496 条环) + 进云队列(128 槽)
    // => 峰值下 83s 即可冲光全部历史日志。
    {
        const uint8_t  cause  = VALVE_CAUSE_WEIGHT_ERROR;   // 本轮唯一来源 = dispense_guard
        const uint32_t now_ms = (uint32_t)millis();

        if (cause != last_force_close_cause ||
            (uint32_t)(now_ms - last_force_close_log_ms) >= VALVE_FORCE_CLOSE_LOG_COOLDOWN_MS)
        {
            last_force_close_cause  = cause;
            last_force_close_log_ms = now_ms;

            LogParamIn p[2];
            p[0] = log_arg_u32(LOG_P_CAUSE,         (uint32_t)cause);
            p[1] = log_arg_u32(LOG_P_VALVE_OPEN_MS, (uint32_t)forced_open_ms);
            log_emit(LOG_VALVE_FORCE_CLOSE, LOG_LVL_CRITICAL, p, 2);
        }
    }

    return true;
}