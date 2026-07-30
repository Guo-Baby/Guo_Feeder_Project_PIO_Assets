// //测试的代码WorkflowActionResult workflow_action_execute(WorkflowActionInstance *action)
// 
//         Serial.println("[DEBUG] Action descriptor is nullptr");
//     
//         Serial.printf("[DEBUG] Action handler is nullptr for %s\n", action->id.c_str());
//    
//         // ===== 调试打印 =====
// Serial.printf("[DEBUG] Step %d: instance_type=%d, instance=%p, type=%d\n",
//     wf.current_step, step.instance_type, step.instance, step.type);







#include <Arduino.h>
#include <ArduinoJson.h>
#include <esp_heap_caps.h>
#include <LittleFS.h>
#include <new>
#include "system_state.h"
#include "workflow.h"
// =====================================================
// 注册表
// =====================================================
#define WORKFLOW_MAX_TRIGGER 32
#define WORKFLOW_MAX_ACTION 32

static WorkflowTriggerDescriptor* trigger_registry[WORKFLOW_MAX_TRIGGER];
static WorkflowActionDescriptor* action_registry[WORKFLOW_MAX_ACTION];
static uint8_t trigger_count = 0;
static uint8_t action_count = 0;


// =====================================================
// Workflow存储区
// =====================================================
Workflow workflows[WORKFLOW_MAX_COUNT];
static uint8_t workflow_count = 0;


// =====================================================
// JSON状态
// =====================================================
static WorkflowJsonState json_state = WORKFLOW_JSON_EMPTY;

// =====================================================
// 当前加载JSON缓存（用于reload）
// =====================================================
static String workflow_json_cache;

// =====================================================
// Workflow运行实例池（PSRAM分配）
// =====================================================
static WorkflowTriggerInstance* trigger_instances = nullptr;
static WorkflowActionInstance* action_instances = nullptr;
static bool trigger_instances_psram = false;
static bool action_instances_psram = false;
static uint16_t trigger_instance_index = 0;
static uint16_t action_instance_index = 0;


// =====================================================
// 临时 Action 任务队列（用于 Command 调用）
// =====================================================
#define MAX_TEMP_ACTIONS 8
#define MAX_TEMP_ACTION_INST MAX_TEMP_ACTIONS


struct TempActionItem {

    uint32_t instance_id;
    WorkflowActionDescriptor *desc;
    WorkflowParamValue params[WORKFLOW_MAX_PARAM];
    uint8_t param_count;
    bool completed;
    WorkflowActionResult result;
    unsigned long start_time;
    WorkflowActionInstance *instance;
    void (*callback)(uint32_t instance_id,WorkflowActionResult result);
    unsigned long timeout_ms;
};

static TempActionItem temp_action_queue[MAX_TEMP_ACTIONS];

static uint8_t queue_wr_ptr = 0;
static uint8_t queue_rd_ptr = 0;
// 临时Action唯一ID生成器
static uint32_t temp_action_instance_counter = 0;

static WorkflowActionInstance temp_action_instances[MAX_TEMP_ACTION_INST];
// 简易分配器：查找空闲实例
static WorkflowActionInstance* temp_action_alloc_instance()
{
    for(uint8_t i = 0; i < MAX_TEMP_ACTION_INST; i++)
    {
        if(temp_action_instances[i].descriptor == nullptr)
        {
            WorkflowActionInstance *inst = &temp_action_instances[i];

            inst->started = false;
            inst->runtime = nullptr;
            inst->result = ACTION_IDLE;

            return inst;
        }
    }

    return nullptr;
}

// 释放实例
static void temp_action_free_instance(WorkflowActionInstance *inst)
{
inst->descriptor = nullptr;
inst->id = "";
inst->param_count = 0;
inst->result = ACTION_IDLE;
inst->started = false;
inst->runtime = nullptr;
    

    for(uint8_t i = 0; i < WORKFLOW_MAX_PARAM; i++)
    {
        inst->params[i].name = "";
        inst->params[i].type = PARAM_INT;
        inst->params[i].string_value = "";
        inst->params[i].bool_value = false;
        inst->params[i].int_value = 0;
        inst->params[i].float_value = 0.0f;
    }
}
// =====================================================
// 内部查找函数
// =====================================================
static WorkflowTriggerDescriptor* find_trigger_descriptor(const String &id)
{
    for(uint8_t i = 0; i < trigger_count; i++) {
        if(trigger_registry[i] == nullptr) continue;
        if(id == trigger_registry[i]->id) {
            return trigger_registry[i];
        }
    }
    return nullptr;
}

static WorkflowActionDescriptor* find_action_descriptor(const String &id)
{
    for(uint8_t i = 0; i < action_count; i++) {
        if(action_registry[i] == nullptr) continue;
        if(id == action_registry[i]->id) {
            return action_registry[i];
        }
    }
    return nullptr;
}

void workflow_destroy_all_instances()
{
    uint16_t count = WORKFLOW_MAX_COUNT * WORKFLOW_MAX_STEP;

    if(trigger_instances != nullptr)
    {
        for(uint16_t i = 0; i < count; i++)
        {
            // 删除 runtime 相关代码
            trigger_instances[i].~WorkflowTriggerInstance();
        }

        if(trigger_instances_psram)
        {
            heap_caps_free(trigger_instances);
        }
        else
        {
            free(trigger_instances);
        }

        trigger_instances = nullptr;
        trigger_instances_psram = false;
    }

    if(action_instances != nullptr)
    {
        for(uint16_t i = 0; i < count; i++)
        {
            // 删除 runtime 相关代码
            action_instances[i].~WorkflowActionInstance();
        }

        if(action_instances_psram)
        {
            heap_caps_free(action_instances);
        }
        else
        {
            free(action_instances);
        }

        action_instances = nullptr;
        action_instances_psram = false;
    }
}

// =====================================================
// 辅助函数：从 params 中获取整数参数
// =====================================================
static int timer_get_int_param(WorkflowParamValue *params, uint8_t count, const char *name, int default_val)
{
    for (uint8_t i = 0; i < count; i++) {
        if (params[i].name == name) {
            return params[i].int_value;
        }
    }
    return default_val;
}

// =====================================================
// 辅助函数：从 params 中获取字符串参数
// =====================================================
static String timer_get_string_param(WorkflowParamValue *params, uint8_t count, const char *name)
{
    for (uint8_t i = 0; i < count; i++) {
        if (params[i].name == name) {
            return params[i].string_value;
        }
    }
    return "";
}

// =====================================================
// Timer 核心算法：计算下一次触发时间
// =====================================================
static time_t timer_calculate_next(WorkflowTriggerInstance *trigger)
{
    TimerRuntime *rt = &trigger->timer_runtime;

    int hour = timer_get_int_param(trigger->params, trigger->param_count, "hour", 8);
    int minute = timer_get_int_param(trigger->params, trigger->param_count, "minute", 0);

    time_t now = time(nullptr);
    struct tm *tm_now = localtime(&now);
    struct tm target = *tm_now;
    target.tm_hour = hour;
    target.tm_min = minute;
    target.tm_sec = 0;

    if (rt->type == 0) {  // once
        int year = timer_get_int_param(trigger->params, trigger->param_count, "year", 2026);
        int month = timer_get_int_param(trigger->params, trigger->param_count, "month", 1);
        int day = timer_get_int_param(trigger->params, trigger->param_count, "day", 1);
        target.tm_year = year - 1900;
        target.tm_mon = month - 1;
        target.tm_mday = day;
        return mktime(&target);
    }
    else if (rt->type == 1) {  // daily
        time_t t = mktime(&target);
        if (t <= now) t += 86400;
        return t;
    }
    else if (rt->type == 2) {  // weekly
        int target_wday = timer_get_int_param(trigger->params, trigger->param_count, "day_of_week", 1);
        int days_ahead = target_wday - tm_now->tm_wday;
        if (days_ahead <= 0) days_ahead += 7;
        target.tm_mday += days_ahead;
        return mktime(&target);
    }

    return 0;
}

// =====================================================
// Timer Trigger Handler
// =====================================================
static WorkflowTriggerState timer_handler(WorkflowTriggerInstance *trigger)
{
    // =============================================
    // 第一步：检查系统时间是否可信
    // =============================================
    if (!state_get_bool(STATE_TIME_VALID)) {
        // 时间不可信，不触发，不初始化，等待下一次轮询
        return TRIGGER_RUNNING;
    }

    time_t now = time(nullptr);
    TimerRuntime *rt = &trigger->timer_runtime;

    // =============================================
    // 第二步：初始化（第一次调用时）
    // =============================================
    if (trigger->is_timer == false) {
        trigger->is_timer = true;
        // 初始化 rt 的所有字段
        rt->next_trigger_time = 0;
        rt->last_trigger_minute = -1;
        rt->triggered = false;
        rt->expired = false;
        rt->type = 1;  // 默认 daily
    


        // 解析类型
        String type_str = timer_get_string_param(trigger->params, trigger->param_count, "type");
        if (type_str == "once") rt->type = 0;
        else if (type_str == "daily") rt->type = 1;
        else if (type_str == "weekly") rt->type = 2;
        else rt->type = 1;  // 默认 daily

        // 计算目标时间
        rt->next_trigger_time = timer_calculate_next(trigger);
        if (rt->next_trigger_time == 0)
        {
            rt->expired = true;
            return TRIGGER_FAILED;
        }

        // =============================================
        // 一次性过期检测（once 类型，时间已过）
        // =============================================
        if (rt->type == 0 && rt->next_trigger_time < now) {
            rt->expired = true;
            return TRIGGER_FAILED;
        }

        return TRIGGER_RUNNING;
    }

    // =============================================
    // 第三步：已过期 → 永远失败
    // =============================================
    if (rt->expired) {
        return TRIGGER_FAILED;
    }

    // =============================================
    // 第四步：跨分钟复位（用于每日/每周重复）
    // =============================================
    int current_minute = now / 60;
    if (current_minute != rt->last_trigger_minute) {
        rt->triggered = false;
        rt->last_trigger_minute = current_minute;
    }

    // =============================================
    // 第五步：本轮已触发 → 等待下一轮
    // =============================================
    if (rt->triggered) {
        return TRIGGER_RUNNING;
    }

    // =============================================
    // 第六步：检查是否到达目标时间（容忍窗口 ±6 秒）
    // =============================================
    time_t diff = now - rt->next_trigger_time;
    if (diff >= -6 && diff <= 6) {
        rt->triggered = true;

        // 计算下一次触发时间（daily/weekly）
        rt->next_trigger_time = timer_calculate_next(trigger);
        if (rt->next_trigger_time == 0) {
            rt->expired = true;
        }
        return TRIGGER_SUCCESS;
    }

    return TRIGGER_RUNNING;
}

// =====================================================
// Delay Trigger Handler
// =====================================================
static WorkflowTriggerState delay_handler(WorkflowTriggerInstance *trigger)
{
    // ===== 将 rt 声明移到函数开头 =====
    DelayRuntime *rt = &trigger->delay_runtime;

    if (!trigger->is_delay) {
        trigger->is_delay = true;
        int seconds = timer_get_int_param(trigger->params, trigger->param_count, "seconds", 1);
        if (seconds < 1) seconds = 1;
        rt->delay_ms = (unsigned long)seconds * 1000UL;
        rt->started = false;
        rt->start_time = 0;
        return TRIGGER_RUNNING;
    }

    if (!rt->started) {
        rt->start_time = millis();
        rt->started = true;
        return TRIGGER_RUNNING;
    }

    if (millis() - rt->start_time >= rt->delay_ms) {
        return TRIGGER_SUCCESS;
    }

    return TRIGGER_RUNNING;
}

// =====================================================
// 初始化
// =====================================================
bool workflow_init()
{
    workflow_count = 0;
    trigger_count = 0;
    action_count = 0;
    trigger_instance_index = 0;
    action_instance_index = 0;
    json_state = WORKFLOW_JSON_EMPTY;
    workflow_json_cache = "";

    for(uint8_t i = 0; i < WORKFLOW_MAX_COUNT; i++)
    {
        new (&workflows[i]) Workflow();
    }
    memset(trigger_registry, 0, sizeof(trigger_registry));
    memset(action_registry, 0, sizeof(action_registry));

    workflow_destroy_all_instances();

    size_t trigger_size = WORKFLOW_MAX_COUNT * WORKFLOW_MAX_STEP * sizeof(WorkflowTriggerInstance);
    size_t action_size = WORKFLOW_MAX_COUNT * WORKFLOW_MAX_STEP * sizeof(WorkflowActionInstance);

    trigger_instances = (WorkflowTriggerInstance*)heap_caps_malloc(trigger_size, MALLOC_CAP_SPIRAM);
    action_instances = (WorkflowActionInstance*)heap_caps_malloc(action_size, MALLOC_CAP_SPIRAM);
    if(trigger_instances != nullptr)
    {
        trigger_instances_psram = true;
    }

    if(action_instances != nullptr)
    {
        action_instances_psram = true;
    }

    if (trigger_instances == nullptr || action_instances == nullptr) {
        if (trigger_instances != nullptr) {
            heap_caps_free(trigger_instances);
            trigger_instances = nullptr;
        }
        if (action_instances != nullptr) {
            heap_caps_free(action_instances);
            action_instances = nullptr;
        }

        trigger_instances = (WorkflowTriggerInstance*)malloc(trigger_size);
        action_instances = (WorkflowActionInstance*)malloc(action_size);
        trigger_instances_psram = false;
        action_instances_psram = false;

        if (trigger_instances == nullptr || action_instances == nullptr) {
            if (trigger_instances != nullptr) {
                free(trigger_instances);
                trigger_instances = nullptr;
            }
            if (action_instances != nullptr) {
                free(action_instances);
                action_instances = nullptr;
            }
            Serial.println("[Workflow] PSRAM + RAM allocate fail");
            return false;
        }
    }

    uint16_t instance_count = WORKFLOW_MAX_COUNT * WORKFLOW_MAX_STEP;

    for(uint16_t i = 0; i < instance_count; i++)
    {
        new (&trigger_instances[i]) WorkflowTriggerInstance();
        new (&action_instances[i]) WorkflowActionInstance();
    }
    // ===== 新增：注册内置 Timer Trigger =====
    static WorkflowParam timer_params[] = {
        {"type", PARAM_STRING, "", "once/daily/weekly"},
        {"hour", PARAM_INT, "", "0-23"},
        {"minute", PARAM_INT, "", "0-59"},
        {"year", PARAM_INT, "", "2026"},
        {"month", PARAM_INT, "", "1-12"},
        {"day", PARAM_INT, "", "1-31"},
        {"day_of_week", PARAM_INT, "", "0-6 (0=Sunday)"}
    };

    static WorkflowTriggerDescriptor timer_desc = {
        "timer",
        "Timer",
        "system",
        "系统定时触发器",
        timer_params,
        7,
        timer_handler
    };

    // ===== 新增：注册内置 Delay Trigger =====
    static WorkflowParam delay_params[] = {
        {"seconds", PARAM_INT, "", "1-3600"}
    };

    static WorkflowTriggerDescriptor delay_desc = {
        "delay",
        "Delay",
        "system",
        "延时触发器",
        delay_params,
        1,
        delay_handler
    };

    workflow_register_trigger(&timer_desc);
    workflow_register_trigger(&delay_desc);
            // 初始化临时action环形队列指针
    queue_wr_ptr = 0;
    queue_rd_ptr = 0;
    // 初始化实例ID计数
    temp_action_instance_counter = 0;
    memset(temp_action_queue,0,sizeof(temp_action_queue));
    
    return true;
}

// =====================================================
// 注册Trigger
// =====================================================
bool workflow_register_trigger(WorkflowTriggerDescriptor *trigger)
{
    if(trigger == nullptr) return false;
    if(trigger->id == nullptr) return false;
    if(trigger_count >= WORKFLOW_MAX_TRIGGER) return false;

    for(uint8_t i = 0; i < trigger_count; i++) {
        if(strcmp(trigger_registry[i]->id, trigger->id) == 0) {
            return false;
        }
    }

    trigger_registry[trigger_count] = trigger;
    trigger_count++;
    return true;
}

// =====================================================
// 注册Action
// =====================================================
bool workflow_register_action(WorkflowActionDescriptor *action)
{
    if(action == nullptr) return false;
    if(action->id == nullptr) return false;
    if(action_count >= WORKFLOW_MAX_ACTION) return false;

    for(uint8_t i = 0; i < action_count; i++) {
        if(strcmp(action_registry[i]->id, action->id) == 0) {
            return false;
        }
    }

    action_registry[action_count] = action;
    action_count++;
    return true;
}

// =====================================================
// 查询接口
// =====================================================
uint8_t workflow_get_trigger_count() { return trigger_count; }
uint8_t workflow_get_action_count() { return action_count; }
// =====================================================
// 根据ID查找Action Descriptor
// Command内部调用
// 获取Action数量
// 已有 workflow_get_action_count()
// 获取Action Descriptor
// 已有 workflow_get_action_descriptor()
// =====================================================
// =====================================================
WorkflowActionDescriptor*
workflow_find_action(
    const String &id
)
{
    return find_action_descriptor(id);
}

WorkflowTriggerDescriptor* workflow_get_trigger_descriptor(uint8_t index)
{
    if(index >= trigger_count) return nullptr;
    return trigger_registry[index];
}

WorkflowActionDescriptor* workflow_get_action_descriptor(uint8_t index)
{
    if(index >= action_count) return nullptr;
    return action_registry[index];
}



uint8_t workflow_get_count() { return workflow_count; }

Workflow* workflow_get(uint8_t index)
{
    if(index >= workflow_count) return nullptr;
    return &workflows[index];
    
}

// =====================================================
// 清空Workflow（只清空数据，不销毁实例池）
// =====================================================
void workflow_clear()
{
    // =====================================================
    // 1. 清理所有 Workflow
    // =====================================================
    for(uint8_t i = 0; i < WORKFLOW_MAX_COUNT; i++) {
        workflows[i].id = "";
        workflows[i].name = "";
        workflows[i].enable = false;
        workflows[i].state = WORKFLOW_IDLE;
        workflows[i].step_count = 0;
        workflows[i].current_step = 0;
        workflows[i].start_time = 0;
        workflows[i].timeout_ms = 0;
        for(uint8_t j = 0; j < WORKFLOW_MAX_STEP; j++) {
            workflows[i].steps[j].id = "";
            workflows[i].steps[j].instance.trigger = nullptr;
            workflows[i].steps[j].instance.action = nullptr;
            workflows[i].steps[j].type = WORKFLOW_STEP_ACTION;
            workflows[i].steps[j].instance_type = INSTANCE_ACTION;
        }
    }

    // =====================================================
    // 2. 清理 Trigger Instance 池
    // =====================================================
    for(uint16_t i = 0; i < WORKFLOW_MAX_COUNT * WORKFLOW_MAX_STEP; i++) {
        WorkflowTriggerInstance &inst = trigger_instances[i];
        inst.id = "";
        inst.descriptor = nullptr;
        inst.param_count = 0;
        inst.state = TRIGGER_IDLE;
        // timer runtime 清零
        inst.timer_runtime.next_trigger_time = 0;
        inst.timer_runtime.last_trigger_minute = -1;
        inst.timer_runtime.triggered = false;
        inst.timer_runtime.expired = false;
        inst.timer_runtime.type = 0;
        // delay runtime 清零
        inst.delay_runtime.start_time = 0;
        inst.delay_runtime.delay_ms = 0;
        inst.delay_runtime.started = false;
        inst.is_timer = false;
        inst.is_delay = false;
        // 参数清理
        for(uint8_t p = 0; p < WORKFLOW_MAX_PARAM; p++) {
            inst.params[p].name = "";
            inst.params[p].type = PARAM_INT;
            inst.params[p].int_value = 0;
            inst.params[p].float_value = 0.0f;
            inst.params[p].bool_value = false;
            inst.params[p].string_value = "";
        }
    }

    // =====================================================
    // 3. 清理 Action Instance 池
    // =====================================================
    for(uint16_t i = 0; i < WORKFLOW_MAX_COUNT * WORKFLOW_MAX_STEP; i++) {
        WorkflowActionInstance &inst = action_instances[i];
        // 如果未来 runtime 使用 heap，这里统一释放入口
        if(inst.runtime != nullptr) {
            inst.runtime = nullptr;
        }
        inst.id = "";
        inst.descriptor = nullptr;
        inst.param_count = 0;
        inst.result = ACTION_IDLE;
        // 新增：异步 Action 状态清理
        inst.started = false;
        for(uint8_t p = 0; p < WORKFLOW_MAX_PARAM; p++) {
            inst.params[p].name = "";
            inst.params[p].type = PARAM_INT;
            inst.params[p].int_value = 0;
            inst.params[p].float_value = 0.0f;
            inst.params[p].bool_value = false;
            inst.params[p].string_value = "";
        }
    }

    // =====================================================
    // 4. 清理索引
    // =====================================================
    workflow_count = 0;
    trigger_instance_index = 0;
    action_instance_index = 0;
}

// =====================================================
// 获取JSON状态
// =====================================================
WorkflowJsonState workflow_get_json_state() { return json_state; }

// =====================================================
// 参数解析
// =====================================================
static void workflow_parse_params(JsonObject obj, WorkflowParamValue *values, uint8_t &count)
{
    count = 0;
    for(JsonPair kv : obj) {
        if(count >= WORKFLOW_MAX_PARAM) break;
        WorkflowParamValue &p = values[count];
        p.name = kv.key().c_str();

        JsonVariant v = kv.value();
        if(v.is<bool>()) {
            p.type = PARAM_BOOL;
            p.bool_value = v.as<bool>();
        } else if(v.is<int>()) {
            p.type = PARAM_INT;
            p.int_value = v.as<int>();
        } else if(v.is<float>()) {
            p.type = PARAM_FLOAT;
            p.float_value = v.as<float>();
        } else {
            p.type = PARAM_STRING;
            p.string_value = v.as<String>();
        }
        count++;
    }
}

// =====================================================
// JSON解析
// =====================================================
bool workflow_parse_json(JsonDocument &doc)
{
    trigger_instance_index = 0;
    action_instance_index = 0;

    workflow_clear();

    JsonArray arr = doc["workflows"];
    if(arr.isNull()) {
        json_state = WORKFLOW_JSON_ERROR;
        return false;
    }

    uint8_t index = 0;
    for(JsonObject wf : arr) {
        if(index >= WORKFLOW_MAX_COUNT) break;

        Workflow &workflow = workflows[index];
        workflow.id = wf["id"].as<String>();
        workflow.name = wf["name"].as<String>();
        workflow.enable = wf["enable"] | false;
        workflow.timeout_ms = wf["timeout_ms"] | 600000;
        workflow.state = WORKFLOW_IDLE;
        workflow.current_step = 0;

        JsonArray steps = wf["steps"];
        uint8_t step_index = 0;

        for(JsonObject step : steps) {
            if(step_index >= WORKFLOW_MAX_STEP) break;

            WorkflowStep &s = workflow.steps[step_index];
            String type = step["type"].as<String>();
            s.id = step["id"].as<String>();

            if(type == "trigger") {
                s.type = WORKFLOW_STEP_TRIGGER;
                s.instance_type = INSTANCE_TRIGGER;

                if(trigger_instance_index >= WORKFLOW_MAX_COUNT * WORKFLOW_MAX_STEP) {
                    json_state = WORKFLOW_JSON_ERROR;
                    return false;
                }

                // ===== 修复后的 Trigger 实例分配逻辑 =====
                // 先查找 descriptor，确认存在后再分配实例
                WorkflowTriggerDescriptor *desc = find_trigger_descriptor(s.id);
                if (desc == nullptr) {
                    // descriptor 不存在，不分配实例，直接跳过
                    s.instance.trigger = nullptr;
                    step_index++;
                    continue;
                }

                // 确认 descriptor 存在后，再分配实例
                if (trigger_instance_index >= WORKFLOW_MAX_COUNT * WORKFLOW_MAX_STEP) {
                    json_state = WORKFLOW_JSON_ERROR;
                    return false;
                }

                WorkflowTriggerInstance *inst = &trigger_instances[trigger_instance_index++];


                // 完整初始化 Trigger Instance

                inst->id = s.id;
                inst->descriptor = desc;

                inst->param_count = 0;
                inst->state = TRIGGER_IDLE;


                // 清理 runtime 状态

                inst->timer_runtime.next_trigger_time = 0;
                inst->timer_runtime.last_trigger_minute = -1;
                inst->timer_runtime.triggered = false;
                inst->timer_runtime.expired = false;
                inst->timer_runtime.type = 0;


                inst->delay_runtime.start_time = 0;
                inst->delay_runtime.delay_ms = 0;
                inst->delay_runtime.started = false;


                inst->is_timer = false;
                inst->is_delay = false;


                // 清理参数

                for(uint8_t i = 0; i < WORKFLOW_MAX_PARAM; i++)
                {
                    inst->params[i].name = "";
                    inst->params[i].type = PARAM_INT;
                    inst->params[i].int_value = 0;
                    inst->params[i].float_value = 0.0f;
                    inst->params[i].bool_value = false;
                    inst->params[i].string_value = "";
                }

                JsonObject step_params = step["params"];
                workflow_parse_params(step_params, inst->params, inst->param_count);
                s.instance.trigger = inst;
            }
            else if(type == "action") {
                s.type = WORKFLOW_STEP_ACTION;
                s.instance_type = INSTANCE_ACTION;

                if(action_instance_index >= WORKFLOW_MAX_COUNT * WORKFLOW_MAX_STEP) {
                    json_state = WORKFLOW_JSON_ERROR;
                    return false;
                }

                // ===== 修复后的 Action 实例分配逻辑 =====
                // 先查找 descriptor，确认存在后再分配实例
                WorkflowActionDescriptor *desc = find_action_descriptor(s.id);
                if (desc == nullptr) {
                    // descriptor 不存在，不分配实例，直接跳过
                    s.instance.action = nullptr;
                    step_index++;
                    continue;
                }

                // 确认 descriptor 存在后，再分配实例
                if (action_instance_index >= WORKFLOW_MAX_COUNT * WORKFLOW_MAX_STEP) {
                    json_state = WORKFLOW_JSON_ERROR;
                    return false;
                }

                WorkflowActionInstance *inst = &action_instances[action_instance_index++];


                // 完整初始化 Action Instance

                inst->id = s.id;
                inst->descriptor = desc;

                inst->param_count = 0;
                inst->result = ACTION_IDLE;
                inst->started = false;
                inst->runtime = nullptr;


                // 清理参数

                for(uint8_t i = 0; i < WORKFLOW_MAX_PARAM; i++)
                {
                    inst->params[i].name = "";
                    inst->params[i].type = PARAM_INT;
                    inst->params[i].int_value = 0;
                    inst->params[i].float_value = 0.0f;
                    inst->params[i].bool_value = false;
                    inst->params[i].string_value = "";
                }

                JsonObject step_params = step["params"];
                workflow_parse_params(step_params, inst->params, inst->param_count);
                s.instance.action = inst;
            }

            step_index++;
        }

        workflow.step_count = step_index;

        // ===== Step0 必须是 Trigger =====
        if (workflow.step_count > 0) {
            WorkflowStep &first = workflow.steps[0];
            if (first.type != WORKFLOW_STEP_TRIGGER) {
                workflow.state = WORKFLOW_ERROR;
            }
        }

        index++;
    }

    workflow_count = index;
    json_state = WORKFLOW_JSON_READY;

    #if WORKFLOW_EVENT_ENABLED
    workflow_event_init();
    #endif

    
    return true;
}

// =====================================================
// JSON加载
// =====================================================
bool workflow_load_json(const String &json)
{
    json_state = WORKFLOW_JSON_LOADING;
    workflow_json_cache = json;

    JsonDocument doc;
    DeserializationError error = ArduinoJson::deserializeJson(doc, json);
    if(error) {
        json_state = WORKFLOW_JSON_ERROR;
        return false;
    }

    bool result = workflow_parse_json(doc);
    json_state = result ? WORKFLOW_JSON_READY : WORKFLOW_JSON_ERROR;
    return result;
}

// =====================================================
// JSON文件加载
// =====================================================
bool workflow_load_json_file(const char *path)
{
    if (path == nullptr) return false;

    

    File file = LittleFS.open(path, "r");
    if (!file) {
            Serial.printf("Failed to open %s\n", path);
            return false;
        }


    String json;
    while (file.available()) {
        json += (char)file.read();
    }
    file.close();

    return workflow_load_json(json);
}

// =====================================================
// JSON保存文件
// =====================================================
bool workflow_save_json_file(const char *path)
{
    if (path == nullptr) return false;

    String json;
    if (!workflow_export_json(json)) return false;

    File file = LittleFS.open(path, "w");
    if (!file) {
        Serial.printf("Failed to open %s for writing\n", path);
        return false;
    }

    size_t written = file.print(json);
    file.close();

    return (written == json.length());
}

// =====================================================
// 保存JSON缓存
// =====================================================
bool workflow_save_json(String &json)
{
    json = workflow_json_cache;
    return true;
}

// =====================================================
// Workflow重新加载
// =====================================================
bool workflow_reload()
{

    if(workflow_json_cache.length() == 0)
        return false;
    // 防止运行中的 Workflow 被强制清理
    for(uint8_t i = 0; i < workflow_count; i++)
    {
        if(workflows[i].state == WORKFLOW_RUNNING)
        {
            Serial.println(
                "[Workflow] Reload blocked: workflow running"
            );
            return false;
        }
    }
    // 检查临时 Action 队列
    if(queue_rd_ptr != queue_wr_ptr)
    {
        Serial.println(
            "[Workflow] Reload blocked: temp action running"
        );
        return false;
    }
    return workflow_load_json(
        workflow_json_cache
    );
}

// =====================================================
// Workflow启动
// =====================================================
bool workflow_start(Workflow *workflow, bool skip_first_step)
{
    if (workflow == nullptr) return false;
    if (!workflow->enable) return false;
    if (workflow->state == WORKFLOW_RUNNING) return false;
    
    // 重置所有 step 的状态
    for (uint8_t i = 0; i < workflow->step_count; i++) {
        WorkflowStep &step = workflow->steps[i];
        if (
            step.instance_type == INSTANCE_TRIGGER 
            && step.instance.trigger != nullptr
        )
        {    WorkflowTriggerInstance *trigger = step.instance.trigger;
            trigger->state = TRIGGER_IDLE;
            // Timer reset
            trigger->timer_runtime.next_trigger_time = 0;
            trigger->timer_runtime.last_trigger_minute = -1;
            trigger->timer_runtime.triggered = false;
            trigger->timer_runtime.expired = false;
            trigger->timer_runtime.type = 0;
            // Delay reset
            trigger->delay_runtime.start_time = 0;
            trigger->delay_runtime.delay_ms = 0;
            trigger->delay_runtime.started = false;
            trigger->is_timer = false;
            trigger->is_delay = false;
        }
        else if (
            step.instance_type == INSTANCE_ACTION 
            && step.instance.action != nullptr
        )
        {
            WorkflowActionInstance *action = step.instance.action;
            action->result = ACTION_IDLE;
            action->started = false;
            // 清理运行上下文
            // 具体Action如果需要重新创建runtime，
            // 将在handler第一次执行时重新初始化
            action->runtime = nullptr;
        }
    }
    
    workflow->state = WORKFLOW_RUNNING;
    workflow->current_step = skip_first_step ? 1 : 0;
    workflow->start_time = millis();
    return true;
}

// =====================================================
// Trigger检查
// =====================================================
WorkflowTriggerState workflow_trigger_check(WorkflowTriggerInstance *trigger)
{
    if(trigger == nullptr) return TRIGGER_FAILED;
    if(trigger->descriptor == nullptr) return TRIGGER_FAILED;
    if(trigger->descriptor->handler == nullptr) return TRIGGER_FAILED;

    trigger->state = trigger->descriptor->handler(trigger);
    return trigger->state;
}

// =====================================================
// Action执行
// =====================================================
WorkflowActionResult workflow_action_execute(WorkflowActionInstance *action)
{
    if(action == nullptr)
        return ACTION_FAILED;

    if(action->descriptor == nullptr) {
        Serial.println("[Workflow] Action descriptor nullptr");
        action->result = ACTION_FAILED;
        action->started = false;
        return ACTION_FAILED;
    }
    if(action->descriptor->handler == nullptr) {
        Serial.printf("[Workflow] Action handler nullptr:%s\n", action->id.c_str());
        action->result = ACTION_FAILED;
        action->started = false;
        return ACTION_FAILED;
    }
    /*
        Action 状态机:
        第一次: started=false, handler负责启动动作
        中间:   started=true,  handler负责轮询状态
        结束:   SUCCESS / FAILED, 自动复位 started
    */
    WorkflowActionResult result = action->descriptor->handler(action);
    if(result == ACTION_RUNNING) {
        action->started = true;
    } else {
        action->started = false;
    }
    action->result = result;
    return result;
}
// =====================================================
// 入队临时 Action（非阻塞）
// =====================================================
static bool enqueue_temp_action(
    const String &id,
    WorkflowParamValue *params,
    uint8_t param_count,
    uint32_t *instance_id,
    void (*callback)(
        uint32_t instance_id,
        WorkflowActionResult result
    ),
    unsigned long timeout_ms
)
{
    uint8_t next_wr = (queue_wr_ptr + 1) % MAX_TEMP_ACTIONS;
    // 环形队列判满
    if (next_wr == queue_rd_ptr)
    {
        return false;
    }

    WorkflowActionDescriptor *desc = find_action_descriptor(id);
    if (desc == nullptr) {
        return false;
    }

    WorkflowActionInstance *inst = temp_action_alloc_instance();
    if(inst == nullptr)
    {
        return false;
    }

    // 初始化 Action Instance

    inst->descriptor = desc;
    inst->id = desc->id;
    inst->param_count = 0;
    inst->result = ACTION_IDLE;
    inst->started = false;
    inst->runtime = nullptr;


    // 清空旧参数
    for(uint8_t i = 0; i < WORKFLOW_MAX_PARAM; i++)
    {
        inst->params[i].name = "";
        inst->params[i].type = PARAM_INT;
        inst->params[i].int_value = 0;
        inst->params[i].float_value = 0.0f;
        inst->params[i].bool_value = false;
        inst->params[i].string_value = "";
    }


    // 写入新参数
    for(uint8_t i = 0; i < param_count; i++)
    {
        inst->params[i] = params[i];
    }

    inst->param_count = param_count;

    TempActionItem &item = temp_action_queue[queue_wr_ptr];
    // ===============================
    // 分配唯一实例ID
    // ===============================
    temp_action_instance_counter++;
    // 防止uint32溢出后变0
    if(temp_action_instance_counter == 0)
    {
        temp_action_instance_counter = 1;
    }
    item.instance_id = temp_action_instance_counter;
    if(instance_id != nullptr)
    {
        *instance_id = item.instance_id;
    }
    item.desc = desc;
    item.instance = inst;
    item.param_count = param_count;
    item.completed = false;
    item.result = ACTION_IDLE;
    item.start_time = millis();
    item.callback = callback;
    item.timeout_ms = timeout_ms;
    queue_wr_ptr = next_wr;
    return true;
}
// =====================================================
// 外部Action执行接口
//
// Command调用
//
// 不创建Workflow运行实例
//
// =====================================================
bool workflow_enqueue_action(
    const String &id,
    WorkflowParamValue *params,
    uint8_t param_count,
    uint32_t *instance_id,
    void (*callback)(
        uint32_t instance_id,
        WorkflowActionResult result
    ),
    unsigned long timeout_ms
)
{
    return enqueue_temp_action(
        id,
        params,
        param_count,
        instance_id,
        callback,
        timeout_ms
    );
}

// =====================================================
// 查询临时 Action 是否已完成
// CommandManager 轮询使用
// =====================================================
bool workflow_temp_action_is_complete(
    uint32_t instance_id,
    WorkflowActionResult &result
)
{
    uint8_t rd = queue_rd_ptr;
    while (rd != queue_wr_ptr)
    {
        TempActionItem &item = temp_action_queue[rd];
        if (item.instance_id == instance_id)
        {
            if (item.completed)
            {
                result = item.result;
                return true;
            }
            return false;
        }
        rd = (rd + 1) % MAX_TEMP_ACTIONS;
    }
    // 找不到实例
    result = ACTION_IDLE;
    return false;
}

// =====================================================
// 回收已经完成的 Temp Action
// =====================================================
static void workflow_temp_action_cleanup()
{
    while(queue_rd_ptr != queue_wr_ptr)
    {
        TempActionItem &item =
            temp_action_queue[queue_rd_ptr];
        if(!item.completed)
        {
            break;
        }

        // 清理 instance
        if(item.instance != nullptr)
        {
            item.instance->runtime = nullptr;
            item.instance->started = false;
            item.instance->result = ACTION_IDLE;
        }

        item.desc = nullptr;
        item.instance = nullptr;
        item.param_count = 0;
        item.callback = nullptr;
        item.timeout_ms = 0;
        item.start_time = 0;
        item.instance_id = 0;
        item.result = ACTION_IDLE;
        item.completed = false;

        queue_rd_ptr =
            (queue_rd_ptr + 1) % MAX_TEMP_ACTIONS;
    }
}
// =====================================================
// 获取等待中的临时 Action 数量
// =====================================================
uint8_t workflow_temp_action_pending_count()
{
    if (queue_wr_ptr >= queue_rd_ptr)
    {
        return queue_wr_ptr - queue_rd_ptr;
    }
    else
    {
        return MAX_TEMP_ACTIONS - queue_rd_ptr + queue_wr_ptr;
    }
}
// =====================================================
// Timer 外部触发检查（由 loop 调用）
// 模式：独立于 workflow_task，未来 Event 同样方式
// =====================================================
void workflow_timer_check()
{
    static unsigned long last_timer_check = 0;
    unsigned long now = millis();

    if (now - last_timer_check < 5000) {
        return;
    }
    last_timer_check = now;

    for (uint8_t i = 0; i < workflow_count; i++) {
        Workflow &wf = workflows[i];
        if (!wf.enable) continue;
        if (wf.state != WORKFLOW_IDLE) continue;
        if (wf.step_count == 0) continue;

        WorkflowStep &step = wf.steps[0];
        if (step.instance_type != INSTANCE_TRIGGER) continue;

        // 只定义一次
        WorkflowTriggerInstance *trigger = step.instance.trigger;
        if (trigger == nullptr) {
            wf.state = WORKFLOW_ERROR;
            continue;
        }
        if (trigger->descriptor == nullptr) continue;

        // 只检查 Timer Trigger
        if (strcmp(trigger->descriptor->id, "timer") != 0) continue;

        // ===== 调用前调试 =====
        Serial.println("[Timer] Calling workflow_trigger_check");
        WorkflowTriggerState result = workflow_trigger_check(trigger);
        // ===== 调用后调试 =====
        Serial.printf("[Timer] result=%d\n", result);

        if (result == TRIGGER_SUCCESS) {
            workflow_start(&wf, true);
        } else if (result == TRIGGER_FAILED) {
            wf.state = WORKFLOW_ERROR;
        }
    }
}


// =====================================================
// Workflow执行核心（非阻塞，loop调用）
// =====================================================
void workflow_task()
{
    unsigned long now = millis();

    // =============================================
    // 只执行 RUNNING 状态的 Workflow
    // 不再处理任何 Trigger 触发逻辑
    // =============================================
    for (uint8_t i = 0; i < workflow_count; i++) {
        Workflow &wf = workflows[i];
        if (!wf.enable) continue;
        if (wf.state != WORKFLOW_RUNNING) continue;

        if ((uint32_t)(now - wf.start_time) > wf.timeout_ms) {
            wf.state = WORKFLOW_TIMEOUT;
            continue;
        }

        if (wf.current_step >= wf.step_count) {
            wf.state = WORKFLOW_FINISHED;
            continue;
        }

        WorkflowStep &step = wf.steps[wf.current_step];
        if (step.instance.trigger == nullptr && step.instance.action == nullptr) {
            wf.state = WORKFLOW_ERROR;
            continue;
        }
        else if (step.instance_type == INSTANCE_ACTION)
        {
            WorkflowActionInstance *action = step.instance.action;

            if (action == nullptr)
            {
                wf.state = WORKFLOW_ERROR;
                continue;
            }

            WorkflowActionResult result = workflow_action_execute(action);


            if (result == ACTION_SUCCESS) {
                wf.current_step++;
            }
            else if (result == ACTION_FAILED)
            {
                wf.state = WORKFLOW_ERROR;
            }
            else if(result == ACTION_RUNNING)
            {
                // 保持当前step
                // 下一轮继续调用handler
            }
        }
    }

    // =====================================================
    // 驱动临时 Action 环形队列（无数组拷贝）
    // =====================================================
    while (queue_rd_ptr != queue_wr_ptr)
    {
        TempActionItem &item = temp_action_queue[queue_rd_ptr];

        // 超时判断
        if ((unsigned long)(millis() - item.start_time) > item.timeout_ms)
        {
            Serial.printf("[Workflow] Temp action %s timeout after %lu ms\n", item.desc->id, item.timeout_ms);
            item.completed = true;
            item.result = ACTION_FAILED;

            if (item.callback != nullptr)
            {
                item.callback(
                    item.instance_id,
                    ACTION_FAILED
                );
            }

        }
        else
        {
            WorkflowActionResult result = workflow_action_execute(item.instance);
            if (result != ACTION_RUNNING)
            {
                item.completed = true;
                item.result = result;


                if(item.callback != nullptr)
                {
                    item.callback(
                        item.instance_id,
                        result
                    );
                }


                // ===============================
                // 生命周期回收
                // ===============================
            }
        }

        // 任务完成，释放实例、清空本条item，移动读指针
        if (item.completed)
        {
            temp_action_free_instance(item.instance);
            item.instance = nullptr;

            // 清空本条item，避免脏数据
            item.instance_id = 0;
            item.desc = nullptr;
            item.callback = nullptr;
            item.param_count = 0;
            item.completed = false;
            item.result = ACTION_IDLE;
            item.start_time = 0;
            item.timeout_ms = 0;
            memset(item.params,0,sizeof(item.params));

            queue_rd_ptr = (queue_rd_ptr + 1) % MAX_TEMP_ACTIONS;
        }
        else
        {
            // 当前任务还在运行，跳出，下一轮loop继续轮询
            break;
        }
    }
        // =====================================================
    // 清理已经完成的 Temp Action
    // =====================================================
    workflow_temp_action_cleanup();
}

// =====================================================
// Workflow导出JSON
// =====================================================
bool workflow_export_json(String &json)
{
    JsonDocument doc;
    JsonArray arr = doc["workflows"].to<JsonArray>();

    for (uint8_t i = 0; i < workflow_count; i++) {
        Workflow &wf = workflows[i];
        JsonObject obj = arr.add<JsonObject>();

        obj["id"] = wf.id;
        obj["name"] = wf.name;
        obj["enable"] = wf.enable;
        obj["timeout_ms"] = wf.timeout_ms;

        JsonArray steps = obj["steps"].to<JsonArray>();
        for (uint8_t j = 0; j < wf.step_count; j++) {
            WorkflowStep &s = wf.steps[j];
            JsonObject step = steps.add<JsonObject>();

            if (s.type == WORKFLOW_STEP_TRIGGER) {
                WorkflowTriggerInstance *t = s.instance.trigger;

                if(t == nullptr) {
                    step["type"] = "trigger";
                    step["id"] = "invalid";
                    continue;
                }

                step["type"] = "trigger";
                step["id"] = t->id;

                if (t->param_count > 0) {
                    JsonObject params = step["params"].to<JsonObject>();
                    for (uint8_t k = 0; k < t->param_count; k++) {
                        WorkflowParamValue &pv = t->params[k];
                        switch (pv.type) {
                            case PARAM_INT:
                                params[pv.name] = pv.int_value;
                                break;
                            case PARAM_FLOAT:
                                params[pv.name] = pv.float_value;
                                break;
                            case PARAM_BOOL:
                                params[pv.name] = pv.bool_value;
                                break;
                            case PARAM_STRING:
                                params[pv.name] = pv.string_value;
                                break;
                        }
                    }
                }

            } else {
                WorkflowActionInstance *a = s.instance.action;

                if(a == nullptr) {
                    step["type"] = "action";
                    step["id"] = "invalid";
                    continue;
                }

                step["type"] = "action";
                step["id"] = a->id;

                if (a->param_count > 0) {
                    JsonObject params = step["params"].to<JsonObject>();
                    for (uint8_t k = 0; k < a->param_count; k++) {
                        WorkflowParamValue &pv = a->params[k];
                        switch (pv.type) {
                            case PARAM_INT:
                                params[pv.name] = pv.int_value;
                                break;
                            case PARAM_FLOAT:
                                params[pv.name] = pv.float_value;
                                break;
                            case PARAM_BOOL:
                                params[pv.name] = pv.bool_value;
                                break;
                            case PARAM_STRING:
                                params[pv.name] = pv.string_value;
                                break;
                        }
                    }
                }
            }
        }
    }

    json = "";
    ArduinoJson::serializeJson(doc, json);
    return true;
}

// =====================================================
// Workflow启用
// =====================================================
bool workflow_enable(const String &id)
{
    for(uint8_t i = 0; i < workflow_count; i++) {
        if(workflows[i].id == id) {
            workflows[i].enable = true;
            return true;
        }
    }
    return false;
}

// =====================================================
// Workflow禁用
// =====================================================
bool workflow_disable(const String &id)
{
    for(uint8_t i = 0; i < workflow_count; i++) {
        if(workflows[i].id == id) {
            workflows[i].enable = false;
            workflows[i].state = WORKFLOW_IDLE;
            return true;
        }
    }
    return false;
}

// =====================================================
// Workflow停止
// =====================================================
bool workflow_stop(const String &id)
{
    for(uint8_t i = 0; i < workflow_count; i++) {
        if(workflows[i].id == id) {
            workflows[i].state = WORKFLOW_IDLE;
            workflows[i].current_step = 0;
            workflows[i].start_time = 0;
            return true;
        }
    }
    return false;
}


// =====================================================
// Event响应相关代码
// =====================================================

// =====================================================
// 向 Event Manager 订阅所有事件
// =====================================================
void workflow_event_init()
{
    for(uint8_t i = 0; i < workflow_count; i++)
    {
        Workflow &wf = workflows[i];
        if(!wf.enable)
            continue;
        if(wf.step_count == 0)
            continue;
        WorkflowStep &step = wf.steps[0];
        if(step.instance_type != INSTANCE_TRIGGER)
            continue;
        WorkflowTriggerInstance *trigger =
            step.instance.trigger;
        if(trigger == nullptr)
            continue;
        if(!trigger->id.startsWith("event_"))
            continue;
        SystemEvent event =
            event_from_string(trigger->id);
        if(event != EVENT_NONE)
        {
            event_subscribe(event, workflow_event_callback);
        }
    }
}

// =====================================================
// Event 回调（由 Event Manager 调用）
// =====================================================
void workflow_event_callback(const EventMessage &msg)
{
    SystemEvent event = msg.event;

    for (uint8_t i = 0; i < workflow_count; i++) {
        Workflow &wf = workflows[i];
        if (!wf.enable) continue;
        if (wf.step_count == 0) continue;
        if (wf.state != WORKFLOW_IDLE) continue;

        WorkflowStep &step = wf.steps[0];
        if (step.type != WORKFLOW_STEP_TRIGGER) continue;
        if (step.instance.trigger == nullptr && step.instance.action == nullptr) continue;

        WorkflowTriggerInstance *trigger = step.instance.trigger;
        if (!trigger->id.startsWith("event_")) continue;

        // 将字符串转换为事件枚举，与接收到的事件对比
        SystemEvent trigger_event = event_from_string(trigger->id);
        if (trigger_event == event) {
            workflow_start(&wf, true);
        }
    }
}

bool workflow_start_by_id(const String &id, bool skip_first_step)
{
    for (uint8_t i = 0; i < workflow_count; i++) {
        if (workflows[i].id == id) {
            return workflow_start(&workflows[i], skip_first_step);
        }
    }
    return false;
}


