锅氏自动猫粮机项目（Guo Feeder Project）｜正式开发文档
项目简介
本项目基于 ESP32-S3 开发，是一套全自动宠物投喂、供水智能设备。全程采用 模块化分层架构 + 非阻塞状态机 + 全局状态中心 设计，彻底摒弃阻塞延时、散乱全局变量、硬编码配置，支持长期稳定挂机运行。
设备支持本地OLED界面显示、按键操作、局域网Web配置、NTP网络校时、断网RTC兜底、参数持久化存储，可扩展云端远程控制、异常故障检测、定时投喂供水业务。
核心目标
- 自动定量猫粮粉末投喂
- 自动供水、控水、防溢水保护
- 本地OLED可视化状态显示、人机交互
- WiFi联网、NTP自动校时、网络异常自愈
- 全程非阻塞运行，无卡死、无卡顿
- 参数持久化保存，支持手机网页在线修改
- 可扩展云端远程控制、设备状态上报、故障日志
当前项目阶段
软件架构定型、基础外设全部调通、核心分层架构落地，进入业务功能开发阶段。

---
## 一、硬件平台
主控单元
ESP32-S3 N16R8
- 主频：240MHz 双核
- Flash：16MB
- PSRAM：8MB
显示模块
0.96寸 OLED 128*64
- 驱动：SSD1315
- 通信：I2C
- 驱动库：U8G2
外设预留
功能模块
GPIO
状态
OLED SDA
GPIO4
已启用
OLED SCL
GPIO5
已启用
电磁阀控水
GPIO21
预留
震动电机投喂
GPIO22
预留
温度传感器 DS18B20
GPIO23
预留

---
## 二、最新软件架构（最终定型版）
本项目已完成架构迭代，确立 三层严格解耦架构，彻底杜绝模块耦合、变量散乱、逻辑混杂问题。
1. 配置层（config_manager）
负责用户静态配置参数持久化存储
- 存储介质：LittleFS + 兼容NVS扩展
- 存放内容：WiFi账号密码、OLED参数、业务阈值、定时配置等
- 特性：断电不丢失、支持网页远程修改、只读不参与实时逻辑运算
2. 运行状态中心（system_state）
全局唯一运行时状态数据源（RAM常驻）
核心规则：所有模块禁止互相 extern 变量，统一读写状态中心接口
- 仅保存动态实时状态，断电清空
- 包含：WiFi状态、系统时间、NTP同步状态、RTC状态、错误码、系统运行模式
- 为OLED、云端、业务任务提供统一数据源
3. 业务驱动层
各功能模块独立、职责单一、非阻塞轮询调度
- wifi_module：非阻塞状态机联网、自动重连、网络状态维护
- time_manager：NTP校时、系统时间管理、后续对接RTC硬件
- oled_module：界面刷新、状态展示、动画显示
- 后续扩展：称重模块、供水模块、投喂模块、故障检测、MQTT云端
架构数据流
各业务模块 → 更新状态中心 → 显示/云端/控制层读取状态
配置层仅提供初始参数，不干预运行逻辑

---
## 三、当前工程目录结构
Guo_Feeder_Project
├── platformio.ini        // 工程编译配置
├── README.md             // 项目文档
├── src
│   ├── main.cpp          // 总入口、模块初始化、任务调度
│   ├── oled.cpp/.h       // OLED显示驱动与界面
│   ├── oled_animation.h  // 动画点阵资源
│   ├── wifi_module.cpp/.h    // WiFi非阻塞状态机
│   ├── config_manager.cpp/.h // 配置文件管理
│   ├── system_state.cpp/.h   // 全局运行状态中心
│   └── time_manager.cpp/.h   // NTP时间管理
├── data                  // LittleFS配置文件目录
│   └── config.json
└── backup                // 历史测试代码备份

---
## 四、核心模块说明
1. system_state 状态中心
全项目核心数据中心，所有运行时状态统一托管，全部私有static变量，仅通过 set/get 接口访问，为后续加锁、RTOS多任务铺垫。
管理内容：
- WiFi连接状态
- 系统时间戳、时间有效性
- NTP同步状态、最后同步时间
- RTC硬件可用状态
- 系统错误码
- 设备运行状态枚举（空闲/运行/故障）
2. config_manager 配置管理
负责读写 config.json，提供全局配置读取接口，支持后续网页在线修改、参数持久化。
3. wifi_module 网络模块
已完成全非阻塞状态机重构，彻底消除开机阻塞问题。
- IDLE 空闲、CONNECTING 连接中、CONNECTED 已连接、DISCONNECTED 断开
- 30s连接超时、10s自动重连机制
- 不阻塞loop，不影响其他外设运行
- 联网状态实时同步至 system_state
4. time_manager 时间管理
基于国内稳定NTP服务器校时，自动时区+8，维护系统合法时间状态，同步至状态中心，预留RTC硬件对接接口。
5. oled 显示模块
U8G2驱动稳定运行，支持清屏、文本显示、点阵动画，后续统一读取 system_state 数据渲染界面。

---
## 五、已完成功能清单
- ✅ ESP32-S3 工程环境、Flash/PSRAM 适配完成
- ✅ SSD1315 OLED 稳定驱动、动画显示
- ✅ LittleFS 文件系统、config.json 配置体系
- ✅ 全局配置管理层 config_manager
- ✅ 全局运行状态中心 system_state（架构完全定型）
- ✅ WiFi 完全非阻塞状态机、自动重连、超时机制
- ✅ 国内多节点NTP网络校时、时间状态管理

---
## 六、开发中 & 待开发计划
🔹 下一阶段优先迭代
- 优化 time_manager 联动 WiFi 状态、按需校时
- OLED 界面对接 system_state，实现状态自动刷新
- 完善WiFi异常状态识别、故障码上报
🔹 中期功能
- HX711 称重模块接入、重量校准
- 电磁阀供水状态机、防溢出保护
- 震动电机投喂定量控制
- RTC硬件对接、断网时间兜底
🔹 远期扩展
- Web配置页面完善、参数在线保存下发
- 巴法云MQTT云端通信、远程控制
- 设备故障日志、运行日志存储
- FreeRTOS精细化任务拆分

---
## 七、项目开发规范
1. 分层绝对隔离：配置、运行状态、业务逻辑三层互不越界
2. 零全局裸变量：所有运行状态统一托管 system_state
3. 全程非阻塞：禁止 while 死等、禁止 delay 阻塞主循环
4. 模块单一职责：一个模块只做一件事，不跨域处理逻辑
5. 数据单向流动：业务更新状态中心，界面/云端读取状态中心
6. 旧代码归档：废弃测试代码统一放入backup，src只保留有效工程代码


---
## 八、核心关键技术记录（项目专属技术资产）
本章记录项目定型的专属技术方案、硬性规范、核心策略，为后续迭代、维护、复刻提供唯一标准，所有开发严格遵循本章节规则。
8.1 OLED动画核心技术方案
本项目OLED动画采用 U8G2 + XBMP点阵素材 + PROGMEM静态存储 整套成熟方案，专为ESP32闪存优化，是项目核心视觉技术资产。
- 素材格式：统一使用 BMP 转 XBMP 专用工具批量转换，适配U8G2单色屏渲染规范
- 存储方式：所有动画点阵资源存入PROGMEM，常驻Flash、不占用SRAM内存
- 数据结构：采用动画指针数组统一管理多帧素材const uint8_t* anim_table[]
- 读取硬性规范：禁止直接访问Flash裸指针，必须使用 pgm_read_ptr() 读取PROGMEM动画数据，规避ESP32 Flash缓存、指针偏移、读取异常问题
8.2 WiFi连接自愈恢复策略
设备支持多级容错联网机制，适配家庭网络异常、配置丢失、密码错误等场景，实现全自动自愈，联网优先级固定不可修改：
连接优先级：config.json 配置账号 → NVS历史成功账号 → AP配网兜底模式
- 优先读取本地JSON配置的WiFi账号密码，常规场景直接联网
- JSON配置异常、联网失败时，自动读取NVS存储的历史成功联网账号重试
- 所有账号重试失败后，自动开启设备热点，进入AP配网模式，供手机在线重新配置参数
8.3 系统时间同步兜底策略
采用 NTP网络校准 + RTC硬件兜底 双时间体系，保障设备全天候时间精准有效，断网不失效：
- 首次联网：WiFi连接成功、DNS就绪后，立即发起NTP校时，初始化系统标准时间
- 断线重连：网络中断恢复后，自动重新同步NTP时间，修正运行漂移
- 长期运行：设备在线稳定运行时，7天自动周期同步一次NTP，长期校准时间误差
- 断网兜底：后续接入RTC硬件，断网、重启、无NTP场景下，由RTC硬件持续维持精准时间，保障定时业务正常运行
8.4 系统状态中心硬性接口规范
为保障项目架构统一、无耦合、可长期维护，确立全局唯一数据交互规范，所有模块严格遵守：
- 绝对禁止：各业务模块之间直接 extern 跨模块变量、直接读写对方内部状态
- 强制规范：所有状态交互必须经过 system_state 状态中心
- 标准数据流：业务模块更新状态中心数据 → 显示/云端/控制模块读取状态中心数据，单向流动、互不干扰


## 九、工程开发约束（长期维护）
## 代码提交规则

1. src目录只保存当前有效代码
2. 测试代码进入backup
3. 一个模块对应一个cpp/h
4. 禁止setup()/loop()重复定义
5. 新功能必须先确定所属模块
6. 修改架构必须同步更新README

---
当前版本
Version：V0.3 架构定型版
更新时间：2026-07-22
更新说明：完成全套分层架构、状态中心落地、WiFi非阻塞重构、NTP时间体系搭建、补充核心专属技术资产记录


# time_manager优化计划

- NTP同步采用低频触发
- 同步等待存在短暂阻塞(max 5s)

后续版本：
- 改为完全非阻塞NTP状态机

目标：

TIME_SYNC_START
        |
        ↓
TIME_SYNC_WAIT
        |
        ↓
TIME_SYNC_SUCCESS

禁止：
while等待
delay等待




# Workflow Action / Trigger 注册规范

所有模块通过 Workflow 注册自身提供的 Trigger 和 Action。

注册完成后，Workflow Registry 统一维护接口信息，Command Manager、UI 等模块通过 Workflow 提供的查询接口获取可用能力。

## 模块统一注册模板

```cpp
#include "workflow.h"
#include "module.h"


// =====================================================
// Trigger Handler（可选）
// =====================================================

static WorkflowTriggerState module_trigger_handler(
    WorkflowTriggerInstance *trigger
)
{
    // 参数解析
    // 条件判断

    return TRIGGER_SUCCESS;
}


// =====================================================
// Trigger Descriptor
// =====================================================

static WorkflowParam trigger_params[] =
{
    {"param_name", PARAM_INT, "unit", "description"}
};


static WorkflowTriggerDescriptor module_trigger_desc =
{
    .id = "MODULE_TRIGGER",
    .name = "触发名称",
    .module = "module",
    .params = trigger_params,
    .param_count = 1,
    .handler = module_trigger_handler
};


// =====================================================
// Action Handler
// =====================================================

static WorkflowActionResult module_action_handler(
    WorkflowActionInstance *action
)
{
    // 执行动作

    return ACTION_SUCCESS;
}


// =====================================================
// Action Descriptor
// =====================================================

static WorkflowParam action_params[] =
{
    {"param_name", PARAM_STRING, "", "description"}
};


static WorkflowActionDescriptor module_action_desc =
{
    .id = "MODULE_ACTION",
    .name = "动作名称",
    .module = "module",
    .params = action_params,
    .param_count = 1,
    .handler = module_action_handler
};


// =====================================================
// 模块统一注册入口
// =====================================================

void module_workflow_register()
{
    workflow_register_trigger(
        &module_trigger_desc
    );

    workflow_register_action(
        &module_action_desc
    );
}
```

## ID 命名规范

| 类型      | 格式       | 示例             |
| ------- | -------- | -------------- |
| Trigger | 大写 + 下划线 | `WEIGHT_ABOVE` |
| Action  | 大写 + 下划线 | `VALVE_OPEN`   |

## 参数规范

参数统一使用 `WorkflowParam`：

```cpp
{
    name,
    type,
    unit,
    description
}
```

Workflow、UI、Command Manager 均通过 Descriptor 获取参数定义，不重复维护注册表。




# System State 新增状态规范

System State 用于维护系统共享状态。

设计原则：

* 状态产生模块负责调用 `state_set_xxx()`
* 其他模块通过 `state_get_xxx()` 读取
* 不允许模块直接访问其他模块内部变量
* Command Manager / UI 通过 System State 查询接口获取状态

---

# 新增 System State 流程

新增一个系统状态需要修改：

1. `system_state.h`
2. `system_state.cpp`
3. 状态产生模块

---

# 1. 添加状态枚举

位置：

`system_state.h`

在 `SystemStateKey` 中增加：

```cpp
enum SystemStateKey
{
    // 已有状态

    // 新增
    STATE_MODULE_STATUS,

    STATE_MAX
};
```

注意：

* 新增状态必须放在 `STATE_MAX` 前
* `STATE_MAX` 始终保持最后

---

# 2. 添加字符串映射

位置：

`system_state.cpp`

在 `state_map[]` 增加：

```cpp
{
    "module_status",
    STATE_MODULE_STATUS,
    STATE_TYPE_INT,
    true
},
```

字段说明：

| 字段       | 说明       |
| -------- | -------- |
| name     | 外部访问名称   |
| key      | 对应枚举     |
| type     | 状态类型     |
| readable | 是否允许外部查询 |

---

# readable规则

```cpp
true
```

允许：

* Command 查询
* UI显示

```cpp
false
```

仅内部使用：

* 系统运行状态
* 调试状态
* 安全状态

---

# 3. 模块写入状态

状态产生模块调用：

## INT

```cpp
state_set_int(
    STATE_MODULE_STATUS,
    value
);
```

## BOOL

```cpp
state_set_bool(
    STATE_MODULE_ENABLE,
    true
);
```

## LONG

```cpp
state_set_long(
    STATE_COUNTER,
    count
);
```

## FLOAT

```cpp
state_set_float(
    STATE_WEIGHT_VALUE,
    weight
);
```

示例：

```cpp
void weight_update()
{
    float weight = sensor_read();

    state_set_float(
        STATE_WEIGHT_VALUE,
        weight
    );
}
```

---

# 4. 其他模块读取状态

示例：

```cpp
float weight =
    state_get_float(
        STATE_WEIGHT_VALUE
    );
```

BOOL：

```cpp
bool connected =
    state_get_bool(
        STATE_WIFI_STATUS
    );
```

读取前可检查：

```cpp
if(
    state_is_valid(
        STATE_WEIGHT_VALUE
    )
)
{
    // 使用数据
}
```

---

# 5. Command / UI 查询

无需新增代码。

只需要保证：

```cpp
state_map[]
```

存在对应记录。

外部查询：

```text
weight_value
```

自动映射：

```
字符串
  |
  v
SystemStateKey
  |
  v
state_table
```

---

# 新增状态检查清单

| 项目                   | 是否需要 |
| -------------------- | ---- |
| 增加 `SystemStateKey`  | 必须   |
| 增加 `state_map` 映射    | 必须   |
| 选择数据类型               | 必须   |
| 决定 readable          | 必须   |
| 业务模块调用 state_set_xxx | 必须   |
| 增加 Command 注册        | 不需要  |
| 增加 UI 注册             | 不需要  |
| 增加独立查询接口             | 不需要  |

---

# 命名规范

## 枚举

格式：

```cpp
STATE_MODULE_NAME
```

示例：

```cpp
STATE_VALVE_STATUS
STATE_WEIGHT_VALUE
```

---

## 外部名称

格式：

```text
module_name
```

示例：

```cpp
valve_status
weight_value
```

---

System State 是系统唯一状态中心，新模块只需要注册状态并通过统一接口读写。


Workflow Action 编码规范（精简强制执行版）
适用范围：系统所有 WorkflowActionDescriptor、Action Handler、临时 Command Action
一、实例所有权规则（最高优先级）
WorkflowActionInstance = 运行状态载体，所有权唯一
任意时刻，一个实例只能归属一处持有者：WorkflowStep 或者 TempActionItem。
禁止将同一个 WorkflowActionInstance* 赋值给多个持有者。
实例未释放前，不允许再次分配；释放标记（descriptor == nullptr）生效后才可复用。
WorkflowActionDescriptor = 只读模板，允许无限共享
描述符仅存放 ID、参数定义、handler 函数指针，不能存放任何运行时状态。
多个流程 / 指令共用同一个 Action 描述符完全安全。
二、Handler 函数强制约束
严禁在 handler 内部使用 static 局部变量保存运行状态
所有跨 loop 持续状态，必须放置在 inst->runtime 自定义结构体。
handler 入参 WorkflowActionInstance *inst 不可为空
所有参数读取、状态读写，全部依托 inst，禁止访问全局业务状态充当运行上下文。
返回值规范
ACTION_RUNNING：未完成，下一轮 workflow_task 继续调度；
ACTION_SUCCESS / ACTION_FAILED：任务终结，触发下一步跳转 / 流程报错。
禁止阻塞
不能使用 delay()、长时间阻塞读写；耗时操作采用非阻塞轮询。
三、临时 Action（Command 调用）专项约束
临时 Action 使用独立实例池 temp_action_instances，与 Workflow 主实例池物理隔离。
temp_action_alloc_instance() 获取实例；任务结束先回调、后释放实例。
释放函数 temp_action_free_instance() 必须清空所有字段、字符串缓冲区，恢复空闲标记。
不手动修改 TempActionItem->instance 指针，仅由分配 / 释放函数管理生命周期。
四、资源竞态约束（硬件类 Action 重点）
Action 实例内存隔离 ≠ 硬件资源隔离。
多流程并发调用同一个硬件 Action（阀门、称重、输出），会出现硬件争抢，实例隔离无法解决该问题。
外设互斥方案二选一：
方案 1：业务层限制，同一硬件不允许多任务同时触发；
方案 2：增加外设互斥锁，进入 handler 抢占、结束释放。
五、内存与字符串规范
释放实例时，清空内部所有 String 对象，避免碎片累积。
参数读取仅使用 inst->params，禁止缓存参数指针长期持有。
六、调试与防御性编码建议
分配失败、空实例指针，统一返回 ACTION_FAILED；
开发阶段可开启实例分配日志，排查实例池耗尽、泄漏问题；
禁止跨实例拷贝 WorkflowActionInstance 完整结构体（体积大、极易引入隐性风险）。
极简一句话备忘录（可粘贴代码头部）
Descriptor 只读共享；Instance 唯一归属；状态存入 runtime；禁用 static；不阻塞；硬件资源独立加锁。
如果你需要，我可以把这份规范精简成注释版本，直接贴在 workflow.h 文件顶部。


action 标准写法
static WorkflowActionResult xxx_action(
    WorkflowActionInstance *action
)
{
    if(action == nullptr)
        return ACTION_FAILED;


    if(!action->started)
    {
        // 参数检查


        // 创建runtime（需要才创建）


        // 启动硬件动作


        return ACTION_RUNNING;
    }


    // 检查运行状态


    if(完成)
    {
        // 释放runtime

        action->runtime=nullptr;

        return ACTION_SUCCESS;
    }


    if(错误)
    {
        // 释放runtime

        action->runtime=nullptr;

        return ACTION_FAILED;
    }


    return ACTION_RUNNING;
}