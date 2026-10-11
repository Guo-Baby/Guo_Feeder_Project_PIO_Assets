# P3 · DT:WP-4 —— **固件侧实施前设计评审**
## （`WP4-FW-LOG` Cloud Log Envelope · `WP4-FW-TELEMETRY` Firmware Telemetry Publisher）

> ## 🔵 `DESIGN REVIEW ONLY` —— **只分析 · 不实施 · 不修改任何既有文件**
>
> | 项 | 值 |
> |---|---|
> | **性质** | **设计评审（只读分析 + 方案设计）** —— 产出**可实施路径 / 影响范围 / 风险 / 实施计划** |
> | **本轮改动的文件** | **仅本文件（新增）** —— **未修改任何既有文件**（`src/` · 文档 · 配置 · 迁移 **零改动**） |
> | **前置基线** | `DT:WP-4 = FROZEN`（`2097c4e`）· **`WP-4 = IMPLEMENTED`**（`9b25203`，Worker 侧 `POST /api/history/ingest` 已落地 + 真实 D1 48/48）· Phase C 两项固件裁决 **已下达**（`0badf9e` / `4b6f6ed`） |
> | **上游裁决** | **`H-WP4C-01` = `CLOSED`**（Firmware **LogManager** 增加 **Cloud Log Envelope producer**；**Worker remains CBOR-blind**）· **`H-WP4C-02` = `CLOSED`**（新增 **Firmware Telemetry Publisher**；**不复用 command uplink**） |
> | **本文档不做的** | ❌ 不改 firm/Worker/EMQX · ❌ 不建 migration · ❌ 不改冻结设计文档 · ❌ 不实施 · ❌ 不 **push**（**本文件已作为 review artifact 提交**：`b813022` = 设计评审基线；**`§9` = 人工裁决留痕轮** ⇒ **实施仍未开始**） |
> | **术语铁律** | `PASS` / `NOT RUN` / `HUMAN_REQUIRED` / `BLOCKED` / `DEFERRED` / `CLOSED` —— **本文档所有判定均为「设计期判定」，不构成实现验证**；**`NOT RUN` / `HUMAN_REQUIRED` / `BLOCKED` 一律 ≠ `PASS`** |
> | **引用规范** | `DT:WP-n` = 设计轨 · `计划:WP-n` = `P3-History-D1-Plan.md` · `WP1:§n` / `WP2:§n` / `WP3:§n` = 三份冻结文档 · `proto:§n` = `cloud_protocol.md` · `file:line` = 固件源码证据 |

---

## Executive Summary

### 结论（可实施性判定）

| 工作流 | 可实施性判定 | 一句话理由 |
|---|---|---|
| **`WP4-FW-LOG`**（Cloud Log Envelope） | **⚠️ `CONDITIONAL — 可实施，但有 3 项硬前置`** | **方案收敛、改动面小、编码器可复用**；但受阻于 ① `/up`+`/log` 切换后的**云端命令结果回归**（`F-WP4-FW-2`）② **无云端 `log_ack` ⇒ Flash 段环永不回收**（`F-WP4-FW-1`）③ **Flash 预算余量仅 1,788 B**（`R-WP4-FW-1`） |
| **`WP4-FW-TELEMETRY`**（Telemetry Publisher） | **⚠️ `CONDITIONAL — 可实施，但有 4 项硬前置`** | **数据源已就绪（System State 覆盖 8/9 类）**、**模块边界清晰**；但受阻于 ① **配置键 `mqtt.json.telemetry` 全不存在**（固件零 `telemetry` 字样）② **`type` 词表 / `value` 编码未冻结** ③ **权重阈值 `X` 无数值**（WP-1 明令不冻结）④ **`/up` 混流**（与 LOG 共用同一前置） |

**共同硬约束**：**Flash 增量余量 = 1,788 B**（P2-0.2 ceiling `≤14,336 B` − P2-7 实测 `+12,548 B`，`P2-7-Software-Readiness-Report.md:27`）—— **两条工作流合并后的 Flash 增量极可能超过该余量** ⇒ **预算重裁是必答题**（见 `H-WP4-FW-08`）。

### Task 4 —— 冻结约束检查（**设计期**）

| Constraint | 判定 | 依据（设计层） |
|---|---|---|
| **Worker CBOR blind** | ✅ **`PASS`（设计）** | 信封只把 **设备侧 CBOR 字节** 做 `Base64` 承载（`WP4:§2.5`）；**Worker 侧零解码**（`history-ingest.js:183` 响应体明确不含 `log_ack`、全文无 CBOR 解码） |
| **log idempotency unchanged** | ✅ **`PASS`（设计）** | **逐条**信封明文携带 `(device_id, boot_seq, seq)`；**复用冻结唯一索引 `ux_lh_dedup`**；**不新增第二套 key**（`log_manager.cpp:2272-2275` 的批次三分量与逐条 seq 同源） |
| **no command uplink reuse** | ✅ **`PASS`（设计）** | telemetry **不经** `cloud_compress_uplink()`（`cloud_manager.cpp:945`）⇒ **不复用 `c/i/s/r` 压缩协议**；新增独立窄接口（`cloud_send_telemetry()`） |
| **no topic change** | ✅ **`PASS`（设计）** | 三模板**零改动**（`topic_renderer.h:28-30`）；**不新增 topic / 不新增 ACL**（P2-5 的 5/5 覆盖已含 `/up` 与 `/log`，`credential.js:118`） |
| **no Cloud protocol regression** | ⚠️ **`ATTENTION`（非 `PASS`）** | **`/up` 混流**：命令结果（`c/i/s/r`）与 telemetry 共用 `/up`（`cloud_manager.cpp:1692-1699`）⇒ EMQX Action 切到新端点后，**命令结果会被逐条 `rejected`**，且旧 `/api/ingest` 的 `mqtt_messages` 记账链路断开 ⇒ **须裁决 `H-WP4-FW-01`** |

> **⚠️ 上述 4 个 `PASS` 均为「设计期、未被实现验证」的判定** —— **无一行代码被改动**，**无一次上板/联调被执行**。

---

## §1 Current Architecture Review

### 1.1 相关模块地图（**已核实**）

| 层 | 模块 | 职责 | 与本次工作流的关系 |
|---|---|---|---|
| 契约 | `src/log/log_events.h`（652 行） | **冻结契约**：Level Policy · `LogRecord` 128 B · EventId/ParamId · 段/`meta.bin` · Batch/ACK 常量 · `LOG_BKEY_*` · **编译期 `static_assert`** | **⛔ 零改动（铁律）** |
| 编码 | `src/log/log_cbor.h`（171 行） | **freestanding** CBOR 批次编码器（定序数组 12 项 + `bstr(128)` 记录） | **格式零改动**；**信封可复用其「单条」调用** |
| 存储/管道 | `src/log/log_manager.{h,cpp}`（473 / 3,684 行） | RAM 环 → routing → Flash 段环 → 云队列 → CBOR → `/log` → ACK 水位 / 补发 / 退避 / 段回收 | **主要改造点（唯一的 wire 出口）** |
| ACK 状态机 | `src/log/log_ack.h`（498 行） | 纯函数：区间判定 / 部分覆盖 / 退避 / 放弃 / 空洞 / 段可回收 | **零改动**（信封化不改变 `(boot_seq, seq)` 语义） |
| 传输 | `src/cloud/cloud_manager.{h,cpp}`（159 / 2,606 行） | MQTT（esp-mqtt QoS1）· 下行压缩 JSON 解析 · 上行字段压缩 · registry/分片 · **三 topic 渲染** | **只加一个窄上行接口**；既有函数**不改语义** |
| Topic | `src/services/topic_renderer.{h,cpp}`（55 / 182 行） | `<device_id>` 替换（**全工程唯一渲染点**） | **零改动** |
| 身份 | `src/services/device_identity.h` | `device_id()`（12-hex） | **只读调用**（信封字段来源） |
| 状态 | `src/services/system_state.{h,cpp}` | 全系统共享状态（**telemetry 的唯一数据源**） | **只读调用** |
| 事件 | `src/services/event_manager.{h,cpp}` | 事件推送/订阅（非阻塞） | telemetry 的**事件驱动入口**候选 |
| 配置 | `src/services/config_manager.{h,cpp}` | 7 模块（`wifi/oled/valve/time/weight/mqtt/mi_thermo`） | **新增 `mqtt.json.telemetry` 键**（WP-1 `H-W1-5`） |
| 循环 | `src/main.cpp`（2,216 行） | `setup()` 初始化 + `loop()` 任务序列（`log_task()` @`main.cpp:664`） | **接线点**（新增 `telemetry_task()`） |

### 1.2 ★ Current Log Pipeline（**产生 → 缓存 → 序列化 → 上传 → MQTT topic**）

```
        ┌──────────────────── ①  产生（Producers）────────────────────┐
        │ 116 处 log_emit()（17 个 .cpp）                              │
        │   workflow.cpp 22 · time_manager 13 · main 10 · valve 8 ·     │
        │   cloud_manager 7 · config/storage/cmd/weight/MiThermo 各 6 … │
        │ API: log_emit(event_id, level, params, n)  log_manager.h:223  │
        └───────────────────────────┬─────────────────────────────────┘
                                    ▼  log_emit_internal()  log_manager.cpp:1703
        ┌──────────────────── ②  缓存（RAM 环）──────────────────────┐
        │ 64 槽 × 128 B = 8,192 B（PSRAM 优先 / DRAM 回退）            │
        │ 快照组装：uptime_ms=millis() · timestamp=time_get()（无效=0）│
        │          flags |= LOG_FLAG_TIMESTAMP_VALID                  │
        │ log_manager.cpp:1762-1775 · 环满 ⇒ 淘汰最旧 + ring_drop      │
        │ ★ 突发合并（白名单 EventId，5 s 窗口，Σ LOG_P_COUNT 守恒）   │
        └───────────────────────────┬─────────────────────────────────┘
                                    ▼  log_task()  log_manager.cpp:1892（loop 内，每轮 ≤8 条）
        ┌──────────────────── ③  路由（Level Policy）─────────────────┐
        │ to_flash = log_level_to_flash(lv) · to_cloud = …            │
        │ DEBUG{N,N} INFO{N,Y} WARN{Y,Y} ERROR{Y,Y} CRITICAL{Y,Y}      │
        │ log_manager.cpp:1937-1941 · log_events.h:64-80（冻结）       │
        └───────┬─────────────────────────────────┬───────────────────┘
                ▼ to_flash                        ▼ to_cloud
   ┌────────────────────────┐        ┌──────────────────────────────────────────┐
   │ ④a Flash 段环（落盘）   │        │ ④b 云待发队列（RAM）                       │
   │ 16 段 × (16 B + 31×128)│        │ 128 槽 × 128 B = 16,384 B（PSRAM 优先）    │
   │ = 3,984 B/段 ⇒ 62.2 KiB│        │ INFO 仅在此（Flash NO ⇒ 无兜底）           │
   │ 容量 496 条            │        │ cloud_queue_push()  log_manager.cpp:2135   │
   │ flash_append_batch()   │        │ 满 ⇒ FIFO 淘汰 + drop_overflow             │
   │ log_manager.cpp:1228   │        └──────────────────┬───────────────────────┘
   │ meta.bin 32 B（boot_seq│                           │
   │ + seq_reserved 预留 256）│                         │
   └───────────┬────────────┘                           │
               │ 补发（队列空时按 slot 遍历段）           │
               └──────────────►  cloud_replay_step()  log_manager.cpp:2623
                                             │
                                             ▼
                           ┌──────────────── ⑤  序列化 ────────────────┐
                           │ cloud_build_cbor(n)  log_manager.cpp:2249 │
                           │   → log_cbor_encode_batch()  log_cbor.h:124│
                           │   → CBOR 定序数组 12 项 [+ records[] bstr(128)] │
                           │   最大：N=1 → 188 B · N=16 → 2,153 B      │
                           │   缓冲：LOG_BATCH_MAX_PAYLOAD = 4,096 B    │
                           │   侧信道：drop_ring/overflow/unacked/degraded│
                           │   标志：flags.bit0 = seq_reliable           │
                           └──────────────────┬─────────────────────────┘
                                              ▼
                           ┌──────────────── ⑥  上传 ──────────────────┐
                           │ cloud_try_send_batch(n)  log_manager.cpp:2743│
                           │   → cloud_send_log(bytes, len)             │
                           │     cloud_manager.cpp:2504-2544            │
                           │     → cloud_mqtt_publish_binary()  :661     │
                           │       QoS1 · esp_mqtt_client_enqueue()      │
                           │       store = CLOUD_MQTT_STORE (true, :52)   │
                           │ ★ 离线 ⇒ 立即 return false（**不排队不落盘**）│
                           │ ★ **不分片、不压缩**（Batch ≤ 4,096 B）      │
                           │ 成功 ⇒ inflight=true + deadline(15 s)        │
                           └──────────────────┬─────────────────────────┘
                                              ▼
                           ┌──────── ⑦  MQTT topic（唯一 wire 出口）────┐
                           │ mqtt_log_topic = cloud_render_topic(       │
                           │     config_get_mqtt_log_topic(),           │
                           │     GF_TOPIC_TPL_LOG, "log_topic")          │
                           │ cloud_manager.cpp:1992 / 2511-2522         │
                           │ 模板：`guo_feeder/<device_id>/log`           │
                           │ topic_renderer.h:30 · 渲染点 topic_render()  │
                           └──────────────────┬─────────────────────────┘
                                              ▼
        ┌──────────────── ⑧  闭环（ACK 驱动）────────────────────────────┐
        │ 下行 `{"c":"log_ack","i":"…","p":{"b":…,"f":…,"t":…}}`         │
        │ log_events.h:550-555 · cloud_manager.cpp:1532/1560（旁路去重）  │
        │ → cloud_set_log_ack_callback()  :2547 ← 注册 log_manager:1659  │
        │ → cloud_handle_ack()  log_manager.cpp:2433                    │
        │ → 水位：acked_seq（真实最大值）/ gc_seq（连续可回收，FIX-BT9）  │
        │ → 段回收 flash_segment_reclaimable()  :2375-2420              │
        │   ⚠️ 守卫：`s_cloud_gc_seq == 0 ⇒ 不回收`  :2383              │
        │ 超时 15 s × 最多 5 次 + 退避(2/4/8/16/32 s) → GIVE_UP_NOT_ADVANCE │
        │ （记录仍留 Flash，登记空洞，等下次开机补发）                     │
        └───────────────────────────────────────────────────────────────┘
```

**证据（逐条可回查）**

| # | 事实 | 证据 |
|---|---|---|
| 1 | 116 处 `log_emit` 调用点 / 17 个 `.cpp` | `grep -rn log_emit src/ --include=*.cpp` |
| 2 | `LogRecord` 128 B packed（`seq`/`boot_seq` 均 uint32，offset 4 / 8） | `log_events.h:455-473` · `:582-583` |
| 3 | 时间戳：`time_get()` 无效 ⇒ `timestamp=0` 且 **不置 valid 位** | `log_manager.cpp:1762-1775` |
| 4 | Level Policy（**冻结**，无 EventId 例外） | `log_events.h:44-80` |
| 5 | 云队列 128 槽（**INFO 无 Flash 兜底**，注释明示原因） | `log_manager.h:74-81` |
| 6 | CBOR 批次 12 项定序数组 + `bstr(128)` | `log_cbor.h:14-20` · `:145-166` |
| 7 | 批次最大字节公式（预检用） | `log_cbor.h:113-120` |
| 8 | `cloud_send_log` 离线即返回 false（**CloudManager 不为 Log 建队列**） | `cloud_manager.cpp:2524-2528` · `cloud_manager.h:57` |
| 9 | `/log` topic 惰性渲染 + 失败丢弃 | `cloud_manager.cpp:2511-2522` |
| 10 | QoS1 `store=true`（MQTT 层自带离线 outbox） | `cloud_manager.cpp:52` · `:693-700` |
| 11 | 段回收**依赖 ACK**，`gc_seq==0` 直接不回收 | `log_manager.cpp:2381-2383` |
| 12 | 段环满 ⇒ `未 ACK 记录被淘汰时计数` | `log_manager.cpp:1052-1101` · `:1188` |

### 1.3 CloudManager 现状边界（**"传输层"到底做了什么**）

| 项 | 现状 | 判定 |
|---|---|---|
| **对外上行 API** | `cloud_send_set()`（`/set` 文本）· `cloud_send_up()`（`/up` 文本 **+ 压缩**）· `cloud_upload_json()`（→ `cloud_send_up`）· `cloud_send_log()`（`/log` 二进制） | 共 4 个 |
| **上行压缩** | `cloud_send_up()` 内部调 `cloud_compress_uplink()`（`cloud_manager.cpp:2477`）⇒ **`status→s` / `id→i` / `cmd→c` / `registry→r` / `version→v` / `count→n` / `checksum→cs` 等短键**（`:940-945`） | ⚠️ **这正是「command uplink」的形态** ⇒ telemetry 若走它即违规 |
| **`/up` 的实际载荷** | **命令结果**：`on_command_result()` → `cloud_send_up()`（`:1692-1699`）；构造端为 `command_manager.cpp` 的 `{"cmd":"result","id":…,"status":…}`（`:176` / `:477` / `:579` / `:712` …） | ⚠️ **与 telemetry 抢同一 topic**（见 `F-WP4-FW-2`） |
| **分片** | `cloud_publish_fragmented()`（`:863-931`）· 阈值 `CLOUD_DEFAULT_MSG_LIMIT = 8000`（`cloud_manager.h:157`）—— **仅被 registry/大 JSON 路径使用**；**`/log` 与 `/up` 普通上行都不分片** | 信封需自证 ≤ 上限 |
| **静态缓冲（DRAM）** | `cloud_bin_buffer` 8,192 · `cloud_frag_packet` 8,192 · `rx_buffer[4][8,192] = 32,768` · `mqtt_large_rx_buffer` 8,192（`cloud_manager.cpp:258-276`） | 已占 DRAM ≈ 57 KB；**不宜再加 DRAM 大缓冲** |
| **Transport Facade** | `cloud_transport_publish()` —— 契约明示**只供 `mqtt_transport.cpp` 转发使用，业务代码不直接调用**（`cloud_manager.h:70-88`） | ⚠️ **telemetry 不得借用它**（否则破坏 P2-2 边界） |
| **LWT / retain** | 仅 Facade 路径可设；业务上行 `retain=false` | 与本次工作流无关（不动） |

### 1.4 Topic 现状（**零改动的理由**）

| topic | 模板常量 | 用途 | ACL（P2-5） |
|---|---|---|---|
| `guo_feeder/<device_id>/down` | `GF_TOPIC_TPL_DOWN`（`topic_renderer.h:28`） | Cloud → Device（命令 + `log_ack`） | sub ✅ |
| `guo_feeder/<device_id>/up` | `GF_TOPIC_TPL_UP`（`:29`） | **命令结果**（现状）→ **+ telemetry（未来）** | pub ✅ |
| `guo_feeder/<device_id>/log` | `GF_TOPIC_TPL_LOG`（`:30`） | Log 批次（现状：CBOR） | pub ✅ |

> **★ 结论**：**入口族 = `guo_feeder/<device_id>/…` 已由 `H-WP4-01`/`13` 冻结**（`DT:WP-4` R3）⇒ **本次两条工作流均不新增 topic、不改 ACL**。渲染点唯一：`cloud_render_topic()`（`cloud_manager.cpp:1846`）→ `topic_render()`（全工程唯一替换点）。

### 1.5 离线/可靠性现状（**信封化不得破坏的三条**）

| 语义 | 现状实现 | 信封化的影响 |
|---|---|---|
| **WARN+ 离线保命** | 双写：Flash 段环（496 条）+ 云队列（128 槽）；离线时 `cloud_poll` 不消费（`:2891-2898`），重启后 `acked_seq=0` ⇒ **全量补发**（`:1665`） | ✅ **不受影响**（补发读的是 Flash 记录，与 wire 编码无关） |
| **INFO 无兜底** | INFO 仅进云队列（128 槽），满则 FIFO 淘汰 + `drop_overflow` | ⚠️ **不变**（信封化不改善；且见 `F-WP4-FW-9`） |
| **at-least-once + 云端幂等** | QoS1 + 超时重发 + `GIVE_UP_NOT_ADVANCE`（不推进水位，留着下次开机重放） | ✅ **不受影响**（`(boot_seq, seq)` 语义未变 ⇒ `ux_lh_dedup` 继续吸收） |
| **★ 段回收依赖 ACK** | `gc_seq==0 ⇒ 不回收`（`:2383`）；`gc_seq` 仅由 `log_ack` 推进（`:2433-2508`） | 🔴 **本评审最高优先级发现** —— 见 `F-WP4-FW-1` |

### 1.6 资源现状（**P2-7 实测基线**）

| 项 | 值 | 证据 |
|---|---|---|
| **Flash 合计增量（ceiling 对象）** | **+12,548 B ≤ 14,336 B** ⇒ **headroom 1,788 B** | `P2-7-Software-Readiness-Report.md:27` / `:88` |
| **ceiling 定义** | **`≤ 14 KiB`（14,336 B）= 唯一 Flash acceptance ceiling**（P2-0.2 修订；`.text`/`.rodata` 不再单列） | `P2-0-…freeze.md:981` / `:992` / `:1000-1001` |
| **DRAM 静态** | ≈130 KB / 327 KB（`platformio.ini` 注释） | `platformio.ini` build_flags 注释 |
| **loopTask 栈** | 16,384 B（`-DARDUINO_LOOP_STACK_SIZE=16384`） | `platformio.ini` |
| **LogManager 运行期缓冲** | RAM 环 8,192 B + 云队列 16,384 B + Flash 批 2,048 B + CBOR 4,096 B ≈ **30.7 KB（PSRAM 优先，可回退 DRAM）** | `log_manager.h:34` / `:81` · `log_manager.cpp:2966-3045`（`cloud_init_buffers()`） |
| **LittleFS 分区** | `0xBF0000` ≈ **11.94 MiB**；`/log` 仅用 ≈ **62.2 KiB** | `partitions.csv` |
| **MQTT 缓冲上限** | `CLOUD_MQTT_BUFFER_SIZE = 8192`；分片阈值 8,000 | `cloud_manager.cpp:63-65` · `cloud_manager.h:157` |
| **ArduinoJson** | `7.4.3`（`lib_deps` 精确锁版）· `-DARDUINOJSON_USE_CBOR=1` | `platformio.ini` |

---

## §2 WP4-FW-LOG Design Proposal

### 2.1 目标形态

```
现状：  LogRecord ──► CBOR 批次(数组12项) ──► /log  （二进制，EMQX 无法转 JSON，Worker 不能解码）
目标：  LogRecord ──► Cloud Log Envelope(JSON) ──► /log  （UTF-8 JSON，Worker 可直接 consume）
                        └─ payload_base64 = Base64(设备侧 CBOR 字节，**原样**，Worker 不解析)
```

**冻结信封（`H-WP4C-01`）**：`device_id` · `boot_seq` · `seq` · `ts` · `level` · `payload_base64`

### 2.2 方案比较（**3 个候选，含"不引入第二套 log protocol"的判据**）

| 方案 | 语义 | 幂等键 | 与 WP-2 schema | 带宽（16 条） | 冻结冲突 | 判定 |
|---|---|---|---|---|---|---|
| **`L-A` 逐条信封 + 单条 CBOR 载荷**（数组容器） | 每条记录 = 1 个信封；`payload_base64` = **`log_cbor_encode_batch(count=1)` 的 188 B** | ✅ 逐条 `(device_id,boot_seq,seq)` | ✅ **逐条对位**（`level`/`seq`/`ts` 每行都有值） | ≈ **5,969 B**（×2.77） | 无（CBOR **格式**零改动，仅**调用粒度**从批→条） | ✅ **推荐** |
| **`L-B` 整批信封（载荷 = 整批 CBOR）** | 1 个信封 = 1 批；`payload_base64` = `base64(2,153 B)` | ❌ 标量 `seq`/`ts`/`level` **无法表达 16 条** ⇒ 只能取 `seq_from`/首条 ts/最高级 ⇒ **1 批 = 1 行** | ❌ **破坏 WP-2 的 log 逐行语义**（16 条坍缩为 1 行） | ≈ **2,992 B**（×1.39） | ⚠️ 与 `WP2:§4.2` 字段意图冲突 | ❌ 不推荐 |
| **`L-C` 信封携带整批载荷 + 附加 `records[]` 元数据数组** | 标量给聚合值，另加逐条 `seq/ts/level` 数组 | ⚠️ 需 Worker 接受**冻结字段表之外**的新字段 | ⚠️ 越出 `DT:WP-4` §2.4/§5.2 的字段矩阵 | ≈ 5,000 B | ❌ **需重裁 `DT:WP-4`（触碰冻结文档）** | ❌ 不推荐（本轮） |

> **★ `L-A` 的关键优点（为什么它不算"第二套 log protocol"）**：
> ① **CBOR 编码格式与常量一字未改** —— 仍调用同一个 `log_cbor_encode_batch()`（`log_cbor.h:124`），仍产出 12 项定序数组 + `bstr(128)`；
> ② **改变的是「一次编码几条」**（`h.count` 由 ≤16 变为 1）—— 属**调用粒度**，不属协议；
> ③ **Worker 侧零改动**（CBOR-blind 保持）；
> ④ **幂等键与索引零改动**（`ux_lh_dedup`）。
>
> **`L-A` 的代价**：基 64 之后的**带宽 ×2.77**（逐条 CBOR 头 57 B/条 + `Base64` +33% + 每信封重复 `device_id`/键名 ≈ 120 B）—— **须 `H-WP4-FW-02`/`03` 裁决是否接受**；替代优化 = **信封级只写一次 `device_id`**（但偏离冻结的逐条字段列表 ⇒ 需重裁）。

### 2.3 容器与批大小（**派生结论**）

| 项 | 计算 | 结论 |
|---|---|---|
| 单条 CBOR | `log_cbor_batch_max_size(1)` = 1 + 55 + 1 + 131 = **188 B** | — |
| `base64(188)` | ⌈188/3⌉×4 = **252 B** | +34% |
| 单信封固定开销（键名+标点+标量值） | ≈ **120 B**（`{"device_id":"xxxxxxxxxxxx","boot_seq":…,"seq":…,"ts":…,"level":"…","payload_base64":"…"}`） | — |
| **单信封总量** | 120 + 252 ≈ **372 B** | — |
| 16 条数组 | 16×372 + 15 逗号 + 2 方括号 ≈ **5,969 B** | ⚠️ **> 4,096 B（现 CBOR 缓冲）** |
| 现缓冲可容量 | ⌊(4,096−17)/372⌋ ≈ 10 条 | ⇒ **N ≤ 10** |
| 若缓冲提升至 8,192 B | N ≤ 16（5,969 B ≤ 8,192） | ⇒ **需 +4,096 B PSRAM（或 −6 条/批）** |

> **⇒ 两项派生要求**：① **工作缓冲从 4,096 B 提升到 8,192 B，或把每报文条数降到 ≤10**（**资源取舍 ⇒ `H-WP4-FW-04`**）；② **必须在发送前做容量预检**（沿用既有模式：`log_cbor.h` 的 `cap` 检查 + `cloud_try_send_batch` 的 `self_degraded` 计数，`log_manager.cpp:2747-2753`）。

### 2.4 逐条回答 Task 1 的 6 个分析问题

| 问题 | 结论 | 依据/理由 |
|---|---|---|
| **是否新增 serializer？** | ✅ **是** —— 新增 **freestanding `src/log/log_envelope.h`**（Base64 + JSON 信封文本 + 容量预检） | 与 `log_cbor.h` **同构**（零依赖 ⇒ 可 **wasm32 主机真跑**做字节级验证，复用 `test/log_contract/*` 模式）；**不引入 ArduinoJson**（避免堆抖动、避免 `-DARDUINOJSON_USE_CBOR` 双编码路径） |
| **是否复用现有 CBOR？** | ✅ **复用，格式零改动**，仅**按条调用**（`count=1`） | `log_cbor_encode_batch()` 已支持 `count≥1`（`log_cbor.h:130`/`:135`）；`LOG_BKEY_*`/`LOG_BATCH_FMT` 全部不变 |
| **是否需要新消息类型？** | ❌ **不需要** —— 仍是「`/log` 上的一批记录」，只是一个 MQTT 消息内从 CBOR 变为 JSON 数组 | `CloudMessageType`（`cloud_manager.h:139-146`）是**下行**分类，与上行 Log 无关 |
| **是否需要修改 CloudManager？** | ⚠️ **只改 `cloud_send_log()` 的**载荷形态语义（`const uint8_t*` → UTF-8 文本）**；**不改**其他函数、不改签名族、不加 topic | 最小改法 = 让 `cloud_send_log()` 继续用 `cloud_mqtt_publish_binary(topic, (const uint8_t*)json, len, 1)`（它只是字节发布，**不要求内容真是二进制**）⇒ **零新增发布路径**（见「实施计划 S3」的两个子选项） |
| **是否需要修改 TopicRenderer？** | ❌ **不需要** | `GF_TOPIC_TPL_LOG` 不变（`topic_renderer.h:30`） |
| **是否影响离线缓存？** | ✅ **不影响** | 段环（WARN+）、云队列（INFO+）、补发、ACK 水位**全部与 wire 编码无关**（§1.5）—— **只有 ⑤ 序列化与 ⑥ 上传两步被替换** |

> **★ 唯一"穿透"到下游的约束**：`LOG_BATCH_MAX_PAYLOAD = 4096`（`log_events.h:523`）当前同时充当 **CBOR 缓冲容量** 与 **每批 wire 上限**；信封化后两者**不再等价** ⇒ **必须新增一个独立常量**（如 `LOG_ENV_MAX_BYTES`）**并明确哪个是"发送上限"**（`H-WP4-FW-04`）。
> ⚠️ **不得**修改 `log_events.h` 的既有常量语义（**冻结契约 + `static_assert` 契约测试**，`log_events.h:566-650`）。

### 2.5 资源影响评估（**ESP32-S3 / N16R8**）

| 维度 | 评估 | 量级 | 处置建议 |
|---|---|---|---|
| **RAM（新增）** | 信封工作缓冲 | 4,096 B（N≤10）**或** 8,192 B（N=16） | **PSRAM 分配**（沿用 `heap_caps_malloc` + `MALLOC_CAP_SPIRAM` → `MALLOC_CAP_8BIT` 回退，`log_manager.cpp:2966-3045`）⇒ **DRAM 零增长** |
| **RAM（可减）** | 若 `L-A` 不再需要 4,096 B 的整批 CBOR 缓冲，可**复用/替换**该缓冲 | **−4,096 B** | ⇒ **净变化可为 0**（把 CBOR 缓冲改造成「单条 CBOR 188 B 暂存 + 信封 8,192 B」） |
| **PSRAM** | +0 ~ +4,096 B | 可忽略（N16R8 有 8 MB OPI PSRAM） | — |
| **Flash（.text/.rodata）** | `log_envelope.h`（Base64 表 64×3 B ≈ 192 B .rodata + ~150-250 B .text） + `log_manager` 调用点改写（±小） | **≈ +0.3 ~ +0.8 KB** | 🔴 **与 telemetry 共享 1,788 B 余量** ⇒ 见 `R-WP4-FW-1` |
| **CPU** | `base64(188 B)`/条：≈ 250 次移位查表 ⇒ **微秒级**（240 MHz）；16 条/批 ⇒ 可忽略 | 忽略 | — |
| **CPU（限流不变）** | `LOG_TX_MIN_INTERVAL_MS = 500`（`:524`）⇒ 上限 **2 Hz**、**8 条/s**（N=16）⇒ 保持 | 不变 | — |
| **MQTT payload** | 单报文 ≈ **5.9 KB**（N=16）· **3.7 KB**（N=10）· **372 B**（N=1） | 均 **< 8,000 B**（分片阈值）且 **< 8,192 B**（缓冲） | **不需要分片**；但**需要新的显式预检** |
| **是否需要压缩？** | ❌ 不需要（且**不得**做业务压缩） | — | JSON 键名固定、可预测；若需省带宽 ⇒ 应回到 `L-B` 方案的**裁决**（不得静默改协议） |
| **是否需要分片？** | ❌ 不需要（单报文 ≤ 8 KB；`/log` 现状本就不分片） | — | 保留 `cloud_publish_fragmented` 仅给 registry 路径 |
| **queue 调整？** | ❌ **不需要** | 云队列 128 槽与 wire 编码正交 | — |
| **★ 未覆盖风险** | **Flash 段环永不回收**（无 ACK）⇒ 496 条硬上限 | 🔴 | 见 `F-WP4-FW-1` / `H-WP4-FW-06` |

---

## §3 WP4-FW-TELEMETRY Design Proposal

### 3.1 ★ Telemetry Candidate Sources（**逐个核实**）

| Data | Source（System State / API） | Existing API | Ready |
|---|---|---|---|
| **temperature** | `STATE_MI_THERMO_TEMP`（FLOAT，°C） | `state_get_float()` + `state_is_valid()`；有效性另有 `STATE_MI_THERMO_VALID`；写入点 `MiThermometer.cpp:531` / `:557` | ✅ **就绪**（条件：`mi_thermo` 已配置且 `VALID`） |
| **humidity** | `STATE_MI_THERMO_HUMID`（FLOAT，%） | 同上 | ✅ **就绪**（同上） |
| **battery** | `STATE_MI_THERMO_BAT_V`（FLOAT，V） | `state_get_float()` | ✅ 可用 · ⛔ **按 `WP1:§2.1` 属「设备元数据」⇒ 不落库** |
| **weight** | `STATE_WEIGHT_VALUE`（FLOAT，g） | `state_get_float()`；写入 `weight.cpp:290`/`:358`/`:756`；有效性 `STATE_WEIGHT_VALID` | ⚠️ **条件就绪** —— 依赖 `weight.enable`（**P2-8-KI-2 三方不一致，未修复**） |
| **weight error** | `STATE_WEIGHT_ERROR`（BOOL） | `state_get_bool()`（`weight.cpp:721` 附近同簇） | ✅ 就绪 |
| **valve** | `STATE_VALVE_STATUS`（BOOL）+ 事件 `EVENT_VALVE_OPEN`/`EVENT_VALVE_CLOSE`/`EVENT_VALVE_ERROR` | `state_get_bool()` / `valve_is_open()` / `event_subscribe()`；写入 `valve.cpp:111` | ✅ 就绪 |
| **wifi** | `STATE_WIFI_STATUS`（BOOL）· `STATE_WIFI_RSSI`（INT）· `STATE_WIFI_SIGNAL`（INT）· `STATE_WIFI_STATE`（INT） | `state_get_bool/int()` | ✅ 就绪（**仅异常落库**，`WP1:§3.9`） |
| **time** | `STATE_TIME_VALID`（BOOL）· `time_get()`（Unix 秒）· `time_get_state()` | `state_get_bool()` / `time_get()` | ✅ 就绪（**`time-valid` 翻转计入异常**） |
| **mqtt / online** | `STATE_MQTT_STATUS` · `STATE_MQTT_RETRY_COUNT` · `STATE_MQTT_LAST_CONNECT_TIME` · `STATE_MQTT_LAST_ERROR` | `state_get_*()` | ✅ 就绪；⚠️ **`$state` 迁移词表**（`init`/`ready`/`disconnected`/`lost`）**无现成跟踪器** ⇒ **需新增**；⚠️ **`lost` = broker LWT，设备侧不可自报告**（见 `F-WP4-FW-7`） |
| **event queue** | `STATE_EVENT_QUEUE_COUNT`（INT） | `state_get_int()` | ✅ 就绪（**高水位计入异常**） |
| **system health 扩展**（heap / uptime / 队列水位） | `esp_get_free_heap_size()` · `millis()` · `LogStats`/`LogCloudInfo` | **工程内当前未使用** ⇒ 需新增读取（只读，无副作用） | ⚠️ **需新增**（`WP1:§3.9`：**仅异常落库**） |
| **water level（水位）** | — | — | ❌ **不存在独立传感器**（`WP1:§2.3` 明令不得臆造；水量由 Weight 派生） |
| **heartbeat** | — | — | ⛔ **冻结：不产生、不入库**（`WP1:§5 H-W1-3`） |

**其他关键事实**

| 事实 | 影响 |
|---|---|
| 固件**全仓零 `telemetry` 字样**（`grep -rn telemetry src/` = 空） | ⇒ **`WP1:§5 H-W1-5` 的配置键 `mqtt.json.telemetry` 从未落地**（`F-WP4-FW-4`） |
| `MiThermometer.h` **无 getter**（只有 `MiThermometer_task()` / 队列句柄） | ⇒ 只能经 **System State** 取值（**可接受**，且符合「谁产生谁写、大家读」的既有规则） |
| Homie 投影**已**把 `climate/temperature` · `water/weight` · `system/wifi-rssi` 等映射到 `homie/5/<id>/…` | ⚠️ **不得**把 Homie 投影当作 telemetry 入口（`H-WP4-01`/`13`：入口族 = `/up` + `/log`；Homie 是**设备模型投影**，非 ingest 家族） |

### 3.2 Publisher 架构设计（**推荐：两级拆分，policy 可离线验证**）

```
        ┌────────────────── System State（唯一数据源）──────────────────┐
        │ wi / valve / weight / mi_thermo / time / mqtt / event-queue     │
        └───────────────────────────┬───────────────────────────────────┘
                                    │ 只读 state_get_*()
                                    ▼
   ┌───────────────── src/services/telemetry_policy.h（★ freestanding · 纯函数 · 主机可测）─────────┐
   │  · 变化阈值判定   policy_weight_should_emit(now_g, last_g, thr)                        │
   │  · 抽样兜底       policy_cadence_due(now_ms, last_ms, N_ms)                            │
   │  · 事件去重/边沿  policy_edge_accept(event_id, now_ms, &state)                         │
   │  · $state 迁移    policy_online_should_emit(prev, cur)（仅 init/ready/disconnected）    │
   │  · 健康异常判定   policy_health_abnormal(metric, value, thr)                           │
   │  ⇒ **零硬件依赖 ⇒ wasm32 真跑**（与 test/log_contract 同模式）                          │
   └───────────────────────────┬───────────────────────────────────────────────────────────┘
                               ▼ 决策结果（emit? / type / value）
   ┌──────── src/services/telemetry_publisher.{h,cpp}（glue：读状态 → 组信封 → 交传输）────────┐
   │  telemetry_publisher_init()  ·  telemetry_task()（loop 内、非阻塞、millis 节流）        │
   │  事件订阅：EVENT_WEIGHT_ERROR / EVENT_VALVE_* / EVENT_CLOUD_* / EVENT_ERROR            │
   └───────────────────────────┬───────────────────────────────────────────────────────────┘
                               ▼  Telemetry Envelope（JSON，单条 = 单观测）
   ┌───────────────────────────────────────────────────────────────────────────────────────┐
   │ {"ts":<Unix秒>,"type":"<词表>","value":<num|str>,"seq":<单调计数>[,"source":"<…>"][,"schema_version":1]} │
   │ 依据：`DT:WP-4` §2.5 + §5.2 必需字段矩阵（ts/type/value/seq 必填；source/schema_version 可选）         │
   └───────────────────────────┬───────────────────────────────────────────────────────────┘
                               ▼
   ┌──────────── CloudManager（**只做传输**）──── cloud_send_telemetry(const char* json) ───────────┐
   │  ★ **不经 `cloud_compress_uplink()`** ⇒ 不复用命令上行（`H-WP4C-02`）                          │
   │  ★ 复用既有 `cloud_mqtt_publish_text(mqtt_pub_topic, …, 1)`（`cloud_manager.cpp:724`）          │
   └───────────────────────────┬───────────────────────────────────────────────────────────────────┘
                               ▼
                 MQTT  `guo_feeder/<device_id>/up`   （**topic 零改动**）
```

**逐条回答 Task 2 的分析问题**

| 问题 | 结论 | 理由 |
|---|---|---|
| **需要新模块？** | ✅ **是，2 个文件对**：`services/telemetry_publisher.{h,cpp}` + `services/telemetry_policy.h` | 放 `services/`（跨模块服务层）**不新建分层目录**（避免动 7 层目录约定）；`telemetry_policy.h` 走 **freestanding** ⇒ 可主机验证（**这是本方案最重要的可验证性设计**） |
| **EventManager 接入？** | ✅ **是**（事件类 + 阀动作类） | 复用既有 `event_subscribe()`（`event_manager.h:130`）；**复用现有 `EVENT_*` 词表**（`event_manager.h:13-48`）⇒ **不新增事件枚举**（也就**不动** `event_to_string/from_string` 与 workflow 的 trigger 映射） |
| **Workflow integration？** | ❌ **否（刻意不接入）** | telemetry 是**设备侧观测**（`WP1:§2.1`：设备侧观测 = telemetry）；把它做成 Workflow Action/Trigger 会**引入双通道与额外生命周期**，且 workflow 契约不受影响（**零改动**） |
| **定时发送？** | ✅ **是**，但**只有两类**：① weight `N = 15 min` 抽样兜底（`WP1:§5 H-W1-2`）② climate 5–15 min 或超阈值（`WP1:§3.5`） | 统一由 `telemetry_task()` 的 **millis 节流**实现（**禁 `delay()`/`while`**；与 `time_task`/`weight_task` 同风格） |
| **状态变化触发？** | ✅ **是**：weight 阈值 · climate 阈值 · `$state` 迁移 · system health 异常 | 变化驱动为主 + 抽样兜底为辅（逐类见 `WP1:§3`） |
| **心跳？** | ⛔ **不产生** | `WP1:§5 H-W1-3`（heartbeat 不入库）；**也不得**用「周期 online / 超时 offline」伪装在线历史（`H-W1-4`） |
| **`value` 编码（数字 vs 字符串）** | ⚠️ **须裁决** | `WP2:§3.3`：物理 `TEXT` + 逻辑类型由 `schema_version` 解释 ⇒ 设备侧必须与云端约定「`value` 是 JSON number 还是 string」⇒ `H-WP4-FW-05` |
| **`type` 词表** | ⚠️ **须裁决** | `WP2` 未逐字冻结 `type` 取值 ⇒ **必须给出一份封闭词表**（如 `weight.value` / `climate.temperature` / `valve.action` / `online.state` …）⇒ `H-WP4-FW-05` |

### 3.3 与 CloudManager 的关系（**原则保持**）

| 原则 | 落实 |
|---|---|
| **CloudManager = transport layer** | ✅ 只**新增 1 个窄上行接口** `cloud_send_telemetry(const char* json)`；**不新增** action type / message type / topic / serializer |
| **Publisher = data producer** | ✅ 决策（该不该发、发什么）**全部在 publisher / policy**；CloudManager **不知道** `type` 的语义 |
| **不复用 command uplink** | ✅ 绕开 `cloud_compress_uplink()`（短键 `c/i/s/r` 是**命令结果**专用）；**不得**把 telemetry 包成 `{"cmd":"result",…}` |
| **不引入第二套 telemetry policy** | ✅ **唯一权威仍在设备侧**（`WP1:§1`）；EMQX 只过滤、Worker 只 ingest/storage —— **本方案不向 EMQX/Worker 转移任何决策** |
| **不破坏既有上行** | ✅ `cloud_send_up()` / `cloud_send_set()` / `cloud_send_log()` **签名与语义零改动** |

> **⚠️ 一个必须显式登记的副作用**：`/up` 上**同时存在**（a）命令结果的 `c/i/s/r` 与（b）telemetry 的长键 JSON ⇒ **消费侧必须能区分**。`DT:WP-4` 的 ingest 对「无 `ts`/`type`/`value`/`seq` 的记录」会**逐条 `rejected`** ⇒ 命令结果会被拒（**见 `F-WP4-FW-2` / `H-WP4-FW-01`**）。

### 3.4 资源影响评估（Telemetry）

| 维度 | 评估 | 量级 |
|---|---|---|
| **RAM** | publisher 静态状态（last 值/时间戳/计数器/迁移态）≈ **64–128 B**；信封文本无固定大缓冲（单条 ≤ 200 B，可用栈上小缓冲，**< 1 KB ⇒ 不触「>1KB 禁上栈」红线**） | **≈ +100 B DRAM** |
| **Flash** | `telemetry_publisher.cpp`（决策 glue + 状态读取）≈ **0.6–1.2 KB**；`telemetry_policy.h`（纯函数 + 阈值常量）≈ **0.2–0.5 KB**；配置键解析 + getter ≈ **0.3–0.6 KB** | **≈ +1.1 ~ +2.3 KB** 🔴 |
| **PSRAM** | 无新增 | 0 |
| **CPU** | 稳态：每轮 1 次 `millis()` 比较（纳秒级）；事件驱动：每次事件 1 次组包（微秒级）；15 min 抽样：可忽略 | **≈ 0** |
| **MQTT payload** | 单条 ≈ **120–200 B**（远小于任何上限） | 可忽略 |
| **上行频次（理论）** | 事件 < 100/天 · valve < 500/天 · weight < 300/天 · climate < 300/天 · online < 50/天 · health ≈ 0（稳态） | 满足 `WP1` 的单设备 **< 1200 行/天**（**须 WP-6 实测确认**） |

---

## §4 File Impact Analysis

### 4.1 拟改动/新增文件（**固件仓**）

| 文件 | 类型 | 修改原因 | 风险 |
|---|---|---|---|
| `src/log/log_envelope.h` | **新增** | `Base64` + JSON 信封（`L-A`）+ 容量预检；**freestanding ⇒ 主机可测** | 🟢 低（纯函数、无状态） |
| `src/log/log_manager.cpp` | **修改** | ⑤序列化/⑥上传 两步替换为逐条信封；新增 `LOG_ENV_MAX_BYTES` 预检 | 🟠 中（**唯一 wire 出口** ⇒ 必须保持 `inflight`/重试/ACK/水位/段回收**逐字不变**） |
| `src/log/log_manager.h` | **修改** | 新增常量/观测字段（**不动既有常量语义**） | 🟡 中低 |
| `src/log/log_cbor.h` | **零改动（格式）** | — | — |
| `src/log/log_events.h` | **⛔ 零改动（冻结契约 + `static_assert`）** | — | 🔴 **红线** |
| `src/log/log_ack.h` | **零改动** | — | — |
| `src/cloud/cloud_manager.cpp` | **修改（+~6 行）** | 新增 `cloud_send_telemetry()`；`cloud_send_log()` 的载荷**形态**改为 UTF-8 信封（**函数仍发布字节**） | 🟡 低（不触碰既有 4 个 API 语义） |
| `src/cloud/cloud_manager.h` | **修改（+1 声明）** | 声明新接口 | 🟢 低 |
| `src/services/telemetry_publisher.{h,cpp}` | **新增** | 数据生产者（读 System State → 组信封 → 交传输） | 🟡 中（新模块，须遵守「零裸全局 / 状态走 System State / 非阻塞」） |
| `src/services/telemetry_policy.h` | **新增** | **纯决策函数**（阈值/节流/边沿/迁移/健康） | 🟢 低 |
| `src/services/config_manager.{h,cpp}` | **修改** | 新增 `mqtt.json.telemetry` 键与 getter（`WP1:§5 H-W1-5`：**不新增 Config 模块**） | 🟡 低中 |
| `data/config/mqtt.json` | **修改** | 新增 `telemetry` 默认键 | ⚠️ **`data/` 已 gitignore** ⇒ 仓库有值 ≠ 设备端生效（**已知坑**） |
| `src/main.cpp` | **修改（+2 行接线）** | `telemetry_publisher_init()`（`setup`）+ `telemetry_task()`（`loop`，置于 `MiThermometer_task()`/`weight_task()` **之后**以保证当轮状态新鲜） | 🟡 低（只插入调用，**不重排既有顺序**） |
| `platformio.ini` | **⛔ 零改动（推荐）** | 不新增依赖（Base64 自持实现，避免引库 + 保持可复现构建） | — |
| `test/log_envelope_host/*` | **新增** | 信封/Base64 主机探针 + 运行脚本（`wasm32` 真跑） | 🟢 低 |
| `test/telemetry_policy_host/*` | **新增** | policy 决策探针（阈值/节流/迁移） | 🟢 低 |
| `test/mqtt_log_probe.py` | **修改（建议）** | 新增「信封解码 → 断言逐条键」模式 | 🟢 低 |

### 4.2 明确「零改动」清单（**红线**）

| 必须零改动 | 理由 |
|---|---|
| `src/log/log_events.h` | **冻结契约 + 编译期 `static_assert`**（改动 = 改变已冻结的 wire/存储契约） |
| `src/homie/**` · `src/automation/**` · `src/storage/**` · `src/app/**`（除接线） | 与本次两条工作流**无职责交集** |
| `src/cloud/mqtt_transport.{h,cpp}` · `mqtt_routing.h` | P2-2 Facade 边界；**禁止**业务模块借用 `cloud_transport_publish()` |
| Topic 模板 / ACL（`topic_renderer.h` · EMQX 侧） | `H-WP4C-01`/`13` 冻结 |
| `partitions.csv` · `platformio.ini`（依赖） | 保持可复现构建；LittleFS 余量充足（62 KiB / 11.94 MiB） |
| `migrations/`（CF 仓）· Worker 代码 · EMQX 配置 | **不在本工作流范围** |

### 4.3 协议影响（Protocol）

| 协议面 | 是否变更 | 说明 |
|---|---|---|
| **MQTT topic** | ❌ **不变** | 三模板零改动；**不新增 topic、不新增订阅、不改 ACL** |
| **Envelope（上行载荷形态）** | ✅ **变**（`/log` 由 CBOR → JSON 信封；`/up` 新增 telemetry JSON） | ⚠️ **须同步 `docs/interfaces/cloud_protocol.md` §8.2**（当前记的是 CBOR 批次）—— **文档更新须人工批准**（见 `H-WP4-FW-09`） |
| **ACK（`log_ack`）** | ❌ **报文不变** | `{"c":"log_ack","i":…,"p":{"b":…,"f":…,"t":…}}`（`log_events.h:550-555`）**保持不变**；⚠️ **但生产环境当前无人产生它**（见 `F-WP4-FW-1`） |
| **Command protocol（下行）** | ❌ **不变** | 不新增命令、不新增 `c` 值；telemetry **不注册** CommandManager Action |
| **Command result（`/up` 上行）** | ⚠️ **被动受影响** | 载荷本身不变，但**EMQX Action 切换后会被新端点 `rejected`** ⇒ `F-WP4-FW-2` |
| **`$description` / Homie 投影** | ❌ **不变** | Homie 正交，不受影响 |

### 4.4 存储影响（Storage）

| 面 | 影响 |
|---|---|
| **LittleFS `/log`** | ✅ **不变**（段环 16 段 / 62.2 KiB；`meta.bin` 不变） |
| **BinStorage / JsonStorage / FileStorage** | ✅ **零改动** |
| **Config** | ⚠️ **新增 1 组键**（`mqtt.json.telemetry`）⇒ 走既有「业务管合法性 → ConfigManager → JsonStorage」链；**Cloud 改动一律重启生效** |
| **NVS（`gfid`/`gfcred`）** | ✅ 零改动 |
| **D1（云端）** | ✅ 零改动（表/索引/retention **不动**） |

### 4.5 测试设计（Testing）

| 层 | 测试 | 期望 |
|---|---|---|
| **`L1` 主机（wasm32 真跑）** | `log_envelope` 探针：① 信封 JSON 形状逐字符 ② `Base64` 标准向量（含 `=` 填充 / 0/1/2 余数）③ 容量预检（不足 ⇒ 0）④ 单条 vs 数组容器 ⑤ `level` 串词表 | **逐字节可比** |
| **`L1` 主机** | `telemetry_policy` 探针：阈值边界（= / < / >）、15 min 节流边界、边沿去重、`$state` 迁移表、健康异常 | 纯函数 ⇒ **可穷举** |
| **`L2` 契约回归** | `python test/log_contract/run_contract_test.py`（4 个探针，含 `probe_bad` 可证伪）**必须仍全绿** | 证明 `log_events.h`/`log_cbor.h`/`log_ack.h` 契约未被破坏 |
| **`L3` 上板（既有套件）** | `test/log_*.txt`（core / flash / meta / p15 ack / giveup / p4 dispense）**全部必须仍通过** | 证明 ACK/补发/段回收/放弃语义未变 |
| **`L4` 联调（本地）** | 设备 → 本地 EMQX 规则 → `wrangler dev --local` 的 `POST /api/history/ingest` → **本地真实 D1** 断言行数（**复用 `test/real_d1/history_ingest_real_d1.sh` 的双 server 模式**） | 端到端 `accepted` 计数 + 幂等重投 |
| **`L4'` MQTT 探针** | 扩展 `test/mqtt_log_probe.py`：订阅 `/log` → 解信封 → `base64` → 解 CBOR 头 → 断言 `(boot_seq, seq)` 逐条 | 无 D1 也能验证 wire 正确性 |
| **`L5` 负向** | 超大体量（> 缓冲）· `ts=0`（时间未同步）· `weight.enable=false` · `mi_thermo` 未配置 · MQTT 离线 | **不得崩溃 / 不得静默丢 WARN+ / 不得产生 heartbeat** |
| **`L6` 资源** | `pio run` 段尺寸对比（+Flash 增量）+ 与 **14 KiB ceiling** 对账 | **超限即停下报告（不得自行改 ceiling）** |

---

## §5 Protocol Impact Analysis（**汇总口径**）

| 项 | 现状 | 目标 | 兼容性 |
|---|---|---|---|
| `/log` 载荷 | CBOR 二进制（12 项数组 + `bstr(128)`） | **UTF-8 JSON 信封数组**（逐条 `L-A`） | ⚠️ **破格式**：`/log` 的**唯一消费者**是 EMQX→Worker 链路；旧链路（若仍有旁路订阅者）**会读到 JSON** ⇒ 须确认无第三方订阅者（见 `H-WP4-FW-09`） |
| `/up` 载荷 | 命令结果（短键 JSON） | 命令结果 **+** telemetry 长键 JSON（**混流**） | ⚠️ **须裁决区分机制**（`H-WP4-FW-01`） |
| `/down` | 命令 + `log_ack` | **不变** | ✅ |
| 幂等键 | telemetry `(device_id,type,ts,seq)` · log `(device_id,boot_seq,seq)` | **不变** | ✅ |
| `schema_version` | 缺省 1 · `>1` 原样保存 | 设备侧**显式携带 `schema_version`**（建议） | ✅ 前向兼容 |
| `log_ack` | 报文不变 | 报文不变 | ⚠️ **产生方缺失**（`F-WP4-FW-1`） |

---

## §6 Risk Assessment

### 6.1 冻结约束检查（Task 4）—— **见 Executive Summary 表**（含 5 项判定与依据）

### 6.2 ★ 设计期发现（`F-WP4-FW-*` · **只登记，不自行解决**）

| ID | 级别 | 发现 | 影响 |
|---|---|---|---|
| **`F-WP4-FW-1`** | 🔴 **高** | **无云端 `log_ack` 产生方 ⇒ Flash 段环永不回收**。段回收守卫 `s_cloud_gc_seq == 0 ⇒ 不回收`（`log_manager.cpp:2383`），而 `gc_seq` **只**由 `log_ack` 推进（`:2433`）；已核实 **Worker 明确不含 `log_ack`**（`history-ingest.js:183`：属 `计划:WP-5`）+ **EMQX 仓零 `log_ack` 字样**。 | **496 条硬上限**：写满后 WARN+ 追加持续失败 ⇒ RAM 环背压 ⇒ 最终 `ring_drop`。**这是 `BT-1` 的直接后果（`G-4 = BLOCKED`）**，但**信封化会把它的严重性放大**（新链路同样是"只发不收"） |
| **`F-WP4-FW-2`** | 🔴 **高** | **`/up` 混流**：命令结果（`c/i/s/r`，`cloud_manager.cpp:1692-1699`）与 telemetry 共用 `/up`；ingest 对缺 `ts/type/value/seq` 的记录**逐条 `rejected`**（`DT:WP-4` §5.2）⇒ **命令结果被拒**；且 EMQX Action 切到新端点后，旧 `/api/ingest` 的 `mqtt_messages` 记账链路**断开** | 云端**命令结果可观测性丧失 + 记账中断**（P1-6 `credential_set` 帧等）；且产生持续 `rejected` 噪声 |
| **`F-WP4-FW-3`** | 🟠 **中高** | **信封标量 vs 批次多记录**：冻结信封的 `boot_seq`/`seq`/`ts`/`level` 是**标量**，而 CBOR 批次含 ≤16 条**异质**记录（级别/时间戳可不同） | 只有 `L-A`（逐条）能同时满足「逐条幂等键」+「WP-2 逐行语义」⇒ **决定带宽 ×2.77**（§2.3） |
| **`F-WP4-FW-4`** | 🟠 **中** | **`mqtt.json.telemetry` 从未落地**（固件零 `telemetry` 字样） | `WP1:§5 H-W1-5` 冻结项**未实现** ⇒ telemetry 的开关/阈值**无处可配** |
| **`F-WP4-FW-5`** | 🟡 **低中** | `MiThermometer` **无公开 getter**（只能经 System State） | 设计上可接受（System State 即接口），但**取值时序**须注意（`MiThermometer_task()` 在 `loop` 中位于 `telemetry_task()` 之前更佳） |
| **`F-WP4-FW-6`** | 🟡 **低** | **工程内无 base64 实现**；工具链**有** mbedtls（`MiThermometer.cpp:9` 用 `mbedtls/ccm.h`） | 建议**自持 freestanding 实现**（可主机验证 + 零新依赖 + `--gc-sections` 友好） |
| **`F-WP4-FW-7`** | 🟠 **中** | **`lost` 无法由设备侧产生**：`lost` 是 **broker LWT**（`WP1:§3.6` / P1-6），设备无法自报告；而 LWT topic 是 `homie/5/<id>/$state`，**不在 `/up`+`/log` 入口族内** | `online` 类的**四态词表在设备侧只能覆盖 3 态**（`init`/`ready`/`disconnected`）⇒ `lost` 须由**云端 presence**产生（**不属本工作流**）⇒ 需裁决 |
| **`F-WP4-FW-8`** | 🟡 **低** | `test/mqtt_log_probe.py` 头注释仍写扁平 `guo_feeder/log`（历史遗留；实际已参数化为含 `<device_id>`） | 仅文档瑕疵；**信封化后该脚本必须同步升级**（否则无法解码新载荷） |
| **`F-WP4-FW-9`** | 🟠 **中** | **INFO 必然被 ingest 逐条 `rejected`**：Level Policy 冻结 `INFO → Cloud=YES`（`log_events.h:66`，即 `{0,1}`），而 `WP2:§4.2` 的 `level` 词表 = `{WARN,ERROR,CRITICAL}` ⇒ INFO 会进入信封但**结构校验失败被拒** | **带宽浪费 + `rejected` 噪声**；`DT:WP-4` 已把该拒绝归类为「结构一致性校验」（非业务过滤）⇒ **合规但不经济**；优化选项（设备侧历史出口过滤）**会改变 INFO 的云端可达性** ⇒ 需裁决 |
| **`F-WP4-FW-10`** | 🟡 **低** | 侧信道计数（`drop_ring/overflow/unacked/degraded`）与 `flags.seq_reliable` 当前放在 **CBOR 批次头**（`log_cbor.h:104-109`） | 信封化后**这些字段无处安放** ⇒ 需明确「保留（放在逐条 CBOR 头里 → 每批重复 10 次）或迁移到信封层（需新增字段 ⇒ 触碰冻结）」⇒ 见 `H-WP4-FW-03` |

### 6.3 风险清单（`R-WP4-FW-*`）

| ID | 级别 | 风险 | 护栏 |
|---|---|---|---|
| **`R-WP4-FW-1`** | 🔴 **高** | **Flash 预算不足**：两条工作流合计 Flash 增量预计 **+1.4 ~ +3.1 KB**，而余量 **仅 1,788 B** ⇒ **极可能超 `≤14 KiB` ceiling** | **`S7` 强制实测**；**超限即停下报告**（**不得自行改 ceiling** —— P2-0.2 修订属架构级变更） |
| **`R-WP4-FW-2`** | 🔴 **高** | **切换即断链**：EMQX Action 切到新端点后，命令结果链路与 `mqtt_messages` 记账**同时受影响**（`F-WP4-FW-2`） | **`H-WP4-FW-01` 裁决**后再动 EMQX（Phase C 的 `V-4` 亦依赖此） |
| **`R-WP4-FW-3`** | 🔴 **高** | **段环饱和**：无 ACK ⇒ 496 条上限（`F-WP4-FW-1`） | 短期：监控 `flash_append_fail` / `ring_drop` / 段占用；长期：`计划:WP-5`（`BT-1` / `G-4`） |
| **`R-WP4-FW-4`** | 🟠 **中高** | **带宽 ×2.77**（`L-A`）⇒ MQTT 上行字节数与 EMQX 规则处理量上升 | 若不可接受 ⇒ 需重裁「粒度/是否允许信封级共享 `device_id`」（`H-WP4-FW-02`/`03`） |
| **`R-WP4-FW-5`** | 🟠 **中** | **缓冲不足**：4,096 B 只能容纳 ≤10 条信封 | 提升至 8,192 B（PSRAM，+4 KB）或降批大小（`H-WP4-FW-04`） |
| **`R-WP4-FW-6`** | 🟠 **中** | **遥测静默失效**：配置键缺失时若默认"关闭"⇒ **不烧新固件就完全无 telemetry**（**已知坑**：配置字段只对"读过该字段的固件"生效） | 明确默认值策略（`H-WP4-FW-06`）+ 启动期日志可见性 |
| **`R-WP4-FW-7`** | 🟠 **中** | **幂等碰撞**：`ts=0`（时间未同步）+ `seq` 每次开机归零 ⇒ 同 `(device_id,type,ts,seq)` ⇒ **后到者被静默吸收** | 裁决 `seq` 的持久化/递增语义（`H-WP4-FW-07`）；或要求 `ts` 无效时**不产生** telemetry |
| **`R-WP4-FW-8`** | 🟠 **中** | **telemetry 无离线兜底**（fire-and-forget）⇒ 离线期间数据丢失（与 WARN+ 日志不同，**无 Flash 保命**） | 明确接受或引入受限缓冲（`H-WP4-FW-08`）；**不得**因此改动 LogManager 的存储语义 |
| **`R-WP4-FW-9`** | 🟡 **低中** | **`weight.enable` 三方不一致**（`P2-8-KI-2`，未修复）⇒ weight telemetry 是否存在**不可判定** | `WP6` 实测前先确认真机取值；publisher **必须**读 `STATE_WEIGHT_VALID` |
| **`R-WP4-FW-10`** | 🟡 **低中** | QoS1 + `store=true` 在**链路抖动**期可能堆积 outbox | 保持现状（`cloud_send_log` 离线即失败 ⇒ 不排队）；观测 `esp_mqtt_client_get_outbox_size()` |
| **`R-WP4-FW-11`** | 🟡 **低** | `/log` 破格式 ⇒ 若有**第三方订阅者**（如既有 MQTT 探针/仪表盘）会解析失败 | 变更前确认消费者清单（`H-WP4-FW-09`） |

---

## §7 Implementation Plan

### 7.1 阶段与顺序（**本轮不执行**）

| 阶段 | 内容 | 前置 | 产出/判据 |
|---|---|---|---|
| **`S0` 决策门** | 裁决 `H-WP4-FW-01…09` | 人工 | 每一项均有明确答案 |
| **`S1` 冻结线核对** | 逐条确认「零改动清单」（§4.2） | `S0` | `log_events.h`/`log_cbor.h`(格式)/topic/ACL **未动** |
| **`S2` `log_envelope.h` + 主机探针** | 信封 + Base64 + 容量预检（`freestanding`） | `S0`（`02`/`03`/`04`） | `L1` 全绿（**先离线验证字节**） |
| **`S3` `log_manager` 接线** | ⑤⑥ 替换为逐条信封；缓冲调整；**`inflight`/重试/ACK/水位/段回收逐字不变** | `S2` | `L2` 契约回归全绿 + `L3` 上板套件全绿 |
| **`S4` `cloud_send_telemetry()` 窄接口** | +~6 行；不经压缩 | `S1` | 既有 4 个 API 行为不变 |
| **`S5` `telemetry_policy.h` + 主机探针** | 阈值/节流/边沿/迁移/健康 | `S0`（`05`/`06`/`07`） | `L1` 全绿 |
| **`S6` publisher 接线 + Config 键 + `main.cpp`** | 事件订阅 + `telemetry_task()` | `S4`/`S5` | `L5` 负向通过；无 heartbeat |
| **`S7` 资源对账** | `pio run` 段尺寸 vs **14 KiB ceiling** | `S3`/`S6` | **≤ 14,336 B**；否则**停下报告** |
| **`S8` 端到端（本地）** | 设备/探针 → 本地 EMQX 规则 → 本地 `wrangler dev` → **本地真实 D1** | `S3`/`S6` + 人工授权（EMQX 侧） | `accepted` 计数 + 幂等重投断言（复用 `test/real_d1/*` 模式） |
| **`S9` 文档同步** | `cloud_protocol.md §8.2`（载荷形态）等 | 人工批准 | **不擅自改文档**（`H-WP4-FW-09`） |

### 7.2 验收（四态严格区分）

| 项 | 目标态 | 判定方式 |
|---|---|---|
| `log_envelope` 字节级 | `PASS` | 主机 `wasm32` 真跑（`L1`） |
| 契约/回归 | `PASS` | `L2` + `L3` 全绿 |
| on-board 端到端（envelope → D1） | `PASS` | `L4`（**须真实 D1 行数断言**） |
| telemetry policy | `PASS` | `L1`（纯函数穷举） |
| telemetry 端到端 | `PASS` | `L4` |
| Flash 预算 | `PASS` / **`FAIL`** | `L6` + `S7` |
| **当前** | **`NOT RUN`** | **全部未执行（本轮仅设计）** |

### 7.3 提交策略（**本轮不提交**）

- 代码与文档分离；**每阶段独立 commit**；**禁 `git add -A`/`.`**；提交前 `git diff --cached --name-only` 全量核对；**不含** `log_ack`（`计划:WP-5`）· 不含 EMQX 配置 · 不含 migration。
- **本轮：零 commit、零 push**（按任务要求）。

---

## §8 Open Questions / Human Decisions

> **纪律**：**发现设计冲突不自行解决** —— 以下全部为**待人工裁决**，**本文档不代裁决**。

| ID | 问题 | 为什么必须人工裁决 | 关联 |
|---|---|---|---|
| **`H-WP4-FW-01`** | **`/up` 混流处置** —— 命令结果（`c/i/s/r`）与 telemetry 共用 `/up`。候选：**(a)** EMQX 侧按载荷结构分流（两条 Action：命令结果 → 旧 `/api/ingest`；telemetry → `/api/history/ingest`）；**(b)** 只切 `/log`，`/up` **暂不切**（telemetry 顺延）；**(c)** 接受命令结果被 `rejected`（**丢记账**）；**(d)** telemetry 改用 `/up` 之外的入口（**触碰 `H-WP4-01`/`13` 冻结 ⇒ 须重裁**） | 决定 Phase C 的 `V-2`/`V-4` 能否成立；**涉及跨组件回归（云侧记账）** | `F-WP4-FW-2` · `R-WP4-FW-2` |
| **`H-WP4-FW-02`** | **信封粒度** —— 逐条（`L-A`，**推荐**）vs 整批（`L-B`，破坏逐行语义） | 决定幂等键/行语义是否成立 | `F-WP4-FW-3` · §2.2 |
| **`H-WP4-FW-03`** | **`payload_base64` 的字节边界** —— ① **单条 CBOR（`count=1` 的 188 B）**（推荐，格式零改动）② 整批 CBOR ③ 裸 128 B `LogRecord`；**并含**侧信道计数（`drop_*`/`seq_reliable`）与 `device_id` 是否**每信封重复** | 二者决定**带宽**与**是否触碰冻结**（③/② 的组合可能形成"第二套 log protocol"） | `F-WP4-FW-3`/`10` · `R-WP4-FW-4` |
| **`H-WP4-FW-04`** | **工作缓冲与每报文条数** —— 4,096 B（N≤10）vs **8,192 B（N=16，+4 KB PSRAM）**；以及 `LOG_ENV_MAX_BYTES` 与既有 `LOG_BATCH_MAX_PAYLOAD` 的关系（**不得改 `log_events.h` 常量语义**） | 资源取舍 + 契约边界 | `R-WP4-FW-5` |
| **`H-WP4-FW-05`** | **telemetry `type` 封闭词表 + `value` 编码**（JSON number / string？单位？小数位？）与 `schema_version` 的初值 | `WP2:§3.3` 把解释权交给 `schema_version` ⇒ **必须给出封闭词表**，否则云侧无法消费 | §3.2 · `F-WP4-FW-4` |
| **`H-WP4-FW-06`** | **`mqtt.json.telemetry` 的字段集与默认值**（`enable` / `weight_threshold_g` / `weight_sample_min` / `climate_*` / `health_*`）—— `WP1:§5 H-W1-5` 只冻结了**键名** | 实现前必须知道字段与默认值；默认值直接决定「烧不烧新固件」的行为 | `R-WP4-FW-6` |
| **`H-WP4-FW-07`** | **telemetry `seq` 的语义** —— 单调计数是否**跨 boot 持久化**；`ts` 无效（未同步）时是**上报 `0`** 还是**不发** | 直接决定幂等碰撞与静默丢数据（`ts=0` + `seq` 归零 ⇒ 被 `ux_th_dedup` 吸收） | `R-WP4-FW-7` |
| **`H-WP4-FW-08`** | **Flash 预算处置** —— 若 `S7` 实测超 `≤14 KiB`：① 重裁 ceiling（**架构级变更**）② 拆 WP 分阶段 ③ 削减功能（如暂不做 telemetry） | **不得由实施方自行决定**（P2-0.2 属冻结架构条款） | `R-WP4-FW-1` |
| **`H-WP4-FW-09`** | **文档与消费者清单** —— ① `cloud_protocol.md §8.2` 是否更新（**载荷形态已变**）② `/log` 是否有第三方订阅者 ③ `log_ack` 产生方（`计划:WP-5`）何时补齐 ④ INFO 的历史出口是否优化（`F-WP4-FW-9`） | 跨文档/跨组件；`log_ack` 涉及 `G-4` | `F-WP4-FW-1`/`8`/`9` · `R-WP4-FW-3`/`11` |

### 8.1 裁决状态回填（2026-10-11）

| ID | 主题 | 裁决状态 | 留痕 |
|---|---|---|---|
| **`H-WP4-FW-01`** | `/up` 混流处置 | **`CLOSED`** | §9.1 |
| **`H-WP4-FW-02`** | Log Envelope 粒度 | **`CLOSED`** | §9.2 |
| **`H-WP4-FW-03`** | `payload_base64` 边界 | **`CLOSED`** | §9.3 |
| **`H-WP4-FW-04`** | 缓冲数量 | **`CLOSED`** | §9.4 |
| `H-WP4-FW-05` | telemetry `type` 词表 / `value` 编码 | **`OPEN`** | 未裁决 |
| `H-WP4-FW-06` | `mqtt.json.telemetry` 字段与默认值 | **`OPEN`** | 未裁决 |
| `H-WP4-FW-07` | telemetry `seq` 语义 | **`OPEN`** | 未裁决 |
| **`H-WP4-FW-08`** | Flash 预算处置 | **`CLOSED WITH GUARD`** | §9.5 |
| `H-WP4-FW-09` | 文档与消费者清单 | **`OPEN`** | 未裁决 |

> **★ 状态口径**：`CLOSED` = **治理 / 设计决策已定**，**≠ 阻塞解除 ≠ 实施完成**；`CLOSED WITH GUARD` = **允许实施，但附带强制资源门禁**（§9.5）。**`05` / `06` / `07` / `09` 仍为 `OPEN`（`HUMAN_REQUIRED`）**。

---

## §9 Human Decisions（人工裁决留痕 · 2026-10-11）

> **性质**：**人工裁决留痕** —— 记录 `WP4-FW` 设计评审（`b813022`）的裁决结果，形成**实施前冻结输入**。
> **⚠️ 本轮仅记录裁决**：**未改任何 `src/` · 未改 Worker · 未改 EMQX · 未建 migration**；**未改变 `Phase C` 状态、未改变任何 Gate、未进入固件实现**。

### 9.1 `H-WP4-FW-01` —— `/up` 混流处置 = **`CLOSED`**

**Decision**：**采用方案 A：双 Action 分流。**

**冻结规则**：
- **MQTT topic / domain separation 优先于 payload inspection。**
- **Worker 不负责 command / telemetry 判断。**
- **Worker 不解析 payload 进行分流。**
- **EMQX Rule 层负责根据 topic / message metadata 分流。**
- **不新增 Worker policy。**
- **不改变 `DT:WP-4` ingestion contract。**

**状态**：`H-WP4-FW-01 = CLOSED`

> **承接提示**：本裁决冻结「**分流职责在 EMQX Rule 层、Worker 不解析 payload**」；telemetry 与命令结果的**分离所依据的 topic / domain 边界**由 Phase C 的 EMQX Rule 设计承接（`H-WP4C-03` = `OPEN`）。关联 `F-WP4-FW-2` · `R-WP4-FW-2`。

### 9.2 `H-WP4-FW-02` —— Log Envelope 粒度 = **`CLOSED`**

**Decision**：**采用 `L-A`：逐条 Cloud Log Envelope。**

**冻结规则**：
- **一个 MQTT log message 对应一个 `log_history` record。**
- **`payload_base64` = 单条 `LogRecord` CBOR bytes 的 Base64。**
- **不做 batch。**
- **不新增字段。**
- **不修改 Worker。**

**状态**：`H-WP4-FW-02 = CLOSED`

### 9.3 `H-WP4-FW-03` —— `payload_base64` 边界 = **`CLOSED`**

**Decision**：**冻结**：`payload_base64` = **完整 CBOR `LogRecord` bytes**。

**禁止**：
- **二次 JSON 化**
- **字段映射**
- **截断**
- **自定义 envelope 内部字段**

**状态**：`H-WP4-FW-03 = CLOSED`

### 9.4 `H-WP4-FW-04` —— 缓冲数量 = **`CLOSED`**

**Decision**：**首版采用 `count = 1`。**

**原因**：
- **无 `log_ack`**
- **Flash 回收机制未完成**
- **首版优先闭环可靠性**

**状态**：`H-WP4-FW-04 = CLOSED`

### 9.5 `H-WP4-FW-08` —— Flash budget = **`CLOSED WITH GUARD`**

**Decision**：**允许进入实施，但增加资源门禁。**

**冻结规则**：
- **实施过程中必须执行 firmware size report。**
- **若 Flash 增量超过预算，必须停止并请求人工裁决。**
- **禁止自行删除功能。**
- **禁止修改冻结协议。**
- **禁止降低日志可靠性。**

**状态**：`H-WP4-FW-08 = CLOSED WITH GUARD`

> **门禁落点**：对接 §7.1 的 **`S7`（资源对账）** —— `≤ 14,336 B` 判 `PASS`；**超限即「停下报告」**，**不得自行改 ceiling / 删功能 / 降可靠性**。

### 9.6 裁决后的残留（`HUMAN_REQUIRED`）

| 项 | 说明 |
|---|---|
| **`H-WP4-FW-05`** | telemetry `type` 封闭词表 + `value` 编码 + `schema_version` 初值（`OPEN`） |
| **`H-WP4-FW-06`** | `mqtt.json.telemetry` 字段集与默认值（`OPEN`） |
| **`H-WP4-FW-07`** | telemetry `seq` 语义（跨 boot 持久化？`ts` 无效时上报 `0` 或不发）（`OPEN`） |
| **`H-WP4-FW-09`** | 文档同步 / 第三方订阅者 / `log_ack` 补齐 / INFO 出口（`OPEN`） |
| **`log_ack`** | 属 `计划:WP-5`，受 `G-4`（仍缺） |
| **`H-WP4C-03/04/05/06`** | EMQX Rule 范围 / 远端 `0003` 授权 / Action 切换授权 / 部署顺序（`OPEN`） |

> **Gate 未变**：`G-1 HUMAN_REQUIRED` · `G-2 PASS` · `G-3 PASS` · `G-4 BLOCKED` · `G-5 PASS`。

---

> **`§9` = 人工裁决留痕（2026-10-11）**：**`H-WP4-FW-01` / `02` / `03` / `04` = `CLOSED`** · **`H-WP4-FW-08` = `CLOSED WITH GUARD`** ⇒ **实施前冻结输入已形成**。
> **本轮 = `DESIGN REVIEW ONLY`（未实施）。** **No firmware change. No Worker change. No EMQX change. No migration. No DDL. No deploy. No push.**
> **`WP4-FW-LOG` = `CONDITIONAL`**（裁决后 **`01` / `08` 已 `CLOSED`**；**残留硬前置 = `log_ack`（`计划:WP-5` · 受 `G-4`）**）**· `WP4-FW-TELEMETRY` = `CONDITIONAL`**（裁决后 **`/up` 混流已 `CLOSED`**；**残留 = `H-WP4-FW-05` / `06` / `07`**）。
> **`CF-1` / `CF-2` = `BLOCKED` · `C-1 V-2` / `V-3` = `NOT RUN / BLOCKED` · `Phase C` = `PLANNED / BLOCKED`（均未变）。**
> **No Gate changed. `P3` remains `NOT STARTED / PLANNED`.**

---

*本文档于 **2026-10-11** 建立，为 **`DT:WP-4` 的固件侧实施前设计评审**（`WP4-FW-LOG` + `WP4-FW-TELEMETRY`）；
`§1–§8` 只做**分析、影响评估、风险登记与计划**，**不含任何代码 / 配置改动**（评审基线 commit = `b813022`）。
`§9` = **人工裁决留痕**（在 `b813022` 之后追加），形成**实施前冻结输入**，仍**不含任何实现**。
基线 = 固件仓 HEAD **`b813022`**（`src/` 与 `4b6f6ed` 一致，工作树 clean）· CF 仓 **`9b25203`** · EMQX 仓 **`eec9444`**。
所有 `file:line` 引用均取自基线，可逐条回查。*
