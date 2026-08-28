#include "json_storage.h"

#include <LittleFS.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

// =====================================================
// 内部常量
// =====================================================

// LittleFS 挂载参数：与 main.cpp 保持一致
//   main.cpp: LittleFS.begin(true, "/littlefs", 10, "littlefs")
//
// 注意此处 formatOnFail 固定为 false：
//   格式化是破坏性操作，不应由底层存储模块自行决定。
//   系统在 setup 阶段已完成挂载，本模块只做可用性确认。
#define JS_BASE_PATH        "/littlefs"
#define JS_MAX_OPEN_FILES   10
#define JS_PARTITION_LABEL  "littlefs"

// 原子写使用的临时文件后缀
#define JS_TMP_SUFFIX       ".tmp"

// 内部读取块大小：栈上缓冲，避免堆分配与内存碎片
#define JS_BLOCK_SIZE       512

// 路径缓冲区大小（LittleFS 路径通常远小于此值）
#define JS_PATH_MAX         256

// 一次性读取的体积上限（32 KB）
//
// json_storage_read() 只服务小型 JSON。超过此值一律拒绝，
// 强制调用方改用 json_storage_read_chunk() 分块处理，
// 避免大文件被整体灌进 RAM 造成 OOM。
#define JS_MAX_FULL_READ    32768

// 单条日志上限，超长截断，避免栈溢出
#define JS_LOG_BUF_SIZE     192

// =====================================================
// 内部状态
// =====================================================
static bool s_ready = false;
static JsonStorageLogCallback s_log_cb = nullptr;

// =====================================================
// 内部工具
// =====================================================

// 统一日志出口。未注册回调时静默，不产生任何串口输出。
static void js_log(
    const char *level,
    const char *fmt,
    ...
)
{
    if (s_log_cb == nullptr)
    {
        return;
    }

    char buf[JS_LOG_BUF_SIZE];

    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    s_log_cb(level, buf);
}

// 合成 "<path>.tmp"
// 使用定长缓冲区而非 String，避免底层存储模块引入堆分配。
static bool js_make_tmp_path(
    const char *path,
    char *out,
    size_t out_size
)
{
    if (path == nullptr || out == nullptr || out_size == 0)
    {
        return false;
    }

    size_t len = strlen(path);
    size_t suffix_len = sizeof(JS_TMP_SUFFIX) - 1;

    // +1 留给字符串结束符
    if (len + suffix_len + 1 > out_size)
    {
        return false;
    }

    memcpy(out, path, len);
    memcpy(out + len, JS_TMP_SUFFIX, suffix_len + 1);

    return true;
}

// =====================================================
// 日志回调
// =====================================================
void json_storage_set_log_callback(
    JsonStorageLogCallback callback
)
{
    s_log_cb = callback;
}

// =====================================================
// 初始化
// =====================================================
bool json_storage_init()
{
    // 幂等：已初始化直接返回
    if (s_ready)
    {
        return true;
    }

    // 系统在 setup() 阶段已完成 LittleFS 挂载，本模块默认直接使用，
    // 不在每次读写时重复 begin/end。
    //
    // totalBytes() 在未挂载时返回 0，因此只有当探测到尚未挂载时，
    // 才做一次兜底挂载尝试。
    // 兜底挂载同样使用 formatOnFail=false：
    //   格式化是破坏性操作，不应由底层存储模块自行决定。
    if (LittleFS.totalBytes() == 0)
    {
        if (!LittleFS.begin(
                false,
                JS_BASE_PATH,
                JS_MAX_OPEN_FILES,
                JS_PARTITION_LABEL
            ))
        {
            s_ready = false;
            js_log("E", "LittleFS unavailable (mount failed)");
            return false;
        }
    }

    s_ready = true;
    js_log("I", "ready");

    return true;
}

// =====================================================
// 文件基础操作
// =====================================================
bool json_storage_exists(
    const char *path
)
{
    if (!s_ready)
    {
        // 未初始化时必须留下痕迹：
        // 若直接静默返回 false，上层可能误读成"配置文件不存在"，
        // 进而用默认值覆盖掉真实存在的配置。
        js_log("E", "exists: not initialized");
        return false;
    }

    if (path == nullptr || path[0] == '\0')
    {
        return false;
    }

    return LittleFS.exists(path);
}

size_t json_storage_size(
    const char *path
)
{
    if (!s_ready)
    {
        js_log("E", "size: not initialized");
        return 0;
    }

    if (path == nullptr || path[0] == '\0')
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

bool json_storage_remove(
    const char *path
)
{
    if (!s_ready)
    {
        js_log("E", "remove: not initialized");
        return false;
    }

    if (path == nullptr || path[0] == '\0')
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
        js_log("E", "remove failed: %s", path);
        return false;
    }

    return true;
}

bool json_storage_rename(
    const char *from,
    const char *to
)
{
    if (!s_ready)
    {
        js_log("E", "rename: not initialized");
        return false;
    }

    if (from == nullptr || to == nullptr)
    {
        return false;
    }

    if (from[0] == '\0' || to[0] == '\0')
    {
        return false;
    }

    if (!LittleFS.exists(from))
    {
        js_log("E", "rename: source missing: %s", from);
        return false;
    }

    // 目标存在时由文件系统覆盖（littlefs rename 语义）。
    // 本函数不理解"为什么 rename"，轮换策略由上层决定。
    if (!LittleFS.rename(from, to))
    {
        js_log("E", "rename failed: %s -> %s", from, to);
        return false;
    }

    return true;
}

// =====================================================
// 一次性读写
// =====================================================
bool json_storage_read(
    const char *path,
    String &output
)
{
    output.clear();

    if (!s_ready)
    {
        js_log("E", "read: not initialized");
        return false;
    }

    if (path == nullptr || path[0] == '\0')
    {
        return false;
    }

    File file = LittleFS.open(path, "r");
    if (!file)
    {
        js_log("W", "read: open failed: %s", path);
        return false;
    }

    size_t total = file.size();

    // 防御：一次性读取只服务小型文件。超限直接拒绝，
    // 强制调用方改用分块读取，避免大文件整体进 RAM 造成 OOM。
    if (total > JS_MAX_FULL_READ)
    {
        js_log(
            "E",
            "read: file too large (%u > %u), use read_chunk: %s",
            (unsigned int)total,
            (unsigned int)JS_MAX_FULL_READ,
            path
        );
        file.close();
        return false;
    }

    bool ok = true;

    if (total > 0)
    {
        // 一次性预留，避免追加过程中反复重分配与拷贝
        if (!output.reserve(total))
        {
            js_log(
                "E",
                "read: reserve %u bytes failed: %s",
                (unsigned int)total,
                path
            );
            ok = false;
        }
        else
        {
            uint8_t block[JS_BLOCK_SIZE];

            while (file.available())
            {
                int n = file.read(block, sizeof(block));

                if (n < 0)
                {
                    js_log("E", "read: read error: %s", path);
                    ok = false;
                    break;
                }

                if (n == 0)
                {
                    break;
                }

                if (!output.concat((const char *)block, (unsigned int)n))
                {
                    js_log("E", "read: concat failed: %s", path);
                    ok = false;
                    break;
                }
            }
        }
    }

    file.close();

    // 读到的长度必须与文件大小一致，否则视为不完整
    if (ok && output.length() != total)
    {
        js_log(
            "E",
            "read: length mismatch (%u != %u): %s",
            (unsigned int)output.length(),
            (unsigned int)total,
            path
        );
        ok = false;
    }

    if (!ok)
    {
        output.clear();
    }

    return ok;
}

bool json_storage_write(
    const char *path,
    const String &data
)
{
    if (!s_ready)
    {
        js_log("E", "write: not initialized");
        return false;
    }

    if (path == nullptr || path[0] == '\0')
    {
        return false;
    }

    File file = LittleFS.open(path, "w");
    if (!file)
    {
        js_log("W", "write: open failed: %s", path);
        return false;
    }

    size_t want = data.length();
    size_t written = 0;

    if (want > 0)
    {
        written = file.write((const uint8_t *)data.c_str(), want);
    }

    file.flush();
    file.close();

    // 打开成功不等于写入成功，必须校验实际字节数
    if (written != want)
    {
        js_log(
            "E",
            "write: %u/%u bytes: %s",
            (unsigned int)written,
            (unsigned int)want,
            path
        );
        return false;
    }

    return true;
}

bool json_storage_write_atomic(
    const char *path,
    const String &data
)
{
    if (!s_ready)
    {
        js_log("E", "atomic: not initialized");
        return false;
    }

    if (path == nullptr || path[0] == '\0')
    {
        return false;
    }

    char tmp[JS_PATH_MAX];

    if (!js_make_tmp_path(path, tmp, sizeof(tmp)))
    {
        js_log("E", "atomic: tmp path too long: %s", path);
        return false;
    }

    // 1) 完整写入临时文件
    if (!json_storage_write(tmp, data))
    {
        // 清理可能残留的半截文件，不留垃圾
        json_storage_remove(tmp);
        js_log("E", "atomic: tmp write failed: %s", tmp);
        return false;
    }

    // 2) 落盘校验：大小一致才允许替换正式文件
    if (json_storage_size(tmp) != data.length())
    {
        json_storage_remove(tmp);
        js_log("E", "atomic: tmp size mismatch: %s", tmp);
        return false;
    }

    // 3) 原子替换
    //    替换前后，正式文件始终是完整内容（旧或新），
    //    不存在"写了一半"的中间状态。
    if (!LittleFS.rename(tmp, path))
    {
        json_storage_remove(tmp);
        js_log("E", "atomic: rename failed: %s -> %s", tmp, path);
        return false;
    }

    return true;
}

// =====================================================
// 分块读写
// =====================================================
bool json_storage_read_chunk(
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
        js_log("E", "read_chunk: not initialized");
        return false;
    }

    if (path == nullptr || path[0] == '\0')
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
        js_log("W", "read_chunk: open failed: %s", path);
        return false;
    }

    size_t total = file.size();

    // 恰好位于文件末尾：EOF，以 bytes_read == 0 表示
    if (offset == total)
    {
        file.close();
        return true;
    }

    // 越过文件末尾：属于调用错误（分片乱序 / offset 计算错误）。
    // 与 write_chunk 保持同样的严格性，立刻暴露失败，
    // 而不是静默返回空数据让上层误以为传输完成。
    if (offset > total)
    {
        js_log(
            "E",
            "read_chunk: offset %u beyond size %u: %s",
            (unsigned int)offset,
            (unsigned int)total,
            path
        );
        file.close();
        return false;
    }

    if (!file.seek(offset))
    {
        js_log(
            "E",
            "read_chunk: seek %u failed: %s",
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
        js_log("E", "read_chunk: read error: %s", path);
        return false;
    }

    bytes_read = (size_t)n;

    return true;
}

bool json_storage_write_chunk(
    const char *path,
    size_t offset,
    const uint8_t *data,
    size_t length
)
{
    if (!s_ready)
    {
        js_log("E", "write_chunk: not initialized");
        return false;
    }

    if (path == nullptr || path[0] == '\0')
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
        // 必须截断。若沿用 "r+" 从头部覆写，当新数据短于旧内容时，
        // 尾部会残留旧字节，产出半新半旧的损坏文件。
        file = LittleFS.open(path, "w");
    }
    else
    {
        // 续写：要求文件已存在
        file = LittleFS.open(path, "r+");

        if (!file)
        {
            js_log(
                "E",
                "write_chunk: file missing, cannot resume at %u: %s",
                (unsigned int)offset,
                path
            );
            return false;
        }

        // 拒绝越界 offset：
        //   分块接收出现丢块或乱序时应当立刻暴露失败，
        //   而不是生成一个中间填 0 的损坏文件。
        //   文件大小直接从已打开的句柄读取，避免重复开关文件。
        size_t total = file.size();

        if (offset > total)
        {
            js_log(
                "E",
                "write_chunk: offset %u beyond size %u: %s",
                (unsigned int)offset,
                (unsigned int)total,
                path
            );
            file.close();
            return false;
        }

        if (!file.seek(offset))
        {
            js_log("E", "write_chunk: seek failed: %s", path);
            file.close();
            return false;
        }
    }

    if (!file)
    {
        js_log("W", "write_chunk: open failed: %s", path);
        return false;
    }

    size_t written = file.write(data, length);

    file.flush();
    file.close();

    if (written != length)
    {
        js_log(
            "E",
            "write_chunk: %u/%u bytes: %s",
            (unsigned int)written,
            (unsigned int)length,
            path
        );
        return false;
    }

    return true;
}

// =====================================================
// 流式接口
// =====================================================
bool json_storage_open_read(
    const char *path,
    JsonStorageFile &file
)
{
    if (!s_ready)
    {
        js_log("E", "open_read: not initialized");
        return false;
    }

    if (path == nullptr || path[0] == '\0')
    {
        return false;
    }

    // 防止复用同一个 JsonStorageFile 造成句柄泄漏：
    // LittleFS 的句柄数量有限，耗尽后所有 open 都会失败且难以定位。
    if (file.file)
    {
        js_log("E", "open_read: handle already open");
        return false;
    }

    file.file = LittleFS.open(path, "r");

    if (!file.file)
    {
        js_log("W", "open_read failed: %s", path);
        return false;
    }

    return true;
}

bool json_storage_open_write(
    const char *path,
    JsonStorageFile &file
)
{
    if (!s_ready)
    {
        js_log("E", "open_write: not initialized");
        return false;
    }

    if (path == nullptr || path[0] == '\0')
    {
        return false;
    }

    if (file.file)
    {
        js_log("E", "open_write: handle already open");
        return false;
    }

    file.file = LittleFS.open(path, "w");

    if (!file.file)
    {
        js_log("W", "open_write failed: %s", path);
        return false;
    }

    return true;
}

bool json_storage_open_append(
    const char *path,
    JsonStorageFile &file
)
{
    if (!s_ready)
    {
        js_log("E", "open_append: not initialized");
        return false;
    }

    if (path == nullptr || path[0] == '\0')
    {
        return false;
    }

    if (file.file)
    {
        js_log("E", "open_append: handle already open");
        return false;
    }

    file.file = LittleFS.open(path, "a");

    if (!file.file)
    {
        js_log("W", "open_append failed: %s", path);
        return false;
    }

    return true;
}

size_t json_storage_read(
    JsonStorageFile &file,
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

size_t json_storage_write(
    JsonStorageFile &file,
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

bool json_storage_close(
    JsonStorageFile &file
)
{
    // 未打开或已关闭时幂等返回成功
    if (!file.file)
    {
        return true;
    }

    // 注意：ESP32 Arduino 的 File::flush() 与 close() 均无返回值，
    // 因此本接口无法感知底层落盘失败（分区写满、flash 写保护等）。
    // 这是框架限制。需要可靠确认落盘的场景，请改用
    // json_storage_write() 并校验其返回的字节数。
    file.file.flush();
    file.file.close();

    return true;
}
