// =====================================================
// probe.cpp —— LAN 回包解析宿主侧探针（wasm32 freestanding + node）
//
// 只编译 **纯头文件** `src/app/xiaomi_lan_parse.h`（零依赖），因此这里执行的是
// 固件里**同一份**解析代码，而不是复制品。
//
// 导出：
//   probe_run()       → 失败用例数（0 = 全通过）
//   probe_log_ptr()   → 日志缓冲区起始
//   probe_log_len()   → 日志字节数
//
// 用例覆盖（对应任务 D 清单）：
//   1 顶层缺 code 的成功回包        2 整数 value       3 布尔 value
//   4 顶层非零错误码                5 单属性非零错误码  6 result 缺失
//   7 result 空数组                 8 result 类型错误   9 value 缺失
//   10 value null                  11 value 类型不匹配 12 JSON 截断
//   13 非对象根                    14 尾部垃圾        15 键顺序打乱+额外字段
//   16 数字字符串                   17 失败不覆盖上次有效值  18 online 与 last_result 语义
// =====================================================

#include "app/xiaomi_lan_parse.h"

namespace
{
char g_log[8192];
int  g_logLen = 0;
int  g_fail = 0;

void putc(char c)
{
    if (g_logLen < (int)sizeof(g_log) - 1) g_log[g_logLen++] = c;
}

void puts(const char *s)
{
    while (*s) putc(*s++);
}

void putInt(int v)
{
    char buf[12];
    int n = 0;
    if (v < 0) { putc('-'); v = -v; }
    if (v == 0) { putc('0'); return; }
    while (v > 0 && n < 11) { buf[n++] = (char)('0' + v % 10); v /= 10; }
    while (n > 0) putc(buf[--n]);
}

void check(bool cond, const char *name)
{
    puts(cond ? "  PASS  " : "  FAIL  ");
    puts(name);
    putc('\n');
    if (!cond) g_fail++;
}

/** 用固定长度调用（模拟"密文解出来的明文长度"） */
bool parseN(const char *json, xlan::Reply &out)
{
    int n = 0;
    while (json[n]) n++;
    return xlan::parseReply(json, n, out);
}

// ---------------------------------------------------------------------------
// 上层语义模拟：与 xiaomi_lan.cpp stage-2 的判定规则保持一致
// ---------------------------------------------------------------------------
struct SimState
{
    bool online;
    bool hasOut;
    int  outStatus;
    bool hasPower;
    bool power;
    int  lastResult;   // 0 未执行 / 1 成功 / 2 失败
};

void simInit(SimState &s)
{
    s.online = false; s.hasOut = false; s.outStatus = -1;
    s.hasPower = false; s.power = false; s.lastResult = 0;
}

/** kind: 0=读喂食器 1=读插座 */
void simApply(SimState &s, bool commOk, bool parsed, const xlan::Reply &r, int kind)
{
    if (!commOk || !parsed) { s.online = false; s.lastResult = 2; return; }
    s.online = true;                                   // 能解密 ⇒ 通信成功
    const bool okCode = (r.code == 0) && (r.propCode == 0);
    bool applied = false;
    if (okCode)
    {
        if (kind == 0 && r.value.hasValue) { s.hasOut = true; s.outStatus = r.value.intVal; applied = true; }
        else if (kind == 1 && r.value.hasValue) { s.hasPower = true; s.power = r.value.boolVal; applied = true; }
        else if (kind >= 2) applied = true;             // 控制类：设备受理
    }
    s.lastResult = (okCode && applied) ? 1 : 2;
}

} // namespace

extern "C"
{

int probe_run()
{
    xlan::Reply r;

    puts("D1 · 顶层缺 code 的成功回包（真机形态）\n");
    {
        const char *j = "{\"id\":1,\"result\":[{\"did\":\"p\",\"siid\":2,\"piid\":26,\"code\":0,\"value\":0}]}";
        bool ok = parseN(j, r);
        check(ok && r.ok, "可解析");
        check(ok && !r.hasCode && r.code == 0, "★ 顶层无 code ⇒ hasCode=false 且 code=0");
        check(ok && r.hasResult, "result 存在");
        check(ok && r.propCode == 0, "单属性 code=0");
        check(ok && r.value.hasValue && !r.value.isBool && r.value.intVal == 0, "★ int value=0 且 hasValue");
    }

    puts("D2 · 布尔 value（插座 power=false / true）\n");
    {
        const char *j = "{\"id\":2,\"result\":[{\"did\":\"p\",\"siid\":2,\"piid\":1,\"value\":false}]}";
        bool ok = parseN(j, r);
        check(ok && r.value.hasValue && r.value.isBool && r.value.boolVal == false, "bool false");
        const char *j2 = "{\"id\":3,\"result\":[{\"siid\":2,\"piid\":1,\"value\":true}]}";
        bool ok2 = parseN(j2, r);
        check(ok2 && r.value.hasValue && r.value.isBool && r.value.boolVal == true, "bool true");
    }

    puts("D3 · 顶层非零错误码（回包无 result）\n");
    {
        const char *j = "{\"id\":1,\"code\":-704220043,\"message\":\"invalid\"}";
        bool ok = parseN(j, r);
        check(ok && r.hasCode && r.code == -704220043, "★ 取到负错误码");
        check(ok && !r.hasResult && !r.value.hasValue, "无 result / 无 value");
    }

    puts("D4 · 单属性非零错误码\n");
    {
        const char *j = "{\"id\":1,\"result\":[{\"siid\":2,\"piid\":26,\"code\":-704010000}]}";
        bool ok = parseN(j, r);
        check(ok && r.code == 0 && r.propCode == -704010000, "★ propCode 被识别");
        check(ok && !r.value.hasValue, "无 value ⇒ hasValue=false");
    }

    puts("D5 · result 缺失 / 空数组 / 类型错误\n");
    {
        bool ok = parseN("{\"id\":1}", r);
        check(ok && !r.hasResult, "result 缺失 ⇒ hasResult=false");
        bool ok2 = parseN("{\"id\":1,\"result\":[]}", r);
        check(ok2 && !r.hasResult, "空数组 ⇒ hasResult=false");
        bool ok3 = parseN("{\"id\":1,\"result\":{\"value\":1}}", r);
        check(!ok3, "★ result 是对象 ⇒ 整帧判非法");
    }

    puts("D6 · value 缺失 / null / 类型不匹配\n");
    {
        bool ok = parseN("{\"id\":1,\"result\":[{\"siid\":2,\"piid\":26}]}", r);
        check(ok && r.hasResult && !r.value.hasValue, "★ value 缺失 ⇒ 不得当成 0");
        bool ok2 = parseN("{\"id\":1,\"result\":[{\"siid\":2,\"piid\":26,\"value\":null}]}", r);
        check(ok2 && !r.value.hasValue, "★ value=null ⇒ 无值");
        bool ok3 = parseN("{\"id\":1,\"result\":[{\"siid\":2,\"piid\":26,\"value\":[1,2]}]}", r);
        check(ok3 && !r.value.hasValue, "★ value 是数组 ⇒ 无值（不猜类型）");
        bool ok4 = parseN("{\"id\":1,\"result\":[{\"siid\":2,\"piid\":26,\"value\":\"abc\"}]}", r);
        check(ok4 && !r.value.hasValue, "★ value 非数字字符串 ⇒ 无值");
    }

    puts("D7 · JSON 截断 / 非对象根 / 尾部垃圾\n");
    {
        check(!parseN("{\"id\":1,\"result\":[{\"siid\":2,\"piid\":26,\"value\":0", r), "★ 截断（缺右括号）⇒ 非法");
        check(!parseN("[{\"value\":0}]", r), "非对象根 ⇒ 非法");
        check(!parseN("{\"id\":1}xx", r), "★ 尾部垃圾 ⇒ 非法");
        check(!xlan::parseReply("", 0, r), "空输入 ⇒ 非法");
        check(!xlan::parseReply(nullptr, 5, r), "空指针 ⇒ 非法");
    }

    puts("D8 · 键序打乱 + 额外字段 + 数字字符串\n");
    {
        const char *j = "{\"result\":[{\"value\":\"12\",\"code\":0,\"piid\":18,\"extra\":{\"a\":[1,2]},\"siid\":2}],\"id\":9}";
        bool ok = parseN(j, r);
        check(ok && r.value.hasValue && r.value.intVal == 12, "★ 乱序 + 额外嵌套字段仍能取到 12");
        check(ok && r.code == 0 && !r.hasCode, "顶层仍无 code");
    }

    puts("D9 · 语义：失败不覆盖上次有效值 / online 与 last_result\n");
    {
        SimState s; simInit(s);
        // 第一次：成功读到 out_status=3
        const char *j1 = "{\"id\":1,\"result\":[{\"siid\":2,\"piid\":26,\"code\":0,\"value\":3}]}";
        xlan::Reply r1; parseN(j1, r1);
        simApply(s, true, true, r1, 0);
        check(s.online && s.hasOut && s.outStatus == 3 && s.lastResult == 1, "首次成功 ⇒ online=1/值=3/last=1");

        // 第二次：通信成功但属性级错误 ⇒ 不得覆盖
        const char *j2 = "{\"id\":1,\"result\":[{\"siid\":2,\"piid\":26,\"code\":-704010000}]}";
        xlan::Reply r2; parseN(j2, r2);
        simApply(s, true, true, r2, 0);
        check(s.online, "★ 通信仍算成功（online=true）");
        check(s.lastResult == 2, "★ last_result=2（属性未取到）");
        check(s.outStatus == 3, "★ 上一次有效值 3 **未被覆盖**");

        // 第三次：value 缺失 ⇒ 同样不覆盖
        const char *j3 = "{\"id\":1,\"result\":[{\"siid\":2,\"piid\":26,\"code\":0}]}";
        xlan::Reply r3; parseN(j3, r3);
        simApply(s, true, true, r3, 0);
        check(s.outStatus == 3 && s.lastResult == 2, "★ value 缺失 ⇒ 不覆盖 + 判失败");

        // 第四次：解密/校验失败 ⇒ 离线
        simApply(s, false, false, r3, 0);
        check(!s.online && s.lastResult == 2, "★ 通信失败 ⇒ online=false / last=2");
        check(s.outStatus == 3, "离线也不清掉已有值");

        // 插座：bool 值必须按布尔落库
        const char *p1 = "{\"id\":2,\"result\":[{\"siid\":2,\"piid\":1,\"value\":true}]}";
        xlan::Reply pr; parseN(p1, pr);
        SimState s2; simInit(s2);
        simApply(s2, true, true, pr, 1);
        check(s2.hasPower && s2.power == true && s2.lastResult == 1, "插座 true 正确落库");
    }

    puts(g_fail == 0 ? "\n== ALL PASS ==\n" : "\n== HAS FAILURES ==\n");
    return g_fail;
}

const char *probe_log_ptr() { return g_log; }
int probe_log_len() { return g_logLen; }

} // extern "C"
