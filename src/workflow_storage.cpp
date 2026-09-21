#include "workflow_storage.h"
#include "workflow.h"   // 仅用于常量一致性 static_assert，不调用其任何函数
#include "log_manager.h"   // Phase 6-B-1：观测埋点（EventId / ParamId + log_emit）

#include <string.h>
#include <stdio.h>
#include <time.h>
#include <esp_heap_caps.h>

// =====================================================
// 常量一致性校验
//
// WorkflowStorage 头文件中不 include workflow.h（避免头文件相互包含），
// 因此在此处用 static_assert 保证两套常量永不漂移。
// =====================================================
static_assert(WF_STG_MAX_COUNT == WORKFLOW_MAX_COUNT, "WF_STG_MAX_COUNT 与 WORKFLOW_MAX_COUNT 不一致");
static_assert(WF_STG_MAX_STEP == WORKFLOW_MAX_STEP, "WF_STG_MAX_STEP 与 WORKFLOW_MAX_STEP 不一致");
static_assert(WF_STG_MAX_PARAM == WORKFLOW_MAX_PARAM, "WF_STG_MAX_PARAM 与 WORKFLOW_MAX_PARAM 不一致");

// =====================================================
// BIN 布局常量
// =====================================================
#define WF_STG_STEP_HEADER_SIZE 16u
#define WF_STG_META_HEADER_SIZE 12u

// Meta Entry 序列化布局：
//   v1（81B）：valid(1) version(1) step_count(2) update_time(4) crc32(4)
//              id(32) name(32) enable(1) timeout_ms(4)
//   v2（85B）：v1 布局 + txn_id(4)   —— Workflow 级事务提交标识
//   v3（89B）：v2 布局 + variant(4)  —— Workflow 内容版本（云端增量同步）
#define WF_STG_META_ENTRY_SIZE_V1 81u
#define WF_STG_META_ENTRY_SIZE_V2 85u
#define WF_STG_META_ENTRY_SIZE_V3 89u

#define WF_STG_META_SIZE_V1 (WF_STG_META_HEADER_SIZE + WF_STG_META_ENTRY_SIZE_V1 * WF_STG_MAX_COUNT)
#define WF_STG_META_SIZE_V2 (WF_STG_META_HEADER_SIZE + WF_STG_META_ENTRY_SIZE_V2 * WF_STG_MAX_COUNT)
#define WF_STG_META_SIZE_V3 (WF_STG_META_HEADER_SIZE + WF_STG_META_ENTRY_SIZE_V3 * WF_STG_MAX_COUNT)

// =====================================================
// Phase 6-B-1：日志埋点用的模块标识 / 路径标识
//
// 与 LOG_P_ERR_CODE 同一约定 —— 取值是**本模块私有枚举**，不扩 LogManager API、
// 不做字符串参数（P2 已定版：字符串一律哈希/枚举化）。
//   LOG_P_MODULE：与 main.cpp 的 STG_BRIDGE_MODULE_* 共用同一编号空间
//                 json=0 / file=1 / bin=2 / workflow_storage=3
//   LOG_P_PATH  ：本模块内的"路径标识"（字符串路径不参与日志）
// =====================================================
static constexpr uint32_t WF_STG_LOG_MODULE    = 3u;
static constexpr uint32_t WF_STG_LOG_PATH_META = 1u;   // 对应 WF_STG_META_PATH

// 事务暂存文件后缀：stepNN.bin.t<txn_id 十六进制>
// 见 workflow_storage_save() 事务说明。
#define WF_STG_STAGE_SUFFIX ".t"

// =====================================================
// 故障注入状态（仅测试用；生产默认全部关闭）
// =====================================================
static int8_t s_test_fail_step = -1;   // -1 = 关闭
static int s_test_abort_phase = 0;     // 0 = 关闭, 1 = 预提交中断, 2 = 提交后中断
static int8_t s_test_fail_wf = -1;     // -1 = 关闭；指定 Workflow 的整次 save 失败

// =====================================================
// 模块内部状态
// =====================================================
static bool s_initialized = false;
static bool s_meta_loaded = false;
static WorkflowMetaEntry s_meta[WF_STG_MAX_COUNT];

// =====================================================
// CRC32（IEEE 802.3，反射，poly 0xEDB88320）
//
// 本模块必须自己算 buffer CRC，因为 BinStorage / FileStorage 只提供
// 基于文件路径的 CRC，而序列化在内存中进行。
// =====================================================
static uint32_t wf_crc32_init()
{
    return 0xFFFFFFFFu;
}

static uint32_t wf_crc32_update(
    uint32_t crc,
    const uint8_t *data,
    size_t length
)
{
    for (size_t i = 0; i < length; i++)
    {
        crc ^= data[i];
        for (uint8_t b = 0; b < 8; b++)
        {
            if (crc & 1u)
            {
                crc = (crc >> 1) ^ 0xEDB88320u;
            }
            else
            {
                crc >>= 1;
            }
        }
    }
    return crc;
}

static uint32_t wf_crc32_final(
    uint32_t crc
)
{
    return crc ^ 0xFFFFFFFFu;
}

// =====================================================
// 逐字节序列化原语
//
// 全部手工 put/get，绝不 memcpy 结构体，
// 规避编译器对齐与 padding 差异导致的格式漂移。
// =====================================================
static void put_u8(uint8_t *buf, size_t &pos, uint8_t value)
{
    buf[pos++] = value;
}

static void put_u16(uint8_t *buf, size_t &pos, uint16_t value)
{
    buf[pos++] = (uint8_t)(value & 0xFFu);
    buf[pos++] = (uint8_t)((value >> 8) & 0xFFu);
}

static void put_u32(uint8_t *buf, size_t &pos, uint32_t value)
{
    for (uint8_t i = 0; i < 4; i++)
    {
        buf[pos++] = (uint8_t)((value >> (i * 8)) & 0xFFu);
    }
}

static void put_i32(uint8_t *buf, size_t &pos, int32_t value)
{
    put_u32(buf, pos, (uint32_t)value);
}

static void put_f32(uint8_t *buf, size_t &pos, float value)
{
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    put_u32(buf, pos, bits);
}

// 定长字符串：不足补 0，永远写满 n 字节
static void put_fixed(uint8_t *buf, size_t &pos, const char *str, size_t n)
{
    size_t k = 0;
    if (str != nullptr)
    {
        for (; k < n && str[k] != '\0'; k++)
        {
            buf[pos + k] = (uint8_t)str[k];
        }
    }
    for (; k < n; k++)
    {
        buf[pos + k] = 0;
    }
    pos += n;
}

static uint8_t get_u8(const uint8_t *buf, size_t &pos)
{
    return buf[pos++];
}

static uint16_t get_u16(const uint8_t *buf, size_t &pos)
{
    uint16_t v = (uint16_t)buf[pos];
    v |= (uint16_t)((uint16_t)buf[pos + 1] << 8);
    pos += 2;
    return v;
}

static uint32_t get_u32(const uint8_t *buf, size_t &pos)
{
    uint32_t v = 0;
    for (uint8_t i = 0; i < 4; i++)
    {
        v |= ((uint32_t)buf[pos + i]) << (i * 8);
    }
    pos += 4;
    return v;
}

static int32_t get_i32(const uint8_t *buf, size_t &pos)
{
    return (int32_t)get_u32(buf, pos);
}

static float get_f32(const uint8_t *buf, size_t &pos)
{
    uint32_t bits = get_u32(buf, pos);
    float value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

static void get_fixed(const uint8_t *buf, size_t &pos, char *dst, size_t n)
{
    memcpy(dst, buf + pos, n);
    dst[n - 1] = '\0';
    pos += n;
}

// =====================================================
// 路径构造
// =====================================================
static bool build_workflow_dir(
    uint8_t workflow_id,
    char *out,
    size_t out_size
)
{
    if (workflow_id >= WF_STG_MAX_COUNT)
    {
        return false;
    }
    int n = snprintf(out, out_size, "%s/wf%02u", WF_STG_DIR, (unsigned)workflow_id);
    return (n > 0 && (size_t)n < out_size);
}

static bool build_step_path(
    uint8_t workflow_id,
    uint8_t step_id,
    char *out,
    size_t out_size
)
{
    if (workflow_id >= WF_STG_MAX_COUNT || step_id >= WF_STG_MAX_STEP)
    {
        return false;
    }
    int n = snprintf(out, out_size, "%s/wf%02u/step%02u.bin",
                     WF_STG_DIR, (unsigned)workflow_id, (unsigned)step_id);
    return (n > 0 && (size_t)n < out_size);
}

// =====================================================
// Step Payload 序列化
//
// 布局：
//   u8  workflow_index
//   u8  step_index
//   u8  type
//   u8  instance_type
//   char id[32]
//   u8  param_count
//   params[8]
//
// 返回写入的 payload 字节数。
// =====================================================
static size_t serialize_step_payload(
    const WorkflowStepDefinition *step,
    uint8_t workflow_id,
    uint8_t step_id,
    uint8_t *out
)
{
    size_t pos = 0;

    put_u8(out, pos, workflow_id);
    put_u8(out, pos, step_id);
    put_u8(out, pos, step->type);
    put_u8(out, pos, step->instance_type);
    put_fixed(out, pos, step->id, WF_STG_ID_MAX_LEN);

    uint8_t count = step->param_count;
    if (count > WF_STG_MAX_PARAM)
    {
        count = WF_STG_MAX_PARAM;
    }
    put_u8(out, pos, count);

    for (uint8_t i = 0; i < count; i++)
    {
        const WorkflowParamValueDefinition &p = step->params[i];
        put_fixed(out, pos, p.name, WF_STG_PARAM_NAME_LEN);
        put_u8(out, pos, p.type);
        put_i32(out, pos, p.int_value);
        put_f32(out, pos, p.float_value);
        put_u8(out, pos, p.bool_value);
        put_fixed(out, pos, p.string_value, WF_STG_PARAM_STR_LEN);
    }

    return pos;
}

static bool deserialize_step_payload(
    const uint8_t *in,
    size_t payload_size,
    uint8_t workflow_id,
    uint8_t step_id,
    WorkflowStepDefinition *step
)
{
    size_t pos = 0;

    // 最小长度：4 字节头 + id(32) + param_count(1)
    if (payload_size < 4u + WF_STG_ID_MAX_LEN + 1u)
    {
        return false;
    }

    uint8_t wf = get_u8(in, pos);
    uint8_t st = get_u8(in, pos);
    if (wf != workflow_id || st != step_id)
    {
        return false;
    }

    step->type = get_u8(in, pos);
    step->instance_type = get_u8(in, pos);
    get_fixed(in, pos, step->id, WF_STG_ID_MAX_LEN);

    uint8_t count = get_u8(in, pos);
    if (count > WF_STG_MAX_PARAM)
    {
        return false;
    }

    size_t need = pos + (size_t)count * (WF_STG_PARAM_NAME_LEN + 1u + 4u + 4u + 1u + WF_STG_PARAM_STR_LEN);
    if (need > payload_size)
    {
        return false;
    }

    step->param_count = count;
    for (uint8_t i = 0; i < count; i++)
    {
        WorkflowParamValueDefinition &p = step->params[i];
        get_fixed(in, pos, p.name, WF_STG_PARAM_NAME_LEN);
        p.type = get_u8(in, pos);
        p.int_value = get_i32(in, pos);
        p.float_value = get_f32(in, pos);
        p.bool_value = get_u8(in, pos);
        get_fixed(in, pos, p.string_value, WF_STG_PARAM_STR_LEN);
    }

    return true;
}

// =====================================================
// 内部：加载单个 Step，并可回传 payload CRC
// =====================================================
static WorkflowStorageResult load_step_internal(
    uint8_t workflow_id,
    uint8_t step_id,
    WorkflowStepDefinition *step,
    uint32_t *payload_crc_out
)
{
    char path[64];
    if (!build_step_path(workflow_id, step_id, path, sizeof(path)))
    {
        return WF_STG_ERR_INVALID_ARGUMENT;
    }

    if (!bin_storage_exists(path))
    {
        return WF_STG_ERR_NOT_FOUND;
    }

    size_t file_size = bin_storage_size(path);
    if (file_size < WF_STG_STEP_HEADER_SIZE || file_size > WF_STG_STEP_BIN_MAX)
    {
        return WF_STG_ERR_FORMAT_INVALID;
    }

    uint8_t buf[WF_STG_STEP_BIN_MAX];
    size_t bytes_read = 0;
    WorkflowStorageResult r = workflow_storage_from_bin_result(
        bin_storage_read(path, buf, file_size, bytes_read)
    );
    if (r != WF_STG_OK)
    {
        return r;
    }
    if (bytes_read != file_size)
    {
        return WF_STG_ERR_READ_FAILED;
    }

    size_t pos = 0;
    uint32_t magic = get_u32(buf, pos);
    uint16_t version = get_u16(buf, pos);
    uint16_t header_size = get_u16(buf, pos);
    uint32_t payload_size = get_u32(buf, pos);
    uint32_t crc_stored = get_u32(buf, pos);

    if (magic != WF_STG_STEP_MAGIC)
    {
        return WF_STG_ERR_FORMAT_INVALID;
    }
    if (version > WF_STG_VERSION)
    {
        return WF_STG_ERR_VERSION_TOO_NEW;
    }
    if (version != WF_STG_VERSION)
    {
        return WF_STG_ERR_VERSION_MISMATCH;
    }
    if (header_size != WF_STG_STEP_HEADER_SIZE)
    {
        return WF_STG_ERR_FORMAT_INVALID;
    }
    if (payload_size == 0 || header_size + payload_size != file_size)
    {
        return WF_STG_ERR_FORMAT_INVALID;
    }

    const uint8_t *payload = buf + header_size;
    uint32_t crc_calc = wf_crc32_final(
        wf_crc32_update(wf_crc32_init(), payload, payload_size)
    );
    if (crc_calc != crc_stored)
    {
        return WF_STG_ERR_CRC_FAILED;
    }

    if (!deserialize_step_payload(payload, payload_size, workflow_id, step_id, step))
    {
        return WF_STG_ERR_FORMAT_INVALID;
    }

    if (payload_crc_out != nullptr)
    {
        *payload_crc_out = crc_calc;
    }

    return WF_STG_OK;
}

// =====================================================
// Meta 序列化 / 反序列化
// =====================================================
static size_t serialize_meta(
    uint8_t *out
)
{
    size_t pos = 0;

    // Header 占位，entries 序列化后再回填
    pos = WF_STG_META_HEADER_SIZE;

    for (uint8_t i = 0; i < WF_STG_MAX_COUNT; i++)
    {
        const WorkflowMetaEntry &e = s_meta[i];
        put_u8(out, pos, e.valid ? 1u : 0u);
        put_u8(out, pos, e.version);
        put_u16(out, pos, e.step_count);
        put_u32(out, pos, e.update_time);
        put_u32(out, pos, e.crc32);
        put_fixed(out, pos, e.id, WF_STG_ID_MAX_LEN);
        put_fixed(out, pos, e.name, WF_STG_NAME_MAX_LEN);
        put_u8(out, pos, e.enable ? 1u : 0u);
        put_u32(out, pos, e.timeout_ms);
        put_u32(out, pos, e.txn_id);
        put_u32(out, pos, e.variant);
    }

    size_t entries_size = pos - WF_STG_META_HEADER_SIZE;

    uint32_t crc = wf_crc32_final(
        wf_crc32_update(wf_crc32_init(), out + WF_STG_META_HEADER_SIZE, entries_size)
    );

    size_t hp = 0;
    put_u32(out, hp, WF_STG_META_MAGIC);
    put_u16(out, hp, WF_STG_META_VERSION);
    put_u16(out, hp, WF_STG_MAX_COUNT);
    put_u32(out, hp, crc);

    return pos;
}

static WorkflowStorageResult deserialize_meta(
    const uint8_t *in,
    size_t size
)
{
    // 版本 1 / 2 / 3 布局长度不同：
    //   v2 每 entry 多 txn_id 4 字节
    //   v3 每 entry 再多 variant 4 字节
    // 兼容读取：旧版固件写入的 meta 仍可加载
    //（缺失的 txn_id / variant 视为 0）。
    int layout;   // 1 / 2 / 3
    if (size == WF_STG_META_SIZE_V3)
    {
        layout = 3;
    }
    else if (size == WF_STG_META_SIZE_V2)
    {
        layout = 2;
    }
    else if (size == WF_STG_META_SIZE_V1)
    {
        layout = 1;
    }
    else
    {
        return WF_STG_ERR_FORMAT_INVALID;
    }

    size_t pos = 0;
    uint32_t magic = get_u32(in, pos);
    uint16_t version = get_u16(in, pos);
    uint16_t workflow_count = get_u16(in, pos);
    uint32_t crc_stored = get_u32(in, pos);

    if (magic != WF_STG_META_MAGIC)
    {
        return WF_STG_ERR_FORMAT_INVALID;
    }
    if (version > WF_STG_META_VERSION)
    {
        return WF_STG_ERR_VERSION_TOO_NEW;
    }
    if (version < 1 || version > WF_STG_META_VERSION)
    {
        return WF_STG_ERR_VERSION_MISMATCH;
    }
    if ((layout == 3 && version < 3) ||
        (layout == 2 && version != 2) ||
        (layout == 1 && version != 1))
    {
        // 头部版本与文件长度自相矛盾，视为损坏
        return WF_STG_ERR_FORMAT_INVALID;
    }
    if (workflow_count != WF_STG_MAX_COUNT)
    {
        return WF_STG_ERR_FORMAT_INVALID;
    }

    size_t entries_size = size - WF_STG_META_HEADER_SIZE;
    uint32_t crc_calc = wf_crc32_final(
        wf_crc32_update(wf_crc32_init(), in + WF_STG_META_HEADER_SIZE, entries_size)
    );
    if (crc_calc != crc_stored)
    {
        return WF_STG_ERR_CRC_FAILED;
    }

    for (uint8_t i = 0; i < WF_STG_MAX_COUNT; i++)
    {
        WorkflowMetaEntry &e = s_meta[i];
        e.valid = get_u8(in, pos) ? 1u : 0u;
        e.version = get_u8(in, pos);
        e.step_count = get_u16(in, pos);
        e.update_time = get_u32(in, pos);
        e.crc32 = get_u32(in, pos);
        get_fixed(in, pos, e.id, WF_STG_ID_MAX_LEN);
        get_fixed(in, pos, e.name, WF_STG_NAME_MAX_LEN);
        e.enable = get_u8(in, pos) ? 1u : 0u;
        e.timeout_ms = get_u32(in, pos);
        e.txn_id = (layout >= 2) ? get_u32(in, pos) : 0u;
        e.variant = (layout >= 3) ? get_u32(in, pos) : 0u;

        if (e.step_count > WF_STG_MAX_STEP)
        {
            e.valid = 0;
            e.step_count = 0;
        }
    }

    return WF_STG_OK;
}

static void reset_meta()
{
    memset(s_meta, 0, sizeof(s_meta));
    for (uint8_t i = 0; i < WF_STG_MAX_COUNT; i++)
    {
        s_meta[i].version = WF_STG_META_VERSION;
    }
}

// =====================================================
// 事务辅助（暂存文件 / 恢复）
// =====================================================

// 把 uint32 显式折叠进 CRC 流（统一 little-endian 字节序，
// 不依赖 (const uint8_t *)&value 直读内存 —— 直读在非小端平台会漂移）
static void crc_append_u32_le(uint32_t &crc, uint32_t value)
{
    uint8_t le[4];
    for (uint8_t i = 0; i < 4; i++)
    {
        le[i] = (uint8_t)((value >> (i * 8)) & 0xFFu);
    }
    crc = wf_crc32_update(crc, le, sizeof(le));
}

// 事务暂存文件：/workflow/wfNN/stepMM.bin.t<txn_id>
// txn_id 编码在文件名中，掉电后据此判断应发布还是丢弃。
static bool build_stage_path(
    uint8_t workflow_id,
    uint8_t step_id,
    uint32_t txn_id,
    char *out,
    size_t out_size
)
{
    if (workflow_id >= WF_STG_MAX_COUNT || step_id >= WF_STG_MAX_STEP)
    {
        return false;
    }
    int n = snprintf(out, out_size, "%s/wf%02u/step%02u.bin.t%08lx",
                     WF_STG_DIR, (unsigned)workflow_id, (unsigned)step_id,
                     (unsigned long)txn_id);
    return (n > 0 && (size_t)n < out_size);
}

// 解析暂存文件名中的 txn_id；非暂存文件返回 false。
// name 形如 "step03.bin.t0000000a"。
static bool parse_stage_txn_id(
    const char *name,
    uint32_t &txn_id
)
{
    const char *dot_t = strstr(name, ".bin.t");
    if (dot_t == nullptr)
    {
        return false;
    }

    const char *hex = dot_t + 6;  // 跳过 ".bin.t"
    if (strlen(hex) == 0 || strlen(hex) > 8)
    {
        return false;
    }

    uint32_t v = 0;
    for (const char *p = hex; *p != '\0'; p++)
    {
        uint8_t d;
        if (*p >= '0' && *p <= '9')       d = (uint8_t)(*p - '0');
        else if (*p >= 'a' && *p <= 'f')  d = (uint8_t)(*p - 'a' + 10);
        else if (*p >= 'A' && *p <= 'F')  d = (uint8_t)(*p - 'A' + 10);
        else return false;
        v = (v << 4) | d;
    }

    txn_id = v;
    return true;
}

// 单个 Workflow 目录下暂存文件收集上限（防御异常残留撑爆数组）
#define WF_STG_SCAN_MAX 40u

struct StageScanCtx
{
    uint8_t wf;
    uint32_t publish_txn;   // 匹配该 txn_id 的暂存文件被发布；其余删除
    int matched_count;      // 已发布数量
    char names[WF_STG_SCAN_MAX][48];  // 暂存文件名副本
    uint8_t count;
};

static bool stage_scan_cb(
    const char *name,
    bool is_dir,
    void *user
)
{
    if (is_dir)
    {
        return true;
    }

    StageScanCtx *ctx = (StageScanCtx *)user;
    uint32_t id;

    if (!parse_stage_txn_id(name, id))
    {
        return true;  // 正式 step*.bin 文件，跳过
    }

    // 只收集名字副本，不在遍历回调中 rename/remove：
    // LittleFS readdir 在回调里 rename 会破坏目录游标
    // （实测 6 个暂存只发布 1 个，剩余 5 个残留）。
    if (ctx->count < WF_STG_SCAN_MAX)
    {
        snprintf(ctx->names[ctx->count], 48u, "%s", name);
        ctx->count++;
    }

    return true;
}

// 处理单个 Workflow 目录下的全部暂存文件：
//   publish_txn != UINT32_MAX：把 id == publish_txn 的发布、其余删除
//   publish_txn == UINT32_MAX：全部删除（未提交残留 / 无效 Workflow）
//
// 两阶段：先 foreach 收集文件名副本，遍历结束后统一 rename/remove，
// 避免遍历回调中 rename 破坏 LittleFS 目录游标导致发布不完整。
static void stage_process(
    uint8_t wf,
    uint32_t publish_txn
)
{
    char dir[48];
    if (!build_workflow_dir(wf, dir, sizeof(dir)))
    {
        return;
    }
    if (!bin_storage_exists(dir))
    {
        return;
    }

    StageScanCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.wf = wf;
    ctx.publish_txn = publish_txn;

    bin_storage_foreach(dir, stage_scan_cb, &ctx);

    // ---- 遍历结束，统一处理收集到的暂存文件 ----
    for (int i = 0; i < (int)ctx.count; i++)
    {
        uint32_t id;
        if (!parse_stage_txn_id(ctx.names[i], id))
        {
            continue;
        }

        char full[64];
        snprintf(full, sizeof(full), "%s/wf%02u/%s",
                 WF_STG_DIR, (unsigned)wf, ctx.names[i]);

        if (id == publish_txn)
        {
            // 已提交事务的暂存文件 → 发布（rename 覆盖正式文件，
            // LittleFS rename 具备原子替换语义）
            unsigned st = 0;
            if (sscanf(ctx.names[i], "step%2u", &st) != 1 ||
                st >= WF_STG_MAX_STEP)
            {
                continue;
            }

            char base[64];
            snprintf(base, sizeof(base), "%s/wf%02u/step%02u.bin",
                     WF_STG_DIR, (unsigned)wf, st);

            if (bin_storage_rename(full, base) != BIN_STORAGE_OK)
            {
                Serial.printf("[WorkflowStorage] recover publish rename failed: %s\n", full);

                // ---- Phase 7-1 埋点：LOG_STG_ATOMIC_WRITE_FAILED（ERROR）----
                //
                // 位置：stage_process() 中"已提交事务的暂存文件 → 正式文件"的
                //       rename 失败分支。
                // ★ 一个埋点覆盖两条路径（stage_process 为二者共用）：
                //   ① workflow_storage_recover_internal()（启动期自动恢复，本处）
                //   ② workflow_storage_save() 发布/清理阶段（:1399/:1434/:1452/:1466/:1502/:1535）
                // ★ 语义：**事务已提交但发布未完成** ⇒ 正式文件保持旧版本
                //   ⇒ 内容静默停留在上一版（数据可见偏差，非数据丢失）。
                // ★ Level=ERROR（与 log_events.h:244 对 0x0302 的标注一致）。
                // ★ ERR_CODE 复用既有 WF_STG_ERR_RENAME_FAILED(9)，不新增错误码。
                // ★ PATH 沿用本模块 6-B 起的"路径标识"空间（1=meta）；此处为
                //   step 暂存/正式文件路径 ⇒ 取 2（与 6-B 的 WF_STG_LOG_PATH_META=1 同空间，
                //   不新增任何常量声明）。
                // ★ STAGE 携带解析出的 step 序号 st；SLOT 语义已由 wf 承载 ⇒ 用 STAGE。
                // ★ 无 static / 无 String / 无堆。
                {
                    LogParamIn p[4];
                    p[0] = log_arg_u32(LOG_P_MODULE,   WF_STG_LOG_MODULE);
                    p[1] = log_arg_u32(LOG_P_PATH,     2u);
                    p[2] = log_arg_u32(LOG_P_ERR_CODE, (uint32_t)WF_STG_ERR_RENAME_FAILED);
                    p[3] = log_arg_u32(LOG_P_STAGE,    (uint32_t)st);
                    log_emit(LOG_STG_ATOMIC_WRITE_FAILED, LOG_LVL_ERROR, p, 4);
                }
            }
            else
            {
                ctx.matched_count++;
            }
        }
        else
        {
            // 未提交事务的残留暂存文件 → 丢弃
            bin_storage_remove(full);
        }
    }
}

// =====================================================
// 对外实现
// =====================================================

WorkflowStorageResult workflow_storage_from_bin_result(
    BinStorageResult result
)
{
    switch (result)
    {
        case BIN_STORAGE_OK:                    return WF_STG_OK;
        case BIN_STORAGE_ERR_NOT_INITIALIZED:   return WF_STG_ERR_NOT_INITIALIZED;
        case BIN_STORAGE_ERR_INVALID_ARGUMENT:  return WF_STG_ERR_INVALID_ARGUMENT;
        case BIN_STORAGE_ERR_NOT_FOUND:         return WF_STG_ERR_NOT_FOUND;
        case BIN_STORAGE_ERR_BUFFER_TOO_SMALL:  return WF_STG_ERR_BUFFER_TOO_SMALL;
        case BIN_STORAGE_ERR_OPEN_FAILED:       return WF_STG_ERR_OPEN_FAILED;
        case BIN_STORAGE_ERR_READ_FAILED:       return WF_STG_ERR_READ_FAILED;
        case BIN_STORAGE_ERR_WRITE_FAILED:      return WF_STG_ERR_WRITE_FAILED;
        case BIN_STORAGE_ERR_DELETE_FAILED:     return WF_STG_ERR_DELETE_FAILED;
        case BIN_STORAGE_ERR_RENAME_FAILED:     return WF_STG_ERR_RENAME_FAILED;
        default:                                return WF_STG_ERR_READ_FAILED;
    }
}

const char *workflow_storage_result_name(
    WorkflowStorageResult result
)
{
    switch (result)
    {
        case WF_STG_OK:                     return "OK";
        case WF_STG_ERR_NOT_INITIALIZED:    return "NOT_INITIALIZED";
        case WF_STG_ERR_INVALID_ARGUMENT:   return "INVALID_ARGUMENT";
        case WF_STG_ERR_NOT_FOUND:          return "NOT_FOUND";
        case WF_STG_ERR_BUFFER_TOO_SMALL:   return "BUFFER_TOO_SMALL";
        case WF_STG_ERR_OPEN_FAILED:        return "OPEN_FAILED";
        case WF_STG_ERR_READ_FAILED:        return "READ_FAILED";
        case WF_STG_ERR_WRITE_FAILED:       return "WRITE_FAILED";
        case WF_STG_ERR_DELETE_FAILED:      return "DELETE_FAILED";
        case WF_STG_ERR_RENAME_FAILED:      return "RENAME_FAILED";
        case WF_STG_ERR_CRC_FAILED:         return "CRC_FAILED";
        case WF_STG_ERR_VERSION_MISMATCH:   return "VERSION_MISMATCH";
        case WF_STG_ERR_FORMAT_INVALID:     return "FORMAT_INVALID";
        case WF_STG_ERR_VERSION_TOO_NEW:    return "VERSION_TOO_NEW";
        case WF_STG_ERR_TEST_ABORTED:       return "TEST_ABORTED";
        default:                            return "UNKNOWN";
    }
}

bool workflow_storage_init()
{
    if (s_initialized)
    {
        return true;
    }

    if (!bin_storage_init())
    {
        return false;
    }

    if (bin_storage_mkdir(WF_STG_DIR) != BIN_STORAGE_OK)
    {
        return false;
    }

    reset_meta();
    s_initialized = true;
    s_meta_loaded = false;
    return true;
}

// -----------------------------------------------------
// Meta
// -----------------------------------------------------

// 依据 RAM Meta 处理全部 Workflow 的暂存文件：
//   valid     → 发布 txn_id 匹配的暂存（提交后掉电恢复）
//   invalid   → 删除全部暂存（不复活已删除的 Workflow）
// 收集 /workflow 下实际存在的 wfNN 目录
//
// 用途：恢复扫描只处理真实存在的目录。
//   ⚠️ 不能改成"逐个 wfNN 调 bin_storage_exists()"——
//   ESP32 Arduino core 的 FS::exists() 内部是 open(path,"r")，
//   路径不存在时 vfs_api 会直接打印
//   "[E][vfs_api.cpp] ... does not exist, no permits for creation"。
//   启动时 16 个槽位会有 13 条噪音错误日志，淹没真实故障。
struct WfDirCtx
{
    bool present[WF_STG_MAX_COUNT];
};

static bool wf_dir_cb(
    const char *name,
    bool is_dir,
    void *user
)
{
    if (!is_dir)
    {
        return true;
    }

    WfDirCtx *ctx = (WfDirCtx *)user;
    unsigned wf = 0;

    if (sscanf(name, "wf%2u", &wf) == 1 && wf < WF_STG_MAX_COUNT)
    {
        ctx->present[wf] = true;
    }

    return true;
}

static void workflow_storage_recover_internal()
{
    WfDirCtx ctx;
    memset(&ctx, 0, sizeof(ctx));

    bin_storage_foreach(WF_STG_DIR, wf_dir_cb, &ctx);

    for (uint8_t wf = 0; wf < WF_STG_MAX_COUNT; wf++)
    {
        // 目录不存在 → 不可能有暂存文件，跳过（同时避免 exists() 噪音）
        if (!ctx.present[wf])
        {
            continue;
        }

        if (s_meta[wf].valid)
        {
            stage_process(wf, s_meta[wf].txn_id);
        }
        else
        {
            stage_process(wf, UINT32_MAX);
        }
    }
}

// 清理全部存在目录下的暂存文件（无 Meta / Meta 损坏时用）
static void stage_cleanup_all()
{
    WfDirCtx ctx;
    memset(&ctx, 0, sizeof(ctx));

    bin_storage_foreach(WF_STG_DIR, wf_dir_cb, &ctx);

    for (uint8_t wf = 0; wf < WF_STG_MAX_COUNT; wf++)
    {
        if (ctx.present[wf])
        {
            stage_process(wf, UINT32_MAX);
        }
    }
}

bool workflow_storage_load_meta()
{
    if (!s_initialized)
    {
        return false;
    }

    reset_meta();

    if (!bin_storage_exists(WF_STG_META_PATH))
    {
        // 首次启动：没有 meta.bin 属正常，全部 Workflow 视为 Invalid。
        // 清掉可能的历史暂存残留（没有 Meta 就没有可恢复的事务）。
        stage_cleanup_all();
        s_meta_loaded = true;
        return true;
    }

    size_t file_size = bin_storage_size(WF_STG_META_PATH);
    if (file_size == 0 || file_size > WF_STG_META_BIN_MAX)
    {
        stage_cleanup_all();
        s_meta_loaded = true;

        // ---- Phase 6-C-1 埋点：LOG_STG_READ_FAILED（ERROR）----
        //
        // 位置：meta 文件尺寸非法分支（file_size == 0 || > WF_STG_META_BIN_MAX）。
        // ★ ERR_CODE 复用既有枚举值 WF_STG_ERR_FORMAT_INVALID(12)：
        //   其定义即"Magic 错误 / 长度非法"，与本分支的"长度非法"语义一致；
        //   file_size == 0 也落在"0 不在 [1,1536]"这一"长度非法"范围内。
        //   与同文件 deserialize_step_meta() 的同类检查（尺寸越界 → FORMAT_INVALID）保持同一约定。
        // ★ 不新增枚举值 / 不新增 ParamId / 无 static / 无 String / 无堆。
        // ★ 仅启动期触发一次 ⇒ 无频率风险、无需门控。
        {
            LogParamIn p[3];
            p[0] = log_arg_u32(LOG_P_MODULE,   WF_STG_LOG_MODULE);
            p[1] = log_arg_u32(LOG_P_ERR_CODE, (uint32_t)WF_STG_ERR_FORMAT_INVALID);
            p[2] = log_arg_u32(LOG_P_PATH,     WF_STG_LOG_PATH_META);
            log_emit(LOG_STG_READ_FAILED, LOG_LVL_ERROR, p, 3);
        }
        return false;
    }

    uint8_t buf[WF_STG_META_BIN_MAX];
    size_t bytes_read = 0;
    if (bin_storage_read(WF_STG_META_PATH, buf, file_size, bytes_read) != BIN_STORAGE_OK)
    {
        s_meta_loaded = true;

        // ---- Phase 6-C-2 埋点：LOG_STG_READ_FAILED（ERROR）----
        //
        // 位置：bin_storage_read() 返回非 BIN_STORAGE_OK 的失败分支。
        // ★ ERR_CODE 复用既有枚举值 WF_STG_ERR_READ_FAILED(6)：
        //   语义即"meta BIN 读取失败"，为该事实在 WF_STG_ERR_* 中最直接的对应值。
        // ★ 本分支未捕获 bin_storage_read() 的具体返回码（保持既有 if 语句不变、
        //   不改变控制流）⇒ ERR_CODE 为通用"读取失败"，不含 BIN_STORAGE_ERR_* 细分。
        //   若要细分（NOT_INITIALIZED / OPEN_FAILED / …），需捕获返回值并经
        //   workflow_storage_from_bin_result() 映射 —— 属可选增强，本轮不做。
        // ★ 无 static / 无 String / 无堆；仅启动期触发一次 ⇒ 无需门控。
        {
            LogParamIn p[3];
            p[0] = log_arg_u32(LOG_P_MODULE,   WF_STG_LOG_MODULE);
            p[1] = log_arg_u32(LOG_P_ERR_CODE, (uint32_t)WF_STG_ERR_READ_FAILED);
            p[2] = log_arg_u32(LOG_P_PATH,     WF_STG_LOG_PATH_META);
            log_emit(LOG_STG_READ_FAILED, LOG_LVL_ERROR, p, 3);
        }
        return false;
    }
    if (bytes_read != file_size)
    {
        s_meta_loaded = true;

        // ---- Phase 6-C-3 埋点：LOG_STG_READ_FAILED（ERROR）----
        //
        // 位置：读取长度不足分支（bytes_read != file_size）。
        // ★ ERR_CODE 复用既有枚举值 WF_STG_ERR_READ_FAILED(6)：
        //   与同文件 deserialize_step_meta() 中完全相同的检查
        //   （`bytes_read != file_size` → WF_STG_ERR_READ_FAILED）保持同一约定。
        // ★ 与 6-C-2 同为 READ_FAILED(6) —— 二者语义同族（meta BIN 未被完整读出），
        //   仅靠原分支位置区分；如需在云端日志中区分，须细化 ERR_CODE（见交付说明）。
        // ★ 无 static / 无 String / 无堆；仅启动期触发一次 ⇒ 无需门控。
        {
            LogParamIn p[3];
            p[0] = log_arg_u32(LOG_P_MODULE,   WF_STG_LOG_MODULE);
            p[1] = log_arg_u32(LOG_P_ERR_CODE, (uint32_t)WF_STG_ERR_READ_FAILED);
            p[2] = log_arg_u32(LOG_P_PATH,     WF_STG_LOG_PATH_META);
            log_emit(LOG_STG_READ_FAILED, LOG_LVL_ERROR, p, 3);
        }
        return false;
    }

    WorkflowStorageResult r = deserialize_meta(buf, bytes_read);
    s_meta_loaded = true;

    if (r != WF_STG_OK)
    {
        // 损坏时绝不猜测旧格式，全部保持 Invalid，并清历史暂存
        stage_cleanup_all();

        // ---- Phase 6-B-1 埋点：LOG_STG_CRC_FAILED（CRITICAL）----
        //
        // ★ 跨域复用 Storage 段 ID：meta 是 BIN 持久化数据，其损坏属于
        //   "存储完整性失败"而非 Workflow 业务错误 ⇒ 复用 0x0303，不新增 WF EventId。
        // ★ 精确原因由 LOG_P_ERR_CODE 携带（deserialize_meta 的返回码原值）：
        //     10=WF_STG_ERR_CRC_FAILED  11=VERSION_MISMATCH
        //     12=WF_STG_ERR_FORMAT_INVALID  13=VERSION_TOO_NEW
        // ★ 此分支此前**连串口都不打**：meta 损坏 ⇒ s_meta_loaded=true + 全部条目
        //   保持 Invalid ⇒ 上层 workflow_load_from_storage() 失败 ⇒
        //   main.cpp 静默回退 JSON 路径 ⇒ 全部 Workflow 消失且无任何因果记录。
        // ★ 只在启动期触发一次 ⇒ 无频率风险、无需门控。
        // ★ 无 String / 无堆：路径用本模块私有标识（见上方常量区）。
        {
            LogParamIn p[3];
            p[0] = log_arg_u32(LOG_P_MODULE,   WF_STG_LOG_MODULE);
            p[1] = log_arg_u32(LOG_P_ERR_CODE, (uint32_t)r);
            p[2] = log_arg_u32(LOG_P_PATH,     WF_STG_LOG_PATH_META);
            log_emit(LOG_STG_CRC_FAILED, LOG_LVL_CRITICAL, p, 3);
        }
        return false;
    }

    // 加载成功 → 自动完成"提交后掉电"的恢复发布
    workflow_storage_recover_internal();

    return true;
}

bool workflow_storage_recover()
{
    if (!s_initialized)
    {
        return false;
    }

    if (!s_meta_loaded)
    {
        workflow_storage_load_meta();
    }

    workflow_storage_recover_internal();
    return true;
}

bool workflow_storage_save_meta()
{
    if (!s_initialized)
    {
        return false;
    }

    uint8_t buf[WF_STG_META_BIN_MAX];
    size_t size = serialize_meta(buf);

    return (bin_storage_write_atomic(WF_STG_META_PATH, buf, size) == BIN_STORAGE_OK);
}

bool workflow_storage_get_valid(
    uint8_t workflow_id
)
{
    if (workflow_id >= WF_STG_MAX_COUNT || !s_meta_loaded)
    {
        return false;
    }
    return s_meta[workflow_id].valid != 0;
}

void workflow_storage_set_valid(
    uint8_t workflow_id,
    bool valid
)
{
    if (workflow_id >= WF_STG_MAX_COUNT)
    {
        return;
    }
    s_meta[workflow_id].valid = valid ? 1u : 0u;
    s_meta[workflow_id].version = WF_STG_META_VERSION;
}

uint8_t workflow_storage_get_step_count(
    uint8_t workflow_id
)
{
    if (workflow_id >= WF_STG_MAX_COUNT)
    {
        return 0;
    }
    return (uint8_t)s_meta[workflow_id].step_count;
}

bool workflow_storage_get_info(
    uint8_t workflow_id,
    char *id_out,
    size_t id_out_size,
    char *name_out,
    size_t name_out_size,
    bool &enable_out,
    uint32_t &timeout_ms_out
)
{
    if (workflow_id >= WF_STG_MAX_COUNT || !s_meta_loaded)
    {
        return false;
    }

    const WorkflowMetaEntry &e = s_meta[workflow_id];

    if (id_out != nullptr && id_out_size > 0)
    {
        strncpy(id_out, e.id, id_out_size - 1);
        id_out[id_out_size - 1] = '\0';
    }
    if (name_out != nullptr && name_out_size > 0)
    {
        strncpy(name_out, e.name, name_out_size - 1);
        name_out[name_out_size - 1] = '\0';
    }

    enable_out = e.enable != 0;
    timeout_ms_out = e.timeout_ms;
    return true;
}

void workflow_storage_set_info(
    uint8_t workflow_id,
    const char *id,
    const char *name,
    bool enable,
    uint32_t timeout_ms
)
{
    if (workflow_id >= WF_STG_MAX_COUNT)
    {
        return;
    }

    WorkflowMetaEntry &e = s_meta[workflow_id];
    strncpy(e.id, id != nullptr ? id : "", WF_STG_ID_MAX_LEN - 1);
    e.id[WF_STG_ID_MAX_LEN - 1] = '\0';
    strncpy(e.name, name != nullptr ? name : "", WF_STG_NAME_MAX_LEN - 1);
    e.name[WF_STG_NAME_MAX_LEN - 1] = '\0';
    e.enable = enable ? 1u : 0u;
    e.timeout_ms = timeout_ms;
    e.version = WF_STG_META_VERSION;
}

uint32_t workflow_storage_get_variant(
    uint8_t workflow_id
)
{
    if (workflow_id >= WF_STG_MAX_COUNT || !s_meta_loaded)
    {
        return 0;
    }
    return s_meta[workflow_id].variant;
}

void workflow_storage_set_variant(
    uint8_t workflow_id,
    uint32_t variant
)
{
    if (workflow_id >= WF_STG_MAX_COUNT)
    {
        return;
    }
    s_meta[workflow_id].variant = variant;
    s_meta[workflow_id].version = WF_STG_META_VERSION;
}

// -----------------------------------------------------
// Step
// -----------------------------------------------------

WorkflowStorageResult workflow_storage_save_step(
    uint8_t workflow_id,
    uint8_t step_id,
    const WorkflowStepDefinition *step
)
{
    if (!s_initialized || step == nullptr)
    {
        return WF_STG_ERR_INVALID_ARGUMENT;
    }
    if (workflow_id >= WF_STG_MAX_COUNT || step_id >= WF_STG_MAX_STEP)
    {
        return WF_STG_ERR_INVALID_ARGUMENT;
    }

    char dir[48];
    if (!build_workflow_dir(workflow_id, dir, sizeof(dir)))
    {
        return WF_STG_ERR_INVALID_ARGUMENT;
    }
    if (bin_storage_mkdir(dir) != BIN_STORAGE_OK)
    {
        return WF_STG_ERR_OPEN_FAILED;
    }

    char path[64];
    if (!build_step_path(workflow_id, step_id, path, sizeof(path)))
    {
        return WF_STG_ERR_INVALID_ARGUMENT;
    }

    uint8_t buf[WF_STG_STEP_BIN_MAX];
    size_t payload_size = serialize_step_payload(
        step, workflow_id, step_id, buf + WF_STG_STEP_HEADER_SIZE
    );

    uint32_t crc = wf_crc32_final(
        wf_crc32_update(wf_crc32_init(), buf + WF_STG_STEP_HEADER_SIZE, payload_size)
    );

    size_t pos = 0;
    put_u32(buf, pos, WF_STG_STEP_MAGIC);
    put_u16(buf, pos, WF_STG_VERSION);
    put_u16(buf, pos, WF_STG_STEP_HEADER_SIZE);
    put_u32(buf, pos, (uint32_t)payload_size);
    put_u32(buf, pos, crc);

    size_t total = WF_STG_STEP_HEADER_SIZE + payload_size;

    return workflow_storage_from_bin_result(
        bin_storage_write_atomic(path, buf, total)
    );
}

WorkflowStorageResult workflow_storage_load_step(
    uint8_t workflow_id,
    uint8_t step_id,
    WorkflowStepDefinition *step
)
{
    if (!s_initialized || step == nullptr)
    {
        return WF_STG_ERR_INVALID_ARGUMENT;
    }
    return load_step_internal(workflow_id, step_id, step, nullptr);
}

// -----------------------------------------------------
// Workflow
// -----------------------------------------------------

WorkflowStorageResult workflow_storage_load(
    uint8_t workflow_id,
    WorkflowDefinition *definition
)
{
    if (!s_initialized || definition == nullptr)
    {
        return WF_STG_ERR_INVALID_ARGUMENT;
    }
    if (workflow_id >= WF_STG_MAX_COUNT)
    {
        return WF_STG_ERR_INVALID_ARGUMENT;
    }

    if (!s_meta_loaded)
    {
        workflow_storage_load_meta();
    }

    const WorkflowMetaEntry &e = s_meta[workflow_id];
    if (!e.valid)
    {
        return WF_STG_ERR_NOT_FOUND;
    }
    if (e.step_count == 0 || e.step_count > WF_STG_MAX_STEP)
    {
        return WF_STG_ERR_FORMAT_INVALID;
    }

    memset(definition, 0, sizeof(*definition));

    strncpy(definition->id, e.id, WF_STG_ID_MAX_LEN - 1);
    strncpy(definition->name, e.name, WF_STG_NAME_MAX_LEN - 1);
    definition->enable = e.enable;
    definition->timeout_ms = e.timeout_ms;
    definition->step_count = (uint8_t)e.step_count;
    // variant 必须回填（Meta v3）：
    // 漏掉会导致重启后 variant 归 0 → 被上层提升为 1，
    // 云端看来"版本倒退"，永远同步不到正确内容。
    definition->variant = e.variant;

    // 只加载 0 .. step_count-1，绝不读取 step_count .. 15
    uint32_t crc = wf_crc32_init();
    for (uint8_t i = 0; i < definition->step_count; i++)
    {
        uint32_t step_crc = 0;
        WorkflowStorageResult r = load_step_internal(
            workflow_id, i, &definition->steps[i], &step_crc
        );
        if (r != WF_STG_OK)
        {
            return r;
        }
        crc_append_u32_le(crc, step_crc);
    }
    crc = wf_crc32_final(crc);

    // CRC32 合法结果可能是 0，因此【始终】校验，
    // 不得用 "crc32 != 0 &&" 表示"有没有 CRC"。
    if (crc != e.crc32)
    {
        return WF_STG_ERR_CRC_FAILED;
    }

    return WF_STG_OK;
}

WorkflowStorageResult workflow_storage_save(
    uint8_t workflow_id,
    const WorkflowDefinition *definition
)
{
    if (!s_initialized || definition == nullptr)
    {
        return WF_STG_ERR_INVALID_ARGUMENT;
    }
    if (workflow_id >= WF_STG_MAX_COUNT)
    {
        return WF_STG_ERR_INVALID_ARGUMENT;
    }
    if (definition->step_count == 0 || definition->step_count > WF_STG_MAX_STEP)
    {
        return WF_STG_ERR_INVALID_ARGUMENT;
    }

    // 故障注入：指定 Workflow 的整次 save 失败（一次性，用于验证
    // "部分成功不得清空全部 Dirty"）。放在参数校验之后，
    // 模拟的是"真正开始写盘时才失败"。
    if (s_test_fail_wf >= 0 && s_test_fail_wf == (int8_t)workflow_id)
    {
        s_test_fail_wf = -1;
        Serial.printf(
            "[WFStg] TEST fail_wf injected: wf=%u\n",
            (unsigned)workflow_id
        );
        return WF_STG_ERR_WRITE_FAILED;
    }

    if (!s_meta_loaded)
    {
        workflow_storage_load_meta();
    }

    char dir[48];
    if (!build_workflow_dir(workflow_id, dir, sizeof(dir)))
    {
        return WF_STG_ERR_INVALID_ARGUMENT;
    }
    if (bin_storage_mkdir(dir) != BIN_STORAGE_OK)
    {
        return WF_STG_ERR_OPEN_FAILED;
    }

    // 事务标识：基于当前 Meta.txn_id 递增。
    // 文件名携带该标识 → 掉电后能判断暂存文件属于"已提交事务"还是"废弃残留"。
    const uint32_t txn_id = s_meta[workflow_id].txn_id + 1u;

    uint8_t buf[WF_STG_STEP_BIN_MAX];

    // ---- 1. 逐个 Step 写入【暂存文件】.tX（不触碰正式文件）----
    uint32_t crc = wf_crc32_init();

    for (uint8_t i = 0; i < definition->step_count; i++)
    {
        // 故障注入：让第 N 步写入失败（仅测试）
        if (s_test_fail_step == (int8_t)i)
        {
            s_test_fail_step = -1;
            stage_process(workflow_id, UINT32_MAX);  // 清掉本次已写的暂存
            Serial.printf("[WorkflowStorage] test: injected write failure at step %u\n",
                          (unsigned)i);
            return WF_STG_ERR_WRITE_FAILED;
        }

        size_t payload_size = serialize_step_payload(
            &definition->steps[i], workflow_id, i, buf + WF_STG_STEP_HEADER_SIZE
        );

        uint32_t step_crc = wf_crc32_final(
            wf_crc32_update(wf_crc32_init(), buf + WF_STG_STEP_HEADER_SIZE, payload_size)
        );
        crc_append_u32_le(crc, step_crc);

        size_t pos = 0;
        put_u32(buf, pos, WF_STG_STEP_MAGIC);
        put_u16(buf, pos, WF_STG_VERSION);
        put_u16(buf, pos, WF_STG_STEP_HEADER_SIZE);
        put_u32(buf, pos, (uint32_t)payload_size);
        put_u32(buf, pos, step_crc);

        size_t total = WF_STG_STEP_HEADER_SIZE + payload_size;

        char stage[72];
        if (!build_stage_path(workflow_id, i, txn_id, stage, sizeof(stage)))
        {
            return WF_STG_ERR_INVALID_ARGUMENT;
        }

        WorkflowStorageResult r = workflow_storage_from_bin_result(
            bin_storage_write(stage, buf, total)
        );
        if (r != WF_STG_OK)
        {
            stage_process(workflow_id, UINT32_MAX);
            return r;
        }

        // 读回校验（与 load_step_internal 一致）：
        //   重读整个暂存文件，解析后对 payload 重算 CRC，与写入的 step_crc 比对。
        //   ⚠️ 不能用 bin_storage_crc32()——它算的是【整个文件】的 CRC，
        //   而 step_crc 仅覆盖 payload（header 之后的部分），两者必然不等。
        {
            uint8_t rbuf[WF_STG_STEP_BIN_MAX];
            size_t rbytes = 0;
            WorkflowStorageResult rr = workflow_storage_from_bin_result(
                bin_storage_read(stage, rbuf, total, rbytes)
            );
            if (rr != WF_STG_OK || rbytes != total)
            {
                Serial.printf("[WorkflowStorage] save readback failed: %s rr=%d\n",
                              stage, (int)rr);
                stage_process(workflow_id, UINT32_MAX);
                return WF_STG_ERR_WRITE_FAILED;
            }
            uint32_t rb_crc = wf_crc32_final(
                wf_crc32_update(
                    wf_crc32_init(),
                    rbuf + WF_STG_STEP_HEADER_SIZE,
                    payload_size
                )
            );
            if (rb_crc != step_crc)
            {
                Serial.printf("[WorkflowStorage] save crc mismatch: %s disk=%u expect=%u\n",
                              stage, (unsigned)rb_crc, (unsigned)step_crc);
                stage_process(workflow_id, UINT32_MAX);
                return WF_STG_ERR_WRITE_FAILED;
            }
        }
    }
    crc = wf_crc32_final(crc);

    // 故障注入：预提交中断（模拟"暂存写完、Meta 未提交"时掉电）
    if (s_test_abort_phase == 1)
    {
        s_test_abort_phase = 0;
        Serial.println("[WorkflowStorage] test: aborted PRE-COMMIT (power loss simulated)");
        return WF_STG_ERR_TEST_ABORTED;
    }

    // ---- 2. 原子提交 Meta（事务提交点）----
    WorkflowMetaEntry &e = s_meta[workflow_id];
    e.valid = 1u;
    e.version = WF_STG_META_VERSION;
    e.step_count = definition->step_count;
    e.update_time = (uint32_t)time(nullptr);
    e.crc32 = crc;
    e.txn_id = txn_id;
    strncpy(e.id, definition->id, WF_STG_ID_MAX_LEN - 1);
    e.id[WF_STG_ID_MAX_LEN - 1] = '\0';
    strncpy(e.name, definition->name, WF_STG_NAME_MAX_LEN - 1);
    e.name[WF_STG_NAME_MAX_LEN - 1] = '\0';
    e.enable = definition->enable ? 1u : 0u;
    e.timeout_ms = definition->timeout_ms;
    e.variant = definition->variant;

    if (!workflow_storage_save_meta())
    {
        // Meta 提交失败：事务未提交，清掉本次暂存，正式文件保持旧版本
        Serial.printf("[WorkflowStorage] save: meta commit failed (wf=%u)\n",
                      (unsigned)workflow_id);
        stage_process(workflow_id, UINT32_MAX);
        return WF_STG_ERR_WRITE_FAILED;
    }

    // 故障注入：提交后中断（模拟"Meta 已提交、尚未发布"时掉电）
    if (s_test_abort_phase == 2)
    {
        s_test_abort_phase = 0;
        Serial.println("[WorkflowStorage] test: aborted POST-COMMIT (power loss simulated)");
        return WF_STG_ERR_TEST_ABORTED;
    }

    // ---- 3. 发布：逐个 rename 暂存 → 正式文件 ----
    for (uint8_t i = 0; i < definition->step_count; i++)
    {
        char stage[72];
        char final[64];
        if (!build_stage_path(workflow_id, i, txn_id, stage, sizeof(stage)) ||
            !build_step_path(workflow_id, i, final, sizeof(final)))
        {
            return WF_STG_ERR_INVALID_ARGUMENT;
        }

        if (bin_storage_rename(stage, final) != BIN_STORAGE_OK)
        {
            // 事务【已提交】，但发布未完成：
            // 不清剩余暂存 —— 留给 recover()（下次启动自动执行）完成发布
            Serial.printf("[WorkflowStorage] publish rename failed: %s\n", stage);
            return WF_STG_ERR_RENAME_FAILED;
        }
    }

    // ---- 4. 清理历史暂存残留（发布已全部完成，可安全删除）----
    stage_process(workflow_id, UINT32_MAX);

    return WF_STG_OK;
}

WorkflowStorageResult workflow_storage_delete(
    uint8_t workflow_id
)
{
    if (!s_initialized)
    {
        return WF_STG_ERR_INVALID_ARGUMENT;
    }
    if (workflow_id >= WF_STG_MAX_COUNT)
    {
        return WF_STG_ERR_INVALID_ARGUMENT;
    }

    if (!s_meta_loaded)
    {
        workflow_storage_load_meta();
    }

    // 只置 Invalid，不删除任何 Step BIN
    s_meta[workflow_id].valid = 0u;
    s_meta[workflow_id].version = WF_STG_META_VERSION;

    if (!workflow_storage_save_meta())
    {
        return WF_STG_ERR_WRITE_FAILED;
    }

    return WF_STG_OK;
}

// =====================================================
// WorkflowDefinition 缓冲分配
// =====================================================
//
// WorkflowDefinition ~8.6KB（见头文件说明），栈上声明会击穿
// Arduino loopTask 8KB 栈（已实测栈溢出重启循环）。必须堆分配。
// 优先 PSRAM（8MB 充足），分配失败回退内部 RAM。

WorkflowDefinition *workflow_storage_alloc_definition(void)
{
    void *p = heap_caps_malloc(
        sizeof(WorkflowDefinition), MALLOC_CAP_SPIRAM);

    if (p == NULL)
    {
        // PSRAM 不可用 / 不足时回退内部 RAM（MALLOC_CAP_8BIT）
        p = heap_caps_malloc(
            sizeof(WorkflowDefinition), MALLOC_CAP_8BIT);
    }

    return (WorkflowDefinition *)p;
}

void workflow_storage_free_definition(
    WorkflowDefinition *definition
)
{
    if (definition != NULL)
    {
        // heap_caps_malloc 分配的内存统一用 heap_caps_free 释放
        heap_caps_free(definition);
    }
}

// =====================================================
// 故障注入（仅测试用）
// =====================================================

void workflow_storage_test_fail_step(int step)
{
    s_test_fail_step = (step >= 0 && step < WF_STG_MAX_STEP) ? (int8_t)step : (int8_t)-1;
}

void workflow_storage_test_fail_wf(int wf)
{
    s_test_fail_wf = (wf >= 0 && wf < (int)WF_STG_MAX_COUNT) ? (int8_t)wf : (int8_t)-1;
    Serial.printf(
        "[WFStg] TEST fail_wf armed: wf=%d\n",
        (int)s_test_fail_wf
    );
}

void workflow_storage_test_abort_phase(int phase)
{
    s_test_abort_phase = (phase == 1 || phase == 2) ? phase : 0;
}

int workflow_storage_test_staged_count(
    uint8_t workflow_id
)
{
    if (workflow_id >= WF_STG_MAX_COUNT)
    {
        return -1;
    }

    char dir[48];
    if (!build_workflow_dir(workflow_id, dir, sizeof(dir)))
    {
        return -1;
    }
    if (!bin_storage_exists(dir))
    {
        return 0;
    }

    struct CountCtx
    {
        int count;
    } ctx;
    ctx.count = 0;

    bin_storage_foreach(
        dir,
        [](const char *name, bool is_dir, void *user) -> bool
        {
            (void)is_dir;
            uint32_t id;
            if (parse_stage_txn_id(name, id))
            {
                ((CountCtx *)user)->count++;
            }
            return true;
        },
        &ctx
    );

    return ctx.count;
}
