# -*- coding: utf-8 -*-
"""通用串口批量测试：只复位一次 → 保留 boot 日志 → 顺序发命令。

用法:
    python serial_batch.py <port> <logfile> <cmds_file> [quiet_sec]

cmds_file：每行一条命令；空行与 # 开头行忽略。
每行可写成 "命令 ||| 期望"（可选），命中则记 [EXPECT-OK]。
期望串匹配规则：默认**子串**匹配；写成 /正则/ 则按 re.search 匹配
（用于"某计数非零"这类与加载历史无关的判据，见下方 RE_EXPECT 注释）。
quiet_sec：**未命中**时期望静默的秒数（默认 2.5）。
           用于等待同步阻塞型命令（如 logt flush 落盘 62 条约需 2.4s）。
命中期望串后只再等 HIT_GRACE 秒即返回 —— 大量即时
（flash/stats/emit）命令因此从 ~3.1s 降到 ~0.5s。

宏（可选）：
    <BOOT>  替换为**最近一次**从设备输出中捕获到的 boot 值
            （匹配 `boot=N` 或 `boot_seq=N`）。
            用途：`logt ack` 的 boot 参数必须精确匹配当前 boot_seq，而它跨会话
            不确定（取决于 meta 是否重建）。有了本宏，用例可以写成
                logt ack <BOOT> 1 4294967295 ||| acked=
            由运行器在发命令前代入，无需人工改动用例文件。
            若尚未捕获到 boot 值，该行记 `>>> SKIP` 并跳过（不算 MISS）。
"""
import re
import serial
import sys
import time
import datetime

PORT = sys.argv[1]
LOG = sys.argv[2]
CMDS = sys.argv[3]
QUIET = float(sys.argv[4]) if len(sys.argv) > 4 else 2.5
HIT_GRACE = float(sys.argv[5]) if len(sys.argv) > 5 else 0.35

logf = open(LOG, "w", encoding="utf-8", errors="replace")

# <BOOT> 宏：从设备输出里抓 `boot=N` / `boot_seq=N`（后写覆盖先写）
BOOT_RE = re.compile(r"boot(?:_seq)?=(\d+)")
LAST_BOOT = [None]


def note_boot(s):
    m = BOOT_RE.search(s)
    if m:
        LAST_BOOT[0] = m.group(1)


def emit(s):
    logf.write(s + "\n")
    logf.flush()


def ts():
    return datetime.datetime.now().strftime("%H:%M:%S.%f")[:-3]


# 期望串匹配
#
#   默认：**子串**匹配（历史行为，完全向后兼容）。
#   若含成对的 `/正则/` 片段 ⇒ 该片段按正则处理，其余部分按字面量处理。
#   例：`replay=/[1-9][0-9]*/`  ⇔  正则 `replay=[1-9][0-9]*`
#                              （字面 `replay=` + 正则 `[1-9][0-9]*`）
#
# 为什么需要：日志流里有一类计数**与加载历史相关**（补发积压条数、跨 boot 的
# 队列表项数…），把它们写成精确数值会让用例随"上一次运行遗留了什么"而时通时不通。
# 而真正要判别的往往只是"**该字段非零**"（例如补发确实发生过）⇒ 用
# `replay=/[1-9][0-9]*/` 表达，精确且与积压历史无关。
#
# ⚠️⚠️ 必须把**字段名写进字面量**（`replay=/…/` 而不是 `/[1-9][0-9]*/`）：
#    纯正则 `/[1-9][0-9]*/` 会在**整行**里 search，`offskip=1`、`ack_ok=2`
#    之类的其它数字会造成**假命中**（实测 `replay=0` 也被判 OK）。
def expect_hit(expect, line):
    if "/" not in expect:
        return expect in line

    parts = expect.split("/")
    if len(parts) % 2 == 0:
        # 斜杠不成对 ⇒ 不当作正则，退回子串（避免误伤含 "/" 的普通期望）
        return expect in line

    pat = "".join(
        re.escape(parts[i]) if i % 2 == 0 else parts[i]
        for i in range(len(parts))
    )
    try:
        if re.search(pat, line) is not None:
            return True
    except re.error:
        pass

    # ⚠️ 退回**子串匹配**（2026-10-02 修复）：
    #    `/re/` 语法与"含**字面斜杠**的普通期望"存在**固有歧义** ——
    #    `guo_feeder/<device_id>/down` 与 `replay=/[1-9][0-9]*/` 都是 2 个斜杠，
    #    split 后 parts 数同为奇数，**无法用奇偶区分**；而拼接 pat 时字面斜杠会**丢失**。
    #    ⇒ 若只走正则，含 "/" 的普通期望**必然 MISS**
    #      （实测：Topic V3 用例的 `guo_feeder/<device_id>/down` 全 MISS，
    #       而不含斜杠的 `dev_` / `internal` 却 OK —— 这就是真因）。
    #    因此：**正则优先**（保留 `/re/` 语义），不命中则按子串再判一次。
    return expect in line


cmds = []
with open(CMDS, "r", encoding="utf-8") as f:
    for ln in f:
        ln = ln.strip()
        if not ln or ln.startswith("#"):
            continue
        if "|||" in ln:
            c, exp = ln.split("|||", 1)
            cmds.append((c.strip(), exp.strip()))
        else:
            cmds.append((ln, None))

ser = serial.Serial(PORT, 115200, timeout=0.3)
ser.setDTR(False)
ser.rts = False

# ---- 采集 boot 日志直到连续 1.5s 静默 ----
emit("===== BOOT =====")
last = time.time()
buf = b""
while True:
    d = ser.read(256)
    if d:
        buf += d
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            line = line.replace(b"\r", b"")
            if line:
                s = line.decode("utf-8", "replace")
                note_boot(s)
                emit("[%s] %s" % (ts(), s))
        last = time.time()
    elif time.time() - last >= 1.5:
        break

for cmd, expect in cmds:
    if "<BOOT>" in cmd:
        if LAST_BOOT[0] is None:
            emit("===== CMD: %s =====" % cmd)
            emit("[%s] >>> SKIP : 尚未从设备输出捕获到 boot 值，无法代入 <BOOT>" % ts())
            continue
        cmd = cmd.replace("<BOOT>", LAST_BOOT[0])
    emit("===== CMD: %s =====" % cmd)
    ser.reset_input_buffer()
    ser.write((cmd + "\n").encode("utf-8"))
    time.sleep(0.15)
    buf = b""
    last = time.time()
    deadline = time.time() + 20.0
    hit = False
    while time.time() < deadline:
        d = ser.read(256)
        if d:
            buf += d
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                line = line.replace(b"\r", b"")
                if not line:
                    continue
                s = line.decode("utf-8", "replace")
                note_boot(s)
                emit("[%s] %s" % (ts(), s))
                if expect and expect_hit(expect, s):
                    hit = True
            last = time.time()
        else:
            # 命中期望串后响应已完整，只需短暂静默即可进入下一条
            # （部分命令如 logt flush 会同步阻塞数秒，必须等它真正返回）；
            # 未命中时仍按 QUIET 等待，避免把慢命令误判成 MISS。
            grace = HIT_GRACE if hit else QUIET
            if time.time() - last >= grace:
                break
    if expect:
        emit("[%s] >>> EXPECT %r : %s" % (ts(), expect, "OK" if hit else "MISS"))
    time.sleep(0.2)

emit("===== END =====")
ser.close()
logf.close()
print("done")
