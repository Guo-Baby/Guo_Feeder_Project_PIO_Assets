#include <Arduino.h>

#include "command_manager.h"
#include "event_manager.h"



// =====================================================
// 命令注册表
// =====================================================

struct CommandEntry
{

    CloudCommand command;


    const char* name;


    CommandCallback callback;


};




static CommandEntry command_table
[
    MAX_COMMAND_TABLE
];


static int command_count = 0;


static void command_event_callback(const EventMessage &msg);


// =====================================================
// 初始化
// =====================================================

void command_manager_init()
{

    command_count = 0;


    Serial.println(
        "Command manager init OK"
    );


    // 注册云端命令事件监听

    event_subscribe(
        EVENT_CLOUD_COMMAND,
        command_event_callback
    );

}






// =====================================================
// 查找命令
// =====================================================


static int find_command_by_name
(
    const String &name
)
{

    for(
        int i=0;
        i<command_count;
        i++
    )
    {

        if(
            strcmp(
                    command_table[i].name,
                    name.c_str()
                )==0
        )
        {
            return i;
        }

    }


    return -1;

}






static int find_command_by_enum
(
    CloudCommand command
)
{

    for(
        int i=0;
        i<command_count;
        i++
    )
    {

        if(
            command_table[i].command
            ==
            command
        )
        {
            return i;
        }

    }


    return -1;

}






// =====================================================
// 注册命令
// =====================================================


bool command_register
(
    CloudCommand command,
    const char* name,
    CommandCallback callback
)
{


    if(
        name == nullptr
        ||
        callback == nullptr
    )
    {
        return false;
    }



    if(
        command_count
        >=
        MAX_COMMAND_TABLE
    )
    {

        Serial.println(
            "Command table full"
        );


        return false;

    }




    // 防重复注册

    for(
        int i=0;
        i<command_count;
        i++
    )
    {

        if(
            command_table[i].command
            ==
            command
            ||
            strcmp(
                command_table[i].name,
                name
            )
            ==0
        )
        {

            Serial.println(
                "Command already registered"
            );

            return false;

        }

    }




    command_table[command_count].command =
        command;


    command_table[command_count].name =
        name;


    command_table[command_count].callback =
        callback;



    command_count++;



    return true;

}







// =====================================================
// 字符串解析
//
// feed_start
//     |
//     v
// CMD_FEED_START
//
// =====================================================


CloudCommand command_parse
(
    const String &name
)
{

    int index =
        find_command_by_name(name);


    if(
        index < 0
    )
    {
        return CMD_UNKNOWN;
    }


    return
    command_table[index].command;

}






// =====================================================
// 枚举转字符串
//
// CMD_FEED_START
//        |
//        v
// feed_start
//
// =====================================================


const char* command_get_name
(
    CloudCommand command
)
{

    int index =
        find_command_by_enum(command);


    if(
        index < 0
    )
    {
        return "unknown";
    }


    return
    command_table[index].name;

}







// =====================================================
// 结果字符串
// =====================================================


const char* command_result_name
(
    CommandResult result
)
{

    switch(result)
    {

        case CMD_OK:
            return "OK";


        case CMD_BUSY:
            return "BUSY";


        case CMD_INVALID_PARAM:
            return "INVALID_PARAM";


        case CMD_UNKNOWN_CMD:
            return "UNKNOWN_CMD";


        case CMD_ERROR:
            return "ERROR";


        default:
            return "UNKNOWN";

    }

}








// =====================================================
// 命令提交
//
// feed_start:20
//
// =====================================================


CommandResult command_submit
(
    const String &command_string
)
{
    // =========================
    // 空命令防护
    // =========================

    if(
        command_string.isEmpty()
    )
    {
        return CMD_UNKNOWN_CMD;
    }



    String name;

    long param = 0;



    // -----------------------
    // 参数分割
    // -----------------------

    int pos =
        command_string.indexOf(':');



    if(
        pos >= 0
    )
    {

        name =
            command_string.substring(
                0,
                pos
            );


        String value =
            command_string.substring(
                pos + 1
            );


        param =
            value.toInt();

    }
    else
    {

        name =
            command_string;

    }




    // -----------------------
    // 查表
    // -----------------------

    int index =
        find_command_by_name(name);



    if(
        index < 0
    )
    {

        return CMD_UNKNOWN_CMD;

    }




    CommandMessage msg;


    msg.command =
        command_table[index].command;


    msg.name =
        name;


    msg.param =
        param;


    msg.source =
        "command_manager";




    // -----------------------
    // 调用插件
    // -----------------------


    CommandResult result =
        command_table[index].callback(
            msg
        );




    // -----------------------
    // 自动发布结果事件
    // -----------------------


    String result_data;


    result_data =
        name
        +
        ","
        +
        command_result_name(result)
        +
        ","
        +
        String(param);



    event_push(
        EVENT_COMMAND_RESULT,
        result_data,
        "command_manager",
        EVENT_PRIORITY_NORMAL,
        EVENT_POLICY_NORMAL,
        0
    );



    return result;

}







// =====================================================
// 云端命令事件回调
// =====================================================


static void command_event_callback(const EventMessage &msg)
{    if(
        msg.source=="command_manager"
    )
    {
        return;
    }

    Serial.println();


    Serial.println(
        "===== COMMAND ROUTER ====="
    );


    Serial.print(
        "Command:"
    );


    Serial.println(
        msg.data
    );



    CommandResult result =
        command_submit(
            msg.data
        );



    Serial.print(
        "Result:"
    );


    Serial.println(
        command_result_name(result)
    );



    Serial.println(
        "=========================="
    );


}


