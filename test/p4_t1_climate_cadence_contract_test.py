#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""p4_t1_climate_cadence_contract_test.py —— T1 温湿度「重复时间点」接线守卫

为什么需要这条测试（2026-11-02 线上实测）
------------------------------------------
首页「历史曲线」出现"1 小时内多个同值点"。沿全链路取证：

  BLE 采样 → 固件遥测 → MQTT → Worker → D1 → API → 前端图表

D1 实测（read-only SELECT）：

  SELECT id, ts, type, value, seq FROM telemetry_history
   WHERE type='climate.temperature' ORDER BY ts DESC LIMIT 30;
  → 相邻两行 ts **严格相差 300 s**，value 全为 23.5，seq 逐条递增；
  → 总量：climate.temperature 1366 行 / climate.humidity 1353 行。

而 LYWSD03MMC 的实际读数 70–80 min 才变一次。

根因：`TP_CLIMATE_PERIOD_MS = 5 min` 的**抽样兜底**每 5 min 到期一次，
      设备把内存里的旧读数原样再发一次（信封 ts 是发布时刻、seq 递增，
      看起来像一条新遥测）⇒ 云端逐条落库 ⇒ 图表凭空多出同值点。
      这不是"不同采样恰好同值"，也不是云端重复写入或前端重复绘制：
      每条都是**一次独立的发布**，只是内容就是缓存值。

为什么只能在设备侧修：
  信封 v1 已冻结为 `{ts,type,value,seq,schema_version}`，**没有采样标识**；
  `mi_thermo_temp_ts` 是"最后一次成功解密广播帧的时刻"（每帧都更新），
  不是采样时间 ⇒ 云端无法在不使用"数值相同"这一被禁止的判据下区分
  「同一采样的重复传输」与「不同采样恰好同值」。
  ⇒ 修在**拥有真实变化信息**的那一侧：数值没变就不发。

本测试锁住「接线」，因为纯函数测试（test/telemetry_host）证明不了
发布器**有没有**用 0 这个周期 —— 那正是"两端单测全绿、线上功能全死"的经典缺口。

判据
----
  C-1 `TP_CLIMATE_CADENCE_MS` 必须存在且为 0（0 = 无时间兜底）
  C-2 kChannels 里 climate.temperature / climate.humidity 两行的周期参数
      必须是 `TP_CLIMATE_CADENCE_MS`
  C-3 weight.value 行必须**保持** `TP_WEIGHT_PERIOD_MS`（本轮不扩大改动）
  C-4 发布器必须走 `tp_should_emit(...)` 统一决策（不允许再手搓 delta/cadence 组合）
  C-5 **负向探针**：把 climate 行的周期换成 15 min 后，检查器必须报错
      （没有负向探针的测试不可证伪 —— 与 test/p4_wire_envelope_contract_test.py 同款约定）

用法：python test/p4_t1_climate_cadence_contract_test.py
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
POLICY = os.path.join(ROOT, "src", "services", "telemetry_policy.h")
PUB = os.path.join(ROOT, "src", "services", "telemetry_publisher.cpp")

passes = [0]
failures = []


def ok(name, cond, detail=""):
    if cond:
        passes[0] += 1
        print("  PASS  " + name)
    else:
        failures.append(name + (("  <- " + detail) if detail else ""))
        print("  FAIL  " + name + (("  <- " + detail) if detail else ""))


def read(p):
    with open(p, "rb") as f:
        return f.read().decode("utf-8", "replace").replace("\r\n", "\n")


def climate_cadence_ms(policy_src):
    """`TP_CLIMATE_CADENCE_MS` 的数值（解析失败 ⇒ None）。"""
    m = re.search(r"#define\s+TP_CLIMATE_CADENCE_MS\s+\(?\s*(\d+)[uUlL]*\s*\)?", policy_src)
    return int(m.group(1)) if m else None


def channel_rows(pub_src):
    """从 kChannels 初始化表里取出 (type, 周期表达式) 列表。"""
    at = pub_src.find("kChannels[]")
    if at < 0:
        return []
    seg = pub_src[at:at + 2000]
    rows = []
    for m in re.finditer(
        r'\{\s*STATE_[A-Z0-9_]+,\s*"([^"]+)",\s*&\w+,\s*&\w+,\s*'
        r'([A-Za-z0-9_]+)\s*,\s*([A-Za-z0-9_()\s\*]+?)\s*\}',
        seg,
    ):
        rows.append((m.group(1), m.group(2), m.group(3).strip()))
    return rows


def check(policy_src, pub_src):
    """返回 (是否全部满足, 明细列表)。C-5 的负向探针复用本函数。"""
    problems = []

    n = climate_cadence_ms(policy_src)
    if n != 0:
        problems.append("TP_CLIMATE_CADENCE_MS 不是 0（实际 %s）" % n)

    rows = dict((t, per) for (t, _thr, per) in channel_rows(pub_src))
    for t in ("climate.temperature", "climate.humidity"):
        if t not in rows:
            problems.append("kChannels 缺少通道 %s" % t)
        elif rows[t] != "TP_CLIMATE_CADENCE_MS":
            problems.append("%s 的周期是 %s（应为 TP_CLIMATE_CADENCE_MS）" % (t, rows[t]))

    if rows.get("weight.value") != "TP_WEIGHT_PERIOD_MS":
        problems.append("weight.value 的周期被改成了 %s（本轮不应改动 weight）"
                        % rows.get("weight.value"))

    if "tp_should_emit(" not in pub_src:
        problems.append("发布器未使用 tp_should_emit() 统一决策")

    return (len(problems) == 0), problems


def main():
    policy_src = read(POLICY)
    pub_src = read(PUB)

    print("[C-1] 策略常量：climate 关闭时间兜底")
    ok("C-1 TP_CLIMATE_CADENCE_MS == 0（无时间兜底）",
       climate_cadence_ms(policy_src) == 0,
       "实际 %s" % climate_cadence_ms(policy_src))

    print("\n[C-2] 接线：两条 climate 通道必须用该常量")
    rows = channel_rows(pub_src)
    ok("C-2a kChannels 可解析（3 条通道）", len(rows) == 3, "解析到 %d 条" % len(rows))
    by = dict((t, per) for (t, _thr, per) in rows)
    ok("C-2b climate.temperature 周期 = TP_CLIMATE_CADENCE_MS",
       by.get("climate.temperature") == "TP_CLIMATE_CADENCE_MS", str(by.get("climate.temperature")))
    ok("C-2c climate.humidity 周期 = TP_CLIMATE_CADENCE_MS",
       by.get("climate.humidity") == "TP_CLIMATE_CADENCE_MS", str(by.get("climate.humidity")))

    print("\n[C-3] 不扩大改动：weight 仍保留 15 min 兜底")
    ok("C-3 weight.value 周期 = TP_WEIGHT_PERIOD_MS",
       by.get("weight.value") == "TP_WEIGHT_PERIOD_MS", str(by.get("weight.value")))

    print("\n[C-4] 发布器走统一决策函数")
    ok("C-4 使用 tp_should_emit()", "tp_should_emit(" in pub_src)

    print("\n[C-5] 负向探针：把 climate 周期改回 15 min，检查器必须报错")
    broken = pub_src.replace(
        'TP_CLIMATE_DELTA_TEMP_C,    TP_CLIMATE_CADENCE_MS',
        'TP_CLIMATE_DELTA_TEMP_C,    TP_WEIGHT_PERIOD_MS',
    )
    ok("C-5a 探针确实改动了源文本", broken != pub_src)
    bad_ok, bad_problems = check(policy_src, broken)
    ok("C-5b 检查器能识别该回归",
       (bad_ok is False) and any("climate.temperature" in p for p in bad_problems),
       "problems=%s" % bad_problems)

    print("\n" + "=" * 66)
    print("PASS %d / FAIL %d" % (passes[0], len(failures)))
    if failures:
        for f in failures:
            print("  ! " + f)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
