#include "services/topic_renderer.h"

#include "services/device_identity.h"

// =====================================================
// 常量
// =====================================================
// MQTT topic / client_id 长度上限（保守值；EMQX 侧实际上限见设计 §1.4.1，P1 实测）
static const size_t kRenderedMaxLen = 128u;

// =====================================================
// 内部工具
// =====================================================

// 通用字符合法性检查（**仅做设计 §3.3 规则 4 规定的四项之一**）：
//   · 残留的 < >   ⇒ 说明占位符写法错误（如 <dev_id>），绝不能用它去发布
//   · 控制字符     ⇒ MQTT topic 不允许
// 注意：严格的 [0-9a-zA-Z_-] 字符集**只用于 client_id**，由调用方在 P0-3 校验；
//       此处刻意不做通配符/点号等更严检查，避免误杀合法 topic（清单 P0-2 风险 ④）。
static bool has_illegal_char(const String& s)
{
    for(size_t i = 0; i < s.length(); i++)
    {
        const unsigned char c = (unsigned char)s[i];

        if(c == '<' || c == '>')
        {
            return true;
        }

        if(c < 0x20u || c == 0x7Fu)
        {
            return true;
        }
    }

    return false;
}

// =====================================================
// 渲染（渲染点全工程唯一 —— T-2）
// =====================================================
bool topic_render(const String& tmpl, String& out)
{
    out = "";

    // 规则 4（前置）：空模板 ⇒ 配置错误
    if(tmpl.length() == 0)
    {
        return false;
    }

    // 规则 3：device_id 不可用 ⇒ 返回 false。
    // 理由：既无法完成替换，内建兜底模板（同含 <device_id>）也依赖它 ⇒ 交给调用方判错。
    if(!device_id_valid())
    {
        return false;
    }

    const char* id = device_id();   // device_id_valid() 已保证非空且已初始化

    // 规则 1 / 2：含占位符 ⇒ 全部替换（replace 替换所有出现）；不含 ⇒ 原样放行（回滚通路）
    if(tmpl.indexOf(GF_DEVICE_ID_PLACEHOLDER) >= 0)
    {
        String rendered = tmpl;
        rendered.replace(GF_DEVICE_ID_PLACEHOLDER, String(id));
        out = rendered;
    }
    else
    {
        out = tmpl;
    }

    // 规则 4：非空 / 长度 / 字符
    if(out.length() == 0)
    {
        out = "";
        return false;
    }

    if(out.length() > kRenderedMaxLen)
    {
        out = "";
        return false;
    }

    if(has_illegal_char(out))
    {
        out = "";
        return false;
    }

    return true;
}

// =====================================================
// 自检（DEBUG-only；默认开关 0 ⇒ 整段不参与编译）
// =====================================================
#if GF_TOPIC_RENDER_SELFTEST

static int s_st_pass = 0;
static int s_st_fail = 0;

static void st_case(const char* name,
                    const String& tmpl,
                    bool want_ok,
                    const String& want_out)
{
    String out;
    const bool ok = topic_render(tmpl, out);

    bool pass = (ok == want_ok);

    if(pass && want_ok)
    {
        pass = (out == want_out);
    }

    if(pass)
    {
        s_st_pass++;
    }
    else
    {
        s_st_fail++;
        Serial.printf("[TopicRender]   FAIL %s: tmpl=\"%s\" ok=%d out=\"%s\" (want ok=%d out=\"%s\")\n",
                      name,
                      tmpl.c_str(),
                      (int)ok,
                      out.c_str(),
                      (int)want_ok,
                      want_out.c_str());
    }
}

void topic_renderer_selftest()
{
    s_st_pass = 0;
    s_st_fail = 0;

    const String id = String(device_id());

    Serial.printf("[TopicRender] running selftest (device_id=%s)\n", id.c_str());

    // 1) 标准模板（含占位符）⇒ 替换为实际 device_id
    st_case("tpl_down", String(GF_TOPIC_TPL_DOWN), true, String("guo_feeder/") + id + "/down");
    st_case("tpl_up",   String(GF_TOPIC_TPL_UP),   true, String("guo_feeder/") + id + "/up");
    st_case("tpl_log",  String(GF_TOPIC_TPL_LOG),  true, String("guo_feeder/") + id + "/log");
    st_case("tpl_cid",  String(GF_CLIENT_ID_TPL),  true, String("dev_") + id);

    // 2) 无占位符 ⇒ 原样放行（配置级回滚通路，**不可报错**）
    st_case("legacy",   String("guo_feeder/down"), true, String("guo_feeder/down"));

    // 3) 多次出现占位符 ⇒ 全部替换
    st_case("multi",    String("a/<device_id>/b/<device_id>"),
                        true, String("a/") + id + "/b/" + id);

    // 4) 空模板 ⇒ false
    st_case("empty",    String(""), false, String(""));

    // 5) 渲染后仍含 '<'（占位符写法错误，如 <dev_id>）⇒ false
    st_case("bad_ph",   String("guo_feeder/<dev_id>/down"), false, String(""));

    // 6) 超长（渲染后长度 > 128）⇒ false
    {
        String long_tpl = "guo_feeder/";

        for(int i = 0; i < 130; i++)
        {
            long_tpl += 'a';
        }

        st_case("too_long", long_tpl, false, String(""));
    }

    Serial.printf("[TopicRender] selftest %s (pass=%d fail=%d)\n",
                  (s_st_fail == 0) ? "PASS" : "FAIL",
                  s_st_pass,
                  s_st_fail);
}

#endif  // GF_TOPIC_RENDER_SELFTEST
