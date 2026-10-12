# P3 · **`Phase C-C1` · `HD-1`** —— **Runtime Telemetry Enable Gate（运行时遥测开关）实施规格**

> ## 📐 `SPEC ONLY` —— **本轮只出规格：不写代码 · 不执行修改 · 不烧录 · 不碰生产**
>
> | 项 | 值 |
> |---|---|
> | **性质** | **实施规格（Implementation Specification）** —— 解决 `Phase C-C1` 审查中的阻断项 **`HD-1`** |
> | **问题（`HD-1`）** | `GF_TELEMETRY_ENABLED=1` 的镜像**目前没有任何运行时开关** —— `mqtt.json` 的 `telemetry.enable` 在 `src/` 中**无任何读取点**（`H-WP4-FW-06` 冻结时只做了配置模型，明示「未改 `src/` / 未改 `ConfigManager`」）⇒ 烧录即上电上行，回退只能重烧 |
> | **目标（本规格）** | 让 `GF_TELEMETRY_ENABLED=1` 的固件**仍可通过 `mqtt.json` 中的 `telemetry` 开关**控制是否发送 telemetry |
> | **基线** | 固件仓 **`d2d6619`**（tag `wp4c-closure-20261012` = `dd78a29`）· 审查文档 `docs/review/Phase-C-C1-Telemetry-Publisher-Implementation-Review.md` |
> | **上游冻结** | `H-WP4-FW-06`（Telemetry Configuration Freeze）· `H-WP4-FW-08`（Resource Budget Override）· `H-WP4C-02`（独立 transport） |
> | **本轮不做** | ❌ 不写代码 ❌ 不执行修改 ❌ **不扩大范围到 `interval_ms` / `type` / 阈值配置** ❌ **不设计云端下发配置** ❌ 不改 topic / Envelope / Worker / EMQX / D1 |
> | **判定** | ✅ **`GO`（附 1 项必须先裁决的承载位置问题 `HD-1b`）** —— 见 §12 / §13 |

---

## §1 目标与范围

### 1.1 Goal

使**有效发送条件**从「仅编译期成立」变为**冻结文档所定义的「两道门」**：

```
有效发送  =  GF_TELEMETRY_ENABLED == 1   （编译期：代码是否在镜像内）
           ∧  mqtt.json telemetry.enable == true   （运行时：是否实际发送）
```

达成后：同一份镜像可通过**配置 + 重启**在「发 / 不发」之间切换，**无需重烧**。

### 1.2 In Scope

| # | 内容 |
|---|---|
| I-1 | 配置字段的**承载位置与键名**规格（含承载方式裁决项 `HD-1b`） |
| I-2 | 默认值与「缺键 / 缺模块 / 存储不可用」的缺省语义（**fail-closed**） |
| I-3 | 启动加载流程与生效时点 |
| I-4 | `telemetry_task()` 的**判定逻辑**（顺序、短路、与既有前置条件的关系） |
| I-5 | 与 `config_set` / `config_save` / `restart` 生命周期的关系 |
| I-6 | 异常配置处理策略（类型错 / 损坏 / 未加载 / 并发） |
| I-7 | 影响评估（文件范围 / Flash-RAM / 分层合规） |
| I-8 | 验证矩阵与回退策略 |

### 1.3 Out of Scope（**本规格明确不覆盖**）

| # | 排除项 | 归属 |
|---|---|---|
| O-1 | **`interval_ms` 的消费与映射**（单一整数 ⇒ `TP_CLIMATE_PERIOD_MS` / `TP_WEIGHT_PERIOD_MS`） | `HD-2`（另一项待裁；本规格只要求**不破坏**其字段位置，见 §3.4） |
| O-2 | `type` 词表 / `value` 编码 / 阈值配置化 | 已冻结为**不可配置**（`H-WP4-FW-06` Non-goals） |
| O-3 | **云端下发 / 修改** telemetry 配置 | 冻结明示不支持（无下行命令） |
| O-4 | 逐通道开关、动态能力配置、value mapping | 同上 |
| O-5 | `/log` 域、WP4C 路由、Worker、D1 | 其他工作包 |

---

## §2 现状事实与约束（**基线实测**）

### 2.1 代码事实（决定方案可行性）

| # | 事实 | 出处 | 对本规格的含义 |
|---|---|---|---|
| F-1 | `ConfigManager` 是配置**唯一入口**；按模块缓存 `JsonDocument`；`/config/<name>.json` + `.bak` + factory 副本 | `config_manager.h:27-41` / `:147-154` | 开关**必须**经 ConfigManager 读取，不得自行读文件 |
| F-2 | 模块名 = 文件名：`wifi/oled/valve/time/weight/mqtt/mi_thermo/rtc` | `config_manager.h:50-57` | 若走独立模块需新增 `CONFIG_MODULE_*` |
| F-3 | **全部配置文件均为纯扁平结构**（实测 9 个 json，**嵌套对象数 = 0**） | `data/config/*.json` 实测 | 任何**嵌套对象**都将是配置系统首例 |
| F-4 | **ConfigManager 无点号路径解析**（`config_manager.cpp` 全域无点号/嵌套键处理） | 实测 | `config set mqtt telemetry.enable true` **不会**写入嵌套对象，只会写一个**字面键名**含 `.` 的扁平键 |
| F-5 | 读取模式 = `module_ro(MODULE)["key"] \| 默认值`（如 `valve["enable"] \| true`） | `config_manager.cpp:3549-3552` / `:3618-3621` | 新增 getter 应沿用同一模式（一行式） |
| F-6 | `config_get_bool(module,key,&v)` 为**扁平单键**读取 | `config_manager.cpp:1488-1502` | 扁平键可直接用通用 API |
| F-7 | 配置改动**重启生效**：`config_set` 只改 RAM + 置 dirty + 排 5 min 重启；`config_save` 落盘 + 10 s 安全重启；**漏 `save` 也会 5 min 后自动 save + 重启** | `config_manager.h:117-134` / `H-WP4-FW-06` Runtime behavior | 开关的**生效时点 = 重启后**（与全部既有配置一致） |
| F-8 | **配置字段只对「读过该字段的固件」生效**（既知坑） | 项目铁律 | 老固件读不到该字段 ⇒ 本次改动**必须随新固件烧录**才有效 |
| F-9 | telemetry 编译器门：`#if !GF_TELEMETRY_ENABLED` ⇒ 空实现 | `telemetry_publisher.cpp:22-36` | 门禁 OFF 时**本规格的全部逻辑不存在**（零成本） |
| F-10 | `telemetry_task()` 既有前置条件：`s_ready` → `cloud_is_connected()` → 通道遍历 → 每轮 ≤1 条 | `telemetry_publisher.cpp:161-219` | 新判定**必须插在既有短路链中**，不得改变既有语义 |
| F-11 | `services/*.cpp` 已普遍 include `services/config_manager.h`（`time_manager` / `wifi_module` / `command_manager` …） | 实测 10 处 | 产出器读配置**不构成**新的分层越界 |

### 2.2 冻结约束（不可自行放宽）

| # | 约束 | 来源 |
|---|---|---|
| C-1 | `mqtt.json` 的 `telemetry` **恰含 2 键**（`enable` bool / `interval_ms` uint32），**无其他键、无更深嵌套** | `H-WP4-FW-06` Frozen fields |
| C-2 | 默认值 **`enable = false`** · **`interval_ms = 300000`**；**缺键行为 = 等同默认值** | `H-WP4-FW-06` Default value |
| C-3 | `enable = false` ⇒ **不产生任何上行报文**（`telemetry_task()` 直接返回） | `H-WP4-FW-06` Runtime behavior |
| C-4 | cloud dynamic config / per-channel enable / threshold tuning / type list customization **一律不支持** | `H-WP4-FW-06` Non-goals |
| C-5 | 阈值 / 周期语义**设备侧固件权威**；`type` 封闭词表**不可配置** | `H-WP4-FW-06` 理由 §1/§2 |
| C-6 | `telemetry v1` 免于增量 ceiling，但**整体固件安全余量检查为强制**（G-1 Flash < 85 % · G-2 RAM 检查不放宽） | `H-WP4-FW-08` 覆盖裁决 + Guard |
| C-7 | 不得改 topic / Envelope 协议 / Worker / EMQX / D1 | `Phase C-C1` 边界 |

---

## §3 配置字段设计

### 3.1 ★ 关键冲突（必须先裁决：`HD-1b`）

冻结形态（C-1/C-2）要求 **`mqtt.json` 内嵌一个 `telemetry` 对象**；但现状 F-3/F-4 表明配置系统是**纯扁平**的，且**无点号路径解析**：

| 维度 | 冻结形态（C-1） | 现状能力 |
|---|---|---|
| 文件形态 | `{ "telemetry": { "enable": false, "interval_ms": 300000 } }` | 全库**零嵌套**（F-3） |
| 读 | `module_ro("mqtt")["telemetry"]["enable"]`（ArduinoJson 链式可表达） | 现有 getter 均为**单键**（F-5） |
| 写（`config set mqtt <key> true`） | 需「点号 = 路径」语义 | **无该语义**（F-4）⇒ 只会生成**字面键** `"telemetry.enable"` |
| 结论 | 直接实施**不可行**：读写路径**形态不一致**（读嵌套 vs 写扁平） | 需在三种承载方案中选择 |

### 3.2 承载方案对比

| 方案 | 文件形态 | 读路径 | `config set` 可用性 | ConfigManager 改动 | 与冻结一致性 | 风险 |
|---|---|---|---|---|---|---|
| **A · 扁平前缀键（推荐）** | `"telemetry_enable": false`（`mqtt.json` 顶层） | `module_ro(CONFIG_MODULE_MQTT)["telemetry_enable"] \| false` | ✅ 现有 `SET_FIELD` **直接可用** | **零**（仅新增 getter） | ⚠️ **形态偏离**（布局层），**字段集/默认值/语义不变** | 低 |
| **B · 嵌套对象 + 路径语义扩展** | 冻结原文形态 | `["telemetry"]["enable"]` | ❌ 需先扩展 ConfigManager 键语义 | **有**（把 `.` 解释为路径，**影响所有模块的键语义**） | ✅ 完全一致 | 高（改动语义面广） |
| **C · 独立模块 `telemetry.json`** | `{"enable":false,"interval_ms":300000}` | `module_ro("telemetry")["enable"] \| false` | ✅（需按「新增模块改 4 处」注册） | 中（模块注册 + 默认值 + 加载） | ⚠️ **承载位置偏离**（换文件），字段集/默认值不变 | 中 |

> ★ **推荐 A**：改动最小（**零 ConfigManager 语义变更** · 零新增模块 · `config set` 命令链路**开箱可用**），且 `mqtt.json` 现有键名本身即带冗余前缀（`mqtt_server` / `mqtt_port`）⇒ 命名风格自洽。
> ⚠️ **须人工确认**：A 属于对 `H-WP4-FW-06` 的**布局层修订**（字段集 / 默认值 / 运行行为**均不变**）—— 依项目纪律，冻结修订须由人工裁决并留痕（`HD-1b`）。
> 备选：若坚持「冻结原文逐字实现」⇒ 选 **B**（须接受 ConfigManager 键语义扩展的影响面）。

### 3.3 规格（按方案 A 展开 · 方案 B/C 见 §3.5）

| 项 | 规格 |
|---|---|
| **承载模块** | `mqtt`（`CONFIG_MODULE_MQTT` · `/config/mqtt.json`） |
| **键名** | **`telemetry_enable`**（全小写 + 下划线；与既有 `mqtt_server` / `sleep_retry_interval` 同风格） |
| **类型** | `bool`（JSON `true` / `false`；**不接受**字符串 / 数字） |
| **语义** | 「telemetry capability 是否实际发送」（＝冻结 `telemetry.enable` 的语义，逐字不变） |
| **默认值** | **`false`**（与 C-2 一致 ⇒ **安全默认：不发送**） |
| **缺键行为** | 等同默认值 `false`（与 C-2 一致） |
| **写入方式** | `config set mqtt telemetry_enable true` → `config save` → 自动重启 |
| **只读性** | 仅设备侧使用；**不接受云端下发该配置**（C-4） |
| **不做** | 不引入第二个键（本包**不**实现 `interval_ms` 消费 —— `O-1`） |

### 3.4 与 `interval_ms`（`HD-2`）的兼容性

- 若人工在 `HD-2` 选择「扁平键」路线 ⇒ 键名预留 **`telemetry_interval_ms`**（本规格**不创建**该键、**不读取**它）。
- 若人工在 `HD-2` 选择「嵌套对象」路线 ⇒ 届时需要**独立裁决**（会重新触碰 §3.1 的路径语义问题）。
- ⇒ **本规格不构成 `HD-2` 的前置约束**，但要求实现时**不占用**该命名空间以外的键。

### 3.5 若人工改选 B / C（差异点摘要）

| 方案 | 追加要求 |
|---|---|
| **B** | 必须定义并冻结「点号 = 路径」语义（作用域 / 冲突处理 / 既有扁平键的兼容），并同步修订 `config_manager.h` 的接口契约与相关的命令文档；风险面覆盖**所有模块的所有键** |
| **C** | 需新增 `CONFIG_MODULE_TELEMETRY` + 模块注册 + 默认值 + `/config/telemetry.json` 的出厂/备份/factory 语义；需人工确认「配置承载位置迁移」这一冻结修订 |

---

## §4 默认值与缺省语义

| 情形 | 行为 | 依据 |
|---|---|---|
| 键存在且为 `true` | **允许发送**（仍需 `GF_TELEMETRY_ENABLED=1`） | C-3 取反 |
| 键存在且为 `false` | **不发送**（`telemetry_task()` 直接返回） | C-3 |
| **键缺失**（老设备 / 未初始化文件） | **视为 `false` ⇒ 不发送** | C-2「缺键等同默认值」+ fail-closed |
| **模块未加载**（`loaded=false` / 存储不可用） | **视为 `false` ⇒ 不发送** | 既有「getter 走默认值兜底」语义（`config_manager.h:141-146`） |
| 键存在但**类型非 bool** | **视为 `false` ⇒ 不发送**（并按 §8 记录告警） | fail-closed；`config_get_bool` 对非 bool 亦返回 false（`config_manager.cpp:1499-1502`） |
| 文件损坏 / 解析失败 | 走既有 **Active → Backup → 默认值** 回退链；最终仍不可用 ⇒ 视为 `false` | `config_manager.h:283-293` |

> ★ **安全总则（fail-closed）**：**任何不确定情形一律判为「不发送」** —— 与冻结默认值一致，且**不可能**因配置异常而意外开启上行。

---

## §5 启动加载流程（时序）

```
上电
 └─ config_init()                       ← 按模块加载 /config/*.json（Active → Backup 回退）
      └─ mqtt 模块 → RAM 缓存（loaded=true/false，含 telemetry_enable 若存在）
 └─ …… 各层初始化 ……
 └─ telemetry_publisher_init()          ← 只记基线、**不发送**（既有语义，不变）
      └─ （本规格可选）在此快照一次开关值；或保持"每轮现读"
 └─ config_boot_validate()              ← setup 末尾写启动标记（既有）
 └─ loop()
      └─ telemetry_task()
           ├─ 门禁 `GF_TELEMETRY_ENABLED==0` ⇒ 空实现（本规格逻辑不存在）
           ├─ 【新增】运行时开关 == false ⇒ 直接返回（不发送、不累计 emitted）
           ├─ s_ready / cloud_is_connected() / 通道遍历 …（既有链路，逐字不动）
           └─ 每轮 ≤1 条 → cloud_send_telemetry()
```

**生效时点**：开关值在**重启后**生效（与全部既有配置一致，F-7）。运行中修改配置 ⇒ `config_set` 置 dirty 并排定 5 min 自动重启。

---

## §6 `telemetry_task()` 判断逻辑（规格 · 非代码）

### 6.1 判定顺序（短路，`GF_TELEMETRY_ENABLED==1` 分支内）

| 序 | 判定 | 不满足时的动作 | 是否新增 |
|---|---|---|---|
| 1 | **运行时开关 = true** | **直接 `return`**（本规格新增；**不**计入 `emitted` / `build_fail` / `transport_fail` / `skipped_offline`，或计入**新增**的「disabled 计数」—— 见 §6.3） | ★ 新增 |
| 2 | `s_ready == true` | `return` | 既有 |
| 3 | `cloud_is_connected()` | `return`（累加 `skipped_offline`） | 既有 |
| 4 | 通道有效 + 阈值/周期命中 | `continue` | 既有 |
| 5 | 组包 + 发送成功 | — | 既有 |

> **顺序理由**：把「开关」放在**最前**（而非放在 `s_ready` 之后），使「关闭状态下零副作用」的语义最清晰，也最小化每次 loop 的额外工作量。

### 6.2 读取方式（二选一 · 由实现按 §13 的 `HD-1c` 定）

| 选项 | 行为 | 优点 | 代价 |
|---|---|---|---|
| **① 每轮现读**（推荐） | 每次 `telemetry_task()` 调 getter 取当前值 | **无状态、无缓存失效逻辑**；`config_reload()` 后立即一致；fail-closed 天然成立 | 每轮一次模块/键查找（微秒量级，可忽略） |
| **② init 快照** | `telemetry_publisher_init()` 读一次存入文件内静态标志 | loop 路径最省 | 运行中 `config_reload()` 后**不生效**，直到重启；需明确「配置改动需重启」这一前提（与 F-7 自洽） |

> 无论哪种：**不做**配置解析、**不做**文件 IO、**不缓存** `JsonDocument`（避开 `config_manager.h:363-376` 的悬空引用陷阱）。

### 6.3 可观测性要求（建议）

| 要求 | 说明 |
|---|---|
| 关闭状态**可区分** | 建议在 `TelemetryStats` 增补「因开关关闭而跳过」的计数（或复用既有计数 + 明确不计入 `emitted`）；避免把「被开关关掉」误读为「设备故障」 |
| 不影响既有诊断 | 既有 4 个计数（`emitted` / `transport_fail` / `build_fail` / `skipped_offline`）语义**不得改变** |

---

## §7 与 `config_set` / `config_save` / `restart` 生命周期关系

| 步 | 动作 | 系统行为 | 开关状态 |
|---|---|---|---|
| 1 | `config set mqtt telemetry_enable true` | 入队 → `config_task()` 执行 → 改 **RAM** + 置 **dirty** + 排定 **5 min** 自动重启；回调返回 `SUCCESS`（**≠ 已生效**） | **仍未生效**（RAM 已改，运行中的逻辑按 §6.2 决定是否即时可见） |
| 2a | `config save` | 落盘（原子写 + Active/Backup 轮换）→ **10 s 安全重启** | **重启后生效** |
| 2b | 若漏发 `save` | **5 min 后自动 save + 重启** | 同样**重启后生效** |
| 3 | 重启完成 | `config_init()` 重新加载 ⇒ 开关值进入 RAM | **生效** |
| 4 | 回退（关闭） | 同一命令置 `false` → `save` → 重启 | **恢复不发送**；**无需重烧**（这是本规格相对现状的核心收益） |

**必须遵守的既有约束**：

| # | 约束 | 含义 |
|---|---|---|
| L-1 | `set` 与 `save` 是**两步**（漏 `save` 由 5 min 兜底） | 操作单必须写清两步 |
| L-2 | `valid` 是 **RAM 标志、不进 BIN** ⇒ 重启后 `config get` 可能返 `error 5` | 验证时以**重启后行为**为准，不以 `get` 为唯一判据 |
| L-3 | **禁 `uploadfs`** | 配置只能经命令链路写入，不得刷文件系统 |
| L-4 | **配置字段只对读过该字段的固件生效**（F-8） | 老固件上设置该键**无效**；必须先烧入本改动 |
| L-5 | `cmd_id` 去重（10 项 / 30 s 重复静默丢弃） | 连续下发需换新 `id` |

---

## §8 异常配置处理策略

| # | 异常 | 处理 | 可见性 |
|---|---|---|---|
| E-1 | 键缺失 | 视为 `false`（不发送） | 静默（与默认值同义） |
| E-2 | 类型错误（字符串 / 数字 / null） | 视为 `false`；**不改写**配置文件 | 建议记一条配置告警（不阻断启动） |
| E-3 | `mqtt.json` 解析失败 | 走既有 Active → Backup 回退；均失败 ⇒ 模块未加载 ⇒ `false` | 既有启动日志 |
| E-4 | 存储不可用（LittleFS 挂） | `config_init()` 返回 false ⇒ 全部 getter 走默认值 ⇒ `false` | 既有启动日志 |
| E-5 | 模块被 `config_release_module()` 释放 | getter 走默认值 ⇒ `false` | 与 E-1 同 |
| E-6 | 运行中 `config_reload()` | ①每轮现读 ⇒ 立即一致；②init 快照 ⇒ 保持旧值至重启 | 由 `HD-1c` 决定 |
| E-7 | 非法值（非 bool）经 `config set` 写入 | 入队阶段按既有规则处理（`SET_FIELD` 仅接受标量）；若写入非 bool ⇒ 由 E-2 兜底判 `false` | 既有命令响应 + E-2 |
| E-8 | 键被误删（`delete_field`） | 回到 E-1 ⇒ `false`（安全） | 命令响应 |

> ★ 所有异常路径**均收敛到「不发送」**，**不存在**「配置异常 ⇒ 意外开启」的路径（fail-closed 闭合）。

---

## §9 影响评估

### 9.1 修改文件范围

| # | 文件 | 改动性质 | 说明 |
|---|---|---|---|
| M-1 | `src/services/config_manager.h` | **新增 1 个 getter 声明** | `config_get_telemetry_enable()`（命名待定稿，遵循既有 `config_get_*` 风格） |
| M-2 | `src/services/config_manager.cpp` | **新增 1 个 getter 实现**（一行式，沿用 `\| 默认值` 模式） | 位置：MQTT 配置段（`config_get_mqtt_keep_alive()` 一带） |
| M-3 | `src/services/telemetry_publisher.cpp` | **新增判定 1 处** + （可选）include `services/config_manager.h` | §6.1 序 1；**不改**既有 4 步链路 |
| M-4 | `src/services/telemetry_publisher.h` | （可选）`TelemetryStats` 增补「disabled」计数 | §6.3；若不增补则不改 |
| M-5 | `data/config/mqtt.json` / `.example` | **不必须**（缺键 = 默认值） | ⚠️ `data/` 为 gitignore ⇒ 仓库有值 ≠ 设备端有值；如需示例可另附 |
| — | `src/main.cpp` | **零改动** | 既有调用点不变 |
| — | Worker / EMQX / D1 / topic / Envelope | **零改动** | C-7 |

> **未新增** 模块、未新增文件、未改命令协议、未改下行格式。

### 9.2 Flash / RAM 影响（估算 → **实测已回填** · 见 §15.2）

| 项 | 估算 | 依据 / 要求 |
|---|---|---|
| Flash | **≈ +30 ~ +150 B**（新增 getter + 1 个调用点） | 量级参考：现行「门禁 OFF 相对基线 = +48 B」= 两次空实现调用；**须在实现后 clean build 实测**，并与 `1,394,373 B`（门禁 ON 现值）对照 |
| RAM | **+0 B**（每轮现读）或 **+1 B**（init 快照，受 BSS 对齐影响可能 +4 B） | 不引入容器 / 不引入 DynamicJsonDocument |
| 栈 | **+0**（不新增缓冲区） | 复用既有 192 B 信封缓冲（`TELEM_ENV_MAX_BYTES`） |
| 余量对照 | Flash ≈ **66.5 %** / ≤85 %（G-1）· RAM **不变**（G-2 不放宽） | `H-WP4-FW-08` Guard 仍满足 |
| 成本归属 | 归属 `telemetry v1` ⇒ **免增量 ceiling**（C-6） | 但**整体安全余量检查仍强制** |

> ✅ **实测回填（`2026-10-13` · clean build）**：`HD-1` 自身增量 = **+92 B Flash / +0 B RAM**（同树 `B0→B` 对照，见 §15.2）—— **落在本表估算区间内**；Flash **66.5 %** ≤ 85 %（G-1）、RAM **40.5 %** 不变（G-2 不放宽）⇒ **Guard 仍满足**。

### 9.3 分层合规性检查

| # | 规则 | 本规格是否违反 | 依据 |
|---|---|---|---|
| L-1 | **ConfigManager = 配置唯一入口** | ✅ 不违反 | 只经 `config_get_*` 读；不读文件、不缓存文档 |
| L-2 | **`telemetry_publisher` 不负责配置存储** | ✅ 不违反 | 产出器**不含**键名常量以外的配置知识？→ ⚠️ 见下注 |
| L-3 | 不绕过 `CommandManager` | ✅ 不违反 | 无新命令、无旁路；配置写入仍走 `config_cmd_enqueue_*` |
| L-4 | 不改 topic / Envelope / Worker / EMQX / D1 | ✅ 不违反 | 零触碰 |
| L-5 | 非阻塞 / 无动态分配 | ✅ 不违反 | 判定为纯内存查表 + 早返回 |
| L-6 | 状态放置（"禁 static"铁律） | ✅ 不违反 | 仅 `loopTask` 上下文；②方案新增静态标志亦为文件内、单线程语义（与既有 `s_ready` / `s_seq` 同型） |

> ⚠️ **注（`L-2` 的边界）**：产出器只需**知晓「有一个开关」**（调用 getter），**不得**自行解析 JSON、不得持有 `JsonDocument`、不得缓存文件内容。**键名/默认值/类型**等配置知识**必须**封装在 ConfigManager 侧（M-2），产出器只消费布尔结果 ⇒ 该边界应在实现评审中逐条核对。

---

## §10 验证矩阵

| ID | 场景 | 前置 | 操作 | 期望 | 判据 |
|---|---|---|---|---|---|
| `V-C1-11` | **编译门关闭** | `GF_TELEMETRY_ENABLED=0` | 构建 + 运行 | 无任何 telemetry 上行；代码不进入镜像（空实现） | 订阅 `/up` 无 telemetry 信封；构建体积 ≈ 基线 |
| `V-C1-12` | **编译门开启 + 开关 false** | 门禁 ON · 键 = `false`（或**缺键**） | 构建 + 烧录 + 运行 | **零上行**；`emitted` 不增长 | MQTT 无 telemetry 信封 ≥10 min；D1 `telemetry_history` 不变 |
| `V-C1-13` | **编译门开启 + 开关 true** | 门禁 ON · `config set mqtt telemetry_enable true` + `save` | 重启后观察 | **开始上行**：`/up` 出现 5 字段信封 | 信封形态 ✅ · 规则 A 触发 ✅ · Worker `200` ✅ · D1 首行 ✅ |
| `V-C1-14` | **配置缺失** | 删除该键（或老配置文件） | 重启 | 视为 `false` ⇒ 零上行 | 同 `V-C1-12` |
| `V-C1-15` | **配置损坏** | 人为损坏 `/config/mqtt.json`（Active） | 重启 | 回退 Backup / 或默认值 ⇒ **不发送**；设备可正常启动 | 设备启动成功 + 零上行 |
| `V-C1-16` | **重启后持久化** | 开关 = `true` 且**已 `save`** | 断电重启 ≥2 次 | 每次重启后**仍发送**（值来自文件） | 重启后首个信封出现；值未被重置 |
| `V-C1-17` | **未 `save` 的临时值** | 开关改 `true` 但**不** `save` | 等待 5 min | 自动 save + 重启后**生效**（与既有语义一致） | 5 min 后重启且生效 |
| `V-C1-18` | **关闭回退** | 开关 = `true` 运行中 | `set false` + `save` + 重启 | **恢复到零上行**；无需重烧 | 无新信封；D1 停止增长 |
| `V-C1-19` | **异常类型** | 把键改为字符串/数字 | 重启 | 视为 `false`（E-2） | 零上行 + 告警可观测 |
| `V-C1-20` | **资源复核** | — | clean build（门禁 ON + 本改动） | Flash 增量在 §9.2 估算区间内；G-1/G-2 仍满足 | 构建输出数值记录在案 |

> ★ **必测组合**：`V-C1-11` ↔ `V-C1-12/13` 构成「编译门 × 运行时门」二维矩阵的四个角，**四角必须全绿**才算本规格验收通过。

---

## §11 回退策略

| 层 | 回退动作 | 影响 |
|---|---|---|
| 运行时 | `config set mqtt telemetry_enable false` + `save` + 重启 | 立即停止上行；**无需重烧**（本规格核心收益） |
| 固件 | 重烧 `GF_TELEMETRY_ENABLED=0` 的镜像（或上一版） | 彻底移除能力；`telemetry_history` 停止增长 |
| 配置 | `config reset_module mqtt`（factory 恢复） | 回到出厂值 ⇒ 缺键 ⇒ `false`（fail-closed） |
| 数据 | 无需回退（只增表；幂等索引吸收重复） | — |

---

## §12 GO / NO-GO 建议

| 维度 | 评估 |
|---|---|
| 目标达成 | ✅ 可实现「编译门 ON + 运行时开关」的冻结语义（两道门全成立） |
| 改动面 | 🟢 小：1 getter（声明 + 实现）+ 1 判定点；（可选）+1 计数 |
| 分层合规 | ✅ 全部满足（§9.3）；产出器只消费布尔、不碰存储 |
| 冻结一致性 | ⚠️ **方案 A 为布局层修订**（字段集 / 默认值 / 运行行为不变）⇒ **须人工确认 `HD-1b`**；方案 B 完全一致但需改 ConfigManager 键语义（风险高） |
| 资源 | 🟢 估算 +30~150 B Flash / +0~4 B RAM；G-1/G-2 仍满足（须实测） |
| 可验证性 | ✅ 10 项验证用例，含二维矩阵四角 + 异常路径 |
| **结论** | ✅ **`GO`（条件：`HD-1b` 与 `HD-1c` 先裁决；实现后须实测资源并回填 §9.2）** |

---

## §13 需人工确认事项

| ID | 事项 | 选项 | 说明 |
|---|---|---|---|
| **`HD-1b`** | **开关的承载方式**（**必须先定**） | **A** 扁平键 `"telemetry_enable"`（推荐 · 零 ConfigManager 改动）· **B** 冻结原文嵌套对象（需扩展 ConfigManager 点号路径语义）· **C** 独立 `telemetry.json` 模块 | A/C 属**布局层冻结修订**（字段集 / 默认值 / 运行行为不变），须留痕；B 完全贴合冻结但**影响面覆盖所有模块的键语义** |
| **`HD-1c`** | **读取时机** | **① 每轮现读（推荐）** · ② `init` 快照 | ①无状态、`config_reload` 即时一致；②loop 更省但需重启才反映改动（与 F-7 自洽） |
| **`HD-1d`** | 是否**同时**创建 `interval_ms` 占位键 | 是 / 否 | 本包**不消费**它（`O-1`）；若创建，仅为保持与 `H-WP4-FW-06` 的字段集一致（防后续 `HD-2` 二次修订） |
| **`HD-1e`** | 是否在 `TelemetryStats` 增补「disabled」计数 | 是 / 否 | 影响 `telemetry_publisher.h`；建议「是」（提升可观测性） |
| **`HD-1f`** | 是否**同批**做 `Phase C-C1` 审查发现的**文档漂移修正**（`P-5` / `P-6`） | 是 / 否 | 纯 docs 同步（实施记录 7 字段/320 B、代码头注释 `OPEN` 残留），可与本包合并批或独立批 |

---

## §14 参考（权威文档）

| 文档 | 用途 |
|---|---|
| `docs/review/Phase-C-C1-Telemetry-Publisher-Implementation-Review.md` | 本规格的**上游审查**（`HD-1` 由来 · 架构/协议/资源结论） |
| `docs/review/P3-WP4-Firmware-Telemetry-Decisions.md` **§H-WP4-FW-06** | 配置模型冻结（字段集 / 默认值 / 运行行为 / Non-goals） |
| 同上 **§H-WP4-FW-08** | 资源预算覆盖裁决 + Guard G-1…G-4 |
| `docs/interfaces/cloud_protocol.md`（§ 配置命令 · § telemetry） | 配置命令语义与 `/up` telemetry 载荷形态 |
| `src/services/config_manager.h` | 配置唯一入口接口契约（模块名 / 命令 / 生命周期 / 返回值语义） |
| `src/services/telemetry_publisher.{h,cpp}` · `telemetry_policy.h` · `telemetry_envelope.h` | 产出器现状（本规格的唯一改动对象） |

---

## §15 实施结果回填（`2026-10-13` · 已实施）

> ★ 本节由**实施阶段**追加（`§1–§14` 保持为「实施前」快照）。
> **裁决记录**：`HD-1b` = **A**（扁平键 `telemetry_enable`）· `HD-1c` = **① 每轮现读** ·
> `HD-1d` = **否**（未创建 `interval_ms` 占位键）· `HD-1e` = **本轮不增补** disabled 计数
> （保持既有 `TelemetryStats` 四计数语义不变）· `HD-1f` = **否**（文档漂移另批）。

### 15.1 变更文件（In Scope · 5 个 · 全部零接口语义变更）

| 规格项 | 文件 | 改动 |
|---|---|---|
| **M-1** | `src/services/config_manager.h` | +8：`config_get_telemetry_enable()` 声明 + 契约注释 |
| **M-2** | `src/services/config_manager.cpp` | +13：getter 实现（一行式 `module_ro(CONFIG_MODULE_MQTT)["telemetry_enable"] \| false`） |
| **M-3** | `src/services/telemetry_publisher.cpp` | +13：`#include "services/config_manager.h"` + `telemetry_task()` **最前置**判定 |
| — | `src/services/telemetry_publisher.h` | ±1：`H-WP4-FW-06` 头注释由「仍 `OPEN`」校正为「已接入」（**仅注释**，零编译影响） |
| **M-5** | `data/config/mqtt.json.example` | +1：`"telemetry_enable": false`（可发现性；`false` = fail-closed 安全默认） |

**零改动**：`main.cpp` · topic / Envelope / CloudManager 传输 · Worker / EMQX / D1 / migration · 命令协议 · 模块注册 · 新增文件/模块。

**`telemetry_task()` 判定顺序**（实现）：`运行时开关==false ⇒ return`（最前置）→ `s_ready` → `cloud_is_connected()` → 通道遍历 → 每轮 ≤1 条。
既有 4 步链路与 4 个诊断计数**语义逐字未动**；关闭时**零副作用**（不进入任何计数与遍历）。

### 15.2 实测资源（clean build · 三个变体 · 独立构建目录）

| 变体 | PlatformIO Flash | `firmware.bin` | `.flash.text` | `.dram0.data` | `.dram0.bss` | PlatformIO RAM |
|---|---|---|---|---|---|---|
| **A** 编译门 OFF（默认） | 1,391,897（66.4 %） | 1,392,256 | 1,023,471 | 24,072 | 108,632 | 132,704（40.5 %） |
| **B0** 编译门 ON · HEAD（本改动**前**） | 1,393,525（66.4 %） | 1,393,888 | 1,024,835 | 24,072 | 108,680 | 132,752（40.5 %） |
| **B** 编译门 ON · 本改动**后** | 1,393,617（66.5 %） | 1,393,984 | 1,024,911 | 24,072 | 108,680 | 132,752（40.5 %） |

| 增量 | Flash | RAM |
|---|---|---|
| **`HD-1` 自身（`B − B0` · 同树对照）** | **+92 B**（`bin` +96 B） | **+0 B**（`.data` / `.bss` 逐字节不变） |
| 整个 telemetry 能力（`B − A`） | +1,720 B | +48 B |

- ✅ **落在 §9.2 估算区间内**（+30 ~ +150 B Flash / +0 ~ +4 B RAM）。
- ✅ `HD-1c` = ① 每轮现读 ⇒ RAM **+0 B**（无静态标志），与预判一致。
- ✅ 编译门 OFF 变体**不含该 getter 调用点** ⇒ `--gc-sections` 剔除，**零代价**（与 `F-9` 自洽）。
- ⚠️ 规格 §9.2 引用的「`1,394,373 B`（门禁 ON 现值）」为**更早树状态**的实测值（其间另有 `src` 变更）⇒ 与当前树**不可直接相减**；`HD-1` 的**严格增量 = +92 B**（`B0→B` 同树对照）。
- 构建日志 `error:` 计数：**A / B0 / B 均为 0**；末尾 `SystemExit` 为沙箱 `atexit` 清理保护，**非编译失败**（已知环境现象）。

### 15.3 验证矩阵执行状态（`§10` 逐项）

| ID | 场景 | 本轮状态 |
|---|---|---|
| `V-C1-11` | 编译门关闭 | ✅ **已验（构建级）**：OFF 变体 0 error；telemetry 代码不入镜像（`bin` 1,392,256 vs ON 1,393,984） |
| `V-C1-12` | 门开 + 开关 false | ⏸ **待硬件** |
| `V-C1-13` | 门开 + 开关 true | ⏸ **待硬件** |
| `V-C1-14` | 配置缺失 | ⏸ 待硬件（语义静态可证：缺键 / 模块未加载 ⇒ 空文档 ⇒ `false` · §4） |
| `V-C1-15` | 配置损坏 | ⏸ **待硬件** |
| `V-C1-16` | 重启持久化 | ⏸ **待硬件** |
| `V-C1-17` | 未 `save` 的临时值 | ⏸ **待硬件** |
| `V-C1-18` | 关闭回退 | ⏸ **待硬件** |
| `V-C1-19` | 异常类型 | ⏸ 待硬件（语义静态可证：非 `bool` ⇒ `\| 默认值` 得 `false` · §4/E-2） |
| `V-C1-20` | 资源复核 | ✅ **已验**：见 §15.2 |

> 「编译门 × 运行时门」四角中，**编译门两角（`V-C1-11` / `V-C1-20`）已在构建层闭合**；运行时两角（`V-C1-12` / `V-C1-13`）须待设备烧录后闭合。

### 15.4 遗留 / 待人工

- **烧录与上板验证**（`V-C1-12`…`V-C1-19`）—— **未执行**（本轮零烧录、零配置变更）。
- **`HD-1e`**（disabled 计数）—— 未实施；如需可观测性另批。
- **`HD-1f`**（`Phase C-C1` 审查的 `P-5`/`P-6` 文档漂移）—— 未处理。
- **`HD-2`**（`interval_ms` 消费）—— 本包未触碰；扁平键名 `telemetry_interval_ms` 预留待裁。

---

*本文件于 **`2026-10-13`** 建立，为 **`Phase C-C1 · HD-1`** 的**实施规格**（`SPEC ONLY`）。
基线 = 固件仓 **`d2d6619`**（tag `wp4c-closure-20261012` = `dd78a29`）。
**`§1–§14` 为实施前快照（不含代码改动）；`§15` 为 `2026-10-13` 实施结果回填。***
