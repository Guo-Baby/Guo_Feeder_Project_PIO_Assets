#pragma once

#include <Arduino.h>

// =====================================================
// 米家 LAN 设备模块（喂食器 pet-food-out / 智能插座 power）
//
// 职责：
//   · MIoT 局域网协议（UDP 54321，AES-128-CBC + 0x2131 帧）——直连设备，不经云端
//   · Workflow Action 注册：XIAOMI_FEED / XIAOMI_PLUG_ON / XIAOMI_PLUG_OFF
//   · SystemState 维护：两台设备的 enable / online / 关键属性
//   · 周期性只读探活（60s）→ 维护 online 状态
//
// 边界（重要）：
//   · **不做自动重试**。出粮是非幂等动作，结果未知时重发 = 重复出粮。
//     超时只如实置 online=false + last_result=失败，绝不重发。
//   · Token 只存 NVS（namespace `gflan`），**不进 config JSON、不进日志**。
//     config 里只放 enable 开关（与其他模块一致）。
//   · 只读探活用 get_properties，不产生任何物理动作。
//
// 协议来源：MiLAN_Test/src/miot_lan.py（本机实测通过）
//   key = md5(token) / iv = md5(key + token) / PKCS7 / 明文尾 0x00
//   frame = [0x2131|len|0|device_id|stamp][md5(header+token+ct)][ct]
// =====================================================

void xiaomi_lan_init();

// 每个 loop 调用一次（非阻塞状态机）
void xiaomi_lan_loop();

// 是否至少一台设备被配置启用
bool xiaomi_lan_enabled();

// =====================================================
// ★ [P0-2 · 2026-10-31] 单台设备的**门禁事实**（供 Workflow 整份预检使用）
//
// 为什么必须由本模块导出，而不是让 workflow.cpp 自己去读 state / config：
//   · "是否启用"的**权威来源**是 ConfigManager 内存里的 `feeder_enable/plug_enable`
//     （`config_init()` 已补齐声明默认值）。System State 里的
//     `xiaomi_*_enable` 是**派生值**，只在 loop 里变化时才刷新 ⇒
//     开机早期它可能还是旧值，用它做门禁会出现
//     "配置页显示启用、Workflow 却说模块停用"的假拒绝。
//   · "是否在线 / 状态是否新鲜"只有本模块知道（它维护连败计数与最近成功时刻）。
//
// ★ 这些函数**不做任何网络 I/O**，纯读内存 ⇒ 可以在 pre-flight 里安全调用。
// =====================================================

/** 该设备模块是否启用（读 ConfigManager 内存值，缺省 true） */
bool xiaomi_lan_dev_enabled(bool feeder);

/**
 * ★ [P0-2 · 2026-10-31] 在线结论的**新鲜度上限**（毫秒）。
 *
 * 与前端 `stateFresh()` 的 120 s 必须一致：两边不同会出现
 * "前端认为新鲜、固件判过期" ⇒ 用户点了被拒却看不出原因。
 * 定义在此处（唯一定义源），workflow pre-flight 直接引用。
 */
#define XIAOMI_STATE_FRESH_MS 120000UL

/** 该设备是否已配置 IP + Token（不含任何凭据内容） */
bool xiaomi_lan_dev_configured(bool feeder);

/** 最近一次通信结论是否在线（false 也可能是"从未成功"） */
bool xiaomi_lan_dev_online(bool feeder);

/**
 * 距最近一次**通信成功**过去了多少毫秒。
 * @return `UINT32_MAX` 表示「自本次启动以来从未成功」
 */
uint32_t xiaomi_lan_dev_last_ok_age_ms(bool feeder);

/**
 * 状态是否"新鲜"：最近一次成功在 `freshMs` 以内。
 * ★ 注意与"在线"是两个不同问题：
 *   在线 = 最近结论；新鲜 = 这个结论有多旧（结论会过期，过期就必须重查）。
 */
bool xiaomi_lan_dev_state_fresh(bool feeder, uint32_t freshMs);

/**
 * 请求尽快做一次**只读**探活（非阻塞，只置标志）。
 *
 * 用途：pre-flight 发现某台设备状态过期/未知时，**先踢一次探活再拒绝本次执行**，
 *   让用户重试时能拿到新结论；绝不在 pre-flight 里阻塞等待回包。
 */
void xiaomi_lan_request_probe(bool feeder);

/**
 * [T11 · 2026-11-01] 作废**在途的非安全请求**（安全停堆 / 重启前必须调用）。
 *
 * 为什么必须有这个接口（真机故障）：
 *   米家设备是**外部设备**，命令一旦通过 UDP 发出去就收不回。
 *   一次请求分两阶段（先握手 800 ms，再发命令等回包 3000 ms）。
 *   强制重启只等 300 ms ⇒ 若此时正有一条「插座通电」排在握手阶段，
 *   它会在重启倒计时窗口内**真的发出去**，插座就一直通电了。
 *   旧 `safety_stop_all()` 只管本地执行器（阀/步进/振动），完全没碰米家 ⇒
 *   "强制重启不得继续消费普通命令"在米家这条链路上是**不成立的**。
 *
 * 语义：
 *   · **只作废非安全类**请求（出粮 / 通电 / 只读查询）；
 *   · **不动** `XIAOMI_PLUG_OFF`：断电是安全方向，重启窗口内也应尽量送出去；
 *   · 作废 = 直接 finishRequest(false) + 动作回调置 FAILED，
 *     不让它继续占据 slot，也不触发任何自动重试。
 *
 * @param reason 只用于串口日志，可为 nullptr
 * @return 实际被作废的请求数（0 / 1 / 2）
 */
uint8_t xiaomi_lan_abort_noncritical(const char *reason);

// =====================================================
// 本地配凭证（NVS）—— ★ [2026-11-04] IP **不再是配置项**
//
// dev   : "feeder" | "plug"
// token : 32 位 hex（必填，写入 NVS `gflan` 的 f_tok / p_tok）
//
// 设备 IP 由 ESP32 在家庭 LAN 内经 miIO 广播发现 + Token 认证获得，
// **只存 RAM**（重启 / WiFi 重连 / 换址后自动重新发现），绝不写入 NVS。
// 旧版本 NVS 里遗留的 f_ip / p_ip 既不读也不删，运行时不依赖它们。
//
// 返回 false 时 reason 指向静态错误串（不含任何凭据值）。
// ★ 刻意不接受 MQTT 回显：写入后只回 ok/错误，不返回值本身。
// =====================================================
bool xiaomi_lan_provision(const char *dev, const char *token, const char **reason);

// 是否已配置（NVS 中存在合法 Token）。不返回任何凭证内容，与 IP 无关。
bool xiaomi_lan_configured(bool feeder);

// =====================================================
// 失败原因编码（写入 SystemState `xiaomi_<dev>_last_err`）
//
// ★ 存在的唯一理由：`*_online=false` 把 6 种完全不同的情况压成一个布尔，
//   UI 只能显示"离线"，用户无法判断该去改 Token、改 IP，还是等一会再点。
//   **0 不等于失败**，它表示"从启动到现在还没有查询过"。
// =====================================================
enum XiaomiErr : uint8_t
{
    XIAOMI_ERR_NONE = 0,        // 尚未查询（不是故障）
    XIAOMI_ERR_OK = 1,          // 最近一次成功
    XIAOMI_ERR_TIMEOUT = 2,     // 局域网无响应（握手/回包超时）
    XIAOMI_ERR_AUTH = 3,        // 收到回包但校验和/解密失败 ⇒ 多半 Token 不对
    XIAOMI_ERR_NO_CFG = 4,      // 未配置 IP/Token
    XIAOMI_ERR_DISABLED = 5,    // 模块被暂停用（enabled=false，不会发起请求）
    XIAOMI_ERR_BUSY = 6,        // 上一条请求还在进行，本次未发出
    /*
     * 通信成功（能解密、校验和通过）但**属性层失败**：
     * 设备回了包、也认了命令，但 result 里的 code/propCode 非 0，
     * 或该属性没有返回 value（型号不支持这个 siid/piid）。
     * 此时设备是**在线**的，只是这个属性读不到 —— 与"离线"必须分开。
     */
    XIAOMI_ERR_ATTR = 7,
    /*
     * ★ [2026-11-05 二期] 身份冲突：认证阶段出现**多个**候选都能用本设备 Token
     * 通过（或同一 IP 被两台设备同时认领）。这种情况下**不允许随便挑一个**：
     * 清空该设备缓存、本轮放弃，下次操作从零重新发现。
     * 正常家庭网络不会出现，属于防串台/防伪造的兜底。
     */
    XIAOMI_ERR_CONFLICT = 8,
};
