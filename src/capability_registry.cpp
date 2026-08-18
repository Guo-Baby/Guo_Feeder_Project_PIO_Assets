#include "capability_registry.h"

#include <LittleFS.h>

#include "workflow.h"

// =====================================================
// RAM Runtime Cache
//
// 初始化完成后所有查询只访问 RAM，禁止频繁读取 Flash。
// =====================================================
static CapabilityRegistryTable g_action_table;
static CapabilityRegistryTable g_trigger_table;
static CapabilityRegistryTable g_workflow_table;
static bool g_initialized = false;

// =====================================================
// 轻量 CRC32（逐位实现，无查表，输入量小）
//
// 标准 CRC-32 (IEEE 802.3)：
//   poly = 0xEDB88320
//   init = 0xFFFFFFFF，结果异或 0xFFFFFFFF
// =====================================================
static uint32_t crc32_byte(uint32_t crc, uint8_t byte)
{
    crc ^= byte;
    for (uint8_t i = 0; i < 8; i++) {
        if (crc & 1UL) {
            crc = (crc >> 1) ^ 0xEDB88320UL;
        } else {
            crc >>= 1;
        }
    }
    return crc;
}

// =====================================================
// 按映射顺序计算 checksum
//
// 输入固定顺序（每个条目）：
//   1 字节 stable_id
//   1 字节 runtime_id 长度
//   runtime_id 字节
// 因此 stable_id / 顺序 / 增删条目变化都会导致 checksum 变化。
// 不包含函数指针 / 运行时指针 / 实例状态等非稳定信息。
// =====================================================
static uint32_t registry_checksum(const CapabilityRegistryTable &table)
{
    uint32_t crc = 0xFFFFFFFFUL;
    for (uint8_t i = 0; i < table.count; i++)
    {
        uint8_t sid = table.entries[i].stable_id;
        uint8_t len = (uint8_t)table.entries[i].runtime_id.length();
        crc = crc32_byte(crc, sid);
        // 加入version
        uint32_t ver = table.entries[i].object_version;
        crc = crc32_byte(crc, (uint8_t)(ver & 0xFF));
        crc = crc32_byte(crc, (uint8_t)((ver >> 8) & 0xFF));
        crc = crc32_byte(crc, (uint8_t)((ver >> 16) & 0xFF));
        crc = crc32_byte(crc, (uint8_t)((ver >> 24) & 0xFF));
        crc = crc32_byte(crc, len);
                for (uint8_t j = 0; j < len; j++)
                {
                    crc = crc32_byte(crc, (uint8_t)table.entries[i].runtime_id[j]);
                }
                /*
                    Workflow Version 预留
                    未来 WorkflowDescriptor 增加 version 后：
                    这里加入:
                    workflow_version uint32_t
                    例如:
                    crc = crc32_byte(crc, version byte0);
                    crc = crc32_byte(crc, version byte1);
                    当前版本：
                    不加入，避免修改 workflow 结构。
                */
}
    return crc ^ 0xFFFFFFFFUL;
}

// =====================================================
// 表清理（String 用赋值清空，避免 memcpy 复制指针）
// =====================================================
static void table_reset(CapabilityRegistryTable &table)
{
    for (uint8_t i = 0; i < CAPABILITY_MAX_ENTRY; i++) {
        table.entries[i].stable_id = 0;
        table.entries[i].runtime_id = "";
    }
    table.version = 0;
    table.checksum = 0;
    table.count = 0;
}

// =====================================================
// 固定小端字段读写（不依赖结构体内存布局）
// =====================================================
static bool file_write_u32(File &file, uint32_t value)
{
    uint8_t buf[4];
    buf[0] = (uint8_t)(value & 0xFFUL);
    buf[1] = (uint8_t)((value >> 8) & 0xFFUL);
    buf[2] = (uint8_t)((value >> 16) & 0xFFUL);
    buf[3] = (uint8_t)((value >> 24) & 0xFFUL);
    return file.write(buf, 4) == 4;
}

static bool file_read_u32(File &file, uint32_t &value)
{
    uint8_t buf[4];
    if (file.read(buf, 4) != 4) {
        return false;
    }
    value = (uint32_t)buf[0]
          | ((uint32_t)buf[1] << 8)
          | ((uint32_t)buf[2] << 16)
          | ((uint32_t)buf[3] << 24);
    return true;
}

static bool file_write_u16(File &file, uint16_t value)
{
    uint8_t buf[2];
    buf[0] = (uint8_t)(value & 0xFF);
    buf[1] = (uint8_t)((value >> 8) & 0xFF);
    return file.write(buf, 2) == 2;
}

static bool file_read_u16(File &file, uint16_t &value)
{
    uint8_t buf[2];
    if (file.read(buf, 2) != 2) {
        return false;
    }
    value = (uint16_t)buf[0] | ((uint16_t)buf[1] << 8);
    return true;
}

// =====================================================
// Flash 写入 Mapping
// =====================================================
static bool save_registry_file(
    const char *path,
    uint32_t magic,
    const CapabilityRegistryTable &table
)
{
    if (!LittleFS.mkdir(CAPABILITY_REGISTRY_DIR)) {
        // 目录创建失败时继续尝试打开文件，由 open 结果决定成败
    }

    // 断电安全：先写完整临时文件，成功后再替换正式文件。
    // 正式文件任何时候要么是完整旧版本，要么是完整新版本。
    String tmp_path = String(path) + ".tmp";

    File file = LittleFS.open(tmp_path.c_str(), "w");
    if (!file) {
        Serial.printf(
            "[CapRegistry] open %s for write failed\n",
            tmp_path.c_str()
        );
        return false;
    }

    bool ok = true;
    ok = ok && file_write_u32(file, magic);
    ok = ok && file_write_u32(file, table.version);
    ok = ok && file_write_u16(file, table.count);
    ok = ok && file_write_u32(file, table.checksum);

    for (uint8_t i = 0; ok && i < table.count; i++) {
        uint16_t len = table.entries[i].runtime_id.length();
        if (len == 0 || len > 255) {
            ok = false;
            break;
        }
        uint8_t sid = table.entries[i].stable_id;
        uint8_t l8 = (uint8_t)len;
        ok = ok && file.write(&sid, 1) == 1;
        ok = ok && file.write(&l8, 1) == 1;
        ok = ok && file.write(
            (const uint8_t *)table.entries[i].runtime_id.c_str(),
            len
        ) == len;
    }

    file.flush();   // 确保数据落盘（框架中为 void）
    file.close();

    if (!ok) {
        Serial.printf(
            "[CapRegistry] write %s failed\n",
            tmp_path.c_str()
        );
        return false;
    }

    // =====================================================
    // 安全替换正式文件
    // 保证任何时刻至少存在一份有效registry文件 old -> backup, tmp -> real, delete backup
    // =====================================================
    String backup_path = String(path) + ".bak";
    // 删除旧backup
    if (LittleFS.exists(backup_path.c_str()))
    {
        LittleFS.remove(backup_path.c_str());
    }
    // 原文件存在，则先备份
    if (LittleFS.exists(path))
    {
        if (!LittleFS.rename(
                path,
                backup_path.c_str()))
        {
            Serial.printf(
                "[CapRegistry] backup rename failed\n"
            );
            return false;
        }
    }
    // tmp成为正式文件
    if (!LittleFS.rename(
            tmp_path.c_str(),
            path))
    {
        Serial.printf(
            "[CapRegistry] tmp rename failed\n"
        );
        // 尝试恢复旧文件
        if (LittleFS.exists(backup_path.c_str()))
        {
            LittleFS.rename(
                backup_path.c_str(),
                path
            );
        }
        return false;
    }
    // 删除backup
    if (LittleFS.exists(backup_path.c_str()))
    {
        LittleFS.remove(
            backup_path.c_str()
        );
    }
    return true;
}

// =====================================================
// Flash 读取 Mapping
//
// 返回 false 表示：文件不存在 / 损坏 / 魔数不符 / 版本非法(0) /
// 条目结构非法。调用方按"Mapping 不存在"处理。
// =====================================================
static bool load_registry_file(
    const char *path,
    uint32_t magic,
    CapabilityRegistryTable &table
)
{
    File file = LittleFS.open(path, "r");
    if (!file) {
        return false;
    }

    uint32_t file_magic = 0;
    uint32_t file_version = 0;
    uint16_t file_count = 0;
    uint32_t file_checksum = 0;

    bool ok = true;
    ok = ok && file_read_u32(file, file_magic);
    ok = ok && file_read_u32(file, file_version);
    ok = ok && file_read_u16(file, file_count);
    ok = ok && file_read_u32(file, file_checksum);

    if (!ok
        || file_magic != magic
        || file_version == 0
        || file_count > CAPABILITY_MAX_ENTRY)
    {
        file.close();
        return false;
    }

    CapabilityRegistryTable tmp;
    table_reset(tmp);

    for (uint16_t i = 0; i < file_count; i++) {
        uint8_t sid = 0;
        uint8_t len = 0;
        if (file.read(&sid, 1) != 1
            || file.read(&len, 1) != 1)
        {
            ok = false;
            break;
        }
        if (len == 0) {
            ok = false;
            break;
        }
        char buf[256];
        if (file.read((uint8_t *)buf, len) != len) {
            ok = false;
            break;
        }
        buf[len] = '\0';
        tmp.entries[i].stable_id = sid;
        tmp.entries[i].runtime_id = String(buf, len);
        tmp.count = (uint8_t)(i + 1);
    }

    file.close();

    if (!ok) {
        return false;
    }

    // 结构校验：stable_id 必须从 0 连续递增
    for (uint16_t i = 0; i < file_count; i++) {
        if (tmp.entries[i].stable_id != (uint8_t)i) {
            return false;
        }
    }

    table_reset(table);
    table.version = file_version;
    table.checksum = file_checksum;
    table.count = tmp.count;
    for (uint16_t i = 0; i < file_count; i++) {
        table.entries[i].stable_id = tmp.entries[i].stable_id;
        table.entries[i].runtime_id = tmp.entries[i].runtime_id;
    }
    return true;
}

// =====================================================
// 扫描当前已注册的 Action 列表（只读，不修改注册代码）
// =====================================================
static uint8_t scan_action_ids(
    CapabilityMapping *out,
    uint8_t max
)
{
    uint8_t count = 0;
    uint8_t total = workflow_get_action_count();

    for (uint8_t i = 0; i < total && count < max; i++) {
        const WorkflowActionDescriptor *desc =
            workflow_get_action_descriptor(i);
        if (desc == nullptr || desc->id == nullptr) {
            continue;
        }
        String id(desc->id);
        if (id.length() == 0) {
            continue;
        }
        // 防御重复（框架已保证唯一）
        bool dup = false;
        for (uint8_t k = 0; k < count; k++) {
            if (out[k].runtime_id == id) {
                dup = true;
                break;
            }
        }
        if (dup) {
            continue;
        }
        out[count].runtime_id = id;
        out[count].object_version = 0;
        count++;
    }
    return count;
}

// =====================================================
// 扫描当前已注册的 Trigger 列表（只读）
// =====================================================
static uint8_t scan_trigger_ids(
    CapabilityMapping *out,
    uint8_t max
)
{
    uint8_t count = 0;
    uint8_t total = workflow_get_trigger_count();

    for (uint8_t i = 0; i < total && count < max; i++) {
        const WorkflowTriggerDescriptor *desc =
            workflow_get_trigger_descriptor(i);
        if (desc == nullptr || desc->id == nullptr) {
            continue;
        }
        String id(desc->id);
        if (id.length() == 0) {
            continue;
        }
        bool dup = false;
        for (uint8_t k = 0; k < count; k++) {
            if (out[k].runtime_id == id) {
                dup = true;
                break;
            }
        }
        if (dup) {
            continue;
        }
        out[count].runtime_id = id;
        out[count].object_version = 0;
        count++;
    }
    return count;
}

// =====================================================
// 扫描当前已加载的 Workflow 列表（只读）
// =====================================================
static uint8_t scan_workflow_ids(
    CapabilityMapping *out,
    uint8_t max
)
{
    uint8_t count = 0;
    uint8_t total = workflow_get_count();

    for (uint8_t i = 0; i < total && count < max; i++) {
        Workflow *wf = workflow_get(i);
        if (wf == nullptr || wf->id.length() == 0) {
            continue;
        }
        bool dup = false;
        for (uint8_t k = 0; k < count; k++) {
            if (out[k].runtime_id == wf->id) {
                dup = true;
                break;
            }
        }
        if (dup) {
            continue;
        }
        out[count].runtime_id = wf->id;
        // TODO Task9:
        // 等 workflow_version 接入 workflow.cpp 后启用
        //
        // out[count].object_version = wf->version;
        out[count].object_version = 0;
        count++;
    }
    return count;
}

// =====================================================
// 对 mapping 按 runtime_id 字符串升序排序
//
// Stable ID 由 runtime_id 唯一决定：排序后的 index 即 stable_id。
// 因此注册顺序变化（新增 / 删除 / 调整顺序）不会改变
// 同一 runtime_id 的 stable_id。
//
// 只排序本模块内部 mapping，不修改原始 workflow registry。
// =====================================================
static void sort_capability_mapping(
    CapabilityMapping *arr,
    uint8_t count
)
{
    for (uint8_t i = 1; i < count; i++) {
        CapabilityMapping key = arr[i];
        int j = (int)i - 1;
        while (j >= 0
               && arr[j].runtime_id.compareTo(key.runtime_id) > 0)
        {
            arr[j + 1] = arr[j];
            j--;
        }
        arr[j + 1] = key;
    }
}

static const char *capability_type_name(CapabilityType type)
{
    switch (type) {
        case CAP_ACTION:   return "ACTION";
        case CAP_TRIGGER:  return "TRIGGER";
        case CAP_WORKFLOW: return "WORKFLOW";
        default:           return "UNKNOWN";
    }
}

// =====================================================
// 单类 Registry 同步：
//   1. 扫描当前能力列表
//   2. 计算当前 checksum
//   3. 读取 Flash；一致则复用，不一致/不存在则重建并保存
// =====================================================
static bool registry_sync(CapabilityType type)
{
    const char *path = nullptr;
    uint32_t magic = 0;
    CapabilityRegistryTable *table = nullptr;
    CapabilityMapping current[CAPABILITY_MAX_ENTRY];
    uint8_t count = 0;

    switch (type) {
        case CAP_ACTION:
            path = CAPABILITY_ACTION_FILE;
            magic = CAPABILITY_MAGIC_ACTION;
            table = &g_action_table;
            count = scan_action_ids(current, CAPABILITY_MAX_ENTRY);
            break;
        case CAP_TRIGGER:
            path = CAPABILITY_TRIGGER_FILE;
            magic = CAPABILITY_MAGIC_TRIGGER;
            table = &g_trigger_table;
            count = scan_trigger_ids(current, CAPABILITY_MAX_ENTRY);
            break;
        case CAP_WORKFLOW:
            path = CAPABILITY_WORKFLOW_FILE;
            magic = CAPABILITY_MAGIC_WORKFLOW;
            table = &g_workflow_table;
            count = scan_workflow_ids(current, CAPABILITY_MAX_ENTRY);
            break;
        default:
            return false;
    }

    // 当前能力列表
    CapabilityRegistryTable current_table;
    table_reset(current_table);
    current_table.count = count;
    for (uint8_t i = 0; i < count; i++) {
        current_table.entries[i].runtime_id =
            current[i].runtime_id;
    }
    // Stable ID 由 runtime_id 排序决定，与注册顺序无关
    sort_capability_mapping(
        current_table.entries,
        current_table.count
    );
    for (uint8_t i = 0; i < count; i++) {
        current_table.entries[i].stable_id = i;
    }
    current_table.checksum =
        registry_checksum(current_table);

    CapabilityRegistryTable loaded;
    table_reset(loaded);
    bool loaded_ok = load_registry_file(
        path,
        magic,
        loaded
    );

    if (loaded_ok
        && loaded.checksum == current_table.checksum)
    {
        // 校验一致：读取 Flash Mapping，保持原 version
        table_reset(*table);
        table->version = loaded.version;
        table->checksum = loaded.checksum;
        table->count = loaded.count;
        for (uint8_t i = 0; i < loaded.count; i++) {
            table->entries[i].stable_id =
                loaded.entries[i].stable_id;
            table->entries[i].runtime_id =
                loaded.entries[i].runtime_id;
        }
        Serial.printf(
            "[CapRegistry] %s reuse version=%u count=%u\n",
            capability_type_name(type),
            table->version,
            table->count
        );
    }
    else
    {
        // 校验失败 / 文件不存在 / 损坏：
        // 重新生成 Mapping，version = old + 1（无旧版则初始 1）
        uint32_t new_version =
            loaded_ok ? (loaded.version + 1UL) : 1UL;

        table_reset(*table);
        table->version = new_version;
        table->checksum = current_table.checksum;
        table->count = count;
        for (uint8_t i = 0; i < count; i++) {
            table->entries[i].stable_id = i;
            table->entries[i].runtime_id =
                current_table.entries[i].runtime_id;
        }

if (!save_registry_file(path, magic, *table))
{
    Serial.printf(
        "[CapRegistry] %s flash save failed, RAM cache kept\n",
        capability_type_name(type)
    );
}

Serial.printf(
    "[CapRegistry] %s rebuild version=%u count=%u checksum=%u\n",
    capability_type_name(type),
    table->version,
    table->count,
    table->checksum
);
    }

    return true;
}

// =====================================================
// 初始化
//
// 必须在所有 Action / Trigger 注册完成
// 且 Workflow JSON 加载完成后调用。
// =====================================================
bool capability_registry_init()
{
    table_reset(g_action_table);
    table_reset(g_trigger_table);
    table_reset(g_workflow_table);
    g_initialized = false;

    bool ok = capability_registry_rescan();

    // 即使 Flash 写入失败，RAM Cache 仍然可用
    g_initialized = true;
    Serial.printf("capability_registry_init: %s\n", ok ? "OK" : "FAIL");
    return ok;
}

// =====================================================
// 重新扫描（供未来 Workflow 更新后重建 Mapping）
// =====================================================
bool capability_registry_rescan()
{
    bool ok = true;
    ok = registry_sync(CAP_ACTION) && ok;
    ok = registry_sync(CAP_TRIGGER) && ok;
    ok = registry_sync(CAP_WORKFLOW) && ok;
    return ok;
}

// =====================================================
// 调试输出
// =====================================================
static void dump_table(
    const char *name,
    const CapabilityRegistryTable &table
)
{
    Serial.printf("[%s]\n", name);
    Serial.printf("version=%u\n", table.version);
    Serial.printf("checksum=%u\n", table.checksum);
    for (uint8_t i = 0; i < table.count; i++) {
        Serial.printf(
            "%u %s\n",
            table.entries[i].stable_id,
            table.entries[i].runtime_id.c_str()
        );
    }
}

void capability_registry_dump()
{
    Serial.println("[CapRegistry] === dump ===");
    dump_table("ACTION", g_action_table);
    dump_table("TRIGGER", g_trigger_table);
    dump_table("WORKFLOW", g_workflow_table);
}

// =====================================================
// 数量 / 版本 / 校验和
// =====================================================
uint8_t capability_get_action_count()
{
    return g_initialized ? g_action_table.count : 0;
}

uint32_t capability_get_action_version()
{
    return g_initialized ? g_action_table.version : 0;
}

uint32_t capability_get_action_checksum()
{
    return g_initialized ? g_action_table.checksum : 0;
}

uint8_t capability_get_trigger_count()
{
    return g_initialized ? g_trigger_table.count : 0;
}

uint32_t capability_get_trigger_version()
{
    return g_initialized ? g_trigger_table.version : 0;
}

uint32_t capability_get_trigger_checksum()
{
    return g_initialized ? g_trigger_table.checksum : 0;
}

uint8_t capability_get_workflow_count()
{
    return g_initialized ? g_workflow_table.count : 0;
}

uint32_t capability_get_workflow_version()
{
    return g_initialized ? g_workflow_table.version : 0;
}

uint32_t capability_get_workflow_checksum()
{
    return g_initialized ? g_workflow_table.checksum : 0;
}

// =====================================================
// 双向查询
// =====================================================
bool capability_get_action_by_stable_id(
    uint8_t stable_id,
    String &runtime_id
)
{
    if (!g_initialized || stable_id >= g_action_table.count) {
        return false;
    }
    if (g_action_table.entries[stable_id].stable_id
        != stable_id)
    {
        return false;
    }
    runtime_id = g_action_table.entries[stable_id].runtime_id;
    return true;
}

bool capability_get_action_stable_id(
    const String &runtime_id,
    uint8_t &stable_id
)
{
    if (!g_initialized) {
        return false;
    }
    for (uint8_t i = 0; i < g_action_table.count; i++) {
        if (g_action_table.entries[i].runtime_id == runtime_id) {
            stable_id = g_action_table.entries[i].stable_id;
            return true;
        }
    }
    return false;
}

bool capability_get_trigger_by_stable_id(
    uint8_t stable_id,
    String &runtime_id
)
{
    if (!g_initialized || stable_id >= g_trigger_table.count) {
        return false;
    }
    if (g_trigger_table.entries[stable_id].stable_id
        != stable_id)
    {
        return false;
    }
    runtime_id = g_trigger_table.entries[stable_id].runtime_id;
    return true;
}

bool capability_get_trigger_stable_id(
    const String &runtime_id,
    uint8_t &stable_id
)
{
    if (!g_initialized) {
        return false;
    }
    for (uint8_t i = 0; i < g_trigger_table.count; i++) {
        if (g_trigger_table.entries[i].runtime_id == runtime_id) {
            stable_id = g_trigger_table.entries[i].stable_id;
            return true;
        }
    }
    return false;
}

bool capability_get_workflow_by_stable_id(
    uint8_t stable_id,
    String &runtime_id
)
{
    if (!g_initialized || stable_id >= g_workflow_table.count) {
        return false;
    }
    if (g_workflow_table.entries[stable_id].stable_id
        != stable_id)
    {
        return false;
    }
    runtime_id = g_workflow_table.entries[stable_id].runtime_id;
    return true;
}

bool capability_get_workflow_stable_id(
    const String &runtime_id,
    uint8_t &stable_id
)
{
    if (!g_initialized) {
        return false;
    }
    for (uint8_t i = 0; i < g_workflow_table.count; i++) {
        if (g_workflow_table.entries[i].runtime_id == runtime_id) {
            stable_id = g_workflow_table.entries[i].stable_id;
            return true;
        }
    }
    return false;
}

// =====================================================
// Registry 导出
// =====================================================
static String export_table(
    const char *type_name,
    const CapabilityRegistryTable &table
)
{
    String out;
    out.reserve(256);

    out += "[";
    out += type_name;
    out += "]\n";

    out += "version=";
    out += String(table.version);
    out += "\n";

    out += "count=";
    out += String(table.count);
    out += "\n";

    out += "checksum=";
    out += String(table.checksum);
    out += "\n";

    for (uint8_t i = 0; i < table.count; i++) {
        out += String(table.entries[i].stable_id);
        out += "=";
        out += table.entries[i].runtime_id;
        out += "\n";
    }
    return out;
}

String capability_export_action_registry()
{
    return export_table("ACTION", g_action_table);
}

String capability_export_trigger_registry()
{
    return export_table("TRIGGER", g_trigger_table);
}

String capability_export_workflow_registry()
{
    return export_table("WORKFLOW", g_workflow_table);
}
