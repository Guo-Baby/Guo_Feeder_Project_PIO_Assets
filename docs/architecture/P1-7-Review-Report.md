# P1-7 Production Check —— 只读代码审阅报告（Review Report）

> **性质**：只读审阅 + 判定 + 修复方案。**未修改任何代码**。  
> **范围**：固件仓 `PIO_Assets/Guo_Feeder_Project`（分支 `wb`，审阅时 HEAD `2fb66ca`）。  
> **日期**：2026-10-07  
> **依据**：`P0-设备身份与Topic隔离设计.md` §3.3.1 / §8.2 / §9 · `P1-实现清单.md` §P1-7 / §3.2 (D-7) · 实际源码（**代码优先于文档**）。

---

## 1. 当前实现结论

### **FAIL —— P1-7 尚未实现**

并且不是"现有架构可安全修复的纯实现缺陷"，而是**前置设计缺口**：

> **P1-7 的真正缺口是「production mode 判定机制尚未定义」，而不是 TopicRenderer 缺功能。**

TopicRenderer 本身**按设计正确**（T-5 刻意允许 legacy 透传）；缺的是它**上游**的两件事：

1. **没有任何 production / dev / test 判定机制**（P0 §3.3.1 明确 defer 给后续阶段）；
2. **没有任何占位符检查，也没有任何"阻止上线"的闸门** —— 现有的"渲染失败 ⇒ 配置错误"路径**从未被调用方接线**。

---

## 2. Production 判定机制

### **当前:不存在。** 可靠性:**不可用（机制缺失）**

| 候选机制                       | 实际状态          | 证据                                                                                                                                 |
| -------------------------- | ------------- | ---------------------------------------------------------------------------------------------------------------------------------- |
| 编译期宏 / build profile       | ❌ **无**       | `platformio.ini` 只有 **1 个 env**（`esp32-s3-devkitc-1`）；`build_flags` 仅 `-DARDUINOJSON_USE_CBOR=1`、`-DARDUINO_LOOP_STACK_SIZE=16384` |
| 运行时 JSON 配置项               | ❌ **无**       | `src/` 全量扫描无 environment / mode 字段；`config_manager` 无相关 getter                                                                     |
| 运行时环境判定                    | ❌ **无**       | `grep -rn "PRODUCTION\|production\|STRICT_\|ENFORCE" src/` → 仅命中日志等级常量与注释                                                          |
| `GF_TOPIC_RENDER_SELFTEST` | ⚠️ **不是环境开关** | `topic_renderer.h:37` 默认 `0`，且**全工程从未被定义/覆盖**（`platformio.ini` 与 `src/` 均无）⇒ 它是**自检开关**，不是「生产/开发」判定                                |

**文档层面**：`P0-设备身份与Topic隔离设计.md` §3.3.1 **原文**：

> 「本节**只增加设计约束**，**不要求 P0 实现完整的"生产模式管理"**（**不新增**配置项 / 模块 / EventId）。  
> "production 标志"如何确定（编译期宏 / 配置项 / 构建 profile）属**后续阶段**的设计问题，**P0 不决策**。」

⇒ **该"后续阶段"就是 P1-7，而 P1-7 也从未做过这个决策。** 这是本工作包的第一号缺口。

### 候选方案分析（不擅自选择）

| #     | 方案                                            | 正确性                                                                                                    | 安全性（可否被运行期绕过）                                | 对 dev/test 影响                                                              | 需新增 JSON 配置                | 需新增模块 | 满足 P1-7 目标 | 结论     |
| ----- | --------------------------------------------- | ------------------------------------------------------------------------------------------------------ | -------------------------------------------- | -------------------------------------------------------------------------- | -------------------------- | ----- | ---------- | ------ |
| **①** | **编译期宏**（如 `-DGF_ENV_PRODUCTION=1`，默认 0）      | 高 —— 构建时决定，不可运行期改写                                                                                     | **最高** —— 串口 `config_set` / 云端下发**均无法关闭**    | 无（宏=0 即现状）                                                                 | ❌ 否（编译开关）                  | ❌ 否   | ✅          | **推荐** |
| ②     | 运行时 JSON 配置项（如 `env.mode`）                    | 中                                                                                                      | **低** —— 可被 `config_set` / 云端关掉 ⇒ 与"生产不可改"矛盾 | 无                                                                          | ✅ **是**（违反 P1-7「不新增配置项」边界） | ❌ 否   | ⚠️ 部分      | 不推荐    |
| ③     | 复用既有可观测状态（`gfcred` 凭据存在性 / 云端 `device.state`） | **低** —— 语义不稳：E 阶段测试设备 `3cfb56c8a5fc` 同样持有凭据；`CLAIM_PENDING` 也有凭据；固件**读不到**云端 `device.state`（P1 无状态下行） | 中                                            | 无                                                                          | ❌ 否                        | ❌ 否   | ❌ 不可靠      | 不推荐    |
| ④     | 对**所有**环境强制要求 `<device_id>`                   | 高                                                                                                      | 高                                            | **破坏** —— 违反 §3.3.1（dev/test 必须保留回滚）与 `topic_renderer.h` **T-5**（"不可改为报错"） | ❌ 否                        | ❌ 否   | ⚠️ 过度      | 不推荐    |

**推荐 ①（编译期宏）**，理由：P0 §3.3.1 列举的三种候选中最强的一个；**唯一**同时满足「不可运行期篡改」「不新增 JSON 配置项」「不新增模块」「不破坏 T-5」的方案。

---

## 3. Topic V3 enforcement

### **不存在。`<device_id>` 检查与"阻止上线"闸门均无。**

**当前实际行为链**（`cloud_manager.cpp`）：

| 步骤    | 位置                                                                  | 实际行为                                                                                                   |
| ----- | ------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------ |
| ① 读模板 | `config_manager.cpp:3694-3726`                                      | 返回**模板**（未渲染）；缺省值即 V3 宏                                                                                |
| ② 渲染  | `cloud_manager.cpp:1726/1730/1734` → `cloud_render_topic()` `:1588` | 含占位符 ⇒ 替换；**不含 ⇒ 原样放行 + 一次 WARN**（`:1594-1598`）；渲染失败 ⇒ ERROR + 内建 V3 兜底（`:1603-1612`）；内建亦失败 ⇒ **返回空串** |
| ③ 连接  | `cloud_connect()` `:1470`                                           | **无任何前置校验** ⇒ 直接 `esp_mqtt_client_init()` `:1545` → `esp_mqtt_client_start()` `:1560`                  |
| ④ 订阅  | `MQTT_EVENT_CONNECTED` 回调 `:369-378`                                | 用 `mqtt_sub_topic.c_str()` 订阅（即便为空串）                                                                   |

**结论**：

- 模板写成 `guo_feeder/down`（缺占位符）→ **识别为"共享 Topic 模式"？有**（仅 WARN）；**报 ERROR？没有**；**阻止 MQTT 上线？没有**。
- **MQTT 会正常建立连接与订阅** —— 即"允许生产长期跑共享 Topic"这一被 §3.3.1 明确禁止的状态，目前**毫无技术拦截**。
- 反向证据：`cloud_render_topic()` 自己的注释（`:1583`）写着「内建模板亦失败 → 返回空串，**交由调用方判为配置错误**」，但 `cloud_connect()` **从未检查空串** ⇒ **该契约未实现**（见 F-3）。

---

## 4. Dev / Test 影响

- **legacy fallback 目前完全按 P0 设计工作**：`topic_renderer.cpp:63-72` 对无占位符模板原样返回 `true`（**T-5 冻结：不可改为报错**）+ `cloud_manager.cpp:1594` 打一次 WARN。**这是刻意设计，不是 BUG。**
- **P1-7 目前零误伤** —— 因为**根本没有检查**。（一旦补上检查，**只要沿用「编译期宏默认 0」，dev/test 固件行为 100% 不变**。）
- **真正的设计问题是**：`production 是否被允许继续使用 legacy template` —— 当前**允许**（且无任何提示级别以上的信号），与 §3.3.1 冲突。

---

## 5. 发现的问题

| #       | 级别                | 文件 : 函数 : 行                                                                            | 现象                                                                                                                                      | 原因                                                 | 与 P1-7 的关系                                                            |
| ------- | ----------------- | -------------------------------------------------------------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------- | -------------------------------------------------- | --------------------------------------------------------------------- |
| **F-1** | **BLOCKER**（设计缺口） | `docs/architecture/P0-设备身份与Topic隔离设计.md` §3.3.1（`:546-557`）；全仓                         | 工程中**不存在** production / dev / test 判定机制                                                                                                 | P0 **显式 defer**：「编译期宏 / 配置项 / 构建 profile … P0 不决策」 | **P1-7 的前置条件缺失** ⇒ 无法安全实现（无法区分"该拦"与"不该拦"）                             |
| **F-2** | **MAJOR**（实现缺口）   | `cloud_manager.cpp` : `cloud_render_topic()` `:1588-1613`                              | 模板缺 `<device_id>` 时**原样放行**，仅 WARN，不报错、不拦截                                                                                              | 遵循 T-5（透传是刻意的）；**但 P1-7 要求的"生产 ⇒ 报错并阻止上线"未叠加**     | **直接违反 P1-7 目标行为**                                                    |
| **F-3** | **MAJOR**（实现缺口）   | `cloud_manager.cpp` : `cloud_connect()` `:1470-1570`                                   | `esp_mqtt_client_init/start` 前**不校验** `mqtt_sub_topic` / `mqtt_pub_topic` / `mqtt_client_id` 是否为空 ⇒ `device_id` 不可用时仍启动 MQTT 并订阅空 topic | `cloud_render_topic()` 的"返回空串交由调用方判错"契约**调用方未实现**  | 说&#x660E;**"阻止上线"的闸门整体不存在**（不只缺 legacy 检查）                            |
| **F-4** | MINOR             | `cloud_manager.cpp` : `:1586` / `:1594-1598`                                           | `s_warned_shared_topic` 是**单个** static ⇒ subscribe / publish / log 三个 topic **合计只警告一次**                                                 | 单标志去重粒度过粗                                          | 诊断不完整（排障时易漏看另外两个 topic 也是 legacy）                                     |
| **F-5** | MINOR             | `cloud_manager.cpp` : `cloud_send_log()` `:2183-2195`                                  | `mqtt_log_topic` 为空的场景有**懒兜底**（重新渲染 V3），而 sub/pub **没有**                                                                                | 两处演进不同步                                            | 三 topic 处理不一致；sub/pub 的空值永远不会被兜底                                      |
| **F-6** | **QUESTION**（需裁决） | `P0-…设计.md` §3.3.1 `:556` vs `topic_renderer.h` T-5 `:23` / `topic_renderer.cpp:69-72` | §3.3.1 要求「复用既有的**渲染失败 ⇒ 判为配置错误**路径」；但 **legacy 模板按 T-5 是"成功返回"**，**不产生渲染失败** ⇒ 该路径**覆盖不到**"缺占位符"这一情形                                    | 设计的两个约束在此处**语义不闭合**                                | ⇒ 必须**新增一处显式占位符检查**（约 10 行，仍在 CloudManager 内）。**请裁决这是否算违反"不新增重复校验"。** |
| **F-7** | NIT               | `platformio.ini`                                                                       | 仅 1 个 env，无 profile 组织方式                                                                                                                | C-3 只固定了版本，未做环境分层                                  | 若采用方案 ①，需决定"新 env" vs "`build_flags` 开关 + 默认 0"                       |
| **F-8** | MINOR（文档）         | `HANDOFF.md` §4.1 `:121` · `docs/architecture/P1-实现清单.md` §1 `:48`                     | 仍把 **P1-6 标为「⏳ 未开始」**、Phase D 标为「下一阶段」                                                                                                  | 未随 P1-6 三代工作流交付而更新                                 | 与 P1-7 **无直接关系**，但会误导后续会话（**本轮未修**）                                   |

---

## 6. 最小修复方案（前提：F-1 的判定机制先被裁决）

> ⚠️ **只有 production 判定机制被裁决后才可实施。本轮不实施。**

假设采用**推荐方案 ①（编译期宏 `GF_ENV_PRODUCTION`，默认 0）**：

| 项              | 内容                                                                                                                                                                                |
| -------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| **改动文件**       | `platformio.ini`（新增宏，默认 0 时不改变现状）· `src/cloud/cloud_manager.cpp`（2 处）                                                                                                             |
| **① 宏定义**      | `#ifndef GF_ENV_PRODUCTION` / `#define GF_ENV_PRODUCTION 0`（与 `GF_TOPIC_RENDER_SELFTEST` 同一模式：默认 0 ⇒ 生产分支与字符串被 `--gc-sections` 剔除）                                                |
| **② 启动校验**     | `cloud_init()` **末尾**（3 个 topic 渲染完成后）：`#if GF_ENV_PRODUCTION` 下逐个模板判 `indexOf(GF_DEVICE_ID_PLACEHOLDER) < 0` ⇒ **Serial ERROR + 明确文本**，并置模块内 static 标志（如 `s_prod_topic_blocked`） |
| **③ 阻断上线**     | `cloud_connect()` **开头**（`:1470`，**在 `esp_mqtt_client_init()` `:1545` 之前**）早退 `return false` —— 这是**唯一**的 MQTT 启动入口（静态验证确认），故可保证"不建立连接、不订阅"                                       |
| **不修改**        | `topic_renderer.{h,cpp}`（**T-5 冻结，不动**）· `config_manager` · MQTT 协议 · Topic V3 语义 · 云端 / EMQX / D1 · P1-6 相关代码                                                                    |
| **不新增**        | JSON 配置项 ❌ · Config 模块 ❌ · System State ❌ · EventId ❌（若需落日志，**复用既有** `LOG_CFG_MODULE_LOAD_FAILED = 0x0202`）                                                                       |
| **为什么是最小**     | 仅 1 个编译开关 + 2 处十余行判断；**复用**既有"配置错误"概念与既有单点连接入口；**无新机制、无新模块、无配置面扩大**                                                                                                               |
| **非阻塞性**       | 纯**启动期静态比较**（无 `delay()` / 无 `while` / 无网络等待 / 无大栈对象 / 无 static instance runtime state）；不进 `loopTask` 热路径                                                                         |
| **对已有设备影响**    | 宏=0（dev/test 固件）行为**逐字不变**；宏=1 需**重新烧录**才生效 ⇒ 不会追溯改变已部署设备                                                                                                                         |
| **把 F-6 一并消化** | 新增的占位符检查**正是** F-6 指出的那处必要新增（因为 T-5 决定了它无法并入渲染失败路径）；若裁决认为不可接受，则 P1-7 需回到设计阶段                                                                                                      |

---

## 7. P1-8 Go / No-Go

### **`BLOCKED — NEED DESIGN DECISION`**

- **阻塞项**：**F-1** —— production 判定机制未定义。在它被裁决前，任何实现都会是"硬编码一个猜测"，违反 P1-7 边界。
- **对 P1-8 的精确影响**：`P1-实现清单.md` §3.2 用例 **D-7（生产模式检查）** 在 P1-7 落地前**不可能通过**；P1-8 的其余用例（C-1…C-9 / D-1…D-6 / X-1…X-3）**不受阻塞**。
- **另有一项独立遗留**：**P1-5-5 正式验收**（真实 D1 + 部署版 Worker 跑 T-1…T-19）仍未执行。

### 需要你裁决（3 选 1）

1. **采纳方案 ①（编译期宏，推荐）** ⇒ 下一轮可直接进入实现（F-2 / F-3 + F-6 一并消化）。
2. **改用方案 ②/③/④** ⇒ 需先出**设计文档**（P1-7 设计冻结），再实施。
3. **暂缓 P1-7，先做 P1-5-5 + P1-8 其余项** ⇒ P1-7 保留为已知开放项（需在 P1 收口前解决）。

---

## 附：静态验证清单（§八 逐项）

| # | 检查项                        | 结果                                                                                                              |
| - | -------------------------- | --------------------------------------------------------------------------------------------------------------- |
| 1 | 所有 MQTT topic template 使用点 | `cloud_manager.cpp:1726/1730/1734` + `:2186`（log 懒兜底）+ `config_manager.cpp:3694-3726`（getter）。**无遗漏**           |
| 2 | 所有 `<device_id>` 使用点       | `topic_renderer.{h,cpp}`（唯一实现）· `cloud_manager.cpp:1594`（WARN 判定）· `credential_manager.cpp`（client_id 模板）       |
| 3 | 所有 topic renderer 调用点      | `cloud_manager.cpp:1592 / 1607 / 1646 / 1662 / 2186` —— **全部**经 `topic_render()`（T-2 唯一渲染点成立）                   |
| 4 | 所有 MQTT connect 调用点        | **唯一入口** `cloud_connect()` `:1470`；调用点仅 `:2060` / `:2067`（均在同一 `cloud_task()` 状态机内）                             |
| 5 | 所有 production/dev/test 判定  | **零命中**（见 §2）                                                                                                   |
| 6 | 相关配置字段                     | `config_get_mqtt_{client_id,subscribe_topic,publish_topic,log_topic}`（`config_manager.cpp:3688-3726`）           |
| 7 | P0 Topic V3 实现与测试          | 实现 = `topic_renderer.{h,cpp}`；测试 = `topic_renderer_selftest()`（6 用例，**默认不编译**，需 `-DGF_TOPIC_RENDER_SELFTEST=1`） |

**绕过风险排查**：✅ 无绕过 `TopicRenderer` 的 topic · ✅ 无第二个 MQTT 初始化入口 · ✅ 无直接使用的 legacy topic 字面量（`"guo_feeder/..."` 仅存在于 `topic_renderer.h` 宏与已剔除的 selftest）· ✅ 无重复 production check（因为一个都没有）· ✅ 无其他启动路径可绕过未来新增的闸门。

---

**报告结束 —— 本轮为审阅 + 判定 + 方案；未修改任何代码，未 commit / push。**

---

# 8. Implementation Record（2026-10-07 · 方案 ① 已实施）

> 本节为**实施后的追加记录**；§1–§7 的审阅结论与判定保留为**实施前历史**，不改写。

**裁决**：采纳 §6 的**方案 ① 编译期宏 `GF_ENV_PRODUCTION`**。

### 8.1 改动文件（**仅 2 个源文件**，+107 行 / −0）

| 文件 | 改动 |
|---|---|
| `platformio.ini` | +18 行**注释块**：说明开关语义、生产构建命令、以及**为何不在此写 `-DGF_ENV_PRODUCTION=0`**（避免与覆盖值并存触发 `macro redefined`） |
| `src/cloud/cloud_manager.cpp` | +89 行：宏定义（`#ifndef`/默认 0）+ 启动前检查标志 + `cloud_init()` 内策略判定 + `cloud_connect()` 首条守卫 |

**未修改**（逐文件 `git diff --quiet` 验证）：`topic_renderer.{h,cpp}` · `config_manager` · `system_state` · `event_manager` · `log_events.h` · `credential_manager` · 其余全部模块。
**未新增**：JSON 配置项 / Config 字段 / System State / EventId / migration。

### 8.2 判定与拦截点

| 项 | 位置 | 条件 | 行为 |
|---|---|---|---|
| 判定（一次） | `cloud_init()` 内 `#if GF_ENV_PRODUCTION` 块 | 三个 **模板**（`config_get_mqtt_{subscribe,publish,log}_topic()`）任一**不含** `<device_id>` | `Serial "[Identity] ERROR MQTT blocked (production policy: X lacks <device_id>)"` + `s_mqtt_precheck_failed = true` |
| 拦截 | `cloud_connect()` **首条语句**（L1501） | `s_mqtt_precheck_failed` | `return false` —— 在 `esp_mqtt_client_init()`（L1579）/ `esp_mqtt_client_start()`（L1594）**之前**，不建 session、不 subscribe |
| F-3（任何模式） | 同上检查块第二段 | `mqtt_sub_topic` / `mqtt_pub_topic` / `mqtt_client_id` 为空（渲染最终失败） | 同上阻断 |

- 判**模板**而非渲染结果：渲染后占位符已被替换，无法再判定。
- 阻断标志为**模块内 `static bool`**（非 System State、非实例 runtime、不进 loop 热路径）；**判定一次、读取多次**，避免 `cloud_task()` 每轮刷屏。
- dev 构型（宏=0）下策略块整体被预处理剔除，且 `cloud_connect()` 守卫退化为恒假分支（编译器后续可消除）。

### 8.3 证据

| 证据 | 结果 |
|---|---|
| dev 构建（`GF_ENV_PRODUCTION=0`，默认） | `[SUCCESS]` 265.6 s · **0 error / 2 warning**（2 条均为既有 `ArduinoJson` deprecation，与改动无关）· RAM 40.0% / Flash 66.2% |
| prod 构建（`PLATFORMIO_BUILD_FLAGS="-DGF_ENV_PRODUCTION=1"`） | `[SUCCESS]` 256.9 s · **0 error / 2 warning** · **0 `macro redefined`** · RAM 40.0%（同）/ Flash 66.3%（**+436 B**） |
| 二进制门控（`firmware.bin` 字节统计） | dev: `production policy` = **0**（分支被 `--gc-sections` 剔除）· prod: = **3**（编入） |
| 断言脚本 `test/p1_7_prod_check.py --bins` | **27 PASS / 0 FAIL**（源码级 21 项 + 二进制级 6 项） |

> ⚠️ **残留证据缺口（如实登记）**：**MQTT 实际连接行为未在硬件上执行**（需烧录 + broker，超出本轮授权）。
> 按约定以「静态调用路径 + 构型门控 + 二进制字符串门控」替代；**D-7 的 LIVE 复现已并入 P1-8 用例**。

### 8.4 未做项（按裁决刻意保留）

F-4（WARN 粒度）· F-5（log topic 懒兜底风格）· F-7（PlatformIO 多 env 重构）· F-8（历史文档整理）—— 均与 P1-7 核心闭环无关。

### 8.5 状态

**`PASS — READY FOR COMMIT`** · **未 commit / 未 push**。

**实施记录结束。**

---

# 9. Final Review & Closure（2026-10-07 · 人工审阅通过）

> 本节为**实施后独立复审**的结论；§1–§7 审阅、§8 实施记录均保留不改写。

### 9.1 F-6 正式裁决（已采纳）

> **F-6 的显式 `<device_id>` 检查不属于违反「不新增重复校验」。**

理由（冻结）：T-5 规定「无 `<device_id>` 的 legacy 模板是**合法输入**，必须原样透传并返回成功」⇒「缺少 `<device_id>`」**不会**触发 renderer failure。P1-7 要求的是「**production 环境禁止这种合法但不安全的 legacy 模板**」⇒ CloudManager 中的检查是**环境策略检查**，与 TopicRenderer 的**渲染正确性**检查职责正交。**不因 F-6 回滚或重构实现。**

### 9.2 复审清单（逐项取证）

| # | 检查项 | 结论 | 证据 |
|---|---|---|---|
| 1a | 默认值为 `0` | ✅ | `#ifndef` / `#define GF_ENV_PRODUCTION 0`（脚本 A1） |
| 1b | 编译期机制，非运行期配置 | ✅ | 无 JSON 字段 / 无 Config getter / 无 State；脚本 A2（`platformio.ini` 生效行不含 `-D`） |
| 1c | production=1 不可被运行期关闭 | ✅ | `s_mqtt_precheck_failed` 全文件仅 3 处：声明 `:42` / 读取 `:1501` / 置 `true` `:1822` —— **无任何复位路径**；`cloud_init()` 仅 `main.cpp:561` 调用一次 |
| 1d | 普通构建不会误入 production | ✅ | 宏未在 `platformio.ini` 定义 ⇒ 代码默认 0；脚本 A2 |
| 1e | 未新增 JSON 配置 / State / EventId | ✅ | 脚本 A8 / A8b；`git diff` 无配置字段 |
| 2a | 三模板（subscribe/publish/log）均检查 | ✅ | 脚本 A6 / A6b×3 |
| 2b | 任一缺失即阻止上线 | ✅ | 三段 `else if` 链 → 同一 `block_reason` → 同一阻断 |
| 2c | 检查对象是**模板**而非渲染结果 | ✅ | 脚本 A6d（`config_get_mqtt_*…indexOf(GF_DEVICE_ID_PLACEHOLDER)`） |
| 2d | 阻断在 `esp_mqtt_client_init()` 之前 | ✅ | 脚本 A4b/A4c/A4d：守卫为 `cloud_connect()` **首条语句**（L1501）< init（L1579）< start（L1594） |
| 2e | 无第二条 MQTT 启动路径绕过 | ✅ | `esp_mqtt_client_init/start` **各仅 1 处**（脚本 A5）；凭据换连路径仅 `stop+destroy`（`:1999-2000`）后交回既有重连逻辑 ⇒ 必经 `cloud_connect()` |
| 3a | `topic_renderer.h/.cpp` unchanged | ✅ | 脚本 A3 + 逐文件 `git diff --quiet` = UNCHANGED |
| 3b | legacy 透传未被改成 renderer error | ✅ | 脚本 A3b（`out = tmpl;` 保留） |
| 3c | dev/test 既有行为保持 | ✅ | 策略块整体处于 `#if GF_ENV_PRODUCTION` 内；dev 固件中 `production policy` 字符串计数 **0**（脚本 B） |
| 3d | 策略检查与渲染职责分离 | ✅ | 见 §9.1 F-6 裁决 |
| 4a | 空串不再进入 init/start | ✅ | 脚本 A7 / A7b（空 topic / client_id 检查**不在** `#if` 内 ⇒ 任何模式生效） |
| 4b | 不破坏合法 legacy 非空 topic | ✅ | legacy 非空 ⇒ 不触发任一阻断条件 |
| 4c | 未把 log 懒兜底重构成新机制 | ✅ | `cloud_send_log()` 的既有懒兜底**未改**（`cloud_manager.cpp` 该函数不在 diff 内） |
| 5a | 无阻塞等待 | ✅ | 脚本 A9（守卫体内无 `delay()`/`while`/`vTaskDelay`）；纯启动期静态比较 |
| 5b | 未进 loop 热路径反复检查 | ✅ | **判定一次**（`cloud_init()`），`cloud_connect()` 只读 `bool`（1 次比较，无分配、无打印） |
| 5c | 无大栈对象 / 堆分配 | ✅ | 仅 `const char*` + 既有全局 `String` 的 `.length()`；3 次 `config_get_mqtt_*()` 与原有调用同址同量（仅 prod 构型） |
| 5d | 无新增 include | ✅ | `git diff` 无 `+#include` |
| 5e | 无 `macro redefined` 警告 | ✅ | prod 构建日志 `redefined` 计数 **0** |
| 5f | 未修改 P1-6 / 其他冻结模块 | ✅ | 14 个冻结文件逐文件 `git diff --quiet` = UNCHANGED；CF / EMQX 仓工作区为空 |

### 9.3 复审验证结果

| 项 | 结果 |
|---|---|
| dev 构型编译（默认） | `[SUCCESS]` · **0 error / 0 warning**（增量 no-op ⇒ 证明当前源码与已构建内容一致） |
| prod 构型编译（`-DGF_ENV_PRODUCTION=1`） | `[SUCCESS]` · **0 error / 0 warning / 0 `redefined`** |
| 首轮全量构建（证据留存） | dev 265.6 s · prod 256.9 s，**各 0 error / 2 warning**（均为既有 `ArduinoJson` deprecation）· Flash 差 **+436 B** |
| `test/p1_7_prod_check.py --bins` | **27 PASS / 0 FAIL**（复跑一致） |
| `git diff --name-only` | `platformio.ini` · `src/cloud/cloud_manager.cpp` —— **均在授权范围内** |
| 新增文件 | `docs/architecture/P1-7-Review-Report.md`（本文）· `test/p1_7_prod_check.py` |
| 未 push | ✅（commit 后需再次确认） |

### 9.4 未处理项（按裁决刻意保留，均**非阻断**）

F-4（WARN 粒度）· F-5（log topic 懒兜底一致性）· F-7（PlatformIO 多 env/profile 重构）· F-8（历史文档状态更新）。
**残留证据缺口**：MQTT 实际连接行为未在硬件执行（需烧录 + broker）⇒ **D-7 LIVE 复现并入 P1-8 用例**。

### 9.5 结论

## **`P1-7 PASS / CLOSED / READY FOR P1-8`**

- 最终 diff 审阅：**PASS**
- dev / prod 编译：**PASS**
- P1-7 验证脚本：**PASS**（27 / 0）
- 新增 BLOCKER / MAJOR：**无**
- 工作区：**仅含 P1-7 应有的变更与报告**

**P1-7 记录结束。**
