# P3 · `Phase C-C2` —— **`/log` 分流控制台施工任务单（方案 B · 照抄版）**

> ## 🟡 `PLAN ONLY` —— 本文件**只是实施方案**，**不含任何生产变更**
>
> - **不做**：不改 EMQX（本文仅为「如何改」的规格）· 不改固件 · 不改 Worker · 不建 / 改 migration · 不部署 · 不 push。
> - **待人工审核** → 裁决 **`HR-C2-*`**（见 §7）→ 人工按本文控制台施工 → 回写存档 → 执行 **`V-3.1…V-3.6`** 验证。
> - **裁决依据**：`HR-C2-1 = 方案 B（改规则 A+B · 配额中立）`（用户 `2026-10-15` 已选）。
> - **本文档定位**：把 C2 审查（`docs/review/Phase-C-C2-Log-History-End-to-End-Review.md`）的**方案 B** 落实为**可照抄执行**的控制台规格。

| 项 | 值 |
|---|---|
| **状态** | **`Phase C-C2 = PLAN READY / AWAITING HUMAN REVIEW`** |
| **前置审查** | `docs/review/Phase-C-C2-Log-History-End-to-End-Review.md`（判定 `CONDITIONAL GO / BLOCKED-1`） |
| **变更承载** | 固件仓（本文档 · docs-only）· 施工对象在 EMQX 控制台（人工）· 归档落点 `EMQX_Assets/config/data-integration/` |
| **变更对象** | **恰好 2 条规则**（A `r-n302933b-cf0cf2` · B `r-n302933b-f731cf`）· **连接器 / 动作 / 端点 / presence 全不动** |
| **配额影响** | **4/4 不变**（原地编辑，不新增对象） |
| **生产变更** | **零**（本文件） |
| **门禁** | `G-1 HUMAN_REQUIRED` · `G-4 BLOCKED` **均未变** |

---

## §0 一句话总览

> **只改 2 条规则的 `SQL`**（都不是删重建 —— **规则可原地编辑**）：
> **规则 A**：`FROM` 加一个 topic（`…+/log`）· `WHERE` 由「telemetry 谓词」变「**telemetry 谓词 `OR` log 谓词**」。
> **规则 B**：`WHERE` 由「telemetry 之否定」变「**（telemetry 之否定）`AND`（log 之否定）**」—— 即**排除 `/log`**。
> **连接器 `CB` · 动作 `HI`/`MF` · 端点 / 请求头 / 请求体**：**逐字不动**（log 信封复用 `HI` → `/api/history/ingest`，Worker 已按 topic 域自动落 `log_history`）。
> **预计 6–10 分钟**；**顺序：规则 A 先 → 验证 → 规则 B 后**（§3）。

| 项 | 值 |
|---|---|
| **执行人** | 你（EMQX 控制台人工）。代理**无**控制台 / 数据集成 API 权限（API 对 rules / actions / connectors 全 `403` · 无 CLI） |
| **依据** | 方案 B（`HR-C2-1` 裁决）· `R3.1` SQL 写法（`is_null` / `is_not_null`）· 现网 SQL 原文（`P3-WP4C-V2V4-Config-After.md` §2.4/§2.5） |
| **不要忘** | ① SQL 用 **`is_null` / `is_not_null`**（**禁 `==`**）② **保存前用页面「调试」试跑**（§4.0）③ **规则 A / B 必须同批完成**（防双投，§3） |
| **禁止** | ❌ 不新增规则 / 动作 / 连接器（配额满）❌ 不改连接器 / 动作 / presence ❌ 不改 firmware / Worker / D1 ❌ 密钥值不写进任何文件 ❌ 不猜 SQL（一律照抄本文） |

> **流水账（打勾用）**：
> `[ ] 0 抄变更前 SQL` → `[ ] 1 改规则 A 的 FROM` → `[ ] 2 改规则 A 的 WHERE` → `[ ] 3 保存 + 调试试跑 + 观察 /log 落 log_history`
> → `[ ] 4 改规则 B 的 WHERE` → `[ ] 5 保存 + 观察 mqtt_messages 不再收 /log` → `[ ] 6 跑 V-3.1…V-3.6` → `[ ] 7 回写存档`

---

## §1 【存档】变更前 SQL 原文（**回滚快照 · 逐字 · 先抄下来**）

> 来源：`EMQX_Assets/config/data-integration/P3-WP4C-V2V4-Config-After.md` §2.4/§2.5（**权威现网原文**）。
> ⚠️ **变更前必须再次从控制台逐字复核**（若与下列不符 ⇒ **STOP**，以控制台为准并回写）。

### 1.1 规则 A `r-n302933b-cf0cf2`（现网 · 变更前）

```sql
SELECT
  *
FROM
  "guo_feeder/+/up"
WHERE
      is_not_null(payload.schema_version)
  AND is_not_null(payload.seq)
  AND is_not_null(payload.ts)
  AND is_not_null(payload.type)
  AND is_not_null(payload.value)
  AND is_null(payload.c)
  AND is_null(payload.cmd)
```

### 1.2 规则 B `r-n302933b-f731cf`（现网 · 变更前）

```sql
SELECT
  *
FROM
  "guo_feeder/#"
WHERE
      is_null(payload.schema_version)
   OR is_null(payload.seq)
   OR is_null(payload.ts)
   OR is_null(payload.type)
   OR is_null(payload.value)
   OR is_not_null(payload.c)
   OR is_not_null(payload.cmd)
```

> **回滚 = 把这两段逐字贴回**（§5）。**变更前请把控制台里这两条规则的「运行统计」也抄下来**（触发 / 成功 / 失败），作为变更后对照基线。

---

## §2 变更后 SQL（**照抄**）

### 2.1 规则 A `r-n302933b-cf0cf2`（**FROM 加 `/log` · WHERE 加 log 分支**）

```sql
SELECT
  *
FROM
  "guo_feeder/+/up", "guo_feeder/+/log"
WHERE
  (
        is_not_null(payload.schema_version)
    AND is_not_null(payload.seq)
    AND is_not_null(payload.ts)
    AND is_not_null(payload.type)
    AND is_not_null(payload.value)
    AND is_null(payload.c)
    AND is_null(payload.cmd)
  )
  OR
  (
        is_not_null(payload.payload_base64)
    AND is_not_null(payload.boot_seq)
    AND is_not_null(payload.seq)
    AND is_not_null(payload.ts)
    AND is_not_null(payload.level)
  )
```

> ★ **与原谓词的关系**：第一组括号 = **冻结 telemetry 谓词，逐字不变**；第二组括号 = **新增 `log_predicate`**（§2.3）。**用 `OR` 合并** ⇒ telemetry 行为零变化，新增 `/log` 命中。

### 2.2 规则 B `r-n302933b-f731cf`（**WHERE 扩为「排除 log」**）

```sql
SELECT
  *
FROM
  "guo_feeder/#"
WHERE
  (
        is_null(payload.schema_version)
     OR is_null(payload.seq)
     OR is_null(payload.ts)
     OR is_null(payload.type)
     OR is_null(payload.value)
     OR is_not_null(payload.c)
     OR is_not_null(payload.cmd)
  )
  AND
  (
        is_null(payload.payload_base64)
     OR is_null(payload.boot_seq)
     OR is_null(payload.seq)
     OR is_null(payload.ts)
     OR is_null(payload.level)
  )
```

> ★ **为什么要改 B**：`/log` 的信封**无 `schema_version`** ⇒ 变更前的 `NOT telemetry` 对 `/log` **恒为真** ⇒ B 命中 ⇒ `/log` 被送到 `/api/webhook`（落错表 `mqtt_messages`）。**补上「AND（log 之否定）」后，`/log` 不再被 B 命中**。
> ★ 写法 = 「NOT(telemetry) **AND** NOT(log)」的 **德摩根展开**（`NOT (X OR Y) = (NOT X) AND (NOT Y)`）——**无 `NOT` 关键字**；括号为分组，EMQX 规则 SQL 支持（presence 规则同为带括号形态）。

### 2.3 ★ 新增 `log_predicate` 定义（**提案 · 待 `HR-C2-5` 批准**）

**判据（5 键「存在」语义）** —— 取自 log 信封契约（`src/log/log_envelope.h` L22–24）：

```
{ "device_id", "boot_seq", "seq", "ts", "level", "payload_base64" }
```

`log_predicate` = `payload_base64 ∧ boot_seq ∧ seq ∧ ts ∧ level` **均存在**（∀ `is_not_null`）。
（`device_id` **不进**谓词 —— 与冻结铁律一致：**身份只来自 topic**。）

**互斥性论证（关键 · 决定不会双投）**：

| 报文类型 | topic | `schema_version`/`type`/`value` | `payload_base64` | `telemetry` | `log` | A 命中 | B 命中 |
|---|---|---|---|---|---|---|---|
| telemetry 信封 | `…/up` | ✅ 有 | ❌ 无 | ✅ | ❌ | ✅ | ❌ |
| **log 信封** | `…/log` | ❌ 无 | ✅ 有 | ❌ | ✅ | ✅（新） | ❌（排除后） |
| 命令 ack/result | `…/up` | ❌ | ❌ | ❌（有 `c`） | ❌ | ❌ | ✅ |
| `online` 帧 | `…/up` | ❌ | ❌ | ❌（有 `cmd`） | ❌ | ❌ | ✅ |
| `/down` 下行 | `…/down` | ❌ | ❌ | ❌ | ❌ | ❌（不在 FROM） | ✅ |

> ⇒ **telemetry 与 log 两组谓词永不双真**（字段集不相交）；**改后 A、B 仍严格互斥** ⇒ **无双投**。

---

## §3 施工顺序（**规则 A 先 → 验证 → 规则 B 后**）

| 步 | 动作 | 瞬时窗口影响 |
|---|---|---|
| **1** | 改 **规则 A**（§2.1）保存并启用 | **严格增量**：`/log` 开始落 `log_history`（正确路径开通）；**与此同时 B 未改 ⇒ `/log` **仍**落 `mqtt_messages`（= 今日既有行为，**无回归**） |
| **2** | 观察 `/log` 已落 `log_history`（§4 步骤 3） | — |
| **3** | 改 **规则 B**（§2.2）保存并启用 | `/log` 不再被 B 命中 ⇒ **`mqtt_messages` 停止收 `/log`**（隔离达成） |

> ★ **为何 A 先**：先开通正确路径 ⇒ 过渡窗口内 `/log` **至少落对一处**，且**与今日行为零差异**（今日 `/log` 本就进 `mqtt_messages`）；最后改 B 才真正「堵漏」。
> ⚠️ **反序（B 先）不可取**：会让 `/log` 在窗口内**两头皆落空**（丢失）。
> ⚠️ **两条规则必须同批完成**（同一施工）。若只改 A 不改 B ⇒ **双投**（`/log` 同时进 `log_history` + `mqtt_messages`）⇒ 触发 §6 STOP（须尽快补 B）。

---

## §4 验证（`V-3.1` … `V-3.6` · 施工后执行）

> **观测纪律**：`/log` **仅在设备产生日志时**上行（非周期）⇒ 需**构造触发**（下发一条会产生日志的命令，或等自然日志）；**不得以「窗口内无日志」判定 FAIL**。

### §4.0 保存前 · 页面「调试」试跑（**必做**）

| 试跑输入（body） | 规则 A 期望 | 规则 B 期望 |
|---|---|---|
| telemetry 外形：`{"schema_version":1,"seq":1,"ts":1791966947,"type":"climate.temperature","value":24.4}` | ✅ **命中** | ❌ 不命中 |
| log 外形：`{"device_id":"288485896ce4","boot_seq":95,"seq":108545,"ts":1791968500,"level":1,"payload_base64":"AA=="}` | ✅ **命中** | ❌ 不命中 |
| 命令外形：`{"c":"result","i":"x","o":"config_set","m":"ok"}` | ❌ 不命中 | ✅ **命中** |

> `payload.x` 是否需 `json_decode` —— **现网 A/B 已用 `payload.<field>` 直取且 `V-1…V-7` 全 PASS** ⇒ 维持直取；试跑若不符 ⇒ **STOP** 并回写。

### §4.1 判据矩阵

| # | 步骤 | 判据（`PASS` 定义） | 证据 |
|---|---|---|---|
| `V-3.1` | 设备发出 ≥1 条 `/log`（JSON 信封） | MQTT `guo_feeder/<device_id>/log` 出现 `{device_id,boot_seq,seq,ts,level,payload_base64}` | MQTT 订阅抓包 |
| `V-3.2` | 观测 Worker | `wrangler tail` 命中 **`/api/history/ingest`**（**非** `/api/webhook`）· `200 {"accepted":1,…}` | tail（走代理 `127.0.0.1:7890`） |
| `V-3.3` | D1 落库 | `log_history` **+1 行**；`payload` **原样**（Worker 不解码）；幂等键 `(device_id,boot_seq,seq)` 唯一；`created_at` 由 D1 生成 | D1 查询 |
| `V-3.4` | 隔离 | `mqtt_messages` **不因 `/log` 增长** | D1 计数对照（改 B 后） |
| `V-3.5` | 鉴权不变 | 无 `X-Webhook-Key` ⇒ `401`（不放宽） | 探针 |
| `V-3.6` | 回归 | `/up` telemetry 仍落 `telemetry_history` · presence 仍 `200` · `/down` 命令回包仍落 `mqtt_messages` | 复测 |

### §4.2 D1 计数对照（施工前 → 施工后）

| 表 | 施工前（基线 · 现场复核） | 期望（施工后） |
|---|---|---|
| `log_history` | `0`（**将结束**） | **`+≥1`** |
| `telemetry_history` | `15`（`2026-10-14` `V-2` 后） | **不变 / 仅随真实 telemetry 增** |
| `mqtt_messages` | 现场复核（`2026-10-14` 终态 `3162`） | **`/log` 贡献归零**（`/up` 命令 + `/down` 仍增） |

---

## §5 回滚（**最低成本 · 原地改回**）

| 层 | 回退手段 | 代价 |
|---|---|---|
| 规则 A / B | **把 §1 的两段 SQL 逐字贴回**（规则可原地编辑） | 最低（无需重建） |
| 连接器 / 动作 | **未变更** ⇒ 无需回退 | — |
| D1 | **无 schema 变更** ⇒ 无需回退（`log_history` 表已在）；如需清测试行属定点清理，需人工授权 | — |
| 固件 | **本轮无固件改动** ⇒ 无需回退 | — |

**回滚触发**：§4.1 任一 `PASS` 判据失败且确认为本轮引入；或出现 telemetry/Presence 回归；或 `resource down` 告警持续。

---

## §6 STOP 条件（**任一命中即停**）

1. 控制台现网 SQL **与 §1 不符** ⇒ 停，以控制台为准并回写；
2. **多 topic `FROM`（`"a", "b"`）语法不被接受** ⇒ 停（方案 B 依赖它；改走其它方案须回到 `HR-C2-1` 重裁）；
3. 页面「调试」试跑**与 §4.0 期望不符** ⇒ 停；
4. 出现 **`/log` 双投**（同一 `/log` 同时进 `log_history` 与 `mqtt_messages`）且未在下一批前消除 ⇒ 立即补改 B / 或回滚；
5. 出现 telemetry 或 presence **回归** ⇒ 立即回滚（§5）；
6. 规则 A / B 的运行统计出现**持续非 2xx / `resource down`** ⇒ 停并诊断。

---

## §7 ★ 待人工裁决登记

| ID | 事项 | 状态 / 建议 |
|---|---|---|
| **`HR-C2-1`** | `/log` 分流方案 | ✅ **已裁决 = 方案 B**（`2026-10-15`） |
| **`HR-C2-2`** | 若选 A 的腾槽方式 | 🚫 **不适用**（未选 A） |
| **`HR-C2-3`** | 是否接受过渡期 `/log` 落 `mqtt_messages` | 🟡 **建议：接受** —— 方案 B 下该状态**只在规则 B 改完前**存在，且**与今日行为零差异**；改完即消除 |
| **`HR-C2-4`（新）** | **解除对规则 A / B 的冻结约束**（冻结设计要求 `FROM` / 谓词不动；本方案须改） | 🔴 **待裁** · 建议批准（方案 B 为 `HR-C2-1` 已选项的必然前提） |
| **`HR-C2-5`（新）** | **`log_predicate` 定义批准**（§2.3 的 5 键「存在」语义） | 🔴 **待裁** · 建议批准（字段取自 log 信封契约 · 与 telemetry 互斥已论证） |
| **`HR-C2-6`（新）** | **施工顺序（A 先 → B 后）+ 控制台作业授权** | 🔴 **待裁** · 建议批准（A 先为严格增量、无回归） |

---

## §8 边界与纪律核对

- **本轮零生产变更**（本文档仅为规格）· 未改 EMQX / firmware / Worker / D1 migration · 未部署 · 未 push。
- **配额**：连接器 `2/2` · 规则 `4/4` · 动作 `4/4` —— **原地编辑不改变配额**。
- **冻结**：第一组括号内 telemetry 谓词 **逐字不变**（`R3.1` 写法）；仅**新增** log 分支 · 仅**扩** B 的否定。
- **密钥**：本文件**不含任何密钥值**；`X-Webhook-Key` 维持现状（在 `HI` 动作层）。
- **未解除门禁**：`G-1` / `G-4` 不变。

---

## §9 关联文档

| 文档 | 作用 |
|---|---|
| `docs/review/Phase-C-C2-Log-History-End-to-End-Review.md` | 前置审查（`CONDITIONAL GO / BLOCKED-1` · 方案 A/B/C/D 评估） |
| `EMQX_Assets/config/data-integration/P3-WP4C-V2V4-Config-After.md` | **现网 SQL 权威原文**（§1 回滚快照来源 · §2.4/§2.5） |
| `docs/review/P3-WP4C-Console-Task-Sheet.md` | 上游同型任务单（对象 / 端点 / 动作规格 · `R3.1` 写法参照） |
| `docs/review/P3-WP4C-Implementation-Discipline.md` | 生产变更纪律 + **§6A 控制台边界规则**（禁猜 SQL · 变更准入序列） |
| `docs/review/Phase-C-C1-V2-Telemetry-Landing-Verification-Record.md` | `V-2` 已闭合（`/log` 实时证据 / `log_history=0` 现状） |
| `src/log/log_envelope.h` | log 信封字段契约（`log_predicate` 依据） |

---

*本文件于 **`2026-10-15`** 建立，为 **`Phase C-C2`（`/log` → `log_history`）方案 B** 的**控制台施工任务单**（`PLAN ONLY`）。*
*基线 = 固件仓 **`e4ef766`** · EMQX 仓 **`3165aab`**；**不含任何生产变更、代码改动或配置变更**；**待人工审核 `HR-C2-4/5/6`**。*
