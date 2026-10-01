#include "storage/file_storage.h"

#include <LittleFS.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

// =====================================================
// 内部常量
// =====================================================

// LittleFS 挂载参数仅作为文件系统识别信息保留。
// FileStorage 不负责挂载 LittleFS。
#define FS_BASE_PATH        "/littlefs"
#define FS_MAX_OPEN_FILES   10
#define FS_PARTITION_LABEL  "littlefs"

// 内部读取块大小：栈上缓冲，避免堆分配与内存碎片
#define FS_BLOCK_SIZE       512

// 目录遍历条目名上限（超过截断）
#define FS_LIST_NAME_MAX    64

// 单条日志上限，超长截断，避免栈溢出
#define FS_LOG_BUF_SIZE     192

// =====================================================
// 内部状态
// =====================================================

static bool s_ready = false;
static FileStorageLogCallback s_log_cb = nullptr;

// =====================================================
// 内部工具
// =====================================================

// 统一日志出口。未注册回调时静默，不产生任何串口输出。
static void fs_log(
    const char *level,
    const char *fmt,
    ...
)
{
    if (s_log_cb == nullptr)
    {
        return;
    }

    char buf[FS_LOG_BUF_SIZE];

    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    s_log_cb(level, buf);
}

// 路径合法性：非空且非 nullptr
static bool fs_path_valid(
    const char *path
)
{
    return (path != nullptr && path[0] != '\0');
}

// CRC32 增量更新（标准 CRC-32 / IEEE 802.3，多项式 0xEDB88320）
static uint32_t fs_crc32_update(
    uint32_t crc,
    const uint8_t *data,
    size_t length
)
{
    for (size_t i = 0; i < length; ++i)
    {
        crc ^= data[i];

        for (uint8_t bit = 0; bit < 8; ++bit)
        {
            if (crc & 1U)
            {
                crc = (crc >> 1) ^ 0xEDB88320UL;
            }
            else
            {
                crc >>= 1;
            }
        }
    }

    return crc;
}

// =====================================================
// 日志回调
// =====================================================

void file_storage_set_log_callback(
    FileStorageLogCallback callback
)
{
    s_log_cb = callback;
}

// =====================================================
// 初始化
// =====================================================

bool file_storage_init()
{
    // 幂等：已初始化直接返回
    if (s_ready)
    {
        return true;
    }

    // =================================================
    // 重要：
    // FileStorage 不负责 LittleFS.begin()
    //
    // LittleFS 的挂载生命周期由系统 setup() 统一管理。
    // 这里仅确认文件系统已经可用。
    // =================================================

    if (LittleFS.totalBytes() == 0)
    {
        s_ready = false;
        fs_log("E", "LittleFS unavailable (not mounted)");
        return false;
    }

    s_ready = true;

    fs_log("I", "ready");

    return true;
}

// =====================================================
// 文件基础操作
// =====================================================

bool file_storage_exists(
    const char *path
)
{
    if (!s_ready)
    {
        fs_log("E", "exists: not initialized");
        return false;
    }

    if (!fs_path_valid(path))
    {
        return false;
    }

    return LittleFS.exists(path);
}

size_t file_storage_size(
    const char *path
)
{
    if (!s_ready)
    {
        fs_log("E", "size: not initialized");
        return 0;
    }

    if (!fs_path_valid(path))
    {
        return 0;
    }

    File file = LittleFS.open(path, "r");

    if (!file)
    {
        return 0;
    }

    size_t size = file.size();

    file.close();

    return size;
}

bool file_storage_mkdir(
    const char *path
)
{
    if (!s_ready)
    {
        fs_log("E", "mkdir: not initialized");
        return false;
    }

    if (!fs_path_valid(path))
    {
        return false;
    }

    // 幂等：目录已存在视为目标已达成
    if (LittleFS.exists(path))
    {
        return true;
    }

    if (!LittleFS.mkdir(path))
    {
        fs_log("E", "mkdir failed: %s", path);
        return false;
    }

    return true;
}

bool file_storage_remove(
    const char *path
)
{
    if (!s_ready)
    {
        fs_log("E", "remove: not initialized");
        return false;
    }

    if (!fs_path_valid(path))
    {
        return false;
    }

    // 幂等：文件本就不存在视为目标已达成
    if (!LittleFS.exists(path))
    {
        return true;
    }

    if (!LittleFS.remove(path))
    {
        fs_log("E", "remove failed: %s", path);
        return false;
    }

    return true;
}

bool file_storage_rename(
    const char *from,
    const char *to
)
{
    if (!s_ready)
    {
        fs_log("E", "rename: not initialized");
        return false;
    }

    if (!fs_path_valid(from) || !fs_path_valid(to))
    {
        return false;
    }

    if (!LittleFS.exists(from))
    {
        fs_log("E", "rename: source missing: %s", from);
        return false;
    }

    // 目标存在时由文件系统覆盖（LittleFS rename 语义）。
    // 本函数不理解"为什么 rename"，替换 / 轮换策略由上层决定。
    if (!LittleFS.rename(from, to))
    {
        fs_log("E", "rename failed: %s -> %s", from, to);
        return false;
    }

    return true;
}

bool file_storage_foreach(
    const char *dir,
    FileStorageListCallback callback,
    void *user
)
{
    if (!s_ready)
    {
        fs_log("E", "foreach: not initialized");
        return false;
    }

    if (dir == nullptr || callback == nullptr || !fs_path_valid(dir))
    {
        return false;
    }

    if (!LittleFS.exists(dir))
    {
        return false;
    }

    File root = LittleFS.open(dir);

    if (!root || !root.isDirectory())
    {
        if (root)
        {
            root.close();
        }
        fs_log("E", "foreach: not a directory: %s", dir);
        return false;
    }

    while (true)
    {
        File entry = root.openNextFile();

        if (!entry)
        {
            break;
        }

        // 先复制条目名并关闭句柄，再回调 ——
        // 回调内可安全 remove / rename 该条目。
        char name[FS_LIST_NAME_MAX];
        const char *n = entry.name();
        size_t len = (n != nullptr) ? strlen(n) : 0;

        if (len >= sizeof(name))
        {
            len = sizeof(name) - 1;
        }
        if (len > 0)
        {
            memcpy(name, n, len);
        }
        name[len] = '\0';

        bool is_dir = entry.isDirectory();
        entry.close();

        if (!callback(name, is_dir, user))
        {
            break;
        }
    }

    root.close();
    return true;
}

// =====================================================
// 原始字节读写（offset 定位）
// =====================================================

bool file_storage_read(
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
        fs_log("E", "read: not initialized");
        return false;
    }

    if (!fs_path_valid(path))
    {
        return false;
    }

    if (buffer == nullptr || buffer_size == 0)
    {
        return false;
    }

    File file = LittleFS.open(path, "r");

    if (!file)
    {
        fs_log("W", "read: open failed: %s", path);
        return false;
    }

    size_t total = file.size();

    // 恰好位于文件末尾：EOF
    if (offset == total)
    {
        file.close();
        return true;
    }

    // 越过文件末尾：调用错误
    if (offset > total)
    {
        fs_log(
            "E",
            "read: offset %u beyond size %u: %s",
            (unsigned int)offset,
            (unsigned int)total,
            path
        );

        file.close();

        return false;
    }

    if (!file.seek(offset))
    {
        fs_log(
            "E",
            "read: seek %u failed: %s",
            (unsigned int)offset,
            path
        );

        file.close();

        return false;
    }

    int n = file.read(buffer, buffer_size);

    file.close();

    if (n < 0)
    {
        fs_log("E", "read: read error: %s", path);
        return false;
    }

    bytes_read = (size_t)n;

    return true;
}

bool file_storage_write(
    const char *path,
    size_t offset,
    const uint8_t *data,
    size_t length
)
{
    if (!s_ready)
    {
        fs_log("E", "write: not initialized");
        return false;
    }

    if (!fs_path_valid(path))
    {
        return false;
    }

    if (data == nullptr && length > 0)
    {
        return false;
    }

    // 空写入无副作用，直接视为成功
    if (length == 0)
    {
        return true;
    }

    File file;

    if (offset == 0)
    {
        // 从头写入：以 "w" 打开，新建或截断已有文件。
        //
        // 必须截断。若沿用 "r+" 从头部覆写，当新数据短于旧文件时，
        // 尾部会残留旧字节，产出半新半旧的损坏文件。
        file = LittleFS.open(path, "w");
    }
    else
    {
        // 续写：要求文件已存在
        file = LittleFS.open(path, "r+");

        if (!file)
        {
            fs_log(
                "E",
                "write: file missing, cannot resume at %u: %s",
                (unsigned int)offset,
                path
            );

            return false;
        }

        size_t total = file.size();

        if (offset > total)
        {
            fs_log(
                "E",
                "write: offset %u beyond size %u: %s",
                (unsigned int)offset,
                (unsigned int)total,
                path
            );

            file.close();

            return false;
        }

        if (!file.seek(offset))
        {
            fs_log("E", "write: seek failed: %s", path);

            file.close();

            return false;
        }
    }

    if (!file)
    {
        fs_log("W", "write: open failed: %s", path);
        return false;
    }

    size_t written = file.write(data, length);

    file.flush();
    file.close();

    // 打开成功不等于写入成功，必须校验实际字节数
    if (written != length)
    {
        fs_log(
            "E",
            "write: %u/%u bytes: %s",
            (unsigned int)written,
            (unsigned int)length,
            path
        );

        return false;
    }

    return true;
}

// =====================================================
// 原始 CRC32（工具）
// =====================================================

bool file_storage_crc32(
    const char *path,
    uint32_t &crc_out
)
{
    crc_out = 0;

    if (!s_ready)
    {
        fs_log("E", "crc32: not initialized");
        return false;
    }

    if (!fs_path_valid(path))
    {
        return false;
    }

    File file = LittleFS.open(path, "r");

    if (!file)
    {
        fs_log("W", "crc32: open failed: %s", path);
        return false;
    }

    // 标准 CRC-32 初始值
    uint32_t crc = 0xFFFFFFFF;

    uint8_t block[FS_BLOCK_SIZE];

    bool ok = true;

    while (file.available())
    {
        int n = file.read(block, sizeof(block));

        if (n < 0)
        {
            fs_log("E", "crc32: read error: %s", path);
            ok = false;
            break;
        }

        if (n == 0)
        {
            break;
        }

        crc = fs_crc32_update(crc, block, (size_t)n);
    }

    file.close();

    if (!ok)
    {
        crc_out = 0;
        return false;
    }

    crc_out = crc ^ 0xFFFFFFFF;

    return true;
}

// =====================================================
// 流式接口
// =====================================================

bool file_storage_open_read(
    const char *path,
    FileStorageFile &file
)
{
    if (!s_ready)
    {
        fs_log("E", "open_read: not initialized");
        return false;
    }

    if (!fs_path_valid(path))
    {
        return false;
    }

    // 防止复用同一个 FileStorageFile 造成句柄泄漏
    if (file.file)
    {
        fs_log("E", "open_read: handle already open");
        return false;
    }

    file.file = LittleFS.open(path, "r");

    if (!file.file)
    {
        fs_log("W", "open_read failed: %s", path);
        return false;
    }

    return true;
}

bool file_storage_open_write(
    const char *path,
    FileStorageFile &file
)
{
    if (!s_ready)
    {
        fs_log("E", "open_write: not initialized");
        return false;
    }

    if (!fs_path_valid(path))
    {
        return false;
    }

    if (file.file)
    {
        fs_log("E", "open_write: handle already open");
        return false;
    }

    file.file = LittleFS.open(path, "w");

    if (!file.file)
    {
        fs_log("W", "open_write failed: %s", path);
        return false;
    }

    return true;
}

bool file_storage_open_append(
    const char *path,
    FileStorageFile &file
)
{
    if (!s_ready)
    {
        fs_log("E", "open_append: not initialized");
        return false;
    }

    if (!fs_path_valid(path))
    {
        return false;
    }

    if (file.file)
    {
        fs_log("E", "open_append: handle already open");
        return false;
    }

    file.file = LittleFS.open(path, "a");

    if (!file.file)
    {
        fs_log("W", "open_append failed: %s", path);
        return false;
    }

    return true;
}

size_t file_storage_read(
    FileStorageFile &file,
    uint8_t *buffer,
    size_t buffer_size
)
{
    if (!file.file || buffer == nullptr || buffer_size == 0)
    {
        return 0;
    }

    int n = file.file.read(buffer, buffer_size);

    if (n <= 0)
    {
        return 0;
    }

    return (size_t)n;
}

size_t file_storage_write(
    FileStorageFile &file,
    const uint8_t *data,
    size_t length
)
{
    if (!file.file || data == nullptr || length == 0)
    {
        return 0;
    }

    return file.file.write(data, length);
}

bool file_storage_close(
    FileStorageFile &file
)
{
    // 未打开或已关闭时幂等返回成功
    if (!file.file)
    {
        return true;
    }

    // ESP32 Arduino 的 File::flush() 与 close() 均无返回值
    file.file.flush();
    file.file.close();

    return true;
}
