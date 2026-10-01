#pragma once

#include <Arduino.h>
#include <time.h>

// =====================================================
// 时间管理模块（TimeManager V2）
//
// 功能：
//
// 1. 管理 ESP32 系统时间（System Time）
//
// 2. WiFi 连接后通过 ESP-IDF SNTP 获取标准时间（官方机制）
//
// 3. SNTP 成功后以 System Time 为准，必要时校准 PCF8563T RTC
//
// 4. RTC 作为本地持久化时间源，掉电后继续计时
//
// 5. 启动时 RTC → System Time 快速恢复（硬同步）
//
// 6. 提供统一时间访问接口
//
// V2 数据流：
//
//   Boot: RTC ──(有效则硬同步)──> System Time
//   WiFi: SNTP ──(平滑同步)──> System Time ──(差值>阈值)──> RTC
//
// 时间来源可信度：SNTP > RTC
// 内部统一使用 UTC/Unix 时间戳；TZ/DST 仅用于 UTC→Local 显示。
//
// RTC 与 OLED 共用同一组 I2C（SDA/SCL 由 oled_init() 初始化，
// 本模块不得再次调用 Wire.begin()）。
// =====================================================

// =====================================================
// 初始化
//
// 上电流程（须在 oled_init() 之后调用，保证 Wire 已就绪）:
//
// 1. 设置 TZ 时区环境变量
//
// 2. 配置 SNTP（服务器 / 同步周期 / 回调），暂不启动
//
// 3. 初始化 PCF8563T（复用 OLED I2C Bus）
//
// 4. 读取 RTC 时间并校验：
//      合法 → settimeofday 硬同步 System Time，time_valid=true
//      非法 → time_valid=false（不阻塞启动，等 SNTP）
//
// =====================================================

void time_init();

// =====================================================
// 时间任务
//
// loop 中周期调用，非阻塞。
//
// 负责:
// - WiFi 就绪后启动/重启 SNTP
// - 处理 SNTP 同步完成事件（记录 / 推事件 / 触发 RTC 校准）
// - time_valid 维护与状态迁移
// - RTC 校准（Critical Operation 保护）
//
// =====================================================

void time_task();

// =====================================================
// 获取系统时间
//
// 返回 Unix timestamp（UTC）。
// 时间无效（未达 2026-07-01）时返回 0。
// =====================================================

time_t time_get();

// =====================================================
// 获取格式化时间（本地时区显示）
//
// 时间无效/传 0 返回 "No Time"
// =====================================================

String time_get_string(time_t timestamp);

// 对外查询接口，返回当前本地时间字符串；无效则 "No Time"
String time_now_string();

// =====================================================
// 手动校时（Unix 时间戳，UTC）
//
// 流程:
// 1. 校验时间合法性
// 2. 写 RTC（Critical Operation：acquire → write → release）
// 3. settimeofday 更新 System Time
// 4. time_valid = true
//
// RTC Write 失败不撤销 System Time 更新（RTC 可下次再校准）。
// 返回 false 仅表示: 时间非法 / RTC 禁用但写入被拒 / 重启挂起中。
// =====================================================

bool time_set_manual(time_t timestamp);

// 手动校时（字符串，本地时区墙钟时间 "YYYY-MM-DD HH:MM:SS"）
bool time_set_manual_string(const String &time_string);

// =====================================================
// 主动触发一次 SNTP 立即校时
//
// 返回 true 表示已发起（需要 WiFi 在线且 SNTP 已配置）。
// 非阻塞；结果通过同步回调后续异步呈现。
// =====================================================

bool time_sync_ntp();

// =====================================================
// PCF8563T RTC 接口
//
// 内部实现使用复用 OLED 的 Wire 总线（I2C 7bit 地址 0x51）。
// 每次读写都是完整的 start…stop I2C 事务，不长期占用总线。
// =====================================================

// RTC 初始化（探测芯片是否存在，读取配置）
// 返回 true = 芯片可通信
bool rtc_init();

// 从 RTC 读取时间（UTC Unix 时间戳）
// 返回 false = 通信失败 / VL(电压低)置位 / BCD 解码非法
bool rtc_read_time(time_t &timestamp);

// 写入 RTC 时间（UTC Unix 时间戳）
// 内部不包含 Critical Operation；调用方需要自行配对
// （校准/手动校时路径已在 time_manager 内部处理）。
bool rtc_write_time(time_t timestamp);

// =====================================================
// 时间合法性检查
//
// System Time >= 2026-07-01 (UTC) 认为有效
// =====================================================

bool time_validate(time_t timestamp);

// =====================================================
// 最近一次 SNTP 成功同步的时间（Unix，UTC）
// 仅运行时保存，不持久化
// =====================================================

time_t time_get_last_ntp_sync();

// =====================================================
// Time Manager 状态（调试用）
//
// 0 INVALID/Boot
// 1 WAIT_WIFI
// 2 SYNCING
// 3 READY
// =====================================================

int time_get_state();

// =====================================================
// 时间查询结果（system.get_time 命令专用）
//
// 所有转换逻辑集中在 TimeManager 内完成，
// 外部模块不直接访问 RTC / SNTP / settimeofday。
// =====================================================

struct TimeQueryResult
{
    bool        system_valid;    // System Time 是否有效
    time_t      system_unix;     // System Time (UTC)，无效为 0
    String      system_local;    // 本地时区显示，无效为 "No Time"

    bool        rtc_present;     // RTC 芯片是否可通信
    bool        rtc_valid;       // RTC 时间是否有效（含 VL/解码/阈值校验）
    time_t      rtc_unix;        // RTC 时间 (UTC)，不可用为 0
    String      rtc_local;       // 本地时区显示，不可用为 "No Time"

    const char *source;          // INVALID / RTC / SNTP（当前主要时间来源）
    time_t      last_ntp_sync;   // 最近一次 SNTP 成功时刻，0 = 从未
    bool        ntp_started;     // SNTP 是否已启动
    int         timezone_offset_h; // 时区偏移（小时），正数 = UTC+
};

// 非阻塞填充查询结果；RTC 读取为短 I2C 事务
bool time_query(TimeQueryResult &out);

// RTC 芯片是否可通信（诊断用）
bool time_rtc_present();

// =====================================================
// 预留：set_time 系统命令入口
// =====================================================
void time_manager_set_time();
