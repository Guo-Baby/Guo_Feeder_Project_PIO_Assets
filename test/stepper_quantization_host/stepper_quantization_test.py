#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
步进电机 120° 出粮量化误差分配算法 —— 主机侧验证（纯数学，不涉及硬件）

镜像对象（固件侧同算法，逐行对应）：
    src/app/stepper.cpp  ::  stepper_ideal_pulses()
    src/app/stepper.cpp  ::  stepper_plan_dispense_pulses()

算法：
    ideal(n)   = floor(n * steps_per_rev * angle_deg / 360)      # 累计理想脉冲
    plan(N)    = ideal(N+1) - ideal(N)                            # 本次应发脉冲
    成功后：N += 1，issued_total = ideal(N)
    失败后：N 不变（本次不计入）

验证内容：
    T1  前 3 次分配序列 == [533, 533, 534]
    T2  每 3 次累计 == 1600 == 360°
    T3  累计误差恒 < 1 个脉冲（即 <= 0.225°），与执行次数无关
    T4  固定 533 的"错误做法"误差无界增长（对照，证明必须补偿）
    T5  人工调整（不进账本）不影响出粮账本
    T6  失败动作不增加成功计数（账本不被污染）
    T7  换参数（200 整步/圈、90°）依然保持同样的误差上界性质

运行： python test/stepper_quantization_host/stepper_quantization_test.py
"""

import sys

STEPS_PER_REV = 1600      # 1.8° 整步 => 200 整步/圈；1/8 微步 => 1600 有效上升沿/圈
ANGLE_DEG     = 120       # 单次标准出粮角度
DEG_PER_PULSE = 360.0 / STEPS_PER_REV


def ideal(n, steps_per_rev=STEPS_PER_REV, angle=ANGLE_DEG):
    """累计理想脉冲数（向下取整）。固件 stepper_ideal_pulses() 同式。"""
    return (n * steps_per_rev * angle) // 360


def plan(n_success, steps_per_rev=STEPS_PER_REV, angle=ANGLE_DEG):
    """下一次出粮应发脉冲数。固件 stepper_plan_dispense_pulses() 同式。"""
    return ideal(n_success + 1, steps_per_rev, angle) - ideal(n_success, steps_per_rev, angle)


RESULTS = []


def check(name, cond, detail=""):
    RESULTS.append((name, bool(cond), detail))
    print("[%s] %-52s %s" % ("PASS" if cond else "FAIL", name, detail))
    return bool(cond)


def main():
    print("=" * 78)
    print("步进电机出粮量化误差分配 —— 主机侧验证")
    print("  steps_per_rev=%d  angle=%d deg  => 每脉冲 %.4f deg"
          % (STEPS_PER_REV, ANGLE_DEG, DEG_PER_PULSE))
    print("  120° 理想脉冲 = %d * %d / 360 = %.6f"
          % (STEPS_PER_REV, ANGLE_DEG, STEPS_PER_REV * ANGLE_DEG / 360.0))
    print("=" * 78)

    # ---------- T1：前 3 次分配序列 ----------
    seq = [plan(i) for i in range(3)]
    check("T1 前 3 次分配序列 == [533,533,534]",
          seq == [533, 533, 534], "实际 %s" % seq)

    # ---------- T2：每 3 次累计 1600 == 360° ----------
    ok3 = True
    detail = []
    for base in range(0, 30, 3):
        # 从第 base 次成功之后开始，再执行 3 次
        n0 = base
        total = sum(plan(n0 + k) for k in range(3))
        ang = total * DEG_PER_PULSE
        detail.append("N=%d..%d:%d脉冲/%.4f°" % (n0, n0 + 3, total, ang))
        if total != STEPS_PER_REV:
            ok3 = False
    check("T2 任意连续 3 次累计 == %d 脉冲 == 360°" % STEPS_PER_REV,
          ok3, "; ".join(detail[:4]) + " ...")

    # ---------- T3：累计误差恒有界（< 1 脉冲） ----------
    max_err = 0.0
    for n in range(1, 100001):
        err = abs(n * STEPS_PER_REV * ANGLE_DEG / 360.0 - ideal(n))
        if err > max_err:
            max_err = err
    check("T3 1..100000 次累计误差 < 1 脉冲",
          max_err < 1.0,
          "最大 %.6f 脉冲 = %.4f°（上界 1 脉冲 = %.4f°）"
          % (max_err, max_err * DEG_PER_PULSE, DEG_PER_PULSE))

    # ---------- T4：对照 —— 固定 533 的错误做法 ----------
    err_fixed_100 = 100 * (STEPS_PER_REV * ANGLE_DEG / 360.0) - 100 * 533
    check("T4 固定 533 做法误差无界（100 次已偏 %.2f 脉冲）",
          err_fixed_100 > 30,
          "偏差 %.4f 脉冲 = %.3f°（本方案为 %.6f 脉冲）"
          % (err_fixed_100, err_fixed_100 * DEG_PER_PULSE, max_err))

    # ---------- T5：人工调整不进账本 ----------
    n_success = 0
    ledger = 0                      # issued_total
    manual_total = 0
    for _ in range(3):
        p = plan(n_success)
        ledger += p                 # 出粮：进账本
        n_success += 1
        manual_total += 20          # 人工调整 20 脉冲：不进账本
    check("T5 人工调整脉冲不计入出粮账本",
          ledger == ideal(3) and manual_total == 60,
          "账本=%d(== ideal(3)=%d) 人工累计=%d" % (ledger, ideal(3), manual_total))

    # ---------- T6：失败动作不更新账本 ----------
    n_a = 0
    plan_before = plan(n_a)
    # 模拟：本次执行失败 => N 不变，账本不变
    plan_after_fail = plan(n_a)
    n_a += 1
    plan_after_next = plan(n_a)
    check("T6 失败后账本不变（下一次仍为同一次序的脉冲数）",
          plan_before == plan_after_fail == 533 and plan_after_next == 533,
          "before=%d after_fail=%d next=%d" % (plan_before, plan_after_fail, plan_after_next))

    # ---------- T7：换参数仍保持误差上界 ----------
    ok7 = True
    for spr, ang in ((200, 90), (1600, 90), (3200, 120), (400, 120)):
        m = 0.0
        for n in range(1, 5001):
            e = abs(n * spr * ang / 360.0 - ideal(n, spr, ang))
            m = max(m, e)
        if m >= 1.0:
            ok7 = False
    check("T7 其它 (steps_per_rev, angle) 组合同样误差 < 1 脉冲", ok7,
          "已验证 (200,90) (1600,90) (3200,120) (400,120)")

    # ---------- 汇总 ----------
    passed = sum(1 for _, ok, _ in RESULTS if ok)
    total = len(RESULTS)
    print("=" * 78)
    print("结果：%d/%d PASS" % (passed, total))
    print("=" * 78)
    print()
    print("★ 边界说明（务必随报告一起阅读）：")
    print("  本算法只解决【整数 STEP 脉冲的数学量化误差】。")
    print("  它不能检测或纠正失步、机械滑动、传动间隙、断电后位置变化；")
    print("  软件不声称电机真实转过 120° 或拨片已到位（开环，无编码器）。")
    return 0 if passed == total else 1


if __name__ == "__main__":
    sys.exit(main())
