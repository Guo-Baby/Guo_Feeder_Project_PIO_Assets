# P2-0 · Architecture Review + Design Freeze（Homie Bridge）

> **状态**：`FROZEN（本文件即 P2-1…P2-8 的唯一上位设计依据）`
> **日期**：2026-10-08 · **阶段性质**：**只读审查 + 设计冻结**（零生产代码 / 零配置 / 零协议 / 零 ACL / 零 schema 改动）
> **修订**：**P2-0.1 Freeze Corrections（2026-10-08）** —— 修复 BLOCKER-1（QoS 规范语义 vs transport profile）· BLOCKER-2（`$description` / Registry rebuild 一致性）· BLOCKER-3（P2-7 三级成立判据）+ 6 项冻结修正 + 全文一致性 pass。**本修订不改变任何既定架构决策，仅消除契约/边界/验收/内部一致性问题。**
> **修订**：**P2-0.2 Flash Budget Amendment（2026-10-09）** —— **仅修改 §17.2 的 Flash 增量预算条款**：`.text` / `.rodata` 不再单列为硬性 acceptance ceiling，**Flash 合计增量上限由 `+4…8.5 KiB`（8,704 B）修订为 `≤ 14 KiB`（14,336 B）**；其余资源约束（DRAM / PSRAM / 栈 / CPU / MQTT / 网络）**全部保持不变**。**本修订不改变任何架构决策、行为契约、ABI、安全语义与 topic 契约，只调整资源预算数字。** 依据：`docs/architecture/P2-Flash-Budget-Reconciliation.md`（审计 `b1ca1de`）+ `docs/architecture/P2-6-Resource-Acceptance-Closure.md`。
> **基线**：Firmware `623da59`(wb) · EMQX `76bd1c9`(main) · Cloudflare `d604f2e`(main) · Worker `846384cd-8b84-488b-a7a1-3ee5f9fae52a`
> **上位文档**：`Cloud-APP-Platform-Plan.md` §2/§3/§6/§10/§11（P0–P6 路线，§2.3 投影规则与 §2.4 双协议并存）
> **权威外部规范**：**Homie Convention v5**（`homieiot.github.io/specification`）
> **方法论**：**一切结论以实际代码为准**；每条决策附「现有代码在哪 / 当前行为 / 可复用性 / 需新增 / 风险」。

---

## 1. Scope

### 1.1 本阶段做什么
1. 审阅固件（Registry / CommandManager / CloudManager / SystemState / EventManager / Workflow / Config / Log / Time）、云端 Worker、EMQX ACL 的**实际实现**。
2. 回答 §五…§十四 的 16 个架构问题，逐条给出 `FROZEN / DEFERRED / REJECTED / OPEN`。
3. 产出 **P2-1…P2-8 可直接开工、不再重新讨论核心架构**的上位设计。

### 1.2 本阶段**不**做什么
- 不写任何 `src/` 代码、不改 `platformio.ini`、不改 Worker、不改 ACL、不改 D1 schema、不加测试、不加依赖、不 commit、不 push。
- 不实现 P2-7（只定义验收）。

### 1.3 冻结判据（进入 P2-1 的条件）
> 全文**无 BLOCKER 级 OPEN**；§23 Freeze Checklist 的 **26 项**（23 FROZEN + 3 DEFERRED）全部 `FROZEN` 或显式 `DEFERRED`。

---

## 2. Current Architecture Baseline

### 2.1 固件实际分层（`src/`，7 个子目录，`main.cpp` 留根）

| 目录 | 关键模块 | 与 P2 的关系 |
|---|---|---|
| `app/` | `valve` · `weight` · `dispense_guard` · `computer_reset` · `MiThermometer` · `oled` | **Device Model 的读取面**（System State 产生方） |
| `automation/` | `workflow` · `workflow_storage` · `capability_registry` | Action/Trigger **Descriptor 元数据**所在 |
| `services/` | `system_state` · `config_manager` · `event_manager` · `command_manager` · `device_identity` · `topic_renderer` · `time_manager` · `wifi_module` · `system_command` · `credential_manager` · `cred_store` | **Device Model / Projection 的宿主层** |
| `cloud/` | `cloud_manager` | **唯一 MQTT 传输所有者**（见 §7.1） |
| `log/` | `log_manager` · `log_events` · `log_cbor` · `log_ack` | 错误可观测性（Homie 失败路径只记录，不发业务 up） |
| `storage/` | `json_storage` · `bin_storage` · `file_storage` | **Homie 不使用** |

**启动顺序（`main.cpp:439+`，与 P2 相关）**
```
log_init → json_storage_init → bin_storage_init → workflow_storage_init
→ system_state_init → config_init → device_identity_init → event_manager_init
→ ble_init → wifi_init → workflow_init → weight_init → valve_init
→ computer_reset_init → dispense_guard_init → oled_init → oled_event_init
→ time_init → test_mqtt_init → credential_manager_init → command_manager_init
→ cloud_init → … → capability_registry_init()          ← L587（**最后**）
→ config_boot_validate()
```
**loop（`main.cpp:617+`）**：`system_command_task → wifi_task → event_dispatch → time_task → cloud_task → credential_manager_task → command_manager_task → config_task → workflow_task → oled_task → weight_task → valve_task → computer_reset_task → MiThermometer_task → test_mqtt_task → log_task → serial_debug_command_process`
⇒ **Homie 任务必须插入 loop**，且**严禁阻塞**（现有铁律：禁 `delay()` / while 死等）。

### 2.2 关键事实（代码级，P2 设计的输入）

| # | 事实 | 证据 |
|---|---|---|
| F-1 | **Capability Registry 只是"短 ID 映射表"**，条目 = `{stable_id(uint8), runtime_id(String), object_version(uint32)}` | `capability_registry.h:121-149` |
| F-2 | `stable_id` = **runtime_id 字符串升序排序后的 index** ⇒ 新增 Action 会**平移**已有 ID | `capability_registry.h:66-68` |
| F-3 | Registry 容量：Action/Trigger 各 32、Workflow 16 | `capability_registry.h:82-85` |
| F-4 | Registry 有 `version`（uint32 递增）+ `checksum`（CRC32），可作"能力集是否变化"的**廉价指纹** | `capability_registry.h:132-149` |
| F-5 | **Descriptor 已含丰富元数据**：`WorkflowActionDescriptor{id,name,module,description,params,param_count}`、`WorkflowParam{name,type,unit,description}` | `workflow.h:157-264` |
| F-6 | `WorkflowParamType` 只有 **4 种**：`PARAM_INT/FLOAT/BOOL/STRING`（**无 enum / 无 min-max / 无 step**） | `workflow.h:91-97` |
| F-7 | **System State 是"无元数据的键值表"**：`SystemStateValue{type,value,value_float,value_string,valid}`；只有 `state_get_field_name()` 给名字 | `system_state.h:78-85,140-141` |
| F-8 | System State 现状 **21 项**（WiFi×4 · Time×1 · Valve×1 · Weight×2 · Event×1 · MQTT×4 · MiThermo×8） | `system_state.h:20-62` |
| F-9 | **CommandManager 有两条去重防线**：`command_runtime_insert()` 的 **cmd_id 重复拒绝**（运行中）+ 槽位容量 `MAX_COMMAND_RUNTIME` | `command_manager.cpp:348-390` |
| F-10 | **MQTT 层去重缓存（10 项 / 30 s）在 `cloud_manager` 内**，仅服务业务下行路径 | `cloud_manager.cpp:2341-2366` |
| F-11 | **全系统只有一个 `esp_mqtt_client`**，且**只订阅 1 个 topic**（`mqtt_sub_topic`） | `cloud_manager.cpp:394-397, 124` |
| F-12 | **MQTT_EVENT_DATA 完全不看 topic** — 无条件做分片重组后入队 | `cloud_manager.cpp:450-488` |
| F-13 | **当前无 LWT**（`cfg.lwt_*` 未设置） | `cloud_manager.cpp` 无 `lwt_` 引用 |
| F-14 | `disable_clean_session = 0` ⇒ **clean session = true** | `cloud_manager.cpp:1522` |
| F-15 | **发布层 retain 恒为 0**（`esp_mqtt_client_publish(...,qos,0)` / `enqueue(...,qos,0,store)`） | `cloud_manager.cpp:550-567, 600-607` |
| F-16 | `CLOUD_MQTT_QOS=1`、`CLOUD_MQTT_STORE=true`、`CLOUD_MQTT_BUFFER_SIZE=8192` | `cloud_manager.cpp:50-62` |
| F-17 | 下行入口 `cloud_process_rx_message()` 顺序：解析 → `compact=!doc["c"].isNull()` → **`change_msg_limit` / `log_ack` 旁路** → `cloud_translate_command()` → `cloud_send_ack()` → `command_manager_execute()` | `cloud_manager.cpp:1237-1418` |
| F-18 | `cmd.source = "cloud"` 硬编码；`credential_set` 用 **source allow-list（仅 `cloud`）** 做安全门控 | `cloud_manager.cpp:1124`；`command_manager.cpp:2296-2309` |
| F-19 | `execute_action` 的 `payload` 由 **WorkflowManager** 转 Action 参数，CommandManager 不解析 | `command_manager.cpp:829-839` |
| F-20 | `esp_mqtt_event_t` **含 `topic` / `topic_len` / `retain` / `session_present`**；`esp_mqtt_client_publish/enqueue` **均支持 retain**；`cfg.lwt_*` **齐备** | `mqtt_client.h:153-161,186-192,347,368` |
| F-21 | 已注册 **Action 6 个** + **Trigger 1 个**（见 §12.1 清单）；`weight.enable=false` 时 weight 的 Action/Trigger **不注册** | `valve.cpp:83/95` · `weight.cpp:123/142` · `computer_reset.cpp:235` · `MiThermometer.cpp:737/752` |
| F-22 | `capability_registry_rescan()` 的**唯一业务触发点** = command_manager 的 workflow CRUD（4 处） | `command_manager.cpp:1884,1993,2081,2137` |
| F-23 | `LOG_REG_REBUILT = 0x0B01` 是 **LogManager 事件**，**不是** EventManager 事件（无 `event_subscribe` 宿主） | `log_events.h:338`；`capability_registry.cpp:717` |
| F-24 | OLED **只播动画帧**，**未展示任何 System State**（`oled_show_frame()` 读 `anim_table`） | `oled.cpp:154-188` |
| F-25 | PSRAM 分配范式已确立：`heap_caps_malloc(size, MALLOC_CAP_SPIRAM)` + 失败回退 `MALLOC_CAP_8BIT` | `capability_registry.cpp:510-513` · `workflow.cpp:783` |
| F-26 | ELF 实测：**text 1,120,774 / data 284,452 / bss 1,222,621**（bss 含 PSRAM 段） | `xtensa-esp32s3-elf-size` on `firmware.elf` |
| F-27 | Worker 的 `deviceIdFromTopic()` **只认 `guo_feeder/<id>/{up,log}`（严格 3 段）** | `device-registry.js:168-176` |
| F-28 | 设备 ACL 由云端**逐设备写入** 3 条（down/up/log），**PUT 覆盖式**（整组替换） | `credential.js:101-103` · `emqx-admin.js:264` |
| F-29 | EMQX 现有兜底 `全部用户 → # → deny`（**白名单语义**）⇒ **任何新 topic 都必须显式 allow** | `acl-rules.v3.json:3-8` |
| F-30 | `acl-rules.v3.json` 明记：**`homie/5/<device_id>/#` 属 P2，本阶段不添加** | `acl-rules.v3.json:32` |
| F-31 | 规划 §2.4 **已冻结**：`homie/5/<device_id>/...` 与 `guo_feeder/<device_id>/...` **永久并存** | `Cloud-APP-Platform-Plan.md:249-255` |
| F-32 | 规划 §6.2 **已冻结**：实时控制/状态 = `客户端 ↔ EMQX ↔ 设备`，**不经 Worker** | `Cloud-APP-Platform-Plan.md:520-528` |

---

## 3. Architecture Decision（Executive Decision）

### 3.1 一句话结论

> **建立独立、协议无关的「Device Model」；在其之上叠加「Homie Projection」；两者均与 CloudManager 并列；MQTT 传输仍由 CloudManager 独有，对 Homie 只开一个窄口（Transport Facade）；Homie 写入一律经 CommandManager。**

### 3.2 目标数据流（FROZEN）

```
                    ┌──────────────────────────────────────────┐
                    │  WorkflowActionDescriptor / Descriptor   │  ← 已有（F-5）
                    │  SystemStateKey + SystemStateValue       │  ← 已有（F-7）
                    └───────────────────┬──────────────────────┘
                                        │  人工编著（每模块一张 const 表）
                                        ▼
                    ┌──────────────────────────────────────────┐
                    │        Device Model（设备语义模型）        │  ★ 新增（FROZEN）
                    │  node / property / datatype / unit /     │
                    │  format / settable / retained / source    │
                    │  （**协议无关**：不认识 Homie / MQTT）      │
                    └───────┬───────────────────────┬──────────┘
                            │                       │
              ┌─────────────▼────────┐   ┌──────────▼─────────────┐
              │ Homie Projection     │   │ OLED UI（W-1，P2 并行） │
              │ （MQTT 序列化 + 主题映射）│   │ 只读 System State + 模型 │
              └────────┬─────────────┘   └────────────────────────┘
                       │
        ┌──────────────┴───────────────┐
        │                              │
   ┌────▼─────┐                 ┌──────▼──────────────┐
   │ 写（set） │                 │ 读（state/描述）      │
   └────┬─────┘                 └──────┬──────────────┘
        │                              │
        ▼                              ▼
┌────────────────────┐        ┌───────────────────────┐
│ Transport Facade   │        │ Transport Facade      │
│ （CloudManager 提供）│        │ （CloudManager 提供）   │
└────────┬───────────┘        └───────────┬───────────┘
         ▼                                ▼
┌────────────────────┐        ┌───────────────────────┐
│ CommandManager     │        │ 唯一 esp_mqtt_client   │
│ （校验/去重/生命周期） │        │ （CloudManager 独有）   │
└────────┬───────────┘        └───────────────────────┘
         ▼
  现有业务层（Workflow / Action）→ 硬件
         ▼
  System State / Event
         ▼
  Homie property state（回到"读"侧）
```

**关键不变量（4 条）**
1. **Homie 永不直连硬件 / Action**（只经 CommandManager）。
2. **只有 CloudManager 持有 MQTT 客户端**（Homie 不建第二个连接）。
3. **Device Model 不依赖 MQTT / Homie / CommandManager**。
4. **Homie topics 与业务 topics 物理隔离**（互不影响，§2.4）。

> **注（与 §4.3 对齐）**：上图 "Device Model → Homie Projection" 表示**语义来源**；Homie Projection **同时**读取 **Capability Registry** 的 **runtime capability availability** 做过滤（§4.3 ②）⇒ **Device Model 与 Capability Registry 均为 Projection 输入**（AD-1c/1d），但二者**互不依赖**。

### 3.3 决策总表

| ID | 决策 | 状态 |
|---|---|---|
| **AD-1** | 建立独立 **Device Model**（非 1:1 Capability Registry） | **FROZEN** |
| **AD-2** | Device Model = **编译期 `const` 静态描述表**（Flash），**零新增 DRAM 静态数组** | **FROZEN** |
| **AD-3** | Device Model **协议无关**，不 include `mqtt_client.h` / `homie_bridge.h` / `command_manager.h` | **FROZEN** |
| **AD-4** | **Homie Projection 是独立模块**（新增 `src/homie/`），**不放进 CloudManager** | **FROZEN** |
| **AD-5** | MQTT **单客户端不变**；CloudManager 增开 **Transport Facade**（publish/subscribe/topic-dispatch/LWT） | **FROZEN** |
| **AD-6** | Homie `set` → 合成 `CommandMessage` → **`command_manager_execute()`** | **FROZEN** |
| **AD-7** | Homie 结果**不新开 CommandManager 回调**，用**只读轮询** `command_manager_get_runtime()` | **FROZEN** |
| **AD-8** | Base topic = **`homie/5/<device_id>/`**（`device_id` 同 DeviceIdentity） | **FROZEN** |
| **AD-9** | **Workflow 不进 `$description`**（用户数据 ≠ 设备能力） | **FROZEN** |
| **AD-10** | `$description` **仅在 boot 的 `init` 阶段生成/发布**；运行期不重发 | **FROZEN** |
| **AD-11** | EMQX ACL：**禁 `homie/5/+/#`**；只用设备自身子树 + 受限通配（`+/+/set`） | **FROZEN** |
| **AD-12** | OLED / Homie **共用 Device Model**；远端 UI **只经 Homie 投影**消费 | **FROZEN** |

### 3.4 ★ 术语基线：Homie 语义要求 vs Guo Feeder Transport Profile（FROZEN — BLOCKER-1）

> **本设计严格区分两个概念，全文不得混用。** 任何"规范要求 X"的表述，仅当 X 确为 Homie 5 强制项时才允许使用。

| 类别 | 含义 | 本设计实例 |
|---|---|---|
| **Homie semantic requirement**（**规范强制**，违反即非合规设备） | Homie 5 对 topic 存在性 / retained / lifecycle / `$description` 结构 / LWT 语义的**强制要求** | ① `$state` **必须** retained（§10.3）② `$description` **必须** retained（§11.2）③ `$description` **只能**在 `$state ∈ {init, disconnected, lost}` 时变更（§11.2，AD-10 根因）④ LWT **必须**指向 `$state`、payload = `"lost"`、且 **LWT 必须 retained**（§10.3）⑤ property 的 retained 由 `$description` 的 `retained` 字段声明（§14） |
| **Guo Feeder transport profile**（**工程选择，非规范要求**） | 本项目对 QoS / store / clean-session / 缓冲的**工程取值**；改成 QoS0/QoS2 设备**依然合规** | ① **QoS = 1**：`$state` / `$description` / 只读属性 / `/set` 全部 QoS1 —— 依据仅为本工程"与全系统 QoS1 统一"（F-16），**不是** Homie 推荐值 ② `store = true`（F-16）③ clean session = `true`（F-14，**保持 P1 现状不改**）④ MQTT 单客户端（F-11） |

> **FROZEN（AD-3a）**：**Homie 5 规范未规定 QoS。** 全文所有 "QoS1" 字样一律理解为 **Guo Feeder transport profile**；**禁止**表述为"Homie 规范要求 QoS1"。本次修订**不因此把 QoS 改成 2** —— QoS1 的工程决策**原样保留**。
> **FROZEN（AD-3b）**：`retained`（retain 标志）与 `QoS` 是两个**正交维度** —— 前者的取值**多数由规范**决定，后者**全部由本工程**决定。文档中二者**不得合并表述**（如"规范要求 retained + QoS1"）。
> **FROZEN（AD-3c）**：ACL / topic 结构 / `$state` 语义 / `$description` 结构均属"规范 + 本设计"，与 QoS 列**无因果关系**。

---

## 4. Capability Registry vs Device Model

### 4.1 结论
> **Capability Registry ≠ Device Model，且不足以承担设备模型。** ⇒ 必须建立 Device Model（AD-1）。

### 4.2 逐项论证（"Registry 是能力注册表，不是设备模型"）

| 维度 | Capability Registry 现状 | Device Model 需要 | 差距 |
|---|---|---|---|
| 条目内容 | `{stable_id, runtime_id, object_version}` | + datatype / unit / format / settable / retained / readable source | **缺 6 项** |
| 标识稳定性 | `stable_id` = **字母序 index**（F-2）⇒ 新增 Action **平移** | 语义 **字符串 id**，跨版本恒定 | **语义冲突** |
| 可读面 | **完全没有**（只映射 Action/Trigger/Workflow） | System State 的**白名单投影 + 元数据** | **缺失** |
| 分组 | 无 | node（`water` / `climate` / `system`） | **缺失** |
| 取值域 | 无 | min/max/step/enum 成员 | **缺失** |
| 协议归属 | **服务于 Cloud Protocol 短 ID**（业务协议私有） | 必须**协议无关** | **定位不同** |
| 持久化 | LittleFS `/registry/*.bin` | 只读编译期常量 | **不需要** |
| 生命周期 | 随 workflow CRUD `rescan()`（F-22） | 随固件 / 启动配置变化 | **不同** |

**⇒ 三条硬理由**
1. **`stable_id` 会平移 ⇒ 不能作为 UI 身份**（UI 会"记得"错误的控件）。
2. Registry **没有可读面**，而 Homie 的一半是"只读状态属性"。
3. Registry 的**存在目的**是业务协议的短 ID 压缩，把它当设备模型会把"业务协议"和"设备语义"焊死，未来 OLED/Web 会各自再解释一遍。

### 4.3 关系（FROZEN · **模块依赖 ≠ Projection 输入**）

> **AD-1c（关键澄清）**：Device Model 与 Capability Registry **是兄弟模块**（**互不 include · 无父子 · 无 1:1 映射**）；二者**仅在 Homie Projection 内部被同时读取**。文档不得把"二者并列"读成"二者都可作为 Projection 的等价输入"。

**① 模块依赖（编译期 —— 单向、零耦合）**
```
Capability Registry  ──(业务协议短 ID 压缩)──►  Cloud Protocol（不变，P2 零改动）
Device Model         ──(设备语义)────────►  Homie Projection / OLED / 未来 UI
        ▲
        └── 两者共用同一批 runtime_id（Action id），但**互不依赖、互不 include**
```

**② Projection 输入（运行期 —— Homie Projection **同时**读两者）**
```
System State
     │
     ▼
Device Model
     │
     │ semantic definition
     ▼
Homie Projection
     ▲
     │ capability availability / runtime filtering
     │
Capability Registry
```

| 输入 | 提供什么 | 不提供 |
|---|---|---|
| **Device Model**（静态、协议无关） | `node` · `property` · `datatype` · `unit` · `settable` · **semantic identity** · **state source**（读侧绑定） · **command mapping**（写侧 Action 映射） | 运行期能力是否存在；不感知 Registry |
| **Capability Registry**（运行期） | **当前 runtime capability 是否存在**（Action / Trigger / Workflow registry 状态） · **runtime capability filtering**（决定哪些静态 property 在本次 boot 被投影） | 属性语义（datatype / unit / settable 等）；不感知 Device Model |

> **★ `weight.enable=false` 时为什么 `weight` 相关 property 不进入 `$description`？**
> 判据 = **Device Model 静态定义（property 存在）+ boot-time Capability Registry availability filter（该 Action/能力本次未注册 ⇒ 过滤掉）**。
> ⇒ **不修改 Device Model 本身**（静态表照旧含 `weight` 条目）；**只是本次投影时被 Registry 可用性过滤**。故同一固件在 `weight.enable=true` 时 boot 会投影出这些 property，`false` 时不会（§12.2 生成规则 / §15.3 AD-15b）。

> **FROZEN（AD-1a）**：Homie **禁止**使用 `stable_id`；Homie 属性 id 一律用语义字符串。
> **FROZEN（AD-1b）**：Registry **本阶段零改动**（P2 不扩字段、不改 `stable_id` 规则）。
> **FROZEN（AD-1d）**：**Device Model 不 include Capability Registry；Capability Registry 不 include Device Model**；二者**不是父子、不是 1:1**；Homie Projection 是**唯一**同时读取二者的地方。

---

## 5. Device Model Definition

### 5.1 定位
**Device Model = 设备对外语义的唯一权威、协议无关描述**。它描述"这台设备有哪些可读状态、哪些可写能力、各自的数据类型与取值域"。

### 5.2 职责（FROZEN）

**Device Model 必须负责**
| 职责 | 说明 |
|---|---|
| device identity binding | 模型绑定**已存在的** `device_id()`（不自造身份） |
| node | 逻辑子系统分组（`water` / `climate` / `system` / `control`） |
| property | 语义属性 id（`[a-z0-9-]`，**稳定**，不随注册顺序变化） |
| datatype | `boolean` / `integer` / `float` / `string` / `enum` / `json` |
| format | enum 成员表 / 数值范围 `min:max[:step]` |
| unit | 单位（可为空） |
| settable / readable | 可写性 / 可读性 |
| state source | 只读面的绑定：`SystemStateKey`（或"无来源"） |
| capability mapping | 可写面 → Action `runtime_id`（含 enum→多 Action 映射） |
| value validation | **协议层**取值校验（类型 / 枚举成员 / 范围） |
| metadata | `name`（展示名）；`description` 并入 name（Homie 5 无独立 description 字段） |
| protocol-independent semantic identity | 属性 id 与 Homie topic / MQTT 无关 |

**Device Model 明确不负责（越界即违规）**
`MQTT transport` · `MQTT reconnect` · `hardware control` · `valve GPIO` · `motor control` · `business workflow` · `database persistence` · `cloud authentication` · `保留任何运行期可变状态`。

> **FROZEN（AD-2a）**：Device Model **无状态**：只有 `const` 表 + **纯函数**访问器；不产生 System State、不注册 Event、不持有句柄。

### 5.3 结构契约（FROZEN — P2-1 按此实现）

```c
// src/homie/device_model.h   （协议无关；只 include <stdint.h> 与 services/system_state.h 的 key 枚举）

enum DeviceModelDatatype : uint8_t { DM_BOOL, DM_INT, DM_FLOAT, DM_STRING, DM_ENUM, DM_JSON };

enum DeviceModelSourceKind : uint8_t { DM_SRC_NONE = 0, DM_SRC_STATE };

// 可写面：一个属性值 → 一个 Action 调用
struct DeviceModelCommand
{
    const char *value;        // 该属性取值（enum 成员；或 "true" / "1" 等）
    const char *action_id;    // 已注册 Action 的 runtime_id（如 "VALVE_OPEN"）
    const char *payload_json; // 传给 Action 的 payload（无参 = "{}"）
};

// 只读枚举映射：把 SystemState 的布尔 / 整数值映射为 Homie enum 成员串
// （**显式**；禁止从 runtime_id 字符串推导枚举，见 §12.5）
struct DeviceModelEnumMap { int32_t state_value; const char *member; };

struct DeviceModelProperty
{
    const char *node;        // node id
    const char *id;          // property id（[a-z0-9-]）
    const char *name;        // 展示名（如 "电磁阀"）
    DeviceModelDatatype dtype;
    const char *format;      // enum:"open,close"  num:"0:300:1"  其余 ""
    const char *unit;        // 可为 ""
    bool settable;
    bool retained;           // 只读状态属性默认 true
    float deadband;          // 数值死区（0 = 精确比较）
    uint16_t min_pub_ms;     // 最小发布间隔（0 = 用全局默认）
    // ---- 只读面 ----
    DeviceModelSourceKind src_kind;
    uint16_t state_key;      // SystemStateKey（DM_SRC_STATE 时有效）
    const DeviceModelEnumMap *enum_state_map; // 状态值→枚举成员（仅 DM_ENUM 读侧用；否则 nullptr）
    uint8_t enum_state_map_count;
    // ---- 可写面 ----
    const DeviceModelCommand *cmds;
    uint8_t cmd_count;
};

struct DeviceModelNode { const char *id; const char *name; const char *type; };
```

访问器（**纯函数**）：
```
uint8_t                     dm_node_count();
const DeviceModelNode*      dm_node_at(uint8_t i);
uint8_t                     dm_property_count();
const DeviceModelProperty*  dm_property_at(uint8_t i);
const DeviceModelProperty*  dm_find(const char* node, const char* prop);
bool                        dm_resolve_command(const DeviceModelProperty*, const char* value, /*out*/ const DeviceModelCommand**);
```

**存储位置与开销（FROZEN）**
- 全部表为 `static const` ⇒ 落 `.rodata`（**Flash**，可内存映射读取）⇒ **DRAM 静态占用 = 0**。
- 单个 property 条目 ≈ 44–52 B + 字符串常量；20 个属性 ≈ **1.2–2.0 KB Flash**。

### 5.4 属性命名与稳定性（FROZEN）
- property id：`[a-z0-9-]`，**不得**以 `$` 开头（Homie 保留），**不得**含 `/`。
- **一旦发布即视为 ABI**：若必须改名 ⇒ **新增**属性并保留旧的（deprecate），**不得原地改名**（否则所有 UI 控件失联）。

---

## 6. Homie Projection Boundary

### 6.1 模块边界（FROZEN — AD-4）

**新增目录 `src/homie/`**（**与 `cloud/` 并列**，第 8 个物理子目录）：
```
src/homie/
├── device_model.h / .cpp      ← Device Model（§5）
└── homie_bridge.h / .cpp      ← Homie Projection（本节）
```

**为什么新开 `src/homie/` 而不是塞进 `src/cloud/`**
1. 规划 §2.2 明确「**不是 CloudManager 的内部模块**」；
2. 新目录让"**不得 include `cloud/cloud_manager.h` 的业务 API**"成为可静态检查的边界（只允许 include **Transport Facade 头**）；
3. 目录名 ASCII、不与现有头文件重名（项目铁律）。

> **Device Model 也放在 `src/homie/`** 而非 `services/`：其**存在理由就是投影**；OLED 消费它时 include `homie/device_model.h`，语义为"读取设备模型"（模型本身协议无关，目录名仅作归类）。

### 6.2 Projection 负责 / 不负责（FROZEN）

| ✅ 负责 | ❌ 不负责（越界即 BLOCKER） |
|---|---|
| node/property → topic 映射 | 直接操作 Valve / Weight / Motor |
| `$state` / `$description` 生成与发布 | 直接写 System State（`state_set_*`） |
| property state 的**变化检测 + 发布** | 直接 `event_push()` 造假事件 |
| `<prop>/set` 的解析 → `CommandMessage` | 直接 `workflow_enqueue_action()`（须经 CommandManager） |
| Homie 元数据（name/unit/format/…） | 直接调硬件 / `dispense_guard` |
| retained 语义 / 序列化 | 读写 Config / LittleFS |
| 协议层取值校验 | 订阅业务下行 topic（`guo_feeder/.../down`） |

### 6.3 允许的依赖（白名单，FROZEN）
```
homie_bridge.cpp 可 include：
  services/system_state.h        （只读 state_get_*）
  services/command_manager.h     （execute + get_runtime）
  services/event_manager.h       （只读订阅，可选）
  log/log_manager.h + log_events.h（错误可观测）
  homie/device_model.h · homie/homie_bridge.h
  cloud/mqtt_transport.h         ← ★ 新增的 Transport Facade（§7.1）
禁止 include：cloud/cloud_manager.h（业务 API）· 任何 app/*.h · automation/*.h（除只读查询）
```

### 6.4 ★ Transport Facade 单向依赖（FROZEN — P2-2 必须遵守）

> **依赖方向单向冻结**，**不得**形成 `cloud → homie → cloud` 循环：

```
homie/homie_bridge
        │
        ▼
cloud/mqtt_transport        ← Transport Facade（P2-2）
        │
        ▼
cloud/cloud_manager         ← 唯一 esp_mqtt_client 持有者
        │
        ▼
esp_mqtt_client（ESP-IDF）
```

**FROZEN 铁律**
1. **Homie 可以依赖** `cloud/mqtt_transport.h`（**仅此一个** cloud 头）。
2. **`mqtt_transport.cpp` 可以调用** CloudManager 内部实现（Facade 即 CloudManager 的窄口）。
3. **CloudManager 不依赖 Homie**：`cloud_manager.cpp` **不 include** `homie/homie_bridge.h`。
4. **不得**形成 `cloud → homie → cloud` 循环依赖。
5. **Homie 不获得** `esp_mqtt_client*` 句柄。
6. **Homie 不直接调用** ESP-IDF MQTT API（`esp_mqtt_client_publish` / `subscribe` / …）。
7. topic 分派（`event->topic` 判定）**只发生在 `mqtt_transport.cpp` / CloudManager 侧**；Homie 只接收"非业务 topic"的 `(topic, data, len, retain)` 回调（§7.1 AD-5a）。

> **P2-2 遵守边界**：本节的单向依赖是 P2-2（Transport Facade）的**实现前提**；违反即视为 P2 架构 BLOCKER。

---

## 7. CommandManager Integration

### 7.1 MQTT 传输：单客户端不变 + Transport Facade（FROZEN — AD-5）

**问题（代码实证）**：F-11（只有一个 client）+ F-12（RX 不看 topic）+ F-13（无 LWT）+ F-15（无 retain）⇒ Homie **无法**在旁路自立门户。

| 方案 | 判定 |
|---|---|
| **A. Homie 自建第二个 `esp_mqtt_client`** | **REJECTED** — 需要第二个 client_id（撞 `dev_<id>` 唯一性 / 触发 session takeover）、翻倍 RAM、破坏"一机一连接"安全语义 |
| **B. Homie 直接 `extern` 拿 `mqtt_client`** | **REJECTED** — 泄露 esp-mqtt 类型进投影层，破坏边界 |
| **C. CloudManager 暴露窄口 Transport Facade** | **FROZEN** |
| **D. 抽出独立 `mqtt_transport` 模块，CloudManager 与 Homie 都改成它的客户** | **DEFERRED（P3+）** — 架构更纯，但属对 P0/P1 冻结模块的**重构**，超出 P2 风险预算（违 §二十一"不得为推进 P2 强行改 P1 冻结架构"的精神）；**P2 先用 C，并保证 C→D 可无痛迁移** |

**Transport Facade 契约（FROZEN — P2-2 实现，`src/cloud/mqtt_transport.h`）**
```
// 发布（retain 可显式指定；QoS 由调用方指定）
bool mqtt_transport_publish(const char* topic, const char* payload, size_t len, int qos, bool retain, bool store);
// 订阅（幂等；连接建立后由 CloudManager 统一重放注册表）
bool mqtt_transport_subscribe(const char* topic_filter, int qos);
// 注册"非业务 topic"的接收回调（在 loop 上下文调用，**不在 esp-mqtt 任务内**）
void mqtt_transport_set_foreign_handler(void (*cb)(const char* topic, const uint8_t* data, size_t len, bool retain));
// 连接状态（供 $state 判定）
bool mqtt_transport_is_connected();
```

**对 CloudManager 的**加性**改动清单（FROZEN）**
| # | 改动 | 位置 | 风险 |
|---|---|---|---|
| T-1 | `lwt_topic/lwt_msg/lwt_qos/lwt_retain` 配置 | `cloud_init()` 的 `cfg` | 低（新增字段；仅 Homie 场景生效） |
| T-2 | CONNECTED 时按**注册表**重放订阅（业务 topic + Homie filter） | `mqtt_event_handler:385-406` | 低（业务订阅行为不变） |
| T-3 | `MQTT_EVENT_DATA` 取 `event->topic`，**非业务 topic ⇒ 交 foreign handler** | `mqtt_event_handler:450-488` | **中** — 必须**逐字保持**业务路径不变（byte-for-byte） |
| T-4 | `cloud_mqtt_publish_binary/text` 增 `retain` 参数（默认 0 ⇒ 业务**零行为变化**） | `:526-618` | 低 |
| T-5 | 新增 `src/cloud/mqtt_transport.{h,cpp}`（**纯转发**，无业务逻辑） | 新文件 | 低 |

> **STOP 条件**：若 T-3 无法在**不改变业务下行语义**（`change_msg_limit` / `log_ack` 旁路、去重、ACK 顺序）的前提下完成 ⇒ **STOP**（不得改动 P1 已冻结的入站契约）。
> **FROZEN（AD-5a）**：Facade 的 foreign 回调**必须**在 **loop 上下文**（经 `cloud_rx_enqueue` 同款队列）交付，**禁止**在 esp-mqtt 任务里直接执行 Homie 逻辑（现有分片重组缓冲是全局单例，重入会串包）。

### 7.2 Homie `set` → CommandManager（FROZEN — AD-6）

```
homie/5/<id>/<node>/<prop>/set   payload = <raw value>
        │
        ├─ ① 丢弃 retain=1 的投递（防"订阅即执行"陈旧指令）        ← F-20
        ├─ ② Device Model 解析 <node>/<prop>（未知 ⇒ 丢弃 + WARN 日志）
        ├─ ③ 协议层校验：dtype 解析 + enum 成员 + 数值范围（§13）
        ├─ ④ dm_resolve_command() → { action_id, payload_json }
        ├─ ⑤ 合成 CommandMessage：
        │      command  = "execute_action"
        │      object   = action_id            （如 "VALVE_OPEN"）
        │      payload  = payload_json          （无参 = "{}"）
        │      cmd_id   = 自生成（§7.3）
        │      source   = "homie"               ★ 与 "cloud" 区分
        │      timestamp= time_get()
        └─ ⑥ command_manager_execute(cmd) → 成功则进入 pending 表（§7.4）
```

**为什么复用 `execute_action` 而不是新造 Homie 命令（FROZEN）**
- 复用即继承：**cmd_id 去重（F-9）** + **Action 参数解析（F-19）** + **超时清理** + **Workflow 回调**；
- 新造命令会让两条控制面分叉 ⇒ 双份校验、双份超时、双份日志（违"不重复埋点"铁律）；
- `source="homie"` 天然与 `credential_set` 的 `source=="cloud"` allow-list 隔离 ⇒ **Homie 永远无法触发凭据类操作**（安全收益，非巧合）。

### 7.3 cmd_id 生成（FROZEN）
```
cmd_id = "h" + <boot_seq:4hex> + "-" + <counter:6hex>      // 例 "h3f2a-00001b"
- boot_seq  : 本次启动的随机/递增标记（复用现有 boot 序号机制或 millis 低位）
- counter   : 单调递增（每次 set +1，回绕到 0 时 boot_seq 不变仍唯一，因为 counter 6 hex = 16M）
- 语义：**仅需"设备内短期唯一"**（CommandManager 只在运行期窗口内查重）
- 禁止：使用客户端提供值（Homie set 载荷**没有** cmd_id 字段）
```

### 7.4 结果与属性状态回写（FROZEN — AD-7）
**不新增 CommandManager 回调**（全局只有 1 个结果回调槽，已被 CloudManager 占用）。
```
Homie Bridge 维持**定容 pending 表**（≤ 4 条）：{ cmd_id, node, prop, expect_value, deadline_ms }
每轮 homie_task():
  for each pending:
     rt = command_manager_get_runtime(cmd_id)
     if rt == nullptr                 → SUCCESS/FAILED 已释放 ⇒ 以**最后一次已知结果**收尾（见下）
     else if rt->state == COMMAND_STATE_SUCCESS  → 成功
     else if rt->state == COMMAND_STATE_FAILED   → 失败
     else if rt->state == COMMAND_STATE_TIMEOUT  → 超时
  成功 ⇒ **不直接写属性值**；等待 System State 变化自然反映（下轮变化检测再发布）
  失败/超时 ⇒ log_emit(WARN) + 释放槽位，**property 保持原值**（Homie 无 ACK 通道）
```
> **FROZEN（AD-7a）**：**不使用** `command_manager_set_result_callback()`（会窃取 CloudManager 的回调）。
> **FROZEN（AD-7b）**：错误**不**发布到 `guo_feeder/.../up`（防污染业务上行）；亦**不**改 `$state`（Homie `$alert` 见 §20 DEFERRED）。

### 7.5 去重责任（FROZEN）
| 层 | 现状 | P2 处理 |
|---|---|---|
| MQTT 层（cloud_manager 10 项/30 s） | 只服务业务下行 | **Homie 不复用**（F-10）⇒ Homie 侧**不新增去重缓存**；依赖 §14 的 `retain=1` 丢弃 + QoS1 |
| CommandManager 层（运行中 cmd_id） | 已有（F-9） | **自动继承**；`cmd_id` 唯一由 §7.3 保证 |
| 残留风险 | MQTT 重投同一 set（QoS1 重传）⇒ 生成**新** cmd_id ⇒ **二次执行** | **已知接受**（DEFERRED，见 §20 D-3） |

---

## 8. MQTT Topic Design

### 8.1 Base topic（FROZEN — AD-8）
```
homie/5/<device_id>/
        ▲        ▲
        │        └─ DeviceIdentity 的 12 位小写 hex（`aabbccddeeff`）
        └─ Homie **主版本号段**（规范要求；**不可省、不可改**）
```
- `device_id` 字符集满足 Homie ID 规则（小写字母/数字）。
- **与业务 topic 并存**：`guo_feeder/<device_id>/{down,up,log}`（F-31，规划已冻结）。
- **不改动**任何现有业务 topic（P2 零破坏）。

### 8.2 ★ Homie 5 vs Homie 4 属性模型（必须纠正的既有认知）
> **Homie 5 已废除 v4 的"每个属性一个 `$` topic"模型**：`$name` / `$nodes` / `$properties` / `$datatype` / `$unit` / `$settable` / `$retained` / `$format` **不再是 topic**，全部并入**单个 `$description` JSON**。

**设备级 topic（Homie 5 实际存在的 4 个）**
| Topic | 必需 | P2 是否实现 | 说明 |
|---|---|---|---|
| `$state` | ✅ | **实现** | retained；生命周期（§9/§10） |
| `$description` | ✅ | **实现** | retained；JSON 文档（§11） |
| `$log/{level}` | ❌ | **不实现（DEFERRED）** | 设备已有 LogManager 走业务 `guo_feeder/<id>/log`；**不重复造第二条日志通道** |
| `$alert/{alert_id}` | ❌ | **不实现（DEFERRED）** | 见 §20 D-2 |

> ⚠️ **评审纠正**：任务书 §8.2 列出的 `$nodes` / `$name` / `$extensions` **不是 Homie 5 的 topic**，而是 `$description` 的**字段**。本设计按 Homie 5 实际规范执行，`$name`/`$nodes`/`$extensions` **不出现在 topic 层**。

### 8.3 Node / Property topic（FROZEN）
| Topic | 方向 | retained | QoS | 实现 |
|---|---|---|---|---|
| `homie/5/<id>/<node>/<prop>` | 设备 → 客户端 | 按 property 的 `retained`（默认 true） | 1 | ✅ |
| `homie/5/<id>/<node>/<prop>/set` | 客户端 → 设备 | **由客户端决定；设备忽略 retain=1** | 1 | ✅ |

- 订阅 filter（**唯一，精确，不用 `#`**）：`homie/5/<device_id>/+/+/set`
- 节点/属性**不设子 `$` topic**（Homie 5）。
- `<prop>/set` 的 payload = **裸值**（`open` / `true` / `22` / `{"a":1}`），**不是** JSON 信封。

> **类别标注（§3.4）**：上表 **retained 列** —— 属性状态行属**工程选择**（由 `$description` 的 `retained` 字段声明）；**QoS 列（= 1）全部属 Guo Feeder transport profile（非规范要求）**。`/set` 行"retained 由客户端决定"是**规范未约束项**，本设备**强制忽略 `retain=1`**（§14 AD-14a）。

### 8.4 Topic 矩阵（实现范围总表）
| 层级 | Topic | P2 | 备注 |
|---|---|---|---|
| 设备 | `homie/5/<id>/$state` | ✅ | §10 |
| 设备 | `homie/5/<id>/$description` | ✅ | §11 |
| 设备 | `homie/5/<id>/$log/{level}` | ❌ DEFERRED | 复用业务 log |
| 设备 | `homie/5/<id>/$alert/{id}` | ❌ DEFERRED | §20 D-2 |
| 节点 | `homie/5/<id>/<node>/$name` | ❌ N/A | v5 无此 topic（进 `$description`） |
| 节点 | `homie/5/<id>/<node>/$properties` | ❌ N/A | v5 无此 topic |
| 属性 | `homie/5/<id>/<node>/<prop>` | ✅ | 状态 |
| 属性 | `homie/5/<id>/<node>/<prop>/set` | ✅ | 写 |
| 设备层级 | `root` / `parent` / `children` | ❌ | 单设备无层级（字段省略） |

---

## 9. Homie Lifecycle

```
上电
 └─ Homie 初始化（model 装载；**不发布任何东西**）
     └─ MQTT 已连接？
         ├─ 否 ────────────────► 等待（$state 未发布；broker 侧无 retained 值）
         └─ 是
             ├─ publish $state = "init"            (retained, QoS1)
             ├─ 构建 $description（从 Device Model + Registry 实时过滤）
             │    ├─ 失败 ⇒ **停在 init** + log ERROR（**绝不发布 ready**）
             │    └─ 成功 ⇒ publish $description   (retained, QoS1)
             ├─ publish 所有 retained 只读属性初值   (QoS1)
             └─ publish $state = "ready"           (retained, QoS1)
```
**异常/边界（FROZEN）**
| 场景 | 行为 |
|---|---|
| MQTT 断开（非本机主动） | 设备不发布（`lost` 由 **broker 依 LWT** 发布，见 §10） |
| 重连成功 | **重走** `init → description → 属性初值 → ready`（幂等；retained 覆盖同值） |
| 凭据换连 | 先 `$state = "disconnected"`（**仅在能发出的窗口内**）→ 换连 → 重走 init→ready |
| 安全重启倒计时（10 s） | 同上，尽力发 `disconnected` |
| Registry 重建（运行期） | **不发任何 Homie 消息**（§15） |
| `$description` 构建失败 | 维持 `init`；**不发布 `ready`**；WARN/ERROR 落 LogManager |
| MQTT 未连接即上电 | 不发布；连接后走全流程 |
| 设备即将下线（正常） | 发布 `$state = ""`（**空 payload = 注销**）—— **FROZEN：P2 不实现**（设备无"优雅下线"语义），DEFERRED |

---

## 10. `$state` Semantics

### 10.1 取值（Homie 5 共 6 态）
| 值 | 含义 | P2 |
|---|---|---|
| `init` | 已连 broker，但**尚未发完全部 Homie 消息**，不可操作 | ✅ 使用（boot / 重连 / `$description` 构建失败暂留） |
| `ready` | **全部 Homie 消息已发布，设备可操作** | ✅ 使用 |
| `disconnected` | 设备**主动**断开（有意） | ✅ 使用（凭据换连 / 重启倒计时） |
| `lost` | **LWT**：非预期断线 | ✅ 使用（仅由 broker 发） |
| `sleeping` | 深度睡眠中 | ❌ **REJECTED**（本设备无 deep-sleep 业务） |
| `alert` | 告警态 | ❌ DEFERRED（§20 D-2） |

### 10.2 ★ `ready` 的**精确定义**（本设计冻结，禁止简化为"MQTT connected"）
```
$state == ready  ⇔  ① mqtt_transport_is_connected()        （MQTT 已连）
                 ∧  ② $description 已成功发布               （描述可用）
                 ∧  ③ 只读属性初值已发布                     （状态可用）
                 ∧  ④ capability_registry_init() 已成功      （能力集可用）
                 ∧  ⑤ command_manager 已初始化               （写通道可用）
```
**与 `STATE_MQTT_STATUS` 的关系（FROZEN）**
- `STATE_MQTT_STATUS` = **仅 ①**；它是 `ready` 的**必要非充分**条件。
- ⇒ **实现铁律**：`STATE_MQTT_STATUS == true` **不得**直接推导 `$state = ready`。

### 10.3 retained / QoS / LWT（FROZEN）
| 项 | 取值 | 依据 | 类别（§3.4） |
|---|---|---|---|
| `$state` retained | **true** | Homie 规范强制 | **semantic requirement** |
| `$state` QoS | **1** | 与全系统 QoS1 统一（F-16） | **transport profile（非规范要求）** |
| LWT topic | `homie/5/<device_id>/$state` | 规范强制 | **semantic requirement** |
| LWT payload | **`lost`** | 规范强制 | **semantic requirement** |
| LWT retain | **true** | 规范强制 | **semantic requirement** |
| LWT QoS | **1** | 与全系统 QoS1 统一（F-16） | **transport profile（非规范要求）** |
| LWT 配置位置 | `cloud_init()` 的 `cfg.lwt_*`（**F-13 目前缺失 ⇒ 必须新增**） | §7.1 T-1 | 实现细节 |
| clean session | 保持 `disable_clean_session = 0`（**不改**，F-14） | 避免触发 P1 的会话语义变化 | **transport profile** |

> **FROZEN（AD-3d）**：本表**原表述**将 LWT 的 "QoS / retain" 合并标为"规范"——**已按 §3.4 拆分**：**retain 属规范，QoS 属本工程**。`$state` / LWT 的 **QoS=1 不是** Homie 要求。

> **FROZEN（AD-10a）**：LWT 只在 **Homie 启用**时设置。若 Homie 编译期关闭（`GF_HOMIE_ENABLED=0`，默认 0 ⇒ 生产零代价），LWT **不得**设置，保持 P1 现状。

---

## 11. `$description` Semantics

### 11.1 文档结构（Homie 5，FROZEN）
```json
{
  "homie": "5.0",
  "version": 28456017,
  "nodes": {
    "water": {
      "name": "供水",
      "type": "guo-feeder/water",
      "properties": {
        "valve": { "name": "电磁阀", "datatype": "enum", "format": "open,close", "settable": true, "retained": true },
        "weight":{ "name": "重量", "datatype": "float", "unit": "g", "settable": false, "retained": true }
      }
    }
  }
}
```
> **注**：`nodes` 用**对象（keyed by node id）**形式（Homie 5 现行规范）；`root`/`parent`/`children`/`extensions` **省略**（单设备、无扩展）。
> **★ 设备级 `name` 字段（FROZEN — §24 OQ-1 / §20 D-8）**：**P2 不引入设备友好名 Config**。设备级 `name` **可省略** ⇒ Homie 客户端按规范**回落 device identity**（上例即**省略**设备级 `name`；`water` 是 **node 级** `name`，与此无关）。若实现/文档需要示例，可用 `"name": "guo-feeder <device_id>"`，但该串**仅为文档示例，不是 P2 runtime contract** —— 实现者**不得**据此新增"设备名 Config"字段。友好名统一延后 **P3（D-8）**。
> **注（节点级）**：node 的 `name`（如 `"供水"`）与 property 的 `name`（如 `"电磁阀"`）来自 Device Model 静态表（§5.3 `DeviceModelNode.name` / `DeviceModelProperty.name`），**与设备友好名无关**。

### 11.2 生成时机（FROZEN — AD-10）
| 时点 | 是否生成/发布 |
|---|---|
| boot 首次 MQTT 连接后（`$state=init` 窗口） | ✅ **唯一生成点** |
| MQTT 重连 | ✅ 重新发布（**内容不变**，retained 覆盖同值 ⇒ 不违规范） |
| Registry 运行期重建（workflow CRUD） | ❌ **不发布**（§15） |
| 属性状态变化 | ❌ 绝不重发 |
| 固件升级 / `weight.enable` 变更 | ✅ 下次 boot 的 `init` 窗口自然生效 |

**依据**：Homie 规范规定 `$description` **只能在 `$state ∈ {init, disconnected, lost}` 时变更**。⇒ **运行期"随时重发"在规范上非法**（这是 AD-10 的根因）。

### 11.3 version（FROZEN）
```
version = fnv1a32( GF_HOMIE_DESC_VERSION , action_registry.version , trigger_registry.version )
```
- **语义**：同一台设备上，**description 内容变 ⇔ version 变**（F-4 的两个 version 恰好覆盖"配置/固件导致的能力集变化"）。
- **禁止**假定单调递增（规范允许 timestamp/random）⇒ 客户端**必须**用"不等"而非"更大"判断变化。
- `GF_HOMIE_DESC_VERSION`：编译期常量（表结构变更时手工 +1）。

### 11.4 尺寸与缓冲（FROZEN）
| 项 | 预算 |
|---|---|
| 目标体积 | **≤ 2048 B**（20 属性典型值 ≈ 1.2 KB） |
| 硬上限 | **4096 B**（超过 ⇒ **不发布** `$description` + ERROR 日志 + 停在 `init`） |
| 缓冲区 | **单块 PSRAM 堆**：`heap_caps_malloc(HOMIE_DESC_CAP, MALLOC_CAP_SPIRAM)`（失败回退 `MALLOC_CAP_8BIT`），**boot 时分配一次并复用** |
| 是否分片 | **不分片**（4096 < `CLOUD_MQTT_BUFFER_SIZE`=8192，F-16）⇒ 单次 publish |
| 持久化 | **无**（不落 LittleFS；纯运行时生成） |

> **FROZEN（AD-11a）**：**严禁** `EXT_RAM_BSS_ATTR` 静态大数组；一律走 `heap_caps_malloc`（与 F-25 既有范式一致）。

---

## 12. Node / Property Semantics

### 12.1 现有可投影能力清单（源码实证 — 这是投影表的唯一输入）

**Action（6 个已注册）**
| runtime_id | module | name | param_count | 来源 |
|---|---|---|---|---|
| `VALVE_OPEN` | `valve` | 电磁阀开 | 0 | `valve.cpp:83` |
| `VALVE_CLOSE` | `valve` | 电磁阀关 | 0 | `valve.cpp:95` |
| `WEIGHT_ZERO` | `weight` | 电子秤零点校准 | 0 | `weight.cpp:142` |
| `COMPUTER_RESET` | `computer_reset` | 电脑重启脉冲 | 0 | `computer_reset.cpp:235` |
| `MI_THERMO_START_SCAN` | `mi_thermo` | 启动扫描 | — | `MiThermometer.cpp:737` |
| `MI_THERMO_STOP_SCAN` | `mi_thermo` | 停止扫描 | — | `MiThermometer.cpp:752` |
| （`weight.enable=false` ⇒ 前 3 项中的 weight 项**不注册**） | | | | `weight.cpp:745` |

**Trigger（1 个）**：`weight_decrease`（`weight.cpp:123`，float `gram`，0<gram≤300）
> 事件语义的 Trigger **不投影**（Homie 无事件模型）⇒ 见 §12.3。

**System State 可读项（21 项，白名单见 §12.2）**：`STATE_WIFI_{STATUS,RSSI,SIGNAL,STATE}` · `STATE_TIME_VALID` · `STATE_VALVE_STATUS` · `STATE_WEIGHT_{VALUE,ERROR}` · `STATE_EVENT_QUEUE_COUNT` · `STATE_MQTT_{STATUS,RETRY_COUNT,LAST_CONNECT_TIME,LAST_ERROR}` · `STATE_MI_THERMO_{TEMP,HUMID,BAT_V,TEMP_TS,HUMID_TS,BAT_TS,ENABLE,VALID}`

### 12.2 ★ Capability → Homie 投影规则表（P2-0 核心产出，**FROZEN**）

| Capability / State（已有） | node | property | datatype | format | unit | settable | retained | 生成规则 |
|---|---|---|---|---|---|---|---|---|
| `VALVE_OPEN` + `VALVE_CLOSE`（**2 Action → 1 属性**） | `water` | `valve` | `enum` | `open,close` | — | ✅ | ✅ | **多 Action 合并为一个 enum 属性**；`set` 值 → Action 映射 |
| `STATE_VALVE_STATUS`（bool） | `water` | `valve` | — | — | — | — | — | **作为上条的"读侧"**：`true→"open"`、`false→"close"`（**同属性双面**，见 §12.4 / §12.5） |
| `STATE_WEIGHT_VALUE`（float） | `water` | `weight` | `float` | `0:10000:1` | `g` | ❌ | ✅ | 数值状态；死区 1.0 |
| `STATE_WEIGHT_ERROR`（bool） | `water` | `weight-error` | `boolean` | — | — | ❌ | ✅ | 布尔状态 |
| `WEIGHT_ZERO`（0 参 Action） | `water` | `zero` | `boolean` | — | — | ✅ | ❌ | **命令型属性**：写 `true` ⇒ 执行；不保留（防重放） |
| `STATE_MI_THERMO_TEMP`（float） | `climate` | `temperature` | `float` | — | `°C` | ❌ | ✅ | 有效性由 `STATE_MI_THERMO_VALID` 门控（`false` ⇒ 不发） |
| `STATE_MI_THERMO_HUMID`（float） | `climate` | `humidity` | `float` | `0:100` | `%` | ❌ | ✅ | 同上 |
| `STATE_MI_THERMO_BAT_V`（float） | `climate` | `battery` | `float` | — | `V` | ❌ | ✅ | 同上 |
| `STATE_MI_THERMO_ENABLE`（bool） | `climate` | `scan-enabled` | `boolean` | — | — | ❌ | ✅ | — |
| `MI_THERMO_START_SCAN`+`STOP_SCAN`（2 Action → 1 属性） | `climate` | `scan` | `enum` | `start,stop` | — | ✅ | ❌ | 命令型枚举 |
| `STATE_MQTT_STATUS`（bool） | `system` | `mqtt-connected` | `boolean` | — | — | ❌ | ✅ | — |
| `STATE_WIFI_RSSI`（int） | `system` | `wifi-rssi` | `integer` | — | `dBm` | ❌ | ✅ | — |
| `STATE_WIFI_SIGNAL`（int 0..4） | `system` | `wifi-signal` | `enum` | `0,1,2,3,4` | — | ❌ | ✅ | 离散等级 → enum |
| `STATE_TIME_VALID`（bool） | `system` | `time-valid` | `boolean` | — | — | ❌ | ✅ | — |
| `STATE_EVENT_QUEUE_COUNT`（int） | `system` | `event-queue` | `integer` | — | — | ❌ | ✅ | — |
| `STATE_MQTT_RETRY_COUNT`（int） | `system` | `mqtt-retries` | `integer` | — | — | ❌ | ✅ | — |
| `COMPUTER_RESET`（0 参 Action） | `control` | `computer-reset` | `boolean` | — | — | ✅ | ❌ | **瞬时命令**（不保留） |
| `WorkflowActionDescriptor.name/description` | — | property `name` | — | — | — | — | — | `name` 进 `$description` 的 **node / property 级 `name` 字段**（**非**设备级 `name`，§11.1） |
| `WorkflowParam.unit` | — | property `unit` | — | — | — | — | — | 空则**不发**该字段 |
| `WorkflowParam.type`（`PARAM_INT/FLOAT/BOOL/STRING`） | — | `datatype` | `integer/float/boolean/string` | — | — | — | — | 4→4 **一一对应**（F-6） |
| **`Workflow`（含 variant）** | — | **不投影** | — | — | — | — | — | **AD-9：用户数据不进 `$description`** |
| **`weight_decrease` 等事件 Trigger** | — | **不投影** | — | — | — | — | — | Homie 无事件语义 |
| `capability_registry.version/checksum` | — | **不投影为属性** | — | — | — | — | — | 仅用于 `$description.version`（§11.3） |

### 12.3 不投影的东西（FROZEN — 显式排除）
| 项 | 理由 |
|---|---|
| **Workflow（16 槽）** | **用户数据**非设备能力；运行期可变 ⇒ 会违反 `$description` 不可变性（§11.2）；且**无界** ⇒ 体积不可控 |
| 事件类 Trigger | Homie 无事件模型（只有状态属性） |
| `STATE_MQTT_LAST_ERROR`（String） | **内部诊断串**，可能含 SSID/主机名等 ⇒ **白名单排除** |
| `STATE_*_TS`（long 时间戳 ×3） | P2 不投影（Homie `datetime` 要求 ISO8601 字符串，需格式化 ⇒ 增成本）；DEFERRED |
| `STATE_MI_THERMO_VALID` | 作为**门控**使用，不单独暴露 |
| `STATE_WIFI_STATE`（枚举机内部态） | 内部状态机语义，非用户语义 |

### 12.4 同属性"双面"（读+写）规则（FROZEN）
一个 property **可以同时**有读侧（`state_key`）与写侧（`cmds`）：
- `water/valve` 既是**可写**（enum → 两个 Action），又是**可读**（`STATE_VALVE_STATUS` 映射为 `open`/`close`）；
- **写后不改写内存**：`set` 只触发命令；属性值的最终来源**永远**是 System State（单一事实源）。
- **值映射函数**：读侧映射由 `dtype` 决定（bool → `"true"`/`"false"`；enum 需要「状态值 → 枚举成员」映射；P2 用**显式映射表** `enum_state_map[]`，见 §5.3/§12.5，不用隐式推断）。

### 12.5 ★ `water/valve` 显式状态 / Action 映射（FROZEN）

> `water/valve` 是"同属性双面"的**唯一样板**；其**读 / 写两侧映射必须显式**，**禁止任何字符串推导**。

**property 静态定义（Device Model 表）**
```
node       = "water"
id         = "valve"
datatype   = DM_ENUM
format     = "open,close"
settable   = true
retained   = true
src_kind   = DM_SRC_STATE
state_key  = STATE_VALVE_STATUS
```

**写方向（`/set` → Action）：显式 `cmds[]` 映射表**

| `/set` 载荷（裸值） | Action `runtime_id` | payload |
|---|---|---|
| `"open"` | `VALVE_OPEN` | `{}` |
| `"close"` | `VALVE_CLOSE` | `{}` |

**读方向（System State → property 值）：显式 `enum_state_map[]` 映射表**

| `STATE_VALVE_STATUS` | 发布值 |
|---|---|
| `true` | `"open"` |
| `false` | `"close"` |

**铁律（FROZEN）**
1. **禁止**根据字符串自动拼接 Action ID（如 `"VALVE_" + upper(value)`）⇒ 必须查 `cmds[]` 显式表。
2. **禁止**根据 `runtime_id` 字符串推导 enum 成员 ⇒ 必须查 `enum_state_map[]` 显式表。
3. Device Model **使用显式 mapping table**（写侧 `cmds[]` / 读侧 `enum_state_map[]`）。
4. `set` 与 read **使用同一 property**，但**状态事实源永远是 System State**。
5. **`set` 成功后不得直接修改 property 内存值**；**必须等待 System State 变化后**由变化检测发布 property（§17.4 / AD-7）。
6. §5.3 `DeviceModelCommand` / `dm_resolve_command()` 契约**已能表达**写侧映射（`value → action_id + payload_json`）；读侧由 `enum_state_map` + `dtype` 表达。本次修订**仅补齐 `enum_state_map` 字段**，**不改变** AD-1 / AD-2 等既有决策。

---

## 13. Datatype / Validation

### 13.1 类型映射（FROZEN）
| Device Model | Homie `datatype` | `format` 规则 |
|---|---|---|
| `DM_BOOL` | `boolean` | 无 |
| `DM_INT` | `integer` | `min:max` / `min:` / `:max`（可选） |
| `DM_FLOAT` | `float` | `min:max` / `min:max:step` |
| `DM_STRING` | `string` | 无 |
| `DM_ENUM` | `enum` | **逗号分隔成员表**（必须非空） |
| `DM_JSON` | `json` | 无（**P2 暂不使用**，DEFERRED） |

> **未使用（FROZEN 明确排除）**：`color`、`datetime`、`duration`。

### 13.2 取值校验（FROZEN — 两层，不重复担责）
| 层 | 校验内容 | 失败行为 |
|---|---|---|
| **① Homie Bridge（协议层）** | 属性存在 · `settable==true` · dtype 可解析 · enum 成员在 `format` 表内 · 数值在 `format` 范围内 | 丢弃 + `log_emit(WARN)`；**不产生 cmd_id、不进 CommandManager** |
| **② 业务层（已有）** | Action 参数语义（如 `weight_decrease.gram ∈ (0,300]`） | 现有 `command_send_error` 路径（**保持原样**） |

**关键原则（FROZEN）**：协议层**只做"形状"校验**，**不复制**业务语义校验（避免两处规则漂移）。

### 13.3 枚举大小写与空白（FROZEN）
- 枚举成员**全小写**、`-` 分词；`set` 载荷比较**大小写敏感**（规范未定义大小写折叠 ⇒ 不引入隐式折叠）。
- 载荷**不 trim**（严格匹配）；如需容错，由**客户端**负责。

---

## 14. Retained / QoS

| 消息 | retain | QoS | store | 说明 |
|---|---|---|---|---|
| `$state` | **true**（规范） | 1 | true | 规范要求 retained |
| `$description` | **true**（规范） | 1 | true | 规范要求 retained |
| 只读状态属性 | **true**（模型可关，工程选择） | 1 | true | 客户端订阅即得初值 |
| 命令型属性（`zero` / `computer-reset` / `scan`） | **false**（工程选择） | 1 | true | **防重放**：不让命令残留 |
| `set`（设备订阅侧） | 客户端决定 | 1 | — | **设备强制忽略 `retain=1`**（F-20 的 `event->retain`） |

> **FROZEN（AD-14b）**：本表 **QoS 列**（全部 = 1）属 **Guo Feeder transport profile**（§3.4），**非 Homie 规范要求**。**retain 列**：`$state`/`$description` 属**规范强制**，其余为**工程选择**。二者**不得混述**（如"规范要求 retained + QoS1"）。

**基础设施缺口（必须补，见 §7.1）**
- ❌ 当前发布层 **retain 恒为 0**（F-15）⇒ T-4 必做。
- ✅ `CLOUD_MQTT_STORE=true`（F-16）⇒ QoS1 会入 outbox 重试。
- ✅ clean session = true（F-14）⇒ **不改**；retained 消息由 broker 侧保管，与会话无关。

> **FROZEN（AD-14a）**：`set` 忽略 retained 投递是**安全要求**（否则新订阅者会立刻执行陈旧命令），实现上以 `event->retain` 判定，**不得**用其它启发式替代。

---

## 15. Registry Rebuild

### 15.1 触发链（代码实证）
```
workflow.create/set/delete/save  →  command_manager.cpp:1884/1993/2081/2137
                                 →  capability_registry_rescan()
                                 →  registry_sync(ACTION|TRIGGER|WORKFLOW)
                                 →  真正 rebuild 时 log_emit(LOG_REG_REBUILT=0x0B01)
```

### 15.2 ★ 结论：**Registry 重建不影响 `$description`**（FROZEN）
| 问题 | 决策 |
|---|---|
| rebuild 是否触发 `$description` 重生成？ | **否** |
| 理由 | 触发源**全是 workflow CRUD**（F-22），而 **Workflow 不投影**（AD-9）⇒ Action/Trigger 表**不变** ⇒ description 内容**必然不变** |
| 是否允许无条件重发大包？ | **禁止**（明确 REJECTED：违反 §11.2 规范 + 无谓流量） |
| debounce / throttle 是否还需要？ | **不需要**（因为根本不发布）；但**保留一条**：若未来接入"描述可变"能力，必须 **≥5 s 合并窗口**（DEFERRED 机制的约束） |
| description 与 state 的先后顺序 | `$state=init` → `$description` → **属性初值** → `$state=ready`（§9） |
| registry 未 ready 时 Homie 状态 | 停在 `init`（**不发布 `ready`**） |
| rebuild 失败 | 现有行为不变（`registry_sync` 返回 false）；Homie **不感知**（不影响已发布内容） |
| description 生成失败 | 停在 `init` + `log_emit(ERROR)`；**不发布** `ready` |
| 是否需要 generation/version | `version`（§11.3）**已足够**；不引入额外 generation |

> **FROZEN（AD-15a）**：实现**必须**包含一条**运行期一致性哨兵**：`homie_task()` 周期性比较"当前 registry 指纹（F-4）"与"上次生成 description 时的指纹"；若**不一致** ⇒ `log_emit(WARN, LOG_HOMIE_DESC_STALE)`（新增日志 EventId 需在 P2 走 LogManager 冻结流程），**但不重发**。正式定义见 §15.3 ⑤。

### 15.3 ★ Projection Contract 冻结语义（FROZEN — BLOCKER-2）

> **P2 Homie Projection 的 capability/property projection contract 在 boot-time description generation 时确定。** 以下 5 条为 contract 语义，P2-3 / P2-4 实现**必须**遵守。

**① 生成窗口**
`$description` **只能**在允许的 lifecycle 窗口生成/更新，即 `$state ∈ {init, disconnected, lost}`（§11.2）。P2 的**唯一生成点**是 **boot 首次 MQTT 连接后的 `init` 窗口**（AD-10）；`lost` 由 broker 发出，设备**不**据此重生成（§9）。

**② Workflow CRUD ⇒ Registry rebuild 时**
- **不重新生成** `$description`
- **不重新发布** `$description`
- **不修改**已建立的 Homie description contract（property 集合 / `datatype` / `format` / `settable` / `retained` **一律不变**）

**③ runtime Registry 与 description snapshot 不一致时**
- **禁止** silently 改变 Homie property 的 **settable / availability contract**（例：不得因某 Action 从 runtime registry 消失，就把已发布 property 偷偷改成 read-only 或删除）
- **记录 `WARN`**（`LOG_HOMIE_DESC_STALE`，AD-15a）
- **不进入 `$state=ready` 重建流程**（不因不一致而重走 `init → description → ready`）

**④ 对"已存在但 runtime 已失效"的 property 发 `/set`**
- **仍进入正常协议层解析**（§7.2 步骤 ①–③：`retain` 判定 → Device Model 查表 → dtype / enum / range 校验）
- 若最终 Action **不存在 / 不可执行** ⇒ 由**现有业务层**（CommandManager → Workflow/Action）**拒绝**（§13.2 第②层）
- **禁止** Homie 自己绕过 CommandManager 做"提前拒绝"或"代执行"

**⑤ description snapshot fingerprint（新增冻结概念）**
```
boot 生成 $description 时：
    desc_fp = fnv1a32( action_registry.version , trigger_registry.version )   // §11.3 / F-4
    记录到 homie_bridge 内部（RAM）

运行期 homie_task()：
    cur_fp = fnv1a32( action_registry.version , trigger_registry.version )
    if (cur_fp != desc_fp) → log_emit(WARN, LOG_HOMIE_DESC_STALE)   // 限频；不重发、不改 $state
```
- fingerprint **只读**、**只告警**；**不得**触发 description 重生成 / 重发布 / contract 变更。
- fingerprint 不一致是**合法运行态**（Workflow CRUD 必然改 `version`）⇒ 定性为 **WARN（非 ERROR）**，且**不阻塞** `$state` 维持 `ready`。

> **FROZEN（AD-15a′）**：AD-15a 的"陈旧哨兵"在本节正式定义为 **description snapshot fingerprint**；二者为同一机制，全文表述统一为 fingerprint。
> **FROZEN（AD-15b）**：`weight.enable=false` 导致的"本次 boot 未注册某能力" ⇒ 该 property **在 boot 生成 `$description` 时就不进入**（属**静态 Device Model + boot-time Registry availability filter**，§4.3 / §12.2 生成规则），**不属**运行期不一致 ⇒ **不触发** fingerprint WARN。
> **DEFERRED（D-4，不变）**：运行期 `$description` 刷新机制（`init` 重入周期）留待 P2.5/P3 需要时设计；**本次修订不重新设计 D-4**。

---

## 16. ACL Design

### 16.1 边界（FROZEN — AD-11）
```
❌ 绝对禁止：homie/5/#      homie/5/+/#      homie/#      （跨设备）
✅ 允许：    homie/5/<device_id>/#  ·  homie/5/<device_id>/+/+/set
```

### 16.2 ACL 矩阵（FROZEN）
| 主体（username） | topic | 动作 | 权限 |
|---|---|---|---|
| 设备 `dev_<id>` | `guo_feeder/<id>/down` | sub | ✅（既有，不变） |
| 设备 `dev_<id>` | `guo_feeder/<id>/up` | pub | ✅（既有，不变） |
| 设备 `dev_<id>` | `guo_feeder/<id>/log` | pub | ✅（既有，不变） |
| 设备 `dev_<id>` | `homie/5/<id>/#` | **pub** | ✅ **新增** |
| 设备 `dev_<id>` | `homie/5/<id>/+/+/set` | **sub** | ✅ **新增** |
| UI `app_slot_<n>`（P4/P5 落地） | `homie/5/<id>/#` | **sub** | ✅ **新增（P2-5 仅登记，不发凭据）** |
| UI `app_slot_<n>` | `homie/5/<id>/+/+/set` | **pub** | ✅ **新增（同上）** |
| 任意主体 | `homie/5/+/#` 等跨设备通配 | any | ❌ **deny（兜底）** |
| 兜底 | `#` | pubsub | ❌ deny（**保持不动**，F-29） |

**设计说明（FROZEN）**
1. **设备 publish 用 `homie/5/<id>/#`**：无法用单条规则表达"除 `set` 外全部"（ACL 无排除语义）⇒ 退而求其次；**设备向自己的 `set` 发布是无害自环**（且设备根本不订阅自己要发的路径之外的东西）。
2. **仅允许"设备子树内"通配**（`+/+/set`），**设备 ID 段永不使用通配** ⇒ 跨设备隔离**不破**（P1 的核心不变量）。
3. **legacy topic（`guo_feeder/down|up|log`）零改动**（仍在过渡期，退出条件见规划）。
4. **不需要新的 credential generation**：ACL 是账号的**规则集**，不是凭据。设备账号复用（`dev_<id>`，P1 已一机一账号）。

### 16.3 落地路径（FROZEN — P2-5）
| 步 | 动作 | 仓库 |
|---|---|---|
| 1 | `credential.js` 的设备 ACL 模板**追加 2 条**（pub `homie/5/<id>/#` · sub `homie/5/<id>/+/+/set`） | Cloudflare |
| 2 | `acl-rules.v3.json` 增 `homieRuleTemplate`（供 `apply-acl.mjs` 展开）+ 更新 `_rules` ⑤ 说明 | EMQX |
| 3 | 已在册设备（生产 `288485896ce4` 等）的 ACL **补写**需**显式授权**（`replaceUserAclRules` 是 **PUT 覆盖式**，F-28 ⇒ 必须**一次给全 5 条**，**不得**只发 2 条） | Cloudflare/EMQX |
| 4 | 验证：设备账号订阅数 = **2**（业务 down + homie `/+/+/set`）；越权订阅 `homie/5/<他机>/#` 必须被拒 | 实测 |

> **STOP 条件**：若"补写"必须改动 D1 历史或触发凭据轮换 ⇒ **STOP**（属另一授权任务）。

### 16.4 ★ ACL 与 Capability Authorization 的职责边界（FROZEN）

> **四层职责不得互相顶替。** 冻结如下：

| 层 | 负责 | **不**负责 |
|---|---|---|
| **ACL** | **namespace / device isolation**（哪台设备可读写哪个 topic 子树） | per-property 能力授权 |
| **Device Model** | **semantic property existence + datatype + settable**（属性是否存在 / 类型是否合法 / 是否可写） | 运行期能力是否存在 |
| **Capability Registry** | **runtime capability availability**（Action / Trigger 本 boot 是否注册） | 属性语义 |
| **CommandManager** | **实际命令授权 / 执行校验**（业务层最终裁决） | namespace 隔离 |

**FROZEN 铁律**
- `homie/5/<device_id>/+/+/set` 的 ACL 通配**只用于 namespace isolation**，**不承担** per-property capability authorization。
- **不要**为每个 property 设计 ACL（ACL 无排除语义且规则会爆炸；per-property 语义属 Device Model + CommandManager）。
- 一个 `/set` **通过 ACL** 只说明"该设备账号有权写此子树"，**不**代表属性可写 ⇒ 仍需 **Device Model**（存在性 / settable / dtype）+ **CommandManager** 判定。

---

## 17. Resource Budget

### 17.1 当前基线（ELF 实测，F-26）
```
text = 1,120,774 B   data = 284,452 B   bss = 1,222,621 B   （bss 含 PSRAM 段）
```

### 17.2 P2 预算（**FROZEN — 上限，非目标** · 已按 P2-0.2 修订 Flash 条款）

| 项 | 预算 | 说明 |
|---|---|---|
| **新增 DRAM 静态数组** | **0 B** | 铁律；所有表 `const` → Flash（口径见 `P2-6-Resource-Acceptance.md` §4 正式裁决：约束针对**静态语义/描述数据表**，runtime state/cache 不属该行） |
| Flash `.rodata` | **不再单列为硬性 acceptance ceiling** | 保留为**观测项**（当前实测 +3,284 B） |
| Flash `.text` | **不再单列为硬性 acceptance ceiling** | 保留为**观测项**（当前实测 +9,264 B） |
| **Flash 合计增量** ★ | **≤ 14 KiB（14,336 B）** | **唯一 Flash acceptance ceiling**；相对 1.12 MB 基线 = **+1.22%** |
| PSRAM 堆（`$description` 缓冲） | **4096 B**（boot 分配一次，复用） | 非 static 数组 |
| DRAM 堆 | **0 B**（nominal） | 无动态字符串拼接；`MALLOC_CAP_8BIT` 回退路径存在 ⇒ 见 `P2-6-Resource-Acceptance.md` §5 `DRAM fallback = WARNING` |
| 栈 | `homie_task()` 帧 **≤ 512 B** | 禁大局部（含 JSON 写缓冲） |
| CPU | boot：构建 description **< 5 ms** 一次性<br>稳态：每 loop **~21 次** state 取值 + 比较 | 变化检测 |
| MQTT | description **单包 ≤ 4096 B** < 8192 缓冲（F-16）⇒ **不分片** | |
| 网络 | 稳态**零** Homie 流量（除属性变化） | 变化驱动，非轮询上报 |

#### 17.2.1 Flash Budget Amendment（**FROZEN · 2026-10-09**）

> **原冻结**：`.text +3…6 KB` · `.rodata +1.2…2.5 KB` · **合计 `+4…8.5 KB`（≤ 8,704 B）**
> **修订后**：`.text` / `.rodata` **不再单列为硬性 ceiling** ⇒ **Flash 合计增量上限 = `≤ 14 KiB`（14,336 B）**

**当前正式基线**（`6775bb3`，Homie-enabled 构型 B − base `623da59`，ELF 段级）：

```
.flash.text   增量 = +9,264 B
.flash.rodata 增量 = +3,284 B
Flash 合计增量     = +12,548 B
新 ceiling         = +14,336 B（14 KiB）
headroom           = +1,788 B（≈ 14.2%）
```

**修订理由（原文照录）**：

> Original P2-0 budget was based on an earlier and materially smaller implementation scope. The final frozen architecture includes Device Model, Homie Projection, Homie Bridge, MQTT Transport Facade/Routing, retained-set safety handling, pending command tracking, diagnostics, and 16 properties. The revised 14 KiB ceiling reflects the final frozen architecture and current measured implementation scale.

**追溯依据（`P2-Flash-Budget-Reconciliation.md` §1）**：原 `+3…6 KB` 预算**只有"行数"一个推算变量**（"估 400–700 行"），而其上游假设来自更早的范围（`Cloud-APP-Platform-Plan.md:428` 的 250–350 行 + §4.2 的 *Node 4–5 / Property 8–10*）；最终冻结范围扩为 **4 node / 16 property / 6 命令**，并新增 Device Model 独立层、Transport Facade、分片重组、订阅注册表、pending 结果分类、fingerprint 哨兵、诊断等，行数**未随之重估**。凡按"量"推导的预算项（属性表 1,546 B ≤ 2.5 KB、description 1,907 B ≤ 2,048 B）**实测全部达标**；失控仅发生在"只有行数"的 `.text` 上。归因结论 = **D. 预算与功能共同漂移**（非实现低效：密度 13.9 B/SLOC、符号级 100% 对账、无跨 TU 重复）。

**14 KiB 的性质**：是 **acceptance ceiling（上限）**，**不是**要求实现必须达到的目标值；**不得**描述为"Homie 本身需要 14 KiB"。

**本次修订的边界（FROZEN）**：
- **只**修改 §17.2 的 Flash 数字条款；**未**修改任何架构决策 / 行为契约 / ABI / topic 契约 / 安全语义。
- 上表其余各行（DRAM / PSRAM / 栈 / CPU / MQTT / 网络）**继续有效**，§17.3 禁止事项与 §17.4 变化检测 **继续有效**。
- **不得**通过删除安全机制（retained `/set` 防护、command validation、namespace/设备隔离）或削减已发布 ABI 来"满足预算"——修订后已无此需要。


### 17.3 禁止事项（FROZEN）
- ❌ 每个 property 一个固定大数组
- ❌ `EXT_RAM_BSS_ATTR` 静态缓冲
- ❌ 为 `$description` 引入 LittleFS 持久化
- ❌ 周期性无条件重发全部属性（必须**变化驱动 + 死区 + 最小间隔**）

### 17.4 变化检测与限流（FROZEN）
```
每个 retained 只读属性，每轮 homie_task():
   v_now = 读取来源（state_get_*）
   if (!valid) → 跳过（不发布）
   changed = (dtype == FLOAT|INT) ? (|v_now - v_last| >= deadband)
                                   : (v_now != v_last)
   if (changed && (now - last_pub_ms >= min_pub_ms)) → publish
   （min_pub_ms 默认 500；weight 类建议 1000）
```
- **首次**（`$state=ready` 前）无条件发布初值。
- 数值型**必须**有死区（否则 HX711 抖动会刷屏）。

---

## 18. OLED / Web / Android Relationship

### 18.1 结论（FROZEN — AD-12）
```
                Device Model（on-device，唯一语义源）
                 ├──────────────► Homie Projection ──► EMQX ──► Web / Android / 任何 Homie 客户端
                 ├──────────────► OLED UI（W-1）
                 └──────────────► （未来）按键 / 本地 Web 配置
```
| 消费者 | 如何获得语义 | 禁止 |
|---|---|---|
| **OLED（板载）** | **直接** include `homie/device_model.h` + 只读 `state_get_*` | 自己维护一份页面↔能力映射表 |
| **Homie Projection** | 消费 Device Model | — |
| **Web / Android（远端）** | **只消费 Homie 投影**（`$description` + 属性） | **不得**解析 Capability Registry；**不得**硬编码 per-device 控件 |
| 未来本地 Web 配置页 | 可消费 Device Model（同板载） | — |

**⇒ 明确回答任务 §11 的问题**：**P2-0 冻结 Device Model 作为"设备侧全部 UI / 投影"的共同语义层**（AD-12）。远端 UI 通过 Homie **间接**共享同一模型（这正是 Homie 的意义）。

### 18.2 为什么不是"各 UI 各解释一遍"（REJECTED 方案）
```
❌ REJECTED：
Capability Registry ─┬─► OLED（自己写映射）
                     ├─► Homie（自己写映射）
                     └─► Web（自己写映射）
```
理由：三份映射**必然漂移**；新增一个 Action 要改三处；且 Registry **根本没有可读面**（§4.2），OLED/Web 只能各自去读 System State 枚举 ⇒ 语义分叉不可控。

---

## 19. P2-7 Vertical Slice（**三级架构成立判据**）

### 19.1 测试对象（FROZEN）
**`water/valve`** —— 兼具**可写**（enum→2 Action）与**可读**（System State）⇒ 一条路径覆盖全部关键机制。

### 19.2 步骤与预期（FROZEN）
| # | 输入 / 动作 | 预期 topic | 预期 payload | 判据 |
|---|---|---|---|---|
| 1 | 设备上电 + MQTT 连接 | `homie/5/<id>/$state` | `init` → `ready` | retained；顺序正确 |
| 2 | 订阅 `homie/5/<id>/#`（客户端） | `homie/5/<id>/$description` | JSON 含 `nodes.water.properties.valve{datatype:enum,format:"open,close",settable:true}` | **自动发现成功** |
| 3 | 订阅 | `homie/5/<id>/water/valve` | `close`（retained 初值 = 关） | 自动渲染出控件 |
| 4 | 客户端 publish | `homie/5/<id>/water/valve/set` | `open` | — |
| 5 | **命令链路** | — | `command_manager_execute({command:"execute_action", object:"VALVE_OPEN", source:"homie"})` | CommandManager 产生 RESULT（`accepted`） |
| 6 | **硬件** | — | **电磁阀实际打开** | 物理动作 |
| 7 | **状态回传** | `homie/5/<id>/water/valve` | `open`（retained 更新） | UI 实时变化 |
| 8 | 失败路径 | `homie/5/<id>/water/valve/set` = `bogus` | 无任何 Homie 发布；WARN 落 LogManager | **协议层拒绝** |
| 9 | 安全路径（retained `/set`，**两场景 A/B**，见 §19.5） | `/set` 投递 `retain=1`（实时）**或** broker 重放 retained `/set` | **无任何 Homie 发布 / 无硬件动作** | `event->retain` 生效（**仅**此为判据） |
| 10 | 隔离路径 | 越权订阅 `homie/5/<他机>/#` | **SUBACK 拒绝** | ACL 生效 |

### 19.3 Failure cases（必须覆盖）
`$description` 超 4096 B ⇒ 停在 `init` · registry 未就绪 ⇒ 停在 `init` · 目标 Action 未注册（`weight.enable=false`）⇒ 该属性**不出现在** `$description` · MQTT 断开 ⇒ broker 发 `lost` · `set` 到只读属性 ⇒ 丢弃 + WARN · **retained `/set` 两场景（§19.5 场景 A/B）⇒ 丢弃 + 无硬件动作**。

### 19.4 通过标准（FROZEN — **三级语义**，BLOCKER-3）

> **原表述"第 7 步完成 = 架构成立（自描述 + 自动发现 + 控制闭环 + 身份 + 隔离全通）"已撤销** —— 它把"控制闭环"与"安全/隔离"混为一谈，且第 7 步本身**未包含**步骤 8–10 的失败/安全/隔离验证。**冻结为下述三级语义**：

**① Step 7 — Homie semantic / control-loop architecture proven**
证明**语义 + 控制闭环**成立：
```
description → discovery → set → CommandManager → hardware
            → System State → Homie property feedback
```
（对应步骤 **1–7**；**不含**安全 / 身份 / 隔离）

**② Step 10 — security / identity / cross-device isolation proven**
证明**安全 / 身份 / 跨设备隔离**成立：
- `device_id` binding（base topic 段 = DeviceIdentity）
- device-scoped topic（`homie/5/<id>/...`）
- ACL（设备子树内允许、跨设备通配拒绝）
- **cross-device SUBACK rejection**（越权订阅被 SUBACK 拒绝）

**③ P2-7 Overall PASS（Vertical Slice / Architecture Acceptance PASS）**
> **必须 Step 1～10 全部 PASS**，才能称为 **P2-7 Vertical Slice / Architecture Acceptance PASS**。

> **FROZEN（AD-19a）**：任何单一 step（**含 Step 7**）**单独 PASS 均不足以**宣告架构成立；架构成立判据 = **Step 1–10 全 PASS**。

### 19.5 retained `/set` 安全测试语义（FROZEN — 两场景）

> **保留原判定**：`event->retain == true` ⇒ **丢弃**（§14 AD-14a / §7.2 步骤 ①）。P2-7 的 **failure / security acceptance** 必须**分别**验证下列**两种**场景。

**场景 A — 实时 retained publish**
```
client publish /set  (retain=1)
→ 设备收到 event->retain == true
→ discard
→ 不生成 cmd_id
→ 不进入 CommandManager 执行
```

**场景 B — broker retained replay**
```
client 之前 retained 过一个 /set
→ 设备重连
→ broker 重放 retained /set
→ 设备收到 event->retain == true
→ discard
→ 无任何硬件动作
```

**判据铁律（FROZEN）**
- 判定依据**必须**是 `esp_mqtt_event_t::retain`（即 `event->retain`）。
- **禁止**以 payload / topic / 时间 / `cmd_id` 等**启发式**替代 retain 判定。
- 场景 A 验证"实时投递不回放"，场景 B 验证"重连不回放"；**二者必须都有 PASS 记录，缺一不可**。

---

## 20. Deferred Items

| ID | 项 | 目标阶段 | 理由 |
|---|---|---|---|
| **D-1** | `$log/{level}`（Homie 日志通道） | P3+ | 已有业务 `guo_feeder/<id>/log` + LogManager；**不重复造** |
| **D-2** | `$alert/{alert_id}`（告警态） | P2.5+ | 需要"什么算告警"的业务定义（属安全/产品决策） |
| **D-3** | Homie `set` 的 MQTT-QoS1 重投去重 | P2.5+ | 需与 CommandManager 协商"幂等 set"语义；当前**已知接受二次执行**（§7.5） |
| **D-4** | 运行期 `$description` 刷新（`init` 重入周期） | P2.5/P3 | 当前**无**运行期描述变化源（AD-9/§15.2） |
| **D-5** | `json` datatype 属性（结构化参数集合） | P2.5+ | 现有 Action 全为 0 参（§12.1）⇒ 无需求 |
| **D-6** | 时间戳类属性（`datetime`） | P3 | 需 ISO8601 格式化 + 与 `fw_version`（P3）同批 |
| **D-7** | 多设备发现（一个用户多台） | P4/P5 | 需按用户绑定下发**多组** ACL（`homie/5/<id1>/#` + `<id2>`） |
| **D-8** | 设备友好名（`$description.name` 来源） | P3 | **P2 不引入友好名 Config**（设备级 `name` 省略 → 客户端回落 device identity）；P3 新增 Config 字段（设备名）实现 |
| **D-9** | `sleeping` / 优雅下线（空 `$state`） | 永不（除非新增 deep-sleep 业务） | 无业务场景 |
| **D-10** | 设备 ACL 补写到**已部署**设备 | P2-5（需显式授权） | 涉及线上设备 ACL（§16.3） |
| **D-11** | Transport Facade → 独立 `mqtt_transport` 模块（方案 D） | P3+ | 属对 P1 冻结模块的重构；P2 用 Facade 等价能力 |
| **D-12** | Homie 属性 ↔ Workflow 的联动 | P3+ | Workflow 属业务协议（AD-9） |
| **D-13** | Homie **`$target`** | P3+ | 当前 `water/valve` 为**简单离散状态控制**，property state **直接来自 System State**，**无独立 target-state / 异步 target contract 需求**；待 **Motor / 长时间异步动作 / position-target-actual** 出现后再重新评估 |

> **FROZEN（AD-20a）**：**P2 不实现 `$target`**（D-13）。全文**不得**出现"P2 实现 `$target`"的隐含描述；`$target` 一律按 DEFERRED 处理。

---

## 21. Risks

| # | 风险 | 等级 | 缓解 |
|---|---|---|---|
| R-1 | **T-3（RX topic 分派）改动业务入站契约** | **高** | 逐字保持业务分支不变；双构型编译 + 全量契约回归；**不通过 ⇒ STOP** |
| R-2 | `$description` 超尺寸 | 中 | 硬上限 4096 + 生成期检查 + 停在 `init`（不发布坏描述） |
| R-3 | 属性抖动导致 MQTT 刷屏 | 中 | 死区 + 最小间隔（§17.4） |
| R-4 | Homie topics 与 ACL 白名单冲突（F-29） | 中 | §16 显式 allow；P2-5 前 Homie 功能**保持编译期关闭** |
| R-5 | `retain=1` 陈旧 `set` 被投递 | 中 | 强制忽略（AD-14a） |
| R-6 | 多 Action 合并成 1 属性后的**状态↔枚举映射**不一致 | 中 | **显式映射表**（§12.5 `cmds[]` / `enum_state_map[]`）；P2-7 步骤 3/7 双向验证 |
| R-7 | `$description.version` 被客户端当"排序"用 | 低 | 文档明示（§11.3）+ 用"不等"判断 |
| R-8 | 在 esp-mqtt 任务上下文执行 Homie 逻辑 ⇒ 与业务 RX 抢全局分片缓冲 | **高** | Facade 强制**得经队列进 loop**（AD-5a） |
| R-9 | 生产固件（`GF_HOMIE_ENABLED=0`）被误开启 | 中 | 编译期宏默认 0 + 与 P1-7 同款"生产构建"纪律 |
| R-10 | OLED 与 Homie 对同一 State 的语义理解不一致 | 低 | 共用 Device Model（AD-12） |
| R-11 | `weight.enable=false` 时投影出"不可用属性" | 中 | 投影时按 **boot-time Registry availability filter** 过滤（§4.3 / §12.2 / §15.3 AD-15b） |
| R-12 | 单客户端假设被未来破坏（有人再建 client） | 中 | Facade 为唯一入口 + 代码审查清单 |

---

## 22. P2-1 ~ P2-8 Dependency Graph

```
                 ┌────────────────────────────────────────────┐
                 │ P2-0 / P2-0.1 ← 本文件（DESIGN FREEZE，已完成）│
                 └───────────────────┬────────────────────────┘
                                     │
        ┌────────────────────────────┼──────────────────────────────┐
        ▼                            ▼                              ▼
┌───────────────┐          ┌──────────────────┐          ┌────────────────────┐
│ P2-1          │          │ P2-2             │          │ P2-5               │
│ Device Model  │          │ Transport Facade │          │ EMQX ACL +         │
│ (homie/       │          │ (cloud/          │          │ credential.js 模板 │
│  device_model)│          │  mqtt_transport) │          │ （可并行）          │
└───────┬───────┘          └────────┬─────────┘          └─────────┬──────────┘
        │                           │                              │
        └──────────┬────────────────┘                              │
                   ▼                                               │
        ┌──────────────────────────┐                               │
        │ P2-3                     │                               │
        │ Homie Bridge 骨架        │                               │
        │ ($state/$description/    │                               │
        │  属性发布)                │                               │
        └──────────┬───────────────┘                               │
                   ▼                                               │
        ┌──────────────────────────┐                               │
        │ P2-4                     │                               │
        │ set → CommandManager     │                               │
        │ + pending/结果收尾        │                               │
        └──────────┬───────────────┘                               │
                   ▼                                               │
        ┌──────────────────────────┐                               │
        │ P2-6 资源实测 / 预算核对   │                               │
        └──────────┬───────────────┘                               │
                   ▼                                               │
        ┌──────────────────────────┐                               │
        │ P2-7 垂直切片 E2E（§19）  │◄──────────────────────────────┘
        │  = 三级成立判据（Step 1~10）│
        └──────────┬───────────────┘
                   ▼
        ┌──────────────────────────┐
        │ P2-8 收口评审 + 文档同步   │
        └──────────────────────────┘

并行（不阻塞主线）：W-1 OLED UI（消费 Device Model，只读）· W-2 米家解码 · S-1 安全审计规范
```

### P2-1…P2-8 readiness（各自开工前置）
| 阶段 | 交付 | 前置 | Readiness（P2-0.1 后） |
|---|---|---|---|
| **P2-1** | `homie/device_model.{h,cpp}` + 投影表 | 本文 §5/§12 | **READY** |
| **P2-2** | `cloud/mqtt_transport.{h,cpp}` + CloudManager 5 处加性改动（含 LWT） | §6.4（**依赖方向**）/§7.1 | **READY**（T-3 是唯一高风险点） |
| **P2-3** | `homie/homie_bridge.{h,cpp}`（`$state`/`$description`/属性发布 + `homie_task()` 入 loop） | P2-1, P2-2；§15.3（fingerprint） | **READY** |
| **P2-4** | `set` → CommandManager + pending 表 + 结果收尾 | P2-3；§12.5（显式映射） | **READY** |
| **P2-5** | EMQX ACL 模板 + `credential.js` 模板 + 补写流程 | §16（含 §16.4 职责边界） | **READY**（补写线上设备需授权，D-10） |
| **P2-6** | 资源实测（size/nm/栈/PSRAM/CPU） | P2-1…P2-4 | **READY** |
| **P2-7** | 垂直切片 E2E（§19）；**三级成立判据 = Step 1~10 全 PASS** | P2-1…P2-6 | **READY** |
| **P2-8** | 收口评审 + `readme.md`/`docs/README.md`/`HANDOFF.md`/`cloud_protocol.md` §Homie 章节 | P2-7 | **READY** |

---

## 23. Architecture Freeze Checklist

> **26 项** = **23 FROZEN** + **3 DEFERRED**（D-4 / D-8 / D-13）。P2-0.1 修订新增 #3 / #6 / #13 / #16 / #17 / #20 及 DEFERRED 三行，并细化 #2 / #14 / #18。**P2-0.2 修订仅更新 #19 的 Flash 数字（`≤ 14 KiB`，见 §17.2.1），不新增/删除任何 checklist 项。**

| # | 冻结项 | 决策 | 状态 |
|---|---|---|---|
| 1 | 是否建立 Device Model | **YES**（AD-1） | **FROZEN** |
| 2 | Device Model ↔ Capability Registry 关系（**模块依赖**） | **兄弟模块 · 互不依赖 · 互不 include · 非父子 · 非 1:1**；Homie **禁用 `stable_id`**（AD-1a/1b/1d/§4.3） | **FROZEN** |
| 3 | Homie Projection **输入**（**运行期**） | 同时读 **Device Model（语义定义）** + **Capability Registry（runtime 可用性过滤）**（AD-1c/1d/§4.3/§12.2） | **FROZEN** |
| 4 | Device Model 存储与开销 | `const` 表 → Flash；**DRAM 静态 +0**（AD-2/AD-2a） | **FROZEN** |
| 5 | Homie Projection 边界 | 新增 `src/homie/`，与 `cloud/` 并列；白名单依赖（AD-4/§6.3） | **FROZEN** |
| 6 | **Transport Facade 依赖方向** | **单向**：`homie_bridge → mqtt_transport → cloud_manager → esp_mqtt_client`；CloudManager **不** include `homie_bridge.h`；Homie **不**持有 `esp_mqtt_client*`（§6.4） | **FROZEN** |
| 7 | CommandManager 接入方式 | 复用 `execute_action`；`source="homie"`（AD-6） | **FROZEN** |
| 8 | Homie 是否绕过 CommandManager | **绝对禁止**（§6.2/§7.2） | **FROZEN** |
| 9 | MQTT 客户端归属 | **单客户端（CloudManager）**；Homie 用 Facade（AD-5） | **FROZEN** |
| 10 | Homie base topic | **`homie/5/<device_id>/`**，与业务 topic 并存（AD-8） | **FROZEN** |
| 11 | `$state` 语义 | 6 态取 4；**`ready` = 五条件合取**（§10.2） | **FROZEN** |
| 12 | `$description` 语义 | 仅 boot `init` 生成；`version`=fnv(desc/action/trigger)；≤4096 B（AD-10/§11） | **FROZEN** |
| 13 | **`$description` ↔ Registry rebuild** | **boot 生成时即确定 projection contract**；rebuild **不重生成 / 不重发布 / 不改 contract**；仅 fingerprint 陈旧哨兵 `WARN`（AD-15a/§15.2/§15.3） | **FROZEN** |
| 14 | `/set` 语义 | `<node>/<prop>/set` **裸值**；**强制忽略 `retain=1`**（§8.3/§14 AD-14a） | **FROZEN** |
| 15 | `retained` | `$state`/`$description` **规范强制** retained；命令型属性 **retained=false**（工程）（§14） | **FROZEN** |
| 16 | **QoS profile 定性** | 全 **QoS1 = Guo Feeder transport profile**，**非** Homie 规范要求（AD-3a…3d/§3.4） | **FROZEN** |
| 17 | **LWT** | topic=`$state`、payload=`lost`、**retain=true（规范）**；**QoS=1（工程）**（§10.3） | **FROZEN** |
| 18 | ACL 基础模型 + **职责边界** | 子树内允许、**跨设备通配禁止**；ACL **仅 namespace 隔离**，**不承担 per-property capability authorization**（AD-11/§16.2/§16.4） | **FROZEN** |
| 19 | 资源策略 | DRAM 静态 +0；PSRAM 4 KB 单缓冲；变化驱动 + 死区 + 限流（§17）；**Flash 合计增量上限 `≤ 14 KiB`（P2-0.2 修订，§17.2.1）** | **FROZEN** |
| 20 | **P2-7 三级成立判据** | **Step 7 = semantic/control-loop proven**；**Step 10 = security/identity/isolation proven**；**Overall PASS = Step 1~10 全 PASS**（§19.4） | **FROZEN** |
| 21 | OLED / Web / Android | **共用 Device Model**；远端经 Homie 间接共享（AD-12） | **FROZEN** |
| 22 | Workflow 是否投影 | **否**（AD-9） | **FROZEN** |
| 23 | Homie 实现开关 | 编译期 `GF_HOMIE_ENABLED`（**默认 0**，生产零代价） | **FROZEN** |
| 24 | runtime `$description` refresh | 不在 P2 实现（D-4） | **DEFERRED** |
| 25 | 设备友好名（`$description.name` 来源） | 不在 P2 引入（OQ-1 → D-8，P3） | **DEFERRED** |
| 26 | Homie `$target` | 不在 P2 实现（D-13） | **DEFERRED** |

---

## 24. Open Questions

> **无 BLOCKER。** 以下 3 项**不阻塞 P2-1**（均已给出默认值/延后路径）。

| ID | 问题 | 为何现在不冻结 | 阻塞谁 | 是否需人工决策 |
|---|---|---|---|---|
| **OQ-1** | 设备友好名（`$description.name`）来源 | 需新增 Config 字段（设备名），属 P3 配置扩展面。**P2 不引入友好名 Config** | 不阻塞（**P2 省略设备级 `name`** ⇒ Homie 规范允许省略 → 客户端**回落 device identity**；`guo-feeder <device_id>` 仅文档示例，非 runtime contract） | 否（P3 再定，D-8） |
| **OQ-2** | 生产设备（`288485896ce4`）的 Homie ACL 何时补写 | 涉及线上 ACL 变更，须独立授权 | **P2-7 之前**（否则真机验证做不了） | **是**（D-10，需显式授权） |
| **OQ-3** | `weight` 类数值属性的死区最终值 | 需真机 HX711 数据（当前**未安装**） | 不阻塞（默认 1.0 g + 1000 ms） | 否（P2-6 实测后调参） |

---

## 25. Final Decision

> ## `P2-0.1 = PASS · DESIGN FROZEN · P2-1 READY`
>
> 1. **建立 Device Model**（AD-1），与 Capability Registry 为**兄弟模块** —— **互不依赖 · 非父子 · 非 1:1**（AD-1c/1d/§4.3）。
> 2. **Homie Projection 独立成模块**（`src/homie/`），**不放进 CloudManager**、**不直连硬件/Action**（§6）。
> 3. **MQTT 单客户端不变**，CloudManager 增开 **Transport Facade**（含 LWT + retain + topic 分派）—— 这是 P2 唯一的"触碰 P1 冻结模块"处，且**纯加性**，附 STOP 条件（§7.1）。**依赖方向单向冻结**（§6.4）。
> 4. **`homie/5/<device_id>/`** 与 `guo_feeder/<device_id>/` **永久并存**（§8）。
> 5. **写路径**：`set → Device Model 解析 → 协议层校验 → CommandMessage(source="homie") → CommandManager`（§7.2）。
> 6. **读路径**：`System State（唯一事实源）→ 变化检测 → retained 属性`（§17.4）。
> 7. **ACL**：子树内允许、跨设备通配禁止；**ACL 只做 namespace 隔离**（§16）。
> 8. **QoS1 = Guo Feeder transport profile**（**非** Homie 规范要求）；规范语义与工程 profile 严格分离（§3.4）。
> 9. **`$description` ↔ Registry rebuild**：projection contract 于 **boot 生成时确定**；rebuild 不改 contract，仅 fingerprint 陈旧哨兵 `WARN`（§15.3）。
> 10. **P2-7 三级成立判据**：Step 7 = semantic/control-loop proven；Step 10 = security/identity/isolation proven；**Overall PASS = Step 1~10 全 PASS**（§19.4）。
>
> **P2-1 可以立即开工，无需再讨论核心架构。**

---

## 附录 A — Architecture Diagram（模块级）

```
┌──────────────────────────── ESP32-S3 固件（src/）────────────────────────────┐
│                                                                              │
│  app/            services/                      automation/                  │
│  ┌──────────┐    ┌──────────────┐  ┌──────────┐ ┌──────────────────┐         │
│  │ valve    │    │ system_state │  │ command_ │ │ workflow         │         │
│  │ weight   │───►│ (唯一事实源)  │◄─│ manager  │◄│ workflow_storage │         │
│  │ thermo   │    │ event_manager│  │          │ │ capability_      │         │
│  │ oled ◄───┼─┐  │ config_mgr   │  └────▲─────┘ │  registry        │         │
│  └──────────┘ │  │ device_id    │       │       └──────────────────┘         │
│               │  └──────▲───────┘       │                                    │
│               │         │               │                                    │
│               │  ★ homie/（新增，与 cloud/ 并列）                              │
│               │  ┌──────────────────────────────────────────┐                │
│               │  │ device_model.{h,cpp}  ← 协议无关语义模型   │                │
│               │  │      ▲                                   │                │
│               │  │      │ 被消费                             │                │
│               └──┼── homie_bridge.{h,cpp}   ← 投影层          │                │
│                  │      │                    │              │                │
│                  └──────┼────────────────────┼──────────────┘                │
│                         │ 读 state_get_*    │ 写 command_manager_execute()   │
│                         ▼                   ▼                                │
│  cloud/          ┌───────────────────────────────────────────┐               │
│                  │ ★ mqtt_transport.{h,cpp}（Transport Facade）│               │
│                  │      ▲ publish/subscribe/foreign-handler    │               │
│                  │      │                                     │               │
│                  │  cloud_manager.cpp  ← **唯一 esp_mqtt_client** │           │
│                  │   · 业务 topic guo_feeder/<id>/{down,up,log} │              │
│                  │   · ★ LWT homie/5/<id>/$state = lost         │              │
│                  │   · ★ 第二订阅 homie/5/<id>/+/+/set          │              │
│                  └───────────────────────────────────────────┘               │
└──────────────────────────────────────────────────────────────────────────────┘
                              │ 8883 (mqt  ts)
                              ▼
                      ┌───────────────┐        8084 (wss)
                      │ EMQX  ────────┼──────────────► Web / Android（Homie 客户端）
                      │ (ACL 白名单)   │
                      └───────┬───────┘
                              │ 数据集成（业务 up/log，**不含 Homie**）
                              ▼
                      Cloudflare Worker ──► D1
```

## 附录 B — Data Flow（三条独立链路）

```
① 业务控制（既有，P2 零改动）
   Worker/脚本 → EMQX → guo_feeder/<id>/down → cloud_manager.cloud_process_rx_message
              → cloud_translate_command → command_manager_execute → Workflow/Action → 硬件

② Homie 控制（新增）
   Homie 客户端 → EMQX → homie/5/<id>/+/+/set → [mqtt_transport foreign handler]
              → 队列 → loop: homie_task → Device Model 解析/校验
              → command_manager_execute(source="homie") → Workflow/Action → 硬件
              → System State 变化 → homie_task 变化检测 → homie/5/<id>/<node>/<prop>

③ Homie 状态/发现（新增，只读）
   Device Model + Registry（boot 一次）
              → homie_bridge 生成 $description → EMQX（retained）
              → 客户端订阅 homie/5/<id>/# → 自动发现 + 渲染
```
> **关键隔离**：**Homie 流量不进 Worker**（§6.2 规划已冻结）；**业务流量不进 Homie 路径**。

## 附录 C — Protocol Tables（报文契约）

### C-1 `$description` 字段（设备级）
| 字段 | 类型 | 必需 | P2 取值 |
|---|---|---|---|
| `homie` | string | ✅ | `"5.0"` |
| `version` | integer | ✅ | fnv1a32(...)（§11.3） |
| `name` | string | ❌ | **P2 省略**（客户端回落 device identity）；`"guo-feeder <device_id>"` **仅为文档示例**（OQ-1/D-8） |
| `type` | string | ❌ | `"guo-feeder/device"` |
| `nodes` | object | ❌ | `{ <nodeId>: {name,type,properties} }` |
| `extensions` | array | ❌ | **省略** |
| `root`/`parent`/`children` | — | — | **省略** |

### C-2 Node 对象
| 字段 | 类型 | 必需 |
|---|---|---|
| `name` | string | ❌ |
| `type` | string | ❌ |
| `properties` | object | ✅（含至少 1 个属性时） |

### C-3 Property 对象
| 字段 | 类型 | 必需 | 默认 | P2 |
|---|---|---|---|---|
| `datatype` | string | ✅ | — | boolean/integer/float/string/enum |
| `name` | string | ❌ | — | 有 |
| `format` | string | 视类型 | — | enum 成员表 / 数值范围 |
| `settable` | boolean | ❌ | `false` | 有 |
| `retained` | boolean | ❌ | `true` | 有 |
| `unit` | string | ❌ | — | 有（可空 ⇒ 省略） |
| `$target` | — | — | — | **不用**（D-13） |
| `json` 等 | — | — | — | **不用**（D-5） |

## 附录 D — Topic Matrix（见 §8.4 表 · 摘要）
```
homie/5/<id>/$state             ✅ retained QoS1   写=设备
homie/5/<id>/$description       ✅ retained QoS1   写=设备
homie/5/<id>/<node>/<prop>      ✅ 见 property 的 retained
homie/5/<id>/<node>/<prop>/set  ✅ 设备订阅（忽略 retain=1）
homie/5/<id>/$log/{level}       ❌ DEFERRED
homie/5/<id>/$alert/{id}        ❌ DEFERRED
homie/5/<id>/$homie|$name|$nodes ❌ Homie 5 不存在（v4 遗留）
```
> **注**：本摘要 "retained" 对 `$state`/`$description` 属**规范强制**；**"QoS1" 属 Guo Feeder transport profile（非规范要求）**（§3.4 / §14 AD-14b）。

## 附录 E — ACL Matrix（见 §16.2 表 · 摘要）
```
设备 dev_<id> : pub guo_feeder/<id>/{up,log} | sub guo_feeder/<id>/down        （既有）
设备 dev_<id> : pub homie/5/<id>/# | sub homie/5/<id>/+/+/set                 （P2 新增）
UI  app_slot_<n> : sub homie/5/<id>/# | pub homie/5/<id>/+/+/set              （P2-5 登记）
兜底: # → deny（白名单语义，不动）
禁止: homie/5/+/#  homie/5/#  homie/#
```
> **注**：ACL **仅做 namespace / device isolation**，**不**承担 per-property capability authorization（§16.4）。
