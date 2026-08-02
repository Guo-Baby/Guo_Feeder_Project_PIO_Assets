#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>

// 云通信初始化
void cloud_init();

// 云通信后台任务，loop循环调用
void cloud_task();

// MQTT底层原始发送接口
bool cloud_send_set(const char* message);

bool cloud_send_up(const char *message);

// 【扩展】直接传入JsonDocument序列化发布（新增接口）
bool cloud_upload_json(JsonDocument& doc);


// 查询MQTT当前在线状态
bool cloud_is_connected();
