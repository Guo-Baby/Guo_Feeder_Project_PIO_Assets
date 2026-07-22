#pragma once

#include <Arduino.h>


// 初始化配置系统
bool config_init();


// 保存配置
bool config_save();


// WiFi配置
String config_get_wifi_ssid();

String config_get_wifi_password();

int config_get_wifi_connect_timeout();

int config_get_wifi_reconnect_interval();

// OLED配置

int config_get_oled_sda();

int config_get_oled_scl();

int config_get_oled_speed();

int config_get_oled_frame_delay();

int config_get_oled_rotation();//屏幕显示旋转角度，0~3,对应旋转角度0，90，180，270度


// ==========================
// 时间配置
// ==========================

int config_get_timezone();

String config_get_ntp_server1();

String config_get_ntp_server2();

int config_get_ntp_sync_interval_day();