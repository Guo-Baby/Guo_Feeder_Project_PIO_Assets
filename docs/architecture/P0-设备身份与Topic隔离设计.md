# P0 设计文档 · 设备身份 + Topic V3 隔离 + ACL 模型

> **文档定位**：`docs/architecture/Cloud-APP-Platform-Plan.md`（v2）§10 **P0** 的实施级设计。
> Plan 是**架构决策依据**，本文是**可施工的设计**：把 P0 的 7 个子项拆到"文件 / 函数 / 行号 / 验收"级别。
>
> - **状态**：**设计冻结（FROZEN）** · **日期**：2026-10-02 · **版本**：**P0-Design v1.2**
> - **改动范围**：**本文不写任何固件代码**。落地时的代码改动见 §7「改造清单」。
> - **上游依据**：`Cloud-APP-Platform-Plan.md` §3 · §5 · §9 · §13 · §15
> - **关联接口文档**：`docs/interfaces/cloud_protocol.md`（V2.0）· `docs/interfaces/workflow_cloud_interface.md`
> - **标记约定**：**【已定】** 不再讨论 · **【待实测】** 需上板/上云实测 · **【P1+】** 属后续阶段 · **【P0-可选】** 建议一并做但可剥离
>
> ### 修订记录
>
> | 版本 | 日期 | 变更 |
> |---|---|---|
> | v1 | 2026-10-02 | 初版：D1/D2/D3/D5/D7 定案 + 身份 / 渲染 / ACL / 上线 / 验收 |
> | **v1.1** | 2026-10-02 | **设计冻结前最后修订** —— 仅涉及**架构描述 / 决策说明 / 安全边界 / 实施约束**，**未新增 P0 范围、未改动任何技术方案与 D 系列取值**：<br>① 修正 NVS 与 MAC 的身份关系表述（引入「运行期身份锚点」+「云端注册为唯一依据」）<br>② 新增 `dver` 版本迁移**硬约束**（§2.3.1）<br>③ 新增 `device_id` **身份边界**（芯片实例身份 ≠ 物理设备，§2.6）<br>④ 新增 legacy Topic 的**生产环境安全边界 + 启动检查规则**（§3.3.1 / §8.2）<br>⑤ 新增 APP **MQTT `client_id` 生命周期约束**（§1.4.1） |
> | **v1.2** | 2026-10-02 | **实施级修订（仍不改 D1–D7 冻结取值、不新增 P0 范围）**：<br>① **拆分 TopicRenderer**：`topic_render()` 移出 DeviceIdentity ⇒ 新增 `src/services/topic_renderer.{h,cpp}`（§2.4.1）<br>② **删除 `device_identity_reset()`**（身份重置 ≠ factory reset，防误调用致身份漂移）<br>③ **修正 NVS `did` 描述**：ASCII 字符串 / **12 个字符** / 小写 hex / 由 `Preferences` 管理（**禁止写 "12 bytes"**）<br>④ **自检改为 `topic_renderer_selftest()`**（**DEBUG-only**，默认 0，生产不编译不执行；§3.3.2），并**新增 S-11 OTA 身份持久性用例**（DV-3） |

---

## 0. 结论速览（TL;DR）

| 决策 | 取值 | 一句话理由 |
|---|---|---|
| **D1** `device_id` 格式 | **`aabbccddeeff`**（12 位小写 hex，无分隔） | 无分隔便于 Topic 匹配；小写是 Homie 5 硬要求；12 位短且无歧义 |
| **D2** `device_id` 持久化 | **NVS**（复用现有 `nvs` 分区 + **独立 namespace `gfid`**） | 分区表已 100% 分配完（无空闲）；独立 namespace 同样满足"与 ConfigManager 解耦、`update_config` 改不到" |
| **D3** Topic 实现方式 | **方案 A：配置留模板 + 新增 `topic_render()` 运行时渲染** | 可配置 / 可回滚（**改配置即回滚，无需回退固件**）/ 可单测 |
| **D5** APP 凭据模型 | **slot 池 + API 改写 ACL**（`PUT .../rules/users/{username}`） | Serverless 不支持 HTTP auth/authz ⇒ 这是唯一解；**API 路径已实测 200**，脚本已封装 |
| **D7** 遥测落库策略 | **分层策略（事件 100% / 遥测抽样）**，目标 < 1200 行/设备/天 | 容量是首位真瓶颈；**P0 只冻结策略与字段定义，配置项随"心跳功能"落地** |
| 附加：`client_id` | **`dev_<device_id>`**（模板化） | **不是美化，是修缺陷**：固定 `guo_feeder_001` ⇒ 两台设备互相踢线（MQTT session takeover） |

**P0 交付物 = 1 个新模块（2 文件）+ 5 处既有代码小改 + 1 份配置迁移 + 1 套 ACL 规则 + 1 个串口测试用例集。**

---

## 1. 决策记录（含与 Plan 建议的差异）

Plan §15 给出的建议值本轮**全部采纳**，但有 **1 处必须偏离**、**1 处补充**，说明如下。

### 1.1 D1 — `device_id` 格式 【已定】

```
格式：aabbccddeeff        ← 12 个字符，全部小写，仅含 [0-9a-f]
示例：a1b2c3d4e5f6
长度：固定 12，禁止变长（不是"最长 12"）
```

理由：

| 维度 | 说明 |
|---|---|
| Homie 5 兼容 | Homie 规范要求 device-id 为**小写**、不得含大写字母 ⇒ 小写 hex 天然合法 |
| MQTT Topic | 无分隔符 ⇒ 主题层级干净（`guo_feeder/a1b2c3d4e5f6/down`，恒为 3 级） |
| 与 ACL 规则 | EMQX 规则里逐设备枚举时无转义问题；`+`/`#` 通配不会与内容冲突 |
| 与 MAC 的关系 | 直接是 MAC 的 hex，**肉眼可核对**（排障时 `config_query` 与路由器 MAC 表能对上） |
| 大小写 | **强制小写**。理由不是"好看"：ACL 规则是**字符串精确匹配**，大小写不一致 = 静默拒绝（P1 的 `username = dev_<id>` 也同理） |

> ⚠️ **禁止**出现 `gf-` 前缀（Plan 的备选）。理由：一旦引入前缀，`Homie device-id`、`MQTT Topic`、`EMQX username`、`D1 主键`四处的字符串拼接规则就不再一致，属**身份漂移**风险源（Plan §13.3 铁律）。

### 1.2 D2 — 持久化位置 【已定，**与 Plan 建议有偏差，需你确认**】

Plan §15 建议"**NVS 独立分区**"。**实测后必须改为：复用现有 `nvs` 分区 + 独立 namespace。**

**实测事实**（`partitions.csv`，本仓库当前值）：

```
# Name,   Type, SubType, Offset,  Size,    Flags
nvs,      data, nvs,     0x9000,  0x5000,
otadata,  data, ota,     0xe000,  0x2000,
app0,     app,  ota_0,   0x10000, 0x200000,
app1,     app,  ota_1,   0x210000,0x200000,
littlefs, data, spiffs,  0x410000,0xBF0000,   ← 0x410000 + 0xBF0000 = 0x1000000 = 16 MB
```

⇒ **Flash 16 MB 已被 100% 分配，无空闲空间**。新增一个独立分区必须：改写分区表 → 缩小 `littlefs` 或 `app0/app1` → **全片重烧**（`uploadfs` 级别，会丢设备端独有文件）→ 且 OTA 分区的既有镜像布局被打断。

**收益/代价**：

| 方案 | 收益 | 代价 | 结论 |
|---|---|---|---|
| A. 复用 `nvs` 分区 + namespace `gfid` | 零分区改动、零重烧、`Preferences` 开箱可用 | 与 Wi-Fi 驱动共享同一 NVS 分区（不同 namespace，互不可见） | ✅ **采纳** |
| B. 新增独立 `nvs_factory` 分区 | 物理隔离 | 重排分区表、全片重烧、需 `nvs_flash_init_partition()`、littlefs 容量需重算 | ❌ 风险与收益不成比例 |

**更关键的设计论证（身份来源分层 —— v1.1 修正表述）**：

| 层 | 角色 | 说明 |
|---|---|---|
| **MAC**（固定取 `ESP_MAC_WIFI_STA`） | **`device_id` 的初始生成来源 + 恢复来源** | 提供"这一颗芯片实例"的物理唯一性；**只在"首次生成"与"NVS 丢失后的恢复"两种场景被读取** |
| **NVS**（namespace `gfid`） | **运行期身份锚点（runtime identity anchor）** | **正常运行期的 `device_id` 一律以 NVS 为准** —— 它使身份**不随固件版本、配置改动、网络环境变化而漂移** |
| **云端注册后的 `device_id`** | **设备身份的最终唯一依据** | 设备一旦注册/绑定入云，**云端记录即为权威**；设备侧与云端不一致时，**由显式 identity migration 流程裁决**（§2.3.1），**不走"自动重算"** |

> **★ 表述纪律（v1.1）**：**全文禁止**任何把 NVS 描述成「非身份来源 / 非真相来源」的表述（该说法易误导，**已废弃**）。
> **唯一正确**的表述是：**「NVS 保存设备实例身份锚点；MAC 负责首次生成和恢复。」**（三层来源见 §1.2 表 / §2.3 生命周期表）

**为什么不采用"独立分区"**：在**未发生身份迁移、且云端绑定未变化**的前提下，`device_id` 可由 MAC **幂等重建**（§2.3）⇒ 物理隔离对身份数据**没有额外安全收益**，代价却是"重排分区表 + 全片重烧（丢设备端独有文件）" ⇒ 属**过度设计**。

> ⚠️ **该结论有前提（v1.1 补）**：一旦发生过**身份迁移**或**云端绑定变更**，NVS 即成为**设备侧唯一可用的身份来源**（**不可**再由 MAC 重建）⇒ 届时 NVS 必须被视为**需备份的资产**（见 §2.3 / §2.5 `I-4`）。

### 1.3 D3 — Topic 实现方式 【已定】

**方案 A：`data/config/mqtt.json` 保留"模板"，运行时渲染。**

```
配置里写：guo_feeder/<device_id>/down
运行时由新增的 topic_render()（**TopicRenderer 模块**，见 §2.4.1）把 <device_id> 换成实际值
```

补充 Plan 未写明的**第 3 个理由（本轮发现，价值最高）**：

> **方案 A 使"回滚"变成改配置，而不是回退固件。**
> 渲染器对**不含占位符**的模板**原样返回**（§3.3）⇒ 只要把 `mqtt.json` 的 topic 改回 `guo_feeder/down`，设备立刻回到 legacy 共享 Topic 行为，**无需换固件、无需重烧、无需 `uploadfs`**。
> 这让"Topic V3 上线"这件事本身变成**可灰度的**（单设备先上，出问题秒回退）。
> ⚠️ **v1.1 边界**：该"配置级回滚"在**生产环境受 §3.3.1 约束**（生产不允许长期运行共享 Topic）。

### 1.4 D5 — APP 凭据模型 【已定】（P1 实施，P0 需先立 ACL 模型）

**slot 池 + Worker 用 EMQX API 动态改写该 slot 账号的 ACL。**

Plan §5.2 疑问 3（"EMQX API 能否按 username 改写 ACL"）**已实测可通**：

| 端点 | 方法 | 实测 | 封装 |
|---|---|---|---|
| `/authorization/sources/built_in_database/rules/users/{username}` | **PUT**（覆盖该用户全部规则） | ✅ **200** | `EMQX_Assets/scripts/emqx-api.mjs` → `replaceUserAclRules()` |
| `/authorization/sources/built_in_database/rules/all` | GET / PUT（白名单兜底 `deny #` 在这里） | ✅ 200 | `listAllUsersAclRules()` / `setAllUsersAclRules()` |
| `/authentication/password_based:built_in_database/users` | POST（建号）/ GET（列号） | ✅ GET 200；POST 为脚本既定用法 | `createUser()` / `listUsers()` |

> ⚠️ **认证器 id 无法列举**（`GET /authentication` → **403**）⇒ 必须用**常量** `password_based:built_in_database`（已在 `emqx-api.mjs:125` 固化）。

**⚠️ 新增待实测项（P0 只需登记，P1 必须解决）**：EMQX 内置数据库授权的**缓存刷新延迟**。规则 PUT 成功 ≠ 立刻对新/旧连接生效（EMQX 授权缓存通常有 TTL，默认量级约 1 分钟）。⇒ P1 签发时序必须设计为 **"先改 ACL，再返回凭据给 APP 连接"**（而不是"APP 先连、再改 ACL"），并在文档中显式声明可接受的生效延迟。

#### 1.4.1 `username` 与 MQTT `client_id` 是两个维度（v1.1 新增）【已定】

> 背景：P0 已确认**设备端** `client_id` 必须唯一（§1.6）。本节把该结论**推广到 APP 侧**，并明确二者职责边界 —— **混用会导致 session 互相抢占（takeover）**。

| 维度 | 用途 | 谁校验 |
|---|---|---|
| **`username`** | **ACL 身份授权**（`dev_<device_id>` / `app_slot_<n>`） | EMQX 内置认证 + 授权规则 |
| **`client_id`** | **MQTT session 唯一标识**（会话句柄 / retained 会话 / LWT 归属） | MQTT broker（EMQX） |

**规则（硬约束）**：

| # | 主体 | 规则 |
|---|---|---|
| **ID-1** | **APP 客户端** | `client_id` **必须唯一** |
| **ID-2** | **APP 客户端** | **不允许多个客户端共享同一个固定 `client_id`**（否则后连者把先连者**踢下线**） |
| **ID-3** | **APP 客户端** | **不应使用 `username` 作为 `client_id`** —— username 是**共享的 slot 名**（`app_slot_<n>`），多个 APP 必然撞号 |
| **ID-4** | **设备端** | 继续使用 **`dev_<device_id>`**（P0 已定，§1.6）；它天然唯一，因为 `device_id` 唯一 |

**推荐（APP 侧）**：使用**随机 UUID v4**，或由平台在签发凭据时一并生成（与 P1 slot 池的**同一返回体**下发）—— **不需要 P0 实现**。

**长度检查约束（设备端与 APP 端共同适用）**：

| 检查项 | 约束 |
|---|---|
| 上限 | 沿用 §3.3 渲染契约的既有上限 **≤ 128 字符**（**本设计自定，非 broker 上限**） |
| 字符集 | 仅 `[0-9a-zA-Z_-]`（避免 broker / 日志 / URL 转义问题） |
| 超限处置 | **判为配置错误**：设备端走内建兜底 `dev_<device_id>` 并打 ERROR（**不静默截断**）；APP 端由 Worker **拒绝签发**并回错误 |
| ⚠️ broker 侧真实上限 | **未实测** ⇒ 登记为 P1 待确认项（§4.5）；**P0 不依赖它**（`dev_` + 12 字符 = 16，远低于任何合理上限） |

> **为什么放在本节而不新开章节**：这是 D5 凭据模型的**同一枚硬币的另一面**（同一次签发同时给出 `username` + `client_id`）⇒ 属**约束说明**，**不新增 P0 功能**。

### 1.5 D7 — 遥测落库频率策略 【已定（策略），实现随功能】

**分层策略表（冻结）**：

| 数据类别 | 频率 | 落库策略 | 预估行数/设备/天 |
|---|---|---|---|
| 故障 / 安全事件（`EVENT_*` / `VALVE_ERROR` / `0x05xx`） | 低频 | **100% 落库** | < 100 |
| 供水动作（`VALVE_OPEN/CLOSE`、`0x0505`） | 中低频 | **100% 落库** | < 500 |
| 重量（`weight.value`） | 高频 | **阈值 + 抽样**（变化 > X g 才记 / 每 N 分钟一条） | < 300 |
| 水位 / 温湿度 | 高频 | 每 5–15 分钟一条（或超阈值） | < 300 |
| 在线状态（`$state`） | 低频 | **仅状态变化落库** | < 50 |
| 心跳 | 高频 | **不落库**（仅内存/在线态） | 0 |
| **合计目标** | — | — | **< 1200**（11 台 ≈ 13.2k ≈ **13% 额度**） |

**⚠️ 与 Plan 的差异（必须说明）**：Plan §15 D7 措辞是"设备侧降频需 **P0 一起设计配置项**"。本轮设计结论是——

> **P0 冻结"策略 + 字段定义"，但 P0 不引入任何遥测配置项。**
> 理由：① 设备侧遥测上报本身**尚未实现**（`readme.md` §六「9. Device State Heartbeat」仍是待开发项）⇒ 为"还不存在的上报"设计配置，属于凭空设计；
> ② 引入配置项意味着**新增 Config 模块**（`CONFIG_MODULE_COUNT` + `kModuleNames[]` + 模块名宏 + `data/config/<m>.json` **四处改动**）或扩大既有模块 schema ⇒ 会把 P0 从"身份与隔离"污染成"遥测改造"，违反 §12「暂不实施」精神。
> ⇒ **落地时机**：随"Device State Heartbeat"功能一起实现；届时**优先落在 `mqtt.json` 的 `telemetry` 子对象**（不新增 Config 模块，零 4 处改动）。

### 1.6 附加决策：`client_id` 必须改（**Plan 只提"身份漂移"，本轮发现更严重的后果**）

Plan §13.3 把 `client_id = guo_feeder_001` 列为"身份漂移风险"。实测后**严重度要上调**：

> **MQTT 语义后果：`client_id` 是全局唯一的会话标识。两台设备用同一个 `client_id` 连接同一 broker ⇒ 后连者触发 session takeover，把先连者踢下线。**
> 当前配置（`data/config/mqtt.json: "client_id": "guo_feeder_001"`）⇒ **第二台设备一上电，第一台就掉线**，且表现为"莫名重连/命令时好时坏"，**极难排查**。

⇒ **`client_id` 改为 `dev_<device_id>` 属 P0 必需项**（不是美化，是缺陷修复）。改动量：`mqtt.json` 1 个字段 + 渲染 1 处（`cloud_manager.cpp:1462`）。
> **APP 侧的同类问题见 §1.4.1**（`username` 是共享 slot 名，**绝不可**当 `client_id` 用）。

### 1.7 P0 决策汇总表

| # | 项 | 取值 | 状态 |
|---|---|---|---|
| D1 | `device_id` 格式 | `aabbccddeeff`（12 小写 hex） | 【已定】 |
| D2 | 持久化 | NVS · 现有 `nvs` 分区 · namespace `gfid` | 【已定·需你点头】 |
| D3 | Topic 实现 | 配置模板 + 运行时渲染 | 【已定】 |
| — | `client_id`（设备） | `dev_<device_id>`（模板） | 【已定】 |
| — | `client_id`（APP） | **必须唯一**（推荐 UUID）；不得用 `username` 充当 | 【已定·§1.4.1】 |
| D5 | 凭据模型 | slot 池 + API 改写 ACL；**缓存延迟待实测** | 【已定·P1 实施】 |
| D7 | 遥测策略 | 分层策略表；**P0 不改代码** | 【已定】 |
| D4 | Homie domain | `homie` | 【已定·P2】 |
| D6 | 多参数映射 | 单 `json` Property | 【已定·P2】 |
| D8 | legacy 过渡期 | 双发/只读兼容，**长度待定**；**生产禁止长期共享 Topic（§3.3.1）** | 【待定·见 §3.6】 |
| D9 | 前端框架 | MQTT-Tiles | 【已定·P4】 |
| D10 | BT-1 时机 | P3 | 【已定·见 §11】 |

---

## 2. 设备身份模型

### 2.1 定位：设备身份 ≠ 普通配置参数

| 要求 | 落实手段 |
|---|---|
| 稳定性 | 重启 / 升级 / 改配置后**不变** |
| 唯一性 | 全局唯一（来源：芯片 eFuse 出厂 MAC） |
| 来源 | **MAC 派生（初始生成来源）**；**运行期以 NVS 身份锚点为准**；不改用手输、不与 client_id 等其它标识混用（§1.2 / §2.3） |
| 持久化 | 首启生成并**写入 NVS**（作为**运行期身份锚点**，见 §1.2 / §2.3） |
| 不可改性 | **不进 ConfigManager**（`update_config` 在结构上够不到） |
| 可迁移 | **必须走显式 identity migration 流程**（§2.3.1）；P0 只定义，不实现 |
| **语义边界** | **表示 ESP32 芯片实例身份**，不等于物理设备（**见 §2.6**） |

**为什么"不进 ConfigManager"就够**（Plan §12 第 12 条）：

```
ConfigManager  只写 /littlefs/data/config/*.json
device_identity 只读/写  nvs namespace "gfid"
⇒ 两条路径物理不相交 ⇒ config_set / update_config / uploadfs 均无法修改 device_id
```

> **【P0-可选】低成本护栏（建议做）**：在 `ConfigManager` 加载 `mqtt` 模块时，若发现 JSON 中出现 `device_id` 键 ⇒ **打一条串口 WARN 并忽略该键**。
> 理由：防止未来有人"顺手"把 `device_id` 写进配置，形成**第二个身份来源**（= 身份漂移）。成本约 5 行、无行为变更。

### 2.2 派生算法 【已定】

```
输入：芯片 eFuse 出厂 MAC —— 固定取 ESP_MAC_WIFI_STA
输出：12 位小写 hex（sprintf "%02x" × 6）
```

```c
// 概念代码（P0 不落地，此处仅为接口语义说明）
uint8_t mac[6];
esp_read_mac(mac, ESP_MAC_WIFI_STA);      // ← 必须固定用 WIFI_STA 这一路
snprintf(buf, sizeof(buf), "%02x%02x%02x%02x%02x%02x",
         mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
```

**核实过的事实**（本轮已查，不是记忆）：

| 事实 | 证据 |
|---|---|
| `esp_read_mac()` 在本 SDK 可用 | `framework-arduinoespressif32/tools/sdk/esp32s3/include/esp_hw_support/include/esp_mac.h:129` |
| 头文件位置 | 需 `#include "esp_mac.h"`（**IDF 头**，与 `esp_sntp.h` 同类，属项目允许的"非项目头"） |
| Arduino core 版本 | `2.0.17`（`package.json: "3.20017.241212+sha.dcc1105b"`）⇒ IDF **v4.4**，与 `readme.md` §8.5.1 的记录一致 |

**⚠️ 三条硬约束（必须写进实现注释，否则极易踩）**：

1. **必须固定 `ESP_MAC_WIFI_STA`**。ESP32 的 STA / AP / BT 地址之间存在 ±1 的偏移关系，**取哪一路会得到不同的值**。若 A 版本取 STA、B 版本取 BT ⇒ **同一台设备算出两个 device_id = 身份漂移**。
2. **禁止用 `WiFi.macAddress()`**。它依赖 Wi-Fi 协议栈（在 `wifi_init()` 之前调用行为不确定）；`device_identity_init()` 必须在 `wifi_init()` **之前**执行，故只能用 `esp_read_mac()`（纯 eFuse 读取，无依赖）。
3. **禁止派生时引入其它输入**（芯片型号字符串、flash size、时间、随机数）。派生必须是**纯函数** —— 这是 §2.3「**有条件重建**」的前提（**仅在"未迁移、未绑定"时成立**，§2.3.1）。

### 2.3 持久化与身份锚点语义 【已定】

**NVS 布局**：

| 项 | 值 | 说明 |
|---|---|---|
| namespace | `gfid` | 4 字符，远低于 NVS 的 15 字符上限 |
| key `did` | **ASCII 字符串**：长度 **12 个字符**、内容为**小写 hex**；实际 NVS 存储由 `Preferences` 管理（**禁止描述为 "12 bytes"** —— 它是**字符数**，不是字节数） | 身份本体 |
| key `dver` | uint8 = `1` | **`device_id` 派生算法版本**（derivation algorithm version）—— 语义与硬约束见 §2.3.1 |

**★ `device_id` 生命周期语义（v1.1 明确）**：

| 场景 | 行为 | 依据 |
|---|---|---|
| **首次启动** | `ESP32 STA MAC` → 派生 `device_id` → **写入 NVS** | MAC 是**初始生成来源** |
| **后续启动** | **优先使用 NVS 中保存的 `device_id`**（即使与当前 MAC 派生值不一致，也**以 NVS 为准**） | NVS 是**运行期身份锚点** |
| **NVS 丢失** | 重新从 MAC 派生 | **⚠️ 仅适用于"未发生身份迁移、且云端绑定未变化"的情形**；若已迁移 / 已绑定，**不得自动重算**，必须走显式 identity migration（§2.3.1） |
| **芯片替换**（更换 ESP32 主控） | 新主控 = **新芯片实例** ⇒ **属身份迁移场景**；必须走显式 identity migration，**不得**直接沿用旧 `did`，也不得让云端"当成两台设备"了事 | §2.6 `B-1` / §2.3.1 |
| **身份迁移**（显式流程，【P1+】） | 由**显式流程**写入新 `did`（若换算法则另开新 `dver`）；**先完成云端绑定迁移**；全程可审计、可回滚；**普通 OTA / 自动路径一律不触发** | §2.3.1 / §2.6 `B-2` |

**启动决策表（幂等，四条路径全覆盖）**：

| NVS 状态 | 与当前 MAC 派生值比较 | 动作 | 串口输出 | 判定 |
|---|---|---|---|---|
| 无 `did` | — | 派生 → **写入** `did`+`dver` | `[Identity] new device_id=... (nvs written)` | 首启 |
| 有 `did`，`dver` = 1 | **一致** | 直接使用 | `[Identity] device_id=... (nvs ok)` | 正常 |
| 有 `did`，`dver` = 1 | **不一致** | **使用 NVS 值**（身份优先于 MAC），**不覆盖** | `[Identity] WARN mac mismatch, keep nvs id` | ⚠️ MAC 被改写 / 产线换芯片 |
| 有 `did`，`dver` = **未知**（≠ 本固件内置值） | — | **使用 NVS 值** | `[Identity] WARN unknown dver=N, keep id` | 降级兼容 |

#### 2.3.1 `dver` 版本迁移硬约束（v1.1 新增）【已定】

> **`dver` 只表示 `device_id` 派生算法的版本** —— **不表示**固件版本、配置版本或协议版本。

**禁止（硬约束）**：

| # | 禁止项 |
|---|---|
| **DV-1** | **禁止**因固件升级后 `dver` 与本固件内置值不匹配，而**自动重新生成 `device_id`** |
| **DV-2** | **禁止**在任何路径下**自动覆盖已有 `did`**（包括"新算法算出来的值看起来更合理"这种情形） |
| **DV-3** | **禁止**把"普通 OTA 升级"当作改变 `device_id` 的手段 ⇒ **普通 OTA 升级不得改变 `device_id`** |

**规则**：

| 情形 | 要求 |
|---|---|
| 读到**未知 `dver`**（例：固件回退到旧版，而 NVS 中是新版写入的 `dver`） | **必须保留已有 `did`** ⇒ 走"降级兼容"路径；**只打 WARN**，**不重算、不覆盖、不阻断启动** |
| 未来确需**更换派生算法**（换 MAC 来源 / 改字符集或长度 / 改派生输入） | **必须另立显式 `identity migration` 流程**（【P1+】）：新算法**新开 `dver` 值** → 先完成云端侧**绑定迁移** → 再由迁移流程写入新 `did` → **全程可审计、可回滚** |
| 固件内置的 `dver` 常量 | 与"当前算法"**一对一绑定**：**只有改算法才改常量**，且必须同步更新本节 + §2.5 `I-2` |

> **一句话备忘**：**`dver` 是"只读版本标记"，不是"自动升级触发器"。**

**关键设计判断（刻意不做的两件事）**：

| 不做 | 理由 |
|---|---|
| **不做 Active + Backup 双版本**（项目 Config 有双版本，但这里不需要） | 在**未发生身份迁移、且云端绑定未变化**的前提下，`device_id` 可**由 MAC 幂等重建** ⇒ NVS 丢失可**重建**，双份冗余无收益。刻意避免过度设计（与 Plan §12「防范围蔓延」一致）。**⚠️ 该"可重建"结论以"未迁移 / 未绑定"为前提**（§2.3.1） |
| **不做 NVS 写入失败重试循环** | 写入失败时**内存中仍持有可用值**（本次上电功能完整），下次启动重试即可 ⇒ 不阻塞、不重启、零风险 |

**NVS 写入失败的降级语义（必须实现）**：

```
Preferences.begin("gfid", false) 失败 或 putString 失败
  ⇒ 保留内存中的派生值（功能不受影响）
  ⇒ 串口打 WARN（一次）
  ⇒ 不重启、不置任何 System State、不重复重试
```

> ⚠️ **可观测性缺口（如实登记）**：此失败**无法进 LogManager**——`0x01xx`/`0x03xx` 段内**没有**"身份持久化失败"的 EventId，而 **EventId 属冻结协议、P0 不新增**。
> ⇒ 与 `LV-1`（`dispense_guard` 订阅失败）**同族**：**有宿主、有判据、无 ID**。处置：**P0 只打串口 WARN**，登记为缺口，与 `LV-1`/`LV-3` 一起在"日志协议解冻"时统一处理。
> ⚠️ **v1.1 补充**：该降级语义**不得**被解释为"身份可以随意丢弃" —— 它与 §2.3.1 的 DV-1/DV-2 **不冲突**：降级只影响**本次上电**（内存中继续用 NVS 或派生值），**绝不**触发 `did` 的**重写**。

**是否接入 Critical Operation？—— 不接**：

| 判断 | 理由 |
|---|---|
| 写入时机 | 仅 `setup()` 阶段（单线程、无并发、无 MQTT、无 Workflow） |
| 失败后果 | 可降级（§2.3 上文）⇒ 不需要"失败即不重启"的保护语义 |
| 规范约束 | 《critical_operation接入规范》禁止扩大保护范围 ⇒ 接入会污染语义 |

### 2.4 模块设计与接口契约

**归属**：`src/services/device_identity.{h,cpp}`（**服务层**；与 `config_manager` 同层）

**职责（v1.2 收窄为"只做身份"）**：MAC 获取（`ESP_MAC_WIFI_STA` 固定来源）· `device_id` 派生 · NVS namespace `gfid`（`did` / `dver`）管理 · identity 生命周期。**不含模板渲染** —— 渲染已拆至 §2.4.1 **TopicRenderer**。

**分层合法性**：`cloud_manager`（云通信层）**已经在**调用 `config_get_mqtt_*()`（服务层）⇒ 云层 → 服务层是**既有、已存在**的依赖方向；本模块不新建依赖方向。

```
src/services/device_identity.h     ← 新文件（切勿与现有 26 个头文件重名）
src/services/device_identity.cpp   ← 新文件
（模板渲染见 §2.4.1：src/services/topic_renderer.h / .cpp）
```

**对外接口（P0 冻结签名）**：

| 函数 | 语义 | 失败语义 |
|---|---|---|
| `void device_identity_init()` | 首启派生 + 写 NVS / 后续加载（幂等）；打一行串口 | 内部降级，**不抛错、不阻塞** |
| `const char* device_id()` | 返回 `\0` 结尾的 12 字符 id | **永不返回 `nullptr`**；未初始化/失败返回 `""` |
| `bool device_id_valid()` | id 是否可用（长度 12 且全 `[0-9a-f]`） | — |
> **★ 职责边界（v1.2）**：
> ① **模板渲染不属于本模块** ⇒ 拆为独立模块 `src/services/topic_renderer.{h,cpp}`（**TopicRenderer**，见 §2.4.1），使 DeviceIdentity **只做身份**（单一职责）；
> ② **不提供 `device_identity_reset()`**（v1.2 **已删除**）—— 身份重置**不等于**普通 factory reset，保留该接口会被误调用并**直接造成身份漂移**。未来身份迁移**只允许**通过**独立 identity migration 流程**（§2.3.1）实现，**P0 不实现**。

**关于 String 的说明（预先回应"内部模块不传 String"铁律）**：

> **TopicRenderer 的接口（§2.4.1）**位于**"云通信层 ↔ 服务层"的既有 String 边界**上——`config_get_mqtt_*_topic()` / `config_get_mqtt_client_id()` **本来就返回 `String`**，`cloud_manager` 也**本来就**用 `static String` 缓存 topic（`:60/:65/:66/:68`）。
> ⇒ 本设计**不新增 String 传播路径**，只是把"配置读出的字符串"再多经过一次替换。若强行改为 `char*`，反而要在 `cloud_manager` 引入手工缓冲管理（更多出错面）。**结论：沿既有边界，不扩大。**
> 注：**DeviceIdentity 自身不引入 `String`**，对外只用 `const char*`。

**零全局裸变量**：

- 模块内状态用**文件作用域 `static`**（`static char s_device_id[13]` + `static uint8_t s_dver` + `static bool s_ready`），**不引入任何裸全局变量** ⇒ 与 `cloud_manager.cpp` 的既有做法（`static String mqtt_sub_topic` 等）一致。
- **刻意不进 System State**：`System State` 的语义是"**动态运行时状态**"（INT/BOOL/LONG/FLOAT + 通用字符串表，且带 `readable` 会被 `state_report` 上云）。而 `device_id` 是**不可变身份常量**。放进去会：① 让"恒定值"占用动态状态表；② 通过 `readable=true` 把它推给云端 —— 但**云端从 Topic 就能拿到 device_id**，属重复来源。⇒ **P0 不加 State**；device_id 的对外暴露路径 = **P2 Homie 根主题**（`homie/5/<device-id>/$description`）+ **串口**。

**内存影响（实测口径，落地时核对）**：

| 项 | 量级 | 放置 |
|---|---|---|
| `s_device_id[13]` + `s_dver` + `s_ready` | **≈ 16 B** | DRAM `.bss`（栈/堆均不涉及） |
| 代码（派生 + NVS 决策表，**P0-1**） | **≈ 0.8–1.2 KB** | Flash |
| 代码（渲染 + 校验 + 自检，**P0-2**） | **≈ 0.6–1.0 KB** | Flash |
| 堆分配 | **0** | **不使用 PSRAM**（16 B 级数据，上堆反而增加碎片） |
| NVS 写入 | 首启 **1 次**（页内追加，非擦除） | 不上传、不进日志 |

> 验收时**必须**给出两份 ELF 的 `.data/.bss` 符号表 `diff`（项目既有铁律：这是"RAM +0 / +N"的最硬证据）。

#### 2.4.1 TopicRenderer 模块（v1.2 从 DeviceIdentity 拆出）

> **拆分理由**：模板渲染若留在 DeviceIdentity，会让该模块同时承担
> 「MAC 获取 / NVS 身份锚点 / 模板渲染」三件事 ⇒ **违反单一职责**。
> ⇒ 渲染独立成模块，**DeviceIdentity 只做身份**。

| 项 | 内容 |
|---|---|
| 归属 | `src/services/topic_renderer.{h,cpp}`（**服务层**） |
| **职责（只做这些）** | ① `<device_id>` **占位符替换** · ② topic / `client_id` **模板渲染** · ③ **长度检查** · ④ **字符合法性检查** |
| **接口** | `bool topic_render(const String& tmpl, String& out)` |
| 依赖 | **只读**调用 `device_id()`（`services/device_identity.h`）；**零**对 ConfigManager / MQTT / CloudManager 的依赖 |
| 模板常量 | 5 个 `GF_*` 模板宏**单点定义在本模块头文件**（§3.2） |
| 自检 | `topic_renderer_selftest()` —— **仅 DEBUG / 测试环境**编译与调用；**生产固件不执行**（§3.3.2） |

**约束（冻结）**：

| # | 约束 |
|---|---|
| T-1 | **只支持 `<device_id>` 一种占位符** —— 不实现通用模板系统（无变量表、无表达式、无转义语法） |
| T-2 | **渲染点全工程唯一** —— 其它模块（含 `config_manager`）**一律不得**做替换 |
| T-3 | **无 ConfigManager 依赖**：模板由调用方传入，本模块**不读配置** |
| T-4 | **无 MQTT / CloudManager 依赖**：只做字符串处理，不感知发布与订阅 |
| T-5 | 失败语义见 §3.3（4 条规则）：**"不含占位符 ⇒ 原样放行 `true`"**（回滚通路，不可改为报错） |

### 2.5 已知风险（身份层）

| # | 风险 | 影响 | 处置 |
|---|---|---|---|
| I-1 | 产线用同一 MAC 烧录（克隆 MAC） | 两设备身份相同 ⇒ Topic/ACL/绑定全冲突 | 依赖 ESP32 出厂 eFuse MAC 唯一；**量产抽检 + 上线时云端检测"同一 device_id 二次注册"**（P1 的 D1 `Device` 表唯一键天然拦截） |
| I-2 | `dver` 未来升级时改了算法 | 老设备读到新 `dver` ⇒ 降级路径（**保留已有 `did`**，不重算、不覆盖） | **硬约束见 §2.3.1（DV-1/DV-2/DV-3）**：`dver` 只增不改语义；**升级派生算法必须新开 `dver` 值并走显式 identity migration** |
| I-3 | NVS 损坏导致 id 变化（若同时 MAC 也变） | 身份"跳变" ⇒ 云端出现幽灵设备 | `dver` 一致 + 与 MAC 一致 = 正常；不一致必打 WARN（可见） |
| I-4 | 排障时把 NVS 当"缓存"清掉 | ① **未迁移 / 未绑定**：MAC 未变 ⇒ 可重建（无害）<br>② **已迁移 / 已绑定**：**不可由 MAC 重建** ⇒ 身份丢失（严重） | 文档写明：**"可重建"仅适用于"未发生身份迁移、且云端绑定未变化"**；一旦迁移 / 绑定，**NVS 必须备份**（§2.3 / §2.3.1 / §2.6） |
### 2.6 身份边界：`device_id` 表示什么、不表示什么（v1.1 新增）【已定】

**`device_id` 表示**：

> **ESP32 芯片实例身份（ESP32 instance identity）** —— 即"这一颗 ESP32 主控（及其 eFuse MAC 派生结果）"在本系统中的稳定标识。

**`device_id` 不表示**：

| 不是 | 说明 |
|---|---|
| ❌ **产品型号身份** | 不是"Guo Feeder v0.7 这个型号"的标识；**同型号多台设备各有不同的 `device_id`** |
| ❌ **机械外壳身份** | 不绑定外壳 / 结构件 / 批次 / 序列号标签；**换壳不影响 `device_id`** |
| ❌ **用户账号身份** | 不是 `uid`；**用户 ↔ 设备是绑定关系**（P1 的 `UserDeviceBinding`），`device_id` 只是被绑定的对象 |

**生命周期关系（对象层次，不是等号关系）**：

```
物理设备（外壳 + 水箱 + 阀 + HX711 + ESP32 主控 + …）
└── ESP32 芯片实例（eFuse MAC 唯一）
    └── device_id                    ← 本设计冻结的标识（12 位小写 hex）
        └── 云端绑定关系（UserDeviceBinding：uid ↔ device_id）   ← P1
```

**由此得出的两条结论**：

| # | 结论 |
|---|---|
| **B-1** | **更换 ESP32 主控 = 身份迁移场景**（`device_id` 会变）⇒ **必须走显式 identity migration 流程**（§2.3.1）；**不得**让"换上去自动成为新身份、云端当成两台设备"了事 |
| **B-2** | **禁止把 `device_id` 当作"物理设备的永久 ID"来设计云端模型** ⇒ 云端需区分**「设备实例（`device_id`，P0 已定）」**与**「物理设备 / 资产（可选，如 `asset_id`）」**两个层次；P1 建表时**保留该余地**，**P0 不实现** |

> **与 §2.3.1 的关系**：本节的 `B-1` 是**除"算法升级"之外的第二种需要显式迁移的场景**（"算法换了" vs "主控换了"）⇒ 二者**共用同一套 identity migration 流程**。
> **禁止的暗示**：**不得**在任何文档 / 代码 / 云端模型里把 `device_id` 表述为「物理设备的永久 ID」或「等同于物理设备」（二者是**不同层次**的对象，见上方层次树）。


---

## 3. Topic V3 与 `topic_render()`

### 3.1 Topic 规范 【已定】

```
guo_feeder/<device_id>/down     Cloud → Device   设备**唯一订阅**
guo_feeder/<device_id>/up       Device → Cloud   业务上行（ACK / Result / Registry / 上线通告）
guo_feeder/<device_id>/log      Device → Cloud   LogManager 结构化批次（CBOR）
```

| 规则 | 内容 |
|---|---|
| 层级 | 恒为 3 级（`guo_feeder` / `<device_id>` / 方向） |
| 前缀 | **保留 `guo_feeder`**（不改名）。理由：与既有 ACL、既有 Worker/EMQX 规则、既有脚本的习惯一致，改名只会扩大回归面 |
| 设备订阅范围 | **只订阅自己的 `down`**。禁止订阅 `guo_feeder/+/down` 或 `#` |
| Homie 树 | 并存且独立：`homie/5/<device_id>/#`（P2 落地） |
| legacy 三段 | `guo_feeder/down|up|log` 在过渡期内**仅作只读兼容**，**不允许作为控制入口**（Plan §9.4） |

### 3.2 占位符与模板 【已定】

```
占位符：<device_id>          ← 唯一支持的一种，不做通用模板引擎
```

模板常量**单点定义**在 `src/services/topic_renderer.h`（避免"两个文件各写一遍模板"的漂移）：

```
GF_DEVICE_ID_PLACEHOLDER   "<device_id>"
GF_TOPIC_TPL_DOWN          "guo_feeder/<device_id>/down"
GF_TOPIC_TPL_UP            "guo_feeder/<device_id>/up"
GF_TOPIC_TPL_LOG           "guo_feeder/<device_id>/log"
GF_CLIENT_ID_TPL           "dev_<device_id>"
```

> **为什么是 `<device_id>` 而不是 `${device_id}` / `{device_id}`**：
> ① Plan §3.1 已采用该写法（保持一致）；
> ② `${username}`/`${clientid}` 是 **EMQX ACL 侧**的占位符，**不参与固件渲染** —— 用不同写法可从视觉上区分"云侧占位符 / 固件侧占位符"，避免误以为固件要处理 `${...}`；
> ③ `{uid}` 在 `cloud_protocol.md` §1.1 里已被声明为"**设备不会替换**"，沿用会造成语义冲突。

### 3.3 渲染契约（含失败语义）【已定】

```
输入：tmpl（配置读出的原始串）
输出：out（渲染结果）

规则：
1. tmpl 含 "<device_id>" ⇒ 全部替换为实际 device_id（支持多次出现）
2. tmpl 不含 "<device_id>" ⇒ 原样返回 true（= legacy 兼容路径，允许）
3. device_id 不可用（""） ⇒ 返回 false（调用方走内建 V3 兜底）
4. 渲染后仍含 '<'，或长度 > 128，或为空 ⇒ 返回 false（判定为配置错误）
```

本契约由 **TopicRenderer（§2.4.1）** 实现，**渲染点全工程唯一**。

**为什么"不含占位符"要 `true` 而不是报错**：这是**方案 A 的回滚通路**（§1.3）。若报错，回滚就必须改固件。

**但"允许"必须"可见"**——三条可见性要求（否则共享 Topic 会静默扩散）：

| # | 要求 | 落地位置 |
|---|---|---|
| 1 | 渲染后若**不含** `<device_id>` ⇒ 串口打**一次** WARN：`[Identity] WARN topic has no <device_id>, shared-topic mode` | `cloud_manager.cpp:1471–1479` 渲染之后 |
| 2 | 渲染后**仍含** `<` ⇒ 打印 `[Identity] ERROR invalid topic, using built-in V3 default` 并**改用内建兜底模板**（绝不用错误 topic 去发布） | 同上 |
| 3 | 三个 topic 的实际值**在启动时打印**（现有代码已经在打印，`:1473/:1476/:1479`）⇒ 保留，作为"渲染结果的现场证据" | 既有行为 |

> **设计原则**：**宁可连不上，也不要用共享 Topic 静默跑起来**。共享 `down` 的后果（Plan §9.2：一条命令两台都执行）比"连不上"危险得多。
> ⚠️ **v1.1 补充**：上面这条"允许 legacy"仅针对 **dev / test**；**生产环境按 §3.3.1 不允许**。
#### 3.3.1 legacy Topic 回退的**生产环境**安全边界（v1.1 新增）【已定】

> **保留** §1.3 / §3.3 的"配置级回滚"设计（**不改技术方案**）；本节只**增加环境分级的安全边界**。

| 环境 | 是否允许 legacy（模板**不含** `<device_id>`） | 说明 |
|---|---|---|
| **开发 / 测试**（dev / test） | ✅ **允许** | 保留"改配置即回滚"的能力 —— 单机调试、灰度回退都需要它 |
| **生产**（production） | ❌ **不允许** | 生产设备的三个 topic 模板**必须包含 `<device_id>` 占位符** |

**禁止**：生产设备**长期运行**共享 Topic：

```
guo_feeder/down        ← 生产禁止（多设备互相串扰：一条命令多台执行）
guo_feeder/up          ← 生产禁止
guo_feeder/log         ← 生产禁止
```

**启动检查规则（设计要求）**：

| 条件 | 要求行为 |
|---|---|
| **production 模式** 下，任一 topic 模板**不含** `<device_id>` | **输出配置错误**（串口 ERROR + 明确文本），并**阻止 MQTT 正常上线**（不建立/不保持业务订阅与发布） |
| **dev / test 模式** | 仅打 WARN（§3.3 第 1 条），**允许继续运行**，以便回滚验证 |

**范围边界（重要）**：

> 本节**只增加设计约束**，**不要求 P0 实现完整的"生产模式管理"**（**不新增**配置项 / 模块 / EventId）。
> P0 落地时该检查**复用既有的"渲染失败 ⇒ 判为配置错误"路径**（§3.3 规则 4）即可；
> "production 标志"如何确定（编译期宏 / 配置项 / 构建 profile）属**后续阶段**的设计问题，**P0 不决策**。


#### 3.3.2 模板渲染自检（v1.2 新增，**DEBUG-only**）【已定】

| 项 | 内容 |
|---|---|
| 函数 | `topic_renderer_selftest()` —— 归属 **TopicRenderer**（§2.4.1），**不放在** `device_identity_init()` 内 |
| 内容 | 对 6–8 个模板样例（含占位符 / 无占位符 / 多次占位符 / 超长 / 非法字符 / `device_id` 不可用）断言期望输出 |
| 输出 | 串口一行 `[TopicRender] selftest PASS/FAIL`（**仅见** `GF_TOPIC_RENDER_SELFTEST` 打开时） |
| 编译开关 | `GF_TOPIC_RENDER_SELFTEST`（**默认 0**）—— 与 `MI_THERMO_DEBUG_VERBOSE` 同一模式 |
| **生产** | **生产固件不编译、不执行**（默认 0 ⇒ 连字符串表都不进镜像） |

> **为什么默认关**：① 自检是**开发期**工具，不属于运行期契约；② 默认 0 时其字符串常量与分支
**被 `--gc-sections` 剔除** ⇒ 零 Flash/RAM 代价；③ 避免"生产设备每次启动都跑测试代码"。
> `device_identity_init()` 内**不得**出现任何 selftest 调用或输出（v1.2 明确）。

### 3.4 改造清单（精确到行）【P0 代码改动】

**新增（4 文件：P0-1 / P0-2 各 2）**

| 文件 | 归属 | 内容 |
|---|---|---|
| `src/services/device_identity.h` | **P0-1** | 身份接口（`device_identity_init` / `device_id` / `device_id_valid`）+ 头文件保护 |
| `src/services/device_identity.cpp` | **P0-1** | MAC 派生（`#include "esp_mac.h"` **仅在此文件**）· NVS `gfid`（`did`/`dver`）· 启动决策表 · 启动打印 |
| `src/services/topic_renderer.h` | **P0-2** | 5 个模板宏 + `topic_render()` + `topic_renderer_selftest()` 声明 |
| `src/services/topic_renderer.cpp` | **P0-2** | 占位符替换 · 长度/字符校验 · **DEBUG-only** 自检实现 |

**改动（5 处既有代码，均为小改）**

| # | 位置 | 现状 | 改为 | 性质 |
|---|---|---|---|---|
| 1 | `src/services/config_manager.cpp:3688` | `["subscribe_topic"] \| String("")` | `\| String(GF_TOPIC_TPL_DOWN)` | 兜底值 |
| 2 | `src/services/config_manager.cpp:3704` | `\| String("guo_feeder/up")` | `\| String(GF_TOPIC_TPL_UP)` | 兜底值 |
| 3 | `src/services/config_manager.cpp:3711` | `\| String("guo_feeder/log")` | `\| String(GF_TOPIC_TPL_LOG)` | 兜底值 |
| 4 | `src/services/config_manager.cpp:3683` | `["client_id"] \| String("")` | `\| String(GF_CLIENT_ID_TPL)` | 兜底值 |
| 5 | `src/services/config_manager.h:629/630/633/634` | 无契约说明 | 补注释：**"返回**模板**，未渲染；渲染点唯一在 `topic_render()`（`topic_renderer.h`）"** | 注释（防误用） |

**改动（`cloud_manager.cpp`，4 处）**

| # | 位置 | 现状 | 改为 |
|---|---|---|---|
| 6 | `:1462` | `mqtt_client_id = config_get_mqtt_client_id();` | 读配置 → **`topic_render()`**（失败走内建兜底 `dev_<id>`） |
| 7 | `:1471` | `mqtt_sub_topic = config_get_mqtt_subscribe_topic();` | 读配置 → **`topic_render()`**（含 §3.3 的 WARN 判定） |
| 8 | `:1474` / `:1477` | 同上（pub / log） | 同上 · **需新增 `#include "services/topic_renderer.h"`** |
| 9 | `:1802–1805` | `if(len==0) mqtt_log_topic = "guo_feeder/log";` | 兜底改为**渲染后的 V3 模板**（保持与 :1477 同源） |

> `:65/:66/:68` 三个 `static String` **不改**（只是语义从"原始配置值"变为"已渲染值"）。
> 使用点 `:333`（subscribe）· `:550/:554`（topic 选择）· `:1745/:1775/:1814/:1822`（发布）**全部不改** —— 它们用的都是缓存值。

**改动（`main.cpp`，1 处）**

| # | 位置 | 内容 |
|---|---|---|
| 10 | `:517`（`config_init();`）之后、`:518`（`event_manager_init();`）之前 | 插入 `device_identity_init();` |

**插入位置论证**：

| 约束 | 满足情况 |
|---|---|
| 必须早于 `wifi_init()`（`:523`） | ✅ 满足（身份不依赖 Wi-Fi，且**必须先于任何网络标识生成**） |
| 必须早于 `cloud_init()`（`:549`） | ✅ 满足（cloud 启动即渲染并打印 topic） |
| 必须早于 `ble_init()`（`:522`） | ✅ 满足（未来 Homie/BLE 广播名要用到身份） |
| 无需早于 `log_init()`（`:468`） | ✅ 因为本模块**不埋 LogManager**（§2.3），故不依赖日志子系统 |
| 无需依赖 `system_state_init()`（`:516`） | ✅ 因为本模块**不进 System State**（§2.4） |

**配置改动（`data/config/mqtt.json`）**

| 字段 | 现值（仓库） | 改为 |
|---|---|---|
| `client_id` | `guo_feeder_001` | `dev_<device_id>` |
| `subscribe_topic` | `guo_feeder/down` | `guo_feeder/<device_id>/down` |
| `publish_topic` | `guo_feeder/up` | `guo_feeder/<device_id>/up` |
| `log_topic` | `guo_feeder/log` | `guo_feeder/<device_id>/log` |

> ⚠️ **设备端配置与仓库不一致是常态（已遇 4 例）**⇒ 上线时**不要 `uploadfs`**，必须用
> `config_set`（逐字段）+ `config_save`（原子落盘 + 安全重启）。理由见 §8 与 `.workbuddy/memory/MEMORY.md` 铁律：
> `uploadfs` = 整区重建 LittleFS ⇒ `/factory/*.json`、`/config/.commit`、设备上配的 workflow BIN 全丢 + `version.json` 被仓库值覆盖。

### 3.5 改造清单（**测试与工具，本轮发现的连带影响**）【P0 必做，否则工具全废】

**问题**：以下**受跟踪资产硬编码了 legacy Topic** —— Topic V3 上线后它们**收不到任何东西**：

| 文件 | 行 | 硬编码 |
|---|---|---|
| `tools/mqtt_test_client.py` | `:29/:30` | `SUB="guo_feeder/up"` / `PUB="guo_feeder/down"` |
| `tools/mqtt_send.py` | `:31/:32` | 同上 |
| `tools/mqtt_system_test.py` | `:24/:25` | 同上 |
| `test/mqtt_log_probe.py` | `:84/:85/:94/:111/:116/:193` 等 | `guo_feeder/log` / `guo_feeder/down` |
| `test/log_fix_tests.txt` | `:431/:436/:449`（注释内） | 命令行示例 |
| `test/p4_dispense_log_tests.txt` | `:92`（注释内） | 说明文字 |

**处置（设计）**：给工具加 `--device-id <id>`（缺省读环境变量 `GF_DEVICE_ID`；**不提供"默认走通配 `+/up`"选项**，避免多设备混流假象）。

> ⚠️ 注意 `guo_feeder/+/up` **不能**匹配 legacy `guo_feeder/up`（通配符占整整一级）⇒ 过渡期若既要 legacy 又要 V3，必须**显式订阅两条**或**显式指定 device_id**。这一点最容易在"以为工具没问题"时踩坑。

### 3.6 legacy 过渡（D8，只定原则，长度留 P1）【待定】

```
Legacy                  Compat 期                       Topic V3 生产
guo_feeder/down   ──►   固件只订 V3；legacy 由配置决定   ──►   guo_feeder/<device_id>/down
guo_feeder/up     ──►   单发 V3（双发为可选）            ──►   guo_feeder/<device_id>/up
guo_feeder/log    ──►   单发 V3（双发为可选）            ──►   guo_feeder/<device_id>/log
```

**P0 的做法（比 Plan §9.3 更省）**：

| 项 | 决定 | 理由 |
|---|---|---|
| 是否实现"同时订阅 legacy + V3" | **P0 不实现双订阅** | 双订阅 = 共享 `down` 仍可控制设备（Plan §9.4 第 4 条：legacy **不允许作为控制入口**）。用"配置模板"实现回滚（§1.3）已经覆盖了过渡需求，比双订阅更干净 |
| 双发 | **P0 不做** | 双发只服务"云端还没切"的场景；当前云端仅 1 张 `mqtt_messages` 表、无按 device_id 的落库逻辑（P3 才做）⇒ 双发无消费方 |
| legacy 长度 | **待定**：以"P1 凭据模型上线 + 云端按 device_id 落库（P3）完成"为终点 | 现在定一个"X 天"没有依据 |

> ⇒ **P0 迁移形态 = "一个一个设备切，出问题用配置回滚"**，而不是"全局双跑"。这条比 Plan 的原方案更保守，也更容易验收。
> ⚠️ **v1.1 边界**：这里说的"用配置回滚"**仅适用于 dev / test 环境**；**生产环境禁止长期运行共享 Topic**（§3.3.1）。

---

## 4. EMQX ACL 模型（P0 子项 0.6）

### 4.1 分层：P0 的 Topic 隔离 ≠ ACL 级隔离（**必须说清，否则会误判 P0 的安全性**）

| 层 | 拦得住什么 | 拦不住什么 | 归属 |
|---|---|---|---|
| **固件行为**（P0） | 设备**只订阅自己的** `down` ⇒ **消除"一条命令两台都执行"的误操作** | 恶意/误配的设备可以自己改订阅 | P0 |
| **EMQX ACL**（P0 建规则，P1 收窄） | 设备账号**没有** `#`/`+/down` 权限 ⇒ 不能订阅他人 Topic | ⚠️ **当前两台设备共用账号 `GuoFeederDevice` ⇒ ACL 无法按设备区分** | P0 建规则 / **P1 才真隔离** |
| **一机一账号** `dev_<device_id>`（P1） | 真正按设备隔离（ACL 维度） | — | P1 |

> ⇒ **诚实结论**：**P0 交付"Topic 隔离（防误操作）"，P1 交付"ACL 级越权隔离"。**
> P0 的 ACL 工作 = ① 让 V3 主题**可用**（否则设备连上就被 `deny #` 兜底拒掉）；② 用**枚举具体 device_id** 的方式把"共享账号"的越权面压到最小。

### 4.2 白名单语义与匹配顺序 【已定】

```
匹配顺序：  Username / ClientID 规则   →   全部用户规则
⇒ 必须存在：全部用户 → # → pubsub → deny       ← 兜底（已存在，勿删）
⇒ 每个客户端必须有**显式 allow**，否则连不上   ← 这是"看起来没配错但连不上"的头号原因
```

⚠️ **只加 `deny #` 而不加 allow = 把所有客户端全拒**（2026-10-01 曾处此状态，靠授权缓存暂时侥幸）。

### 4.3 P0 规则集（**V3 兼容版**，账号仍为共享）

**设备账号**（`GuoFeederDevice` / `GuoFeederDevice001`）——**逐 device_id 枚举，不用 `+/down`**：

| 主题 | 动作 | 权限 | 说明 |
|---|---|---|---|
| `guo_feeder/<id_1>/down` | subscribe | allow | **逐个已注册设备各一条** |
| `guo_feeder/<id_2>/down` | subscribe | allow | … |
| `guo_feeder/<id_N>/down` | subscribe | allow | … |
| `guo_feeder/<id_1>/up` | publish | allow | 同上，逐条 |
| `guo_feeder/<id_N>/up` | publish | allow | … |
| `guo_feeder/<id_N>/log` | publish | allow | … |
| `guo_feeder/down` | subscribe | allow | **legacy，过渡期保留**（设备端配置未更新前仍要用） |
| `guo_feeder/up` | publish | allow | legacy |
| `guo_feeder/log` | publish | allow | legacy |
| `homie/5/<id_N>/#` | pubsub | allow | **P2 预留**（可先不加，避免无用条目） |

> ⚠️ **legacy 三条（`guo_feeder/down` / `up` / `log`）仅用于过渡 / 兼容**：**只允许 dev / test 环境**；**生产环境禁止长期运行共享 Topic**（§3.3.1）。生产设备的 ACL 仍**必须**逐 device_id 枚举 V3 规则。


**运维 / 调试账号**（`workbuddy` / `test001` / `shouji`）：

| 主题 | 动作 | 权限 | 说明 |
|---|---|---|---|
| `guo_feeder/#` | all | allow | **暂维持**（调试用）；P1 收窄为按 device_id |

> ★ **APP 侧 `client_id` 约束见 §1.4.1**：ACL 规则按 **`username`** 授权，而 session 由 **`client_id`** 区分 ⇒ **不得把 slot 的用户名当作 APP 的 `client_id`**（`app_slot_<n>` 是共享名，多 APP 会撞号互踢）。

**兜底（全部用户）**：`#` → pubsub → **deny**（不动）。

**为什么"逐 id 枚举"优于 `guo_feeder/+/down`**：

| 方案 | 越权面 | 条目数 |
|---|---|---|
| `guo_feeder/+/down` | 任意设备可订阅**全部**设备的 down ⇒ 等于没隔离 | 少（1 条） |
| **逐 id 枚举** ✅ | 只能订阅**本部署已注册**设备的 down，且固件只订自己的 ⇒ 双层收窄 | 每设备 ~4 条（上限 = 连接数×2 封顶 10 万，2000 设备 ≈ 8000 条，**绰绰有余**） |

### 4.4 生成方式

| 对象 | 手段 | 现状 |
|---|---|---|
| 设备规则 | **脚本按 device_id 列表生成**（`EMQX_Assets/scripts/`）| `apply-acl.mjs` 已封装 `replaceUserAclRules()`（PUT 覆盖，幂等） |
| 兜底规则 | 控制台/API 一次性配置 | ✅ 已存在 |
| 账号创建（P1） | `createUser()`（逐条，服务端自动哈希）或 CSV 批量导入（≤1000/次、≤2000 总量） | 【P1】 |

> ⚠️ `EMQX_Assets/config/acl-rules.proposed.json` 当前仍是 **Topic V2 稿**（文件内已自述"P0 上 Topic V3 后应改为 `guo_feeder/<device_id>/...`"）⇒ **P0 落地时需产出 V3 版规则文件**（属 EMQX 仓库的改动，不在本仓库）。

### 4.5 ACL 相关待实测项

| # | 待实测 | 影响 |
|---|---|---|
| A-1 | 规则 PUT 后**授权缓存的生效延迟** | P1 签发时序（必须"先改后连"） |
| A-2 | 以设备账号订阅**其它 device_id 的 down** 是否真被拒（越权验证） | P0 验收项 §9-#7 |
| A-3 | Serverless 对 `PUT .../rules/users/{username}` 的**条目上限**行为（每用户规则数上限） | 多设备时的规则组织方式 |
| A-4 | **MQTT broker 对 `client_id` 的长度上限**（EMQX 实际值；wss 与 mqtts 是否一致） | §1.4.1 的长度约束（P0 自定 ≤128，**不依赖实测值**）；P1 签发时需确认 |

---

## 5. DEF-2 修复（P0 子项 0.7）：凭据明文打印串口

### 5.1 事实（实测行号，非记忆）

| # | 位置 | 打印内容 |
|---|---|---|
| 1 | `cloud_manager.cpp:1383–1390` | `client_id=` · `username=` · **`password=`**（`===== MQTT DEBUG =====` 块内） |
| 2 | `cloud_manager.cpp:1463–1464` | `MQTT client_id=` |
| 3 | `cloud_manager.cpp:1465–1467` | `MQTT username=` |
| 4 | `cloud_manager.cpp:1468–1470` | **`MQTT password=`** |

### 5.2 修复设计 【已定】

**分级脱敏，而非"全删"**（排障价值要保留）：

| 字段 | 处置 | 理由 |
|---|---|---|
| `password` | **绝不打明文**。改为 `password=<len>B`（或 `<masked>`） | `dev_<device_id>` 一机一密后，串口日志 = 凭据泄露入口（§11 DEF-2 风险） |
| `username` | **保留**（打印结构变化） | 用户名 `dev_<device_id>` 本身就是**公开身份**（Topic 里就有），打出来有排障价值 |
| `client_id` | **保留** | 同上（且正是 P0 要验证的字段） |

**实现方式（沿用项目既有先例）**：

> 项目已有"**编译期调试开关**"的先例：`MiThermometer.cpp` 的 `MI_THERMO_DEBUG_VERBOSE`（默认 0，宏改 1 即恢复逐字节打印，见未修复问题 `P0-3`/`C7`）。
> ⇒ 本项采用同一模式：新增 `MQTT_DEBUG_CREDENTIALS`（**默认 0**）。
> `0`：脱敏（安全默认）；`1`：恢复明文（**仅本地排障编译使用，禁止入库产线固件**）。

**行为变更提示（必须独立提交 + 独立回归）**：

| 影响面 | 说明 |
|---|---|
| 串口输出变化 | 任何**断言串口包含 `MQTT password=xxx`** 的夹具都会失败 ⇒ 落地前须 `grep` 全部 `test/*.txt`（本轮已抽查：**无匹配**，风险低） |
| 无功能影响 | 不涉及协议 / 不涉及 MQTT 行为 / 不涉及 System State |

### 5.3 与"轮换密码"的关系（P0-可选，建议 P0 后立即做）

> `docs/README.md` §「已知待办：文档中的明文凭据」与未修复问题 `P0-2` 已登记：**10 个受跟踪文件含明文凭据**（`docs/interfaces/cloud_protocol.md` · `docs/archive/log/LogManager-P1.5-Board-Test-Report0918.md` · `tools/mqtt_*.py` 等）。
> **gitignore 清不掉 git 历史** ⇒ 唯一彻底修复是**轮换 MQTT 密码**。
> **P0 不做轮换**（会打断现有验证链路，且与"一机一密"改造重复）⇒ **放到 P1 与 `dev_<device_id>` 一机一密一起做**（Plan §7 待决事项 3 已如此规划）。

---

## 6. D7 落实细节（只设计，不实现）

见 §1.5 的策略表。本节的**唯一目的是把"未来的配置落点"预先钉死**，避免将来新增 Config 模块。

**未来配置落点（预登记，P0 不落地）**：`data/config/mqtt.json` 新增子对象

```json
"telemetry": {
    "enabled": true,
    "state_report_interval_sec": 900,
    "weight_report_delta_g": 20,
    "weight_report_min_interval_sec": 300
}
```

| 为什么放 `mqtt.json` 子对象 | 说明 |
|---|---|
| 零 Config 模块新增 | 不触碰 `CONFIG_MODULE_COUNT` / `kModuleNames[]` / 模块名宏 / `data/config/<m>.json` 四处 |
| 语义可解释 | 遥测上报的**唯一出口是 MQTT**，放 mqtt 模块语义自洽 |
| 云侧一致性 | 云端只需订阅 `up` + `log`，不需要新 Topic ⇒ 与 Plan §2.1 架构一致 |

---

## 7. 改造清单汇总（P0 实施清单）

### 7.1 固件仓库（本仓库）

| # | 文件 | 性质 | 规模 | 说明 |
|---|---|---|---|---|
| 1 | `src/services/device_identity.h` | **新增（P0-1）** | ~35 行 | **身份接口**（3 个函数）+ 头保护 |
| 2 | `src/services/device_identity.cpp` | **新增（P0-1）** | **~150–200 行** | 派生 / NVS `gfid` / 启动决策表 / 打印 |
| 3 | `src/services/topic_renderer.h` | **新增（P0-2）** | ~50 行 | 5 个模板宏 + `topic_render()` + selftest 声明 |
| 4 | `src/services/topic_renderer.cpp` | **新增（P0-2）** | ~120–150 行 | 渲染 / 校验 / DEBUG-only 自检 |
| 5 | `src/main.cpp` | 改（P0-1） | **+1 行** | `:517` 后插 `device_identity_init();` |
| 6 | `src/services/config_manager.cpp` | 改（P0-2） | **4 行** | `:3683/:3688/:3704/:3711` 兜底值改模板（**只返回模板，不渲染**） |
| 7 | `src/services/config_manager.h` | 改（P0-2） | 注释 | `:629–634` 补"返回模板"契约 |
| 8 | `src/cloud/cloud_manager.cpp` | 改（P0-2/P0-3） | **~12 行** | `include topic_renderer.h`；`:1462` client_id 与 `:1471/:1474/:1477` topic 调 **`topic_render()`**；`:1802–1805` 兜底；§3.3 的 WARN |
| 9 | `data/config/mqtt.json` | 改（P0-3） | 4 字段 | client_id + 3 topic 改模板（⚠️ gitignore 内，不进版本库） |
| 10 | `platformio.ini` | 改（**P0-可选**） | 4 行 | 固定 `platform` / `lib_deps` 版本（未修复问题 `DD-5`/`9.7`，构建可复现） |

### 7.2 测试与工具（本仓库，**不做则工具全废**，见 §3.5）

| # | 文件 | 改动 |
|---|---|---|
| 9 | `tools/mqtt_test_client.py` · `tools/mqtt_send.py` · `tools/mqtt_system_test.py` | 加 `--device-id`（或 `GF_DEVICE_ID`） |
| 10 | `test/mqtt_log_probe.py` | 同上（订阅/发布 topic 参数化） |
| 11 | `test/log_fix_tests.txt` · `test/p4_dispense_log_tests.txt` | 注释中的示例命令更新 |
| 12 | `test/p0_identity_tests.txt` | **新增**串口用例集（内容见 §9.3） |

### 7.3 文档

| # | 文件 | 改动 |
|---|---|---|
| 13 | `docs/interfaces/cloud_protocol.md` | §1.1 补一句：**"`<device_id>` 占位符由固件 `topic_render()`（TopicRenderer）显式替换；设备不会替换 `${...}`"** |
| 14 | `docs/architecture/Cloud-APP-Platform-Plan.md` | §15 D1/D2/D3/D5/D7 标注 **【已定 + 本文链接】**；§3.1 补"方案 A 的回滚价值" |
| 15 | `docs/README.md` | 索引新增本文件 |
| 16 | `HANDOFF.md` | §4 固件侧进度 + §5 P0 状态 + §7 待决事项收尾 |

### 7.4 其它仓库（不在本仓库范围，仅登记）

| 仓库 | 改动 |
|---|---|
| `EMQX_Assets` | 产出 V3 版 ACL 规则文件；按 device_id 列表套用（`apply-acl.mjs`） |
| `Cloudflare_Assets` | P0 **无改动**（主题硬编码/无 device_id 属 P3 范畴） |

---

## 8. 上线顺序与回滚（**顺序错了会连不上，本节最关键**）

### 8.1 上线顺序（4 步，不可调换）

```
① EMQX：先加 ACL ─────────────────────────────────────────────
   为每个已注册 device_id 增补 V3 的 sub/publish 允许规则
   **legacy 三条 allow 必须同时保留**（设备端配置尚未更新）
   校验：node scripts/check-connection.mjs（干跑 + 回读）

② 固件：编出 P0 版本并烧录（**仅灰度 / 验证阶段**）─────────────
   此时设备端 mqtt.json 仍是 legacy 三段
   ⇒ 渲染器走"无占位符 ⇒ 原样返回"路径 ⇒ 行为与现在完全一致
   ⇒ **这是刻意的：先验证"零回归"，再改配置**
   ⚠️ 本步骤**不得作为生产形态停留**：生产环境按 §3.3.1 要求 topic 模板
      必须含 <device_id>，缺失即配置错误并阻止 MQTT 上线
      ⇒ 灰度过关后**必须**继续执行步骤 ③

③ 设备端：改配置（config_set + config_save）───────────────────
   client_id      → dev_<device_id>
   subscribe_topic→ guo_feeder/<device_id>/down
   publish_topic  → guo_feeder/<device_id>/up
   log_topic      → guo_feeder/<device_id>/log
   ⚠️ 禁止 uploadfs（整区重建 LittleFS：丢 /factory/*.json、/config/.commit、
      设备上配的 workflow BIN + version.json 被仓库值覆盖）
   ⇒ config_save 后安全重启（10 s），串口应打印渲染后的 3 个 topic

④ 验证：见 §9 ────────────────────────────────────────────────
```

> **为什么必须"先 ACL 后配置"**：`deny #` 兜底的存在 ⇒ 只要 V3 主题**没有显式 allow**，设备一切到 V3 就会被**全部拒绝**（表现为连接成功但收不到/发不出，**极易误判为固件 bug**）。

### 8.2 回滚路径（**全部是配置级，不需要回退固件**）

| 场景 | 回滚动作 | 耗时 |
|---|---|---|
| V3 出现异常，需立刻回到 legacy | `config_set` 三个 topic 改回 `guo_feeder/down|up|log` + `config_save` | 1 条命令 + 10 s 重启 |
| `client_id` 出问题 | `config_set` 改回原值 | 同上 |
| ACL 配错 | legacy 规则**始终保留** ⇒ 无需改 ACL | 0 |
| 固件本身有问题 | 烧回上一版二进制（`esptool write_flash 0x10000`）+ 设备端配置已是 legacy | 分钟级 |

> **方案 A 的核心价值在此体现**：因为"渲染器对无占位符模板原样放行"，**"是否启用 Topic V3"这件事完全由配置决定** ⇒ 灰度与回滚都不需要碰固件。
>
> ⚠️ **v1.1 边界（生产环境）**：上表第 1 行的"回滚到 legacy"**仅限 dev / test**。**生产环境按 §3.3.1 不允许长期运行共享 Topic** ⇒ 生产侧的正确处置是**回退到上一版固件（其已渲染 V3 模板）或修正模板**，**不是**把模板改回 `guo_feeder/down|up|log`。若确需临时降级，必须先**显式**把设备切到 dev / test 模式（该切换机制属后续阶段，**P0 不实现**）。

---

## 9. 验收标准（DoD）与测试计划

### 9.1 DoD（8 条，逐条可证）

| # | 验收项 | 证据 |
|---|---|---|
| 1 | 首启生成 device_id 并写入 NVS | 串口 `[Identity] new device_id=...` 且**重启后不再出现 `new`** |
| 2 | **跨重启稳定**（同一设备多次重启 id 不变） | 连续 3 次上电，串口 id 一致 |
| 3 | **幂等重建（⚠️ 有条件）**：抹掉 NVS 后，**仅在"未发生身份迁移、且云端绑定未变化"时**才重新派生得到同一值 | 擦除 namespace 后重启，id 与①一致（**迁移 / 绑定后不适用**，见 §2.3.1） |
| 4 | 渲染正确 | 串口 3 行：`MQTT subscribe=guo_feeder/<id>/down` / `publish=.../up` / `log=.../log` |
| 5 | legacy 模板可回滚（**仅 dev / test 环境**） | 改回 `guo_feeder/down` ⇒ 串口出现"无 `<device_id>`，共享 Topic 模式"WARN，且链路正常；**生产环境按 §3.3.1 属配置错误**（应阻止 MQTT 上线） |
| 6 | **双设备不再互相踢线** | 两台设备同时在线 ≥10 min，双方 `keep_alive` 内不出现非预期重连；`client_id` 不同 |
| 7 | **跨设备越权被拒** | 以设备账号订阅 `guo_feeder/<另一台>/down` ⇒ 被拒（EMQX 侧确认） |
| 8 | 零回归 + 资源实测 | 命令 / ACK / Registry / 日志批次全通；`nm -S` + `.data/.bss` 符号表 `diff` 给出 RAM 增量（预期 **+16 B 左右**） |
| 9 | **OTA 身份持久性**：普通 OTA 升级后 `device_id` **不变**（§2.3.1 **DV-3**） | 记录升级前 id → 执行 OTA → 重启 → 串口 `[Identity] device_id=` 与升级前**完全一致**（用例 **S-11**） |

### 9.2 静态验证（项目既有三件套，必做）

| 手段 | 目标 |
|---|---|
| `nm -S`（两份 ELF） | RAM 增量归因到 `s_device_id` 等具体符号 |
| `.data/.bss` 符号表 `diff` | **"RAM +0/+N"的最硬证据**（项目铁律） |
| `objdump` | 渲染函数的调用站点；确认**未**混入 `String` 临时对象爆发（`String` 构造站点数与改动前对照） |
| `size` / `readelf` | Flash 段尺寸（预期 +1–2 KB） |

### 9.3 串口用例集（`test/p0_identity_tests.txt`，实施时新增）

> 格式遵循 `test/serial_batch.py` 约定：`<命令> ||| 期望子串`，**断言串不加引号**。

| # | 命令 | 断言 | 意图 |
|---|---|---|---|
| 1 | `logt stats` | `emit` | 探活（被动观测不可靠，项目铁律） |
| 2 | `config_query mqtt` | `client_id` | **读设备端实值**（而非信仓库 `data/`）；同时确认输出**不含** `device_id` 字段（§2.1 断言：身份不得进 Config） |
| 3 | `system.time` | `source` | 顺带回归（时间链路未变） |
| 4 | （重启） | `[Identity] device_id=` | 启动打印 |
| 5 | （配置回滚后重启） | `shared-topic` | §3.3 的 WARN 通路（**仅 dev/test**） |
| 6 | （**OTA 升级后重启**） | `[Identity] device_id=` | **S-11 身份持久性**：与升级前**一致**（DV-3；完整用例见 §P0-5 清单） |

> ⚠️ **注意**：`config_query mqtt` 的输出里**不应**出现 `device_id` 字段（§2.1 的"不进 Config"断言）。若出现 ⇒ 说明有人把身份写进了配置，**必须修**。

### 9.4 云端侧验证（MQTTX / 脚本）

| # | 步骤 | 期望 |
|---|---|---|
| 1 | 用 `tools/mqtt_test_client.py --device-id <id>` 订阅 `guo_feeder/<id>/up` | 收到设备上线通告 / 命令回包 |
| 2 | 向 `guo_feeder/<id>/down` 下发只读命令（如 `{"cmd":"system","ob":"time","id":"<新id>"}`） | 设备执行并回包（⚠️ `cmd_id` 30 s 去重 ⇒ **每次换新 id**） |
| 3 | 订阅 `guo_feeder/<id>/log` | 收到日志批次（需云端/工具回 `log_ack` 才会继续；`BT-1` 未修时表现为周期性重发） |
| 4 | 订阅 legacy `guo_feeder/up` | 设备切 V3 后**应为空**（证明真的切过去了） |

---

## 10. 风险登记（P0）

| # | 风险 | 概率 | 影响 | 缓解 |
|---|---|---|---|---|
| R1 | **先改设备配置、后加 ACL**（顺序错） | 中 | 设备"连上但全被拒"，误判为固件 bug | §8.1 顺序 + §8.2 回滚；串口有 topic 打印可快速定位 |
| R2 | 配置模板写错（如漏 `/`、写成 `${device_id}`） | 中 | 渲染后仍在发错主题 | §3.3 规则 4（含 `<` 或超长即判非法）+ 串口 ERROR |
| R3 | **设备端 `mqtt.json` 与仓库不一致**（已遇 4 例） | **高** | 以为改了其实没改 | 上线前 `config_query` 读实值；用 `config_set`+`config_save`，**禁 `uploadfs`** |
| R4 | NVS 首写失败（配额/损坏） | 低 | 本次启动功能正常，下次重试 | §2.3 降级语义（不阻塞、不重启） |
| R5 | 身份持久化失败**不可进日志**（无 EventId） | — | 排障只能靠串口 | 登记为缺口（与 `LV-1`/`LV-3` 同族），P0 只打串口 |
| R6 | 测试工具未同步改造 ⇒ "验证通过"是假象 | 中 | 误判上线成功 | §3.5 列为 P0 必做项（不是可选） |
| R7 | `client_id` 改了但 EMQX 侧有旧会话残留 | 低 | 初次连接出现一次 takeover（正常现象） | 上线后观察 1 个 `keep_alive` 周期 |
| R8 | 量产 MAC 克隆 | 低 | 身份冲突 | §2.5 I-1；P1 的 D1 `Device` 唯一键拦截 |
| R9 | **"生产模式检查"在 P0 未实现**（§3.3.1 明示不实现） | 中 | 生产设备可能**仍以 legacy 共享 Topic 运行**而不报错 ⇒ 多设备串扰 | 上线检查清单**人工核对** topic 模板含 `<device_id>`（§9.1 DoD#5 / §9.4 #4）；自动化实现属后续阶段 |

---

## 11. 待实测 / 待你确认

### 11.1 待实测（上板/上云）

| # | 项 | 时机 | 阻塞谁 |
|---|---|---|---|
| T-1 | 渲染 + 打印 + 跨重启一致性（DoD #1–#5） | P0 实施时（需串口设备） | P0 验收 |
| T-2 | 双设备隔离（DoD #6/#7） | P0 实施时（**需 2 台设备 + 2 个 device_id**） | P0 验收 |
| T-3 | EMQX 授权缓存生效延迟（§4.5 A-1） | P1 开工前 | **P1 签发时序** |
| T-4 | ACL 越权拒绝的实际表现（A-2） | P0 实施时 | P0 验收 |
| T-5 | `nm -S` RAM 增量（预期 ~+16 B） | P0 实施时 | 资源预算 |

### 11.2 待你确认（4 项，其余已按建议定案）

| # | 待确认 | 我的建议 |
|---|---|---|
| C-1 | **D2 改为"复用 `nvs` 分区 + namespace `gfid`"**（而非 Plan 建议的"独立分区"） | ✅ 接受（§1.2 已给出实测依据：flash 已 100% 分配；`device_id` 在**未迁移、未绑定**时可由 MAC 幂等重建 —— 见 §2.3 / §2.3.1） |
| C-2 | `client_id` 一并改为 `dev_<device_id>`（Plan §15 第 3 项） | ✅ 建议做 —— 这是**缺陷修复**（双设备互相踢线），不是美化 |
| C-3 | 把 `platformio.ini` 固定版本（`DD-5`/`9.7`）并入 P0 | ✅ 建议做（4 行改动，构建可复现；可独立 commit） |
| C-4 | 测试工具（§3.5）改造是否并入 P0 | ✅ 建议并入 —— 否则 P0 的"验证"没有可信工具 |

### 11.3 已知**不在** P0 范围（防蔓延）

| 不做 | 归属 |
|---|---|
| 一机一账号 `dev_<device_id>` + 凭据 slot 池 | **P1** |
| Homie Bridge / `$description` | **P2** |
| 云端按 device_id 落 D1 + 遥测降频**实现** | **P3** |
| 修 `BT-1`（云端回 `log_ack`） | **P3**（Plan D10） |
| 轮换 MQTT 密码 | **P1**（与一机一密一起） |
| 新增 EventId（身份日志埋点） | **日志协议解冻后**（与 `LV-1`/`LV-3` 一起） |
| 双发 / 双订阅 legacy | **不做**（§3.6 论证） |
| 重排分区表 / 新增 NVS 分区 | **不做**（§1.2） |
| APP 开发（Android 原生 / WebView 壳 / 前端 UI） | **P4 / P5 / P6** |
| 云端**完整设备管理**（用户 / 绑定 / 凭据 / 多设备列表） | **P1 / P3** |
| 遥测（telemetry）**实现** | **P3**（P0 只冻结策略：§1.5 / §6） |
| 心跳（Device State Heartbeat）**实现** | **P3**（`readme.md` §六 仍为待开发项） |
| **production mode 管理**（模式标志 / 配置项 / 构建 profile） | **后续阶段**（§3.3.1 只加约束） |
| **identity migration 实现**（显式迁移流程） | **P1+**（§2.3.1 只定义规则） |

---

## 附录 A：本设计核实过的代码事实（防止后续会话凭记忆推翻）

| # | 事实 | 证据位置 |
|---|---|---|
| 1 | 分区表已 100% 分配，`nvs` 分区为 `0x9000 / 0x5000` | `partitions.csv` |
| 2 | 三个 topic getter + client_id getter 的真实实现与兜底值 | `config_manager.cpp:3681–3712` |
| 3 | topic 静态缓存与使用点（无需改） | `cloud_manager.cpp:60/65/66/68` · `:333` · `:550` · `:554` · `:1745` · `:1775` · `:1814` · `:1822` |
| 4 | topic / client_id 的读取与打印点 | `cloud_manager.cpp:1462–1479` |
| 5 | DEF-2 明文打印的 4 处（共 6 行 printf） | `cloud_manager.cpp:1383–1390` · `:1463–1470` |
| 6 | log topic 兜底硬编码 | `cloud_manager.cpp:1802–1805` |
| 7 | `esp_read_mac()` 声明存在 | `framework-arduinoespressif32/tools/sdk/esp32s3/include/esp_hw_support/include/esp_mac.h:129` |
| 8 | Arduino core = 2.0.17（IDF v4.4） | `framework-arduinoespressif32/package.json` |
| 9 | 初始化顺序（插入点上下文） | `main.cpp:436/468/516/517/518/522/523/527/540/546/549/575` |
| 10 | 项目内**零** NVS 代码（无 `Preferences` / `nvs_flash` / `esp_read_mac` 调用） | 全 `src/` grep |
| 11 | ACL PUT 覆盖某用户全部规则可用 | `EMQX_Assets/scripts/emqx-api.mjs:211` `replaceUserAclRules()` |
| 12 | 兜底 `deny #` 已存在于部署 | `EMQX_Assets/config/acl-rules.proposed.json` `_doc` |
| 13 | 测试工具硬编码 legacy topic | `tools/mqtt_*.py:24–32` · `test/mqtt_log_probe.py:84–193` |

## 附录 B：本设计对上游文档的回写项（避免两处维护）

| 上游文档 | 需回写 |
|---|---|
| `Cloud-APP-Platform-Plan.md` §15 | D1/D2/D3/D5/D7 由【建议】→【已定】，并链到本文；D2 需注明**偏离理由** |
| `Cloud-APP-Platform-Plan.md` §3.1 | 补方案 A 的**回滚价值**（本轮新增论据） |
| `Cloud-APP-Platform-Plan.md` §13.3 | `client_id` 的后果由"身份漂移风险"**上调为"双设备互相踢线缺陷"** |
| `cloud_protocol.md` §1.1 | 补"`<device_id>` 由固件显式替换"一句 |
| `docs/issues/未修复的问题.md` | `DEF-2` / `9.7`（`DD-5`）挂上"P0 处理"标记；`P0-2`（轮换密码）标注 P1 |
