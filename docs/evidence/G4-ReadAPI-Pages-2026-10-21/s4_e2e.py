"""
S-4 端到端：浏览器 → Pages（会话/代理）→ Worker（真实 SQL）→ 本地 D1
=============================================================================
链路：127.0.0.1:8793（Pages dev）→ 127.0.0.1:8794（Worker dev，--local D1）
所有值均为**本地测试假值**，不涉及生产凭据、不写生产数据库。
"""
import json
import time
import urllib.error
import urllib.parse
import urllib.request

PAGES = "http://127.0.0.1:8793"
PW = "local-test-pw"
DEV = "0b0053a00e2e"


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None


_opener = urllib.request.build_opener(NoRedirect())


def call(path, method="GET", body=None, cookie=None, follow=False):
    url = PAGES + path
    data = None
    headers = {"User-Agent": "Mozilla/5.0 (S4 e2e)"}
    if body is not None:
        data = json.dumps(body).encode()
        headers["Content-Type"] = "application/json"
    if cookie:
        headers["Cookie"] = cookie
    req = urllib.request.Request(url, data=data, headers=headers, method=method)

    opener = urllib.request.build_opener() if follow else _opener
    try:
        with opener.open(req, timeout=20) as r:
            return r.status, r.read().decode("utf-8", "replace"), dict(r.headers)
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode("utf-8", "replace"), dict(e.headers)


results = []


def rec(tag, desc, actual, expect, extra=""):
    ok = (actual == expect)
    results.append((tag, desc, actual, expect, ok, extra))
    print("  %s  %-6s %-52s actual=%s expect=%s %s"
          % ("PASS" if ok else "FAIL", tag, desc, actual, expect, extra))


print("== A. 未登录 ==")
st, bd, hd = call("/api/auth/session")
j = json.loads(bd)
rec("A-1", "GET /api/auth/session 未登录", j.get("authenticated"), False)
rec("A-2", "turnstile_site_key 未配置时为 null", j.get("turnstile_site_key"), None)

st, bd, hd = call("/app.html")
rec("A-3", "未登录访问 /app.html 被重定向", st, 302)
loc = hd.get("Location", "")
rec("A-4", "重定向目标为登录页且带 next", "/login.html" in loc and "next=" in loc, True, loc)

st, bd, hd = call("/api/read/admin/metrics")
rec("A-5", "未登录调代理 ⇒ 401", st, 401)

print("== B. 登录 ==")
st, bd, hd = call("/api/auth/login", "POST", {"password": "wrong-pw"})
j = json.loads(bd)
rec("B-1", "错口令 ⇒ 401", st, 401)
rec("B-2", "返回剩余尝试次数", isinstance(j.get("remaining_attempts"), int), True)

st, bd, hd = call("/api/auth/login", "POST", {"password": PW})
j = json.loads(bd)
rec("B-3", "正确口令 ⇒ 200", st, 200)
rec("B-4", "响应 ok:true", j.get("ok"), True)
raw_cookie = hd.get("Set-Cookie", "")
rec("B-5", "下发 gf_session", "gf_session=" in raw_cookie, True)
rec("B-6", "HttpOnly 属性", "HttpOnly" in raw_cookie, True)
cookie = raw_cookie.split(";")[0]

st, bd, hd = call("/api/auth/session", cookie=cookie)
j = json.loads(bd)
rec("B-7", "带 cookie 的会话状态", j.get("authenticated"), True)

st, bd, hd = call("/app.html", cookie=cookie)
# Pages 的 clean-URL 行为：/app.html 会 308 到 /app，故此处接受 200（已规范）或 308（重定向到无后缀）
rec("B-8", "已登录访问 /app.html 不被拦（200 或 308→/app）", st in (200, 308), True, "status=%s" % st)

st, bd, hd = call("/app", cookie=cookie)
rec("B-9", "已登录访问 /app（clean URL）得到页面", st, 200)

st, bd, hd = call("/app")
rec("B-10", "★ 未登录访问 /app（clean URL）仍被拦", st, 302,
    hd.get("Location", ""))

print("== C. 同源代理 → 真实 Worker → 本地 D1 ==")
st, bd, hd = call("/api/read/admin/metrics", cookie=cookie)
j = json.loads(bd)
rec("C-1", "metrics 经代理可达", st, 200)
tbl = j.get("tables") or {}
rec("C-2", "表规模为真实计数（telemetry=5）", tbl.get("telemetry_history"), 5)
rec("C-3", "ingest.scope 自陈为 isolate", (j.get("ingest") or {}).get("scope"), "isolate")
rec("C-4", "ingest.persistent 为 false", (j.get("ingest") or {}).get("persistent"), False)
rec("C-5", "当前生效模式为 observe", (j.get("server") or {}).get("ingest_validation"), "observe")

st, bd, hd = call("/api/read/history/log?limit=3", cookie=cookie)
j = json.loads(bd)
rec("C-6", "日志读端点经代理可达", st, 200)
rec("C-7", "返回 3 条", j.get("count"), 3)
rec("C-8", "has_more=true", j.get("has_more"), True)

st, bd, hd = call("/api/read/history/log?level=WARN", cookie=cookie)
j = json.loads(bd)
rec("C-9", "按级别过滤生效", j.get("count"), 1,
    (j.get("items") or [{}])[0].get("message") if j.get("items") else "")

st, bd, hd = call("/api/read/history/telemetry?limit=2", cookie=cookie)
j = json.loads(bd)
rec("C-10", "遥测读端点经代理可达", st, 200)
rec("C-11", "keyset 游标可用", isinstance(j.get("next_before_id"), int), True)

st, bd, hd = call("/api/read/admin/device/%s/snapshot?recent=5" % DEV, cookie=cookie)
j = json.loads(bd)
rec("C-12", "设备快照经代理可达", st, 200)
rec("C-13", "快照含设备状态", (j.get("device") or {}).get("state"), "CLAIM_PENDING")
rec("C-14", "每类遥测各取最新一条（3 类）", len(j.get("telemetry_latest") or []), 3)
rec("C-15", "counts.telemetry=5", (j.get("counts") or {}).get("telemetry"), 5)

print("== D. 白名单（写端点不可被代理） ==")
for tag, path in [
    ("D-1", "/api/read/admin/reconcile"),
    ("D-2", "/api/read/admin/device/%s/revoke" % DEV),
    ("D-3", "/api/read/history/ingest"),
    ("D-4", "/api/read/admin/device/../../x/snapshot"),
    ("D-5", "/api/read/"),
]:
    st, bd, hd = call(path, cookie=cookie)
    rec(tag, "拒绝 " + path, st, 403)

print("== E. EMQX 写入入口不被中间件拦 ==")
st, bd, hd = call("/api/webhook")
rec("E-1", "GET /api/webhook 可达（既有状态页）", st, 200, bd[:24])

print("== F. 登出 ==")
st, bd, hd = call("/api/auth/logout", "POST", {}, cookie=cookie)
rec("F-1", "登出 200", st, 200)
rec("F-2", "清除 cookie（Max-Age=0）", "Max-Age=0" in hd.get("Set-Cookie", ""), True)

print("== G. 限流（独立 IP 段不适用，此处用同一路径连续错口令） ==")
codes = []
for i in range(6):
    st, bd, hd = call("/api/auth/login", "POST", {"password": "wrong-%d" % i})
    codes.append(st)
    time.sleep(0.05)
rec("G-1", "连续失败第 5 次起出现 429", 429 in codes[:6], True, "codes=%s" % codes)

st, bd, hd = call("/api/auth/login", "POST", {"password": PW})
rec("G-2", "封禁后即使口令正确也被拒（429）", st, 429)

total = len(results)
passed = sum(1 for r in results if r[4])
print("\n===== S4 e2e =====")
print("  total %d | PASS %d | FAIL %d" % (total, passed, total - passed))
if passed != total:
    print("  ★ FAIL:")
    for r in results:
        if not r[4]:
            print("    - %s %s actual=%s expect=%s %s" % (r[0], r[1], r[2], r[3], r[5]))
