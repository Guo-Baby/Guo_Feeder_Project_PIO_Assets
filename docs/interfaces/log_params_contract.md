# 日志参数（Param）契约

> 状态：`2026-10-19` 新建（`HD-3`·`A1-03`·批次 3）
> 上游：`src/log/log_events.h`（参数 ID / 类型 / 记录布局，带 `static_assert` 锁定）
> 相关：`docs/interfaces/log_envelope_contract.md`（记录与上报信封）
> **本文只登记"参数表"这一层，不改变任何既有字段、Topic 或 ACK 语义。**

---

## 1. 为什么需要本文

`log_envelope_contract.md` 定义了**记录级**布局（128 B、CRC、`boot_seq`、`seq`…），
但**从未定义参数表**（哪个 `pid` 是什么含义）。结果是：

- 新增参数时**没有地方可查**"这个 `0x5A` 有没有被占用"；
- 消费方拿到 `payload_base64` 也不知道该解析什么。

`A1-03` 需要在启动事件里加 4 个诊断参数，因此**必须先把这个空洞补上**。

---

## 2. 线格式（wire format）

参数位于记录体 `[28, 76)` 共 48 B，即 `params[8]`，每项 **6 B**：

```
offset  size  field
0       1     pid    参数 ID（`LogParamId`）
1       1     type   类型（`LogParamType`）
2       4     value  u32 小端（语义由 type 决定）
```

`param_count`（记录头 offset 3）指明**实际有效项数**；`params[param_count..7]` 为填充，**不得解析**。

**这是自描述格式**：每一项自带 `pid` 与 `type` ⇒ 消费方可以**按 pid 取值**，
不需要"必须知道全部 pid 才能解析"。

---

## 3. 类型编码（`LogParamType`）

| 值 | 名称 | 编码方式 |
|---|---|---|
| 1 | `LOG_PTYPE_I32` | `value` 按 `int32_t` 解释 |
| 2 | `LOG_PTYPE_U32` | `value` 按 `uint32_t` 解释 |
| 3 | `LOG_PTYPE_F32` | `value` 按 `float` 位模式解释 |
| 4 | `LOG_PTYPE_BOOL` | `value` 仅取 `0` / `1`（其它值视为非法） |
| 5 | `LOG_PTYPE_ENUM` | 无符号整数，取值域由**该 pid 的定义**约束 |
| 6 | `LOG_PTYPE_STR` | `value = (blob_off << 16) | blob_len`，字符存于记录体 `blob[32]` |
| 7 | `LOG_PTYPE_I8` | `value` 低 8 位按 `int8_t` 解释 |
| 8 | `LOG_PTYPE_U16` | `value` 低 16 位按 `uint16_t` 解释 |

> ⚠️ **没有独立的 `U8` 类型**。需要表达 0…255 的小整数时，用 `ENUM`（语义枚举）
> 或 `BOOL`（二值）或 `U32`（通用整数）——这也是 `A1-03` 的选型依据。

---

## 4. 参数表（登记制）

> 规则：**新增参数必须先在本表登记**，且 `pid` 不得复用。
> 完整清单以 `src/log/log_events.h` 的 `LogParamId` 为准（`static_assert` 锁定关键值）。

### 4.1 本次登记（`HD-3`·`A1-03`）

| pid | 名称 | 类型 | 单位 | 取值域 | 语义 |
|---|---|---|---|---|---|
| `0x21` | `LOG_P_BOOT_SEQ` | `U32` | — | ≥1 | 本次开机序号（与记录头 `boot_seq` **同值**，属冗余自描述参数） |
| `0x22` | `LOG_P_INIT_MS` | `U32` | ms | 0…2³²−1 | 从 `setup()` 起算到本次事件发出的耗时 |
| **`0x5A`** | **`LOG_P_RESERVED_HW`** | **`U32`** | 序号 | 0…2³²−1 | 序号**已预留**高水位（`LogFlashInfo.seq_reserved`）；掉电恢复时用于判断"是否重复分配" |
| **`0x5B`** | **`LOG_P_RECOVERY_SRC`** | **`ENUM`** | — | `0…5` | 本次开机采用的**恢复来源**：`0=NVS` `1=SLOT` `2=LEGACY` `3=SEGMENT` `4=PROVISION` `5=QUARANTINE`（对应 `log_meta_evidence::Recovery`） |
| **`0x5C`** | **`LOG_P_FAILDCLOSED`** | **`BOOL`** | — | `0` / `1` | `1` = 处于隔离态（拒绝分配新 `seq`）。**`1` 时本事件本身可能发不出去**，见 §6 |
| **`0x5D`** | **`LOG_P_TRACE_CORRUPT`** | **`U32`** | 次 | 0…2³²−1 | `/log/corrupt.trace` 累计损坏段数（跨重启固化，只增不减） |

`LOG_P_MAX` 已由 `0x59` 抬至 **`0x5D`**（`0x00…0x59` 原有 90 个 pid 已用满）。
`LOG_MAX_PARAMS` 仍为 **8**，本次启动事件用 **5** 个参数（`INIT_MS` + 上述 4 个），未超限。

### 4.2 使用位置

| 事件 | EventId | 参数 |
|---|---|---|
| `LOG_SYS_BOOT_COMPLETE` | `0x0101`（**未变**） | `0x22` `0x5A` `0x5B` `0x5C` `0x5D`（共 5 个） |

- **每次启动最多发 1 条**该事件（`main.cpp` 的 `setup()` 尾部单点）；
- **不**进入突发合并白名单，**不**新增周期日志/遥测；
- `boot_seq` **不在参数里重复发送**——它是记录头字段（`LOG_OFF_BOOT_SEQ = 8`），每条记录自带。

---

## 5. 未知 `pid` 的消费规则（**契约要求**）

> **消费方必须跳过不认识的 `pid`，不得因未知 `pid` 拒绝整条记录或整批数据。**

理由与**证据边界**（必须原样理解，不得简化成"已由代码实测证明"）：

| 项 | 事实 | 证据 |
|---|---|---|
| 线格式自描述 | 每项自带 `pid` + `type` + 定长 `value` ⇒ 跳过无需知道语义 | §2（`LOG_PARAM_SIZE = 6`，`static_assert` 锁 `offsetof`） |
| 本仓库**没有** TLV 解码器 | 契约探针只置 `param_count = 0`，从不解析 `params[]` | `test/log_contract/probe_envelope.cpp:164` |
| 云端**不解析**参数 | 云端把整条记录作为 `payload_base64` **不透明**存储/转发 | `Cloudflare_Assets/guo-feeder-api/src/history-ingest.js:23` |

因此本次能得到的结论**只有**：

1. 当前仓库内的云端路径**不解析** `payload` ⇒ 新增 `pid` 对它**零影响**；
2. 本契约**要求**任何解码方跳过未知 `pid`。

**不能**据此声称"所有既有消费者都已兼容"——那需要逐个消费者实测。
若未来出现按 `pid` 解析的消费方（App / 分析脚本 / 第三方），**必须先**核对其跳过逻辑。

---

## 6. 已知边界（不得当作缺陷，也不得当作无问题）

| # | 边界 | 说明 |
|---|---|---|
| B-1 | `failclosed = 1` 时诊断发不出 | `log_emit()` 在隔离态被短路 ⇒ **看不到诊断 ≠ 没问题**。此时唯一通道是串口（`[LogT] evidence`） |
| B-2 | `RECOVERY_SRC = 5`（`QUARANTINE`）一般观测不到 | 同上：隔离态下这条启动事件通常已被短路，只有"隔离前一 boot 成功写入"的记录才带该值 |
| B-3 | `TRACE_CORRUPT` 的读取有一次性 I/O | `log_flash_get_info()` 会**只读**一次 `/log/corrupt.trace`（32 B）。文件不存在直接返回 0，**不创建、不重试** |
| B-4 | 参数只覆盖"启动瞬间" | 运行期累积（积压量、上传进度）**不在**本事件内；那需要遥测（协议授权） |
| B-5 | 旧固件没有这些参数 | 消费方必须容忍"参数缺失"（按 `param_count` 判断），不得要求字段必存在 |

---

## 7. 变更流程（强制）

1. 在本表**登记** `pid`（类型 / 单位 / 取值域 / 语义），并确认**全仓唯一**；
2. 若 `pid` 超过 `LOG_P_MAX`，同步抬 `LOG_P_MAX` **并改两处** `static_assert`
   （`src/log/log_events.h`、`test/log_contract/probe_ok.cpp`）；
3. 参数数不得超过 `LOG_MAX_PARAMS = 8`；
4. 在 `test/log_contract/probe_boot_diag.cpp`（或对应探针）加断言；
5. 若消费方语义受影响，同步更新 `docs/interfaces/log_envelope_contract.md`；
6. **不要**在未登记的情况下临时使用某个 `pid`。
