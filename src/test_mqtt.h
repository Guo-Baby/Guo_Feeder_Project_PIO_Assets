#pragma once


// =====================================================
// MQTT 测试 Action 模块
//
// 用于测试：
// 1. Temp Action调用链
// 2. payload参数传递
// 3. Action自主解析参数
// 4. cloud_send_up最大消息长度
//
// =====================================================


void test_mqtt_init();

void test_mqtt_task();