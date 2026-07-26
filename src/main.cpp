// =====================================================
// 合并文件：main.cpp + workflow_test.cpp
// =====================================================

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
// 【测试代码块开始 - 可整体删除】
// 用途：仅用于测试 Workflow Timer/Delay 功能
// 正式环境可删除此区域
// =====================================================

// 测试 Action Handler
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

// 测试 JSON（Timer 配置为 1970-01-01 以立即触发失败）
const char *test_json = 
"{"
"\"workflows\":["
"{"
"\"id\":\"wf_timer\","
"\"name\":\"Timer Test (expired)\","
"\"enable\":true,"
"\"timeout_ms\":600000,"
"\"steps\":["
"{"
"\"type\":\"trigger\","
"\"id\":\"timer\","
"\"params\":{"
"\"type\":\"once\","
"\"hour\":0,"
"\"minute\":0,"
"\"year\":1970,"
"\"month\":1,"
"\"day\":1"
"}"
"},"
"{"
"\"type\":\"action\","
"\"id\":\"test.action\","
"\"params\":{"
"\"value\":100"
"}"
"}"
"]"
"},"
"{"
"\"id\":\"wf_delay\","
"\"name\":\"Delay Test (3s)\","
"\"enable\":true,"
"\"timeout_ms\":600000,"
"\"steps\":["
"{"
"\"type\":\"trigger\","
"\"id\":\"delay\","
"\"params\":{"
"\"seconds\":3"
"}"
"},"
"{"
"\"type\":\"action\","
"\"id\":\"test.action\","
"\"params\":{"
"\"value\":200"
"}"
"}"
"]"
"}"
"]"
"}";

// =====================================================
// 【测试代码块结束】
// =====================================================


void setup()
{
    Serial.begin(115200);
    delay(1000);

    // 初始化 LittleFS
    if (!LittleFS.begin(true, "/littlefs", 10, "littlefs")) {
        Serial.println("LittleFS mount failed!");
    }

    system_state_init();
    config_init();
    event_manager_init();
    wifi_init();
    time_init();
    cloud_init();
    command_manager_init();
    oled_init();
    oled_event_init();
    weight_init();

    // =====================================================
    // Workflow 初始化（永久保留）
    // =====================================================
    workflow_init();

    // =====================================================
    // 【测试代码块开始 - 可整体删除】
    // 注册测试 Action 并加载测试 JSON
    // =====================================================
    Serial.println("");
    Serial.println("========================================");
    Serial.println("   Workflow Test Mode (Temporary)");
    Serial.println("========================================");

    if (!workflow_register_action(&test_action_desc)) {
        Serial.println("[ERROR] Test action register failed!");
    } else {
        Serial.println("[OK] Test action registered");
    }

    if (!workflow_load_json(test_json)) {
        Serial.println("[ERROR] Test JSON load failed!");
    } else {
        Serial.println("[OK] Test JSON loaded");
    }

    // 打印测试 Workflow 信息
    for (uint8_t i = 0; i < workflow_get_count(); i++) {
        Workflow *wf = workflow_get(i);
        if (wf == nullptr) continue;
        Serial.printf("[TEST] Workflow %d: %s (%s), steps: %d\n",
            i, wf->id.c_str(), wf->name.c_str(), wf->step_count);
    }
    Serial.println("========================================");
    Serial.println();
    
    // =====================================================
    // 【测试代码块结束】
    // =====================================================
}


void loop()
{
    // 原有任务
    wifi_task();
    event_dispatch();
    time_task();
    cloud_task();
    oled_task();
    weight_task();

    // =====================================================
    // Workflow 任务（永久保留）
    // =====================================================
    workflow_timer_check();   // 检查 Timer 触发
    workflow_task();          // 执行 Workflow

    event_dispatch();  // 最后一次事件分发
    delay(2200);  
}