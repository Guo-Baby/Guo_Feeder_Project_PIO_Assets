#pragma once

// =====================================================
// xiaomi_lan_slot.h —— 米家 LAN 命令互斥决策（**纯函数 / 零依赖**）
//
// =====================================================
// 为什么单独抽出来：
//   P4 任务书 P0-2 的核心缺陷是「米家设备之间错误的命令互斥」。
//   根因有两处：
//     · 前端：一个全局 `busy` 标志 ⇒ 一台设备执行时两台都变灰；
//     · 固件：一个全局请求槽 `g_req`     ⇒ 一台设备在飞时另一台被拒。
//   固件侧的并发安全**无法靠真机验证**（要两台真设备同时在手边），
//   所以把「能不能发起 / 能不能抢占」这条决策抽成纯函数，
//   在宿主侧（test/xiaomi_lan_slot_host）真实执行，正反用例都可测。
//
// ★ 这里**只做决策**，不做任何 I/O：
//   决策结果由上层（xiaomi_lan.cpp）翻译成具体动作。
//
// =====================================================
// 语义边界（上层必须遵守）：
//   · `SlotDecision::Allow`        —— 可以发起；上层负责真正发包
//   · `SlotDecision::RejectBusy`   —— 该设备自己的通道被占（同设备串行）
//   · `SlotDecision::PreemptRead`  —— 抢占：中止该设备正在进行的**只读查询**
//                                     （只读无副作用，中止安全）
//   · `SlotDecision::RejectOther`  —— **别的设备**在忙 —— 但**不阻塞**本设备
//                                     （P0-2：跨设备必须互不阻塞）
//
// ★ 抢占规则（任务书 P0-4「紧急断电不能被普通命令长期锁定」）：
//   只有「断电」可以抢占「只读查询」。
//   断电**不抢占**物理动作（出粮 / 通电）——
//   半途打断物理动作可能留下不确定的机械/电气状态，风险更大。
// =====================================================

namespace xlan
{

/** 请求种类（与固件的 `ReqKind` 一一对应，数值必须一致）。 */
enum ReqKind : unsigned char
{
    REQ_NONE = 0,
    REQ_READ_FEEDER = 1,
    REQ_READ_PLUG = 2,
    REQ_FEED = 3,
    REQ_PLUG_ON = 4,
    REQ_PLUG_OFF = 5,
};

enum SlotDecision : unsigned char
{
    Allow = 0,        // 可以发起
    RejectBusy = 1,   // 同设备通道被占
    PreemptRead = 2,  // 抢占只读查询后可以发起
};

/** 该请求是不是**只读**（无副作用，可安全中止）。 */
inline bool reqIsRead(unsigned char kind)
{
    return kind == REQ_READ_FEEDER || kind == REQ_READ_PLUG;
}

/** 该请求是不是**插座断电**（安全方向，可抢占只读查询）。 */
inline bool reqIsPlugOff(unsigned char kind)
{
    return kind == REQ_PLUG_OFF;
}

/**
 * 判定"能不能发起这次请求"。
 *
 * @param slotBusy   该设备自己的通道是否被占（true = 有请求在飞）
 * @param slotKind   该设备当前在飞请求的种类（slotBusy 为 true 时才有意义）
 * @param wantKind   本次想发起的请求种类
 * @return 决策
 */
inline SlotDecision decideSlot(bool slotBusy, unsigned char slotKind, unsigned char wantKind)
{
    if (!slotBusy) return Allow;
    /*
     * ★ P0-4：断电抢占只读查询。
     *   · 只在「本次是插座断电」且「在飞的是只读查询」时才抢占；
     *   · 断电**不**抢占物理动作（出粮 / 通电）—— 半途打断更危险。
     */
    if (reqIsPlugOff(wantKind) && reqIsRead(slotKind)) return PreemptRead;
    return RejectBusy;
}

} // namespace xlan
