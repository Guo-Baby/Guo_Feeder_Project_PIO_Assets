// =====================================================
// P1.1 Contract Test —— 正向探针（离线可跑，不依赖 Arduino/硬件）
//
// 目的：证明 log_events.h 的 static_assert 与 constexpr 自检
//       在本项目工具链（gnu++11）下**真的被求值并且通过**，
//       而不是被预处理器跳过。
//
// 编译（不需链接，仅语法/常量求值）：
//   xtensa-esp32s3-elf-g++ -std=gnu++11 -fsyntax-only -Isrc probe_ok.cpp
// =====================================================

#include "log_events.h"

// 显式把自检函数用作常量表达式：若其内部任一断言为假，编译失败
static_assert(log_contract_self_check(),
              "P1.1 contract self-check must be a valid constant expression");

// 关键冻结值二次显式断言（与头文件内断言互为印证）
static_assert(LOG_RECORD_SIZE == 128u,          "Record must be 128 B");
static_assert(sizeof(LogRecord) == 128u,        "sizeof(LogRecord) must be 128");
static_assert(LOG_RECORD_VERSION == 2u,         "Record version must be 2");
static_assert(LOG_SEGMENT_HEADER_SIZE == 16u,   "segment header must be 16 B");
static_assert(LOG_RECORDS_PER_SEGMENT == 31u,   "31 records per segment");
static_assert(LOG_SEGMENT_SIZE == 3984u,        "segment must be 3984 B");
static_assert(LOG_SEGMENT_COUNT == 16u,         "16 segments");
static_assert(LOG_SEGMENT_CAPACITY == 496u,     "496 record capacity");
static_assert(LOG_META_SIZE == 32u,             "meta.bin must be 32 B");
static_assert(LOG_META_VERSION == 2u,           "meta must be fmt v2");
static_assert(LOG_SEQ_RESERVE == 256u,          "seq reserve block = 256");
static_assert(LOG_BATCH_FMT == 2u,              "cloud batch fmt = 2");
static_assert(LOG_ACK_TIMEOUT_MS == 15000u,     "ack timeout 15 s");
static_assert(LOG_ACK_MAX_RETRY == 5u,          "ack max retry 5");

// seq / boot_seq 必须是 uint32
static_assert(sizeof(((LogRecord *)0)->seq) == 4u,      "seq must be uint32");
static_assert(sizeof(((LogRecord *)0)->boot_seq) == 4u, "boot_seq must be uint32");

// flags 只有两个冻结位；不存在 uploaded / persist
static_assert(LOG_FLAG_DEFINED_MASK == 0x03u,   "only 2 defined flag bits");
static_assert(LOG_FLAG_TIMESTAMP_VALID == 0x01u, "bit0 = timestamp_valid");
static_assert(LOG_FLAG_CONTEXT_PRESENT == 0x02u, "bit1 = context_present");

// Level Policy
static_assert(!log_level_to_flash(LOG_LVL_DEBUG)    && !log_level_to_cloud(LOG_LVL_DEBUG),    "DEBUG  no/no");
static_assert(!log_level_to_flash(LOG_LVL_INFO)     &&  log_level_to_cloud(LOG_LVL_INFO),     "INFO   no/yes");
static_assert( log_level_to_flash(LOG_LVL_WARN)     &&  log_level_to_cloud(LOG_LVL_WARN),     "WARN   yes/yes");
static_assert( log_level_to_flash(LOG_LVL_ERROR)    &&  log_level_to_cloud(LOG_LVL_ERROR),    "ERROR  yes/yes");
static_assert( log_level_to_flash(LOG_LVL_CRITICAL) &&  log_level_to_cloud(LOG_LVL_CRITICAL), "CRIT   yes/yes");

// Level 取值冻结
static_assert(LOG_LVL_DEBUG == 0 && LOG_LVL_INFO == 1 && LOG_LVL_WARN == 2 &&
              LOG_LVL_ERROR == 3 && LOG_LVL_CRITICAL == 4, "level values 0..4");

// EventId 抽样（段式范围 + 关键点）
static_assert(LOG_SYS_BOOT_COMPLETE == 0x0101,        "0x0101");
static_assert(LOG_CFG_LOAD_DONE == 0x0201,            "0x0201");
static_assert(LOG_STG_CRC_FAILED == 0x0303,           "0x0303");
static_assert(LOG_WF_SAVE_PARTIAL == 0x0407,          "0x0407");
static_assert(LOG_DISPENSE_START == 0x0501,           "0x0501");
static_assert(LOG_WEIGHT_CALIB_FAILED == 0x0510,      "0x0510");
static_assert(LOG_WIFI_PROVISION_DONE == 0x0607,      "0x0607");
static_assert(LOG_LOG_SELF_DEGRADED == 0x070B,        "0x070B");
static_assert(LOG_TIME_RTC_BCD_INVALID == 0x080A,     "0x080A");
static_assert(LOG_BLE_SCAN_DISABLED == 0x0904,        "0x0904");
static_assert(LOG_CMD_APPLIED == 0x0A04,              "0x0A04");
static_assert(LOG_REG_SAVE_FAILED == 0x0B02,          "0x0B02");
static_assert(LOG_EVT_STORM_DROPPED == 0x0C02,        "0x0C02");
static_assert(LOG_CRESET_POOL_EXHAUSTED == 0x0D03,    "0x0D03");
static_assert(LOG_OLED_INIT_FAILED == 0x0E01,         "0x0E01");

// ParamId 抽样
static_assert(LOG_P_NONE == 0x00,      "0x00");
static_assert(LOG_P_BOOT_SEQ == 0x21,  "0x21");
static_assert(LOG_P_DROP_OVERFLOW == 0x55, "0x55");
static_assert(LOG_P_REMAIN_MS == 0x58, "0x58");
static_assert(LOG_P_MAX == 0x59,       "param count = 0x59");

// 布局偏移
static_assert(LOG_OFF_SEQ == 4 && LOG_OFF_BOOT_SEQ == 8 && LOG_OFF_EVENT_ID == 12, "offsets 4/8/12");
static_assert(LOG_OFF_TIMESTAMP == 20 && LOG_OFF_PARAMS == 28 && LOG_OFF_BLOB == 76, "offsets 20/28/76");
static_assert(LOG_OFF_CRC32 == 108 && LOG_OFF_RESERVED == 112, "offsets 108/112");

// 结构尺寸关系
static_assert(sizeof(LogParamIn) <= 8u, "LogParamIn must be <= 8 B");
static_assert(LOG_OFF_PARAMS + LOG_MAX_PARAMS * LOG_PARAM_SIZE == LOG_OFF_BLOB, "params end -> blob");
static_assert(LOG_OFF_BLOB + LOG_BLOB_SIZE == LOG_OFF_CRC32, "blob end -> crc32");
static_assert(LOG_OFF_CRC32 + 4u == LOG_OFF_RESERVED, "crc32 -> reserved");
static_assert(LOG_OFF_RESERVED + 16u == LOG_RECORD_SIZE, "reserved end == 128");
