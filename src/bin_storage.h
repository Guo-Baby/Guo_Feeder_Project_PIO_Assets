#pragma once

#include <Arduino.h>
#include "file_storage.h"

// =====================================================
// BIN Storage — 通用二进制文件存储模块
// =====================================================
//
// 职责：
//   把任意二进制文件（BIN）可靠地读出来、写进去、替换掉、删掉。
//
// 只认识三样东西：
//   path + raw bytes + length
//
// 本模块不知道 BIN 里装的是什么，可能是：
//   Workflow Step / Calibration / OTA / 设备档案 / 任何其他原始数据
//
// 不负责（也永远不会负责）：
//   Workflow 最大数量、Step 最大数量
//   W01S00.bin 之类的文件命名规则
//   WorkflowMeta、Dirty Bitmap、Critical、version
//   Action、Trigger、StepDefinition、WorkflowRuntime
//   Step 0 必须是 Trigger、step_count、workflow valid 等规则
//   StepDefinition 的序列化 / 反序列化
//   CRC 业务规则（本模块只提供原始 CRC 计算工具，不解释结果）
//
// 依赖方向（严格单向，禁止反向依赖）：
//   WorkflowManager → BinStorage → FileStorage → LittleFS
//
// 与 JsonStorage 的关系：
//   两者平级，分别服务 BIN 与 JSON 两条存储链路，互不包含、互不调用。
//   本模块不依赖 ArduinoJson，不使用 String 承载数据。
//
// 内存约定：
//   - 数据一律 uint8_t* + length
//   - 允许出现 0x00，绝不使用 strlen() 等字符串逻辑
//   - 不在模块内部复制业务 buffer
//   - 不长期持有任何句柄或 buffer
// =====================================================


// =====================================================
// 操作结果
// =====================================================
//
// 只区分"存储层能感知的事实"，不引入业务含义。
// 上层（WorkflowManager）自行决定遇到某个结果该如何处理。

enum BinStorageResult
{
    BIN_STORAGE_OK = 0,

    BIN_STORAGE_ERR_NOT_INITIALIZED,   // 文件系统不可用 / 未初始化
    BIN_STORAGE_ERR_INVALID_ARGUMENT,  // path 为空、长度与指针矛盾等
    BIN_STORAGE_ERR_NOT_FOUND,         // 文件不存在
    BIN_STORAGE_ERR_BUFFER_TOO_SMALL,  // 调用方 buffer 小于文件实际大小
    BIN_STORAGE_ERR_OPEN_FAILED,       // 文件打开失败
    BIN_STORAGE_ERR_READ_FAILED,       // 读取过程中失败 / 长度不足
    BIN_STORAGE_ERR_WRITE_FAILED,      // 写入字节数不符或落盘校验失败
    BIN_STORAGE_ERR_DELETE_FAILED,     // 删除失败
    BIN_STORAGE_ERR_RENAME_FAILED      // 重命名 / 替换失败
};

// 结果转可读字符串，便于日志输出
//
// 返回静态字符串，调用方无需释放。

const char *bin_storage_result_name(
    BinStorageResult result
);


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

typedef void (*BinStorageLogCallback)(
    const char *level,
    const char *message
);

// 设置日志回调。传 nullptr 可关闭日志输出。
void bin_storage_set_log_callback(
    BinStorageLogCallback callback
);


// =====================================================
// 初始化
// =====================================================
//
// 内部会级联初始化 FileStorage。
//
// 重复调用是幂等的：已初始化则直接返回 true。
//
// 返回：
//   true  = 文件系统可用，后续 BIN 接口可用
//   false = 文件系统不可用，后续所有 BIN 接口均返回
//           BIN_STORAGE_ERR_NOT_INITIALIZED

bool bin_storage_init();


// =====================================================
// 文件基础信息
// =====================================================

// 文件是否存在
//
// 返回：
//   true  = 文件存在
//   false = 文件不存在 / 未初始化 / 参数非法

bool bin_storage_exists(
    const char *path
);

// 获取文件大小（字节）
//
// 返回：
//   文件大小；文件不存在或无法获取时返回 0。
//
// 用途：
//   读取前先取大小，用于分配 buffer 或判断是否分块读取。
//   注意 0 既可能表示"空文件"，也可能表示"文件不存在"，
//   需要严格区分时请先调用 bin_storage_exists()。

size_t bin_storage_size(
    const char *path
);

// 创建目录
//
// LittleFS 不会在写文件时自动创建父目录。
// 写入"非随固件烧录"的目录前必须先调用本函数。
// 幂等：目录已存在时返回 BIN_STORAGE_OK。

BinStorageResult bin_storage_mkdir(
    const char *path
);


// =====================================================
// BIN 读取
// =====================================================

// 一次性读取整个文件
//
// 要求调用方先准备足够大的 buffer：
//   典型用法: size = bin_storage_size(path); 再按 size 准备 buffer。
//
// buffer 不足时：
//   直接返回 BIN_STORAGE_ERR_BUFFER_TOO_SMALL，并且【不读取任何数据】。
//   绝不截断填充，避免上层拿到半截数据还以为成功。
//
// 返回：
//   BIN_STORAGE_OK = 读取成功，bytes_read 为实际读取字节数
//                    空文件返回 OK 且 bytes_read == 0
//   其他值 = 见 BinStorageResult

BinStorageResult bin_storage_read(
    const char *path,
    uint8_t *buffer,
    size_t buffer_size,
    size_t &bytes_read
);

// 分块读取
//
// 从 offset 处读取最多 buffer_size 字节，用于大文件（BIN / OTA / Log）。
//
// 行为：
//   offset == 文件大小 → EOF，返回 OK 且 bytes_read == 0
//   offset  >  文件大小 → 返回 BIN_STORAGE_ERR_INVALID_ARGUMENT
//
// 返回：
//   BIN_STORAGE_OK = 调用成功，bytes_read 为实际读取字节数

BinStorageResult bin_storage_read_chunk(
    const char *path,
    size_t offset,
    uint8_t *buffer,
    size_t buffer_size,
    size_t &bytes_read
);


// =====================================================
// BIN 写入
// =====================================================

// 直接写入（截断覆盖）
//
// 警告：
//   这是直接覆盖写入，写入过程中断电会导致文件损坏。
//   需要断电安全请用 bin_storage_write_atomic()。
//
// 语义：
//   offset 概念不适用于本函数 —— 无论原文件多大，一律截断为 length 字节。
//   length == 0 时创建 / 清空为空文件。
//
// 数据要求：
//   data 允许包含任意字节（含 0x00），按 length 精确写入。
//   data == nullptr 且 length > 0 视为参数非法。

BinStorageResult bin_storage_write(
    const char *path,
    const uint8_t *data,
    size_t length
);

// 分块写入
//
// 语义：
//   offset == 0 → 新建或截断后从头写入
//   offset  > 0 → 要求 offset <= 当前文件大小
//
// 用于分块接收（BEGIN / CHUNK / END）或分步构造大 BIN。
// offset 超过当前文件大小时返回 BIN_STORAGE_ERR_INVALID_ARGUMENT，
// 而不是填充空洞，便于上层立刻察觉丢块或乱序。

BinStorageResult bin_storage_write_chunk(
    const char *path,
    size_t offset,
    const uint8_t *data,
    size_t length
);

// 原子写入（安全替换）
//
// 流程：
//   data → path.tmp → 完整落盘并校验大小 → rename → 正式文件
//
// 保证：
//   正式文件在任何时刻要么是完整的旧内容，要么是完整的新内容，
//   不会出现"写了一半"的损坏状态。
//
// 不做：
//   不生成 backup、不管理版本、不理解"为什么替换"。
//   backup / recovery / 版本轮换全部由上层决定。
//
// 失败时：
//   会尽力清理残留的临时文件。
//   正式文件保持替换前的完整内容，不会被破坏。

BinStorageResult bin_storage_write_atomic(
    const char *path,
    const uint8_t *data,
    size_t length
);


// =====================================================
// BIN 删除
// =====================================================

// 删除文件
//
// 说明：
//   幂等 —— 文件本就不存在时同样返回 BIN_STORAGE_OK
//  （"确保文件不存在"这一目标已达成）。
//
//   本模块不理解任何业务删除规则。
//   例如"删除 Workflow 4 时不能删除 Step BIN"属于上层业务，
//   由 WorkflowManager 自行判断，不进入本模块。

BinStorageResult bin_storage_remove(
    const char *path
);


// =====================================================
// BIN Rename
// =====================================================

// 重命名 / 移动文件
//
// 用途：
//   安全替换、临时文件、事务机制、文件迁移。
//
// 说明：
//   目标文件已存在时由文件系统覆盖（LittleFS rename 语义）。
//   本函数不理解"为什么 rename"。
//
// 返回：
//   BIN_STORAGE_OK               = 成功
//   BIN_STORAGE_ERR_NOT_FOUND    = 源文件不存在
//   BIN_STORAGE_ERR_RENAME_FAILED = 重命名失败

BinStorageResult bin_storage_rename(
    const char *from,
    const char *to
);


// =====================================================
// 原始 CRC32（工具）
// =====================================================
//
// 流式计算整个文件的 CRC32，不把文件加载进 RAM。
//
// 本函数只负责"算出一个数值"，
// 不解释这个数值意味着什么 —— CRC 业务规则属于上层。
//
// 返回：
//   BIN_STORAGE_OK = 计算成功，crc_out 为文件 CRC32
//   其他值 = 失败，crc_out 置 0
//
// 注意：
//   CRC32 本身可能合法地等于 0，
//   因此需要严格区分"错误"与"CRC=0"时请以返回值为准。

BinStorageResult bin_storage_crc32(
    const char *path,
    uint32_t &crc_out
);
