锅氏自动猫粮机项目（Guo Feeder Project）｜正式开发文档
## 项目简介
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

## 当前项目阶段
v0.7  2026-08-27

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

---
## 二、最新软件架构（最终定型版）

本项目采用分层 + 模块化 + 全局状态中心 + 事件机制 + 非阻塞状态机的软件架构。

随着设备功能逐渐增加，系统目前形成以下四个逻辑层：

┌─────────────────────────────────────────────┐
│                应用 / 能力层                │
│                                             │
│ Valve   Weight   Dispense Guard   Mijia     │
│ OLED    Motor    其他未来业务模块           │
└─────────────────────────────────────────────┘
                      │
                      ▼
┌─────────────────────────────────────────────┐
│                自动化 / 中间层              │
│                                             │
│ Workflow Manager                            │
│   ├── Trigger                               │
│   ├── Action                                │
│   ├── Workflow                              │
│   └── Temporary Action                      │
│                                             │
│ Capability Registry                         │
│   ├── Action Registry                       │
│   ├── Trigger Registry                      │
│   ├── Workflow Registry                     │
│   └── Stable ID                             │
└─────────────────────────────────────────────┘
                      │
                      ▼
┌─────────────────────────────────────────────┐
│                  服务层                     │
│                                             │
│ System State                                │
│ Config Manager                              │
│ Event Manager                               │
│ Time Manager                                │
│ WiFi Module                                 │
│ Command Manager                             │
│ Log Manager                                 │
└─────────────────────────────────────────────┘
                      │
                      ▼
┌─────────────────────────────────────────────┐
│                  云通信层                   │
│                                             │
│ Cloud Manager                               │
│   ├── MQTT                                  │
│   ├── Cloud Protocol                        │
│   ├── JSON / CBOR                           │
│   ├── ACK                                   │
│   └── UP / DOWN                             │
└─────────────────────────────────────────────┘

注：以上为逻辑分层，不要求每一层对应一个实际文件夹。当前所有代码仍统一位于 src 目录。

# 2.1 应用 / 能力层

应用 / 能力层负责具体的设备功能和硬件能力。
每个业务模块保持独立，原则上不直接依赖其他业务模块的内部变量，而是通过 System State、Event Manager 和统一接口进行数据交互。

当前主要包括：
Valve 电磁阀控制模块。
HX711 称重模块。
Dispense Guard 定量供水安全保护模块,这是一个独立的高优先级保护模块。
米家蓝牙温湿度计模块。
OLED 本地显示与人机交互模块。
Motor 步进电机投喂模块,目前属于后续开发能力。

# 2.2 Workflow 自动化层

Workflow 是整个设备自动化能力的核心中间层。
它不直接定义具体硬件，而是负责将各种设备能力组合成为可执行的工作流。

# 2.3 Capability Registry

Capability Registry 是设备能力的统一登记与对外描述层。
它可以从 Workflow Framework 获取已经注册的能力，并为这些能力建立Stable ID

# 2.4 服务层

服务层负责提供整个设备运行所需要的基础服务。

# 2.4.1 System State

System State 是整个设备唯一的运行时状态中心。

所有模块的动态运行状态统一由它维护。

# 2.4.2 Config Manager

Config Manager 负责用户配置数据的持久化管理。

当前配置文件位于：

data/
├── config.json
└── workflow.json

Config Manager 负责：
配置读取
JSON 解析
类型转换
配置查询
配置持久化
支持云端/UI修改配置


# 2.4.3 Event Manager

Event Manager 是系统的一次性事件通信中心。
与 System State 的区别：System State = 持久存在的状态, Event = 一次性发生的事件

事件产生模块负责发布 Event。
其他模块可以订阅 Event。

支持：
Event 发布
Event 订阅
多模块同时订阅同一个 Event

# 2.4.4 WiFi Module

负责设备网络连接。

主要功能：
WiFi 连接
自动重连
连接超时
WiFi 状态维护
信号强度查询
WiFi 状态写入 System State
WiFi Connected / Disconnected Event

# 2.4.5 Time Manager

负责系统时间管理（TimeManager V2）。

双时间体系：

SNTP（最高可信时间源）
  - ESP-IDF esp_sntp（非自实现 NTP、非 configTime）
  - Smooth Sync 平滑同步，固定 24h 校时周期（单调时钟计时）

PCF8563T RTC（启动恢复 / 断网兜底时间源）
  - I2C 7-bit 地址 0x51
  - 与 OLED 共用同一 I2C 总线（SDA/SCL 由 oled.json 决定）

主要功能：
SNTP 校时
RTC 读写（PCF8563T）
上电 RTC → System Time 硬同步恢复
SNTP 成功后按阈值校准 RTC（Write 走 Critical Operation）
Unix Timestamp 内部统一基准
时间有效性管理（STATE_TIME_VALID 保持 bool）
时间查询（ESP 系统时间 + RTC 时间，非阻塞）

时间基准约定：
RTC 与 System Time 内部一律使用 UTC / Unix Timestamp
时区（config time.timezone，默认 GMT+8）仅用于 UTC → Local 显示
本地时间禁止直接写入 RTC
SNTP 为最高可信源，RTC 为启动恢复 / 离线源

其中 Time Valid 是非常重要的系统状态。
如果时间不可信，则依赖准确时间的 Workflow 不应执行。

# 2.4.6 Command Manager

Command Manager 是设备内部的统一命令路由中心。
所有来自云端、UI、本地的命令最终统一进入 Command Manager。

基本链路：
Cloud Manager
      ↓
Command Manager
      ↓
Command Routing
      ↓
目标模块 / Workflow / System Service

# 2.5 云通信层 Cloud Manager

Cloud Manager 是设备 MQTT 云通信的唯一入口。

它负责：
MQTT 连接
MQTT 自动重连
MQTT RX
MQTT UP
MQTT SET
云协议解析
JSON 消息转换
ACK
Command 转换
云端消息上传
云端命令下发

# 2.6 Log Manager

Log Manager 属于服务层，负责保存设备运行过程中重要的：
系统事件
错误
状态变化
关键运行数据
故障信息
业务运行记录

目标是在不明显影响 ESP32 实时运行性能的前提下，实现可靠的本地日志记录，并Log Upload。

# 2.7 系统整体数据流

整个系统最终形成：

                ┌──────────────┐
                │ Cloud / UI   │
                └──────┬───────┘
                       │
                       ▼
                ┌──────────────┐
                │ Cloud Manager│
                └──────┬───────┘
                       │
                       ▼
                ┌──────────────┐
                │Command Manager│
                └──────┬───────┘
                       │
             ┌─────────┴─────────┐
             ▼                   ▼
       Workflow Manager      System Service
             │                   │
       ┌─────┴─────┐       ┌─────┴──────────┐
       ▼           ▼       ▼                ▼
    Trigger      Action  Config          System State
       │           │
       └─────┬─────┘
             │
             ▼
       Application Modules

同时系统状态和事件作为横向基础设施：

Application Modules
       │
       ├──────────────→ System State
       │
       └──────────────→ Event Manager

System State
       ↑       ↑       ↑
      OLED   Cloud   Command

Event Manager
       │
       ├── Time Manager
       ├── Cloud Manager
       ├── Dispense Guard
       └── Other Modules

这种设计使得设备可以在保持模块独立的情况下不断增加新的硬件能力和业务能力。

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
│   └── time_manager.cpp/.h   // SNTP + PCF8563T RTC 时间管理
├── data                  // LittleFS配置文件目录
│   └── config            // 模块级配置（按模块拆分）
│       ├── time.json     // 时区 / NTP 服务器 / 校时周期
│       ├── rtc.json      // RTC 使能 / SDA / SCL / I2C 地址 / 校准阈值
│       └── oled.json     // OLED I2C 引脚（RTC 复用同一总线）
└── backup                // 历史测试代码备份

---
## 四、核心模块说明
# 4.1 System State

整个项目的唯一运行时状态中心。
负责保存所有需要跨模块共享的动态状态。

核心原则：
产生状态的模块
        ↓
state_set_xxx()
        ↓
System State
        ↓
state_get_xxx()
        ↓
其他模块

禁止模块直接访问其他模块内部变量。

支持：
INT
BOOL
LONG
FLOAT
通用字符串查询
状态有效性判断
Enum → String 映射
String → SystemStateKey 映射

# 4.2 Config Manager
负责用户配置参数的持久化管理。
当前配置文件：
data/config.json
data/workflow.json

负责：
读取配置
JSON解析
类型转换
参数查询
参数保存
远程修改


# 4.3 WiFi Module

负责设备网络连接和网络状态维护。
主要功能：
WiFi连接
自动重连
超时处理
RSSI查询
状态同步
WiFi Event发布
配置读取
NVS历史账号兜底
AP配网兜底
全程采用非阻塞状态机。

#4.4 Time Manager

负责系统时间（TimeManager V2）。
主要功能：
SNTP 同步（ESP-IDF esp_sntp，平滑同步，24h 周期）
PCF8563T RTC 读写（I2C 0x51，复用 OLED 总线）
Unix Timestamp
系统时间设置
时间有效性（STATE_TIME_VALID，bool）
SNTP 状态
时间查询（系统时间 + RTC 时间，非阻塞）
SNTP → RTC（校准）
RTC → System Time（上电恢复）

硬件与配置：
RTC 芯片：PCF8563T，I2C 7-bit 地址 0x51
配置文件：data/config/rtc.json、data/config/time.json
RTC 禁止调用 Wire.begin() / Wire.setClock()，I2C 总线由 OLED 初始化

RTC 寄存器布局（PCF8563T）：
0x00 Control_status_1
0x01 Control_status_2
0x02 VL_seconds
0x03 Minutes
0x04 Hours
0x05 Days
0x06 Weekdays
0x07 Century_months
0x08 Years

读：0x02 起连续 7 bytes
写：0x00 起连续写入
seconds bit7 = VL，置位表示 RTC 时间不可信

初始化顺序约束：
time_init() 必须晚于 oled_init()（否则 I2C 总线尚未建立，RTC 探测必失败）

实现断网、重启情况下的时间兜底。

# 4.5 Event Manager

负责一次性事件的发布与订阅。
主要功能：
Event 注册
Event 发布
Event 订阅
多订阅者
Event 回调

用于降低模块之间的直接耦合。

# 4.6 Valve

负责电磁阀控制。
提供：
Valve Open
Valve Close
Valve Status
Workflow Action
是自动供水业务的基础执行模块。

# 4.7 Weight
负责 HX711 和称重系统。
提供：
实时重量
重量状态
调零
校准
重量异常检测
Weight Event
Weight Trigger
重量数据统一进入 System State。

# 4.8 Dispense Guard
负责定量供水过程中的高优先级安全保护。

核心逻辑：
Weight Error
     ↓
Event
     ↓
Dispense Guard
     ↓
Valve Close

用于防止重量传感器异常时继续进水。

# 4.9 Workflow Manager

负责自动化工作流。
核心功能：
Trigger 注册
Action 注册
Workflow 注册
Step 调度
Trigger 生命周期
Action 生命周期
Workflow JSON 解析
Workflow 执行
Temporary Action
Action Instance 管理

## 4.9.1 架构组成

```
Workflow
├── Definition          持久化定义（可被云端修改）
├── Runtime             执行期内存快照（不受 Definition 修改影响）
├── WorkflowStorage     Definition ↔ BIN 持久化（原子事务 + 双版本备份）
├── Variant             单个 Workflow 的内容版本（云端增量同步依据）
├── Capability Registry Stable ID ↔ Runtime ID 映射
└── CommandManager      命令路由（Cloud Sync 入口）
```

## 4.9.2 数据形态与边界

```
Cloud  ←→ JSON ←→  CommandManager  ←→  Workflow.cpp  ←→  Definition
                                                             │
                                                     WorkflowStorage
                                                             │
                                                     Meta BIN + Step BIN
                                                             │
                                                         LittleFS
```

- **Cloud 使用 JSON**：JSON 只是 Cloud / CommandManager ↔ Workflow.cpp 的
  **通信格式**，不是设备内部长期存储格式。
- **设备内部使用 BIN**：Definition 落盘为 Meta BIN + Step BIN，
  带原子事务与备份，云端不需要也不允许理解。
- **Workflow Variant 用于对象版本同步**：每个 Workflow 独立维护
  `uint32_t variant`。`create → 1`，内容变化 `→ +1`，`delete → +1`，
  内容无变化不增加，**重启后不得回退**。
- **Capability Registry 提供 Stable ID**：由 `runtime_id` 字母序生成，
  仅作定位；`object_version = workflow.variant`，是 registry
  版本与校验值的输入。

## 4.9.3 Workflow 定位模型

```text
              Workflow
                 │
        ┌────────┴────────┐
      p.id            workflow.id
   唯一 Slot 定位      用户业务 ID
    （整数 0..15）     （字符串，可重复）
```

- **`p.id` = Slot 索引**，是 `create`/`set`/`delete`/`get` 的**唯一定位键**。
- **`workflow.id` 允许重复**，不能作为主键，设备不按它搜索对象。
- **`stable_id` 是 Registry 映射元数据**，按 `runtime_id` 排序生成，
  新增/改名会整体重排，**不得长期引用**。

## 4.9.4 云端同步命令

```
workflow.sync_info    轻量同步入口（version / checksum / count / dirty）
workflow.list         摘要列表（slot / id / variant / valid / stable_id）
workflow.get          按 Slot 拉取单个完整 JSON
workflow.create       新建（variant = 1，允许重复 workflow.id）
workflow.set          按 Slot 整体替换（variant +1；内容相同则不变）
workflow.delete       逻辑删除（valid=false，variant +1；重复删除幂等）
workflow.save         全 Dirty 落盘（逐 Workflow 事务），成功后请求安全重启
```

关键语义（详见接口文档）：

- `count` = **占用 Slot 数**，含 `valid=false` 的已删对象，**不等于**可执行数量。
- `dirty` 是**全局**标志，不指明是哪个对象。
- `save` 逻辑上全局、物理上**逐 Workflow 事务**：成功者清自己的 Dirty，
  失败者保留；任一失败则整体返回失败。
- `restarting:true` 只表示**重启请求已被接受**，不代表已完成 reboot。
- `type` 必填；`steps>16` / `params>8` **整体拒绝**（不截断）。

> **完整协议（请求 / 参数 / 返回 / 错误码 / variant 与 dirty 行为 /
> 同步流程 / 可用 Action·Trigger 清单 / UI 对接注意事项）见
> `workflow_cloud_interface.md`，README 不重复展开。**

所有耗时任务采用：

start()
 ↓
poll()
 ↓
SUCCESS / FAILED

禁止：

delay()
while等待
阻塞式执行

# 4.10 Capability Registry

负责设备能力统一登记和 Stable ID 管理。
管理：
Action
Trigger
Workflow

主要作用：
能力名称
   ↓
Stable ID
   ↓
云端/UI

以及：
Stable ID
   ↓
Capability Registry
   ↓
真实 Action / Trigger / Workflow

减少云端通信报文长度，同时为未来 UI 自动生成设备能力界面提供基础。

## 4.10.1 与 Workflow Variant 的关系

```
CapabilityMapping
├── stable_id        按 runtime_id 字母序排序后的下标
├── runtime_id       真实对象名（Action / Trigger / Workflow id）
└── object_version   Workflow 填 workflow.variant

registry_version    整个 Mapping 的版本（任一映射变化 +1）
registry_checksum   整个 Mapping 的校验值
```

- `registry_version` 与 `Workflow variant` 是**两个层级**，不得混淆：
  前者描述"整体映射变没变"，后者描述"单个 Workflow 内容变没变"。
- `object_version` 必须参与 checksum，否则会出现
  "内容变了但 checksum 没变 → 云端永远同步不到"。
- `stable_id` 会随 `runtime_id` 集合变化**整体平移**，
  云端应以 **`p.id`（Slot）** 为 Workflow 唯一主键，`stable_id` 只作定位。
- `registry_version` 也可能因 Action / Trigger 映射变化而改变，
  **不能**据此推断"只有 Workflow 变了"。
- Registry 按 `runtime_id` **去重**，因此 `workflow.id` 重复时
  Registry 条目数**少于**占用 Slot 数 —— `list` / `sync_info` 的
  `count` 用的是**占用 Slot 数**，不是 Registry 条目数。

# 4.11 Command Manager
负责设备命令的统一接收、分类和路由。

主要负责：
Command
   ↓
解析
   ↓
分类
   ↓
路由
   ↓
执行

支持：
Execute Command
Query Command
System Command
Workflow Manage Command（workflow.sync_info / list / get / create / set / delete / save）

分层约束：
CommandManager 只做命令路由，**不允许**直接操作
WorkflowStorage / BinStorage / FileStorage；
Workflow 的 JSON ↔ 内部结构语义转换一律由 Workflow.cpp 承担。

Workflow 管理命令专用错误码：
1  Unknown command
5  Workflow not found
6  Invalid payload / missing workflow
11 Invalid JSON payload
12 No free slot
13 Rejected（运行中 / Critical 获取失败）
Config Command
Workflow Command

Command Manager 不负责具体业务逻辑，而是调用对应模块提供的接口。

# 4.12 Cloud Manager

负责设备与 MQTT 云端之间的通信。

主要功能：
MQTT连接
MQTT重连
RX
UP
DOWN
ACK
Cloud Protocol
JSON解析
Command转换
State上传

核心原则：
Cloud
  ↕
Cloud Manager
  ↕
Command Manager / State / Capability

避免云通信代码直接侵入业务模块。

整体通信链路：
Cloud
  ↓
MQTT
  ↓
Cloud Manager
  ↓
Command Manager
  ↓
Device

反向：
Device
  ↓
Cloud Manager
  ↓
MQTT
  ↓
Cloud

Cloud Manager 与设备业务模块保持解耦。

云端原则上只通过 Cloud Manager 与设备 Command / State / Capability 等体系交互，不直接调用具体业务模块。

Cloud Protocol

当前已经形成统一 MQTT JSON 外壳：
cmd
ob
id
pl
src
ts

采用topic：
UP
DOWN
作为方向区分。

同时已经实现：
Command 压缩
Object / Command 映射
ACK
Action Result
Workflow Result
State Report

目前 MQTT JSON 协议已经基本进入冻结阶段。

# 4.13 Mijia Temperature / Humidity

负责米家蓝牙温湿度计。
主要功能：
BLE扫描
广播解析
加密报文解码
温度读取
湿度读取
状态保存
Config管理
System State更新

当前框架已经完成，下一阶段重点是完善广播报文解码算法。

# 4.14 OLED

负责本地显示和人机交互。

SSD1315
U8G2
文本显示
System State显示
菜单
状态页面
故障页面
四按键操作
本地命令

# 4.15 Log Manager

目前尚未正式实现。

未来负责：

系统事件
错误
故障
关键状态
运行记录
       ↓
Log
       ↓
本地保存
       ↓
未来云端上传

设计重点是降低 Flash 写入次数和对主循环性能的影响。

---
## 五、已完成功能清单
✅ ESP32-S3 N16R8 工程环境、Flash / PSRAM 适配完成
✅ SSD1315 OLED 稳定驱动、U8G2 动画显示
✅ LittleFS 文件系统
✅ Config Manager 基础配置读取
✅ System State 全局运行状态中心
✅ Event Manager 事件发布 / 订阅框架
✅ WiFi 非阻塞状态机、自动重连、连接超时机制
✅ NTP 网络校时、Unix 时间戳及时间有效性管理
✅ Time Manager 基础框架
✅ TimeManager V2：ESP-IDF SNTP 平滑同步（固定 24h 周期，单调时钟计时）
✅ TimeManager V2：PCF8563T RTC 驱动（I2C 0x51，与 OLED 共用总线）
✅ TimeManager V2：上电 RTC → System Time 硬同步恢复、SNTP 成功后按阈值校准 RTC
✅ TimeManager V2：RTC Write 接入 Critical Operation（Acquire/Release 配对）
✅ TimeManager V2：system.time 时间查询命令（系统时间 + RTC 时间，非阻塞）
✅ Valve 阀门控制模块
✅ Weight 称重模块基础框架、HX711 数据采集及重量状态管理
✅ Weight 异常事件机制
✅ Dispense Guard 重量异常自动关阀保护机制
✅ Workflow Framework 基础架构
✅ Workflow Action / Trigger 注册机制
✅ Action / Trigger reset() / start() / poll() 生命周期机制
✅ Workflow JSON 解析及工作流执行框架
✅ Temporary Action 临时动作队列
✅ Action Instance / Descriptor 生命周期及实例隔离规范
✅ Timer / Delay Trigger
✅ Valve Action
✅ Command Manager 命令路由框架
✅ Query / Execute / System Command 分类机制
✅ Cloud Manager MQTT 通信框架
✅ EMQX MQTT Broker 接入
✅ MQTT QoS 1 + ACK 通信机制
✅ MQTT UP / DOWN Topic 通信模型
✅ Cloud Protocol JSON 消息格式
✅ Command → Cloud Protocol → MQTT 上行链路
✅ MQTT → Cloud Protocol → Command Manager 下行链路
✅ 云端网页 → Worker → EMQX → ESP32 完整命令下发链路
✅ Capability Registry 能力注册及 Stable ID 映射机制
✅ Action / Trigger / Workflow 能力查询接口
✅ 米家 BLE 温湿度计模块基础框架
✅ 米家温湿度计 BLE 扫描及数据状态管理框架
⏳ 米家温湿度计加密广播报文解码算法已完成分析，待正式集成
✅ OLED 模块基础显示框架
⏳ OLED UI 及四按键交互框架待完善
✅ Cloudflare Worker 基础 MQTT 消息接收、解析及 D1 数据保存
✅ Cloudflare Pages 基础调试页面
✅ Pages → Worker → EMQX → ESP32 下行命令链路
✅ EMQX → Pages → Worker → D1 上行消息链路
✅ MQTTX 云端调试链路
⏳ JSON → CBOR → MQTT Binary 压缩方案已完成验证，待正式整合进 Cloud Protocol

---
## 六、开发中 & 待开发计划
🔴 第一阶段：设备基础能力完善

1. System Command

✅ System Restart（含 Safe Restart V2 / Critical Operation）
⏳ System Sync Time（set_time 目前为预留接口，尚未实现写入逻辑）
✅ System Get Time（已实现：system.time，返回 ESP 系统时间 + RTC 时间，非阻塞）
完善系统级 Command 路由及执行结果返回

↓

2. Config Manager 重构

Config JSON 拆分
重新设计 LittleFS 配置文件结构
模块级配置独立存储
配置字段统一类型管理
增加 Config 写入 / 更新接口
增加配置完整性及异常恢复机制
为云端 / UI 修改配置提供基础接口

↓

3. WiFi / MQTT 远程配置

云端修改 WiFi SSID / Password
云端修改 MQTT 配置
Config Manager 持久化
WiFi Manager / Cloud Manager 配置重新加载
配置修改后的安全重连机制
🟠 第二阶段：称重、时间及核心业务能力

4. Weight 完善

Weight Zero
Weight Calibration
重量有效性判断
完善 Weight Error
完善重量异常 Event
完善重量相关 System State

↓

5. RTC / Time Manager【软件部分已完成，待上板验证】

✅ RTC 硬件接入（PCF8563T 驱动已实现，I2C 0x51，复用 OLED 总线）
✅ NTP + RTC 双时间体系（SNTP 为最高可信源，RTC 为启动恢复 / 离线源）
✅ 断网时间维持
✅ 重启后的 RTC 时间恢复（上电硬同步）
✅ 时间可信状态（STATE_TIME_VALID 保持 bool）
✅ 完全非阻塞 SNTP 状态机（callback + loop 延迟确认）

⏳ 待上板验证：当前实测 rtc_present = false，需确认 PCF8563T 接线 / 供电 / 地址
   （串口会打印 [Time] RTC probe failed (addr=0x51 err=N)，凭 err 码定位）

↓

6. Workflow 持久化管理

Workflow JSON 持久化
Workflow 文件独立存储
Workflow 创建 / 修改 / 删除
Workflow reload
Workflow 数据校验
Workflow 与 Capability Registry 联动

暂不急于开发云端 Workflow 编辑器。

必须先稳定：

ConfigManager → LittleFS → Workflow 持久化 → Workflow reload

再开发云端 Workflow 编辑，避免反复返工。

↓

7. Weight Trigger

新增重量相关 Trigger，例如：

WEIGHT_DECREASE
后续可扩展 WEIGHT_ABOVE
WEIGHT_BELOW
其他重量条件

最终实现：

VALVE_OPEN → WEIGHT_DECREASE(20g) → VALVE_CLOSE

形成完整的定量供水 Workflow。

🟡 第三阶段：可靠性及设备化

8. Log Manager

ESP32 本地 Log 框架
Event → Log
Error → Log
Command → Log
Workflow → Log
关键运行状态记录
RAM Buffer
LittleFS 持久化
环形日志 / 日志容量控制
降低 Flash 写入频率
Log 上传云端

↓

9. Device State Heartbeat

周期性设备状态上报
System State → Cloud
在线状态维护
最后在线时间
MQTT 心跳 / Device Heartbeat
云端设备在线 / 离线判断
🔵 第四阶段：UI、云端及完整产品功能
OLED 完整 UI
四按键本地操作
OLED System State 可视化
米家温湿度计解码算法正式集成
温湿度 System State 完善
温湿度数据云端上报
Cloud Protocol CBOR 正式整合
Cloudflare D1 数据结构完善
System State 数据库
MQTT Message / Message Log 数据库
Device Log 数据库
Cloudflare Worker API 完善
Pages 完整 Web UI
Capability Registry → Web UI 动态能力展示
Action / Trigger / Workflow 可视化操作
手机 App
步进电机自动出粮
定量猫粮粉末投喂
投喂异常检测
完整故障检测及远程诊断

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
采用 SNTP网络校准 + PCF8563T RTC硬件兜底 双时间体系，保障设备全天候时间精准有效，断网不失效：
- 上电恢复：启动阶段（time_init，须晚于 oled_init）读取 RTC，时间有效则硬同步设置 System Time 并置 time_valid = true；RTC 无效不是错误，不阻塞启动，等 SNTP
- 首次联网：WiFi 连接成功后启动 SNTP，采用平滑同步（Smooth Sync），不产生时间跳变
- 长期运行：固定 24h 校时周期，用单调时钟计时，不依赖可跳变的 System Time
- 断线重连：尚未同步成功过则立即重启 SNTP 拉取；已同步过则交给 lwip 按周期自行校时
- RTC 校准：SNTP 成功后，|System Time − RTC| > 阈值（config rtc.calibrate_threshold_sec，默认 2s）才写 RTC；写入值取 time(nullptr)（已稳定的系统时间），不使用 SNTP callback 参数
- 断网兜底：断网、重启、无 SNTP 场景下，由 PCF8563T 维持时间，保障定时业务正常运行
- 时间基准：RTC 与 System Time 内部统一 UTC / Unix Timestamp，时区只用于显示
8.4 系统状态中心硬性接口规范
为保障项目架构统一、无耦合、可长期维护，确立全局唯一数据交互规范，所有模块严格遵守：
- 绝对禁止：各业务模块之间直接 extern 跨模块变量、直接读写对方内部状态
- 强制规范：所有状态交互必须经过 system_state 状态中心
- 标准数据流：业务模块更新状态中心数据 → 显示/云端/控制模块读取状态中心数据，单向流动、互不干扰
8.5 TimeManager V2 硬性规范（SNTP 状态机 / RTC 校准 / 时间查询）
本章为 TimeManager V2 定版规范，后续修改时间模块必须严格遵守。

8.5.1 SNTP 完成判定：禁止用 status != IN_PROGRESS
本项目基于 ESP-IDF v4.4（Arduino core 2.x），SNTP API 为 esp_sntp_*（esp_netif_sntp_* 是 IDF v5.0 才引入，本 SDK 不存在）。
SDK 头文件 esp_sntp.h 明确说明：
  时间同步完成后状态为 SNTP_SYNC_STATUS_COMPLETED，
  但 COMPLETED 是瞬时状态，随后会被自动重置为 SNTP_SYNC_STATUS_RESET 等待下一个同步周期。
同时「尚未同步」的初始状态也是 RESET，且 loop 每 500ms 才采样一次，几乎不可能稳定捕捉到瞬时的 COMPLETED。
结论：
  ✗ 禁止 status != IN_PROGRESS —— 等价于把 RESET（含「从未同步」与「已同步后回落」）误判为成功
  ✓ 采用「callback 通知 + loop 延迟确认」三重判据：
      a) sntp_sync_seq 变化 = callback 已通知，确实发生过一次同步（可靠证据）
      b) 排除 IN_PROGRESS，避免 Smooth Sync（adjtime）未收敛就写 RTC；超时兜底防挂死
      c) RESET 需再经过稳定窗口，确保 adjtime 收敛

8.5.2 SNTP callback 约束（运行在 lwip 上下文）
  - callback 只做轻量标记：记录 tv / 通知时刻 / 自增 seq
  - 严禁在 callback 中执行 I2C（Wire.*）、rtc_read_time()、rtc_write_time()
  - 严禁在 callback 中执行长耗时操作
  正确顺序：
    SNTP callback（只置标记）
      ↓
    loop 检测 + 确认 Smooth Sync 已完成
      ↓
    读取 time(nullptr) → 读取 RTC → 比较 → 必要时写 RTC

8.5.3 RTC 校准写入值必须是 time(nullptr)
  - sntp_sync_tv_sec 表示「服务器给出的时间」，是同步时刻的瞬时值
  - Smooth Sync 模式下 System Time 由 adjtime 逐步逼近，callback 触发瞬间可能尚未收敛
  - RTC 应最终跟随已稳定后的 System Time，故写入值一律取 time(nullptr)

8.5.4 RTC 校准规则
  RTC 读取成功：
    |SystemTime − RTCTime| <= threshold  → 不写 RTC
    |SystemTime − RTCTime| >  threshold  → System Time → RTC
  RTC 读取失败（I2C 失败 / VL bit 置位 / BCD 非法）：
    尝试用当前 System Time 写入 RTC
  RTC 写入失败：
    不得置 STATE_TIME_VALID = false；只记录错误，等待下一次 SNTP 同步重试

8.5.5 Critical Operation 边界
  RTC Read  → 非 Critical
  RTC Write → Critical（Acquire → rtc_write_time() → Release）
  硬性要求：
    - Acquire 失败直接 return，不得 Release
    - 写入成功与失败路径都必须 Release
    - 禁止扩大 Critical Operation 范围

8.5.6 I2C 总线归属
  - I2C 总线由 OLED 初始化（Wire.begin / setClock），RTC 只复用，禁止重复初始化
  - rtc.json 的 sda/scl 仅作校验提示，实际引脚以 oled.json 为准（不一致会打印 WARN）
  - 本项目所有业务模块均在同一个 loop() 中轮询运行，不存在多 FreeRTOS Task 并发访问 I2C，
    因此不引入 Mutex / Semaphore；只需保证每次 Wire 操作都是完整、正常结束的 I2C transaction

8.5.7 时间查询命令（system.time）
  云端下发（新格式）：
    {"c":"system","i":"<唯一命令 id>","p":{"o":"time"}}
  旧格式（调试透传）：
    {"cmd":"system","ob":"time","id":"<唯一命令 id>"}
  返回 data 字段：
    system_valid / system_unix / system_str   ESP 系统时间（本地时区字符串）
    rtc_present / rtc_valid / rtc_unix / rtc_str   RTC 芯片时间（无芯片时 rtc_str = "No Time"）
    source          当前时间来源（RTC / SNTP / INVALID）
    last_ntp_sync   最近一次 SNTP 获得的 Unix 时间
    ntp_started / timezone_offset_h
  约束：非阻塞，立即返回；禁止在命令执行路径中做 I2C 重试等待


## 九、工程开发约束（长期维护）
## 代码提交规则

1. src目录只保存当前有效代码
2. 测试代码进入backup
3. 一个模块对应一个cpp/h
4. 禁止setup()/loop()重复定义
5. 新功能必须先确定所属模块
6. 修改架构必须同步更新README




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


# Workflow Action 编码规范（精简强制执行版）
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




# 云端通信协议（MQTT JSON）

## 1. 通信模型

设备与云端通过 MQTT 通信。

通信采用统一 JSON 消息格式：

- 云端 → 设备：发送控制命令
- 设备 → 云端：返回 ACK、执行结果、状态、数据


所有消息均使用同一个 JSON 外壳。


---

# 2. 统一 JSON 格式（核心协议）

所有 MQTT 消息必须符合以下结构：

```json
{
    "cmd":"execute_action",
    "ob":"VALVE_OPEN",
    "id":"1785514667334",
    "pl":{},
    "src":"cloud",
    "ts":1785514667
}
```


## 字段说明




| 字段 | 含义 | 类型 | 说明 |
cmd    |     command      |    消息类型，决定消息行为，采用枚举匹配，查询command.h |
ob      |    object       |  操作对象或目标，由各模块定义其动作函数名|
id      |    cmd_id        |  消息关联ID，用于匹配请求和响应，采用发出命令时的unix时间戳+毫秒 |
pl      |    payload       |  自定义数据区域 |
src      |   source       | 消息来源 |
ts       |   timestamp       | Unix时间戳 |

---

# 3. 字段设计原则

## command

表示消息意图。


Command Manager 根据 command 进行解析和路由。


---

## object


表示操作目标。


例如：

```
VALVE_OPEN

FEED_WORKFLOW

SYSTEM

WEIGHT
```


---

## id

用于关联一次完整通信过程。


例如：

云端发送：

```
1785514667334
```


设备返回：

```
1785514667334
```


表示属于同一次请求。


一个 id 可以对应多个响应消息。


例如：

```
1785514667334
 |
 +---- ack
 |
 +---- action_result
 |
 +---- state_report
```



---

## payload


自定义数据区域。


Command Manager 不解析具体业务内容。


payload 由目标模块自行定义。


例如 Action 参数：


```json
{
    "id":"cmd_001",

    "command":"execute_action",

    "object":"MOTOR_MOVE",

    "payload":
    {
        "speed":100,
        "position":500
    },

    "timestamp":1785514667,

    "source":"cloud"
}
```


Motor 模块自行解析 payload。


---

# 4. Command 枚举


## 4.1 云端 → 设备


请求类型：

```text
execute_action

execute_workflow


query_state

query_actions

query_triggers

query_workflows


system_restart

system_sync_time

system_get_time


query_config

update_config
```


---

## 4.2 设备 → 云端


响应类型：

```text
ack


action_result

workflow_result


state_report


config_upload

log_upload


error


device_online

device_offline
```


---

# 5. ACK协议


设备收到命令后立即返回 ACK。


示例：

```json
{
    "id":"cmd_001",

    "command":"ack",

    "object":"VALVE_OPEN",

    "payload":
    {
        "state":"received"
    },

    "timestamp":1785514668,

    "source":"device"
}
```


说明：

ACK 仅表示：

```
设备已经收到消息
```


不代表执行完成。


---

# 6. Action执行协议


## 请求


云端：

```json
{
    "id":"cmd_001",

    "command":"execute_action",

    "object":"VALVE_OPEN",

    "payload":
    {
    },

    "timestamp":1785514667,

    "source":"cloud"
}
```


---

## 执行完成


成功：

```json
{
    "id":"cmd_001",

    "command":"action_result",

    "object":"VALVE_OPEN",

    "payload":
    {
        "state":"success"
    },

    "timestamp":1785514670,

    "source":"device"
}
```


失败：

```json
{
    "id":"cmd_001",

    "command":"action_result",

    "object":"VALVE_OPEN",

    "payload":
    {
        "state":"failed",
        "error":"timeout"
    },

    "timestamp":1785514670,

    "source":"device"
}
```


---

# 7. Workflow执行协议


请求：

```json
{
    "id":"cmd_002",

    "command":"execute_workflow",

    "object":"FEED_CAT",

    "payload":
    {
    },

    "timestamp":1785514700,

    "source":"cloud"
}
```


结果：

```json
{
    "id":"cmd_002",

    "command":"workflow_result",

    "object":"FEED_CAT",

    "payload":
    {
        "state":"success"
    },

    "timestamp":1785514800,

    "source":"device"
}
```


---

# 8. 查询协议


所有查询统一使用：

```
query_xxx
```


例如：

查询系统状态：

```json
{
    "id":"cmd_003",

    "command":"query_state",

    "object":"system",

    "payload":
    {},

    "timestamp":1785514900,

    "source":"cloud"
}
```


返回：

```json
{
    "id":"cmd_003",

    "command":"state_report",

    "object":"system",

    "payload":
    {
        "state":"success",

        "data":
        {
            "wifi":true,
            "valve":false
        }
    },

    "timestamp":1785514901,

    "source":"device"
}
```


---

# 9. Source来源


用于日志和追踪。


当前：

```text
cloud

device

ui

local
```


未来新增控制端无需修改协议。


---

# 10. 时间格式


设备内部统一使用：

```
Unix timestamp
```


例如：

```
1785514667
```


进入用户显示层时转换为：

```
2026-08-01 02:51:07
```


转换由 Cloud Manager 或 UI 层完成。


---

# 11. MQTT发送规则


设备发送消息：

普通广播：

```
topic/set
```


仅更新云端：

```
topic/up
```


规则：

|消息|方式|
|-|-|
|ACK|up|
|Action结果|up|
|Workflow结果|up|
|状态上传|up|
|配置上传|up|
|日志上传|up|
|需要通知其他设备|set|


---

# 12. 扩展原则


协议冻结以下部分：

固定：

```
id
command
object
payload
timestamp
source
```


新增功能：

只允许增加：

```
command枚举
object定义
payload结构
```


禁止修改基础 JSON 外壳。




# 
# Workflow Trigger / Action 模块注册规范
#

## 1. 概述

所有业务模块通过 Workflow Framework 注册自身提供的：

- Trigger（触发器）
- Action（动作）

注册完成后，由 Workflow Registry 统一维护模块能力信息。

其他模块：

- Command Manager
- UI
- Cloud Manager
- 自动化流程编辑器

均通过 Workflow 查询接口获取可用 Trigger / Action。

模块自身不维护额外注册表。
---

# 2. 模块注册结构

Workflow 模块结构：

```
Module
 |
 |
 +---- Trigger Descriptor
 |
 +---- Action Descriptor
 |
 +---- Register Function
```

运行流程：

```
Workflow Step
       |
       |
 Trigger / Action Instance
       |
       |
 Descriptor
       |
       |
 start()
       |
       |
 poll()
       |
       |

 SUCCESS / FAILED
```

# 3. Trigger 注册模板


## 3.1 Trigger Reset（必须）


用于 Workflow 重新执行前初始化状态。


```cpp
static void module_trigger_reset(
    WorkflowTriggerInstance *trigger
)
{
    if(trigger == nullptr)
        return;
    trigger->state =
        TRIGGER_IDLE;
    trigger->running =
        false;
    // 清理模块私有 runtime

}
```


---

## 3.2 Trigger Start（必须）


Workflow 第一次进入 Trigger Step 时调用。

```cpp
static void module_trigger_start(
    WorkflowTriggerInstance *trigger
)
{
    if(trigger == nullptr)
        return;
    // 初始化 runtime

    trigger->state =
        TRIGGER_RUNNING;
}
```


---

## 3.3 Trigger Poll（必须）


Workflow Task 周期调用。

要求：

- 非阻塞
- 完成后修改 state


```cpp
static void module_trigger_poll(
    WorkflowTriggerInstance *trigger
)
{
    if(trigger == nullptr)
        return;


    // 查询条件


    if(condition_ok)
    {
        trigger->state =
            TRIGGER_SUCCESS;
    }
}
```


---

# 4. Trigger Descriptor


示例：


```cpp
static WorkflowParam trigger_params[] =
{
    {
        "threshold",
        PARAM_INT,
        "",
        "触发阈值"
    }
};


static const WorkflowTriggerDescriptor
module_trigger_desc =
{
    .id =
        "MODULE_TRIGGER",
    .name =
        "触发名称",
    .module =
        "module",
    .description =
        "触发说明",
    .params =
        trigger_params,
    .param_count =
        1,
    .reset =
        module_trigger_reset,
    .start =
        module_trigger_start,
    .poll =
        module_trigger_poll
};
```

---

# 5. Action 注册模板


Action 支持：

- 同步动作
- 异步动作


生命周期：


```
reset()
   |
start()
   |
poll()
   |
SUCCESS / FAILED
```
---

# 5.1 Action Reset


```cpp
static void module_action_reset(
    WorkflowActionInstance *action
)
{
    if(action == nullptr)
        return;


    action->result =
        ACTION_IDLE;


    action->running =
        false;


    action->runtime =
        nullptr;
}
```


---

# 5.2 Action Start
第一次执行 Action 时调用。
```cpp
static void module_action_start(
    WorkflowActionInstance *action
)
{
    if(action == nullptr)
        return;
    // 执行动作
    action->result =
        ACTION_RUNNING;
    // 如果立即完成

    action->result =
        ACTION_SUCCESS;
}
```
---

# 5.3 Action Poll
异步任务继续执行。
```cpp
static void module_action_poll(
    WorkflowActionInstance *action
)
{
    if(action == nullptr)
        return;
    // 查询执行状态
    if(done)
    {
        action->result =
            ACTION_SUCCESS;
    }
    if(error)
    {
        action->result =
            ACTION_FAILED;
    }
}
```
---

# 6. Action Descriptor

示例：

```cpp
static WorkflowParam action_params[] =
{
    {
        "duration",
        PARAM_INT,
        "ms",
        "执行时间"
    }
};

static const WorkflowActionDescriptor
module_action_desc =
{
    .id =
        "MODULE_ACTION",
    .name =
        "动作名称",
    .module =
        "module",
    .description =
        "动作说明",
    .params =
        action_params,
    .param_count =
        1,
    .reset =
        module_action_reset,
    .start =
        module_action_start,
    .poll =
        module_action_poll
};
```

---

# 7. 模块统一注册入口


每个模块提供：

```cpp
void module_workflow_register();
```

示例：
```cpp
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
系统启动时调用：

```cpp
module_workflow_register();
```



---

# 8. ID 命名规范


| 类型 | 格式 | 示例 |
|-|-|-|
| Trigger | 大写 + 下划线 | `WEIGHT_ABOVE` |
| Action | 大写 + 下划线 | `VALVE_OPEN` |



规则：

- 全部大写
- 单词之间使用 `_`
- 必须唯一
- 不允许重复注册



---

# 9. 参数规范


统一使用：

```cpp
WorkflowParam
```
结构：

```cpp
{
    name,
    type,
    unit,
    description
}
```
示例：

```cpp
{
    "speed",
    PARAM_INT,
    "rpm",
    "电机转速"
}
```
参数类型：
| 类型 | 说明 |
|-|-|
| PARAM_INT | 整数 |
| PARAM_FLOAT | 浮点 |
| PARAM_BOOL | 布尔 |
| PARAM_STRING | 字符串 |
---

# 10. 查询接口
Workflow Registry 提供：

## 查询 Trigger
```cpp
workflow_get_trigger_count();

workflow_get_trigger_descriptor(index);
```
## 查询 Action
```cpp
workflow_get_action_count();

workflow_get_action_descriptor(index);
```

Command Manager / UI 不直接访问模块。

统一通过 Workflow Framework 获取能力。
---

# 11. Cloud Command 流程


云端下发：


```
Cloud

 |

Cloud Manager

 |

Command Manager

 |

Workflow Action Registry

 |

workflow_enqueue_action()

 |

Temporary Action Queue

 |

Action Instance

 |

start()

 |

poll()

 |

SUCCESS / FAILED

```



---

# 12. 开发注意事项


## Trigger

必须保证：

- start 非阻塞
- poll 非阻塞
- 不使用 delay()
- 完成修改 state


例如：

```cpp
trigger->state =
    TRIGGER_SUCCESS;
```

---

## Action
同步动作：

```
start()

直接 SUCCESS
```


异步动作：

```
start()

ACTION_RUNNING


poll()

ACTION_SUCCESS
```

禁止：

- 阻塞等待
- while等待完成
- delay()
- 自建任务循环
---

# 13. 当前支持模块

| 模块 | Trigger | Action |
|-|-|-|
| Timer | ✅ | ❌ |
| Delay | ✅ | ❌ |
| Valve | ❌ | ✅ |
| Motor | 待开发 | 待开发 |


---

# 14. 总结

Workflow Framework 负责：

- 生命周期管理
- Step 调度
- Trigger / Action Registry
- JSON解析
- Cloud Action 调用
- 临时 Action 队列

业务模块只负责：

- Descriptor定义
- start()
- poll()
- reset()
实现模块与自动化系统完全解耦。