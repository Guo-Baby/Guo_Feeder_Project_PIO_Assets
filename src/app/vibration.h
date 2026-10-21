#ifndef VIBRATION_H
#define VIBRATION_H

#include <Arduino.h>
#include <stdint.h>

// =====================================================
// Vibration 模块 - 振动马达控制（单 GPIO）
//
// 硬件事实（用户确认）：
//   两个物理振动马达【共用同一路 GPIO】，因此固件只有一路振动输出。
//   两个马达同时启动、同时停止；不提供两个独立的软件马达通道。
//
// ★ 该 GPIO 只是驱动电路（继电器 / MOS 管）的【逻辑控制信号】，
//   绝不假定 ESP32 的 GPIO 直接给马达供电。
//   GPIO 高低电平对应的启停状态由 `active_level` 配置决定，
//   必须由实际电路确认（默认 1 = 高电平启动）。
//
// 职责:
//   - 单路振动输出的安全默认与启停
//   - 持续模式（duration_ms = 0）与定时模式（duration_ms > 0）
//   - 30 秒最长运行时间保护（模块层兜底，与 Workflow 定时等待无关）
//   - Workflow Action 注册（VIBRATION_START / VIBRATION_STOP）
//   - SystemState 维护 / EventManager 事件发布
// =====================================================

enum VibrationResult : int
{
    VIB_RESULT_NONE     = 0,   // 从未执行 / 持续模式已启动
    VIB_RESULT_SUCCESS  = 1,   // 定时模式正常走完并关闭
    VIB_RESULT_FAILED   = 2,   // 启动失败
    VIB_RESULT_TIMEOUT  = 3,   // 30s 兜底超时关闭
    VIB_RESULT_STOPPED  = 4,   // 被主动停止（未完成原定定时）
    VIB_RESULT_REJECTED = 5    // 参数 / 状态校验拒绝（未改变输出）
};

// =====================================================
// 初始化（main.cpp setup 第四层）
// =====================================================
void vibration_init();

// =====================================================
// 任务（main.cpp loop() 中调用，非阻塞）
// 负责：定时到期关闭 + 30s 最长运行时间兜底
// =====================================================
void vibration_task();

// =====================================================
// 查询接口
// =====================================================
bool vibration_is_enabled();        // 配置是否启用
bool vibration_is_active();         // 输出当前是否开启
int  vibration_get_last_result();   // VibrationResult
uint32_t vibration_get_last_duration_ms();
uint32_t vibration_get_active_ms();
int  vibration_get_pin();

// =====================================================
// 硬件输出控制（供 Action 与未来安全模块使用）
//
// vibration_stop() 语义：
//   · 若有定时振动 Action 正在等待 → 该 Action 以【失败】结束
//     （不得误报为"正常完成"）
//   · 无论何种模式都会关闭输出
// =====================================================
bool vibration_start(uint32_t duration_ms);
bool vibration_stop();

#endif
