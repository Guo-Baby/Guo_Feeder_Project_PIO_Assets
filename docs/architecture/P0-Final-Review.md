# P0 Final Review —— 设备身份 + Topic V3 隔离

> **状态**：P0 已完成、已上板、已端到端验证、**已冻结**
> **设计依据**：`docs/architecture/P0-设备身份与Topic隔离设计.md`（P0-Design v1.2, FROZEN）
> **施工工单**：`docs/architecture/P0-实现清单.md`
> **撰写日期**：2026-10-02
> **本文用途**：P0 的**验收快照** —— 记录"已冻结的基线是什么"、"验证到什么程度"、
> "哪些限制是**已知且有意保留**的"。P1 规划以本文为起点。

---

## 1. P0 Final Baseline

### 1.1 Identity（设备身份）

| 项 | 冻结值 |
|---|---|
| `device_id` | **MAC 派生**（`aabbccddeeff`，12 位小写 hex，无分隔、无前缀） |
| 派生来源 | **`esp_read_mac(mac, ESP_MAC_WIFI_STA)`**（固定用 STA MAC；**不用** `WiFi.macAddress()`） |
| 存储 | **NVS**，复用现有 `nvs` 分区（`0x9000`, `0x5000`） |
| namespace | **`gfid`** |
| keys | **`did`**（ASCII 字符串 / 12 字符 / 小写 hex）· **`dver`**（`uint8` 派生算法版本） |
| 身份层级 | `did` = **设备身份唯一依据**；`dver` **只表示派生算法版本**，**不参与身份判定** |

**身份三层来源（不做等号关系）**

| 角色 | 职责 |
|---|---|
| MAC | 初始生成来源 + 恢复来源 |
| NVS (`gfid/did`) | **运行期身份锚点（identity anchor）** |
| 云端注册后的 `device_id` | **最终业务身份依据** |

**生命周期语义（5 场景）**

| 场景 | 行为 |
|---|---|
| 首次启动（无 `did`） | 由 MAC 派生 → 写入 NVS |
| 后续启动 | **以 NVS 为准**（不回写、不重派生） |
| NVS 丢失 | 重新派生同一值 —— **仅在未迁移、云端绑定未变化的前提下成立** |
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
| current limitation | **共享凭据**（两个设备账号被多台设备共用 ⇒ P0 阶段 ACL 层**无法隔离两台设备**） |
| 未使用 | `homie/5/<device_id>/#`（P2 预留，**未添加**） |

### 1.4 构建环境（C-3 固定）

`platformio.ini` 已固定：`espressif32@7.0.1` + `platform_packages`（framework/toolchain/tools 全锁）
+ `lib_deps` 精确版本 ⇒ 重新 clone 后 `pio run` 结果一致。

---

## 2. Known Limitations（**已知且有意保留**，非缺陷）

| # | 限制 | 说明 / 归属 |
|---|---|---|
| L-1 | **无 per-device credential** | 设备账号共享 ⇒ 无法在 ACL 层区分两台设备。**P1 解决**（一机一凭据） |
| L-2 | **无 production mode enforcement** | 设计 §3.3.1 要求"生产必须含 `<device_id>`，否则阻止 MQTT 上线"，P0 **只落设计约束、不实现**（不新增配置项/模块/EventId）；P0 期靠人工核对 |
| L-3 | **legacy ACL 仍存在** | `guo_feeder/{down,up,log}` 三条过渡期保留（配置级回滚需要）；生产前需收窄/移除 |
| L-4 | **无 identity migration** | 身份迁移（换主控 / 云端重新绑定）无流程；P0 只保证"不自动破坏身份" |
| L-5 | **无 OTA 模块** | 固件当前**不含任何 OTA 代码**；"升级不改身份"只能用重烧 app 分区等价验证 |
| L-6 | **遥测未实现** | D7 只冻结策略表（目标 < 1200 行/设备/天）；上报功能随 `readme.md` §六 Device State Heartbeat 落地，落点预登记 `mqtt.json.telemetry` |
| L-7 | **S-5 / S-9 场景不可构造** | 见 §3.2 —— 属**设计的稳健性证据**，非测试缺口 |

---

## 3. Verification

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
| S-4 NVS 丢失 → **重新派生同一 `device_id`** | ✅ |
| S-6 topic 渲染（`client_id` + 三 topic 全 V3，零 WARN） | ✅ |
| S-7 回滚通路（legacy 模板 ⇒ 原样放行 + 一次 WARN，仅 dev/test） | ✅ |
| S-8 `client_id` 实值（`dev_<device_id>`；配置回包**不含** `device_id` 字段） | ✅ |
| S-10 命令链路回归 · S-11 升级（重烧 app 分区）后身份不变 | ✅ |
| M-1 单设备上线 + MQTT 下发（memory / flash / config_query） | ✅ |
| M-3 越权被拒（设备账号 SUBACK = `(1,128,1)`：自己 down ALLOW / 他人 down DENY） | ✅ |
| **S-5 `did` 与 MAC 不一致** | ⛔ **不可构造**（下） |
| **S-9 `client_id` 超长** | ⛔ **不可构造**（下） |
| M-2 双设备互踢 | ⏳ 无第二台设备 |

**S-5 不可构造**：等长篡改 NVS 镜像中的 `did` 后写回 ⇒ 设备报 `nvs_get_str fail: did NOT_FOUND`
（**NVS 自带完整性校验**，篡改条目被判无效）；且固件**不提供任何修改 `did` 的接口**
（设计明确禁 `device_identity_reset`）⇒ 「`did` 存在但与 MAC 派生不一致」在**正常运维路径下不可达**。

**S-9 不可构造**：配置命令链路**先行拒绝**超长值
（`CONFIG_ENQUEUE_INVALID` → `{"e":6,"m":"invalid config param or unsupported value type"}`，
随后 `config_save` 报 `no dirty module`）⇒ 值根本没写入设备。
该分支由编译期自检 `topic_renderer_selftest()` 的 `too_long` 用例覆盖（DEBUG 构建实测 PASS）。

### 3.4 端到端（P0 的核心目标）

| 验证 | 结果 |
|---|---|
| 设备订阅 V3 topic 是否真的生效 | EMQX clients 中 `subscriptions` **0 → 1**（ACL 修复 + 重连后） |
| 经 MQTT 下发命令 | `guo_feeder/<device_id>/down` 下发 memory / flash / config_query ⇒ **设备正确执行并回包** |
| 越权隔离 | 设备账号订阅**他人** `down` ⇒ **DENY(0x80)** |
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
| 4 | 一机一凭据（消除 L-1） | **P1 核心** |
| 5 | production mode 检查（消除 L-2） | 后续阶段（P0 不决策其实现方式） |
| 6 | legacy ACL 收窄（消除 L-3） | 以"P1 凭据 + P3 落库"完成为终点 |

---

## 6. 结论

**P0 已冻结。** 设备身份**一机一值、NVS 持久、升级不变**；MQTT **一机一 topic、client_id 唯一**（消除互相踢线）；
ACL **device-scoped、越权被拒**；端到端下发已验证。已知限制均为**有意保留**（L-1..L-7），
其中 L-1（共享凭据）、L-2（生产模式）、L-4（身份迁移）构成 **P1 的主要输入**。
