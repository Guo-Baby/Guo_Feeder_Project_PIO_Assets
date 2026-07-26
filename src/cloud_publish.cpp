#include <Arduino.h>

#include "cloud_publish.h"
#include "cloud_manager.h"
#include "system_state.h"


// ==========================
// 发布普通消息
// ==========================

bool cloud_publish_message(
    const char* message
)

{

    if(message == nullptr)
    {
        Serial.println(
            "Publish null message"
        );

        return false;
    }



    return cloud_send_raw(
        message
    );

}




// ==========================
// 发布状态JSON
// 目前简单版本
// 后续接JsonDocument
// ==========================

bool cloud_publish_status()

{


    String json;


    json =
    "{";

    json +=
    "\"event\":\"status\"";

    json +=
    "}";



    return cloud_send_raw(
        json.c_str()
    );

}
