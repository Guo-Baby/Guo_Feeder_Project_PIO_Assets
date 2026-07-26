#pragma once


// WiFi初始化
void wifi_init();


// WiFi任务
void wifi_task();


// 查询WiFi状态
bool wifi_is_connected();

// 获取当前WiFi RSSI(dBm)
int wifi_get_rssi();


// 获取当前WiFi信号质量
int wifi_get_signal_quality();