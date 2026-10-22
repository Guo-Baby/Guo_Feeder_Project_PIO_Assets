#ifndef TEST_STEPPER_H
#define TEST_STEPPER_H

#include <Arduino.h>
#include <stdint.h>

// =====================================================
// test_stepper 模块 —— 步进电机【纯测试】输出模块
//
// ★ 定位：仅用于排障期人工测量，不是生产功能。
//
// 与 stepper 模块的区别（刻意的差异）：
//   1. 【无 30 秒限制】—— 启动后持续输出方波直到收到 STOP，
//      便于用万用表逐点测量（EN / DIR / STEP / VMOT / 相电流）。
//      唯一的守护是 TEST_STEPPER_MAX_RUN_MS（默认 10 分钟），
//      到点强制停止，避免电机长时间堵转烧毁。
//   2. 【频率可调】—— hz 参数（10 ~ 2000），默认 100 Hz。
//      用于排查「频率太高导致失步」这一类原因。
//   3. 【脉冲源不同】—— 使用 LEDC 硬件 PWM 产生方波，而不是 RMT。
//      · 与 stepper 的 RMT_CHANNEL_0 不冲突（LEDC 是独立外设）
//      · 若 LEDC 能转、RMT 不能转 ⇒ 直接证明问题在 RMT 输出侧
//      · 若两者都不能转  ⇒ 证明问题在驱动器/电机/电源侧
//
// 硬件注意（必须知道）：
//   · 引脚复用 stepper 模块的配置（step/dir/en），不新增配置文件。
//   · ★ 启动时会用 LEDC 接管 STEP 引脚（覆盖 stepper 的 GPIO 矩阵路由）。
//     测试结束后，stepper 模块的 RMT 输出【无法自动恢复】，
//     必须重启设备才能让 stepper 重新掌控该引脚。
//   · 本模块【不持 Critical Operation】，允许系统正常重启（重启即恢复）。
//
// 职责边界：
//   · 只做「使能 + 定频方波 + 停止」，不含量化账本、不含 120° 语义
//   · 通过 Action 注册接入 Workflow（与其它硬件模块一致）
// =====================================================

// 最长运行时间守护（毫秒）。设为 0 表示不限制（仅在明确知情时使用）。
// 默认 10 分钟：足够逐点测量，又能防止忘停导致电机长时间堵转发热。
#define TEST_STEPPER_MAX_RUN_MS   600000UL

// 允许的频率范围（Hz）
#define TEST_STEPPER_HZ_MIN       10
#define TEST_STEPPER_HZ_MAX       2000
#define TEST_STEPPER_HZ_DEFAULT   100

// =====================================================
// 初始化（main.cpp setup 第四层，需在 stepper_init() 之后）
// =====================================================
void test_stepper_init();

// =====================================================
// 任务（main.cpp loop() 中调用，非阻塞）
// 负责：最长运行时间守护 + 心跳日志
// =====================================================
void test_stepper_task();

// =====================================================
// 启停（供 Action / 串口调试使用）
//   hz        : 10 ~ 2000，超出范围返回 false 且不改动输出
//   direction : 0 = 正转方向，1 = 反转方向
// =====================================================
bool test_stepper_start(uint32_t hz, int direction);
bool test_stepper_stop();          // 幂等：未运行时返回 true（不报错）

// =====================================================
// 查询
// =====================================================
bool     test_stepper_is_running();
uint32_t test_stepper_get_hz();
int      test_stepper_get_direction();
uint32_t test_stepper_get_running_ms();
int      test_stepper_get_step_pin();
int      test_stepper_get_dir_pin();
int      test_stepper_get_en_pin();

#endif
