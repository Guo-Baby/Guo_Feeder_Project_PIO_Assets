// =====================================================
// WP4-FW-LOG —— Cloud Log Envelope Producer（P3 · DT:WP-4）
//
// 为什么单独成模块：
//   · **freestanding**（只依赖 <stdint.h> / <stddef.h> + log_events.h / log_cbor.h）
//     ⇒ 可用主机 clang++ 编到 wasm32 **真跑**，做字节级 / 可逆性验证，
//       见 test/log_contract/probe_envelope.cpp
//   · LogManager 只负责"取记录 + 调这里"，信封 / Base64 逻辑可独立回归。
//
// 冻结依据（人工裁决，2026-10-11）：
//   H-WP4-FW-02 = CLOSED : 1 个 MQTT log message = 1 条 log_history record；不做 batch
//   H-WP4-FW-03 = CLOSED : payload_base64 = 完整 CBOR LogRecord bytes（禁二次 JSON 化 /
//                          字段映射 / 截断 / 自定义 envelope 内部字段）
//   H-WP4-FW-04 = CLOSED : count = 1
//   ⇒ 见 docs/review/P3-WP4-Firmware-Log-Implementation.md
//
// ★ 本模块**不修改** LogRecord 布局（log_events.h 的 128 B packed）与
//   CBOR 批次 schema（log_cbor.h 的 12 项定序数组，键值 0..11 不变）：
//   payload_base64 正是 log_cbor_encode_batch(count = 1) 的**原样输出**，
//   因此不构成"第二套 log protocol"。
//
// Envelope（JSON，字段顺序即契约）：
//   { "device_id":..., "boot_seq":..., "seq":..., "ts":...,
//     "level":..., "payload_base64":"..." }
// =====================================================

#ifndef LOG_ENVELOPE_H
#define LOG_ENVELOPE_H

#include <stdint.h>
#include <stddef.h>

#include "log/log_events.h"
#include "log/log_cbor.h"

// ---- 冻结常量 ----

// 每个 MQTT log message 恰好承载 1 条记录（H-WP4-FW-02 / 04）
#define LOG_ENV_RECORDS_PER_MSG 1u

// payload = log_cbor_encode_batch(count = 1) 的字节数
//   = 1 (array12) + 55 (11 × 最坏 uint) + 1 (array1) + (3 + 128) = 188
#define LOG_ENV_CBOR_BYTES 188u

// Base64(188 B) = ceil(188/3) × 4 = 63 × 4 = 252
#define LOG_ENV_B64_BYTES 252u

// envelope JSON 上界（实测最大约 387 B：device_id 按 32 字符上限计）
#define LOG_ENV_MAX_BYTES 512u

// 编译期自检：Base64 长度公式与常量一致
static_assert(LOG_ENV_B64_BYTES == ((LOG_ENV_CBOR_BYTES + 2u) / 3u) * 4u,
              "LOG_ENV_B64_BYTES must equal ceil(n/3)*4");
static_assert(LOG_ENV_MAX_BYTES >= LOG_ENV_B64_BYTES + 96u,
              "LOG_ENV_MAX_BYTES must cover JSON envelope framing");

// ---- 侧信道计数（沿用既有 CBOR 批次头，键值不变）----
//
// H-WP4-FW-03 禁止新增 envelope 字段 ⇒ drop_* / flags 继续放在 CBOR 批次头里，
// 随 payload_base64 原样承载（每条重复一次）。语义与既有 cloud_build_cbor() 一致。
struct LogEnvelopeMeta
{
    uint32_t drop_ring;
    uint32_t drop_overflow;
    uint32_t drop_unacked;
    uint32_t self_degraded;
    uint32_t flags;          // bit0 = LOG_BATCH_FLAG_SEQ_RELIABLE
};

// ---- API ----

// 标准 Base64（含 '=' 填充、无换行）。返回写入字符数；参数非法 / 容量不足 → 0。
uint32_t log_envelope_base64(const uint8_t *in, uint32_t in_len,
                             char *out, uint32_t out_cap);

// 把**单条**记录编码为 count = 1 的 CBOR 批次（复用冻结编码器，格式零改动）。
// 返回字节数；参数非法 / 容量不足 → 0。out_cap 必须 ≥ LOG_ENV_CBOR_BYTES。
uint32_t log_envelope_build_cbor(const LogRecord &rec,
                                 const LogEnvelopeMeta &meta,
                                 uint8_t *out, uint32_t out_cap);

// 核心：**已编码的 CBOR bytes** → JSON envelope（写入 out，含结尾 '\0'）。
// 返回 JSON 字符数（不含结尾 '\0'）；失败 → 0。
uint32_t log_envelope_encode_cbor(const uint8_t *cbor, uint32_t cbor_len,
                                  uint32_t boot_seq, uint32_t seq, uint32_t ts,
                                  uint8_t level, const char *device_id,
                                  char *out, uint32_t out_cap);

// 便捷封装：LogRecord → CBOR(count=1) → JSON envelope。
// 返回 JSON 字符数；失败 → 0（调用方保留记录、下轮再试）。
uint32_t log_envelope_encode_record(const LogRecord &rec,
                                    const LogEnvelopeMeta &meta,
                                    const char *device_id,
                                    char *out, uint32_t out_cap);

#endif // LOG_ENVELOPE_H
