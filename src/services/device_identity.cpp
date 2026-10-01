#include "services/device_identity.h"

#include "esp_mac.h"
#include "Preferences.h"

// =====================================================
// 常量
// =====================================================
static const char    kPrefsNamespace[] = "gfid";   // NVS namespace（身份锚点）
static const char    kKeyDid[]         = "did";    // 身份本体
static const char    kKeyDver[]        = "dver";   // 派生算法版本号
static const uint8_t kDeriveVer        = 1u;       // 本固件内置的派生算法版本
static const size_t  kDeviceIdLen      = 12u;      // 固定 12 个字符（不是"最长 12"）

// =====================================================
// 模块状态（文件作用域 static —— 零裸全局；不进 System State）
// =====================================================
static char    s_device_id[13] = {0};
static uint8_t s_dver          = 0u;
static bool    s_ready         = false;

// =====================================================
// 内部工具
// =====================================================

// 校验：长度恰为 12，且全部为 [0-9a-f]（强制小写）
static bool is_valid_id(const char* id)
{
    if(id == nullptr)
    {
        return false;
    }

    for(size_t i = 0; i < kDeviceIdLen; i++)
    {
        const char c = id[i];

        if(c == '\0')
        {
            return false;
        }

        if(!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
        {
            return false;
        }
    }

    return id[kDeviceIdLen] == '\0';
}

// 从 eFuse 出厂 MAC 派生 device_id（纯函数：输入只有 MAC，无时间 / 随机数 / 其它输入）
static bool derive_from_mac(char out[])
{
    uint8_t mac[6] = {0};

    // 固定取 ESP_MAC_WIFI_STA：STA / AP / 蓝牙三路地址之间存在 ±1 偏移，
    // 换一路就会算出不同的值 => 必须永远使用同一路，否则造成身份漂移
    if(esp_read_mac(mac, ESP_MAC_WIFI_STA) != ESP_OK)
    {
        return false;
    }

    snprintf(out, kDeviceIdLen + 1,
             "%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    return is_valid_id(out);
}

// =====================================================
// 对外接口
// =====================================================

const char* device_id()
{
    return s_device_id;
}

bool device_id_valid()
{
    return s_ready && is_valid_id(s_device_id);
}

void device_identity_init()
{
    char derived[13] = {0};
    const bool mac_ok = derive_from_mac(derived);

    Preferences prefs;
    const bool nvs_open = prefs.begin(kPrefsNamespace, false);

    char    stored[13] = {0};
    uint8_t stored_ver = 0u;
    bool    have_did   = false;

    if(nvs_open)
    {
        if(prefs.getString(kKeyDid, stored, sizeof(stored)) > 0)
        {
            have_did   = true;
            stored_ver = prefs.getUChar(kKeyDver, 0u);
        }
    }

    // ---------- 路径 1：无 did（首启 / NVS 丢失）=> 派生并写入 ----------
    if(!have_did)
    {
        if(!mac_ok)
        {
            // 降级：无身份可用，但不阻塞启动、不重启
            Serial.println("[Identity] ERROR no did in nvs and mac read failed");
            if(nvs_open)
            {
                prefs.end();
            }
            return;
        }

        memcpy(s_device_id, derived, sizeof(s_device_id));
        s_dver  = kDeriveVer;
        s_ready = true;

        if(nvs_open)
        {
            const bool wrote =
                (prefs.putString(kKeyDid, s_device_id) > 0) &&
                (prefs.putUChar(kKeyDver, s_dver) > 0);

            if(!wrote)
            {
                // 降级：本次上电仍可用（内存值有效），下次启动重新尝试
                Serial.println("[Identity] WARN nvs write failed, id kept in ram only");
            }

            prefs.end();
        }
        else
        {
            Serial.println("[Identity] WARN nvs open failed, id kept in ram only");
        }

        Serial.printf("[Identity] new device_id=%s (nvs written)\n", s_device_id);
        return;
    }

    // ---------- 路径 2~4：已有 did => 永远优先（禁止自动覆盖） ----------
    memcpy(s_device_id, stored, sizeof(s_device_id));
    s_dver  = stored_ver;
    s_ready = is_valid_id(s_device_id);

    if(!s_ready)
    {
        // 存量值非法（NVS 被外部改写 / 损坏）：保留原值 + WARN，不覆盖
        Serial.printf("[Identity] WARN nvs did invalid (len=%u), keep as-is\n",
                      (unsigned)strlen(s_device_id));

        if(nvs_open)
        {
            prefs.end();
        }
        return;
    }

    if(s_dver != kDeriveVer)
    {
        // 未知 dver：保留已有 did；不重算、不覆盖、不阻断启动（降级兼容）
        Serial.printf("[Identity] WARN unknown dver=%u, keep id=%s\n",
                      (unsigned)s_dver, s_device_id);
    }
    else if(mac_ok && (strcmp(stored, derived) != 0))
    {
        // MAC 与首次生成时不一致（产线换芯片 / MAC 被改写）：身份仍以 NVS 为准
        Serial.printf("[Identity] WARN mac mismatch, keep nvs id (nvs=%s mac=%s)\n",
                      s_device_id, derived);
    }
    else
    {
        Serial.printf("[Identity] device_id=%s (nvs ok)\n", s_device_id);
    }

    if(nvs_open)
    {
        prefs.end();
    }
}
