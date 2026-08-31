#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>

// =====================================================
// SystemCommand 模块（V1）
//
// 定位: 设备自身基础系统控制与资源查询。
// 不负责: 业务功能 / 配置管理 / 网络 / MQTT / 时间 / Workflow /
//         Action / Trigger / Log / OTA。
// 这些功能已由 SystemState / ConfigManager / WiFi / MQTT / TimeManager
// 等实现，本模块不重复实现。
//
// 调用关系（与需求文档一致）:
//   CloudManager -> CommandManager(路由) -> SystemCommand(执行 + 结果构建)
//
// V1 指令:
//   system.memory  查询 Internal RAM / External PSRAM
//   system.flash   查询 Internal Flash / External Flash + 递归文件列表
//                 （原 system.files 已合并进本命令，避免多余命令）
//   system.restart 请求安全重启
//                 （复用 ConfigManager 重启窗口 CONFIG_RESTART_SAFE_DELAY_MS，
//                  当前 10s；不在本模块重建第二套重启机制，
//                  故 restart 的入队由 CommandManager 直接调用
//                  config_cmd_enqueue_restart 完成，本头文件不单独声明）
//
// 设计约束（来自需求文档）:
//   - 不修改其他模块；命令结果直接写入调用方提供的 JsonDocument
//   - 文件遍历只读：不创建 / 删除 / 修改任何文件，不影响 ConfigManager 文件
//   - 不引入不必要的大动态内存；结果直接构造在调用方文档中（扁平 path 列表）
//   - 任何底层失败必须返回 false，禁止"查询失败却 success=true"
//   - 数据单位统一为 bytes，UI 负责格式化显示
// =====================================================

// 初始化（当前无状态；LittleFS 由 main.cpp 挂载，这里不重复挂载）
void system_command_init();

// system.memory
// 写入 out["data"] = { internal:{...}, external:{...} }
//   internal/external 各含 total/free/used/largest_free_block/minimum_free
// 返回 true=成功；false=内存查询失败（理论上不会失败，保留错误通道）
bool syscmd_memory(JsonDocument &out);

// system.flash（合并原 system.files）
// 写入 out["data"] = {
//     internal:{total,used,free},
//     external:{total,used,free},
//     files:[ {path,type,size}, ... ]   // 递归 LittleFS 全部目录与文件
// }
// 返回 true=成功；false=存储/文件系统不可用 或 枚举失败
bool syscmd_flash(JsonDocument &out);
