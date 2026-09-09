#include <Arduino.h>
#include <LittleFS.h>
#include <NimBLEDevice.h>
#include "system_state.h"
#include "system_command.h"
#include "json_storage.h"
#include "bin_storage.h"
#include "workflow_storage.h"
#include "config_manager.h"
#include "event_manager.h"
#include "oled.h"
#include "command_manager.h"
#include "wifi_module.h"
#include "time_manager.h"
#include "cloud_manager.h"
#include "weight.h"
#include "valve.h"
#include "computer_reset.h"
#include "workflow.h"
#include "dispense_guard.h"
#include "test_mqtt.h"
#include "capability_registry.h"
#include "MiThermometer.h"

// =====================================================
// setup
// =====================================================
void setup()
{
    Serial.begin(115200);
    delay(1000);
    // =====================================================
    // 第一层：基础系统（无依赖）
    // =====================================================
    // ===== 一次性挂载 LittleFS =====
    if (!LittleFS.begin(true, "/littlefs", 10, "littlefs")) {
        Serial.println("[System] LittleFS mount failed!");
        // 根据你的错误处理策略，可以选择重启或继续
    } else {
        Serial.println("[System] LittleFS mounted");
    }
    Serial.printf("PSRAM size: %u\n", ESP.getPsramSize());
    Serial.printf("PSRAM free: %u\n", ESP.getFreePsram());
    // ===== 初始化 JSON Storage（底层文件存储，ConfigManager 依赖它）=====
    if (!json_storage_init()) {
        Serial.println("[System] JsonStorage init failed!");
    }
    // ===== 初始化 BIN Storage（底层二进制文件存储，Workflow BIN 持久化依赖它）=====
    //
    // 内部会级联初始化 FileStorage。
    // 与 JsonStorage 平级，两条链路互不干扰：
    //   ConfigManager → JsonStorage → LittleFS
    //   WorkflowManager(后续) → BinStorage → FileStorage → LittleFS
    if (!bin_storage_init()) {
        Serial.println("[System] BinStorage init failed!");
    }
    // ===== 初始化 Workflow Storage（Workflow 定义持久化，依赖 BinStorage）=====
    //
    // 内部会创建 /workflow 目录并加载 meta.bin。
    // 本阶段只初始化存储层，Workflow.cpp 仍走原有 JSON 加载路径，行为不变。
    if (!workflow_storage_init()) {
        Serial.println("[System] WorkflowStorage init failed!");
    }
    system_state_init();
    config_init();
    event_manager_init();
    // =====================================================
    // 第二层：通信（依赖基础系统）
    // =====================================================
    ble_init();
    wifi_init();
    // =====================================================
    // 第三层：Workflow 框架（依赖基础系统）
    // =====================================================
    workflow_init();
    // =====================================================
    // 第四层：业务模块注册（依赖 workflow_init）
    // =====================================================
    weight_init();
    valve_init();
    computer_reset_init();     // GPIO8 脉冲（电脑重启），注册 COMPUTER_RESET Action
    dispense_guard_init();
    oled_init();
    oled_event_init();
    // ---- TimeManager V2：必须在 oled_init() 之后 ----
    // RTC(PCF8563T) 复用 OLED 建立的 Wire I2C Bus，绝不在本模块重复
    // Wire.begin()；故 time_init() 从第二层移到此处。
    time_init();
    test_mqtt_init();
    bool ok = MiThermometerInit();
    // =====================================================
    // 第五层：命令和云端（依赖业务模块注册完成）
    // =====================================================
    command_manager_init();
    cloud_init();
    // =====================================================
    // 第六层：加载 Workflow 配置（依赖所有注册完成）
    // =====================================================
    // 优先从 Flash BIN 加载（meta.bin + wfNN/stepNN.bin）；
    // Flash 中没有任何 Valid Workflow 时（首次启动）回退到 JSON。
    if (workflow_load_from_storage()) {
        Serial.println("[OK] Workflow loaded from Flash BIN");
    } else if (workflow_load_json_file("/workflow.json")) {
        Serial.println("[OK] Workflow loaded from /workflow.json");
    } else {
        Serial.println("[WARN] No workflow.json found");
    }
    // =====================================================
    // 第七层：capability registry（依赖所有注册完成）
    // =====================================================
    capability_registry_init();
    // =====================================================
    // 第八层：启动确认（必须位于 setup 最后）
    //
    // 只有完整走完 setup 才会写入启动标记。
    // 若因配置错误导致 panic / 反复重启，标记不会被写入，
    // 下次启动 ConfigManager 会自动从 Backup 恢复配置。
    // =====================================================
    if (!config_boot_validate()) {
        Serial.println("[WARN] Config boot validate failed");
    }
}

void serial_debug_command_process();
// WorkflowStorage 独立测试控制台（wfst 命令，仅上板自测用）
void wfst_console(const String &cmd);
// =====================================================
// loop
// =====================================================
void loop()
{
    // ---- Safe Restart 状态机（SystemCommand）----
    // 必须每轮调用：它是全系统唯一的 Restart 执行点。
    // 放在最前面，保证已进入的 10 秒安全窗口不被其他任务拖长。
    system_command_task();

    // ---- 原有任务（保持不变） ----
    wifi_task();
    event_dispatch();
    time_task();
    cloud_task();

    command_manager_task();
    config_task();            // 配置修改后的自动重启倒计时
    // ---- 新增：Workflow 任务 ----
    workflow_task();          // 执行 Workflow 状态机
    oled_task();
    weight_task();
    valve_task();
    computer_reset_task();     // 手动脉冲回收 + GPIO8 安全兜底
    MiThermometer_task();
    test_mqtt_task();//测试代码，需要删除
    serial_debug_command_process();
}

//测试代码
//==== 放在 main.cpp 全局区域（不要写在loop内部）====
String serial_cmd_buffer;
//==== 命令解析函数，在loop()中调用 ====
void serial_debug_command_process(void)
{
    while (Serial.available() > 0)
    {
        char ch = Serial.read();
        // 回车 / 换行 触发解析
        if (ch == '\n' || ch == '\r')
        {
            if (serial_cmd_buffer.length() == 0)
            {
                continue;
            }
            serial_cmd_buffer.trim(); // 清除首尾空格
            Serial.print("Recv cmd: [");
            Serial.print(serial_cmd_buffer);
            Serial.println("]");
            if (serial_cmd_buffer == "valve_open")
            {
                bool ok = valve_open();
                Serial.printf("valve_open execute ret = %d\n", ok);
            }
            else if (serial_cmd_buffer == "valve_close")
            {
                bool ok = valve_close();
                Serial.printf("valve_close execute ret = %d\n", ok);
            }
            else if (serial_cmd_buffer == "valve_toggle")
            {
                bool ok = valve_toggle();
                Serial.printf("valve_toggle execute ret = %d\n", ok);
            }
            else if (serial_cmd_buffer == "valve_status")
            {
                Serial.printf("Valve current open state: %d\n", valve_is_open());
            }
            else if (serial_cmd_buffer == "computer_reset")
            {
                // 手动触发一次 800ms 脉冲（绕过 MQTT，用于本地验证 GPIO8）
                bool ok = computer_reset_trigger();
                Serial.printf("computer_reset trigger ret = %d\n", ok);
            }
            else if (serial_cmd_buffer == "computer_reset_status")
            {
                Serial.printf("ComputerReset active state: %d (pin=%d, hold=%lu ms)\n",
                    computer_reset_is_active(),
                    COMPUTER_RESET_PIN,
                    COMPUTER_RESET_HOLD_MS);
            }
            else if (serial_cmd_buffer.startsWith("wfst "))
            {
                // WorkflowStorage 独立测试控制台（仅上板自测用）
                wfst_console(serial_cmd_buffer);
            }
            else
            {
                Serial.println("unknown command");
            }
            serial_cmd_buffer = ""; //清空缓冲区
        }
        else
        {
            serial_cmd_buffer += ch;
        }
    }
}
// =====================================================
// WorkflowStorage 独立测试控制台（仅上板自测用）
//
// 命令（wfst <op> ...）：
//   help
//   seed <wf> <steps> <base>       构造并保存一个 Workflow（param = base+step）
//   overwrite <wf> <steps> <base>  换参重新保存（复用当前 wf）
//   dump <wf>                      读取并打印 step_count / step id / param[0]
//   verify <wf> <steps> <base>     读取并自动比对，PASS/FAIL
//   ls <wf>                        打印 step bin 是否存在 + 暂存文件数
//   del <wf>                       删除（只置 meta.valid=false）
//   fail <wf> <failstep>           保存时注入第 N 步写失败（覆盖当前参数）
//   abort1 <wf> <steps> <base>     模拟提交前掉电（返回 TEST_ABORTED）
//   abort2 <wf> <steps> <base>     模拟提交后掉电（返回 TEST_ABORTED）
//   recover                        执行事务恢复（发布/清理）
//
// 掉电场景测试方法：
//   abort1 → staged>0 → recover → staged=0 → verify 仍为旧参数
//   abort2 → staged>0 → recover → staged=0 → verify 为新参数
// =====================================================

static int g_wfst_steps = 0;
static int g_wfst_base = 0;

static void wfst_make_def(
    WorkflowDefinition &def,
    int wf,
    int steps,
    int base
)
{
    memset(&def, 0, sizeof(def));
    snprintf(def.id, sizeof(def.id), "wfst_wf%d", wf);
    snprintf(def.name, sizeof(def.name), "wfst_test_%d", wf);
    def.enable = 1;
    def.timeout_ms = 10000;
    def.step_count = (uint8_t)steps;

    for (int i = 0; i < steps && i < (int)WF_STG_MAX_STEP; i++)
    {
        WorkflowStepDefinition &st = def.steps[i];
        st.type = 0;                       // ACTION
        st.instance_type = 1;              // ACTION
        snprintf(st.id, sizeof(st.id), "act_%d_%d", wf, i);
        st.param_count = 1;
        snprintf(st.params[0].name, sizeof(st.params[0].name), "k");
        st.params[0].type = 0;             // PARAM_INT
        st.params[0].int_value = base + i;
    }
}

static bool wfst_verify(
    int wf,
    int steps,
    int base
)
{
    WorkflowDefinition def;
    WorkflowStorageResult r = workflow_storage_load((uint8_t)wf, &def);

    if (r != WF_STG_OK)
    {
        Serial.printf("verify: load failed, r=%s\n", workflow_storage_result_name(r));
        return false;
    }
    if ((int)def.step_count != steps)
    {
        Serial.printf("verify: step_count=%u expect=%d\n", def.step_count, steps);
        return false;
    }
    for (int i = 0; i < steps; i++)
    {
        char expect_id[WF_STG_ID_MAX_LEN];
        snprintf(expect_id, sizeof(expect_id), "act_%d_%d", wf, i);
        if (strcmp(def.steps[i].id, expect_id) != 0)
        {
            Serial.printf("verify: step[%d] id=%s expect=%s\n",
                          i, def.steps[i].id, expect_id);
            return false;
        }
        if (def.steps[i].param_count < 1 ||
            def.steps[i].params[0].int_value != base + i)
        {
            Serial.printf("verify: step[%d] param=%d expect=%d\n",
                          i,
                          def.steps[i].param_count ? (int)def.steps[i].params[0].int_value : -999,
                          base + i);
            return false;
        }
    }
    return true;
}

void wfst_console(const String &cmd)
{
    String op = cmd.substring(5);
    op.trim();

    int a = 0, b = 0, c = 0;

    if (op == "help")
    {
        Serial.println("wfst ops: seed overwrite dump verify ls del fail abort1 abort2 recover");
        return;
    }

    if (op == "recover")
    {
        bool ok = workflow_storage_recover();
        Serial.printf("recover ret=%d\n", ok ? 1 : 0);
        for (int wf = 0; wf < (int)WF_STG_MAX_COUNT; wf++)
        {
            int n = workflow_storage_test_staged_count((uint8_t)wf);
            if (n > 0)
            {
                Serial.printf("  wf%02d staged=%d\n", wf, n);
            }
        }
        return;
    }

    if (sscanf(op.c_str(), "seed %d %d %d", &a, &b, &c) == 3 ||
        sscanf(op.c_str(), "overwrite %d %d %d", &a, &b, &c) == 3 ||
        sscanf(op.c_str(), "abort1 %d %d %d", &a, &b, &c) == 3 ||
        sscanf(op.c_str(), "abort2 %d %d %d", &a, &b, &c) == 3)
    {
        if (a < 0 || a >= (int)WF_STG_MAX_COUNT || b <= 0 || b > (int)WF_STG_MAX_STEP)
        {
            Serial.println("bad args");
            return;
        }
        g_wfst_steps = b;
        g_wfst_base = c;

        if (op.startsWith("abort1")) workflow_storage_test_abort_phase(1);
        if (op.startsWith("abort2")) workflow_storage_test_abort_phase(2);

        WorkflowDefinition def;
        wfst_make_def(def, a, b, c);
        WorkflowStorageResult r = workflow_storage_save((uint8_t)a, &def);
        Serial.printf("%s -> r=%s staged=%d\n",
                      op.substring(0, op.indexOf(' ')).c_str(),
                      workflow_storage_result_name(r),
                      workflow_storage_test_staged_count((uint8_t)a));

        workflow_storage_test_abort_phase(0);
        return;
    }

    if (sscanf(op.c_str(), "fail %d %d", &a, &b) == 2)
    {
        if (a < 0 || a >= (int)WF_STG_MAX_COUNT ||
            b < 0 || b >= g_wfst_steps)
        {
            Serial.println("bad args");
            return;
        }
        WorkflowDefinition def;
        wfst_make_def(def, a, g_wfst_steps, g_wfst_base + 100);
        workflow_storage_test_fail_step(b);
        WorkflowStorageResult r = workflow_storage_save((uint8_t)a, &def);
        workflow_storage_test_fail_step(-1);
        Serial.printf("fail@%d -> r=%s staged=%d\n",
                      b,
                      workflow_storage_result_name(r),
                      workflow_storage_test_staged_count((uint8_t)a));
        return;
    }

    if (sscanf(op.c_str(), "dump %d", &a) == 1)
    {
        WorkflowDefinition def;
        WorkflowStorageResult r = workflow_storage_load((uint8_t)a, &def);
        Serial.printf("load r=%s staged=%d\n",
                      workflow_storage_result_name(r),
                      workflow_storage_test_staged_count((uint8_t)a));
        if (r == WF_STG_OK)
        {
            Serial.printf("  wf%02d id=%s name=%s enable=%u count=%u\n",
                          a, def.id, def.name, def.enable, def.step_count);
            for (int i = 0; i < (int)def.step_count && i < (int)WF_STG_MAX_STEP; i++)
            {
                Serial.printf("    step%02d id=%s p0=%d\n",
                              i, def.steps[i].id,
                              def.steps[i].param_count
                                  ? (int)def.steps[i].params[0].int_value
                                  : -999);
            }
        }
        return;
    }

    if (sscanf(op.c_str(), "verify %d %d %d", &a, &b, &c) == 3)
    {
        bool ok = wfst_verify(a, b, c);
        Serial.printf("verify wf%02d steps=%d base=%d -> %s\n",
                      a, b, c, ok ? "PASS" : "FAIL");
        return;
    }

    if (sscanf(op.c_str(), "ls %d", &a) == 1)
    {
        int exist = 0;
        for (int i = 0; i < (int)WF_STG_MAX_STEP; i++)
        {
            char p[64];
            snprintf(p, sizeof(p), "/workflow/wf%02d/step%02d.bin", a, i);
            if (bin_storage_exists(p))
            {
                exist++;
            }
        }
        Serial.printf("wf%02d step-bin-exist=%d staged=%d\n",
                      a, exist,
                      workflow_storage_test_staged_count((uint8_t)a));
        return;
    }

    if (sscanf(op.c_str(), "del %d", &a) == 1)
    {
        WorkflowStorageResult r = workflow_storage_delete((uint8_t)a);
        Serial.printf("del wf%02d -> r=%s\n", a, workflow_storage_result_name(r));
        return;
    }

    Serial.printf("unknown wfst op: %s\n", op.c_str());
}
