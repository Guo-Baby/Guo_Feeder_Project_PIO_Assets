#pragma once
#include "services/event_manager.h"

// OLED初始化
void oled_init();


// OLED循环任务
// 放入main.cpp的loop中不断调用
void oled_task();


// 清屏
void oled_clear();

// Event Manager调用
void oled_event_handler(
    SystemEvent event
);
void oled_event_init();