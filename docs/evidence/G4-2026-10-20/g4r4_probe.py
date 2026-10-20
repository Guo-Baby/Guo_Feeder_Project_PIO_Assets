# -*- coding: utf-8 -*-
"""G4-R4 上板探针：一次开串口、按序发多条命令并采集响应。

为什么不用 serial_cmd.py 逐条调用：
  每次 open() 都可能脉冲 DTR/RTS 导致 MCU 复位（已知坑），
  而本轮要对比"烧录前 / 烧录后"的状态 ⇒ 必须**单会话**采集。

用法:
  python g4r4_probe.py <port> --tag pre  --cmds "<c1>" "<c2>" ...
  python g4r4_probe.py <port> --tag post --reset --cmds "<c1>" ...

  --reset : 打开后脉冲 EN（RTS）复位，用于抓启动日志
  --tag   : 只用于输出分隔标记
"""
import sys
import time
import serial


def parse_args(argv):
    port = argv[1]
    tag = "run"
    reset = False
    cmds = []
    i = 2
    while i < len(argv):
        a = argv[i]
        if a == "--tag":
            tag = argv[i + 1]
            i += 2
        elif a == "--reset":
            reset = True
            i += 1
        elif a == "--cmds":
            cmds = argv[i + 1:]
            break
        else:
            i += 1
    return port, tag, reset, cmds


def open_port(port):
    # 先设 dtr/rts 再 open，避免打开瞬间拉低 EN 造成复位
    ser = serial.Serial()
    ser.port = port
    ser.baudrate = 115200
    ser.timeout = 0.25
    ser.dtr = False
    ser.rts = False
    ser.open()
    return ser


def drain(ser, quiet_s=1.5, max_s=60.0):
    """读到连续 quiet_s 秒无数据为止（用于等 boot 完成 / 命令响应结束）"""
    t0 = time.time()
    last = time.time()
    out = []
    while True:
        d = ser.read(512)
        if d:
            out.append(d)
            last = time.time()
        else:
            if time.time() - last >= quiet_s:
                break
            if time.time() - t0 > max_s:
                break
    return b"".join(out)


def main():
    port, tag, reset, cmds = parse_args(sys.argv)
    ser = open_port(port)

    if reset:
        # 精确复位：RTS 拉低 → 保持 → 释放（CH343 上 RTS 接 EN）
        ser.dtr = False
        ser.rts = True
        time.sleep(0.15)
        ser.rts = False

    boot = drain(ser, quiet_s=2.5, max_s=90.0)
    sys.stdout.write("\n===== [%s] BOOT/IDLE OUTPUT (%d bytes) =====\n"
                     % (tag, len(boot)))
    sys.stdout.write(boot.decode("utf-8", "replace"))
    sys.stdout.flush()

    for idx, c in enumerate(cmds):
        ser.reset_input_buffer()
        ser.write((c + "\n").encode("utf-8"))
        sys.stdout.write("\n----- [%s] CMD[%d]: %s -----\n" % (tag, idx, c))
        sys.stdout.flush()
        resp = drain(ser, quiet_s=1.2, max_s=20.0)
        sys.stdout.write(resp.decode("utf-8", "replace"))
        sys.stdout.flush()

    ser.close()
    sys.stdout.write("\n===== [%s] DONE =====\n" % tag)


if __name__ == "__main__":
    main()
