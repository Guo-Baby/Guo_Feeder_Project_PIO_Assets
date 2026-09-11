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
#include "system_command.h"

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
// Workflow 管理命令专用
#define CMD_ERROR_INVALID_PAYLOAD     11   // payload 不是合法 JSON
#define CMD_ERROR_NO_FREE_SLOT        12   // Workflow 槽位已满（16）
#define CMD_ERROR_REJECTED            13   // 修改被拒（运行中 / Critical acquire 失败）
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

// 注: 原 reboot_pending / reboot_start_ms / REBOOT_WAIT_TIMEOUT_MS
// （legacy reboot 的等待后 ESP.restart() 机制）已随 V2 迁移移除。
// 所有重启统一由 SystemCommand 的安全重启状态机执行。

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

// 命令结果回显（调试用，默认关闭）
static bool result_echo_enabled = false;

void command_manager_set_result_echo(bool enable)
{
    result_echo_enabled = enable;
}

static void command_report_result(const String &json)
{
    if (result_echo_enabled) {
        command_log("RESULT", json.c_str());
    }
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
// ---- SystemCommand（V2：资源查询 + 统一 Safe Restart）----
static bool command_system_memory(const CommandMessage &cmd, JsonDocument &response);
static bool command_system_flash(const CommandMessage &cmd, JsonDocument &response);
static bool command_system_restart(const CommandMessage &cmd, JsonDocument &response);
static bool command_system_restart_status(const CommandMessage &cmd, JsonDocument &response);
// system.time：查询 ESP 系统时间 + RTC 芯片时间（非阻塞，TimeManager V2 新增）
static bool command_system_get_time(const CommandMessage &cmd, JsonDocument &response);

// Config 异步命令 handler（定义在文件后段，此处前向声明以便路由）
// ---- Workflow 管理命令（云端同步 / 远程 CRUD）----
static bool command_workflow_list(const CommandMessage &cmd, JsonDocument &response);
static bool command_workflow_get(const CommandMessage &cmd, JsonDocument &response);
static bool command_workflow_create(const CommandMessage &cmd, JsonDocument &response);
static bool command_workflow_set(const CommandMessage &cmd, JsonDocument &response);
static bool command_workflow_delete(const CommandMessage &cmd, JsonDocument &response);
static bool command_workflow_save(const CommandMessage &cmd, JsonDocument &response);
static bool workflow_router(const CommandMessage &cmd, JsonDocument &response);

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

    // 初始化 SystemCommand（V1：系统资源查询 + 安全重启）
    system_command_init();
}

// =====================================================
// 主任务（loop 调用）
//
// 仅做 CommandRuntime 超时扫描（1s 节流）。
// 禁止: 轮询 Workflow / Action、调用 workflow_temp_action_is_complete()
// =====================================================
void command_manager_task()
{
    // 注: 原 legacy reboot 的"等待后 ESP.restart()"已移除，
    // 现在由 SystemCommand 的安全重启状态机统一执行。

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
    // ArduinoJson 7 的 JsonDocument 使用堆分配、按需自动增长，
    // 足以容纳 system.flash 的文件列表，无需指定固定容量。
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
// 一级路由: workflow（Workflow 管理 / 云端同步）
//
// 命令:
//   workflow.list    设备 Workflow 摘要（stable_id + variant）
//   workflow.get     拉取单个 Workflow 完整 JSON
//   workflow.create  新建
//   workflow.set     整体替换
//   workflow.delete  删除（只置 valid=false）
//   workflow.save    立即落盘（默认随后安全重启）
//
// 分层铁律:
//   本层只做【参数校验 + 调用 workflow.cpp API + 返回 JSON】，
//   不解析 BIN、不直接操作 LittleFS、不保存 Workflow 内部结构。
// =====================================================

// payload 解析（允许为空）
static bool workflow_parse_payload(
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
            CMD_ERROR_INVALID_PAYLOAD,
            "workflow payload is not valid JSON"
        );
        return false;
    }
    return true;
}

// 从 payload 中取出 workflow 对象
//
// 两种写法都接受:
//   {"workflow":{ ... }}
//   { "id":"x", "steps":[...] }        （payload 本身即 workflow）
static bool workflow_pick_object(
    const CommandMessage &cmd,
    JsonDocument &pl,
    JsonObject &out
)
{
    JsonVariant wf = pl["workflow"];
    if (!wf.isNull() && wf.is<JsonObject>())
    {
        out = wf.as<JsonObject>();
        return true;
    }
    if (!pl["id"].isNull())
    {
        out = pl.as<JsonObject>();
        return true;
    }

    command_send_error(
        cmd,
        CMD_ERROR_PARAM,
        "Missing 'workflow' object"
    );
    return false;
}

// 定位目标 Workflow
//
// 优先级: payload.stable_id > payload.id > cmd.object
// stable_id 经 CapabilityRegistry 翻译成 runtime_id，再用
// workflow_find_index_by_id() 找槽位。
static bool workflow_resolve_index(
    const CommandMessage &cmd,
    JsonDocument &pl,
    uint8_t &index,
    String &runtime_id
)
{
    if (!pl["stable_id"].isNull())
    {
        long k = pl["stable_id"].as<long>();
        if (k < 0 || k > 255 ||
            !capability_get_workflow_by_stable_id((uint8_t)k, runtime_id))
        {
            command_send_error(
                cmd,
                CMD_ERROR_WORKFLOW_NOT_FOUND,
                "workflow stable_id not found"
            );
            return false;
        }
    }
    else if (!pl["id"].isNull())
    {
        runtime_id = pl["id"].as<String>();
    }
    else if (cmd.object.length() > 0)
    {
        runtime_id = cmd.object;
    }
    else
    {
        command_send_error(
            cmd,
            CMD_ERROR_MISSING_OBJECT,
            "Missing 'stable_id' / 'id' (workflow)"
        );
        return false;
    }

    int idx = workflow_find_index_by_id(runtime_id);
    if (idx < 0)
    {
        String msg = "Workflow not found: ";
        msg += runtime_id;
        command_send_error(
            cmd,
            CMD_ERROR_WORKFLOW_NOT_FOUND,
            msg
        );
        return false;
    }

    index = (uint8_t)idx;
    return true;
}

// 找空槽位（id 为空 / 槽位从未使用）
static int workflow_find_free_slot()
{
    for (uint8_t i = 0; i < WORKFLOW_MAX_COUNT; i++)
    {
        Workflow *w = workflow_get(i);
        if (w == nullptr || w->id.length() == 0)
        {
            return (int)i;
        }
    }
    return -1;
}

// ---- workflow.list ----
static bool command_workflow_list(
    const CommandMessage &cmd,
    JsonDocument &response
)
{
    (void)cmd;

    uint8_t count = capability_get_workflow_count();

    Serial.printf(
        "[CMD][WF] list: registry_version=%u checksum=%u count=%u dirty=%d\n",
        (unsigned)capability_get_workflow_version(),
        (unsigned)capability_get_workflow_checksum(),
        (unsigned)count,
        workflow_has_any_dirty() ? 1 : 0
    );

    response["registry_version"] = capability_get_workflow_version();
    response["registry_checksum"] = capability_get_workflow_checksum();
    response["count"] = count;
    response["dirty"] = workflow_has_any_dirty();

    JsonArray arr = response["workflows"].to<JsonArray>();

    for (uint8_t i = 0; i < count; i++)
    {
        String runtime_id;
        uint32_t variant = 0;

        if (!capability_get_workflow_by_stable_id(i, runtime_id))
        {
            continue;
        }
        capability_get_workflow_object_version(i, variant);

        JsonObject item = arr.add<JsonObject>();
        item["stable_id"] = i;
        item["id"] = runtime_id;
        item["variant"] = variant;

        Serial.printf(
            "[CMD][WF]   #%u %s variant=%u\n",
            (unsigned)i, runtime_id.c_str(), (unsigned)variant
        );
    }

    response["status"] = "success";
    last_result = CMD_RESULT_OK;
    return true;
}

// ---- workflow.get ----
static bool command_workflow_get(
    const CommandMessage &cmd,
    JsonDocument &response
)
{
    JsonDocument pl;
    if (!workflow_parse_payload(cmd, pl))
    {
        return false;
    }

    uint8_t index = 0;
    String runtime_id;
    if (!workflow_resolve_index(cmd, pl, index, runtime_id))
    {
        return false;
    }

    String json;
    if (!workflow_export_workflow_json(index, json))
    {
        command_send_error(
            cmd,
            CMD_ERROR_EXECUTION,
            "workflow export failed"
        );
        return false;
    }

    // 导出结果是 String，这里反序列化回结构化对象再挂到 response，
    // 保证云端拿到的是真正可解析的 JSON 对象（而不是嵌套字符串）。
    JsonDocument tmp;
    if (deserializeJson(tmp, json))
    {
        command_send_error(
            cmd,
            CMD_ERROR_EXECUTION,
            "workflow export re-parse failed"
        );
        return false;
    }

    JsonObject dst = response["workflow"].to<JsonObject>();
    for (JsonPair kv : tmp.as<JsonObject>())
    {
        dst[kv.key()] = kv.value();
    }

    uint8_t stable_id = 0xFF;
    if (capability_get_workflow_stable_id(runtime_id, stable_id))
    {
        response["stable_id"] = stable_id;
    }

    Serial.printf(
        "[CMD][WF] get: slot=%u id=%s variant=%u bytes=%u\n",
        (unsigned)index, runtime_id.c_str(),
        (unsigned)workflow_get_variant(index), (unsigned)json.length()
    );

    response["status"] = "success";
    last_result = CMD_RESULT_OK;
    return true;
}

// ---- workflow.create ----
static bool command_workflow_create(
    const CommandMessage &cmd,
    JsonDocument &response
)
{
    JsonDocument pl;
    if (!workflow_parse_payload(cmd, pl))
    {
        return false;
    }

    JsonObject wf_obj;
    if (!workflow_pick_object(cmd, pl, wf_obj))
    {
        return false;
    }

    int slot = workflow_find_free_slot();
    if (slot < 0)
    {
        command_send_error(
            cmd,
            CMD_ERROR_NO_FREE_SLOT,
            "No free workflow slot (max 16)"
        );
        return false;
    }

    if (!workflow_apply_workflow_json((uint8_t)slot, wf_obj, true))
    {
        command_send_error(
            cmd,
            CMD_ERROR_REJECTED,
            "workflow create rejected (running / dirty acquire failed)"
        );
        return false;
    }

    // id 集合变化 → stable_id 映射与 checksum 都要重算
    capability_registry_rescan();

    String runtime_id = wf_obj["id"] | "";
    uint8_t stable_id = 0xFF;
    capability_get_workflow_stable_id(runtime_id, stable_id);

    Serial.printf(
        "[CMD][WF] create: slot=%u id=%s stable_id=%u variant=%u\n",
        (unsigned)slot, runtime_id.c_str(),
        (unsigned)stable_id,
        (unsigned)workflow_get_variant((uint8_t)slot)
    );

    response["status"] = "success";
    response["slot"] = slot;
    response["stable_id"] = stable_id;
    response["id"] = runtime_id;
    response["variant"] = workflow_get_variant((uint8_t)slot);
    response["dirty"] = workflow_has_any_dirty();
    last_result = CMD_RESULT_OK;
    return true;
}

// ---- workflow.set ----
static bool command_workflow_set(
    const CommandMessage &cmd,
    JsonDocument &response
)
{
    JsonDocument pl;
    if (!workflow_parse_payload(cmd, pl))
    {
        return false;
    }

    JsonObject wf_obj;
    if (!workflow_pick_object(cmd, pl, wf_obj))
    {
        return false;
    }

    uint8_t index = 0;
    String runtime_id;
    if (!workflow_resolve_index(cmd, pl, index, runtime_id))
    {
        return false;
    }

    if (!workflow_apply_workflow_json(index, wf_obj, false))
    {
        command_send_error(
            cmd,
            CMD_ERROR_REJECTED,
            "workflow set rejected (running / dirty acquire failed)"
        );
        return false;
    }

    capability_registry_rescan();

    String new_id = wf_obj["id"] | "";
    if (new_id.length() == 0)
    {
        new_id = runtime_id;
    }
    uint8_t stable_id = 0xFF;
    capability_get_workflow_stable_id(new_id, stable_id);

    Serial.printf(
        "[CMD][WF] set: slot=%u id=%s stable_id=%u variant=%u\n",
        (unsigned)index, new_id.c_str(),
        (unsigned)stable_id,
        (unsigned)workflow_get_variant(index)
    );

    response["status"] = "success";
    response["slot"] = index;
    response["stable_id"] = stable_id;
    response["id"] = new_id;
    response["variant"] = workflow_get_variant(index);
    response["dirty"] = workflow_has_any_dirty();
    last_result = CMD_RESULT_OK;
    return true;
}

// ---- workflow.delete ----
static bool command_workflow_delete(
    const CommandMessage &cmd,
    JsonDocument &response
)
{
    JsonDocument pl;
    if (!workflow_parse_payload(cmd, pl))
    {
        return false;
    }

    uint8_t index = 0;
    String runtime_id;
    if (!workflow_resolve_index(cmd, pl, index, runtime_id))
    {
        return false;
    }

    if (!workflow_delete(index))
    {
        command_send_error(
            cmd,
            CMD_ERROR_EXECUTION,
            "workflow delete failed"
        );
        return false;
    }

    capability_registry_rescan();

    Serial.printf(
        "[CMD][WF] delete: slot=%u id=%s variant=%u\n",
        (unsigned)index, runtime_id.c_str(),
        (unsigned)workflow_get_variant(index)
    );

    response["status"] = "success";
    response["slot"] = index;
    response["id"] = runtime_id;
    response["variant"] = workflow_get_variant(index);
    response["dirty"] = workflow_has_any_dirty();
    last_result = CMD_RESULT_OK;
    return true;
}

// ---- workflow.save ----
//
// 立即把 Dirty 落盘（不等 5 分钟延迟窗口），成功后默认请求
// 安全重启 —— 与 ConfigManager 的 config_save 同一套语义：
// 重启由 SystemCommand 统一执行，等 Critical Operation 归零
// 后进入 10s 安全窗口。
static bool command_workflow_save(
    const CommandMessage &cmd,
    JsonDocument &response
)
{
    JsonDocument pl;
    if (!workflow_parse_payload(cmd, pl))
    {
        return false;
    }

    bool had_dirty = workflow_has_any_dirty();
    bool restart = pl["restart"] | true;

    Serial.printf(
        "[CMD][WF] save: dirty=%d restart=%d\n",
        had_dirty ? 1 : 0, restart ? 1 : 0
    );

    bool ok = workflow_save_transaction();

    if (!ok)
    {
        command_send_error(
            cmd,
            CMD_ERROR_EXECUTION,
            "workflow save failed (dirty kept for retry)"
        );
        return false;
    }

    capability_registry_rescan();

    bool restarting = false;
    if (had_dirty && restart)
    {
        restarting = system_command_request_restart();
        Serial.printf(
            "[CMD][WF] save: restart requested=%d\n",
            restarting ? 1 : 0
        );
    }

    response["status"] = "success";
    response["saved"] = true;
    response["dirty"] = workflow_has_any_dirty();
    response["restarting"] = restarting;
    response["registry_version"] = capability_get_workflow_version();
    last_result = CMD_RESULT_OK;
    return true;
}

static bool workflow_router(
    const CommandMessage &cmd,
    JsonDocument &response
)
{
    const String &command = cmd.command;

    if (command == "workflow.list") {
        return command_workflow_list(cmd, response);
    }
    if (command == "workflow.get") {
        return command_workflow_get(cmd, response);
    }
    if (command == "workflow.create") {
        return command_workflow_create(cmd, response);
    }
    if (command == "workflow.set") {
        return command_workflow_set(cmd, response);
    }
    if (command == "workflow.delete") {
        return command_workflow_delete(cmd, response);
    }
    if (command == "workflow.save") {
        return command_workflow_save(cmd, response);
    }

    String msg = "Unknown workflow command: ";
    msg += command;
    command_send_error(
        cmd,
        CMD_ERROR_UNKNOWN_COMMAND,
        msg
    );
    return false;
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
//   "ob":"reboot|set_time|time|weight_zero|wifi_config|wifi_ap|memory|flash|restart|restart_status|config_*",
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
        // reboot（legacy 别名）：内部自行上报 result 并请求
        // SystemCommand 安全重启，随后返回 false（已自管上报，
        // 框架不再二次包装）。真正的重启由状态机在安全窗口后执行。
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

    // ---- SystemCommand（V2）：系统资源查询 + 统一 Safe Restart ----
    if (object == "memory") {
        return command_system_memory(cmd, response);
    }
    if (object == "flash") {
        return command_system_flash(cmd, response);
    }
    if (object == "restart") {
        return command_system_restart(cmd, response);
    }
    if (object == "restart_status") {
        return command_system_restart_status(cmd, response);
    }
    if (object == "time") {
        return command_system_get_time(cmd, response);
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
// system: reboot（legacy 别名）
//
// 流程:
//   1. 上报 result: status=rebooting（保留原有上报格式）
//   2. 请求 SystemCommand 执行安全重启
//
// V2 迁移: 本命令不再自己持有等待定时器、也不再直接 ESP.restart()。
// 与 restart 的区别仅在于上报文案，重启机制完全相同 ——
// 都会等 Critical Operation 归零后进入 10s 安全窗口再重启。
//
// 新代码请优先使用 restart。
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

    // 交给 SystemCommand，由它决定何时真正重启。
    system_command_request_restart();
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
//   {}               -> 请求重启（经 ConfigManager 落盘后转交 SystemCommand）
//   {"cancel":true}  -> 仅取消 ConfigManager 的 5 分钟自动重启倒计时
//
// V2 迁移说明:
//   Restart 的真正执行已收回 SystemCommand。
//   cancel=true 只能取消"尚未发生的自动重启倒计时"；
//   已交给 SystemCommand 的重启请求不可取消（需求文档 §13）。
//   新代码请优先使用 system / restart。
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
                        ? "config auto-restart timer cancel queued"
                        : "restart queued via SystemCommand";
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
// system: memory（V1）
//
// object: memory
// 同步查询 Internal RAM / External PSRAM，结果写入 response["data"]。
// 框架统一包装 cmd=result / id / command / timestamp 后上报。
// =====================================================
static bool command_system_memory(
    const CommandMessage &cmd,
    JsonDocument &response)
{
    if (!syscmd_memory(response))
    {
        command_send_error(cmd, CMD_ERROR_SYSTEM, "memory query failed");
        return false;
    }
    response["status"] = "success";
    last_result = CMD_RESULT_OK;
    return true;
}

// =====================================================
// system: flash（V1，合并原 system.files）
//
// object: flash
// 同步查询 Internal Flash / External Flash + 递归文件列表，结果写入 response["data"]。
// 文件列表只读：不影响 ConfigManager 的 Active / Backup / Factory 文件。
// =====================================================
static bool command_system_flash(
    const CommandMessage &cmd,
    JsonDocument &response)
{
    if (!syscmd_flash(response))
    {
        command_send_error(
            cmd,
            CMD_ERROR_SYSTEM,
            "flash query failed (storage/filesystem unavailable)"
        );
        return false;
    }
    response["status"] = "success";
    last_result = CMD_RESULT_OK;
    return true;
}

// =====================================================
// system: restart（V2：统一 Safe Restart）
//
// object: restart
//
// V2 变更: Restart 能力已从 ConfigManager 收回，改由 SystemCommand 统一执行。
// 系统内不再存在第二套重启机制，本命令只做一件事 —— 请求重启。
//
// 时序:
//   本 handler 立即返回 success
//     -> system_command_task() 等待 Critical Operation Count 归零
//     -> 进入 SYSTEM_RESTART_SAFE_DELAY_MS（10s）安全窗口
//     -> 窗口到点才执行 ESP.restart()
//
// 先回包再重启，保证云端能收到本次命令的成功应答。
//
// 不再支持 {"cancel":true}（需求文档 §13：Restart 一旦请求不可取消）。
// 若要取消 ConfigManager 的 5 分钟自动重启倒计时，请使用 config_restart。
// =====================================================
static bool command_system_restart(
    const CommandMessage &cmd,
    JsonDocument &response)
{
    JsonDocument pl;
    bool has_payload = config_parse_payload(cmd, pl);
    bool cancel = false;
    if (has_payload)
    {
        cancel = pl["cancel"] | false;
    }

    if (cancel)
    {
        command_send_error(
            cmd,
            CMD_ERROR_PARAM,
            "restart cannot be cancelled once requested"
        );
        return false;
    }

    if (!system_command_request_restart())
    {
        command_send_error(cmd, CMD_ERROR_SYSTEM, "restart request rejected");
        return false;
    }

    JsonObject data = response["data"].to<JsonObject>();
    if (!data.isNull())
    {
        unsigned long remain_ms = 0;
        system_command_restart_pending(remain_ms);

        data["state"] = system_command_restart_state_name(
                            system_command_restart_state());
        data["critical_operations"] =
            system_command_critical_operation_count();
        data["safe_delay_ms"] =
            (unsigned long)SYSTEM_RESTART_SAFE_DELAY_MS;
        data["remain_ms"] = remain_ms;
    }

    response["status"] = "success";
    response["message"] = "restart accepted (safe delay 10s)";
    last_result = CMD_RESULT_OK;
    return true;
}

// =====================================================
// system: restart_status（只读诊断）
//
// object: restart_status
//
// 用于观察 Safe Restart 状态机:
//   state / critical_operations / restart_pending / remain_ms / safe_delay_ms
// =====================================================
static bool command_system_restart_status(
    const CommandMessage &cmd,
    JsonDocument &response)
{
    if (!syscmd_restart_status(response))
    {
        command_send_error(cmd, CMD_ERROR_SYSTEM, "restart status build failed");
        return false;
    }
    response["status"] = "success";
    last_result = CMD_RESULT_OK;
    return true;
}

// =====================================================
// system: time（TimeManager V2 新增查询）
//
// object: time
// 返回 ESP 系统时间 + PCF8563T RTC 时间，非阻塞。
// 所有时间读取/转换逻辑在 time_manager 内部完成，
// 本 handler 只做结构填充（不直接访问 RTC / SNTP）。
//
// 响应 data 结构:
//   system_valid / system_unix(UTC) / system_str(本地时区)
//   rtc_present / rtc_valid / rtc_unix(UTC) / rtc_str(本地时区)
//   source (INVALID|RTC|SNTP) / last_ntp_sync / ntp_started
//   timezone_offset_h
// =====================================================
static bool command_system_get_time(
    const CommandMessage &cmd,
    JsonDocument &response)
{
    (void)cmd;

    TimeQueryResult r;
    if (!time_query(r))
    {
        command_send_error(cmd, CMD_ERROR_SYSTEM, "time query failed");
        return false;
    }

    JsonObject data = response["data"].to<JsonObject>();
    if (data.isNull())
    {
        command_send_error(cmd, CMD_ERROR_SYSTEM, "time query data alloc failed");
        return false;
    }

    data["system_valid"] = r.system_valid;
    data["system_unix"] = (int64_t)r.system_unix;
    data["system_str"] = r.system_local;

    data["rtc_present"] = r.rtc_present;
    data["rtc_valid"] = r.rtc_valid;
    data["rtc_unix"] = (int64_t)r.rtc_unix;
    data["rtc_str"] = r.rtc_local;

    data["source"] = r.source;
    data["last_ntp_sync"] = (int64_t)r.last_ntp_sync;
    data["ntp_started"] = r.ntp_started;
    data["timezone_offset_h"] = r.timezone_offset_h;

    response["status"] = "success";
    response["message"] = "get_time ok";
    last_result = CMD_RESULT_OK;
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
