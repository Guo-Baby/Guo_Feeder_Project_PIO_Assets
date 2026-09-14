WorkflowStorage BIN 格式与持久化设计需求文档

版本：V1.0
目标平台：ESP32-S3 N16R8
依赖：

WorkflowManager
        |
        ↓
WorkflowStorage
        |
        ↓
BinStorage
        |
        ↓
FileStorage
        |
        ↓
LittleFS
一、设计目标
1.1 核心目标

为 Workflow 系统提供独立持久化能力：

支持：

Workflow Definition 保存
Workflow Definition 加载
Workflow Definition 修改
Workflow Definition 删除
Workflow Meta 管理
BIN 文件校验
原子保存
断电保护

同时满足：

Workflow.cpp 不直接依赖 LittleFS
Workflow.cpp 不知道 BIN 文件路径规则
Workflow.cpp 不处理 CRC
Workflow.cpp 不处理临时文件
Workflow.cpp 不处理 rename
二、职责边界
2.1 WorkflowStorage 负责

负责：

Definition对象
        |
        ↓
序列化
        |
        ↓
BIN格式
        |
        ↓
BinStorage

包括：

BIN Header
Version
CRC
Serialize
Deserialize
Meta管理
文件命名
2.2 WorkflowStorage 不负责

禁止：

Action执行
Trigger执行
Runtime状态
Workflow调度
Step Poll
Critical业务逻辑

例如：

禁止：

workflow_storage_start()
workflow_storage_execute()
workflow_storage_poll()

Storage只保存数据。

三、文件结构设计

LittleFS：

/workflow/

    meta.bin

    wf00/
        step00.bin
        step01.bin
        step02.bin

    wf01/
        step00.bin
        step01.bin

    wf02/
        ...

说明：

一个 Workflow 一个目录
一个 Step 一个 BIN 文件
meta.bin 管理总体状态
四、为什么采用 Step 独立 BIN
不采用：
workflow00.bin

原因：

一个 Workflow 内修改一个 Step：

需要：

读取整个 workflow
修改
整体重新写入

风险：

写放大
掉电损坏范围大
后续增量编辑困难

采用：

wf00/

step00.bin
step01.bin
step02.bin

优势：

修改 Step：

只影响一个文件。

同时满足：

单 Step 更新
多 Step Transaction
Lazy Load
五、Meta 文件设计

文件：

/workflow/meta.bin

作用：

Workflow目录索引。

不保存 Runtime。

5.1 Meta Header

结构：

struct WorkflowMetaHeader
{
    uint32_t magic;

    uint16_t version;

    uint16_t workflow_count;

    uint32_t crc32;
};
5.2 Workflow Meta Entry

最大：

WORKFLOW_MAX_COUNT = 16

结构：

struct WorkflowMetaEntry
{
    uint8_t valid;

    uint8_t version;

    uint16_t step_count;

    uint32_t update_time;

    uint32_t crc32;
};

大小：

约：

12 bytes

16个：

192 bytes

非常小。

六、为什么 meta 必须保存 step_count

step_count 必须存在。

原因：

Step Slot：

0
1
2
3
...
15

不是所有 Step 都有效。

例如：

step00 valid
step01 valid
step02 valid
step03 valid

step04
...
step15 empty

如果没有 step_count：

加载时无法知道：

step04.bin 是否应该读取？

也无法知道：

Workflow什么时候结束。

因此：

Workflow执行范围：

for(i=0;i<step_count;i++)
{
    execute(step[i]);
}

而不是：

for(i=0;i<16;i++)
{
    try load;
}
七、Step BIN格式设计

文件：

wf00/step00.bin
7.1 Header
struct WorkflowStepBinHeader
{
    uint32_t magic;

    uint16_t version;

    uint16_t header_size;

    uint32_t payload_size;

    uint32_t crc32;
};
7.2 Payload

Payload保存：

StepDefinition

不保存：

runtime
state
result
callback
running

示例：

Header

↓

StepDefinition

{
 type

 instance_type

 id

 params[]
}

八、Runtime禁止持久化

禁止保存：

current_step

RUNNING

timeout

timer_state

action_runtime

trigger_runtime

callback

原因：

掉电恢复：

不应该继续半执行 Workflow。

例如：

投喂：

step0
  开电机

step1
  称重

断电

恢复后继续：

可能导致：

重复投喂。

所以：

启动：

Workflow = STOPPED

重新 start。

九、保存事务模型
9.1 单 Step 修改

流程：

Modify Definition

↓

Serialize

↓

BinStorage.write_atomic()

↓

更新meta crc

↓

完成
9.2 多 Step 修改

例如：

用户修改：

step00
step01
...
step30

不允许：

保存一半。

采用：

Transaction。

流程：

Begin Transaction


write step00.tmp

write step01.tmp

...

全部成功


rename tmp → bin


update meta


Commit


失败：

rollback

保持旧版本
十、Dirty机制

WorkflowStorage 不管理 Dirty。

原因：

Dirty属于：

WorkflowManager状态。

职责：

WorkflowManager

知道：

哪个Definition变化

什么时候保存


↓

调用


WorkflowStorage.save()


WorkflowStorage只提供：

save_workflow()

成功：

返回：

OK

失败：

返回：

WRITE_FAILED

CRC_FAILED

RENAME_FAILED
十一、Lazy Load设计

启动：

只加载：

meta.bin

不加载：

step00.bin
...

第一次：

workflow_start(id)


流程：

WorkflowManager

↓

WorkflowStorage.load(id)

↓

读取step BIN

↓

生成Definition

↓

Runtime snapshot

十二、API设计
初始化
bool workflow_storage_init();
Meta
bool workflow_storage_load_meta();

bool workflow_storage_save_meta();

bool workflow_storage_get_valid(
    uint8_t workflow_id
);


bool workflow_storage_set_valid(
    uint8_t workflow_id,
    bool valid
);

Workflow
BinStorageResult workflow_storage_load(
    uint8_t workflow_id,
    WorkflowDefinition* definition
);


BinStorageResult workflow_storage_save(
    uint8_t workflow_id,
    const WorkflowDefinition* definition
);


BinStorageResult workflow_storage_delete(
    uint8_t workflow_id
);

Step
BinStorageResult workflow_storage_load_step(
    uint8_t workflow_id,
    uint8_t step_id,
    StepDefinition* step
);


BinStorageResult workflow_storage_save_step(
    uint8_t workflow_id,
    uint8_t step_id,
    const StepDefinition* step
);

十三、错误处理

复用：

BinStorageResult

不新增错误体系。

例如：

NOT_FOUND

INVALID_ARGUMENT

CRC_FAILED

WRITE_FAILED

RENAME_FAILED

VERSION_MISMATCH


其中：

WorkflowStorage只增加 Workflow语义错误：

例如：

WORKFLOW_INVALID_FORMAT

WORKFLOW_VERSION_TOO_NEW
十四、版本兼容

所有 BIN：

必须：

magic

version

加载：

if(version != CURRENT_VERSION)

{
 reject
}

禁止：

自动猜测旧格式。

十五、CRC策略
Step BIN

必须 CRC。

原因：

Step是独立文件。

损坏：

只影响一个Step。

Meta

必须 CRC。

原因：

Meta决定：

哪些Workflow存在。

损坏：

可能导致整个Workflow不可见。

十六、删除策略

删除 Workflow：

不立即删除文件。

流程：

valid = false

save meta


BIN保留。

原因：

避免：

大量LittleFS删除。

未来：

GC。

十七、禁止事项

实现时禁止：

禁止1

WorkflowStorage调用：

workflow_manager

防止循环依赖。

禁止2

WorkflowStorage直接操作：

LittleFS.open()

必须：

BinStorage
禁止3

保存Runtime

禁止4

保存函数指针

例如：

void (*callback)
十八、验收标准

完成后必须满足：

项目	要求
不修改JsonStorage	必须
不修改ConfigManager	必须
不修改BinStorage	原则上禁止
Workflow不直接访问LittleFS	必须
支持meta	必须
支持step独立BIN	必须
支持CRC	必须
支持atomic save	必须
支持lazy load	必须
不保存runtime	必须
支持多step事务	必须
十九、本阶段明确不实现

以下属于下一阶段：

Workflow.cpp Definition/Runtime拆分
Runtime Pool
params snapshot
Dirty/Critical机制
CommandManager接口
MQTT编辑Workflow
二十、最终目标架构

完成全部阶段后：

                MQTT

                  |

          CommandManager

                  |

          WorkflowManager

          /              \

 Definition             Runtime

     |

 WorkflowStorage

     |

 BinStorage

     |

 FileStorage

     |

 LittleFS


最终效果：

Workflow逻辑完全不知道Flash
Flash存储完全不知道Workflow执行
Definition与Runtime完全隔离
支持在线编辑
支持断电保护
支持未来云端Workflow管理

交付给开发者要求：

本阶段只实现：

WorkflowStorage

不得提前修改：

workflow.cpp
workflow.h
CommandManager
MQTT
ConfigManager
JsonStorage

完成后必须：

编译通过
无新增warning
提供修改文件清单
提供API说明
不烧录测试（除非另行要求）