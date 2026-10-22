// =====================================================
// test_stepper.cpp —— 步进电机纯测试输出模块
//
// 设计要点：
//   ① 无 30 秒限制（仅 TEST_STEPPER_MAX_RUN_MS 守护）
//   ② 频率可调（hz 参数）
//   ③ 脉冲源 = LEDC 硬件 PWM（与 stepper 的 RMT 解耦，可作对照诊断）
//   ④ 通过 Workflow Action 注册，MQTT 走正式链路
//
// Action：
//   TEST_STEPPER_START  {hz?:int, direction?:int}   启动并立即返回成功
//   TEST_STEPPER_STOP                               停止并返回成功
//
// 串口调试（main.cpp）：
//   tstep_start [hz] [dir] / tstep_stop / tstep_status
// =====================================================

#include <Arduino.h>
#include <stdint.h>

#include "app/test_stepper.h"
#include "app/stepper.h"                // stepper_is_busy / stepper_is_enabled
#include "services/config_manager.h"
#include "automation/workflow.h"

// LEDC 通道：ESP32-S3 共 8 路（0~7）。工程此前未使用 LEDC，取 7 号避免与未来冲突。
#define TS_LEDC_CH        7

// =====================================================
// 模块状态
// =====================================================
static bool          s_initialized   = false;
static bool          s_running       = false;
static uint32_t      s_hz            = TEST_STEPPER_HZ_DEFAULT;
static int           s_direction     = 0;
static unsigned long s_start_ms      = 0;
static unsigned long s_last_log_ms   = 0;
static uint8_t       s_res_bits      = 0;

static int s_step_pin    = -1;
static int s_dir_pin     = -1;
static int s_en_pin      = -1;
static int s_en_active   = 0;       // 0 = 低电平使能（DRV8825 默认）
static bool s_dir_invert = false;

// =====================================================
// 日志（结构化日志码为 P1.1 冻结契约，本模块仅用串口）
// =====================================================
static void ts_log(const char *fmt, ...)
{
    char buf[192];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    Serial.print("[TStep] ");
    Serial.println(buf);
}

// =====================================================
// 引脚输出
// =====================================================
static void ts_write_en(bool enable)
{
    if (s_en_pin < 0) {
        return;
    }
    int level = enable ? (s_en_active != 0 ? 1 : 0)
                       : (s_en_active != 0 ? 0 : 1);
    gpio_set_level((gpio_num_t)s_en_pin, level);
}

static void ts_write_dir(int direction)
{
    if (s_dir_pin < 0) {
        return;
    }
    int level = (direction != 0) ? 1 : 0;
    if (s_dir_invert) {
        level = level ? 0 : 1;
    }
    gpio_set_level((gpio_num_t)s_dir_pin, level);
}

// =====================================================
// 选择 LEDC 分辨率
//
// LEDC: f = 80MHz / (2^res * divider)，divider ∈ [1, 1023.996]
// 从高分辨率往低找第一个满足的档位（分辨率越高，占空比越精确）
// 返回 0 表示该频率在 LEDC 能力之外
// =====================================================
static uint8_t ts_pick_res(uint32_t hz)
{
    if (hz == 0) {
        return 0;
    }
    const uint64_t base = 80000000ULL;      // APB 80 MHz
    for (int res = 16; res >= 6; --res) {
        uint64_t div = base / ((uint64_t)hz * (1ULL << res));
        if (div >= 1 && div <= 1023) {
            return (uint8_t)res;
        }
    }
    return 0;
}

// =====================================================
// 内部：停止输出（所有退出路径共用）
//
// 顺序：先停脉冲 → 再断 STEP 引脚 → 最后禁 EN
// ★ 不得只禁 EN 而让 PWM 继续输出
// =====================================================
static void ts_stop_internal(const char *reason)
{
    if (!s_running) {
        return;
    }

    // ① 停 LEDC：占空比清零 → 输出恒定低（不再产生上升沿）
    ledcWrite(TS_LEDC_CH, 0);

    // ② 解除 LEDC 对 GPIO 的矩阵路由，并恢复为普通输出低
    ledcDetachPin(s_step_pin);
    if (s_step_pin >= 0) {
        gpio_config_t io;
        io.intr_type    = GPIO_INTR_DISABLE;
        io.mode         = GPIO_MODE_OUTPUT;
        io.pin_bit_mask = (1ULL << s_step_pin);
        io.pull_down_en = GPIO_PULLDOWN_DISABLE;
        io.pull_up_en   = GPIO_PULLUP_DISABLE;
        gpio_config(&io);
        gpio_set_level((gpio_num_t)s_step_pin, 0);
    }

    // ③ 禁用驱动器
    ts_write_en(false);

    s_running = false;

    ts_log("STOP (%s): ran %lu ms at %lu Hz, dir=%d; EN disabled",
           reason ? reason : "-",
           (unsigned long)(millis() - s_start_ms),
           (unsigned long)s_hz, s_direction);
}

// =====================================================
// 对外：启动
// =====================================================
bool test_stepper_start(uint32_t hz, int direction)
{
    if (!s_initialized) {
        ts_log("start refused: module not initialized");
        return false;
    }
    if (hz < TEST_STEPPER_HZ_MIN || hz > TEST_STEPPER_HZ_MAX) {
        ts_log("start refused: hz=%lu out of range [%d, %d]",
               (unsigned long)hz, TEST_STEPPER_HZ_MIN, TEST_STEPPER_HZ_MAX);
        return false;
    }
    if (direction != 0 && direction != 1) {
        ts_log("start refused: direction=%d, expect 0 or 1", direction);
        return false;
    }
    if (s_step_pin < 0 || s_dir_pin < 0 || s_en_pin < 0) {
        ts_log("start refused: pins invalid (step=%d dir=%d en=%d)",
               s_step_pin, s_dir_pin, s_en_pin);
        return false;
    }
    // 正式 Action 正在跑时不抢引脚（否则会静默打断正式动作）
    if (stepper_is_busy()) {
        ts_log("start refused: formal stepper action is running");
        return false;
    }

    // 已在运行：先停再按新参数重启（方便边测边改频率）
    if (s_running) {
        ts_stop_internal("restart_with_new_params");
    }

    uint8_t res = ts_pick_res(hz);
    if (res == 0) {
        ts_log("start refused: hz=%lu unsupported by LEDC", (unsigned long)hz);
        return false;
    }

    // ① 方向先建立（避免方向未定时的任何脉冲）
    //    注意：DIR/EN 可能在 stepper 未启用时尚未配置为输出，此处幂等配置
    gpio_config_t io;
    io.intr_type    = GPIO_INTR_DISABLE;
    io.mode         = GPIO_MODE_OUTPUT;
    io.pin_bit_mask = (1ULL << s_dir_pin) | (1ULL << s_en_pin);
    io.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io.pull_up_en   = GPIO_PULLUP_DISABLE;
    gpio_config(&io);

    ts_write_en(false);              // 先确保禁用
    ts_write_dir(direction);         // 再定方向
    ts_write_en(true);               // 最后使能

    // ② LEDC 起振（50% 占空比）
    double actual = ledcSetup(TS_LEDC_CH, (double)hz, res);
    if (actual <= 0) {
        ts_log("start failed: ledcSetup(hz=%lu, res=%u) rejected",
               (unsigned long)hz, (unsigned)res);
        ts_write_en(false);
        return false;
    }
    ledcAttachPin(s_step_pin, TS_LEDC_CH);
    ledcWrite(TS_LEDC_CH, (uint32_t)1u << (res - 1));   // 50%

    s_hz          = hz;
    s_direction   = direction;
    s_res_bits    = res;
    s_running     = true;
    s_start_ms    = millis();
    s_last_log_ms = s_start_ms;

    ts_log("START: %lu Hz (actual %.1f Hz, res=%u bit, duty=50%%), dir=%d, "
           "step=%d dir=%d en=%d; 无 30s 限制，最长 %lu ms",
           (unsigned long)hz, actual, (unsigned)res, direction,
           s_step_pin, s_dir_pin, s_en_pin,
           (unsigned long)TEST_STEPPER_MAX_RUN_MS);

    if (stepper_is_enabled()) {
        ts_log("WARN: stepper 模块处于启用状态，STEP 引脚已被本测试模块接管；"
               "测试结束后请重启设备以恢复 stepper 的 RMT 输出");
    }
    return true;
}

// =====================================================
// 对外：停止（幂等）
// =====================================================
bool test_stepper_stop()
{
    if (!s_running) {
        ts_log("stop: not running (idempotent)");
        return true;
    }
    ts_stop_internal("user_stop");
    return true;
}

// =====================================================
// 参数读取（可选参数：缺失时保留默认值）
// =====================================================
static bool ts_param_int(WorkflowActionInstance *action, const char *name, int *out)
{
    if (action == nullptr || name == nullptr || out == nullptr) {
        return false;
    }
    for (uint8_t i = 0; i < action->param_count && i < WORKFLOW_MAX_PARAM; i++) {
        if (action->params[i].type == PARAM_INT && action->params[i].name == name) {
            *out = action->params[i].int_value;
            return true;
        }
    }
    return false;
}

// =====================================================
// Action 回调
// =====================================================
static void ts_action_reset(WorkflowActionInstance *action)
{
    if (action == nullptr) {
        return;
    }
    action->runtime = nullptr;
    action->result  = ACTION_IDLE;
}

static void ts_start_action(WorkflowActionInstance *action)
{
    if (action == nullptr) {
        return;
    }

    int hz  = TEST_STEPPER_HZ_DEFAULT;
    int dir = 0;
    ts_param_int(action, "hz", &hz);            // 缺失时保留默认 100
    ts_param_int(action, "direction", &dir);    // 缺失时保留默认 0

    if (hz < TEST_STEPPER_HZ_MIN || hz > TEST_STEPPER_HZ_MAX) {
        ts_log("action rejected: hz=%d out of range [%d, %d]",
               hz, TEST_STEPPER_HZ_MIN, TEST_STEPPER_HZ_MAX);
        action->result = ACTION_FAILED;
        return;
    }
    if (dir != 0 && dir != 1) {
        ts_log("action rejected: direction=%d, expect 0 or 1", dir);
        action->result = ACTION_FAILED;
        return;
    }
    if (!test_stepper_start((uint32_t)hz, dir)) {
        action->result = ACTION_FAILED;
        return;
    }

    // 启动成功即刻返回成功：输出保持，Workflow 可继续下一步
    // （与 VIBRATION_START duration_ms=0 的语义一致）
    action->result = ACTION_SUCCESS;
}

static void ts_stop_action(WorkflowActionInstance *action)
{
    bool ok = test_stepper_stop();
    if (action != nullptr) {
        action->result = ok ? ACTION_SUCCESS : ACTION_FAILED;
    }
}

static void ts_poll(WorkflowActionInstance *action)
{
    // START / STOP 都是即时完成，正常不会进入 poll
    if (action != nullptr && action->result == ACTION_RUNNING) {
        action->result = ACTION_FAILED;
    }
}

// =====================================================
// Action 描述符
// =====================================================
static WorkflowParam ts_start_params[] = {
    {"hz",        PARAM_INT, "Hz",   "方波频率 10~2000，默认 100"},
    {"direction", PARAM_INT, "enum", "旋转方向：0 = 正向，1 = 反向，默认 0"}
};

static WorkflowParam ts_no_params[] = {
    {"_unused", PARAM_INT, "", ""}
};

static WorkflowActionDescriptor ts_start_desc = {
    .id          = "TEST_STEPPER_START",
    .name        = "步进测试-启动",
    .module      = "test_stepper",
    .description = "测试用：使能驱动器并持续输出指定频率方波（无 30 秒限制，直到 STOP）",
    .params      = ts_start_params,
    .param_count = 2,
    .reset       = ts_action_reset,
    .start       = ts_start_action,
    .poll        = ts_poll
};

static WorkflowActionDescriptor ts_stop_desc = {
    .id          = "TEST_STEPPER_STOP",
    .name        = "步进测试-停止",
    .module      = "test_stepper",
    .description = "测试用：停止方波输出并禁用驱动器（幂等）",
    .params      = ts_no_params,
    .param_count = 0,
    .reset       = ts_action_reset,
    .start       = ts_stop_action,
    .poll        = ts_poll
};

// =====================================================
// 初始化
// =====================================================
void test_stepper_init()
{
    // 复用 stepper 的引脚配置（不新增配置文件，避免 LittleFS 缺文件问题）
    s_step_pin    = config_get_stepper_step_pin();
    s_dir_pin     = config_get_stepper_dir_pin();
    s_en_pin      = config_get_stepper_en_pin();
    s_en_active   = config_get_stepper_en_active_level();
    s_dir_invert  = config_get_stepper_dir_invert();

    // 安全默认：先写禁用/低电平，再配置为输出（同 stepper 的 V-2 规避策略）
    if (s_step_pin >= 0 && s_dir_pin >= 0 && s_en_pin >= 0) {
        gpio_set_level((gpio_num_t)s_en_pin, (s_en_active != 0) ? 0 : 1);
        gpio_set_level((gpio_num_t)s_dir_pin, 0);

        gpio_config_t io;
        io.intr_type    = GPIO_INTR_DISABLE;
        io.mode         = GPIO_MODE_OUTPUT;
        io.pin_bit_mask = (1ULL << s_dir_pin) | (1ULL << s_en_pin);
        io.pull_down_en = GPIO_PULLDOWN_DISABLE;
        io.pull_up_en   = GPIO_PULLUP_DISABLE;
        gpio_config(&io);
    }

    s_initialized = true;

    // Action 始终注册（避免 stable_id 随设备配置漂移）
    if (!workflow_register_action(&ts_start_desc)) {
        ts_log("TEST_STEPPER_START register failed");
    }
    if (!workflow_register_action(&ts_stop_desc)) {
        ts_log("TEST_STEPPER_STOP register failed");
    }

    ts_log("Init: step=%d dir=%d en=%d (en_active=%d dir_invert=%d), "
           "hz=%d..%d default=%d, max_run=%lu ms",
           s_step_pin, s_dir_pin, s_en_pin, s_en_active, (int)s_dir_invert,
           TEST_STEPPER_HZ_MIN, TEST_STEPPER_HZ_MAX, TEST_STEPPER_HZ_DEFAULT,
           (unsigned long)TEST_STEPPER_MAX_RUN_MS);
}

// =====================================================
// 任务（loop 中调用，非阻塞）
// =====================================================
void test_stepper_task()
{
    if (!s_initialized || !s_running) {
        return;
    }

    unsigned long now = millis();
    unsigned long run = now - s_start_ms;

#if TEST_STEPPER_MAX_RUN_MS > 0
    if (run >= TEST_STEPPER_MAX_RUN_MS) {
        ts_log("HARD LIMIT %lu ms reached -> forced stop", (unsigned long)TEST_STEPPER_MAX_RUN_MS);
        ts_stop_internal("hard_limit");
        return;
    }
#endif

    // 每 10 秒心跳（便于确认"还在跑"，不必一直盯串口）
    if ((unsigned long)(now - s_last_log_ms) >= 10000UL) {
        s_last_log_ms = now;
        ts_log("running: %lu Hz dir=%d elapsed=%lu ms",
               (unsigned long)s_hz, s_direction, run);
    }
}

// =====================================================
// 查询
// =====================================================
bool     test_stepper_is_running()      { return s_running; }
uint32_t test_stepper_get_hz()          { return s_hz; }
int      test_stepper_get_direction()   { return s_direction; }
uint32_t test_stepper_get_running_ms()
{
    return s_running ? (uint32_t)(millis() - s_start_ms) : 0;
}
int test_stepper_get_step_pin()         { return s_step_pin; }
int test_stepper_get_dir_pin()          { return s_dir_pin; }
int test_stepper_get_en_pin()           { return s_en_pin; }
