#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>

// =====================================================
// Config Manager
//
// 用户自定义配置的管理者。
//
// =====================================================
// 数据流（异步边界在 CommandManager → ConfigManager）
//
//   MQTT → CloudManager → CommandManager
//        → ConfigManager 命令队列（enqueue，立即返回 ACCEPTED）
//        → config_task()（每次 loop 取一个任务）
//        → JsonStorage（同步文件 IO，不再二次异步）
//        → completion callback
//        → CommandManager → CloudManager → MQTT（SUCCESS / ERROR）
//
// 需要异步的是 CommandManager → ConfigManager 这一段，
// 目的是避免命令处理过程中同步执行文件 IO 阻塞主循环。
//
// ConfigManager → JsonStorage 保持同步调用：
//   JsonStorage 只是一次同步 IO，不再建立第二层队列。
//
// ConfigManager 负责:
//   - 按模块缓存配置（RAM）
//   - 每个模块独立文件、独立版本、独立备份
//   - 原子保存 + Active/Backup 轮换
//   - 配置命令的排队、执行与结果回调
//   - 修改后触发重启倒计时
//
// ConfigManager 不负责:
//   - 判断业务参数是否合法（GPIO 是否合适、阈值是否合理由业务模块决定）
//   - 直接访问 LittleFS（一律经由 JsonStorage）
//   - 注册命令 / 解析 payload / 发送 MQTT
//
// 依赖方向: ConfigManager → JsonStorage（禁止反向）
// 本头文件不包含 command_manager.h。
// =====================================================

// =====================================================
// 配置模块名
//
// 与 LittleFS 上的文件一一对应:
//   /config/<name>.json        当前生效配置（Active）
//   /config/<name>.json.bak    上一版本配置（Backup）
// =====================================================
#define CONFIG_MODULE_WIFI      "wifi"
#define CONFIG_MODULE_OLED      "oled"
#define CONFIG_MODULE_VALVE     "valve"
#define CONFIG_MODULE_TIME      "time"
#define CONFIG_MODULE_WEIGHT    "weight"
#define CONFIG_MODULE_MQTT      "mqtt"
#define CONFIG_MODULE_MITHERMO  "mi_thermo"
#define CONFIG_MODULE_RTC       "rtc"

// 配置目录与后缀
#define CONFIG_DIR              "/config"
#define CONFIG_BAK_SUFFIX       ".bak"

// factory 副本目录
//
// 用途: 存放永不滚动的初始基线。
//
// 与 Backup 的关键区别:
//   Backup  —— 每次保存自动滚动，只能回退一步，用户不可控
//   factory —— 只在用户显式要求时更新，可随时回到初始状态
//
// 举例: 出厂值 Home → 改成 Office → 改成 Bad，
//       此时 Backup 已是 Office，Home 丢失；只有 factory 还留着 Home。
//
// 不随 uploadfs 烧录，由云端下发命令创建。
#define CONFIG_FACTORY_DIR      "/factory"

// 版本文件名（存放各模块版本号，业务 JSON 内不含版本字段）
#define CONFIG_VERSION_FILE     "/config/version.json"

// 启动成功标记文件（Boot Validation 用）
#define CONFIG_BOOT_FLAG_FILE   "/config/.bootok"

// 提交状态文件（Commit 状态保护用）
//
// 文件内存的是状态字符串，而不是用"存在 / 不存在"表达状态:
//   "PENDING" —— 提交进行中，尚未确认完成
//   "DONE"    —— 最近一次提交完整成功
//   "FAILED"  —— 最近一次提交失败（且已回滚到 Last Known Good）
//
// 之所以用内容而非存在性:
//   若用"存在 = 异常"，那么最后一步（写 DONE）失败时无法表达 ——
//   无论写入成功与否最终都只能呈现同一种状态，
//   无法区分"提交成功但标记没写成"与"提交真的失败了"。
//
// 启动时读到 PENDING 即说明上次提交中断，需从 Backup 恢复。
// 它与 Boot Flag 解决的是不同问题:
//   Boot Flag   —— 上次启动是否在初始化阶段崩溃
//   Commit Flag —— 上次配置提交是否走完全流程
#define CONFIG_COMMIT_FLAG_FILE "/config/.commit"

// =====================================================
// 配置命令队列
//
// 固定容量数组，编译期确定，运行期不分配、不释放、不使用动态链表。
// 队列满时 enqueue 返回 CONFIG_ENQUEUE_QUEUE_FULL，绝不静默丢弃命令。
// =====================================================
#define CONFIG_CMD_QUEUE_SIZE   8

// 队列任务中各字段的最大长度（含结束符）
#define CONFIG_CMD_ID_MAX       32    // 命令 ID
#define CONFIG_CMD_NAME_MAX     24    // command / object
#define CONFIG_CMD_SOURCE_MAX   16    // 来源
#define CONFIG_MODULE_NAME_MAX  16    // 模块名
#define CONFIG_KEY_NAME_MAX     32    // 字段名
#define CONFIG_VALUE_STRING_MAX 96    // 字符串型 value（入队时深拷贝至此）

// =====================================================
// 修改配置后的自动重启阈值
//
// 云端 / UI 修改配置后需要重启才能生效。
// 用户可能连续修改多个字段，因此不在每次修改时立即重启，
// 而是给出 5 分钟窗口：用户可在此期间自行选择重启时机，
// 超时则由设备本地自动重启。
// =====================================================
#define CONFIG_RESTART_TIMEOUT_MS   (5UL * 60UL * 1000UL)

// 执行重启命令后的安全延迟
//
// completion callback 把 SUCCESS 交还上层之后不立即重启，
// 而是进入安全等待窗口，给系统留出完成收尾的时间:
//   配置持久化 → ACK / UP 消息 → MQTT / 网络层处理 → 其他收尾 → Restart
//
// 10 秒属于安全等待窗口，不是业务逻辑中的正常运行延迟。
#define CONFIG_RESTART_SAFE_DELAY_MS 10000UL

// =====================================================
// 模块缓存结构
// =====================================================
// ConfigManager 维护"当前模块缓存"，每个模块一份。
//
// 采用缓存结构而非单一大文档，是为了支持按需释放：
//   未来接入 workflow / 大 JSON / 用户自定义数据后，
//   不会让所有配置同时常驻 RAM。
//
// 释放后 loaded=false；再次访问时该模块的 getter 走默认值兜底，
// 与"配置缺失"行为一致，不会引入空指针风险。
struct ConfigModule
{
    const char *name;     // 模块名，对应文件名
    JsonDocument data;    // 模块配置数据（扁平结构，不含模块名根）
    bool loaded;          // 是否已加载到 RAM
    bool dirty;           // 是否存在未保存的修改
    uint8_t version;      // 模块配置版本（0~255 循环，仅用于判断新旧）
};

// =====================================================
// 配置命令类型
// =====================================================
enum ConfigCommandType
{
    CONFIG_CMD_NONE = 0,
    CONFIG_CMD_QUERY_FIELD,     // 查询单个字段
    CONFIG_CMD_QUERY_MODULE,    // 查询整个模块
    CONFIG_CMD_QUERY_ALL,       // 查询全部模块
    CONFIG_CMD_SET_FIELD,       // 修改 / 新增单个字段
    CONFIG_CMD_SAVE,            // 把 dirty 配置写入 Flash（幂等：无 dirty 也算成功）
    CONFIG_CMD_RESTART,         // 重启；cancel=true 表示取消待重启
    CONFIG_CMD_DELETE_FIELD,    // 删除单个字段（真正从配置中移除）
    CONFIG_CMD_SET_MODULE,      // 批量写入模块字段（严格校验：字段必须已存在）
    CONFIG_CMD_RESET_MODULE,    // 从 factory 副本恢复模块
    CONFIG_CMD_BACKUP_MODULE    // 把当前模块备份为 factory 副本
};

// =====================================================
// 命令执行错误码（回调中返回）
// =====================================================
enum ConfigCommandError
{
    CONFIG_ERR_NONE = 0,
    CONFIG_ERR_INVALID_PARAM,      // 参数非法
    CONFIG_ERR_UNAVAILABLE,        // 存储不可用
    CONFIG_ERR_MODULE_NOT_FOUND,   // 模块不存在
    CONFIG_ERR_KEY_NOT_FOUND,      // 字段不存在
    CONFIG_ERR_VALUE_REJECTED,     // value 类型不被接受
    CONFIG_ERR_IO_FAILED,          // 文件 IO 失败
    CONFIG_ERR_VERSION_MISMATCH,   // 版本号不匹配（乐观锁拦截）
    CONFIG_ERR_FACTORY_MISSING     // factory 副本不存在
};

// =====================================================
// 提交状态
//
// 表示"最近一次完整提交是否完成"。
//
// 一次完整提交 =
//     Config Data 写入成功
//   + Version 写入成功
//   + 提交收尾成功
//
// 三者缺一不可；任一步失败都必须回滚到 Last Known Good Config。
// =====================================================
enum ConfigCommitState
{
    CONFIG_COMMIT_NONE = 0,   // 尚未发生任何提交
    CONFIG_COMMIT_DONE,       // 最近一次提交完整成功
    CONFIG_COMMIT_PENDING,    // 提交进行中，尚未确认完成
    CONFIG_COMMIT_FAILED      // 提交失败并已回滚
};

// 查询最近一次提交状态
ConfigCommitState config_commit_state();

// =====================================================
// 入队结果
//
// 这是 enqueue 的即时返回值，表示"命令有没有被收下"，
// 与命令最终执行成功与否无关。
// =====================================================
enum ConfigEnqueueResult
{
    CONFIG_ENQUEUE_ACCEPTED = 0,   // 已排队，等待 config_task 执行
    CONFIG_ENQUEUE_INVALID,        // 参数非法（含 value 类型被拒绝）
    CONFIG_ENQUEUE_QUEUE_FULL,     // 队列满，未收下
    CONFIG_ENQUEUE_UNAVAILABLE     // ConfigManager 尚未就绪
};

// =====================================================
// 命令完成回调
//
// 在 config_task() 中、文件 IO 完成后调用。
// CommandManager 应在此回调内组装响应并上报 —— 本模块不碰 MQTT。
//
// 参数:
//   cmd_id / command / object / source —— 原样回传，便于上层关联命令
//   type     —— 命令类型
//   success  —— 最终执行结果（true = SUCCESS，false = ERROR）
//   error    —— 失败原因；success 为 true 时恒为 CONFIG_ERR_NONE
//   result   —— 结果数据（查询类命令的有效载荷）
//   message  —— 可选说明文字（可能为 ""）
//
// 生命周期（重要）:
//   result 与 message 仅在回调执行期间有效，回调返回后即失效。
//   上层若需留存，必须把数据复制进自己的文档。
//
// 复制方式（ArduinoJson 7 深拷贝语义，已逐层核对源码）:
//     my_doc["data"] = result.as<JsonVariantConst>();
//
//   内部路径:
//     Converter<JsonVariantConst>::toJson
//       → copyVariant()
//       → VariantData::setString()
//       → resources->saveString()
//   即把字符串内容拷进"目标文档自己的内存池"，
//   不会与 ConfigManager 的内部文档共享缓冲区，
//   因此 ConfigManager 随后 clear / reload 都不影响已复制出去的数据。
//
//   唯一的例外是 isStatic() 分支（编译期字符串字面量）只存指针，
//   但那指向 Flash 常量，同样不会失效。
//
//   严禁把 result 的引用或指针保存到回调之外使用。
// =====================================================
typedef void (*ConfigCommandCompletionCallback)(
    const char *cmd_id,
    const char *command,
    const char *object,
    const char *source,
    ConfigCommandType type,
    bool success,
    ConfigCommandError error,
    const JsonDocument &result,
    const char *message
);

// 注册完成回调（由 CommandManager 初始化时调用一次）
void config_set_completion_callback(
    ConfigCommandCompletionCallback callback
);

// =====================================================
// 生命周期
// =====================================================

// 初始化：初始化 JsonStorage、检查启动标记、按模块加载配置
//
// 加载策略:
//   1. 读取 /config/<name>.json（Active）
//   2. Active 读取或解析失败 → 回退 /config/<name>.json.bak（Backup）
//   3. 上一次启动未成功（无启动标记）→ 先将 Backup 恢复为 Active
//
// 返回:
//   true  = JsonStorage 可用且至少一个模块加载成功
//   false = 文件系统不可用（此时所有 getter 走默认值）
bool config_init();

// 保存所有 dirty 模块
//
// 单个模块的保存顺序:
//   1. 原子写入临时文件
//   2. 旧 Active 轮转为 Backup
//   3. 临时文件成为新 Active
//   4. 最后更新 version.json
//
// 版本文件最后更新：即使版本写入失败，配置本身已经生效，
// 不一致留待下次启动时通过文件扫描与启动标记判定是否回滚。
bool config_save();

// 主任务（loop 调用）
//
// 职责:
//   1. 检查配置修改后的自动重启倒计时
//   2. 从命令队列取出**一个**任务并执行（同步调用 JsonStorage）
//   3. 执行完成后调用完成回调，把 SUCCESS / ERROR 交还上层
//
// 非阻塞:
//   每次调用最多执行一个配置命令，
//   即使队列积压也不会在一个 loop 内连续做多次文件 IO。
//
// 注意:
//   本函数是命令队列的唯一消费者，不调用则队列永不被处理。
void config_task();

// 启动成功确认（setup 末尾调用）
//
// 只有完整走完 setup 才会写入启动标记。
// 若配置错误导致 panic / 反复重启，标记不会被写入，
// 下次启动即判定上一次启动失败并触发 Backup 恢复。
bool config_boot_validate();

// 重新加载所有模块（丢弃未保存的 RAM 修改）
bool config_reload();

// 日志回调（可选，未注册则静默）
typedef void (*ConfigLogCallback)(const char *level, const char *message);
void config_set_log_callback(ConfigLogCallback callback);

// =====================================================
// 通用访问接口
//
// 采用 module + key 两级参数，不做字符串路径解析
// （如 "wifi/ssid" 这类 path 语法在此模块中不存在）。
// 调用方明确知道自己要访问哪个模块的哪个字段。
// =====================================================

bool config_get_int(const char *module, const char *key, int &value);
bool config_get_bool(const char *module, const char *key, bool &value);
bool config_get_float(const char *module, const char *key, float &value);
bool config_get_long(const char *module, const char *key, long &value);

// 字符串读取：输出到调用方提供的缓冲区，保证 '\0' 结尾
bool config_get_string(
    const char *module,
    const char *key,
    char *buffer,
    size_t size
);

bool config_set_int(const char *module, const char *key, int value);
bool config_set_bool(const char *module, const char *key, bool value);
bool config_set_float(const char *module, const char *key, float value);
bool config_set_long(const char *module, const char *key, long value);
bool config_set_string(const char *module, const char *key, const char *value);

// =====================================================
// 整体获取某个模块
//
// 采用"拷贝到调用方提供的 Document"，而不是返回 JsonVariantConst。
//
// 原因:
//   JsonVariant / JsonObject 的引用生命周期依赖内部 JsonDocument。
//   若调用方持有引用后又发生 config_reload()，该引用即失效。
//   深拷贝输出可彻底避免悬空引用。
//
// 返回:
//   true  = 模块存在且已加载，output 为模块完整内容的副本
//   false = 模块不存在或未加载
bool config_get_json(const char *module, JsonDocument &output);

// 获取模块版本号
uint8_t config_get_version(const char *module);

// 释放模块缓存（未来按需加载 / 大文件场景使用）
bool config_release_module(const char *module);

// =====================================================
// 异步配置命令接口
//
// CommandManager 收到配置命令后只做 enqueue，
// 不得同步调用任何查询 / 保存 / JsonStorage 接口。
//
// enqueue 只完成:
//   参数检查 → 复制任务参数（含 value 深拷贝）→ 入队 → 立即返回
//
// 真正执行发生在 config_task()，完成后经完成回调交还结果。
// 因此一个命令有两个阶段:
//   1) enqueue 立即返回 ACCEPTED（已排队，尚未执行）
//   2) config_task() 执行后回调 SUCCESS 或 ERROR
//
// ACCEPTED 与 SUCCESS 必须严格区分：ACCEPTED 不代表执行成功。
// =====================================================

// 统一入队接口
//
// 不适用的参数传 nullptr / JsonVariantConst() 即可:
//   QUERY_ALL / SAVE        → module / key / value 均不需要
//   QUERY_MODULE            → 需要 module
//   QUERY_FIELD / SET_FIELD → 需要 module + key
//   SET_FIELD               → 需要 value（仅标量）
//   RESTART                 → 用 cancel 区分"取消"与"立即重启"
//
// value 在入队时即被深拷贝到任务槽位，
// 任务不持有调用方 JsonDocument 的任何引用。
ConfigEnqueueResult config_cmd_enqueue(
    ConfigCommandType type,
    const char *cmd_id,
    const char *command,
    const char *object,
    const char *source,
    const char *module,
    const char *key,
    JsonVariantConst value,
    bool cancel
);

// ---- 便捷封装（内部转调 config_cmd_enqueue）----

// 查询单个字段
ConfigEnqueueResult config_cmd_enqueue_query_field(
    const char *cmd_id,
    const char *command,
    const char *object,
    const char *source,
    const char *module,
    const char *key
);

// 查询整个模块
// keys_only = true 时只返回字段名，不返回字段值（降低报文长度）
ConfigEnqueueResult config_cmd_enqueue_query_module(
    const char *cmd_id,
    const char *command,
    const char *object,
    const char *source,
    const char *module,
    bool keys_only
);

// 查询全部模块
// keys_only = true 时只返回模块名列表
ConfigEnqueueResult config_cmd_enqueue_query_all(
    const char *cmd_id,
    const char *command,
    const char *object,
    const char *source,
    bool keys_only
);

// 修改 / 新增单个字段。
//
// value 只接受标量: int / bool / float / 字符串。
// null / object / array 在入队阶段即被拒绝并返回
// CONFIG_ENQUEUE_INVALID —— 不会拖到执行阶段才失败。
//
// 字段不存在则新增，存在则覆盖；不要求字段预先存在。
// 本模块不判断业务取值是否合法（GPIO 范围、端口范围等由上层决定）。
ConfigEnqueueResult config_cmd_enqueue_set_field(
    const char *cmd_id,
    const char *command,
    const char *object,
    const char *source,
    const char *module,
    const char *key,
    JsonVariantConst value,
    int expect_version
);

// 保存所有 dirty 模块（真正的 Flash 写入发生在执行阶段）
ConfigEnqueueResult config_cmd_enqueue_save(
    const char *cmd_id,
    const char *command,
    const char *object,
    const char *source
);

// 重启: cancel = false 立即重启；cancel = true 取消待重启
ConfigEnqueueResult config_cmd_enqueue_restart(
    const char *cmd_id,
    const char *command,
    const char *object,
    const char *source,
    bool cancel
);

// 删除单个字段
//
// 字段不存在时返回 CONFIG_ERR_KEY_NOT_FOUND。
// 删除后需要重启生效，与 config_set 一致（自动进入重启倒计时）。
ConfigEnqueueResult config_cmd_enqueue_delete_field(
    const char *cmd_id,
    const char *command,
    const char *object,
    const char *source,
    const char *module,
    const char *key,
    int expect_version
);

// 批量写入模块字段
//
// value 必须是 object。执行阶段做严格校验:
//   传入的每个字段都必须在目标模块中已存在，
//   任一字段不存在则整体拒绝，不写入任何内容。
//
// value 可能超过内部定长缓冲，此时会在 PSRAM 中暂存，
// 任务执行完立刻释放，不长期占用。
ConfigEnqueueResult config_cmd_enqueue_set_module(
    const char *cmd_id,
    const char *command,
    const char *object,
    const char *source,
    const char *module,
    JsonVariantConst value,
    int expect_version
);

// 从 factory 副本恢复模块
//
// factory 文件不存在时返回 CONFIG_ERR_FACTORY_MISSING。
ConfigEnqueueResult config_cmd_enqueue_reset_module(
    const char *cmd_id,
    const char *command,
    const char *object,
    const char *source,
    const char *module
);

// 把当前模块备份为 factory 副本（覆盖原有副本）
ConfigEnqueueResult config_cmd_enqueue_backup_module(
    const char *cmd_id,
    const char *command,
    const char *object,
    const char *source,
    const char *module
);

// ---- 队列状态 ----

// 队列总容量（固定值 CONFIG_CMD_QUEUE_SIZE）
size_t config_cmd_queue_capacity();

// 当前排队等待执行的任务数
size_t config_cmd_queue_pending();

// =====================================================
// 重启管理
//
// 配置修改需要重启生效。为避免每改一个字段就重启，
// 采用"倒计时窗口"：UI 可在窗口内择机重启，超时则本地自动重启。
// =====================================================

// 查询是否处于重启倒计时窗口
// remain_ms 输出剩余毫秒数；不在窗口内返回 false
bool config_restart_pending(unsigned long &remain_ms);

// 立即重启（UI 主动触发，等同用户选择了重启时机）
bool config_restart_now();

// 取消待重启（设备将在当前配置下继续运行，改动仍会在本次重启后生效）
void config_cancel_restart();

// =====================================================
// 以下为现有业务接口
//
// 名称与语义保持不变，业务模块（WiFi / OLED / Valve / Time /
// Weight / MQTT / MiThermometer）无需任何修改。
// 内部实现已迁移到新的模块化缓存 + JsonStorage 架构。
// =====================================================

// WiFi 配置
String config_get_wifi_ssid();
String config_get_wifi_password();
int config_get_wifi_connect_timeout();
int config_get_wifi_reconnect_interval();

// OLED 配置
int config_get_oled_sda();      // sda 对应的 gpio 引脚
int config_get_oled_scl();      // scl 对应的 gpio 引脚
int config_get_oled_speed();    // i2c 总线通信频率，单位 Hz
int config_get_oled_frame_delay(); // 帧刷新延迟，配合动画展示功能使用
int config_get_oled_rotation(); // 屏幕旋转角度，0~3 对应 0/90/180/270 度

// =============================
// Valve 阀门配置读取
// =============================
int config_get_valve_gpio_pin();
int config_get_valve_active_level();     // 控制 GPIO 输出电平的逻辑极性。1=高电平有效，GPIO 输出 HIGH 时阀门打开，LOW 时关闭；0=低电平有效，GPIO 输出 LOW 时阀门打开，HIGH 时关闭
int config_get_valve_open_duration_ms(); // 阀门打开持续时间，0=持续打开
bool config_get_valve_enable();          // 是否启用阀门模块
int config_get_valve_safety_timeout_sec(); // 阀门最大开启时间，默认 300s，合法值 10~300，如需修改上下限去 valve.cpp 修改

// ==========================
// 时间配置
// ==========================
int config_get_timezone();
String config_get_ntp_server1();
String config_get_ntp_server2();
int config_get_ntp_sync_interval_sec(); // SNTP 校时周期，单位秒，默认 86400（24 小时）

// ==========================
// HX711 称重模块配置
// ==========================
int config_get_weight_dt();               // HX711 DT 数据引脚
int config_get_weight_sck();              // HX711 SCK 时钟引脚
int config_get_weight_sample_interval();  // HX711 采样间隔，单位 ms
float config_get_weight_scale();          // 比例因子
long config_get_weight_zero_offset();     // 零点偏移量
int config_get_weight_filter_samples();   // 滤波采样数

// 设置 HX711 零点（称重校准完成后调用）
//
// 注意: 本函数只更新 RAM 并标记 dirty，
// 调用方需随后调用 config_save() 才会落盘。
bool config_set_weight_zero_offset(long offset);

// ==========================
// Bemfa_Cloud MQTT 配置
// ==========================
String config_get_mqtt_server();
int config_get_mqtt_port();
String config_get_mqtt_client_id();
String config_get_mqtt_subscribe_topic();
String config_get_mqtt_username();
String config_get_mqtt_password();
String config_get_mqtt_publish_topic();
String config_get_mqtt_ca_path();
unsigned long config_get_mqtt_retry_interval(); // MQTT 重连间隔(ms)
int config_get_mqtt_retry_max();                // 最大快速重试次数
unsigned long config_get_mqtt_sleep_interval(); // 进入休眠重试间隔(ms)
int config_get_mqtt_keep_alive();               // MQTT keep alive 秒

// ==========================
// WiFi 配置更新
// ==========================
// 更新 WiFi SSID / 密码。
// 成功后标记 dirty 并启动重启倒计时，调用方需调用 config_save() 落盘。
bool config_update_wifi(
    const String &ssid,
    const String &password
);

// ==========================
// PCF8563T RTC 配置（rtc.json）
//
// 只保存"芯片接口"本身，不涉及任何时间语义。
// 时间与校时策略仍归 time 模块。
//
// 与 OLED 共用同一组 I2C Bus（SDA/SCL 配置值须与 oled 一致），
// 由不同 I2C Address 区分设备，不额外占用 GPIO。
// ==========================
bool config_get_rtc_enable();         // 是否启用 RTC 芯片
int config_get_rtc_sda();             // RTC 所在 I2C Bus 的 SDA 引脚
int config_get_rtc_scl();             // RTC 所在 I2C Bus 的 SCL 引脚
int config_get_rtc_i2c_addr();        // PCF8563T 7bit I2C 地址，默认 0x51(81)
int config_get_rtc_calibrate_threshold_sec(); // System 与 RTC 差值超过该秒数才写 RTC，默认 2

// ==========================
// 米家 LYWSD03MMC 温湿度计配置
// ==========================

/**
 * @brief 读取 BLE bindkey，输出到调用方提供的 buf
 * @param buf 调用方提供 char 缓冲区
 * @param buf_size 缓冲区字节数，建议传入 33
 */
void config_get_mithermometer_blekey(char *buf, size_t buf_size);

/**
 * @brief 读取温度计 MAC 地址，输出到调用方提供的 buf
 * @param buf 调用方提供 char 缓冲区
 * @param buf_size 缓冲区字节数，建议传入 24
 */
void config_get_mithermometer_mac(char *buf, size_t buf_size);

bool config_get_mi_thermo_allow_collect();
