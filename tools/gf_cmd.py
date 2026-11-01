#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
gf_cmd.py —— Guo Feeder 串口命令工具（带**异步结果关联**）

为什么需要它
------------
固件的 `system/config_query` 是**异步**命令：

    发送 query  ->  立刻回 {"status":"accepted","command_id":"<id>"}
                ->  过一会儿才回 {"cmd":"result","id":"<id>",
                                  "type":"config_result","status":"success",
                                  "data":{...}}

旧工具只用「串口静默 N 秒」判断结束，结果是：
    · 静默期一到就返回  =>  值没等到  =>  报成 NaN / ts=0 / 未知
    · 这会把「等待逻辑有缺陷」误判成「设备侧没有数据」

本工具改为按 **command_id 关联**：发命令时带 `id`，收到 accepted 记下
`command_id`，随后只认 `cmd=="result"` 且 `id==command_id`（或 `command_id`
字段等于我们发的 id）的那一帧。

安全约束
--------
· 只发用户显式指定的命令；默认不发任何写操作、不发动作命令。
· **脱敏**：MAC 只显示后 4 位；BLE key / Token 只报「是否为空 + 长度」。
· 全部串口原文落盘，但打印到终端/报告的一律是摘要。

用法
----
    python tools/gf_cmd.py query mi_thermo              # 查整个模块（等待终态）
    python tools/gf_cmd.py query mi_thermo allow_collect
    python tools/gf_cmd.py state mi_thermo_valid        # 读 system state
    python tools/gf_cmd.py restart                      # 重启设备（需 --yes）
    python tools/gf_cmd.py watch 120                    # 只监控 N 秒
"""
import argparse
import json
import os
import sys
import threading
import time

import serial

PORT = "COM8"
BAUD = 115200

# 凭据类键名（命中后只报长度，绝不上报内容）
SECRET_KEYS = (
    "key", "token", "bindkey", "blekey", "secret", "password", "passwd",
)


def _is_secret_key(k):
    kl = str(k).lower()
    return any(s in kl for s in SECRET_KEYS)


def mask_value(k, v):
    """脱敏：MAC 留后 4 位；凭据只报存在性与长度。"""
    kl = str(k).lower()
    if v is None:
        return "(null)"
    if _is_secret_key(kl):
        s = str(v)
        return "<set len=%d>" % len(s) if s else "<empty>"
    if "mac" in kl:
        s = str(v)
        return ("***" + s[-4:]) if len(s) > 4 else "<empty>"
    if isinstance(v, float):
        if v != v:  # NaN
            return "<NaN>"
        return "%.2f" % v
    return v


def summarize(obj, prefix=""):
    """把任意 JSON 结构压成单行脱敏摘要（只保留必要信息）。"""
    if isinstance(obj, dict):
        parts = []
        for k, v in obj.items():
            m = mask_value(k, v)
            if isinstance(v, (dict, list)):
                parts.append("%s=%s" % (k, summarize(v)))
            else:
                parts.append("%s=%s" % (k, m))
        return "{" + ", ".join(parts) + "}"
    if isinstance(obj, list):
        if len(obj) > 3:
            return "[%d items: %s, ...]" % (len(obj), summarize(obj[0]))
        return "[" + ", ".join(summarize(x) for x in obj) + "]"
    return str(obj)


class Monitor:
    """后台线程持续读串口，写入带时间戳的日志文件。"""

    def __init__(self, port=PORT, baud=BAUD, log_path=None):
        self.ser = serial.Serial(port, baud, timeout=0.2)
        self.ser.setDTR(False)
        self.ser.rts = False
        self.log_path = log_path
        self._fh = open(log_path, "w", encoding="utf-8") if log_path else None
        self.buf = []
        self.lock = threading.Lock()
        self.stop = threading.Event()
        self.marks = []
        self.th = threading.Thread(target=self._loop, daemon=True)
        self.th.start()

    def _loop(self):
        """
        ★ [2026-10-29 修复] 旧版遇到任何读异常直接 `break` ⇒ 读取线程**静默退出**，
        而主线程继续等，表现为"心跳还在跳、日志却不再增长"（本轮实测踩到：
        16:40 后串口日志停更 17 分钟，差点被误判成"设备没动作"）。
        现在：瞬时错误（USB 抖动/驱动重枚举）只记计数并重试，只有 stop 才退出。
        """
        while not self.stop.is_set():
            try:
                d = self.ser.read(4096)
                self.read_errors = getattr(self, "read_errors", 0)
            except Exception as e:
                self.read_errors = getattr(self, "read_errors", 0) + 1
                self.last_error = str(e)[:120]
                if self.stop.is_set():
                    break
                time.sleep(0.5)
                continue
            self.rx_bytes = getattr(self, "rx_bytes", 0) + len(d or b"")
            if d:
                txt = d.decode("utf-8", "ignore")
                with self.lock:
                    self.buf.append(txt)
                if self._fh:
                    ts = time.strftime("%H:%M:%S")
                    self._fh.write("[%s] %s" % (ts, txt))
                    self._fh.flush()
            else:
                time.sleep(0.02)

    def mark(self, note):
        """在日志里打一条标记，用于区分重启前/后。"""
        line = "\n=== MARK [%s] %s ===\n" % (time.strftime("%H:%M:%S"), note)
        with self.lock:
            self.buf.append(line)
        if self._fh:
            self._fh.write(line)
            self._fh.flush()

    def snapshot(self):
        with self.lock:
            return "".join(self.buf)

    def since(self, n):
        return self.snapshot()[n:]

    def size(self):
        return len(self.snapshot())

    def close(self):
        self.stop.set()
        self.th.join(timeout=2.0)
        try:
            self.ser.close()
        except Exception:
            pass
        if self._fh:
            self._fh.close()


def send(mon, obj):
    """
    先写一个空行，再写命令。

    为什么：ESP32 侧 `serial_cmd_buffer` 是逐字符累积的，只有遇到 \\n / \\r
    才解析。若上一次会话/上一条命令在缓冲区里留下半个残行，本次命令会被
    拼在残行后面 ⇒ 整行不再是 "cm {...}" ⇒ 设备打印 "unknown command"。
    先写一个 \\n 可以让设备先把残行 flush 掉（空 buffer 时直接 continue，
    没有任何副作用），从而保证本次命令独占一行。
    """
    mon.ser.write(b"\n")
    time.sleep(0.12)
    mon.ser.write(("cm " + json.dumps(obj) + "\n").encode())


def wait_result(mon, cmd_id, timeout=12.0):
    """
    ★★ 异步命令结果的**终态**等待（B1 修复点）

    固件对 `system/config_query` 会先后吐出多条同 id 的 result 帧：

        ① {"cmd":"result","id":"<id>","status":"accepted",        ...}  ← 已受理
        ② {"cmd":"result","id":"<id>","status":"success","data":..} ← 真结果

    旧逻辑「收到第一条 cmd==result 就返回」⇒ 停在 ① 上，
    `status=="accepted"` != success ⇒ 被判成 failed，
    ⇒ 表现为「配置查询永远失败 / 值永远是 NaN」。

    现在只有 `success` / `failed` / `error` 才算终态；
    `accepted` / `running` / `pending` 一律继续等。

    返回 (state, payload, raw_line)
      state: "done" | "failed" | "timeout"
    """
    NON_TERMINAL = ("accepted", "running", "pending", "queued")
    end = time.time() + timeout
    start_len = mon.size()
    while time.time() < end:
        new = mon.since(start_len)
        # ★ 日志里混着 \r，先归一化，否则一行被覆盖后 JSON 会被截断
        for ln in new.replace("\r", "\n").split("\n"):
            i = ln.find("{")
            if i < 0:
                continue
            try:
                d = json.loads(ln[i:])
            except Exception:
                continue
            # ★★ 双格式兼容：串口回显用 cmd/id；上行云端压缩帧用 c/i
            is_result = (d.get("cmd") == "result") or (d.get("c") == "result")
            if not is_result:
                continue
            rid = str(d.get("id", d.get("i", "")))
            cid_field = str(d.get("command_id", rid))
            if cid_field != cmd_id and rid != cmd_id:
                continue

            # 终态判定：优先字符串 status；否则用数字码 s（0=OK 1=ACCEPTED
            # 2=RUNNING 3=FAILED 4=ERROR）
            st = str(d.get("status", "")).lower()
            if st in NON_TERMINAL:
                continue                      # 还没到终态，继续等
            if st:
                return (("done" if st == "success" else "failed"), d, ln)

            code = d.get("s", d.get("e", None))
            if code is None:
                return ("failed", d, ln)
            try:
                code = int(code)
            except Exception:
                return ("failed", d, ln)
            if code in (1, 2):
                continue                      # ACCEPTED / RUNNING
            return (("done" if code == 0 else "failed"), d, ln)
        time.sleep(0.15)
    return ("timeout", None, "")


def cmd_query(mon, module, key=None, timeout=12.0):
    """
    查询配置并**等待异步终态**。

    `config_query` 只回 accepted，真值在后续 result 帧里。这里按 cmd_id
    关联等待；查询是幂等操作，超时后**最多重试一次**（不重试非幂等写）。
    """
    p = {"module": module}
    if key:
        p["key"] = key

    for attempt in (1, 2):
        cid = "q%d%02d" % (int(time.time() * 1000) % 10000, attempt)
        send(mon, {"cmd": "system", "id": cid, "ob": "config_query", "p": p})
        state, payload, raw = wait_result(mon, cid, timeout)
        if state != "timeout":
            return cid, state, payload
    return cid, "timeout", None


def cmd_state(mon, key, timeout=6.0):
    cid = "s%d" % int(time.time() * 1000 % 100000)
    send(mon, {"cmd": "query_state", "ob": key, "id": cid})
    end = time.time() + timeout
    start_len = mon.size()
    while time.time() < end:
        new = mon.since(start_len)
        for ln in new.split("\n"):
            i = ln.find("{")
            if i < 0:
                continue
            try:
                d = json.loads(ln[i:])
            except Exception:
                continue
            if d.get("command") == "query_state" and str(d.get("id", "")) == cid:
                return cid, "done", d
        time.sleep(0.1)
    return cid, "timeout", None


def main():
    ap = argparse.ArgumentParser(description="Guo Feeder serial command tool")
    ap.add_argument("action", choices=["query", "state", "restart", "watch", "diag", "act"])
    ap.add_argument("args", nargs="*")
    ap.add_argument("--port", default=PORT)
    ap.add_argument("--log", default=None)
    ap.add_argument("--timeout", type=float, default=12.0)
    ap.add_argument("--yes", action="store_true", help="confirm restart")
    ns = ap.parse_args()

    log = ns.log or (".pio/gf_%s_%s.log" % (ns.action, time.strftime("%H%M%S")))
    os.makedirs(os.path.dirname(log) or ".", exist_ok=True)
    mon = Monitor(ns.port, BAUD, log)

    try:
        if ns.action == "watch":
            secs = float(ns.args[0]) if ns.args else 60.0
            print("watching %.0fs -> %s" % (secs, log))
            time.sleep(secs)
            return 0

        if ns.action == "state":
            for k in ns.args:
                cid, st, d = cmd_state(mon, k, ns.timeout)
                if st == "done":
                    print("%-26s = %s" % (k, d.get("value")))
                else:
                    print("%-26s = <%s>" % (k, st))
            return 0

        if ns.action == "act":
            # 执行任意 Action（走既有 CommandManager → WorkflowManager 链路）
            for aid in ns.args:
                cid = "a%d" % int(time.time() * 1000 % 100000)
                send(mon, {"cmd": "execute_action", "id": cid, "ob": aid})
                st, payload, raw = wait_result(mon, cid, ns.timeout)
                msg = ((payload or {}).get("message")) or ((payload or {}).get("m")) or ""
                print("ACT %-24s -> %-8s %s" % (aid, st, msg))
            return 0

        if ns.action == "query":
            module = ns.args[0]
            key = ns.args[1] if len(ns.args) > 1 else None
            cid, st, d = cmd_query(mon, module, key, ns.timeout)
            if st != "done":
                print("QUERY %s.%s -> <%s>" % (module, key or "*", st))
                return 2
            data = (d or {}).get("data")
            print("QUERY %s.%s ->" % (module, key or "*"))
            print("  " + summarize(data))
            return 0

        if ns.action == "restart":
            if not ns.yes:
                print("refusing: restart needs --yes")
                return 9
            mon.mark("about to send system/restart")
            send(mon, {"cmd": "system", "id": "r1", "ob": "restart", "p": {"restart": True}})
            print("restart command sent; keeping monitor alive")
            time.sleep(ns.timeout)
            return 0

        if ns.action == "diag":
            # 一次性抓 mi_thermo 全貌 + 关键 state
            mon.mark("DIAG start")
            cid, st, d = cmd_query(mon, "mi_thermo", None, ns.timeout)
            print("--- config_query mi_thermo: <%s> ---" % st)
            if st == "done":
                print(summarize((d or {}).get("data")))
            for k in [
                "mi_thermo_enable", "mi_thermo_valid", "mi_thermo_temp",
                "mi_thermo_humid", "mi_thermo_bat_v", "mi_thermo_temp_ts",
                "mi_thermo_scan_state",
            ]:
                _, st2, d2 = cmd_state(mon, k, 4.0)
                print("%-24s = %s" % (k, (d2 or {}).get("value", "<%s>" % st2)))
            return 0
    finally:
        print("\n[serial log] %s" % os.path.abspath(log))
        mon.close()


if __name__ == "__main__":
    sys.exit(main())
