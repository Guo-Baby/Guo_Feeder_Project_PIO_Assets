"""G-4 方案 A · 可控突发注入（只读 config_query）· curl 版。

为什么用 curl：urllib 的 `Python-urllib` UA 被 Cloudflare 以 `error code: 1010` 拦截（403）；
curl 已在同一路径上反复验证可用。

约束：
  · 只发 system.config_query（只读，不写配置、不动硬件）；
  · 每条命令 ID 唯一（固件 30 s 内重复 ID 会被静默丢弃）；
  · 按固定间隔**异步**发起（不等返回），以形成真实突发；
  · 全程记录发起时刻、完成时刻、HTTP 状态、响应体。

用法：python g4_burst.py <count> <interval_sec> <tag> <outfile>
"""
import json
import subprocess
import sys
import threading
import time
import datetime

COUNT = int(sys.argv[1])
INTERVAL = float(sys.argv[2])
TAG = sys.argv[3]
OUT = sys.argv[4]

URL = "https://guo-feeder-pagesdev.pages.dev/api/command"
PROXY = "http://127.0.0.1:7890"
CURL = "curl.exe"

records = []
lock = threading.Lock()


def iso(t):
    return datetime.datetime.fromtimestamp(t, datetime.UTC).strftime("%H:%M:%S.%f")[:-3]


def one(idx, cmd_id, t_send):
    body = json.dumps(
        {
            "device_id": "288485896ce4",
            "c": "system",
            "i": cmd_id,
            "p": {"o": "config_query", "module": "valve"},
        },
        ensure_ascii=False,
        separators=(",", ":"),
    )
    rec = {
        "tag": TAG,
        "idx": idx,
        "cmd_id": cmd_id,
        "t_send": t_send,
        "t_send_iso": iso(t_send),
        "http": None,
        "t_done": None,
        "elapsed": None,
        "body": None,
        "error": None,
    }
    cmd = [
        CURL, "-sS", "-m", "30", "-x", PROXY,
        "-X", "POST", URL,
        "-H", "Content-Type: application/json",
        "--data-binary", body,
        "-w", "\n__HTTP__%{http_code}",
    ]
    try:
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=40)
        raw = (p.stdout or "").strip()
        if "__HTTP__" in raw:
            body_part, code = raw.rsplit("__HTTP__", 1)
            rec["http"] = int(code.strip() or 0)
            rec["body"] = body_part.strip()[:120]
        else:
            rec["body"] = raw[:120]
        if p.returncode != 0:
            rec["error"] = (p.stderr or "")[:160]
    except Exception as e:  # noqa: BLE001
        rec["error"] = repr(e)[:160]
    rec["t_done"] = time.time()
    rec["elapsed"] = round(rec["t_done"] - t_send, 3)
    with lock:
        records.append(rec)


print("# burst tag=%s count=%d interval=%.2fs start=%s" % (TAG, COUNT, INTERVAL, iso(time.time())))
threads = []
t_start = time.time()
for i in range(COUNT):
    cid = "g4%s%02d%d" % (TAG, i, int(t_start * 1000) % 100000)
    t_send = time.time()
    th = threading.Thread(target=one, args=(i, cid, t_send), daemon=True)
    th.start()
    threads.append(th)
    print("  send idx=%02d id=%s at %s" % (i, cid, iso(t_send)), flush=True)
    if i < COUNT - 1:
        time.sleep(INTERVAL)

t_sent_all = time.time()
for th in threads:
    th.join(timeout=45)

records.sort(key=lambda x: x["idx"])
ok = sum(1 for r in records if r["http"] == 200)
print("# sent_all=%s | done=%d | http200=%d" % (iso(t_sent_all), len(records), ok))

with open(OUT, "w", encoding="utf-8") as f:
    json.dump(
        {
            "tag": TAG,
            "count": COUNT,
            "interval": INTERVAL,
            "t_start": t_start,
            "t_start_iso": iso(t_start),
            "t_sent_all": t_sent_all,
            "t_sent_all_iso": iso(t_sent_all),
            "http200": ok,
            "records": records,
        },
        f,
        ensure_ascii=False,
        indent=1,
    )
print("# written %s" % OUT)
