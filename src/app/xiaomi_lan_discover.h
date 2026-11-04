#pragma once

// =====================================================
// xiaomi_lan_discover.h —— 米家 LAN 设备发现 / IP 归属决策（**纯函数 / 零依赖**）
//
// =====================================================
// 背景（2026-11-04 任务书）：
//   小米设备 IP 由 DHCP 分配，会变化。IP **只存在于 RAM**，Token 持久化在 NVS。
//   每次需要操作设备前，先经 miIO UDP 54321 广播发现候选，再用各自 Token
//   做只读认证查询，认证通过才把该候选 IP 认领给对应设备。
//
// 为什么单独抽成纯头文件：
//   「两台设备同时被发现」「IP 变化后重新发现」「WiFi 重连失效」「两台 IP 不互换」
//   这些场景很难靠真机稳定复现，必须能在宿主侧真实执行固件同一份代码。
//   本文件**不做任何 I/O**：UDP 收发由 xiaomi_lan.cpp 负责，决策在这里。
//
// ★ 实测依据（2026-11-04，本机 LAN，纯 hello / get_properties 只读）：
//   · 广播 hello 3/3 轮同时发现两台目标设备；同网段共 4 台 miIO 设备响应；
//   · 响应顺序每轮不同 ⇒ **绝不能按响应顺序分配身份**；
//   · 只有本设备 Token 能解密通过并返回特征属性：
//       feeder → siid2/piid26 (out_status)，plug → siid2/piid1 (power)；
//     另外 2 台设备对两个 Token 全部拒绝 ⇒ 认证即身份，不需要整段扫描。
//
// ★ 语义边界（上层必须遵守）：
//   · 本文件里的 `ip` 是 IPAddress 的 uint32_t 表示，仅用于比较/传递；
//     上层用 `IPAddress(uint32_t)` 还原，不假设字节序语义。
//   · `authOk` 必须由「校验和通过 + 解密成功 + result[0].code==0 + 有 value」
//     共同构成；仅收到 UDP 包**不算**认证成功。
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
    PhaseCollecting = 1,  // 广播 hello 已发，正在收集候选响应
    PhaseProbing = 2,     // 逐 (设备 × 候选) 做 Token 认证查询
    PhaseDone = 3         // 本轮结束（可能部分成功）
};

struct Candidate
{
    unsigned int ip;
    unsigned int devId;
    unsigned int stamp;
    unsigned char hits;
};

struct Discovery
{
    unsigned char phase;
    unsigned char pending;      // 待发现设备位掩码（kMaskFeeder / kMaskPlug）
    unsigned char curDev;       // 当前正在认证的设备（kDevFeeder / kDevPlug）
    unsigned char candCount;
    unsigned char candIdx;      // 当前候选下标
    unsigned char probeStage;   // 0 = 等 hello 回包，1 = 等认证属性回包
    unsigned char padding;
    Candidate cand[kMaxCandidates];
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
    d.curDev = kDevFeeder;
    d.candCount = 0;
    d.candIdx = 0;
    d.probeStage = 0;
    d.padding = 0;
    for (unsigned char i = 0; i < kMaxCandidates; i++)
    {
        d.cand[i].ip = 0;
        d.cand[i].devId = 0;
        d.cand[i].stamp = 0;
        d.cand[i].hits = 0;
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

/** 缓存是否可直接使用（必须同时满足：已认证 + 非零 + 网络仍连接）。 */
inline bool cacheUsable(const IpCache &c, unsigned char dev, bool wifiConnected)
{
    return wifiConnected && c.valid[dev] && c.ip[dev] != 0;
}

/**
 * ★★ [2026-11-05 · 任务书 §2.1] **每次独立操作前的强制刷新**。
 *
 * 语义（与 `cacheUsable` 的分工必须分清）：
 *   · `cacheUsable` 只回答"这份缓存值在物理上是否还可信"（网络没变、认领过）。
 *   · `beginOperation` 回答"这次**操作**要不要重新发现" ⇒ **永远是"要"**。
 *
 * 因此本函数**无条件**作废该设备的 RAM IP（即使缓存看起来有效），
 * 并把该设备重新排入待发现集合；`cacheUsable()` 随后必然返回 false。
 *
 * 为什么必须这样：DHCP 换址后旧 IP 依旧"看起来有效"（valid=true、非零、
 * WiFi 也连着），任何"缓存有效就跳过发现"的捷径都会把请求打到别的设备或
 * 打空 —— 这正是 2026-11-04 两台设备 `last_err=2` 的成因。
 *
 * 约束：
 *   · 只影响 `dev` 这一台，另一台的缓存原样保留（两台各自独立）；
 *   · 不触碰 Token（那是 NVS 的事，本文件不做 I/O）；
 *   · `phase` 若正处在 Collecting/Probing，只把设备并入本轮（保留在途进度）。
 */
inline void beginOperation(IpCache &c, Discovery &d, unsigned char dev)
{
    c.ip[dev] = 0;
    c.valid[dev] = false;
    d.pending |= devMask(dev);
    if (d.phase == PhaseIdle || d.phase == PhaseDone) d.phase = PhaseCollecting;
}

/**
 * 本轮是否**根本没试过**某个仍在等待的设备。
 *
 * 场景：设备 A 的请求在轮次已经开始认证设备 B 之后才到达 ⇒ A 被
 * `nextProbe` 的游标跳过（`probedMask` 里没有它）。
 * 补轮由 I/O 层在 `advanceSlot()` 的 stage0 用**每请求有界计数**触发
 * （最多 2 次），避免设备真离线时变成一轮接一轮的广播风暴。
 */
inline bool roundMissedDevice(unsigned char probedMask, unsigned char needMask)
{
    return (needMask & (unsigned char)~probedMask) != 0;
}

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

// ------------------------------------------------------------ 认证轮转

/**
 * 选下一个「待发现设备 × 候选 IP」组合。
 *
 * ★ 轮转顺序：先设备、后候选；设备按 pending 位固定为 feeder→plug，
 *   **与广播响应顺序完全无关**（实测顺序每轮都变）。
 * @return false = 本轮所有组合都试完（phase 应转 Done）
 */
inline bool nextProbe(Discovery &d, unsigned char *devOut, unsigned int *ipOut)
{
    if (d.pending == 0 || d.candCount == 0) return false;

    while (d.curDev < 2)
    {
        if ((d.pending & devMask(d.curDev)) == 0) { d.curDev++; d.candIdx = 0; continue; }
        if (d.candIdx < d.candCount)
        {
            *devOut = d.curDev;
            *ipOut = d.cand[d.candIdx].ip;
            d.probeStage = 0;
            return true;
        }
        d.curDev++;
        d.candIdx = 0;
    }
    return false;
}

/** 当前候选试完（认证失败 / 超时）⇒ 移到下一个候选。 */
inline void skipCandidate(Discovery &d)
{
    if (d.candIdx < d.candCount) d.candIdx++;
    d.probeStage = 0;
}

/**
 * ★★ 认证结果归属（本文件最核心的一条）。
 *
 * 只有 `authOk == true` 才把当前候选 IP 认领给 `d.curDev`：
 *   · 绝不按响应顺序分配；
 *   · 绝不因为「另一台设备刚好也认证成功」而串台；
 *   · 若该 IP 已被另一台设备认领 ⇒ 拒绝（两台设备 IP 不互换）；
 *   · 失败时**不写任何缓存**，也不清 Token。
 *
 * @return true = 成功认领并清除该设备的 pending 位
 */
inline bool claimOnAuth(Discovery &d, IpCache &c, bool authOk)
{
    if (!authOk) return false;
    const unsigned char dev = d.curDev;
    const unsigned char other = (dev == kDevFeeder) ? kDevPlug : kDevFeeder;
    const unsigned int ip = d.cand[d.candIdx].ip;

    if (c.valid[other] && c.ip[other] == ip) return false;  // 防串用

    c.ip[dev] = ip;
    c.valid[dev] = true;
    d.pending &= (unsigned char)~devMask(dev);
    return true;
}

/** 本轮发现是否完成（所有待发现设备都已认领，或无候选可试）。 */
inline bool discoveryFinished(const Discovery &d)
{
    return d.pending == 0 || d.candCount == 0;
}

} // namespace xland
