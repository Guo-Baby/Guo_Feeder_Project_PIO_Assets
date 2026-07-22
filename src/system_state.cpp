#include <Arduino.h>

#include "system_state.h"


// =====================================================
// 系统状态变量
//
// 所有变量只存在RAM
//
// 断电后消失
//
// =====================================================


// ==========================
// WiFi状态
// ==========================
static bool wifi_status = false;


// ==========================
// 时间状态
// ==========================
static time_t system_time = 0;
static bool time_valid = false;


// ==========================
// NTP状态
// ==========================
static bool ntp_synced = false;
static time_t last_ntp_sync = 0;


// ==========================
// RTC状态
// ==========================
static bool rtc_available = false;


// ==========================
// 错误状态
// ==========================
static uint16_t error_code = 0;


// ==========================
// 运行状态
// ==========================
static SystemRunState run_state =
    SYSTEM_IDLE;

void system_state_init()
{

    wifi_status = false;


    system_time = 0;
    time_valid = false;


    ntp_synced = false;
    last_ntp_sync = 0;


    rtc_available = false;


    error_code = 0;


    run_state = SYSTEM_IDLE;


    Serial.println(
        "System state init OK"
    );

}

// =====================================================
// WiFi
// =====================================================
void system_set_wifi_status(
    bool status
)
{
    wifi_status = status;
}

bool system_get_wifi_status()
{
    return wifi_status;
}


// =====================================================
// 时间
// =====================================================
void system_set_time(
    time_t timestamp
)
{
    system_time = timestamp;


    if(timestamp > 0)
    {
        time_valid = true;
    }
    else
    {
        time_valid = false;
    }

}

time_t system_get_time()
{
    return system_time;
}


bool system_is_time_valid()
{
    return time_valid;
}

// =====================================================
// NTP
// =====================================================
void system_set_ntp_synced(
    bool status
)
{
    ntp_synced = status;
}

bool system_is_ntp_synced()
{
    return ntp_synced;
}

void system_set_last_ntp_sync(
    time_t timestamp
)
{
    last_ntp_sync = timestamp;
}

time_t system_get_last_ntp_sync()
{
    return last_ntp_sync;
}


// =====================================================
// RTC
// =====================================================
void system_set_rtc_available(
    bool status
)
{
    rtc_available = status;
}

bool system_is_rtc_available()
{
    return rtc_available;
}


// =====================================================
// 错误
// =====================================================
void system_set_error_code(
    uint16_t code
)
{
    error_code = code;
}

uint16_t system_get_error_code()
{
    return error_code;
}


// =====================================================
// 系统运行状态
// =====================================================
void system_set_run_state(
    SystemRunState state
)
{
    run_state = state;
}

SystemRunState system_get_run_state()
{
    return run_state;
}