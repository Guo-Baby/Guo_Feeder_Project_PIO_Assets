#include <Arduino.h>
#include <WiFi.h>
#include "wifi_module.h"
#include "config_manager.h"
#include "system_state.h"
#include "event_manager.h"
#include "log_manager.h"

// ==========================
// WiFi状态机
// ==========================
enum WifiState
{
    WIFI_IDLE,
    WIFI_CONNECTING,
    WIFI_CONNECTED,
    WIFI_DISCONNECTED
};

static WifiState wifi_state =
    WIFI_IDLE;

// ==========================
// WiFi运行状态
// ==========================
static bool wifi_connected = false;
// 连接开始时间
static unsigned long wifi_connect_start = 0;
// 重连计时
static unsigned long lastReconnectTime = 0;

// ==========================
// 信号更新
// ==========================
static unsigned long lastSignalUpdate = 0;
static unsigned long signal_update_interval = 10000;

// ==========================
// 参数
// ==========================
static unsigned long wifi_connect_timeout = 30000;
static unsigned long wifi_reconnect_interval = 10000;

// ==========================
// P2-C：LogManager 埋点用局部状态
//
// ⚠️ 以下变量**只服务于日志**，不参与任何状态机判定、不改变连接流程、
//    不引入任何等待/阻塞。wifi_task() 的分支逻辑与接入前完全一致。
// ==========================

// 累计发起连接次数（含重连）；永不复位 —— 就是"device 一生中第几次尝试"
static uint32_t wifi_attempt_n = 0;

// 断线后**连续**重连次数；连接成功时复位 ⇒ RETRY_N 语义 = "本次掉线第几次尝试"
static uint32_t wifi_retry_n = 0;

// 最近一次进入 WIFI_CONNECTED 的时刻（用于 LOST 时的 CONNECTED_MS）
static unsigned long wifi_connected_since = 0;

// 重连日志节流：最近一次发出 RECONNECT_TRY 的时刻
static unsigned long wifi_reconnect_log_ms = 0;

// 连接超时日志节流：累计超时次数 + 最近一次发出 CONNECT_TIMEOUT 的时刻
//
// ⚠️ 为什么 CONNECT_TIMEOUT 也要节流：本状态机里 timeout 与 reconnect 是**同一个
//    失败循环**的两半（超时 → DISCONNECTED → 重连 → 再超时…）。默认
//    connect_timeout=30s ⇒ 失败循环每 ~30s 一轮 ⇒ 超时事件本身就是 2 条/分钟
//    （WARN，落 Flash）。只节流 RECONNECT_TRY 的话，总速率仍被超时事件主导。
static uint32_t wifi_timeout_n = 0;
static unsigned long wifi_timeout_log_ms = 0;

// 节流参数（审查报告 §5 H-8 / §5.3「N 次记 1 次」）
//   全量 ⇒ 失败循环约 4 条/分钟；节流后 ⇒ 每事件约 1 条 / 150s（≈0.8 条/分钟）
#define WIFI_FAIL_LOG_EVERY_N   5u
#define WIFI_FAIL_LOG_MIN_MS    60000u

// 通用节流门：失败循环的第 n 次，是否应记一条
//
//   n        该事件自身的累计次数（1 起）
//   last_ms  **该事件**上一次记录的时刻（0 = 从未记过）
//
// ⚠️ 两个条件是 **AND** 而不是 OR：
//   · N 次记 1 次是主体
//   · MIN_MS 是"最小间隔下限"，用于防止把 reconnect_interval / connect_timeout
//     配得过小（如 1s）时"N 次"也很快 ⇒ 又变洪泛
//   若写成 OR，60s 会反过来变成**上限**：当尝试间隔本来就 >12s 时它会架空
//   N 次规则、让日志**变多**（实测 30s 一轮时 OR 每 60s 一条、AND 每 150s 一条）。
static bool wifi_fail_should_log(
    uint32_t n,
    unsigned long *last_ms
)
{
    if(
        (
            n
            %
            WIFI_FAIL_LOG_EVERY_N
        )
        !=
        1u
    )
    {
        return false;
    }

    if(
        *last_ms
        !=
        0
        &&
        (
            millis()
            -
            *last_ms
        )
        <
        WIFI_FAIL_LOG_MIN_MS
    )
    {
        return false;
    }

    return true;
}

// SSID 哈希（FNV-1a 32）
//
// 为什么不直接传字符串：`LogParamIn` 的 union 只有 {i,u,f,b}，**无 blob 字段**，
// `LOG_PTYPE_STR` 当前不可构造 ⇒ 按 P2 定版走「哈希/枚举化」（沿用
// ConfigManager 的 `cfg_hash32()` 先例，不为此扩 LogManager API）。
// ⚠️ 该 helper 与 config_manager.cpp 的 cfg_hash32() **重复**；按本次约束
//    （只在 wifi_module.cpp 内改动）先本地实现，后续多模块共用时应上移为共享工具。
static uint32_t wifi_ssid_hash32(const char *s)
{
    uint32_t h = 2166136261u;

    while (s != nullptr && *s != '\0')
    {
        h ^= (uint8_t)(*s++);
        h *= 16777619u;
    }

    return h;
}


// ==========================
// RSSI转换信号等级
// ==========================
static int wifi_calculate_signal(
    int rssi
)
{
    if(rssi >= -50)
    {
        return 4;
    }
    else if(rssi >= -65)
    {
        return 3;
    }
    else if(rssi >= -75)
    {
        return 2;
    }
    else if(rssi >= -90)
    {
        return 1;
    }
    else
    {
        return 0;
    }
}

// ==========================
// 更新WiFi信号状态
// ==========================
static void wifi_update_signal()
{
    if( 
        millis()
        -
        lastSignalUpdate
        <
        signal_update_interval
    )
    {
        return;
    }
    lastSignalUpdate =
        millis();
    if(
        WiFi.status()
        !=
        WL_CONNECTED
    )
    {
        return;
    }
    int rssi =
        WiFi.RSSI();
    state_set_int(
        STATE_WIFI_RSSI,
        rssi
    );
    state_set_int(
        STATE_WIFI_SIGNAL,
        wifi_calculate_signal(rssi)
    );
}

// ==========================
// WiFi初始化
// ==========================
// 前向声明：定义在文件下方，init 阶段需要提前调用一次发起连接
static void wifi_start_connect();

void wifi_init()
{
    Serial.println();
    Serial.println(
        "WiFi init..."
    );
    WiFi.mode(
        WIFI_STA
    );
    wifi_connect_timeout =
        config_get_wifi_connect_timeout();
    wifi_reconnect_interval =
        config_get_wifi_reconnect_interval();
    wifi_state =
        WIFI_IDLE;
    wifi_connected =
        false;
    state_set_bool(
        STATE_WIFI_STATUS,
        false
    );
    state_set_int(
        STATE_WIFI_STATE,
        WIFI_IDLE
    );
    state_set_int(
        STATE_WIFI_RSSI,
        -100
    );
    state_set_int(
        STATE_WIFI_SIGNAL,
        0
    );

    // 初始化阶段直接发起一次 WiFi 连接（按 config 中的 ssid / password）。
    // loop 中的 wifi_task 仍负责状态轮询与断线后的重连，此处只提前"开一次头"。
    wifi_start_connect();
}

// ==========================
// 开始连接
// ==========================
static void wifi_start_connect()
{
    String ssid =
        config_get_wifi_ssid();
    String password =
        config_get_wifi_password();
    Serial.println();
    Serial.print(
        "Connecting to:"
    );
    Serial.println(
        ssid
    );

    // ---- P2-C 埋点：LOG_WIFI_CONNECT_START ----
    //
    // 只在"非重连"时发（wifi_retry_n == 0）：
    //   重连的每一次由 LOG_WIFI_RECONNECT_TRY 表达（见 wifi_task 的
    //   WIFI_DISCONNECTED 分支），否则两个事件会在同一个 10s tick 上重复。
    //   ⇒ 本事件天然低频（开机 / IDLE 重入），无需节流。
    const uint32_t was_state =
        (uint32_t)wifi_state;
    const uint32_t ssid_hash =
        wifi_ssid_hash32(
            ssid.c_str()
        );
    wifi_attempt_n++;

    WiFi.begin(
        ssid.c_str(),
        password.c_str()
    );
    wifi_connect_start =
        millis();
    wifi_state =
        WIFI_CONNECTING;
    state_set_int(
        STATE_WIFI_STATE,
        WIFI_CONNECTING
    );

    if(wifi_retry_n == 0)
    {
        LogParamIn p[4];
        p[0] =
            log_arg_u32(
                LOG_P_SSID_HASH,
                ssid_hash
            );
        p[1] =
            log_arg_u32(
                LOG_P_ATTEMPT_N,
                wifi_attempt_n
            );
        p[2] =
            log_arg_enum(
                LOG_P_WAS,
                was_state
            );
        p[3] =
            log_arg_enum(
                LOG_P_STATE,
                (uint32_t)WIFI_CONNECTING
            );
        log_emit(
            LOG_WIFI_CONNECT_START,
            LOG_LVL_INFO,
            p,
            4
        );
    }
}

// ==========================
// WiFi任务
// loop调用
// ==========================
void wifi_task()
{
    wifi_update_signal();
    switch(wifi_state)
    {
        case WIFI_IDLE:
        {
            wifi_start_connect();
            break;
        }
        case WIFI_CONNECTING:
        {
            if(
                WiFi.status()
                ==
                WL_CONNECTED
            )
            {
                if(!wifi_connected)
                {
                    Serial.println(
                        "WiFi connected"
                    );

                    // ---- P2-C 埋点：LOG_WIFI_CONNECTED（边沿内，只发一次）----
                    const uint32_t connect_ms =
                        (uint32_t)(
                            millis()
                            -
                            wifi_connect_start
                        );
                    LogParamIn p[5];
                    p[0] =
                        log_arg_u32(
                            LOG_P_CONNECT_MS,
                            connect_ms
                        );
                    p[1] =
                        log_arg_i32(
                            LOG_P_RSSI,
                            WiFi.RSSI()
                        );
                    p[2] =
                        log_arg_u32(
                            LOG_P_SSID_HASH,
                            wifi_ssid_hash32(
                                config_get_wifi_ssid()
                                    .c_str()
                            )
                        );
                    p[3] =
                        log_arg_enum(
                            LOG_P_WAS,
                            (uint32_t)WIFI_CONNECTING
                        );
                    p[4] =
                        log_arg_enum(
                            LOG_P_STATE,
                            (uint32_t)WIFI_CONNECTED
                        );
                    log_emit(
                        LOG_WIFI_CONNECTED,
                        LOG_LVL_INFO,
                        p,
                        5
                    );

                    // 连接成功 ⇒ 连续重连计数复位（RETRY_N 语义 = "本次掉线第几次"）
                    wifi_retry_n =
                        0;
                    wifi_connected_since =
                        millis();

                    wifi_connected =
                        true;
                    state_set_bool(
                        STATE_WIFI_STATUS,
                        true
                    );
                    event_push(
                        EVENT_WIFI_CONNECTED,
                        "",
                        "wifi_module",
                        EVENT_PRIORITY_NORMAL,
                        EVENT_POLICY_NORMAL,
                        0
                    );
                }
                wifi_state =
                    WIFI_CONNECTED;
                state_set_int(
                    STATE_WIFI_STATE,
                    WIFI_CONNECTED
                );
            }
            else
            {
                if(
                    millis()
                    -
                    wifi_connect_start
                    >
                    wifi_connect_timeout
                )
                {
                    Serial.println(
                        "WiFi connect timeout"
                    );

                    // ---- P2-C 埋点：LOG_WIFI_CONNECT_TIMEOUT（WARN）----
                    //
                    // 与 RECONNECT_TRY 共用同一个失败循环节流门（见
                    // wifi_fail_should_log 的说明）。被抑制的轮次不丢信息：
                    // 发出的记录带 LOG_P_ATTEMPT_N（累计第几次连接尝试）。
                    wifi_timeout_n++;

                    if(wifi_fail_should_log(
                           wifi_timeout_n,
                           &wifi_timeout_log_ms
                       ))
                    {
                        LogParamIn p[4];
                    p[0] =
                        log_arg_u32(
                            LOG_P_TIMEOUT_MS,
                            (uint32_t)wifi_connect_timeout
                        );
                    p[1] =
                        log_arg_u32(
                            LOG_P_ATTEMPT_N,
                            wifi_attempt_n
                        );
                    p[2] =
                        log_arg_enum(
                            LOG_P_WAS,
                            (uint32_t)WIFI_CONNECTING
                        );
                    p[3] =
                        log_arg_enum(
                            LOG_P_STATE,
                            (uint32_t)WIFI_DISCONNECTED
                        );
                    log_emit(
                        LOG_WIFI_CONNECT_TIMEOUT,
                        LOG_LVL_WARN,
                        p,
                        4
                    );

                        wifi_timeout_log_ms =
                            millis();
                    }

                    wifi_state =
                        WIFI_DISCONNECTED;
                    state_set_bool(
                        STATE_WIFI_STATUS,
                        false
                    );
                    state_set_int(
                        STATE_WIFI_STATE,
                        WIFI_DISCONNECTED
                    );
                }
            }
            break;
        }
        case WIFI_CONNECTED:
        {
            if(
                WiFi.status()
                !=
                WL_CONNECTED
            )
            {
                Serial.println(
                    "WiFi lost"
                );

                // ---- P2-C 埋点：LOG_WIFI_LOST（WARN，现场断网首因判据）----
                //
                // · CONNECTED_MS = 本次在线时长
                // · RSSI         = **最后一次采样到的** RSSI（此刻已断连，
                //                  WiFi.RSSI() 只会返回 -100，故取 STATE_WIFI_RSSI）
                // · CAUSE        = 断连瞬间 WiFi.status()（WL_CONNECTION_LOST /
                //                  WL_DISCONNECTED / WL_CONNECT_FAILED …）
                const uint32_t connected_ms =
                    (wifi_connected_since == 0)
                        ? 0u
                        : (uint32_t)(
                              millis()
                              -
                              wifi_connected_since
                          );
                LogParamIn p[5];
                p[0] =
                    log_arg_u32(
                        LOG_P_CONNECTED_MS,
                        connected_ms
                    );
                p[1] =
                    log_arg_i32(
                        LOG_P_RSSI,
                        state_get_int(
                            STATE_WIFI_RSSI
                        )
                    );
                p[2] =
                    log_arg_enum(
                        LOG_P_CAUSE,
                        (uint32_t)WiFi.status()
                    );
                p[3] =
                    log_arg_enum(
                        LOG_P_WAS,
                        (uint32_t)WIFI_CONNECTED
                    );
                p[4] =
                    log_arg_enum(
                        LOG_P_STATE,
                        (uint32_t)WIFI_DISCONNECTED
                    );
                log_emit(
                    LOG_WIFI_LOST,
                    LOG_LVL_WARN,
                    p,
                    5
                );

                // 新的一次掉线开始 ⇒ 连续重连计数从 0 起
                wifi_retry_n =
                    0;

                wifi_connected =
                    false;
                state_set_bool(
                    STATE_WIFI_STATUS,
                    false
                );
                event_push(
                    EVENT_WIFI_DISCONNECTED,
                    "",
                    "wifi_module",
                    EVENT_PRIORITY_NORMAL,
                    EVENT_POLICY_NORMAL,
                    0
                );
                wifi_state =
                    WIFI_DISCONNECTED;
                state_set_int(
                    STATE_WIFI_STATE,
                    WIFI_DISCONNECTED
                );
            }
            break;
        }
        case WIFI_DISCONNECTED:
        {
            if(
                millis()
                -
                lastReconnectTime
                >
                wifi_reconnect_interval
            )
            {
                lastReconnectTime =
                    millis();
                Serial.println(
                    "Try reconnect..."
                );

                // ---- P2-C 埋点：LOG_WIFI_RECONNECT_TRY（WARN，**必须节流**）----
                //
                // 默认 reconnect_interval = 10s ⇒ 全量 6 条/分钟（WARN 会落 Flash）。
                // 采用审查报告 §5.3「N 次记 1 次」+ §5 H-8 的 60s 最小间隔：
                //   · 每 5 次尝试记 1 次
                //   · **且**距本事件上一条 ≥60s（下限，防 interval 配得过小）
                // 被抑制的尝试**不丢信息**：发出的记录带 LOG_P_RETRY_N（本次掉线
                // 第几次尝试）与 LOG_P_ATTEMPT_N（累计第几次），云端可还原节奏。
                //
                // ⚠️ 必须在 wifi_start_connect() **之前**自增：后者据此判断
                //    "这次是重连" ⇒ 不再重复发 LOG_WIFI_CONNECT_START。
                wifi_retry_n++;

                if(wifi_fail_should_log(
                       wifi_retry_n,
                       &wifi_reconnect_log_ms
                   ))
                {
                    LogParamIn p[4];
                    p[0] =
                        log_arg_u32(
                            LOG_P_RETRY_N,
                            wifi_retry_n
                        );
                    p[1] =
                        log_arg_u32(
                            LOG_P_ATTEMPT_N,
                            wifi_attempt_n
                        );
                    p[2] =
                        log_arg_enum(
                            LOG_P_WAS,
                            (uint32_t)WIFI_DISCONNECTED
                        );
                    p[3] =
                        log_arg_enum(
                            LOG_P_STATE,
                            (uint32_t)WIFI_CONNECTING
                        );
                    log_emit(
                        LOG_WIFI_RECONNECT_TRY,
                        LOG_LVL_WARN,
                        p,
                        4
                    );

                    wifi_reconnect_log_ms =
                        millis();
                }

                wifi_start_connect();
            }
            break;
        }
    }
}

// ==========================
// 查询状态
// ==========================
bool wifi_is_connected()
{
    return wifi_connected;
}

// ==========================
// RSSI查询接口
// ==========================
int wifi_get_rssi()
{
    if(
        WiFi.status()
        ==
        WL_CONNECTED
    )
    {
        return WiFi.RSSI();
    }
    return -100;
}

// ==========================
// 信号等级查询
// ==========================
int wifi_get_signal_quality()
{
    return wifi_calculate_signal(
        wifi_get_rssi()
    );
}