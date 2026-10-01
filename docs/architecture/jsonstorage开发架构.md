JSON Storage 模块设计规格
1. 目标

新增一个通用的 JSON Storage 基础模块，为项目提供统一的 LittleFS 文件读写能力。

本阶段只实现：

src/json_storage.h
src/json_storage.cpp

禁止修改任何其他文件。

JSON Storage 是底层通用存储组件，不属于 Config Manager，也不属于 Workflow Manager。

它只负责：

文件
 ↓
读取 / 写入
 ↓
String / buffer / stream

不负责：

JSON字段解析
业务参数校验
Config版本管理
Workflow业务逻辑
Cloud/MQTT
Command
日志
2. 核心设计原则
2.1 JSON Storage 不理解业务

JSON Storage 不知道：

config.json
workflow.json
log.json

里面分别有什么字段。

它只知道：

“这是一个文件，需要把数据可靠地读出来或者写进去。”

因此：

JsonDocument
JsonObject
JsonArray

都不是 JSON Storage 必须理解的业务对象。

业务模块自行负责：

JSON解析
JSON生成
字段转换
数据合法性判断

JSON Storage 只提供底层存储 API。

3. 与 Config Manager 的关系

最终架构：

                    Cloud / Command
                           │
                           ▼
                    Config Manager
                           │
             ┌─────────────┴─────────────┐
             │                           │
             ▼                           ▼
       Config业务逻辑                Version管理
             │                           │
             └─────────────┬─────────────┘
                           ▼
                     JSON Storage
                           │
                           ▼
                         LittleFS

JSON Storage 不允许反向依赖 Config Manager。

即：

Config Manager → JSON Storage

允许。

JSON Storage → Config Manager

禁止。

4. 第一阶段严格范围

本次 JSON Storage 只新增：

src/json_storage.h
src/json_storage.cpp

本阶段：

不修改 config_manager.cpp
不修改 config_manager.h
不修改 workflow.cpp
不修改 workflow.h
不修改 cloud_manager.cpp
不修改 command_manager.cpp
不修改任何其他文件
不修改现有 Workflow JSON 格式
不修改现有 Config JSON 格式
不接入 MQTT
不接入 CommandManager
不增加任何新的业务逻辑

如果实现过程中发现必须修改其他文件：

停止实现，不得自行修改。

必须先输出：

需要修改的文件：
1. xxx
2. xxx

修改原因：
...

修改内容：
...

为什么 JSON Storage 本身无法解决：
...

等待下一阶段明确批准。

5. LittleFS 生命周期

JSON Storage 应尽量复用项目已有的 LittleFS。

不能让每一次简单的读写都重复：

LittleFS.begin(...)
LittleFS.end()

如果系统架构已经在启动阶段完成 LittleFS 初始化，则 JSON Storage 默认直接使用已经挂载的 LittleFS。

JSON Storage 不负责决定整个系统何时初始化/卸载 LittleFS。

因此应提供明确的初始化接口，例如：

bool json_storage_init();

但初始化策略必须保持简单。

不要为了“兼容未来 RTOS”设计复杂的线程锁、任务、事件等机制。

当前项目的基本原则是：

setup()
    ↓
初始化
    ↓
loop()
    ↓
非阻塞状态机

JSON Storage 必须符合当前架构。

6. 基础 API

建议头文件提供以下能力。

6.1 初始化
bool json_storage_init();

作用：

确认 LittleFS 可用
完成 JSON Storage 自身初始化
返回成功/失败
7. 文件存在检查
bool json_storage_exists(const char *path);

用于：

config.json 是否存在
config.backup.json 是否存在
workflow_x.json 是否存在
8. 获取文件大小
size_t json_storage_size(const char *path);

用途：

决定是否可以一次性读取
决定是否需要分块读取
决定内存需求

如果文件不存在或无法获取：

return 0;
9. 删除文件
bool json_storage_remove(const char *path);

只负责文件删除。

不负责：

backup策略
version策略
recovery策略

这些全部由上层 Config Manager 决定。

10. 重命名文件
bool json_storage_rename(
    const char *from,
    const char *to
);

这是 Config Manager 实现原子替换的重要基础能力。

例如：

config.new
    ↓
config.json

或者：

config.json
    ↓
config.backup.json

JSON Storage 只执行 rename。

不理解为什么 rename。

11. 一次性读取

对于较小 JSON：

bool json_storage_read(
    const char *path,
    String &output
);

行为：

文件
 ↓
String

适用于：

小型 config JSON
小型 workflow JSON

但是实现必须避免不必要的重复内存。

不要：

File
 ↓
临时String
 ↓
再次复制String

应该尽可能直接构造最终结果。

12. 一次性写入
bool json_storage_write(
    const char *path,
    const String &data
);

只负责：

String
 ↓
文件

必须检查：

open成功
write成功
实际写入长度 == data.length()
close成功

失败必须返回：

false

不能把“打开成功”当成“写入成功”。

13. 原子写入

建议 JSON Storage 提供基础能力：

bool json_storage_write_atomic(
    const char *path,
    const String &data
);

实现原则：

data
 ↓
path.tmp
 ↓
完整写入
 ↓
确认写入成功
 ↓
close
 ↓
rename
 ↓
正式文件

禁止：

直接打开正式文件
 ↓
写一半
 ↓
断电
 ↓
正式文件损坏

JSON Storage 的原子写只负责：

“一个文件如何可靠地替换成另一个完整文件。”

它不负责 backup。

14. 为什么 Atomic Write 与 Backup 必须分开

Config Manager 最终需要：

config_v10
config_v11

或者逻辑上的：

current
backup

JSON Storage 不应该知道：

哪个是 current
哪个是 backup
哪个 version 更新

因此：

JSON Storage
    ↓
提供 atomic write / rename / remove

Config Manager
    ↓
决定文件如何轮换
    ↓
决定什么时候更新 version
    ↓
决定什么时候 recovery
15. 分块读取

必须考虑大文件。

例如：

1024 KB JSON

不能默认：

String json;
json.reserve(1024 * 1024);

因为 ESP32 的 RAM 不应该被一个大型文件一次性占满。

因此 JSON Storage 应提供基础的分块读取能力。

推荐接口：

bool json_storage_read_chunk(
    const char *path,
    size_t offset,
    uint8_t *buffer,
    size_t buffer_size,
    size_t &bytes_read
);

行为：

文件
 ↓
offset
 ↓
读取最多 buffer_size
 ↓
buffer

例如：

offset = 0
size = 512

offset = 512
size = 512

offset = 1024
size = 512

直到：

bytes_read == 0

表示 EOF。

16. 分块写入

同样需要支持：

bool json_storage_write_chunk(
    const char *path,
    size_t offset,
    const uint8_t *data,
    size_t length
);

但是：

第一阶段不允许为了这个接口自行设计复杂的“随机写文件协议”。

如果 ESP32 LittleFS 的文件写入模式不适合安全地做任意 offset 覆盖，应采用：

临时文件
+
顺序追加写入

而不是强行实现复杂随机写。

17. 更推荐的 Streaming API

为了未来支持：

大 JSON
BIN
OTA
Log
Cloud Transfer

可以额外提供简单的文件流接口。

例如：

struct JsonStorageFile
{
    File file;
};

提供：

bool json_storage_open_read(
    const char *path,
    JsonStorageFile &file
);

bool json_storage_open_write(
    const char *path,
    JsonStorageFile &file
);

size_t json_storage_read(
    JsonStorageFile &file,
    uint8_t *buffer,
    size_t buffer_size
);

size_t json_storage_write(
    JsonStorageFile &file,
    const uint8_t *data,
    size_t length
);

bool json_storage_close(
    JsonStorageFile &file
);

如果 Arduino/ESP32 的 File 本身已经足够稳定，也可以直接封装，而不是重新设计一套复杂 stream abstraction。

18. 分块大小由谁决定？

JSON Storage 不应该固定规定业务层必须使用多少字节。

例如不要设计成：

json_storage_read_chunk(..., 1024);

然后内部强制 1024。

应该由调用者提供：

buffer
buffer_size

例如：

Config Manager
    → 512 bytes

Cloud Transfer
    → 256 bytes

OTA
    → 4096 bytes

这样：

JSON Storage

只提供：

“给我一个 buffer，我帮你读/写。”

而：

Config Manager / CloudManager / OTA

决定自己的内存预算和传输策略。

19. 推荐的分块大小原则

当前 ESP32-S3 项目建议：

256 ~ 1024 bytes

作为普通文件操作的合理范围。

但 JSON Storage 本身不要写死。

尤其不要因为 MQTT 当前约 256 字节的单包限制，就让 JSON Storage 也固定成 256 字节。

两者属于不同层：

Flash IO chunk
≠
MQTT packet chunk

未来 CloudManager 可以：

JSON Storage
    ↓
读取 512 bytes
    ↓
组装 Transfer Chunk
    ↓
MQTT

也可以：

JSON Storage
    ↓
读取 1024 bytes
    ↓
拆成多个 MQTT Chunk

JSON Storage 不关心。

20. JSON Storage 不负责 JSON 解析

例如 Config Manager：

String json;

json_storage_read(
    "/config.json",
    json
);

deserializeJson(doc, json);

Workflow Manager：

String json;

json_storage_read(
    "/workflow.json",
    json
);

deserializeJson(doc, json);

未来其他模块也可以：

Log
Registry
Device Profile
Calibration

自行决定：

ArduinoJson
CBOR
Binary
Text

JSON Storage 的名字虽然叫 JSON Storage，但底层实际上应该尽可能保持“文件存储”思维。

如果 API 不依赖 ArduinoJson，则未来甚至可以用于非 JSON 文件。

21. 内存原则

必须遵守：

小文件

允许：

File
 ↓
String
 ↓
ArduinoJson
大文件

必须支持：

File
 ↓
small buffer
 ↓
处理
 ↓
释放/复用 buffer

禁止设计：

整个文件
+
临时副本
+
ArduinoJson Document
+
String

同时存在于 RAM。

22. 错误处理

JSON Storage 所有操作尽可能采用：

bool

或者明确的：

size_t

返回值。

错误由调用者决定如何处理。

例如：

文件不存在
文件打开失败
读取失败
写入失败
空间不足
rename失败
remove失败

JSON Storage：

return false

Config Manager：

决定 recovery
决定 backup
决定 log
决定是否重启
23. Log 接口预留

本阶段不要直接依赖 Log Manager。

因为 Log Manager 还没有实现。

JSON Storage 内部可以保留非常轻量的日志接口，例如：

typedef void (*JsonStorageLogCallback)(
    const char *level,
    const char *message
);

void json_storage_set_log_callback(
    JsonStorageLogCallback callback
);

但是：

如果为了这个 callback 导致代码明显复杂，则可以暂时不实现。

最终目标是未来：

JSON Storage
    ↓
Log callback
    ↓
Log Manager

而不是：

JSON Storage
    ↓
Serial.println()

大量散落在底层模块。

24. 与 Config Manager 的最终关系

JSON Storage 完成以后，Config Manager 才负责实现：

Load
Get
Set
Save
Backup
Recovery
Version
Cloud Update
Cloud Upload

其中：

Load
Config Manager
    ↓
JSON Storage
    ↓
current config

失败：

current失败
    ↓
backup
    ↓
recovery
25. Config Version 不属于 JSON Storage

Config Manager 单独维护：

config_versions.json

或者其他专用版本文件。

这个文件记录：

module
version

例如：

{
    "wifi": 10,
    "oled": 7,
    "mi_thermo": 4,
    "system": 12
}

JSON Storage 只负责保存这个文件。

它不理解 version。

26. Config Backup 不属于 JSON Storage

最终 Config Manager 可能实现：

module.json
module.backup.json
module.tmp

更新流程：

新配置
   ↓
module.tmp
   ↓
完整写入成功
   ↓
旧 current → backup
   ↓
tmp → current
   ↓
更新 version

具体顺序、失败恢复和 version 一致性：

全部属于 Config Manager。

JSON Storage 只提供：

write
write_atomic
rename
remove
read
27. Workflow 未来迁移

当前 Workflow 已经拥有自己的 JSON 文件读写实现，例如当前代码直接 LittleFS.open()、逐字节读取到 String，保存时直接打开文件并写入。

未来可以逐步改造成：

Workflow Manager
       ↓
JSON Storage
       ↓
LittleFS

但是：

本次 JSON Storage 开发禁止修改 Workflow。

下一阶段单独做 Workflow Storage 迁移。

28. 不允许做的事情

本次本地 AI 实现严禁：

❌ 修改 Config Manager
❌ 修改 Workflow Manager
❌ 修改 Cloud Manager
❌ 修改 Command Manager
❌ 修改 System State
❌ 修改 Time Manager
❌ 修改 platformio.ini
❌ 修改项目目录结构
❌ 自动重构现有代码
❌ 自动替换现有 LittleFS 代码
❌ 增加 MQTT
❌ 增加 Config Version
❌ 增加 Backup 策略
❌ 增加 Recovery 策略
❌ 增加业务 JSON Schema
❌ 修改现有 JSON 格式
29. 本阶段交付物

只允许产生：

src/json_storage.h
src/json_storage.cpp

要求：

pio run

能够正常编译。

如果编译需要修改其他文件：

不允许自行修改。

只报告需要修改的文件和原因。

30. 最终架构定位

整个存储体系最终应该形成：

                ┌──────────────────┐
                │   ConfigManager  │
                │                  │
                │ Load/Get/Set     │
                │ Save/Backup      │
                │ Recovery         │
                │ Version          │
                │ Cloud Command    │
                └────────┬─────────┘
                         │
                         ▼
                ┌──────────────────┐
                │  JSON Storage    │
                │                  │
                │ read             │
                │ write            │
                │ atomic write     │
                │ read chunk       │
                │ write chunk      │
                │ rename           │
                │ remove           │
                │ exists           │
                │ size             │
                └────────┬─────────┘
                         │
                         ▼
                     LittleFS

JSON Storage 是：

最底层、最简单、最稳定的文件存储基础设施。

它不参与任何业务决策。

Config Manager 才是：

整个用户自定义配置数据的管理者。

31. 实现完成后的验收要求

本阶段至少验证：

Test 1 — 文件不存在
read nonexistent
→ false
Test 2 — 普通写入
write
→ read
→ 内容一致
Test 3 — 原子写入
write_atomic
→ read
→ 内容一致
Test 4 — rename
A → B
→ A不存在
→ B存在
Test 5 — remove
remove
→ exists == false
Test 6 — chunk read
大文件
→ 多次 read_chunk
→ 拼接结果
→ 与原文件完全一致
Test 7 — chunk write
多次写入
→ 文件
→ read
→ 内容完全一致
Test 8 — 编译
pio run

必须通过。

32. 给本地 AI 的最终执行指令

现在只执行 JSON Storage 的实现。

创建：

src/json_storage.h
src/json_storage.cpp

严格按照本文档实现。

不要修改任何其他文件。

如果发现现有工程 API、LittleFS 初始化方式或其他代码结构导致必须修改其他文件：

立即停止，不要自行修改。

只输出：

需要修改的文件
修改原因
预计修改内容

等待用户批准后再进行下一阶段。

当前任务不是 Config Manager 重构，也不是 Workflow 重构。

当前任务只有一个：

建立稳定、通用、低耦合的 JSON Storage 文件存储底座。