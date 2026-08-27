#pragma once
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>


/**
 * @brief 初始化米家LYWSD03MMC BLE被动扫描
 * 内部读取全局config中的 lywsd03_mac / lywsd03_bindkey
 * 如果mac或者bindkey为空，直接不启动扫描
 * @return true:已启动扫描 false:配置缺失，未启用
 */
bool MiThermometerInit(void);

// 对外队列句柄，由本模块创建；业务task使用xQueueReceive读取
extern QueueHandle_t xMiThermometerQueue;

//主loop的task
void MiThermometer_task(void);

void MiThermometer_decode_test();

void ble_init();

//workflow调用action入口
bool mi_thermo_start_scan();
bool mi_thermo_stop_scan();