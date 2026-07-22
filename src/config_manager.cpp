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

    // 挂载ESP32内部LittleFS文件系统
    if(!LittleFS.begin())
    {
        Serial.println("LittleFS mount failed");
        return false;
    }

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
    // 以写入模式打开配置文件
    File file =
        LittleFS.open(
            CONFIG_FILE,
            "w"
        );

    // 文件打开失败
    if(!file)
    {
        return false;
    }

    // 将JSON对象写入Flash
    serializeJson(
        config,
        file
    );

    // 关闭文件
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