#ifndef TOPIC_RENDERER_H
#define TOPIC_RENDERER_H

#include <Arduino.h>

// =====================================================
// TopicRenderer —— Topic / client_id 模板渲染（P0-2）
// =====================================================
// 职责（**只做这些**）：
//   1. <device_id> 占位符替换
//   2. topic / client_id 模板渲染
//   3. 长度检查
//   4. 字符合法性检查
//
// 依赖：只读调用 device_id() / device_id_valid()（services/device_identity.h）。
// 零依赖：配置管理 / MQTT / 云通信 —— 模板由调用方传入，本模块不读配置。
//
// 约束（冻结，见 docs/architecture/P0-设备身份与Topic隔离设计.md §2.4.1）：
//   T-1 只支持 <device_id> 一种占位符（无变量表 / 表达式 / 转义语法）
//   T-2 渲染点全工程唯一：其它模块（含 config_manager）一律不得做替换
//   T-3 不读配置（模板由调用方传入）
//   T-4 不感知 MQTT 的发布与订阅
//   T-5 "不含占位符 ⇒ 原样放行 true"（回滚通路，不可改为报错）
// =====================================================

// ---- 模板常量（单点定义，避免"两个文件各写一遍模板"的漂移）----
#define GF_DEVICE_ID_PLACEHOLDER "<device_id>"
#define GF_TOPIC_TPL_DOWN        "guo_feeder/<device_id>/down"
#define GF_TOPIC_TPL_UP          "guo_feeder/<device_id>/up"
#define GF_TOPIC_TPL_LOG         "guo_feeder/<device_id>/log"
#define GF_CLIENT_ID_TPL         "dev_<device_id>"

// ---- 渲染自检开关（默认 0 = 生产固件；DEBUG / 测试环境置 1）----
// 与 MI_THERMO_DEBUG_VERBOSE 同一模式：默认 0 时其分支与字符串常量
// 由 --gc-sections 剔除 ⇒ 零 Flash / RAM 代价。
#ifndef GF_TOPIC_RENDER_SELFTEST
#define GF_TOPIC_RENDER_SELFTEST 0
#endif

// 渲染契约（4 条规则，冻结；见设计 §3.3）：
//   1. tmpl 含 "<device_id>"   ⇒ 全部替换（支持多次出现）
//   2. tmpl 不含 "<device_id>" ⇒ 原样返回 true（legacy 兼容 / 配置级回滚通路）
//   3. device_id 不可用         ⇒ 返回 false（调用方走内建 V3 兜底）
//   4. 渲染后仍含 '<' / '>' / 控制字符，或长度 > 128，或为空 ⇒ 返回 false（判为配置错误）
// 成功：写入 out 并返回 true。失败：out 被清空并返回 false。
bool topic_render(const String& tmpl, String& out);

#if GF_TOPIC_RENDER_SELFTEST
// 模板渲染自检（**仅 DEBUG / 测试环境**编译与调用）：
//   对内置样例断言渲染结果，输出一行 "[TopicRender] selftest PASS/FAIL"。
//   生产固件不编译、不执行；亦不得放进任何启动必需路径（如 device_identity_init()）。
void topic_renderer_selftest();
#endif

#endif  // TOPIC_RENDERER_H
