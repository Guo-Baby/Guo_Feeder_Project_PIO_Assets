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
//   CommandManager (路由 + CommandRuntime 生命周期 + 超时 + 结果JSON)
//     ↓
//   WorkflowManager (保存 cmd_id 关联，完成后回调 cmd_id)
//     ↓
//   Action Module
//
// CommandManager 负责:
//   - 命令路由
//   - CommandRuntime 生命周期管理（CommandMessage 唯一所有权）
//   - cmd_id 查询接口
//   - 超时保护与清理
//   - 结果 JSON 生成与上报
//
// CommandManager 不负责:
//   - 解析 MQTT JSON / payload（payload 为自定义参数容器，不解析、不理解语义）
//   - 执行 Workflow / Action
//   - 控制硬件
//   - 管理/轮询 Workflow 或 Action 状态机
//
// 依赖方向: workflow.h 包含本头文件（回调契约由 CommandManager 定义）
// 本头文件不包含 workflow.h；所需 Workflow 枚举以前向声明提供，
// workflow.h 中对应枚举定义必须带 ": int" 底层类型。
// =====================================================

// =====================================================
// Workflow 枚举前向声明（固定底层类型，避免包含 workflow.h）
// =====================================================
enum WorkflowActionResult : int;
enum WorkflowState : int;

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
// Command 运行时状态
// =====================================================
enum CommandState
{
    COMMAND_STATE_PENDING,
    COMMAND_STATE_SUCCESS,
    COMMAND_STATE_FAILED,
    COMMAND_STATE_TIMEOUT
};

// =====================================================
// 命令消息
//
// CloudManager 负责 MQTT JSON → CommandMessage。
// payload 是用户自定义参数容器（String），
// CommandManager 不解析、不理解其含义，仅随 CommandRuntime 保存。
// =====================================================
struct CommandMessage
{
    String command;        // 命令名: execute_action / execute_workflow / query_*
    String object;         // 目标: action id / workflow id / state key
    String cmd_id;         // 命令唯一 ID（生命周期主键）
    String payload;        // 原始 payload 字符串（归 CommandRuntime 所有）
    String source;         // 命令来源（仅记录，不作路由）
    unsigned long timestamp;
};

// =====================================================
// Command Runtime（命令请求生命周期记录）
//
// CommandManager 是 CommandMessage 的唯一所有者；
// 其他模块只能通过 command_manager_get_runtime() /
// command_manager_get_message() 按 cmd_id 查询，不得复制保存。
// 生命周期: 命令创建 → PENDING → 回调/超时 → 生成结果 → 释放。
// =====================================================
struct CommandRuntime
{
    bool active;               // 槽位占用标记
    String cmd_id;             // 生命周期主键
    CommandMessage message;    // 命令消息（唯一所有权）
    CommandState state;
    unsigned long create_time; // millis() 创建时间
    unsigned long start_ms;    // 超时计时起点
    unsigned long timeout_ms;  // 超时阈值
    uint32_t instance_id;      // 临时 Action 实例号（action 命令，回调校验用）
    String workflow_id;        // 关联 workflow id（workflow 命令）
};

// =====================================================
// 回调契约（CommandManager 定义，workflow.h 通过包含本头文件引用）
// =====================================================

// 日志回调（外部注入）
typedef void (*CommandLogCallback)(const char *level, const char *msg);

// 命令结果上报回调（由 CloudManager 注入）
typedef void (*CommandResultCallback)(const String &json);

// 临时 Action 完成回调（最终契约）
typedef void (*CommandTempActionCallback)(
    const String &cmd_id,
    uint32_t instance_id,
    WorkflowActionResult result
);

// Workflow 完成回调（最终契约）
typedef void (*WorkflowResultCallback)(
    const String &cmd_id,
    WorkflowState result
);

// =====================================================
// 公共接口
// =====================================================

// 初始化
void command_manager_init();

// 主任务（loop 调用）：仅扫描 CommandRuntime 超时
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

// =====================================================
// cmd_id 查询接口
//
// 供 WorkflowManager / Action 等模块查询当前有效命令参数。
// 只读查询：不创建、不复制数据；返回指针生命周期归 CommandManager。
// 找不到或已释放返回 nullptr。
// =====================================================
CommandRuntime *command_manager_get_runtime(const String &cmd_id);

const CommandMessage *command_manager_get_message(const String &cmd_id);
