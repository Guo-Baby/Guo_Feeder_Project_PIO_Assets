# JsonStorage 接口文档

> 项目：Guo Feeder Project（ESP32-S3 N16R8）
> 版本：定版（含 mkdir 收口，编译通过 EXIT=0）
> 面向：AI 辅助开发 / 后续维护
> 关联源码：`src/json_storage.h`、`src/json_storage.cpp`
> 关联文档：`jsonstorage开发架构.md`、`config_manager接口文档.md`

---

## 1. 模块定位

JsonStorage 是**底层通用文件存储模块**。职责只有一句话：**把文件可靠地读出来、写进去。**

- **负责**：文件的存在性 / 大小 / 删除 / 重命名 / 目录创建 / CRC32 校验、一次性读写、原子写、分块读写、流式句柄
- **不负责**：JSON 字段解析、业务参数合法性、Config 版本管理、Backup / Recovery 策略、Workflow 业务逻辑、Cloud / MQTT、Command

它**不依赖 ArduinoJson**，内容对它是不透明的字节流，因此同样适用于 BIN / OTA / Log / 设备档案 / 校准数据等非 JSON 文件。

### 分层位置（依赖严格单向，禁止反向）

```
ConfigManager / WorkflowManager（业务策略：版本 / backup / 轮换 / 校验）
    ↓
JsonStorage（本模块：文件可靠读写）
    ↓
LittleFS（文件系统，挂载生命周期由 main.cpp setup() 统一管理）
```

**禁止跨层**：业务模块（如 ConfigManager）不得直接调用 `LittleFS.*`，目录创建等一切文件操作必须经 JsonStorage。

---

## 2. 初始化与生命周期

### 2.1 `json_storage_init()`

```cpp
bool json_storage_init();
```

- **只做可用性确认，不负责挂载**。系统在 `setup()` 阶段已统一 `LittleFS.begin()`（main.cpp），本模块**永不调用** begin/end。
- 探测方式：`LittleFS.totalBytes() == 0` → 视为未挂载，返回 false。
- **幂等**：已初始化时重复调用直接返回 true。
- 返回 false 时，**后续所有接口都会失败**（exists 返回 false、size 返回 0、读写返回 false），不会崩溃但也不会有任何副作用。

### 2.2 日志回调（可选）

```cpp
typedef void (*JsonStorageLogCallback)(const char *level, const char *message);
void json_storage_set_log_callback(JsonStorageLogCallback callback);
```

- `level` 取值：`"E"`（ERROR）/ `"W"`（WARN）/ `"I"`（INFO）
- **未注册回调时模块完全静默**（不产生任何串口输出，与 ConfigManager 的 `cfg_log` 串口兜底不同），不影响功能。
- `message` 已格式化，单条上限 192 字节，超长截断。
- 传 `nullptr` 关闭日志。
- 设计意图：底层不散落 Serial 输出，未来由 Log Manager 统一接管。

---

## 3. 接口总览

| 分类 | 接口 | 幂等 | 一句话说明 |
|---|---|---|---|
| 目录 | `json_storage_mkdir` | ✅ | 创建目录（LittleFS 不自动建父目录，写文件前必须先建） |
| 基础 | `json_storage_exists` | — | 文件是否存在 |
| 基础 | `json_storage_size` | — | 文件大小（字节），不存在返回 0 |
| 基础 | `json_storage_remove` | ✅ | 删除文件（不存在也返回 true） |
| 基础 | `json_storage_rename` | — | 重命名（目标存在则覆盖），原子替换/backup 轮换的基石 |
| 校验 | `json_storage_crc32` | — | 流式计算整文件 CRC32（512B 栈缓冲，不占堆） |
| 一次性 | `json_storage_read` | — | 整文件读入 String，**上限 32KB** |
| 一次性 | `json_storage_write` | — | 截断覆盖写，校验实际写入字节数，**非断电安全** |
| 一次性 | `json_storage_write_atomic` | — | **原子写**（tmp → 校验 → rename → 读回确认），断电安全 |
| 分块 | `json_storage_read_chunk` | — | 按 offset 读取，`bytes_read==0` 表示 EOF |
| 分块 | `json_storage_write_chunk` | — | 按 offset 写入，offset>0 拒绝越界（防丢块产出损坏文件） |
| 流式 | `json_storage_open_read / open_write / open_append` | — | 打开句柄（防止重复打开泄漏） |
| 流式 | `json_storage_read / json_storage_write`（句柄重载） | — | 从已打开句柄读 / 写，返回实际字节数 |
| 流式 | `json_storage_close` | ✅ | 关闭句柄（先 flush），重复关闭安全 |

**注意**：`json_storage_read` / `json_storage_write` 各有**两个重载**——path 版（一次性）与句柄版（流式），用途完全不同，不要混用。

---

## 4. 通用约定（所有接口）

1. **未初始化保护**：`json_storage_init()` 未成功前调用任何接口，直接失败并记日志，无副作用。
2. **参数校验**：`path` 为 `nullptr` 或空串 → 失败；缓冲区为 `nullptr` / 长度为 0 → 失败（或无副作用）。
3. **无互斥**：模块内部**没有锁**。多任务并发访问同一文件需上层自行串行化（当前 ConfigManager 命令队列天然单线程执行，无需额外处理）。
4. **路径**：以 `/` 开头的 LittleFS 绝对路径，如 `/config/wifi.json`。
5. **返回值即契约**：所有 bool 返回 false 都代表"本次操作未可信完成"，调用方必须检查，**不存在静默失败**（失败时必有 `E`/`W` 级日志，前提是注册了回调）。

---

## 5. 接口详细说明

### 5.1 目录：`json_storage_mkdir`

```cpp
bool json_storage_mkdir(const char *path);   // 例: "/factory"
```

- **背景**：LittleFS 写文件时**不会自动创建父目录**。写入任何"非随 uploadfs 烧录"的目录（如 `/factory/`）前必须先调用，否则 `write` / `write_atomic` 会因无法创建 `.tmp` 文件而失败。
- 幂等：目录已存在直接返回 true，可安全重复调用。
- 只建单级目录（LittleFS mkdir 语义），不做递归创建。

### 5.2 基础操作

```cpp
bool   json_storage_exists(const char *path);
size_t json_storage_size(const char *path);
bool   json_storage_remove(const char *path);
bool   json_storage_rename(const char *from, const char *to);
```

| 接口 | 语义要点 |
|---|---|
| `exists` | 纯查询，无日志噪音 |
| `size` | 内部 open("r") 读取，不存在 / 打不开返回 **0**。注意 0 同时可能是空文件的真实大小，需要区分时先 `exists` |
| `remove` | **幂等**：文件本就不存在返回 true（语义是"确保不存在"）。不理解 backup / version，删谁由上层决定 |
| `rename` | 源文件不存在 → false；**目标存在则覆盖**（LittleFS rename 语义）。这是上层实现原子替换与 backup 轮换的基础，本函数不理解"为什么 rename" |

### 5.3 校验：`json_storage_crc32`

```cpp
uint32_t json_storage_crc32(const char *path);
```

- 标准 CRC-32（初始 `0xFFFFFFFF`，多项式 `0xEDB88320`），512B 栈缓冲流式读取，**不把文件加载进 RAM**，不占堆。
- 适用 Config / Workflow / BIN / OTA 任意文件完整性校验。
- **歧义警告**：文件不存在 / 打不开 / 读失败时返回 **0**，但 CRC32 合法值也可能恰好为 0。需严格区分时先 `exists()` + `size()` 确认文件有效。

### 5.4 一次性读写（小型文件 ≤ 32KB）

```cpp
bool json_storage_read(const char *path, String &output);
bool json_storage_write(const char *path, const String &data);
```

**read 行为**：
- 先 `output.clear()`，再按 512B 块读入；`reserve(total)` 一次性预留，避免反复重分配。
- **文件 > 32768 字节直接拒绝**（`JS_MAX_FULL_READ`），强制走 `read_chunk`，防止大文件灌爆 RAM。
- 收尾校验 `output.length() == 文件大小`，不一致视为不完整 → 清空 output 并返回 false。
- 任何失败都会把 `output` 清空后返回 false，不会返回半截数据。

**write 行为**：
- 截断覆盖（open "w"）。
- **打开成功 ≠ 写入成功**：校验实际写入字节数 == `data.length()`，不等则 false。
- ⚠️ **直接覆盖，写入中途断电会损坏文件**。配置类数据一律用 `write_atomic`。

### 5.5 原子写：`json_storage_write_atomic`（核心接口）

```cpp
bool json_storage_write_atomic(const char *path, const String &data);
```

四步流程，任何一步失败都会清理残留 `.tmp` 并返回 false，**正式文件保持替换前的完整内容**：

```
① data → path.tmp          完整写入临时文件（复用 write，含字节校验）
② size(path.tmp) 校验      与 data.length() 不符 → 删 tmp，失败
③ LittleFS.rename(tmp, path)  原子替换（依赖 rename 原子语义）
④ size(path) 读回确认      最终落盘校验（只读大小，不分配缓冲）
```

- **为什么有第 ④ 步**：ESP32 Arduino 的 `File::flush()` 返回 void，无法直接确认落盘；重新打开读回大小是唯一可信校验手段。④ 失败时返回 false——此时新文件已在位但**内容不可信**，上层应走自己的回滚策略（ConfigManager 由此触发 Backup Recovery）。
- **不做**：不生成 backup。current / backup 的轮换、版本更新全部由上层 ConfigManager 决定。
- 适用体积：与 read 同量级（32KB 内）。更大的文件（BIN/OTA）：流式写 tmp → 自己 `json_storage_rename()` 完成替换。

### 5.6 分块读写

```cpp
bool json_storage_read_chunk(const char *path, size_t offset,
                             uint8_t *buffer, size_t buffer_size,
                             size_t &bytes_read);
bool json_storage_write_chunk(const char *path, size_t offset,
                              const uint8_t *data, size_t length);
```

**分块大小完全由调用方决定**（Config 512B / Cloud Transfer 256B / OTA 4096B 均可），模块不写死数值。Flash IO chunk 与 MQTT packet chunk 属不同层，互不影响。

**read_chunk 返回语义**（三种，易混淆，务必区分）：

| 情况 | 返回 | bytes_read |
|---|---|---|
| 正常读到数据 | true | 实际字节数 |
| offset == 文件大小（恰好读完） | **true** | **0**（EOF） |
| offset > 文件大小 / 打不开 / seek 失败 / 参数非法 | false | 0 |

**write_chunk 语义**：

| offset | 行为 |
|---|---|
| `== 0` 且文件不存在 | 新建文件 |
| `== 0` 且文件存在 | **截断后从头写**（用 "w" 打开。若沿用 "r+" 覆写，新数据短于旧文件时尾部残留旧字节 → 半新半旧的损坏文件，代码注释已明确此坑） |
| `> 0` | 续写，要求文件已存在且 `offset <= 当前文件大小`，**越界直接 false 而不是填 0**——上层分块接收（BEGIN/CHUNK/END）时能立刻察觉丢块/乱序 |
| length == 0 | 无副作用，直接 true |

- 每次调用都会 open → write → flush → close（无句柄驻留），天然适合云端分块下发这种"块间隔不确定"的场景。

### 5.7 流式接口（大文件顺序处理）

```cpp
struct JsonStorageFile { File file; };

bool   json_storage_open_read  (const char *path, JsonStorageFile &file);
bool   json_storage_open_write (const char *path, JsonStorageFile &file);  // 截断写
bool   json_storage_open_append(const char *path, JsonStorageFile &file);  // 追加写

size_t json_storage_read (JsonStorageFile &file, uint8_t *buffer, size_t buffer_size);
size_t json_storage_write(JsonStorageFile &file, const uint8_t *data, size_t length);

bool   json_storage_close(JsonStorageFile &file);
```

- 适用：大 JSON / BIN / OTA / Log / Cloud Transfer 等需长句柄顺序处理的场景。
- **三个 open 都会拒绝"句柄已打开"的传入**（防止复用同一结构体造成句柄泄漏），报 `"handle already open"`。
- 句柄版 `read`：返回实际字节数，**0 = EOF**（或参数非法 / 句柄未打开——注意与分块版不同，此处错误与 EOF 同为 0，需调用方自行判断）。
- 句柄版 `write`：返回实际写入字节数，**0 = 失败**（句柄无效 / data 空 / length 0）。调用方必须校验返回值 == length。
- `close`：先 flush 再 close，**幂等**（未打开/已关闭返回 true）。
- ⚠️ **已知限制（硬件 API 决定，无法在本层解决）**：ESP32 的 `File::flush()/close()` 均无返回值，本接口**无法感知底层落盘失败**（分区写满、flash 写保护）。需要可靠确认的场景用 `json_storage_write()`（path 版）并校验返回值，或写完后 `size()` 复核。
- 使用方负责配对 open/close，**不得长期持有句柄**。

---

## 6. 内部常量（了解即可，勿在模块外引用）

| 常量 | 值 | 含义 |
|---|---|---|
| `JS_MAX_FULL_READ` | 32768 | 一次性读取体积上限，超过必须分块 |
| `JS_BLOCK_SIZE` | 512 | 内部栈上读缓冲，避免堆分配与碎片 |
| `JS_PATH_MAX` | 256 | 原子写合成 tmp 路径的缓冲上限（即 path + ".tmp" 总长 ≤ 255） |
| `JS_TMP_SUFFIX` | ".tmp" | 原子写临时文件后缀（失败时会被自动清理） |
| `JS_LOG_BUF_SIZE` | 192 | 单条日志上限，超长截断 |
| `JS_BASE_PATH` / `JS_MAX_OPEN_FILES` / `JS_PARTITION_LABEL` | — | 仅作挂载识别信息保留，模块不使用 |

---

## 7. 典型用法

### 7.1 小型配置 JSON（ConfigManager 模式）

```cpp
// 前置：main.cpp setup() 已 LittleFS.begin()

json_storage_init();

// 读
String content;
if (json_storage_read("/config/wifi.json", content)) {
    // 交给 ArduinoJson 解析（解析/校验是上层的事）
}

// 写（断电安全，配置写入一律用它）
if (!json_storage_write_atomic("/config/wifi.json", new_content)) {
    // 写入不可信 → 上层走 Backup 恢复策略
}

// 非烧录目录先建目录（幂等，可在 init 时调用一次）
json_storage_mkdir("/factory");
```

### 7.2 云端分块接收文件（BEGIN / CHUNK / END 模式）

```cpp
// BEGIN: 收到 offset=0 + 第一块
json_storage_write_chunk(path, 0, data1, len1);        // 新建/截断

// CHUNK: 按 offset 顺序续写
if (!json_storage_write_chunk(path, offset, chunk, n)) {
    // offset 越界 = 丢块或乱序 → 立刻中止本次传输，不得继续
}

// END: 完整性确认
size_t expect = /* 云端声明的总大小 */;
if (json_storage_size(path) != expect) { /* 损坏，重传 */ }
uint32_t crc = json_storage_crc32(path);               // 可选，双重校验
```

### 7.3 大文件顺序处理（流式）

```cpp
JsonStorageFile f;
if (!json_storage_open_read("/log/app.bin", f)) return;

uint8_t buf[1024];
size_t n;
while ((n = json_storage_read(f, buf, sizeof(buf))) > 0) {
    // 处理 n 字节
}
json_storage_close(f);   // 幂等，务必配对调用
```

---

## 8. 开发注意事项（踩坑清单）

1. **写非烧录目录前先 mkdir**。LittleFS 不自动建父目录；`/config/` 之所以存在是因为 `data/config/*.json` 随 uploadfs 烧录创建了它，`/factory/` 这类目录必须代码显式创建。
2. **配置类写入一律用 `write_atomic`**，不要用 `json_storage_write`（后者断电即损毁）。`write` 仅用于写 `.tmp` / 日志等可丢弃数据。
3. **所有返回值必须检查**。本模块不存在静默失败，但返回 false 只有在注册了日志回调时才能在日志里看到原因。
4. **`read_chunk` 的 EOF 是 `(true, bytes_read==0)`**，而句柄版 `read` 的 EOF 是 `0`——两套语义不同，封装时不要搞混。
5. **`write_chunk` 从 offset=0 写入会截断**——"从头写"和"续写"是同一个接口，靠 offset 区分。
6. **区分"错误 0"与"真实 0"**：`size()==0` 可能是空文件也可能是不存在；`crc32()==0` 可能是合法值也可能是错误。需要区分时先 `exists()`。
7. **流式句柄不可复用、不可长期持有**；三个 open 接口会主动拒绝已打开的句柄。
8. **落盘不可知论**：`flush()/close()` 无返回值是 ESP32 Arduino 的硬限制。本模块在 `write_atomic` 内部用"读回大小"补救，自己写流式逻辑时也应写完后 `size()` 复核。
9. **不越层**：上层模块禁止直接 `#include <LittleFS.h>`；任何文件操作（含 mkdir / rename / remove）都走 JsonStorage。

---

## 9. 已知边界

- 无互斥锁，并发访问需上层串行化。
- `mkdir` 不递归（单级）。
- `read`/`write` 的 String 参数意味着存在一次堆分配（约等于文件大小），32KB 上限即为此而设；对内存极敏感的场景用分块接口。
- `rename` 目标覆盖、`remove`/`mkdir`/`close` 幂等，都是刻意设计——上层轮换/恢复策略依赖这些语义。
