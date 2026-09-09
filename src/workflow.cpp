#include <Arduino.h>
#include <ArduinoJson.h>
#include <esp_heap_caps.h>
#include <LittleFS.h>
#include <time.h>
#include <new>
#include "system_state.h"
#include "time_manager.h"
#include "system_command.h"
#include "workflow.h"
#include "workflow_storage.h"
// =====================================================
// 注册表
// =====================================================
#define WORKFLOW_MAX_TRIGGER 32
#define WORKFLOW_MAX_ACTION 32

static const WorkflowTriggerDescriptor* trigger_registry[WORKFLOW_MAX_TRIGGER];
static const WorkflowActionDescriptor* action_registry[WORKFLOW_MAX_ACTION];
static uint8_t trigger_count = 0;
static uint8_t action_count = 0;

struct WorkflowTriggerInstance;
struct WorkflowActionInstance;

struct WorkflowTriggerDescriptor;
struct WorkflowActionDescriptor;

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
// Step Definition 池（优先 PSRAM）
//
// Definition 与 Runtime 分离后，"下一次执行什么"单独存放，
// 不进入 WorkflowStep，避免常驻 DRAM 的 workflows[] 膨胀。
static WorkflowStepDef* step_definitions = nullptr;
static bool step_definitions_psram = false;
static bool trigger_instances_psram = false;
static bool action_instances_psram = false;
static uint16_t trigger_instance_index = 0;
static uint16_t action_instance_index = 0;

//前置声明
static void workflow_trigger_callback(
    WorkflowTriggerInstance *trigger,
    WorkflowTriggerState state
);


static void workflow_action_callback(
    WorkflowActionInstance *action,
    WorkflowActionResult result
);

// =====================================================
// Critical Operation 保护
//
// 依据: critical_operation接入规范.md §3 / §5
//
// 判定: Workflow 与临时 Action 执行期间会驱动阀门 / 电机等外部设备，
//       并可能写入持久化数据；中途重启会让硬件与 Flash 数据处于
//       不可预期状态，因此属于 Critical Operation。
//
// 计数模型: 每个执行体（一个 Workflow / 一个临时 Action）各 acquire 一次，
//       结束时各 release 一次。因此并发运行的多个 Workflow 与多个
//       临时 Action 会各自计数，只有全部结束 critical count 才会归零，
//       系统才允许进入重启安全窗口。
//
// 持有标记: 用与 workflows[] 下标一一对应的静态数组记录，避免修改
//       workflow.h 的 Workflow 结构（本次改动限定在 workflow.cpp）。
//       标记的作用有三个:
//         1) release 幂等 —— 未持有就不调用 release，避免
//            system_command 打印 "release underflow" 配对错误日志；
//         2) 覆盖"无 finish_callback 的 Workflow"（事件 / 定时器触发），
//            这类 Workflow 同样持有 Critical Operation，必须能被释放；
//         3) 覆盖 workflow_stop() / workflow_disable() / workflow_clear()
//            等强制终止路径，防止计数泄漏导致系统永久无法重启。
// =====================================================
static bool workflow_critical_held[WORKFLOW_MAX_COUNT];

// 由 Workflow 指针反查其在 workflows[] 中的下标，越界返回 -1
static int workflow_index_of(const Workflow *wf)
{
    if(wf == nullptr)
    {
        return -1;
    }

    long idx = (long)(wf - workflows);

    if(idx < 0 || idx >= (long)WORKFLOW_MAX_COUNT)
    {
        return -1;
    }

    return (int)idx;
}

// 释放指定 Workflow 持有的 Critical Operation（幂等）
static void workflow_critical_release_by_index(int idx)
{
    if(idx < 0 || idx >= (int)WORKFLOW_MAX_COUNT)
    {
        return;
    }

    if(!workflow_critical_held[idx])
    {
        return;
    }

    workflow_critical_held[idx] = false;

    system_command_critical_operation_release();

    Serial.printf(
        "[Workflow] critical op released (workflow idx=%d, count=%u)\n",
        idx,
        (unsigned)system_command_critical_operation_count()
    );
}

// =====================================================
// 临时 Action 任务队列（用于 Command 调用）
// =====================================================
#define MAX_TEMP_ACTIONS 8 //实际可用7个位置
#define MAX_TEMP_ACTION_INST MAX_TEMP_ACTIONS

struct TempActionItem {

    uint32_t instance_id;
    const WorkflowActionDescriptor *desc;
    bool completed;
    WorkflowActionResult result;
    unsigned long start_time;
    WorkflowActionInstance *instance;
    String cmd_id;   // CommandManager 关联 ID（仅保存关联，不理解命令业务）
    CommandTempActionCallback callback;   // CommandManager 契约回调
    unsigned long timeout_ms;
    // 本 Item 是否已 acquire 了 Critical Operation（acquire/release 配对标记）
    bool critical_held;
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
// 释放临时 Action 持有的 Critical Operation（幂等）
//
// 只有此前 acquire() 成功的 Item 才会真正 release，
// 避免触发 system_command 的 "release underflow" 配对错误日志。
// =====================================================
static void temp_action_critical_release(TempActionItem &item)
{
    if(!item.critical_held)
    {
        return;
    }

    item.critical_held = false;

    system_command_critical_operation_release();

    Serial.printf(
        "[Workflow] critical op released (temp action id=%u, count=%u)\n",
        (unsigned)item.instance_id,
        (unsigned)system_command_critical_operation_count()
    );
}

// =====================================================
// 临时 Action 结束统一收口
//
// 所有结束路径（SUCCESS / FAILED / 等待超时）都必须走这里:
//   1) 标记完成 + 写入最终 result
//   2) 释放本 Item 持有的 Critical Operation（无论成败与超时）
//   3) 回调 CommandManager 上报结果
//
// 集中收口是为了杜绝"某条分支忘记 release"导致的
// critical count 永不归零、系统永久无法重启。
// =====================================================
static void temp_action_complete(
    TempActionItem &item,
    WorkflowActionResult result
)
{
    item.completed = true;
    item.result = result;

    temp_action_critical_release(item);

    if(item.callback != nullptr)
    {
        item.callback(
            item.cmd_id,
            item.instance_id,
            item.result
        );
    }
}

// =====================================================
// 内部查找函数
// =====================================================
static const WorkflowTriggerDescriptor* find_trigger_descriptor(const String &id)
{
    for(uint8_t i = 0; i < trigger_count; i++){
        if(trigger_registry[i] == nullptr)
            continue;
        if(id == trigger_registry[i]->id)
        {
            return trigger_registry[i];
        }
    }
    return nullptr;
}

static const WorkflowActionDescriptor* find_action_descriptor(const String &id)
{
    for(uint8_t i = 0; i < action_count; i++){
        if(action_registry[i] == nullptr)
            continue;
        if(id == action_registry[i]->id)
        {
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

    // ---- Step Definition 池 ----
    if(step_definitions != nullptr)
    {
        for(uint16_t i = 0; i < count; i++)
        {
            step_definitions[i].~WorkflowStepDef();
        }

        if(step_definitions_psram)
        {
            heap_caps_free(step_definitions);
        }
        else
        {
            free(step_definitions);
        }

        step_definitions = nullptr;
        step_definitions_psram = false;
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
    TimerRuntime *rt =
        &trigger->timer_runtime;
    time_t now =
        time_get();
    if(now == 0)
    {
        return 0;
    }
    struct tm tm_now;
    localtime_r(
        &now,
        &tm_now
    );
    int hour =
        timer_get_int_param(
            trigger->params,
            trigger->param_count,
            "hour",
            8
        );
    int minute =
        timer_get_int_param(
            trigger->params,
            trigger->param_count,
            "minute",
            0
        );
    struct tm target =
        tm_now;
    target.tm_hour = hour;
    target.tm_min = minute;
    target.tm_sec = 0;
    if(rt->type == 0)
    {
        int year =
            timer_get_int_param(
                trigger->params,
                trigger->param_count,
                "year",
                2026
            );
        int month =
            timer_get_int_param(
                trigger->params,
                trigger->param_count,
                "month",
                1
            );
        int day =
            timer_get_int_param(
                trigger->params,
                trigger->param_count,
                "day",
                1
            );
        target.tm_year =
            year - 1900;
        target.tm_mon =
            month - 1;
        target.tm_mday =
            day;
        time_t result =
            mktime(&target);
        // once已经过期
        if(result <= now)
        {
            return 0;
        }
        return result;
    }
    else if(rt->type == 1)
    {
        time_t result =
            mktime(&target);
        if(result <= now)
        {
            result += 86400;
        }
        return result;
    }
    else if(rt->type == 2)
    {
        int target_wday =
            timer_get_int_param(
                trigger->params,
                trigger->param_count,
                "day_of_week",
                1
            );
        int days_ahead =
            target_wday -
            tm_now.tm_wday;
        if(days_ahead <=0)
        {
            days_ahead +=7;
        }
        target.tm_mday +=
            days_ahead;
        return mktime(&target);
    }
    return 0;
}

//timer 的初始化
static void timer_reset(
    WorkflowTriggerInstance *trigger
)
{
    if(trigger == nullptr)
        return;


    trigger->state = TRIGGER_IDLE;


    TimerRuntime &rt =
        trigger->timer_runtime;


    rt.next_trigger_time = 0;
    rt.last_trigger_minute = -1;
    rt.triggered = false;
    rt.expired = false;
    rt.type = 0;


    trigger->running = false;
}



//第一次进入workflow step时候调用
static void timer_start(WorkflowTriggerInstance *trigger)
{
    if(trigger == nullptr)
        return;
    TimerRuntime &rt =
        trigger->timer_runtime;
    rt.next_trigger_time = 0;
    rt.last_trigger_minute = -1;
    rt.triggered = false;
    rt.expired = false;
    rt.type = 1;
    String type_str =
        timer_get_string_param(
            trigger->params,
            trigger->param_count,
            "type"
        );
    if(type_str == "once")
        rt.type = 0;
    else if(type_str == "daily")
        rt.type = 1;
    else if(type_str == "weekly")
        rt.type = 2;
    rt.next_trigger_time =
        timer_calculate_next(trigger);
    if(rt.next_trigger_time == 0)
    {
        trigger->state =
            TRIGGER_FAILED;
        return;
    }
    trigger->state =
        TRIGGER_RUNNING;
}


//每次 task 调用，timer runtime
static void timer_poll(WorkflowTriggerInstance *trigger)
{
    if(trigger == nullptr)
        return;
    TimerRuntime &rt =
        trigger->timer_runtime;

    time_t now =
    time_get();

    if(now == 0)
    {
        Serial.println(
            "[Workflow Timer] time invalid"
        );

        trigger->state =
            TRIGGER_FAILED;

        return;
    }

    if(rt.expired)
    {
        trigger->state =
            TRIGGER_FAILED;
        return;
    }

    int current_minute =
        now / 60;
    if(current_minute !=
       rt.last_trigger_minute)
    {
        rt.triggered = false;
        rt.last_trigger_minute =
            current_minute;
    }
    if(rt.triggered)
    {
        trigger->state =
            TRIGGER_RUNNING;

        return;
    }
    time_t diff =
        now - rt.next_trigger_time;
    if(diff >= -6 && diff <= 6)
    {
        rt.triggered = true;
        
        if(rt.type != 0)
        {
            rt.next_trigger_time =
                timer_calculate_next(trigger);
        }
        else
        {
            rt.expired = true;
        }

        trigger->state =
            TRIGGER_SUCCESS;

        return;
    }
    trigger->state =
        TRIGGER_RUNNING;
}


//===================================
//delay处理核心函数
//===================================

//delay 初始化
static void delay_reset(
    WorkflowTriggerInstance *trigger
)
{
    if(trigger == nullptr)
        return;


    trigger->state =
        TRIGGER_IDLE;


    DelayRuntime &rt =
        trigger->delay_runtime;


    rt.start_time = 0;
    rt.delay_ms = 0;
    rt.started = false;


    trigger->running = false;
}

//delay 第一次在workflow step调用
static void delay_start(
    WorkflowTriggerInstance *trigger
)
{
    if(trigger == nullptr)
        return;


    DelayRuntime &rt =
        trigger->delay_runtime;


    int seconds =
        timer_get_int_param(
            trigger->params,
            trigger->param_count,
            "seconds",
            1
        );


    if(seconds < 1)
        seconds = 1;


    rt.delay_ms =
        (unsigned long)seconds *
        1000UL;


    rt.start_time =
        millis();


    rt.started = true;


    trigger->state =
        TRIGGER_RUNNING;
}

//delay 的runtime
static void delay_poll(
    WorkflowTriggerInstance *trigger
)
{
    if(trigger == nullptr)
        return;


    DelayRuntime &rt =
        trigger->delay_runtime;



    if(!rt.started)
    {
        trigger->state =
            TRIGGER_FAILED;

        return;
    }



    if(
        millis() -
        rt.start_time
        >=
        rt.delay_ms
    )
    {
        trigger->state =
            TRIGGER_SUCCESS;
        return;
    }


    trigger->state =
        TRIGGER_RUNNING;
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

    // 仅重置本模块内部的"持有 Critical Operation"标记。
    //
    // 注意：workflow_init() 在 setup() 中早于 system_command_init()
    // （后者由 command_manager_init() 调用），此刻不可能有进行中的
    // 操作，因此这里【不】调用 system_command_critical_operation_release()
    // —— 避免在自旋锁初始化之前触碰 system_command 内部状态。
    memset(
        workflow_critical_held,
        0,
        sizeof(workflow_critical_held)
    );

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

    // ---- Step Definition 池：优先 PSRAM，失败回退普通 RAM ----
    size_t def_size = instance_count * sizeof(WorkflowStepDef);
    step_definitions = (WorkflowStepDef*)heap_caps_malloc(def_size, MALLOC_CAP_SPIRAM);
    if(step_definitions != nullptr)
    {
        step_definitions_psram = true;
    }
    else
    {
        step_definitions = (WorkflowStepDef*)malloc(def_size);
        step_definitions_psram = false;
    }

    if(step_definitions == nullptr)
    {
        Serial.println("[Workflow] Step definition pool allocate fail");
        return false;
    }

    for(uint16_t i = 0; i < instance_count; i++)
    {
        new (&trigger_instances[i]) WorkflowTriggerInstance();
        new (&action_instances[i]) WorkflowActionInstance();
        new (&step_definitions[i]) WorkflowStepDef();
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

static WorkflowTriggerDescriptor timer_desc =
{
            "timer",
            "Timer",
            "system",
            "系统定时触发器",
            timer_params,
            7,

            timer_reset,
            timer_start,
            timer_poll
};

// ===== 新增：注册内置 Delay Trigger =====
static WorkflowParam delay_params[] = {
        {"seconds", PARAM_INT, "", "1-3600"}
};

static WorkflowTriggerDescriptor delay_desc =
{
            "delay",
            "Delay",
            "system",
            "延时触发器",
            delay_params,
            1,

            delay_reset,
            delay_start,
            delay_poll
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
bool workflow_register_trigger(
    const WorkflowTriggerDescriptor *trigger
)
{
    if(trigger == nullptr)
        return false;

    if(trigger->id == nullptr)
        return false;

    if(trigger_count >= WORKFLOW_MAX_TRIGGER)
        return false;


    for(uint8_t i = 0; i < trigger_count; i++)
    {
        if(trigger_registry[i] == nullptr)
            continue;

        if(strcmp(
            trigger_registry[i]->id,
            trigger->id
        ) == 0)
        {
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
bool workflow_register_action(
    const WorkflowActionDescriptor *action
)
{
    if(action == nullptr)
        return false;

    if(action->id == nullptr)
        return false;

    if(action_count >= WORKFLOW_MAX_ACTION)
        return false;


    for(uint8_t i = 0; i < action_count; i++)
    {
        if(action_registry[i] == nullptr)
            continue;

        if(strcmp(
            action_registry[i]->id,
            action->id
        ) == 0)
        {
            return false;
        }
    }


    action_registry[action_count] = action;
    action_count++;

    return true;
}

//=====================================================
//action的核心处理函数
//====================================================

//action第一次在workflow step 中调用
void workflow_action_start(
    WorkflowActionInstance *action
)
{
    if(action == nullptr)
        return;

    if(action->descriptor == nullptr ||
       action->descriptor->start == nullptr)
    {
        action->result = ACTION_FAILED;
        return;
    }

    action->running = true;

    action->descriptor->start(action);

    if(action->result == ACTION_IDLE)
    {
        action->result = ACTION_RUNNING;
    }
}

//action的runtime
void workflow_action_poll(
    WorkflowActionInstance *action
)
{
    if(action == nullptr)
        return;

    if(action->descriptor == nullptr ||
       action->descriptor->poll == nullptr)
    {
        action->result = ACTION_FAILED;

        if(action->callback)
            action->callback(
                action,
                ACTION_FAILED
            );

        return;
    }
    action->descriptor->poll(action);
    if(action->result != ACTION_RUNNING)
    {
        action->running = false;
    }
}

//action的重新初始化
void workflow_action_reset(
    WorkflowActionInstance *action
)
{
    if(action==nullptr)
        return;
    action->result =
        ACTION_IDLE;
    action->running =
        false;
    action->runtime =
        nullptr;
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

const WorkflowActionDescriptor* workflow_find_action(const String &id)
{
    return find_action_descriptor(id);
}

const WorkflowTriggerDescriptor* workflow_get_trigger_descriptor(uint8_t index)
{
    if(index >= trigger_count)
        return nullptr;
    return trigger_registry[index];
}

const WorkflowActionDescriptor* workflow_get_action_descriptor(uint8_t index)
{
    if(index >= action_count)
        return nullptr;
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
// =====================================================
// Step Definition 访问与快照（Definition / Runtime 分离核心）
// =====================================================
//
// 铁律（需求文档 §10 / §43）：
//   Runtime 绝不允许长期引用 Definition 的 params，一律【值拷贝】。
//   禁止 runtime.params = definition.params; 这种写法。
// =====================================================

WorkflowStepDef *workflow_step_def_at(
    uint8_t workflow_index,
    uint8_t step_index
)
{
    if(step_definitions == nullptr)
        return nullptr;
    if(workflow_index >= WORKFLOW_MAX_COUNT ||
       step_index >= WORKFLOW_MAX_STEP)
        return nullptr;

    return &step_definitions[
        (uint16_t)workflow_index * WORKFLOW_MAX_STEP + step_index
    ];
}

static void workflow_clear_step_def(
    uint8_t workflow_index,
    uint8_t step_index
)
{
    WorkflowStepDef *def =
        workflow_step_def_at(workflow_index, step_index);
    if(def == nullptr)
        return;

    def->type = WORKFLOW_STEP_ACTION;
    def->instance_type = INSTANCE_ACTION;
    def->id = "";
    def->param_count = 0;

    for(uint8_t i = 0; i < WORKFLOW_MAX_PARAM; i++)
    {
        def->params[i].name = "";
        def->params[i].type = PARAM_INT;
        def->params[i].int_value = 0;
        def->params[i].float_value = 0.0f;
        def->params[i].bool_value = false;
        def->params[i].string_value = "";
    }
}

// 解析 / CRUD 之后：把 Step 当前的可持久化数据写入 Definition
static void workflow_capture_step_def(
    uint8_t workflow_index,
    uint8_t step_index,
    const WorkflowStep &step
)
{
    WorkflowStepDef *def =
        workflow_step_def_at(workflow_index, step_index);
    if(def == nullptr)
        return;

    def->type = step.type;
    def->instance_type = step.instance_type;
    def->id = step.id;
    def->param_count = 0;

    const WorkflowParamValue *src = nullptr;
    uint8_t count = 0;

    if(step.instance_type == INSTANCE_TRIGGER &&
       step.instance.trigger != nullptr)
    {
        src = step.instance.trigger->params;
        count = step.instance.trigger->param_count;
    }
    else if(step.instance_type == INSTANCE_ACTION &&
            step.instance.action != nullptr)
    {
        src = step.instance.action->params;
        count = step.instance.action->param_count;
    }
    else
    {
        // Descriptor 不存在等异常：Definition 只保留 ID，参数为空
        return;
    }

    if(count > WORKFLOW_MAX_PARAM)
        count = WORKFLOW_MAX_PARAM;

    def->param_count = count;
    for(uint8_t i = 0; i < count; i++)
    {
        def->params[i] = src[i];
    }
}

// workflow_start() 时调用：Definition → Runtime 参数快照（值拷贝）
//
// 快照生成后，运行期间对 Definition 的任何修改都不会影响本次运行。
static void workflow_snapshot_step_def(
    int workflow_index,
    uint8_t step_index,
    WorkflowStep &step
)
{
    if(workflow_index < 0)
        return;

    const WorkflowStepDef *def = workflow_step_def_at(
        (uint8_t)workflow_index,
        step_index
    );
    if(def == nullptr)
        return;

    step.type = def->type;
    step.instance_type = def->instance_type;

    WorkflowParamValue *dst = nullptr;
    uint8_t *dst_count = nullptr;
    String *dst_id = nullptr;

    if(def->instance_type == INSTANCE_TRIGGER &&
       step.instance.trigger != nullptr)
    {
        dst = step.instance.trigger->params;
        dst_count = &step.instance.trigger->param_count;
        dst_id = &step.instance.trigger->id;
    }
    else if(def->instance_type == INSTANCE_ACTION &&
            step.instance.action != nullptr)
    {
        dst = step.instance.action->params;
        dst_count = &step.instance.action->param_count;
        dst_id = &step.instance.action->id;
    }
    else
    {
        return;
    }

    *dst_id = def->id;
    *dst_count = def->param_count;

    for(uint8_t i = 0; i < def->param_count; i++)
    {
        dst[i] = def->params[i];
    }
}

// =====================================================
// Dirty Bitmap（需求文档 §15）
// =====================================================
//
// 256 bit = 16 Workflow × 16 Step
//
//   slot = workflow_index * WORKFLOW_MAX_STEP + step_index
//   W00S00 -> bit 0    W00S15 -> bit 15
//   W01S00 -> bit 16   W15S15 -> bit 255
//
// 之所以用 uint32_t[8] 而不是 uint8_t[256]：
//   省 224 字节 RAM；且 has_any_dirty() 只需 8 次 32 位比较，
//   不必遍历 256 次 —— 满足 §47 性能原则。

#define WORKFLOW_DIRTY_WORDS \
    ((WORKFLOW_MAX_COUNT * WORKFLOW_MAX_STEP + 31) / 32)

// 延迟保存窗口：与 ConfigManager 已验证的 5 分钟窗口对齐（§22）
#define WORKFLOW_SAVE_DELAY_MS (5UL * 60UL * 1000UL)

static uint32_t dirty_bitmap[WORKFLOW_DIRTY_WORDS];

// Critical Operation 持有标记（幂等用，避免 release underflow）
static bool wf_dirty_critical_held = false;

// 0 = 无待保存；非 0 = 延迟保存窗口起点
static unsigned long workflow_save_since_ms = 0;

static inline void dirty_mark(uint8_t wf, uint8_t step)
{
    uint16_t slot = (uint16_t)wf * WORKFLOW_MAX_STEP + step;
    dirty_bitmap[slot >> 5] |= (1UL << (slot & 31));
}

static inline void dirty_clear(uint8_t wf, uint8_t step)
{
    uint16_t slot = (uint16_t)wf * WORKFLOW_MAX_STEP + step;
    dirty_bitmap[slot >> 5] &= ~(1UL << (slot & 31));
}

static inline bool dirty_is(uint8_t wf, uint8_t step)
{
    uint16_t slot = (uint16_t)wf * WORKFLOW_MAX_STEP + step;
    return ((dirty_bitmap[slot >> 5] >> (slot & 31)) & 1UL) != 0;
}

static bool dirty_any()
{
    for(uint8_t i = 0; i < WORKFLOW_DIRTY_WORDS; i++)
    {
        if(dirty_bitmap[i] != 0)
        {
            return true;
        }
    }
    return false;
}

static bool dirty_workflow_has(uint8_t wf)
{
    for(uint8_t s = 0; s < WORKFLOW_MAX_STEP; s++)
    {
        if(dirty_is(wf, s))
        {
            return true;
        }
    }
    return false;
}

static void dirty_workflow_clear(uint8_t wf)
{
    for(uint8_t s = 0; s < WORKFLOW_MAX_STEP; s++)
    {
        dirty_clear(wf, s);
    }
}

// =====================================================
// Definition(RAM,String) <-> WorkflowStorage(char[]) 转换
// =====================================================
//
// 只转换可持久化数据：type / instance_type / ID / 参数。
// 绝不写入：函数指针 / Descriptor 指针 / runtime / callback / result。

static void workflow_def_to_storage(
    const WorkflowStepDef *src,
    WorkflowStepDefinition *dst
)
{
    dst->type = (uint8_t)src->type;
    dst->instance_type = (uint8_t)src->instance_type;

    strncpy(dst->id, src->id.c_str(), WF_STG_ID_MAX_LEN - 1);
    dst->id[WF_STG_ID_MAX_LEN - 1] = '\0';

    dst->param_count = src->param_count;

    for(uint8_t i = 0; i < src->param_count; i++)
    {
        WorkflowParamValueDefinition *d = &dst->params[i];
        const WorkflowParamValue &s = src->params[i];

        strncpy(d->name, s.name.c_str(), WF_STG_PARAM_NAME_LEN - 1);
        d->name[WF_STG_PARAM_NAME_LEN - 1] = '\0';

        d->type = (uint8_t)s.type;
        d->int_value = (int32_t)s.int_value;
        d->float_value = s.float_value;
        d->bool_value = s.bool_value ? 1 : 0;

        strncpy(d->string_value,
                s.string_value.c_str(),
                WF_STG_PARAM_STR_LEN - 1);
        d->string_value[WF_STG_PARAM_STR_LEN - 1] = '\0';
    }
}

// 由 RAM 状态构造完整 WorkflowDefinition
//
// 注意：保存粒度是【整个 Workflow】而不是单个 Step ——
// Meta Entry 的 crc32 覆盖该 Workflow 全部 Step payload，
// 若只写 Dirty Step，CRC 会因缺失数据而不一致。
static void workflow_build_definition(
    uint8_t wf,
    WorkflowDefinition *out
)
{
    memset(out, 0, sizeof(*out));

    strncpy(out->id, workflows[wf].id.c_str(), WF_STG_ID_MAX_LEN - 1);
    strncpy(out->name, workflows[wf].name.c_str(), WF_STG_NAME_MAX_LEN - 1);
    out->enable = workflows[wf].enable ? 1 : 0;
    out->timeout_ms = (uint32_t)workflows[wf].timeout_ms;
    out->step_count = workflows[wf].step_count;

    for(uint8_t s = 0; s < out->step_count; s++)
    {
        const WorkflowStepDef *def = workflow_step_def_at(wf, s);
        if(def == nullptr)
        {
            continue;
        }
        workflow_def_to_storage(def, &out->steps[s]);
    }
}

// =====================================================
// BIN → RAM 加载（需求文档 §8 / §31）
// =====================================================
//
// 与 JSON 解析路径平行：数据来源是 WorkflowStorage 而不是 JSON，
// 但构建 Runtime 的规则完全一致（Descriptor 由 ID 重新解析）。

static void workflow_storage_to_def(
    const WorkflowStepDefinition *src,
    WorkflowStepDef *dst
)
{
    dst->type = (WorkflowStepType)src->type;
    dst->instance_type = (WorkflowInstanceType)src->instance_type;
    dst->id = String(src->id);
    dst->param_count = src->param_count;

    for(uint8_t i = 0; i < src->param_count; i++)
    {
        const WorkflowParamValueDefinition *s = &src->params[i];
        WorkflowParamValue &d = dst->params[i];

        d.name = String(s->name);
        d.type = (WorkflowParamType)s->type;
        d.int_value = (int)s->int_value;
        d.float_value = s->float_value;
        d.bool_value = s->bool_value != 0;
        d.string_value = String(s->string_value);
    }
}

// 由 BIN Step Definition 构建单个 Step 的 Runtime
//
// 顺序（关键）：
//   1. 分配 Instance + 绑定 Descriptor（params 清空）
//   2. BIN → WorkflowStepDef（Definition）
//   3. Definition → Runtime 快照
//
// 第 3 步复用 workflow_snapshot_step_def()，保证 BIN 与 JSON 两条路径
// 最终落到 Runtime 的数据完全一致。
static void workflow_apply_step_definition(
    uint8_t wf,
    uint8_t step_index,
    const WorkflowStepDefinition *sd
)
{
    WorkflowStep &s = workflows[wf].steps[step_index];

    s.id = String(sd->id);
    s.type = (WorkflowStepType)sd->type;
    s.instance_type = (WorkflowInstanceType)sd->instance_type;
    s.instance.trigger = nullptr;
    s.instance.action = nullptr;

    uint16_t pool_max =
        (uint16_t)WORKFLOW_MAX_COUNT * WORKFLOW_MAX_STEP;

    if(s.instance_type == INSTANCE_TRIGGER)
    {
        const WorkflowTriggerDescriptor *desc =
            find_trigger_descriptor(s.id);

        if(desc != nullptr && trigger_instance_index < pool_max)
        {
            WorkflowTriggerInstance *inst =
                &trigger_instances[trigger_instance_index++];

            inst->id = s.id;
            inst->descriptor = desc;
            inst->param_count = 0;
            inst->state = TRIGGER_IDLE;
            inst->running = false;
            inst->callback = workflow_trigger_callback;

            inst->timer_runtime.next_trigger_time = 0;
            inst->timer_runtime.last_trigger_minute = -1;
            inst->timer_runtime.triggered = false;
            inst->timer_runtime.expired = false;
            inst->timer_runtime.type = 0;
            inst->delay_runtime.start_time = 0;
            inst->delay_runtime.delay_ms = 0;
            inst->delay_runtime.started = false;

            for(uint8_t i = 0; i < WORKFLOW_MAX_PARAM; i++)
            {
                inst->params[i].name = "";
                inst->params[i].type = PARAM_INT;
                inst->params[i].int_value = 0;
                inst->params[i].float_value = 0.0f;
                inst->params[i].bool_value = false;
                inst->params[i].string_value = "";
            }

            s.instance.trigger = inst;
        }
    }
    else
    {
        const WorkflowActionDescriptor *desc =
            find_action_descriptor(s.id);

        if(desc != nullptr && action_instance_index < pool_max)
        {
            WorkflowActionInstance *inst =
                &action_instances[action_instance_index++];

            inst->id = s.id;
            inst->descriptor = desc;
            inst->param_count = 0;
            inst->result = ACTION_IDLE;
            inst->running = false;
            inst->runtime = nullptr;
            inst->callback = workflow_action_callback;

            for(uint8_t i = 0; i < WORKFLOW_MAX_PARAM; i++)
            {
                inst->params[i].name = "";
                inst->params[i].type = PARAM_INT;
                inst->params[i].int_value = 0;
                inst->params[i].float_value = 0.0f;
                inst->params[i].bool_value = false;
                inst->params[i].string_value = "";
            }

            s.instance.action = inst;
        }
    }

    // BIN → Definition
    WorkflowStepDef *def = workflow_step_def_at(wf, step_index);
    if(def != nullptr)
    {
        workflow_storage_to_def(sd, def);
    }

    // Definition → Runtime 快照
    workflow_snapshot_step_def((int)wf, step_index, s);
}

bool workflow_load_from_storage()
{
    if(!workflow_storage_init())
    {
        return false;
    }

    if(!workflow_storage_load_meta())
    {
        return false;
    }

    workflow_clear();
    trigger_instance_index = 0;
    action_instance_index = 0;

    bool any = false;
    uint8_t max_index = 0;

    for(uint8_t wf = 0; wf < WORKFLOW_MAX_COUNT; wf++)
    {
        if(!workflow_storage_get_valid(wf))
        {
            continue;
        }

        WorkflowDefinition def;
        WorkflowStorageResult r = workflow_storage_load(wf, &def);

        if(r != WF_STG_OK)
        {
            Serial.printf(
                "[Workflow] bin load failed: wf=%u err=%s\n",
                (unsigned)wf,
                workflow_storage_result_name(r)
            );
            continue;
        }

        Workflow &w = workflows[wf];

        w.id = String(def.id);
        w.name = String(def.name);
        w.enable = def.enable != 0;
        w.timeout_ms = def.timeout_ms;
        w.state = WORKFLOW_IDLE;
        w.current_step = 0;
        w.step_count = def.step_count;
        w.cmd_id = "";
        w.finish_callback = nullptr;
        w.start_time = 0;

        for(uint8_t s = 0; s < def.step_count; s++)
        {
            workflow_apply_step_definition(wf, s, &def.steps[s]);
        }

        max_index = wf;
        any = true;
    }

    if(!any)
    {
        workflow_count = 0;
        return false;
    }

    workflow_count = (uint8_t)(max_index + 1);

    return true;
}

bool workflow_has_any_dirty()
{
    return dirty_any();
}

bool workflow_mark_step_dirty(
    uint8_t workflow_index,
    uint8_t step_index
)
{
    if(workflow_index >= WORKFLOW_MAX_COUNT ||
       step_index >= WORKFLOW_MAX_STEP)
    {
        return false;
    }

    // Dirty Transaction 模型（§16 / §17）：
    //   只有"当前不存在任何 Dirty"且本次修改产生第一个 Dirty 时才 +1
    if(!dirty_any())
    {
        if(!system_command_critical_operation_acquire())
        {
            // Acquire 失败 = 系统已进入 RESTART_PENDING / RESTARTING
            Serial.println(
                "[Workflow] step modify rejected: critical op acquire failed"
            );
            return false;
        }
        wf_dirty_critical_held = true;
    }

    dirty_mark(workflow_index, step_index);

    // 启动 / 刷新延迟保存窗口（§22）
    workflow_save_since_ms = millis();

    return true;
}

bool workflow_save_transaction()
{
    if(!dirty_any())
    {
        return true;
    }

    bool all_ok = true;

    for(uint8_t wf = 0; wf < WORKFLOW_MAX_COUNT; wf++)
    {
        if(!dirty_workflow_has(wf))
        {
            continue;
        }

        WorkflowDefinition def;
        workflow_build_definition(wf, &def);

        WorkflowStorageResult r = workflow_storage_save(wf, &def);

        if(r != WF_STG_OK)
        {
            Serial.printf(
                "[Workflow] save transaction failed: wf=%u err=%s\n",
                (unsigned)wf,
                workflow_storage_result_name(r)
            );
            all_ok = false;
            break;
        }
    }

    if(!all_ok)
    {
        // §18 / §25：失败不 release、不清 Dirty，允许后续重新 Save
        return false;
    }

    for(uint8_t wf = 0; wf < WORKFLOW_MAX_COUNT; wf++)
    {
        if(dirty_workflow_has(wf))
        {
            dirty_workflow_clear(wf);
        }
    }

    workflow_save_since_ms = 0;

    if(wf_dirty_critical_held)
    {
        system_command_critical_operation_release();
        wf_dirty_critical_held = false;
    }

    return true;
}

void workflow_request_save()
{
    workflow_save_since_ms = millis();
}

bool workflow_delete(
    uint8_t workflow_index
)
{
    if(workflow_index >= WORKFLOW_MAX_COUNT)
    {
        return false;
    }

    WorkflowStorageResult r = workflow_storage_delete(workflow_index);

    if(r != WF_STG_OK)
    {
        Serial.printf(
            "[Workflow] delete failed: wf=%u err=%s\n",
            (unsigned)workflow_index,
            workflow_storage_result_name(r)
        );
        return false;
    }

    // 禁止下一次启动
    workflows[workflow_index].enable = false;

    // §27：正在运行时【不销毁 Runtime Snapshot】
    //   运行中的 Runtime 持有自己的参数快照，继续执行到结束；
    //   同时不清 Definition / 不改 step_count —— 否则会让在飞运行提前结束。
    if(workflows[workflow_index].state == WORKFLOW_RUNNING)
    {
        return true;
    }

    for(uint8_t s = 0; s < WORKFLOW_MAX_STEP; s++)
    {
        workflow_clear_step_def(workflow_index, s);
    }

    workflows[workflow_index].step_count = 0;
    workflows[workflow_index].current_step = 0;

    return true;
}

// 延迟保存窗口到期检查（由 workflow_task() 调用）
//
// 注意：这里不是"为了维护 Critical 而持续轮询"，
// Critical 的 +1/-1 完全由 mark_dirty / save_transaction 显式驱动，
// 本函数只负责在窗口到期时触发一次保存。
static void workflow_delayed_save_poll()
{
    if(workflow_save_since_ms == 0)
    {
        return;
    }

    if((unsigned long)(millis() - workflow_save_since_ms) >=
       WORKFLOW_SAVE_DELAY_MS)
    {
        workflow_save_transaction();
    }
}

void workflow_clear()
{
    // =====================================================
    // 1. 清理所有 Workflow
    // =====================================================
    for(uint8_t i = 0; i < WORKFLOW_MAX_COUNT; i++) {
        // 清空会丢弃所有运行态，必须先释放可能持有的
        // Critical Operation（reload 路径会走到这里）
        workflow_critical_release_by_index(i);
        workflows[i].id = "";
        workflows[i].name = "";
        workflows[i].enable = false;
        workflows[i].state = WORKFLOW_IDLE;
        workflows[i].step_count = 0;
        workflows[i].current_step = 0;
        workflows[i].start_time = 0;
        workflows[i].timeout_ms = 0;
        workflows[i].cmd_id = "";
        workflows[i].finish_callback = nullptr;
        for(uint8_t j = 0; j < WORKFLOW_MAX_STEP; j++) {
            workflows[i].steps[j].id = "";
            workflows[i].steps[j].instance.trigger = nullptr;
            workflows[i].steps[j].instance.action = nullptr;
            workflows[i].steps[j].type = WORKFLOW_STEP_ACTION;
            workflows[i].steps[j].instance_type = INSTANCE_ACTION;
            workflow_clear_step_def(i, j);
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
    if(arr.isNull())
    {
        json_state = WORKFLOW_JSON_ERROR;
        return false;
    }
    uint8_t index = 0;
    for(JsonObject wf : arr)
    {
        if(index >= WORKFLOW_MAX_COUNT)
            break;
        Workflow &workflow =
            workflows[index];
        workflow.id =
            wf["id"].as<String>();
        workflow.name =
            wf["name"].as<String>();
        workflow.enable =
            wf["enable"] | false;
        workflow.timeout_ms =
            wf["timeout_ms"] | 600000;
        workflow.state =
            WORKFLOW_IDLE;
        workflow.current_step =
            0;
        JsonArray steps =
            wf["steps"];
        uint8_t step_index = 0;
        for(JsonObject step : steps)
        {
            if(step_index >= WORKFLOW_MAX_STEP)
                break;
            WorkflowStep &s =
                workflow.steps[step_index];
            String type =
                step["type"].as<String>();
            s.id =
                step["id"].as<String>();
            // =================================================
            // Trigger
            // =================================================
            if(type == "trigger")
            {
                s.type =
                    WORKFLOW_STEP_TRIGGER;
                s.instance_type =
                    INSTANCE_TRIGGER;
                const WorkflowTriggerDescriptor *desc =
                    find_trigger_descriptor(s.id);
                if(desc == nullptr)
                {
                    s.instance.trigger = nullptr;
                    workflow_capture_step_def(index, step_index, s);
                    step_index++;
                    continue;
                }
                if(trigger_instance_index >=
                   WORKFLOW_MAX_COUNT * WORKFLOW_MAX_STEP)
                {
                    json_state =
                        WORKFLOW_JSON_ERROR;
                    return false;
                }
                WorkflowTriggerInstance *inst =
                    &trigger_instances[
                        trigger_instance_index++
                    ];
                inst->id =
                    s.id;
                inst->descriptor =
                    desc;
                inst->param_count =
                    0;
                inst->state =
                    TRIGGER_IDLE;
                inst->running =
                    false;
                inst->callback =
                    workflow_trigger_callback;
                // runtime reset
                inst->timer_runtime.next_trigger_time = 0;
                inst->timer_runtime.last_trigger_minute = -1;
                inst->timer_runtime.triggered = false;
                inst->timer_runtime.expired = false;
                inst->timer_runtime.type = 0;
                inst->delay_runtime.start_time = 0;
                inst->delay_runtime.delay_ms = 0;
                inst->delay_runtime.started = false;
                for(uint8_t i = 0;
                    i < WORKFLOW_MAX_PARAM;
                    i++)
                {
                    inst->params[i].name = "";
                    inst->params[i].type = PARAM_INT;
                    inst->params[i].int_value = 0;
                    inst->params[i].float_value = 0.0f;
                    inst->params[i].bool_value = false;
                    inst->params[i].string_value = "";
                }
                JsonObject step_params =
                    step["params"];
                workflow_parse_params(
                    step_params,
                    inst->params,
                    inst->param_count
                );
                s.instance.trigger =
                    inst;
            }
            // =================================================
            // Action
            // =================================================
            else if(type == "action")
            {
                s.type =
                    WORKFLOW_STEP_ACTION;
                s.instance_type =
                    INSTANCE_ACTION;
                const WorkflowActionDescriptor *desc =
                    find_action_descriptor(s.id);
                if(desc == nullptr)
                {
                    s.instance.action = nullptr;
                    workflow_capture_step_def(index, step_index, s);
                    step_index++;
                    continue;
                }
                if(action_instance_index >=
                   WORKFLOW_MAX_COUNT * WORKFLOW_MAX_STEP)
                {
                    json_state =
                        WORKFLOW_JSON_ERROR;
                    return false;
                }
                WorkflowActionInstance *inst =
                    &action_instances[
                        action_instance_index++
                    ];
                inst->id =
                    s.id;
                inst->descriptor =
                    desc;
                inst->param_count =
                    0;
                inst->result =
                    ACTION_IDLE;
                inst->running =
                    false;
                inst->runtime =
                    nullptr;
                inst->callback =
                    workflow_action_callback;
                for(uint8_t i = 0;
                    i < WORKFLOW_MAX_PARAM;
                    i++)
                {
                    inst->params[i].name = "";
                    inst->params[i].type = PARAM_INT;
                    inst->params[i].int_value = 0;
                    inst->params[i].float_value = 0.0f;
                    inst->params[i].bool_value = false;
                    inst->params[i].string_value = "";
                }
                JsonObject step_params =
                    step["params"];
                workflow_parse_params(
                    step_params,
                    inst->params,
                    inst->param_count
                );
                s.instance.action =
                    inst;
            }
            // 解析完成后把可持久化数据写入 Definition（Definition/Runtime 分离）
            workflow_capture_step_def(index, step_index, s);
            step_index++;
        }
        workflow.step_count =
            step_index;
        // Step0必须Trigger
        if(workflow.step_count > 0)
        {
            WorkflowStep &first =
                workflow.steps[0];
            if(first.type != WORKFLOW_STEP_TRIGGER)
            {
                workflow.state =
                    WORKFLOW_ERROR;
            }
        }
        index++;
    }
    workflow_count =
        index;
    json_state =
        WORKFLOW_JSON_READY;
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
//workflow执行完毕后重置step状态，以备下一次执行
void workflow_reset_step(WorkflowStep &step)
{
    if(step.instance_type == INSTANCE_TRIGGER)
    {
        WorkflowTriggerInstance *trigger =
            step.instance.trigger;

        if(trigger &&
           trigger->descriptor &&
           trigger->descriptor->reset)
        {
            trigger->descriptor->reset(trigger);
        }
    }
    else if(step.instance_type == INSTANCE_ACTION)
    {
        WorkflowActionInstance *action =
            step.instance.action;
        if(action &&
           action->descriptor &&
           action->descriptor->reset)
        {
            action->descriptor->reset(action);
        }
    }
    step.waiting=false;
}
// =====================================================
// Workflow启动
// =====================================================
bool workflow_start(
    Workflow *workflow,
    bool skip_first_step,
    const String &cmd_id,
    WorkflowResultCallback callback
)
{
    if(workflow == nullptr)
        return false;
    if(!workflow->enable)
        return false;
    if(workflow->state == WORKFLOW_RUNNING)
        return false;

    // ============================================
    // Critical Operation 保护（接入规范 §3）
    //
    // Workflow 执行期间（含 Step0 Trigger 的等待阶段）会驱动外部设备
    // 并可能修改持久化数据，不可被重启打断，因此开始前必须获得许可。
    //
    // 系统已进入 RESTART_PENDING（10s 安全窗口）时 acquire() 返回 false，
    // 按规范此时【不得】开始本次 Workflow，直接放弃并返回 false。
    // 绝不能在 acquire 失败的情况下继续执行 —— 否则重启可能正好落在
    // 阀门开启 / 电机运转的过程中。
    //
    // 配对: 释放统一由 workflow_terminate() 负责（以及
    //       workflow_stop / workflow_disable / workflow_clear 强制终止路径）。
    // ============================================
    int wf_index = workflow_index_of(workflow);

    if(wf_index < 0)
        return false;

    if(!system_command_critical_operation_acquire())
    {
        Serial.printf(
            "[Workflow] critical op acquire rejected, "
            "workflow start aborted: %s\n",
            workflow->id.c_str()
        );
        return false;
    }

    workflow_critical_held[wf_index] = true;

    Serial.printf(
        "[Workflow] critical op acquired (workflow idx=%d, id=%s, count=%u)\n",
        wf_index,
        workflow->id.c_str(),
        (unsigned)system_command_critical_operation_count()
    );

    // ============================================
    // Reset所有Step运行状态    //    // Trigger:       descriptor->reset()     // Action:    descriptor->reset()   // 不关心具体类型    // ============================================
    for(uint8_t i = 0;
        i < workflow->step_count;
        i++)
    {
        workflow_reset_step(
            workflow->steps[i]
        );
        // Definition → Runtime 参数快照（值拷贝）
        //
        // 必须先 reset 再快照：descriptor->reset() 可能改写 instance 内部字段，
        // 放在其后可以保证本次运行拿到的一定是 Definition 的当前值。
        // 快照生成后，运行期间对 Definition 的修改不会影响本次运行。
        workflow_snapshot_step_def(
            wf_index, i, workflow->steps[i]
        );
    }
    // ============================================
    // Workflow状态初始化
    // ============================================
    workflow->state =
        WORKFLOW_RUNNING;
    workflow->current_step =
        skip_first_step ? 1 : 0;
    workflow->start_time =
        millis();
    // CommandManager 关联：只保存关联 ID 与完成回调
    workflow->cmd_id =
        cmd_id;
    workflow->finish_callback =
        callback;
    return true;
}

// =====================================================
// Workflow 完成通知（一次性）
//
// 在 Workflow 进入 FINISHED / TIMEOUT / ERROR 时，
// 将 cmd_id + 最终状态经 CommandManager 定义的回调返回；
// 回调后立即清空，避免重复通知或跨命令误报。
// =====================================================
static void workflow_notify_finish(Workflow &wf)
{
    if(wf.finish_callback == nullptr)
        return;

    WorkflowResultCallback cb =
        wf.finish_callback;
    String cmd_id =
        wf.cmd_id;
    WorkflowState state =
        wf.state;

    wf.finish_callback = nullptr;
    wf.cmd_id = "";

    cb(cmd_id, state);
}

// =====================================================
// Workflow 终止统一收口（Critical Operation 的唯一释放点）
//
// Workflow 的所有结束路径（FINISHED / TIMEOUT / ERROR）都必须走这里:
//   1) 释放本 Workflow 持有的 Critical Operation（无论成功 / 失败 / 超时）
//   2) 写入最终状态
//   3) 回调 CommandManager 上报结果
//
// ========================= 顺序说明（勿调换）=========================
// 必须【先 Release，后置 state】。
//
// 原因: workflow_start() 由 CommandManager 调用，运行在 esp-mqtt 任务；
//       本函数运行在 Arduino loop 任务，两者可能在不同核上并发。
//
// 若先置 state 再 Release，会存在一个竞争窗口:
//   loop  : wf.state = WORKFLOW_ERROR   ← 状态已非 RUNNING
//   mqtt  : workflow_start() 的 "state != RUNNING" 守卫通过
//           → acquire()（count+1）→ flag=true → state=RUNNING
//   loop  : Release → 读到 flag==true → count-1 → flag=false
//   结果  : Workflow 处于 RUNNING 但 flag=false、count 残留 +1
//           → Critical Operation 永久泄漏，系统再也无法重启。
//
// 先 Release 后置 state 可彻底闭合该窗口:
//   Release 之后、state 改写之前，wf.state 仍是 WORKFLOW_RUNNING，
//   workflow_start() 的 RUNNING 守卫会直接拒绝新启动，无法插入。
// ====================================================================
//
// 为什么不把 release 写进 workflow_notify_finish() 内部:
//   该函数在 finish_callback == nullptr 时会提前 return，而事件触发 /
//   定时器触发的 Workflow 没有回调。若放在那里，这类 Workflow 会漏掉
//   release，critical count 永不归零，系统将永久无法重启（严重 bug）。
//
// 幂等保证: workflow_critical_release_by_index() 依赖持有标记，
//   同一 Workflow 被重复收口也只会 release 一次。
// =====================================================
static void workflow_terminate(
    Workflow &wf,
    WorkflowState state
)
{
    // ---- 1) 先释放 Critical Operation（早于 state 改写）----
    workflow_critical_release_by_index(
        workflow_index_of(&wf)
    );

    // ---- 2) 再写最终状态（workflow_notify_finish 会读取它）----
    wf.state = state;

    // ---- 3) 最后回调上报 ----
    workflow_notify_finish(wf);
}


//============================================
//trigger调度函数
//=============================================
void workflow_trigger_start(
    WorkflowTriggerInstance *trigger
)
{
    if(trigger == nullptr)
        return;
    if(trigger->descriptor == nullptr ||
       trigger->descriptor->start == nullptr)
    {
        trigger->state =
            TRIGGER_FAILED;
        return;
    }
    trigger->running = true;
    trigger->state =
        TRIGGER_RUNNING;
    trigger->descriptor->start(trigger);
}

void workflow_trigger_poll(
    WorkflowTriggerInstance *trigger
)
{
    if(trigger == nullptr)
        return;
    if(!trigger->running)
        return;
    if(trigger->descriptor == nullptr ||
       trigger->descriptor->poll == nullptr)
    {
        trigger->state =
            TRIGGER_FAILED;
        trigger->running = false;
        return;
    }
    trigger->descriptor->poll(trigger);
    if(trigger->state == TRIGGER_SUCCESS ||
       trigger->state == TRIGGER_FAILED)
    {
        trigger->running = false;
    }
}

void workflow_trigger_reset(
    WorkflowTriggerInstance *trigger
)
{
    if(trigger == nullptr)
        return;
    trigger->state =
        TRIGGER_IDLE;
    trigger->running =
        false;
    if(trigger->descriptor &&
       trigger->descriptor->reset)
    {
        trigger->descriptor->reset(trigger);
    }
}

// =====================================================
// 入队临时 Action（非阻塞）
// =====================================================
static bool enqueue_temp_action(
    const String &id,
    const String &payload,
    uint32_t *instance_id,
    const String &cmd_id,
    CommandTempActionCallback callback,
    unsigned long timeout_ms
)
{
    uint8_t next_wr = (queue_wr_ptr + 1) % MAX_TEMP_ACTIONS;
    // 环形队列判满
    if (next_wr == queue_rd_ptr)
    {
        return false;
    }

    const WorkflowActionDescriptor *desc = find_action_descriptor(id);
    if (desc == nullptr) {
        return false;
    }

    WorkflowActionInstance *inst = temp_action_alloc_instance();
    if(inst == nullptr)
    {
        return false;
    }

    // ================================================
    // Critical Operation 保护（接入规范 §3）
    //
    // 临时 Action 由 CommandManager 直接下发，执行期间驱动硬件，
    // 不可被重启打断，因此入队前必须获得许可。
    //
    // acquire 失败（系统已进入 10s 安全窗口）时【不得】开始该 Action:
    // 归还刚申请到的实例后放弃，避免实例池泄漏。
    //
    // 放在申请实例之后、写队列之前,是为了让失败路径只需回收实例,
    // 不产生任何需要 release 的中间状态。
    // ================================================
    if(!system_command_critical_operation_acquire())
    {
        Serial.printf(
            "[Workflow] critical op acquire rejected, "
            "temp action aborted: %s\n",
            id.c_str()
        );

        temp_action_free_instance(inst);
        return false;
    }

    // 初始化 Action Instance

    inst->descriptor = desc;
    inst->id = desc->id;
    inst->param_count = 0;
    inst->result = ACTION_IDLE;
    
    inst->runtime = nullptr;
    inst->running = false;
    inst->callback = nullptr;


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


    // payload 由 Workflow 转换为 Action 参数（CommandManager 不解析）
    uint8_t param_count = 0;
    WorkflowParamValue params[WORKFLOW_MAX_PARAM];
    if(payload.length() > 0)
    {
        JsonDocument doc;
        DeserializationError err =
            deserializeJson(doc, payload);
        if(!err)
        {
            JsonObject obj =
                doc.as<JsonObject>();
            if(!obj.isNull())
            {
                workflow_parse_params(
                    obj,
                    params,
                    param_count
                );
            }
        }
    }

    // 写入参数
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
    item.completed = false;
    item.result = ACTION_IDLE;
    item.start_time = millis();
    item.cmd_id = cmd_id;
    item.callback = callback;
    item.timeout_ms = timeout_ms;
    // 标记本 Item 持有 Critical Operation，由 temp_action_complete()
    // 在结束（成功 / 失败 / 超时）时配对释放
    item.critical_held = true;
    queue_wr_ptr = next_wr;

    Serial.printf(
        "[Workflow] critical op acquired (temp action id=%u, act=%s, count=%u)\n",
        (unsigned)item.instance_id,
        id.c_str(),
        (unsigned)system_command_critical_operation_count()
    );

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
    const String &payload,
    uint32_t *instance_id,
    const String &cmd_id,
    CommandTempActionCallback callback,
    unsigned long timeout_ms
)
{
    return enqueue_temp_action(
        id,
        payload,
        instance_id,
        cmd_id,
        callback,
        timeout_ms
    );
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

        // 防御性兜底：正常路径下 Item 完成时已由 temp_action_complete()
        // 释放过（标记已置 false，此处为空操作）。若将来新增了绕过
        // temp_action_complete() 的结束分支，这里保证 Item 被回收时
        // 不会带着未释放的 Critical Operation 消失。
        temp_action_critical_release(item);

        // 清理 instance
        if(item.instance != nullptr)
        {
            temp_action_free_instance(
                item.instance
            );

            item.instance = nullptr;
        }

        item.desc = nullptr;
        item.instance = nullptr;
        item.cmd_id = "";
        item.callback = nullptr;
        item.timeout_ms = 0;
        item.start_time = 0;
        item.instance_id = 0;
        item.result = ACTION_IDLE;
        item.completed = false;
        item.critical_held = false;

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
// Workflow执行核心（非阻塞，loop调用）
// =====================================================
void workflow_task()
{
    unsigned long now = millis();

    // 延迟保存窗口到期 → 执行一次完整保存事务
    workflow_delayed_save_poll();

    // =====================================================
    // 执行 Workflow
    // =====================================================
    for(uint8_t i = 0; i < workflow_count; i++)
    {
        Workflow &wf = workflows[i];
        if(!wf.enable)
            continue;
        if(wf.state != WORKFLOW_RUNNING)
            continue;
        // Workflow 超时
        if((uint32_t)(now - wf.start_time) > wf.timeout_ms)
        {
            // 超时属于"结束"的一种，同样必须释放 Critical Operation
            workflow_terminate(
                wf,
                WORKFLOW_TIMEOUT
            );
            continue;
        }
        // 全部完成
        if(wf.current_step >= wf.step_count)
        {
            workflow_terminate(
                wf,
                WORKFLOW_FINISHED
            );
            continue;
        }
        WorkflowStep &step =
            wf.steps[wf.current_step];
        // =================================================
        // Trigger
        // =================================================
        if(step.instance_type == INSTANCE_TRIGGER)
        {
            WorkflowTriggerInstance *trigger =
                step.instance.trigger;
            if(trigger == nullptr)
            {
                workflow_terminate(
                    wf,
                    WORKFLOW_ERROR
                );
                continue;
            }
            if(!trigger->running)
            {
                if(trigger->descriptor == nullptr ||
                   trigger->descriptor->start == nullptr)
                {
                    trigger->state =
                        TRIGGER_FAILED;
                }
                else
                {
                    trigger->running = true;
                    trigger->state =
                        TRIGGER_RUNNING;
                    trigger->descriptor->start(trigger);
                }
            }
            else
            {
                if(trigger->descriptor == nullptr ||
                   trigger->descriptor->poll == nullptr)
                {
                    trigger->state =
                        TRIGGER_FAILED;
                }
                else
                {
                    trigger->descriptor->poll(trigger);
                }
            }
            if(trigger->state == TRIGGER_SUCCESS)
            {
                trigger->running = false;
                wf.current_step++;
            }
            else if(trigger->state == TRIGGER_FAILED)
            {
                trigger->running = false;

                workflow_terminate(
                    wf,
                    WORKFLOW_ERROR
                );
            }
            continue;
        }
        // =================================================
        // Action
        // =================================================
        if(step.instance_type == INSTANCE_ACTION)
        {
            WorkflowActionInstance *action =
                step.instance.action;

            if(action == nullptr)
            {
                workflow_terminate(
                    wf,
                    WORKFLOW_ERROR
                );
                continue;
            }


            // 第一次进入 Action
            if(!action->running)
            {
                workflow_action_start(action);
            }
            else
            {
                workflow_action_poll(action);
            }


            // Action完成
            if(action->result == ACTION_SUCCESS)
            {
                action->running = false;

                wf.current_step++;
            }
            // Action失败
            else if(action->result == ACTION_FAILED)
            {
                action->running = false;

                workflow_terminate(
                    wf,
                    WORKFLOW_ERROR
                );
            }
        }
    }
    // =====================================================
    // Temp Action Queue
    // =====================================================
    while(queue_rd_ptr != queue_wr_ptr)
    {
        TempActionItem &item =
            temp_action_queue[queue_rd_ptr];
        bool timeout =
            ((unsigned long)(millis() - item.start_time)
             >
             item.timeout_ms);
        if(timeout)
        {
            Serial.printf(
                "[Workflow] Temp action timeout:%s\n",
                item.desc ?
                item.desc->id :
                "nullptr"
            );

            if(item.instance != nullptr)
            {
                item.instance->result = ACTION_FAILED;
                item.instance->running = false;
            }

            // 等待超时同样属于"执行完毕"，必须释放 Critical Operation
            // 并回调上报 ACTION_FAILED
            temp_action_complete(
                item,
                ACTION_FAILED
            );
        }
        else
        {
            WorkflowActionInstance *action =
                item.instance;
            if(action == nullptr)
            {
                // 实例缺失，Action 不可能执行。
                // 走统一收口：释放 Critical Operation 并上报失败，
                // 避免命令既不上报、计数又泄漏。
                temp_action_complete(
                    item,
                    ACTION_FAILED
                );
            }
            else
            {
                // start
                if(!action->running)
                {
                    if(action->descriptor == nullptr ||
                       action->descriptor->start == nullptr)
                    {
                        action->result =
                            ACTION_FAILED;
                    }
                    else
                    {
                        action->running = true;
                        action->result =
                            ACTION_RUNNING;
                        action->descriptor->start(action);
                    }
                }
                // poll
                if(action->running)
                {
                    if(action->descriptor == nullptr ||
                       action->descriptor->poll == nullptr)
                    {
                        action->result =
                            ACTION_FAILED;
                    }
                    else
                    {
                        action->descriptor->poll(action);
                    }
                }
                if(action->result == ACTION_SUCCESS ||
                   action->result == ACTION_FAILED)
                {
                    // 执行完毕（成功或失败）统一收口：
                    // 释放 Critical Operation + 回调上报结果
                    temp_action_complete(
                        item,
                        action->result
                    );
                }
            }
        }
        if(!item.completed)
        {
            // 队头未完成，等待下一次workflow_task()
            break;
        }
        else
        {
            // 队头未完成
            break;
        }
    }
    // =====================================================
    // 最终清理
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
            // 禁用会强制结束正在运行的 Workflow，
            // 必须释放其持有的 Critical Operation
            workflow_critical_release_by_index(i);
            workflows[i].enable = false;
            workflows[i].state = WORKFLOW_IDLE;
            workflows[i].cmd_id = "";
            workflows[i].finish_callback = nullptr;
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
            // 强制停止是"结束"的一种：无论当前处于哪一步都要释放
            // Critical Operation，否则计数永不归零，系统永久无法重启
            workflow_critical_release_by_index(i);
            workflows[i].state = WORKFLOW_IDLE;
            workflows[i].current_step = 0;
            workflows[i].start_time = 0;
            workflows[i].cmd_id = "";
            workflows[i].finish_callback = nullptr;
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

static void workflow_trigger_callback(
    WorkflowTriggerInstance *trigger,
    WorkflowTriggerState state
)
{
    if(trigger == nullptr)
        return;


    trigger->state = state;
}

static void workflow_action_callback(
    WorkflowActionInstance *action,
    WorkflowActionResult result
)
{
    if(action == nullptr)
        return;


    action->result = result;
}
