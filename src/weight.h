#pragma once

// =====================================================
// Weight 模块 - HX711 称重（应用模块 + 底层驱动模块）
//
// 职责:
//   - HX711 采样（is_ready 触发，非阻塞）
//   - 5 点窗口滤波 + 重量计算
//   - Workflow Trigger 注册（weight_decrease）
//   - Workflow Action 注册（WEIGHT_ZERO）
//   - SystemState 维护（STATE_WEIGHT_VALUE / STATE_WEIGHT_ERROR）
//   - Event 发布（EVENT_WEIGHT_ERROR）
//
// 边界:
//   - 不实现 CommandManager 逻辑，不生成命令结果
//   - 不主动处理命令回调（由 Workflow 框架 / CommandManager 契约管理）
//   - 同一份 Descriptor 同时支持 Workflow Step 与 Command 临时 Action 调用
// =====================================================

// 初始化（main.cpp setup 中调用，须在 workflow_init() 之后）
void weight_init();

// 任务（main.cpp loop 中调用）
// 负责 HX711 采样 / 滤波 / 重量计算 / 零点校准 / 异常检测 / 状态同步
void weight_task();

// 获取原始值（最近一次成功读取的 HX711 数据）
int weight_get_raw();

// 获取重量值，单位克（滤波后的当前重量）
float weight_get_gram();

// 启动零点校准（非阻塞）
//
// 只记录开始状态并立即返回，实际采样在 weight_task() 中完成；
// 校准完成后自动调用 config_set_weight_zero_offset() 与 config_save()。
void weight_zero_calibrate();

// 获取 weight_active 状态
// false = 空闲；true = workflow 重量触发运行中
bool weight_is_active();
