#include <Arduino.h>
#include <time.h>
#include <sys/time.h>
#include <Wire.h>

#include "time_manager.h"

#include "esp_sntp.h"
#include "config_manager.h"
#include "system_state.h"
#include "event_manager.h"
#include "system_command.h"

// =====================================================
// TimeManager V2
//
// 架构（相对旧版的变化）：
//   1. 删除自实现 NTP（UDP 轮询 / configTime / settimeofday 硬校时），
//      改用 ESP-IDF SNTP（esp_sntp_*，IDF v4.4 线程安全别名）。
//   2. RTC 由内存占位实现替换为真实 PCF8563T 驱动，复用 OLED I2C Bus
//      （不重复调用 Wire.begin()；SDA/SCL 由 oled_init() 建立）。
//   3. RTC Write 走 Critical Operation（acquire/release 配对）。
//   4. RTC Read 非 Critical Operation。
//   5. 内部统一 UTC/Unix 时间戳；TZ 仅用于显示。
//
// 时间来源可信度：SNTP > RTC
// =====================================================

// =====================================================
// 时间有效规则
//
// System Time >= 2026-07-01 00:00:00 UTC 认为有效
// =====================================================
#define TIME_VALID_START_TIMESTAMP 1782864000

// SNTP 周期下限（RFC 4330 强制 >= 15 秒；本项目固定 24h）
#define SNTP_MIN_INTERVAL_MS 15000UL

// Smooth Sync（adjtime）稳定窗口：
//   SNTP callback 触发后，再观察该时长确认 System Time 已收敛，才允许写 RTC。
//   典型 24h 漂移远小于 1 秒，3 秒窗口足以让 adjtime 收敛。
#define SNTP_SMOOTH_SETTLE_MS 3000UL

// Smooth Sync 等待上限：
//   SNTP_SYNC_STATUS_IN_PROGRESS 持续超过该时长则放弃等待并进入后续流程，
//   避免 adjtime 迟迟不收敛时 RTC 永远得不到校准。
#define SNTP_SMOOTH_TIMEOUT_MS 30000UL

// =====================================================
// 配置缓存（config 只读一次）
// =====================================================
static String ntp_server1;
static String ntp_server2;
static int timezone_offset_hours = 8;
static uint32_t ntp_sync_interval_ms = 86400000UL;   // 24h
static int rtc_calibrate_threshold_sec = 2;

// =====================================================
// 时间状态
// =====================================================

// 最近一次 SNTP 同步成功时刻（Unix/UTC；0 = 从未）
static volatile time_t last_ntp_sync_time = 0;
static bool synced_once = false;

// 内部时间来源（仅日志/调试用，不对外提供可信度接口）
static const char *time_source = "INVALID";

// 调试状态（0 boot / 1 wait_wifi / 2 syncing / 3 ready）
static uint8_t dbg_state = 0;

// =====================================================
// SNTP 管理
//
// sntp_sync_seq：SNTP 每完成一次同步自增（回调运行在 lwip 上下文，
// 只能写易失标记，不得执行 I2C / 长耗时操作）。
// time_task（loop 任务）轮询该序列号，决定何时做 RTC 校准。
//
// sntp_sync_notify_ms：记录 callback 触发时刻，供 loop 做“延迟确认”，
// 即确认 Smooth Sync（adjtime）已收敛后才允许写 RTC。
// =====================================================
static bool sntp_configured = false;
static bool sntp_started = false;
static bool prev_wifi = false;
static volatile uint32_t sntp_sync_seq = 0;
static volatile time_t sntp_sync_tv_sec = 0;
static volatile uint32_t sntp_sync_notify_ms = 0;

// =====================================================
// RTC 状态（PCF8563T）
// =====================================================
static int8_t rtc_addr = -1;       // I2C 7bit 地址（0x51 = 81）
static bool rtc_present = false;   // 芯片探测结果
static bool rtc_enabled = false;   // config rtc.enable

// 校准去重：每完成一次 SNTP 同步，最多校准一次 RTC
static uint32_t rtc_last_calibrate_seq = 0;

// =====================================================
// 内部工具：BCD 转换（PCF8563T 时间寄存器为 BCD 码）
// =====================================================

static uint8_t rtc_bin2bcd(uint8_t value)
{
    return (uint8_t)(((value / 10) << 4) | (value % 10));
}

static uint8_t rtc_bcd2bin(uint8_t value)
{
    return (uint8_t)(((value >> 4) & 0x0F) * 10 + (value & 0x0F));
}

// =====================================================
// 内部工具：UTC 日历 → Unix 时间戳
//
// 不用 mktime()：mktime 依赖本地时区，会把"UTC 日历"错误地按 TZ 解释。
// 使用 days-from-civil 算法，与时区无关。
// =====================================================

static time_t rtc_make_utc_timestamp(
    int year, int mon, int day,
    int hour, int min, int sec)
{
    year -= (mon <= 2);
    const int era = (year >= 0 ? year : year - 399) / 400;
    const unsigned yoe = (unsigned)(year - era * 400);        // [0,399]
    const unsigned doy = (153U * (unsigned)(mon + (mon > 2 ? -3 : 9)) + 2U) / 5U
                         + (unsigned)day - 1U;                 // [0,365]
    const unsigned doe = yoe * 365U + yoe / 4U - yoe / 100U + doy; // [0,146096]
    int64_t days = (int64_t)era * 146097LL + (int64_t)doe - 719468LL;
    return (time_t)(days * 86400LL + hour * 3600LL + min * 60LL + sec);
}

// =====================================================
// 内部工具：设置 ESP32 系统时钟（硬同步，仅用于 RTC→System 启动恢复
// 与手动校时。SNTP 的校时由 ESP-IDF 内部完成，不经过这里。）
// =====================================================

static void time_set_system_clock(time_t timestamp)
{
    struct timeval tv;
    tv.tv_sec = timestamp;
    tv.tv_usec = 0;
    settimeofday(&tv, nullptr);
}

// =====================================================
// 内部工具：时区环境变量设置
//
// POSIX TZ 符号与习惯相反：东八区写成 "GMT-8"。
// =====================================================

static void time_apply_timezone()
{
    String tz = "GMT";
    if (timezone_offset_hours >= 0) {
        tz += "-";
        tz += String(timezone_offset_hours);
    } else {
        tz += "+";
        tz += String(-timezone_offset_hours);
    }
    setenv("TZ", tz.c_str(), 1);
    tzset();
}

// =====================================================
// SNTP 同步完成回调（lwip 上下文，只置标记）
// =====================================================

static void time_on_sntp_sync(struct timeval *tv)
{
    // 本 SDK 下该 callback 可能被调用多次：
    //   1) 时间更新完成后调用，此时 tv != NULL，携带 SNTP 服务器给出的时间；
    //   2) 同步状态被置为 COMPLETED 时调用，此时 tv == NULL。
    // 因此仅在 tv != NULL 时更新 sntp_sync_tv_sec，避免用 NULL 覆盖真实值。
    //
    // sntp_sync_tv_sec 语义：最近一次 SNTP 获得的 Unix 时间（服务器侧时间），
    // 并非“本地完成同步的时刻”。对外由 time_get_last_ntp_sync() 暴露。
    if (tv != nullptr) {
        sntp_sync_tv_sec = tv->tv_sec;
    }
    sntp_sync_notify_ms = millis();
    sntp_sync_seq++;           // 通知 loop 侧“发生了一次同步”

    // 严禁在此执行：rtc_read_time() / rtc_write_time() / Wire.* / Serial / 长耗时操作。
}

// =====================================================
// 内部工具：启动 / 重启 SNTP
//
// 配置（servers / mode / interval / cb）在 time_init() 完成；
// 这里只负责 init / restart，保证 WiFi 就绪后才发起请求。
// =====================================================

static void sntp_do_start()
{
    if (!sntp_configured) {
        Serial.println("[Time] SNTP not configured, skip start");
        return;
    }
    esp_sntp_init();
    sntp_started = true;
    synced_once = false;      // 重新开始一轮同步
    Serial.println("[Time] SNTP started (waiting first sync)");
}

static void sntp_do_restart()
{
    if (!sntp_started) {
        sntp_do_start();
        return;
    }
    esp_sntp_restart();
    synced_once = false;
    Serial.println("[Time] SNTP restarted");
}

// =====================================================
// PCF8563T RTC 驱动
//
// 与 OLED 共用同一组 SDA/SCL（Wire 已由 oled_init() begin）。
// 本模块绝不调用 Wire.begin() / Wire.setClock()。
//
// 时序安全：所有事务均为完整 start…stop，
// 不依赖跨 Wire 调用的"总线占用"，避免与 OLED 渲染（同任务）
// 或云端命令（MQTT 任务）交叉访问 I2C 时破坏帧。
// =====================================================

bool rtc_init()
{
    rtc_enabled = config_get_rtc_enable();
    if (!rtc_enabled) {
        Serial.println("[Time] RTC disabled by config, skip");
        rtc_addr = -1;
        rtc_present = false;
        return false;
    }

    int addr = config_get_rtc_i2c_addr();
    if (addr <= 0 || addr > 127) {
        addr = 0x51;
    }

    // 探测：向 Control/Status1 寄存器写指针（不写数据）
    Wire.beginTransmission((uint8_t)addr);
    Wire.write(0x00);
    uint8_t err = Wire.endTransmission(true);
    if (err != 0) {
        Serial.printf("[Time] RTC probe failed (addr=0x%02X err=%u), "
                      "chip not present?\n", addr, (unsigned)err);
        rtc_addr = -1;
        rtc_present = false;
        return false;
    }

    int cfg_sda = config_get_rtc_sda();
    int cfg_scl = config_get_rtc_scl();
    int oled_sda = config_get_oled_sda();
    int oled_scl = config_get_oled_scl();
    if (cfg_sda != oled_sda || cfg_scl != oled_scl) {
        Serial.printf("[Time] WARN rtc sda/scl (%d/%d) != oled (%d/%d); "
                      "RTC reuses OLED bus pins\n",
                      cfg_sda, cfg_scl, oled_sda, oled_scl);
    }

    rtc_addr = (int8_t)addr;
    rtc_present = true;
    Serial.printf("[Time] PCF8563T detected at 0x%02X (shared OLED I2C)\n",
                  addr);
    return true;
}

bool rtc_read_time(time_t &timestamp)
{
    if (!rtc_present || rtc_addr < 0) {
        return false;
    }

    // 1) 把内部指针写到 0x02（seconds），完整事务 + STOP
    Wire.beginTransmission((uint8_t)rtc_addr);
    Wire.write(0x02);
    if (Wire.endTransmission(true) != 0) {
        return false;
    }

    // 2) 独立读事务：从 0x02 起连续读 7 字节
    //    （seconds..minutes..hours..days..weekdays..months..years）
    uint8_t n = Wire.requestFrom((uint8_t)rtc_addr, (uint8_t)7, (uint8_t)1);
    if (n < 7) {
        return false;
    }
    uint8_t raw[7];
    for (uint8_t i = 0; i < 7; i++) {
        raw[i] = (uint8_t)Wire.read();
    }

    // VL（Voltage Low）位：seconds 寄存器 bit7，置位表示断电/振荡器停摆，
    // 内部计时数据不可信。
    if (raw[0] & 0x80) {
        Serial.println("[Time] RTC VL flag set (battery/low power), "
                       "time unreliable");
        return false;
    }

    // 连续读 7 字节的寄存器顺序（PCF8563 memory map 0x02..0x08）：
    //   raw[0]=seconds(0x02) raw[1]=minutes(0x03) raw[2]=hours(0x04)
    //   raw[3]=days(0x05)    raw[4]=weekdays(0x06)  raw[5]=months(0x07)
    //   raw[6]=years(0x08)
    uint8_t sec   = rtc_bcd2bin(raw[0] & 0x7F);
    uint8_t min   = rtc_bcd2bin(raw[1] & 0x7F);
    uint8_t hour  = rtc_bcd2bin(raw[2] & 0x3F);
    uint8_t day   = rtc_bcd2bin(raw[3] & 0x3F);
    uint8_t month = rtc_bcd2bin(raw[5] & 0x1F);   // bit7 = century，忽略
    uint8_t year  = rtc_bcd2bin(raw[6]);          // 0-99 → 2000-2099

    // BCD 解码合法性兜底
    if (month < 1 || month > 12 ||
        day < 1 || day > 31 ||
        hour > 23 || min > 59 || sec > 59) {
        Serial.println("[Time] RTC BCD decode out of range");
        return false;
    }

    timestamp = rtc_make_utc_timestamp(2000 + year, month, day,
                                       hour, min, sec);
    return true;
}

bool rtc_write_time(time_t timestamp)
{
    if (!rtc_present || rtc_addr < 0) {
        return false;
    }

    struct tm t;
    gmtime_r(&timestamp, &t);          // UTC 分解，与 RTC 内部基准一致
    if (t.tm_year < 100 || t.tm_year > 199) {
        // PCF8563T 年份寄存器仅 0-99（2000-2099）
        Serial.println("[Time] RTC write rejected: year out of 2000-2099");
        return false;
    }

    uint8_t sec = rtc_bin2bcd((uint8_t)t.tm_sec);
    uint8_t min = rtc_bin2bcd((uint8_t)t.tm_min);
    uint8_t hour = rtc_bin2bcd((uint8_t)t.tm_hour);
    uint8_t day = rtc_bin2bcd((uint8_t)t.tm_mday);
    uint8_t wday = rtc_bin2bcd((uint8_t)(t.tm_wday & 0x07));
    uint8_t mon = rtc_bin2bcd((uint8_t)t.tm_mon + 1);      // century bit = 0
    uint8_t year = rtc_bin2bcd((uint8_t)(t.tm_year - 100));

    // 从 0x00 起一次写完：Ctrl1=0x00(时钟运行) + Ctrl2=0x00 +
    // seconds..years；写入秒寄存器同时清除 VL。
    Wire.beginTransmission((uint8_t)rtc_addr);
    Wire.write(0x00);
    Wire.write(0x00);   // Control/Status1: TEST1=0, STOP=0, 内部振荡器运行
    Wire.write(0x00);   // Control/Status2: 中断/闹钟/Timer 输出全关
    Wire.write(sec);
    Wire.write(min);
    Wire.write(hour);
    Wire.write(day);
    Wire.write(wday);
    Wire.write(mon);
    Wire.write(year);
    if (Wire.endTransmission(true) != 0) {
        Serial.println("[Time] RTC write NACK");
        return false;
    }
    return true;
}

bool time_rtc_present()
{
    return rtc_present;
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
// 内部：推送时间有效性状态迁移事件
// =====================================================

static void time_update_valid_event(bool now_valid)
{
    static bool last_reported = false;   // 冷启动初值 false
    if (now_valid == last_reported) {
        return;
    }
    last_reported = now_valid;

    if (now_valid) {
        event_push(EVENT_TIME_VALID, "", "time_manager",
                   EVENT_PRIORITY_NORMAL, EVENT_POLICY_NORMAL, 0);
        Serial.println("[Time] Event: TIME_VALID");
    } else {
        event_push(EVENT_TIME_INVALID, "", "time_manager",
                   EVENT_PRIORITY_NORMAL, EVENT_POLICY_NORMAL, 0);
        Serial.println("[Time] Event: TIME_INVALID");
    }
}

// =====================================================
// 内部：RTC 校准（每次 SNTP 同步完成后调用一次）
//
// 规则：
//   System Time 有效后：
//     RTC 读成功且 |System-RTC| <= 阈值  → 不写
//     RTC 读成功且 |System-RTC| >  阈值  → System Time 写入 RTC
//     RTC 读失败（VL/通信）             → 尝试写入（芯片需要初值）
//
// RTC Write = Critical Operation：
//   acquire 成功 → write（无论成败）→ release 配对
//   acquire 失败（重启挂起）→ 跳过本次，不 release、不写
// =====================================================

static void time_rtc_calibrate()
{
    if (!rtc_present) {
        return;
    }

    // 校准写入值必须取 System Time（time(nullptr)），不得使用 sntp_sync_tv_sec。
    //
    // 原因：
    //   - SNTP callback 的参数表示“服务器给出的时间”，是同步时刻的瞬时值；
    //   - Smooth Sync 模式下 System Time 会由 adjtime 逐步逼近该值，
    //     callback 触发瞬间 System Time 可能尚未收敛；
    //   - RTC 最终应当跟随已经稳定之后的 System Time。
    time_t sys = time(nullptr);
    if (!time_validate(sys)) {
        return;   // System Time 还没进入有效区间，等下次
    }

    time_t rtc = 0;
    bool rtc_ok = rtc_read_time(rtc);

    if (rtc_ok) {
        long long diff = (long long)sys - (long long)rtc;
        if (diff < 0) diff = -diff;
        if (diff <= (long long)rtc_calibrate_threshold_sec) {
            Serial.printf("[Time] RTC drift %llds <= %ds, skip write\n",
                          diff, rtc_calibrate_threshold_sec);
            return;
        }
    } else {
        Serial.println("[Time] RTC read failed, try to write current "
                       "System Time");
    }

    // ---- Critical Operation：只覆盖 RTC 写入 ----
    if (!system_command_critical_operation_acquire()) {
        Serial.println("[Time] RTC calibrate skip: restart pending");
        return;   // acquire 被拒，不得 release
    }

    bool ok = rtc_write_time(sys);
    if (ok) {
        Serial.println("[Time] RTC calibrated to System Time");
    } else {
        Serial.println("[Time] RTC write failed (keep System Time, "
                       "retry next SNTP)");
    }
    system_command_critical_operation_release();
}

// =====================================================
// 初始化（须在 oled_init() 之后调用）
// =====================================================

void time_init()
{
    Serial.println();
    Serial.println("Time manager init...");

    // -------------------------
    // 读取配置
    // -------------------------
    timezone_offset_hours = config_get_timezone();
    ntp_server1 = config_get_ntp_server1();
    ntp_server2 = config_get_ntp_server2();
    rtc_calibrate_threshold_sec = config_get_rtc_calibrate_threshold_sec();
    if (rtc_calibrate_threshold_sec < 0) {
        rtc_calibrate_threshold_sec = 2;
    }

    long cfg_interval = config_get_ntp_sync_interval_sec();
    if (cfg_interval <= 0) {
        cfg_interval = 86400;   // 默认 24h
    }
    ntp_sync_interval_ms = (uint32_t)cfg_interval * 1000UL;
    if (ntp_sync_interval_ms < SNTP_MIN_INTERVAL_MS) {
        ntp_sync_interval_ms = SNTP_MIN_INTERVAL_MS;
    }

    // -------------------------
    // 设置时区（仅影响 UTC→Local 显示）
    // -------------------------
    time_apply_timezone();
    Serial.printf("[Time] timezone offset = GMT%+d\n",
                  timezone_offset_hours);

    // -------------------------
    // 初始化 RTC 并从 RTC 恢复系统时间
    // -------------------------
    rtc_enabled = config_get_rtc_enable();
    if (rtc_enabled && rtc_init()) {
        time_t rtc_now = 0;
        if (rtc_read_time(rtc_now) && time_validate(rtc_now)) {
            // 硬同步：RTC → System Time（不等待 WiFi / SNTP）
            time_set_system_clock(rtc_now);
            state_set_bool(STATE_TIME_VALID, true);
            time_source = "RTC";
            Serial.printf("[Time] RTC boot restore OK: %s\n",
                          time_get_string(rtc_now).c_str());
        } else {
            state_set_bool(STATE_TIME_VALID, false);
            time_source = "INVALID";
            Serial.println("[Time] RTC present but time invalid "
                           "(wait SNTP)");
        }
    } else {
        state_set_bool(STATE_TIME_VALID, false);
        time_source = "INVALID";
        if (rtc_enabled) {
            Serial.println("[Time] RTC init failed (wait SNTP)");
        }
    }

    // RTC 无效本身不是错误：不阻塞启动。事件通知一次。
    time_update_valid_event(state_get_bool(STATE_TIME_VALID));

    // -------------------------
    // 配置 ESP-IDF SNTP（此时不启动，等 WiFi）
    // -------------------------
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    if (ntp_server1.length() > 0) {
        esp_sntp_setservername(0, ntp_server1.c_str());
    }
    if (ntp_server2.length() > 0) {
        esp_sntp_setservername(1, ntp_server2.c_str());
    }
    // 有效 System Time 存在时采用平滑同步（差值 >35min 自动立即同步）
    esp_sntp_set_sync_mode(SNTP_SYNC_MODE_SMOOTH);
    esp_sntp_set_sync_interval(ntp_sync_interval_ms);
    esp_sntp_set_time_sync_notification_cb(time_on_sntp_sync);
    sntp_configured = true;

    Serial.printf("[Time] SNTP servers=%s/%s interval=%lus\n",
                  ntp_server1.c_str(), ntp_server2.c_str(),
                  (unsigned long)(ntp_sync_interval_ms / 1000UL));

    dbg_state = state_get_bool(STATE_TIME_VALID) ? 3 : 1;
    Serial.println("Time manager init done");
}

// =====================================================
// 时间任务（非阻塞，loop 周期调用）
// =====================================================

void time_task()
{
    static unsigned long last_check_ms = 0;
    unsigned long now_ms = millis();
    if (now_ms - last_check_ms < 500) {
        return;
    }
    last_check_ms = now_ms;

    bool wifi = state_get_bool(STATE_WIFI_STATUS);

    // -------------------------
    // 1) SNTP 生命周期管理
    // -------------------------
    if (wifi && !sntp_started) {
        sntp_do_start();
    } else if (wifi && sntp_started && !prev_wifi) {
        // WiFi 重连：尚未同步过一次则立即重启 SNTP 拉取；
        // 已同步过则交给 lwip 按 24h 周期自行校时。
        if (!synced_once) {
            sntp_do_restart();
        }
    }
    prev_wifi = wifi;

    // -------------------------
    // 2) 处理 SNTP 同步完成
    //    （回调只自增 seq，这里在 loop 上下文消费）
    //
    // 本 SDK 状态机特性（见 SDK 头文件 esp_sntp.h 的说明）：
    //   - 时间同步完成后状态为 SNTP_SYNC_STATUS_COMPLETED，
    //     但 COMPLETED 是**瞬时状态**，随后会被自动重置为
    //     SNTP_SYNC_STATUS_RESET 等待下一个同步周期；
    //   - 尚未完成同步时状态同样是 SNTP_SYNC_STATUS_RESET。
    //
    // 因此：
    //   禁止用 "status != IN_PROGRESS" 判定成功
    //   ——那等价于把 RESET（含"从未同步"与"已同步后回落"）误判为成功。
    //   同时 loop 每 500ms 才采样一次，几乎不可能稳定捕捉到瞬时的 COMPLETED。
    //
    // 采用「callback 通知 + loop 延迟确认」方案：
    //   a) seq 变化 = callback 已通知，确实发生过一次同步（可靠证据）；
    //   b) 排除 IN_PROGRESS，避免 Smooth Sync（adjtime）未收敛就写 RTC；
    //   c) 再经过一个稳定窗口（或超时兜底）后，才执行后续动作与 RTC 校准。
    // -------------------------
    if (sntp_sync_seq != 0 && sntp_sync_seq != rtc_last_calibrate_seq) {
        sntp_sync_status_t st = esp_sntp_get_sync_status();
        unsigned long waited_ms = (unsigned long)(millis() - sntp_sync_notify_ms);

        bool settled = false;
        if (st == SNTP_SYNC_STATUS_IN_PROGRESS) {
            // adjtime 仍在工作，System Time 还在逐步调整：继续等待。
            // 超时则放弃等待，避免 RTC 永不校准。
            settled = (waited_ms >= SNTP_SMOOTH_TIMEOUT_MS);
        } else if (st == SNTP_SYNC_STATUS_COMPLETED) {
            // 瞬时窗口内被采到：明确完成。
            settled = true;
        } else {
            // RESET：能走到此处的前提是 callback 已经通知过（seq 已变化），
            // 说明本轮同步已结束并回到等待态；再等一个稳定窗口，
            // 确保 adjtime 收敛后再写 RTC。
            settled = (waited_ms >= SNTP_SMOOTH_SETTLE_MS);
        }

        if (settled) {
            rtc_last_calibrate_seq = sntp_sync_seq;
            last_ntp_sync_time = sntp_sync_tv_sec;   // SNTP 获得的 Unix 时间
            synced_once = true;
            time_source = "SNTP";

            Serial.printf("[Time] SNTP sync OK (%s)\n",
                          time_get_string(sntp_sync_tv_sec).c_str());
            event_push(EVENT_NTP_SYNC_OK, "", "time_manager",
                       EVENT_PRIORITY_NORMAL, EVENT_POLICY_NORMAL, 0);

            // SNTP 成功后：校准 RTC
            //   顺序固定为 callback → 标记 → loop 确认 → 读 time(nullptr)
            //   → 读 RTC → 比较 → 必要时写 RTC。
            //   校准写入值在 time_rtc_calibrate() 内取 time(nullptr)，
            //   不使用 sntp_sync_tv_sec（见该函数注释）。
            time_rtc_calibrate();
        }
    }

    // -------------------------
    // 3) time_valid 维护（以 System Time 是否进入有效区间为准）
    // -------------------------
    time_t sys = time(nullptr);
    bool now_valid = time_validate(sys);
    if (now_valid) {
        state_set_bool(STATE_TIME_VALID, true);
    } else {
        state_set_bool(STATE_TIME_VALID, false);
    }
    time_update_valid_event(now_valid);

    // -------------------------
    // 4) 调试状态
    // -------------------------
    if (!state_get_bool(STATE_TIME_VALID)) {
        dbg_state = wifi ? 2 : 1;
    } else {
        dbg_state = 3;
    }
}

// =====================================================
// 获取系统时间（UTC Unix；无效返回 0）
// =====================================================

time_t time_get()
{
    if (!state_get_bool(STATE_TIME_VALID)) {
        return 0;
    }
    time_t now = time(nullptr);
    if (!time_validate(now)) {
        return 0;
    }
    return now;
}

// =====================================================
// 格式化时间（本地时区显示）
// =====================================================

String time_get_string(time_t timestamp)
{
    if (timestamp == 0) {
        return "No Time";
    }
    struct tm timeinfo;
    localtime_r(&timestamp, &timeinfo);
    char buffer[32];
    sprintf(buffer, "%04d-%02d-%02d %02d:%02d:%02d",
            timeinfo.tm_year + 1900,
            timeinfo.tm_mon + 1,
            timeinfo.tm_mday,
            timeinfo.tm_hour,
            timeinfo.tm_min,
            timeinfo.tm_sec);
    return String(buffer);
}

// 对外查询：当前本地时间字符串；无效返回 "No Time"
String time_now_string()
{
    time_t now = time_get();
    if (now == 0) {
        return "No Time";
    }
    return time_get_string(now);
}

// =====================================================
// 手动校时（Unix UTC）
//
// 流程：
//   1. 校验
//   2. RTC Write（Critical Operation：acquire → write → release）
//   3. settimeofday 硬同步 System Time
//   4. time_valid = true
//
// RTC Write 失败不视为致命：System Time 照常更新，RTC 下次 SNTP 再校准。
// =====================================================

bool time_set_manual(time_t timestamp)
{
    if (!time_validate(timestamp)) {
        return false;
    }

    bool need_rtc_write = rtc_present;
    bool acquired = false;

    if (need_rtc_write) {
        if (!system_command_critical_operation_acquire()) {
            // 重启挂起中：拒绝改动
            Serial.println("[Time] set_manual rejected: restart pending");
            return false;
        }
        acquired = true;
    }

    if (need_rtc_write) {
        if (!rtc_write_time(timestamp)) {
            Serial.println("[Time] set_manual: RTC write failed "
                           "(System Time will still be set)");
        } else {
            Serial.println("[Time] set_manual: RTC updated");
        }
    }

    if (acquired) {
        system_command_critical_operation_release();
    }

    time_set_system_clock(timestamp);
    state_set_bool(STATE_TIME_VALID, true);
    time_source = "RTC";
    return true;
}

// =====================================================
// 手动校时（本地时区墙钟字符串 "YYYY-MM-DD HH:MM:SS"）
// =====================================================

bool time_set_manual_string(const String &time_string)
{
    struct tm tm_data;
    memset(&tm_data, 0, sizeof(tm_data));

    int result = sscanf(time_string.c_str(), "%d-%d-%d %d:%d:%d",
                        &tm_data.tm_year,
                        &tm_data.tm_mon,
                        &tm_data.tm_mday,
                        &tm_data.tm_hour,
                        &tm_data.tm_min,
                        &tm_data.tm_sec);
    if (result != 6) {
        return false;
    }
    tm_data.tm_year -= 1900;
    tm_data.tm_mon -= 1;

    time_t manual_time = mktime(&tm_data);   // 按本地 TZ 解释
    return time_set_manual(manual_time);
}

// =====================================================
// 主动触发 SNTP 立即校时（非阻塞）
// =====================================================

bool time_sync_ntp()
{
    if (!sntp_configured) {
        return false;
    }
    if (!state_get_bool(STATE_WIFI_STATUS)) {
        Serial.println("[Time] NTP skip: no wifi");
        return false;
    }
    sntp_do_restart();
    return true;
}

time_t time_get_last_ntp_sync()
{
    return last_ntp_sync_time;
}

int time_get_state()
{
    return (int)dbg_state;
}

// =====================================================
// 时间查询（system.get_time 命令）
//
// 非阻塞：只做一次 RTC 短读 + 本地时间转换。
// =====================================================

bool time_query(TimeQueryResult &out)
{
    out.system_valid = state_get_bool(STATE_TIME_VALID);
    time_t sys = time(nullptr);

    if (out.system_valid && time_validate(sys)) {
        out.system_unix = sys;
        out.system_local = time_get_string(sys);
    } else {
        out.system_unix = 0;
        out.system_local = "No Time";
    }

    out.rtc_present = rtc_present;
    time_t rtc = 0;
    if (rtc_present && rtc_read_time(rtc)) {
        out.rtc_valid = time_validate(rtc);
        if (out.rtc_valid) {
            out.rtc_unix = rtc;
            out.rtc_local = time_get_string(rtc);
        } else {
            out.rtc_unix = 0;
            out.rtc_local = "No Time";
        }
    } else {
        out.rtc_valid = false;
        out.rtc_unix = 0;
        out.rtc_local = "No Time";
    }

    out.source = (time_source != nullptr) ? time_source : "INVALID";
    out.last_ntp_sync = last_ntp_sync_time;
    out.ntp_started = sntp_started;
    out.timezone_offset_h = timezone_offset_hours;
    return true;
}

// =====================================================
// 预留：set_time 系统命令入口
//
// 由 command_manager 的 system.set_time 路由调用。
// V2 保持预留：手动校时应走 time_set_manual() / time_set_manual_string()
// （云命令解析由 CommandManager 负责）。
// =====================================================
void time_manager_set_time()
{
    Serial.println("[Time] set_time: reserved (use manual APIs directly)");
}
