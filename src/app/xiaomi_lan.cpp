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
 * ★★ [2026-11-04] 动态发现时序（**IP 只在 RAM，绝不写 NVS**）。
 *
 * 实测依据（本机 LAN，纯 hello / get_properties 只读）：
 *   · 一次广播 hello 即可同时收到两台目标设备响应（3/3 轮）；
 *   · 广播响应自带 device_id / stamp，可直接用于后续加密认证查询
 *     （两台设备均实测成功）⇒ 每个候选认证只需**一次**往返；
 *   · 同网段共 4 台 miIO 设备响应 ⇒ 必须按各自 Token 认证归属，不按响应顺序。
 *
 * 流程 = 广播(2 包) → 收集窗口 → 逐 (设备 × 候选) 一次认证查询。
 * 不做 254 地址扫描，不引入大数组，不阻塞主循环。
 */
/*
 * ★★ [2026-11-05] 每次独立操作都要重跑一遍这个流程 ⇒ 单轮耗时直接影响
 *   用户体感与安全定时器（插座到点断电）。因此把「固定等满」改成「静默即收尾」：
 *     · 收集窗口：最多 `kDiscCollectMs`，但**连续 `kDiscQuietMs` 没有新候选就提前结束**；
 *     · 单候选认证回包：`kDiscReplyMs`（局域网实测 20–100 ms，700 ms 已很宽）。
 *   实测值见报告（广播 → 收集 → 认证 → 总耗时）。
 */
constexpr uint32_t kDiscCollectMs = 1200;   // 广播响应收集窗口（上限）
constexpr uint32_t kDiscQuietMs = 400;      // 收集期"无新候选"静默阈值（提前收尾）
constexpr uint32_t kDiscReplyMs = 700;      // 单个认证查询的回包等待
constexpr uint32_t kDiscRetryMs = 15000;    // 整轮失败后的静默期（防风暴）
constexpr uint32_t kDiscRequestBudgetMs = 20000; // 单条命令等待发现完成的上界
constexpr uint8_t  kDiscBroadcasts = 2;

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
uint32_t g_discDeadline = 0;
uint32_t g_discRetryAt = 0;
IPAddress g_discProbeIp;
uint32_t g_lastLocalIp = 0;
bool g_lastWifiConnected = false;

/*
 * ★★ [2026-11-05] 本**轮**发现覆盖的设备位掩码。
 *
 * 为什么要记在 `Discovery` 之外：
 *   · 轮次开始后，另一台设备的操作可能中途加入（`xland::beginOperation`），
 *     它的 pending 位会被 `nextProbe` 的游标跳过（curDev 已经走过去）；
 *   · 轮末必须能区分「本轮没覆盖到这台」和「覆盖到了但认证失败」，
 *     前者要**立刻补一轮**，后者按失败如实返回。
 */
uint8_t g_roundMask = 0;
/* 本轮**真正发过认证查询**的设备位（用于区分「没试过」与「试过但失败」） */
uint8_t g_roundProbedMask = 0;

/* 发现耗时统计（只读、纯数字，不含凭据） */
uint32_t g_discRoundStartMs = 0;
uint32_t g_discProbeStartMs = 0;
uint32_t g_discLastCandMs = 0;
uint32_t g_discLastCollectMs = 0;
uint32_t g_discLastProbeMs = 0;
uint32_t g_discLastTotalMs = 0;
uint16_t g_discRounds = 0;
uint16_t g_discRoundFail = 0;

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

/** 开启一轮广播发现（非阻塞）。只纳入「有 Token 且当前无有效 RAM IP」的设备。 */
void startDiscoveryRound()
{
    /*
     * ★★ [2026-11-05 · 真机 boot 实测修复] 没有网络时**绝不伪造"正在发现"**。
     *
     * 现象（刷写后串口实拍）：上电时 `periodicProbe()` 立刻发起一次探活
     *   ⇒ `beginOperationDiscovery()` 把 phase 置成 Collecting，但此时 WiFi
     *   还没连上 ⇒ 本函数提前返回、既没广播也没设 deadline
     *   ⇒ 下一轮 `advanceDiscovery()` 拿**过期 deadline** 立刻判"超时"
     *   ⇒ 连打 3 次 `discovery round=... total=0ms collect=0ms`（空转），
     *   把 stage0 的补发现额度烧光，等 WiFi 真连上后反而不再发现。
     *
     * 修法：不能在无网络时开轮 ⇒ 明确回到 Idle，把"还没开始"和"正在跑"分开；
     *   stage0 的补发现也会在无网络时**不计数**（见 advanceSlot）。
     */
    if (WiFi.status() != WL_CONNECTED || !g_udpReady)
    {
        g_disc.phase = xland::PhaseIdle;
        return;
    }

    xland::resetDiscovery(g_disc);
    if (g_feeder.configured && !xland::cacheUsable(g_ipCache, xland::kDevFeeder, true))
        xland::requestDiscovery(g_disc, xland::kDevFeeder);
    if (g_plug.configured && !xland::cacheUsable(g_ipCache, xland::kDevPlug, true))
        xland::requestDiscovery(g_disc, xland::kDevPlug);
    if (g_disc.pending == 0)
    {
        g_disc.phase = xland::PhaseDone;
        g_roundMask = 0;
        return;
    }

    g_roundMask = g_disc.pending;
    g_roundProbedMask = 0;
    g_disc.phase = xland::PhaseCollecting;
    g_discRoundStartMs = millis();
    g_discRounds++;
    const IPAddress bc = subnetBroadcast();
    for (uint8_t i = 0; i < kDiscBroadcasts; i++)
    {
        if (!sendHelloTo(bc)) break;
    }
    g_discLastCandMs = millis();
    g_discDeadline = millis() + kDiscCollectMs;
}

/**
 * ★★ [2026-11-05 · 任务书 §2.1/§2.2] 一次独立操作开始前的**强制**发现入口。
 *
 * 无条件作废该设备的 RAM IP —— **缓存不是跳过发现的理由**。
 * 同一操作内部的 hello / 加密帧 / 重试属于"同一次操作"，不再重复广播。
 *
 * ★★ 真机踩坑（刷写后串口实拍，必须保留这段说明）：
 *   旧写法先调 `xland::beginOperation()` 再判断 phase —— 而该函数在
 *   Idle/Done 时会**自己把 phase 置成 Collecting** ⇒ 判断永远命中"在途"
 *   分支 ⇒ `startDiscoveryRound()` **一次都没被调用**。
 *   表现：`discovery round=0 ... total=0ms collect=0ms` 反复出现，
 *   请求在 stage0 等到超时，UI 报 `last_err=2`（无响应）——
 *   而设备其实完全正常。
 *   ⇒ **`inFlight` 必须在任何改动 phase 的调用之前算好**。
 */
void beginOperationDiscovery(unsigned char devIdx)
{
    const unsigned char mask = xland::devMask(devIdx);
    const bool inFlight = (g_disc.phase == xland::PhaseCollecting ||
                           g_disc.phase == xland::PhaseProbing);

    xland::beginOperation(g_ipCache, g_disc, devIdx);

    DevInfo &d = (devIdx == xland::kDevFeeder) ? g_feeder : g_plug;
    d.devId = 0;
    d.stamp = 0;

    if (inFlight)
    {
        /* 在途轮次：并入本轮（保留另一台已收集到的候选，不打断） */
        g_roundMask |= mask;
        return;
    }

    g_discRetryAt = 0;      // 操作优先：不受上一轮失败后的静默期影响
    startDiscoveryRound();  // 内部按「已清缓存」重新组 pending
    g_roundMask |= mask;
}

/**
 * 本轮发现结束的**唯一**出口（所有 PhaseDone 都必须走这里）。
 *
 * 做两件事：
 *   ① 打印耗时（只读诊断，不含凭据）；
 *   ② 若有 stage0 请求仍在等且本次没拿到 IP ⇒ **立刻补一轮**，
 *      不被 `kDiscRetryMs` 静默期拖到请求超时。
 */
void endDiscoveryRound()
{
    g_disc.phase = xland::PhaseDone;
    g_discRetryAt = millis() + kDiscRetryMs;

    const uint32_t now = millis();
    g_discLastTotalMs = (g_discRoundStartMs != 0) ? (now - g_discRoundStartMs) : 0;
    if (g_discLastTotalMs > 0 && (g_ipCache.ip[0] == 0 && g_ipCache.ip[1] == 0))
        g_discRoundFail++;

    Serial.printf("[XiaomiLAN] discovery round=%u mask=%u probed=%u cand=%u total=%ums "
                  "collect=%ums probe=%ums ok=%d/%d\n",
                  (unsigned)g_discRounds, (unsigned)g_roundMask, (unsigned)g_roundProbedMask,
                  (unsigned)g_disc.candCount, (unsigned)g_discLastTotalMs,
                  (unsigned)g_discLastCollectMs, (unsigned)g_discLastProbeMs,
                  g_ipCache.valid[0] ? 1 : 0, g_ipCache.valid[1] ? 1 : 0);
}

/** 给当前 (设备 × 候选 IP) 组合发一条**只读**认证查询。 */
bool sendAuthProbe()
{
    unsigned char dev = 0;
    unsigned int ip = 0;
    if (!xland::nextProbe(g_disc, &dev, &ip)) return false;

    DevInfo &target = (dev == xland::kDevFeeder) ? g_feeder : g_plug;
    if (!target.configured) return false;

    g_roundProbedMask |= xland::devMask(dev);   // 记录"本轮确实试过这台"

    /*
     * 广播 hello 响应自带的 devId / stamp 可直接用于加密请求
     * （2026-11-04 本机实测：两台设备均一次通过，无需再单播握手）。
     */
    DevInfo probe;
    memcpy(probe.token, target.token, 16);
    probe.devId = g_disc.cand[g_disc.candIdx].devId;
    probe.stamp = g_disc.cand[g_disc.candIdx].stamp + 1;
    probe.configured = true;

    char payload[kMaxPayload];
    buildCommand(dev == xland::kDevFeeder ? REQ_READ_FEEDER : REQ_READ_PLUG,
                 0, payload, sizeof(payload));
    g_discProbeIp = IPAddress(ip);
    if (!sendEncryptedTo(probe, g_discProbeIp, payload)) return false;
    g_disc.probeStage = 1;
    g_discDeadline = millis() + kDiscReplyMs;
    return true;
}

/**
 * 认证回包判定：**四个条件同时满足**才算身份确认。
 *   parseReply 内部已做「校验和 = md5(header+token+ct)」⇒ 校验失败即 Token 不匹配。
 */
bool authReplyOk(bool feeder, uint8_t *buf, int len)
{
    DevInfo probe;
    DevInfo &target = feeder ? g_feeder : g_plug;
    memcpy(probe.token, target.token, 16);
    probe.devId = 0;
    probe.stamp = 0;
    probe.configured = true;
    Reply r;
    if (!parseReply(probe, buf, len, r) || !r.valid) return false;
    return (r.code == 0) && (r.propCode == 0) && r.hasValue;
}

/**
 * 发现状态机推进（每个 loop 一次）。
 * @param len 0 表示本轮没有属于发现层的包，仅推进超时。
 */
void advanceDiscovery(uint8_t *buf, int len, IPAddress from)
{
    if (g_disc.phase != xland::PhaseCollecting && g_disc.phase != xland::PhaseProbing) return;
    const uint32_t now = millis();

    if (g_disc.phase == xland::PhaseCollecting)
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
        /*
         * ★ [2026-11-05] 提前收尾：已经有候选且连续 kDiscQuietMs 没有新响应
         *   ⇒ 不再死等满 kDiscCollectMs（每次操作都要重跑，省下的是纯等待）。
         */
        const bool quiet = (g_disc.candCount > 0) &&
                           ((uint32_t)(now - g_discLastCandMs) >= kDiscQuietMs);
        if (quiet || (int32_t)(now - g_discDeadline) >= 0)
        {
            g_discLastCollectMs = (g_discRoundStartMs != 0) ? (now - g_discRoundStartMs) : 0;
            g_discProbeStartMs = now;
            if (g_disc.candCount == 0)
            {
                /* 没有任何 UDP 响应：如实结束，绝不把旧 IP 当有效地址 */
                g_discLastProbeMs = 0;
                endDiscoveryRound();
            }
            else
            {
                g_disc.phase = xland::PhaseProbing;
                g_disc.curDev = xland::kDevFeeder;
                g_disc.candIdx = 0;
                if (!sendAuthProbe())
                {
                    g_discLastProbeMs = millis() - g_discProbeStartMs;
                    endDiscoveryRound();
                }
            }
        }
        return;
    }

    /* ---- PhaseProbing：等当前候选的认证回包 ---- */
    if (g_disc.probeStage == 0)
    {
        if (!sendAuthProbe())
        {
            g_discLastProbeMs = (g_discProbeStartMs != 0) ? (now - g_discProbeStartMs) : 0;
            endDiscoveryRound();
        }
        return;
    }

    if (len > 32 && from == g_discProbeIp)
    {
        const bool ok = authReplyOk(g_disc.curDev == xland::kDevFeeder, buf, len);
        if (xland::claimOnAuth(g_disc, g_ipCache, ok))
        {
            /* 只更新「本设备」的会话参数，绝不写另一台 */
            DevInfo &target = (g_disc.curDev == xland::kDevFeeder) ? g_feeder : g_plug;
            target.devId = g_disc.cand[g_disc.candIdx].devId;
            target.stamp = g_disc.cand[g_disc.candIdx].stamp;
            g_disc.curDev++;
            g_disc.candIdx = 0;
        }
        else
        {
            xland::skipCandidate(g_disc);   // Token 不匹配 / 属性不支持 / 无 value
        }
    }
    else if ((int32_t)(now - g_discDeadline) < 0)
    {
        return;                              // 还没超时，继续等
    }
    else
    {
        xland::skipCandidate(g_disc);        // 超时：试下一个候选，不重发同一个
    }

    if (xland::discoveryFinished(g_disc) || !sendAuthProbe())
    {
        g_discLastProbeMs = (g_discProbeStartMs != 0) ? (millis() - g_discProbeStartMs) : 0;
        endDiscoveryRound();
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
     * ★★ [2026-11-05 · 任务书 §2.1] **每一次独立操作都必须重新发现**。
     *
     * 旧实现（2026-11-04）：`if (devIp()==0) 才去发现，否则直接用缓存 IP`。
     *   ⇒ 只要本次运行周期内成功过一次，后续所有操作都复用那个 IP；
     *     DHCP 换址后缓存"看起来仍然有效"（valid=true、非零、WiFi 也连着），
     *     请求就会打到旧地址/别的设备上，且没有任何入口能自愈。
     *
     * 现在：无条件进入 `stage 0`（等发现），由 `beginOperationDiscovery()`
     *   先作废该设备的 RAM IP，再发起/并入一轮广播 + Token 认证。
     *   `dev.configured` 只表示「NVS 里有合法 Token」，与 IP 是否已知无关。
     *
     * 时间预算：`kDiscRequestBudgetMs`(20 s) 覆盖「广播 + 收集 + 多候选认证」，
     *   发现成功后才发 hello（`kHelloTimeoutMs`）与命令帧（`kReplyTimeoutMs`）。
     *   超时如实失败，绝不自动重试出粮/通断这类非幂等物理动作。
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
        const IPAddress ip = devIp(isFeeder);
        if ((uint32_t)ip != 0)
        {
            g_req.stage = 1;
            g_req.deadline = now + kHelloTimeoutMs;
            if (!sendHelloTo(ip))
            {
                finishRequest(isFeeder, false);
                markErr(isFeeder, XIAOMI_ERR_TIMEOUT);
            }
            return;
        }

        /*
         * ★★ [2026-11-05] 有界补发现（最多 2 次）。
         *   条件：发现层已结束（Done/Idle）但本设备仍无 IP ⇒ 要么本轮被游标
         *   跳过、要么上一轮已失败。两种情况都值得再跑一轮，但**必须有上限**：
         *   设备真离线时绝不允许把 20 s 预算刷成十几轮广播。
         *   剩余预算不足 3 s 时不再补 —— 补了也来不及完成握手与命令帧。
         *
         *   ★ 无网络时**不计次、不补轮**（上电瞬间就在等 WiFi 的情况）：
         *     否则 3 次空转会把额度烧光，WiFi 连上后反而不发现（真机已踩到）。
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
            Serial.printf("[XiaomiLAN] stage0 re-discover dev=%s try=%u\n",
                          isFeeder ? "feeder" : "plug", (unsigned)g_req.discKick);
            beginOperationDiscovery(isFeeder ? xland::kDevFeeder : xland::kDevPlug);
            return;
        }

        if ((int32_t)(now - g_req.deadline) >= 0)
        {
            finishRequest(isFeeder, false);
            markErr(isFeeder, XIAOMI_ERR_TIMEOUT);
        }
        return;
    }

    if (g_req.stage == 1)
    {
        // 等握手：拿到 device_id / stamp 才能组后续密文帧
        if (len >= 32)
        {
            Reply r;
            if (parseReply(*g_req.dev, buf, len, r) && r.valid)
            {
                g_req.dev->devId = r.devId;
                g_req.dev->stamp = r.stamp;

                char payload[kMaxPayload];
                buildCommand(g_req.kind, g_req.portions, payload, sizeof(payload));
                if (sendEncryptedTo(*g_req.dev, devIp(isFeeder), payload))
                {
                    g_req.stage = 2;
                    g_req.deadline = now + kReplyTimeoutMs;
                    return;
                }
            }
            finishRequest(isFeeder, false);
            markErr(isFeeder, classifyParseFail(isFeeder));
            return;
        }
        if ((int32_t)(now - g_req.deadline) >= 0)
        {
            // ★ 不重试：超时即如实记超时，交给上层决定
            g_statTimeout++;
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
                    if (g_req.kind == REQ_PLUG_ON || g_req.kind == REQ_PLUG_OFF) forceProbe(false);
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
 *     ① 来自某台**已认领 IP** 且该槽在飞 ⇒ 交给该槽；
 *     ② 否则若发现层在跑 ⇒ 交给发现层（广播 hello 响应 / 认证回包）；
 *     ③ 都不匹配 ⇒ 丢弃（仍计入 g_statRecv）。
 *   发现层每轮都被调用一次：没有属于自己的包时传 len=0，仅推进超时。
 */
void advanceRequest()
{
    const bool feederBusy = devBusy(true);
    const bool plugBusy = devBusy(false);
    const bool discActive = (g_disc.phase == xland::PhaseCollecting ||
                             g_disc.phase == xland::PhaseProbing);
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
        if (feederBusy && (uint32_t)fIp != 0 && from == fIp)
        {
            feederLen = len;
        }
        else if (plugBusy && (uint32_t)pIp != 0 && from == pIp)
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

    /* ★★ [2026-11-04] IP 缓存是纯 RAM：每次启动都从空开始，首次操作前重新发现 */
    xland::resetDiscovery(g_disc);
    xland::resetCache(g_ipCache);
    g_discDeadline = 0;
    g_discRetryAt = 0;
    g_lastLocalIp = 0;
    g_lastWifiConnected = false;

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
        invalidateIpCache();
        g_discRetryAt = 0;                  // 立即允许重新发现
        g_disc.phase = xland::PhaseIdle;    // 打断在途发现（网络已变，旧候选作废）
        g_disc.candCount = 0;

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
    periodicProbe();
}
