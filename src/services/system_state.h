#ifndef SYSTEM_STATE_H
#define SYSTEM_STATE_H

#include <Arduino.h>


// =====================================================
// System State 类型
//
// 所有系统共享状态统一登记位置
//
// 规则：
// 1. 谁产生状态，谁调用 state_set_xxx()
// 2. 所有模块通过 state_get_xxx()读取
// 3. 不允许业务逻辑写入其他模块状态
//
// =====================================================


enum SystemStateKey
{
    // WiFi
    STATE_WIFI_STATUS,//连接状态，bool值
    STATE_WIFI_RSSI,//信号分贝
    STATE_WIFI_SIGNAL,//信号质量，int值，规则为0，1极差，2中等，3良好，4极好
    STATE_WIFI_STATE,//WiFi模块工作状态机,int值，规则为0 idle，1 connecting，2 connected，3 disconnected
    // 时间
    //
    STATE_TIME_VALID,
    // 阀门
    STATE_VALVE_STATUS,
    // 重量(g)
    STATE_WEIGHT_VALUE,
    // 称重异常
    STATE_WEIGHT_ERROR,
    // 称重可用性（weight.enable=false ⇒ false；
    // 与 STATE_MI_THERMO_VALID 同构的"专用有效性"键）
    STATE_WEIGHT_VALID,
    // Event队列数量
    STATE_EVENT_QUEUE_COUNT,
    //MQTT连接状态
    STATE_MQTT_STATUS,
    //MQTT重连次数
    STATE_MQTT_RETRY_COUNT,
    //MQTT最后连接时间
    STATE_MQTT_LAST_CONNECT_TIME,
    //MQTT最后错误信息
    STATE_MQTT_LAST_ERROR,
    // 米家BLE温湿度计 LYWSD03MMC
    STATE_MI_THERMO_TEMP,
    STATE_MI_THERMO_HUMID,
    STATE_MI_THERMO_BAT_V,

    STATE_MI_THERMO_TEMP_TS,
    STATE_MI_THERMO_HUMID_TS,
    STATE_MI_THERMO_BAT_TS,
    
    STATE_MI_THERMO_ENABLE,
    STATE_MI_THERMO_VALID,

    // 步进电机（stepper）
    //
    // ★ 三个概念严格分开，绝不合并成同一个布尔：
    //   stepper_enable  = 配置是否允许使用该模块
    //   stepper_busy    = 当前是否有动作正在运行
    //   stepper_last_result = 最近一次动作结果（StepperResult）
    STATE_STEPPER_ENABLE,
    STATE_STEPPER_BUSY,
    STATE_STEPPER_LAST_RESULT,
    STATE_STEPPER_LAST_PULSES,

    // 振动马达（vibration，单 GPIO 双马达共路）
    STATE_VIBRATION_ENABLE,
    STATE_VIBRATION_ACTIVE,
    STATE_VIBRATION_LAST_RESULT,
    STATE_VIBRATION_LAST_DURATION_MS,

    // 米家 LAN 设备（喂食器 / 智能插座）—— 直连局域网，不经云端
    //
    // ★ 三个概念分开（与 stepper 同构）：
    //   *_enable = config 是否允许使用该设备
    //   *_online = 最近一次局域网通信是否成功（超时/校验失败 ⇒ false，**不重试**）
    STATE_XIAOMI_FEEDER_ENABLE,
    STATE_XIAOMI_FEEDER_ONLINE,
    // 2/26 out_status：0 空闲 / 1 出粮中 / 其他按设备枚举
    STATE_XIAOMI_FEEDER_OUT_STATUS,
    STATE_XIAOMI_PLUG_ENABLE,
    STATE_XIAOMI_PLUG_ONLINE,
    // 2/1 power
    STATE_XIAOMI_PLUG_POWER,
    // 最近一次 LAN 动作结果：0 未执行 / 1 成功 / 2 失败（**不代表已重试**）
    STATE_XIAOMI_LAN_LAST_RESULT,

    /*
     * ★ [2026-10-29 · 阶段 O 后续] 把"失败原因"从 `*_online=false` 里拆出来。
     *
     * 背景（真机实测 + 代码核对）：`*_online` 只在 LAN 探活成功/失败时被写，
     * 失败时一律 false ⇒ UI 只能显示"离线"，无法区分：
     *   · 从未查询过（刚启动，还没探活）
     *   · 模块被停用（根本不探活）
     *   · 未配置 IP/Token
     *   · 局域网无响应（超时）
     *   · 收到回包但校验和/解密失败（= Token 不对，设备其实在线）
     *   · 上一次请求还在进行（忙，本次未发出）
     * 这些原因的处理方式完全不同，混成"离线"会直接误导用户
     * （典型误判：Token 错 ⇒ 显示离线 ⇒ 用户以为设备断电）。
     *
     * 编码见 `xiaomi_lan.h` 的 `XIAOMI_ERR_*`；0 = 尚未查询。
     */
    STATE_XIAOMI_FEEDER_LAST_ERR,
    STATE_XIAOMI_PLUG_LAST_ERR,
    // 最近一次**成功**通信的 Unix 秒（0 = 从未成功）。用于"上次成功查询时间"
    STATE_XIAOMI_FEEDER_LAST_OK_TS,
    STATE_XIAOMI_PLUG_LAST_OK_TS,
    // 只读诊断计数（脱敏、不含任何凭据）：收包数 / 校验失败数 / 超时数
    STATE_XIAOMI_LAN_RECV,
    STATE_XIAOMI_LAN_BADSUM,
    STATE_XIAOMI_LAN_TIMEOUT_CNT,
    // 普通通电的自动断电剩余秒数（0 = 无待执行自动断电）
    STATE_XIAOMI_PLUG_AUTOOFF_SEC,

    /*
     * ★ [2026-10-30 · 任务 A2] 凭据是否已配置（**只读布尔，绝不暴露任何凭据值**）。
     *
     * 为什么需要它：IP/Token 只写 NVS（`gflan`），`config.get` 永远读不到
     * ⇒ 配置页没法回答"这台设备的 Token 到底存进去了没有、重启后还在不在"。
     * 之前只能靠 `*_last_err==4`（NO_CFG）间接推断，但模块被停用（err=5）时
     * 就完全不可知，用户会误以为"保存失败"。
     *
     * 语义：`true` = NVS 里同时有合法 IP 与 32-hex Token（`loadDev()` 成功）；
     *       在 `xiaomi_lan_init()` 与每次 `lan_prov` 写入后重新计算 ⇒
     *       重启后仍为 true 即证明**已落 NVS**。
     */
    STATE_XIAOMI_FEEDER_CONFIGURED,
    STATE_XIAOMI_PLUG_CONFIGURED,

    //新增state需要写在上方，同时维护cpp内的state_map映射表，state_get_field_count()和state_get_field_name()函数会自动更新，无需维护
    // 状态数量
    STATE_MAX,
};
// =====================================================
// 状态值类型
// =====================================================
enum StateValueType
{
    STATE_TYPE_INT,
    STATE_TYPE_BOOL,
    STATE_TYPE_LONG,
    STATE_TYPE_FLOAT,
    STATE_TYPE_STRING
};

// =====================================================
// 状态存储结构
// ====================================================
struct SystemStateValue
{
    StateValueType type;
    long value;
    float value_float;
    String value_string;
    bool valid;
};

// =====================================================
// 初始化
// =====================================================

void system_state_init();
// =====================================================
// 写接口
//
// 对应模块内部调用
// =====================================================
bool state_set_int(SystemStateKey key,int value);
bool state_set_bool(SystemStateKey key,bool value);
bool state_set_long(SystemStateKey key,long value);
bool state_set_float(SystemStateKey key,float value);
bool state_set_string(SystemStateKey key,String value);

// =====================================================
// 读接口，所有模块公开读取
// =====================================================
int state_get_int(
    SystemStateKey key
);
bool state_get_bool(
    SystemStateKey key
);
long state_get_long(
    SystemStateKey key
);
float state_get_float(
    SystemStateKey key
);

String state_get_string(
    SystemStateKey key
);
// =====================================================
// 查询有效性
// =====================================================
bool state_is_valid(SystemStateKey key);
// =====================================================
// 枚举 → 字符串 映射
// =====================================================
const char* state_key_to_string(SystemStateKey key);
SystemStateKey state_string_to_key(const String &str);

// =====================================================
// 通用查询接口（供 Command Manager 调用）
// =====================================================
bool system_state_query(const String &key,String &output);

// =====================================================
// 新增：获取所有字段列表（供 UI 使用）
// =====================================================
uint8_t state_get_field_count();
const char* state_get_field_name(uint8_t index);


#endif
