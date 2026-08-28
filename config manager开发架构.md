《Config Manager 架构设计定版》**保存。它把我们刚才讨论的职责边界、双版本恢复、版本管理、云端交互、大文件分块，以及 JsonStorage 的依赖关系都固定下来，后面写代码时就以这份为准。

Config Manager 架构设计
1. 模块定位

ConfigManager 是整个系统的基础业务底座，负责管理所有用户可自定义、需要持久化保存的配置数据。

核心目标：

配置可靠保存
配置安全恢复
配置统一 API
支持云端修改
支持配置上传
降低 Flash 写入次数
为后续业务模块提供稳定的配置接口

ConfigManager 不负责业务参数是否合法。

例如：

OLED GPIO = 12
WiFi SSID = xxx
温湿度扫描时间 = 15min

ConfigManager 只负责保存和读取。

至于 GPIO 12 是否适合 OLED、扫描时间是否合理，由对应业务模块负责。

2. 总体架构
                    Cloud / MQTT
                         │
                         ▼
                 CommandManager
                         │
                  Config Command
                         │
                         ▼
                ┌─────────────────┐
                │ ConfigManager   │
                │                 │
                │ load            │
                │ get             │
                │ set             │
                │ save            │
                │ backup          │
                │ recovery        │
                │ version         │
                │ cloud           │
                └────────┬────────┘
                         │
                         ▼
                ┌─────────────────┐
                │  JsonStorage    │
                │                 │
                │ read            │
                │ write           │
                │ atomic write    │
                │ backup          │
                │ recovery        │
                │ chunk read/write│
                │ file management │
                └────────┬────────┘
                         │
                         ▼
                      LittleFS

职责严格分层：

ConfigManager

负责：

配置管理
配置版本管理
Active / Backup 管理
配置加载
配置 API
云端配置 Command
启动成功确认
JsonStorage

负责：

JSON / 文件的实际存储
原子写
Backup 文件操作
文件恢复
分块读写
文件存在性、大小等基础操作
LittleFS

只作为底层文件系统使用。

3. ConfigManager 能力边界

ConfigManager 提供以下核心能力：

load()
get()
set()
save()
backup()
recovery()
version management
cloud command
boot validation
3.1 Load

系统启动时读取配置。

正常情况：

Active Config
      ↓
读取成功
      ↓
加载到 RAM

如果 Active 配置无法读取：

Active Config
      ↓
读取失败
      ↓
Backup Config
      ↓
读取 Backup

Backup 是系统的第二套配置。

4. 双版本配置机制

Config 永远保留：

Active
Backup

两个版本。

例如：

config_xxx.json
config_xxx.json.bak

当新版本配置写入成功：

Version 10
Active

Version 9
Backup

云端下发 Version 11：

Version 10 → Backup
Version 11 → Active

再次下发 Version 12：

Version 11 → Backup
Version 12 → Active

Version 10 被淘汰。

即：

永远只保留最新版本 + 上一个版本
5. 原子写机制

配置禁止直接覆盖 Active 文件。

错误方式：

config.json
    ↓
直接覆盖

如果写入过程中断电，可能得到损坏的 JSON。

正确方式由 JsonStorage 提供原子写接口：

config.json
     │
     ├── 当前 Active
     │
     └── backup

写入新配置
     ↓
临时文件
     ↓
完整写入
     ↓
写入成功
     ↓
旧 Active → Backup
     ↓
临时文件 → Active

因此：

ConfigManager 不自己实现文件级原子操作，由 JsonStorage 提供。

6. 配置合法性判断边界

ConfigManager 不负责判断业务配置是否合法。

例如：

{
    "oled_sda": 12,
    "oled_scl": 13
}

ConfigManager 不知道：

12 / 13 是否真的适合 OLED

因为 ConfigManager 不应该参与业务逻辑。

因此系统采用：

配置文件结构可读取
        +
所有模块正常完成初始化
        ↓
setup 最后
        ↓
Config Valid

启动完成后才认为当前 Active Config 是可信配置。

如果启动过程中因为配置导致严重异常，例如：

panic
重启
无法执行到 setup 末尾

则无法完成 Valid 标记。

下一次启动可以依据启动成功标记判断当前配置是否可靠，并进行 Backup Recovery。

ConfigManager 不试图判断每个业务字段是否正确，而是通过“完整系统成功启动”作为最终可信依据。

7. Boot Validation

系统启动流程：

LittleFS
   ↓
ConfigManager load
   ↓
各基础模块初始化
   ↓
各业务模块初始化
   ↓
Command / Cloud 初始化
   ↓
所有模块初始化完成
   ↓
ConfigManager boot_validate()

只有执行到最后：

ConfigManager boot_validate()

才记录：

当前配置启动成功

如果没有成功到达这里，则认为上一次启动可能存在配置问题。

8. Version 管理

版本信息不放进业务 JSON 文件内部。

例如：

{
    "ssid": "xxx",
    "password": "xxx"
}

保持纯业务数据。

版本信息由 ConfigManager 单独管理。

例如：

config_version.json

保存：

{
    "config_wifi": 10,
    "config_oled": 4,
    "config_mithermo": 7,
    "workflow_0": 12
}

这样：

业务 JSON 保持干净
ConfigManager 统一管理版本
每个文件可以独立拥有版本号
9. Version 类型

当前不需要复杂版本系统。

使用简单递增数字即可。

建议：

uint8_t

即：

0 ~ 255

达到最大值：

255 → 0

重新循环。

版本只用于判断新旧，不承担软件版本、协议版本等其他职责。

10. 时间戳不作为 Config 版本判断依据

Config 版本不依赖 RTC / NTP 时间。

原因：

系统启动时可能：

time invalid
NTP 尚未同步
RTC 尚未恢复

因此 Config 版本直接使用内部递增版本号。

时间戳可以作为日志、云端记录等其他用途，但不参与 Config Active / Backup 的版本判断。

11. Config 文件组织方式

每个模块独立保存配置。

例如：

/config/
    wifi.json
    oled.json
    valve.json
    weight.json
    mi_thermo.json

而不是一个巨大的：

config.json

这样做的优点：

修改一个模块只写一个文件
减少 Flash 写入
降低单个文件损坏影响范围
模块之间解耦
更容易云端单独修改
更容易进行 Active / Backup 管理

每个配置文件拥有自己的：

Active
Backup
Version
12. Workflow 存储方式

现有 workflow.json 可以进行结构调整。

不再把全部 Workflow 放在一个大文件中：

workflow.json

而改为：

/workflow/
    workflow_0.json
    workflow_1.json
    workflow_2.json
    ...
    workflow_15.json

因为系统最多：

WORKFLOW_MAX_COUNT = 16

修改 Workflow 8：

只修改 workflow_8.json

不需要重新写入整个 Workflow 集合。

WorkflowManager 的业务逻辑保持不变，只调整持久化层。

13. JsonStorage 与 Workflow / Config 的统一

JsonStorage 是统一的 JSON 文件基础设施。

因此：

ConfigManager
       │
       ├── JsonStorage
       │
WorkflowManager
       │
       └── JsonStorage

两个模块可以使用同一套：

read
write
atomic write
backup
recovery
chunk read
chunk write

但：

JsonStorage 不理解 Workflow，也不理解 Config。

例如它不知道：

workflow_8 是什么
oled_gpio 是什么

它只知道：

这是一个文件
这是一些 JSON 数据
14. Config API

业务模块不直接访问 LittleFS。

禁止：

LittleFS.open(...)

业务模块统一通过 ConfigManager：

config_get_xxx()
config_set_xxx()

或者统一 API。

例如：

config_get_mithermometer_mac(...)
config_get_mi_thermo_allow_collect()

未来新增：

config_get_wifi_ssid()
config_get_oled_sda()
config_get_mi_thermo_scan_window()

这样业务模块与实际存储方式完全解耦。

15. 云端 Config Command

Config 的云端修改不参与 Workflow Action。

它属于：

CommandManager

直接注册和处理。

结构：

MQTT
 ↓
CloudManager
 ↓
CommandManager
 ↓
ConfigManager

例如：

config_set

修改配置后：

云端下发
   ↓
ConfigManager set
   ↓
保存新配置
   ↓
旧 Active → Backup
   ↓
新配置 → Active
   ↓
返回结果
   ↓
系统重启

配置修改统一采用重启生效。

暂时不设计：

某些配置实时生效
某些配置需要重启
某些模块局部 reload

这样可以大幅降低系统复杂度。

16. 云端上传 Config

云端请求配置时：

ConfigManager
      ↓
JsonStorage
      ↓
读取 JSON 文件
      ↓
上传 MQTT

云端收到的是完整 JSON。

ConfigManager 不负责：

云端数据库如何保存
云端如何解析
云端如何展示
云端如何比较业务字段

这些属于云端职责。

17. 大文件 / 分块传输

不能假设 JSON 永远很小。

未来可能出现：

几十 KB
几百 KB
甚至 MB

因此不能设计为：

整个文件
 ↓
一次性 malloc
 ↓
整个文件进入 RAM
 ↓
MQTT

而应该支持：

LittleFS
   ↓
小块读取
   ↓
MQTT Chunk
   ↓
云端

例如：

FILE_BEGIN
FILE_CHUNK 0
FILE_CHUNK 1
FILE_CHUNK 2
...
FILE_END

同样：

云端
 ↓
FILE_BEGIN
 ↓
FILE_CHUNK
 ↓
FILE_CHUNK
 ↓
FILE_END
 ↓
LittleFS

这套能力属于 JsonStorage / 文件存储基础设施提供的底层能力。

ConfigManager 只负责调用。

18. RAM 使用原则

禁止因为文件较大而强制：

File Size = RAM Usage

推荐：

File
 ↓
固定大小 Buffer
 ↓
分块处理

PSRAM 可以作为缓冲区使用，但不应该依赖“把整个文件加载到 PSRAM”解决问题。

未来 OTA、日志上传、大 JSON 上传等功能都可以复用这一机制。

19. Flash 写入优化

原则：

没有实际变化，就不写 Flash。

例如：

set(key, value)
       ↓
比较旧值
       ↓
相同
       ↓
不写 Flash

只有真正发生变化时：

旧配置 ≠ 新配置
       ↓
保存
       ↓
生成新 Version
       ↓
Active → Backup
       ↓
新配置 → Active

这样降低：

Flash 擦写次数
文件系统压力
掉电风险
20. Log 接口预留

ConfigManager 的以下重要操作应该预留 Log 接口：

CONFIG_LOAD
CONFIG_LOAD_BACKUP
CONFIG_SAVE
CONFIG_SAVE_FAILED
CONFIG_RECOVERY
CONFIG_VERSION_CHANGED
CONFIG_BOOT_VALID
CONFIG_CLOUD_SET
CONFIG_CLOUD_UPLOAD

第一阶段可以先使用：

Serial

但代码结构上预留：

LOG_INFO(...)
LOG_WARN(...)
LOG_ERROR(...)

未来 Log 模块完成后可以直接接入。

ConfigManager 本身不负责日志存储。

21. Debug 模式

ConfigManager 不保存 debug 配置。

不设计：

"debug": true

各模块自己的 Debug 信息采用编译期宏：

#define MI_THERMO_DEBUG

Debug 代码：

#ifdef MI_THERMO_DEBUG

// debug code

#endif

这样 Release 编译时 Debug 代码可以直接被编译器移除，不占用运行时资源。

22. JsonStorage 第一阶段能力

在开发 ConfigManager 之前，先完成 JsonStorage。

第一版至少提供：

exists()
get_size()

read()
write()

write_atomic()

backup()
recovery()

remove()

read_chunk()
write_chunk()

其中：

read / write

用于普通小型 JSON。

write_atomic

用于安全写入。

backup / recovery

用于 Active / Backup 管理。

read_chunk / write_chunk

用于未来的大文件、MQTT 分片、OTA 等场景。

23. JsonStorage 的职责边界

JsonStorage 只负责文件存储，不负责业务。

它不能知道：

WiFi
OLED
温度计
Workflow
GPIO

它只知道：

文件路径
文件内容
文件大小
文件读写
原子操作
Backup
Recovery
Chunk

这样未来除了 ConfigManager，还可以被：

WorkflowManager
LogManager
OTA
文件传输模块

复用。

24. 最终模块关系
                         MQTT
                           │
                           ▼
                     CloudManager
                           │
                           ▼
                    CommandManager
                           │
             ┌─────────────┴─────────────┐
             ▼                           ▼
      ConfigManager                WorkflowManager
             │                           │
             └─────────────┬─────────────┘
                           ▼
                      JsonStorage
                           │
                           ▼
                        LittleFS

ConfigManager 与 WorkflowManager 都是上层业务模块。

JsonStorage 是底层文件存储基础设施。

25. 开发顺序

最终按照以下顺序实施：

① JsonStorage
       ↓
② ConfigManager
       ↓
③ Workflow JSON 存储改造
       ↓
④ Config 云端 Command
       ↓
⑤ Log Manager

其中：

JsonStorage 必须先定版，再开始 ConfigManager。

因为 ConfigManager 的：

原子写
Backup
Recovery
分块读写
文件管理

全部建立在 JsonStorage 之上。

26. 明确不做的事情

ConfigManager 当前不负责：

判断业务参数是否合法
判断 GPIO 是否正确
判断温湿度计是否存在
实时通知业务模块配置变化
根据不同配置决定是否重启
管理 Debug 开关
管理 Workflow 执行逻辑
管理 MQTT 底层通信
解析云端业务逻辑
保存云端数据库
整个大文件一次性加载到 RAM

这些均属于其他模块职责。

27. 最终设计原则

ConfigManager 的核心原则可以概括为：

ConfigManager 管“配置”，JsonStorage 管“文件”，业务模块管“参数是否合法”。

同时：

Active + Backup + Version + Boot Validation

构成 Config 的基本可靠性机制。

云端修改：

修改 → 原子保存 → Active/Backup 滚动 → 重启 → 全系统重新加载。

大文件：

永远优先考虑分块/流式处理，而不是一次性加载整个文件。

这份可以作为我们后面写 JsonStorage 和 ConfigManager 时的约束文档。