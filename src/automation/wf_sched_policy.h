#pragma once

// =====================================================
// 定时触发策略（纯函数 · 无 libc · 可宿主机真实执行）
//
// 2026-10-27 补齐固件定时调度器时抽出。
//
// ---- 为什么要单独成一个头 ----
// 1) 定时判定是最容易出错的一段（跨秒/跨日/跨周/once 过期/时间校正），
//    必须能被**宿主机探针真实执行**（test/workflow_sched_host），
//    而不是只靠"读代码觉得对"。
// 2) 宿主机探针在 wasm32 freestanding 下编译（无 libc 头文件），
//    因此这里**不得**使用 mktime / localtime_r / String / 堆分配。
// 3) 全部为 `inline` 纯函数：无状态、无副作用、可重入、零分配。
//
// ---- 为什么用"本地墙钟伪纪元"而不是 mktime ----
// mktime 依赖进程 TZ 与 DST 规则，同一份代码在板端与宿主机可能给出不同结果，
// 探针就失去意义。改为：由调用方把 `now` 拆成本地日历字段（localtime_r），
// 本文件用**公历整数算术**（Howard Hinnant days_from_civil）把日历字段
// 换算成"本地墙钟秒"（pseudo-epoch）。
//
// 于是"每天 08:00"这种日历语义被直接比较（不受 DST 影响）；中国无夏令时，
// 与既有 `timer_calculate_next()`（用 mktime）完全一致。差异仅限有 DST 的
// 时区，且本调度器运行时不经过 Step0 的 timer trigger（skip_first_step=true），
// 因此不会出现"同一路径两套算法打架"。
// =====================================================

#include <stdint.h>

// 与固件 timer trigger 的 "type" 参数取值一一对应（workflow.cpp:884）
enum WfSchedTimerType
{
    WF_SCHED_TYPE_ONCE = 0,
    WF_SCHED_TYPE_DAILY = 1,
    WF_SCHED_TYPE_WEEKLY = 2,
    WF_SCHED_TYPE_UNKNOWN = 255
};

// 匹配窗口（秒）：从计划时刻起算 [target, target + window)
// 10s 的意义：既容忍主循环抖动，又短到"重启+对时"不可能落在同一窗口内。
#define WF_SCHED_WINDOW_SEC 10

// 检查节拍（毫秒）：调度器最多每秒跑一次，避免每个 loop 都遍历 16 个 Slot。
// 放在本头文件是为了让宿主机探针能校验"非阻塞"的节流常量本身。
#define WF_SCHED_TICK_MS 1000u

/**
 * 时间可信门禁（硬门禁，优先于一切）。
 *
 * ★ 语义铁律：**只认 System State 的 time_valid 标志**。
 *   即使 now_sec 看起来完全合理（比如 2026-10-27 08:00:00），
 *   只要标志为 false 就必须判定"不可信" ⇒ 不匹配、不启动、不补执行。
 *   绝不拿 RTC / 系统时钟的值去反推可信。
 */
inline bool wf_sched_time_trusted(bool time_valid, int64_t now_sec)
{
    if(!time_valid) return false;
    if(now_sec <= 0) return false;
    return true;
}

/**
 * 该 Slot 是否有资格参与定时匹配（顺序无关，全部满足才 true）。
 *
 * 与 wf_sched_time_trusted 分开，是为了让"停用/已删除/运行中/首步不是 timer"
 * 这四类拒绝都能被宿主机探针逐项覆盖。
 */
inline bool wf_sched_candidate_ready(
    bool enable,
    bool valid,
    bool running,
    int step_count,
    bool step0_is_trigger,
    bool step0_is_timer)
{
    if(!enable) return false;             // 停用 ⇒ 绝不由定时器启动
    if(!valid) return false;              // 已逻辑删除
    if(running) return false;             // 运行中 ⇒ 不打断、不重入
    if(step_count <= 0) return false;
    if(!step0_is_trigger) return false;
    if(!step0_is_timer) return false;
    return true;
}

/** 整数钳位 */
inline int wf_sched_clamp_int(int v, int lo, int hi)
{
    if(v < lo) return lo;
    if(v > hi) return hi;
    return v;
}

/** 参数 "type" 字符串 → 枚举（大小写敏感，与固件一致；未知一律 UNKNOWN） */
inline WfSchedTimerType wf_sched_parse_type(const char *s)
{
    if(s == nullptr) return WF_SCHED_TYPE_UNKNOWN;
    // 手写比较：不引 <string.h>（freestanding 探针无 libc）
    const char *once = "once";
    const char *daily = "daily";
    const char *weekly = "weekly";
    int i = 0;
    while(s[i] != '\0' && once[i] != '\0' && s[i] == once[i]) i++;
    if(s[i] == '\0' && once[i] == '\0') return WF_SCHED_TYPE_ONCE;
    i = 0;
    while(s[i] != '\0' && daily[i] != '\0' && s[i] == daily[i]) i++;
    if(s[i] == '\0' && daily[i] == '\0') return WF_SCHED_TYPE_DAILY;
    i = 0;
    while(s[i] != '\0' && weekly[i] != '\0' && s[i] == weekly[i]) i++;
    if(s[i] == '\0' && weekly[i] == '\0') return WF_SCHED_TYPE_WEEKLY;
    return WF_SCHED_TYPE_UNKNOWN;
}

/**
 * 公历日期 → 距 1970-01-01 的天数（Howard Hinnant days_from_civil）。
 * 仅对 1<=m<=12 有定义；调用方须先校验。
 */
inline int64_t wf_sched_days_from_civil(int y, unsigned m, unsigned d)
{
    y -= (m <= 2) ? 1 : 0;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153u * (m + (m > 2 ? (unsigned)-3 : 9u)) + 2u) / 5u + d - 1u;
    const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

/** 当月天数（公历） */
inline int wf_sched_days_in_month(int y, int m)
{
    static const unsigned char kDays[12] =
    {
        31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
    };
    if(m < 1 || m > 12) return 0;
    if(m != 2) return kDays[m - 1];
    const bool leap = ((y % 4 == 0) && (y % 100 != 0)) || (y % 400 == 0);
    return leap ? 29 : 28;
}

/** 本地日历字段 → 本地墙钟秒（pseudo-epoch） */
inline int64_t wf_sched_pseudo_epoch(
    int y, int mon, int day, int hour, int minute, int second)
{
    return wf_sched_days_from_civil(y, (unsigned)mon, (unsigned)day) * 86400
        + (int64_t)hour * 3600 + (int64_t)minute * 60 + (int64_t)second;
}

/**
 * 计算「当前周期」的计划触发时刻（本地墙钟秒）。0 = 本周期无计划。
 *
 * @param type        定时类型
 * @param now_year/mon/day/wday  由 localtime_r(now) 提供的本地日历字段
 * @param hour/minute 计划时刻（会钳位到 0-23 / 0-59）
 * @param dow         weekly 的目标星期（0=周日，会钳位到 0-6）
 * @param once_year/once_mon/once_day  once 的绝对日期
 *
 * @return >0 = 本周期目标时刻；0 = 无计划（once 日期非法 / 今天不是目标星期 /
 *         type 未识别）。**返回 0 一律不触发**，不猜语义。
 */
inline int64_t wf_sched_period_target(
    WfSchedTimerType type,
    int now_year, int now_mon, int now_day, int now_wday,
    int hour, int minute,
    int dow,
    int once_year, int once_mon, int once_day
)
{
    hour = wf_sched_clamp_int(hour, 0, 23);
    minute = wf_sched_clamp_int(minute, 0, 59);

    if(type == WF_SCHED_TYPE_ONCE)
    {
        // once：绝对日期。日期非法 ⇒ 拒绝（不借 mktime 的规范化"猜"出一天）
        if(once_mon < 1 || once_mon > 12) return 0;
        const int dim = wf_sched_days_in_month(once_year, once_mon);
        if(once_day < 1 || once_day > dim) return 0;
        return wf_sched_pseudo_epoch(once_year, once_mon, once_day, hour, minute, 0);
    }

    if(type == WF_SCHED_TYPE_DAILY || type == WF_SCHED_TYPE_WEEKLY)
    {
        if(type == WF_SCHED_TYPE_WEEKLY)
        {
            const int want = wf_sched_clamp_int(dow, 0, 6);
            if(now_wday != want) return 0;   // 今天不是目标星期 ⇒ 本周期无计划
        }
        return wf_sched_pseudo_epoch(now_year, now_mon, now_day, hour, minute, 0);
    }

    return 0;   // 未识别类型
}

/**
 * 是否应当在本轮触发。
 *   · 未到计划时刻 ⇒ 否
 *   · 已过窗口 ⇒ 否（**不补执行**：失效期/错过的窗口永远错过）
 *   · 本周期已触发（ledger == target）⇒ 否
 */
inline bool wf_sched_should_fire(int64_t now_pseudo, int64_t target, int64_t fired_target,
                                 unsigned window_sec)
{
    if(target <= 0) return false;
    if(now_pseudo < target) return false;
    if(now_pseudo >= target + (int64_t)window_sec) return false;
    if(fired_target == target) return false;
    return true;
}
