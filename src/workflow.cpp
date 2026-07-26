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
            if(trigger_instances[i].runtime != nullptr)
            {
                free(trigger_instances[i].runtime);
                trigger_instances[i].runtime = nullptr;
            }

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
            if(action_instances[i].runtime != nullptr)
            {
                free(action_instances[i].runtime);
                action_instances[i].runtime = nullptr;
            }

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
    TimerRuntime *rt = (TimerRuntime*)trigger->runtime;
    if (rt == nullptr) return 0;

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
    TimerRuntime *rt = (TimerRuntime*)trigger->runtime;

    // =============================================
    // 第二步：初始化（第一次调用时）
    // =============================================
    if (rt == nullptr) {
        rt = (TimerRuntime*)calloc(1, sizeof(TimerRuntime));
        if (rt == nullptr) return TRIGGER_FAILED;
        trigger->runtime = rt;

        // 解析类型
        String type_str = timer_get_string_param(trigger->params, trigger->param_count, "type");
        if (type_str == "once") rt->type = 0;
        else if (type_str == "daily") rt->type = 1;
        else if (type_str == "weekly") rt->type = 2;
        else rt->type = 1;  // 默认 daily

        // 计算目标时间
        rt->next_trigger_time = timer_calculate_next(trigger);
        rt->last_trigger_minute = -1;
        rt->triggered = false;
        rt->expired = false;

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
    if (diff >= -6 && diff < 6) {
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
    DelayRuntime *rt = (DelayRuntime*)trigger->runtime;

    if (rt == nullptr) {
        rt = (DelayRuntime*)calloc(1, sizeof(DelayRuntime));
        if (rt == nullptr) return TRIGGER_FAILED;
        trigger->runtime = rt;

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
        delay_params,
        1,
        delay_handler
    };

    workflow_register_trigger(&timer_desc);
    workflow_register_trigger(&delay_desc);
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
            workflows[i].steps[j].instance = nullptr;
            workflows[i].steps[j].type = WORKFLOW_STEP_ACTION;
            workflows[i].steps[j].instance_type = INSTANCE_ACTION;
        }
    }

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

                WorkflowTriggerInstance *inst = &trigger_instances[trigger_instance_index++];
                inst->id = "";
                inst->param_count = 0;
                inst->state = TRIGGER_IDLE;
                inst->runtime = nullptr;
                inst->descriptor = nullptr;

                inst->id = s.id;
                inst->descriptor = find_trigger_descriptor(s.id);

                if(inst->descriptor == nullptr) {
                    s.instance = nullptr;
                    step_index++;
                    continue;
                }

                JsonObject params = step["params"];
                workflow_parse_params(params, inst->params, inst->param_count);
                inst->state = TRIGGER_IDLE;
                s.instance = inst;

            } else if(type == "action") {
                s.type = WORKFLOW_STEP_ACTION;
                s.instance_type = INSTANCE_ACTION;

                if(action_instance_index >= WORKFLOW_MAX_COUNT * WORKFLOW_MAX_STEP) {
                    json_state = WORKFLOW_JSON_ERROR;
                    return false;
                }

                WorkflowActionInstance *inst = &action_instances[action_instance_index++];
                
                inst->id = s.id;
                inst->descriptor = find_action_descriptor(s.id);
                inst->id = "";
                inst->param_count = 0;
                inst->result = ACTION_IDLE;
                inst->runtime = nullptr;
                inst->descriptor = nullptr;

                if(inst->descriptor == nullptr) {
                    s.instance = nullptr;
                    step_index++;
                    continue;
                }

                JsonObject params = step["params"];
                workflow_parse_params(params, inst->params, inst->param_count);
                inst->result = ACTION_IDLE;
                s.instance = inst;
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

    if (!LittleFS.begin(true, "/littlefs", 10, "littlefs")) {
        Serial.println("LittleFS mount failed");
        return false;
    }

    File file = LittleFS.open(path, "r");
    if (!file) {
        Serial.printf("Failed to open %s\n", path);
        LittleFS.end();
        return false;
    }

    String json;
    while (file.available()) {
        json += (char)file.read();
    }
    file.close();
    LittleFS.end();

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

    if (!LittleFS.begin(true, "/littlefs", 10, "littlefs")) {
        Serial.println("LittleFS mount failed");
        return false;
    }

    File file = LittleFS.open(path, "w");
    if (!file) {
        Serial.printf("Failed to open %s for writing\n", path);
        LittleFS.end();
        return false;
    }

    size_t written = file.print(json);
    file.close();
    LittleFS.end();

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
    if(workflow_json_cache.length() == 0) return false;
    return workflow_load_json(workflow_json_cache);
}

// =====================================================
// Workflow启动
// =====================================================
bool workflow_start(Workflow *workflow, bool skip_first_step)
{
    if (workflow == nullptr) return false;
    if (!workflow->enable) return false;
    if (workflow->state == WORKFLOW_RUNNING) return false;

    // 检查 Step0 是否为 Trigger（防御）
    if (workflow->step_count > 0) {
        WorkflowStep &first = workflow->steps[0];
        if (first.type != WORKFLOW_STEP_TRIGGER) {
            return false;
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
    if(action == nullptr) return ACTION_FAILED;
    if(action->descriptor == nullptr) return ACTION_FAILED;
    if(action->descriptor->handler == nullptr) return ACTION_FAILED;

    action->result = action->descriptor->handler(action);
    return action->result;
}

// =====================================================
// Timer 外部触发检查（由 loop 调用）
// 模式：独立于 workflow_task，未来 Event 同样方式
// =====================================================
void workflow_timer_check()
{
    static unsigned long last_timer_check = 0;
    unsigned long now = millis();

    // 调试：确认函数被调用
    Serial.println("[Timer] workflow_timer_check() called");

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
        if (step.instance == nullptr) continue;

        WorkflowTriggerInstance *trigger = (WorkflowTriggerInstance*)step.instance;
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
        if (step.instance == nullptr) {
            wf.state = WORKFLOW_ERROR;
            continue;
        }

        if (step.instance_type == INSTANCE_TRIGGER) {
            WorkflowTriggerInstance *trigger = (WorkflowTriggerInstance*)step.instance;
            if (trigger == nullptr) {
                wf.state = WORKFLOW_ERROR;
                continue;
            }

            WorkflowTriggerState result = workflow_trigger_check(trigger);

            if (result == TRIGGER_SUCCESS) {
                wf.current_step++;
            } else if (result == TRIGGER_FAILED) {
                wf.state = WORKFLOW_ERROR;
            }
        }
        else if (step.instance_type == INSTANCE_ACTION) {
            WorkflowActionInstance *action = (WorkflowActionInstance*)step.instance;
            if (action == nullptr) {
                wf.state = WORKFLOW_ERROR;
                continue;
            }

            WorkflowActionResult result = workflow_action_execute(action);

            if (result == ACTION_SUCCESS) {
                wf.current_step++;
            } else if (result == ACTION_FAILED) {
                wf.state = WORKFLOW_ERROR;
            }
        }
    }
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
                WorkflowTriggerInstance *t = (WorkflowTriggerInstance*)s.instance;

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
                WorkflowActionInstance *a = (WorkflowActionInstance*)s.instance;

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