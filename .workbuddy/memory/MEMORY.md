# Guo Feeder Project 长期笔记
ESP32-S3 N16R8 宠物投喂/饮水 v0.7；主攻自动猫咪饮水（水箱+重力供水+电磁阀+HX711 按重量控水）。
**只留铁律与踩坑；细节看专项文档。**
> 专题：**LogManager → `MEMORY-logmanager.md`** · 每日日志 → `YYYY-MM-DD.md`

## 文档索引
`readme.md`(架构) · `需求文档.md` V2.1 · **`未修复的问题.md`（全项目未修复问题权威清单，🔴6/🟠11/🟡9/⚪8 组）** · `AI_RULES.md`(分层·非阻塞) · Config：`config_manager接口文档.md`+`jsonstorage开发架构.md`+`json_storage接口文档.md`+`bin_storage开发说明0910.md` · `system_command接口文档.md`+`critical_operation接入规范.md` · `cloud_protocol.md`(V2.0 权威) · `workflow_cloud_interface.md`(Workflow↔Cloud 唯一契约) · `docs/LogManager-Integration-Guide.md`+`docs/P2_Log_Integration_Matrix.md` · `log模块历史/` · `workflow修改历史需求/`

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

## Workflow 云端契约（09-13 定版；细节全在 `workflow_cloud_interface.md`）
- **`p.id` = Slot 整数(0..15) 唯一定位键**；`workflow.id` 可重复；`stable_id` 仅 Registry metadata；`count` = 占用 Slot 数（含 valid=false）
- `variant` = uint32 内容版本号（**非 hash、绝不时间戳**）：create=1/有变化 +1/一致不变/delete+1/**重复 delete 幂等**；Meta v3(89B)；`valid` = **RAM 标志**（不进 BIN）→ 重启后 `get` 返回 **error 5**；Registry v2 magic `AP2A/AP2T/AP2W`
- `save` = **逻辑全局、物理逐 Workflow 事务**：成功者清 Dirty、失败者保留、循环不中断；任一失败→10；`delete` 逻辑删且**立即落盘**后清 Dirty（留 Dirty 会让 save 把 valid 置回 true **撤销删除**）
- **严格 Contract**：Step `type` 必填、`steps>16`、`params>8` 一律**整体拒绝(11)**
- **四条易踩**：① `stable_id` 按 `runtime_id` 字母序生成，新增/改名会让后续**整体平移**；② `enable` 缺省两路径不一致（JSON `true`/BIN `false`）⇒ 永远显式传；③ `step.id` 写错**不报错**但永不执行；④ `registry_version` 变 ≠ 只有 Workflow 变
- **形态约束**：内容必须嵌套在 `p.workflow` 下 —— 曾允许平铺，`p:{"id":"WF1"}` 被当完整 Workflow 而**静默清空 steps**。**定位字段与内容载荷绝不能共用同一 key**
- 新增 Workflow 字段时 `workflow_storage_load()` 每个拷贝点都要回填（variant 曾漏过）

## LogManager（**摘要；细节在 `MEMORY-logmanager.md`**）
> **P2 已完成 8 个模块**（Storage→Config→WiFi→Cloud→Time→Workflow→Weight→**Valve**），
> ⛔ **但回归阻塞在 168/195**（断言总数仍 195、未删改）。
> **★★ 已判定（2026-09-19 隔离实验）：不是 P2-H 代码问题、也不是 Valve 日志本身的问题。**
> 决定性证据：**P2-G 固件（完全没有 Valve 日志）在环境异常强度高时跑同一 A 段夹具同样 MISS（9 条 / 2 条）**；
> MISS 只随"环境重量异常强度（`STATE_WEIGHT_ERROR` 边沿数）"单调上升 ⇒ 与固件版本不相关。
> Weight 侧单独就能产生 **+13 条**额外记录（P2-G #3：`emit` 509 vs 夹具基数 496，`crit` 恒 0）；
> 同强度下记录级配比 **Valve(crit)=9 : Weight≈6** ⇒ **只去掉 Valve 日志最多把污染减半，不能归零**。
> ⇒ **需要的是"测试隔离"（让被测系统安静），不是消除某一个模块的日志。**
> 另：`qdrop`/`fdrop`/`seg_evict_unacked`/`seg_new`/`seg_del` 在两固件间**几乎或完全相同** ⇒
> 结构性行为未被改变，故夹具的**结构性断言大多仍通过**，只有**精确计数类**失败。
> **`R-7`/`R-8`/`VALVE-6` 已登记 OPEN、本轮不修**；**`R-6` 表述已修正**：
> `force_close` 调用数 == `guard_rx` 在全部 9 次运行中**恒等** ⇒ 不是"调用被复制"，
> 而是**同毫秒两个独立的 `EVENT_WEIGHT_ERROR`**（风暴抑制允许 5 条/500 ms）⇒ **不是缺陷，无需修复**。
> 下一步：**等决策**（`R-7` 的解法未定，故暂不进 Dispense）。
> ⚠️ P2-H 铁律：① `valve_force_close()` 是**纯事件非状态迁移** ⇒ 门控位置必须在"GPIO→状态→SystemState→`open_start_time`→event→Serial"**全部之后**，**绝不 `if(!need_log)return;`**、**绝不改成 `current_state` 边沿**；② **`OPEN_MS` 必须在 `open_start_time=0` 之前取**（本模块踩过 3 次）；③ 一次性报告锁的**解除点 = 真实状态迁移完成处**；④ `STATE`/`WAS` 用 `log_arg_bool`；⑤ **不给无数据源的参数填伪造 0**。
> ⚠️ 矩阵 6 状态**有 4 个不存在**（实际只有 `bool current_state`）；矩阵 3 处与代码不符（`FORCE_CLOSE_FAILED` **有**失败分支 / IMM 只是置一次 flush 标志 / RATE_LIMITED 实测 0 次无需去重）—— 矩阵 §9 **已按真实模型重写**。
> 🔴 未修：**VALVE-1**（四个 valve API 均不检查 `initialized` ⇒ 假成功）· **VALVE-6**（`FORCE_CLOSE_FAILED` **未门控**，54 次调用→52 条 CRITICAL）· **R-7**（回归夹具与跨模块埋点根本冲突）· **R-8**（FS 阻塞诱发重量跳变）。`VALVE-2`/`VALVE-3` ✅ 已解决。
> 具体条文（含全部 BT-/WF-/R-/NC-/T-/D- 编号、实测数字、复测方法）见 `MEMORY-logmanager.md` 的
> 「【P2 阶段】铁律与实测结论」+ 仓库根目录 **`未修复的问题.md`**。

**十一条铁律（详细论证在专题文件）**
1. `seq` 对**每条非 DEBUG 记录**分配（含 INFO）；INFO 是 Flash NO ⇒ **Flash 段内 seq 必然稀疏** ⇒ 禁止用 `first_seq + n - 1` 之类推导（曾致补发永久终止 + 段提前回收）。
2. 冻结：Level 无 EventId 例外 / Record 128B v2 / 16 段×31 条（建段预分配 3984B）/ LittleFS COW ⇒ **零原地修改** / ACK 批次级、**at-least-once ⇒ 云端必须幂等** / 无独立 Task、`log_task()` 在 `loop()` / **推进依据 = 收到 ACK**。
3. **Level Policy：INFO 只上云不落 Flash；WARN+ 落 Flash** ⇒ 任何新埋点如果是 WARN，就会往 496 条 Flash 环里加记录（回归绝对计数会被扰动）。
4. **埋点位置三原则**：① 只放既有状态迁移点/错误分支；② **高频源禁止埋点**（10 Hz 采样、2 Hz 滤波、每 loop 路径、每事件 push）；③ 必须验证**可达性**（分支存在 ≠ 可达）。
5. **失败是状态、不是事件** ⇒ 用**边沿锁**而不是降频；每次失败都记一条会在故障持续期洪泛。
6. **高频路径用"边沿 / 计数聚合"**：WiFi 失败循环 = N 次记 1 **AND** ≥60s；MQTT 断连用既有单槽 pending 标志做边沿；发布失败 60s 聚合。
7. **水位语义必须分离**（BT-9 已修）：`acked_seq`=真实最大（仅观测）/ `gc_seq`=**连续**可回收（**全部判定，含 classify 重复检测**）/ `gc_floor`=钳制下界。
8. **回归夹具必须顺序无关**：新埋点会加记录 ⇒ 绝对计数断言易被扰动；`serial_batch.py` 支持内联正则（`replay=/[1-9][0-9]*/`，**必须把字段名写进字面量**否则假命中）。
9. **低频模块（Weight 类）**：`CAUSE` 用**位掩码**（可多因同时为真）；`jump_error` 是"异常**事件**"不是"错误**状态**"⇒ 位掩码如实报告，但**绝不在跳变现场另加埋点**。
10. **★ 跑 196 回归时禁止注入 ACK**：夹具隐含依赖 BT-1（无 ACK ⇒ 队列写满）⇒ 回 ACK 会让 `qdrop`/`evict_inf`/`replay` 断言全失效（实测 A 52/56 · B 56/66 · E 13/23）。
11. **★ 断言不能隐含"执行期间只有本用例在写日志"**：四种形态依次踩过 —— 绝对计数（P2-D）/ 与历史相关（P2-E）/ **"恰好填满·恰好为 0"的边界（P2-G，F3-A 的 `total=496`）** / **"只有本用例在写日志"这个前提本身（P2-H，整套 F0-F8 全部失准 26 条）** ⇒ 改结构性判据 + 保证必然越界，但**保留精确记账断言**。**当"改判据"等于删掉记账本体时，就不再是修夹具而是降断言 ⇒ 此时正确解是"测试隔离"，不是继续打补丁。**
12. **★ 精确计数实验前必须先清零**：`logt stats` 的 `crit=`/`emit=` 是**设备侧真值**（不减实：`logt fill crit 62` ⇒ `Δcrit` 恰好 62），**但不清零时会被"回放/前序 Boot 记录"污染** —— 表现为 `crit` 增量点与事件时间差 18~37 s 的"孤儿"，极易误判成"门控失效"。**先 `fwipe`+`mwipe`+`creset`+`reset` 再测。**
13. **★ 门控只能加在"全部执行完成之后"**：对"纯事件型"函数（无状态判等）做日志去重时，判据必须晚于 GPIO/状态/事件/打印，**绝不提前 return**（否则改变对外语义）。另：**任何"取值用于日志"的变量都必须在源头被清零之前取**（`OPEN_MS` 类踩过 3 次）。

## 其它模块要点（详见对应文档）
- **TimeManager V2**：`time_init()` 必须晚于 `oled_init()`；RTC(PCF8563T,0x51) **复用 OLED 的 Wire，绝不 `Wire.begin()`/`setClock()`**；**SNTP 陷阱**：IDF v4.4 COMPLETED **瞬时**并自动回落 RESET，"从未同步"也是 RESET ⇒ **禁用 `status != IN_PROGRESS` 判成功**，用「callback 通知 + loop 延迟确认」；上板 **RTC 仍 `rtc_present=false`**
- **CloudManager**：两种下发格式 `compact = !doc["c"].isNull()` —— 旧 `{"cmd":"execute_action","id":"8010","ob":"<RUNTIME_ID>"}`（调试首选）/ 新 `{"c":"action",i,v,k}`；查 registry `{"c":"registry","i":..,"k":0}`（0=ACTION/1=TRIGGER/2=WORKFLOW）；命令 `id` **每条唯一**；**协议级消息旁路 CommandManager**；**无应用层 TX 队列**；**上行是 JSON 文本**；**离线直接返回 false 不排队**；`retry_interval` 是**死配置**；⚠️ 明文打印密码
- **烧录基线**：**`uploadfs` 整分区擦除**（`0x410000→0xffffff`），`esptool write_flash 0x10000 firmware.bin` **不碰** LittleFS；`data/` 被 gitignore ⇒ `tools/gen_config_version.py`+`gen_workflow_bin.py` 重建（⚠️ `WF_STG_META_VERSION` 升级必须同步脚本，否则 `VERSION_TOO_NEW` → 回退 `/workflow.json`）；**`bootstrap_version_file()` 必须在模块加载之后**

## 环境操作铁律（Windows + PlatformIO）
- **Bash PATH 可能损坏** ⇒ 前加 `export PATH="/c/Users/wang/.workbuddy/binaries/PortableGit/versions/1.2.0/usr/bin:$PATH"`
- 沙箱拦删 `.o`/`.elf` ⇒ **增量编译必然失败** ⇒ `PLATFORMIO_BUILD_DIR=.pio/build/xxx pio run`（全量 60–190s）；⚠️ 全新 BD 首次偶发 `sconsign314.dblite` 缺失 ⇒ 先 `mkdir -p "$BD/esp32-s3-devkitc-1"`
- `pio run -t upload` **可能完全不烧录** ⇒ 直接 `esptool.py write_flash -z 0x10000 <firmware.bin>`（`D:/platformIO/packages/tool-esptoolpy/esptool.py`）；Python 用 `C:/Users/wang/.workbuddy/binaries/python/envs/default/Scripts/python.exe`
- **串口回归**：`python test/serial_batch.py <COM> <log> <用例txt> [QUIET] [HIT_GRACE]`；用例 `<命令> ||| 期望子串`（断言串**不要加引号**）；命中后 0.35s 静默即返回；**QUIET**：fill ≤62 用 **3.0**，含 `fill 100/200/300` 用 **5.0**（`logt stats` **先排空 RAM 环**）；**开串口即复位板子**
- **⚠️ `serial_batch.py` 串口静默 1.5s 即提前返回**（不是等满 QUIET）⇒ **做不了"挂着等 N 分钟"的实验**。长静默观测必须写**单会话探针**（`.pio/p15run/storm_probe.py`）；且 **RAM 状态类实验（Dirty/5 分钟窗口/重试计数）必须单会话** —— 重开串口会复位、清掉 RAM。用 `logt stats` 的 **`emit` 增量**度量"某动作产生几条日志"（`emit` 恒等于 `cloud`）
- **★ 等待上限**：编译 `timeout 190` / 烧录 `timeout 120` / 串口回归 `timeout 185`；超时(124) **不重跑**，grep 结果文件或拆用例
- **同一文件的多个 Edit 必须串行**（并行互相覆盖且仍报成功，改完必须 grep 复核）；**Bash heredoc 写 Python 极易被引号搞崩且失败静默无改动** ⇒ 先 Write 成文件再跑
- ⚠️ `platformio.ini` 的 `platform`/`lib_deps` **未固定版本**（违反 PIO 生产规范，待处理）
- **用户偏好**：每阶段独立 commit；**禁 `git add -A`/`.`**（`.workbuddy/memory/`、`data/config/` 下有 force-add 的 tracked 文件）；测试资产放受跟踪的 `test/`；编译临时文件可直接删，其他目录删除需用户同意；文档（除 gitignore 内的）都可 commit
