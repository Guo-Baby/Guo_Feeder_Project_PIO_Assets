#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include "event_manager.h"

//=============================
//是否启用workflow的事件响应，填0则不会注册任何事件监听
//=============================
#define WORKFLOW_EVENT_ENABLED 0   // 0=禁用, 1=启用


// =====================================================
// Workflow 自动化框架
//
// 结构:
//
// Workflow
//   |
//   |
// Step[]
//   |
//   +------ Action
//   |
//   +------ Trigger
//

// =====================================================
// 前向声明
// =====================================================

struct WorkflowTriggerDescriptor;
struct WorkflowActionDescriptor;

struct WorkflowTriggerInstance;
struct WorkflowActionInstance;
struct WorkflowStep;
struct Workflow;

#define WORKFLOW_MAX_PARAM 8
#define WORKFLOW_MAX_STEP 16
#define WORKFLOW_MAX_COUNT 16

// =====================================================
// =====================================================
// Step类型
// =====================================================
enum WorkflowStepType
{
    WORKFLOW_STEP_ACTION = 0,
    WORKFLOW_STEP_TRIGGER
};
// =====================================================
// Trigger运行状态
// =====================================================
enum WorkflowTriggerState
{
    TRIGGER_IDLE = 0,
    TRIGGER_RUNNING,
    TRIGGER_SUCCESS,
    TRIGGER_FAILED
};
// =====================================================
// Action执行结果
// =====================================================
enum WorkflowActionResult
{
    ACTION_IDLE = 0,      // 未开始
    ACTION_RUNNING,       // 执行中
    ACTION_SUCCESS,       // 完成
    ACTION_FAILED         // 失败
};
// =====================================================
// 参数类型
//
// 给UI使用
//
// 未来自动生成配置界面
//
// =====================================================
enum WorkflowParamType
{
    PARAM_INT,
    PARAM_FLOAT,
    PARAM_BOOL,
    PARAM_STRING
};

// =====================================================
// Workflow状态
// =====================================================
enum WorkflowState
{
    WORKFLOW_IDLE = 0,
    WORKFLOW_RUNNING,
    WORKFLOW_WAITING,
    WORKFLOW_FINISHED,
    WORKFLOW_TIMEOUT,
    WORKFLOW_ERROR
};

// =====================================================
// Workflow JSON状态
// =====================================================
enum WorkflowJsonState
{
    WORKFLOW_JSON_EMPTY = 0,
    WORKFLOW_JSON_LOADING,
    WORKFLOW_JSON_READY,
    WORKFLOW_JSON_ERROR
};

enum WorkflowInstanceType
{
    INSTANCE_TRIGGER = 0,
    INSTANCE_ACTION
};

struct WorkflowParamValue
{
    String name;

    WorkflowParamType type;

    int int_value;

    float float_value;

    bool bool_value;

    String string_value;
};
// =====================================================
// 参数描述
//
// 注册Trigger / Action时使用
//
// =====================================================
struct WorkflowParam
{
    const char *name;
    WorkflowParamType type;
    const char *unit;
    const char *description;
};

// =====================================================
// Timer Runtime 结构（内部使用）
// =====================================================
struct TimerRuntime
{
    time_t next_trigger_time;
    int last_trigger_minute;
    bool triggered;
    bool expired;
    int type;  // 0: once, 1: daily, 2: weekly
};

// =====================================================
// Delay Runtime 结构（内部使用）
// =====================================================
struct DelayRuntime
{
    unsigned long start_time;
    unsigned long delay_ms;
    bool started;
};
// =====================================================
// Trigger实例
//
// Workflow运行时创建
//
// =====================================================
typedef WorkflowTriggerState
(*WorkflowTriggerHandler)
(
    WorkflowTriggerInstance *trigger
);


struct WorkflowTriggerInstance
{
    const WorkflowTriggerDescriptor *descriptor;
    String id;
    WorkflowParamValue params[WORKFLOW_MAX_PARAM];
    uint8_t param_count;
    WorkflowTriggerState state;
    // void *runtime;  // 删除这一行
    TimerRuntime timer_runtime;  // 新增：内嵌 Timer 运行时
    DelayRuntime delay_runtime;  // 新增：内嵌 Delay 运行时
    bool is_timer;              // 新增：标记是否为 Timer
    bool is_delay;              // 新增：标记是否为 Delay
};
// =====================================================
// Action实例
//
// Workflow运行时创建
//
// =====================================================
struct WorkflowActionInstance
{
    const WorkflowActionDescriptor *descriptor;
    String id;
    WorkflowParamValue params[WORKFLOW_MAX_PARAM];
    uint8_t param_count;
    WorkflowActionResult result;
    // Action是否已经启动 ，用于异步Action。false: 第一次执行。true:后续轮询
    bool started;
    // Action运行上下文，保留给电机、阀门等模块
    void *runtime;
};

// =====================================================
// Workflow对象
//
// 一个自动化任务
//
// =====================================================
//Workflow Step结构


union StepInstance {
    WorkflowTriggerInstance *trigger;
    WorkflowActionInstance *action;
};

struct WorkflowStep
{
    WorkflowStepType type;
    WorkflowInstanceType instance_type;
    String id;
    StepInstance instance;  // ← 替代 void* instance
};

struct Workflow
{
    String id;
    String name;
    bool enable;
    unsigned long start_time;
    unsigned long timeout_ms;
    WorkflowState state;
    WorkflowStep steps[WORKFLOW_MAX_STEP];  // ← 改为固定数组
    uint8_t step_count;
    uint8_t current_step;
};
// =====================================================
// Trigger注册接口
// =====================================================
bool workflow_register_trigger(
    WorkflowTriggerDescriptor *trigger
);
// =====================================================
// Action注册接口
// =====================================================
bool workflow_register_action(
    WorkflowActionDescriptor *action
);
// =====================================================
// Workflow执行
// =====================================================
bool workflow_start(Workflow *workflow, bool skip_first_step = false);
// =====================================================
// Workflow任务
//
// loop调用
//
// 非阻塞
//
// =====================================================
void workflow_task();
void workflow_timer_check();  // 新增：Timer 外部触发检查

// =====================================================
// Trigger检查接口
//
// Workflow内部调用
//
// =====================================================
WorkflowTriggerState workflow_trigger_check(
    WorkflowTriggerInstance *trigger
);
// =====================================================
// Action执行接口
//
// Workflow内部调用
//
// =====================================================
WorkflowActionResult workflow_action_execute(
    WorkflowActionInstance *action
);

// =====================================================
// 外部Action调用，Command Manager调用，根据action id创建临时实例并执行
// =====================================================

struct WorkflowActionTicket
{
    uint32_t instance_id;
    WorkflowActionResult result;
};

bool workflow_enqueue_action(
    const String &id,
    WorkflowParamValue *params,
    uint8_t param_count,
    uint32_t *instance_id,
    void (*callback)(uint32_t instance_id,WorkflowActionResult result) = nullptr,
    unsigned long timeout_ms = 600000
);
// 查询临时 Action 执行状态（用于 CommandManager 轮询）
bool workflow_temp_action_is_complete(
    uint32_t instance_id,
    WorkflowActionResult &result
);

// 获取等待中的临时 Action 数量
uint8_t workflow_temp_action_pending_count();

WorkflowJsonState workflow_get_json_state();
// =====================================================
// Workflow JSON处理，包含加载，保存，重载等动作
//
// 来源:
// Flash文件
// SD卡
// 云端下发
//
// =====================================================
bool workflow_load_json(const String &json);
bool workflow_load_json_file(const char *path);
bool workflow_save_json_file(const char *path);
bool workflow_reload();//Workflow重新加载
bool workflow_export_json(String &json);
//==================================================
//JSON解析结构
//=================================================

extern Workflow workflows[WORKFLOW_MAX_COUNT];

// =====================================================
// Event 响应（Workflow 内部独立实现）
// =====================================================
void workflow_event_init();
void workflow_event_callback(const EventMessage &msg);

//11. JSON解析接口
bool workflow_parse_json(
    JsonDocument &doc
);
//查询接口
uint8_t workflow_get_count();

Workflow*workflow_get(uint8_t index);
// 查询Action注册表
// Command/UI使用
uint8_t workflow_get_action_count();

WorkflowActionDescriptor*
workflow_get_action_descriptor(
    uint8_t index
);
//Trigger查询
uint8_t workflow_get_trigger_count();
WorkflowTriggerDescriptor*
workflow_get_trigger_descriptor(
    uint8_t index
);



void workflow_clear();


typedef WorkflowActionResult(*WorkflowActionHandler)
(WorkflowActionInstance *action);


struct WorkflowActionDescriptor
{
    const char *id;              // 内部唯一ID，例如 valve_open
    const char *name;            // 显示名称，例如 Open Valve
    const char *description;     // UI说明，例如 打开出粮阀门
    const char *module;          // 来源模块

    WorkflowParam *params;
    uint8_t param_count;

    WorkflowActionHandler handler;

};


struct WorkflowTriggerDescriptor
{
    const char *id;
    const char *name;
    const char *description;
    const char *module;

    WorkflowParam *params;
    uint8_t param_count;

    WorkflowTriggerHandler handler;
};

bool workflow_init();

bool workflow_enable(
    const String &id
);


bool workflow_disable(
    const String &id
);


bool workflow_stop(
    const String &id
);


bool workflow_start_by_id(const String &id, bool skip_first_step = false);

