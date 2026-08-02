#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>

// =====================================================
// Command Manager
//
// 统一命令调度与生命周期管理层
//
// 数据流:
//   CloudManager (MQTT JSON → CommandMessage)
//     ↓
//   CommandManager (路由 + 生命周期 + 超时 + 结果JSON)
//     ↓
//   WorkflowManager (Phase 2 接口适配)
//     ↓
//   Action Module
//
// CommandManager 负责:
//   - 命令路由
//   - 命令生命周期管理 (CommandRuntimeEntry)
//   - 异步结果匹配
//   - 超时保护
//   - 结果 JSON 生成与上报
//
// CommandManager 不负责:
//   - 解析 MQTT JSON / payload（payload 为自定义参数容器，不解析、不理解语义）
//   - 执行 Workflow / Action
//   - 控制硬件
//   - 管理/轮询 Workflow 或 Action 状态机
// =====================================================

// =====================================================
// 命令类型
// =====================================================
enum CommandType
{
    CMD_NONE = 0,
    CMD_ACTION,          // 执行 Action
    CMD_WORKFLOW,        // 启动 Workflow
    CMD_QUERY_STATE,     // 查询 System State
    CMD_QUERY_ACTIONS,   // 查询 Action 列表
    CMD_QUERY_TRIGGERS,  // 查询 Trigger 列表
    CMD_QUERY_WORKFLOWS, // 查询 Workflow 列表
    CMD_SYSTEM,          // 系统命令（reboot 等）
    CMD_UPDATE           // 更新命令（OTA 等）
};

// =====================================================
// 执行结果（同步应答）
// =====================================================
enum CommandResult
{
    CMD_RESULT_OK = 0,
    CMD_RESULT_RUNNING,
    CMD_RESULT_FAILED,
    CMD_RESULT_ERROR,
    CMD_RESULT_UNKNOWN
};

// =====================================================
// 异步 Action 结果（CommandManager 自有类型）
// Phase 2: WorkflowManager 将内部结果转换为本枚举
// =====================================================
enum CommandActionResult
{
    COMMAND_ACTION_SUCCESS = 0,
    COMMAND_ACTION_FAILED,
    COMMAND_ACTION_TIMEOUT
};

// =====================================================
// 异步 Workflow 结果（CommandManager 自有类型）
// =====================================================
enum CommandWorkflowResult
{
    COMMAND_WORKFLOW_SUCCESS = 0,
    COMMAND_WORKFLOW_FAILED,
    COMMAND_WORKFLOW_TIMEOUT
};

// =====================================================
// 命令消息
//
// CloudManager 负责 MQTT JSON → CommandMessage。
// payload 是用户自定义参数容器（String），
// CommandManager 不解析、不理解其含义，仅随 CommandRuntimeEntry 保存。
// =====================================================
struct CommandMessage
{
    String command;        // 命令名: execute_action / execute_workflow / query_*
    String object;         // 目标: action id / workflow id / state key
    String cmd_id;         // 命令唯一 ID（生命周期主键）
    String payload;        // 原始 payload 字符串（归 CommandRuntimeEntry 所有）
    String source;         // 命令来源（仅记录，不作路由）
    unsigned long timestamp;
};

// =====================================================
// 回调契约
// =====================================================

// 日志回调（外部注入）
typedef void (*CommandLogCallback)(const char *level, const char *msg);

// 命令结果上报回调（由 CloudManager 注入）
typedef void (*CommandResultCallback)(const String &json);

// 临时 Action 完成回调（最终契约，Phase 2 WorkflowManager 调用）
typedef void (*CommandTempActionCallback)(
    uint32_t instance_id,
    const String &action_id,
    const String &command_id,
    CommandActionResult result
);

// Workflow 完成回调（最终契约，Phase 2 WorkflowManager 调用）
typedef void (*WorkflowResultCallback)(
    const String &workflow_id,
    const String &command_id,
    CommandWorkflowResult result
);

// =====================================================
// 公共接口
// =====================================================

// 初始化
void command_manager_init();

// 主任务（loop 调用）：仅扫描 CommandRuntimeEntry 超时
void command_manager_task();

// 接收并执行命令（已解析的 CommandMessage，不解析任何 JSON）
String command_manager_execute(const CommandMessage &cmd);

// 获取最后一次执行结果
CommandResult command_manager_last_result();

// 清除状态
void command_manager_clear();

// 日志回调注册
void command_manager_set_log_callback(CommandLogCallback callback);

// 命令结果回调注册（CloudManager 契约保持不变）
void command_manager_set_result_callback(CommandResultCallback callback);
