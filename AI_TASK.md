# AI Task: CommandManager V1.0 Refactor


## 修改范围

主要修改：

src/command_manager.cpp
src/command_manager.h


允许增加：

system command handler

query command handler

error/result helper


禁止修改：

workflow核心逻辑

action注册方式

trigger注册方式

CommandRuntime生命周期

MQTT协议底层


==================================================


# Task 1 Command Router 重构


command_manager_execute()


改造成三个一级路由：

execute

query

system


结构：


command_manager_execute()

{

    if(command属于execute)
    {
        execute_router();
    }

    else if(command属于query)
    {
        query_router();
    }

    else if(command属于system)
    {
        system_router();
    }

    else
    {
        返回错误结果
    }

}



禁止继续增加大量：

else if(command=="xxx")


==================================================


# Task 2 统一 Command Result / Error


增加统一错误出口。


新增内部函数：


command_send_error()


功能：

生成标准错误结果。


格式：


{
"cmd":"result",
"id":"xxx",
"type":"command",
"status":"error",
"error_code":xxx,
"message":"xxx"
}



所有错误必须经过该函数。


包括：


- command不存在

- object不存在

- action不存在

- workflow不存在

- 参数错误

- system command失败


==================================================


# Task 3 保留 ACK + RESULT 双阶段


ACK逻辑保持。


收到MQTT命令后：

CloudManager继续发送：


cmd=ack


表示收到。



CommandManager执行后：

必须返回：


cmd=result



成功：


{
"cmd":"result",
"id":"xxx",
"status":"success"
}



失败：

{
"cmd":"result",
"id":"xxx",
"status":"error"
}



==================================================


# Task 4 execute 路由


保留现有：


execute_action

execute_workflow


行为不变。



失败必须通过统一error出口返回。


==================================================


# Task 5 query 路由


新增：


query_capabilities


调用：

capability_registry接口。



返回：

包含：


action registry

trigger registry

workflow registry



必须包含：

version

checksum

count

stable_id

runtime_id



数据来源：

禁止重新扫描workflow。


直接使用：

capability_registry API



==================================================


# Task 6 原query接口处理


保留：

query_state

query_actions

query_triggers

query_workflows



但是：

后续调用统一转向 capability registry。


不得删除旧接口。


==================================================


# Task 7 system command框架


新增：


system_router()



格式：


{
"cmd":"system",
"ob":"xxx",
"pl":{}
}



system command列表：



## reboot


object:

reboot



执行流程：


1. 发送result


内容：

status=rebooting


2. MQTT publish QoS1


3. 不等待云端ack


4. 执行ESP.restart()



禁止delay模拟等待。




==================================================


## set_time


预留接口。


CommandManager只负责路由。


调用函数暂时保留。


未来由time_manager实现。


例如：


time_manager_set_time()



当前不存在时：

创建空声明保证编译。



==================================================


## weight_zero_calibrate


object:

weight_zero



调用：


weight_zero_calibrate()



当前weight manager已经存在。


CommandManager只负责调用。



==================================================


## wifi_config


object:

wifi_config



功能：

接收：

ssid

password



CommandManager负责：

解析参数


调用：


config_update_wifi()



当前config_manager没有实现时：

创建空函数。


保持编译通过。


==================================================


## wifi_ap


object:

wifi_ap



当前只建立command入口。


暂不实现AP逻辑。


==================================================


# Task 8 Capability Registry 查询


增加：

query_capabilities



调用：

capability_registry



返回：

action:

trigger:

workflow:


每项：

stable_id

runtime_id



同时返回：


version

checksum



==================================================


# Task 9 MQTT callback返回处理


修改CloudManager：


command_manager_execute(cmd)



返回值必须处理。


如果返回false：

调用统一错误反馈。



禁止静默失败。


==================================================


# Task 10 禁止修改内容


以下暂不修改：


1.

CommandRuntime


2.

String payload设计


3.

MQTT payload缓存方式


4.

CBOR协议


5.

Workflow执行逻辑


6.

Capability Registry内部实现



==================================================


# Task 11 编译要求


修改后必须：

pio run


通过。


不得新增warning。



==================================================
