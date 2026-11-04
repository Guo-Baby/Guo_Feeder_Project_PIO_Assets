#pragma once

// =====================================================
// xiaomi_lan_discover.h —— 米家 LAN 设备发现 / IP 归属决策（**纯函数 / 零依赖**）
//
// =====================================================
// 背景：小米设备 IP 由 DHCP 分配，会变化。IP **只存在于 RAM**，Token 持久化在 NVS。
//   每次需要操作设备前都必须**重新确认身份**，但确认方式分两条：
//
//   ★ 快路径（2026-11-05 §2）：上次认证过的 RAM IP 还在 ⇒ 向该 IP 单播 hello，
//     用它自己的 device_id/stamp 发一条加密只读查询，解密+关联都通过 ⇒ 身份确认。
//     无需广播、无需收集候选。
//
//   ★ 回退（§3）：单播无响应 / 解密失败 / device_id 变了 ⇒ 全子网广播收集候选，
//     再对各候选**并行**认证（同一轮发出，统一接收窗口）。
//
// 为什么单独抽成纯头文件：
//   「缓存 IP 被别的设备占用」「并行认证的迟到/重复/乱序响应」「两台设备不串台」
//   这些场景很难靠真机稳定复现，必须能在宿主侧真实执行固件同一份决策代码。
//   本文件**不做任何 I/O**：UDP 收发由 xiaomi_lan.cpp 负责，决策在这里。
//
// ★ 实测依据（本机 LAN，纯 hello / get_properties 只读）：
//   · 两台设备都会**原样回显请求 `id`**（实测 id=1234 → 回包 id=1234）
//     ⇒ 可用 id 做「请求-响应」关联，排除迟到/跨轮的旧响应；
//   · 单播 hello+认证：喂食器 p50 19 ms；插座 p50 291 ms（设备端唤醒慢，
//     不是代码问题，见任务书 §8 的瓶颈说明）；
//   · 同网段另有 2 台米家设备响应广播，且每轮响应顺序不同
//     ⇒ 绝不能按响应顺序分配身份。
//
// ★ 语义边界（上层必须遵守）：
//   · 本文件里的 `ip` 是 IPAddress 的 uint32_t 表示，仅用于比较/传递。
//   · 「认证成功」= 校验和通过（Token 匹配）+ 回包 id 与请求关联 + 设备属性有效；
//     仅收到 UDP 包**不算**。
//   · 认证失败**绝不**更新任何设备的有效 IP，也**绝不**改动 Token。
// =====================================================

namespace xland
{

constexpr unsigned char kMaxCandidates = 8;
constexpr unsigned char kDevFeeder = 0;
constexpr unsigned char kDevPlug = 1;
constexpr unsigned char kMaskFeeder = 1u;
constexpr unsigned char kMaskPlug = 2u;
constexpr unsigned char kMaskBoth = 3u;

enum DiscPhase : unsigned char
{
    PhaseIdle = 0,        // 没有发现任务
    PhaseVerifying = 1,   // 单播验证缓存 IP（快路径）
    PhaseCollecting = 2,  // 广播 hello 已发，正在收集候选
    PhaseAuthing = 3,     // 并行 Token 认证窗口（所有候选同时发出）
    PhaseDone = 4         // 本轮结束（可能部分成功）
};

struct Candidate
{
    unsigned int ip;
    unsigned int devId;
    unsigned int stamp;
    unsigned char hits;
};

/**
 * 单播验证子状态（每台设备一份，两台可**同时**验证）。
 *
 * 分两个 stage 而不是一口气发完：
 *   miIO 的密文帧必须带设备自己的 device_id 与 stamp，而这两个值只能从
 *   hello 回包里拿到 ⇒ 必须先 hello、后认证帧。
 */
struct Verify
{
    unsigned char active;    // 1 = 本设备正在验证
    unsigned char stage;     // 0 = 已发 hello 等回包；1 = 已发认证帧等回包
    unsigned char devIdOk;   // hello 回包的 devId 是否与上次认证一致（0 = 无基准，首次）
    unsigned char pad;
    unsigned int ip;         // 待验证的缓存 IP
    unsigned int devId;      // hello 回包给出的 device_id
    unsigned int stamp;      // hello 回包给出的设备时间戳
    unsigned int replyId;    // 本次认证请求的 id（用于关联回包）
    unsigned int deadline;   // 当前 stage 的超时时刻
};

struct Discovery
{
    unsigned char phase;
    unsigned char pending;       // 待发现设备位掩码（kMaskFeeder / kMaskPlug）
    unsigned char candCount;
    unsigned char gen;           // 代号：每开一次验证/认证就 +1，用于 reply id
    unsigned char conflictMask;  // 本轮出现「身份冲突」的设备位
    unsigned char pad;
    unsigned short authResolved; // 已结束的 (dev, cand) 探针位（防重复认领）
    unsigned int authDeadline;   // 并行认证的统一接收窗口截止时刻
    Candidate cand[kMaxCandidates];
    Verify vf[2];
};

/** RAM 中的 IP 缓存。ip=0 表示「本次运行周期内尚未发现」；valid 表示经认证确认过。 */
struct IpCache
{
    unsigned int ip[2];
    bool valid[2];
};

// ------------------------------------------------------------ 基础工具

inline void resetDiscovery(Discovery &d)
{
    d.phase = PhaseIdle;
    d.pending = 0;
    d.candCount = 0;
    /* ★ gen 刻意**不清零**：代号必须跨轮单调，否则 16 轮后会与旧响应撞号。 */
    d.conflictMask = 0;
    d.pad = 0;
    d.authResolved = 0;
    d.authDeadline = 0;
    for (unsigned char i = 0; i < kMaxCandidates; i++)
    {
        d.cand[i].ip = 0;
        d.cand[i].devId = 0;
        d.cand[i].stamp = 0;
        d.cand[i].hits = 0;
    }
    for (unsigned char i = 0; i < 2; i++)
    {
        d.vf[i].active = 0;
        d.vf[i].stage = 0;
        d.vf[i].devIdOk = 0;
        d.vf[i].pad = 0;
        d.vf[i].ip = 0;
        d.vf[i].devId = 0;
        d.vf[i].stamp = 0;
        d.vf[i].replyId = 0;
        d.vf[i].deadline = 0;
    }
}

inline void resetCache(IpCache &c)
{
    c.ip[0] = 0; c.ip[1] = 0;
    c.valid[0] = false; c.valid[1] = false;
}

inline unsigned char devMask(unsigned char dev)
{
    return dev == kDevFeeder ? kMaskFeeder : kMaskPlug;
}

inline bool needsDiscovery(const Discovery &d, unsigned char dev)
{
    return (d.pending & devMask(dev)) != 0;
}

inline void requestDiscovery(Discovery &d, unsigned char dev)
{
    d.pending |= devMask(dev);
    if (d.phase == PhaseIdle || d.phase == PhaseDone) d.phase = PhaseCollecting;
}

/** 缓存值在物理上是否可信（已认证 + 非零 + 网络仍连接）。★ 不代表可以跳过验证。 */
inline bool cacheUsable(const IpCache &c, unsigned char dev, bool wifiConnected)
{
    return wifiConnected && c.valid[dev] && c.ip[dev] != 0;
}

/** 是否有可直接单播验证的缓存地址（不要求 valid：valid 会在操作开始时被降级）。 */
inline bool hasCachedIp(const IpCache &c, unsigned char dev)
{
    return c.ip[dev] != 0;
}

inline unsigned char bumpGen(Discovery &d)
{
    d.gen = (unsigned char)((d.gen + 1) & 0x3F);
    return d.gen;
}

inline bool discoveryInFlight(const Discovery &d)
{
    return d.phase == PhaseVerifying || d.phase == PhaseCollecting || d.phase == PhaseAuthing;
}

// ------------------------------------------------------------ 单播验证（快路径）

/**
 * 单播验证请求的回包 id。
 *
 * ★ 为什么不用固定 id：同一台设备可能同时收到「上一轮的迟到响应」，
 *   固定 id 无法区分。带代号 + 设备位后，跨轮次的旧响应必然对不上号。
 * 值域与普通命令 id(1..5) 及并行探针 id(700+) 都不重叠。
 */
inline unsigned int verifyReplyId(unsigned char gen, unsigned char dev)
{
    return 600u + (unsigned int)(gen & 0x1Fu) * 2u + (unsigned int)(dev & 1u);
}

/**
 * ★★ [2026-11-05 · 任务书 §2.1/§2.2] 一次独立操作开始前的**强制**身份确认。
 *
 * **缓存不是跳过验证的理由**：本函数只是把「怎么验证」分成两条路，
 * 绝不返回「可以不用验证」。
 *
 * @return true  = 有缓存 IP ⇒ 走单播快路径（已把 vf[dev] 置为 active/stage0）
 *         false = 无缓存 / 已有轮次在飞 ⇒ 由调用方开广播轮（或并入在途轮次）
 *
 * ★ 只作用于 `dev` 这一台；另一台的缓存与验证状态原样保留。
 * ★ 不触碰 Token（NVS 的事，本文件不做 I/O）。
 */
/**
 * 只**登记**单播验证子状态，不改 `phase`。
 *
 * 用途：已经有一轮单播验证在飞时，另一台设备也可以立刻开始**自己那条**单播验证
 * （两台是相互独立的 unicast 流：不同 IP、不同 Token、reply id 带设备位）。
 * 若不允许这样并入，那台设备只能退化成广播（实测多花 ~540 ms）。
 *
 * @return true = 已登记（调用方负责发 hello 并设 deadline）
 */
inline bool beginVerifyOnly(Discovery &d, IpCache &c, unsigned char dev)
{
    if (d.vf[dev].active) return false;          // 已在验证
    if (!hasCachedIp(c, dev)) return false;      // 没有候选地址 ⇒ 只能广播

    Verify &v = d.vf[dev];
    v.active = 1;
    v.stage = 0;
    v.ip = c.ip[dev];
    v.devId = 0;
    v.stamp = 0;
    v.replyId = 0;
    v.deadline = 0;                              // 由 I/O 层按 hello 超时填
    v.devIdOk = 0;
    d.pending |= devMask(dev);
    return true;
}

inline bool beginOperation(IpCache &c, Discovery &d, unsigned char dev)
{
    d.pending |= devMask(dev);
    /*
     * ★ 无论走哪条路，**新操作一开始就把"已确认"降级**：
     *   · 走快路径 ⇒ 待验证（vf active）；
     *   · 并入在途轮次 ⇒ 也必须降级，否则别处仍可能把旧 IP 当"已验证"。
     */
    c.valid[dev] = false;

    if (discoveryInFlight(d)) return false;      // 并入在途轮次（由调用方决定是否 beginVerifyOnly）

    if (!beginVerifyOnly(d, c, dev)) return false;   // 没有候选地址 ⇒ 只能广播
    d.phase = PhaseVerifying;
    return true;
}

/**
 * hello 回包到达：记录设备给出的 device_id/stamp，准备发认证帧。
 *
 * @param expectedDevId 上次认证过的 device_id（0 = 无基准）。非 0 且不相等 ⇒
 *        该 IP 已被**另一台**米家设备占用（§2.1 的核心场景），直接判失败，
 *        省掉一次必然失败的认证往返。
 * @return true = 可以发认证帧（调用方填 deadline 并发出）；false = 立即判失败
 */
inline bool verifyHelloOk(Discovery &d, unsigned char dev,
                          unsigned int helloDevId, unsigned int stamp,
                          unsigned int expectedDevId)
{
    Verify &v = d.vf[dev];
    if (!v.active || v.stage != 0) return false;

    v.devIdOk = (expectedDevId == 0 || expectedDevId == helloDevId) ? 1 : 0;
    if (!v.devIdOk) return false;

    v.devId = helloDevId;
    v.stamp = stamp;
    v.replyId = verifyReplyId(bumpGen(d), dev);
    v.stage = 1;
    return true;
}

/** 验证成功 ⇒ 该 IP 被本设备 Token 确认（**保留**缓存值，只把 valid 提回来）。 */
inline void verifyOk(Discovery &d, IpCache &c, unsigned char dev)
{
    d.vf[dev].active = 0;
    d.vf[dev].stage = 0;
    c.ip[dev] = d.vf[dev].ip;
    c.valid[dev] = true;
    d.pending &= (unsigned char)~devMask(dev);
}

/** 验证失败 ⇒ 作废该设备的缓存值，留在 pending 里等广播回退。 */
inline void verifyFail(Discovery &d, IpCache &c, unsigned char dev)
{
    d.vf[dev].active = 0;
    d.vf[dev].stage = 0;
    c.ip[dev] = 0;
    c.valid[dev] = false;
    d.pending |= devMask(dev);
}

inline bool verifyAnyActive(const Discovery &d)
{
    return d.vf[kDevFeeder].active || d.vf[kDevPlug].active;
}

/** 所有验证子流程都结束了吗（成功或失败都算结束）。 */
inline bool verifyAllDone(const Discovery &d)
{
    return !verifyAnyActive(d);
}

// ------------------------------------------------------------ 广播候选收集

/**
 * 记录一条 hello 响应。
 * @return true = 新增候选；false = 重复 / 非法 / 容量已满。
 * ★ 只认 32 字节 hello 响应；其它长度的上层不应传进来。
 */
inline bool addCandidate(Discovery &d, unsigned int ip,
                         unsigned int devId, unsigned int stamp)
{
    if (ip == 0 || ip == 0xFFFFFFFFu) return false;   // 0 / 受限广播不算设备
    for (unsigned char i = 0; i < d.candCount; i++)
    {
        if (d.cand[i].ip == ip)
        {
            /* 同一 IP 的重复响应：只更新会话参数与计数，不新增条目 */
            d.cand[i].devId = devId;
            d.cand[i].stamp = stamp;
            if (d.cand[i].hits < 255) d.cand[i].hits++;
            return false;
        }
    }
    if (d.candCount >= kMaxCandidates) return false;
    Candidate &c = d.cand[d.candCount++];
    c.ip = ip;
    c.devId = devId;
    c.stamp = stamp;
    c.hits = 1;
    return true;
}

// ------------------------------------------------------------ 并行认证（§3）

/**
 * 并行认证请求的回包 id。
 *
 * 三段编码（低到高）：候选序号 4 bit | 设备位 1 bit | 轮次代号 4 bit。
 * ⇒ 同一轮内每个 (dev, cand) 唯一；跨轮次的迟到响应必然对不上号。
 * 值域 700..1211，与普通命令 id(1..5)、单播验证 id(600..) 都不重叠。
 */
inline unsigned int probeReplyId(unsigned char gen, unsigned char dev, unsigned char cand)
{
    return 700u + (unsigned int)(gen & 0x0Fu) * 32u
                + (unsigned int)(dev & 1u) * 16u
                + (unsigned int)(cand & 0x0Fu);
}

/** 探针位：低 8 位 = 喂食器 × 候选，高 8 位 = 插座 × 候选（kMaxCandidates ≤ 8）。 */
inline unsigned short probeBit(unsigned char dev, unsigned char cand)
{
    return (unsigned short)(1u << ((unsigned int)(dev & 1u) * 8u + (unsigned int)(cand & 0x07u)));
}

inline bool probeResolved(const Discovery &d, unsigned char dev, unsigned char cand)
{
    return (d.authResolved & probeBit(dev, cand)) != 0;
}

inline void resolveProbe(Discovery &d, unsigned char dev, unsigned char cand)
{
    d.authResolved |= probeBit(dev, cand);
}

/** 该设备在本轮是否还有未结束的探针（false ⇒ 该设备本轮已无望，可提前收尾）。 */
inline bool probeRemaining(const Discovery &d, unsigned char dev, unsigned char candCount)
{
    if ((d.pending & devMask(dev)) == 0) return false;
    for (unsigned char i = 0; i < candCount; i++)
        if (!probeResolved(d, dev, i)) return true;
    return false;
}

/** 开启并行认证轮：代号 +1、清空已完成位与冲突位。 */
inline void beginAuthRound(Discovery &d)
{
    bumpGen(d);
    d.authResolved = 0;
    d.conflictMask = 0;
    d.phase = PhaseAuthing;
}

/**
 * 匹配一条认证回包。
 *
 * ★ 匹配是**三段联合**的，缺一不可：
 *     ① 来源 IP 必须是某个候选（不是候选的包与本轮无关）；
 *     ② 该候选上本设备的探针还没结束（防重复响应重复认领）；
 *     ③ 密文能用**本设备的 Token** 解开且回包 id 等于本次请求 id
 *        （由上层判定，本函数只给出「应该期望哪个 id」）。
 *
 * @param outCand 匹配到的候选下标
 * @param outExpectId 该 (dev, cand) 对应的期望回包 id
 * @return true = 找到匹配槽位
 */
inline bool matchProbe(const Discovery &d, unsigned char dev,
                       unsigned int fromIp, unsigned char *outCand,
                       unsigned int *outExpectId)
{
    if ((d.pending & devMask(dev)) == 0) return false;
    for (unsigned char i = 0; i < d.candCount; i++)
    {
        if (d.cand[i].ip != fromIp) continue;
        if (probeResolved(d, dev, i)) return false;    // 该组合已结束
        *outCand = i;
        *outExpectId = probeReplyId(d.gen, dev, i);
        return true;
    }
    return false;
}

enum ClaimResult : unsigned char
{
    ClaimNone = 0,
    ClaimOk = 1,         // 成功认领
    ClaimDup = 2,        // 重复响应（同一 IP 同一设备），忽略
    ClaimConflict = 3    // 身份冲突：拒绝认领并作废该设备的缓存
};

/**
 * ★★ 并行认证的归属判定（本文件最核心的一条）。
 *
 * 只有 `authOk == true` 且 id 关联通过才认领；另外两条**冲突规则**：
 *   · 同一 IP 已被另一台设备认领 ⇒ 拒绝（两台设备 IP 不互换）；
 *   · 本设备已有另一个 IP 认证通过 ⇒ **冲突**：不随意取第一个，
 *     而是清空该设备缓存、记 conflictMask、本轮放弃该设备（下轮重新来）。
 */
inline ClaimResult claimParallel(Discovery &d, IpCache &c,
                                 unsigned char dev, unsigned char cand)
{
    const unsigned int ip = d.cand[cand].ip;
    const unsigned char other = (dev == kDevFeeder) ? kDevPlug : kDevFeeder;

    if (c.valid[dev] && c.ip[dev] == ip) return ClaimDup;

    if (c.valid[other] && c.ip[other] == ip)
    {
        c.ip[dev] = 0;
        c.valid[dev] = false;
        d.conflictMask |= devMask(dev);
        d.pending &= (unsigned char)~devMask(dev);
        return ClaimConflict;
    }

    if (c.valid[dev] && c.ip[dev] != ip)
    {
        /* 两台不同 IP 都用同一台设备的 Token 通过认证 ⇒ 身份不唯一 */
        c.ip[dev] = 0;
        c.valid[dev] = false;
        d.conflictMask |= devMask(dev);
        d.pending &= (unsigned char)~devMask(dev);
        return ClaimConflict;
    }

    c.ip[dev] = ip;
    c.valid[dev] = true;
    d.pending &= (unsigned char)~devMask(dev);
    return ClaimOk;
}

/** 本轮发现是否完成（所有待发现设备都已认领，或无候选可试）。 */
inline bool discoveryFinished(const Discovery &d)
{
    return d.pending == 0 || d.candCount == 0;
}

/**
 * 本轮是否**根本没试过**某个仍在等待的设备。
 * 补轮由 I/O 层在 `advanceSlot()` 的 stage0 用**每请求有界计数**触发
 * （最多 2 次），避免设备真离线时变成一轮接一轮的广播风暴。
 */
inline bool roundMissedDevice(unsigned char probedMask, unsigned char needMask)
{
    return (needMask & (unsigned char)~probedMask) != 0;
}

// ------------------------------------------------------------ 网络变化

/**
 * ★ 网络变化必须让 IP 缓存整体失效。
 * 判据：连接态变化，或本机 IP 变化（DHCP 换址 / 切 AP）。
 * 不改变任何 Token 配置（那是 NVS 的事）。
 */
inline bool networkInvalidates(bool prevConnected, bool nowConnected,
                               unsigned int prevLocalIp, unsigned int nowLocalIp)
{
    return (prevConnected != nowConnected)
        || (nowConnected && prevLocalIp != nowLocalIp);
}

inline void invalidateAll(IpCache &c, Discovery &d, unsigned char mask = kMaskBoth)
{
    for (unsigned char dev = 0; dev < 2; dev++)
    {
        if ((mask & devMask(dev)) == 0) continue;
        c.ip[dev] = 0;
        c.valid[dev] = false;
        /*
         * ★ 网络已变 ⇒ 在途的单播验证必须一起作废：
         *   它正在验证的旧 IP 在新网络里毫无意义，继续等只会白等到超时。
         */
        d.vf[dev].active = 0;
        d.vf[dev].stage = 0;
    }
    requestDiscovery(d, kDevFeeder);
    requestDiscovery(d, kDevPlug);
    /* 只重新发现 mask 指定的设备 */
    d.pending = (d.pending & ~kMaskBoth) | mask;
}

/** 单台设备通信失败（超时/认证失败）⇒ 该设备 IP 失效，但**不动 Token / 不动另一台**。 */
inline void invalidateOne(IpCache &c, Discovery &d, unsigned char dev)
{
    c.ip[dev] = 0;
    c.valid[dev] = false;
    d.pending |= devMask(dev);
    if (d.phase == PhaseIdle || d.phase == PhaseDone) d.phase = PhaseCollecting;
}

} // namespace xland
