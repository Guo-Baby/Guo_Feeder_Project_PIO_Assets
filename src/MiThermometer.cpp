#include "MiThermometer.h"
#include "config_manager.h"
#include "system_state.h"
#include "NimBLEDevice.h"
#include "workflow.h"
#include <time.h>
#include <NimBLEDevice.h>
#include <mbedtls/ccm.h>
#include <cstring>
#include <math.h>
#include <WiFi.h>
#include <strings.h>
#include <Arduino.h>
//前置声明
void mi_thermo_workflow_register();

QueueHandle_t xRawAdvQueue = nullptr;
static uint8_t g_bin_bindkey[16] = {0};

// 模块静态缓存配置，仅MiThermometerInit()中从config读取一次
static char s_target_mac[24] = {0};
static char s_bindkey_str[33] = {0};
static uint8_t s_target_mac_bin[6] = {0};

static NimBLEScan* pBLEScan = nullptr;


// =============================
// MiThermo 扫描状态机
// =============================
enum MiThermoScanState
{
    MI_THERMO_BOOT_DELAY,
    MI_THERMO_SLEEP,    // 长时间休眠
    MI_THERMO_SCAN_WINDOW_RUN,    // 扫描窗口内部运行
    MI_THERMO_SCAN_WINDOW_WAIT,   //获取到温度+湿度，关闭扫描，等待
    MI_THERMO_DISABLED,    //mi_thermo_allow_collect = true 时不执行任何任务
    MI_THERMO_SCAN_ON,    // 当前1秒BLE扫描
    MI_THERMO_SCAN_OFF    // 当前关闭BLE扫描，让WiFi运行

};

// 扫描等级
enum MiThermoLevel
{
    MI_THERMO_LEVEL0,
    MI_THERMO_LEVEL1,
    MI_THERMO_LEVEL2
};
static MiThermoScanState s_scan_state = MI_THERMO_BOOT_DELAY;

static constexpr uint32_t MI_THERMO_SLEEP_MIN_MS = 48UL * 60UL * 1000UL; // 生产环境：48分钟
static constexpr uint32_t MI_THERMO_SLEEP_MAX_MS = 60UL * 60UL * 1000UL;       // 生产环境：60分钟
static constexpr uint32_t MI_THERMO_SCAN_WINDOW_MS = 15UL * 60UL * 1000UL;     // 生产环境：15分钟
static constexpr uint32_t MI_THERMO_SCAN_ON_MS = 1000UL;                        // 每次扫描1秒
static uint32_t s_sleep_start_time = 0; // 休眠开始时间
static uint32_t s_boot_time = 0;
static uint32_t s_sleep_target_ms = 0; // 休眠目标时长
static uint32_t s_scan_window_start_time = 0; // 扫描窗口开始时间

static uint8_t s_scan_fail_count = 0; // 扫描失败统计
// 当前扫描窗口是否已经收到温湿度
static bool s_got_temperature = false;
static bool s_got_humidity = false;
static constexpr uint8_t MI_THERMO_MAX_FAIL = 4; // Level2故障阈值
static bool s_ble_scanning = false; // 当前是否BLE扫描中
static uint32_t s_next_scan_switch_time = 0;
static uint32_t s_scan_on_start_time = 0;
static MiThermoLevel s_thermo_level = MI_THERMO_LEVEL0;

struct RawAdvItem
{
    uint8_t mac[6];               // 广播设备MAC
    uint8_t adv_data[62];         // 完整BLE Advertisement payload，包含所有数据段
    uint8_t adv_len;              // 实际长度
    uint32_t timestamp;           // 接收时间
};

// =====================================================
//捕获蓝牙数据后回调，做mac过滤+空包过滤
// =====================================================
class MiAdvCallback : public NimBLEScanCallbacks
{
public:
    void onResult(const NimBLEAdvertisedDevice* advertisedDevice) override
    {
        if(!xRawAdvQueue) { return; }
        // =====================================================
        // 1. 获取原始MAC（不转换string，不malloc）
        // =====================================================
        const uint8_t* addr = advertisedDevice->getAddress().getVal();
        if(!addr) { return; }
        // NimBLE getVal() 地址顺序: 17:D4:31:38:C1:A4
        // 用户输入: A4:C1:38:31:D4:17，需要反向比较
        if(addr[0] != s_target_mac_bin[5] || addr[1] != s_target_mac_bin[4] ||
        addr[2] != s_target_mac_bin[3] || addr[3] != s_target_mac_bin[2] ||
        addr[4] != s_target_mac_bin[1] || addr[5] != s_target_mac_bin[0]) {return;}
        // =====================================================
        // 2. 获取payload
        // =====================================================
        const std::vector<uint8_t>& payload = advertisedDevice->getPayload();
        if(payload.empty()) { return; }
        // =====================================================
        // 3. 长度过滤
        // =====================================================
        if(payload.size()<29){return;}
        // =====================================================
        // 4. 构造RawAdvItem
        // =====================================================
        RawAdvItem item{};
        memcpy(item.mac, addr, 6);
        size_t len = payload.size();
        if(len > sizeof(item.adv_data)) { len = sizeof(item.adv_data); }
        item.adv_len = static_cast<uint8_t>(len);
        memcpy(item.adv_data, payload.data(), len);
        item.timestamp = millis();
        Serial.printf("[MiThermo] ADV len=%d : ", payload.size());
        
        //debug
        for(size_t i = 0; i < payload.size(); i++) {
            Serial.printf("%02X ", payload[i]);
        }
        Serial.println();
        // =====================================================
        // 5. 入队
        // =====================================================
        xQueueSend(xRawAdvQueue, &item, 0);
        
    }
};


static MiAdvCallback s_advCallback;

//随机休眠函数，避免和温湿度计广播周期锁相。
static uint32_t mi_random_sleep_ms()
{
    return random(
        MI_THERMO_SLEEP_MIN_MS,
        MI_THERMO_SLEEP_MAX_MS
    );
}


//扫描间隔时间随机数，避免和温湿度计广播周期锁相。
static uint32_t mi_random_scan_off_ms()
{
    return random(
        1800UL,
        2301UL
    );
}
//Level 1 增强扫描窗口周期调整
static uint32_t mi_get_scan_window_ms()
{
    switch(s_thermo_level) {
        case MI_THERMO_LEVEL0: return 15UL * 60UL * 1000UL;
        case MI_THERMO_LEVEL1: return 30UL * 60UL * 1000UL;
        default: return 15UL * 60UL * 1000UL;
    }
}

static bool lywsd03_decrypt(
    const uint8_t mac[6],
    const uint8_t bindkey[16],
    const uint8_t* advData,
    size_t advLen,
    float &out_temp,
    uint8_t &out_type
);

//========================
//蓝牙协议栈初始化
//========================
void ble_init(void)
{
    NimBLEDevice::init("");
    Serial.println("[BLE] init OK");
}


//========================
//业务层初始化
//========================
bool MiThermometerInit(void)
{   
    state_set_float(STATE_MI_THERMO_TEMP, NAN);
    state_set_float(STATE_MI_THERMO_HUMID, NAN);
    state_set_float(STATE_MI_THERMO_BAT_V, NAN);

    state_set_long(STATE_MI_THERMO_TEMP_TS, 0L);
    state_set_long(STATE_MI_THERMO_HUMID_TS, 0L);
    state_set_long(STATE_MI_THERMO_BAT_TS, 0L);

    state_set_bool(STATE_MI_THERMO_ENABLE,true);
    state_set_bool(STATE_MI_THERMO_VALID, false);

    bool allow_collect = config_get_mi_thermo_allow_collect();
    if(!allow_collect) {
    Serial.println("[MiThermo] collection disabled by config");
    state_set_bool(STATE_MI_THERMO_ENABLE,false);
    }

    char macBuf[24] = {0};
    char keyBuf[33] = {0};
    config_get_mithermometer_mac(macBuf, sizeof(macBuf));
    config_get_mithermometer_blekey(keyBuf, sizeof(keyBuf));


    if (strlen(macBuf) == 0U || strlen(keyBuf) == 0U)
    {
        Serial.printf("[MiThermo] ERR mac or bindkey empty! mac=\"%s\" key=\"%s\"\n", macBuf, keyBuf);
        return false;
    }

    if(xRawAdvQueue != nullptr)
    {
        return true;
    }

    strncpy(s_target_mac, macBuf, sizeof(s_target_mac) - 1U);
    s_target_mac[sizeof(s_target_mac)-1] = '\0';
    // MAC字符串转换为二进制缓存，callback中禁止再次解析字符串
    sscanf(s_target_mac, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
        &s_target_mac_bin[0], &s_target_mac_bin[1], &s_target_mac_bin[2],
        &s_target_mac_bin[3], &s_target_mac_bin[4], &s_target_mac_bin[5]);

    strncpy(s_bindkey_str, keyBuf, sizeof(s_bindkey_str) -1U);
    s_bindkey_str[sizeof(s_bindkey_str)-1] = '\0';

    //=====新增：初始化一次性把bindkey字符串转为16字节二进制密钥=====
    memset(g_bin_bindkey, 0, sizeof(g_bin_bindkey));
    for(int i = 0; i < 16; i++)
    {
        char buf[3] = {s_bindkey_str[i*2], s_bindkey_str[i*2+1], '\0'};
        g_bin_bindkey[i] = static_cast<uint8_t>(strtol(buf, nullptr, 16));
    }
    //=============================================================

    xRawAdvQueue = xQueueCreate(16, sizeof(RawAdvItem));
    if (!xRawAdvQueue)
    {
        Serial.println("[MiThermo] ERR xQueueCreate failed, no heap memory");
    }

    // BLE温度计启动延迟
    s_scan_window_start_time = millis();

    s_scan_state = MI_THERMO_BOOT_DELAY;

    s_boot_time = millis();

    s_scan_fail_count = 0;
    randomSeed(esp_random());

    pBLEScan = NimBLEDevice::getScan();   
    pBLEScan->setScanCallbacks(&s_advCallback);
    pBLEScan->setActiveScan(false);
    // BLE radio扫描参数
    // interval/window不是我们的3秒占空
    // 这里只控制BLE监听效率，160/160：约100% BLE占空。实际占空由start 和stop 的调用时间调节。
    pBLEScan->setInterval(160);
    pBLEScan->setWindow(160);
    mi_thermo_workflow_register();
    Serial.println("[MiThermo] init OK");
    return true;
}


static void mi_enter_sleep()
{
    s_sleep_start_time = millis();
    s_sleep_target_ms = mi_random_sleep_ms();
    Serial.printf("[MiThermo] enter sleep %u sec\n", s_sleep_target_ms / 1000);
    s_scan_state = MI_THERMO_SLEEP;
}

void MiThermometer_task()
{
    if(xRawAdvQueue == nullptr) {
        Serial.println("[MiThermo] ERR xQueueCreate failed, no heap memory");
        return;
    }

    uint32_t now = millis();
    switch(s_scan_state)
    {
        // =====================================================
        // config中设置allow_collect为false，则不执行任何采集
        // =====================================================        
        case MI_THERMO_DISABLED:
        {
            break;
        }
        // =====================================================
        // 开机等待
        // =====================================================
        case MI_THERMO_BOOT_DELAY:
        {
            if(now - s_boot_time >= 15000UL) {
                Serial.println("[MiThermo] enter scan window");
                s_got_temperature = false;
                s_got_humidity = false;
                s_scan_window_start_time = now;
                s_next_scan_switch_time = now;
                s_scan_state = MI_THERMO_SCAN_WINDOW_RUN;
            }
            break;
        }
        // =====================================================
        // 扫描窗口运行：1秒扫描 -> 2秒关闭，无限循环
        // =====================================================
        case MI_THERMO_SCAN_WINDOW_RUN:
        {
            // 检查扫描窗口是否结束
            if(now - s_scan_window_start_time >= mi_get_scan_window_ms()) {
                if(s_ble_scanning) { pBLEScan->stop(); s_ble_scanning = false; }
                Serial.println("[MiThermo] scan window finished");
                // 判断本轮扫描是否成功（必须同时收到 temperature + humidity）
                if(s_got_temperature && s_got_humidity) {
                    Serial.println("[MiThermo] scan success");
                    s_scan_fail_count = 0;
                    s_thermo_level = MI_THERMO_LEVEL0;
                } else {
                    s_scan_fail_count++;
                    Serial.printf("[MiThermo] scan failed count=%d level=%d\n", s_scan_fail_count, s_thermo_level);
                    if(s_thermo_level == MI_THERMO_LEVEL0) {
                        s_thermo_level = MI_THERMO_LEVEL1;
                        Serial.println("[MiThermo] enter LEVEL1");
                    } else if(s_thermo_level == MI_THERMO_LEVEL1) {
                        if(s_scan_fail_count >= MI_THERMO_MAX_FAIL) {
                            s_thermo_level = MI_THERMO_LEVEL2;
                            if(s_ble_scanning) {
                                pBLEScan->stop();
                                s_ble_scanning = false;
                            }
                            state_set_bool(STATE_MI_THERMO_ENABLE, false);
                            s_scan_state = MI_THERMO_DISABLED;
                            Serial.println("[MiThermo] LEVEL2: scanning disabled");
                        }
                    }
                }
                mi_enter_sleep();
                break;
            }
            // 正常启动1秒扫描
            if(now >= s_next_scan_switch_time) {
                pBLEScan->clearResults();
                pBLEScan->start(0, true);
                s_ble_scanning = true;
                s_scan_on_start_time = now;
                s_scan_state = MI_THERMO_SCAN_ON;
            }
            break;
        }
        // =====================================================
        // 关闭BLE扫描，等待计时结束
        // =====================================================
        case MI_THERMO_SCAN_WINDOW_WAIT:
        {
            if(now - s_scan_window_start_time >= mi_get_scan_window_ms()) {
                Serial.println("[MiThermo] scan window finished");
                s_scan_fail_count = 0;
                s_thermo_level = MI_THERMO_LEVEL0;
                mi_enter_sleep();
            }
            break;
        }
        // =====================================================
        // BLE扫描开启
        // =====================================================
        case MI_THERMO_SCAN_ON:
        {
            if(now - s_scan_on_start_time >= MI_THERMO_SCAN_ON_MS) {
                pBLEScan->stop();
                s_ble_scanning = false; 
                s_next_scan_switch_time = now + mi_random_scan_off_ms();
                s_scan_state = MI_THERMO_SCAN_OFF;
            }
            break;
        }
        // =====================================================
        // BLE关闭等待
        // =====================================================
        case MI_THERMO_SCAN_OFF:
        {
            if(now >= s_next_scan_switch_time) {
                s_scan_state = MI_THERMO_SCAN_WINDOW_RUN;
            }
            break;
        }
        // =====================================================
        // BLE休眠期
        // =====================================================
        case MI_THERMO_SLEEP:
        {
            if(now - s_sleep_start_time >= s_sleep_target_ms) {
                Serial.println("[MiThermo] wakeup from sleep");
                s_got_temperature = false;
                s_got_humidity = false;
                s_scan_window_start_time = now;
                s_next_scan_switch_time = now;
                s_scan_state = MI_THERMO_SCAN_WINDOW_RUN;
            }
            break;
        } 

        default:
        {
            s_scan_state = MI_THERMO_SCAN_WINDOW_RUN;
            break;
        }
    }
    
    // =====================================================
    // 解码队列处理
    // 不在BLE callback里面执行
    // =====================================================
    RawAdvItem rawItem;
    while(xQueueReceive(xRawAdvQueue, &rawItem, 0) == pdTRUE)
    {
        float value = NAN;
        uint8_t data_type = 0;
        bool result = lywsd03_decrypt(rawItem.mac, g_bin_bindkey, rawItem.adv_data, rawItem.adv_len, value, data_type);
        if(result)
        {
            Serial.println("[MiThermo] decrypt OK");
            time_t ts = time(nullptr);
            switch(data_type)
            {
                case 1: // temperature
                    state_set_float(STATE_MI_THERMO_TEMP, value);
                    state_set_long(STATE_MI_THERMO_TEMP_TS, ts);
                    s_got_temperature = true;
                    break;
                case 2: // humidity
                    state_set_float(STATE_MI_THERMO_HUMID, value);
                    state_set_long(STATE_MI_THERMO_HUMID_TS, ts);
                    s_got_humidity = true;
                    break;
                case 3: // battery
                    state_set_float(STATE_MI_THERMO_BAT_V, value);
                    state_set_long(STATE_MI_THERMO_BAT_TS, ts);
                    break;
            }
            if(s_got_temperature && s_got_humidity) {
                state_set_bool(STATE_MI_THERMO_VALID, true);
                // 立即停止BLE扫描，但窗口计时继续
                if(s_ble_scanning) {
                    pBLEScan->stop();
                    s_ble_scanning = false;
                    Serial.println("[MiThermo] temp + humidity received, BLE scan stopped");
                    s_scan_state = MI_THERMO_SCAN_WINDOW_WAIT;
                }
            }
        }
    }
}

//=====================================
//解码函数
//=====================================
static bool lywsd03_decrypt(const uint8_t mac[6], const uint8_t bindkey[16], const uint8_t* advData, size_t advLen, float &out_value, uint8_t &out_type)
{
    out_value = NAN;
    out_type = 0;
    // LYWSD03MMC 加密广播最短长度 29 字节
    if(advLen < 29) { return false; }
    // 固定协议头检查: index 5=0x95, 6=0xFE, 7=0x58, 8=0x58
    if(advData[5] != 0x95 || advData[6] != 0xFE) { return false; }
    if(advData[7] != 0x58 || advData[8] != 0x58) { return false; }
    // FE95作为base，固定偏移
    const size_t base = 5;
    uint8_t type_id[2];
    type_id[0] = advData[base + 4];
    type_id[1] = advData[base + 5];
    uint8_t pid = advData[base + 6];
    // cipher长度动态计算: advLen - base - 13 - 7
    size_t cipher_len = advLen - base - 13 - 7;
    if(cipher_len == 0 || cipher_len > 16) { return false; }
    uint8_t cipher[16] = {0};
    memcpy(cipher, advData + base + 13, cipher_len);
    // counter: 最后7字节前3字节
    uint8_t counter[3];
    memcpy(counter, advData + advLen - 7, 3);
    // mic: 最后4字节
    uint8_t mic[4];
    memcpy(mic, advData + advLen - 4, 4);
    // nonce: MAC(6) + type(2) + pid(1) + counter(3)
    uint8_t nonce[12];
    memcpy(nonce, advData + base + 7, 6);
    memcpy(nonce + 6, type_id, 2);
    nonce[8] = pid;
    memcpy(nonce + 9, counter, 3);
    // AES-CCM 解密
    mbedtls_ccm_context ctx;
    mbedtls_ccm_init(&ctx);
    int ret = mbedtls_ccm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES, bindkey, 128);
    if(ret != 0) { mbedtls_ccm_free(&ctx); return false; }
    uint8_t plain[12] = {0};
    uint8_t aad = 0x11; // LYWSD03MMC 固定AAD
    ret = mbedtls_ccm_auth_decrypt(&ctx, cipher_len, nonce, sizeof(nonce), &aad, 1, cipher, plain, mic, 4);
    mbedtls_ccm_free(&ctx);
    if(ret != 0) { return false; }
    // 明文解析: temp(04 10), humidity(06 10), battery(0A 10)
    switch(plain[0])
    {
        case 0x04:
            out_value = (plain[3] | (plain[4] << 8)) * 0.1f;
            out_type = 1;
            break;
        case 0x06:
            out_value = (plain[3] | (plain[4] << 8)) * 0.1f;
            out_type = 2;
            break;
        case 0x0A:
            out_value = plain[3];
            out_type = 3;
            break;
        default:
            return false;
    }
    Serial.printf("[MiThermo] decode type=%d value=%.2f\n", out_type, out_value);
    return true;
}


//=====================================
//action start scan
//=====================================
bool mi_thermo_start_scan()
{
    if(!config_get_mi_thermo_allow_collect()) {
        Serial.println("[MiThermo] start rejected: collection disabled by config");
        return false;
    }
    bool enable = state_get_bool(STATE_MI_THERMO_ENABLE);
    if(enable) {
        Serial.println("[MiThermo] already enabled");
        return true;
    }
    Serial.println("[MiThermo] start scan");
    state_set_bool(STATE_MI_THERMO_ENABLE, true);
    s_scan_fail_count = 0;
    s_thermo_level = MI_THERMO_LEVEL0;
    s_got_temperature = false;
    s_got_humidity = false;
    s_scan_window_start_time = millis();
    s_next_scan_switch_time = millis();
    s_scan_state = MI_THERMO_BOOT_DELAY;
    return true;
}

//workflow调用入口
static void mi_thermo_start_scan_start(WorkflowActionInstance *action)
{
    if(action == nullptr) { return; }
    action->result = mi_thermo_start_scan() ? ACTION_SUCCESS : ACTION_FAILED;
}

//=====================================
//action stop scan
//=====================================
bool mi_thermo_stop_scan()
{
    bool enable = state_get_bool(STATE_MI_THERMO_ENABLE);
    if(!enable) {
        Serial.println("[MiThermo] already disabled");
        return true;
    }
    Serial.println("[MiThermo] stop scan");
    if(s_ble_scanning) {
        pBLEScan->stop();
        s_ble_scanning = false;
    }
    state_set_bool(STATE_MI_THERMO_ENABLE, false);
    s_scan_state = MI_THERMO_DISABLED;
    return true;
}

//workflow调用入口
static void mi_thermo_stop_scan_start(WorkflowActionInstance *action)
{
    if(action == nullptr) { return; }
    action->result = mi_thermo_stop_scan() ? ACTION_SUCCESS : ACTION_FAILED;
}

//=====================================
//workflow调用：action运行状态清零
//=====================================
static void mi_thermo_action_reset(WorkflowActionInstance *action)
{
    if(action==nullptr)
        return;
    action->result=ACTION_IDLE;
    action->running=false;
    action->runtime=nullptr;
}
//=====================================
//action poll，同步action，此函数做占位
//=====================================
static void mi_thermo_action_poll(WorkflowActionInstance *action)
{
    if(action == nullptr) { return; }
    if(action->result == ACTION_IDLE) {
        action->result = ACTION_FAILED;
    }
}
//=====================================
//空参数，此函数仅占位
//=====================================
static WorkflowParam mi_thermo_action_params[] ={};
//=====================================
//START_SCAN action注册
//=====================================
static WorkflowActionDescriptor mi_thermo_start_scan_desc =
{
    .id = "MI_THERMO_START_SCAN",
    .name = "开启温湿度扫描",
    .module = "mi_thermo",
    .description = "启动米家温湿度计扫描",
    .params = mi_thermo_action_params,
    .param_count = 0,
    .reset = mi_thermo_action_reset,
    .start = mi_thermo_start_scan_start,
    .poll = mi_thermo_action_poll
};
//=====================================
//STOP_SCAN action注册
//=====================================
static WorkflowActionDescriptor mi_thermo_stop_scan_desc =
{
    .id = "MI_THERMO_STOP_SCAN",
    .name = "关闭温湿度扫描",
    .module = "mi_thermo",
    .description = "停止米家温湿度计扫描",
    .params = mi_thermo_action_params,
    .param_count = 0,
    .reset = mi_thermo_action_reset,
    .start = mi_thermo_stop_scan_start,
    .poll = mi_thermo_action_poll
};

//初始化注册action，由init调用
void mi_thermo_workflow_register()
{
    workflow_register_action(
        &mi_thermo_start_scan_desc
    );


    workflow_register_action(
        &mi_thermo_stop_scan_desc
    );
    Serial.println("[MiThermo] register workflow action");
}