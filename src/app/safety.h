#ifndef SAFETY_H
#define SAFETY_H

#include <Arduino.h>
#include <stdint.h>

// =====================================================
// safety 模块 —— 启动安全电平 + 紧急安全停堆
//
// 定位（两条职责，都必须在这里，不能散落到各模块）：
//   1. `safety_outputs_init()`  启动期把三个执行器引脚**尽早**置为安全电平
//      （早于 WiFi/BLE 初始化），避免复位后引脚高阻导致继电器误动作。
//   2. `safety_stop_all()`      紧急停止的**编排层**：
//      停 Workflow 账本 → 停各执行器物理输出 → 逐项读回 → 组报告。
//
// 为什么需要"编排层"（这是本模块存在的**唯一**理由）：
//   `workflow_stop()` 只清状态：它释放 Critical Operation、把 state 置 IDLE、
//   清 cmd_id/finish_callback，但**不碰任何硬件**。而 `WorkflowActionDescriptor`
//   只有 reset/start/poll，没有 stop ⇒ 无法从 Workflow 层"停掉一个正在跑的 Action"。
//   所以"停止"必须由知道各执行器物理停止 API 的一层来做 —— 就是这里。
//
// 边界（不得越界）：
//   · 只调**公开 API**：workflow_stop / valve_force_close /
//     stepper_emergency_stop / vibration_stop / test_stepper_stop
//   · 不直接改 `workflows[i].state`、不直接写 GPIO（除 safety_outputs_init 的启动期）
//   · 不注册 Action / Trigger（注册会重排 stable_id 并递增 registry 版本，
//     导致云端所有缓存的 k 失效 —— 所以它走独立的 `c="safety"` 一级命令）
//
// 诚实性（刻意设计）：
//   `verified` 字段当前**恒为 false**。固件没有任何执行器状态回读通道
//   （阀只有 GPIO 电平镜像、步进只有 s_busy、振动只有 s_output_on），
//   这些都只是"意图镜像"，不是电气测量。⇒ 不得上报"已物理停止"。
// =====================================================

/** 单个执行器的处置结果 */
struct SafetyActuatorResult
{
    bool         attempted;   // 是否尝试过
    bool         ok;          // 命令是否被执行（false 也可能是"本来就没动"，见 note）
    bool         verified;    // 是否经独立回读确认（**恒 false**，见文件头）
    bool         was_active;  // 处置前是否处于活动状态（尽力判断）
    const char  *note;        // 固定字符串，不进堆
};

/** 一次停堆的完整报告（全静态字段，可安全放栈上，约 90 B） */
struct SafetyReport
{
    uint8_t  workflows_seen;      // 扫描到的有效 workflow 槽位数
    uint8_t  workflows_stopped;   // 实际执行了 stop 的数量
    uint8_t  temp_cancelled;      // 被取消的排队临时 Action 数量（F-1 补强）
    bool     any_was_running;     // 是否原本有 RUNNING/WAITING 的流程

    SafetyActuatorResult valve;
    SafetyActuatorResult stepper;
    SafetyActuatorResult vibration;
    SafetyActuatorResult test_stepper;

    bool     latched;             // 停堆后是否处于"已停机"锁态
    bool     all_ok;              // 所有被尝试项均 ok
    uint32_t elapsed_ms;          // 编排耗时（仅统计，不用作判据）
};

/**
 * 启动期安全电平初始化。
 *
 * ★ 调用位置是**硬约束**：`config_init()` 之后、`ble_init()`/`wifi_init()` **之前**。
 *   理由：GPIO 复位默认是输入/高阻，继电器模块的输入若浮空可能被拉向导通侧；
 *   把这一步放到 WiFi/BLE 之后会留下 200~1500 ms 的未定义窗口。
 *
 * 只做三件事：配成输出 → 写安全电平 → 打印一行。不注册 Action、不改模块状态。
 * 失败（引脚未配置 / 配置读不到）**不阻塞启动**，只打 WARN。
 */
void safety_outputs_init();

/**
 * 紧急安全停堆。
 *
 * 顺序（不得调整）：
 *   ① 置 latched（先立牌子，防止同一 loop 内已排队的下一步又开阀）
 *   ② 逐个 workflow_stop（账本层）
 *   ②b 取消排队中的临时 Action（账本层，回调云端失败）
 *   ③ 阀 → 步进 → 振动 → 步进测试（物理层）
 *   ④ 逐项收集结果（不做电气验证，见文件头）
 *
 * @param out 输出报告（调用方提供，函数内 memset）
 * @return true = 至少完成了一次有效处置（不代表全部成功，看 out.all_ok）
 *
 * 幂等：无 workflow、执行器本来就没动，都返回 true（安全方向不判失败）。
 */
bool safety_stop_all(SafetyReport &out);

/** 解除停机锁（`safety.clear`）。返回是否真的从锁定状态切换 */
bool safety_clear_latch();

/** 当前是否处于停机锁态 */
bool safety_is_latched();

/**
 * [F-1 补强] 锁存期间某个 **Action** 是否必须被拒绝。
 *
 * 语义：**白名单 + 失败即拒绝**
 *   · 只有"停止类 / 传感器类 / 无执行器副作用"的 Action 被放行；
 *   · 其它一切（含将来新增但未登记进白名单的 Action）在锁存期间一律拒绝。
 *
 * 为什么用白名单而不是黑名单：
 *   黑名单在新增 Action 时默认放行 ⇒ 新 Action 会**静默绕过**急停，
 *   而且没有任何编译期或运行期提示。白名单的失效方向是"多拦一个"，
 *   最坏结果是用户必须显式 clear 一次，而不是电机动起来。
 *
 * 放行的 Action 及其理由（逐条可核）：
 *   VALVE_CLOSE              只关阀，安全方向
 *   STEPPER_EMERGENCY_STOP   只停步进，安全方向
 *   TEST_STEPPER_STOP        只停测试步进，安全方向
 *   VIBRATION_STOP           只停振动，安全方向
 *   MI_THERMO_STOP_SCAN      只停扫描，无执行器
 *
 * ★ 2026-10-23 裁决后**移出**白名单（锁存期间一律拒绝）：
 *   WEIGHT_ZERO              置皮重会让"秤是否被改动"不可判定
 *   MI_THERMO_START_SCAN     启动持续扫描不是"停止"动作
 * 判据：**这个 Action 会不会让系统离开"更停"的状态**。会 ⇒ 拒绝。
 *
 * 注意：本判定**只**决定"是否拒绝"，不改变任何模块状态；
 *       真正的物理停止仍然只由 `safety_stop_all()` 编排。
 *
 * @param action_id Action 的 runtime id（如 "VALVE_OPEN"）
 * @return true = 锁存中且不属于白名单 ⇒ 调用方必须拒绝
 */
bool safety_action_blocked_by_latch(const String &action_id);

#endif // SAFETY_H
