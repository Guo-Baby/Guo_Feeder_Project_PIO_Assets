#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Guo Feeder —— 统一诊断入口（BT-1 三项联合诊断）
================================================================================
把「设备串口 / Worker 日志 / EMQX / D1」四个证据面收在**一个**关联模型下。

子命令
------
  diagnose            只读快照：串口 `logt stats|cloud|flash|fseg` + EMQX 会话 + D1 计数
  test-ack            发**一条**只读命令（`query_capabilities`），关联 ACK 请求与设备结果
  test-segment-gc     段回收：`--isolated` 跑固件 wasm 探针｜`--real` 真机只读观测
  test-result-frame   调云端仓的隔离真实 D1 测试（缺陷 C 回归）
  report              把某次运行的 JSON 汇总成 Markdown

统一关联字段（每条证据都带）
----------------------------
  run_id · 采于 UTC · device_id · boot_seq · seq · cmd_id · topic
  http_status · latency_ms · 三态(published/failed/unknown) · emqx 记录 · d1 行 · 串口行

安全边界（硬编码，不可通过参数放宽）
------------------------------------
  · 串口只用**只读** `logt stats|cloud|flash|fseg|policy`；本脚本**没有**
    调用 `logt fill/cpush/ack/emit/fwipe/mffail` 的代码路径。
  · `test-ack` 只发 `query_capabilities`（纯读注册表），且**默认 dry-run**，
    必须显式 `--allow-publish` 才会真的发。
  · D1 只跑 `SELECT` / `PRAGMA table_info`（`_assert_readonly_sql` 拦截）。
  · 凭据只从 `Cloudflare_Assets/guo-feeder-api/.dev.vars` 读入**内存**，
    不写入任何输出；报告里只出现 `device_id`（12 hex）与状态码。
  · 每个等待都有上限（`--serial-max-s` / 发布超时 / 验收窗口）。

用法
----
  python test/diag/guo_diag.py diagnose
  python test/diag/guo_diag.py test-ack --device-id 288485896ce4            # dry-run
  python test/diag/guo_diag.py test-ack --device-id 288485896ce4 --allow-publish
  python test/diag/guo_diag.py test-segment-gc --isolated
  python test/diag/guo_diag.py test-segment-gc --real --device-id 288485896ce4
  python test/diag/guo_diag.py test-result-frame
  python test/diag/guo_diag.py report --run <run_dir_or_最新>

依赖：pyserial（本机在 PlatformIO 的 venv 里）· wrangler（Cloudflare_Assets/node_modules）
"""

import argparse
import base64
import json
import os
import re
import subprocess
import sys
import time
import urllib.error
import urllib.request
from datetime import datetime, timezone

# ---------------------------------------------------------------------------
# 路径
# ---------------------------------------------------------------------------
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))          # 固件仓根
CLOUD = os.path.abspath(os.path.join(ROOT, "..", "..", "Cloudflare_Assets"))
API_DIR = os.path.join(CLOUD, "guo-feeder-api")
DEV_VARS = os.path.join(API_DIR, ".dev.vars")
WRANGLER = os.path.join(CLOUD, "node_modules", "wrangler", "bin", "wrangler.js")
WRANGLER_CFG = os.path.join(API_DIR, "wrangler.toml")
RUNS_ROOT = os.path.join(ROOT, ".pio", "diag")

# 固件常量（只用于**解释**观测值，不驱动设备行为；来源 src/log/log_events.h）
LOG_RECORDS_PER_SEGMENT = 31
LOG_SEGMENT_COUNT = 16

NODE_BIN = os.environ.get("NODE_BIN") or "node"

# 只读 SQL 白名单（前缀匹配，大小写不敏感）
_READONLY_SQL = ("select", "pragma table_info", "pragma index_list", "explain")


# ---------------------------------------------------------------------------
# 基础工具
# ---------------------------------------------------------------------------
def utc_now():
    return datetime.now(timezone.utc)


def iso(dt=None):
    return (dt or utc_now()).strftime("%Y-%m-%dT%H:%M:%S.%f")[:-3] + "Z"


def new_run_dir(tag):
    rid = utc_now().strftime("%Y%m%dT%H%M%SZ") + "_" + tag
    d = os.path.join(RUNS_ROOT, rid)
    os.makedirs(d, exist_ok=True)
    return d, rid


def save_json(run_dir, name, obj):
    p = os.path.join(run_dir, name)
    with open(p, "w", encoding="utf-8") as f:
        json.dump(obj, f, ensure_ascii=False, indent=1)
    return p


def load_dev_vars():
    """只读 `.dev.vars` 到内存；**绝不**打印或落盘。"""
    out = {}
    if not os.path.isfile(DEV_VARS):
        return out
    with open(DEV_VARS, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#") or "=" not in line:
                continue
            k, v = line.split("=", 1)
            out[k.strip()] = v.strip().strip('"').strip("'")
    return out


def _no_proxy_opener():
    return urllib.request.build_opener(urllib.request.ProxyHandler({}))


def emqx_request(env, method, path, body=None, timeout=8.0, maxlen=400):
    """直连 EMQX（绕过代理）。返回 {status, ms, body}；**不**回显凭据。"""
    base = (env.get("EMQX_API_BASE") or "").rstrip("/")
    if not base:
        return {"status": None, "ms": 0, "body": "", "error": "EMQX_API_BASE 缺失"}
    auth = base64.b64encode(
        (env.get("EMQX_APP_ID", "") + ":" + env.get("EMQX_APP_SECRET", "")).encode()
    ).decode()
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(
        base + path, data=data, method=method,
        headers={"Authorization": "Basic " + auth, "Content-Type": "application/json"},
    )
    t0 = time.time()
    try:
        with _no_proxy_opener().open(req, timeout=timeout) as r:
            return {"status": r.status, "ms": int((time.time() - t0) * 1000),
                    "body": r.read().decode("utf-8", "replace")[:maxlen]}
    except urllib.error.HTTPError as e:
        return {"status": e.code, "ms": int((time.time() - t0) * 1000),
                "body": e.read().decode("utf-8", "replace")[:maxlen]}
    except Exception as e:  # noqa: BLE001
        return {"status": 0, "ms": int((time.time() - t0) * 1000),
                "body": "", "error": type(e).__name__}


def d1_query(sql, timeout=90):
    """生产 D1 **只读**查询。非 SELECT/PRAGMA/EXPLAIN 一律拒绝。"""
    low = sql.strip().lower()
    if not any(low.startswith(p) for p in _READONLY_SQL):
        raise ValueError("拒绝非只读 SQL: " + sql[:60])
    cmd = [NODE_BIN, WRANGLER, "d1", "execute", "guofeeder", "--remote",
           "--json", "--config", WRANGLER_CFG, "--command", sql]
    env = dict(os.environ)
    for k in ("http_proxy", "https_proxy", "HTTP_PROXY", "HTTPS_PROXY", "ALL_PROXY", "all_proxy"):
        env.pop(k, None)
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout, env=env,
                       cwd=API_DIR)   # ★ cwd 必须在云端仓：否则 wrangler 会在**固件仓**
                                      #   生成 `.wrangler/cache/`（实测踩到，非 gitignored）
    out = (r.stdout or "") + (r.stderr or "")
    i = out.find("[")
    if i < 0:
        return {"error": out.strip()[:300], "rows": []}
    try:
        doc = json.loads(out[i:])
        return {"rows": doc[0].get("results", []), "meta": doc[0].get("meta", {})}
    except Exception as e:  # noqa: BLE001
        return {"error": "parse: " + str(e), "rows": []}


# ---------------------------------------------------------------------------
# 串口会话
# ---------------------------------------------------------------------------
def find_serial_port(prefer=None):
    """优先 USB 串口（CH343 / VID 1A86），**排除**蓝牙虚拟口。"""
    try:
        from serial.tools import list_ports
    except ImportError:
        return None, "pyserial 不可用（请用 PlatformIO 的 venv 运行）"
    cands = []
    for p in list_ports.comports():
        desc = (p.description or "")
        hwid = (p.hwid or "")
        if "BTHENUM" in hwid.upper() or "蓝牙" in desc or "Bluetooth" in desc:
            continue
        if prefer and p.device.upper() == prefer.upper():
            return p.device, desc
        cands.append((p.device, desc, hwid))
    for dev, desc, hwid in cands:
        if "1A86" in hwid.upper() or "CH34" in desc.upper() or "USB" in hwid.upper():
            return dev, desc
    return (cands[0][0], cands[0][1]) if cands else (None, "未发现串口")


class SerialSession:
    """只读串口会话。

    ★ `dtr=False/rts=False` 必须在 `open()` **之前**设置 —— 否则 CH343 打开瞬间
      会拉 EN 复位设备（实测：之后设备 `boot_seq` +1、EMQX 重连）。本类构造函数
      在 open 之前设置，因此**不改变设备运行状态**。
    """

    def __init__(self, port, baud=115200):
        import serial
        self.lines = []       # [(rel_s, iso_ts, text)]
        self.t0 = time.time()
        self._buf = b""
        self.ser = serial.Serial()
        self.ser.port = port
        self.ser.baudrate = baud
        self.ser.timeout = 0.05
        self.ser.dtr = False
        self.ser.rts = False
        self.ser.open()
        self.port = port

    def _emit(self, text):
        self.lines.append((round(time.time() - self.t0, 3), iso(), text))

    def drain(self, seconds, stop_when=None):
        end = time.time() + seconds
        while time.time() < end:
            chunk = self.ser.read(4096)
            if chunk:
                self._buf += chunk
                while b"\n" in self._buf:
                    ln, self._buf = self._buf.split(b"\n", 1)
                    s = ln.decode("utf-8", "replace").rstrip("\r")
                    if s:
                        self._emit(s)
                        if stop_when and stop_when(s):
                            return True
            elif stop_when and stop_when(None):
                return True
        return False

    def send(self, cmd):
        self._emit("SEND> " + cmd)
        self.ser.write((cmd + "\n").encode())
        self.ser.flush()

    def since(self, marker_index):
        return self.lines[marker_index:]

    def close(self):
        try:
            self.ser.close()
        except Exception:  # noqa: BLE001
            pass


# ---------------------------------------------------------------------------
# `logt` 行解析
# ---------------------------------------------------------------------------
_KV = re.compile(r"([A-Za-z_][A-Za-z0-9_]*)=([^\s]+)")


def parse_logt(lines):
    """从串口行里抽 `logt` 的键值。返回 dict（同名字段后者覆盖前者）。

    ⚠️ `logt fseg` 是**两行**：`fseg seg=N ... first_seq= recs=` + `fseg2 last= derived= gap=`。
       `fseg2` 行**不带 seg 号** ⇒ 必须挂到上一条 `fseg` 的段号上（否则 `last_seq` 永远取不到，
       而"整段 ≤ gc"的判定恰恰只依赖 `last_seq`）。
    """
    out = {"stats": {}, "cloud": {}, "cloud2": {}, "cloud3": {}, "cloud4": {},
           "flash": {}, "fseg": {}, "ackst": {}, "policy": {}, "raw": []}
    last_seg = None
    for _, _, s in lines:
        if "[LogT]" not in s:
            continue
        out["raw"].append(s)
        body = s.split("[LogT]", 1)[1].strip()
        head = body.split(" ", 1)[0]
        kv = {k: v for k, v in _KV.findall(body)}
        if head == "stats":
            out["stats"].update(kv)
        elif head == "cstats":
            out["stats"].update({"cstats_" + k: v for k, v in kv.items()})
        elif head == "ackst":
            out["ackst"].update(kv)
        elif head in ("cloud", "cloud2", "cloud3", "cloud4"):
            out[head].update(kv)
        elif head == "flash":
            out["flash"].update(kv)
        elif head == "policy":
            out["policy"].update(kv)
        elif head == "fseg":
            seg = kv.get("seg")
            if seg is None:
                continue
            last_seg = seg
            d = out["fseg"].setdefault(seg, {})
            d.update(kv)
            d["absent"] = ("absent" in body)
        elif head == "fseg2" and last_seg is not None:
            out["fseg"].setdefault(last_seg, {}).update(kv)
    return out


def _i(d, k, default=None):
    v = d.get(k)
    if v is None:
        return default
    try:
        return int(str(v).split("@")[0])
    except ValueError:
        return default


def snapshot_serial(sess, wait_stats=0.7):
    """读一整轮只读诊断。返回 (logt_dict, 分段时刻)。"""
    mark = len(sess.lines)
    for c in ("logt stats", "logt cloud", "logt flash", "logt policy info"):
        sess.send(c)
        sess.drain(wait_stats)
    lg = parse_logt(sess.since(mark))

    # 逐段 peek（只读）：段号从 `oldest` 起、按 `segs` 个环绕（环容量 16）
    flash = lg["flash"]
    oldest = _i(flash, "oldest")
    segs_n = _i(flash, "segs", 0) or 0
    if oldest is not None and segs_n > 0:
        targets = [(oldest + i) % LOG_SEGMENT_COUNT for i in range(min(segs_n, LOG_SEGMENT_COUNT))]
        for seg in targets:
            sess.send("logt fseg %d" % seg)
            sess.drain(0.6)
        lg = parse_logt(sess.since(mark))
    return lg, mark


# ---------------------------------------------------------------------------
# ★ T05 状态分类（唯一判据）
# ---------------------------------------------------------------------------
def classify_gc(lg):
    """把 `logt` 观测值映射为 T05 的 5 类状态。

    判据**全部来自固件**：
      · 可回收 = `log_ack_segment_reclaimable(first,last,gc,holes,count,overflow)`
        （`src/log/log_ack.h:384`）—— 这里用同一套条件在**观测值**上重算。
      · append 段**不可回收**：`log_manager.cpp:2408` 的调用方守卫。
      · `gc` 取 `cloud4 acked=<acked> gc=<gc> gcfloor=<floor>` 的 **gc**，
        不是 `acked`（FIX-BT9：用 acked_max 会误删未补发记录的段）。
    """
    flash = lg["flash"]
    cloud4 = lg["cloud4"]
    cloud3 = lg["cloud3"]
    segs_total = _i(flash, "segs", 0)
    append = _i(flash, "append")
    raw_append = flash.get("append", "")
    try:
        append_index = int(str(raw_append).split("@")[1])
    except (IndexError, ValueError):
        append_index = None

    gc = _i(cloud4, "gc")
    acked = _i(cloud4, "acked")
    hole_count = _i(cloud3, "holes", 0)
    hole_ovf = str(cloud3.get("hovf", "0")) not in ("0", "0u")
    rseg = _i(cloud3, "rseg")

    seg_detail = {}
    for seg_key, kv in lg["fseg"].items():
        if kv.get("absent"):
            seg_detail[seg_key] = {"state": "ABSENT"}
            continue
        try:
            seg = int(seg_key)
        except ValueError:
            continue
        first = _i(kv, "first_seq")
        last = _i(kv, "last")
        recs = _i(kv, "recs", 0)
        if first is None:
            seg_detail[seg_key] = {"state": "UNREADABLE"}
            continue

        if recs == 0:
            st = "EMPTY"
        elif seg == append:
            st = "PROTECTED_APPEND"
        elif hole_ovf:
            st = "BLOCKED_OVERFLOW"
        elif gc is None:
            st = "UNKNOWN_GC"
        elif last is not None and last > gc:
            st = "WAIT_UNACKED"
        elif hole_count and last is not None and first <= gc:
            # 观测面拿不到逐洞区间 ⇒ 只能保守记为"可能有洞"
            st = "WAIT_HOLE_MAYBE"
        else:
            st = "READY_NOT_RECLAIMED"
        seg_detail[seg_key] = {"state": st, "first_seq": first, "last_seq": last,
                               "records": recs, "is_append": seg == append}

    states = [v["state"] for v in seg_detail.values()]
    non_append_present = [k for k, v in seg_detail.items()
                          if v["state"] not in ("ABSENT", "EMPTY") and not v.get("is_append")]

    if segs_total == 0:
        overall = "NO_SEGMENT"
    elif not non_append_present:
        overall = "NOT_READY"
    elif "READY_NOT_RECLAIMED" in states:
        overall = "ANOMALY_READY_BUT_NOT_DELETED"
    elif any(s.startswith("WAIT") for s in states):
        overall = "WAIT"
    else:
        overall = "NOT_READY"

    # 最小前置条件（可执行、不含"造数"）
    hint = None
    if overall == "NOT_READY" and append_index is not None:
        need = max(0, LOG_RECORDS_PER_SEGMENT - append_index)
        hint = ("append 段 %d（%d/%d 条）：再落 **%d** 条 WARN+ 记录即轮转；"
                "轮转后该段变为非 append 段，若 last_seq(=%s) <= gc(=%s) 则会被回收"
                % (append or 0, append_index, LOG_RECORDS_PER_SEGMENT, need,
                   (seg_detail.get(str(append), {}) or {}).get("last_seq"), gc))

    return {
        "overall": overall,
        "segments": seg_detail,
        "non_append_present": non_append_present,
        "gc": gc, "acked": acked, "holes": hole_count, "hole_overflow": hole_ovf,
        "reclaimable_seg_reported_by_fw": rseg,
        "segs": segs_total, "append": raw_append, "append_index": append_index,
        "minimal_precondition": hint,
    }


# ---------------------------------------------------------------------------
# 子命令：diagnose
# ---------------------------------------------------------------------------
READ_ONLY_HINT = ("只读：不发命令、不改配置、不重启、不清日志；串口仅 `logt` 只读项")


def cmd_diagnose(args):
    run_dir, rid = new_run_dir("diagnose")
    env = load_dev_vars()
    doc = {"run_id": rid, "mode": "diagnose", "started_utc": iso(),
           "note": READ_ONLY_HINT}

    dev = args.device_id
    if not dev:
        # 从 D1 最近一条日志反推设备（只读）
        r = d1_query("SELECT device_id, COUNT(*) c FROM log_history "
                     "GROUP BY device_id ORDER BY c DESC LIMIT 1")
        if r.get("rows"):
            dev = r["rows"][0]["device_id"]
    doc["device_id"] = dev

    # --- 串口 ---
    port, desc = find_serial_port(args.port)
    doc["serial"] = {"port": port, "description": desc}
    if port:
        sess = None
        try:
            sess = SerialSession(port, args.baud)
            lg, _ = snapshot_serial(sess)
            doc["serial"]["logt"] = lg
            doc["serial"]["boot_seq"] = _i(lg["stats"], "boot")
            doc["serial"]["seq"] = _i(lg["stats"], "seq")
            doc["serial"]["acked_seq"] = _i(lg["cloud4"], "acked")
            doc["serial"]["gc_seq"] = _i(lg["cloud4"], "gc")
            doc["serial"]["policy"] = lg["policy"]
            doc["t05"] = classify_gc(lg)
            doc["t05"]["evidence"] = "REAL_DEVICE_OBSERVATION"
        except Exception as e:  # noqa: BLE001
            doc["serial"]["error"] = "%s: %s" % (type(e).__name__, e)
        finally:
            if sess:
                sess.close()

    # --- EMQX 会话 ---
    if dev:
        r = emqx_request(env, "GET", "/clients/dev_" + dev, maxlen=8000)
        doc["emqx_client"] = {"status": r["status"], "ms": r["ms"]}
        if r["status"] == 200:
            try:
                j = json.loads(r["body"])
                doc["emqx_client"].update({
                    "connected": j.get("connected"),
                    "connected_at": j.get("connected_at"),
                    "subscriptions_cnt": j.get("subscriptions_cnt"),
                })
            except Exception:  # noqa: BLE001
                pass
        r2 = emqx_request(env, "GET", "/clients/dev_%s/subscriptions" % dev)
        if r2["status"] == 200:
            try:
                doc["emqx_client"]["topics"] = [x.get("topic") for x in json.loads(r2["body"]) or []]
            except Exception:  # noqa: BLE001
                pass

    # --- D1（只读） ---
    if dev:
        doc["d1"] = {
            "log_history": d1_query(
                "SELECT COUNT(*) n, MAX(ts) max_ts FROM log_history WHERE device_id='%s'" % dev),
            "last_logs": d1_query(
                "SELECT id, boot_seq, seq, level, ts FROM log_history WHERE device_id='%s' "
                "ORDER BY id DESC LIMIT 5" % dev),
            "mqtt_messages": d1_query(
                "SELECT COUNT(*) n FROM mqtt_messages WHERE device_id='%s'" % dev),
            "recent_down": d1_query(
                "SELECT id, cmd, msg_id, created_at FROM mqtt_messages "
                "WHERE topic LIKE '%%/%s/down' ORDER BY id DESC LIMIT 5" % dev),
        }

    doc["finished_utc"] = iso()
    save_json(run_dir, "result_diagnose.json", doc)
    _print_diagnose(doc, run_dir)
    return 0


def _print_diagnose(doc, run_dir):
    print("=" * 74)
    print("diagnose  ", doc["run_id"], "  设备=", doc.get("device_id"))
    print("=" * 74)
    s = doc.get("serial", {})
    print("串口   :", s.get("port"), "|", s.get("description"), "→", s.get("error") or "OK")
    if s.get("boot_seq") is not None:
        print("         boot=%-4s seq=%-8s acked=%-8s gc=%s"
              % (s.get("boot_seq"), s.get("seq"), s.get("acked_seq"), s.get("gc_seq")))
    c = doc.get("emqx_client", {})
    if c:
        print("EMQX   : status=%s connected=%s subs=%s"
              % (c.get("status"), c.get("connected"), c.get("subscriptions_cnt")))
        if c.get("topics"):
            print("         订阅:", c["topics"])
    t = doc.get("t05")
    if t:
        print("T05    :", t["overall"], "| segs=%s append=%s gc=%s"
              % (t["segs"], t["append"], t["gc"]))
        for k, v in (t.get("segments") or {}).items():
            print("         seg %-3s %-22s first=%-8s last=%-8s recs=%s"
                  % (k, v.get("state"), v.get("first_seq"), v.get("last_seq"), v.get("records")))
        if t.get("minimal_precondition"):
            print("         最小前置条件:", t["minimal_precondition"])
    print("\n证据:", run_dir)


# ---------------------------------------------------------------------------
# 子命令：test-ack
# ---------------------------------------------------------------------------
ACK_FRAME_OK = "query_capabilities"   # 只读：返回注册表，不触达执行器/配置


def cmd_test_ack(args):
    run_dir, rid = new_run_dir("test-ack")
    env = load_dev_vars()
    dev = args.device_id
    cmd_id = args.cmd_id or ("diag" + utc_now().strftime("%H%M%S"))
    topic = "guo_feeder/%s/down" % dev

    doc = {"run_id": rid, "mode": "test-ack", "started_utc": iso(),
           "device_id": dev, "cmd_id": cmd_id, "topic": topic,
           "command": ACK_FRAME_OK,
           "payload": {"c": "query", "i": cmd_id, "p": {"q": ACK_FRAME_OK}},
           "publish_allowed": bool(args.allow_publish)}
    checks = []

    def chk(name, cond, detail=""):
        checks.append({"check": name, "pass": bool(cond), "detail": str(detail)})
        print(("  PASS  " if cond else "  FAIL  ") + name + (("   " + str(detail)) if detail else ""))

    port, desc = find_serial_port(args.port)
    doc["serial"] = {"port": port, "description": desc}
    sess = None
    try:
        if port is None:
            chk("串口可用", False, "未发现 USB 串口")
            raise RuntimeError("no serial")
        sess = SerialSession(port, args.baud)
        chk("串口已打开（未复位设备）", True, port)

        lg0, _ = snapshot_serial(sess)
        boot0 = _i(lg0["stats"], "boot")
        seq0 = _i(lg0["stats"], "seq")
        acked0 = _i(lg0["cloud4"], "acked")
        segdel0 = _i(lg0["ackst"], "segdel")
        doc["before"] = {"boot_seq": boot0, "seq": seq0, "acked_seq": acked0,
                         "segdel": segdel0, "gc": _i(lg0["cloud4"], "gc")}
        print("基线: boot=%s seq=%s acked=%s segdel=%s" % (boot0, seq0, acked0, segdel0))

        if not args.allow_publish:
            doc["dry_run"] = True
            print("\n[dry-run] 将向 %s 发布 %s —— 未执行（加 --allow-publish 才真的发）"
                  % (topic, json.dumps(doc["payload"], separators=(",", ":"))))
            doc["checks"] = checks
            doc["finished_utc"] = iso()
            save_json(run_dir, "result_test_ack.json", doc)
            return 0

        # --- 发布 ---
        mark = len(sess.lines)
        t_pub = time.time()
        pub = emqx_request(env, "POST", "/publish", {
            "topic": topic,
            "payload": json.dumps(doc["payload"], separators=(",", ":")),
            "qos": 1, "retain": False,
        }, timeout=args.publish_timeout)
        pub["at_utc"] = iso()
        doc["publish"] = pub
        chk("publish HTTP 200", pub["status"] == 200, "status=%s %s" % (pub["status"], pub["body"][:80]))

        body = pub["body"] or ""
        try:
            pub_id = json.loads(body).get("id")
        except Exception:  # noqa: BLE001
            pub_id = None
        chk("响应体含非空 id（成功契约）", bool(pub_id), str(pub_id))
        doc["publish"]["id"] = pub_id

        # --- 设备收到（串口） ---
        got_rx = sess.drain(args.rx_max_s, stop_when=lambda s: s is not None and "[Cloud] MQTT RX" in s)
        rx_at = None
        for rel, ts, s in sess.since(mark):
            if "[Cloud] MQTT RX" in s:
                rx_at = ts
                break
        doc["device_rx"] = {"seen": bool(got_rx), "at_utc": rx_at,
                            "latency_ms": (None if not rx_at else int(
                                (datetime.strptime(rx_at, "%Y-%m-%dT%H:%M:%S.%fZ")
                                 .replace(tzinfo=timezone.utc).timestamp() - t_pub) * 1000))}
        chk("设备收到下行（串口 MQTT RX）", got_rx, doc["device_rx"].get("latency_ms"))

        saw_uplink = False
        for rel, ts, s in sess.since(mark):
            if '"c":"result"' in s or '"c": "result"' in s:
                saw_uplink = True
                break
        doc["device_uplink"] = {"seen": saw_uplink}
        chk("设备回 result（上行）", saw_uplink)

        # --- 日志上传 + ACK ---
        got_ack = sess.drain(args.ack_max_s,
                             stop_when=lambda s: s is not None and "[Log Cloud] ack ok" in s)
        ack_line = ""
        for rel, ts, s in sess.since(mark):
            if "[Log Cloud] ack ok" in s:
                ack_line = s
                break
        doc["device_ack"] = {"seen": bool(got_ack), "line": ack_line}
        chk("设备处理 log_ack（串口 ack ok）", got_ack, ack_line[:90])

        # --- 事后快照 ---
        sess.drain(0.5)
        lg1, _ = snapshot_serial(sess)
        doc["after"] = {"boot_seq": _i(lg1["stats"], "boot"), "seq": _i(lg1["stats"], "seq"),
                        "acked_seq": _i(lg1["cloud4"], "acked"),
                        "segdel": _i(lg1["ackst"], "segdel"),
                        "gc": _i(lg1["cloud4"], "gc")}
        a1 = doc["after"]["acked_seq"]
        chk("acked_seq 推进（> 基线）",
            a1 is not None and acked0 is not None and a1 > acked0,
            "%s -> %s" % (acked0, a1))
        chk("boot_seq 未变（测试未导致复位）",
            doc["after"]["boot_seq"] == boot0, "%s -> %s" % (boot0, doc["after"]["boot_seq"]))
        chk("segdel 未回退", (doc["after"]["segdel"] or 0) >= (segdel0 or 0))

        # --- D1 关联（只读） ---
        d1 = {
            "log_rows_for_seq": d1_query(
                "SELECT COUNT(*) n FROM log_history WHERE device_id='%s' AND boot_seq=%s AND seq=%s"
                % (dev, doc["after"]["boot_seq"], doc["after"]["seq"])),
            "msg_by_cmd_id": d1_query(
                "SELECT id, direction, cmd, msg_id FROM mqtt_messages WHERE msg_id='%s' ORDER BY id"
                % cmd_id),
        }
        doc["d1"] = d1
        rows = (d1["log_rows_for_seq"] or {}).get("rows") or []
        chk("D1 中该 seq 恰 1 行（幂等）", bool(rows) and rows[0].get("n") == 1,
            rows[0].get("n") if rows else "无结果")

        doc["t05_after"] = classify_gc(lg1)
        doc["t05_after"]["evidence"] = "REAL_DEVICE_OBSERVATION"
    except Exception as e:  # noqa: BLE001
        doc["error"] = "%s: %s" % (type(e).__name__, e)
    finally:
        if sess:
            sess.close()

    doc["checks"] = checks
    doc["ok"] = all(c["pass"] for c in checks) and not doc.get("error")
    doc["finished_utc"] = iso()
    save_json(run_dir, "result_test_ack.json", doc)
    print("\n结果: %s  (%d/%d)" % ("PASS" if doc["ok"] else "FAIL",
                                   sum(1 for c in checks if c["pass"]), len(checks)))
    print("证据:", run_dir)
    return 0 if doc["ok"] else 1


# ---------------------------------------------------------------------------
# 子命令：test-segment-gc
# ---------------------------------------------------------------------------
def cmd_test_segment_gc(args):
    run_dir, rid = new_run_dir("test-segment-gc")
    doc = {"run_id": rid, "mode": "test-segment-gc", "started_utc": iso()}

    if args.isolated:
        cxx = os.environ.get("CXX") or r"C:\Program Files\LLVM\bin\clang++.exe"
        wasm = os.path.join(ROOT, ".pio", "probe_gc.wasm")
        cmd = [cxx, "--target=wasm32", "-std=gnu++11", "-nostdlib", "-fno-builtin", "-O1",
               "-I", os.path.join(ROOT, "src"), "-I", os.path.join(ROOT, "src", "log"),
               os.path.join(ROOT, "test", "log_contract", "probe_gc.cpp"),
               "-Wl,--no-entry", "-Wl,--export-all", "-o", wasm]
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=180)
        doc["compile"] = {"rc": r.returncode, "stderr": (r.stderr or "")[:600]}
        if r.returncode == 0:
            r2 = subprocess.run([NODE_BIN, os.path.join(ROOT, "test", "log_contract", "run_wasm_probe.js"), wasm],
                                capture_output=True, text=True, timeout=120)
            doc["probe"] = {"rc": r2.returncode, "out": (r2.stdout or "")}
        doc["result"] = "PASS" if doc.get("probe", {}).get("rc") == 0 else "FAIL"
        doc["evidence"] = "ISOLATED_WASM_PROBE"
        save_json(run_dir, "result_segment_gc.json", doc)
        print(doc.get("probe", {}).get("out", ""))
        print("result =", doc["result"], "（隔离探针，**不**代表真机段回收已验证）")
        return 0 if doc["result"] == "PASS" else 1

    if args.real:
        port, desc = find_serial_port(args.port)
        doc["serial"] = {"port": port, "description": desc}
        sess = None
        try:
            sess = SerialSession(port, args.baud)
            lg0, _ = snapshot_serial(sess)
            c0 = classify_gc(lg0)
            doc["snapshot_0"] = {"at_utc": iso(), "boot_seq": _i(lg0["stats"], "boot"),
                                 "segdel": _i(lg0["ackst"], "segdel"), "classify": c0}

            # 纯观测：不触发、不造数。等待窗口内只读两次。
            sess.drain(args.observe_s)
            lg1, _ = snapshot_serial(sess)
            c1 = classify_gc(lg1)
            doc["snapshot_1"] = {"at_utc": iso(), "boot_seq": _i(lg1["stats"], "boot"),
                                 "segdel": _i(lg1["ackst"], "segdel"), "classify": c1}

            deleted = [s for _, _, s in sess.lines if "fully acked -> deleted" in s]
            doc["deleted_lines"] = deleted
            d0 = _i(lg0["ackst"], "segdel", 0) or 0
            d1 = _i(lg1["ackst"], "segdel", 0) or 0

            if d1 > d0 and deleted:
                doc["result"] = "PASS"
                doc["reason"] = "观察到非 append 段被回收（segdel %d→%d）" % (d0, d1)
            elif c1["overall"] == "NOT_READY":
                doc["result"] = "NOT_READY"
                doc["reason"] = c1.get("minimal_precondition") or "无回收候选段"
            elif c1["overall"] == "ANOMALY_READY_BUT_NOT_DELETED":
                doc["result"] = "ANOMALY"
                doc["reason"] = "存在满足条件的段但未被回收（需查调用方守卫/IO 失败）"
            else:
                doc["result"] = "NOT_OBSERVED"
                doc["reason"] = "窗口内无可回收事件（状态=%s）" % c1["overall"]
            doc["evidence"] = "REAL_DEVICE_OBSERVATION"
        except Exception as e:  # noqa: BLE001
            doc["error"] = "%s: %s" % (type(e).__name__, e)
            doc["result"] = "BLOCKED"
        finally:
            if sess:
                sess.close()
        save_json(run_dir, "result_segment_gc.json", doc)
        print("T05 result =", doc.get("result"), "|", doc.get("reason") or doc.get("error"))
        print("证据:", run_dir)
        return 0 if doc.get("result") in ("PASS", "NOT_READY", "NOT_OBSERVED") else 1

    print("请指定 --isolated 或 --real")
    return 2


# ---------------------------------------------------------------------------
# 子命令：test-result-frame
# ---------------------------------------------------------------------------
def cmd_test_result_frame(args):
    run_dir, rid = new_run_dir("test-result-frame")
    script = os.path.join(API_DIR, "test", "real_d1", "mqtt_result_frame_real_d1.sh")
    env = dict(os.environ)
    env["NODE_BIN"] = NODE_BIN
    for k in ("http_proxy", "https_proxy", "HTTP_PROXY", "HTTPS_PROXY", "ALL_PROXY", "all_proxy"):
        env.pop(k, None)
    r = subprocess.run(["bash", script], capture_output=True, text=True, timeout=args.timeout, env=env)
    out = (r.stdout or "") + (r.stderr or "")
    with open(os.path.join(run_dir, "real_d1.log"), "w", encoding="utf-8") as f:
        f.write(out)
    doc = {"run_id": rid, "mode": "test-result-frame", "started_utc": iso(),
           "script": script, "rc": r.returncode,
           "result": "PASS" if "RESULT: PASS" in out else "FAIL",
           "tail": out.strip().splitlines()[-12:],
           "finished_utc": iso()}
    save_json(run_dir, "result_test_result_frame.json", doc)
    print("\n".join(doc["tail"]))
    print("result =", doc["result"], "（真实 D1 引擎 · 隔离；不代表生产已修复验证）")
    return 0 if doc["result"] == "PASS" else 1


# ---------------------------------------------------------------------------
# 子命令：report
# ---------------------------------------------------------------------------
def cmd_report(args):
    root = args.run
    if not root:
        cands = sorted(os.listdir(RUNS_ROOT)) if os.path.isdir(RUNS_ROOT) else []
        if not cands:
            print("没有任何运行记录")
            return 2
        root = os.path.join(RUNS_ROOT, cands[-1])
    docs = []
    for fn in sorted(os.listdir(root)):
        if fn.startswith("result_") and fn.endswith(".json"):
            with open(os.path.join(root, fn), encoding="utf-8") as f:
                docs.append((fn, json.load(f)))
    if not docs:
        print("目录里没有 result_*.json:", root)
        return 2

    L = ["# Guo Feeder 诊断报告", "",
         "- 目录：`%s`" % root, "- 生成：`%s`（UTC）" % iso(), ""]
    for fn, d in docs:
        L.append("## %s" % d.get("mode", fn))
        L.append("")
        L.append("| 项 | 值 |")
        L.append("|---|---|")
        for k in ("run_id", "device_id", "started_utc", "finished_utc"):
            if d.get(k) is not None:
                L.append("| %s | `%s` |" % (k, d[k]))
        if d.get("result"):
            L.append("| **result** | **%s** |" % d["result"])
        if d.get("ok") is not None:
            L.append("| ok | `%s` |" % d["ok"])
        if d.get("reason"):
            L.append("| reason | %s |" % d["reason"])
        if d.get("error"):
            L.append("| error | `%s` |" % d["error"])
        L.append("")
        if d.get("checks"):
            L.append("| 检查 | 结果 | 细节 |")
            L.append("|---|---|---|")
            for c in d["checks"]:
                L.append("| %s | %s | `%s` |" % (c["check"], "PASS" if c["pass"] else "FAIL", c["detail"]))
            L.append("")
        if d.get("t05") or d.get("snapshot_1"):
            t = d.get("t05") or (d.get("snapshot_1") or {}).get("classify") or {}
            if t:
                L.append("**T05 分段状态**：`%s`（gc=`%s`，append=`%s`）" % (
                    t.get("overall"), t.get("gc"), t.get("append")))
                L.append("")
                if t.get("minimal_precondition"):
                    L.append("> 最小前置条件：%s" % t["minimal_precondition"])
                    L.append("")
    md = "\n".join(L) + "\n"
    out = os.path.join(root, "REPORT.md")
    with open(out, "w", encoding="utf-8") as f:
        f.write(md)
    print(md)
    print("已写入:", out)
    return 0


# ---------------------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser(description="Guo Feeder 统一诊断入口")
    sub = ap.add_subparsers(dest="cmd", required=True)

    common = dict()
    p = sub.add_parser("diagnose", help="只读快照")
    p.add_argument("--device-id")
    p.add_argument("--port")
    p.add_argument("--baud", type=int, default=115200)
    p.set_defaults(func=cmd_diagnose)

    p = sub.add_parser("test-ack", help="发一条只读命令并关联 ACK")
    p.add_argument("--device-id", required=True)
    p.add_argument("--port")
    p.add_argument("--baud", type=int, default=115200)
    p.add_argument("--cmd-id")
    p.add_argument("--allow-publish", action="store_true",
                   help="真的发布（否则 dry-run）")
    p.add_argument("--publish-timeout", type=float, default=8.0)
    p.add_argument("--rx-max-s", type=float, default=10.0)
    p.add_argument("--ack-max-s", type=float, default=15.0)
    p.set_defaults(func=cmd_test_ack)

    p = sub.add_parser("test-segment-gc", help="段回收（隔离/真机）")
    p.add_argument("--isolated", action="store_true")
    p.add_argument("--real", action="store_true")
    p.add_argument("--device-id")
    p.add_argument("--port")
    p.add_argument("--baud", type=int, default=115200)
    p.add_argument("--observe-s", type=float, default=30.0)
    p.set_defaults(func=cmd_test_segment_gc)

    p = sub.add_parser("test-result-frame", help="缺陷 C：真实 D1 隔离回归")
    p.add_argument("--timeout", type=float, default=240.0)
    p.set_defaults(func=cmd_test_result_frame)

    p = sub.add_parser("report", help="汇总某次运行的 JSON")
    p.add_argument("--run")
    p.set_defaults(func=cmd_report)

    args = ap.parse_args()
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
