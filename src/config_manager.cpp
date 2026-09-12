#include "config_manager.h"

#include "json_storage.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

// PSRAM 分配接口（heap_caps_malloc / heap_caps_free）
#include <esp_heap_caps.h>

// Restart 能力已收回 SystemCommand（V2 迁移）。
//
// 本模块不再执行 ESP.restart()，也不再维护自己的重启安全窗口。
// 需要重启时统一调用 system_command_request_restart()，
// 由 SystemCommand 等待 Critical Operation 归零后进入 10s 安全窗口并执行。
//
// 本模块仍负责自己的业务逻辑：改配置后的 5 分钟自动重启倒计时
// （SystemCommand 不知道也不关心这个计时，见需求文档 §8）。
#include "system_command.h"

// =====================================================
// 内部常量
// =====================================================

// 模块数量（与 kModuleNames 一致）
#define CONFIG_MODULE_COUNT     8

// 路径缓冲区大小
// 最长路径形如 /config/mi_thermo.json.bak
#define CONFIG_PATH_MAX         64

// 单条日志上限，超长截断
#define CONFIG_LOG_BUF_SIZE     192

// 启动标记文件内容（内容无意义，文件存在即代表上次启动成功）
#define CONFIG_BOOT_FLAG_BODY   "1"

// int 可无损表示的数值范围
//
// 超出该范围的整数必须被拒绝，而不是降级存成 float ——
// 降级会静默丢失精度，还会让字段类型从整数变成浮点。
#define CONFIG_INT_VALUE_MAX    2147483647.0
#define CONFIG_INT_VALUE_MIN    (-2147483648.0)

// 大 value（整模块 JSON）上限，字节
//
// 超过 CONFIG_VALUE_STRING_MAX 的 value 会暂存在 PSRAM，
// 任务执行完立即释放。上限用于拒绝异常大的输入，
// 避免单次命令把内存吃光。
#define CONFIG_VALUE_BIG_MAX    8192

// PSRAM 分配标志
#define CONFIG_MEM_SPIRAM       MALLOC_CAP_SPIRAM

// =====================================================
// 模块名表
// =====================================================
static const char *const kModuleNames[CONFIG_MODULE_COUNT] = {
    CONFIG_MODULE_WIFI,
    CONFIG_MODULE_OLED,
    CONFIG_MODULE_VALVE,
    CONFIG_MODULE_TIME,
    CONFIG_MODULE_WEIGHT,
    CONFIG_MODULE_MQTT,
    CONFIG_MODULE_MITHERMO,
    CONFIG_MODULE_RTC
};

// =====================================================
// 内部状态
// =====================================================

// 模块缓存表
//
// 采用缓存结构而非单一大文档：未来接入 workflow / 大 JSON /
// 用户自定义数据后，可以按需释放，不让所有配置同时常驻 RAM。
static ConfigModule s_modules[CONFIG_MODULE_COUNT];

// 文件系统是否可用（JsonStorage 初始化结果）
static bool s_ready = false;

// 模块表是否已初始化
static bool s_table_ready = false;

// 修改配置后的自动重启倒计时起点（millis 快照）。0 表示无待重启。
//
// 注意: 这只是本模块自己的 5 分钟业务倒计时（需求文档 §8）。
// 重启命令的"安全延迟窗口"已移交给 SystemCommand，本模块不再维护。
static unsigned long s_restart_since_ms = 0;

// 是否存在"已修改、尚未落盘"的配置事务（需求文档 §3.2 / §4）。
//
// 语义:
//   配置修改开始        -> 首次 Acquire Critical Operation（连续修改不重复 Acquire）
//   配置 Save 成功落盘  -> Release + Request Restart（见 pending_save_finalize）
//   Save 失败           -> 不得 Release、不得 Request Restart
//
// 该标志用于保证"一个 Pending Save 事务对应一个 Critical Operation"，
// 避免连续 config_set 造成 Critical Operation Count 永久增加。
static bool s_pending_save = false;

// 最近一次提交状态
//
// 一次完整提交 = Data 成功 + Version 成功 + 收尾成功。
// 任一步失败都必须回滚，并把状态置为 CONFIG_COMMIT_FAILED。
static ConfigCommitState s_commit_state = CONFIG_COMMIT_NONE;

// 日志回调
static ConfigLogCallback s_log_cb = nullptr;

// 完成回调（由 CommandManager 注册）
static ConfigCommandCompletionCallback s_completion_cb = nullptr;

// =====================================================
// 配置命令队列
//
// 固定容量数组，编译期确定大小，运行期不分配、不释放、无动态链表。
// 每个槽位自带定长缓冲区，任务参数（含 value 的字符串内容）
// 在入队时完整复制进来 —— 任务不持有调用方 JsonDocument 的任何引用，
// 因此 CommandManager 的临时文档析构之后任务依然有效。
// =====================================================

// value 的类型
enum ConfigValueKind
{
    CONFIG_VALUE_NONE = 0,
    CONFIG_VALUE_INT,
    CONFIG_VALUE_BOOL,
    CONFIG_VALUE_FLOAT,
    CONFIG_VALUE_STRING,
    CONFIG_VALUE_OBJECT      // 对象 JSON（批量写模块），存放于 big 缓冲
};

// value 存储槽
//
// 小标量直接放联合体，无动态分配。
// 超过定长缓冲的对象 JSON 放 big（PSRAM 中分配），
// 由 value_slot_free() 释放 —— 用后必须立即释放，不得长期占用。
struct ConfigValueSlot
{
    ConfigValueKind kind;
    char  *big;              // PSRAM 缓冲；nullptr 表示未使用
    size_t big_len;
    union
    {
        int   i;
        bool  b;
        float f;
        char  s[CONFIG_VALUE_STRING_MAX];
    } v;
};

// 释放大 value 占用的 PSRAM
//
// 必须对每一个填充过的槽位调用，包括:
//   任务执行完、入队失败、槽位清理
// 释放后指针置空，可安全重复调用。
static void value_slot_free(ConfigValueSlot &slot)
{
    if (slot.big != nullptr)
    {
        heap_caps_free(slot.big);
        slot.big = nullptr;
        slot.big_len = 0;
    }
}

// 取槽位中的文本（大 value 与小缓冲统一视图）
//
// big != nullptr 时用 PSRAM 缓冲，否则用定长缓冲。
static const char *value_slot_text(const ConfigValueSlot &slot)
{
    return (slot.big != nullptr) ? slot.big : slot.v.s;
}

// 队列任务
struct ConfigCommandTask
{
    bool active;                              // 槽位占用
    ConfigCommandType type;                   // 命令类型
    char cmd_id[CONFIG_CMD_ID_MAX];
    char command[CONFIG_CMD_NAME_MAX];
    char object[CONFIG_CMD_NAME_MAX];
    char source[CONFIG_CMD_SOURCE_MAX];
    char module[CONFIG_MODULE_NAME_MAX];
    char key[CONFIG_KEY_NAME_MAX];
    ConfigValueSlot value;                    // 深拷贝后的 value
    bool cancel;                              // 仅 CONFIG_CMD_RESTART 使用
    bool keys_only;                           // 查询只返回键名，不返回值
    bool check_version;                       // 是否校验版本号（乐观锁）
    uint8_t expect_version;                   // 期望的版本号
};

static ConfigCommandTask s_cmd_queue[CONFIG_CMD_QUEUE_SIZE];
static uint8_t s_cmd_head = 0;    // 下一次从哪个槽位开始扫描
static uint8_t s_cmd_count = 0;   // 当前排队数量

// 前向声明（实现位于文件后段）
static void config_cmd_process_one();
static void restart_timer_check();
static void pending_save_finalize();

// =====================================================
// 内部工具
// =====================================================
static void cfg_log(const char *level, const char *fmt, ...)
{
    char buf[CONFIG_LOG_BUF_SIZE];

    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    // 未注册回调时兜底输出到串口。
    //
    // 否则"启动恢复 / 提交回滚"这类关键事件会完全静默，
    // 出现配置莫名回退时无从排查。
    if (s_log_cb == nullptr)
    {
        Serial.printf("[CFG][%s] %s\n", level, buf);
        return;
    }

    s_log_cb(level, buf);
}

// 模块表初始化
static void module_table_init()
{
    if (s_table_ready)
    {
        return;
    }

    for (int i = 0; i < CONFIG_MODULE_COUNT; i++)
    {
        s_modules[i].name = kModuleNames[i];
        s_modules[i].loaded = false;
        s_modules[i].dirty = false;
        s_modules[i].version = 0;
    }

    s_table_ready = true;
}

// 按模块名查找缓存
static ConfigModule *find_module(const char *module)
{
    if (module == nullptr || module[0] == '\0')
    {
        return nullptr;
    }

    for (int i = 0; i < CONFIG_MODULE_COUNT; i++)
    {
        if (strcmp(s_modules[i].name, module) == 0)
        {
            return &s_modules[i];
        }
    }

    return nullptr;
}

// 只读访问模块数据
//
// 模块不存在或未加载时返回一个空文档（const），
// 使调用方的 `doc[key] | 默认值` 依然成立，不会引入空指针风险。
static const JsonDocument &module_ro(const char *module)
{
    static const JsonDocument s_empty;

    ConfigModule *m = find_module(module);

    if (m == nullptr || !m->loaded)
    {
        return s_empty;
    }

    return m->data;
}

// 合成配置文件路径
//   backup = false → /config/<module>.json
//   backup = true  → /config/<module>.json.bak
static bool module_path(
    const char *module,
    bool backup,
    char *out,
    size_t out_size
)
{
    if (module == nullptr || out == nullptr || out_size == 0)
    {
        return false;
    }

    int n;

    if (backup)
    {
        n = snprintf(
            out,
            out_size,
            "%s/%s.json%s",
            CONFIG_DIR,
            module,
            CONFIG_BAK_SUFFIX
        );
    }
    else
    {
        n = snprintf(out, out_size, "%s/%s.json", CONFIG_DIR, module);
    }

    return (n > 0 && (size_t)n < out_size);
}

// 合成 factory 副本路径: /factory/<module>.json
//
// factory 是永不滚动的初始基线，由云端显式下发创建，
// 不随 uploadfs 烧录。详见 CONFIG_FACTORY_DIR 说明。
static bool module_factory_path(
    const char *module,
    char *out,
    size_t out_size
)
{
    if (module == nullptr || out == nullptr || out_size == 0)
    {
        return false;
    }

    int n = snprintf(
        out,
        out_size,
        "%s/%s.json",
        CONFIG_FACTORY_DIR,
        module
    );

    return (n > 0 && (size_t)n < out_size);
}

// 确保 factory 目录存在
//
// /factory 不随 uploadfs 烧录（data/factory 目录本来就不存在），
// 而 LittleFS 也不会在写文件时自动创建父目录，因此首次写入前必须
// 显式创建，否则 json_storage_write_atomic() 会因为无法创建 .tmp
// 文件而失败。
//
// 目录创建逻辑已收进 JsonStorage（json_storage_mkdir，幂等），
// 这里只做转发与日志，业务层不再直接依赖 LittleFS。
static bool factory_dir_ensure()
{
    if (json_storage_mkdir(CONFIG_FACTORY_DIR))
    {
        return true;
    }

    cfg_log("E", "cannot create factory dir: %s", CONFIG_FACTORY_DIR);
    return false;
}

// =====================================================
// 单模块加载
//
// 优先读取 Active，失败则回退 Backup。
// 解析失败时必须清空文档，避免留下半解析状态。
// =====================================================
static bool load_module(ConfigModule *m, bool allow_backup)
{
    char path[CONFIG_PATH_MAX];
    String content;

    // ---- 尝试 Active ----
    if (module_path(m->name, false, path, sizeof(path)))
    {
        if (json_storage_read(path, content))
        {
            if (deserializeJson(m->data, content) == DeserializationError::Ok)
            {
                m->loaded = true;
                return true;
            }
            cfg_log("W", "parse failed, module=%s", m->name);
            m->data.clear();
        }
    }

    // ---- 回退 Backup ----
    if (allow_backup && module_path(m->name, true, path, sizeof(path)))
    {
        if (json_storage_read(path, content))
        {
            if (deserializeJson(m->data, content) == DeserializationError::Ok)
            {
                m->loaded = true;
                cfg_log("W", "loaded from backup, module=%s", m->name);
                return true;
            }
            m->data.clear();
        }
    }

    m->loaded = false;
    cfg_log("E", "load failed, module=%s", m->name);

    return false;
}

// =====================================================
// 单模块保存
//
// 顺序（关键是先保住旧数据，再替换新数据）:
//   1. 旧 Active 轮转为 Backup
//   2. 原子写入新 Active（tmp → 完整写入 → 校验 → rename）
//
// 这样任何时刻都至少存在一份完整数据：
//   轮转成功但写入失败 → Active 缺失、Backup 完好，
//   下次加载自动回退 Backup。
// =====================================================
static bool save_module(ConfigModule *m)
{
    char path[CONFIG_PATH_MAX];
    char bak[CONFIG_PATH_MAX];

    if (!module_path(m->name, false, path, sizeof(path)))
    {
        return false;
    }

    if (!module_path(m->name, true, bak, sizeof(bak)))
    {
        return false;
    }

    // 根必须是对象。
    //
    // 单靠序列化长度检查拦不住空文档:
    //   null 文档会被序列化成 "null"（4 字节），长度非 0 因而能通过检查，
    //   随后被原子写入 Active —— 等于把整个模块的配置清空，
    //   而此时旧 Active 已经被轮转成 Backup，已经无法回头。
    if (!m->data.is<JsonObject>())
    {
        cfg_log("E", "data root is not an object, module=%s", m->name);
        return false;
    }

    String content;

    if (serializeJson(m->data, content) == 0)
    {
        cfg_log("E", "serialize failed, module=%s", m->name);
        return false;
    }

    // 1) 旧 Active → Backup
    if (json_storage_exists(path))
    {
        if (!json_storage_rename(path, bak))
        {
            cfg_log("E", "rotate to backup failed, module=%s", m->name);
            return false;
        }
    }

    // 2) 写入新 Active
    if (!json_storage_write_atomic(path, content))
    {
        cfg_log("E", "write active failed, module=%s", m->name);
        return false;
    }

    // 3) 版本递增（版本号落到 version.json 由 config_save 统一处理）
    m->version++;
    m->dirty = false;

    return true;
}

// =====================================================
// Backup 恢复：把 Backup 内容重新写入 Active
//
// 重要: 不使用 rename。
//
//   rename 会消耗 Backup —— 恢复之后只剩一份数据，
//   若再发生一次失败就再也无处可退。
//
//   改为"读取 Backup 内容 → 原子写入 Active"，
//   Backup 始终原样保留，持续充当 Last Known Good Config。
//
// 恢复后状态:
//   active = backup 的内容
//   backup = 保持不变
// =====================================================
static bool recover_module(ConfigModule *m)
{
    char path[CONFIG_PATH_MAX];
    char bak[CONFIG_PATH_MAX];

    if (!module_path(m->name, false, path, sizeof(path)))
    {
        return false;
    }

    if (!module_path(m->name, true, bak, sizeof(bak)))
    {
        return false;
    }

    if (!json_storage_exists(bak))
    {
        return false;
    }

    // 1) 读取 Backup 内容
    String content;

    if (!json_storage_read(bak, content))
    {
        cfg_log("E", "recover: backup read failed, module=%s", m->name);
        return false;
    }

    // 2) 校验内容合法
    //    避免把一份损坏的 Backup 写成新的 Active
    {
        JsonDocument probe;

        if (deserializeJson(probe, content) != DeserializationError::Ok)
        {
            cfg_log(
                "E",
                "recover: backup content invalid, module=%s",
                m->name
            );
            return false;
        }
    }

    // 3) 原子写入 Active（Backup 原样保留）
    if (!json_storage_write_atomic(path, content))
    {
        cfg_log("E", "recover: write active failed, module=%s", m->name);
        return false;
    }

    cfg_log("W", "recovered from backup, module=%s", m->name);

    return true;
}

// =====================================================
// 版本文件读写
// =====================================================

// 前向声明：bootstrap_version_file() 依赖它，而它定义在下方
static bool save_version_file();

// 版本文件缺失 / 损坏时的自愈
//
// 背景（实测 bug）：
//   data/config/ 只烧录了 8 个模块 JSON，没有 version.json。
//   于是每次启动 load_version_file() 都失败 → 打 [CFG][E] version reload
//   failed，且所有模块 version 停在 0（"未知"）。
//
//   更糟的是它不会自愈：save_version_file() 会跳过 version==0 的模块
//   （防止未知值覆盖文件中已记录的版本），所以只要没有模块被真正修改，
//   version.json 永远不会被创建 —— 错误永久存在。
//
// 修复：缺失 / 损坏时，为【已成功加载】的模块建立版本基线并立即落盘。
//   - 只对 loaded 的模块置 1：未加载的模块 version 保持 0，
//     不污染"未知"语义，后续真正加载后会自然进入版本管理。
//   - 返回 false 表示连写入都失败，调用方据此打 E 级日志。
static bool bootstrap_version_file()
{
    bool any = false;

    for (int i = 0; i < CONFIG_MODULE_COUNT; i++)
    {
        if (!s_modules[i].loaded)
        {
            continue;
        }
        if (s_modules[i].version == 0)
        {
            s_modules[i].version = 1;
        }
        any = true;
    }

    if (!any)
    {
        // 没有任何模块加载成功，写下去也只是空文件，等下次启动再试
        return false;
    }

    return save_version_file();
}

static bool load_version_file()
{
    String content;

    if (!json_storage_read(CONFIG_VERSION_FILE, content))
    {
        return false;
    }

    JsonDocument doc;

    if (deserializeJson(doc, content) != DeserializationError::Ok)
    {
        return false;
    }

    for (int i = 0; i < CONFIG_MODULE_COUNT; i++)
    {
        JsonVariantConst v = doc[s_modules[i].name];

        if (!v.isNull())
        {
            s_modules[i].version = v.as<uint8_t>();
        }
    }

    return true;
}

// 版本文件最后更新：
//   配置本身此时已经生效，版本写入失败不回滚、不阻塞本次保存，
//   不一致留待下次启动时通过文件扫描与启动标记判定。
static bool save_version_file()
{
    JsonDocument doc;

    // 以文件现有内容为基底，而不是每次从空文档重建。
    String existing;

    if (json_storage_read(CONFIG_VERSION_FILE, existing))
    {
        if (deserializeJson(doc, existing) != DeserializationError::Ok)
        {
            // 文件已损坏，丢弃后重新构建
            doc.clear();
        }
    }

    for (int i = 0; i < CONFIG_MODULE_COUNT; i++)
    {
        // version == 0 表示"版本未知"（例如 version.json 读取失败）。
        //
        // 不能用未知的 0 覆盖文件里已记录的版本 ——
        // 否则一次读取失败就会抹掉所有模块的版本历史。
        //
        // 本次保存过的模块在 save_module 中已经 version++，
        // 必然 >= 1，因此不会被这里跳过。
        if (s_modules[i].version == 0)
        {
            continue;
        }

        doc[s_modules[i].name] = s_modules[i].version;
    }

    String content;

    if (serializeJson(doc, content) == 0)
    {
        return false;
    }

    return json_storage_write_atomic(CONFIG_VERSION_FILE, content);
}

// =====================================================
// 提交失败后的安全恢复
//
// 触发场景:
//   Config Data 写入成功，但 Version 写入失败。
//
// 此时 Data 与 Version 已经失去一致性，绝不能因为
// "数据写入成功"就认为本次提交成功。
//
// 处理策略:
//   1. 文件层: Backup → Active，回到上一次可信配置
//   2. RAM 层: 从 Active 重新加载，丢弃本次未提交的修改
//   3. 版本  : 回到 version.json 中记录的可信值
//
// 目标:
//   让 Config Data / Version / RAM 三者重新回到上一次可信状态，
//   即 Backup 所代表的 Last Known Good Config。
//
// 注意:
//   Backup 保存的是"修改前的 Active"，也就是 Last Known Good；
//   本次尚未证明成功的新配置不会被保留为 Backup。
// =====================================================
static bool commit_recover_from_backup()
{
    bool all_ok = true;

    // 1) 文件层：把 Backup 内容写回 Active（不消耗 Backup）
    for (int i = 0; i < CONFIG_MODULE_COUNT; i++)
    {
        char bak[CONFIG_PATH_MAX];

        if (!module_path(s_modules[i].name, true, bak, sizeof(bak)))
        {
            continue;
        }

        // 没有 Backup 的模块无从恢复，属正常情况，跳过
        if (!json_storage_exists(bak))
        {
            continue;
        }

        // 恢复失败必须记录：不能静默忽略
        if (!recover_module(&s_modules[i]))
        {
            cfg_log(
                "E",
                "rollback failed, module=%s",
                s_modules[i].name
            );
            all_ok = false;
        }
    }

    // 2) RAM 层：从 Active 重新加载，丢弃本次未提交的修改
    for (int i = 0; i < CONFIG_MODULE_COUNT; i++)
    {
        s_modules[i].data.clear();
        s_modules[i].loaded = false;
        s_modules[i].dirty = false;

        // 回滚后重新加载失败必须记录:
        // 否则该模块会静默停留在"未加载"状态，全部走默认值。
        //
        // allow_backup 必须为 true —— 回滚恰恰是最需要回退 Backup 的场景:
        // 若 recover_module 未能写入 Active（Active 缺失而 Backup 完好），
        // 只允许读 Active 会让该模块直接降级成默认值。
        if (!load_module(&s_modules[i], true))
        {
            cfg_log(
                "E",
                "rollback reload failed, module=%s",
                s_modules[i].name
            );
        }
    }

    // 4) 版本：回到 version.json 中记录的可信值
    //
    //    保证回滚之后:
    //      active      = backup 的内容
    //      backup      = 保持不变
    //      RAM version = 文件里的版本值
    //    三者重新一致。
    // 此路径下模块已在上面重新加载完毕（loaded 已置位），
    // 因此可以直接自愈：为已加载模块建立版本基线。
    if (!load_version_file())
    {
        if (!bootstrap_version_file())
        {
            cfg_log("E", "version reload failed");
        }
        else
        {
            cfg_log("W", "version file rebuilt after rollback");
        }
    }

    // 与文件状态保持一致:
    //   回滚成功 → FAILED（状态已一致，下次启动无需恢复）
    //   回滚失败 → PENDING（仍不一致，下次启动继续恢复）
    //
    // 不能无条件置 FAILED，否则回滚失败时上层查询会被误导。
    s_commit_state = all_ok ? CONFIG_COMMIT_FAILED : CONFIG_COMMIT_PENDING;

    cfg_log("W", "commit failed, recovered to last known good config");

    return all_ok;
}

// 把提交状态写入标记文件
//
// 文件内存的是状态字符串: PENDING / DONE / FAILED。
//
// 不用"存在 / 不存在"表达状态，原因:
//   若用"存在 = 异常"，那么最后一步（写 DONE）失败时无法表达 ——
//   无论写入成功与否，最终都只能呈现成同一种状态，
//   无法区分"提交成功但标记没写成"与"提交真的失败了"。
static bool commit_state_write(ConfigCommitState state)
{
    const char *body = "UNKNOWN";

    switch (state)
    {
        case CONFIG_COMMIT_PENDING: body = "PENDING"; break;
        case CONFIG_COMMIT_DONE:    body = "DONE";    break;
        case CONFIG_COMMIT_FAILED:  body = "FAILED";  break;
        default:                    body = "NONE";    break;
    }

    // 必须先写文件，成功之后再更新 RAM 状态。
    //
    // 若先改 RAM：一旦写文件失败，就会出现
    //   RAM 显示 DONE、磁盘却仍是 PENDING
    // 的矛盾状态 —— 对外查询给出与磁盘相反的答案，
    // 下次启动还会回滚，上层却以为提交已经成功。
    if (!json_storage_write_atomic(
            CONFIG_COMMIT_FLAG_FILE,
            String(body)))
    {
        cfg_log("E", "commit state write failed: %s", body);
        return false;
    }

    s_commit_state = state;

    return true;
}

// 读取提交状态
//
//   文件不存在   → CONFIG_COMMIT_NONE（尚无提交记录）
//   内容无法识别 → 保守按 PENDING 处理，交由启动恢复兜底
static ConfigCommitState commit_state_read()
{
    String content;

    if (!json_storage_read(CONFIG_COMMIT_FLAG_FILE, content))
    {
        return CONFIG_COMMIT_NONE;
    }

    content.trim();

    if (content == "PENDING")
    {
        return CONFIG_COMMIT_PENDING;
    }

    if (content == "DONE")
    {
        return CONFIG_COMMIT_DONE;
    }

    if (content == "FAILED")
    {
        return CONFIG_COMMIT_FAILED;
    }

    cfg_log("E", "commit flag content unknown, treat as PENDING");

    return CONFIG_COMMIT_PENDING;
}

// 标记提交开始
static bool commit_mark_pending()
{
    return commit_state_write(CONFIG_COMMIT_PENDING);
}

// 标记提交完整完成
static bool commit_mark_done()
{
    return commit_state_write(CONFIG_COMMIT_DONE);
}

// 标记提交失败（且已回滚到 Last Known Good）
static bool commit_mark_failed()
{
    return commit_state_write(CONFIG_COMMIT_FAILED);
}

// 提交失败后的统一处理：回滚 + 写入最终状态
//
//   回滚成功 → 写 FAILED:
//       本次提交确实失败，但已回到 Last Known Good，
//       状态一致，下次启动无需再恢复。
//
//   回滚失败 → 状态保持 PENDING:
//       系统仍处于不一致，下次启动继续尝试恢复。
//
// 始终返回 false: 提交失败就是失败，
// 绝不因为"回滚成功"就把本次提交改判为成功。
static bool commit_fail_and_rollback(const char *stage)
{
    cfg_log("E", "commit failed at %s, rolling back", stage);

    if (commit_recover_from_backup())
    {
        // 回滚成功 ≠ 提交成功，记为 FAILED。
        // 必须检查返回值：写入失败时不能静默放过。
        if (!commit_mark_failed())
        {
            cfg_log("E", "rollback ok, but failed to mark FAILED");
        }
    }
    else
    {
        // 回滚失败：保留 PENDING。
        // 严禁在此改写成"已处理完"，否则下次启动会误判状态正常。
        cfg_log("E", "rollback incomplete, commit state kept as PENDING");
    }

    return false;
}

// =====================================================
// 重启倒计时
// =====================================================
// 启动 / 刷新重启倒计时
//
// 每次"配置被成功修改"都要重新开始完整的倒计时窗口。
// 用户可能连续修改多个字段，窗口应当从最后一次修改起算，
// 而不是从第一次修改起算。
//
// 只有真正修改了 RAM 的操作才调用本函数；
// 查询、保存本身不刷新计时器。
static void restart_timer_start()
{
    s_restart_since_ms = millis();

    cfg_log(
        "I",
        "restart timer refreshed, timeout=%lu ms",
        (unsigned long)CONFIG_RESTART_TIMEOUT_MS
    );
}

// 配置事务收尾：Save 已确认落盘 -> Release + Request Restart
//
// 只在 config_save() 确认 Flash 写入成功【之后】调用（需求文档 §四）:
//   1) Release Critical Operation —— 配置已安全落盘，不再需要阻塞重启
//   2) 请求重启 —— 交由 SystemCommand 状态机执行，不直接 ESP.restart()
//
// 失败路径严禁调用本函数（需求文档 §九）:
//   保持 Critical Operation 占用、保持 Pending Save 事务，
//   等待下一次成功 Save 再收尾。
//
// 无待收尾事务时是安全的幂等空操作：
//   例如业务模块仅落盘"不需要重启"的修改时，s_pending_save 为 false。
static void pending_save_finalize()
{
    if (!s_pending_save)
    {
        return;
    }

    // 1) Release: acquire/release 严格配对，见 set_begin() 中的首次 Acquire。
    //    只有 Save 成功才走到这里，count 必然 >= 1，不会触发下溢保护。
    system_command_critical_operation_release();
    s_pending_save = false;

    cfg_log(
        "I",
        "config saved, critical op released, requesting restart"
    );

    // 2) 请求重启。request_restart() 幂等（需求文档 §13）:
    //    已在 REQUESTED / PENDING 时重复调用返回 true，不会重置倒计时。
    if (!system_command_request_restart())
    {
        cfg_log("E", "restart request rejected after save");
    }
}

// 检查倒计时是否到期（由 config_task 调用）
static void restart_timer_check()
{
    if (s_restart_since_ms == 0)
    {
        return;
    }

    // 用无符号差值计算，天然兼容 millis() 溢出回绕
    unsigned long elapsed = millis() - s_restart_since_ms;

    if (elapsed >= CONFIG_RESTART_TIMEOUT_MS)
    {
        cfg_log("W", "restart timeout reached, saving before reboot");

        // 重启前必须先落盘。
        //
        // 配置修改默认只改 RAM（便于连续改多个字段只写一次 Flash），
        // 若这里不保存，倒计时一到重启就会丢掉全部未保存的修改。
        if (!config_save())
        {
            // 需求文档 §九: Save 落盘失败 -> 不得 Release、不得 Request Restart。
            //
            // 保持 Critical Operation 占用与 Pending Save 事务，
            // 重新开始完整的 5 分钟倒计时，等待下一次自动重试。
            // 不在此处请求重启，避免"配置尚未确认落盘"时进入重启流程。
            cfg_log(
                "E",
                "pre-restart save failed; critical op kept, "
                "restart timer re-armed"
            );
            s_restart_since_ms = millis();
            return;
        }

        // 落盘成功 -> Release + Request Restart（需求文档 §四 / §七）。
        //
        // 本模块不执行重启。"怎么安全地重启"由 SystemCommand 统一负责:
        //   等 Critical Operation 归零 -> 10s 安全窗口 -> ESP.restart()
        pending_save_finalize();

        s_restart_since_ms = 0;
    }
}

// 注: 原 restart_safe_delay_check()（重启命令的 10 秒安全延迟窗口）
// 已随 V2 迁移整体移除，该职责由 SystemCommand 的状态机承担。
// 本模块不再维护 s_restart_request_ms，也不再直接调用 ESP.restart()。

// =====================================================
// 日志回调
// =====================================================
void config_set_log_callback(ConfigLogCallback callback)
{
    s_log_cb = callback;
}

// =====================================================
// 生命周期
// =====================================================
bool config_init()
{
    module_table_init();

    // 1) 底层存储
    if (!json_storage_init())
    {
        s_ready = false;
        cfg_log("E", "JsonStorage unavailable");
        return false;
    }

    s_ready = true;

    // factory 目录不随 uploadfs 烧录，开机时确保存在。
    //
    // 失败不阻塞启动: 只影响备份/恢复功能，且在每次执行
    // config_backup 时还会再尝试创建一次。
    factory_dir_ensure();

    // 2) 读取两个保护状态
    //
    //    Boot Flag:
    //      每次 config_init() 中清除，只有完整走完 setup 并调用
    //      config_boot_validate() 才会重新写入。
    //      标记缺失 = 上一次启动未成功（panic / 反复重启）。
    //
    //    Commit Flag:
    //      文件内是提交状态字符串 PENDING / DONE / FAILED。
    //      PENDING = 上次提交未走完全流程，需要恢复。
    //
    //    两者解决不同问题，任一异常都要回到 Last Known Good Config。
    bool last_boot_ok = json_storage_exists(CONFIG_BOOT_FLAG_FILE);

    s_commit_state = commit_state_read();

    // 清除启动标记：本次启动必须重新证明自己能走完 setup。
    //
    // 必须检查返回值。若清除失败、而本次启动又发生崩溃，
    // 下次启动会读到残留标记并误判"上次启动成功"，从而跳过恢复。
    if (!json_storage_remove(CONFIG_BOOT_FLAG_FILE))
    {
        cfg_log("E", "cannot clear boot flag, recovery may misjudge");
    }

    // 3) 版本信息
    //
    //    版本文件缺失 / 损坏时【不在此处】重建 —— 此时模块尚未加载
    //    （步骤 5），无法判断哪些模块真正可用。先记下状态，
    //    等模块加载完成后再统一自愈（见步骤 6）。
    bool version_ok = load_version_file();

    if (!version_ok)
    {
        // 首次烧录（data/config 未带 version.json）或文件损坏都属预期场景，
        // 用 W 而非 E —— 它不是故障，步骤 6 会自动补上。
        cfg_log("W", "version file missing or corrupt, will rebuild");
    }

    // 4) 恢复判定
    //      - 上次启动未完成（初始化阶段崩溃）
    //      - 上次提交停在 PENDING（Data 写入后中断）
    bool need_recover =
        (!last_boot_ok) || (s_commit_state == CONFIG_COMMIT_PENDING);

    if (need_recover)
    {
        cfg_log(
            "W",
            "recovering (boot_ok=%d, commit_state=%d)",
            last_boot_ok ? 1 : 0,
            (int)s_commit_state
        );

        bool recover_ok = true;

        for (int i = 0; i < CONFIG_MODULE_COUNT; i++)
        {
            char bak[CONFIG_PATH_MAX];

            if (!module_path(s_modules[i].name, true, bak, sizeof(bak)))
            {
                continue;
            }

            // 没有 Backup 的模块无从恢复，属正常情况，不计入失败
            if (!json_storage_exists(bak))
            {
                continue;
            }

            // 恢复失败必须记录，不得静默忽略。
            //
            // recover_module 不消耗 Backup，
            // 因此即便恢复失败，系统也仍保有最后的退路。
            if (!recover_module(&s_modules[i]))
            {
                cfg_log(
                    "E",
                    "recover failed, module=%s",
                    s_modules[i].name
                );
                recover_ok = false;
            }
        }

        // 提交状态更新（仅当上次停在 PENDING 时）
        //
        //   恢复成功 → 写 FAILED:
        //       上次提交确实失败，但已回到一致状态，下次启动无需再恢复。
        //
        //   恢复失败 → 保持 PENDING:
        //       状态仍不一致，下次启动继续尝试恢复。
        //
        //   严禁在恢复失败时改写或清除标记 ——
        //   那会让下次启动误以为状态已经处理完毕。
        if (s_commit_state == CONFIG_COMMIT_PENDING)
        {
            if (recover_ok)
            {
                if (!commit_mark_failed())
                {
                    cfg_log("E", "cannot update commit state to FAILED");
                }
            }
            else
            {
                cfg_log(
                    "E",
                    "recover incomplete, commit state kept as PENDING"
                );
            }
        }
    }

    // 5) 逐模块加载
    int loaded = 0;

    for (int i = 0; i < CONFIG_MODULE_COUNT; i++)
    {
        if (load_module(&s_modules[i], true))
        {
            loaded++;
        }
        else
        {
            // 模块加载失败 = 该模块所有配置走默认值。
            // 必须逐模块留下痕迹，否则设备行为异常却毫无提示。
            cfg_log(
                "W",
                "module not loaded, defaults will be used: %s",
                s_modules[i].name
            );
        }
    }

    cfg_log("I", "init done, loaded %d/%d modules", loaded, CONFIG_MODULE_COUNT);

    // 6) 版本文件自愈（必须在模块加载之后）
    //
    //    缺失 / 损坏时为已加载模块建立版本基线并落盘。
    //    不做的后果：save_version_file() 会跳过 version==0 的模块，
    //    于是 version.json 永远生成不了，每次启动都报"版本读取失败"，
    //    expect_version 乐观锁也因版本恒为 0 而失效。
    if (!version_ok)
    {
        if (bootstrap_version_file())
        {
            cfg_log("I", "version file created (baseline)");
        }
        else
        {
            cfg_log("E", "version rebuild failed");
        }
    }

    return loaded > 0;
}

bool config_save()
{
    if (!s_ready)
    {
        cfg_log("E", "save: storage unavailable");
        return false;
    }

    // 无变化不写 Flash，属幂等成功
    bool any_dirty = false;

    for (int i = 0; i < CONFIG_MODULE_COUNT; i++)
    {
        if (s_modules[i].dirty)
        {
            any_dirty = true;
            break;
        }
    }

    if (!any_dirty)
    {
        // 幂等成功（无变化不写 Flash）。
        //
        // 若仍存在待收尾事务（例如上次 Save 失败已回滚、dirty 被清空），
        // 在此收尾，避免 Critical Operation 被永久占用、阻塞一切后续重启。
        // 无事务时为空操作（需求文档 §四 的 Release 只针对真实事务）。
        pending_save_finalize();
        return true;
    }

    // ---- 1) 标记提交开始 ----
    //
    //     此后任何一步失败或掉电，下次启动都能据此判定
    //     "上次提交未完成"并从 Backup 恢复。
    if (!commit_mark_pending())
    {
        cfg_log("E", "save: cannot mark commit pending, abort");
        s_commit_state = CONFIG_COMMIT_FAILED;
        return false;
    }

    // ---- 2) Config Data 写入 ----
    //
    //     多模块一致性: 任一模块失败即整批失败并回滚，
    //     不允许出现"部分模块新、部分模块旧"的混合状态。
    bool all_ok = true;

    for (int i = 0; i < CONFIG_MODULE_COUNT; i++)
    {
        if (!s_modules[i].dirty)
        {
            continue;
        }

        if (!save_module(&s_modules[i]))
        {
            all_ok = false;
        }
    }

    if (!all_ok)
    {
        return commit_fail_and_rollback("data");
    }

    // ---- 3) Version 写入 ----
    //
    //     只有 Data 成功 + Version 成功，本次提交才算完成。
    if (!save_version_file())
    {
        return commit_fail_and_rollback("version");
    }

    // ---- 4) 标记提交完成 ----
    if (!commit_mark_done())
    {
        return false;
    }

    // ---- 5) 事务收尾 ----
    //
    // 只有走到这里才确认 Data + Version + 提交状态【全部】落盘成功。
    // 此时才允许 Release Critical Operation 并请求重启（需求文档 §四 / §七）。
    // 前面任何一步失败都会走 commit_fail_and_rollback() 提前返回，
    // 不会执行到本行 —— 失败路径保持占用、不请求重启（需求文档 §九）。
    pending_save_finalize();

    return true;
}

// 查询最近一次提交状态
//
// CONFIG_COMMIT_DONE    最近一次提交完整成功
// CONFIG_COMMIT_PENDING 提交进行中（尚未确认完成）
// CONFIG_COMMIT_FAILED  提交失败并已回滚
// CONFIG_COMMIT_NONE    尚未发生任何提交
ConfigCommitState config_commit_state()
{
    return s_commit_state;
}

void config_task()
{
    // 1) 改配置后的 5 分钟自动重启倒计时。
    //
    //    注意: 这里只判断"是否该请求重启"，真正的重启由
    //    SystemCommand 的状态机执行（main.cpp 的 loop 中
    //    system_command_task() 已在本函数之前调用）。
    restart_timer_check();

    // 2) 每次 loop 最多处理一个配置命令。
    //    即使队列积压，也不会在一个 loop 内连续执行多次文件 IO。
    config_cmd_process_one();
}

bool config_boot_validate()
{
    if (!s_ready)
    {
        return false;
    }

    if (!json_storage_write_atomic(
            CONFIG_BOOT_FLAG_FILE,
            String(CONFIG_BOOT_FLAG_BODY)
        ))
    {
        cfg_log("E", "boot flag write failed");
        return false;
    }

    cfg_log("I", "boot validated");

    return true;
}

bool config_reload()
{
    if (!s_ready)
    {
        return false;
    }

    for (int i = 0; i < CONFIG_MODULE_COUNT; i++)
    {
        s_modules[i].data.clear();
        s_modules[i].loaded = false;
        s_modules[i].dirty = false;
    }

    // 与 config_init() 同样处理：文件缺失 / 损坏时先记状态，
    // 等本函数末尾模块重新加载完成后再自愈。
    bool version_ok = load_version_file();

    if (!version_ok)
    {
        cfg_log("W", "version file missing or corrupt, will rebuild");
    }

    for (int i = 0; i < CONFIG_MODULE_COUNT; i++)
    {
        if (!load_module(&s_modules[i], true))
        {
            cfg_log(
                "E",
                "reload failed, module=%s",
                s_modules[i].name
            );
        }
    }

    if (!version_ok)
    {
        if (bootstrap_version_file())
        {
            cfg_log("I", "version file created (baseline)");
        }
        else
        {
            cfg_log("E", "version rebuild failed");
        }
    }

    return true;
}

// =====================================================
// 通用读取
// =====================================================
bool config_get_int(const char *module, const char *key, int &value)
{
    const JsonDocument &d = module_ro(module);

    JsonVariantConst v = d[key];

    // 键不存在或值为 null
    if (v.isNull())
    {
        return false;
    }

    // ArduinoJson 7 移除了 isNumber()。
    // is<double>() 对整数与浮点均为 true，对字符串 / bool / null 为 false。
    if (!v.is<double>())
    {
        return false;
    }

    value = v.as<int>();

    return true;
}

bool config_get_bool(const char *module, const char *key, bool &value)
{
    const JsonDocument &d = module_ro(module);

    JsonVariantConst v = d[key];

    if (v.isNull())
    {
        return false;
    }

    if (!v.is<bool>())
    {
        return false;
    }

    value = v.as<bool>();

    return true;
}

bool config_get_float(const char *module, const char *key, float &value)
{
    const JsonDocument &d = module_ro(module);

    JsonVariantConst v = d[key];

    if (v.isNull())
    {
        return false;
    }

    if (!v.is<double>())
    {
        return false;
    }

    value = v.as<float>();

    return true;
}

bool config_get_long(const char *module, const char *key, long &value)
{
    const JsonDocument &d = module_ro(module);

    JsonVariantConst v = d[key];

    if (v.isNull())
    {
        return false;
    }

    if (!v.is<double>())
    {
        return false;
    }

    value = v.as<long>();

    return true;
}

bool config_get_string(
    const char *module,
    const char *key,
    char *buffer,
    size_t size
)
{
    if (buffer == nullptr || size == 0)
    {
        return false;
    }

    const JsonDocument &d = module_ro(module);

    JsonVariantConst v = d[key];

    if (v.isNull())
    {
        return false;
    }

    const char *src = v.as<const char *>();

    if (src == nullptr)
    {
        return false;
    }

    size_t len = strlen(src);
    size_t copy = (len >= size) ? (size - 1) : len;

    memcpy(buffer, src, copy);
    buffer[copy] = '\0';

    return true;
}

// =====================================================
// 通用写入
// =====================================================
// 写入前的公共准备：定位模块、必要时初始化为空模块、标记 dirty
//
// schedule_restart:
//   true  —— 来自命令通道的修改，需要重启才能生效，刷新重启倒计时
//   false —— 业务模块自身的运行期修改（如称重校准写回零点），
//            不应为此重启设备，因此不刷新倒计时
static bool set_begin(
    const char *module,
    bool schedule_restart,
    ConfigModule *&out
)
{
    ConfigModule *m = find_module(module);

    if (m == nullptr)
    {
        return false;
    }

    // 模块未加载到 RAM 时，必须先尝试从文件恢复，不能直接清空。
    //
    // 未加载有两种情况，行为完全不同:
    //   1. 文件确实缺失（首次使用）→ 视为空模块，允许写入新配置
    //   2. 曾被 config_release_module() 释放过
    //                              → 文件里仍有完整数据，
    //                                直接 clear 会丢掉整份配置
    if (!m->loaded)
    {
        if (!load_module(m, true))
        {
            // 文件也读不到，才当作空模块
            m->data.clear();
        }

        m->loaded = true;
    }

    if (schedule_restart)
    {
        // Critical Operation 生命周期（需求文档 §3.2 / §七）:
        //
        //   配置修改事务开始 -> 首次 Acquire，连续修改不重复 Acquire。
        //   一个 Pending Save 事务只对应一个 Critical Operation，
        //   避免连续 config_set 造成 Count 永久增加。
        //
        //   Acquire 失败 = 系统已进入 RESTART_PENDING / RESTARTING
        //   （10 秒安全窗口），此时禁止新的配置修改（需求文档 §14），
        //   因此本函数直接拒绝本次修改，调用方收到 false。
        if (!s_pending_save)
        {
            if (!system_command_critical_operation_acquire())
            {
                cfg_log(
                    "E",
                    "set rejected: critical op acquire failed "
                    "(restart pending?) module=%s",
                    module
                );
                return false;
            }

            s_pending_save = true;

            cfg_log(
                "I",
                "critical op acquired (pending save transaction), module=%s",
                module
            );
        }

        restart_timer_start();
    }

    m->dirty = true;

    out = m;

    return true;
}

bool config_set_int(const char *module, const char *key, int value)
{
    ConfigModule *m = nullptr;

    if (!set_begin(module, true, m))
    {
        return false;
    }

    // 赋值可能因内存不足而失败。
    // 此时 dirty 与重启倒计时已经设置，必须如实报错并留痕；
    // 数据本身不会被破坏 —— save_module 的根对象校验会兜住。
    if (!m->data[key].set(value))
    {
        cfg_log("E", "set failed (oom?), module=%s key=%s", module, key);
        return false;
    }

    return true;
}

bool config_set_bool(const char *module, const char *key, bool value)
{
    ConfigModule *m = nullptr;

    if (!set_begin(module, true, m))
    {
        return false;
    }

    if (!m->data[key].set(value))
    {
        cfg_log("E", "set failed (oom?), module=%s key=%s", module, key);
        return false;
    }

    return true;
}

bool config_set_float(const char *module, const char *key, float value)
{
    ConfigModule *m = nullptr;

    if (!set_begin(module, true, m))
    {
        return false;
    }

    if (!m->data[key].set(value))
    {
        cfg_log("E", "set failed (oom?), module=%s key=%s", module, key);
        return false;
    }

    return true;
}

bool config_set_long(const char *module, const char *key, long value)
{
    ConfigModule *m = nullptr;

    if (!set_begin(module, true, m))
    {
        return false;
    }

    if (!m->data[key].set(value))
    {
        cfg_log("E", "set failed (oom?), module=%s key=%s", module, key);
        return false;
    }

    return true;
}

bool config_set_string(const char *module, const char *key, const char *value)
{
    if (value == nullptr)
    {
        return false;
    }

    ConfigModule *m = nullptr;

    if (!set_begin(module, true, m))
    {
        return false;
    }

    if (!m->data[key].set(value))
    {
        cfg_log("E", "set failed (oom?), module=%s key=%s", module, key);
        return false;
    }

    return true;
}

// =====================================================
// 整体获取 / 版本 / 释放
// =====================================================
bool config_get_json(const char *module, JsonDocument &output)
{
    ConfigModule *m = find_module(module);

    if (m == nullptr || !m->loaded)
    {
        return false;
    }

    // 深拷贝到调用方文档，避免返回内部引用后因 reload 失效。
    //
    // set() 在内存不足时返回 false，必须检查；
    // 否则调用方会拿到一个空的 output 却认为读取成功。
    output.clear();

    if (!output.set(m->data.as<JsonVariantConst>()))
    {
        return false;
    }

    return true;
}

uint8_t config_get_version(const char *module)
{
    ConfigModule *m = find_module(module);

    if (m == nullptr)
    {
        return 0;
    }

    return m->version;
}

bool config_release_module(const char *module)
{
    ConfigModule *m = find_module(module);

    if (m == nullptr)
    {
        return false;
    }

    m->data.clear();
    m->data.shrinkToFit();
    m->loaded = false;

    // 必须一并清除 dirty。
    //
    // 否则后续 config_save() 会把这个已清空的模块当作"待保存"，
    // 把空文档写进 Active。save_module 的根对象校验虽能兜住，
    // 但 dirty 残留本身已经是错误状态，不应留下。
    m->dirty = false;

    return true;
}

// =====================================================
// 异步配置命令：value 的入队与还原
// =====================================================
// value 生命周期的核心：任务槽位持有的是自己的副本，
// 不保存任何指向调用方 JsonDocument 的引用。
// allow_object: 仅批量写模块(CONFIG_CMD_SET_MODULE)时为 true；
//               单字段写入只接受标量。
static bool value_slot_from_variant(
    JsonVariantConst value,
    bool allow_object,
    ConfigValueSlot &out
)
{
    out.kind = CONFIG_VALUE_NONE;
    out.big = nullptr;
    out.big_len = 0;

    // null / 未绑定
    if (value.isNull() || value.isUnbound())
    {
        return false;
    }

    // 数组一律不接受
    if (value.is<JsonArrayConst>())
    {
        return false;
    }

    // 对象：仅批量写模块时接受，序列化后暂存
    if (value.is<JsonObjectConst>())
    {
        if (!allow_object)
        {
            return false;
        }

        // 先量长度，避免中间产生临时 String
        size_t n = measureJson(value);

        if (n == 0 || n >= CONFIG_VALUE_BIG_MAX)
        {
            return false;
        }

        // 短对象直接落定长缓冲，不惊动 PSRAM
        if (n < CONFIG_VALUE_STRING_MAX)
        {
            memset(out.v.s, 0, sizeof(out.v.s));

            if (serializeJson(value, out.v.s, sizeof(out.v.s)) != n)
            {
                return false;
            }

            out.kind = CONFIG_VALUE_OBJECT;
            return true;
        }

        // 长对象放 PSRAM，由 value_slot_free() 负责释放
        char *buf = (char *)heap_caps_malloc(n + 1, CONFIG_MEM_SPIRAM);

        if (buf == nullptr)
        {
            return false;
        }

        size_t written = serializeJson(value, buf, n + 1);

        if (written == 0 || written > n)
        {
            heap_caps_free(buf);
            return false;
        }

        buf[written] = '\0';

        out.big = buf;
        out.big_len = written;
        out.kind = CONFIG_VALUE_OBJECT;

        return true;
    }

    // bool 优先判断，避免被后面的整数分支吞掉
    if (value.is<bool>())
    {
        out.kind = CONFIG_VALUE_BOOL;
        out.v.b = value.as<bool>();
        return true;
    }

    if (value.is<const char *>())
    {
        const char *src = value.as<const char *>();

        if (src == nullptr)
        {
            return false;
        }

        size_t len = strlen(src);

        if (len >= CONFIG_VALUE_STRING_MAX)
        {
            return false;   // 槽位放不下
        }

        // 必须先整体清零，再复制。
        //
        // 原因: v.s 位于联合体中，之前可能存放过其它类型，
        // 且本次 memcpy 只覆盖前 len 字节，尾部仍残留旧内容。
        // 这些残留字节会随字符串一起被写进配置，
        // 典型现象就是"第一次写入正常、之后再写就出现乱码"。
        memset(out.v.s, 0, sizeof(out.v.s));
        memcpy(out.v.s, src, len);

        out.kind = CONFIG_VALUE_STRING;

        return true;
    }

    if (value.is<int>())
    {
        out.kind = CONFIG_VALUE_INT;
        out.v.i = value.as<int>();
        return true;
    }

    if (value.is<double>())
    {
        double d = value.as<double>();

        // 超出 int 表示范围的数值直接拒绝。
        //
        // 不能降级存成 float —— 那会静默丢失精度，
        // 并让该字段的类型从整数变成浮点，后续类型校验随之失真。
        if (d > CONFIG_INT_VALUE_MAX || d < CONFIG_INT_VALUE_MIN)
        {
            return false;
        }

        // 无小数部分 → 仍归为整数
        if (d == (double)(long)d)
        {
            out.kind = CONFIG_VALUE_INT;
            out.v.i = (int)d;
            return true;
        }

        out.kind = CONFIG_VALUE_FLOAT;
        out.v.f = (float)d;

        return true;
    }

    return false;
}

// 把槽位中的值写回模块数据
//
// 返回是否真正写入。
//
// 槽位为空（CONFIG_VALUE_NONE）时返回 false。
// 调用方必须在改动任何状态之前拦截这种情况，
// 否则会出现"已标记 dirty 并安排重启，却什么都没写入"的污染状态。
static bool value_slot_apply(
    const ConfigValueSlot &slot,
    JsonDocument &data,
    const char *key
)
{
    switch (slot.kind)
    {
        case CONFIG_VALUE_INT:
            data[key] = slot.v.i;
            return true;

        case CONFIG_VALUE_BOOL:
            data[key] = slot.v.b;
            return true;

        case CONFIG_VALUE_FLOAT:
            data[key] = slot.v.f;
            return true;

        case CONFIG_VALUE_STRING:
            // 必须先转成 const char* 再赋值。
            //
            // 若直接传定长数组 slot.v.s，ArduinoJson 的 adaptString
            // 会走 StringAdapter<const char(&)[N]>，长度取【数组大小-1】
            // (= CONFIG_VALUE_STRING_MAX - 1)，而不是字符串实际长度，
            // 于是把缓冲区尾部的残留字节也一起写进配置，
            // 表现为字符串后面拖着一串乱码。
            data[key] = (const char *)slot.v.s;
            return true;

        default:
            return false;
    }
}

// 检查新 value 的类型是否与目标字段的现有类型匹配
//
// 原则: 类型必须匹配字段定义，不做危险的隐式转换。
//   已有 bool 字段    → 只接受 bool
//   已有 string 字段  → 只接受 string
//   已有数值字段      → 接受 int / float
//
//     关于 int / float 不做严格区分的说明:
//       ArduinoJson 7 中 is<float>() 对整数同样返回 true，
//       无法用它区分"int 字段"与"float 字段"。
//       若强行按注释写 return existing.is<int>()，
//       浮点字段会被判定为 int 字段，反而产生错误拦截。
//       因此两者统一按"数值"处理。
//
//     真正危险的是跨类型转换，那些仍然被严格挡住:
//       "123" → int、123 → bool、true → string、object → string
//
//   字段不存在 → 属于新增字段，接受任意标量
//                （null / object / array 已在入队阶段被拒绝）
//
// 本函数必须在任何 RAM 修改之前调用：
//   类型不匹配的请求不得污染当前已生效的配置。
static bool value_type_matches(
    JsonVariantConst existing,
    const ConfigValueSlot &slot
)
{
    // 字段不存在 → 新增
    if (existing.isNull())
    {
        return true;
    }

    switch (slot.kind)
    {
        case CONFIG_VALUE_BOOL:
            return existing.is<bool>();

        case CONFIG_VALUE_INT:
        case CONFIG_VALUE_FLOAT:
            // 数值字段统一按"数字"处理（理由见函数头注释）
            return existing.is<double>();

        case CONFIG_VALUE_STRING:
            return existing.is<const char *>();

        default:
            return false;
    }
}

// =====================================================
// 异步配置命令：执行体
//
// 这些函数只在 config_task() 中被调用，
// 因此可以安全地执行同步文件 IO。
// =====================================================
struct ExecResult
{
    bool success;
    ConfigCommandError error;
    const char *message;
};

static ExecResult result_ok(const char *msg)
{
    ExecResult r;
    r.success = true;
    r.error = CONFIG_ERR_NONE;
    r.message = msg;
    return r;
}

static ExecResult result_fail(ConfigCommandError err, const char *msg)
{
    ExecResult r;
    r.success = false;
    r.error = err;
    r.message = msg;
    return r;
}

static ExecResult exec_query_field(
    const ConfigCommandTask &t,
    JsonDocument &out
)
{
    ConfigModule *m = find_module(t.module);

    if (m == nullptr || !m->loaded)
    {
        return result_fail(CONFIG_ERR_MODULE_NOT_FOUND, "module not found");
    }

    // 用 const 引用访问，确保走"纯查找"路径（JsonDocument 的 const
    // operator[] 直接调用 getMember）。
    //
    // 若用非 const 的 m->data[key]，走的是 MemberProxy，
    // 它同时具备 get / get-or-create 两种语义，
    // 一旦触发创建会往配置里插入 null 成员，
    // 后续 config_save() 就会把这个 null 写进文件。
    const JsonDocument &data = m->data;

    // 关键: 数组成员必须先退化成 const char* 再用于 JSON 查找 / 写入。
    //
    // ArduinoJson 的 adaptString 对定长数组走
    //   StringAdapter<const char (&)[N]> → RamString(p, N - 1)
    // 长度取的是【数组大小 N-1】，而不是字符串实际长度。
    //   char key[32] 存 "ssid" → 按长度 31 去匹配键 "ssid"(长度 4)
    //   → 永远匹配不上，表现为 "值正确、数据里也有、就是查不到"。
    //
    // 退化成 const char* 后会走指针重载，用 strlen 计算长度，行为才正确。
    const char *key_ptr = t.key;

    JsonVariantConst v = data[key_ptr];

    if (v.isNull())
    {
        // 诊断输出：打印实际查找用的 module / key，
        // 以及该模块里真实存在的所有键，便于定位参数传递问题。
        Serial.printf(
            "[CFG] key not found: module=[%s] key=[%s]\n",
            t.module,
            key_ptr
        );

        for (JsonPairConst kv : data.as<JsonObjectConst>())
        {
            Serial.printf("[CFG]   available: [%s]\n", kv.key().c_str());
        }

        return result_fail(CONFIG_ERR_KEY_NOT_FOUND, "key not found");
    }

    out.clear();

    // 逐项检查写入结果。
    //
    // ArduinoJson 在内存不足时会静默失败（set 返回 false），
    // 若不检查就会把一个残缺的 result 当作成功结果交给上层。
    //
    // set() 内部走 copyVariant → saveString，
    // 会把字符串拷进 out 自己的内存池，不共享 ConfigManager 内部缓冲。
    //
    // 同样注意: module / key 都要传退化后的指针，理由同上。
    // module 同样是定长数组，必须先退化成指针，理由同 key
    // （否则 ArduinoJson 按数组大小写入，值后面会拖一串填充字节）。
    const char *module_ptr = t.module;

    // 返回结构采用 {data:{...}, version:N}。
    //
    // 把 data 单独包一层，是为了让版本号与业务数据分离，
    // 同时不污染模块内容本身（否则往模块里塞一个 version
    // 字段，保存时会把它写进配置文件）。
    JsonObject d = out["data"].to<JsonObject>();

    if (!d["module"].set(module_ptr) ||
        !d["key"].set(key_ptr) ||
        !d["value"].set(v) ||
        !out["version"].set(m->version))
    {
        return result_fail(CONFIG_ERR_IO_FAILED, "result build failed");
    }

    return result_ok("query field ok");
}

static ExecResult exec_query_module(
    const ConfigCommandTask &t,
    JsonDocument &out
)
{
    ConfigModule *m = find_module(t.module);

    if (m == nullptr || !m->loaded)
    {
        return result_fail(CONFIG_ERR_MODULE_NOT_FOUND, "module not found");
    }

    // keys_only: 只返回字段名，不返回字段值。
    //
    // 查询整个模块会带回全部字段值，模块变大后容易逼近报文上限；
    // 只想知道有哪些字段时用这个模式，能显著降低报文长度。
    if (t.keys_only)
    {
        out.clear();

        JsonArray arr = out["keys"].to<JsonArray>();

        for (JsonPairConst kv : m->data.as<JsonObjectConst>())
        {
            if (!arr.add(kv.key().c_str()))
            {
                return result_fail(
                    CONFIG_ERR_IO_FAILED,
                    "result build failed"
                );
            }
        }

        if (!out["version"].set(m->version))
        {
            return result_fail(CONFIG_ERR_IO_FAILED, "result build failed");
        }

        return result_ok("query module keys ok");
    }

    out.clear();

    // 深拷贝：set 内部把数据拷进 out 自己的内存池。
    // 内存不足时返回 false，必须检查。
    //
    // 结构与 query_field 保持一致: {data:{...}, version:N}
    // 版本号不放进 data，避免保存时被写进配置文件。
    if (!out["data"].set(m->data.as<JsonVariantConst>()) ||
        !out["version"].set(m->version))
    {
        return result_fail(CONFIG_ERR_IO_FAILED, "result build failed");
    }

    return result_ok("query module ok");
}

static ExecResult exec_query_all(
    const ConfigCommandTask &t,
    JsonDocument &out
)
{
    // keys_only: 只返回模块名列表
    if (t.keys_only)
    {
        out.clear();

        JsonArray arr = out["modules"].to<JsonArray>();

        for (int i = 0; i < CONFIG_MODULE_COUNT; i++)
        {
            if (!s_modules[i].loaded)
            {
                continue;
            }

            if (!arr.add(s_modules[i].name))
            {
                return result_fail(
                    CONFIG_ERR_IO_FAILED,
                    "result build failed"
                );
            }
        }

        return result_ok("query module list ok");
    }

    out.clear();

    // data     : 各模块内容
    // versions : 各模块版本号
    //
    // 版本号与内容分离，不写进模块数据本身。
    JsonObject root = out["data"].to<JsonObject>();
    JsonObject vers = out["versions"].to<JsonObject>();

    for (int i = 0; i < CONFIG_MODULE_COUNT; i++)
    {
        if (!s_modules[i].loaded)
        {
            continue;
        }

        const char *name = s_modules[i].name;

        // 深拷贝到 out，并检查结果（内存不足会失败）
        if (!root[name].set(s_modules[i].data.as<JsonVariantConst>()) ||
            !vers[name].set(s_modules[i].version))
        {
            return result_fail(CONFIG_ERR_IO_FAILED, "result build failed");
        }
    }

    return result_ok("query all ok");
}

// 乐观锁校验
//
// 返回 true 表示通过（含未启用校验的情况）。
// 不通过时把当前版本号写进 out，UI 可直接据此刷新，无需再查一次。
static bool version_check_pass(
    const ConfigCommandTask &t,
    const ConfigModule *m,
    JsonDocument &out
)
{
    if (!t.check_version)
    {
        return true;
    }

    if (m->version == t.expect_version)
    {
        return true;
    }

    out.clear();
    out["current_version"] = m->version;

    return false;
}

static ExecResult exec_set_field(
    const ConfigCommandTask &t,
    JsonDocument &out
)
{
    // ---- 前置检查（必须在任何 RAM 修改之前完成）----
    //
    // 非法请求不得污染当前已生效的配置:
    //   不修改 RAM、不写盘、不更新 version、不触发 restart。

    // value 槽位必须已填充。
    //
    // 入队阶段已保证 SET_FIELD 的 value 是合法标量，
    // 这里是二次防御，且必须在 set_begin 之前完成 ——
    // 否则会出现"dirty 已标记、重启已安排，却什么都没写入"。
    if (t.value.kind == CONFIG_VALUE_NONE)
    {
        return result_fail(CONFIG_ERR_VALUE_REJECTED, "value slot empty");
    }

    ConfigModule *target = find_module(t.module);

    if (target == nullptr)
    {
        return result_fail(CONFIG_ERR_MODULE_NOT_FOUND, "module not found");
    }

    // 乐观锁：版本不符直接拒绝，避免覆盖他人已改动的配置
    if (!version_check_pass(t, target, out))
    {
        return result_fail(CONFIG_ERR_VERSION_MISMATCH, "version mismatch");
    }

    if (target->loaded)
    {
        // 只读访问: 避免 MemberProxy 的 get-or-create 语义。
        //
        // 此处 key 可能并不存在（新增字段），若用非 const 访问，
        // 一旦触发创建就会往配置里插入一个 null 成员，
        // 后续 config_save() 会把这个 null 写进文件。
        const JsonDocument &existing_data = target->data;

        // 同样必须退化为 const char* 后查找:
        // 定长数组会让 ArduinoJson 按【数组大小】而非实际长度匹配，
        // 结果是永远查不到已有字段，类型校验随之失效。
        const char *key_ptr = t.key;

        JsonVariantConst existing = existing_data[key_ptr];

        if (!value_type_matches(existing, t.value))
        {
            cfg_log(
                "W",
                "set rejected: type mismatch, module=%s key=%s",
                t.module,
                t.key
            );

            return result_fail(
                CONFIG_ERR_VALUE_REJECTED,
                "value type does not match field definition"
            );
        }
    }

    // ---- 写入 ----
    //
    // 命令通道的修改需要重启生效 → schedule_restart = true
    ConfigModule *m = nullptr;

    if (!set_begin(t.module, true, m))
    {
        return result_fail(CONFIG_ERR_MODULE_NOT_FOUND, "module not found");
    }

    // 不存在则新增，存在则覆盖
    if (!value_slot_apply(t.value, m->data, t.key))
    {
        return result_fail(CONFIG_ERR_VALUE_REJECTED, "value apply failed");
    }

    return result_ok("field updated, restart required");
}

// 删除单个字段
//
// 字段不存在时返回明确错误，不改动任何内容。
// 删除同样需要重启生效（配置在启动时加载）。
static ExecResult exec_delete_field(
    const ConfigCommandTask &t,
    JsonDocument &out
)
{
    ConfigModule *m = find_module(t.module);

    if (m == nullptr || !m->loaded)
    {
        return result_fail(CONFIG_ERR_MODULE_NOT_FOUND, "module not found");
    }

    // 乐观锁：版本不符直接拒绝
    if (!version_check_pass(t, m, out))
    {
        return result_fail(CONFIG_ERR_VERSION_MISMATCH, "version mismatch");
    }

    // 必须退化成指针：定长数组会让 ArduinoJson 按数组大小匹配
    const char *key_ptr = t.key;

    const JsonDocument &cur = m->data;

    if (cur[key_ptr].isNull())
    {
        return result_fail(CONFIG_ERR_KEY_NOT_FOUND, "key not found");
    }

    // 走统一写入入口：标记 dirty 并安排重启
    ConfigModule *target = nullptr;

    if (!set_begin(t.module, true, target))
    {
        return result_fail(CONFIG_ERR_MODULE_NOT_FOUND, "module not found");
    }

    target->data.remove(key_ptr);

    return result_ok("field deleted, restart required");
}

// 批量写入模块字段（严格校验）
//
// 规则:
//   1. value 必须是对象
//   2. 传入的每个字段都必须在目标模块中已存在
//   3. 任一字段不存在 → 整体拒绝，不写入任何内容
//
// 语义是"覆盖式更新": 只覆盖传入的字段，未提及的字段保持原样。
// 这样即便 UI 少传字段也不会误删配置；
// 需要删除字段请使用 config_delete。
static ExecResult exec_set_module(
    const ConfigCommandTask &t,
    JsonDocument &out
)
{
    ConfigModule *m = find_module(t.module);

    if (m == nullptr || !m->loaded)
    {
        return result_fail(CONFIG_ERR_MODULE_NOT_FOUND, "module not found");
    }

    // 乐观锁：版本不符直接拒绝
    if (!version_check_pass(t, m, out))
    {
        return result_fail(CONFIG_ERR_VERSION_MISMATCH, "version mismatch");
    }

    if (t.value.kind != CONFIG_VALUE_OBJECT)
    {
        return result_fail(
            CONFIG_ERR_VALUE_REJECTED,
            "module value must be an object"
        );
    }

    const char *json = value_slot_text(t.value);

    JsonDocument incoming;

    if (deserializeJson(incoming, json) != DeserializationError::Ok)
    {
        return result_fail(CONFIG_ERR_VALUE_REJECTED, "invalid module json");
    }

    if (!incoming.is<JsonObject>())
    {
        return result_fail(
            CONFIG_ERR_VALUE_REJECTED,
            "module value must be an object"
        );
    }

    const JsonDocument &cur = m->data;

    // ---- 严格校验: 每个字段都必须已存在 ----
    for (JsonPairConst kv : incoming.as<JsonObjectConst>())
    {
        const char *k = kv.key().c_str();

        if (cur[k].isNull())
        {
            Serial.printf(
                "[CFG] module write rejected, unknown field: [%s]\n",
                k
            );

            return result_fail(
                CONFIG_ERR_VALUE_REJECTED,
                "unknown field, module write rejected"
            );
        }
    }

    // ---- 校验通过，覆盖写入 ----
    ConfigModule *target = nullptr;

    if (!set_begin(t.module, true, target))
    {
        return result_fail(CONFIG_ERR_MODULE_NOT_FOUND, "module not found");
    }

    for (JsonPairConst kv : incoming.as<JsonObjectConst>())
    {
        if (!target->data[kv.key().c_str()].set(kv.value()))
        {
            return result_fail(
                CONFIG_ERR_IO_FAILED,
                "module field write failed"
            );
        }
    }

    return result_ok("module updated, restart required");
}

// 把当前模块备份为 factory 副本
//
// 这个动作只写 factory 文件，不改动当前配置，
// 因此不标记 dirty、不安排重启。
static ExecResult exec_backup_module(const ConfigCommandTask &t)
{
    ConfigModule *m = find_module(t.module);

    if (m == nullptr || !m->loaded)
    {
        return result_fail(CONFIG_ERR_MODULE_NOT_FOUND, "module not found");
    }

    char path[CONFIG_PATH_MAX];

    if (!module_factory_path(t.module, path, sizeof(path)))
    {
        return result_fail(CONFIG_ERR_IO_FAILED, "factory path invalid");
    }

    // 目录可能尚不存在（首次备份），必须先创建，
    // 否则 write_atomic 会因无法创建 .tmp 文件而失败。
    if (!factory_dir_ensure())
    {
        return result_fail(CONFIG_ERR_IO_FAILED, "factory dir unavailable");
    }

    String content;

    if (serializeJson(m->data, content) == 0)
    {
        return result_fail(CONFIG_ERR_IO_FAILED, "serialize failed");
    }

    if (!json_storage_write_atomic(path, content))
    {
        return result_fail(CONFIG_ERR_IO_FAILED, "factory write failed");
    }

    // 不调用 set_begin: 备份不属于配置修改
    return result_ok("module backed up to factory");
}

// 从 factory 副本恢复模块
//
// 这是"完全替换"语义: 模块内容被 factory 内容整体覆盖。
// 与 config_module 的"覆盖式更新"不同，重置就是要回到基线。
static ExecResult exec_reset_module(const ConfigCommandTask &t)
{
    ConfigModule *m = find_module(t.module);

    if (m == nullptr)
    {
        return result_fail(CONFIG_ERR_MODULE_NOT_FOUND, "module not found");
    }

    char path[CONFIG_PATH_MAX];

    if (!module_factory_path(t.module, path, sizeof(path)))
    {
        return result_fail(CONFIG_ERR_IO_FAILED, "factory path invalid");
    }

    String content;

    if (!json_storage_read(path, content))
    {
        return result_fail(
            CONFIG_ERR_FACTORY_MISSING,
            "factory copy not found"
        );
    }

    // 先解析校验，确认是合法对象再动配置
    JsonDocument parsed;

    if (deserializeJson(parsed, content) != DeserializationError::Ok ||
        !parsed.is<JsonObject>())
    {
        return result_fail(CONFIG_ERR_IO_FAILED, "factory copy invalid");
    }

    // 走统一写入入口：标记 dirty 并安排重启
    ConfigModule *target = nullptr;

    if (!set_begin(t.module, true, target))
    {
        return result_fail(CONFIG_ERR_MODULE_NOT_FOUND, "module not found");
    }

    target->data.clear();

    if (!target->data.set(parsed.as<JsonVariantConst>()))
    {
        return result_fail(CONFIG_ERR_IO_FAILED, "factory apply failed");
    }

    return result_ok("module reset to factory, restart required");
}

static ExecResult exec_save(JsonDocument &out)
{
    if (!s_ready)
    {
        return result_fail(CONFIG_ERR_UNAVAILABLE, "storage unavailable");
    }

    // 先判断是否存在 dirty 模块，此阶段不写入
    bool any_dirty = false;

    for (int i = 0; i < CONFIG_MODULE_COUNT; i++)
    {
        if (s_modules[i].dirty)
        {
            any_dirty = true;
            break;
        }
    }

    // 无 dirty 是合法的幂等结果，不是错误：
    // 连续多次调用 save 应当得到一致的成功应答。
    // 用 saved=false 告知"本次没有实际写入"，而不是返回 ERROR。
    if (!any_dirty)
    {
        if (!out["saved"].set(false) || !out["dirty"].set(false))
        {
            return result_fail(CONFIG_ERR_IO_FAILED, "result build failed");
        }

        // 幂等成功。若仍存在待收尾事务（如上次 Save 失败回滚后无 dirty 残留），
        // 在此收尾，避免 Critical Operation 被永久占用（需求文档 §四 / §九 恢复路径）。
        pending_save_finalize();

        return result_ok("no dirty module, nothing to save");
    }

    // ---- 1) 标记提交开始 ----
    if (!commit_mark_pending())
    {
        return result_fail(
            CONFIG_ERR_IO_FAILED,
            "cannot mark commit pending"
        );
    }

    // ---- 2) Config Data 写入 ----
    bool all_ok = true;

    for (int i = 0; i < CONFIG_MODULE_COUNT; i++)
    {
        if (!s_modules[i].dirty)
        {
            continue;
        }

        if (!save_module(&s_modules[i]))
        {
            all_ok = false;
        }
    }

    // Data 提交阶段：任一模块写入失败即视为整批失败，
    // 不允许出现"部分模块是新值、部分模块是旧值"的混合状态。
    if (!all_ok)
    {
        commit_fail_and_rollback("data");

        return result_fail(
            CONFIG_ERR_IO_FAILED,
            "config data commit failed, rolled back"
        );
    }

    // Version 提交阶段。
    //
    // 只有 Data 成功 + Version 成功，本次 Commit 才算成功。
    // Version 失败意味着 Data 与 Version 失去一致性，
    // 新配置不可信，必须回滚到 Last Known Good Config。
    if (!save_version_file())
    {
        commit_fail_and_rollback("version");

        return result_fail(
            CONFIG_ERR_IO_FAILED,
            "version commit failed, rolled back"
        );
    }

    // 标记提交完成。
    //
    // 走到这里说明 Data 与 Version 均已写入成功，
    // 只是状态标记没能写成 DONE（文件仍为 PENDING）。
    //
    // 后果: 下次启动读到 PENDING 会回滚，本次提交可能被撤销。
    // 因此必须如实报错，并在 message 中说明，
    // 避免调用方误以为"数据压根没保存"。
    if (!commit_mark_done())
    {
        return result_fail(
            CONFIG_ERR_IO_FAILED,
            "data saved, but commit state not finalized; "
            "config may roll back on next boot"
        );
    }

    if (!out["saved"].set(true) || !out["dirty"].set(false))
    {
        return result_fail(CONFIG_ERR_IO_FAILED, "result build failed");
    }

    // 落盘成功 -> Release + Request Restart（需求文档 §四 / §七）。
    //
    // 只有 Data + Version + 提交状态全部写入成功才走到这里；
    // 前面的任何失败都已 commit_fail_and_rollback() 提前返回，
    // 保持 Critical Operation 占用、不请求重启（需求文档 §九）。
    pending_save_finalize();

    return result_ok("config saved");
}

static ExecResult exec_restart(const ConfigCommandTask &t)
{
    if (t.cancel)
    {
        // 取消的只是本模块自己的 5 分钟自动重启倒计时。
        //
        // 真正的 Restart 请求一旦交给 SystemCommand 就不可取消
        //（需求文档 §13）。这里能取消的仅是"尚未发生的自动重启"。
        config_cancel_restart();
        return result_ok("config auto-restart timer cancelled");
    }

    // 重启前先落盘，避免未保存的修改丢失。
    //
    // 需求文档 §九: Save 写入 Flash 失败 -> 不得 Release、不得 Request Restart。
    // 配置已回滚到 Last Known Good，本次重启请求直接拒绝，
    // 由调用方决定是否重试（可再次下发 save / restart）。
    if (!config_save())
    {
        cfg_log("E", "pre-restart save failed, restart request aborted");
        return result_fail(
            CONFIG_ERR_IO_FAILED,
            "save failed, restart aborted"
        );
    }

    // 落盘成功 -> Release + Request Restart（需求文档 §四）。
    //
    // pending_save_finalize() 只处理"真实存在的待收尾事务"；
    // 这里再显式请求一次以覆盖无事务场景（例如无修改直接 restart），
    // request_restart() 幂等，重复调用安全（需求文档 §13）。
    pending_save_finalize();

    // 本模块不执行重启，只发出请求。
    //
    // SystemCommand 会先等 Critical Operation 归零，再进入 10s 安全窗口。
    // 这个窗口足以让本函数的 SUCCESS 经 completion callback 交给上层、
    // 并由 MQTT 真正发出，到点后才执行 ESP.restart()。
    if (!system_command_request_restart())
    {
        return result_fail(CONFIG_ERR_UNAVAILABLE, "restart request rejected");
    }

    return result_ok("restart requested via SystemCommand");
}

// =====================================================
// 队列消费：每次只处理一个任务
// =====================================================
static void config_cmd_process_one()
{
    if (s_cmd_count == 0)
    {
        return;
    }

    // FIFO：从 s_cmd_head 起找第一个占用的槽位
    int idx = -1;

    for (int n = 0; n < CONFIG_CMD_QUEUE_SIZE; n++)
    {
        int i = (s_cmd_head + n) % CONFIG_CMD_QUEUE_SIZE;

        if (s_cmd_queue[i].active)
        {
            idx = i;
            break;
        }
    }

    if (idx < 0)
    {
        s_cmd_count = 0;   // 计数与实际状态不一致时自愈
        return;
    }

    ConfigCommandTask &t = s_cmd_queue[idx];

    // ---- 执行（同步调用 JsonStorage）----
    JsonDocument result;
    ExecResult r = result_fail(CONFIG_ERR_INVALID_PARAM, "unknown type");

    switch (t.type)
    {
        case CONFIG_CMD_QUERY_FIELD:
            r = exec_query_field(t, result);
            break;

        case CONFIG_CMD_QUERY_MODULE:
            r = exec_query_module(t, result);
            break;

        case CONFIG_CMD_QUERY_ALL:
            r = exec_query_all(t, result);
            break;

        case CONFIG_CMD_SET_FIELD:
            r = exec_set_field(t, result);
            break;

        case CONFIG_CMD_DELETE_FIELD:
            r = exec_delete_field(t, result);
            break;

        case CONFIG_CMD_SET_MODULE:
            r = exec_set_module(t, result);
            break;

        case CONFIG_CMD_RESET_MODULE:
            r = exec_reset_module(t);
            break;

        case CONFIG_CMD_BACKUP_MODULE:
            r = exec_backup_module(t);
            break;

        case CONFIG_CMD_SAVE:
            r = exec_save(result);
            break;

        case CONFIG_CMD_RESTART:
            r = exec_restart(t);
            break;

        default:
            break;
    }

    // ---- 先把回调所需参数复制到栈上，再释放槽位 ----
    //
    // 释放之后才回调，这样回调中若再次 enqueue，槽位已经可用。
    char cmd_id[CONFIG_CMD_ID_MAX];
    char command[CONFIG_CMD_NAME_MAX];
    char object[CONFIG_CMD_NAME_MAX];
    char source[CONFIG_CMD_SOURCE_MAX];

    strncpy(cmd_id,  t.cmd_id,  CONFIG_CMD_ID_MAX - 1);
    strncpy(command, t.command, CONFIG_CMD_NAME_MAX - 1);
    strncpy(object,  t.object,  CONFIG_CMD_NAME_MAX - 1);
    strncpy(source,  t.source,  CONFIG_CMD_SOURCE_MAX - 1);

    cmd_id[CONFIG_CMD_ID_MAX - 1]     = '\0';
    command[CONFIG_CMD_NAME_MAX - 1]  = '\0';
    object[CONFIG_CMD_NAME_MAX - 1]   = '\0';
    source[CONFIG_CMD_SOURCE_MAX - 1] = '\0';

    ConfigCommandType type = t.type;

    // 释放大 value 占用的 PSRAM。
    //
    // 必须在此处完成：任务已经执行完，后续回调不再需要 t.value。
    // 不释放会造成长期运行的内存泄漏。
    value_slot_free(t.value);

    // 释放槽位
    t.active = false;
    s_cmd_count--;
    s_cmd_head = (idx + 1) % CONFIG_CMD_QUEUE_SIZE;

    // ---- 完成回调 ----
    //
    // result 是栈上文档，仅在回调期间有效。
    if (s_completion_cb != nullptr)
    {
        s_completion_cb(
            cmd_id,
            command,
            object,
            source,
            type,
            r.success,
            r.error,
            result,
            r.message
        );
    }

    // 注: 原"回调之后进入安全延迟并在到点后 ESP.restart()"的逻辑已移除。
    // Restart 请求在 exec_restart() 内已直接交给 SystemCommand，
    // 由其状态机统一等待 Critical Operation 并执行安全窗口。
}

// =====================================================
// 异步配置命令：入队
//
// 只做: 参数检查 → 复制参数（含 value 深拷贝）→ 入队 → 立即返回。
// 不执行任何查询 / 保存 / 文件 IO。
// =====================================================
static bool copy_str(char *dst, size_t dst_size, const char *src)
{
    if (dst == nullptr || dst_size == 0)
    {
        return false;
    }

    if (src == nullptr)
    {
        dst[0] = '\0';
        return true;
    }

    size_t len = strlen(src);

    if (len >= dst_size)
    {
        return false;   // 超长，拒绝而不是截断
    }

    memcpy(dst, src, len + 1);

    return true;
}

ConfigEnqueueResult config_cmd_enqueue(
    ConfigCommandType type,
    const char *cmd_id,
    const char *command,
    const char *object,
    const char *source,
    const char *module,
    const char *key,
    JsonVariantConst value,
    bool cancel,
    bool keys_only,
    int expect_version
)
{
    if (!s_ready)
    {
        return CONFIG_ENQUEUE_UNAVAILABLE;
    }

    if (type == CONFIG_CMD_NONE)
    {
        return CONFIG_ENQUEUE_INVALID;
    }

    // 容量检查放在最前，避免部分填充后才发现队列满
    if (s_cmd_count >= CONFIG_CMD_QUEUE_SIZE)
    {
        cfg_log("W", "enqueue rejected: queue full");
        return CONFIG_ENQUEUE_QUEUE_FULL;
    }

    int idx = -1;

    for (int i = 0; i < CONFIG_CMD_QUEUE_SIZE; i++)
    {
        if (!s_cmd_queue[i].active)
        {
            idx = i;
            break;
        }
    }

    if (idx < 0)
    {
        cfg_log("W", "enqueue rejected: queue full");
        return CONFIG_ENQUEUE_QUEUE_FULL;
    }

    // 各命令必需的参数
    bool need_module = (type == CONFIG_CMD_QUERY_FIELD ||
                        type == CONFIG_CMD_QUERY_MODULE ||
                        type == CONFIG_CMD_SET_FIELD ||
                        type == CONFIG_CMD_DELETE_FIELD ||
                        type == CONFIG_CMD_SET_MODULE);

    bool need_key = (type == CONFIG_CMD_QUERY_FIELD ||
                     type == CONFIG_CMD_SET_FIELD ||
                     type == CONFIG_CMD_DELETE_FIELD);

    if (need_module && (module == nullptr || module[0] == '\0'))
    {
        return CONFIG_ENQUEUE_INVALID;
    }

    if (need_key && (key == nullptr || key[0] == '\0'))
    {
        return CONFIG_ENQUEUE_INVALID;
    }

    // value 校验 + 深拷贝（仅 SET_FIELD 需要）
    // value 槽位。
    //
    // 必须整体清零: 联合体尾部若残留上一次的数据，
    // 会被当作字符串内容一起写进配置。
    ConfigValueSlot slot;
    memset(&slot, 0, sizeof(slot));
    slot.kind = CONFIG_VALUE_NONE;

    // 单字段写入：只接受标量
    if (type == CONFIG_CMD_SET_FIELD)
    {
        if (!value_slot_from_variant(value, false, slot))
        {
            cfg_log("W", "enqueue rejected: value is not a scalar");
            return CONFIG_ENQUEUE_INVALID;
        }
    }
    // 批量写模块：接受 object
    else if (type == CONFIG_CMD_SET_MODULE)
    {
        if (!value_slot_from_variant(value, true, slot))
        {
            cfg_log("W", "enqueue rejected: module value is not an object");
            return CONFIG_ENQUEUE_INVALID;
        }
    }

    ConfigCommandTask &t = s_cmd_queue[idx];

    t.type = type;
    t.cancel = cancel;
    t.value = slot;

    // 查询只返回键名（不返回值），用于降低报文长度
    t.keys_only = keys_only;

    // 乐观锁：expect_version < 0 表示不校验
    t.check_version = (expect_version >= 0 && expect_version <= 255);
    t.expect_version = t.check_version ? (uint8_t)expect_version : 0;

    if (!copy_str(t.cmd_id,  sizeof(t.cmd_id),  cmd_id) ||
        !copy_str(t.command, sizeof(t.command), command) ||
        !copy_str(t.object,  sizeof(t.object),  object) ||
        !copy_str(t.source,  sizeof(t.source),  source) ||
        !copy_str(t.module,  sizeof(t.module),  module) ||
        !copy_str(t.key,     sizeof(t.key),     key))
    {
        // 清理已写入的部分内容，保持槽位干净。
        //
        // 必须同时释放大 value 占用的 PSRAM，
        // 否则这条失败路径会泄漏内存。
        value_slot_free(t.value);

        t.type = CONFIG_CMD_NONE;
        t.cancel = false;
        t.value.kind = CONFIG_VALUE_NONE;
        t.cmd_id[0]  = '\0';
        t.command[0] = '\0';
        t.object[0]  = '\0';
        t.source[0]  = '\0';
        t.module[0]  = '\0';
        t.key[0]     = '\0';

        cfg_log("E", "enqueue rejected: parameter too long");
        return CONFIG_ENQUEUE_INVALID;
    }

    t.active = true;
    s_cmd_count++;

    cfg_log("I", "enqueue accepted, pending=%d", (int)s_cmd_count);

    return CONFIG_ENQUEUE_ACCEPTED;
}

// ---- 便捷封装 ----
ConfigEnqueueResult config_cmd_enqueue_query_field(
    const char *cmd_id,
    const char *command,
    const char *object,
    const char *source,
    const char *module,
    const char *key
)
{
    return config_cmd_enqueue(
        CONFIG_CMD_QUERY_FIELD,
        cmd_id, command, object, source,
        module, key,
        JsonVariantConst(),
        false,
        false,      // keys_only: 单字段查询不支持
        -1          // 不校验版本
    );
}

ConfigEnqueueResult config_cmd_enqueue_query_module(
    const char *cmd_id,
    const char *command,
    const char *object,
    const char *source,
    const char *module,
    bool keys_only
)
{
    return config_cmd_enqueue(
        CONFIG_CMD_QUERY_MODULE,
        cmd_id, command, object, source,
        module, nullptr,
        JsonVariantConst(),
        false,
        keys_only,
        -1
    );
}

ConfigEnqueueResult config_cmd_enqueue_query_all(
    const char *cmd_id,
    const char *command,
    const char *object,
    const char *source,
    bool keys_only
)
{
    return config_cmd_enqueue(
        CONFIG_CMD_QUERY_ALL,
        cmd_id, command, object, source,
        nullptr, nullptr,
        JsonVariantConst(),
        false,
        keys_only,
        -1
    );
}

ConfigEnqueueResult config_cmd_enqueue_set_field(
    const char *cmd_id,
    const char *command,
    const char *object,
    const char *source,
    const char *module,
    const char *key,
    JsonVariantConst value,
    int expect_version
)
{
    return config_cmd_enqueue(
        CONFIG_CMD_SET_FIELD,
        cmd_id, command, object, source,
        module, key,
        value,
        false,
        false,
        expect_version
    );
}

ConfigEnqueueResult config_cmd_enqueue_save(
    const char *cmd_id,
    const char *command,
    const char *object,
    const char *source
)
{
    return config_cmd_enqueue(
        CONFIG_CMD_SAVE,
        cmd_id, command, object, source,
        nullptr, nullptr,
        JsonVariantConst(),
        false,
        false,
        -1
    );
}

ConfigEnqueueResult config_cmd_enqueue_restart(
    const char *cmd_id,
    const char *command,
    const char *object,
    const char *source,
    bool cancel
)
{
    return config_cmd_enqueue(
        CONFIG_CMD_RESTART,
        cmd_id, command, object, source,
        nullptr, nullptr,
        JsonVariantConst(),
        cancel,
        false,
        -1
    );
}

ConfigEnqueueResult config_cmd_enqueue_delete_field(
    const char *cmd_id,
    const char *command,
    const char *object,
    const char *source,
    const char *module,
    const char *key,
    int expect_version
)
{
    return config_cmd_enqueue(
        CONFIG_CMD_DELETE_FIELD,
        cmd_id, command, object, source,
        module, key,
        JsonVariantConst(),
        false,
        false,
        expect_version
    );
}

ConfigEnqueueResult config_cmd_enqueue_set_module(
    const char *cmd_id,
    const char *command,
    const char *object,
    const char *source,
    const char *module,
    JsonVariantConst value,
    int expect_version
)
{
    return config_cmd_enqueue(
        CONFIG_CMD_SET_MODULE,
        cmd_id, command, object, source,
        module, nullptr,
        value,
        false,
        false,
        expect_version
    );
}

ConfigEnqueueResult config_cmd_enqueue_reset_module(
    const char *cmd_id,
    const char *command,
    const char *object,
    const char *source,
    const char *module
)
{
    return config_cmd_enqueue(
        CONFIG_CMD_RESET_MODULE,
        cmd_id, command, object, source,
        module, nullptr,
        JsonVariantConst(),
        false,
        false,
        -1
    );
}

ConfigEnqueueResult config_cmd_enqueue_backup_module(
    const char *cmd_id,
    const char *command,
    const char *object,
    const char *source,
    const char *module
)
{
    return config_cmd_enqueue(
        CONFIG_CMD_BACKUP_MODULE,
        cmd_id, command, object, source,
        module, nullptr,
        JsonVariantConst(),
        false,
        false,
        -1
    );
}

// ---- 队列状态 ----
size_t config_cmd_queue_capacity()
{
    return CONFIG_CMD_QUEUE_SIZE;
}

size_t config_cmd_queue_pending()
{
    return s_cmd_count;
}

// ---- 完成回调注册 ----
void config_set_completion_callback(
    ConfigCommandCompletionCallback callback
)
{
    s_completion_cb = callback;
}

// =====================================================
// 重启管理
//
// V2 迁移: 本模块不再执行 ESP.restart()。
//   - 真正的重启窗口由 SystemCommand 持有，这里优先反映它的状态
//   - 本模块只额外报告自己的 5 分钟自动重启倒计时
// =====================================================
bool config_restart_pending(unsigned long &remain_ms)
{
    // SystemCommand 的重启窗口优先 —— 它时间更近、更紧急。
    //
    // 若只检查 5 分钟倒计时，那么
    // "已请求重启、即将进入 10 秒窗口" 会被错误地报成"无待重启"。
    if (system_command_restart_pending(remain_ms))
    {
        return true;
    }

    if (s_restart_since_ms == 0)
    {
        remain_ms = 0;
        return false;
    }

    unsigned long elapsed = millis() - s_restart_since_ms;

    if (elapsed >= CONFIG_RESTART_TIMEOUT_MS)
    {
        remain_ms = 0;
    }
    else
    {
        remain_ms = CONFIG_RESTART_TIMEOUT_MS - elapsed;
    }

    return true;
}

bool config_restart_now()
{
    // 语义变更（V2）: 不再立即重启。
    //
    // 全系统唯一的重启执行点是 SystemCommand。本函数只发出请求，
    // 实际重启会等 Critical Operation 归零并经过 10s 安全窗口后发生。
    // 调用方若需要"确认重启已发生"，应观察 system.restart_status。
    cfg_log("W", "restart requested by caller, delegating to SystemCommand");

    return system_command_request_restart();
}

void config_cancel_restart()
{
    // 只取消本模块自己的 5 分钟自动重启倒计时。
    //
    // 注意: 已经交给 SystemCommand 的 Restart 请求【无法】取消
    //（需求文档 §13：Restart 不可取消，只能被 Critical Operation 延迟）。
    s_restart_since_ms = 0;
}

// =====================================================
// 现有业务接口实现
//
// 名称与默认值语义保持不变，内部改为访问模块化缓存。
// =====================================================

// ---------------- WiFi ----------------
String config_get_wifi_ssid()
{
    return module_ro(CONFIG_MODULE_WIFI)["ssid"] | String("");
}

String config_get_wifi_password()
{
    return module_ro(CONFIG_MODULE_WIFI)["password"] | String("");
}

int config_get_wifi_connect_timeout()
{
    return module_ro(CONFIG_MODULE_WIFI)["connect_timeout"] | 30000;
}

int config_get_wifi_reconnect_interval()
{
    return module_ro(CONFIG_MODULE_WIFI)["reconnect_interval"] | 10000;
}

// ---------------- OLED ----------------
int config_get_oled_sda()
{
    return module_ro(CONFIG_MODULE_OLED)["sda"] | 4;
}

int config_get_oled_scl()
{
    return module_ro(CONFIG_MODULE_OLED)["scl"] | 5;
}

int config_get_oled_speed()
{
    return module_ro(CONFIG_MODULE_OLED)["i2c_speed"] | 100000;
}

int config_get_oled_frame_delay()
{
    return module_ro(CONFIG_MODULE_OLED)["frame_delay"] | 50;
}

int config_get_oled_rotation()
{
    return module_ro(CONFIG_MODULE_OLED)["rotation"] | 0;
}

// ---------------- Valve ----------------
int config_get_valve_gpio_pin()
{
    return module_ro(CONFIG_MODULE_VALVE)["gpio_pin"] | -1;
}

int config_get_valve_active_level()
{
    return module_ro(CONFIG_MODULE_VALVE)["active_level"] | 1;
}

int config_get_valve_open_duration_ms()
{
    return module_ro(CONFIG_MODULE_VALVE)["open_duration_ms"] | 0;
}

bool config_get_valve_enable()
{
    return module_ro(CONFIG_MODULE_VALVE)["enable"] | true;
}

int config_get_valve_safety_timeout_sec()
{
    return module_ro(CONFIG_MODULE_VALVE)["safety_timeout_sec"] | 300;
}

// ---------------- 时间 ----------------
// 修正：timezone 归 time 模块（原实现读的是空的 system 分组，
// 实际恒返回默认值 8）
int config_get_timezone()
{
    return module_ro(CONFIG_MODULE_TIME)["timezone"] | 8;
}

String config_get_ntp_server1()
{
    return module_ro(CONFIG_MODULE_TIME)["ntp_server1"]
           | String("ntp.aliyun.com");
}

String config_get_ntp_server2()
{
    return module_ro(CONFIG_MODULE_TIME)["ntp_server2"]
           | String("cn.pool.ntp.org");
}

int config_get_ntp_sync_interval_sec()
{
    return module_ro(CONFIG_MODULE_TIME)["sync_interval_sec"] | 86400;
}

// ---------------- PCF8563T RTC ----------------
//
// 与 OLED 共用同一组 I2C Bus，默认引脚与 oled.json 保持一致。
// RTC 模块只描述"芯片怎么接"，不描述时间怎么用。
bool config_get_rtc_enable()
{
    return module_ro(CONFIG_MODULE_RTC)["enable"] | true;
}

int config_get_rtc_sda()
{
    return module_ro(CONFIG_MODULE_RTC)["sda"] | 4;
}

int config_get_rtc_scl()
{
    return module_ro(CONFIG_MODULE_RTC)["scl"] | 5;
}

int config_get_rtc_i2c_addr()
{
    return module_ro(CONFIG_MODULE_RTC)["i2c_addr"] | 0x51;
}

int config_get_rtc_calibrate_threshold_sec()
{
    return module_ro(CONFIG_MODULE_RTC)["calibrate_threshold_sec"] | 2;
}

// ---------------- HX711 称重 ----------------
int config_get_weight_dt()
{
    return module_ro(CONFIG_MODULE_WEIGHT)["dt"] | 4;
}

int config_get_weight_sck()
{
    return module_ro(CONFIG_MODULE_WEIGHT)["sck"] | 5;
}

int config_get_weight_sample_interval()
{
    return module_ro(CONFIG_MODULE_WEIGHT)["sample_interval"] | 50;
}

float config_get_weight_scale()
{
    return module_ro(CONFIG_MODULE_WEIGHT)["scale"] | 741.0;
}

long config_get_weight_zero_offset()
{
    return module_ro(CONFIG_MODULE_WEIGHT)["zero_offset"] | 0;
}

int config_get_weight_filter_samples()
{
    return module_ro(CONFIG_MODULE_WEIGHT)["filter_samples"] | 10;
}

// 修正：原实现声明返回 bool 却没有 return 语句（未定义行为），
// 而 weight.cpp 会依据该返回值判定校准是否成功。
//
// 重启策略：与其他配置修改保持一致，schedule_restart = true。
//
// 系统模块之间存在初始化依赖（Action / Trigger → Workflow 等），
// ConfigManager 不判断"某个参数是否只需刷新 RAM"，
// 统一按"保存后重启"处理，以换取清晰的安全边界。
// 开发者新增配置参数时无需判断是否需要重启，走默认即可。
bool config_set_weight_zero_offset(long offset)
{
    ConfigModule *m = nullptr;

    if (!set_begin(CONFIG_MODULE_WEIGHT, true, m))
    {
        return false;
    }

    if (!m->data["zero_offset"].set(offset))
    {
        cfg_log("E", "set zero_offset failed");
        return false;
    }

    return true;
}

// ---------------- MQTT ----------------
String config_get_mqtt_server()
{
    return module_ro(CONFIG_MODULE_MQTT)["mqtt_server"] | String("");
}

int config_get_mqtt_port()
{
    return module_ro(CONFIG_MODULE_MQTT)["mqtt_port"] | 8883;
}

String config_get_mqtt_client_id()
{
    return module_ro(CONFIG_MODULE_MQTT)["client_id"] | String("");
}

String config_get_mqtt_subscribe_topic()
{
    return module_ro(CONFIG_MODULE_MQTT)["subscribe_topic"] | String("");
}

String config_get_mqtt_username()
{
    return module_ro(CONFIG_MODULE_MQTT)["username"] | String("");
}

String config_get_mqtt_password()
{
    return module_ro(CONFIG_MODULE_MQTT)["password"] | String("");
}

String config_get_mqtt_publish_topic()
{
    return module_ro(CONFIG_MODULE_MQTT)["publish_topic"]
           | String("guo_feeder/up");
}

String config_get_mqtt_ca_path()
{
    return module_ro(CONFIG_MODULE_MQTT)["ca_path"]
           | String("/emqxsl-ca.crt");
}

unsigned long config_get_mqtt_retry_interval()
{
    return module_ro(CONFIG_MODULE_MQTT)["retry_interval"] | 8000;
}

int config_get_mqtt_retry_max()
{
    return module_ro(CONFIG_MODULE_MQTT)["retry_max"] | 30;
}

unsigned long config_get_mqtt_sleep_interval()
{
    return module_ro(CONFIG_MODULE_MQTT)["sleep_retry_interval"] | 3600000;
}

int config_get_mqtt_keep_alive()
{
    return module_ro(CONFIG_MODULE_MQTT)["mqtt_keep_alive"] | 60;
}

// ---------------- WiFi 配置更新 ----------------
// CommandManager 已通过 command_system_wifi_config 接入本接口。
bool config_update_wifi(
    const String &ssid,
    const String &password
)
{
    if (ssid.length() == 0)
    {
        cfg_log("E", "update_wifi: empty ssid");
        return false;
    }

    ConfigModule *m = nullptr;

    if (!set_begin(CONFIG_MODULE_WIFI, true, m))
    {
        return false;
    }

    if (!m->data["ssid"].set(ssid) ||
        !m->data["password"].set(password))
    {
        cfg_log("E", "update_wifi: assign failed");
        return false;
    }

    m->dirty = true;

    return true;
}

// ---------------- 米家温湿度计 ----------------
void config_get_mithermometer_blekey(char *buf, size_t buf_size)
{
    if (buf == nullptr || buf_size == 0)
    {
        return;
    }

    buf[0] = '\0';

    config_get_string(
        CONFIG_MODULE_MITHERMO,
        "BLE_key",
        buf,
        buf_size
    );
}

void config_get_mithermometer_mac(char *buf, size_t buf_size)
{
    if (buf == nullptr || buf_size == 0)
    {
        return;
    }

    buf[0] = '\0';

    config_get_string(
        CONFIG_MODULE_MITHERMO,
        "MAC",
        buf,
        buf_size
    );
}

// 修正：该字段位于 mi_thermo 模块内（原实现读的是顶层，
// 实际恒返回默认值 true）
bool config_get_mi_thermo_allow_collect()
{
    return module_ro(CONFIG_MODULE_MITHERMO)["mi_thermo_allow_collect"]
           | true;
}
