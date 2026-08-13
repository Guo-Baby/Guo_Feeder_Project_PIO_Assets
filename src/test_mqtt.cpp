#include <Arduino.h>
#include <ArduinoJson.h>

#include "test_mqtt.h"

#include "workflow.h"
#include "cloud_manager.h"


#define TEST_MQTT_RUNTIME_POOL 4


struct TestMqttRuntime
{
    bool used;

    unsigned long trigger_time;

    unsigned long delay_ms;

    String message;
};


static TestMqttRuntime test_runtime_pool[TEST_MQTT_RUNTIME_POOL];



static TestMqttRuntime* test_runtime_alloc()
{
    for(uint8_t i = 0; i < TEST_MQTT_RUNTIME_POOL; i++)
    {
        if(!test_runtime_pool[i].used)
        {
            test_runtime_pool[i].used = true;
            return &test_runtime_pool[i];
        }
    }

    return nullptr;
}



static void test_runtime_free(
    TestMqttRuntime *ctx
)
{
    if(ctx == nullptr)
        return;


    ctx->used = false;
    ctx->message = "";
    ctx->delay_ms = 0;
    ctx->trigger_time = 0;
}

struct MqttTestRuntime
{
    unsigned long start_ms;

    unsigned long delay_ms;

    int length;

    String message;

    bool sent;
};

static int test_get_length(
    WorkflowActionInstance *action
)
{
    if(action == nullptr)
        return 100;


    for(uint8_t i=0;i<action->param_count;i++)
    {
        if(action->params[i].name=="length")
        {
            return action->params[i].int_value;
        }
    }


    return 100;
}

static String build_test_message(
    int length
)
{
    String msg;


    msg.reserve(length+100);


    msg="{\"cmd\":\"test\",\"length\":";


    msg += length;


    msg += ",\"data\":\"";


    while(msg.length()+2 < length)
    {
        msg += "0";
    }


    msg += "\"}";


    return msg;
}

// =====================================================
// 参数定义
//
// payload:
//
// {
//    "length":512
// }
//
// Action自己解析
// =====================================================


static WorkflowParam mqtt_test_params[] =
{
    {
        "length",
        PARAM_INT,
        "byte",
        "MQTT测试消息长度"
    }
};

// =====================================================
// Action 私有参数读取
//
// Action自己解析 params
//
// 不依赖Workflow内部逻辑
// =====================================================


static int mqtt_test_get_int(
    WorkflowActionInstance *action,
    const char *name,
    int def
)
{
    if(action == nullptr)
        return def;


    for(uint8_t i = 0;
        i < action->param_count;
        i++)
    {

        if(action->params[i].name == name)
        {
            if(action->params[i].type == PARAM_INT)
            {
                return action->params[i].int_value;
            }
        }
    }


    return def;
}



// =====================================================
// reset
// =====================================================
static void mqtt_test_reset(
    WorkflowActionInstance *action
)
{
    if(action==nullptr)
        return;


    action->runtime=nullptr;

    action->running=false;

    action->result=
        ACTION_IDLE;
}


// =====================================================
// start
//
// 即时完成Action
//
// 1. 解析length
// 2. 构造消息
// 3. cloud_send_up
// 4. 返回ACTION_SUCCESS/FAILED
//
// =====================================================
static void mqtt_test_start(
    WorkflowActionInstance *action
)
{
    if(action == nullptr)
        return;



    int length =
        test_get_length(action);



    Serial.printf(
        "[TestMQTT] request length=%d\n",
        length
    );



    String msg =
        build_test_message(length);



    TestMqttRuntime *ctx =
        test_runtime_alloc();



    if(ctx == nullptr)
    {
        Serial.println(
            "[TestMQTT] runtime alloc failed"
        );


        action->result =
            ACTION_FAILED;

        return;
    }



    ctx->delay_ms = 3000;

    ctx->trigger_time =
        millis();


    // 独立保存消息
    ctx->message = msg;



    Serial.printf(
        "[TestMQTT] runtime created len=%d\n",
        ctx->message.length()
    );



    // 立即结束Action
    action->result =
        ACTION_SUCCESS;
}


// =====================================================
// poll
//
// 即时完成
//
// 防止异常残留
//
// =====================================================
static void mqtt_test_poll(
    WorkflowActionInstance *action
)
{
    if(action==nullptr)
        return;


    if(action->result==ACTION_IDLE)
    {
        action->result =
            ACTION_FAILED;
    }
}

// =====================================================
// Descriptor
// =====================================================
static WorkflowActionDescriptor mqtt_test_desc =
{
    .id =
        "MQTT_TEST",

    .name =
        "MQTT长度测试",

    .module =
        "test",

    .description =
        "发送指定长度MQTT消息",

    .params =
        mqtt_test_params,

    .param_count =
        1,

    .reset =
        mqtt_test_reset,

    .start =
        mqtt_test_start,

    .poll =
        mqtt_test_poll
};


// =====================================================
// init
//
// 注册Action
//
// =====================================================
void test_mqtt_init()
{
    workflow_register_action(
        &mqtt_test_desc
    );


    Serial.println(
        "[TestMQTT] Action registered"
    );
}


// =====================================================
// task
//
// 当前无需任务
// =====================================================

void test_mqtt_task()
{
    unsigned long now =
        millis();



    for(uint8_t i=0;i<TEST_MQTT_RUNTIME_POOL;i++)
    {
        TestMqttRuntime &ctx =
            test_runtime_pool[i];


        if(!ctx.used)
            continue;



        if(now - ctx.trigger_time >= ctx.delay_ms)
        {

            Serial.printf(
                "[TestMQTT] send delayed message len=%d\n",
                ctx.message.length()
            );


            bool ok =
                cloud_send_up(
                    ctx.message.c_str()
                );


            Serial.println(
                ok?
                "[TestMQTT] send OK":
                "[TestMQTT] send FAIL"
            );


            test_runtime_free(
                &ctx
            );
        }
    }
}