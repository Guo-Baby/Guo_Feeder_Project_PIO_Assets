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

int config_get_oled_sda();//sda对应的gpio引脚

int config_get_oled_scl();//scl对应的gpio引脚

int config_get_oled_speed();//i2c总线的通信频率，单位Hz 

int config_get_oled_frame_delay();//帧刷新延迟，配合动画展示功能刷新使用

int config_get_oled_rotation();//屏幕显示旋转角度，0~3,对应旋转角度0，90，180，270度

// =============================
// Valve 阀门配置读取
// =============================
int config_get_valve_gpio_pin();
int config_get_valve_active_level();//控制 GPIO 输出电平的逻辑极性,1=高电平有效，GPIO 输出 HIGH（3.3V）时阀门打开，LOW（0V）时关闭。0=低电平有效，GPIO 输出 LOW（0V）时阀门打开，HIGH（3.3V）时关闭。
int config_get_valve_open_duration_ms();//阀门打开持续时间，0=持续打开
bool config_get_valve_enable();//是否启用阀门模块，true=启用。false=阀门模块被禁用
int config_get_valve_safety_timeout_sec();//启用阀门最大开启时间，默认300s,合法值10~300，如需修改上下限去valve.cpp修改
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
bool config_set_weight_zero_offset(long offset);// 设置HX711零点
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