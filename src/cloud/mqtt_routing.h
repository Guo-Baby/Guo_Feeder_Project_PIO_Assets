#pragma once

// =====================================================
// P2-2 · MQTT 传输边界 —— 纯逻辑（**零依赖**）
//
// 本头文件只包含可离线求值的纯逻辑，供两处共用（**单一真源**）：
//   · src/cloud/cloud_manager.cpp   （生产路径：topic 分派 / 分片重组 / 订阅注册表）
//   · test/mqtt_transport_host/*    （宿主 harness：在 PC 上真实执行这些语义）
//
// 内容：
//   ① MQTT topic filter 匹配（'+' 单层 / '#' 多层）
//   ② 订阅注册表 MqttSubRegistry（**有限容量、幂等**，无动态分配）
//   ③ 下行分片重组状态机 MqttRxAssembler（**缓冲区由调用方提供**，不分配）
//   ④ 统一的 topic 分派 mqtt_route_topic()
//
// ★ 不依赖 libc / Arduino / ESP-IDF：自带最小字符串工具，
//   以免 host（wasm32 freestanding）侧缺少 C 运行库。
// ★ 无任何全局状态：所有状态都在 struct 实例内（由调用方持有）。
// =====================================================

#include <stddef.h>
#include <stdint.h>

// ---- 容量上限（有限、明确、可检查） ----
#define MQTT_ROUTE_MAX_FOREIGN   4      // 除业务 topic 外，最多注册的 foreign filter 数
#define MQTT_ROUTE_FILTER_MAX    96     // 单个 filter 最大字符数（超限 ⇒ 注册失败，不静默截断）
#define MQTT_ROUTE_TOPIC_MAX     128    // 单条 topic 最大字符数（超限按上限截断记录）

// =====================================================
// 最小字符串工具（无 libc）
// =====================================================
inline size_t mqtt_cstr_len(const char* s)
{
    size_t n = 0;
    if(s != NULL) { while(s[n] != '\0') ++n; }
    return n;
}

inline bool mqtt_cstr_eq(const char* a, const char* b)
{
    if(a == NULL || b == NULL) { return a == b; }
    size_t i = 0;
    for(; a[i] != '\0' && b[i] != '\0'; ++i) { if(a[i] != b[i]) { return false; } }
    return a[i] == b[i];
}

// 复制至多 cap-1 字节并补 '\0'（**不做越界**；src 超长则截断）
inline void mqtt_cstr_copy(char* dst, size_t cap, const char* src)
{
    if(dst == NULL || cap == 0) { return; }
    size_t i = 0;
    if(src != NULL) { for(; src[i] != '\0' && (i + 1) < cap; ++i) { dst[i] = src[i]; } }
    dst[i] = '\0';
}

// =====================================================
// ① MQTT topic filter 匹配
//
//   '+' 匹配**恰好一层**（可空层，如 "a//b" 中的空层）
//   '#' 匹配**剩余所有层（含零层）**，且必须是 filter 的最后一级
//   filter == NULL / topic == NULL  ⇒ false
//
// 采用"逐层切分"实现（对 '+'/'#'/字面量统一处理），避免手写下标状态机出错。
// =====================================================
inline bool mqtt_topic_matches_filter(const char* filter, const char* topic)
{
    if(filter == NULL || topic == NULL) { return false; }

    const char* f = filter;
    const char* t = topic;

    for(;;)
    {
        // 当前 filter 层
        const char* f_end = f;
        while(f_end[0] != '\0' && f_end[0] != '/') { ++f_end; }
        const size_t f_len = (size_t)(f_end - f);

        // 当前 topic 层
        const char* t_end = t;
        while(t_end[0] != '\0' && t_end[0] != '/') { ++t_end; }
        const size_t t_len = (size_t)(t_end - t);

        const bool f_last = (f_end[0] == '\0');
        const bool t_last = (t_end[0] == '\0');

        if(f_len == 1 && f[0] == '#')
        {
            // '#' 匹配剩余全部（含 0 层）⇒ 立刻成功
            return true;
        }

        if(f_len == 1 && f[0] == '+')
        {
            // 通配一层：无需比较内容（空层亦算一层）
        }
        else
        {
            if(f_len != t_len) { return false; }
            for(size_t i = 0; i < f_len; ++i)
            {
                if(f[i] != t[i]) { return false; }
            }
        }

        if(f_last && t_last) { return true; }
        if(f_last)           { return false; }   // filter 用尽，topic 还有剩余层

        if(t_last)
        {
            // topic 用尽：仅当剩余 filter 恰为 "/#"（匹配父级）时成立
            return (f_end[0] == '/' && f_end[1] == '#' && f_end[2] == '\0');
        }

        // 双双还有剩余层 ⇒ 越过 '/'
        f = f_end + 1;
        t = t_end + 1;
    }
}

// =====================================================
// ② 订阅注册表（foreign filter；有限容量 + 幂等）
//
//   业务 topic **不在此表**：业务订阅沿用 CloudManager 既有订阅调用（逐字不变），
//   业务 topic 的"是否属于业务"由 mqtt_route_topic() 直接用 business_filter 判定。
//   本表只登记"非业务（foreign）"filter，供 CONNECTED replay 与 topic 分派使用。
// =====================================================
struct MqttSubRegistry
{
    struct Entry
    {
        char filter[MQTT_ROUTE_FILTER_MAX];
        int  qos;
    };

    Entry entries[MQTT_ROUTE_MAX_FOREIGN];
    int   count;

    void init()
    {
        count = 0;
        for(int i = 0; i < MQTT_ROUTE_MAX_FOREIGN; ++i)
        {
            entries[i].filter[0] = '\0';
            entries[i].qos = 0;
        }
    }

    int find(const char* filter) const
    {
        if(filter == NULL) { return -1; }
        for(int i = 0; i < count; ++i)
        {
            if(mqtt_cstr_eq(entries[i].filter, filter)) { return i; }
        }
        return -1;
    }

    // 幂等：已存在 ⇒ 返回 true 且**不新增条目**（保留首次注册的 qos）
    //         超长 / 满员 ⇒ 返回 false
    bool add(const char* filter, int qos)
    {
        if(filter == NULL || filter[0] == '\0') { return false; }
        if(mqtt_cstr_len(filter) >= MQTT_ROUTE_FILTER_MAX)  { return false; }  // 不静默截断
        if(find(filter) >= 0) { return true; }                                  // 幂等
        if(count >= MQTT_ROUTE_MAX_FOREIGN) { return false; }                   // 有限容量
        mqtt_cstr_copy(entries[count].filter, MQTT_ROUTE_FILTER_MAX, filter);
        entries[count].qos = qos;
        ++count;
        return true;
    }

    // 返回匹配的 foreign 下标；无匹配 ⇒ -1
    int route(const char* topic) const
    {
        if(topic == NULL) { return -1; }
        for(int i = 0; i < count; ++i)
        {
            if(mqtt_topic_matches_filter(entries[i].filter, topic)) { return i; }
        }
        return -1;
    }
};

// =====================================================
// ④ 统一 topic 分派
//
//   优先级：业务 > foreign > 丢弃（确定性；业务永不因 wildcard 误判为 foreign）
// =====================================================
enum MqttRouteKind
{
    MQTT_ROUTE_DROP     = 0,   // 未注册 topic ⇒ 既不入业务管线，也不交 foreign
    MQTT_ROUTE_BUSINESS = 1,   // ⇒ cloud_rx_enqueue()（既有业务路径）
    MQTT_ROUTE_FOREIGN  = 2    // ⇒ foreign 队列（loop 上下文交付）
};

inline MqttRouteKind mqtt_route_topic(const char* topic,
                                      const char* business_filter,
                                      const MqttSubRegistry& foreign,
                                      int* out_foreign_index = NULL)
{
    if(out_foreign_index != NULL) { *out_foreign_index = -1; }
    if(topic == NULL) { return MQTT_ROUTE_DROP; }

    if(business_filter != NULL && business_filter[0] != '\0'
       && mqtt_topic_matches_filter(business_filter, topic))
    {
        return MQTT_ROUTE_BUSINESS;
    }

    const int idx = foreign.route(topic);
    if(idx >= 0)
    {
        if(out_foreign_index != NULL) { *out_foreign_index = idx; }
        return MQTT_ROUTE_FOREIGN;
    }

    return MQTT_ROUTE_DROP;
}

// =====================================================
// ③ 下行分片重组状态机
//
//   背景（esp-mqtt 契约，见 mqtt_client.h MQTT_EVENT_DATA 注）：
//     一条消息若长于内部缓冲，会拆成多次 MQTT_EVENT_DATA；
//     **只有第一片携带 topic**，其余片只有 data / data_len / current_data_offset。
//   因此：topic 在 current_data_offset == 0 时捕获。
//
//   设计约束（对齐现有实现，保证业务路径行为等价）：
//     · 单例式状态由调用方持有（struct 实例），缓冲区由调用方提供（不分配内存）
//     · 以 current_data_offset 作为写入位置，累计 data_len 判断完成
//     · total_data_len > cap ⇒ 丢弃（不越界）
//     · 完成后调用方读取 data()/len()/topic_cstr()/retain_flag()，再自行 reset()
//
//   feed() 返回：
//     0 = 尚未完成
//     1 = 已完成（读 data()/len()/topic()/retain()）
//     2 = 因超限/越界而丢弃（内部已 reset）
// =====================================================
struct MqttRxAssembler
{
    uint8_t* buf;         // 调用方提供的重组缓冲
    size_t   cap;         // 缓冲容量

    size_t   total;       // 本条消息总长（首片给定）
    size_t   offset;      // 已写入字节数（累计 data_len）
    bool     active;      // 是否正在重组
    bool     retain;      // 本条消息的 retain 标志

    char     topic[MQTT_ROUTE_TOPIC_MAX];
    size_t   topic_len;

    void init(uint8_t* buffer, size_t capacity)
    {
        buf = buffer;
        cap = capacity;
        reset();
    }

    void reset()
    {
        total = 0;
        offset = 0;
        active = false;
        retain = false;
        topic[0] = '\0';
        topic_len = 0;
    }

    void capture_topic(const char* tpc, int tpc_len)
    {
        if(tpc == NULL || tpc_len <= 0) { return; }
        size_t n = (size_t)tpc_len;
        if(n >= MQTT_ROUTE_TOPIC_MAX) { n = MQTT_ROUTE_TOPIC_MAX - 1; }
        for(size_t i = 0; i < n; ++i) { topic[i] = tpc[i]; }
        topic[n] = '\0';
        topic_len = n;
    }

    int feed(int current_offset,
             int total_len,
             const char* tpc,
             int tpc_len,
             bool ret,
             const uint8_t* data,
             int data_len)
    {
        if(data == NULL || data_len <= 0) { return 0; }

        if(current_offset == 0)
        {
            // 新消息：先判容量，再登记
            if((size_t)total_len > cap)
            {
                reset();
                return 2;   // 超限 ⇒ 丢弃（与既有实现一致）
            }
            total  = (size_t)total_len;
            offset = 0;
            active = true;
            retain = ret;
            topic_len = 0;
            topic[0] = '\0';
            capture_topic(tpc, tpc_len);
        }
        else if(topic_len == 0)
        {
            // 防御：契约保证 topic 只在首片；若后续片意外携带则补捕获
            capture_topic(tpc, tpc_len);
        }

        if(!active) { return 0; }

        // 越界防御（正常路径下 current_offset + data_len <= total <= cap）
        if(((size_t)current_offset + (size_t)data_len) > cap)
        {
            reset();
            return 2;
        }

        for(int i = 0; i < data_len; ++i)
        {
            buf[(size_t)current_offset + (size_t)i] = data[i];
        }
        offset += (size_t)data_len;

        if(offset >= total)
        {
            return 1;   // 完成；调用方读完即 reset()
        }
        return 0;
    }

    const uint8_t* data()       const { return buf; }
    size_t         len()        const { return total; }
    const char*    topic_cstr() const { return topic; }
    bool           retain_flag()const { return retain; }
};
