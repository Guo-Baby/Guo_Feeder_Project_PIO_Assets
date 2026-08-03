#include <Arduino.h>
#include <driver/gpio.h>

#include "valve.h"
#include "config_manager.h"
#include "system_state.h"
#include "event_manager.h"
#include "workflow.h"

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
        return;
    }
    last_operation_time = now;

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
    } else {
        event_push(EVENT_VALVE_CLOSE, "", "valve", 0, EVENT_POLICY_STATE, 0);
        open_start_time = 0;   // 关闭时清零
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
// 旧命令队列已移除：阀门命令统一走
// CommandManager → WorkflowManager → Action 链路。
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
        // 强制关闭
        valve_set_gpio(false);
        // 推送异常事件
        event_push(EVENT_VALVE_ERROR, "Safety timeout", "valve", 0, EVENT_POLICY_STATE, 0);
    }
}
