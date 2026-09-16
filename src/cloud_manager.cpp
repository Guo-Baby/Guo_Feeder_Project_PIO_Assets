#include <Arduino.h>
#include <mqtt_client.h>
#include <esp_event.h>
#include <WiFi.h>
#include <ArduinoJson.h>
#include <LittleFS.h>

#include "cloud_manager.h"
#include "system_state.h"
#include "config_manager.h"
#include "event_manager.h"
#include "time_manager.h"
#include "command_manager.h"
#include "capability_registry.h"
#include "log_events.h"   // P1.4/P1.5：Log Topic 与 log_ack 常量（冻结契约）
// =====================================================
// MQTT QoS 测试开关
//
// 0: QoS0，不进入 outbox，不等待 PUBACK
// 1: QoS1，需要 PUBACK确认
// =====================================================
#define CLOUD_MQTT_QOS 1
#define CLOUD_MQTT_STORE true

// =====================================================
// MQTT 常量
// =====================================================
#define MQTT_RETRY_MAX             10
#define MQTT_FAIL_SLEEP_TIME       3600000UL
#define MQTT_DUP_CACHE_SIZE        10
#define MQTT_DUP_CACHE_TTL_MS      30000UL

// MQTT 收发缓冲
#define CLOUD_MQTT_BUFFER_SIZE     8192
#define CLOUD_MSG_LIMIT_MIN        64
#define CLOUD_MSG_LIMIT_MAX        (CLOUD_MQTT_BUFFER_SIZE - 64)

// 分片消息类型（写入 BEGIN 的 t 字段）
#define CLOUD_FRAG_UP              0
#define CLOUD_FRAG_SET             1

// =====================================================
// MQTT 对象（ESP-IDF 原生 esp-mqtt，QoS1）
// =====================================================
static esp_mqtt_client_handle_t mqtt_client = nullptr;
static bool mqtt_client_active = false;
static bool mqtt_connected = false;
static bool wifi_connected = false;

// MQTT 事件（回调线程）-> 主循环（cloud_task）同步标记
static bool mqtt_connect_pending = false;
static bool mqtt_disconnect_pending = false;

// =====================================================
// MQTT 参数（从 config 读取）
// =====================================================
static String mqtt_server;
static int mqtt_port;
static String mqtt_client_id;

static String mqtt_username;
static String mqtt_password;

static String mqtt_sub_topic;
static String mqtt_pub_topic;
// P1.4：LogManager 专用上行 Topic（独立于 up / down）
static String mqtt_log_topic;
// P1.5：log_ack 回调（由 LogManager 注入，CloudManager 不反向依赖它）
static CloudLogAckCallback s_log_ack_cb = nullptr;

static String mqtt_ca_file;

static unsigned long retry_interval;
static int retry_max;
static unsigned long sleep_retry_interval;
static int keep_alive;

// =====================================================
// MQTT 重连控制
// =====================================================
static unsigned long mqtt_retry_timer = 0;
static uint8_t mqtt_retry_count = 0;
static bool mqtt_sleep_mode = false;

// =====================================================
// 防重复缓存
// =====================================================
struct CmdIdCacheEntry
{
    String cmd_id;
    unsigned long received_ms;
};

static CmdIdCacheEntry cmd_id_cache[MQTT_DUP_CACHE_SIZE];
static uint8_t cmd_cache_index = 0;

// =====================================================
// 云端接收缓存（原 cloud_receive 兼容）
// =====================================================
static String last_command;

// =====================================================
// 二进制 / 分片缓冲
// =====================================================
static uint8_t cloud_bin_buffer[CLOUD_MQTT_BUFFER_SIZE];
static uint8_t cloud_frag_packet[CLOUD_MQTT_BUFFER_SIZE];

// 当前分片上限（RAM 保存，可由 change_msg_limit 修改）
static size_t cloud_msg_limit = CLOUD_DEFAULT_MSG_LIMIT;

// =====================================================
// 下行接收环形缓冲（MQTT 回调只入队，cloud_task 主循环处理）
// =====================================================
#define MQTT_RX_SLOT_COUNT 4
#define MQTT_RX_SLOT_SIZE  8192

static uint8_t  rx_buffer[MQTT_RX_SLOT_COUNT][MQTT_RX_SLOT_SIZE];
static uint16_t rx_len[MQTT_RX_SLOT_COUNT];
static uint8_t  rx_head = 0;   // 下一个待读取
static uint8_t  rx_tail = 0;   // 下一个写入
static uint8_t  rx_count = 0;
// MQTT 底层长消息重组缓存
static uint8_t mqtt_large_rx_buffer[CLOUD_MQTT_BUFFER_SIZE];

static size_t mqtt_large_rx_total = 0;
static size_t mqtt_large_rx_offset = 0;
static bool mqtt_large_rx_active = false;

// =====================================================
// 前置声明
// =====================================================
static void mqtt_event_handler(
    void* handler_args,
    esp_event_base_t base,
    int32_t event_id,
    void* event_data);
static void cloud_process_rx_message(const uint8_t* data, size_t len);
static void cloud_process_rx_queue();
static void cloud_process_mqtt_events();
static bool cloud_connect();
static bool cloud_publish_fragmented(
    const String& topic_suffix,
    const uint8_t* data,
    size_t len,
    uint8_t msg_type);
static bool cloud_compress_uplink(const String& in, String& out);
static void on_command_result(const String& json);
static bool cloud_check_duplicate_cmd(const String& id);
static void cloud_send_ack(const String& cmd_id, const String& object);
static void cloud_send_protocol_error(
    const String& cmd_id,
    int status,
    int error,
    const char* msg);
static void cloud_handle_change_msg_limit(
    const String& cmd_id,
    JsonVariant payload);
static bool cloud_add_readable_time(String& message);

// =====================================================
// CRC32（标准 IEEE 802.3，用于分片完整性校验）
// =====================================================
static uint32_t cloud_crc32(const uint8_t* data, size_t len)
{
    uint32_t crc = 0xFFFFFFFFUL;
    for(size_t i = 0; i < len; i++)
    {
        crc ^= data[i];
        for(uint8_t b = 0; b < 8; b++)
        {
            crc = (crc & 1UL) ? ((crc >> 1) ^ 0xEDB88320UL) : (crc >> 1);
        }
    }
    return crc ^ 0xFFFFFFFFUL;
}

// =====================================================
// 下行接收环形缓冲
// =====================================================
static void cloud_rx_enqueue(const uint8_t* data, size_t len)
{
    if(data == nullptr || len == 0 || len > MQTT_RX_SLOT_SIZE)
    {
        Serial.printf("[Cloud] RX drop: len=%u\n", (unsigned)len);
        return;
    }
    if(rx_count >= MQTT_RX_SLOT_COUNT)
    {
        Serial.println("[Cloud] RX queue full, message dropped");
        return;
    }
    memcpy(rx_buffer[rx_tail], data, len);
    rx_len[rx_tail] = (uint16_t)len;
    rx_tail = (uint8_t)((rx_tail + 1) % MQTT_RX_SLOT_COUNT);
    rx_count++;
}

static bool cloud_rx_dequeue(uint8_t* out, size_t& out_len, size_t cap)
{
    if(rx_count == 0)
    {
        return false;
    }
    size_t len = rx_len[rx_head];
    if(len > cap)
    {
        len = cap;
    }
    memcpy(out, rx_buffer[rx_head], len);
    out_len = len;
    rx_head = (uint8_t)((rx_head + 1) % MQTT_RX_SLOT_COUNT);
    rx_count--;
    return true;
}

// =====================================================
// MQTT 事件回调（运行在 esp-mqtt 任务线程）
//
// 只做轻量操作：
//   - 置连接状态标记
//   - 收到消息仅拷入环形缓冲
// 解析 / ACK / 执行命令全部放到 cloud_task() 主循环。
// =====================================================
static void mqtt_event_handler(
    void* handler_args,
    esp_event_base_t base,
    int32_t event_id,
    void* event_data)
{
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;
    switch((esp_mqtt_event_id_t)event_id)
    {
        case MQTT_EVENT_CONNECTED:
            Serial.println("[Cloud] MQTT connected");
            mqtt_connected = true;
            mqtt_connect_pending = true;
            {
                int msg_id = esp_mqtt_client_subscribe(
                    mqtt_client,
                    mqtt_sub_topic.c_str(),
                    CLOUD_MQTT_QOS);   // subscribe QoS1 or 0
                Serial.printf(
                    "[Cloud] MQTT subscribe QoS=%d msg_id=%d\n",
                    CLOUD_MQTT_QOS,
                    msg_id);
            }
            Serial.printf(
                "[Cloud] MQTT connected outbox=%d\n",
                esp_mqtt_client_get_outbox_size(mqtt_client));
            break;

        case MQTT_EVENT_SUBSCRIBED:
            Serial.printf(
                "[Cloud] MQTT subscribed msg_id=%d\n",
                event->msg_id);
            break;

        case MQTT_EVENT_DISCONNECTED:
            Serial.printf(
                "[Cloud] MQTT disconnected outbox=%d\n",
                esp_mqtt_client_get_outbox_size(mqtt_client));
            mqtt_connected = false;
            mqtt_disconnect_pending = true;
            break;

        case MQTT_EVENT_PUBLISHED:
            Serial.printf(
                "[Cloud] MQTT published/ack msg_id=%d outbox=%d t=%lu\n",
                event->msg_id,
                esp_mqtt_client_get_outbox_size(mqtt_client),
                (unsigned long)(millis() & 0xFFFFFFFFUL));
            break;

        case MQTT_EVENT_DATA:
            if(event->data != nullptr && event->data_len > 0)
            {
                // 第一次收到长消息
                if(event->current_data_offset == 0)
                {
                    if(event->total_data_len > CLOUD_MQTT_BUFFER_SIZE)
                    {
                        Serial.printf(
                            "[Cloud] MQTT message too large: %d\n",
                            event->total_data_len);
                        mqtt_large_rx_active = false;
                        break;
                    }
                    mqtt_large_rx_total = event->total_data_len;
                    mqtt_large_rx_offset = 0;
                    mqtt_large_rx_active = true;
                }
                if(!mqtt_large_rx_active)
                {
                    break;
                }
                memcpy(
                    mqtt_large_rx_buffer + event->current_data_offset,
                    event->data,
                    event->data_len);
                mqtt_large_rx_offset += event->data_len;
                // 接收完成
                if(mqtt_large_rx_offset >= mqtt_large_rx_total)
                {
                    cloud_rx_enqueue(
                        mqtt_large_rx_buffer,
                        mqtt_large_rx_total);
                    mqtt_large_rx_active = false;
                    mqtt_large_rx_total = 0;
                    mqtt_large_rx_offset = 0;
                }
            }
            break;

        case MQTT_EVENT_ERROR:
            Serial.println("[Cloud] MQTT error event");
            break;

        default:
            break;
    }
}

// =====================================================
// 底层发布（QoS1，非阻塞，失败自动重发直到 PUBACK）
// =====================================================
static bool cloud_mqtt_publish_binary(
    const String& topic,
    const uint8_t* data,
    size_t len,
    int qos)
{
    if(mqtt_client == nullptr || !mqtt_connected)
    {
        return false;
    }
    int msg_id;
    int outbox_before = esp_mqtt_client_get_outbox_size(mqtt_client);

    if(CLOUD_MQTT_QOS == 0)
    {
        msg_id = esp_mqtt_client_publish(
            mqtt_client,
            topic.c_str(),
            (const char*)data,
            len,
            0,
            0);
    }
    else
    {
        msg_id = esp_mqtt_client_enqueue(
            mqtt_client,
            topic.c_str(),
            (const char*)data,
            len,
            CLOUD_MQTT_QOS,
            0,
            CLOUD_MQTT_STORE);
    }
    if(msg_id < 0)
    {
        Serial.printf(
            "[Cloud] publish FAIL %s len=%u\n",
            topic.c_str(),
            (unsigned)len);
        return false;
    }
    Serial.printf(
        "[Cloud] ENQ id=%d qos=%d store=%d outbox=%d->%d len=%u t=%lu\n",
        msg_id,
        CLOUD_MQTT_QOS,
        (int)CLOUD_MQTT_STORE,
        outbox_before,
        esp_mqtt_client_get_outbox_size(mqtt_client),
        (unsigned)len,
        (unsigned long)(millis() & 0xFFFFFFFFUL));
    return true;
}

static bool cloud_mqtt_publish_text(
    const String& topic,
    const String& text,
    int qos)
{
    if(mqtt_client == nullptr || !mqtt_connected)
    {
        return false;
    }
    int msg_id;

    if(CLOUD_MQTT_QOS == 0)
    {
        msg_id = esp_mqtt_client_publish(
            mqtt_client,
            topic.c_str(),
            text.c_str(),
            text.length(),
            0,
            0);
    }
    else
    {
        msg_id = esp_mqtt_client_enqueue(
            mqtt_client,
            topic.c_str(),
            text.c_str(),
            0,
            CLOUD_MQTT_QOS,
            0,
            CLOUD_MQTT_STORE);
    }

    if(msg_id < 0)
    {
        Serial.printf(
            "[Cloud] publish FAIL qos=%d store=%d topic=%s len=%u\n",
            CLOUD_MQTT_QOS,
            CLOUD_MQTT_STORE,
            topic.c_str(),
            (unsigned)text.length());
        return false;
    }

    Serial.printf(
        "[Cloud] enqueue OK id=%d qos=%d store=%d len=%u\n",
        msg_id,
        CLOUD_MQTT_QOS,
        CLOUD_MQTT_STORE,
        (unsigned)text.length());
    return true;
}

// =====================================================
// 发送前长度判断 + 分片
//
// BEGIN: {"f":0,"i":"msg001","n":5,"l":<总长度>,"t":<类型>,"c":<crc32>}
// DATA : binary payload = [fragment index][data]
// END  : {"f":2,"i":"msg001"}
// =====================================================
static bool cloud_publish_fragmented(
    const String& topic_suffix,
    const uint8_t* data,
    size_t len,
    uint8_t msg_type)
{
    String topic;
    if(msg_type == CLOUD_FRAG_UP)
    {
        topic = mqtt_pub_topic;
    }
    else
    {
        topic = mqtt_sub_topic;
    }

    // 未超限：直接发送
    if(len <= cloud_msg_limit)
    {
        return cloud_mqtt_publish_binary(topic, data, len, 1);
    }

    // 超限：分片
    size_t chunk = cloud_msg_limit - 2;   // 2 字节留给 fragment index
    uint16_t n = (uint16_t)((len + chunk - 1) / chunk);
    String frag_id = "m";
    frag_id += String(millis());
    uint32_t crc = cloud_crc32(data, len);

    Serial.printf(
        "[Cloud] fragment len=%u n=%u limit=%u type=%u\n",
        (unsigned)len,
        n,
        (unsigned)cloud_msg_limit,
        msg_type);

    // BEGIN
    JsonDocument begin;
    begin["f"] = FRAG_BEGIN;
    begin["i"] = frag_id;
    begin["n"] = n;
    begin["l"] = (unsigned long)len;
    begin["t"] = msg_type;
    begin["c"] = (unsigned long)crc;
    String begin_json;
    serializeJson(begin, begin_json);
    cloud_mqtt_publish_text(topic, begin_json, 1);

    // DATA
    for(uint16_t idx = 0; idx < n; idx++)
    {
        size_t off = (size_t)idx * chunk;
        size_t clen = min(chunk, len - off);
        // 前2字节保存 fragment index
        cloud_frag_packet[0] = idx & 0xff;
        cloud_frag_packet[1] = (idx >> 8) & 0xff;
        memcpy(
            cloud_frag_packet + 2,
            data + off,
            clen);
        cloud_mqtt_publish_binary(
            topic,
            cloud_frag_packet,
            2 + clen,
            1);
    }

    // END
    JsonDocument end;
    end["f"] = FRAG_END;
    end["i"] = frag_id;
    String end_json;
    serializeJson(end, end_json);
    cloud_mqtt_publish_text(topic, end_json, 1);

    return true;
}

// =====================================================
// 上行压缩：长字段 -> 缩写字段
//
// status->s  command->c  object->o  id->i  payload->p
// timestamp->t  error->e  message->m  version->v  stable id->k
//
// registry 查询结果特化: {s,r,v,n,cs,d}
// 其余未定义字段原样保留（嵌套数组等）。
// =====================================================
static bool cloud_compress_uplink(const String& in, String& out)
{
    JsonDocument doc;
    if(deserializeJson(doc, in))
    {
        return false;
    }
    JsonDocument comp;

    // ---- registry 查询结果特化 ----
    if(doc["registry"].is<const char*>())
    {
        const char* reg = doc["registry"];
        int r = REGISTRY_ACTION;
        if(strcmp(reg, "trigger") == 0)
        {
            r = REGISTRY_TRIGGER;
        }
        else if(strcmp(reg, "workflow") == 0)
        {
            r = REGISTRY_WORKFLOW;
        }

        comp["c"] = doc["cmd"] | "result";
        if(!doc["id"].isNull())
        {
            comp["i"] = doc["id"];
        }
        else if(!doc["command_id"].isNull())
        {
            comp["i"] = doc["command_id"];
        }
        comp["s"] = STATUS_OK;
        comp["r"] = r;
        if(!doc["version"].isNull())
        {
            comp["v"] = doc["version"];
        }
        if(!doc["count"].isNull())
        {
            comp["n"] = doc["count"];
        }
        if(!doc["checksum"].isNull())
        {
            comp["cs"] = doc["checksum"];
        }
        JsonArray d = comp["d"].to<JsonArray>();
        JsonArrayConst entries = doc["entries"].as<JsonArrayConst>();
        for(JsonObjectConst e : entries)
        {
            JsonArray pair = d.add<JsonArray>();
            pair.add(e["stable_id"]);
            pair.add(e["runtime_id"]);
        }
        serializeJson(comp, out);
        return true;
    }

    // ---- 通用字段缩写 ----
    if(!doc["status"].isNull())
    {
        if(doc["status"].is<int>())
        {
            comp["s"] = doc["status"].as<int>();
        }
        else if(doc["status"].is<const char*>())
        {
            const char* st = doc["status"];
            int s = -1;
            if(strcmp(st, "success") == 0)
            {
                s = STATUS_OK;
            }
            else if(strcmp(st, "accepted") == 0)
            {
                s = STATUS_ACCEPTED;
            }
            else if(strcmp(st, "running") == 0)
            {
                s = STATUS_RUNNING;
            }
            else if(strcmp(st, "failed") == 0)
            {
                s = STATUS_FAILED;
            }
            else if(strcmp(st, "error") == 0)
            {
                s = STATUS_ERROR;
            }
            else if(strcmp(st, "timeout") == 0)
            {
                s = STATUS_FAILED;
            }
            else if(strcmp(st, "rebooting") == 0)
            {
                s = STATUS_RUNNING;
            }
            if(s >= 0)
            {
                comp["s"] = s;
            }
            else
            {
                comp["s"] = st;
            }
        }
        else
        {
            comp["s"] = doc["status"];
        }
    }
    if(!doc["cmd"].isNull())
    {
        comp["c"] = doc["cmd"];
    }
    if(!doc["id"].isNull())
    {
        comp["i"] = doc["id"];
    }
    else if(!doc["command_id"].isNull())
    {
        comp["i"] = doc["command_id"];
    }
    if(!doc["object"].isNull())
    {
        comp["o"] = doc["object"];
    }
    else if(!doc["action"].isNull())
    {
        comp["o"] = doc["action"];
    }
    else if(!doc["workflow"].isNull())
    {
        comp["o"] = doc["workflow"];
    }
    else if(!doc["key"].isNull())
    {
        comp["o"] = doc["key"];
    }
    else if(!doc["ob"].isNull())
    {
        comp["o"] = doc["ob"];
    }
    if(!doc["timestamp"].isNull())
    {
        comp["t"] = doc["timestamp"];
    }
    if(!doc["payload"].isNull())
    {
        comp["p"] = doc["payload"];
    }
    else if(!doc["pl"].isNull())
    {
        comp["p"] = doc["pl"];
    }
    if(!doc["message"].isNull())
    {
        comp["m"] = doc["message"];
    }
    if(!doc["error_code"].isNull())
    {
        comp["e"] = doc["error_code"];
    }
    else if(!doc["error"].isNull())
    {
        comp["e"] = doc["error"];
    }
    if(!doc["version"].isNull())
    {
        comp["v"] = doc["version"];
    }
    if(!doc["stable_id"].isNull())
    {
        comp["k"] = doc["stable_id"];
    }

    // 其余字段原样保留
    JsonObjectConst src = doc.as<JsonObjectConst>();
    for(JsonPairConst kv : src)
    {
        const char* key = kv.key().c_str();
        if(strcmp(key, "status") == 0 ||
           strcmp(key, "cmd") == 0 ||
           strcmp(key, "id") == 0 ||
           strcmp(key, "command_id") == 0 ||
           strcmp(key, "object") == 0 ||
           strcmp(key, "action") == 0 ||
           strcmp(key, "workflow") == 0 ||
           strcmp(key, "key") == 0 ||
           strcmp(key, "ob") == 0 ||
           strcmp(key, "timestamp") == 0 ||
           strcmp(key, "payload") == 0 ||
           strcmp(key, "pl") == 0 ||
           strcmp(key, "message") == 0 ||
           strcmp(key, "error_code") == 0 ||
           strcmp(key, "error") == 0 ||
           strcmp(key, "version") == 0 ||
           strcmp(key, "stable_id") == 0)
        {
            continue;
        }
        comp[key] = kv.value();
    }

    serializeJson(comp, out);
    return true;
}

// =====================================================
// 辅助：JSON 变体 -> long（兼容数字 / 字符串）
// =====================================================
static long cloud_variant_to_long(JsonVariant v, long def)
{
    if(v.is<int>())
    {
        return v.as<int>();
    }
    if(v.is<const char*>())
    {
        String s(v.as<const char*>());
        s.trim();
        if(s.length() > 0)
        {
            return s.toInt();
        }
    }
    return def;
}

// =====================================================
// 协议错误响应（不经过 CommandManager）
// =====================================================
static void cloud_send_protocol_error(
    const String& cmd_id,
    int status,
    int error,
    const char* msg)
{
    JsonDocument doc;
    doc["c"] = "result";
    doc["i"] = cmd_id;
    doc["s"] = status;
    doc["e"] = error;
    if(msg != nullptr)
    {
        doc["m"] = msg;
    }
    time_t ts = time_get();
    if(ts > 0)
    {
        doc["t"] = ts;
    }
    String out;
    serializeJson(doc, out);
    cloud_send_up(out.c_str());
}

// =====================================================
// ACK（压缩格式）
// =====================================================
static void cloud_send_ack(const String& cmd_id, const String& object)
{
    JsonDocument ack;
    ack["c"] = "ack";
    ack["i"] = cmd_id;
    if(object.length() > 0)
    {
        ack["o"] = object;
    }
    JsonObject pl = ack["p"].to<JsonObject>();
    pl["result"] = "received";
    ack["t"] = time_get();
    String ack_string;
    serializeJson(ack, ack_string);
    Serial.printf(
        "[Cloud] UP-ACK id=%s obj=%s t=%lu\n",
        cmd_id.c_str(),
        object.c_str(),
        (unsigned long)(millis() & 0xFFFFFFFFUL));
    cloud_send_up(ack_string.c_str());
}

// =====================================================
// change_msg_limit：协议级配置，不进入 CommandManager
// 结果通过 on_command_result 回调上报（成功 / 失败）
// =====================================================
static void cloud_handle_change_msg_limit(
    const String& cmd_id,
    JsonVariant payload)
{
    long limit = -1;
    if(payload.is<int>())
    {
        limit = payload.as<int>();
    }
    else if(payload.is<const char*>())
    {
        String s(payload.as<const char*>());
        int pos = s.indexOf(':');
        if(pos < 0)
        {
            pos = s.indexOf("：");   // 兼容全角冒号
        }
        if(pos >= 0)
        {
            String num = s.substring(pos + 1);
            num.trim();
            limit = num.toInt();
        }
    }
    else if(payload.is<JsonObject>())
    {
        if(!payload["lengh"].isNull())
        {
            limit = cloud_variant_to_long(payload["lengh"], -1);
        }
        else if(!payload["length"].isNull())
        {
            limit = cloud_variant_to_long(payload["length"], -1);
        }
    }

    if(limit < CLOUD_MSG_LIMIT_MIN)
    {
        JsonDocument fail;
        fail["c"] = "result";
        fail["i"] = cmd_id;
        fail["s"] = STATUS_ERROR;
        fail["e"] = ERROR_INVALID_COMMAND;
        fail["m"] = "invalid msg limit";
        time_t ts = time_get();
        if(ts > 0)
        {
            fail["t"] = ts;
        }
        String out;
        serializeJson(fail, out);
        on_command_result(out);
        return;
    }

    if(limit > CLOUD_MSG_LIMIT_MAX)
    {
        limit = CLOUD_MSG_LIMIT_MAX;
    }
    cloud_msg_limit = (size_t)limit;
    Serial.printf("[Cloud] msg limit -> %u\n", (unsigned)cloud_msg_limit);

    JsonDocument ok;
    ok["c"] = "result";
    ok["i"] = cmd_id;
    ok["s"] = STATUS_OK;
    ok["m"] = "msg_limit=" + String((long)cloud_msg_limit);
    ok["l"] = (long)cloud_msg_limit;
    time_t ts = time_get();
    if(ts > 0)
    {
        ok["t"] = ts;
    }
    String out;
    serializeJson(ok, out);
    on_command_result(out);
}

// =====================================================
// 下行命令翻译：压缩 JSON / 旧 JSON -> CommandMessage
//
// 新格式: {c,i,v,k,p}
//   action/workflow : v=registry version, k=stable id -> runtime id
//   registry        : k=RegistryType -> query_*_registry
//   system          : p.o = object（如 reboot）
//   query           : p.q = 命令, p.o = object
//   其他            : c 直接作为 CommandManager 命令名透传
// =====================================================
static bool cloud_translate_command(
    JsonDocument& doc,
    bool compact,
    CommandMessage& cmd,
    int& error_code,
    String& error_msg)
{
    const char* c = compact ? doc["c"] : doc["cmd"];
    if(c == nullptr)
    {
        error_code = ERROR_INVALID_COMMAND;
        error_msg = "Missing command";
        return false;
    }
    cmd.cmd_id = compact ? (doc["i"] | "") : (doc["id"] | "");
    cmd.source = "cloud";
    cmd.timestamp = compact ? (doc["t"] | 0UL) : (doc["ts"] | 0UL);

    JsonVariant pv = compact ? doc["p"] : doc["pl"];
    if(!pv.isNull())
    {
        serializeJson(pv, cmd.payload);
    }

    // ---- 旧格式：命令/对象原样透传 ----
    if(!compact)
    {
        cmd.command = c;
        cmd.object = doc["ob"] | "";
        return true;
    }

    // ---- 新格式 ----
    String sc = String(c);
    if(sc == "action" || sc == "execute_action")
    {
        long v = cloud_variant_to_long(doc["v"], -1);
        if(v != (long)capability_get_action_version())
        {
            error_code = ERROR_VERSION_MISMATCH;
            error_msg = "action registry version mismatch";
            return false;
        }
        long k = cloud_variant_to_long(doc["k"], -1);
        String runtime_id;
        if(k < 0 || k > 255 ||
           !capability_get_action_by_stable_id((uint8_t)k, runtime_id))
        {
            error_code = ERROR_INVALID_OBJECT;
            error_msg = "action stable id not found";
            return false;
        }
        cmd.command = "execute_action";
        cmd.object = runtime_id;
    }
    else if(sc == "workflow" || sc == "execute_workflow")
    {
        long v = cloud_variant_to_long(doc["v"], -1);
        if(v != (long)capability_get_workflow_version())
        {
            error_code = ERROR_VERSION_MISMATCH;
            error_msg = "workflow registry version mismatch";
            return false;
        }
        long k = cloud_variant_to_long(doc["k"], -1);
        String runtime_id;
        if(k < 0 || k > 255 ||
           !capability_get_workflow_by_stable_id((uint8_t)k, runtime_id))
        {
            error_code = ERROR_INVALID_OBJECT;
            error_msg = "workflow stable id not found";
            return false;
        }
        cmd.command = "execute_workflow";
        cmd.object = runtime_id;
    }
    else if(sc == "registry")
    {
        long k = cloud_variant_to_long(doc["k"], -1);
        if(k == REGISTRY_ACTION)
        {
            cmd.command = "query_action_registry";
        }
        else if(k == REGISTRY_TRIGGER)
        {
            cmd.command = "query_trigger_registry";
        }
        else if(k == REGISTRY_WORKFLOW)
        {
            cmd.command = "query_workflow_registry";
        }
        else
        {
            error_code = ERROR_INVALID_OBJECT;
            error_msg = "invalid registry type";
            return false;
        }
        cmd.object = "";
    }
    else if(sc == "system")
    {
        cmd.command = "system";
        cmd.object = doc["p"]["o"] | doc["p"]["object"] | "";
    }
    else if(sc == "query")
    {
        cmd.command = doc["p"]["q"] | "";
        cmd.object = doc["p"]["o"] | doc["p"]["object"] | "";
    }
    else
    {
        // 透传：c 已是 CommandManager 现有命令名
        cmd.command = sc;
        cmd.object = doc["p"]["o"] | doc["p"]["object"] | doc["ob"] | "";
    }

    if(cmd.command.length() == 0)
    {
        error_code = ERROR_INVALID_COMMAND;
        error_msg = "invalid command";
        return false;
    }
    return true;
}

// =====================================================
// 接收消息处理（主循环上下文）
// =====================================================
static void cloud_process_rx_message(const uint8_t* data, size_t len)
{
    String message;
    message.reserve(len + 1);
    for(size_t i = 0; i < len; i++)
    {
        message += (char)data[i];
    }
    if(message.length() == 0)
    {
        Serial.println("[Cloud] Empty MQTT message");
        return;
    }
    Serial.println("=================");
    Serial.println("[Cloud] MQTT RX");
    Serial.println(message);

    DynamicJsonDocument doc(4096);
    if(deserializeJson(doc, message))
    {
        Serial.println("[Cloud] JSON parse failed");
        return;
    }

    bool compact = !doc["c"].isNull();
    const char* c = compact ? doc["c"] : doc["cmd"];
    if(c == nullptr)
    {
        Serial.println("[Cloud] Missing command");
        return;
    }
    // FIX-4：协议级 log_ack 必须绕过"命令 id 必需性"与"命令去重缓存"
    //
    // 理由（三条）：
    //   ① log_ack 是**自描述**的（p.b / p.f / p.t 已足够定位区间），不需要 i；
    //   ② MQTT QoS1 的重复投递复用**完全相同**的 payload ⇒ 同一个 i ⇒
    //      若走去重会被当作"重复命令"静默丢弃（设备丢 ACK ⇒ 无谓重传 ⇒
    //      放大 give-up 概率 ⇒ 与空洞压力联动）；
    //   ③ ACK 频率最高 1 条 / 500 ms（LOG_TX_MIN_INTERVAL_MS），而命令去重
    //      缓存只有 10 项 / 30 s TTL ⇒ 持续日志流量会在数秒内把缓存刷满，
    //      把**真实命令**的 id 挤出去 ⇒ 命令去重整体失效（同一条命令可能被
    //      执行两次）。这是比"丢一条 ACK"更严重的副作用。
    //
    // 重复 ACK 无需在此拦截：log_ack_classify() 会判为 DUPLICATE（幂等无副作用）。
    const bool is_log_ack = (strcmp(c, LOG_ACK_COMMAND) == 0);

    String cmd_id = compact ? (doc["i"] | "") : (doc["id"] | "");
    if(!is_log_ack && cmd_id.length() == 0)
    {
        Serial.println("[Cloud] Missing id");
        return;
    }
    if(!is_log_ack && cloud_check_duplicate_cmd(cmd_id))
    {
        Serial.println("[Cloud] Command duplicate");
        return;
    }

    // change_msg_limit：协议级配置，不进入 CommandManager
    if(strcmp(c, "change_msg_limit") == 0)
    {
        cloud_send_ack(cmd_id, "");
        cloud_handle_change_msg_limit(
            cmd_id,
            compact ? doc["p"] : doc["pl"]);
        return;
    }

    // P1.5：log_ack —— 协议级消息，同样旁路 CommandManager（§19）
    //   {"c":"log_ack","i":"<id>","p":{"b":<boot_seq>,"f":<seq_from>,"t":<seq_to>}}
    // 不回 ACK：Log 通道是 at-least-once，重复发送由云端幂等去重消化，
    // 再套一层 ACK 只会增加无意义的上下行流量。
    if(strcmp(c, LOG_ACK_COMMAND) == 0)
    {
        JsonVariant pl = compact ? doc["p"] : doc["pl"];
        const uint32_t boot_seq = pl[LOG_ACK_P_BOOT_SEQ] | 0u;
        const uint32_t seq_from = pl[LOG_ACK_P_SEQ_FROM] | 0u;
        const uint32_t seq_to   = pl[LOG_ACK_P_SEQ_TO]   | 0u;

        Serial.printf(
            "[Cloud LOG] ack rx boot=%u from=%u to=%u\n",
            (unsigned)boot_seq,
            (unsigned)seq_from,
            (unsigned)seq_to);

        if(s_log_ack_cb != nullptr)
        {
            s_log_ack_cb(boot_seq, seq_from, seq_to);
        }
        return;
    }

    CommandMessage cmd;
    int error_code = ERROR_INVALID_COMMAND;
    String error_msg = "invalid command";
    if(!cloud_translate_command(doc, compact, cmd, error_code, error_msg))
    {
        Serial.printf(
            "[Cloud] translate fail: %s\n",
            error_msg.c_str());
        cloud_send_protocol_error(
            cmd_id,
            STATUS_ERROR,
            error_code,
            error_msg.c_str());
        return;
    }

    cloud_send_ack(cmd.cmd_id, cmd.object);

    if(!command_manager_execute(cmd))
    {
        Serial.println(
            "[Cloud] Command execution failed (error result sent)");
    }
}

static uint8_t mqtt_rx_process_buffer[MQTT_RX_SLOT_SIZE];


static void cloud_process_rx_queue()
{
    size_t len = 0;

    if(cloud_rx_dequeue(
        mqtt_rx_process_buffer,
        len,
        sizeof(mqtt_rx_process_buffer)))
    {
        cloud_process_rx_message(
            mqtt_rx_process_buffer,
            len);
    }
}

// =====================================================
// WiFi 连接事件（主循环 context，由 event_dispatch 触发）
// =====================================================
static void cloud_event_callback(const EventMessage& message)
{
    switch(message.event)
    {
        case EVENT_WIFI_CONNECTED:
            Serial.println("[Cloud] WiFi connected event");
            wifi_connected = true;
            mqtt_sleep_mode = false;
            mqtt_retry_count = 0;
            mqtt_retry_timer = 0;
            break;

        case EVENT_WIFI_DISCONNECTED:
            Serial.println("[Cloud] WiFi disconnected event");
            wifi_connected = false;
            mqtt_connected = false;
            mqtt_connect_pending = false;
            mqtt_disconnect_pending = false;
            if(mqtt_client != nullptr)
            {
                esp_mqtt_client_stop(mqtt_client);
                mqtt_client_active = false;
            }
            state_set_bool(STATE_MQTT_STATUS, false);
            state_set_int(STATE_MQTT_LAST_ERROR, -1);
            event_push(
                EVENT_CLOUD_DISCONNECTED,
                "",
                "cloud",
                EVENT_PRIORITY_NORMAL,
                EVENT_POLICY_STATE);
            break;

        default:
            break;
    }
}

// =====================================================
// Command Manager 结果回调 -> 上行
// =====================================================
static void on_command_result(const String& json)
{
    Serial.printf(
        "[Cloud] UP-RESULT len=%u t=%lu\n",
        json.length(),
        (unsigned long)(millis() & 0xFFFFFFFFUL));
    cloud_send_up(json.c_str());
}

// =====================================================
// MQTT 建立连接（创建/启动 esp-mqtt client）
// =====================================================
static bool cloud_connect()
{
    if(!wifi_connected)
    {
        Serial.println("[Cloud] WiFi offline, skip MQTT");
        return false;
    }
    if(mqtt_client == nullptr)
    {
        Serial.println("[Cloud] MQTT connecting...");
        esp_mqtt_client_config_t cfg = {};
        cfg.host = mqtt_server.c_str();
        cfg.port = mqtt_port;
        cfg.client_id = mqtt_client_id.c_str();
        cfg.username = mqtt_username.c_str();
        cfg.password = mqtt_password.c_str();
        cfg.keepalive = keep_alive;
        cfg.buffer_size = CLOUD_MQTT_BUFFER_SIZE;
        cfg.disable_clean_session = 0;
        // MQTT TLS
        if(mqtt_port == 8883)
        {
            cfg.transport = MQTT_TRANSPORT_OVER_SSL;
        }
        else
        {
            cfg.transport = MQTT_TRANSPORT_OVER_TCP;
        }

        Serial.println("===== MQTT DEBUG =====");

        Serial.print("host=");
        Serial.println(cfg.host);

        Serial.print("port=");
        Serial.println(cfg.port);

        Serial.print("client_id=");
        Serial.println(cfg.client_id);

        Serial.print("username=");
        Serial.println(cfg.username);

        Serial.print("password=");
        Serial.println(cfg.password);

        Serial.print("buffer=");
        Serial.println(cfg.buffer_size);

        Serial.print("cert=");
        Serial.println(cfg.cert_pem ? "YES" : "NO");

        Serial.println("======================");
        
        static String ca_cert;
        File file = LittleFS.open(
            mqtt_ca_file,
            "r"
        );

        if(file)
        {
            ca_cert = file.readString();
            file.close();
            cfg.cert_pem = ca_cert.c_str();
            Serial.printf("[Cloud] CA length=%u\n", ca_cert.length());
            Serial.println(
                "[Cloud] CA certificate loaded");
        }
        else
        {
            Serial.println(
                "[Cloud] CA certificate missing");
        }

        mqtt_client = esp_mqtt_client_init(&cfg);

        if(mqtt_client == nullptr)
        {
            Serial.println("[Cloud] MQTT client init failed");
            Serial.println("Check MQTT config parameters");
            return false;
        }
        esp_mqtt_client_register_event(
            mqtt_client,
            MQTT_EVENT_ANY,
            mqtt_event_handler,
            nullptr);
    }

    esp_err_t err = esp_mqtt_client_start(mqtt_client);
    if(err != ESP_OK)
    {
        Serial.printf("[Cloud] MQTT start failed: %d\n", (int)err);
        mqtt_client_active = false;
        return false;
    }
    mqtt_client_active = true;
    Serial.println("[Cloud] MQTT client started");
    return true;
}

// =====================================================
// 初始化入口
// =====================================================
void cloud_init()
{
    Serial.println();
    Serial.println("Cloud manager init...");

    mqtt_server = config_get_mqtt_server();
    Serial.print("MQTT server=");
    Serial.println(mqtt_server);
    mqtt_port = config_get_mqtt_port();
    Serial.print("MQTT port=");
    Serial.println(mqtt_port);
    mqtt_client_id = config_get_mqtt_client_id();
    Serial.print("MQTT client_id=");
    Serial.println(mqtt_client_id);
    mqtt_username = config_get_mqtt_username();
    Serial.print("MQTT username=");
    Serial.println(mqtt_username);
    mqtt_password = config_get_mqtt_password();
    Serial.print("MQTT password=");
    Serial.println(mqtt_password);
    mqtt_sub_topic = config_get_mqtt_subscribe_topic();
    Serial.print("MQTT subscribe=");
    Serial.println(mqtt_sub_topic);
    mqtt_pub_topic = config_get_mqtt_publish_topic();
    Serial.print("MQTT publish=");
    Serial.println(mqtt_pub_topic);
    mqtt_log_topic = config_get_mqtt_log_topic();
    Serial.print("MQTT log=");
    Serial.println(mqtt_log_topic);
    mqtt_ca_file = config_get_mqtt_ca_path();
    Serial.print("MQTT CA=");
    Serial.println(mqtt_ca_file);

    Serial.println("MQTT Config:");

    
    Serial.printf(
        "[Cloud] QoS=%d store=%d buffer=%d msg_limit=%u\n",
        CLOUD_MQTT_QOS,
        (int)CLOUD_MQTT_STORE,
        CLOUD_MQTT_BUFFER_SIZE,
        (unsigned)CLOUD_DEFAULT_MSG_LIMIT);

    mqtt_client = nullptr;
    mqtt_client_active = false;
    mqtt_connected = false;
    mqtt_retry_count = 0;
    mqtt_retry_timer = 0;
    mqtt_sleep_mode = false;
    mqtt_connect_pending = false;
    mqtt_disconnect_pending = false;
    last_command.clear();
    cloud_msg_limit = CLOUD_DEFAULT_MSG_LIMIT;
    rx_head = 0;
    rx_tail = 0;
    rx_count = 0;

    // 注册 Command Manager 结果回调
    command_manager_set_result_callback(on_command_result);

    event_subscribe(EVENT_WIFI_CONNECTED, cloud_event_callback);
    event_subscribe(EVENT_WIFI_DISCONNECTED, cloud_event_callback);
}

// =====================================================
// MQTT 事件 -> 主循环同步（状态 / 事件 / 上线消息）
// =====================================================
static void cloud_process_mqtt_events()
{
    if(mqtt_connect_pending)
    {
        mqtt_connect_pending = false;
        mqtt_retry_count = 0;
        mqtt_sleep_mode = false;
        state_set_bool(STATE_MQTT_STATUS, true);
        state_set_int(STATE_MQTT_RETRY_COUNT, 0);
        state_set_string(STATE_MQTT_LAST_CONNECT_TIME, time_now_string());
        state_set_int(STATE_MQTT_LAST_ERROR, 0);
        event_push(
            EVENT_CLOUD_CONNECTED,
            "",
            "cloud",
            EVENT_PRIORITY_NORMAL,
            EVENT_POLICY_DEDUP);
        // 上线通知
        cloud_send_set(
            "{\"cmd\":\"system\",\"id\":\"online\",\"src\":\"device\"}");
        return;
    }
    if(mqtt_disconnect_pending)
    {
        mqtt_disconnect_pending = false;
        state_set_bool(STATE_MQTT_STATUS, false);
        state_set_int(STATE_MQTT_LAST_ERROR, -1);
        event_push(
            EVENT_CLOUD_DISCONNECTED,
            "",
            "cloud",
            EVENT_PRIORITY_NORMAL,
            EVENT_POLICY_STATE);
        mqtt_retry_count++;
        state_set_int(STATE_MQTT_RETRY_COUNT, mqtt_retry_count);
    }
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
            mqtt_connected = false;
            state_set_bool(STATE_MQTT_STATUS, false);
            state_set_int(STATE_MQTT_LAST_ERROR, -1);
            event_push(
                EVENT_CLOUD_DISCONNECTED,
                "",
                "cloud",
                EVENT_PRIORITY_NORMAL,
                EVENT_POLICY_STATE);
        }
        return;
    }

    // MQTT 连接/断开事件同步（状态 + 事件 + 上线消息）
    cloud_process_mqtt_events();

    if(mqtt_connected)
    {
        // 主循环处理下行消息
        cloud_process_rx_queue();
        return;
    }

    // ---- 未连接：重连 / 休眠控制 ----
    unsigned long now = millis();
    if(mqtt_sleep_mode)
    {
        unsigned long sleep_time =
            (sleep_retry_interval > 0) ?
            sleep_retry_interval :
            MQTT_FAIL_SLEEP_TIME;
        if(now - mqtt_retry_timer >= sleep_time)
        {
            mqtt_sleep_mode = false;
            mqtt_retry_count = 0;
            mqtt_retry_timer = now;
            cloud_connect();
        }
        return;
    }

    if(mqtt_client == nullptr || !mqtt_client_active)
    {
        cloud_connect();
        return;
    }

    // esp-mqtt 自动重连中；连续失败达到上限进入休眠
    int max_retries = (retry_max > 0) ? retry_max : MQTT_RETRY_MAX;
    if(mqtt_retry_count >= max_retries)
    {
        Serial.println("[Cloud] enter sleep retry");
        mqtt_sleep_mode = true;
        mqtt_retry_timer = now;
        esp_mqtt_client_stop(mqtt_client);
        mqtt_client_active = false;
        state_set_int(STATE_MQTT_RETRY_COUNT, mqtt_retry_count);
    }
}

// =====================================================
// 底层原始发送：/set（文本，超限分片）
// =====================================================
bool cloud_send_set(const char* message)
{
    if(message == nullptr)
        return false;
    if(!mqtt_connected)
        return false;
    String upload = String(message);
    Serial.println("[Cloud SET JSON]");
    Serial.println(upload);
    return cloud_mqtt_publish_text(
        mqtt_pub_topic,
        upload,
        1
    );
}

// =====================================================
// 底层原始发送：/up（JSON 
// =====================================================
bool cloud_send_up(const char* message)
{
    if(message == nullptr)
    {
        Serial.println("[Cloud UP] null");
        return false;
    }
    if(!mqtt_connected)
    {
        Serial.println("[Cloud UP] MQTT offline");
        return false;
    }
    String upload = String(message);
    String compact;
    if(cloud_compress_uplink(upload, compact))
    {
        upload = compact;
    }
    Serial.println("[Cloud UP JSON]");
    Serial.println(upload);
    bool result = cloud_mqtt_publish_text(
        mqtt_pub_topic,
        upload,
        1
    );
    Serial.println(
        result ?
        "[Cloud UP] OK":
        "[Cloud UP] FAIL"
    );
    return result;
}

// =====================================================
// P1.4：Log Topic 上行（二进制 CBOR，独立于 up / down）
//
// - 复用现有 cloud_mqtt_publish_binary()，不改动它
// - 离线时返回 false：保留记录 / 退避重试由 LogManager 负责
//   （CloudManager 不为 Log 建立任何 TX 队列）
// - 不做字段压缩、不分片：Batch 上界 LOG_BATCH_MAX_PAYLOAD = 4096 B
// =====================================================
bool cloud_send_log(const uint8_t* data, size_t length)
{
    if(data == nullptr || length == 0)
    {
        return false;
    }

    if(mqtt_log_topic.length() == 0)
    {
        mqtt_log_topic = "guo_feeder/log";   // 配置缺失时的兜底
    }

    if(mqtt_client == nullptr || !mqtt_connected)
    {
        Serial.println("[Cloud LOG] MQTT offline");
        return false;
    }

    const bool ok = cloud_mqtt_publish_binary(
        mqtt_log_topic,
        data,
        length,
        1);

    Serial.printf(
        "[Cloud LOG] %s topic=%s len=%u t=%lu\n",
        ok ? "OK" : "FAIL",
        mqtt_log_topic.c_str(),
        (unsigned)length,
        (unsigned long)(millis() & 0xFFFFFFFFUL));

    return ok;
}

// P1.5：由 LogManager 注入 ACK 处理回调（CloudManager 不反向依赖 LogManager）
void cloud_set_log_ack_callback(CloudLogAckCallback callback)
{
    s_log_ack_cb = callback;
}

// =====================================================
// 新增：直接传入 JsonDocument 序列化发布
// =====================================================
bool cloud_upload_json(JsonDocument& doc)
{
    String buf;
    serializeJson(doc, buf);
    return cloud_send_up(buf.c_str());
}

// =====================================================
// 获取最新云端指令（原 cloud_receive_get_command，兼容保留）
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
// =====================================================
// cmd id 过滤，防风暴
// =====================================================
static bool cloud_check_duplicate_cmd(const String& id)
{
    if(id.length() == 0)
    {
        return false;
    }
    unsigned long now = millis();
    for(int i = 0; i < MQTT_DUP_CACHE_SIZE; i++)
    {
        if(cmd_id_cache[i].cmd_id == id &&
           now - cmd_id_cache[i].received_ms < MQTT_DUP_CACHE_TTL_MS)
        {
            Serial.println("[Cloud] duplicate command");
            return true;
        }
    }
    // FIFO 写入最新 cmd_id（覆盖最旧条目）
    cmd_id_cache[cmd_cache_index].cmd_id = id;
    cmd_id_cache[cmd_cache_index].received_ms = now;
    cmd_cache_index++;
    if(cmd_cache_index >= MQTT_DUP_CACHE_SIZE)
    {
        cmd_cache_index = 0;
    }
    return false;
}

