#define ARDUINOJSON_USE_CBOR 1
#define STR_HELPER(x) #x
#define STR(x) STR_HELPER(x)
#include <Arduino.h>
#include <PubSubClient.h>
#include <WiFi.h>
#include <ArduinoJson.h>

#include "cloud_manager.h"
#include "system_state.h"
#include "config_manager.h"
#include "event_manager.h"
#include "time_manager.h"
#include "command_manager.h"

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
// WiFi状态
// =====================================================
static bool wifi_connected = false;
// =====================================================
// MQTT重连控制
// =====================================================
static unsigned long mqtt_retry_timer = 0;
static uint8_t mqtt_retry_count = 0;
// 最大连续失败次数
#define MQTT_RETRY_MAX 10
// 最大退避时间
#define MQTT_RETRY_MAX_INTERVAL 120000
// 失败休眠时间
#define MQTT_FAIL_SLEEP_TIME 3600000
static bool mqtt_sleep_mode = false;
// =====================================================
// 防重复缓存
// =====================================================
#define MQTT_DUP_CACHE_SIZE 10
#define MQTT_DUP_CACHE_TTL_MS 30000UL

// 短周期 FIFO 防风暴缓存：仅防 MQTT 重复投递 / UI 重复点击 / 消息风暴
struct CmdIdCacheEntry
{
    String cmd_id;
    unsigned long received_ms;
};

static CmdIdCacheEntry cmd_id_cache[MQTT_DUP_CACHE_SIZE];
static uint8_t cmd_cache_index = 0;

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
// =====================================================
// MQTT消息回调
// 接收云端命令
// =====================================================
// 短周期 cmd_id 防风暴过滤（定义在文件底部，此处前向声明）
static bool cloud_check_duplicate_cmd(const String &id);

static void mqtt_callback(
    char* topic,
    byte* payload,
    unsigned int length
)
{
    String message;
    for(unsigned int i=0;i<length;i++)
    {
        message += (char)payload[i];
    }
    if(message.length()==0)
    {
        Serial.println("[Cloud] Empty MQTT message");
        return;
    }
    Serial.println("=================");
    Serial.println("[Cloud] MQTT RX");
    Serial.println(message);
    JsonDocument doc;
    DeserializationError error =
        deserializeJson(doc,message);
    if(error)
    {
        Serial.println("[Cloud] JSON parse failed");
        return;
    }
    const char* command =
        doc["cmd"];
    const char* object =
        doc["ob"];
    const char* source =
        doc["src"];
    if(command==nullptr)
    {
        Serial.println("[Cloud] Missing cmd");
        return;
    }
    String cmd_id =
        doc["id"] | "";
    if(cmd_id.length()==0)
    {
        Serial.println("[Cloud] Missing id");
        return;
    }
    if(cloud_check_duplicate_cmd(cmd_id))
    {
        Serial.println("[Cloud] Command duplicate");
        return;
    }
    unsigned long timestamp =
        doc["ts"] | 0;
    CommandMessage cmd;
    cmd.command =
        command;
    cmd.object =
        object ? object : "";
    cmd.cmd_id =
        cmd_id;
    if(!doc["pl"].isNull())
    {
        // pl 原样序列化为 String 透明传递，不解析内部字段
        serializeJson(
            doc["pl"],
            cmd.payload
        );
    }
    cmd.source =
        source ? source : "";
    cmd.timestamp =
        timestamp;
    JsonDocument ack;
    ack["cmd"]="ack";
    ack["ob"] =
        cmd.object;
    ack["id"] =
        cmd.cmd_id;
    JsonObject pl =
        ack["pl"].to<JsonObject>();
    pl["result"]="received";
    ack["src"]="esp32";
    ack["ts"]= time_get();
    String ack_string;
    serializeJson(
        ack,
        ack_string
    );
    cloud_send_up(
        ack_string.c_str()
    );
    // 执行命令：同步返回 String 不在此处理，
    // 结果只经已注册 CommandResultCallback → on_command_result → cloud_send_up 单通道上报
    command_manager_execute(cmd);
}

//对wifi连接事件的处理，主要是为了在wifi连接后立即尝试连接云端
static void cloud_event_callback(const EventMessage &message)
{
    SystemEvent event = message.event;
    switch(message.event)
    {
        case EVENT_WIFI_CONNECTED:
            Serial.println(
                "[Cloud] WiFi connected event"
            );
            wifi_connected = true;
            mqtt_sleep_mode = false;
            mqtt_retry_count = 0;
            mqtt_retry_timer = 0;
            break;
        case EVENT_WIFI_DISCONNECTED:
            Serial.println(
                "[Cloud] WiFi disconnected event"
            );
            wifi_connected = false;
            mqtt_connected = false;
            mqttClient.disconnect();
            break;
        default:
            break;
    }
}

// =====================================================
// MQTT建立连接
// =====================================================
static bool cloud_connect()
{
    if(!wifi_connected)
    {
        Serial.println("[Cloud] WiFi offline, skip MQTT");
        return false;
    }
    Serial.println("[Cloud] MQTT connecting...");
    bool result =
        mqttClient.connect(
            mqtt_client_id.c_str(),
            "",
            ""
        );
    if(result)
    {
        Serial.println(
            "[Cloud] MQTT connected"
        );
        mqtt_connected = true;
        mqtt_retry_count = 0;
        mqtt_sleep_mode = false;
        state_set_bool(
            STATE_MQTT_STATUS,
            true
        );
        state_set_int(
            STATE_MQTT_RETRY_COUNT,
            0
        );
        state_set_string(
            STATE_MQTT_LAST_CONNECT_TIME,
            time_now_string()
        );
        state_set_int(
            STATE_MQTT_LAST_ERROR,
            0
        );
        bool sub =
            mqttClient.subscribe(
                mqtt_sub_topic.c_str()
            );
        if(sub)
        {
            Serial.println(
                "[Cloud] MQTT subscribe OK"
            );
            event_push(
                EVENT_CLOUD_CONNECTED,
                "",
                "cloud",
                EVENT_PRIORITY_NORMAL,
                EVENT_POLICY_DEDUP
            );
        }
        else
        {
            Serial.println(
                "[Cloud] MQTT subscribe FAIL"
            );
            mqtt_connected = false;
            state_set_bool(
                STATE_MQTT_STATUS,
                false
            );
            state_set_int(
                STATE_MQTT_LAST_ERROR,
                mqttClient.state()
            );
            mqttClient.disconnect();
            return false;
        }
        cloud_send_set(
            "{\"cmd\":\"system\",\"id\":\"online\",\"src\":\"device\"}"
        );
        return true;
    }
    Serial.print(
        "[Cloud] MQTT connect failed:"
    );
    Serial.println(
        mqttClient.state()
    );
    mqtt_connected = false;
    state_set_bool(
        STATE_MQTT_STATUS,
        false
    );
    state_set_int(
        STATE_MQTT_RETRY_COUNT,
        mqtt_retry_count + 1
    );
    state_set_int(
        STATE_MQTT_LAST_ERROR,
        mqttClient.state()
    );
    mqtt_retry_count++;
    if(mqtt_retry_count >= MQTT_RETRY_MAX)
    {
        Serial.println(
            "[Cloud] enter sleep retry"
        );
        mqtt_sleep_mode = true;
        mqtt_retry_timer = millis();
    }
    return false;
}

// =====================================================
// Command Manager 结果回调
// =====================================================
static void on_command_result(const String &json)
{
    cloud_send_up(json.c_str());
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
    mqttClient.setBufferSize(1024);
    mqttClient.setServer(mqtt_server.c_str(), mqtt_port);
    mqttClient.setCallback(mqtt_callback);
    mqttClient.setKeepAlive(keep_alive);

    mqtt_connected = false;
    mqtt_retry_count = 0;
    retry_mode = FAST_RETRY;
    last_command.clear();

        // ===== 注册 Command Manager 结果回调 =====
    command_manager_set_result_callback(on_command_result);


    event_subscribe(EVENT_WIFI_CONNECTED, cloud_event_callback);
    event_subscribe(EVENT_WIFI_DISCONNECTED, cloud_event_callback);
}



// =====================================================
// 云主循环任务
// =====================================================
void cloud_task()
{
    if(!wifi_connected)
    {
        if(mqtt_connected)
        {
            Serial.println(
                "[Cloud] WiFi lost, MQTT offline"
            );
            mqtt_connected = false;
            mqttClient.disconnect();
            state_set_bool(
                STATE_MQTT_STATUS,
                false
            );
            state_set_int(
                STATE_MQTT_LAST_ERROR,
                -1
            );
            event_push(
                EVENT_CLOUD_DISCONNECTED,
                "",
                "cloud",
                EVENT_PRIORITY_NORMAL,
                EVENT_POLICY_STATE
            );
        }
        return;
    }
    if(mqtt_connected)
    {
        mqttClient.loop();
        if(!mqttClient.connected())
        {
            Serial.println(
                "[Cloud] MQTT lost"
            );
            mqtt_connected = false;
            state_set_bool(
                STATE_MQTT_STATUS,
                false
            );
            state_set_int(
                STATE_MQTT_LAST_ERROR,
                mqttClient.state()
            );
            event_push(
                EVENT_CLOUD_DISCONNECTED,
                "",
                "cloud",
                EVENT_PRIORITY_NORMAL,
                EVENT_POLICY_STATE
            );
        }
        return;
    }
    unsigned long now = millis();
    unsigned long interval;
    if(mqtt_sleep_mode)
    {
        interval =
            MQTT_FAIL_SLEEP_TIME;
    }
    else
    {
        interval =
            5000 * (mqtt_retry_count + 1);
        if(interval > MQTT_RETRY_MAX_INTERVAL)
        {
            interval =
                MQTT_RETRY_MAX_INTERVAL;
        }
    }
    if(now - mqtt_retry_timer < interval)
    {
        return;
    }
    mqtt_retry_timer = now;
    cloud_connect();
}



// =====================================================
// 底层原始发送，分为/set消息和/up消息
// =====================================================
bool cloud_add_readable_time(
    String &message
)
{
    int ts_pos =
        message.indexOf("\"ts\":");
    if(ts_pos < 0)
    {
        return false;
    }
    int ts_start =
        ts_pos + 5;
    while(
        ts_start < message.length() &&
        message[ts_start] == ' '
    )
    {
        ts_start++;
    }
    int ts_end =
        ts_start;
    while(
        ts_end < message.length() &&
        isDigit(message[ts_end])
    )
    {
        ts_end++;
    }
    if(
        ts_end <= ts_start
    )
    {
        return false;
    }
    unsigned long timestamp =
        message.substring(
            ts_start,
            ts_end
        ).toInt();
    if(timestamp == 0)
    {
        return false;
    }
    String readable =
        time_get_string(timestamp);
    int pl_pos =
        message.indexOf("\"pl\":");
    if(pl_pos < 0)
    {
        return false;
    }
    int insert_pos =
        pl_pos + 5;
    while(
        insert_pos < message.length() &&
        message[insert_pos] == ' '
    )
    {
        insert_pos++;
    }
    if(
        insert_pos >= message.length()
    )
    {
        return false;
    }
    if(
        message[insert_pos] != '{'
    )
    {
        return false;
    }
    int payload_end_check =
        insert_pos + 1;
    if(
        payload_end_check >= message.length()
    )
    {
        return false;
    }
    String payload_head =
        message.substring(
            insert_pos,
            min(
                insert_pos + 64,
                (int)message.length()
            )
        );
    if(
        payload_head.indexOf(
            "\"time\""
        ) >= 0
    )
    {
        return false;
    }
    if(
        message[insert_pos + 1] == '}'
    )
    {
        String add =
            "\"time\":\"" +
            readable +
            "\"";
        message =
            message.substring(
                0,
                insert_pos + 1
            )
            +
            add
            +
            message.substring(
                insert_pos + 1
            );
        return true;
    }
    String add =
        "\"time\":\"" +
        readable +
        "\",";
    message =
        message.substring(
            0,
            insert_pos + 1
        )
        +
        add
        +
        message.substring(
            insert_pos + 1
        );
    return true;
}

// 这个函数发送/set 消息，推送给订阅设备，且发送消息的设备不会收到本条消息
bool cloud_send_set(
    const char* message
)
{
    if(message==nullptr)
    {
        Serial.println("[Cloud SET] null");
        return false;
    }
    if(!mqtt_connected)
    {
        Serial.println("[Cloud SET] MQTT offline");
        return false;
    }
    String upload =
        String(message);
    cloud_add_readable_time(
        upload
    );
    size_t len =
        upload.length();
    if(
        len >= mqttClient.getBufferSize()-32
    )
    {
        Serial.println("[Cloud SET] message too long");
        return false;
    }
    String topic =
        mqtt_sub_topic + "/set";
    bool result =
        mqttClient.publish(
            topic.c_str(),
            upload.c_str()
        );
    Serial.println(
        result?
        "[Cloud SET] OK":
        "[Cloud SET] FAIL"
    );
    if(result)
    {
        Serial.println(upload);
    }
    return result;
}


///up = 只更新云端数据，不广播
bool cloud_send_up(
    const char* message
)
{
    if(message==nullptr)
    {
        Serial.println("[Cloud UP] null");
        return false;
    }
    if(!mqtt_connected)
    {
        Serial.println("[Cloud UP] MQTT offline");
        return false;
    }
    // ===============================
    // 1. 保持原JSON生成逻辑
    // ===============================
    String upload =
        String(message);
    cloud_add_readable_time(
        upload
    );
    Serial.println("[Cloud UP JSON]");
    Serial.println(upload);
    // ===============================
    // 2. JSON -> CBOR
    // ===============================
    JsonDocument doc;
    DeserializationError error =
        deserializeJson(
            doc,
            upload
        );
    if(error)
    {
        Serial.println(
            "[Cloud UP] JSON parse failed"
        );
        return false;
    }
    uint8_t buffer[512];
    size_t cbor_len =
        serializeMsgPack(
            doc,
            buffer,
            sizeof(buffer)
        );
    if(cbor_len==0)
    {
        Serial.println(
            "[Cloud UP] CBOR encode failed"
        );
        return false;
    }
    Serial.printf(
        "[Cloud UP] JSON=%d CBOR=%d\n",
        upload.length(),
        cbor_len
    );
    // ===============================
    // 3. MQTT binary publish
    // ===============================
    String topic =
        mqtt_sub_topic + "/up";
    bool result =
        mqttClient.publish(
            topic.c_str(),
            buffer,
            cbor_len
        );
    Serial.println(
        result?
        "[Cloud UP] OK":
        "[Cloud UP] FAIL"
    );
    return result;
}

// =====================================================
// 新增：直接传入JsonDocument序列化发布
// =====================================================
bool cloud_upload_json(JsonDocument& doc)
{
    String buf;
    serializeJson(doc, buf);
    return cloud_send_up(buf.c_str());
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

//cmd id 过滤，防风暴
static bool cloud_check_duplicate_cmd(
    const String &id
)
{
    if(id.length()==0)
        return false;

    unsigned long now = millis();
    for(
        int i=0;
        i<MQTT_DUP_CACHE_SIZE;
        i++
    )
    {
        if(
            cmd_id_cache[i].cmd_id == id
            &&
            now - cmd_id_cache[i].received_ms < MQTT_DUP_CACHE_TTL_MS
        )
        {
            Serial.println(
              "[Cloud] duplicate command"
            );
            return true;
        }
    }

    // FIFO 写入最新 cmd_id（覆盖最旧条目）
    cmd_id_cache[cmd_cache_index].cmd_id = id;
    cmd_id_cache[cmd_cache_index].received_ms = now;
    cmd_cache_index++;
    if(
        cmd_cache_index >=
        MQTT_DUP_CACHE_SIZE
    )
    {
        cmd_cache_index = 0;
    }
    return false;
}
