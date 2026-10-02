# P0 Final Review —— 设备身份锚点 + Topic V3 隔离

> **状态**：P0 已完成（**完成范围 = 设备标识 + 访问隔离**；**不含设备真实性认证**）、已上板、已端到端验证、**已冻结**
> **设计依据**：`docs/architecture/P0-设备身份与Topic隔离设计.md`（P0-Design v1.2, FROZEN）
> **施工工单**：`docs/architecture/P0-实现清单.md`
> **撰写日期**：2026-10-02
> **本文用途**：P0 的**验收快照** —— 记录"已冻结的基线是什么"、"验证到什么程度"、
> "哪些限制是**已知且有意保留**的"。P1 规划以本文为起点。
> **⚠️ 阅读提示**：本文档同时**显式声明 P0 的安全边界**（§0 速览 · §6 详述）。
> **请勿**据此认为 P0 已完成"设备身份认证 / 防伪造 / 防克隆" ——
> P0 的 `device_id` 是**设备标识符**，**不等同于安全身份凭据**。

---

## 0. 完成范围与安全边界（速览）

> ⚠️ **本文档的全部结论仅覆盖下列范围。** P0 解决的是 **「设备标识」与「访问隔离」**问题，
> **不是「设备身份认证」问题**。外部评审请按此边界理解，避免高估 P0 的安全能力。

### 0.1 P0 已完成

| # | 项 | 含义 |
|---|---|---|
| 1 | **Device identity anchor（设备身份锚点）** | 设备侧存在一个跨重启稳定的 `device_id`，存于 NVS（namespace `gfid`） |
| 2 | **`device_id` 持久化** | 写入 NVS；重启 / 固件替换不改变（`did` 优先，**禁止自动覆盖**） |
| 3 | **MQTT `client_id` 唯一化** | `dev_<device_id>`，修复固定 `client_id` 导致的 session takeover 互踢 |
| 4 | **Topic V3 device-scoped isolation** | `guo_feeder/<device_id>/{down,up,log}`，一台设备的命令只进自己的 Topic |
| 5 | **ACL 基础隔离** | 逐设备枚举授权 + 兜底 `# deny` 白名单；越权订阅被拒（SUBACK `0x80`） |

### 0.2 P0 未完成（**明确不在 P0 范围**）

| # | 项 | 说明 |
|---|---|---|
| 1 | **device authenticity（设备真实性认证）** | 无法证明"声称 `device_id=X` 的确实是 X 那台物理设备" |
| 2 | **per-device credential** | 设备账号仍为**共享凭据**（见 §2 `L-1`） |
| 3 | **credential 与 `device_id` 绑定** | 凭据与设备之间**不存在**任何绑定关系 |
| 4 | **防设备克隆** | 复制 NVS 内容即可得到相同 `device_id`，固件无法识别 |
| 5 | **生命周期管理** | 注册 / 绑定 / 解绑 / 退役 / 凭据回收均未实现 |
| 6 | **identity migration** | 身份迁移流程不存在（见 §2 `L-4`） |

### 0.3 ★ 核心边界（务必按此理解）

> **P0 的 `device_id` 是「设备标识符」（identifier），不等同于「安全身份凭据」（credential）。**
>
> 它是一个**公开的、可从芯片 MAC 推导的、可被复制的**标识，唯一的作用是把消息
> **分门别类投递到正确的 Topic**；它**不承担**"证明设备是谁"的职责。
> ⇒ **把标识符当作身份凭据使用是不安全的。**

---

## 1. P0 Final Baseline

> **范围提示**：本节描述的是**设备身份锚点（identity anchor）**的冻结基线 —— 即"设备有一个稳定标识"，
> **不含设备真实性认证**。安全边界速览见 §0，详述见 §6。

### 1.1 Identity（设备身份锚点 · identity anchor）

| 项 | 冻结值 |
|---|---|
| `device_id` | **MAC 派生**（`aabbccddeeff`，12 位小写 hex，无分隔、无前缀） |
| 派生来源 | **`esp_read_mac(mac, ESP_MAC_WIFI_STA)`**（固定用 STA MAC；**不用** `WiFi.macAddress()`） |
| 存储 | **NVS**，复用现有 `nvs` 分区（`0x9000`, `0x5000`） |
| namespace | **`gfid`** |
| keys | **`did`**（ASCII 字符串 / 12 字符 / 小写 hex）· **`dver`**（`uint8` 派生算法版本） |
| 身份层级 | `did` = **设备标识的唯一依据**（**标识符，非安全凭据**）；`dver` **只表示派生算法版本**，**不参与身份判定** |

**标识三层来源（不做等号关系）**

| 角色 | 职责 |
|---|---|
| MAC | 初始生成来源 + 恢复来源 |
| NVS (`gfid/did`) | **运行期标识锚点（identity anchor）** —— 是标识，不是凭据 |
| 云端注册后的 `device_id` | **最终业务标识依据** —— 仍然只是标识，不是凭据 |

**生命周期语义（5 场景）**

| 场景 | 行为 |
|---|---|
| 首次启动（无 `did`） | 由 MAC 派生 → 写入 NVS |
| 后续启动 | **以 NVS 为准**（不回写、不重派生） |
| NVS 丢失 | **在同一芯片 MAC 未变化、且未发生 identity migration 的条件下，可重新恢复相同 `device_id`** |
| 芯片替换 | 属**身份迁移**场景，需单独流程 |
| 身份迁移 | **P0 不实现**；只允许显式 identity migration |

**硬约束（DV-1..DV-3）**：`dver` 不匹配**禁止**自动重新生成 `device_id` · **禁止**自动覆盖已有 `did` ·
**普通 OTA/固件升级不得改变 `device_id`**；读到未知 `dver` ⇒ 保留 `did` + WARN + 不阻断启动。

### 1.2 MQTT

| 项 | 冻结值 |
|---|---|
| `client_id` | **`dev_<device_id>`**（唯一化 —— 修复"固定 client_id ⇒ 多设备 session takeover 互相踢线"） |
| Topic | **`guo_feeder/<device_id>/{down,up,log}`**（3 级，前缀保留 `guo_feeder`） |
| 设备订阅范围 | **只订阅自己的 `down`**（禁止 `guo_feeder/+/down` / `#`） |
| 渲染 | **配置存模板 + `topic_render()` 运行期渲染**；**渲染点全工程唯一**（`src/services/topic_renderer.*`） |
| 渲染契约 | 含 `<device_id>` ⇒ 全部替换 · **不含 ⇒ 原样放行 `true`（配置级回滚通路）** · `device_id` 不可用 ⇒ `false` · 渲染后含 `<>`/控制字符/长度 > 128/空 ⇒ `false` |
| 失败语义 | 渲染失败 ⇒ ERROR + 回落内建 V3 模板；**宁可连不上，也不用共享 Topic 静默跑起来** |

> **回滚通路的价值**：把模板改回 `guo_feeder/down` 即可回滚到 legacy，**无需回退固件**（仅 dev/test 允许；生产按设计 §3.3.1 必须含 `<device_id>`）。

### 1.3 ACL

| 项 | 冻结值 |
|---|---|
| 模式 | **device-scoped topic ACL**（逐 `device_id` 枚举，禁用通配） |
| 规则文件 | `EMQX_Assets/config/acl-rules.v3.json`（+ `apply-acl.mjs --device-ids`） |
| 兜底 | `全部用户 → # → deny`（**白名单语义的根基，不触碰**） |
| 设备账号规则 | 3 条 V3（sub `down` / pub `up` / pub `log`）+ 3 条 legacy（**过渡期**，仅 dev/test 语义） |
| current limitation | **共享凭据（shared credential）** —— 详见 §6。① 两个设备账号被多台设备共用 ⇒ P0 阶段 ACL 层**无法隔离两台设备**；② **合法设备仍可以使用任意 `device_id` 上报**；③ **Topic ACL 可以限制账号的访问范围，但无法证明消息来源设备的真实性** |
| 未使用 | `homie/5/<device_id>/#`（P2 预留，**未添加**） |

> **★ ACL 隔离的依赖关系与真实边界（重要）**
>
> **P0 的 ACL 隔离依赖两个要素：`device_id` + ACL 规则。**
> 但由于**共享 MQTT credential** 的存在：
> **攻击者一旦获得共享账号密码**：① **可以连接 broker**；② **可以尝试伪造 `device_id`**
> （ACL 只按 `username` + Topic 判定，无法识别"这条消息是不是真从那台设备来的"）。
>
> ⇒ **P0 解决的是「正常设备之间的 Topic 串扰」**；
> **P1 解决的是「凭据泄露情况下的设备身份绑定」**（per-device credential + 注册 + 生命周期）。

### 1.4 构建环境（C-3 固定）

`platformio.ini` 已固定：`espressif32@7.0.1` + `platform_packages`（framework/toolchain/tools 全锁）
+ `lib_deps` 精确版本 ⇒ 重新 clone 后 `pio run` 结果一致。

---

## 2. Known Limitations（**已知且有意保留**，非缺陷）

| # | 限制 | 说明 / 归属 |
|---|---|---|
| L-1 | **无 per-device credential（共享凭据）** | 当前共享 MQTT credential 下：① **合法设备仍可以使用任意 `device_id` 上报**；② **Topic ACL 可以限制账号的访问范围，但无法证明消息来源设备的真实性**。⇒ 既无法在 ACL 层区分两台设备，也无法识别冒充来源。**由 P1 解决**：**per-device credential** + **credential lifecycle** + **device registration**（见 `P1-Cloud-Device-Lifecycle-Plan.md`） |
| L-2 | **无 production mode enforcement** | 设计 §3.3.1 的"**生产必须含 `<device_id>`，否则阻止 MQTT 上线**"是**设计目标，不是 P0 已实现能力**。P0 **已具备**：① **渲染失败保护**（判 `false`、不静默退回共享 Topic）· ② **V3 fallback**（失败回落内建 V3 模板）· ③ **配置约束**（模板常量单点定义）。P0 **未实现**：**production mode enforcement**（主动阻止生产环境误配 legacy topic）—— **已裁决纳入 P1**（P1-7，见 `P1-Cloud-Device-Lifecycle-Plan.md`） |
| L-3 | **legacy ACL 仍存在** | `guo_feeder/{down,up,log}` 三条过渡期保留（配置级回滚需要）；生产前需收窄/移除 |
| L-4 | **无 identity migration** | 身份迁移（换主控 / 云端重新绑定）无流程；P0 只保证"不自动破坏 `device_id`（标识）" |
| L-5 | **无 OTA 模块** | 固件当前**不含任何 OTA 代码**；"升级不改 `device_id`"只能用重烧 app 分区等价验证 |
| L-6 | **遥测未实现** | D7 只冻结策略表（目标 < 1200 行/设备/天）；上报功能随 `readme.md` §六 Device State Heartbeat 落地，落点预登记 `mqtt.json.telemetry` |
| L-7 | **S-5 / S-9 在正常软件运维路径不可达** | 见 §3.3 —— 属**软件路径防误操作能力**的证据，**不代表安全存储保证**，亦非"绝对不可发生" |

---

## 3. Verification

### 3.0 P0 验证范围声明

> **⚠️ 本节只描述 P0"验证到什么程度"。P0 不是完整的设备安全体系。**

**P0 验证覆盖**

- 固件身份生成与持久化逻辑（`device_identity`：MAC 派生 / NVS 锚点 / 启动决策表）
- MQTT Topic 渲染逻辑（`topic_renderer`：占位符替换 / 长度与字符集校验 / 失败回落）
- `client_id` 唯一化（`dev_<device_id>`）
- ACL 隔离行为（device-scoped 授权 + 兜底 `# deny`；越权订阅被拒）
- **单设备**端到端通信（订阅生效 / 下发命令 / 回包）

**P0 未覆盖**

- 多真实设备长期运行测试（M-2 双机互踢实测因缺第二台设备未做）
- 生产规模设备注册
- 一机一凭据（L-1，属 P1）
- 身份迁移（L-4）
- OTA 升级体系（L-5，固件当前无 OTA 代码）
- 物理攻击模型（脱机读写 flash / NVS 分区 / 芯片级操作）

⇒ **请勿把 P0 解释为"完整设备安全体系"**：P0 验证的是"**标识稳定 + Topic 不串台 + 单设备能通**"，
其余维度由 P1 及后续阶段覆盖（见 §6）。

---

### 3.1 提交

| 包 | commit | 内容 |
|---|---|---|
| **P0-1** | `9825b09` | DeviceIdentity 模块（`src/services/device_identity.{h,cpp}` + `main.cpp` 接入） |
| **P0-2** | `5c4ffb6` | TopicRenderer 模块（`src/services/topic_renderer.{h,cpp}` + `config_manager` 4 处兜底改模板） |
| **P0-3** | `607b16d` | CloudManager 接入渲染 + `client_id` 唯一化（`cloud_manager.cpp` 单文件） |
| **P0-4** | `17e459c` | ACL V3（`EMQX_Assets`：`acl-rules.v3.json` + `apply-acl.mjs`），**已 apply 到线上** |
| **P0-5** | `9e07d6d` | 上板用例集 + 4 个 MQTT 工具参数化（`--device-id`） |
| 后续 | `249f359` `579ceaf` `424d658` `23af6d0` | 文档漂移修正 · P0 设计与实施清单 · 测试修复 · HANDOFF 验收块 |

### 3.2 资源增量（实测）

| 包 | RAM | Flash |
|---|---|---|
| P0-1 | +80 B | +4272 B（Preferences/NVS 首次链接，一次性） |
| P0-2 | **+0 B** | +160 B（渲染器尚无调用方，被 `--gc-sections` 剔除） |
| P0-3 | +8 B | +2608 B |
| **合计** | **+88 B** | **+7,040 B**（RAM 39.9% / Flash 65.8%） |

证据方法：两份 ELF 的 `nm -S` **归一化符号 diff**（`type|size|name`）+ `objdump -h` 段级归因。

### 3.3 上板验收（COM8 实机）

| 用例 | 结果 |
|---|---|
| S-1 探活 · S-2 首次启动 · S-3 重复启动 | ✅ |
| S-4 NVS 丢失 → **恢复相同 `device_id`**（条件见下） | ✅ |
| S-6 topic 渲染（`client_id` + 三 topic 全 V3，零 WARN） | ✅ |
| S-7 回滚通路（legacy 模板 ⇒ 原样放行 + 一次 WARN，仅 dev/test） | ✅ |
| S-8 `client_id` 实值（`dev_<device_id>`；配置回包**不含** `device_id` 字段） | ✅ |
| S-10 命令链路回归 · S-11 升级（重烧 app 分区）后 `device_id` 不变 | ✅ |
| M-1 单设备上线 + MQTT 下发（memory / flash / config_query） | ✅ |
| M-3 越权被拒（设备账号 SUBACK = `(1,128,1)`：自己 down ALLOW / 他人 down DENY） | ✅ |
| **S-5 `did` 与 MAC 不一致** | ⛔ **正常软件运维路径不可达**（下） |
| **S-9 `client_id` 超长** | ⛔ **正常软件运维路径不可达**（下） |
| M-2 双设备互踢 | ⏳ 无第二台设备 |

**S-4 的条件（严格表述）**：NVS 丢失后能恢复相同 `device_id`，**仅在「同一芯片 MAC 未变化」
且「未发生 identity migration」的条件下成立**；该"同值"**不是无条件成立**的
（芯片替换 ⇒ 属身份迁移场景，见 §1.1 生命周期语义）。

**S-5（`did` 与 MAC 不一致）** —— 结论需分两层表述：

1. **在正常固件接口与正常运维流程下不可达**：等长篡改 NVS 镜像中的 `did` 后写回 ⇒
   设备报 `nvs_get_str fail: did NOT_FOUND`（**NVS 自带完整性校验**，篡改条目被判无效）；
   且固件**不提供任何修改 `did` 的接口**（设计明确禁 `device_identity_reset`）。
2. **但从物理攻击、NVS 分区离线修改、芯片级操作等角度，不应定义为"绝对不可发生"** ——
   P0 **未做**物理防篡改与安全芯片级身份保护的验证。

> **★ P0 对身份的保证究竟是什么（务必准确理解）**
>
> **不是**"无法改变身份"，**而是**：
> "**固件没有提供自动改变身份的路径；检测到身份冲突时不会自动覆盖已有身份。**"
>
> **P0 的身份安全边界**
>
> | 维度 | P0 是否负责 |
> |---|---|
> | 防止**软件路径**导致身份漂移（正常命令 / 配置接口改不动 `did`） | ✅ **负责** |
> | 物理防篡改（脱机读写 flash / 离线修改 NVS 分区） | ❌ **不负责** |
> | 安全芯片级身份保护（eFuse / 安全启动 / flash 加密等） | ❌ **不负责** |

**S-9（`client_id` 超长）** —— 结论需分两层表述：

1. **当前 CommandManager / ConfigManager 路径会提前拒绝非法长度**：
   （`CONFIG_ENQUEUE_INVALID` → `{"e":6,"m":"invalid config param or unsupported value type"}`，
   随后 `config_save` 报 `no dirty module`）⇒ 值根本没写入设备。
2. **`topic_renderer` 自身也具备运行时防御检查**：渲染后校验残留 `<` / `>`、控制字符、
   长度 > 128、空串 ⇒ 判 `false` 并回落内建 V3 模板。
   该分支由编译期自检 `topic_renderer_selftest()` 的 `too_long` 用例覆盖（DEBUG 构建实测 PASS）。

> **两层保护共同保证当前路径安全。**
> 但**这不代表所有未来配置入口都天然满足该约束** ——
> **未来新增任何配置入口，必须复用 `ConfigManager` 或 `TopicRenderer`，不得绕过这两层校验。**

> **⚠️ 结论强度的限制（勿过度解读）**：上述两项结论均来自**软件路径**的验证，
> **未进行物理存储攻击或 NVS 数据破坏实验**（例如直接擦写 flash、脱机读取/伪造 NVS 镜像、
> 降级固件绕过校验等）。⇒ 该结果**证明的是"软件路径具有防误操作能力"**（正常的命令 / 配置接口
> 不会把设备改坏），**不代表"安全存储保证"**。能物理接触设备的攻击者**不在本次验证范围内**；
> 这与 §3.0「P0 验证范围声明」与 §6「P0 Security Boundary」一致。

### 3.4 端到端（P0 的核心目标）

| 验证 | 结果 |
|---|---|
| 设备订阅 V3 topic 是否真的生效 | EMQX clients 中 `subscriptions` **0 → 1**（ACL 修复 + 重连后） |
| 经 MQTT 下发命令 | `guo_feeder/<device_id>/down` 下发 memory / flash / config_query ⇒ **设备正确执行并回包** |
| 越权被拒（Topic 访问控制） | 设备账号订阅**他人** `down` ⇒ **DENY(0x80)** |
| 双设备串扰（P0 要修的问题） | V3 后一台设备的命令**只**进自己的 topic（M-2 双机实测待补） |

---

## 4. 排障铁律（P0 期间实测得出，务必保留）

1. **`[Cloud] MQTT subscribed` 日志 ≠ 订阅被授权**。白名单模式下未显式 allow 的 topic 落到兜底 `# deny`
   ⇒ EMQX clients 里 `subscriptions=0`（设备收不到任何下行），**但固件照样打 `subscribed`**
   （那只是协议层 SUBACK）。**判据只能是 EMQX 的 `subscriptions`**。
2. **ACL 改完不会让已建立的连接自动重订阅** ⇒ 必须让设备重连；
   无串口时可用 `DELETE /clients/{clientid}` 踢连接（实测 204，设备自动重连重订阅）。
3. **串口命令必须 `cm {JSON}` 包装**（`cmd`/`ob`/`id`/`p` 四键，与 MQTT 下行的 `c`/`i`/`p` 不同）；
   且每条用例**必须换新 `id`**（cmd_id 去重缓存 10 项 / 30 s，重复 id 被静默丢弃）。
4. **首启/擦除后必现 `[E][Preferences] did NOT_FOUND`** —— 属预期路径（无 `did` ⇒ 派生并写回）。

---

## 5. 遗留事项

| # | 事项 | 归属 |
|---|---|---|
| 1 | M-2 双设备互踢实测 | 需第二台设备 |
| 2 | 授权缓存生效延迟实测（`A-1`） | P1（签发时序的前提） |
| 3 | `client_id` broker 长度上限实测（`A-4`） | P1 |
| 4 | 一机一凭据 + credential lifecycle + device registration（消除 L-1） | **P1 核心** |
| 5 | production mode enforcement（消除 L-2）：**防止生产环境误配置 legacy topic** | **P1-7**（已裁决纳入 P1） |
| 6 | legacy ACL 收窄（消除 L-3） | 以"P1 凭据 + P3 落库"完成为终点 |

---

## 6. P0 Security Boundary

> 本节是 P0 的**安全边界声明**，用于避免外部评审高估 P0 的安全能力。
> 结论先行：**P0 是「设备标识 + 访问控制」语义，不是「身份认证」语义。**

### 6.1 P0 解决

| 问题 | 解决方式 | 验证 |
|---|---|---|
| **多设备 MQTT Topic 串扰** | Topic V3：`guo_feeder/<device_id>/{down,up,log}`，一台设备的命令只进自己的 Topic | M-1 端到端下发成功；M-3 订阅**他人** `down` ⇒ **DENY(0x80)** |
| **固定 `client_id` 导致 session takeover** | `client_id` 唯一化为 `dev_<device_id>` | S-8 实测回包含 `dev_`；S-6 渲染零 WARN |
| **Topic 模板漂移** | 渲染点**全工程唯一**（`topic_renderer`），5 个模板宏单点定义 | 生产 ELF 不含自检串（负向检查）；S-6 / S-7 实测 |

> 归纳：P0 解决的是 **"消息该投给谁"** 与 **"设备标识稳不稳"**。

### 6.2 P0 不解决

| # | 未解决的问题 | 后果 |
|---|---|---|
| 1 | **谁是真实设备**（device authenticity） | 无法证明"声称 `device_id=X` 的就是 X 那台物理设备" |
| 2 | **credential 是否属于该设备** | 凭据与设备之间**无绑定关系**；共享账号可被任意设备使用 |
| 3 | **credential 泄露后的影响范围** | 共享凭据一旦泄露，**影响所有使用该账号的设备**，且**无法按设备单独吊销** |
| 4 | **设备注册与解绑** | 无注册表、无绑定、无解绑 / 退役流程（见 L-1 / L-4） |

**当前共享凭据下的具体风险（务必如实理解）**

1. **合法设备仍可以使用任意 `device_id` 上报** —— 只要持有共享账号，就能把消息发到
   任意 `guo_feeder/<任意 device_id>/up`。约束仅来自 ACL 的**账号级 Topic 范围**，而非设备身份。
2. **Topic ACL 可以限制账号的访问范围，但无法证明消息来源设备的真实性** ——
   ACL 的判定维度是 `username`，不是"哪台物理设备"。
3. ⇒ 因此 P0 的隔离是 **"访问控制"语义**，**不是"身份认证"语义**。

> **★ 一句话分界**：**P0 解决「正常设备之间的 Topic 串扰」**；
> **P1 解决「凭据泄露情况下的设备身份绑定」**（`credential ↔ device_id` 绑定 + 注册 + 生命周期）。

### 6.3 与 P1 的关系

**P1 的目标是把「设备标识」升级为「设备身份」。**

```
P0（已完成）      设备标识 + 访问隔离
  device_id  ──►  公开 · 可从 MAC 推导 · 可复制  ⇒  只用于"投递寻址"
                 │
P1（规划已冻结）  ▼  设备身份 + 真实性
  device_id + credential（+ 绑定 + 生命周期）
                 ⇒  "声称是谁" 与 "实际是谁" 可被验证
```

P1 通过 **per-device credential** + **credential lifecycle** + **device registration**
（见 `P1-Cloud-Device-Lifecycle-Plan.md`，已冻结）补齐 §6.2 的 4 项。

### 6.4 表述规范（本文档内部约定）

| ❌ 易产生歧义的表述 | ✅ 准确表述 |
|---|---|
| "设备身份安全完成" | "设备身份锚点建立" |
| "身份隔离完成" | "Topic 访问隔离完成" |
| "设备唯一身份" | "设备标识建立" |

---

## 7. 结论

**P0 已冻结 —— 完成的是「设备标识 + 访问隔离」，不是「设备身份认证」。**

设备标识**一机一值、NVS 持久、固件替换不变**；MQTT **一机一 topic、`client_id` 唯一**（消除互相踢线）；
ACL **device-scoped、越权被拒**；端到端下发已验证。已知限制均为**有意保留**（L-1..L-7），
其中 L-1（共享凭据）、L-2（生产模式）、L-4（身份迁移）构成 **P1 的主要输入**。

> **★ 边界重申**：`device_id` 是**设备标识符**，**不是安全身份凭据**。
> 当前**任何合法设备都可以用任意 `device_id` 上报**；ACL 只能限制"账号能访问哪些 Topic"，
> **不能证明消息来自哪台真实设备**。
> **P1 的目标就是把「设备标识」升级为「设备身份」。**
