// =====================================================
// P2-2 · MQTT Transport Facade 实现
//
// ★ 纯转发：本文件不含任何业务逻辑、不持状态、不接触 esp_mqtt_client。
//   所有能力由 CloudManager 的窄口 cloud_transport_*() 提供。
//
// 为什么 Facade 这么"薄"（而非自己持有 esp_mqtt_client / 队列）：
//   依赖方向被冻结为 mqtt_transport → cloud_manager（单向）。
//   若 Facade 直接持有 client 或让 cloud_manager 回调 Facade，
//   就会形成 cloud_manager ↔ mqtt_transport 循环依赖（违反 P2-0 §6.4）。
//   ⇒ 状态与队列留在 CloudManager 侧，本层只提供**稳定、窄、无 esp-mqtt 类型**的入口。
// =====================================================

#include "cloud/mqtt_transport.h"
#include "cloud/cloud_manager.h"

bool mqtt_transport_publish(const char* topic,
                            const char* payload,
                            size_t len,
                            int qos,
                            bool retain,
                            bool store)
{
    return cloud_transport_publish(topic, payload, len, qos, retain, store);
}

bool mqtt_transport_subscribe(const char* topic_filter, int qos)
{
    return cloud_transport_subscribe(topic_filter, qos);
}

void mqtt_transport_set_foreign_handler(
    void (*cb)(const char* topic, const uint8_t* data, size_t len, bool retain))
{
    cloud_transport_set_foreign_handler(cb);
}

bool mqtt_transport_is_connected()
{
    return cloud_is_connected();
}

void mqtt_transport_set_lwt(const char* topic,
                            const char* message,
                            int qos,
                            bool retain)
{
    cloud_transport_set_lwt(topic, message, qos, retain);
}
