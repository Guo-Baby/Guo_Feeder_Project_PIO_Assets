#include "services/credential_manager.h"

#include "services/topic_renderer.h"   // GF_CLIENT_ID_TPL：client_id 模板**单点定义**

// =====================================================
// 内部状态（模块级，与 device_identity 同一风格）
// =====================================================
enum CredPhase : uint8_t
{
    CRED_PHASE_IDLE = 0,
    CRED_PHASE_WAITING_TEST,
    CRED_PHASE_PROMOTING,
    CRED_PHASE_ROLLBACK
};

static bool     s_init              = false;
static uint8_t  s_phase             = CRED_PHASE_IDLE;

static int      s_desired           = -1;      // 期望使用的槽位（-1 = 无，回落配置）
static uint32_t s_epoch             = 1;       // 身份版本（变化即换连）

static int      s_test_slot         = -1;      // 正在验证的槽位
static uint32_t s_test_gen          = 0;
static uint32_t s_test_deadline     = 0;       // millis() 截止
static uint8_t  s_test_attempts     = 0;
static char     s_test_cmd_id[40]   = { 0 };

// 连接结果（由 cloud_task 在 loop 上下文转发进来）
static bool     s_conn_pending      = false;
static bool     s_conn_ok           = false;
static int      s_conn_reason       = 0;

// 待上报事件
static CredentialEvent s_event      = { false, false, false, 0, { 0 }, { 0 } };

// 对外暴露的"不透明身份"
static char     s_id_user[CRED_USERNAME_MAX] = { 0 };
static char     s_id_pass[CRED_PASSWORD_MAX] = { 0 };
static char     s_id_tpl[64]                 = { 0 };

// =====================================================
// 工具
// =====================================================
static void bump_epoch()
{
    s_epoch++;

    if (s_epoch == 0)
    {
        s_epoch = 1;    // 0 保留作"未初始化"
    }
}

/**
 * 依据 `s_desired` 重建对外身份。
 *
 * - `s_desired` 指向的槽位状态为 TESTING ⇒ client_id 用**试连专用模板**
 *   （官方模板 + "_t<generation>"，冻结：方案 B）
 * - 其余情况（ACTIVE / 无）⇒ 官方模板
 *
 * ⚠️ 模板的**渲染**由 CloudManager 完成（工程内渲染点唯一，T-2），本模块只给模板。
 */
static void refresh_identity()
{
    s_id_user[0] = '\0';
    s_id_pass[0] = '\0';
    s_id_tpl[0]  = '\0';

    if (s_desired < 0)
    {
        return;
    }

    CredSlot slot;

    if (!cred_store_read((uint8_t)s_desired, slot) || !slot.present)
    {
        return;
    }

    strncpy(s_id_user, slot.username, sizeof(s_id_user) - 1);
    s_id_user[sizeof(s_id_user) - 1] = '\0';
    strncpy(s_id_pass, slot.password, sizeof(s_id_pass) - 1);
    s_id_pass[sizeof(s_id_pass) - 1] = '\0';

    if (slot.state == CRED_SLOT_TESTING)
    {
        snprintf(s_id_tpl, sizeof(s_id_tpl), "%s_t%lu",
                 GF_CLIENT_ID_TPL, (unsigned long)slot.generation);
    }
    else
    {
        snprintf(s_id_tpl, sizeof(s_id_tpl), "%s", GF_CLIENT_ID_TPL);
    }
}

static void emit_event(bool confirmed,
                       bool ok,
                       uint32_t generation,
                       const char* reason)
{
    s_event.pending   = true;
    s_event.confirmed = confirmed;
    s_event.ok        = ok;
    s_event.generation = generation;

    strncpy(s_event.cmd_id, s_test_cmd_id, sizeof(s_event.cmd_id) - 1);
    s_event.cmd_id[sizeof(s_event.cmd_id) - 1] = '\0';

    strncpy(s_event.reason, reason ? reason : "", sizeof(s_event.reason) - 1);
    s_event.reason[sizeof(s_event.reason) - 1] = '\0';
}

/**
 * 认证类失败判定。
 *   CONNACK 4 = bad user name or password，5 = not authorized
 *   ⇒ 这两类说明**凭据本身**不可用，立即 rollback
 *   其余（TLS / DNS / 超时 / 断网）属**网络类** ⇒ 只计次、留待重试（Q7 禁止误判）
 */
static bool is_auth_failure(int reason_code)
{
    return (reason_code == 4) || (reason_code == 5);
}

static void enter_idle()
{
    s_phase         = CRED_PHASE_IDLE;
    s_test_slot     = -1;
    s_test_gen      = 0;
    s_test_attempts = 0;
    s_test_deadline = 0;
    s_test_cmd_id[0] = '\0';
}

// =====================================================
// promote / rollback
// =====================================================
static void do_promote()
{
    CredSlot target;

    if (!cred_store_read((uint8_t)s_test_slot, target) || !target.present)
    {
        s_phase = CRED_PHASE_ROLLBACK;
        return;
    }

    const int old_active = cred_store_active_slot();

    target.state = CRED_SLOT_ACTIVE;

    if (!cred_store_write((uint8_t)s_test_slot, target))
    {
        Serial.println("[Cred] promote: store write failed");
        s_phase = CRED_PHASE_ROLLBACK;
        return;
    }

    // 旧生效槽退役（仅置 INVALID，**不删除** —— 保留以支持人工排障/回退）
    if (old_active >= 0 && old_active != s_test_slot)
    {
        CredSlot old;

        if (cred_store_read((uint8_t)old_active, old))
        {
            old.state = CRED_SLOT_INVALID;
            cred_store_write((uint8_t)old_active, old);
        }
    }

    cred_store_set_active_slot(s_test_slot);
    cred_store_set_last_generation(s_test_gen);

    s_desired = s_test_slot;    // 同一槽位，但状态已是 ACTIVE ⇒ 模板回到官方
    bump_epoch();               // 促使 CloudManager 用官方 client_id 重连

    emit_event(true, true, s_test_gen, "connack_ok");

    Serial.printf("[Cred] promote OK slot=%d gen=%lu\n",
                  s_test_slot, (unsigned long)s_test_gen);

    enter_idle();
}

static void do_rollback(const char* reason)
{
    if (s_test_slot >= 0)
    {
        cred_store_erase((uint8_t)s_test_slot);
    }

    // 回到原生效凭据；**绝不**因一次失败把 ACTIVE 判为无效（Q7）
    s_desired = cred_store_active_slot();
    bump_epoch();

    emit_event(false, false, s_test_gen, reason);

    Serial.printf("[Cred] rollback (%s) -> desired=%d\n",
                  reason ? reason : "?", s_desired);

    enter_idle();
}

// =====================================================
// 公共接口
// =====================================================
void credential_manager_init()
{
    cred_store_init();

    s_desired = cred_store_active_slot();
    s_epoch   = 1;
    s_phase   = CRED_PHASE_IDLE;
    s_init    = true;

    refresh_identity();

    Serial.printf("[Cred] init: store=%s active_slot=%d\n",
                  cred_store_ready() ? "ok" : "unavailable",
                  s_desired);
}

bool credential_manager_install(const CredentialSetRequest& req, uint8_t* err_out)
{
    if (err_out != nullptr)
    {
        *err_out = CRED_ERR_NONE;
    }

    if (!cred_store_ready())
    {
        if (err_out) *err_out = CRED_ERR_STORE_FAIL;
        return false;
    }

    if (s_phase != CRED_PHASE_IDLE)
    {
        if (err_out) *err_out = CRED_ERR_BUSY;
        return false;
    }

    if (req.username[0] == '\0' || req.password[0] == '\0')
    {
        if (err_out) *err_out = CRED_ERR_INVALID_PAYLOAD;
        return false;
    }

    // generation 必须**严格递增**（防重放；依据上次 promote 成功的值）
    uint32_t last_gen = 0;
    cred_store_get_last_generation(last_gen);

    if (req.generation <= last_gen)
    {
        if (err_out) *err_out = CRED_ERR_GENERATION_STALE;
        return false;
    }

    // 目标槽：**绝不覆盖当前生效槽**（否则失败即失联）
    const int act = cred_store_active_slot();
    int target = (act == 0) ? 1 : 0;

    if (req.slot < CRED_SLOT_COUNT && (int)req.slot != act)
    {
        target = (int)req.slot;     // 采纳云端建议（在安全前提下）
    }

    CredSlot slot;
    slot.present    = true;
    slot.state      = CRED_SLOT_TESTING;
    slot.generation = req.generation;
    memset(slot.username, 0, sizeof(slot.username));
    memset(slot.password, 0, sizeof(slot.password));
    strncpy(slot.username, req.username, sizeof(slot.username) - 1);
    strncpy(slot.password, req.password, sizeof(slot.password) - 1);

    if (!cred_store_write((uint8_t)target, slot))
    {
        if (err_out) *err_out = CRED_ERR_STORE_FAIL;
        return false;
    }

    s_test_slot     = target;
    s_test_gen      = req.generation;
    s_test_attempts = 0;
    s_test_deadline = millis() + CRED_TEST_TIMEOUT_MS;

    // ★ 清掉任何残留的连接结果：试连窗口必须从"零"开始
    //   （否则上一次正常连接的成功结果会被误当作试连成功 ⇒ 未验证即 promote）
    s_conn_pending = false;
    s_conn_ok      = false;
    s_conn_reason  = 0;

    strncpy(s_test_cmd_id, req.username, 0);
    s_test_cmd_id[0] = '\0';                 // cmd_id 由 CommandManager 随后补写

    s_desired = target;
    bump_epoch();                            // ⇒ CloudManager 下一轮换用试连身份
    s_phase = CRED_PHASE_WAITING_TEST;

    Serial.printf("[Cred] install accepted slot=%d gen=%lu (testing)\n",
                  target, (unsigned long)req.generation);

    return true;
}

void credential_manager_task()
{
    if (!s_init)
    {
        // init 尚未调用（防御）：直接返回，不触碰 NVS
        return;
    }

    if (s_phase == CRED_PHASE_IDLE)
    {
        return;
    }

    if (s_phase == CRED_PHASE_ROLLBACK)
    {
        do_rollback("store_error");
        return;
    }

    if (s_phase == CRED_PHASE_PROMOTING)
    {
        do_promote();
        return;
    }

    // ---- WAITING_TEST ----
    if (s_conn_pending)
    {
        s_conn_pending = false;

        if (s_conn_ok)
        {
            s_phase = CRED_PHASE_PROMOTING;
            return;
        }

        if (is_auth_failure(s_conn_reason))
        {
            do_rollback("auth_failed");
            return;
        }

        // 网络类失败：计次，继续等重连或超时（**不**判凭据失败）
        s_test_attempts++;

        Serial.printf("[Cred] test: network failure (attempt %u/%u, reason=%d)\n",
                      (unsigned)s_test_attempts,
                      (unsigned)CRED_TEST_MAX_ATTEMPTS,
                      s_conn_reason);

        if (s_test_attempts >= CRED_TEST_MAX_ATTEMPTS)
        {
            do_rollback("net_error");
            return;
        }
    }

    // 上电/换连超时（rollover-safe 比较）
    if (s_test_deadline != 0 && (int32_t)(millis() - s_test_deadline) >= 0)
    {
        do_rollback("timeout");
    }
}

bool credential_manager_get_identity(CredentialIdentity& out)
{
    memset(&out, 0, sizeof(out));

    // 每次读取前刷新：NVS 内容可能已被 promote/rollback 改动
    refresh_identity();

    if (s_id_user[0] == '\0' || s_id_pass[0] == '\0')
    {
        return false;
    }

    out.valid              = true;
    out.username           = s_id_user;
    out.password           = s_id_pass;
    out.client_id_template = (s_id_tpl[0] != '\0') ? s_id_tpl : GF_CLIENT_ID_TPL;
    out.epoch              = s_epoch;

    return true;
}

bool credential_manager_get_active(char* user, size_t user_len, char* pass, size_t pass_len)
{
    if (user == nullptr || pass == nullptr || user_len == 0 || pass_len == 0)
    {
        return false;
    }

    user[0] = '\0';
    pass[0] = '\0';

    const int act = cred_store_active_slot();

    if (act < 0)
    {
        return false;
    }

    CredSlot slot;

    if (!cred_store_read((uint8_t)act, slot) || !slot.present || slot.state != CRED_SLOT_ACTIVE)
    {
        return false;
    }

    strncpy(user, slot.username, user_len - 1);
    user[user_len - 1] = '\0';
    strncpy(pass, slot.password, pass_len - 1);
    pass[pass_len - 1] = '\0';

    return true;
}

void credential_manager_notify_conn_result(bool ok, int reason_code)
{
    // =====================================================
    // ★ 只接受"试连窗口内"的连接结果
    //
    // 缺陷教训（D-2 首次上板实测暴露）：此前不判阶段，导致**上一次正常连接的
    // 成功结果**（CONNECTED 事件，本来只是给"在网状态"用的）残留在标志里，
    // 等下一次 `install` 把阶段切成 WAITING_TEST 后，被 task() 当作
    // "新凭据试连成功"消费 ⇒ **未经验证即 promote**（实测日志里
    // `promote OK` 出现在任何 CONNACK 之前）。
    //
    // 修正：窗口外的结果一律丢弃 —— 试连结果的唯一合法来源是
    //        install 之后、promote/rollback 之前那段时间。
    // =====================================================
    if (s_phase != CRED_PHASE_WAITING_TEST)
    {
        return;
    }

    // 由 cloud_task 在 **loop 上下文**转发进来（避免在 esp-mqtt 任务里写 NVS）
    s_conn_pending = true;
    s_conn_ok      = ok;
    s_conn_reason  = reason_code;
}

bool credential_manager_take_event(CredentialEvent& out)
{
    if (!s_event.pending)
    {
        return false;
    }

    out = s_event;
    s_event.pending = false;
    return true;
}

bool credential_manager_is_credential_object(const char* object)
{
    if (object == nullptr)
    {
        return false;
    }

    return strcmp(object, CRED_OBJECT_CREDENTIAL_SET) == 0;
}

const char* credential_manager_phase_name()
{
    switch (s_phase)
    {
        case CRED_PHASE_WAITING_TEST: return "waiting_test";
        case CRED_PHASE_PROMOTING:    return "promoting";
        case CRED_PHASE_ROLLBACK:     return "rollback";
        default:                      return "idle";
    }
}

uint32_t credential_manager_epoch()
{
    return s_epoch;
}

// 供 CommandManager 在 install 成功后补写 cmd_id（用于 confirm 帧的 `i` 关联）
void credential_manager_set_pending_cmd_id(const char* cmd_id)
{
    if (cmd_id == nullptr)
    {
        return;
    }

    strncpy(s_test_cmd_id, cmd_id, sizeof(s_test_cmd_id) - 1);
    s_test_cmd_id[sizeof(s_test_cmd_id) - 1] = '\0';
}
