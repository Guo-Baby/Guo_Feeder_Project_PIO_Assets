#include <Arduino.h>
#include <PubSubClient.h>
#include <WiFi.h>
#include <ArduinoJson.h>

#include "cloud_manager.h"
#include "system_state.h"
#include "config_manager.h"
#include "event_manager.h"

// =====================================================
// MQTT对象
// =====================================================
static WiFiClient espClient;
static PubSubClient mqttClient(espClient);

// =====================================================
// MQTT参数（从config读取）
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
// MQTT连接状态
// =====================================================
static bool mqtt_connected = false;

// =====================================================
// 重连管理
// =====================================================
static unsigned long last_try_time = 0;
static int retry_count = 0;

enum RetryMode
{
    FAST_RETRY,
    SLOW_RETRY
};
static RetryMode retry_mode = FAST_RETRY;

// =====================================================
// 云端接收缓存（原cloud_receive）
// =====================================================
static String last_command;

// =====================================================
// MQTT消息回调（接收处理，原cloud_receive_handle）
// =====================================================
static void mqtt_callback(char* topic, byte* payload, unsigned int length)
{
    String message;
    for (unsigned int i = 0; i < length; i++)
    {
        message += (char)payload[i];
    }

    if (message.length() == 0)
    {
        Serial.println("Empty command");
        return;
    }

    Serial.println("Topic:");
    Serial.println(topic);
    Serial.println("Cloud command:");
    Serial.println(message);

    last_command = message;
    // 推送事件给EventManager
    event_push(EVENT_CLOUD_COMMAND, message, "mqtt");
}

// =====================================================
// MQTT建立连接
// =====================================================
static void cloud_connect()
{
    Serial.println();
    Serial.println("MQTT connecting...");

    bool result = mqttClient.connect(mqtt_client_id.c_str(), "", "");

    if (result)
    {
        Serial.println("MQTT connected OK");
        mqtt_connected = true;
        retry_count = 0;
        retry_mode = FAST_RETRY;

        bool sub_result = mqttClient.subscribe(mqtt_sub_topic.c_str());
        if (sub_result)
        {
            Serial.print("Subscribe topic:");
            Serial.println(mqtt_sub_topic);
            Serial.println("MQTT subscribe OK");
        }
        else
        {
            Serial.println("MQTT subscribe FAIL");
        }

        // 上线通知
        cloud_send_raw("device_online");
    }
    else
    {
        Serial.print("MQTT failed:");
        Serial.println(mqttClient.state());

        switch (mqttClient.state())
        {
            case -4:
                Serial.println("TIMEOUT");
                break;
            case 5:
                Serial.println("AUTH FAILED");
                break;
            default:
                break;
        }

        mqtt_connected = false;
        retry_count++;

        if (retry_count >= retry_max)
        {
            Serial.println("Enter slow retry mode");
            retry_mode = SLOW_RETRY;
        }
    }
}

// =====================================================
// 初始化入口
// =====================================================
void cloud_init()
{
    Serial.println();
    Serial.println("Cloud manager init...");

    // 读取配置
    mqtt_server = config_get_mqtt_server();
    mqtt_port = config_get_mqtt_port();
    mqtt_client_id = config_get_mqtt_client_id();
    mqtt_sub_topic = config_get_mqtt_subscribe_topic();

    retry_interval = config_get_mqtt_retry_interval();
    retry_max = config_get_mqtt_retry_max();
    sleep_retry_interval = config_get_mqtt_sleep_interval();
    keep_alive = config_get_mqtt_keep_alive();

    Serial.println("MQTT Config:");
    Serial.print("Server:");
    Serial.println(mqtt_server);
    Serial.print("Port:");
    Serial.println(mqtt_port);
    Serial.print("Topic:");
    Serial.println(mqtt_sub_topic);

    // MQTT客户端配置
    mqttClient.setBufferSize(512);
    mqttClient.setServer(mqtt_server.c_str(), mqtt_port);
    mqttClient.setCallback(mqtt_callback);
    mqttClient.setKeepAlive(keep_alive);

    mqtt_connected = false;
    retry_count = 0;
    retry_mode = FAST_RETRY;
    last_command.clear();
}

// =====================================================
// 云主循环任务
// =====================================================
void cloud_task()
{
    if (mqtt_connected)
    {
        mqttClient.loop();
        if (!mqttClient.connected())
        {
            Serial.println("MQTT disconnected");
            mqtt_connected = false;
        }
        return;
    }

    unsigned long interval;
    if (retry_mode == FAST_RETRY)
    {
        interval = retry_interval;
    }
    else
    {
        interval = sleep_retry_interval;
    }

    if (millis() - last_try_time < interval)
    {
        return;
    }
    last_try_time = millis();
    cloud_connect();
}

// =====================================================
// 底层原始发送
// =====================================================
bool cloud_send_raw(const char* message)
{
    if (message == nullptr)
    {
        Serial.println("MQTT send null");
        return false;
    }
    if (!mqtt_connected)
    {
        Serial.println("MQTT offline");
        return false;
    }

    bool result = mqttClient.publish(mqtt_sub_topic.c_str(), message);
    if (result)
    {
        Serial.println("MQTT send OK");
        Serial.print("Message:");
        Serial.println(message);
    }
    else
    {
        Serial.println("MQTT send FAIL");
    }
    return result;
}

// =====================================================
// 发布普通消息（原cloud_publish_message）
// =====================================================
bool cloud_publish_message(const char* message)
{
    if (message == nullptr)
    {
        Serial.println("Publish null message");
        return false;
    }
    return cloud_send_raw(message);
}

// =====================================================
// 发布状态报文（原cloud_publish_status）
// =====================================================
bool cloud_publish_status()
{
    String json;
    json = "{";
    json += "\"event\":\"status\"";
    json += "}";
    return cloud_send_raw(json.c_str());
}

// =====================================================
// 新增：直接传入JsonDocument序列化发布
// =====================================================
bool cloud_publish_json(JsonDocument& doc)
{
    String buf;
    serializeJson(doc, buf);
    return cloud_send_raw(buf.c_str());
}

// =====================================================
// 获取最新云端指令（原cloud_receive_get_command）
// =====================================================
String cloud_receive_get_command()
{
    return last_command;
}

// =====================================================
// 辅助：查询在线状态（新增对外接口）
// =====================================================
bool cloud_is_connected()
{
    return mqtt_connected;
}