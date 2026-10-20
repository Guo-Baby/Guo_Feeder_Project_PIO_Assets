# Log 信封双层契约 —— 「设备 MQTT 日志信封」vs「EMQX → Worker HTTP 接收信封」

> ## 📄 契约澄清文档（`docs-only` · **只描述已实现的现状**，不含任何实现变更）
>
> - **本文解决的问题**：`/log` 链路上存在**两层含义不同的「信封」**，长期被混为一谈 —— 这正是 **`F-C2-9`**（真实 `/log` 恒判 `unparsable_payload`）的根因。
> - **本文不做**：不改固件实现 · 不改 Worker · 不改 EMQX · 不改 D1 schema / migration · 不部署。
> - **权威优先级**：**代码 + 实测 > 本文 > 其他文档**。本文所有行为均可在代码与测试中复核（§8 依据）。
> - **状态**：`ACTIVE`（`2026-10-15` 建立）。

---

## §0 一句话区分

| 层 | 位置 | 「信封」指什么 |
|---|---|---|
| **第一层** | 设备 → MQTT topic `guo_feeder/<device_id>/log` 的 **payload** | **设备 Log envelope**（6 字段 JSON 对象） |
| **第二层** | EMQX HTTP Action → Worker 的 **HTTP body** | **接收信封** `{topic, clientid, payload}`，其中 `payload` **就是**第一层那 6 字段对象 |

> **★ 关键**：**第一层信封是第二层 `payload` 的值**。设备**不**把字段摊平到 HTTP body 顶层；**HTTP body 顶层只有 3 个键**。

---

## §1 全景链路

```
设备（LogManager）
  └─ publish  topic = guo_feeder/<device_id>/log
              payload = { device_id, boot_seq, seq, ts, level, payload_base64 }   ← 第一层信封
        │
        ▼
EMQX Rule（history_ingest 域）
  └─ HTTP Action  POST https://guo-feeder-pagesdev.pages.dev/api/history/ingest
        body = {"topic":"${topic}","clientid":"${clientid}","payload": ${payload}}  ← 第二层信封
        │
        ▼
Pages 中转（逐字转发，不解析 JSON）
  └─ POST https://guo-feeder-api.guobaby.workers.dev/api/history/ingest
        │
        ▼
Worker `history-ingest.js`
  └─ 解包 `body.payload`（第二层） → 逐条校验（第一层字段） → D1 `log_history`
```

---

## §2 第一层：设备 MQTT 日志信封

**Topic 模板**：`guo_feeder/<device_id>/log`（`GF_TOPIC_TPL_LOG` · `src/services/topic_renderer.h`）
**生成位置**：`src/log/log_envelope.cpp` · `log_envelope_encode_cbor()`
**发布入口**：`cloud_send_log()`（`src/log/log_manager.cpp` · CloudManager 只做 transport，不解释载荷）

**字段（顺序即契约 · 逐条 · `count = 1`）**：

| 字段 | 类型 | 说明 |
|---|---|---|
| `device_id` | string | 12 位小写 hex；**必须与 topic 中的 `<device_id>` 一致** |
| `boot_seq` | uint32 | 设备启动序号（幂等键分量 ②） |
| `seq` | uint32 | **跨 boot 连续**日志序号（幂等键分量 ③）—— 见 §2.0 |
| `ts` | uint32 | Unix 秒 |
| `level` | **uint8（JSON number）** | 见下表 |
| `payload_base64` | string | `Base64(log_cbor_encode_batch(count=1))` —— **业务载荷**，上限 188 B |

> **幂等键（云端）**：`(device_id, boot_seq, seq)` —— **复用冻结唯一索引** `ux_lh_dedup`。

### 2.0 `seq` 语义（**权威 · `2026-10-21` 修订**）

> **本小节是 `seq` 的唯一权威定义**。与任何报告、评审记录、归档文本冲突时，以本小节为准。
> 修订依据：用户书面裁决（`2026-10-21 03:00`，`D-2` = **保持跨 boot 连续，只改文档不改代码**）。

| 项 | 定义 |
|---|---|
| 语义 | **跨 boot 连续**的单调递增日志序号，**不是**本 boot 内从 1 开始的序号 |
| 实现 | `log_manager.cpp` → `next_seq = s_meta_slot_seq + 1`，并持久化回 A/B 双槽（`s_meta_slot_seq` 为所选槽的 `slot_seq`） |
| 重启行为 | **不回绕、不重置** ⇒ 新 boot 的 `seq` 接着上一次继续（真机样例：`boot_seq=94, seq=108289`） |
| 与 `boot_seq` 的关系 | `boot_seq` 标识「第几次启动」，`seq` 是**全局唯一**计数器；两者组合 `(device_id, boot_seq, seq)` 才是幂等键 |
| 唯一性推论 | 因 `seq` 跨 boot 单调，`(device_id, seq)` 本身即近似唯一；**不得**假设"新 boot 从 1 开始" |
| 统计约束 | 任何"按 boot 分组统计 `seq` 区间"的做法**必须**用 `boot_seq` 分组，**不得**用 `seq` 是否从 1 起算来判断新 boot |
| DEBUG 影响 | `DEBUG` 不上云但**仍消耗 `seq`** ⇒ `seq` 空间**天然有洞**，空洞**不是**丢失证据 |

> **修订沿革（不追溯改写历史）**：本文件 `2026-10-21` 之前写作「boot 内日志序号」，
> 与实现不符（登记为 `SEQ-SEM`）。本次**只改文档**，实现未动。
> 历史评审记录（`G4-Production-Change-Plan-Review-2026-10-20.md` §A-8/§5、`G4-Log-Credibility-and-Observability-Report-2026-10-20.md` `B-6-4`）
> **原文保留**，其"文档与实现不符"的判断在当时是**正确**的。

---

### 2.1 `level` 数值契约（**严格** · 权威 = `cloud_protocol.md` §8.2.1）

| 数值 | 语义 | 云端处置 |
|---|---|---|
| `0` | `DEBUG` | **策略过滤** —— **不写** `log_history`，计入 `filtered`（**不**计入 `rejected`） |
| `1` | `INFO` | 接受 ⇒ `log_history.level = 'INFO'` |
| `2` | `WARN` | 接受 ⇒ `'WARN'` |
| `3` | `ERROR` | 接受 ⇒ `'ERROR'` |
| `4` | `CRITICAL` | 接受 ⇒ `'CRITICAL'` |
| 其他整数（`5..255`）· 越界（`>255` / 负数）· 非整数（浮点）· 类型错误（字符串 / 布尔 / `null`）· 缺失 | 未定义 / 非法 | **逐条拒收**（`rejected`） |

> **★ 禁宽松转换**：**不**接受 `"3"`、`"ERROR"`、`true`、`1.0`（浮点形态）、`-1`、`256`；**不得**把上述任何输入"顺带"映射为合法等级。
> **★ 固件上传策略（事实）**：`DEBUG` **不上传**（`cloud = 0`）；`INFO` / `WARN` / `ERROR` / `CRITICAL` **均上传** ⇒ **Worker 确实会收到 `INFO`**（"Worker 不会收到 INFO" 的旧假设已被 `F-C2-8` 更正）。

> **⚠️ 实现容差（非契约承诺）**：`ts` / `seq` / `boot_seq` / `code` 在 Worker 侧经 `asInt()` 解析，**同时接受整数与纯数字字符串**。设备**只应**发整数；该容差仅为兼容历史/调试输入，**不构成协议扩展**。

---

## §3 第二层：EMQX → Worker HTTP 接收信封

**EMQX Action 请求体模板（逐字）**：

```json
{"topic":"${topic}","clientid":"${clientid}","payload": ${payload}}
```

> **★ `${payload}` 不加引号** ⇒ 设备 JSON 对象被**内联**为 `payload` 的**值**（对象），而非字符串。

### 3.1 规范接收形态（**当前预期**）

```json
{
  "topic": "guo_feeder/288485896ce4/log",
  "clientid": "…",
  "payload": {
    "device_id": "288485896ce4",
    "boot_seq": 94,
    "seq": 108289,
    "ts": 0,
    "level": 1,
    "payload_base64": "jAIC…"
  }
}
```

- **外层 `payload`** = **设备日志信封**（§2 全字段）。
- **内层 `payload_base64`** = **日志业务载荷**（CBOR 的 Base64），Worker **不解码**，原样落 `log_history.payload`。
- ⇒ **嵌套对象形态 = 标准接收形式**。任何把字段摊平到 body 顶层的做法都是**兼容形态**，不是"设备 MQTT 原始消息的唯一形式"。

### 3.2 兼容形态（**仅为既有调用方保留**）

顶层直接携带键分量（`payload_base64` 或字符串 `payload` 作为承载）：

```json
{"topic":"…/log","device_id":"…","boot_seq":7,"seq":1,"ts":…,"level":3,"payload_base64":"…"}
```

> **★ 定位**：兼容形态**只为既有扁平调用方**保留；**不得**据此认为设备会发扁平形态 —— 设备**不会**（§2 的 6 字段对象必然被 EMQX 包进 `payload`）。

### 3.3 冲突与一致性（**拒绝优先**）

| 情形 | 处置 | 响应 |
|---|---|---|
| 嵌套对象与**顶层同名键分量**取值冲突（如两边 `payload_base64` / `ts` 不同） | **拒收整条请求**（**不得静默择一**） | `400 {"error":"unparsable_payload"}` |
| 嵌套 `device_id` 与顶层 `device_id` **同时存在且不一致** | **拒收**（防校验读取错位） | `400 {"error":"device_id_mismatch"}` |
| 嵌套/顶层 `device_id` 与之 **topic 解析结果**不一致 | **拒收** | `400 {"error":"device_id_mismatch"}` |
| 嵌套对象**缺少** `device_id` | **接受** —— 身份**只**来自 topic；信封级 `device_id` 仅作**一致性校验**，**非存在性要求** | `200`（正常处理） |

> **★ 身份铁律**：`device_id` **只能**来自 broker 侧事实（topic 第二段）。信封 `device_id` **绝不**作为身份来源，**仅**用于一致性校验。

---

## §4 两层不得混同（对照表）

| 维度 | 第一层（设备 MQTT 信封） | 第二层（HTTP 接收信封） |
|---|---|---|
| 载体 | MQTT publish payload | HTTP POST body |
| 顶层键 | `device_id` / `boot_seq` / `seq` / `ts` / `level` / `payload_base64` | `topic` / `clientid` / `payload` |
| 「`payload`」一词的含义 | **不存在**该键 | **设备日志信封本身**（对象） |
| 「`payload_base64`」的位置 | **第一层顶层** | **第二层 `payload` 之内** |
| 谁生成 | 固件 `log_envelope.cpp` | EMQX Action 模板（`${payload}` 内联） |
| 可否改动 | 冻结（`protocol version` 不变） | 由 EMQX 动作体模板决定（**本任务不改**） |

> **⚠️ CBOR schema ≠ cloud envelope schema**（沿用 `cloud_protocol.md` §8.2.2）：内层 CBOR 批次（`log_cbor.h`）**不属于** cloud envelope schema，其消费者是 LogManager 语义层，**不是** ingest。

---

## §5 Worker 实际行为（**以代码为准**）

**记录提取**（`history-ingest.js` · `extractRecords()`）——按序判定：

1. `body.payload` 是**数组** ⇒ 数组 = **记录数组**，逐条取；
2. `payload` 是**对象** ⇒ **设备 Log envelope 单记录**（**§3.1 规范形态**）；
3. 否则若顶层 `payload_base64`（非空字符串）或 `payload` 为**非空字符串** ⇒ **顶层信封级单记录**（**§3.2 兼容形态**）；
4. 否则 ⇒ `null` ⇒ `400 {"error":"unparsable_payload"}`。

**落库值**（`normalizeLog()` → D1 `log_history`）：
`(device_id, ts, level, code, boot_seq, seq, message, payload)`
—— `device_id` 取自 topic；`level` 为**规范字符串**（`INFO`/`WARN`/`ERROR`/`CRITICAL`，**不是**数字文本）；`payload` = 原样 Base64。

**响应（HTTP 200）**：

```json
{"accepted": <新写入行数>, "duplicate": <幂等命中数>, "rejected": <逐条校验失败数>, "filtered": <DEBUG 策略过滤数>}
```

> **★ HTTP 200 ≠ 记录被接受**：记录级拒收**同样是 200**。判定必须**同时**看 `rejected`/`filtered` 与 D1 增量。

---

## §6 与「未实现」的边界（避免误读）

| 项 | 状态 |
|---|---|
| `log_ack`（批次级 ACK · 走 `down` topic） | ❌ **未实现**（属 `计划:WP-5`） |
| Worker 侧 CBOR 解码 / `payload` 内容解析 / 脱敏 | ❌ **不做**（脱敏责任在**设备侧**：禁止上传含 secret 的日志内容） |
| 按 `level` 做**业务**过滤 | ❌ 禁止 —— `level = 0` 的过滤属**词表不可表示**的**结构性过滤**，是唯一例外 |
| 本契约是否改变 `protocol version` / topic / ACL | ❌ 均不变 |

---

## §7 常见误判清单

- ❌ 「设备发的是扁平 JSON」→ 设备发的**是 6 字段对象**，被 EMQX 包进 `payload`。
- ❌ 「顶层没看到 `payload_base64` 就是坏包」→ 规范形态里它**在 `payload` 之内**。
- ❌ 「`level` 是字符串」→ 设备发的是 **JSON number**。
- ❌ 「`INFO` 不会上传」→ `INFO` **会上传**（`cloud = 1`）。
- ❌ 「HTTP 200 就是入库成功」→ 记录级拒收也是 200。

---

## §8 依据（可复核）

| 层 | 依据 |
|---|---|
| 第一层字段与 `level` | `src/log/log_envelope.cpp` · `src/log/log_events.h` · `docs/interfaces/cloud_protocol.md` §8.2.1 |
| 第一层真实帧 | `.pio/v2run/mqtt_v2.log:62`（`{"device_id":"288485896ce4","boot_seq":94,"seq":108289,"ts":0,"level":1,"payload_base64":"jAIC…"}`） |
| 第二层形态 | EMQX 仓 `config/data-integration/P3-WP4C-V2V4-Config-After.md` §2.3 · `P3-WP4C-V0-EMQX-Baseline.md` §3.2.1 |
| Worker 行为 | `Cloudflare_Assets/guo-feeder-api/src/history-ingest.js`（`extractRecords` / `normalizeLog` / `asLevelCode`） |
| Worker 测试 | `Cloudflare_Assets/guo-feeder-api/test/current/log_envelope_shape.test.mjs` · `log_level_contract.test.mjs` · `test/real_d1/history_ingest_real_d1.sh` §[17] |
| 验收与裁决 | 固件仓 `docs/review/F-C2-8-Log-Level-Contract-Assessment.md` §11.5（`F-C2-9` 登记）· `docs/architecture/P3-DT-WP4-History-Ingestion-Design.md` §2.4 / §5.3 |

---

*本文件于 **`2026-10-15`** 建立，为 `F-C2-9` 收尾的 **docs-only 契约澄清**；**不含任何实现变更**；固件 / Worker / EMQX / D1 schema 均未改动。*
