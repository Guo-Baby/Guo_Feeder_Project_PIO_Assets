#pragma once

// =====================================================
// xiaomi_lan_parse.h —— 米家 LAN 回包解析（**纯函数 / 零依赖**）
//
// 为什么单独抽出来：
//   解析逻辑原先混在 `xiaomi_lan.cpp` 的 static 函数里，依赖 ArduinoJson，
//   只能在真机上验证 ⇒ 异常路径（code 缺失 / 单属性错误码 / result 非数组 /
//   value 为 null / JSON 截断）无法自动化测试。
//   这里改成不依赖任何库的手写扫描器：既能在固件里跑，也能在**宿主侧**
//   （test/xiaomi_lan_parse_host）真实执行，正向与反向用例都可测。
//
// 协议事实（真机实测，见 docs/reports/2026-10-28-phaseI-*）：
//   成功回包：{"id":1,"result":[{"did":"p","siid":2,"piid":26,"code":0,"value":0}]}
//     ★ **顶层没有 `code`** —— 缺省必须视为 0（出错的回包才带 code）
//   出错回包：{"id":1,"code":-704220043,"message":"..."}
//
// 语义边界（上层必须遵守）：
//   · `ok`       = JSON 结构可解析（**不代表属性读到**）
//   · `code`     = 顶层设备错误码（缺省 0）
//   · `propCode` = result[0].code 单属性错误码（缺省 0）
//   · `value.hasValue` = 真的取到值；缺 value / null ⇒ false（**绝不能**当 0/false）
// =====================================================

namespace xlan
{

struct PropValue
{
    bool hasValue;
    bool isBool;     // true=布尔型，false=数值型
    bool boolVal;
    int  intVal;
};

struct Reply
{
    bool ok;
    /*
     * ★ [2026-11-05] `id` 回显：真机实测两台设备都会把请求 id 原样带回
     *   （请求 id=1234 → 回包 id=1234）。并行认证与单播验证都用它做
     *   「这条回包是不是回答我这次请求」的关联，从而排除跨轮次的迟到响应。
     *   缺失（hasId=false）时上层必须判为**不可关联**，不能退化成"随便认"。
     */
    bool hasId;
    int  id;
    bool hasCode;
    int  code;
    bool hasResult;
    int  propCode;
    PropValue value;
};

// ---------------------------------------------------------------------------
// 内部：极简扫描工具（仅处理本协议需要的子集；不接受任何非法结构）
// ---------------------------------------------------------------------------
namespace detail
{

inline void skipWs(const char *&p, const char *end)
{
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
}

/** 读一个 JSON 字符串（跳过转义），p 指向 '"'。返回 false = 未闭合 */
inline bool skipString(const char *&p, const char *end)
{
    if (p >= end || *p != '"') return false;
    p++;
    while (p < end)
    {
        if (*p == '\\') { p += 2; continue; }
        if (*p == '"') { p++; return true; }
        p++;
    }
    return false;
}

/** 读一个数字（整数/小数/指数），只要首字符像数字就吞掉整段 */
inline bool readNumber(const char *&p, const char *end, int &out)
{
    const char *s = p;
    bool neg = false;
    if (p < end && (*p == '-' || *p == '+')) { neg = (*p == '-'); p++; }
    bool any = false;
    long long v = 0;
    bool overflow = false;
    while (p < end && *p >= '0' && *p <= '9')
    {
        any = true;
        if (!overflow)
        {
            v = v * 10 + (*p - '0');
            if (v > 2147483647LL) overflow = true;
        }
        p++;
    }
    // 小数/指数部分：吞掉但不参与取值（属性值不会是小数字符串，容错处理）
    if (p < end && *p == '.')
    {
        p++;
        while (p < end && *p >= '0' && *p <= '9') p++;
    }
    if (p < end && (*p == 'e' || *p == 'E'))
    {
        p++;
        if (p < end && (*p == '-' || *p == '+')) p++;
        while (p < end && *p >= '0' && *p <= '9') p++;
    }
    if (!any) { p = s; return false; }
    if (overflow) { out = 2147483647; return true; }
    out = (int)(neg ? -v : v);
    return true;
}

/** 跳过任意一个 JSON 值（用于未知字段） */
inline bool skipValue(const char *&p, const char *end, int depth);

inline bool skipContainer(const char *&p, const char *end, char open, char close, int depth)
{
    if (depth > 8) return false;
    if (p >= end || *p != open) return false;
    p++;
    skipWs(p, end);
    if (p < end && *p == close) { p++; return true; }
    for (;;)
    {
        skipWs(p, end);
        if (p >= end) return false;
        if (!skipValue(p, end, depth + 1)) return false;
        skipWs(p, end);
        if (p >= end) return false;
        // ★ 对象里的 `"key": value` —— 跳过 key 后还必须吃掉 ':' 与第二个值，
        //   否则遇到 `{"extra":{"a":1}}` 这类嵌套对象会误判非法（D8 用例踩到过）。
        if (open == '{' && *p == ':')
        {
            p++;
            skipWs(p, end);
            if (!skipValue(p, end, depth + 1)) return false;
            skipWs(p, end);
            if (p >= end) return false;
        }
        if (*p == ',') { p++; continue; }
        if (*p == close) { p++; return true; }
        return false;
    }
}

inline bool skipValue(const char *&p, const char *end, int depth)
{
    skipWs(p, end);
    if (p >= end) return false;
    if (*p == '"') return skipString(p, end);
    if (*p == '{') return skipContainer(p, end, '{', '}', depth);
    if (*p == '[') return skipContainer(p, end, '[', ']', depth);
    if (*p == 't') { if (end - p >= 4 && p[1] == 'r' && p[2] == 'u' && p[3] == 'e') { p += 4; return true; } return false; }
    if (*p == 'f') { if (end - p >= 5 && p[1] == 'a' && p[2] == 'l' && p[3] == 's' && p[4] == 'e') { p += 5; return true; } return false; }
    if (*p == 'n') { if (end - p >= 4 && p[1] == 'u' && p[2] == 'l' && p[3] == 'l') { p += 4; return true; } return false; }
    int tmp;
    return readNumber(p, end, tmp);
}

/** 比较当前字符串内容是否为 key；返回时 p 停在字符串之后 */
inline bool keyIs(const char *start, const char *afterQuote, const char *key)
{
    // start 指向开引号后第一个字符；afterQuote 指向闭引号后
    const char *q = key;
    const char *r = start;
    while (*q && r < afterQuote - 1)
    {
        if (*r != *q) return false;
        r++; q++;
    }
    return *q == 0 && r == afterQuote - 1;
}

inline bool readBool(const char *&p, const char *end, bool &out)
{
    if (end - p >= 4 && p[0] == 't' && p[1] == 'r' && p[2] == 'u' && p[3] == 'e') { p += 4; out = true; return true; }
    if (end - p >= 5 && p[0] == 'f' && p[1] == 'a' && p[2] == 'l' && p[3] == 's' && p[4] == 'e') { p += 5; out = false; return true; }
    return false;
}

/** 解析一个属性对象 {"did":..,"siid":..,"piid":..,"code":..,"value":..} */
inline bool parsePropObject(const char *&p, const char *end, Reply &out)
{
    if (p >= end || *p != '{') return false;
    p++;
    skipWs(p, end);
    if (p < end && *p == '}') { p++; return true; }   // 空对象：无 code / 无 value

    for (;;)
    {
        skipWs(p, end);
        if (p >= end || *p != '"') return false;
        const char *ks = p + 1;
        if (!skipString(p, end)) return false;
        const char *ke = p;
        skipWs(p, end);
        if (p >= end || *p != ':') return false;
        p++;
        skipWs(p, end);

        if (keyIs(ks, ke, "code"))
        {
            int v = 0;
            if (!readNumber(p, end, v)) return false;
            out.propCode = v;
        }
        else if (keyIs(ks, ke, "value"))
        {
            // value 可能是 int / bool / string / null / 数组
            if (p < end && *p == 't' || (p < end && *p == 'f'))
            {
                bool b = false;
                if (!readBool(p, end, b)) return false;
                out.value.hasValue = true;
                out.value.isBool = true;
                out.value.boolVal = b;
                out.value.intVal = b ? 1 : 0;
            }
            else if (p < end && (*p == '-' || (*p >= '0' && *p <= '9')))
            {
                int v = 0;
                if (!readNumber(p, end, v)) return false;
                out.value.hasValue = true;
                out.value.isBool = false;
                out.value.intVal = v;
                out.value.boolVal = (v != 0);
            }
            else if (p < end && *p == '"')
            {
                const char *vs = p + 1;
                if (!skipString(p, end)) return false;
                // 数字字符串（部分固件把值序列化成 "0"）
                int v = 0;
                const char *q = vs;
                if (readNumber(q, p - 1, v) && q == p - 1)
                {
                    out.value.hasValue = true;
                    out.value.isBool = false;
                    out.value.intVal = v;
                    out.value.boolVal = (v != 0);
                }
                else
                {
                    out.value.hasValue = false;   // 非数字字符串 ⇒ 不当成值
                }
            }
            else
            {
                if (!skipValue(p, end, 1)) return false;
                out.value.hasValue = false;       // null / 数组 ⇒ 无值
            }
        }
        else
        {
            if (!skipValue(p, end, 1)) return false;
        }

        skipWs(p, end);
        if (p >= end) return false;
        if (*p == ',') { p++; continue; }
        if (*p == '}') { p++; return true; }
        return false;
    }
}

} // namespace detail

// ---------------------------------------------------------------------------
// 对外：解析一帧明文 JSON（不含尾部 0x00，长度由调用方给出）
// ---------------------------------------------------------------------------
inline bool parseReply(const char *json, int len, Reply &out)
{
    out.ok = false;
    out.hasId = false;
    out.id = 0;
    out.hasCode = false;
    out.code = 0;
    out.hasResult = false;
    out.propCode = 0;
    out.value.hasValue = false;
    out.value.isBool = false;
    out.value.boolVal = false;
    out.value.intVal = 0;

    if (!json || len <= 0) return false;
    const char *p = json;
    const char *end = json + len;

    detail::skipWs(p, end);
    if (p >= end || *p != '{') return false;
    p++;

    detail::skipWs(p, end);
    if (p < end && *p == '}') { p++; out.ok = true; return true; }   // 空对象：合法但无内容

    for (;;)
    {
        detail::skipWs(p, end);
        if (p >= end || *p != '"') return false;
        const char *ks = p + 1;
        if (!detail::skipString(p, end)) return false;
        const char *ke = p;
        detail::skipWs(p, end);
        if (p >= end || *p != ':') return false;
        p++;
        detail::skipWs(p, end);

        if (detail::keyIs(ks, ke, "code"))
        {
            int v = 0;
            if (!detail::readNumber(p, end, v)) return false;   // 类型错误 ⇒ 整帧判非法
            out.hasCode = true;
            out.code = v;
        }
        else if (detail::keyIs(ks, ke, "id"))
        {
            /*
             * ★ [2026-11-05] 请求/响应关联字段。
             *   只接受整数（本协议 id 恒为整数）；其它类型按"不可关联"处理，
             *   但**不判整帧非法** —— 否则老固件/异常回包会把有效属性一起丢掉。
             */
            int v = 0;
            if (detail::readNumber(p, end, v)) { out.hasId = true; out.id = v; }
            else { if (!detail::skipValue(p, end, 1)) return false; out.hasId = false; }
        }
        else if (detail::keyIs(ks, ke, "result"))
        {
            if (p >= end || *p != '[') return false;            // result 不是数组 ⇒ 非法
            p++;
            detail::skipWs(p, end);
            if (p < end && *p == ']') { p++; out.hasResult = false; }
            else
            {
                out.hasResult = true;
                if (!detail::parsePropObject(p, end, out)) return false;
                // 跳过 result 数组里其余元素（我们只请求一个属性）
                detail::skipWs(p, end);
                while (p < end && *p == ',')
                {
                    p++;
                    if (!detail::skipValue(p, end, 1)) return false;
                    detail::skipWs(p, end);
                }
                if (p >= end || *p != ']') return false;
                p++;
            }
        }
        else
        {
            if (!detail::skipValue(p, end, 1)) return false;
        }

        detail::skipWs(p, end);
        if (p >= end) return false;
        if (*p == ',') { p++; continue; }
        if (*p == '}') { p++; break; }
        return false;
    }

    // 顶层对象闭合后允许尾部空白，但不允许有垃圾（截断/拼接帧一律判非法）
    detail::skipWs(p, end);
    if (p != end) return false;

    out.ok = true;
    return true;
}

} // namespace xlan
