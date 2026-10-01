# 郭氏自动猫粮机项目（Guo Feeder Project）

## 项目简介

本项目是一套基于 ESP32-S3 的自动猫粮粉末投喂设备。

目标：

- 自动定量输出猫粮粉末
- 支持远程控制
- 支持状态显示
- 支持传感器检测
- 支持后续智能化扩展

当前阶段：
硬件验证与软件架构搭建阶段。


---

# 一、硬件平台


## 主控

MCU:

ESP32-S3 N16R8

主要参数：

- CPU：240MHz
- Flash：16MB
- PSRAM：8MB


## 显示模块

OLED：

- 尺寸：0.96寸
- 分辨率：128×64
- 驱动芯片：SSD1315
- 通信方式：I2C


当前OLED测试：

已完成：

- I2C通信测试
- SSD1315初始化
- U8g2驱动测试
- XBMP动画显示
- PROGMEM动画数组读取


---

# 二、当前工程结构

Guo_Feeder_Project

│
├── platformio.ini
│ PlatformIO工程配置
│
├── README.md
│ 项目说明文档
│
├── src
│
│ ├── main.cpp
│ │ 系统主程序入口
│ │
│ ├── oled.cpp
│ │ OLED显示功能实现
│ │
│ ├── oled.h
│ │ OLED模块接口定义
│ │
│ └── oled_animation.h
│ OLED动画点阵数据
│
└── backup
历史测试代码备份


---

# 三、软件架构原则


## main.cpp

职责：

系统总入口。

负责：

- 初始化各功能模块
- 调度各模块任务


禁止：

- 编写具体硬件驱动代码
- 存放大量业务逻辑


未来结构：
main.cpp

setup()
|
|-- oled_init()
|-- wifi_init()
|-- motor_init()
|-- sensor_init()

loop()

|-- oled_task()
|-- wifi_task()
|-- motor_task()
|-- sensor_task()


---

# 四、OLED模块说明


文件：
oled.cpp
oled.h


## 功能

负责：

- SSD1315初始化
- OLED显示刷新
- 动画播放
- 后续UI显示


---

# OLED接口函数


## oled_init()

功能：

初始化OLED设备。


执行内容：

- 初始化I2C
- 初始化SSD1315
- 清屏
- 显示启动画面


调用位置：
loop()



---

## oled_clear()

功能：

清空OLED显示。


---

# 五、当前GPIO规划


|功能|GPIO|状态|
|-|-|-|
|OLED SDA|GPIO4|已使用|
|OLED SCL|GPIO5|已使用|
|电磁阀控制|GPIO21|预留|
|震动电机PWM|GPIO22|预留|
|DS18B20温度|GPIO23|预留|


注意：

GPIO可能根据硬件布局调整。


---

# 六、当前完成情况


## V0.1 OLED阶段


已完成：

✅ ESP32-S3 PlatformIO环境搭建

✅ ESP32-S3 N16R8 Flash识别

✅ 8MB PSRAM识别

✅ SSD1315 OLED通信

✅ U8g2驱动

✅ BMP转XBMP工具

✅ OLED动画显示

✅ OLED代码模块化


---

# 七、后续开发计划


## V0.2 WiFi通信模块

计划：

新增：


wifi.cpp
wifi.h



功能：

- WiFi连接
- 自动重连
- 网络状态显示


---

## V0.3 巴法云通信


新增：


bemfa.cpp
bemfa.h



功能：

- MQTT连接
- 手机远程控制
- 状态上传


---

## V0.4 执行机构


新增：


motor.cpp
motor.h

feeder.cpp
feeder.h



功能：

- 步进电机控制
- 定量出粮
- 卡粮检测


---

# 八、开发记录


## 2026-07

完成：

- ESP32-S3基础环境
- OLED SSD1315显示
- 动画显示框架


---

# 九、开发注意事项


1. src目录只保存当前有效代码。

2. 历史测试代码放入backup目录。

3. 不允许多个cpp文件定义：


setup()
loop()


4. 每个硬件模块独立：


xxx.cpp
xxx.h


5. main.cpp只负责系统调度。


---

# 当前版本

Guo Feeder Project

Version: V0.2


已完成：

[√] ESP32-S3 PlatformIO环境

[√] SSD1315 OLED驱动

[√] OLED动画显示

[√] LittleFS文件系统

[√] config.json配置管理

[√] WiFi基础连接


开发中：

[ ] WiFi非阻塞状态机

[ ] WiFi自动重连

[ ] NTP时间管理


未开始：

[ ] HX711重量模块

[ ] 电磁阀控制

[ ] 定量出水状态机

[ ] 巴法云通信

[ ] 手机配置页面

[ ] FreeRTOS任务化




WiFi模块开发规划文档

文件建议：

docs/wifi_module_plan.md
WiFi模块功能规划
1. 当前目标

WiFi模块负责ESP32-S3设备的网络通信能力。

主要任务：

连接用户家庭WiFi
保持网络稳定
为云端通信提供基础
为手机配置、远程控制提供网络入口

当前版本仅完成：

WiFi STA模式连接
基础连接状态检测
从config.json读取WiFi账号密码

后续逐步扩展为完整WiFi管理系统。

2. WiFi模块规划功能列表
2.1 基础WiFi连接（已完成）

功能：

ESP32启动后读取config.json
获取WiFi SSID
获取WiFi Password
自动连接路由器
输出IP地址

涉及文件：

wifi_module.cpp
wifi_module.h
config_manager.cpp
config.json
2.2 非阻塞WiFi状态机（下一步优先开发）

当前问题：

现有WiFi连接方式：

while(WiFi.status()!=WL_CONNECTED)
{
    delay(500);
}

存在问题：

WiFi连接期间阻塞程序流程
后续OLED、电机、传感器任务无法及时运行
不利于未来FreeRTOS任务迁移

目标：

改为非阻塞状态机。

计划状态：

enum WifiState
{
    WIFI_IDLE,

    WIFI_CONNECTING,

    WIFI_CONNECTED,

    WIFI_DISCONNECTED,

    WIFI_AP_MODE
};

实现：

由：

wifi_task()

循环执行：

检查状态
更新状态
执行动作
立即返回

要求：

禁止长时间while等待。

2.3 自动断线重连

目标：

设备长期运行过程中：

例如：

路由器重启
WiFi信号异常
网络临时中断

ESP32自动：

检测断开
 ↓
等待
 ↓
重新连接
 ↓
恢复通信

需要记录：

断开时间
重连次数
当前状态
2.4 WiFi异常原因分析

增加WiFi错误报告。

支持识别：

状态	说明
WL_NO_SSID_AVAIL	找不到WiFi
WL_CONNECT_FAILED	密码错误
WL_CONNECTION_LOST	连接丢失
WL_DISCONNECTED	主动断开

用途：

OLED显示：

WiFi:
密码错误

云端上传：

{
 "wifi_status":"error",
 "reason":"password"
}
2.5 WiFi历史账号保存

目标：

防止config.json错误导致设备无法连接。

方案：

使用ESP32 NVS：

库：

Preferences.h

保存：

wifi_ssid
wifi_password
last_success_time

逻辑：

启动：

读取config.json

↓

连接失败

↓

尝试NVS历史WiFi

↓

仍失败

↓

进入AP配置模式
2.6 AP热点配置模式

目标：

用户无需连接电脑即可配置设备。

ESP32开启热点：

例如：

GuoFeeder_Setup

手机连接后：

访问：

192.168.4.1

网页配置：

参数：

WiFi名称
WiFi密码
OLED参数
电机参数
电磁阀参数

保存：

config.json

设备重启。

新增模块：

web_config.cpp
web_config.h
2.7 NTP网络时间同步

目标：

获取真实时间。

用途：

未来：

喂食计划
日志记录
定时任务
云端数据

新增：

time_manager.cpp
time_manager.h

提供：

get_time()

get_date()

get_timestamp()
2.8 巴法云通信支持

目标：

实现：

云端状态上传
手机远程控制
参数修改

可能协议：

MQTT

新增：

mqtt_manager.cpp
mqtt_manager.h

功能：

上传：

WiFi状态
设备状态
粮仓状态
电机状态

接收：

立即喂食
修改参数
查询状态
WiFi模块最终结构规划

未来：

src

├── wifi_module.cpp
├── wifi_module.h
│
├── time_manager.cpp
├── time_manager.h
│
├── mqtt_manager.cpp
├── mqtt_manager.h
│
├── web_config.cpp
├── web_config.h
│
└── config_manager.cpp
    config_manager.h


---

# time_manager


职责：
统一管理系统时间


时间来源：

当前：
NTP


未来：
RTC


原则：

NTP负责校准
RTC负责长期运行


同步策略：

首次WiFi连接：
立即同步


WiFi重新连接：
重新同步


长期运行：
每7天自动同步一次


禁止：
循环高频访问NTP


# system_state 系统运行状态管理模块

## 1. 模块作用

system_state模块用于保存ESP32设备运行期间的公共状态数据。

该模块是整个软件系统的数据共享中心。

其他模块之间禁止直接互相访问内部变量，应通过system_state提供的接口获取或修改状态。

例如：

WiFi模块：
    更新网络连接状态

time_manager：
    更新系统时间状态

RTC模块：
    更新RTC可用状态

OLED：
    读取状态并显示

云端模块：
    读取状态并上传


---

# 2. 设计原则


## 2.1 与config_manager区别

system_state：

保存：
    运行状态


特点：

- RAM保存
- 断电丢失
- 实时变化


例如：


WiFi是否连接
当前时间
当前错误
设备运行状态




config_manager：

保存：

用户配置


特点：

- LittleFS保存
- 断电保持
- 用户修改


例如：


WiFi名称
WiFi密码
目标出水量
最大开阀时间
OLED参数




二者不能混用。


---

# 3. 文件结构



src

/system

system_state.h

system_state.cpp


---

# 4. 当前管理的数据


## 4.1 WiFi状态


变量：


wifi_status



作用：

表示当前网络状态。


来源：

wifi_module


调用：

OLED显示：


WiFi OK



云端上传：


wifi:true




接口：


```cpp
system_set_wifi_status(bool status)

system_get_wifi_status()

4.2 系统时间

变量：

system_time

类型：

time_t

作用：

保存当前系统时间。

当前来源：

NTP

未来来源：

RTC

接口：

system_set_time(time_t timestamp)

system_get_time()

4.3 时间有效状态

变量：

time_valid

作用：

判断系统时间是否可信。

例如：

设备刚启动：

time_valid=false

NTP同步成功：

time_valid=true

接口：

system_is_time_valid()

4.4 NTP同步状态

变量：

ntp_synced

作用：

记录是否完成网络校时。

流程：

WiFi连接

↓

NTP同步

↓

ntp_synced=true


接口：

system_set_ntp_synced()

system_is_ntp_synced()

4.5 最近一次NTP同步时间

变量：

last_ntp_sync

作用：

用于自动校时周期判断。

例如：

设置：

7天自动校时一次

判断：

当前时间-last_ntp_sync

>

7天


接口：

system_set_last_ntp_sync()

system_get_last_ntp_sync()

4.6 RTC状态

变量：

rtc_available

作用：

记录RTC硬件是否存在。

当前：

false

原因：

RTC模块尚未安装。

未来：

启动检测：

发现DS3231

↓

rtc_available=true


接口：

system_set_rtc_available()

system_is_rtc_available()

4.7 错误代码

变量：

error_code

作用：

保存当前系统错误。

例如：

E001 HX711异常

E002 重量突增

E003 出水异常

E004 WiFi异常


接口：

system_set_error_code()

system_get_error_code()


未来由：

error_manager模块统一管理。

4.8 系统运行状态

枚举：

SystemRunState

当前状态：

SYSTEM_IDLE

SYSTEM_RUNNING

SYSTEM_ERROR


用途：

表示设备整体状态。

未来扩展：

WATERING

FEEDING

CALIBRATION

OTA_UPDATE


接口：

system_set_run_state()

system_get_run_state()

5. 数据流关系
                 config_manager

                      |
                      |
              用户配置参数


                     


wifi_module
     |
     |
     v

system_state

     ^
     |
time_manager

     ^
     |
rtc_manager



system_state

     |
     |
 --------------------------
 |            |            |
OLED       Cloud       Control

6. 当前版本限制

当前：

无RTC硬件
时间依赖NTP
状态仅保存在RAM

设备断电：

system_state全部清空

属于正常设计。

7. 未来扩展方向
RTC接入

增加：

rtc_manager

启动流程：

开机

↓

读取RTC

↓

恢复系统时间

↓

WiFi连接

↓

NTP校准

↓

写入RTC

状态持久化

如果未来需要：

断电恢复：

最近一次运行状态
错误记录
维护日志

增加：

storage_manager

负责：

LittleFS
NVS
Preferences

不修改system_state。

8. 开发原则
模块之间禁止直接访问变量

错误：

wifi.cpp
直接修改oled变量

正确：

wifi.cpp

↓

system_state

↓

oled.cpp

system_state不包含业务逻辑

例如：

错误：

WiFi断开自动重连

属于wifi_module。

错误：

NTP同步

属于time_manager。

system_state只负责：

保存状态
提供接口
当前开发状态

完成：

✅ system_state架构设计

待开发：

⬜ time_manager

⬜ rtc_manager

⬜ error_manager

⬜ water_control


---

## 后续修改顺序建议

按照你现在工程状态，我建议：

### 第一步（下一步）
修改 `time_manager`

目标：

- 引入 `wifi_is_connected()`
- 引入 `system_state`
- 去掉5秒NTP轮询
- 改为状态机
- 预留RTC接口


### 第二步
微调 `wifi_module`

增加：

- WiFi连接成功事件
- 通知time_manager同步


### 第三步
修改OLED

只读取：

```cpp
system_state

不直接读取WiFi、时间模块。

这样最终架构：

main.cpp
   |
   |
各模块task


wifi_module
time_manager
oled
hx711
water_control
rtc


        ↓

 system_state

        ↓

共享状态