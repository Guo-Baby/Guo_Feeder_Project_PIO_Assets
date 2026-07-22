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
// 参数
// ==========================

static unsigned long wifi_connect_timeout = 30000;

static unsigned long wifi_reconnect_interval = 10000;



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


    system_set_wifi_status(
        false
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

}



// ==========================
// WiFi任务
// loop调用
// ==========================

void wifi_task()
{


    switch(wifi_state)
    {


        // ======================
        // 空闲
        // ======================

        case WIFI_IDLE:
        {

            wifi_start_connect();

            break;
        }



        // ======================
        // 连接中
        // ======================

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

                    // 更新系统状态
                    system_set_wifi_status(
                        true
                    );

                    // 发布事件
                    event_push(
                        EVENT_WIFI_CONNECTED
                    );


                    Serial.println(
                        "EVENT WIFI CONNECTED"
                    );
                }


              



                wifi_state =
                    WIFI_CONNECTED;



            }
            else
            {


                // 超时

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


                    system_set_wifi_status(
                        false
                    );

                }


            }


            break;

        }




        // ======================
        // 已连接
        // ======================

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



                system_set_wifi_status(
                    false
                );



                event_push(
                    EVENT_WIFI_DISCONNECTED
                );



                Serial.println(
                    "EVENT WIFI DISCONNECTED"
                );



                wifi_state =
                    WIFI_DISCONNECTED;

            }


            break;

        }




        // ======================
        // 断开
        // ======================

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
//
// 保留接口
//
// 未来调试使用
//
// Time Manager不要调用
//

bool wifi_is_connected()
{

    return wifi_connected;

}