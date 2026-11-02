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
    """从 kChannels 初始化表里取出 {type: [字段, ...]}（按逗号切分、去空白）。

    ★ [P4-T2 · 2026-11-02] 行从 6 字段扩到 **9** 字段（新增
      `sample_ts_key` / `last_sample_ts` / `sample_min_gap_ms`）。
      这里必须按"整行"解析，**不能**再假定"第 6 个字段就是周期"；
      旧版正则要求行尾紧跟 `}`，字段一多就直接解析到 0 行 ——
      而 0 行会让"某通道缺失"的判断**假绿**（见 C-2a 的数量断言兜底）。
    """
    at = pub_src.find("kChannels[]")
    if at < 0:
        return {}
    end = pub_src.find("};", at)
    seg = pub_src[at:(end if end > at else at + 3000)]
    rows = {}
    for m in re.finditer(r"\{([^{}]*)\}", seg):
        fields = [f.strip() for f in m.group(1).split(",")]
        if len(fields) < 7 or not fields[0].startswith("STATE_"):
            continue
        t = re.match(r'"([^"]+)"', fields[1])
        if t:
            rows[t.group(1)] = fields
    return rows


def check(policy_src, pub_src):
    """返回 (是否全部满足, 明细列表)。负向探针复用本函数。"""
    problems = []

    n = climate_cadence_ms(policy_src)
    if n != 0:
        problems.append("TP_CLIMATE_CADENCE_MS 不是 0（实际 %s）" % n)

    rows = channel_rows(pub_src)
    for t in ("climate.temperature", "climate.humidity"):
        if t not in rows:
            problems.append("kChannels 缺少通道 %s" % t)
        elif rows[t][5] != "TP_CLIMATE_CADENCE_MS":
            problems.append("%s 的周期是 %s（应为 TP_CLIMATE_CADENCE_MS）" % (t, rows[t][5]))

    if "weight.value" not in rows:
        problems.append("kChannels 缺少通道 weight.value")
    elif rows["weight.value"][5] != "TP_WEIGHT_PERIOD_MS":
        problems.append("weight.value 的周期被改成了 %s（本轮不应改动 weight）"
                        % rows["weight.value"][5])

    if "tp_should_emit(" not in pub_src:
        problems.append("发布器未使用 tp_should_emit() 统一决策")

    # C-6 NaN 基线放行（2026-11-02 线上事故根因）
    if "!tp_finite(last_v)" not in policy_src:
        problems.append("策略层未对 NAN 基线放行（tp_should_emit 缺少 !tp_finite(last_v) 分支）")

    # C-7 「真实新样本」通道接线
    for t, key in (("climate.temperature", "STATE_MI_THERMO_TEMP_TS"),
                   ("climate.humidity", "STATE_MI_THERMO_HUMID_TS")):
        if t not in rows:
            continue
        if rows[t][6] != key:
            problems.append("%s 的采样时刻键是 %s（应为 %s）" % (t, rows[t][6], key))
    if "weight.value" in rows and rows["weight.value"][6] != "TELE_NO_SAMPLE_TS":
        problems.append("weight.value 不该挂采样时刻键（实际 %s）" % rows["weight.value"][6])
    if "tp_sample_advanced(" not in pub_src:
        problems.append("发布器未实现「新样本」放行（缺少 tp_sample_advanced 调用）")

    # C-8 非有限读数不得上报
    if "!tp_finite(v)" not in pub_src:
        problems.append("发布器未跳过 NAN 读数（缺少 !tp_finite(v) 守卫）")

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
    ok("C-2b climate.temperature 周期 = TP_CLIMATE_CADENCE_MS",
       "climate.temperature" in rows and rows["climate.temperature"][5] == "TP_CLIMATE_CADENCE_MS",
       str(rows.get("climate.temperature", [None] * 7)[5]))
    ok("C-2c climate.humidity 周期 = TP_CLIMATE_CADENCE_MS",
       "climate.humidity" in rows and rows["climate.humidity"][5] == "TP_CLIMATE_CADENCE_MS",
       str(rows.get("climate.humidity", [None] * 7)[5]))

    print("\n[C-3] 不扩大改动：weight 仍保留 15 min 兜底")
    ok("C-3 weight.value 周期 = TP_WEIGHT_PERIOD_MS",
       "weight.value" in rows and rows["weight.value"][5] == "TP_WEIGHT_PERIOD_MS",
       str(rows.get("weight.value", [None] * 7)[5]))

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

    # ---------------------------------------------------------------
    # ★ [P4-T2 · 2026-11-02] 线上事故：「刷写 a910a60 后 climate 一条都不发」
    #
    #   根因链（D1 取证）：weight 的 seq 从 1 连号、climate 0 行 ⇒ 设备侧没发。
    #   设备侧原因：`MiThermometerInit()` 把 `mi_thermo_temp` 写成 `NAN`（valid=true），
    #   而 `telemetry_publisher_init()` 在其后 ⇒ 基线 = NaN ⇒ 与 NaN 的比较恒 false
    #   ⇒ cadence=0 之后 `tp_should_emit()` **再无任何放行路径**。
    #   这两组判据锁住"放行 NAN 基线"与"真实新样本必发"两条接线。
    # ---------------------------------------------------------------
    print("\n[C-6] NAN 基线不得静默（否则整条 channel 永久死）")
    ok("C-6a 策略层对非有限基线放行",
       "!tp_finite(last_v)" in policy_src)
    ok("C-6b tp_finite 自身不依赖 <cmath>（freestanding）",
       "v == v" in policy_src and "#include <cmath>" not in policy_src)

    print("\n[C-7] 「真实新样本」通道：采样时戳推进才发，且两条 climate 都接上")
    ok("C-7a climate.temperature 挂 STATE_MI_THERMO_TEMP_TS",
       "climate.temperature" in rows and rows["climate.temperature"][6] == "STATE_MI_THERMO_TEMP_TS",
       str(rows.get("climate.temperature", [None] * 7)[6]))
    ok("C-7b climate.humidity 挂 STATE_MI_THERMO_HUMID_TS",
       "climate.humidity" in rows and rows["climate.humidity"][6] == "STATE_MI_THERMO_HUMID_TS",
       str(rows.get("climate.humidity", [None] * 7)[6]))
    ok("C-7c weight 不挂采样时戳（HX711 连续采样，走周期兜底）",
       "weight.value" in rows and rows["weight.value"][6] == "TELE_NO_SAMPLE_TS",
       str(rows.get("weight.value", [None] * 7)[6]))
    ok("C-7d 发布器调用 tp_sample_advanced()", "tp_sample_advanced(" in pub_src)

    print("\n[C-8] 非有限读数不得上报（NAN 会被信封写成 null）")
    ok("C-8 发布器有 !tp_finite(v) 守卫", "!tp_finite(v)" in pub_src)

    print("\n[C-9] 负向探针 ②：删掉 NAN 放行分支，检查器必须报错")
    broken2 = policy_src.replace("!tp_finite(last_v)", "!tp_finite(0.0f)")
    ok("C-9a 探针确实改动了源文本", broken2 != policy_src)
    bad_ok2, bad_problems2 = check(broken2, pub_src)
    ok("C-9b 检查器能识别该回归",
       (bad_ok2 is False) and any("NAN" in p for p in bad_problems2),
       "problems=%s" % bad_problems2)

    print("\n[C-10] 负向探针 ③：摘掉 climate.temperature 的采样时戳键，检查器必须报错")
    broken3 = pub_src.replace(
        "STATE_MI_THERMO_TEMP_TS,  &s_temp_sample_ts",
        "TELE_NO_SAMPLE_TS,        nullptr          ",
    )
    ok("C-10a 探针确实改动了源文本", broken3 != pub_src)
    bad_ok3, bad_problems3 = check(policy_src, broken3)
    ok("C-10b 检查器能识别该回归",
       (bad_ok3 is False) and any("climate.temperature" in p for p in bad_problems3),
       "problems=%s" % bad_problems3)

    print("\n" + "=" * 66)
    print("PASS %d / FAIL %d" % (passes[0], len(failures)))
    if failures:
        for f in failures:
            print("  ! " + f)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
