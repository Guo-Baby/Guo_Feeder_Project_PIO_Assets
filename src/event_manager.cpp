#include <Arduino.h>
#include "event_manager.h"

#define EVENT_QUEUE_SIZE     32
#define MAX_EVENT_TYPE       32
#define MAX_EVENT_SUBSCRIBER 8

// ====================== 事件风暴限流配置（按独立事件统计） ======================
#define EVENT_STORM_WINDOW_MS    500
#define EVENT_STORM_MAX_PER_EVENT 5

// 每个事件独立风暴窗口统计
static unsigned long storm_window_tick[MAX_EVENT_TYPE] = {0};
static int storm_counter[MAX_EVENT_TYPE] = {0};

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
EventPushResult event_push(
    SystemEvent event,
    String data,
    String source,
    int priority,
    EventPolicy policy,
    unsigned long expire
)
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
bool event_subscribe(
    SystemEvent event,
    EventCallback callback
)
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
    const int MAX_PROCESS_PER_DISPATCH = 4;
    int processed = 0;

    while (queue_count > 0 && processed < MAX_PROCESS_PER_DISPATCH)
    {
        int index = find_highest_priority();
        EventMessage msg = event_queue[index];

        bool is_expired = false;
        if (msg.expire > 0)
        {
            if (millis() - msg.timestamp > msg.expire)
            {
                is_expired = true;
            }
        }

        remove_event(index);

        if (is_expired)
        {
            drop_count++;
            continue;
        }

        int eid = static_cast<int>(msg.event);
        if (eid >= 0 && eid < MAX_EVENT_TYPE)
        {
            for (int i = 0; i < sub_count[eid]; i++)
            {
                EventCallback cb = callback_table[eid][i];
                if (cb != nullptr)
                {
                    cb(msg);
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