# LogManager P1 契约冻结（v1.0）

> 项目：Guo Feeder Project
> 日期：2026-09-15
> 代码基线：git `ce9ba37`
> 上游文档：`log模块历史/LogManager详细设计规划0915.md`（总体架构，本报告为其**修订与冻结层**）
> 状态：**P1 冻结**。以下 7 项结论与上游报告冲突时，**以本报告为准**。
> 性质：仅修订设计结论，不改任何代码。

---

## 0. 本次修订依据

针对上游报告的评审意见，共 8 处设计问题。其中 3 处为必须修正（🔴），5 处为需明确冻结（🟡）。

| # | 问题 | 处置 |
|---|---|---|
| 1 | "INFO 不落 Flash" 被 3 条例外推翻 | 🔴 **删除全部例外**，删除 `persist` 概念（§1） |
| 2 | `flags.bit6 uploaded` 与"零原地修改"自相矛盾 | 🔴 **删除 bit6**（§2.1） |
| 3 | `seq` 跨重启只保证"已 flush 单调" | 🔴 **改为 Boot 预留区间**（§3） |
| 4 | `boot_seq` 用 uint16 省 2 B | 🟡 **改 uint32**（§2.1、§3.2） |
| 5 | 段容量 31 / 32 未决 | 🟡 **冻结 31**（§4） |
| 6 | at-least-once 未提升为正式契约 | 🟡 **写入 §5** |
| 7 | `FORCE_ADVANCE` 混入核心协议 | 🟡 **降级为可选策略，默认关闭**（§6） |
| 8 | CRITICAL "≤16 s" 是错误承诺 | 🟡 **改写为"下一批优先"，不承诺秒级**（§7） |

**保留不变（评审通过）**：总体架构、独立 Log Topic、Log 独立 Queue（方案 A）、
CloudManager 仅提供发送能力、不改现有 Up/Down、CBOR 编码、无独立 Log Task、
Flash 段式 Ring、128 B Record、Typed Param、EventId 分类、
**LogManager 不接管现有文本 Log callback**。

---

## 1. Level Policy（冻结）

### 1.1 唯一策略表

| Level | Flash | Cloud | 实时性 |
|---|---|---|---|
| **DEBUG** | ❌ NO | ❌ NO | —（编译期可整体关闭） |
| **INFO** | ❌ NO | ✅ YES | 批量（默认 5 s / 8 条） |
| **WARN** | ✅ YES | ✅ YES | 批量 |
| **ERROR** | ✅ YES | ✅ YES | 可提前 flush |
| **CRITICAL** | ✅ YES | ✅ YES | **立即落盘** + 进入最高优先级下一发送批次 |

### 1.2 硬性约束

1. **Level 是持久化策略的唯一决定因素。**
2. **不允许任何 EventId 级别的覆盖机制** —— 不得存在 `persist` / `flash_override` /
   `force_flash` 之类的字段、标志或字典条目。
3. 上游报告 §11.7 的「3 条例外 INFO」（`LOG_WIFI_PROVISION_ENTER/DONE`、
   `LOG_TIME_VALID_ENTER`）**全部撤销**，改为普通 INFO（Flash NO）。
4. 记录格式中的 `persist` 位**删除**（见 §2.1）。

### 1.3 例外需求的正确归属

"配网过程中掉电导致信息丢失"这类需求，属于 **Boot / Provision 状态持久化**，
应作为独立机制设计（例如一个小而独立的 provisioning 状态文件），
**不得通过给 Log Level 开例外来实现**。

> **理由**：一旦开了第一条例外，"Level 决定策略"这一不变量就失效，
> 后续每一条 INFO 都会有人来申请例外，最终退化为逐事件人工配置。

---

## 2. LogRecord（冻结 v2，128 B 定长）

### 2.1 布局

```
偏移  长度  字段              说明
────────────────────────────────────────────────────────────────────
0     1    version            记录格式版本 = **2**（布局变更，从 1 升级）
1     1    level              0=DEBUG 1=INFO 2=WARN 3=ERROR 4=CRITICAL
2     1    flags              见下
3     1    param_count        0..8
────────────────────────────────────────────────────────────────────
4     4    seq                uint32  全局单调，**允许空洞，不允许重复**
8     4    boot_seq           uint32  ★ 由 uint16 升级为 uint32
12    2    event_id           uint16
14    2    packed             bit0-2 = context_kind；bit3-15 保留
16    4    uptime_ms          uint32  millis()
20    4    timestamp          uint32  Unix 秒；无效时写 0
────────────────────────────────────────────────────────────────────
24    2    blob_len           uint16  context blob 有效字节数（0..32）
26    2    reserved16         写 0
────────────────────────────────────────────────────────────────────
28    48   params[8]          每项 6 B：{ id:u8, type:u8, value:u32le }
76    32   blob               context / 短字符串区
────────────────────────────────────────────────────────────────────
108   4    crc32              覆盖 [0..107]
112   16   reserved           写 0，解码必须忽略
────────────────────────────────────────────────────────────────────
合计  128 B
```

### 2.2 flags（重定义）

| bit | 名称 | 说明 |
|---|---|---|
| 0 | `timestamp_valid` | `timestamp` 是否有效（0 = 无效，排序退化为 `(boot_seq, uptime_ms)`） |
| 1 | `context_present` | `blob` 区有有效内容（配合 `packed.context_kind` 解释） |
| 2–7 | reserved | **写 0，解码必须忽略** |

### 2.3 删除的字段（重要）

| 删除项 | 原位置 | 删除理由 |
|---|---|---|
| `bit1 persist` | flags | **重复语义**。持久化由 §1 的 Level Policy 唯一决定，不是 Record 自身的生命周期状态 |
| `bit6 uploaded` | flags | **与"零原地修改"直接矛盾**。消费状态已由"**段文件是否存在**"表达；保留该位会让后续实现者写出"ACK 后回写 uploaded 位"，把刚否决的 4 KB block 重写方案又引回来 |

> **不变量**：**LogRecord 不知道自己是否已被上传。**
> 上传状态的唯一真相源是"该记录所在的段文件是否仍存在于 `/log/`"。

---

## 3. seq / boot_seq（冻结，含漏洞修复）

### 3.1 上游缺陷

上游定义 `seq` 全局单调、`meta.last_seq` 在 **flush 后**更新。于是：

```
meta.last_seq = 100
  → emit 101, 102, 103（仍在 RAM）
  → ESP.restart()
启动后 meta.last_seq 仍 = 100
  → 下一条 seq = 101   ← 与重启前重复
```

即"跨重启单调"实际只做到"**已 flush 的**跨重启单调"，与声明不符。

### 3.2 修复方案：**Boot 时预留 seq 区间（reserve-on-boot）**

**meta.bin（32 B，fmt_version = 2）**

```
偏移  长度  字段             说明
0     4    magic             0x474C4F47 ("GLOG")
4     1    fmt_version       = 2
5     1    reserved
6     2    reserved16
8     4    boot_seq          uint32  本次启动代次
12    4    seq_reserved      uint32  已"预留出去"的 seq 高水位
16    4    corrupt_count     uint32  历史损坏段计数
20    8    reserved
28    4    crc32             覆盖 [0..27]
```

**boot_seq**：在 `log_init()` 时 **+1 并立即落盘**（不是"Boot 完成时"）。
理由：崩溃重启也必须获得新代次；代价仅为每次启动 1 次 meta 写（本来就有）。

**seq 区间预留**：

```
log_init():
    seq_base      = meta.seq_reserved + 1
    meta.seq_reserved += LOG_SEQ_RESERVE        // 默认 256
    meta.boot_seq     += 1
    写 meta.bin（1 次）                          ← seq_base 之前的全部 seq 被永久预留

log_emit():
    seq = seq_base + (本轮已用计数++)
    if (seq > meta.seq_reserved):               // 区间用尽
        meta.seq_reserved += LOG_SEQ_RESERVE
        写 meta.bin（1 次）
```

| 性质 | 说明 |
|---|---|
| **唯一性** | ✅ 任何 seq 一旦被预留，永不再次分配 |
| **单调性** | ✅ 跨重启严格递增 |
| **空洞** | ⚠️ **允许且预期**。掉电会浪费未使用的 seq；**seq 只需唯一/单调，不需要连续** |
| **meta 写次数** | Boot 1 次 + 每 256 条 1 次 —— **比"每次 flush 更新"更少** |

**Flash 不可用时的降级**：若 meta 写入失败 → 置内部 `s_seq_reliable = false`，
后续批次头 `flags.bit0 = 0`（见 §5.2）。云端对 `seq_reliable=0` 的批次按
"可能重复"处理。

### 3.3 幂等键

```
(device_id, boot_seq, seq)
```

`boot_seq` 与 `seq` **均为 uint32**。128 B 记录里仍有 16 B reserved，
不为 2 B 牺牲幂等键的生命周期。

---

## 4. Flash Segment（冻结）

| 项 | 值 |
|---|---|
| 段头 | 16 B（`magic(4) + seg_index(4) + first_seq(4) + crc32(4)`） |
| 记录数/段 | **31**（冻结，不再"编码阶段决定"） |
| 段大小 | 16 + 31 × 128 = **3984 B < 4096** —— 严格落在单个 LittleFS block 内 |
| 段数 | 16 |
| 容量 | 16 × 31 = **496 条** |
| 总占用 | ≈ **63.7 KB**（LittleFS 12.5 MB 的 0.5%） |
| 文件名 | `s%07u.log`（字典序 = 数值序） |
| 淘汰 | FIFO 删除最旧段；计入 `drop_overflow`（其中未确认的另计 `drop_unacked`） |

**为什么是 31 而非 32**：32 条 = 4112 B 会**跨两个 block**，使 LittleFS 的
写入行为不确定。31 条 = 3984 B 干净落在单 block 内。少 16 条容量无产品意义。

---

## 5. Cloud Batch / ACK 契约（冻结）

### 5.1 传输语义（正式契约）

```
Device:  at-least-once delivery（至少一次）
Cloud:   idempotent deduplication（幂等去重）
```

**显式声明**：

- ACK 的语义是"**云端已持久化该 seq 区间**"，
  **不等于"该区间不会再次出现"**。
- 以下情况都会导致同一区间被重复投递，云端**必须**能接受：
  - 设备重启后重放未确认段
  - ACK 在网络中丢失后的重传
  - 同区间部分记录跨段（段未完全确认即被淘汰前的多次发送）

> 不得让后续实现者把 ACK 误解为"exactly-once"。

### 5.2 批次头（CBOR 整数键；`fmt = 2`）

| 键 | 字段 | 说明 |
|---|---|---|
| `0` | `fmt` | = **2** |
| `1` | `event_dict_ver` | 事件字典版本 |
| `2` | `boot_seq` | |
| `3` | `seq_from` | 本批最小 seq（含） |
| `4` | `seq_to` | 本批最大 seq（含） |
| `5` | `count` | |
| `6` | `drop_ring` | 自上次上报累计：RAM 环丢弃 |
| `7` | `drop_overflow` | 自上次上报累计：段环淘汰总数 |
| `8` | `drop_unacked` | 其中"未确认即被淘汰"的数量 |
| `9` | `self_degraded` | LogManager 自降级次数 |
| `10` | `flags` | **bit0 = `seq_reliable`**（0 = 不可靠，可能重复） |
| `11` | `records` | 记录数组（定序数组编码） |

> 丢弃计数走**侧信道**的原因不变：丢弃事件本身无法落盘（环已满），
> 只能借下一批的头部带出。

### 5.3 ACK 报文（走 `down` Topic）

```json
{"c":"log_ack","i":"<云端唯一id>","p":{"b":<boot_seq>,"f":<seq_from>,"t":<seq_to>}}
```

- 在 `cloud_process_rx_message()` 中**旁路 CommandManager**（照 `change_msg_limit` 落点）。
- 设备侧反应：仅当 `b == 当前 boot_seq` 且区间覆盖"最旧未确认记录"时推进（删除已完全确认的段）。
- **tail 必须单调**，重复/回退的 ACK 一律忽略。

### 5.4 重试（v1 默认）

| 参数 | 值 |
|---|---|
| `ACK_TIMEOUT_MS` | 15000 |
| 最大重试 | 5（退避 2/4/8/16/32 s，上限 60 s） |
| 重试耗尽后 | **保留记录，不删除**（见 §6） |

---

## 6. FORCE_ADVANCE（**不冻结** —— 降级为可选恢复策略）

### 6.1 v1 默认行为：**不启用**

上游把「5 次重试 + 30 min → 强制推进 tail」写进核心协议，**本次撤回**。

理由（评审意见）：它等于设备单方面决定"我不知道云端收没收到，但我等够了，
所以删掉本地唯一副本" —— 这把系统从"尽可能不丢 WARN+"变成
"某些情况下主动允许丢 WARN+"。这是**产品可靠性策略**，不是技术细节，
在云端真实跑起来并拿到实测数据前不应冻结。

### 6.2 v1 默认路径

```
ACK 未到达
   → 重试 5 次（退避）
   → 仍未确认：记录保留在段环，继续正常 FIFO 发送（不阻塞新日志）
   → 段环满：最旧段被淘汰（无论是否已确认），计入 drop_overflow 与 drop_unacked
```

**因此"ACK 永不返回"的最坏后果被环容量自然限界**：最多丢 496 条，
且有 `drop_unacked` 计数可见 —— **不会死锁、不会卡住**。

### 6.3 何时再评估

云端真实运行 ≥ 2 周后，依据实测 `drop_unacked` 决定是否引入
`FORCE_ADVANCE`（或改为"未确认段保留更久 / 增大环"）。
**若引入，必须作为独立可配置项，默认关闭。**

---

## 7. CRITICAL 语义（冻结措辞）

| 维度 | 行为 |
|---|---|
| **Flash** | **立即**（单条 flush，不等 8 条 / 5 s 批量窗口） |
| **Cloud** | **进入最高优先级的"下一发送批次"**（提前 flush 当前待发批次） |
| **独立 MQTT 通道** | ❌ **不做** |
| **端到端延迟** | ❌ **不承诺秒级** |

**措辞约束**（防止后续误读）：

- ✅ 正确表述：CRITICAL **尽快进入发送调度**。
- ❌ 错误表述：CRITICAL **立即发送** / **秒级到达**。

上游 §15.3 给出的"≤16 s 上界"仅作为"典型排空时间"参考，
**不作为 SLA 或契约承诺**。真实延迟取决于 backlog 深度与网络质量。

保留 §15.3 的四条论证（延迟上界已被环容量限界 / 独立路径会破坏 seq 连续性 /
发生率 0–2 次每日 / **已立即落 Flash 故证据不丢**）。

---

## 8. 明确保留的设计

| 项 | 结论 |
|---|---|
| **LogManager 不接管现有文本 Log callback** | ✅ 保留。5 个 `set_log_callback` 继续指向 Serial；LogManager 只收**结构化 typed 事件**。理由：文本回调接入会导致 ①递归 ②双份日志 ③无法结构化 |
| 独立 Log Topic + 独立 Queue（方案 A） | ✅ 保留 |
| CloudManager 4 处纯新增，现有函数 0 改动 | ✅ 保留 |
| 无独立 Task（`log_task()` 在 `loop()`，保 LittleFS 单写者） | ✅ 保留 |
| 段式 Flash 环（整段删除，零原地修改） | ✅ 保留 |
| 递归四层防护 | ✅ 保留 |

---

## 9. P0 / P1 顺序（按评审调整）

### P0 —— 独立前置修复（5 项，各自独立 commit）

1. 修 `event_names[]` 错位（`event_manager.cpp:37/82`）
2. **去除 MQTT 调试横幅的明文密码**（`cloud_manager.cpp:1205-1228`）
3. 注册 `json_storage` / `file_storage` 日志回调（→ Serial）
4. BLE 回调逐字节 hex 输出隔离为编译期宏（`MiThermometer.cpp:117`）
5. 固定 `platformio.ini` 的 `platform` 与 `lib_deps` 版本

> `test_mqtt` 移入 `backup/`：与 Log 弱相关，**可选、可延后**，不纳入 P0 硬性项。

### P1.0 —— Log Contract Review（**本阶段，仅冻结不编码**）

冻结以下 6 项：

1. **Level Policy**（§1）
2. **LogEventId**（100 个，段式 ID）
3. **LogParamId**（86 个 + 类型字典）
4. **LogRecord 128 B**（§2，v2 布局）
5. **Flash Segment**（§4，31 条/段）
6. **Cloud Batch / ACK 契约**（§5，at-least-once）

### P1.1 —— 冻结通过后落地 `log_events.h`

（含 Level/枚举/EventId/ParamId/Record 布局常量/策略表）

### P2+ —— 同上游（Core → Flash → CBOR → MultiTopic → ACK/Retry → 模块接入 → Context → Config+测试 → 文档）

---

## 10. EMQX 现状与测试影响（已知约束）

**当前 `guo_feeder/log` 尚未在 EMQX 配置。** 用户会在需要验证时再添加。

| 测试组 | 可否执行 | 说明 |
|---|---|---|
| Log Core（C1–C8） | ✅ 可测 | 纯设备侧 |
| Flash（F1–F12） | ✅ 可测 | 纯设备侧，含掉电注入 |
| **Cloud（N1–N8、N10–N11）** | ⚠️ **EMQX 配置前 NOT TESTABLE** | 无真实 log Topic 消费者 |
| **N9（Log Topic 被拒）** | ✅ **天然可测** | EMQX 未配置 = 天然的"发布被拒"场景，正好验证"发不出去时不崩、不递归、业务 `up` 不受影响" |
| MultiTopic（T1–T5） | ⚠️ **Log 相关项 NOT TESTABLE** | 同上 |
| 高负载（L1–L7） | ✅ 可测 | 设备侧负载与降级 |

**开发期替代验证手段**：

- 串口 `logt dump` / `logt stats` 直接观察 RAM 环与段环内容
- 本地 mosquitto broker（改 `mqtt.json` 指向）做端到端联调
- 待 EMQX 添加 `guo_feeder/log` 后补齐 N/T 组

---

## 11. 冻结结论速查

| 项 | 冻结值 |
|---|---|
| Level → Flash | DEBUG✗ INFO✗ WARN✓ ERROR✓ CRITICAL✓（**无例外**） |
| Level → Cloud | DEBUG✗ INFO✓ WARN✓ ERROR✓ CRITICAL✓ |
| `persist` 覆盖机制 | **不存在** |
| Record | 128 B，`version = 2` |
| `flags` | bit0 `timestamp_valid`、bit1 `context_present`、bit2-7 reserved |
| `uploaded` 位 | **已删除** |
| `seq` | uint32，Boot 预留区间（默认 256），**允许空洞** |
| `boot_seq` | **uint32**，`log_init()` 时 +1 并落盘 |
| 幂等键 | `(device_id, boot_seq, seq)` |
| 段 | 16 B 头 + **31** × 128 B = 3984 B；16 段 = **496 条** ≈ 63.7 KB |
| 批次头 | `fmt = 2`，含 `drop_unacked` 与 `flags.seq_reliable` |
| 传输语义 | **at-least-once** + 云端幂等 |
| ACK 语义 | "已持久化该区间"，**不代表不会再次出现** |
| `FORCE_ADVANCE` | **可选恢复策略，v1 默认关闭，不冻结** |
| CRITICAL | Flash 立即；Cloud **下一批优先**；**不承诺秒级**；无独立通道 |
| 文本 Log callback | **不接入** LogManager |

---

*本报告仅修订设计结论，未修改任何代码、配置、测试或数据文件。*
