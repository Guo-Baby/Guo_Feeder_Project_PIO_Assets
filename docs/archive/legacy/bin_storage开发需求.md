# File Storage / BIN Storage 新增功能需求文档

## 一、任务背景

当前项目已经存在一套经过实际开发和验证的 JSON 配置存储机制，主要用于 ConfigManager 管理各模块的 JSON 配置文件。

当前 JSON 体系已经包含：

* JSON 文件读取
* JSON 文件保存
* JSON 文件修改
* JSON 解析
* JSON 序列化
* ConfigManager 对各模块配置文件的管理

**这套 JSON 机制目前不进行重构。**

后续确实存在一个 ConfigManager 的 `var` 合法性校验机制问题：目前设计上通过 `var` 值判断参数是否合法，但现有实现尚未完善，后续单独处理。

**本次任务禁止处理这个问题。**

当前阶段的唯一目标是：

> 在不破坏、不重构、不改变现有 JSON 存储机制的前提下，为项目新增一套通用的 BIN / 原始二进制文件存储能力，为下一阶段 Workflow Manager 的 BIN 持久化改造提供底层 API。

---

# 二、本次任务的核心原则

## 2.1 不修改现有 JSON 机制

这是本次任务最重要的约束。

现有：

```text
ConfigManager
    ↓
JsonStorage
    ↓
LittleFS
```

保持原样。

不得为了实现 BIN 支持而：

* 重构 ConfigManager
* 修改 ConfigManager 的 JSON 业务逻辑
* 修改 ConfigManager 的 JSON 数据结构
* 修改 JsonStorage 的现有接口语义
* 把 JsonStorage 改造成新的统一接口
* 把 JSON 解析/序列化逻辑搬到其他模块
* 修改 ConfigManager 当前 `var` 校验机制
* 顺便修复 ConfigManager 的其他问题

**尤其不要为了“架构统一”而迁移现有 ConfigManager。**

现有 JSON 代码已经能够工作，应最大程度保持不动。

---

# 三、本次新增的架构

本次只增加 BIN 文件能力。

目标结构：

```text
                 ConfigManager
                       ↓
                  JsonStorage
                       ↓
                    LittleFS


                 WorkflowManager
                       ↓
                   BinStorage
                       ↓
                   FileStorage
                       ↓
                    LittleFS
```

其中：

### JsonStorage

负责现有 JSON 相关能力。

本次不重构。

### BinStorage

本次新增。

负责：

* BIN 文件读取
* BIN 文件写入
* BIN 文件替换/覆盖
* BIN 文件删除
* BIN 文件存在性判断
* BIN 文件大小获取
* 原始 byte 数据处理

### FileStorage

如果当前项目已有适合的底层 FileStorage，可以复用。

如果没有，则新增一个**非常薄的通用文件操作层**，用于封装 LittleFS 的底层文件操作。

其职责只能是：

> 文件系统操作。

它不负责：

* JSON 业务
* Workflow 业务
* StepDefinition
* WorkflowMeta
* Dirty
* Critical
* CRC 业务规则
* Action
* Trigger

---

# 四、为什么不直接把 JsonStorage 改成 FileStorage

不要将当前：

```text
JsonStorage
```

强行改造成：

```text
FileStorage
```

因为 JsonStorage 已经承担了 JSON 数据层职责。

例如：

```text
JSON Document
↓
deserialize
↓
修改字段
↓
serialize
↓
保存
```

这些不是通用文件系统职责。

BIN 文件则完全不同：

```text
struct / byte buffer
↓
binary serialize
↓
byte buffer
↓
写入文件
```

因此本项目应该采用：

```text
JsonStorage
    → 保持原有 JSON 能力

BinStorage
    → 新增 BIN 能力

FileStorage
    → 可选的底层通用文件操作
```

而不是：

```text
所有东西强制改成 FileStorage
```

---

# 五、BinStorage 功能要求

新增一个独立的 BIN Storage 模块。

建议命名：

```text
bin_storage.h
bin_storage.cpp
```

如果项目当前命名规则有既定规范，可以遵循项目现有命名规则。

不要创建 Workflow 专用 Storage。

也就是说，本次：

**不要创建：**

```text
WorkflowStorage
WorkflowBinStorage
WorkflowFileManager
```

BIN Storage 应该是通用能力。

---

# 六、BinStorage 必须提供的基础能力

具体 API 名称可以根据当前项目代码风格确定，但必须具备以下能力。

## 6.1 判断文件是否存在

功能：

```text
exists(path)
```

例如：

```text
/W01S00.bin
```

返回：

```text
true / false
```

---

## 6.2 获取文件大小

功能：

```text
size(path)
```

返回：

```text
size_t
```

用于读取 BIN 前确定数据长度。

---

## 6.3 读取 BIN

提供：

```text
read(path, buffer, buffer_size)
```

或者等价 API。

要求：

* 支持原始二进制数据
* 不进行 JSON 解析
* 不对数据内容做任何解释
* 正确返回实际读取长度
* 正确处理文件不存在
* 正确处理 buffer 不足
* 正确处理读取失败

例如：

```text
W01S03.bin
        ↓
uint8_t buffer[]
        ↓
BinStorage
```

BinStorage 不知道这个 BIN 是：

```text
Workflow Step
```

还是：

```text
Calibration
```

还是：

```text
其他数据
```

---

# 七、BIN 写入

提供类似：

```text
write(path, data, length)
```

功能。

要求：

* 支持任意 binary byte
* 支持 `uint8_t* + length`
* 数据中允许出现 `0x00`
* 不允许使用 C 字符串规则判断长度
* 不允许使用 `strlen()` 等字符串逻辑
* 正确处理长度
* 正确处理写入失败

例如：

```text
0x00
0x01
0xFF
0x00
...
```

都必须能够正常保存。

---

# 八、BIN 覆盖 / Replace

需要支持安全的文件替换能力。

至少应具备：

```text
write / replace
```

这样的能力。

如果当前 JsonStorage 已经存在：

```text
临时文件
↓
写入
↓
关闭
↓
rename
```

这样的安全保存机制，则可以复用其底层实现思想。

但注意：

**不要修改 JsonStorage 的现有外部行为。**

可以复用底层代码，也可以在 FileStorage 中提供通用能力。

目标是：

```text
旧文件
      ↓
新数据写入临时文件
      ↓
写入成功
      ↓
完成替换
```

而不是直接破坏原文件后再写。

---

# 九、BIN 删除

提供：

```text
remove(path)
```

或项目现有命名风格对应的 API。

要求：

* 文件存在 → 删除
* 文件不存在 → 返回合理结果
* 删除失败 → 能够明确返回错误

注意：

本次只是提供通用删除能力。

**不要在 BinStorage 中加入 Workflow 删除逻辑。**

例如：

```text
“删除 Workflow 4 时不能删除 Step BIN”
```

这是下一阶段 Workflow Manager 的业务规则。

BinStorage 不知道 Workflow 是什么。

---

# 十、BIN Rename

建议提供通用：

```text
rename(old_path, new_path)
```

能力。

主要用于：

* 安全替换
* 临时文件
* 后续事务机制
* 文件迁移

同样，BinStorage 不应该理解 Workflow。

---

# 十一、原始 FileStorage 层

如果新增 FileStorage，则只负责最底层的通用文件操作。

建议能力包括：

```text
exists
size
read
write
remove
rename
atomic replace
```

FileStorage 的输入应该是：

```text
path
+
raw bytes
+
length
```

而不是：

```text
JsonDocument
Workflow
StepDefinition
WorkflowMeta
```

例如：

```cpp
FileStorage::write(
    "/W01S03.bin",
    data,
    data_length
);
```

FileStorage 不关心：

```text
W01S03
```

是什么意思。

---

# 十二、不要在 Storage 层加入业务逻辑

以下内容**禁止进入 FileStorage / BinStorage**：

```text
Workflow 最大数量 = 16
Step 最大数量 = 16
W01S00.bin
WorkflowMeta
Dirty Bitmap
Critical
Action
Trigger
WorkflowDefinition
WorkflowRuntime
Step 0 必须 Trigger
step_count
Workflow valid
```

这些全部属于：

```text
WorkflowManager
```

而不是 Storage。

正确关系：

```text
WorkflowManager
    ↓
BinStorage
    ↓
FileStorage
    ↓
LittleFS
```

---

# 十三、与现有 JsonStorage 的关系

本次不要求把所有现有 JSON 调用迁移到新的 FileStorage。

也就是说：

**不要做：**

```text
ConfigManager
    ↓
FileStorage
```

的整体迁移。

保持：

```text
ConfigManager
    ↓
JsonStorage
```

即可。

如果实现过程中发现 JsonStorage 内部可以复用 FileStorage 的某些底层函数，可以进行非常小范围的内部复用，但必须满足：

1. JsonStorage 对外 API 不变。
2. ConfigManager 调用方式不变。
3. JSON 数据处理逻辑不变。
4. JSON 保存行为不变。
5. 不改变 ConfigManager 的功能。
6. 不顺便修改 `var` 校验。

如果为了引入 FileStorage 而需要大规模修改 JsonStorage：

> **不要做。**

宁可让 BinStorage 独立实现当前需要的底层文件能力。

---

# 十四、禁止修改 Workflow

本阶段只做 Storage。

**不得修改：**

```text
workflow.cpp
workflow.h
Workflow Manager 行为
Workflow JSON
Workflow Runtime
Workflow Action
Workflow Trigger
```

下一阶段才会进行 Workflow BIN 架构改造。

---

# 十五、未来 Workflow 的预期调用方式

本阶段完成以后，下一阶段 Workflow Manager 应该能够做到：

```text
workflow.cpp
     ↓
BinStorage
     ↓
FileStorage
     ↓
LittleFS
```

例如：

```text
保存：

StepDefinition
    ↓
binary serialize
    ↓
BinStorage::write()
    ↓
W01S03.bin
```

读取：

```text
W01S03.bin
    ↓
BinStorage::read()
    ↓
byte buffer
    ↓
WorkflowManager deserialize
    ↓
StepDefinition
```

Storage 本身不负责 StepDefinition 的序列化。

---

# 十六、错误处理

Storage API 必须能够区分至少：

```text
SUCCESS
FILE_NOT_FOUND
INVALID_ARGUMENT
BUFFER_TOO_SMALL
OPEN_FAILED
READ_FAILED
WRITE_FAILED
DELETE_FAILED
RENAME_FAILED
```

具体错误码可以按照项目已有错误处理规范设计。

不要为了这个模块重新创造一套复杂的错误系统。

如果项目已有统一错误码体系，应优先复用。

---

# 十七、内存要求

由于这是 ESP32-S3 项目，BIN Storage 不应该引入不必要的动态内存。

尤其注意：

* 不要把 BIN 自动转换成 String。
* 不要使用字符串函数处理 binary data。
* 不要为了方便而复制多份 buffer。
* `uint8_t* + length` 优先。
* 避免不必要的 malloc/free。
* 不要在 Storage 层长期持有业务 buffer。
* 读写完成后及时释放临时资源。

Storage 本身应该尽可能薄。

---

# 十八、目录与文件名

本阶段不需要实现 Workflow 文件命名规则。

例如：

```text
W01S00.bin
W01S01.bin
```

这些属于下一阶段 Workflow Manager。

BinStorage 只接受：

```text
path
```

例如：

```text
"/W01S00.bin"
```

不要在 BinStorage 中写：

```cpp
makeWorkflowStepPath(...)
```

因为这会把 Workflow 业务耦合进 Storage。

---

# 十九、测试要求

本阶段至少测试以下内容。

## JSON 回归测试

确认现有：

```text
ConfigManager
JsonStorage
```

行为没有变化。

至少验证：

* JSON 读取
* JSON 保存
* JSON 修改
* JSON 文件存在
* JSON 文件正常加载

**不得出现因为本次新增 BIN Storage 导致的 JSON 回归。**

---

## BIN 测试

测试：

```text
创建 BIN
↓
写入
↓
读取
↓
比较原始 bytes
↓
结果完全一致
```

测试数据必须包含：

```text
0x00
0x01
0x7F
0x80
0xFF
```

确保没有字符串处理问题。

---

## BIN 覆盖测试

```text
写入旧数据
↓
写入新数据
↓
读取
↓
确认读取到新数据
```

---

## BIN 删除测试

```text
创建
↓
exists = true
↓
delete
↓
exists = false
```

---

## Rename 测试

```text
old.bin
↓
rename
↓
new.bin
```

确认：

```text
old 不存在
new 存在
内容正确
```

---

## 编译测试

执行项目完整编译。

要求：

* 无新增 warning
* 无 API 冲突
* 不影响现有模块
* 不修改与本任务无关的代码

---

# 二十、Git 操作要求

开始修改之前：

```bash
git status
```

如果当前存在用户已有修改：

* 不覆盖
* 不 reset
* 不 stash 用户修改
* 不使用 `git add .`
* 先保护用户现有工作

修改过程中只修改本阶段必要文件。

完成后：

```bash
git diff
git status
```

仔细确认：

> 本次提交只包含 Storage/BIN 相关修改以及必要的工程配置修改。

不得把：

* ConfigManager 的 VAR 修复
* Workflow 修改
* 其他实验代码
* 临时文件
* AI 生成的报告

混入本次提交。

---

# 二十一、本阶段最终验收标准

本阶段完成后必须满足：

### 现有 JSON 系统

```text
ConfigManager
    ↓
JsonStorage
```

**功能、接口、行为基本保持不变。**

### 新增 BIN 系统

能够提供：

```text
exists
size
read
write
replace / atomic write
remove
rename
```

等通用能力。

### 架构

形成：

```text
ConfigManager
      ↓
 JsonStorage
      ↓
 LittleFS


Workflow
      ↓
 BinStorage
      ↓
 FileStorage（如果采用）
      ↓
 LittleFS
```

### 最重要的边界

本次任务：

**只新增 BIN 文件支持。**

本次任务：

**不重构 ConfigManager。**

本次任务：

**不修改现有 JsonStorage 的 JSON 业务机制。**

本次任务：

**不修 ConfigManager 的 VAR 合法性问题。**

本次任务：

**不修改 Workflow。**

---

# 二十二、给实施者的最终要求

请先完整检查当前项目已有的：

```text
JsonStorage
ConfigManager
LittleFS 调用
```

再决定具体文件和 API 的实现方式。

**不要看到“FileStorage”这个名字就把现有 JsonStorage 全部重构。**

本任务的核心不是“统一所有 Storage 接口”，而是：

> **在保护现有 JSON 架构的前提下，为后续 Workflow BIN 持久化增加一个稳定、通用、低耦合的二进制文件存储能力。**

完成后先停止，不要自行继续修改 Workflow。

等待上层审查通过后，再进行第二阶段：

```text
Workflow Definition / Runtime 分离
+
Step BIN
+
WorkflowMeta
+
Dirty Bitmap
+
Critical
+
Lazy Load
+
Workflow 持久化事务
```

第二阶段另行提供需求文档。
