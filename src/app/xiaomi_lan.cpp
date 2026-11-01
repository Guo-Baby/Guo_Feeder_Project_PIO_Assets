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
struct DevInfo
{
    IPAddress ip;
    uint8_t token[16];
    uint32_t devId;      // 握手得到的设备号（0 = 未知）
    uint32_t stamp;      // 握手得到的时间戳
    bool configured;
};

DevInfo g_feeder;
DevInfo g_plug;

bool g_inited = false;
bool g_udpReady = false;
WiFiUDP g_udp;

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
bool sendHello(DevInfo &dev)
{
    uint8_t hello[32];
    hello[0] = 0x21; hello[1] = 0x31; hello[2] = 0x00; hello[3] = 0x20;
    for (int i = 4; i < 32; i++) hello[i] = 0xFF;   // Python: "21310020" + "ff"*28

    if (!g_udp.beginPacket(dev.ip, kPort)) return false;
    g_udp.write(hello, sizeof(hello));
    g_statSend++;
    return g_udp.endPacket() == 1;
}

bool sendEncrypted(DevInfo &dev, const char *json)
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

    if (!g_udp.beginPacket(dev.ip, kPort)) return false;
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
    rq.stage = 1;
    rq.kind = kind;
    rq.portions = portions;
    rq.dev = &dev;
    rq.owner = owner;
    rq.fin = false;
    rq.ok = false;
    rq.gotPacket = false;
    rq.badsum0 = g_statBadChecksum;
    rq.deadline = millis() + kHelloTimeoutMs;

    if (!sendHello(dev))
    {
        finishRequest(feeder, false);
        markErr(feeder, XIAOMI_ERR_TIMEOUT);
        return false;
    }
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
                if (sendEncrypted(*g_req.dev, payload))
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
 *   （g_feeder.ip / g_plug.ip 来自 NVS 配置，来源 IP 必须与之一致。）
 *
 * ★ 若来源 IP 两边都不匹配（陌生设备/迟到包）⇒ 丢弃（不喂给任何槽），
 *   但**仍然计入** g_statRecv（那确实是收到了包）。
 */
void advanceRequest()
{
    const bool feederBusy = devBusy(true);
    const bool plugBusy = devBusy(false);
    if (!feederBusy && !plugBusy) return;

    uint8_t buf[kMaxFrame];
    int len = 0;
    IPAddress from;

    if (g_udp.parsePacket() > 0)
    {
        from = g_udp.remoteIP();
        len = g_udp.read(buf, sizeof(buf));
        if (len > 0) g_statRecv++;
    }

    /* ---- 分派：把包交给来源设备对应的槽（最多一个） ---- */
    int feederLen = 0;
    int plugLen = 0;
    if (len > 0)
    {
        if (feederBusy && from == g_feeder.ip)
        {
            feederLen = len;
        }
        else if (plugBusy && from == g_plug.ip)
        {
            plugLen = len;
        }
        else
        {
            /*
             * 来源不明（陌生设备 / 迟到包 / 槽已结束）：
             *   · 计入 g_statRecv（确实收到了包）；
             *   · **不喂给任何槽** —— 喂错槽会造成串台误判。
             */
        }
    }

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
// NVS 凭证
// =====================================================
bool loadDev(DevInfo &dev, const char *ipKey, const char *tokKey)
{
    Preferences p;
    if (!p.begin(kNvsNs, true)) return false;

    String ipStr = p.getString(ipKey, "");
    String tokStr = p.getString(tokKey, "");
    p.end();

    bool ok = false;
    if (ipStr.length() > 0 && tokStr.length() == 32)
    {
        IPAddress ip;
        if (ip.fromString(ipStr))
        {
            uint8_t tok[16];
            if (parseToken(tokStr.c_str(), tok))
            {
                dev.ip = ip;
                memcpy(dev.token, tok, 16);
                dev.configured = true;
                ok = true;
            }
            zeroBytes(tok, sizeof(tok));
        }
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
bool xiaomi_lan_provision(const char *dev, const char *ip, const char *token, const char **reason)
{
    if (reason) *reason = "";
    if (dev == nullptr) { if (reason) *reason = "missing_dev"; return false; }

    const bool isFeeder = (strcmp(dev, "feeder") == 0);
    const bool isPlug = (strcmp(dev, "plug") == 0);
    if (!isFeeder && !isPlug) { if (reason) *reason = "unknown_dev"; return false; }

    if (ip != nullptr && ip[0] != 0)
    {
        IPAddress tmp;
        if (!tmp.fromString(ip)) { if (reason) *reason = "bad_ip"; return false; }
    }
    if (token != nullptr && token[0] != 0)
    {
        uint8_t tmp[16];
        if (!parseToken(token, tmp)) { if (reason) *reason = "bad_token"; zeroBytes(tmp, sizeof(tmp)); return false; }
        zeroBytes(tmp, sizeof(tmp));
    }
    if ((ip == nullptr || ip[0] == 0) && (token == nullptr || token[0] == 0))
    {
        if (reason) *reason = "nothing_to_write";
        return false;
    }

    Preferences p;
    if (!p.begin(kNvsNs, false)) { if (reason) *reason = "nvs_open_failed"; return false; }
    bool ok = true;
    if (ip != nullptr && ip[0] != 0) ok = ok && p.putString(isFeeder ? "f_ip" : "p_ip", ip) > 0;
    if (token != nullptr && token[0] != 0) ok = ok && p.putString(isFeeder ? "f_tok" : "p_tok", token) > 0;
    p.end();

    if (!ok) { if (reason) *reason = "nvs_write_failed"; return false; }

    // 立即生效（无需重启）
    if (isFeeder) loadDev(g_feeder, "f_ip", "f_tok");
    else loadDev(g_plug, "p_ip", "p_tok");

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

    Serial.printf("[XiaomiLAN] provision ok dev=%s ip_set=%d token_set=%d\n",
                  dev, (ip && ip[0]) ? 1 : 0, (token && token[0]) ? 1 : 0);
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

    loadDev(g_feeder, "f_ip", "f_tok");
    loadDev(g_plug, "p_ip", "p_tok");

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

    if (WiFi.status() != WL_CONNECTED)
    {
        if (g_udpReady) { g_udp.stop(); g_udpReady = false; }
        return;
    }
    if (!g_udpReady)
    {
        if (g_udp.begin(0) == 1) g_udpReady = true;
        else return;
    }

    /* ★ 安全定时器优先：到点必须发断电，不能被探活饿死 */
    serviceAutoOff();

    /*
     * ★ [P4 · 2026-11-01] 两台设备**各自**推进（并行），不再二选一。
     *   旧实现 `if (g_req.active) advanceRequest(); else periodicProbe();`
     *   意味着：一台在飞时，另一台既不推进、也不做周期探活。
     */
    if (devBusy(true) || devBusy(false)) advanceRequest();
    if (!devBusy(true) && !devBusy(false)) periodicProbe();
    else periodicProbe();
}
