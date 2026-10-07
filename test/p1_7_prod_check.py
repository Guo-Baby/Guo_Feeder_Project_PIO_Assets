#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""P1-7 Production Topic Enforcement —— 静态调用路径 + 构型门控断言。

设计前提（见 docs/architecture/P1-7-Review-Report.md）：
  · production 判定 = 编译期宏 `GF_ENV_PRODUCTION`（默认 0），运行期不可改；
  · TopicRenderer 的 T-5（无占位符⇒原样放行）**不得**修改 ⇒ 策略判断放在 CloudManager；
  · 拦截点必须是 `cloud_connect()` 的**第一条语句**，位于 `esp_mqtt_client_init()` 之前。

本脚本**不需要硬件 / broker**，用两组客观证据替代 LIVE 验证：
  A. 源码级：守卫位置、`#if` 门控、T-5 未被触碰、platformio.ini 不含 `-D...`；
  B. 二进制级（`--bins`）：dev 构型**不含** production 分支字符串，prod 构型**含**。

⚠️ 所有"某 token 是否存在"的断言一律在**剥离注释后**的文本上求值 ——
   否则注释里提到 `esp_mqtt_client_init()` / `-DGF_ENV_PRODUCTION` 会造成假 FAIL。

用法：
    python test/p1_7_prod_check.py            # 仅 A
    python test/p1_7_prod_check.py --bins     # A + B（需先完成两种构建）

退出码：0 = 全部 PASS，1 = 存在 FAIL。
"""

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CM = os.path.join(ROOT, "src", "cloud", "cloud_manager.cpp")
TR_H = os.path.join(ROOT, "src", "services", "topic_renderer.h")
TR_C = os.path.join(ROOT, "src", "services", "topic_renderer.cpp")
PIO_INI = os.path.join(ROOT, "platformio.ini")

DEV_BIN = os.path.join(ROOT, ".pio", "build", "esp32-s3-devkitc-1", "firmware.bin")
PROD_BIN = os.path.join(ROOT, ".pio", "build", "p17prod", "esp32-s3-devkitc-1", "firmware.bin")

results = []


def check(name, ok, detail=""):
    results.append((name, bool(ok), detail))
    print("  [%s] %-52s %s" % ("PASS" if ok else "FAIL", name, detail))


def read(path):
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        return fh.read()


def strip_c_comments(text):
    """剥离 C/C++ 行注释与块注释；**保持字节位置与换行不变**，便于与原文行号对齐。"""
    out = []
    i, n = 0, len(text)
    while i < n:
        if text.startswith("//", i):
            j = text.find("\n", i)
            j = n if j < 0 else j
            out.append(" " * (j - i))
            i = j
        elif text.startswith("/*", i):
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            seg = text[i:j]
            out.append("".join("\n" if ch == "\n" else " " for ch in seg))
            i = j
        else:
            out.append(text[i])
            i += 1
    return "".join(out)


def strip_ini_comments(text):
    """剥离 ini 注释（; 或 # 开头的整行）；保持行数不变（空行占位）。"""
    lines = []
    for ln in text.splitlines():
        s = ln.lstrip()
        lines.append("" if (s.startswith(";") or s.startswith("#")) else ln)
    return "\n".join(lines)


def find_function_body(text, signature_regex):
    """返回 (起始行号(1-based), 函数体文本)。以行首签名定位，匹配到列 0 的 '}'。"""
    lines = text.splitlines()
    start = None
    for i, ln in enumerate(lines):
        if re.match(signature_regex, ln):
            start = i
            break
    if start is None:
        return None, None
    depth, began, end = 0, False, None
    for j in range(start, len(lines)):
        depth += lines[j].count("{") - lines[j].count("}")
        if "{" in lines[j]:
            began = True
        if began and depth <= 0:
            end = j
            break
    return start + 1, "\n".join(lines[start:end + 1] if end else lines[start:])


def main():
    cm = read(CM)
    ini = read(PIO_INI)
    cm_code = strip_c_comments(cm)          # 内容类断言用（无注释）
    ini_code = strip_ini_comments(ini)
    tr_h_code = strip_c_comments(read(TR_H))
    tr_c_code = strip_c_comments(read(TR_C))
    cm_lines = cm.splitlines()

    print("== A. 源码级断言（注释已剥离）==")

    # A1 宏定义与默认值
    check("A1 GF_ENV_PRODUCTION 默认 0（#ifndef 保护）",
          re.search(r"#ifndef\s+GF_ENV_PRODUCTION\s*\n\s*#define\s+GF_ENV_PRODUCTION\s+0", cm_code) is not None)

    # A2 platformio.ini 生效行不得定义该宏
    hit = re.search(r"-D\s*GF_ENV_PRODUCTION", ini_code)
    check("A2 platformio.ini 生效行不含 -DGF_ENV_PRODUCTION", hit is None,
          "" if hit is None else "发现: %s" % hit.group(0))

    # A3 T-5 未被触碰
    check("A3 topic_renderer.h/.cpp 不含 GF_ENV_PRODUCTION",
          ("GF_ENV_PRODUCTION" not in tr_h_code) and ("GF_ENV_PRODUCTION" not in tr_c_code))
    check("A3b topic_renderer 仍保留「不含占位符 ⇒ 原样放行 true」",
          "out = tmpl;" in tr_c_code and "T-5" in read(TR_H))

    # A4 cloud_connect() 守卫位置
    start, body = find_function_body(cm, r"^static bool cloud_connect\(\)\s*$")
    if body is None:
        check("A4 找到 cloud_connect() 定义", False, "未匹配到函数定义")
    else:
        check("A4 找到 cloud_connect() 定义 @L%d" % start, True)
        first_stmt = None
        for ln in body.splitlines()[1:]:
            s = ln.strip()
            if s in ("", "{") or s.startswith("//") or s.startswith("/*") or s.startswith("*"):
                continue
            first_stmt = s
            break
        check("A4b cloud_connect() 首条语句 = 前置检查早退",
              first_stmt is not None and first_stmt.startswith("if(s_mqtt_precheck_failed)"),
              "首条=%r" % first_stmt)

        guard_no = next((i + 1 for i, ln in enumerate(cm_lines)
                         if "if(s_mqtt_precheck_failed)" in ln and i + 1 >= start), None)
        init_no = next((i + 1 for i, ln in enumerate(cm_lines)
                        if "esp_mqtt_client_init(&cfg)" in ln), None)
        start_no = next((i + 1 for i, ln in enumerate(cm_lines)
                         if "esp_mqtt_client_start(mqtt_client)" in ln), None)
        check("A4c guard < esp_mqtt_client_init()（真实调用处）",
              bool(guard_no and init_no) and guard_no < init_no,
              "guard=L%s init=L%s start=L%s" % (guard_no, init_no, start_no))
        check("A4d guard < esp_mqtt_client_start()（真实调用处）",
              bool(guard_no and start_no) and guard_no < start_no)
        check("A4e guard 在 if(!wifi_connected) 早退之前",
              "if(s_mqtt_precheck_failed)" in body and
              body.index("if(s_mqtt_precheck_failed)") < body.index("if(!wifi_connected)"))

    # A5 唯一 MQTT 入口（注释已剥离 ⇒ 只统计真实调用）
    n_init = len(re.findall(r"esp_mqtt_client_init\s*\(", cm_code))
    n_start = len(re.findall(r"esp_mqtt_client_start\s*\(", cm_code))
    check("A5 esp_mqtt_client_init/start 各仅 1 处（真实调用）",
          n_init == 1 and n_start == 1, "init=%d start=%d" % (n_init, n_start))

    # A6 production 策略块：三处模板 + #if 门控 + 在 cloud_init() 内
    pol = re.search(r"#if\s+GF_ENV_PRODUCTION(.*?)#endif", cm_code, re.S)
    pol_body = pol.group(1) if pol else ""
    check("A6 production 策略块存在且被 #if GF_ENV_PRODUCTION 包裹", pol is not None)
    for what in ("subscribe_topic", "publish_topic", "log_topic"):
        check("A6b %s 参与 production 判定" % what, what in pol_body)
    check("A6c 策略块位于 cloud_init() 内",
          "production policy" in find_function_body(cm_code, r"^void cloud_init\(\)\s*$")[1])
    check("A6d 判定的是**模板**（config_get_mqtt_*）而非渲染结果",
          "config_get_mqtt_subscribe_topic().indexOf(GF_DEVICE_ID_PLACEHOLDER)" in pol_body)

    # A7 F-3 空串契约：任何模式都把空 topic/client_id 拦在 init 之前
    check("A7 F-3 空串检查存在（config error: ...unresolved）",
          all(s in cm_code for s in ("config error: subscribe_topic unresolved",
                                     "config error: publish_topic unresolved",
                                     "config error: client_id unresolved")))
    check("A7b 空串检查**不在** #if GF_ENV_PRODUCTION 内（任何模式生效）",
          pol is not None and "config error: subscribe_topic unresolved" not in pol_body)

    # A8 未新增 System State / EventId
    bad_events = [n for n in ("LOG_P17", "LOG_PROD_ENFORCE", "LOG_ENV_PRODUCTION") if n in cm_code]
    check("A8 未新增 EventId", not bad_events, ",".join(bad_events))
    bad_states = [n for n in ("STATE_P17", "STATE_PRODUCTION", "STATE_ENV_MODE") if n in cm_code]
    check("A8b 未新增 System State（阻断标志仅模块内 static）",
          (not bad_states) and re.search(r"^static bool s_mqtt_precheck_failed", cm_code, re.M) is not None,
          ",".join(bad_states))

    # A9 无阻塞原语引入
    blk = re.search(r"if\(s_mqtt_precheck_failed\)\s*\{(.*?)\}", cm_code, re.S)
    seg = blk.group(1) if blk else ""
    check("A9 守卫体内无 delay()/while/vTaskDelay",
          seg != "" and not re.search(r"delay\(|vTaskDelay|while\s*\(", seg))

    if "--bins" in sys.argv:
        print("== B. 二进制级门控断言 ==")
        for label, path, want in (("dev", DEV_BIN, 0), ("prod", PROD_BIN, 3)):
            if not os.path.exists(path):
                check("B[%s] 产物存在" % label, False, path)
                continue
            with open(path, "rb") as fh:
                data = fh.read()
            n_prod = data.count(b"production policy")
            check("B[%s] 'production policy' 出现 %d 次（期望 %d）" % (label, n_prod, want), n_prod == want)
            check("B[%s] F-3 'config error:' 字符串存在" % label,
                  data.count(b"config error: subscribe_topic") >= 1)
            if label == "dev":
                check("B[dev] production 分支已被 --gc-sections 剔除",
                      n_prod == 0 and data.count(b"MQTT blocked") == 1)
            else:
                check("B[prod] production enforcement 已编入固件",
                      n_prod == 3 and data.count(b"MQTT blocked") == 1)

    fails = [r for r in results if not r[1]]
    print("\n===== 汇总：%d PASS / %d FAIL =====" % (len(results) - len(fails), len(fails)))
    for name, _, detail in fails:
        print("  FAIL: %s  %s" % (name, detail))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
