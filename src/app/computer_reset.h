#ifndef COMPUTER_RESET_H
#define COMPUTER_RESET_H

#include <Arduino.h>

// =====================================================
// Computer Reset 模块 - GPIO8 脉冲控制（docs/archive/legacy/AI_TASK.md）
//
// 职责:
//   - GPIO8 初始化为安全状态 LOW
//   - Workflow Action 注册（COMPUTER_RESET）
//   - 非阻塞 800ms 高电平脉冲，模拟电脑主板 Reset 按键
//
// 边界:
//   - 不实现 CommandManager 逻辑，不生成命令结果
//   - 不调用任何 system_command_* 接口（本 Action 不占用 Critical Operation）
//   - 不新增独立 Command 执行路径：
//       云端 -> CommandManager -> WorkflowManager -> Temporary Action -> 本模块
//   - GPIO8 的控制权只属于本模块
// =====================================================

// =====================================================
// GPIO 定义（docs/archive/legacy/AI_TASK.md §4）
//
// 全项目唯一定义点，其他位置禁止硬编码 8。
// =====================================================
constexpr uint8_t COMPUTER_RESET_PIN = 8;

// =====================================================
// HIGH 持续时间（docs/archive/legacy/AI_TASK.md §7）
//
// 固定 800ms，本版本不从 Command Payload / ConfigManager 获取，
// 不新增可配置参数。
// =====================================================
constexpr unsigned long COMPUTER_RESET_HOLD_MS = 800UL;

// =====================================================
// 初始化
//
// main.cpp setup() 中调用，须在 workflow_init() 之后。
// 会将 GPIO8 配置为 OUTPUT 并立即置 LOW。
// =====================================================
void computer_reset_init();

// =====================================================
// 任务（main.cpp loop() 中调用）
//
// 职责:
//   1. 回收手动触发（computer_reset_trigger）产生的脉冲
//   2. 安全兜底：输出异常长时间保持 HIGH 时强制拉低
//
// 非阻塞，不使用 delay()。
// =====================================================
void computer_reset_task();

// =====================================================
// 手动触发一次 800ms 脉冲（调试 / 其他业务模块使用）
//
// 返回 true 表示脉冲已启动；false 表示模块未初始化或上下文池已满。
// 与 Workflow Action 共用同一套输出仲裁，不会互相干扰。
// =====================================================
bool computer_reset_trigger();

// =====================================================
// 查询 GPIO8 当前是否处于 HIGH（脉冲进行中）
// =====================================================
bool computer_reset_is_active();

#endif
