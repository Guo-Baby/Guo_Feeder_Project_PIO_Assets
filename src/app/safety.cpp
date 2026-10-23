#include "app/safety.h"

#include <cstring>

#include "driver/gpio.h"

#include "app/valve.h"
#include "app/stepper.h"
#include "app/vibration.h"
#include "app/test_stepper.h"
#include "automation/workflow.h"
#include "services/config_manager.h"

// =====================================================
// 内部状态
//
// `s_latched` 只表示"最近一次停堆之后还没有解除"。
// 它**不是**安全前提，也不用于判断硬件状态 —— 只用于阻止
// Workflow 在停堆后自动继续推进（见 command_manager 的检查点）。
// =====================================================
static bool s_latched = false;

// =====================================================
// 启动期：把单个引脚配成输出并写安全电平
//
// 顺序必须是「先 gpio_config(OUTPUT) → 再 gpio_set_level」：
//   反过来写不进去（引脚此时还是输入）。
// 安全电平 = 有效电平取反：
//   阀 active_level=1（高开）⇒ 安全 = LOW
//   步进 EN active_level=0（低使能）⇒ 失能 = HIGH
//   振动 active_level=1（高开）⇒ 安全 = LOW
// =====================================================
static void safety_init_output(int pin, bool active_level, const char *tag)
{
    if (pin < 0)
    {
        Serial.printf("[Safety] %s: pin not configured, skip\n", tag);
        return;
    }

    gpio_config_t io = {};
    io.pin_bit_mask = (1ULL << pin);
    io.mode         = GPIO_MODE_OUTPUT;
    io.pull_up_en   = GPIO_PULLUP_DISABLE;
    io.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io.intr_type    = GPIO_INTR_DISABLE;

    esp_err_t err = gpio_config(&io);
    if (err != ESP_OK)
    {
        Serial.printf("[Safety] %s: gpio_config failed pin=%d err=%d\n", tag, pin, (int)err);
        return;
    }

    const int safe_level = active_level ? 0 : 1;
    gpio_set_level((gpio_num_t)pin, safe_level);

    Serial.printf("[Safety] %s: pin=%d safe_level=%d\n", tag, pin, safe_level);
}

void safety_outputs_init()
{
    /*
     * 只做 GPIO 电平，**不**初始化模块、**不**注册 Action、**不**读模块状态。
     * 三个引脚全部来自 config（不得硬编码）：继电器有高/低触发两种模块，
     * 写死电平会在另一种模块上变成"上电即导通"。
     *
     * ★ 这一步只降低风险，不消除风险：
     *   · 从复位到本函数执行仍有极短的未定义窗口（GPIO 复位默认高阻）
     *   · 继电器模块输入级是否有下拉、是否光耦隔离，取决于实际电路
     *   ⇒ 必须用万用表实测（见 docs 的烧录后验证清单）
     */
    safety_init_output(config_get_valve_gpio_pin(),
                       config_get_valve_active_level() != 0,
                       "valve");

    safety_init_output(config_get_stepper_en_pin(),
                       config_get_stepper_en_active_level() != 0,
                       "stepper_en");

    safety_init_output(config_get_vibration_relay_pin(),
                       config_get_vibration_active_level() != 0,
                       "vibration");

    Serial.println("[Safety] outputs init done (network not started yet)");
}

// =====================================================
// 紧急安全停堆
// =====================================================
bool safety_stop_all(SafetyReport &out)
{
    memset(&out, 0, sizeof(out));

    const uint32_t t0 = millis();

    /*
     * ① 先立牌子。
     *
     * 为什么必须在停硬件**之前**置位：
     *   同一个 loop 里可能已经排好了 workflow 的下一步 start。
     *   若先停硬件再置位，那一步会在两者之间把阀重新打开。
     */
    s_latched = true;

    /* ② 账本层：停掉所有非 IDLE 的 workflow */
    for (uint8_t i = 0; i < WORKFLOW_MAX_COUNT; i++)
    {
        if (!workflow_slot_occupied(i))
        {
            continue;
        }

        Workflow *w = workflow_get(i);
        if (w == nullptr)
        {
            continue;
        }

        out.workflows_seen++;

        if (w->state != WORKFLOW_IDLE)
        {
            out.any_was_running = true;
            if (workflow_stop(w->id))
            {
                out.workflows_stopped++;
            }
        }
    }

    /* ③ 物理层：阀 —— 最优先，漏水是唯一"持续造成损失"的失效模式 */
    out.valve.attempted  = true;
    out.valve.was_active = valve_is_open();
    out.valve.ok         = valve_force_close();
    out.valve.verified   = false;
    out.valve.note       = out.valve.ok
        ? (out.valve.was_active ? "force_closed" : "already_closed")
        : "pin_not_configured";

    /* ④ 物理层：步进（正常模块，可能有 RMT 正在输出） */
    out.stepper.attempted  = true;
    out.stepper.ok         = stepper_emergency_stop();
    out.stepper.was_active = out.stepper.ok;   // 该 API 仅在确实停了动作时返回 true
    out.stepper.verified   = false;
    out.stepper.note       = out.stepper.ok ? "estop" : "no_action_running";

    /* ⑤ 物理层：振动（单 GPIO，两马达共路） */
    out.vibration.attempted  = true;
    out.vibration.was_active = vibration_is_active();
    out.vibration.ok         = vibration_stop();
    out.vibration.verified   = false;
    out.vibration.note       = out.vibration.ok
        ? (out.vibration.was_active ? "stopped" : "already_off")
        : "module_not_ready";

    /*
     * ⑥ 物理层：步进测试模块。
     *
     * ★ 这个必须停：它绕过正式 stepper 的 30 秒限制、可长时间通电，
     *   是现场最容易"忘了停"的输出。它的 stop 是幂等的。
     */
    out.test_stepper.attempted  = true;
    out.test_stepper.was_active = test_stepper_is_running();
    out.test_stepper.ok         = test_stepper_stop();
    out.test_stepper.verified   = false;
    out.test_stepper.note       = out.test_stepper.was_active
        ? (out.test_stepper.ok ? "stopped" : "stop_failed")
        : "not_running";

    /*
     * ⑦ 汇总。
     *
     * "本来就没动"算安全（幂等语义），"想停但停不掉"才算失败：
     *   · 阀：pin 未配置 ⇒ 硬件不存在，视为安全
     *   · 步进：no_action_running ⇒ 安全
     *   · 振动：module_not_ready（模块禁用/未初始化）⇒ 安全
     */
    const bool valve_safe   = out.valve.ok || (out.valve.note != nullptr
                                              && strcmp(out.valve.note, "pin_not_configured") == 0);
    const bool stepper_safe = out.stepper.ok || !out.stepper.was_active;
    const bool vib_safe     = out.vibration.ok || !out.vibration.was_active;
    const bool test_safe    = out.test_stepper.ok || !out.test_stepper.was_active;

    out.latched    = s_latched;
    out.all_ok     = valve_safe && stepper_safe && vib_safe && test_safe;
    out.elapsed_ms = millis() - t0;

    Serial.printf(
        "[Safety] stop_all: wf_seen=%u wf_stopped=%u running=%d "
        "valve=%d(%s) stepper=%d(%s) vib=%d(%s) tstep=%d(%s) all_ok=%d %lu ms\n",
        (unsigned)out.workflows_seen,
        (unsigned)out.workflows_stopped,
        out.any_was_running ? 1 : 0,
        out.valve.ok ? 1 : 0, out.valve.note ? out.valve.note : "?",
        out.stepper.ok ? 1 : 0, out.stepper.note ? out.stepper.note : "?",
        out.vibration.ok ? 1 : 0, out.vibration.note ? out.vibration.note : "?",
        out.test_stepper.ok ? 1 : 0, out.test_stepper.note ? out.test_stepper.note : "?",
        out.all_ok ? 1 : 0,
        (unsigned long)out.elapsed_ms
    );

    return true;
}

bool safety_clear_latch()
{
    if (!s_latched)
    {
        return false;
    }
    s_latched = false;
    Serial.println("[Safety] latch cleared");
    return true;
}

bool safety_is_latched()
{
    return s_latched;
}
