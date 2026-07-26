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
int config_get_ntp_sync_hour();
int config_get_ntp_sync_minute();

// ==========================
// HX711称重模块配置
// ==========================
int config_get_weight_dt();// HX711DT数据引脚
int config_get_weight_sck();// HX711SCK时钟引脚
int config_get_weight_sample_interval();// HX711采样间隔，单位ms


float config_get_weight_scale();// HX711称重模块的比例因子
long config_get_weight_zero_offset();// HX711称重模块的零点偏移量
void config_set_weight_zero_offset(long offset);// 设置HX711零点
int config_get_weight_filter_samples();// HX711称重模块的滤波采样数


// ==========================
// Bemfa_Cloud MQTT配置
// ==========================
String config_get_mqtt_server();
int config_get_mqtt_port();
String config_get_mqtt_client_id();
String config_get_mqtt_subscribe_topic();
// MQTT重连间隔(ms)
unsigned long config_get_mqtt_retry_interval();
// 最大快速重试次数
int config_get_mqtt_retry_max();
// 进入休眠重试间隔(ms)
unsigned long config_get_mqtt_sleep_interval();
// MQTT keep alive 秒
int config_get_mqtt_keep_alive();