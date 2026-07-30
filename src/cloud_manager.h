#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>

// 云通信初始化
void cloud_init();

// 云通信后台任务，loop循环调用
void cloud_task();

// MQTT底层原始发送接口
bool cloud_send_raw(const char* message);

// 发布普通字符串消息
bool cloud_publish_message(const char* message);

// 发布状态JSON（简易版本）
bool cloud_publish_status();

// 【扩展】直接传入JsonDocument序列化发布（新增接口）
bool cloud_publish_json(JsonDocument& doc);

// 获取最近收到的云端指令
String cloud_receive_get_command();

// 查询MQTT当前在线状态
bool cloud_is_connected();
