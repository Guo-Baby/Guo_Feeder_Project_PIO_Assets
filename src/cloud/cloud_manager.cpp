#include <Arduino.h>
#include <mqtt_client.h>
#include <esp_event.h>
#include <WiFi.h>
#include <ArduinoJson.h>
#include <LittleFS.h>

#include "cloud/cloud_manager.h"
#include "services/system_state.h"
#include "services/config_manager.h"
#include "services/event_manager.h"
#include "services/time_manager.h"
#include "services/command_manager.h"
#include "automation/capability_registry.h"
#include "log/log_events.h"   // P1.4/P1.5：Log Topic 与 log_ack 常量（冻结契约）
#include "log/log_manager.h"  // P2-D：Cloud/MQTT 日志埋点（只调 log_emit / log_arg_*）
#include "services/topic_renderer.h"  // P0-3：Topic / client_id 模板渲染（渲染点唯一）
#include "services/device_identity.h" // P0-3：device_id()（判定 client_id 是否与设备身份相关）
#include "services/credential_manager.h"  // Phase D-2：凭据身份获取 + 连接结果回报（Q9）
#include "cloud/mqtt_routing.h"  // P2-2：topic 分派 / 分片重组 / 订阅注册表（纯逻辑，宿主可测）

// =====================================================
// P1-7：Production Topic Enforcement（编译期环境开关）
// =====================================================
//   0（默认）= dev / test —— 完全保持 P0 行为：legacy 模板原样透传（T-5 不变）
//   1        = production —— 3 个 MQTT Topic 模板必须含 <device_id>；
//                            任一缺失 ⇒ 启动期 ERROR + **阻止 MQTT 上线**
//
// 为什么用**编译期宏**而不是配置项：运行期（config_set / 云端下发）**无法**关闭该检查，
//   这是 P1-7 的安全前提。默认 0 ⇒ 生产分支与其字符串被 --gc-sections 剔除（零代价）。
// 与 P0 §3.3.1 的关系：该节只加约束并明确"production 标志如何确定属后续阶段"，
//   本宏即该"后续阶段"的裁决结果（P1-7，见 docs/architecture/P1-7-Review-Report.md 方案 ①）。
// 生产构建：PLATFORMIO_BUILD_FLAGS="-DGF_ENV_PRODUCTION=1" pio run
#ifndef GF_ENV_PRODUCTION
#define GF_ENV_PRODUCTION 0
#endif

// P1-7 / F-3：启动前置检查结果。
//   ★ 语义**仅限**"本模块当前启动配置被阻断"—— 不是 System State、
//     不承担任何 Action/Workflow 实例 runtime、不进 loop 热路径。
//   ★ 判定只在 cloud_init() 做**一次**；cloud_connect() 每轮 loop 都会调用，
//     故只读该标志、不重复判定与打印（否则离线时会刷屏）。
static bool s_mqtt_precheck_failed = false;

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
// Phase D-2：与 CredentialManager 的唯一交界
//
// ★ 冻结（Phase C Q9）：CloudManager **只**做两件事
//     A. 取"当前要用的 MQTT 身份"（不透明三元组）
//     B. 回报连接结果
//   **不得感知** ACTIVE / TESTING / generation / phase / state。
//   `client_id_template` 对这里只是**不透明字符串**：本模块只做渲染，
//   不理解（也不得判断）其是否含测试后缀。
//
// 连接结果**只在任务回调里记录**，由 cloud_task()（loop 上下文）转发，
// 避免在 esp-mqtt 任务里做 NVS 写入。
// =====================================================
static uint32_t s_cred_epoch_applied = 0;   // 已应用的凭据身份版本
static bool     s_swapping_credentials = false;  // 换连窗口（抑制"离线"误报）

// ★ D-3 上板实测修复（**消息丢失缺陷**）：
//   凭据 promote 后立刻重建客户端（stop+destroy），会把**刚刚入队、尚未发出**的上行
//   一起丢掉 —— 实测正是 `credential_confirm`（`enqueue OK` 后无投递，云端永远收不到）。
//   修法：身份变化时**先进入"等待 outbox 排空"状态**，排空（或有界超时）后再重建。
//   全程**非阻塞**（不 delay、不 while 死等），每轮 loop 推进一次。
#define CRED_SWITCH_DRAIN_MIN_MS   400UL    // 最短宽限：给 esp-mqtt 任务一次发送机会
#define CRED_SWITCH_DRAIN_MAX_MS   2500UL   // 上界：网络异常时不得长期挂着旧连接

static bool     s_cred_switch_waiting = false;
static uint32_t s_cred_switch_armed_ms = 0;
static uint32_t s_cred_switch_deadline = 0;

static bool     s_conn_result_pending = false;
static bool     s_conn_result_ok = false;
static int      s_conn_result_reason = 0;

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
// ★ LOG-ACK-SPLIT（2026-10-23）：日志回执专用下行 Topic。
//   为切换期原子性：**同时订阅** `mqtt_sub_topic`（旧，兼容）与 `mqtt_ack_topic`（新）。
//   两者都走业务分派 ⇒ 收到哪条都能进 `c=="log_ack"` 分支，不存在断链窗口。
static String mqtt_ack_topic;
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
// P2-D：LogManager 埋点用局部状态
//
// ⚠️ 以下变量**只服务于日志**，不参与任何连接判定、不改变重连/退避流程、
//    不引入任何等待或阻塞。cloud_task() 的分支逻辑与接入前完全一致。
// =====================================================

// MQTT 状态迁移的 WAS/STATE 取值（与埋点强绑定，勿随意改值）
//
// 底层就是 bool `mqtt_connected` ⇒ 用最小两值枚举。
// ⚠️ 这是 CloudManager 局部约定；若将来出现第三态必须先冻结再扩展。
#define CLOUD_MQTT_STATE_OFFLINE  0u
#define CLOUD_MQTT_STATE_ONLINE   1u

// 发布失败聚合（离线时每 loop 可能失败多次 ⇒ **绝不逐条记**）
//
// 采用审查报告 §5.3「计数聚合」：失败只累加计数，由 cloud_task() 按周期窗口
// 上报 1 条，被抑制的次数放进 LOG_P_FAIL_COUNT ⇒ 信息不丢、条目不洪泛。
#define CLOUD_PUBLISH_FAIL_REPORT_MS   60000u

static uint32_t cloud_publish_fail_count = 0;
static unsigned long cloud_publish_fail_report_ms = 0;

// 记一次发布失败（只累加，不发射）
static void cloud_note_publish_fail()
{
    if(cloud_publish_fail_count < 0xFFFFFFFFu)
    {
        cloud_publish_fail_count++;
    }
}

// 周期窗口到 ⇒ 上报一条 LOG_MQTT_PUBLISH_FAIL
//（由 cloud_task() 在 loop 上下文调用；窗口内无失败 ⇒ 完全静默）
static void cloud_report_publish_fail()
{
    if(cloud_publish_fail_count == 0)
    {
        return;
    }

    const unsigned long now = millis();

    if(
        cloud_publish_fail_report_ms != 0
        &&
        (now - cloud_publish_fail_report_ms)
        <
        CLOUD_PUBLISH_FAIL_REPORT_MS
    )
    {
        return;
    }

    cloud_publish_fail_report_ms = now;

    LogParamIn p[1];
    p[0] =
        log_arg_u32(
            LOG_P_FAIL_COUNT,
            cloud_publish_fail_count
        );
    log_emit(
        LOG_MQTT_PUBLISH_FAIL,
        LOG_LVL_WARN,
        p,
        1
    );

    cloud_publish_fail_count = 0;
}

// 命令 ID 哈希（FNV-1a 32）
//
// 为什么不直接传字符串：`LogParamIn` 的 union 只有 {i,u,f,b}，**无 blob 字段**，
// `LOG_PTYPE_STR` 当前不可构造 ⇒ 按 P2 定版走「哈希/枚举化」（与
// `config_manager.cpp` 的 `cfg_hash32()`、`wifi_module.cpp` 的
// `wifi_ssid_hash32()` 同一套做法），不为此扩 LogManager API。
// ⚠️ 三处重复实现，后续多模块共用时应上移为共享工具。
static uint32_t cloud_cmd_id_hash32(const char *s)
{
    uint32_t h = 2166136261u;

    while(s != nullptr && *s != '\0')
    {
        h ^= (uint8_t)(*s++);
        h *= 16777619u;
    }

    return h;
}

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

// =====================================================
// P2-2：MQTT 传输边界状态（分片重组 / 订阅注册表 / foreign 队列 / LWT）
//
// ★ 全部为**有限容量**的静态状态，无动态分配、无无限增长容器。
// ★ 分片重组逻辑在 mqtt_routing.h（纯函数，宿主可测），此处只持状态与缓冲。
// =====================================================
static MqttRxAssembler s_rx_asm;      // 下行分片重组状态机（缓冲 = mqtt_large_rx_buffer）
static MqttSubRegistry s_foreign_subs; // 非业务（foreign）订阅注册表（有限容量、幂等）

// foreign 消息队列：esp-mqtt 任务**只入队**（轻量拷贝），
// loop 上下文（cloud_task）出队并交付 handler ⇒ 满足 AD-5a「不在 MQTT 任务内执行」。
#define MQTT_FOREIGN_QUEUE_SLOTS  2
#define MQTT_FOREIGN_PAYLOAD_MAX  256

struct CloudForeignMsg
{
    char     topic[MQTT_ROUTE_TOPIC_MAX];
    uint16_t len;
    bool     retain;
    uint8_t  data[MQTT_FOREIGN_PAYLOAD_MAX];
};

static CloudForeignMsg s_foreign_q[MQTT_FOREIGN_QUEUE_SLOTS];
static uint8_t s_foreign_head  = 0;
static uint8_t s_foreign_tail  = 0;
static uint8_t s_foreign_count = 0;

// foreign 接收回调（仅函数指针；由 mqtt_transport_set_foreign_handler 注入）
static void (*s_foreign_cb)(const char*, const uint8_t*, size_t, bool) = nullptr;

// P2-2 · T-1：LWT 能力准备（**默认未设置 ⇒ CONNECT 报文与 P1 完全一致**）
static char s_lwt_topic[MQTT_ROUTE_TOPIC_MAX];
static char s_lwt_msg[64];
static int  s_lwt_qos    = 0;
static bool s_lwt_retain = false;

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
// P2-2：topic 分派 / foreign 队列（transport 边界）
//
// ★ 只做"分类 + 拷贝入队"，**不执行业务命令、不调 foreign handler**：
//     · 业务 topic ⇒ 沿用既有 cloud_rx_enqueue()（业务管线**逐字不变**）
//     · foreign    ⇒ 仅拷贝进 s_foreign_q（由 cloud_task 在 loop 上下文交付）
//     · 未注册     ⇒ 丢弃（既不进业务管线，也不交 foreign）
//
// ★ 本段运行在 **esp-mqtt 任务上下文**（由 mqtt_event_handler 调用）
//   ⇒ 只允许无阻塞、无 NVS、无回调的轻量操作。
// =====================================================
static void cloud_transport_enqueue_foreign(
    const char* topic,
    const uint8_t* data,
    size_t len,
    bool retain)
{
    if(s_foreign_count >= MQTT_FOREIGN_QUEUE_SLOTS)
    {
        Serial.println("[Cloud] foreign queue full, message dropped");
        return;
    }
    if(len > MQTT_FOREIGN_PAYLOAD_MAX)
    {
        Serial.printf(
            "[Cloud] foreign message too large (%u), dropped\n",
            (unsigned)len);
        return;
    }

    CloudForeignMsg& m = s_foreign_q[s_foreign_tail];
    mqtt_cstr_copy(m.topic, MQTT_ROUTE_TOPIC_MAX, topic);
    m.len = (uint16_t)len;
    m.retain = retain;
    for(size_t i = 0; i < len; ++i)
    {
        m.data[i] = data[i];
    }
    s_foreign_tail = (uint8_t)((s_foreign_tail + 1) % MQTT_FOREIGN_QUEUE_SLOTS);
    s_foreign_count++;
}

static void cloud_transport_dispatch_rx(
    const char* topic,
    const uint8_t* data,
    size_t len,
    bool retain)
{
    MqttRouteKind kind =
        mqtt_route_topic(topic, mqtt_sub_topic.c_str(), s_foreign_subs);

    // ★ LOG-ACK-SPLIT（2026-10-23）：日志回执的独立 topic 也**判为业务**。
    //   这样它复用既有 `c=="log_ack"` bypass 分支（`cloud_manager.cpp` 的
    //   `log_ack` 处理），**零新增 handler**；也避免引入 foreign 路径 ——
    //   全仓没有任何模块注册 foreign handler（`s_foreign_cb` 恒为 null），
    //   走那条路等于启用一条从未在生产跑过的分支。
    if(kind != MQTT_ROUTE_BUSINESS && mqtt_ack_topic.length() > 0
       && mqtt_topic_matches_filter(mqtt_ack_topic.c_str(), topic))
    {
        kind = MQTT_ROUTE_BUSINESS;
    }

    if(kind == MQTT_ROUTE_BUSINESS)
    {
        // 既有业务路径（P2-2 前即为此调用，逐字保留）
        cloud_rx_enqueue(data, len);
        return;
    }
    if(kind == MQTT_ROUTE_FOREIGN)
    {
        cloud_transport_enqueue_foreign(topic, data, len, retain);
        return;
    }

    Serial.printf(
        "[Cloud] RX dropped (unregistered topic): %s\n",
        (topic != nullptr) ? topic : "(null)");
}

// 由 cloud_task() 在 **loop 上下文** 调用：出队并交付 foreign handler
static void cloud_transport_process_foreign()
{
    while(s_foreign_count > 0)
    {
        CloudForeignMsg& m = s_foreign_q[s_foreign_head];
        if(s_foreign_cb != nullptr)
        {
            s_foreign_cb(m.topic, m.data, (size_t)m.len, m.retain);
        }
        s_foreign_head = (uint8_t)((s_foreign_head + 1) % MQTT_FOREIGN_QUEUE_SLOTS);
        s_foreign_count--;
    }
}

// 由 mqtt_event_handler 在 CONNECTED 时调用：重放 foreign 订阅
static void cloud_transport_replay_foreign()
{
    if(mqtt_client == nullptr)
    {
        return;
    }
    for(int i = 0; i < s_foreign_subs.count; ++i)
    {
        const int msg_id = esp_mqtt_client_subscribe(
            mqtt_client,
            s_foreign_subs.entries[i].filter,
            s_foreign_subs.entries[i].qos);
        Serial.printf(
            "[Cloud] MQTT replay subscribe filter=%s qos=%d msg_id=%d\n",
            s_foreign_subs.entries[i].filter,
            s_foreign_subs.entries[i].qos,
            msg_id);
    }
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
            // Phase D-2：记录"连接成功"（由 cloud_task 转发给 CredentialManager）
            s_conn_result_pending = true;
            s_conn_result_ok      = true;
            s_conn_result_reason  = 0;
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
            // ★ LOG-ACK-SPLIT：**同时订阅**日志回执专用 topic。
            //   与上一段并存 ⇒ 无论 Worker 发旧 `/down` 还是新 `/log/ack`，
            //   设备都能收到 ACK，切换过程不存在"收不到 ack ⇒ 停等超时"的断链窗口。
            //   ★ 该订阅必须在 ACL 里被授权（`credential.js::buildAclRules`），
            //     ACL 缺失时 broker **静默拒绝**（不会报错，只表现为收不到消息）。
            if(mqtt_ack_topic.length() > 0)
            {
                int ack_msg_id = esp_mqtt_client_subscribe(
                    mqtt_client,
                    mqtt_ack_topic.c_str(),
                    CLOUD_MQTT_QOS);
                Serial.printf(
                    "[Cloud] MQTT subscribe(log-ack) filter=%s qos=%d msg_id=%d\n",
                    mqtt_ack_topic.c_str(),
                    CLOUD_MQTT_QOS,
                    ack_msg_id);
            }
            // ★ P2-2（T-2）：重放注册表中已登记的 **foreign** 订阅。
            //   业务订阅保持上面那段**逐字不变**（filter/QoS/日志一致）。
            cloud_transport_replay_foreign();
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

            // 凭据换连窗口内的断开是本模块自身动作，**不算离线**（避免误报）
            if (!s_swapping_credentials)
            {
                mqtt_disconnect_pending = true;
            }

            // Phase D-2：记录"未连上"。⚠️ reason 采用**保留更具体者**的策略：
            //   认证类失败由 MQTT_EVENT_ERROR 给出（CONNACK 4/5），
            //   若随后 DISCONNECTED 用 0 覆盖，会把"认证失败"误判为"网络失败"。
            if (!s_conn_result_ok)
            {
                s_conn_result_pending = true;
            }
            else
            {
                s_conn_result_pending = true;
                s_conn_result_ok      = false;
                s_conn_result_reason  = 0;
            }
            break;

        case MQTT_EVENT_PUBLISHED:
            Serial.printf(
                "[Cloud] MQTT published/ack msg_id=%d outbox=%d t=%lu\n",
                event->msg_id,
                esp_mqtt_client_get_outbox_size(mqtt_client),
                (unsigned long)(millis() & 0xFFFFFFFFUL));
            break;

        case MQTT_EVENT_DATA:
        {
            // ★ P2-2（T-3）：topic-aware 分派。
            //   esp-mqtt 契约（mqtt_client.h）：一条消息若被分片，**只有首片携带 topic**，
            //   其余片只有 data/data_len/current_data_offset ⇒ topic 在 offset==0 时捕获。
            //   ⚠️ 业务路径**逐字保留**：业务 topic 仍走 cloud_rx_enqueue() 同一入口；
            //      foreign / 未注册 topic 不进入业务管线（不分派到 translate/ACK/dedupe）。
            const int rx_rc = s_rx_asm.feed(
                event->current_data_offset,
                event->total_data_len,
                event->topic,
                event->topic_len,
                event->retain,
                (const uint8_t*)event->data,
                event->data_len);

            if(rx_rc == 1)
            {
                // 完整一条消息 ⇒ 按 topic 分派
                cloud_transport_dispatch_rx(
                    s_rx_asm.topic_cstr(),
                    s_rx_asm.data(),
                    s_rx_asm.len(),
                    s_rx_asm.retain_flag());
                s_rx_asm.reset();
            }
            else if(rx_rc == 2)
            {
                Serial.printf(
                    "[Cloud] MQTT message too large: %d\n",
                    event->total_data_len);
            }
            break;
        }

        case MQTT_EVENT_ERROR:
            Serial.println("[Cloud] MQTT error event");
            {
                // Phase D-2：把 CONNACK 返回码提取出来交给 CredentialManager 判定
                //   （4 = bad user name or password，5 = not authorized）
                //   仅 MQTT_ERROR_TYPE_CONNECTION_REFUSED 才带 connect_return_code；
                //   其余（TLS / DNS / 超时）保持 0 ⇒ 属"网络类"，不得判凭据失败。
                int reason = 0;

                if (event->error_handle != nullptr &&
                    event->error_handle->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED)
                {
                    reason = (int)event->error_handle->connect_return_code;
                }

                s_conn_result_pending = true;
                s_conn_result_ok      = false;

                // 保留更具体的失败原因（非 0 不被 0 覆盖）
                if (reason != 0)
                {
                    s_conn_result_reason = reason;
                }

                Serial.printf("[Cloud] MQTT conn reason=%d\n", s_conn_result_reason);
            }
            break;

        default:
            break;
    }
}

// =====================================================
// 底层发布（QoS1，非阻塞，失败自动重发直到 PUBACK）
// =====================================================
static bool cloud_mqtt_publish_binary(
    const char* topic,
    const uint8_t* data,
    size_t len,
    int qos,
    bool retain = false,
    bool store = CLOUD_MQTT_STORE)
{
    if(mqtt_client == nullptr || !mqtt_connected)
    {
        // P2-D：只累加计数，不逐条记（离线时可能每 loop 都走到这里）
        cloud_note_publish_fail();
        return false;
    }
    int msg_id;
    int outbox_before = esp_mqtt_client_get_outbox_size(mqtt_client);

    // ★ P2-2（T-4）：retain / store 改为**显式参数**（默认 false / CLOUD_MQTT_STORE）
    //   ⇒ 现有业务调用点（4 处，均传 qos=1、不传 retain/store）语义**完全不变**；
    //     仅 Transport Facade 显式传入 retain=true 时才改变（且只影响非业务消息）。
    if(qos == 0)
    {
        msg_id = esp_mqtt_client_publish(
            mqtt_client,
            topic,
            (const char*)data,
            len,
            0,
            retain ? 1 : 0);
    }
    else
    {
        msg_id = esp_mqtt_client_enqueue(
            mqtt_client,
            topic,
            (const char*)data,
            len,
            qos,
            retain ? 1 : 0,
            store);
    }
    if(msg_id < 0)
    {
        Serial.printf(
            "[Cloud] publish FAIL %s len=%u\n",
            topic,
            (unsigned)len);
        // P2-D：只累加计数（真正的上报由 cloud_task() 按 60s 窗口做）
        cloud_note_publish_fail();
        return false;
    }
    Serial.printf(
        "[Cloud] ENQ id=%d qos=%d store=%d outbox=%d->%d len=%u t=%lu\n",
        msg_id,
        qos,
        (int)store,
        outbox_before,
        esp_mqtt_client_get_outbox_size(mqtt_client),
        (unsigned)len,
        (unsigned long)(millis() & 0xFFFFFFFFUL));
    return true;
}

static bool cloud_mqtt_publish_text(
    const char* topic,
    const String& text,
    int qos,
    bool retain = false,
    bool store = CLOUD_MQTT_STORE)
{
    if(mqtt_client == nullptr || !mqtt_connected)
    {
        // P2-D：只累加计数，不逐条记
        cloud_note_publish_fail();
        return false;
    }
    int msg_id;

    if(qos == 0)
    {
        msg_id = esp_mqtt_client_publish(
            mqtt_client,
            topic,
            text.c_str(),
            text.length(),
            0,
            retain ? 1 : 0);
    }
    else
    {
        msg_id = esp_mqtt_client_enqueue(
            mqtt_client,
            topic,
            text.c_str(),
            0,
            qos,
            retain ? 1 : 0,
            store);
    }

    if(msg_id < 0)
    {
        Serial.printf(
            "[Cloud] publish FAIL qos=%d store=%d topic=%s len=%u\n",
            qos,
            (int)store,
            topic,
            (unsigned)text.length());
        // P2-D：只累加计数（真正的上报由 cloud_task() 按 60s 窗口做）
        cloud_note_publish_fail();
        return false;
    }

    Serial.printf(
        "[Cloud] enqueue OK id=%d qos=%d store=%d len=%u\n",
        msg_id,
        qos,
        (int)store,
        (unsigned)text.length());
    return true;
}

// =====================================================
// P2-2：Transport Facade 面向实现（public）
//
// ★ 仅由 cloud/mqtt_transport.cpp 转发使用；业务代码不直接调用。
// ★ 保持 CloudManager **不依赖** mqtt_transport（本文件不 include mqtt_transport.h）
//   ⇒ 无 cloud_manager ↔ mqtt_transport 循环（P2-0 §6.4）。
// =====================================================
bool cloud_transport_publish(const char* topic,
                             const char* payload,
                             size_t len,
                             int qos,
                             bool retain,
                             bool store)
{
    if(topic == nullptr || payload == nullptr)
    {
        return false;
    }
    return cloud_mqtt_publish_binary(
        topic,
        (const uint8_t*)payload,
        len,
        qos,
        retain,
        store);
}

bool cloud_transport_subscribe(const char* topic_filter, int qos)
{
    // 注册（幂等 + 有限容量）；超限 / 超长直接拒绝（不静默截断）
    if(!s_foreign_subs.add(topic_filter, qos))
    {
        Serial.printf(
            "[Cloud] subscribe reject (invalid/null/full): %s\n",
            (topic_filter != nullptr) ? topic_filter : "(null)");
        return false;
    }

    // 已连接 ⇒ 立即订阅；未连接 ⇒ 只注册（不失败），待 CONNECTED replay
    if(mqtt_client != nullptr && mqtt_connected)
    {
        const int msg_id = esp_mqtt_client_subscribe(mqtt_client, topic_filter, qos);
        Serial.printf(
            "[Cloud] MQTT subscribe(foreign) filter=%s qos=%d msg_id=%d\n",
            topic_filter,
            qos,
            msg_id);
        return (msg_id >= 0);
    }
    return true;
}

void cloud_transport_set_foreign_handler(
    void (*cb)(const char* topic, const uint8_t* data, size_t len, bool retain))
{
    s_foreign_cb = cb;
}

void cloud_transport_set_lwt(const char* topic,
                             const char* message,
                             int qos,
                             bool retain)
{
    if(topic == nullptr || topic[0] == '\0')
    {
        return;
    }
    mqtt_cstr_copy(s_lwt_topic, MQTT_ROUTE_TOPIC_MAX, topic);
    mqtt_cstr_copy(s_lwt_msg, sizeof(s_lwt_msg), (message != nullptr) ? message : "");
    s_lwt_qos = qos;
    s_lwt_retain = retain;
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
    // P2-2：publish 助手改为接受 const char* topic（避免为 Facade 造临时 String）
    const char* topic = (msg_type == CLOUD_FRAG_UP)
                            ? mqtt_pub_topic.c_str()
                            : mqtt_sub_topic.c_str();

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

    DynamicJsonDocument doc(4096);
    if(deserializeJson(doc, message))
    {
        Serial.println("[Cloud] MQTT RX (unparsable, printed raw)");
        Serial.println(message);
        Serial.println("[Cloud] JSON parse failed");
        return;
    }

    bool compact = !doc["c"].isNull();
    const char* c = compact ? doc["c"] : doc["cmd"];
    if(c == nullptr)
    {
        Serial.println("[Cloud] MQTT RX (missing command, printed raw)");
        Serial.println(message);
        Serial.println("[Cloud] Missing command");
        return;
    }

    // =====================================================
    // Phase D-2：**凭据类命令的原始报文绝不回显**
    //
    // 原因（安全边界 D-5）：下行 `credential_set` 报文内含明文 password，
    // 原实现在此处无条件 `Serial.println(message)` ⇒ 等同于把凭据写进串口日志。
    // 现改为：先解析、识别对象名，命中凭据对象则**只打印命令名与 id**。
    //
    // ⚠️ 这里只做"是否脱敏"的判定，不解析凭据字段、也不持有其内容。
    // =====================================================
    const char* rx_object = nullptr;
    if(compact)
    {
        rx_object = doc["p"]["o"] | (const char*)nullptr;
        if(rx_object == nullptr)
        {
            rx_object = doc["p"]["object"] | (const char*)nullptr;
        }
    }
    else
    {
        rx_object = doc["ob"] | (const char*)nullptr;
    }

    const bool rx_is_credential =
        credential_manager_is_credential_object(rx_object);

    Serial.println("=================");
    Serial.println("[Cloud] MQTT RX");
    if(rx_is_credential)
    {
        Serial.printf(
            "[Cloud] [credential cmd redacted] c=%s id=%s\n",
            c,
            (compact ? (doc["i"] | "") : (doc["id"] | "")));
    }
    else
    {
        Serial.println(message);
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

        // ---- P2-D 埋点：LOG_MQTT_CMD_EXEC_FAILED（WARN）----
        //
        // 命令 ID 以**哈希**承载：`LOG_P_CMD_ID`(0x2D) 是字符串语义，而
        // `LogParamIn` 无 blob 字段、`LOG_PTYPE_STR` 不可构造 ⇒ 按 P2 定版走
        // 哈希（与 `LOG_P_SSID_HASH` 先例一致），不为此扩 LogManager API。
        // ⚠️ 因此云端只能按哈希聚合/比对，**无法从日志反查命令原文**。
        //
        // 无需限流：频率由云端下发速率决定（非 loop 驱动的循环）。
        {
            LogParamIn p[1];
            p[0] =
                log_arg_u32(
                    LOG_P_CMD_ID,
                    cloud_cmd_id_hash32(
                        cmd.cmd_id.c_str()
                    )
                );
            log_emit(
                LOG_MQTT_CMD_EXEC_FAILED,
                LOG_LVL_WARN,
                p,
                1
            );
        }
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
    // ---- P1-7 / F-3：启动前检查未通过 ⇒ 立即失败 ----
    // ★ 必须在本函数**最前面**（esp_mqtt_client_init() 之前）：
    //   不建立 MQTT session、不执行 subscribe。
    // ★ 静默返回：失败原因已由 cloud_init() 打印**一次**；cloud_task() 每轮 loop
    //   都会调用本函数，若在此打印会持续刷屏。
    if(s_mqtt_precheck_failed)
    {
        return false;
    }

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

        // ---- P2-2（T-1）：LWT 能力准备 ----
        // ★ 仅当调用方显式设置过 LWT（s_lwt_topic 非空）才写入 cfg。
        //   默认未设置 ⇒ cfg 与 P1 **完全一致**（CONNECT 报文不带 will）⇒ 零行为变化。
        // ★ 本阶段只提供 transport 能力，不实现任何 Homie $state / 在线状态机。
        if(s_lwt_topic[0] != '\0')
        {
            cfg.lwt_topic   = s_lwt_topic;
            cfg.lwt_msg     = s_lwt_msg;
            cfg.lwt_msg_len = (int)mqtt_cstr_len(s_lwt_msg);
            cfg.lwt_qos     = s_lwt_qos;
            cfg.lwt_retain  = s_lwt_retain ? 1 : 0;
        }

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

        // ★ Phase D-2（安全边界 D-5）：**不得打印凭据**
        //   原实现曾打印 username / password 明文 ⇒ 已移除。
        //   仅打印"凭据来源"这一非敏感事实，便于排障。
        Serial.println("username=(hidden)");
        Serial.println("password=(hidden)");

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
// =====================================================
// P0-3：Topic / client_id 渲染（渲染点全工程唯一 —— topic_render()）
//
// 契约见 docs/architecture/P0-设备身份与Topic隔离设计.md §3.3：
//   · 渲染成功、模板含 <device_id>   -> 直接使用
//   · 渲染成功、模板不含 <device_id> -> 仍使用（配置级回滚通路），但打**一次** WARN
//   · 渲染失败（空 / 超长 / 残留 <> / device_id 不可用）
//                                   -> ERROR + 回落**内建 V3 模板**
//   · 内建模板亦失败（device_id 不可用）-> 返回空串，交由调用方判为配置错误
//     ★ 设计原则：宁可连不上，也不要用共享 Topic 静默跑起来
// =====================================================
static bool s_warned_shared_topic = false;

static String cloud_render_topic(const String& tpl, const char* fallback_tpl, const char* what)
{
    String out;

    if(topic_render(tpl, out))
    {
        if(tpl.indexOf(GF_DEVICE_ID_PLACEHOLDER) < 0 && !s_warned_shared_topic)
        {
            s_warned_shared_topic = true;
            Serial.printf("[Identity] WARN %s has no <device_id>, shared-topic mode\n", what);
        }

        return out;
    }

    Serial.printf("[Identity] ERROR invalid %s, using built-in V3 default\n", what);

    String fb;

    if(topic_render(String(fallback_tpl), fb))
    {
        return fb;
    }

    return String();
}

// client_id 的字符集比 topic 更严（设计 §1.4.1 ID-4）：
//   仅 [0-9a-zA-Z_-]，长度 <= 128；**超限判配置错误，不静默截断**
static bool is_valid_client_id(const String& s)
{
    if(s.length() == 0 || s.length() > 128)
    {
        return false;
    }

    for(size_t i = 0; i < s.length(); i++)
    {
        const char ch = s[i];

        const bool ok = (ch >= '0' && ch <= '9')
                     || (ch >= 'a' && ch <= 'z')
                     || (ch >= 'A' && ch <= 'Z')
                     || ch == '_' || ch == '-';

        if(!ok)
        {
            return false;
        }
    }

    return true;
}

static String cloud_render_client_id(const String& tpl)
{
    String out;

    if(topic_render(tpl, out) && is_valid_client_id(out))
    {
        // 可见性（设计 §3.3「允许必须可见」）：client_id 与 device_id 无关
        // 会让多台设备共享同一 MQTT session => 互相踢下线（§1.4.1 ID-2）
        if(out.indexOf(device_id()) < 0)
        {
            Serial.println("[Identity] WARN client_id has no device_id, may collide across devices");
        }

        return out;
    }

    Serial.println("[Identity] ERROR invalid client_id, using built-in default");

    String fb;

    if(topic_render(String(GF_CLIENT_ID_TPL), fb) && is_valid_client_id(fb))
    {
        return fb;
    }

    return String();
}

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
    // P0-3：读模板 -> 渲染（失败回落内建 dev_<device_id>）
    mqtt_client_id = cloud_render_client_id(config_get_mqtt_client_id());
    Serial.print("MQTT client_id=");
    Serial.println(mqtt_client_id);

    // =====================================================
    // Phase D-2：凭据来源 —— **凭据存储优先，配置兜底**
    //
    // 冻结（Phase C Q6 / 用例 D-3）：
    //   - 已有 `gfcred` ACTIVE 凭据 ⇒ 用它（一机一密）
    //   - 无（未签发 / NVS 损坏）⇒ 回落配置值，**不阻塞启动**（P0 过渡凭据）
    //   - **绝不打印凭据内容**（安全边界 D-5）
    // =====================================================
    {
        char cred_user[CRED_USERNAME_MAX] = { 0 };
        char cred_pass[CRED_PASSWORD_MAX] = { 0 };

        if (credential_manager_get_active(cred_user, sizeof(cred_user),
                                          cred_pass, sizeof(cred_pass)))
        {
            mqtt_username = cred_user;
            mqtt_password = cred_pass;
            Serial.println("[Cloud] credential source: gfcred (ACTIVE)");
        }
        else
        {
            mqtt_username = config_get_mqtt_username();
            mqtt_password = config_get_mqtt_password();
            Serial.println("[Cloud] credential source: config (fallback)");
        }

        // 本地副本清零，缩短明文驻留
        memset(cred_user, 0, sizeof(cred_user));
        memset(cred_pass, 0, sizeof(cred_pass));
    }

    // 记录初始身份版本：避免 cloud_task 首次运行即触发一次多余换连
    {
        CredentialIdentity ident;

        if (credential_manager_get_identity(ident))
        {
            s_cred_epoch_applied = ident.epoch;
        }
    }
    mqtt_sub_topic = cloud_render_topic(config_get_mqtt_subscribe_topic(),
                                        GF_TOPIC_TPL_DOWN, "subscribe_topic");
    Serial.print("MQTT subscribe=");
    Serial.println(mqtt_sub_topic);
    mqtt_pub_topic = cloud_render_topic(config_get_mqtt_publish_topic(),
                                        GF_TOPIC_TPL_UP, "publish_topic");
    Serial.print("MQTT publish=");
    Serial.println(mqtt_pub_topic);
    mqtt_log_topic = cloud_render_topic(config_get_mqtt_log_topic(),
                                        GF_TOPIC_TPL_LOG, "log_topic");
    Serial.print("MQTT log=");
    Serial.println(mqtt_log_topic);
    // ★ LOG-ACK-SPLIT：ack topic 由固定模板渲染（不读配置，避免与 Worker 漂移）
    mqtt_ack_topic = cloud_render_topic(String(GF_TOPIC_TPL_ACK),
                                        GF_TOPIC_TPL_ACK, "ack_topic");
    Serial.print("MQTT log-ack=");
    Serial.println(mqtt_ack_topic);

    // =====================================================
    // P1-7 / F-3：MQTT 上线前置检查（**判定一次**，结果供 cloud_connect() 读）
    //
    //   ① P1-7 production policy（仅 GF_ENV_PRODUCTION=1）：
    //      三个 Topic **模板**（渲染前）必须含 <device_id>，否则判配置错误。
    //      ★ 必须判**模板**：渲染后占位符已被替换，无法再判定。
    //      ★ 这是"当前环境是否允许使用这个渲染结果"的**策略**判断，
    //        不是渲染功能本身 ⇒ 放在 CloudManager；
    //        TopicRenderer 的 T-5（无占位符 ⇒ 原样放行）**保持不变**。
    //   ② F-3：渲染最终失败时 cloud_render_topic() 返回空串，其契约要求调用方
    //      判为配置错误 ⇒ 绝不允许带空 topic / client_id 进入 init/start。
    //      （任何模式都成立；复用既有"串口 ERROR"表达，不新增错误处理体系）
    // =====================================================
    {
        const char* block_reason = nullptr;

#if GF_ENV_PRODUCTION
        if(config_get_mqtt_subscribe_topic().indexOf(GF_DEVICE_ID_PLACEHOLDER) < 0)
        {
            block_reason = "production policy: subscribe_topic lacks <device_id>";
        }
        else if(config_get_mqtt_publish_topic().indexOf(GF_DEVICE_ID_PLACEHOLDER) < 0)
        {
            block_reason = "production policy: publish_topic lacks <device_id>";
        }
        else if(config_get_mqtt_log_topic().indexOf(GF_DEVICE_ID_PLACEHOLDER) < 0)
        {
            block_reason = "production policy: log_topic lacks <device_id>";
        }
#endif

        if(block_reason == nullptr)
        {
            if(mqtt_sub_topic.length() == 0)
            {
                block_reason = "config error: subscribe_topic unresolved";
            }
            else if(mqtt_pub_topic.length() == 0)
            {
                block_reason = "config error: publish_topic unresolved";
            }
            else if(mqtt_client_id.length() == 0)
            {
                block_reason = "config error: client_id unresolved";
            }
        }

        if(block_reason != nullptr)
        {
            s_mqtt_precheck_failed = true;
            Serial.printf("[Identity] ERROR MQTT blocked (%s)\n", block_reason);
        }
    }

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

    // ---- P2-2：传输边界状态初始化 ----
    //   ★ 分片重组器绑定既有的 mqtt_large_rx_buffer（不新增缓冲）
    //   ★ 注册表 / foreign 队列清零（无动态分配）
    //   ★ s_foreign_cb 与 LWT 配置**不在此清零**：保留调用方（P2-3）已注入的设置
    s_rx_asm.init(mqtt_large_rx_buffer, CLOUD_MQTT_BUFFER_SIZE);
    s_foreign_subs.init();
    s_foreign_head  = 0;
    s_foreign_tail  = 0;
    s_foreign_count = 0;

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

        // ---- P2-D 埋点：LOG_MQTT_CONNECTED（INFO）----
        //
        // 边沿保证：本分支只由 mqtt_event_handler 的 MQTT_EVENT_CONNECTED 置位
        // 的 `mqtt_connect_pending` 触发，且进入即清零 ⇒ **一次连接只记一条**，
        // 事件重复到达不会重复发。
        // 落点选在这里（loop 上下文）而不是 MQTT 回调里：符合"集中在 *_task()
        // 调用 log_emit()"的约定，回调仍只做置标志。
        {
            LogParamIn p[3];
            p[0] =
                log_arg_u32(
                    LOG_P_OUTBOX,
                    (mqtt_client != nullptr)
                        ? (uint32_t)esp_mqtt_client_get_outbox_size(mqtt_client)
                        : 0u
                );
            p[1] =
                log_arg_enum(
                    LOG_P_WAS,
                    CLOUD_MQTT_STATE_OFFLINE
                );
            p[2] =
                log_arg_enum(
                    LOG_P_STATE,
                    CLOUD_MQTT_STATE_ONLINE
                );
            log_emit(
                LOG_MQTT_CONNECTED,
                LOG_LVL_INFO,
                p,
                3
            );
        }

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

        // ---- P2-D 埋点：LOG_MQTT_DISCONNECTED（WARN）----
        //
        // 边沿保证：同样只由回调置位的 `mqtt_disconnect_pending` 触发且进入即清零。
        // 底层 MQTT 抖动时 DISCONNECTED 可能连续上报，但标志是**单槽**的
        // ⇒ 每个 loop 至多一条，不会与重连计数一起放大成洪泛。
        {
            LogParamIn p[4];
            p[0] =
                log_arg_u32(
                    LOG_P_OUTBOX,
                    (mqtt_client != nullptr)
                        ? (uint32_t)esp_mqtt_client_get_outbox_size(mqtt_client)
                        : 0u
                );
            p[1] =
                log_arg_u32(
                    LOG_P_RETRY_N,
                    (uint32_t)mqtt_retry_count
                );
            p[2] =
                log_arg_enum(
                    LOG_P_WAS,
                    CLOUD_MQTT_STATE_ONLINE
                );
            p[3] =
                log_arg_enum(
                    LOG_P_STATE,
                    CLOUD_MQTT_STATE_OFFLINE
                );
            log_emit(
                LOG_MQTT_DISCONNECTED,
                LOG_LVL_WARN,
                p,
                4
            );
        }
    }
}

// =====================================================
// 云主循环任务
// =====================================================
// =====================================================
// Phase D-2：与 CredentialManager 的同步（**唯一交界**）
//
// 本函数做两件事，且**仅此两件**（冻结：Phase C Q9）：
//   A. 把 esp-mqtt 任务记录下的"连接结果"转交给 CredentialManager
//   B. 若期望身份发生变化 ⇒ 应用新身份并重建 MQTT 客户端
//
// ⚠️ 本函数**不判断** ok/reason 的语义、**不认识** generation / TESTING / ACTIVE，
//    也不推断"当前是不是在试连" —— 全部语义归 CredentialManager。
// =====================================================
// =====================================================
// ★ 凭据换连：先等 outbox 排空，再重建客户端
//
// 为什么需要「排空」：上行结果（含 `credential_confirm`）走
// `esp_mqtt_client_enqueue`（QoS1 + store）⇒ **入队成功 ≠ 已投递**。
// 若紧接着 stop+destroy，队列里那条确认帧会被一起销毁（D-3 上板实测）。
// =====================================================
static void cloud_apply_credential_switch()
{
    if (mqtt_client != nullptr)
    {
        esp_mqtt_client_stop(mqtt_client);
        esp_mqtt_client_destroy(mqtt_client);

        mqtt_client = nullptr;
        mqtt_client_active = false;
        mqtt_connected = false;
        mqtt_connect_pending = false;
        mqtt_disconnect_pending = false;

        state_set_bool(STATE_MQTT_STATUS, false);
    }
}

static void cloud_sync_credentials()
{
    // ---- A. 转交连接结果（在 loop 上下文做，允许对端写 NVS）----
    if (s_conn_result_pending)
    {
        const bool ok = s_conn_result_ok;
        const int reason = s_conn_result_reason;

        s_conn_result_pending = false;
        s_conn_result_ok = false;
        s_conn_result_reason = 0;

        credential_manager_notify_conn_result(ok, reason);

        // 换连窗口结束（结果已消费 ⇒ 后续断开按正常语义处理）
        s_swapping_credentials = false;
    }

    // ---- B0. 等待 outbox 排空（非阻塞，有界）----
    if (s_cred_switch_waiting)
    {
        const unsigned long now = millis();
        const int outbox = (mqtt_client != nullptr)
                               ? esp_mqtt_client_get_outbox_size(mqtt_client)
                               : 0;
        const bool min_ok = (now - s_cred_switch_armed_ms) >= CRED_SWITCH_DRAIN_MIN_MS;
        const bool expire = (int32_t)(now - s_cred_switch_deadline) >= 0;

        if ((outbox == 0 && min_ok) || expire)
        {
            Serial.printf("[Cloud] credential switch: outbox=%d%s -> rebuild\n",
                          outbox, expire ? " (drain timeout)" : "");
            s_cred_switch_waiting = false;
            cloud_apply_credential_switch();
        }
        return;   // 等待期间不做其它换连决策
    }

    // ---- B. 期望身份变化 ⇒ 换连 ----
    CredentialIdentity ident;

    if (!credential_manager_get_identity(ident))
    {
        return;   // 无凭据：保持配置兜底（P0 行为）
    }

    if (ident.epoch == s_cred_epoch_applied)
    {
        return;   // 未变化
    }

    s_cred_epoch_applied = ident.epoch;

    mqtt_username = ident.username;
    mqtt_password = ident.password;

    // client_id 模板来自 CredentialManager（对这里是不透明字符串）；
    // 渲染仍然走工程内**唯一**渲染点（T-2）
    String rendered = cloud_render_client_id(String(ident.client_id_template));

    if (rendered.length() > 0)
    {
        mqtt_client_id = rendered;
    }

    Serial.printf(
        "[Cloud] credential epoch=%lu applied (client_id=%s)\n",
        (unsigned long)ident.epoch,
        mqtt_client_id.c_str());

    // 重建客户端：stop + destroy ⇒ 由既有重连逻辑用新身份重连。
    // （不是 ESP.restart()，不触碰 Critical Operation 契约）
    if (mqtt_client != nullptr)
    {
        s_swapping_credentials = true;

        // ★ 先排空 outbox（避免丢弃刚入队的上行，如 credential_confirm）
        const unsigned long now = millis();
        s_cred_switch_armed_ms = now;
        s_cred_switch_deadline = now + CRED_SWITCH_DRAIN_MAX_MS;
        s_cred_switch_waiting = true;
    }
}

void cloud_task()
{
    // P2-D：发布失败的**周期聚合上报**
    //
    // ⚠️ 必须在任何早期 return **之前**：离线时最需要这条记录，而离线恰恰是
    //    下面 `!wifi_connected` 直接 return 的情形。窗口内无失败 ⇒ 完全静默。
    cloud_report_publish_fail();

    // Phase D-2：凭据结果转交 + 期望身份变化时的换连
    // ⚠️ 同样放在 `!wifi_connected` 之前：离线时也需要把失败结果转交，
    //    否则凭据验证只能等超时。
    cloud_sync_credentials();

    // ---- P2-2：foreign 消息交付（**loop 上下文**）----
    // ⚠️ 同样放在 `!wifi_connected` 之前：即便此刻已断开，也应把断开前入队的
    //    foreign 消息交付给 handler（不得在 esp-mqtt 任务内执行，AD-5a）。
    cloud_transport_process_foreign();

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

        // ---- P2-D 埋点：LOG_MQTT_SLEEP_ENTER（ERROR）----
        //
        // 落点天然边沿：进入本分支前已由上面的 `if(mqtt_sleep_mode) { ... return; }`
        // 排除"已在休眠"的情形 ⇒ **一次连续失败只记一条**，休眠期间不再重复。
        {
            const unsigned long sleep_time_for_log =
                (sleep_retry_interval > 0)
                    ? sleep_retry_interval
                    : (unsigned long)MQTT_FAIL_SLEEP_TIME;
            LogParamIn p[2];
            p[0] =
                log_arg_u32(
                    LOG_P_RETRY_N,
                    (uint32_t)mqtt_retry_count
                );
            p[1] =
                log_arg_u32(
                    LOG_P_SLEEP_MS,
                    (uint32_t)sleep_time_for_log
                );
            log_emit(
                LOG_MQTT_SLEEP_ENTER,
                LOG_LVL_ERROR,
                p,
                2
            );
        }

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
        mqtt_pub_topic.c_str(),
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
        mqtt_pub_topic.c_str(),
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
// P1.4：Log Topic 上行（独立于 up / down）
//
// - 复用现有 cloud_mqtt_publish_binary()，不改动它
// - P3 · WP4-FW-LOG：载荷 = **Cloud Log Envelope JSON**（逐条，count=1）；
//   本层**不解释**载荷（不解析 JSON / 不解码 CBOR）—— 只做 transport
// - 离线时返回 false：保留记录 / 退避重试由 LogManager 负责
//   （CloudManager 不为 Log 建立任何 TX 队列）
// - 不做字段压缩、不分片：上界 LOG_BATCH_MAX_PAYLOAD = 4096 B
// =====================================================
bool cloud_send_log(const uint8_t* data, size_t length)
{
    if(data == nullptr || length == 0)
    {
        return false;
    }

    if(mqtt_log_topic.length() == 0)
    {
        // P0-3：兜底改为**渲染后的 V3 模板**（与 cloud_init() 同源）
        mqtt_log_topic = cloud_render_topic(String(GF_TOPIC_TPL_LOG),
                                            GF_TOPIC_TPL_LOG, "log_topic");

        if(mqtt_log_topic.length() == 0)
        {
            Serial.println("[Cloud LOG] no valid log topic, drop");
            return false;
        }
    }

    if(mqtt_client == nullptr || !mqtt_connected)
    {
        Serial.println("[Cloud LOG] MQTT offline");
        return false;
    }

    const bool ok = cloud_mqtt_publish_binary(
        mqtt_log_topic.c_str(),
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

// =====================================================
// P3 · WP4-FW-TELEMETRY：Telemetry 独立上行 transport（纯新增）
//
// - **不复用 command uplink**：不调用 cloud_compress_uplink()，不包成
//   {"cmd":"result",…}（H-WP4C-02）
// - 本层**不解释**载荷：只当不透明文本发送（不知道 type 语义）
// - Topic = mqtt_pub_topic（guo_feeder/<device_id>/up）—— **topic 零改动**
// - 不排队 / 不落盘 / 不重试（fire-and-forget；与 telemetry 语义一致）
// - 复用现有 cloud_mqtt_publish_binary()，不改动它
// =====================================================
bool cloud_send_telemetry(const char* json)
{
    if(json == nullptr || json[0] == '\0')
    {
        return false;
    }

    if(mqtt_client == nullptr || !mqtt_connected)
    {
        return false;
    }

    if(mqtt_pub_topic.length() == 0)
    {
        // P0-3：兜底改为**渲染后的 V3 模板**（与 cloud_init() 同源）
        mqtt_pub_topic = cloud_render_topic(String(GF_TOPIC_TPL_UP),
                                            GF_TOPIC_TPL_UP, "publish_topic");

        if(mqtt_pub_topic.length() == 0)
        {
            Serial.println("[Cloud TELEM] no valid up topic, drop");
            return false;
        }
    }

    return cloud_mqtt_publish_binary(
        mqtt_pub_topic.c_str(),
        (const uint8_t*)json,
        strlen(json),
        1);
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

