// =====================================================
// P2-3 · Homie Bridge —— 实现（投影层核心）
//
// 上位冻结设计（唯一依据）：
//   docs/architecture/P2-0-architecture-review-and-design-freeze.md
//   §6（Projection Boundary）· §7（CommandManager Integration）
//   §9（Lifecycle）· §10（$state）· §11（$description）
//   §12（Node / Property / valve 显式映射）· §13（Validation）
//   §14（Retained / QoS）· §15.3（Projection Contract / fingerprint）
//   §17.4（变化检测与限流）· §37（GF_HOMIE_ENABLED）
//   （P2-0.1 Freeze Corrections 后版本）
//
// 数据关系（必须保持 —— 任务 §一）：
//     Capability Registry ─┐
//                          ├─> Homie Bridge Projection ──> MQTT Transport
//     Device Model ────────┘
//     Homie /set ─> Homie Bridge ─> CommandMessage ─> CommandManager
//
// 禁止（越界即架构问题）：
//     Homie → 硬件 · Homie → Workflow 执行 · Homie → System State 写入
//     Homie → esp_mqtt_client · Homie → CloudManager internals
//
// 依赖边界（FROZEN §6.3 白名单 + 本阶段必需的只读扩展）：
//   ✅ services/system_state.h      （只读 state_get_* / state_is_valid）
//   ✅ services/command_manager.h   （execute + get_runtime；**不新增 callback**，AD-7a）
//   ✅ services/device_identity.h   （base topic 段 —— §8.1 强制要求，属必需扩展）
//   ✅ automation/capability_registry.h（**只读**可用性查询 —— §6.3 允许只读查询；
//                                      §五 明确 Registry 负责 capability availability）
//   ✅ cloud/mqtt_transport.h       （**唯一**允许的 cloud 头 —— §6.4）
//   ✅ homie/device_model.h · homie/homie_projection.h
//   ❌ cloud/cloud_manager.h · mqtt_client.h · app/*.h（硬件）· workflow.h
// =====================================================

#include <Arduino.h>

#include "homie/homie_bridge.h"
#include "homie/device_model.h"
#include "homie/homie_projection.h"

#include "cloud/mqtt_transport.h"
#include "services/system_state.h"
#include "services/command_manager.h"
#include "services/device_identity.h"
#include "services/time_manager.h"
#include "automation/capability_registry.h"

#include <esp_random.h>
#include <esp_heap_caps.h>

// =====================================================
// 编译期开关（FROZEN §37：默认 0 ⇒ 生产零代价）
// =====================================================
#ifndef GF_HOMIE_ENABLED
#define GF_HOMIE_ENABLED 0
#endif

#if GF_HOMIE_ENABLED

// =====================================================
// 常量
// =====================================================
// QoS = 1 / store = true：属 **Guo Feeder transport profile**（§3.4），非规范要求。
// retain：$state / $description = 规范强制（§14）；属性按 Device Model 的 retained。
#define HOMIE_QOS                1
#define HOMIE_STORE              true

#define HOMIE_MAX_PROPERTIES     32    // 属性发布状态槽上限（当前模型 16 个）
#define HOMIE_PUB_MIN_MS_DEFAULT 500   // min_pub_ms 默认值（§17.4）
#define HOMIE_PENDING_MAX        4     // pending 表定容（FROZEN §7.4）
#define HOMIE_PENDING_TIMEOUT_MS 30000 // pending 兜底超时
#define HOMIE_FP_WARN_MIN_MS     60000 // fingerprint 陈旧告警限频
#define HOMIE_SET_VALUE_MAX      40    // /set 裸值缓冲

// =====================================================
// 生命周期相位（FROZEN §9）
// =====================================================
enum HomiePhase : uint8_t
{
    HOMIE_PHASE_OFFLINE = 0,   // MQTT 未连接（不发布任何 Homie 消息）
    HOMIE_PHASE_INIT,          // 已发 $state=init
    HOMIE_PHASE_DESC,          // 已发 $description
    HOMIE_PHASE_READY          // 已发属性初值 + $state=ready
};

// =====================================================
// 模块状态（文件作用域 static；零裸全局）
// =====================================================
static HomiePhase    s_phase       = HOMIE_PHASE_OFFLINE;
static bool          s_initialized = false;
static char          s_base[HOMIE_TOPIC_MAX];

static char*         s_desc_buf    = nullptr;   // PSRAM 单块，boot 分配一次并复用（§11.4）
static uint32_t      s_desc_version = 0;
static uint32_t      s_desc_fp      = 0;        // description snapshot fingerprint（§15.3 ⑤）

static uint16_t      s_boot_seq     = 0;
static uint32_t      s_cmd_counter  = 0;
static unsigned long s_last_fp_warn_ms = 0;

// 属性发布状态（变化检测 + 限流）
struct HomiePropState
{
    HomieSample   last;
    bool          have_last;
    unsigned long last_pub_ms;
};
static HomiePropState s_prop_state[HOMIE_MAX_PROPERTIES];

// pending 表（FROZEN §7.4：定容 ≤4；只读轮询 get_runtime，不新增回调）
struct HomiePending
{
    bool          active;
    bool          saw_terminal;
    uint8_t       terminal;          // CommandState
    char          cmd_id[16];
    unsigned long start_ms;
};
static HomiePending s_pending[HOMIE_PENDING_MAX];

// =====================================================
// 日志
//
// ⚠️ 已知待办（不自行扩大设计）：P2-0 §15.3 要求 fingerprint 陈旧时
//    `log_emit(WARN, LOG_HOMIE_DESC_STALE)`，并注明「新增日志 EventId 需在 P2
//    走 LogManager 冻结流程」。log_events.h 的 EventId 段（0x01xx…0x0Fxx）
//    属已冻结契约，**不得**在 P2-3 擅自新增段。⇒ 本阶段所有 Homie 日志走串口，
//    待 LogManager EventId 冻结流程走完后再替换。
// =====================================================
static void homie_log(const char* level, const char* msg)
{
    Serial.printf("[Homie][%s] %s\n", level, msg);
}

// =====================================================
// Capability Registry 可用性谓词（boot-time availability filter）
//
// ★ 只读查询：某 action 的 runtime_id 在本次 boot 是否**已注册**。
//   这正是 §五 / §15.3 AD-15b 要求的「weight.enable=false ⇒ WEIGHT_ZERO 不注册」
//   判定来源 —— 而不是猜测配置、也不是读取 System State。
// =====================================================
static bool homie_action_available(const char* action_id, void* ctx)
{
    (void)ctx;
    if(action_id == nullptr || action_id[0] == '\0') { return false; }
    uint8_t stable_id = 0;
    return capability_get_action_stable_id(String(action_id), stable_id);
}

// =====================================================
// 前置条件（FROZEN §10.2：`ready` 的五条件合取）
//   ① MQTT 已连      —— 调用方保证
//   ② description 已发 —— 相位推进保证
//   ③ 只读初值已发     —— 相位推进保证
//   ④ capability_registry_init() 已成功
//   ⑤ command_manager 已初始化
// =====================================================
static bool homie_prereq_ready()
{
    // ④ Registry 就绪判据：已扫描且至少有 1 个 Action。
    //    （capability_registry_init() 在 setup 第七层；此处只做只读确认）
    if(capability_get_action_count() == 0)
    {
        return false;
    }
    // ⑤ command_manager_init() 在 setup 第五层完成 ⇒ loop 中必然已初始化，
    //   且本模块无只读查询接口 ⇒ 不额外探测（避免臆造判据）。
    return true;
}

// =====================================================
// 属性索引（用于状态缓存定位）
// =====================================================
static int homie_property_index(const DeviceModelProperty* p)
{
    if(p == nullptr) { return -1; }
    const uint8_t n = dm_property_count();
    for(uint8_t i = 0; i < n && i < HOMIE_MAX_PROPERTIES; ++i)
    {
        if(dm_property_at(i) == p) { return (int)i; }
    }
    return -1;
}

// =====================================================
// System State → HomieSample（泛型读取）
//
// ★ 只用 Device Model 的 `state_key` + `dtype` 决定取哪个 getter，
//   **禁止** `if (prop == "valve") read STATE_VALVE_STATUS` 这类硬编码（§十五）。
//   dtype → getter 的映射：
//     FLOAT                     → state_get_float()
//     BOOL / INT / ENUM         → state_get_int()   （bool 以 1/0 存入 .value）
//   有效性：state_is_valid(key)，再叠加 HomieGate 声明的 validity gate
//          （如 MI_THERMO_VALID —— 沿用现有语义，**不**在 Bridge 内重新定义，§三十）
// =====================================================
static bool homie_read_sample(const DeviceModelProperty* p, HomieSample* out)
{
    out->i     = 0;
    out->f     = 0.0f;
    out->valid = false;

    if(p == nullptr || out == nullptr)          { return false; }
    if(p->src_kind != DM_SRC_STATE)             { return false; }   // 命令型属性无读侧

    const SystemStateKey key = (SystemStateKey)p->state_key;
    if(!state_is_valid(key))                    { return false; }

    const HomieGate* g = homie_gate_find(p->node, p->id);
    if(g != nullptr && g->valid_key < (uint16_t)STATE_MAX)
    {
        if(!state_get_bool((SystemStateKey)g->valid_key)) { return false; }
    }

    switch(p->dtype)
    {
        case DM_FLOAT:
            out->f = state_get_float(key);
            break;
        case DM_BOOL:
        case DM_INT:
        case DM_ENUM:
            out->i = state_get_int(key);
            break;
        case DM_STRING:
        case DM_JSON:
        default:
            return false;      // P2 无 string / json 状态属性
    }

    out->valid = true;
    return true;
}

// =====================================================
// 发布属性值（初值 / 稳态变化检测）
//
//   force=true  ：跳过 min_pub_ms 门限（首次无条件发布初值 —— §17.4）
//   force=false ：变化驱动 + 死区 + 最小间隔（§17.4）
//   ★ retain 严格取 Device Model 的 `retained`（§二十四，禁止统一硬编码 true）
//   ★ 值无效 / 无法序列化 ⇒ 跳过（**绝不伪造 0** —— §二十九）
// =====================================================
static void homie_publish_values(bool force)
{
    const unsigned long now = millis();
    const uint8_t       pc  = dm_property_count();

    for(uint8_t i = 0; i < pc && i < HOMIE_MAX_PROPERTIES; ++i)
    {
        const DeviceModelProperty* p = dm_property_at(i);
        if(p == nullptr) { continue; }

        // 未进入本次 description 的属性 ⇒ 不发布（与 $description 保持一致，§15.3 ②）
        if(!homie_property_available(p, homie_action_available, nullptr)) { continue; }

        HomieSample s;
        if(!homie_read_sample(p, &s)) { continue; }

        char   val[HOMIE_SET_VALUE_MAX];
        size_t vlen = 0;
        if(!homie_format_value(p, s, val, sizeof(val), &vlen)) { continue; }

        HomiePropState& st = s_prop_state[i];
        const uint16_t  min_pub = (p->min_pub_ms != 0) ? p->min_pub_ms
                                                       : (uint16_t)HOMIE_PUB_MIN_MS_DEFAULT;

        const bool changed = homie_value_changed(
            p->dtype, st.last, !st.have_last, s, p->deadband);

        if(!changed)                                    { continue; }
        if(!force && (now - st.last_pub_ms < min_pub))  { continue; }

        char topic[HOMIE_TOPIC_MAX];
        if(!homie_property_topic(s_base, p->node, p->id, topic, sizeof(topic))) { continue; }

        if(mqtt_transport_publish(
               topic, val, vlen, HOMIE_QOS, p->retained, HOMIE_STORE))
        {
            st.last        = s;
            st.have_last   = true;
            st.last_pub_ms = now;
        }
    }
}

// =====================================================
// $state 发布（FROZEN §10：retained=true 属**规范强制**）
// =====================================================
static bool homie_publish_state(const char* state)
{
    char topic[HOMIE_TOPIC_MAX];
    if(!homie_state_topic(s_base, topic, sizeof(topic))) { return false; }
    return mqtt_transport_publish(
        topic, state, hp_len(state), HOMIE_QOS, true, HOMIE_STORE);
}

// =====================================================
// $description：boot-time snapshot（FROZEN §11.2 / §15.3）
//
//   ★ 唯一生成点：进入 init 窗口后（§八）。
//   ★ 缓冲：PSRAM 单块，首次分配后**复用**（§11.4 / AD-11a 禁止静态大数组）。
//   ★ 超 HOMIE_DESC_CAP ⇒ 失败 ⇒ 停在 init（**不发布坏描述**，§11.4）。
//   ★ 成功后记录 fingerprint，供运行期一致性哨兵比较（§15.3 ⑤）。
// =====================================================
static bool homie_build_and_publish_description()
{
    if(s_desc_buf == nullptr)
    {
        s_desc_buf = (char*)heap_caps_malloc(HOMIE_DESC_CAP, MALLOC_CAP_SPIRAM);
        if(s_desc_buf == nullptr)
        {
            // 回退 DRAM 堆（有总比没有强；失败仍安全停在 init）
            s_desc_buf = (char*)heap_caps_malloc(HOMIE_DESC_CAP, MALLOC_CAP_8BIT);
        }
        if(s_desc_buf == nullptr)
        {
            homie_log("ERROR", "description buffer alloc failed");
            return false;
        }
    }

    HomieDescInput in;
    in.device_id       = device_id();
    in.buf             = s_desc_buf;
    in.cap             = HOMIE_DESC_CAP;
    in.avail           = homie_action_available;
    in.avail_ctx       = nullptr;
    in.action_version  = capability_get_action_version();
    in.trigger_version = capability_get_trigger_version();

    size_t   len = 0;
    uint32_t ver = 0;
    if(!homie_desc_build(in, &len, &ver))
    {
        homie_log("ERROR", "description build failed (too large)");
        return false;
    }

    char topic[HOMIE_TOPIC_MAX];
    if(!homie_description_topic(s_base, topic, sizeof(topic))) { return false; }

    if(!mqtt_transport_publish(topic, s_desc_buf, len, HOMIE_QOS, true, HOMIE_STORE))
    {
        return false;
    }

    s_desc_version = ver;
    s_desc_fp      = ver;      // fingerprint = 生成时的 description version（§15.3 ⑤）
    if(len > HOMIE_DESC_TARGET)
    {
        homie_log("WARN", "description exceeds target size");
    }
    return true;
}

// =====================================================
// pending 表：只读轮询（FROZEN §7.4 / AD-7a）
//
//   ★ **不**调用 command_manager_set_result_callback()（会窃取 CloudManager 的回调）
//   ★ 成功 ⇒ 不写属性值，等 System State 自然反映（§12.5-5）
//   ★ 失败 / 超时 ⇒ WARN + 释放槽位，property 保持原值
// =====================================================
static bool homie_pending_add(const char* cmd_id)
{
    for(uint8_t i = 0; i < HOMIE_PENDING_MAX; ++i)
    {
        if(!s_pending[i].active)
        {
            s_pending[i].active       = true;
            s_pending[i].saw_terminal = false;
            s_pending[i].terminal     = 0;
            s_pending[i].start_ms     = millis();
            hp_copy(s_pending[i].cmd_id, sizeof(s_pending[i].cmd_id), cmd_id);
            return true;
        }
    }
    homie_log("WARN", "pending table full, command result not tracked");
    return false;
}

static void homie_poll_pending()
{
    const unsigned long now = millis();

    for(uint8_t i = 0; i < HOMIE_PENDING_MAX; ++i)
    {
        HomiePending& pe = s_pending[i];
        if(!pe.active) { continue; }

        CommandRuntime* rt = command_manager_get_runtime(String(pe.cmd_id));

        if(rt == nullptr)
        {
            // 运行时记录已释放 ⇒ 结果已由 CommandManager 走既有通道处理；
            // 本表只负责释放槽位（§7.4「以最后一次已知结果收尾」）。
            pe.active = false;
            continue;
        }

        if(rt->state != COMMAND_STATE_PENDING)
        {
            pe.saw_terminal = true;
            pe.terminal     = (uint8_t)rt->state;
            pe.active       = false;
            if(rt->state != COMMAND_STATE_SUCCESS)
            {
                homie_log("WARN", "homie command not successful");
            }
            continue;
        }

        if(now - pe.start_ms >= HOMIE_PENDING_TIMEOUT_MS)
        {
            homie_log("WARN", "homie command tracking timeout");
            pe.active = false;
        }
    }
}

// =====================================================
// 一致性哨兵（FROZEN §15.3 ③⑤ / AD-15a′）
//
//   运行期 Registry 与 description snapshot 不一致 ⇒ **只告警**：
//     ✗ 不重新生成 $description
//     ✗ 不重新发布 $description
//     ✗ 不修改 property 的 settable / availability contract
//     ✗ 不进入 $state=ready 重建流程
// =====================================================
static void homie_check_fingerprint()
{
    if(s_phase != HOMIE_PHASE_READY) { return; }

    const uint32_t cur = homie_desc_version(
        capability_get_action_version(), capability_get_trigger_version());

    if(cur == s_desc_fp) { return; }

    const unsigned long now = millis();
    if(now - s_last_fp_warn_ms >= HOMIE_FP_WARN_MIN_MS)
    {
        s_last_fp_warn_ms = now;
        homie_log("WARN", "LOG_HOMIE_DESC_STALE: registry changed since boot snapshot");
    }
}

// =====================================================
// Homie `/set` 处理（**loop 上下文** —— 由 Transport Facade 的 foreign 队列交付）
//
// 路径（FROZEN §7.2，不可跳过任何一步）：
//   ① 丢弃 retain=1 的投递          （§14 AD-14a / §19.5，判据**仅** retain）
//   ② 命名空间校验 + node/prop 解析   （§十九，本设备 Homie namespace）
//   ③ dm_find() 查属性                （未知 ⇒ 丢弃 + WARN）
//   ④ dm_validate_set() 协议层校验    （§13.2 第①层：dtype / enum / range / settable）
//   ⑤ dm_resolve_command() 显式映射   （§12.5，禁止字符串推导 Action ID）
//   ⑥ 自生成 cmd_id                   （§7.3；客户端无法提供 / 覆盖）
//   ⑦ command_manager_execute()       （source="homie"，与 "cloud" 隔离）
//   ⑧ 进 pending 表                   （§7.4）
//
// ★ 绝不：直接调硬件 / 直接写 System State / 绕过 CommandManager
// =====================================================
static void homie_on_foreign(const char* topic,
                             const uint8_t* data,
                             size_t len,
                             bool retain)
{
    // ---- ① retained 投递一律丢弃 ----
    //
    // 覆盖两种场景（§19.5）：
    //   A 实时 retained publish  ：client publish /set (retain=1)
    //   B broker retained replay ：设备重连后 broker 重放历史 retained /set
    // 判据**只有** retain（由 P2-2 从 event->retain 保留下来），
    // 禁止用 payload / topic / 时间 / cmd_id 等启发式替代。
    if(retain)
    {
        homie_log("WARN", "discard retained /set");
        return;
    }

    // ---- ② 命名空间 + node/prop 解析 ----
    char node[32];
    char prop[32];
    if(!homie_parse_set_topic(topic, s_base, node, sizeof(node), prop, sizeof(prop)))
    {
        homie_log("WARN", "set topic outside own homie namespace, discarded");
        return;
    }

    // ---- ③ 属性查找 ----
    const DeviceModelProperty* p = dm_find(node, prop);
    if(p == nullptr)
    {
        homie_log("WARN", "unknown node/property in /set, discarded");
        return;
    }

    // ---- /set 载荷：裸值（不是 JSON 信封，§8.3）----
    char value[HOMIE_SET_VALUE_MAX];
    if(len == 0 || len + 1 > sizeof(value))
    {
        homie_log("WARN", "empty or oversized /set payload, discarded");
        return;
    }
    for(size_t i = 0; i < len; ++i)
    {
        const uint8_t c = data[i];
        if(c == 0 || c < 0x20 || c > 0x7E)
        {
            homie_log("WARN", "non-printable byte in /set payload, discarded");
            return;
        }
        value[i] = (char)c;
    }
    value[len] = '\0';

    // ---- ④ 协议层校验（形状；业务语义校验由 CommandManager / Action 负责）----
    const DeviceModelValidateResult vr = dm_validate_set(p, value);
    if(vr != DM_VALID_OK)
    {
        homie_log("WARN", "protocol-layer validation rejected /set");
        return;
    }

    // ---- ⑤ 显式命令映射 ----
    const DeviceModelCommand* cmd = nullptr;
    if(!dm_resolve_command(p, value, &cmd) || cmd == nullptr)
    {
        homie_log("WARN", "no command mapping for /set value");
        return;
    }

    // ---- ⑥ cmd_id（设备自生成；客户端无法介入）----
    char cid[16];
    homie_cmd_id(s_boot_seq, ++s_cmd_counter, cid, sizeof(cid));

    // ---- ⑦ 经 CommandManager 执行 ----
    CommandMessage m;
    m.command   = "execute_action";
    m.object    = cmd->action_id;      // 如 "VALVE_OPEN"（来自 Device Model 显式表）
    m.cmd_id    = String(cid);
    m.payload   = String(cmd->payload_json);
    m.source    = "homie";             // ★ 与 "cloud" 区分 ⇒ 天然隔离凭据类命令（§7.2）
    m.timestamp = (unsigned long)time_get();

    if(!command_manager_execute(m))
    {
        homie_log("WARN", "command_manager_execute rejected homie command");
        return;
    }

    // ---- ⑧ 进 pending 表（只读轮询结果）----
    homie_pending_add(cid);
}

// =====================================================
// 对外接口
// =====================================================
void homie_init()
{
    if(s_initialized) { return; }

    // §8.1：base topic 段必须来自 DeviceIdentity（禁止自行取 MAC / 随机 / 另一套 ID）
    if(!device_id_valid())
    {
        homie_log("ERROR", "device_id invalid, homie disabled");
        return;
    }
    if(!homie_base_topic(device_id(), s_base, sizeof(s_base)))
    {
        homie_log("ERROR", "base topic build failed, homie disabled");
        return;
    }

    // ① foreign 回调（★ 在 **loop 上下文**执行 —— P2-2 已保证）
    mqtt_transport_set_foreign_handler(homie_on_foreign);

    // ② 订阅本设备 set filter（幂等；未连接只注册，CONNECTED 时自动 replay —— §33）
    //    ★ 精确 filter，不含 '#'，不跨设备
    char filter[HOMIE_TOPIC_MAX];
    if(homie_set_filter(s_base, filter, sizeof(filter)))
    {
        if(!mqtt_transport_subscribe(filter, HOMIE_QOS))
        {
            homie_log("ERROR", "homie subscribe registration failed");
        }
    }
    else
    {
        homie_log("ERROR", "set filter build failed");
    }

    // ③ LWT：仅在 Homie 启用时设置（AD-10a）；payload=`lost`、retain=true 属规范强制（§10.3）
    char st[HOMIE_TOPIC_MAX];
    if(homie_state_topic(s_base, st, sizeof(st)))
    {
        mqtt_transport_set_lwt(st, "lost", HOMIE_QOS, true);
    }

    // boot 序号（§7.3）：用于 cmd_id 的"设备内短期唯一"
    s_boot_seq    = (uint16_t)(esp_random() & 0xFFFFu);
    s_cmd_counter = 0;
    s_initialized = true;

    Serial.printf("[Homie] enabled, base=%s\n", s_base);
}

void homie_task()
{
    if(!s_initialized) { return; }

    // ---- MQTT 未连接：保持在 OFFLINE，**不发布任何 Homie 消息**（§9）----
    //    `lost` 由 broker 依 LWT 发布；设备不自行发 disconnected/lost。
    if(!mqtt_transport_is_connected())
    {
        if(s_phase != HOMIE_PHASE_OFFLINE)
        {
            s_phase = HOMIE_PHASE_OFFLINE;   // 重连后重走 init→ready（幂等，§9）
        }
        return;
    }

    switch(s_phase)
    {
        case HOMIE_PHASE_OFFLINE:
        {
            // §10.2 ④⑤：Registry / CommandManager 未就绪 ⇒ 停在 OFFLINE（不进 init）
            if(!homie_prereq_ready()) { return; }
            if(!homie_publish_state("init")) { return; }
            s_phase = HOMIE_PHASE_INIT;
            break;
        }

        case HOMIE_PHASE_INIT:
        {
            // 失败 ⇒ 停在 init（**绝不发布 ready** —— §9）
            if(!homie_build_and_publish_description()) { return; }
            s_phase = HOMIE_PHASE_DESC;
            break;
        }

        case HOMIE_PHASE_DESC:
        {
            // 只读属性初值（首次无条件发布，§17.4）
            homie_publish_values(true);
            if(!homie_publish_state("ready")) { return; }
            s_phase = HOMIE_PHASE_READY;
            Serial.println("[Homie] $state=ready");
            break;
        }

        case HOMIE_PHASE_READY:
        {
            // 稳态：变化驱动 + 死区 + 最小间隔（§17.4）
            homie_publish_values(false);
            break;
        }

        default:
            s_phase = HOMIE_PHASE_OFFLINE;
            break;
    }

    homie_poll_pending();
    homie_check_fingerprint();
}

bool homie_is_ready()
{
    return s_phase == HOMIE_PHASE_READY;
}

#else   // =====================================================
        // GF_HOMIE_ENABLED == 0 —— 生产默认（FROZEN §37）
        //
        // 空实现：不订阅、不发布、不注册回调 ⇒ **零 MQTT 流量、零运行时开销**。
        // =====================================================

void homie_init()      { }
void homie_task()      { }
bool homie_is_ready()  { return false; }

#endif // GF_HOMIE_ENABLED
