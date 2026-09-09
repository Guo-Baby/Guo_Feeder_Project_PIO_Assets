#include "bin_storage.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

// =====================================================
// 内部常量
// =====================================================

// 原子写使用的临时文件后缀
#define BIN_TMP_SUFFIX      ".tmp"

// 临时路径缓冲区大小（栈上定长，避免堆分配）
#define BIN_PATH_MAX        256

// 单条日志上限，超长截断，避免栈溢出
#define BIN_LOG_BUF_SIZE    192

// =====================================================
// 内部状态
// =====================================================

static bool s_ready = false;
static BinStorageLogCallback s_log_cb = nullptr;

// =====================================================
// 内部工具
// =====================================================

// 统一日志出口。未注册回调时静默，不产生任何串口输出。
static void bin_log(
    const char *level,
    const char *fmt,
    ...
)
{
    if (s_log_cb == nullptr)
    {
        return;
    }

    char buf[BIN_LOG_BUF_SIZE];

    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    s_log_cb(level, buf);
}

// 路径合法性：非空且非 nullptr
static bool bin_path_valid(
    const char *path
)
{
    return (path != nullptr && path[0] != '\0');
}

// 合成 "<path>.tmp"
// 使用定长缓冲区而非 String，避免底层存储模块引入堆分配。
static bool bin_make_tmp_path(
    const char *path,
    char *out,
    size_t out_size
)
{
    if (!bin_path_valid(path) || out == nullptr || out_size == 0)
    {
        return false;
    }

    size_t len = strlen(path);
    size_t suffix_len = sizeof(BIN_TMP_SUFFIX) - 1;

    // +1 留给字符串结束符
    if (len + suffix_len + 1 > out_size)
    {
        return false;
    }

    memcpy(out, path, len);
    memcpy(out + len, BIN_TMP_SUFFIX, suffix_len + 1);

    return true;
}

// =====================================================
// 结果转字符串
// =====================================================

const char *bin_storage_result_name(
    BinStorageResult result
)
{
    switch (result)
    {
    case BIN_STORAGE_OK:
        return "OK";
    case BIN_STORAGE_ERR_NOT_INITIALIZED:
        return "NOT_INITIALIZED";
    case BIN_STORAGE_ERR_INVALID_ARGUMENT:
        return "INVALID_ARGUMENT";
    case BIN_STORAGE_ERR_NOT_FOUND:
        return "NOT_FOUND";
    case BIN_STORAGE_ERR_BUFFER_TOO_SMALL:
        return "BUFFER_TOO_SMALL";
    case BIN_STORAGE_ERR_OPEN_FAILED:
        return "OPEN_FAILED";
    case BIN_STORAGE_ERR_READ_FAILED:
        return "READ_FAILED";
    case BIN_STORAGE_ERR_WRITE_FAILED:
        return "WRITE_FAILED";
    case BIN_STORAGE_ERR_DELETE_FAILED:
        return "DELETE_FAILED";
    case BIN_STORAGE_ERR_RENAME_FAILED:
        return "RENAME_FAILED";
    default:
        return "UNKNOWN";
    }
}

// =====================================================
// 日志回调
// =====================================================

void bin_storage_set_log_callback(
    BinStorageLogCallback callback
)
{
    s_log_cb = callback;
}

// =====================================================
// 初始化
// =====================================================

bool bin_storage_init()
{
    // 幂等：已初始化直接返回
    if (s_ready)
    {
        return true;
    }

    // 级联初始化底层文件层。
    // FileStorage 同样不负责 LittleFS.begin()，
    // 挂载生命周期由系统 setup() 统一管理。
    if (!file_storage_init())
    {
        s_ready = false;
        bin_log("E", "init: FileStorage unavailable");
        return false;
    }

    s_ready = true;

    bin_log("I", "ready");

    return true;
}

// =====================================================
// 文件基础信息
// =====================================================

bool bin_storage_exists(
    const char *path
)
{
    if (!s_ready)
    {
        return false;
    }

    if (!bin_path_valid(path))
    {
        return false;
    }

    return file_storage_exists(path);
}

size_t bin_storage_size(
    const char *path
)
{
    if (!s_ready)
    {
        return 0;
    }

    if (!bin_path_valid(path))
    {
        return 0;
    }

    return file_storage_size(path);
}

BinStorageResult bin_storage_mkdir(
    const char *path
)
{
    if (!s_ready)
    {
        return BIN_STORAGE_ERR_NOT_INITIALIZED;
    }

    if (!bin_path_valid(path))
    {
        return BIN_STORAGE_ERR_INVALID_ARGUMENT;
    }

    if (!file_storage_mkdir(path))
    {
        bin_log("E", "mkdir failed: %s", path);
        return BIN_STORAGE_ERR_WRITE_FAILED;
    }

    return BIN_STORAGE_OK;
}

// =====================================================
// BIN 读取
// =====================================================

BinStorageResult bin_storage_read(
    const char *path,
    uint8_t *buffer,
    size_t buffer_size,
    size_t &bytes_read
)
{
    bytes_read = 0;

    if (!s_ready)
    {
        return BIN_STORAGE_ERR_NOT_INITIALIZED;
    }

    if (!bin_path_valid(path))
    {
        return BIN_STORAGE_ERR_INVALID_ARGUMENT;
    }

    if (!file_storage_exists(path))
    {
        bin_log("W", "read: not found: %s", path);
        return BIN_STORAGE_ERR_NOT_FOUND;
    }

    size_t file_size = file_storage_size(path);

    // buffer 不足：直接拒绝，一个字节都不读。
    // 绝不截断填充，避免上层拿到半截数据还以为成功。
    if (file_size > buffer_size)
    {
        bin_log(
            "E",
            "read: buffer too small (need %u, have %u): %s",
            (unsigned int)file_size,
            (unsigned int)buffer_size,
            path
        );

        return BIN_STORAGE_ERR_BUFFER_TOO_SMALL;
    }

    // 空文件：直接成功，bytes_read == 0
    if (file_size == 0)
    {
        return BIN_STORAGE_OK;
    }

    FileStorageFile file;

    if (!file_storage_open_read(path, file))
    {
        return BIN_STORAGE_ERR_OPEN_FAILED;
    }

    size_t total = 0;
    BinStorageResult result = BIN_STORAGE_OK;

    while (total < file_size)
    {
        size_t n = file_storage_read(
            file,
            buffer + total,
            file_size - total
        );

        // 读取返回 0 表示 EOF 或底层失败，
        // 无论哪种，都没有拿到预期长度的数据。
        if (n == 0)
        {
            bin_log(
                "E",
                "read: got %u/%u bytes: %s",
                (unsigned int)total,
                (unsigned int)file_size,
                path
            );

            result = BIN_STORAGE_ERR_READ_FAILED;
            break;
        }

        total += n;
    }

    file_storage_close(file);

    if (result != BIN_STORAGE_OK)
    {
        bytes_read = 0;
        return result;
    }

    bytes_read = total;

    return BIN_STORAGE_OK;
}

BinStorageResult bin_storage_read_chunk(
    const char *path,
    size_t offset,
    uint8_t *buffer,
    size_t buffer_size,
    size_t &bytes_read
)
{
    bytes_read = 0;

    if (!s_ready)
    {
        return BIN_STORAGE_ERR_NOT_INITIALIZED;
    }

    if (!bin_path_valid(path))
    {
        return BIN_STORAGE_ERR_INVALID_ARGUMENT;
    }

    if (buffer == nullptr || buffer_size == 0)
    {
        return BIN_STORAGE_ERR_INVALID_ARGUMENT;
    }

    if (!file_storage_exists(path))
    {
        bin_log("W", "read_chunk: not found: %s", path);
        return BIN_STORAGE_ERR_NOT_FOUND;
    }

    size_t file_size = file_storage_size(path);

    if (offset > file_size)
    {
        bin_log(
            "E",
            "read_chunk: offset %u beyond size %u: %s",
            (unsigned int)offset,
            (unsigned int)file_size,
            path
        );

        return BIN_STORAGE_ERR_INVALID_ARGUMENT;
    }

    if (!file_storage_read(
            path,
            offset,
            buffer,
            buffer_size,
            bytes_read
        ))
    {
        return BIN_STORAGE_ERR_READ_FAILED;
    }

    return BIN_STORAGE_OK;
}

// =====================================================
// BIN 写入
// =====================================================

BinStorageResult bin_storage_write(
    const char *path,
    const uint8_t *data,
    size_t length
)
{
    if (!s_ready)
    {
        return BIN_STORAGE_ERR_NOT_INITIALIZED;
    }

    if (!bin_path_valid(path))
    {
        return BIN_STORAGE_ERR_INVALID_ARGUMENT;
    }

    if (data == nullptr && length > 0)
    {
        return BIN_STORAGE_ERR_INVALID_ARGUMENT;
    }

    // 空写入：以 "w" 打开再关闭，等价于把文件截断为 0 字节。
    // 注意不能直接走 file_storage_write(path, 0, data, 0)，
    // 那条路径会因为 length == 0 而短路，不会截断已有文件。
    if (length == 0)
    {
        FileStorageFile empty;

        if (!file_storage_open_write(path, empty))
        {
            bin_log("W", "write: open failed (truncate): %s", path);
            return BIN_STORAGE_ERR_OPEN_FAILED;
        }

        file_storage_close(empty);

        return BIN_STORAGE_OK;
    }

    FileStorageFile file;

    // "w" 模式：新建或截断已有文件，保证不会残留旧字节
    if (!file_storage_open_write(path, file))
    {
        bin_log("W", "write: open failed: %s", path);
        return BIN_STORAGE_ERR_OPEN_FAILED;
    }

    size_t written = file_storage_write(file, data, length);

    file_storage_close(file);

    // 打开成功不等于写入成功，必须校验实际字节数
    if (written != length)
    {
        bin_log(
            "E",
            "write: %u/%u bytes: %s",
            (unsigned int)written,
            (unsigned int)length,
            path
        );

        return BIN_STORAGE_ERR_WRITE_FAILED;
    }

    // 落盘确认：重新读取大小，确认与预期一致
    if (file_storage_size(path) != length)
    {
        bin_log("E", "write: size verify failed: %s", path);
        return BIN_STORAGE_ERR_WRITE_FAILED;
    }

    return BIN_STORAGE_OK;
}

BinStorageResult bin_storage_write_chunk(
    const char *path,
    size_t offset,
    const uint8_t *data,
    size_t length
)
{
    if (!s_ready)
    {
        return BIN_STORAGE_ERR_NOT_INITIALIZED;
    }

    if (!bin_path_valid(path))
    {
        return BIN_STORAGE_ERR_INVALID_ARGUMENT;
    }

    if (data == nullptr && length > 0)
    {
        return BIN_STORAGE_ERR_INVALID_ARGUMENT;
    }

    // 空写入无副作用，直接视为成功
    if (length == 0)
    {
        return BIN_STORAGE_OK;
    }

    if (offset > 0)
    {
        // 续写要求文件已存在，且 offset 不得超过当前大小
        if (!file_storage_exists(path))
        {
            bin_log(
                "E",
                "write_chunk: file missing, cannot resume at %u: %s",
                (unsigned int)offset,
                path
            );

            return BIN_STORAGE_ERR_NOT_FOUND;
        }

        if (offset > file_storage_size(path))
        {
            bin_log(
                "E",
                "write_chunk: offset %u beyond size: %s",
                (unsigned int)offset,
                path
            );

            return BIN_STORAGE_ERR_INVALID_ARGUMENT;
        }
    }

    if (!file_storage_write(path, offset, data, length))
    {
        return BIN_STORAGE_ERR_WRITE_FAILED;
    }

    return BIN_STORAGE_OK;
}

BinStorageResult bin_storage_write_atomic(
    const char *path,
    const uint8_t *data,
    size_t length
)
{
    if (!s_ready)
    {
        return BIN_STORAGE_ERR_NOT_INITIALIZED;
    }

    if (!bin_path_valid(path))
    {
        return BIN_STORAGE_ERR_INVALID_ARGUMENT;
    }

    if (data == nullptr && length > 0)
    {
        return BIN_STORAGE_ERR_INVALID_ARGUMENT;
    }

    char tmp[BIN_PATH_MAX];

    if (!bin_make_tmp_path(path, tmp, sizeof(tmp)))
    {
        bin_log("E", "atomic: tmp path too long: %s", path);
        return BIN_STORAGE_ERR_INVALID_ARGUMENT;
    }

    // 1) 清理可能残留的上次临时文件
    file_storage_remove(tmp);

    // 2) 完整写入临时文件
    BinStorageResult result = bin_storage_write(tmp, data, length);

    if (result != BIN_STORAGE_OK)
    {
        file_storage_remove(tmp);

        bin_log("E", "atomic: tmp write failed: %s", tmp);

        return result;
    }

    // 3) 落盘校验：大小一致才允许替换正式文件
    if (file_storage_size(tmp) != length)
    {
        file_storage_remove(tmp);

        bin_log("E", "atomic: tmp size mismatch: %s", tmp);

        return BIN_STORAGE_ERR_WRITE_FAILED;
    }

    // 4) 原子替换
    //
    // 依赖 LittleFS rename 的原子替换语义。
    // backup / recovery / 版本轮换由上层决定，本模块不参与。
    if (!file_storage_rename(tmp, path))
    {
        file_storage_remove(tmp);

        bin_log("E", "atomic: rename failed: %s -> %s", tmp, path);

        return BIN_STORAGE_ERR_RENAME_FAILED;
    }

    // 5) 最终确认
    //
    // ESP32 Arduino 的 File::flush() 返回 void，无法直接确认落盘。
    // 这里改用"重新读取正式文件大小"做最终校验：
    // 若大小与预期不一致，说明落盘异常，必须让调用方知道本次写入不可信。
    if (file_storage_size(path) != length)
    {
        bin_log("E", "atomic: final verify failed: %s", path);

        return BIN_STORAGE_ERR_WRITE_FAILED;
    }

    return BIN_STORAGE_OK;
}

// =====================================================
// BIN 删除
// =====================================================

BinStorageResult bin_storage_remove(
    const char *path
)
{
    if (!s_ready)
    {
        return BIN_STORAGE_ERR_NOT_INITIALIZED;
    }

    if (!bin_path_valid(path))
    {
        return BIN_STORAGE_ERR_INVALID_ARGUMENT;
    }

    // 幂等：文件本就不存在视为目标已达成。
    // 业务层若需要区分"真的删掉了"与"本来就没有"，
    // 请自行先调用 bin_storage_exists()。
    if (!file_storage_exists(path))
    {
        return BIN_STORAGE_OK;
    }

    if (!file_storage_remove(path))
    {
        bin_log("E", "remove failed: %s", path);
        return BIN_STORAGE_ERR_DELETE_FAILED;
    }

    return BIN_STORAGE_OK;
}

// =====================================================
// BIN Rename
// =====================================================

BinStorageResult bin_storage_rename(
    const char *from,
    const char *to
)
{
    if (!s_ready)
    {
        return BIN_STORAGE_ERR_NOT_INITIALIZED;
    }

    if (!bin_path_valid(from) || !bin_path_valid(to))
    {
        return BIN_STORAGE_ERR_INVALID_ARGUMENT;
    }

    if (!file_storage_exists(from))
    {
        bin_log("E", "rename: source missing: %s", from);
        return BIN_STORAGE_ERR_NOT_FOUND;
    }

    if (!file_storage_rename(from, to))
    {
        return BIN_STORAGE_ERR_RENAME_FAILED;
    }

    return BIN_STORAGE_OK;
}

// =====================================================
// 原始 CRC32（工具）
// =====================================================

BinStorageResult bin_storage_crc32(
    const char *path,
    uint32_t &crc_out
)
{
    crc_out = 0;

    if (!s_ready)
    {
        return BIN_STORAGE_ERR_NOT_INITIALIZED;
    }

    if (!bin_path_valid(path))
    {
        return BIN_STORAGE_ERR_INVALID_ARGUMENT;
    }

    if (!file_storage_exists(path))
    {
        bin_log("W", "crc32: not found: %s", path);
        return BIN_STORAGE_ERR_NOT_FOUND;
    }

    uint32_t crc = 0;

    if (!file_storage_crc32(path, crc))
    {
        return BIN_STORAGE_ERR_READ_FAILED;
    }

    crc_out = crc;

    return BIN_STORAGE_OK;
}
