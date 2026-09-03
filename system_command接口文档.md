# SystemCommand 接口文档（V1）

> **源码**：`src/system_command.h` / `src/system_command.cpp`
> **接入点**：`src/command_manager.cpp`（仅路由与 handler，未改其它模块）
> **状态**：V1 已定版，已在真实硬件（ESP32-S3 N16R8）+ EMQX MQTT 上端到端验证
> **最后验证**：2026-08-31，git tag `feat/system_command_v1`
> **本文档一切内容取自源码通读与实测回包，无推测**

---

## 1. 模块定位

SystemCommand 是**设备自身基础系统控制与资源查询**的执行层。

它只回答两类问题：

1. **控制类**：让设备安全重启。
2. **资源类**：设备现在还有多少 RAM、Flash，文件系统里存了什么。

它**不是**通用命令入口，**不是**状态中心，**不是**配置模块。它只做"系统资源与系统控制"这一件事，
且尽量薄——查询、组织结果、返回，不持有长期状态、不缓存、不做后台任务。

设计上模块本身**无状态**：`system_command_init()` 目前是空实现（LittleFS 由 `main.cpp` 在 setup 中挂载，
本模块不重复挂载），保留该函数仅为未来扩展留口。

---

## 2. 业务边界

### 2.1 本模块负责

| 能力 | 说明 |
|---|---|
| RAM 查询 | Internal RAM（片上 heap）与 External PSRAM **分别**统计，单位统一 bytes |
| Flash 查询 | Internal Flash（固件区）与 External Flash（LittleFS 数据区）**分别**统计 |
| 文件枚举 | 递归遍历 LittleFS，返回**扁平** `path/type/size` 列表，纯只读 |
| 安全重启 | 暴露 `restart` 指令（实际重启由 ConfigManager 既有安全窗口执行） |

### 2.2 本模块明确不负责（已由其它模块实现，禁止重复实现）

| 不做的事 | 归属模块 |
|---|---|
| System Info、通用运行状态查询、系统状态字段 | **SystemState** |
| 配置读/写/保存/版本/Active-Backup/Commit-Boot Recovery/Factory Reset | **ConfigManager** |
| WiFi 配置与状态查询 | **WiFi 模块** |
| MQTT 配置与状态查询 | **CloudManager / MQTT** |
| 当前时间、NTP 状态、时间配置 | **TimeManager** |
| 喂食、电机、阀门、称重、Workflow / Action / Trigger | **业务层 + WorkflowManager** |
| 日志查询 | Log 模块（未来独立实现，V1 不做） |
| OTA / 固件升级 | Firmware/OTA 模块（未来独立设计，V1 不做） |
| 命令路由、cmd_id 生成、source 解析、超时管理 | **CommandManager** |
| MQTT 收发、协议翻译、上行压缩 | **CloudManager** |
| 文件读写、原子写、分块流式、CRC | **JsonStorage** |

### 2.3 一条重要的边界例外（必读）

`system.restart` 的 **handler 位于 `command_manager.cpp`，不在本模块内**。

原因：项目中 ConfigManager 已实现完整的安全重启机制（`config_cmd_enqueue_restart()` +
`CONFIG_RESTART_SAFE_DELAY_MS`）。需求明确禁止建第二套重启机制，因此 restart 只是
"CommandManager 直接转发给 ConfigManager 队列"，本模块**不参与、不持有任何重启状态**，
`system_command.h` 也**不为 restart 声明任何函数**。

> 结论：`system_command.cpp` 里只有 memory 与 flash 两个查询实现。restart 属于本模块的
> **指令集**，但不属于本模块的**代码**。

---

## 3. 架构与调用链

```
        云端 (EMQX MQTT)
              │  guo_feeder/down   (JSON, 紧凑格式 c/i/p)
              ▼
        CloudManager                 收包 → deserializeJson → 协议翻译
              │  CommandMessage {command, object, cmd_id, source, payload}
              ▼
        CommandManager               路由 (c=="system" → system_router → 按 object 分发)
              │
     ┌────────┴─────────┬──────────────────────┐
     ▼                  ▼                      ▼
 command_system_    command_system_       command_system_
   memory()            flash()               restart()
     │                  │                      │
     ▼                  ▼                      ▼
 syscmd_memory()   syscmd_flash()      config_cmd_enqueue_restart()
  [SystemCommand]   [SystemCommand]        [ConfigManager 队列]
     │                  │                      │
     │  写 response["data"]                    │ 10s 安全窗口后 ESP.restart()
     └────────┬─────────┘                      │
              ▼                                ▼
        CommandManager 统一包装 cmd=result → CloudManager 压缩上行
              │  guo_feeder/up   (紧凑 JSON, 键名缩写 s/c/i/o/t)
              ▼
            云端 / UI
```

**同步 vs 异步**

- `memory` / `flash`：**同步**。handler 直接填 `response` 并 `return true`，框架统一包装
  `cmd=result` 一次上报。上行为 **ACK + result 两段**。
- `restart`：**异步**（复用 ConfigManager 命令队列）。handler 入队后立即返回 `accepted`，
  真正重启在 10 秒安全窗口后发生。上行为 **ACK + accepted + result 三段**，
  保证"completion callback 在真正重启之前完成"。

---

## 4. 代码接入点（本次改动清单）

| 文件 | 改动 |
|---|---|
| `src/system_command.h` | **新建**。模块说明 + 3 个声明 |
| `src/system_command.cpp` | **新建**。`fill_ram_block()`、`files_collect()`（递归、只读）、两个查询实现 |
| `src/command_manager.cpp:12` | `#include "system_command.h"` |
| `src/command_manager.cpp:171-173` | 3 个 handler 前向声明 |
| `src/command_manager.cpp:1282-1291` | `system_router` 增 `memory` / `flash` / `restart` 分支 |
| `src/command_manager.cpp:389` | `command_manager_init()` 中调用 `system_command_init()` |
| `src/command_manager.cpp:2034-2140` | 3 个 handler 实现 |

**未改动**：ConfigManager、SystemState、WorkflowManager、Action/Trigger、CloudManager 协议、
WiFi、MQTT、TimeManager、JsonStorage、`main.cpp`、`partitions.csv`。

---

## 5. 本地 C 接口

```cpp
#include "system_command.h"
```

### `void system_command_init()`

初始化。当前**无状态、空实现**（LittleFS 由 `main.cpp` 挂载，此处不重复挂载）。
由 `command_manager_init()` 调用一次。

### `bool syscmd_memory(JsonDocument &out)`

写入 `out["data"] = { internal:{...}, external:{...} }`，每块含 5 个字段。

- 返回 `true`：成功。
- 返回 `false`：`out["data"]` 创建失败（内存不足）。理论上不会发生，保留错误通道以满足
  "禁止查询失败却 success=true"。
- **无 PSRAM 的硬件**：`external` 5 个字段全填 0，但**仍返回 `true`**（属硬件差异，不是错误）。

底层 API：`ESP.getHeapSize/getFreeHeap/getMaxAllocHeap/getMinFreeHeap`、
`ESP.getPsramSize/getFreePsram/getMaxAllocPsram/getMinFreePsram`。

### `bool syscmd_flash(JsonDocument &out)`

写入 `out["data"] = { internal:{...}, external:{...}, files:[...] }`。

- 返回 `true`：成功。
- 返回 `false`：以下任一情况
  - `LittleFS.totalBytes() == 0` → 文件系统未挂载 / 不可用（**明确失败，绝不返回 success**）
  - `data` 或 `files` 数组创建失败（内存不足）

**只读保证**：`files_collect()` 仅 `open` 取元数据（`name/isDirectory/size`），不读文件内容，
不创建/删除/重命名任何文件，不触碰 ConfigManager 的 Active / Backup / Factory 文件，
不产生临时文件。

---

## 6. 云端指令

统一走 CloudManager 紧凑下行格式：`c` = 命令域，`i` = 命令 ID，`p` = payload（`o` 为 object）。
下行发 **JSON**（设备侧 `deserializeJson` 解析），上行为**紧凑 JSON 文本**（键名缩写）。

上行 `s` 字段含义：`0`=OK、`1`=ACCEPTED、`3`=FAILED。

### 6.1 `system.memory` — RAM 查询

一条命令返回全部 RAM 信息（按用户要求合并，不拆多条）。

**下行**
```json
{"c":"system","i":"3003","p":{"o":"memory"}}
```

**上行（实测原始回包）**
```json
{"c":"ack","i":"3003","o":"memory","p":{"result":"received"},"t":1788191553}
```
```json
{
  "s": 0,
  "c": "result",
  "i": "3003",
  "t": 1788191553,
  "data": {
    "internal": {
      "total": 256316,
      "free": 75868,
      "used": 180448,
      "largest_free_block": 65524,
      "minimum_free": 64260
    },
    "external": {
      "total": 8385815,
      "free": 8131175,
      "used": 254640,
      "largest_free_block": 8126452,
      "minimum_free": 8124191
    }
  },
  "command": "system"
}
```

**字段语义（单位统一 bytes，UI 负责格式化，模块内不返回 KB/MB 字符串）**

| 字段 | 含义 |
|---|---|
| `internal` | 片上 Internal RAM（heap） |
| `external` | External PSRAM（8MB，无 PSRAM 时全 0） |
| `total` | 总容量 |
| `free` | 当前空闲 |
| `used` | `total - free`（下溢保护，负值归 0） |
| `largest_free_block` | 当前最大可分配**连续**块 → 与 `free` 的差距即碎片程度 |
| `minimum_free` | 启动以来的历史最小空闲 → 判断是否曾接近 OOM |

> **诊断提示**：`internal.free=75868` 但 `largest_free_block=65524`，说明片上 heap 已有碎片；
> 大块分配应优先走 PSRAM（`external.largest_free_block` 有 8.1MB）。

### 6.2 `system.flash` — Flash 容量 + 文件列表

按用户要求，原需求中的 `system.files` **已合并进本命令**，一次返回容量与文件列表，
减少 UI 往返。

**下行**
```json
{"c":"system","i":"3004","p":{"o":"flash"}}
```

**上行（实测，files 已截取部分）**
```json
{
  "s": 0,
  "c": "result",
  "i": "3004",
  "t": 1788191561,
  "data": {
    "external": { "total": 12517376, "used": 73728,   "free": 12443648 },
    "internal": { "total": 4259840,  "used": 1278832, "free": 2981008  },
    "files": [
      { "path": "/config.json",            "type": "file",      "size": 1576 },
      { "path": "/config",                 "type": "directory", "size": 0    },
      { "path": "/config/.bootok",         "type": "file",      "size": 1    },
      { "path": "/config/.commit",         "type": "file",      "size": 4    },
      { "path": "/config/mqtt.json",       "type": "file",      "size": 407  },
      { "path": "/config/weight.json.bak", "type": "file",      "size": 91   },
      { "path": "/factory",                "type": "directory", "size": 0    },
      { "path": "/factory/weight.json",    "type": "file",      "size": 91   },
      { "path": "/registry/action.bin",    "type": "file",      "size": 106  },
      { "path": "/workflow.json",          "type": "file",      "size": 2911 }
    ]
  },
  "command": "system"
}
```

**字段语义**

| 字段 | 含义 |
|---|---|
| `external.total/used/free` | **LittleFS 数据分区**（`partitions.csv` 中 `0x410000` 起、`0xBF0000` ≈ 12.4MB），取自 `LittleFS.totalBytes()/usedBytes()` |
| `internal.total` | `ESP.getFlashChipSize() - external.total`（16MB − 12.4MB ≈ 4.06MB），即固件区 |
| `internal.used` | `ESP.getSketchSize()`，当前运行 app 的大小 |
| `internal.free` | `internal.total - internal.used` |
| `files[].path` | LittleFS **绝对路径**，已规范化补前导 `/`（部分核心版本 `openNextFile()` 返回不带 `/`） |
| `files[].type` | `"file"` 或 `"directory"` |
| `files[].size` | 文件为实际字节数；**目录恒为 0** |

> **⚠️ `internal` 是语义映射，不是物理分区**
> ESP32-S3 只有一片外置 SPI Flash（16MB）。这里按需求文档的语义把它切成
> "固件区（internal）"与"数据区（external）"两部分。`internal.total` 是
> **整片容量减去 LittleFS 分区**的近似值，实际还含 `nvs` / `otadata` / `app0` / `app1`；
> `internal.used` 用当前 app 大小作**代理值**，不等于 OTA 双分区的真实占用。
> 精确布局请查 `partitions.csv`。UI 展示时建议标注为"固件区（近似）"。

**列表特性（UI 必读）**

- **扁平结构**，不含 `children` 嵌套，不生成 `├──`/`└──` 文本树。UI 依据 `path` 自行构建 Tree View。
- **顺序不保证**：由 `openNextFile()` 决定，目录项可能出现在其子项之前或之后，也可能夹在文件之间
  （实测 `/config.json` 出现在 `/config` 目录项之前）。**UI 必须按 `path` 自行排序/建树，不可依赖返回顺序。**
- **递归完整**：包含所有层级的目录与文件，实测返回 23 项，覆盖 `/config`、`/factory`、`/registry`
  三个子目录及全部文件（含 `.bootok`、`.commit` 隐藏标记文件与 `.bak` 备份文件）。
- 会暴露 ConfigManager 的内部文件（`.commit` / `.bootok` / `*.bak`）与历史遗留文件
  （如 `/config.json`、`/workflow.json`）。这是**有意为之**的诊断能力，UI 可自行过滤。

### 6.3 `system.restart` — 安全重启

**下行**
```json
{"c":"system","i":"2003","p":{"o":"restart"}}
```

取消一个待执行的重启：
```json
{"c":"system","i":"2004","p":{"o":"restart","cancel":true}}
```

**上行三段**

1. ACK：`{"c":"ack","i":"2003","o":"restart","p":{"result":"received"}}`
2. accepted（`s:1`）：`message` = `"restart queued (safe delay 10s)"`（取消时为 `"restart cancel queued"`），
   附 `command_id`
3. result（`s:0`）：来自 ConfigManager 完成回调，`"restart scheduled after callback"`
   （取消时 `"restart cancelled"`）；随后串口打印 `restart scheduled in 10000 ms`

**时序**
```
收到命令 → 入 ConfigManager 队列 → 立即回 accepted
        → 完成回调回 result（此时尚未重启，保证上报送达）
        → 10s 安全窗口（CONFIG_RESTART_SAFE_DELAY_MS）
        → 串口 "restart safe delay elapsed, rebooting"
        → ESP.restart()  → rst:0xc (RTC_SW_CPU_RST) 软复位 → 重新启动并 MQTT 上线
```

**命名约定**：本指令统一叫 **`restart`**，禁止叫 `reset`。项目中 `config_reset` 是
**Factory Reset（恢复出厂配置）**，两者语义完全不同，必须严格区分。

> **⚠️ 烧录/重启前的既有坑**：设备非干净重启会触发 Boot Validation 的 Backup 恢复。
> 若刚通过 `config_set` 改过配置但尚未 `config_save`，重启会回滚到旧 backup。
> 测试改参后请先 `config_save` 再 restart。

---

## 7. 与其他模块的交互

### 7.1 依赖（本模块 → 其它）

| 被依赖方 | 用途 | 方向 |
|---|---|---|
| `LittleFS`（Arduino FS） | `totalBytes/usedBytes/open/openNextFile` | 只读 |
| `ESP`（Arduino 核心） | heap / PSRAM / flash chip / sketch size | 只读 |
| `ArduinoJson` | 把结果写入调用方提供的 `JsonDocument` | 写调用方文档 |

> 注：本模块直接 `#include <LittleFS.h>`。这是**目录遍历**能力，JsonStorage 当前只提供
> 单文件级操作（`exists/size/read/write/write_atomic/rename/mkdir/chunk/stream`），
> **不提供目录枚举**，故遍历无法经由 JsonStorage 实现。
> 若未来 JsonStorage 增加 `list_dir()` 之类接口，应把 `files_collect()` 迁过去，
> 使本模块彻底不依赖具体文件系统。

### 7.2 被依赖（其它 → 本模块）

只有 **CommandManager** 调用本模块，且仅调 `system_command_init()` / `syscmd_memory()` /
`syscmd_flash()` 三个函数。除此之外**没有任何模块引用本模块**。

### 7.3 委托（不自己实现）

| 事项 | 委托给 |
|---|---|
| 实际重启执行、10s 安全窗口、重启取消 | ConfigManager（`config_cmd_enqueue_restart` / `CONFIG_RESTART_SAFE_DELAY_MS`） |
| cmd_id 生成、source 解析、超时、CommandRuntime 生命周期 | CommandManager |
| 结果包装（`cmd=result` / `id` / `command` / `timestamp`）与错误上报 | CommandManager（`command_report_result` / `command_send_error`） |
| MQTT 收发、协议翻译、上行键名压缩 | CloudManager |

### 7.4 明确不接触

不读写任何 System State 字段，不注册 Trigger / Action，不订阅任何事件，不启动任务，
不修改任何文件，不持有长期缓存，无静态可变状态（除函数内局部变量）。

---

## 8. 错误处理

原则：**任何底层失败必须返回明确失败，禁止"查询失败但 success=true"，禁止静默吞错。**

| 场景 | handler 行为 | 上行 |
|---|---|---|
| `syscmd_memory()` 返回 false | `command_send_error(CMD_ERROR_SYSTEM, "memory query failed")` → `return false` | `s:3`（FAILED）+ `e:9` + `m` |
| `syscmd_flash()` 返回 false | `command_send_error(CMD_ERROR_SYSTEM, "flash query failed (storage/filesystem unavailable)")` → `return false` | `s:3` + `e:9` + `m` |
| restart `CommandRuntime` 分配失败 | `CMD_ERROR_SYSTEM, "restart runtime alloc failed"` | `s:3` + `e:9` |
| restart 入队失败（队列满等） | 先释放 runtime，再 `CMD_ERROR_SYSTEM, "restart enqueue failed"` | `s:3` + `e:9` |
| object 不认识 | `CMD_ERROR_UNKNOWN_COMMAND, "Unknown system object: xxx"` | `s:3` + `e:1` |

**错误码**（`command_manager.cpp:17-26`，本模块用到的）

| 宏 | 值 | 触发 |
|---|---|---|
| `CMD_ERROR_UNKNOWN_COMMAND` | 1 | object 未注册 |
| `CMD_ERROR_SYSTEM` | 9 | 查询失败 / 运行时分配失败 / 入队失败 |

错误包结构（`command_send_error`，压缩前）：
`{cmd:"result", id, type:"command", status:"error", error_code, message, timestamp}`

**handler 返回值约定**：`false` 表示"我已自行上报（错误包已发出）"，
`command_manager_execute` 的调用方（CloudManager）只打一行日志，**不会重复发错误包**，
因此不存在双重上报。

---

## 9. 内存 / 性能 / 容量边界

### 9.1 内存

- 结果**直接构造在调用方提供的 `JsonDocument`** 上，不做中间拷贝、不建临时树、不引入长期缓存。
- ArduinoJson 7 的 `JsonDocument` 为**堆分配 + 自动增长**，无需（也不能）指定容量。
- 无 `static` 可变状态、无常驻缓冲区。
- 递归 `files_collect()` 每层一个 `File` 句柄，进出成对 `close()`；本项目 FS 深度仅 2 层，
  栈开销可控。

### 9.2 执行时间

三条命令均为同步执行。`flash` 的遍历耗时随文件数增长；当前 23 项在单次 loop 内即时完成，
未观察到看门狗或任务饥饿。**若未来文件数量大幅增长（数百项），需评估是否改为分页或异步。**

### 9.3 上行 payload 上限（已知边界）

CloudManager 的 MQTT 缓冲为 `CLOUD_MQTT_BUFFER_SIZE = 8192`，单条消息上限
`CLOUD_MSG_LIMIT_MAX = 8128` 字节。

- 每个文件条目序列化后约 55–70 字节。
- 实测 23 项的完整 `flash` 回包约 1.6KB，余量充足。
- 粗略安全上界约 **100–120 个条目**。

**V1 未实现超限裁剪/分页**。若文件数逼近该量级，`system.flash` 回包可能被截断。
建议的后续方案（未实现）：`{"o":"flash","files":false}` 只返回容量；或加 `path` 前缀过滤 /
`offset`+`limit` 分页。

---

## 10. restart 保护架构规划（V2 设计意图，V1 未实现）

V1 行为就是"收到命令 → 10 秒倒计时 → 无条件重启"，**不检查系统是否正在执行危险操作**。

已识别的风险窗口：

- 正在向 Flash 写文件（ConfigManager 原子写的 tmp→rename 中间态、JsonStorage 分块写入未收尾）
- 未来向 RTC 芯片写时间
- 未来 OTA 写固件分区

**规划方向**（已写入 `command_manager.cpp` 代码注释）：保护逻辑应落在
**ConfigManager 的重启执行路径**，而非本模块：

1. 引入全局 `system_busy` 标志（或按位标记的 busy 域：`BUSY_FLASH_WRITE` / `BUSY_RTC_WRITE` / `BUSY_OTA`）。
2. 关键区进入时置位、退出时清位（配对，必须异常安全）。
3. `config_restart_now()` 在安全窗口到期时检查该标志：忙则**顺延**窗口（例如再等 1s 重试），
   并设最大顺延次数上限，避免永不重启。
4. 顺延与最终执行都要上报/打日志，避免"命令消失"的观感。

**为什么放在 ConfigManager 而不是 SystemCommand**：重启的唯一执行者是 ConfigManager，
把检查放在执行点才能覆盖**所有**重启来源（云端命令、未来的定时重启、异常自恢复重启）。
SystemCommand 只是重启的**请求方之一**，不应持有系统忙状态。

> 未来"定时重启"功能应复用同一条 `config_cmd_enqueue_restart()` 路径，
> 从而自动获得同一套保护，**不要再造第三套重启机制**。

---

## 11. 验证记录

**编译**：`PLATFORMIO_BUILD_DIR=.pio/build/wb_syscmd_b2 pio run` → SUCCESS（101s）
零 error、**零新增 warning**（唯一 warning 是既有 `cloud_manager.cpp:1041`
`DynamicJsonDocument` deprecated，与本次无关）。

**烧录**：`pio run -t upload --upload-port COM8` 成功，校验通过。

**真实硬件 MQTT 端到端**（EMQX TLS，`guo_feeder/down` 下发、`guo_feeder/up` 监听）：

| 命令 | 结果 |
|---|---|
| `system.memory` | ✅ `s:0`，internal/external 各 5 字段齐全，PSRAM total=8385815 |
| `system.flash` | ✅ `s:0`，容量正确，递归列出 23 项（3 个子目录 + 全部文件） |
| `system.restart` | ✅ accepted → result → 10s 后 `rst:0xc (RTC_SW_CPU_RST)` 软复位 → 重新上线并发 `online` |

**测试工具**（可复用）：
- `tools/mqtt_system_test.py`：固定跑 memory / flash / restart 三条（restart 置末尾）
- `tools/mqtt_send.py`：通用下发，命令由命令行传入，例：
  ```bash
  python tools/mqtt_send.py '{"c":"system","i":"3001","p":{"o":"memory"}}'
  ```

---

## 12. 开发 AI 须知（踩坑清单）

1. **restart 不在本模块实现**。要改重启行为，改 ConfigManager；要改重启命令的参数解析，
   改 `command_manager.cpp` 的 `command_system_restart`。别在 `system_command.cpp` 里找。
2. **ArduinoJson 7 陷阱**：
   - `JsonDocument(8192)` 这种容量构造**不存在**，会编译失败（v7 是堆分配自动增长）。
   - `createNestedObject()` / `createNestedArray()` **已废弃**，会引入新 warning。
     必须用 `obj["k"].to<JsonObject>()` / `arr.add<JsonObject>()`。
   - 定长 `char[N]` 数组**绝不能**直传 ArduinoJson 做 key（会按 N-1 取长度而非 `strlen`），
     必须先转 `const char*`。
3. **`files` 顺序不可依赖**，目录项可能出现在其子项之前。UI 必须自行排序建树。
4. **`internal` Flash 是近似值**，不是物理分区读数（见 §6.2 警示）。
5. **`size()==0` 对目录是正常值**，不是错误；判断错误看 handler 返回值 / 上行 `s` 字段。
6. **只读铁律**：`system.flash` 的遍历绝不能演变成"顺便清理/修复文件系统"。
   任何写操作都属于其它模块的职责。
7. **payload 上限 8128 字节**，文件数量增长时优先考虑加过滤参数，而不是加大 MQTT 缓冲。
8. 新增系统指令的正确姿势：在 `system_command.h/.cpp` 加 `syscmd_xxx()` →
   在 `command_manager.cpp` 加前向声明 + `system_router` 分支 + handler。
   **不要**绕过 CommandManager 直接对接 CloudManager。
9. 编译时用**全新 build 目录**（如 `PLATFORMIO_BUILD_DIR=.pio/build/wb_xxx`），
   沙箱 `safe-delete` 会拦截 `.o` 删除导致增量编译必然失败——这是环境问题，不是代码问题。

---

## 附：V1 指令速查

| 分类 | 指令 | 同步性 | 下行 payload |
|---|---|---|---|
| CONTROL | `system.restart` | 异步（accepted → result → 10s → 重启） | `{"o":"restart"}` / `{"o":"restart","cancel":true}` |
| RESOURCE | `system.memory` | 同步 | `{"o":"memory"}` |
| RESOURCE | `system.flash` | 同步（含递归文件列表） | `{"o":"flash"}` |

V1 不提供其它系统指令。`system.files` 已合并进 `system.flash`。
