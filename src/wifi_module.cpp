#include <Arduino.h>
#include <WiFi.h>

#include "wifi_module.h"
#include "config_manager.h"
#include "system_state.h"
#include "event_manager.h"


// ==========================
// WiFi状态机
// ==========================

enum WifiState
{
    WIFI_IDLE,
    WIFI_CONNECTING,
    WIFI_CONNECTED,
    WIFI_DISCONNECTED
};


static WifiState wifi_state =
    WIFI_IDLE;



// ==========================
// WiFi运行状态
// ==========================

static bool wifi_connected = false;


// 连接开始时间

static unsigned long wifi_connect_start = 0;


// 重连计时

static unsigned long lastReconnectTime = 0;



// ==========================
// 信号更新
// ==========================

static unsigned long lastSignalUpdate = 0;

static unsigned long signal_update_interval = 10000;



// ==========================
// 参数
// ==========================

static unsigned long wifi_connect_timeout = 30000;

static unsigned long wifi_reconnect_interval = 10000;



// ==========================
// RSSI转换信号等级
// ==========================

static int wifi_calculate_signal(
    int rssi
)
{

    if(rssi >= -50)
    {
        return 4;
    }
    else if(rssi >= -65)
    {
        return 3;
    }
    else if(rssi >= -75)
    {
        return 2;
    }
    else if(rssi >= -90)
    {
        return 1;
    }
    else
    {
        return 0;
    }

}



// ==========================
// 更新WiFi信号状态
// ==========================

static void wifi_update_signal()
{

    if(
        millis()
        -
        lastSignalUpdate
        <
        signal_update_interval
    )
    {
        return;
    }


    lastSignalUpdate =
        millis();



    if(
        WiFi.status()
        !=
        WL_CONNECTED
    )
    {
        return;
    }



    int rssi =
        WiFi.RSSI();



    state_set_int(
        STATE_WIFI_RSSI,
        rssi
    );



    state_set_int(
        STATE_WIFI_SIGNAL,
        wifi_calculate_signal(rssi)
    );

}



// ==========================
// WiFi初始化
// ==========================

void wifi_init()
{

    Serial.println();

    Serial.println(
        "WiFi init..."
    );


    WiFi.mode(
        WIFI_STA
    );


    wifi_connect_timeout =
        config_get_wifi_connect_timeout();


    wifi_reconnect_interval =
        config_get_wifi_reconnect_interval();


    wifi_state =
        WIFI_IDLE;


    wifi_connected =
        false;



    state_set_bool(
        STATE_WIFI_STATUS,
        false
    );


    state_set_int(
        STATE_WIFI_STATE,
        WIFI_IDLE
    );


    state_set_int(
        STATE_WIFI_RSSI,
        -100
    );


    state_set_int(
        STATE_WIFI_SIGNAL,
        0
    );

}



// ==========================
// 开始连接
// ==========================

static void wifi_start_connect()
{

    String ssid =
        config_get_wifi_ssid();


    String password =
        config_get_wifi_password();



    Serial.println();

    Serial.print(
        "Connecting to:"
    );

    Serial.println(
        ssid
    );



    WiFi.begin(
        ssid.c_str(),
        password.c_str()
    );



    wifi_connect_start =
        millis();



    wifi_state =
        WIFI_CONNECTING;



    state_set_int(
        STATE_WIFI_STATE,
        WIFI_CONNECTING
    );

}



// ==========================
// WiFi任务
// loop调用
// ==========================

void wifi_task()
{

    wifi_update_signal();


    switch(wifi_state)
    {


        case WIFI_IDLE:
        {

            wifi_start_connect();

            break;

        }



        case WIFI_CONNECTING:
        {

            if(
                WiFi.status()
                ==
                WL_CONNECTED
            )
            {

                if(!wifi_connected)
                {

                    Serial.println(
                        "WiFi connected"
                    );


                    wifi_connected =
                        true;


                    state_set_bool(
                        STATE_WIFI_STATUS,
                        true
                    );


                    event_push(
                        EVENT_WIFI_CONNECTED,
                        "",
                        "wifi_module",
                        EVENT_PRIORITY_NORMAL,
                        EVENT_POLICY_NORMAL,
                        0
                    );


                }


                wifi_state =
                    WIFI_CONNECTED;


                state_set_int(
                    STATE_WIFI_STATE,
                    WIFI_CONNECTED
                );


            }
            else
            {

                if(
                    millis()
                    -
                    wifi_connect_start
                    >
                    wifi_connect_timeout
                )
                {

                    Serial.println(
                        "WiFi connect timeout"
                    );


                    wifi_state =
                        WIFI_DISCONNECTED;


                    state_set_bool(
                        STATE_WIFI_STATUS,
                        false
                    );


                    state_set_int(
                        STATE_WIFI_STATE,
                        WIFI_DISCONNECTED
                    );

                }

            }


            break;

        }




        case WIFI_CONNECTED:
        {


            if(
                WiFi.status()
                !=
                WL_CONNECTED
            )
            {

                Serial.println(
                    "WiFi lost"
                );


                wifi_connected =
                    false;



                state_set_bool(
                    STATE_WIFI_STATUS,
                    false
                );



                event_push(
                    EVENT_WIFI_DISCONNECTED,
                    "",
                    "wifi_module",
                    EVENT_PRIORITY_NORMAL,
                    EVENT_POLICY_NORMAL,
                    0
                );



                wifi_state =
                    WIFI_DISCONNECTED;


                state_set_int(
                    STATE_WIFI_STATE,
                    WIFI_DISCONNECTED
                );

            }


            break;

        }




        case WIFI_DISCONNECTED:
        {


            if(
                millis()
                -
                lastReconnectTime
                >
                wifi_reconnect_interval
            )
            {

                lastReconnectTime =
                    millis();



                Serial.println(
                    "Try reconnect..."
                );



                wifi_start_connect();

            }


            break;

        }


    }

}



// ==========================
// 查询状态
// ==========================

bool wifi_is_connected()
{

    return wifi_connected;

}



// ==========================
// RSSI查询接口
// ==========================

int wifi_get_rssi()
{

    if(
        WiFi.status()
        ==
        WL_CONNECTED
    )
    {
        return WiFi.RSSI();
    }


    return -100;

}



// ==========================
// 信号等级查询
// ==========================

int wifi_get_signal_quality()
{

    return wifi_calculate_signal(
        wifi_get_rssi()
    );

}