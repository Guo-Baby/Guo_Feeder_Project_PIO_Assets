#include <Arduino.h>
#include <time.h>

#include "config_manager.h"
#include "time_manager.h"
#include "system_state.h"
#include "event_manager.h"

// =====================================================
// NTP服务器配置
// =====================================================
static String ntp_server1;

static String ntp_server2;

// 中国时区
// UTC+8
static int timezone_offset;

#define DAYLIGHT_OFFSET_SEC 0

// =====================================================
// 自动校时间隔
// 7天一次
// =====================================================

static unsigned long ntp_sync_interval_sec;

// =====================================================
// 时间状态机
// =====================================================

enum TimeState
{
    TIME_WAIT_WIFI = 0,
    TIME_NEED_SYNC,
    TIME_SYNCING,
    TIME_READY
};

static TimeState time_state =
    TIME_WAIT_WIFI;

// =====================================================
// 内部变量
// =====================================================

static unsigned long lastCheckTime = 0;

// 状态检查周期
// 不是NTP周期

#define TIME_CHECK_INTERVAL 1000

// 防止重复同步

static bool sync_running = false;

// =====================================================
// 时间初始化
// =====================================================

void time_init()
{
    Serial.println();

    Serial.println(
        "Time manager init..."
    );

    time_state = TIME_WAIT_WIFI;

    Serial.println(
        "Time waiting WiFi..."
    );


    ntp_server1 =
    config_get_ntp_server1();


    ntp_server2 =
    config_get_ntp_server2();


    timezone_offset =
    config_get_timezone();


    ntp_sync_interval_sec =
    config_get_ntp_sync_interval_day()
    *
    24
    *
    60
    *
    60;
}

// =====================================================
// 时间任务
//
// loop调用
//
// =====================================================

void time_task()
{
    // 非阻塞1秒检查一次

    if(
        millis() - lastCheckTime
        <
        TIME_CHECK_INTERVAL
    )
    {
        return;
    }

    lastCheckTime = millis();

    switch(time_state)
    {
        // =========================
        // 等待WiFi
        // =========================

       case TIME_WAIT_WIFI:
{
    if(
       event_get(EVENT_WIFI_CONNECTED)
    )
    {
        Serial.println(
            "WiFi event received"
        );

        time_state =
        TIME_NEED_SYNC;
    }

    break;
}

        // =========================
        // 判断是否需要同步
        // =========================

        case TIME_NEED_SYNC:
        {
            time_t last_sync =
                system_get_last_ntp_sync();

            time_t now =
                time(nullptr);

            // 第一次同步

            if(
                last_sync == 0
            )
            {
                time_state =
                    TIME_SYNCING;
            }

            else if(
                now - last_sync
                >
                ntp_sync_interval_sec
            )
            {
                Serial.println(
                    "NTP sync period reached"
                );

                time_state =
                    TIME_SYNCING;
            }

            else
            {
                time_state =
                    TIME_READY;
            }

            break;
        }

        // =========================
        // 执行同步
        // =========================

        case TIME_SYNCING:
        {
            if(
                time_sync_ntp()
            )
            {
                time_state =
                    TIME_READY;
            }

            else
            {
                //失败后等待下一次

                time_state =
                    TIME_WAIT_WIFI;
            }

            break;
        }

        // =========================
        // 正常运行
        // =========================
        #define TIME_SYNC_CHECK_INTERVAL 3600000
        static unsigned long lastSyncCheck = 0;
       case TIME_READY:
{
    if(
        event_get(EVENT_WIFI_DISCONNECTED)
    )
    {
        time_state =
        TIME_WAIT_WIFI;

        break;
    }

    if(
        millis()-lastSyncCheck
        >
        TIME_SYNC_CHECK_INTERVAL
    )
    {
        lastSyncCheck =
        millis();

        time_state =
        TIME_NEED_SYNC;
    }

    break;
}

    }
}

// =====================================================
// NTP同步
// =====================================================

bool time_sync_ntp()
{
    if(sync_running)
    {
        return false;
    }

    if(
         !system_get_wifi_status()
    )
    {
        Serial.println(
            "No WiFi, skip NTP"
        );

        return false;
    }

    sync_running = true;

    Serial.println(
        "NTP syncing..."
    );

    configTime(
        timezone_offset * 3600,
         DAYLIGHT_OFFSET_SEC,
         ntp_server1.c_str(),
         ntp_server2.c_str()
    );

    struct tm timeinfo;

    bool success = false;

    // 最多等待5秒

    unsigned long start =
        millis();

    while(
        millis() - start < 5000
    )
    {
        if(
            getLocalTime(
                &timeinfo
            )
        )
        {
            success = true;

            break;
        }

        delay(100);
    }

    if(success)
    {
        time_t now =
            time(nullptr);

        system_set_time(now);

        system_set_ntp_synced(true);

        system_set_last_ntp_sync(now);
        event_push(
            EVENT_NTP_SYNC_OK
        );

        Serial.println("NTP sync OK");
       // =====================
       // 输出当前系统时间
       // =====================

       Serial.print(
          "System Time: "
        );

Serial.println(
    time_get_string()
);

        // =====================
        // 未来RTC写入位置
        // =====================

        time_sync_to_rtc();
    }

    else
    {
        Serial.println(
            "NTP sync failed"
        );
    }

    sync_running = false;

    return success;
}

// =====================================================
// 获取时间字符串
// =====================================================

String time_get_string()
{
    time_t now =
        system_get_time();

    if(        now == 0    )
    {
        return "No Time";
    }

    struct tm timeinfo;

    localtime_r(
        &now,
        &timeinfo
    );

    char buffer[32];

    sprintf(
        buffer,
        "%04d-%02d-%02d %02d:%02d:%02d",
        timeinfo.tm_year + 1900,
        timeinfo.tm_mon + 1,
        timeinfo.tm_mday,
        timeinfo.tm_hour,
        timeinfo.tm_min,
        timeinfo.tm_sec
    );
    return String(buffer);
}

// =====================================================
// RTC接口
//
// 当前没有RTC硬件
//
// =====================================================

void time_sync_to_rtc()
{
    /*
    
    TODO:

    RTC模块到货后：

    这里调用：

    rtc_write_time(
        system_get_time()
    );

    */
}