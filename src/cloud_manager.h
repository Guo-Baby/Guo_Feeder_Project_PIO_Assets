#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>

// =====================================================
// CloudManager 协议层
//
// 职责:
//   - MQTT 通信（QoS1, ESP-IDF 原生 esp-mqtt）
//   - 下行压缩 JSON 解析 -> CommandMessage（stable id -> runtime id）
//   - 上行结果字段压缩 + 二进制(MessagePack)上传
//   - registry 查询 / 版本校验 / 分片 / change_msg_limit
//
// 对外接口（签名保持不变）:
//   cloud_send_up() / cloud_send_set() / cloud_upload_json()
// =====================================================

// 云通信初始化
void cloud_init();

// 云通信后台任务，loop 循环调用
void cloud_task();

// MQTT 底层原始发送接口
bool cloud_send_set(const char* message);

bool cloud_send_up(const char *message);

// 【扩展】直接传入 JsonDocument 序列化发布（新增接口）
bool cloud_upload_json(JsonDocument& doc);


// 查询 MQTT 当前在线状态
bool cloud_is_connected();

// =====================================================
// P1.4：多 Topic 上行（纯新增，不改动现有 Up / Down 语义）
//
//   down  : Cloud → Device   命令 + log_ack
//   up    : Device → Cloud   现有业务上行（语义不变）
//   log   : Device → Cloud   LogManager 专用（本 API）
//
// LogManager **不得**直接调用 MQTT；一律经此 API。
// 原 `cloud_send_up_cbor` / `cloud_send_set_cbor` / `cloud_send_up_binary`
// 三个声明从未实现、也无任何调用点（审计已标记为陷阱），
// 现由下面这组真实 API 取代。
// =====================================================

// 上行路由（后续新增 Topic 时在此扩展，不改现有函数签名）
enum CloudRoute
{
    CLOUD_ROUTE_UP  = 0,
    CLOUD_ROUTE_LOG = 1
};

// 向 log Topic 发送二进制（CBOR）负载。
// 离线 / 未连接时返回 false（**不排队、不落盘** —— 由 LogManager 负责重试）。
bool cloud_send_log(const uint8_t* data, size_t length);

// log_ack 回调（P1.5）：收到的下行 ACK 区间
typedef void (*CloudLogAckCallback)(
    uint32_t boot_seq,
    uint32_t seq_from,
    uint32_t seq_to
);

void cloud_set_log_ack_callback(CloudLogAckCallback callback);

// =====================================================
// 协议枚举（云端协议定义）
// =====================================================

// 状态枚举: 上行统一 status -> s
enum CloudStatus
{
    STATUS_OK = 0,
    STATUS_ACCEPTED,
    STATUS_RUNNING,
    STATUS_FAILED,
    STATUS_ERROR        // 协议错误（版本不匹配 / 非法命令 / 非法对象等）
};

// 错误码: 上行统一 error -> e
enum ErrorCode
{
    ERROR_NONE = 0,
    ERROR_INVALID_COMMAND,     // 非法命令
    ERROR_INVALID_OBJECT,      // 非法对象 / stable id 不存在
    ERROR_VERSION_MISMATCH,    // registry 版本不匹配
    ERROR_NOT_FOUND,
    ERROR_BUSY,
    ERROR_TIMEOUT,
    ERROR_INTERNAL
};

// registry 类型: 下行 { "c":"registry", "k":0/1/2 }
enum RegistryType
{
    REGISTRY_ACTION = 0,
    REGISTRY_TRIGGER,
    REGISTRY_WORKFLOW
};

// 内部消息类型判断
enum CloudMessageType
{
    CLOUD_MSG_COMMAND = 0,
    CLOUD_MSG_RESULT,
    CLOUD_MSG_ACK,
    CLOUD_MSG_REGISTRY,
    CLOUD_MSG_FRAGMENT
};

// 分片类型: BEGIN 0 / DATA 1 / END 2
enum FragmentType
{
    FRAG_BEGIN = 0,
    FRAG_DATA,
    FRAG_END
};

// 分片默认上限：二进制化后消息长度超过该值进入分片
#define CLOUD_DEFAULT_MSG_LIMIT 8000

//=========================
