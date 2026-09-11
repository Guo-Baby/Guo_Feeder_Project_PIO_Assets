#pragma once

#include <Arduino.h>

// =====================================================
// Capability Registry
//
// 独立能力映射层：为 Cloud Protocol / APP 提供稳定短 ID。
//
// 设计原则:
//   - 不修改现有 Action / Trigger / Workflow 注册结构
//   - 不修改任何 Descriptor 结构体
//   - 不修改 WorkflowManager / CommandManager / CloudManager
//   - 只读取 workflow.h 已公开的查询接口
//   - Stable ID 只存在于本模块 Mapping 中，不写入原 Descriptor
//
// 数据流:
//   Existing Action / Trigger / Workflow Registry
//                       |
//                       v
//              CapabilityRegistry (本模块)
//                       |
//          +------------+------------+
//          |            |            |
//          v            v            v
//     Action       Trigger      Workflow
//     Mapping      Mapping      Mapping
//          |            |            |
//          +------------+------------+
//                       |
//                RAM Runtime Cache
//                       |
//               Flash Persistence
//              (LittleFS /registry/)
//
// 初始化时机:
//   capability_registry_init() 必须在以下条件全部满足后调用:
//     1. workflow_init() 完成
//     2. 所有业务模块的 Action / Trigger 注册完成
//     3. workflow.json 加载完成 (workflow_load_json_file)
//   否则 Workflow Mapping 扫描不到已加载的 Workflow。
//
// 文件存储 (LittleFS):
//   /registry/action.bin
//   /registry/trigger.bin
//   /registry/workflow.bin
//
// 文件格式:
//   magic    : uint32_t  文件魔数（区分三种 Registry）
//   version  : uint32_t  Mapping 逻辑版本，变化时 +1（初始 1，禁止时间戳）
//   count    : uint16_t  条目数
//   checksum : uint32_t  CRC32（每个条目: stable_id + object_version + len + runtime_id）
//   entries  : count 个 { stable_id(uint8) object_version(uint32) len(uint8) runtime_id[len] }
//
// 文件格式 v2（相对 v1 的变更）：
//   - 每个条目新增 object_version(uint32)
//     Workflow = workflow.variant（内容版本，云端增量同步依据）
//     Action / Trigger 恒为 0
//   - checksum 计算早已包含 object_version，v2 才把它真正落盘
//   - 魔数整体变更 → 旧 v1 文件被判定为"不存在"，自动重建，
//     无需编写容易出错的兼容解析分支。
//
// 影响：内容变化 → variant++ → checksum 变化 → version++ →
//       云端发现不同 → 增量同步（§5 / §6）。
//
// Stable ID 规则:
//   runtime_id 按字符串升序排序，排序后的 index 即 stable_id；
//   与注册顺序无关，同一 runtime_id 的 stable_id 恒定。
//
// 写入安全:
//   先写 <file>.tmp 并 flush/close，成功后删除旧文件并 rename；
//   正式文件任何时候都是完整旧版本或完整新版本。
// =====================================================

// =====================================================
// 容量
//
// Action / Trigger 与 workflow.cpp 的注册上限保持一致(32)；
// Workflow 与 WORKFLOW_MAX_COUNT 保持一致(16)。
// Stable ID 使用 uint8_t，0~255 足够覆盖。
// =====================================================
#define CAPABILITY_MAX_ACTION     32
#define CAPABILITY_MAX_TRIGGER    32
#define CAPABILITY_MAX_WORKFLOW   16
#define CAPABILITY_MAX_ENTRY      32

// =====================================================
// LittleFS 路径
// =====================================================
#define CAPABILITY_REGISTRY_DIR       "/registry"
#define CAPABILITY_ACTION_FILE        "/registry/action.bin"
#define CAPABILITY_TRIGGER_FILE       "/registry/trigger.bin"
#define CAPABILITY_WORKFLOW_FILE      "/registry/workflow.bin"

// =====================================================
// 文件魔数（v2 格式：条目含 object_version）
//
// 'AP2A' / 'AP2T' / 'AP2W'
// 与 v1（'CAPA' / 'CAPT' / 'CAPW'）不同 → v1 文件自动失效并重建。
// =====================================================
#define CAPABILITY_MAGIC_ACTION      0x41503241UL
#define CAPABILITY_MAGIC_TRIGGER     0x41503254UL
#define CAPABILITY_MAGIC_WORKFLOW    0x41503257UL

// =====================================================
// Registry 类型
// =====================================================
enum CapabilityType
{
    CAP_ACTION = 0,
    CAP_TRIGGER,
    CAP_WORKFLOW
};

// =====================================================
// 单条映射
//
// stable_id  : Cloud Protocol / APP 使用的短 ID
// runtime_id : 设备内部原始 ID (如 VALVE_OPEN)
// =====================================================
struct CapabilityMapping
{
    uint8_t stable_id;
    String runtime_id;
    // Workflow 专用版本
    // Action / Trigger 保留0
    uint32_t object_version;
};
// =====================================================
// Flash 文件头（逻辑结构；实际序列化按固定小端字段写入）
// =====================================================
struct CapabilityRegistryHeader
{
    uint32_t magic;
    uint32_t version;
    uint16_t count;
    uint32_t checksum;
};

// =====================================================
// 单类 Registry 的 RAM Runtime Cache
// =====================================================
struct CapabilityRegistryTable
{
    uint32_t version;
    uint32_t checksum;
    uint8_t count;
    CapabilityMapping entries[CAPABILITY_MAX_ENTRY];
};

// =====================================================
// 初始化 / 重扫
//
// capability_registry_init():
//   清空 RAM Cache 后扫描当前注册表并同步 Flash。
//
// capability_registry_rescan():
//   重新扫描当前 Action / Trigger / Workflow 注册表，
//   校验 Flash Mapping；一致则复用，不一致则 version++ 重建。
//   供未来 Workflow 更新后重新生成 Mapping 使用。
// =====================================================
bool capability_registry_init();
bool capability_registry_rescan();

// 打印三类 Registry 的 version / count / checksum 与映射（调试用）
void capability_registry_dump();

// =====================================================
// 数量 / 版本 / 校验和（全部来自 RAM Cache）
// =====================================================
uint8_t capability_get_action_count();
uint32_t capability_get_action_version();
uint32_t capability_get_action_checksum();

uint8_t capability_get_trigger_count();
uint32_t capability_get_trigger_version();
uint32_t capability_get_trigger_checksum();

uint8_t capability_get_workflow_count();
uint32_t capability_get_workflow_version();
uint32_t capability_get_workflow_checksum();

// =====================================================
// 双向查询（RAM 查询，禁止频繁读 Flash）
// =====================================================
// stable_id -> runtime_id
bool capability_get_action_by_stable_id(
    uint8_t stable_id,
    String &runtime_id
);
// runtime_id -> stable_id
bool capability_get_action_stable_id(
    const String &runtime_id,
    uint8_t &stable_id
);

bool capability_get_trigger_by_stable_id(
    uint8_t stable_id,
    String &runtime_id
);
bool capability_get_trigger_stable_id(
    const String &runtime_id,
    uint8_t &stable_id
);

bool capability_get_workflow_by_stable_id(
    uint8_t stable_id,
    String &runtime_id
);
bool capability_get_workflow_stable_id(
    const String &runtime_id,
    uint8_t &stable_id
);

// 读取 Workflow 条目的 object_version（= workflow.variant）
//
// 用途：workflow.list 需要把 (stable_id, variant) 一起返回给云端
// 做增量比对。stable_id 越界 / 未初始化返回 false。
bool capability_get_workflow_object_version(
    uint8_t stable_id,
    uint32_t &object_version
);

// =====================================================
// Registry 导出（结构化文本，供 CloudManager 后续 JSON/CBOR 编码）
//
// 格式示例:
//   [ACTION]
//   version=1
//   count=2
//   checksum=12345678
//   0=VALVE_OPEN
//   1=VALVE_CLOSE
//
// CapabilityRegistry 只做数据编码，不负责 MQTT。
// =====================================================
String capability_export_action_registry();
String capability_export_trigger_registry();
String capability_export_workflow_registry();
