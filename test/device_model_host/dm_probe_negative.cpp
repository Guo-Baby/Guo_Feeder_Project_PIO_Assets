// =====================================================
// Device Model —— 负向探针（可证伪性保证）
//
// 目的：证明"探针能真的检测到失败"。
// 本文件**故意**断言若干与冻结设计**相反**的事实 ⇒ 运行 MUST 报告 fail>0。
// 若本探针 fail==0，说明探针/断言机制失效，"通过"不可信。
// =====================================================

#include "homie/device_model.h"

static char g_log[2048];
static int  g_log_len = 0;
static int  g_fail = 0;

static void put(const char* s)
{
    while (*s != '\0' && g_log_len < (int)sizeof(g_log) - 1) g_log[g_log_len++] = *s++;
}

static void expect_fail(bool should_be_true, const char* name)
{
    if (!should_be_true)
    {
        ++g_fail;
        put("  [FAIL-as-expected] "); put(name); put("\n");
    }
    else
    {
        put("  [UNEXPECTED-PASS] "); put(name); put("\n");
    }
}

extern "C" int probe_run()
{
    g_log_len = 0; g_fail = 0;
    put("Device Model NEGATIVE probe (must fail)\n");

    expect_fail(dm_property_count() == 99, "property count == 99 (false)");
    expect_fail(dm_node_count() == 7, "node count == 7 (false)");
    expect_fail(dm_find("water", "valve") == nullptr, "water/valve missing (false)");
    expect_fail(dm_find("water", "valve") != nullptr && dm_find("water", "valve")->retained == false, "valve retained false (false)");

    {
        const DeviceModelCommand* cmd = nullptr;
        expect_fail(dm_resolve_command(dm_find("water", "valve"), "OPEN", &cmd), "OPEN resolves (false)");
        expect_fail(dm_resolve_command(dm_find("water", "weight"), "5", &cmd), "readonly resolves (false)");
        expect_fail(dm_validate_set(dm_find("water", "valve"), "bogus") == DM_VALID_OK, "bogus enum valid (false)");
    }

    put("NEGATIVE probe fail_count=");
    if (g_fail == 0) put("0");
    else
    {
        char b[8]; int n = 0; int v = g_fail;
        while (v > 0) { b[n++] = (char)('0' + v % 10); v /= 10; }
        for (int i = n - 1; i >= 0; --i) { if (g_log_len < (int)sizeof(g_log) - 1) g_log[g_log_len++] = b[i]; }
    }
    put("\n");
    return g_fail;
}

extern "C" const char* probe_log_ptr() { return g_log; }
extern "C" int         probe_log_len() { return g_log_len; }
