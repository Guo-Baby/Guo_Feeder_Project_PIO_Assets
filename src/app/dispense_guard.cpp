#include <Arduino.h>
#include "app/dispense_guard.h"
#include "services/event_manager.h"
#include "app/valve.h"
#include "log/log_manager.h"   // Phase 4：观测埋点（EventId / ParamId + log_emit）

// =====================================================
// Dispense Safety Guard
//
// 职责:
//   监听重量异常
//   紧急关闭阀门
//
// 不参与:
//   workflow状态
//   command结果
//   weight计算
//
// =====================================================

// =====================================================
// Phase 4：安全响应 CAUSE 编码（与 valve.cpp:55-59 的 VALVE_CAUSE_* 同构、同值）
//
// 采用与 Valve 侧**相同数值**，便于云端把
//   LOG_DISPENSE_SAFETY_RESPONSE(0x0511) 与 LOG_VALVE_FORCE_CLOSE(0x0505)
// 两条记录按 LOG_P_CAUSE 交叉关联为同一次安全响应。
// 2/3 为预留（与 Valve 侧对齐），本轮不产生。
// =====================================================
enum : uint8_t {
    DISPENSE_CAUSE_WEIGHT_ERROR = 1    // event == EVENT_WEIGHT_ERROR
};

// =====================================================
// Phase 4：埋点职责边界（★ 本模块只记"决策"，不记"输入"也不记"输出"）
// =====================================================
//
// 全链路的记录分工（每段恰好一条，无重复）：
//   ① 重量异常事实 → weight.cpp:236  LOG_WEIGHT_ERROR_ENTER(0x050C, WARN)
//      （weight 是"重量异常"的定义点 ⇒ 由 weight 记；本模块**不重复记录**）
//   ② 安全响应决策 → 本文件 callback  LOG_DISPENSE_SAFETY_RESPONSE(0x0511, WARN)
//      （DispenseGuard 是"决策"的定义点 ⇒ 唯一由本模块记）
//   ③ 关阀动作事实 → valve.cpp:470  LOG_VALVE_FORCE_CLOSE(0x0505, CRITICAL)
//      （valve 是"关阀"的定义点 ⇒ 由 valve 记；本模块**不重复记录**）
//
// ★ 为何 ② 不是 ③ 的重复（决定性的两点）：
//   · valve 侧 0x0505 带 5s 日志门控（VALVE_FORCE_CLOSE_LOG_COOLDOWN_MS）
//     ⇒ 只记"发生过"，**丢失次数**；本 ID 走 LogManager 突发合并 ⇒
//       Σ LOG_P_COUNT = 真实响应次数（信息守恒）。
//   · valve 侧记的是**执行器动作**（GPIO 写入尝试，pin<0 时走 0x0506 失败分支，
//     且 VALVE-1 缺陷下可能返回假成功）；本 ID 记的是**决策**，与执行结果解耦。
//
// ★ 埋点位置 = 过滤通过之后、valve_force_close() 调用之前：
//   · 过滤之后 ⇒ 只有真正的 EVENT_WEIGHT_ERROR 才记
//   · 调用之前 ⇒ "决策 → 执行"在时间序上正确（先记决定，再执行）
//
// ★ 纯观测约束（安全逻辑零改动）：
//   本埋点为独立花括号作用域，**无 return / 无分支 / 无 delay**，
//   不读取也不修改 result ⇒ 短时间多个重量异常时，安全动作次数**不减少**。
// =====================================================

// =====================================================
// Event callback
// =====================================================
static void dispense_guard_event_callback(
    const EventMessage &msg
)
{
    if(
        msg.event != EVENT_WEIGHT_ERROR
    )
    {
        return;
    }
    Serial.println(
        "[DispenseGuard] Weight error received"
    );

    // =====================================================
    // Phase 4 埋点：安全响应决策（WARN）
    //
    // 位置：过滤通过之后、valve_force_close() 之前 —— 本模块唯一业务决策点。
    //
    // 参数：LOG_P_CAUSE = 1（来源事件 = EVENT_WEIGHT_ERROR）。
    //   ★ 必须至少带 1 个参数：LogManager 的 log_coalesce_emit_summary()
    //     在 base_n == 0 时会 **静默丢弃** 折叠计数 ⇒ 不带参将破坏 ΣCOUNT 守恒。
    //   ★ 参数总数必须 < LOG_MAX_PARAMS(8)：合并器要追加 LOG_P_COUNT 占 1 位。
    //     本处 1 个 + 追加 1 个 = 2，安全。
    //
    // 限流：本 EventId 已在 LogManager 的 s_coalesce_targets[] 白名单内
    //   （5s 窗口：首次立即记录，窗口内重复只累计 LOG_P_COUNT）
    //   ⇒ 安全动作次数不变、日志数量下降、Σ LOG_P_COUNT = 真实响应次数。
    //
    // 安全逻辑零改动：无 return / 无分支 / 不读不改 result。
    // =====================================================
    {
        LogParamIn p[1];
        p[0] = log_arg_u32(LOG_P_CAUSE, (uint32_t)DISPENSE_CAUSE_WEIGHT_ERROR);
        log_emit(LOG_DISPENSE_SAFETY_RESPONSE, LOG_LVL_WARN, p, 1);
    }

    bool result =
        valve_force_close();
    if(result)
    {
        Serial.println(
            "[DispenseGuard] Valve force closed"
        );
    }
    else
    {
        Serial.println(
            "[DispenseGuard] Valve force close failed"
        );
    }
}

// =====================================================
// Init
// =====================================================
void dispense_guard_init()
{
    bool result =
        event_subscribe(
            EVENT_WEIGHT_ERROR,
            dispense_guard_event_callback
        );
    if(result)
    {
        Serial.println(
            "[DispenseGuard] Init OK"
        );
    }
    else
    {
        Serial.println(
            "[DispenseGuard] Event subscribe failed"
        );
    }
}