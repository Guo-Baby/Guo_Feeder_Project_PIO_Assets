// =====================================================
// probe.cpp —— 米家 LAN 动态发现 / RAM IP 归属决策 宿主侧探针
//
// 只编译纯头文件 src/app/xiaomi_lan_discover.h（零依赖，wasm32 freestanding）
// ⇒ 执行的是固件里同一份决策代码，不是复制品。
//
// 覆盖任务书 §3.1：
//   双设备同时发现 / 响应顺序变化 / 身份按 Token 归属 / IP 变化后重新发现 /
//   缓存有效与失效 / Token 错误 / 无响应 / 离线 / WiFi 重连 / 两台不互换 /
//   重启不依赖旧 IP / 候选去重与容量 / 不做整段扫描。
// =====================================================

#include "app/xiaomi_lan_discover.h"

namespace
{
char g_log[16384];
int  g_logLen = 0;
int  g_fail = 0;
int  g_total = 0;

void putc(char c)
{
    if (g_logLen < (int)sizeof(g_log) - 1) g_log[g_logLen++] = c;
}
void puts(const char *s) { while (*s) putc(*s++); }
void putInt(int v)
{
    char buf[12]; int n = 0;
    if (v < 0) { putc('-'); v = -v; }
    if (v == 0) { putc('0'); return; }
    while (v > 0 && n < 11) { buf[n++] = (char)('0' + v % 10); v /= 10; }
    while (n > 0) putc(buf[--n]);
}
void check(bool cond, const char *name)
{
    g_total++;
    puts(cond ? "PASS  " : "FAIL  ");
    puts(name);
    puts("\n");
    if (!cond) g_fail++;
}

using namespace xland;

const unsigned int IP_FEEDER = 0x0F7CA8C0u;   // 仅作逻辑标识，非真实地址
const unsigned int IP_PLUG   = 0x047CA8C0u;
const unsigned int IP_OTHER1 = 0x077CA8C0u;
const unsigned int IP_OTHER2 = 0x037CA8C0u;

/** 完整驱动一轮：对每个 (dev, cand) 组合调用 claimOnAuth，模拟真实状态机 */
int drive(Discovery &d, IpCache &c, const bool authOk[2][8])
{
    int claims = 0;
    d.phase = PhaseProbing;
    d.curDev = kDevFeeder;
    d.candIdx = 0;
    int guard = 0;
    while (guard++ < 64)
    {
        unsigned char dev = 0;
        unsigned int ip = 0;
        if (!nextProbe(d, &dev, &ip)) break;
        const bool ok = (d.candIdx < 8) ? authOk[dev][d.candIdx] : false;
        if (claimOnAuth(d, c, ok))
        {
            claims++;
            /* 该设备已认领 ⇒ 跳到下一台设备 */
            d.curDev++;
            d.candIdx = 0;
        }
        else
        {
            skipCandidate(d);
        }
    }
    d.phase = PhaseDone;
    return claims;
}

void fresh(Discovery &d, IpCache &c)
{
    resetDiscovery(d);
    resetCache(c);
}
} // namespace

extern "C" {

int probe_run(void)
{
    Discovery d; IpCache c;

    /* ---------- 1. 初始态：IP 只在 RAM，重启后必须为空 ---------- */
    fresh(d, c);
    check(!c.valid[kDevFeeder] && !c.valid[kDevPlug] && c.ip[0] == 0 && c.ip[1] == 0,
          "T01 重启后 RAM IP 缓存为空（不依赖 NVS 旧 IP）");
    check(!cacheUsable(c, kDevFeeder, true) && !cacheUsable(c, kDevPlug, true),
          "T02 未发现前缓存不可用");

    /* ---------- 2. 广播候选收集：去重 / 容量 / 非法地址 ---------- */
    fresh(d, c);
    requestDiscovery(d, kDevFeeder);
    requestDiscovery(d, kDevPlug);
    check(d.pending == kMaskBoth && d.phase == PhaseCollecting,
          "T03 两台设备同时请求发现");
    check(addCandidate(d, IP_FEEDER, 0x2670c3be, 100), "T04 首个候选新增");
    check(!addCandidate(d, IP_FEEDER, 0x2670c3be, 101), "T05 同一 IP 重复响应不新增");
    check(d.cand[0].hits == 2 && d.cand[0].stamp == 101, "T06 重复响应只更新会话参数与计数");
    check(!addCandidate(d, 0, 1, 1) && !addCandidate(d, 0xFFFFFFFFu, 1, 1),
          "T07 0 / 受限广播地址不算设备");
    addCandidate(d, IP_PLUG, 0x333a9260, 200);
    addCandidate(d, IP_OTHER1, 0x41f3e61c, 300);
    addCandidate(d, IP_OTHER2, 0x3dd94a08, 400);
    check(d.candCount == 4, "T08 同网段 4 台 miIO 设备全部登记为候选");
    for (unsigned int i = 0; i < 16; i++)
    {
        unsigned int fake = 0x0100A8C0u + i;
        addCandidate(d, fake, 0xAA, 500);
    }
    check(d.candCount == kMaxCandidates, "T09 候选容量硬上限 8，不引入大数组/整段扫描");

    /* ---------- 3. 身份按 Token 归属（不按响应顺序） ---------- */
    fresh(d, c);
    requestDiscovery(d, kDevFeeder);
    requestDiscovery(d, kDevPlug);
    /* 故意让「其它设备」排在前面，且 plug 排在 feeder 前面 */
    addCandidate(d, IP_OTHER1, 0x41f3e61c, 1);
    addCandidate(d, IP_PLUG,   0x333a9260, 2);
    addCandidate(d, IP_OTHER2, 0x3dd94a08, 3);
    addCandidate(d, IP_FEEDER, 0x2670c3be, 4);
    bool auth1[2][8] = {};
    auth1[kDevFeeder][3] = true;   // feeder Token 只认第 4 个候选
    auth1[kDevPlug][1]   = true;   // plug Token 只认第 2 个候选
    int claims = drive(d, c, auth1);
    check(claims == 2 && d.pending == 0, "T10 两台设备同时被发现且各自认领");
    check(c.ip[kDevFeeder] == IP_FEEDER && c.valid[kDevFeeder],
          "T11 喂食器 IP 来自其 Token 认证通过的候选（非响应顺序）");
    check(c.ip[kDevPlug] == IP_PLUG && c.valid[kDevPlug],
          "T12 插座 IP 来自其 Token 认证通过的候选（非响应顺序）");
    check(c.ip[kDevFeeder] != c.ip[kDevPlug], "T13 两台设备 IP 不互换");

    /* 顺序颠倒再来一轮 */
    fresh(d, c);
    requestDiscovery(d, kDevFeeder);
    requestDiscovery(d, kDevPlug);
    addCandidate(d, IP_FEEDER, 0x2670c3be, 1);
    addCandidate(d, IP_OTHER2, 0x3dd94a08, 2);
    addCandidate(d, IP_OTHER1, 0x41f3e61c, 3);
    addCandidate(d, IP_PLUG,   0x333a9260, 4);
    bool auth2[2][8] = {};
    auth2[kDevFeeder][0] = true;
    auth2[kDevPlug][3]   = true;
    drive(d, c, auth2);
    check(c.ip[kDevFeeder] == IP_FEEDER && c.ip[kDevPlug] == IP_PLUG,
          "T14 响应顺序颠倒后身份仍正确");

    /* ---------- 4. 认证失败 / 无响应 / 离线 ---------- */
    fresh(d, c);
    requestDiscovery(d, kDevFeeder);
    addCandidate(d, IP_OTHER1, 1, 1);
    addCandidate(d, IP_OTHER2, 2, 2);
    bool badTok[2][8] = {};   // 全错 Token
    check(drive(d, c, badTok) == 0, "T15 Token 错误 ⇒ 零认领");
    check(!c.valid[kDevFeeder] && c.ip[kDevFeeder] == 0, "T16 认证失败不更新有效 IP");
    check(!c.valid[kDevPlug] && c.ip[kDevPlug] == 0, "T17 认证失败不波及其它设备");
    check(needsDiscovery(d, kDevFeeder), "T18 失败后该设备仍处于待发现");

    fresh(d, c);
    requestDiscovery(d, kDevFeeder);
    check(d.candCount == 0, "T19 设备离线 / 无任何 UDP 响应 ⇒ 候选列表为空");
    bool none[2][8] = {};
    drive(d, c, none);
    check(!c.valid[kDevFeeder] && c.ip[kDevFeeder] == 0,
          "T20 无响应不得把旧 IP 当有效地址");
    check(discoveryFinished(d), "T21 无候选时本轮发现结束（不进入整段扫描）");

    /* ---------- 5. 缓存有效 / 失效 / IP 变化 ---------- */
    fresh(d, c);
    c.ip[kDevFeeder] = IP_FEEDER; c.valid[kDevFeeder] = true;
    c.ip[kDevPlug]   = IP_PLUG;   c.valid[kDevPlug]   = true;
    check(cacheUsable(c, kDevFeeder, true) && cacheUsable(c, kDevPlug, true),
          "T22 缓存值本身物理可信（cacheUsable 只管值，不代表可跳过发现）");
    check(!cacheUsable(c, kDevFeeder, false), "T23 WiFi 断开时缓存不可用");
    /* 模拟 DHCP 换址：旧 IP 通信超时 */
    invalidateOne(c, d, kDevFeeder);
    check(!c.valid[kDevFeeder] && c.ip[kDevFeeder] == 0, "T24 旧 IP 失效后立即作废");
    check(c.valid[kDevPlug] && c.ip[kDevPlug] == IP_PLUG,
          "T25 单台失效不影响另一台缓存");
    check(needsDiscovery(d, kDevFeeder) && !needsDiscovery(d, kDevPlug),
          "T26 只重新发现失效的那台");
    /* 重新发现到新 IP */
    d.curDev = kDevFeeder; d.candIdx = 0;
    addCandidate(d, 0x117CA8C0u, 0x2670c3be, 9);
    bool redisc[2][8] = {};
    redisc[kDevFeeder][0] = true;
    drive(d, c, redisc);
    check(c.ip[kDevFeeder] == 0x117CA8C0u && c.valid[kDevFeeder],
          "T27 IP 变化后自动发现并恢复（模拟 DHCP 换址）");

    /* ---------- 6. WiFi 断连 / 重连 / 本机换址 ---------- */
    check(!networkInvalidates(true, true, 0x087CA8C0u, 0x087CA8C0u),
          "T28 网络无变化时不误失效");
    check(networkInvalidates(true, false, 0x087CA8C0u, 0x087CA8C0u),
          "T29 WiFi 断开 ⇒ 缓存失效");
    check(networkInvalidates(false, true, 0x087CA8C0u, 0x087CA8C0u),
          "T30 WiFi 重新连接 ⇒ 缓存失效");
    check(networkInvalidates(true, true, 0x087CA8C0u, 0x097CA8C0u),
          "T31 本机 IP 变化（切 AP / DHCP）⇒ 缓存失效");
    check(!networkInvalidates(false, false, 0x087CA8C0u, 0x097CA8C0u),
          "T32 未连接时的 IP 抖动不触发额外失效");
    invalidateAll(c, d);
    check(!c.valid[0] && !c.valid[1] && c.ip[0] == 0 && c.ip[1] == 0,
          "T33 网络变化后两台设备 IP 全部清空");
    check(d.pending == kMaskBoth, "T34 网络变化后两台都重新排队发现");

    /* ---------- 7. 防串用 ---------- */
    fresh(d, c);
    requestDiscovery(d, kDevFeeder);
    requestDiscovery(d, kDevPlug);
    addCandidate(d, IP_FEEDER, 1, 1);
    c.ip[kDevPlug] = IP_FEEDER; c.valid[kDevPlug] = true;   // 已被插座认领
    d.curDev = kDevFeeder; d.candIdx = 0;
    bool same[2][8] = {}; same[kDevFeeder][0] = true;
    check(!claimOnAuth(d, c, true), "T35 同一 IP 不允许被两台设备同时认领");
    check(!c.valid[kDevFeeder], "T36 串用被拒后该设备 IP 仍为空");

    /* ---------- 8. 部分成功 / 单台设备发现 ---------- */
    fresh(d, c);
    requestDiscovery(d, kDevFeeder);   // 只有一台需要发现
    addCandidate(d, IP_FEEDER, 1, 1);
    bool one[2][8] = {}; one[kDevFeeder][0] = true;
    drive(d, c, one);
    check(c.valid[kDevFeeder] && !c.valid[kDevPlug],
          "T37 只发现一台时另一台保持未发现状态");
    check(d.pending == 0 && discoveryFinished(d), "T38 部分目标完成即结束本轮");

    /* ---------- 9. 认证失败不得改动 Token 配置 ---------- */
    fresh(d, c);
    bool tokenCfg[2] = { true, true };   // 模拟 NVS 中的 Token 存在标志
    requestDiscovery(d, kDevFeeder);
    addCandidate(d, IP_OTHER1, 1, 1);
    drive(d, c, badTok);
    invalidateOne(c, d, kDevFeeder);
    check(tokenCfg[0] && tokenCfg[1],
          "T39 设备无响应 / 认证失败 / 缓存失效都不清空 NVS Token");

    /* ---------- 10. 每次独立操作必须重新发现（2026-11-05 任务书 §2.1/§2.2） ---------- */
    fresh(d, c);
    c.ip[kDevFeeder] = IP_FEEDER; c.valid[kDevFeeder] = true;
    c.ip[kDevPlug]   = IP_PLUG;   c.valid[kDevPlug]   = true;
    beginOperation(c, d, kDevFeeder);
    check(!c.valid[kDevFeeder] && c.ip[kDevFeeder] == 0,
          "T40 新操作开始 ⇒ 无条件作废该设备 RAM IP（即使缓存有效）");
    check(!cacheUsable(c, kDevFeeder, true),
          "T41 作废后本设备缓存不可用（缓存不是跳过发现的理由）");
    check(c.valid[kDevPlug] && c.ip[kDevPlug] == IP_PLUG,
          "T42 只作废本设备，另一台缓存原样保留");
    check(needsDiscovery(d, kDevFeeder) && !needsDiscovery(d, kDevPlug),
          "T43 只把本设备重新排入待发现集合");

    /* 连续两次独立操作：第二次仍必须重新发现 */
    addCandidate(d, IP_FEEDER, 1, 1);
    bool op1[2][8] = {}; op1[kDevFeeder][0] = true;
    drive(d, c, op1);
    check(c.valid[kDevFeeder] && c.ip[kDevFeeder] == IP_FEEDER,
          "T44 第一次操作发现并认领成功");
    beginOperation(c, d, kDevFeeder);
    check(!c.valid[kDevFeeder] && c.ip[kDevFeeder] == 0,
          "T45 第二次独立操作前仍强制作废（不因上轮成功而跳过发现）");

    /* 在途轮次不被 beginOperation 打断：只并入，不重置游标/候选 */
    fresh(d, c);
    requestDiscovery(d, kDevFeeder);
    addCandidate(d, IP_OTHER1, 1, 1);
    addCandidate(d, IP_FEEDER, 2, 2);
    d.phase = PhaseProbing; d.curDev = kDevFeeder; d.candIdx = 1;
    const unsigned char candBefore = d.candCount;
    beginOperation(c, d, kDevPlug);
    check(d.phase == PhaseProbing && d.candCount == candBefore && d.curDev == kDevFeeder,
          "T46 在途轮次: 新操作只并入（不打断已收集的候选与游标）");
    check(needsDiscovery(d, kDevPlug), "T47 在途轮次: 新设备已排入本轮待发现");

    /* 轮次结束态 ⇒ beginOperation 让发现层回到 Collecting */
    d.phase = PhaseDone; d.pending = 0;
    beginOperation(c, d, kDevPlug);
    check(d.phase == PhaseCollecting, "T48 轮次已结束 ⇒ 新操作重新拉起发现");

    /* 本轮"没试过"与"试过但失败"必须区分（决定是否补轮） */
    check(roundMissedDevice(kMaskFeeder, kMaskPlug),
          "T49 本轮没试过该设备 ⇒ 判为 missed（应补轮）");
    check(!roundMissedDevice(kMaskBoth, kMaskPlug),
          "T50 本轮试过但认证失败 ⇒ 不算 missed（有界补轮，不能刷广播）");

    puts("TOTAL="); putInt(g_total); puts(" FAIL="); putInt(g_fail); puts("\n");
    return g_fail;
}

const char *probe_log_ptr(void) { return g_log; }
int probe_log_len(void) { return g_logLen; }

} // extern "C"
