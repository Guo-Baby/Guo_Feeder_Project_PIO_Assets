# LogManager P1.2 阶段报告 —— Log Core / RAM Queue

- 日期：2026-09-15
- 需求：`log模块历史/log模块开发需求规划0915.md` §7 / §8 / §9 / §10 / §33 / §34
- 契约：`src/log_events.h`（P1.1 冻结）+ `log模块历史/LogManager-P1契约冻结0915.md`
- 范围：**只做 P1.2**。未进入 P1.3（Flash Segment Ring）；未实现 MQTT（P1.4）

---

## 1. 交付物

| 文件 | 类型 | 说明 |
|---|---|---|
| `src/log_manager.h` | 新增 | LogManager API / `LogStats` / 参数 helper（`log_arg_*`） |
| `src/log_manager.cpp` | 新增 | RAM 环、`log_emit()`、`log_task()`、Level→(Flash,Cloud) routing 决策 |
| `src/main.cpp` | 修改 | include；`setup()` 调 `log_init()`；`loop()` 调 `log_task()`；新增 `logt` 测试控制台 |
| `test/log_core_tests.txt` | 新增 | 66 条上板断言（覆盖 §10 全部要求） |
| `log模块历史/LogManager-P1.2阶段报告0915.md` | 新增 | 本报告 |

---

## 2. 关键实现

### 2.1 RAM 环（冻结参数，未改动）

```
LOG_RAM_QUEUE_SLOTS = 64 槽
LOG_RECORD_SIZE     = 128 B
合计                = 8 KB（PSRAM 优先，失败回退内部 DRAM）
```

实测命中 PSRAM：

```
[Log] RAM ring ready: 64 slots x 128 B = 8192 B in PSRAM (boot_seq=1)
```

### 2.2 并发模型（严格遵守冻结设计）

| 项 | 结论 |
|---|---|
| 独立 FreeRTOS Task | **不创建**（LittleFS 单写者模型） |
| `log_task()` 位置 | `loop()` 内，每轮最多消费 `LOG_DRAIN_MAX_PER_TASK = 8` 条 |
| 生产者 | 任意 Task 上下文（环索引 + 槽拷贝在临界区内，128 B ≈ 1 µs） |
| 消费者 | 仅 loopTask（临界区内 memcpy 出记录，避免被覆盖撕裂） |
| ISR 上下文 | **不支持**（需要时另加 `portENTER_CRITICAL_ISR` 版本，P1.2 不提供） |
| 环满策略 | **淘汰最旧**（`rd++`），累加侧信道计数 `ring_drop` |

### 2.3 Level 行为（表驱动，唯一策略）

`log_task()` 用 `log_level_to_flash()` / `log_level_to_cloud()` 决策，**无 EventId 级例外**：

| Level | 入队 | Flash | Cloud | 说明 |
|---|---|---|---|---|
| DEBUG | **否**（`log_emit` 早退） | NO | NO | 只累加 `debug_dropped`，不占 RAM 环 |
| INFO | 是 | NO | YES | |
| WARN | 是 | YES | YES | |
| ERROR | 是 | YES | YES | |
| CRITICAL | 是 | YES | YES | 额外置 `flush_requested`（P1.3/P1.4 消费） |

> P1.2 只做 **routing 决策与计数**（需求 §10 明确"这一阶段暂时可以只验证 Flash routing decision"）。
> 真正的 Flash 落盘在 P1.3，MQTT 上传在 P1.4 —— 代码中已标注落点注释。

### 2.4 结构化 API（无动态 String）

```cpp
bool log_emit(LogEventId event_id, LogLevel level,
              const LogParamIn *params, uint8_t param_count);
```

- `LogParamIn` 复用 P1.1 已冻结类型（`{id, type, union{i,u,f,b} v}`）。
- 参数编码进 Record：每项 6 B `{ id:u8, type:u8, value:u32le }`，最多 8 项。
- `param_count > 8` → **整体拒绝**（返回 false，不截断），符合契约。
- 提供 `log_arg_i32/u32/f32/bool/enum` helper，避免调用方误用 union 成员。
- `BOOL` 在编码时显式归一化（union 只写了 1 字节，其余字节不可信）。

### 2.5 Record 生成

| 字段 | P1.2 取值 |
|---|---|
| `version` | 2（冻结） |
| `flags` | 仅 `bit0 timestamp_valid`（`time_get() > 0` 时置位）；`bit1 context_present` 恒 0 |
| `timestamp` | `time_get()`；未同步写 0 |
| `packed` | 0（`context_kind`，P1.2 未实现 Context） |
| `seq` / `boot_seq` | **RAM 态占位**：`boot_seq=1`，`seq` 自增 |
| `crc32` | 覆盖 `[0..107]`，与项目既有实现一致 |

> ⚠️ `seq` / `boot_seq` 的 **持久化与区间预留属 P1.3**（需求 §13）。
> 本阶段只保证"本次开机内单调递增且不重复"，代码中已标注替换点。

### 2.6 `logt` 测试控制台（仅上板自测）

```
logt help | stats | ring | reset | policy <lv> | emit | fill | mix | overlimit
```

`stats` / `ring` / `reset` 会**先排空 RAM 环**再打印，保证断言稳定。

---

## 3. 验证结果

```text
P1.2 RESULT

Changed      : src/log_manager.h (新增)
               src/log_manager.cpp (新增)
               src/main.cpp (+include / log_init / log_task / logt console)
               test/log_core_tests.txt (新增 66 断言)
               log模块历史/LogManager-P1.2阶段报告0915.md (新增)

Tests        : log_core_tests.txt      66/66 PASS   (.pio/logcore_r1.log)
               log_contract_test        ALL PASS    (正向通过 + 负向按预期失败)

Build        : PASS  21.57s   RAM 39.6% (129632/327680)   Flash 64.0% (1342469/2097152)

Device Flash : PASS  esptool write_flash 0x10000, "Hash of data verified."

Device Test  : PASS  串口实机，0 崩溃（无 Guru / canary / abort）

Commit       : 本阶段独立 commit（见 git log HEAD）

PASS/FAIL    : 🟢 全部 PASS（Build + Contract Test + Device Test）
```

### 3.1 关键实测证据

**RAM 环落在 PSRAM：**

```
[Log] RAM ring ready: 64 slots x 128 B = 8192 B in PSRAM (boot_seq=1)
```

**Level Policy 表（5 行全部符合冻结设计）：**

```
[LogT] policy DEBUG flash=0 cloud=0
[LogT] policy INFO flash=0 cloud=1
[LogT] policy WARN flash=1 cloud=1
[LogT] policy ERROR flash=1 cloud=1
[LogT] policy CRITICAL flash=1 cloud=1
```

**DEBUG 不入队、不产生任何持久化负担：**

```
[LogT] emit level=DEBUG event=0x0401 params=0 queued=0
[LogT] stats emit=0 debug=1 ring=0/64 hw=0 drop=0 consumed=0 flash=0 cloud=0 ...
```

**INFO：Cloud YES / Flash NO（10 条）：**

```
[LogT] stats emit=10 debug=0 ring=0/64 hw=10 drop=0 consumed=10 flash=0 cloud=10 ...
```

**CRITICAL：Flash + Cloud + flush 请求：**

```
[LogT] stats emit=1 debug=0 ring=0/64 hw=1 drop=0 consumed=1 flash=1 cloud=1 crit=1 flush=1 ...
```

**混合 Level（DEBUG 5 条被丢弃，WARN+ 15 条需落 Flash）：**

```
[LogT] mix n=5 queued=20 levels=5
[LogT] stats emit=20 debug=5 ring=0/64 hw=20 drop=0 consumed=20 flash=15 cloud=20 crit=5 ...
```

**RAM 环满（单命令内 100 条 → 淘汰 36 条最旧）：**

```
[LogT] fill level=INFO n=100 event=0x0401 queued=100
[LogT] stats emit=100 ... drop=36 ... consumed=64 cloud=64 ...
```

**高频压力（单命令内 200 条，Ring 64 槽）：**

```
[LogT] stats emit=200 debug=0 ring=0/64 hw=64 drop=136 consumed=64 flash=64 cloud=64 ...
```

**契约：`params > 8` 整体拒绝（不截断）：**

```
[LogT] overlimit params=9 queued=0 (expect 0)
[LogT] stats emit=0 ...
```

**结构化参数（0 / 1 / 2 / 8 参数往返）：**

```
[LogT] emit level=WARN event=0x0403 params=0 queued=1
[LogT] emit level=WARN event=0x0403 params=1 queued=1
[LogT] emit level=WARN event=0x0403 params=2 queued=1
[LogT] emit level=WARN event=0x0403 params=8 queued=1
[LogT] stats emit=4 ...
```

---

## 4. 与冻结设计的偏差

**无。** 本阶段未修改任何协议 / ID / 数值 / 结构布局。

唯一需记录的是**范围边界**（非偏差）：

| 项 | 本阶段 | 何时完成 |
|---|---|---|
| Flash 真正落盘 | 只做 routing 决策与计数 | P1.3 |
| `seq` 区间预留 / `boot_seq` 持久化 | RAM 占位（`boot_seq=1`，`seq` 自增） | P1.3 |
| `context_present` / Context blob | 恒 0 / `blob_len=0` | P2（模块接入时按需） |
| Cloud 真正上传 | 只做 routing 决策与计数 | P1.4 |
| ACK / Retry / Offline | 未实现 | P1.5 |

---

## 5. 已知问题

| 级别 | 问题 | 状态 |
|---|---|---|
| 低 | `log_task()` 每轮消费 8 条，无时间节流（P1.3 引入 Flash 写后才需要 20 ms 节流） | 设计如此，P1.3 处理 |
| 低 | `log_flush_requested()` 只置一次（首次 CRITICAL），后续 CRITICAL 不重复计数 | 设计如此：请求是"边沿触发"，避免重复 flush |
| 低 | ISR 上下文不支持 | 已在头文件显式声明；当前无需 ISR 产生 Log |

**无崩溃、无内存泄漏（环为一次性分配，全生命周期驻留）。**

---

## 6. 下一步（P1.3，待命令）

按需求 §11–§14：

1. `/log/s%07u.log` 段文件（16 B header + 31 × 128 B = 3984 B）
2. 整段创建 / 整段追加 / 整段删除（禁止修改 Record 内 uploaded bit）
3. `meta.bin`（32 B，fmt v2）：`boot_seq` +1 落盘、`seq_reserved += 256`
4. 掉电测试（写入 → 突然重启 → 验证旧 Record 不损坏、seq 不重复、boot_seq 增加）
