#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <Preferences.h>
#include <time.h>

#include <mbedtls/md5.h>
#include <mbedtls/aes.h>

#include <ArduinoJson.h>

#include "app/xiaomi_lan.h"
#include "app/xiaomi_lan_parse.h"   // 纯函数解析（宿主侧可测，与固件同一份代码）
#include "app/xiaomi_lan_slot.h"    // 纯函数互斥决策（P4 · 跨设备命令独立性，宿主侧可测）
#include "app/xiaomi_lan_discover.h" // 纯函数发现 / RAM IP 归属决策（宿主侧 39 项用例）
#include "services/config_manager.h"
#include "services/system_state.h"
#include "automation/workflow.h"

// =====================================================
// 米家 LAN 设备模块（实现说明见 xiaomi_lan.h）
//
// ★ 协议常量全部来自 MiLAN_Test/src/miot_lan.py（本机实测通过）：
//     magic 0x2131 | length = ct_len + 32 | 0 | device_id | stamp
//     checksum = md5(header16 + token16 + ct)
//     key = md5(token16)     iv = md5(key16 + token16)
//     AES-128-CBC + PKCS7，明文尾部补 0x00
//   ⇒ 不做任何"凭印象改参数"，改这里必须同步改 Python 侧并重新对拍。
// =====================================================

namespace
{
constexpr uint16_t kPort = 54321;
constexpr uint16_t kMagic = 0x2131;
constexpr uint16_t kHeaderLen = 16;
constexpr uint16_t kMaxPayload = 1024;
constexpr uint16_t kMaxFrame = kMaxPayload + 64;

constexpr uint32_t kHelloTimeoutMs = 800;
constexpr uint32_t kReplyTimeoutMs = 3000;
constexpr uint32_t kProbeIntervalMs = 60000;

/*
 * ★ [T12 · 2026-11-02] 探活为"未领取的动作结果"让步的最大轮数。
 *   正常路径 workflow_task 的 actionPoll 在下一轮主循环就会经
 *   `xiaomi_lan_release_result()` 领走结果 ⇒ 计数几乎不可能到 1；
 *   上限只是兜底（例如 workflow 已终止、结果永远没人领的场景）。
 */
constexpr uint8_t XLAN_PROBE_DEFER_MAX = 5;

/*
 * ★ [P0-3 · 2026-10-31] 只读请求首败后的**单次**重试间隔。
 *
 * 取 3 s 的理由（不是随手写的）：
 *   · 米家局域网单次超时窗口是数秒级（见 kReplyTimeoutMs），3 s 足够让上一包
 *     的残留/对端处理完；
 *   · 又远小于 60 s 周期，用户"再点一次"时状态已经更新；
 *   · 明显大于"连打"（几十毫秒），不会加剧丢包。
 */
constexpr uint32_t kProbeRetryMs = 3000;

/*
 * ★ [P0-2 · 2026-10-31] 状态"新鲜度"上限：定义在 `xiaomi_lan.h` 的
 *   `XIAOMI_STATE_FRESH_MS`（唯一定义源，workflow pre-flight 直接引用）。
 */
/*
 * ★★ [2026-11-05 · 二期] 身份确认有**两条**路，不是一条：
 *
 *   ① 快路径（缓存 IP 单播验证，§2）
 *        上次认证过的 RAM IP 还在 ⇒ 向它单播 hello（拿 device_id/stamp）
 *        ⇒ 用它自己的会话参数发**一条加密只读查询** ⇒ 解密 + id 回显都对上
 *        ⇒ 身份确认，直接执行本次命令。**不需要广播、不需要候选收集。**
 *
 *   ② 回退（广播 + 并行认证，§3）
 *        单播无响应 / 解密失败 / hello 的 device_id 与上次不符（IP 被别的
 *        米家设备占用了）⇒ 作废缓存，全子网广播收集候选，再**同时**向
 *        所有 (设备 × 候选) 发认证帧，用一个接收窗口统一收集。
 *        旧实现是逐个候选串行等 700 ms 超时 ⇒ 4 个候选最坏 2.8 s；
 *        并行后与候选数量**无关**。
 *
 * 实测（本机 LAN，纯 hello / get_properties 只读，各 6 轮）：
 *   · 喂食器 hello p50 8 ms / 认证 p50 11 ms ⇒ 快路径合计约 19 ms；
 *   · 插座   hello p50 88 ms / 认证 p50 203 ms ⇒ 约 291 ms（设备端唤醒慢，
 *     属协议现实，不能靠缩短安全步骤去"凑"指标）；
 *   · 两台设备都**原样回显请求 id** ⇒ 用它做请求/响应关联，排除迟到与跨轮响应。
 */
constexpr uint32_t kVerifyHelloMs = 600;    // 单播 hello 回包等待（插座 p95 121 ms）
constexpr uint32_t kVerifyAuthMs  = 900;    // 单播认证回包等待（插座 p95 222 ms）
constexpr uint32_t kDiscCollectMs = 1200;   // 广播响应收集窗口（上限）
constexpr uint32_t kDiscQuietMs = 400;      // 收集期"无新候选"静默阈值（提前收尾）
constexpr uint32_t kAuthWindowMs  = 900;    // 并行认证统一接收窗口（覆盖最慢设备）
constexpr uint32_t kDiscRetryMs = 15000;    // 整轮失败后的静默期（防风暴）
constexpr uint32_t kDiscRequestBudgetMs = 20000; // 单条命令等待发现完成的上界
constexpr uint8_t  kDiscBroadcasts = 2;

/*
 * ★★ [2026-11-05 二期 · 真机验收发现] 控制命令受理后，**延时**再补一次只读确认。
 *
 * 为什么不能立刻读：插座收到 set_properties 后回包很快，但**真正切换**要再过
 *   一秒左右。实测（UI 通电 → 8 s 后查询仍为 false，而 LAN 独立查询已是 true）：
 *   立刻读会把"还没切换"的旧值写回 System State ⇒ 页面一直显示「已断电」。
 * 取 2.5 s：覆盖插座的最坏切换时延，又远小于 60 s 安全定时器。
 */
constexpr uint32_t kConfirmReadDelayMs = 2500;

// ★ [2026-10-29] 普通通电的执行端安全时限（秒）
constexpr uint32_t kPlugAutoOffSec = 60;

// NVS（**凭证只在这里**，绝不写入 config JSON / 日志）
constexpr const char *kNvsNs = "gflan";

// 配置模块名：只有 enable 开关放 config（与其他模块一致）
constexpr const char *kCfgModule = "xiaomi_lan";

enum DevKind : uint8_t { DEV_FEEDER = 0, DEV_PLUG = 1 };

enum ReqKind : uint8_t
{
    REQ_NONE = 0,
    REQ_READ_FEEDER,
    REQ_READ_PLUG,
    REQ_FEED,
    REQ_PLUG_ON,
    REQ_PLUG_OFF
};

// ---------------- 设备描述 ----------------
/*
 * ★★ [2026-11-04] `DevInfo` 里**不再有 IP 字段**。
 *   IP 的唯一存储是下面的 `g_ipCache`（纯 RAM），由发现流程写入、网络变化时清空。
 *   NVS `gflan` 只保留 `f_tok` / `p_tok`；旧版本遗留的 `f_ip` / `p_ip`
 *   既不读、也不删（不破坏既有 NVS 数据与回滚）。
 */
struct DevInfo
{
    uint8_t token[16];
    uint32_t devId;      // 握手得到的设备号（0 = 未知）
    uint32_t stamp;      // 握手得到的时间戳
    bool configured;     // 只表示「NVS 里有合法 Token」，与 IP 无关
};

DevInfo g_feeder;
DevInfo g_plug;

/* ★ RAM 中的 IP 缓存（唯一权威）+ 发现状态机，重启后自然清空 */
xland::Discovery g_disc;
xland::IpCache g_ipCache;
uint32_t g_collectDeadline = 0;     // 广播收集截止（秒级窗口）
uint32_t g_discRetryAt = 0;
uint32_t g_lastLocalIp = 0;
bool g_lastWifiConnected = false;

/*
 * ★ [2026-11-05 二期] 上次**认证通过**时的 device_id（每台一份，纯 RAM）。
 *
 * 用途只有一个：快路径单播 hello 回来后，若 device_id 与上次不同 ⇒
 * 这个 IP 上已经不是原来那台设备（典型的 DHCP 把地址分给了别的米家设备，
 * §2.1 的核心场景）。此时**立即**判失败转广播，不必再发一条必然解不开的
 * 认证帧（插座那一条要多花 200 ms）。
 *
 * ⚠ 它不是身份依据：device_id 只在明文 hello 头里，谁都能伪造。
 *   真正的身份依据永远是「Token 解密 + 校验和 + id 关联」。
 */
uint32_t g_lastDevId[2] = {0, 0};

/*
 * ★ [2026-11-05 二期] 本设备的**本次操作**是否已完成身份确认。
 *   `startRequest()` 一开始就清 false；只有发现层确认成功才置 true。
 *   ⇒ 缓存值存在本身**绝不能**让 stage0 认为"可以发命令了"。
 */
bool g_devReady[2] = {false, false};

/*
 * ★★ [2026-11-05] 本**轮**发现覆盖的设备位掩码。
 *   轮次开始后另一台设备的操作可能中途加入 ⇒ 轮末必须能区分
 *   「本轮没覆盖到这台」和「覆盖到了但认证失败」。
 */
uint8_t g_roundMask = 0;
/* 本轮**真正发过认证查询**的设备位（用于区分「没试过」与「试过但失败」） */
uint8_t g_roundProbedMask = 0;
/* 本轮实际发出的并行认证探针条数（诊断用；未被认领的探针不会置 resolved 位） */
uint8_t g_roundSentProbes = 0;

/* 发现耗时统计（只读、纯数字，不含凭据） */
uint32_t g_discRoundStartMs = 0;
uint32_t g_discLastCandMs = 0;
uint32_t g_discLastCollectMs = 0;
uint32_t g_discLastAuthMs = 0;
uint32_t g_discAuthStartMs = 0;
uint32_t g_discLastTotalMs = 0;
uint16_t g_discRounds = 0;
uint16_t g_discRoundFail = 0;

/*
 * ★ [2026-11-05 二期] 控制类命令受理后的**延时确认读**排程（0 = 无）。
 *   独立于 periodicProbe 的 60 s 计时，避免"被打上计时又被延迟"导致永不执行。
 */
uint32_t g_confirmAt[2] = {0, 0};

/* 单播快路径统计（每个设备一份，供报告与串口取证） */
uint32_t g_verifyT0[2] = {0, 0};
uint32_t g_verifyAuthT[2] = {0, 0};
uint32_t g_verifyHelloMs[2] = {0, 0};
uint32_t g_verifyAuthMs[2] = {0, 0};
uint8_t  g_verifyResult[2] = {0, 0};   // 0=未做 1=快路径成功 2=已回退广播
uint16_t g_verifyCount = 0;
uint16_t g_verifyOkCnt = 0;
uint16_t g_verifyFallbackCnt = 0;

bool g_inited = false;
bool g_udpReady = false;
WiFiUDP g_udp;

/* 该设备的当前有效 IP；无有效缓存时返回 0.0.0.0（绝不回落到旧 NVS 值） */
IPAddress devIp(bool feeder)
{
    const unsigned char idx = feeder ? xland::kDevFeeder : xland::kDevPlug;
    if (!xland::cacheUsable(g_ipCache, idx, WiFi.status() == WL_CONNECTED)) return IPAddress(0, 0, 0, 0);
    return IPAddress(g_ipCache.ip[idx]);
}

/* 网络状态变化 ⇒ 两台设备的 RAM IP 全部作废，重新排队发现（Token 不动） */
void invalidateIpCache()
{
    xland::invalidateAll(g_ipCache, g_disc);
    g_feeder.devId = 0; g_feeder.stamp = 0;
    g_plug.devId = 0;   g_plug.stamp = 0;

    /*
     * ★ [2026-11-05 二期] 网络变了，三样东西一起作废：
     *   · 在途的单播验证（旧 IP 在新网络里毫无意义）；
     *   · 上次认证过的 device_id 基准（换了网络可能换了一整套设备）；
     *   · "本次操作已验证"标志。
     * 唯一不动的是 NVS 里的 Token。
     */
    g_disc.candCount = 0;   /* vf 已由 invalidateAll 清空 */
    g_disc.phase = xland::PhaseIdle;
    g_lastDevId[0] = 0;
    g_lastDevId[1] = 0;
    g_devReady[0] = false;
    g_devReady[1] = false;
}

/*
 * ★★ [2026-11-05] 只丢弃缓存值，**不碰发现状态机**。
 *
 * 为什么不能用 `xland::invalidateOne()`：
 *   它会把 `phase` 置成 Collecting，但**不发广播、不设 deadline** ⇒
 *   下一轮 `advanceDiscovery()` 会拿着上一轮的过期 deadline 立刻"超时"，
 *   用上一轮的旧候选做认证 —— 既浪费一轮又可能误判。
 * 现在发现完全由「操作」驱动（`beginOperationDiscovery`），
 * 失败路径只负责把值丢掉，下一次操作自然会重新广播。
 */
void dropCachedIp(bool feeder)
{
    const unsigned char idx = feeder ? xland::kDevFeeder : xland::kDevPlug;
    g_ipCache.ip[idx] = 0;
    g_ipCache.valid[idx] = false;
    g_devReady[idx] = false;        /* ★ 缓存没了，"本次已验证"自然也失效 */
    DevInfo &d = feeder ? g_feeder : g_plug;
    d.devId = 0;
    d.stamp = 0;
}

// ---------------- 单请求状态机 ----------------
struct Request
{
    bool active;
    uint8_t stage;            // 1=等握手  2=等命令回包
    uint32_t deadline;
    uint8_t kind;
    uint8_t portions;
    DevInfo *dev;
    const WorkflowActionDescriptor *owner;   // nullptr = 内部探活
    bool fin;
    bool ok;
    /*
     * ★ [2026-10-29] 本次请求内是否出现过"校验和失败"，以及起始计数快照。
     *   不能用全局累计值判断（一次失败会让之后所有超时都被误判成 Token 错）。
     */
    uint32_t badsum0;
    bool gotPacket;
    /*
     * ★ [T12 · 2026-11-02] 探活为本槽"未领取的动作结果"让步的连续轮数。
     *   见 startRequest() 的 T12 注释；达到 XLAN_PROBE_DEFER_MAX 后允许覆盖
     *   （结果视为陈旧）。正常路径下 workflow_task 下一轮就会领走 ⇒ 计数到不了 1。
     */
    uint8_t defer_count;
    /*
     * ★ [2026-11-05] stage0 阶段"补发现"次数（**每请求有界，最多 2 次**）。
     *   为什么需要：本轮发现可能因「游标已走过本设备」而根本没试过它，
     *   若等 `kDiscRetryMs`(15 s) 静默期才补，请求早已超时。
     *   为什么要上限：设备真离线时不能变成一轮接一轮的广播风暴。
     */
    uint8_t discKick;
};

/*
 * ★★ [P4 · 2026-11-01 · 任务书 P0-2] 请求槽改为**每设备一个**。
 *
 * ── 缺陷根因 ──
 *   旧实现只有**一个全局槽** `g_req`。任何一台设备的请求在飞时，
 *   另一台设备的请求会被 `startRequest()` 判为 BUSY 并**直接拒绝**：
 *     · 喂食器在出粮（stage2 等回包，最长 kReplyTimeoutMs=3 s）
 *       ⇒ 插座断电被拒；
 *     · 插座在查询 ⇒ 喂食器出粮被拒。
 *   ⇒ 用户报的「米家设备之间错误的命令互斥」，**固件侧是根因之一**。
 *
 * ── 修法（不是"删掉互斥"）──
 *   ① 槽按设备索引：`g_req[DEV_FEEDER]` / `g_req[DEV_PLUG]`；
 *   ② **同设备**仍然严格串行（一次只有一个 LAN 握手在飞）——
 *      这是协议要求：米家设备对密集握手会丢包（2026-10-30 真机实测），
 *      且同一台设备的 UDP 端口/会话号不能并发；
 *   ③ **跨设备**互不阻塞：喂食器忙时插座照常可发；
 *   ④ 两台设备的回包靠**来源 IP** 分流到各自的槽
 *      （`g_udp.remoteIP()`），不会串台。
 *
 * ⚠ 关键安全约束（任务书 P0-4）：
 *   插座断电（REQ_PLUG_OFF）在**插座自己的槽**被占用时仍可能被拒。
 *   为此断电路径增加了**抢占**语义：见 `startRequest()` 里的插口说明 ——
 *   断电可以**中止**插座槽里正在进行的只读查询（只读无副作用，中止安全），
 *   但**不会**中止另一个物理动作（避免半途打断出粮/通电）。
 */
Request g_req[2];

/* ★ [P4] 取得某台设备的请求槽（DEV_FEEDER / DEV_PLUG）。 */
static inline Request &reqOf(bool feeder) { return g_req[feeder ? DEV_FEEDER : DEV_PLUG]; }

/* ★ [P4] 某台设备当前是否正在占用通道。 */
static inline bool devBusy(bool feeder) { return g_req[feeder ? DEV_FEEDER : DEV_PLUG].active; }

uint32_t g_nextProbeMs = 0;
uint8_t g_probeTurn = 0;

// ---------------- 只读诊断计数（不含凭据） ----------------
uint32_t g_statSend = 0;
uint32_t g_statRecv = 0;
uint32_t g_statBadChecksum = 0;
uint32_t g_statTimeout = 0;

/*
 * ★ [2026-10-30 真机缺陷修复] 「一次没回包」不等于「设备离线」。
 *
 * 实测现象（生产页面，控制页点一次「读取两台设备状态」）：
 *   先给喂食器发一次只读查询，某一拍米家设备没回（它自己也在忙）⇒ 超时，
 *   旧代码立刻 `setOnline(false)` ⇒ 页面画成「不可达」，
 *   而同一时刻 MQTT `query_state` 读到 `last_err=1`、周期探活随后也成功。
 *   用户看到的就是"明明在线却显示不可达"，与真实状态相反。
 *
 * 现在：连续 `kOfflineAfterFails` 次失败才判离线。单次失败**只更新 last_err**
 *   （UI 会显示"在线（最近一次查询失败）"+ 原因），不再一票否决。
 *   成功一次即清零。
 *
 * ★ [2026-10-30 22:1x] 阈值由 2 提到 **3**：
 *   实测在生产页面上，周期探活（60 s 一次）与用户主动探活偶尔撞车 ⇒
 *   米家设备只回其中一个包 ⇒ 另一个超时。两连败就判离线会**频繁误报**
 *   （页面显示「不可达」，而设备实际在线）。三连败仍能覆盖真离线，
 *   且把误报窗口显著拉长。
 */
uint8_t g_failStreak[2] = {0, 0};
constexpr uint8_t kOfflineAfterFails = 3;

/*
 * ★ [P0-3 · 2026-10-31] "最近一次通信成功"的**单调时刻**（millis）。
 *
 * 为什么不复用 `g_lastOkEpoch` / `*_last_ok_ts`：
 *   · 那两个是**墙钟**，设备未授时（SNTP 没同步）时就只能是 0
 *     ⇒ 无法回答"这个结论有多旧"，而"结论过期"正是 pre-flight 必须判断的事。
 *   · `millis()` 单调、不受对时跳变影响，正好用来算"多久以前"。
 *   0 表示"自本次启动以来从未成功"（重启后本就该当作未确认）。
 */
unsigned long g_lastOkMs[2] = {0, 0};

/*
 * ★ [P0-3 · 2026-10-31] "尽快探活"标志（由 pre-flight 置位）。
 *   只影响**调度时机**，不改变任何判定规则。
 */
bool g_probeNow[2] = {false, false};

/*
 * ★ [2026-10-30 22:3x] 判离线再加一道**时间窗**。
 *
 * 实测（生产页面，自动化连点 + 写 Token 后的连续探活）：
 *   22:28:48 通信成功 → 22:29:13 页面读到 online=false
 *   但同一时刻用 MQTT 直查设备：boot_seq 未变（没重启）、online=true。
 *   ⇒ 这 25 秒里设备连着失败了 3 次（连发 hello：写 Token 后的延迟探活 +
 *     UI 的验证探活 + 页面刷新探活），米家设备对密集握手丢包。
 *   ⇒ 丢包是**瞬时**的，"离线"却是**持久**的；用连败次数直接判离线
 *     会把瞬时丢包说成设备掉线（用户看到「不可达」，设备其实好好的）。
 *
 * 现在判离线要**同时**满足：
 *   ① 连续失败 ≥ kOfflineAfterFails 次，且
 *   ② 距上次成功已 ≥ kOfflineWindowSec 秒（默认 120 s）
 * ⇒ 瞬时丢包（几秒内自愈）永远只表现为"最近一次查询失败"，不会被判成离线；
 *   真离线会在 2 分钟内被识别出来。
 */
constexpr long kOfflineWindowSec = 120;
long g_lastOkEpoch[2] = {0, 0};

/* ★ [2026-10-29] 前置声明：这两个函数定义在状态机之后，但状态机里要用 */
void disarmAutoOff();
void forceProbe(bool feeder);

/* ★ [2026-10-29] 前置声明：这两个函数定义在状态机之后，但状态机里要用 */
void disarmAutoOff();
void forceProbe(bool feeder);

// 诊断输出剩余次数（0 = 静默）。默认编译关闭，排查时 -DGF_XIAOMI_LAN_DIAG=1 打开。
#ifndef GF_XIAOMI_LAN_DIAG
#define GF_XIAOMI_LAN_DIAG 0
#endif
#if GF_XIAOMI_LAN_DIAG
static uint8_t g_diagLeft = 8;
#endif

// =====================================================
// 小工具
// =====================================================
void md5Of(const uint8_t *data, size_t len, uint8_t out[16])
{
    mbedtls_md5(data, len, out);
}

int hexVal(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool parseToken(const char *hex, uint8_t out[16])
{
    if (hex == nullptr) return false;
    size_t n = strlen(hex);
    if (n != 32) return false;
    for (size_t i = 0; i < 16; i++)
    {
        int hi = hexVal(hex[i * 2]);
        int lo = hexVal(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return true;
}

void zeroBytes(void *p, size_t n)
{
    volatile uint8_t *q = (volatile uint8_t *)p;
    while (n--) *q++ = 0;
}

// =====================================================
// 加解密（AES-128-CBC + PKCS7）
// =====================================================
// 返回密文长度；失败返回 0
size_t encryptPayload(const uint8_t token[16], const uint8_t *plain, size_t plainLen,
                      uint8_t *out, size_t outCap)
{
    uint8_t key[16];
    uint8_t iv[16];
    uint8_t keyIvIn[32];

    md5Of(token, 16, key);
    memcpy(keyIvIn, key, 16);
    memcpy(keyIvIn + 16, token, 16);
    md5Of(keyIvIn, 32, iv);

    // PKCS7(128) 补位
    size_t padded = ((plainLen / 16) + 1) * 16;
    if (padded > outCap) return 0;

    uint8_t buf[kMaxFrame];
    if (padded > sizeof(buf)) return 0;
    memcpy(buf, plain, plainLen);
    uint8_t pad = (uint8_t)(padded - plainLen);
    for (size_t i = plainLen; i < padded; i++) buf[i] = pad;

    mbedtls_aes_context ctx;
    mbedtls_aes_init(&ctx);
    if (mbedtls_aes_setkey_enc(&ctx, key, 128) != 0)
    {
        mbedtls_aes_free(&ctx);
        zeroBytes(key, sizeof(key));
        return 0;
    }
    uint8_t ivWork[16];
    memcpy(ivWork, iv, 16);
    int rc = mbedtls_aes_crypt_cbc(&ctx, MBEDTLS_AES_ENCRYPT, padded, ivWork, buf, out);
    mbedtls_aes_free(&ctx);
    zeroBytes(key, sizeof(key));

    return (rc == 0) ? padded : 0;
}

// 返回明文长度（已去 PKCS7 与尾部 0x00）；失败返回 0
size_t decryptPayload(const uint8_t token[16], const uint8_t *ct, size_t ctLen, uint8_t *out, size_t outCap)
{
    if (ctLen == 0 || (ctLen % 16) != 0 || ctLen > outCap) return 0;

    uint8_t key[16];
    uint8_t iv[16];
    uint8_t keyIvIn[32];

    md5Of(token, 16, key);
    memcpy(keyIvIn, key, 16);
    memcpy(keyIvIn + 16, token, 16);
    md5Of(keyIvIn, 32, iv);

    mbedtls_aes_context ctx;
    mbedtls_aes_init(&ctx);
    if (mbedtls_aes_setkey_dec(&ctx, key, 128) != 0)
    {
        mbedtls_aes_free(&ctx);
        zeroBytes(key, sizeof(key));
        return 0;
    }
    uint8_t ivWork[16];
    memcpy(ivWork, iv, 16);
    int rc = mbedtls_aes_crypt_cbc(&ctx, MBEDTLS_AES_DECRYPT, ctLen, ivWork, ct, out);
    mbedtls_aes_free(&ctx);
    zeroBytes(key, sizeof(key));
    if (rc != 0) return 0;

    uint8_t pad = out[ctLen - 1];
    if (pad == 0 || pad > 16 || pad > ctLen) return 0;
    size_t len = ctLen - pad;

    // 协议在 JSON 之后补 0x00
    while (len > 0 && out[len - 1] == 0x00) len--;
    return len;
}

// =====================================================
// 组帧 / 发帧
// =====================================================
bool sendHelloTo(IPAddress ip)
{
    uint8_t hello[32];
    hello[0] = 0x21; hello[1] = 0x31; hello[2] = 0x00; hello[3] = 0x20;
    for (int i = 4; i < 32; i++) hello[i] = 0xFF;   // Python: "21310020" + "ff"*28

    if (!g_udp.beginPacket(ip, kPort)) return false;
    g_udp.write(hello, sizeof(hello));
    g_statSend++;
    return g_udp.endPacket() == 1;
}

bool sendEncryptedTo(DevInfo &dev, IPAddress ip, const char *json)
{
    uint8_t ct[kMaxFrame];
    size_t plainLen = strlen(json) + 1;             // 尾部 0x00
    uint8_t plain[kMaxPayload];
    if (plainLen + 32 > sizeof(plain)) return false;
    memcpy(plain, json, strlen(json));
    plain[strlen(json)] = 0x00;

    size_t ctLen = encryptPayload(dev.token, plain, plainLen, ct, sizeof(ct));
    if (ctLen == 0) return false;

    uint8_t frame[kMaxFrame];
    uint16_t total = (uint16_t)(kHeaderLen + 16 + ctLen);

    frame[0] = (uint8_t)(kMagic >> 8);
    frame[1] = (uint8_t)(kMagic & 0xFF);
    uint16_t lenField = (uint16_t)(ctLen + 32);
    frame[2] = (uint8_t)(lenField >> 8);
    frame[3] = (uint8_t)(lenField & 0xFF);
    frame[4] = frame[5] = frame[6] = frame[7] = 0x00;
    frame[8]  = (uint8_t)(dev.devId >> 24);
    frame[9]  = (uint8_t)(dev.devId >> 16);
    frame[10] = (uint8_t)(dev.devId >> 8);
    frame[11] = (uint8_t)(dev.devId);
    frame[12] = (uint8_t)(dev.stamp >> 24);
    frame[13] = (uint8_t)(dev.stamp >> 16);
    frame[14] = (uint8_t)(dev.stamp >> 8);
    frame[15] = (uint8_t)(dev.stamp);

    // checksum = md5(header + token + ciphertext)
    uint8_t md5in[kHeaderLen + 16 + kMaxPayload];
    memcpy(md5in, frame, kHeaderLen);
    memcpy(md5in + kHeaderLen, dev.token, 16);
    memcpy(md5in + kHeaderLen + 16, ct, ctLen);
    uint8_t sum[16];
    md5Of(md5in, kHeaderLen + 16 + ctLen, sum);
    memcpy(frame + kHeaderLen, sum, 16);
    memcpy(frame + kHeaderLen + 16, ct, ctLen);

    if (!g_udp.beginPacket(ip, kPort)) return false;
    g_udp.write(frame, total);
    g_statSend++;
    return g_udp.endPacket() == 1;
}

// =====================================================
// 收到一包（握手 or 命令回包）
// =====================================================
struct Reply
{
    bool valid;
    uint32_t devId;
    uint32_t stamp;
    /*
     * ★ [2026-11-05 二期] 请求 id 的回显。身份认证用它做「这条回包是不是
     *   回答我这次请求」的关联：迟到响应 / 上一轮的残留 / 别人的探测
     *   都会因为 id 对不上而被丢弃。
     */
    bool hasId;
    int  reqId;
    int code;          // 顶层 code：**成功回包不带此字段** ⇒ 缺省视为 0
    int propCode;      // result[0].code：单属性级错误码（成功同样可能缺省 ⇒ 0）
    bool hasResult;
    bool hasValue;     // 是否真的取到 value（不是"有 result 数组"）
    int resultInt;
    bool resultBool;
};

bool parseReply(DevInfo &dev, uint8_t *buf, int len, Reply &out)
{
    out.valid = false;
    out.hasId = false;
    out.reqId = 0;
    if (len < 32) return false;

    uint16_t magic = (uint16_t)((buf[0] << 8) | buf[1]);
    if (magic != kMagic) return false;

    out.devId = ((uint32_t)buf[8] << 24) | ((uint32_t)buf[9] << 16) |
                ((uint32_t)buf[10] << 8) | (uint32_t)buf[11];
    out.stamp = ((uint32_t)buf[12] << 24) | ((uint32_t)buf[13] << 16) |
                ((uint32_t)buf[14] << 8) | (uint32_t)buf[15];

    // 32 字节 = 握手响应（无密文）
    if (len == 32)
    {
        out.valid = true;
        out.code = 0;
        out.hasResult = false;
        return true;
    }

    uint8_t *ct = buf + 32;
    size_t ctLen = (size_t)(len - 32);

    uint8_t md5in[kHeaderLen + 16 + kMaxPayload];
    if (16 + ctLen > kMaxPayload) return false;
    memcpy(md5in, buf, kHeaderLen);
    memcpy(md5in + kHeaderLen, dev.token, 16);
    memcpy(md5in + kHeaderLen + 16, ct, ctLen);
    uint8_t sum[16];
    md5Of(md5in, kHeaderLen + 16 + ctLen, sum);

    if (memcmp(sum, buf + kHeaderLen, 16) != 0)
    {
        // 校验和不符 = token 不对（或包的来源不是这台设备）
        g_statBadChecksum++;
        return false;
    }

    uint8_t plain[kMaxPayload];
    size_t plainLen = decryptPayload(dev.token, ct, ctLen, plain, sizeof(plain));
    if (plainLen == 0) return false;
    plain[plainLen] = 0x00;

    /*
     * ★ 解析交给 `xlan::parseReply()`（纯函数、零依赖，宿主侧有 33 项用例覆盖）。
     *   协议事实：成功回包**没有顶层 code**（缺省=0），出错才带；
     *   单属性错误码在 result[0].code；value 缺失/null/类型不符 ⇒ hasValue=false。
     */
    xlan::Reply xr;
    if (!xlan::parseReply((const char *)plain, (int)plainLen, xr) || !xr.ok) return false;

    out.valid = true;
    out.hasId = xr.hasId;
    out.reqId = xr.id;
    out.code = xr.code;
    out.propCode = xr.propCode;
    out.hasResult = xr.hasResult;
    out.hasValue = xr.value.hasValue;
    out.resultInt = xr.value.intVal;
    out.resultBool = xr.value.boolVal;
    return true;
}

// =====================================================
// 命令负载
// =====================================================
const char *buildCommand(uint8_t kind, uint8_t portions, char *buf, size_t cap)
{
    switch (kind)
    {
    case REQ_READ_FEEDER:
        // 2/26 = out_status（当前出粮状态）
        snprintf(buf, cap, "{\"id\":1,\"method\":\"get_properties\",\"params\":[{\"did\":\"p\",\"siid\":2,\"piid\":26}]}");
        return buf;
    case REQ_READ_PLUG:
        // 2/1 = power（开关状态）
        snprintf(buf, cap, "{\"id\":2,\"method\":\"get_properties\",\"params\":[{\"did\":\"p\",\"siid\":2,\"piid\":1}]}");
        return buf;
    case REQ_FEED:
        // pet-food-out: siid 2 / aiid 1，输入 piid 8 = 份数
        snprintf(buf, cap,
                 "{\"id\":3,\"method\":\"action\",\"params\":{\"did\":\"a\",\"siid\":2,\"aiid\":1,\"in\":[{\"piid\":8,\"value\":%u}]}}",
                 (unsigned)portions);
        return buf;
    case REQ_PLUG_ON:
        snprintf(buf, cap, "{\"id\":4,\"method\":\"set_properties\",\"params\":[{\"did\":\"p\",\"siid\":2,\"piid\":1,\"value\":true}]}");
        return buf;
    case REQ_PLUG_OFF:
        snprintf(buf, cap, "{\"id\":5,\"method\":\"set_properties\",\"params\":[{\"did\":\"p\",\"siid\":2,\"piid\":1,\"value\":false}]}");
        return buf;
    default:
        buf[0] = 0;
        return buf;
    }
}

/*
 * ★ [2026-11-05 二期] **身份认证专用**只读查询：与 `REQ_READ_*` 同属性，
 *   但请求 id 由调用方指定（`verifyReplyId` / `probeReplyId`）。
 *
 * 为什么必须换 id：
 *   · 固定 id（1/2）无法判断一条回包是"回答我这次请求"还是"上一轮的迟到包"；
 *   · 带代号 + 设备位 + 候选位的 id 让每个在飞请求都有唯一关联键，
 *     回包 id 对不上就一律丢弃 ⇒ 迟到/重复/跨轮响应不会被误认领；
 *   · 值域与普通命令 id(1..5) 不重叠，避免被当成命令结果。
 */
const char *buildProbeCommand(uint8_t kind, unsigned int replyId, char *buf, size_t cap)
{
    const int piid = (kind == REQ_READ_FEEDER) ? 26 : 1;   // 2/26 out_status · 2/1 power
    snprintf(buf, cap,
             "{\"id\":%u,\"method\":\"get_properties\",\"params\":[{\"did\":\"p\",\"siid\":2,\"piid\":%d}]}",
             replyId, piid);
    return buf;
}

// =====================================================
// ★★ [2026-11-04] miIO 动态发现 —— IP 只存在于 RAM
//
// 分层：xland::*（纯函数，宿主侧 39 项用例）负责「归属决策」；
//       本节负责 UDP I/O，把决策翻译成实际收发。
//
// ★ 五件事必须分开，不能互相冒充：
//     发现到设备（收到 hello）≠ 认证成功（Token 校验和通过）
//     ≠ 身份确认（属性 code=0 且有 value）≠ 状态查询成功 ≠ 控制执行成功
//   本节只做到「身份确认」并写入 RAM IP；后续查询/控制仍由 Request 槽负责。
// =====================================================

/** 当前子网定向广播地址 = localIP | ~subnetMask（不用 255.255.255.255，减少跨网段噪声） */
IPAddress subnetBroadcast()
{
    IPAddress ip = WiFi.localIP();
    IPAddress mask = WiFi.subnetMask();
    return IPAddress((uint32_t)ip | ~(uint32_t)mask);
}

/* =====================================================
 * 身份确认 ①：单播快路径（§2）
 *   缓存 IP → hello（取 device_id/stamp）→ 加密只读认证 → 身份确认
 * ===================================================== */

static bool applyVerifiedReply(unsigned char dev, const Reply &r);

/**
 * 为**单台**设备启动单播验证（发一条单播 hello 并起自己的倒计时）。
 *
 * ★ 之所以按设备拆开：两台是相互独立的 unicast 流，第二台可以在第一台
 *   验证在飞时**立刻加入**，不必等它结束、更不必退化成广播。
 */
void startVerifyDev(unsigned char dev)
{
    if (WiFi.status() != WL_CONNECTED || !g_udpReady)
    {
        g_disc.vf[dev].active = 0;
        g_disc.vf[dev].stage = 0;
        return;
    }
    xland::Verify &v = g_disc.vf[dev];
    const uint32_t now = millis();
    g_verifyT0[dev] = now;
    g_verifyHelloMs[dev] = 0;
    g_verifyAuthMs[dev] = 0;
    g_verifyResult[dev] = 0;
    v.deadline = now + kVerifyHelloMs;
    if (!sendHelloTo(IPAddress(v.ip)))
    {
        Serial.printf("[XiaomiLAN] verify dev=%s hello send failed -> broadcast\n",
                      dev == xland::kDevFeeder ? "feeder" : "plug");
        xland::verifyFail(g_disc, g_ipCache, dev);
        g_verifyResult[dev] = 2;
        g_verifyFallbackCnt++;
    }
}

/**
 * 启动单播验证：对每台 active 的设备各发一条**单播** hello。
 *
 * ★ 两台可以同时验证：目标 IP 不同、Token 不同、回包 id 带设备位 ⇒ 不会串台。
 */
void startVerify()
{
    if (WiFi.status() != WL_CONNECTED || !g_udpReady)
    {
        /*
         * ★★ 没有网络时**绝不伪造"正在验证"**。
         *   2026-11-05 真机踩到：上电瞬间伪造在途状态 ⇒ 下一轮拿过期 deadline
         *   立刻判超时 ⇒ 空转把 stage0 的补发现额度烧光，WiFi 连上后反而不发现。
         */
        for (unsigned char dev = 0; dev < 2; dev++)
        {
            g_disc.vf[dev].active = 0;
            g_disc.vf[dev].stage = 0;
        }
        g_disc.phase = xland::PhaseIdle;
        return;
    }

    bool any = false;
    for (unsigned char dev = 0; dev < 2; dev++)
    {
        if (!g_disc.vf[dev].active) continue;
        any = true;
        startVerifyDev(dev);
    }
    g_disc.phase = any ? xland::PhaseVerifying : xland::PhaseDone;
}

/** 开启一轮广播发现（非阻塞）。只纳入调用方已排入 pending 且已配 Token 的设备。 */
void startDiscoveryRound()
{
    if (WiFi.status() != WL_CONNECTED || !g_udpReady)
    {
        g_disc.phase = xland::PhaseIdle;
        return;
    }

    /* 保留"要发现哪几台"的意图（resetDiscovery 会清 pending / 候选 / vf） */
    unsigned char want = g_disc.pending;
    if (!g_feeder.configured) want &= (unsigned char)~xland::kMaskFeeder;
    if (!g_plug.configured)   want &= (unsigned char)~xland::kMaskPlug;

    xland::resetDiscovery(g_disc);
    g_disc.pending = (unsigned char)(want & xland::kMaskBoth);

    if (g_disc.pending == 0)
    {
        g_disc.phase = xland::PhaseDone;
        g_roundMask = 0;
        return;
    }

    g_roundMask = g_disc.pending;
    g_roundProbedMask = 0;
    g_roundSentProbes = 0;
    g_disc.phase = xland::PhaseCollecting;
    g_discRoundStartMs = millis();
    g_discRounds++;
    const IPAddress bc = subnetBroadcast();
    for (uint8_t i = 0; i < kDiscBroadcasts; i++)
    {
        if (!sendHelloTo(bc)) break;
    }
    g_discLastCandMs = millis();
    g_collectDeadline = millis() + kDiscCollectMs;
}

/**
 * ★★ [2026-11-05] 一次独立操作开始前的**强制**身份确认入口。
 *
 * 「缓存不是跳过验证的理由」在这里落地成：
 *   · 有缓存 IP ⇒ 单播快路径（必须 hello + 加密认证都过才算数）；
 *   · 无缓存 / 验证失败 ⇒ 广播 + 并行认证。
 * 两条路都**必然**做一次 Token 身份认证，没有"免验"分支。
 */
void beginOperationDiscovery(unsigned char devIdx)
{
    const unsigned char mask = xland::devMask(devIdx);
    DevInfo &d = (devIdx == xland::kDevFeeder) ? g_feeder : g_plug;
    d.devId = 0;
    d.stamp = 0;
    g_devReady[devIdx] = false;      // ★ 本次操作尚未验证

    /*
     * ★★ 在途轮次：只并入，不打断。
     *   判断必须在 `beginOperation()` **之前** —— 后者在 Idle/Done 时会
     *   自己把 phase 置成 PhaseVerifying；判晚一步就永远走不到"开轮"分支
     *   （2026-11-05 真机踩过：串口恒为 `discovery round=0 total=0ms`）。
     */
    if (xland::discoveryInFlight(g_disc))
    {
        /* 并入在途轮次；顺带把该设备的"已确认"降级（beginOperation 内部处理） */
        xland::beginOperation(g_ipCache, g_disc, devIdx);
        g_disc.pending |= mask;
        g_roundMask |= mask;

        /*
         * ★★ [2026-11-05 二期 · 真机日志发现] 在途的如果是**单播验证**，
         *   本设备也可以立刻并行开始自己那条验证（两台互不干扰）。
         *   旧行为是干等对方验证结束，然后因为 pending 非空而**退化成整轮广播**
         *   —— 实测多花 ~540 ms，而这正是"一次点读取两台状态"最常见的交错。
         */
        if (g_disc.phase == xland::PhaseVerifying &&
            xland::beginVerifyOnly(g_disc, g_ipCache, devIdx))
        {
            g_verifyCount++;
            startVerifyDev(devIdx);
        }
        return;
    }

    const bool fast = xland::beginOperation(g_ipCache, g_disc, devIdx);
    g_discRetryAt = 0;               // 操作优先：不受上一轮失败后的静默期影响
    g_disc.pending |= mask;
    g_roundMask = g_disc.pending;
    g_discRoundStartMs = millis();

    if (fast)
    {
        g_verifyCount++;
        startVerify();
    }
    else
    {
        startDiscoveryRound();
    }
}

/**
 * 本轮发现结束的**唯一**出口。
 * 只打印诊断（纯数字，不含任何凭据），不做补轮 —— 补轮由 stage0 有界触发。
 */
void endDiscoveryRound()
{
    g_disc.phase = xland::PhaseDone;
    g_discRetryAt = millis() + kDiscRetryMs;

    const uint32_t now = millis();
    g_discLastTotalMs = (g_discRoundStartMs != 0) ? (now - g_discRoundStartMs) : 0;
    if (g_discLastTotalMs > 0 && g_ipCache.ip[0] == 0 && g_ipCache.ip[1] == 0)
        g_discRoundFail++;

    uint8_t resolved = 0;
    for (unsigned char i = 0; i < 16; i++)
        if (g_disc.authResolved & (1u << i)) resolved++;

    Serial.printf("[XiaomiLAN] discovery round=%u gen=%u mask=%u cand=%u probes=%u/%u conflict=%u "
                  "total=%ums collect=%ums auth=%ums ok=%d/%d\n",
                  (unsigned)g_discRounds, (unsigned)g_disc.gen, (unsigned)g_roundMask,
                  (unsigned)g_disc.candCount, (unsigned)g_roundSentProbes,
                  (unsigned)resolved, (unsigned)g_disc.conflictMask,
                  (unsigned)g_discLastTotalMs, (unsigned)g_discLastCollectMs,
                  (unsigned)g_discLastAuthMs,
                  g_ipCache.valid[0] ? 1 : 0, g_ipCache.valid[1] ? 1 : 0);
}

/* =====================================================
 * 身份确认 ②：广播 + **并行**认证（§3）
 *
 * 为什么必须并行：旧实现逐候选串行、每错一个等 700 ms 超时，
 *   4 个候选最坏 2.8 s（插座更慢，实测单次认证就 200 ms）。
 *   现在同一轮把所有 (设备 × 候选) 全发出去，用一个接收窗口收，
 *   总耗时与候选数量**无关**。
 *
 * ★ 每个探针都用**该候选自己的** device_id/stamp 组帧，绝不复制同一个报文；
 *   回包按「来源 IP + 解密成功 + id 回显」三段联合匹配，
 *   迟到 / 重复 / 跨轮响应都会被 id 挡掉。
 * ===================================================== */

/** 向某个候选发一条本设备的认证查询。 */
bool sendProbe(unsigned char dev, unsigned char cand)
{
    DevInfo &target = (dev == xland::kDevFeeder) ? g_feeder : g_plug;
    if (!target.configured) return false;

    /*
     * probe 只是借一个 Token 的载体：devId/stamp 必须用**该候选自己**的，
     * 否则多个候选会共用同一会话号（任务书明令禁止）。
     */
    DevInfo probe;
    memcpy(probe.token, target.token, 16);
    probe.devId = g_disc.cand[cand].devId;
    probe.stamp = g_disc.cand[cand].stamp + 1;   // 与 MiLAN_Test 一致：hello ts + 1
    probe.configured = true;

    char payload[kMaxPayload];
    buildProbeCommand(dev == xland::kDevFeeder ? REQ_READ_FEEDER : REQ_READ_PLUG,
                      xland::probeReplyId(g_disc.gen, dev, cand), payload, sizeof(payload));
    return sendEncryptedTo(probe, IPAddress(g_disc.cand[cand].ip), payload);
}

/** 把本轮所有 (设备 × 候选) 认证帧一次性发出。@return 实际发出条数 */
uint8_t sendAllAuthProbes()
{
    uint8_t sent = 0;
    for (unsigned char dev = 0; dev < 2; dev++)
    {
        if ((g_disc.pending & xland::devMask(dev)) == 0) continue;
        g_roundProbedMask |= xland::devMask(dev);   // 记录"本轮确实试过这台"
        for (unsigned char i = 0; i < g_disc.candCount; i++)
        {
            if (sendProbe(dev, i)) sent++;
            else xland::resolveProbe(g_disc, dev, i);   // 发不出去就别再等它
        }
    }
    return sent;
}

/**
 * 处理一条并口认证回包。
 *
 * ★ 为什么不能"按来源 IP 直接归属"：同一个 IP 上可能同时有两个探针在飞
 *   （喂食器与插座都对它试过），只有 Token 能解开的那条才是真的。
 *   所以顺序是：先定位候选 → 再逐设备尝试解密 → 解开的那个再看 id 关联。
 */
void onAuthPacket(uint8_t *buf, int len, IPAddress from)
{
    for (unsigned char dev = 0; dev < 2; dev++)
    {
        unsigned char cand = 0;
        unsigned int expectId = 0;
        if (!xland::matchProbe(g_disc, dev, (uint32_t)from, &cand, &expectId)) continue;

        DevInfo &target = (dev == xland::kDevFeeder) ? g_feeder : g_plug;
        if (!target.configured) continue;

        DevInfo probe;
        memcpy(probe.token, target.token, 16);
        probe.devId = 0;
        probe.stamp = 0;
        probe.configured = true;

        Reply r;
        /* 解密/校验和不过 ⇒ 不是这台设备的回包（Token 不匹配），交给另一台试 */
        if (!parseReply(probe, buf, len, r) || !r.valid) continue;
        /* 解开但 id 不是本次请求 ⇒ 迟到 / 跨轮 / 别人的响应 ⇒ 丢弃 */
        if (!r.hasId || r.reqId != (int)expectId) continue;

        const bool attrOk = (r.code == 0) && (r.propCode == 0) && r.hasValue;
        if (!attrOk)
        {
            /* 能解密说明 Token 对，但属性读不到 ⇒ 该组合不成立，别再等它 */
            xland::resolveProbe(g_disc, dev, cand);
            return;
        }

        const xland::ClaimResult cr = xland::claimParallel(g_disc, g_ipCache, dev, cand);
        if (cr == xland::ClaimOk)
        {
            DevInfo &tgt = (dev == xland::kDevFeeder) ? g_feeder : g_plug;
            tgt.devId = r.devId;
            tgt.stamp = r.stamp;
            g_lastDevId[dev] = r.devId;
            g_devReady[dev] = true;
            Serial.printf("[XiaomiLAN] auth claim dev=%s cand=%u (parallel)\n",
                          dev == xland::kDevFeeder ? "feeder" : "plug", (unsigned)cand);
            applyVerifiedReply(dev, r);
        }
        else if (cr == xland::ClaimConflict)
        {
            Serial.printf("[XiaomiLAN] auth CONFLICT dev=%s cand=%u -> cache dropped\n",
                          dev == xland::kDevFeeder ? "feeder" : "plug", (unsigned)cand);
        }
        return;   // 一个包只归一台设备
    }
    /* 谁都没认领：陌生设备 / 迟到包 ⇒ 丢弃（已计入 g_statRecv） */
}

/* ------------------------------------------------------------ 分阶段推进 */

void advanceVerify(uint8_t *buf, int len, IPAddress from, uint32_t now)
{
    bool changed = false;

    for (unsigned char dev = 0; dev < 2; dev++)
    {
        xland::Verify &v = g_disc.vf[dev];
        if (!v.active) continue;

        const bool mine = (len > 0 && (uint32_t)from == v.ip);
        DevInfo &target = (dev == xland::kDevFeeder) ? g_feeder : g_plug;

        /* ---------- stage 0：等单播 hello 回包 ---------- */
        if (v.stage == 0)
        {
            if (mine && len == 32 && ((uint16_t)((buf[0] << 8) | buf[1]) == kMagic))
            {
                const uint32_t did = ((uint32_t)buf[8] << 24) | ((uint32_t)buf[9] << 16) |
                                     ((uint32_t)buf[10] << 8) | (uint32_t)buf[11];
                const uint32_t ts  = ((uint32_t)buf[12] << 24) | ((uint32_t)buf[13] << 16) |
                                     ((uint32_t)buf[14] << 8) | (uint32_t)buf[15];
                g_verifyHelloMs[dev] = now - g_verifyT0[dev];

                /*
                 * ★ device_id 只作**早退**判据，不作身份依据：
                 *   与上次认证过的值不同 ⇒ 这个 IP 上已经换成别的米家设备
                 *   （DHCP 换址，§2.1 的核心场景）⇒ 直接回退广播，
                 *   省掉一条必然解不开的认证帧（插座要 200 ms）。
                 */
                if (!xland::verifyHelloOk(g_disc, dev, did, ts, g_lastDevId[dev]))
                {
                    Serial.printf("[XiaomiLAN] verify dev=%s ip owner changed -> broadcast\n",
                                  dev == xland::kDevFeeder ? "feeder" : "plug");
                    xland::verifyFail(g_disc, g_ipCache, dev);
                    g_verifyResult[dev] = 2;
                    g_verifyFallbackCnt++;
                    changed = true;
                    continue;
                }

                DevInfo probe;
                memcpy(probe.token, target.token, 16);
                probe.devId = did;
                probe.stamp = ts + 1;
                probe.configured = true;
                char payload[kMaxPayload];
                buildProbeCommand(dev == xland::kDevFeeder ? REQ_READ_FEEDER : REQ_READ_PLUG,
                                  v.replyId, payload, sizeof(payload));
                if (!sendEncryptedTo(probe, IPAddress(v.ip), payload))
                {
                    xland::verifyFail(g_disc, g_ipCache, dev);
                    g_verifyResult[dev] = 2;
                    g_verifyFallbackCnt++;
                }
                else
                {
                    v.deadline = now + kVerifyAuthMs;
                    g_verifyAuthT[dev] = now;
                }
                changed = true;
                continue;
            }
        }
        /* ---------- stage 1：等加密认证回包 ---------- */
        else if (mine && len > 32)
        {
            DevInfo probe;
            memcpy(probe.token, target.token, 16);
            probe.devId = 0;
            probe.stamp = 0;
            probe.configured = true;

            Reply r;
            const bool decoded = parseReply(probe, buf, len, r) && r.valid;
            const bool linked = decoded && r.hasId && r.reqId == (int)v.replyId;
            const bool ok = linked && (r.code == 0) && (r.propCode == 0) && r.hasValue;

            if (ok)
            {
                g_verifyAuthMs[dev] = now - g_verifyAuthT[dev];
                xland::verifyOk(g_disc, g_ipCache, dev);
                target.devId = r.devId;
                target.stamp = r.stamp;
                g_lastDevId[dev] = r.devId;
                g_devReady[dev] = true;
                g_verifyResult[dev] = 1;
                g_verifyOkCnt++;
                Serial.printf("[XiaomiLAN] verify dev=%s ok hello=%ums auth=%ums total=%ums\n",
                              dev == xland::kDevFeeder ? "feeder" : "plug",
                              (unsigned)g_verifyHelloMs[dev], (unsigned)g_verifyAuthMs[dev],
                              (unsigned)(g_verifyHelloMs[dev] + g_verifyAuthMs[dev]));
                applyVerifiedReply(dev, r);
            }
            else
            {
                /* 有回包但对不上：可能被别的设备占用 / Token 已变 / 响应过期 */
                Serial.printf("[XiaomiLAN] verify dev=%s auth rejected (decoded=%d linked=%d) -> broadcast\n",
                              dev == xland::kDevFeeder ? "feeder" : "plug",
                              decoded ? 1 : 0, linked ? 1 : 0);
                xland::verifyFail(g_disc, g_ipCache, dev);
                g_verifyResult[dev] = 2;
                g_verifyFallbackCnt++;
            }
            changed = true;
            continue;
        }

        /* ---------- 超时 ---------- */
        if ((int32_t)(now - v.deadline) >= 0)
        {
            if (v.stage == 0) g_verifyHelloMs[dev] = now - g_verifyT0[dev];
            else              g_verifyAuthMs[dev] = now - g_verifyAuthT[dev];
            Serial.printf("[XiaomiLAN] verify dev=%s stage=%u timeout -> broadcast\n",
                          dev == xland::kDevFeeder ? "feeder" : "plug", (unsigned)v.stage);
            xland::verifyFail(g_disc, g_ipCache, dev);
            g_verifyResult[dev] = 2;
            g_verifyFallbackCnt++;
            changed = true;
        }
    }

    if (changed && xland::verifyAllDone(g_disc))
    {
        if (g_disc.pending != 0)
        {
            /* 验证失败的那几台 ⇒ 立刻回退广播（不受静默期限制） */
            Serial.println("[XiaomiLAN] verify -> broadcast fallback");
            g_discRetryAt = 0;
            startDiscoveryRound();
        }
        else
        {
            g_disc.phase = xland::PhaseDone;
        }
    }
}

void advanceCollect(uint8_t *buf, int len, IPAddress from, uint32_t now)
{
    if (len == 32 && ((uint16_t)((buf[0] << 8) | buf[1]) == kMagic))
    {
        const uint32_t did = ((uint32_t)buf[8] << 24) | ((uint32_t)buf[9] << 16) |
                             ((uint32_t)buf[10] << 8) | (uint32_t)buf[11];
        const uint32_t ts  = ((uint32_t)buf[12] << 24) | ((uint32_t)buf[13] << 16) |
                             ((uint32_t)buf[14] << 8) | (uint32_t)buf[15];
        xland::addCandidate(g_disc, (uint32_t)from, did, ts);
        g_discLastCandMs = now;      // 有任何 hello 响应 ⇒ 收集窗口重新计静默
    }

    /* 已有候选且连续 kDiscQuietMs 没有新响应 ⇒ 不再死等满 kDiscCollectMs */
    const bool quiet = (g_disc.candCount > 0) &&
                       ((uint32_t)(now - g_discLastCandMs) >= kDiscQuietMs);
    if (!quiet && (int32_t)(now - g_collectDeadline) < 0) return;

    g_discLastCollectMs = (g_discRoundStartMs != 0) ? (now - g_discRoundStartMs) : 0;

    if (g_disc.candCount == 0)
    {
        /* 没有任何 UDP 响应：如实结束，绝不把旧 IP 当有效地址 */
        g_discLastAuthMs = 0;
        endDiscoveryRound();
        return;
    }

    xland::beginAuthRound(g_disc);
    g_discAuthStartMs = now;
    g_discLastAuthMs = 0;
    g_roundSentProbes = sendAllAuthProbes();
    if (g_roundSentProbes == 0)
    {
        endDiscoveryRound();
        return;
    }
    g_disc.authDeadline = now + kAuthWindowMs;
}

void advanceAuth(uint8_t *buf, int len, IPAddress from, uint32_t now)
{
    if (len > 32) onAuthPacket(buf, len, from);

    bool remaining = false;
    for (unsigned char dev = 0; dev < 2; dev++)
    {
        if (xland::probeRemaining(g_disc, dev, g_disc.candCount)) { remaining = true; break; }
    }

    if (!remaining || (int32_t)(now - g_disc.authDeadline) >= 0)
    {
        g_discLastAuthMs = (g_discAuthStartMs != 0) ? (now - g_discAuthStartMs) : 0;
        endDiscoveryRound();
    }
}

/**
 * 发现状态机推进（每个 loop 一次）。
 * @param len 0 表示本轮没有属于发现层的包，仅推进超时。
 */
void advanceDiscovery(uint8_t *buf, int len, IPAddress from)
{
    const uint32_t now = millis();
    if (g_disc.phase == xland::PhaseVerifying)
    {
        advanceVerify(buf, len, from, now);
        return;
    }
    if (g_disc.phase == xland::PhaseCollecting)
    {
        advanceCollect(buf, len, from, now);
        return;
    }
    if (g_disc.phase == xland::PhaseAuthing)
    {
        advanceAuth(buf, len, from, now);
    }
}

// =====================================================
// 状态更新
// =====================================================
void setOnline(bool feeder, bool online)
{
    state_set_bool(feeder ? STATE_XIAOMI_FEEDER_ONLINE : STATE_XIAOMI_PLUG_ONLINE, online);
}

/*
 * ★ [2026-10-29] 诊断计数发布（只读、脱敏：纯数字，不含任何凭据）。
 *
 * 为什么必须暴露：只有 `recv` 与 `badsum` 两个数字就能区分
 *   recv=0             ⇒ 局域网根本没回包（IP 变了 / 不在同一网段 / 设备断电）
 *   recv>0 & badsum>0  ⇒ 设备**在线**且回了包，但校验和不对 ⇒ **Token 不对**
 * 没有这两个数字时，上面两种完全不同的故障在 UI 上都只是"离线"。
 */
void publishDiag()
{
    state_set_int(STATE_XIAOMI_LAN_RECV, (int)g_statRecv);
    state_set_int(STATE_XIAOMI_LAN_BADSUM, (int)g_statBadChecksum);
    state_set_int(STATE_XIAOMI_LAN_TIMEOUT_CNT, (int)g_statTimeout);
}

/** 记录"这台设备最近一次为什么不是成功"——online 只在**连续**失败后置 false */
void markErr(bool feeder, uint8_t err)
{
    state_set_int(feeder ? STATE_XIAOMI_FEEDER_LAST_ERR : STATE_XIAOMI_PLUG_LAST_ERR, (int)err);
    uint8_t &s = g_failStreak[feeder ? 0 : 1];
    if (s < 255) s++;

    /* ---------------------------------------------------------------
     * ★ [P0-3 · 2026-10-31] 只读请求的**有界重试**：每个失败序列只重试 1 次。
     *
     * 为什么只对只读请求做：
     *   · 出粮 / 通电是**非幂等**物理动作，超时重发 = 可能重复出粮或让插座
     *     通断两次 ⇒ 永远不自动重试（既有铁律，不变）。
     *   · 只读 `get_properties` 无副作用，失败多为"米家设备对密集握手丢包"，
     *     隔几秒再问一次通常就成功 —— 这正是用户"再点一次就恢复"的真实体验。
     *
     * 为什么只重试一次（`s == 1`）：
     *   · 连败计数已经负责"连续失败 3 次 + 距上次成功 ≥120 s ⇒ 判离线"；
     *     若每次失败都立即重试，会在几秒内把 3 次配额打光，
     *     把"瞬时丢包"误判成"设备离线"（历史上踩过这个坑）。
     *   · 所以：首败后隔 kProbeRetryMs 再问一次；仍失败就如实返回未知，
     *     等下一个 60 s 周期，绝不无限重试。
     * --------------------------------------------------------------- */
    const bool wasRead = (reqOf(feeder).kind == REQ_READ_FEEDER || reqOf(feeder).kind == REQ_READ_PLUG);
    const bool commErr = (err == XIAOMI_ERR_TIMEOUT || err == XIAOMI_ERR_AUTH);

    /*
     * ★★ [2026-11-04] 通信层失败 ⇒ 该设备的 RAM IP 作废，重新发现。
     *   这是 DHCP 换址后自动恢复的触发点：旧 IP 不再被当作有效地址。
     *   · AUTH（校验和/解密失败）立刻失效 —— 这个 IP 上很可能已不是原设备；
     *   · TIMEOUT 要求连败 ≥2 才失效 —— 米家设备对密集握手偶发丢包，
     *     单次丢包不该触发一次全网广播（否则反而加剧拥塞）。
     *   · 只动 IP 缓存，**绝不清 NVS Token**，也不影响另一台设备。
     */
    if (err == XIAOMI_ERR_AUTH || (err == XIAOMI_ERR_TIMEOUT && s >= 2))
    {
        dropCachedIp(feeder);      // 只丢值，不伪造"正在发现"的状态
    }

    if (wasRead && commErr && s == 1)
    {
        g_probeNow[feeder ? 0 : 1] = true;
        g_nextProbeMs = millis() + kProbeRetryMs;
        Serial.printf("[XiaomiLAN] read failed (err=%u), one bounded retry in %lums\n",
                      (unsigned)err, (unsigned long)kProbeRetryMs);
    }
    /*
     * ★ [2026-10-30] 单次失败**不改 online**（见 g_failStreak 注释）。
     *   连续失败达阈值，且**距上次成功已超过时间窗**才判离线 ——
     *   瞬时丢包（几秒内自愈）不能被说成设备掉线。
     */
    if (s >= kOfflineAfterFails)
    {
        const long now = (long)time(nullptr);
        const long lastOk = g_lastOkEpoch[feeder ? 0 : 1];
        const bool clockOk = (now > 1600000000L);
        /* 时钟未同步 ⇒ 无法判断"距上次成功多久"，此时只按连败次数判 */
        const bool agedOut = (!clockOk || lastOk <= 0) ? true
            : ((now - lastOk) >= kOfflineWindowSec);
        if (agedOut) setOnline(feeder, false);
    }
    publishDiag();
}

/** 记录"这台设备最近一次通信成功"——online=true + 记成功时刻 */
void markOk(bool feeder)
{
    setOnline(feeder, true);
    g_failStreak[feeder ? 0 : 1] = 0;      /* ★ 成功一次即清零连败计数 */
    /* ★ [P0-3] 单调时刻（供"状态是否新鲜"判断，与墙钟无关） */
    g_lastOkMs[feeder ? 0 : 1] = millis();
    state_set_int(feeder ? STATE_XIAOMI_FEEDER_LAST_ERR : STATE_XIAOMI_PLUG_LAST_ERR,
                  (int)XIAOMI_ERR_OK);

    /*
     * ★ [2026-10-30] 时钟未授时（SNTP 尚未同步）时**不写时间戳**。
     *
     * 实测踩到的假数据：设备上电约 7 s 时首次探活已成功，此时 `time(nullptr)==7`
     * ⇒ `*_last_ok_ts` 被写成 7 ⇒ UI 显示"上次成功 = 1970-01-01 08:00:07"。
     * 这属于**伪造时间**（宁可说"未记录"，也不能给一个看起来有效的 1970），
     * 而 0 在契约里就表示"未记录/从未成功"。
     */
    time_t now = time(nullptr);
    if (now > 1600000000L)   // 2020-09-13 之后才算有效墙钟
    {
        state_set_long(feeder ? STATE_XIAOMI_FEEDER_LAST_OK_TS : STATE_XIAOMI_PLUG_LAST_OK_TS,
                       (long)now);
        g_lastOkEpoch[feeder ? 0 : 1] = (long)now;   /* ★ 供"判离线时间窗"用 */
    }
    else
    {
        state_set_long(feeder ? STATE_XIAOMI_FEEDER_LAST_OK_TS : STATE_XIAOMI_PLUG_LAST_OK_TS, 0L);
    }
    publishDiag();
}

/**
 * 解析失败时区分"没收到"与"收到但解不开"。
 * ★ 判据是**本次请求内**是否新增了校验失败计数，而不是全局累计值。
 */
uint8_t classifyParseFail(bool feeder)
{
    const Request &r = reqOf(feeder);
    if (!r.gotPacket) return XIAOMI_ERR_TIMEOUT;
    if (g_statBadChecksum > r.badsum0) return XIAOMI_ERR_AUTH;
    return XIAOMI_ERR_TIMEOUT;
}

void finishRequest(bool feeder, bool ok)
{
    Request &r = reqOf(feeder);
    r.fin = true;
    r.ok = ok;
    r.active = false;
    state_set_int(STATE_XIAOMI_LAN_LAST_RESULT, ok ? 1 : 2);
    publishDiag();
}

/*
 * ★★ [2026-11-05 二期] 身份认证回包同时也是**一条成功的只读查询**。
 *
 * 认证用的就是该设备的特征属性（喂食器 2/26 out_status、插座 2/1 power），
 * 与 `REQ_READ_*` 完全同一条报文。因此：
 *   · 只读请求 ⇒ 认证回包就是答案，**不必再发第二条**（插座能省 ~200 ms）；
 *   · 控制请求 ⇒ 身份已确认，用回包里的最新 stamp 立刻发真正的命令帧。
 *
 * 语义边界没有变：控制类仍然"设备受理 ≠ 动作完成"，结果仍由 stage2 的回包决定。
 */
static bool applyVerifiedReply(unsigned char dev, const Reply &r)
{
    const bool feeder = (dev == xland::kDevFeeder);
    Request &rq = reqOf(feeder);
    if (!rq.active || rq.stage != 0) return false;   // 请求已超时/已被领走

    const bool isRead = (rq.kind == REQ_READ_FEEDER || rq.kind == REQ_READ_PLUG);
    if (isRead)
    {
        markOk(feeder);
        if (r.code == 0 && r.propCode == 0 && r.hasValue)
        {
            if (feeder) state_set_int(STATE_XIAOMI_FEEDER_OUT_STATUS, r.resultInt);
            else        state_set_bool(STATE_XIAOMI_PLUG_POWER, r.resultBool);
        }
        finishRequest(feeder, true);
        return true;
    }

    /* 控制类：用认证回包给出的 device_id / stamp 直接组命令帧。
       stamp 用"回包 ts + 1"，与 MiLAN_Test 的 `send_ts = _device_ts + 1` 一致。 */
    rq.dev->devId = r.devId;
    rq.dev->stamp = r.stamp + 1;

    char payload[kMaxPayload];
    buildCommand(rq.kind, rq.portions, payload, sizeof(payload));
    const IPAddress ip = devIp(feeder);
    if ((uint32_t)ip != 0 && sendEncryptedTo(*rq.dev, ip, payload))
    {
        rq.stage = 2;
        rq.deadline = millis() + kReplyTimeoutMs;
        return true;
    }

    /* 连命令都发不出去 ⇒ 如实失败，绝不"假装已下发" */
    finishRequest(feeder, false);
    markErr(feeder, XIAOMI_ERR_TIMEOUT);
    return false;
}

// =====================================================
// [T11 · 2026-11-01] 作废在途的非安全请求
//
// 真机故障：强制重启（300 ms 窗口）之前，插座又被通电了一次。
// 根因之一：米家请求分两阶段（握手 800 ms → 发命令 → 等回包 3000 ms），
// 而 `safety_stop_all()` 只停本地执行器（阀/步进/振动），**完全没碰米家**。
// ⇒ 重启倒计时窗口里，一条排在握手阶段的「插座通电」照样被发出去，
//   插座属于外部设备，重启也断不掉它，于是"重启后插座一直通电"。
//
// 语义约束（刻意保守）：
//   · 只作废 `REQ_PLUG_OFF` **以外**的请求；断电是安全方向，绝不在这里丢；
//   · 作废 = finishRequest(false)，即动作回调看到 FAILED，不会自动重试；
//   · 不改变任何"设备在线"状态（在线与否与"我们中止了一次请求"无关）。
// =====================================================
uint8_t abortNoncriticalImpl(const char *reason)
{
    uint8_t cancelled = 0;

    for (uint8_t i = 0; i < 2; i++)
    {
        Request &r = g_req[i];
        if (!r.active)
        {
            continue;
        }
        /* 断电保留：它是唯一允许在重启窗口内继续推进的米家动作 */
        if (r.kind == REQ_PLUG_OFF)
        {
            continue;
        }

        const bool feeder = (i == DEV_FEEDER);
        Serial.printf(
            "[XiaomiLAN] abort in-flight req kind=%u dev=%s (%s) stage=%u\n",
            (unsigned)r.kind,
            feeder ? "feeder" : "plug",
            (reason != nullptr) ? reason : "abort",
            (unsigned)r.stage);

        finishRequest(feeder, false);
        cancelled++;
    }

    return cancelled;
}

bool devEnabled(bool feeder)
{
    /*
     * ★ [P0-1 · 2026-10-31] 只读 **ConfigManager 内存**，不碰 Flash。
     *
     * 键名与默认值统一定义在 `config_manager.h`：
     *   `CONFIG_XIAOMI_KEY_FEEDER_ENABLE` / `..._PLUG_ENABLE`
     *   `CONFIG_XIAOMI_ENABLE_DEFAULT`（true = 默认启用）
     * `config_init()` 会把缺失的键补成默认值（只补缺、不覆盖、不落盘）
     * ⇒ 这里读到的永远是一个**明确**的布尔，不再是"键不存在 ⇒ false"。
     *
     * 若 config 尚未初始化（理论上不会：main.cpp 里 config_init 早于
     * xiaomi_lan_init），退回默认值而不是 false —— 保持与声明一致。
     */
    bool v = CONFIG_XIAOMI_ENABLE_DEFAULT;
    if (!config_get_bool(kCfgModule,
                         feeder ? CONFIG_XIAOMI_KEY_FEEDER_ENABLE
                                : CONFIG_XIAOMI_KEY_PLUG_ENABLE,
                         v))
    {
        v = CONFIG_XIAOMI_ENABLE_DEFAULT;
    }
    return v;
}

// =====================================================
// 发起一次请求（内部统一入口）
//
// ★ [2026-10-29] 返回值语义收紧：**失败必须能说清原因**。
//   旧实现所有失败都 `return false` ⇒ 上层只能显示"失败"，
//   UI 也就只能显示"离线"。现在失败前先把原因写进 `*_last_err`：
//     忙（BUSY）/ 未配置（NO_CFG）/ 暂停用（DISABLED）/ 发送失败（TIMEOUT）
//   ★ BUSY 与 DISABLED **不改变 online**：设备可能好好在线，只是这次没发出去。
// =====================================================
bool startRequest(uint8_t kind, uint8_t portions, DevInfo &dev, bool feeder,
                  const WorkflowActionDescriptor *owner)
{
    /*
     * ★★ [P4 · 2026-11-01 · 任务书 P0-2 / P0-4] 互斥粒度改为**按设备**。
     *
     * 旧实现：`if (g_req.active)` —— **全局**单槽 ⇒ 喂食器忙 ⇒ 插座被拒。
     *
     * 现在分三种情况：
     *   ① **别的设备**的槽在忙 ⇒ **不阻塞**（跨设备并行，任务书 P0-2 核心要求）；
     *   ② **同一台设备**的槽在忙 ⇒ 拒绝（BUSY），保持协议要求的同设备串行；
     *      ★ 例外：**断电**可以抢占该设备正在进行的**只读查询**
     *        （只读无副作用，中止安全；不抢占物理动作，避免半途打断出粮/通电）
     *        —— 这是任务书 P0-4「紧急断电不能被普通命令长期锁定」的实现。
     *
     * ★ 判定走**纯函数** `xlan::decideSlot()`（src/app/xiaomi_lan_slot.h），
     *   宿主侧有配套测试 ⇒ 这里执行的就是被测的那份逻辑，不是复制品。
     */
    Request &r = reqOf(feeder);

    /*
     * ★★ [T11 · 2026-11-01] 先清掉**上一轮**残留在槽里的完成标记。
     *
     * 缺陷（真机）：`actionPoll()` 只按 `descriptor` 指针认领结果
     *   （loop2：`r.owner == descriptor && r.fin`）。
     *   而 `fin/ok` 是**槽级**状态，finishRequest() 之后不会自动清。
     *   ⇒ 本次 startRequest **发不出去**（BUSY / 未启用 / 未配置）时，
     *     fin 仍是上一轮的 true ⇒ poll 会用**上一次的结果**覆盖本次的
     *     ACTION_FAILED ⇒ "这次断电根本没发出去"，UI 却显示成功。
     *
     *   这在插座上是真危险：断电失败被显示成"已断电"。
     *
     * 修法：只要本槽当前没有在途请求，就先把完成标记清掉并认领给本次
     *   descriptor。这样任何"没发出去"的路径都会走到 poll 的兜底
     *   `if (result == ACTION_IDLE) result = ACTION_FAILED`，如实报失败。
     *   ★ 槽正忙时不碰（那是别人的在途请求，不能替它清标记）。
     */
    if (!r.active)
    {
        /*
         * ★★ [T12 · 2026-11-02] 探活（owner==nullptr）**不得覆盖未领取的动作结果**。
         *
         * 真机复现（boot 253，用户授权的真实插座）：plug_test 的 step1
         *   XIAOMI_PLUG_ON **物理执行成功**（power=true），但完成结果被
         *   随后的探活抹掉 ⇒ actionPoll 匹配不上 ⇒ workflow 永挂 RUNNING
         *   ⇒ 10 分钟后才被 timeout 兜底（state=TIMEOUT，current_step=1）。
         *
         * 时序：动作完成（finishRequest：fin=true, owner=desc）→ forceProbe →
         *   **同一轮** xiaomi_lan_loop 尾部 periodicProbe() 立即 startRequest，
         *   走到下面这个分支把 owner 覆盖成 nullptr、fin 清零 —— 而
         *   workflow_task 的 actionPoll 在主循环里排在 xiaomi_lan_loop
         *   **之前** ⇒ 下一轮 poll 时 owner 已经不是自己了 ⇒ 结果永久丢失。
         *
         * 修法：探活类请求（owner==nullptr）看到槽里残留"fin 未领取"的
         *   动作结果时**本轮让路** —— workflow_task 下一轮 poll 会经
         *   `xiaomi_lan_release_result()` 领走并清槽，探活下轮自然放行。
         *   让步有上限（XLAN_PROBE_DEFER_MAX 轮），防止"动作永远没人领"
         *   时探活被饿死（例如 workflow 已超时终止后残留的结果）。
         */
        if (owner == nullptr && r.owner != nullptr && r.fin)
        {
            if (r.defer_count < XLAN_PROBE_DEFER_MAX)
            {
                r.defer_count++;
                return false;
            }
            /* 让步上限已到 ⇒ 结果视为陈旧，允许覆盖（计数复位在清槽/启动处） */
        }
        r.fin = false;
        r.ok = false;
        r.owner = owner;
        r.defer_count = 0;
    }

    if (r.active)
    {
        const xlan::SlotDecision dec = xlan::decideSlot(
            true, r.kind, kind);
        if (dec == xlan::PreemptRead)
        {
            Serial.println("[XiaomiLAN] plug-off preempts in-flight read query");
            finishRequest(feeder, false);   /* 中止那次只读查询（它没有副作用） */
        }
        else
        {
            /* 只记原因，**不动** online（设备是否在线与"忙"无关） */
            state_set_int(feeder ? STATE_XIAOMI_FEEDER_LAST_ERR : STATE_XIAOMI_PLUG_LAST_ERR,
                          (int)XIAOMI_ERR_BUSY);
            return false;
        }
    }
    if (!devEnabled(feeder))
    {
        state_set_int(feeder ? STATE_XIAOMI_FEEDER_LAST_ERR : STATE_XIAOMI_PLUG_LAST_ERR,
                      (int)XIAOMI_ERR_DISABLED);
        setOnline(feeder, false);
        return false;
    }
    if (!dev.configured)
    {
        markErr(feeder, XIAOMI_ERR_NO_CFG);
        return false;
    }

    Request &rq = reqOf(feeder);
    rq.active = true;
    rq.kind = kind;
    rq.portions = portions;
    rq.dev = &dev;
    rq.owner = owner;
    rq.fin = false;
    rq.ok = false;
    rq.gotPacket = false;
    rq.badsum0 = g_statBadChecksum;

    /*
     * ★★ [2026-11-05 二期] **每一次独立操作都必须重新确认身份**。
     *
     * 这里刻意不区分"有没有缓存"：两条路都必然做一次 Token 认证 ——
     *   · 有缓存 IP ⇒ `beginOperationDiscovery()` 走单播快路径
     *     （hello → 加密只读查询 → 解密 + id 关联，全都通过才算数）；
     *   · 无缓存 / 验证失败 ⇒ 广播 + 并行认证。
     * 「缓存有效就跳过验证」的捷径被彻底删除：DHCP 换址后旧地址可能
     * 已经属于**另一台米家设备**（§2.1 的核心场景），跳过验证就是把命令
     * 打到别人家里去。
     *
     * 时间预算：`kDiscRequestBudgetMs`(20 s) 覆盖「单播验证 + 可能的广播回退
     *   + 并行认证 + 命令帧（kReplyTimeoutMs）」。超时如实失败，
     *   绝不自动重试出粮/通断这类非幂等物理动作。
     */
    rq.stage = 0;
    rq.discKick = 0;
    rq.deadline = millis() + kDiscRequestBudgetMs;
    beginOperationDiscovery(feeder ? xland::kDevFeeder : xland::kDevPlug);
    return true;
}

// =====================================================
// 状态机推进（每个 loop 调用）
// =====================================================

/*
 * ★ [P4 · 2026-11-01] 推进**单个设备**的请求槽。
 *
 * `buf/len` 是本轮收到的回包（可能为 0 = 本轮没收到包）。
 * ★ 回包必须**按来源 IP** 分派到正确的槽 —— 否则两台设备并行时会串台：
 *   喂食器的槽收到插座的回包 ⇒ 解析失败 ⇒ 被误判成超时/Token 错。
 *   分派逻辑在 `advanceRequest()` 里做（收包后立即按 remoteIP 归位）。
 */
static void advanceSlot(bool feeder, uint8_t *buf, int len)
{
    Request &g_req = reqOf(feeder);   /* ★ 局部引用，语义同旧全局变量 */
    const bool isFeeder = feeder;
    const uint32_t now = millis();

    if (len > 0) { g_statRecv++; g_req.gotPacket = true; }

    /*
     * ★★ [2026-11-04] stage 0 = 等待动态发现给出有效 IP。
     *   发现是 RAM-only 的，成功后 `devIp()` 才有值；超时如实失败，
     *   **不自动重试**（出粮/通断非幂等）。
     */
    if (g_req.stage == 0)
    {
        const unsigned char idx = isFeeder ? xland::kDevFeeder : xland::kDevPlug;

        /*
         * ★★ [2026-11-05 二期] 只有发现层**本次操作内**确认过身份，
         *   `g_devReady` 才为真。缓存里"看起来有个 IP"绝不代表可以发命令 ——
         *   那正是 DHCP 换址后把命令打到别的米家设备上的成因。
         */
        if (g_devReady[idx] && (uint32_t)devIp(isFeeder) != 0)
        {
            char payload[kMaxPayload];
            buildCommand(g_req.kind, g_req.portions, payload, sizeof(payload));
            if (sendEncryptedTo(*g_req.dev, devIp(isFeeder), payload))
            {
                g_req.stage = 2;
                g_req.deadline = now + kReplyTimeoutMs;
            }
            else
            {
                finishRequest(isFeeder, false);
                markErr(isFeeder, XIAOMI_ERR_TIMEOUT);
            }
            return;
        }

        /*
         * ★ 身份冲突（同一 Token 有多台设备通过认证 / 同一 IP 被两台设备认领）
         *   ⇒ 本轮**不猜**，如实失败。下一轮从干净状态重新发现。
         */
        if ((g_disc.conflictMask & xland::devMask(idx)) &&
            (g_disc.phase == xland::PhaseDone || g_disc.phase == xland::PhaseIdle))
        {
            finishRequest(isFeeder, false);
            markErr(isFeeder, XIAOMI_ERR_CONFLICT);
            return;
        }

        /*
         * ★★ [2026-11-05] 有界补发现（每请求最多 2 次）。
         *   发现层已结束（Done/Idle）但本设备仍未确认身份 ⇒ 要么本轮没覆盖到它
         *   （中途加入被游标跳过），要么单播快路径刚失败。两种情况都值得再跑一轮，
         *   但**必须有上限**：设备真离线时绝不允许把 20 s 预算刷成十几轮广播。
         *   剩余预算不足 3 s 时不再补 —— 补了也来不及完成认证与命令帧。
         *
         *   ★ 无网络时**不计次、不补轮**（上电瞬间就在等 WiFi 的情况）：
         *     否则空转会把额度烧光，WiFi 连上后反而不发现（真机已踩到）。
         */
        if (WiFi.status() != WL_CONNECTED)
        {
            return;                     // 等网络；网络恢复时由 loop 重置额度与预算
        }
        if ((g_disc.phase == xland::PhaseIdle || g_disc.phase == xland::PhaseDone) &&
            g_req.discKick < 2 &&
            (int32_t)(g_req.deadline - now) > 3000)
        {
            g_req.discKick++;
            g_discRetryAt = 0;
            Serial.printf("[XiaomiLAN] stage0 re-verify dev=%s try=%u\n",
                          isFeeder ? "feeder" : "plug", (unsigned)g_req.discKick);
            beginOperationDiscovery(idx);
            return;
        }

        if ((int32_t)(now - g_req.deadline) >= 0)
        {
            finishRequest(isFeeder, false);
            markErr(isFeeder, XIAOMI_ERR_TIMEOUT);
        }
        return;
    }

    if (g_req.stage == 2)
    {
        if (len > 32)
        {
            Reply r;
            if (parseReply(*g_req.dev, buf, len, r) && r.valid)
            {
                /*
                 * 语义分层（不得混为一谈）：
                 *   · 能解密且校验和通过 ⇒ **通信成功** ⇒ online=true
                 *   · code==0 且属性级 code==0 且真的取到 value ⇒ **本属性读取成功**
                 *   · 控制类 code==0 只代表**设备受理**，不代表物理动作已完成
                 */
                markOk(isFeeder);

                const bool okCode = (r.code == 0) && (r.propCode == 0);
                bool applied = false;

                if (okCode)
                {
                    if (g_req.kind == REQ_READ_FEEDER)
                    {
                        if (r.hasValue)
                        {
                            state_set_int(STATE_XIAOMI_FEEDER_OUT_STATUS, r.resultInt);
                            applied = true;
                        }
                    }
                    else if (g_req.kind == REQ_READ_PLUG)
                    {
                        if (r.hasValue)
                        {
                            state_set_bool(STATE_XIAOMI_PLUG_POWER, r.resultBool);
                            applied = true;
                        }
                    }
                    else
                    {
                        applied = true;   // 控制类：设备受理
                        /*
                         * ★★ [2026-11-05 二期 · 真机验收发现] 控制类成功只代表
                         *   「设备受理」，**不代表**物理动作完成，更不能据此改写
                         *   缓存的 power/out_status（那会把"受理"当成"执行成功"）。
                         *   但页面会立刻拿旧值下结论：刚点完「通电」，卡片仍写
                         *   「已断电」—— 这是**错误的确定性**，比"未知"更糟。
                         *
                         *   做法：受理成功后**立刻安排一次只读确认**（REQ_READ_*），
                         *   让 UI 在几百毫秒内收敛到设备事实。
                         *   ★ 这不是重发控制命令：只读 get_properties，无副作用。
                         */
                        /* 真正的排程在下面 `g_confirmAt`（见控制类收尾处） */
                    }
                }

                /*
                 * ★ [2026-10-29] 通信成功但属性层失败 ⇒ 记 ATTR，
                 *   **不把 online 打回 false**（设备明明在线，只是这个属性读不到）。
                 */
                if (!(okCode && applied))
                {
                    state_set_int(isFeeder ? STATE_XIAOMI_FEEDER_LAST_ERR
                                           : STATE_XIAOMI_PLUG_LAST_ERR,
                                  (int)XIAOMI_ERR_ATTR);
                }

                // 属性缺失/类型不符 ⇒ 失败（不覆盖上一次有效值，也不冒充成功）
                finishRequest(isFeeder, okCode && applied);

                /*
                 * ★ [2026-10-29] 控制类成功后的收尾：
                 *   · 断电成功 ⇒ 解除限时定时器（含 NVS 里的截止时刻）
                 *   · 通电/断电成功 ⇒ **立刻安排一次只读探活**，用真实回报
                 *     确认 power，而不是靠本地猜测写 xiaomi_plug_power
                 *     （"设备受理"不等于"已执行"，所以不写死状态值）
                 */
                if (g_req.ok && g_req.kind == REQ_PLUG_OFF) disarmAutoOff();
                if (g_req.ok)
                {
                    if (g_req.kind == REQ_PLUG_ON || g_req.kind == REQ_PLUG_OFF)
                    {
                        /*
                         * ★★ [2026-11-05 二期 · 真机验收发现] 这里**不能**用
                         *   `forceProbe()`：它只是把 g_nextProbeMs 置 0，随后的
                         *   `periodicProbe()` 会先把 60 s 计时打上、再调 startRequest；
                         *   而控制类刚结束时"上一条结果还没被 workflow 领走"，
                         *   startRequest 会**延迟不发** ⇒ 探测被白等 60 s，
                         *   System State 里 power 一直是旧值（真机实测）。
                         *   改成一个独立的**延时确认队列**（见 xiaomi_lan_loop），
                         *   到点、槽空、且 startRequest 真的受理了才清标志。
                         */
                        g_confirmAt[isFeeder ? 0 : 1] = millis() + kConfirmReadDelayMs;
                        Serial.printf("[XiaomiLAN] confirm read dev=%s scheduled in %ums\n",
                                      isFeeder ? "feeder" : "plug",
                                      (unsigned)kConfirmReadDelayMs);
                    }
                    else if (g_req.kind == REQ_FEED) forceProbe(true);
                }
                return;
            }
            // 解不开（校验和/JSON 失败）⇒ 失败，不重试
            finishRequest(isFeeder, false);
            markErr(isFeeder, classifyParseFail(isFeeder));
            return;
        }
        if ((int32_t)(now - g_req.deadline) >= 0)
        {
            g_statTimeout++;
            finishRequest(isFeeder, false);
            markErr(isFeeder, XIAOMI_ERR_TIMEOUT);
        }
    }
}

/*
 * ★★ [P4 · 2026-11-01 · 任务书 P0-2] 双槽调度器。
 *
 * 职责：
 *   ① 本轮**只收一个** UDP 包（UDP 是单 socket，物理上也只能逐个取）；
 *   ② 按**来源 IP** 把这个包分派给对应的设备槽；
 *      —— 这是双槽并行安全的关键：不分派就会串台
 *         （喂食器的槽吃到插座的回包 ⇒ 解析失败 ⇒ 误判超时/Token 错）。
 *   ③ 依次推进两个槽（各自独立判超时）。
 *
 * ★ 为什么要按 IP 而不是按 devId：
 *   devId 要**先解密**才能拿到，而解密需要该设备自己的 token 与会话号；
 *   而回包**来源 IP** 是明文可见的，直接比对即可。
 *   （IP 来自 `g_ipCache` —— 本次运行周期里经 Token 认证确认过的 RAM 值。）
 *
 * ★ [2026-11-04] 分派优先级（发现层与请求槽共存同一个 UDP socket）：
 *     ① 来自某台**已验证 IP** 且该槽处在"命令在飞"（stage>=2）⇒ 交给该槽；
 *     ② 否则若发现层在跑（单播验证 / 广播收集 / 并行认证）⇒ 交给发现层；
 *     ③ 都不匹配 ⇒ 丢弃（仍计入 g_statRecv）。
 *   发现层每轮都被调用一次：没有属于自己的包时传 len=0，仅推进超时。
 *
 * ★ [2026-11-05 二期] ① 增加了 `stage >= 2` 条件：stage0 是"身份还没确认"，
 *   此时的包属于**验证流程**，喂给请求槽会把认证回包吃掉、让验证永远超时。
 */
void advanceRequest()
{
    const bool feederBusy = devBusy(true);
    const bool plugBusy = devBusy(false);
    const bool discActive = (g_disc.phase == xland::PhaseVerifying ||
                             g_disc.phase == xland::PhaseCollecting ||
                             g_disc.phase == xland::PhaseAuthing);
    if (!feederBusy && !plugBusy && !discActive) return;

    uint8_t buf[kMaxFrame];
    int len = 0;
    IPAddress from;

    if (g_udp.parsePacket() > 0)
    {
        from = g_udp.remoteIP();
        len = g_udp.read(buf, sizeof(buf));
        if (len > 0) g_statRecv++;
    }

    int feederLen = 0;
    int plugLen = 0;
    int discLen = 0;
    if (len > 0)
    {
        const IPAddress fIp = devIp(true);
        const IPAddress pIp = devIp(false);
        if (feederBusy && reqOf(true).stage >= 2 && (uint32_t)fIp != 0 && from == fIp)
        {
            feederLen = len;
        }
        else if (plugBusy && reqOf(false).stage >= 2 && (uint32_t)pIp != 0 && from == pIp)
        {
            plugLen = len;
        }
        else if (discActive)
        {
            discLen = len;
        }
        /* 其余：陌生设备 / 迟到包 ⇒ 丢弃，不喂给任何槽（防串台） */
    }

    if (discActive) advanceDiscovery(buf, discLen, from);
    if (feederBusy) advanceSlot(true, buf, feederLen);
    if (plugBusy) advanceSlot(false, buf, plugLen);
}

// =====================================================
// 周期探活（只读）
// =====================================================
void periodicProbe()
{
    /*
     * ★ [P0-3 · 2026-10-31] "尽快探活"优先于周期节拍。
     *
     * 场景：pre-flight 发现某台设备状态过期/从未成功 ⇒ 置 g_probeNow[dev]，
     *   本次执行被拒绝；用户重试时若还要等满 60 s 周期，体验就是"点了没反应"。
     * ⇒ 允许把下一次探活提前到现在（**仍然只发一包**，不是重试风暴）。
     * ★ 这里只改"什么时候发"，不改"怎么判离线"，120 s 时间窗规则原样保留。
     */
    bool forceFeeder = g_probeNow[0];
    bool forcePlug = g_probeNow[1];

    if (forceFeeder || forcePlug)
    {
        /*
         * 强制探活**仍然受 g_nextProbeMs 约束**：
         *   · `xiaomi_lan_request_probe()` 把它设为"现在" ⇒ 立刻发；
         *   · 失败后的**有界重试**把它设为 now + kProbeRetryMs ⇒ 至少隔一会儿，
         *     不会变成"连打"，也就不会触发米家设备对密集丢包的老问题。
         */
        if ((int32_t)(millis() - g_nextProbeMs) < 0) return;

        /* 只清掉本次要用的那个标志；发不出去（未启用/未配置）也必须清，
           否则标志会永远挂着，把后面的周期探活全部压缩成立即探活。 */
        if (forceFeeder) g_probeNow[0] = false;
        if (forcePlug) g_probeNow[1] = false;

        if (forceFeeder && devEnabled(true) && g_feeder.configured && !devBusy(true))
        {
            g_nextProbeMs = millis() + kProbeIntervalMs;
            startRequest(REQ_READ_FEEDER, 0, g_feeder, true, nullptr);
            return;
        }
        if (forcePlug && devEnabled(false) && g_plug.configured && !devBusy(false))
        {
            g_nextProbeMs = millis() + kProbeIntervalMs;
            startRequest(REQ_READ_PLUG, 0, g_plug, false, nullptr);
            return;
        }
        /* 落到这里说明那台设备本轮无法发请求（未启用/未配置）⇒ 继续走周期逻辑 */
    }

    if ((int32_t)(millis() - g_nextProbeMs) < 0) return;

    // 轮转两台设备，避免每轮都打两包
    for (uint8_t i = 0; i < 2; i++)
    {
        uint8_t turn = (uint8_t)((g_probeTurn + i) % 2);
        /* ★ [P4] 已在飞的设备不再发起新查询（自己的槽忙） */
        if (turn == 0 && devEnabled(true) && g_feeder.configured && !devBusy(true))
        {
            g_probeTurn = 1;
            g_nextProbeMs = millis() + kProbeIntervalMs;
            startRequest(REQ_READ_FEEDER, 0, g_feeder, true, nullptr);
            return;
        }
        if (turn == 1 && devEnabled(false) && g_plug.configured && !devBusy(false))
        {
            g_probeTurn = 0;
            g_nextProbeMs = millis() + kProbeIntervalMs;
            startRequest(REQ_READ_PLUG, 0, g_plug, false, nullptr);
            return;
        }
    }
    g_nextProbeMs = millis() + kProbeIntervalMs;
}

// =====================================================
// ★ [2026-10-29] 插座"普通通电 60 秒"的执行端安全定时器
//
// 为什么必须在固件里做：
//   UI 倒计时在**页面关闭 / 手机锁屏 / MQTT 断线**时就没了，
//   而插座会一直通电 —— 这正是需求禁止的"只靠前端保证安全"。
//   这里由设备自己到点发断电，并把截止时刻**持久化到 NVS**，
//   即使 ESP32 重启也能恢复（重启后时钟有效即继续计时）。
// =====================================================
uint32_t g_autoOffEpoch = 0;   // 0 = 无待执行自动断电；否则为 Unix 秒
uint8_t g_autoOffTry = 0;

void disarmAutoOff()
{
    g_autoOffEpoch = 0;
    g_autoOffTry = 0;
    Preferences p;
    if (p.begin(kNvsNs, false)) { p.remove("p_off_at"); p.end(); }
    state_set_int(STATE_XIAOMI_PLUG_AUTOOFF_SEC, 0);
}

void armAutoOff(uint32_t sec)
{
    time_t now = time(nullptr);
    if (now <= 0)
    {
        /*
         * 时钟还没校准 ⇒ 不用"伪 epoch"硬算（否则会在下次对时后突然触发或永不到期）。
         * 此时拒绝启用限时通电，把决定权交回上层：宁可不做，也不留一个不可靠的定时器。
         */
        Serial.println("[XiaomiLAN] auto-off armed refused: clock invalid");
        return;
    }
    g_autoOffEpoch = (uint32_t)now + sec;
    g_autoOffTry = 0;
    Preferences p;
    if (p.begin(kNvsNs, false)) { p.putUInt("p_off_at", g_autoOffEpoch); p.end(); }
    state_set_int(STATE_XIAOMI_PLUG_AUTOOFF_SEC, (int)sec);
    Serial.printf("[XiaomiLAN] auto-off armed in %us\n", (unsigned)sec);
}

/** 每个 loop 调用：到点就发断电（幂等、可重试；与出粮不同，断电重发无副作用） */
void serviceAutoOff()
{
    if (g_autoOffEpoch == 0) return;

    time_t now = time(nullptr);
    if (now <= 0) return;                       // 时钟无效：等待，不误动作

    int32_t left = (int32_t)g_autoOffEpoch - (int32_t)now;
    if (left > 0)
    {
        state_set_int(STATE_XIAOMI_PLUG_AUTOOFF_SEC, (int)left);
        return;
    }
    /*
     * ★ [P4 · 2026-11-01] 只等**插座自己的**通道。
     *   旧实现等全局通道 ⇒ 喂食器在飞时，插座的自动断电也被拖延（最多 3 s）。
     *   现在双槽并行 ⇒ 喂食器忙不再拖延插座断电。
     */
    if (devBusy(false)) return;                 // 等当前请求让出通道

    if (g_autoOffTry >= 5)
    {
        // 连续 5 次发不出去 ⇒ 如实记录失败，不再无限重试
        state_set_int(STATE_XIAOMI_PLUG_LAST_ERR, (int)XIAOMI_ERR_TIMEOUT);
        Serial.println("[XiaomiLAN] auto-off failed after 5 tries");
        disarmAutoOff();
        return;
    }
    g_autoOffTry++;
    Serial.printf("[XiaomiLAN] auto-off firing (try %u)\n", (unsigned)g_autoOffTry);
    startRequest(REQ_PLUG_OFF, 0, g_plug, false, nullptr);
}

/** 让下一次 loop 立刻探活指定设备（用于动作完成后尽快确认真实状态） */
void forceProbe(bool feeder)
{
    g_probeTurn = feeder ? 0 : 1;
    g_nextProbeMs = 0;
}

// =====================================================
// Action 回调
// =====================================================
void actionReset(WorkflowActionInstance *action)
{
    if (action) action->result = ACTION_IDLE;
}

void actionPoll(WorkflowActionInstance *action)
{
    if (action == nullptr) return;
    /*
     * ★ [P4 · 2026-11-01] 两个槽都要看。
     *   旧实现只看全局单槽；双槽后，动作的 owner 可能落在任一槽上。
     *   （owner 是描述符指针，全局唯一 ⇒ 两个槽里至多一个会匹配。）
     */
    for (uint8_t i = 0; i < 2; i++)
    {
        const Request &r = g_req[i];
        if (r.active && r.owner == action->descriptor)
        {
            action->result = ACTION_RUNNING;
            return;
        }
    }
    for (uint8_t i = 0; i < 2; i++)
    {
        Request &r = g_req[i];
        if (r.owner == action->descriptor && r.fin)
        {
            action->result = r.ok ? ACTION_SUCCESS : ACTION_FAILED;
            /*
             * ★★ [T12 · 2026-11-02] 领走即清槽。
             *   结果被 workflow 领走后立刻清掉 owner/fin —— 否则探活的
             *   startRequest 要靠"让步"来保护一个已经没有意义的结果，
             *   而且下次 startRequest 的提前认领也依赖槽是干净的。
             *   （非阻塞：清的是本动作自己的槽，不动另一台。）
             */
            if (!r.active)
            {
                r.fin = false;
                r.ok = false;
                r.owner = nullptr;
                r.defer_count = 0;
            }
            return;
        }
    }
    /*
     * ★★ [T12 · 2026-11-02] 兜底必须覆盖 RUNNING，不能只救 IDLE。
     *
     * 真机复现（boot 253）：动作物理执行成功，结果被探活覆盖 ⇒ 两个
     *   匹配 loop 都找不到 ⇒ 旧兜底 `result == ACTION_IDLE` 不生效
     *   （result 已是 start 时置的 RUNNING）⇒ 永挂 RUNNING ⇒ 只能等
     *   workflow 级 10 分钟超时（用户看到的就是"卡在 Delay 不推进"）。
     *
     * 能走到这里 = 槽里**既没有**本动作的在途请求（loop1 未命中），
     *   **也没有**本动作的完成结果（loop2 未命中）⇒ 请求已经消失且
     *   永远不会再回来 ⇒ 唯一诚实的终态就是 ACTION_FAILED。
     *   （若未来出现"请求短暂消失但会回来"的新路径，必须在这里补白名单，
     *     而不是回退本兜底 —— 永挂比误报失败危险得多：误报失败用户会
     *     重试，永挂则插座保持通电且 UI 永远"执行中"。）
     */
    if (action->result == ACTION_IDLE || action->result == ACTION_RUNNING)
    {
        action->result = ACTION_FAILED;
    }
}

int readPortions(WorkflowActionInstance *action)
{
    if (action == nullptr) return 0;
    for (uint8_t i = 0; i < action->param_count && i < WORKFLOW_MAX_PARAM; i++)
    {
        // 参数名 "portions"
        if (action->params[i].type == PARAM_INT && action->params[i].name == "portions")
        {
            return action->params[i].int_value;
        }
    }
    return 0;
}

void startResult(WorkflowActionInstance *action, bool ok)
{
    if (action == nullptr) return;
    action->result = ok ? ACTION_RUNNING : ACTION_FAILED;
}

void feedStart(WorkflowActionInstance *action)
{
    int portions = readPortions(action);
    if (portions < 1 || portions > 150) { startResult(action, false); return; }
    if (!devEnabled(true)) { startResult(action, false); return; }
    // ★ 幂等性：同一时刻只允许一个 LAN 请求；不排队、不重试
    startResult(action, startRequest(REQ_FEED, (uint8_t)portions, g_feeder, true, action->descriptor));
}

void plugOnStart(WorkflowActionInstance *action)
{
    if (!devEnabled(false)) { startResult(action, false); return; }
    /* 强制通电 = 不设自动断电（保持既有 XIAOMI_PLUG_ON 语义不变） */
    disarmAutoOff();
    startResult(action, startRequest(REQ_PLUG_ON, 0, g_plug, false, action->descriptor));
}

/*
 * ★ [2026-10-29] 普通通电（限时 60 秒）。
 *
 * 与 `XIAOMI_PLUG_ON` **不是同一个动作**：
 *   · XIAOMI_PLUG_ON      = 强制/长通电（不自动断电，供既有自动化沿用，语义未变）
 *   · XIAOMI_PLUG_ON_60S  = 普通通电（执行端 60 秒后自动断电）
 * 只有在"命令确实发出去"之后才武装定时器；发失败不武装，避免凭空断电。
 */
void plugOn60Start(WorkflowActionInstance *action)
{
    if (!devEnabled(false)) { startResult(action, false); return; }

    /*
     * ★★ 安全性（2026-10-29 自检修正）：**先确认"能武装定时器"，再发通电**。
     *
     * 若时钟还没校准（`time(nullptr) == 0`），`armAutoOff()` 无法算出截止时刻。
     * 旧写法是"先发通电、再武装" ⇒ 武装失败时插座会**一直通电且没有任何定时器**，
     * 正是需求禁止的"看似有 60 秒保护、实际没有"。
     * 现在的顺序保证：要么"通电 + 必然到点断电"，要么"干脆不通电"。
     */
    if (time(nullptr) <= 0)
    {
        Serial.println("[XiaomiLAN] plug-on-60s refused: clock invalid (no auto-off guarantee)");
        state_set_int(STATE_XIAOMI_PLUG_LAST_ERR, (int)XIAOMI_ERR_BUSY);
        startResult(action, false);
        return;
    }

    if (!startRequest(REQ_PLUG_ON, 0, g_plug, false, action->descriptor))
    {
        startResult(action, false);
        return;
    }
    armAutoOff(kPlugAutoOffSec);
    startResult(action, true);
}

void plugOffStart(WorkflowActionInstance *action)
{
    if (!devEnabled(false)) { startResult(action, false); return; }
    /* 用户主动断电 ⇒ 立即解除限时通电的定时器（幂等） */
    disarmAutoOff();
    startResult(action, startRequest(REQ_PLUG_OFF, 0, g_plug, false, action->descriptor));
}

/*
 * ★ [2026-10-29] 只读状态刷新（每台一条）。
 *
 * 存在的理由：UI 的「读取设备状态」过去只查 System State 的**缓存**，
 * 从来不发起一次真实 LAN 查询 ⇒ 用户点了也只看到旧值/初始 false。
 * 这两个动作让"点一下 = 真的去问一次设备"，且**不产生任何物理动作**
 * （只发 get_properties），可以安全地暴露给 UI 与自动化。
 */
void feederStatusStart(WorkflowActionInstance *action)
{
    startResult(action, startRequest(REQ_READ_FEEDER, 0, g_feeder, true, action->descriptor));
}

void plugStatusStart(WorkflowActionInstance *action)
{
    startResult(action, startRequest(REQ_READ_PLUG, 0, g_plug, false, action->descriptor));
}

static WorkflowParam feedParams[] = {
    {"portions", PARAM_INT, "portion", "1-150（1 份 = 10 克）"},
};

static WorkflowParam plugParams[] = {
    // 无参数
};

static WorkflowActionDescriptor feedDesc = {
    .id = "XIAOMI_FEED",
    .name = "米家喂食器出粮",
    .module = "xiaomi_lan",
    .description = "经局域网向米家喂食器下发 pet-food-out（份数）",
    .params = feedParams,
    .param_count = 1,
    .reset = actionReset,
    .start = feedStart,
    .poll = actionPoll,
};

static WorkflowActionDescriptor plugOnDesc = {
    .id = "XIAOMI_PLUG_ON",
    .name = "米家插座通电",
    .module = "xiaomi_lan",
    .description = "经局域网把米家插座 power 置 true",
    .params = plugParams,
    .param_count = 0,
    .reset = actionReset,
    .start = plugOnStart,
    .poll = actionPoll,
};

static WorkflowActionDescriptor plugOffDesc = {
    .id = "XIAOMI_PLUG_OFF",
    .name = "米家插座断电",
    .module = "xiaomi_lan",
    .description = "经局域网把米家插座 power 置 false",
    .params = plugParams,
    .param_count = 0,
    .reset = actionReset,
    .start = plugOffStart,
    .poll = actionPoll,
};

/* ★ [2026-10-29] 限时通电（执行端 60 秒自动断电） */
static WorkflowActionDescriptor plugOn60Desc = {
    .id = "XIAOMI_PLUG_ON_60S",
    .name = "米家插座通电（60 秒）",
    .module = "xiaomi_lan",
    .description = "通电并在设备端 60 秒后自动断电（不依赖浏览器）",
    .params = plugParams,
    .param_count = 0,
    .reset = actionReset,
    .start = plugOn60Start,
    .poll = actionPoll,
};

/* ★ [2026-10-29] 只读状态刷新：让 UI「读取设备状态」真的去问设备 */
static WorkflowActionDescriptor feederStatusDesc = {
    .id = "XIAOMI_FEEDER_STATUS",
    .name = "米家喂食器状态刷新",
    .module = "xiaomi_lan",
    .description = "读 get_properties 刷新喂食器在线与出粮状态（无物理动作）",
    .params = plugParams,
    .param_count = 0,
    .reset = actionReset,
    .start = feederStatusStart,
    .poll = actionPoll,
};

static WorkflowActionDescriptor plugStatusDesc = {
    .id = "XIAOMI_PLUG_STATUS",
    .name = "米家插座状态刷新",
    .module = "xiaomi_lan",
    .description = "读 get_properties 刷新插座在线与电源状态（无物理动作）",
    .params = plugParams,
    .param_count = 0,
    .reset = actionReset,
    .start = plugStatusStart,
    .poll = actionPoll,
};

// =====================================================
// NVS 凭证（★ 2026-11-04：IP **不再**持久化，这里只读 Token）
// =====================================================
bool loadDev(DevInfo &dev, const char *tokKey)
{
    Preferences p;
    if (!p.begin(kNvsNs, true)) return false;

    /* 旧版本 NVS 里可能残留 `f_ip` / `p_ip`：**既不读、也不删**。
       不读 ⇒ 运行时绝不依赖旧 IP；不删 ⇒ 不破坏既有数据与回滚能力。 */
    String tokStr = p.getString(tokKey, "");
    p.end();

    bool ok = false;
    if (tokStr.length() == 32)
    {
        uint8_t tok[16];
        if (parseToken(tokStr.c_str(), tok))
        {
            memcpy(dev.token, tok, 16);
            dev.configured = true;
            ok = true;
        }
        zeroBytes(tok, sizeof(tok));
    }
    // ★ 不在日志里出现任何凭据片段
    return ok;
}

} // namespace

// =====================================================
// 对外接口
// =====================================================

/*
 * [T11 · 2026-11-01] 公开包装。
 *
 * 本文件把内部实现整体放在匿名命名空间里（内部链接），
 * 对外只暴露 xiaomi_lan_* 这一层 ⇒ 实现必须定义在 namespace 之外，
 * 否则链接期找不到符号（本次编译第一次就是撞的这个）。
 */
uint8_t xiaomi_lan_abort_noncritical(const char *reason)
{
    return abortNoncriticalImpl(reason);
}
/*
 * ★★ [2026-11-04] 协议变更：`system.lan_prov` **只接受 dev + token**。
 *   IP 由设备在家庭 LAN 内广播发现后只存 RAM，不再是用户可维护的配置。
 */
bool xiaomi_lan_provision(const char *dev, const char *token, const char **reason)
{
    if (reason) *reason = "";
    if (dev == nullptr) { if (reason) *reason = "missing_dev"; return false; }

    const bool isFeeder = (strcmp(dev, "feeder") == 0);
    const bool isPlug = (strcmp(dev, "plug") == 0);
    if (!isFeeder && !isPlug) { if (reason) *reason = "unknown_dev"; return false; }

    if (token != nullptr && token[0] != 0)
    {
        uint8_t tmp[16];
        if (!parseToken(token, tmp)) { if (reason) *reason = "bad_token"; zeroBytes(tmp, sizeof(tmp)); return false; }
        zeroBytes(tmp, sizeof(tmp));
    }
    if (token == nullptr || token[0] == 0)
    {
        if (reason) *reason = "nothing_to_write";
        return false;
    }

    Preferences p;
    if (!p.begin(kNvsNs, false)) { if (reason) *reason = "nvs_open_failed"; return false; }
    /* ★ 只写 Token；绝不写任何 IP 键 */
    const bool ok = p.putString(isFeeder ? "f_tok" : "p_tok", token) > 0;
    p.end();

    if (!ok) { if (reason) *reason = "nvs_write_failed"; return false; }

    // 立即生效（无需重启）
    if (isFeeder) loadDev(g_feeder, "f_tok");
    else loadDev(g_plug, "p_tok");

    /* ★ Token 变了 ⇒ 旧 IP 是「旧凭证认证过」的结论，必须作废；下一次操作会重新发现 */
    dropCachedIp(isFeeder);

    /* ★ [2026-10-30 · A2] 写完立刻重算并发布"是否已配置"（页面据此回显，不信前端假成功） */
    state_set_bool(isFeeder ? STATE_XIAOMI_FEEDER_CONFIGURED : STATE_XIAOMI_PLUG_CONFIGURED,
                   isFeeder ? g_feeder.configured : g_plug.configured);

    /*
     * ★ [2026-10-30 · 凭证变更后状态作废] 这一步是"改 Token 后仍显示在线"的根治点。
     *
     * 为什么必须做：
     *   `xiaomi_<dev>_online` 记的是**上一次通信结论**，它属于**旧凭证**。
     *   凭证一换，那个 true 就不再描述任何事实 —— 可 UI 会照旧渲染「在线」，
     *   用户于是以为"Token 改错了也还是在线"，从而完全无法判断写入是否生效。
     *   （真机踩到：把 Token 改成 32 个 1 之后查询仍返回 online=true /
     *     last_err=1 / last_ok_ts=刚刚 —— 那是更换前最后一次成功的残留。）
     *
     * 做法：把"是否在线"打回**未验证**（online=false + last_err=NONE(0)，
     *   0 在契约里就是"尚未查询"），并立刻排一次**只读探活**。
     *   几秒后 UI 会拿到真正属于新凭证的结论：
     *     last_err=1 ⇒ Token 正确 · 3 ⇒ Token 不对 · 2 ⇒ 无响应（IP/网段/断电）
     *   注意：**不清 last_ok_ts** —— 它是历史事实，清掉等于抹掉证据。
     */
    state_set_bool(isFeeder ? STATE_XIAOMI_FEEDER_ONLINE : STATE_XIAOMI_PLUG_ONLINE, false);
    state_set_int(isFeeder ? STATE_XIAOMI_FEEDER_LAST_ERR : STATE_XIAOMI_PLUG_LAST_ERR,
                  (int)XIAOMI_ERR_NONE);
    g_failStreak[isFeeder ? 0 : 1] = 0;    /* ★ 凭证换了，连败计数重新算 */
    g_lastOkEpoch[isFeeder ? 0 : 1] = 0;  /* ★ 旧凭证的成功不再作数 */
    /*
     * ★ [2026-10-30] 这里**不要**立刻探活（原来调 forceProbe）——
     *   UI 写完 Token 紧接着就会发一条 status 动作，两个 hello 几乎同时
     *   到达米家设备会互相把包挤掉（实测丢包 ⇒ 误判离线）。
     *   改为延后 25 s 且优先探这一台：既尽快拿到结论，又避开撞车。
     */
    g_probeTurn = isFeeder ? 0 : 1;
    g_nextProbeMs = millis() + 25000;

    Serial.printf("[XiaomiLAN] provision ok dev=%s token_set=1 ip=nvs_never\n", dev);
    return true;
}

bool xiaomi_lan_configured(bool feeder)
{
    return feeder ? g_feeder.configured : g_plug.configured;
}

bool xiaomi_lan_enabled()
{
    return devEnabled(true) || devEnabled(false);
}

/* =====================================================
 * ★ [P0-2 · 2026-10-31] 门禁事实（纯内存读，零网络 I/O）
 * ===================================================== */

bool xiaomi_lan_dev_enabled(bool feeder)
{
    return devEnabled(feeder);
}

bool xiaomi_lan_dev_configured(bool feeder)
{
    return feeder ? g_feeder.configured : g_plug.configured;
}

bool xiaomi_lan_dev_online(bool feeder)
{
    return state_get_bool(feeder ? STATE_XIAOMI_FEEDER_ONLINE : STATE_XIAOMI_PLUG_ONLINE);
}

uint32_t xiaomi_lan_dev_last_ok_age_ms(bool feeder)
{
    const unsigned long at = g_lastOkMs[feeder ? 0 : 1];
    if (at == 0) return UINT32_MAX;      /* 本次启动以来从未成功 */
    return (uint32_t)(millis() - at);
}

bool xiaomi_lan_dev_state_fresh(bool feeder, uint32_t freshMs)
{
    const uint32_t age = xiaomi_lan_dev_last_ok_age_ms(feeder);
    if (age == UINT32_MAX) return false;
    return age <= freshMs;
}

void xiaomi_lan_request_probe(bool feeder)
{
    g_probeNow[feeder ? 0 : 1] = true;
    /* 把下一次探活提前到"现在"，但**不**破坏 60 s 周期基数：
       periodicProbe() 在真正发出请求时才重算 g_nextProbeMs。 */
    g_nextProbeMs = millis();
}

void xiaomi_lan_init()
{
    if (g_inited) return;
    g_inited = true;

    memset(g_req, 0, sizeof(g_req));   /* ★ [P4] 两个槽一起清 */
    g_failStreak[0] = 0;
    g_failStreak[1] = 0;
    g_feeder.devId = 0; g_feeder.stamp = 0; g_feeder.configured = false;
    g_plug.devId = 0;   g_plug.stamp = 0;   g_plug.configured = false;

    /* ★★ [2026-11-04] IP 缓存是纯 RAM：每次启动都从空开始，首次操作前重新验证 */
    xland::resetDiscovery(g_disc);
    xland::resetCache(g_ipCache);
    g_collectDeadline = 0;
    g_discRetryAt = 0;
    g_lastLocalIp = 0;
    g_lastWifiConnected = false;
    g_lastDevId[0] = 0;
    g_lastDevId[1] = 0;
    g_devReady[0] = false;
    g_devReady[1] = false;

    /* ★ 只读 Token；NVS 里旧版遗留的 IP 键既不读也不删（兼容回滚） */
    loadDev(g_feeder, "f_tok");
    loadDev(g_plug, "p_tok");

    state_set_bool(STATE_XIAOMI_FEEDER_ENABLE, devEnabled(true));
    state_set_bool(STATE_XIAOMI_PLUG_ENABLE, devEnabled(false));
    state_set_bool(STATE_XIAOMI_FEEDER_ONLINE, false);
    state_set_bool(STATE_XIAOMI_PLUG_ONLINE, false);
    state_set_int(STATE_XIAOMI_FEEDER_OUT_STATUS, -1);
    state_set_bool(STATE_XIAOMI_PLUG_POWER, false);
    state_set_int(STATE_XIAOMI_LAN_LAST_RESULT, 0);

    /* ★ [2026-10-29] 新增诊断状态初值：0 = 从未查询（不是"失败"） */
    state_set_int(STATE_XIAOMI_FEEDER_LAST_ERR, (int)XIAOMI_ERR_NONE);
    state_set_int(STATE_XIAOMI_PLUG_LAST_ERR, (int)XIAOMI_ERR_NONE);
    state_set_long(STATE_XIAOMI_FEEDER_LAST_OK_TS, 0L);
    state_set_long(STATE_XIAOMI_PLUG_LAST_OK_TS, 0L);
    state_set_int(STATE_XIAOMI_LAN_RECV, 0);
    state_set_int(STATE_XIAOMI_LAN_BADSUM, 0);
    state_set_int(STATE_XIAOMI_LAN_TIMEOUT_CNT, 0);
    state_set_int(STATE_XIAOMI_PLUG_AUTOOFF_SEC, 0);

    /*
     * ★ [2026-10-30 · A2] 把 NVS 读到的"是否已配置"如实发布出去。
     *   只发布**布尔**，不发布 IP/Token 的任何片段（凭据永不出状态表）。
     *   配置页据此显示「已配置 / 未配置」，重启后仍为 true 即证明已落盘。
     */
    state_set_bool(STATE_XIAOMI_FEEDER_CONFIGURED, g_feeder.configured);
    state_set_bool(STATE_XIAOMI_PLUG_CONFIGURED, g_plug.configured);
    Serial.printf("[XiaomiLAN] creds feeder=%d plug=%d\n",
                  g_feeder.configured ? 1 : 0, g_plug.configured ? 1 : 0);

    /* ★ 恢复"限时通电"的持久化截止时刻（掉电/重启后仍然会到点断电） */
    g_autoOffEpoch = 0;
    {
        Preferences p;
        if (p.begin(kNvsNs, true)) { g_autoOffEpoch = p.getUInt("p_off_at", 0); p.end(); }
        time_t now = time(nullptr);
        if (g_autoOffEpoch == 0) state_set_int(STATE_XIAOMI_PLUG_AUTOOFF_SEC, 0);
        else if (now > 0)
        {
            int32_t left = (int32_t)g_autoOffEpoch - (int32_t)now;
            state_set_int(STATE_XIAOMI_PLUG_AUTOOFF_SEC, left > 0 ? (int)left : 0);
        }
    }

    bool ok1 = workflow_register_action(&feedDesc);
    bool ok2 = workflow_register_action(&plugOnDesc);
    bool ok3 = workflow_register_action(&plugOffDesc);
    bool ok4 = workflow_register_action(&plugOn60Desc);
    bool ok5 = workflow_register_action(&feederStatusDesc);
    bool ok6 = workflow_register_action(&plugStatusDesc);

    Serial.printf("[XiaomiLAN] init: feeder_cfg=%d plug_cfg=%d enable=%d/%d actions=%d%d%d%d%d%d autooff=%u\n",
                  g_feeder.configured ? 1 : 0, g_plug.configured ? 1 : 0,
                  devEnabled(true) ? 1 : 0, devEnabled(false) ? 1 : 0,
                  ok1 ? 1 : 0, ok2 ? 1 : 0, ok3 ? 1 : 0,
                  ok4 ? 1 : 0, ok5 ? 1 : 0, ok6 ? 1 : 0,
                  (unsigned)g_autoOffEpoch);
    // ★ 无任何 token / ip 打印（脱敏：只回 boolean）
}

void xiaomi_lan_loop()
{
    if (!g_inited) return;

    // 配置开关变化时同步到 state（UI 只看 state，不必每次查 NVS）
    static bool lastF = false, lastP = false;
    bool f = devEnabled(true);
    bool p = devEnabled(false);
    if (f != lastF) { lastF = f; state_set_bool(STATE_XIAOMI_FEEDER_ENABLE, f); }
    if (p != lastP) { lastP = p; state_set_bool(STATE_XIAOMI_PLUG_ENABLE, p); }

    if (!f && !p) return;

    /*
     * ★★ [2026-11-04] 网络状态变化 ⇒ RAM IP 缓存整体失效。
     *   WiFi 断开 / 重连 / 本机 DHCP 换址 / 切 AP 都会让上次认证过的 IP
     *   不再可信；Token 仍在 NVS，不受影响。
     */
    const bool connected = (WiFi.status() == WL_CONNECTED);
    const uint32_t localIp = connected ? (uint32_t)WiFi.localIP() : 0;
    if (xland::networkInvalidates(g_lastWifiConnected, connected, g_lastLocalIp, localIp))
    {
        /* invalidateIpCache() 内部已清空缓存 / 在途验证 / 候选 / device_id 基准，
           并把 phase 收回 Idle（不伪造"正在发现"）。 */
        invalidateIpCache();
        g_discRetryAt = 0;                  // 立即允许重新发现

        /*
         * ★★ [2026-11-05] 网络变化 ⇒ 重置 stage0 的**补发现额度与预算**。
         *   为什么必须：上电瞬间发起的探活会一直排在 stage0 等 WiFi；
         *   旧逻辑下它的 20 s 预算从"上电那一刻"开始算，WiFi 若 11 s 才连上，
         *   剩下不到 9 s（扣掉 3 s 保留）就不够再跑一轮发现 ⇒ 首次探活必然失败。
         *   网络状态变了，等于"重新开始"，所以额度与截止时间都按现在重算。
         */
        for (uint8_t i = 0; i < 2; i++)
        {
            Request &r = g_req[i];
            if (r.active && r.stage == 0)
            {
                r.discKick = 0;
                r.deadline = millis() + kDiscRequestBudgetMs;
            }
        }
    }
    g_lastWifiConnected = connected;
    g_lastLocalIp = localIp;

    if (!connected)
    {
        if (g_udpReady) { g_udp.stop(); g_udpReady = false; }
        return;
    }
    if (!g_udpReady)
    {
        if (g_udp.begin(0) == 1) g_udpReady = true;
        else return;
    }

    /*
     * ★★ [2026-11-05] 发现**只由操作驱动**（`startRequest` → `beginOperationDiscovery`），
     *   这里不再做"后台预热"：缓存不再是跳过发现的理由，也就不需要提前把 IP 备好。
     *   若发现层已结束而仍有 stage0 请求在等，由 `advanceSlot()` 的**有界补发现**
     *   （每请求最多 2 次）处理，避免设备离线时刷成广播风暴。
     */

    /* ★ 安全定时器优先：到点必须发断电，不能被探活饿死 */
    serviceAutoOff();

    /*
     * ★ [P4 · 2026-11-01] 两台设备**各自**推进（并行），不再二选一。
     * ★ [2026-11-04] 发现层与请求槽共用一个 UDP socket ⇒ 只要有任一方在跑就推进。
     */
    advanceRequest();

    /*
     * ★★ [2026-11-05 二期] 延时确认读：到点 + 槽空 ⇒ 发一条**只读**刷新，
     *   让 System State 收敛到设备事实（不重发控制命令，不改写状态值）。
     *   12 s 内一直被拒（结果未领取/总线忙）就放弃，绝不无限重试。
     */
    for (uint8_t i = 0; i < 2; i++)
    {
        if (g_confirmAt[i] == 0) continue;
        if ((int32_t)(millis() - g_confirmAt[i]) < 0) continue;
        const bool fd = (i == 0);
        if (devBusy(fd)) continue;
        DevInfo &dv = fd ? g_feeder : g_plug;
        if (!devEnabled(fd) || !dv.configured) { g_confirmAt[i] = 0; continue; }
        if (startRequest(fd ? REQ_READ_FEEDER : REQ_READ_PLUG, 0, dv, fd, nullptr))
        {
            g_confirmAt[i] = 0;
            Serial.printf("[XiaomiLAN] confirm read dev=%s fired\n", fd ? "feeder" : "plug");
        }
        else if ((uint32_t)(millis() - g_confirmAt[i]) > 12000)
        {
            g_confirmAt[i] = 0;
            Serial.printf("[XiaomiLAN] confirm read dev=%s gave up (busy)\n", fd ? "feeder" : "plug");
        }
    }

    periodicProbe();
}
