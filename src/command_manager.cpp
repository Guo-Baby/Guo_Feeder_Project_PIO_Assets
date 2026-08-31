#include <Arduino.h>
#include <ArduinoJson.h>
#include <time.h>

#include "command_manager.h"
#include "time_manager.h"
#include "workflow.h"
#include "system_state.h"
#include "capability_registry.h"
#include "config_manager.h"
#include "weight.h"

// =====================================================
// 统一错误码（command_send_error 使用）
// =====================================================
#define CMD_ERROR_UNKNOWN_COMMAND      1
#define CMD_ERROR_MISSING_COMMAND      2
#define CMD_ERROR_MISSING_OBJECT       3
#define CMD_ERROR_ACTION_NOT_FOUND     4
#define CMD_ERROR_WORKFLOW_NOT_FOUND   5
#define CMD_ERROR_PARAM                6
#define CMD_ERROR_QUEUE_FULL           7
#define CMD_ERROR_DUPLICATE_CMD_ID     8
#define CMD_ERROR_SYSTEM               9
#define CMD_ERROR_EXECUTION           10
// =====================================================
// Command Runtime（命令生命周期管理）
//
// CommandRuntime 只管理"命令请求生命周期":
//   - 运行时槽位与容量保护
//   - command_id 去重
//   - 异步回调匹配（主键: cmd_id）
//   - 超时保护与清理
//   - CommandMessage 唯一所有权（其他模块只读查询，不得复制保存）
//
// 它不是执行 tracker:
//   - 不轮询 Workflow / Action 状态
//   - 不调用 workflow_temp_action_is_complete()
//   - 不替代 WorkflowManager 运行时
//
// 超时职责分离:
//   - CommandManager 超时: 保护命令生命周期队列
//   - WorkflowManager 超时: 保护工作流实际执行
//   两者独立，互不替代。
// =====================================================

#define MAX_COMMAND_RUNTIME 8

static const unsigned long COMMAND_ACTION_TIMEOUT_MS        = 600000UL;
static const unsigned long COMMAND_WORKFLOW_TIMEOUT_MS      = 600000UL;
static const unsigned long COMMAND_RUNTIME_SCAN_INTERVAL_MS = 1000UL;

static CommandRuntime runtime_queue[MAX_COMMAND_RUNTIME];

static unsigned long last_scan_ms = 0;
static bool reboot_pending = false;
static unsigned long reboot_start_ms = 0;

#define REBOOT_WAIT_TIMEOUT_MS 10000UL
// =====================================================
// 日志与结果回调
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

// 命令结果上报回调（CloudManager 注入）
static CommandResultCallback result_callback = nullptr;

void command_manager_set_result_callback(CommandResultCallback callback)
{
    result_callback = callback;
}

static void command_report_result(const String &json)
{
    if (result_callback != nullptr) {
        result_callback(json);
    }
}

// 最近执行结果
static CommandResult last_result = CMD_RESULT_OK;

// 获取可用Unix时间戳，未同步返回0
static time_t get_unix_timestamp()
{
    return time_get();
}

// =====================================================
// 统一错误出口
//
// 所有错误结果必须经过此函数生成并上报，包括：
//   - command 不存在
//   - object 不存在
//   - action 不存在
//   - workflow 不存在
//   - 参数错误
//   - system command 失败
//
// 格式:
// {
//   "cmd":"result",
//   "id":"xxx",
//   "type":"command",
//   "status":"error",
//   "error_code":xxx,
//   "message":"xxx"
// }
// =====================================================
static void command_send_error(
    const CommandMessage &cmd,
    int error_code,
    const String &message
)
{
    JsonDocument doc;
    doc["cmd"] = "result";
    doc["id"] = cmd.cmd_id;
    doc["type"] = "command";
    doc["status"] = "error";
    doc["error_code"] = error_code;
    doc["message"] = message;

    time_t ts = get_unix_timestamp();
    if (ts > 0) {
        doc["timestamp"] = ts;
    }

    String json;
    serializeJson(doc, json);
    command_report_result(json);

    last_result = CMD_RESULT_ERROR;
    command_log("ERROR", message.c_str());
}

// =====================================================
// 内部函数声明
// =====================================================
static bool command_execute_action(const CommandMessage &cmd, JsonDocument &response);
static bool command_execute_workflow(const CommandMessage &cmd, JsonDocument &response);
static bool command_query_actions(JsonDocument &response);
static bool command_query_triggers(JsonDocument &response);
static bool command_query_workflows(JsonDocument &response);
static bool command_query_state(const CommandMessage &cmd, JsonDocument &response);
static bool command_query_capabilities(JsonDocument &response);
static bool command_query_action_registry(JsonDocument &response);
static bool command_query_trigger_registry(JsonDocument &response);
static bool command_query_workflow_registry(JsonDocument &response);
static bool execute_router(const CommandMessage &cmd, JsonDocument &response);
static bool query_router(const CommandMessage &cmd, JsonDocument &response);
static bool system_router(const CommandMessage &cmd, JsonDocument &response);
static void command_system_reboot(const CommandMessage &cmd);
static bool command_system_set_time(const CommandMessage &cmd, JsonDocument &response);
static bool command_system_weight_zero(const CommandMessage &cmd, JsonDocument &response);
static bool command_system_wifi_config(const CommandMessage &cmd, JsonDocument &response);

// Config 异步命令 handler（定义在文件后段，此处前向声明以便路由）
static bool command_config_query(const CommandMessage &cmd, JsonDocument &response);
static bool command_config_set(const CommandMessage &cmd, JsonDocument &response);
static bool command_config_save(const CommandMessage &cmd, JsonDocument &response);
static bool command_config_restart(const CommandMessage &cmd, JsonDocument &response);
static bool command_config_delete(const CommandMessage &cmd, JsonDocument &response);
static bool command_config_module(const CommandMessage &cmd, JsonDocument &response);
static bool command_config_reset(const CommandMessage &cmd, JsonDocument &response);
static bool command_config_backup(const CommandMessage &cmd, JsonDocument &response);

// ConfigManager 异步完成回调（定义在文件后段，此处前向声明以便注册）
static void command_config_completion(
    const char *cmd_id,
    const char *command,
    const char *object,
    const char *source,
    ConfigCommandType type,
    bool success,
    ConfigCommandError error,
    const JsonDocument &result,
    const char *message
);
static bool command_system_wifi_ap(const CommandMessage &cmd, JsonDocument &response);
static bool command_is_execute(const String &command);
static bool command_is_query(const String &command);
static bool command_is_system(const String &command);

// 异步回调接收者（WorkflowManager 按最终契约调用）
static void command_temp_action_callback(
    const String &cmd_id,
    uint32_t instance_id,
    WorkflowActionResult result
);
static void command_workflow_callback(
    const String &cmd_id,
    WorkflowState result
);

// 运行时管理
static CommandRuntime *command_runtime_insert(
    const CommandMessage &cmd,
    unsigned long timeout_ms
);
static CommandRuntime *command_runtime_find_by_cmd_id(const String &cmd_id);
static void command_runtime_release(CommandRuntime *rt);
static void command_runtime_report_timeout(CommandRuntime *rt);
static void command_runtime_scan_timeouts();

// =====================================================
// 运行时：插入记录（CommandMessage 所有权转移至 Runtime）
// 失败（队列满 / cmd_id 重复）时由 command_send_error 统一上报并返回 nullptr
// =====================================================
static CommandRuntime *command_runtime_insert(
    const CommandMessage &cmd,
    unsigned long timeout_ms
)
{
    // 1. 队列容量保护：满则拒绝新命令（不覆盖正在运行的记录）
    CommandRuntime *slot = nullptr;
    for (uint8_t i = 0; i < MAX_COMMAND_RUNTIME; i++) {
        if (!runtime_queue[i].active) {
            slot = &runtime_queue[i];
            break;
        }
    }
    if (slot == nullptr) {
        command_log("WARN", "Command runtime queue full");
        command_send_error(
            cmd,
            CMD_ERROR_QUEUE_FULL,
            "Command runtime queue full"
        );
        return nullptr;
    }

    // 2. cmd_id 重复保护
    if (cmd.cmd_id.length() > 0 &&
        command_runtime_find_by_cmd_id(cmd.cmd_id) != nullptr) {
        command_log("WARN", "Duplicate command_id rejected");
        command_send_error(
            cmd,
            CMD_ERROR_DUPLICATE_CMD_ID,
            "Duplicate command_id"
        );
        return nullptr;
    }

    // 3. 填充记录（CommandMessage 唯一所有权在本记录）
    slot->active = true;
    slot->cmd_id = cmd.cmd_id;
    slot->message = cmd;
    slot->state = COMMAND_STATE_PENDING;
    slot->create_time = millis();
    slot->start_ms = slot->create_time;
    slot->timeout_ms = timeout_ms;
    slot->instance_id = 0;
    slot->workflow_id = "";
    return slot;
}

// =====================================================
// 运行时：按 cmd_id 查找（回调匹配主键 / 对外查询接口）
// =====================================================
static CommandRuntime *command_runtime_find_by_cmd_id(const String &cmd_id)
{
    if (cmd_id.length() == 0) {
        return nullptr;
    }
    for (uint8_t i = 0; i < MAX_COMMAND_RUNTIME; i++) {
        if (runtime_queue[i].active &&
            runtime_queue[i].cmd_id == cmd_id) {
            return &runtime_queue[i];
        }
    }
    return nullptr;
}

// =====================================================
// 运行时：释放记录（CommandMessage 随记录销毁，不复制到其他模块）
// =====================================================
static void command_runtime_release(CommandRuntime *rt)
{
    if (rt == nullptr) return;
    rt->active = false;
    rt->cmd_id = "";
    rt->message.command = "";
    rt->message.object = "";
    rt->message.cmd_id = "";
    rt->message.payload = "";
    rt->message.source = "";
    rt->message.timestamp = 0;
    rt->state = COMMAND_STATE_PENDING;
    rt->create_time = 0;
    rt->start_ms = 0;
    rt->timeout_ms = 0;
    rt->instance_id = 0;
    rt->workflow_id = "";
}

// =====================================================
// 运行时：超时结果上报并释放记录
// 后续到达的迟到回调将因 cmd_id 不存在而被忽略
// =====================================================
static void command_runtime_report_timeout(CommandRuntime *rt)
{
    command_log("WARN", "Command runtime timeout, entry released");

    JsonDocument doc;
    doc["cmd"] = "result";
    doc["id"] = rt->cmd_id;
    if (rt->message.command == "execute_action") {
        doc["type"] = "action_result";
        doc["action"] = rt->message.object;
        doc["command_id"] = rt->cmd_id;
        doc["status"] = "timeout";
        doc["instance_id"] = rt->instance_id;
    } else {
        doc["type"] = "workflow_result";
        doc["workflow"] = rt->workflow_id.length() > 0
                            ? rt->workflow_id
                            : rt->message.object;
        doc["command_id"] = rt->cmd_id;
        doc["status"] = "timeout";
    }

    time_t ts = get_unix_timestamp();
    if (ts > 0) {
        doc["timestamp"] = ts;
    }

    String json;
    serializeJson(doc, json);
    command_report_result(json);

    command_runtime_release(rt);
}

// =====================================================
// 运行时：超时扫描（command_manager_task 每 1s 调用一次）
// 只扫描 CommandRuntime，不查询 Workflow/Action 状态
// =====================================================
static void command_runtime_scan_timeouts()
{
    unsigned long now = millis();
    for (uint8_t i = 0; i < MAX_COMMAND_RUNTIME; i++) {
        CommandRuntime *rt = &runtime_queue[i];
        if (!rt->active) continue;
        if (rt->state != COMMAND_STATE_PENDING) continue;
        // Arduino 标准无符号减法，回绕安全
        if (now - rt->start_ms >= rt->timeout_ms) {
            command_runtime_report_timeout(rt);
        }
    }
}

// =====================================================
// 初始化
// =====================================================
void command_manager_init()
{
    last_result = CMD_RESULT_OK;
    for (uint8_t i = 0; i < MAX_COMMAND_RUNTIME; i++) {
        command_runtime_release(&runtime_queue[i]);
    }
    last_scan_ms = 0;

    // 注册 ConfigManager 完成回调。
    //
    // 配置命令是异步的: 本模块入队后立即返回 accepted，
    // 真正的执行结果由 ConfigManager 在 config_task() 中
    // 经该回调交回，再由本模块上报云端。
    config_set_completion_callback(command_config_completion);
}

// =====================================================
// 主任务（loop 调用）
//
// 仅做 CommandRuntime 超时扫描（1s 节流）。
// 禁止: 轮询 Workflow / Action、调用 workflow_temp_action_is_complete()
// =====================================================
void command_manager_task()
{
    if(reboot_pending){
        if(millis() - reboot_start_ms >= REBOOT_WAIT_TIMEOUT_MS){
            ESP.restart();
        }
    }
    
    unsigned long now = millis();
    if (now - last_scan_ms >= COMMAND_RUNTIME_SCAN_INTERVAL_MS) {
        last_scan_ms = now;
        command_runtime_scan_timeouts();
    }
}

//判断命令是否为异步命令（execute_action / execute_workflow）   
static bool command_is_async(const String &command)
{
    return (
        command == "execute_action" ||
        command == "execute_workflow"
    );
}

// 一级路由分类
static bool command_is_execute(const String &command)
{
    return command.startsWith("execute_");
}

static bool command_is_query(const String &command)
{
    return command.startsWith("query_");
}

static bool command_is_system(const String &command)
{
    return command == "system";
}

// =====================================================
// 接收并执行命令
//
// 输入: 已解析的 CommandMessage（payload 原样，不解析）
// 输出: true=路由成功 / false=失败（统一错误已上报）
//
// 一级路由:
//   execute -> execute_router
//   query   -> query_router
//   system  -> system_router
// =====================================================
bool command_manager_execute(const CommandMessage &cmd)
{
    const String command = cmd.command;
    command_log(
        "INFO",
        "Received command"
    );
    // =====================================================
    // 命令字段检查
    // =====================================================
    if(command.length() == 0)
    {
        command_send_error(
            cmd,
            CMD_ERROR_MISSING_COMMAND,
            "Missing 'command' field"
        );
        return false;
    }

    // =====================================================
    // 路由分发
    // =====================================================
    JsonDocument response;
    bool success = false;
    if (command_is_execute(command)) {
        success = execute_router(cmd, response);
    } else if (command_is_query(command)) {
        success = query_router(cmd, response);
    } else if (command_is_system(command)) {
        success = system_router(cmd, response);
    } else {
        String msg = "Unknown command: ";
        msg += command;
        command_send_error(
            cmd,
            CMD_ERROR_UNKNOWN_COMMAND,
            msg
        );
        return false;
    }

    if (!success) {
        // 错误已由 command_send_error 统一上报，此处不再重复上报
        return false;
    }

    // =====================================================
    // 通用字段（ACK + RESULT 双阶段：本层统一返回 cmd=result）
    // =====================================================
    response["cmd"] = "result";
    response["id"] = cmd.cmd_id;
    response["command"] = command;

    time_t ts =
        get_unix_timestamp();
    if(ts > 0)
    {
        response["timestamp"] = ts;
    }
    // =====================================================
    // 序列化结果
    // =====================================================
    String output;
    serializeJson(
        response,
        output
    );
    // =====================================================
    // 所有成功命令立即返回 RESULT
    //
    // execute:
    //   RESULT accepted
    //   后续 callback 返回最终结果
    //
    // query/system:
    //   RESULT success
    // =====================================================
    command_report_result(output);
    command_log(
        "INFO",
        "Command executed"
    );
    return true;
}

// =====================================================
// 获取最近执行结果
// =====================================================
CommandResult command_manager_last_result()
{
    return last_result;
}

// =====================================================
// 清除状态（仅复位同步应答结果；运行中的记录仍由回调/超时完成）
// =====================================================
void command_manager_clear()
{
    last_result = CMD_RESULT_OK;
    command_log("INFO", "Command cleared");
}

// =====================================================
// cmd_id 查询接口
//
// 供 WorkflowManager / Action 等模块查询当前有效命令。
// 只读查询：不创建、不复制；数据生命周期归 CommandManager。
// =====================================================
CommandRuntime *command_manager_get_runtime(const String &cmd_id)
{
    return command_runtime_find_by_cmd_id(cmd_id);
}

const CommandMessage *command_manager_get_message(const String &cmd_id)
{
    CommandRuntime *rt = command_runtime_find_by_cmd_id(cmd_id);
    return (rt != nullptr) ? &rt->message : nullptr;
}

// =====================================================
// 执行 Action
//
// 流程:
//   1. 校验 action id（cmd.object）
//   2. 创建 CommandRuntime（CommandMessage 归记录所有，不解析）
//   3. 调用 WorkflowManager 最终入队接口（携带 cmd_id + 回调）
//   4. 返回 accepted；完成由回调/超时路径处理
// =====================================================
static bool command_execute_action(const CommandMessage &cmd, JsonDocument &response)
{
    // 1. 校验目标 action id
    if (cmd.object.length() == 0) {
        command_send_error(
            cmd,
            CMD_ERROR_MISSING_OBJECT,
            "Missing 'object' (action id)"
        );
        return false;
    }

    // 2. 创建运行时记录
    CommandRuntime *rt = command_runtime_insert(
        cmd,
        COMMAND_ACTION_TIMEOUT_MS
    );
    if (rt == nullptr) {
        // 队列满 / cmd_id 重复：错误已由 command_send_error 统一上报
        return false;
    }

    // 3. 调用 WorkflowManager 最终接口（cmd_id 关联 + CommandManager 回调契约）
    //    payload 由 WorkflowManager 自行转换为 Action 参数，CommandManager 不解析。
    uint32_t instance_id = 0;
    bool queued = workflow_enqueue_action(
        rt->message.object,     // action id
        rt->message.payload,    // 原始 payload
        &instance_id,
        rt->cmd_id,             // 关联 ID
        command_temp_action_callback,
        rt->timeout_ms
    );

    if (!queued) {
        command_send_error(
            cmd,
            CMD_ERROR_EXECUTION,
            "Failed to queue action"
        );
        command_runtime_release(rt);
        return false;
    }

    // 回填 WorkflowManager 域实例号（回调校验用）
    rt->instance_id = instance_id;

    response["status"] = "accepted";
    response["message"] = "Action queued";
    response["action"] = rt->message.object;
    response["instance_id"] = instance_id;
    last_result = CMD_RESULT_RUNNING;
    return true;
}

// =====================================================
// 执行 Workflow（通过 cmd.object 指定 workflow id）
//
// 流程:
//   1. 校验并查找 Workflow
//   2. 创建 CommandRuntime
//   3. 调用 WorkflowManager 最终启动接口（cmd_id + WorkflowResultCallback）
//   4. 返回 accepted；完成由回调/超时路径处理
// =====================================================
static bool command_execute_workflow(const CommandMessage &cmd, JsonDocument &response)
{
    if (cmd.object.length() == 0) {
        command_send_error(
            cmd,
            CMD_ERROR_MISSING_OBJECT,
            "Missing 'object' (workflow id)"
        );
        return false;
    }

    Workflow *wf = nullptr;
    for (uint8_t i = 0; i < workflow_get_count(); i++) {
        Workflow *w = workflow_get(i);
        if (w != nullptr && w->id == cmd.object) {
            wf = w;
            break;
        }
    }

    if (wf == nullptr) {
        String msg = "Workflow not found: ";
        msg += cmd.object;
        command_send_error(
            cmd,
            CMD_ERROR_WORKFLOW_NOT_FOUND,
            msg
        );
        return false;
    }

    CommandRuntime *rt = command_runtime_insert(
        cmd,
        COMMAND_WORKFLOW_TIMEOUT_MS
    );
    if (rt == nullptr) {
        // 队列满 / cmd_id 重复：错误已由 command_send_error 统一上报
        return false;
    }
    rt->workflow_id = wf->id;

    // 调用 WorkflowManager 最终接口（cmd_id 关联 + 完成回调）
    bool started = workflow_start(
        wf,
        true,
        rt->cmd_id,
        command_workflow_callback
    );

    if (!started) {
        command_send_error(
            cmd,
            CMD_ERROR_EXECUTION,
            "Failed to start workflow"
        );
        command_runtime_release(rt);
        return false;
    }

    response["status"] = "accepted";
    response["message"] = "Workflow started";
    response["workflow"] = wf->id;
    last_result = CMD_RESULT_RUNNING;
    return true;
}

// =====================================================
// Action 完成回调（最终契约）
//
// 流程:
//   1. 按 cmd_id 查找运行时记录（主键）
//   2. 校验 instance_id
//   3. 生成 action_result JSON
//   4. command_report_result() 上报
//   5. 释放记录（迟到回调因 cmd_id 不存在而被忽略）
// =====================================================
static void command_temp_action_callback(
    const String &cmd_id,
    uint32_t instance_id,
    WorkflowActionResult result
)
{
    CommandRuntime *rt = command_runtime_find_by_cmd_id(cmd_id);
    if (rt == nullptr) {
        command_log("WARN", "Action callback for unknown cmd_id, ignored");
        return;
    }

    if (rt->instance_id != instance_id) {
        command_log("WARN", "Action callback mismatch (instance_id), ignored");
        return;
    }

    JsonDocument doc;
    doc["cmd"] = "result";
    doc["id"] = cmd_id;
    doc["type"] = "action_result";
    doc["action"] = rt->message.object;
    doc["command_id"] = cmd_id;
    doc["instance_id"] = instance_id;

    if (result == ACTION_SUCCESS) {
        doc["status"] = "success";
        rt->state = COMMAND_STATE_SUCCESS;
    } else {
        doc["status"] = "failed";
        rt->state = COMMAND_STATE_FAILED;
    }

    time_t ts = get_unix_timestamp();
    if (ts > 0) {
        doc["timestamp"] = ts;
    }

    String json;
    serializeJson(doc, json);
    command_report_result(json);

    command_runtime_release(rt);
}

// =====================================================
// Workflow 完成回调（最终契约）
// =====================================================
static void command_workflow_callback(
    const String &cmd_id,
    WorkflowState result
)
{
    CommandRuntime *rt = command_runtime_find_by_cmd_id(cmd_id);
    if (rt == nullptr) {
        command_log("WARN", "Workflow callback for unknown cmd_id, ignored");
        return;
    }

    JsonDocument doc;
    doc["cmd"] = "result";
    doc["id"] = cmd_id;
    doc["type"] = "workflow_result";
    doc["workflow"] = rt->workflow_id.length() > 0
                        ? rt->workflow_id
                        : rt->message.object;
    doc["command_id"] = cmd_id;

    if (result == WORKFLOW_FINISHED) {
        doc["status"] = "success";
        rt->state = COMMAND_STATE_SUCCESS;
    } else if (result == WORKFLOW_TIMEOUT) {
        doc["status"] = "timeout";
        rt->state = COMMAND_STATE_TIMEOUT;
    } else {
        doc["status"] = "failed";
        rt->state = COMMAND_STATE_FAILED;
    }

    time_t ts = get_unix_timestamp();
    if (ts > 0) {
        doc["timestamp"] = ts;
    }

    String json;
    serializeJson(doc, json);
    command_report_result(json);

    command_runtime_release(rt);
}

// =====================================================
// 查询 Action Registry
// 数据来源:capability_registry RAM cache
// =====================================================
static bool command_query_action_registry(JsonDocument &response)
{
    response["registry"] = "action";
    response["version"] = capability_get_action_version();
    response["checksum"] = capability_get_action_checksum();
    response["count"] = capability_get_action_count();

    JsonArray entries = response["entries"].to<JsonArray>();

    for(uint8_t i = 0;
        i < capability_get_action_count();
        i++)
    {
        String runtime_id;

        if(!capability_get_action_by_stable_id(
                i,
                runtime_id))
        {
            continue;
        }

        JsonObject item = entries.add<JsonObject>();

        item["stable_id"] = i;
        item["runtime_id"] = runtime_id;
    }

    response["status"] = "success";

    last_result = CMD_RESULT_OK;

    return true;
}
// =====================================================
// 查询 Trigger Registry
// 数据来源:
// capability_registry RAM cache
// =====================================================
static bool command_query_trigger_registry(JsonDocument &response)
{
    response["registry"] = "trigger";
    response["version"] = capability_get_trigger_version();
    response["checksum"] = capability_get_trigger_checksum();
    response["count"] = capability_get_trigger_count();

    JsonArray entries = response["entries"].to<JsonArray>();

    for(uint8_t i = 0;
        i < capability_get_trigger_count();
        i++)
    {
        String runtime_id;

        if(!capability_get_trigger_by_stable_id(
                i,
                runtime_id))
        {
            continue;
        }

        JsonObject item = entries.add<JsonObject>();

        item["stable_id"] = i;
        item["runtime_id"] = runtime_id;
    }

    response["status"] = "success";

    last_result = CMD_RESULT_OK;

    return true;
}
// =====================================================
// 查询 Action Registry
// 数据来源:capability_registry RAM cache
// =====================================================
static bool command_query_workflow_registry(JsonDocument &response)
{
    response["registry"] = "workflow";
    response["version"] = capability_get_workflow_version();
    response["checksum"] = capability_get_workflow_checksum();
    response["count"] = capability_get_workflow_count();

    JsonArray entries = response["entries"].to<JsonArray>();

    for(uint8_t i = 0;
        i < capability_get_workflow_count();
        i++)
    {
        String runtime_id;

        if(!capability_get_workflow_by_stable_id(
                i,
                runtime_id))
        {
            continue;
        }

        JsonObject item = entries.add<JsonObject>();

        item["stable_id"] = i;
        item["runtime_id"] = runtime_id;
    }

    response["status"] = "success";

    last_result = CMD_RESULT_OK;

    return true;
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
            item["description"] = desc->description;
        }
        // stable_id 统一来自 Capability Registry（不重新扫描 workflow）
        uint8_t stable_id = 0;
        if (capability_get_action_stable_id(desc->id, stable_id)) {
            item["stable_id"] = stable_id;
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
        // stable_id 统一来自 Capability Registry（不重新扫描 workflow）
        uint8_t stable_id = 0;
        if (capability_get_trigger_stable_id(desc->id, stable_id)) {
            item["stable_id"] = stable_id;
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
        // stable_id 统一来自 Capability Registry（不重新扫描 workflow）
        uint8_t stable_id = 0;
        if (capability_get_workflow_stable_id(wf->id, stable_id)) {
            item["stable_id"] = stable_id;
        }

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
// 目标 key 由 cmd.object 提供（Phase 3: 云侧把查询 key 放入 ob）。
// payload 不解析。
// =====================================================
static bool command_query_state(const CommandMessage &cmd, JsonDocument &response)
{
    if (cmd.object.length() == 0) {
        command_send_error(
            cmd,
            CMD_ERROR_MISSING_OBJECT,
            "Missing 'object' (state key)"
        );
        return false;
    }

    String value;
    if (system_state_query(cmd.object, value)) {
        Serial.printf(
            "[Command] query state: %s\n",
            cmd.object.c_str()
        );
        response["status"] = "success";
        response["key"] = cmd.object;
        response["value"] = value;
        last_result = CMD_RESULT_OK;
        return true;
    }

    String msg = "State not found: ";
    msg += cmd.object;
    command_send_error(
        cmd,
        CMD_ERROR_EXECUTION,
        msg
    );
    return false;
}

// =====================================================
// 查询 Capability Registry（新增）
//
// 数据来源: capability_registry API（RAM Cache）
// 禁止重新扫描 workflow registry。
//
// 返回:
//   action / trigger / workflow 每项包含:
//     version / checksum / count
//     entries[]: { stable_id, runtime_id }
// =====================================================
static bool command_query_capabilities(JsonDocument &response)
{
    // ---- Action ----
    JsonObject action = response["action"].to<JsonObject>();
    action["version"] = capability_get_action_version();
    action["checksum"] = capability_get_action_checksum();
    action["count"] = capability_get_action_count();
    JsonArray action_entries = action["entries"].to<JsonArray>();
    for (uint8_t i = 0; i < capability_get_action_count(); i++) {
        String runtime_id;
        if (!capability_get_action_by_stable_id(i, runtime_id)) {
            continue;
        }
        JsonObject item = action_entries.add<JsonObject>();
        item["stable_id"] = i;
        item["runtime_id"] = runtime_id;
    }

    // ---- Trigger ----
    JsonObject trigger = response["trigger"].to<JsonObject>();
    trigger["version"] = capability_get_trigger_version();
    trigger["checksum"] = capability_get_trigger_checksum();
    trigger["count"] = capability_get_trigger_count();
    JsonArray trigger_entries = trigger["entries"].to<JsonArray>();
    for (uint8_t i = 0; i < capability_get_trigger_count(); i++) {
        String runtime_id;
        if (!capability_get_trigger_by_stable_id(i, runtime_id)) {
            continue;
        }
        JsonObject item = trigger_entries.add<JsonObject>();
        item["stable_id"] = i;
        item["runtime_id"] = runtime_id;
    }

    // ---- Workflow ----
    JsonObject workflow = response["workflow"].to<JsonObject>();
    workflow["version"] = capability_get_workflow_version();
    workflow["checksum"] = capability_get_workflow_checksum();
    workflow["count"] = capability_get_workflow_count();
    JsonArray workflow_entries = workflow["entries"].to<JsonArray>();
    for (uint8_t i = 0; i < capability_get_workflow_count(); i++) {
        String runtime_id;
        if (!capability_get_workflow_by_stable_id(i, runtime_id)) {
            continue;
        }
        JsonObject item = workflow_entries.add<JsonObject>();
        item["stable_id"] = i;
        item["runtime_id"] = runtime_id;
    }

    response["status"] = "success";
    last_result = CMD_RESULT_OK;
    return true;
}

// =====================================================
// 一级路由: execute
//
// 保留:
//   execute_action
//   execute_workflow
//
// 行为不变；失败必须通过 command_send_error 统一上报。
// =====================================================
static bool execute_router(
    const CommandMessage &cmd,
    JsonDocument &response
)
{
    const String &command = cmd.command;

    if (command == "execute_action") {
        return command_execute_action(cmd, response);
    }
    if (command == "execute_workflow") {
        return command_execute_workflow(cmd, response);
    }

    String msg = "Unknown execute command: ";
    msg += command;
    command_send_error(
        cmd,
        CMD_ERROR_UNKNOWN_COMMAND,
        msg
    );
    return false;
}

// =====================================================
// 一级路由: query
//
// 保留旧接口:
//   query_state / query_actions / query_triggers / query_workflows
//
// 新增:
//   query_capabilities
// =====================================================
static bool query_router(
    const CommandMessage &cmd,
    JsonDocument &response
)
{
    const String &command = cmd.command;

    if (command == "query_state") {
        return command_query_state(cmd, response);
    }
    if (command == "query_actions") {
        return command_query_actions(response);
    }
    if (command == "query_triggers") {
        return command_query_triggers(response);
    }
    if (command == "query_workflows") {
        return command_query_workflows(response);
    }
    if (command == "query_capabilities") {
        return command_query_capabilities(response);
    }
    if (command == "query_action_registry") {
        return command_query_action_registry(response);
    }
    if (command == "query_trigger_registry") {
        return command_query_trigger_registry(response);
    }
    if (command == "query_workflow_registry") {
        return command_query_workflow_registry(response);
    }

    String msg = "Unknown query command: ";
    msg += command;
    command_send_error(
        cmd,
        CMD_ERROR_UNKNOWN_COMMAND,
        msg
    );
    return false;
}

// =====================================================
// 一级路由: system
//
// 消息格式:
// {
//   "cmd":"system",
//   "ob":"reboot|set_time|weight_zero|wifi_config|wifi_ap",
//   "pl":{}
// }
// =====================================================
static bool system_router(
    const CommandMessage &cmd,
    JsonDocument &response
)
{
    const String &object = cmd.object;

    if (object == "reboot") {
        // reboot 流程内部上报 result 并立即 ESP.restart()，正常不可达
        command_system_reboot(cmd);
        return false;
    }
    if (object == "set_time") {
        return command_system_set_time(cmd, response);
    }
    if (object == "weight_zero") {
        return command_system_weight_zero(cmd, response);
    }
    if (object == "wifi_config") {
        return command_system_wifi_config(cmd, response);
    }
    if (object == "wifi_ap") {
        return command_system_wifi_ap(cmd, response);
    }
    // ---- Config 异步命令（入队后立即返回 accepted）----
    if (object == "config_query") {
        return command_config_query(cmd, response);
    }
    if (object == "config_set") {
        return command_config_set(cmd, response);
    }
    if (object == "config_save") {
        return command_config_save(cmd, response);
    }
    if (object == "config_restart") {
        return command_config_restart(cmd, response);
    }
    if (object == "config_delete") {
        return command_config_delete(cmd, response);
    }
    if (object == "config_module") {
        return command_config_module(cmd, response);
    }
    if (object == "config_reset") {
        return command_config_reset(cmd, response);
    }
    if (object == "config_backup") {
        return command_config_backup(cmd, response);
    }

    String msg = "Unknown system object: ";
    msg += object;
    command_send_error(
        cmd,
        CMD_ERROR_UNKNOWN_COMMAND,
        msg
    );
    return false;
}

// =====================================================
// system: reboot
//
// 流程:
//   1. 上报 result: status=rebooting
//   2. MQTT publish（当前 PubSubClient QoS0；QoS1 为后续独立任务）
//   3. 不等待云端 ACK
//   4. 立即执行 ESP.restart()
//
// 禁止使用 delay 模拟等待。
// =====================================================
static void command_system_reboot(const CommandMessage &cmd)
{
    JsonDocument doc;
    doc["cmd"] = "result";
    doc["id"] = cmd.cmd_id;
    doc["type"] = "command";
    doc["status"] = "rebooting";

    time_t ts = get_unix_timestamp();
    if (ts > 0) {
        doc["timestamp"] = ts;
    }

    String json;
    serializeJson(doc, json);
    command_report_result(json);
    command_log("INFO", "System reboot requested");
    reboot_pending = true;
    reboot_start_ms = millis();
}

// =====================================================
// system: set_time（预留接口）
//
// CommandManager 只负责路由；
// 未来由 time_manager 实现（当前为空实现，仅保证编译）。
// =====================================================
static bool command_system_set_time(
    const CommandMessage &cmd,
    JsonDocument &response
)
{
    time_manager_set_time();

    response["status"] = "success";
    response["message"] = "set_time reserved";
    last_result = CMD_RESULT_OK;
    return true;
}

// =====================================================
// system: weight_zero_calibrate
//
// object: weight_zero
// 调用 weight_manager 现有接口，CommandManager 只负责调用。
// =====================================================
static bool command_system_weight_zero(
    const CommandMessage &cmd,
    JsonDocument &response
)
{
    if(!weight_zero_calibrate())
    {
        command_send_error(
            cmd,
            CMD_ERROR_SYSTEM,
            "weight zero calibration failed"
        );
        return false;
    }
    response["status"] = "accepted";
    response["message"] = "weight zero calibration started";
    last_result = CMD_RESULT_OK;
    return true;
}

// =====================================================
// system: wifi_config
//
// object: wifi_config
// pl: { "ssid":"...", "password":"..." }
//
// CommandManager 负责解析参数并调用 config_manager 接口。
// =====================================================
static bool command_system_wifi_config(
    const CommandMessage &cmd,
    JsonDocument &response
)
{
    JsonDocument pl;
    DeserializationError error =
        deserializeJson(pl, cmd.payload);
    if (error) {
        command_send_error(
            cmd,
            CMD_ERROR_PARAM,
            "wifi_config payload invalid"
        );
        return false;
    }

    String ssid = pl["ssid"] | "";
    String password = pl["password"] | "";
    if (ssid.length() == 0) {
        command_send_error(
            cmd,
            CMD_ERROR_PARAM,
            "wifi_config missing 'ssid'"
        );
        return false;
    }

    if (!config_update_wifi(ssid, password)) {
        command_send_error(
            cmd,
            CMD_ERROR_SYSTEM,
            "config_update_wifi failed"
        );
        return false;
    }

    response["status"] = "success";
    response["message"] = "wifi config updated";
    last_result = CMD_RESULT_OK;
    return true;
}

// =====================================================
// system: wifi_ap（入口预留）
//
// 当前只建立 command 入口，暂不实现 AP 逻辑。
// =====================================================
static bool command_system_wifi_ap(
    const CommandMessage &cmd,
    JsonDocument &response
)
{
    response["status"] = "success";
    response["message"] = "wifi_ap reserved (not implemented)";
    last_result = CMD_RESULT_OK;
    return true;
}

// =====================================================
// Config 异步命令（system 命令族）
//
// 完整链路:
//   MQTT RX
//     -> CommandManager 解析
//     -> 创建 CommandRuntime
//     -> config_cmd_enqueue_xxx()        （只入队，不执行文件 IO）
//     -> 立即上报 accepted
//     -> ConfigManager::config_task() 真正执行
//     -> command_config_completion()
//     -> 上报最终 result -> CloudManager -> MQTT UP
//
// 本文件严禁直接调用:
//   config_set_xxx() / config_save() / ESP.restart()
// 配置修改与落盘一律走 config_cmd_enqueue_xxx()。
// =====================================================

// 配置命令的运行时超时。
// ConfigManager 队列最多 8 个任务、单次 IO 为毫秒级，1 分钟足够宽松。
static const unsigned long COMMAND_CONFIG_TIMEOUT_MS = 60000UL;

// 入队失败 -> 统一错误上报
static bool config_enqueue_report_error(
    const CommandMessage &cmd,
    ConfigEnqueueResult ret
)
{
    switch (ret)
    {
        case CONFIG_ENQUEUE_QUEUE_FULL:
            command_send_error(
                cmd,
                CMD_ERROR_QUEUE_FULL,
                "config command queue full"
            );
            return false;

        case CONFIG_ENQUEUE_INVALID:
            command_send_error(
                cmd,
                CMD_ERROR_PARAM,
                "invalid config param or unsupported value type"
            );
            return false;

        case CONFIG_ENQUEUE_UNAVAILABLE:
            command_send_error(
                cmd,
                CMD_ERROR_SYSTEM,
                "config manager unavailable"
            );
            return false;

        default:
            command_send_error(
                cmd,
                CMD_ERROR_SYSTEM,
                "config enqueue failed"
            );
            return false;
    }
}

// 解析 payload（允许为空）
static bool config_parse_payload(
    const CommandMessage &cmd,
    JsonDocument &pl
)
{
    if (cmd.payload.length() == 0)
    {
        return true;   // 无参数，合法
    }

    if (deserializeJson(pl, cmd.payload))
    {
        command_send_error(
            cmd,
            CMD_ERROR_PARAM,
            "config payload is not valid JSON"
        );
        return false;
    }

    return true;
}

// system: config_query
//
// payload:
//   {}                              -> 查询全部模块
//   {"module":"wifi"}               -> 查询整个模块
//   {"module":"wifi","key":"ssid"}  -> 查询单个字段
//   {"keys_only":true}              -> 只返回模块名/字段名（不含值）
static bool command_config_query(
    const CommandMessage &cmd,
    JsonDocument &response
)
{
    JsonDocument pl;
    if (!config_parse_payload(cmd, pl))
    {
        return false;
    }

    String module = pl["module"] | "";
    String key    = pl["key"] | "";
    bool keys_only = pl["keys_only"] | false;

    // 先占 runtime 槽位（内部处理 cmd_id 去重与队列满）
    CommandRuntime *rt =
        command_runtime_insert(cmd, COMMAND_CONFIG_TIMEOUT_MS);
    if (rt == nullptr)
    {
        return false;   // 错误已由 command_runtime_insert 统一上报
    }

    ConfigEnqueueResult ret;

    if (module.length() == 0)
    {
        ret = config_cmd_enqueue_query_all(
            rt->cmd_id.c_str(),
            cmd.command.c_str(),
            cmd.object.c_str(),
            cmd.source.c_str(),
            keys_only
        );
    }
    else if (key.length() == 0)
    {
        ret = config_cmd_enqueue_query_module(
            rt->cmd_id.c_str(),
            cmd.command.c_str(),
            cmd.object.c_str(),
            cmd.source.c_str(),
            module.c_str(),
            keys_only
        );
    }
    else
    {
        ret = config_cmd_enqueue_query_field(
            rt->cmd_id.c_str(),
            cmd.command.c_str(),
            cmd.object.c_str(),
            cmd.source.c_str(),
            module.c_str(),
            key.c_str()
        );
    }

    if (ret != CONFIG_ENQUEUE_ACCEPTED)
    {
        // 入队失败：立即归还槽位，并上报明确错误
        command_runtime_release(rt);
        return config_enqueue_report_error(cmd, ret);
    }

    // 第一阶段：只表示"已收下"，不是最终成功
    response["status"] = "accepted";
    response["message"] = "config query queued";
    response["command_id"] = rt->cmd_id;
    last_result = CMD_RESULT_ACCEPTED;
    return true;
}

// system: config_set
//
// payload: {"module":"wifi","key":"ssid","value":"new_ssid"}
//
// value 只支持标量 (int / bool / float / string)；
// null / object / array 会在入队阶段被 ConfigManager 拒绝。
static bool command_config_set(
    const CommandMessage &cmd,
    JsonDocument &response
)
{
    JsonDocument pl;
    if (!config_parse_payload(cmd, pl))
    {
        return false;
    }

    String module = pl["module"] | "";
    String key    = pl["key"] | "";
    JsonVariantConst value = pl["value"];

    // 乐观锁: expect_version 缺省时（is<int> 为 false）→ -1 表示不校验
    int expect_version = pl["expect_version"].is<int>()
                       ? pl["expect_version"].as<int>()
                       : -1;

    if (module.length() == 0 || key.length() == 0)
    {
        command_send_error(
            cmd,
            CMD_ERROR_PARAM,
            "config_set requires module and key"
        );
        return false;
    }

    if (value.isNull() || value.isUnbound())
    {
        command_send_error(
            cmd,
            CMD_ERROR_PARAM,
            "config_set requires a non-null value"
        );
        return false;
    }

    CommandRuntime *rt =
        command_runtime_insert(cmd, COMMAND_CONFIG_TIMEOUT_MS);
    if (rt == nullptr)
    {
        return false;
    }

    ConfigEnqueueResult ret = config_cmd_enqueue_set_field(
        rt->cmd_id.c_str(),
        cmd.command.c_str(),
        cmd.object.c_str(),
        cmd.source.c_str(),
        module.c_str(),
        key.c_str(),
        value,
        expect_version
    );

    if (ret != CONFIG_ENQUEUE_ACCEPTED)
    {
        command_runtime_release(rt);
        return config_enqueue_report_error(cmd, ret);
    }

    response["status"] = "accepted";
    response["message"] = "config set queued, restart required";
    response["command_id"] = rt->cmd_id;
    last_result = CMD_RESULT_ACCEPTED;
    return true;
}

// system: config_save
//
// payload: {}（无参数）
static bool command_config_save(
    const CommandMessage &cmd,
    JsonDocument &response
)
{
    JsonDocument pl;
    if (!config_parse_payload(cmd, pl))
    {
        return false;
    }

    CommandRuntime *rt =
        command_runtime_insert(cmd, COMMAND_CONFIG_TIMEOUT_MS);
    if (rt == nullptr)
    {
        return false;
    }

    ConfigEnqueueResult ret = config_cmd_enqueue_save(
        rt->cmd_id.c_str(),
        cmd.command.c_str(),
        cmd.object.c_str(),
        cmd.source.c_str()
    );

    if (ret != CONFIG_ENQUEUE_ACCEPTED)
    {
        command_runtime_release(rt);
        return config_enqueue_report_error(cmd, ret);
    }

    response["status"] = "accepted";
    response["message"] = "config save queued";
    response["command_id"] = rt->cmd_id;
    last_result = CMD_RESULT_ACCEPTED;
    return true;
}

// system: config_restart
//
// payload:
//   {}               -> 安排重启（ConfigManager 先回调成功，再重启）
//   {"cancel":true}  -> 取消待重启
static bool command_config_restart(
    const CommandMessage &cmd,
    JsonDocument &response
)
{
    JsonDocument pl;
    if (!config_parse_payload(cmd, pl))
    {
        return false;
    }

    bool cancel = pl["cancel"] | false;

    CommandRuntime *rt =
        command_runtime_insert(cmd, COMMAND_CONFIG_TIMEOUT_MS);
    if (rt == nullptr)
    {
        return false;
    }

    ConfigEnqueueResult ret = config_cmd_enqueue_restart(
        rt->cmd_id.c_str(),
        cmd.command.c_str(),
        cmd.object.c_str(),
        cmd.source.c_str(),
        cancel
    );

    if (ret != CONFIG_ENQUEUE_ACCEPTED)
    {
        command_runtime_release(rt);
        return config_enqueue_report_error(cmd, ret);
    }

    response["status"] = "accepted";
    response["message"] = cancel
                        ? "restart cancel queued"
                        : "restart queued";
    response["command_id"] = rt->cmd_id;
    last_result = CMD_RESULT_ACCEPTED;
    return true;
}

// system: config_delete
//
// payload: {"module":"weight","key":"skc"}
//
// 真正从配置中移除该字段（不是置默认值）。
// 字段不存在会在执行阶段返回 key not found。
static bool command_config_delete(
    const CommandMessage &cmd,
    JsonDocument &response
)
{
    JsonDocument pl;
    if (!config_parse_payload(cmd, pl))
    {
        return false;
    }

    String module = pl["module"] | "";
    String key    = pl["key"] | "";

    int expect_version = pl["expect_version"].is<int>()
                       ? pl["expect_version"].as<int>()
                       : -1;

    if (module.length() == 0 || key.length() == 0)
    {
        command_send_error(
            cmd,
            CMD_ERROR_PARAM,
            "config_delete requires module and key"
        );
        return false;
    }

    CommandRuntime *rt =
        command_runtime_insert(cmd, COMMAND_CONFIG_TIMEOUT_MS);
    if (rt == nullptr)
    {
        return false;
    }

    ConfigEnqueueResult ret = config_cmd_enqueue_delete_field(
        rt->cmd_id.c_str(),
        cmd.command.c_str(),
        cmd.object.c_str(),
        cmd.source.c_str(),
        module.c_str(),
        key.c_str(),
        expect_version
    );

    if (ret != CONFIG_ENQUEUE_ACCEPTED)
    {
        command_runtime_release(rt);
        return config_enqueue_report_error(cmd, ret);
    }

    response["status"] = "accepted";
    response["message"] = "config delete queued, restart required";
    response["command_id"] = rt->cmd_id;
    last_result = CMD_RESULT_ACCEPTED;
    return true;
}

// system: config_module
//
// payload: {"module":"weight","value":{"dt":6,"sck":7,...}}
//
// 批量写入模块字段。value 必须是 object。
// 执行阶段做严格校验：传入的每个字段都必须已存在，
// 任一不存在则整体拒绝（result 中返回原因）。
static bool command_config_module(
    const CommandMessage &cmd,
    JsonDocument &response
)
{
    JsonDocument pl;
    if (!config_parse_payload(cmd, pl))
    {
        return false;
    }

    String module = pl["module"] | "";
    JsonVariantConst value = pl["value"];

    int expect_version = pl["expect_version"].is<int>()
                       ? pl["expect_version"].as<int>()
                       : -1;

    if (module.length() == 0)
    {
        command_send_error(
            cmd,
            CMD_ERROR_PARAM,
            "config_module requires module"
        );
        return false;
    }

    if (value.isNull() || value.isUnbound())
    {
        command_send_error(
            cmd,
            CMD_ERROR_PARAM,
            "config_module requires a non-null value"
        );
        return false;
    }

    CommandRuntime *rt =
        command_runtime_insert(cmd, COMMAND_CONFIG_TIMEOUT_MS);
    if (rt == nullptr)
    {
        return false;
    }

    ConfigEnqueueResult ret = config_cmd_enqueue_set_module(
        rt->cmd_id.c_str(),
        cmd.command.c_str(),
        cmd.object.c_str(),
        cmd.source.c_str(),
        module.c_str(),
        value,
        expect_version
    );

    if (ret != CONFIG_ENQUEUE_ACCEPTED)
    {
        command_runtime_release(rt);
        return config_enqueue_report_error(cmd, ret);
    }

    response["status"] = "accepted";
    response["message"] = "config module queued, restart required";
    response["command_id"] = rt->cmd_id;
    last_result = CMD_RESULT_ACCEPTED;
    return true;
}

// system: config_reset
//
// payload: {"module":"weight"}
//
// 从 factory 副本恢复模块（完全替换为 factory 内容）。
// factory 不存在时返回 CONFIG_ERR_FACTORY_MISSING。
static bool command_config_reset(
    const CommandMessage &cmd,
    JsonDocument &response
)
{
    JsonDocument pl;
    if (!config_parse_payload(cmd, pl))
    {
        return false;
    }

    String module = pl["module"] | "";

    if (module.length() == 0)
    {
        command_send_error(
            cmd,
            CMD_ERROR_PARAM,
            "config_reset requires module"
        );
        return false;
    }

    CommandRuntime *rt =
        command_runtime_insert(cmd, COMMAND_CONFIG_TIMEOUT_MS);
    if (rt == nullptr)
    {
        return false;
    }

    ConfigEnqueueResult ret = config_cmd_enqueue_reset_module(
        rt->cmd_id.c_str(),
        cmd.command.c_str(),
        cmd.object.c_str(),
        cmd.source.c_str(),
        module.c_str()
    );

    if (ret != CONFIG_ENQUEUE_ACCEPTED)
    {
        command_runtime_release(rt);
        return config_enqueue_report_error(cmd, ret);
    }

    response["status"] = "accepted";
    response["message"] = "config reset queued, restart required";
    response["command_id"] = rt->cmd_id;
    last_result = CMD_RESULT_ACCEPTED;
    return true;
}

// system: config_backup
//
// payload: {"module":"weight"}
//
// 把当前模块备份为 factory 副本（覆盖原有副本）。
// 只写 /factory/<module>.json，不改动当前配置。
static bool command_config_backup(
    const CommandMessage &cmd,
    JsonDocument &response
)
{
    JsonDocument pl;
    if (!config_parse_payload(cmd, pl))
    {
        return false;
    }

    String module = pl["module"] | "";

    if (module.length() == 0)
    {
        command_send_error(
            cmd,
            CMD_ERROR_PARAM,
            "config_backup requires module"
        );
        return false;
    }

    CommandRuntime *rt =
        command_runtime_insert(cmd, COMMAND_CONFIG_TIMEOUT_MS);
    if (rt == nullptr)
    {
        return false;
    }

    ConfigEnqueueResult ret = config_cmd_enqueue_backup_module(
        rt->cmd_id.c_str(),
        cmd.command.c_str(),
        cmd.object.c_str(),
        cmd.source.c_str(),
        module.c_str()
    );

    if (ret != CONFIG_ENQUEUE_ACCEPTED)
    {
        command_runtime_release(rt);
        return config_enqueue_report_error(cmd, ret);
    }

    response["status"] = "accepted";
    response["message"] = "config backup queued";
    response["command_id"] = rt->cmd_id;
    last_result = CMD_RESULT_ACCEPTED;
    return true;
}

// =====================================================
// ConfigManager 完成回调
//
// 在 ConfigManager::config_task() 上下文中被调用。
//
// 生命周期（重要）:
//   result 与 message 仅在本次回调期间有效。
//   需要留存的数据必须深拷贝进本文档 —— 下面的
//   doc["data"].set(...) 即为深拷贝。
//   严禁把 result 的引用或指针保存到回调之外。
// =====================================================
static void command_config_completion(
    const char *cmd_id,
    const char *command,
    const char *object,
    const char *source,
    ConfigCommandType type,
    bool success,
    ConfigCommandError error,
    const JsonDocument &result,
    const char *message
)
{
    // 结果一律以 runtime 中保存的原始 CommandMessage 为准，
    // 回调自带的参数仅用于日志与兜底。
    (void)command;
    (void)object;
    (void)source;
    (void)type;

    CommandRuntime *rt = command_runtime_find_by_cmd_id(String(cmd_id));

    if (rt == nullptr)
    {
        // runtime 已被超时清理，迟到回调只能忽略
        command_log("WARN", "Config callback for unknown cmd_id, ignored");
        return;
    }

    JsonDocument doc;
    doc["cmd"] = "result";
    doc["id"] = rt->cmd_id;
    doc["type"] = "config_result";
    doc["command"] = rt->message.command;
    doc["object"] = rt->message.object;
    doc["command_id"] = rt->cmd_id;
    doc["status"] = success ? "success" : "failed";
    doc["error_code"] = (int)error;
    doc["message"] = (message != nullptr) ? message : "";

    // 深拷贝结果数据。
    // set() 会把字符串拷进 doc 自己的内存池，
    // 因此 doc 与 ConfigManager 内部文档互不依赖。
    JsonVariantConst rv = result.as<JsonVariantConst>();

    if (!rv.isNull() && !rv.isUnbound())
    {
        // 查询类结果使用 {data:..., version/versions:...} 结构。
        // 拆到顶层，避免云端收到 data.data 这种嵌套。
        JsonVariantConst inner = rv["data"];

        if (!inner.isNull())
        {
            if (!doc["data"].set(inner))
            {
                command_log("ERROR", "config result: data copy failed");
            }

            JsonVariantConst ver = rv["version"];

            if (!ver.isNull() && !doc["version"].set(ver))
            {
                command_log("ERROR", "config result: version copy failed");
            }

            JsonVariantConst vers = rv["versions"];

            if (!vers.isNull() && !doc["versions"].set(vers))
            {
                command_log("ERROR", "config result: versions copy failed");
            }
        }
        else
        {
            if (!doc["data"].set(rv))
            {
                command_log("ERROR", "config result: data copy failed");
            }
        }
    }

    time_t ts = get_unix_timestamp();
    if (ts > 0)
    {
        doc["timestamp"] = ts;
    }

    rt->state = success ? COMMAND_STATE_SUCCESS : COMMAND_STATE_FAILED;

    String json;
    serializeJson(doc, json);
    command_report_result(json);

    command_runtime_release(rt);
}
