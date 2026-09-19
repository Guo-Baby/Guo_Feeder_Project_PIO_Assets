#include <Arduino.h>
#include "event_manager.h"

#include "log_manager.h"   // P2-M：观测埋点（EventId / ParamId + log_emit）

#define EVENT_QUEUE_SIZE     32
#define MAX_EVENT_TYPE       32
#define MAX_EVENT_SUBSCRIBER 8

// ====================== 事件风暴限流配置（按独立事件统计） ======================
#define EVENT_STORM_WINDOW_MS    500
#define EVENT_STORM_MAX_PER_EVENT 5

// 每个事件独立风暴窗口统计
static unsigned long storm_window_tick[MAX_EVENT_TYPE] = {0};
static int storm_counter[MAX_EVENT_TYPE] = {0};

// =====================================================
// P2-M：EventManager 自身异常观测
//
// ★ 职责边界：EventManager 是**传输 / 分发基础设施**，不是事实源。
//   业务事件（EVENT_WIFI_* / EVENT_TIME_* / EVENT_CLOUD_* /
//   EVENT_WEIGHT_ERROR / EVENT_VALVE_* 等）由各自的**发布方模块**记录
//   —— 实测 17 个 event_push 调用点中 13 个发布方已埋 log_emit。
//   在此逐条转发业务事件会造成**同一事实重复记录** ⇒ 一律不记。
//
//   本模块**只记录自身运行异常**：
//     A. 队列溢出（无法接受新事件）  → LOG_EVT_QUEUE_FULL
//     B. 风暴抑制触发（事件被丢弃）  → LOG_EVT_STORM_DROPPED
//
// ★ 为什么必须由本模块记录：17 个调用点 **100% 忽略 event_push()
//   返回值**，且 event_get_drop_count() / duplicate_count() /
//   queue_count() 三个 getter **零消费方** ⇒ 丢弃事件在此之前
//   **完全不可观测**：发布方看不到、外部查不到。
//
// ★ 为什么必须聚合：风暴的定义就是"超过 5 次 / 500ms 的持续流"，
//   逐条记录会形成**新的日志风暴**并瞬间填满 64 槽 RAM 环。
//   故采用「只累加 + 周期窗口上报 1 条」：
//     Σ LOG_P_COUNT = 真实被丢弃数量（信息零丢失，条目数恒定）
//   与 cloud_manager.cpp 的 CLOUD_PUBLISH_FAIL_REPORT_MS 同构。
//
// ★ 节拍器：event_dispatch() 由 loop() 每轮无条件调用 ⇒ 直接复用，
//   **不新增 Task / 不修改 main.cpp / 不新建 timer / 无 delay / 无 while 等待**。
//
// ★ 为什么累加器用模块内 static 而不是栈上：
//     · 累加器必须跨调用存活 ⇒ 不能是栈上局部；
//     · 参数（LogParamIn）才是栈上局部，且只在 evt_report_faults()
//       内构造 —— 该函数**只**由 event_dispatch()（loop 上下文）调用，
//       不存在 Registry 那种"可能跑在 esp-mqtt 任务"的问题 ⇒ 无需 PSRAM。
//
// ★ 跨任务可见性（诚实标注）：cloud_manager 的 4 处 event_push 可能跑在
//   esp-mqtt 任务 ⇒ 累加器存在理论上的无锁竞争（可能少计，不会损坏内存）。
//   这与既有 drop_count / duplicate_count **完全相同**（本就是无锁全局），
//   不额外加锁以免改变事件路径时序 —— 属"观测精度"而非"正确性"风险。
// =====================================================
#define EVT_FAULT_REPORT_WINDOW_MS  60000u

static uint32_t evt_queue_full_drops = 0;   // A. 队列溢出被丢弃条数
static uint32_t evt_storm_drops      = 0;   // B. 风暴抑制被丢弃条数
static unsigned long evt_fault_report_ms = 0;

// 只累加，不发射（由 evt_report_faults() 周期上报）
static void evt_note_queue_full()
{
    if (evt_queue_full_drops < 0xFFFFFFFFu) evt_queue_full_drops++;
}

static void evt_note_storm_drop()
{
    if (evt_storm_drops < 0xFFFFFFFFu) evt_storm_drops++;
}

// 周期窗口到 ⇒ 各上报 1 条；窗口内无异常 ⇒ 完全静默
static void evt_report_faults()
{
    // 无异常 ⇒ 完全静默，且**不推进窗口**
    // （保证"首次异常立即上报"，而非等满 60 s 才可见）
    if (evt_queue_full_drops == 0 && evt_storm_drops == 0) return;

    const unsigned long now = millis();
    if (evt_fault_report_ms != 0 &&
        (now - evt_fault_report_ms) < EVT_FAULT_REPORT_WINDOW_MS)
    {
        return;
    }
    evt_fault_report_ms = now;

    if (evt_queue_full_drops > 0)
    {
        // LOG_P_COUNT = 本窗口被丢弃的条数；LOG_P_QUEUE_SIZE = 队列容量
        LogParamIn p[2];
        p[0] = log_arg_u32(LOG_P_COUNT,      evt_queue_full_drops);
        p[1] = log_arg_u32(LOG_P_QUEUE_SIZE, (uint32_t)EVENT_QUEUE_SIZE);
        log_emit(LOG_EVT_QUEUE_FULL, LOG_LVL_WARN, p, 2);
        evt_queue_full_drops = 0;
    }

    if (evt_storm_drops > 0)
    {
        LogParamIn p[1];
        p[0] = log_arg_u32(LOG_P_COUNT, evt_storm_drops);
        log_emit(LOG_EVT_STORM_DROPPED, LOG_LVL_WARN, p, 1);
        evt_storm_drops = 0;
    }
}

// ==========================
// 队列
// ==========================
static EventMessage event_queue[EVENT_QUEUE_SIZE];
static int queue_count = 0;

// ==========================
// 二维监听表
// ==========================
static EventCallback callback_table[MAX_EVENT_TYPE][MAX_EVENT_SUBSCRIBER];
static int sub_count[MAX_EVENT_TYPE];

// ==========================
// 统计
// ==========================
static unsigned long drop_count = 0;
static unsigned long duplicate_count = 0;

// =====================================================
// 字符串 → 事件枚举
// =====================================================
SystemEvent event_from_string(const String &str)
{
    // 去掉 "event." 前缀（如果存在）
    String name = str;
    if (name.startsWith("event.")) {
        name = name.substring(6);
    }
    
    // 转换为大写
    name.toUpperCase();
    
    // 构建完整事件名
    String full_name = "EVENT_" + name;
    
    // 定义事件名称映射表（与 SystemEvent 枚举顺序一致）
    static const char* event_names[] = {
        "EVENT_NONE",
        "EVENT_WIFI_CONNECTED",
        "EVENT_WIFI_DISCONNECTED",
        "EVENT_NTP_SYNC_OK",
        "EVENT_TIME_VALID",
        "EVENT_TIME_INVALID",
        "EVENT_CONFIG_CHANGED",
        "EVENT_CLOUD_CONNECTED",
        "EVENT_CLOUD_DISCONNECTED",
        "EVENT_CLOUD_UPLOAD",
        "EVENT_CLOUD_COMMAND",
        "EVENT_COMMAND_RESULT",
        "EVENT_WEIGHT_READY",
        "EVENT_WEIGHT_ERROR",
        "EVENT_ERROR"
    };
    
    for (int i = 0; i < SYSTEM_EVENT_COUNT; i++) {
        if (full_name == event_names[i]) {
            return (SystemEvent)i;
        }
    }
    
    return EVENT_NONE;
}

// =====================================================
// 事件枚举 → 字符串
// =====================================================
String event_to_string(SystemEvent event)
{
    int idx = (int)event;
    if (idx < 0 || idx >= SYSTEM_EVENT_COUNT) {
        return "EVENT_UNKNOWN";
    }
    
    static const char* event_names[] = {
        "EVENT_NONE",
        "EVENT_WIFI_CONNECTED",
        "EVENT_WIFI_DISCONNECTED",
        "EVENT_NTP_SYNC_OK",
        "EVENT_TIME_VALID",
        "EVENT_TIME_INVALID",
        "EVENT_CONFIG_CHANGED",
        "EVENT_CLOUD_CONNECTED",
        "EVENT_CLOUD_DISCONNECTED",
        "EVENT_CLOUD_UPLOAD",
        "EVENT_CLOUD_COMMAND",
        "EVENT_COMMAND_RESULT",
        "EVENT_WEIGHT_READY",
        "EVENT_WEIGHT_ERROR",


        //新增的枚举字符串按顺序放在上方
        "EVENT_ERROR"
    };
    
    return String(event_names[idx]);
}



void event_manager_init()
{
    queue_count = 0;
    drop_count = 0;
    duplicate_count = 0;

    // 初始化监听表
    for (int e = 0; e < MAX_EVENT_TYPE; e++)
    {
        sub_count[e] = 0;
        storm_window_tick[e] = millis();
        storm_counter[e] = 0;
        for (int s = 0; s < MAX_EVENT_SUBSCRIBER; s++)
        {
            callback_table[e][s] = nullptr;
        }
    }

    Serial.println("Event manager init OK");
}

// =================================================
// 删除指定位置事件
// =================================================
static void remove_event(int index)
{
    for (int i = index; i < queue_count - 1; i++)
    {
        event_queue[i] = event_queue[i + 1];
    }
    queue_count--;
}

// =================================================
// DEDUP：仅匹配event枚举
// =================================================
static int find_same_event(SystemEvent event)
{
    for (int i = 0; i < queue_count; i++)
    {
        if (event_queue[i].event == event)
        {
            return i;
        }
    }
    return -1;
}

// =================================================
// 寻找队列最低优先级
// =================================================
static int find_lowest_priority()
{
    int index = 0;
    for (int i = 1; i < queue_count; i++)
    {
        if (event_queue[i].priority < event_queue[index].priority)
        {
            index = i;
        }
    }
    return index;
}

// =================================================
// 寻找队列最高优先级
// =================================================
static int find_highest_priority()
{
    int index = 0;
    for (int i = 1; i < queue_count; i++)
    {
        if (event_queue[i].priority > event_queue[index].priority)
        {
            index = i;
        }
    }
    return index;
}

// =================================================
// event_push
// =================================================
EventPushResult event_push(SystemEvent event,String data,String source,int priority,EventPolicy policy,unsigned long expire)
{
    int eid = static_cast<int>(event);
    unsigned long now = millis();
    
    // event id越界保护
    if(eid <0 || eid >= MAX_EVENT_TYPE)
    {
        return EVENT_DROPPED;
    }

    // ========== 事件风暴抑制：FORCE事件不受限流限制 ==========
    if (policy != EVENT_POLICY_FORCE)
    {
        // 窗口刷新
        if (now - storm_window_tick[eid] > EVENT_STORM_WINDOW_MS)
        {
            storm_window_tick[eid] = now;
            storm_counter[eid] = 0;
        }
        storm_counter[eid]++;

        if (storm_counter[eid] > EVENT_STORM_MAX_PER_EVENT)
        {
            // P2-M：风暴抑制触发 —— **只累加**，由 event_dispatch() 周期聚合上报。
            // 绝不在此逐条 log_emit：本分支的定义就是"高频持续流"，
            // 逐条记录会形成新的日志风暴（也会重复记录业务模块已记的事实）。
            evt_note_storm_drop();
            drop_count++;
            return EVENT_DROPPED;
        }
    }

    // ====================== STATE 状态策略：清空队列所有同event旧消息 ======================
    if (policy == EVENT_POLICY_STATE)
    {
        int index;
        while ((index = find_same_event(event)) != -1)
        {
            remove_event(index);
        }
    }

    // ====================== DEDUP策略：队列不允许存在相同event枚举 ======================
    if (policy == EVENT_POLICY_DEDUP)
    {
        if (find_same_event(event) >= 0)
        {
            duplicate_count++;
            return EVENT_DUPLICATE;
        }
    }

    // ====================== 队列满处理 ======================
    if (queue_count >= EVENT_QUEUE_SIZE)
    {
        if (policy == EVENT_POLICY_FORCE)
        {
            int low_idx = find_lowest_priority();
            if (priority > event_queue[low_idx].priority)
            {
                remove_event(low_idx);
            }
            else
            {
                drop_count++;
                return EVENT_DROPPED;
            }
        }
        else
        {
            // P2-M：队列溢出 —— **只累加**，由 event_dispatch() 周期聚合上报。
            // 只在此分支累加（= 返回 EVENT_QUEUE_FULL 的唯一出口）：
            //   · 不在调用者记录（17 个调用点 100% 忽略返回值，看不到）
            //   · 不在 event_push() 入口记录（入口不等于丢弃）
            //   · FORCE 分支（上方 `priority > 最低优先级` 判否时的
            //     `return EVENT_DROPPED`）**不计入**：语义是"被更高优先级
            //     挤占"而非"队列溢出"，且实测 EVENT_POLICY_FORCE 零发布方
            //     ⇒ 当前为死路径。
            evt_note_queue_full();
            drop_count++;
            return EVENT_QUEUE_FULL;
        }
    }

    EventMessage msg;
    msg.event = event;
    msg.data = data;
    msg.source = source;
    msg.priority = priority;
    msg.policy = policy;
    msg.timestamp = millis();
    msg.expire = expire;

    event_queue[queue_count] = msg;
    queue_count++;

    return EVENT_PUSH_OK;
}

// =================================================
// 注册监听
// =================================================
bool event_subscribe(SystemEvent event,EventCallback callback)
{
    if (callback == nullptr)
        return false;

    int eid = static_cast<int>(event);
    if (eid < 0 || eid >= MAX_EVENT_TYPE)
    {
        return false;
    }

    // 防重复注册
    for (int i = 0; i < sub_count[eid]; i++)
    {
        if (callback_table[eid][i] == callback)
        {
            return false;
        }
    }

    if (sub_count[eid] >= MAX_EVENT_SUBSCRIBER)
    {
        return false;
    }

    callback_table[eid][sub_count[eid]] = callback;
    sub_count[eid]++;
    return true;
}

// =================================================
// dispatch 调度
// 单次最多连续处理 4 条事件，防止长时间阻塞主循环
// =================================================
void event_dispatch()
{
    // P2-M 节拍器：必须放在**函数最开头、while 之前**。
    // event_dispatch() 由 loop() 每轮无条件调用 ⇒ 天然周期节拍，
    // 不新增 Task / 不改 main.cpp / 不新建 timer / 无 delay / 无 while 等待。
    // 放在 while 之后会被 processed 上限与"队列空则直接跳过"吞掉节拍。
    evt_report_faults();

    const int MAX_PROCESS_PER_DISPATCH = 4;
    int processed = 0;

    while (queue_count > 0 && processed < MAX_PROCESS_PER_DISPATCH)
    {
        int index = find_highest_priority();
        const EventMessage &msg = event_queue[index];

        bool is_expired = false;
        if (msg.expire > 0)
        {
            if (millis() - msg.timestamp > msg.expire)
            {
                is_expired = true;
            }
        }
        EventMessage dispatch_msg = msg;

        remove_event(index);

        if (is_expired)
        {
            drop_count++;
            continue;
        }

        int eid = static_cast<int>(dispatch_msg.event);
        if (eid >= 0 && eid < MAX_EVENT_TYPE)
        {
            for (int i = 0; i < sub_count[eid]; i++)
            {
                EventCallback cb = callback_table[eid][i];
                if (cb != nullptr)
                {
                    cb(dispatch_msg);
                }
            }
        }

        processed++;
    }
}

unsigned long event_get_drop_count()
{
    return drop_count;
}

unsigned long event_get_duplicate_count()
{
    return duplicate_count;
}

int event_get_queue_count()
{
    return queue_count;
}