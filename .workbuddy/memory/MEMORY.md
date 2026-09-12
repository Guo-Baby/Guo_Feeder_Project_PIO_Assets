# 项目长期笔记：锅氏自动猫粮机（Guo Feeder Project）

## 项目定位
ESP32-S3 N16R8 智能宠物供水/投喂设备，当前主攻「自动猫咪饮水系统」：水箱 + 重力供水 + 电磁阀 + HX711 称重。核心目标：按重量变化精确控水，异常时不持续漏水。当前 v0.7。

## 关键文档（改架构必须同步更新）
- `readme.md`：架构总纲、编码规范、MQTT 协议、System State / Trigger 注册规范
- `需求文档.md`：V2.1，8 项待开发计划 + 推荐顺序
- `jsonstorage开发架构.md` + `config_manager接口文档.md`：**Config 存储架构定版，当前行动基线**（⚠️ 旧笔记里的 `config manager开发架构.md` 文件名不存在，以此为准）
- `config_manager接口文档.md`：8 个云端命令参考 + 错误码 + 3 条踩坑规范（定版）
- `json_storage接口文档.md`：JsonStorage 全部接口语义 / 幂等性 / EOF 语义 / 踩坑清单（定版）
- `system_command接口文档.md`：SystemCommand V1 定位/边界/交互/3 条指令 + restart 保护规划（⚠️ restart 行为已被 V2 取代，以代码为准）
- `critical_operation接入规范.md`：**V2 Safe Restart 后各业务模块接入 Critical Operation 的标准**（acquire/release 配对、各模块接入点、测试方法）
- `workflow_cloud_interface.md`：**Workflow ↔ Cloud 唯一契约**（15 章 + 2 附录：架构分层 / JSON Schema / Definition-Runtime / Variant / Stable ID / 传输格式与紧凑键映射 / 7 命令详解 / 错误码 / 同步流程 / Action·Trigger 清单 / UI 注意事项）。**写云端/UI 只读这一份。**
- `workflow_cloud_sync_progress.md`：Workflow 云端同步**进度台账**（中断后先读它）。同主题还有 `workflow_cloud_sync测试报告.md`（56/56 断言 + 未验证边界）。
- `workflow_storage改造进度.md` / `bin_storage开发说明0910.md`：BIN 存储层实现与需求

## 软件架构（四层）
应用/能力层（Valve / Weight / DispenseGuard / Mijia / OLED）→ 自动化层（Workflow Manager + Capability Registry）→ 服务层（System State / Config / Event / Time / WiFi / Command / Log）→ 云通信层（Cloud Manager / MQTT）。

## 硬性编码规范
- 零全局裸变量，状态统一走 System State（`state_set_xxx` / `state_get_xxx`）
- 全程非阻塞：禁 `delay()`、禁 while 死等
- 模块单一职责，禁止跨模块直接读写内部状态，数据单向流动
- Action / Trigger 生命周期 `reset → start → poll → SUCCESS/FAILED`，状态放 `inst->runtime`，禁 static
- 新增 System State 必改三处：`system_state.h` 枚举（STATE_MAX 前）、`.cpp` state_map、产生模块
- src 只留有效代码，测试代码进 backup，一模块一 .cpp/.h

## Config 存储架构（已定版，写代码以此为准）
分层：**业务模块管"参数是否合法" → ConfigManager 管"配置" → JsonStorage 管"文件" → LittleFS**。
- ConfigManager：load / get / set / save / backup / recovery / version / cloud command / boot validate
- JsonStorage（已实现）：exists / size / remove / rename / read / write / write_atomic / read_chunk / write_chunk + 轻量流式接口；不理解任何业务语义。backup/recovery 由 ConfigManager 用 rename 实现，JsonStorage 不提供
- **双版本**：永远保留 Active + Backup（最新 + 上一个），版本 uint8_t 递增循环，不依赖时间戳（启动期时间不可信）
- **原子写**：写临时文件 → 完整写入 → 旧 Active 转 Backup → 临时文件转 Active，禁止直接覆盖
- **Boot Validation**：setup 全部模块初始化完成后才标记 Config Valid；未到达则配置可疑，下次启动走 Backup Recovery
- **文件组织**：按模块拆分（`/config/wifi.json`、`oled.json`...），Workflow 拆为 `/workflow/workflow_N.json`（最多 16 个）
- **版本独立**：`config_version.json` 单独管版本，业务 JSON 保持纯数据
- **云端修改**：config_set → 原子保存 → Active/Backup 滚动 → 重启生效（不做局部 reload / 实时生效）
- **Flash 优化**：set 时比对旧值，无变化不写盘
- **大文件**：一律分块流式（FILE_BEGIN / CHUNK / END），禁止整文件 malloc 进 RAM
- 明确不做：业务参数合法性判断、debug 开关（走编译期宏）、实时通知配置变化

## 当前开发顺序（Config 专项）
① JsonStorage（**已完成**：`src/json_storage.h/.cpp`，编译通过，待上板验收）→ ② ConfigManager → ③ Workflow JSON 存储改造 → ④ Config 云端 Command → ⑤ Log Manager

### JsonStorage 实现要点
- init 用 `LittleFS.totalBytes()==0` 探测挂载，不重复 begin（main.cpp:31 已挂载）
- 原子写 = tmp 完整写入 + 大小校验 + `rename(tmp,path)`，失败清理 tmp，不生成 backup
- write_chunk：offset==0 用 `"w"` 截断，offset>0 用 `"r+"` 且拒绝越界 offset
- read 有 32KB 上限；不依赖 ArduinoJson；日志走 `JsonStorageLogCallback` 预留

### 环境注意（PlatformIO）
- 沙箱 `safe-delete` 拦截删除 `.o`/`.elf`，**增量编译必然失败**（SAFE_DELETE_FAIL_CLOSED），非代码问题
- 解法：`PLATFORMIO_BUILD_DIR=.pio/build/xxx pio run`（PlatformIO 6 无 `--build-dir`），全量约 130s
- board 识别为 N8、**PSRAM 未启用**，实际硬件 N16R8

## 开发计划（需求文档 V2.1）
1. Config 模块完善（见上，拆为 JsonStorage + ConfigManager）
2. WiFi 初始化优化　3. Cloud Protocol 文档同步　4. 系统内置命令
5. Log 日志模块（分级 + Ring Buffer + 串口 / MQTT）　6. 米家温湿度定版
7. RTC 芯片支持（横切影响 Workflow 定时 / Event 时间 / Cloud 时间字段）
8. 按键 + OLED UI（按键走事件队列，统一 CommandManager 入口）

## Critical Operation 接入进度（Safe Restart V2）
规范见 `critical_operation接入规范.md`；全系统唯一 `ESP.restart()` 在 `system_command.cpp:312`。

| 模块 | 状态 | 接入方式 |
|---|---|---|
| ConfigManager | ✅ 已接入 | `set_begin()` acquire（`s_pending_save` 去重）→ Save 落盘成功后 `pending_save_finalize()` release + request_restart；**Save 失败不 release、不重启** |
| Workflow | ✅ 已接入并通过自查（2026-09-03） | 每个 Workflow / 每个临时 Action 各自 +1 / -1；统一收口函数 `workflow_terminate()` / `temp_action_complete()`；接入报告 `workflow_critical_operation接入报告.md`，自查报告 `workflow_critical_operation自查报告.md` |
| WOF（阀门） | ❌ 未接入 | 计划：动作开始前 acquire → 完成后 release |
| TimeManager / RTC | ✅ 已接入（2026-09-04） | 仅 RTC **写**是 Critical（`time_rtc_calibrate()` / `time_set_manual()`），读不是；acquire 被拒直接 return 不 release，成功/失败都 release |
| ComputerReset | ⛔ **明确不接入** | AI_TASK §9：只是 800ms GPIO 脉冲，非 Flash 写 / 阀门保持 / 电机运行类危险操作，不阻止重启 |

**Action 引擎调用时机差异（写异步 Action 必看）**：
- 临时 Action（workflow.cpp:2172-2201）：`start()` 后**同轮立即** `poll()`
- Workflow Step Action（workflow.cpp:2093-2100）：`if/else`，首轮只 start，次轮起才 poll
- 两种引擎下 `poll()` 都可能带 `result != RUNNING` 被调用（如 start 已置 FAILED），**不得**在此情况下做全局强制复位，否则误伤并发实例

**接入铁律**（踩过/规避过的坑）：
- release **不能**放进会在中途 `return` 的函数里（如 `workflow_notify_finish()` 在 callback 为空时提前返回）→ 会漏 release 导致**系统永久无法重启**
- 强制终止路径（`workflow_stop` / `workflow_disable` / `workflow_clear`）必须补 release
- `workflow_init()`（main.cpp:57）**早于** `system_command_init()`（command_manager_init :71），init 里不得调用 system_command API（自旋锁尚未初始化）
- 用"持有标记"（bool）实现 release 幂等，避免触发 `critical op release underflow` 错误日志
- 变更 init 顺序前务必复查上述时序

**跨任务并发铁律（P0 级，务必遵守）**：
- `workflow_start()` 经 `command_manager_execute()` 由 **esp-mqtt 任务**调用；`workflow_task()` 在 **loop 任务**。两者可能在不同核并发。
- 收口函数必须**先 Release、后置 state**。若先置 state，会打开窗口让 mqtt 侧的新 `workflow_start()` 通过 `state != RUNNING` 守卫并 acquire，随后被 loop 侧的 Release 把 flag 清成 false → **count 永久泄漏，系统再也无法重启**。
- 通式：**"释放"必须早于"让对象对外可见地变为可重新开始"**。任何"先改状态、后释放"的写法都有同类风险。
- 同理，`enqueue_temp_action()` 中 `critical_held = true` 必须早于 `queue_wr_ptr` 提交。

**已知遗留隐患（待独立立项）**：
- P2：Temp Action 环形队列 `queue_wr_ptr` / `queue_rd_ptr` 跨任务无 volatile / 原子 / 临界区保护（**既有问题**，非 Critical Operation 引入）。修复需为队列加临界区，但入队路径要跑 JSON 反序列化，需评估持锁时长与死锁风险。

## TimeManager V2（已定版，2026-09-04）
- **双时间源**：SNTP 最高可信（ESP-IDF `esp_sntp_*`，平滑同步，固定 24h 周期，单调计时）；PCF8563T RTC 为启动恢复 / 离线兜底（I2C 0x51，**复用 OLED 的 Wire 总线，绝不 `Wire.begin()` / `setClock()`**）。
- **初始化顺序铁律**：`time_init()` 必须晚于 `oled_init()`（main.cpp 65/70），否则 I2C 总线未建立，RTC 探测必失败。
- **SNTP 状态机陷阱（最重要）**：IDF v4.4 `esp_sntp.h` 明确 COMPLETED 是**瞬时状态**并自动回落 RESET，而"从未同步"也是 RESET ⇒ **禁止用 `status != IN_PROGRESS` 判成功**。现用「callback 通知 + loop 延迟确认」：seq 变化（可靠证据）+ 排除 IN_PROGRESS（adjtime 未收敛，30s 超时兜底）+ RESET 需过 3s 稳定窗口。
- **callback 只做轻量标记**（tv / 时刻 / seq++），严禁 I2C / Serial / 长耗时。
- **RTC 校准写入值一律取 `time(nullptr)`**（已稳定的系统时间），不用 `sntp_sync_tv_sec`（服务器给出的瞬时值）。阈值 2s（`rtc.calibrate_threshold_sec`）；写失败不置 `time_valid=false`，等下次 SNTP 重试。
- **RTC 寄存器**：读 0x02 起连续 7 字节（顺序 sec,min,hour,day,weekday,month,year ⇒ month=raw[5]&0x1F、year=raw[6]）；写 0x00 起；sec bit7=VL 置位即时间不可信。
- **内部统一 UTC/Unix**，时区（默认 GMT+8）仅用于显示；本地时间禁止写 RTC。
- **查询命令**：`{"c":"system","i":"<唯一>","p":{"o":"time"}}` → 返回 system_* / rtc_* / source / last_ntp_sync / timezone_offset_h，非阻塞。
- 上板状态：SNTP 已实测通过；**RTC 仍 `rtc_present=false`**（硬件接线 / 供电 / 地址待查，串口看 `[Time] RTC probe failed (addr=0x51 err=N)`）。

## 冲突与依赖
- 协议冲突：README 记 `{cmd,ob,id,pl,src,ts}`，Cloud Manager 已重构为 `{c,i,v,k,p}`；以代码 + 需求文档为准
- Config 是全局底座：米家参数、Log 等级、RTC 参数都要进 Config
- 需求文档聚焦饮水系统；README 含投喂 / 出粮 / App 等远期目标，排期以需求文档为准

## 云端下发命令的两种格式（cloud_manager.cpp:1048 起）
- `compact = !doc["c"].isNull()` —— 带 `c` 走新格式，否则走旧格式透传
- **旧格式（无需 version/stable_id，调试首选）**：`{"cmd":"execute_action","id":"8010","ob":"<RUNTIME_ID>"}`
- **新格式**：`{"c":"action","i":"...","v":<action_version>,"k":<stable_id>}`；`v`/`k` 不匹配 → `ERROR_VERSION_MISMATCH` / `ERROR_INVALID_OBJECT`
- 查 registry：`{"c":"registry","i":"...","k":0}`（k: 0=ACTION / 1=TRIGGER / 2=WORKFLOW，见 cloud_manager.h:82）
- **Capability Registry 陷阱**：新增/删除任何 Action 都会令 **version+1 且全部 stable_id 按 runtime_id 升序重排**（capability_registry.h:56），云端缓存的 mapping 必须重新拉取
- 命令 `id`（或新格式 `i`）每条必须唯一，设备按 cmd_id 去重

## 内存分配铁律（PSRAM）
- 硬件 N16R8 = 8MB PSRAM，**实测可用**（boot 日志 `PSRAM size: 8386279`）；
  platformio.ini 已配 `memory_type = qio_opi` + `psram_type = opi`。
- **栈永远在内部 RAM，PSRAM 只能用于堆。** 栈溢出的解法：① 大对象移出栈 → ② 堆分配走 PSRAM。
  `-DARDUINO_LOOP_STACK_SIZE=16384` 与 PSRAM 是两件事，都要保留。
- **项目统一范式：大对象 `MALLOC_CAP_SPIRAM` 优先，失败回退 `MALLOC_CAP_8BIT`**
  （参考 `capability_registry.cpp: registry_buf_alloc` / `workflow.cpp:782` / `workflow_storage.cpp:1481`）。
- 任何 >1KB 的结构禁止在栈上声明（setup/loop/命令路径尤其危险）。
- ArduinoJson 7.4.3 **无** PSRAM 支持，所有 JsonDocument 缓冲仍占内部 RAM（要迁移需自定义 Allocator）。

## Workflow 云端同步协议（2026-09-13 契约修订版，已上板 171/171）
**定位模型（★最重要，2026-09-13 变更）**
- **`p.id` = Slot 整数（0..15），是唯一定位键**；`workflow.id` 是用户业务 ID，**允许重复、允许内容完全相同**；`stable_id` 只是 Registry metadata。
- 已**移除**「按 workflow.id 搜索对象」——`workflow_find_index_by_id()` 禁止用于云端定位。
- `list` / `sync_info` 的 `count` = **占用 Slot 数**（`workflow_get_occupied_count()`，含 valid=false），**不是** `capability_get_workflow_count()`（Registry 按 runtime_id 去重，重复 id 时两者不等）。
- `list` 按 **Slot 升序**输出 `{slot, id, variant, valid, stable_id}`。

- `Workflow.variant` = uint32 **内容版本号/修订号**（不是 hash/指纹，相同 variant 不代表内容相同）：create=1 / set 有变化 +1 / set 完全一致不变 / delete(valid=true)+1 / **重复 delete 不变（幂等 no-op）**，绝不用时间戳；存在 Meta entry（Meta v3，89B）。
- `Workflow.valid` = **RAM 标志**（不进 BIN）：delete→false，create/set/加载→true；`workflow_is_valid()`。重启后 valid=false 不再加载 → `get` 返回 **error 5**。
- CapabilityRegistry 文件 **v2**：entry = stable_id + **object_version(uint32)** + runtime_id；workflow 的 object_version 直接取 `wf->variant`；magic `AP2A/AP2T/AP2W`（改格式必须换 magic 强制重建）。
- **7 条命令**：`workflow.sync_info / list / get / create / set / delete / save`。
  - `sync_info` 只回 `registry_version / registry_checksum / count / dirty`（轻量入口，不返回列表）
  - `get` 的完整对象在 `workflow{}`（云端紧凑键 `o`），**`valid`/`slot`/`k` 放在它外面**（`workflow{}` 要能原样回传 set）
  - `set` 幂等靠 `workflow_definition_matches_json()` canonical 比较（字段定序 + 参数按 name 排序 + 值带 `i/f/b/s` 前缀）→ 响应带 `changed`
  - `save` **逻辑全局、物理逐 Workflow 事务**：成功者立即清自己的 Dirty，失败者保留，循环不中断；任一失败 → 错误码 10；仅当 `!dirty_any()` 才 release Critical；`dirty=false` 时 `saved=true, restarting=false`（不空转重启）
  - `restarting:true` **只表示重启请求已被接受**，不代表已完成 reboot
- **严格 Cloud Contract**：Step `type` 必填（缺失不再默认 action）、`steps>16`、`params>8` 一律**整体拒绝（错误码 11）**，绝不截断/降级。校验在 create 分配 Slot 之前、set 幂等比较之前。
- delete 是**逻辑删**（valid=false + variant++），不物理删 BIN、不杀 Runtime；**立即落盘**（meta.valid=0），随后清本对象 Dirty —— 这是刻意的：保留 Dirty 会让后续 save 把 valid 置回 true 撤销删除。重启前 `list` 仍可见（valid=false）。**delete 后无需再 save。**
- 串口调试透传：`cm {"cmd":"workflow.xxx","ob":"-","id":"8010","p":{...}}`（main.cpp `cm_console`），与 MQTT 走同一 `command_manager_execute()` 链路。
- 云端上行两种形态：CommandManager 明文（`status/cmd/id/command/timestamp`）→ CloudManager 紧凑键（`s/c/i/t/o/k`，见 cloud_manager.cpp:540-720）。

### 四条极易踩的语义（写云端/UI 必看）
1. **`stable_id` ≠ Slot 编号**，= 按 `runtime_id` 字符串升序的下标。新增/改名一个字母序靠前的 id → **后面所有对象的 stable_id 整体平移**。⇒ 云端以 **`p.id`（Slot）为唯一主键**，`stable_id` 只作定位；`registry_version` 变了就重建映射。
2. **`enable` 缺省值两条路径不一致**：JSON 导入缺省 `true`，BIN 加载缺省 `false`。⇒ 云端永远显式传 `enable`。（代码未改）
3. **`step.id` 写错不报错**：未注册的 id 被安静保存但永不执行。⇒ 云端提交前用注册清单校验。
4. **`registry_version` 变化 ≠ 只有 Workflow 变了**：Action/Trigger 映射变化同样会改变它。⇒ 必须再走 `workflow.list` 才能判断。

### 形态约束（高危缺陷已修，dd49512）
`workflow_pick_object()` **必须**要求内容嵌套在 `p.workflow` 下。
曾允许平铺 → `set` 的定位字段同为 `p.id`，导致 `p:{"id":"WF1"}`（只想定位）
被当成完整 Workflow 而**静默清空目标 steps**。现在平铺一律返回错误码 6。
**教训：定位字段与内容载荷绝不能共用同一个 key。**

- **铁律**：新增 Workflow 字段时，`workflow_storage_load()` 的每个拷贝点都要回填，否则重启丢值（variant 曾漏过）。

## 文件系统烧录与初始基线（2026-09-13 定版）
- **`uploadfs` 会整分区擦除**（实测 `Flash will be erased from 0x00410000 to 0x00ffffff`），
  `/config/*` 与 `/workflow/*.bin` 全清；日常 `esptool write_flash 0x10000 firmware.bin`
  **不碰** LittleFS。
- `data/` 被 `.gitignore` 忽略（含 WiFi 密码 / CA），初始文件不在版本库 →
  用 `tools/gen_config_version.py` + `tools/gen_workflow_bin.py` 重建：
  - `gen_config_version.py` → `data/config/version.json`（8 模块 version=1）
  - `gen_workflow_bin.py` → `data/workflow/meta.bin` + `wfNN/stepMM.bin`
    （Meta v3 = 12B header + 16×89B entry；Step v1 = 16B header + payload；
    CRC32 init 0xFFFFFFFF / poly 0xEDB88320 reflected / final xor 0xFFFFFFFF；
    `meta.entry.crc32` = 各 step payload CRC 依次 u32 LE 追加后的 CRC）
  - ⚠️ `WF_STG_META_VERSION` 升级（v3→v4）必须同步脚本，否则 `VERSION_TOO_NEW`
    拒绝 → 回退 `/workflow.json`（功能不丢，BIN 基线失效）
- **ConfigManager version 自愈**：`bootstrap_version_file()` 为已加载模块置 version=1。
  必须在**模块加载之后**调用（config_init 步骤 6），否则 `loaded` 全 false。
  背景：`save_version_file()` 跳过 `version==0` 的模块 → 缺 version.json 时
  文件永远生成不了，每次启动 `[CFG][E] version reload failed`。
- **验证技巧**：串口批量脚本开串口会复位板子，"烧录后第一次启动"捕获不到。
  用 `esptool --after no_reset write_flash 0x410000 littlefs.bin` 留在 bootloader，
  再开串口即可捕获真正的首次启动日志。

## 新增硬件 Action 模块速查
- 模板见 `新增动作模板.md`；参照实现 `src/valve.cpp`（即时完成）/ `src/computer_reset.cpp`（异步 + 单例 GPIO 仲裁）
- 接入点：main.cpp `setup()` 第四层（必须在 `workflow_init()` 之后）+ `loop()`
- Capability Registry **自动扫描** workflow 注册表，新增 Action 无需改动任何既有模块
- Action runtime id 惯例：全大写下划线（`COMPUTER_RESET` / `VALVE_OPEN` / `WEIGHT_ZERO` / `MI_THERMO_START_SCAN`）
