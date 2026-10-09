// =====================================================
// P2-7 · Homie 软件侧制品 Dump（wasm32 + node；无 CRT / 无硬件 / 无 broker）
//
// 与 hb_probe.cpp 同属「纯逻辑层宿主验证」，但**只做输出、不做判定**：
//   把 P2-7 §4 要求的软件侧制品**逐字节打印出来**，供 readiness report 引用：
//     ① topic renderer（base / $state / $description / set filter）
//     ② Homie discovery payload 静态生成（$description JSON）
//     ③ $description 格式与体积（16 属性 / 13 属性两态）
//     ④ node / property ID 合法性（homie_id_valid）
//     ⑤ set topic parser（homie_parse_set_topic 的 node/prop 结果）
//     ⑥ 只读属性枚举映射 + 可写面命令解析（dm_resolve_command）
//
// ★ 边界（与 hb_probe 一致）：**不**证明真机 / broker 行为。
//   真机 $description · LWT · retained replay · ACL 一律仍为 NOT RUN。
// =====================================================

#include "homie/homie_projection.h"
#include "homie/device_model.h"

// ---------------- 输出缓冲（无 libc） ----------------
static char g_log[16384];
static int  g_log_len = 0;

static void wr(const char* s)
{
    if(s == NULL) { return; }
    for(const char* p = s; *p != '\0'; ++p)
    {
        if(g_log_len < (int)(sizeof(g_log) - 1)) { g_log[g_log_len++] = *p; }
    }
}

static void wr_ch(char c)
{
    if(g_log_len < (int)(sizeof(g_log) - 1)) { g_log[g_log_len++] = c; }
}

static void wr_u32(unsigned v)
{
    char b[12];
    int  n = 0;
    if(v == 0) { b[n++] = '0'; }
    while(v > 0) { b[n++] = (char)('0' + (v % 10u)); v /= 10u; }
    for(int i = n - 1; i >= 0; --i) { wr_ch(b[i]); }
}

// 打印至多 max 字符，其余用 "…" 省略
static void wr_capped(const char* s, int max)
{
    int i = 0;
    for(const char* p = s; *p != '\0' && i < max; ++p, ++i) { wr_ch(*p); }
    if(s != NULL)
    {
        int total = 0;
        for(const char* p = s; *p != '\0'; ++p) { ++total; }
        if(total > max) { wr(" ...(+"); wr_u32((unsigned)(total - max)); wr(" more)"); }
    }
}

// 子串查找（**仅测试用**；无 libc）
static int str_find(const char* hay, const char* needle)
{
    if(hay == NULL || needle == NULL) { return -1; }
    const size_t hn = hp_len(hay);
    const size_t nn = hp_len(needle);
    if(nn == 0 || nn > hn) { return -1; }
    for(size_t i = 0; i + nn <= hn; ++i)
    {
        size_t k = 0;
        while(k < nn && hay[i + k] == needle[k]) { ++k; }
        if(k == nn) { return (int)i; }
    }
    return -1;
}

// ---------------- 可用性谓词 ----------------
static bool g_disabled_weight = false;

static bool avail_fn(const char* action_id, void* ctx)
{
    (void)ctx;
    if(action_id == NULL) { return false; }
    if(g_disabled_weight && hp_eq(action_id, "WEIGHT_ZERO")) { return false; }
    return true;
}

static char g_desc[HOMIE_DESC_CAP];

// 打印一行 property 摘要
static void dump_property(const DeviceModelProperty* p)
{
    if(p == NULL) { return; }
    wr("  prop ");   wr(p->node); wr("/"); wr(p->id);
    wr(" dtype=");   wr(homie_datatype_name(p->dtype));
    wr(" settable="); wr(p->settable ? "1" : "0");
    wr(" retained="); wr(p->retained ? "1" : "0");
    wr(" format=\""); wr(p->format); wr("\"");
    if(p->unit[0] != '\0') { wr(" unit=\""); wr(p->unit); wr("\""); }
    wr(" cmds=");    wr_u32(p->cmd_count);
    wr(" id_valid="); wr(homie_id_valid(p->id) ? "1" : "0");
    wr("\n");
}

extern "C" void probe_main()
{
    const char* DEV = "a5a0000000a1";

    // ---------- ① topic renderer ----------
    {
        char base[HOMIE_TOPIC_MAX];
        char t[HOMIE_TOPIC_MAX];
        wr("== 1. topic renderer ==\n");
        wr("basis            : homie/5/ + <device_id> + '/'\n");
        if(homie_base_topic(DEV, base, sizeof(base)))
        { wr("base_topic       : "); wr(base); wr("\n"); }
        if(homie_state_topic(base, t, sizeof(t)))
        { wr("state_topic      : "); wr(t); wr("\n"); }
        if(homie_description_topic(base, t, sizeof(t)))
        { wr("description_topic: "); wr(t); wr("\n"); }
        if(homie_set_filter(base, t, sizeof(t)))
        {
            wr("set_filter       : "); wr(t); wr("\n");
            wr("set_filter_has_# : "); wr(t[0] != '\0' && (t[hp_len(t) - 1] == '#') ? "1" : "0"); wr("\n");
        }
    }

    // ---------- ②③ discovery payload / $description ----------
    {
        wr("\n== 2. Homie discovery payload ($description, static generation) ==\n");
        HomieDescInput in;
        in.device_id       = DEV;
        in.buf             = g_desc;
        in.cap             = sizeof(g_desc);
        in.avail           = avail_fn;
        in.avail_ctx       = NULL;
        in.action_version  = 7;
        in.trigger_version = 3;

        g_disabled_weight = false;
        size_t len = 0; uint32_t ver = 0;
        bool ok = homie_desc_build(in, &len, &ver);
        wr("desc_build_ok    : "); wr(ok ? "1" : "0"); wr("\n");
        wr("desc_len(16prop) : "); wr_u32((unsigned)len); wr(" B  (cap="); wr_u32(HOMIE_DESC_CAP);
        wr(" target="); wr_u32(HOMIE_DESC_TARGET); wr(")\n");
        wr("desc_version     : "); wr_u32(ver); wr("\n");
        wr("desc_head        : "); wr_capped(g_desc, 220); wr("\n");
        wr("desc_key_order   : homie,version,nodes  ==> ");
        wr(str_find(g_desc, "\"homie\":\"5.0\"") >= 0 ? "ok" : "MISMATCH"); wr("\n");
    }

    // ---------- ③ 两态体积 ----------
    {
        wr("\n== 3. $description volume under availability filter ==\n");
        HomieDescInput in;
        in.device_id = DEV; in.buf = g_desc; in.cap = sizeof(g_desc);
        in.avail = avail_fn; in.avail_ctx = NULL;
        in.action_version = 7; in.trigger_version = 3;

        g_disabled_weight = false;
        size_t len_a = 0; uint32_t va = 0;
        homie_desc_build(in, &len_a, &va);
        g_disabled_weight = true;
        size_t len_b = 0; uint32_t vb = 0;
        homie_desc_build(in, &len_b, &vb);
        g_disabled_weight = false;

        wr("with weight.enable=true  : desc_len="); wr_u32((unsigned)len_a); wr("\n");
        wr("with weight.enable=false : desc_len="); wr_u32((unsigned)len_b); wr("\n");
    }

    // ---------- ④ node / property ID 合法性 ----------
    {
        wr("\n== 4. node / property inventory & ID legality ==\n");
        const uint8_t nc = dm_node_count();
        wr("node_count       : "); wr_u32(nc); wr("\n");
        for(uint8_t i = 0; i < nc; ++i)
        {
            const DeviceModelNode* n = dm_node_at(i);
            if(n == NULL) { continue; }
            wr("  node "); wr(n->id);
            wr(" name=\""); wr(n->name); wr("\"");
            wr(" type=\""); wr(n->type); wr("\"");
            wr(" id_valid="); wr(homie_id_valid(n->id) ? "1" : "0");
            wr("\n");
        }
        const uint8_t pc = dm_property_count();
        wr("property_count   : "); wr_u32(pc); wr("\n");
        for(uint8_t k = 0; k < pc; ++k) { dump_property(dm_property_at(k)); }
    }

    // ---------- ⑤ set topic parser ----------
    {
        wr("\n== 5. set topic parser (homie_parse_set_topic) ==\n");
        char base[HOMIE_TOPIC_MAX];
        homie_base_topic(DEV, base, sizeof(base));
        struct { const char* topic; } cases[] = {
            { "homie/5/a5a0000000a1/water/valve/set" },
            { "homie/5/a5a0000000a1/water/weight/set" },
            { "homie/5/ffffffffffff/water/valve/set" },   // 他机 namespace
            { "homie/5/a5a0000000a1/water/valve" },       // 缺 /set 段
            { "homie/5/a5a0000000a1/$state" },            // 非 set 面
        };
        for(unsigned i = 0; i < sizeof(cases)/sizeof(cases[0]); ++i)
        {
            char node[32]; char prop[32];
            bool ok = homie_parse_set_topic(cases[i].topic, base,
                                            node, sizeof(node), prop, sizeof(prop));
            wr("  "); wr(cases[i].topic);
            wr("  => accepted="); wr(ok ? "1" : "0");
            if(ok) { wr(" node="); wr(node); wr(" prop="); wr(prop); }
            wr("\n");
        }
        char own[64];
        wr("  homie_topic_is_own(own base)  = ");
        wr(homie_topic_is_own("homie/5/a5a0000000a1/water/valve/set", base) ? "1" : "0"); wr("\n");
        (void)own;
    }

    // ---------- ⑥ 只读枚举 + 可写面解析 ----------
    {
        wr("\n== 6. readonly enum mapping & writable command resolution ==\n");
        char v[HOMIE_VALUE_MAX]; size_t vl = 0;
        const DeviceModelProperty* p = dm_find("water", "valve");
        HomieSample s; s.valid = true; s.f = 0.0f;
        s.i = 1;
        homie_format_value(p, s, v, sizeof(v), &vl);
        wr("  water/valve state=1 -> \""); wr(v); wr("\"\n");
        s.i = 0;
        homie_format_value(p, s, v, sizeof(v), &vl);
        wr("  water/valve state=0 -> \""); wr(v); wr("\"\n");

        wr("  settle \"open\"  -> validate="); wr_u32((unsigned)dm_validate_set(p, "open"));
        const DeviceModelCommand* c = NULL;
        if(dm_resolve_command(p, "open", &c) && c != NULL)
        { wr(" cmd="); wr(c->action_id); wr(" payload="); wr(c->payload_json); }
        wr("\n");
        wr("  settle \"bogus\" -> validate="); wr_u32((unsigned)dm_validate_set(p, "bogus"));
        c = NULL;
        wr(" resolve="); wr(dm_resolve_command(p, "bogus", &c) ? "1" : "0"); wr("\n");

        // 只读属性必须拒绝写入
        p = dm_find("water", "weight");
        wr("  readonly water/weight settle \"1\" -> validate=");
        wr_u32((unsigned)dm_validate_set(p, "1")); wr("\n");
        wr("  readonly water/weight settle \"1\" -> resolve=");
        c = NULL;
        wr(dm_resolve_command(p, "1", &c) ? "1" : "0"); wr("\n");
    }
}

// ---------------- 导出（契约与 run_probe.js 一致）----------------
extern "C" int        probe_run()
{
    probe_main();
    wr("\n---- homie software artifacts dump ----\nlog_bytes=");
    wr_u32((unsigned)g_log_len);
    wr("\n");
    return 0;
}
extern "C" int        probe_log_len()  { return g_log_len; }
extern "C" const char* probe_log_ptr() { return g_log; }
