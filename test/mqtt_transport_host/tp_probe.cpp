// =====================================================
// P2-2 · MQTT Transport —— wasm32 宿主探针（无硬件 / 无 CRT，真执行）
//
// 用途：在宿主机上**真实运行** mqtt_routing.h 的纯逻辑，输出 PASS/FAIL 数字：
//   · B 订阅注册表（新增 / 幂等 / 容量 / 超长拒绝）
//   · C topic 分派矩阵（业务 / foreign / 未注册 / 互不误判）
//   · D 分片重组（单帧 / 多帧 / 不串包 / retain 保留 / 超限丢弃）
//
// 与生产共用**同一份** src/cloud/mqtt_routing.h（单一真源）。
// 编译见 run.sh（clang --target=wasm32 -nostdlib）。
// =====================================================

#include "cloud/mqtt_routing.h"

// ---- freestanding 运行期支撑（避免 libcall） ----
extern "C" void* memcpy(void* dst, const void* src, unsigned long n)
{
    unsigned char* d = (unsigned char*)dst;
    const unsigned char* s = (const unsigned char*)src;
    for (unsigned long i = 0; i < n; ++i) { d[i] = s[i]; }
    return dst;
}
extern "C" void* memset(void* dst, int c, unsigned long n)
{
    unsigned char* d = (unsigned char*)dst;
    for (unsigned long i = 0; i < n; ++i) { d[i] = (unsigned char)c; }
    return dst;
}

// ---- 结果记录 ----
static char g_log[32768];
static int  g_log_len = 0;
static int  g_pass = 0;
static int  g_fail = 0;

static void put(const char* s)
{
    while (*s != '\0' && g_log_len < (int)sizeof(g_log) - 1) { g_log[g_log_len++] = *s++; }
}
static void put_u32(unsigned int v)
{
    char b[12];
    int n = 0;
    if (v == 0) { b[n++] = '0'; }
    while (v > 0) { b[n++] = (char)('0' + (v % 10)); v /= 10; }
    for (int i = n - 1; i >= 0; --i)
    {
        if (g_log_len < (int)sizeof(g_log) - 1) { g_log[g_log_len++] = b[i]; }
    }
}
static void section(const char* name) { put("\n== "); put(name); put(" ==\n"); }

static void check(bool ok, const char* name)
{
    if (ok) { ++g_pass; put("  PASS  "); put(name); put("\n"); }
    else    { ++g_fail; put("  FAIL  "); put(name); put("\n"); }
}

static bool eq(const char* a, const char* b) { return mqtt_cstr_eq(a, b); }

// 比较 buf 的前 n 字节是否等于字面量
static bool bytes_eq(const uint8_t* buf, size_t n, const char* lit)
{
    for (size_t i = 0; i < n; ++i)
    {
        if (buf[i] != (uint8_t)lit[i]) { return false; }
    }
    return (lit[n] == '\0');
}

// 构造一个长度 len 的 'a' 字符串（含结束符位置由调用方保证）
static void fill(char* dst, int len, char c)
{
    for (int i = 0; i < len; ++i) { dst[i] = c; }
    dst[len] = '\0';
}

extern "C" int probe_run();
extern "C" int probe_log_len();
extern "C" const char* probe_log_ptr();

int probe_run()
{
    // =================================================
    section("A. topic filter 匹配");
    // =================================================
    check(mqtt_topic_matches_filter("a/b", "a/b"),          "exact match");
    check(!mqtt_topic_matches_filter("a/b", "a/c"),         "exact mismatch");
    check(mqtt_topic_matches_filter("a/+", "a/b"),          "'+' matches one level");
    check(!mqtt_topic_matches_filter("a/+", "a/b/c"),       "'+' not two levels");
    check(mqtt_topic_matches_filter("a/#", "a"),            "'#' matches parent (0 levels)");
    check(mqtt_topic_matches_filter("a/#", "a/b/c"),        "'#' matches many");
    check(mqtt_topic_matches_filter("#", "x/y"),            "'#' matches all");
    check(mqtt_topic_matches_filter("+/b", "a/b"),          "leading '+'");
    check(!mqtt_topic_matches_filter("a/b", "a/b/c"),       "shorter filter");
    check(!mqtt_topic_matches_filter("a/b/c", "a/b"),       "longer filter");
    check(!mqtt_topic_matches_filter(NULL, "a"),            "null filter");
    check(!mqtt_topic_matches_filter("a", NULL),            "null topic");

    // 真实形态
    const char* biz  = "guo_feeder/a5a0000000a1/down";
    const char* hom  = "homie/5/a5a0000000a1/+/+/set";
    check(mqtt_topic_matches_filter(biz, "guo_feeder/a5a0000000a1/down"), "business filter matches business topic");
    check(!mqtt_topic_matches_filter(biz, "guo_feeder/a5a0000000a1/up"),  "business filter not up");
    check(mqtt_topic_matches_filter(hom, "homie/5/a5a0000000a1/water/valve/set"), "homie filter matches valve/set");
    check(!mqtt_topic_matches_filter(hom, "homie/5/a5a0000000a1/water/valve"),    "homie filter not bare property");
    check(!mqtt_topic_matches_filter(hom, "guo_feeder/a5a0000000a1/down"),        "homie filter not business");

    // =================================================
    section("B. 订阅注册表（幂等 / 容量 / 超长）");
    // =================================================
    MqttSubRegistry reg;
    reg.init();
    check(reg.count == 0, "init -> count 0");
    check(reg.find("x") < 0, "find on empty -> -1");

    check(reg.add("homie/5/A/+/+/set", 1), "add #1");
    check(reg.count == 1, "count == 1");
    check(reg.add("homie/5/A/+/+/set", 1), "re-add same -> true (idempotent)");
    check(reg.count == 1, "count still 1 (no dup entry)");
    check(reg.find("homie/5/A/+/+/set") == 0, "find #1 -> index 0");

    check(reg.add("f/2", 0), "add #2");
    check(reg.add("f/3", 1), "add #3");
    check(reg.add("f/4", 1), "add #4 (at capacity)");
    check(reg.count == MQTT_ROUTE_MAX_FOREIGN, "count == MAX");
    check(!reg.add("f/5", 1), "add #5 -> false (full)");
    check(reg.count == MQTT_ROUTE_MAX_FOREIGN, "count unchanged after full reject");

    check(!reg.add(NULL, 1), "add NULL -> false");
    check(!reg.add("", 1),   "add empty -> false");

    {
        char big[MQTT_ROUTE_FILTER_MAX + 8];
        fill(big, MQTT_ROUTE_FILTER_MAX, 'a');   // len == MAX -> reject (>= MAX)
        MqttSubRegistry r2;
        r2.init();
        check(!r2.add(big, 1), "oversize filter rejected (no silent truncation)");
        check(r2.count == 0, "no entry added for oversize");
    }

    // =================================================
    section("C. topic 分派矩阵");
    // =================================================
    MqttSubRegistry fr;
    fr.init();
    check(fr.add(hom, 1), "register homie filter");
    const char* bizf = biz;

    int idx = -123;
    check(mqtt_route_topic("guo_feeder/a5a0000000a1/down", bizf, fr) == MQTT_ROUTE_BUSINESS, "business topic -> BUSINESS");
    check(mqtt_route_topic("homie/5/a5a0000000a1/water/valve/set", bizf, fr, &idx) == MQTT_ROUTE_FOREIGN, "foreign topic -> FOREIGN");
    check(idx == 0, "foreign index == 0");
    check(mqtt_route_topic("unregistered/x", bizf, fr) == MQTT_ROUTE_DROP, "unregistered -> DROP");
    check(mqtt_route_topic("guo_feeder/a5a0000000a1/up", bizf, fr) == MQTT_ROUTE_DROP, "business-sibling (up) -> DROP (not business, not foreign)");
    check(mqtt_route_topic("homie/5/a5a0000000a1/water/valve", bizf, fr) == MQTT_ROUTE_DROP, "bare homie property -> DROP");
    check(mqtt_route_topic(NULL, bizf, fr) == MQTT_ROUTE_DROP, "null topic -> DROP");

    // 互不误判：业务 topic 不得被判为 foreign
    check(mqtt_route_topic("guo_feeder/a5a0000000a1/down", bizf, fr) != MQTT_ROUTE_FOREIGN, "business never FOREIGN");
    // foreign topic 不得被判为 business
    check(mqtt_route_topic("homie/5/a5a0000000a1/water/valve/set", bizf, fr) != MQTT_ROUTE_BUSINESS, "foreign never BUSINESS");

    // 无 foreign 注册时的行为（P2-2 实况：仅业务订阅）
    {
        MqttSubRegistry empty;
        empty.init();
        check(mqtt_route_topic("guo_feeder/a5a0000000a1/down", bizf, empty) == MQTT_ROUTE_BUSINESS, "no-foreign: business still BUSINESS");
        check(mqtt_route_topic("homie/5/a5a0000000a1/water/valve/set", bizf, empty) == MQTT_ROUTE_DROP, "no-foreign: foreign topic DROPPED (not subscribed)");
    }

    // =================================================
    section("D. 分片重组（单帧 / 多帧 / 不串包 / retain / 超限）");
    // =================================================
    static uint8_t abuf[256];
    MqttRxAssembler as;
    as.init(abuf, sizeof(abuf));

    // --- 单帧 ---
    {
        const char* pl = "hello";
        int rc = as.feed(0, 5, "t/a", 3, false, (const uint8_t*)pl, 5);
        check(rc == 1, "single-frame completes immediately");
        check(as.len() == 5, "single-frame len == 5");
        check(eq(as.topic_cstr(), "t/a"), "single-frame topic captured");
        check(bytes_eq(as.data(), as.len(), "hello"), "single-frame payload correct");
        check(as.retain_flag() == false, "single-frame retain=false");
        as.reset();
    }

    // --- 多帧（第 2 片无 topic，模拟 esp-mqtt 契约）---
    {
        int rc1 = as.feed(0, 10, "t/b", 3, true, (const uint8_t*)"01234", 5);
        check(rc1 == 0, "multi-frame frag1 -> incomplete");
        int rc2 = as.feed(5, 10, NULL, 0, true, (const uint8_t*)"56789", 5);
        check(rc2 == 1, "multi-frame frag2 -> complete");
        check(as.len() == 10, "multi-frame len == 10");
        check(eq(as.topic_cstr(), "t/b"), "multi-frame topic from frag1 retained");
        check(bytes_eq(as.data(), as.len(), "0123456789"), "multi-frame payload reassembled");
        check(as.retain_flag() == true, "multi-frame retain=true preserved");
        as.reset();
    }

    // --- 不串包：先 business 多帧，再 foreign 单帧 ---
    {
        // m1: business, 2 frames
        check(as.feed(0, 6, "guo_feeder/A/down", 17, false, (const uint8_t*)"AAA", 3) == 0, "m1 frag1 incomplete");
        check(as.feed(3, 6, NULL, 0, false, (const uint8_t*)"BBB", 3) == 1, "m1 frag2 complete");
        check(eq(as.topic_cstr(), "guo_feeder/A/down"), "m1 topic is business");
        check(bytes_eq(as.data(), as.len(), "AAABBB"), "m1 payload not corrupted");
        as.reset();

        // m2: foreign, 1 frame — must not inherit m1's topic
        check(as.feed(0, 3, "homie/5/A/x/y/set", 17, true, (const uint8_t*)"OPN", 3) == 1, "m2 foreign complete");
        check(eq(as.topic_cstr(), "homie/5/A/x/y/set"), "m2 topic is foreign (no cross-talk)");
        check(bytes_eq(as.data(), as.len(), "OPN"), "m2 payload correct (no cross-talk)");
        check(as.retain_flag() == true, "m2 retain correct");
        as.reset();
    }

    // --- retain 逐条独立 ---
    {
        check(as.feed(0, 1, "r/1", 3, true, (const uint8_t*)"x", 1) == 1, "retain msg1 done");
        check(as.retain_flag() == true, "retain msg1 == true");
        as.reset();
        check(as.feed(0, 1, "r/2", 3, false, (const uint8_t*)"y", 1) == 1, "retain msg2 done");
        check(as.retain_flag() == false, "retain msg2 == false");
        as.reset();
    }

    // --- 超限丢弃（不越界）---
    {
        int rc = as.feed(0, (int)sizeof(abuf) + 1, "big", 3, false, (const uint8_t*)"z", 1);
        check(rc == 2, "oversize -> drop(2)");
        // 丢弃后仍可正常重组下一条
        check(as.feed(0, 2, "ok", 2, false, (const uint8_t*)"hi", 2) == 1, "recovers after drop");
        check(eq(as.topic_cstr(), "ok"), "recovered topic clean");
        as.reset();
    }

    // --- data 为 null / len<=0 ⇒ 忽略（不崩溃）---
    {
        check(as.feed(0, 5, "t", 1, false, NULL, 5) == 0, "null data ignored");
        check(as.feed(0, 5, "t", 1, false, (const uint8_t*)"", 0) == 0, "zero data_len ignored");
    }

    put("\n----\npass=");
    put_u32((unsigned)g_pass);
    put(" fail=");
    put_u32((unsigned)g_fail);
    put("\n");
    return g_fail;
}

extern "C" int probe_log_len() { return g_log_len; }
extern "C" const char* probe_log_ptr() { return g_log; }
