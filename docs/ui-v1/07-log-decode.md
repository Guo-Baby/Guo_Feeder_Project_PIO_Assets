# 日志解码设计（已用真实样本验证）

> 这里所有结构都来自**固件源码 + 真实 payload 逐字节核对**，不是文档推断。
> 本轮**只读**：没有改任何代码，只是把格式钉死并给出实现规格。

---

## 1. 从 MQTT 到 D1 的形态转换（必须分清三层）

```
设备 → MQTT /log           JSON envelope（字段顺序即契约）
                            {"device_id","boot_seq","seq","ts","level","payload_base64"}
        payload_base64  =  base64( CBOR 12 项定序数组，其中 records = [1 × bstr(128 B)] )
        ↓ EMQX 规则 → Worker /api/history/ingest
D1 log_history             列：id, device_id, ts, level, code, boot_seq, seq, message, payload, created_at
        payload        =  上面那个 payload_base64 原文（未再加工）
        message        =  **永远 NULL**（envelope 里没有这个字段）
        code           =  **永远 NULL**（同上）
        level          =  来自 envelope 的 level（**有值**，可直接做 SQL 过滤）
        ts / created_at =  **秒**
```

依据：`src/log/log_envelope.h` 头注释（冻结项 `H-WP4-FW-02/03/04`）；
**实测**3970 行 `message`/`code` 100% NULL、`payload` 0 行为空、`level` 有值。

---

## 2. CBOR 批次结构（12 项定序数组，下标即键）

来源：`src/log/log_cbor.h:14-16`

| 下标 | 字段 | 实测值（样本 id=7202） |
|---|---|---|
| 0 | `fmt` | `2` |
| 1 | `event_dict_ver` | `2` ← **不是字典版本，见 §7.2** |
| 2 | `boot_seq` | `69` |
| 3 | `seq_from` | `19386` |
| 4 | `seq_to` | `19386` |
| 5 | `count` | `1`（冻结） |
| 6 | `drop_ring` | `0` |
| 7 | `drop_overflow` | `0` |
| 8 | `drop_unacked` | `0` |
| 9 | `self_degraded` | `0` |
| 10 | `flags` | `1` |
| 11 | `records[]` | 1 项，每项 `bstr` **128 B** |

**实测长度**：`cbor_len = 148`（不是常量 188 —— 188 是**最坏情况**上界，
因为整数按值大小用 1/2/4/8 字节编码）；`b64_len = 200`。
样本实测分布：`196 / 200 / 204` 字符。

---

## 3. LogRecord v2（128 B 定长，小端）

来源：`src/log/log_events.h:411-453`（含偏移常量 `LOG_OFF_*`）

| 偏移 | 长度 | 字段 | 样本实值 |
|---|---|---|---|
| 0 | 1 | `version` = 2 | `2` |
| 1 | 1 | `level`（**0 基**：0=DEBUG 1=INFO 2=WARN 3=ERROR 4=CRITICAL） | `2` = WARN |
| 2 | 1 | `flags`（bit0 = `TIMESTAMP_VALID`，bit1 = `CONTEXT_PRESENT`） | `0x01` |
| 3 | 1 | `param_count` | `2` |
| 4 | 4 | `seq` u32le | `19386` |
| 8 | 4 | `boot_seq` u32le | `69` |
| 12 | 2 | `event_id` u16le | `0x0511` |
| 14 | 2 | `packed`（bit0-2 `context_kind`） | `0x0000` |
| 16 | 4 | `uptime_ms` u32le | `1587883` |
| 20 | 4 | `timestamp` u32le（**0 = 无效**） | `1792702961` |
| 24 | 2 | `blob_len` u16le | `0` |
| 26 | 2 | `reserved16` | — |
| 28 | 48 | `params[8]`，每项 6 B：`{id:u8, type:u8, value:u32le}` | 见 §4 |
| 76 | 32 | `blob`（字符串参数的字符区） | 空 |
| 108 | 4 | `crc32`（覆盖 `[0..107]`） | `0x0DE82A28` |
| 112 | 16 | `reserved` | — |

**样本参数原始字节**（id=7202，`CAUSE`+`COUNT`）：
```
16 02 01 00 00 00   → id=0x16(type=2=U32, value=1)
45 02 04 00 00 00   → id=0x45(type=2=U32, value=4)   ← 0x45 = 69 = COUNT? 见下
```
（`0x16`=`LOG_P_CAUSE`=1，`0x45`=`LOG_P_COUNT`=4 —— 与 `0x0511 DISPENSE_SAFETY_RESPONSE` 语义吻合）

**另一条**（id=7201，`VALVE_FORCE_CLOSE`）：`16 02 01 …` + `48 02 00 00 00 00`
（`0x48`=`LOG_P_VALVE_OPEN_MS`=0）

---

## 4. 参数类型（6 种，**字符串特殊**）

`log_events.h:86-96`

| 值 | 类型 | 读法 |
|---|---|---|
| 1 | `I32` | `int32le` |
| 2 | `U32` | `uint32le` |
| 3 | `F32` | `float32le` |
| 4 | `BOOL` | `uint32le`（非 0 为真） |
| 5 | `ENUM` | `uint32le` |
| **6** | **`STR`** | **value = `(blob_off << 16) \| blob_len`**，字符在 `blob` 区 |
| 7 | `I8` | `int8`（低 8 位） |
| 8 | `U16` | `uint16`（低 16 位） |

★ **`STR` 是最容易做错的一种**：它不直接给值，而是一个"偏移+长度"打包整数，
字符要从 offset 76 的 32 B blob 里切。
★ 当前实采样本里 `blob_len = 0`（没有字符串参数），
所以 **`STR` 分支必须用人工构造样本单测**，不能等真机出现。

---

## 5. 真实样本（可直接作为黄金用例）

```json
// 样本 A：WARN / DISPENSE_SAFETY_RESPONSE / 2 参数
{
  "id": 7202, "b64_len": 200, "cbor_len": 148,
  "batch": {"fmt":2,"event_dict_ver":2,"boot":69,"seq_from":19386,"seq_to":19386,
            "count":1,"flags":1},
  "record": {"version":2,"level":2,"flags":1,"param_count":2,
             "seq":19386,"boot_seq":69,"event_id":"0x0511","packed":0,
             "uptime_ms":1587883,"timestamp":1792702961,"blob_len":0,
             "crc32":"0x0DE82A28",
             "params":[{"id":"0x16","type":2,"value":1},{"id":"0x45","type":2,"value":4}]}
}

// 样本 B：CRITICAL / VALVE_FORCE_CLOSE
{"id":7201,"level":4,"event_id":"0x0505","timestamp":1792702956,
 "params":[{"id":"0x16","type":2,"value":1},{"id":"0x48","type":2,"value":0}]}

// 样本 C：ts=0（开机早期，RET-1）
//   flags bit0 = 0 ⇒ 必须回退用 created_at
```

---

## 6. 解码实现规格

### 6.1 接口

```
GET /api/history/log?decode=1
→ items[] 每项增加 "decoded"：
   {
     "ok": true,
     "event": {"id":"0x0509","name":"LOG_VALVE_OPEN","label":"电磁阀开","module":"valve"},
     "level": "WARN",
     "params": [{"id":"0x21","name":"LOG_P_STATE","label":"状态","value":1}],
     "time_source": "record" | "created_at",
     "warnings": []
   }
或（失败降级）
   { "ok": false, "reason": "unknown_event" | "cbor_error" | "short_record" | "crc_mismatch",
     "raw_event_id": "0x09FF", "raw_payload": "<base64>" }
```

- **`payload` 原文始终返回**（便于对拍/调试），`decoded` 是**附加字段**
- 默认 `decode` 缺省 = 0 ⇒ **老行为不变**（向后兼容，避免影响现有看板）

### 6.2 降级规则（必须逐条实现）

| 情形 | 处理 | **不允许** |
|---|---|---|
| CBOR 解析失败 | `ok:false, reason:"cbor_error"` + 保留原文 | 抛 500 / 整页报错 |
| `records` 为空或长度 < 128 | `reason:"short_record"` | 静默丢掉整行 |
| `event_id` 不在码表 | `reason:"unknown_event"` + 显示 `0x%04X` | 显示"未知"就完事（要保留 id 便于排查） |
| `param id` 不在码表 | 该参数显示 `p0x45` + 原值 | 丢弃整个参数列表 |
| `param type` 未知 | 显示 `type=?` + 4 B 十六进制 | 猜类型 |
| `blob_len > 32` 或越界 | `reason:"blob_overflow"`，字符串置 `null` | 越界读取 |
| `crc32` 不匹配 | **解码照常**，但 `warnings:["crc_mismatch"]` 并在 UI 标黄 | 拒绝解码（日志仍有价值） |
| `version != 2` | `reason:"unsupported_version"` + 保留原文 | 按 v2 强行解 |

### 6.3 时间处理（两个独立的坑）

| 坑 | 规则 |
|---|---|
| 记录内 `ts = 0`（开机早期，`RET-1`） | 用 `flags bit0` 判定：`0` ⇒ 无效 ⇒ **回退 `created_at`**（秒），`time_source:"created_at"` |
| 表单位不同 | `log_history.created_at` 是**秒**、`mqtt_messages.created_at` 是**毫秒**；解码器内部**统一成毫秒**再输出 |

样本 A 的 `flags=0x01` + `ts=1792702961` ⇒ `time_source:"record"`（正确路径）。

### 6.4 `module` 过滤的数据来源（禁止引用不存在的字段）

`log_history` **没有 `module` 列**（曾按它过滤导致 SQL 报错 → 返回空 → 误判"没日志"）。

**正确做法**：
```
module 来自"码表"：EVENTS[event_id].module  (由描述符/事件定义归属推导)
⇒ UI 的"按模块过滤"是**解码后在前端过滤**，或后端解码后 WHERE 派生字段
⇒ 绝不 SELECT ... WHERE module = ?
```
同理"事件类别（阀门/秤/流程）"也是**码表派生**，不是列。

---

## 7. 码表生成与漂移检测（含**诚实的能力边界**）

### 7.1 生成器（可重复）

```
Cloudflare_Assets/scripts/gen-log-codes.mjs
  输入：PIO_Assets/Guo_Feeder_Project/src/log/log_events.h
  抽：  LOG_XXX = 0xNNNN      → EVENTS[id] = {name, module, level}
        LOG_P_YYY = 0xNN      → PARAMS[id] = {name, label}
        enum LogLevel (0 基)  → LEVELS[]
        枚举名 → 中文标签表（单独维护在 labels.zh.json，不进生成文件）
  输出：guo-feeder-api/src/log-codes.generated.js
        export const SOURCE_SHA = "<log_events.h 的 sha256>";
        export const EVENTS = {...}; export const PARAMS = {...}; export const LEVELS = [...];
```

**重复性要求**：同一份 `log_events.h` 必须产出**逐字节相同**的输出（按 id 排序，不用时间戳）。

### 7.2 ★ 漂移检测：现有协议**没有**字典版本号（重要纠正）

原本想"比较 hash 检测固件漂移"，核查后发现**做不到真正的运行期漂移检测**，原因：

1. **`event_dict_ver` 是假的字典版本**：
   `log_envelope.cpp:90` 写的是 `h.event_dict_ver = LOG_RECORD_VERSION;`（= 2）
   ⇒ 它是**记录格式版本**被复用，**完全不随码表变化**。
   ⇒ **绝不能用它判断"事件字典是否变了"。**

2. **`query_capabilities` 的 `checksum` 只覆盖 id 集合**（`stable_id ↔ runtime_id`），
   不覆盖 EventId / ParamId 的含义；算法本身也 **[待验证 V-1]**。

3. 因此：**若只比较两个都由同一份 Worker 文件生成的 hash，等于什么都没检测。**

### 7.3 能做的三层（按可信度排序）

| 层 | 机制 | 检测什么 | 局限 |
|---|---|---|---|
| **L1 编译期同步检查（可信）** | 生成的 `SOURCE_SHA` = `log_events.h` 的 sha256；测试里重算并比对 | 「改了头文件但忘了重跑生成器」 | 不能检测"设备跑的是旧固件" |
| **L2 运行期经验指标（弱）** | 统计最近 N 条日志里 `unknown_event` / `unknown_param` 的比例；> 1% 就在 UI 顶部提示"码表可能过期" | 「设备端的码表比云端新」 | 有**假阴性**：新增事件若刚好没出现，就检测不到 |
| **L3 人工核对（兜底）** | 设备 `query_capabilities` 的 `action.version` 变化时，提示"固件能力集变了，请核对码表" | 「Action 集合变了」⇒ 大概率也改了事件 | 只覆盖 Action 集合，不覆盖纯 EventId 变更 |

**结论**：漂移检测是**三层拼起来的近似**，不是权威判定。
UI 必须把"码表可能过期"作为**提示**而非**断言**，并附上"如何重新生成"的说明。

### 7.4 重新生成的触发条件（写进开发清单）

改以下任一文件 ⇒ **必须**重跑生成器并提交生成物：
```
src/log/log_events.h          （EventId / ParamId / LogLevel）
```

---

## 8. 自动化测试（必须真的能挡住"LogLevel 1 基"这个错）

| # | 用例 | 断言 |
|---|---|---|
| T-1 | 码表生成幂等 | 连续生成两次，输出 sha256 相同 |
| T-2 | **LEVELS 是 0 基** | `LEVELS[0]==="DEBUG" && LEVELS[2]==="WARN" && LEVELS[4]==="CRITICAL"` |
| T-3 | 真实样本 A 解码 | `event==="LOG_DISPENSE_SAFETY_RESPONSE"`、`level==="WARN"`、2 个参数且值 1/4 |
| T-4 | 真实样本 B 解码 | `event==="LOG_VALVE_FORCE_CLOSE"`、`level==="CRITICAL"`、`VALVE_OPEN_MS===0` |
| T-5 | `ts=0` + `flags=0` | `time_source==="created_at"`，且输出毫秒 |
| T-6 | 未知 event id | `ok:false, reason:"unknown_event"`，且 `payload` 原文仍在 |
| T-7 | 截断记录（60 B） | `reason:"short_record"`，不抛异常 |
| T-8 | 损坏 CBOR | `reason:"cbor_error"`，不抛异常 |
| T-9 | **构造 STR 参数** | 正确从 blob 切出字符串（因为真机样本里没有 STR） |
| T-10 | `crc32` 不匹配 | 仍解码成功 + `warnings` 含 `crc_mismatch` |
| T-11 | 全表抽检（离线） | 用 D1 导出的 100 条真实 payload 跑，`ok` 率 ≥ 99% |

T-2/T-3/T-4 **就是**防"LogLevel 偏移"的回归点（本轮我已实际踩过一次：
1 基映射把 `WARN` 显示成 `INFO`，导致 RTC 探测失败被误读成正常）。

---

## 9. 与原计划的差异

| 原计划 | 修正 |
|---|---|
| "比较 hash 检测固件漂移" | **改为三层近似**；并指出 `event_dict_ver` 是记录版本不是字典版本（§7.2） |
| "按 module 过滤日志" | **没有该列**；module 必须由码表派生（§6.4） |
| "解码失败跳过该行" | **改为保留原文 + 降级标记**（§6.2） |
| 未提 `STR` 参数 | 必须单独实现并用构造样本测试（§4） |
| 未提 `crc32` | 校验但不拒绝，仅告警（§6.2） |
| 未提 `flags bit0` | 用它判定 `ts` 是否可信（§6.3） |
