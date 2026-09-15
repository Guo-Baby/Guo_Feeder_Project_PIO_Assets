# LogManager P1.1 阶段报告

> 依据：`log模块历史/log模块开发需求规划0915.md`（§5 P1.1 / §6 P1.1 测试 / §34 测试记录 / §36 当前任务）
> 日期：2026-09-15
> 基线：git `38377a5`

---

## P1.1 RESULT

```text
Changed      : src/log_events.h（新增，冻结契约头）
               src/main.cpp（+15 行：1 处 include + 1 段启动 banner）
               test/log_contract/（新增：契约测试探针 + 运行器）

Tests        : Contract Test（编译期 static_assert + constexpr 自检）
               正向探针 probe_ok.cpp  : PASS（编译通过）
               负向探针 probe_bad.cpp : PASS（按预期编译失败，证明断言生效）
               合计 46 条 static_assert

Build        : pio run → SUCCESS（103.16 s）
               RAM   [====      ] 39.5% (129536 / 327680 B)
               Flash [======    ] 63.8% (1337545 / 2097152 B)

Device Flash : ⛔ BLOCKED —— 开发板当前未连接（枚举到的串口只有
               3 个蓝牙虚拟口 COM5/6/7，无任何 USB-Serial 设备）

Device Test  : ⛔ BLOCKED —— 同上

Commit       : （见文末）

PASS/FAIL    : 🟡 BUILD PASS + CONTRACT TEST PASS
               设备项 PENDING（非代码问题，属外部条件缺失）
```

---

## 1. 本阶段交付物

| 文件 | 说明 |
|---|---|
| `src/log_events.h` | **冻结契约**（纯声明 + 编译期常量，**无任何实现**） |
| `src/main.cpp` | +1 include（使 static_assert 参与编译）+ 1 段启动 banner（供实机验证） |
| `test/log_contract/probe_ok.cpp` | 正向探针：显式把契约值用作常量表达式 |
| `test/log_contract/probe_bad.cpp` | 负向探针：故意失败的断言 |
| `test/log_contract/run_contract_test.py` | 离线 Contract Test 运行器 |

## 2. `log_events.h` 内容清单（对应需求 §5）

| 需求 §5 条目 | 实现位置 | 状态 |
|---|---|---|
| Level（DEBUG…CRITICAL） | `enum LogLevel`，值 0..4 | ✅ |
| EventId 完整表 | `enum LogEventId`，**100 个**（0x0000 + 0x0101–0x0E01 + 0x0F00 预留） | ✅ 未增/删/重编号 |
| ParamId 与类型定义 | `enum LogParamId`，**0x00–0x58（89 项）**；类型 `enum LogParamType` | ✅ |
| LogRecord v2 / 128 B | `struct LogRecord`（packed）+ `static_assert(sizeof==128)` | ✅ |
| 编译期检查 | 46 条 `static_assert`（尺寸/偏移/取值/策略） | ✅ |
| Segment 常量 | `SEGMENT_HEADER_SIZE=16` `RECORD_SIZE=128` `RECORDS_PER_SEGMENT=31` `SEGMENT_SIZE=3984` `SEGMENT_COUNT=16`（+ `CAPACITY=496`） | ✅ |
| Cloud 常量 | `LOG_BATCH_FMT=2` + **11 个 CBOR 批次头键** + **log_ack 字段名** | ✅ |
| Level Policy 代码化 | `LOG_LEVEL_POLICY[]` + `log_level_to_flash()` / `log_level_to_cloud()` | ✅ |
| 禁止 persist / flash_override / force_flash | `LOG_FLAG_DEFINED_MASK == 0x03` 断言；**不存在** `LOG_FLAG_PERSIST` / `LOG_FLAG_UPLOADED` | ✅ |

## 3. Record v2 冻结布局（已实现并逐项断言）

```
偏移 长度 字段                                    断言
0    1    version      = 2                        ✅
1    1    level                                   ✅
2    1    flags        (bit0 timestamp_valid,      ✅
                        bit1 context_present,
                        bit2-7 reserved)
3    1    param_count                             ✅
4    4    seq          uint32                     ✅ sizeof==4
8    4    boot_seq     uint32                     ✅ sizeof==4
12   2    event_id                                ✅
14   2    packed       (bit0-2 context_kind)      ✅
16   4    uptime_ms                               ✅
20   4    timestamp    (0 = 无效)                  ✅
24   2    blob_len                                 ✅
26   2    reserved16                               ✅
28   48   params[8] {id:u8,type:u8,value:u32le}   ✅
76   32   blob                                     ✅
108  4    crc32        (覆盖 [0..107])             ✅
112  16   reserved                                 ✅
────────────────────────────────────────────────
合计 128 B                                        ✅ sizeof(LogRecord)==128
```

---

## 4. 编译期发现并解决的两处真实冲突

> 依据需求 §1.1「如果发现冻结设计与现有代码存在冲突：停止扩大修改范围，
> 先报告冲突位置、原因和最小修改方案」。

### 冲突 1（阻断编译）：`LOG_LEVEL_*` 与 NimBLE-Arduino 宏重名

| 项 | 内容 |
|---|---|
| 位置 | `log_events.h` 的 Level 枚举 vs `.pio/libdeps/.../NimBLE-Arduino/src/nimble/porting/nimble/include/log_common/log_common.h:38-45` |
| 现象 | `error: expected unqualified-id before numeric constant` —— 宏把标识符展开成了 `(0)` |
| 原因 | NimBLE **无条件** `#define LOG_LEVEL_DEBUG (0)` … `LOG_LEVEL_CRITICAL (4)`（另有 `NONE/MAX/STR`），且无 `#ifndef` 保护。`main.cpp` 先包含 `NimBLEDevice.h`，故宏已生效 |
| 最小修改 | 枚举改名 `LOG_LEVEL_*` → **`LOG_LVL_*`**，并在头文件注明冲突来源 |
| **是否改动冻结设计** | **否**。冻结的是 Level 的**取值与策略**（0..4 + Flash/Cloud 矩阵），并未规定 C++ 标识符名。取值、顺序、策略一字未动 |

### 冲突 2（阻断编译）：工具链为 gnu++11，constexpr 函数体受限

| 项 | 内容 |
|---|---|
| 现象 | `error: body of 'constexpr' function 'log_contract_self_check()' not a return-statement` |
| 原因 | C++11 要求 `constexpr` 函数体**只能是一条 return**，不得声明局部变量。原写法用了 `bool policy_ok = ...` 等局部变量 |
| 最小修改 | 改写为**单条 return 表达式**（用 `&&` 串联五组检查），语义完全等价 |
| **是否改动冻结设计** | **否**。纯实现写法调整 |

### 附带发现（未改，仅报告）

| # | 内容 | 影响 | 建议 |
|---|---|---|---|
| A | `LOG_P_BOOT_SEQ(0x21)` 在设计报告附录 A 的字典表标注类型为 **U16**，但 P1 契约 §3.3 与 Record 字段均为 **uint32** | 若实现时按 U16 编码会截断 | **ID 保持 0x21 不变**；实现时按 `LOG_PTYPE_U32` 使用。已在头文件中注明，建议下次契约修订时同步字典表 |
| B | `LOG_WF_SAVE_PARTIAL_RETRY_OK(0x040C)` 在冻结表中标注"可选" | 需确认是否真正落地 | 已按冻结表原样保留 ID；是否实现待 P2 决定 |

---

## 5. Contract Test 设计与结果

### 为什么要"负向探针"

只做"编译通过"的测试是**不可证伪的**：万一断言因某种原因被跳过，"通过"同样成立。
因此本测试包含两个探针：

| 探针 | 期望 | 实际 | 判定 |
|---|---|---|---|
| `probe_ok.cpp` | 编译**通过** | 通过 | ✅ PASS |
| `probe_bad.cpp` | 编译**失败**且命中 `NEGATIVE PROBE` 断言 | 失败并命中 | ✅ PASS |

```text
$ python test/log_contract/run_contract_test.py
toolchain: D:\platformIO\packages\toolchain-xtensa-esp32s3\bin\xtensa-esp32s3-elf-g++.exe
src      : D:\Guo_Feeder_Project\PIO_Assets\Guo_Feeder_Project\src
--------------------------------------------------------
[1/2 positive] PASS : 编译通过（期望通过）
[2/2 negative] PASS : 编译失败且命中负向探针断言（期望失败）
--------------------------------------------------------
CONTRACT TEST: ALL PASS
```

### 覆盖矩阵（需求 §6 逐条）

| 需求 §6 要求验证 | 断言 | 结果 |
|---|---|---|
| enum 值正确 | Level 0..4；EventId/ParamId 抽样 24 项 | ✅ |
| Record size = 128 | `sizeof(LogRecord)==128` + 逐字段偏移 | ✅ |
| Segment size = 3984 | `LOG_SEGMENT_SIZE==3984` 且 `==16+31*128` | ✅ |
| 31 records | `LOG_RECORDS_PER_SEGMENT==31` | ✅ |
| 16 segments | `LOG_SEGMENT_COUNT==16` | ✅ |
| Level Policy 正确 | 5 组 `log_level_to_flash/cloud` 组合断言 | ✅ |
| seq / boot_seq 为 uint32 | `sizeof(...seq)==4` / `sizeof(...boot_seq)==4` | ✅ |
| flags 只有冻结字段 | `LOG_FLAG_DEFINED_MASK==0x03` | ✅ |
| 不存在 uploaded | 无该宏/位；mask 断言排除 | ✅ |
| 不存在 persist | 无该宏/位；mask 断言排除 | ✅ |
| 附加：段严格单 block | `LOG_SEGMENT_SIZE < 4096` | ✅ |
| 附加：容量自洽 | `LOG_SEGMENT_CAPACITY==31*16==496` | ✅ |
| 附加：meta/ack 常量 | `META_SIZE==32` `META_VERSION==2` `SEQ_RESERVE==256` `BATCH_FMT==2` `ACK_TIMEOUT==15000` `ACK_MAX_RETRY==5` | ✅ |

---

## 6. Build 结果（§34 记录）

```text
Build        : PASS
Command      : PLATFORMIO_BUILD_DIR=.pio/build/p11c<ts> pio run
Took         : 103.16 s
RAM          : 39.5%  (129536 / 327680 B)
Flash        : 63.8%  (1337545 / 2097152 B)
Firmware     : .pio/build/p11c<ts>/esp32-s3-devkitc-1/firmware.bin
```

> 注：本次为**全量构建**（项目沙箱下增量编译因 `safe-delete` 被拦截而必然失败，属环境限制，非代码问题）。
> 静态检查未引入任何新依赖；`log_events.h` 仅依赖 `<stdint.h>` / `<stddef.h>`，是 freestanding 的。

---

## 7. Device / Serial / Flash / MQTT 测试（§34 记录）

```text
Device       : ⛔ BLOCKED —— 开发板未连接
Firmware     : 已生成（未烧录）
Serial Test  : ⛔ 未执行
Flash Test   : N/A（P1.1 不涉及 Flash Ring 实现）
MQTT Test    : N/A（P1.1 不涉及 MQTT）
```

**阻塞原因（真实、非推测）**

```text
$ python -c "from serial.tools import list_ports; print([p.device for p in list_ports.comports()])"
可用 USB 串口: 无
（仅 COM5 / COM7 / COM6 —— "蓝牙链接上的标准串行"，无 USB-Serial 设备）

$ esptool.py --chip esp32s3 --port COM8 write_flash -z 0x10000 firmware.bin
A fatal error occurred: Could not open COM8, the port is busy or doesn't exist.
```

> 本会话早期曾成功枚举到 COM8（ESP32-S3），构建期间设备被拔出。

**设备就绪后需执行的验证（命令已备好）**

```bash
# 1) 烧录（只写 app 分区，保留 LittleFS）
BD=$(cat .pio/last_bd.txt)
python D:/platformIO/packages/tool-esptoolpy/esptool.py \
  --chip esp32s3 --port COM8 --baud 921600 write_flash -z 0x10000 \
  "$BD/esp32-s3-devkitc-1/firmware.bin"

# 2) 串口确认（期望在 PSRAM 行之后出现）
#    [LogContract] v=2 rec=128 seg=3984 x16 cap=496 fmt=2 check=PASS
python test/serial_batch.py COM8 .pio/p11_device.log <用例文件> 5
```

**判定标准**：`check=PASS` 且设备启动流程无异常
（`LittleFS mounted` / `init done, loaded 8/8 modules` / `Workflow loaded from Flash BIN` 均照旧）。

---

## 8. Git

```text
Commit       : <见下方 git log>
Scope        : src/log_events.h · src/main.cpp · test/log_contract/ · 本报告
未纳入       : AI_RULES.md（用户自己的修改，未触碰）
               .workbuddy/（.gitignore 已忽略）
```

遵循需求 §2.1：**未使用** `git add .` / `reset --hard` / `clean` / `checkout -- .` / `restore .` / `stash`。

---

## 9. 结论与下一步

| 项 | 结论 |
|---|---|
| P1.1 代码交付 | ✅ 完成（冻结契约 100% 落地，未增删重编号） |
| Build | ✅ PASS |
| Contract Test | ✅ ALL PASS（含可证伪的负向探针） |
| Device 验证 | ⛔ 待设备接入后补做（**非代码问题**） |
| 是否进入 P1.2 | ❌ **未进入** —— 按 §36「P1.1 完成后不要自动继续 P1.2」 |

**待用户处理**

1. 接入 ESP32-S3 开发板（USB-Serial），我即可补做烧录 + 串口实机验证并给出最终判定。
2. 确认 §4 附带发现 A（`LOG_P_BOOT_SEQ` 类型）与 B（`0x040C` 是否落地）。
