# -*- coding: utf-8 -*-
"""RTC 探测性启用验证探针（COM8，单会话：drain → 一次 RTS 脉冲 → 抓 boot → 分类）。

目的：不改硬件，只把 `rtc.enable` 置 true，用 boot 日志一锤定音板上有没有 PCF8563T。

判定（源：src/services/time_manager.cpp rtc_init() / time_init()）：
  · `[Time] PCF8563T detected at 0x51 (shared OLED I2C)`   ⇒ 芯片在（PROBE_OK）
  · `[Time] RTC probe failed (addr=0x51 err=2), ...`       ⇒ 芯片不在（NACK）
  · `[Time] RTC disabled by config, skip`                  ⇒ 配置没生效（enable 仍 false）
  · `[Time] RTC boot restore OK: <时间>`                   ⇒ 芯片在且时间有效（硬同步成功）
  · `[Time] RTC present but time invalid`                  ⇒ 芯片在但时间无效（等 SNTP 校准）

纪律（pio-esp32-windows-workflow §6.2）：
  · setDTR(False)/setRTS(False) 后才 open()；本环境开串口仍会复位（CH343）
  ⇒ 先 drain 排空已开始的 boot，再主动脉冲 RTS 一次，全程只把最后一次 boot 当判据
  · 断言用 boot 日志行，不看退出码

用法：
  python rtc_probe.py COM8 75
"""
import sys, time, re

PORT = sys.argv[1] if len(sys.argv) > 1 else "COM8"
SECS = int(sys.argv[2]) if len(sys.argv) > 2 else 75
BAUD = 115200

import serial  # pyserial（PlatformIO venv 已装 3.5）

# (说明, 正则) —— 用于"抓到就打印"，不参与 PASS/FAIL 判定
INTERESTING = [
    ("RTC 探测成功（芯片在）", re.compile(r"PCF8563T detected at 0x([0-9A-Fa-f]{2})")),
    ("RTC 探测失败（芯片不在）", re.compile(r"RTC probe failed \(addr=0x([0-9A-Fa-f]{2}) err=(\d+)\)")),
    ("RTC 被配置禁用", re.compile(r"RTC disabled by config")),
    ("RTC boot 硬同步成功", re.compile(r"RTC boot restore OK:\s*(.+?)\s*$", re.M)),
    ("RTC 在但时间无效", re.compile(r"RTC present but time invalid")),
    ("RTC 初始化失败（等待 SNTP）", re.compile(r"RTC init failed \(wait SNTP\)")),
    ("WiFi 已连", re.compile(r"WiFi connected", re.I)),
    ("MQTT 已连", re.compile(r"MQTT connected", re.I)),
    ("boot 完成（无 panic）", re.compile(r"\[CFG\]\[I\] boot validated")),
]
FORBID = [
    ("栈溢出", re.compile(r"stack overflow|Stack canary", re.I)),
    ("panic/assert", re.compile(r"Guru Meditation|assert failed|abort\(\)", re.I)),
    ("看门狗重启", re.compile(r"watchdog|rst:.*tg\d", re.I)),
]


def main():
    # ★★ 关键修正（2026-10-22 实测）：**不再** drain + RTS 脉冲。
    # 原流程 = 开串口复位#1（CH343 固有）→ 3 s 后 RTS 脉冲复位#2；
    # 复位#1 的 boot 在 3 s 内走不完 setup ⇒ boot flag 未写入 ⇒ 复位#2 时
    # `!last_boot_ok` ⇒ config_manager 从 Backup 恢复，**覆盖 Active 配置**
    # （实测：rtc.enable 与 mqtt.telemetry_enable 双双丢失）。
    # 修正后：只利用 open() 自带的那一次复位，直接抓这段 boot，全程只复位 1 次。
    s = serial.Serial(PORT, BAUD, timeout=0.2)
    s.setDTR(False)
    s.setRTS(False)
    print(">>> PORT-OPENED %s (port=%s, window=%ds, 单次复位不复脉冲)"
          % (time.strftime("%H:%M:%S"), PORT, SECS))
    # 观测窗口（open() 触发的复位立即开始 boot，从这一刻起抓）
    buf = b""
    t0 = time.time()
    while time.time() - t0 < SECS:
        n = s.read(4096)
        if n:
            buf += n
    s.close()

    text = buf.decode("utf-8", "replace")

    print("\n>>> 关键行（RTC 相关 + 连接状态）")
    for name, rx in INTERESTING:
        for m in rx.finditer(text):
            print("    [%s] %s" % (name, m.group(0).strip()))

    print("\n>>> 判定")
    if re.search(r"PCF8563T detected", text):
        verdict = "CHIP_PRESENT"
    elif re.search(r"RTC probe failed", text):
        verdict = "CHIP_ABSENT"
    elif re.search(r"RTC disabled by config", text):
        verdict = "CONFIG_NOT_APPLIED"
    else:
        verdict = "UNKNOWN"

    print("    RTC 探测结论 = %s" % verdict)
    if verdict == "CHIP_PRESENT":
        if re.search(r"RTC boot restore OK", text):
            print("    → 芯片在且时间有效：已硬同步 System Time（可消 NC-5 的 BOOT_RESTORE）")
        elif re.search(r"RTC present but time invalid", text):
            print("    → 芯片在但时间无效（VL/未初始化）：等首次 SNTP 校准即可（CALIBRATED）")
    elif verdict == "CHIP_ABSENT":
        print("    → 板上无 PCF8563T（与 NC-5 一致）。建议：焊芯片后重跑本探针；")
        print("      若暂不焊，应把 rtc.enable 改回 false 避免每次 boot 都打 WARN。")
    elif verdict == "CONFIG_NOT_APPLIED":
        print("    → rtc.enable 仍为 false：config_set/save 未生效，需复核设备实值。")

    for name, rx in FORBID:
        m = rx.search(text)
        if m:
            print("    [异常命中] %s : %s" % (name, m.group(0).strip()))
    print(">>> RESULT: %s" % verdict)
    sys.exit(0 if verdict in ("CHIP_PRESENT", "CHIP_ABSENT") else 1)


if __name__ == "__main__":
    main()
