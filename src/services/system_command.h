#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>

// =====================================================
// SystemCommand 模块（V2：新增统一 Safe Restart）
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
// -----------------------------------------------------
// V2 核心职责：全系统唯一的 Restart 执行机制
// -----------------------------------------------------
// 本模块是全系统唯一允许调用 ESP.restart() 的地方。
// 任何模块（用户命令 / ConfigManager / Workflow / 未来定时重启）
// 都只能通过 system_command_request_restart() 请求重启，
// 由本模块统一判断"什么时候可以重启"并执行。
//
// Restart 模型（需求文档 §2）:
//   Request -> 等待 Critical Operation Count == 0
//           -> RESTART_PENDING(10s 安全窗口)
//           -> ESP.restart()
//
// 三条硬性规则:
//   1. Restart 一旦被请求，不可取消（不提供 cancel 接口）。
//   2. Restart 可以被延迟（Critical Count > 0），但不能被撤销。
//   3. 进入 RESTART_PENDING 后禁止新的 Critical Operation。
//
// -----------------------------------------------------
// V1 指令（保持不变）
// -----------------------------------------------------
//   system.memory  查询 Internal RAM / External PSRAM
//   system.flash   查询 Internal Flash / External Flash + 递归文件列表
//   system.restart 请求安全重启
//
// 设计约束（来自需求文档）:
//   - 不修改其他模块；命令结果直接写入调用方提供的 JsonDocument
//   - 文件遍历只读：不创建 / 删除 / 修改任何文件，不影响 ConfigManager 文件
//   - 不引入不必要的大动态内存；结果直接构造在调用方文档中（扁平 path 列表）
//   - 任何底层失败必须返回 false，禁止"查询失败却 success=true"
//   - 数据单位统一为 bytes，UI 负责格式化显示
// =====================================================

// =====================================================
// Restart 状态机（需求文档 §6）
// =====================================================
enum RestartState
{
    RESTART_IDLE = 0,      // 正常运行，无 Restart 请求
    RESTART_REQUESTED,     // 已收到 Restart 请求，正在等待 Critical Operation 归零
    RESTART_PENDING,       // Critical 已归零，进入最终安全倒计时窗口
    RESTARTING             // 倒计时结束，已调用 ESP.restart()（理论上不可达）
};

// Restart 安全窗口（需求文档 §14）
//
// 进入 RESTART_PENDING 后的固定倒计时。
// 目的与 ConfigManager 原先的 CONFIG_RESTART_SAFE_DELAY_MS 一致:
// 给 completion callback / MQTT 上行留出把结果真正发出去的时间。
//
// 注意: 本模块刻意不引用 config_manager.h —— Restart 能力已从
// ConfigManager 收回，方向必须是 ConfigManager -> SystemCommand，
// 不能反向依赖。因此这里使用本模块自己的常量。
#define SYSTEM_RESTART_SAFE_DELAY_MS 10000UL

// =====================================================
// 生命周期
// =====================================================

// 初始化。复位内部状态、初始化自旋锁、缓存本次启动的 Reset Reason。
// 幂等，可重复调用。
void system_command_init();

// 主任务，必须由主循环（loop）每轮调用一次。
//
// 这是 Restart 状态机唯一的驱动点。非阻塞：除真正重启外不做任何等待。
// 注意: 本函数由 Arduino loop 任务调用，而 acquire/release/request_restart
// 可能由 esp-mqtt 任务调用（cloud_manager 在 mqtt_event_handler 中同步执行
// 命令），因此内部状态全部由自旋锁保护。
void system_command_task();

// =====================================================
// Restart Request API（需求文档 §9）
// =====================================================

// 请求安全重启。全系统统一的 Restart 入口。
//
// 返回:
//   true  -> Restart 已被接受（首次请求）或早已处于等待中（幂等）
//   false -> 系统已进入 RESTARTING，无法再接受请求
//
// 幂等: 重复调用不会重置已经开始的 10s 安全窗口，也不会取消既有请求。
// 本函数绝不直接重启，真正的重启只发生在 system_command_task() 中。
bool system_command_request_restart();

// 查询当前 Restart 状态
RestartState system_command_restart_state();

// 状态名（便于日志 / JSON 上报）
const char *system_command_restart_state_name(RestartState st);

// 是否存在"已请求但尚未执行"的 Restart
//
// remain_ms 语义:
//   RESTART_REQUESTED -> 置 0。此时还在等 Critical Operation，
//                        剩余时间未知（可能无限期），不能用 0 理解为"马上重启"。
//   RESTART_PENDING   -> 安全窗口剩余 ms（到点为 0）
//   RESTARTING        -> 0
//   RESTART_IDLE      -> 置 0 并返回 false
bool system_command_restart_pending(unsigned long &remain_ms);

// =====================================================
// Critical Operation API（需求文档 §10 / §11）
// =====================================================

// 声明一个"不可被重启中断"的操作开始。
//
// 返回:
//   true  -> 获得许可，调用方可以开始执行该 Critical Operation
//   false -> 被拒绝。当前已处于 RESTART_PENDING / RESTARTING，
//            调用方【不得】开始该操作（需求文档 §14）
//
// 调用方在收到 false 时必须放弃本次操作，并且【不得】调用 release()。
bool system_command_critical_operation_acquire();

// 声明一个 Critical Operation 完成。
//
// 只有此前 acquire() 成功的操作才可以 release()。
// 返回:
//   true  -> 正常配对释放
//   false -> 计数已经是 0（调用方配对错误）。已做下溢保护，
//            不会造成 uint32 下溢。该情况属于调用方 bug，会打印错误日志。
bool system_command_critical_operation_release();

// 只读查询当前 Critical Operation 数量（诊断用）
uint32_t system_command_critical_operation_count();

// =====================================================
// Reset Reason 预留接口（需求文档 §15）
//
// 本阶段只提供查询，不引入 Log 系统。
// 未来 Log 模块建立后再决定是否把 Restart 过程纳入 Log。
// =====================================================

// 返回本次启动的 Reset Reason（esp_reset_reason_t 原值，启动时缓存）
uint8_t system_command_reset_reason();

// 返回 Reset Reason 的可读名称（如 "software" / "power_on"）
const char *system_command_reset_reason_name();

// =====================================================
// 指令实现
// =====================================================

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

// system.stats（G-4 观测缺口补齐 · 只读）
// 写入 out["data"] = {
//     available:true|false,          // false ⇒ LogManager 未初始化（≠ 全零）
//     uptime_ms, boot_seq,
//     queue:{used,capacity,inflight,retry,gave_up,acked_seq,hole_overflow},
//     counters:{emit_total,ring_drop,drop_overflow,drop_unacked,
//               cloud_batch_sent,cloud_records_sent,cloud_retx,cloud_ack_ok,
//               cloud_ack_timeout,cloud_ack_lost,ack_ignored,ack_partial,
//               replay_records,offline_skip,give_up,seg_evicted_unacked,
//               seg_deleted,self_degraded},
//     flash:{valid_segments,total_records,corrupt_count,seq_reliable}
// }
// 返回 true=成功；false=统计不可用（**不得**降级为"全部为 0"）
bool syscmd_stats(JsonDocument &out);

// system.restart_status（诊断 / 观察用）
// 写入 out["data"] = {
//     state, critical_operations, restart_pending, remain_ms, safe_delay_ms
// }
// 返回 true=成功；false=结果构造失败
bool syscmd_restart_status(JsonDocument &out);

// system.loop（S-3：loop() 周期耗时统计 · **只读**）
//
// 写入 out["data"] = {
//     available:true|false,      // false ⇒ 尚无首样本 / 编译期关闭（≠ 全零）
//     enabled, scope:"uptime",   // ★ 单次查询 = **自首样本起的累积口径**
//     uptime_ms, elapsed_ms,
//     bucket_width_us, bucket_count, over_limit_us,
//     samples, over, anomaly, sum_us, mean_us, min_us, max_us,
//     sat_buckets, n_saturated, snap_crit_max_us, snap_count,
//     valid, invalid_reasons:[...], quantiles_suppressed,
//     p50_us / p50_low_us / p50_high_us, p99_us / p99_low_us / p99_high_us,
//     p50_bucket, p99_bucket, gate_p99_us, p99_lt_gate,
//     hist: [bucket, count, ...]  // 非零稀疏对，供"两次快照差分"
// }
// ★ 不提供任何清零/重置写操作（窗口统计由调用方差分实现）
// ★ 若统计不可信（样本不足/桶饱和/n 饱和/计数回退）则**不输出** p50/p99 数值，
//   改为 valid:false + invalid_reasons，杜绝"误导性的有效 P99"
// 返回 true=成功；false=结果构造失败（**不得**降级为"全部为 0"）
bool syscmd_loop(JsonDocument &out);

// system.memwatch —— `G4-R4` 内存观测（H-1…H-5 · 只读）
//
// 写入 out["data"] = {
//     available:true|false,             // false ⇒ MemWatch 未初始化（≠ 全零）
//     uptime_ms,
//     dram:{ total, free, used, largest_free_block, minimum_free_since_boot },
//     psram:{ total, free, largest_free_block, minimum_free_since_boot },
//     drift:{ dram:{valid,reason,samples,span_ms,free_delta,slope_per_hour,
//                   free_min,largest_min},
//             psram:{...},
//             window:{ target_ms, samples, span_ms, warmup_ms } },
//     alloc:{ calls, psram_ok, psram_fail, dram_fallback_ok, dram_fallback_fail,
//             final_fail, last_fail_site, last_fail_size, last_fail_ms,
//             sites:{ <name>:{final_fail} }, coverage:{...} },
//     stack:{ available, task, stack_bytes, hwm_raw, hwm_bytes, used_bytes,
//             unit:"bytes"|"words", unit_verified },
//     thresholds:{ note, ... }          // ★ 全部为**候选**，未批准，非门禁
// }
//
// ★ 本命令**不**输出 PASS/FAIL：`G4-R4` 阈值尚未人工裁决（`HUMAN_REQUIRED`）。
// ★ `drift.dram.valid=false` ⇒ `slope_per_hour` 无意义（窗口未建立），
//   调用方**不得**把它当 0 解读（与 `system.stats.available:false` 同一纪律）。
// 返回 true=成功；false=结果构造失败（**不得**降级为"全部为 0"）
bool syscmd_memwatch(JsonDocument &out);
