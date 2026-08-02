#include <Arduino.h>
#include <ArduinoJson.h>
#include <time.h>

#include "command_manager.h"
#include "time_manager.h"
#include "workflow.h"
#include "system_state.h"

// =====================================================
// Command Runtime（命令生命周期管理）
//
// CommandRuntimeEntry 只管理"命令请求生命周期":
//   - 运行时槽位与容量保护
//   - command_id 去重
//   - 异步回调匹配（主键: command_id）
//   - 超时保护与清理
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

// 运行时目标类型
enum CommandTargetType
{
    COMMAND_TARGET_ACTION,
    COMMAND_TARGET_WORKFLOW
};

// 运行时状态
enum CommandRuntimeStatus
{
    COMMAND_RUNTIME_PENDING,
    COMMAND_RUNTIME_SUCCESS,
    COMMAND_RUNTIME_FAILED,
    COMMAND_RUNTIME_TIMEOUT
};

// 命令请求生命周期记录
struct CommandRuntimeEntry
{
    bool used;

    String command_id;          // 生命周期主键

    CommandTargetType target_type;

    String action_id;           // action 目标（action 命令）
    String workflow_id;         // workflow 目标（workflow 命令）

    uint32_t instance_id;       // WorkflowManager 域实例号（仅用于校验，不作主键）

    String source;              // 来源（仅记录）
    String object;              // 目标字段原值

    String payload;             // 原始 payload 唯一所有权；命令完成后随记录销毁

    unsigned long start_ms;
    unsigned long timeout_ms;

    CommandRuntimeStatus status;
};

static CommandRuntimeEntry runtime_queue[MAX_COMMAND_RUNTIME];
static unsigned long last_scan_ms = 0;

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
// 内部函数声明
// =====================================================
static bool command_execute_action(const CommandMessage &cmd, JsonDocument &response);
static bool command_execute_workflow(const CommandMessage &cmd, JsonDocument &response);
static bool command_query_actions(JsonDocument &response);
static bool command_query_triggers(JsonDocument &response);
static bool command_query_workflows(JsonDocument &response);
static bool command_query_state(const CommandMessage &cmd, JsonDocument &response);

// 异步回调接收者（Phase 2 WorkflowManager 将按最终契约调用）
static void command_temp_action_callback(
    uint32_t instance_id,
    const String &action_id,
    const String &command_id,
    CommandActionResult result
);
static void command_workflow_callback(
    const String &workflow_id,
    const String &command_id,
    CommandWorkflowResult result
);

// 运行时管理
static CommandRuntimeEntry *command_runtime_insert(
    const CommandMessage &cmd,
    CommandTargetType target_type,
    const String &action_id,
    const String &workflow_id,
    unsigned long timeout_ms,
    JsonDocument &response
);
static CommandRuntimeEntry *command_runtime_find_by_command_id(const String &command_id);
static bool command_runtime_validate_action(
    CommandRuntimeEntry *entry,
    uint32_t instance_id,
    const String &action_id
);
static bool command_runtime_validate_workflow(
    CommandRuntimeEntry *entry,
    const String &workflow_id
);
static void command_runtime_remove(CommandRuntimeEntry *entry);
static void command_runtime_report_timeout(CommandRuntimeEntry *entry);
static void command_runtime_scan_timeouts();

// =====================================================
// 运行时：插入记录
// 失败时填写 response 错误信息并返回 nullptr
// =====================================================
static CommandRuntimeEntry *command_runtime_insert(
    const CommandMessage &cmd,
    CommandTargetType target_type,
    const String &action_id,
    const String &workflow_id,
    unsigned long timeout_ms,
    JsonDocument &response
)
{
    // 1. 队列容量保护：满则拒绝新命令（不覆盖正在运行的记录）
    CommandRuntimeEntry *slot = nullptr;
    for (uint8_t i = 0; i < MAX_COMMAND_RUNTIME; i++) {
        if (!runtime_queue[i].used) {
            slot = &runtime_queue[i];
            break;
        }
    }
    if (slot == nullptr) {
        response["status"] = "error";
        response["message"] = "Command runtime queue full";
        command_log("WARN", "Command runtime queue full");
        return nullptr;
    }

    // 2. command_id 重复保护
    if (cmd.cmd_id.length() > 0 && command_runtime_find_by_command_id(cmd.cmd_id) != nullptr) {
        response["status"] = "error";
        response["message"] = "Duplicate command_id";
        command_log("WARN", "Duplicate command_id rejected");
        return nullptr;
    }

    // 3. 填充记录（payload 单一所有权）
    slot->used = true;
    slot->command_id = cmd.cmd_id;
    slot->target_type = target_type;
    slot->action_id = action_id;
    slot->workflow_id = workflow_id;
    slot->instance_id = 0;
    slot->source = cmd.source;
    slot->object = cmd.object;
    slot->payload = cmd.payload;
    slot->start_ms = millis();
    slot->timeout_ms = timeout_ms;
    slot->status = COMMAND_RUNTIME_PENDING;
    return slot;
}

// =====================================================
// 运行时：按 command_id 查找（回调匹配主键）
// =====================================================
static CommandRuntimeEntry *command_runtime_find_by_command_id(const String &command_id)
{
    if (command_id.length() == 0) {
        return nullptr;
    }
    for (uint8_t i = 0; i < MAX_COMMAND_RUNTIME; i++) {
        if (runtime_queue[i].used && runtime_queue[i].command_id == command_id) {
            return &runtime_queue[i];
        }
    }
    return nullptr;
}

// =====================================================
// 运行时：action 回调二次校验（instance_id / action_id）
// =====================================================
static bool command_runtime_validate_action(
    CommandRuntimeEntry *entry,
    uint32_t instance_id,
    const String &action_id
)
{
    if (entry == nullptr) return false;
    if (entry->target_type != COMMAND_TARGET_ACTION) return false;
    if (entry->instance_id != instance_id) return false;
    if (entry->action_id.length() > 0 && entry->action_id != action_id) return false;
    return true;
}

// =====================================================
// 运行时：workflow 回调二次校验（workflow_id）
// =====================================================
static bool command_runtime_validate_workflow(
    CommandRuntimeEntry *entry,
    const String &workflow_id
)
{
    if (entry == nullptr) return false;
    if (entry->target_type != COMMAND_TARGET_WORKFLOW) return false;
    if (entry->workflow_id.length() > 0 && entry->workflow_id != workflow_id) return false;
    return true;
}

// =====================================================
// 运行时：移除记录（payload 随记录销毁，不复制到其他模块）
// =====================================================
static void command_runtime_remove(CommandRuntimeEntry *entry)
{
    if (entry == nullptr) return;
    entry->used = false;
    entry->command_id = "";
    entry->action_id = "";
    entry->workflow_id = "";
    entry->instance_id = 0;
    entry->source = "";
    entry->object = "";
    entry->payload = "";
    entry->start_ms = 0;
    entry->timeout_ms = 0;
    entry->status = COMMAND_RUNTIME_PENDING;
}

// =====================================================
// 运行时：超时结果上报并移除记录
// 后续到达的迟到回调将因 command_id 不存在而被忽略
// =====================================================
static void command_runtime_report_timeout(CommandRuntimeEntry *entry)
{
    command_log("WARN", "Command runtime timeout, entry removed");

    JsonDocument doc;
    if (entry->target_type == COMMAND_TARGET_ACTION) {
        doc["type"] = "action_result";
        doc["action"] = entry->action_id;
        doc["command_id"] = entry->command_id;
        doc["status"] = "timeout";
        doc["instance_id"] = entry->instance_id;
    } else {
        doc["type"] = "workflow_result";
        doc["workflow"] = entry->workflow_id;
        doc["command_id"] = entry->command_id;
        doc["status"] = "timeout";
    }

    time_t ts = get_unix_timestamp();
    if (ts > 0) {
        doc["timestamp"] = ts;
    }

    String json;
    serializeJson(doc, json);
    command_report_result(json);

    command_runtime_remove(entry);
}

// =====================================================
// 运行时：超时扫描（command_manager_task 每 1s 调用一次）
// 只扫描 CommandRuntimeEntry，不查询 Workflow/Action 状态
// =====================================================
static void command_runtime_scan_timeouts()
{
    unsigned long now = millis();
    for (uint8_t i = 0; i < MAX_COMMAND_RUNTIME; i++) {
        CommandRuntimeEntry *entry = &runtime_queue[i];
        if (!entry->used) continue;
        if (entry->status != COMMAND_RUNTIME_PENDING) continue;
        // Arduino 标准无符号减法，回绕安全
        if (now - entry->start_ms >= entry->timeout_ms) {
            command_runtime_report_timeout(entry);
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
        command_runtime_remove(&runtime_queue[i]);
    }
    last_scan_ms = 0;
}

// =====================================================
// 主任务（loop 调用）
//
// 仅做 CommandRuntimeEntry 超时扫描（1s 节流）。
// 禁止: 轮询 Workflow / Action、调用 workflow_temp_action_is_complete()
// =====================================================
void command_manager_task()
{
    unsigned long now = millis();
    if (now - last_scan_ms >= COMMAND_RUNTIME_SCAN_INTERVAL_MS) {
        last_scan_ms = now;
        command_runtime_scan_timeouts();
    }
}

// =====================================================
// 接收并执行命令
//
// 输入: 已解析的 CommandMessage（payload 原样，不解析）
// 输出: JSON 字符串（响应）
// =====================================================
String command_manager_execute(const CommandMessage &cmd)
{
    JsonDocument response;
    const String command = cmd.command;
    command_log("INFO", "Received command");

    if (command.length() == 0) {
        response["status"] = "error";
        response["message"] = "Missing 'command' field";
        last_result = CMD_RESULT_ERROR;
        String output;
        serializeJson(response, output);
        return output;
    }

    // 路由分发
    bool success = false;
    if (command == "execute_action") {
        success = command_execute_action(cmd, response);
    } else if (command == "execute_workflow") {
        success = command_execute_workflow(cmd, response);
    } else if (command == "query_actions") {
        success = command_query_actions(response);
    } else if (command == "query_state") {
        success = command_query_state(cmd, response);
    } else if (command == "query_triggers") {
        success = command_query_triggers(response);
    } else if (command == "query_workflows") {
        success = command_query_workflows(response);
    } else {
        response["status"] = "error";
        String msg = "Unknown command: ";
        msg += command;
        response["message"] = msg;
        success = false;
    }

    // 通用字段
    response["command"] = command;
    if (!success && response["status"].isNull()) {
        response["status"] = "error";
        response["message"] = "Execution failed";
        last_result = CMD_RESULT_ERROR;
    }

    time_t ts = get_unix_timestamp();
    if (ts > 0) {
        response["timestamp"] = ts;
    }

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
// 清除状态（仅复位同步应答结果；运行中的记录仍由回调/超时完成）
// =====================================================
void command_manager_clear()
{
    last_result = CMD_RESULT_OK;
    command_log("INFO", "Command cleared");
}

// =====================================================
// 执行 Action
//
// 流程:
//   1. 校验 action id（cmd.object）
//   2. 创建 CommandRuntimeEntry（payload 归记录所有，不解析）
//   3. 调用 WorkflowManager 最终入队接口
//   4. 返回 accepted；完成由回调/超时路径处理
// =====================================================
static bool command_execute_action(const CommandMessage &cmd, JsonDocument &response)
{
    // 1. 校验目标 action id
    if (cmd.object.length() == 0) {
        response["status"] = "error";
        response["message"] = "Missing 'object' (action id)";
        last_result = CMD_RESULT_ERROR;
        return false;
    }

    // 2. 创建运行时记录
    CommandRuntimeEntry *entry = command_runtime_insert(
        cmd,
        COMMAND_TARGET_ACTION,
        cmd.object,
        "",
        COMMAND_ACTION_TIMEOUT_MS,
        response
    );
    if (entry == nullptr) {
        last_result = CMD_RESULT_FAILED;
        return false;
    }

    // 3. 调用 WorkflowManager 最终接口
    //
    // TODO(Phase 2): WorkflowManager 需按最终契约升级 workflow_enqueue_action:
    //   - 增加 command_id 传递
    //   - 回调升级为 CommandTempActionCallback(instance_id, action_id, command_id, CommandActionResult)
    //   - payload 由 WorkflowManager / Action 模块自行解析（CommandManager 不解析）
    // 当前 workflow.h 为旧签名 (id, params, param_count, instance_id*, 2参回调, timeout)，
    // 与本调用点不兼容 —— 外部依赖不匹配，已记录，Phase 2 适配。
    uint32_t instance_id = 0;
    bool queued = workflow_enqueue_action(
        entry->action_id,
        entry->payload,
        &instance_id,
        entry->command_id,
        command_temp_action_callback,
        entry->timeout_ms
    );

    if (!queued) {
        response["status"] = "error";
        response["message"] = "Failed to queue action";
        last_result = CMD_RESULT_FAILED;
        command_runtime_remove(entry);
        return false;
    }

    // 回填 WorkflowManager 域实例号（仅用于回调二次校验）
    entry->instance_id = instance_id;

    response["status"] = "accepted";
    response["message"] = "Action queued";
    response["action"] = entry->action_id;
    response["instance_id"] = instance_id;
    last_result = CMD_RESULT_RUNNING;
    return true;
}

// =====================================================
// 执行 Workflow（通过 cmd.object 指定 workflow id）
//
// 流程:
//   1. 校验并查找 Workflow
//   2. 创建 CommandRuntimeEntry
//   3. 调用 WorkflowManager 最终启动接口
//   4. 返回 accepted；完成由回调/超时路径处理
// =====================================================
static bool command_execute_workflow(const CommandMessage &cmd, JsonDocument &response)
{
    if (cmd.object.length() == 0) {
        response["status"] = "error";
        response["message"] = "Missing 'object' (workflow id)";
        last_result = CMD_RESULT_ERROR;
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
        response["status"] = "error";
        String msg = "Workflow not found: ";
        msg += cmd.object;
        response["message"] = msg;
        last_result = CMD_RESULT_ERROR;
        return false;
    }

    CommandRuntimeEntry *entry = command_runtime_insert(
        cmd,
        COMMAND_TARGET_WORKFLOW,
        "",
        cmd.object,
        COMMAND_WORKFLOW_TIMEOUT_MS,
        response
    );
    if (entry == nullptr) {
        last_result = CMD_RESULT_FAILED;
        return false;
    }

    // TODO(Phase 2): WorkflowManager 需按最终契约升级 workflow_start:
    //   workflow_start(workflow, skip_first_step, command_id, WorkflowResultCallback)
    //   WorkflowManager 将内部 WorkflowState 转换为 CommandWorkflowResult 后回调。
    // 当前 workflow.h 仅有 workflow_start(Workflow*, bool) —— 外部依赖不匹配，已记录。
    bool started = workflow_start(
        wf,
        true,
        entry->command_id,
        command_workflow_callback
    );

    if (!started) {
        response["status"] = "error";
        response["message"] = "Failed to start workflow";
        last_result = CMD_RESULT_ERROR;
        command_runtime_remove(entry);
        return false;
    }

    response["status"] = "accepted";
    response["message"] = "Workflow started";
    response["workflow"] = wf->id;
    last_result = CMD_RESULT_OK;
    return true;
}

// =====================================================
// Action 完成回调（最终契约）
//
// 流程:
//   1. 按 command_id 查找运行时记录（主键）
//   2. 校验 instance_id / action_id
//   3. 生成 action_result JSON
//   4. command_report_result() 上报
//   5. 移除记录（迟到回调因 command_id 不存在而被忽略）
// =====================================================
static void command_temp_action_callback(
    uint32_t instance_id,
    const String &action_id,
    const String &command_id,
    CommandActionResult result
)
{
    CommandRuntimeEntry *entry = command_runtime_find_by_command_id(command_id);
    if (entry == nullptr) {
        command_log("WARN", "Action callback for unknown command_id, ignored");
        return;
    }

    if (!command_runtime_validate_action(entry, instance_id, action_id)) {
        command_log("WARN", "Action callback mismatch (instance_id/action_id), ignored");
        return;
    }

    JsonDocument doc;
    doc["type"] = "action_result";
    doc["action"] = action_id;
    doc["command_id"] = command_id;
    doc["instance_id"] = instance_id;

    if (result == COMMAND_ACTION_SUCCESS) {
        doc["status"] = "success";
        entry->status = COMMAND_RUNTIME_SUCCESS;
    } else {
        doc["status"] = "failed";
        entry->status = COMMAND_RUNTIME_FAILED;
    }

    time_t ts = get_unix_timestamp();
    if (ts > 0) {
        doc["timestamp"] = ts;
    }

    String json;
    serializeJson(doc, json);
    command_report_result(json);

    command_runtime_remove(entry);
}

// =====================================================
// Workflow 完成回调（最终契约）
// =====================================================
static void command_workflow_callback(
    const String &workflow_id,
    const String &command_id,
    CommandWorkflowResult result
)
{
    CommandRuntimeEntry *entry = command_runtime_find_by_command_id(command_id);
    if (entry == nullptr) {
        command_log("WARN", "Workflow callback for unknown command_id, ignored");
        return;
    }

    if (!command_runtime_validate_workflow(entry, workflow_id)) {
        command_log("WARN", "Workflow callback mismatch (workflow_id), ignored");
        return;
    }

    JsonDocument doc;
    doc["type"] = "workflow_result";
    doc["workflow"] = workflow_id;
    doc["command_id"] = command_id;

    if (result == COMMAND_WORKFLOW_SUCCESS) {
        doc["status"] = "success";
        entry->status = COMMAND_RUNTIME_SUCCESS;
    } else if (result == COMMAND_WORKFLOW_TIMEOUT) {
        doc["status"] = "timeout";
        entry->status = COMMAND_RUNTIME_TIMEOUT;
    } else {
        doc["status"] = "failed";
        entry->status = COMMAND_RUNTIME_FAILED;
    }

    time_t ts = get_unix_timestamp();
    if (ts > 0) {
        doc["timestamp"] = ts;
    }

    String json;
    serializeJson(doc, json);
    command_report_result(json);

    command_runtime_remove(entry);
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
// 目标 key 由 cmd.object 提供（Phase 3: 云侧把查询 key 放入 ob）。
// payload 不解析。
// =====================================================
static bool command_query_state(const CommandMessage &cmd, JsonDocument &response)
{
    if (cmd.object.length() == 0) {
        response["status"] = "error";
        response["message"] = "Missing 'object' (state key)";
        last_result = CMD_RESULT_ERROR;
        return false;
    }

    String value;
    if (system_state_query(cmd.object, value)) {
        response["status"] = "success";
        response["key"] = cmd.object;
        response["value"] = value;
        last_result = CMD_RESULT_OK;
        return true;
    }

    response["status"] = "error";
    String msg = "State not found: ";
    msg += cmd.object;
    response["message"] = msg;
    last_result = CMD_RESULT_ERROR;
    return false;
}
