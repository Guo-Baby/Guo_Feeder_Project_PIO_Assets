# -*- coding: utf-8 -*-
"""UI v1.1 真机综合测试 —— MQTT 场景执行器（T3 / T4 / T5 / T6 用）

设计原则
--------
1. **不做"猜测式"断言**：断言只针对设备真实回帧（ack / result）。
2. **每条命令用唯一 cmd_id**（设备 cmd_id 去重窗口 10 条 / 30 s）。
3. **动作类命令必须带 v + k**：固件压缩下行里 `c="action"` 只认 registry
   version + stable_id，不接受 runtime 名称 ⇒ 本脚本先查 registry 再构造命令
   （顺带把 T3 的 registry 一致性也验了）。
4. 结果同时落盘：`--json` 输出结构化结果，便于写报告。

⚠️ 本脚本**不做**安全判断。危险用例（开阀 / 振动 / 急停）由调用方显式选择，
   并且必须已经满足现场安全前置（水不溢出、人手在场、可断电）。

用法（必须在隔离 venv 下跑）：
    C:/Users/wang/.workbuddy/binaries/python/envs/default/Scripts/python.exe \
        test/ui_v11_contract/scenario.py t3 --device-id 288485896ce4
"""

import argparse
import json
import os
import ssl
import sys
import time

BROKER = "n302933b.ala.cn-hangzhou.emqxsl.cn"
PORT = 8883
CA = os.path.join(
    os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))),
    "data", "emqxsl-ca.crt",
)
USER, PASS = "workbuddy", "workbuddy"

# 固件上行 status 压缩枚举（cloud_manager.h::CloudStatus）
S_OK, S_ACCEPTED, S_RUNNING, S_FAILED, S_ERROR = 0, 1, 2, 3, 4


# ---------------------------------------------------------------------------
# MQTT 探针
# ---------------------------------------------------------------------------
class Probe(object):
    def __init__(self, device_id, tag):
        self.device_id = device_id
        self.tag = tag
        self.sub_topic = "guo_feeder/%s/up" % device_id
        self.pub_topic = "guo_feeder/%s/down" % device_id
        self.frames = []
        self.raw = []
        self.client = None

    def connect(self, wait=4.0):
        from paho.mqtt.client import Client as MQTTClient
        try:
            from paho.mqtt.client import CallbackAPIVersion
            cli = MQTTClient(CallbackAPIVersion.VERSION2,
                             client_id="wb_%s_%d" % (self.tag, int(time.time())))
        except Exception:
            cli = MQTTClient(client_id="wb_%s_%d" % (self.tag, int(time.time())))
        cli.username_pw_set(USER, PASS)
        cli.tls_set(ca_certs=CA, cert_reqs=ssl.CERT_REQUIRED)
        cli.on_connect = self._on_connect
        cli.on_message = self._on_message
        cli.connect(BROKER, PORT, keepalive=60)
        cli.loop_start()
        time.sleep(wait)
        self.client = cli
        return self

    def _on_connect(self, client, userdata, flags, rc, *a):
        code = getattr(rc, "value", rc)
        print("[MQTT] connected rc=%s" % (code,))
        client.subscribe(self.sub_topic, qos=1)

    def _on_message(self, client, userdata, msg):
        text = msg.payload.decode("utf-8", "replace")
        self.raw.append(text)
        try:
            obj = json.loads(text)
        except Exception:
            obj = {"_raw": text}
        self.frames.append(obj)
        print("[UP] %s" % json.dumps(obj, ensure_ascii=False))

    def close(self):
        if self.client is not None:
            try:
                self.client.loop_stop()
                self.client.disconnect()
            except Exception:
                pass

    def send(self, cmd_id, body, wait=8.0, want_results=1):
        body = dict(body)
        body["i"] = cmd_id
        before = len(self.frames)
        self.client.publish(self.pub_topic, json.dumps(body), qos=1)
        print("[DOWN] %s" % json.dumps(body, ensure_ascii=False))

        t0 = time.time()
        ack = None
        results = []
        while time.time() - t0 < wait:
            results = []
            for f in self.frames[before:]:
                if f.get("i") != cmd_id and f.get("id") != cmd_id:
                    continue
                kind = f.get("c") or f.get("cmd")
                if kind == "ack" and ack is None:
                    ack = f
                elif kind == "result":
                    results.append(f)
            if len(results) >= want_results:
                break
            time.sleep(0.15)
        return ack, (results[0] if results else None), results


# ---------------------------------------------------------------------------
# 结果记录
# ---------------------------------------------------------------------------
RESULTS = []


def rec(case, desc, ok, detail="", status=None):
    if status is None:
        status = "PASS" if ok else "FAIL"
    RESULTS.append({"case": case, "desc": desc, "status": status,
                    "detail": str(detail)[:400]})
    print("  [%-10s] %-12s %s   %s" % (case, status, desc, str(detail)[:170]))
    return ok


def brief(r):
    return json.dumps(r, ensure_ascii=False)[:220] if r else "无 result"


# ---------------------------------------------------------------------------
# registry 解析（动作类/工作流类命令必须用 v + k）
# ---------------------------------------------------------------------------
def fetch_registry(p, kind, cmd_id):
    """kind: 0=action 1=trigger 2=workflow。返回 (version, {runtime_id: stable_id})"""
    ack, r, _rs = p.send(cmd_id, {"c": "registry", "k": kind}, wait=10.0)
    if r is None:
        return None, {}
    ver = r.get("v")
    m = {}
    for pair in (r.get("d") or []):
        if isinstance(pair, list) and len(pair) >= 2:
            m[str(pair[1])] = int(pair[0])
    return ver, m


# ---------------------------------------------------------------------------
# T3：只读命令
# ---------------------------------------------------------------------------
def t3_cases():
    return [
        ("T3-01", "query_state valve_status", {"c": "query", "p": {"q": "query_state", "o": "valve_status"}}, None),
        ("T3-02", "query_state weight_valid", {"c": "query", "p": {"q": "query_state", "o": "weight_valid"}}, None),
        ("T3-03", "query_actions", {"c": "query", "p": {"q": "query_actions"}}, S_OK),
        ("T3-04", "query_triggers", {"c": "query", "p": {"q": "query_triggers"}}, S_OK),
        ("T3-05", "query_workflows", {"c": "query", "p": {"q": "query_workflows"}}, S_OK),
        ("T3-06", "query_capabilities", {"c": "query", "p": {"q": "query_capabilities"}}, S_OK),
        ("T3-07", "query_action_registry", {"c": "query", "p": {"q": "query_action_registry"}}, S_OK),
        ("T3-08", "query_trigger_registry", {"c": "query", "p": {"q": "query_trigger_registry"}}, S_OK),
        ("T3-09", "query_workflow_registry", {"c": "query", "p": {"q": "query_workflow_registry"}}, S_OK),
        ("T3-10", "registry k=0 action", {"c": "registry", "k": 0}, S_OK),
        ("T3-11", "registry k=1 trigger", {"c": "registry", "k": 1}, S_OK),
        ("T3-12", "registry k=2 workflow", {"c": "registry", "k": 2}, S_OK),
        ("T3-13", "system.memory", {"c": "system", "p": {"o": "memory"}}, S_OK),
        ("T3-14", "system.flash", {"c": "system", "p": {"o": "flash"}}, S_OK),
        ("T3-15", "system.stats", {"c": "system", "p": {"o": "stats"}}, S_OK),
        ("T3-16", "system.loop", {"c": "system", "p": {"o": "loop"}}, S_OK),
        ("T3-17", "system.memwatch", {"c": "system", "p": {"o": "memwatch"}}, S_OK),
        ("T3-18", "system.restart_status", {"c": "system", "p": {"o": "restart_status"}}, S_OK),
        ("T3-19", "system.time", {"c": "system", "p": {"o": "time"}}, S_OK),
        # ★ config_query 是**异步**的：第一帧 s=1 accepted，第二帧才带 data
        #   ⇒ 单独处理（见 run_t3 末尾 T3-20/T3-20b）
        ("T3-21", "workflow.sync_info", {"c": "workflow.sync_info", "p": {}}, S_OK),
        ("T3-22", "workflow.list", {"c": "workflow.list", "p": {}}, S_OK),
        # ★ 参数键是 `id`（workflow_resolve_slot 读 pl["id"]），**不是** `o`
        ("T3-23", "workflow.get id=3", {"c": "workflow.get", "p": {"id": 3}}, S_OK),
        ("T3-24", "负向 query_state 缺 o", {"c": "query", "p": {"q": "query_state"}}, S_ERROR),
        ("T3-25", "负向 registry 缺 k", {"c": "registry"}, S_ERROR),
        ("T3-26", "负向 未知 system 对象", {"c": "system", "p": {"o": "no_such_object"}}, S_ERROR),
        ("T3-27", "负向 action 版本不符(v=99)", {"c": "action", "v": 99, "k": 0}, S_ERROR),
    ]


def run_t3(p):
    print("\n===== T3 只读命令及协议 =====")
    for i, (case, desc, frame, expect_s) in enumerate(t3_cases()):
        cid = "wbt3-%02d" % i
        ack, result, _rs = p.send(cid, frame, wait=10.0)

        # ack：压缩格式 {"c":"ack","i":<id>,...}
        if ack is None:
            # 版本不符等协议层拒绝不发 ack（cloud_translate_command 直接报错）
            rec(case + "a", desc + " | ack", expect_s == S_ERROR,
                "无 ack（协议层拒绝时属预期）", "PASS" if expect_s == S_ERROR else "FAIL")
        else:
            ok = (ack.get("c") == "ack" and ack.get("i") == cid)
            rec(case + "a", desc + " | ack c/i", ok,
                "c=%r i=%r" % (ack.get("c"), ack.get("i")))

        if result is None:
            rec(case, desc + " | result", False, "超时无 result", "FAIL")
        elif expect_s is None:
            rec(case, desc + " | result", True, brief(result))
        else:
            got = result.get("s")
            rec(case, desc + " | s", got == expect_s,
                "s=%r 期望 %r  m=%r" % (got, expect_s, result.get("m")))
        time.sleep(0.35)

    # ---- T3-20 config_query（异步：两帧）
    ack, first, frames = p.send(
        "wbt3-cq",
        {"c": "system", "p": {"o": "config_query", "module": "valve"}},
        wait=14.0, want_results=2)
    rec("T3-20", "config_query 第一帧为 accepted(1)",
        first is not None and first.get("s") == S_ACCEPTED,
        "s=%r m=%r" % (first.get("s") if first else None, first.get("m") if first else None))
    data_frame = None
    for f in frames[1:]:
        if "data" in f or f.get("s") == S_OK:
            data_frame = f
    ok = data_frame is not None and data_frame.get("s") == S_OK
    detail = brief(data_frame)
    if ok:
        blob = json.dumps(data_frame, ensure_ascii=False)
        ok = "safety_timeout_sec" in blob
        detail = "含 safety_timeout_sec" if ok else "data 中无 safety_timeout_sec"
    rec("T3-20b", "config_query 第二帧携带 valve 配置", ok, detail)
    p.valve_cfg_frame = data_frame


# ---------------------------------------------------------------------------
# T4：紧急停止
# ---------------------------------------------------------------------------
def build_action(p, action_id, params=None):
    """用真实 registry 构造 c="action" 帧。返回 (frame, err)。"""
    ver, amap = p.act_ver, p.act_map
    if ver is None:
        return None, "registry 未取到"
    if action_id not in amap:
        return None, "registry 中无 %s" % action_id
    f = {"c": "action", "v": ver, "k": amap[action_id]}
    if params:
        f["p"] = params
    return f, None


def build_workflow(p, wf_id):
    if p.wf_ver is None:
        return None, "workflow registry 未取到"
    if wf_id not in p.wf_map:
        return None, "workflow registry 中无 %s" % wf_id
    return {"c": "workflow", "v": p.wf_ver, "k": p.wf_map[wf_id]}, None


def run_t4(p, allow_actuators):
    print("\n===== T4 紧急停止 =====")

    # ---- T4-1 空闲 stop_all
    ack, r, _rs = p.send("wbt4-01", {"c": "safety", "p": {"o": "stop_all"}}, wait=14.0)
    if r is None:
        rec("T4-01", "空闲 stop_all", False, "无 result")
    else:
        rec("T4-01", "空闲 stop_all", r.get("s") == S_OK,
            "s=%r all_ok=%r latched=%r" % (r.get("s"), r.get("all_ok"), r.get("latched")))
        rec("T4-01b", "stop_all 置 latched=true", r.get("latched") is True,
            "latched=%r" % (r.get("latched"),))
        rec("T4-01c", "response 显式声明 physical_verified=false",
            r.get("physical_verified") is False, "physical_verified=%r" % (r.get("physical_verified"),))
        act = r.get("actuators") or {}
        rec("T4-01d", "逐执行器 verified 均为 false（无电气回读）",
            len(act) >= 3 and all(isinstance(v, dict) and v.get("verified") is False for v in act.values()),
            brief(act))

    # ---- T4-2 锁存期间"启动类"Action 必须被拒（不会真正驱动硬件）
    for case, act_id in [
        ("T4-02", "VALVE_OPEN"),
        ("T4-03", "STEPPER_ROTATE_STEPS"),
        ("T4-04", "VIBRATION_START"),
        ("T4-05", "TEST_STEPPER_START"),
        # 2026-10-23 裁决：这两项已移出白名单，锁存期间必须被拒
        ("T4-05b", "MI_THERMO_START_SCAN"),
        ("T4-05c", "WEIGHT_ZERO"),
    ]:
        frame, err = build_action(p, act_id)
        if frame is None:
            rec(case, "锁存中 %s 被拒" % act_id, False, err, "BLOCKED")
            continue
        ack, r, _rs = p.send("wb" + case.lower().replace("-", ""), frame, wait=10.0)
        if r is None:
            rec(case, "锁存中 %s 被拒" % act_id, False, "无 result")
        else:
            msg = str(r.get("m", ""))
            ok = r.get("s") == S_ERROR and ("latch" in msg.lower() or "emergency" in msg.lower())
            rec(case, "锁存中 %s 被拒" % act_id, ok, "s=%r m=%r" % (r.get("s"), msg))

    # ---- T4-6 锁存中停止类仍被允许
    frame, err = build_action(p, "VALVE_CLOSE")
    if frame is None:
        rec("T4-06", "锁存中 VALVE_CLOSE 允许", False, err, "BLOCKED")
    else:
        ack, r, _rs = p.send("wbt4-06", frame, wait=10.0)
        rec("T4-06", "锁存中 VALVE_CLOSE 允许", r is not None and r.get("s") in (S_OK, S_ACCEPTED),
            "s=%r m=%r" % (r.get("s") if r else None, r.get("m") if r else None))

    # ---- T4-7 锁存中 Workflow 不得启动
    wf_id = None
    for k in p.wf_map:
        if k.startswith("wf_"):
            wf_id = k
            break
    if wf_id is None:
        rec("T4-07", "锁存中 Workflow 启动被拒", False, "registry 无 workflow", "BLOCKED")
    else:
        frame, err = build_workflow(p, wf_id)
        ack, r, _rs = p.send("wbt4-07", frame, wait=10.0)
        rec("T4-07", "锁存中 Workflow(%s) 启动被拒" % wf_id,
            r is not None and r.get("s") == S_ERROR,
            "s=%r m=%r" % (r.get("s") if r else None, r.get("m") if r else None))

    # ---- T4-8 clear 只解锁
    ack, r, _rs = p.send("wbt4-08", {"c": "safety", "p": {"o": "clear"}}, wait=10.0)
    if r is None:
        rec("T4-08", "safety.clear", False, "无 result")
    else:
        rec("T4-08", "safety.clear", r.get("s") == S_OK, "s=%r" % (r.get("s"),))
        rec("T4-08b", "clear 后 latched=false / was_latched=true",
            r.get("latched") is False and r.get("was_latched") is True,
            "latched=%r was=%r" % (r.get("latched"), r.get("was_latched")))

    # ---- T4-9 clear 后旧 workflow 不自动恢复
    ack, r, _rs = p.send("wbt4-09", {"c": "query", "p": {"q": "query_workflows"}}, wait=10.0)
    if r is None:
        rec("T4-09", "clear 后 workflow 未自动恢复", False, "无 result")
    else:
        runs = 0
        for item in (r.get("wf") or r.get("workflows") or []):
            if isinstance(item, dict) and str(item.get("state", "")).upper() in ("RUNNING", "WAITING"):
                runs += 1
        rec("T4-09", "clear 后无 RUNNING/WAITING 的 workflow", runs == 0, brief(r))

    # ---- T4-10 clear 后显式新操作恢复正常
    frame, err = build_action(p, "VALVE_CLOSE")
    ack, r, _rs = p.send("wbt4-10", frame, wait=10.0)
    rec("T4-10", "解锁后显式动作恢复受理", r is not None and r.get("s") in (S_OK, S_ACCEPTED),
        "s=%r" % (r.get("s") if r else None))

    if allow_actuators:
        run_t4_live(p)


def run_t4_live(p):
    """真实驱动执行器后急停。

    顺序刻意先振动后阀门：
      · 振动是单 GPIO、无水路、无重量耦合 —— 是"运行中急停"最干净的用例；
      · 阀门会放水，且 HX711 未接时 DispenseGuard 会随机强关 ⇒ 只做短时（2 s）开阀，
        并把 `was_active` 原样报出来（如果它已被强关，如实记为"未隔离"）。
    """
    print("\n===== T4-live 执行器运行中急停 =====")

    # ---- ① 振动：启动 → 急停
    frame, err = build_action(p, "VIBRATION_START", {"duration_ms": 0})
    if frame is None:
        rec("T4L-V1", "振动启动受理", False, err, "BLOCKED")
    else:
        ack, r, _ = p.send("wbt4lv-01", frame, wait=10.0)
        rec("T4L-V1", "振动启动受理", r is not None and r.get("s") in (S_OK, S_ACCEPTED),
            "s=%r m=%r" % (r.get("s") if r else None, r.get("m") if r else None))

        time.sleep(2.0)
        ack, q, _ = p.send("wbt4lv-02", {"c": "query", "p": {"q": "query_state", "o": "vibration_active"}}, wait=10.0)
        rec("T4L-V2", "振动运行中状态查询", q is not None, brief(q))

        ack, r, _ = p.send("wbt4lv-03", {"c": "safety", "p": {"o": "stop_all"}}, wait=14.0)
        if r is None:
            rec("T4L-V3", "振动运行中 stop_all", False, "无 result")
        else:
            rec("T4L-V3", "振动运行中 stop_all 返回 success", r.get("s") == S_OK,
                "s=%r all_ok=%r" % (r.get("s"), r.get("all_ok")))
            vib = (r.get("actuators") or {}).get("vibration") or {}
            rec("T4L-V4", "振动处置 note=stopped（确实停了正在转的马达）",
                vib.get("ok") is True and vib.get("was_active") is True and vib.get("note") == "stopped",
                brief(vib))

        p.send("wbt4lv-04", {"c": "safety", "p": {"o": "clear"}}, wait=10.0)

    # ---- ② 阀门：短时开阀 → 急停（放水，需现场有人）
    frame, err = build_action(p, "VALVE_OPEN")
    if frame is None:
        rec("T4L-01", "开阀受理", False, err, "BLOCKED")
        return
    ack, r, _ = p.send("wbt4l-01", frame, wait=10.0)
    rec("T4L-01", "开阀受理", r is not None and r.get("s") in (S_OK, S_ACCEPTED),
        "s=%r m=%r" % (r.get("s") if r else None, r.get("m") if r else None))

    time.sleep(2.0)
    ack, q, _ = p.send("wbt4l-02", {"c": "query", "p": {"q": "query_state", "o": "valve_status"}}, wait=10.0)
    rec("T4L-02", "开阀 2 s 后 valve_status（应仍为 true，否则说明被 DispenseGuard 强关）",
        q is not None, brief(q))

    ack, r, _ = p.send("wbt4l-03", {"c": "safety", "p": {"o": "stop_all"}}, wait=14.0)
    if r is None:
        rec("T4L-03", "运行中 stop_all", False, "无 result")
    else:
        rec("T4L-03", "运行中 stop_all", r.get("s") == S_OK, "s=%r" % (r.get("s"),))
        valve = (r.get("actuators") or {}).get("valve") or {}
        rec("T4L-03b", "阀处置 note=force_closed（证明急停关掉了正在开着的阀）",
            valve.get("ok") is True and valve.get("note") == "force_closed", brief(valve))

    time.sleep(1.5)
    ack, q, _ = p.send("wbt4l-04", {"c": "query", "p": {"q": "query_state", "o": "valve_status"}}, wait=10.0)
    rec("T4L-04", "急停后 valve_status 应为 false", q is not None and str(q.get("value")) == "false",
        brief(q))

    p.send("wbt4l-05", {"c": "safety", "p": {"o": "clear"}}, wait=10.0)


# ---------------------------------------------------------------------------
# T5：强制重启
# ---------------------------------------------------------------------------
def run_t5(p, confirm):
    print("\n===== T5 强制重启 =====")
    if not confirm:
        rec("T5-00", "强制重启（需 --confirm-force-restart）", False,
            "未显式确认，跳过", "BLOCKED")
        return

    ack, r, _rs = p.send("wbt5-01", {"c": "system", "p": {"o": "restart_status"}}, wait=10.0)
    rec("T5-01", "重启前 restart_status", r is not None, brief(r))

    ack, r, _rs = p.send("wbt5-02", {"c": "system", "p": {"o": "force_restart"}}, wait=12.0)
    rec("T5-02", "force_restart 受理", r is not None, brief(r))
    if r:
        rec("T5-02b", "status=rebooting(2) 且 boot_flag_written 为布尔",
            r.get("s") == S_RUNNING and isinstance(r.get("boot_flag_written"), bool),
            "s=%r boot_flag_written=%r critical_at_request=%r latched=%r" % (
                r.get("s"), r.get("boot_flag_written"),
                r.get("critical_at_request"), r.get("latched")))
        rec("T5-02c", "config_integrity_guaranteed=false 且 physical_verified=false",
            r.get("config_integrity_guaranteed") is False and r.get("physical_verified") is False,
            "cfg_guaranteed=%r phys=%r" % (r.get("config_integrity_guaranteed"), r.get("physical_verified")))

    # 等设备重启 + 重连（MQTT 自动重连，观察 /up 是否有新上行）
    print("  ... 等待设备重启（45 s）")
    time.sleep(45.0)
    ack, r, _rs = p.send("wbt5-03", {"c": "system", "p": {"o": "restart_status"}}, wait=12.0)
    rec("T5-03", "重启后设备恢复通信", r is not None, brief(r))


# ---------------------------------------------------------------------------
# T6：阀门配置读取（只读）
# ---------------------------------------------------------------------------
def run_t6_read(p):
    print("\n===== T6-a 阀门配置读取（只读） =====")
    ack, r, _rs = p.send("wbt6-01", {"c": "system", "p": {"o": "config_query", "module": "valve"}}, wait=12.0)
    rec("T6-01", "config_query valve", r is not None and r.get("s") == S_OK, brief(r))


# ---------------------------------------------------------------------------
# T4q：排队任务在急停时被取消（F-1 补强的 temp_cancelled 路径）
# ---------------------------------------------------------------------------
def run_t4q(p):
    print("\n===== T4q 排队任务取消 =====")
    # ① 先起一个长动作占住队头（振动 8 s，安全、无水路）
    frame, err = build_action(p, "VIBRATION_START", {"duration_ms": 8000})
    if frame is None:
        rec("T4Q-01", "占位长动作（振动 8 s）", False, err, "BLOCKED")
        return
    ack, r, _ = p.send("wbt4q-01", frame, wait=10.0)
    rec("T4Q-01", "占位长动作（振动 8 s）受理", r is not None and r.get("s") in (S_OK, S_ACCEPTED),
        "s=%r" % (r.get("s") if r else None))
    time.sleep(1.0)

    # ② 队头未完成 ⇒ 第二条必然排队
    fc, err = build_action(p, "VALVE_CLOSE")
    ack, r, _ = p.send("wbt4q-02", fc, wait=2.0)
    rec("T4Q-02", "第二条命令入队（等待队头）", r is not None, "s=%r" % (r.get("s") if r else None))

    # ③ 急停：应把排队项取消并回调失败
    ack, r, _ = p.send("wbt4q-03", {"c": "safety", "p": {"o": "stop_all"}}, wait=14.0)
    if r is None:
        rec("T4Q-03", "急停 stop_all", False, "无 result")
    else:
        rec("T4Q-03", "急停 stop_all 返回 success", r.get("s") == S_OK, "s=%r" % (r.get("s"),))
        rec("T4Q-04", "temp_cancelled >= 1（排队任务被取消）",
            int(r.get("temp_cancelled") or 0) >= 1, "temp_cancelled=%r" % (r.get("temp_cancelled"),))
        vib = (r.get("actuators") or {}).get("vibration") or {}
        rec("T4Q-05", "振动同时被停（note=stopped）",
            vib.get("ok") is True and vib.get("note") == "stopped", brief(vib))

    # ④ 取消的排队项必须给云端一个失败终态（不是静默消失）
    time.sleep(1.5)
    late = [f for f in p.frames if f.get("i") == "wbt4q-02" and (f.get("c") or f.get("cmd")) == "result"]
    rec("T4Q-06", "被取消的排队命令收到失败终态（s=3）",
        len(late) >= 1 and late[-1].get("s") == S_FAILED,
        "帧数=%d 最后一帧=%s" % (len(late), brief(late[-1]) if late else "无"))

    p.send("wbt4q-04", {"c": "safety", "p": {"o": "clear"}}, wait=10.0)


# ---------------------------------------------------------------------------
# T7：振动定时停止 / 步进（阻塞）
# ---------------------------------------------------------------------------
def run_t7(p):
    print("\n===== T7 振动定时停止 + 步进状态 =====")

    # ---- ① 定时振动：3000 ms 后应自动关闭
    frame, err = build_action(p, "VIBRATION_START", {"duration_ms": 3000})
    if frame is None:
        rec("T7-01", "定时振动（3000 ms）受理", False, err, "BLOCKED")
    else:
        ack, r, _ = p.send("wbt7-01", frame, wait=10.0)
        rec("T7-01", "定时振动（3000 ms）受理", r is not None and r.get("s") in (S_OK, S_ACCEPTED),
            "s=%r" % (r.get("s") if r else None))

        time.sleep(1.0)
        ack, q1, _ = p.send("wbt7-02", {"c": "query", "p": {"q": "query_state", "o": "vibration_active"}}, wait=10.0)
        rec("T7-02", "1 s 后振动仍在运行（vibration_active=true）",
            q1 is not None and str(q1.get("value")) == "true",
            "value=%r" % (q1.get("value") if q1 else None))

        time.sleep(4.0)   # 累计 5 s > 3 s
        ack, q2, _ = p.send("wbt7-03", {"c": "query", "p": {"q": "query_state", "o": "vibration_active"}}, wait=10.0)
        rec("T7-03", "5 s 后振动已自动停止（vibration_active=false）",
            q2 is not None and str(q2.get("value")) == "false",
            "value=%r" % (q2.get("value") if q2 else None))

    # ---- ② 持续振动 + 显式停止
    frame, err = build_action(p, "VIBRATION_START", {"duration_ms": 0})
    if frame is not None:
        ack, r, _ = p.send("wbt7-04", frame, wait=10.0)
        rec("T7-04", "持续振动受理", r is not None and r.get("s") in (S_OK, S_ACCEPTED),
            "s=%r" % (r.get("s") if r else None))
        time.sleep(1.5)
        fr2, err2 = build_action(p, "VIBRATION_STOP")
        if fr2 is not None:
            ack, r, _ = p.send("wbt7-05", fr2, wait=10.0)
            rec("T7-05", "VIBRATION_STOP 受理", r is not None, "s=%r" % (r.get("s") if r else None))
            time.sleep(1.0)
            ack, q, _ = p.send("wbt7-06", {"c": "query", "p": {"q": "query_state", "o": "vibration_active"}}, wait=10.0)
            rec("T7-06", "停止后 vibration_active=false",
                q is not None and str(q.get("value")) == "false",
                "value=%r" % (q.get("value") if q else None))
    else:
        rec("T7-04", "持续振动受理", False, err, "BLOCKED")

    # ---- ③ 步进：DRV8825 损坏 ⇒ 不驱动（按用户指令不得带载测试）
    rec("T7-07", "步进电机带载/转动验证", False,
        "DRV8825 已损坏、替换件与接线未确认 ⇒ 按任务规则不执行", "BLOCKED")
    rec("T7-08", "TEST_STEPPER 启停验证", False,
        "与步进共用 STEP/EN 引脚，驱动未确认前不执行", "BLOCKED")


# ---------------------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("phase", choices=["t3", "t4", "t4live", "t4q", "t5", "t6read", "t7", "all"])
    ap.add_argument("--device-id", required=True)
    ap.add_argument("--json", default="")
    ap.add_argument("--allow-actuators", action="store_true")
    ap.add_argument("--confirm-force-restart", action="store_true")
    args = ap.parse_args()

    p = Probe(args.device_id, args.phase).connect()
    try:
        # 动作/工作流帧必须依赖真实 registry ⇒ 先取（这也顺带验证 F-4）
        p.act_ver, p.act_map = fetch_registry(p, 0, "wbfetch-a")
        p.wf_ver, p.wf_map = fetch_registry(p, 2, "wbfetch-w")
        print("[REG] action v=%r n=%d | workflow v=%r n=%d" % (
            p.act_ver, len(p.act_map), p.wf_ver, len(p.wf_map)))
        print("[REG] actions = %s" % sorted(p.act_map.keys()))
        print("[REG] workflows = %s" % sorted(p.wf_map.keys()))

        if args.phase == "t3":
            run_t3(p)
        elif args.phase == "t4":
            run_t4(p, args.allow_actuators)
        elif args.phase == "t4live":
            run_t4_live(p)
        elif args.phase == "t4q":
            run_t4q(p)
        elif args.phase == "t5":
            run_t5(p, args.confirm_force_restart)
        elif args.phase == "t6read":
            run_t6_read(p)
        elif args.phase == "t7":
            run_t7(p)
        elif args.phase == "all":
            run_t3(p)
            run_t4(p, args.allow_actuators)
            run_t6_read(p)
    finally:
        p.close()

    print("\n===== 汇总 =====")
    for status in ("PASS", "FAIL", "NOT VERIFIED", "BLOCKED"):
        n = sum(1 for r in RESULTS if r["status"] == status)
        print("  %-13s %d" % (status, n))

    if args.json:
        with open(args.json, "w", encoding="utf-8", newline="\n") as f:
            json.dump(RESULTS, f, ensure_ascii=False, indent=2)
        print("  -> %s" % args.json)

    return 0 if not any(r["status"] == "FAIL" for r in RESULTS) else 1


if __name__ == "__main__":
    sys.exit(main())
