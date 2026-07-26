#include <Arduino.h>
#include <LittleFS.h>
#include <esp_partition.h>

#include "system_state.h"
#include "config_manager.h"
#include "event_manager.h"
#include "oled.h"
#include "command_manager.h"
#include "wifi_module.h"
#include "time_manager.h"
#include "cloud_manager.h"
#include "weight.h"
#include "workflow.h"



// =====================================================
// 测试 Action Handler
// =====================================================
static WorkflowActionResult test_action_handler(WorkflowActionInstance *action)
{
    Serial.println("[ACTION] Test action executed!");
    return ACTION_SUCCESS;
}

static WorkflowParam test_action_params[] = {
    {"value", PARAM_INT, "", "test value"}
};

static WorkflowActionDescriptor test_action_desc = {
    "test.action",
    "Test Action",
    "test",
    test_action_params,
    1,
    test_action_handler
};

// =====================================================
// 测试 JSON（包含 Timer 和 Delay）
// =====================================================
const char *test_json = 
"{"
"    \"workflows\": ["
"        {"
"            \"id\": \"wf_timer\","
"            \"name\": \"Timer Test (expired)\","
"            \"enable\": true,"
"            \"timeout_ms\": 600000,"
"            \"steps\": ["
"                {"
"                    \"type\": \"trigger\","
"                    \"id\": \"timer\","
"                    \"params\": {"
"                        \"type\": \"once\","
"                        \"hour\": 0,"
"                        \"minute\": 0,"
"                        \"year\": 2000,"
"                        \"month\": 1,"
"                        \"day\": 1"
"                    }"
"                },"
"                {"
"                    \"type\": \"action\","
"                    \"id\": \"test.action\","
"                    \"params\": {"
"                        \"value\": 100"
"                    }"
"                }"
"            ]"
"        },"
"        {"
"            \"id\": \"wf_delay\","
"            \"name\": \"Delay Test (3s)\","
"            \"enable\": true,"
"            \"timeout_ms\": 600000,"
"            \"steps\": ["
"                {"
"                    \"type\": \"trigger\","
"                    \"id\": \"delay\","
"                    \"params\": {"
"                        \"seconds\": 3"
"                    }"
"                },"
"                {"
"                    \"type\": \"action\","
"                    \"id\": \"test.action\","
"                    \"params\": {"
"                        \"value\": 200"
"                    }"
"                }"
"            ]"
"        }"
"    ]"
"}";

// =====================================================
// setup
// =====================================================
void setup()
{
    Serial.begin(115200);
    delay(1000);
    // 初始化littleFS
    if (!LittleFS.begin(true, "/littlefs", 10, "littlefs")) 
        {Serial.println("LittleFS mount failed!");}
    system_state_init();// 初始化系统状态，顺序必须先于其他模块
    config_init();// 初始化配置参数文档，顺序必须先于其他模块
    event_manager_init();
    wifi_init();// 初始化WiFi模块
    time_init();// 初始化时间模块
    cloud_init();// 初始化云通信模块
    command_manager_init();//初始化命令模块
    oled_init();// 初始化OLED
    oled_event_init();//事件注册
    weight_init();

    Serial.println();
    Serial.println("========================================");
    Serial.println("    Workflow Timer + Delay Test");
    Serial.println("========================================");
    Serial.println();

    // 1. 初始化 Workflow（只初始化框架，不加载 JSON）
    if (!workflow_init()) {
        Serial.println("[ERROR] Workflow init failed!");
        return;
    }
    Serial.println("[OK] Workflow init");

    // 2. 注册测试 Action
    if (!workflow_register_action(&test_action_desc)) {
        Serial.println("[ERROR] Action register failed!");
        return;
    }
    Serial.println("[OK] Action registered: test.action");

    // 3. 加载测试 JSON（Timer 初始化时会检查 TIME_VALID）
    if (!workflow_load_json(test_json)) {
        Serial.println("[ERROR] JSON load failed!");
        return;
    }
    Serial.println("[OK] JSON loaded");

    // 4. 打印所有 Workflow
    Serial.printf("[INFO] Total workflows: %d\n", workflow_get_count());
    for (uint8_t i = 0; i < workflow_get_count(); i++) {
        Workflow *wf = workflow_get(i);
        if (wf == nullptr) continue;
        Serial.printf("  [%d] %s (%s), steps: %d\n",
            i, wf->id.c_str(), wf->name.c_str(), wf->step_count);
        for (uint8_t j = 0; j < wf->step_count; j++) {
            const char *type_str = (wf->steps[j].type == WORKFLOW_STEP_TRIGGER) ? "TRIGGER" : "ACTION";
            Serial.printf("      Step %d: %s (%s)\n", j, wf->steps[j].id.c_str(), type_str);
        }
    }

    // =============================================
    // Timer Workflow 由 workflow_timer_check() 自动检测触发
    // 不手动启动
    // =============================================
    Serial.println();
    Serial.println("[INFO] Timer Workflow (wf_timer) will be auto-detected by workflow_timer_check()");
    Serial.println("[INFO] Expected: timer expired → workflow_start(skip_first_step=true) → ERROR");

    // =============================================
    // Delay Workflow 手动启动（测试 Delay 功能）
    // =============================================
    Serial.println();
    Serial.println("[START] Delay Workflow (wf_delay) will start in 5 seconds...");
}

// =====================================================
// loop
// =====================================================
static bool delay_workflow_started = false;
static unsigned long status_print_time = 0;
static unsigned long last_timer_log = 0;

void loop()
{
    wifi_task();
    event_dispatch();
    time_task();
    cloud_task();// 巴法云通信模块
    oled_task();
    weight_task();
    event_dispatch();    
    unsigned long now = millis();

    // =============================================
    // 1. Timer 自动触发检查（每 5 秒）
    //    由 workflow_timer_check() 扫描 IDLE 的 Workflow
    // =============================================
    workflow_timer_check();

    // =============================================
    // 2. 延时启动 Delay Workflow（setup 后 5 秒）
    //    手动启动，验证 Delay 功能
    // =============================================
    if (!delay_workflow_started && now > 5000) {
        delay_workflow_started = true;
        Serial.println();
        Serial.println("[START] Starting Delay Workflow (wf_delay)...");
        Workflow *wf_delay = workflow_get(1);
        if (wf_delay) {
            if (workflow_start(wf_delay, false)) {
                Serial.println("[OK] Delay Workflow started (should execute after 3 seconds)");
            } else {
                Serial.println("[WARN] Delay Workflow start failed");
            }
        }
    }

    // =============================================
    // 3. 执行 Workflow 任务
    // =============================================
    workflow_task();

    // =============================================
    // 4. 每 3 秒打印状态
    // =============================================
    if (now - status_print_time > 5000) {
        status_print_time = now;
        Serial.println("--- Status ---");
        for (uint8_t i = 0; i < workflow_get_count(); i++) {
            Workflow *wf = workflow_get(i);
            if (wf == nullptr) continue;
            const char *state_str[] = {"IDLE", "RUNNING", "WAITING", "FINISHED", "TIMEOUT", "ERROR"};
            const char *state_name = (wf->state <= WORKFLOW_ERROR) ? state_str[wf->state] : "UNKNOWN";
            Serial.printf("  %s: state=%s, step=%d/%d\n",
                wf->id.c_str(), state_name, wf->current_step, wf->step_count);
            Serial.printf("[DEBUG] TIME_VALID = %d\n", state_get_bool(STATE_TIME_VALID));
        }
        Serial.println();
    
    }
    


    delay(100);
}