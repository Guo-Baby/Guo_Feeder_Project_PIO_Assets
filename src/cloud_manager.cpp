#include <Arduino.h>
#include <PubSubClient.h>
#include <WiFi.h>


#include "cloud_manager.h"
#include "system_state.h"
#include "config_manager.h"
#include "cloud_publish.h"
#include "cloud_receive.h"



// =====================================================
// MQTT对象
// =====================================================

static WiFiClient espClient;


static PubSubClient mqttClient(
    espClient
);



// =====================================================
// MQTT参数
// 从config.json读取
// =====================================================

static String mqtt_server;

static int mqtt_port;


static String mqtt_client_id;

static String mqtt_sub_topic;


static unsigned long retry_interval;

static int retry_max;


static unsigned long sleep_retry_interval;

static int keep_alive;

// =====================================================
// MQTT状态
// =====================================================

static bool mqtt_connected = false;



// =====================================================
// 重连管理
// =====================================================

// 上次尝试时间
static unsigned long last_try_time = 0;


// 快速重试次数
static int retry_count = 0;


// 重连模式
enum RetryMode
{
    FAST_RETRY,
    SLOW_RETRY
};


static RetryMode retry_mode =
    FAST_RETRY;



// =====================================================
// MQTT消息回调
// =====================================================
//接受消息
static void mqtt_callback(
    char* topic,
    byte* payload,
    unsigned int length
)
{   //转换为字符串
    String message;
    for(
        unsigned int i=0;
        i<length;
        i++
    )
    {
        message +=
            (char)payload[i];
    }
    cloud_receive_handle(
        topic,
        message
    );

}


// =====================================================
// 初始化
// =====================================================

void cloud_init()

{

    Serial.println();

    Serial.println(
        "Cloud manager init..."
    );



    // =====================
    // 读取配置
    // =====================

    mqtt_server =
        config_get_mqtt_server();


    mqtt_port =
        config_get_mqtt_port();


    mqtt_client_id =
        config_get_mqtt_client_id();


    mqtt_sub_topic =
        config_get_mqtt_subscribe_topic();



    retry_interval =
        config_get_mqtt_retry_interval();


    retry_max =
        config_get_mqtt_retry_max();


    sleep_retry_interval =
        config_get_mqtt_sleep_interval();


    keep_alive =
        config_get_mqtt_keep_alive();





    Serial.println(
        "MQTT Config:"
    );


    Serial.print(
        "Server:"
    );

    Serial.println(
        mqtt_server
    );


    Serial.print(
        "Port:"
    );

    Serial.println(
        mqtt_port
    );


    Serial.print(
        "Topic:"
    );

    Serial.println(
        mqtt_sub_topic
    );



    // =====================
    // MQTT初始化
    // =====================


    mqttClient.setBufferSize(
        512
    );


    mqttClient.setServer(
        mqtt_server.c_str(),
        mqtt_port
    );



    mqttClient.setCallback(
        mqtt_callback
    );



    mqttClient.setKeepAlive(
        keep_alive
    );



    mqtt_connected=false;


    retry_count=0;


    retry_mode =
        FAST_RETRY;


}



// =====================================================
// MQTT连接
// =====================================================

static void cloud_connect()

{


    Serial.println();


    Serial.println(
        "MQTT connecting..."
    );



    bool result =
    mqttClient.connect(
       mqtt_client_id.c_str(),
        "",
        ""
    );



    if(result)

    {


        Serial.println(
            "MQTT connected OK"
        );



        mqtt_connected=true;



        retry_count=0;



        retry_mode =
            FAST_RETRY;




        bool sub_result =
        mqttClient.subscribe(
            mqtt_sub_topic.c_str()
        );



        if(sub_result)

        {

            Serial.print(
                "Subscribe topic:"
            );


            Serial.println(
                mqtt_sub_topic
            );


            Serial.println(
                "MQTT subscribe OK"
            );

        }

        else

        {

            Serial.println(
                "MQTT subscribe FAIL"
            );

        }



        // 上线通知

        cloud_send_raw(
            "device_online"
        );


    }


    else

    {


        Serial.print(
            "MQTT failed:"
        );


        Serial.println(
            mqttClient.state()
        );



        switch(
            mqttClient.state()
        )

        {

            case -4:

                Serial.println(
                    "TIMEOUT"
                );

                break;


            case 5:

                Serial.println(
                    "AUTH FAILED"
                );

                break;


            default:

                break;

        }



        mqtt_connected=false;



        retry_count++;



        // 快速重试达到上限

        if(
            retry_count >= retry_max
        )

        {

            Serial.println(
                "Enter slow retry mode"
            );


            retry_mode =
                SLOW_RETRY;

        }


    }


}




// =====================================================
// MQTT任务
// loop调用
// =====================================================

void cloud_task()

{


    // ==========================
    // 在线状态
    // ==========================

    if(mqtt_connected)

    {


        mqttClient.loop();



        if(
            !mqttClient.connected()
        )

        {

            Serial.println(
                "MQTT disconnected"
            );


            mqtt_connected=false;


        }


        return;


    }



    // ==========================
    // 离线重连
    // ==========================


    unsigned long interval;



    if(
        retry_mode == FAST_RETRY
    )

    {

        interval =
            retry_interval;

    }

    else

    {

        interval =
            sleep_retry_interval;

    }




    if(
        millis()-last_try_time
        <
        interval
    )

    {

        return;

    }



    last_try_time =
        millis();



    cloud_connect();
}


// =====================================================
// MQTT底层发送,调用cloud_publish
// =====================================================

bool cloud_send_raw(
    const char* message
)

{

    if(
        message == nullptr
    )

    {

        Serial.println(
            "MQTT send null"
        );

        return false;

    }



    if(
        !mqtt_connected
    )

    {

        Serial.println(
            "MQTT offline"
        );

        return false;

    }



    bool result =
    mqttClient.publish(
        mqtt_sub_topic.c_str(),
        message
    );



    if(result)

    {

        Serial.println(
            "MQTT send OK"
        );


        Serial.print(
            "Message:"
        );


        Serial.println(
            message
        );

    }

    else

    {

        Serial.println(
            "MQTT send FAIL"
        );

    }



    return result;

}



