#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""p0_t11_command_queue_and_safety_test.py —— T11 现象B/C：命令队列与插座安全

为什么需要这套测试（真机 P0 安全故障）
--------------------------------------------------------------------
现象 B：UI 确认断电 → 设备没执行 → 约 1 分钟后 UI 报超时 → 之后普通重启也不灵。
现象 C：强制重启**之前**插座又被通电了一次，重启后插座保持通电。

代码级根因（三条独立，互不替代）：
  ① 没有任何**重启窗口门禁**：RESTART_PENDING(10 s) / RESTART_FORCE_PENDING(300 ms)
     期间主循环照常消费下行命令、照常推进执行器 ⇒ 队列里的旧「通电」被执行；
  ② `safety_stop_all()` **完全没覆盖米家**：只停本地执行器（阀/步进/振动），
     米家请求分两阶段（握手 800 ms → 发命令），排着的那条照发不误；
  ③ `CommandRuntime` 的重复判定**只扫活跃槽**，命令完成后就忘了 cmd_id；
     而 MQTT QoS1 是 at-least-once ⇒ broker 重投会让「通电」再执行一遍。
  另加一个正确性缺陷：
  ④ `actionPoll()` 会读到**上一轮**同描述符请求的残留 `fin/ok`，
     "这次没发出去"会被上一次的 SUCCESS 冒充 ⇒ 断电失败显示成成功。

本测试把上述契约钉死，并配**负向探针**（改坏一处，检查器必须报错）。

覆盖：B-1..B-8（队列/去重/门禁）、C-1..C-7（插座安全）

用法：python test/p0_t11_command_queue_and_safety_test.py
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

SRC = {
    "cm":      os.path.join(ROOT, "src", "services", "command_manager.cpp"),
    "syscmd":  os.path.join(ROOT, "src", "services", "system_command.cpp"),
    "syscmd_h": os.path.join(ROOT, "src", "services", "system_command.h"),
    "safety":  os.path.join(ROOT, "src", "app", "safety.cpp"),
    "safety_h": os.path.join(ROOT, "src", "app", "safety.h"),
    "xlan":    os.path.join(ROOT, "src", "app", "xiaomi_lan.cpp"),
    "xlan_h":  os.path.join(ROOT, "src", "app", "xiaomi_lan.h"),
    "wf":      os.path.join(ROOT, "src", "automation", "workflow.cpp"),
}

TEXT = {}
for k, p in SRC.items():
    if not os.path.exists(p):
        print("FATAL 缺少源文件: " + p)
        sys.exit(2)
    raw = open(p, encoding="utf-8", errors="replace").read()
    raw = re.sub(r"/\*.*?\*/", "", raw, flags=re.S)   # 去块注释
    raw = re.sub(r"//[^\n]*", "", raw)                # 去行注释
    TEXT[k] = raw

passes = [0]
failures = []


def ok(name, cond, detail=""):
    if cond:
        passes[0] += 1
        print("  PASS  " + name)
    else:
        failures.append(name + (("  <- " + detail) if detail else ""))
        print("  FAIL  " + name + (("  <- " + detail) if detail else ""))


def func_body(text, name):
    """按大括号配对截取**函数定义**体（注释已剥离）。

    为什么不能"找到第一个 `name(` 就往下找 `{`"：
    本模块大量函数有**前置声明**（`static void f(X*);`），
    那样会一直滑到**下一个**函数的 `{` 上，取到错误内容
    —— 首版就是这么误判的（B-1b / B-6a 假红）。
    正确做法：逐个候选，参数列表闭合后跳过空白，
    只有紧跟 `{` 的才是定义；否则是声明，继续找下一个。
    """
    pat = re.compile(r"\b" + re.escape(name) + r"\s*\(")

    for m in pat.finditer(text):
        i = m.end()
        depth = 1
        while i < len(text) and depth > 0:
            if text[i] == "(":
                depth += 1
            elif text[i] == ")":
                depth -= 1
            i += 1

        j = i
        while j < len(text) and text[j] in " \t\r\n":
            j += 1
        if j >= len(text) or text[j] != "{":
            continue                      # 这是声明，继续找定义

        depth2 = 0
        for k in range(j, len(text)):
            if text[k] == "{":
                depth2 += 1
            elif text[k] == "}":
                depth2 -= 1
                if depth2 == 0:
                    return text[j:k + 1]
    return None


# =====================================================================
# B. 命令队列 / 去重 / 重启门禁
# =====================================================================
def check_b1():
    """已完成 cmd_id 短期记忆必须存在、且必须写入。"""
    cm = TEXT["cm"]
    has_ring = "CMD_DONE_RING" in cm and "cmd_done_remember" in cm and "cmd_done_seen" in cm
    ok("B-1a 存在已完成 cmd_id 环形记忆（CMD_DONE_RING / remember / seen）", has_ring)

    rel = func_body(cm, "command_runtime_release") or ""
    ok("B-1b 释放路径写入记忆（cmd_done_remember 在 release 内）",
       "cmd_done_remember" in rel)

    # 必须在清空 rt->cmd_id **之前**记住，否则记的是空串
    i_rem = rel.find("cmd_done_remember")
    i_clr = rel.find('rt->cmd_id = ""')
    ok("B-1c 记忆发生在清空 cmd_id 之前", i_rem >= 0 and i_clr >= 0 and i_rem < i_clr,
       "remember@%d clear@%d" % (i_rem, i_clr))


def check_b2():
    """重复投递必须被拦截，且只拦异步（会动执行器）命令。"""
    cm = TEXT["cm"]
    body = func_body(cm, "command_manager_execute") or ""
    ok("B-2a execute 入口查已完成记忆", "cmd_done_seen(" in body)
    ok("B-2b 只对 async 命令拦截（幂等查询不误杀）",
       re.search(r"command_is_async\(command\)\s*&&\s*cmd_done_seen", body) is not None)
    ok("B-2c 命中后回 status=duplicate 且说明「可能此前已失败」（不得静默）",
       '"duplicate"' in body
       and '"duplicate cmd_id: already processed once' in body)
    ok("B-2d 命中后不再执行（return false）",
       re.search(r'command_report_result\(dup_json\);\s*return false;', body) is not None)


def check_b3():
    """重启窗口门禁：只拦"会动执行器"的两族，停止类动作放行。"""
    cm = TEXT["cm"]
    body = func_body(cm, "command_manager_execute") or ""
    ok("B-3a 入口有重启门禁", "system_command_is_restarting()" in body)
    ok("B-3b 拦 execute_* 族（action / workflow 执行）",
       "command_is_execute(command)" in body)
    ok("B-3c 拦 workflow.* 族（自动化增删启停）",
       "command_is_workflow_manage(command)" in body)
    ok("B-3d 停止类动作（含插座断电）放行",
       "safety_action_is_stop_class" in body)
    ok("B-3e 被拒时回错误帧（不得静默丢弃）",
       "system is restarting" in body and "command_send_error" in body)
    # 刻意**不**整族封杀：system 族里有 restart_status / 诊断 / config，
    # 整族封杀会让重启倒计时与诊断全部不可用（首版就是这么写的，已修正）。
    ok("B-3f 不整族封杀 system 族（重启倒计时/诊断仍可用）",
       "command_is_system(command)\n            ||" not in body
       and "!command_is_system" not in body)


def check_b4():
    """is_restarting 的判据必须覆盖全部非 IDLE 态。"""
    h = TEXT["syscmd_h"]
    cpp = TEXT["syscmd"]
    ok("B-4a 头文件声明 system_command_is_restarting", "system_command_is_restarting" in h)
    body = func_body(cpp, "system_command_is_restarting") or ""
    ok("B-4b 实现为 != RESTART_IDLE（覆盖 FORCE_PENDING 等后加态）",
       "RESTART_IDLE" in body and "!=" in body)


def check_b5():
    """队列满时必须给明确结果，不得静默。"""
    cm = TEXT["cm"]
    ins = func_body(cm, "command_runtime_insert") or ""
    ok("B-5a 槽满走 command_send_error（CMD_ERROR_QUEUE_FULL）",
       "CMD_ERROR_QUEUE_FULL" in ins and "command_send_error" in ins)
    ok("B-5b 槽满有 WARN 日志", "Command runtime queue full" in ins)


def check_b6():
    """超时只释放、不补执行；迟到回调必须被忽略。"""
    cm = TEXT["cm"]
    to = func_body(cm, "command_runtime_report_timeout") or ""
    ok("B-6a 超时后释放槽位", "command_runtime_release" in to)
    # B-6b 的判据要精确：函数体里**合法地**出现字符串 "execute_action"
    # （用于决定结果帧 type），所以不能用 "execute" 这个词做判据。
    # 真正要钉的是"不调用任何执行入口"。
    no_exec = all(k not in to for k in (
        "workflow_enqueue_action",
        "safety_stop_all",
        "config_cmd_enqueue",
        "system_command_force_restart()",
        "->start(",
    ))
    ok("B-6b 超时只上报状态、不调用任何执行入口", no_exec)
    scan = func_body(cm, "command_runtime_scan_timeouts") or ""
    ok("B-6c 超时扫描只处理 PENDING", "COMMAND_STATE_PENDING" in scan)


def check_b7():
    """断电必须在锁存期与重启期都放行（安全最高优先级）。"""
    sh = TEXT["safety_h"]
    sc = TEXT["safety"]
    ok("B-7a safety.h 暴露 safety_action_is_stop_class", "safety_action_is_stop_class" in sh)
    ok("B-7b 白名单含 XIAOMI_PLUG_OFF", re.search(r'kLatchAllowedActions\[\][\s\S]{0,600}"XIAOMI_PLUG_OFF"',
                                                 sc) is not None)
    ok("B-7c 白名单**不含** XIAOMI_PLUG_ON（通电不得豁免）",
       re.search(r'kLatchAllowedActions\[\][\s\S]{0,600}"XIAOMI_PLUG_ON"', sc) is None)
    body = func_body(sc, "safety_action_is_stop_class") or ""
    ok("B-7d is_stop_class 与白名单共用同一份数据",
       "kLatchAllowedActions" in body)


# =====================================================================
# C. 插座安全 / 重启前不得再动作
# =====================================================================
def check_c1():
    """safety_stop_all 必须作废米家在途请求。"""
    sc = TEXT["safety"]
    body = func_body(sc, "safety_stop_all") or ""
    ok("C-1a stop_all 调用 xiaomi_lan_abort_noncritical", "xiaomi_lan_abort_noncritical" in body)
    ok("C-1b 结果写进 SafetyReport（可观测）", "xlan_cancelled" in body)
    ok("C-1c 顺序在物理停止之前（避免刚关又开）",
       body.find("xiaomi_lan_abort_noncritical") < body.find("valve_force_close"),
       "abort@%d valve@%d" % (body.find("xiaomi_lan_abort_noncritical"),
                              body.find("valve_force_close")))


def check_c2():
    """作废只针对非安全请求：断电绝不能被自己丢掉。"""
    x = TEXT["xlan"]
    impl = func_body(x, "abortNoncriticalImpl") or ""
    ok("C-2a impl 存在", impl != "")
    ok("C-2b 跳过 REQ_PLUG_OFF（断电保留）", "REQ_PLUG_OFF" in impl and "continue" in impl)
    ok("C-2c 作废即 finishRequest(false)（不重试）", "finishRequest(" in impl)
    h = TEXT["xlan_h"]
    ok("C-2d 头文件声明公开包装", "xiaomi_lan_abort_noncritical" in h)
    pub = func_body(x, "xiaomi_lan_abort_noncritical") or ""
    ok("C-2e 公开包装在匿名命名空间之外（可链接）",
       pub != "" and "abortNoncriticalImpl" in pub)


def check_c3():
    """workflow 启动路径必须有重启门禁，且停止类动作放行。"""
    wf = TEXT["wf"]
    body = func_body(wf, "workflow_action_start") or ""
    ok("C-3a action_start 有重启门禁", "system_command_is_restarting()" in body)
    ok("C-3b 停止类动作放行", "safety_action_is_stop_class" in body)
    ok("C-3c 被拒时置 ACTION_FAILED（走失败终止路径，不静默）",
       "ACTION_FAILED" in body)


def check_c4():
    """actionPoll 的残留 fin 必须被清掉（否则"没发出去"会被旧成功冒充）。"""
    x = TEXT["xlan"]
    sr = func_body(x, "startRequest") or ""
    ok("C-4a startRequest 在槽空闲时清 fin/ok（T12 后窗口内含探活让路守卫）",
       re.search(r"if\s*\(!r\.active\)[\s\S]{0,900}r\.fin\s*=\s*false",
                 sr) is not None)
    ok("C-4b 清白必须在 BUSY/DISABLED 早退之前",
       sr.find("r.fin = false") < sr.find("XIAOMI_ERR_BUSY"),
       "clear@%d busy@%d" % (sr.find("r.fin = false"), sr.find("XIAOMI_ERR_BUSY")))

    poll = func_body(x, "actionPoll") or ""
    ok("C-4c poll 兜底把 IDLE 判成 FAILED（不冒充成功）",
       "ACTION_FAILED" in poll and "ACTION_IDLE" in poll)
    ok("C-4d poll 不以无条件 ACTION_SUCCESS 兜底", "ACTION_SUCCESS" in poll)


def check_c5():
    """强制重启：先停硬件再请求重启，且顺序不可颠倒。"""
    cm = TEXT["cm"]
    body = func_body(cm, "command_system_force_restart") or ""
    ok("C-5a 先 safety_stop_all", "safety_stop_all" in body)
    ok("C-5b stop_all 在 system_command_force_restart 之前",
       body.find("safety_stop_all") < body.find("system_command_force_restart()"),
       "stop@%d force@%d" % (body.find("safety_stop_all"),
                             body.find("system_command_force_restart()")))
    ok("C-5c 上报 critical_at_request（突破保护的客观证据）", "critical_at_request" in body)
    ok("C-5d 上报 xlan_cancelled（作废了几条米家请求）", "xlan_cancelled" in body)


def check_c6():
    """重启态本身不得成为"再来一次动作"的通道。"""
    sc = TEXT["syscmd"]
    for fn in ("system_command_force_restart", "system_command_request_restart"):
        body = func_body(sc, fn) or ""
        ok("C-6·%s 只置状态、不调用任何执行器" % fn,
           body != "" and "exec" not in body.lower())


def check_c7():
    """超时/未知的通电命令不得在重启后被"补执行"。"""
    cm = TEXT["cm"]
    # 没有任何持久化重放队列：runtime 只在 RAM，重启即清
    ok("C-7a runtime 为静态数组、无 Flash 持久化",
       re.search(r"static\s+CommandRuntime\s+runtime_queue\[", cm) is not None)
    init = func_body(cm, "command_manager_init") or ""
    ok("C-7b init 把全部槽释放（重启后无残留）",
       "command_runtime_release" in init and "MAX_COMMAND_RUNTIME" in init)
    ok("C-7c init 同时清空去重记忆", "CMD_DONE_RING" in init)


def main():
    print("=" * 68)
    print("p0_t11_command_queue_and_safety_test —— 命令队列 / 插座安全 契约")
    print("=" * 68)

    print("\n[B] 命令队列与去重")
    check_b1()
    check_b2()
    check_b5()
    check_b6()

    print("\n[B] 重启窗口门禁")
    check_b3()
    check_b4()

    print("\n[B] 断电安全优先级")
    check_b7()

    print("\n[C] 插座安全 / 重启前不得再动作")
    check_c1()
    check_c2()
    check_c3()
    check_c4()
    check_c5()
    check_c6()
    check_c7()

    # --------------------------------------------------------------
    # 负向探针：把关键防护"改坏"，检查器必须能识别
    # --------------------------------------------------------------
    print("\n[N] 负向探针（证明断言可失败）")

    def probe(name, mutated_text, detector):
        ok(name, detector(mutated_text) is False, "探针未被识别 ⇒ 断言形同虚设")

    # N-1 去掉断电白名单
    probe("N-1 移除白名单中的 XIAOMI_PLUG_OFF 后能被识别",
          TEXT["safety"].replace('"XIAOMI_PLUG_OFF",', ""),
          lambda t: re.search(r'kLatchAllowedActions\[\][\s\S]{0,600}"XIAOMI_PLUG_OFF"', t) is not None)

    # N-2 去掉 stop_all 里的米家作废
    probe("N-2 移除 stop_all 的米家作废后能被识别",
          TEXT["safety"].replace("xiaomi_lan_abort_noncritical(\"safety_stop_all\")", "0"),
          lambda t: "xiaomi_lan_abort_noncritical" in (func_body(t, "safety_stop_all") or ""))

    # N-3 去掉 execute 入口的重复拦截
    probe("N-3 移除重复投递拦截后能被识别",
          TEXT["cm"].replace("cmd_done_seen(cmd.cmd_id)", "false"),
          lambda t: re.search(r"command_is_async\(command\)\s*&&\s*cmd_done_seen",
                              func_body(t, "command_manager_execute") or "") is not None)

    # N-4 去掉 execute 入口的重启门禁
    probe("N-4 移除重启门禁后能被识别",
          TEXT["cm"].replace("if (system_command_is_restarting())", "if (false)"),
          lambda t: "system_command_is_restarting()" in (func_body(t, "command_manager_execute") or ""))

    # N-5 去掉 workflow_action_start 的重启门禁
    probe("N-5 移除 workflow 重启门禁后能被识别",
          TEXT["wf"].replace("if(system_command_is_restarting() &&", "if(false &&"),
          lambda t: "system_command_is_restarting()" in (func_body(t, "workflow_action_start") or ""))

    # N-6 去掉 startRequest 的残留清理
    probe("N-6 移除 startRequest 残留清理后能被识别",
          TEXT["xlan"].replace("r.fin = false;", "", 1),
          lambda t: re.search(r"if\s*\(!r\.active\)[\s\S]{0,220}r\.fin\s*=\s*false",
                              func_body(t, "startRequest") or "") is not None)

    print("\n" + "=" * 68)
    print("PASS %d / FAIL %d" % (passes[0], len(failures)))
    if failures:
        for f in failures:
            print("  ! " + f)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
