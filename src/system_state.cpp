#include <Arduino.h>

#include "system_state.h"


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

    if(key >= STATE_MAX)
        return false;


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