#include "services/cred_store.h"

#include <Preferences.h>

// =====================================================
// 内部状态
// =====================================================
static Preferences s_prefs;
static bool        s_ready = false;

// NVS 键名（NVS 限制：<= 15 字符）
//   u<N> : username        p<N> : password
//   g<N> : generation      s<N> : state
static const char* kKeyUser[CRED_SLOT_COUNT] = { "u0", "u1" };
static const char* kKeyPass[CRED_SLOT_COUNT] = { "p0", "p1" };
static const char* kKeyGen [CRED_SLOT_COUNT] = { "g0", "g1" };
static const char* kKeySt  [CRED_SLOT_COUNT] = { "s0", "s1" };

static const char* kKeyActive     = "act";    // int8 : -1 / 0 / 1
static const char* kKeyLastGen    = "lgen";   // uint32

static void slot_clear(CredSlot& s)
{
    s.present    = false;
    s.state      = CRED_SLOT_INVALID;
    s.generation = 0;
    s.username[0] = '\0';
    s.password[0] = '\0';
}

void cred_store_init()
{
    if (s_ready)
    {
        return;
    }

    // 读写模式打开；NVS 首次使用会自建 namespace
    s_ready = s_prefs.begin(CRED_NVS_NAMESPACE, false);

    if (!s_ready)
    {
        // 仅报"不可用"，**不**输出任何凭据内容
        Serial.println("[CredStore] NVS open failed (credential feature disabled)");
        return;
    }

    Serial.println("[CredStore] NVS 'gfcred' ready");
}

bool cred_store_ready()
{
    return s_ready;
}

bool cred_store_read(uint8_t slot, CredSlot& out)
{
    slot_clear(out);

    if (!s_ready || slot >= CRED_SLOT_COUNT)
    {
        return false;
    }

    // 先看 state 是否已写入：未写入即视为空槽
    if (!s_prefs.isKey(kKeySt[slot]))
    {
        return false;
    }

    out.state      = s_prefs.getUChar(kKeySt[slot], CRED_SLOT_INVALID);
    out.generation = s_prefs.getULong(kKeyGen[slot], 0);

    String u = s_prefs.getString(kKeyUser[slot], "");
    String p = s_prefs.getString(kKeyPass[slot], "");

    if (u.length() == 0 || p.length() == 0)
    {
        // 半写状态：判为空槽（不覆盖、不崩溃）
        slot_clear(out);
        return false;
    }

    // 有界拷贝（截断而非溢出）
    strncpy(out.username, u.c_str(), CRED_USERNAME_MAX - 1);
    out.username[CRED_USERNAME_MAX - 1] = '\0';
    strncpy(out.password, p.c_str(), CRED_PASSWORD_MAX - 1);
    out.password[CRED_PASSWORD_MAX - 1] = '\0';

    out.present = true;
    return true;
}

bool cred_store_write(uint8_t slot, const CredSlot& in)
{
    if (!s_ready || slot >= CRED_SLOT_COUNT)
    {
        return false;
    }

    if (in.username[0] == '\0' || in.password[0] == '\0')
    {
        return false;
    }

    // 写入顺序：先写内容，最后写 state —— 保证"有 state 即内容完整"
    // （读取侧以 state 存在为判据，规避半写窗口）
    bool ok = true;
    ok = s_prefs.putString(kKeyUser[slot], in.username) && ok;
    ok = s_prefs.putString(kKeyPass[slot], in.password) && ok;
    ok = s_prefs.putULong (kKeyGen [slot], in.generation) && ok;
    ok = s_prefs.putUChar (kKeySt  [slot], in.state) && ok;

    return ok;
}

bool cred_store_erase(uint8_t slot)
{
    if (!s_ready || slot >= CRED_SLOT_COUNT)
    {
        return false;
    }

    s_prefs.remove(kKeyUser[slot]);
    s_prefs.remove(kKeyPass[slot]);
    s_prefs.remove(kKeyGen [slot]);
    s_prefs.remove(kKeySt  [slot]);

    return true;
}

int cred_store_active_slot()
{
    if (!s_ready)
    {
        return -1;
    }

    int v = (int)s_prefs.getChar(kKeyActive, (int8_t)-1);

    if (v < 0 || v >= CRED_SLOT_COUNT)
    {
        return -1;
    }

    return v;
}

bool cred_store_set_active_slot(int slot)
{
    if (!s_ready)
    {
        return false;
    }

    if (slot < 0 || slot >= CRED_SLOT_COUNT)
    {
        return s_prefs.putChar(kKeyActive, (int8_t)-1) == 1;
    }

    return s_prefs.putChar(kKeyActive, (int8_t)slot) == 1;
}

bool cred_store_get_last_generation(uint32_t& gen)
{
    gen = 0;

    if (!s_ready)
    {
        return false;
    }

    gen = s_prefs.getULong(kKeyLastGen, 0);
    return true;
}

bool cred_store_set_last_generation(uint32_t gen)
{
    if (!s_ready)
    {
        return false;
    }

    // putULong 成功时返回写入字节数（4）；0 视为失败
    return s_prefs.putULong(kKeyLastGen, gen) == sizeof(uint32_t);
}

bool cred_store_has_active()
{
    int a = cred_store_active_slot();

    if (a < 0)
    {
        return false;
    }

    CredSlot s;
    return cred_store_read((uint8_t)a, s) && s.state == CRED_SLOT_ACTIVE;
}
