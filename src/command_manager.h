#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>

// =====================================================
// Command Manager
//
// 统一命令调度层
//
// Cloud Manager → Command Manager → Workflow / System State
//
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
// 执行结果
// =====================================================
enum CommandResult
{
    CMD_RESULT_OK = 0,
    CMD_RESULT_RUNNING,
    CMD_RESULT_FAILED,
    CMD_RESULT_ERROR,
    CMD_RESULT_UNKNOWN
};

struct CommandMessage 
{ 
    String command; 
    String object; 
    String cmd_id; 
    JsonObject payload; 
    String source; 
    unsigned long timestamp; 
};

// =====================================================
// 初始化
// =====================================================
void command_manager_init();

// =====================================================
// 主任务（loop 调用）
// =====================================================
void command_manager_task();

// =====================================================
// 接收并执行命令
//
// 输入: JSON 字符串
// 输出: JSON 字符串（响应）
// =====================================================
String command_manager_execute(const String &json);

// =====================================================
// 获取最后一次执行结果
// =====================================================
CommandResult command_manager_last_result();

// =====================================================
// 清除状态
// =====================================================
void command_manager_clear();

// =====================================================
// 日志回调（外部注入）
// =====================================================
typedef void (*CommandLogCallback)(const char *level, const char *msg);
void command_manager_set_log_callback(CommandLogCallback callback);

// =====================================================
// Command 执行结果回调（由上层注入）
// =====================================================
typedef void (*CommandResultCallback)(const String &json);

void command_manager_set_result_callback(CommandResultCallback callback);