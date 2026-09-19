#include <Arduino.h>

#include "computer_reset.h"
#include "workflow.h"
#include "log_manager.h"   // P2-K：观测埋点（EventId / ParamId + log_emit）

// =====================================================
// Computer Reset 模块 - 实现
//
// 需求来源: AI_TASK.md《Computer Reset Action 功能需求文档》
//
// 执行链路（不新增独立路径）:
//
//   云端
//     -> CloudManager
//     -> CommandManager (execute_action)
//     -> WorkflowManager (Temporary Action 队列)
//     -> COMPUTER_RESET Action (本模块 start/poll)
//     -> GPIO8
//
// 同时也天然支持 Workflow Step 调用（同一份 Descriptor）。
//
// 安全不变式:
//   1. 空闲状态 GPIO8 必须为 LOW
//   2. HIGH 只能由本模块的脉冲流程产生
//   3. 任何退出路径（完成 / 失败 / 超时 / reset / 取消 / 异常）
//      最终都必须使 GPIO8 回到 LOW
// =====================================================

// =====================================================
// 内部常量
// =====================================================

// 同时存在的脉冲上下文上限
//
// 说明: Temporary Action 队列本身是串行的（队头未完成则不推进），
// 但多个 Workflow 可能并行运行各自的 Step Action，因此这里保留
// 一个小容量静态池，避免任何动态分配。
#define COMPUTER_RESET_MAX_INSTANCE 4

// 安全兜底余量
//
// 正常情况下 800ms 到期即由 poll() 拉低。
// 若因实例被强制回收、Workflow 被 Stop/Clear 等异常路径导致
// poll() 不再被调用，由 computer_reset_task() 在
// HOLD_MS + MARGIN 后强制拉低，杜绝 GPIO 长高。
#define COMPUTER_RESET_SAFETY_MARGIN_MS 1200UL

// =====================================================
// 模块私有状态
// =====================================================
static bool initialized = false;

// GPIO 当前实际输出电平（true = HIGH）
static bool output_active = false;

// 最近一次由 LOW -> HIGH 的时间戳（0 = 输出为 LOW）
static unsigned long pulse_start_ms = 0;

// 当前活跃脉冲数量
//
// 输出仲裁：> 0 时保持 HIGH，归零时拉低。
// 这样并发的多个实例不会互相踩踏，最后一个结束者负责拉低。
static uint8_t active_pulse_count = 0;

// =====================================================
// 脉冲运行时上下文（静态池，禁止动态分配）
//
// used   : 槽位占用标记，同时用于保证 release 幂等
// manual : true = 由 computer_reset_trigger() 产生，
//                由 computer_reset_task() 到期回收；
//          false = 由 Workflow Action 产生，由 poll() 回收
// =====================================================
struct ComputerResetCtx
{
    bool used;
    bool manual;
    unsigned long start_ms;
};

static ComputerResetCtx computer_reset_ctx_pool[COMPUTER_RESET_MAX_INSTANCE];

// 前向声明（实现位于文件后段）
static void computer_reset_set_output(bool active);

// =====================================================
// 上下文池分配 / 释放
// =====================================================
static ComputerResetCtx *computer_reset_ctx_alloc()
{
    for(uint8_t i = 0; i < COMPUTER_RESET_MAX_INSTANCE; i++)
    {
        if(!computer_reset_ctx_pool[i].used)
        {
            computer_reset_ctx_pool[i].used = true;
            computer_reset_ctx_pool[i].manual = false;
            computer_reset_ctx_pool[i].start_ms = 0;
            return &computer_reset_ctx_pool[i];
        }
    }
    return nullptr;
}

// 释放单个上下文。
//
// used 标记保证幂等：同一上下文被重复释放不会导致
// active_pulse_count 下溢，也就不会误拉低正在进行的脉冲。
static void computer_reset_ctx_release(ComputerResetCtx *ctx)
{
    if(ctx == nullptr || !ctx->used)
    {
        return;
    }

    ctx->used = false;

    if(active_pulse_count > 0)
    {
        active_pulse_count--;
    }

    // 最后一个活跃脉冲结束 -> 拉低 GPIO
    if(active_pulse_count == 0)
    {
        computer_reset_set_output(false);
    }
}

// =====================================================
// 强制回到空闲态（异常路径兜底）
//
// 清空所有上下文与计数，无条件拉低 GPIO。
// 仅由异常路径与 task() 安全兜底调用。
// =====================================================
static void computer_reset_force_idle()
{
    for(uint8_t i = 0; i < COMPUTER_RESET_MAX_INSTANCE; i++)
    {
        computer_reset_ctx_pool[i].used = false;
        computer_reset_ctx_pool[i].manual = false;
        computer_reset_ctx_pool[i].start_ms = 0;
    }

    active_pulse_count = 0;
    computer_reset_set_output(false);
}

// =====================================================
// 内部函数：GPIO 输出控制（唯一写 GPIO 的出口）
//
// 所有对 GPIO8 的写操作都必须经由本函数，
// 保证 output_active / pulse_start_ms 与实际电平严格同步。
// =====================================================
static void computer_reset_set_output(bool active)
{
    if(!initialized)
    {
        return;
    }

    // 相同电平不重复写
    if(active == output_active)
    {
        return;
    }

    digitalWrite(COMPUTER_RESET_PIN, active ? HIGH : LOW);
    output_active = active;

    if(active)
    {
        pulse_start_ms = millis();
    }
    else
    {
        pulse_start_ms = 0;
    }

    // ---- P2-K 埋点：LOG_CRESET_PULSE（INFO，LOW -> HIGH 上升沿）----
    //
    // 为什么挂在这里而不是 computer_reset_trigger()：
    //   脉冲有**两条产生路径** ——
    //     ① 手动 `computer_reset_trigger()`（main.cpp 控制台命令）
    //     ② Workflow Action `computer_reset_action_start()`
    //   二者最终都经本函数拉高 GPIO。若只挂 ①，则 ② 路径产生的脉冲
    //   **完全没有日志**（漏记）。挂在本函数的上升沿可**同时覆盖两条路径**。
    //
    // 为什么无需去重门控（天然边沿）：
    //   本函数开头 `if(active == output_active) return;` 已保证**同电平不重复写**
    //   ⇒ 进入此处必然是一次真实的 LOW -> HIGH 跳变，不可能被连续进入。
    //
    // 参数：HOLD_MS = 本次脉冲的设计保持时长（常量，非实测）
    //       实际时长由 poll()/task() 到期时刻决定，异常时由 SAFETY_TIMEOUT 记录。
    if(active)
    {
        LogParamIn p[1];
        p[0] = log_arg_u32(LOG_P_HOLD_MS, (uint32_t)COMPUTER_RESET_HOLD_MS);
        log_emit(LOG_CRESET_PULSE, LOG_LVL_INFO, p, 1);
    }

    Serial.printf(
        "[ComputerReset] GPIO%d = %s\n",
        COMPUTER_RESET_PIN,
        active ? "HIGH" : "LOW"
    );
}

// =====================================================
// Workflow Action 实现（异步完成）
//
// 生命周期: reset -> start -> poll -> SUCCESS
//
// reset: 复位实例状态；若该实例仍持有上下文则释放，
//        保证 Action 被取消 / Workflow 被 Stop 时 GPIO 能回到 LOW。
// start: 分配上下文、占用输出、拉高 GPIO，进入 RUNNING。
// poll : 非阻塞检查 elapsed；到期释放输出并置 SUCCESS。
// =====================================================

// Action 参数（AI_TASK.md §7：本版本不增加参数）
static WorkflowParam computer_reset_params[] = {
    // 预留
};

static void computer_reset_action_reset(WorkflowActionInstance *action);
static void computer_reset_action_start(WorkflowActionInstance *action);
static void computer_reset_action_poll(WorkflowActionInstance *action);

// Action ID 说明:
//   AI_TASK.md §11 的功能名为 computer_reset；
//   项目现有 Action runtime id 全部为大写下划线（VALVE_OPEN / VALVE_CLOSE /
//   WEIGHT_ZERO / MI_THERMO_START_SCAN），为保持 Capability Registry 的
//   ID 风格一致，这里使用 COMPUTER_RESET 作为 runtime id，
//   module 字段使用 computer_reset。
static WorkflowActionDescriptor computer_reset_action_desc = {
    .id          = "COMPUTER_RESET",
    .name        = "电脑重启",
    .module      = "computer_reset",
    .description = "GPIO8 输出 800ms 高电平脉冲，模拟电脑主板 Reset 按键",
    .params      = computer_reset_params,
    .param_count = 0,
    .reset       = computer_reset_action_reset,
    .start       = computer_reset_action_start,
    .poll        = computer_reset_action_poll
};

static void computer_reset_action_reset(WorkflowActionInstance *action)
{
    if(action == nullptr)
    {
        return;
    }

    // 若该实例上一次执行尚未收尾（被 Stop / Clear / 重置打断），
    // 这里必须释放上下文，否则 GPIO 会一直保持 HIGH。
    ComputerResetCtx *ctx = (ComputerResetCtx *)action->runtime;
    if(ctx != nullptr)
    {
        computer_reset_ctx_release(ctx);
        action->runtime = nullptr;
    }

    action->result  = ACTION_IDLE;
    action->running = false;
}

static void computer_reset_action_start(WorkflowActionInstance *action)
{
    if(action == nullptr)
    {
        return;
    }

    if(!initialized)
    {
        Serial.println("[ComputerReset] Action rejected: module not initialized");
        action->result = ACTION_FAILED;
        return;
    }

    ComputerResetCtx *ctx = computer_reset_ctx_alloc();
    if(ctx == nullptr)
    {
        Serial.println("[ComputerReset] Action rejected: ctx pool exhausted");

        // ---- P2-K 埋点：LOG_CRESET_POOL_EXHAUSTED（ERROR）----
        // 天然边沿：进入即代表 4 个槽位全部占用（见下方 computer_reset_trigger()
        // 同分支注释）。ERROR 级 ⇒ 落 Flash 且上云，作为**扩容依据**。
        {
            LogParamIn p[2];
            p[0] = log_arg_u32(LOG_P_ACTIVE,   (uint32_t)active_pulse_count);
            p[1] = log_arg_u32(LOG_P_CAPACITY, (uint32_t)COMPUTER_RESET_MAX_INSTANCE);
            log_emit(LOG_CRESET_POOL_EXHAUSTED, LOG_LVL_ERROR, p, 2);
        }

        action->result = ACTION_FAILED;
        return;
    }

    ctx->manual   = false;
    ctx->start_ms = millis();
    action->runtime = ctx;

    // 占用输出（计数归零者负责拉低）
    active_pulse_count++;
    computer_reset_set_output(true);

    Serial.printf(
        "[ComputerReset] Pulse started (hold=%lu ms, active=%u)\n",
        COMPUTER_RESET_HOLD_MS,
        (unsigned)active_pulse_count
    );

    // 异步执行：等待 poll() 判定 800ms 到期
    action->result = ACTION_RUNNING;
}

static void computer_reset_action_poll(WorkflowActionInstance *action)
{
    if(action == nullptr)
    {
        return;
    }

    // 注意:
    //   Workflow / 临时 Action 引擎在 start() 返回后【必定】再调用一次 poll()。
    //   因此 poll 必须能正确处理"start 失败"这一轮，
    //   不能因为 result 已经是 FAILED 就去做全局强制复位，
    //   否则会误伤其他并发实例的脉冲。

    ComputerResetCtx *ctx = (ComputerResetCtx *)action->runtime;

    // ---- 1) 没有运行时上下文 ----
    if(ctx == nullptr)
    {
        // start 失败（result 已为 FAILED）或已完成（result 已为 SUCCESS）:
        // 本实例不再占用输出，保持既有结果返回，不触碰 GPIO。
        if(action->result != ACTION_RUNNING)
        {
            return;
        }

        // 处于 RUNNING 却没有上下文: 属于不可能出现的异常路径。
        // 强制回到空闲态，杜绝 GPIO 长高。
        Serial.println("[ComputerReset] Poll RUNNING without ctx, force idle");
        computer_reset_force_idle();
        action->result = ACTION_FAILED;
        return;
    }

    // ---- 2) 未 start 而直接 poll（异常路径）----
    if(action->result == ACTION_IDLE)
    {
        Serial.println("[ComputerReset] Poll before start, aborted");
        computer_reset_ctx_release(ctx);
        action->runtime = nullptr;
        action->result = ACTION_FAILED;
        return;
    }

    // ---- 3) 已经结束但上下文未回收（异常路径）----
    //    补一次释放（used 标记保证幂等），保持原结果不变。
    if(action->result != ACTION_RUNNING)
    {
        computer_reset_ctx_release(ctx);
        action->runtime = nullptr;
        return;
    }

    // ---- 4) 正常路径: 非阻塞等待 800ms ----
    unsigned long elapsed = (unsigned long)(millis() - ctx->start_ms);
    if(elapsed >= COMPUTER_RESET_HOLD_MS)
    {
        computer_reset_ctx_release(ctx);
        action->runtime = nullptr;

        Serial.printf(
            "[ComputerReset] Pulse finished (elapsed=%lu ms, active=%u)\n",
            elapsed,
            (unsigned)active_pulse_count
        );

        action->result = ACTION_SUCCESS;
    }
    else
    {
        action->result = ACTION_RUNNING;
    }
}

// =====================================================
// 初始化
// =====================================================
void computer_reset_init()
{
    // 1. 清空全部运行时状态
    for(uint8_t i = 0; i < COMPUTER_RESET_MAX_INSTANCE; i++)
    {
        computer_reset_ctx_pool[i].used      = false;
        computer_reset_ctx_pool[i].manual    = false;
        computer_reset_ctx_pool[i].start_ms  = 0;
    }
    active_pulse_count = 0;
    output_active      = false;
    pulse_start_ms     = 0;

    // 2. GPIO8 配置为输出并立即置为安全状态 LOW
    pinMode(COMPUTER_RESET_PIN, OUTPUT);
    digitalWrite(COMPUTER_RESET_PIN, LOW);

    initialized = true;

    // 3. 注册 Workflow Action（id 全局唯一，重复注册返回 false）
    if(!workflow_register_action(&computer_reset_action_desc))
    {
        Serial.println("[ComputerReset] COMPUTER_RESET register failed");
    }

    Serial.printf(
        "[ComputerReset] Init OK (pin=%d, hold=%lu ms)\n",
        COMPUTER_RESET_PIN,
        COMPUTER_RESET_HOLD_MS
    );
}

// =====================================================
// 任务（loop 中调用）
// =====================================================
void computer_reset_task()
{
    if(!initialized)
    {
        return;
    }

    unsigned long now = millis();

    // ---- 1) 回收手动触发的脉冲 ----
    for(uint8_t i = 0; i < COMPUTER_RESET_MAX_INSTANCE; i++)
    {
        ComputerResetCtx &ctx = computer_reset_ctx_pool[i];
        if(!ctx.used || !ctx.manual)
        {
            continue;
        }
        if((unsigned long)(now - ctx.start_ms) >= COMPUTER_RESET_HOLD_MS)
        {
            computer_reset_ctx_release(&ctx);
        }
    }

    // ---- 2) 安全兜底 ----
    //
    // 正常脉冲 800ms 内必被 poll() / 上面的手动回收处理。
    // 若输出 HIGH 超过 HOLD_MS + MARGIN，说明有实例未走完正常收尾
    // （被 Stop / Clear / 异常中断），这里无条件拉低。
    if(!output_active || pulse_start_ms == 0)
    {
        return;
    }

    if((unsigned long)(now - pulse_start_ms) >=
       (COMPUTER_RESET_HOLD_MS + COMPUTER_RESET_SAFETY_MARGIN_MS))
    {
        // ---- P2-K 埋点：LOG_CRESET_SAFETY_TIMEOUT（WARN）----
        //
        // 天然边沿：紧随其后的 `computer_reset_force_idle()` 会置
        // output_active=false，而本函数上方 `if(!output_active) return;`
        // 会拦住后续调用 ⇒ **不可能连续两次进入本块**，无需去重门控。
        //
        // 语义：GPIO8 卡在 HIGH 超过设计上限 ⇒ 有硬件风险
        //       （持续按住电脑 Reset 键），故为 WARN。
        //
        // ★ 取值时机：DURATION_MS 必须在 force_idle() **之前**计算，
        //   因为它依赖的 pulse_start_ms 会被 force_idle() 经 set_output(false)
        //   清零（本文件 `pulse_start_ms = 0`）—— 属 LOG-13 铁律
        //   "取值用于日志的变量须在源头清零前取"（`OPEN_MS` 曾踩过 3 次）。
        {
            LogParamIn p[2];
            p[0] = log_arg_u32(LOG_P_HOLD_MS, (uint32_t)COMPUTER_RESET_HOLD_MS);
            p[1] = log_arg_u32(LOG_P_DURATION_MS, (uint32_t)(now - pulse_start_ms));
            log_emit(LOG_CRESET_SAFETY_TIMEOUT, LOG_LVL_WARN, p, 2);
        }

        Serial.printf(
            "[ComputerReset] Safety timeout! Forced LOW (HIGH > %lu ms)\n",
            COMPUTER_RESET_HOLD_MS + COMPUTER_RESET_SAFETY_MARGIN_MS
        );
        computer_reset_force_idle();
    }
}

// =====================================================
// 对外接口：手动触发一次脉冲
// =====================================================
bool computer_reset_trigger()
{
    if(!initialized)
    {
        return false;
    }

    ComputerResetCtx *ctx = computer_reset_ctx_alloc();
    if(ctx == nullptr)
    {
        Serial.println("[ComputerReset] Manual trigger rejected: ctx pool exhausted");

        // ---- P2-K 埋点：LOG_CRESET_POOL_EXHAUSTED（ERROR）----
        // 与 computer_reset_action_start() 的同名分支共用同一 EventId
        // （同一语义的两个产生路径，与 PULSE 的双路径处理一致）。
        {
            LogParamIn p[2];
            p[0] = log_arg_u32(LOG_P_ACTIVE,   (uint32_t)active_pulse_count);
            p[1] = log_arg_u32(LOG_P_CAPACITY, (uint32_t)COMPUTER_RESET_MAX_INSTANCE);
            log_emit(LOG_CRESET_POOL_EXHAUSTED, LOG_LVL_ERROR, p, 2);
        }

        return false;
    }

    ctx->manual   = true;
    ctx->start_ms = millis();

    active_pulse_count++;
    computer_reset_set_output(true);

    Serial.printf(
        "[ComputerReset] Manual pulse started (active=%u)\n",
        (unsigned)active_pulse_count
    );

    return true;
}

// =====================================================
// 对外接口：查询输出是否处于 HIGH
// =====================================================
bool computer_reset_is_active()
{
    return output_active;
}
