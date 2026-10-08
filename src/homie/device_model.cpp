// =====================================================
// Device Model —— 实现（P2-1）
//
// 上位冻结设计：docs/architecture/P2-0-architecture-review-and-design-freeze.md
//   §4.3（Model ↔ Registry 关系）· §5.3（结构契约）
//   §12.2/§12.5（投影规则 + 显式映射）· §13（Datatype / Validation）
//
// 存储与开销（FROZEN §5.3 / §17.2）：
//   · 全部描述表为 static constexpr ⇒ .rodata（Flash）；DRAM 静态占用 = 0
//   · 无动态分配 / 无可变全局状态 / 无句柄 / 无缓存 / 无运行期注册 API
//   · 全部 dm_*() 为「纯函数（入参 → 查表 → 返回）」，且为 constexpr
//   · 不依赖 libc（不 include <string.h>/<stdlib.h>/<math.h>）⇒ 自包含、
//     可在编译期求值（见文末静态测试套件）
//
// ★ 本文件不 include 任何 MQTT / CloudManager / CommandManager / EventManager /
//   Workflow / app（硬件）头文件；也不调用任何 Action（只「描述」映射）。
// =====================================================

#include "homie/device_model.h"

// ---- 编译期能力探测 ----
// 固件工具链（ESP32 Arduino / GCC，-std=gnu++11）**不支持** C++14 宽松 constexpr
// （C++11 的 constexpr 函数体只能是单条 return）⇒ 核心函数在固件侧编译为普通
// 静态函数；在支持 C++14 宽松 constexpr 的构建（宿主 wasm 探针，-std=gnu++17）
// 下自动升级为 constexpr，并启用文末的编译期测试套件。
#if defined(__cpp_constexpr) && (__cpp_constexpr >= 201304L)
#  define DM_CONSTEXPR constexpr
#  define DM_HAS_RELAXED_CONSTEXPR 1
#else
#  define DM_CONSTEXPR
#  define DM_HAS_RELAXED_CONSTEXPR 0
#endif

// =====================================================
// 命令映射表（显式；禁止字符串推导 —— FROZEN §12.5）
// =====================================================
static constexpr DeviceModelCommand kCmdsValve[] = {
    { "open",  "VALVE_OPEN",  "{}" },
    { "close", "VALVE_CLOSE", "{}" },
};

static constexpr DeviceModelCommand kCmdsZero[] = {
    { "true", "WEIGHT_ZERO", "{}" },
};

static constexpr DeviceModelCommand kCmdsScan[] = {
    { "start", "MI_THERMO_START_SCAN", "{}" },
    { "stop",  "MI_THERMO_STOP_SCAN",  "{}" },
};

static constexpr DeviceModelCommand kCmdsComputerReset[] = {
    { "true", "COMPUTER_RESET", "{}" },
};

// =====================================================
// 只读枚举映射表（显式；禁止 runtime_id 推导 —— FROZEN §12.5）
// =====================================================
// water/valve 读侧：STATE_VALVE_STATUS == true → "open"；false → "close"
static constexpr DeviceModelEnumMap kEnumValve[] = {
    { 1, "open"  },
    { 0, "close" },
};

// system/wifi-signal 读侧：整数等级 0..4 → 成员串
static constexpr DeviceModelEnumMap kEnumWifiSignal[] = {
    { 0, "0" },
    { 1, "1" },
    { 2, "2" },
    { 3, "3" },
    { 4, "4" },
};

// =====================================================
// Node 表（FROZEN §5.2 / §12.2）
// =====================================================
static constexpr DeviceModelNode kNodes[] = {
    { "water",   "供水", "guo-feeder/water"   },
    { "climate", "环境", "guo-feeder/climate" },
    { "system",  "系统", "guo-feeder/system"  },
    { "control", "控制", "guo-feeder/control" },
};

// =====================================================
// Property 表（字段顺序必须与 DeviceModelProperty 一致 —— FROZEN §5.3）
//   node,id,name,dtype,format,unit,settable,retained,deadband,min_pub_ms,
//   src_kind,state_key,enum_state_map,enum_state_map_count,cmds,cmd_count
//
// ★ 这是「设备语义」的唯一静态来源；Capability Registry 不会、也不能改写它。
//   weight.enable=false 时这些 property 依然存在于此表；是否投影由未来
//   Projection 的 boot-time Registry availability filter 决定（§4.3/§15.3）。
// =====================================================
static constexpr DeviceModelProperty kProperties[] = {
    // ---------- water ----------
    { "water", "valve", "电磁阀", DM_ENUM, "open,close", "",
      true, true, 0.0f, 0,
      DM_SRC_STATE, (uint16_t)STATE_VALVE_STATUS, kEnumValve, 2, kCmdsValve, 2 },

    { "water", "weight", "重量", DM_FLOAT, "0:10000:1", "g",
      false, true, 1.0f, 1000,
      DM_SRC_STATE, (uint16_t)STATE_WEIGHT_VALUE, nullptr, 0, nullptr, 0 },

    { "water", "weight-error", "称重异常", DM_BOOL, "", "",
      false, true, 0.0f, 0,
      DM_SRC_STATE, (uint16_t)STATE_WEIGHT_ERROR, nullptr, 0, nullptr, 0 },

    { "water", "zero", "零点校准", DM_BOOL, "", "",
      true, false, 0.0f, 0,
      DM_SRC_NONE, 0, nullptr, 0, kCmdsZero, 1 },

    // ---------- climate ----------
    { "climate", "temperature", "温度", DM_FLOAT, "", "°C",
      false, true, 0.0f, 0,
      DM_SRC_STATE, (uint16_t)STATE_MI_THERMO_TEMP, nullptr, 0, nullptr, 0 },

    { "climate", "humidity", "湿度", DM_FLOAT, "0:100", "%",
      false, true, 0.0f, 0,
      DM_SRC_STATE, (uint16_t)STATE_MI_THERMO_HUMID, nullptr, 0, nullptr, 0 },

    { "climate", "battery", "电池电压", DM_FLOAT, "", "V",
      false, true, 0.0f, 0,
      DM_SRC_STATE, (uint16_t)STATE_MI_THERMO_BAT_V, nullptr, 0, nullptr, 0 },

    { "climate", "scan-enabled", "扫描已启用", DM_BOOL, "", "",
      false, true, 0.0f, 0,
      DM_SRC_STATE, (uint16_t)STATE_MI_THERMO_ENABLE, nullptr, 0, nullptr, 0 },

    { "climate", "scan", "扫描", DM_ENUM, "start,stop", "",
      true, false, 0.0f, 0,
      DM_SRC_NONE, 0, nullptr, 0, kCmdsScan, 2 },

    // ---------- system ----------
    { "system", "mqtt-connected", "MQTT 已连接", DM_BOOL, "", "",
      false, true, 0.0f, 0,
      DM_SRC_STATE, (uint16_t)STATE_MQTT_STATUS, nullptr, 0, nullptr, 0 },

    { "system", "wifi-rssi", "WiFi 信号强度", DM_INT, "", "dBm",
      false, true, 0.0f, 0,
      DM_SRC_STATE, (uint16_t)STATE_WIFI_RSSI, nullptr, 0, nullptr, 0 },

    { "system", "wifi-signal", "WiFi 信号等级", DM_ENUM, "0,1,2,3,4", "",
      false, true, 0.0f, 0,
      DM_SRC_STATE, (uint16_t)STATE_WIFI_SIGNAL, kEnumWifiSignal, 5, nullptr, 0 },

    { "system", "time-valid", "时间有效", DM_BOOL, "", "",
      false, true, 0.0f, 0,
      DM_SRC_STATE, (uint16_t)STATE_TIME_VALID, nullptr, 0, nullptr, 0 },

    { "system", "event-queue", "事件队列", DM_INT, "", "",
      false, true, 0.0f, 0,
      DM_SRC_STATE, (uint16_t)STATE_EVENT_QUEUE_COUNT, nullptr, 0, nullptr, 0 },

    { "system", "mqtt-retries", "MQTT 重试次数", DM_INT, "", "",
      false, true, 0.0f, 0,
      DM_SRC_STATE, (uint16_t)STATE_MQTT_RETRY_COUNT, nullptr, 0, nullptr, 0 },

    // ---------- control ----------
    { "control", "computer-reset", "电脑重启", DM_BOOL, "", "",
      true, false, 0.0f, 0,
      DM_SRC_NONE, 0, nullptr, 0, kCmdsComputerReset, 1 },
};

static constexpr uint8_t kNodeCount     = (uint8_t)(sizeof(kNodes) / sizeof(kNodes[0]));
static constexpr uint8_t kPropertyCount = (uint8_t)(sizeof(kProperties) / sizeof(kProperties[0]));

static_assert(kNodeCount == 4,     "P2 frozen Device Model must declare exactly 4 nodes");
static_assert(kPropertyCount == 16, "P2 frozen Device Model must declare exactly 16 properties");

// =====================================================
// 编译期字符串 / 数值工具（无 libc 依赖）
// =====================================================
static DM_CONSTEXPR uint32_t cstr_len(const char* s)
{
    uint32_t n = 0;
    if (s == nullptr)
        return 0;
    while (s[n] != '\0')
        ++n;
    return n;
}

static DM_CONSTEXPR bool cstr_eq(const char* a, const char* b)
{
    if (a == nullptr || b == nullptr)
        return false;
    for (uint32_t i = 0;; ++i)
    {
        if (a[i] != b[i])
            return false;
        if (a[i] == '\0')
            return true;
    }
}

static DM_CONSTEXPR const char* cstr_chr(const char* s, char c)
{
    if (s == nullptr)
        return nullptr;
    for (const char* p = s; *p != '\0'; ++p)
    {
        if (*p == c)
            return p;
    }
    return nullptr;
}

// 布尔：严格接受 "true" / "false"（大小写敏感、不 trim —— FROZEN §13.3）
static DM_CONSTEXPR bool dm_parse_bool(const char* v, bool* out)
{
    if (v == nullptr)
        return false;
    if (cstr_eq(v, "true"))  { *out = true;  return true; }
    if (cstr_eq(v, "false")) { *out = false; return true; }
    return false;
}

// 整数：可选符号 + 十进制数字；整体必须耗尽（拒绝 "1.0" / "1x" / ""）
static DM_CONSTEXPR bool dm_parse_int_strict(const char* v, long* out)
{
    if (v == nullptr)
        return false;
    const char* p = v;
    bool neg = false;
    if (*p == '+' || *p == '-') { neg = (*p == '-'); ++p; }
    if (*p < '0' || *p > '9')
        return false;
    long acc = 0;
    while (*p >= '0' && *p <= '9')
    {
        if (acc > 100000000L)     // 防溢出（远超本工程取值域）
            return false;
        acc = acc * 10 + (long)(*p - '0');
        ++p;
    }
    if (*p != '\0')
        return false;
    *out = neg ? -acc : acc;
    return true;
}

// 浮点：可选符号 + 十进制数字 [. 数字]；整体必须耗尽
// 刻意不接受 inf/nan/十六进制/指数（严格解析 ⇒ 拒绝异常载荷）
static DM_CONSTEXPR bool dm_parse_float_strict(const char* v, float* out)
{
    if (v == nullptr)
        return false;
    const char* p = v;
    bool neg = false;
    if (*p == '+' || *p == '-') { neg = (*p == '-'); ++p; }

    bool any = false;
    double acc = 0.0;
    while (*p >= '0' && *p <= '9') { acc = acc * 10.0 + (double)(*p - '0'); any = true; ++p; }
    if (*p == '.')
    {
        ++p;
        double scale = 0.1;
        while (*p >= '0' && *p <= '9') { acc += (double)(*p - '0') * scale; scale *= 0.1; any = true; ++p; }
    }
    if (!any)
        return false;
    if (*p != '\0')
        return false;
    *out = (float)(neg ? -acc : acc);
    return true;
}

// 解析 format 的一段数值（[s, end) 必须整体为合法十进制数）
static DM_CONSTEXPR bool dm_parse_num_segment(const char* s, const char* end, float* out)
{
    if (s == nullptr || end == nullptr || s >= end)
        return false;
    char buf[24] = {0};
    int32_t n = 0;
    for (const char* p = s; p < end; ++p)
    {
        if (n >= (int32_t)(sizeof(buf) - 1))
            return false;
        buf[n++] = *p;
    }
    buf[n] = '\0';
    return dm_parse_float_strict(buf, out);
}

// format 数值范围：支持 "min:max" / "min:max:step" / "min:" / ":max"
// 非范围格式（无 ':'，如 enum 成员表）⇒ 恒通过
static DM_CONSTEXPR bool dm_range_check(const char* fmt, float v)
{
    if (fmt == nullptr || fmt[0] == '\0')
        return true;
    const char* c1 = cstr_chr(fmt, ':');
    if (c1 == nullptr)
        return true;
    const char* c2 = cstr_chr(c1 + 1, ':');
    const char* max_end = (c2 != nullptr) ? c2 : (fmt + cstr_len(fmt));

    float mn = 0.0f;
    float mx = 0.0f;
    const bool has_min = dm_parse_num_segment(fmt, c1, &mn);
    const bool has_max = dm_parse_num_segment(c1 + 1, max_end, &mx);

    if (has_min && v < mn)
        return false;
    if (has_max && v > mx)
        return false;
    return true;
}

// enum 成员精确匹配（大小写敏感、不 trim —— FROZEN §13.3）
static DM_CONSTEXPR bool dm_enum_member(const char* fmt, const char* value)
{
    if (fmt == nullptr || value == nullptr)
        return false;
    const uint32_t vlen = cstr_len(value);
    const char* p = fmt;
    while (*p != '\0')
    {
        const char* comma = cstr_chr(p, ',');
        const uint32_t len = (comma != nullptr) ? (uint32_t)(comma - p) : cstr_len(p);
        if (len == vlen)
        {
            bool same = true;
            for (uint32_t i = 0; i < len; ++i)
            {
                if (p[i] != value[i]) { same = false; break; }
            }
            if (same)
                return true;
        }
        if (comma == nullptr)
            break;
        p = comma + 1;
    }
    return false;
}

// =====================================================
// 内部 constexpr 核心（单一真源）
//   · 编译期：被文末静态测试套件求值
//   · 运行期：被下面的公开 API 薄包装调用
// =====================================================
static DM_CONSTEXPR uint8_t dm_node_count_core() { return kNodeCount; }
static DM_CONSTEXPR uint8_t dm_property_count_core() { return kPropertyCount; }

static DM_CONSTEXPR const DeviceModelNode* dm_node_at_core(uint8_t index)
{
    return (index < kNodeCount) ? &kNodes[index] : nullptr;
}
static DM_CONSTEXPR const DeviceModelProperty* dm_property_at_core(uint8_t index)
{
    return (index < kPropertyCount) ? &kProperties[index] : nullptr;
}

static DM_CONSTEXPR const DeviceModelProperty* dm_find_core(const char* node_id, const char* property_id)
{
    if (node_id == nullptr || property_id == nullptr)
        return nullptr;
    for (uint8_t i = 0; i < kPropertyCount; ++i)
    {
        if (cstr_eq(kProperties[i].node, node_id) && cstr_eq(kProperties[i].id, property_id))
            return &kProperties[i];
    }
    return nullptr;
}

// ---- 协议无关形状校验（FROZEN §13.2 第①层）----
static DM_CONSTEXPR DeviceModelValidateResult dm_validate_core(const DeviceModelProperty* prop, const char* value)
{
    if (prop == nullptr)
        return DM_VALID_UNKNOWN_PROPERTY;
    if (!prop->settable)
        return DM_VALID_NOT_SETTABLE;
    if (value == nullptr)
        return DM_VALID_BAD_DATATYPE;

    switch (prop->dtype)
    {
        case DM_BOOL:
        {
            bool b = false;
            return dm_parse_bool(value, &b) ? DM_VALID_OK : DM_VALID_BAD_DATATYPE;
        }
        case DM_INT:
        {
            long x = 0;
            if (!dm_parse_int_strict(value, &x))
                return DM_VALID_BAD_DATATYPE;
            return dm_range_check(prop->format, (float)x) ? DM_VALID_OK : DM_VALID_OUT_OF_RANGE;
        }
        case DM_FLOAT:
        {
            float f = 0.0f;
            if (!dm_parse_float_strict(value, &f))
                return DM_VALID_BAD_DATATYPE;
            return dm_range_check(prop->format, f) ? DM_VALID_OK : DM_VALID_OUT_OF_RANGE;
        }
        case DM_ENUM:
        {
            return dm_enum_member(prop->format, value) ? DM_VALID_OK : DM_VALID_ENUM_NOT_FOUND;
        }
        case DM_STRING:
        {
            return DM_VALID_OK;
        }
        case DM_JSON:
        default:
        {
            // P2 不使用 json 属性（FROZEN §13.1）⇒ 不可写
            return DM_VALID_BAD_DATATYPE;
        }
    }
    return DM_VALID_BAD_DATATYPE;   // 不可达（switch 已穷尽）；仅为满足 -Wreturn-type
}

// ---- 可写面解析（仅查显式映射表；不调用 CommandManager）----
static DM_CONSTEXPR bool dm_resolve_core(const DeviceModelProperty* prop,
                                      const char* value,
                                      const DeviceModelCommand** out_cmd)
{
    if (out_cmd != nullptr)
        *out_cmd = nullptr;

    if (dm_validate_core(prop, value) != DM_VALID_OK)
        return false;

    if (prop->cmds == nullptr || prop->cmd_count == 0)
        return false;

    for (uint8_t i = 0; i < prop->cmd_count; ++i)
    {
        if (cstr_eq(prop->cmds[i].value, value))
        {
            if (out_cmd != nullptr)
                *out_cmd = &prop->cmds[i];
            return true;
        }
    }
    return false;
}

// =====================================================
// 公开 API（FROZEN §5.3 签名；薄包装，无额外状态）
// =====================================================
uint8_t dm_node_count() { return dm_node_count_core(); }
const DeviceModelNode* dm_node_at(uint8_t index) { return dm_node_at_core(index); }
uint8_t dm_property_count() { return dm_property_count_core(); }
const DeviceModelProperty* dm_property_at(uint8_t index) { return dm_property_at_core(index); }
const DeviceModelProperty* dm_find(const char* node_id, const char* property_id)
{
    return dm_find_core(node_id, property_id);
}

DeviceModelValidateResult dm_validate_set(const DeviceModelProperty* prop, const char* value)
{
    return dm_validate_core(prop, value);
}

bool dm_resolve_command(const DeviceModelProperty* prop,
                        const char* value,
                        const DeviceModelCommand** out_cmd)
{
    return dm_resolve_core(prop, value, out_cmd);
}

// =====================================================
// =====================================================
//  ★ 编译期静态测试套件（P2-1 验证）
//
//  这些断言在编译期求值 —— 编译通过 ⇔ 全部测试 PASS。
//  不产生任何运行期代码 / 数据 / 开销。
//  分区对应任务 §十二 的验证类别。
// =====================================================
// =====================================================

#if DM_HAS_RELAXED_CONSTEXPR   // 仅在支持 C++14 宽松 constexpr 的构建中启用

// property / node id 必须满足 [a-z0-9-]，不以 $ 开头，不含 '/'（FROZEN §5.4）
// （仅静态测试套件使用 ⇒ 不参与固件构建，避免 unused-static 噪声）
static DM_CONSTEXPR bool id_charset_ok(const char* s)
{
    if (s == nullptr || s[0] == '\0')
        return false;
    if (s[0] == '$')
        return false;
    for (const char* p = s; *p != '\0'; ++p)
    {
        const char c = *p;
        const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || (c == '-');
        if (!ok)
            return false;
    }
    return true;
}

// ---- 表结构聚合谓词 ----
static DM_CONSTEXPR bool t_all_ids_ok()
{
    for (uint8_t i = 0; i < kPropertyCount; ++i)
    {
        if (!id_charset_ok(kProperties[i].id) || !id_charset_ok(kProperties[i].node))
            return false;
    }
    for (uint8_t i = 0; i < kNodeCount; ++i)
    {
        if (!id_charset_ok(kNodes[i].id))
            return false;
    }
    return true;
}

static DM_CONSTEXPR bool t_no_slash_or_dollar()
{
    for (uint8_t i = 0; i < kPropertyCount; ++i)
    {
        if (cstr_chr(kProperties[i].id, '/') != nullptr)   return false;
        if (cstr_chr(kProperties[i].node, '/') != nullptr) return false;
        if (kProperties[i].id[0] == '$')                   return false;
        if (kProperties[i].node[0] == '$')                 return false;
    }
    for (uint8_t i = 0; i < kNodeCount; ++i)
    {
        if (cstr_chr(kNodes[i].id, '/') != nullptr) return false;
        if (kNodes[i].id[0] == '$')                 return false;
    }
    return true;
}

static DM_CONSTEXPR bool t_settable_have_cmds()
{
    for (uint8_t i = 0; i < kPropertyCount; ++i)
    {
        if (kProperties[i].settable && (kProperties[i].cmds == nullptr || kProperties[i].cmd_count == 0))
            return false;
    }
    return true;
}

static DM_CONSTEXPR bool t_readonly_no_cmds()
{
    for (uint8_t i = 0; i < kPropertyCount; ++i)
    {
        if (!kProperties[i].settable && kProperties[i].cmd_count != 0)
            return false;
    }
    return true;
}

static DM_CONSTEXPR bool t_enum_formats_nonempty()
{
    for (uint8_t i = 0; i < kPropertyCount; ++i)
    {
        if (kProperties[i].dtype == DM_ENUM &&
            (kProperties[i].format == nullptr || kProperties[i].format[0] == '\0'))
            return false;
    }
    return true;
}

static DM_CONSTEXPR bool t_state_keys_in_range()
{
    for (uint8_t i = 0; i < kPropertyCount; ++i)
    {
        if (kProperties[i].src_kind == DM_SRC_STATE && kProperties[i].state_key >= (uint16_t)STATE_MAX)
            return false;
        if (kProperties[i].src_kind == DM_SRC_NONE && kProperties[i].state_key != 0)
            return false;
    }
    return true;
}

// 解析/范围工具的直接单元验证
static DM_CONSTEXPR bool t_parse_helpers()
{
    long iv = 0;
    float fv = 0.0f;
    bool bv = false;
    if (!dm_parse_int_strict("42", &iv) || iv != 42)            return false;
    if (dm_parse_int_strict("4.2", &iv))                        return false;   // 整数不接受小数
    if (dm_parse_int_strict("", &iv))                           return false;
    if (dm_parse_int_strict("12x", &iv))                        return false;
    if (!dm_parse_float_strict("22.5", &fv) || fv < 22.49f || fv > 22.51f) return false;
    if (dm_parse_float_strict("nan", &fv))                      return false;
    if (dm_parse_float_strict("inf", &fv))                      return false;
    if (!dm_parse_bool("true", &bv) || bv != true)              return false;
    if (!dm_parse_bool("false", &bv) || bv != false)            return false;
    if (dm_parse_bool("True", &bv))                             return false;
    if (dm_range_check("0:10", 0.0f) != true)                   return false;
    if (dm_range_check("0:10", 10.0f) != true)                  return false;
    if (dm_range_check("0:10", 10.5f) != false)                 return false;
    if (dm_range_check("0:10", -0.5f) != false)                 return false;
    if (dm_range_check("open,close", 99.0f) != true)            return false;   // 非范围格式
    if (!dm_enum_member("open,close", "close"))                 return false;
    if (dm_enum_member("open,close", "OPEN"))                   return false;
    return true;
}

// 校验器分支覆盖：使用**局部**描述符（单元测试夹具，非模型数据）
static DM_CONSTEXPR bool t_validator_int_float_branches()
{
    constexpr DeviceModelProperty kIntProp   = { "t", "t-int",   "t", DM_INT,   "0:10:1",  "",
                                                 true, false, 0.0f, 0, DM_SRC_NONE, 0, nullptr, 0, nullptr, 0 };
    constexpr DeviceModelProperty kFloatProp = { "t", "t-float", "t", DM_FLOAT, "0:100:1", "",
                                                 true, false, 0.0f, 0, DM_SRC_NONE, 0, nullptr, 0, nullptr, 0 };
    constexpr DeviceModelProperty kJsonProp  = { "t", "t-json",  "t", DM_JSON,  "",        "",
                                                 true, false, 0.0f, 0, DM_SRC_NONE, 0, nullptr, 0, nullptr, 0 };
    if (dm_validate_core(&kIntProp, "5")        != DM_VALID_OK)             return false;
    if (dm_validate_core(&kIntProp, "11")       != DM_VALID_OUT_OF_RANGE)   return false;
    if (dm_validate_core(&kIntProp, "abc")      != DM_VALID_BAD_DATATYPE)   return false;
    if (dm_validate_core(&kFloatProp, "50")     != DM_VALID_OK)             return false;
    if (dm_validate_core(&kFloatProp, "100.5")  != DM_VALID_OUT_OF_RANGE)   return false;
    if (dm_validate_core(&kFloatProp, "")       != DM_VALID_BAD_DATATYPE)   return false;
    if (dm_validate_core(&kJsonProp, "{}")      != DM_VALID_BAD_DATATYPE)   return false;
    if (dm_validate_core(nullptr, "x")          != DM_VALID_UNKNOWN_PROPERTY) return false;
    if (dm_validate_core(&kIntProp, nullptr)    != DM_VALID_BAD_DATATYPE)   return false;
    return true;
}

// 命令解析结果断言辅助
static DM_CONSTEXPR bool t_resolve(const char* node, const char* id, const char* value, const char* expect_action)
{
    const DeviceModelCommand* cmd = nullptr;
    if (!dm_resolve_core(dm_find_core(node, id), value, &cmd))
        return false;
    return (cmd != nullptr) && cstr_eq(cmd->action_id, expect_action);
}

static DM_CONSTEXPR bool t_reject(const char* node, const char* id, const char* value)
{
    const DeviceModelCommand* cmd = nullptr;
    if (dm_resolve_core(dm_find_core(node, id), value, &cmd))
        return false;
    return cmd == nullptr;
}

// 解析返回的命令指针必须落在该属性自己的 cmds 数组内（客户端无法提供 action_id）
static DM_CONSTEXPR bool t_cmd_ptr_within_table()
{
    const DeviceModelProperty* p = dm_find_core("water", "valve");
    const DeviceModelCommand* cmd = nullptr;
    if (!dm_resolve_core(p, "open", &cmd) || cmd == nullptr)
        return false;
    return (cmd >= p->cmds) && (cmd < p->cmds + p->cmd_count);
}

// =====================================================
// 断言分区
// =====================================================

// ---- §T1 Model enumeration ----
static_assert(dm_node_count_core() == 4,                     "T1: node count must be 4");
static_assert(dm_property_count_core() == 16,                "T1: property count must be 16");
static_assert(dm_find_core("water",   "valve")          != nullptr, "T1: water/valve missing");
static_assert(dm_find_core("water",   "weight")         != nullptr, "T1: water/weight missing");
static_assert(dm_find_core("water",   "weight-error")   != nullptr, "T1: water/weight-error missing");
static_assert(dm_find_core("water",   "zero")           != nullptr, "T1: water/zero missing");
static_assert(dm_find_core("climate", "temperature")    != nullptr, "T1: climate/temperature missing");
static_assert(dm_find_core("climate", "humidity")       != nullptr, "T1: climate/humidity missing");
static_assert(dm_find_core("climate", "battery")        != nullptr, "T1: climate/battery missing");
static_assert(dm_find_core("climate", "scan-enabled")   != nullptr, "T1: climate/scan-enabled missing");
static_assert(dm_find_core("climate", "scan")           != nullptr, "T1: climate/scan missing");
static_assert(dm_find_core("system",  "mqtt-connected") != nullptr, "T1: system/mqtt-connected missing");
static_assert(dm_find_core("system",  "wifi-rssi")      != nullptr, "T1: system/wifi-rssi missing");
static_assert(dm_find_core("system",  "wifi-signal")    != nullptr, "T1: system/wifi-signal missing");
static_assert(dm_find_core("system",  "time-valid")     != nullptr, "T1: system/time-valid missing");
static_assert(dm_find_core("system",  "event-queue")    != nullptr, "T1: system/event-queue missing");
static_assert(dm_find_core("system",  "mqtt-retries")   != nullptr, "T1: system/mqtt-retries missing");
static_assert(dm_find_core("control", "computer-reset") != nullptr, "T1: control/computer-reset missing");
static_assert(t_all_ids_ok(),                            "T1: id charset [a-z0-9-] violated");
static_assert(t_no_slash_or_dollar(),                    "T1: id must not contain '/' or lead with '$'");
static_assert(dm_node_at_core(0) != nullptr && dm_node_at_core(3) != nullptr, "T1: nodes 0..3 must exist");
static_assert(dm_node_at_core(4) == nullptr,                  "T1: node out-of-range must be nullptr");
static_assert(dm_property_at_core(15) != nullptr,             "T1: property 15 must exist");
static_assert(dm_property_at_core(16) == nullptr,             "T1: property out-of-range must be nullptr");

// ---- §T2 Lookup ----
static_assert(dm_find_core("water", "valve") != nullptr,      "T2: dm_find water/valve");
static_assert(dm_find_core("water", "weight") != nullptr,     "T2: dm_find water/weight");
static_assert(dm_find_core("nope",  "valve") == nullptr,      "T2: unknown node must fail");
static_assert(dm_find_core("water", "nope")  == nullptr,      "T2: unknown property must fail");
static_assert(dm_find_core(nullptr, "valve") == nullptr,      "T2: null node must fail");
static_assert(dm_find_core("water", nullptr) == nullptr,      "T2: null property must fail");
static_assert(dm_find_core("water", "Valve") == nullptr,      "T2: lookup must be case-sensitive");

// ---- §T3 Valve ----
static_assert(dm_find_core("water", "valve")->dtype == DM_ENUM,     "T3: valve must be enum");
static_assert(cstr_eq(dm_find_core("water", "valve")->format, "open,close"), "T3: valve format");
static_assert(dm_find_core("water", "valve")->settable == true,     "T3: valve must be settable");
static_assert(t_resolve("water", "valve", "open",  "VALVE_OPEN"),  "T3: open -> VALVE_OPEN");
static_assert(t_resolve("water", "valve", "close", "VALVE_CLOSE"), "T3: close -> VALVE_CLOSE");
static_assert(t_reject("water", "valve", "OPEN"),              "T3: OPEN must be rejected (case)");
static_assert(t_reject("water", "valve", "foo"),               "T3: foo must be rejected");
static_assert(dm_validate_core(dm_find_core("water", "valve"), "OPEN") == DM_VALID_ENUM_NOT_FOUND, "T3: invalid enum result code");
static_assert(t_cmd_ptr_within_table(),                        "T10: resolved cmd must live in property table");

// ---- §T4 Readonly rejection ----
static_assert(dm_validate_core(dm_find_core("water", "weight"), "5") == DM_VALID_NOT_SETTABLE, "T4: readonly must reject");
static_assert(t_reject("water", "weight", "5"),                "T4: readonly weight must not resolve");
static_assert(t_reject("climate", "temperature", "20"),        "T4: readonly temperature must not resolve");
static_assert(t_reject("system", "wifi-rssi", "1"),            "T4: readonly integer must not resolve");

// ---- §T5 Type / range ----
static_assert(t_resolve("water", "zero", "true", "WEIGHT_ZERO"),      "T5: bool true -> WEIGHT_ZERO");
static_assert(t_reject("water", "zero", "1"),                         "T5: bool must be true/false only");
static_assert(t_reject("water", "zero", "True"),                      "T5: bool case-sensitive");
static_assert(t_resolve("climate", "scan", "start", "MI_THERMO_START_SCAN"), "T5: scan start");
static_assert(t_resolve("climate", "scan", "stop",  "MI_THERMO_STOP_SCAN"),  "T5: scan stop");
static_assert(t_reject("climate", "scan", "pause"),                   "T5: unknown enum member rejected");
static_assert(t_resolve("control", "computer-reset", "true", "COMPUTER_RESET"), "T5: computer-reset");
static_assert(t_validator_int_float_branches(),                       "T5: int/float/range validator branches");
static_assert(t_parse_helpers(),                                      "T5: parse helpers strictness");

// ---- §T6 Weight ----
static_assert(cstr_eq(dm_find_core("water", "weight")->format, "0:10000:1"), "T6: weight format");
static_assert(dm_find_core("water", "weight")->deadband == 1.0f,             "T6: weight deadband 1.0");
static_assert(dm_find_core("water", "weight")->settable == false,            "T6: weight readonly");
static_assert(cstr_eq(dm_find_core("water", "weight")->unit, "g"),           "T6: weight unit g");
static_assert(dm_find_core("water", "weight")->src_kind == DM_SRC_STATE,     "T6: weight source");
static_assert(dm_find_core("water", "weight")->state_key == (uint16_t)STATE_WEIGHT_VALUE, "T6: weight state key");

// ---- §T7 Retained metadata ----
static_assert(dm_find_core("water", "valve")->retained == true,              "T7: valve retained true");
static_assert(dm_find_core("water", "weight")->retained == true,             "T7: weight retained true");
static_assert(dm_find_core("water", "zero")->retained == false,              "T7: zero retained false");
static_assert(dm_find_core("climate", "scan")->retained == false,            "T7: scan retained false");
static_assert(dm_find_core("control", "computer-reset")->retained == false,  "T7: computer-reset retained false");
static_assert(dm_find_core("system", "mqtt-connected")->retained == true,    "T7: readonly state retained true");

// ---- §T8 State source binding ----
static_assert(dm_find_core("water", "valve")->state_key == (uint16_t)STATE_VALVE_STATUS,        "T8: valve state key");
static_assert(dm_find_core("water", "weight-error")->state_key == (uint16_t)STATE_WEIGHT_ERROR, "T8: weight-error state key");
static_assert(dm_find_core("climate", "temperature")->state_key == (uint16_t)STATE_MI_THERMO_TEMP, "T8: temperature key");
static_assert(dm_find_core("climate", "humidity")->state_key == (uint16_t)STATE_MI_THERMO_HUMID,   "T8: humidity key");
static_assert(dm_find_core("climate", "battery")->state_key == (uint16_t)STATE_MI_THERMO_BAT_V,    "T8: battery key");
static_assert(dm_find_core("climate", "scan-enabled")->state_key == (uint16_t)STATE_MI_THERMO_ENABLE, "T8: scan-enabled key");
static_assert(dm_find_core("system", "mqtt-connected")->state_key == (uint16_t)STATE_MQTT_STATUS,  "T8: mqtt-connected key");
static_assert(dm_find_core("system", "wifi-rssi")->state_key == (uint16_t)STATE_WIFI_RSSI,         "T8: wifi-rssi key");
static_assert(dm_find_core("system", "wifi-signal")->state_key == (uint16_t)STATE_WIFI_SIGNAL,     "T8: wifi-signal key");
static_assert(dm_find_core("system", "time-valid")->state_key == (uint16_t)STATE_TIME_VALID,       "T8: time-valid key");
static_assert(dm_find_core("system", "event-queue")->state_key == (uint16_t)STATE_EVENT_QUEUE_COUNT, "T8: event-queue key");
static_assert(dm_find_core("system", "mqtt-retries")->state_key == (uint16_t)STATE_MQTT_RETRY_COUNT, "T8: mqtt-retries key");
static_assert(dm_find_core("water", "zero")->src_kind == DM_SRC_NONE,        "T8: zero has no read source");
static_assert(dm_find_core("climate", "scan")->src_kind == DM_SRC_NONE,      "T8: scan has no read source");
static_assert(dm_find_core("control", "computer-reset")->src_kind == DM_SRC_NONE, "T8: computer-reset has no read source");
static_assert(t_state_keys_in_range(),                                  "T8: state keys must be within STATE_MAX");

// ---- §T9 Enum read map ----
static_assert(dm_find_core("water", "valve")->enum_state_map_count == 2,     "T9: valve read map size");
static_assert(dm_find_core("water", "valve")->enum_state_map[0].state_value == 1,  "T9: valve map true");
static_assert(cstr_eq(dm_find_core("water", "valve")->enum_state_map[0].member, "open"),  "T9: true -> open");
static_assert(dm_find_core("water", "valve")->enum_state_map[1].state_value == 0,  "T9: valve map false");
static_assert(cstr_eq(dm_find_core("water", "valve")->enum_state_map[1].member, "close"), "T9: false -> close");
static_assert(dm_find_core("system", "wifi-signal")->enum_state_map_count == 5,    "T9: wifi-signal map size");

// ---- §T10 Command mapping integrity ----
static_assert(dm_find_core("water", "valve")->cmd_count == 2,                "T10: valve cmd count");
static_assert(dm_find_core("water", "zero")->cmd_count == 1,                 "T10: zero cmd count");
static_assert(dm_find_core("climate", "scan")->cmd_count == 2,               "T10: scan cmd count");
static_assert(dm_find_core("control", "computer-reset")->cmd_count == 1,     "T10: computer-reset cmd count");
static_assert(t_settable_have_cmds(),                                   "T10: every settable property maps >=1 command");
static_assert(t_readonly_no_cmds(),                                     "T10: readonly property must not carry commands");
static_assert(t_enum_formats_nonempty(),                                "T10: enum must declare format");


#endif // DM_HAS_RELAXED_CONSTEXPR