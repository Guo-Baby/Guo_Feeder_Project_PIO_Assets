#include <Arduino.h>
#include <LittleFS.h>
#include <NimBLEDevice.h>
#include "services/system_state.h"
#include "services/system_command.h"
#include "storage/json_storage.h"
#include "storage/file_storage.h"
#include "storage/bin_storage.h"
#include "automation/workflow_storage.h"
#include "services/config_manager.h"
#include "services/device_identity.h"
#include "services/event_manager.h"
#include "app/oled.h"
#include "services/command_manager.h"
#include "services/wifi_module.h"
#include "services/time_manager.h"
#include "cloud/cloud_manager.h"
#include "app/weight.h"
#include "app/valve.h"
#include "app/computer_reset.h"
#include "automation/workflow.h"
#include "app/dispense_guard.h"
#include "test/test_mqtt.h"
#include "automation/capability_registry.h"
#include "app/MiThermometer.h"
// LogManager P1.1 冻结契约（仅契约 + 编译期 static_assert，不含实现）
//
// 必须被某个编译单元包含，否则 log_events.h 里的 static_assert 不会被求值。
// 此处包含即完成 P1.1 的 Contract Test（编译期校验）。
#include "log/log_events.h"
#include "log/log_manager.h"

// CommandManager 日志 / 结果回显 → 串口
//
// CommandManager 默认静默（与 BinStorage 一样只留回调接口）。
// 注册后可以看到命令路由过程；配合 command_manager_set_result_echo()
// 还能在串口直通（cm 命令）时看到完整的结果 JSON。
static void command_log_serial(const char *level, const char *message)
{
    Serial.printf("[CMD][%s] %s\n", level, message);
}

// =====================================================
// P2-A：Storage 日志回调 → LogManager 桥接
//
// 背景：json_storage / file_storage 各自带回调接口，但**从未被注册**，
//       其 67 处 E/W（json 30E+7W、file 24E+6W）完全静默丢弃。
//       本桥接把它们接入 LogManager，同时保留串口输出（双路）。
//
// 只调用 log_emit()；不触碰 log_flash_* / log_meta_* / log_seg_* / log_cloud_test_*。
//
// 语义还原的限制（重要）：
//   回调只给「自由文本」（level + message），无法还原精确事件语义。
//   ⇒ EventId 由 message 的**前缀 op 词**分类；op 明细写入 LOG_P_ERR_CODE
//   ⇒ LOG_P_MODULE 区分 json(0) / file(1) / bin(2)
//   ⇒ 不做字符串参数（P2 已定版：字符串一律哈希/枚举化，不扩 LogManager API）
//   ⇒ 若日后需要精确定位，应把回调改成结构化（level + op + path_hash + err），
//     属 API 变更，需单独评审（见 docs/specs/LogManager-Integration-Guide.md §Storage）
//
// 高频抑制：
//   同一 (module, op) 在 STG_BRIDGE_DEDUP_MS 窗口内只上报 1 条，其余累加计数，
//   下个窗口用 LOG_P_COUNT 上报。防止"FS 不可用"时按调用次数刷屏。
//
// 实现约束：纯静态表、零堆分配、无 String、无阻塞、无 ISR 调用。
// =====================================================

#define STG_BRIDGE_MODULE_JSON   0u
#define STG_BRIDGE_MODULE_FILE   1u
#define STG_BRIDGE_MODULE_BIN    2u   // Phase 6-A：bin_storage（Workflow BIN 持久化底座）
#define STG_BRIDGE_MODULE_COUNT  3u

// op 分类（写入 LOG_P_ERR_CODE；本桥接私有枚举，不是冻结 ParamId）
enum StgBridgeOp : uint8_t
{
    STG_OP_UNKNOWN = 0,
    STG_OP_FS_UNAVAILABLE,   // LittleFS 未挂载（挂载失败）
    STG_OP_NOT_INITIALIZED,  // 模块尚未 init
    STG_OP_MKDIR,
    STG_OP_REMOVE,
    STG_OP_RENAME,
    STG_OP_READ,
    STG_OP_WRITE,
    STG_OP_CRC32,
    STG_OP_VERIFY,
    STG_OP_FOREACH,
    STG_OP_EXISTS,
    STG_OP_SIZE,
    STG_OP_RECOVER,
    STG_OP_SEEK_TRUNC,
    STG_OP_COUNT
};

// 同一 (module, op) 的上报抑制窗口
#define STG_BRIDGE_DEDUP_MS 10000u

static unsigned long s_stg_last_ms[STG_BRIDGE_MODULE_COUNT][STG_OP_COUNT];
static uint16_t      s_stg_suppressed[STG_BRIDGE_MODULE_COUNT][STG_OP_COUNT];

// message → op（定长前缀匹配，不构造 String）
static StgBridgeOp stg_bridge_classify(const char *msg)
{
    if (msg == nullptr)
        return STG_OP_UNKNOWN;

    // 更具体的"原因"优先于"操作名"
    if (strncmp(msg, "LittleFS", 8) == 0)  return STG_OP_FS_UNAVAILABLE;
    // Phase 6-A：bin_storage 的级联 FS 失败措辞是 "init: FileStorage unavailable"
    // （json/file_storage 用 "LittleFS unavailable (not mounted)" ⇒ 已被上面命中）
    // 原因是"FS 不可用"而非"写失败" ⇒ 归类到 FS 不可用，避免落入 UNKNOWN 的严重度兜底
    if (strstr(msg, "unavailable"))        return STG_OP_FS_UNAVAILABLE;
    if (strstr(msg, "not initialized"))    return STG_OP_NOT_INITIALIZED;
    // Phase 6-A：显式"校验失败"措辞归到 WRITE_VERIFY_FAILED（对应映射表第 ④ 条）
    //   受影响消息："write: size verify failed"（bin）、"atomic: final verify failed"（json/file/bin）
    //   ⚠️ 这会同时把 json_storage 的 "atomic: final verify failed" 从
    //      兜底的 ATOMIC_WRITE_FAILED 改为更精确的 WRITE_VERIFY_FAILED（Level 不变，仍为 ERROR）
    if (strstr(msg, "verify failed"))      return STG_OP_VERIFY;

    if (strncmp(msg, "mkdir", 5) == 0)     return STG_OP_MKDIR;
    if (strncmp(msg, "remove", 6) == 0)    return STG_OP_REMOVE;
    if (strncmp(msg, "rename", 6) == 0)    return STG_OP_RENAME;
    if (strncmp(msg, "read", 4) == 0)      return STG_OP_READ;
    if (strncmp(msg, "write", 5) == 0)     return STG_OP_WRITE;
    if (strncmp(msg, "crc32", 5) == 0)     return STG_OP_CRC32;
    if (strncmp(msg, "verify", 6) == 0)    return STG_OP_VERIFY;
    if (strncmp(msg, "foreach", 7) == 0)   return STG_OP_FOREACH;
    if (strncmp(msg, "exists", 6) == 0)    return STG_OP_EXISTS;
    if (strncmp(msg, "size", 4) == 0)      return STG_OP_SIZE;
    if (strncmp(msg, "recover", 7) == 0)   return STG_OP_RECOVER;
    if (strncmp(msg, "seek", 4) == 0)      return STG_OP_SEEK_TRUNC;
    if (strncmp(msg, "truncate", 8) == 0)  return STG_OP_SEEK_TRUNC;

    return STG_OP_UNKNOWN;
}

// op → 冻结 EventId（全部取自 Storage 段 0x03，未新增任何 ID）
static LogEventId stg_bridge_event(StgBridgeOp op, bool is_error)
{
    switch (op)
    {
        case STG_OP_FS_UNAVAILABLE:
        case STG_OP_NOT_INITIALIZED:
            return LOG_STG_FS_UNAVAILABLE;

        case STG_OP_CRC32:
            return LOG_STG_CRC_FAILED;

        case STG_OP_VERIFY:
            return LOG_STG_WRITE_VERIFY_FAILED;

        case STG_OP_RECOVER:
            return LOG_STG_TXN_RECOVERED;

        case STG_OP_RENAME:
        case STG_OP_WRITE:
        case STG_OP_MKDIR:
        case STG_OP_REMOVE:
            return LOG_STG_ATOMIC_WRITE_FAILED;

        case STG_OP_READ:
        case STG_OP_FOREACH:
        case STG_OP_EXISTS:
        case STG_OP_SIZE:
        case STG_OP_SEEK_TRUNC:
            return LOG_STG_READ_FAILED;

        case STG_OP_UNKNOWN:
        default:
            // 兜底：op 未识别时按严重度选读写侧；真实语义看 LOG_P_ERR_CODE
            return is_error ? LOG_STG_ATOMIC_WRITE_FAILED : LOG_STG_READ_FAILED;
    }
}

// 统一入口：只上报 'E' / 'W'，'I'（Storage 仅 "ready" 一条）不上报
static void stg_bridge_emit(uint8_t module, const char *level, const char *message)
{
    if (level == nullptr || module >= STG_BRIDGE_MODULE_COUNT)
        return;

    const bool is_error = (level[0] == 'E');
    const bool is_warn  = (level[0] == 'W');
    if (!is_error && !is_warn)
        return;

    const StgBridgeOp op = stg_bridge_classify(message);

    // ---- 抑制：同一 (module, op) 每 STG_BRIDGE_DEDUP_MS 只发 1 条 ----
    const unsigned long now  = millis();
    const unsigned long last = s_stg_last_ms[module][op];

    if (last != 0 && (now - last) < STG_BRIDGE_DEDUP_MS)
    {
        if (s_stg_suppressed[module][op] < 0xFFFFu)
        {
            s_stg_suppressed[module][op]++;
        }
        return;
    }
    s_stg_last_ms[module][op] = now;

    const uint32_t suppressed = (uint32_t)s_stg_suppressed[module][op];
    s_stg_suppressed[module][op] = 0;

    // 严重度：跟随回调的 level；但"LittleFS 未挂载"按冻结契约提升为 CRITICAL
    //（LOG_STG_FS_UNAVAILABLE 在契约中即标注 CRITICAL：数据有损坏风险，触发立即 flush）
    LogLevel lv = is_error ? LOG_LVL_ERROR : LOG_LVL_WARN;
    if (op == STG_OP_FS_UNAVAILABLE)
    {
        lv = LOG_LVL_CRITICAL;
    }

    LogParamIn a[3];
    uint8_t n = 0;
    a[n++] = log_arg_u32(LOG_P_MODULE,   (uint32_t)module);
    a[n++] = log_arg_u32(LOG_P_ERR_CODE, (uint32_t)op);
    if (suppressed > 0)
    {
        a[n++] = log_arg_u32(LOG_P_COUNT, suppressed);
    }

    log_emit(stg_bridge_event(op, is_error), lv, a, n);
}

// ---- 两个模块的桥接回调（双路：串口 + LogManager）----

static void json_storage_log_bridge(const char *level, const char *message)
{
    Serial.printf("[JStg][%s] %s\n",
                  level   ? level   : "?",
                  message ? message : "");
    stg_bridge_emit(STG_BRIDGE_MODULE_JSON, level, message);
}

static void file_storage_log_bridge(const char *level, const char *message)
{
    Serial.printf("[FStg][%s] %s\n",
                  level   ? level   : "?",
                  message ? message : "");
    stg_bridge_emit(STG_BRIDGE_MODULE_FILE, level, message);
}

// ---- Phase 6-A：BinStorage 桥接（与上两个同构）
//
// 背景：bin_storage 的 bin_log() 共 23 个站点（E16 / W6 / I1），此前只注册了
//       串口回调 ⇒ 其 E/W 从未上云。bin_storage 是 Workflow BIN 持久化的底座
//       （workflow_storage 的 24 处调用全部经它落盘），其失败此前不可观测。
//
// 本桥接不新增任何 EventId：分类复用 P2-A 的 stg_bridge_classify()/stg_bridge_event()，
// 只用 STG_BRIDGE_MODULE_BIN 这个新的 LOG_P_MODULE 取值区分来源。
// 与 json/file 的差别是"消息措辞"（如 "atomic:" / "init: … unavailable"），
// 未识别的前缀按既有 UNKNOWN 兜底（不新增分类、不新增 ID）。
static void bin_log_bridge(const char *level, const char *message)
{
    Serial.printf("[Bin][%s] %s\n",
                  level   ? level   : "?",
                  message ? message : "");
    stg_bridge_emit(STG_BRIDGE_MODULE_BIN, level, message);
}

// =====================================================
// P2-B：ConfigManager 日志回调 → LogManager 桥接
//
// 与 Storage bridge（P2-A）同构，但有两点关键差别：
//
//   ① **ConfigManager 的 cfg_log() 在未注册回调时会兜底打串口**
//      （config_manager.cpp:220）。一旦注册了回调，兜底分支被跳过 ⇒ 桥接**必须
//      自己补串口输出**，否则原有日志会突然"消失"（行为回退）。
//      本桥接因此是"串口 + LogManager"双路。
//
//   ② 与 Storage 不同，这里**允许 INFO**：Config 的 INFO 承载真实诊断信息
//      （`init done, loaded 8/8 modules`、`boot validated`），且频率极低。
//
// 只调用 log_emit()；不触碰 log_flash_* / log_meta_* / log_seg_* / log_cloud_test_*。
//
// ★ 与"显式语义埋点"的分工（**必须遵守，否则重复上报**）：
//   config_manager.cpp 内已显式补了 2 个 cfg_log 里**没有对应文本**的事件：
//     · `exec_set_module()` 成功 → LOG_CFG_CHANGE_APPLIED（带 module/key 哈希）
//     · `config_save()` 成功     → LOG_CFG_SAVE_OK
//   因此本桥接**不**映射这两类文本：
//     · `enqueue accepted, pending=%d` → SKIP（入队 ≠ 生效；生效点由上面负责）
//     · `restart requested by caller, delegating to SystemCommand` → SKIP
//       （重启请求归 System 段 `LOG_SYS_RESTART_REQUESTED`，避免跨段重复）
//
// 语义还原的固有限制：cfg_log 只给自由文本 ⇒ EventId 靠**关键词**分类，
// op 明细写入 LOG_P_ERR_CODE；无法匹配的落"等级兜底桶"。真实语义看串口原文。
// 要彻底解决需把回调改成结构化（level + event_hint + module_hash + err_code），
// 属 API 变更，需单独评审（对应 Integration-Guide §7.2 F-2）。
// =====================================================

// op 分类（写入 LOG_P_ERR_CODE；本桥接私有枚举，不是冻结 ParamId）
enum CfgBridgeOp : uint8_t
{
    CFG_OP_UNKNOWN = 0,
    CFG_OP_LOAD_DONE,             // init done / boot validated
    CFG_OP_LOAD_FAILED,           // 模块加载/解析/序列化失败等地带
    CFG_OP_RECOVERED_BACKUP,      // loaded from backup / recovered from backup
    CFG_OP_COMMIT_FAILED_ROLLBACK,// 提交/回滚/原子写轮转失败（含 save 失败）
    CFG_OP_VERSION_REBUILT,       // version 文件缺失/损坏/重建
    CFG_OP_WRITE_REJECTED,        // 写请求被拒
    CFG_OP_RESTART_TIMEOUT,       // 重启倒计时超时（保存后重启）
    CFG_OP_SKIP,                  // 明确不由本桥接上报（见文件头说明）
    CFG_OP_COUNT
};

#define CFG_BRIDGE_DEDUP_MS 10000u

static unsigned long s_cfg_last_ms[CFG_OP_COUNT];
static uint16_t      s_cfg_suppressed[CFG_OP_COUNT];

// message → op（关键词匹配；顺序即优先级，先具体后笼统）
static CfgBridgeOp cfg_bridge_classify(const char *msg)
{
    if (msg == nullptr)
        return CFG_OP_UNKNOWN;

    // ---- 明确 skip：由其它机制/其它段负责 ----
    if (strstr(msg, "delegating to SystemCommand"))  return CFG_OP_SKIP;
    if (strstr(msg, "enqueue accepted"))             return CFG_OP_SKIP;

    // ---- 具体事件 ----
    if (strstr(msg, "from backup"))                  return CFG_OP_RECOVERED_BACKUP;
    if (strstr(msg, "restart timeout"))              return CFG_OP_RESTART_TIMEOUT;
    if (strstr(msg, "init done"))                    return CFG_OP_LOAD_DONE;
    if (strstr(msg, "boot validated"))                return CFG_OP_LOAD_DONE;
    if (strstr(msg, "version"))                      return CFG_OP_VERSION_REBUILT;

    // ---- 被拒 ----
    if (strstr(msg, "rejected"))                     return CFG_OP_WRITE_REJECTED;
    if (strstr(msg, "set failed"))                   return CFG_OP_WRITE_REJECTED;

    // ---- 提交 / 回滚 / 原子写 ----
    if (strstr(msg, "rotate to backup"))             return CFG_OP_COMMIT_FAILED_ROLLBACK;
    if (strstr(msg, "write active"))                 return CFG_OP_COMMIT_FAILED_ROLLBACK;
    if (strstr(msg, "commit"))                       return CFG_OP_COMMIT_FAILED_ROLLBACK;
    if (strstr(msg, "rollback"))                     return CFG_OP_COMMIT_FAILED_ROLLBACK;
    if (strstr(msg, "save failed"))                  return CFG_OP_COMMIT_FAILED_ROLLBACK;
    if (strstr(msg, "save:"))                        return CFG_OP_COMMIT_FAILED_ROLLBACK;

    return CFG_OP_UNKNOWN;
}

// op → 冻结 EventId（全部取自 Config 段 0x02，未新增任何 ID）
static LogEventId cfg_bridge_event(CfgBridgeOp op, bool is_error)
{
    switch (op)
    {
        case CFG_OP_LOAD_DONE:               return LOG_CFG_LOAD_DONE;
        case CFG_OP_LOAD_FAILED:             return LOG_CFG_MODULE_LOAD_FAILED;
        case CFG_OP_RECOVERED_BACKUP:        return LOG_CFG_RECOVERED_FROM_BACKUP;
        case CFG_OP_COMMIT_FAILED_ROLLBACK:  return LOG_CFG_COMMIT_FAILED_ROLLBACK;
        case CFG_OP_VERSION_REBUILT:         return LOG_CFG_VERSION_REBUILT;
        case CFG_OP_WRITE_REJECTED:          return LOG_CFG_WRITE_REJECTED;
        case CFG_OP_RESTART_TIMEOUT:         return LOG_CFG_RESTART_TIMEOUT;

        case CFG_OP_UNKNOWN:
        default:
            // 兜底：无法分类时按严重度落到该等级下最保守的 Config 事件。
            // 真实语义必须看串口原文与 LOG_P_ERR_CODE，**不要**依赖此处的事件名。
            if (is_error)
            {
                return LOG_CFG_MODULE_LOAD_FAILED;
            }
            // ⚠️ INFO 的兜底分类不可靠（`critical op acquired` / `restart timer
            //    refreshed` 这类文本没有对应的冻结事件）⇒ **不上报**，只在串口保留。
            //    用 cfg_bridge_emit 里的 CFG_OP_SKIP 分支拦截。
            return LOG_CFG_WRITE_REJECTED;
    }
}

// 统一入口：'E' / 'W' / 'I' 都上报；CFG_OP_SKIP 不发射
static void cfg_bridge_emit(const char *level, const char *message)
{
    if (level == nullptr)
        return;

    const bool is_error = (level[0] == 'E');
    const bool is_warn  = (level[0] == 'W');
    const bool is_info  = (level[0] == 'I');

    if (!is_error && !is_warn && !is_info)
        return;

    const CfgBridgeOp op = cfg_bridge_classify(message);

    if (op == CFG_OP_SKIP)
        return;

    // 未分类的 INFO 不上报：兜底桶在 INFO 上没有语义正确的落点
    //（`critical op acquired` / `restart timer refreshed` 等），错标比不报更糟。
    // 串口仍保留原文。E/W 则必须上报（宁可标签粗，也不能静默）。
    if (op == CFG_OP_UNKNOWN && is_info)
        return;

    // ---- 抑制：同一 op 每 CFG_BRIDGE_DEDUP_MS 只发 1 条 ----
    const unsigned long now  = millis();
    const unsigned long last = s_cfg_last_ms[op];

    if (last != 0 && (now - last) < CFG_BRIDGE_DEDUP_MS)
    {
        if (s_cfg_suppressed[op] < 0xFFFFu)
        {
            s_cfg_suppressed[op]++;
        }
        return;
    }
    s_cfg_last_ms[op] = now;

    const uint32_t suppressed = (uint32_t)s_cfg_suppressed[op];
    s_cfg_suppressed[op] = 0;

    // 等级跟随回调字符（ConfigManager 已按语义区分 E/W/I）
    const LogLevel lv = is_error ? LOG_LVL_ERROR
                      : (is_warn ? LOG_LVL_WARN : LOG_LVL_INFO);

    LogParamIn a[2];
    uint8_t n = 0;
    a[n++] = log_arg_u32(LOG_P_ERR_CODE, (uint32_t)op);
    if (suppressed > 0)
    {
        a[n++] = log_arg_u32(LOG_P_COUNT, suppressed);
    }

    log_emit(cfg_bridge_event(op, is_error), lv, a, n);
}

// 桥接回调：双路（串口 + LogManager）。串口这一路是**必须**的 —— 见文件头 ①。
static void config_log_bridge(const char *level, const char *message)
{
    Serial.printf("[CFG][%s] %s\n",
                  level   ? level   : "?",
                  message ? message : "");
    cfg_bridge_emit(level, message);
}

// =====================================================
// setup
// =====================================================
void setup()
{
    const unsigned long setup_begin_ms = millis();

    Serial.begin(115200);
    delay(1000);
    // =====================================================
    // 第一层：基础系统（无依赖）
    // =====================================================
    // ===== 一次性挂载 LittleFS =====
    if (!LittleFS.begin(true, "/littlefs", 10, "littlefs")) {
        Serial.println("[System] LittleFS mount failed!");
        // 根据你的错误处理策略，可以选择重启或继续
    } else {
        Serial.println("[System] LittleFS mounted");
    }
    Serial.printf("PSRAM size: %u\n", ESP.getPsramSize());
    Serial.printf("PSRAM free: %u\n", ESP.getFreePsram());
    // LogManager P1.1：冻结契约校验（值全部来自 log_events.h 的编译期常量）
    Serial.printf(
        "[LogContract] v=%u rec=%u seg=%u x%u cap=%u fmt=%u check=%s\n",
        (unsigned)LOG_RECORD_VERSION,
        (unsigned)LOG_RECORD_SIZE,
        (unsigned)LOG_SEGMENT_SIZE,
        (unsigned)LOG_SEGMENT_COUNT,
        (unsigned)LOG_SEGMENT_CAPACITY,
        (unsigned)LOG_BATCH_FMT,
        log_contract_self_check() ? "PASS" : "FAIL");
    // ===== LogManager P1.2：RAM 环（PSRAM 优先，失败回退 DRAM）=====
    //
    // 只分配 RAM 环，不写 Flash、不发 MQTT。
    // 必须在 loop() 之前完成，保证后续模块可安全调用 log_emit()。
    log_init();
    // ===== P2-A：Storage 日志回调桥接（必须早于各 Storage 模块自己的 init）=====
    //
    // 此前 json_storage / file_storage 的回调从未注册 ⇒ 67 处 E/W 完全静默。
    // 在此注册（log_init 之后、json_storage_init / file_storage_init 之前），
    // 才能把"初始化期"的 Storage 错误也捕获进来。
    //
    // 回调 setter 只做指针赋值，不依赖模块已 init，故可安全前置调用。
    json_storage_set_log_callback(json_storage_log_bridge);
    file_storage_set_log_callback(file_storage_log_bridge);
    // P2-B：ConfigManager 桥接。
    //
    // ConfigManager 的 cfg_log() 在**未注册**时会兜底打串口；注册后兜底分支被跳过
    // ⇒ 桥接自己补串口输出（见 config_log_bridge）。
    // 注册点同样放在 config_init() 之前，才能捕获初始化期的配置错误。
    config_set_log_callback(config_log_bridge);
    // ===== 初始化 JSON Storage（底层文件存储，ConfigManager 依赖它）=====
    if (!json_storage_init()) {
        Serial.println("[System] JsonStorage init failed!");
    }
    // 注册日志回调（Phase 7-1 / DEF-1）：**必须早于 bin_storage_init()**。
    //
    // BinStorage 默认静默，注册后其 E/W 级错误为"串口 + LogManager"双路
    // （Phase 6-A：由 bin_log_serial 换为 bin_log_bridge，串口格式不变）。
    // ★ 此前本行位于 bin_storage_init() **之后** ⇒ init 内部产生的
    //   bin_log("E", "init: FileStorage unavailable")（bin_storage.cpp:153）
    //   因 s_log_cb == nullptr 被 bin_log() 直接 return 丢弃（连串口都不打）
    //   ⇒ 初始化期 ERROR 完全不可观测（Phase 6-A 的 23 站点实际只达 22 站）。
    // ★ 与 json_storage / file_storage 的桥接注册顺序保持一致（见上方 P2-A 段）：
    //   注册先于 init，才能捕获"初始化期"的 Storage 错误。
    // ★ setter 只做指针赋值，不依赖模块已 init，故前置调用安全。
    bin_storage_set_log_callback(bin_log_bridge);
    // ===== 初始化 BIN Storage（底层二进制文件存储，Workflow BIN 持久化依赖它）=====
    //
    // 内部会级联初始化 FileStorage。
    // 与 JsonStorage 平级，两条链路互不干扰：
    //   ConfigManager → JsonStorage → LittleFS
    //   WorkflowManager(后续) → BinStorage → FileStorage → LittleFS
    if (!bin_storage_init()) {
        Serial.println("[System] BinStorage init failed!");
    }
    // ===== 初始化 Workflow Storage（Workflow 定义持久化，依赖 BinStorage）=====
    //
    // 内部会创建 /workflow 目录并加载 meta.bin。
    // 本阶段只初始化存储层，Workflow.cpp 仍走原有 JSON 加载路径，行为不变。
    if (!workflow_storage_init()) {
        Serial.println("[System] WorkflowStorage init failed!");
    }
    system_state_init();
    config_init();
    // P0-1：设备身份（必须在 wifi_init() / cloud_init() 之前；身份不依赖网络）
    device_identity_init();
    event_manager_init();
    // =====================================================
    // 第二层：通信（依赖基础系统）
    // =====================================================
    ble_init();
    wifi_init();
    // =====================================================
    // 第三层：Workflow 框架（依赖基础系统）
    // =====================================================
    workflow_init();
    // =====================================================
    // 第四层：业务模块注册（依赖 workflow_init）
    // =====================================================
    weight_init();
    valve_init();
    computer_reset_init();     // GPIO8 脉冲（电脑重启），注册 COMPUTER_RESET Action
    dispense_guard_init();
    oled_init();
    oled_event_init();
    // ---- TimeManager V2：必须在 oled_init() 之后 ----
    // RTC(PCF8563T) 复用 OLED 建立的 Wire I2C Bus，绝不在本模块重复
    // Wire.begin()；故 time_init() 从第二层移到此处。
    time_init();
    test_mqtt_init();
    bool ok = MiThermometerInit();
    // =====================================================
    // 第五层：命令和云端（依赖业务模块注册完成）
    // =====================================================
    command_manager_init();
    // 注册日志回调：CommandManager 默认静默，注册后可看到路由/错误/结果
    command_manager_set_log_callback(command_log_serial);
    cloud_init();
    // =====================================================
    // 第六层：加载 Workflow 配置（依赖所有注册完成）
    // =====================================================
    // 优先从 Flash BIN 加载（meta.bin + wfNN/stepNN.bin）；
    // Flash 中没有任何 Valid Workflow 时（首次启动）回退到 JSON。
    if (workflow_load_from_storage()) {
        Serial.println("[OK] Workflow loaded from Flash BIN");
    } else if (workflow_load_json_file("/workflow.json")) {
        Serial.println("[OK] Workflow loaded from /workflow.json");
        // JSON → BIN 一次性迁移。
        //
        // 不做迁移的后果：BIN 只保存 Dirty Workflow，若之后只改了一个
        // Workflow，下次启动时 BIN 已 Valid → 只加载这一个，
        // 其余从 JSON 来的 Workflow 会静默丢失。
        if (workflow_migrate_to_storage()) {
            Serial.println("[OK] Workflow migrated JSON -> Flash BIN");
        } else {
            Serial.println("[WARN] Workflow JSON -> BIN migration failed");
        }
    } else {
        Serial.println("[WARN] No workflow.json found");
    }
    // =====================================================
    // 第七层：capability registry（依赖所有注册完成）
    // =====================================================
    capability_registry_init();
    // =====================================================
    // 第八层：启动确认（必须位于 setup 最后）
    //
    // 只有完整走完 setup 才会写入启动标记。
    // 若因配置错误导致 panic / 反复重启，标记不会被写入，
    // 下次启动 ConfigManager 会自动从 Backup 恢复配置。
    // =====================================================
    if (!config_boot_validate()) {
        Serial.println("[WARN] Config boot validate failed");
    } else {
        LogParamIn p[1];
        p[0] = log_arg_u32(LOG_P_INIT_MS, (uint32_t)(millis() - setup_begin_ms));
        log_emit(LOG_SYS_BOOT_COMPLETE, LOG_LVL_INFO, p, 1);
    }
}

void serial_debug_command_process();
// WorkflowStorage 独立测试控制台（wfst 命令，仅上板自测用）
void wfst_console(const String &cmd);
// Workflow CRUD + 回归测试控制台（wfc 命令，仅上板自测用）
void wfc_console(const String &cmd);
// CommandManager 直通控制台（cm 命令，仅上板自测用）
void cm_console(const String &cmd);

// LogManager 上板测试控制台（P1.2）
void logt_console(const String &cmd);
// =====================================================
// loop
// =====================================================
void loop()
{
    // ---- Safe Restart 状态机（SystemCommand）----
    // 必须每轮调用：它是全系统唯一的 Restart 执行点。
    // 放在最前面，保证已进入的 10 秒安全窗口不被其他任务拖长。
    system_command_task();

    // ---- 原有任务（保持不变） ----
    wifi_task();
    event_dispatch();
    time_task();
    cloud_task();

    command_manager_task();
    config_task();            // 配置修改后的自动重启倒计时
    // ---- 新增：Workflow 任务 ----
    workflow_task();          // 执行 Workflow 状态机
    oled_task();
    weight_task();
    valve_task();
    computer_reset_task();     // 手动脉冲回收 + GPIO8 安全兜底
    MiThermometer_task();
    test_mqtt_task();//测试代码，需要删除
    // ---- LogManager P1.2：消费 RAM 环 + Level → (Flash, Cloud) routing 决策 ----
    // 不创建独立 Task（LittleFS 单写者模型）；每轮最多消费 LOG_DRAIN_MAX_PER_TASK 条。
    log_task();
    serial_debug_command_process();
}

//测试代码
//==== 放在 main.cpp 全局区域（不要写在loop内部）====
String serial_cmd_buffer;
//==== 命令解析函数，在loop()中调用 ====
void serial_debug_command_process(void)
{
    while (Serial.available() > 0)
    {
        char ch = Serial.read();
        // 回车 / 换行 触发解析
        if (ch == '\n' || ch == '\r')
        {
            if (serial_cmd_buffer.length() == 0)
            {
                continue;
            }
            serial_cmd_buffer.trim(); // 清除首尾空格
            Serial.print("Recv cmd: [");
            Serial.print(serial_cmd_buffer);
            Serial.println("]");
            if (serial_cmd_buffer == "valve_open")
            {
                bool ok = valve_open();
                Serial.printf("valve_open execute ret = %d\n", ok);
            }
            else if (serial_cmd_buffer == "valve_close")
            {
                bool ok = valve_close();
                Serial.printf("valve_close execute ret = %d\n", ok);
            }
            else if (serial_cmd_buffer == "valve_toggle")
            {
                bool ok = valve_toggle();
                Serial.printf("valve_toggle execute ret = %d\n", ok);
            }
            else if (serial_cmd_buffer == "valve_status")
            {
                Serial.printf("Valve current open state: %d\n", valve_is_open());
            }
            else if (serial_cmd_buffer == "computer_reset")
            {
                // 手动触发一次 800ms 脉冲（绕过 MQTT，用于本地验证 GPIO8）
                bool ok = computer_reset_trigger();
                Serial.printf("computer_reset trigger ret = %d\n", ok);
            }
            else if (serial_cmd_buffer == "computer_reset_status")
            {
                Serial.printf("ComputerReset active state: %d (pin=%d, hold=%lu ms)\n",
                    computer_reset_is_active(),
                    COMPUTER_RESET_PIN,
                    COMPUTER_RESET_HOLD_MS);
            }
            else if (serial_cmd_buffer.startsWith("wfst "))
            {
                // WorkflowStorage 独立测试控制台（仅上板自测用）
                wfst_console(serial_cmd_buffer);
            }
            else if (serial_cmd_buffer.startsWith("cm "))
            {
                // CommandManager 直通（走完整路由链，含结果 JSON 上报）
                cm_console(serial_cmd_buffer);
            }
            else if (serial_cmd_buffer.startsWith("wfc "))
            {
                // Workflow CRUD + 回归测试控制台（仅上板自测用）
                wfc_console(serial_cmd_buffer);
            }
            else if (serial_cmd_buffer.startsWith("logt "))
            {
                // LogManager 测试控制台（P1.2 上板自测用）
                logt_console(serial_cmd_buffer);
            }
            else
            {
                Serial.println("unknown command");
            }
            serial_cmd_buffer = ""; //清空缓冲区
        }
        else
        {
            serial_cmd_buffer += ch;
        }
    }
}
// =====================================================
// LogManager 测试控制台（P1.2，仅上板自测用）
//
// 命令（logt <op> ...）：
//   help                                   显示帮助
//   stats                                  打印统计（先排空 RAM 环）
//   reset                                  清零统计 + 清 flush 请求
//   ring                                   打印 RAM 环状态
//   policy <level>                         打印该 Level 的 Flash/Cloud 策略
//   emit <level> <event_hex> [pid:val ...] 发一条结构化日志（最多 8 参数）
//   fill <level> <n> [event_hex]           连发 n 条（测环满 / 高频）
//   mix <n>                                5 个 Level 各发 n 条
//
// level：dbg | info | warn | error | crit（或 0..4）
// event_hex：16 进制事件 ID（如 401 = LOG_WF_START）
// =====================================================

static uint8_t logt_parse_level(const String &s, bool &ok)
{
    ok = true;

    String v = s;
    v.toLowerCase();

    if (v == "dbg" || v == "debug" || v == "0")  return LOG_LVL_DEBUG;
    if (v == "info" || v == "i" || v == "1")     return LOG_LVL_INFO;
    if (v == "warn" || v == "w" || v == "2")     return LOG_LVL_WARN;
    if (v == "error" || v == "err" || v == "e" || v == "3")
    {
        return LOG_LVL_ERROR;
    }
    if (v == "crit" || v == "critical" || v == "c" || v == "4")
    {
        return LOG_LVL_CRITICAL;
    }

    ok = false;
    return LOG_LVL_INFO;
}

static const char *logt_level_name(uint8_t lv)
{
    switch (lv)
    {
        case LOG_LVL_DEBUG:    return "DEBUG";
        case LOG_LVL_INFO:     return "INFO";
        case LOG_LVL_WARN:     return "WARN";
        case LOG_LVL_ERROR:    return "ERROR";
        case LOG_LVL_CRITICAL: return "CRITICAL";
        default:               return "?";
    }
}

// 取空格分隔的第 n 个 token（0 = 操作名）
static String logt_arg(const String &s, uint8_t n)
{
    int pos = 0;

    for (uint8_t count = 0; count <= n; count++)
    {
        while (pos < (int)s.length() && s[pos] == ' ')
        {
            pos++;
        }

        int start = pos;

        while (pos < (int)s.length() && s[pos] != ' ')
        {
            pos++;
        }

        if (start == pos)
        {
            return String();
        }

        if (count == n)
        {
            return s.substring(start, pos);
        }
    }

    return String();
}

// 排空 RAM 环（每轮 log_task 最多消费 LOG_DRAIN_MAX_PER_TASK 条）
static void logt_drain_all()
{
    for (uint8_t i = 0; i < 16; i++)
    {
        log_task();
    }
}

static void logt_print_stats()
{
    LogStats st;
    log_get_stats(st);

    Serial.printf(
        "[LogT] stats emit=%u debug=%u ring=%u/%u hw=%u drop=%u consumed=%u "
        "flash=%u cloud=%u crit=%u flush=%u boot=%u seq=%u ready=%u psram=%u bytes=%u\n",
        (unsigned)st.emit_total,
        (unsigned)st.debug_dropped,
        (unsigned)st.ring_used, (unsigned)LOG_RAM_QUEUE_SLOTS,
        (unsigned)st.ring_high_water,
        (unsigned)st.ring_drop,
        (unsigned)st.consumed,
        (unsigned)st.flash_routed,
        (unsigned)st.cloud_routed,
        (unsigned)st.critical_seen,
        (unsigned)st.flush_requests,
        (unsigned)st.boot_seq,
        (unsigned)st.last_seq,
        (unsigned)st.ring_ready,
        (unsigned)st.ring_in_psram,
        (unsigned)st.ring_bytes);

    Serial.printf(
        "[LogT] fstats ok=%u fail=%u seg_new=%u seg_del=%u crc_err=%u corrupt=%u "
        "retry=%u blocked=%u honored=%u\n",
        (unsigned)st.flash_append_ok,
        (unsigned)st.flash_append_fail,
        (unsigned)st.flash_segment_created,
        (unsigned)st.flash_segment_deleted,
        (unsigned)st.flash_crc_error,
        (unsigned)st.flash_corrupt_segment,
        (unsigned)st.flash_retry,
        (unsigned)st.flash_blocked_rounds,
        (unsigned)st.flush_honored);

    Serial.printf(
        "[LogT] cstats batch=%u rec=%u retx=%u ack_ok=%u ack_to=%u ack_lost=%u "
        "qdrop=%u fdrop=%u deg=%u qused=%u\n",
        (unsigned)st.cloud_batch_sent,
        (unsigned)st.cloud_records_sent,
        (unsigned)st.cloud_retx,
        (unsigned)st.cloud_ack_ok,
        (unsigned)st.cloud_ack_timeout,
        (unsigned)st.cloud_ack_lost,
        (unsigned)st.cloud_q_drop,
        (unsigned)st.cloud_flash_drop,
        (unsigned)st.self_degraded,
        (unsigned)st.cloud_q_used);

    // FIX-1 / FIX-3 / FIX-H2：淘汰归因分离 + 空洞补齐
    Serial.printf(
        "[LogT] cstats2 evict_inf=%u seg_evict_unacked=%u hole_evict=%u\n",
        (unsigned)st.cloud_q_evict_inflight,
        (unsigned)st.flash_seg_evict_unacked,
        (unsigned)st.cloud_hole_from_evict);

    // P1.5：ACK / Retry / Offline 观测
    Serial.printf(
        "[LogT] ackst ignored=%u partial=%u segdel=%u replay=%u offskip=%u giveup=%u\n",
        (unsigned)st.cloud_ack_ignored,
        (unsigned)st.cloud_ack_partial,
        (unsigned)st.cloud_seg_acked_del,
        (unsigned)st.cloud_replay_records,        (unsigned)st.cloud_offline_skip,
        (unsigned)st.cloud_give_up);
}

static void logt_print_ring()
{
    LogStats st;
    log_get_stats(st);

    Serial.printf(
        "[LogT] ring used=%u/%u hw=%u drop=%u ready=%u psram=%u bytes=%u\n",
        (unsigned)st.ring_used, (unsigned)LOG_RAM_QUEUE_SLOTS,
        (unsigned)st.ring_high_water, (unsigned)st.ring_drop,
        (unsigned)st.ring_ready, (unsigned)st.ring_in_psram,
        (unsigned)st.ring_bytes);
}

// ---- P1.3 Flash 观测 ----

static void logt_print_flash()
{
    LogFlashInfo fi;
    log_flash_get_info(fi);

    Serial.printf(
        "[LogT] flash ready=%u segs=%u oldest=%u newest=%u append=%u@%u "
        "total=%u fseq_old=%u fseq_new=%u batch=%u psram=%u reliable=%u\n",
        (unsigned)fi.flash_ready,
        (unsigned)fi.valid_segments,
        (unsigned)fi.oldest_segment,
        (unsigned)fi.newest_segment,
        (unsigned)fi.append_segment,
        (unsigned)fi.append_index,
        (unsigned)fi.total_records,
        (unsigned)fi.first_seq_oldest,
        (unsigned)fi.first_seq_newest,
        (unsigned)fi.batch_bytes,
        (unsigned)fi.batch_in_psram,
        (unsigned)fi.seq_reliable);
}

static void logt_do_flush()
{
    // 反复 drain，直到 RAM 环排空（WARN+ 在此过程中落 Flash）
    logt_drain_all();
    Serial.println("[LogT] flush done");
}

static void logt_print_fseg(const String &op)
{
    long seg = strtol(logt_arg(op, 1).c_str(), nullptr, 10);
    LogSegmentHead h;

    if (!log_flash_peek_segment((uint32_t)seg, h))
    {
        Serial.printf("[LogT] fseg seg=%ld absent\n", seg);
        return;
    }

    Serial.printf(
        "[LogT] fseg seg=%ld ok=%u magic=0x%08X index=%u first_seq=%u recs=%u crc=0x%08X\n",
        seg, (unsigned)h.ok, (unsigned)h.magic, (unsigned)h.index,
        (unsigned)h.first_seq, (unsigned)h.records, (unsigned)h.crc32);

    // FIX-3b：暴露 seq 稀疏性 —— derived（旧的错误推导）vs last（Flash 真相）
    // `gap` = last - derived = 该段内被 INFO 等"不落盘记录"消耗掉的 seq 个数。
    // 这个差值与开机时刻无关，是**最稳的上板断言**。
    {
        const uint32_t derived =
            (h.records > 0) ? (h.first_seq + (uint32_t)h.records - 1u) : 0u;
        const uint32_t gap = (h.last_seq > derived) ? (h.last_seq - derived) : 0u;

        Serial.printf("[LogT] fseg2 last=%u derived=%u gap=%u\n",
                      (unsigned)h.last_seq, (unsigned)derived, (unsigned)gap);
    }
}

static void logt_print_fver(const String &op)
{
    long seg = strtol(logt_arg(op, 1).c_str(), nullptr, 10);
    long rec = strtol(logt_arg(op, 2).c_str(), nullptr, 10);

    LogRecord r;
    uint8_t crc_ok = 0;

    if (!log_flash_peek_record((uint32_t)seg, (uint8_t)rec, r, crc_ok))
    {
        Serial.printf("[LogT] fver seg=%ld rec=%ld absent\n", seg, rec);
        return;
    }

    Serial.printf(
        "[LogT] fver seg=%ld rec=%ld crc=%u ver=%u lvl=%u seq=%u boot=%u "
        "evt=0x%04X params=%u up=%u\n",
        seg, rec, (unsigned)crc_ok, (unsigned)r.version, (unsigned)r.level,
        (unsigned)r.seq, (unsigned)r.boot_seq, (unsigned)r.event_id,
        (unsigned)r.param_count, (unsigned)r.uptime_ms);
}

static void logt_do_fwipe()
{
    const bool ok = log_flash_wipe();
    Serial.printf("[LogT] fwipe ok=%d\n", ok ? 1 : 0);
}

// ---- P1.3 meta / sequence ----

static void logt_print_meta()
{
    LogFlashInfo fi;
    log_flash_get_info(fi);

    Serial.printf(
        "[LogT] meta boot=%u reserved=%u corrupt=%u reliable=%u base=%u limit=%u last=%u\n",
        (unsigned)fi.boot_seq,
        (unsigned)fi.seq_reserved,
        (unsigned)fi.corrupt_count,
        (unsigned)fi.seq_reliable,
        (unsigned)fi.seq_base,
        (unsigned)fi.seq_limit,
        (unsigned)fi.seq_last);
}

static void logt_do_mwipe()
{
    const bool ok = log_meta_wipe();
    Serial.printf("[LogT] mwipe ok=%d\n", ok ? 1 : 0);
}

static void logt_do_mcorrupt()
{
    const bool ok = log_meta_corrupt();
    Serial.printf("[LogT] mcorrupt ok=%d\n", ok ? 1 : 0);
}

static void logt_do_mfail(const String &op)
{
    const long v = strtol(logt_arg(op, 1).c_str(), nullptr, 10);
    log_meta_test_fail_next(v != 0);
    Serial.printf("[LogT] mfail armed=%ld\n", (v != 0) ? 1L : 0L);
}

// Commit 5：令接下来 n 次 Flash append 整批失败（验证 §22/§23 安全交接）
static void logt_do_ffail(const String &op)
{
    const long n = strtol(logt_arg(op, 1).c_str(), nullptr, 10);
    const uint8_t v = (n > 0 && n < 256) ? (uint8_t)n : (uint8_t)0;
    log_flash_test_fail_next(v);
    Serial.printf("[LogT] ffail armed=%u\n", (unsigned)v);
}

// ---- P1.4 / P1.5：云侧观测与测试钩子 ----

static void logt_print_cloud()
{
    LogCloudInfo ci;
    log_cloud_get_info(ci);

    Serial.printf(
        "[LogT] cloud ready=%u psram=%u bytes=%u qused=%u acked=%u "
        "inflight=%u boot=%u from=%u to=%u n=%u retry=%u giveup=%u conn=%u lastlen=%u\n",
        (unsigned)ci.queue_ready,
        (unsigned)ci.queue_in_psram,
        (unsigned)ci.queue_bytes,
        (unsigned)ci.queue_used,
        (unsigned)ci.acked_seq,
        (unsigned)ci.inflight,
        (unsigned)ci.tx_boot_seq,
        (unsigned)ci.tx_from,
        (unsigned)ci.tx_to,
        (unsigned)ci.tx_count,
        (unsigned)ci.retry,
        (unsigned)ci.gave_up,
        (unsigned)ci.connected,
        (unsigned)ci.last_batch_bytes);

    // P1.5：补发游标 / 放弃水位 / 下次可发送时间
    Serial.printf(
        "[LogT] cloud2 giveup_seq=%u replay_seq=%u replay_done=%u next_tx_in=%u "
        "force_online=%u ackto=%u backoff_base=%u\n",
        (unsigned)ci.give_up_seq,
        (unsigned)ci.replay_seq,
        (unsigned)ci.replay_done,
        (unsigned)ci.next_tx_in_ms,
        (unsigned)ci.force_online,
        (unsigned)ci.ack_timeout_ms,
        (unsigned)ci.backoff_base_ms);

    // FIX-1 / FIX-2 / FIX-3 / DIR-1 观测
    Serial.printf(
        "[LogT] cloud3 tx_valid=%u rd_base=%u holes=%u hovf=%u "
        "rseg=%u ridx=%u rarmed=%u\n",
        (unsigned)ci.tx_valid,
        (unsigned)ci.tx_rd_base,
        (unsigned)ci.hole_count,
        (unsigned)ci.hole_overflow,
        (unsigned)ci.replay_seg,
        (unsigned)ci.replay_idx,
        (unsigned)ci.replay_armed);

    // FIX-BT9：水位语义分离观测
    //   gc < acked  ⟺ 存在"已被更新 ACK 越过、但仍未确认"的旧记录（钳制生效中）
    Serial.printf(
        "[LogT] cloud4 acked=%u gc=%u gcfloor=%u\n",
        (unsigned)ci.acked_seq,
        (unsigned)ci.gc_seq,
        (unsigned)ci.gc_floor);
}

// 直接入队 n 条 INFO（不经 RAM 环 / 不落 Flash），用于无 MQTT 条件下
// 验证云队列容量、FIFO 淘汰与 CBOR 组包
static void logt_do_cpush(const String &op)
{
    const long n = strtol(logt_arg(op, 1).c_str(), nullptr, 10);

    // 可选等级（默认 info）：便于验证 FIX-2 的"Flash-routed 淘汰登记空洞"
    uint8_t lv = LOG_LVL_INFO;

    if (logt_arg(op, 2).length() > 0)
    {
        bool ok = false;
        const uint8_t parsed = logt_parse_level(logt_arg(op, 2), ok);

        if (!ok)
        {
            Serial.println("[LogT] bad level");
            return;
        }

        lv = parsed;
    }

    long done = 0;

    for (long i = 0; i < n && i < 1024; i++)
    {
        log_cloud_test_push(LOG_LOG_ACK_LOST, (LogLevel)lv);
        done++;
    }

    LogCloudInfo ci;
    log_cloud_get_info(ci);
    Serial.printf("[LogT] cpush n=%ld lv=%u qused=%u\n",
                  done, (unsigned)lv, (unsigned)ci.queue_used);
}

static void logt_do_creset()
{
    log_cloud_test_reset();
    Serial.println("[LogT] creset ok");
}

// ---- P1.5：ACK / 离线注入 ----

// 注入一条 log_ack（走真实 ACK 状态机，只跳过 MQTT 传输）：
//   logt ack <boot_seq> <seq_from> <seq_to>
static void logt_do_ack(const String &op)
{
    const uint32_t boot = (uint32_t)strtoul(logt_arg(op, 1).c_str(), nullptr, 10);
    const uint32_t from = (uint32_t)strtoul(logt_arg(op, 2).c_str(), nullptr, 10);
    const uint32_t to = (uint32_t)strtoul(logt_arg(op, 3).c_str(), nullptr, 10);

    log_cloud_test_ack(boot, from, to);

    // 立刻驱动一轮消费，让断言能直接看到结果（无需等 loop 下一拍）
    log_task();

    LogCloudInfo ci;
    log_cloud_get_info(ci);

    Serial.printf(
        "[LogT] ack boot=%u from=%u to=%u -> acked=%u qused=%u inflight=%u\n",
        (unsigned)boot, (unsigned)from, (unsigned)to,
        (unsigned)ci.acked_seq, (unsigned)ci.queue_used,
        (unsigned)ci.inflight);
}

// 令接下来 n 次 cloud_send_log() 失败（模拟离线）
static void logt_do_cfail(const String &op)
{
    const long n = strtol(logt_arg(op, 1).c_str(), nullptr, 10);
    const uint8_t v = (n > 0 && n < 256) ? (uint8_t)n : (uint8_t)0;

    log_cloud_test_fail_next(v);

    Serial.printf("[LogT] cfail armed=%u\n", (unsigned)v);
}

// 强制"在线"并跳过真实 cloud_send_log() —— 无 MQTT 对端时驱动 ACK 全链路
static void logt_do_sonline(const String &op)
{
    const long v = strtol(logt_arg(op, 1).c_str(), nullptr, 10);

    log_cloud_test_set_online(v != 0);

    Serial.printf("[LogT] sonline=%ld\n", (v != 0) ? 1L : 0L);
}

// 上板自测：临时缩短 ACK 超时 / 退避基数（毫秒；0 = 恢复冻结默认）
static void logt_do_atiming(const String &name, const String &op)
{
    const uint32_t ms = (uint32_t)strtoul(logt_arg(op, 1).c_str(), nullptr, 10);

    if (name == "atimeout")
    {
        log_cloud_test_set_ack_timeout(ms);
    }
    else
    {
        log_cloud_test_set_backoff_base(ms);
    }

    Serial.printf("[LogT] %s=%u\n", name.c_str(), (unsigned)ms);
}

// 按当前在途批次自动构造 ACK（seq 由 Boot 区间预留分配，脚本无法预知）
//   ackauto 0 [k]  完整覆盖 -> ACCEPT
//   ackauto 1 <k>  只覆盖前 k 条 -> PARTIAL
//   ackauto 2      回退到已确认水位 -> DUPLICATE
//   ackauto 3      错误 boot_seq -> IGNORE
static void logt_do_ackauto(const String &op)
{
    const uint8_t mode = (uint8_t)strtoul(logt_arg(op, 1).c_str(), nullptr, 10);
    const uint8_t k = (uint8_t)strtoul(logt_arg(op, 2).c_str(), nullptr, 10);

    const bool ok = log_cloud_test_ack_inflight(mode, k);

    if (!ok)
    {
        Serial.printf("[LogT] ackauto mode=%u FAIL no-inflight\n", (unsigned)mode);
        return;
    }

    // 立刻驱动一轮消费，让断言能直接看到结果（无需等 loop 下一拍）
    log_task();

    LogCloudInfo ci;
    log_cloud_get_info(ci);

    Serial.printf(
        "[LogT] ackauto mode=%u k=%u -> acked=%u qused=%u inflight=%u\n",
        (unsigned)mode, (unsigned)k,
        (unsigned)ci.acked_seq, (unsigned)ci.queue_used,
        (unsigned)ci.inflight);
}

// ---- P1.3 损坏注入 ----

static void logt_do_fcorrupt(const String &op)
{
    const long seg = strtol(logt_arg(op, 1).c_str(), nullptr, 10);
    const bool ok = log_seg_corrupt_head((uint32_t)seg);
    Serial.printf("[LogT] fcorrupt seg=%ld ok=%d\n", seg, ok ? 1 : 0);
}

static void logt_do_fbrec(const String &op)
{
    const long seg = strtol(logt_arg(op, 1).c_str(), nullptr, 10);
    const long rec = strtol(logt_arg(op, 2).c_str(), nullptr, 10);
    const bool ok = log_seg_corrupt_record((uint32_t)seg, (uint8_t)rec);
    Serial.printf("[LogT] fbrec seg=%ld rec=%ld ok=%d\n", seg, rec, ok ? 1 : 0);
}

static void logt_do_ftrunc(const String &op)
{
    const long seg = strtol(logt_arg(op, 1).c_str(), nullptr, 10);
    const long bytes = strtol(logt_arg(op, 2).c_str(), nullptr, 10);
    const bool ok = log_seg_truncate((uint32_t)seg, (uint32_t)bytes);
    Serial.printf("[LogT] ftrunc seg=%ld bytes=%ld ok=%d\n", seg, bytes, ok ? 1 : 0);
}

static void logt_print_policy(const String &op)
{
    bool ok = false;
    uint8_t lv = logt_parse_level(logt_arg(op, 1), ok);

    if (!ok)
    {
        Serial.println("[LogT] bad level");
        return;
    }

    Serial.printf(
        "[LogT] policy %s flash=%d cloud=%d\n",
        logt_level_name(lv),
        log_level_to_flash((LogLevel)lv) ? 1 : 0,
        log_level_to_cloud((LogLevel)lv) ? 1 : 0);
}

static void logt_do_emit(const String &op)
{
    bool ok = false;
    uint8_t lv = logt_parse_level(logt_arg(op, 1), ok);

    if (!ok)
    {
        Serial.println("[LogT] bad level");
        return;
    }

    long ev = strtol(logt_arg(op, 2).c_str(), nullptr, 16);

    LogParamIn args[LOG_MAX_PARAMS];
    uint8_t n = 0;

    for (uint8_t i = 3; i < 11 && n < LOG_MAX_PARAMS; i++)
    {
        String a = logt_arg(op, i);

        if (a.length() == 0)
        {
            break;
        }

        int colon = a.indexOf(':');

        if (colon <= 0)
        {
            continue;
        }

        long pid = strtol(a.substring(0, colon).c_str(), nullptr, 16);
        uint32_t val = (uint32_t)strtoul(a.substring(colon + 1).c_str(), nullptr, 0);

        args[n++] = log_arg_u32((uint8_t)pid, val);
    }

    bool queued = log_emit((LogEventId)ev, (LogLevel)lv, args, n);

    Serial.printf(
        "[LogT] emit level=%s event=0x%04X params=%u queued=%d\n",
        logt_level_name(lv), (unsigned)ev, (unsigned)n, queued ? 1 : 0);
}

static void logt_do_fill(const String &op)
{
    bool ok = false;
    uint8_t lv = logt_parse_level(logt_arg(op, 1), ok);

    if (!ok)
    {
        Serial.println("[LogT] bad level");
        return;
    }

    long n = strtol(logt_arg(op, 2).c_str(), nullptr, 10);

    if (n <= 0)
    {
        Serial.println("[LogT] bad count");
        return;
    }

    long ev = strtol(logt_arg(op, 3).c_str(), nullptr, 16);

    if (ev <= 0)
    {
        ev = (long)LOG_WF_START;
    }

    uint32_t queued = 0;

    for (long i = 0; i < n; i++)
    {
        LogParamIn a[1];
        a[0] = log_arg_u32(LOG_P_SLOT, (uint32_t)i);

        if (log_emit((LogEventId)ev, (LogLevel)lv, a, 1))
        {
            queued++;
        }
    }

    Serial.printf(
        "[LogT] fill level=%s n=%ld event=0x%04lX queued=%u\n",
        logt_level_name(lv), n, (unsigned long)ev, (unsigned)queued);
}

static void logt_do_mix(const String &op)
{
    long n = strtol(logt_arg(op, 1).c_str(), nullptr, 10);

    if (n <= 0)
    {
        Serial.println("[LogT] bad count");
        return;
    }

    const uint8_t levels[5] =
    {
        LOG_LVL_DEBUG, LOG_LVL_INFO, LOG_LVL_WARN, LOG_LVL_ERROR, LOG_LVL_CRITICAL
    };

    uint32_t queued = 0;

    for (uint8_t k = 0; k < 5; k++)
    {
        for (long i = 0; i < n; i++)
        {
            if (log_emit0(LOG_WF_FINISHED, (LogLevel)levels[k]))
            {
                queued++;
            }
        }
    }

    Serial.printf("[LogT] mix n=%ld queued=%u levels=5\n", n, (unsigned)queued);
}

void logt_console(const String &cmd)
{
    String op = cmd.substring(5);
    op.trim();

    const String name = logt_arg(op, 0);

    if (name.length() == 0 || name == "help")
    {
        Serial.println("[LogT] ops: stats reset ring policy emit fill mix");
        Serial.println("[LogT]   emit <lv> <event_hex> [pid:val ...]  (max 8 params)");
        Serial.println("[LogT]   fill <lv> <n> [event_hex]");
        Serial.println("[LogT]   mix <n>");
        Serial.println("[LogT]   lv = dbg|info|warn|error|crit (or 0..4)");
        Serial.println("[LogT] P1.3: flush | flash | fseg <seg> | fver <seg> <rec> | fwipe");
        Serial.println("[LogT] P1.3: stats | mwipe | mcorrupt | mfail <0|1> | ffail <n>");
        Serial.println("[LogT] P1.4: cloud | cpush <n> [lv] | creset");
        Serial.println("[LogT] P1.5: ack <boot> <from> <to> | ackauto <mode> [k] | cfail <n> | sonline <0|1>");
        Serial.println("[LogT] P1.5: atimeout <ms> | abackoff <ms>   (0 = 恢复默认)");
        Serial.println("[LogT] P1.3: meta | mwipe | mcorrupt");
        return;
    }

    if (name == "stats")
    {
        logt_drain_all();
        logt_print_stats();
        return;
    }

    if (name == "ring")
    {
        logt_drain_all();
        logt_print_ring();
        return;
    }

    // ---- P1.3 Flash Segment Ring ----
    if (name == "flush")
    {
        logt_do_flush();
        return;
    }

    if (name == "flash")
    {
        logt_print_flash();
        return;
    }

    if (name == "fseg")
    {
        logt_print_fseg(op);
        return;
    }

    if (name == "fver")
    {
        logt_print_fver(op);
        return;
    }

    if (name == "fwipe")
    {
        logt_do_fwipe();
        return;
    }

    if (name == "meta")
    {
        logt_print_meta();
        return;
    }

    if (name == "mwipe")
    {
        logt_do_mwipe();
        return;
    }

    if (name == "mcorrupt")
    {
        logt_do_mcorrupt();
        return;
    }

    if (name == "mfail")
    {
        logt_do_mfail(op);
        return;
    }

    if (name == "ffail")
    {
        logt_do_ffail(op);
        return;
    }

    if (name == "cloud")
    {
        logt_print_cloud();
        return;
    }

    if (name == "cpush")
    {
        logt_do_cpush(op);
        return;
    }

    if (name == "creset")
    {
        logt_do_creset();
        return;
    }

    if (name == "ack")
    {
        logt_do_ack(op);
        return;
    }

    if (name == "cfail")
    {
        logt_do_cfail(op);
        return;
    }

    if (name == "sonline")
    {
        logt_do_sonline(op);
        return;
    }

    if (name == "ackauto")
    {
        logt_do_ackauto(op);
        return;
    }

    if (name == "atimeout" || name == "abackoff")
    {
        logt_do_atiming(name, op);
        return;
    }

    if (name == "fcorrupt")
    {
        logt_do_fcorrupt(op);
        return;
    }

    if (name == "fbrec")
    {
        logt_do_fbrec(op);
        return;
    }

    if (name == "ftrunc")
    {
        logt_do_ftrunc(op);
        return;
    }

    if (name == "reset")
    {
        logt_drain_all();
        log_stats_reset();
        log_clear_flush_request();
        Serial.println("[LogT] reset ok");
        return;
    }

    if (name == "policy")
    {
        logt_print_policy(op);
        return;
    }

    if (name == "emit")
    {
        logt_do_emit(op);
        return;
    }

    if (name == "fill")
    {
        logt_do_fill(op);
        return;
    }

    if (name == "mix")
    {
        logt_do_mix(op);
        return;
    }

    if (name == "overlimit")
    {
        // 契约验证：param_count > LOG_MAX_PARAMS 必须**整体拒绝**（不截断）
        LogParamIn args[LOG_MAX_PARAMS + 1];

        for (uint8_t i = 0; i < LOG_MAX_PARAMS + 1; i++)
        {
            args[i] = log_arg_u32(LOG_P_SLOT, (uint32_t)i);
        }

        bool queued = log_emit((LogEventId)LOG_WF_START, LOG_LVL_WARN,
                               args, (uint8_t)(LOG_MAX_PARAMS + 1));

        Serial.printf("[LogT] overlimit params=%u queued=%d (expect 0)\n",
                      (unsigned)(LOG_MAX_PARAMS + 1), queued ? 1 : 0);
        return;
    }

    Serial.println("[LogT] unknown op (try: logt help)");
}

// =====================================================
// WorkflowStorage 独立测试控制台（仅上板自测用）
//
// 命令（wfst <op> ...）：
//   help
//   seed <wf> <steps> <base>       构造并保存一个 Workflow（param = base+step）
//   overwrite <wf> <steps> <base>  换参重新保存（复用当前 wf）
//   dump <wf>                      读取并打印 step_count / step id / param[0]
//   verify <wf> <steps> <base>     读取并自动比对，PASS/FAIL
//   ls <wf>                        打印 step bin 是否存在 + 暂存文件数
//   del <wf>                       删除（只置 meta.valid=false）
//   fail <wf> <failstep>           保存时注入第 N 步写失败（覆盖当前参数）
//   abort1 <wf> <steps> <base>     模拟提交前掉电（返回 TEST_ABORTED）
//   abort2 <wf> <steps> <base>     模拟提交后掉电（返回 TEST_ABORTED）
//   recover                        执行事务恢复（发布/清理）
//
// 掉电场景测试方法：
//   abort1 → staged>0 → recover → staged=0 → verify 仍为旧参数
//   abort2 → staged>0 → recover → staged=0 → verify 为新参数
// =====================================================

static int g_wfst_steps = 0;
static int g_wfst_base = 0;

static void wfst_make_def(
    WorkflowDefinition &def,
    int wf,
    int steps,
    int base
)
{
    memset(&def, 0, sizeof(def));
    snprintf(def.id, sizeof(def.id), "wfst_wf%d", wf);
    snprintf(def.name, sizeof(def.name), "wfst_test_%d", wf);
    def.enable = 1;
    def.timeout_ms = 10000;
    def.step_count = (uint8_t)steps;

    for (int i = 0; i < steps && i < (int)WF_STG_MAX_STEP; i++)
    {
        WorkflowStepDefinition &st = def.steps[i];
        st.type = 0;                       // ACTION
        st.instance_type = 1;              // ACTION
        snprintf(st.id, sizeof(st.id), "act_%d_%d", wf, i);
        st.param_count = 1;
        snprintf(st.params[0].name, sizeof(st.params[0].name), "k");
        st.params[0].type = 0;             // PARAM_INT
        st.params[0].int_value = base + i;
    }
}

static bool wfst_verify(
    int wf,
    int steps,
    int base
)
{
    // WorkflowDefinition ~8.6KB 不能放栈（loopTask 栈 8KB），必须堆分配
    WorkflowDefinition *def_buf =
        workflow_storage_alloc_definition();

    if (def_buf == NULL)
    {
        Serial.println("verify: def alloc failed");
        return false;
    }

    WorkflowDefinition &def = *def_buf;
    WorkflowStorageResult r = workflow_storage_load((uint8_t)wf, &def);

    if (r != WF_STG_OK)
    {
        Serial.printf("verify: load failed, r=%s\n", workflow_storage_result_name(r));
        workflow_storage_free_definition(def_buf);
        return false;
    }
    if ((int)def.step_count != steps)
    {
        Serial.printf("verify: step_count=%u expect=%d\n", def.step_count, steps);
        workflow_storage_free_definition(def_buf);
        return false;
    }
    for (int i = 0; i < steps; i++)
    {
        char expect_id[WF_STG_ID_MAX_LEN];
        snprintf(expect_id, sizeof(expect_id), "act_%d_%d", wf, i);
        if (strcmp(def.steps[i].id, expect_id) != 0)
        {
            Serial.printf("verify: step[%d] id=%s expect=%s\n",
                          i, def.steps[i].id, expect_id);
            workflow_storage_free_definition(def_buf);
            return false;
        }
        if (def.steps[i].param_count < 1 ||
            def.steps[i].params[0].int_value != base + i)
        {
            Serial.printf("verify: step[%d] param=%d expect=%d\n",
                          i,
                          def.steps[i].param_count ? (int)def.steps[i].params[0].int_value : -999,
                          base + i);
            workflow_storage_free_definition(def_buf);
            return false;
        }
    }
    workflow_storage_free_definition(def_buf);
    return true;
}

void wfst_console(const String &cmd)
{
    String op = cmd.substring(5);
    op.trim();

    int a = 0, b = 0, c = 0;

    if (op == "help")
    {
        Serial.println("wfst ops: seed overwrite dump verify ls del fail failwf abort1 abort2 recover");
        return;
    }

    if (op == "recover")
    {
        bool ok = workflow_storage_recover();
        Serial.printf("recover ret=%d\n", ok ? 1 : 0);
        for (int wf = 0; wf < (int)WF_STG_MAX_COUNT; wf++)
        {
            int n = workflow_storage_test_staged_count((uint8_t)wf);
            if (n > 0)
            {
                Serial.printf("  wf%02d staged=%d\n", wf, n);
            }
        }
        return;
    }

    if (sscanf(op.c_str(), "seed %d %d %d", &a, &b, &c) == 3 ||
        sscanf(op.c_str(), "overwrite %d %d %d", &a, &b, &c) == 3 ||
        sscanf(op.c_str(), "abort1 %d %d %d", &a, &b, &c) == 3 ||
        sscanf(op.c_str(), "abort2 %d %d %d", &a, &b, &c) == 3)
    {
        if (a < 0 || a >= (int)WF_STG_MAX_COUNT || b <= 0 || b > (int)WF_STG_MAX_STEP)
        {
            Serial.println("bad args");
            return;
        }
        g_wfst_steps = b;
        g_wfst_base = c;

        if (op.startsWith("abort1")) workflow_storage_test_abort_phase(1);
        if (op.startsWith("abort2")) workflow_storage_test_abort_phase(2);

        // WorkflowDefinition ~8.6KB 不能放栈（loopTask 栈 8KB），必须堆分配
        WorkflowDefinition *def_buf = workflow_storage_alloc_definition();
        if (def_buf == NULL)
        {
            Serial.println("def alloc failed");
            workflow_storage_test_abort_phase(0);
            return;
        }
        WorkflowDefinition &def = *def_buf;
        wfst_make_def(def, a, b, c);
        WorkflowStorageResult r = workflow_storage_save((uint8_t)a, &def);
        workflow_storage_free_definition(def_buf);
        Serial.printf("%s -> r=%s staged=%d\n",
                      op.substring(0, op.indexOf(' ')).c_str(),
                      workflow_storage_result_name(r),
                      workflow_storage_test_staged_count((uint8_t)a));

        workflow_storage_test_abort_phase(0);
        return;
    }

    if (sscanf(op.c_str(), "fail %d %d", &a, &b) == 2)
    {
        if (a < 0 || a >= (int)WF_STG_MAX_COUNT ||
            b < 0 || b >= g_wfst_steps)
        {
            Serial.println("bad args");
            return;
        }
        // WorkflowDefinition ~8.6KB 不能放栈（loopTask 栈 8KB），必须堆分配
        WorkflowDefinition *def_buf = workflow_storage_alloc_definition();
        if (def_buf == NULL)
        {
            Serial.println("def alloc failed");
            workflow_storage_test_fail_step(-1);
            return;
        }
        WorkflowDefinition &def = *def_buf;
        wfst_make_def(def, a, g_wfst_steps, g_wfst_base + 100);
        workflow_storage_test_fail_step(b);
        WorkflowStorageResult r = workflow_storage_save((uint8_t)a, &def);
        workflow_storage_free_definition(def_buf);
        workflow_storage_test_fail_step(-1);
        Serial.printf("fail@%d -> r=%s staged=%d\n",
                      b,
                      workflow_storage_result_name(r),
                      workflow_storage_test_staged_count((uint8_t)a));
        return;
    }

    if (sscanf(op.c_str(), "dump %d", &a) == 1)
    {
        // WorkflowDefinition ~8.6KB 不能放栈（loopTask 栈 8KB），必须堆分配
        WorkflowDefinition *def_buf = workflow_storage_alloc_definition();
        if (def_buf == NULL)
        {
            Serial.println("def alloc failed");
            return;
        }
        WorkflowDefinition &def = *def_buf;
        WorkflowStorageResult r = workflow_storage_load((uint8_t)a, &def);
        Serial.printf("load r=%s staged=%d\n",
                      workflow_storage_result_name(r),
                      workflow_storage_test_staged_count((uint8_t)a));
        if (r == WF_STG_OK)
        {
            Serial.printf("  wf%02d id=%s name=%s enable=%u count=%u\n",
                          a, def.id, def.name, def.enable, def.step_count);
            for (int i = 0; i < (int)def.step_count && i < (int)WF_STG_MAX_STEP; i++)
            {
                Serial.printf("    step%02d id=%s p0=%d\n",
                              i, def.steps[i].id,
                              def.steps[i].param_count
                                  ? (int)def.steps[i].params[0].int_value
                                  : -999);
            }
        }
        workflow_storage_free_definition(def_buf);
        return;
    }

    if (sscanf(op.c_str(), "verify %d %d %d", &a, &b, &c) == 3)
    {
        bool ok = wfst_verify(a, b, c);
        Serial.printf("verify wf%02d steps=%d base=%d -> %s\n",
                      a, b, c, ok ? "PASS" : "FAIL");
        return;
    }

    if (sscanf(op.c_str(), "ls %d", &a) == 1)
    {
        int exist = 0;
        for (int i = 0; i < (int)WF_STG_MAX_STEP; i++)
        {
            char p[64];
            snprintf(p, sizeof(p), "/workflow/wf%02d/step%02d.bin", a, i);
            if (bin_storage_exists(p))
            {
                exist++;
            }
        }
        Serial.printf("wf%02d step-bin-exist=%d staged=%d\n",
                      a, exist,
                      workflow_storage_test_staged_count((uint8_t)a));
        return;
    }

    if (sscanf(op.c_str(), "del %d", &a) == 1)
    {
        WorkflowStorageResult r = workflow_storage_delete((uint8_t)a);
        Serial.printf("del wf%02d -> r=%s\n", a, workflow_storage_result_name(r));
        return;
    }

    // 故障注入：让下一次 save 的第 N 个 Step 写失败（-1 关闭）
    if (sscanf(op.c_str(), "fail %d", &a) == 1)
    {
        workflow_storage_test_fail_step(a);
        Serial.printf("fail step armed=%d\n", a);
        return;
    }

    // 故障注入：让指定 Workflow 的整次 save 失败（-1 关闭）
    // 用于验证 workflow.save 的"部分失败不得清空全部 Dirty"
    if (sscanf(op.c_str(), "failwf %d", &a) == 1)
    {
        workflow_storage_test_fail_wf(a);
        Serial.printf("fail wf armed=%d\n", a);
        return;
    }

    Serial.printf("unknown wfst op: %s\n", op.c_str());
}

// =====================================================
// Workflow CRUD + 回归测试控制台（仅上板自测用）
//
// 命令（wfc <op> ...）：
//   help
//   list                                  列出全部 Workflow RAM 状态
//   create <wf> <id> <name> <timeout>      新建空 Workflow
//   meta <wf> <id> <name> <0|1> <timeout>  改 Workflow 级 Definition
//   step <wf> <st> <t|a> <id> <intval>     新建/改写 Step（1 个 int 参数）
//   count <wf> <n>                         设置 step_count
//   del <wf>                               删除（只置 meta.valid=false）
//   param <wf> <st> <pidx> <intval>        改单个参数（运行中允许，§13）
//   def <wf> <st>                          打印该 Step 的 Definition
//   dirty                                  是否存在未持久化修改
//   save                                   显式触发保存事务
//   migrate                                全量迁移到 BIN
//   run <wf>                               启动 Workflow
//
// Phase 9（运行中修改隔离）验证步骤：
//   wfc run 0        → 运行中
//   wfc param 0 1 0 999    → 改 Definition（返回 ok=1）
//   wfc def 0 1      → Definition 已是 999，但本次运行仍用旧值跑完
// =====================================================

void wfc_console(const String &cmd)
{
    String op = cmd.substring(4);
    op.trim();

    if (op.length() == 0 || op.startsWith("help"))
    {
        Serial.println("wfc ops: list create meta step count del stop param def dirty save migrate run");
        return;
    }

    int a = 0, b = 0, c = 0;
    char sid[32] = {0};
    char sname[32] = {0};
    char kind[4] = {0};

    if (op.startsWith("list"))
    {
        Serial.printf("count=%u dirty=%u\n",
                      (unsigned)workflow_get_count(),
                      workflow_has_any_dirty() ? 1u : 0u);
        for (uint8_t i = 0; i < workflow_get_count(); i++)
        {
            Workflow *w = workflow_get(i);
            if (w == nullptr) continue;
            Serial.printf("  wf%02u id=%s name=%s en=%u st=%d steps=%u cur=%u to=%lu\n",
                          (unsigned)i,
                          w->id.c_str(),
                          w->name.c_str(),
                          w->enable ? 1u : 0u,
                          (int)w->state,
                          (unsigned)w->step_count,
                          (unsigned)w->current_step,
                          (unsigned long)w->timeout_ms);
        }
        return;
    }

    if (sscanf(op.c_str(), "create %d %31s %31s %d", &a, sid, sname, &c) == 4)
    {
        bool ok = workflow_create((uint8_t)a, String(sid), String(sname), (uint32_t)c);
        Serial.printf("create wf%02d -> %d\n", a, ok ? 1 : 0);
        return;
    }

    if (sscanf(op.c_str(), "meta %d %31s %31s %d %d", &a, sid, sname, &b, &c) == 5)
    {
        bool ok = workflow_update_meta((uint8_t)a, String(sid), String(sname),
                                       b != 0, (uint32_t)c);
        Serial.printf("meta wf%02d -> %d\n", a, ok ? 1 : 0);
        return;
    }

    // step <wf> <st> <t|a> <id> <pname> <pval>
    //   pname = "-" 表示该 Step 无参数
    {
        char pname[24] = {0};
        int pval = 0;
        if (sscanf(op.c_str(), "step %d %d %1s %31s %23s %d",
                   &a, &b, kind, sid, pname, &pval) >= 5)
        {
            WorkflowParamValue pv;
            bool has_param = (strcmp(pname, "-") != 0);
            pv.name = has_param ? String(pname) : String("");
            pv.type = PARAM_INT;
            pv.int_value = pval;
            pv.float_value = 0.0f;
            pv.bool_value = false;
            pv.string_value = "";

            bool is_trigger = (kind[0] == 't');
            bool ok = workflow_set_step(
                (uint8_t)a, (uint8_t)b,
                is_trigger ? WORKFLOW_STEP_TRIGGER : WORKFLOW_STEP_ACTION,
                is_trigger ? INSTANCE_TRIGGER : INSTANCE_ACTION,
                String(sid),
                has_param ? &pv : nullptr,
                has_param ? 1 : 0);

            Serial.printf("step wf%02d s%02d %s id=%s %s=%d -> %d\n",
                          a, b, kind, sid, pname, pval, ok ? 1 : 0);
            return;
        }
    }

    if (sscanf(op.c_str(), "count %d %d", &a, &b) == 2)
    {
        bool ok = workflow_set_step_count((uint8_t)a, (uint8_t)b);
        Serial.printf("count wf%02d=%d -> %d\n", a, b, ok ? 1 : 0);
        return;
    }

    if (sscanf(op.c_str(), "del %d", &a) == 1)
    {
        bool ok = workflow_delete((uint8_t)a);
        Serial.printf("del wf%02d -> %d\n", a, ok ? 1 : 0);
        return;
    }

    {
        int pidx = 0, val = 0;
        if (sscanf(op.c_str(), "param %d %d %d %d", &a, &b, &pidx, &val) == 4)
        {
            WorkflowParamValue pv;
            pv.name = "v";
            pv.type = PARAM_INT;
            pv.int_value = val;
            pv.float_value = 0.0f;
            pv.bool_value = false;
            pv.string_value = "";
            bool ok = workflow_update_step_param(
                (uint8_t)a, (uint8_t)b, (uint8_t)pidx, pv);
            Serial.printf("param wf%02d s%02d p%d=%d -> %d\n",
                          a, b, pidx, val, ok ? 1 : 0);
            return;
        }
    }

    if (sscanf(op.c_str(), "def %d %d", &a, &b) == 2)
    {
        WorkflowStepDef *d = workflow_step_def_at((uint8_t)a, (uint8_t)b);
        if (d == nullptr)
        {
            Serial.println("def: null");
            return;
        }
        Serial.printf("def wf%02d s%02d type=%d inst=%d id=%s n=%u p0=%d\n",
                      a, b, (int)d->type, (int)d->instance_type,
                      d->id.c_str(), (unsigned)d->param_count,
                      d->param_count ? d->params[0].int_value : -999);
        return;
    }

    if (sscanf(op.c_str(), "stop %d", &a) == 1)
    {
        Workflow *w = workflow_get((uint8_t)a);
        if (w == nullptr)
        {
            Serial.println("stop: null workflow");
            return;
        }
        bool ok = workflow_stop(w->id);
        Serial.printf("stop wf%02d -> %d state=%d\n", a, ok ? 1 : 0, (int)w->state);
        return;
    }

    if (op.startsWith("dirty"))
    {
        Serial.printf("dirty=%u\n", workflow_has_any_dirty() ? 1u : 0u);
        return;
    }

    if (op.startsWith("save"))
    {
        bool ok = workflow_save_transaction();
        Serial.printf("save -> %d (dirty=%u)\n", ok ? 1 : 0,
                      workflow_has_any_dirty() ? 1u : 0u);
        return;
    }

    if (op.startsWith("migrate"))
    {
        bool ok = workflow_migrate_to_storage();
        Serial.printf("migrate -> %d\n", ok ? 1 : 0);
        return;
    }

    {
        int skip = 0;
        int n = sscanf(op.c_str(), "run %d %d", &a, &skip);
        if (n >= 1)
        {
            Workflow *w = workflow_get((uint8_t)a);
            if (w == nullptr)
            {
                Serial.println("run: null workflow");
                return;
            }
            // skip=1 → 跳过第 0 步 Trigger，直接进入 Action（无硬件等待的快速验证）
            bool ok = workflow_start(w, n >= 2 && skip != 0);
            Serial.printf("run wf%02d skip=%d -> %d state=%d\n",
                          a, skip, ok ? 1 : 0, (int)w->state);
            return;
        }
    }

    Serial.printf("unknown wfc op: %s\n", op.c_str());
}

// =====================================================
// CommandManager 直通控制台（cm 命令，仅上板自测用）
//
// 作用：不经过 MQTT，直接把一条云端命令喂给 CommandManager，
// 走的是与云端完全相同的路由链（一级路由 → 二级路由 → handler
// → 结果 JSON 上报），因此串口测通 ≈ 云端可用。
//
// 输入格式（与 CloudManager 的下行 JSON 一致，长短字段都接受）：
//   cm {"cmd":"workflow.list","id":"t1"}
//   cm {"cmd":"workflow.get","id":"t2","p":{"stable_id":0}}
//   cm {"cmd":"workflow.set","id":"t3","p":{"stable_id":0,"workflow":{...}}}
//
// 字段映射：
//   cmd → CommandMessage.command
//   ob  → CommandMessage.object
//   id  → CommandMessage.cmd_id（缺省自动补 cm<millis>）
//   p / pl → 序列化后存入 CommandMessage.payload
//
// 执行期间临时打开结果回显，结果 JSON 会以
//   [CMD][RESULT] {...}
// 打印出来（生产路径走 MQTT，不开回显，避免刷屏）。
// =====================================================
void cm_console(const String &cmd)
{
    String json = cmd.substring(3);
    json.trim();

    if (json.length() == 0)
    {
        Serial.println("cm: usage: cm {\"cmd\":\"workflow.list\",\"id\":\"t1\"}");
        return;
    }

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, json);
    if (err)
    {
        Serial.printf("cm: bad json (%s)\n", err.c_str());
        return;
    }

    CommandMessage msg;
    msg.command = doc["cmd"] | "";
    msg.object  = doc["ob"]  | "";
    msg.cmd_id  = doc["id"]  | "";
    msg.source  = "serial";
    msg.timestamp = millis();

    if (msg.cmd_id.length() == 0)
    {
        msg.cmd_id = "cm" + String(millis());
    }

    JsonVariant pv = doc.containsKey("p") ? doc["p"] : doc["pl"];
    if (!pv.isNull())
    {
        serializeJson(pv, msg.payload);
    }

    Serial.printf("cm: cmd=%s ob=%s id=%s payload=%s\n",
                  msg.command.c_str(),
                  msg.object.c_str(),
                  msg.cmd_id.c_str(),
                  msg.payload.length() ? msg.payload.c_str() : "(empty)");

    command_manager_set_result_echo(true);
    bool ok = command_manager_execute(msg);
    command_manager_set_result_echo(false);

    Serial.printf("cm: ret=%d\n", ok ? 1 : 0);
}
