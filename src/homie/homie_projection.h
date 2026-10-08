#ifndef HOMIE_PROJECTION_H
#define HOMIE_PROJECTION_H

// =====================================================
// P2-3 · Homie Projection —— 纯逻辑层（**零 MQTT / 零硬件 / 零业务依赖**）
//
// 上位冻结设计（唯一依据）：
//   docs/architecture/P2-0-architecture-review-and-design-freeze.md
//   §6（Projection Boundary）· §8（Topic）· §11（$description）
//   §12（Node / Property / valve 显式映射）· §13（Datatype / Validation）
//   §15.3（Projection Contract + fingerprint）· §17.4（变化检测与限流）
//   （P2-0.1 Freeze Corrections 后版本）
//
// 为什么单独抽出本文件：
//   · **单一真源**：固件（homie/homie_bridge.cpp）与宿主测试
//     （test/homie_bridge_host/*）共同编译同一份逻辑，避免实现与测试漂移
//     —— 与 P2-2 的 `cloud/mqtt_routing.h` 同范式。
//   · **可离线求值**：不依赖 libc / Arduino runtime，可在 PC 上真实执行。
//
// 依赖边界（必须保持）：
//   ✅ <stdint.h> / <stddef.h> / homie/device_model.h
//   ❌ cloud/mqtt_transport.h · cloud/cloud_manager.h · mqtt_client.h
//      services/command_manager.h · app/*.h（硬件）
//   理由：本层只做「数据 → 语义」变换；传输与执行分别在 P2-2 / CommandManager。
//
// 边界外的东西（本层刻意不碰）：
//   MQTT 收发、System State 读取、CommandManager 执行、Registry 查询。
//   这些都由 homie_bridge.cpp 通过**泛型接口**注入（见 HomieActionAvailFn /
//   HomieSample），因此既保持本层可移植，也不把 await/exec 逻辑塞进纯逻辑。
// =====================================================

#include <stdint.h>
#include <stddef.h>
#include "homie/device_model.h"

// =====================================================
// 容量常量（有限、明确、可检查 —— P2-0 §17.3 禁止无上限增长）
// =====================================================
#define HOMIE_TOPIC_MAX        128   // 单条 topic 最大字符数（含结尾 '\0'）
#define HOMIE_DESC_CAP         4096  // $description 硬上限（超过 ⇒ 不发布，§11.4）
#define HOMIE_DESC_TARGET      2048  // 目标体积（仅告警参考，非硬失败）
#define HOMIE_VALUE_MAX        40    // 单个属性值序列化缓冲
#define HOMIE_MAX_BOOT_SEQ     0xFFFF

// =====================================================
// $description schema 版本（编译期常量；表结构变更时手工 +1 —— §11.3）
// =====================================================
#ifndef GF_HOMIE_DESC_VERSION
#define GF_HOMIE_DESC_VERSION  1u
#endif

// =====================================================
// 最小字符串工具（无 libc；与 mqtt_routing.h 同范式）
// =====================================================
inline size_t hp_len(const char* s)
{
    size_t n = 0;
    if(s == NULL) { return 0; }
    while(s[n] != '\0') { ++n; }
    return n;
}

inline bool hp_eq(const char* a, const char* b)
{
    if(a == NULL || b == NULL) { return a == b; }
    size_t i = 0;
    for(; a[i] != '\0' && b[i] != '\0'; ++i) { if(a[i] != b[i]) { return false; } }
    return a[i] == b[i];
}

// 复制并补 '\0'；超长则**失败**（不静默截断 —— 与 Topic/Subscription 边界一致）
inline bool hp_copy(char* dst, size_t cap, const char* src)
{
    if(dst == NULL || cap == 0) { return false; }
    size_t n = hp_len(src);
    if(n + 1 > cap) { return false; }
    for(size_t i = 0; i < n; ++i) { dst[i] = src[i]; }
    dst[n] = '\0';
    return true;
}

// 追加；容量不足 ⇒ 返回 false（**绝不**越界写）
inline bool hp_append(char* dst, size_t cap, size_t* len, const char* src)
{
    if(dst == NULL || src == NULL || len == NULL) { return false; }
    const size_t sn = hp_len(src);
    if(*len + sn + 1 > cap) { return false; }
    for(size_t i = 0; i < sn; ++i) { dst[*len + i] = src[i]; }
    *len += sn;
    dst[*len] = '\0';
    return true;
}

inline bool hp_append_char(char* dst, size_t cap, size_t* len, char c)
{
    if(dst == NULL || len == NULL) { return false; }
    if(*len + 2 > cap) { return false; }
    dst[*len] = c;
    *len += 1;
    dst[*len] = '\0';
    return true;
}

// =====================================================
// ① datatype → Homie 字符串（FROZEN §13.1）
//   4→4 一一对应；json 类型定义了但 P2 不使用（§13.1 明确）
// =====================================================
inline const char* homie_datatype_name(DeviceModelDatatype t)
{
    switch(t)
    {
        case DM_BOOL:   return "boolean";
        case DM_INT:    return "integer";
        case DM_FLOAT:  return "float";
        case DM_STRING: return "string";
        case DM_ENUM:   return "enum";
        case DM_JSON:   return "json";
        default:        return "";
    }
}

// =====================================================
// ② FNV-1a 32（$description.version 与 snapshot fingerprint —— §11.3 / §15.3）
// =====================================================
#define HOMIE_FNV_SEED 0x811C9DC5u

inline uint32_t homie_fnv1a32(uint32_t h, const char* s)
{
    if(s == NULL) { return h; }
    for(const char* p = s; *p != '\0'; ++p)
    {
        h ^= (uint32_t)(unsigned char)*p;
        h *= 0x01000193u;
    }
    return h;
}

inline uint32_t homie_fnv1a32_u32(uint32_t h, uint32_t v)
{
    char dec[12];
    size_t n = 0;
    if(v == 0) { dec[n++] = '0'; }
    while(v > 0) { dec[n++] = (char)('0' + (v % 10u)); v /= 10u; }
    for(size_t i = 0; i < n / 2; ++i) { char t = dec[i]; dec[i] = dec[n - 1 - i]; dec[n - 1 - i] = t; }
    dec[n] = '\0';
    return homie_fnv1a32(h, dec);
}

// version = fnv1a32(desc_schema_version, action_registry.version, trigger_registry.version)
inline uint32_t homie_desc_version(uint32_t action_version, uint32_t trigger_version)
{
    uint32_t h = HOMIE_FNV_SEED;
    h = homie_fnv1a32_u32(h, (uint32_t)GF_HOMIE_DESC_VERSION);
    h = homie_fnv1a32(h, "|a=");
    h = homie_fnv1a32_u32(h, action_version);
    h = homie_fnv1a32(h, "|t=");
    h = homie_fnv1a32_u32(h, trigger_version);
    return h;
}

// =====================================================
// ③ Topic 构造（FROZEN §8.1：homie/5/<device_id>/）
//
//   ★ device_id 必须由**调用方**传入（来自 DeviceIdentity），本层不产身份。
//   ★ 所有函数遵守「容量不足 ⇒ 失败」原则，避免截断产生错误 topic。
// =====================================================
inline bool homie_base_topic(const char* device_id, char* out, size_t cap)
{
    if(device_id == NULL || device_id[0] == '\0') { return false; }
    size_t len = 0;
    out[0] = '\0';
    if(!hp_append(out, cap, &len, "homie/5/"))              { return false; }
    if(!hp_append(out, cap, &len, device_id))               { return false; }
    if(!hp_append_char(out, cap, &len, '/'))                { return false; }
    return true;
}

inline bool homie_state_topic(const char* base, char* out, size_t cap)
{
    if(base == NULL) { return false; }
    size_t len = 0;
    out[0] = '\0';
    return hp_append(out, cap, &len, base) && hp_append(out, cap, &len, "$state");
}

inline bool homie_description_topic(const char* base, char* out, size_t cap)
{
    if(base == NULL) { return false; }
    size_t len = 0;
    out[0] = '\0';
    return hp_append(out, cap, &len, base) && hp_append(out, cap, &len, "$description");
}

// 属性值 topic：<base><node>/<prop>
inline bool homie_property_topic(const char* base, const char* node, const char* prop,
                                 char* out, size_t cap)
{
    if(base == NULL || node == NULL || prop == NULL) { return false; }
    size_t len = 0;
    out[0] = '\0';
    return hp_append(out, cap, &len, base)
        && hp_append(out, cap, &len, node)
        && hp_append_char(out, cap, &len, '/')
        && hp_append(out, cap, &len, prop);
}

// 订阅 filter：<base>+/+/set（★ 精确、不含 '#'、不跨设备 —— FROZEN §8.3/§33）
inline bool homie_set_filter(const char* base, char* out, size_t cap)
{
    if(base == NULL) { return false; }
    size_t len = 0;
    out[0] = '\0';
    return hp_append(out, cap, &len, base) && hp_append(out, cap, &len, "+/+/set");
}

// =====================================================
// ④ Topic 解析 / 命名空间校验（安全边界 —— FROZEN §十九 / §34）
//
//   职责划分：
//     ACL  : namespace isolation（设备子树）
//     本层 : 二次确认「这条消息确实属于**本设备自己的** Homie namespace」
//   ⇒ 即使 ACL 被误配，也不会接受别的设备的 topic。
//
//   ★ 判据是 topic 本身（不是 payload / cmd_id 等启发式）。
// =====================================================
inline bool homie_topic_is_own(const char* topic, const char* base)
{
    if(topic == NULL || base == NULL) { return false; }
    const size_t bl = hp_len(base);
    if(hp_len(topic) <= bl) { return false; }
    for(size_t i = 0; i < bl; ++i)
    {
        if(topic[i] != base[i]) { return false; }
    }
    return true;
}

// 取第 n 层（0-based）的 [begin, end) 指针；层数不足 ⇒ false
inline bool homie_topic_level(const char* topic, size_t idx,
                              const char** out_begin, const char** out_end)
{
    if(topic == NULL || out_begin == NULL || out_end == NULL) { return false; }
    const char* p = topic;
    size_t cur = 0;
    for(;;)
    {
        const char* b = p;
        while(*p != '\0' && *p != '/') { ++p; }
        if(cur == idx)
        {
            *out_begin = b;
            *out_end = p;
            return true;
        }
        if(*p == '\0') { return false; }
        ++p;
        ++cur;
    }
}

// id 合法性：[a-z0-9-]，非空，不以 '$' 开头（FROZEN §三 / §5.4）
inline bool homie_id_valid(const char* s)
{
    if(s == NULL || s[0] == '\0') { return false; }
    if(s[0] == '$') { return false; }
    for(const char* p = s; *p != '\0'; ++p)
    {
        const char c = *p;
        const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || (c == '-');
        if(!ok) { return false; }
    }
    return true;
}

// 解析 `<base><node>/<prop>/set` ⇒ node/prop
//   ★ 必须：① 属于本设备 namespace ② 恰好 3 段（node/prop/set）
//          ③ node/prop 字符集合法
//   任何一条不满足 ⇒ false（不进入后续任何处理）
inline bool homie_parse_set_topic(const char* topic, const char* base,
                                  char* node, size_t node_cap,
                                  char* prop, size_t prop_cap)
{
    if(!homie_topic_is_own(topic, base)) { return false; }

    // base = "homie/5/<device_id>/" 共 3 层（末尾 '/' 之后的空层不计）
    // ⇒ 其后应恰好是：第 3 层 = node，第 4 层 = prop，第 5 层 = 字面 "set"，
    //    第 6 层**不得存在**（多出层级 ⇒ 拒绝）。
    const char* b0 = NULL; const char* e0 = NULL;
    const char* b1 = NULL; const char* e1 = NULL;
    const char* b2 = NULL; const char* e2 = NULL;
    const char* b3 = NULL; const char* e3 = NULL;
    if(!homie_topic_level(topic, 3, &b0, &e0)) { return false; }
    if(!homie_topic_level(topic, 4, &b1, &e1)) { return false; }
    if(!homie_topic_level(topic, 5, &b2, &e2)) { return false; }
    if(homie_topic_level(topic, 6, &b3, &e3))  { return false; }   // 多出层级 ⇒ 拒绝

    // 第 6 层必须是字面 "set"
    {
        char tmp[HOMIE_TOPIC_MAX];
        size_t n = (size_t)(e2 - b2);
        if(n + 1 > sizeof(tmp)) { return false; }
        for(size_t i = 0; i < n; ++i) { tmp[i] = b2[i]; }
        tmp[n] = '\0';
        if(!hp_eq(tmp, "set")) { return false; }
    }

    // node / prop 复制（长度校验后逐字节）
    const size_t nl = (size_t)(e0 - b0);
    const size_t pl = (size_t)(e1 - b1);
    if(nl == 0 || pl == 0)                  { return false; }
    if(nl + 1 > node_cap || pl + 1 > prop_cap) { return false; }
    for(size_t i = 0; i < nl; ++i) { node[i] = b0[i]; }
    node[nl] = '\0';
    for(size_t i = 0; i < pl; ++i) { prop[i] = b1[i]; }
    prop[pl] = '\0';

    return homie_id_valid(node) && homie_id_valid(prop);
}

// =====================================================
// ⑤ Availability / Validity Gate（可用性 & 有效性策略）
//
// 为什么需要这张表 —— 代码实证：
//   · weight.enable=false 时 weight.cpp:716-725 仍会
//     state_set_float(STATE_WEIGHT_VALUE, 0.0f) / state_set_bool(STATE_WEIGHT_ERROR,false)
//     ⇒ **两个 state 的 valid 都是 true** ⇒ 仅靠 state_is_valid() 无法区分。
//   · MiThermometer.cpp:230-239 初始把 TEMP/HUMID/BAT 置为 NAN（valid=true），
//     真正的有效性由独立的 STATE_MI_THERMO_VALID 表达。
//   ⇒ 「该能力本次 boot 是否存在」只能来自 Capability Registry（action 是否注册）；
//     「该读数当前是否有效」来自显式的 validity state key。
//
// 职责定位（避免越界）：
//   Device Model        = 语义（node/prop/datatype/unit/state_key/command 映射）
//   Capability Registry = 本次 boot 的能力是否存在（action 注册情况）
//   本表                = **两者之外的第三件**：把某个 property 与「谁代表它存在」
//                         关联起来的**可用性策略**（既不重复 datatype/name，
//                         也不定义新的状态语义，只复用已有 key / action_id）
//
// ★ 只登记「Device Model 无法表达」的项：
//     - settable 属性的可用性直接由其 cmds[] 中的 action 推导（无需在此登记）
//     - 仅**无自身 action 的纯状态属性**需要在此显式锚定
// =====================================================
#define HOMIE_GATE_NONE ((uint16_t)STATE_MAX)

struct HomieGate
{
    const char* node;
    const char* prop;
    const char* avail_action;   // boot-time 可用性锚：该 action 未注册 ⇒ 本属性不投影
    uint16_t    valid_key;      // 运行期有效性门控 state key；HOMIE_GATE_NONE = 无
};

inline const HomieGate* homie_gate_find_all(size_t* out_count);   // 前向（定义见下）

inline const HomieGate* homie_gate_find(const char* node, const char* prop)
{
    size_t n = 0;
    const HomieGate* tbl = homie_gate_find_all(&n);
    if(tbl == NULL) { return NULL; }
    for(size_t i = 0; i < n; ++i)
    {
        if(hp_eq(tbl[i].node, node) && hp_eq(tbl[i].prop, prop)) { return &tbl[i]; }
    }
    return NULL;
}

// gate 表本体（编译期常量 ⇒ Flash；无运行期可变状态）
inline const HomieGate* homie_gate_find_all(size_t* out_count)
{
    static const HomieGate kGates[] = {
        // ---- weight 可用性簇：`weight.enable=false` ⇒ WEIGHT_ZERO 不注册（§12.1）
        //      ⇒ 这三个 water 属性在 boot 生成 $description 时就都不进入（AD-15b）
        { "water", "weight",       "WEIGHT_ZERO", HOMIE_GATE_NONE },
        { "water", "weight-error", "WEIGHT_ZERO", HOMIE_GATE_NONE },
        // ---- 米家读数：有效性由 STATE_MI_THERMO_VALID 表达（§三十 / MiThermometer.cpp）
        //      ⇒ 保留现有 validity 语义，**不**在 Bridge 内重新定义
        { "climate", "temperature", NULL, (uint16_t)STATE_MI_THERMO_VALID },
        { "climate", "humidity",    NULL, (uint16_t)STATE_MI_THERMO_VALID },
        { "climate", "battery",     NULL, (uint16_t)STATE_MI_THERMO_VALID },
    };
    if(out_count != NULL) { *out_count = (size_t)(sizeof(kGates) / sizeof(kGates[0])); }
    return kGates;
}

// =====================================================
// ⑥ 属性值读取结果（调用方按 Device Model 的 dtype/state_key 泛型填充）
// =====================================================
struct HomieSample
{
    int32_t i;      // bool / int / enum 的整数值
    float   f;      // float 值
    bool    valid;  // 读取成功且通过了 gate（false ⇒ 不发布，也不伪造值 —— §二十九）
};

// =====================================================
// ⑦ 数值 → Homie 序列化（FROZEN §十六：禁止 String(state) 强转）
//
//   ★ datatype 严格按照 Device Model：
//       BOOL  → "true" / "false"
//       INT   → 十进制
//       FLOAT → 定点小数；小数位由 format 的 step 段推导（无 step ⇒ 2 位）
//       ENUM  → 查 enum_state_map（显式表；禁止从 runtime_id 推导）
//       STRING→ 不支持读侧（P2 无 string 状态属性）
// =====================================================

// 由 format 的第三段（step）推导小数位；
// 例："0:10000:1" ⇒ step="1" ⇒ 0 位；"0:100:0.1" ⇒ 1 位。
// 至少 1 位（保证 JSON 侧被解析为 float），至多 3 位。
inline int homie_float_decimals(const char* format)
{
    if(format == NULL) { return 2; }
    int colon = 0;
    const char* p = format;
    const char* step = NULL;
    for(; *p != '\0'; ++p)
    {
        if(*p == ':') { ++colon; if(colon == 2) { step = p + 1; } }
    }
    if(colon < 2 || step == NULL || *step == '\0') { return 2; }
    // step 形如 "1" / "0.1" / "0.01"
    bool seen_dot = false;
    int  digits = 0;
    for(const char* q = step; *q != '\0'; ++q)
    {
        if(*q == '.') { seen_dot = true; continue; }
        if(*q < '0' || *q > '9') { return 2; }
        if(seen_dot) { ++digits; }
    }
    if(!seen_dot)      { return 1; }   // 整数 step ⇒ 至少保留 1 位，输出仍是 float
    if(digits > 3)     { return 3; }
    return digits;
}

// 有符号整数 → 十进制（不依赖 libc）
inline bool homie_int_to_str(int32_t v, char* out, size_t cap)
{
    if(out == NULL || cap == 0) { return false; }
    size_t len = 0;
    out[0] = '\0';
    if(v < 0)
    {
        if(!hp_append_char(out, cap, &len, '-')) { return false; }
    }
    uint32_t u = (v < 0) ? (uint32_t)(0u - (uint32_t)v) : (uint32_t)v;

    char digits[16];
    size_t n = 0;
    if(u == 0) { digits[n++] = '0'; }
    while(u > 0) { digits[n++] = (char)('0' + (u % 10u)); u /= 10u; }
    for(size_t k = 0; k < n; ++k)
    {
        if(!hp_append_char(out, cap, &len, digits[n - 1 - k])) { return false; }
    }
    return true;
}

// 无符号整数 → 十进制（供 $description.version 等 uint32 字段使用）
inline bool homie_uint_to_str(uint32_t u, char* out, size_t cap)
{
    if(out == NULL || cap == 0) { return false; }
    size_t len = 0;
    out[0] = '\0';
    char digits[16];
    size_t n = 0;
    if(u == 0) { digits[n++] = '0'; }
    while(u > 0) { digits[n++] = (char)('0' + (u % 10u)); u /= 10u; }
    for(size_t k = 0; k < n; ++k)
    {
        if(!hp_append_char(out, cap, &len, digits[n - 1 - k])) { return false; }
    }
    return true;
}

// 定点小数序列化（不依赖 libm / snprintf）
//
//   做法：先按 decimals 放大并四舍五入成整数，再从整数拆出「整数部分 / 小数部分」。
//   这样既避免了浮点逐位取余的误差，也保持实现简短可验证。
//   ★ 定容缓冲：容量不足 ⇒ 失败（绝不截断、绝不越界）。
inline bool homie_float_to_str(float v, int decimals, char* out, size_t cap)
{
    if(out == NULL || cap == 0) { return false; }
    if(decimals < 0) { decimals = 0; }
    if(decimals > 3) { decimals = 3; }

    // NaN / Inf ⇒ 拒绝（不把非法数写进 JSON）
    if(!(v == v) || v > 3.0e30f || v < -3.0e30f) { return false; }

    const bool neg = (v < 0.0f);
    double x = neg ? -(double)v : (double)v;

    // 10^decimals（decimals ≤ 3 ⇒ 精确）
    uint64_t pow10 = 1;
    for(int k = 0; k < decimals; ++k) { pow10 *= 10u; }

    double scaled = x * (double)pow10 + 0.5;             // 四舍五入
    if(scaled > 9.0e14) { scaled = 9.0e14; }             // 上界保护（远超本工程量程）
    const uint64_t total = (uint64_t)scaled;

    const uint64_t ipart = total / pow10;
    const uint64_t fpart = total - ipart * pow10;

    size_t len = 0;
    out[0] = '\0';
    if(neg)
    {
        if(!hp_append_char(out, cap, &len, '-')) { return false; }
    }

    // 整数部分
    {
        char digits[24];
        size_t n = 0;
        uint64_t t = ipart;
        if(t == 0) { digits[n++] = '0'; }
        while(t > 0) { digits[n++] = (char)('0' + (t % 10u)); t /= 10u; }
        for(size_t k = 0; k < n; ++k)
        {
            if(!hp_append_char(out, cap, &len, digits[n - 1 - k])) { return false; }
        }
    }

    // 小数部分（定宽补零）
    if(decimals > 0)
    {
        if(!hp_append_char(out, cap, &len, '.')) { return false; }
        char buf[8];
        uint64_t t = fpart;
        for(int k = decimals - 1; k >= 0; --k) { buf[k] = (char)('0' + (t % 10u)); t /= 10u; }
        buf[decimals] = '\0';
        if(!hp_append(out, cap, &len, buf)) { return false; }
    }
    return true;
}

// 读侧格式化：sample → Homie payload
inline bool homie_format_value(const DeviceModelProperty* p,
                               const HomieSample& s,
                               char* out, size_t cap, size_t* out_len)
{
    if(p == NULL || out == NULL || cap == 0) { return false; }
    if(!s.valid) { return false; }
    if(out_len != NULL) { *out_len = 0; }

    switch(p->dtype)
    {
        case DM_BOOL:
        {
            if(!hp_copy(out, cap, s.i != 0 ? "true" : "false")) { return false; }
            if(out_len != NULL) { *out_len = hp_len(out); }
            return true;
        }
        case DM_INT:
        {
            if(!homie_int_to_str(s.i, out, cap)) { return false; }
            if(out_len != NULL) { *out_len = hp_len(out); }
            return true;
        }
        case DM_FLOAT:
        {
            if(!homie_float_to_str(s.f, homie_float_decimals(p->format), out, cap)) { return false; }
            if(out_len != NULL) { *out_len = hp_len(out); }
            return true;
        }
        case DM_ENUM:
        {
            // ★ 显式 enum_state_map 查表（禁止从 runtime_id / 大小写变换推导 —— §12.5）
            if(p->enum_state_map == NULL || p->enum_state_map_count == 0) { return false; }
            for(uint8_t i = 0; i < p->enum_state_map_count; ++i)
            {
                if(p->enum_state_map[i].state_value == s.i)
                {
                    if(!hp_copy(out, cap, p->enum_state_map[i].member)) { return false; }
                    if(out_len != NULL) { *out_len = hp_len(out); }
                    return true;
                }
            }
            return false;   // 状态值不在映射表内 ⇒ **不发布**（不伪造）
        }
        case DM_STRING:
        case DM_JSON:
        default:
        {
            // P2 没有 string / json 状态属性 ⇒ 不支持读侧序列化
            return false;
        }
    }
}

// =====================================================
// ⑧ 变化检测（FROZEN §17.4：变化驱动 + 死区 + 最小间隔）
//
//   float/int ⇒ |Δ| >= deadband（deadband==0 ⇒ 退化为严格不等）
//   其余      ⇒ 逐值不等
//   ★ first==true（无历史）⇒ 无条件视为变化（§17.4「首次无条件发布初值」）
// =====================================================
inline bool homie_value_changed(DeviceModelDatatype dtype,
                                const HomieSample& prev, bool first,
                                const HomieSample& cur, float deadband)
{
    if(first) { return true; }
    if(!cur.valid) { return false; }
    if(dtype == DM_FLOAT)
    {
        if(!prev.valid) { return true; }
        if(prev.f == cur.f) { return false; }
        if(deadband <= 0.0f) { return true; }
        float d = cur.f - prev.f;
        if(d < 0.0f) { d = -d; }
        return d >= deadband;
    }
    if(dtype == DM_INT)
    {
        if(!prev.valid) { return true; }
        if(prev.i == cur.i) { return false; }
        if(deadband <= 0.0f) { return true; }
        double d = (double)cur.i - (double)prev.i;
        if(d < 0.0) { d = -d; }
        return d >= (double)deadband;
    }
    if(!prev.valid) { return true; }
    return !(prev.i == cur.i && prev.f == cur.f);
}

// =====================================================
// ⑨ $description 构建（FROEN §11；boot-time snapshot，本层只做纯变换）
//
//   输入：device_id + 「action 是否可用」谓词 + registry versions
//   输出：JSON 文本（写入调用方提供的缓冲区）+ version
//
//   ★ 溢出即失败（返回 false），调用方停在 init + ERROR —— §11.4 硬上限语义
//   ★ 不含：workflow、stable_id、任何 secret（§七 / §11.2 AD-9）
// =====================================================
typedef bool (*HomieActionAvailFn)(const char* action_id, void* ctx);

struct HomieDescInput
{
    const char*        device_id;
    char*              buf;      // 输出缓冲
    size_t             cap;      // 缓冲容量（应 ≥ HOMIE_DESC_CAP）
    HomieActionAvailFn avail;    // 「某 action 本次 boot 是否注册」谓词
    void*              avail_ctx;
    uint32_t           action_version;
    uint32_t           trigger_version;
};

// 单个属性是否进入本次 description（boot-time availability filter）
inline bool homie_property_available(const DeviceModelProperty* p,
                                     HomieActionAvailFn avail, void* ctx)
{
    if(p == NULL) { return false; }

    // ① 写侧：所有显式映射的 action 都必须可用
    if(p->cmds != NULL && p->cmd_count > 0)
    {
        for(uint8_t i = 0; i < p->cmd_count; ++i)
        {
            if(avail == NULL || !avail(p->cmds[i].action_id, ctx)) { return false; }
        }
    }

    // ② 读侧：无自身 action 的属性，按 HomieGate 显式锚定的 action 判定
    const HomieGate* g = homie_gate_find(p->node, p->id);
    if(g != NULL && g->avail_action != NULL)
    {
        if(avail == NULL || !avail(g->avail_action, ctx)) { return false; }
    }

    return true;
}

// JSON 字符串转义（控制字符 / " / \）；UTF-8 原样输出（node/property 名含中文）
inline bool hp_json_append_string(char* dst, size_t cap, size_t* len, const char* s)
{
    if(!hp_append_char(dst, cap, len, '"')) { return false; }
    if(s != NULL)
    {
        for(const char* p = s; *p != '\0'; ++p)
        {
            const unsigned char c = (unsigned char)*p;
            if(c == '"' || c == '\\')
            {
                if(!hp_append_char(dst, cap, len, '\\')) { return false; }
                if(!hp_append_char(dst, cap, len, (char)c)) { return false; }
            }
            else if(c < 0x20)
            {
                if(!hp_append(dst, cap, len, "\\u00")) { return false; }
                const char* hex = "0123456789abcdef";
                if(!hp_append_char(dst, cap, len, hex[(c >> 4) & 0xF])) { return false; }
                if(!hp_append_char(dst, cap, len, hex[c & 0xF]))        { return false; }
            }
            else
            {
                if(!hp_append_char(dst, cap, len, (char)c)) { return false; }
            }
        }
    }
    return hp_append_char(dst, cap, len, '"');
}

inline bool homie_desc_build(const HomieDescInput& in, size_t* out_len, uint32_t* out_version)
{
    if(in.device_id == NULL || in.buf == NULL || in.cap == 0) { return false; }
    if(out_len != NULL) { *out_len = 0; }
    if(out_version != NULL) { *out_version = 0; }

    char*  dst = in.buf;
    size_t cap = in.cap;
    size_t len = 0;
    dst[0] = '\0';

    char num[16];

    // ---- 头 ----
    if(!hp_append(dst, cap, &len, "{\"homie\":\"5.0\",\"version\":")) { return false; }
    {
        const uint32_t v = homie_desc_version(in.action_version, in.trigger_version);
        homie_uint_to_str(v, num, sizeof(num));
        if(!hp_append(dst, cap, &len, num)) { return false; }
        if(out_version != NULL) { *out_version = v; }
    }
    // ★ 设备级 name **省略**（P2 不引入友好名 Config —— OQ-1 / D-8 / §11.1）
    if(!hp_append(dst, cap, &len, ",\"nodes\":{")) { return false; }

    bool any_node = false;
    const uint8_t ncount = dm_node_count();
    for(uint8_t ni = 0; ni < ncount; ++ni)
    {
        const DeviceModelNode* node = dm_node_at(ni);
        if(node == NULL) { continue; }

        // 收集本节点本次可用的属性（保持 Device Model 表序，确定性输出）
        const DeviceModelProperty* plist[32];
        uint8_t pn = 0;
        const uint8_t pcount = dm_property_count();
        for(uint8_t pi = 0; pi < pcount; ++pi)
        {
            const DeviceModelProperty* p = dm_property_at(pi);
            if(p == NULL) { continue; }
            if(!hp_eq(p->node, node->id)) { continue; }
            if(!homie_property_available(p, in.avail, in.avail_ctx)) { continue; }
            if(pn < (uint8_t)(sizeof(plist) / sizeof(plist[0]))) { plist[pn++] = p; }
        }
        if(pn == 0) { continue; }     // 该节点无任何可用属性 ⇒ 整个节点省略

        if(any_node)
        {
            if(!hp_append_char(dst, cap, &len, ',')) { return false; }
        }
        any_node = true;

        if(!hp_json_append_string(dst, cap, &len, node->id))     { return false; }
        if(!hp_append(dst, cap, &len, ":{\"name\":"))            { return false; }
        if(!hp_json_append_string(dst, cap, &len, node->name))   { return false; }
        if(!hp_append(dst, cap, &len, ",\"type\":"))             { return false; }
        if(!hp_json_append_string(dst, cap, &len, node->type))   { return false; }
        if(!hp_append(dst, cap, &len, ",\"properties\":{"))      { return false; }

        for(uint8_t k = 0; k < pn; ++k)
        {
            const DeviceModelProperty* p = plist[k];
            if(k > 0)
            {
                if(!hp_append_char(dst, cap, &len, ',')) { return false; }
            }
            if(!hp_json_append_string(dst, cap, &len, p->id))          { return false; }
            if(!hp_append(dst, cap, &len, ":{\"datatype\":"))          { return false; }
            if(!hp_json_append_string(dst, cap, &len, homie_datatype_name(p->dtype))) { return false; }
            // name
            if(p->name != NULL && p->name[0] != '\0')
            {
                if(!hp_append(dst, cap, &len, ",\"name\":"))            { return false; }
                if(!hp_json_append_string(dst, cap, &len, p->name))     { return false; }
            }
            // format（enum 成员表 / 数值范围；非空才发 —— §12.2）
            if(p->format != NULL && p->format[0] != '\0')
            {
                if(!hp_append(dst, cap, &len, ",\"format\":"))          { return false; }
                if(!hp_json_append_string(dst, cap, &len, p->format))   { return false; }
            }
            // unit（空 ⇒ 省略该字段 —— §12.2）
            if(p->unit != NULL && p->unit[0] != '\0')
            {
                if(!hp_append(dst, cap, &len, ",\"unit\":"))            { return false; }
                if(!hp_json_append_string(dst, cap, &len, p->unit))     { return false; }
            }
            // settable / retained
            if(!hp_append(dst, cap, &len, ",\"settable\":"))           { return false; }
            if(!hp_append(dst, cap, &len, p->settable ? "true" : "false")) { return false; }
            if(!hp_append(dst, cap, &len, ",\"retained\":"))           { return false; }
            if(!hp_append(dst, cap, &len, p->retained ? "true" : "false")) { return false; }
            if(!hp_append_char(dst, cap, &len, '}'))                   { return false; }
        }

        if(!hp_append(dst, cap, &len, "}}")) { return false; }   // 关 properties + node
    }

    if(!hp_append(dst, cap, &len, "}}")) { return false; }       // 关 nodes + root

    if(out_len != NULL) { *out_len = len; }
    return true;
}

// =====================================================
// ⑩ cmd_id 生成（FROZEN §7.3）
//    "h" + <boot_seq:4hex> + "-" + <counter:6hex>
//    ★ 设备自生成；客户端永远无法提供 / 覆盖（set 载荷无 cmd_id 字段）
// =====================================================
inline void homie_cmd_id(uint16_t boot_seq, uint32_t counter, char* out, size_t cap)
{
    if(out == NULL || cap < 13) { if(out != NULL && cap > 0) { out[0] = '\0'; } return; }
    static const char* kHex = "0123456789abcdef";
    size_t n = 0;
    out[n++] = 'h';
    for(int i = 3; i >= 0; --i) { out[n++] = kHex[(boot_seq >> (i * 4)) & 0xF]; }
    out[n++] = '-';
    for(int i = 5; i >= 0; --i) { out[n++] = kHex[(counter >> (i * 4)) & 0xF]; }
    out[n] = '\0';
}

// =====================================================
// ⑪ 命令结果观察 + pending 表（纯逻辑 —— FROZEN §7.4 / 任务 §五）
//
// 目的：把「/set → CommandManager → 结果观察」的全部判定做成**无副作用纯函数**，
//       固件与宿主测试共用同一份真源（不新增第二套 command result system）。
//
// ★ 冻结约束（AD-7a / §7.4）：
//   - Bridge **不注册** CommandManager 结果回调（唯一回调槽归 CloudManager）
//   - 只读轮询 command_manager_get_runtime()
//   - 不新增去重缓存、不新增队列（pending 表本身即唯一有界跟踪结构）
//
// ★ 设备侧可观测性（**代码实证**，非假设）：
//   command_manager.cpp 的 command_temp_action_callback() 与
//   command_runtime_report_timeout() 在**同一次同步调用**内「置终态 + 上报 + release」，
//   而 command_runtime_release() 会清 active 与 cmd_id（:434-451），
//   command_runtime_find_by_cmd_id() 要求 active && cmd_id 匹配（:417-429）。
//   ⇒ command_manager_get_runtime() 只会返回「PENDING 的 runtime」或「nullptr」。
//     设备侧可观察到的只有：
//       HOMIE_OBS_RUNNING （还在执行）
//       HOMIE_OBS_RELEASED（终态已交付 ⇒ 具体结果走既有业务 up 结果通道）
//     HOMIE_OBS_SUCCESS / FAILED / TIMEOUT 三个终态**在设备侧不可达**，
//     由宿主测试直接注入验证（纯函数可表达），并作为 OPEN ISSUE 记录：
//     设备侧区分终态需改 CommandManager ⇒ 触发 STOP-3 / STOP-16。
// =====================================================
#define HOMIE_PENDING_CAP       4      // pending 表定容（FROZEN §7.4；不得运行时增长）
#define HOMIE_CMD_ID_MAX        16     // "h<4hex>-<6hex>" = 12 字符 + '\0'

// 设备侧对 CommandManager 运行时的观察结果
enum HomieObservedState : uint8_t
{
    HOMIE_OBS_RUNNING = 0,   // runtime 存在且 state == PENDING
    HOMIE_OBS_SUCCESS,       // runtime 存在且 state == SUCCESS
    HOMIE_OBS_FAILED,        // runtime 存在且 state == FAILED
    HOMIE_OBS_TIMEOUT,       // runtime 存在且 state == TIMEOUT
    HOMIE_OBS_RELEASED       // runtime 不存在 ⇒ 终态已交付（或从未创建）
};

// 分类结论（任务 §五.5）
enum HomieOutcome : uint8_t
{
    HOMIE_OUTCOME_NONE = 0,   // 未跟踪 / 无结论
    HOMIE_OUTCOME_PENDING,    // accepted，仍在执行
    HOMIE_OUTCOME_SUCCESS,
    HOMIE_OUTCOME_FAILED,
    HOMIE_OUTCOME_TIMEOUT,    // 到期（CommandManager 超时或 Bridge 兜底看门狗）
    HOMIE_OUTCOME_REJECTED,   // command_manager_execute() 拒绝（未创建 runtime）
    HOMIE_OUTCOME_COMPLETED   // 终态已由既有业务通道交付（本通道不携带具体结果）
};

inline HomieOutcome homie_classify_observed(HomieObservedState s)
{
    switch(s)
    {
        case HOMIE_OBS_RUNNING:  return HOMIE_OUTCOME_PENDING;
        case HOMIE_OBS_SUCCESS:  return HOMIE_OUTCOME_SUCCESS;
        case HOMIE_OBS_FAILED:   return HOMIE_OUTCOME_FAILED;
        case HOMIE_OBS_TIMEOUT:  return HOMIE_OUTCOME_TIMEOUT;
        case HOMIE_OBS_RELEASED: return HOMIE_OUTCOME_COMPLETED;
        default:                 return HOMIE_OUTCOME_NONE;
    }
}

inline bool homie_outcome_is_terminal(HomieOutcome o)
{
    return (o == HOMIE_OUTCOME_SUCCESS) || (o == HOMIE_OUTCOME_FAILED) ||
           (o == HOMIE_OUTCOME_TIMEOUT) || (o == HOMIE_OUTCOME_REJECTED) ||
           (o == HOMIE_OUTCOME_COMPLETED);
}

struct HomiePendingSlot
{
    bool          active;
    char          cmd_id[HOMIE_CMD_ID_MAX];
    unsigned long start_ms;
};

struct HomiePendingTable
{
    HomiePendingSlot slot[HOMIE_PENDING_CAP];
};

inline void homie_pending_init(HomiePendingTable* t)
{
    if(t == NULL) { return; }
    for(uint8_t i = 0; i < HOMIE_PENDING_CAP; ++i)
    {
        t->slot[i].active    = false;
        t->slot[i].cmd_id[0] = '\0';
        t->slot[i].start_ms  = 0;
    }
}

inline int homie_pending_find(const HomiePendingTable* t, const char* cmd_id)
{
    if(t == NULL || cmd_id == NULL || cmd_id[0] == '\0') { return -1; }
    for(uint8_t i = 0; i < HOMIE_PENDING_CAP; ++i)
    {
        if(t->slot[i].active && hp_eq(t->slot[i].cmd_id, cmd_id)) { return (int)i; }
    }
    return -1;
}

inline uint8_t homie_pending_count(const HomiePendingTable* t)
{
    if(t == NULL) { return 0; }
    uint8_t n = 0;
    for(uint8_t i = 0; i < HOMIE_PENDING_CAP; ++i) { if(t->slot[i].active) { ++n; } }
    return n;
}

// 插入：空 cmd_id / 重复 cmd_id / 表满 ⇒ false（**不静默覆盖、不无限增长**）
inline bool homie_pending_add(HomiePendingTable* t, const char* cmd_id, unsigned long now)
{
    if(t == NULL || cmd_id == NULL || cmd_id[0] == '\0') { return false; }
    if(homie_pending_find(t, cmd_id) >= 0) { return false; }   // 幂等：重复拒绝
    for(uint8_t i = 0; i < HOMIE_PENDING_CAP; ++i)
    {
        if(!t->slot[i].active)
        {
            t->slot[i].active = true;
            hp_copy(t->slot[i].cmd_id, sizeof(t->slot[i].cmd_id), cmd_id);
            t->slot[i].start_ms = now;
            return true;
        }
    }
    return false;   // 表满
}

inline bool homie_pending_remove(HomiePendingTable* t, const char* cmd_id)
{
    const int i = homie_pending_find(t, cmd_id);
    if(i < 0) { return false; }
    t->slot[i].active    = false;
    t->slot[i].cmd_id[0] = '\0';
    t->slot[i].start_ms  = 0;
    return true;
}

// 单槽推进：返回当前结论；**终态自动移除** ⇒ 不出现永久 pending。
//   obs == RUNNING 且已超时 ⇒ 兜底超时（TIMEOUT）并移除。
inline HomieOutcome homie_pending_tick(HomiePendingTable* t,
                                       const char* cmd_id,
                                       HomieObservedState obs,
                                       unsigned long now,
                                       unsigned long timeout_ms)
{
    const int i = homie_pending_find(t, cmd_id);
    if(i < 0) { return HOMIE_OUTCOME_NONE; }        // 未被跟踪（如入表失败）

    if(obs == HOMIE_OBS_RUNNING)
    {
        if(timeout_ms != 0 && (now - t->slot[i].start_ms) >= timeout_ms)
        {
            homie_pending_remove(t, cmd_id);        // Bridge 侧兜底看门狗
            return HOMIE_OUTCOME_TIMEOUT;
        }
        return HOMIE_OUTCOME_PENDING;
    }

    const HomieOutcome oc = homie_classify_observed(obs);
    if(homie_outcome_is_terminal(oc)) { homie_pending_remove(t, cmd_id); }
    return oc;
}

#endif // HOMIE_PROJECTION_H
