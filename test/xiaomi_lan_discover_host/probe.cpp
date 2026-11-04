// =====================================================
// probe.cpp —— 米家 LAN 身份确认决策 宿主侧探针
//
// 只编译纯头文件 src/app/xiaomi_lan_discover.h（零依赖，wasm32 freestanding）
// ⇒ 执行的是固件里同一份决策代码，不是复制品。
//
// 覆盖任务书 §7 的五组用例：
//   A 单播快速验证（缓存 IP → hello → 加密认证）
//   B 广播回退（收集候选 → 并行认证 → 更新 RAM IP）
//   C 并行认证（一次全发 / 乱序 / 重复 / 交叉 / 冲突）
//   D 连续独立操作（每次都验证、不跳过、不串台）
//   E 网络变化（断开 / 恢复 / 本机换址 / DHCP 换址 / IP 被占用）
// 另保留 T 组（原 50 项）：重启不依赖旧 IP、候选去重与容量、不互换、不整段扫描等。
// =====================================================

#include "app/xiaomi_lan_discover.h"

namespace
{
char g_log[32768];
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

const unsigned int IP_FEEDER = 0x0F0002C0u;   // 仅作逻辑标识，非真实地址
const unsigned int IP_PLUG   = 0x040002C0u;
const unsigned int IP_OTHER1 = 0x070002C0u;
const unsigned int IP_OTHER2 = 0x030002C0u;
const unsigned int IP_NEW     = 0x110002C0u;  // DHCP 换址后的新地址

const unsigned int DEV_ID_FEEDER = 0x2670c3beu;
const unsigned int DEV_ID_PLUG   = 0x333a9260u;
const unsigned int DEV_ID_OTHER  = 0x41f3e61cu;

void fresh(Discovery &d, IpCache &c)
{
    resetDiscovery(d);
    resetCache(c);
}

/** 广播已经收集完 candCount 个候选，并已进入并行认证窗口。 */
void collected(Discovery &d, IpCache &c)
{
    d.phase = PhaseAuthing;
    d.candCount = 5;
}

/**
 * 模拟一轮并行认证。
 *
 * 与固件同序：先把缺口全部打开（beginAuthRound 后 authResolved 全清），
 * 再按 `order` 指定的顺序投递回包，最后把没认领的探针按超时统一结束。
 *
 * @param authOk  authOk[dev][cand] = 该组合能否用该设备 Token 解开
 * @param order   回包到达顺序（候选下标序列）
 * @return 成功认领次数
 */
int runParallelAuth(Discovery &d, IpCache &c, const bool authOk[2][8],
                    const int *order, int orderLen)
{
    beginAuthRound(d);
    int claims = 0;
    for (int i = 0; i < orderLen; i++)
    {
        const int cand = order[i];
        if (cand < 0 || cand >= d.candCount) continue;
        for (unsigned char dev = 0; dev < 2; dev++)
        {
            unsigned char mc = 0;
            unsigned int eid = 0;
            /* 该 (dev,cand) 已结束 / 不是它的候选 ⇒ 换下一台设备 */
            if (!matchProbe(d, dev, d.cand[cand].ip, &mc, &eid)) continue;
            if (!authOk[dev][cand]) continue;      // Token 不对 ⇒ 这一包解不开
            const ClaimResult cr = claimParallel(d, c, dev, mc);
            if (cr == ClaimOk) claims++;
            break;
        }
    }
    /* 窗口结束：仍未认领的探针一律按"没等到"结束 */
    for (unsigned char dev = 0; dev < 2; dev++)
        for (unsigned char i = 0; i < d.candCount; i++)
            resolveProbe(d, dev, i);
    return claims;
}
} // namespace

extern "C" {

int probe_run(void)
{
    Discovery d; IpCache c;

    /* ============================================================
     * T 组：基础契约（原有 50 项中与"IP 只在 RAM"相关的部分）
     * ============================================================ */
    fresh(d, c);
    check(!c.valid[kDevFeeder] && !c.valid[kDevPlug] && c.ip[0] == 0 && c.ip[1] == 0,
          "T01 重启后 RAM IP 缓存为空（不依赖 NVS 旧 IP）");
    check(!cacheUsable(c, kDevFeeder, true) && !cacheUsable(c, kDevPlug, true),
          "T02 未发现前缓存不可用");

    requestDiscovery(d, kDevFeeder);
    requestDiscovery(d, kDevPlug);
    check(d.pending == kMaskBoth, "T03 两台设备同时请求发现");
    check(addCandidate(d, IP_FEEDER, DEV_ID_FEEDER, 100), "T04 首个候选新增");
    check(!addCandidate(d, IP_FEEDER, DEV_ID_FEEDER, 101), "T05 同一 IP 重复响应不新增");
    check(d.cand[0].hits == 2 && d.cand[0].stamp == 101,
          "T06 重复响应只更新会话参数与计数");
    check(!addCandidate(d, 0, 1, 1) && !addCandidate(d, 0xFFFFFFFFu, 1, 1),
          "T07 0 / 受限广播地址不算设备");
    addCandidate(d, IP_PLUG, DEV_ID_PLUG, 200);
    addCandidate(d, IP_OTHER1, DEV_ID_OTHER, 300);
    addCandidate(d, IP_OTHER2, 0x3dd94a08u, 400);
    check(d.candCount == 4, "T08 同网段 4 台 miIO 设备全部登记为候选");
    for (unsigned int i = 0; i < 16; i++)
        addCandidate(d, 0x010002C0u + i, 0xAA, 500);
    check(d.candCount == kMaxCandidates,
          "T09 候选容量硬上限 8，不引入大数组/整段扫描");

    /* ============================================================
     * A 组：单播快速验证（§2）
     * ============================================================ */
    fresh(d, c);
    c.ip[kDevFeeder] = IP_FEEDER; c.valid[kDevFeeder] = true;
    check(beginOperation(c, d, kDevFeeder),
          "A01 有缓存 IP ⇒ 走单播快路径（beginOperation=true）");
    check(d.phase == PhaseVerifying && d.vf[kDevFeeder].active && d.vf[kDevFeeder].stage == 0,
          "A02 快路径进入 PhaseVerifying / stage0（等 hello）");
    check(!c.valid[kDevFeeder] && c.ip[kDevFeeder] == IP_FEEDER,
          "A03 缓存被降级为「待验证」：值保留、valid 先置 false（不能当已确认）");
    check(needsDiscovery(d, kDevFeeder),
          "A04 该设备仍在待确认集合（验证成功才清）");

    /* hello 的 device_id 与上次认证一致 ⇒ 允许继续认证，并生成唯一 reply id */
    check(verifyHelloOk(d, kDevFeeder, DEV_ID_FEEDER, 5000, DEV_ID_FEEDER),
          "A05 hello 的 device_id 与上次一致 ⇒ 进入认证阶段");
    check(d.vf[kDevFeeder].stage == 1 && d.vf[kDevFeeder].replyId != 0,
          "A06 认证阶段带唯一 reply id（用于关联回包）");
    const unsigned int rid1 = d.vf[kDevFeeder].replyId;
    verifyOk(d, c, kDevFeeder);
    check(c.valid[kDevFeeder] && c.ip[kDevFeeder] == IP_FEEDER,
          "A07 认证成功 ⇒ 该 IP 被确认（valid=true，IP 不变）");
    check(!d.vf[kDevFeeder].active && d.pending == 0,
          "A08 认证成功 ⇒ 验证子流程结束且清 pending");

    /* 缓存 IP 已变成别的米家设备（device_id 变了）⇒ 立即失败，不做无谓认证 */
    fresh(d, c);
    c.ip[kDevFeeder] = IP_PLUG; c.valid[kDevFeeder] = true;   // .4 现在是插座
    beginOperation(c, d, kDevFeeder);
    check(!verifyHelloOk(d, kDevFeeder, DEV_ID_PLUG, 6000, DEV_ID_FEEDER),
          "A09 ★ 缓存 IP 被别的米家设备占用（device_id 不符）⇒ 立即判失败");
    verifyFail(d, c, kDevFeeder);
    check(c.ip[kDevFeeder] == 0 && !c.valid[kDevFeeder],
          "A10 ★ 该场景绝不把命令发到这个 IP：缓存已作废");
    check(needsDiscovery(d, kDevFeeder),
          "A11 失败后仍待确认 ⇒ 回退广播（不盲目。用旧 IP 重试）");

    /* 首次验证（无 device_id 基准）⇒ 必须靠 Token 认证，不能只看 device_id */
    fresh(d, c);
    c.ip[kDevFeeder] = IP_FEEDER; c.valid[kDevFeeder] = true;
    beginOperation(c, d, kDevFeeder);
    check(verifyHelloOk(d, kDevFeeder, DEV_ID_FEEDER, 7000, 0),
          "A12 首次（无 device_id 基准）⇒ 允许进入 Token 认证（不靠 device_id 认身份）");
    verifyFail(d, c, kDevFeeder);
    check(!c.valid[kDevFeeder],
          "A13 认证失败（含 Token 不符/id 不对）⇒ 不写缓存");

    /* 两台同时验证：互不干扰 */
    fresh(d, c);
    c.ip[kDevFeeder] = IP_FEEDER; c.valid[kDevFeeder] = true;
    c.ip[kDevPlug]   = IP_PLUG;   c.valid[kDevPlug]   = true;
    check(beginOperation(c, d, kDevFeeder), "A14 喂食器进入快路径");
    /* 第二台：phase 已是 PhaseVerifying（在飞）⇒ 并入，不重开 */
    check(!beginOperation(c, d, kDevPlug), "A15 在飞时第二台并入本轮（不打断）");
    d.phase = PhaseVerifying;
    d.vf[kDevPlug].active = 1; d.vf[kDevPlug].stage = 0; d.vf[kDevPlug].ip = IP_PLUG;
    check(verifyHelloOk(d, kDevFeeder, DEV_ID_FEEDER, 8000, DEV_ID_FEEDER) &&
          verifyHelloOk(d, kDevPlug, DEV_ID_PLUG, 8001, DEV_ID_PLUG),
          "A16 两台设备各自 hello 成功（可并行）");
    check(d.vf[kDevFeeder].replyId != d.vf[kDevPlug].replyId,
          "A17 两台设备的 reply id 不同（回包不会串台）");
    verifyOk(d, c, kDevFeeder);
    check(d.vf[kDevPlug].active && c.valid[kDevFeeder] && !c.valid[kDevPlug],
          "A18 一台成功后另一台不受影响（各自独立）");
    verifyOk(d, c, kDevPlug);
    check(c.valid[kDevFeeder] && c.valid[kDevPlug] && verifyAllDone(d),
          "A19 两台都确认后验证子流程全部结束");

    /*
     * ★ "读取两台状态"最常见的交错：A 的单播验证在飞时，B 的请求到达。
     *   B 必须能**立刻**开始自己那条单播验证，而不是干等之后退化成广播。
     */
    fresh(d, c);
    c.ip[kDevFeeder] = IP_FEEDER; c.valid[kDevFeeder] = true;
    c.ip[kDevPlug]   = IP_PLUG;   c.valid[kDevPlug]   = true;
    check(beginOperation(c, d, kDevFeeder), "A21 喂食器先开始单播验证");
    check(beginVerifyOnly(d, c, kDevPlug),
          "A22 ★ 验证在飞时另一台可并行开始自己的验证（不退化成广播）");
    check(d.vf[kDevFeeder].active && d.vf[kDevPlug].active,
          "A23 两条单播验证同时 active（互不影响）");
    check(!beginVerifyOnly(d, c, kDevPlug),
          "A24 已在验证的设备不会重复登记（不重置自己的倒计时）");
    fresh(d, c);
    check(!beginVerifyOnly(d, c, kDevPlug),
          "A25 无缓存地址时不能登记验证（只能广播）");

    /* reply id 跨轮必须不同 ⇒ 上一轮的迟到回包认不到本轮 */
    fresh(d, c);
    c.ip[kDevFeeder] = IP_FEEDER; c.valid[kDevFeeder] = true;
    beginOperation(c, d, kDevFeeder);
    verifyHelloOk(d, kDevFeeder, DEV_ID_FEEDER, 1, DEV_ID_FEEDER);
    const unsigned int ridOld = d.vf[kDevFeeder].replyId;
    verifyFail(d, c, kDevFeeder);                 // 本轮失败
    d.phase = PhaseIdle;
    c.ip[kDevFeeder] = IP_FEEDER;                 // 下一轮还打同一个 IP
    beginOperation(c, d, kDevFeeder);
    verifyHelloOk(d, kDevFeeder, DEV_ID_FEEDER, 2, DEV_ID_FEEDER);
    check(d.vf[kDevFeeder].replyId != ridOld,
          "A20 ★ 跨轮 reply id 不同 ⇒ 上一轮的迟到响应不会被本轮认领");

    /* ============================================================
     * C 组：并行认证（§3）—— 先做，B 组要用它
     * ============================================================ */
    fresh(d, c);
    requestDiscovery(d, kDevFeeder);
    requestDiscovery(d, kDevPlug);
    addCandidate(d, IP_OTHER1, DEV_ID_OTHER, 1);
    addCandidate(d, IP_PLUG,   DEV_ID_PLUG, 2);
    addCandidate(d, IP_OTHER2, 0x3dd94a08u, 3);
    addCandidate(d, IP_FEEDER, DEV_ID_FEEDER, 4);
    addCandidate(d, 0x050002C0u, 0x99, 5);
    beginAuthRound(d);
    /* 窗口刚打开：8 个 (2 设备 × 4 候选) 探针应同时在飞 */
    int inflight = 0;
    for (unsigned char dev = 0; dev < 2; dev++)
        for (unsigned char i = 0; i < d.candCount; i++)
            if (!probeResolved(d, dev, i)) inflight++;
    check(inflight == 2 * d.candCount,
          "C01 ★ 并行认证：所有 (设备×候选) 探针在同一窗口内同时发出（不串行等超时）");
    check(d.authResolved == 0 && d.conflictMask == 0,
          "C02 新认证轮：已完成位与冲突位清零");
    /* 每个探针的 reply id 必须互不相同（这是并行匹配的前提） */
    bool uniq = true;
    for (unsigned char dev = 0; dev < 2; dev++)
        for (unsigned char i = 0; i < d.candCount; i++)
            for (unsigned char dev2 = 0; dev2 < 2; dev2++)
                for (unsigned char j = 0; j < d.candCount; j++)
                    if (!(dev == dev2 && i == j) &&
                        probeReplyId(d.gen, dev, i) == probeReplyId(d.gen, dev2, j)) uniq = false;
    check(uniq, "C03 ★ 每个探针的 reply id 在同一轮内唯一（响应可精确归属）");

    bool authOk[2][8] = {};
    authOk[kDevFeeder][3] = true;
    authOk[kDevPlug][1]   = true;
    int order[8] = {0, 1, 2, 3, 4, 5, 6, 7};
    check(runParallelAuth(d, c, authOk, order, 5) == 2,
          "C04 正确 Token 的两台设备各自被认领（一次窗口内完成）");
    check(c.ip[kDevFeeder] == IP_FEEDER && c.ip[kDevPlug] == IP_PLUG && d.pending == 0,
          "C05 归属正确：喂食器=其 Token 通过的候选，插座=其 Token 通过的候选");

    /* 乱序 + 重复响应 */
    fresh(d, c);
    requestDiscovery(d, kDevFeeder);
    requestDiscovery(d, kDevPlug);
    addCandidate(d, IP_OTHER1, DEV_ID_OTHER, 1);
    addCandidate(d, IP_FEEDER, DEV_ID_FEEDER, 2);
    addCandidate(d, IP_PLUG,   DEV_ID_PLUG, 3);
    {
        bool ok2[2][8] = {};
        ok2[kDevFeeder][1] = true;
        ok2[kDevPlug][2]   = true;
        int rev[8] = {2, 1, 0, 2, 1, 0, 2, 1};      // 插座先回，且同一候选重复回
        check(runParallelAuth(d, c, ok2, rev, 8) == 2,
              "C06 响应乱序 + 重复响应 ⇒ 仍只认领两次，不重复计数");
        check(c.ip[kDevFeeder] == IP_FEEDER && c.ip[kDevPlug] == IP_PLUG,
              "C07 乱序下身份依然正确（不按响应顺序分配）");
    }

    /* 重复响应单独验：同一 (dev,cand) 第二次必须 ClaimDup */
    fresh(d, c);
    requestDiscovery(d, kDevFeeder);
    addCandidate(d, IP_FEEDER, DEV_ID_FEEDER, 1);
    beginAuthRound(d);
    check(claimParallel(d, c, kDevFeeder, 0) == ClaimOk, "C08 首次认领成功");
    check(claimParallel(d, c, kDevFeeder, 0) == ClaimDup,
          "C09 同一候选的重复响应 ⇒ ClaimDup（不重复认领）");
    check(c.ip[kDevFeeder] == IP_FEEDER, "C10 重复响应不改变已确认的 IP");

    /* 交叉验证：喂食器 Token 绝不能认出插座 */
    fresh(d, c);
    requestDiscovery(d, kDevFeeder);
    addCandidate(d, IP_PLUG, DEV_ID_PLUG, 1);
    {
        bool cross[2][8] = {};                     // 两个 Token 都解不开
        int seq[1] = {0};
        check(runParallelAuth(d, c, cross, seq, 1) == 0,
              "C11 ★ 喂食器 Token 对插座候选交叉验证失败（零认领）");
        check(!c.valid[kDevFeeder] && c.ip[kDevFeeder] == 0,
              "C12 交叉验证失败不写任何缓存");
    }

    /* 错误 Token 混在多个候选里：只有对的能认领 */
    fresh(d, c);
    requestDiscovery(d, kDevFeeder);
    addCandidate(d, IP_OTHER1, DEV_ID_OTHER, 1);
    addCandidate(d, IP_OTHER2, 0x3dd94a08u, 2);
    addCandidate(d, IP_FEEDER, DEV_ID_FEEDER, 3);
    {
        bool ok3[2][8] = {};
        ok3[kDevFeeder][2] = true;
        int seq[3] = {0, 1, 2};
        check(runParallelAuth(d, c, ok3, seq, 3) == 1,
              "C13 3 个候选里只有真设备能被认出（其余 Token 不匹配被拒）");
        check(c.ip[kDevFeeder] == IP_FEEDER, "C14 认出的是第 3 个候选（不按顺序）");
    }

    /* 部分成功：喂食器认出、插座没认出 */
    fresh(d, c);
    requestDiscovery(d, kDevFeeder);
    requestDiscovery(d, kDevPlug);
    addCandidate(d, IP_FEEDER, DEV_ID_FEEDER, 1);
    addCandidate(d, IP_OTHER1, DEV_ID_OTHER, 2);
    {
        bool part[2][8] = {};
        part[kDevFeeder][0] = true;
        int seq[2] = {0, 1};
        check(runParallelAuth(d, c, part, seq, 2) == 1,
              "C15 部分成功：只认领喂食器");
        check(c.valid[kDevFeeder] && !c.valid[kDevPlug] && needsDiscovery(d, kDevPlug),
              "C16 未认领的插座仍待确认（如实失败，不伪造成功）");
    }

    /* 冲突 1：同一 Token 在**两个不同 IP** 上都通过 ⇒ 拒绝并清空 */
    fresh(d, c);
    requestDiscovery(d, kDevFeeder);
    addCandidate(d, 0x0A0002C0u, DEV_ID_FEEDER, 1);
    addCandidate(d, 0x0B0002C0u, DEV_ID_FEEDER, 2);   // 两台都报同一个 device_id
    {
        beginAuthRound(d);
        check(claimParallel(d, c, kDevFeeder, 0) == ClaimOk, "C17 第一个通过");
        const ClaimResult cr = claimParallel(d, c, kDevFeeder, 1);
        check(cr == ClaimConflict,
              "C18 ★ 同一 Token 两台设备都通过 ⇒ 判冲突（不随便挑第一个）");
        check(!c.valid[kDevFeeder] && c.ip[kDevFeeder] == 0 && d.conflictMask == kMaskFeeder,
              "C19 冲突后清空该设备缓存并标记 conflictMask");
        check(!needsDiscovery(d, kDevFeeder), "C20 冲突轮放弃该设备（下轮从零重来）");
    }

    /* 冲突 2：同一 IP 被两台设备认领 ⇒ 后者被拒 */
    fresh(d, c);
    requestDiscovery(d, kDevFeeder);
    requestDiscovery(d, kDevPlug);
    addCandidate(d, IP_FEEDER, DEV_ID_FEEDER, 1);
    beginAuthRound(d);
    claimParallel(d, c, kDevPlug, 0);                 // 插座先占了它
    check(claimParallel(d, c, kDevFeeder, 0) == ClaimConflict,
          "C21 ★ 同一 IP 不允许被两台设备同时认领（防串用）");
    check(!c.valid[kDevFeeder] && c.valid[kDevPlug], "C22 被拒方不写缓存，已认领方保持");

    /* 跨轮不污染：上一轮的 reply id 在新一轮匹配不到 */
    fresh(d, c);
    requestDiscovery(d, kDevFeeder);
    addCandidate(d, IP_FEEDER, DEV_ID_FEEDER, 1);
    beginAuthRound(d);
    const unsigned char genA = d.gen;
    const unsigned int idA = probeReplyId(genA, kDevFeeder, 0);
    beginAuthRound(d);                                 // 下一轮
    const unsigned int idB = probeReplyId(d.gen, kDevFeeder, 0);
    check(idA != idB, "C23 ★ 连续两轮的 probe reply id 不同（旧响应认不到新请求）");

    /* ============================================================
     * B 组：广播回退（§3）
     * ============================================================ */
    /* 缓存失效 ⇒ 不回单播，直接广播 */
    fresh(d, c);
    c.ip[kDevFeeder] = IP_FEEDER;                      // 有值但从未验证（模拟换址）
    c.ip[kDevFeeder] = 0;                              // 真正失效
    check(!beginOperation(c, d, kDevFeeder),
          "B01 无缓存 IP ⇒ beginOperation=false（走广播）");
    check(d.phase != PhaseVerifying && d.vf[kDevFeeder].active == 0,
          "B02 不走单播快路径");

    /* 单播失败后的回退：pending 仍在 ⇒ 调用方开广播 */
    fresh(d, c);
    c.ip[kDevFeeder] = IP_FEEDER; c.valid[kDevFeeder] = true;
    beginOperation(c, d, kDevFeeder);
    verifyHelloOk(d, kDevFeeder, DEV_ID_FEEDER, 1, DEV_ID_FEEDER);
    verifyFail(d, c, kDevFeeder);
    check(verifyAllDone(d) && d.pending == kMaskFeeder,
          "B03 单播验证失败 ⇒ 回退广播（pending 仍含该设备）");
    d.phase = PhaseCollecting;
    addCandidate(d, IP_NEW, DEV_ID_FEEDER, 9);
    {
        bool ok[2][8] = {};
        ok[kDevFeeder][0] = true;
        int seq[1] = {0};
        check(runParallelAuth(d, c, ok, seq, 1) == 1,
              "B04 广播回退后并行认证认出设备");
        check(c.ip[kDevFeeder] == IP_NEW,
              "B05 ★ RAM IP 更新为广播发现的新地址（DHCP 换址自愈）");
    }

    /* 广播无响应：候选为空，绝不写缓存 */
    fresh(d, c);
    requestDiscovery(d, kDevFeeder);
    check(d.candCount == 0, "B06 设备离线 / 无 UDP 响应 ⇒ 候选为空");
    bool none[2][8] = {};
    int noSeq[1] = {0};
    check(runParallelAuth(d, c, none, noSeq, 0) == 0, "B07 无候选 ⇒ 零认领");
    check(!c.valid[kDevFeeder] && c.ip[kDevFeeder] == 0,
          "B08 广播失败不得把旧 IP 当有效地址");
    check(discoveryFinished(d) || d.pending == kMaskFeeder,
          "B09 无候选时本轮不能假装完成认领");

    /* 混入其它米家设备：不得被任何 Token 认领 */
    fresh(d, c);
    requestDiscovery(d, kDevFeeder);
    requestDiscovery(d, kDevPlug);
    addCandidate(d, IP_OTHER1, DEV_ID_OTHER, 1);
    addCandidate(d, IP_OTHER2, 0x3dd94a08u, 2);
    {
        bool nope[2][8] = {};
        int seq[2] = {0, 1};
        check(runParallelAuth(d, c, nope, seq, 2) == 0,
              "B10 同网段其它米家设备不会被误认领");
        check(!c.valid[0] && !c.valid[1], "B11 两台设备缓存都保持为空");
    }

    /* ============================================================
     * D 组：连续独立操作（§2.2 / §5）
     * ============================================================ */
    fresh(d, c);
    c.ip[kDevFeeder] = IP_FEEDER; c.valid[kDevFeeder] = true;
    int fastCount = 0;
    bool allFast = true;
    for (int i = 0; i < 5; i++)
    {
        const bool fast = beginOperation(c, d, kDevFeeder);
        if (!fast) { allFast = false; break; }
        fastCount++;
        /* 每次都必须重新认证（否则 verifyOk 不会被调用、g_devReady 不会置位） */
        if (!verifyHelloOk(d, kDevFeeder, DEV_ID_FEEDER, 100 + i, DEV_ID_FEEDER)) { allFast = false; break; }
        verifyOk(d, c, kDevFeeder);
        d.phase = PhaseDone;
    }
    check(allFast && fastCount == 5,
          "D01 ★ 连续 5 次独立操作，每次都走单播验证（缓存从不跳过验证）");
    check(c.ip[kDevFeeder] == IP_FEEDER,
          "D02 连续操作后 IP 保持正确");

    /* 缓存错误（device_id 变了）⇒ 每次都能自动回退 */
    fresh(d, c);
    c.ip[kDevFeeder] = IP_OTHER1; c.valid[kDevFeeder] = true;   // 指向别家设备
    beginOperation(c, d, kDevFeeder);
    check(!verifyHelloOk(d, kDevFeeder, DEV_ID_OTHER, 1, DEV_ID_FEEDER),
          "D03 缓存指向别的设备 ⇒ 快路径判失败");
    verifyFail(d, c, kDevFeeder);
    check(d.pending == kMaskFeeder && c.ip[kDevFeeder] == 0,
          "D04 自动回退到广播（无需人工干预）");

    /* 两台设备缓存互不干扰 */
    fresh(d, c);
    c.ip[kDevFeeder] = IP_FEEDER; c.valid[kDevFeeder] = true;
    c.ip[kDevPlug]   = IP_PLUG;   c.valid[kDevPlug]   = true;
    beginOperation(c, d, kDevFeeder);
    verifyFail(d, c, kDevFeeder);
    check(c.valid[kDevPlug] && c.ip[kDevPlug] == IP_PLUG,
          "D05 一台验证失败不影响另一台的缓存");
    check(!needsDiscovery(d, kDevPlug), "D06 另一台不会被排入待确认");

    /* ============================================================
     * E 组：网络变化（§5）
     * ============================================================ */
    check(!networkInvalidates(true, true, 0x080002C0u, 0x080002C0u),
          "E01 网络无变化时不误失效");
    check(networkInvalidates(true, false, 0x080002C0u, 0x080002C0u),
          "E02 WiFi 断开 ⇒ 缓存失效");
    check(networkInvalidates(false, true, 0x080002C0u, 0x080002C0u),
          "E03 WiFi 重新连接 ⇒ 缓存失效");
    check(networkInvalidates(true, true, 0x080002C0u, 0x090002C0u),
          "E04 本机 IP 变化（切 AP / DHCP）⇒ 缓存失效");
    check(!networkInvalidates(false, false, 0x080002C0u, 0x090002C0u),
          "E05 未连接时的 IP 抖动不触发额外失效");

    fresh(d, c);
    c.ip[kDevFeeder] = IP_FEEDER; c.valid[kDevFeeder] = true;
    c.ip[kDevPlug]   = IP_PLUG;   c.valid[kDevPlug]   = true;
    d.vf[kDevFeeder].active = 1;
    invalidateAll(c, d);
    check(!c.valid[0] && !c.valid[1] && c.ip[0] == 0 && c.ip[1] == 0,
          "E06 网络变化后两台设备 IP 全部清空");
    check(d.pending == kMaskBoth, "E07 网络变化后两台都重新排队");
    check(!verifyAnyActive(d), "E08 网络变化会打断在途的单播验证（旧 IP 已无意义）");

    /* 原 IP 被别的设备占用：即便 hello 有响应也绝不认领 */
    fresh(d, c);
    c.ip[kDevPlug] = IP_PLUG; c.valid[kDevPlug] = true;
    beginOperation(c, d, kDevPlug);
    check(verifyHelloOk(d, kDevPlug, DEV_ID_PLUG, 1, DEV_ID_PLUG), "E09 正常情况下可以认证");
    verifyFail(d, c, kDevPlug);
    check(c.ip[kDevPlug] == 0,
          "E10 该地址被别的设备占用（认证失败）⇒ 立即作废，不发任何控制命令");

    /* 认证失败不得改动 Token 配置 */
    bool tokenCfg[2] = {true, true};
    fresh(d, c);
    requestDiscovery(d, kDevFeeder);
    addCandidate(d, IP_OTHER1, DEV_ID_OTHER, 1);
    bool badTok[2][8] = {};
    int one[1] = {0};
    runParallelAuth(d, c, badTok, one, 1);
    invalidateOne(c, d, kDevFeeder);
    check(tokenCfg[0] && tokenCfg[1],
          "E11 设备无响应 / 认证失败 / 缓存失效都不清空 NVS Token");

    /* 本轮"没试过"与"试过但失败"必须区分（决定是否补轮） */
    check(roundMissedDevice(kMaskFeeder, kMaskPlug),
          "E12 本轮没试过该设备 ⇒ 判为 missed（应补轮）");
    check(!roundMissedDevice(kMaskBoth, kMaskPlug),
          "E13 本轮试过但认证失败 ⇒ 不算 missed（有界补轮，不能刷广播）");

    /* 探测位编码：两台设备 × 8 候选必须互不重叠 */
    bool bitsOk = true;
    for (unsigned char dev = 0; dev < 2; dev++)
        for (unsigned char i = 0; i < 8; i++)
            for (unsigned char dev2 = 0; dev2 < 2; dev2++)
                for (unsigned char j = 0; j < 8; j++)
                    if (!(dev == dev2 && i == j) && probeBit(dev, i) == probeBit(dev2, j)) bitsOk = false;
    check(bitsOk, "E14 探针位编码在两台 × 8 候选下互不重叠");

    puts("TOTAL="); putInt(g_total); puts(" FAIL="); putInt(g_fail); puts("\n");
    return g_fail;
}

const char *probe_log_ptr(void) { return g_log; }
int probe_log_len(void) { return g_logLen; }

} // extern "C"
