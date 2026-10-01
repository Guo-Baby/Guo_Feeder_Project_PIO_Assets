#pragma once

#include <Arduino.h>
#include <FS.h>

// =====================================================
// JSON Storage — 底层通用文件存储模块（LittleFS）
// =====================================================
//
// 职责：
//   把文件可靠地读出来、写进去。
//
// 不负责：
//   JSON 字段解析、业务参数校验、Config 版本管理、
//   Backup / Recovery 策略、Workflow 业务逻辑、
//   Cloud / MQTT、Command、日志存储。
//
// 依赖方向（严格单向，禁止反向依赖）：
//   ConfigManager / WorkflowManager → JSON Storage → LittleFS
//
// 本模块不依赖 ArduinoJson，因此未来也可用于非 JSON 文件，
// 例如 BIN / OTA / Log / 设备档案 / 校准数据。
// =====================================================


// =====================================================
// 日志回调（可选）
// =====================================================
//
// level 取值：
//   "E" = ERROR
//   "W" = WARN
//   "I" = INFO
//
// 用途：
//   本模块不直接使用 Serial.println，避免底层散落串口输出。
//   未来 Log Manager 完成后，由上层注册回调统一接管。
//
// 不注册回调时，模块静默运行，不影响任何功能。

typedef void (*JsonStorageLogCallback)(
    const char *level,
    const char *message
);

// 设置日志回调。传 nullptr 可关闭日志输出。
void json_storage_set_log_callback(
    JsonStorageLogCallback callback
);


// =====================================================
// 创建目录
// =====================================================
//
// LittleFS 不会在写文件时自动创建父目录。
// 写入"非随固件烧录"的目录前必须先调用本函数，
// 否则 write / write_atomic 会因无法创建 .tmp 文件而失败。
//
// 幂等: 目录已存在时直接返回 true，可安全重复调用。
//
// 返回：
//   true  = 目录已存在或创建成功
//   false = 参数非法 / 未初始化 / 创建失败

bool json_storage_mkdir(const char *path);


// =====================================================
// 初始化
// =====================================================
//
// 确认 LittleFS 可用并完成模块自身初始化。
//
// 说明：
//   系统在 setup 阶段已完成 LittleFS 挂载（一次性）。
//   本函数只做可用性确认，不负责挂载 / 卸载 LittleFS。
//
//   JsonStorage 不调用 LittleFS.begin()。
//   LittleFS 的生命周期完全由系统初始化阶段统一管理。
//
//   重复调用是幂等的：已初始化则直接返回 true。
//
// 返回：
//   true  = 文件系统可用
//   false = 文件系统不可用，后续所有读写接口均会失败

bool json_storage_init();


// =====================================================
// 文件基础操作
// =====================================================

// 文件是否存在
bool json_storage_exists(
    const char *path
);

// 获取文件大小（字节）
//
// 返回：
//   文件大小；文件不存在或无法获取时返回 0
//
// 用途：
//   由调用方决定一次性读取还是分块读取，以及内存预算。

size_t json_storage_size(
    const char *path
);

// 删除文件
//
// 说明：
//   幂等 —— 文件本就不存在时同样返回 true（"确保文件不存在"）。
//   本函数不理解 backup / version / recovery，
//   这些策略全部由上层 ConfigManager 决定。

bool json_storage_remove(
    const char *path
);

// 重命名文件
//
// 说明：
//   目标文件已存在时由文件系统覆盖（LittleFS 的 rename 语义）。
//   这是上层实现原子替换与备份轮换的基础能力，
//   但本函数不理解"为什么 rename"。

bool json_storage_rename(
    const char *from,
    const char *to
);


// =====================================================
// 文件校验
// =====================================================
//
// 计算整个文件的 CRC32。
//
// 特点：
//   - 流式读取
//   - 不会把整个文件加载到 RAM
//   - 内部使用固定大小缓冲区
//   - 可用于 Config / Workflow / BIN / OTA 等文件
//
// 返回：
//   CRC32 值
//
// 错误：
//   文件不存在、无法打开或读取失败时返回 0。
//   注意：CRC32 本身可能合法地等于 0，
//   因此调用方如需严格区分"错误"与"CRC=0"，
//   应先调用 json_storage_exists() / json_storage_size()
//   或使用文件读取接口确认文件有效。

uint32_t json_storage_crc32(
    const char *path
);


// =====================================================
// 一次性读写
// =====================================================

// 一次性读取整个文件到 String
//
// 适用：
//   小型 JSON（config / workflow 等）
//
// 限制：
//   文件超过 32 KB 时本函数直接返回 false，不会把大文件灌进 RAM。
//   大文件请改用 json_storage_read_chunk() 分块处理。
//
// 返回：
//   true  = 读取成功，output 为文件完整内容
//   false = 文件不存在 / 打开失败 / 文件过大 / 内存不足 / 长度不符

bool json_storage_read(
    const char *path,
    String &output
);

// 一次性写入（截断覆盖）
//
// 警告：
//   这是直接覆盖写入，写入过程中断电会导致文件损坏。
//   需要断电安全请用 json_storage_write_atomic()。

bool json_storage_write(
    const char *path,
    const String &data
);

// 原子写入
//
// 流程：
//   data → path.tmp → 完整落盘并校验 → rename → 正式文件
//
// 保证：
//   正式文件在任何时刻要么是完整的旧内容，要么是完整的新内容，
//   不会出现"写了一半"的损坏状态。
//
// 不做：
//   不负责生成 backup。哪个是 current、哪个是 backup、
//   何时轮换、版本如何更新，全部由上层 ConfigManager 决定。
//
// 适用体积：
//   与 json_storage_read() 同量级（32 KB 以内的小型 JSON）。
//   更大的文件（BIN / OTA）请用流式接口写入临时文件，
//   再用 json_storage_rename() 自行完成原子替换。
//
// 失败时：
//   会尽力清理残留的临时文件，并返回 false。
//   正式文件保持替换前的完整内容，不会被破坏。

bool json_storage_write_atomic(
    const char *path,
    const String &data
);


// =====================================================
// 分块读写
// =====================================================

// 分块大小完全由调用方决定，本模块不写死任何数值：
//   Config Manager 可用 512B，Cloud Transfer 可用 256B，
//   OTA 可用 4096B。
// Flash IO chunk 与 MQTT packet chunk 属于不同层，互不影响。

// 分块读取
//
// 行为：
//   从 offset 处读取最多 buffer_size 字节到调用方提供的 buffer。
//
// 返回：
//   true  = 调用成功，bytes_read 为实际读取字节数
//           bytes_read == 0 表示已到文件末尾（EOF）
//   false = 文件不存在 / 打开失败 / 定位失败 / 参数非法

bool json_storage_read_chunk(
    const char *path,
    size_t offset,
    uint8_t *buffer,
    size_t buffer_size,
    size_t &bytes_read
);

// 分块写入
//
// 语义：
//   定位到 offset 后写入 data。
//
//   offset == 0 且文件不存在 → 新建文件
//   offset == 0 且文件存在   → 截断后从头写入
//   offset  > 0              → 要求 offset <= 当前文件大小
//
// 重要：
//   offset 超过当前文件大小时返回 false，而不是填充空洞。
//   这样上层在分块接收（BEGIN / CHUNK / END）时能立刻察觉
//   丢块或乱序，而不是得到一个中间填 0 的损坏文件。

bool json_storage_write_chunk(
    const char *path,
    size_t offset,
    const uint8_t *data,
    size_t length
);


// =====================================================
// 流式接口
// =====================================================
//
// 对 File 的薄封装，供大文件顺序处理使用
// （大 JSON / BIN / OTA / Log / Cloud Transfer）。
//
// 使用方负责配对 open 与 close，不得长期持有句柄。

struct JsonStorageFile
{
    File file;
};

// 以只读方式打开
bool json_storage_open_read(
    const char *path,
    JsonStorageFile &file
);

// 以截断写方式打开（文件存在则清空）
bool json_storage_open_write(
    const char *path,
    JsonStorageFile &file
);

// 以追加写方式打开（用于分块接收后顺序落盘）
bool json_storage_open_append(
    const char *path,
    JsonStorageFile &file
);

// 从已打开的文件读取，返回实际读取字节数，0 表示 EOF
size_t json_storage_read(
    JsonStorageFile &file,
    uint8_t *buffer,
    size_t buffer_size
);

// 向已打开的文件写入，返回实际写入字节数
size_t json_storage_write(
    JsonStorageFile &file,
    const uint8_t *data,
    size_t length
);

// 关闭文件（会先 flush）。重复关闭是安全的。
//
// 限制：
//   ESP32 的 File::flush() / close() 均无返回值，
//   本接口无法感知底层落盘失败（分区写满、flash 写保护等）。
//   需要可靠确认的场景，请改用 json_storage_write() 并校验返回字节数。

bool json_storage_close(
    JsonStorageFile &file
);