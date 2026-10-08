#pragma once

// =====================================================
// P2-3 · Homie Bridge —— 投影层对外接口（**极窄**）
//
// 上位冻结设计（唯一依据）：
//   docs/architecture/P2-0-architecture-review-and-design-freeze.md
//   §6（Projection Boundary）· §9（Lifecycle）· §10（$state）
//   §11（$description）· §17.4（变化检测）· §37（GF_HOMIE_ENABLED）
//
// 定位（FROZEN §6.2）：
//   ✅ 负责：node/property → topic 映射 · $state / $description 生成与发布 ·
//            属性状态的变化检测与发布 · /set 解析 → CommandMessage
//   ❌ 不负责：直接操作硬件 · 直接写 System State · 绕过 CommandManager ·
//            解析业务下行 topic · 读写 Config / LittleFS
//
// 依赖方向（FROZEN §6.4，单向）：
//     homie_bridge → cloud/mqtt_transport → cloud/cloud_manager → esp_mqtt_client
//   ★ 本头文件刻意**不暴露**任何 esp-mqtt / CloudManager / Homie 内部结构。
//
// 编译期门控（FROZEN §37 / AD-10a）：
//   GF_HOMIE_ENABLED（默认 **0**）为 0 时，本模块所有函数为空实现 ⇒
//   **不订阅、不发布、不产生任何 MQTT 流量**（生产零代价）。
// =====================================================

#include <stdint.h>

// 初始化（幂等）：注册 foreign handler + 订阅本设备 set filter + 准备 LWT。
// ★ 不发布任何消息（首次发布在 homie_task() 检测到 MQTT 已连接后发生 —— §9）。
// ★ 必须在 device_identity_init() / cloud_init() 之后调用。
void homie_init();

// 主任务：**loop 上下文**调用（非阻塞；无 delay / 无死等 —— §三十一）。
// 内部按 §9 生命周期推进：init → description → 属性初值 → ready，
// 之后进入稳态变化检测 + pending 轮询 + fingerprint 哨兵。
void homie_task();

// 当前是否处于 $state=ready（§10.2 五条件合取）。
// 仅供诊断 / 测试观察使用，不参与任何业务决策。
bool homie_is_ready();
