#include <Arduino.h>
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
    Serial.println();
    Serial.println("========================================");
    Serial.println("    Workflow Timer + Delay Test");
    Serial.println("========================================");
    Serial.println();

    // 1. 初始化 Workflow
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

    // 3. 加载测试 JSON
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
    // 关键改动：Timer Workflow 不手动启动
    // 让 workflow_timer_check() 自动检测触发
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
static bool timer_checked = false;

void loop()
{
    unsigned long now = millis();

    // Timer 检查
    workflow_timer_check();

    // ===== 调试：打印 wf_timer 状态和 timer trigger 信息 =====
    static unsigned long last_debug = 0;
    if (now - last_debug > 10000) {  // 每 10 秒打印一次
        last_debug = now;
        Workflow *wf = workflow_get(0);
        if (wf) {
            Serial.printf("[DEBUG] wf_timer: state=%d, step=%d/%d\n", 
                wf->state, wf->current_step, wf->step_count);
            if (wf->step_count > 0) {
                WorkflowStep &step = wf->steps[0];
                Serial.printf("[DEBUG]   Step0: instance_type=%d, instance=%p\n", 
                    step.instance_type, step.instance);
                if (step.instance != nullptr) {
                    WorkflowTriggerInstance *trigger = (WorkflowTriggerInstance*)step.instance;
                    if (trigger->descriptor != nullptr) {
                        Serial.printf("[DEBUG]   trigger id=%s\n", trigger->descriptor->id);
                    } else {
                        Serial.printf("[DEBUG]   trigger descriptor is NULL\n");
                    }
                }
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
    if (now - status_print_time > 3000) {
        status_print_time = now;
        Serial.println("--- Status ---");
        for (uint8_t i = 0; i < workflow_get_count(); i++) {
            Workflow *wf = workflow_get(i);
            if (wf == nullptr) continue;
            const char *state_str[] = {"IDLE", "RUNNING", "WAITING", "FINISHED", "TIMEOUT", "ERROR"};
            const char *state_name = (wf->state <= WORKFLOW_ERROR) ? state_str[wf->state] : "UNKNOWN";
            Serial.printf("  %s: state=%s, step=%d/%d\n",
                wf->id.c_str(), state_name, wf->current_step, wf->step_count);
        }
        Serial.println();
    }

    delay(1000);
}