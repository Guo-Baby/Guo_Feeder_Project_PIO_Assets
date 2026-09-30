# Guo Feeder Project 长期笔记
ESP32-S3 N16R8 宠物投喂/饮水 v0.7；主攻自动猫咪饮水（水箱+重力供水+电磁阀+HX711 按重量控水）。
**只留铁律与踩坑；细节看专项文档。**
> 专题：**LogManager（含铁律 1–32） → `MEMORY-logmanager.md`** · 每日日志 → `YYYY-MM-DD.md`（append-only，含各阶段 commit 与验证数据）

## 文档索引
`readme.md`(架构) · `需求文档.md` V2.1 · **`未修复的问题.md`（全项目问题权威清单：`LOG-n`/`VALVE-n`/`R-n`/`T-n`/`EVT-n`/`LV-n`/`P0-n`/`DSP-n`/`PROTO-n`/`OLED-n`）** · `AI_RULES.md` · Config：`config_manager接口文档.md`+`jsonstorage开发架构.md`+`json_storage接口文档.md`+`bin_storage开发说明0910.md` · `system_command接口文档.md`+`critical_operation接入规范.md` · `cloud_protocol.md`(V2.0) · `workflow_cloud_interface.md` · **`docs/Cloud-APP-Platform-Plan.md`（云端/安卓端平台选型权威）** · `docs/LogManager-Integration-Guide.md`+`docs/P2_Log_Integration_Matrix.md` · `log模块历史/`

## ★★ 当前阶段（2026-09-30 · **云端 / 安卓端平台规划起步**）
> **权威文档：`docs/Cloud-APP-Platform-Plan.md`**（含全部核实来源）；本节只留"铁律"。
- **目标架构**：设备侧 **Homie 5 自描述（复用 Capability Registry）** + 前端 **MQTT-Tiles**（MIT/Vue/Quasar）托管 **Cloudflare Pages** + **APP 直连 EMQX WSS 8084** + **Worker 仅负责鉴权/绑定/临时凭据** + **D1 经 EMQX 数据集成落历史**
- **★ 控制平面 / 数据平面分离**（核心决策）：低频控制走 Worker（登录 → `GET /api/device/<id>/credential` → 返回**一次性 MQTT 临时凭据**）；高频数据 **APP 直连 EMQX**，**完全不消耗 Worker 请求额度**；落库由 EMQX 规则引擎异步 `→ HTTP → ingest Worker → D1`
- **★ 四条联网核实的硬约束**（决定架构，勿凭记忆推翻）：
  1. **EMQX Serverless 只开 8883(mqtts) + 8084(wss)**，1883/8083 **不支持**；WSS URL **必带 `/mqtt` 路径**（`wss://<域名>:8084/mqtt`）；**必须 SNI**，否则拒连(错误码 `-5`)；强制 TLS 无明文
  2. **CF Worker 免费 10ms CPU/请求**，但**外部 `fetch()` 等待不计 CPU** ⇒ 可做"IO 为主"转调（EMQX REST / D1）；**禁止**在 Worker 内压缩/解析大 JSON、长循环（报 `Error 1102`）；100k req/天
  3. **D1 免费 100k rows write/天**；**KV 仅 1,000 write/天** ⇒ **历史数据必须落 D1，绝不能用 KV**
  4. **EMQX 数据集成（规则引擎 → HTTP）在 Serverless 可用**，Beta 期免费 ⇒ D1 落库**设备侧零改动**
- **★ Homie 5 要点**：根主题 `homie/5/<device-id>/`；`homie` 域**可自定义**、**版本段 `5` 不可改**；控制器发现订阅 `+/5/+/$state`；`$state` **必须 retained + LWT=`lost`**；生命周期 `init`→**`ready`**（`online` 是 Homie 4 旧值）；`$description` **retained** 且**仅可在 `$state`=`init`/`disconnected`/`lost` 时变更**；写属性走 `<node>/<prop>/set`；空 payload on `$state` = 设备注销
- **★ 映射复用度极高**：`capability_export_*_registry()` → `$description` JSON；`registry.version`（uint32 递增）→ `$description.version`；`workflow.variant`/`object_version` → 已有；挂 `LOG_REG_REBUILT` 回调重发即可
- **★ 落地方案**：新增 `homie_bridge`（放 CloudManager 内，**只做序列化/主题映射**），写入方向统一转成现有命令 → 走 **CommandManager**（复用 `cmd_id` 去重 + `v`/`k` 校验）；**不新增 Action/Trigger/EventId/ParamId**
- **★ Homie domain 选 `homie` 而非 `guofeeder`**：MQTT-Tiles 默认按 `homie` 发现，改自定义域反而要改前端源码；**旧 `guo_feeder/up|down|log` 保留不动**，Homie 树是**并列的面向 UI 的投影** ⇒ 即使 Homie 层出问题主链路不受影响
- **★ 前端选 MQTT-Tiles**：官方**没有自动发现逻辑**，必须自己补（`subscribe('homie/5/+/$state')` → `ready` → 取 `$description` → 动态生成 tile）；**必做项**：静态 tile 无法生成唯一 `id`，而设备**对重复 `id` 静默丢弃**（`MQTT_DUP_CACHE_SIZE=10`/`TTL 30s`）⇒ button tile 每次发布必须生成唯一 `id`
- **★ 安卓端**：**Eclipse Paho Android 2018 停更**（需 Jetifier、无 MQTT 5.0）⇒ **弃用**；用 **HiveMQ MQTT Client**（`com.hivemq:hivemq-mqtt-client`，MQTT 5.0、异步非阻塞、内置指数退避重连）；先 **WebView 壳**（一套代码两端）→ 需要后台常驻/蓝牙配网再上原生
- **★ 本地工具全落 D 盘**（脚本 `docs/scripts/setup-cloud-app-env.ps1`，默认 dry-run）：npm 全局包+缓存 → `D:\Guo_Feeder_Project\tools\{node-global,npm-cache}`；Android Studio/SDK/JDK17/`GRADLE_USER_HOME`/AVD → `D:\Android\*`。**已具备**：Node 22.22.2、Git、Python 3.13.12、PlatformIO 6.2.0、**MQTTX（已装）**、VS Code、Wireshark；**待装**：Wrangler、pnpm、Android Studio、JDK17
- **⚠️ `.ps1` 脚本必须纯 ASCII**：PowerShell 5.1 读无 BOM 的 `.ps1` 按 ANSI 解码 ⇒ **中文注释直接 `ParserError`**，首版实际踩到
- **★★ 量产阻塞项（写 APP 前必须先解决）**：**6.1 Topic 无 `device_id`**（`cloud_protocol.md` §1.1 明确"Topic 是完整字符串，设备不会替换 `{uid}`"）⇒ **两台设备会都订阅同一 `down`，一条命令两台都执行**；**Homie 5 强制 `<device-id>` 唯一 ⇒ 不通则 Homie 树立不起来**；建议模板化 `guo_feeder/<device_id>/down` · **6.2 EMQX ACL 必须按 device_id 隔离**（否则 APP 复用设备账号即可控制任意设备）· **BT-1** 云端**从不返回 `log_ack`** ⇒ Flash 日志段**永不回收** · **DEF-2** 明文打印 MQTT 账号密码到串口 · **9.7** `platformio.ini` 未固定版本
- **分阶段**：P0 Topic 模板化+`device_id` → P1 EMQX ACL+临时凭据接口 → P2 `homie_bridge` → P3 EMQX 数据集成→ingest Worker→D1 → P4 MQTT-Tiles 改版 → P5 Pages 部署+WebView 壳 → P6（可选）原生 Kotlin
- **用户待确认**：① Homie domain（建议 `homie`）② 前端（建议 MQTT-Tiles）③ 安卓路线（建议先壳）④ `device_id` 来源 ⑤ 是否现在装 Android Studio（约 25–30 GB）

### LogManager P2（2026-09-22 **已冻结**，细节见 `2026-09-2x.md` 与 `MEMORY-logmanager.md`）
- **P2 完成判定 5 条全达成**；权威覆盖状态 = `docs/P2_Log_Integration_Matrix.md` **§15**。**已接入 16 模块**（Storage·Config·WiFi·Cloud·Time·Workflow·Weight·Valve·Command·ComputerReset·Registry·Event·DispenseGuard·BLE·bin_storage·workflow_storage；零宿主 23）
- **P2 明确不包含**（转独立 backlog，不再扩大埋点）：OLED(`OLED-1`)·`test_mqtt` 生产路径清理·MQTT 凭据串口泄露整改·Reliability 专项(`R-7`/`R-8` DEFERRED)
- **★ 已定案（用户裁决 09-22）**：**允许同一 EventId 按影响范围使用不同 Level** —— `0x0306` 在 6-C 用 **ERROR**、在 7-1-A 用 **WARN** 均保留；**不要**改 `log_events.h` 注释
- **★ P3 候选日志缺口**（`workflow_storage.cpp`）：`:1525` rename 失败 · `:1500` meta commit 失败 · `:1450/:1464` save readback/CRC 失败 · `main.cpp:447` LittleFS mount 失败（`0x0106` 仍零宿主）
- **其它待开工**：`load_meta()` 三处失败分支的 `stage_cleanup_all()` 不对称 · `LV-1..3` · `PROTO-1/2` · `P0-1b` · `P0-4` · `state_table[].type` 死存储

### ★ Dispense 边界（Phase 4 审查定论；细节见报告文件）
- **Dispense 本体不存在**：只有 `dispense_guard.cpp/.h`；**无电机/步进/出粮状态机**；重量反馈闭环属 **Water 域**（`VALVE_OPEN` → `weight_decrease` Trigger → `VALVE_CLOSE`）
- **★ 命名冲突**：代码里 "Dispense" = **供水**（`0x05xx`）；**出粮已叫 "Motor"**（`0x0Fxx` 已预留）⇒ 推荐 `DispenseManager`（编排）+ `Motor`（驱动）
- **三硬约束**：① 重量反馈必用 `weight_get_gram()`（读 System State 会得 30 s 陈值）② `timeout_ms` 必填并钳位 ≤60 s（Temp Action FIFO 队头阻塞）③ **DispenseGuard 不能守护出粮**（只关水阀）；`DSP-1..10` 未决（P0 = 命名 / 日志段 / 队头阻塞）
- **`0x0511`** 埋在 `dispense_guard_event_callback()`（过滤后、`valve_force_close()` **前**）⇒ 记"**决策**"，与 Valve 侧"**动作**"（`0x0505`）互补：后者丢次数，前者 **Σ `LOG_P_COUNT` = 真实响应次数**。**`0x0501–0x0504` 未占用**（安全响应是**终止**供水 ⇒ 复用 `START` 会伪造事实）

### ★★ 铁律 29–32（**完整论述在 `MEMORY-logmanager.md`**）
- **29 纯转发模块的埋点数天然为 0**：列全部数据流 → 逐个问"**定义点**在哪"（在别的模块 ⇒ 别人记）→ 只剩"自己失效"这类自指事实时才记。**"总线类模块记几条"没有统一答案**
- **30 突发合并三条硬约束**：① 被合并埋点**必须至少带 1 个参数**（`base_n==0` 时折叠计数被**静默丢弃**）② 参数须 `< LOG_MAX_PARAMS(8)` ③ 折叠对调用方**透明**（`logt fill` 的 `queued=N` **恒等于 N**，真实记录数看 `stats` 的 `emit`/`flash` 增量）
- **31 新增日志前必须问"现有日志缺的是哪一维"** —— "**来源可分**"与"**次数可数**"是两件独立的事；**答不上来就说明不该新增**
- **32 统计"宿主覆盖率"必须检索三类证据**：① 字面实参 ② **`log_emit0(...)` 变体** ③ **映射函数的 `return LOG_X;`**（`cfg_bridge_event()`/`stg_bridge_event()`），并剥除注释 —— 否则 **`0x02xx`/`0x03xx` 会被系统性误判为整段零宿主**（首版实际踩到）。工具 `.pio/p5run/evtid_audit.py`
- 配套：`LOG_P_CAUSE`=0x16（`0x1F` 是 `LOG_P_REASON`）；埋点前核对两侧"计数语义"是否同频（N:1 冗余判据）

## 硬性规范
- 零全局裸变量，状态走 System State；新增 State 改三处：`system_state.h` 枚举 + `.cpp` state_map + 产生模块
- 全程非阻塞：禁 `delay()`、禁 while 死等；队列定容、满则拒绝+告警；内部模块**不处理 JSON、不传 String**
- Action/Trigger 生命周期 `reset→start→poll→SUCCESS/FAILED`，状态放 `inst->runtime` 禁 static
- **内存**：**栈永远在内部 RAM，PSRAM 只能用于堆**；**>1KB 结构禁止上栈**（loopTask 16KB，余量≈6.5KB）；**`MALLOC_CAP_SPIRAM` 优先、失败回退 `MALLOC_CAP_8BIT`**；`-DARDUINO_LOOP_STACK_SIZE=16384` 与 PSRAM 都要保留；ArduinoJson 7.4.3 **无** PSRAM 支持
- **Config**：业务管参数合法 → ConfigManager → JsonStorage → LittleFS；双版本 Active+Backup（`uint8_t` 递增、**不用时间戳**）；**原子写** tmp→校验→旧 Active 转 Backup→tmp 转 Active；**Boot Validation**；Cloud 改动**一律重启生效**；新增模块改 4 处：`CONFIG_MODULE_COUNT`、`kModuleNames[]`、模块名宏、`data/config/<m>.json`
- **★ 改设备端配置 = "set + save" 两步**：`config_set`（`:2341`）**只改 RAM + 置 dirty + 排 5 分钟重启**（回 "restart required"），**不立即落盘**；`config_save`（`:2716` → `save_module` 原子写 + `version++` + 清 dirty，回 `{"saved":true,"dirty":false}`）才立即落盘并安全重启（10 s）。**兜底**：漏发 save 也会在 5 分钟后由 `restart_timer_check()` 自动 save+重启 ⇒ **不丢数据**。`system/wifi_config` 走 `set_begin(...,true,...)` 同样自动落盘（**实测无缺陷**；09-26 曾误判为"只置 dirty"，09-27 纠正）。也可用 `{"cmd":"system","ob":"restart"}`（内部先 save）
- **★★ config 落盘链路已实测验证正确（09-27 纯 MQTT 全链路）**：新增→修改→删除字段，**每次真实重启后查询均持久**，`version.json` 逐次 +1。**无需修复**。设备重启后发上线通告 `{"cmd":"system","id":"online","src":"device"}`（**未压缩**，用 `cmd`/`id` 而非 `c`/`i`）
- **★ 设备端配置可能与仓库 `data/` 不一致（已遇 4 例）**：`wifi.ssid=__P2C_NOAP__` · **`weight.zero_offset=-1`（应 `-800750`，否则称重不准）** · **`valve.safety_timeout_sec=2`（应 `300`；2 秒会让阀开 2s 就被强关 + 推 `EVENT_VALVE_ERROR`，连累 DispenseGuard）** · **`rtc.enable=false`（板子未焊 RTC 芯片，09-28 用户确认；仓库 `data/config/rtc.json` 已同步为 `false`）** ⇒ 排障/点检**先读设备端实值**（`config_query`），**不要默认仓库 `data/` 就是设备端现状**。未来焊上 PCF8563@0x51 后须改回 `true`，否则每次 uploadfs 都改错
- **★ 修正设备端配置用"精确 `config_set`+`config_save`"，不要 `uploadfs`**：uploadfs = **整区重建 littlefs** ⇒ ① 设备端独有文件**全丢**（`/factory/*.json`、`/config/.commit`、**设备上配的 workflow BIN**）② `version.json` 被仓库值（全 1）覆盖 ⇒ **版本号重置**（version 仅用于云端乐观锁，模块文件不含它 ⇒ 功能无碍；云端/APP 缓存的旧版本号需重新 query 同步）

## Critical Operation（Safe Restart V2）
- 全系统**唯一** `ESP.restart()` 在 `system_command.cpp:312`；接入 ConfigManager（Save 成功才 release+request，**失败不 release 不重启**）/ Workflow（每 Workflow、每临时 Action 各 ±1）/ TimeManager（**仅 RTC 写**）；**不接入** ComputerReset
- **release 不能放在会中途 return 的函数里**（`workflow_notify_finish()` callback 为空时提前返回 → 漏 release = **永久无法重启**）；强制终止路径（`workflow_stop`/`_disable`/`_clear`）必须补 release；acquire 被拒**直接 return 不 release**
- **跨任务并发（P0）**：`workflow_start()` 在 esp-mqtt 任务、`workflow_task()` 在 loop 任务 ⇒ 收口必须**先 Release、后置 state**。通式：**"释放"早于"让对象对外可见地变为可重新开始"**
- **Action 引擎差异**：临时 Action 同轮立即 `poll()`；Step Action 首轮只 start；两者 `poll()` 都可能带 `result != RUNNING` → **不得**在此全局强制复位

## Workflow 云端契约（09-13 定版；**细节全在 `workflow_cloud_interface.md`**）
- **`p.id` = Slot 整数(0..15) 唯一定位键**；`count` = 占用 Slot 数（含 valid=false）；`variant` = uint32 内容版本号（**非 hash、绝不时间戳**）；`valid` = **RAM 标志**（不进 BIN）⇒ 重启后 `get` 返回 **error 5**
- `save` = **逻辑全局、物理逐 Workflow 事务**；`delete` 逻辑删且**立即落盘**后清 Dirty（留 Dirty 会让 save 把 valid 置回 true **撤销删除**）
- **严格 Contract**：Step `type` 必填、`steps>16`、`params>8` 一律**整体拒绝(11)**
- **四条易踩**：① `stable_id` 按 `runtime_id` 字母序生成，新增/改名会让后续**整体平移**；② `enable` 缺省两路径不一致（JSON `true`/BIN `false`）⇒ 永远显式传；③ `step.id` 写错**不报错**但永不执行；④ `registry_version` 变 ≠ 只有 Workflow 变
- **形态约束**：内容必须嵌套在 `p.workflow` 下 —— 平铺会让 `p:{"id":"WF1"}` 被当完整 Workflow 而**静默清空 steps**。**定位字段与内容载荷绝不能共用同一 key**
- **★ 下行紧凑格式由「有无 `c` 字段」判定**（`cloud_manager.cpp:1154`）：**无 `c` = 旧格式**（`cmd`/`ob`/`id`/`pl`，**原样透传、零版本校验，最稳**）；**有 `c` = 新格式** —— 且 `c:"action"` **与** `c:"execute_action"` **都**走 action 分支，**必须带 `v`=registry version、`k`=stable_id 且必须匹配**，否则回 `version mismatch` 且**不执行**（这是"旧命令没响应"的真相：带 `c` 却漏 v/k）
- **★ ComputerReset 是 Action、不是 system 命令**（runtime_id=`COMPUTER_RESET`，GPIO8 脉冲 800 ms；2026-09-26 实测 `stable_id=0`、`registry version=3`）。正确下发：`{"cmd":"execute_action","ob":"COMPUTER_RESET","id":"<唯一id>"}` 或 `{"c":"action","i":"<唯一id>","v":3,"k":0,"p":{}}`。**`v`/`k` 随 registry 增删漂移**（stable_id 按 runtime_id **字母序**，新增 action 会让后续整体平移）⇒ **APP 绝不可写死定值**，应先 `{"c":"registry","i":"<id>","k":0}` 拉取缓存
- **★★ `cmd_id` 去重缓存 10 项 / 30 s：重复 id 被 `cloud_check_duplicate_cmd()` 静默丢弃（连 ack/error 都不回）** ⇒ 这是"命令下发毫无响应"最常见的真因；APP/云端每次下发**必须换新 `id`**（建议 `前缀+毫秒时间戳`）

## LogManager 摘要（**细节与铁律 1–32 在 `MEMORY-logmanager.md`**）
- 冻结：Record 128B v2 / 16 段×31 条 / COW 零原地改 / ACK **at-least-once ⇒ 云端幂等** / 无独立 Task（`log_task()` 在 loop）
- **INFO 只上云不落 Flash；WARN+ 落 Flash** ⇒ 任何新 WARN 埋点都会扰动 496 条环的绝对计数。回归当前 **168/195**（26 MISS 全属 `R-7` 环境干扰）
- **★ 三层职责（用户 09-22 定案）**：**`EventId` = 事实分类 · `Level` = 本次场景的严重度 · `Param` = 具体上下文** ⇒ **同一 EventId 允许按影响范围用不同 Level**。`log_events.h` 的 Level 注释只是**典型值**，不是约束
- **刻意不埋**：LogManager 自身 5 个 ID ⇒ 走**批次头侧信道计数**（**是设计非缺口**）
- **仍无宿主**：`CFG_FACTORY_RESET`(0208)·`CLOUD_FRAG_FAIL`(0706)·`TIME_NTP_FAIL`(0802)·`OLED_INIT_FAILED`(0E01)；**需新增检测**：`VALVE_OVERFLOW_RISK`(0507)=`P0-4`
- **★ 跨段复用先例**：Workflow 域 meta 损坏**复用 Storage 段 `0x0303`**（BIN 持久化损坏 = 存储完整性失败，非业务错误）——后续跨域复用须在文档显式记理由
- **★ 新增 WARN+ 埋点前先判宿主频率**：仅启动期一次 ⇒ 无需门控；每 loop 可达 ⇒ **必须有边沿锁**

**铁律（完整 1–32 在 `MEMORY-logmanager.md`）**，跨模块通用的 4 条：
- **★ 精确计数前必须先清零**（`logt fwipe`+`mwipe`+`creset`+`reset`），否则"孤儿增量"易误判成门控失效
- **★ 断言不能隐含"执行期间只有本用例在写日志"** ⇒ "改判据"（＝删记账本体）就是降断言 ⇒ 正解是**测试隔离**
- **★ 门控只能加在"全部执行完成之后"**：纯事件型函数去重**绝不提前 return**；**"取值用于日志"的变量须在源头清零前取**
- **★★ 折叠/去重/门控类验证用"定向注入"**：`logt fill <lv> <n> <EventId(hex)>`；**记录级核对须 ACK 前置**（与"跑回归禁注入 ACK"相反，区别只在**是否在跑回归**）

## Weight / R-8（**细节在 `log模块历史/R7-R8-后续评审与系统性能权衡0920.md`**）
- **`R-8` = 真实缺陷**：滤波窗口无时间信息 + HX711 **覆盖式输出**（无 FIFO）⇒ 阻塞跨界 ⇒ 混合窗口 ⇒ 假跳变。**★ 危险区间非单调**：`<100ms` 无害 ｜ **`100–500ms` 最危险** ｜ `>500ms` 整窗换血反而不误报
- **后果升级**：假跳变经 `EVENT_WEIGHT_ERROR` → DispenseGuard → **`valve_force_close()`** ⇒ **`R-3` 与 `R-8` 同根因**（污染安全功能）
- **`R-8-A` 已修（`129606f`）**：`HX711::read()` 内 `wait_ready()` 无超时 ⇒ 掉线时 loop 永久挂起；修法＝`read()` 前二次 `is_ready()` 确认。**`R-8-B` 顺带解决**；**`R-8-C`** OPEN
- **`SYS-1`** LogManager **无需新增性能限制**；**`SYS-2`** BLE 与 LogManager **无并发**（共用 loopTask）
- **执行顺序铁律**：先修"会卡死/误动作的"，**再**修"让测试失准的"

## 其它模块要点（详见对应文档）
- **TimeManager V2**：`time_init()` 必须晚于 `oled_init()`；RTC(PCF8563T,0x51) **复用 OLED 的 Wire，绝不 `Wire.begin()`**；**SNTP 陷阱**：IDF v4.4 COMPLETED **瞬时**并自动回落 RESET ⇒ **禁用 `status != IN_PROGRESS` 判成功**
- **CloudManager**：**无应用层 TX 队列**、**离线直接返回 false 不排队**；`retry_interval` 是**死配置**（声明未读）；**协议级消息旁路 CommandManager**
- **烧录基线**：**`uploadfs` 整分区擦除**；`esptool write_flash 0x10000` **不碰** LittleFS；`data/` 被 gitignore ⇒ 用 `tools/gen_config_version.py`+`gen_workflow_bin.py` 重建（⚠️ `WF_STG_META_VERSION` 升级必须同步脚本）；**`bootstrap_version_file()` 必须在模块加载之后**

## 环境操作铁律（Windows + PlatformIO）
- **Bash PATH 可能损坏**（实测 `dirname`/`grep`/`ls` 全 `command not found`，但 `python.exe`/`git` 直调可用）⇒ 前加 `export PATH="/c/Users/wang/.workbuddy/binaries/PortableGit/versions/1.2.0/usr/bin:$PATH"`；仍不可用则改用 **PowerShell / Read / Grep / Glob 工具**（勿在坏 shell 里硬拼）
- 沙箱拦删 `.o`/`.elf` ⇒ **增量编译必然失败** ⇒ `PLATFORMIO_BUILD_DIR=.pio/build/xxx pio run`；⚠️ 全新 BD 首次偶发 `sconsign314.dblite` 缺失 ⇒ 先 `mkdir -p "$BD/esp32-s3-devkitc-1"`
- `pio run -t upload` **可能完全不烧录** ⇒ 直接 `esptool.py write_flash -z 0x10000 <firmware.bin>`（`D:/platformIO/packages/tool-esptoolpy/`）；Python 用 `C:/Users/wang/.workbuddy/binaries/python/envs/default/Scripts/python.exe`
- **串口回归**：`python test/serial_batch.py <COM> <log> <用例txt> [QUIET] [HIT_GRACE]`；用例 `<命令> ||| 期望子串`（断言串**不要加引号**）；QUIET：fill ≤62 用 **3.0**、`fill 100+` 用 **5.0**；**开串口即复位**
- **⚠️ `serial_batch.py` 串口静默 1.5s 即提前返回** ⇒ 长静默 / RAM 状态类实验**必须单会话探针**。用 `logt stats` 的 **`emit` 增量**度量"某动作产生几条日志"
- **★ 自写探针必须按行缓冲**：`read(in_waiting)` 会把**半行**当整行 ⇒ 解析 `logt` 输出全部失败（踩过）
- **★ 等待上限**：编译 `timeout 190` / 烧录 `120` / 串口回归 `185`；超时(124) **不重跑**，grep 结果文件或拆用例
- **同一文件多个 Edit 必须串行**（并行互相覆盖且仍报成功，改完必须复核）；**★ Bash heredoc 跑 Python 极易被引号搞崩且失败静默无改动** ⇒ **先 Write 成文件再执行**
- **静态验证三件套**：`nm -S` 看符号级 RAM 增量 · `objdump` 看站点 · `readelf`/`size` 看段尺寸
  - ⚠️ **EventId 载入方式随大小变化**：`0x9xx` 超 `movi` 12 位 ⇒ 经**字面量池**加载（需解析 `.flash.text`）；`0x3xx`/`0x4xx` 直接 `movi a10, 0xNNN`
  - ★★ **`call8 log_emit` 指令数 ≠ 源码调用数**：**载荷相同的日志块被编译器尾部合并**（多个入口各自 `movi a10,<EventId>` 后 `j <同一条 call>`）⇒ **必须按"逻辑入口"计数**，否则 3 个新增埋点会被误判成 2 个
  - ★ **RAM +0 的最硬证据** = 两份 ELF 的 `.data/.bss` 符号表 `diff` **逐项一致**（比只看 `pio` 总数强）
  - ★ **站点数对 ≠ 可区分**：两条独立路径若载荷值全同 ⇒ 云端无法区分（Phase 6-C 点 2/3 实测，根因是"不允许新增枚举值"）
- **用户偏好**：每阶段独立 commit（**代码 commit 与文档 commit 分离**）；**禁 `git add -A`/`.`**；⚠️ **提交记忆文件必须 `git add -f .workbuddy/memory/<file>`**（`.workbuddy/` 被 gitignore，**即使已 tracked 也报 `paths are ignored`**）；测试资产放受跟踪的 `test/`；编译临时文件可直接删，其他目录删除需用户同意；文档（除 gitignore 内的）都可 commit
