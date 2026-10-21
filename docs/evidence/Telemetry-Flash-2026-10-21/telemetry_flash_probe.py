# -*- coding: utf-8 -*-
"""Telemetry 烧录后验证探针（单会话：脉冲复位 → 抓 boot → 断言）。

纪律（来自 pio-esp32-windows-workflow 技能）：
  · setDTR(False)/setRTS(False) 后才 open()；本环境开串口仍会复位（CH343）
  ⇒ 先 drain 排空可能已经开始的 boot，再主动脉冲 RTS 一次，全程只复位 1 次
  · 断言用 boot 日志行 + 命令回包；不看 upload 退出码

用法：
  python telemetry_flash_probe.py COM8 75
  （75 = 观测秒数；期望 60-75 s 足够覆盖 boot + MQTT 上线）
"""
import sys, time, re

PORT = sys.argv[1] if len(sys.argv) > 1 else "COM8"
SECS = int(sys.argv[2]) if len(sys.argv) > 2 else 75
BAUD = 115200

import serial  # pyserial（隔离 venv 已装）

EXPECT = [
    # (说明, 正则)
    ("boot 完成（无 panic/overflow）", re.compile(r"\[CFG\]\[I\] boot validated")),
    ("配置未回滚（mqtt 模块 loaded）", re.compile(r"init done, loaded \d+/8")),
    ("WiFi 已连", re.compile(r"WiFi connected", re.I)),
    ("MQTT 已连", re.compile(r"MQTT connected", re.I)),
]
FORBID = [
    ("栈溢出", re.compile(r"stack overflow|Stack canary", re.I)),
    ("panic/assert", re.compile(r"Guru Meditation|assert failed|abort\(\)", re.I)),
    ("看门狗重启", re.compile(r"watchdog|rst:.*tg\d", re.I)),
]


def main():
    # ★★ 修正（2026-10-22 实测）：**不再** drain 3 s + RTS 脉冲。
    # 旧流程 = 开串口复位#1（CH343 固有）→ 3 s 后 RTS 脉冲复位#2；
    # 复位#1 的 boot 3 s 内走不到 config_boot_validate() ⇒ boot flag 未写入
    # ⇒ 复位#2 时 config_manager 判定 !last_boot_ok **从 Backup 恢复**，
    # 覆盖 Active ⇒ 新落盘的配置键丢失（实测 rtc.enable / mqtt.telemetry_enable 双双回退）。
    # 烧录场景本来就只需要一次复位来脱离 bootloader —— 而 open() 自带这一次，够用。
    s = serial.Serial(PORT, BAUD, timeout=0.2)
    s.setDTR(False)
    s.setRTS(False)
    print(">>> PORT-OPENED %s (单次复位，不复脉冲)" % time.strftime("%H:%M:%S"))
    # 观测窗口（open() 触发的复位立即开始 boot，从这一刻起抓）
    buf = b""
    t0 = time.time()
    last_line = ""
    while time.time() - t0 < SECS:
        n = s.read(4096)
        if n:
            buf += n
            sys.stdout.buffer.write(n)
            sys.stdout.flush()
            if b"\n" in n:
                last_line = n.rsplit(b"\n", 1)[-1].decode("utf-8", "replace")
    s.close()

    text = buf.decode("utf-8", "replace")
    print("\n>>> EXPECT-CHECK")
    ok = True
    for name, rx in EXPECT:
        hit = rx.search(text) is not None
        ok = ok and hit
        print("    [%s] %s" % ("OK " if hit else "MISS", name))
    for name, rx in FORBID:
        hit = rx.search(text) is not None
        if hit:
            ok = False
        print("    [%s] %s" % ("HIT" if hit else "none", name))
    print(">>> RESULT:", "PASS" if ok else "FAIL")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
