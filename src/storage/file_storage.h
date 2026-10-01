#pragma once

#include <Arduino.h>
#include <FS.h>

// =====================================================
// File Storage — 最底层通用文件操作层（LittleFS）
// =====================================================
//
// 职责：
//   把原始字节可靠地读出来、写进去。
//
// 只认识三样东西：
//   path + raw bytes + length
//
// 不负责（也永远不会负责）：
//   JSON 解析 / 序列化
//   Workflow、StepDefinition、WorkflowMeta
//   Dirty Bitmap、Critical、version、backup / recovery
//   Action、Trigger
//   CRC 业务规则（本模块只提供原始 CRC 计算工具，不解释结果）
//
// 依赖方向（严格单向，禁止反向依赖）：
//   BinStorage → FileStorage → LittleFS
//
// 与 JsonStorage 的关系：
//   两者平级，各自独立封装 LittleFS，互不包含、互不调用。
//   JsonStorage 继续服务现有 JSON 体系（本次不重构、不修改）。
//   FileStorage 服务新的 BIN / 原始字节体系。
//   不为"接口统一"去改动 JsonStorage。
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
// 本模块不直接使用 Serial.println，避免底层散落串口输出。
// 未来 Log Manager 完成后，由上层注册回调统一接管。
// 不注册回调时，模块静默运行，不影响任何功能。

typedef void (*FileStorageLogCallback)(
    const char *level,
    const char *message
);

// 设置日志回调。传 nullptr 可关闭日志输出。
void file_storage_set_log_callback(
    FileStorageLogCallback callback
);


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
//   FileStorage 不调用 LittleFS.begin()。
//   LittleFS 的生命周期完全由系统初始化阶段统一管理。
//
//   重复调用是幂等的：已初始化则直接返回 true。
//
// 返回：
//   true  = 文件系统可用
//   false = 文件系统不可用，后续所有读写接口均会失败

bool file_storage_init();


// =====================================================
// 文件基础操作
// =====================================================

// 文件是否存在
bool file_storage_exists(
    const char *path
);

// 获取文件大小（字节）
//
// 返回：
//   文件大小；文件不存在或无法获取时返回 0
//
// 用途：
//   由调用方决定一次性读取还是分块读取，以及内存预算。
//   注意 0 既可能表示"空文件"，也可能表示"文件不存在"，
//   需要严格区分时请先调用 file_storage_exists()。

size_t file_storage_size(
    const char *path
);

// 创建目录
//
// LittleFS 不会在写文件时自动创建父目录。
// 写入"非随固件烧录"的目录前必须先调用本函数。
//
// 幂等：目录已存在时直接返回 true，可安全重复调用。

bool file_storage_mkdir(
    const char *path
);

// 删除文件
//
// 说明：
//   幂等 —— 文件本就不存在时同样返回 true（"确保文件不存在"）。
//   本函数不理解任何业务删除规则。

bool file_storage_remove(
    const char *path
);

// 重命名 / 移动文件
//
// 说明：
//   目标文件已存在时由文件系统覆盖（LittleFS 的 rename 语义）。
//   这是上层实现原子替换的基础能力，
//   但本函数不理解"为什么 rename"。

bool file_storage_rename(
    const char *from,
    const char *to
);


// =====================================================
// 目录遍历（只读）
// =====================================================
//
// 用途：
//   供上层实现"恢复扫描 / 残留清理"（例如扫描未发布的 .t 临时文件）。
//   本函数不解释文件名含义，不理解任何业务规则。
//
// 语义：
//   - 目录不存在时返回 false（调用方自行决定是否视为正常）。
//   - 回调每收到一个条目返回 true 继续；返回 false 立即停止遍历。
//   - 回调接收的是【文件名副本】（不含路径），回调返回后即可安全
//     remove / rename 该条目 —— 本函数已先关闭条目句柄。
//   - 回调内不得再调用 file_storage_foreach()（本目录或他目录），
//     避免嵌套遍历造成句柄耗尽。
//
// 返回：
//   true  = 目录存在且已遍历（或回调主动停止）
//   false = 未初始化 / 参数非法 / 目录不存在 / 打开失败

typedef bool (*FileStorageListCallback)(
    const char *name,        // 条目名副本（basename，不含路径）
    bool is_dir,             // true = 目录条目
    void *user               // 透传用户上下文
);

bool file_storage_foreach(
    const char *dir,
    FileStorageListCallback callback,
    void *user
);


// =====================================================
// 原始字节读写（offset 定位）
// =====================================================
//
// 数据一律以 uint8_t* + length 表达：
//   - 允许出现 0x00
//   - 不使用 strlen() 等任何字符串逻辑
//   - 不把数据转换成 String
//   - 不在模块内部复制 buffer

// 从 offset 处读取最多 buffer_size 字节
//
// 行为：
//   offset == 文件大小 → EOF，返回 true 且 bytes_read == 0
//   offset  >  文件大小 → 调用错误，返回 false
//
// 返回：
//   true  = 调用成功，bytes_read 为实际读取字节数
//   false = 未初始化 / 参数非法 / 打开失败 / 定位失败 / 读取失败

bool file_storage_read(
    const char *path,
    size_t offset,
    uint8_t *buffer,
    size_t buffer_size,
    size_t &bytes_read
);

// 从 offset 处写入 data
//
// 语义：
//   offset == 0 且文件不存在 → 新建文件
//   offset == 0 且文件存在   → 截断后从头写入
//   offset  > 0              → 要求 offset <= 当前文件大小
//
// 重要：
//   offset 超过当前文件大小时返回 false，而不是填充空洞。
//   这样上层在分块接收时能立刻察觉丢块或乱序，
//   而不是得到一个中间填 0 的损坏文件。

bool file_storage_write(
    const char *path,
    size_t offset,
    const uint8_t *data,
    size_t length
);


// =====================================================
// 原始 CRC32（工具）
// =====================================================
//
// 流式计算整个文件的 CRC32：
//   - 不把整个文件加载到 RAM
//   - 内部使用固定大小栈缓冲
//
// 本函数只负责"算出一个数值"，
// 不解释这个数值意味着什么 —— CRC 业务规则属于上层。
//
// 返回：
//   true  = 计算成功，crc_out 为文件 CRC32
//   false = 未初始化 / 参数非法 / 打开失败 / 读取失败（crc_out 置 0）

bool file_storage_crc32(
    const char *path,
    uint32_t &crc_out
);


// =====================================================
// 流式接口
// =====================================================
//
// 对 File 的薄封装，供大文件顺序处理使用（BIN / OTA / Log）。
// 使用方负责配对 open 与 close，不得长期持有句柄。

struct FileStorageFile
{
    File file;
};

// 以只读方式打开
bool file_storage_open_read(
    const char *path,
    FileStorageFile &file
);

// 以截断写方式打开（文件存在则清空）
bool file_storage_open_write(
    const char *path,
    FileStorageFile &file
);

// 以追加写方式打开（用于分块接收后顺序落盘）
bool file_storage_open_append(
    const char *path,
    FileStorageFile &file
);

// 从已打开的文件读取，返回实际读取字节数，0 表示 EOF 或失败
size_t file_storage_read(
    FileStorageFile &file,
    uint8_t *buffer,
    size_t buffer_size
);

// 向已打开的文件写入，返回实际写入字节数
size_t file_storage_write(
    FileStorageFile &file,
    const uint8_t *data,
    size_t length
);

// 关闭文件（会先 flush）。重复关闭是安全的。
//
// 限制：
//   ESP32 的 File::flush() / close() 均无返回值，
//   本接口无法感知底层落盘失败（分区写满、flash 写保护等）。
//   需要可靠确认的场景，请改用 file_storage_write()
//   并自行校验返回字节数与文件大小。

bool file_storage_close(
    FileStorageFile &file
);
