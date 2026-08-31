// =====================================================
// SystemCommand 模块（V1 实现）
//
// 仅负责设备自身基础系统控制与资源查询。
// 详见 system_command.h 顶部说明与需求文档。
// =====================================================
#include "system_command.h"

#include <LittleFS.h>
#include <ESP.h>        // ESP.getHeapSize() / getFlashChipSize() 等片上资源 API

// =====================================================
// 内部辅助
// =====================================================

// 填充单个 RAM 区块的 5 个指标
static void fill_ram_block(
    JsonObject block,
    size_t total,
    size_t free_,
    size_t used,
    size_t largest_free,
    size_t min_free)
{
    block["total"] = total;
    block["free"] = free_;
    block["used"] = used;
    block["largest_free_block"] = largest_free;
    block["minimum_free"] = min_free;
}

// 递归收集 LittleFS 的目录与文件，写入 files 数组。
//
// 只读约束（需求文档 §8）：
//   - 仅 open 取元数据（name/isDirectory/size），不读写文件内容
//   - 不创建 / 删除 / 重命名任何文件
//   - 不修改 ConfigManager 的 Active / Backup / Factory 文件
//   - 直接返回扁平 path 列表，不构建嵌套 Tree 结构
//
// 部分核心版本 openNextFile() 返回的 name 不带前导 '/'，
// 这里统一规范化为 LittleFS 绝对路径。
static void files_collect(const String &dir_path, JsonArray files)
{
    File dir = LittleFS.open(dir_path);
    if (!dir || !dir.isDirectory())
    {
        if (dir)
        {
            dir.close();
        }
        return;
    }

    File f = dir.openNextFile();
    while (f)
    {
        String name = f.name();
        if (!name.startsWith("/"))
        {
            String full = dir_path;
            if (!dir_path.endsWith("/"))
            {
                full += "/";
            }
            full += name;
            name = full;
        }

        bool isDir = f.isDirectory();
        JsonObject entry = files.add<JsonObject>();
        if (!entry.isNull())
        {
            entry["path"] = name;
            entry["type"] = isDir ? "directory" : "file";
            entry["size"] = isDir ? 0 : f.size();
        }

        // 递归进入子目录（FS 深度很浅，栈开销可控）
        if (isDir)
        {
            files_collect(name, files);
        }

        f.close();
        f = dir.openNextFile();
    }
    dir.close();
}

// =====================================================
// 公开接口
// =====================================================

void system_command_init()
{
    // 当前无状态；LittleFS 由 main.cpp 在 setup 中挂载，这里不重复挂载。
    // 保留此函数以便未来扩展（如缓存句柄）。
}

bool syscmd_memory(JsonDocument &out)
{
    JsonObject data = out["data"].to<JsonObject>();
    if (data.isNull())
    {
        return false;
    }

    // ---- Internal RAM（片上 heap）----
    size_t internal_total = ESP.getHeapSize();
    size_t internal_free = ESP.getFreeHeap();
    size_t internal_used =
        (internal_total > internal_free)
            ? (internal_total - internal_free)
            : 0;
    JsonObject internal = data["internal"].to<JsonObject>();
    fill_ram_block(
        internal,
        internal_total,
        internal_free,
        internal_used,
        ESP.getMaxAllocHeap(),   // 当前最大可分配连续块
        ESP.getMinFreeHeap());   // 启动以来最小空闲（碎片参考）

    // ---- External PSRAM ----
    size_t psram_total = ESP.getPsramSize();
    JsonObject external = data["external"].to<JsonObject>();
    if (psram_total > 0)
    {
        size_t psram_free = ESP.getFreePsram();
        size_t psram_used =
            (psram_total > psram_free)
                ? (psram_total - psram_free)
                : 0;
        fill_ram_block(
            external,
            psram_total,
            psram_free,
            psram_used,
            ESP.getMaxAllocPsram(),
            ESP.getMinFreePsram());
    }
    else
    {
        // 无 PSRAM：整体填 0（属硬件差异，不报错）
        fill_ram_block(external, 0, 0, 0, 0, 0);
    }

    return true;
}

bool syscmd_flash(JsonDocument &out)
{
    JsonObject data = out["data"].to<JsonObject>();
    if (data.isNull())
    {
        return false;
    }

    // ---- External Flash = LittleFS 数据分区 ----
    size_t ext_total = LittleFS.totalBytes();
    size_t ext_used = LittleFS.usedBytes();
    if (ext_total == 0)
    {
        // 文件系统未挂载 / 不可用：明确失败，禁止返回 success
        return false;
    }
    size_t ext_free = (ext_total > ext_used) ? (ext_total - ext_used) : 0;
    JsonObject external = data["external"].to<JsonObject>();
    external["total"] = ext_total;
    external["used"] = ext_used;
    external["free"] = ext_free;

    // ---- Internal Flash = 固件区（Flash 总容量 - LittleFS 分区）----
    // 近似：固件区 = 整片 Flash - 数据分区。
    // 精确分区布局见 partitions.csv（nvs/otadata/app0/app1 位于 LittleFS 之前）。
    // used 以当前运行 app 大小（ESP.getSketchSize()）作为固件占用代理值。
    size_t chip = ESP.getFlashChipSize();
    size_t int_total = (chip > ext_total) ? (chip - ext_total) : 0;
    size_t int_used = ESP.getSketchSize();
    size_t int_free = (int_total > int_used) ? (int_total - int_used) : 0;
    JsonObject internal = data["internal"].to<JsonObject>();
    internal["total"] = int_total;
    internal["used"] = int_used;
    internal["free"] = int_free;

    // ---- 递归文件列表（只读）----
    JsonArray files = data["files"].to<JsonArray>();
    if (files.isNull())
    {
        return false;
    }
    files_collect("/", files);

    return true;
}
