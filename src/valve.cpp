#include "valve.h"
#include "config_manager.h"
#include <driver/gpio.h>
#include "system_state.h"
#include "workflow.h"
#include "event_manager.h"

// =====================================================
// 静态变量
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
// 命令队列（环形缓冲区，用于 EventManager 回调）
// =====================================================
#define CMD_QUEUE_SIZE 8
enum ValveCommand {
    CMD_NONE,
    CMD_OPEN,
    CMD_CLOSE,
    CMD_TOGGLE
};
static ValveCommand cmd_queue[CMD_QUEUE_SIZE];
static volatile uint8_t cmd_head = 0;
static volatile uint8_t cmd_tail = 0;

// =====================================================
// Workflow Action Descriptor（在 valve_init 前定义）
// =====================================================
static WorkflowParam valve_params[] = {
    // 无参数
};

static WorkflowActionResult valve_open_action(WorkflowActionInstance *action);
static WorkflowActionResult valve_close_action(WorkflowActionInstance *action);

static WorkflowActionDescriptor valve_open_desc = {
    .id = "VALVE_OPEN",
    .name = "电磁阀开",
    .module = "valve",
    .params = valve_params,
    .param_count = 0,
    .handler = valve_open_action
};

static WorkflowActionDescriptor valve_close_desc = {
    .id = "VALVE_CLOSE",
    .name = "电磁阀关",
    .module = "valve",
    .params = valve_params,
    .param_count = 0,
    .handler = valve_close_action
};

// =====================================================
// 前向声明
// =====================================================
static void valve_update_state();
static void valve_set_gpio(bool open);
static bool valve_enqueue_command(ValveCommand cmd);
static ValveCommand valve_dequeue_command();

// =====================================================
// 内部函数：入队命令（仅 EventManager 回调使用）
// =====================================================
static bool valve_enqueue_command(ValveCommand cmd)
{
    uint8_t next = (cmd_tail + 1) % CMD_QUEUE_SIZE;
    if (next == cmd_head) {
        return false;  // 队列满
    }
    cmd_queue[cmd_tail] = cmd;
    cmd_tail = next;
    return true;
}

// =====================================================
// 内部函数：出队命令
// =====================================================
static ValveCommand valve_dequeue_command()
{
    if (cmd_head == cmd_tail) {
        return CMD_NONE;
    }
    ValveCommand cmd = cmd_queue[cmd_head];
    cmd_head = (cmd_head + 1) % CMD_QUEUE_SIZE;
    return cmd;
}

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

    // 更新 System State
    valve_update_state();

    // 推送状态变化事件
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
// 事件回调（EventManager 上下文）
// 仅做入队，不执行业务逻辑
// =====================================================
static void valve_command_handler(const EventMessage &msg)
{
    if (msg.event != EVENT_CLOUD_COMMAND) return;

    String cmd = msg.data;
    ValveCommand vcmd = CMD_NONE;
    if (cmd == "valve_open") {
        vcmd = CMD_OPEN;
    } else if (cmd == "valve_close") {
        vcmd = CMD_CLOSE;
    } else if (cmd == "valve_toggle") {
        vcmd = CMD_TOGGLE;
    } else {
        return;
    }

    if (!valve_enqueue_command(vcmd)) {
        // 队列满，丢弃命令（可考虑推送错误事件）
        Serial.println("[Valve] Command queue full, dropped");
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

    // 7. 注册事件监听（云端命令）
    event_subscribe(EVENT_CLOUD_COMMAND, valve_command_handler);
    Serial.println("[Valve] Event subscribed: EVENT_CLOUD_COMMAND");

    // 8. 注册 Workflow Action
    workflow_register_action(&valve_open_desc);
    workflow_register_action(&valve_close_desc);
    Serial.println("[Valve] Workflow actions registered: VALVE_OPEN, VALVE_CLOSE");

    // 9. 注册 System State（初始化时更新一次状态）
    valve_update_state();
    Serial.println("[Valve] System state registered: STATE_VALVE_STATUS");
}

// =====================================================
// Workflow Action Handler（供 workflow 调用）
// =====================================================
static WorkflowActionResult valve_open_action(WorkflowActionInstance *action)
{
    return valve_open() ? ACTION_SUCCESS : ACTION_FAILED;
}

static WorkflowActionResult valve_close_action(WorkflowActionInstance *action)
{
    return valve_close() ? ACTION_SUCCESS : ACTION_FAILED;
}

// =====================================================
// 对外接口：打开阀门
// =====================================================
bool valve_open()
{
    if (!initialized || valve_pin < 0) return false;
    valve_set_gpio(true);
    return current_state;  // 返回实际状态
}

// =====================================================
// 对外接口：关闭阀门
// =====================================================
bool valve_close()
{
    if (!initialized || valve_pin < 0) return false;
    valve_set_gpio(false);
    return !current_state;  // 返回实际状态（关闭为 true）
}

// =====================================================
// 对外接口：切换阀门状态
// =====================================================
bool valve_toggle()
{
    if (!initialized || valve_pin < 0) return false;
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
// 1. 消费命令队列
// 2. 安全超时检测
// =====================================================
void valve_task()
{
    if (!initialized || valve_pin < 0) {
        return;
    }

    // ===== 1. 消费命令队列 =====
    ValveCommand cmd;
    while ((cmd = valve_dequeue_command()) != CMD_NONE) {
        switch (cmd) {
            case CMD_OPEN:
                valve_open();
                break;
            case CMD_CLOSE:
                valve_close();
                break;
            case CMD_TOGGLE:
                valve_toggle();
                break;
            default:
                break;
        }
    }

    // ===== 2. 安全超时检测 =====
    if (!current_state) {
        return;  // 关闭状态不检测
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