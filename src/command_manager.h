#ifndef COMMAND_MANAGER_H
#define COMMAND_MANAGER_H


#include <Arduino.h>

// =====================================================
// 最大命令数量
// 静态数组，ESP32安全，无malloc
// =====================================================

#define MAX_COMMAND_TABLE 20

// =====================================================
// 云端命令枚举
// =====================================================

enum CloudCommand
{
    CMD_UNKNOWN = 0,
    CMD_TEST,
    CMD_FEED_START,
    CMD_FEED_STOP,
    CMD_VALVE_OPEN,
    CMD_VALVE_CLOSE
};




// =====================================================
// 命令执行结果
// =====================================================

enum CommandResult
{

    CMD_OK = 0,
    // 当前模块忙
    CMD_BUSY,
    // 参数错误
    CMD_INVALID_PARAM,
    // 未知命令
    CMD_UNKNOWN_CMD,
    // 执行错误
    CMD_ERROR
};
// =====================================================
// 命令消息
// command_manager解析完成后发送给业务模块
// =====================================================

struct CommandMessage
{
    // 枚举类型
    CloudCommand command;
    // 原始命令名称
    // 例如 feed_start
    String name;
    // 参数
    long param;
    // 数据来源
    String source;
};




// =====================================================
// 命令回调函数
//
// 注意：
// 1. 禁止阻塞
// 2. 禁止delay
// 3. 长任务只启动状态机
//
// =====================================================

typedef CommandResult
(*CommandCallback)
(
    CommandMessage message
);




// =====================================================
// 初始化
// =====================================================

void command_manager_init();




// =====================================================
// 注册命令
//
// 由业务模块调用
//
// 示例:
//
// command_register(
//     CMD_FEED_START,
//     "feed_start",
//     feed_start_handler
// );
//
// =====================================================

bool command_register
(
    CloudCommand command,
    const char* name,
    CommandCallback callback
);

// =====================================================
// 提交命令
//
// 外部调用
//
// 格式:
//
// feed_start:20
//
// =====================================================

CommandResult command_submit
(
    const String &command_string
);
// =====================================================
// 字符串解析
//
// feed_start
//      |
//      v
// CMD_FEED_START
//
// =====================================================

CloudCommand command_parse
(
    const String &name
);

// =====================================================
// 枚举转字符串
//
// CMD_FEED_START
//      |
//      v
// "feed_start"
//
// =====================================================

const char* command_get_name
(
    CloudCommand command
);

// =====================================================
// 获取结果字符串
//
// CMD_OK
//      |
//      v
// "OK"
//
// =====================================================

const char* command_result_name
(
    CommandResult result
);

#endif