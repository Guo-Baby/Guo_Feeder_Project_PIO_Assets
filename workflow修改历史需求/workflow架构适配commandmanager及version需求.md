Workflow 云端同步与 CommandManager 扩展需求文档

项目：Guo Feeder 自动猫粮机 IoT 系统
模块范围：WorkflowManager / Workflow.cpp / CommandManager / CapabilityRegistry / CloudManager
目标：实现云端 Workflow 缓存同步、远程修改、版本校验及安全保存机制
确保：每一步修改都有git commit，以确保代码修改可回溯。
确保：每完成1步修改，都写在workflow-command manager适配改造进度文档，以保证任务中断后，可以通过读文档继续工作。

1. 修改目标

当前系统中：

Workflow 已从 JSON 文件存储迁移为 BIN 二进制存储。
Workflow.cpp 负责 Workflow 数据加载、运行及持久化。
CommandManager 负责命令解析与模块调用。
CloudManager 负责 MQTT 通信。
CapabilityRegistry 负责为 Workflow / Action / Trigger 提供稳定 Stable ID。

由于 Workflow 存储格式变化：

云端原有直接读取 workflow.json 的方式失效。
需要重新设计 ESP32 与云端之间 Workflow 同步协议。

本次修改目标：

云端能够查询当前设备 Workflow 状态。
云端能够判断本地缓存是否与设备一致。
云端能够增量同步变化的 Workflow。
云端能够创建、修改、删除 Workflow。
用户能够主动立即保存 Workflow 修改。
保持 Workflow BIN 存储结构仅存在于设备内部。
保持 Cloud Protocol 使用 JSON 格式，不暴露 BIN。
2. 总体架构要求
数据流
Cloud

 |
 | MQTT JSON
 |
 v

CloudManager

 |
 v

CommandManager

 |
 +----------------+
 |                |
 v                v

Capability      Workflow.cpp
Registry
                  |
                  |
                  v

          workflow.bin


3. 模块职责划分
3.1 Workflow.cpp 职责

Workflow.cpp 是 Workflow 数据唯一管理入口。

负责：

Workflow RAM 数据管理。
Workflow JSON ↔ RAM 转换。
Workflow RAM ↔ BIN 存储转换。
Workflow variant 管理。
Workflow dirty 状态管理。

禁止：

CommandManager 直接访问 workflow.bin。
CloudManager 直接读取 LittleFS。
其他模块解析 workflow.bin。
3.2 CommandManager 职责

CommandManager：

负责：

接收 Cloud JSON Command。
参数校验。
调用 Workflow.cpp API。
返回 JSON Response。

禁止：

解析 BIN。
直接操作 LittleFS。
保存 Workflow 内部结构。
3.3 Capability Registry 职责

Capability Registry：

继续保持独立能力映射层。

作用：

提供：

stable_id
      |
      v
runtime_id


供：

Cloud Protocol
APP
CommandManager

使用。

不修改：

WorkflowDescriptor
ActionDescriptor
TriggerDescriptor
4. Workflow Variant 机制
4.1 新增 Workflow Variant

Workflow 数据结构新增：

uint32_t variant;

含义：

表示当前 Workflow 内容版本。

例如：

workflow id:

morning_feed


variant:

1

修改一次:

2

再次修改:

3

4.2 Variant 规则
新建 Workflow
variant = 1
修改 Workflow

例如：

step 修改：

variant++

删除 Workflow

删除不是物理删除。

执行：

valid=false

variant++

重启

variant 保留。

5. Variant 与 Capability Registry 集成

CapabilityMapping 已存在：

struct CapabilityMapping
{
    uint8_t stable_id;

    String runtime_id;

    uint32_t object_version;
};


Workflow Registry 使用：

object_version = workflow.variant;

例如：

Capability Registry


stable_id:

3


runtime_id:

night_feed


object_version:

12

6. Capability Registry 修改要求
6.1 workflow.bin 格式扩展

当前：

stable_id
runtime_id length
runtime_id


修改为：

stable_id

object_version(uint32)

runtime_id length

runtime_id


原因：

重启后需要恢复 Workflow variant。

6.2 checksum

checksum 必须包含：

stable_id

object_version

runtime_id


因此：

Workflow 内容变化：

variant变化

↓

checksum变化

↓

云端发现不同

7. 云端同步机制

同步采用两阶段机制。

第一阶段：workflow.list

目的：

获取设备当前 Workflow 摘要。

请求：

{
    "cmd":"workflow.list"
}

设备返回：

{
    "cmd":"workflow.list.response",

    "registry_version":5,

    "registry_checksum":12345678,

    "workflows":[

        {
            "stable_id":0,
            "variant":12
        },

        {
            "stable_id":1,
            "variant":3
        }

    ]
}


云端比较：

本地缓存：

stable_id
variant


设备：

stable_id
variant


如果完全一致：

结束同步。

如果不同：

进入第二阶段。

第二阶段：workflow.get

请求：

{
    "cmd":"workflow.get",

    "stable_id":3
}


设备：

stable_id

↓

Capability Registry

↓

runtime_id

↓

Workflow.cpp

↓

JSON export


返回：

{
    "cmd":"workflow.get.response",

    "stable_id":3,

    "workflow":

    {
        "id":"night_feed",

        "variant":8,

        "enable":true,

        "timeout":30000,

        "steps":[

        ]

    }

}

8. Workflow JSON 格式

保持当前格式。

不重构。

当前：

{
"id":"xxx",

"name":"xxx",

"enable":true,

"timeout":10000,

"steps":[

]
}


新增：

{
"id":"xxx",

"name":"xxx",

"variant":12,

"enable":true,

"timeout":10000,

"steps":[

]
}


原则：

最小修改。

9. CommandManager 新增 Command

新增：

workflow.list

workflow.get

workflow.create

workflow.set

workflow.delete

workflow.save

10. workflow.create
请求
{
 "cmd":"workflow.create",

 "workflow":

 {

 }
}


流程：

JSON

↓

Workflow.cpp

↓

创建 RAM Workflow

↓

variant=1

↓

dirty=true


返回：

{
"success":true,

"stable_id":3,

"variant":1

}

11. workflow.set
请求
{
 "cmd":"workflow.set",

 "stable_id":3,

 "workflow":

 {

 }

}


流程：

CommandManager

↓

Workflow.cpp

↓

解析 JSON

↓

替换 Workflow RAM 数据

↓

variant++

↓

dirty=true


注意：

默认不立即写 Flash。

12. workflow.delete
请求
{
 "cmd":"workflow.delete",

 "stable_id":3
}


流程：

stable_id

↓

Workflow.cpp

↓

valid=false

↓

variant++

↓

dirty=true


禁止：

直接删除文件。

13. workflow.save
目的

允许用户主动立即保存。

请求：

{
 "cmd":"workflow.save"
}


流程：

Workflow RAM

↓

workflow.bin

↓

flush

↓

保存完成

↓

restart


返回：

{
"success":true
}

14. Dirty 状态

Workflow.cpp 增加：

bool workflow_dirty;


规则：

修改：

create

set

delete

↓

dirty=true


保存：

workflow.save

↓

dirty=false

15. 自动同步流程

云端执行：

用户点击:
同步设备 Workflow


        |

        v


workflow.list


        |

        v


比较 variant


        |

        +----------------+

        |                |

      相同             不同


        |                |

        v                v


完成             workflow.get


                         |

                         v


                    更新云端缓存

16. 内存与复制要求

原则：

减少 String 拷贝。

要求：

能使用 const 引用必须使用。
JSON export 尽量一次生成。
不允许：
BIN

↓

临时JSON文件

↓

读取

↓

上传


正确：

BIN

↓

Workflow RAM

↓

JSON String

↓

CommandManager

↓

CloudManager

17. CloudManager 修改范围

原则：

最小修改。

如果当前接口：

CloudManager.send_json(String)


已经存在：

无需修改。

CloudManager 不理解：

Workflow
Variant
Stable ID
18. 修改文件范围
必须修改
workflow.h

增加：

variant
dirty状态接口
workflow.cpp

增加：

JSON export
JSON import
get/list接口
variant维护
save接口
command_manager.h

增加：

workflow command 定义。

command_manager.cpp

增加：

Command 路由：

workflow.list

workflow.get

workflow.create

workflow.set

workflow.delete

workflow.save

capability_registry.cpp/h

修改：

Workflow object_version 接入 variant。
registry workflow entry 保存 variant。
checksum 包含 variant。
可能修改
cloud_manager.cpp

仅当：

现有发送接口无法直接发送 JSON 时修改。

19. 禁止事项

禁止：

云端解析 workflow.bin。
CommandManager 直接读取 LittleFS。
新增第二套 Workflow JSON 格式。
修改 Workflow Descriptor 结构。
修改 Action / Trigger 注册机制。
物理删除 workflow 文件。
使用时间戳作为版本。
20. 最终目标

完成后：

设备内部：

workflow.bin

+
Capability Registry

+
variant

+
checksum


提供完整状态。

云端：

只需要：

workflow.list

↓

比较 variant

↓

workflow.get

↓

同步缓存


即可保证：

云端缓存可靠
设备数据唯一
修改可追踪
Flash 写入次数降低
协议长期稳定

实现时优先修改顺序：

workflow.cpp 增加 variant 和 JSON/BIN 双向接口
Capability Registry 接入 workflow.variant
CommandManager 增加 workflow.list/get
增加 create/set/delete
增加 save
联调 CloudManager MQTT 通道

此顺序可以保证每一步都有可验证结果。