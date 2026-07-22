#pragma once

#include <Arduino.h>
#include <time.h>


// =====================================================
// 系统运行状态模块
//
// 作用：
// 保存设备运行期间的公共状态变量
//
// 注意：
// 1. 不保存用户配置
// 2. 不操作Flash
// 3. 不负责通信
//
// 其他模块通过这里交换运行状态
//
// 例如：
// OLED读取重量、时间、WiFi状态
// 云端读取设备状态
// RTC读取时间状态
// =====================================================

// 初始化系统状态
void system_state_init();

// ==========================
// WiFi状态
// ==========================
void system_set_wifi_status(bool status);
bool system_get_wifi_status();


// ==========================
// 系统时间状态
// ==========================
// 设置当前系统时间
void system_set_time(time_t timestamp);

// 获取当前系统时间
time_t system_get_time();

// 时间是否有效
bool system_is_time_valid();


// ==========================
// NTP同步状态
// ==========================
// 设置NTP同步完成
void system_set_ntp_synced(bool status);

// 获取NTP同步状态
bool system_is_ntp_synced();

// 记录最后一次NTP同步时间
void system_set_last_ntp_sync(time_t timestamp);

// 获取最后一次NTP同步时间
time_t system_get_last_ntp_sync();

// ==========================
// RTC状态
// ==========================
// RTC是否存在
void system_set_rtc_available(bool status);

// RTC状态查询
bool system_is_rtc_available();


// ==========================
// 系统错误状态
// ==========================
// 当前错误代码
void system_set_error_code(
    uint16_t code
);

// 获取错误代码
uint16_t system_get_error_code();


// ==========================
// 设备运行状态
// ==========================
enum SystemRunState
{
    SYSTEM_IDLE = 0,
    SYSTEM_RUNNING,
    SYSTEM_ERROR
};


// 设置系统状态
void system_set_run_state(
    SystemRunState state
);

// 获取系统状态
SystemRunState system_get_run_state();