# P2-7 Hardware Deferral Record（硬件实物测试延期记录 + 未来返回节点）

> **性质**：**决策与状态记录（documentation-only）**。本文件**不代表任何实物测试已完成**。
> **状态**：**`P2-7 SOFTWARE READY` · `WAITING FOR FUTURE HARDWARE INTEGRATION`**
> **前置**：`P2-7-Software-Readiness-Report.md`（软件侧结论）· `P2-7-Hardware-Test-Readiness.md`（实物准备清单）
> **上位冻结文档**：`P2-0-architecture-review-and-design-freeze.md` §19（P2-7 Vertical Slice, FROZEN）
>
> **⚠️ 术语铁律**：`PASS` · `FAIL` · `NOT RUN` · `HUMAN_REQUIRED` · `DEFERRED`
> **`HUMAN_REQUIRED` ≠ `FAIL`；`DEFERRED`（延期）≠ `PASS`，也 ≠ `FAIL`。**
> 本文件**不改变任何测试标准**，**不删除任何硬件测试清单**，**不把任何硬件项标记为 PASS**。

---

## 1. 决策

| 项 | 内容 |
|---|---|
| 决策 | **P2-7 硬件实物测试阶段暂缓（DEFERRED），不等待硬件接入** |
| 生效 | 2026-10-09 |
| 原因 | 当前**无法连接**真实 Valve、HX711、外部传感器，**也没有第二台 ESP32-S3** |
| 硬件验证去向 | **由人工在未来完成**，计划与 **App 联调阶段**一并做端到端验证 |
| 对阶段划分的影响 | **P2-7 不作为阻塞阶段**（不阻塞 P2-8 及其后续） |
| 对软件侧结论的影响 | **无**：软件侧 PASS 结论全部保留、全部有效 |

### 1.1 为什么可以「延期而非视为失败」

- §19 的三级成立判据中，**软件侧可验证部分已 100% 执行且 PASS**（详见软件侧报告 §1）；
- §19 的 `Overall PASS = Step 1~10 全 PASS` —— 因为依赖实物的 Step（6/7/9/10 等）**本轮不可执行**，
  其状态是 **`NOT RUN`**，**不是** FAIL，也**不构成** 缺陷判断；
- 因此延期**不改变**任何既有结论，只是把「何时取得实物证据」后移。
  **★ 但延期也绝不等于通过：含硬件的 P2-7 Overall 仍为 `NOT RUN`。**

---

## 2. 软件侧已完成（保留，不变）

> 完整结论与证据见 **`P2-7-Software-Readiness-Report.md`**；此处仅列索引，**不重复判据、不修改数字**。

| 类别 | 结果 | 位置 |
|---|---|---|
| 固件三构型构建（含 Homie-enabled） | **PASS**（`[SUCCESS]`、0 error、无新增 warning） | 软件侧报告 §1.1 |
| 资源回归 vs P2-6 baseline | **PASS**（逐段逐字节一致；B − base = +12,548 B ≤ 14,336 B） | 软件侧报告 §1.2 |
| 静态契约（GPIO / 配置 / ACL↔topic 5/5） | **PASS** | 软件侧报告 §1.3 |
| 自动化回归（9 类套件） | **全绿**（728 断言 PASS / 0 FAIL；+7 项已登记 legacy `EXPECTED_FAILURE`） | 软件侧报告 §1.4 |
| MQTT/Homie 软件侧制品（topic / `$description` / ID / set parser） | **PASS** | 软件侧报告 §1.5 |
| EMQX 真实 broker 授权（fixture ACL isolation） | **PASS**（23/23，探针零残留） | 软件侧报告 §1.6 |

> **★ 保留原则**：以上结论在本文件生效后**依然有效**；
> 未来取得实物证据后，**只需新增**（`NOT RUN` → 实测结果），**不得改写**既有 PASS 结论。

---

## 3. 硬件项清单（一律保持 `NOT RUN` / `HUMAN_REQUIRED`）

> **★ 本清单为「原样保留」**：条目、判据、归属**均未修改、未删除**。
> 权威版本 = `P2-7-Hardware-Test-Readiness.md`（准备清单，全文保留）与软件侧报告 §2（N-1…N-16）。

| # | 硬件/真环境项 | 状态 | 说明 |
|---|---|---|---|
| H-1 | Valve 实物动作（`/set=open/close` ⇒ 实际开/关） | **NOT RUN** | 无阀、无驱动、无外部电源 |
| H-2 | HX711 真实重量闭环（读数 ↔ 实际重量） | **NOT RUN** | 无 HX711、无 Load Cell、无砝码 |
| H-3 | 电磁阀安全超时断电（`safety_timeout_sec` 到期强制关闭） | **NOT RUN** | 需实物；设备端历史值为 2 s |
| H-4 | retained `/set` 真机端到端（场景 A 实时 + 场景 B broker 重放） | **NOT RUN** | 需真机 + 能发 retained `/set` 的客户端 |
| H-5 | Homie discovery 真机验证（客户端订阅 `homie/5/<id>/#`） | **NOT RUN** | 需真机 CONNECT 并由 broker 保留 retained |
| H-6 | LWT 真机验证（`$state = lost`） | **NOT RUN** | broker 行为；**不得用 `SIGKILL` 代替** |
| H-7 | 第二设备隔离（Step 10 cross-device ACL） | **NOT RUN** | 需第二台 ESP32-S3 / 第二 MQTT 身份；本轮仅 `fixture ACL isolation` |
| H-8 | `$state` `init`→`ready` 真实生命周期 | **NOT RUN** | 需真机 + 真 broker |
| H-9 | reconnect 行为 | **NOT RUN** | 需可断网 / 断 broker 的真机环境 |
| H-10 | Wi-Fi 真机联网 / MQTT 真实连接 | **NOT RUN** | 需 2.4 GHz 可出公网 + 设备凭据 |
| H-11 | `water/zero`（`WEIGHT_ZERO`）真机归零 | **NOT RUN** | 依赖 H-2 |
| H-12 | `weight-error` 断线置位 / OQ-3 死区真实抖动 | **NOT RUN** | 依赖 H-2 |
| H-13 | P1 业务链路与 Homie 并存不回归（真机） | **NOT RUN** | 需真机 |
| H-14 | boot `$description` 构建 wall-clock（< 5 ms） | **NOT RUN** | 宿主代理值不代表真机 |
| H-15 | 网络稳态零流量抓包 | **NOT RUN** | 需设备侧抓包 |
| H-16 | Mijia `climate/*` 实物读数 | **NOT RUN** | 非 P2-7 核心切片所需；且无该传感器 |

---

## 4. 未来返回节点（Re-entry points）

> 硬件接入后，**按以下节点顺序回归**。每一节点都**只新增证据**，不改既有结论。
> 逐步执行清单（接线 / 烧录 / 设备端配置核对 / Step 1–10）见 `P2-7-Hardware-Test-Readiness.md` §3 与软件侧报告 §3。

| 节点 | 触发条件 | 需回归的项 | 产出 |
|---|---|---|---|
| **R-1 Valve 接入** | 阀 + MOSFET/继电器 + 独立电源 + **共地** + 续流二极管就绪 | **H-1**（Step 6/7 物理动作）· **H-3**（安全超时断电） | Valve 动作证据 + 串口日志 + 状态回传 |
| **R-2 HX711 接入** | HX711 + Load Cell + **已知砝码**就绪（`weight.enable=true`） | **H-2** · **H-11** · **H-12** | 重量标定证据 + 归零证据 + 死区行为 |
| **R-3 真机 MQTT / Homie 测试** | 真机 + 2.4 GHz 可出公网 Wi-Fi + 设备凭据 + MQTT 客户端 | **H-5** · **H-8** · **H-9** · **H-10** · **H-14** · **H-15** · **H-13** | `$description` 自动发现 + `$state` 生命周期 + reconnect + 抓包 |
| **R-4 第二设备隔离测试** | 第二台 ESP32-S3（或等价第二 MQTT 身份） | **H-7**（Step 10） | SUBACK/publish **被拒** 证据（`real second-device isolation`，与 `fixture` 级证据区分） |
| **R-5 retained / LWT / discovery 真机测试** | 真机 + 可控断连/断电 + 能发 retained 的客户端 | **H-4**（场景 A/B）· **H-6** | retained `/set` 被丢弃；`lost` 由 broker 发出 |
| **R-6 与 App 联调测试** | App 侧就绪 + 真实设备在线 | 端到端联调（App ⇄ Homie ⇄ 设备 ⇄ 执行） | 联调报告（含上列硬件项的真机证据合流） |

> **★ R-6 是延期决策的落点**：本次延期的主要去向 = **App 联调阶段**。R-1…R-5 可在 R-6 之前或随其一并完成。

---

## 5. 再入时的纪律（不得放宽）

| 规则 | 说明 |
|---|---|
| **不得把任何硬件项标记为 PASS**，除非取得真实实物证据 | 禁止 mock 成真机、禁止把 host test 当 hardware PASS |
| **不修改测试标准** | §19 三级成立判据、Step 1–10 判据、`NOT RUN` 判据原样适用 |
| **不删除硬件清单** | 本文件 §3 与 `P2-7-Hardware-Test-Readiness.md` 全文保持 |
| **证据分级不得混同** | `fixture ACL isolation` ≠ `real second-device isolation` |
| **真实 LWT 不得用 `SIGKILL` 代替** | 进程被杀 ⇒ OS 发 FIN ⇒ 得 `tcp_closed`，keepalive 计时器不参与 |
| **设备端配置以 `config_query` 实值为准** | 仓库 `data/` 与设备端**已知有 6 例不一致**（含 `valve.safety_timeout_sec`、`weight.enable`、`weight.dt/sck`、`mqtt_keep_alive`） |
| **安全前置** | **严禁 GPIO 直驱电磁阀**；必须驱动级 + 外部电源 + 共地 + 续流二极管 |
| **范围** | 再入时**不得**顺带改动 firmware / 架构 / ACL contract；改动须独立授权 |

---

## 6. 当前状态

```
P2-7 软件侧                    = SOFTWARE READY          （见 P2-7-Software-Readiness-Report.md）
P2-7 硬件实物                  = DEFERRED → NOT RUN      （HUMAN_REQUIRED ×16，见本文件 §3）
P2-7 含硬件 Overall            = WAITING FOR FUTURE HARDWARE INTEGRATION （非 PASS，非 FAIL）
P2-7 是否阻塞                  = 否
返回节点                       = R-1 Valve → R-2 HX711 → R-3 真机 MQTT/Homie → R-4 第二设备隔离
                                 → R-5 retained/LWT/discovery → R-6 App 联调
P2-8                           = NOT STARTED （计划见 P2-8-Plan.md）
```

---

## 7. 范围与纪律声明

**本文件为 doc-only**。**未修改**：任何 `src/` 固件源码 · `platformio.ini` · partition ·
Cloudflare / EMQX 代码与 ACL contract · 架构冻结文档（`P2-0-…freeze.md`）· 任何测试脚本 · 任何测试判据。

**未做**：把硬件项标为 PASS · mock 真机 · 删除硬件清单 · 修改测试标准 · 实现 P2-8 内容 · push。
