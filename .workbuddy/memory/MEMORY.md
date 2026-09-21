# Guo Feeder Project 长期笔记
ESP32-S3 N16R8 宠物投喂/饮水 v0.7；主攻自动猫咪饮水（水箱+重力供水+电磁阀+HX711 按重量控水）。
**只留铁律与踩坑；细节看专项文档。**
> 专题：**LogManager → `MEMORY-logmanager.md`** · 每日日志 → `YYYY-MM-DD.md`

## 文档索引
`readme.md`(架构) · `需求文档.md` V2.1 · **`未修复的问题.md`（全项目问题权威清单：`LOG-n`/`VALVE-n`/`R-n`/`T-n`/`EVT-n`/`LV-n`）** · `AI_RULES.md` · Config：`config_manager接口文档.md`+`jsonstorage开发架构.md`+`json_storage接口文档.md`+`bin_storage开发说明0910.md` · `system_command接口文档.md`+`critical_operation接入规范.md` · `cloud_protocol.md`(V2.0) · `workflow_cloud_interface.md` · `docs/LogManager-Integration-Guide.md`+`docs/P2_Log_Integration_Matrix.md` · `log模块历史/`

## 当前阶段（2026-09-21）
- **LogManager 埋点：Phase 3 ①②③④⑤ + Phase 4 Dispense + Phase 5-A/B/C 全部已完成** —— 已接入 **14 模块**（含 **BLE/MiThermometer**）；零宿主 **23**（定义 102 · 有宿主 79）
- **Phase 5 前置审查（`5b5b529`+`9d5eae4`）与执行计划（`aeb6708`）已入库** —— 报告 `Phase5-BLE-OLED-Dispense边界与LogManager完整性审查0920.md`、计划书 `Phase5-ABC执行计划0920.md`
  - **审查推翻 3 条先前判断**：① 5-A **零协议改动**（7 个 ID + 所需 ParamId 全已冻结）；② **`EVT-1` 断点不是 `event_push`**（它/`subscribe`/`init` 都用 `MAX_EVENT_TYPE`(32) ⇒ 阀门事件**发布订阅正常**），真正断点是 `event_from_string()`；③ **`BOOT_INCOMPLETE_PREV` 判据已存在**（`config_manager.cpp:1108` `last_boot_ok`）⇒ 无需新增持久化
  - **`P0-1`**：两张字符串表都多出幻影 `"EVENT_CLOUD_COMMAND"`（index 10）⇒ index ≥10 全错位；`event_*` 触发器全仓库**零使用者** ⇒ 零回归风险
  - **`logt` 支持 `ring`**（RAM 环）⇒ **INFO 记录可上板自证**（`LV-2` 只影响参数值读取，不影响记录存在性）
- **★ Phase 5 执行进度（全部已提交）**：**5-A ✅**（`6194e3b`+`9c32675`+`1186044`，System/Boot 6 个 ID）· **5-B ✅**（`7fb9437`+`016dc96`，`EVT-1`+`P0-1` Registry 一致性；**休眠修复**：`WORKFLOW_EVENT_ENABLED=0` ⇒ 函数与两表被 GC ⇒ **RAM/Flash +0、运行行为不变**）· **5-C ✅**（`70b0c76`，BLE 4 个 ID）
- **剩余开发顺序**：**#1 `bin_storage` 桥接** → #2 `LV-2` 工具增强 → #3 OLED（需先修判据 `OLED-1`）→ #4 `workflow_storage` 接口 → #5 Dispense 业务（阻塞 `DSP-1/2/4`）
- **OPEN 项**：`LV-1` / `LV-2` / `LV-3` / `P0-1b` / `PROTO-1` / `PROTO-2` / `OLED-1` / `EVT-1`（Registry 部分已在 5-B 修复）
- **`R-7`/`R-8` 仍 DEFERRED —— Phase 5 审查明确未重新升级**（优先级/状态/结论均未改动）

### ★ Dispense 边界（Phase 4 审查定论；**细节全在 `log模块历史/Phase4-Dispense边界审查0920.md`**）
- **Dispense 本体不存在**：只有 `dispense_guard.cpp/.h`；**无电机/步进/出粮状态机/出粮 Action**；重量反馈闭环**属 Water 域**（Workflow 组合：`VALVE_OPEN` → `weight_decrease` Trigger → `VALVE_CLOSE`）
- **★ 命名冲突**：代码里 **"Dispense" = 供水**（`0x05xx` Water 段）；**出粮已叫 "Motor"**（`0x0Fxx` 段已预留）⇒ 推荐 `DispenseManager`（编排）+ `Motor`（驱动）
- **三硬约束**：① 重量反馈必用 `weight_get_gram()`（读 System State 会得 30 s 陈值）② `timeout_ms` 必填并钳位 ≤60 s（Temp Action FIFO 队头阻塞）③ **DispenseGuard 不能守护出粮**（只关水阀）；**DSP-1..10 未决**（P0 = 命名 / 日志段 / 队头阻塞）

### ★★ 铁律 29–32（**完整论述在 `MEMORY-logmanager.md`**）
- **29 纯转发模块的埋点数天然为 0**：① 列全部数据流 → ② 逐个问"**定义点**在哪"（在别的模块 ⇒ 别人记）→ ③ 只剩"自己失效"这类自指事实时，有 EventId 就记、没有就"零埋点 + 登记"。**"总线类模块记几条"没有统一答案**（对比：`dispense_guard` 记 1 条决策；`event_manager` 记 2 条自身异常）
- **30 突发合并三条硬约束**：① **被合并埋点必须至少带 1 个参数**（`log_coalesce_emit_summary()` 在 `base_n==0` 时**静默丢弃**折叠计数）② 参数须 `< LOG_MAX_PARAMS(8)` ③ **折叠对调用方透明**（`logt fill` 的 `queued=N` **恒等于 N**，真实记录数看 `stats` 的 `emit`/`flash` 增量）
- **31 新增日志前必须问"现有日志缺的是哪一维"** —— "**来源可分**"与"**次数可数**"是两件独立的事（Phase 4：来源区分已由 `0x0509/0x050A` vs `0x0505`+`CAUSE=1` 满足，缺的是**次数守恒**）。**答不上来就说明不该新增**
- **32 统计"埋点宿主覆盖率"必须检索三类证据**：① 字面实参 ② **`log_emit0(...)` 变体** ③ **映射函数的 `return LOG_X;`**（`cfg_bridge_event()`/`stg_bridge_event()`），并剥除注释 —— 否则 **`0x02xx`/`0x03xx` 会被系统性误判为整段零宿主**（首版实际踩到）。工具 `.pio/p5run/evtid_audit.py`
- 通用配套：**`LOG_P_CAUSE` = 0x16**（`0x1F` 是 `LOG_P_REASON`）；**N:1 冗余判据**（埋点前核对两侧"计数语义"是否同频）

### Phase 4 实施结果（细节在 `MEMORY-logmanager.md`）
- `0x0511` 埋在 `dispense_guard_event_callback()`（过滤后、`valve_force_close()` **前**）⇒ 记"**决策**"，与 Valve 侧"**动作**"（`0x0505`）互补：后者丢次数、前者 **Σ `LOG_P_COUNT` = 真实响应次数**；`DISPENSE_CAUSE_*` 与 `VALVE_CAUSE_*` **同值对齐**
- **`0x0501–0x0504` 未占用**（语义是"一次供水过程"生命周期，安全响应是**终止**供水 ⇒ 复用 `START` 会伪造事实）；验证 **V1 ✅**（+0 RAM / +28 Flash）+ **V4 objdump ✅**；**V2/V3 ⏳ 未执行**（无串口设备；用例 `test/p4_dispense_log_tests.txt`）

## 硬性规范
- 零全局裸变量，状态走 System State；新增 State 改三处：`system_state.h` 枚举 + `.cpp` state_map + 产生模块
- 全程非阻塞：禁 `delay()`、禁 while 死等；队列定容、满则拒绝+告警；内部模块**不处理 JSON、不传 String**
- Action/Trigger 生命周期 `reset→start→poll→SUCCESS/FAILED`，状态放 `inst->runtime` 禁 static
- **内存**：**栈永远在内部 RAM，PSRAM 只能用于堆**；**>1KB 结构禁止上栈**（loopTask 16KB，余量≈6.5KB）；**`MALLOC_CAP_SPIRAM` 优先、失败回退 `MALLOC_CAP_8BIT`**；`-DARDUINO_LOOP_STACK_SIZE=16384` 与 PSRAM 都要保留；ArduinoJson 7.4.3 **无** PSRAM 支持
- **Config**：业务管参数合法 → ConfigManager → JsonStorage → LittleFS；双版本 Active+Backup（`uint8_t` 递增、**不用时间戳**）；**原子写** tmp→校验→旧 Active 转 Backup→tmp 转 Active；**Boot Validation**；Cloud 改动**一律重启生效**；新增模块改 4 处：`CONFIG_MODULE_COUNT`、`kModuleNames[]`、模块名宏、`data/config/<m>.json`

## Critical Operation（Safe Restart V2）
- 全系统**唯一** `ESP.restart()` 在 `system_command.cpp:312`；接入 ConfigManager（Save 成功才 release+request，**失败不 release 不重启**）/ Workflow（每 Workflow、每临时 Action 各 ±1）/ TimeManager（**仅 RTC 写**）；**不接入** ComputerReset
- **release 不能放在会中途 return 的函数里**（`workflow_notify_finish()` callback 为空时提前返回 → 漏 release = **永久无法重启**）；强制终止路径（`workflow_stop`/`_disable`/`_clear`）必须补 release；acquire 被拒**直接 return 不 release**
- **跨任务并发（P0）**：`workflow_start()` 在 esp-mqtt 任务、`workflow_task()` 在 loop 任务 ⇒ 收口必须**先 Release、后置 state**。通式：**"释放"早于"让对象对外可见地变为可重新开始"**
- **Action 引擎差异**：临时 Action 同轮立即 `poll()`；Step Action 首轮只 start；两者 `poll()` 都可能带 `result != RUNNING` → **不得**在此全局强制复位

## Workflow 云端契约（09-13 定版；**细节全在 `workflow_cloud_interface.md`**）
- **`p.id` = Slot 整数(0..15) 唯一定位键**；`workflow.id` 可重复；`count` = 占用 Slot 数（含 valid=false）；`variant` = uint32 内容版本号（**非 hash、绝不时间戳**）；`valid` = **RAM 标志**（不进 BIN）⇒ 重启后 `get` 返回 **error 5**
- `save` = **逻辑全局、物理逐 Workflow 事务**；`delete` 逻辑删且**立即落盘**后清 Dirty（留 Dirty 会让 save 把 valid 置回 true **撤销删除**）
- **严格 Contract**：Step `type` 必填、`steps>16`、`params>8` 一律**整体拒绝(11)**
- **四条易踩**：① `stable_id` 按 `runtime_id` 字母序生成，新增/改名会让后续**整体平移**；② `enable` 缺省两路径不一致（JSON `true`/BIN `false`）⇒ 永远显式传；③ `step.id` 写错**不报错**但永不执行；④ `registry_version` 变 ≠ 只有 Workflow 变
- **形态约束**：内容必须嵌套在 `p.workflow` 下 —— 平铺会让 `p:{"id":"WF1"}` 被当完整 Workflow 而**静默清空 steps**。**定位字段与内容载荷绝不能共用同一 key**

## LogManager 摘要（**细节与铁律 22–31 在 `MEMORY-logmanager.md`**）
- **已接入 14 模块**：Storage·Config·WiFi·Cloud·Time·Workflow·Weight·Valve·Command·ComputerReset·Registry·Event·DispenseGuard·**BLE(MiThermometer)**
- 冻结：Record 128B v2 / 16 段×31 条 / COW 零原地改 / ACK **at-least-once ⇒ 云端幂等** / 无独立 Task（`log_task()` 在 loop）
- **INFO 只上云不落 Flash；WARN+ 落 Flash** ⇒ 任何新 WARN 埋点都会扰动 496 条环的绝对计数。回归当前 **168/195**（26 MISS 全属 `R-7` 环境干扰）
- **刻意不埋**：LogManager 自身 5 个 ID ⇒ 走**批次头侧信道计数**，**是设计非缺口**
- **无宿主**：`CFG_FACTORY_RESET`(0208)·`CLOUD_FRAG_FAIL`(0706)·`TIME_NTP_FAIL`(0802)·`OLED_INIT_FAILED`(0E01)；**需新增检测**：`VALVE_OVERFLOW_RISK`(0507)=`P0-4`
- 剩余待办：**`LV-1`** · **`LV-2`** · **`LV-3`** · `bin_storage` 桥接 · `workflow_storage` 回调接口 · `OLED-1`（先修判据再埋点）

**铁律（完整 1–31 条在 `MEMORY-logmanager.md`）** —— 以下是跨模块通用的 4 条：
- **★ 精确计数前必须先清零**（`logt fwipe`+`mwipe`+`creset`+`reset`），否则"孤儿增量"易误判成门控失效
- **★ 断言不能隐含"执行期间只有本用例在写日志"** ⇒ "改判据"＝删记账本体时就是降断言 ⇒ 正解是**测试隔离**
- **★ 门控只能加在"全部执行完成之后"**：纯事件型函数去重**绝不提前 return**；**"取值用于日志"的变量须在源头清零前取**
- **★★ 折叠/去重/门控类验证用"定向注入"**：`logt fill <lv> <n> <EventId(hex)>`；**记录级核对须 ACK 前置**（与"跑回归禁注入 ACK"相反，区别只在**是否在跑回归**）

## Weight / R-8（**细节在 `log模块历史/R7-R8-后续评审与系统性能权衡0920.md`**）
- **`R-8` = 真实缺陷**：滤波窗口无时间信息 + HX711 **覆盖式输出**（无 FIFO）⇒ 阻塞跨界 ⇒ 混合窗口 ⇒ 假跳变。**★ 危险区间非单调**：`<100ms` 无害 ｜ **`100–500ms` 最危险** ｜ `>500ms` 整窗换血反而不误报
- **后果升级**：假跳变经 `EVENT_WEIGHT_ERROR` → DispenseGuard → **`valve_force_close()`** ⇒ **`R-3` 与 `R-8` 同根因**（污染安全功能）
- **`R-8-A` 已修（`129606f`）**：`HX711::read()` 内 `wait_ready()` 无超时 ⇒ 掉线时 loop 永久挂起；修法＝`read()` 前二次 `is_ready()` 确认。**`R-8-B` 顺带解决**；**`R-8-C`** OPEN
- **`SYS-1`** LogManager **无需新增性能限制**；**`SYS-2`** BLE 与 LogManager **无并发**（共用 loopTask）
- **执行顺序铁律**：先修"会卡死/误动作的"，**再**修"让测试失准的"。⚠️ 修 `BT-1` 会使上行频次 `~1/15s → 2Hz`（**30×**）

## 其它模块要点（详见对应文档）
- **TimeManager V2**：`time_init()` 必须晚于 `oled_init()`；RTC(PCF8563T,0x51) **复用 OLED 的 Wire，绝不 `Wire.begin()`**；**SNTP 陷阱**：IDF v4.4 COMPLETED **瞬时**并自动回落 RESET ⇒ **禁用 `status != IN_PROGRESS` 判成功**
- **CloudManager**：两种下发格式（`compact = !doc["c"].isNull()`）—— 旧 `{"cmd":...,"ob":"<RUNTIME_ID>"}`（调试首选）/ 新 `{"c":"action",i,v,k}`；命令 `id` **每条唯一**；**协议级消息旁路 CommandManager**；**无应用层 TX 队列**、**离线直接返回 false 不排队**；`retry_interval` 是**死配置**
- **烧录基线**：**`uploadfs` 整分区擦除**；`esptool write_flash 0x10000` **不碰** LittleFS；`data/` 被 gitignore ⇒ 用 `tools/gen_config_version.py`+`gen_workflow_bin.py` 重建（⚠️ `WF_STG_META_VERSION` 升级必须同步脚本）；**`bootstrap_version_file()` 必须在模块加载之后**

## 环境操作铁律（Windows + PlatformIO）
- **Bash PATH 可能损坏** ⇒ 前加 `export PATH="/c/Users/wang/.workbuddy/binaries/PortableGit/versions/1.2.0/usr/bin:$PATH"`
- 沙箱拦删 `.o`/`.elf` ⇒ **增量编译必然失败** ⇒ `PLATFORMIO_BUILD_DIR=.pio/build/xxx pio run`；⚠️ 全新 BD 首次偶发 `sconsign314.dblite` 缺失 ⇒ 先 `mkdir -p "$BD/esp32-s3-devkitc-1"`
- `pio run -t upload` **可能完全不烧录** ⇒ 直接 `esptool.py write_flash -z 0x10000 <firmware.bin>`（`D:/platformIO/packages/tool-esptoolpy/`）；Python 用 `C:/Users/wang/.workbuddy/binaries/python/envs/default/Scripts/python.exe`
- **串口回归**：`python test/serial_batch.py <COM> <log> <用例txt> [QUIET] [HIT_GRACE]`；用例 `<命令> ||| 期望子串`（断言串**不要加引号**）；QUIET：fill ≤62 用 **3.0**、`fill 100+` 用 **5.0**；**开串口即复位**
- **⚠️ `serial_batch.py` 串口静默 1.5s 即提前返回** ⇒ 长静默 / RAM 状态类实验**必须单会话探针**。用 `logt stats` 的 **`emit` 增量**度量"某动作产生几条日志"
- **★ 自写探针必须按行缓冲**：`read(in_waiting)` 会把**半行**当整行 ⇒ 解析 `logt` 输出全部失败（踩过）
- **★ 等待上限**：编译 `timeout 190` / 烧录 `120` / 串口回归 `185`；超时(124) **不重跑**，grep 结果文件或拆用例
- **同一文件多个 Edit 必须串行**（并行互相覆盖且仍报成功，改完必须 grep 复核）；**★ Bash heredoc 跑 Python 极易被引号搞崩且失败静默无改动** ⇒ **先 Write 成文件再执行**（本轮又踩过一次）
- **用户偏好**：每阶段独立 commit；**禁 `git add -A`/`.`**（`.workbuddy/memory/`、`data/config/` 下有 force-add 的 tracked 文件）；测试资产放受跟踪的 `test/`；编译临时文件可直接删，其他目录删除需用户同意；文档（除 gitignore 内的）都可 commit
