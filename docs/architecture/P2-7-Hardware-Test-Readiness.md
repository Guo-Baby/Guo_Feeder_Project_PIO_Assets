# P2-7 Hardware Test Readiness（实物测试准备清单）

> **性质**：**准备清单（documentation-only）**。本文件**不代表任何实物测试已完成**。
> **状态**：`P2-6 = CLOSED`（见 `P2-6-Resource-Acceptance-Closure.md`）· **`P2-7 = READY — WAITING FOR HUMAN HARDWARE SETUP`**
> **上位冻结文档**：`P2-0-architecture-review-and-design-freeze.md` **§19**（P2-7 Vertical Slice，FROZEN）· §16（ACL）· §17.2.1（预算）
> **代码基线**：`6775bb3`（src/ 与 HEAD 一致）
>
> **⚠️ 术语铁律（全文适用）**
> `PASS` · `FAIL` · `NOT RUN` · `HUMAN_REQUIRED` · `BLOCKED`
> **`HUMAN_REQUIRED` ≠ `FAIL`**。Agent **不得**因缺少物理硬件而 mock 成真实硬件测试、不得把 host test 写成 hardware PASS、不得声称 Valve 已动作 / HX711 已产生真实重量 / broker 已重放 retained / LWT 已被真实客户端观察 / cross-device ACL 已验证。

---

## 1. P2-7 测试对象与判据（§19 摘要，FROZEN）

**测试对象 = `water/valve`**（兼具可写 enum→2 Action 与可读 System State，一条路径覆盖全部关键机制）。

| # | 动作 | 预期 | 判据 |
|---|---|---|---|
| 1 | 上电 + MQTT 连接 | `homie/5/<id>/$state` = `init`→`ready` | retained；顺序正确 |
| 2 | 订阅 `homie/5/<id>/#` | `$description` 含 `water.properties.valve{datatype:enum,format:"open,close",settable:true}` | **自动发现** |
| 3 | 订阅 | `homie/5/<id>/water/valve` = `close` | retained 初值 |
| 4 | client publish `/set` = `open` | — | — |
| 5 | 命令链路 | `command_manager_execute({object:"VALVE_OPEN", source:"homie"})` | RESULT `accepted` |
| 6 | **硬件** | **电磁阀实际打开** | **物理动作** |
| 7 | 状态回传 | `homie/5/<id>/water/valve` = `open`（retained 更新） | UI 实时变化 |
| 8 | 失败路径 `/set` = `bogus` | 无任何 Homie 发布；WARN 落 LogManager | 协议层拒绝 |
| 9 | **安全路径** retained `/set`（**场景 A 实时 + 场景 B broker 重放**） | 无发布 / 无硬件动作 | `event->retain` 生效（**唯一判据**） |
| 10 | 越权订阅 `homie/5/<他机>/#` | **SUBACK 拒绝** | ACL 生效 |

**三级成立判据**：Step 1–7 = semantic/control-loop proven；Step 10 = security/identity/isolation proven；**Overall PASS = Step 1–10 全 PASS**。

**Failure cases（§19.3）**：`$description` >4096 ⇒ 停 `init` · registry 未就绪 ⇒ 停 `init` · Action 未注册（`weight.enable=false`）⇒ 属性不出现 · MQTT 断 ⇒ broker 发 `lost` · set 到只读属性 ⇒ 丢弃 · retained `/set` 两场景 ⇒ 丢弃。

---

## 2. 测试责任矩阵（**以代码与 §19 为准**）

| 测试类别 | Agent 自动 | 需真实设备 | 当前状态 | 证据 / 说明 |
|---|:---:|:---:|---|---|
| Device Model semantic（node/property/命令映射） | YES | NO | **可执行** | `test/device_model_host`（正向 PASS + 负向 7 FAIL 如期） |
| Homie topic 生成 / namespace 校验 | YES | NO | **可执行** | `test/homie_bridge_host` 222 PASS |
| `$description` 确定性输出 | YES | NO | **可执行** | 1,907 B / 1,618 B，逐字节稳定 |
| `/set` 协议层校验（存在性/settable/dtype） | YES | NO | **可执行** | host 222 + `p2_4_homie_set_check.py` 64/64 |
| retained `/set` **判定逻辑**（`event->retain`） | YES | NO | **可执行（逻辑层）** | `p2_3_homie_check.py` 116/116 |
| retained `/set` **真实投递 / broker 重放** | NO | **YES** | **HUMAN_REQUIRED** | 需真实 EMQX broker + 设备 |
| CommandManager 集成（`source="homie"`） | YES | NO | **可执行** | `p2_2_transport_check.py` 45/45 |
| pending table / 结果分类 | YES | NO | **可执行** | host 222 PASS |
| **Valve 真实开/关** | NO | **YES** | **HUMAN_REQUIRED** | 需物理阀 + 驱动 + 电源 |
| **HX711 重量反馈** | NO | **YES** | **HUMAN_REQUIRED** | 需秤 + 已知砝码 |
| **Weight zero 命令** | NO | **YES** | **HUMAN_REQUIRED** | 同上 |
| Wi-Fi 真机联网 | NO | **YES** | **HUMAN_REQUIRED** | 需 2.4 GHz AP |
| MQTT/EMQX 真实连接 | NO | **YES** | **HUMAN_REQUIRED** | 需设备凭据 + ACL |
| Homie discovery（真实设备 + 客户端） | NO | **YES** | **HUMAN_REQUIRED** | Step 2–3 |
| `$state` `init`→`ready` 真实生命周期 | 部分 | **YES** | **HUMAN_REQUIRED** | Step 1 |
| **LWT `lost`** | NO | **YES** | **HUMAN_REQUIRED** | 需真实 keepalive 超时或断电 |
| reconnect 行为 | 部分 | **YES** | **HUMAN_REQUIRED** | 需断网/断 broker |
| **cross-device ACL（Step 10）** | NO | **YES（+第二身份）** | **HUMAN_REQUIRED + BLOCKED**（需 P2-5 ACL 先落地，见 §5） | — |
| P1 业务链路不回归 | 部分 | **YES** | **HUMAN_REQUIRED** | 与 Homie 并存 |

> **Agent 侧已可交付的部分**：上述所有 `Agent 自动` 行的测试**均已执行且 PASS**（详见 `P2-6-Resource-Acceptance-Closure.md` §6）。**P2-7 剩余的 100% 是实物/真 broker 环节。**

---

## 3. 需要人工准备 / 连接的真实设备

### A. ESP32-S3 开发板

| 项 | 值 / 要求 |
|---|---|
| 板卡 | **ESP32-S3-DevKitC-1 · N16R8**（16 MB QIO Flash + 8 MB **OPI PSRAM**） |
| 环境 | PlatformIO env `esp32-s3-devkitc-1`（`platformio.ini` 已锁版本） |
| 固件构型 | **必须 Homie 开启**：`PLATFORMIO_BUILD_FLAGS="-DGF_HOMIE_ENABLED=1" pio run`（见 §6 裁决项 Q-1） |
| USB / 串口 | USB 数据线（**非充电线**）；串口日志用于核验 `[Homie] enabled base=…`、`[H] $state=ready`、`[Valve] Init OK (pin=…, active_level=…)`；P0 记录使用 **COM8**（以实际枚举为准） |
| 供电 | 稳定 **5 V USB**；若 Valve 由外部电源驱动，**必须共地（GND 相连）** |
| 串口日志 | **需要**（步骤 1/5/6/8/9 的判据大量依赖串口与 MQTT 双视角） |
| 烧录方式 | `esptool.py write_flash -z 0x10000 <firmware.bin>`（**只写 app 分区**，不动 NVS `gfid`/`gfcred` 与 LittleFS） |

### B. Valve（电磁阀）— **P2-7 必需**

| 项 | 值 / 要求 |
|---|---|
| 是否必需 | ✅ **必需**（Step 6/7 判据是"电磁阀实际打开"） |
| 阀 | 电磁阀（规格需匹配外部电源电压） |
| 驱动 | **MOSFET 模块 / 继电器模块 / 驱动板** —— **严禁直接用 ESP32 GPIO 驱动电磁阀**（浪涌 + 反电动势会损坏 SoC） |
| 外部电源 | 独立电源（电压/电流按阀规格）；**GND 与 ESP32 共地** |
| 保护 | 阀线圈两端 **续流二极管（反接）**；MOSFET 栅极串限流电阻；必要时加 RC 吸收 |
| ESP32 GPIO | **GPIO 12**（`data/config/valve.json` → `gpio_pin: 12`） |
| 有效电平 | `active_level: 1`（高电平开阀）；若硬件为低有效，须经 MQTT `config_set` 改 `active_level` |
| 安全超时 | `safety_timeout_sec`：**仓库值 300 s**，但**设备端历史值为 2 s** ⇒ 实测前用 `config_query` **读取设备端实值** |
| 外部电源是否需要 | ✅ 需要（阀通常 5 V/12 V，且电流超出 GPIO 驱动能力） |

### C. HX711 + Load Cell（电子秤）— **仅"重量相关属性/命令"需要**

| 项 | 值 / 要求 |
|---|---|
| 是否必需 | **对 Step 1–10 核心切片：非必需**（切片 = `water/valve`）。<br>**对 `water/weight`、`water/weight-error`、`water/zero` 的实物验证：必需**（旧 §19 仅定义 valve，故这些属**扩展验证**，不阻塞核心 Step 1–10） |
| 器件 | HX711 模块 + Load Cell（称重传感器） |
| HX711 供电 | 3.3 V（与 ESP32 同域） |
| 引脚 | **DT = GPIO 6** · **SCK = GPIO 7**（`data/config/weight.json`） |
| 量程 | 由 load cell 决定（需人工提供量程与标定砝码） |
| 固件配置 | `weight.enable` **当前为 `false`**（设备端须显式改 `true`）⇒ 不改则 `weight` 属性**不出现**在 `$description`（§19.3 failure case，属**预期行为**） |
| 校准参数 | `scale: 741` · `zero_offset: -800750`（**仓库值**；设备端实际值须 `config_query` 核对） |
| 测试物 | **已知重量的砝码/物体**（用于验证 `weight` 数值与 `zero` 命令把当前读数归零） |
| 必须真实重量才能证明的测试 | ① `weight` 上报数值与实际重量一致（标定正确） ② `water/zero`（`WEIGHT_ZERO`）把随后读数归零 ③ `weight-error` 在断线/未就绪时置位 ④ OQ-3 死区 1.0 g / 1000 ms 在真实抖动下的行为 |

### D. Mijia LYWSD03MMC（温湿度 BLE）

| 项 | 值 / 要求 |
|---|---|
| 是否必需 | ❌ **Not required for P2-7 core vertical slice.**（§19 测试对象 = `water/valve`；`climate/*` 属性不在切片内） |
| 若做扩展验证 | 需：已刷自定义固件并**激活 BLE** 的 LYWSD03MMC；`bind_key`；电池；MAC |
| 仓库现值 | `MAC: A4:C1:38:31:D4:17`（`data/config/mi_thermo.json`，含 `BLE_key`） |
| 距离 | 与 ESP32-S3 保持 BLE 可达距离（数米内，中间无厚墙） |
| 真实温湿度变化 | 需环境变化（如热源/加湿）才能验证变化驱动上报 |
| **不扩大 scope**：不得为测 Mijia 而定制 P2-7 切片 | |

### E. Wi-Fi

| 项 | 值 / 要求 |
|---|---|
| 频段 | **2.4 GHz**（ESP32-S3 不支持 5 GHz） |
| 凭据 | SSID / 密码（`data/config/wifi.json`，仓库值仅示例） |
| 联网 | 需**能出公网**（EMQX 为云 broker，端口 8883 TLS） |
| DHCP | 可用即可（未使用静态 IP） |
| LAN-only | ❌ 不够（broker 在云端） |
| 稳定性 | 需能稳定联网（Step 1 的 `ready` 条件之一 = MQTT 已连接） |

### F. MQTT / EMQX

| 项 | 值 / 要求 |
|---|---|
| Broker | **EMQX Cloud**（`data/config/mqtt.json` → `mqtt_server: n302933b.ala.cn-hangzhou.emqxsl.cn`, `mqtt_port: 8883`, TLS, `ca_path: /emqxsl-ca.crt`） |
| 设备凭据 | 一机一账号 `dev_<device_id>` + password（P1 签发；**明文只在签发帧**，须 `provisionCredential` 显式注入） |
| 设备 topic | 业务：`guo_feeder/<id>/down|up|log` · Homie：`homie/5/<id>/#` |
| **ACL** | **★ 前置阻塞项**：Homie ACL 规则**尚未落地**（见 **§5**）⇒ 未授权时设备**无法 publish/subscribe `homie/5/<id>/#`**（被兜底 `# deny` 全拒） |
| 客户端工具 | **需要** MQTT 客户端（MQTT Explorer / MQTTX / mosquitto_pub-sub）订阅 `$description`、`$state`、属性；发布 `/set`；**场景 B 须能发布 retained `/set` 并让设备重连触发 broker 重放** |
| 第二身份 | **见下** |

#### F.1 单设备可验证（Step 2–9）
`$description` 自动发现 · `$state=ready` · `water/valve` 初值/回传 · `/set=open/close` · retained `/set` 拒绝（场景 A/B）· 命令执行与状态回传 · 失败路径 WARN。

#### F.2 必须第二身份才能验证（**Step 10**）
> **单设备无法证明 cross-device isolation。** Step 10 需要：
> - **第二台 ESP32-S3**（自己的 `device_id` + 凭据），**或**
> - **等价的第二 MQTT identity fixture**（独立的 EMQX 账号 + 其 `homie/5/<id2>/#` ACL 规则）
>
> 验证动作：以**设备 1 的账号**尝试 **SUBSCRIBE `homie/5/<id2>/#`** ⇒ 期望 **SUBACK 拒绝**；以**设备 1 的账号**尝试 **PUBLISH `homie/5/<id2>/water/valve/set`** ⇒ 期望**被 ACL 拒绝**。
> ⚠️ 注意 §16.1 的**硬边界**：`homie/5/#` / `homie/5/+/#` / `homie/#` **绝对禁止**；只有 `homie/5/<id>/#`（子树内）与 `homie/5/<id>/+/+/set` 允许。

---

## 4. 接线清单（ESP32-S3 引脚汇总，源码 / 配置为准）

| 功能 | 引脚 | 来源 | 备注 |
|---|---|---|---|
| **Valve 输出** | **GPIO 12** | `data/config/valve.json` | active_level=1；**经 MOSFET/继电器，不直驱** |
| **HX711 DT** | **GPIO 6** | `data/config/weight.json` | `enable=false`，装秤后置 true |
| **HX711 SCK** | **GPIO 7** | `data/config/weight.json` | — |
| OLED I2C SDA | GPIO 4 | `data/config/oled.json` | 与 P2-7 无关 |
| OLED I2C SCL | GPIO 5 | `data/config/oled.json` | 与 P2-7 无关 |
| Computer Reset | GPIO 8 | `src/app/computer_reset.h` | 与 P2-7 无关 |

**Valve 最小接线（必需）**：
```
ESP32 GPIO12 ──► [栅极电阻] ──► MOSFET Gate
ESP32 GND    ────────────────► 外部电源 GND   （★ 必须共地）
外部电源 +V  ──► 阀线圈一端
阀线圈另一端 ──► MOSFET Drain；MOSFET Source ──► 外部电源 GND
阀线圈两端并联续流二极管（反向）
```

> **无需现在就接线**——本清单仅说明 P2-7 实物测试需要什么。

---

## 5. ★ 前置阻塞项（**必须先解决，否则真机 P2-7 无法开始**）

### 5.1 P2-5（Homie ACL 模板 + 补写）尚未落地

现状核查（只读）：

| 检查项 | 结果 |
|---|---|
| `Cloudflare_Assets/guo-feeder-api/src/credential.js` 是否含 Homie ACL | **否**（`grep homie` = 0 命中） |
| `EMQX_Assets/config/acl-rules.v3.json` 是否含 `homieRuleTemplate` | **否** —— 文件显式写着 *"⑤ `homie/5/<device_id>/#` 属 P2，本阶段**不添加**"* |
| §16.3 步 1/步 2 是否完成 | **未完成**（P2-5 = READY，未开工） |

**后果**：设备账号当前只有 `guo_feeder/<id>/{down,up,log}` 三条；Homie topic 会被**兜底 `# deny`** 全拒 ⇒
- 设备**无法 publish** `homie/5/<id>/#`（Step 1/3/7 全挂）
- 客户端账号**无法 sub/pub** `homie/5/<id>/#`（Step 2/4 挂）

⇒ **P2-5 是 P2-7 真机测试的硬前置**。

### 5.2 生产设备 ACL 补写需**显式授权**（D-10 / OQ-2）

§16.3 步 3：已在册设备（如生产 `288485896ce4`）的 ACL 补写须**显式授权**——`replaceUserAclRules` 是 **PUT 覆盖式** ⇒ **必须一次给全 5 条**，只发 2 条会打掉既有业务规则。
**STOP 条件**：若补写必须改动 D1 历史或触发凭据轮换 ⇒ **STOP**（属另一授权任务）。

> **建议**：P2-7 真机测试**优先使用一台测试设备**（自有 `device_id` + 新签发凭据），而非直接改生产设备 `288485896ce4` 的 ACL。

---

## 6. 待人工确认项（问题清单）

| ID | 问题 | 影响 |
|---|---|---|
| **Q-1** | P2-7 真机固件用哪个构型？`-DGF_HOMIE_ENABLED=1`（dev-like）还是 production（`-DGF_ENV_PRODUCTION=1`，要求三个 MQTT 模板含 `<device_id>`）？ | 决定烧录命令与 P1-7 门禁行为 |
| **Q-2** | 用哪块板 + 哪个 `device_id`/凭据做 P2-7 真机？（建议**测试设备**，勿动生产 `288485896ce4`） | 决定凭据签发与 ACL 目标 |
| **Q-3** | 是否授权 **P2-5 ACL 落地 + 目标设备 ACL 补写**（§5） | **阻塞 Step 1–4、7、10** |
| **Q-4** | 是否有**第二台 ESP32-S3 / 第二 MQTT 身份**？无则 Step 10 无法完成 | 阻塞 **Step 10** |
| **Q-5** | Valve 驱动的**硬件形态**（MOSFET/继电器）、外部电源电压、是否已就绪？ | 决定 GPIO 12 能否安全驱动 |
| **Q-6** | 是否具备 HX711 + Load Cell + **已知砝码**？（仅扩展验证需要） | 不阻塞 Step 1–10 |
| **Q-7** | Wi-Fi 是否 2.4 GHz 且可出公网？ | 阻塞 Step 1 |
| **Q-8** | 是否有 MQTT 客户端工具（MQTT Explorer/MQTTX/mosquitto）？ | 阻塞 Step 2/4/9 |

---

## 7. Agent 与 Human 的边界（**不得越界**）

**Agent 可自动完成（且已 PASS）**：Device Model semantic · Homie topic/namespace · `$description` 确定性 · `/set` 协议层校验 · retained 判定**逻辑** · CommandManager 集成 · pending/结果分类 · P1-7 生产门禁 · 三构型构建 · 宿主/静态回归。

**必须 Human（`HUMAN_REQUIRED`）**：Valve 真实动作 · HX711 真实重量 · Weight zero · Wi-Fi 真机联网 · MQTT/EMQX 真实连接 · Homie discovery（真实 broker） · `$state=ready` 真实生命周期 · LWT `lost` · reconnect · **retained `/set` broker 重放（场景 B）** · **cross-device ACL（Step 10）**。

**Agent 绝对禁止**：mock 成真实硬件测试 · 把 host test 记为 hardware PASS · 声称 Valve 已动作 / HX711 已出真实重量 / broker 已重放 retained / LWT 已被真实客户端观察 / cross-device ACL 已验证。

---

## 8. 当前状态

```
P2-6                          = CLOSED / PASS  （新 ceiling ≤14 KiB，见 Closure 文档）
P2-7 Agent-side 可自动部分     = 已全部执行且 PASS
P2-7 实物/真 broker 部分        = NOT RUN        （HUMAN_REQUIRED）
P2-7 前置依赖                  = BLOCKED  ← P2-5 Homie ACL 未落地 + 目标设备 ACL 需授权（§5）
P2-7 Overall                  = READY — WAITING FOR HUMAN HARDWARE SETUP
P2-8                          = NOT STARTED
```
