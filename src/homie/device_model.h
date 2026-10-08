#ifndef HOMIE_DEVICE_MODEL_H
#define HOMIE_DEVICE_MODEL_H

// =====================================================
// Device Model —— Guo Feeder 设备对外语义的「唯一权威、协议无关」描述
//
// 上位冻结设计（唯一依据）：
//   docs/architecture/P2-0-architecture-review-and-design-freeze.md
//   §4（Device Model ↔ Capability Registry 关系）· §5（Device Model Definition）
//   §12（Node / Property Semantics）· §13（Datatype / Validation）
//   （P2-0.1 Freeze Corrections 后版本）
//
// 定位（FROZEN §5.1 / §5.2）：
//   · 描述「这台设备有哪些可读状态、哪些可写能力、各自 datatype 与取值域」
//   · 与 Capability Registry 是「兄弟模块」：互不 include、非父子、非 1:1
//     （Registry = runtime capability availability，本模型 = 静态语义）
//   · 无状态：只有 const 表 + 纯函数；不产生 System State、不注册 Event、
//     不持有句柄、不保留任何运行期可变状态（AD-2a）
//   · 协议无关：不认识 Homie / MQTT
//
// 明确不负责（越界即违规，FROZEN §5.2）：
//   MQTT transport · hardware control · business workflow · persistence
//   · cloud authentication · 任何运行期可变状态
//
// ★ Device Model 可以「描述」Command/Action 映射（const 表 + 纯查表），
//   但绝不「调用」CommandManager / Action / 硬件。
//
// 依赖边界（FROZEN §6 / §5.3）：仅 <stdint.h> 与 services/system_state.h 的
//   key 枚举。禁止 include mqtt_client / cloud_manager / mqtt_transport /
//   homie_bridge / command_manager / event_manager / workflow* / app/* / GPIO。
// =====================================================

#include <stdint.h>
#include "services/system_state.h"   // 仅取 SystemStateKey 枚举（FROZEN §5.3）

// =====================================================
// datatype（FROZEN §5.3 / §13.1）
// =====================================================
enum DeviceModelDatatype : uint8_t
{
    DM_BOOL = 0,
    DM_INT,
    DM_FLOAT,
    DM_STRING,
    DM_ENUM,
    DM_JSON,     // P2 仅定义类型，不使用（FROZEN §13.1）
};

// =====================================================
// 只读面来源（FROZEN §5.3）
// =====================================================
enum DeviceModelSourceKind : uint8_t
{
    DM_SRC_NONE = 0,   // 无只读来源（命令型属性）
    DM_SRC_STATE,      // 只读值来源于 System State
};

// =====================================================
// 可写面：一个属性取值 → 一个 Action 调用（FROZEN §5.3）
//
// ★ 显式映射表；禁止根据字符串自动拼接 / 推导 Action ID（§12.5）
// ★ action_id 必须是已注册 Action 的 runtime_id（与 Capability Registry
//   共用同一批 runtime_id，但 Device Model 不依赖 Registry）
// =====================================================
struct DeviceModelCommand
{
    const char *value;         // 该属性的取值（enum 成员；或 "true"）
    const char *action_id;     // 已注册 Action 的 runtime_id（如 "VALVE_OPEN"）
    const char *payload_json;  // 传给 Action 的 payload（无参 = "{}"）
};

// =====================================================
// 只读枚举映射：System State 的布尔 / 整数值 → Homie enum 成员串
// （FROZEN §5.3 / §12.5；显式，禁止从 runtime_id 字符串推导）
// =====================================================
struct DeviceModelEnumMap
{
    int32_t     state_value;   // System State 读到的数值（bool 用 1 / 0）
    const char *member;        // 对应的枚举成员串（如 "open"）
};

// =====================================================
// Property（FROZEN §5.3，字段顺序即冻结契约）
// =====================================================
struct DeviceModelProperty
{
    const char *node;          // node id
    const char *id;            // property id（[a-z0-9-]）
    const char *name;          // 展示名（如 "电磁阀"）
    DeviceModelDatatype dtype;
    const char *format;        // enum:"open,close"  num:"0:300:1"  其余 ""
    const char *unit;          // 可为 ""
    bool        settable;
    bool        retained;      // 只读状态属性默认 true
    float       deadband;      // 数值死区（0 = 精确比较）
    uint16_t    min_pub_ms;    // 最小发布间隔（0 = 用全局默认）
    // ---- 只读面 ----
    DeviceModelSourceKind src_kind;
    uint16_t    state_key;     // SystemStateKey（DM_SRC_STATE 时有效）
    const DeviceModelEnumMap *enum_state_map;   // 状态值→枚举成员；空 = nullptr
    uint8_t     enum_state_map_count;
    // ---- 可写面 ----
    const DeviceModelCommand *cmds;
    uint8_t     cmd_count;
};

// =====================================================
// Node（FROZEN §5.3）
// =====================================================
struct DeviceModelNode
{
    const char *id;
    const char *name;
    const char *type;
};

// =====================================================
// 协议无关语义校验结果（FROZEN §13.2 第①层「形状校验」）
//
// ★ 只做「形状」校验，不复制业务语义校验（业务参数合法性属后续
//   CommandManager / 业务层；Device Model 不掺入执行逻辑）
// =====================================================
enum DeviceModelValidateResult : uint8_t
{
    DM_VALID_OK = 0,
    DM_VALID_UNKNOWN_PROPERTY,   // 未知 / 空 property
    DM_VALID_NOT_SETTABLE,       // 只读属性，禁止写
    DM_VALID_BAD_DATATYPE,       // 值无法按 dtype 解析（或 dtype 不支持写）
    DM_VALID_ENUM_NOT_FOUND,     // enum 成员不在 format 表内
    DM_VALID_OUT_OF_RANGE,       // 数值超出 format 范围
};

// =====================================================
// 访问器（纯函数；无状态）
//
// 签名与 P2-0 冻结设计一致（§5.3）。实现为「薄包装 → 内部 constexpr 核心」：
// 核心使同一份逻辑既可在运行期调用（P2-3/P2-4），又可在编译期被
// device_model.cpp 的静态测试套件求值（单一真源，无重复实现）。
// =====================================================
uint8_t                    dm_node_count();
const DeviceModelNode*     dm_node_at(uint8_t index);

uint8_t                    dm_property_count();
const DeviceModelProperty* dm_property_at(uint8_t index);

// 按 node id + property id 查找；未找到返回 nullptr
const DeviceModelProperty* dm_find(const char* node_id, const char* property_id);

// =====================================================
// 协议无关基础语义校验（FROZEN §13.2 / 任务 §十）
// =====================================================
DeviceModelValidateResult dm_validate_set(const DeviceModelProperty* prop,
                                          const char* value);

// =====================================================
// 可写面解析（FROZEN §5.3 / §12.5）
//
// 仅「查表」得到 { action_id, payload_json }；不调用 CommandManager。
// 成功条件：① 通过 dm_validate_set（形状合法 + settable）
//           ② 属性存在显式命令映射
// 失败（含 readonly / 未知 enum / 无映射）返回 false 且 *out_cmd = nullptr。
// ★ 客户端永远无法通过本接口提供或覆盖 action_id。
// =====================================================
bool dm_resolve_command(const DeviceModelProperty* prop,
                        const char* value,
                        const DeviceModelCommand** out_cmd);

#endif // HOMIE_DEVICE_MODEL_H
