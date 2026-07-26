#pragma once


// 云通信初始化
void cloud_init();

// 云通信任务
void cloud_task();

// MQTT底层发送接口
bool cloud_send_raw(
    const char* message
);
