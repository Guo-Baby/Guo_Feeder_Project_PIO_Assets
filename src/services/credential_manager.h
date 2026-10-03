#pragma once
#include <Arduino.h>

#include "services/cred_store.h"

// =====================================================
// CredentialManager —— 凭据生命周期（Phase D-2）
// =====================================================
// ★ 冻结（Phase C Q9）：本模块**独占**以下职责
//     1. 处理 `system.credential_set`
//     2. `gfcred` 双 slot 的选择与写入
//     3. `TESTING` / `ACTIVE` 状态迁移
//     4. **promote**（验证通过 → 生效）
//     5. **rollback**（失败/超时 → 丢弃候选，保留原生效凭据）
//
// ★ CloudManager 只允许做两件事：
//     A. 取"当前要用的 MQTT 身份"（`credential_manager_get_identity()`）
//     B. 回报连接结果（`credential_manager_notify_conn_result()`）
//   **不得感知** `ACTIVE` / `TESTING` / `generation` / `phase` / `state`。
//   `client_id_template` 对它只是**不透明字符串**（不理解 `_t<gen>` 的含义）。
//
// ★ 本模块**不解析 JSON**（架构铁律：内部模块不处理 JSON）：
//   `payload` 由 CommandManager 解析为 `CredentialSetRequest` 后传入。
// =====================================================

/** 云端下发对象名（冻结：Phase C Q3） */
#define CRED_OBJECT_CREDENTIAL_SET  "credential_set"

/** action 取值 */
#define CRED_ACTION_INSTALL         "install"

/** 设备侧试连窗口（云端 confirm timeout = 300 s 是**云端**口径，与此不同） */
#define CRED_TEST_TIMEOUT_MS        45000UL

/** 网络类失败的最大尝试次数（超过即 rollback；认证类失败**立即** rollback） */
#define CRED_TEST_MAX_ATTEMPTS      5

/** 安装结果错误码（内部；CommandManager 负责映射为命令错误码） */
enum CredErr : uint8_t
{
    CRED_ERR_NONE = 0,
    CRED_ERR_INVALID_PAYLOAD,       // 载荷非法 / 缺字段
    CRED_ERR_STORE_FAIL,            // NVS 写入失败
    CRED_ERR_GENERATION_STALE,      // generation 非递增（防重放）
    CRED_ERR_BUSY                   // 已有安装在进行中
};

/** 凭据下发请求（C++ 结构，JSON 解析在 CommandManager） */
struct CredentialSetRequest
{
    char     username[CRED_USERNAME_MAX];
    char     password[CRED_PASSWORD_MAX];
    uint32_t generation;
    uint8_t  slot;                  // 云端建议槽位（0/1）；越界/占用时本机自行选择
    char     action[16];
};

/** 待上报事件（由 CommandManager 的 task 取出，作为命令结果上报） */
struct CredentialEvent
{
    bool     pending;
    bool     confirmed;             // ★ 仅 true 才允许云端 PROVISIONING → ACTIVE（Q10）
    bool     ok;
    uint32_t generation;
    char     cmd_id[40];
    char     reason[24];
};

/** 期望的 MQTT 身份（CloudManager 视为**不透明值**） */
struct CredentialIdentity
{
    bool        valid;
    const char* username;
    const char* password;
    const char* client_id_template;  // 官方模板，或 官方模板 + "_t<gen>"（试连专用，方案 B）
    uint32_t    epoch;               // 变化 ⇒ 应立即换连（语义对 CloudManager 不可见）
};

/** 初始化：读 NVS，确定"当前生效槽位" */
void credential_manager_init();

/** 主循环任务（非阻塞）：推进试连超时与 promote/rollback */
void credential_manager_task();

/** 处理 credential_set（**调用前必须已完成来源门控**，见 Q13） */
bool credential_manager_install(const CredentialSetRequest& req, uint8_t* err_out);

/** 安装成功后由 CommandManager 补写 cmd_id（供 confirm 帧的 `i` 关联） */
void credential_manager_set_pending_cmd_id(const char* cmd_id);

/** 取"当前要用的 MQTT 身份"；valid=false ⇒ 调用方回落配置兜底 */
bool credential_manager_get_identity(CredentialIdentity& out);

/** 取当前**生效**凭据（仅 ACTIVE 槽；启动首次连接用） */
bool credential_manager_get_active(char* user, size_t user_len, char* pass, size_t pass_len);

/** 回报 MQTT 连接结果（`reason_code` = MQTT CONNACK 返回码；0 表示非 CONNACK 原因） */
void credential_manager_notify_conn_result(bool ok, int reason_code);

/** 取出待上报事件（取出即清空）；返回 false = 无事件 */
bool credential_manager_take_event(CredentialEvent& out);

/** 对象名是否属于"凭据类命令"（供 CommandManager 分流 / CloudManager 脱敏判定） */
bool credential_manager_is_credential_object(const char* object);

/** 当前阶段名（仅**串口诊断**用；不得用于任何云侧语义） */
const char* credential_manager_phase_name();

/** 当前期望身份的 epoch（CloudManager 用于比对是否需要换连） */
uint32_t credential_manager_epoch();
