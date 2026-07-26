#pragma once


// ==========================
// Cloud消息发布接口
// ==========================


// 发布普通字符串消息
bool cloud_publish_message(
    const char* message
);


// 发布系统状态JSON
bool cloud_publish_status();
