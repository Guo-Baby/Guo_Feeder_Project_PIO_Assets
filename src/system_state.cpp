#include <Arduino.h>

#include "system_state.h"


// =====================================================
// 枚举 → 字符串 映射表
// =====================================================
static const struct {
    const char *name;
    SystemStateKey key;
    StateValueType type;
    bool readable;
} state_map[] = {
    // WiFi
    {"wifi_status", STATE_WIFI_STATUS, STATE_TYPE_BOOL, true},
    {"wifi_rssi", STATE_WIFI_RSSI, STATE_TYPE_INT, true},
    {"wifi_signal", STATE_WIFI_SIGNAL, STATE_TYPE_INT, true},
    {"wifi_state", STATE_WIFI_STATE, STATE_TYPE_INT, true},
    
    // 时间
    {"time_valid", STATE_TIME_VALID, STATE_TYPE_BOOL, false},
    
    // 阀门
    {"valve_status", STATE_VALVE_STATUS, STATE_TYPE_BOOL, true},
    
    // 重量
    {"weight_value", STATE_WEIGHT_VALUE, STATE_TYPE_FLOAT, true},
    {"weight_error", STATE_WEIGHT_ERROR, STATE_TYPE_BOOL, false},
    
    // 事件队列
    {"event_queue_count", STATE_EVENT_QUEUE_COUNT, STATE_TYPE_INT, false},
};

static const uint8_t STATE_MAP_COUNT = sizeof(state_map) / sizeof(state_map[0]);


// =====================================================
// System State存储区
// =====================================================

static SystemStateValue state_table[STATE_MAX];

// =====================================================
// ESP32 双核临界区锁
// =====================================================

static portMUX_TYPE state_mux = portMUX_INITIALIZER_UNLOCKED;

// =====================================================
// 初始化
// =====================================================

void system_state_init()
{

    portENTER_CRITICAL(&state_mux);


    for(
        int i = 0;
        i < STATE_MAX;
        i++
    )
    {
        state_table[i].type =
            STATE_TYPE_LONG;


        state_table[i].value =
            0;


        state_table[i].valid =
            false;
    }


    portEXIT_CRITICAL(&state_mux);

}



// =====================================================
// 写 INT
// =====================================================

bool state_set_int(
    SystemStateKey key,
    int value
)
{

    if(
        key < 0 ||
        key >= STATE_MAX
    )
    {
        return false;
    }


    portENTER_CRITICAL(&state_mux);


    state_table[key].type =
        STATE_TYPE_INT;


    state_table[key].value =
        value;


    state_table[key].valid =
        true;


    portEXIT_CRITICAL(&state_mux);


    return true;

}



// =====================================================
// 写 BOOL
// =====================================================

bool state_set_bool(
    SystemStateKey key,
    bool value
)
{

    if(
        key < 0 ||
        key >= STATE_MAX
    )
    {
        return false;
    }


    portENTER_CRITICAL(&state_mux);


    state_table[key].type =
        STATE_TYPE_BOOL;


    state_table[key].value =
        value ? 1 : 0;


    state_table[key].valid =
        true;


    portEXIT_CRITICAL(&state_mux);


    return true;

}



// =====================================================
// 写 LONG
// =====================================================

bool state_set_long(
    SystemStateKey key,
    long value
)
{

    if(
        key < 0 ||
        key >= STATE_MAX
    )
    {
        return false;
    }


    portENTER_CRITICAL(&state_mux);


    state_table[key].type =
        STATE_TYPE_LONG;


    state_table[key].value =
        value;


    state_table[key].valid =
        true;


    portEXIT_CRITICAL(&state_mux);


    return true;

}

// =====================================================
// 写 float
// =====================================================

bool state_set_float(
    SystemStateKey key,
    float value
)
{

    if(
        key < 0 ||
        key >= STATE_MAX
    )
    {
        return false;
    }

    portENTER_CRITICAL(&state_mux);


    state_table[key].type = STATE_TYPE_FLOAT;

    state_table[key].value_float = value;

    state_table[key].valid = true;


    portEXIT_CRITICAL(&state_mux);


    return true;
}


// =====================================================
// 读 INT
// =====================================================

int state_get_int(
    SystemStateKey key
)
{

    if(
        key < 0 ||
        key >= STATE_MAX
    )
    {
        return 0;
    }


    int value;


    portENTER_CRITICAL(&state_mux);


    value =
        (int)state_table[key].value;


    portEXIT_CRITICAL(&state_mux);


    return value;

}



// =====================================================
// 读 BOOL
// =====================================================

bool state_get_bool(
    SystemStateKey key
)
{

    if(
        key < 0 ||
        key >= STATE_MAX
    )
    {
        return false;
    }


    bool value;


    portENTER_CRITICAL(&state_mux);


    value =
        state_table[key].value != 0;


    portEXIT_CRITICAL(&state_mux);


    return value;

}



// =====================================================
// 读 LONG
// =====================================================

long state_get_long(
    SystemStateKey key
)
{

    if(
        key < 0 ||
        key >= STATE_MAX
    )
    {
        return 0;
    }


    long value;


    portENTER_CRITICAL(&state_mux);


    value =
        state_table[key].value;


    portEXIT_CRITICAL(&state_mux);


    return value;

}

// =====================================================
// 读 float
// =====================================================

float state_get_float(
    SystemStateKey key
)
{

    if(key >= STATE_MAX)
        return 0;


    float value;


    portENTER_CRITICAL(&state_mux);


    value =
        state_table[key].value_float;


    portEXIT_CRITICAL(&state_mux);


    return value;
}


// =====================================================
// 状态有效性
// =====================================================

bool state_is_valid(
    SystemStateKey key
)
{

    if(
        key < 0 ||
        key >= STATE_MAX
    )
    {
        return false;
    }


    bool valid;


    portENTER_CRITICAL(&state_mux);


    valid =
        state_table[key].valid;


    portEXIT_CRITICAL(&state_mux);


    return valid;

}


// =====================================================
// 枚举 → 字符串
// =====================================================
const char* state_key_to_string(SystemStateKey key)
{
    for (uint8_t i = 0; i < STATE_MAP_COUNT; i++) {
        if (state_map[i].key == key) {
            return state_map[i].name;
        }
    }
    return "unknown";
}
// 内部私有函数：按名称查找state_map，返回数组下标；找不到返回 -1
static int state_lookup_by_name(const String &str)
{
    for (uint8_t i = 0; i < STATE_MAP_COUNT; i++)
    {
        if (str == state_map[i].name)
        {
            return static_cast<int>(i);
        }
    }
    return -1;
}
// =====================================================
// 字符串 → 枚举
// =====================================================
SystemStateKey state_string_to_key(const String &str)
{
    int idx = state_lookup_by_name(str);
    if(idx < 0)
    {
        return STATE_MAX;
    }
    return state_map[idx].key;
}



// =====================================================
// 通用查询接口
// =====================================================
// =====================================================
// 通用查询接口（完全基于映射表驱动）
// =====================================================
// =====================================================
// 通用查询接口（完全基于映射表驱动）
// =====================================================
bool system_state_query(
    const String &key,
    String &output
)
{
    int idx = state_lookup_by_name(key);
    if (idx < 0)
    {
        return false;
    }

    const auto &item = state_map[idx];
    SystemStateKey state_key = item.key;

    if (!state_is_valid(state_key))
    {
        output = "invalid";
        return true;
    }

    switch (item.type)
    {
        case STATE_TYPE_BOOL:
            output = state_get_bool(state_key) ? "true" : "false";
            break;
        case STATE_TYPE_INT:
            output = String(state_get_int(state_key));
            break;
        case STATE_TYPE_LONG:
            output = String(state_get_long(state_key));
            break;
        case STATE_TYPE_FLOAT:
            output = String(state_get_float(state_key), 1);
            break;
        default:
            return false;
    }
    return true;
}
// =====================================================
// 获取字段列表（供 UI 使用）
// =====================================================
uint8_t state_get_field_count()
{
    uint8_t count = 0;
    for (uint8_t i = 0; i < STATE_MAP_COUNT; i++) {
        if (state_map[i].readable) {
            count++;
        }
    }
    return count;
}


// =====================================================
// 获取可读字段名称
// =====================================================
const char* state_get_field_name(uint8_t index)
{
    uint8_t count = 0;
    for (uint8_t i = 0; i < STATE_MAP_COUNT; i++) {
        if (state_map[i].readable) {
            if (count == index) {
                return state_map[i].name;
            }
            count++;
        }
    }
    return nullptr;
}