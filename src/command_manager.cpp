#include <Arduino.h>
#include <ArduinoJson.h>
#include <time.h>  
#include "command_manager.h"
#include "time_manager.h"
#include "workflow.h"
#include "system_state.h"
#include "cloud_manager.h"


//callback格式定义
typedef void (*CommandTempActionCallback)(
    uint32_t instance_id,
    const String &action_id,
    const String &command_id,
    WorkflowActionResult result
);

typedef void (*WorkflowResultCallback)(
    const String &workflow_id,
    const String &command_id,
    WorkflowState state
);
// =====================================================
// 日志回调
// =====================================================
static CommandLogCallback log_callback = nullptr;

void command_manager_set_log_callback(CommandLogCallback callback)
{
    log_callback = callback;
}

static void command_log(const char *level, const char *msg)
{
    if (log_callback != nullptr) {
        log_callback(level, msg);
    }
}

// =====================================================
// 内部函数声明
// =====================================================
static bool command_execute_action(const CommandMessage &cmd, JsonDocument &response);
static bool command_execute_workflow(const CommandMessage &cmd, JsonDocument &response);
static bool command_query_actions(JsonDocument &response);
static bool command_query_triggers(JsonDocument &response);
static bool command_query_workflows(JsonDocument &response);
static bool command_query_state(JsonDocument &doc, JsonDocument &response);
// =====================================================
// 最近执行结果
// =====================================================
static CommandResult last_result = CMD_RESULT_OK;


// 结果回调
static CommandResultCallback result_callback = nullptr;

void command_manager_set_result_callback(CommandResultCallback callback)
{
    result_callback = callback;
}

// 上报结果（内部使用）
static void command_report_result(const String &json)
{
    if (result_callback != nullptr) {
        result_callback(json);
    }
}

// 获取可用Unix时间戳，未同步返回0
static time_t get_unix_timestamp()
{
    return time_get();
}


// =====================================================
// 初始化
// =====================================================
void command_manager_init()
{
    last_result = CMD_RESULT_OK;
}
// =====================================================
// 主任务
// =====================================================
void command_manager_task()
{


}

// =====================================================
// 接收并执行命令
// =====================================================
String command_manager_execute(const String &json)
{
    const String command = cmd.command;
    command_log("INFO", "Received command");

    // 1. 解析 JSON
    DeserializationError error = deserializeJson(doc, json);
    if (error) {
        response["status"] = "error";
        response["message"] = "Invalid JSON";
        last_result = CMD_RESULT_ERROR;
        String output;
        serializeJson(response, output);
        return output;
    }

    // 2. 获取命令类型
    const char *cmd = doc["command"];
    if (cmd == nullptr) {
        response["status"] = "error";
        response["message"] = "Missing 'command' field";
        last_result = CMD_RESULT_ERROR;
        String output;
        serializeJson(response, output);
        return output;
    }

    // 3. 路由分发
    bool success = false;
    if (strcmp(cmd, "execute_action") == 0) {
        success = command_execute_action(doc, response);
    } else if (strcmp(cmd, "execute_workflow") == 0) {
        success = command_execute_workflow(doc, response);
    } else if (strcmp(cmd, "query_actions") == 0) {
        success = command_query_actions(response);
    } else if (strcmp(cmd, "query_state") == 0) {
    success = command_query_state(doc, response);
    } else if (strcmp(cmd, "query_triggers") == 0) {
        success = command_query_triggers(response);
    } else if (strcmp(cmd, "query_workflows") == 0) {
        success = command_query_workflows(response);
    } else {
        response["status"] = "error";
        response["message"] = "Unknown command: ";
        String msg = response["message"].as<String>();
        msg += cmd;
        response["message"] = msg;
        success = false;
    }

    // 4. 添加通用字段
    response["command"] = cmd;

    if (!success && !!response["status"].isNull()) {
        response["status"] = "error";
        response["message"] = "Execution failed";
        last_result = CMD_RESULT_ERROR;
    }
    time_t ts = get_unix_timestamp();
    if (ts > 0)
    {
        response["timestamp"] = ts;
    }
    // 5. 序列化返回
    String output;
    serializeJson(response, output);
    command_log("INFO", "Command executed");
    return output;
}

// =====================================================
// 获取最近执行结果
// =====================================================
CommandResult command_manager_last_result()
{
    return last_result;
}

// =====================================================
// 清除状态
// =====================================================
void command_manager_clear()
{
    last_result = CMD_RESULT_OK;
    command_log("INFO", "Command cleared");
}




// =====================================================
// 执行 Action
// =====================================================
// Action完成回调，用来减少并发计数


static bool command_execute_action(JsonDocument &doc, JsonDocument &response)
{       
    const char *action_id = doc["action"];
    if (action_id == nullptr) {
        response["status"] = "error";
        response["message"] = "Missing 'action' field";
        last_result = CMD_RESULT_ERROR;
        return false;
    }

    WorkflowParamValue params[WORKFLOW_MAX_PARAM];
    uint8_t param_count = 0;

    JsonObject param_obj = doc["params"];
    if (!param_obj.isNull()) {
        for (JsonPair p : param_obj) {
            if (param_count >= WORKFLOW_MAX_PARAM) break;
            params[param_count].name = p.key().c_str();

            if (p.value().is<int>()) {
                params[param_count].type = PARAM_INT;
                params[param_count].int_value = p.value().as<int>();
            } else if (p.value().is<float>()) {
                params[param_count].type = PARAM_FLOAT;
                params[param_count].float_value = p.value().as<float>();
            } else if (p.value().is<bool>()) {
                params[param_count].type = PARAM_BOOL;
                params[param_count].bool_value = p.value().as<bool>();
            } else if (p.value().is<const char*>()) {
                params[param_count].type = PARAM_STRING;
                params[param_count].string_value = p.value().as<const char*>();
            } else {
                continue;
            }
            param_count++;
        }
    }

    uint32_t instance_id=0;


    bool queued =
    workflow_enqueue_action(
        action_id,
        params,
        param_count,
        &instance_id,
        action_finish_callback,
        DEFAULT_ACTION_TIMEOUT_MS
    );


    if(!queued)
    {
        response["status"]="error";
        response["message"]="Failed to queue action";
        last_result=CMD_RESULT_FAILED;
        return false;
    }

    response["action"] = action_id;


    // 新逻辑：
    // workflow_enqueue_action 只负责入队
    // 真正结果由 callback 返回
    const char *cmd_id = doc["command_id"] | "";
    if(tracker_ok)
    {active_action_count++;}
    response["status"] = "accepted";
    response["message"] = "Action queued";
    response["instance_id"] = instance_id;
    last_result = CMD_RESULT_RUNNING;
    return true;
}
//执行完毕后的callback
static void command_temp_action_callback(
    uint32_t instance_id,
    const String &action_id,
    const String &command_id,
    WorkflowActionResult result
)
{
    JsonDocument doc;

    doc["type"] = "action_result";
    doc["instance_id"] = instance_id;
    doc["action"] = action_id;
    doc["command_id"] = command_id;

    if(result == ACTION_SUCCESS)
    {
        doc["status"] = "success";
    }
    else
    {
        doc["status"] = "failed";
    }

    time_t ts = get_unix_timestamp();

    if(ts > 0)
    {
        doc["timestamp"] = ts;
    }

    String json;

    serializeJson(
        doc,
        json
    );

    command_report_result(json);
}


// =====================================================
// 执行 Workflow（通过 Workflow ID）
// =====================================================



static bool command_execute_workflow(JsonDocument &doc, JsonDocument &response)
{
    const char *wf_id = doc["workflow"];
    if (wf_id == nullptr) {
        response["status"] = "error";
        response["message"] = "Missing 'workflow' field";
        last_result = CMD_RESULT_ERROR;
        return false;
    }

    // 查找 Workflow
    Workflow *wf = nullptr;
    for (uint8_t i = 0; i < workflow_get_count(); i++) {
        Workflow *w = workflow_get(i);
        if (w != nullptr && w->id == wf_id) {
            wf = w;
            break;
        }
    }

    if (wf == nullptr) {
        response["status"] = "error";
        String msg = "Workflow not found: ";
        msg += wf_id;
        response["message"] = msg;
        last_result = CMD_RESULT_ERROR;
        return false;
    }

    // 启动 Workflow
    bool success = workflow_start(wf, true);
    if (success) {
        workflow_start(
            wf,
            true,
            workflow_finish_callback,
            cmd.cmd_id
        );
        cmd.cmd_id;
        response["status"] = "accepted";
        response["message"] = "Workflow started";
        response["workflow"] = wf_id;
        last_result = CMD_RESULT_OK;
        return true;
    } else {
        response["status"] = "error";
        response["message"] = "Failed to start workflow";
        last_result = CMD_RESULT_ERROR;
        return false;
    }
}


static void command_workflow_callback(
    const String &workflow_id,
    const String &command_id,
    WorkflowState state
)
{
    JsonDocument doc;

    doc["type"] = "workflow_result";

    doc["workflow"] = workflow_id;

    doc["command_id"] = command_id;


    switch(state)
    {
        case WORKFLOW_FINISHED:
            doc["status"] = "success";
            break;

        case WORKFLOW_TIMEOUT:
            doc["status"] = "timeout";
            break;

        case WORKFLOW_ERROR:
            doc["status"] = "failed";
            break;

        default:
            doc["status"] = "unknown";
            break;
    }


    time_t ts = get_unix_timestamp();

    if(ts > 0)
    {
        doc["timestamp"] = ts;
    }


    String json;

    serializeJson(
        doc,
        json
    );


    command_report_result(json);
}
// =====================================================
// 查询 Action 列表
// =====================================================
static bool command_query_actions(JsonDocument &response)
{
    JsonArray arr = response["actions"].to<JsonArray>();
    uint8_t count = workflow_get_action_count();

    for (uint8_t i = 0; i < count; i++) {
        const WorkflowActionDescriptor *desc = workflow_get_action_descriptor(i);
        if (desc == nullptr) continue;

        JsonObject item = arr.add<JsonObject>();
        item["id"] = desc->id;
        item["name"] = desc->name;
        item["module"] = desc->module;
        if (desc->description != nullptr) {
            item["description"] = desc->name;
        }
    }

    response["status"] = "success";
    response["count"] = count;
    last_result = CMD_RESULT_OK;
    return true;
}

// =====================================================
// 查询 Trigger 列表
// =====================================================
static bool command_query_triggers(JsonDocument &response)
{
    JsonArray arr = response["triggers"].to<JsonArray>();
    uint8_t count = workflow_get_trigger_count();

    for (uint8_t i = 0; i < count; i++) {
        const WorkflowTriggerDescriptor *desc = workflow_get_trigger_descriptor(i);
        if (desc == nullptr) continue;

        JsonObject item = arr.add<JsonObject>();
        item["id"] = desc->id;
        item["name"] = desc->name;
        item["module"] = desc->module;
        if (desc->description != nullptr) {
            item["description"] = desc->description;
        }
    }

    response["status"] = "success";
    response["count"] = count;
    last_result = CMD_RESULT_OK;
    return true;
}

// =====================================================
// 查询 Workflow 列表
// =====================================================
static bool command_query_workflows(JsonDocument &response)
{
    JsonArray arr = response["workflows"].to<JsonArray>();
    uint8_t count = workflow_get_count();

    for (uint8_t i = 0; i < count; i++) {
        Workflow *wf = workflow_get(i);
        if (wf == nullptr) continue;

        JsonObject item = arr.add<JsonObject>();
        item["id"] = wf->id;
        item["name"] = wf->name;
        item["enable"] = wf->enable;

        const char *state_str = "UNKNOWN";
        switch (wf->state) {
            case WORKFLOW_IDLE: state_str = "IDLE"; break;
            case WORKFLOW_RUNNING: state_str = "RUNNING"; break;
            case WORKFLOW_WAITING: state_str = "WAITING"; break;
            case WORKFLOW_FINISHED: state_str = "FINISHED"; break;
            case WORKFLOW_TIMEOUT: state_str = "TIMEOUT"; break;
            case WORKFLOW_ERROR: state_str = "ERROR"; break;
        }
        item["state"] = state_str;
        item["step_count"] = wf->step_count;
        item["current_step"] = wf->current_step;
    }

    response["status"] = "success";
    response["count"] = count;
    last_result = CMD_RESULT_OK;
    return true;
}


// =====================================================
// 查询 System State
//
// 输入:
// {
//   "command": "query_state",
//   "key": "weight_value"
// }
//
// 输出:
// {
//   "status": "success",
//   "key": "weight_value",
//   "value": "123.5"
// }
// =====================================================
static bool command_query_state(JsonDocument &doc, JsonDocument &response)
{
    const char *key = doc["key"];
    if (key == nullptr) {
        response["status"] = "error";
        response["message"] = "Missing 'key' field";
        last_result = CMD_RESULT_ERROR;
        return false;
    }

    String value;
    if (system_state_query(String(key), value)) {
        response["status"] = "success";
        response["key"] = key;
        response["value"] = value;
        last_result = CMD_RESULT_OK;
        return true;
    }

    response["status"] = "error";
    String msg = "State not found: ";
    msg += key;
    response["message"] = msg;
    last_result = CMD_RESULT_ERROR;
    return false;
}



