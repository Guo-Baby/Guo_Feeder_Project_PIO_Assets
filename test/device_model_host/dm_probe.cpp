// =====================================================
// Device Model —— wasm32 宿主探针（无硬件、无 CRT 真执行）
//
// 用途：在宿主机上**真实运行** Device Model 的纯逻辑，输出运行期 PASS/FAIL 数字。
// 编译（见 run.sh）：
//   clang++ --target=wasm32 -std=gnu++17 -nostdlib -fno-builtin -O1 \
//           -I <shim> -I src  dm_probe.cpp src/homie/device_model.cpp \
//           -Wl,--no-entry -Wl,--export-all -o probe.wasm
//
// 注：本探针只依赖 <stdint.h>（freestanding）；被测模块同样 freestanding
//     （仅 <stdint.h> + system_state.h 的 key 枚举）。
// =====================================================

#include "homie/device_model.h"

// ---- freestanding 运行期支撑（避免 libcall） ----
extern "C" void* memcpy(void* dst, const void* src, unsigned long n)
{
    unsigned char* d = (unsigned char*)dst;
    const unsigned char* s = (const unsigned char*)src;
    for (unsigned long i = 0; i < n; ++i) d[i] = s[i];
    return dst;
}
extern "C" void* memset(void* dst, int c, unsigned long n)
{
    unsigned char* d = (unsigned char*)dst;
    for (unsigned long i = 0; i < n; ++i) d[i] = (unsigned char)c;
    return dst;
}
extern "C" int memcmp(const void* a, const void* b, unsigned long n)
{
    const unsigned char* x = (const unsigned char*)a;
    const unsigned char* y = (const unsigned char*)b;
    for (unsigned long i = 0; i < n; ++i)
    {
        if (x[i] != y[i]) return (int)x[i] - (int)y[i];
    }
    return 0;
}

// ---- 结果记录 ----
static char g_log[32768];
static int  g_log_len = 0;
static int  g_pass = 0;
static int  g_fail = 0;

static void put(const char* s)
{
    while (*s != '\0' && g_log_len < (int)sizeof(g_log) - 1)
        g_log[g_log_len++] = *s++;
}

static void put_u32(unsigned int v)
{
    char b[12];
    int n = 0;
    if (v == 0) { b[n++] = '0'; }
    while (v > 0) { b[n++] = (char)('0' + (v % 10)); v /= 10; }
    for (int i = n - 1; i >= 0; --i)
    {
        if (g_log_len < (int)sizeof(g_log) - 1) g_log[g_log_len++] = b[i];
    }
}

static void section(const char* name)
{
    put("\n-- "); put(name); put("\n");
}

static void check(bool ok, const char* name)
{
    if (ok) { ++g_pass; put("  [PASS] "); }
    else    { ++g_fail; put("  [FAIL] "); }
    put(name); put("\n");
}

static bool str_eq(const char* a, const char* b)
{
    if (a == nullptr || b == nullptr) return false;
    while (*a != '\0' && *b != '\0') { if (*a != *b) return false; ++a; ++b; }
    return *a == '\0' && *b == '\0';
}

static bool id_ok(const char* s)
{
    if (s == nullptr || *s == '\0') return false;
    if (s[0] == '$') return false;
    for (const char* p = s; *p != '\0'; ++p)
    {
        const char c = *p;
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || (c == '-'))) return false;
    }
    return true;
}

// =====================================================
// 探针主体
// =====================================================
extern "C" int probe_run()
{
    g_log_len = 0; g_pass = 0; g_fail = 0;
    put("Device Model runtime probe\n");

    // ---- T1 Model enumeration ----
    section("T1 model enumeration");
    check(dm_node_count() == 4, "node count == 4");
    check(dm_property_count() == 16, "property count == 16");

    const char* expect_props[][2] = {
        {"water", "valve"}, {"water", "weight"}, {"water", "weight-error"}, {"water", "zero"},
        {"climate", "temperature"}, {"climate", "humidity"}, {"climate", "battery"},
        {"climate", "scan-enabled"}, {"climate", "scan"},
        {"system", "mqtt-connected"}, {"system", "wifi-rssi"}, {"system", "wifi-signal"},
        {"system", "time-valid"}, {"system", "event-queue"}, {"system", "mqtt-retries"},
        {"control", "computer-reset"},
    };
    for (unsigned i = 0; i < sizeof(expect_props) / sizeof(expect_props[0]); ++i)
    {
        check(dm_find(expect_props[i][0], expect_props[i][1]) != nullptr, expect_props[i][1]);
    }

    bool ids_all_ok = true;
    bool no_slash = true;
    bool no_dollar = true;
    for (uint8_t i = 0; i < dm_property_count(); ++i)
    {
        const DeviceModelProperty* p = dm_property_at(i);
        if (!id_ok(p->id) || !id_ok(p->node)) ids_all_ok = false;
        for (const char* q = p->id; *q; ++q) if (*q == '/') no_slash = false;
        for (const char* q = p->node; *q; ++q) if (*q == '/') no_slash = false;
        if (p->id[0] == '$' || p->node[0] == '$') no_dollar = false;
    }
    for (uint8_t i = 0; i < dm_node_count(); ++i)
    {
        if (!id_ok(dm_node_at(i)->id)) ids_all_ok = false;
    }
    check(ids_all_ok, "all node/property ids match [a-z0-9-]");
    check(no_slash, "no id contains '/'");
    check(no_dollar, "no id leads with '$'");
    check(dm_node_at(0) != nullptr && dm_node_at(3) != nullptr, "node index 0..3 valid");
    check(dm_node_at(4) == nullptr, "node index 4 == nullptr");
    check(dm_property_at(15) != nullptr, "property index 15 valid");
    check(dm_property_at(16) == nullptr, "property index 16 == nullptr");

    // ---- T2 Lookup ----
    section("T2 lookup");
    check(dm_find("water", "valve") != nullptr, "find water/valve");
    check(dm_find("water", "weight") != nullptr, "find water/weight");
    check(dm_find("nope", "valve") == nullptr, "unknown node fails");
    check(dm_find("water", "nope") == nullptr, "unknown property fails");
    check(dm_find(nullptr, "valve") == nullptr, "null node fails");
    check(dm_find("water", nullptr) == nullptr, "null property fails");
    check(dm_find("water", "Valve") == nullptr, "lookup is case-sensitive");

    // ---- T3 Valve ----
    section("T3 valve");
    {
        const DeviceModelProperty* v = dm_find("water", "valve");
        const DeviceModelCommand* cmd = nullptr;
        check(v != nullptr && v->dtype == DM_ENUM, "valve dtype enum");
        check(v != nullptr && str_eq(v->format, "open,close"), "valve format open,close");
        check(dm_resolve_command(v, "open", &cmd) && cmd && str_eq(cmd->action_id, "VALVE_OPEN"), "open -> VALVE_OPEN");
        check(dm_resolve_command(v, "close", &cmd) && cmd && str_eq(cmd->action_id, "VALVE_CLOSE"), "close -> VALVE_CLOSE");
        check(!dm_resolve_command(v, "OPEN", &cmd) && cmd == nullptr, "OPEN rejected");
        check(!dm_resolve_command(v, "foo", &cmd) && cmd == nullptr, "foo rejected");
        check(dm_validate_set(v, "OPEN") == DM_VALID_ENUM_NOT_FOUND, "validate(OPEN) == ENUM_NOT_FOUND");
        check(dm_validate_set(v, "open") == DM_VALID_OK, "validate(open) == OK");
        // 客户端无法覆盖 action_id：返回指针必须落在属性自己的表中
        const DeviceModelCommand* c2 = nullptr;
        dm_resolve_command(v, "open", &c2);
        check(c2 >= v->cmds && c2 < v->cmds + v->cmd_count, "resolved cmd lives in property table");
    }

    // ---- T4 Readonly rejection ----
    section("T4 readonly rejection");
    check(dm_validate_set(dm_find("water", "weight"), "5") == DM_VALID_NOT_SETTABLE, "weight not settable");
    {
        const DeviceModelCommand* cmd = nullptr;
        check(!dm_resolve_command(dm_find("water", "weight"), "5", &cmd) && cmd == nullptr, "weight resolve fails");
        check(!dm_resolve_command(dm_find("climate", "temperature"), "20", &cmd) && cmd == nullptr, "temperature resolve fails");
        check(!dm_resolve_command(dm_find("system", "wifi-rssi"), "1", &cmd) && cmd == nullptr, "wifi-rssi resolve fails");
        check(!dm_resolve_command(dm_find("system", "wifi-signal"), "2", &cmd) && cmd == nullptr, "wifi-signal resolve fails");
    }

    // ---- T5 Type / range ----
    section("T5 type / range");
    {
        const DeviceModelProperty* z = dm_find("water", "zero");
        const DeviceModelCommand* cmd = nullptr;
        check(dm_resolve_command(z, "true", &cmd) && cmd && str_eq(cmd->action_id, "WEIGHT_ZERO"), "zero true -> WEIGHT_ZERO");
        check(!dm_resolve_command(z, "1", &cmd) && cmd == nullptr, "zero '1' rejected");
        check(!dm_resolve_command(z, "True", &cmd) && cmd == nullptr, "zero 'True' rejected");

        const DeviceModelProperty* s = dm_find("climate", "scan");
        check(dm_resolve_command(s, "start", &cmd) && cmd && str_eq(cmd->action_id, "MI_THERMO_START_SCAN"), "scan start");
        check(dm_resolve_command(s, "stop", &cmd) && cmd && str_eq(cmd->action_id, "MI_THERMO_STOP_SCAN"), "scan stop");
        check(!dm_resolve_command(s, "pause", &cmd) && cmd == nullptr, "scan pause rejected");

        const DeviceModelProperty* cr = dm_find("control", "computer-reset");
        check(dm_resolve_command(cr, "true", &cmd) && cmd && str_eq(cmd->action_id, "COMPUTER_RESET"), "computer-reset true");
        check(!dm_resolve_command(cr, "false", &cmd) && cmd == nullptr, "computer-reset false rejected (no mapping)");

        // 校验器 int/float 分支：局部夹具（非模型数据）
        const DeviceModelProperty intProp   = { "t", "t-int",   "t", DM_INT,   "0:10:1",  "",
                                                true, false, 0.0f, 0, DM_SRC_NONE, 0, nullptr, 0, nullptr, 0 };
        const DeviceModelProperty floatProp = { "t", "t-float", "t", DM_FLOAT, "0:100:1", "",
                                                true, false, 0.0f, 0, DM_SRC_NONE, 0, nullptr, 0, nullptr, 0 };
        const DeviceModelProperty strProp   = { "t", "t-str",   "t", DM_STRING, "",       "",
                                                true, false, 0.0f, 0, DM_SRC_NONE, 0, nullptr, 0, nullptr, 0 };
        const DeviceModelProperty jsonProp  = { "t", "t-json",  "t", DM_JSON,  "",        "",
                                                true, false, 0.0f, 0, DM_SRC_NONE, 0, nullptr, 0, nullptr, 0 };
        check(dm_validate_set(&intProp, "5") == DM_VALID_OK, "int in range OK");
        check(dm_validate_set(&intProp, "0") == DM_VALID_OK, "int at min OK");
        check(dm_validate_set(&intProp, "10") == DM_VALID_OK, "int at max OK");
        check(dm_validate_set(&intProp, "11") == DM_VALID_OUT_OF_RANGE, "int above max rejected");
        check(dm_validate_set(&intProp, "-1") == DM_VALID_OUT_OF_RANGE, "int below min rejected");
        check(dm_validate_set(&intProp, "abc") == DM_VALID_BAD_DATATYPE, "int non-numeric rejected");
        check(dm_validate_set(&intProp, "4.2") == DM_VALID_BAD_DATATYPE, "int decimal rejected");
        check(dm_validate_set(&floatProp, "50") == DM_VALID_OK, "float in range OK");
        check(dm_validate_set(&floatProp, "100") == DM_VALID_OK, "float at max OK");
        check(dm_validate_set(&floatProp, "100.5") == DM_VALID_OUT_OF_RANGE, "float above max rejected");
        check(dm_validate_set(&floatProp, "-0.5") == DM_VALID_OUT_OF_RANGE, "float below min rejected");
        check(dm_validate_set(&floatProp, "") == DM_VALID_BAD_DATATYPE, "float empty rejected");
        check(dm_validate_set(&floatProp, "nan") == DM_VALID_BAD_DATATYPE, "float nan rejected");
        check(dm_validate_set(&floatProp, "inf") == DM_VALID_BAD_DATATYPE, "float inf rejected");
        check(dm_validate_set(&strProp, "anything") == DM_VALID_OK, "string accepted");
        check(dm_validate_set(&jsonProp, "{}") == DM_VALID_BAD_DATATYPE, "json not writable in P2");
        check(dm_validate_set(nullptr, "x") == DM_VALID_UNKNOWN_PROPERTY, "null property rejected");
        check(dm_validate_set(&intProp, nullptr) == DM_VALID_BAD_DATATYPE, "null value rejected");
    }

    // ---- T6 Weight ----
    section("T6 weight metadata");
    {
        const DeviceModelProperty* w = dm_find("water", "weight");
        check(w != nullptr && str_eq(w->format, "0:10000:1"), "format 0:10000:1");
        check(w != nullptr && w->deadband == 1.0f, "deadband 1.0");
        check(w != nullptr && w->settable == false, "readonly");
        check(w != nullptr && str_eq(w->unit, "g"), "unit g");
        check(w != nullptr && w->dtype == DM_FLOAT, "datatype float");
        check(w != nullptr && w->src_kind == DM_SRC_STATE && w->state_key == (uint16_t)STATE_WEIGHT_VALUE, "state source");
    }

    // ---- T7 Retained metadata ----
    section("T7 retained metadata");
    check(dm_find("water", "valve")->retained == true, "valve retained true");
    check(dm_find("water", "weight")->retained == true, "weight retained true");
    check(dm_find("water", "weight-error")->retained == true, "weight-error retained true");
    check(dm_find("water", "zero")->retained == false, "zero retained false");
    check(dm_find("climate", "scan")->retained == false, "scan retained false");
    check(dm_find("control", "computer-reset")->retained == false, "computer-reset retained false");
    check(dm_find("system", "mqtt-connected")->retained == true, "mqtt-connected retained true");
    check(dm_find("climate", "temperature")->retained == true, "temperature retained true");

    // ---- T9 Enum read map ----
    section("T9 enum read map");
    {
        const DeviceModelProperty* v = dm_find("water", "valve");
        check(v->enum_state_map_count == 2, "valve map size 2");
        check(v->enum_state_map[0].state_value == 1 && str_eq(v->enum_state_map[0].member, "open"), "1 -> open");
        check(v->enum_state_map[1].state_value == 0 && str_eq(v->enum_state_map[1].member, "close"), "0 -> close");
        const DeviceModelProperty* ws = dm_find("system", "wifi-signal");
        check(ws->enum_state_map_count == 5, "wifi-signal map size 5");
        check(ws->enum_state_map[4].state_value == 4 && str_eq(ws->enum_state_map[4].member, "4"), "4 -> \"4\"");
    }

    // ---- T10 Command mapping integrity ----
    section("T10 command mapping integrity");
    check(dm_find("water", "valve")->cmd_count == 2, "valve cmd count 2");
    check(dm_find("water", "zero")->cmd_count == 1, "zero cmd count 1");
    check(dm_find("climate", "scan")->cmd_count == 2, "scan cmd count 2");
    check(dm_find("control", "computer-reset")->cmd_count == 1, "computer-reset cmd count 1");
    {
        bool settable_ok = true, readonly_ok = true;
        for (uint8_t i = 0; i < dm_property_count(); ++i)
        {
            const DeviceModelProperty* p = dm_property_at(i);
            if (p->settable && (p->cmds == nullptr || p->cmd_count == 0)) settable_ok = false;
            if (!p->settable && p->cmd_count != 0) readonly_ok = false;
        }
        check(settable_ok, "every settable property maps >=1 command");
        check(readonly_ok, "readonly property carries no command");
    }

    put("\nSUMMARY pass=");
    put_u32((unsigned)g_pass);
    put(" fail=");
    put_u32((unsigned)g_fail);
    put("\n");
    return g_fail;
}

extern "C" const char* probe_log_ptr() { return g_log; }
extern "C" int         probe_log_len() { return g_log_len; }
