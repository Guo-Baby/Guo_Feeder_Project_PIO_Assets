#include <Arduino.h>
#include <time.h>
#include <sys/time.h>

#include "time_manager.h"

#include "config_manager.h"
#include "system_state.h"
#include "event_manager.h"

// =====================================================
// NTP配置
// =====================================================

static String ntp_server1;
static String ntp_server2;

// 时区偏移，单位：小时
static int timezone_offset_hours = 0;

// 夏令时
#define DAYLIGHT_OFFSET_SEC 0

// =====================================================
// 时间状态机
//
// 1 WAIT_WIFI
// 2 NEED_SYNC
// 3 SYNCING
// 4 READY
//
// =====================================================

enum TimeState
{
    TIME_WAIT_WIFI,
    TIME_NEED_SYNC,
    TIME_SYNCING,
    TIME_READY
};

static TimeState time_state = TIME_WAIT_WIFI;

// =====================================================
// NTP同步状态机（非阻塞）
// =====================================================

enum NtpState
{
    NTP_IDLE,        // 空闲
    NTP_REQUEST,     // 已发起请求，等待响应
    NTP_WAITING,     // 正在等待
    NTP_SUCCESS,     // 成功
    NTP_FAILED       // 失败
};

static NtpState ntp_state = NTP_IDLE;
static unsigned long ntp_wait_start = 0;

// =====================================================
// 时间变量
// =====================================================

// RTC时间缓存（占位实现，模拟RTC芯片存储）
static time_t rtc_time = 0;

// 最近一次NTP同步时间
static time_t last_ntp_sync_time = 0;

// =====================================================
// 时间有效规则
//
// 2026-07-01之后认为有效
// =====================================================

#define TIME_VALID_START_TIMESTAMP 1782864000
// 2026-07-01 00:00:00 UTC

// =====================================================
// NTP同步参数
// =====================================================

// 自动校时周期，单位：秒，从config读取，默认7天
static unsigned long ntp_sync_interval_sec = 7UL * 86400UL;

// NTP重试
#define NTP_MAX_RETRY 10
#define NTP_RETRY_INTERVAL_MS 5000

static uint8_t ntp_retry_count = 0;
static unsigned long last_ntp_retry_time = 0;

// NTP冷却，单位：秒
#define NTP_COOLDOWN_SEC 600

// 单次NTP等待超时，单位：毫秒
#define NTP_WAIT_TIMEOUT_MS 5000

// 下次同步目标时间（按固定时间点触发）
static time_t next_sync_target = 0;

// NTP限流，防止WiFi事件风暴，1小时最多60次
#define NTP_LIMIT_WINDOW_MS 3600000
#define NTP_LIMIT_COUNT 60

static unsigned long ntp_window_start = 0;
static uint8_t ntp_request_count = 0;

// =====================================================
// 内部工具：设置ESP32系统时钟
// =====================================================

static void time_set_system_clock(time_t timestamp)
{
    struct timeval tv;
    tv.tv_sec = timestamp;
    tv.tv_usec = 0;
    settimeofday(&tv, nullptr);
}

// =====================================================
// NTP请求限流
// =====================================================

static bool ntp_allow_request()
{
    unsigned long now = millis();

    if (ntp_window_start == 0) {
        ntp_window_start = now;
        ntp_request_count = 0;
    }

    if (now - ntp_window_start > NTP_LIMIT_WINDOW_MS) {
        ntp_window_start = now;
        ntp_request_count = 0;
    }

    if (ntp_request_count >= NTP_LIMIT_COUNT) {
        return false;
    }

    ntp_request_count++;
    return true;
}

// =====================================================
// 计算下次同步的目标时间点
// =====================================================

static time_t calculate_next_sync_time(time_t now)
{   
    if (!time_validate(now)) return 0;
    struct tm tm_info;
    localtime_r(&now, &tm_info);

    int interval_days = config_get_ntp_sync_interval_day();
    int sync_hour = config_get_ntp_sync_hour();
    int sync_minute = config_get_ntp_sync_minute();

    // 构造今天的目标时间点
    struct tm target = tm_info;
    target.tm_hour = sync_hour;
    target.tm_min = sync_minute;
    target.tm_sec = 0;

    time_t target_time = mktime(&target);

    // 如果目标时间已过或不足1分钟，往后推 interval_days 天
    if (target_time - now < 60) {
        target_time += interval_days * 86400;
    }

    return target_time;
}

// =====================================================
// RTC接口（占位实现）
// =====================================================

bool rtc_init()
{
    Serial.println("RTC init placeholder");
    return true;
}

bool rtc_read_time(time_t &timestamp)
{
    /*
    未来:
    timestamp = rtc_get_timestamp();
    */
    timestamp = rtc_time;
    return true;
}

bool rtc_write_time(time_t timestamp)
{
    /*
    未来:
    rtc_set_timestamp(timestamp);
    */
    rtc_time = timestamp;
    return true;
}

// =====================================================
// 时间合法性检查
// =====================================================

bool time_validate(time_t timestamp)
{
    if (timestamp < TIME_VALID_START_TIMESTAMP) {
        return false;
    }
    return true;
}

// =====================================================
// 初始化
// =====================================================

void time_init()
{
    Serial.println();
    Serial.println("Time manager init...");

    // =========================
    // 读取配置
    // =========================

    ntp_server1 = config_get_ntp_server1();
    ntp_server2 = config_get_ntp_server2();
    timezone_offset_hours = config_get_timezone();
    String tz ="GMT";

    //设置系统内置时区tz
    if(timezone_offset_hours >= 0)
    {
        tz += "-";
        tz += String(timezone_offset_hours);
    }
    else
    {
        tz += "+";
        tz += String(-timezone_offset_hours);
    }
    setenv(
        "TZ",
        tz.c_str(),
        1
    );
    tzset();

    ntp_sync_interval_sec =
        (unsigned long)config_get_ntp_sync_interval_day() * 86400UL;

    // =========================
    // 初始化RTC
    // =========================

    if (rtc_init()) {
        time_t rtc_now;

        if (rtc_read_time(rtc_now) && time_validate(rtc_now)) {
            time_set_system_clock(rtc_now);

            state_set_bool(STATE_TIME_VALID, true);

            Serial.println("RTC time valid");
        } else {
            state_set_bool(STATE_TIME_VALID, false);

            event_push(
                EVENT_TIME_INVALID,
                "",
                "time_manager",
                EVENT_PRIORITY_NORMAL,
                EVENT_POLICY_NORMAL,
                0
            );

            Serial.println("RTC time invalid");
        }
    } else {
        state_set_bool(STATE_TIME_VALID, false);

        event_push(
            EVENT_TIME_INVALID,
            "",
            "time_manager",
            EVENT_PRIORITY_NORMAL,
            EVENT_POLICY_NORMAL,
            0
        );
    }

    // =========================
    // 初始化NTP状态机
    // =========================

    ntp_state = NTP_IDLE;
    ntp_retry_count = 0;

    // =========================
    // 注册事件
    // =========================

    event_subscribe(
        EVENT_WIFI_CONNECTED,
        [](const EventMessage&msg) {
            // 距上次NTP同步不足一个周期（默认7天）→ 忽略
            time_t now_time =
                time_get();

            if(now_time == 0){
                return;
            }

            if (time_state == TIME_READY) {
                time_state = TIME_NEED_SYNC;
                ntp_retry_count = 0;
                ntp_state = NTP_IDLE;
            }
        }
    );

    time_state = TIME_WAIT_WIFI;

    // 初始化下次同步目标时间
    time_t now =
        time_get();

    if(now != 0){
        next_sync_target = calculate_next_sync_time(now);
    } else {
        next_sync_target = 0;
    }

    Serial.println("Time manager ready");
}

// =====================================================
// 获取系统时间
// =====================================================

time_t time_get() {
    if (!state_get_bool(STATE_TIME_VALID)) 
    { 
        return 0; 
    } 
    time_t now = time(nullptr); 
    if (!time_validate(now)) 
    { return 0; }
    return now; 
    }
// =====================================================
// 获取格式化时间，传入任意时间戳都可计算
// =====================================================

String time_get_string(time_t timestamp)
{
    if(timestamp == 0){return "No Time";}

    struct tm timeinfo;
    localtime_r(&timestamp, &timeinfo);
    char buffer[32];
    sprintf(
        buffer,
        "%04d-%02d-%02d %02d:%02d:%02d",
        timeinfo.tm_year + 1900,
        timeinfo.tm_mon + 1,
        timeinfo.tm_mday,
        timeinfo.tm_hour,
        timeinfo.tm_min,
        timeinfo.tm_sec
    );

    return String(buffer);
}

//对外查询接口，默认返回当前时间的字符串表示，若时间无效则返回"No Time"
String time_now_string()
{
    time_t now =
        time_get();

    if(now == 0)
    {
        return "No Time";
    }

    return time_get_string(now);
}
// =====================================================
// 获取最后一次NTP同步时间
// =====================================================

time_t time_get_last_ntp_sync()
{
    return last_ntp_sync_time;
}

// =====================================================
// NTP同步：发起请求（非阻塞）
// =====================================================

bool time_sync_ntp()
{
    // 如果已经在等待中，不允许重复发起
    if (ntp_state == NTP_WAITING || ntp_state == NTP_REQUEST) {
        return false;
    }

    // -------------------------
    // WiFi检查
    // -------------------------

    if (!state_get_bool(STATE_WIFI_STATUS)) {
        Serial.println("NTP skip: no wifi");
        return false;
    }

    // -------------------------
    // 冷却检查
    // -------------------------

    time_t now = time(nullptr);

    if (last_ntp_sync_time != 0 && now > 0 &&
        now - last_ntp_sync_time < NTP_COOLDOWN_SEC) {
        Serial.println("NTP cooldown");
        return false;
    }

    // -------------------------
    // 限流检查
    // -------------------------

    if (!ntp_allow_request()) {
        Serial.println("NTP rate limited");
        return false;
    }

    // -------------------------
    // 发起NTP请求
    // -------------------------

    Serial.println("NTP syncing...");

    configTime(
        timezone_offset_hours * 3600,
        DAYLIGHT_OFFSET_SEC,
        ntp_server1.c_str(),
        ntp_server2.c_str()
    );

    ntp_state = NTP_WAITING;
    ntp_wait_start = millis();

    return true;
}

// =====================================================
// 手动校时
// =====================================================

bool time_set_manual(time_t timestamp)
{
    if (!time_validate(timestamp)) {
        return false;
    }

    if (!rtc_write_time(timestamp)) {
        return false;
    }

    time_set_system_clock(timestamp);

    state_set_bool(STATE_TIME_VALID, true);

    return true;
}

// =====================================================
// 手动校时（字符串）
// =====================================================

bool time_set_manual_string(const String &time_string)
{
    struct tm tm_data;
    memset(&tm_data, 0, sizeof(tm_data));

    int result = sscanf(
        time_string.c_str(),
        "%d-%d-%d %d:%d:%d",
        &tm_data.tm_year,
        &tm_data.tm_mon,
        &tm_data.tm_mday,
        &tm_data.tm_hour,
        &tm_data.tm_min,
        &tm_data.tm_sec
    );

    if (result != 6) {
        return false;
    }

    tm_data.tm_year -= 1900;
    tm_data.tm_mon -= 1;

    time_t manual_time = mktime(&tm_data);

    return time_set_manual(manual_time);
}

// =====================================================
// 时间任务（非阻塞状态机）
// =====================================================

void time_task()
{
    static unsigned long last_check = 0;

    unsigned long now = millis();

    // 每秒检查一次
    if (now - last_check < 1000) {
        return;
    }
    last_check = now;

    switch (time_state) {

        // =============================================
        // 等待WiFi
        // =============================================

        case TIME_WAIT_WIFI: {
            if (state_get_bool(STATE_WIFI_STATUS)) {
                time_state = TIME_NEED_SYNC;
            }
            break;
        }

        // =============================================
        // 判断是否需要同步
        // =============================================

        case TIME_NEED_SYNC: {
            time_t now_time = time(nullptr);

            // 第一次同步
            if (last_ntp_sync_time == 0) {
                time_state = TIME_SYNCING;
                ntp_retry_count = 0;
                ntp_state = NTP_IDLE;
                break;
            }

            // 周期同步（从config读取间隔）
            if (now_time - last_ntp_sync_time > (time_t)ntp_sync_interval_sec) {
                time_state = TIME_SYNCING;
                ntp_retry_count = 0;
                ntp_state = NTP_IDLE;
                break;
            }

            time_state = TIME_READY;
            break;
        }

        // =============================================
        // NTP同步（非阻塞状态机）
        // =============================================

        case TIME_SYNCING: {
            // ---- 重试间隔保护 ----
            if (ntp_retry_count > 0 &&
                now - last_ntp_retry_time < NTP_RETRY_INTERVAL_MS) {
                break;
            }

            // ---- 状态机 ----
            switch (ntp_state) {

                case NTP_IDLE: {
                    // 发起NTP请求
                    if (!time_sync_ntp()) {
                        // 发起失败（WiFi断、限流等）
                        ntp_retry_count++;
                        if (ntp_retry_count >= NTP_MAX_RETRY) {
                            Serial.println("NTP retry limit");
                            ntp_retry_count = 0;
                            time_state = TIME_READY;
                        } else {
                            last_ntp_retry_time = now;
                        }
                    }
                    // 如果发起成功，ntp_state 已被 time_sync_ntp() 改为 NTP_WAITING
                    break;
                }

                case NTP_WAITING: {
                    if (now - ntp_wait_start > NTP_WAIT_TIMEOUT_MS) {
                        ntp_state = NTP_FAILED;
                        break;
                    }
                    time_t candidate_ts = time(nullptr);
                    if (time_validate(candidate_ts)) {
                        ntp_state = NTP_SUCCESS;
                    }
                    break;
                }

                case NTP_SUCCESS: {
                    time_t ntp_time = time(nullptr);

                    if (!time_validate(ntp_time)) {
                        Serial.println("NTP invalid time");
                        ntp_state = NTP_IDLE;
                        time_state = TIME_READY;
                        break;
                    }

                    // ---- 写RTC ----
                    if (rtc_write_time(ntp_time)) {
                        Serial.println("RTC updated");
                    } else {
                        Serial.println("RTC update failed");
                    }

                    // ---- 更新系统时钟 ----
                    time_set_system_clock(ntp_time);

                    bool old_valid = state_get_bool(STATE_TIME_VALID);

                    state_set_bool(STATE_TIME_VALID, true);

                    // 只有 false → true 才推送 EVENT_TIME_VALID
                    if (!old_valid) {
                        event_push(
                            EVENT_TIME_VALID,
                            "",
                            "time_manager",
                            EVENT_PRIORITY_NORMAL,
                            EVENT_POLICY_NORMAL,
                            0
                        );
                    }
                    // ===== 同步成功后，在这里更新 next_sync_target =====
                    time_t now = time(nullptr);
                    next_sync_target = calculate_next_sync_time(now);
                    last_ntp_sync_time = ntp_time;

                    Serial.println("NTP sync OK");
                    String timestr = time_get_string(ntp_time);
                    Serial.printf("当前时间：%s\n", timestr.c_str());

                    event_push(
                        EVENT_NTP_SYNC_OK,
                        "",
                        "time_manager",
                        EVENT_PRIORITY_NORMAL,
                        EVENT_POLICY_NORMAL,
                        0
                    );

                    ntp_state = NTP_IDLE;
                    ntp_retry_count = 0;
                    time_state = TIME_READY;
                    break;
                }

                case NTP_FAILED: {
                    Serial.println("NTP sync failed");

                    ntp_retry_count++;

                    if (ntp_retry_count >= NTP_MAX_RETRY) {
                        Serial.println("NTP retry limit");
                        ntp_retry_count = 0;
                        ntp_state = NTP_IDLE;
                        time_state = TIME_READY;
                    } else {
                        last_ntp_retry_time = now;
                        ntp_state = NTP_IDLE;  // 下次循环重试
                    }
                    break;
                }

                default:
                    break;
            }
            break;
        }

        // =============================================
        // 正常运行
        // =============================================

        case TIME_READY: {
            // WiFi断开
            if (!state_get_bool(STATE_WIFI_STATUS)) {
                time_state = TIME_WAIT_WIFI;
                break;
            }

            // 时间无效时不检查同步
            if (!state_get_bool(STATE_TIME_VALID)) {
                break;
            }

            time_t now = time(nullptr);
            if (!time_validate(now)) {
                break;
            }

            // 如果 next_sync_target 未初始化，计算一次
            if (next_sync_target == 0) {
                next_sync_target = calculate_next_sync_time(now);
            }

            // 到达目标时间点 → 触发同步
            if (now >= next_sync_target) {
                time_state = TIME_NEED_SYNC;
                ntp_retry_count = 0;
                ntp_state = NTP_IDLE;
                next_sync_target = calculate_next_sync_time(now);
            }

            break;
        }

        default:
            break;
    }
}

// =====================================================
// Time Manager状态（调试用）
// =====================================================

int time_get_state()
{
    return (int)time_state;
}

// =====================================================
// 预留：set_time 系统命令入口
// =====================================================
void time_manager_set_time()
{
    // 预留：未来由 time_manager 实现
}
