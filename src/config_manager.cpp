#include <Arduino.h>
#include <LittleFS.h>
#include <ArduinoJson.h>

#include "config_manager.h"

// =============================
// 配置文件路径
// =============================
// LittleFS根目录下的配置文件
#define CONFIG_FILE "/config.json"

// =============================
// 保存到RAM中的配置
// =============================
// 程序启动时，从LittleFS读取config.json
// 解析后的JSON数据保存在这里
//
// 后续OLED、WiFi、电机等模块
// 不直接访问文件
// 而是通过config_manager提供的函数读取
static JsonDocument config;

// =============================
// 配置初始化
// =============================
// 功能：
// 1. 挂载LittleFS文件系统
// 2. 打开config.json
// 3. 使用ArduinoJson解析配置
// 4. 保存到RAM中的config对象
//
// 返回：
// true  = 初始化成功
// false = 初始化失败
bool config_init()
{
    Serial.println("Config init...");

    // 打开配置文件
    File file = LittleFS.open(
        CONFIG_FILE,
        "r"
    );

    // 配置文件不存在
    if(!file)
    {
        Serial.println("Config file missing");
        return false;
    }

    // 将JSON文件解析到RAM变量config
    DeserializationError error =
        deserializeJson(
            config,
            file
        );

    // 文件读取完成，关闭文件
    file.close();

    // JSON格式错误
    if(error)
    {
        Serial.print("JSON error:");
        Serial.println(
            error.c_str()
        );
        return false;
    }

    Serial.println("Config loaded OK");
    return true;
}

// =============================
// 保存配置
// =============================
// 功能：
// 将RAM中的配置重新写回LittleFS
//
// 使用场景：
// 手机修改参数后
// 修改RAM中的config
// 调用此函数永久保存
bool config_save()
{
    File file = LittleFS.open(CONFIG_FILE, "w");
    if (!file) {
        return false;
    }
    serializeJson(config, file);
    file.close();
    return true;
}

// =============================
// WiFi配置读取
// =============================
// 获取WiFi名称SSID
String config_get_wifi_ssid()
{
    return config["wifi"]["ssid"]
        |
        String("");
}

// 获取WiFi密码
String config_get_wifi_password()
{
    return config["wifi"]["password"]
        |
        String("");
}

// WiFi连接超时最大等待时间
int config_get_wifi_connect_timeout()
{
    return config["wifi"]["connect_timeout"]
        |
        30000;
}

// WiFi自动重连间隔
int config_get_wifi_reconnect_interval()
{
    return config["wifi"]["reconnect_interval"]
        |
        10000;
}




// =============================
// OLED配置读取
// =============================
// 获取OLED SDA GPIO
int config_get_oled_sda()
{
    return config["oled"]["sda"]
        |
        4;
}

// 获取OLED SCL GPIO
int config_get_oled_scl()
{
    return config["oled"]["scl"]
        |
        5;
}

// 获取OLED I2C通信速度
int config_get_oled_speed()
{
    return config["oled"]["i2c_speed"]
        |
        100000;
}

// 获取OLED动画帧间隔时间(ms)
//
// 例如：
// 50ms = 每秒20帧
int config_get_oled_frame_delay()
{
    return config["oled"]["frame_delay"]
        |
        50;
}

// =============================
// OLED旋转方向读取
// =============================
int config_get_oled_rotation()
{
    return config["oled"]["rotation"]
        |
        0;
}

// =============================
// Valve 阀门配置读取
// =============================
int config_get_valve_gpio_pin()
{
    return config["valve"]["gpio_pin"] | -1;
}

int config_get_valve_active_level()
{
    return config["valve"]["active_level"] | 1;
}

int config_get_valve_open_duration_ms()
{
    return config["valve"]["open_duration_ms"] | 0;
}

bool config_get_valve_enable()
{
    return config["valve"]["enable"] | true;
}

int config_get_valve_safety_timeout_sec()
{
    return config["valve"]["safety_timeout_sec"] | 300;
}

// =============================
// 系统配置读取
// =============================
// 获取时区
//
// NTP时间同步时使用
// 中国通常为UTC+8
int config_get_timezone()
{
    return config["system"]["timezone"]
        |
        8;
}

// NTP地址1
String config_get_ntp_server1()
{
    return config["time"]["ntp_server1"]
        |
        String("ntp.aliyun.com");
}

// NTP地址2
String config_get_ntp_server2()
{
    return config["time"]["ntp_server2"]
        |
        String("cn.pool.ntp.org");
}



int config_get_ntp_sync_interval_day()
{
    return config["time"]["sync_interval_day"]
        |
        7;
}


int config_get_ntp_sync_hour()
{
    if (!config.containsKey("time")) return 4;
    JsonObject time = config["time"];
    if (!time["sync_hour"].is<int>()) return 4;
    return time["sync_hour"].as<int>();
}

int config_get_ntp_sync_minute()
{
    if (!config.containsKey("time")) return 10;
    JsonObject time = config["time"];
    if (!time.containsKey("sync_minute")) return 10;
    return time["sync_minute"].as<int>();
}


// ==========================
// HX711称重模块配置读取
// ==========================
int config_get_weight_dt()
{
    return config["weight"]["dt"]
    |
    4;
}

int config_get_weight_sck()
{
    return config["weight"]["sck"]
    |
    5;
}

int config_get_weight_sample_interval()
{
    return config["weight"]["sample_interval"]
    |
    50;
}

float config_get_weight_scale()
{
    return config["weight"]["scale"]
    |
    741.0;
}

long config_get_weight_zero_offset()
{
    return config["weight"]["zero_offset"]
    |
    0;
}

int config_get_weight_filter_samples()
{
    return config["weight"]["filter_samples"]
    |
    10;
}

bool config_set_weight_zero_offset(
    long offset
)
{
    config["weight"]["zero_offset"]
        = offset;
}


// =====================================================
// Bemfa_Cloud MQTT配置读取
// =====================================================


// =====================================================
// MQTT Cloud 配置读取
// =====================================================
String config_get_mqtt_server()
{
    return config["mqtt_cloud"]["mqtt_server"]
           |
           "mqtt.bemfa.com";
}
int config_get_mqtt_port()
{
    return config["mqtt_cloud"]["mqtt_port"]
           |
           9501;
}
String config_get_mqtt_client_id()
{
    return config["mqtt_cloud"]["client_id"]
           |
           "";
}
String config_get_mqtt_subscribe_topic()
{
    return config["mqtt_cloud"]["subscribe_topic"]
           |
           "";
}
String config_get_mqtt_username()
{
    return config["mqtt_cloud"]["username"]
           |
           "";
}
String config_get_mqtt_password()
{
    return config["mqtt_cloud"]["password"]
           |
           "";
}
String config_get_mqtt_publish_topic()
{
    return config["mqtt_cloud"]["publish_topic"]
           |
           "guo_feeder/up";
}
String config_get_mqtt_ca_path()
{
    return config["mqtt_cloud"]["ca_path"]
           |
           "/emqxsl-ca.crt";
}
// MQTT 重连间隔(ms)
unsigned long config_get_mqtt_retry_interval()
{
    return config["mqtt_cloud"]["retry_interval"]
           |
           8000;
}
// 最大快速重试次数
int config_get_mqtt_retry_max()
{
    return config["mqtt_cloud"]["retry_max"]
           |
           30;
}
// 进入休眠重试间隔(ms)
unsigned long config_get_mqtt_sleep_interval()
{
    return config["mqtt_cloud"]["sleep_retry_interval"]
           |
           3600000;
}
// MQTT keep alive 秒
int config_get_mqtt_keep_alive()
{
    return config["mqtt_cloud"]["mqtt_keep_alive"]
           |
           60;
}
// =============================
// WiFi 配置更新（预留）
// =============================
// 当前为空实现（占位），仅保证 CommandManager 路由可编译；
// 未实现时返回 false，由调用方统一错误上报。
bool config_update_wifi(
    const String &ssid,
    const String &password
)
{
    (void)ssid;
    (void)password;
    // 预留：未来实现配置更新与保存
    return false;
}
//=============================================
// 米家LYWSD03MMC温湿度计配置读取
//=============================================
void config_get_mithermometer_blekey(char *buf, size_t buf_size)
{
    if (buf == nullptr || buf_size == 0)
    {
        return;
    }
    // 缺失兜底：取 ""，和你现有 | "" 写法保持一致
    const char *src = config["MiThermometer"]["BLE_key"] | "";
    // strncpy 拷贝，保证末尾'\0'，不使用memset
    size_t copy_len = (strlen(src) >= buf_size) ? (buf_size - 1U) : strlen(src);
    strncpy(buf, src, copy_len);
    buf[copy_len] = '\0';
}

void config_get_mithermometer_mac(char *buf, size_t buf_size)
{
    if (buf == nullptr || buf_size == 0)
    {
        return;
    }
    const char *src = config["MiThermometer"]["MAC"] | "";
    size_t copy_len = (strlen(src) >= buf_size) ? (buf_size - 1U) : strlen(src);
    strncpy(buf, src, copy_len);
    buf[copy_len] = '\0';
}

bool config_get_mi_thermo_allow_collect()
{
    return config["mi_thermo_allow_collect"]
           |
           true;
}
