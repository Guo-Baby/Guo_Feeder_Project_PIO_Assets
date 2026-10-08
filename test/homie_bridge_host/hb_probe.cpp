// =====================================================
// P2-3 · Homie Projection —— 宿主探针（wasm32 + node；无 CRT / 无硬件 / 无 broker）
//
// 编译：clang++ -target wasm32 -std=gnu++17 -nostdlib -fno-builtin -O1 -I src
//       （+ freestanding Arduino 垫片，仅供 device_model.h 的类型声明）
// 运行：node test/device_model_host/run_probe.js <wasm>
//
// 目的：在 PC 上**真实执行**投影层的全部可判定逻辑：
//   序列化 · 映射 · 校验 · 路由 · 投影 · 可用性 · 快照
//
// ★ 明确边界（任务 §四十）：本探针**不能**证明下列真机 / broker 行为，
//   这些一律记为 NOT RUN（不写 PASS）：
//     EMQX retained replay · MQTT broker ACL · 真机 /set ·
//     真机 $description · 真机 LWT · 真机 reconnect
// =====================================================

#include "homie/homie_projection.h"
#include "homie/device_model.h"

// ---------------- 断言框架（无 libc） ----------------
static int  g_pass = 0;
static int  g_fail = 0;
static char g_log[8192];
static int  g_log_len = 0;

static void wr(const char* s)
{
    if(s == NULL) { return; }
    for(const char* p = s; *p != '\0'; ++p)
    {
        if(g_log_len < (int)(sizeof(g_log) - 1)) { g_log[g_log_len++] = *p; }
    }
}

static void CK(bool ok, const char* name)
{
    if(ok) { ++g_pass; }
    else   { ++g_fail; wr("FAIL: "); wr(name); wr("\n"); }
}

static void CKSTR(const char* got, const char* want, const char* name)
{
    if(hp_eq(got, want)) { ++g_pass; }
    else
    {
        ++g_fail;
        wr("FAIL: "); wr(name);
        wr(" got=");  wr(got);
        wr(" want="); wr(want); wr("\n");
    }
}

// 子串查找（**仅测试用**；不进固件）
static bool str_has(const char* hay, const char* needle)
{
    if(hay == NULL || needle == NULL) { return false; }
    const size_t hn = hp_len(hay);
    const size_t nn = hp_len(needle);
    if(nn == 0 || nn > hn) { return false; }
    for(size_t i = 0; i + nn <= hn; ++i)
    {
        size_t k = 0;
        while(k < nn && hay[i + k] == needle[k]) { ++k; }
        if(k == nn) { return true; }
    }
    return false;
}

// =====================================================
// 可用性谓词（模拟 Capability Registry）
//
//   disabled_weight = true  ⇒ WEIGHT_ZERO 未注册（weight.enable=false，§12.1）
//   no_mi           = true  ⇒ 米家 scan Action 未注册
// =====================================================
static bool g_disabled_weight = false;
static bool g_no_mi           = false;

static bool avail_fn(const char* action_id, void* ctx)
{
    (void)ctx;
    if(action_id == NULL) { return false; }
    if(g_disabled_weight && hp_eq(action_id, "WEIGHT_ZERO"))            { return false; }
    if(g_no_mi && (hp_eq(action_id, "MI_THERMO_START_SCAN") ||
                   hp_eq(action_id, "MI_THERMO_STOP_SCAN")))            { return false; }
    return true;
}

// 统计当前可用 node / property 数
static void count_available(int* out_nodes, int* out_props)
{
    int nodes = 0;
    int props = 0;
    const uint8_t nc = dm_node_count();
    for(uint8_t i = 0; i < nc; ++i)
    {
        const DeviceModelNode* n = dm_node_at(i);
        if(n == NULL) { continue; }
        int cnt = 0;
        const uint8_t pc = dm_property_count();
        for(uint8_t k = 0; k < pc; ++k)
        {
            const DeviceModelProperty* p = dm_property_at(k);
            if(p == NULL) { continue; }
            if(!hp_eq(p->node, n->id)) { continue; }
            if(homie_property_available(p, avail_fn, NULL)) { ++cnt; }
        }
        if(cnt > 0) { ++nodes; props += cnt; }
    }
    *out_nodes = nodes;
    *out_props = props;
}

// =====================================================
// 测试用例
// =====================================================
static char g_desc[HOMIE_DESC_CAP];

extern "C" void probe_main()
{
    // ----------------------------------------------------
    // A. Description（基础）
    // ----------------------------------------------------
    {
        HomieDescInput in;
        in.device_id       = "a5a0000000a1";
        in.buf             = g_desc;
        in.cap             = sizeof(g_desc);
        in.avail           = avail_fn;
        in.avail_ctx       = NULL;
        in.action_version  = 7;
        in.trigger_version = 3;

        size_t   len = 0;
        uint32_t ver = 0;
        CK(homie_desc_build(in, &len, &ver), "A: desc build ok");
        CK(len > 0 && len < sizeof(g_desc), "A: desc length sane");
        CK(len <= HOMIE_DESC_TARGET, "A: desc within 2048 target");
        CK(ver != 0, "A: version non-zero");

        // base / identity
        char base[HOMIE_TOPIC_MAX];
        CK(homie_base_topic("a5a0000000a1", base, sizeof(base)), "A: base build");
        CKSTR(base, "homie/5/a5a0000000a1/", "A: base topic value");

        // 结构要点
        const char* d = g_desc;
        CK(str_has(d, "\"homie\":\"5.0\""), "A: homie=5.0");
        CK(str_has(d, "\"nodes\":{"), "A: nodes object");
        CK(str_has(d, "\"water\""), "A: water node present");
        CK(str_has(d, "\"climate\""), "A: climate node present");
        CK(str_has(d, "\"system\""), "A: system node present");
        CK(str_has(d, "\"control\""), "A: control node present");

        // property 级字段
        CK(str_has(d, "\"valve\":{\"datatype\":\"enum\""), "A: valve datatype enum");
        CK(str_has(d, "\"format\":\"open,close\""), "A: valve format");
        CK(str_has(d, "\"settable\":true"), "A: valve settable true");
        CK(str_has(d, "\"weight\":{\"datatype\":\"float\""), "A: weight datatype float");
        CK(str_has(d, "\"unit\":\"g\""), "A: weight unit g");
        CK(str_has(d, "\"unit\":\"°C\""), "A: temperature unit C");
        CK(str_has(d, "\"humidity\":{\"datatype\":\"float\",\"name\":\"湿度\",\"format\":\"0:100\",\"unit\":\"%\",\"settable\":false,\"retained\":true}"),
           "A: humidity exact shape");

        // ★ 排除项（§七 / §11.2 AD-9）
        CK(!str_has(d, "workflow"), "A: workflow must NOT appear");
        CK(!str_has(d, "stable_id"), "A: stable_id must NOT appear");
        CK(!str_has(d, "password"), "A: password must NOT appear");
        CK(!str_has(d, "BLE_key"), "A: BLE key must NOT appear");
        CK(!str_has(d, "\"version\":0"), "A: version must not be 0");
    }

    // ----------------------------------------------------
    // A2. Retained 语义（来自 Device Model，非硬编码）
    // ----------------------------------------------------
    {
        const char* d = g_desc;
        // valve 段内必须 retained:true；zero 段内 retained:false
        CK(str_has(d, "\"valve\":{\"datatype\":\"enum\",\"name\":\"电磁阀\",\"format\":\"open,close\",\"settable\":true,\"retained\":true}"),
           "A2: valve retained true (exact)");
        CK(str_has(d, "\"zero\":{\"datatype\":\"boolean\",\"name\":\"零点校准\",\"settable\":true,\"retained\":false}"),
           "A2: zero retained false (exact)");
        CK(str_has(d, "\"scan\":{\"datatype\":\"enum\",\"name\":\"扫描\",\"format\":\"start,stop\",\"settable\":true,\"retained\":false}"),
           "A2: scan retained false (exact)");
        CK(str_has(d, "\"computer-reset\":{\"datatype\":\"boolean\",\"name\":\"电脑重启\",\"settable\":true,\"retained\":false}"),
           "A2: computer-reset retained false (exact)");
        // 只读状态属性 retained:true
        CK(str_has(d, "\"weight\":{\"datatype\":\"float\",\"name\":\"重量\",\"format\":\"0:10000:1\",\"unit\":\"g\",\"settable\":false,\"retained\":true}"),
           "A2: weight retained true (exact)");
    }

    // ----------------------------------------------------
    // B. Availability（weight disabled / mi disabled）
    // ----------------------------------------------------
    {
        int n0 = 0; int p0 = 0;
        g_disabled_weight = false; g_no_mi = false;
        count_available(&n0, &p0);
        CK(n0 == 4,  "B: all 4 nodes available");
        CK(p0 == 16, "B: all 16 properties available");

        // weight.enable = false ⇒ WEIGHT_ZERO 不注册
        g_disabled_weight = true;
        int n1 = 0; int p1 = 0;
        count_available(&n1, &p1);
        CK(p1 == p0 - 3, "B: weight disabled removes weight/weight-error/zero");
        CK(n1 == 4,      "B: water node still present via valve");

        {
            HomieDescInput in;
            in.device_id       = "a5a0000000a1";
            in.buf             = g_desc;
            in.cap             = sizeof(g_desc);
            in.avail           = avail_fn;
            in.avail_ctx       = NULL;
            in.action_version  = 7;
            in.trigger_version = 3;
            size_t len = 0; uint32_t ver = 0;
            CK(homie_desc_build(in, &len, &ver), "B: desc build with weight disabled");
            CK(!str_has(g_desc, "\"weight\""),       "B: weight absent from description");
            CK(!str_has(g_desc, "\"weight-error\""), "B: weight-error absent");
            CK(!str_has(g_desc, "\"zero\""),         "B: zero absent");
            CK(str_has(g_desc, "\"valve\""),         "B: valve still present");
        }

        // 米家 scan Action 未注册 ⇒ climate/scan 不投影（其余 climate 状态属性保留）
        g_disabled_weight = false; g_no_mi = true;
        int n2 = 0; int p2 = 0;
        count_available(&n2, &p2);
        CK(p2 == p0 - 1, "B: mi scan disabled removes only climate/scan");
        CK(n2 == 4,      "B: climate node still present via sensors");

        g_disabled_weight = false; g_no_mi = false;
    }

    // ----------------------------------------------------
    // C. $description 快照（version 稳定 / registry 变化）
    // ----------------------------------------------------
    {
        uint32_t v1 = homie_desc_version(7, 3);
        uint32_t v2 = homie_desc_version(7, 3);
        uint32_t v3 = homie_desc_version(8, 3);
        uint32_t v4 = homie_desc_version(7, 4);
        CK(v1 == v2, "C: version deterministic for same inputs");
        CK(v1 != v3, "C: action version change ⇒ version change");
        CK(v1 != v4, "C: trigger version change ⇒ version change");
    }

    // ----------------------------------------------------
    // D. $state / topic / 生命周期 topic 构造
    // ----------------------------------------------------
    {
        char base[HOMIE_TOPIC_MAX], t[HOMIE_TOPIC_MAX];
        homie_base_topic("a5a0000000a1", base, sizeof(base));
        CK(homie_state_topic(base, t, sizeof(t)), "D: state topic build");
        CKSTR(t, "homie/5/a5a0000000a1/$state", "D: state topic value");
        CK(homie_description_topic(base, t, sizeof(t)), "D: desc topic build");
        CKSTR(t, "homie/5/a5a0000000a1/$description", "D: desc topic value");
        CK(homie_set_filter(base, t, sizeof(t)), "D: filter build");
        CKSTR(t, "homie/5/a5a0000000a1/+/+/set", "D: filter value (precise, no #)");
        CK(!str_has(t, "#"), "D: filter must not contain '#'");
    }

    // ----------------------------------------------------
    // E. Property 状态投影（读侧序列化，逐属性）
    // ----------------------------------------------------
    {
        // valve: bool state → enum member（显式 enum_state_map）
        const DeviceModelProperty* p = dm_find("water", "valve");
        CK(p != NULL, "E: valve found");
        char v[HOMIE_VALUE_MAX]; size_t vl = 0;
        HomieSample s; s.valid = true; s.i = 1; s.f = 0;
        CK(homie_format_value(p, s, v, sizeof(v), &vl), "E: valve true formats");
        CKSTR(v, "open", "E: valve true -> open");
        s.i = 0;
        CK(homie_format_value(p, s, v, sizeof(v), &vl), "E: valve false formats");
        CKSTR(v, "close", "E: valve false -> close");

        // weight: float + deadband + unit
        p = dm_find("water", "weight");
        s.valid = true; s.f = 1234.0f;
        CK(homie_format_value(p, s, v, sizeof(v), &vl), "E: weight formats");
        CKSTR(v, "1234.0", "E: weight 1234.0 (decimals from step=1)");

        // weight-error: bool
        p = dm_find("water", "weight-error");
        s.valid = true; s.i = 1;
        CK(homie_format_value(p, s, v, sizeof(v), &vl), "E: weight-error formats");
        CKSTR(v, "true", "E: weight-error true");

        // temperature / humidity / battery: float
        p = dm_find("climate", "temperature");
        s.valid = true; s.f = 22.55f;
        CK(homie_format_value(p, s, v, sizeof(v), &vl), "E: temperature formats");
        CKSTR(v, "22.55", "E: temperature 22.55");

        p = dm_find("climate", "humidity");
        s.f = 59.9f;
        CK(homie_format_value(p, s, v, sizeof(v), &vl), "E: humidity formats");
        CKSTR(v, "59.90", "E: humidity 59.90");

        p = dm_find("climate", "battery");
        s.f = 2.7f;
        CK(homie_format_value(p, s, v, sizeof(v), &vl), "E: battery formats");
        CKSTR(v, "2.70", "E: battery 2.70");

        // mqtt-connected / time-valid: bool
        p = dm_find("system", "mqtt-connected");
        s.i = 0;
        CK(homie_format_value(p, s, v, sizeof(v), &vl), "E: mqtt-connected formats");
        CKSTR(v, "false", "E: mqtt-connected false");

        // wifi-rssi: int
        p = dm_find("system", "wifi-rssi");
        s.i = -63;
        CK(homie_format_value(p, s, v, sizeof(v), &vl), "E: wifi-rssi formats");
        CKSTR(v, "-63", "E: wifi-rssi -63");

        // wifi-signal: enum from int
        p = dm_find("system", "wifi-signal");
        s.i = 3;
        CK(homie_format_value(p, s, v, sizeof(v), &vl), "E: wifi-signal formats");
        CKSTR(v, "3", "E: wifi-signal 3");
        s.i = 9;   // 不在映射表内 ⇒ 不发布（不伪造）
        CK(!homie_format_value(p, s, v, sizeof(v), &vl), "E: unmapped enum rejected");

        // event-queue / mqtt-retries: int
        p = dm_find("system", "event-queue");
        s.i = 5;
        CK(homie_format_value(p, s, v, sizeof(v), &vl), "E: event-queue formats");
        CKSTR(v, "5", "E: event-queue 5");
        p = dm_find("system", "mqtt-retries");
        s.i = 2;
        CK(homie_format_value(p, s, v, sizeof(v), &vl), "E: mqtt-retries formats");
        CKSTR(v, "2", "E: mqtt-retries 2");

        // 无效样本 ⇒ 拒绝（§二十九：不伪造值）
        s.valid = false; s.i = 0; s.f = 0;
        CK(!homie_format_value(dm_find("water", "valve"), s, v, sizeof(v), &vl),
           "E: invalid sample rejected");
    }

    // ----------------------------------------------------
    // E2. 变化检测（死区 / 首次）
    // ----------------------------------------------------
    {
        HomieSample prev; prev.valid = true; prev.i = 0; prev.f = 100.0f;
        HomieSample cur;  cur.valid  = true; cur.i  = 0; cur.f  = 100.4f;
        // weight deadband = 1.0 ⇒ 0.4 不触发
        CK(!homie_value_changed(DM_FLOAT, prev, false, cur, 1.0f), "E2: below deadband");
        cur.f = 101.0f;
        CK(homie_value_changed(DM_FLOAT, prev, false, cur, 1.0f),  "E2: at deadband");
        cur.f = 100.0f;
        CK(!homie_value_changed(DM_FLOAT, prev, false, cur, 1.0f), "E2: no change");
        // 首次无条件
        CK(homie_value_changed(DM_FLOAT, prev, true, cur, 1.0f),   "E2: first always changes");
        // 无效不触发
        cur.valid = false;
        CK(!homie_value_changed(DM_FLOAT, prev, false, cur, 1.0f), "E2: invalid no publish");
        // deadband=0 ⇒ 严格不等
        cur.valid = true; cur.f = 100.0001f;
        CK(homie_value_changed(DM_FLOAT, prev, false, cur, 0.0f),  "E2: deadband 0 = strict");
        // int
        HomieSample pi; pi.valid = true; pi.i = 5; pi.f = 0;
        HomieSample ci; ci.valid = true; ci.i = 7; ci.f = 0;
        CK(homie_value_changed(DM_INT, pi, false, ci, 0.0f), "E2: int changed");
        ci.i = 5;
        CK(!homie_value_changed(DM_INT, pi, false, ci, 0.0f), "E2: int unchanged");
    }

    // ----------------------------------------------------
    // F. /set 路由：合法
    // ----------------------------------------------------
    {
        char base[HOMIE_TOPIC_MAX], node[32], prop[32];
        homie_base_topic("a5a0000000a1", base, sizeof(base));

        CK(homie_parse_set_topic("homie/5/a5a0000000a1/water/valve/set",
                                 base, node, sizeof(node), prop, sizeof(prop)),
           "F: own set topic accepted");
        CKSTR(node, "water", "F: node parsed");
        CKSTR(prop, "valve", "F: prop parsed");
        CK(hp_eq(dm_find(node, prop)->id, "valve"), "F: dm_find resolves");

        // 合法：zero=true
        {
            const DeviceModelCommand* cmd = NULL;
            CK(dm_resolve_command(dm_find("water", "zero"), "true", &cmd), "F: zero true resolves");
            CK(cmd != NULL && hp_eq(cmd->action_id, "WEIGHT_ZERO"), "F: zero -> WEIGHT_ZERO");
        }
        // 合法：scan start/stop
        {
            const DeviceModelCommand* cmd = NULL;
            CK(dm_resolve_command(dm_find("climate", "scan"), "start", &cmd), "F: scan start");
            CK(cmd != NULL && hp_eq(cmd->action_id, "MI_THERMO_START_SCAN"), "F: start action");
            CK(dm_resolve_command(dm_find("climate", "scan"), "stop", &cmd), "F: scan stop");
            CK(cmd != NULL && hp_eq(cmd->action_id, "MI_THERMO_STOP_SCAN"), "F: stop action");
        }
        // 合法：computer-reset
        {
            const DeviceModelCommand* cmd = NULL;
            CK(dm_resolve_command(dm_find("control", "computer-reset"), "true", &cmd), "F: reset");
            CK(cmd != NULL && hp_eq(cmd->action_id, "COMPUTER_RESET"), "F: reset action");
        }
        // 合法：valve open/close
        {
            const DeviceModelCommand* cmd = NULL;
            CK(dm_resolve_command(dm_find("water", "valve"), "open", &cmd), "F: valve open");
            CK(cmd != NULL && hp_eq(cmd->action_id, "VALVE_OPEN"), "F: open action");
            CK(dm_resolve_command(dm_find("water", "valve"), "close", &cmd), "F: valve close");
            CK(cmd != NULL && hp_eq(cmd->action_id, "VALVE_CLOSE"), "F: close action");
        }
    }

    // ----------------------------------------------------
    // F2. /set 路由：非法（全部必须在协议层被拒绝）
    // ----------------------------------------------------
    {
        char base[HOMIE_TOPIC_MAX], node[32], prop[32];
        homie_base_topic("a5a0000000a1", base, sizeof(base));

        // ① 跨设备 namespace
        CK(!homie_parse_set_topic("homie/5/ffffffffffff/water/valve/set",
                                  base, node, sizeof(node), prop, sizeof(prop)),
           "F2: other device namespace rejected");
        // ② 业务 topic
        CK(!homie_parse_set_topic("guo_feeder/a5a0000000a1/water/valve/set",
                                  base, node, sizeof(node), prop, sizeof(prop)),
           "F2: business topic rejected");
        // ③ 缺少 /set 后缀
        CK(!homie_parse_set_topic("homie/5/a5a0000000a1/water/valve",
                                  base, node, sizeof(node), prop, sizeof(prop)),
           "F2: missing /set rejected");
        // ④ 多余层级
        CK(!homie_parse_set_topic("homie/5/a5a0000000a1/water/valve/set/x",
                                  base, node, sizeof(node), prop, sizeof(prop)),
           "F2: extra level rejected");
        // ⑤ 设备级 $ topic
        CK(!homie_parse_set_topic("homie/5/a5a0000000a1/$state",
                                  base, node, sizeof(node), prop, sizeof(prop)),
           "F2: device $ topic rejected");

        // ⑥ 未知 node / property
        CK(dm_find("nope", "valve") == NULL, "F2: unknown node");
        CK(dm_find("water", "nope") == NULL, "F2: unknown property");
        CK(dm_find("water", "Valve") == NULL, "F2: lookup case-sensitive");

        // ⑦ 只读属性（§二十三）
        const DeviceModelCommand* cmd = NULL;
        CK(!dm_resolve_command(dm_find("water", "weight"), "5", &cmd),       "F2: weight readonly");
        CK(!dm_resolve_command(dm_find("water", "weight-error"), "true", &cmd), "F2: weight-error readonly");
        CK(!dm_resolve_command(dm_find("climate", "temperature"), "20", &cmd), "F2: temperature readonly");
        CK(!dm_resolve_command(dm_find("climate", "humidity"), "20", &cmd),    "F2: humidity readonly");
        CK(!dm_resolve_command(dm_find("climate", "battery"), "3", &cmd),      "F2: battery readonly");
        CK(!dm_resolve_command(dm_find("system", "mqtt-connected"), "true", &cmd), "F2: mqtt-connected readonly");
        CK(!dm_resolve_command(dm_find("system", "wifi-rssi"), "1", &cmd),     "F2: wifi-rssi readonly");
        CK(!dm_resolve_command(dm_find("system", "time-valid"), "true", &cmd), "F2: time-valid readonly");

        // ⑧ 非法 enum / 非法 datatype（§二十二）
        CK(dm_validate_set(dm_find("water", "valve"), "OPEN")  == DM_VALID_ENUM_NOT_FOUND, "F2: OPEN rejected");
        CK(dm_validate_set(dm_find("water", "valve"), "foo")   == DM_VALID_ENUM_NOT_FOUND, "F2: foo rejected");
        CK(dm_validate_set(dm_find("water", "zero"),  "1")     == DM_VALID_BAD_DATATYPE,   "F2: bool '1' rejected");
        CK(dm_validate_set(dm_find("water", "zero"),  "True")  == DM_VALID_BAD_DATATYPE,   "F2: bool 'True' rejected");
        CK(dm_validate_set(dm_find("water", "weight"), "5")    == DM_VALID_NOT_SETTABLE,   "F2: readonly rejected");
        CK(dm_validate_set(NULL, "x")                          == DM_VALID_UNKNOWN_PROPERTY, "F2: null property");
    }

    // ----------------------------------------------------
    // G. Retained /set（判据 = retain 标志，非 payload/topic）
    //
    //   ★ 本探针只能证明「routing/validate/mapping 逻辑与 retain 解耦」：
    //     即解析与执行路径不读取 retain，retain 只作**独立丢弃开关**。
    //     broker 的 retained replay 行为属 NOT RUN（见文件头声明）。
    // ----------------------------------------------------
    {
        // 同一 topic + 同一 payload，仅 retain 不同 ⇒ 处理入口必须分叉
        // （实现侧由 homie_on_foreign 首行 `if (retain) return;` 保证，
        //   此处验证解析/映射函数本身**不接受** retain 参数 ⇒ 无法被 retain 影响）
        const DeviceModelProperty* p = dm_find("water", "valve");
        const DeviceModelCommand* cmd = NULL;
        CK(dm_resolve_command(p, "open", &cmd), "G: mapping independent of retain");
        CK(cmd != NULL && hp_eq(cmd->action_id, "VALVE_OPEN"), "G: mapping stable");
        CK(homie_topic_is_own("homie/5/a5a0000000a1/water/valve/set",
                              "homie/5/a5a0000000a1/"), "G: topic check independent of retain");
        // 一次性命令（zero / computer-reset）在两种 retain 下映射一致 ⇒ 策略单一
        CK(dm_resolve_command(dm_find("water", "zero"), "true", &cmd), "G: zero mapping stable");
        CK(dm_resolve_command(dm_find("control", "computer-reset"), "true", &cmd), "G: reset mapping stable");
    }

    // ----------------------------------------------------
    // H. CommandManager 契约（source="homie"）
    //   ★ 本阶段只验证「命令构造所需字段全部来自 Device Model，
    //     且客户端无法提供 action_id」；真实执行属 P2-7 / NOT RUN。
    // ----------------------------------------------------
    {
        const DeviceModelProperty* p = dm_find("water", "valve");
        const DeviceModelCommand* cmd = NULL;
        CK(dm_resolve_command(p, "open", &cmd), "H: resolve");
        CK(cmd != NULL, "H: cmd non-null");
        // 关键安全性质：解析出的 cmd 指针必然落在该属性自己的映射表内
        CK(cmd >= p->cmds && cmd < (p->cmds + p->cmd_count), "H: cmd pointer within table");
        CK(hp_eq(cmd->payload_json, "{}"), "H: payload from model");

        // cmd_id 唯一性（§7.3）
        char a[16], b[16];
        homie_cmd_id(0x3f2a, 1, a, sizeof(a));
        homie_cmd_id(0x3f2a, 2, b, sizeof(b));
        CKSTR(a, "h3f2a-000001", "H: cmd_id format 1");
        CKSTR(b, "h3f2a-000002", "H: cmd_id format 2");
        CK(!hp_eq(a, b), "H: cmd_id unique per counter");
        char c[16];
        homie_cmd_id(0x3f2b, 1, c, sizeof(c));
        CK(!hp_eq(a, c), "H: cmd_id differs per boot_seq");
    }

    // ----------------------------------------------------
    // I. 安全边界（不含 secret / 不跨设备 / 不含内部态）
    // ----------------------------------------------------
    {
        HomieDescInput in;
        in.device_id       = "a5a0000000a1";
        in.buf             = g_desc;
        in.cap             = sizeof(g_desc);
        in.avail           = avail_fn;
        in.avail_ctx       = NULL;
        in.action_version  = 7;
        in.trigger_version = 3;
        size_t len = 0; uint32_t ver = 0;
        homie_desc_build(in, &len, &ver);

        const char* secret_keys[] = {
            "password", "passwd", "secret", "api_key", "private_key",
            "bind_key", "access_token", "refresh_token", "token",
            "ssid", "BLE_key", "mqtt_password", "wifi_password",
            "credential", "cred"
        };
        for(unsigned i = 0; i < sizeof(secret_keys)/sizeof(secret_keys[0]); ++i)
        {
            CK(!str_has(g_desc, secret_keys[i]), "I: no secret key in description");
        }
        // 内部状态不得投影（§七）
        const char* internal[] = {
            "STATE_MQTT_LAST_ERROR", "WIFI_STATE", "MI_THERMO_VALID",
            "registry", "checksum", "dedupe", "runtime"
        };
        for(unsigned i = 0; i < sizeof(internal)/sizeof(internal[0]); ++i)
        {
            CK(!str_has(g_desc, internal[i]), "I: no internal state in description");
        }
        // 时间戳类不投影（§12.3）
        CK(!str_has(g_desc, "_TS"), "I: no timestamp properties");
        // 事件触发器不投影（§12.3）
        CK(!str_has(g_desc, "weight_decrease"), "I: no event triggers");
        // $target / json 不出现（D-13 / D-5）
        CK(!str_has(g_desc, "$target"), "I: no $target (D-13)");
        CK(!str_has(g_desc, "\"json\""), "I: no json datatype (D-5)");
    }

    // ----------------------------------------------------
    // J. Property ABI（§三）
    // ----------------------------------------------------
    {
        const uint8_t pc = dm_property_count();
        bool all_ok = true;
        for(uint8_t i = 0; i < pc; ++i)
        {
            const DeviceModelProperty* p = dm_property_at(i);
            if(p == NULL) { all_ok = false; break; }
            if(!homie_id_valid(p->id) || !homie_id_valid(p->node)) { all_ok = false; break; }
        }
        CK(all_ok, "J: all property/node ids match [a-z0-9-]");

        const uint8_t nc = dm_node_count();
        bool nodes_ok = true;
        for(uint8_t i = 0; i < nc; ++i)
        {
            const DeviceModelNode* n = dm_node_at(i);
            if(n == NULL || !homie_id_valid(n->id)) { nodes_ok = false; break; }
        }
        CK(nodes_ok, "J: all node ids match [a-z0-9-]");
        // 无大写 / 无 '/' / 无前导 '$' 已由 homie_id_valid 覆盖
        CK(!homie_id_valid("Valve"), "J: uppercase rejected");
        CK(!homie_id_valid("a/b"),   "J: slash rejected");
        CK(!homie_id_valid("$abc"),  "J: dollar rejected");
        CK(!homie_id_valid(""),      "J: empty rejected");
        CK(!homie_id_valid("a_b"),   "J: underscore rejected");
        CK(homie_id_valid("weight-error"), "J: hyphen allowed");
        CK(homie_id_valid("wifi0"),  "J: digit allowed");
    }

    // ----------------------------------------------------
    // K. 容量边界（不得溢出 / 不得静默截断）
    // ----------------------------------------------------
    {
        char small[16];
        size_t len = 0;
        HomieDescInput in;
        in.device_id       = "a5a0000000a1";
        in.buf             = small;
        in.cap             = sizeof(small);
        in.avail           = avail_fn;
        in.avail_ctx       = NULL;
        in.action_version  = 7;
        in.trigger_version = 3;
        CK(!homie_desc_build(in, &len, NULL), "K: desc fails on tiny buffer (no truncation)");

        // topic 构造容量不足 ⇒ 失败
        char tiny[4];
        CK(!homie_base_topic("a5a0000000a1", tiny, sizeof(tiny)), "K: base topic cap");
        // 复制超长 ⇒ 失败
        char n2[4];
        CK(!hp_copy(n2, sizeof(n2), "homie/5/"), "K: hp_copy cap");
    }
}

// ---------------- 导出（契约与 test/device_model_host/run_probe.js 一致）----------------
static void wr_u32(unsigned v)
{
    char b[12];
    int  n = 0;
    if(v == 0) { b[n++] = '0'; }
    while(v > 0) { b[n++] = (char)('0' + (v % 10u)); v /= 10u; }
    for(int i = n - 1; i >= 0; --i)
    {
        if(g_log_len < (int)(sizeof(g_log) - 1)) { g_log[g_log_len++] = b[i]; }
    }
}

extern "C" int        probe_run()
{
    probe_main();
    wr("\n---- homie projection probe ----\nPASS=");
    wr_u32((unsigned)g_pass);
    wr("  FAIL=");
    wr_u32((unsigned)g_fail);
    wr("\n");
    return g_fail;
}
extern "C" int        probe_pass()     { return g_pass; }
extern "C" int        probe_fail()     { return g_fail; }
extern "C" int        probe_log_len()  { return g_log_len; }
extern "C" const char* probe_log_ptr() { return g_log; }
