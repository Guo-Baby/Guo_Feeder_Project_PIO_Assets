// =====================================================
// probe.cpp —— 米家 LAN 命令互斥决策宿主侧探针（wasm32 freestanding + node）
//
// 只编译 **纯头文件** `src/app/xiaomi_lan_slot.h`（零依赖），
// 因此这里执行的是固件里**同一份**决策代码，而不是复制品。
//
// 导出：
//   probe_run()       → 失败用例数（0 = 全通过）
//   probe_log_ptr()   → 日志缓冲区起始
//   probe_log_len()   → 日志字节数
//
// 用例覆盖（对应任务书 P0-2 / P0-4）：
//   1  通道空闲 ⇒ 一律允许
//   2  同设备忙 ⇒ 拒绝（保留同设备串行）
//   3  ★ 跨设备忙 ⇒ **不阻塞**（两台设备互不影响）
//   4  断电抢占只读查询 ⇒ 允许（紧急断电通路）
//   5  断电**不**抢占物理动作（出粮/通电）
//   6  只读查询不抢占任何东西
//   7  物理动作不抢占只读查询
//   8  断电不抢占另一台设备的槽
//   9  ReqKind 数值与固件一致
// =====================================================

#include "app/xiaomi_lan_slot.h"

namespace
{
char g_log[8192];
int  g_logLen = 0;
int  g_fail = 0;

void putc(char c)
{
    if (g_logLen < (int)sizeof(g_log) - 1) g_log[g_logLen++] = c;
}

void puts(const char *s)
{
    while (*s) putc(*s++);
}

void putInt(int v)
{
    char buf[12];
    int n = 0;
    if (v < 0) { putc('-'); v = -v; }
    if (v == 0) { putc('0'); return; }
    while (v > 0 && n < 11) { buf[n++] = (char)('0' + v % 10); v /= 10; }
    while (n > 0) putc(buf[--n]);
}

void check(bool cond, const char *name)
{
    puts(cond ? "PASS  " : "FAIL  ");
    puts(name);
    puts("\n");
    if (!cond) g_fail++;
}
} // namespace

extern "C" int probe_run()
{
    using namespace xlan;

    g_logLen = 0;
    g_fail = 0;

    // ---- 1 通道空闲 ⇒ 一律允许 ----
    check(decideSlot(false, REQ_NONE, REQ_FEED) == Allow,
          "1a idle slot + feed => Allow");
    check(decideSlot(false, REQ_NONE, REQ_PLUG_ON) == Allow,
          "1b idle slot + plug-on => Allow");
    check(decideSlot(false, REQ_NONE, REQ_PLUG_OFF) == Allow,
          "1c idle slot + plug-off => Allow");
    check(decideSlot(false, REQ_NONE, REQ_READ_FEEDER) == Allow,
          "1d idle slot + read-feeder => Allow");

    // ---- 2 同设备忙 ⇒ 拒绝（保留同设备串行） ----
    check(decideSlot(true, REQ_FEED, REQ_FEED) == RejectBusy,
          "2a same device busy (feed) + feed => RejectBusy");
    check(decideSlot(true, REQ_PLUG_ON, REQ_PLUG_OFF) == RejectBusy,
          "2b plug-on in flight + plug-off => RejectBusy (no preempt of physical action)");
    check(decideSlot(true, REQ_READ_FEEDER, REQ_FEED) == RejectBusy,
          "2c same device read in flight + feed => RejectBusy");
    check(decideSlot(true, REQ_READ_PLUG, REQ_READ_PLUG) == RejectBusy,
          "2d same device read in flight + read => RejectBusy");

    // ---- 3 ★ 跨设备互不阻塞 ----
    //   （跨设备在固件里由"两个独立槽"保证；这里锁死"决策只看自己的槽"这一语义）
    check(decideSlot(false, REQ_NONE, REQ_FEED) == Allow,
          "3a feeder slot free while plug busy => Allow (feeder not blocked)");
    check(decideSlot(false, REQ_NONE, REQ_PLUG_OFF) == Allow,
          "3b plug slot free while feeder busy => Allow (plug not blocked)");

    // ---- 4 断电抢占只读查询（紧急断电通路） ----
    check(decideSlot(true, REQ_READ_PLUG, REQ_PLUG_OFF) == PreemptRead,
          "4a plug read in flight + plug-off => PreemptRead");
    check(decideSlot(true, REQ_READ_FEEDER, REQ_PLUG_OFF) == PreemptRead,
          "4b feeder read in flight + plug-off => PreemptRead");

    // ---- 5 断电**不**抢占物理动作 ----
    check(decideSlot(true, REQ_PLUG_ON, REQ_PLUG_OFF) == RejectBusy,
          "5a plug-on in flight + plug-off => RejectBusy (never preempt physical)");
    check(decideSlot(true, REQ_FEED, REQ_PLUG_OFF) == RejectBusy,
          "5b feed in flight + plug-off => RejectBusy (never preempt physical)");
    check(decideSlot(true, REQ_PLUG_ON, REQ_FEED) == RejectBusy,
          "5c plug-on in flight + feed => RejectBusy");

    // ---- 6 只读查询不抢占任何东西 ----
    check(decideSlot(true, REQ_READ_PLUG, REQ_READ_PLUG) == RejectBusy,
          "6a read never preempts another read");
    check(decideSlot(true, REQ_READ_FEEDER, REQ_READ_PLUG) == RejectBusy,
          "6b read never preempts another read (cross device kind)");
    check(decideSlot(true, REQ_FEED, REQ_READ_FEEDER) == RejectBusy,
          "6c read never preempts a physical action");

    // ---- 7 物理动作不抢占只读查询 ----
    check(decideSlot(true, REQ_READ_FEEDER, REQ_FEED) == RejectBusy,
          "7a feed does not preempt in-flight read");
    check(decideSlot(true, REQ_READ_PLUG, REQ_PLUG_ON) == RejectBusy,
          "7b plug-on does not preempt in-flight read");

    // ---- 8 辅助判定函数 ----
    check(reqIsRead(REQ_READ_FEEDER) && reqIsRead(REQ_READ_PLUG),
          "8a read feeder/plug are read-only");
    check(!reqIsRead(REQ_FEED) && !reqIsRead(REQ_PLUG_ON) && !reqIsRead(REQ_PLUG_OFF),
          "8b feed/plug-on/plug-off are NOT read-only");
    check(reqIsPlugOff(REQ_PLUG_OFF), "8c REQ_PLUG_OFF is plug-off");
    check(!reqIsPlugOff(REQ_PLUG_ON) && !reqIsPlugOff(REQ_FEED),
          "8d plug-on/feed are not plug-off");

    // ---- 9 ReqKind 数值必须与固件 cpp 里的 enum 一致 ----
    check((int)REQ_NONE == 0 && (int)REQ_READ_FEEDER == 1 && (int)REQ_READ_PLUG == 2
          && (int)REQ_FEED == 3 && (int)REQ_PLUG_ON == 4 && (int)REQ_PLUG_OFF == 5,
          "9 ReqKind values match firmware enum");

    return g_fail;
}

extern "C" const char *probe_log_ptr() { return g_log; }
extern "C" int probe_log_len() { return g_logLen; }
