#pragma once

// =====================================================
// P2-2 · MQTT Transport Facade（窄口）
//
// 依赖方向（P2-0 §6.4 冻结）：
//     homie/homie_bridge  →  cloud/mqtt_transport  →  cloud/cloud_manager  →  esp_mqtt_client
//
// 本头文件是 **Homie（及未来任何非业务消费者）唯一允许 include 的 cloud 头**。
// 它刻意**不暴露**任何 esp-mqtt / CloudManager 内部类型：
//   ✗ esp_mqtt_client_handle_t / esp_mqtt_client_config_t / esp_mqtt_event_t
//   ✗ CloudManager 内部结构体、业务 API（cloud_send_up / cloud_send_set ...）
//   ✗ Homie 类型
// 只暴露"发布 / 订阅 / 注册回调 / 连接状态"四件最小事（另加 LWT 能力准备，见下）。
//
// ★ Facade 本身**不**创建 MQTT client、**不**维护第二套 TX 队列：
//   全部转发到 CloudManager（全固件**唯一**的 esp_mqtt_client 持有者）。
// ★ 本模块无任何状态；状态全部在 CloudManager 侧（避免 : 隐藏全局可变状态）。
// =====================================================

#include <stddef.h>
#include <stdint.h>

// -----------------------------------------------------
// 发布原始 payload。
//   topic   : 完整 topic（Facade **不**生成业务 topic，不绕 TopicRenderer）
//   payload : 原始字节
//   len     : payload 长度（0 ⇒ 由底层按 C 字符串处理）
//   qos     : 0 / 1
//   retain  : 是否保留（业务调用恒为 false；Homie 保留消息传 true）
//   store   : 是否进 outbox 持久队列（仅 QoS>0 生效）
// 返回：是否成功入队 / 发布。
// -----------------------------------------------------
bool mqtt_transport_publish(const char* topic,
                            const char* payload,
                            size_t len,
                            int qos,
                            bool retain,
                            bool store);

// -----------------------------------------------------
// 注册一个**非业务** topic filter（幂等）。
//   · 连接已建立 ⇒ 立即订阅
//   · 未连接     ⇒ 只注册（返回 true，不失败）；后续 CONNECTED 自动 replay
//   · 同一 filter 重复注册 ⇒ 不新增条目
//   · 容量有限（见报告），超限 / 超长 ⇒ 返回 false
// -----------------------------------------------------
bool mqtt_transport_subscribe(const char* topic_filter, int qos);

// -----------------------------------------------------
// 注册"非业务 topic"的接收回调。
//   ★ 回调在 **loop 上下文**执行（经 CloudManager 队列转交），
//     **绝不**在 esp-mqtt 事件/任务上下文内被同步调用。
//   传 NULL 表示取消注册。
// -----------------------------------------------------
void mqtt_transport_set_foreign_handler(
    void (*cb)(const char* topic, const uint8_t* data, size_t len, bool retain));

// -----------------------------------------------------
// 连接状态（供未来 $state 判定）。
// -----------------------------------------------------
bool mqtt_transport_is_connected();

// -----------------------------------------------------
// [P2-2 · T-1 能力准备，additive] 设置 LWT（Last Will and Testament）。
//   · 仅在调用方显式设置后，下一次建连才会带 LWT；
//   · **默认未设置 ⇒ CONNECT 报文与 P1 完全一致（零行为变化）**；
//   · 本阶段只提供 transport 能力，**不实现**任何 Homie $state / 在线状态机。
// -----------------------------------------------------
void mqtt_transport_set_lwt(const char* topic,
                            const char* message,
                            int qos,
                            bool retain);
