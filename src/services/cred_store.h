#pragma once
#include <Arduino.h>

// =====================================================
// CredStore —— 凭据的 NVS 持久化（Phase D-2）
// =====================================================
// 职责（**只做这些**）：
//   1. NVS namespace `gfcred` 的读写
//   2. 双 slot（slot0 / slot1）的原始存取
//   3. "哪个 slot 是当前生效" 与 "最近一次已接受的 generation" 的持久化
//
// 明确**不做**：
//   - 不理解 credential_set 协议 / 不解析 JSON（JSON→结构由 CommandManager 负责）
//   - 不做状态机、不做 promote / rollback 决策（属 CredentialManager）
//   - 不连接 MQTT、不打印凭据内容
//
// ★ 与身份隔离（冻结：Phase C Q12 / P0 C-6）：
//   namespace 必须是 `gfcred`，**不得**与身份 `gfid` 混存 —— 身份不可变、凭据可变可轮换。
// =====================================================

/** NVS namespace（冻结：P1 Architecture Freeze #2） */
#define CRED_NVS_NAMESPACE "gfcred"

/** 槽位状态（**设备本地概念**；云端 DB 永不写 state='TESTING'，两端同名不同物） */
enum CredSlotState : uint8_t
{
    CRED_SLOT_INVALID = 0,   // 空 / 已废弃
    CRED_SLOT_TESTING = 1,   // 已写入，等待试连验证
    CRED_SLOT_ACTIVE  = 2    // 当前生效
};

/** 缓冲区上界（username: `dev_` + 12 hex = 16；password: 32；留余量） */
#define CRED_USERNAME_MAX 40
#define CRED_PASSWORD_MAX 48

/** 槽位数（双 slot，冻结） */
#define CRED_SLOT_COUNT 2

struct CredSlot
{
    bool     present;                       // 该 slot 是否已写入
    uint8_t  state;                         // CredSlotState
    uint32_t generation;                    // 该槽凭据的世代
    char     username[CRED_USERNAME_MAX];
    char     password[CRED_PASSWORD_MAX];
};

/** 初始化：打开 NVS namespace（幂等；失败不阻塞启动） */
void cred_store_init();

/** 存储是否可用（NVS 打开成功） */
bool cred_store_ready();

/** 读取某 slot；返回 false 表示未写入/读取失败（out 被清零） */
bool cred_store_read(uint8_t slot, CredSlot& out);

/** 写入某 slot（state 由调用方指定） */
bool cred_store_write(uint8_t slot, const CredSlot& in);

/** 擦除某 slot */
bool cred_store_erase(uint8_t slot);

/** 当前生效 slot；-1 = 无（调用方应回落配置兜底） */
int cred_store_active_slot();

/** 设置当前生效 slot；-1 = 清除 */
bool cred_store_set_active_slot(int slot);

/** 最近一次**已被接受**（promote 成功）的 generation；用于拒绝陈旧/重放下发 */
bool cred_store_get_last_generation(uint32_t& gen);

/** 记录最近一次已被接受的 generation */
bool cred_store_set_last_generation(uint32_t gen);

/** 是否已存在任何有效凭据（用于启动时选择"凭据优先/配置兜底"） */
bool cred_store_has_active();
