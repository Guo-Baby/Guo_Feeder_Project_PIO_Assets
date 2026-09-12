# ConfigManager version 初始化 + Workflow 初始 BIN — 进度台账

> 中断后**先读本文件**再动手。每完成一个阶段即 commit，commit 后回来更新本表。

## 任务背景

上机串口打印：

```
[CFG][E] version reload failed
[CFG][I] init done, loaded 8/8 modules
```

需求：
1. 修复该问题；
2. 若 `/config/version.json` 不存在，在 PC 创建初始化 version 文件；
3. 创建 workflow 初始 BIN 文件（内容来自 `workflow.json`），避免烧录时
   板载文件被清除后没有基线；
4. 保证未来再次烧录数据时不破坏 version 标记；
5. 每步 commit，保留进度记录；
6. 最终给出报告：问题如何产生、如何修复。

## 根因分析（已确认，静态分析 + 目录核对）

### 直接原因

`data/config/`（uploadfs 源目录）**只有 8 个模块 JSON，没有 `version.json`**：

```
data/config/  mi_thermo.json  mqtt.json  oled.json  rtc.json
              time.json  valve.json  weight.json  wifi.json
```

而 `config_init()` 第 3 步直接调用 `load_version_file()`：

```cpp
// src/config_manager.cpp:1027
if (!load_version_file())
{
    cfg_log("E", "version reload failed");
}
```

`load_version_file()`（`src/config_manager.cpp:563`）在
`json_storage_read(CONFIG_VERSION_FILE, content)` 失败时直接 `return false`。
文件不存在 → 读取失败 → 打 E 级日志，且所有模块 `version` 保持 **0**（"未知"）。

### 为什么它不会自愈（关键）

`save_version_file()`（`src/config_manager.cpp:591`）里有一段保护：

```cpp
if (s_modules[i].version == 0)
{
    continue;   // 不能用未知的 0 覆盖文件里已记录的版本
}
```

`version == 0` 的模块会被**跳过**。于是：

```
version.json 缺失 → 所有 version = 0
        ↓
config_save → save_version_file() 全部跳过 → 文件仍不生成
        ↓
下次启动仍然 version reload failed
```

**只有某个模块被真正修改（version++ ≥ 1）后，version.json 才会被创建。**
在此之前每次启动都会打这条 E 级日志。

### 影响

- 日志噪声（E 级，容易被误判为故障）；
- `config_get_version()` 全部返回 0 → `expect_version` 乐观锁在
  version.json 生成前**无法正常工作**（云端拿到的版本号恒为 0）；
- 首次 OTA / 出厂烧录后，UI 显示版本全 0。

### 结论：这是"缺少初始文件 + 缺少自愈路径"的组合问题

## 修复方案

### A. 代码自愈（根本修复）—— `src/config_manager.cpp`

`config_init()` 中 `load_version_file()` 失败时：
1. 判定为"首次启动 / version 文件缺失或损坏"；
2. 把**已成功加载的模块** version 初始化为 1；
3. 立即 `save_version_file()` 落盘；
4. 日志级别从 `E` 降为 `W`（首次启动是预期场景，不是错误），
   仅在**写入也失败**时才打 `E`。

同时给 `load_version_file()` 增加返回值语义区分：
- 文件不存在 → 需要 bootstrap
- 文件存在但损坏 → 需要重建
两者都走同一条自愈路径。

### B. 数据基线 —— `data/config/version.json`

创建初始 version 文件（8 个模块，version = 1），随 uploadfs 一起烧录。
这样即使不依赖代码自愈，上传 fs 后版本号立即就位。

### C. Workflow 初始 BIN —— `data/workflow/`

**先回答"烧录会不会清除板载文件"**：

| 烧录方式 | 是否清除 LittleFS |
|---|---|
| `esptool write_flash 0x10000 firmware.bin`（本项目日常用法） | **否**，只写 app 分区 |
| `pio run -t uploadfs` | **是**，整分区替换为 `data/` 镜像 |

因此：日常刷固件不影响；**跑 `uploadfs` 会清空 `/workflow/*.bin` 和 `/config/*`**。

当前 `/workflow.json` 已在 `data/` 中，BIN 缺失时设备会：
```
workflow_load_from_storage() 失败 → workflow_load_json_file("/workflow.json")
→ workflow_migrate_to_storage()
```
即**已有自愈路径**。但按需求仍生成初始 BIN，使 uploadfs 后直接可用，
不依赖 JSON 回退。

BIN 格式（Meta v3 / Step v1）见下方"格式备忘"。

## 阶段总表

| 阶段 | 内容 | 状态 | Commit |
|---|---|---|---|
| 1 | `config_init()` version 自愈 | ✅ 完成 | `17b8823` |
| 2 | `data/config/version.json` 初始文件 + 生成脚本 | ✅ 完成 | `23889ac`（脚本） |
| 3 | Workflow 初始 BIN 生成器 + `data/workflow/` | ✅ 完成 | `23889ac`（脚本） |
| 4 | 编译 + 烧录 + 上板验证 | ✅ 完成 | 见下方实测 |
| 5 | 报告 | ✅ 完成 | `workflow与config初始文件问题-修复报告.md` |

## 上板实测结果（COM8，2026-09-13）

### 修复前（旧固件，每次启动都报）

```
[04:45:33.995] [  1529][E][vfs_api.cpp:105] open(): /littlefs/config/version.json does not exist, no permits for creation
[04:45:33.995] [CFG][E] version reload failed
[04:45:33.995] [CFG][I] init done, loaded 8/8 modules
```

### 修复后 —— 场景 A：镜像【不含】 version.json（走自愈路径）

用 `--after no_reset` 烧 FS，再开串口捕获**首次启动**（否则脚本开串口会
复位，自愈发生在未被捕获的那一次）：

```
[05:13:45.047] [CFG][W] version file missing or corrupt, will rebuild
[05:13:45.047] [CFG][W] recovering (boot_ok=0, commit_state=0)
[05:13:45.482] [CFG][I] init done, loaded 8/8 modules
[05:13:45.483] [CFG][I] version file created (baseline)      ← 自愈成功
[05:13:47.555] [OK] Workflow loaded from Flash BIN            ← BIN 基线生效
[05:13:47.887] [CFG][I] boot validated
```
**`[CFG][E]` 计数 = 0。**

### 修复后 —— 场景 B：镜像【含】 data/config/version.json（走数据基线路径）

```
[05:18:29.781] [CFG][I] init done, loaded 8/8 modules
[05:18:32.009] [OK] Workflow loaded from Flash BIN
[05:18:32.103] [CFG][I] boot validated
```
无 W、无 E。`config_query` 返回
`"versions":{"wifi":1,...,"rtc":1}`（8 个模块全为 1）。

### Workflow BIN 内容验证（不只是 meta）

`workflow.get p.id=1`：
```json
{"id":"daily_valve_test1","name":"每日开阀测试1","variant":1,"enable":true,
 "timeout_ms":60000,"steps":[
   {"type":"trigger","id":"timer_daily","params":{"hour":7,"minute":0}},
   {"type":"action","id":"VALVE_OPEN"},
   {"type":"trigger","id":"delay","params":{"seconds":30}},
   {"type":"action","id":"VALVE_CLOSE"}]}
```
步骤类型、参数、中文名（UTF-8）全部正确往返。断言 4/4，0 崩溃。

### uploadfs 会清除板载文件（已证实）

```
Flash will be erased from 0x00410000 to 0x00ffffff...
Wrote 12517376 bytes at 0x00410000
```
整分区擦除。日常刷固件（`esptool write_flash 0x10000`）**不会**触碰 LittleFS。

### 镜像内容核对（决定性证据）

对 `littlefs.bin` 做字节扫描：
```
version.json     NOT in image   ← 场景 A 时确实没烧进去
workflow.json    IN IMAGE
meta.bin         IN IMAGE
step00.bin       IN IMAGE
```

## BIN 格式备忘（生成脚本依据，源码 `src/workflow_storage.cpp`）

```
/workflow/meta.bin
  header 12B: magic(4)=0x57464D54 "WFMT" | version(2)=3 | count(2)=16 | crc32(4)
  crc32 = CRC32(entries 区域)   // 标准 CRC32: init 0xFFFFFFFF, poly 0xEDB88320, final xor 0xFFFFFFFF
  entries: 16 × 89B
    valid(1) version(1) step_count(2) update_time(4) crc32(4)
    id(32, zero-padded) name(32, zero-padded)
    enable(1) timeout_ms(4) txn_id(4) variant(4)

/workflow/wfNN/stepMM.bin
  header 16B: magic(4)=0x57465350 "WFSP" | version(2)=1 | header_size(2)=16
              payload_size(4) | crc32(4)
  crc32 = CRC32(payload)
  payload:
    wf_index(1) step_index(1) type(1) instance_type(1)
    id(32) param_count(1)
    params[8] × 58B: name(16) type(1) int32(4) float32(4) bool(1) str(32)

meta.entry.crc32 = CRC32( 每个 step 的 payload CRC 依次以 u32 LE 追加 )
```

`type`: 0=TRIGGER? 需确认枚举值（WORKFLOW_STEP_TRIGGER / ACTION）
`instance_type`: INSTANCE_TRIGGER / INSTANCE_ACTION
`param type`: PARAM_INT / PARAM_FLOAT / PARAM_BOOL / PARAM_STRING

## 工作区保护

以下文件是**用户已有修改**，全程不得触碰 / 提交：

- `workflow修改需求文档.md`（M）
- `workflow架构适配commandmanager及version-修改报告.md`（M）
- `workflow代码分析0910.md`（??）
- `workflow架构设计补充0910.md`（??）
- `workflow架构适配commandmanager及version需求.md`（??）

禁止 `git add .`；只 `git add <明确文件>`。

## 环境备忘

- 板子 COM8，烧录 `esptool --chip esp32s3 --port COM8 --baud 921600 write_flash -z 0x10000`
- uploadfs：`pio run -t uploadfs`（会整分区替换 LittleFS）
- 编译：`PLATFORMIO_BUILD_DIR=<新目录> pio run`（沙箱 safe-delete 拦截增量编译）
- 当前遇到的坑：**COM8 被其它进程占用**（PermissionError 13），
  可能是用户开着串口监视器。验证前需先释放。
