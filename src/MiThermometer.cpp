#include "MiThermometer.h"
#include "config_manager.h"
#include "system_state.h"
#include "cloud_manager.h"
#include "NimBLEDevice.h"
#include <time.h>
#include <NimBLEDevice.h>
#include <mbedtls/ccm.h>
#include <cstring>
#include <math.h>
#include <WiFi.h>
#include <Arduino.h>

QueueHandle_t xRawAdvQueue = nullptr;
static uint8_t g_bin_bindkey[16] = {0};

// 模块静态缓存配置，仅MiThermometerInit()中从config读取一次
static char s_target_mac[24] = {0};
static char s_bindkey_str[33] = {0};

static NimBLEScan* pBLEScan = nullptr;


// =============================
// MiThermo 扫描状态机
// =============================
enum MiThermoScanState
{
    MI_THERMO_BOOT_DELAY,
    MI_THERMO_SLEEP,
    MI_THERMO_START_SCAN,
    MI_THERMO_SCANNING
};
static MiThermoScanState s_scan_state = MI_THERMO_BOOT_DELAY;
static uint32_t s_boot_time = 0;
static uint32_t s_scan_start_time = 0;
static uint32_t s_sleep_start_time = 0;
static constexpr uint32_t MI_THERMO_SLEEP_MS = 20UL * 60UL * 1000UL;// 扫描周期
static constexpr uint32_t MI_THERMO_SCAN_TIMEOUT_MS =60000UL;// 单次扫描最长时间
static uint8_t s_scan_fail_count = 0;// 连续失败次数
static constexpr uint8_t MI_THERMO_MAX_FAIL = 10;
static bool s_disabled_by_fault = false;// 故障锁定


struct RawAdvItem
{
    uint8_t mac[6];
    uint8_t payload[31];
    uint8_t payload_len;
};
class MiAdvCallback : public NimBLEScanCallbacks
{
    void onResult(
        const NimBLEAdvertisedDevice* advertisedDevice) override
    {

        static uint32_t ble_count = 0;
        ble_count++;


        std::string mac =
            advertisedDevice->getAddress().toString();


        Serial.printf(
            "\n[BLE] #%lu MAC=%s\n",
            ble_count,
            mac.c_str()
        );


        std::vector<uint8_t> payload =
            advertisedDevice->getPayload();


        size_t advLen = payload.size();


        Serial.printf(
            "[BLE] payload len=%d : ",
            advLen
        );


        for(size_t i=0;i<advLen;i++)
        {
            Serial.printf(
                "%02X ",
                payload[i]
            );
        }

        Serial.println();


        if(!xRawAdvQueue)
            return;


        if(advLen < 12)
            return;


        RawAdvItem item{};


        /*
            MAC字符串转换为6字节
        */
        sscanf(
            mac.c_str(),
            "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
            &item.mac[0],
            &item.mac[1],
            &item.mac[2],
            &item.mac[3],
            &item.mac[4],
            &item.mac[5]
        );


        item.payload_len =
            advLen > sizeof(item.payload)
            ?
            sizeof(item.payload)
            :
            advLen;


        memcpy(
            item.payload,
            payload.data(),
            item.payload_len
        );


        /*
            所有设备全部进入解密队列
        */
        if(
            xQueueSend(
                xRawAdvQueue,
                &item,
                0
            )
            != pdTRUE
        )
        {
            Serial.println(
                "[BLE] queue full"
            );
        }
    }
};
static MiAdvCallback s_advCallback;


static bool lywsd03_decrypt(const uint8_t mac[6],
                            const uint8_t bindkey[16],
                            const uint8_t* advData, size_t advLen,
                            float &out_temp, float &out_hum, float &out_bat);

bool MiThermometerInit(void)
{
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
        return false;
    }


    state_set_float(STATE_MI_THERMO_TEMP, NAN);
    state_set_float(STATE_MI_THERMO_HUMID, NAN);
    state_set_float(STATE_MI_THERMO_BAT_V, NAN);
    state_set_long(STATE_MI_THERMO_TS, 0L);
    state_set_bool(STATE_MI_THERMO_VALID, false);
    s_boot_time = millis();
    s_scan_state = MI_THERMO_BOOT_DELAY;
    s_scan_fail_count = 0;
    s_disabled_by_fault = false;

    pBLEScan = NimBLEDevice::getScan();   
    pBLEScan->setScanCallbacks(&s_advCallback);
    pBLEScan->setActiveScan(false);
    pBLEScan->setInterval(100);
    pBLEScan->setWindow(80);
    Serial.println("[MiThermo] init OK");
    return true;
}


void MiThermometer_task()
{
    if(xRawAdvQueue == nullptr)
        return;
    if(!config_get_mi_thermo_enable())
        return;
    if(s_disabled_by_fault)
        return;
    uint32_t now = millis();
    switch(s_scan_state)
    {
        //=========================
        // 开机等待30秒
        //=========================
        case MI_THERMO_BOOT_DELAY:
        {
            if(
                now - s_boot_time
                >
                30000UL
            )
            {
                Serial.println(
                    "[MiThermo] start first scan"
                );
                s_scan_state =
                    MI_THERMO_START_SCAN;
            }
            break;
        }
        //=========================
        // 开始扫描
        //=========================
        case MI_THERMO_START_SCAN:
        {
            pBLEScan->clearResults();
            Serial.println(
                "[BLE] scan begin"
            );
            pBLEScan->start(
                60,
                false
            );
            Serial.println(
                "[MiThermo] scan started"
            );
            s_scan_start_time =
                millis();
            s_scan_state =
                MI_THERMO_SCANNING;
            break;
        }
        //=========================
        // 扫描处理
        //=========================
        case MI_THERMO_SCANNING:
        {
            RawAdvItem rawItem;
            while(
                xQueueReceive(
                    xRawAdvQueue,
                    &rawItem,
                    0U
                )
                ==
                pdTRUE
            )
            {
                Serial.println(
                    "[MiThermo] try decrypt"
                );
                Serial.printf(
                    "[MiThermo] mac=%02X:%02X:%02X:%02X:%02X:%02X\n",
                    rawItem.mac[0],
                    rawItem.mac[1],
                    rawItem.mac[2],
                    rawItem.mac[3],
                    rawItem.mac[4],
                    rawItem.mac[5]
                );
                float temp;
                float hum;
                float bat;
                bool ok =
                    lywsd03_decrypt(
                        rawItem.mac,
                        g_bin_bindkey,
                        rawItem.payload,
                        rawItem.payload_len,
                        temp,
                        hum,
                        bat
                    );
                if(ok)
                {
                    Serial.println(
                        "[MiThermo] decrypt SUCCESS"
                    );
                    Serial.printf(
                        "TEMP %.2f HUM %.2f BAT %.2f\n",
                        temp,
                        hum,
                        bat
                    );
                    state_set_float(
                        STATE_MI_THERMO_TEMP,
                        temp
                    );
                    state_set_float(
                        STATE_MI_THERMO_HUMID,
                        hum
                    );
                    state_set_float(
                        STATE_MI_THERMO_BAT_V,
                        bat
                    );
                    state_set_long(
                        STATE_MI_THERMO_TS,
                        time(nullptr)
                    );
                    state_set_bool(
                        STATE_MI_THERMO_VALID,
                        true
                    );
                    s_scan_fail_count = 0;
                    pBLEScan->stop();
                    s_sleep_start_time =
                        now;
                    s_scan_state =
                        MI_THERMO_SLEEP;
                    return;
                }
                else
                {
                    Serial.println(
                        "[MiThermo] decrypt failed"
                    );
                }
            }
            //扫描超时
            if(
                now - s_scan_start_time
                >
                MI_THERMO_SCAN_TIMEOUT_MS
            )
            {
                Serial.println(
                    "[MiThermo] scan timeout"
                );
                pBLEScan->stop();
                s_scan_fail_count++;
                Serial.printf(
                    "[MiThermo] fail count=%d\n",
                    s_scan_fail_count
                );
                if(
                    s_scan_fail_count
                    >=
                    MI_THERMO_MAX_FAIL
                )
                {
                    s_disabled_by_fault=true;
                    Serial.println(
                        "[MiThermo] disabled by fault"
                    );
                    return;
                }
                s_sleep_start_time =
                    now;
                s_scan_state =
                    MI_THERMO_SLEEP;
            }
            break;
        }
        //=========================
        // 睡眠20分钟
        //=========================
        case MI_THERMO_SLEEP:
        {
            if(
                now - s_sleep_start_time
                >
                MI_THERMO_SLEEP_MS
            )
            {
                Serial.println(
                    "[MiThermo] wake scan"
                );
                s_scan_state =
                    MI_THERMO_START_SCAN;
            }
            break;
        }
    }
}

static bool lywsd03_decrypt(const uint8_t mac[6],
                            const uint8_t bindkey[16],
                            const uint8_t* advData, size_t advLen,
                            float &out_temp, float &out_hum, float &out_bat)
{
    Serial.printf(
        "[DEC] advLen=%d\n",
        advLen
        );
    
    Serial.printf(
        "[Decrypt] MAC=%02X:%02X:%02X:%02X:%02X:%02X len=%d\n",
        mac[0],
        mac[1],
        mac[2],
        mac[3],
        mac[4],
        mac[5],
        advLen
    );
    size_t offset = 0;
    bool found = false;
    while(offset + 5 < advLen)
    {
        uint8_t len = advData[offset];
        if(len == 0) break;
        uint8_t type = advData[offset+1];
        if(type == 0xFF)
        {
            Serial.println("[DEC] found manufacturer");
            if(advData[offset+2]==0x02 && advData[offset+3]==0x10)
            {
                offset +=4;
                found = true;
                break;
            }
        }
        offset += (len+1);
    }
    if(!found) return false;

    size_t payloadRemain = advLen - offset;
    if(payloadRemain <13) return false;

    const uint8_t* pMic = advData + offset + 11;
    uint8_t mic[4];
    memcpy(mic, pMic,4);

    uint8_t nonce[13];
    memcpy(nonce, mac,6);
    memcpy(nonce+6, advData+offset,7);

    uint8_t cipherText[11];
    memcpy(cipherText, advData+offset+6, 11);

    mbedtls_ccm_context ccmCtx;
    mbedtls_ccm_init(&ccmCtx);

    int ret = mbedtls_ccm_setkey(&ccmCtx, MBEDTLS_CIPHER_ID_AES, bindkey, 128);
    if(ret != 0)
    {
        mbedtls_ccm_free(&ccmCtx);
        return false;
    }

    uint8_t plain[11] = {0};

    ret = mbedtls_ccm_auth_decrypt(
        &ccmCtx,
        sizeof(cipherText),   // 参数2：密文长度 11字节
        nonce,                // 参数3：nonce(iv)指针
        sizeof(nonce),        // 参数4：nonce长度 13字节
        nullptr,              // 参数5：ad指针
        0U,                   // 参数6：ad_len
        cipherText,           // 参数7：input密文指针
        plain,                // 参数8：output明文输出
        mic,                  // 参数9：tag指针(mic)
        4U                    // 参数10：tag_len
    );


    mbedtls_ccm_free(&ccmCtx);

    if(ret !=0)
    {
        return false;
    }

    uint16_t tempRaw = (plain[1] << 8) | plain[0];
    int16_t  humRaw  = (int16_t)((plain[3] << 8) | plain[2]);
    uint8_t battRaw = plain[4];

    out_temp = static_cast<int16_t>(tempRaw) / 100.0f;
    out_hum  = humRaw / 100.0f;
    out_bat  = battRaw / 100.0f;


    return true;
}