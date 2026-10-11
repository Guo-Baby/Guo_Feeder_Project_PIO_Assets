# P3 · DT:WP-4 · `WP4-FW-LOG` —— **实施规格记录**（Cloud Log Envelope Producer）

> ## 🟢 `IMPLEMENTATION SPEC` —— **固件侧实施规格（冻结输入）**
>
> | 项 | 值 |
> |---|---|
> | **性质** | **实施规格记录** —— 把已裁决的设计冻结项翻译成**可编码的实现契约** |
> | **前置冻结** | `DT:WP-4 = FROZEN`（`2097c4e`）· 固件侧设计评审 = `b813022` · 人工裁决 = `3bc8eb9` |
> | **上游裁决** | **`H-WP4-FW-02` / `03` / `04`**（信封粒度 / 载荷边界 / 条数）· **`H-WP4-FW-08`**（Flash 门禁）· **`H-WP4-FW-01`**（`/up` 分流，本工作流不涉及） |
> | **范围** | **仅 `WP4-FW-LOG`**（Cloud Log Envelope Producer）—— **不含 telemetry** |
> | **本文档不改代码** | 规格记录；实际改动见 `src/log/log_envelope.{h,cpp}` 与 `src/log/log_manager.cpp` 的信封接线 |

---

## §1 数据链路（冻结）

```
LogManager（产生 LogRecord）
        │
        ▼
LogRecord CBOR            ← 复用既有 src/log/log_cbor.h（count = 1）
        │
        ▼
Base64                    ← 本工作流新增（src/log/log_envelope.cpp）
        │
        ▼
Cloud Log Envelope（JSON）
        │
        ▼
CloudManager              ← 既有 cloud_send_log()（最小 transport 支持）
        │
        ▼
MQTT  guo_feeder/<device_id>/log
```

> **链路铁律**：**LogManager 只产生 `LogRecord`**；**不直接调用 MQTT**（一律经 `CloudManager`，P1.4 既有约束）。

---

## §2 固定规则（冻结 · 逐条可核）

| # | 规则 | 来源 |
|---|---|---|
| **R-1** | **1 个 MQTT log message = 1 条 `log_history` record** | `H-WP4-FW-02` |
| **R-2** | **`payload_base64` = 完整 CBOR `LogRecord` bytes 的 Base64**（不做 batch） | `H-WP4-FW-02` / `03` |
| **R-3** | **`count = 1`**（每报文恰好一条记录） | `H-WP4-FW-04` |
| **R-4** | **不修改 `LogRecord` 二进制格式**（`src/log/log_events.h` 的 128 B packed 布局 **零改动**） | `H-WP4-FW-02` |
| **R-5** | **Worker 不解析 CBOR**（Worker 侧 CBOR-blind；本工作流不改 Worker） | `H-WP4C-01` |
| **R-6** | **不新增 topic**（沿用 P2 已冻结的 5/5 ACL 覆盖） | `H-WP4-01` / `13` |
| **R-7** | **不新增 Cloud protocol version**（不改 `cloud_protocol.md` 的协议版本语义） | 本任务约束 |

### §2.1 `payload_base64` 的精确字节边界（关键澄清）

冻结选项为设计评审 §8 `H-WP4-FW-03` 的**候选 ①**：

> **`payload_base64` = `log_cbor_encode_batch(count = 1)` 的完整 CBOR 字节**

- 即：**单条**记录，经**既有冻结的 CBOR 批次编码器**（`src/log/log_cbor.h`）编码为
  **12 项定序数组 + 1 个 `bstr(128)`**，长度 = `log_cbor_batch_max_size(1)` = **188 B**。
- **不是**裸 128 B `LogRecord`（那是被否决的候选 ③）。
- **不做**二次 JSON 化 / 字段映射 / 截断 / 自定义 envelope 内部字段。
- **⇒ 结果**：CBOR 编码器与 `log_events.h` 契约**零改动** ⇒ **不构成"第二套 log protocol"**。

### §2.2 侧信道计数（`drop_*` / `flags.seq_reliable`）的处置

- 冻结禁令 = **不得新增 envelope 字段**。
- ⇒ 侧信道计数**继续留在 CBOR 批次头**（`LOG_BKEY_DROP_*` / `LOG_BKEY_FLAGS`，**键值不变**），
  它们随 `payload_base64` **原样承载**（每条重复一次）。
- **⇒ 不丢数据、不改 schema**：批次头 12 项**全部照填**，仅 `count = 1`。

---

## §3 模块职责（冻结）

| 模块 | 职责 | 边界 |
|---|---|---|
| **`LogManager`** | 产生 `LogRecord`；Flash 段环 / 云队列 / ACK / 重试 / 段回收 | **不**做 JSON / Base64 / MQTT |
| **Envelope Producer**（`src/log/log_envelope.{h,cpp}`，新增） | 生成 JSON envelope + Base64 编码 | **纯函数、freestanding**（只依赖 `stdint.h` / `log_events.h` / `log_cbor.h`）⇒ 可在主机 wasm32 **真跑**做字节级验证 |
| **`CloudManager`** | MQTT transport（既有 `cloud_send_log()`） | **不**做编码 / 不解析 / 不排队；**仅**最小 transport 支持 |

### §3.1 Envelope 字段（冻结 · 顺序即契约）

```json
{
  "device_id":      "<12 位小写 hex>",
  "boot_seq":       <uint32>,
  "seq":            <uint32>,
  "ts":             <uint32>,
  "level":          <uint8>,
  "payload_base64": "<Base64 of 188 B CBOR batch(count=1)>"
}
```

- `device_id` **必须与 `/log` topic 中的 `<device_id>` 一致**（`DT:WP-4` 的 `ID-6`）；
  来源 = `services/device_identity.h::device_id()`（与 topic 渲染**同源**）。
- `ts` = `LogRecord.timestamp`（**0 = 时间未同步**，按原值上报）。
- `level` = `LogRecord.level`（`LogLevel` 枚举）。
- **无其它字段**（`H-WP4-FW-03`：禁止自定义 envelope 内部字段）。

---

## §4 资源约束（`H-WP4-FW-08` = `CLOSED WITH GUARD`）

| # | 门禁 |
|---|---|
| **G-1** | 实施完成后**必须执行 firmware size report**（`pio run` 段尺寸） |
| **G-2** | 若 Flash 增量**超过预算**（P2-0.2 ceiling = **≤ 14,336 B** 的实测增量） ⇒ **STOP** |
| **G-3** | **不自行删除功能** |
| **G-4** | **不自行修改冻结协议** |
| **G-5** | **不降低日志可靠性** |
| **G-6** | 超限 ⇒ **报告 `RESOURCE BLOCKED` 并请求人工裁决** |

> 预算背景：`P2-0.2` ceiling `≤14,336 B` − `P2-7` 实测 `+12,548 B` ⇒ **余量仅 1,788 B**。

---

## §5 允许 / 禁止（本工作流）

**允许**：`src/`（新增 `log_envelope.{h,cpp}` + `log_manager.cpp` 接线）· `test/` · `docs/review/`。

**禁止**：telemetry 实现 · 改 `/up` · 改 EMQX · 改 Cloudflare Worker · 改 migration ·
**改 `src/log/log_events.h` 的 12 项 CBOR 键值 / 128 B 布局** · 改 topic renderer ·
改 command uplink · 改压缩协议 · 远程部署 · push。

---

## §6 与既有设计文档的关系

- 本文件 = **实施规格摘要**；**权威设计**仍是 `docs/review/P3-WP4-Firmware-Integration-Design-Review.md`（`§9` 为裁决留痕）。
- 本文件**不重复**风险登记（`F-WP4-FW-*` / `R-WP4-FW-*`），亦**不改变**任何 Gate / Phase C 状态。

---

*本文件于 **2026-10-11** 建立，为 `WP4-FW-LOG` 的**实施规格记录**（阶段 A）。
基线 = 固件仓 **`3bc8eb9`**。*
