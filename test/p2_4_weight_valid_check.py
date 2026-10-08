#!/usr/bin/env python3
# =====================================================
# P2-4 阶段 A · Weight enable/valid 语义 —— 静态结构校验
#
# 目标：断言「weight.enable=false ⇒ STATE_WEIGHT_VALID=false」这一
#       语义在**源码层面**被正确、唯一地实现，且不越界。
#
# 覆盖：
#   A) System State 新增 STATE_WEIGHT_VALID（枚举 + state_map）
#   B) weight_init() 的 !enable 分支：显式置 VALID=false 且早于 return
#   C) weight_init() 的启用分支：显式置 VALID=true
#   D) 唯一生产者 = weight 模块（其他模块不得写该键）
#   E) Device Model / Homie 未被修改（不引用该键）
#   F) 启动态残留消除不变量：system_state_init() 清零全部 valid
#   G) 禁用 ⇒ weight_task 早退（不采样、不发事件）
#   H) 采样/滤波路径未被无关重构
#
# 用法: python test/p2_4_weight_valid_check.py
# 退出码: 0 = 全部 PASS；1 = 有 FAIL
# =====================================================
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "src")

results = []


def check(name, ok, detail=""):
    results.append((name, bool(ok), detail))


def read(rel):
    with open(os.path.join(ROOT, rel), "r", encoding="utf-8") as f:
        return f.read()


def strip_code(text):
    """去掉注释与字符串/字符字面量内容，避免字面量/注释造成假命中。"""
    out = []
    i = 0
    n = len(text)
    while i < n:
        c = text[i]
        if c == '/' and i + 1 < n and text[i + 1] == '/':
            while i < n and text[i] != '\n':
                i += 1
            continue
        if c == '/' and i + 1 < n and text[i + 1] == '*':
            i += 2
            while i + 1 < n and not (text[i] == '*' and text[i + 1] == '/'):
                i += 1
            i += 2
            continue
        if c == '"' or c == "'":
            quote = c
            i += 1
            while i < n and text[i] != quote:
                if text[i] == '\\':
                    i += 1
                i += 1
            i += 1
            out.append('""')
            continue
        out.append(c)
        i += 1
    return "".join(out)


def extract_body(src, needle):
    """取 needle 之后第一对花括号之间的完整块（含 needle）。"""
    i = src.find(needle)
    if i < 0:
        return ""
    j = src.find('{', i)
    if j < 0:
        return ""
    depth = 0
    k = j
    while k < len(src):
        if src[k] == '{':
            depth += 1
        elif src[k] == '}':
            depth -= 1
            if depth == 0:
                return src[i:k + 1]
        k += 1
    return src[i:]


def gather_src():
    files = {}
    for dirpath, _dirs, names in os.walk(SRC):
        for nm in names:
            if nm.endswith((".cpp", ".h", ".c", ".hpp")):
                p = os.path.join(dirpath, nm)
                rel = os.path.relpath(p, ROOT).replace("\\", "/")
                with open(p, "r", encoding="utf-8", errors="replace") as f:
                    files[rel] = f.read()
    return files


FILES = gather_src()

ss_h = read("src/services/system_state.h")
ss_cpp = read("src/services/system_state.cpp")
weight_cpp = read("src/app/weight.cpp")
dm_cpp = read("src/homie/device_model.cpp")
bridge_cpp = read("src/homie/homie_bridge.cpp")
proj_h = read("src/homie/homie_projection.h")

ss_h_code = strip_code(ss_h)
ss_cpp_code = strip_code(ss_cpp)
weight_code = strip_code(weight_cpp)

# -----------------------------------------------------
# A) System State 新增 STATE_WEIGHT_VALID
# -----------------------------------------------------
check("A1: system_state.h 定义 STATE_WEIGHT_VALID",
      "STATE_WEIGHT_VALID" in ss_h_code)

# 位置：紧邻 STATE_WEIGHT_ERROR（同一"重量"分组）
i_err = ss_h_code.find("STATE_WEIGHT_ERROR")
i_val = ss_h_code.find("STATE_WEIGHT_VALID")
check("A2: STATE_WEIGHT_VALID 紧随 STATE_WEIGHT_ERROR",
      i_err >= 0 and i_val > i_err)

# 枚举成员必须出现在 STATE_MAX 之前（否则越界）
i_max = ss_h_code.find("STATE_MAX")
check("A3: STATE_WEIGHT_VALID 在 STATE_MAX 之前（不越界）",
      i_val >= 0 and i_max > i_val)

# state_map 注册行：名字 weight_valid / 类型 BOOL
#   注意：strip_code() 会把字符串字面量替换为 "" ⇒ 必须用**原始**源码查名字。
i_row = ss_cpp.find("weight_valid")
row = ""
if i_row >= 0:
    i_end = ss_cpp.find("}", i_row)
    row = ss_cpp[i_row:i_end if i_end > i_row else i_row + 120]
check("A4: state_map 注册 weight_valid → STATE_WEIGHT_VALID",
      i_row >= 0 and "STATE_WEIGHT_VALID" in row.replace(" ", ""))
check("A5: state_map 中 weight_valid 类型 = STATE_TYPE_BOOL",
      "STATE_TYPE_BOOL" in row.replace(" ", ""))

# -----------------------------------------------------
# B) weight_init() 的 !enable 分支
# -----------------------------------------------------
init_body = extract_body(weight_code, "void weight_init(")
check("B0: 找到 weight_init() 函数体", len(init_body) > 0)

# 取 !enable 分支：从 "if(!enable)" 到其后第一个 "return;" 之前
i_en = init_body.find("if(!enable)")
seg = ""
if i_en >= 0:
    i_ret = init_body.find("return;", i_en)
    seg = init_body[i_en:i_ret if i_ret > i_en else len(init_body)]

check("B1: !enable 分支存在",
      "if(!enable)" in init_body)
check("B2: !enable 分支显式置 STATE_WEIGHT_VALID = false",
      "state_set_bool(STATE_WEIGHT_VALID,false)" in seg.replace(" ", ""))
check("B3: 该置位早于 !enable 分支的 return（不在其后）",
      "state_set_bool(STATE_WEIGHT_VALID,false)" in seg.replace(" ", ""))

# -----------------------------------------------------
# C) 启用分支显式置 true
# -----------------------------------------------------
check("C1: 启用路径置 STATE_WEIGHT_VALID = true",
      "state_set_bool(STATE_WEIGHT_VALID,true)" in init_body.replace(" ", "")
      or "state_set_bool(STATE_WEIGHT_VALID, true)" in init_body)
# true 置位必须在 initialized = true 之前（初始化完成即声明可用）
i_true = init_body.replace(" ", "").find("state_set_bool(STATE_WEIGHT_VALID,true)")
i_init = init_body.replace(" ", "").find("initialized=true")
check("C2: VALID=true 早于 initialized=true",
      i_true >= 0 and i_init > i_true)

# -----------------------------------------------------
# D) 唯一生产者 = weight 模块
# -----------------------------------------------------
# system_state.{h,cpp} 是「定义处」（枚举 + map），不算生产者；
# 生产者 = 调用 state_set_* 的模块，只应是 weight。
DEF_SITES = ("src/services/system_state.h", "src/services/system_state.cpp")
producers = []
for rel, txt in FILES.items():
    if rel.endswith("weight.cpp") or rel in DEF_SITES:
        continue
    if "STATE_WEIGHT_VALID" in strip_code(txt):
        producers.append(rel)
check("D1: 无其他模块写/读 STATE_WEIGHT_VALID（生产者唯一）",
      len(producers) == 0, "found in: " + ", ".join(producers))

# -----------------------------------------------------
# E) Device Model / Homie 未被修改
# -----------------------------------------------------
check("E1: device_model.cpp 不引用 STATE_WEIGHT_VALID（DM 未改）",
      "STATE_WEIGHT_VALID" not in strip_code(dm_cpp))
check("E2: homie_bridge.cpp 不引用 STATE_WEIGHT_VALID（Bridge 未改）",
      "STATE_WEIGHT_VALID" not in strip_code(bridge_cpp))
check("E3: homie_projection.h 不引用 STATE_WEIGHT_VALID（投影未改）",
      "STATE_WEIGHT_VALID" not in strip_code(proj_h))

# -----------------------------------------------------
# F) 启动态残留消除不变量
# -----------------------------------------------------
init_ss = extract_body(ss_cpp_code, "void system_state_init(")
check("F1: system_state_init() 存在", len(init_ss) > 0)
check("F2: system_state_init() 清零全部 valid（无 RAM 残留）",
      "valid=false" in init_ss.replace(" ", ""))
check("F3: 循环上界为 STATE_MAX",
      "STATE_MAX" in init_ss)

# -----------------------------------------------------
# G) 禁用 ⇒ weight_task 早退
# -----------------------------------------------------
task_body = extract_body(weight_code, "void weight_task(")
check("G1: weight_task() 首行 if(!initialized) return（禁用早退）",
      "if(!initialized)return;" in task_body.replace(" ", "").replace("\n", ""))

# -----------------------------------------------------
# H) 采样/滤波路径未被无关重构
# -----------------------------------------------------
check("H1: 滤波窗口仍为 5（WEIGHT_FILTER_WINDOW）",
      "WEIGHT_FILTER_WINDOW5" in weight_code.replace(" ", "").replace("#define", ""))
check("H2: HX711 read 二次就绪确认仍存在（R-8-A）",
      "is_ready()" in weight_code)

# -----------------------------------------------------
# 报告
# -----------------------------------------------------
n_pass = sum(1 for _, ok, _ in results if ok)
n_fail = len(results) - n_pass
for name, ok, detail in results:
    if not ok:
        line = "FAIL: " + name
        if detail:
            line += "  [" + detail + "]"
        print(line)
print("total=%d pass=%d fail=%d" % (len(results), n_pass, n_fail))
sys.exit(0 if n_fail == 0 else 1)
