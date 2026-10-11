# P3 · `WP4-FW-TELEMETRY` —— **冻结裁决记录**（Freeze Decision Record）

> ## 🟢 `FROZEN INPUT` · **仅文档 · 未改动任何 `src/`**
>
> | 项 | 值 |
> |---|---|
> | **本文档性质** | **人工裁决留痕** —— 使 `WP4-FW-TELEMETRY` 由 **`PROVISIONAL`** 进入 **`FROZEN INPUT`** |
> | **基线** | 固件仓 HEAD = **`baeb177`**（工作树 clean） |
> | **输入** | `docs/review/P3-WP4-Firmware-Telemetry-Design-Freeze-Review.md`（评审 `8385125`） |
> | **本记录的角色** | **后续实施任务的冻结输入**（`schema` / `value` 编码 / `seq` / 范围 / 路由） |
> | **本文档不做的** | ❌ 不改 `src/` · ❌ 不改 Cloud/Worker · ❌ 不改 EMQX · ❌ 不改 migration · ❌ 不改冻结设计文档 · ❌ 不启用 `GF_TELEMETRY_ENABLED` · ❌ 不 push |
>
> **⚠️ 本记录中的裁决为权威**。若与评审文档的建议冲突（评审 §3.3/§3.4 曾**建议** `schema_version` 保留、`seq` **持久化**），**以本记录为准**。
> `DT:WP-4 = FROZEN` · `CF-1 = RESOLVED` · `CF-2 = BLOCKED`（本轮**不变**）· `Phase C = PLANNED / BLOCKED`（**不变**）。
>
> **🔄 更新记录（2026-10-12）**：
> · **`H-WP4-FW-08 Resource Budget Override Decision`** —— 资源预算覆盖 ⇒ **`H-WP4-FW-08` = `CLOSED`**，**资源 blocker 解除**（`CF-2` 其余 blocker 保留）。
> · **`H-WP4-FW-06 Telemetry Configuration Freeze`** —— 配置模型冻结 ⇒ **`H-WP4-FW-06` = `CLOSED`**。
>
> **⇒ 本轮更新后：`H-WP4-FW-05` / `06` / `07` / `08` 全部 `CLOSED`。**

---

## §0 裁决总览

| 裁决项 | 结论 | 状态 |
|---|---|---|
| **`H-WP4-FW-05`** — Envelope schema + `value` 编码 | **冻结**（字段裁减 + 三态编码） | 🟢 **`CLOSED`** |
| **`H-WP4-FW-07`** — telemetry `seq` 语义 | **冻结**（boot lifetime · 不持久化 · 不复用 log seq） | 🟢 **`CLOSED`** |
| **`H-WP4-FW-08`** — Flash 预算处置 | **允许优化实施**（方向 A/B/E；禁 C/D）⇒ **A+B+E 用尽后实测仍超** ⇒ **人工批准资源预算覆盖（Override）** | 🟢 **`CLOSED`**（见 `## H-WP4-FW-08 Resource Budget Override Decision`） |
| **首版 telemetry 范围** | **KEEP** = temperature / humidity / weight；**DEFER** = valve / wifi / online / time.valid | 🟢 **冻结** |
| **`/up` 路由** | topic **保持** `guo_feeder/<device_id>/up`；**分流责任 = EMQX Rule** | 🟢 **冻结** |
| **`H-WP4-FW-06`** — `mqtt.json.telemetry` 字段与默认值 | **冻结配置模型**（`enable` + `interval_ms`，仅 2 键） | 🟢 **`CLOSED`**（见 `## H-WP4-FW-06 Telemetry Configuration Freeze`） |

---

## §1 `H-WP4-FW-05` —— Envelope schema 冻结

**裁决**：冻结 Envelope schema 为 **5 字段**：

```json
{ "ts":…, "type":"…", "value":…, "seq":…, "schema_version":… }
```

| 处理 | 字段 | 说明 |
|---|---|---|
| **保留** | `ts` | Unix 秒 |
| **保留** | `type` | 观测类型标识符（见 §1.2） |
| **保留** | `value` | 观测值（编码规则见 §2） |
| **保留** | `seq` | 序号（语义见 §3） |
| **保留** | `schema_version` | 解码版本锚点（v1 = `1`） |
| **🔴 删除** | ~~`device_id`~~ | **改用 MQTT topic identity**（`guo_feeder/<device_id>/up` 的第 2 段） |
| **🔴 删除** | ~~`source`~~ | 不再随信封发送 |

**说明（裁决定义）**：

1. **`device_id` 使用 MQTT topic identity** —— 设备身份**只由 topic 承载**，信封内**不重复**。
   - 与 `DT:WP-4` `ID-3`（`device_id` 由 topic 第 2 段提取、payload 内**忽略**）**对齐**；
   - 与云端 `telemetry_history.device_id NOT NULL` **不冲突** —— 该列由**服务端**按 topic/auth 上下文填充；
   - ⇒ **关闭**评审发现 `F-WP4-FW-TR-2`（信封多发 `device_id`）。
2. **`source` 删除** —— 来源信息由 `type` 的前缀域（`climate.*` / `weight.*`）**隐含**，不另设字段。
3. **信封不含 `boot_seq`** —— `seq` 语义见 §3（**boot lifetime**）。

### 1.1 Envelope v1 冻结形态（字段顺序即契约）

```
{"ts":<uint32>,"type":"<id>","value":<number|string|boolean>,"seq":<uint32>,"schema_version":<uint32>}
```

### 1.2 `type` 标识符

- 形态沿用评审 §3.1 的 **T1（点分命名空间）**：`<domain>.<metric>`，字符集 `[a-z0-9_]`，长度 ≤ 32。
- **v1 首版实际取值** = §5 的 **KEEP 集**：`climate.temperature` / `climate.humidity` / `weight.value`。
- **追加式演进**：新 `type` 只可新增，**永不改名、永不删除**（因云端幂等键含 `type`，`ux_th_dedup`）。
- 本记录**不另行变更** `type` 标识符字符串；如需改名须新裁决。

### 1.3 `schema_version` 保留

**保留**（拒绝评审 §3.3 的"v1 期省略"变体）：理由 = 云端 `telemetry_history.schema_version` 列存在（`NOT NULL DEFAULT 1`）+ `WP2:§3.3` 将 `value` 逻辑类型的解释权交给它 ⇒ 它是唯一前向兼容锚点。**v1 恒为 `1`**。

---

## §2 Value Encoding 冻结

**裁决**：**三态编码**，按 `value` 的**逻辑类型**决定 JSON 形态：

| 逻辑类型 | JSON 形态 | 适用 | 例 |
|---|---|---|---|
| **number** | **JSON number** | 物理量 | `23.50` · `1042.0` · `61.25` |
| **state** | **JSON string** | 多态枚举 | `"ready"`（词表由 `type` 绑定） |
| **boolean** | **JSON boolean** | 真/假 | `true` · `false` |

**🔴 明令禁止**：**用数字编码 boolean**（即禁止 `1.0` / `0.0` 表示真/假）。

⇒ **关闭**评审发现 `F-WP4-FW-TR-3`（原实现 `tele_emit_bool()` 出数字 `1.0/0.0`，与字符串编码自相矛盾）。

**配套约束**：

1. **单位不进载荷** —— 单位绑定在 `type` 上（如 `climate.temperature` = °C、`weight.value` = g），随 `type` 冻结。
2. **number 的小数位** —— 沿用 2 位小数 + 去尾零（评审 §3.2 描述，实现侧 `te_fixed2`）；若为整数语义则输出整数形。
3. **落库映射** —— 云端 `telemetry_history.value` 为**物理 `TEXT`**：JSON number → 十进制文本；JSON string → 原文；JSON boolean → `"true"`/`"false"` 文本。**逻辑类型由 `schema_version` 解释**（`WP2:§3.3` 不变）。
4. **v1 实际使用**：因 §5 的 KEEP 集**全为 number**，**v1 首版只会产生 `number` 与 `schema_version=1`**；`state` / `boolean` 编码规则**先行冻结**以备后续 `type` 启用。

---

## §3 `H-WP4-FW-07` —— `seq` 语义冻结

**裁决**：

| 项 | 冻结值 |
|---|---|
| **形态** | **boot lifetime `uint32` counter** |
| **持久化** | **不持久化**（不在 NVS / 不落盘） |
| **跨 boot** | **每次开机归零**（`telemetry_publisher_init()` 置 `0`） |
| **与 log 的关系** | **不复用 log `seq`**（不借用 LogManager 的 `boot_seq`/`seq`） |

> **⚠️ 本裁决为方案 A**（评审 §3.4 曾**推荐****方案 B = NVS 持久化单调计数**）。**按本记录执行方案 A。**

**派生后果（须显式知晓，见 §8 残留风险）**：

1. **`ts` 有效时安全** —— 云端幂等键 `(device_id, type, ts, seq)` 中 `ts` 为真实 Unix 秒，跨 boot 的 `ts` 不同 ⇒ 不碰撞。
2. **`ts` 无效（未同步，`ts = 0`）时存在碰撞面** —— 同一 `(device_id, type)` 在**不同 boot**、且均在**未同步状态**发出的第 `n` 条，会产生相同的 `(dev, type, 0, n)` ⇒ 后到者被 `INSERT OR IGNORE` **静默吸收**（评审 `R-WP4-FW-7`）。
3. ⇒ **`ts` 无效时的上报策略成为关键**：建议 **取值类在 `ts` 无效时不上报**（由 `H-WP4-FW-06` / 实施任务细化），以**规避**上述碰撞面。**本记录不单独裁决该项**，仅登记为前置约束。

---

## §4 `H-WP4-FW-08` —— 资源裁决

**记录当前资源状态**：

```
RESOURCE BLOCKED
  实测：telemetry 门禁增量 +2,496 B；剩余预算 636 B ⇒ 超 1,860 B
  （ceiling 14,336 B = P2-7 12,548 + LOG 1,152 + telemetry …）
```

**裁决**：**允许优化实施**（即：允许按下列方向改动 `src/`，以尝试消解超预算）。

**授权优化方向**：

| 方向 | 内容 | 评审依据 |
|---|---|---|
| **A** | **remove `device_id`**（+ `source`） | 评审 §2.3-A / §1.2（`F-WP4-FW-TR-2`） |
| **B** | **publisher optimization**（表驱动派发 / 删未用访问器 / 与既有工具共址） | 评审 §2.3-B |
| **E** | **首版减少非核心 telemetry capability** | 评审 §2.3-E / §5 范围冻结 |

**🔴 明令禁止**：

| 禁止项 | 说明 |
|---|---|
| **ArduinoJson replacement** | 禁止用 ArduinoJson 替换手写 JSON 构建（评审 §2.3-C） |
| **CBOR protocol replacement** | 禁止将 telemetry payload 改为 CBOR / 二进制定长（评审 §2.3-D） |

⇒ ⇒ 评审候选 **C** 与 **D** 均**被禁止**。**payload 保持 JSON 文本形态**。

**状态**：`H-WP4-FW-08` = **`CLOSED WITH GUARD`**（**资源态仍为 `RESOURCE BLOCKED`**；guard 解禁 = 仅限 A/B/E 三项优化）。

> ⚠️ **若 A/B/E 优化后实测仍超预算** ⇒ 须**再次人工裁决**（重裁 ceiling 或进一步削减）。见 §8。
>
> ✅ **该分支已发生并已裁决（2026-10-12）**：A+B+E 用尽后实测增量 **+1,648 B > 636 B**（超 1,012 B）⇒ 人工**批准资源预算覆盖**。
> ⇒ 本节状态**已被下列章节取代**：**`## H-WP4-FW-08 Resource Budget Override Decision`**（`H-WP4-FW-08` = `CLOSED`，资源 blocker 解除）。

---

## §5 首版 telemetry 范围冻结

| 处置 | 观测 | 逻辑类型 | `type`（T1） |
|---|---|---|---|
| ✅ **KEEP** | **temperature** | number | `climate.temperature` |
| ✅ **KEEP** | **humidity** | number | `climate.humidity` |
| ✅ **KEEP** | **weight** | number | `weight.value` |
| ⏸️ **DEFER** | valve | boolean | `valve.state` |
| ⏸️ **DEFER** | wifi | boolean | `wifi.state` |
| ⏸️ **DEFER** | online | state | `online.state` |
| ⏸️ **DEFER** | time.valid | boolean | `time.valid` |

**含义**：

1. 首版 `telemetry_task()` **只保留 3 个取值类分支**（temperature / humidity / weight，含阈值 + 抽样兜底）。
2. **5 个边沿/状态类分支全部 DEFER** ⇒ 首版**不产生** `state` / `boolean` 值（与 §2 的"v1 实际只用 number"一致）。
3. **DEFER ≠ 删除**：相关编码规则（§2）与词表（§1.2）**已冻结**，后续启用**无需重新冻结协议**，仅需恢复对应 capability。
4. ⇒ **关闭/收敛**评审发现 `F-WP4-FW-TR-4`（未接 EventManager）—— 首版明确不含事件类。

---

## §6 `/up` 路由冻结

**裁决**：

| 项 | 冻结值 |
|---|---|
| **上行 topic** | **保持** `guo_feeder/<device_id>/up`（**零改动**） |
| **分流责任** | **EMQX Rule** |
| **Worker** | **不负责** command / telemetry 判断；**不解析 payload** 分流 |
| **固件** | 不新增 topic；不改 topic renderer |

**说明**：

- 命令结果 / registry 结果 / telemetry **继续共用** `/up`；
- 因 topic 不变且共用，**分离只能发生在 EMQX Rule 层** ⇒ 与评审 §4.3 的 **A1（Rule 层谓词分流）** 一致；A2/A3（topic 拆分）**被"topic 保持"排除**。
- 具体的 **EMQX Rule 范围 / 谓词**未在本记录展开 ⇒ 仍归 **`H-WP4C-03 = OPEN`**。
- 与已 `CLOSED` 的 `H-WP4-FW-01`（方案 A：双 Action 分流 / EMQX Rule 层 / Worker 不解析）**一致**。

---

## §7 裁决状态汇总（回填）

| ID | 主题 | 本记录前 | **本记录后** |
|---|---|---|---|
| `H-WP4-FW-05` | `type` 词表 + `value` 编码 + `schema_version` | `OPEN` | 🟢 **`CLOSED`**（§1 + §2） |
| `H-WP4-FW-06` | `mqtt.json.telemetry` 字段集与默认值 | `OPEN` | 🟢 **`CLOSED`**（`## H-WP4-FW-06 Telemetry Configuration Freeze`；仅 2 键 + 明确 Non-goals） |
| `H-WP4-FW-07` | telemetry `seq` 语义 | `OPEN` | 🟢 **`CLOSED`**（§3，方案 A） |
| `H-WP4-FW-08` | Flash 预算处置 | `CLOSED WITH GUARD`（🔴 `RESOURCE BLOCKED`） | 🟢 **`CLOSED`**（A+B+E 后仍超 ⇒ **资源预算 Override 已批准**；见 `## H-WP4-FW-08 Resource Budget Override Decision`） |
| `H-WP4-FW-09` | `/up` telemetry 载荷形态文档同步 | `OPEN` | 🔴 **`OPEN`**（§1 已给冻结 schema ⇒ 可同步，但**本轮未做**） |
| `H-WP4C-03` | EMQX Rule 分流机制（rule scope） | `OPEN` | 🔴 **`OPEN`**（§6 已定"责任方 = EMQX Rule"，**具体 scope 未定**） |
| `H-WP4C-04` / `05` / `06` | migration 授权 / Action 切换 / 部署顺序 | `OPEN` | 🔴 **`OPEN`**（不变） |

**评审发现收敛**：`F-WP4-FW-TR-2` ✅ 关闭（删 `device_id`）· `F-WP4-FW-TR-3` ✅ 关闭（禁数字编码 bool）· `F-WP4-FW-TR-4` ✅ 收敛（首版 DEFER 事件类）· `F-WP4-FW-TR-1` / `-TR-5` ⏸️ 不变（延续）。

---

## §8 对后续实施任务的净影响（冻结输入 → 待办）

> 本节仅**登记**；**本轮不实施**（禁止修改 `src/`）。

| # | 变更 | 目标文件 | 依据 |
|---|---|---|---|
| **I-1** | 信封**删除 `device_id`** 字段 | `telemetry_envelope.h`（struct + 组包） | §1 |
| **I-2** | 信封**删除 `source`** 字段 | 同上 | §1 |
| **I-3** | `value` 编码改为三态；**bool 出 JSON boolean**（禁 1/0） | `telemetry_envelope.h`（新增 bool 写出）+ `telemetry_publisher.cpp`（`tele_emit_bool`） | §2 |
| **I-4** | `telemetry_task()` **删 4 个 DEFER 分支**（valve/wifi/online/time.valid），仅留 3 个取值类 | `telemetry_publisher.cpp` | §5 |
| **I-5** | `online.state` 相关（`tp_online_should_emit` / `tp_online_state_name` / `TP_ONLINE_*`）随之**暂不接线** | `telemetry_policy.h` | §5 |
| **I-6** | `seq` **保持** boot lifetime（**已符合** ⇒ 无需改码） | `telemetry_publisher.cpp` | §3 |
| **I-7** | 优化方向 **A / B** 叠加实施 | `telemetry_envelope.h` / `telemetry_publisher.cpp` | §4 |
| **I-8** | **保持** JSON 文本 payload；**不得**引入 ArduinoJson / CBOR | 全局 | §4 |
| **I-9** | 文档：`cloud_protocol.md §4` 同步 telemetry 上行形态（5 字段冻结 schema） | `docs/interfaces/cloud_protocol.md` | `H-WP4-FW-09` |

**预期效果（粗估，须实测）**：I-4（E）≈ −600 B · I-1/I-2（A）≈ −60…−120 B · I-7（B）≈ −350…−450 B ⇒ 合计 ≈ **−1,000…−1,200 B**。

> 🔴 **残留风险 `R-FRZ-1`（重要）**：即使执行全部授权优化，增量预计仍 ≈ **1,300…1,500 B > 636 B 预算** ⇒ **很可能仍判 `RESOURCE BLOCKED`**。届时须再次人工裁决（重裁 ceiling / 进一步削减 capability）。**本记录不预先授权任何额外削减。**

> 🟠 **残留风险 `R-FRZ-2`**：§3 方案 A（`seq` 不持久化）+ `ts` 无效 ⇒ 幂等碰撞面（§3 派生后果 2）。建议实施时**在 `ts` 无效时不上报取值类**以规避；**该项待 `H-WP4-FW-06` 或实施任务确认**。

---

## §9 剩余 `HUMAN_REQUIRED`

| ID | 主题 | 状态 | 阻塞对象 |
|---|---|---|---|
| **`H-WP4-FW-09`** | `/up` telemetry 载荷形态文档同步 | 🔴 `OPEN` | 文档一致性（可立即做） |
| **`H-WP4C-03`** | EMQX Rule 分流（rule scope / 谓词） | 🔴 `OPEN` | 端到端链路 |
| **`H-WP4C-04`** | 远端 `0003` migration 授权 | 🔴 `OPEN` | CF 侧 |
| **`H-WP4C-05`** | EMQX Action 切换授权 | 🔴 `OPEN` | 端到端链路 |
| **`H-WP4C-06`** | 部署顺序 | 🔴 `OPEN` | 端到端链路 |

> ✅ **已解除（2026-10-12）**：
> · **`H-WP4-FW-08`（余项）** —— 优化后仍超预算，已由 `## H-WP4-FW-08 Resource Budget Override Decision` 裁决（`H-WP4-FW-08 = CLOSED`）。
> · **`H-WP4-FW-06`** —— 配置模型已由 `## H-WP4-FW-06 Telemetry Configuration Freeze` 冻结（`H-WP4-FW-06 = CLOSED`）。
> ⇒ 两者**不再列为剩余项**。**`R-FRZ-2`（`ts` 无效时的上报策略）不改判为配置项**，仍按**实施细节**跟踪（见 `## H-WP4-FW-06 Telemetry Configuration Freeze` §Status）。

---

## §10 边界核对（**零违规**）

| 禁止项 | 状态 |
|---|---|
| 修改 `src/` | ✅ **未改** |
| 修改 Cloud / Worker | ✅ **未改** |
| 修改 EMQX | ✅ **未改** |
| 修改 migration | ✅ **未改** |
| 修改冻结设计文档 | ✅ **未改** |
| 启用 `GF_TELEMETRY_ENABLED` | ✅ **未改**（保持默认 0） |
| 改 `DT:WP-4` / topic | ✅ **未改** |
| 新增文档 | ✅ **唯一** = `docs/review/P3-WP4-Firmware-Telemetry-Decisions.md` |

---

## §11 与本记录的关联文档

| 文档 | 关系 |
|---|---|
| `docs/review/P3-WP4-Firmware-Telemetry-Design-Freeze-Review.md`（`8385125`） | **输入**（评审 + 建议 A/B/C/D/E） |
| `docs/review/P3-WP4-Firmware-Telemetry-Implementation.md`（`baeb177`） | 被冻结对象的实施记录（`RESOURCE BLOCKED` 出处） |
| `docs/review/P3-WP4-Firmware-Integration-Design-Review.md` | `H-WP4-FW-*` 清单与 §9 裁决留痕 |
| **实施提交**（A+B+E） | `d45f7c3`（信封对齐冻结 schema）· `594ff04`（首版范围裁剪）· `9dfc7b5`（publisher Flash 优化） |
| `docs/architecture/P3-WP1-Telemetry-Policy-Final.md` | 设备侧策略权威（阈值不冻结） |
| `Cloudflare_Assets/guo-feeder-api/migrations/0003_p3_history_tables.sql` | 云端表结构 + `ux_th_dedup` 幂等键 |
| `docs/architecture/P2-0-architecture-review-and-design-freeze.md` | 被 Override 的原 Flash 增量 ceiling 出处（**本记录不修改该文档**） |

---

## H-WP4-FW-08 Resource Budget Override Decision

> **裁决日期**：2026-10-12 · **性质**：**资源预算覆盖（Override）** —— 关闭 `telemetry v1` 的 `RESOURCE BLOCKED`。
> **本节仅记录裁决**：❌ 不改 `P2-0` 冻结文档 · ❌ 不改 `src/` · ❌ 不启用 `GF_TELEMETRY_ENABLED`。

### 1. Decision（裁决）

**人工批准 `telemetry v1` 超出原 `P2-0` Flash ceiling 继续实施。**

- `telemetry v1` 的实测增量 **`+1,648 B`** 被**接受**，**不再受**"剩余增量预算 636 B"约束。
- 原增量预算（`P2-0 §17.2`：P3 **Flash 合计增量 `≤ 14 KiB` / 14,336 B**）对 **`telemetry v1` 单项予以豁免（waiver）**。
- **⚠️ 覆盖范围严格受限**：仅针对 **`telemetry v1` 的 Flash 增量**。`P2-0` 的其余资源约束（**DRAM / PSRAM / 栈 / CPU / MQTT / 网络**）**全部不变**；`P2-0` 冻结文档本身**未修改、不修改**。

**实测依据**：

| 项 | 值 |
|---|---|
| baseline Flash（门禁 OFF，`9dfc7b5`） | **1,391,877 B** |
| telemetry enabled Flash（`GF_TELEMETRY_ENABLED=1`） | **1,393,525 B** |
| **enabled 增量** | **`+1,648 B`** |
| 原剩余增量预算 | 636 B |
| **超预算** | **+1,012 B** |
| app 分区总容量 | 2,097,152 B |
| **启用后总占用率** | **66.4 %**（余 **703,627 B / 33.6 %**） |

### 2. Reason（理由）

| # | 理由 |
|---|---|
| **R1** | **原 ceiling 来自早期阶段估算** —— `P2-0` 原始预算基于**更早、更小**的实现范围；`P2-0.2 Flash Budget Amendment` 本身即因范围扩大把增量上限由 `+8,704 B` 修订为 `14,336 B` ⇒ **同性质的"范围↔预算"再修正已有先例**。 |
| **R2** | **当前固件实际 Flash 使用仍处安全范围** —— 实测总占用 **1,393,525 / 2,097,152 = 66.4 %**，app 分区仍余 **703,627 B（33.6 %）**，远高于任何合理安全阈值。 |
| **R3** | **`telemetry v1` 属核心可观测性能力** —— 它是云端 `telemetry_history` 的唯一数据源（`DT:WP-4` ingest 契约）；缺失则整条遥测链路无输入，P3 的"历史/时序"目标不可达。 |
| **R4** | **为避免继续削减 telemetry capability** —— 授权手段 **A+B+E 已用尽**（增量 `2,496 → 1,648 B`，累计 `−848 B`）；再削只能动 **KEEP 集**（temperature / humidity / weight）或破坏**已冻结协议**，**代价高于资源收益**。 |
| **R5** | **纯代码优化已无可行空间** —— 授权范围内最优约 `−1,000…−1,400 B`，仍 > 636 B ⇒ "继续优化即可满足"的路径**客观不存在**（设计评审 §2.5 已预判）。 |

### 3. New policy（新资源政策）

| 维度 | 旧 | **新** |
|---|---|---|
| Flash 增量约束 | 受 P3 合计增量 ceiling（剩余 636 B） | **`telemetry v1` 不再受增量预算限制** |
| 资源检查方式 | 增量对账（delta vs ceiling） | **整体固件安全余量检查**（总占用 vs app 分区） |
| 适用范围 | — | **仅 `telemetry v1`**；其他 P3 项仍按原增量对账 |

> **Firmware total resource safety remains mandatory.**
> （**整体固件资源安全仍为强制要求** —— 增量预算的豁免**不等于**资源安全要求的豁免。）

### 4. Guard 护栏（**强制 · 随本裁决生效**）

| ID | 护栏 | 现行实测 | 阈值 / 动作 |
|---|---|---|---|
| **G-1** | **Flash 总占用不得接近 partition ceiling** | **66.4 %**（1,393,525 / 2,097,152） | 使用率 **≥ 85 %（余量 < 15 %）⇒ 必须停止新增并重新评估** |
| **G-2** | **RAM 仍需通过现有检查** | 132,752 / 327,680（**40.5 %**） | 沿用既有 DRAM / 栈 检查，**不放宽** |
| **G-3** | **后续大型功能增加必须重新评估** | — | 任何新 capability / 新模块上线前**重新执行整体余量评估**；**不得默认继承本豁免** |
| **G-4** | 不得借此放宽其他冻结约束 | — | PSRAM / 栈 / CPU / MQTT / 网络 / **协议契约** 全部不变 |

### 5. Status（状态）

| 项 | 变更 |
|---|---|
| **`H-WP4-FW-08`** | 🟠 `CLOSED WITH GUARD` ⇒ 🟢 **`CLOSED`** |
| **资源态** | 🔴 `RESOURCE BLOCKED` ⇒ 🟢 **解除**（`telemetry v1` 增量已获批准） |
| **`CF-2`** | 🔴 `BLOCKED` ⇒ 🔴 **仍 `BLOCKED`** —— **资源 blocker 已移除**；仍受阻于 **`H-WP4-FW-09`** + **`H-WP4C-03 / 04 / 05 / 06`** |
| `GF_TELEMETRY_ENABLED` | **保持 `0`（默认关闭）** —— 本裁决**不**启用 telemetry；启用属**后续实施任务** |

---

## H-WP4-FW-06 Telemetry Configuration Freeze

> **裁决日期**：2026-10-12 · **性质**：**配置模型冻结** —— `H-WP4-FW-06` 由 `OPEN` → `CLOSED`。
> **本节仅记录裁决**：❌ 不改 `src/` · ❌ 不改 `data/config/` · ❌ 不改 `ConfigManager` · ❌ 不启用 `GF_TELEMETRY_ENABLED`。

### Frozen fields（冻结字段）

`mqtt.json` 新增对象 **`telemetry`**，**恰含 2 个键**（无其他键，无嵌套）：

| 键 | 类型 | 语义 |
|---|---|---|
| **`telemetry.enable`** | `bool` | **telemetry capability 是否运行** |
| **`telemetry.interval_ms`** | `uint32` | **周期采样间隔（毫秒）** |

**冻结形态（逐字）**：

```json
{
  "telemetry": {
    "enable": false,
    "interval_ms": 300000
  }
}
```

### Default value（默认值）

| 键 | **冻结默认值** |
|---|---|
| `telemetry.enable` | **`false`** |
| `telemetry.interval_ms` | **`300000`**（= 5 min = `5 × 60 × 1000`） |

> 缺键行为：**等同默认值**（`enable` 缺省 ⇒ `false` ⇒ **不发送**）。⚠️ 注意既知坑：**配置字段只对"读过该字段的固件"生效** ⇒ 配置仅在**已含 telemetry 的固件**上有意义。

### Runtime behavior（运行行为）

| 条件 | 行为 |
|---|---|
| **`telemetry.enable = false`**（默认） | **telemetry publisher 不发送** —— 不产生任何上行报文（`telemetry_task()` 直接返回） |
| **`telemetry.enable = true`** | 按**已冻结 schema** 发送：§1 Envelope v1（5 字段）+ §2 `value` 三态编码 + §5 首版范围（temperature / humidity / weight）；周期采样按 **`interval_ms`** |
| **修改配置** | **遵循 `ConfigManager` restart policy** —— `config_set` 只改 RAM + 置 dirty + 排 5 min 重启；`config_save` 落盘并**安全重启**（10 s）。漏发 `save` 也会 5 min 后自动 save + 重启 |

**★ 两道门的关系（必须区分，勿混淆）**：

| 门 | 层 | 作用 | 默认 |
|---|---|---|---|
| **`GF_TELEMETRY_ENABLED`** | **编译期**（`platformio.ini` / `-D`） | 决定 telemetry **代码是否存在于镜像**（`0` = 空实现、0 Flash/RAM 成本） | **`0`（关闭）** |
| **`telemetry.enable`** | **运行时配置**（`mqtt.json`） | 在**已编译进镜像**的前提下，决定**是否实际发送** | **`false`（关闭）** |

> ⇒ **有效发送条件 = `GF_TELEMETRY_ENABLED == 1` __且__ `telemetry.enable == true`**（两者皆需成立）。

### Non-goals（v1 **明确不支持**）

| 不支持项 | 说明 |
|---|---|
| **cloud dynamic telemetry configuration** | 云端**不得**下发 / 修改 telemetry 配置（无对应下行命令） |
| **per-channel enable** | **无逐通道开关**（KEEP 集整体启停） |
| **threshold tuning** | **阈值不入配置** —— 设备侧 policy 保持**固件权威**（`WP1` 明令不冻结阈值） |
| **type list customization** | `type` **封闭词表不可配置**（追加式演进，永不改名） |
| value mapping 配置 | **不引入** |
| dynamic capability 配置 | **不引入** |

### 理由（设计意图）

1. **`type` / `value` schema 已冻结**（§1 / §2）⇒ 配置面**不应**再暴露这些维度（否则出现两套权威）。
2. **设备侧 policy 保持固件权威**（`WP1`）⇒ 阈值 / 周期语义**不由云端调参**。
3. **保持简单**：v1 只暴露 **1 个开关 + 1 个周期**，避免配置爆炸，并避免"配置字段只对读过该字段的固件生效"这一既知坑被放大。

### 实施映射（**细节 · 非协议 · 不改本冻结**）

> ⚠️ v1 固件当前使用**编译期常量**：`TP_CLIMATE_PERIOD_MS`（5 min）与 `TP_WEIGHT_PERIOD_MS`（15 min）。
> `interval_ms`（**单一整数**）与这两条周期的映射方式（统一取代 / 派生 / 分层）由**实施任务确定**，**不改变本冻结的字段集与默认值**。

### Status

| 项 | 变更 |
|---|---|
| **`H-WP4-FW-06`** | 🔴 `OPEN` ⇒ 🟢 **`CLOSED`** |
| **未覆盖项（显式声明）** | **`ts` 无效（未同步）时的上报策略** —— 本冻结**仅覆盖配置模型**；该策略**不是配置字段**，按**实施细节**跟踪（残留 `R-FRZ-2`），**不阻塞本项关闭**。 |
| `GF_TELEMETRY_ENABLED` | **保持 `0`（默认）** —— 配置冻结**不**启用 telemetry |

---

*本文档于 **2026-10-12** 建立、并于同日更新（追加 `## H-WP4-FW-08 Resource Budget Override Decision` 与 `## H-WP4-FW-06 Telemetry Configuration Freeze`），
为 **`WP4-FW-TELEMETRY` 的人工冻结裁决记录**；
**仅留痕，不含任何代码改动、配置变更或实现**。
建立基线 = 固件仓 HEAD **`baeb177`**；**本次更新基线 = `9dfc7b5`**（A+B+E 实施后）· CF 仓 **`9b25203`** · EMQX 仓 **`eec9444`**。
所有 `file:line` 与 Flash/RAM 数字取自基线实测，可逐条回查。*
