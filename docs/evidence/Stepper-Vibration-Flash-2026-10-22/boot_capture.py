#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
boot_capture.py —— 单次复位抓 boot 日志（只读观测，不发送任何命令）

★ 纪律（CFG-ROLLBACK 铁律）：
    打开 CH343 串口本身就会让板子复位一次。
    【绝对不要】再额外脉冲 RTS —— "连续两次复位"会让第一次 boot 走不到
    config_boot_validate()，第二次 boot 判定 !last_boot_ok ⇒ 从 Backup 恢复、
    覆盖 Active ⇒ 配置（rtc.enable / mqtt.telemetry_enable 等）被悄悄回退。
    故本脚本【只利用 open() 自带的那一次复位】。

用法: python boot_capture.py <PORT> <SECONDS> [outfile]
"""
import sys
import time
import serial

PORT = sys.argv[1] if len(sys.argv) > 1 else "COM8"
SECS = int(sys.argv[2]) if len(sys.argv) > 2 else 60
OUT = sys.argv[3] if len(sys.argv) > 3 else None
BAUD = 115200

KEYWORDS = ("[CFG]", "[Stepper]", "[Vibration]", "panic", "Guru",
            "Stack canary", "assert failed", "rst:", "boot:")


def main():
    print(">>> OPEN %s (single reset, no RTS pulse) %s"
          % (PORT, time.strftime("%H:%M:%S")))
    s = serial.Serial(PORT, BAUD, timeout=0.2)
    print(">>> CAPTURE %ds" % SECS)
    buf = b""
    t0 = time.time()
    while time.time() - t0 < SECS:
        try:
            data = s.read(4096)
        except Exception:
            break
        if data:
            buf += data
    s.close()

    text = buf.decode("utf-8", errors="replace")
    if OUT:
        with open(OUT, "w", encoding="utf-8") as f:
            f.write(text)
        print(">>> saved full log ->", OUT)

    print("=" * 70)
    print("关键行（[CFG] / [Stepper] / [Vibration] / 异常）:")
    print("=" * 70)
    hit = 0
    for line in text.splitlines():
        if any(k in line for k in KEYWORDS):
            print("  " + line.rstrip())
            hit += 1
    if hit == 0:
        print("  （未命中任何关键行 —— 可能抓到的只是 boot 之后的稳态日志）")

    bad = [l for l in text.splitlines()
           if "Guru Meditation" in l or "panic" in l.lower()
           or "Stack canary" in l or "assert failed" in l]
    print("=" * 70)
    print("异常行数: %d" % len(bad))
    for l in bad[:10]:
        print("  !! " + l.rstrip())
    print("=" * 70)


if __name__ == "__main__":
    main()
