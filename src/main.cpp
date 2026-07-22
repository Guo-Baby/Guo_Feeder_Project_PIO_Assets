#include <Arduino.h>
#include <LittleFS.h>
#include "system_state.h"
#include "config_manager.h"
#include "oled.h"
#include "wifi_module.h"
#include "wifi_module.h"
#include "time_manager.h"
#include "event_manager.h"

void setup()
{

    Serial.begin(115200);
      delay(1000);
    
      system_state_init();// 初始化系统状态，顺序必须先于其他模块
      event_manager_init();
      config_init();// 初始化配置参数文档，顺序必须先于其他模块
   
    wifi_init();// 初始化WiFi模块
    time_init();// 初始化时间模块
    oled_init();// 初始化OLED

    
}

void loop()
{

   
    wifi_task();
    time_task();
    oled_task();

}