// =====================================================
// LogManager 冻结契约（P1.1）
//
// 本文件**只定义冻结契约，不包含任何实现**。
// 所有内容来自（不得自行增/删/重编号）：
//   log模块历史/LogManager-P1契约冻结0915.md   （权威）
//   log模块历史/LogManager详细设计规划0915.md  （§7.2 / §9 / 附录 A）
//
// 冻结范围（评审已通过，除非实机证明有错否则不得重新设计）：
//   Level Policy · LogRecord v2/128B · EventId · ParamId
//   seq/boot_seq · Flash Segment · Cloud Batch · ACK
//   at-least-once · FORCE_ADVANCE 默认关闭 · CRITICAL 语义
//   独立 Log Topic · 独立 Log Queue · 不接管旧文本 Log callback
//
// 本文件由 main.cpp 包含，使下列 static_assert 参与编译期校验。
// =====================================================

#ifndef LOG_EVENTS_H
#define LOG_EVENTS_H

#include <stdint.h>
#include <stddef.h>

// =====================================================
// 1. Level
// =====================================================

// ⚠️ 命名冲突说明（P1.1 实机编译发现）：
//    NimBLE-Arduino 的 log_common.h 无条件 #define 了
//    LOG_LEVEL_DEBUG/INFO/WARN/ERROR/CRITICAL（值 0..4），
//    因此本枚举不能沿用 LOG_LEVEL_* 前缀，改用 LOG_LVL_*。
//    ★ 仅改标识符名，Level 取值与策略完全不变（仍为冻结设计）。
enum LogLevel : uint8_t
{
    LOG_LVL_DEBUG    = 0,
    LOG_LVL_INFO     = 1,
    LOG_LVL_WARN     = 2,
    LOG_LVL_ERROR    = 3,
    LOG_LVL_CRITICAL = 4,
    LOG_LVL_COUNT    = 5
};

// =====================================================
// 2. Level Policy（冻结，唯一策略）
//
//    Level     Flash   Cloud
//    DEBUG      NO      NO
//    INFO       NO      YES
//    WARN       YES     YES
//    ERROR      YES     YES
//    CRITICAL   YES     YES
//
// ★ 无任何 EventId 级例外
// ★ 禁止 persist / flash_override / force_flash
//   （"配网掉电"等需求应另做独立的状态持久化机制，不给 Level 开洞）
// =====================================================

struct LogLevelPolicy
{
    uint8_t flash;   // 0/1，用 uint8_t 避免 bool 在 packed 场景的歧义
    uint8_t cloud;
};

constexpr LogLevelPolicy LOG_LEVEL_POLICY[LOG_LVL_COUNT] = {
    { 0, 0 },   // DEBUG
    { 0, 1 },   // INFO
    { 1, 1 },   // WARN
    { 1, 1 },   // ERROR
    { 1, 1 }    // CRITICAL
};

constexpr bool log_level_to_flash(LogLevel l)
{
    return LOG_LEVEL_POLICY[(uint8_t)l].flash != 0;
}

constexpr bool log_level_to_cloud(LogLevel l)
{
    return LOG_LEVEL_POLICY[(uint8_t)l].cloud != 0;
}

// =====================================================
// 3. 参数类型
// =====================================================

enum LogParamType : uint8_t
{
    LOG_PTYPE_I32  = 1,
    LOG_PTYPE_U32  = 2,
    LOG_PTYPE_F32  = 3,
    LOG_PTYPE_BOOL = 4,
    LOG_PTYPE_ENUM = 5,
    LOG_PTYPE_STR  = 6,   // 值 = (blob_off << 16) | blob_len，字符存于 blob
    LOG_PTYPE_I8   = 7,
    LOG_PTYPE_U16  = 8
};

// =====================================================
// 4. ParamId（冻结，全局共享，跨事件复用）
//
// 类型见设计报告附录 A 的字典表。
// ⚠️ 已确认（P1.1 收尾）：
//    LOG_P_BOOT_SEQ 保持 Param ID 0x21，类型为 uint32_t
//    （与 LogRecord 的 boot_seq 字段一致）。
//    设计报告附录 A 字典表标注 U16 属笔误，以 uint32_t 为准。
//    ★ ID 与数值布局不变。
// =====================================================

enum LogParamId : uint8_t
{
    LOG_P_NONE             = 0x00,
    LOG_P_SLOT             = 0x01,
    LOG_P_WF_ID            = 0x02,
    LOG_P_VARIANT          = 0x03,
    LOG_P_VARIANT_PREV     = 0x04,
    LOG_P_TRIGGER_SOURCE   = 0x05,
    LOG_P_DURATION_MS      = 0x06,
    LOG_P_STEPS_DONE       = 0x07,
    LOG_P_TIMEOUT_MS       = 0x08,
    LOG_P_STUCK_STEP       = 0x09,
    LOG_P_FAIL_STEP        = 0x0A,
    LOG_P_ACTION_ID        = 0x0B,
    LOG_P_ERR_CODE         = 0x0C,
    LOG_P_TARGET_G         = 0x0D,
    LOG_P_START_G          = 0x0E,
    LOG_P_FINAL_G          = 0x0F,
    LOG_P_DELTA_G          = 0x10,
    LOG_P_STOP_REASON      = 0x11,
    LOG_P_SOURCE           = 0x12,
    LOG_P_OPEN_MS          = 0x13,
    LOG_P_LIMIT_MS         = 0x14,
    LOG_P_WEIGHT_G         = 0x15,
    LOG_P_CAUSE            = 0x16,
    LOG_P_JUMP_COUNT       = 0x17,
    LOG_P_RAW              = 0x18,
    LOG_P_FILTERED         = 0x19,
    LOG_P_OFFSET           = 0x1A,
    LOG_P_SAMPLES          = 0x1B,
    LOG_P_SAVED            = 0x1C,
    LOG_P_MODULE           = 0x1D,
    LOG_P_KEY              = 0x1E,
    LOG_P_REASON           = 0x1F,
    LOG_P_RESET_REASON     = 0x20,
    LOG_P_BOOT_SEQ         = 0x21,
    LOG_P_INIT_MS          = 0x22,
    LOG_P_LOADED           = 0x23,
    LOG_P_TOTAL            = 0x24,
    LOG_P_PATH             = 0x25,
    LOG_P_STAGE            = 0x26,
    LOG_P_STORED_CRC       = 0x27,
    LOG_P_CALC_CRC         = 0x28,
    LOG_P_RETRY_N          = 0x29,
    LOG_P_SLEEP_MS         = 0x2A,
    LOG_P_OUTBOX           = 0x2B,
    LOG_P_CONNECTED_MS     = 0x2C,
    LOG_P_CMD_ID           = 0x2D,
    LOG_P_CMD              = 0x2E,
    LOG_P_OBJ              = 0x2F,
    LOG_P_ATTEMPT_N        = 0x30,
    LOG_P_RSSI             = 0x31,
    LOG_P_CONNECT_MS       = 0x32,
    LOG_P_SSID_HASH        = 0x33,
    LOG_P_SERVER           = 0x34,
    LOG_P_UNIX             = 0x35,
    LOG_P_DRIFT_MS         = 0x36,
    LOG_P_ADDR             = 0x37,
    LOG_P_WHICH_POOL       = 0x38,
    LOG_P_NEED_BYTES       = 0x39,
    LOG_P_FREE_BYTES       = 0x3A,
    LOG_P_TEMP             = 0x3B,
    LOG_P_HUMID            = 0x3C,
    LOG_P_BATT_V           = 0x3D,
    LOG_P_MAC_SUFFIX       = 0x3E,
    LOG_P_FAIL_COUNT       = 0x3F,
    LOG_P_DROPPED_TOTAL    = 0x40,
    LOG_P_ACTIVE           = 0x41,
    LOG_P_CAPACITY         = 0x42,
    LOG_P_HOLD_MS          = 0x43,
    LOG_P_STATE            = 0x44,
    LOG_P_COUNT            = 0x45,
    LOG_P_OP               = 0x46,
    LOG_P_WAS              = 0x47,
    LOG_P_VALVE_OPEN_MS    = 0x48,
    LOG_P_GAIN_AFTER_CLOSE_G = 0x49,
    LOG_P_WINDOW_MS        = 0x4A,
    LOG_P_VERSION          = 0x4B,
    LOG_P_CHECKSUM         = 0x4C,
    LOG_P_REG_TYPE         = 0x4D,
    LOG_P_STAGED_COUNT     = 0x4E,
    LOG_P_ARCHIVE_ACTION   = 0x4F,
    LOG_P_LAST_OK_AGE_MS   = 0x50,
    LOG_P_BLE_LEVEL        = 0x51,
    LOG_P_FAIL_KIND        = 0x52,
    LOG_P_QUEUE_SIZE       = 0x53,
    LOG_P_DROP_RING        = 0x54,
    LOG_P_DROP_OVERFLOW    = 0x55,
    LOG_P_MIN_FREE         = 0x56,
    LOG_P_PENDING          = 0x57,
    LOG_P_REMAIN_MS        = 0x58,
    LOG_P_MAX              = 0x59
};

// =====================================================
// 5. EventId（冻结，段式 ID）
//    0x01xx System/Boot  0x02xx Config   0x03xx Storage
//    0x04xx Workflow     0x05xx Water    0x06xx WiFi
//    0x07xx MQTT/Cloud/Log 0x08xx Time   0x09xx BLE
//    0x0Axx Command      0x0Bxx Registry 0x0Cxx Event
//    0x0Dxx ComputerReset 0x0Exx OLED    0x0Fxx Motor（预留）
// =====================================================

enum LogEventId : uint16_t
{
    LOG_EVT_NONE = 0x0000,

    // ---- 0x01xx System / Boot / Restart ----
    LOG_SYS_BOOT_COMPLETE          = 0x0101,   // INFO
    LOG_SYS_BOOT_INCOMPLETE_PREV   = 0x0102,   // CRITICAL
    LOG_SYS_RESET_ABNORMAL         = 0x0103,   // CRITICAL
    LOG_SYS_RESET_NORMAL           = 0x0104,   // INFO
    LOG_SYS_INIT_FAILED            = 0x0105,   // ERROR
    LOG_SYS_FS_MOUNT_FAILED        = 0x0106,   // CRITICAL（FS 不可用，实际无法落盘）
    LOG_SYS_PSRAM_ALLOC_FAILED     = 0x0107,   // CRITICAL
    LOG_SYS_HEAP_LOW               = 0x0108,   // WARN
    LOG_SYS_RESTART_REQUESTED      = 0x0109,   // INFO
    LOG_SYS_RESTART_EXECUTED       = 0x010A,   // INFO（IMM）
    LOG_SYS_RESTART_CANCELLED      = 0x010B,   // INFO
    LOG_SYS_CRITICAL_OP_UNDERFLOW  = 0x010C,   // CRITICAL

    // ---- 0x02xx Config ----
    LOG_CFG_LOAD_DONE              = 0x0201,   // INFO
    LOG_CFG_MODULE_LOAD_FAILED     = 0x0202,   // ERROR
    LOG_CFG_CHANGE_APPLIED         = 0x0203,   // INFO
    LOG_CFG_SAVE_OK                = 0x0204,   // INFO
    LOG_CFG_COMMIT_FAILED_ROLLBACK = 0x0205,   // ERROR
    LOG_CFG_RECOVERED_FROM_BACKUP  = 0x0206,   // WARN
    LOG_CFG_VERSION_REBUILT        = 0x0207,   // WARN
    LOG_CFG_FACTORY_RESET          = 0x0208,   // WARN
    LOG_CFG_WRITE_REJECTED         = 0x0209,   // WARN
    LOG_CFG_RESTART_TIMEOUT        = 0x020A,   // INFO

    // ---- 0x03xx Storage ----
    LOG_STG_FS_UNAVAILABLE         = 0x0301,   // CRITICAL（FS 不可用，实际无法落盘）
    LOG_STG_ATOMIC_WRITE_FAILED    = 0x0302,   // ERROR
    LOG_STG_CRC_FAILED             = 0x0303,   // CRITICAL
    LOG_STG_TXN_RECOVERED          = 0x0304,   // WARN
    LOG_STG_WRITE_VERIFY_FAILED    = 0x0305,   // ERROR
    LOG_STG_READ_FAILED            = 0x0306,   // WARN

    // ---- 0x04xx Workflow ----
    LOG_WF_START                   = 0x0401,   // INFO
    LOG_WF_FINISHED                = 0x0402,   // INFO
    LOG_WF_TIMEOUT                 = 0x0403,   // WARN
    LOG_WF_FAILED                  = 0x0404,   // WARN
    LOG_WF_ACTION_FAILED           = 0x0405,   // WARN
    LOG_WF_SAVE_FAILED             = 0x0406,   // WARN
    LOG_WF_SAVE_PARTIAL            = 0x0407,   // WARN
    LOG_WF_CRUD                    = 0x0408,   // INFO
    LOG_WF_MIGRATED                = 0x0409,   // INFO
    LOG_WF_TEMP_ACTION_TIMEOUT     = 0x040A,   // WARN
    LOG_WF_RUNTIME_ALLOC_FAILED    = 0x040B,   // ERROR
    LOG_WF_SAVE_PARTIAL_RETRY_OK   = 0x040C,   // INFO（ID 保留不变；是否实现待 P2 决定）

    // ---- 0x05xx Water（Dispense / Valve / Weight）----
    LOG_DISPENSE_START             = 0x0501,   // INFO
    LOG_DISPENSE_DONE              = 0x0502,   // INFO
    LOG_DISPENSE_FAILED            = 0x0503,   // WARN
    LOG_DISPENSE_TIMEOUT           = 0x0504,   // WARN
    LOG_VALVE_FORCE_CLOSE          = 0x0505,   // CRITICAL（IMM）
    LOG_VALVE_FORCE_CLOSE_FAILED   = 0x0506,   // CRITICAL（IMM）
    LOG_VALVE_OVERFLOW_RISK        = 0x0507,   // CRITICAL（IMM，需新增检测）
    LOG_VALVE_SAFETY_TIMEOUT       = 0x0508,   // WARN
    LOG_VALVE_OPEN                 = 0x0509,   // INFO
    LOG_VALVE_CLOSE                = 0x050A,   // INFO
    LOG_VALVE_RATE_LIMITED         = 0x050B,   // WARN
    LOG_WEIGHT_ERROR_ENTER         = 0x050C,   // WARN
    LOG_WEIGHT_ERROR_EXIT          = 0x050D,   // INFO
    LOG_WEIGHT_ZERO_DONE           = 0x050E,   // INFO
    LOG_WEIGHT_TRIGGER_FIRED       = 0x050F,   // INFO
    LOG_WEIGHT_CALIB_FAILED        = 0x0510,   // ERROR
    // Phase 4：DispenseGuard 安全响应决策（不是"重量异常"也不是"关阀动作"）
    //   语义 = "Dispense 响应了 EVENT_WEIGHT_ERROR 并决定发起安全动作"
    //   已有记录的分工：重量异常 → LOG_WEIGHT_ERROR_ENTER(0x050C)（weight 定义点）
    //                   关阀动作 → LOG_VALVE_FORCE_CLOSE(0x0505)（valve 定义点）
    //   为何需要独立 ID：0x0505 带 5s 日志门控 ⇒ 只记"发生过"，不记"发生了几次"；
    //                   本 ID 走 LogManager 突发合并（ΣCOUNT 守恒）⇒ 次数权威。
    //   为何不复用 0x0501-0x0504：四个 ID 描述"一次供水过程"的生命周期，
    //                   与"安全响应决策"范畴不同（安全响应是终止供水，非开始）。
    LOG_DISPENSE_SAFETY_RESPONSE   = 0x0511,   // WARN（走突发合并；Σ COUNT = 真实响应次数）

    // ---- 0x06xx WiFi ----
    LOG_WIFI_CONNECT_START         = 0x0601,   // INFO
    LOG_WIFI_CONNECTED             = 0x0602,   // INFO
    LOG_WIFI_CONNECT_TIMEOUT       = 0x0603,   // WARN
    LOG_WIFI_LOST                  = 0x0604,   // WARN
    LOG_WIFI_RECONNECT_TRY         = 0x0605,   // WARN
    LOG_WIFI_PROVISION_ENTER       = 0x0606,   // INFO（P1 撤销 Flash 例外 → Flash NO）
    LOG_WIFI_PROVISION_DONE        = 0x0607,   // INFO（P1 撤销 Flash 例外 → Flash NO）

    // ---- 0x07xx MQTT / Cloud / Log 自身 ----
    LOG_MQTT_CONNECTED             = 0x0701,   // INFO
    LOG_MQTT_DISCONNECTED          = 0x0702,   // WARN
    LOG_MQTT_SLEEP_ENTER           = 0x0703,   // ERROR（IMM）
    LOG_MQTT_PUBLISH_FAIL          = 0x0704,   // WARN
    LOG_MQTT_CMD_EXEC_FAILED       = 0x0705,   // WARN
    LOG_CLOUD_FRAG_FAIL            = 0x0706,   // WARN
    LOG_LOG_UPLOAD_FAIL            = 0x0707,   // WARN（LogManager 自身故障；策略仍为 Flash + Cloud）
    LOG_LOG_ACK_TIMEOUT            = 0x0708,   // WARN（策略 Flash + Cloud）
    LOG_LOG_RING_OVERFLOW          = 0x0709,   // WARN（策略 Flash + Cloud；段环已满时物理无处可写，改由批次头侧信道计数上报）
    LOG_LOG_ACK_LOST               = 0x070A,   // WARN
    LOG_LOG_SELF_DEGRADED          = 0x070B,   // ERROR

    // ---- 0x08xx Time / RTC ----
    LOG_TIME_NTP_OK                = 0x0801,   // INFO
    LOG_TIME_NTP_FAIL              = 0x0802,   // WARN
    LOG_TIME_VALID_ENTER           = 0x0803,   // INFO（P1 撤销 Flash 例外 → Flash NO）
    LOG_TIME_INVALID_ENTER         = 0x0804,   // WARN
    LOG_TIME_RTC_PROBE             = 0x0805,   // INFO（探测成功）/ WARN（探测失败）；Flash 由 Level Policy 决定
    LOG_TIME_RTC_BOOT_RESTORE      = 0x0806,   // INFO
    LOG_TIME_RTC_CALIBRATED        = 0x0807,   // INFO
    LOG_TIME_RTC_WRITE_FAILED      = 0x0808,   // WARN
    LOG_TIME_RTC_VL_FLAG           = 0x0809,   // WARN
    LOG_TIME_RTC_BCD_INVALID       = 0x080A,   // WARN

    // ---- 0x09xx Mijia BLE ----
    LOG_BLE_DATA_DECODED           = 0x0901,   // INFO
    LOG_BLE_DECODE_FAIL            = 0x0902,   // WARN（限流）
    LOG_BLE_SENSOR_LOST            = 0x0903,   // WARN
    LOG_BLE_SCAN_DISABLED          = 0x0904,   // INFO

    // ---- 0x0Axx Command ----
    LOG_CMD_RUNTIME_QUEUE_FULL     = 0x0A01,   // WARN
    LOG_CMD_RUNTIME_TIMEOUT        = 0x0A02,   // WARN
    LOG_CMD_REJECTED               = 0x0A03,   // WARN
    LOG_CMD_APPLIED                = 0x0A04,   // INFO

    // ---- 0x0Bxx Capability Registry ----
    LOG_REG_REBUILT                = 0x0B01,   // INFO
    LOG_REG_SAVE_FAILED            = 0x0B02,   // ERROR

    // ---- 0x0Cxx Event Manager ----
    LOG_EVT_QUEUE_FULL             = 0x0C01,   // WARN（策略 Flash + Cloud；走侧信道计数上报）
    LOG_EVT_STORM_DROPPED          = 0x0C02,   // WARN（策略 Flash + Cloud；走侧信道计数上报）

    // ---- 0x0Dxx ComputerReset ----
    LOG_CRESET_PULSE               = 0x0D01,   // INFO
    LOG_CRESET_SAFETY_TIMEOUT      = 0x0D02,   // WARN
    LOG_CRESET_POOL_EXHAUSTED      = 0x0D03,   // ERROR

    // ---- 0x0Exx OLED ----
    LOG_OLED_INIT_FAILED           = 0x0E01,   // WARN

    // ---- 0x0Fxx Motor（未来模块，ID 段已预留）----
    LOG_MOTOR_RESERVED_BASE        = 0x0F00
};

// =====================================================
// 6. State Context
// =====================================================

enum LogContextKind : uint8_t
{
    LOG_CTX_NONE     = 0,   // blob 无内容
    LOG_CTX_SYSTEM   = 1,   // 12 B
    LOG_CTX_WORKFLOW = 2,   // 16 B
    LOG_CTX_DISPENSE = 3,   // 20 B
    LOG_CTX_WEIGHT   = 4,   // 16 B
    LOG_CTX_MQTT     = 5,   // 12 B
    LOG_CTX_STRING   = 6    // 纯短字符串池
};

// =====================================================
// 7. Record flags（冻结）
//
//   bit0 timestamp_valid
//   bit1 context_present
//   bit2..7 保留（写 0，解码必须忽略）
//
// ★ 不存在 LOG_FLAG_PERSIST
//     理由：持久化由 §2 的 Level Policy 唯一决定，
//          不是 Record 自身的生命周期状态。
// ★ 不存在 LOG_FLAG_UPLOADED
//     理由：与"零原地修改"直接矛盾。LittleFS 是 COW，
//          回写该位会导致整个 block 重写。
//          上传状态由"所在段文件是否仍存在"表达。
//
// 不变量：LogRecord 不知道自己是否已被上传，
//         也不知道自己是否该被持久化。
// =====================================================

#define LOG_FLAG_TIMESTAMP_VALID  0x01u
#define LOG_FLAG_CONTEXT_PRESENT  0x02u
#define LOG_FLAG_DEFINED_MASK     0x03u   // 仅此两位有定义

// =====================================================
// 8. LogRecord v2（128 B 定长）
//
//   偏移 长度 字段
//   0    1    version      = 2
//   1    1    level
//   2    1    flags
//   3    1    param_count
//   4    4    seq          uint32（允许空洞，不允许重复）
//   8    4    boot_seq     uint32（P1 由 uint16 升级）
//   12   2    event_id
//   14   2    packed       bit0-2 context_kind
//   16   4    uptime_ms
//   20   4    timestamp    0 = 无效
//   24   2    blob_len
//   26   2    reserved16
//   28   48   params[8]    每项 { id:u8, type:u8, value:u32le }
//   76   32   blob
//   108  4    crc32        覆盖 [0..107]
//   112  16   reserved
// =====================================================

#define LOG_RECORD_VERSION        2u
#define LOG_RECORD_SIZE           128u
#define LOG_MAX_PARAMS            8u
#define LOG_PARAM_SIZE            6u
#define LOG_BLOB_SIZE             32u

#define LOG_OFF_VERSION           0u
#define LOG_OFF_LEVEL             1u
#define LOG_OFF_FLAGS             2u
#define LOG_OFF_PARAM_COUNT       3u
#define LOG_OFF_SEQ               4u
#define LOG_OFF_BOOT_SEQ          8u
#define LOG_OFF_EVENT_ID          12u
#define LOG_OFF_PACKED            14u
#define LOG_OFF_UPTIME_MS         16u
#define LOG_OFF_TIMESTAMP         20u
#define LOG_OFF_BLOB_LEN          24u
#define LOG_OFF_RESERVED16        26u
#define LOG_OFF_PARAMS            28u
#define LOG_OFF_BLOB              76u
#define LOG_OFF_CRC32             108u
#define LOG_OFF_RESERVED          112u

// 内存中的参数构造形态（调用方栈上；sizeof == 8，含 2 B padding）
struct LogParamIn
{
    uint8_t id;      // LogParamId
    uint8_t type;    // LogParamType
    union
    {
        int32_t  i;
        uint32_t u;
        float    f;
        uint8_t  b;
    } v;
};

// Flash / Wire 语义记录（packed，128 B）
struct __attribute__((packed)) LogRecord
{
    uint8_t  version;
    uint8_t  level;
    uint8_t  flags;
    uint8_t  param_count;
    uint32_t seq;
    uint32_t boot_seq;
    uint16_t event_id;
    uint16_t packed;
    uint32_t uptime_ms;
    uint32_t timestamp;
    uint16_t blob_len;
    uint16_t reserved16;
    uint8_t  params[LOG_MAX_PARAMS * LOG_PARAM_SIZE];  // 48 B
    uint8_t  blob[LOG_BLOB_SIZE];                      // 32 B
    uint32_t crc32;
    uint8_t  reserved[16];
};

// =====================================================
// 9. Flash Segment（冻结）
//
//   段头 16 B + 31 × 128 B = 3984 B（逻辑段大小）
//   ★ 设计目标：适配 4 KB block（3984 < 4096），
//     不对 LittleFS 物理层布局做严格映射假设
//   段数 16 ⇒ 容量 496 条 ≈ 63.7 KB
//   文件名 s%07u.log（字典序 = 数值序）
//
//   原则：整段创建 / 整段追加 / 整段删除
//   禁止：ACK 后修改 Record 内 uploaded bit
// =====================================================

#define LOG_SEGMENT_HEADER_SIZE   16u
#define LOG_RECORDS_PER_SEGMENT   31u
#define LOG_SEGMENT_SIZE          3984u
#define LOG_SEGMENT_COUNT         16u
#define LOG_SEGMENT_CAPACITY      496u   // 31 × 16

// =====================================================
// 10. meta.bin（32 B，fmt_version = 2，可重建的加速缓存）
//
//   0   4  magic        0x474C4F47 ("GLOG")
//   4   1  fmt_version  = 2
//   5   1  reserved
//   6   2  reserved16
//   8   4  boot_seq      uint32
//   12  4  seq_reserved  uint32（已预留出去的 seq 高水位）
//   16  4  corrupt_count uint32
//   20  8  reserved
//   28  4  crc32         覆盖 [0..27]
// =====================================================

#define LOG_META_MAGIC            0x474C4F47u
#define LOG_META_VERSION          2u
#define LOG_META_SIZE             32u
#define LOG_SEQ_RESERVE           256u

// =====================================================
// 11. Cloud Batch / ACK（冻结）
//
//   传输语义：Device = at-least-once，Cloud = 幂等去重
//   ACK 语义：云端已持久化该 seq 区间
//             ★ 不代表该区间不会再次出现
// =====================================================

#define LOG_BATCH_FMT             2u
#define LOG_BATCH_MAX_RECORDS     16u
#define LOG_BATCH_MAX_PAYLOAD     4096u
#define LOG_TX_MIN_INTERVAL_MS    500u
#define LOG_FLASH_MIN_INTERVAL_MS 20u
#define LOG_FLUSH_RECORDS         8u
#define LOG_FLUSH_MS              5000u

#define LOG_ACK_TIMEOUT_MS        15000u
#define LOG_ACK_MAX_RETRY         5u
#define LOG_ACK_BACKOFF_MAX_MS    60000u

// CBOR 批次头整数键（冻结，共 12 个，键值 0..11，不得重编号）
#define LOG_BKEY_FMT              0u
#define LOG_BKEY_EVENT_DICT_VER   1u
#define LOG_BKEY_BOOT_SEQ         2u
#define LOG_BKEY_SEQ_FROM         3u
#define LOG_BKEY_SEQ_TO           4u
#define LOG_BKEY_COUNT            5u
#define LOG_BKEY_DROP_RING        6u
#define LOG_BKEY_DROP_OVERFLOW    7u
#define LOG_BKEY_DROP_UNACKED     8u
#define LOG_BKEY_SELF_DEGRADED    9u
#define LOG_BKEY_FLAGS            10u
#define LOG_BKEY_RECORDS          11u

// flags.bit0：seq_reliable（0 = 区间预留失败，该批可能重复）
#define LOG_BATCH_FLAG_SEQ_RELIABLE 0x01u

// log_ack 下行报文（走 down Topic，旁路 CommandManager）
//   {"c":"log_ack","i":"<id>","p":{"b":<boot_seq>,"f":<seq_from>,"t":<seq_to>}}
#define LOG_ACK_COMMAND           "log_ack"
#define LOG_ACK_P_BOOT_SEQ        "b"
#define LOG_ACK_P_SEQ_FROM        "f"
#define LOG_ACK_P_SEQ_TO          "t"

// =====================================================
// 12. Contract Test（编译期）
//
// 覆盖：Record size / 段尺寸 / 段数 / Level Policy /
//       seq·boot_seq 为 uint32 / flags 仅冻结字段 /
//       不存在 uploaded·persist /
//       EventId·ParamId 关键值 / offsets
// =====================================================

static_assert(sizeof(LogRecord) == LOG_RECORD_SIZE,
              "LogRecord must be exactly 128 bytes");

static_assert(LOG_SEGMENT_SIZE ==
                  LOG_SEGMENT_HEADER_SIZE + LOG_RECORDS_PER_SEGMENT * LOG_RECORD_SIZE,
              "segment size must equal 16 + 31*128 = 3984");

static_assert(LOG_SEGMENT_SIZE < 4096u,
              "segment logical size target: within one 4KB block");

static_assert(LOG_SEGMENT_CAPACITY == LOG_RECORDS_PER_SEGMENT * LOG_SEGMENT_COUNT,
              "capacity must equal 31*16 = 496");

static_assert(LOG_META_SIZE == 32u, "meta.bin must be 32 bytes");

// seq / boot_seq 必须是 uint32
static_assert(sizeof(((LogRecord *)0)->seq) == 4, "seq must be uint32");
static_assert(sizeof(((LogRecord *)0)->boot_seq) == 4, "boot_seq must be uint32");

// flags：仅 bit0/bit1 有定义，不存在 uploaded / persist
static_assert(LOG_FLAG_DEFINED_MASK == 0x03u,
              "only timestamp_valid and context_present may be defined");

// 布局偏移（冻结）
static_assert(offsetof(LogRecord, version)     == LOG_OFF_VERSION,     "offset version");
static_assert(offsetof(LogRecord, level)       == LOG_OFF_LEVEL,       "offset level");
static_assert(offsetof(LogRecord, flags)       == LOG_OFF_FLAGS,       "offset flags");
static_assert(offsetof(LogRecord, param_count) == LOG_OFF_PARAM_COUNT, "offset param_count");
static_assert(offsetof(LogRecord, seq)         == LOG_OFF_SEQ,         "offset seq");
static_assert(offsetof(LogRecord, boot_seq)    == LOG_OFF_BOOT_SEQ,    "offset boot_seq");
static_assert(offsetof(LogRecord, event_id)    == LOG_OFF_EVENT_ID,    "offset event_id");
static_assert(offsetof(LogRecord, packed)      == LOG_OFF_PACKED,      "offset packed");
static_assert(offsetof(LogRecord, uptime_ms)   == LOG_OFF_UPTIME_MS,   "offset uptime_ms");
static_assert(offsetof(LogRecord, timestamp)   == LOG_OFF_TIMESTAMP,   "offset timestamp");
static_assert(offsetof(LogRecord, blob_len)    == LOG_OFF_BLOB_LEN,    "offset blob_len");
static_assert(offsetof(LogRecord, params)      == LOG_OFF_PARAMS,      "offset params");
static_assert(offsetof(LogRecord, blob)        == LOG_OFF_BLOB,        "offset blob");
static_assert(offsetof(LogRecord, crc32)       == LOG_OFF_CRC32,       "offset crc32");
static_assert(offsetof(LogRecord, reserved)    == LOG_OFF_RESERVED,    "offset reserved");

// EventId 关键值（防止误重编号）
static_assert(LOG_EVT_NONE == 0x0000, "LOG_EVT_NONE");
static_assert(LOG_SYS_BOOT_COMPLETE == 0x0101, "LOG_SYS_BOOT_COMPLETE");
static_assert(LOG_WF_START == 0x0401, "LOG_WF_START");
static_assert(LOG_VALVE_FORCE_CLOSE == 0x0505, "LOG_VALVE_FORCE_CLOSE");
static_assert(LOG_DISPENSE_SAFETY_RESPONSE == 0x0511, "LOG_DISPENSE_SAFETY_RESPONSE");
static_assert(LOG_OLED_INIT_FAILED == 0x0E01, "LOG_OLED_INIT_FAILED");

// ParamId 关键值
static_assert(LOG_P_NONE == 0x00, "LOG_P_NONE");
static_assert(LOG_P_SLOT == 0x01, "LOG_P_SLOT");
static_assert(LOG_P_BOOT_SEQ == 0x21, "LOG_P_BOOT_SEQ");
static_assert(LOG_P_REMAIN_MS == 0x58, "LOG_P_REMAIN_MS");
static_assert(LOG_P_MAX == 0x59, "LOG_P_MAX");

// 常量表达式形式的 Contract 自检（与 static_assert 等价，便于集中查看）
//
// ⚠️ 工具链为 gnu++11：constexpr 函数体**只能是一条 return 语句**，
//    不得声明局部变量。因此此处写成单表达式。
constexpr bool log_contract_self_check()
{
    return
        // ---- Level Policy ----
        (!log_level_to_flash(LOG_LVL_DEBUG)    && !log_level_to_cloud(LOG_LVL_DEBUG))    &&
        (!log_level_to_flash(LOG_LVL_INFO)     &&  log_level_to_cloud(LOG_LVL_INFO))     &&
        ( log_level_to_flash(LOG_LVL_WARN)     &&  log_level_to_cloud(LOG_LVL_WARN))     &&
        ( log_level_to_flash(LOG_LVL_ERROR)    &&  log_level_to_cloud(LOG_LVL_ERROR))    &&
        ( log_level_to_flash(LOG_LVL_CRITICAL) &&  log_level_to_cloud(LOG_LVL_CRITICAL)) &&
        // ---- Record / Segment / meta 常量 ----
        (LOG_RECORD_SIZE == 128u) &&
        (LOG_RECORDS_PER_SEGMENT == 31u) &&
        (LOG_SEGMENT_SIZE == 3984u) &&
        (LOG_SEGMENT_COUNT == 16u) &&
        (LOG_SEGMENT_CAPACITY == 496u) &&
        (LOG_SEGMENT_SIZE < 4096u) &&
        (LOG_META_SIZE == 32u) &&
        // ---- Cloud ----
        (LOG_BATCH_FMT == 2u) &&
        (LOG_SEQ_RESERVE == 256u) &&
        // ---- flags 冻结（无 uploaded / persist）----
        (LOG_FLAG_DEFINED_MASK == 0x03u);
}

static_assert(log_contract_self_check(),
              "LogManager P1 contract self-check failed");

#endif // LOG_EVENTS_H
