# P3 · `WP4-FW-TELEMETRY` —— 设计冻结评审（Design Freeze Review）

> ## 🟠 `DESIGN REVIEW ONLY` · **未实施 · 未改动任何 `src/`**
>
> | 项 | 值 |
> |---|---|
> | **本文档性质** | **设计评审 + 优化方案分析 + 冻结建议**（**仅分析**，不含代码改动） |
> | **基线** | 固件仓 HEAD = **`baeb177`**（工作树 clean） |
> | **被评审对象** | `WP4-FW-TELEMETRY`（Telemetry Publisher，`GF_TELEMETRY_ENABLED = 0` 默认关闭） |
> | **触发原因** | `CF-2 = BLOCKED` —— ① `H-WP4-FW-05` / `06` / `07` 均 `OPEN` ② `H-WP4-FW-08` = 🔴 **`RESOURCE BLOCKED`** |
> | **本文档不做的** | ❌ 不改 `src/` · ❌ 不改 Cloud Worker · ❌ 不改 EMQX · ❌ 不改 migration · ❌ 不改冻结设计文档 · ❌ 不启用 `GF_TELEMETRY_ENABLED` · ❌ 不 push |
>
> **本轮所有「建议 / 推荐」均为 `PROPOSAL`，不构成冻结协议。** 冻结须经人工裁决。
> `DT:WP-4 = FROZEN` · `Phase C = PLANNED / BLOCKED` · `CF-1 = RESOLVED` · `CF-2 = BLOCKED`（本轮**不变**）。

> ### 🟢 裁决已落地（2026-10-12 · 回填）
> 本评审的待裁项**已由人工裁决**，权威记录见 **`docs/review/P3-WP4-Firmware-Telemetry-Decisions.md`**：
> **`H-WP4-FW-05` = `CLOSED`**（信封裁减为 5 字段，**删 `device_id`/`source`**；`value` 三态编码，**禁数字编码 bool**）·
> **`H-WP4-FW-07` = `CLOSED`**（`seq` = **boot lifetime 计数，不持久化**）·
> **`H-WP4-FW-08` = `CLOSED WITH GUARD`**（**允许优化 A/B/E；禁 C[D] ArduinoJson/CBOR**）·
> **首版范围** = KEEP {temperature, humidity, weight} / DEFER {valve, wifi, online, time.valid} ·
> **`/up` 路由** = topic 保持，分流责任 = EMQX Rule。
> ⚠️ 本评审 §3.3「`schema_version` 建议保留」**被采纳**，但 §3.4「`seq` 建议持久化」**被否决**（改采方案 A）；**以 `…Decisions.md` 为准**。

---

## §0 评审范围与既有契约依据

| 依据 | 位置 | 用途 |
|---|---|---|
| 固件侧设计评审（含 `H-WP4-FW-*` 清单） | `docs/review/P3-WP4-Firmware-Integration-Design-Review.md` | §3.2 两级拆分 · §3.3 字段矩阵 · 风险登记 |
| 遥测策略（设备侧权威） | `docs/architecture/P3-WP1-Telemetry-Policy-Final.md` | 采样/阈值/词表冻结立场（**明令不冻结阈值 X**） |
| 遥测实施记录 + 资源门禁 | `docs/review/P3-WP4-Firmware-Telemetry-Implementation.md` | 实测 Flash/RAM 数字来源 |
| 遥测历史表 DDL | `Cloudflare_Assets/guo-feeder-api/migrations/0003_p3_history_tables.sql` | **幂等键 = `(device_id, type, ts, seq)`**（`ux_th_dedup`） |
| 协议文档 | `docs/interfaces/cloud_protocol.md` | §4 上行契约 · §8.2 log 已实现 |

**被评审的 4 个实现产物**（均在基线 `baeb177`）：

| 文件 | 行数 | 角色 |
|---|---|---|
| `src/services/telemetry_policy.h` | 131 | 纯决策（freestanding） |
| `src/services/telemetry_envelope.h` | 247 | 有界 JSON 组包（freestanding） |
| `src/services/telemetry_publisher.{h,cpp}` | 63 + 293 | glue（编译期门禁 `GF_TELEMETRY_ENABLED`） |
| `src/cloud/cloud_manager.cpp` `cloud_send_telemetry()` | 2558–2588 | 独立 transport |

---

## §1 当前实现评审（Phase A）

### 1.1 `telemetry_policy.h` —— ✅ 符合设计要求

| 检查项 | 结论 | 证据 |
|---|---|---|
| **freestanding** | ✅ **是** | 唯一包含 `<stdint.h>`（`policy.h:24`）；无 `Arduino.h` / 无 MQTT / 无 FS / 无动态分配 |
| **纯函数** | ✅ **是** | 全部 `static inline`，无副作用、无 static 状态；可穷举验证 |
| **是否存在无必要逻辑** | ⚠️ **2 处**（见下） | |

**无条件逻辑判定**：

1. **`tp_health_abnormal(long, long)`（`policy.h:126`）当前无调用点** —— `telemetry_publisher.cpp` 未引用它 ⇒ 属于**尚未接线的预留**（`WP1:§3.9` 健康异常）。在门禁 ON 时它被内联消除（无 Flash 代价），但**语义上仍是未完成的 capability**。→ 登记 `F-WP4-FW-TR-1`。
2. **`tp_cadence_due` 的 `period_ms == 0` 分支（`policy.h:75`）** 与 `tp_edge_accept` 的 `last_ms == 0` 特判（`policy.h:86`）都是必要的边界保护，**非冗余**。

**其他观察**：阈值常量（`TP_*`）是**草案默认值**，`WP1` 明令**不冻结阈值 X** ⇒ 这些值**不应**被当作契约；实现已在注释中显式声明 `PROVISIONAL`（`policy.h:12-18`），合规。

### 1.2 `telemetry_envelope.h` —— ✅ 可用，但 ⚠️ **与设计评审 §3.2 的字段矩阵存在偏差**

**实际输出字段（`envelope.h:202-236`，顺序即契约）**：

```
{"device_id":"…","ts":…,"type":"…","value":<num|str>[, "source":"…"],"seq":…,"schema_version":…}
```

**关键发现 `F-WP4-FW-TR-2`（🔴 契约偏差）**：

| | 设计评审 §3.2 / §5.2 字段矩阵 | 当前实现 |
|---|---|---|
| 必填 | **`ts` / `type` / `value` / `seq`** | `device_id` / `ts` / `type` / `value` / `seq` / `schema_version` |
| 可选 | `source` / `schema_version` | `source` |
| **`device_id`** | **不在矩阵中**（⇒ 应由 **topic 第 2 段**提取，`DT:WP-4` `ID-3`：payload 内 `device_id` **永远忽略**） | **逐条随信封发送** |

⇒ **当前实现多发送了 1 个被下游明确忽略的字段**。后果：**每条报文 +11 B 载荷 + 约 25 B 代码**，且与 `DT:WP-4` 的 `ID-3` 语义**重复**。**不是错误，但是冗余**。→ 交由 `H-WP4-FW-05` 裁决（见 §3.3）。

**是否存在可压缩字段 / 重复信息**：

| 字段 | 可压缩性 | 说明 |
|---|---|---|
| `device_id` | 🔴 **可删**（见 `F-WP4-FW-TR-2`） | 与 topic 同源、下游忽略 |
| `schema_version` | 🟠 **可删**（v1 期） | DDL 有 `DEFAULT 1`；但它是**唯一前向兼容锚点** ⇒ 建议保留（§3.4） |
| `source` | 🟢 已可选 | 已是"按需出现"，压缩空间≈0 |
| `ts` / `type` / `value` / `seq` | ⛔ 不可删 | DDL `NOT NULL` + 幂等键组成项（`seq`/`type`/`ts`） |

**是否影响未来协议冻结**：⚠️ **是** —— `type` 词表与 `value` 编码类型（number / string）**未冻结**，而 `value` 在 DDL 里是**物理 `TEXT`**、逻辑类型**由 `schema_version` 解释**（`WP2:§3.3`）。若 `type` / `value` 语义后改，**历史行将不可解释** ⇒ 必须一次性冻结（§3）。

### 1.3 `telemetry_publisher.cpp` —— ✅ 架构正确；⚠️ 1 处内部不一致

| 检查项 | 结论 | 证据 |
|---|---|---|
| **编译期开关** | ✅ 与 `GF_HOMIE_ENABLED` **同模式** | `publisher.cpp:18` `#if !GF_TELEMETRY_ENABLED` ⇒ 空实现；默认 0（`publisher.h:38-40`） |
| **runtime overhead** | ✅ **可忽略** | `telemetry_task()` 每轮 loop：① `!s_ready` 早退 ② `!cloud_is_connected()` 早退（离线时**仅 1 次 bool + 1 次计数**）③ 在线时 ~8 次 `state_get_*` + 常数级比较；**每轮最多 1 条**（`TP_MAX_EMIT_PER_TASK`） |
| **初始化方式** | ✅ 幂等、非阻塞 | `telemetry_publisher_init()`（`:119`）只**快照基线**（5 bool + 3 float + 3 时间戳），**不发首帧** ⇒ 避免上电噪声样本 |
| **System State 读取方式** | ✅ 纯只读 | `state_get_bool/float` + `state_is_valid`；**无写**、无注册回调（`policy.h` 无状态） |
| **阻塞** | ✅ 无 | 无 `delay()` / 无 `while` 死等；`char buf[320]` 在 `tele_emit()` 栈上（loopTask，余量充裕） |
| **动态分配** | ✅ 稳态无 | `tele_emit` 全程栈缓冲；仅 `cloud_send_telemetry` 的**首次兜底分支**构造 `String`（沿用既有模式） |
| **内部一致性** | ⚠️ **不一致** | `tele_emit_bool()`（`:112`）把 bool 编成 **数字 1.0/0.0**；而 `online.state` / `time.valid` 走 `tele_emit(..., str, ...)` 编成 **JSON string** ⇒ **同类语义两种 `value` 编码** → `F-WP4-FW-TR-3`（§3.3 解决） |

**⚠️ 一处与设计评审的能力差异（信息性，非缺陷）**：设计评审 §3 曾提"**EventManager 接入**（复用既有 `EVENT_*` 词表）"，而**当前实现未接入 EventManager** —— 它只用 5 个布尔边沿（`valve` / `wifi` / `time.valid` / `weight.error` / `online`）+ 3 个取值（temp / humid / weight）。这是**子集实现**（更省 Flash），差异须登记 → `F-WP4-FW-TR-4`。

### 1.4 `cloud_send_telemetry()` —— ✅ 独立 transport，零污染

| 检查项 | 结论 | 证据 |
|---|---|---|
| **是否独立 transport** | ✅ **是** | 单向 `json → cloud_mqtt_publish_binary()`（`:2583`）；**不排队 / 不落盘 / 不重试**（fire-and-forget，与遥测语义一致） |
| **是否污染 command uplink** | ✅ **否** | **不调用** `cloud_compress_uplink()`；**不构造** `{"cmd":"result"}`；**不经过** `cloud_send_up()`；信封**无** `c` / `i` / `s` / `r`（回归测试已断言，见 `P3-WP4-Firmware-Telemetry-Implementation.md` §6） |
| **是否影响 CloudManager 架构** | ✅ **纯新增** | `cloud_manager.h:73` 新增 1 个声明 + `.cpp:2558` 新增 1 个函数；**既有 4 个上行 API 零改动**（`cloud_manager.cpp.o` 目标文件 OFF/ON **逐字节相同**，见 §2.2） |
| **topic** | ✅ **零改动** | 复用 `mqtt_pub_topic` = `guo_feeder/<device_id>/up`（模板 `GF_TOPIC_TPL_UP`） |

**⚠️ 依赖既有 `mqtt_pub_topic` 的一个隐含耦合** → `F-WP4-FW-TR-5`：telemetry 与命令结果**共用同一 topic 变量** ⇒ 这正是 §4「`/up` 混流」的固件侧根因（`H-WP4-FW-01` 已裁决"EMQX Rule 层分流"，落地见 `H-WP4C-03`）。

### 1.5 阶段 A 结论

**实现质量 = 良好**（架构分层清晰、freestanding、无阻塞、无动态分配、transport 零污染）。**发现 5 项**：

| ID | 级别 | 摘 |
|---|---|---|
| `F-WP4-FW-TR-1` | 🟠 中 | `tp_health_abnormal()` 无调用点（未接线 capability） |
| `F-WP4-FW-TR-2` | 🔴 高 | 信封多发 `device_id`，与 `DT:WP-4` `ID-3`（下游忽略）重复 |
| `F-WP4-FW-TR-3` | 🔴 高 | 布尔类 `value` 编码**自相矛盾**（数字 1/0 vs 字符串） |
| `F-WP4-FW-TR-4` | 🟠 中 | 未接入 EventManager（实现 = 设计评审的子集） |
| `F-WP4-FW-TR-5` | 🟠 中 | telemetry 与命令结果**共用 `/up`**（固件侧混流根因） |

---

## §2 Flash / RAM 预算分析（Phase B）

### 2.1 总量对账（实测）

| 构型 | Flash (used) | RAM (used) | Δ vs **LOG 基线**（`81f6754`） |
|---|---|---|---|
| **LOG 基线**（`81f6754`） | 1,391,829 B | 132,704 B | — |
| **telemetry 门禁 OFF**（**默认**） | 1,391,877 B | 132,704 B | **+48 B** / +0 B |
| **telemetry 门禁 ON**（`-DGF_TELEMETRY_ENABLED=1`） | 1,394,373 B | 132,760 B | **+2,544 B** / **+56 B** |

预算（`P2-0` freeze 的 ceiling）：

```
ceiling（P3 合计增量上限）        = 14,336 B
已用：P2-7 12,548 + WP4-FW-LOG 1,152 = 13,700 B
⇒ telemetry 开始前剩余              =    636 B
```

| 门禁 | 增量 | 判定 |
|---|---|---|
| **OFF**（默认） | +48 B ≤ 636 B | ✅ `PASS`（余 588 B） |
| **ON**（启用） | **+2,496 B > 636 B** | 🔴 **超预算 1,860 B** ⇒ `RESOURCE BLOCKED` |

> 注：`+2,496 B` = 门禁 ON − 门禁 OFF 的**门禁增量**；`+2,544 B` = 相对 LOG 基线的**总增量**（含**未门禁**的 +48 B transport/接线）。两者差额 = 48 B。

### 2.2 逐项拆解（Phase B 核心）

**方法**：对 `-D` 变更导致的**全量重编**产物，用 `xtensa-esp32s3-elf-size` 逐 TU 比对（OFF = 主构建目录，ON = 独立目录 `telem_on`；两目录同源同工具链）。

**(a) 逐 TU 归因 —— 增量**全部**来自 1 个编译单元**

| 目标文件 | OFF (text/data/bss) | ON (text/data/bss) | Δtext | Δbss(RAM) |
|---|---|---|---|---|
| **`telemetry_publisher.cpp.o`** | 30 / 0 / 0 | **2,772 / 0 / 56** | **+2,742** | **+56** |
| `cloud_manager.cpp.o` | 44,652 / 16 / 51,121 | 44,652 / 16 / 51,121 | **0** | 0 |
| `main.cpp.o` | 32,664 / 36 / 348 | 32,664 / 36 / 348 | **0** | 0 |
| （其余 TU） | — | — | **0** | 0 |

⇒ **`main.cpp` 接线与 `cloud_manager` transport 在两种构型下逐字节相同**（证明它们**未被门禁**，其 +48 B 已含在默认构建里）。**RAM +56 B 全部来自 publisher 的 static 变量**（见 2.4）。

**(b) `telemetry_publisher.cpp.o` 段内分解（ON）**

| 段 | 字节 |
|---|---|
| `.literal.*` 合计 | **560** |
| `.text.*` 合计 | **1,845** |
| `.rodata.*` 合计 | **255** |
| **flash 相关段合计** | **2,660** |
| （OFF 同文件 text = 30） ⇒ **对象级增量** | **+2,630** |

链接器净额化（去重/对齐）后落到固件 = **+2,496 B**（差 ≈ 134 B）。

**(c) 归因到「功能来源」（按符号，对象字节 → 折算到实测 2,496 B）**

| 功能来源 | 组成（符号字节） | 对象小计 | **折算增量** | 占比 |
|---|---|---|---|---|
| **Envelope JSON builder（组包/序列化）** | `tele_emit` 652 + `.literal` 196 + `tele_emit_bool` 28 + `.literal` 12 | 888 | **≈ 805 B** | **≈ 32 %** |
| **JSON writer 原语**（envelope header 内联） | `te_ch` 36 + `te_str` 26 + `te_u32` 118 + `te_id_safe` 101 + `.literal` 24 | 305 | **≈ 289 B** | **≈ 12 %** |
| **Telemetry Policy（决策，全部内联）** | `telemetry_task` 682 + `.literal` 228 + `.rodata`（type 名）167 | 1,077 | **≈ 1,022 B** | **≈ 41 %** |
| **Publisher init（基线快照）** | `telemetry_publisher_init` 179 + `.literal` 96 | 275 | **≈ 261 B** | **≈ 10 %** |
| **诊断访问器** | `telemetry_get_stats` 16 + `telemetry_is_enabled` 7 + `.literal` 4 | 27 | **≈ 26 B** | **≈ 1 %** |
| **未归类（对齐/其他）** | — | — | **≈ 57 B** | ≈ 2 % |
| **合计** | | **2,572** | **≈ 2,496 B** | 100 % |

**(d) 未门禁部分（已含在默认构型）**

| 功能来源 | 增量 | 说明 |
|---|---|---|
| `cloud_send_telemetry()` transport + `main.cpp` 接线 | **+48 B** | **不随门禁开关变化**（OFF 已计入）；`cloud_manager.cpp.o` / `main.cpp.o` 逐字节相同 |

> **重要**：`test/telemetry_host/*` **不进入固件镜像** ⇒ **对 Flash 零贡献**（测试仅宿主机 wasm32 执行）。

### 2.3 优化候选（A–E）

> **前置约束**：候选 **A / D** 触及**协议**（须先经 `H-WP4-FW-05` 裁决）；**B / C** 为**纯实现优化**（不动协议）；**E** 为**范围裁剪**（动 capability）。
> ⚠️ **禁止**：直接改协议、直接删功能、改冻结设计。

| 候选 | 内容 | 预估节省（Flash） | 代价 / 前置 |
|---|---|---|---|
| **A. 减少字段** | 删 `device_id`（`F-WP4-FW-TR-2`）；评估删 `schema_version`（§3.4） | **≈ −60 … −120 B**（+ 每条载荷 −11 … −29 B） | 需 `H-WP4-FW-05` 裁决；`device_id` 删除**契约安全**（下游忽略） |
| **B. 减少代码路径** | ① 5 个边沿块 → **表驱动**派发 ② 删未用访问器（−26 B）③ 定点打印与既有工具共址 | **≈ −350 … −450 B** | 纯实现；须保 41 项断言全绿 |
| **C. 共享已有 JSON 工具** | 用**已链接**的 ArduinoJson（`cloud_upload_json` 已用）替换手写原语 + 组包 | **≈ −700 … −900 B** | ⚠️ **失去 freestanding**（无法 wasm32 主机真跑）+ 重新引入动态分配风险 |
| **D. 二进制定长 payload** | 复用**已链接**的 `log_cbor.h` 编码器（日志路径已用）+ Base64 | **≈ −1,100 B** | ⚠️ **改线协议**（JSON → 二进制）；可读性/可观测性下降；须冻结 |
| **E. 延后部分 capability** | 首版只发**取值类**（temp/humid/weight），暂缓 5 个边沿/状态类 | **≈ −600 B**（`telemetry_task` 682 + rodata 167 + 部分 literal） | 覆盖度下降（`F-WP4-FW-TR-4`）；须登记范围变更 |

### 2.4 RAM 与运行时

| 项 | 值 | 说明 |
|---|---|---|
| **RAM 增量（门禁 ON）** | **+56 B** | 全部为 **static 工作集**（`.bss`），**无堆**：`s_temp/humid/weight` 12 + `*_ms` 12 + 5×bool 5 + `s_online` 4 + `s_stats` 16 + `s_last_edge_ms` 4 + `s_seq` 4 + `s_ready` 1 ≈ 58 B ⇒ 对齐 56 B |
| **栈占用** | **≤ 320 B**（瞬态） | `tele_emit()` 内 `char buf[TELEM_ENV_MAX_BYTES=320]`；loopTask 16 KB ⇒ 安全 |
| **堆分配** | **稳态 0** | 仅 `cloud_send_telemetry` 兜底分支临时 `String`（首帧路径） |
| **主循环负担** | **可忽略** | 见 §1.3：离线 1 次 bool；在线 ~8 次读 + 常数比较；每轮 ≤1 条报文 |

### 2.5 ⚠️ 阶段 B 关键结论

> **在现有 ceiling（余 636 B）下，telemetry 不可能靠"纯代码优化"装入。**
>
> - 可行的**纯实现优化上限**（B + C 同时用尽，且接受失去 freestanding）≈ **−1,100 … −1,400 B**；
> - 仍 **> 636 B** ⇒ **单靠优化无法解除 `RESOURCE BLOCKED`**。
>
> ⇒ **唯一可行路径 = ① 人工重裁 ceiling，或 ② 削减范围（E）/ 改协议（A/D），或 ③ 延期。**

---

## §3 协议冻结建议（Phase C）

> 本节全部为 **`PROPOSAL`**。`H-WP4-FW-05` / `07` 裁决前，**不得**当作云端契约。

### 3.1 `type` taxonomy（`H-WP4-FW-05` 之一）

**要求**：稳定 ID · 可扩展 · **不绑定显示名称**。

**方案对比**：

| 方案 | 形如 | 稳定 ID | 可扩展 | 不绑显示名 | 评价 |
|---|---|---|---|---|---|
| **T1 点分命名空间（字符串）** | `climate.temperature`，`weight.value`，`online.state` | ✅（标识符本身） | ✅ 追加新 ID | ✅（"温度"是显示名，ID 不是） | **★ 推荐**：自描述、可读、无需中央数字注册表 |
| T2 扁平短码（字符串） | `temp`，`wt`，`onl` | ✅ | ⚠️ 易冲突 | ✅ | 省字节，但命名冲突风险高 |
| T3 数字枚举 ID | `type: 7` | ✅ | ⚠️ 需中央注册表 | ✅ | 最省字节，但**低可读性**、**需冻结数字表**、与"可扩展"冲突（表扩散） |

**推荐 = T1（点分命名空间字符串）**，约束如下：

1. **形状冻结**：`<domain>.<metric>`，`domain ∈ {climate, weight, valve, online, wifi, time, health, …}`；`[a-z0-9_]`；总长 ≤ 32（与 `te_id_safe()` 上界一致）。
2. **追加式演进**：新 `type` **只能新增**，**永不改名、永不删除**（仅可标记 `deprecated`）。
   - ⇒ 因为**幂等键含 `type`**（`ux_th_dedup`），**改名 = 历史行身份断裂**。
3. **显示名在云端**：i18n / 展示名属云侧资源，**设备侧只发 ID**。
4. **与既有实现对齐**：当前草案（`climate.temperature` / `climate.humidity` / `weight.value` / `weight.error` / `valve.state` / `wifi.state` / `online.state` / `time.valid`）**已符合 T1**，可直接升格为 v1 冻结表（**无需改码**）。

**建议的 v1 封闭表（草案）**：

| `type` | 类别 | `value` 种类 | 单位/取值域 |
|---|---|---|---|
| `climate.temperature` | 取值 | number | °C（2 位小数） |
| `climate.humidity` | 取值 | number | %RH（2 位小数） |
| `weight.value` | 取值 | number | g |
| `weight.error` | 边沿 | **bool/string**（见 3.2） | true/false |
| `valve.state` | 边沿 | **bool/string** | true/false（开/关） |
| `wifi.state` | 边沿 | **bool/string** | true/false |
| `online.state` | 边沿 | **string** | `init` / `ready` / `disconnected`（`lost` 由云端 presence 产生） |
| `time.valid` | 边沿 | **bool/string** | true/false |
| `health.heap`（预留） | 健康 | number | 字节（`tp_health_abnormal` 接线后启用） |

### 3.2 `value` 统一编码规则（`H-WP4-FW-05` 之二）

**问题（`F-WP4-FW-TR-3`）**：当前实现同类语义两种编码 —— 布尔走数字 `1.0/0.0`，状态走字符串。必须**统一**。

**三选一**：

| 规则 | 描述 | 优点 | 缺点 |
|---|---|---|---|
| **V1 二态：number \| string**（**★ 推荐**） | 物理量 = JSON **number**（固定小数位）；**二态/状态一律 JSON string**（`"true"`/`"false"` 或枚举词） | ① 与 DDL `value TEXT` 映射简单 ② 云侧只需 2 个解析分支 ③ **无 JSON bool** 语义歧义 | 布尔以字符串表达，稍冗 |
| V2 三态：number \| string \| bool | 纯布尔用 JSON `true`/`false` | 语义最"正确" | 落 `TEXT` 列时需额外规范化；云侧 3 分支 |
| V3 全字符串 | 一切皆字符串 | 最简；落库零转换 | 数值查询/绘图需云侧再解析；`ix` 意义下降 |

**推荐 = V1**：
- **物理量** → JSON **number**（`climate.*` / `weight.value` / `health.*`），小数位**在 taxonomy 表内冻结**（当前实现 2 位、去尾零，`te_fixed2`）；
- **二态 / 枚举** → JSON **string**（`"true"`/`"false"`，或状态词表）；**单位不进载荷**（单位绑定在 `type` 上，随 taxonomy 冻结）。
- ⇒ 落实方式：`tele_emit_bool()` 改为输出 `"true"`/`"false"`（**1 处小改**，`publisher.cpp:112-115`）。

### 3.3 `schema_version` 去留（`H-WP4-FW-05` 之三）

**推荐 = 保留（KEEP）**，理由：

1. **DDL 已有该列**（`telemetry_history.schema_version INTEGER NOT NULL DEFAULT 1`），且 `DT:WP-4` 冻结"`>1` ⇒ 接收 + 校验格式 + 原样保存"（前向兼容锚点）；
2. `WP2:§3.3` 明确把 `value` 的**逻辑类型解释权**交给 `schema_version` ⇒ **它是 `value` 解码的唯一钥匙**；
3. 成本极小（≈ 25 B 代码 + 每条 ~18 B 载荷）；
4. 删掉它 ⇒ v1 的 `value` 语义将**无版本可依**，未来加 `value` 变体只能靠 `type` 改名（**违反 3.1 的"永不改名"**）。

> 若 §2 需要极限压 Flash，可将 `schema_version` 降为**编译期常量省略**（由 DDL `DEFAULT 1` 兜底）—— 但**仅 v1 期**可行，且须在 taxonomy 冻结文档中显式声明"v1 期省略字段、语义等价 `schema_version = 1`"。**列为 `H-WP4-FW-05` 的子裁决项。**

### 3.4 `seq` 生命周期（`H-WP4-FW-07`）

**决定性事实**：云端幂等键 = **`(device_id, type, ts, seq)`**（`ux_th_dedup`，`0003:117-118`）—— **键中无 `boot_seq`**（与 log 的 `(device_id, boot_seq, seq)` **不同**）。

| 方案 | 描述 | 幂等安全性 | 成本 | 评价 |
|---|---|---|---|---|
| **A. boot 内递增** | 每次开机 `s_seq = 0`（**= 现状**，`publisher.cpp:122`） | 🔴 **不安全**：`ts=0`（未同步）时，跨 boot 的 `(dev,type,0,seq)` **必碰撞** ⇒ 后到者被 `INSERT OR IGNORE` **静默吞掉**（`R-WP4-FW-7`） | 0 | ❌ 仅在"`ts` 恒有效"时才勉强可用 |
| **B. NVS 持久化单调计数** | 单个 u32，NVS 持久；**节流落盘**（如每 N 条或每启动写一次） | ✅ **安全**：`seq` 全局单调 ⇒ `(dev,type,ts,seq)` 恒唯一，`ts=0` 也安全 | 中（NVS 读 1 次 + 低频写，注意磨损） | **★ 推荐** |
| C. 复用 log seq | 借用 LogManager 的 `boot_seq`+`seq` | ⚠️ 亦不安全：log `seq` **同样每次开机归零**，且需额外把 `boot_seq` 塞进键/信封（**改 DDL + 改信封**） | 高（跨模块耦合） | ❌ 耦合 + 仍不唯一 |

**推荐 = 方案 B（NVS 持久化单调 `seq`）** + 两个配套规则：

1. **`ts` 无效（未同步）时的行为** —— 备选：
   - **B-1（推荐）**：**取值类不发**（表是时间序列，`ix_th_device_ts` 是查询路径，`ts=0` 无查询价值）；**边沿类允许发**（事件价值独立于绝对时间），靠持久化 `seq` 保幂等唯一。
   - **B-2**：一律上报 `ts=0`（靠 B 的 `seq` 保唯一）—— 数据不丢，但时间轴不可用。
2. **落盘策略**：`seq` 在**启动时读一次**、**满足任一条件时写回**（① 累计 ≥ N 条 ② 启动后首次发报前 ③ 周期性）。避免"每发一条写一次 NVS"的磨损。

> ⚠️ 若最终采纳 **B**，实现需 **+1 个 NVS 键**且**触及 `settings`/`nvs` 层** —— 属**新增依赖**，须在 `H-WP4-FW-07` 裁决后另立项（**本轮不实现**）。

---

## §4 `/up` 分流分析（Phase D）

### 4.1 现状：`guo_feeder/<device_id>/up` 上并存 3 类上行

| 类 | 形态 | 产生方 | 证据 |
|---|---|---|---|
| **命令结果** | `{"c":"result","i":…,"s":…}`（含 `registry` 特化分支） | `CommandManager` → `on_command_result` → `cloud_send_up()` | `cloud_protocol.md:264/413/433` |
| **registry 结果** | `{"c":"result",…,"registry":…,"v":…}` | 同上（特化分支） | `cloud_protocol.md:433-441` |
| **telemetry**（新） | `{"device_id":…,"ts":…,"type":…,"value":…,"seq":…}` | `telemetry_publisher` → `cloud_send_telemetry()` | 本文 §1.2 |

**冲突机理**：`DT:WP-4` 的 ingest 对"缺 `ts`/`type`/`value`/`seq`"的记录**逐条 `rejected`**（`F-WP4-FW-2`）⇒ 若三类共用**同一端点**，**命令结果会被逐条拒绝**，**云端命令结果可观测性丧失**（P1-6 `credential_set` 帧等）。

### 4.2 三方案对比

| 方案 | 描述 | 是否需改 topic | Worker 是否需改 | 与 `H-WP4-FW-01`（已 `CLOSED`）一致 | 评价 |
|---|---|---|---|---|---|
| **A. EMQX Rule 分流** | EMQX Rule 层按 **topic / message metadata（含 payload 谓词）** 分流到不同下游 | **否** | **否** | ✅ **一致**（冻结裁决即选此） | **★ 推荐** |
| B. 新增 topic | 例如 telemetry 走独立 topic | **是** ⛔ | 否 | ⚠️ 需**冻结修正** | 域隔离最干净，但**违反本轮"禁改 topic"** |
| C. 保持混流 | 不动，共用一端点 | 否 | 可能需降噪 | ❌ | 命令结果被拒 ⇒ **不可接受** |

**约束回顾**：`H-WP4-FW-01` 冻结 —— "**topic/domain separation 优先于 payload inspection** · **Worker 不负责 command/telemetry 判断** · **Worker 不解析 payload 分流** · EMQX Rule 层分流 · 不新增 Worker policy · 不改 ingestion contract"。

### 4.3 推荐 = A，但其「判别子」仍开放（`H-WP4C-03 = OPEN`）

**关键张力**：命令结果与 telemetry **共用 `/up`**，因此 **EMQX Rule 仅凭 topic 无法区分**二者。可选判别子：

| 子方案 | 判别依据 | 是否改 topic | 是否触碰"payload inspection"禁令 | 备注 |
|---|---|---|---|---|
| **A1. Rule 层 payload 谓词**（**★ 推荐**） | EMQX Rule SQL 对 payload 做 **JSON 谓词**（含 `type`+`seq`+`ts` ⇒ 判为 telemetry；否则 → 命令端点） | **否** ✅ | ⚠️ 该禁令针对 **Worker**（`H-WP4-FW-01` 原文"**Worker** 不解析 payload"）；**EMQX Rule 层**做谓词**不违反**该禁令 —— 但与"分离优先于 payload inspection"的**偏好**存在张力 | **零 topic 改动**、**零 Worker 改动** ⇒ 唯一在现行冻结下可落地者 |
| A2. topic 后缀拆分（`/up/telemetry`） | topic 前缀 | **是** ⛔ | 否 | 需**冻结修正**（入口族新增）+ ACL 覆盖复核 |
| A3. 独立 telemetry topic（= 方案 B） | topic | **是** ⛔ | 否 | 同上，改动面最大 |

**结论（建议交 `H-WP4C-03` 裁决）**：
- **在"禁改 topic + `DT:WP-4` 冻结"的前提下，唯一可落地的是 A1**（EMQX Rule 层 JSON 谓词分流）；
- 若人工认为 "payload inspection" 亦不可接受 ⇒ 必须升级为 **A2/A3（改冻结）**，届时须同时补 **ACL 5/5 复核**（P2 ACL 现覆盖 sub `/down` · pub `/up` · pub `/log` · homie 2 条；新增 topic 需配套）；
- **方案 C（保持混流）不可接受**（命令结果被拒）。
- ⚠️ **本轮不改任何 topic、不改 EMQX**（仅分析）。

---

## §5 `HUMAN_REQUIRED` 清单

| ID | 主题 | 当前状态 | 本文档提供的输入 |
|---|---|---|---|
| **`H-WP4-FW-05`** | `type` 词表 + `value` 编码 + `schema_version` 初值 | **`OPEN`** | §3.1（推荐 T1 点分命名空间 + 追加式演进）· §3.2（推荐 V1 = number\|string）· §3.3（推荐**保留** `schema_version`） |
| **`H-WP4-FW-06`** | `mqtt.json.telemetry` 字段集与默认值 | **`OPEN`** | 本文档**未新增配置键**；建议：字段集随 taxonomy 冻结一并裁决；默认值须规避 `R-WP4-FW-6`（缺键=静默失效） |
| **`H-WP4-FW-07`** | telemetry `seq` 语义（跨 boot 持久化？`ts` 无效时上报 0 或不发） | **`OPEN`** | §3.4（推荐 **B = NVS 持久化单调 `seq`**；`ts` 无效时取值类不发、边沿类可发） |
| **`H-WP4-FW-08`** | Flash 预算处置 | `CLOSED WITH GUARD` ⇒ 已触发 🔴 **`RESOURCE BLOCKED`** | §2：**纯优化无法装入 636 B**；须 ① 重裁 ceiling 或 ② 削减范围（E）/改协议（A/D）或 ③ 延期 |
| **`H-WP4-FW-09`** | `/up` telemetry 载荷形态 + 消费者清单（文档同步） | **`OPEN`** | §4：`cloud_protocol.md §4` 上行契约**尚未记录 telemetry 形态** ⇒ 待 `H-WP4-FW-05` 冻结后同步 |
| **`H-WP4C-03`** | EMQX Rule 分流机制（rule scope / 判别子） | **`OPEN`** | §4.3：推荐 **A1（Rule 层 JSON 谓词，零 topic 改动）**；A2/A3 需冻结修正 |
| **`H-WP4C-04`** | 远端 `0003` migration 授权 | **`OPEN`** | 与 telemetry 无关（CF 侧），保持 |
| **`H-WP4C-05`** | EMQX Action 切换授权 | **`OPEN`** | 「完成」遥测端到端的前置 |
| **`H-WP4C-06`** | 部署顺序 | **`OPEN`** | 同上 |

**本文档新登记（评审发现）**：

| ID | 级别 | 内容 |
|---|---|---|
| `F-WP4-FW-TR-1` | 🟠 | `tp_health_abnormal()` 未接线（无调用点） |
| `F-WP4-FW-TR-2` | 🔴 | 信封多发 `device_id`，与 `DT:WP-4` `ID-3` 重复（下游忽略） |
| `F-WP4-FW-TR-3` | 🔴 | 布尔类 `value` 编码自相矛盾（数字 1/0 vs 字符串） |
| `F-WP4-FW-TR-4` | 🟠 | 未接入 EventManager（实现 = 设计评审子集） |
| `F-WP4-FW-TR-5` | 🟠 | telemetry 与命令结果共用 `/up`（混流固件侧根因） |

---

## §6 推荐实施路径（Recommended Implementation Path）

> **总原则**：**先冻结协议、再裁决预算、最后（可选）做实现优化。** 三者**互不阻塞**，但**启用**须三者齐备。

```
┌─ GATE 1：协议冻结（H-WP4-FW-05 / 06 / 07）────────────────────────┐
│  ① type taxonomy     → 采纳 T1（点分命名空间）+ 追加式演进        │
│  ② value 统一编码    → 采纳 V1（number | string），修 bool 不一致  │
│  ③ schema_version    → 保留（或以"v1 省略+DDL DEFAULT"替代）      │
│  ④ seq 生命周期      → 采纳 B（NVS 持久化单调）+ ts 无效策略      │
│  ⑤ device_id 去留    → 建议删除（F-WP4-FW-TR-2，与 ID-3 对齐）     │
└──────────────────────────────────────────────────────────────────┘
                     │（冻结后 → 才可写"契约级"代码与文档）
                     ▼
┌─ GATE 2：预算裁决（H-WP4-FW-08）─────────────────────────────────┐
│  实测：启用增量 +2,496 B；剩余 636 B ⇒ 超 1,860 B                 │
│  可选：① 重裁 ceiling（推荐先评估）② 削减范围 E ③ 改协议 D       │
│  ⚠️ 纯代码优化（B+C）上限 ≈ −1,400 B ⇒ 仍不够 ⇒ 必须动其一        │
└──────────────────────────────────────────────────────────────────┘
                     │（预算放行后 → 才可启用门禁）
                     ▼
┌─ GATE 3：分流落地（H-WP4C-03）───────────────────────────────────┐
│  采纳 A1：EMQX Rule 层 JSON 谓词分流（零 topic / 零 Worker 改动） │
│  前提：H-WP4-FW-05 冻结（Rule 谓词依赖 type/ts/seq 字段名）       │
└──────────────────────────────────────────────────────────────────┘
                     ▼
              （可选）实现优化：B（表驱动+删未用访问器）优先，
              其次 C（换 ArduinoJson，须弃 freestanding），再次 A（删字段）
```

**分步建议（按依赖序）**：

| 步 | 动作 | 依赖 | 产出 |
|---|---|---|---|
| **S1** | 人工裁决 `H-WP4-FW-05` / `06` / `07`（采纳本文 §3 推荐或修正） | — | taxonomy / value 规则 / seq 策略**冻结** |
| **S2** | 依 S1 同步 `cloud_protocol.md §4`（telemetry 上行形态）+ 冻结文档 | S1 | `H-WP4-FW-09` 收口 |
| **S3** | 裁决 `H-WP4-FW-08`：重裁 ceiling **或** 定范围（E）/协议（D） | S1（范围依赖字段） | 预算路径确定 |
| **S4** | 依 S3 选定路径实施：① 若"重裁 ceiling" ⇒ 仅需按 S1 微调（如 `tele_bool` 改字符串、删 `device_id`）；② 若"削减/改协议" ⇒ 按新范围重做 | S1+S3 | 代码就绪 |
| **S5** | 裁决 `H-WP4C-03` ⇒ 落地 EMQX Rule（A1/A2/A3） | S1 | 分流生效 |
| **S6** | 端到端联调（`Phase C` `V-3`） | S1–S5 + `H-WP4C-04/05/06` | `CF-2` → `RESOLVED` |

**当前可立即执行（无需新裁决）的最小动作**（若人工即时授权）：
- 修 `F-WP4-FW-TR-3`（`tele_emit_bool` → 字符串）——**1 行级**，但属"改语义"，须待 `H-WP4-FW-05` 定 V1；
- 删 `F-WP4-FW-TR-2` 的 `device_id` —— 需 `H-WP4-FW-05` 确认。

> **本轮不执行以上任何动作**（任务限定：**仅设计评审与优化方案分析，禁止编码**）。

---

## §7 边界核对（**零违规**）

| 禁止项 | 状态 |
|---|---|
| 修改 `src/` | ✅ **未改** |
| 修改 Cloud（Worker） | ✅ **未改** |
| 修改 EMQX | ✅ **未改** |
| 修改 migration | ✅ **未改** |
| 修改冻结设计文档 | ✅ **未改** |
| 启用 `GF_TELEMETRY_ENABLED` | ✅ **未改**（保持默认 0） |
| 改 `DT:WP-4` / topic | ✅ **未改** |
| 新增文档 | ✅ **唯一** = `docs/review/P3-WP4-Firmware-Telemetry-Design-Freeze-Review.md` |

---

*本文档于 **2026-10-12** 建立，为 **`WP4-FW-TELEMETRY` 的设计冻结评审**（`Design Freeze Review`）；
只做**分析、影响评估、优化候选与冻结建议**，**不含任何代码改动、配置变更或实现**。
基线 = 固件仓 HEAD **`baeb177`**（工作树 clean）· CF 仓 **`9b25203`** · EMQX 仓 **`eec9444`**。
所有 `file:line` 与 Flash/RAM 数字均取自基线实测，可逐条回查。*
