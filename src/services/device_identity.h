#ifndef DEVICE_IDENTITY_H
#define DEVICE_IDENTITY_H

#include <Arduino.h>

// =====================================================
// DeviceIdentity —— 设备身份（P0-1）
// =====================================================
// 职责（**只做身份**；模板渲染不在本模块，见 services/topic_renderer.h）：
//   1. 读取芯片 eFuse 出厂 MAC —— 固定来源 ESP_MAC_WIFI_STA
//   2. 派生 device_id：12 个字符、全部小写 hex、无分隔符（形如 aabbccddeeff）
//   3. NVS 身份锚点：namespace "gfid"
//        key "did"  = ASCII 字符串 / 长度 12 个字符 / 内容为小写 hex
//        key "dver" = uint8，派生算法版本号
//   4. 启动决策表（已有 did 永远优先，禁止自动覆盖）：
//        无 did                     -> 派生并写入 NVS
//        did 与 MAC 派生一致         -> 直接使用
//        did 与 MAC 派生不一致       -> 保留 NVS 值（不覆盖）+ WARN
//        dver 未知（非本固件内置值） -> 保留已有 did（降级兼容）+ WARN
//   5. NVS 写入失败 -> 保留内存值 + 串口 WARN（不重启 / 不重试 / 不阻塞）
//
// 硬约束（评审项，勿破坏）：
//   · 不依赖配置管理 / 云通信 / 显示等其它子系统（不 include）
//   · 不取网络协议栈的 MAC 接口：其行为依赖网络已初始化，而本模块必须在网络初始化之前执行
//   · 状态只用文件作用域 static（零裸全局），不进 System State
//   · 未接入 Critical Operation：仅 setup 期一次性写 NVS，失败可降级，无需保护
//   · 不提供身份重置接口：身份重置不等于普通恢复出厂，误调用会直接造成身份漂移；
//     未来的身份迁移只允许走独立的 identity migration 流程
// =====================================================

// 初始化（幂等）：首启派生并写入 NVS；后续启动以 NVS 为运行期身份锚点。
// 内部降级：不抛错、不阻塞、不重启。
void device_identity_init();

// 返回 '\0' 结尾的 12 字符 device_id。
// 永不返回 nullptr；未初始化 / 不可用时返回空串 ""。
const char* device_id();

// device_id 是否可用（长度恰为 12 且全部为 [0-9a-f]）
bool device_id_valid();

#endif  // DEVICE_IDENTITY_H
