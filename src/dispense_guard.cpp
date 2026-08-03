#include <Arduino.h>
#include "dispense_guard.h"
#include "event_manager.h"
#include "valve.h"

// =====================================================
// Dispense Safety Guard
//
// 职责:
//   监听重量异常
//   紧急关闭阀门
//
// 不参与:
//   workflow状态
//   command结果
//   weight计算
//
// =====================================================

// =====================================================
// Event callback
// =====================================================
static void dispense_guard_event_callback(
    const EventMessage &msg
)
{
    if(
        msg.event != EVENT_WEIGHT_ERROR
    )
    {
        return;
    }
    Serial.println(
        "[DispenseGuard] Weight error received"
    );
    bool result =
        valve_force_close();
    if(result)
    {
        Serial.println(
            "[DispenseGuard] Valve force closed"
        );
    }
    else
    {
        Serial.println(
            "[DispenseGuard] Valve force close failed"
        );
    }
}

// =====================================================
// Init
// =====================================================
void dispense_guard_init()
{
    bool result =
        event_subscribe(
            EVENT_WEIGHT_ERROR,
            dispense_guard_event_callback
        );
    if(result)
    {
        Serial.println(
            "[DispenseGuard] Init OK"
        );
    }
    else
    {
        Serial.println(
            "[DispenseGuard] Event subscribe failed"
        );
    }
}