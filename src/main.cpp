#include <Arduino.h>
#include <LittleFS.h>
#include <NimBLEDevice.h>
#include "system_state.h"
#include "system_command.h"
#include "json_storage.h"
#include "bin_storage.h"
#include "workflow_storage.h"
#include "config_manager.h"
#include "event_manager.h"
#include "oled.h"
#include "command_manager.h"
#include "wifi_module.h"
#include "time_manager.h"
#include "cloud_manager.h"
#include "weight.h"
#include "valve.h"
#include "computer_reset.h"
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
    // ===== 初始化 BIN Storage（底层二进制文件存储，Workflow BIN 持久化依赖它）=====
    //
    // 内部会级联初始化 FileStorage。
    // 与 JsonStorage 平级，两条链路互不干扰：
    //   ConfigManager → JsonStorage → LittleFS
    //   WorkflowManager(后续) → BinStorage → FileStorage → LittleFS
    if (!bin_storage_init()) {
        Serial.println("[System] BinStorage init failed!");
    }
    // ===== 初始化 Workflow Storage（Workflow 定义持久化，依赖 BinStorage）=====
    //
    // 内部会创建 /workflow 目录并加载 meta.bin。
    // 本阶段只初始化存储层，Workflow.cpp 仍走原有 JSON 加载路径，行为不变。
    if (!workflow_storage_init()) {
        Serial.println("[System] WorkflowStorage init failed!");
    }
    system_state_init();
    config_init();
    event_manager_init();
    // =====================================================
    // 第二层：通信（依赖基础系统）
    // =====================================================
    ble_init();
    wifi_init();
    // =====================================================
    // 第三层：Workflow 框架（依赖基础系统）
    // =====================================================
    workflow_init();
    // =====================================================
    // 第四层：业务模块注册（依赖 workflow_init）
    // =====================================================
    weight_init();
    valve_init();
    computer_reset_init();     // GPIO8 脉冲（电脑重启），注册 COMPUTER_RESET Action
    dispense_guard_init();
    oled_init();
    oled_event_init();
    // ---- TimeManager V2：必须在 oled_init() 之后 ----
    // RTC(PCF8563T) 复用 OLED 建立的 Wire I2C Bus，绝不在本模块重复
    // Wire.begin()；故 time_init() 从第二层移到此处。
    time_init();
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
    // ---- Safe Restart 状态机（SystemCommand）----
    // 必须每轮调用：它是全系统唯一的 Restart 执行点。
    // 放在最前面，保证已进入的 10 秒安全窗口不被其他任务拖长。
    system_command_task();

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
    computer_reset_task();     // 手动脉冲回收 + GPIO8 安全兜底
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
            else if (serial_cmd_buffer == "computer_reset")
            {
                // 手动触发一次 800ms 脉冲（绕过 MQTT，用于本地验证 GPIO8）
                bool ok = computer_reset_trigger();
                Serial.printf("computer_reset trigger ret = %d\n", ok);
            }
            else if (serial_cmd_buffer == "computer_reset_status")
            {
                Serial.printf("ComputerReset active state: %d (pin=%d, hold=%lu ms)\n",
                    computer_reset_is_active(),
                    COMPUTER_RESET_PIN,
                    COMPUTER_RESET_HOLD_MS);
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