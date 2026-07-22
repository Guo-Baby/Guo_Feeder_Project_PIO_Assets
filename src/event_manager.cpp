#include <Arduino.h>

#include "event_manager.h"

// =====================================================
// 事件缓存
//
// 当前版本：
// 使用RAM数组保存
//
// 后续RTOS：
// 可以替换为QueueHandle_t
//
// =====================================================

#define EVENT_BUFFER_SIZE 16

static SystemEvent event_buffer
[
    EVENT_BUFFER_SIZE
];

static uint8_t event_count = 0;

// =====================================================
// 初始化
// =====================================================

void event_manager_init()
{
    event_count = 0;

    for(
        int i=0;
        i<EVENT_BUFFER_SIZE;
        i++
    )
    {
        event_buffer[i]
        =
        EVENT_NONE;
    }

    Serial.println(
        "Event manager init OK"
    );
}

// =====================================================
// 添加事件
// =====================================================

void event_push(
    SystemEvent event
)
{
    if(
        event == EVENT_NONE
    )
    {
        return;
    }

    // 防止重复塞满

    for(
        int i=0;
        i<event_count;
        i++
    )
    {
        if(
            event_buffer[i]
            ==
            event
        )
        {
            return;
        }
    }

    if(
        event_count
        <
        EVENT_BUFFER_SIZE
    )
    {
        event_buffer[event_count]
        =
        event;

        event_count++;
    }
}

// =====================================================
// 查询事件
// =====================================================

bool event_available(
    SystemEvent event
)
{
    for(
        int i=0;
        i<event_count;
        i++
    )
    {
        if(
            event_buffer[i]
            ==
            event
        )
        {
            return true;
        }
    }

    return false;
}

// =====================================================
// 获取并删除事件
// =====================================================

bool event_get(
    SystemEvent event
)
{
    for(
        int i=0;
        i<event_count;
        i++
    )
    {
        if(
            event_buffer[i]
            ==
            event
        )
        {
            // 后面的往前移动

            for(
                int j=i;
                j<event_count-1;
                j++
            )
            {
                event_buffer[j]
                =
                event_buffer[j+1];
            }

            event_count--;

            return true;
        }
    }

    return false;
}

// =====================================================
// 清除事件
// =====================================================

void event_clear(
    SystemEvent event
)
{
    event_get(event);
}