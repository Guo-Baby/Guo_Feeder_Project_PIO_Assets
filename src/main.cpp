#include <Arduino.h>
#include <LittleFS.h>
#include "system_state.h"
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

// =====================================================
// setup
// =====================================================
void setup()
{
    Serial.begin(115200);
    delay(1000);

    // ---- 原有初始化（保持不变） ----
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
    valve_init();

    //各模块加载放在上方，否则无法注册workflow
    // 加载真实的 workflow.json（放在 data 目录）
    workflow_init();
    if (workflow_load_json_file("/workflow.json")) {
        Serial.println("[OK] Workflow loaded from /workflow.json");
    } else {
        Serial.println("[WARN] No workflow.json found");
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
    oled_task();
    weight_task();
    valve_task();

    // ---- 新增：Workflow 任务 ----
    workflow_timer_check();   // 检查 Timer 触发
    workflow_task();          // 执行 Workflow 状态机
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
