#ifndef SYSTEM_STATE_H
#define SYSTEM_STATE_H

#include <Arduino.h>


// =====================================================
// System State 类型
//
// 所有系统共享状态统一登记位置
//
// 规则：
// 1. 谁产生状态，谁调用 state_set_xxx()
// 2. 所有模块通过 state_get_xxx()读取
// 3. 不允许业务逻辑写入其他模块状态
//
// =====================================================


enum SystemStateKey
{

    // WiFi
    STATE_WIFI_STATUS,//连接状态，bool值
    STATE_WIFI_RSSI,//信号分贝
    STATE_WIFI_SIGNAL,//信号质量，int值，规则为0，1极差，2中等，3良好，4极好
    STATE_WIFI_STATE,//WiFi模块工作状态机,int值，规则为0 idle，1 connecting，2 connected，3 disconnected

    // 时间
    //
    STATE_TIME_VALID,


    // 阀门
    STATE_VALVE_STATUS,


    // 重量(g)
    STATE_WEIGHT_VALUE,


    // 称重异常
    STATE_WEIGHT_ERROR,


    // Event队列数量
    STATE_EVENT_QUEUE_COUNT,


    // 状态数量
    STATE_MAX
};




// =====================================================
// 状态值类型
// =====================================================


enum StateValueType
{

    STATE_TYPE_INT,

    STATE_TYPE_BOOL,

    STATE_TYPE_LONG,

    STATE_TYPE_FLOAT

};





// =====================================================
// 状态存储结构
// =====================================================


struct SystemStateValue
{

    StateValueType type;


    long value;


    float value_float;


    bool valid;

};





// =====================================================
// 初始化
// =====================================================

void system_state_init();





// =====================================================
// 写接口
//
// 对应模块内部调用
// =====================================================


bool state_set_int(
    SystemStateKey key,
    int value
);



bool state_set_bool(
    SystemStateKey key,
    bool value
);



bool state_set_long(
    SystemStateKey key,
    long value
);



bool state_set_float(
    SystemStateKey key,
    float value
);


// =====================================================
// 读接口
//
// 所有模块公开读取
// =====================================================


int state_get_int(
    SystemStateKey key
);



bool state_get_bool(
    SystemStateKey key
);



long state_get_long(
    SystemStateKey key
);



float state_get_float(
    SystemStateKey key
);





// =====================================================
// 查询有效性
// =====================================================

bool state_is_valid(
    SystemStateKey key
);



#endif