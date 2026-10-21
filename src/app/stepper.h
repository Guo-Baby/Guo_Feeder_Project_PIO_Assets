#ifndef STEPPER_H
#define STEPPER_H

#include <Arduino.h>
#include <stdint.h>

// =====================================================
// Stepper 模块 - DRV8825 步进电机驱动（应用模块 + 底层驱动）
//
// 职责:
//   - STEP / DIR / EN 三个 GPIO 的安全默认与电平控制
//   - 用 ESP32-S3 的 RMT 外设生成 STEP 脉冲（不依赖 loop 软件翻转）
//   - 120° 标准出粮动作的量化误差账本（累计余数分配）
//   - Workflow Action 注册（STEPPER_DISPENSE_ROTATE / STEPPER_ROTATE_STEPS /
//     STEPPER_EMERGENCY_STOP）
//   - SystemState 维护 / EventManager 事件发布
//
// 边界:
//   - 不实现 CommandManager 逻辑，不生成命令结果
//   - 不感知 MQTT / CloudManager：命令一律经 CommandManager →
//     WorkflowManager → 本模块 Action
//   - 开环控制：本模块**不**声称电机真实机械位置（无编码器反馈），
//     只报告"已按计划输出 N 个有效 STEP 上升沿"
// =====================================================

// =====================================================
// 动作结果（写入 STATE_STEPPER_LAST_RESULT 的 int 值）
//
// 与"配置允许使用模块""驱动器是否使能""动作是否运行"严格区分。
// =====================================================
enum StepperResult : int
{
    STEPPER_RESULT_NONE     = 0,   // 从未执行
    STEPPER_RESULT_SUCCESS  = 1,   // 完成（脉冲按计划输出完毕）
    STEPPER_RESULT_FAILED   = 2,   // 启动失败 / 硬件错误
    STEPPER_RESULT_TIMEOUT  = 3,   // 30s 兜底超时
    STEPPER_RESULT_ESTOP    = 4,   // 被急停中止
    STEPPER_RESULT_REJECTED = 5    // 参数 / 状态校验拒绝（未产生任何脉冲）
};

// =====================================================
// 初始化（main.cpp setup 第四层：workflow_init() 之后）
//
// 无论配置是否启用，都会：
//   ① 把 DIR/EN 置为安全电平后再配置为输出（先写电平，后 gpio_config）
//   ② 注册 Action（保证 Capability Registry 条目数不随配置漂移）
// 配置 enable=false 或引脚非法时：不占用 STEP(RMT)、Action 一律拒绝执行。
// =====================================================
void stepper_init();

// =====================================================
// 任务（main.cpp loop() 中调用，非阻塞）
//
// 负责：使能等待 → 启动脉冲 → 完成判定 → 30s 超时兜底
// =====================================================
void stepper_task();

// =====================================================
// 查询接口
// =====================================================
bool     stepper_is_enabled();          // 配置是否启用
bool     stepper_is_driver_enabled();   // 驱动器 EN 当前是否处于使能电平
bool     stepper_is_busy();             // 是否有动作正在进行
int      stepper_get_last_result();     // StepperResult
uint32_t stepper_get_last_pulses();     // 上一次动作计划输出的脉冲数
uint32_t stepper_get_dispense_count();  // 成功完成的出粮动作次数（账本）
uint32_t stepper_get_issued_total();    // 账本累计脉冲数 = floor(N*steps_per_rev*angle/360)
uint32_t stepper_plan_dispense_pulses();// 预演下一次出粮脉冲数（不改账本）
int      stepper_get_step_pin();
int      stepper_get_dir_pin();
int      stepper_get_en_pin();

// =====================================================
// 急停（供未来安全模块调用，本次由 STEPPER_EMERGENCY_STOP Action 调用）
//
// 语义：立即停止 RMT 脉冲输出（先停 RMT，再禁用驱动器），
//       正在运行的 Action 以 ACTION_FAILED 结束，随后统一收尾。
// 返回 false 表示当前没有正在进行的动作（无副作用）。
// =====================================================
bool stepper_emergency_stop();

#endif
