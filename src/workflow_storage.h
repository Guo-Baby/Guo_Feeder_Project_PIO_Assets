#pragma once

#include <Arduino.h>
#include "bin_storage.h"

// =====================================================
// Workflow Storage — Workflow 定义持久化模块
// =====================================================
//
// 职责：
//   把 WorkflowDefinition / StepDefinition 序列化成 BIN，
//   可靠地写入 LittleFS，并在需要时还原回 RAM。
//
// 只认识：
//   Workflow 定义数据 + Slot 编号（workflow_id / step_id）
//
// 负责：
//   BIN Header / Version / CRC32 / 序列化 / 反序列化
//   Meta 管理 / 文件命名 / 原子保存 / 事务顺序（Meta 最后提交）
//
// 不负责（也永远不会负责）：
//   Action / Trigger 执行
//   Runtime 状态
//   Workflow 调度 / Step Poll
//   Dirty Bitmap（属于 WorkflowManager 状态）
//   Critical Operation 业务逻辑（属于 WorkflowManager 编排）
//
// 依赖方向（严格单向）：
//   WorkflowManager → WorkflowStorage → BinStorage → FileStorage → LittleFS
//
// 禁止：
//   调用 workflow_manager 任何接口（防止循环依赖）
//   直接 LittleFS.open / remove / rename
//   持久化函数指针 / Descriptor 指针 / Runtime / callback / result
//   保存任何运行时状态（掉电后 Workflow 一律从 STOPPED 重新开始）
//
// 文件布局：
//   /workflow/meta.bin
//   /workflow/wf00/step00.bin
//   /workflow/wf00/step01.bin
//   ...
//   /workflow/wf15/step15.bin
//
//   不存在的 / 未使用的 Step 不提前创建物理文件。
//
// 序列化约定：
//   全部手工逐字节 put/get，绝不 memcpy 结构体，
//   规避编译器对齐与 padding 差异导致的格式漂移。
// =====================================================


// =====================================================
// 容量常量
//
// 必须与 workflow.h 中的 WORKFLOW_MAX_COUNT / WORKFLOW_MAX_STEP /
// WORKFLOW_MAX_PARAM 保持一致（workflow_storage.cpp 中有 static_assert 校验）。
// 本头文件不 include workflow.h，避免与 Workflow 业务头文件相互包含。
// =====================================================

#define WF_STG_MAX_COUNT 16
#define WF_STG_MAX_STEP 16
#define WF_STG_MAX_PARAM 8


// =====================================================
// 路径与格式常量
// =====================================================

#define WF_STG_DIR "/workflow"
#define WF_STG_META_PATH "/workflow/meta.bin"

#define WF_STG_META_MAGIC 0x57464D54u   // "WFMT"
#define WF_STG_STEP_MAGIC 0x57465350u   // "WFSP"

// Step BIN 格式版本（不变，v1 布局稳定）
#define WF_STG_VERSION 1u
// Meta 格式版本：
//   v1 = 81B/entry（无 txn_id）
//   v2 = 85B/entry（+txn_id，Workflow 级事务提交标识）
//   v3 = 89B/entry（+variant，Workflow 内容版本，云端增量同步用）
//
// 兼容：v1 / v2 旧文件仍可读（txn_id / variant 视为 0），
// 写入一律用当前版本 v3。
#define WF_STG_META_VERSION 3u

#define WF_STG_ID_MAX_LEN 32
#define WF_STG_NAME_MAX_LEN 32
#define WF_STG_PARAM_NAME_LEN 16
#define WF_STG_PARAM_STR_LEN 32

// Step BIN 最大体积（Header + Payload），用于栈上缓冲
#define WF_STG_STEP_BIN_MAX 640u
// Meta BIN 最大体积（Header + 全部 Entry）
#define WF_STG_META_BIN_MAX 1536u


// =====================================================
// 操作结果
// =====================================================
//
// 复用 BinStorageResult 的语义与数值区间（前 10 项一一对应），
// 并在此基础上扩展 WorkflowStorage 层的语义错误。
//
// 之所以不直接扩展 BinStorageResult：
//   验收标准要求"原则上禁止修改 BinStorage"，
//   而底层通用存储层不应感知 Workflow 语义。
//   故本模块在自身作用域内定义对齐数值的错误码，
//   并提供 workflow_storage_from_bin_result() 做无损转换。

enum WorkflowStorageResult
{
    WF_STG_OK = 0,

    WF_STG_ERR_NOT_INITIALIZED = 1,
    WF_STG_ERR_INVALID_ARGUMENT = 2,
    WF_STG_ERR_NOT_FOUND = 3,
    WF_STG_ERR_BUFFER_TOO_SMALL = 4,
    WF_STG_ERR_OPEN_FAILED = 5,
    WF_STG_ERR_READ_FAILED = 6,
    WF_STG_ERR_WRITE_FAILED = 7,
    WF_STG_ERR_DELETE_FAILED = 8,
    WF_STG_ERR_RENAME_FAILED = 9,

    // ---- WorkflowStorage 语义错误 ----
    WF_STG_ERR_CRC_FAILED = 10,           // BIN 内容 CRC 校验失败
    WF_STG_ERR_VERSION_MISMATCH = 11,     // BIN 格式版本不兼容
    WF_STG_ERR_FORMAT_INVALID = 12,       // Magic 错误 / 长度非法
    WF_STG_ERR_VERSION_TOO_NEW = 13,      // BIN 版本高于本固件，拒绝解析
    WF_STG_ERR_TEST_ABORTED = 14          // 仅测试故障注入：模拟掉电中断点
};

// BinStorageResult → WorkflowStorageResult（数值对齐，无损）
WorkflowStorageResult workflow_storage_from_bin_result(
    BinStorageResult result
);

// 结果转可读字符串（静态字符串，无需释放）
const char *workflow_storage_result_name(
    WorkflowStorageResult result
);


// =====================================================
// 可持久化的数据结构（纯数据，无任何指针 / 无 String）
// =====================================================
//
// 这些结构只描述"下一次执行什么"，
// 与"这一次正在执行什么"（Runtime）彻底无关。

struct WorkflowParamValueDefinition
{
    char name[WF_STG_PARAM_NAME_LEN];
    uint8_t type;               // WorkflowParamType: 0=int 1=float 2=bool 3=string
    int32_t int_value;
    float float_value;
    uint8_t bool_value;
    char string_value[WF_STG_PARAM_STR_LEN];
};

struct WorkflowStepDefinition
{
    uint8_t type;               // WorkflowStepType: 0=ACTION 1=TRIGGER
    uint8_t instance_type;      // WorkflowInstanceType: 0=TRIGGER 1=ACTION
    char id[WF_STG_ID_MAX_LEN]; // Action / Trigger 字符串 ID，Descriptor 由 ID 重新解析
    uint8_t param_count;
    WorkflowParamValueDefinition params[WF_STG_MAX_PARAM];
};

struct WorkflowDefinition
{
    char id[WF_STG_ID_MAX_LEN];
    char name[WF_STG_NAME_MAX_LEN];
    uint8_t enable;
    uint32_t timeout_ms;
    uint8_t step_count;
    uint32_t variant;   // Workflow 内容版本（云端增量同步，禁止用时间戳）
    WorkflowStepDefinition steps[WF_STG_MAX_STEP];
};


// =====================================================
// Meta Entry（Workflow 级别持久化生命周期信息）
// =====================================================
//
// 只保存"Workflow 是否存在 / 有多少 Step / 何时更新 / 完整性"，
// 绝不保存 RUNNING / current_step / start_time / runtime / callback。

struct WorkflowMetaEntry
{
    uint8_t valid;
    uint8_t version;
    uint16_t step_count;
    uint32_t update_time;
    uint32_t crc32;      // 该 Workflow 全部 Step payload 的 CRC32
    char id[WF_STG_ID_MAX_LEN];
    char name[WF_STG_NAME_MAX_LEN];
    uint8_t enable;
    uint32_t timeout_ms;
    uint32_t txn_id;     // 最后已提交事务的标识（掉电恢复用，见 workflow_storage_recover）
    uint32_t variant;    // Workflow 内容版本（v3 起；v1/v2 旧文件读为 0）
};


// =====================================================
// 初始化
// =====================================================
//
// 内部级联初始化 BinStorage，并创建 /workflow 目录。
// 幂等：已初始化则直接返回 true。

bool workflow_storage_init();


// =====================================================
// Meta
// =====================================================

// 从 Flash 加载 meta.bin。
// 文件不存在时：初始化为全 Invalid 的空 Meta 并返回 true（首次启动属正常）。
// CRC 失败时：返回 false，Meta 保持全 Invalid，绝不猜测旧格式。
bool workflow_storage_load_meta();

// 将当前 RAM Meta 原子写入 meta.bin
bool workflow_storage_save_meta();

// 查询 / 设置某个 Workflow 是否有效
//
// 注意：set_valid 只改 RAM Meta，不会立即落盘。
//      落盘时机由上层（WorkflowManager）在事务中决定，
//      确保"Step BIN 全部写成功后再提交 Meta"。
bool workflow_storage_get_valid(
    uint8_t workflow_id
);

void workflow_storage_set_valid(
    uint8_t workflow_id,
    bool valid
);

// 读取 Meta 中记录的 step_count
uint8_t workflow_storage_get_step_count(
    uint8_t workflow_id
);

// 读取 / 写入 Meta 中的 Workflow 级别字段
bool workflow_storage_get_info(
    uint8_t workflow_id,
    char *id_out,
    size_t id_out_size,
    char *name_out,
    size_t name_out_size,
    bool &enable_out,
    uint32_t &timeout_ms_out
);

void workflow_storage_set_info(
    uint8_t workflow_id,
    const char *id,
    const char *name,
    bool enable,
    uint32_t timeout_ms
);

// -----------------------------------------------------
// Variant（Workflow 内容版本，v3 Meta 起持久化）
//
// 用途：云端增量同步的比对依据。
//   内容变化 → variant++ → Capability Registry checksum 变化
//   → 云端 workflow.list 发现不同 → workflow.get 拉取新内容
//
// 规则（禁止用时间戳）：
//   新建 = 1；每次内容修改 +1；删除 +1（删除不是物理删除）。
// -----------------------------------------------------
uint32_t workflow_storage_get_variant(
    uint8_t workflow_id
);

void workflow_storage_set_variant(
    uint8_t workflow_id,
    uint32_t variant
);


// =====================================================
// Workflow
// =====================================================

// 加载整个 Workflow（meta 信息 + 全部 Step BIN）
//
// 只加载 0 .. step_count-1，绝不读取 step_count .. 15。
// 任一 Step CRC 失败 → 返回 WF_STG_ERR_CRC_FAILED，
// 不破坏其他 Workflow，也不修改 Flash。
WorkflowStorageResult workflow_storage_load(
    uint8_t workflow_id,
    WorkflowDefinition *definition
);

// 保存整个 Workflow —— Workflow 级多 Step 事务（掉电安全）
//
// 事务过程（严格顺序）：
//   1. 生成事务标识 X = Meta.txn_id + 1
//   2. 逐个 Step 写入【暂存文件】stepNN.bin.tX（不触碰正式文件）
//      - 写完即做 大小 + CRC 读回校验
//   3. 全部暂存成功 → 原子提交 Meta（txn_id 更新为 X）—— 事务提交点
//   4. 发布：逐个 rename 暂存文件 → 正式 stepNN.bin
//   5. 清理其他事务残留的 .t* 暂存文件
//
// 掉电保证（需求文档 §48）：
//   - 第 2 步 / 第 3 步之间掉电（提交前）：
//     正式文件与 Meta 都是旧版本 → 旧 Workflow 完整可读；
//     残留 .tX 为垃圾，由下一次 save 或 recover 清理。
//   - 第 3 步之后、第 4 步完成前掉电（提交后）：
//     下次启动 workflow_storage_load_meta() 会自动调用
//     workflow_storage_recover()，识别 txn_id == X 的 .tX 并发布。
//
// 任一 Step 暂存写失败：立即返回，清理本次 .tX，
// 不提交 Meta、不触碰正式文件，旧版本保持可读。
//
// 注意：删除 Workflow 请使用 workflow_storage_delete()，
//      本函数会把 valid 置为 true。
WorkflowStorageResult workflow_storage_save(
    uint8_t workflow_id,
    const WorkflowDefinition *definition
);

// 事务恢复（掉电后启动恢复 / 测试后手动恢复）
//
// 依赖已加载的 RAM Meta（调用方需先 load_meta）。
// 对每个 valid Workflow：
//   - 发现 txn_id == Meta.txn_id 的 .tX 暂存文件 → rename 发布为正式文件
//   - 其他 .tX（事务未提交的残留）→ 删除
// 对每个 invalid Workflow：删除其全部 .t* 残留（不复活已删除的 Workflow）。
//
// 幂等：可安全重复调用。自动在 load_meta() 成功后执行。
bool workflow_storage_recover();

// 删除 Workflow
//
// 只置 meta.valid = false 并提交 meta.bin，**不删除任何 Step BIN**。
// 目的：避免大量 LittleFS 删除，未来重建可直接复用槽位。
WorkflowStorageResult workflow_storage_delete(
    uint8_t workflow_id
);


// =====================================================
// WorkflowDefinition 缓冲分配
// =====================================================
//
// WorkflowDefinition 约 8.6KB（steps[16] × ~548B），
// Arduino loopTask 默认栈仅 8KB —— 该结构【禁止在栈上声明】，
// 必须通过本分配器取缓冲（PSRAM 优先，回退内部 RAM）。
//
// ⚠️ 每次调用返回独立缓冲，调用方用完必须 free；
//   并发路径（如 MQTT 任务与 loop 任务各自保存）天然隔离。
//   分配失败返回 NULL，调用方需自行处理（打印日志并中止该次操作）。
WorkflowDefinition *workflow_storage_alloc_definition(void);

// 释放 workflow_storage_alloc_definition() 分配的缓冲（NULL 安全）
void workflow_storage_free_definition(
    WorkflowDefinition *definition
);


// =====================================================
// Step
// =====================================================

// 加载单个 Step（Lazy Load 使用）
WorkflowStorageResult workflow_storage_load_step(
    uint8_t workflow_id,
    uint8_t step_id,
    WorkflowStepDefinition *step
);

// 原子保存单个 Step
//
// ⚠️ 语义边界：
//   单 Step 写入【不代表】整个 Workflow 事务已提交 ——
//   本函数不更新 Meta、不产生事务标识。
//   真正的 Dirty 清理必须发生在整个 Workflow 保存事务
//   （workflow_storage_save()）成功之后。
//
// 适用场景：Lazy Load 修复、调试、单点写入。
// 不适用场景：作为多 Step 修改的持久化手段。
WorkflowStorageResult workflow_storage_save_step(
    uint8_t workflow_id,
    uint8_t step_id,
    const WorkflowStepDefinition *step
);


// =====================================================
// 故障注入（仅用于测试；生产代码不得调用）
// =====================================================

// 让下一次 workflow_storage_save() 的第 N 个 Step 暂存写失败。
// step < 0 或 >= 16 表示关闭（默认）。
// 触发后自动复位为关闭。
void workflow_storage_test_fail_step(int step);

// 在 workflow_storage_save() 的指定阶段模拟掉电中断：
//   phase 0 = 关闭（默认）
//   phase 1 = 预提交中断：全部暂存写完后、Meta 提交前返回
//             WF_STG_ERR_TEST_ABORTED（正式文件与 Meta 保持旧版本）
//   phase 2 = 提交后中断：Meta 已提交、发布开始前返回
//             WF_STG_ERR_TEST_ABORTED（需 recover() 或重启完成发布）
// 触发后自动复位为关闭。
void workflow_storage_test_abort_phase(int phase);

// 让指定 Workflow 的【整次】workflow_storage_save() 失败（返回
// WF_STG_ERR_WRITE_FAILED）。wf < 0 或 >= 16 表示关闭（默认）。
//
// 与 fail_step 的区别：fail_step 是"某个 Step 写失败"，
// 本接口是"某个 Workflow 保存失败"，用于验证 workflow.save 的
// 全 Dirty 语义 —— 部分 Workflow 失败时 Dirty 必须整体保留。
// 触发后自动复位为关闭。
void workflow_storage_test_fail_wf(int wf);

// 统计某 Workflow 目录下残留的 .t* 暂存文件数（测试断言用）
int workflow_storage_test_staged_count(
    uint8_t workflow_id
);
