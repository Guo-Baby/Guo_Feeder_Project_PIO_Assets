// =====================================================
// 定时触发策略 —— 宿主机探针（wasm32 freestanding + node）
//
// 用途：在**没有硬件**的环境里**真实执行** `automation/wf_sched_policy.h`
//       的全部纯逻辑，覆盖任务书 A1 要求的判定矩阵：
//         · 时间不可信（哪怕时钟值看起来完全正确）⇒ 不触发
//         · 时间可信 + 到点 ⇒ 触发；未到点 ⇒ 不触发
//         · 过了窗口 ⇒ 不触发（不补执行）
//         · 同周期已触发 ⇒ 不重复
//         · 停用 / 已删除 / 运行中 / 首步不是 Timer ⇒ 不参与
//         · once / daily / weekly 的边界（跨秒 / 跨日 / 跨周 / 非法日期）
//         · 时间被往回校正后不重复触发
//         · 节流常量与窗口常量本身（非阻塞的静态证据）
//
// ★ 本探针只证明"纯决策逻辑正确"，**不**证明目标板时序开销，也不替代
//   真机验收（刷入固件后才会真正周期运行）。
//
// 编译运行见 run.sh。
// =====================================================

#include <stddef.h>
#include <stdint.h>

#include "automation/wf_sched_policy.h"

// ---- freestanding 支撑（避免 libcall） ----
extern "C" void *memcpy(void *dst, const void *src, size_t n)
{
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    for(size_t i = 0; i < n; ++i) d[i] = s[i];
    return dst;
}
extern "C" void *memset(void *dst, int c, size_t n)
{
    unsigned char *d = (unsigned char *)dst;
    for(size_t i = 0; i < n; ++i) d[i] = (unsigned char)c;
    return dst;
}

// ---- 极简日志（无 stdio） ----
static char g_log[32768];
static int g_log_len = 0;
static int g_pass = 0;
static int g_fail = 0;

static void log_ch(char c)
{
    if(g_log_len < (int)sizeof(g_log) - 1) g_log[g_log_len++] = c;
}

static void log_str(const char *s)
{
    while(s != nullptr && *s != '\0') log_ch(*s++);
}

static void log_i64(int64_t v)
{
    if(v < 0) { log_ch('-'); v = -v; }
    char buf[24];
    int n = 0;
    if(v == 0) buf[n++] = '0';
    while(v > 0 && n < 24) { buf[n++] = (char)('0' + (int)(v % 10)); v /= 10; }
    while(n > 0) log_ch(buf[--n]);
}

static void row(const char *id, bool ok, const char *note)
{
    if(ok) g_pass++; else g_fail++;
    log_str(ok ? "  PASS  " : "  FAIL  ");
    log_str(id);
    if(note != nullptr && *note != '\0')
    {
        log_str("   [");
        log_str(ok ? "" : "!= ");
        log_str(note);
        log_str("]");
    }
    log_ch('\n');
}

// ---- 便于阅读的时间常量（本地墙钟伪纪元） ----
// 2026-10-27 是周二（tm_wday = 2），取自公历换算（20753 天）。
static int64_t pe(int y, int mo, int d, int h, int mi, int s)
{
    return wf_sched_pseudo_epoch(y, mo, d, h, mi, s);
}

extern "C" int probe_run()
{
    // =================================================
    // 1. 公历换算基线（错了后面全错，先自证）
    // =================================================
    log_str("[1] 公历换算基线\n");
    row("S-1-1 1970-01-01 = 0",
        wf_sched_days_from_civil(1970, 1, 1) == 0, "");
    row("S-1-2 1970-01-02 = 1",
        wf_sched_days_from_civil(1970, 1, 2) == 1, "");
    row("S-1-3 2000-03-01 = 11017",
        wf_sched_days_from_civil(2000, 3, 1) == 11017, "");
    row("S-1-4 2024-02-29 = 19782（闰日）",
        wf_sched_days_from_civil(2024, 2, 29) == 19782, "");
    row("S-1-5 2026-10-27 = 20753",
        wf_sched_days_from_civil(2026, 10, 27) == 20753, "");
    row("S-1-6 2027-01-01 = 20819（跨年）",
        wf_sched_days_from_civil(2027, 1, 1) == 20819, "");
    row("S-1-7 闰年二月天数 2024=29 / 2026=28",
        wf_sched_days_in_month(2024, 2) == 29 && wf_sched_days_in_month(2026, 2) == 28, "");
    row("S-1-8 伪纪元按秒可加（08:00:00 < 08:00:01）",
        pe(2026, 10, 27, 8, 0, 0) + 1 == pe(2026, 10, 27, 8, 0, 1), "");

    // =================================================
    // 2. 时间可信门禁（A1.3 / A1.4 / A1.5）
    // =================================================
    log_str("[2] 时间可信门禁\n");
    const int64_t plausible = pe(2026, 10, 27, 8, 0, 0);   // 看起来完全正确的时刻
    row("S-2-1 ★ 标志为 false ⇒ 可信=false（哪怕时钟值合理）",
        wf_sched_time_trusted(false, plausible) == false, "");
    row("S-2-2 标志为 true 且 now>0 ⇒ 可信",
        wf_sched_time_trusted(true, plausible) == true, "");
    row("S-2-3 标志 true 但 now=0 ⇒ 不可信（标志与取值不自洽）",
        wf_sched_time_trusted(true, 0) == false, "");
    row("S-2-4 标志 true 但 now<0 ⇒ 不可信",
        wf_sched_time_trusted(true, -1) == false, "");
    {
        // ★ 关键组合：标志 false 时，即便"假如可信"本会触发，也不得触发
        const int64_t target = wf_sched_period_target(
            WF_SCHED_TYPE_DAILY, 2026, 10, 27, 2, 8, 0, 1, 2026, 1, 1);
        const bool trusted = wf_sched_time_trusted(false, plausible);
        const bool would_fire = wf_sched_should_fire(plausible, target, 0, WF_SCHED_WINDOW_SEC);
        row("S-2-5 ★ 时钟恰好等于计划时刻，但标志 false ⇒ 最终不触发",
            trusted == false && would_fire == true, "");
        row("S-2-6 同一时刻、标志 true ⇒ 会触发（证明上一条不是靠时间值挡住的）",
            wf_sched_time_trusted(true, plausible) && would_fire, "");
    }

    // =================================================
    // 3. 候选资格（A1.9 / A1.10）
    // =================================================
    log_str("[3] 候选资格（停用/删除/运行中/首步）\n");
    //            enable valid running steps trig  timer
    row("S-3-1 全部满足 ⇒ 参与",
        wf_sched_candidate_ready(true, true, false, 2, true, true) == true, "");
    row("S-3-2 ★ enable=false ⇒ 不参与（定时器绝不起停用流程）",
        wf_sched_candidate_ready(false, true, false, 2, true, true) == false, "");
    row("S-3-3 valid=false（已逻辑删除）⇒ 不参与",
        wf_sched_candidate_ready(true, false, false, 2, true, true) == false, "");
    row("S-3-4 正在运行 ⇒ 不参与（不打断、不重入）",
        wf_sched_candidate_ready(true, true, true, 2, true, true) == false, "");
    row("S-3-5 无步骤 ⇒ 不参与",
        wf_sched_candidate_ready(true, true, false, 0, false, false) == false, "");
    row("S-3-6 首步不是 Trigger ⇒ 不参与",
        wf_sched_candidate_ready(true, true, false, 2, false, false) == false, "");
    row("S-3-7 首步是 Trigger 但不是 timer ⇒ 不参与",
        wf_sched_candidate_ready(true, true, false, 2, true, false) == false, "");

    // =================================================
    // 4. 到点/未到点/过窗（A1.5 / A1.6 / A1.7）
    // =================================================
    log_str("[4] 触发窗口\n");
    {
        const int64_t t = pe(2026, 10, 27, 8, 0, 0);
        row("S-4-1 未到点（差 1 秒）⇒ 不触发",
            wf_sched_should_fire(t - 1, t, 0, WF_SCHED_WINDOW_SEC) == false, "");
        row("S-4-2 恰好到点 ⇒ 触发",
            wf_sched_should_fire(t, t, 0, WF_SCHED_WINDOW_SEC) == true, "");
        row("S-4-3 窗口内（+5s）⇒ 触发",
            wf_sched_should_fire(t + 5, t, 0, WF_SCHED_WINDOW_SEC) == true, "");
        row("S-4-4 窗口末尾（+9s）⇒ 触发",
            wf_sched_should_fire(t + 9, t, 0, WF_SCHED_WINDOW_SEC) == true, "");
        row("S-4-5 ★ 恰好越过窗口（+10s）⇒ 不触发（不补执行）",
            wf_sched_should_fire(t + 10, t, 0, WF_SCHED_WINDOW_SEC) == false, "");
        row("S-4-6 ★ 错过大段（+1 小时）⇒ 不触发（恢复后不集中补跑）",
            wf_sched_should_fire(t + 3600, t, 0, WF_SCHED_WINDOW_SEC) == false, "");
        row("S-4-7 target=0（本周期无计划）⇒ 不触发",
            wf_sched_should_fire(t, 0, 0, WF_SCHED_WINDOW_SEC) == false, "");
    }

    // =================================================
    // 5. 同周期去重（A1.7 / 时间校正）
    // =================================================
    log_str("[5] 同周期去重\n");
    {
        const int64_t t = pe(2026, 10, 27, 8, 0, 0);
        row("S-5-1 已触发同 target ⇒ 不重复",
            wf_sched_should_fire(t + 1, t, t, WF_SCHED_WINDOW_SEC) == false, "");
        row("S-5-2 ★ 时间被往回校正、再次落回窗口 ⇒ 仍不重复",
            wf_sched_should_fire(t, t, t, WF_SCHED_WINDOW_SEC) == false, "");
        row("S-5-3 换了 target（次日同刻）⇒ 允许触发",
            wf_sched_should_fire(t + 86400, t + 86400, t, WF_SCHED_WINDOW_SEC) == true, "");
        row("S-5-4 未触发过（ledger=0）⇒ 允许触发",
            wf_sched_should_fire(t, t, 0, WF_SCHED_WINDOW_SEC) == true, "");
    }

    // =================================================
    // 6. once
    // =================================================
    log_str("[6] once\n");
    {
        // 2026-11-01（周日）09:30
        const int64_t t = wf_sched_period_target(
            WF_SCHED_TYPE_ONCE, 2026, 10, 27, 2, 9, 30, 1, 2026, 11, 1);
        row("S-6-1 ★ once 目标是绝对日期时刻（不受 now 的日期影响）",
            t == pe(2026, 11, 1, 9, 30, 0), "");
        row("S-6-2 once 未到 ⇒ 不触发",
            wf_sched_should_fire(pe(2026, 10, 27, 9, 30, 0), t, 0, WF_SCHED_WINDOW_SEC) == false, "");
        row("S-6-3 once 到点 ⇒ 触发",
            wf_sched_should_fire(t, t, 0, WF_SCHED_WINDOW_SEC) == true, "");
        row("S-6-4 ★ once 已过（次日）⇒ 不触发（重启不会重复执行）",
            wf_sched_should_fire(t + 86400, t, 0, WF_SCHED_WINDOW_SEC) == false, "");
        row("S-6-5 ★ once 已触发且账本已记 ⇒ 重启再判也不触发",
            wf_sched_should_fire(t + 1, t, t, WF_SCHED_WINDOW_SEC) == false, "");
        row("S-6-6 once 非法月份（13）⇒ 无计划（不借规范化猜一天）",
            wf_sched_period_target(WF_SCHED_TYPE_ONCE, 2026, 10, 27, 2, 9, 30, 1, 2026, 13, 1) == 0, "");
        row("S-6-7 once 非法日（2 月 30 日）⇒ 无计划",
            wf_sched_period_target(WF_SCHED_TYPE_ONCE, 2026, 10, 27, 2, 9, 30, 1, 2026, 2, 30) == 0, "");
        row("S-6-8 once 闰日（2024-02-29）⇒ 合法",
            wf_sched_period_target(WF_SCHED_TYPE_ONCE, 2026, 10, 27, 2, 9, 30, 1, 2024, 2, 29)
                == pe(2024, 2, 29, 9, 30, 0), "");
    }

    // =================================================
    // 7. daily
    // =================================================
    log_str("[7] daily\n");
    {
        const int64_t t = wf_sched_period_target(
            WF_SCHED_TYPE_DAILY, 2026, 10, 27, 2, 8, 0, 1, 2026, 1, 1);
        row("S-7-1 daily 目标 = 今天 08:00",
            t == pe(2026, 10, 27, 8, 0, 0), "");
        row("S-7-2 前天同一时刻 ⇒ 不触发",
            wf_sched_should_fire(pe(2026, 10, 26, 8, 0, 0), t, 0, WF_SCHED_WINDOW_SEC) == false, "");
        // 跨日边界：00:00 计划
        const int64_t mid = wf_sched_period_target(
            WF_SCHED_TYPE_DAILY, 2026, 10, 27, 2, 0, 0, 1, 2026, 1, 1);
        row("S-7-3 ★ 跨日边界：23:59:59 不触发 00:00 计划",
            wf_sched_should_fire(mid - 1, mid, 0, WF_SCHED_WINDOW_SEC) == false, "");
        row("S-7-4 跨日边界：00:00:00 触发",
            wf_sched_should_fire(mid, mid, 0, WF_SCHED_WINDOW_SEC) == true, "");
        row("S-7-5 ★ 昨天的 00:00 账本不压制今天的 00:00",
            wf_sched_should_fire(mid, mid, mid - 86400, WF_SCHED_WINDOW_SEC) == true, "");
        row("S-7-6 今天的 00:00 账本压制今天的 00:00（跨秒不重复）",
            wf_sched_should_fire(mid + 3, mid, mid, WF_SCHED_WINDOW_SEC) == false, "");
    }

    // =================================================
    // 8. weekly
    // =================================================
    log_str("[8] weekly\n");
    {
        // 2026-10-27 是周二（2）；目标周日（0）
        row("S-8-1 ★ 今天不是目标星期 ⇒ 无计划",
            wf_sched_period_target(WF_SCHED_TYPE_WEEKLY, 2026, 10, 27, 2, 8, 0, 0, 2026, 1, 1) == 0, "");
        const int64_t t = wf_sched_period_target(
            WF_SCHED_TYPE_WEEKLY, 2026, 10, 27, 2, 8, 0, 2, 2026, 1, 1);
        row("S-8-2 今天就是目标星期（周二=2）⇒ 目标 = 今天 08:00",
            t == pe(2026, 10, 27, 8, 0, 0), "");
        row("S-8-3 weekly 到点 ⇒ 触发",
            wf_sched_should_fire(t, t, 0, WF_SCHED_WINDOW_SEC) == true, "");
        row("S-8-4 ★ 同周已触发 ⇒ 不再触发",
            wf_sched_should_fire(t + 2, t, t, WF_SCHED_WINDOW_SEC) == false, "");
        row("S-8-5 ★ 跨周：下一周的同一目标日是新 target（允许触发）",
            wf_sched_should_fire(t + 7 * 86400, t + 7 * 86400, t, WF_SCHED_WINDOW_SEC) == true, "");
        row("S-8-6 星期参数越界（9）⇒ 钳到 6，今天(2)不匹配 ⇒ 无计划",
            wf_sched_period_target(WF_SCHED_TYPE_WEEKLY, 2026, 10, 27, 2, 8, 0, 9, 2026, 1, 1) == 0, "");
        row("S-8-7 星期参数为负（-3）⇒ 钳到 0（周日），今天(2)不匹配 ⇒ 无计划",
            wf_sched_period_target(WF_SCHED_TYPE_WEEKLY, 2026, 10, 27, 2, 8, 0, -3, 2026, 1, 1) == 0, "");
    }

    // =================================================
    // 9. 参数钳位与未知类型
    // =================================================
    log_str("[9] 参数钳位 / 未知类型\n");
    {
        row("S-9-1 hour=99 钳到 23",
            wf_sched_period_target(WF_SCHED_TYPE_DAILY, 2026, 10, 27, 2, 99, 0, 1, 2026, 1, 1)
                == pe(2026, 10, 27, 23, 0, 0), "");
        row("S-9-2 hour=-5 钳到 0",
            wf_sched_period_target(WF_SCHED_TYPE_DAILY, 2026, 10, 27, 2, -5, 0, 1, 2026, 1, 1)
                == pe(2026, 10, 27, 0, 0, 0), "");
        row("S-9-3 minute=99 钳到 59",
            wf_sched_period_target(WF_SCHED_TYPE_DAILY, 2026, 10, 27, 2, 8, 99, 1, 2026, 1, 1)
                == pe(2026, 10, 27, 8, 59, 0), "");
        row("S-9-4 未知类型 ⇒ 无计划（不猜语义）",
            wf_sched_period_target(WF_SCHED_TYPE_UNKNOWN, 2026, 10, 27, 2, 8, 0, 1, 2026, 1, 1) == 0, "");
    }

    // =================================================
    // 10. type 解析
    // =================================================
    log_str("[10] type 字符串解析\n");
    row("S-10-1 \"once\"", wf_sched_parse_type("once") == WF_SCHED_TYPE_ONCE, "");
    row("S-10-2 \"daily\"", wf_sched_parse_type("daily") == WF_SCHED_TYPE_DAILY, "");
    row("S-10-3 \"weekly\"", wf_sched_parse_type("weekly") == WF_SCHED_TYPE_WEEKLY, "");
    row("S-10-4 ★ \"dailyx\" ⇒ 未知（不做前缀匹配）",
        wf_sched_parse_type("dailyx") == WF_SCHED_TYPE_UNKNOWN, "");
    row("S-10-5 空串 ⇒ 未知", wf_sched_parse_type("") == WF_SCHED_TYPE_UNKNOWN, "");
    row("S-10-6 nullptr ⇒ 未知", wf_sched_parse_type(nullptr) == WF_SCHED_TYPE_UNKNOWN, "");

    // =================================================
    // 11. 非阻塞的静态证据（常量）
    // =================================================
    log_str("[11] 非阻塞与窗口常量\n");
    row("S-11-1 检查节拍 = 1000ms（每秒最多一次，不是每 loop）",
        WF_SCHED_TICK_MS == 1000u, "");
    row("S-11-2 匹配窗口 = 10s", WF_SCHED_WINDOW_SEC == 10, "");
    row("S-11-3 纯函数：同输入重复调用结果一致（无内部状态）",
        wf_sched_period_target(WF_SCHED_TYPE_DAILY, 2026, 10, 27, 2, 8, 0, 1, 2026, 1, 1)
            == wf_sched_period_target(WF_SCHED_TYPE_DAILY, 2026, 10, 27, 2, 8, 0, 1, 2026, 1, 1), "");

    // ---- 汇总 ----
    log_ch('\n');
    log_str("===== workflow_sched probe =====\n");
    log_str("  total ");
    log_i64(g_pass + g_fail);
    log_str(" | PASS ");
    log_i64(g_pass);
    log_str(" | FAIL ");
    log_i64(g_fail);
    log_ch('\n');

    return g_fail;
}

extern "C" int probe_log_len() { return g_log_len; }
extern "C" const char *probe_log_ptr() { return g_log; }
