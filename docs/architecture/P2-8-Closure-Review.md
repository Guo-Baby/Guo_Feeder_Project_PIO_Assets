# P2-8 Closure Review（P2 阶段收口评审）

> **性质**：**收口评审报告（documentation-only）**。本文件**只做评审与状态对齐**，不实现、不修复、不改代码。
> **状态**：**`P2-8 = CLOSED（评审部分）`** · **`P2 = SOFTWARE CLOSED / HARDWARE DEFERRED`** · **`P2-9 = NOT STARTED`**
> **上位冻结文档**：`P2-0-architecture-review-and-design-freeze.md`（**§19** P2-7 判据 · **§23** Freeze Checklist · **§17.2.1** Flash 预算 · **§16** ACL）
> **配套文档**：`P2-7-Software-Readiness-Report.md` · `P2-7-Hardware-Deferral-Record.md` · `P2-6-Resource-Acceptance-Closure.md` · `P2-5-Homie-ACL.md` · `P2-8-Plan.md`
>
> **⚠️ 术语铁律**：`PASS` · `FAIL` · `NOT RUN` · `HUMAN_REQUIRED` · `DEFERRED` · `CLOSED`
> **`HUMAN_REQUIRED` / `NOT RUN` / `DEFERRED` 一律 ≠ `PASS`**。本评审**不把任何硬件未测试项转换为 PASS**。

---

## 0. 结论摘要

| 维度 | 结论 |
|---|---|
| **§23 Freeze Checklist** | **26/26 已对齐**（23 FROZEN **全部合规** + 3 DEFERRED **保持在 P2 内不实现**） |
| **§19 P2-7 三级成立判据** | Step 1–5 **语义/控制链路 已由软件侧证据支撑**；Step 6/7/9/10 等实物步 = **`NOT RUN`** ⇒ **含硬件 Overall 仍 `NOT RUN`** |
| **§17.2.1 Flash 预算** | **PASS**（B − base = **+12,548 B ≤ 14,336 B**，headroom 1,788 B） |
| **ACL / topic contract** | **PASS**（5/5 双向覆盖；真实 broker 23/23；跨设备拒绝实测） |
| **未记录的代码修改** | **无**（`src/` · `platformio.ini` · `partitions.csv` 自 `6775bb3` 起零改动） |
| **P2 阶段级结论** | **`SOFTWARE CLOSED`** · **硬件 `DEFERRED`（`NOT RUN`，不阻塞）** |

> **★ 一句话**：**P2 的软件与契约部分已完整收口并通过；唯一未完成的是依赖实物/真 broker 的端到端步骤，
> 该部分已正式 `DEFERRED`（保持 `NOT RUN`），因此 P2 阶段级结论是「软件 CLOSED + 硬件 DEFERRED」，
> 而**不是**「P2 整体 PASS」。**

---

## 1. WP-1 · 基线冻结与范围核对

### 1.1 三仓基线（评审时点）

| 仓库 | HEAD | 工作树 |
|---|---|---|
| 固件 `PIO_Assets/Guo_Feeder_Project` | **`75def73`** | clean |
| 云端 `Cloudflare_Assets/guo-feeder-api` | **`392f181`** | clean |
| 消息 `EMQX_Assets` | **`eec9444`** | clean |

### 1.2 阶段状态核对（人工给定 ⇄ 仓库证据）

| 阶段 | 声明状态 | 仓库证据 | 核对 |
|---|---|---|---|
| **P2-0.2** | CLOSED | `ce948c3`（§17.2.1 Flash 预算修订为 ≤14 KiB） | ✅ |
| **P2-1** | CLOSED | `src/homie/device_model.{h,cpp}` + host 探针 | ✅ |
| **P2-2** | CLOSED | `src/cloud/mqtt_transport.{h,cpp}` + `mqtt_routing.h` + cloud_manager 加性改动 | ✅ |
| **P2-3** | CLOSED | `src/homie/homie_projection.h` + `homie_bridge.{h,cpp}` | ✅ |
| **P2-4** | CLOSED | `homie_bridge.cpp` `/set` → CommandManager + pending 表 + 结果收尾 | ✅ |
| **P2-5** | CLOSED | CF `392f181` + EMQX `eec9444` + 文档 `c1f525c` | ✅ |
| **P2-6** | CLOSED | `f924c5b`（新 ceiling 下 PASS） | ✅ |
| **P2-7** | **SOFTWARE READY / HARDWARE DEFERRED** | `9c632a8`（软件侧）+ `9afe597`（延期记录） | ✅ |

### 1.3 「不存在未记录的代码修改」核验

```
git log -1 -- src/                                    → 6775bb3 (2026-10-08, perf(homie): reduce bridge and projection flash footprint)
git diff --name-only 6775bb3 HEAD -- src/ platformio.ini partitions.csv   → 空
git diff --name-only 6775bb3 HEAD | grep -v '^docs/'  → 仅 test/homie_bridge_host/{artifacts_dump.cpp,dump.sh}（P2-7 新增测试，已记录于 9c632a8）
git status --porcelain                                → 空
```

⇒ **结论：`src/` 与构建配置自 `6775bb3` 起零改动；期间所有非文档变更仅 2 个新增测试文件，均已 commit 记录。无未记录的代码修改。**

---

## 2. §23 Architecture Freeze Checklist 逐条对齐（26/26）

> 判据口径：**已实现且与冻结条款一致 = COMPLIANT**；**冻结为「P2 不实现」且确实未实现 = DEFERRED-HELD**。

| # | 冻结项（摘要） | 实现 / 证据 | 状态 |
|---|---|---|---|
| 1 | 建立 Device Model | `src/homie/device_model.{h,cpp}`（`constexpr` 表） | **COMPLIANT** |
| 2 | Device Model ↔ Registry **兄弟模块**；**禁用 `stable_id`** | `device_model.h` 仅 include `<stdint.h>` + `services/system_state.h`；homie **不含** `capability_registry.h`（见 §8 专项） | **COMPLIANT**（附 §8 说明） |
| 3 | Projection 运行期**同时读** Device Model + Registry 可用性 | `homie_action_available()` 只读查询 Registry；`homie_property_available()` 做 boot-time filter | **COMPLIANT** |
| 4 | Device Model 存储 → Flash；**DRAM 静态 +0** | P2-6：`device_model.cpp.o` `data=0 / bss=0`；全表落 `.flash.rodata` | **COMPLIANT** |
| 5 | 新增 `src/homie/`，与 `cloud/` 并列；白名单依赖 | 目录存在（5 文件）；依赖白名单见 `homie_projection.h` 注释头 | **COMPLIANT** |
| 6 | Transport Facade **单向**依赖 | `homie_bridge → mqtt_transport → cloud_manager → esp_mqtt_client`；`cloud_manager.cpp` **0 处** `homie_bridge`；homie **0 处** `esp_mqtt_client`（仅注释说明） | **COMPLIANT** |
| 7 | CommandManager 复用 `execute_action`；`source="homie"` | `homie_bridge.cpp` include `services/command_manager.h`；`m.source = "homie"` | **COMPLIANT** |
| 8 | **禁止绕过 CommandManager** | homie 内无任何 Action/硬件直呼；唯一写路径经 CommandManager | **COMPLIANT** |
| 9 | MQTT **单客户端**（CloudManager） | 全 `src/` 仅 `cloud_manager.cpp:1803` 一处 `esp_mqtt_client_init()` | **COMPLIANT** |
| 10 | base topic = `homie/5/<device_id>/` | `homie_projection.h:186` `hp_append(out,cap,&len,"homie/5/")` | **COMPLIANT** |
| 11 | `$state` 6 态取 4；`ready` = 五条件合取 | `HOMIE_PHASE_{OFFLINE,INIT,DESC,READY}`；`homie_prereq_ready()` 注释列明 5 条件；设备不自行发 `lost`（broker LWT） | **COMPLIANT** |
| 12 | `$description` 仅 boot 生成；`version`=fnv；≤4096 B | `homie_desc_build()` 单一调用点（boot）；`HOMIE_DESC_CAP 4096`；实测 1,907 / 1,618 B | **COMPLIANT** |
| 13 | **Registry rebuild 不重生成/不重发布/不改 contract** | `homie_check_fingerprint()`：仅 `homie_log('W',"desc stale: registry changed")`，无重发布路径（文件内显式列出 4 条「✗ 不做」） | **COMPLIANT** |
| 14 | `/set` **裸值**；**强制忽略 `retain=1`** | `if(retain) { homie_log('W',"retained /set dropped"); return; }` —— 判据**只有** `retain`，无启发式替代 | **COMPLIANT** |
| 15 | `$state`/`$description` retained；命令型属性 `retained=false` | `homie_publish_state(..., retain=true)`；`homie_desc_build` 发布 retain=true；属性按 `p->retained`（`water/zero`·`climate/scan`·`control/computer-reset` = 0） | **COMPLIANT** |
| 16 | 全 **QoS1**（transport profile，非 Homie 规范） | `HOMIE_QOS 1`，单一常量 | **COMPLIANT** |
| 17 | LWT = `$state` / `lost` / retain=true / QoS1 | `mqtt_transport_set_lwt(st, "lost", HOMIE_QOS, true)`（`st` = `homie/5/<id>/$state`） | **COMPLIANT** |
| 18 | ACL：子树内允许、**跨设备通配禁止**；仅 namespace 隔离 | 5 条规则 `<device_id>` **逐字枚举**；模板层拒绝 `homie/5/+/…` / `homie/5/#` / `#`（负向 F-1…F-5） | **COMPLIANT** |
| 19 | 资源策略：DRAM +0、PSRAM 4 KB、**Flash ≤14 KiB（P2-0.2）** | 见 §4 | **COMPLIANT** |
| 20 | **P2-7 三级成立判据** | 见 §3 | **部分 NOT RUN**（见 §3） |
| 21 | OLED / Web / Android 共用 Device Model | 单一 `kProperties` 表；homie 只读投影 | **COMPLIANT** |
| 22 | **Workflow 不投影** | `kNodes` = 4（water/climate/system/control），无 workflow 节点 | **COMPLIANT** |
| 23 | 编译期开关 `GF_HOMIE_ENABLED`（**默认 0**） | `homie_bridge.cpp:53-55` `#ifndef … #define GF_HOMIE_ENABLED 0` | **COMPLIANT** |
| 24 | runtime `$description` refresh（D-4） | **未实现**（唯一 `homie_desc_build()` 在 boot） | **DEFERRED-HELD** |
| 25 | 设备友好名 / `$description.name`（D-8 → P3） | **未实现**（desc 头显式注释「设备级 name **省略**」） | **DEFERRED-HELD** |
| 26 | Homie `$target`（D-13） | **未实现**（homie 内 0 处 `$target`） | **DEFERRED-HELD** |

**小结**：**23 FROZEN 全部 COMPLIANT**（其中 #2 附透明说明，见 §8）· **3 DEFERRED 全部保持未实现**。

---

## 3. §19 P2-7 三级成立判据 —— 完成度对齐

> §19 原文：**`Overall PASS = Step 1~10 全 PASS`**；`Step 7 = semantic/control-loop proven`；`Step 10 = security/identity/isolation proven`。

| Step | 内容 | 状态 | 依据 |
|---|---|---|---|
| 1 | 上电 + MQTT 连接 ⇒ `$state` `init`→`ready` | **NOT RUN**（生命周期真机） | 相位机逻辑已 host 验证；真实时序需真机 + broker |
| 2 | 订阅 `homie/5/<id>/#` ⇒ `$description` 自动发现 | **NOT RUN**（真机） | `$description` **静态生成**已实测（1,907 B，含 `water.properties.valve{enum,open,close,settable:true}`） |
| 3 | `water/valve` = `close` retained 初值 | **NOT RUN**（真机） | 初值序列化逻辑已 host 验证（`state=0 → "close"`） |
| 4 | client publish `/set` = `open` | **NOT RUN**（真机） | set parser 已 host 验证（own namespace accepted） |
| 5 | 命令链路 `command_manager_execute({object:"VALVE_OPEN",source:"homie"})` | **软件侧 PASS / 真机 NOT RUN** | `dm_resolve_command` → `VALVE_OPEN` 已实测；CommandManager 集成 45/45 |
| 6 | **电磁阀实际打开（物理动作）** | **NOT RUN**（`HUMAN_REQUIRED`） | 无阀、无驱动、无电源 |
| 7 | `water/valve` = `open` 状态回传 | **NOT RUN**（真机） | 变化检测/序列化逻辑已 host 验证 |
| 8 | 失败路径 `/set=bogus` ⇒ 无发布 + WARN | **软件侧 PASS / 真机 NOT RUN** | `dm_validate_set("bogus")=4`、`resolve=0`；host 222 PASS |
| 9 | **retained `/set` 场景 A/B ⇒ 丢弃** | **逻辑层 PASS / 真机 NOT RUN** | `event->retain` 判定已实现并 host 验证；**真实 broker 重放 NOT RUN** |
| 10 | **越权订阅他机 `homie/5/<他机>/#` ⇒ SUBACK 拒绝** | **fixture PASS / `real second-device isolation` NOT RUN** | 真实 broker 实测 23/23（含 8 项越权/跨设备 **全部被拒**）；**第二台真实设备 NOT RUN** |

**三级判据结论**：

| 级别 | 判据 | 结论 |
|---|---|---|
| Step 1–7（semantic / control-loop） | 语义与控制链路成立 | **软件侧证据充分；实物步 `NOT RUN`** ⇒ **未判定成立** |
| Step 10（security / identity / isolation） | 安全与隔离成立 | **fixture 级 PASS（23/23）；`real second-device` `NOT RUN`** ⇒ **未判定成立** |
| **Overall** | Step 1~10 全 PASS | **`NOT RUN`**（**非 PASS，非 FAIL**） |

> **★ 纪律声明**：**本评审不把 Step 6/7/9/10 等实物步转换为 PASS。** 它们保持 `NOT RUN` / `HUMAN_REQUIRED`，
> 未来经 `P2-7-Hardware-Deferral-Record.md` §4 的 **R-1…R-6** 节点回归。

---

## 4. §17.2.1 Flash 预算对齐

| 判据 | 上限 | 实测（B − base，段级） | 裁决 |
|---|---:|---:|---|
| **Flash 合计增量** | **≤ 14,336 B**（14 KiB） | **+12,548 B** | **PASS** |
| headroom | — | **+1,788 B**（12.5% of ceiling） | — |
| `.flash.text`（**观测项，非 ceiling**） | — | +9,264 B | 记录 |
| `.flash.rodata`（**观测项，非 ceiling**） | — | +3,284 B | 记录 |

**配套资源项**（沿用 P2-6 / P2-7 实测，均未变化）：

| 项 | 冻结要求 | 实测 | 状态 |
|---|---|---|---|
| Device Model DRAM 静态 | 0 B | `data=0 / bss=0` | **PASS** |
| 静态 PSRAM（`EXT_RAM_BSS_ATTR`） | 禁止 | 三构型无 `.ext_ram.*` | **PASS** |
| PSRAM `$description` 缓冲 | 4096 B，boot 一次分配、复用 | `heap_caps_malloc(4096, MALLOC_CAP_SPIRAM)`，无 `free()` | **PASS** |
| DRAM fallback | — | 存在 `MALLOC_CAP_8BIT` 回退 | **WARNING**（如实记录） |
| `homie_task()` 栈帧 | ≤ 512 B | **240 B** | **PASS** |
| IRAM 增量 | — | `.iram0.text` 三构型 **+0** | **PASS** |
| 变化驱动 / 死区 / 限流 | 变化驱动 + 死区 + 限流 | 16 属性均含 deadband/min_pub_ms（OQ-3 = 1.0 g / 1000 ms 静态） | **PASS（静态）** |
| 真机 boot 耗时 / 稳态零流量 | — | — | **NOT RUN** |

---

## 5. ACL contract / topic contract 对齐

### 5.1 双向覆盖（5/5）

| # | ACL 规则（`credential.js` ≡ `acl-rules.v3.json`） | 固件实际用途 | 一致 |
|---|---|---|---|
| 1 | sub `guo_feeder/<id>/down` | `cloud_manager.cpp:533` 订阅 | ✅ |
| 2 | pub `guo_feeder/<id>/up` | `cloud_manager.cpp:2454/2484` 发布 | ✅ |
| 3 | pub `guo_feeder/<id>/log` | `cloud_manager.cpp:2531` 发布 | ✅ |
| 4 | pub `homie/5/<id>/#` | `$state` / `$description` / `<node>/<prop>` / **LWT** | ✅ |
| 5 | sub `homie/5/<id>/+/+/set` | `homie_bridge.cpp:594`（**全工程唯一** Homie 订阅点） | ✅ |

**无「固件使用但未授权」或「授权但不用」的 topic。**

### 5.2 隔离性证据

| 证据 | 结果 |
|---|---|
| 冻结规则集合 / 方向 / 隔离（离线契约测试） | **26 / 26 PASS** |
| **真实 EMQX broker 授权实测**（临时探针身份，用完即删） | **23 / 23 PASS**（own namespace 全 granted；跨设备/越权 **8 项全部被拒**：SUBACK `Not authorized` / PUB `0x87`） |
| 双来源一致性（`credential.js` 动态签发 ⇔ `acl-rules.v3.json` 静态模板） | **完全收敛**（同 `device_id` 产出同一规则集） |
| 生产设备 `288485896ce4` | **未被改动**（仅作"被拒目标"） |
| 探针残留 | **零**（DELETE 204 + 回读确认） |
| **`real second-device isolation`** | **NOT RUN**（需第二台真实 ESP32-S3） |

> **★ 保持 `fixture ACL isolation` 与 `real second-device isolation` 的证据分级，不混同。**

---

## 6. 状态分类总表（四态严格区分）

### 6.1 PASS（已验证）

| 项 | 依据 |
|---|---|
| 三构型构建（default / Homie / prod+Homie） | P2-7 §1.1（`[SUCCESS]`、0 error、无新增 warning） |
| 资源回归（与 P2-6 逐段逐字节一致） | P2-7 §1.2 |
| Flash 预算 §17.2.1 | §4（+12,548 ≤ 14,336） |
| 静态契约（GPIO / 配置 / ACL↔topic 5/5） | P2-7 §1.3 |
| 自动化回归 9 类（728 PASS / 0 FAIL） | P2-7 §1.4 |
| MQTT/Homie 软件侧制品 | P2-7 §1.5 |
| EMQX ACL（离线 26/26 + 真实 broker 23/23） | P2-7 §1.6 |
| §23 Freeze Checklist 23 FROZEN | §2 |
| 无未记录的代码修改 | §1.3 |

### 6.2 NOT RUN（需实物 / 真 broker；**不得转 PASS**）

Step 1 · 2 · 3 · 4 · 5（真机部分）· **6 Valve 物理动作** · 7 · 8（真机部分）· **9 retained 真机两场景** · **10 第二设备隔离** ·
`$state` 生命周期 · reconnect · Wi-Fi/MQTT 真机连接 · `weight.zero` / `weight-error` / OQ-3 真机 · boot 耗时 · 稳态抓包 · Mijia 读数（16 项，详见 `P2-7-Hardware-Deferral-Record.md` §3）。

### 6.3 HUMAN_REQUIRED（人工裁决）

| # | 项 |
|---|---|
| H-1 | 是否接受「P2 = 软件 CLOSED + 硬件 DEFERRED」的阶段级措辞 |
| H-2 | 是否 push（固件 / Cloudflare / EMQX） |
| H-3 | 是否清理 scratch（`.pio/build/p27sw*` · worktree `.p26base` / `.p2verify`） |
| H-4 | §7 两项配置分歧是否立为修复任务 |
| H-5 | 是否授权生产设备 `288485896ce4` 的 ACL 补写（PUT 全量覆盖；D-10 / OQ-2） |
| H-6 | P2-8 之后进入哪条线（App 联调 / W-1 / W-2 / P3） |

### 6.4 DEFERRED（已裁决延后，非缺陷）

| # | 项 | 去向 |
|---|---|---|
| D-A | P2-7 硬件实物测试全部 | **App 联调阶段**（`P2-7-Hardware-Deferral-Record.md` §4 R-1…R-6） |
| D-B | §23 #24 runtime `$description` refresh | 明确不在 P2（D-4） |
| D-C | §23 #25 设备友好名 | P3（D-8 / OQ-1） |
| D-D | §23 #26 Homie `$target` | 不在 P2（D-13） |

---

## 7. 遗留项登记（**只登记，不修复**）

> 本节点仅建立 TODO / Known Issues 记录。**未修改默认值、未改配置系统、未改 weight 模块**。
> 正式登记位置：`docs/issues/未修复的问题.md`（全项目问题权威清单）。

| ID | 问题 | 影响 | 本轮动作 |
|---|---|---|---|
| **P2-8-KI-1** | **weight 代码默认 `dt=4` / `sck=5` 与 OLED I2C（4/5）冲突**（`config_manager.cpp:3625/3630`）；仓库 `data/config/weight.json` 写 6/7 规避，但设备端若缺键将回落 4/5 | 未装秤时无实际影响（`weight.enable=false`）；一旦启用且缺键 ⇒ HX711 与 OLED 抢引脚 | **仅登记** |
| **P2-8-KI-2** | **`weight.enable` 三方不一致**：仓库 `false` / 代码默认 `true`（`config_manager.cpp:3620`）/ 设备端**键缺失 ⇒ 回落 `true`** | 设备会尝试初始化未接的 HX711（仅告警，不阻塞） | **仅登记** |
| P2-4-OPEN-1 | 设备侧**无法区分命令终态**（沿用） | Homie 命令型属性结果语义受限 | 沿用登记 |
| P2-3-OPEN | LogManager EventId 无冻结编号（Flash 部分已由 P2-0.2 解决） | 新增日志事件需走 P2 Log Integration 流程 | 沿用登记 |
| — | `mqtt_keep_alive` 为**死配置**（生效 120 s，`config_set/save` 无效） | 超 P2/C 边界 | 沿用登记（见 `MEMORY.md`） |
| — | ⚠️ **待办：轮换 MQTT 密码**（10 个受跟踪文件含明文） | 安全 | 沿用既有待办 |

---

## 8. 专项审查：`stable_id` 的使用（透明披露）

**背景**：§23 #2 / AD-1a 冻结「Homie **禁止**使用 `stable_id`；Homie 属性 id 一律用语义字符串」，
其理由（F-2）是 `stable_id` = runtime_id **字母序 index**，新增 Action 会**平移**已有 ID。

**审查发现**：`src/homie/homie_bridge.cpp:146-150` 存在：

```cpp
static bool homie_action_available(const char* action_id, void* ctx)
{
    (void)ctx;
    if(action_id == nullptr || action_id[0] == '\0') { return false; }
    uint8_t stable_id = 0;
    return capability_get_action_stable_id(String(action_id), stable_id);   // ← 仅作「是否已注册」探针
}
```

**裁定 = COMPLIANT**，理由：

1. 该调用的语义是**「某 `action_id` 本次 boot 是否已注册」的存在性探针**，返回值 `stable_id` **被丢弃、未参与任何 Homie 标识**；
2. Homie 的 node / property 标识**全部来自 Device Model 语义字符串**（`water/valve` 等），**未使用 `stable_id`**；
3. §23 #3 **要求** Projection 在运行期读 Registry 做可用性过滤，而 Registry **唯一的只读查询 API 就是** `capability_get_action_stable_id()`
   ⇒ 该调用是唯一合规的读法，**不违反 AD-1a 的立法本意**（禁止的是"用 stable_id 当身份"）。

> **★ 如实披露**：这是**字面上**出现了 `stable_id` 字样的一处，但**不构成合同偏离**。
> 若未来 Registry 提供 `capability_has_action(id)` 之类更直白的 API，可**顺带**替换以消除歧义（**不属 P2-8 范围，不在此实现**）。

---

## 9. 阶段级结论与边界声明

### 9.1 阶段级结论

```
P2-0.2            = CLOSED / FROZEN      （Flash ceiling ≤ 14 KiB）
P2-1 … P2-6       = CLOSED
P2-7 软件侧        = SOFTWARE READY / CLOSED
P2-7 硬件侧        = DEFERRED → NOT RUN  （HUMAN_REQUIRED；不阻塞）
P2-7 含硬件 Overall = NOT RUN            （§19 要求 Step 1~10 全 PASS ⇒ 不宣称 PASS）
P2 §23 Checklist  = 23 FROZEN 全 COMPLIANT + 3 DEFERRED 全 HELD
P2-8 评审          = CLOSED（本文件）
P2 阶段级          = SOFTWARE CLOSED / HARDWARE DEFERRED
P2-9              = NOT STARTED
```

### 9.2 本轮边界声明

- **本评审为 documentation-only**：未修改任何 `src/` 固件源码、`platformio.ini`、partition、Cloudflare / EMQX 代码与 ACL contract。
- **未修改任何冻结架构文档**（`P2-0-…freeze.md` 一字未动）。
- **未把任何硬件未测试项转换为 PASS**；`NOT RUN` / `HUMAN_REQUIRED` / `DEFERRED` 三态保持严格区分。
- **未修复 §7 的遗留项**（仅登记）。
- **未进入 P2-9**，未开发任何 P2 范围外功能。
- **未 push**。
