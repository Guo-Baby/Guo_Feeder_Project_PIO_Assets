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
    //MQTT连接状态
    STATE_MQTT_STATUS,
    //MQTT重连次数
    STATE_MQTT_RETRY_COUNT,
    //MQTT最后连接时间
    STATE_MQTT_LAST_CONNECT_TIME,
    //MQTT最后错误信息
    STATE_MQTT_LAST_ERROR,


    //新增state需要写在上方，同时维护cpp内的state_map映射表，state_get_field_count()和state_get_field_name()函数会自动更新，无需维护
    // 状态数量
    STATE_MAX,
};
// =====================================================
// 状态值类型
// =====================================================
enum StateValueType
{
    STATE_TYPE_INT,
    STATE_TYPE_BOOL,
    STATE_TYPE_LONG,
    STATE_TYPE_FLOAT,
    STATE_TYPE_STRING
};

// =====================================================
// 状态存储结构
// ====================================================
struct SystemStateValue
{
    StateValueType type;
    long value;
    float value_float;
    String value_string;
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
bool state_set_int(SystemStateKey key,int value);
bool state_set_bool(SystemStateKey key,bool value);
bool state_set_long(SystemStateKey key,long value);
bool state_set_float(SystemStateKey key,float value);
bool state_set_string(SystemStateKey key,String value);

// =====================================================
// 读接口，所有模块公开读取
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

String state_get_string(
    SystemStateKey key
);
// =====================================================
// 查询有效性
// =====================================================
bool state_is_valid(SystemStateKey key);
// =====================================================
// 新增：枚举 → 字符串 映射
// =====================================================
const char* state_key_to_string(SystemStateKey key);
SystemStateKey state_string_to_key(const String &str);

// =====================================================
// 新增：通用查询接口（供 Command Manager 调用）
// =====================================================
bool system_state_query(const String &key,String &output);

// =====================================================
// 新增：获取所有字段列表（供 UI 使用）
// =====================================================
uint8_t state_get_field_count();
const char* state_get_field_name(uint8_t index);


#endif
