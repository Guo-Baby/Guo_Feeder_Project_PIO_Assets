#ifndef EVENT_MANAGER_H
#define EVENT_MANAGER_H


#include <Arduino.h>



// ==========================
// 系统事件
// ==========================

enum SystemEvent
{
    EVENT_NONE = 0,
    // WiFi
    EVENT_WIFI_CONNECTED,
    EVENT_WIFI_DISCONNECTED,
    // 时间NTP
    EVENT_NTP_SYNC_OK,
    EVENT_TIME_VALID,
    EVENT_TIME_INVALID,
    // 配置
    EVENT_CONFIG_CHANGED,
    // 云
    EVENT_CLOUD_CONNECTED,
    EVENT_CLOUD_DISCONNECTED,
    EVENT_CLOUD_UPLOAD,
    EVENT_CLOUD_COMMAND,
    // 命令执行结果
    EVENT_COMMAND_RESULT,
    // 重量称重
    EVENT_WEIGHT_READY,
    EVENT_WEIGHT_ERROR,
    // 系统
    EVENT_ERROR


};




// ==========================
// 优先级
// ==========================

#define EVENT_PRIORITY_LOW        0

#define EVENT_PRIORITY_NORMAL     1

#define EVENT_PRIORITY_HIGH       2

#define EVENT_PRIORITY_CRITICAL   3




// ==========================
// 策略
// ==========================

enum EventPolicy
{

    EVENT_POLICY_NORMAL,


    // 同事件只保留一个
    EVENT_POLICY_DEDUP,


    // 新状态覆盖旧状态
    EVENT_POLICY_STATE,


    // 强制执行
    EVENT_POLICY_FORCE

};





// ==========================
// 消息结构
// ==========================


struct EventMessage
{

    SystemEvent event;


    String data;


    String source;


    int priority;


    EventPolicy policy;


    unsigned long timestamp;


    // 生命周期
    // 0 = 永不过期
    unsigned long expire;


};





// ==========================
// 返回
// ==========================

enum EventPushResult
{

    EVENT_PUSH_OK,

    EVENT_QUEUE_FULL,

    EVENT_DROPPED,

    EVENT_DUPLICATE

};





typedef void (*EventCallback)
(
    const EventMessage &message
);





void event_manager_init();





EventPushResult event_push(

    SystemEvent event,

    String data = "",

    String source = "",

    int priority = EVENT_PRIORITY_NORMAL,

    EventPolicy policy = EVENT_POLICY_NORMAL,

    unsigned long expire = 0

);





bool event_subscribe(

    SystemEvent event,

    EventCallback callback

);





void event_dispatch();





unsigned long event_get_drop_count();


unsigned long event_get_duplicate_count();


int event_get_queue_count();



#endif