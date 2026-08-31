#include <Arduino.h>
#include <LittleFS.h>
#include <NimBLEDevice.h>
#include "system_state.h"
#include "json_storage.h"
#include "config_manager.h"
#include "event_manager.h"
#include "oled.h"
#include "command_manager.h"
#include "wifi_module.h"
#include "time_manager.h"
#include "cloud_manager.h"
#include "weight.h"
#include "valve.h"
#include "workflow.h"
#include "dispense_guard.h"
#include "test_mqtt.h"
#include "capability_registry.h"
#include "MiThermometer.h"

// =====================================================
// setup
// =====================================================
void setup()
{
    Serial.begin(115200);
    delay(1000);
    // =====================================================
    // 第一层：基础系统（无依赖）
    // =====================================================
    // ===== 一次性挂载 LittleFS =====
    if (!LittleFS.begin(true, "/littlefs", 10, "littlefs")) {
        Serial.println("[System] LittleFS mount failed!");
        // 根据你的错误处理策略，可以选择重启或继续
    } else {
        Serial.println("[System] LittleFS mounted");
    }
    Serial.printf("PSRAM size: %u\n", ESP.getPsramSize());
    Serial.printf("PSRAM free: %u\n", ESP.getFreePsram());
    // ===== 初始化 JSON Storage（底层文件存储，ConfigManager 依赖它）=====
    if (!json_storage_init()) {
        Serial.println("[System] JsonStorage init failed!");
    }
    system_state_init();
    config_init();
    event_manager_init();
    // =====================================================
    // 第二层：通信（依赖基础系统）
    // =====================================================
    ble_init();
    wifi_init();
    time_init();
    // =====================================================
    // 第三层：Workflow 框架（依赖基础系统）
    // =====================================================
    workflow_init();
    // =====================================================
    // 第四层：业务模块注册（依赖 workflow_init）
    // =====================================================
    weight_init();
    valve_init();
    dispense_guard_init();
    oled_init();
    oled_event_init();
    test_mqtt_init();
    bool ok = MiThermometerInit();
    // =====================================================
    // 第五层：命令和云端（依赖业务模块注册完成）
    // =====================================================
    command_manager_init();
    cloud_init();
    // =====================================================
    // 第六层：加载 Workflow 配置（依赖所有注册完成）
    // =====================================================
    if (workflow_load_json_file("/workflow.json")) {
        Serial.println("[OK] Workflow loaded from /workflow.json");
    } else {
        Serial.println("[WARN] No workflow.json found");
    }
    // =====================================================
    // 第七层：capability registry（依赖所有注册完成）
    // =====================================================
    capability_registry_init();
    // =====================================================
    // 第八层：启动确认（必须位于 setup 最后）
    //
    // 只有完整走完 setup 才会写入启动标记。
    // 若因配置错误导致 panic / 反复重启，标记不会被写入，
    // 下次启动 ConfigManager 会自动从 Backup 恢复配置。
    // =====================================================
    if (!config_boot_validate()) {
        Serial.println("[WARN] Config boot validate failed");
    }
}

void serial_debug_command_process();
// =====================================================
// loop
// =====================================================
void loop()
{
    // ---- 原有任务（保持不变） ----
    wifi_task();
    event_dispatch();
    time_task();
    cloud_task();

    command_manager_task();
    config_task();            // 配置修改后的自动重启倒计时
    // ---- 新增：Workflow 任务 ----
    workflow_task();          // 执行 Workflow 状态机
    oled_task();
    weight_task();
    valve_task();
    MiThermometer_task();
    test_mqtt_task();//测试代码，需要删除
    serial_debug_command_process();
}

//测试代码
//==== 放在 main.cpp 全局区域（不要写在loop内部）====
String serial_cmd_buffer;
//==== 命令解析函数，在loop()中调用 ====
void serial_debug_command_process(void)
{
    while (Serial.available() > 0)
    {
        char ch = Serial.read();
        // 回车 / 换行 触发解析
        if (ch == '\n' || ch == '\r')
        {
            if (serial_cmd_buffer.length() == 0)
            {
                continue;
            }
            serial_cmd_buffer.trim(); // 清除首尾空格
            Serial.print("Recv cmd: [");
            Serial.print(serial_cmd_buffer);
            Serial.println("]");
            if (serial_cmd_buffer == "valve_open")
            {
                bool ok = valve_open();
                Serial.printf("valve_open execute ret = %d\n", ok);
            }
            else if (serial_cmd_buffer == "valve_close")
            {
                bool ok = valve_close();
                Serial.printf("valve_close execute ret = %d\n", ok);
            }
            else if (serial_cmd_buffer == "valve_toggle")
            {
                bool ok = valve_toggle();
                Serial.printf("valve_toggle execute ret = %d\n", ok);
            }
            else if (serial_cmd_buffer == "valve_status")
            {
                Serial.printf("Valve current open state: %d\n", valve_is_open());
            }
            else
            {
                Serial.println("unknown command");
            }
            serial_cmd_buffer = ""; //清空缓冲区
        }
        else
        {
            serial_cmd_buffer += ch;
        }
    }
}