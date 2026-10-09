# P2-7 Software Readiness Report（无硬件条件下可执行的 P2-7 验证）

> **性质**：**软件侧 readiness 报告**。本轮**未连接任何硬件**（无 Valve、无 HX711、无外部传感器、无第二台 ESP32-S3）。
> **代码基线**：固件 `c1f525c`（branch `wb`）· Cloudflare `392f181` · EMQX `eec9444`
> **上位冻结文档**：`P2-0-architecture-review-and-design-freeze.md` **§19**（P2-7 Vertical Slice, FROZEN）· §16（ACL）· §17.2.1（预算）
> **前置文档**：`P2-7-Hardware-Test-Readiness.md`（清单）· `P2-6-Resource-Acceptance-Closure.md`（资源验收）· `P2-5-Homie-ACL.md`
>
> **⚠️ 术语铁律（全文适用）**
> `PASS` · `FAIL` · `NOT RUN` · `HUMAN_REQUIRED` · `BLOCKED`
> **`HUMAN_REQUIRED` ≠ `FAIL`**。本报告**不含**任何 mock 成硬件测试的结论；凡需真实阀/秤/第二设备/broker 行为者，
> 一律记为 **`NOT RUN`** 并说明原因。**本报告不改变任何测试标准，也不构成 P2-7 的最终验收**。

---

## 0. 结论摘要（先读这段）

| 项 | 结果 |
|---|---|
| 固件构建（三构型，clean build） | **PASS**（`[SUCCESS]`，0 error，2 条历史 warning） |
| 资源回归（vs P2-6 baseline） | **PASS**（逐段、逐字节**完全一致**，零回归） |
| P2-7 candidate 构型 Flash 增量 | **+12,548 B ≤ 14,336 B** ⇒ **PASS**（headroom **1,788 B**） |
| 静态契约检查（GPIO / 配置 / ACL↔topic） | **PASS**（含 2 项**需人工复核**的分歧注记） |
| 自动化回归（9 类套件） | **全绿**（累计 **728 项断言 PASS / 0 FAIL**，另有 7 项**已登记的** legacy EXPECTED_FAILURE） |
| MQTT/Homie 软件侧制品 | **PASS**（topic / `$description` / ID / set parser 全部实测） |
| **真机 / 真 broker 行为** | **NOT RUN**（`HUMAN_REQUIRED`，见 §2） |
| **P2-7 Overall** | **`READY — WAITING FOR HUMAN HARDWARE SETUP`** |

**一句话**：**所有不依赖硬件的 P2-7 验证已全部执行且通过；P2-7 剩余部分是 100% 的实物 / 真 broker 环节。**

---

## 1. A. 已验证 PASS 项（可在当前环境自动执行的全部内容）

### 1.1 固件构建验证

**构建方式**：`PLATFORMIO_BUILD_DIR=.pio/build/<name>`（独立 clean build，未复用任何既有产物）·
`PLATFORMIO_BUILD_FLAGS` 传入构型宏（未改 `platformio.ini`、未改源码）。

| 代号 | 构型 | 构建目录 | `[SUCCESS]` | `error:` | `warning:` |
|---|---|---|---|:---:|:---:|
| **A** | default（Homie disabled） | `.pio/build/p27swoff` | ✅ | **0** | 2 |
| **B** | `-DGF_HOMIE_ENABLED=1`（**P2-7 candidate**） | `.pio/build/p27swon` | ✅ | **0** | 2 |
| **C** | `-DGF_ENV_PRODUCTION=1 -DGF_HOMIE_ENABLED=1` | `.pio/build/p27swprod` | ✅ | **0** | 2 |

**无新增 warning/error**：2 条 warning 在 A/B/C 三构型**完全相同**，且均来自 **P2 之前的提交**：

| warning | 位置 | 引入提交 |
|---|---|---|
| `'DynamicJsonDocument' is deprecated` | `src/cloud/cloud_manager.cpp:1461` | `f5b5414`（src 分层重构） |
| `'containsKey' is deprecated` | `src/main.cpp:2190` | `ca168cf`（workflow variant） |

⇒ 三构型共用同一 warning 集合，**Homie 引入 0 条新 warning**。

> **注（沙箱现象，非构建失败）**：A 构型的日志尾部有 `[safe-delete] SAFE_DELETE_BULK_CONFIRM_REQUIRED`
> 与 `SystemExit: 1` —— 这是 SCons **进程退出时（atexit）** 清理临时文件撞上沙箱批量删除保护所致，
> **发生在 `Successfully created esp32s3 image` 之后**。A 的判据以产物为准（0 error + `.elf/.bin` 时间戳新）。

### 1.2 资源回归（与 P2-6 baseline 对比）

**base** 取 P2-6 的 `623da59`：worktree `/d/Guo_Feeder_Project/.p26base`（`git diff 623da59 -- src/ platformio.ini partitions.csv` = 空，
`git status` clean）⇒ base 与冻结基线**逐位同一**。

| 段 / 度量 | **base** `623da59` | **A** | **B（candidate）** | **C** |
|---|---:|---:|---:|---:|
| `.flash.text` | 1,021,171 | 1,022,443 | **1,030,435** | 1,030,663 |
| `.flash.rodata` | 260,380 | 260,824 | **263,664** | 263,860 |
| `.flash.rodata_noload` | 16,009 | 16,009 | 16,009 | 16,009 |
| `.flash.appdesc` | 256 | 256 | 256 | 256 |
| `.dram0.data` | 24,072 | 24,072 | **24,088** | 24,088 |
| `.dram0.bss` | 107,072 | 108,632 | **109,520** | 109,520 |
| `.iram0.text` | 82,311 | 82,311 | **82,311** | 82,311 |
| `.ext_ram.*`（静态 PSRAM） | 不存在 | 不存在 | 不存在 | 不存在 |
| `firmware.bin` | 1,389,328 | 1,391,040 | **1,401,888** | 1,402,320 |

| 增量（相对 base） | A − base | **B − base** | C − base |
|---|---:|---:|---:|
| `.flash.text` | +1,272 | **+9,264** | +9,492 |
| `.flash.rodata` | +444 | **+3,284** | +3,480 |
| **Flash 合计（唯一 ceiling）** | +1,716 | **+12,548** | +12,972 |
| 裁决（`≤ 14,336 B`） | PASS | **PASS** | PASS |
| headroom | 12,620 | **1,788** | 1,364 |
| `.dram0.bss` | +1,560 | **+2,448** | +2,448 |
| `.iram0.text` | 0 | **0** | 0 |

**★ 零资源回归（最硬判据）**：本轮三个**独立 clean build**（A/B/C）的**每一个段、每一个 `size()` 总量、
`firmware.bin` 字节数**，与 P2-6 记录的 `p27{off,on,prod}` **逐位完全一致**：

```
A: text 1,022,443 / rodata 260,824 / bss 108,632 / iram 82,311 / bin 1,391,040   ← 与 P2-6 A 相同
B: text 1,030,435 / rodata 263,664 / bss 109,520 / iram 82,311 / bin 1,401,888   ← 与 P2-6 B 相同
C: text 1,030,663 / rodata 263,860 / bss 109,520 / iram 82,311 / bin 1,402,320   ← 与 P2-6 C 相同
```

⇒ **代码与构建链路完全可复现 ⇒ 相对 P2-6 资源增量 = 0（无回归）**。

**其他资源项复测**：

| 项 | 冻结要求 | 实测 | 裁决 |
|---|---|---|---|
| `homie_task()` 栈帧 | ≤ 512 B | **240 B**（`entry a1, 240`，符号 `_Z10homie_taskv`） | **PASS** |
| 静态 PSRAM | 禁 `EXT_RAM_BSS_ATTR` | 三构型 **无 `.ext_ram.*`** | **PASS** |
| IRAM 增量 | — | `.iram0.text` 82,311 **三构型 +0** | **PASS** |

### 1.3 P2-7 静态契约检查

#### 1.3.1 GPIO 定义 ↔ firmware / config

| 功能 | 引脚（仓库 `data/config/`） | 固件读取点 | 代码默认值 | 一致 |
|---|:---:|---|:---:|:---:|
| **Valve 输出** | **GPIO 12**（`valve.json:gpio_pin`） | `app/valve.cpp:246` `config_get_valve_gpio_pin()` → `gpio_set_level()` | **−1**（未配置 ⇒ 模块禁用） | ✅ |
| Valve 有效电平 | `active_level = 1`（高有效） | `valve.cpp:247` | 1 | ✅ |
| **HX711 DT** | **GPIO 6**（`weight.json:dt`） | `app/weight.cpp:735` `config_get_weight_dt()` | **4** ⚠️ | ✅（见注 ①） |
| **HX711 SCK** | **GPIO 7**（`weight.json:sck`） | `app/weight.cpp:736` `config_get_weight_sck()` | **5** ⚠️ | ✅（见注 ①） |
| OLED SDA / SCL | GPIO 4 / 5（`oled.json`） | `app/oled.cpp:90-91` | — | ✅ |
| RTC SDA / SCL | GPIO 4 / 5（`rtc.json`，`enable=false`） | `services/time_manager.cpp` | — | ✅（与 OLED 共用 I2C，正常） |
| Computer Reset | **GPIO 8** | `app/computer_reset.h:27` `COMPUTER_RESET_PIN`（全工程唯一定义点） | 8 | ✅ |

> **注 ①（静态风险，非阻塞）**：`config_manager.cpp:3625/3630` 的 weight 默认值是 **`dt=4` / `sck=5`**，与
> OLED I2C（4/5）**冲突**。仓库 `data/config/weight.json` 显式写 **6/7** 规避了该冲突；但**若设备端
> `weight.json` 缺这两个键**，将回落到 4/5 而与 OLED 抢引脚。当前 `weight.enable=false` ⇒ 不构成实际冲突。
> **建议**：P2-7 真机前用 `config_query module=weight` 确认设备端实值为 6/7。

**全工程 GPIO 使用点扫描**：`pinMode()/digitalWrite()` 仅出现在 `app/computer_reset.cpp`（GPIO 8）；
Valve 走 `gpio_config()` + `gpio_set_level()`；HX711/OLED 由库内驱动。**无第三方硬编码引脚冲突**。

> ⚠️ **硬件接线铁律（复述，不属软件验证）**：**严禁 GPIO 直驱电磁阀**（浪涌 + 反电动势会损坏 SoC），
> 必须经 MOSFET / 继电器 + 独立电源 + **共地** + 续流二极管。**此条只能由人工在实物上保证。**

#### 1.3.2 配置项检查

| 配置项 | 仓库值 | 代码默认 | 设备端历史已知值 | 结论 |
|---|---|---|---|---|
| **valve safety timeout** | `safety_timeout_sec = 300` | **300**（合法区间 10–300） | **2**（历史值） | ⚠️ **需人工 `config_query` 复核**（见 §1.3.3 / §3） |
| **weight enable（默认状态）** | `enable = false` | **`true`** ⚠️ | **键缺失 ⇒ 回落 true** | ⚠️ **仓库 / 代码 / 设备端三方不一致**（见注 ②） |
| **Homie enable** | ❌ 无配置文件 | 编译期宏 `GF_HOMIE_ENABLED` **默认 0** | — | ✅ **仅编译期可切**，`kModuleNames` 无 `homie`、`version.json` 无 `homie` |
| **MQTT topic 配置** | `client_id=dev_<device_id>`·`subscribe/publish/log_topic=guo_feeder/<device_id>/…`（**全部含占位符**） | 同上模板常量 | 历史曾回滚为 legacy 共享 topic | ✅ 模板正确（生产门禁 P1-7 的前提） |
| **mqtt_keep_alive** | `60` | **60** | **120**（生效值） | ⚠️ **已知死配置**（`config_set/save` 对它无效，超 P2 C 边界，详见 `MEMORY`）；**不影响门禁判定** |
| **Homie topic** | 无配置项（由 `homie/5/` + `device_id` 派生） | — | — | ✅ 单点定义于 `homie_projection.h:186` |

> **注 ②（`weight.enable` 三方不一致）**：`data/config/weight.json` = `false`（正确，因未装 HX711）·
> `config_manager.cpp:3620` 缺键默认 = **`true`** · 设备端**无该键** ⇒ 实际按 `true` 加载。
> 后果：设备会尝试初始化未接的 HX711（`event_push(EVENT_WEIGHT_ERROR)`，并**不阻塞**）。属**已知缺陷点**，
> **不改代码**；P2-7 真机前须由人工按 §3 写入 `weight.enable=false`。
> 另注：`config_get_weight_enable()` 的代码默认值 `true` 与 P2-6/OQ-3 的"未装秤应 false"语义存在漂移，
> **建议登记为待办**（本轮范围内不修改任何源码）。

#### 1.3.3 P2-5 ACL contract ↔ firmware topic 使用一致性

**固件实际 topic（源码取证）**：

| 方向 | topic | 来源 |
|---|---|---|
| SUB | `guo_feeder/<id>/down` | `cloud_manager.cpp:533`（`mqtt_sub_topic` = 渲染 `GF_TOPIC_TPL_DOWN`） |
| PUB | `guo_feeder/<id>/up` | `cloud_manager.cpp:2454 / 2484`（`mqtt_pub_topic`） |
| PUB | `guo_feeder/<id>/log` | `cloud_manager.cpp:2531`（`mqtt_log_topic`） |
| PUB | `homie/5/<id>/$state` | `homie_projection.h:192` `homie_state_topic()` |
| PUB | `homie/5/<id>/$description` | `homie_projection.h:200` `homie_description_topic()` |
| PUB | `homie/5/<id>/<node>/<prop>` | `homie_projection.h:209` `homie_property_topic()` |
| SUB | **`homie/5/<id>/+/+/set`** | `homie_bridge.cpp:594`（`homie_set_filter()`，**全工程唯一** Homie 订阅点） |
| LWT | `homie/5/<id>/$state` = `lost`，retain=1，qos=1 | `homie_bridge.cpp:608` |

**P2-5 ACL 冻结规则（`credential.js:116-121` ≡ `acl-rules.v3.json:homieRuleTemplate`，两路径已证收敛）**：

| # | action | permission | topic | 覆盖固件用途 |
|---|---|---|---|---|
| 1 | subscribe | allow | `guo_feeder/<id>/down` | SUB down ✅ |
| 2 | publish | allow | `guo_feeder/<id>/up` | PUB up ✅ |
| 3 | publish | allow | `guo_feeder/<id>/log` | PUB log ✅ |
| 4 | publish | allow | `homie/5/<id>/#` | $state · $description · `<node>/<prop>` · **以及 LWT** ✅ |
| 5 | subscribe | allow | `homie/5/<id>/+/+/set` | SUB set filter ✅ |

**结论**：**5/5 双向完全覆盖，无「固件使用但未被授权」或「授权但固件不使用」的 topic**。
`<id>` 段**逐字枚举**（无通配）；兜底 `deny all #` 保留不动 ⇒ 设备 publish 自己的 `/set` 属无害自环（ACL 无排除语义）。

> **★ 与前置文档的差异（重要，须登记）**：`P2-7-Hardware-Test-Readiness.md` **§5** 记载
> 「**P2-5 尚未落地** ⇒ P2-7 真机测试被阻塞」。该结论**在当前基线已过期**：
> P2-5 已于 `392f181`（CF）/ `eec9444`（EMQX）落地，且本报告 §1.5 已在真实 broker 上复测通过。
> ⇒ **`P2-7 前置依赖` 由 `BLOCKED` 变为 `UNBLOCKED`**。本轮按纪律**未修改**该文档（不在允许的改动范围内），
> **请人工在下一阶段一并更新**（见 §3 清单 #12）。

### 1.4 自动化测试回归（全部重跑，命令 / exit code / 结果）

| # | 套件 | 命令 | exit | 结果 |
|---|---|---|---|:---:|
| 1 | Device Model host（正向） | `bash test/device_model_host/run.sh` | 0 | **97 PASS / 0 FAIL**（harness OK） |
| 1b | Device Model host（**负向**可证伪探针） | 同上 | 1（如期） | **7 FAIL-as-expected** ✅ |
| 2 | MQTT Transport host | `bash test/mqtt_transport_host/run.sh` | 0 | **74 PASS / 0 FAIL** |
| 3 | Homie Projection / Bridge host | `bash test/homie_bridge_host/run.sh` | 0 | **222 PASS / 0 FAIL** |
| 4 | P2-2 transport 静态检查 | `python test/p2_2_transport_check.py` | 0 | **45/45** |
| 5 | P2-3 homie 静态检查 | `python test/p2_3_homie_check.py` | 0 | **116/116** |
| 6 | P2-4 `/set` 静态检查 | `python test/p2_4_homie_set_check.py` | 0 | **64/64** |
| 7 | P2-4 weight valid 静态检查 | `python test/p2_4_weight_valid_check.py` | 0 | **21/21** |
| 8 | P1-7 生产门禁源码级 | `python test/p1_7_prod_check.py` | 0 | **21 PASS / 0 FAIL** |
| 8b | P1-7 生产门禁二进制级 | `python test/p1_7_prod_check.py --bins` | 0 | **27 PASS / 0 FAIL** |
| 9 | Cloudflare contract 套件 | `node test/run_all.mjs` | 0 | **5 套件全 OK**（见下表） |
| 10 | EMQX ACL 契约（离线） | `node test/p2_5_acl_check.mjs` | 0 | **26/26** |
| 11 | EMQX ACL 真实 broker 授权 | `node --env-file-if-exists=.env test/p2_5_acl_broker_verify.mjs` | 0 | **23/23** |
| 12 | Homie 软件侧制品 dump（**本轮新增**） | `bash test/homie_bridge_host/dump.sh` | 0 | 全制品打印（见 §1.5） |

**Cloudflare 套件明细（`test/run_all.mjs` 聚合）**：

| 套件 | total | PASS | FAIL | EXPECTED_FAILURE | UNEXPECTED_PASS |
|---|---:|---:|---:|---:|---:|
| CURRENT O-1…O-10（presence 契约） | 83 | 83 | 0 | 0 | 0 |
| CURRENT P1-6-3-A evidence | 51 | 51 | 0 | 0 | 0 |
| CURRENT P1-6-3-D hard retirement (V-16) | 80 | 80 | 0 | 0 | 0 |
| CURRENT P1-8 N-3 redaction (R-1…R-11) | 101 | 101 | 0 | 0 | 0 |
| LEGACY P1-6-1 | 133 | 126 | 0 | **7** | 0 |

> legacy 的 7 项 `EXPECTED_FAILURE_AFTER_P1-6-3` 属**已裁决的契约变更**（旧 `timeout ⇒ offline` 语义已被
> `O-3/O-4 + source 枚举收敛`取代），**刻意保留**、不构成 FAIL，`run_all.mjs` 亦不因它们返回非零。

**累计**：**PASS 断言 728 · FAIL 0**（不含 7 项已登记的 legacy 预期失效）。

### 1.5 MQTT / Homie 软件侧可验证项目

**方法**：新增 `test/homie_bridge_host/artifacts_dump.cpp` + `dump.sh`（wasm32 + node，**无 CRT / 无硬件 / 无 broker**），
在 PC 上**真实执行**纯逻辑层并**打印制品**（既有 `run.sh` **未改动**，不影响 222 项回归）。
**边界**：以下全部是**静态生成 / 纯逻辑**证据 —— **不代表**真机发布、broker retained 或 ACL 实测。

| 项 | 实测输出 | 判定 |
|---|---|---|
| **topic renderer** | base `homie/5/a5a0000000a1/`<br>`$state` → `homie/5/a5a0000000a1/$state`<br>`$description` → `homie/5/a5a0000000a1/$description`<br>set filter → `homie/5/a5a0000000a1/+/+/set`（**不含 `#`**） | **PASS** |
| **ACL namespace 规则** | 设备仅使用 `homie/5/<own>/…`；**他机 namespace 一律拒绝**（见 set parser 行） | **PASS** |
| **Homie discovery payload 静态生成** | `desc_build_ok=1`；结构头 `{"homie":"5.0","version":2408993727,"nodes":{…`（键序 `homie,version,nodes` 正确） | **PASS** |
| **`$description` 格式与体积** | **1,907 B**（`weight.enable=true`，16 属性）· **1,618 B**（`enable=false`，13 属性）<br>均 < `HOMIE_DESC_CAP 4096`；< 目标 2048 ⇒ **MQTT 不分片** | **PASS** |
| **node / property ID 合法性** | **4 node**（`water`/`climate`/`system`/`control`）· **16 property**；`homie_id_valid()` 对**全部** node id 与 property id = **1** | **PASS** |
| **`/set` topic parser** | `homie/5/<own>/water/valve/set` ⇒ `accepted=1 node=water prop=valve`<br>`homie/5/<own>/water/weight/set` ⇒ `accepted=1 node=water prop=weight`<br>`homie/5/<他机>/water/valve/set` ⇒ **`accepted=0`**（跨设备拒绝）<br>`homie/5/<own>/water/valve`（缺 `/set`）⇒ **`accepted=0`**<br>`homie/5/<own>/$state` ⇒ **`accepted=0`** | **PASS** |
| 只读枚举映射 | `water/valve` state=1 → `"open"`；state=0 → `"close"` | **PASS** |
| 可写面命令解析 | `/set=open` → `validate=0` `cmd=VALVE_OPEN payload={}`<br>`/set=bogus` → `validate=4`(`ENUM_NOT_FOUND`) `resolve=0`<br>只读 `water/weight` 写 `"1"` → `validate=2`(`NOT_SETTABLE`) `resolve=0` | **PASS** |

**属性清单（16 项，全部 `id_valid=1`）**：

```
water/valve(enum,settable,cmds=2) · water/weight(float,RO,0:10000:1 g) · water/weight-error(bool,RO)
water/zero(bool,settable,cmds=1)
climate/temperature(RO °C) · climate/humidity(RO %) · climate/battery(RO V) · climate/scan-enabled(bool,RO)
climate/scan(enum,settable,cmds=2)
system/mqtt-connected(bool) · system/wifi-rssi(int dBm) · system/wifi-signal(enum 0..4)
system/time-valid(bool) · system/event-queue(int) · system/mqtt-retries(int)
control/computer-reset(bool,settable,cmds=1)
```

> **availability filter（对应 §19.3 failure case）**：`water/weight`、`water/weight-error` 由 `HomieGate`
> 锚定 `WEIGHT_ZERO`；`water/zero` 自身 `cmds=[WEIGHT_ZERO]` ⇒ `weight.enable=false`（⇒ `WEIGHT_ZERO` 未注册）
> 时三者**同时不进入 `$description`**（16 → 13 属性），属**预期行为**而非缺陷。

### 1.6 EMQX 真实 broker 授权实测（**非硬件**，已在真实 broker 执行）

`test/p2_5_acl_broker_verify.mjs`：以**两个自带密码的临时探针身份**（`dev_p25probea01/b01`）写入 §16.2 冻结的
5 条 ACL，用 **MQTT 5 读 reason code** 实测，**用完即删**（含 ACL 回读校验，零残留）。

| 分组 | 结果 |
|---|---|
| 探针 ACL 回读（应各 5 条，含 P1 三条） | **5 项 PASS** |
| own namespace（sub down / sub set filter / pub `$state` / pub `water/valve` / pub up / pub log） | **6 项 PASS**（全部 granted / accepted `0x0`） |
| 跨设备 / 越权（SUB `homie/5/<foreign>/#` · SUB `#` · PUB `homie/5/<foreign>/…` · PUB `guo_feeder/<foreign>/up` …） | **10 项 PASS**（全部 `Not authorized` / `0x87`） |
| 清理（DELETE rules + DELETE users，回读确认无规则组） | **2 项 PASS** |
| **合计** | **23 / 23 PASS** |

> **★ 证据分级（不得混同）**：本项 = **`fixture ACL isolation`**（独立临时凭据证明 ACL 隔离）。
> **`real second-device isolation`（第二台真实 ESP32-S3）仍为 `NOT RUN`**。
> 生产设备 `288485896ce4` **未被改动**（仅作"被拒目标"）。
> 另注：该脚本**不**验证 `homie/5/<id>/$description` 的 publish（由同一 `homie/5/<id>/#` 规则覆盖）。

---

## 2. B. NOT RUN 项（`HUMAN_REQUIRED`，本轮**未执行**）

> **原因统一为**：需要**真实物理硬件**（阀 / 秤 / 第二台设备）或**真实设备 + 真 broker 的端到端时序**。
> 本环境**无 Valve、无 HX711、无外部传感器、无第二台 ESP32-S3**，且**未连接任何真实设备**。
> 依纪律**不使用 mock** 冒充，一律记 **`NOT RUN`**。

| # | 项 | 对应 §19 Step | 为何 NOT RUN | 需要的实物/条件 |
|---|---|---|---|---|
| **N-1** | **Valve 实物动作测试**（`/set=open/close` ⇒ 电磁阀实际开/关） | Step 4–7 | 无阀、无驱动、无外部电源 | 电磁阀 + MOSFET/继电器 + 独立电源 + 共地 + 续流二极管 |
| **N-2** | **HX711 真实重量闭环**（读数与实际重量一致 / 标定正确） | 扩展 | 无 HX711、无 Load Cell、无砝码 | HX711 + Load Cell + 已知砝码（且 `weight.enable=true`） |
| **N-3** | **电磁阀安全超时断电测试**（开启超过 `safety_timeout_sec` ⇒ 强制关闭） | Step 6 衍生 | 同上；且需**把超时临时调小**以在合理时间内观测 | 阀 + 电源 + 串口；设备端 `safety_timeout_sec` 实值（已知历史为 2 s） |
| **N-4** | **retained `/set` 真机端到端**（**场景 A 实时** + **场景 B broker 重放**） | **Step 9** | 逻辑层（`event->retain` 判定）已 PASS；但**真实投递 / broker 重放**必须有真设备 + 真 broker | 真机 + MQTT 客户端（能发 retained `/set`）+ 能触发设备重连 |
| **N-5** | **Homie discovery 真机验证**（客户端订阅 `homie/5/<id>/#` 自动发现） | **Step 2–3** | 需设备真实 CONNECT 并由 broker 保留（retained）`$state`/`$description` | 真机（Homie 构型）+ MQTT 客户端 |
| **N-6** | **LWT 真机验证**（`$state = lost`） | 衍生 | LWT 是 **broker 行为**，只有真实断连（keepalive 超时 / 断电）才触发 | 真机 + broker；**绝不可用 `SIGKILL` 代替**（会得 `tcp_closed`，keepalive 计时器不参与） |
| **N-7** | **第二设备 isolation 实测** | **Step 10** | 需**第二台真实 ESP32-S3**（或等价的第二 MQTT identity）。本轮仅有 **fixture ACL isolation**（§1.6） | 第二台 ESP32-S3 + 其凭据/ACL |
| **N-8** | `$state` `init`→`ready` 真实生命周期 | Step 1 | 需真机 + 真 broker | 真机 + 2.4 GHz 可出公网 Wi-Fi |
| **N-9** | reconnect 行为 | 衍生 | 需真实断网 / 断 broker | 真机 + 可控网络 |
| **N-10** | Wi-Fi 真机联网 / MQTT 真实连接 | Step 1 | 需真机 | 真机 + 2.4 GHz AP + 设备凭据（明文只在签发帧） |
| **N-11** | `water/zero`（`WEIGHT_ZERO`）真机归零 | 扩展 | 同上 N-2 | 秤 + 砝码 |
| **N-12** | `weight-error` 断线置位 / OQ-3 死区在真实抖动下的行为 | 扩展 | 同上 N-2 | 秤 + 可控抖动 |
| **N-13** | P1 业务链路与 Homie 并存不回归 | — | 需真机 | 真机 |
| **N-14** | boot `$description` 构建 wall-clock（< 5 ms） | — | 宿主代理值不能代表真机 | 真机（示波器/日志时间戳） |
| **N-15** | 网络稳态零流量（变化驱动）抓包 | — | 需设备侧抓包 | 真机 + 抓包环境 |
| **N-16** | Mijia `climate/*` 实物读数 | — | **Not required for P2-7 core slice**；且无该传感器 | LYWSD03MMC（已激活 BLE） |

> **`NOT RUN` 不等于缺陷**：以上均为**结构性不可在本环境执行**的项，非测试失败。

---

## 3. C. 下一阶段 HUMAN_REQUIRED checklist（**连上硬件后需人工执行；本轮不执行任何一项**）

> 顺序即执行顺序。**每一步都不要求改代码**；若发现设备端配置与预期不符，按"set + save 两步"处理（`config_save` 会自动安全重启）。

### 阶段 0 —— 准备与凭据（人工决策）

| # | 动作 | 说明 |
|---|---|---|
| 0-1 | 决定 **Q-1 构型** | P2-7 candidate = **`-DGF_HOMIE_ENABLED=1`**（`.pio/build/p27swon`，已构建）；若要顺带验生产门禁则用 `-DGF_ENV_PRODUCTION=1 -DGF_HOMIE_ENABLED=1`（`.pio/build/p27swprod`）。**不要**把 `-D` 写进 `platformio.ini` |
| 0-2 | 决定 **Q-2 设备/凭据** | 建议用**测试设备**（自有 `device_id` + 新签发凭据），**勿动生产 `288485896ce4`** |
| 0-3 | 确认 **Q-3 ACL** 状态 | **已完成**（P2-5 `392f181`/`eec9444`，§1.6 已复测 23/23）。若测试设备是**新设备**，签发凭据时模板会自动带 5 条；若是**既有设备**，须按"**先读全量 → 合并 → 整体 PUT**"补写 5 条（PUT 为全量覆盖） |
| 0-4 | 准备 **Q-4 第二身份** | Step 10 需要第二台 ESP32-S3 及其凭据（否则 Step 10 仍为 NOT RUN） |
| 0-5 | 准备 MQTT 客户端（**Q-8**） | MQTT Explorer / MQTTX / mosquitto；**场景 B 须能发 retained `/set`** |

### 阶段 1 —— 接线（**硬件安全**）

| # | 动作 | 关键点 |
|---|---|---|
| 1-1 | Valve 接线 | `GPIO12 → 栅极限流电阻 → MOSFET Gate`；`ESP32 GND ↔ 外部电源 GND`（**必须共地**）；阀线圈并**续流二极管（反接）**；**严禁 GPIO 直驱** |
| 1-2 | HX711（仅扩展验证） | 3.3 V 供电；`DT=GPIO6`、`SCK=GPIO7`；量程与砝码需人工提供 |
| 1-3 | Wi-Fi 环境 | **2.4 GHz** 且可出公网（broker 为云端 8883 TLS） |

### 阶段 2 —— 烧录与设备端配置核对

| # | 动作 | 说明 |
|---|---|---|
| 2-1 | 烧录（**只写 app 分区**） | `esptool.py --chip esp32s3 --port <COM> --baud 921600 --before default_reset --after hard_reset write_flash -z 0x10000 <BD>/esp32-s3-devkitc-1/firmware.bin`（**不动** NVS `gfid`/`gfcred` 与 LittleFS；**不要用 `uploadfs`**） |
| 2-2 | 抓 boot 日志 | 期望 `[Homie] enabled base=homie/5/<id>/` · `[Valve] Init OK (pin=12, active_level=1, safety=<N> s)` · `[CFG][I] boot validated` · `WiFi connected` · `MQTT connected`；**Homie 构型**另期望 `[H] $state=ready` |
| 2-3 | **核对设备端实值**（勿信仓库 `data/`） | `config_query module=valve`（**safety_timeout_sec 历史为 2**，应为 300）· `module=weight`（**`enable` 键历史缺失 ⇒ 回落 true**，未装秤须写 `false`；并核对 `dt=6/sck=7`）· `module=mqtt`（三模板须含 `<device_id>`） |
| 2-4 | 如需改配置 | `config_set` 改 RAM → **`config_save`** 落盘（10 s 后自动安全重启）；**漏发 save 也会 5 分钟后自动补 save+重启** |

### 阶段 3 —— P2-7 垂直切片（§19 Step 1–10）

| # | Step | 人工动作 | 期望 / 判据 |
|---|---|---|---|
| 3-1 | 1 | 上电 + MQTT 连接 | 订阅 `homie/5/<id>/$state` = `init` → `ready`（retained，顺序正确） |
| 3-2 | 2 | 订阅 `homie/5/<id>/#` | `$description` 含 `water.properties.valve{datatype:enum,format:"open,close",settable:true}`（**自动发现**） |
| 3-3 | 3 | 同上 | `homie/5/<id>/water/valve` = `close`（retained 初值） |
| 3-4 | 4–5 | 客户端 publish `homie/5/<id>/water/valve/set` = `open` | 命令链路 `{object:"VALVE_OPEN", source:"homie"}` RESULT `accepted` |
| 3-5 | **6** | **观察** | **电磁阀实际打开**（物理动作）—— 见 N-1 |
| 3-6 | 7 | 订阅状态回传 | `homie/5/<id>/water/valve` = `open`（retained 更新） |
| 3-7 | 8 | publish `/set` = `bogus` | **无任何 Homie 发布**；WARN 落 LogManager |
| 3-8 | **9** | **场景 A** 实时发 retained `/set`；**场景 B** 先发 retained `/set` 再让设备重连 | **无发布 / 无硬件动作**（唯一判据 = `event->retain` 生效）—— 见 N-4 |
| 3-9 | **10** | 以设备 1 账号尝试 SUBSCRIBE `homie/5/<他机>/#` 与 PUBLISH `homie/5/<他机>/…/set` | **SUBACK 拒绝 / publish 被拒** —— 见 N-7（需第二身份） |
| 3-10 | 衍生 | 真机 LWT（真实断连 / 断电） | `homie/5/<id>/$state` = `lost`（**不得用 `SIGKILL` 代替**） |

### 阶段 4 —— 扩展验证（可选，不阻塞 Step 1–10）

| # | 动作 | 判据 |
|---|---|---|
| 4-1 | `weight.enable=true` + 已知砝码 | `water/weight` 数值与砝码一致；`water/zero` 后读数归零；`weight-error` 断线置位 |
| 4-2 | OQ-3 死区 | 真实抖动下 1.0 g / 1000 ms 行为（静态已 PASS） |
| 4-3 | 网络稳态抓包 | 无周期性空流量（变化驱动） |
| 4-4 | boot `$description` 构建耗时 | < 5 ms（真机 wall-clock） |

### 阶段 5 —— 文档与仓库收尾（人工裁决）

| # | 动作 | 说明 |
|---|---|---|
| 5-1 | **更新 `P2-7-Hardware-Test-Readiness.md` §5** | 该节的「P2-5 未落地 ⇒ 阻塞」**已过期**；应改为 `UNBLOCKED`，并指向本报告 §1.6 |
| 5-2 | 登记 `weight.enable` 三方不一致 | 仓库 `false` / 代码默认 `true` / 设备端缺键（§1.3.2 注 ②） |
| 5-3 | 登记 weight `dt/sck` 代码默认 4/5 与 OLED 冲突 | §1.3.1 注 ① |
| 5-4 | 决定是否 push | 固件 14 commit / CF 1 / EMQX 1 **均未 push**（`origin/wb` 仍停 `623da59`） |
| 5-5 | 决定是否清理 scratch | `.pio/build/p27sw*` 与 worktree `/d/Guo_Feeder_Project/.p26base`、`.p2verify` 保留为证据；`.pio/` 全为 gitignored |

---

## 4. 范围与纪律声明

**本轮新增/修改的文件（仅此二类，符合任务 §6 允许范围）**：

| 文件 | 类型 | 说明 |
|---|---|---|
| `test/homie_bridge_host/artifacts_dump.cpp` | **测试文件（新增）** | 纯逻辑层制品 dump；与既有 `hb_probe.cpp` 同一宿主测试体系 |
| `test/homie_bridge_host/dump.sh` | **测试文件（新增）** | 上述探针的运行器；**未改动** `run.sh` |
| `docs/architecture/P2-7-Software-Readiness-Report.md` | **readiness 报告（新增）** | 本文件 |

**未修改**：任何 `src/` 固件源码 · `platformio.ini` · partition · Frozen 架构文档（含 `P2-0-…freeze.md` 与
`P2-7-Hardware-Test-Readiness.md`）· ACL contract（`credential.js` / `acl-rules.v3.json`）· 既有任何测试脚本。

**未做**：伪造硬件 PASS · 用 mock 冒充真机 · 降低任何测试判据 · 修改 P2-7 冻结切片 · 进入 P2-8 · push。

**构建产物（scratch / gitignored）**：`.pio/build/p27swoff`（A）· `.pio/build/p27swon`（B）· `.pio/build/p27swprod`（C）·
`.pio/p27sw/`（日志与测量脚本）· base worktree `/d/Guo_Feeder_Project/.p26base`（`623da59`）。

---

## 5. 最终状态

```
固件构建（三构型，clean, HEAD c1f525c）        = PASS   （[SUCCESS]，0 error，2 条历史 warning，无新增）
资源回归 vs P2-6 baseline                      = PASS   （A/B/C 逐段逐字节与 P2-6 完全一致 ⇒ 增量 0）
P2-7 candidate（B = Homie-enabled）Flash 增量   = +12,548 B  ≤ 14,336 B   PASS  （headroom 1,788 B）
homie_task 栈帧 / 静态 PSRAM / IRAM 增量        = 240 B / 0 / 0            PASS
静态契约（GPIO / 配置 / ACL↔topic）             = PASS   （2 项需人工复核的分歧注记）
回归套件（9 类）                                = 全绿   （728 PASS / 0 FAIL；另有 7 项 legacy EXPECTED_FAILURE）
MQTT/Homie 软件侧制品                          = PASS   （topic / $description 1,907·1,618 B / 4 node·16 prop / set parser）
EMQX ACL 真实 broker 授权                      = PASS   （fixture ACL isolation 23/23；探针零残留）

真机 / 真 broker 端到端（N-1…N-16）             = NOT RUN （HUMAN_REQUIRED）
P2-7 前置依赖                                  = UNBLOCKED （P2-5 已落地并复测；前置文档 §5 待人工更新）
P2-7 Overall                                   = READY — WAITING FOR HUMAN HARDWARE SETUP
P2-8                                           = NOT STARTED
```
