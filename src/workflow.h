#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include "event_manager.h"
#include "command_manager.h"

// =====================================================
// 依赖说明:
//   本头文件包含 command_manager.h，以引用 CommandManager 定义的回调契约
//   （CommandTempActionCallback / WorkflowResultCallback）。
//   command_manager.h 对 WorkflowActionResult / WorkflowState 使用
//   固定底层类型前向声明，因此本文件中这两个枚举定义必须带 ": int"。
// =====================================================

//=============================
//是否启用workflow的事件响应，填0则不会注册任何事件监听
//=============================
#define WORKFLOW_EVENT_ENABLED 0   // 0=禁用, 1=启用


// =====================================================
// Workflow 自动化框架
//
// 结构:
//
// Workflow
//   |
//   |
// Step[]
//   |
//   +------ Action
//   |
//   +------ Trigger
//

// =====================================================
// 前向声明
// =====================================================

struct WorkflowTriggerDescriptor;
struct WorkflowActionDescriptor;

struct WorkflowTriggerInstance;
struct WorkflowActionInstance;
struct WorkflowStep;
struct Workflow;



#define WORKFLOW_MAX_PARAM 8
#define WORKFLOW_MAX_STEP 16
#define WORKFLOW_MAX_COUNT 16

// =====================================================
// =====================================================
// Step类型
// =====================================================
enum WorkflowStepType
{
    WORKFLOW_STEP_ACTION = 0,
    WORKFLOW_STEP_TRIGGER
};
// =====================================================
// Trigger运行状态
// =====================================================
enum WorkflowTriggerState
{
    TRIGGER_IDLE = 0,
    TRIGGER_RUNNING,
    TRIGGER_SUCCESS,
    TRIGGER_FAILED
};
// =====================================================
// Action执行结果
// =====================================================
enum WorkflowActionResult : int
{
    ACTION_IDLE = 0,      // 未开始
    ACTION_RUNNING,       // 执行中
    ACTION_SUCCESS,       // 完成
    ACTION_FAILED         // 失败
};
// =====================================================
// 参数类型
//
// 给UI使用
//
// 未来自动生成配置界面
//
// =====================================================
enum WorkflowParamType
{
    PARAM_INT,
    PARAM_FLOAT,
    PARAM_BOOL,
    PARAM_STRING
};

// =====================================================
// Workflow状态
// =====================================================
enum WorkflowState : int
{
    WORKFLOW_IDLE = 0,
    WORKFLOW_RUNNING,
    WORKFLOW_WAITING,
    WORKFLOW_FINISHED,
    WORKFLOW_TIMEOUT,
    WORKFLOW_ERROR
};

// =====================================================
// Workflow JSON状态
// =====================================================
enum WorkflowJsonState
{
    WORKFLOW_JSON_EMPTY = 0,
    WORKFLOW_JSON_LOADING,
    WORKFLOW_JSON_READY,
    WORKFLOW_JSON_ERROR
};

enum WorkflowInstanceType
{
    INSTANCE_TRIGGER = 0,
    INSTANCE_ACTION
};


//trigger callback函数
typedef void (*WorkflowTriggerCallback)(
    WorkflowTriggerInstance *trigger,
    WorkflowTriggerState state
);
//action callback 函数
typedef void (*WorkflowActionCallback)(
    WorkflowActionInstance *action,
    WorkflowActionResult result
);


// =====================================================
// 参数值
// =====================================================
struct WorkflowParamValue
{
    String name;
    WorkflowParamType type;
    int int_value;
    float float_value;
    bool bool_value;
    String string_value;
};
// =====================================================
// 参数描述
// =====================================================
struct WorkflowParam
{
    const char *name;
    WorkflowParamType type;
    const char *unit;
    const char *description;
};


// =====================================================
// Timer Runtime
// =====================================================

struct TimerRuntime
{
    time_t next_trigger_time;
    int last_trigger_minute;
    bool triggered;
    bool expired;
    int type;
};
// =====================================================
// Delay Runtime
// =====================================================

struct DelayRuntime
{
    unsigned long start_time;
    unsigned long delay_ms;
    bool started;
};
// =====================================================
// Trigger Descriptor
//
// 用户注册Trigger使用
//
// 生命周期:
// reset
// start
// poll
//
// =====================================================
struct WorkflowTriggerDescriptor
{
    const char *id;
    const char *name;
    const char *module;
    const char *description;
    WorkflowParam *params;
    uint8_t param_count;
    // 初始化运行状态
    void (*reset)
    (
        WorkflowTriggerInstance *trigger
    );
    // 启动Trigger
    //
    // 第一次进入时调用
    //
    void (*start)
    (
        WorkflowTriggerInstance *trigger
    );
    // 非阻塞轮询
    //
    // 完成后调用callback
    //
    void (*poll)
    (
        WorkflowTriggerInstance *trigger
    );
};
// =====================================================
// Action Descriptor
//
// 用户注册Action使用
//
// 生命周期:
// reset
// start
// poll
//
// =====================================================

struct WorkflowActionDescriptor
{
    const char *id;
    const char *name;
    const char *module;
    const char *description;
    WorkflowParam *params;
    uint8_t param_count;
    // 初始化运行状态
    void (*reset)
    (
        WorkflowActionInstance *action
    );
    // 启动Action
    void (*start)
    (
        WorkflowActionInstance *action
    );
    // 非阻塞轮询
    void (*poll)
    (
        WorkflowActionInstance *action
    );
};

// =====================================================
// Trigger Instance
//
// Workflow运行实例
//
// =====================================================
struct WorkflowTriggerInstance
{
    const WorkflowTriggerDescriptor *descriptor;
    String id;
    WorkflowParamValue params[WORKFLOW_MAX_PARAM];
    uint8_t param_count;
    WorkflowTriggerState state;
    // 内部runtime
    TimerRuntime timer_runtime;
    DelayRuntime delay_runtime;
    // 是否已经启动
    //
    // false:
    // 第一次start
    //
    // true:
    // poll阶段
    //
    bool running;
    WorkflowTriggerCallback callback;
};
// =====================================================
// Action Instance
//
// Workflow运行实例
//
// =====================================================
struct WorkflowActionInstance
{
    const WorkflowActionDescriptor *descriptor;
    String id;
    WorkflowParamValue params[WORKFLOW_MAX_PARAM];
    uint8_t param_count;
    WorkflowActionResult result;
    // 是否已经启动
    //
    // false:
    // 调用start()
    //
    // true:
    // 调用poll()
    //
    bool running;
    // 模块私有上下文
    //
    // 电机、阀门等使用
    //
    void *runtime;
    WorkflowActionCallback callback;
};
// =====================================================
// Step Instance
// =====================================================

union StepInstance
{
    WorkflowTriggerInstance *trigger;
    WorkflowActionInstance *action;
};

// =====================================================
// Step Definition（RAM 中的"下一次执行什么"）
// =====================================================
//
// 与 Runtime 彻底分离：
//   Definition = 下一次执行什么（本结构）
//   Runtime     = 这一次正在执行什么（WorkflowTriggerInstance /
//                 WorkflowActionInstance，workflow_start 时值拷贝快照）
//
// 存储位置：PSRAM 的 step_definitions 池，
// 刻意【不放进】WorkflowStep 内部 ——
// 每个 Step 有 8 个参数，256 个 Slot 若内联进 WorkflowStep，
// 会让常驻 DRAM 的 workflows[] 膨胀上百 KB。
//
// 只保存可持久化数据：
//   type / instance_type / Action-Trigger 字符串 ID / 参数
//
// 禁止出现：
//   函数指针 / Descriptor 指针 / runtime / callback / running / result
//
// 生命周期：
//   解析 / 后续 CRUD 修改 → 写入 Definition
//   workflow_start()      → 值拷贝到 Runtime（参数快照）
//   运行期间              → Runtime 只读自己的快照，Definition 可安全修改
// =====================================================

struct WorkflowStepDef
{
    WorkflowStepType type;
    WorkflowInstanceType instance_type;
    String id;                  // Action / Trigger 字符串 ID
    uint8_t param_count;
    WorkflowParamValue params[WORKFLOW_MAX_PARAM];
};

// 取得指定 Slot 的 Step Definition
//
// 返回 nullptr 表示池未初始化或索引越界。
//
// 用途：
//   后续 Workflow CRUD / WorkflowStorage 序列化读取 Definition。
//   Definition 修改后必须调用 workflow_mark_step_dirty()（Phase 6）。

WorkflowStepDef *workflow_step_def_at(
    uint8_t workflow_index,
    uint8_t step_index
);

// =====================================================
// Dirty Bitmap + 保存事务
// =====================================================
//
// Dirty Transaction 模型：
//   一批尚未持久化的 Workflow 修改，共享【一个】Critical Operation。
//   不是每个 Step 一个 Critical。
//
//   Clean -> Dirty（首次）  Critical +1
//   Dirty -> Dirty（后续）  不再 +1
//   Save 事务全部成功      清 Dirty + Critical -1
//   Save 任一环节失败      不清 Dirty、不 release
//
// 延迟保存：首次修改启动 5 分钟窗口，后续修改刷新窗口；
// 显式 workflow_save_transaction() 立即触发，不再等待。

// 从 Flash 加载全部 Valid Workflow（meta.bin + Step BIN）
//
// 只加载 meta.valid == true 的 Workflow，且每个 Workflow 只读 0..step_count-1。
// 返回 false 表示 Flash 中没有任何 Valid Workflow（首次启动属正常），
// 调用方应回退到 JSON 加载路径。
//
// 注意：本函数会先 workflow_clear()，会清空现有 RAM 状态。
bool workflow_load_from_storage();

// 修改 Step Definition 的单个参数（§13 / §45）
//
// 这是"运行中安全修改 Definition"的标准入口：
//   - 只改 Definition（下一次执行什么）
//   - 运行中的 Runtime 继续使用自己启动时拷贝的快照，本次运行不受影响
//   - 自动 mark_dirty，首次产生 Dirty 时 Critical +1
//
// 顺序（关键）：先 mark_dirty 成功，再改 RAM。
// 这样 Critical acquire 被拒时不会留下"改了但没标 Dirty"的不一致状态。
//
// 返回 false：索引越界 / param_index >= param_count / 修改被拒（RESTART_PENDING）
bool workflow_update_step_param(
    uint8_t workflow_index,
    uint8_t step_index,
    uint8_t param_index,
    const WorkflowParamValue &value
);

// 标记 Step Definition 已被修改（Clean -> Dirty）
//
// 返回 false 表示本次修改被拒绝：
//   索引越界，或系统已进入 RESTART_PENDING / RESTARTING（Critical acquire 失败）
bool workflow_mark_step_dirty(
    uint8_t workflow_index,
    uint8_t step_index
);

// 是否存在任何未持久化的修改
bool workflow_has_any_dirty();

// 执行完整保存事务
//
//   所有 Dirty Workflow → 逐个原子写 Step BIN → 提交 Meta
//   → 全部成功 → 清 Dirty → Critical release
//
// 任一环节失败：返回 false，Dirty 与 Critical 都保留，允许后续重试。
bool workflow_save_transaction();

// 请求延迟保存（启动 / 刷新 5 分钟窗口）
void workflow_request_save();

// 删除 Workflow
//
// 只置 meta.valid = false，【不删除任何 Step BIN】。
// 正在运行时不销毁 Runtime：当前运行继续执行，仅禁止下一次启动。
bool workflow_delete(
    uint8_t workflow_index
);

// =====================================================
// Workflow CRUD（需求文档 Phase 8 / §28）
// =====================================================
//
// 统一语义：
//   1. 全部只改 Definition（下一次执行什么），不触碰运行中的 Runtime
//   2. 顺序铁律：先 mark_dirty（含 Critical acquire）成功，再改 RAM
//   3. 修改后不立即落盘 —— 由延迟保存窗口（5 分钟）或显式
//      workflow_save_transaction() 一次性提交整批（§20 / §22）
//
// 运行中保护：
//   会改变 step_count / 重建 Step 的 API（create / set_step_count）
//   在 state == WORKFLOW_RUNNING 时拒绝 —— 否则会让在飞运行提前结束。
//   只改字段的 API（update_meta / set_step 参数）允许运行中调用（§13）。

// 把 RAM 中当前全部 Workflow 一次性写入 Flash BIN
//
// 用途：JSON → BIN 迁移。
//   BIN 只保存 Dirty Workflow，若 JSON 回退加载后只修改了一个 Workflow，
//   下次启动时 BIN 已 Valid → 只加载该 Workflow，其余从 JSON 来的会丢失。
//   故 JSON 加载成功后必须整体迁移一次，让 BIN 成为唯一数据源。
//
// 内部：全部 Workflow 标 Dirty → workflow_save_transaction()。
// 返回 false 表示保存失败（Dirty 与 Critical 保留，可稍后重试）。
bool workflow_migrate_to_storage();

// 创建一个空 Workflow（step_count = 0）
//
// 已存在同名 / 同槽位数据时整体覆盖（先清空全部 Step Definition）。
// 创建后仍处于"未提交"状态，需配合 set_step 填充 Step 再保存。
bool workflow_create(
    uint8_t workflow_index,
    const String &id,
    const String &name,
    uint32_t timeout_ms
);

// 修改 Workflow 级 Definition：id / name / enable / timeout_ms
//
// 只改 Definition。enable=false 不会终止当前运行（§13）；
// 要立即终止请用 workflow_stop() / workflow_disable()。
bool workflow_update_meta(
    uint8_t workflow_index,
    const String &id,
    const String &name,
    bool enable,
    uint32_t timeout_ms
);

// 新建或修改一个 Step 的 Definition，并同步重建其 Runtime
//
// step_index >= 当前 step_count 时自动扩展 step_count（上限 16）。
// Descriptor 由 id 现场解析（BIN 只存 ID，§44）；id 未注册时
// 与 JSON / BIN 加载路径行为一致：不分配 Instance，Step 仍计入 step_count。
bool workflow_set_step(
    uint8_t workflow_index,
    uint8_t step_index,
    WorkflowStepType type,
    WorkflowInstanceType instance_type,
    const String &id,
    const WorkflowParamValue *params,
    uint8_t param_count
);

// 设置 Step 数量
//
// 缩小：清空尾部 Step Definition（Runtime 不回收，随下次 start 重建）
// 扩大：尾部 Step 为空 Definition，需再用 set_step 填充
// 运行中拒绝（改 step_count 会让在飞运行提前结束，§13）
bool workflow_set_step_count(
    uint8_t workflow_index,
    uint8_t step_count
);

// =====================================================
// Workflow Step
// =====================================================

struct WorkflowStep
{
    WorkflowStepType type;
    WorkflowInstanceType instance_type;
    String id;
    StepInstance instance;
    // 等待异步结果
    bool waiting;
};

// =====================================================
// Workflow
// =====================================================

struct Workflow
{
    String id;
    String name;
    bool enable;
    unsigned long start_time;
    unsigned long timeout_ms;
    WorkflowState state;
    // CommandManager 关联 ID（Workflow 只保存关联，不理解命令业务）
    String cmd_id;
    // 完成回调（CommandManager 契约；完成/超时/失败时回调 cmd_id + 结果）
    WorkflowResultCallback finish_callback;
    WorkflowStep steps[WORKFLOW_MAX_STEP];
    uint8_t step_count;
    uint8_t current_step;
};
// =====================================================
// Trigger注册接口
// =====================================================
bool workflow_register_trigger(
    const WorkflowTriggerDescriptor *trigger
);
// =====================================================
// Action注册接口
// =====================================================
bool workflow_register_action(
    const WorkflowActionDescriptor *action
);
// =====================================================
// Workflow执行
// =====================================================
bool workflow_start(
    Workflow *workflow,
    bool skip_first_step = false,
    const String &cmd_id = "",
    WorkflowResultCallback callback = nullptr
);
// =====================================================
// Workflow任务
//
// loop调用
//
// 非阻塞
//
// =====================================================
void workflow_task();


// =====================================================
// 外部Action调用，Command Manager调用，根据action id创建临时实例并执行
// =====================================================

struct WorkflowActionTicket
{
    uint32_t instance_id;
    WorkflowActionResult result;
};

bool workflow_enqueue_action(
    const String &id,
    const String &payload,
    uint32_t *instance_id,
    const String &cmd_id,
    CommandTempActionCallback callback = nullptr,
    unsigned long timeout_ms = 600000
);
// 查询临时 Action 执行状态（用于 CommandManager 轮询）
bool workflow_temp_action_is_complete(
    uint32_t instance_id,
    WorkflowActionResult &result
);

// 获取等待中的临时 Action 数量
uint8_t workflow_temp_action_pending_count();

WorkflowJsonState workflow_get_json_state();
// =====================================================
// Workflow JSON处理，包含加载，保存，重载等动作
//
// 来源:
// Flash文件
// SD卡
// 云端下发
//
// =====================================================
bool workflow_load_json(const String &json);
bool workflow_load_json_file(const char *path);
bool workflow_save_json_file(const char *path);
bool workflow_reload();//Workflow重新加载
bool workflow_export_json(String &json);
//==================================================
//JSON解析结构
//=================================================

extern Workflow workflows[WORKFLOW_MAX_COUNT];

// =====================================================
// Event 响应（Workflow 内部独立实现）
// =====================================================
void workflow_event_init();
void workflow_event_callback(const EventMessage &msg);

//11. JSON解析接口
bool workflow_parse_json(
    JsonDocument &doc
);
//查询接口
uint8_t workflow_get_count();

Workflow*workflow_get(uint8_t index);
// 查询Action注册表
// Command/UI使用
uint8_t workflow_get_action_count();



void workflow_clear();

bool workflow_init();

bool workflow_enable(
    const String &id
);


bool workflow_disable(
    const String &id
);


bool workflow_stop(
    const String &id
);


bool workflow_start_by_id(const String &id, bool skip_first_step = false);

const WorkflowActionDescriptor*
workflow_get_action_descriptor(
    uint8_t index
);
//Trigger查询
uint8_t workflow_get_trigger_count();

const WorkflowTriggerDescriptor*
workflow_get_trigger_descriptor(
    uint8_t index
);
