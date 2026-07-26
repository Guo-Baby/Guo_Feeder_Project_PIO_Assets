#include <Arduino.h>

#include "cloud_receive.h"
#include "event_manager.h"


// 保存最近一次命令

static String last_command;



void cloud_receive_init()
{

    Serial.println(
        "Cloud receive init..."
    );

}



// MQTT收到消息调用

void cloud_receive_handle(
    char* topic,
    String message
)
{


    if(
        message.length()==0
    )
    {
        Serial.println(
            "Empty command"
        );

        return;
    }


    Serial.println(
        "Topic:"
    );

    Serial.println(
        topic
    );


    Serial.println(
        "Cloud command:"
    );


    Serial.println(
        message
    );



    // 保存

    last_command =
        message;



    // 发布事件

    event_push(
        EVENT_CLOUD_COMMAND,
        message,
        "mqtt"
    );  


}



// 给command_manager读取

String cloud_receive_get_command()
{

    return last_command;

}