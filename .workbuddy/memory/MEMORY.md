# Guo Feeder Project 长期笔记
ESP32-S3 N16R8 宠物供水/投喂 v0.7；主攻自动猫咪饮水（水箱+重力供水+电磁阀+HX711，按重量精确控水）。
**只留铁律与踩坑；细节看专项文档。**

## 文档索引（细节都在这里）
`readme.md`（架构/编码规范/MQTT 协议）· `需求文档.md`(V2.1) · `AI_RULES.md`(分层·非阻塞铁律) · `config_manager接口文档.md`+`jsonstorage开发架构.md`+`json_storage接口文档.md`+`bin_storage开发说明0910.md`(**Config 存储定版=行动基线**) · `system_command接口文档.md`+`critical_operation接入规范.md`(restart 以代码 V2 为准) · `cloud_protocol.md`(**09-15 按代码重写 V2.0，权威**) · `workflow_cloud_interface.md`(**Workflow↔Cloud 唯一契约**) · `log模块历史/`(Log 审计·设计·契约冻结·P1.1–P1.5 台账) · `workflow修改历史需求/` · `新增动作模板.md`
架构四层：应用 → 自动化(Workflow+Registry) → 服务(State/Config/Event/Time/WiFi/Command/**Log**) → 云(CloudManager/MQTT)。

## 硬性规范
- 零全局裸变量，状态走 System State；新增 State 必改三处：`system_state.h` 枚举（STATE_MAX 前）+ `.cpp` state_map + 产生模块
- 全程非阻塞：禁 `delay()`、禁 while 死等；队列定容、满则拒绝+告警；模块单一职责、数据单向；内部模块**不处理 JSON、不传 String**
- Action/Trigger 生命周期 `reset→start→poll→SUCCESS/FAILED`，状态放 `inst->runtime` 禁 static
- **内存**：**栈永远在内部 RAM，PSRAM 只能用于堆**；**>1KB 结构禁止上栈**（loopTask 16KB，余量约 6.5 KB）；范式 **`MALLOC_CAP_SPIRAM` 优先、失败回退 `MALLOC_CAP_8BIT`**；`-DARDUINO_LOOP_STACK_SIZE=16384` 与 PSRAM 都要保留；ArduinoJson 7.4.3 **无** PSRAM 支持
- **Config 存储**：业务模块管参数合法 → ConfigManager 管配置 → JsonStorage 管文件 → LittleFS；双版本 Active+Backup（`uint8_t` 递增、**不用时间戳**）；**原子写** tmp 写完→校验→旧 Active 转 Backup→tmp 转 Active；**Boot Validation**（setup 全部模块初始化完才标 Valid）；**Cloud 修改一律"重启生效"**；新增模块改 4 处：`CONFIG_MODULE_COUNT`、`kModuleNames[]`、模块名宏、`data/config/<m>.json`（`gen_config_version.py` 自动扫描）

## Critical Operation（Safe Restart V2）
- 全系统**唯一** `ESP.restart()` 在 `system_command.cpp:312`；已接入 ConfigManager（Save 成功才 release+request，**失败不 release 不重启**）/ Workflow（每 Workflow、每临时 Action 各 ±1，收口 `workflow_terminate()`/`temp_action_complete()`）/ TimeManager（**仅 RTC 写**）；**不接入** ComputerReset
- **release 不能放在会中途 return 的函数里**（`workflow_notify_finish()` callback 为空时提前返回 → 漏 release = **永久无法重启**）；强制终止路径（`workflow_stop`/`_disable`/`_clear`）必须补 release；acquire 被拒**直接 return 不 release**
- **跨任务并发（P0）**：`workflow_start()` 在 **esp-mqtt 任务**、`workflow_task()` 在 **loop 任务** ⇒ 收口必须**先 Release、后置 state**，否则 count 永久泄漏、再也无法重启。通式：**"释放"早于"让对象对外可见地变为可重新开始"**
- **Action 引擎差异**：临时 Action（`workflow.cpp:2172-2201`）`start()` 后**同轮立即** `poll()`；Step Action（:2093-2100）首轮只 start；两者 `poll()` 都可能带 `result != RUNNING` → **不得**在此全局强制复位

## Workflow 云端契约（09-13 定版，171/171 上板；详见 `workflow_cloud_interface.md`）
- **`p.id` = Slot 整数(0..15) 是唯一定位键**；`workflow.id` 允许重复/内容完全相同；`stable_id` 只是 Registry metadata；已**移除**按 `workflow.id` 搜索
- `count` = **占用 Slot 数**（含 valid=false），**不是** `capability_get_workflow_count()`；`variant` = uint32 内容版本号（**非 hash、绝不时间戳**）：create=1/有变化 +1/完全一致不变/delete(valid=true)+1/**重复 delete 幂等**；Meta v3（89B）
- `valid` = **RAM 标志**（不进 BIN），重启后不再加载 → `get` 返回 **error 5**；Registry 文件 **v2** = stable_id + `object_version(uint32)`(=variant) + runtime_id；magic `AP2A/AP2T/AP2W`
- `save` **逻辑全局、物理逐 Workflow 事务**：成功者立即清自己 Dirty、失败者保留、循环不中断；任一失败→错误码 10；`dirty=false` 时 `saved=true,restarting=false`；`restarting:true` 只表示请求被接受
- `delete` 逻辑删，**立即落盘**后清本对象 Dirty（留 Dirty 会让后续 save 把 valid 置回 true **撤销删除**）
- **严格 Contract**：Step `type` 必填、`steps>16`、`params>8` 一律**整体拒绝（11）**
- **四条易踩**：① `stable_id` = 按 `runtime_id` 字母序下标，新增/改名会让后续 stable_id **整体平移** ⇒ 云端以 `p.id` 为主键；② `enable` 缺省两路径不一致（JSON `true`/BIN `false`）⇒ 永远显式传；③ `step.id` 写错**不报错**但永不执行；④ `registry_version` 变化 ≠ 只有 Workflow 变
- **形态约束（高危已修）**：`workflow_pick_object()` **必须**要求内容嵌套在 `p.workflow` 下 —— 曾允许平铺，`p:{"id":"WF1"}` 被当成完整 Workflow 而**静默清空 steps**。**定位字段与内容载荷绝不能共用同一个 key**
- 串口透传 `cm {"cmd":"workflow.xxx","ob":"-","id":"8010","p":{...}}`（与 MQTT 同一 `command_manager_execute()`）；新增 Workflow 字段时 `workflow_storage_load()` 每个拷贝点都要回填，否则重启丢值（variant 曾漏过）

## LogManager（P1.1–P1.5 全部 ✅ 已提交；仅设备侧测试待开发板）
详见 `log模块历史/`（0914 审计、0915 设计、P1 契约冻结、P1.3 与 P1.4-P1.5 台账）。
- **冻结策略**：DEBUG(Flash✗/Cloud✗) · INFO(✗,✓) · WARN(✓,✓) · ERROR(✓,✓) · CRITICAL(✓,✓)；**无 EventId 级例外，禁 `persist`**；三 Topic `down`+`up`(语义不变)+**`log`(CBOR)**；**Queue 方案 A（Log 独立环）**
- Record **128 B 定长 v2**，`flags` 仅 bit0 `timestamp_valid`+bit1 `context_present`（**已删 `persist`/`uploaded`** —— Record 不知道自己是否已上传/该不该持久化）
- 排序四件套 `timestamp_valid`+`boot_seq`(uint32)+`uptime_ms`+`seq`(uint32，**Boot 预留区间** `seq_reserved += 256`，允许空洞、不允许重复)
- **LittleFS 是 COW**：原地改 128B = 重写整 block ⇒ 改"整段创建/追加/删除"，head/tail 由目录结构推导（**零原地修改**）；**建段必须预分配定长 3984 B**（按需增长会被重启扫描判 size 不符而整段丢弃）
- Flash 段环 **16 段 × 31 条 = 496 条 ≈63.7 KB**（`/log/s%07u.log`，16B header + 31×128B，**不改 partitions.csv**）；RAM 环 64×128B=8KB(PSRAM)
- ACK **批次级**（`seq_from..seq_to` 走 `down`，`c:"log_ack"`，**旁路 CommandManager**）；**at-least-once**，ACK ≠ 不会再出现，**云端必须幂等**，幂等键 `(device_id,boot_seq,seq)`；**FORCE_ADVANCE 不冻结**（默认 `GIVE_UP_NOT_ADVANCE`，环满 FIFO 淘汰 + `drop_unacked`）
- **CRITICAL**：Flash 立即、Cloud 进最高优先级**下一批次**，**不建独立通道、不承诺秒级**
- **无独立 Task**，`log_task()` 放 `loop()`（LittleFS 单写者），每轮 ≤1 次 Flash 写 + ≤1 次发送（20 ms / 500 ms 节流）；递归防护四层（重入守卫 / I/O 区只置待报标志 / **不劫持 `set_log_callback` 文本回调** / 侧信道计数）
- **`log_task()` 四阶段**：窥视（**不推进 `s_rd`**）→ 本轮唯一 append 单元 → **按"已落盘前缀"提交** → 落地。**不变量：已落盘 + 环内保留 == 已路由 ⇒ WARN+ 不可能静默丢失**；INFO 不被失败的 WARN 阻塞；CRITICAL 同一轮落盘
- CloudManager 改动**纯新增**：`CloudRoute` + `cloud_send_log`(store=0) + `cloud_set_log_ack_callback` + `log_ack` 分支；**现有函数签名 0 改动**；100 个 LogEventId + 89 个 LogParamId
- **进度（每阶段独立 commit）**：P1.1 `e267fd6`/`e003269` → P1.2 `6e7659a` → P1.3 五步 `056b147`/`3cc1b39`/`ff74f96`/`f418abd`/`2db00be`（330/330 PASS）→ **P1.4 `42f564f`**（`cloud_send_log` + `src/log_cbor.h` freestanding CBOR 编码器 + PSRAM 云待发环 128×128B + `mqtt.json log_topic`）→ **P1.5 `4fdc1b2`**（`src/log_ack.h` **判定与执行分离** + `cloud_poll()` 七段式 + 整段被覆盖才回收 + Flash 补发 + `GIVE_UP_NOT_ADVANCE`；**推进依据由「发布成功」改为「收到 ACK」**；去重守卫由 seq 单调改为 **`(boot_seq,seq)` 相等** —— 单调会让补发永远被拒）→ 报告 `8298f82`
- **契约测试 4 步 ALL PASS**：正向 / 负向 / CBOR 字节级 **16 项** / ACK 逻辑 **50 项**（后两步是 wasm32 **真实执行**，无需开发板；范式见下）
- **P1.5 测试钩子**：`logt ack <boot> <from> <to>` / `ackauto <0-3> [k]`（按在途批次自动造 ACK，因 seq 由 Boot 区间预留、脚本无法预知）/ `cfail <n>` / `sonline <0|1>`（强制在线+跳过 publish，否则 ACCEPT/PARTIAL 走不到）/ `atimeout|abackoff <ms>`（把「重试耗尽」从 152 s 压到秒级；默认值不变，`creset` 复位）
- ⛔ **设备 / MQTT 端到端全部 BLOCKED**：开发板未连接（`list_ports` 只有 3 个蓝牙虚拟串口）⇒ 非代码问题；可直接照跑的命令见 `log模块历史/LogManager-P1.4-P1.5交付报告0916.md` §4
- **测试钩子**：`logt ffail <n>`/`mfail`/`mwipe`/`mcorrupt`/`fcorrupt`/`fbrec`/`ftrunc`/`fwipe`/`flush`/`flash`/`fstats`/`cstats`/`cpush`/`creset`
- **★ `guo_feeder/log` ACL 已放开（09-16 实测，不再阻塞）**：`test/mqtt_log_probe.py probe` → `CONNACK=0`、`granted=1`、publish rc=0；凭据 `data/config/mqtt.json`，CA `data/emqxsl-ca.crt`；**唯一未决 = Q5**（`log.json` 开关是否允许"立即生效"）
- **待修 P0 前置（尚未做）**：`json_storage`/`file_storage` 日志回调**从未注册**（≈88 处 E/W 静默丢弃）；`event_names[]` 第 10 项错位；MQTT 明文密码；BLE 回调逐字节 hex(`MiThermometer.cpp:117`)；`valve_force_close()` 后无残余增重检测；`reset_reason` 从不落盘；`workflow_terminate()` 六处零日志
- **⚠️ 避开 `LOG_LEVEL_*`**：NimBLE-Arduino `log_common.h:38-45` **无条件** `#define LOG_LEVEL_DEBUG (0)`…`(4)`，**无 `#ifndef` 保护** ⇒ 同名枚举被展开报 `expected unqualified-id before numeric constant`；本项目用 **`LOG_LVL_*`**，其它 `LOG_*` 先查 NimBLE 的 `LOG_TYPE_*`/`LOG_MODULE_*`/`LOG_VERSION_V3`
- **⚠️ 工具链 gnu++11：`constexpr` 函数体只能是一条 `return`**，不得声明局部变量
- **契约测试范式（★无需开发板）**：freestanding 头（只依赖 `<stdint.h>`/`<stddef.h>`）⇒ ① `xtensa-esp32s3-elf-g++ -fsyntax-only -Isrc <probe>.cpp` 数秒求值全部 `static_assert`；② 本机 `clang++` 缺 MSVC/CRT 库，但 **`--target=wasm32 -nostdlib` 零依赖**，用 `node` 跑 wasm **真正断言字节**；③ **必须同时写负向探针**，否则"通过"不可证伪；④ 产物写 `.pio/`

## 其它模块要点（详见对应文档）
- **TimeManager V2**：`time_init()` 必须晚于 `oled_init()`（否则 RTC 探测必失败）；RTC(PCF8563T,0x51) **复用 OLED 的 Wire，绝不 `Wire.begin()`/`setClock()`**；**SNTP 陷阱**：IDF v4.4 COMPLETED **瞬时**并自动回落 RESET，"从未同步"也是 RESET ⇒ **禁用 `status != IN_PROGRESS` 判成功**，现用「callback 通知 + loop 延迟确认」；callback 只做轻量标记；RTC 校准值取 `time(nullptr)`；内部统一 UTC，本地时间禁写 RTC；上板 **RTC 仍 `rtc_present=false`**
- **云端/CloudManager**：两种下发格式 `compact = !doc["c"].isNull()`（`cloud_manager.cpp:1048`）—— 旧 `{"cmd":"execute_action","id":"8010","ob":"<RUNTIME_ID>"}`（调试首选）/ 新 `{"c":"action","i":..,"v":<ver>,"k":<stable_id>}`，查 registry `{"c":"registry","i":..,"k":0}`（0=ACTION/1=TRIGGER/2=WORKFLOW）；命令 `id` **每条唯一**；**协议级消息旁路 CommandManager** → 照 `change_msg_limit` 落点（`cloud_process_rx_message:1067`）；**无应用层 TX 队列**（`cloud_send_up()` 同步直发）；**上行是 JSON 文本不是 CBOR**；**离线时直接返回 false，不排队不落盘**；`retry_interval` 是**死配置**；⚠️ MQTT 横幅**明文打印密码**
- **烧录基线**：**`uploadfs` 整分区擦除**（`0x410000→0xffffff`），而 `esptool write_flash 0x10000 firmware.bin` **不碰** LittleFS；`data/` 被 gitignore（含 WiFi 密码/CA）⇒ `tools/gen_config_version.py`+`gen_workflow_bin.py` 重建（⚠️ `WF_STG_META_VERSION` 升级必须同步脚本，否则 `VERSION_TOO_NEW` → 回退 `/workflow.json`）；**ConfigManager version 自愈** `bootstrap_version_file()` 必须在**模块加载之后**（`save_version_file()` 跳过 `version==0`，否则文件永远生成不了）；抓"首次启动"须 `esptool --after no_reset write_flash 0x410000 littlefs.bin`

## 环境操作铁律（Windows + PlatformIO）
- **Bash 工具 PATH 可能损坏**（`ls`/`grep`/`date` 报 `command not found`；git/python 不受影响）⇒ 前加 `export PATH="/c/Users/wang/.workbuddy/binaries/PortableGit/versions/1.2.0/usr/bin:$PATH"`；另一终端工具 stdout 常不回显 ⇒ 重定向到文件再 Read/Grep
- 沙箱 `safe-delete` 拦删 `.o`/`.elf` ⇒ **增量编译必然失败** ⇒ `PLATFORMIO_BUILD_DIR=.pio/build/xxx pio run`（全量 60–190 s）；⚠️ 全新 BD 首次编译偶发 `*.sconsign314.dblite: No such file or directory` ⇒ 先 `mkdir -p "$BD/esp32-s3-devkitc-1"`
- `pio run -t upload` 实际不烧录 ⇒ `esptool.py write_flash -z 0x10000 <firmware.bin>`（`D:/platformIO/packages/tool-esptoolpy/esptool.py`）；Python 用 `C:/Users/wang/.workbuddy/binaries/python/envs/default/Scripts/python.exe`
- **串口回归**：`python test/serial_batch.py <COM> <log> <用例txt> [QUIET] [HIT_GRACE]`；用例 `<命令> ||| 期望子串`（断言串**不要加引号**）；**命中后 0.35 s 静默即返回、未命中仍按 QUIET 等**（`log_flash_tests` 383 s → 165 s）；**QUIET**：fill ≤62 用 **3.0**，含 `fill 100/200/300` 用 **5.0**（`logt stats` **会先排空 RAM 环**，vfs 错误会重置静默计时，曾差 0.5 s 误判）
- **★ 等待上限**：编译 `timeout 190` / 烧录 `timeout 120` / 串口回归 `timeout 185`；超时（124）**不重跑**，直接 grep 结果文件或拆更短用例
- **同一文件的多个 Edit 必须串行**（并行会互相覆盖且仍报成功；改完必须 grep/diff 复核）
- MCP C++（mcp-cpp）对本项目**不可用**；⚠️ `platformio.ini` 的 `platform`/`lib_deps` **未固定版本**
- **用户偏好**：每阶段独立 commit；测试资产放受跟踪的 `test/`（`.pio/` 被 gitignore）；编译临时文件可直接删，其他目录删除需用户同意
