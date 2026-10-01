# BinStorage / FileStorage 开发说明（第一阶段）

> 对应需求：`bin_storage开发需求.md`
> 本阶段目标：在**不重构、不修改现有 JSON 机制**的前提下，新增通用 BIN / 原始二进制文件存储能力。
> 状态：**已完成，编译通过（0 error / 0 新增 warning），未烧录**。

---

## 一、完成情况总览

| 项目 | 结果 |
|---|---|
| 编译 | ✅ SUCCESS（67.9s，全量） |
| 新增 warning | 0 |
| 新增 error | 0 |
| RAM | 39.0%（127848 / 327680） |
| Flash | 61.6%（1292225 / 2097152） |
| 烧录 | ❌ 未烧录（按要求） |
| 修改现有 JSON 机制 | ❌ 无（JsonStorage / ConfigManager 零改动） |
| 修改 Workflow | ❌ 无 |

---

## 二、新增文件

| 文件 | 层级 | 职责 |
|---|---|---|
| `src/file_storage.h` / `.cpp` | 最底层 | 通用文件操作，只认识 `path + raw bytes + length` |
| `src/bin_storage.h` / `.cpp` | BIN 层 | 通用 BIN 存储，提供错误码语义 |

修改的既有文件：

| 文件 | 改动 | 说明 |
|---|---|---|
| `src/main.cpp` | +10 行 | 新增 `#include "bin_storage.h"` 与 `bin_storage_init()` 调用（第一层，紧随 `json_storage_init()`） |

`main.cpp` 是唯一被修改的既有文件，且为纯增量（include + 初始化调用），不改变任何既有逻辑与执行顺序。

---

## 三、目标架构（已形成）

```text
   ConfigManager                  WorkflowManager（下一阶段接入）
         ↓                                 ↓
    JsonStorage                        BinStorage
         ↓                                 ↓
   ───────────────── LittleFS ─────────────────
              （JsonStorage 侧复用既有实现）

   BinStorage → FileStorage → LittleFS
```

- **JsonStorage 与 FileStorage 平级**，各自独立封装 LittleFS，互不包含、互不调用。
- BinStorage 只依赖 FileStorage，不直接触碰 LittleFS。
- 严格单向依赖，无反向依赖。

---

## 四、API 清单

### 4.1 FileStorage（最底层，`file_storage.h`）

| API | 说明 |
|---|---|
| `file_storage_init()` | 确认 LittleFS 可用；不负责 `begin()`；幂等 |
| `file_storage_exists(path)` | 文件是否存在 |
| `file_storage_size(path)` | 文件大小（0 = 空文件或不存在） |
| `file_storage_mkdir(path)` | 创建目录；幂等 |
| `file_storage_remove(path)` | 删除；幂等 |
| `file_storage_rename(from, to)` | 重命名 / 移动 |
| `file_storage_read(path, offset, buf, size, &n)` | offset 定位读取；EOF 返回 true + n=0 |
| `file_storage_write(path, offset, data, len)` | offset==0 截断写；offset>0 续写（拒绝越界） |
| `file_storage_crc32(path, &crc)` | 流式 CRC32（512B 栈缓冲，不加载整文件） |
| `file_storage_open_read/write/append` | 流式句柄 |
| `file_storage_read/write(handle, ...)` | 句柄读写 |
| `file_storage_close(handle)` | 关闭；幂等 |

数据一律 `uint8_t* + length`：**不使用 String、不使用 `strlen()`、允许 `0x00`**。

### 4.2 BinStorage（BIN 层，`bin_storage.h`）

| API | 返回 | 说明 |
|---|---|---|
| `bin_storage_init()` | bool | 内部级联初始化 FileStorage |
| `bin_storage_exists(path)` | bool | — |
| `bin_storage_size(path)` | size_t | — |
| `bin_storage_mkdir(path)` | Result | — |
| `bin_storage_read(path, buf, size, &n)` | Result | buffer 不足 → `BUFFER_TOO_SMALL`，**一个字节都不读** |
| `bin_storage_read_chunk(path, offset, ...)` | Result | 大文件分块读 |
| `bin_storage_write(path, data, len)` | Result | 截断覆盖（非断电安全） |
| `bin_storage_write_chunk(path, offset, ...)` | Result | 分块写 |
| `bin_storage_write_atomic(path, data, len)` | Result | **原子替换**（tmp → 写 → 校验 → rename → 终检） |
| `bin_storage_remove(path)` | Result | 幂等 |
| `bin_storage_rename(from, to)` | Result | — |
| `bin_storage_crc32(path, &crc)` | Result | 原始 CRC32 工具 |
| `bin_storage_result_name(r)` | const char* | 错误码转字符串 |
| `bin_storage_set_log_callback(cb)` | void | 可选日志回调（"E"/"W"/"I"） |

### 4.3 错误码（`BinStorageResult`）

```text
BIN_STORAGE_OK                    成功
BIN_STORAGE_ERR_NOT_INITIALIZED   文件系统不可用 / 未初始化
BIN_STORAGE_ERR_INVALID_ARGUMENT  path 为空、长度与指针矛盾等
BIN_STORAGE_ERR_NOT_FOUND         文件不存在
BIN_STORAGE_ERR_BUFFER_TOO_SMALL  调用方 buffer 小于文件实际大小
BIN_STORAGE_ERR_OPEN_FAILED       打开失败
BIN_STORAGE_ERR_READ_FAILED       读取失败 / 长度不足
BIN_STORAGE_ERR_WRITE_FAILED      写入字节数不符或落盘校验失败
BIN_STORAGE_ERR_DELETE_FAILED     删除失败
BIN_STORAGE_ERR_RENAME_FAILED     重命名 / 替换失败
```

> 未新造复杂错误体系；项目无统一 Storage 错误码（既有 `ErrorCode` 属云端命令域、`ConfigCommandError` 属配置命令域，均不适合复用），故在本模块内定义最小枚举。

---

## 五、关键设计决策

1. **不重构 JsonStorage**：需求 §2.1 是最高约束。两条链路并行共存，BinStorage 独立实现所需底层能力，不为"架构统一"迁移 ConfigManager。

2. **原子替换放在 BinStorage 而非 FileStorage**：
   - 依据项目既有约定——`json_storage.h` 自身文档即写明"更大的文件请用流式接口写入临时文件，再用 `json_storage_rename()` 自行完成原子替换"。
   - 因此 FileStorage 只提供 `write / rename / remove` 原语，BinStorage 用它们组合出原子事务，可精确区分 `WRITE_FAILED` / `RENAME_FAILED`，且不重复实现两遍。

3. **FileStorage 返回 bool、BinStorage 返回错误码**：与 JsonStorage 的 bool 风格保持一致（底层原语），错误语义集中在上层（对外 API），避免同一存储栈维护两套错误体系。

4. **内存约束**：无动态内存分配；临时路径用栈上定长 `char[256]`；CRC32 用栈上 512B 分块；不复制业务 buffer；不长期持有句柄。

5. **零业务耦合**：无 `W01S00.bin` 命名规则、无 WORKFLOW_MAX_COUNT/STEP、无 Dirty / Critical / Meta / valid / step_count / Action / Trigger；不提供 `makeWorkflowStepPath()` 之类的辅助函数。

---

## 六、验收标准自查（需求 §二十一）

| 验收项 | 状态 |
|---|---|
| ConfigManager → JsonStorage 功能/接口/行为基本不变 | ✅ 零改动 |
| 新增 exists / size / read / write / replace(atomic) / remove / rename | ✅ 全部提供 |
| 形成 Workflow → BinStorage → FileStorage → LittleFS 分层 | ✅（Workflow 侧下一阶段接入） |
| 只新增 BIN 支持 | ✅ |
| 不重构 ConfigManager | ✅ |
| 不修改 JsonStorage JSON 业务机制 | ✅ |
| 不修 ConfigManager VAR 合法性 | ✅ |
| 不修改 Workflow | ✅ |
| 无新增 warning / 无 API 冲突 | ✅ |
| 不修改无关代码 | ✅（仅 main.cpp +10 行初始化接入） |

---

## 七、本阶段未做（按需求约定）

- **未做运行时测试**（用户要求：无需烧录测试，只需编译通过）。
  需求 §十九 的 BIN 写读比对（含 `0x00/0x01/0x7F/0x80/0xFF`）、覆盖、删除、rename 测试留待上板阶段执行。
- **未创建 `WorkflowStorage` / `WorkflowBinStorage`** —— 需求 §五 明确禁止。
- **未接入 Workflow** —— 需求 §十四 明确禁止，属第二阶段。
- **未执行 git commit** —— 等待确认。

---

## 八、下一阶段建议接入方式（不在本阶段实现）

```text
保存：StepDefinition → binary serialize → BinStorage::write_atomic() → W01S03.bin
读取：W01S03.bin → BinStorage::read() → byte buffer → WorkflowManager deserialize
```

序列化由 WorkflowManager 负责，Storage 不参与。
