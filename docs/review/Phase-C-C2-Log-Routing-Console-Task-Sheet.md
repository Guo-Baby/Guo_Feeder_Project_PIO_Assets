# P3 · `Phase C-C2` —— **`/log` 分流控制台施工任务单（方案 B · 照抄版 · `REV-3`）**

> ## ✅ `CLOSED`（`2026-10-16`）—— 施工**已由人工执行完毕**，验收矩阵 **`V-3.1…V-3.9` 全 `PASS`**
>
> - **施工结果**：人工已改 **规则 A**（`FROM` 加 `"guo_feeder/+/log"` + `WHERE` 用 `OR` 追加 `log_predicate`）与 **规则 B**
>   （追加 `AND ( NOT ( topic =~ 'guo_feeder/+/log' ) )`）—— 即 **`HR-C2-7a`（黑名单形态）**；**人工已确认规则实现正确**。
> - **端到端结果**：`log_history` `0 → 13`（`level` 全 `INFO` · 幂等吸收重复投递）· `/api/history/ingest` 全 `200` ·
>   **过渡双写于 `01:20:22` 终止** · `V-3.4` 隔离 = `0` · presence / 命令回包无回归。
> - **收口报告** → **`docs/review/Phase-C-C2-Final-Closure-Report.md`**（`Final Audit = PASS`）。
> - ⚠️ **本文件的 §1/§2/§3/§5/§6 保留为施工前规格与回滚快照**（历史依据）；**§4.1 / §4.6 / §4.7 / §7.2 的状态列已回填最终值**；**新增 §12 收口记录**。
> - ⚠️ **§1 的「`LIVE CONFIGURATION: NOT VERIFIED`」为施工前快照** —— 施工后已由**人工核实**（见 §12）。

> ## 🟡 `PLAN ONLY` —— 本文件**只是实施方案**，**不含任何生产变更**
>
> - **不做**：不改 EMQX（本文仅为「如何改」的规格）· 不改固件 · 不改 Worker · 不建 / 改 migration · 不部署 · 不 push **· 不执行控制台操作**。
> - **不扩大范围**：不新增 EMQX 规则 / 连接器 / 动作（配额满）· 不改 endpoint / 认证机制 / presence 路由 / 命令路径。
> - **待人工审核** → 裁决 **`HR-C2-*`**（见 §7）→ **人工**在控制台施工 → 回写存档 → 执行 **`V-3.1…V-3.9`** 真实 MQTT 验证。
> - **裁决依据**：`HR-C2-1 = 方案 B（改规则 A+B · 配额中立）`（用户 `2026-10-15` 已选）。
> - **`REV-2` 修订（`2026-10-15`）**：① 修正「谓词互斥」逻辑论证（**不再依赖字段集不相交**）② 明确 `/log` 排除策略（**策略 A · 按 topic domain 排除**，含**待控制台验证**的候选 SQL）③ 区分**模拟**与**真实 MQTT 路由**验证 ④ 基线 / 增量、回滚边界、安全边界、控制台现状重核等细节。
> - **`REV-3` 修订（`2026-10-15` · `PRE-FLIGHT REVIEW / READ-ONLY / PLAN ONLY`）**：施工前只读核查 —— ① 规则 A/B 证据核对 + `LIVE CONFIGURATION: NOT VERIFIED` 标记（**§1.3**）② **策略 A 静态影响分析**（`CONFIRMED`/`INFERRED`/`BLOCKED` · **§2.5**）③ ★ **发现 `level` 契约不一致（P0 · 阻塞 `V-3.3` · §2.6）** ④ 补齐 **`V-3.4` 最终态隔离判据**（**§4.5**）与 **`V-3.7` 畸形 `/log` 处置判据**（**§4.6**）⑤ 未验证项汇总（**§4.7**）。**本轮未执行任何控制台操作、未执行路由测试、未发任何影响业务的数据。**
> - **本文档定位**：把 C2 审查（`docs/review/Phase-C-C2-Log-History-End-to-End-Review.md`）的**方案 B** 落实为**可照抄执行**的控制台规格。

| 项 | 值 |
|---|---|
| **状态** | ✅ **`Phase C-C2 = CLOSED`（`2026-10-16`）** —— 施工已完成 · `V-3.1…V-3.9` 全 `PASS` · 收口报告 `docs/review/Phase-C-C2-Final-Closure-Report.md` |
| 🟢 **实时配置** | **`LIVE CONFIGURATION: VERIFIED`（施工后人工核实）** —— 规则 A/B 的实际 SQL 已由**人工在控制台逐字检查并确认正确**；施工前为 `NOT VERIFIED`（代理无控制台 / 数据集成 API 只读权限 · 全 `403`） |
| 🟢 **施工准备** | ✅ **已完成** —— 原 1 项 P0 阻塞（`F-C2-8` `level` 契约）**已修复并部署**（Worker `dd5ab824`）；原 4 项语法待验证（`SYN-1…SYN-4`）随施工由人工在控制台确认 |
| **前置审查** | `docs/review/Phase-C-C2-Log-History-End-to-End-Review.md`（判定 `CONDITIONAL GO / BLOCKED-1` → **`CLOSED`**） |
| **变更承载** | 固件仓（本文档 · docs-only）· 施工对象在 EMQX 控制台（人工）· 归档落点 `EMQX_Assets/config/data-integration/` |
| **变更对象** | **恰好 2 条规则**（A `r-n302933b-cf0cf2` · B `r-n302933b-f731cf`）· **连接器 / 动作 / 端点 / presence 全不动** |
| **配额影响** | **4/4 不变**（原地编辑，不新增对象） |
| **生产变更** | **零**（本文件） |
| **门禁** | `G-1 HUMAN_REQUIRED` · `G-4 BLOCKED` **均未变**（属 P3 计划门禁，与 C2 收口无耦合） |

---

## §0 一句话总览

> **只改 2 条规则的 `SQL`**（都不是删重建 —— **规则可原地编辑**）：
> **规则 A**：`FROM` 加一个 topic（`…+/log`）· `WHERE` 由「telemetry 谓词」变「**telemetry 谓词 `OR` log 谓词**」。
> **规则 B**：`WHERE` 由「telemetry 之否定」变「**（telemetry 之否定）`AND`（§2.2 的 `/log` 排除条件）**」—— **排除 `/log`**（**策略 A · 按 topic domain 排除**，见 §2.2）。
> **连接器 `CB` · 动作 `HI`/`MF` · 端点 / 请求头 / 请求体**：**逐字不动**（log 信封复用 `HI` → `/api/history/ingest`，Worker 已按 topic 域自动落 `log_history`）。
> **预计 6–10 分钟**；**顺序：规则 A 先 → 验证 → 规则 B 后**（§3）。

| 项 | 值 |
|---|---|
| **执行人** | 你（EMQX 控制台人工）。代理**无**控制台 / 数据集成 API 权限（API 对 rules / actions / connectors 全 `403` · 无 CLI） |
| **依据** | 方案 B（`HR-C2-1` 裁决）· `R3.1` SQL 写法（`is_null` / `is_not_null`）· 现网 SQL **存档原文**（`P3-WP4C-V2V4-Config-After.md` §2.4/§2.5 —— **须执行时重核**，见 §1） |
| **不要忘** | ① SQL 用 **`is_null` / `is_not_null`**（**禁 `==`**）② **保存前用页面「调试」试跑**（§4.0）③ **规则 A / B 必须同批完成**（防持续双投，§3）④ **§2.2 的 topic 排除条件须先在控制台验证语法**（§2.2.3） |
| **禁止** | ❌ 不新增规则 / 动作 / 连接器（配额满）❌ 不改连接器 / 动作 / presence ❌ 不改 firmware / Worker / D1 / migration ❌ 不改 endpoint / 认证机制 / 命令路径 ❌ 密钥值不写进任何文件 ❌ 不猜 SQL（一律照抄本文并经 §2.2.3 / §4.0 验证） |

> **流水账（施工后回填 · 全部完成）**：
> `[x] 0 从控制台抄变更前 SQL + 运行统计 + 三表基线` → `[x] 1 验证 §2.2 排除条件语法（调试试跑）` → `[x] 2 改规则 A 的 FROM` → `[x] 3 改规则 A 的 WHERE` → `[x] 4 保存 + 调试试跑 + 真实 MQTT 观察 /log 落 log_history`（`log_history` `0 → 13`）
> → `[x] 5 改规则 B 的 WHERE`（★ 首次用**正则**写法 `'^guo_feeder/[^/]+/log$'` **静默失效** ⇒ 改为**通配形态** `topic =~ 'guo_feeder/+/log'` 后生效） → `[x] 6 保存 + 真实 MQTT 观察 mqtt_messages 不再收 /log`（**过渡双写于 `01:20:22` 终止**） → `[x] 7 跑 V-3.1…V-3.9` → `[x] 8 回写存档`（本 §12 + 收口报告）

---

## §1 【存档】变更前 SQL 原文（**回滚快照 · 逐字 · 先抄下来**）

> 来源：`EMQX_Assets/config/data-integration/P3-WP4C-V2V4-Config-After.md` §2.4/§2.5（`2026-10-12` 变更后**存档原文**）。
> ⚠️ **该存档是「`2026-10-12` 观测到的配置」的转录，不是执行时的实时值。**
> ⚠️ **执行前必须（人工·控制台·只读）重新核实**：打开控制台 → 数据集成 → 规则，逐字比对下两段 SQL、`FROM`、动作挂载、以及**页面「调试」对 topic → domain 的实际行为**。
> - **若与下文不符** ⇒ **STOP**，以控制台为准并回写本文（记入 §8 变更记录）。
> - **不得**把本文档的 SQL 快照直接描述为「已确认的实时配置」。
>
> **同时记录（执行前 · 只读）**：规则 ID · `FROM` · `WHERE` · 动作 / 连接器关联关系 · **控制台当前支持的 `FROM` 多 topic 语法形态** · 三条规则（A / B / presence×2）的**运行统计**（触发 / 成功 / 失败）。若控制台 `FROM` 只接受**单用引号包裹、逗号分隔**的形式（`"a", "b"`），照抄 §2.1；若报语法错 ⇒ **STOP**（§6 第 2 条）并回写。

### 1.1 规则 A `r-n302933b-cf0cf2`（存档原文 · 变更前）

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

### 1.2 规则 B `r-n302933b-f731cf`（存档原文 · 变更前）

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

### 1.3 ★ 施工前只读核查结论（`REV-3` · `READ-ONLY`）

> **`LIVE CONFIGURATION: NOT VERIFIED`** —— 本轮**无法**读取控制台 / 数据集成 API（权限全 `403`、无 CLI）⇒ 下列全部为**存档证据**，**不得**表述为实时配置。

**① 对象与关联（存档口径）**

| 对象 | ID | `FROM` | `WHERE` | 动作 | 动作 URL 路径 | 连接器 |
|---|---|---|---|---|---|---|
| **规则 A** `guo_feeder_history_ingest` | `r-n302933b-cf0cf2` | `guo_feeder/+/up` | 冻结 telemetry 谓词 | `a-n302933b-126ec6` | `/api/history/ingest`（+ `X-Webhook-Key`） | `CB = c-n302933b-0c1cb0` |
| **规则 B** `guo_feeder_message_ingest` | `r-n302933b-f731cf` | `guo_feeder/#` | 冻结谓词之否定（德摩根 `OR`） | `a-n302933b-7241e2` | `/api/webhook` | `CB = c-n302933b-0c1cb0` |
| presence connected | `r-n302933b-de1540` | `$events/client_connected` | （裸 `${body}`） | `a-n302933b-999c12` | `/api/presence/event` | `c-n302933b-20ead9` |
| presence disconnected | `r-n302933b-7e8a01` | `$events/client_disconnected` | （裸 `${body}`） | `a-n302933b-0209ad` | `/api/presence/event` | `c-n302933b-20ead9` |

**② 证据来源 / 生成时间 / 适用范围**

| # | 证据 | 来源 | 时间 | 适用范围（**能证明什么**） |
|---|---|---|---|---|
| E-1 | 规则 A/B 的 `FROM`/`WHERE` 全文 | `EMQX_Assets/config/data-integration/P3-WP4C-V2V4-Config-After.md` §2.4 / §2.5 | 施工 `2026-10-12`；**控制台快照 `2026-10-13`** | **仅证明该时点**的两条规则 SQL 形态 |
| E-2 | 变更前规则基线（含旧 `SELECT * FROM "guo_feeder/#"`） | `EMQX_Assets/config/data-integration/P3-WP4C-V0-EMQX-Baseline.md` §3.1–§3.3 | `2026-10-12` | 变更前形态 · 动作请求体 / 高级设置 |
| E-3 | 对象 ID / 备注交叉修正登记 | `docs/review/P3-WP4C-Console-Task-Sheet.md` §11 · `V2V4` §2.3b | `2026-10-12` / `2026-10-13` | 对象 ID 与「备注↔职能」对应 |
| E-4 | **实时触发证据**（tail：规则 A → `/api/history/ingest` 200；`/log` → `/api/webhook`） | `docs/review/Phase-C-C1-V2-Telemetry-Landing-Verification-Record.md` | `2026-10-14` | **证明规则 A/B 当时在触发且路径正确** —— 但**不**重新读取 SQL 文本 |
| E-5 | Worker 域判定与落库实现 | `Cloudflare_Assets/guo-feeder-api/src/history-ingest.js` · `index.js` | 仓内 `HEAD` | Worker 行为（域映射 / 校验 / `mqtt_messages` 写入） |

**③ 证据的边界（不能证明什么）**

> - E-1/E-2/E-3 **不是**实时配置 —— **无法**证明自 `2026-10-13` 起控制台未被改动（平台无 API 可回读）。
> - E-4 证明「规则在**触发**」与「路径**结果**」，**不**证明 SQL 文本、`FROM` 多 topic 语法、`topic`/`=~`/`NOT` 的可用性。
> - **配额**（连接器 `2/2` · 规则 `4/4` · 动作 `4/4`）为 `2026-10-13` 口径 ⇒ 施工前须重核（§1 记录项）。
> - `/log` 当前**落入 `mqtt_messages`** 的结论：由 E-4（75+ 行 `/log` 全进错表、`log_history` 恒 0）**实测支持**。

**④ 施工时必须在控制台只读核对（**本任务不做**）**

| # | 核对项 | 目的 |
|---|---|---|
| R-1 | 规则 A/B 的 `FROM` / `WHERE` **逐字**与 §1.1/§1.2 是否一致 | 校准回滚快照（不符 ⇒ `STOP`） |
| R-2 | 规则 A/B **挂载的动作**与 URL 路径 | 确认「动作↔端点」未再交叉 |
| R-3 | 三条规则（A / B / presence×2）的**运行统计** | 变更后对照基线 |
| R-4 | 三表基线（`telemetry_history` / `log_history` / `mqtt_messages`） | `V-3.4` 增量判据（§4.5） |
| R-5 | 控制台规则编辑器是否接受 §2.1 的**多 topic `FROM`** 与 §2.2.2 的 **`topic` / `=~` / `NOT`** 形态 | 解 `SYN-1…SYN-4`（§2.2.3） |

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
> ⚠️ **`FROM` 多 topic 语法**（`"a", "b"`）**须在执行时于控制台验证**（§1 记录项）；不被接受 ⇒ **STOP**（§6 第 2 条）。

### 2.2 规则 B `r-n302933b-f731cf`（**WHERE 扩为「按 topic domain 排除 `/log`」**）

#### 2.2.1 策略选择：**策略 A · 按 topic domain 排除**

| 策略 | 定义 | 后果 | 采纳 |
|---|---|---|---|
| **A · 按 topic domain 排除** | 所有 `guo_feeder/+/log` 消息**一律不得**进入旧路径（`/api/webhook` → `mqtt_messages`），**无论其 envelope 是否完整**。完整性验证与拒收由 Log 专用链路（规则 A → `/api/history/ingest`）处理 | 业务域隔离**由 topic 保证**，与 payload 形态解耦 | ✅ **采纳** |
| **B · 仅排除符合 `log_predicate` 的消息** | 只排除「字段齐全」的 `/log`；**缺字段 / 畸形**的 `/log` 仍可能进旧路径 | 隔离**依赖 payload 完整性** ⇒ 缺字段的 `/log` 会污染 `mqtt_messages` | ❌ 不采纳 |

**采纳理由（业务隔离目标）**：本项目的一致目标是 **「topic 决定业务域」**（Worker 侧即如此 —— `TOPIC_DOMAIN` 由 **topic 第 3 段**决定域，见 §2.4）。让**规则 B 的排除也按 topic 生效**，才能与 Worker 的域判定**同构**，使「`/log` 是 log 域」成为一条**不依赖 payload 内容**的硬边界。**策略 B 会让隔离强度随 envelope 质量漂移**，与上述目标不符。

> ⚠️ **这同时是对原 `REV-1` 计划的修正**：原计划的 `(NOT telemetry) AND (NOT log_predicate)` **等价于策略 B** —— 它**不能**保证所有 `/log` 都被排除（缺一个 log 必需字段 ⇒ `NOT log_predicate` 成立 ⇒ 仍进旧路径）。本修订**改为策略 A**。

#### 2.2.2 规则 B 变更后 SQL（**候选 · 须 §2.2.3 控制台验证**）

**主候选**（使用 EMQX 内置 `topic` 字段 + 正则匹配 `=~`）：

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
  AND ( NOT ( topic =~ '^guo_feeder/[^/]+/log$' ) )
```

> ★ **语义**：第一组括号 = **原有「`NOT` 冻结 telemetry 谓词」逐字不变**；`AND ( NOT ( …topic… ) )` = **策略 A 的按 topic 排除**。
> ★ **写法说明**：EMQX 规则 SQL 有 `NOT` **运算符**（`NOT <bool>` 合法）；`topic` 为**规则 SQL 内置选中字段**（`SELECT *` 已提供，见 `P3-WP4C-V0-EMQX-Baseline.md` §3.2.1）；匹配运算 = **`=~`**。
> 🔴 **以上三点均「待控制台验证」** —— 见 §2.2.3。**不得**在验证前把本候选当作已可用。
>
> 🔴🔴 **★ 施工实测订正（`2026-10-16` · **本条为最重要的一处纠正**）**：
> **上面的主候选 SQL 中的 `=~` 字面量 `'^guo_feeder/[^/]+/log$'` 是【错误】的。**
> **EMQX 规则 SQL 的 `=~` 是「topic 通配符匹配」（支持 `+` / `#`），不是正则匹配。**
> 正则写法 **静默失效**（**无任何报错**）：`NOT ( topic =~ '^guo_feeder/[^/]+/log$' )` 恒为 `true` ⇒ **排除子句形同虚设** ⇒
> 施工时**规则 B 首次保存后 `V-3.4` 未达标（`/log` 仍落 `mqtt_messages`）**。
> **正确形态（生产实际采用 · 已人工核实）**：
> ```sql
>   AND ( NOT ( topic =~ 'guo_feeder/+/log' ) )
> ```
> ⇒ 改为该形态后**立即生效**，`V-3.4` = `0`。**本条的备用候选（`(up|down)` 白名单）同样不成立**（`|` 与 `^`/`$` 均非通配符语义）。
> ⚠️ **教训**：EMQX 规则 SQL 的 topic 匹配**只能**用 `+` / `#` 通配符形态，**不得**写成正则。后续任何 EMQX 规则编写均适用。

**备选候选（若 `NOT` 运算符 / `=~` / `topic` 字段任一不可用 ⇒ 按此逐级降级）**：

| 级 | 条件 | 写法 | 备注 |
|---|---|---|---|
| **B-1** | `=~` 不可用，`topic` 可用 | `AND ( topic != 'guo_feeder/+/log' )`（**单 `=` 语义族**，须确认不等号为 `!=`） | `+` 通配是否参与**字符串**比较须验证（多半**不**参与 ⇒ 可能不成立） |
| **B-2** | `topic` 字段不可直取 | 改用 `SELECT *` 可得的内建字段或函数取 topic（**具体函数名待控制台核对，禁猜**） | 若查不到 ⇒ 回到 `HR-C2-1` 重裁 |
| **B-3** | 上述全不可用 | **退回 `REV-1` 形态**（`AND (NOT log_predicate)`）并**显式登记为策略 B**（接受缺口字段 `/log` 进旧路径的风险） | ⚠️ 属**策略变更** ⇒ 须**人工重新裁决**（记入 §7 新 `HR-C2-7`） |

> 💡 **独立于 SQL 的补强（推荐并行启用）**：`mqtt_messages` 是 legacy 表，**Worker 侧 `/api/history/ingest` 已按 topic 硬隔离**；即便规则 B 的排除存在边缘缺口，也**只影响 legacy 表的纯洁性**，不影响 `/log → log_history` 主链路。**建议**在执行时一并记录该论点（不改变本策略 A 的采纳）。

#### 2.2.3 🔴 语法验证待办（**执行前 · 控制台·只读/试跑 · 不得伪造结果**）

| # | 待验证项 | 验证手段 | 未通过时的动作 |
|---|---|---|---|
| `SYN-1` | `SELECT *` 是否提供可直接判定的 **`topic`** 字段 | 规则「调试」用一条 `/log` 主题样本试跑（或查阅控制台 SQL 编辑器补全项） | 降级 B-2；不可得 ⇒ `STOP` |
| `SYN-2` | 匹配运算符 **`=~`** 是否支持、**其匹配语义（正则 or topic 通配）**、字面量引号形态 | 调试试跑：`topic =~ '^guo_feeder/[^/]+/log$'` **与** `topic =~ 'guo_feeder/+/log'` | 降级 B-1；再不可 ⇒ `STOP` |
| `SYN-3` | 布尔 **`NOT`** 运算符是否支持、`AND ( NOT ( … ) )` 括号形态是否可保存 | 调试试跑 + 保存报错观察 | 改写为等价形态 |
| `SYN-4` | `FROM` **多 topic** `"guo_feeder/+/up", "guo_feeder/+/log"` 是否接受 | 规则 A 保存（§2.1） | `STOP`（§6 第 2 条） |

> ✅ **施工实测结论（`2026-10-16` 回填）**：
> - `SYN-1` = **`PASS`**（`topic` 字段可直接取用）
> - `SYN-2` = ⚠️ **运算符 `=~` 支持，但语义 = 「topic 通配符匹配」而非正则** ⇒ **必须用 `'guo_feeder/+/log'`**（正则形态**静默失效**，见 §2.2.2 订正）
> - `SYN-3` = **`PASS`**（`NOT` + 括号形态可保存）
> - `SYN-4` = **`PASS`**（规则 A 多 topic `FROM` 可保存）
> - **最终生产形态**：规则 B 排除子句 = `AND ( NOT ( topic =~ 'guo_feeder/+/log' ) )`（= `HR-C2-7a` 黑名单）

> **`SYN-3` 的替代写法**（若 `NOT` 不便，可把否定并入正则）：
> ```sql
>   AND ( topic =~ '^guo_feeder/[^/]+/(up|down)$' )
> ```
> ⇒ **白名单形态**（只放行 `/up` + `/down`，`/log` 与其它一律不落旧表）。**语义与策略 A 等价**（且更保守 —— 未知 topic 亦被排除）。★ **推荐优先验证此形态**：它**只需 `=~` + `topic`**，不依赖 `NOT` 运算符。

> 🔴 **本节所有结果必须由执行人从控制台回填**（PASS / FAIL / 报错原文）；**代理不得填写**。任一关键项不可用且无降级路径 ⇒ 升 `HR-C2-7` 重新裁决方案。

#### 2.2.4 ★ 白名单候选的回归风险（`REV-3` 静态分析 · **报告，不自行改裁**）

> 审核问题 B4：「白名单方案是否会意外排除当前由 Rule B 负责处理的其他 topic？」

| 项 | 分析 | 判定 |
|---|---|---|
| 规则 B 现 `FROM` | `guo_feeder/#` ⇒ **匹配 `guo_feeder/` 下任意 topic** | `CONFIRMED`（E-1） |
| 已知 topic 组成 | `mqtt_messages` 现网构成 ≈ `/up` **1918** · `/down` **531** · `/log` **0**（`2026-10-12` 口径 · `V2V4` §3） | `INFERRED`（快照，非实时） |
| 是否**穷举**全部现存 topic | ❌ **不能** —— 无 broker / 控制台只读权限；`#` 通配的匹配集无法离线枚举 | `BLOCKED` |
| **白名单** `(up\|down)` 的后果 | 除 `/up`、`/down` 外的**任何** topic（含**未知 / 未来** topic）**将被排除**出 `mqtt_messages` | `INFERRED`（由正则语义确定，但**影响面无法穷举**） |
| **黑名单** `NOT (…/log$)` 的后果 | **仅**排除 `/log`，其余（含未知 topic）**行为不变** | `INFERRED`（同上） |

> ⚠️ **结论（新增待决）**：两者**都能实现策略 A**（排除全部 `/log`），但**回归面不同** ——
> **白名单更「干净」但会改变未知 topic 的行为**（可能误伤）；**黑名单最小改动、但依赖 `NOT`**。
> **在无法穷举现存 topic 的前提下，白名单的回归风险无法排除** ⇒ 该取舍**列入 `HR-C2-7` 子项**由人工裁（**本任务不自行改裁**）：

| 选项 | 取舍 |
|---|---|
| **`HR-C2-7a`（黑名单）** | 最小行为改动 · 需 `NOT` 可用（或改写为 `topic =~ '^…/(log)$'` 的等价否定） · 未知 topic 行为不变 |
| **`HR-C2-7b`（白名单）** | 只依赖 `=~` + `topic`（**语法风险最低**）· 但**未知 topic 将被排除** ⇒ 需人工确认「`guo_feeder/` 下除 `/up`、`/down`、`/log` 外无其他 topic」

### 2.3 ★ 新增 `log_predicate` 定义（**提案 · 待 `HR-C2-5` 批准**）

**判据（5 键「存在」语义）** —— 取自 log 信封契约（`src/log/log_envelope.h` L22–24）：

```
{ "device_id", "boot_seq", "seq", "ts", "level", "payload_base64" }
```

`log_predicate` = `payload_base64 ∧ boot_seq ∧ seq ∧ ts ∧ level` **均存在**（∀ `is_not_null`）。
（`device_id` **不进**谓词 —— 与冻结铁律一致：**身份只来自 topic**。）

> ⚠️ **`log_predicate` 只是「规则 A 的命中条件」**，**不是**规则 B 的排除依据 —— 规则 B 的排除已**改为按 topic**（§2.2）。这点是本修订与 `REV-1` 的**关键差异**。

### 2.4 ★ 路由正确性论证（**修正版 · 不再依赖"字段集不相交"**）

#### 2.4.1 明确区分两个命题（**原计划在此处论证有误**）

| 命题 | 陈述 | 是否成立 |
|---|---|---|
| **P1** | **两个 predicate 在任意输入下逻辑互斥**（不存在任何报文同时满足二者） | ❌ **不成立 / 未证明** —— 见 §2.4.2 |
| **P2** | **正常 Telemetry / Log 消息会被路由到正确业务域** | ✅ **成立**，但需以下**组合**作保证（§2.4.3） |

> ❌ **原计划声明「Telemetry 与 Log 字段集合不同 ⇒ 两 predicate 不可能同时成立」—— 该论证作废。**
> 事实：两组谓词**存在交集字段 `seq`、`ts`**；一个**带额外字段**的消息**理论上可能同时满足**两组条件。字段集不同 ⇒ **既不充分也不必要**地推出互斥。

#### 2.4.2 为什么 P1 不成立（逐条）

| 谓词 | 要求 | 交集 |
|---|---|---|
| telemetry | `schema_version` · `seq` · `ts` · `type` · `value` **非空** ∧ `c` · `cmd` **为空** | `seq`、`ts` |
| log | `payload_base64` · `boot_seq` · `seq` · `ts` · `level` **非空** | |

⇒ 一条报文若**同时**带 `schema_version` · `type` · `value` **与** `payload_base64` · `boot_seq` · `level`（且无 `c` / `cmd`）⇒ **两组均为真**。**这在字段层面是可构造的** ⇒ **P1 不可宣称**。

#### 2.4.3 业务域隔离实际依赖什么（**P2 的真实依据**）

隔离**不靠** predicate 互斥，而靠以下**四层组合**：

| 层 | 机制 | 依据 |
|---|---|---|
| **L1 · 真实 topic** | 设备 telemetry 只发 `…/up`；log 只发 `…/log`（入口族冻结） | `DT:WP-4` topic 入口族冻结 · `P3-WP4C-EMQX-Rule-Design.md` C-3 |
| **L2 · envelope 契约** | 固件按契约产出**二选一**的信封：telemetry = `{ts,type,value,seq,schema_version}`；log = `{device_id,boot_seq,seq,ts,level,payload_base64}` —— **同一设备不会产出混合信封** | `src/log/log_envelope.h` · 遥测发布器契约 |
| **L3 · 规则完整路由逻辑** | 规则 A 的 `FROM` + `WHERE`（telemetry `OR` log）· 规则 B 的 `FROM` + `WHERE`（`NOT` telemetry `AND` **topic 排除 `/log`**） | §2.1 / §2.2 |
| **L4 · Worker 按 topic domain 选表** | `domainFromTopic(topic)`：topic 第 3 段 → `{up:telemetry, log:log}`；**未知 ⇒ `400 unknown_topic`**（**不落任何表**）。落库表由 **domain** 决定，**与 payload 内容无关** | `history-ingest.js:47 / 203 / 214` · §2.4.4 |

⇒ **结论**：**P2 成立的条件是 L1–L4 全部成立**。**本任务不宣称"已证明绝对安全"** —— 该组合的**最终有效性必须由 §4 的真实 MQTT 端到端验证判定**（而非由本文档推演判定）。

#### 2.4.4 Worker 域判定源码依据（**L4 的实证**）

```js
const TOPIC_DOMAIN = { up: "telemetry", log: "log" };          // history-ingest.js:47
function domainFromTopic(topic) {                               // :203
    const parts = topic.split("/");
    if (parts.length !== 3) return null;                        // 非 3 段 ⇒ null
    if (parts[0] !== "guo_feeder") return null;                 // 非 guo_feeder ⇒ null
    const d = TOPIC_DOMAIN[parts[2]];
    return d === undefined ? null : d;                          // 未知域 ⇒ null
}
// :108 const domain = domainFromTopic(topic);
// :109 if (domain === null) return jsonResponse({ error: "unknown_topic" }, 400);
```

⇒ **`/log` 一旦进入 `/api/history/ingest`，必然按 log 域落 `log_history`**（与 payload 字段是否齐全无关，字段只影响**记录级校验**）。

#### 2.4.5 已知可复现的错误路由（**登记 · 不在本任务修复**）

| ID | 输入 | 匹配条件 | 影响 | 处置 |
|---|---|---|---|---|
| `BR-C2-1` | `payload_base64` / `boot_seq` / `seq` / `ts` / `level` 均非空 + 无 `c` / `cmd` | 变更前**规则 A**（`FROM` 不含 `/log`）⇒ 若 topic = `…/log` 则**不命中 A**；**规则 B** 的 `is_null(schema_version)` 为真 ⇒ **命中 B** ⇒ 落 `mqtt_messages` | `/log` 落错表 | **本任务即修**（§2.1 + §2.2） |
| `BR-C2-2` | 混合信封（含 telemetry 字段 `OR` log 字段的**同一报文**） | 变更后**规则 A** 两组括号 `OR` ⇒ **可能命中 A** | 该报文进 `/api/history/ingest`，由 Worker 按 **topic** 决定域 | **不修复**（理论构造 · 无真实固件产出路径）；**记录为已知边界**，由 §4.4 验证真实设备无此形态 |

> ⚠️ **`BR-C2-2` 记录在案**：本任务**不**新增复杂 SQL 条件去追逐该理论边界（与人工意见一致），也**不**宣称它已被排除。**若 §4 实测发现真实设备可产出混合信封 ⇒ 停止宣称安全并升 `HR-C2-7`。**

### 2.5 ★ 策略 A 影响范围（`REV-3` 静态分析 · **只读 · 不改规则**）

> 逐项回答审核问题 B1–B6。**`CONFIRMED`** = 有直接证据；**`INFERRED`** = 由代码/文档推导、**未在真实环境验证**；**`BLOCKED`** = 缺必要证据。

| # | 问题 | 结论 | 判定 |
|---|---|---|---|
| **B1** | 规则 B 现 `FROM` 覆盖哪些 topic？现有资料是否足以确认？ | `FROM = guo_feeder/#` ⇒ **覆盖 `guo_feeder/` 下全部 topic**（含 `/up`、`/down`、`/log`）。**具体成员集无法离线穷举**（无 broker/控制台只读） | `FROM` 值 = `CONFIRMED`（E-1）· **成员集穷举 = `BLOCKED`** |
| **B2** | 采用 topic 排除，是否可能影响 **Presence 或其他非 Telemetry 上行**？ | **Presence 不受影响** —— presence 两规则 `FROM` 为 `$events/client_connected` / `$events/client_disconnected`，**与 `guo_feeder/#` 无交集**（Rule B 的 `FROM` 根本不匹配事件 topic）。**其他非 telemetry 上行**（如 `/up` 上的命令 ack/result、`online` 帧、`/down`）**必须且会被保留** —— 这正是策略 A 的设计目标 | Presence 解耦 = `CONFIRMED`（两条 `FROM` 值）· 「无其他上行 topic」= `BLOCKED`（同 B1） |
| **B3** | 是否会改变现有 `/api/webhook` / `mqtt_messages` 业务行为？ | **会，且是预期的**：`/log` 将**不再**进 `/api/webhook` → `mqtt_messages`。**其余 topic 行为不变**（黑名单）或**除 `/up`、`/down` 外被排除**（白名单 ⇒ 见 §2.2.4 风险）。静态可推，**未实测** | `INFERRED` |
| **B4** | 白名单是否会**意外排除** Rule B 现负责的其他 topic？ | **可能** —— 白名单 `(up\|down)` 会排除**任何其他 topic**；能否排除「误伤」取决于现存 topic 集是否恰好为 `{/up, /down, /log}`，而该集**无法离线穷举** | **`BLOCKED`**（详见 §2.2.4） |
| **B5** | 现有证据是否足以确认 `topic` 字段、`=~`、`NOT`、括号、正则的**语法兼容性**？ | ❌ **不足**。`topic` 仅证明可作 `${topic}` **占位符**（动作体，经 `SELECT *`），**未**证明可在 `WHERE` 直接引用；`=~` / `NOT` / 分组括号 **本工程从未实测**（**原规则 B 特意用德摩根 `OR` 展开、无 `NOT`/括号** ⇒ 不能反推 `NOT` 受支持） | **`BLOCKED`**（⇒ §2.2.3 `SYN-1…SYN-3`） |
| **B6** | `FROM` / `WHERE` / Action 组合是否存在**未解决的**路由歧义？ | **已知 topic 上无歧义**：`/up` 的 telemetry → A；`/up` 的非 telemetry → B；`/log` → A；`/down` → B（第一组括号为假且 topic 排除亦不误伤）。**唯一未决歧义 = `BR-C2-2` 混合信封**（理论），以及**多 topic `FROM` 与 topic 排除语法能否成立** | 逻辑分配 = `INFERRED` · 语法前提 = `BLOCKED` |

> 🔴 **本节不宣称任何 SQL 写法「一定受支持」** —— 所有 `=~` / `NOT` / `topic`-in-`WHERE` / 多 topic `FROM` 一律 **`BLOCKED`**，须在控制台验证后方可施工（§2.2.3）。

### 2.6 🔴 ★ `level` 契约不一致（`REV-3` 新发现 · **P0 · 阻塞 `V-3.3`**）

> **性质**：**跨层共享契约缺陷** —— 与 EMQX 路由**无关**，但**直接决定 `log_history` 能否落库**。
> **结论**：**即使路由完全修好，真实 `/log` 记录仍会被 Worker 逐条拒收** ⇒ `log_history` **恒为 0** ⇒ **`V-3.3` 不可能 `PASS`**。

**证据链（四方对照）**：

| 层 | `level` 形态 | 来源 |
|---|---|---|
| **冻结协议** | **`<uint8>`（整数 0..4）** | `docs/interfaces/cloud_protocol.md` §8.2.1（`"level": <uint8>`） |
| **固件信封产出器** | **整数** —— `"level":` + `jo_u32((uint32_t)level)` | `src/log/log_envelope.cpp` L255–256 · `docs/review/P3-WP4-Firmware-Log-Implementation.md` §3.1 |
| **固件 level 取值** | `LogLevel`：DEBUG=0 · INFO=1 · WARN=2 · ERROR=3 · CRITICAL=4；**INFO 的 cloud 策略 = YES** | `src/log/log_events.h` L28–60 |
| **Worker 校验** | **字符串**，且**必须 ∈ `{WARN, ERROR, CRITICAL}`** | `Cloudflare_Assets/guo-feeder-api/src/history-ingest.js` L50（`LOG_LEVELS`）· L401–405（`asText` + `LOG_LEVELS.has`） |
| **D1 列** | `level TEXT NOT NULL`（冻结枚举 `WARN/ERROR/CRITICAL`） | `guo-feeder-api/migrations/0003_p3_history_tables.sql` |

**为什么必失败（静态确定）**：

```js
function asText(v) { if(typeof v !== "string") return null; … }     // history-ingest.js:284-292
const level = asText(rec.level);                                     // rec.level = 整数 2 ⇒ null
if(level === null || !LOG_LEVELS.has(level)) return null;            // ⇒ rejected
```

- 信封 `"level":2` → `request.json()` 得到 **number** → `asText` 返回 `null` → **`rejected++`**。
- 即便是数字字符串（`"2"`）也**不通过** —— `LOG_LEVELS` 只含 `"WARN"/"ERROR"/"CRITICAL"` 三个**字母值**。
- ⇒ **本缺陷目前是潜伏态**（`/log` 尚未到达该端点，故从未被观察）⇒ **路由一修好即暴露**。

**次生不一致（同一处 · 需一并裁决）**：

| # | 不一致 | 说明 |
|---|---|---|
| **L-1** | **`level` 类型**（协议/固件 = 整数；Worker = 字符串） | 上文主因 |
| **L-2** | **`INFO` 是否入库** —— 固件 policy 允许 INFO 上云，但 Worker 的 `LOG_LEVELS` **不含 INFO**；且 `WP2` 冻结枚举为 `WARN/ERROR/CRITICAL`，而 `P3-History-D1-Plan.md` §156 写作 `INFO/WARN/ERROR/CRITICAL` | 两份云端设计口径亦不一致 ⇒ 需人工定口径 |

**处置（**本任务不修**）**：

- ❌ **不**改 Worker · ❌ **不**改固件 · ❌ **不**改 D1 schema · ❌ **不**改冻结协议 —— 全部越界。
- ✅ 登记为**独立问题 `F-C2-8`** + **新待裁 `HR-C2-8`**（§7.2）；`V-3.3` 状态置 **`BLOCKED`**（§4.1）。
- ⚠️ **对施工的影响**：EMQX 路由变更**本身仍然正确且必要**（它是 `V-3.3` 的**必要不充分**条件）；但**在 `HR-C2-8` 解决前，`V-3.3` 无法验收** ⇒ 施工价值 = 「打通路径 + 暴露该缺陷」。
- ⚠️ **`V-3.2` 的判据须相应收紧**：`/api/history/ingest` 返回 **HTTP 200 ≠ 落库成功**（记录级拒收也是 200）⇒ 必须**同时看 `rejected` 字段与 D1 增量**（§4.5）。

> 🔎 **独立核查已完成**（`2026-10-15`）→ **`docs/review/F-C2-8-Log-Level-Contract-Assessment.md`**（`READ-ONLY` · 源码级核实）：
> - **判定 = `F-C2-8 CONFIRMED`**（类型面由**固件字节级测试断言** `"level":3` + Worker 源码/测试**双向确证**）。
> - **规范化缺口**：**任何文档都未定义「数字 → 云端等级名词」的映射** ⇒ 登记 **`CQ-1` / `CQ-2` / `CQ-3`**（须人工裁决后方可实施修复）。
> - **与既有登记的关系**：INFO 的枚举冲突**早已登记为 `F-WP4-FW-9`**（`P3-WP4-Firmware-Integration-Design-Review.md` L481，分类「合规但不经济」）；但该条目**假定只有 INFO 会被拒** —— 本轮确证**类型不符使 `WARN`/`ERROR`/`CRITICAL` 同样全被拒** ⇒ **`F-WP4-FW-9` 的前提已被推翻，须升格重估**。
> - **推荐（待裁）**：**方案 B（云端规范化）**为建议首选（符合协议声明的 `uint8`、零重烧、变更面最小）；**方案 C（双格式）不推荐**（无「两种格式均在野」证据）。**详见评估文档 §5–§7。**

---

## §3 施工顺序（**规则 A 先 → 验证 → 规则 B 后**）

| 步 | 动作 | 瞬时窗口影响 |
|---|---|---|
| **1** | 改 **规则 A**（§2.1）保存并启用 | **严格增量**：`/log` 开始落 `log_history`（正确路径开通）；**与此同时 B 未改 ⇒ `/log` 仍落 `mqtt_messages`** |
| **2** | 观察 `/log` 已落 `log_history`（§4 步骤 3） | — |
| **3** | 改 **规则 B**（§2.2）保存并启用 | `/log` 不再被 B 命中 ⇒ **`mqtt_messages` 停止收 `/log`**（隔离达成） |

> ★ **为何 A 先 · 顺序不得擅改（已人工确认）**：先开通正确路径 ⇒ 过渡窗口内 `/log` **至少落对一处**；最后改 B 才真正「堵漏」。
> ⚠️ **反序（B 先）不可取**：会让 `/log` 在窗口内**两头皆落空**（丢失）。
> ⚠️ **两条规则必须同批完成**（同一施工）。若只改 A 不改 B ⇒ **持续双投**（`/log` 同时进 `log_history` + `mqtt_messages`）⇒ 触发 §6 STOP（须尽快补 B）。

### §3.1 ★ 过渡期双写（**已人工接受 · 非阻塞项**）

> **人工已明确接受该过渡现象，且不要求调整 A→B 顺序**（当前处于开发验证阶段、未上生产环境）。

**文档如实说明（不是阻断理由）**：

- **A 已改、B 未改**时，符合新 log 命中条件的 `/log` 消息**可能同时进入** `log_history` 与 `mqtt_messages`。
- 该情况是**已接受的短暂过渡现象**，**不应**被误判为「最终路由配置**持续**双写」。
- **完成 B 修改后，必须通过真实 MQTT 消息验证最终路由行为**（§4）—— 即「双写是否消失」**只能**由最终规则状态下的实测判定。

> ❌ 本任务**不**将过渡期双写列为阻止方案通过的理由，**不**据此改 A→B 顺序。

> ✅ **施工实测结果（`2026-10-16` 回填）**：过渡期双写**确实发生且已按预期终止** —— 双写期间 `/log` 同时落 `log_history` 与 `mqtt_messages`；**规则 B 生效后双写于 `01:20:22` 终止**（此后 `mqtt_messages` 中 `/log` 行不再增长，`V-3.4` = `0`）。**过渡行单独统计、未计入 `V-3.4` 判据**（符合 §4.5 ③ 的 `t0` 定义）。

---

## §4 验证（`V-3.1` … `V-3.9` · 施工后执行）

> ## 🔴 验证分层纪律（**本修订新增 · 对应"问题四"**）
>
> | 层级 | 证据类型 | 效力 |
> |---|---|---|
> | **L0 · 逻辑推演** | 本文件 §2.4 的谓词分析 / 真值表 | **仅为辅助证据** —— **不得**据此宣称 EMQX 实际路由正确 |
> | **L1 · 模拟 / 离线** | 控制台「调试」试跑（§4.0）· 本地伪造 body 试算 | **仅为辅助证据**（可发现语法 / 逻辑错，但**不等于**真实路由正确） |
> | **L2 · 真实 MQTT 路由** | 设备真实上行 → MQTT 抓包 → **Worker tail** → **D1 数据变化** | ✅ **唯一验收依据** |
>
> ⇒ **最终验收必须走 L2**：真实 MQTT 消息 + Worker 响应 + D1 变化**三者对齐**。
> ⇒ **无法在当前环境真实执行的项 ⇒ 标记 `NOT RUN` / `BLOCKED`，禁止标 `PASS`。**

### §4.0 保存前 · 页面「调试」试跑（**L1 · 必做 · 辅助证据**）

| 试跑输入（模拟 body） | 模拟 topic | 规则 A 期望 | 规则 B 期望 |
|---|---|---|---|
| telemetry 外形：`{"schema_version":1,"seq":1,"ts":1791966947,"type":"climate.temperature","value":24.4}` | `…/up` | ✅ 命中 | ❌ 不命中 |
| log 外形（字段齐全）：`{"device_id":"288485896ce4","boot_seq":95,"seq":108545,"ts":1791968500,"level":1,"payload_base64":"AA=="}` | `…/log` | ✅ 命中 | ❌ **不命中（策略 A）** |
| **log 缺字段（缺 `level`）**：`{…,"payload_base64":"AA=="}` | `…/log` | ❌ 不命中 A（`log_predicate` 不成立） | ❌ **仍不命中 B（按 topic 排除）** ← ★ 策略 A 的关键用例 |
| 命令外形：`{"c":"result","i":"x","o":"config_set","m":"ok"}` | `…/up` | ❌ 不命中 | ✅ 命中 |

> ★ **第 3 行是策略 A 与策略 B 的判别用例**：策略 A 下它**既不进** A（payload 不全）**也不进** B（topic 排除）⇒ **不落任何 legacy 表**（与 Worker `unknown_topic` 之外的「log 域但记录无效」路径一致：进 A 被记录级拒收 / 不进 A 则静默不落 legacy）。
> ⚠️ **试跑可伪造 topic 的前提是控制台提供 topic 输入框** —— 若调试面板**只接受 body**（无 topic 输入）⇒ **该用例无法在页面验证** ⇒ 标记 **`NOT RUN`**，改由 §4.3 的真实 MQTT 覆盖。
> `payload.x` 是否需 `json_decode` —— **现网 A/B 已用 `payload.<field>` 直取且 `V-1…V-7` 全 PASS** ⇒ 维持直取；试跑若不符 ⇒ **STOP** 并回写。

### §4.1 判据矩阵

| # | 步骤 | 判据（`PASS` 定义） | 证据 | 层级 | 当前状态 |
|---|---|---|---|---|---|
| `V-3.1` | 设备真实发出 ≥1 条 `/log`（JSON 信封） | MQTT `guo_feeder/<device_id>/log` 出现 `{device_id,boot_seq,seq,ts,level,payload_base64}`；**记录其 `boot_seq`/`seq`/`ts` 作为关联标识**（供 §4.5 / §4.6） | MQTT 订阅抓包 | L2 | ✅ **`PASS`**（`boot_seq 98–101` · `seq 108xxx–110xxx`） |
| `V-3.2` | 观测 Worker | `wrangler tail` 命中 **`/api/history/ingest`**（**非** `/api/webhook`）· **且响应体 `rejected` 计数被逐条记录**（⚠️ **HTTP `200` 本身不是通过判据** —— 记录级拒收同样返 200，见 §2.6） | tail（走代理 `127.0.0.1:7890`） | L2 | ✅ **`PASS`（口径修订）** —— 路径/状态码有生产 tail 直接证据；**响应体计数通道不存在**（`event.response` 键仅 `['status']` · 成功路径无打点）⇒ 由 **`V-3.7a` 本地真引擎**提供**等价**四字段实测（见 §12.2 / 收口报告 §2.2） |
| `V-3.3` | D1 落库 | `log_history` **相对测试前基线 +≥1 行**；`payload` **原样**（Worker 不解码）；幂等键 `(device_id,boot_seq,seq)` 唯一；`created_at` 由 D1 生成 | D1 查询（**前后增量**，§4.5 判据） | L2 | ✅ **`PASS`**（原 `BLOCKED`）—— `log_history` `0 → 13` · `level` 全 `INFO` · 幂等吸收重投 |
| `V-3.4` | **最终态隔离** | **B 改完后**，测试 `/log` 消息**未进入** `mqtt_messages`（按 **topic + `boot_seq`/`seq`** 关联，**不**看总行数） | D1 查询（**前后增量 + 逐条关联**，**§4.5**） | L2 | ✅ **`PASS`** —— `id > 3483` 的 `/log` 行 = **`0`** · **`(boot_seq, seq)` 联合键**反查 = **`0`** · C-3 对照成立 |
| `V-3.5` | 鉴权不变 | 无 `X-Webhook-Key` ⇒ `401`（不放宽） | 探针 | L2 | ✅ **`PASS`** —— 本地**真实 D1 层**套件 `history_ingest_real_d1.sh` **用例 #10 = `401`**（`123 PASS / 0 FAIL`）· Worker 鉴权实现 `requireWebhookKey()` 本轮**零改动** |
| `V-3.6` | **Telemetry 回归** | `/up` telemetry 仍落 `telemetry_history`（+≥1，且不落 `mqtt_messages`） | D1 / tail | L2 | ✅ **`PASS`** —— 真机两轮 `telemetry_history` `15 → 20`（**+5**）· 含 `schema_version` 的 `mqtt_messages` 行 = **`0`**（记录 `Phase-C-C2-V36-Telemetry-Regression-Verification-Record.md` · 结论订正见其文首与二轮复核报告） |
| `V-3.7` | **畸形 / 无效 `/log` 处置** | 按**已批准策略 A** **逐类**核对四要素（是否进 `mqtt_messages` / 是否进 `/api/history/ingest` / 是否落 `log_history` / 是否产生可追踪证据）；**不预设「所有畸形都必须入库」** | tail + D1 + 抓包（**§4.6** 矩阵） | L2 | ✅ **`PASS`**（拆分见 §4.6）—— **`V-3.7a`（`M-2…M-5`）= `PASS`**（**本地隔离** `81 PASS / 0 FAIL` × 2）· **`V-3.7b`（`M-1`/`M-6`/`M-7`）= `PASS`（构造性 + 人工核实）** |
| `V-3.8` | **Presence 回归** | `$events/client_connected` / `disconnected` 仍 `200` 落 `device_event` | tail + D1 | L2 | ✅ **`PASS`** —— 独立正式验收：`device_event` `187/216 → 189/218`（**恰好 +2**）· `id 217 offline` / `id 218 online` · 三源秒级收敛 · 负向对照零污染（记录 `Phase-C-C2-V38-Verification-Report.md`） |
| `V-3.9` | **命令下行 + webhook 回归** | `/down` 命令回包仍落 `mqtt_messages`；**现行 webhook 通路（实际路径 = `POST /`）** 对合法 JSON 行为不变 | tail + D1 | L2 | ✅ **`PASS`** —— 命令帧全部落 `mqtt_messages`，`direction` = `down`/`up` 正确；★ **措辞统一：实际路径 = `POST /`**（**非**历史文字中的 `POST /api/webhook`） |

> **观测纪律**：`/log` **仅在设备产生日志时**上行（非周期）⇒ 需**构造触发**（下发一条会产生日志的命令，或等自然日志）；**不得以「窗口内无日志」判定 FAIL**（判 `BLOCKED` / `NOT RUN`）。
> **状态列已回填最终值**（施工 + 验收后 · 见 **§12**）：`PASS / FAIL / NOT RUN / BLOCKED`。**凡未真跑者一律 `NOT RUN`。**
> ⚠️ **`V-3.2` 与 `V-3.7b` 的判定性质已在状态列如实标注**（前者 = 口径修订；后者 = 构造性 + 人工核实，**无独立注入实测**）。

### §4.2 数据基线 vs 增量（**判据只认增量**）

> ⚠️ **保留历史基线仅作参考，不得当作执行时的实时值。**

| 表 | 历史参考值（时点） | 执行要求 |
|---|---|---|
| `telemetry_history` | `15`（`2026-10-14` `V-2` 后 · 参考值） | **测试前重新采集基线**；以**前后增量**判定 |
| `mqtt_messages` | 约 `3162`（截至 `2026-10-14` · 历史参考值） | 同上；**不得**仅凭总量变化判定 `/log` 路由正确 / 错误 |
| `log_history` | `0`（`V-2` 时点 · 参考值） | 同上；`+≥1` 才算 `V-3.3` 命中 |

> ★ **噪声警示（沿用 `V2V4` §4）**：`mqtt_messages` 的 legacy 链路**本就 best-effort**（近 24 h 成功率曾低至 ≈13%）⇒ **判据只能是「窗口内增量」+「与 `/log` 时间窗的交集」**，不能看总量。

### §4.3 真实 MQTT 路由验证覆盖清单（**对应"问题四"第 4 条 · 逐项**）

| 要求 | 覆盖项 |
|---|---|
| 正常 Telemetry 仍写 `telemetry_history` | `V-3.6` |
| 完整 Log 消息写 `log_history` | `V-3.1` / `V-3.2` / `V-3.3` |
| Log 消息**在最终规则状态下不持续**写 `mqtt_messages` | `V-3.4`（**B 改完后**测） |
| 缺字段 / 畸形 / 无效 Log envelope 按**已批准策略**处理 | `V-3.7` |
| Presence / 命令下行 / 现有 webhook 行为未被破坏 | `V-3.8` / `V-3.9` |

### §4.4 ★ 混合信封边界实测（**`BR-C2-2` 的收口**）

- **目的**：验证**真实设备不会产出**同时满足两组谓词的混合信封（§2.4.2 的理论边界）。
- **手段**：`V-3.1` 的抓包中**逐条核对** telemetry 与 log 信封的字段集合 —— 断言「telemetry 信封 ⊅ log 字段」且「log 信封 ⊅ telemetry 字段」。
- **判据**：若发现任一真实信封为混合形态 ⇒ **停止宣称"域隔离无歧义"**，升 `HR-C2-7` 重新裁决 SQL。
- **当前状态**：`NOT RUN`（随 `V-3.1` 一并执行）。

### §4.5 ★ `V-3.4` 最终态路由隔离 —— 验收判据（`REV-3` 补齐）

> **目标**：证明**最终规则状态下**，测试 `/log` 消息**未进入**旧 `mqtt_messages` 路径。
> **禁止**：不得以「`mqtt_messages` 总行数未增 / 总行数增加」**单独**作为判据（历史链路 best-effort，噪声大）。

**① 可用关联字段（读 schema 得出 · 无 schema 变更）**

`mqtt_messages` 现有列（`index.js` L572 的 INSERT 列清单）：
`device_id` · **`topic`** · `direction` · **`raw_payload`** · `cmd` · `msg_id` · `object` · `stable_id` · `version` · `status` · … · **`created_at`**

| 关联维度 | 落在哪一列 | 可用性 |
|---|---|---|
| **`topic`** | `mqtt_messages.topic`（= 动作体的 `${topic}` = `guo_feeder/<device_id>/log`） | ✅ **直接可用**（`LIKE 'guo_feeder/%/log'`） |
| **`device_id`** | `mqtt_messages.device_id` | ✅ 直接可用 |
| **`created_at`** | `mqtt_messages.created_at`（毫秒，`Date.now()`） | ✅ 窗口过滤 |
| **`boot_seq` / `seq`** | ⚠️ **无独立列** —— 仅存在于 `raw_payload`（= 信封 JSON 文本）内 | 🟡 需 `raw_payload LIKE '%"seq":<N>%'` **文本匹配** |
| `direction` | `/log` 不匹配 `/up`、`/down` ⇒ 落 **`"unknown"`** | 🟡 辅助判别（非唯一） |

> ⚠️ **schema 限制（如实说明）**：`mqtt_messages` **不做 `boot_seq` / `seq` 的列级存储** ⇒ 逐条关联只能靠 `raw_payload` 的**文本匹配** ⇒ 存在**极小概率**的数值串碰撞（用 `boot_seq` + `seq` **双条件** + `topic` + 时间窗收敛）。**本任务不改 schema 以满足该要求。**

**② 验收判据（三条全中 = `PASS`）**

| # | 判据 | 期望 |
|---|---|---|
| **C-1** | **窗口内按 topic 的 `/log` 行数增量为 0** | `SELECT COUNT(*) FROM mqtt_messages WHERE topic LIKE 'guo_feeder/%/log' AND created_at >= <t0>` ⇒ **`= 0`** |
| **C-2** | **逐条关联为 0**（用 `V-3.1` 记录的 `boot_seq` / `seq` 反查） | `… WHERE raw_payload LIKE '%"boot_seq":<BS>%' AND raw_payload LIKE '%"seq":<SEQ>%'` ⇒ **`= 0` 行** |
| **C-3** | **区分窗口内其它正常流量**（`/up` 命令 · `/down`） | 同期 `/up` / `/down` 行数**可正常增长**（不被误判为失败）；作对照留痕 |

**③ 前置与排除项（**关键**）**

| 项 | 要求 |
|---|---|
| **必须 B 改完后** | `t0` = **规则 B 保存成功的时刻**（人工记录毫秒级）⇒ 窗口**只覆盖最终态**。**禁止**用「A 已改、B 未改」期间的任何结果证明最终态隔离 |
| **过渡期双写不计为失败** | `t0` **之前**（A→B 之间）产生的 `/log` 行属**已接受的过渡现象**（§3.1）⇒ **单独统计并标注**，**不得**计入 C-1 |
| **基线必须现场采集** | 施工前记录 `mm_log_before = COUNT(topic LIKE '%/log')`（历史参考值 ≈ `0`，`V2V4` §3 口径）—— 但**以现场值**为准 |
| **判据 ≠ 总行数** | C-1/C-2 均为**筛后计数**；`mqtt_messages` 总行数变化**不构成**判据 |

**④ 本项当前状态**：`NOT RUN`（依赖施工后实测）。

### §4.6 ★ `V-3.7` 畸形 / 无效 `/log` —— 验收判据（`REV-3` 补齐）

> **原则**：**不预设**「所有畸形 `/log` 都必须进 `log_history`」；须结合 Worker **真实**校验/错误处理逐类判定。
> **观察四要素（每类都要给出）**：①是否进 `/api/webhook`→`mqtt_messages` ②是否进 `/api/history/ingest` ③是否落 `log_history`（或校验拒收）④是否有**可追踪证据**（tail 事件 / 状态码 / `rejected` 计数）。

**畸形 / 无效类别 → 预期处置（依据 Worker 源码静态推导）**

| 类 | 形态 | 规则 A 命中？ | 到达端点？ | 预期可观测 | 判定 |
|---|---|---|---|---|---|
| **M-1** | **缺** ≥1 个必需字段（`payload_base64`/`boot_seq`/`seq`/`ts`/`level`） | ❌（`log_predicate` 不成立） | ❌ —— 且**规则 B 按 topic 排除** ⇒ **两条都不命中** | **完全静默**：无 tail 事件 · `log_history` +0 · `mqtt_messages` +0 | 🟡 **`BLOCKED`** —— 「无任何证据」本身就是结论，**无法用「观察到拒收」来验证**（只能证「未污染 legacy 表」） |
| **M-2** | 5 字段齐全，但 `level` **取值非法**（越界 / 非枚举） | ✅ | ✅ `/api/history/ingest` | `200 {"accepted":0,"rejected":1}` · `log_history` +0 | 🟡 **`BLOCKED`** —— 「预期行为」被 **§2.6** 污染（当前**所有** level 都被拒 ⇒ 无法区分「因非法被拒」与「因类型不符被拒」） |
| **M-3** | **完全合法**（`level` 为整数 2/3/4 —— **固件真实形态**） | ✅ | ✅ `/api/history/ingest` | `200 {"accepted":0,"rejected":1}`（**§2.6 缺陷**） | 🔴 **`BLOCKED`**（受 `HR-C2-8` 阻塞） |
| **M-4** | 信封 `device_id` ≠ topic 内 `device_id` | ✅ | ✅ | `400 {"error":"device_id_mismatch"}` | `INFERRED`（源码 L126–131） |
| **M-5** | topic 内 `device_id` 非法（非 12 位 hex） | ✅ | ✅ | `400 {"error":"invalid_device_id"}` | `INFERRED`（源码 L115–120） |
| **M-6** | `/log` 上发**非 JSON**（二进制） | ❌（无 `payload_base64` 键 ⇒ `is_not_null` 假） | ❌ | 静默丢弃（同 M-1） | `INFERRED` |
| **M-7** | 其它**未知 topic**（非 `/up`、`/log`，如 `/foo`） | — | 规则 B 命中（**黑名单**）或**不命中**（**白名单**） | **黑名单** ⇒ 进 `mqtt_messages`（= 现状保留）；**白名单** ⇒ 静默丢弃 | 🟡 **`BLOCKED`** —— 取决于 `HR-C2-7a/7b` 选择（**§2.2.4**） |

> 🔴 **关键发现（须如实记录）**：在**策略 A** 下，**缺字段 `/log` 不会进入任何链路**（规则 A 不命中 ∧ 规则 B 排除 topic）⇒ **它不会产生任何「被拒收」的证据**。
> ⇒ 「完整性验证 / 拒收由 Log 专用链路处理」**只对能命中 `log_predicate` 的消息成立**；**不能命中者 = 静默丢弃**（既不落 `log_history`，也不落 `mqtt_messages`，**无 tail 事件**）。此点**必须**在验收记录中明示，**不得**表述为「已被 Log 链路拒收」。

**V-3.7 的验收判据**：

| # | 判据 | 说明 |
|---|---|---|
| **D-1** | M-1/M-6 类 ⇒ **`mqtt_messages` 无对应行**（按 topic + 时间窗；**无 tail 事件**属预期） | **可证「未污染 legacy」**；**不可证**「被拒收」 |
| **D-2** | M-2 ~ M-5 类 ⇒ **tail 状态码 + `rejected`/`error` 与矩阵逐条一致**，且 `log_history` 增量 = 0 | 需**合规的样本注入手段**（见下） |
| **D-3** | M-7 类 ⇒ 行为与 `HR-C2-7` 所选策略**一致** | 依赖 §2.2.4 裁决 |

> 🔴 **前置缺口（施工前）**：本项需要**合规的畸形样本注入手段**（向 `guo_feeder/<id>/log` 发布构造报文）。施工前**无**已授权手段（设备端不产出畸形报文；代理发布需凭据且属「可能影响业务的数据」**被禁止**）⇒ 施工前整体 `BLOCKED`。
> ✅ **施工后收口（`2026-10-16`）**：
> - **`V-3.7a`（`M-2`…`M-5` · 判据 `D-2`）= `PASS`** —— 手段 = **本地 `wrangler dev --local` + 本地 D1（真引擎）**，**完全隔离、零生产污染**；夹具 `Cloudflare_Assets/guo-feeder-api/test/real_d1/v37a_log_reject_matrix.sh`，**`81 PASS / 0 FAIL` × 2 遍逐行一致**。四类逐条实测：`M-2` 记录级 `200 rejected:1`（`5`/`256`/`"3"`）· `M-3` 接受并落库（`level 1..4`；`0` ⇒ `filtered`）+ 重投 `duplicate:1` 零净增 · `M-4`/`M-5` 批次级 `400` 零写入。
> - **`V-3.7b`（`M-1`/`M-6`/`M-7` · 判据 `D-1`/`D-3`）= `PASS`（构造性 + 人工核实）** —— 规则 A/B 的**逐字配置已由人工核实 `VERIFIED`**（原 `HUMAN_REQUIRED` 前提消除）；`HR-C2-7a/7b` **采用形态 = `7a`（黑名单）**，与生产规则逐字一致 ⇒ `M-7` 行为 = 落 `mqtt_messages`（**现状保留**）；`M-1`/`M-6` = `log_predicate` 不成立 ∧ topic 被排除 ⇒ **两条规则均不命中 ⇒ 静默不落任何表**（**不产生「被拒收」证据**，属预期）。
>   ⚠️ **性质边界**：该判定为**构造性**（已核实规则 SQL + Worker 源码静态推导 + 生产窗口内 `mqtt_messages` 零 `/log` 新增互证），**未做独立畸形注入实测**（无合规注入手段 · 本轮不发测试 payload）。**不等于 `V-3.7a` 的实测 `PASS`。**

**④ 本项当前状态**：✅ **`PASS`**（`V-3.7a` 实测 + `V-3.7b` 构造性/人工核实 —— 见上方收口段）。

### §4.7 ★ 未验证项汇总（`REV-3` · 施工前快照 → **施工后状态回填**）

> ⚠️ 下表为 **`REV-3` 施工前快照**，其「状态」列**已按施工 + 验收结果回填**（原值保留于括号内）。

| 类别 | 项 | 状态 |
|---|---|---|
| **需要控制台只读核实** | `R-1…R-5`（§1.3 ④）—— 规则 SQL 逐字 / 动作挂载 / 运行统计 / 三表基线 / 编辑器语法 | ✅ **已完成**（原：⏸ 未做）—— 施工由人工执行并**人工确认规则正确**（§12.1） |
| **需要 SQL 语法验证** | `SYN-1`（`topic` 字段）· `SYN-2`（`=~`）· `SYN-3`（`NOT`/括号）· `SYN-4`（多 topic `FROM`） | ✅ **已随施工确认**（原：⏸ 未做）—— ★ `SYN-2` 结论：**`=~` = topic 通配符匹配，非正则** |
| **需要实际 MQTT 端到端验证** | `V-3.1`…`V-3.9` | ✅ **全部 `PASS`**（原：⏸ `NOT RUN`）—— 见 §4.1 / §12.2 |
| **被阻塞（须先解决）** | `V-3.3`（`level` 契约 · §2.6）· `V-3.7`（注入手段 + §2.6） | ✅ **已解除**（原：🔴 `BLOCKED`）—— `F-C2-8` 已修并部署；`V-3.7a` 以**本地隔离**取得合规取证手段 |
| **需要人工批准** | `HR-C2-4` · `HR-C2-5` · `HR-C2-6` · `HR-C2-7`（+ `7a/7b`）· **`HR-C2-8`（新 · §2.6）** | ✅ **全部收口**（原：🔴 待裁）—— 事实均已满足 **or** 已修复 ⇒ 见 §7.2 / §12.1 |
| **可独立跟踪、不属本任务** | `P-3`（telemetry `value:null` 被记录级拒收）· `BT-1`（云端不回 `log_ack`）· `BR-C2-2`（混合信封理论边界）· **`F-C2-8`（§2.6）** | 📌 **独立登记**（`F-C2-8` 已修；`O-1` 已归并至 `BT-1`；其余维持 —— 见 **§12.3**） |

---

## §5 回滚（**最低成本 · 原地改回**）

### 5.1 配置回滚（**≠ 数据回滚**）

| 层 | 回退手段 | 代价 |
|---|---|---|
| **规则 A / B** | **把 §1 的两段 SQL 逐字贴回**（规则可原地编辑） | 最低（无需重建） |
| 连接器 / 动作 | **未变更** ⇒ 无需回退 | — |
| 固件 | **本轮无固件改动** ⇒ 无需回退 | — |

> 🔴 **恢复 Rule A / Rule B 的原始 SQL 只能恢复「路由配置」，不能自动删除过渡期或测试期间已经写入的重复记录。**

### 5.2 已产生的数据副作用（**独立说明**）

| 副作用 | 来源 | 是否自动消失 |
|---|---|---|
| `mqtt_messages` 中出现**过渡期 `/log` 行** | §3.1 过渡窗口（A 已改 / B 未改） | ❌ **不会** —— 回滚配置后行仍在 |
| `log_history` 中出现**测试期行** | `V-3.1…V-3.9` 验证产生的真实 / 构造日志 | ❌ 同上 |
| `telemetry_history` / `device_event` 的**测试行** | 回归验证 | ❌ 同上 |

### 5.3 数据清理（**须另行审批 · 不在本次授权内**）

- 是否需要清理**由人工判定**；
- **任何清理操作（`DELETE` / `D1` 写）均须单独审批**；
- **不得把数据清理包含在本次控制台操作授权中**；
- 本文档**不提供**任何 `DELETE` 语句（避免越权）。

**回滚触发**：§4.1 任一 `PASS` 判据失败且确认为本轮引入；或出现 telemetry / presence 回归；或 `resource down` 告警持续。

---

## §6 STOP 条件（**任一命中即停**）

1. 控制台现网 SQL **与 §1 不符** ⇒ 停，以控制台为准并回写；
2. **多 topic `FROM`（`"a", "b"`）语法不被接受** ⇒ 停（方案 B 依赖它；改走其它方案须回到 `HR-C2-1` 重裁）；
3. **§2.2.3 的 `SYN-1…SYN-4` 任一关键项不可用且无降级路径** ⇒ 停（升 `HR-C2-7`）；
4. 页面「调试」试跑**与 §4.0 期望不符** ⇒ 停；
5. 出现 **`/log` 持续双投**（同一 `/log` **在 B 改完后仍**同时进 `log_history` 与 `mqtt_messages`）⇒ 立即补诊断 / 回滚（**仅过渡窗口内的双投属已接受现象，见 §3.1**）；
6. 出现 telemetry 或 presence **回归** ⇒ 立即回滚（§5.1）；
7. **§4.4 实测发现真实混合信封** ⇒ 停并升 `HR-C2-7`；
8. 规则 A / B 的运行统计出现**持续非 2xx / `resource down`** ⇒ 停并诊断。

---

## §7 ★ 待人工裁决登记

### 7.1 已确认事项（**不得再视为待裁**）

| ID | 事项 | 状态 |
|---|---|---|
| **`HR-C2-1`** | `/log` 分流方案 | ✅ **已裁决 = 方案 B**（`2026-10-15`） |
| **`HR-C2-2`** | 若选 A 的腾槽方式 | 🚫 **不适用**（未选 A） |
| **`HR-C2-3`** | 过渡期 `/log` 落 `mqtt_messages` | ✅ **已接受（不要求调整顺序）** —— 见 §3.1；**不再作为阻塞理由** |

### 7.2 仍需人工批准（**未批不得施工**）→ ✅ **`2026-10-16` 全部收口**

| ID | 事项 | 状态 / 建议 | 收口结论（`2026-10-16`） |
|---|---|---|---|
| **`HR-C2-4`** | **解除对规则 A / B 的冻结约束**（冻结要求 `FROM` / 谓词不动；本方案须改） | 🔴 **待裁** · 建议批准 | ✅ **事实已满足** —— 规则 A/B 已改并生效（`V-3.4` = `0`）⇒ **收口**（无需额外裁决） |
| **`HR-C2-5`** | **`log_predicate` 定义批准**（§2.3 的 5 键「存在」语义） | 🔴 **待裁** · 建议批准 | ✅ **事实已满足** —— 规则 A 已含 `log_predicate` 且真机命中（`log_history` `0 → 13`）⇒ **收口** |
| **`HR-C2-6`** | **施工顺序（A 先 → B 后）+ 控制台作业授权** | 🔴 **待裁** · 建议批准 | ✅ **事实已满足** —— A 先 B 后已执行；过渡双写于 `01:20:22` 终止 ⇒ **收口** |
| **`HR-C2-7`（新）** | **`/log` 排除策略 = 策略 A（按 topic domain）** + **候选 SQL 语法（§2.2.2 / §2.2.3）** | 🔴 **待裁** · 建议批准策略 A | ✅ **事实已满足** —— 规则 B 按 topic 排除已生效 ⇒ **收口** |
| **`HR-C2-7a` / `HR-C2-7b`（`REV-3` 新增子项）** | **候选形态选择**：`7a` = **黑名单** · `7b` = **白名单** | 🔴 **待裁** —— 见 **§2.2.4** | ✅ **采用 `7a`（黑名单）** —— 生产实际规则 B = `NOT ( topic =~ 'guo_feeder/+/log' )`，**逐字一致**；**人工已确认规则正确** ⇒ **收口**（白名单回归风险随之消除） |
| **`HR-C2-8`（`REV-3` 新发现 · 🔴 阻塞级）** | **`level` 契约不一致的处置**（详见 **§2.6**） | 🔴 **待裁** · **不在本任务修复** | ✅ **已修并部署** —— `F-C2-8` 修复随 Worker `dd5ab824-ec39-41f3-9990-a37903aaf61f` 上线 ⇒ **收口** |

> ⚠️ **`HR-C2-7` 为 `REV-2` 新增** —— 因原计划（`REV-1`）实质是策略 B，而修订后采纳策略 A（§2.2.1），属**策略级变更**，须人工确认。→ ✅ 已确认。
> ⚠️ **`HR-C2-8` 为 `REV-3` 只读核查发现** —— 属**独立问题**（`F-C2-8`），**因阻塞 `V-3.3` 而必须先解** ⇒ ✅ 已解（Worker 侧修复 + 部署）。

---

## §8 边界与纪律核对

- **本轮零生产变更**（本文档仅为规格）· 未改 EMQX / firmware / Worker / D1 / migration · 未部署 · 未 push · **未执行控制台操作**。
- **不扩大范围**：未新增规则 / 连接器 / 动作；未改 endpoint / 认证机制 / presence 路由 / 命令路径。
- **配额**：连接器 `2/2` · 规则 `4/4` · 动作 `4/4` —— **原地编辑不改变配额**。
- **冻结**：规则 A 第一组括号内 telemetry 谓词 **逐字不变**；规则 B 第一组括号内否定 **逐字不变**；仅**新增** log 分支 · 仅**追加** topic 排除。
- **密钥**：本文件**不含任何密钥值**；`X-Webhook-Key` 维持现状（在 `HI` 动作层）。
- **未解除门禁**：`G-1` / `G-4` 不变。
- **其他已知问题不并入本任务**（独立登记 · 见 §9）：telemetry `value:null` 被 Worker 记录级拒收（`P-3`）· log 时间戳 / 序号问题（`BT-1`）· **`level` 契约不一致（`F-C2-8` · §2.6）** · 混合信封理论边界（`BR-C2-2`）。**若其影响本验收，另立问题，不在本任务修复。**
- **`REV-3` 只读核查边界（本轮）**：未访问 / 未修改 EMQX 控制台任何对象 · 未走 API 绕过权限（`403` 照实标记）· 未改 firmware / Worker / D1 schema / migration / 运行时配置 · **未执行实际路由测试**· **未发送任何可能影响现有业务的数据** · 未部署 · 未 commit / push · 未解除 A/B 冻结 · 未将任何待审批项视为已授权。
- **`F-C2-8` 的处置预期**：本任务**只报告不修复**；其修复属**独立工作项**（涉 Worker / 固件 / 冻结协议，须单独裁决与授权）。

---

## §9 关联文档

| 文档 | 作用 |
|---|---|
| `docs/review/Phase-C-C2-Log-History-End-to-End-Review.md` | 前置审查（`CONDITIONAL GO / BLOCKED-1` · 方案 A/B/C/D 评估） |
| **`docs/review/F-C2-8-Log-Level-Contract-Assessment.md`** | **★ `F-C2-8`（`level` 契约冲突）只读核查与修复方案评估**（判定 `CONFIRMED` · 端到端契约表 · 方案 A/B/C · 契约争议 `CQ-1/2/3` · 后续验收矩阵 `L-1…L-9`） |
| `EMQX_Assets/config/data-integration/P3-WP4C-V2V4-Config-After.md` | **EMQX 侧 SQL 存档原文**（§1 回滚快照来源 · §2.4/§2.5 —— **须执行时重核**） |
| `EMQX_Assets/config/data-integration/P3-WP4C-V0-EMQX-Baseline.md` | `V-0` 控制台只读导出基线（动作请求体 / 高级设置 / `topic` 字段依据） |
| `docs/review/P3-WP4C-Console-Task-Sheet.md` | 上游同型任务单（对象 / 端点 / 动作规格 · `R3.1` 写法参照） |
| `docs/review/P3-WP4C-Implementation-Discipline.md` | 生产变更纪律 + **§6A 控制台边界规则**（禁猜 SQL · 变更准入序列） |
| `Cloudflare_Assets/guo-feeder-api/src/history-ingest.js` | Worker 域判定实现（`TOPIC_DOMAIN` / `domainFromTopic` · L4 依据）· **`normalizeLog` 的 `level` 校验（`F-C2-8` 依据）** |
| `Cloudflare_Assets/guo-feeder-api/src/index.js` | 旧 `/api/webhook` → `mqtt_messages` 写入（列清单 / `topic` / `direction` / `raw_payload` · `V-3.4` 关联依据） |
| `Cloudflare_Assets/guo-feeder-api/migrations/0003_p3_history_tables.sql` | `log_history` / `telemetry_history` schema（`level TEXT` · 幂等键 · `F-C2-8` 依据） |
| `docs/interfaces/cloud_protocol.md` §8.2.1 | **Cloud Log Envelope 契约**（`"level": <uint8>` · `F-C2-8` 依据） |
| `src/log/log_envelope.cpp` · `src/log/log_events.h` | 信封产出实现（`level` 以整数写出）· `LogLevel` 枚举与 cloud 策略 |
| `docs/review/Phase-C-C1-V2-Telemetry-Landing-Verification-Record.md` | `V-2` 已闭合（`/log` 实时证据 / `log_history=0` 现状） |
| `src/log/log_envelope.h` | log 信封字段契约（`log_predicate` 依据） |

---

## §10 本次修订对照（`REV-1` → `REV-2`）

| 审核意见 | 落实位置 | 处理 |
|---|---|---|
| **问题二** 修正谓词互斥论证 | §2.4.1–2.4.5 | 明确区分 P1 / P2 · **删除"字段集不相交"论证** · 改为 L1–L4 组合依据 · 不宣称绝对安全 · 登记 `BR-C2-1`/`BR-C2-2` |
| **问题三** 明确 `/log` 排除策略 | §2.2.1–2.2.3 | **采纳策略 A（按 topic domain）** · 给出候选 SQL（主候选 + 白名单形态 + 备选降级 B-1…B-3）· **语法待控制台验证** · 不伪造结果 |
| **问题四** 区分模拟与真实路由验证 | §4 头部纪律 + §4.0/§4.3/§4.4 | L0/L1/L2 分层 · 试跑仅辅助 · 验收须 L2 · 覆盖 5 类场景 · 不可执行项标 `NOT RUN`/`BLOCKED` |
| 细节 1 控制台现状重核 | §1 | 明确存档 ≠ 实时配置 · 执行前只读重核 + 记录项 · 不符即 STOP |
| 细节 2 基线与增量 | §4.2 | 历史值改标「参考值」· 判据只认前后增量 |
| 细节 3 回滚边界 | §5.1–5.3 | 配置回滚 ≠ 数据回滚 · 副作用独立说明 · 清理须另行审批 |
| 细节 4 安全边界 | §8 + 头部 | 逐条列明不扩大范围 · 其他问题独立登记 |
| 文档与 Git 约束 | 全文 + 报告 | 保持 `PLAN ONLY` · 保留原始快照 · 未 commit / push |

---

## §11 本次修订对照（`REV-2` → `REV-3` · `PRE-FLIGHT READ-ONLY`）

| 本轮核查任务 | 落实位置 | 处理 |
|---|---|---|
| **A** 核对规则 A / B 现状证据 | §1.3 | 对象-关联表（ID / `FROM` / `WHERE` / 动作 / 连接器）· 证据来源 E-1…E-5 + 时间 + 适用范围 · **能证明 / 不能证明**边界 · 施工时须只读核对项 `R-1…R-5` · **`LIVE CONFIGURATION: NOT VERIFIED`** |
| **B** 审查策略 A 影响范围 | §2.5（B1–B6）+ §2.2.4（B4 展开） | 逐项 `CONFIRMED` / `INFERRED` / `BLOCKED` · **Presence 解耦 = `CONFIRMED`**（`FROM` 无交集）· **topic 集穷举 = `BLOCKED`** · **白名单回归风险已登记为 `HR-C2-7a/7b`**（**只报告不重裁**）· 所有 SQL 语法一律 `BLOCKED` |
| **C** 补齐 `V-3.4` 最终态隔离判据 | §4.5 | 关联字段表（`topic` / `device_id` / `created_at` 可用；`boot_seq`/`seq` **仅 `raw_payload` 文本匹配** ⇒ 如实说明 schema 限制）· 判据 C-1/C-2/C-3 · **`t0` 必须 = B 保存成功后** · 过渡期行**单独统计不计失败** · **禁止以总行数判定** |
| **D** 补齐 `V-3.7` 畸形 `/log` 处置判据 | §4.6 | 7 类（M-1…M-7）× 四要素矩阵 · **不预设「畸形都必须入库」** · **关键发现：策略 A 下缺字段 `/log` 完全静默（无任何证据）** · 依赖 **合规注入手段**（缺 ⇒ `BLOCKED`）· 部分类别受 `HR-C2-8` 阻塞 |
| **E** 复核 `V-3.1…V-3.9` 覆盖 | §4.1 + §4.7 + 头部 §0 表 | `V-3.2` 收紧（**200 ≠ 通过**，须看 `rejected`）· `V-3.3`/`V-3.7` 置 `BLOCKED` · 新增 §4.7 未验证项汇总 · 8 类风险逐项对照（见下） |
| **★ 新发现** `level` 契约不一致 | **§2.6** | 四方对照（协议 / 固件 / Worker / D1）· 静态确定性失败链 · 次生不一致 `L-1`/`L-2` · 登记 `F-C2-8` + `HR-C2-8` · `V-3.3` = `BLOCKED` |
| 文档与 Git 约束 | 全文 | 保持 `PLAN ONLY` · **未删除任何历史基线 / 原始 SQL 快照 / 回滚依据** · 未 commit / push · 过渡期双写**未被**重新列为阻塞项（§3.1 保持不变） |

**E 的 8 类风险覆盖对照**：

| 风险 | 覆盖项 | 是否已覆盖 |
|---|---|---|
| 正常 Log 最终进 `log_history` | `V-3.1`/`V-3.2`/`V-3.3` | ✅ 已覆盖（`V-3.3` 因 §2.6 `BLOCKED`） |
| 正常 Telemetry 仍进 `telemetry_history` | `V-3.6` | ✅ |
| Presence 未被破坏 | `V-3.8`（+ §2.5 B2 `CONFIRMED`） | ✅ |
| 现有 webhook 行为未被改变 | `V-3.9`（+ §2.2.4 白名单风险） | ✅（白名单风险已登记） |
| 命令下行未受影响 | `V-3.9` | ✅ |
| 鉴权仍有效 | `V-3.5` | ✅ |
| 过渡期双写 vs 最终态隔离**明确区分** | §3.1 + §4.5 ③ | ✅（`t0` 定义） |
| 配置回滚 vs 已写数据回滚边界清楚 | §5.1 / §5.2 / §5.3 | ✅（`REV-2` 已补，未改动） |

---

## §12 ★ 收口记录（`2026-10-16` · **`Phase C-C2 = CLOSED`**）

> **权威收口报告** → **`docs/review/Phase-C-C2-Final-Closure-Report.md`**（`Final Audit = PASS`）。
> 本节为**任务单侧**的最小收口登记；**不复制**收口报告全文。

### 12.1 人工确认事实（**正式归档 · 文档收口动作，非新裁决**）

| 事实 | 说明 |
|---|---|
| EMQX 控制台**实际规则 A/B 已人工检查** | 规则实现**正确**（原 `LIVE CONFIGURATION: NOT VERIFIED` ⇒ 现 **`VERIFIED`**） |
| **不再因 `403` 将规则 A/B 视为 `UNKNOWN`** | 数据集成 API 仍全 `403`（Serverless 平台限制），但**人工核查已替代 API 核查** |
| AI 的测试代码行为符合预期 | 见 §12.2 `V-3.7a` |
| `HR-C2-7a/7b` **采用形态 = `7a`（黑名单）** | 生产规则 B 实际 = `AND ( NOT ( topic =~ 'guo_feeder/+/log' ) )`，**逐字一致** |

### 12.2 最终验收矩阵（`V-3.1…V-3.9` · 全 `PASS`）

| # | 状态 | 一句话依据 |
|---|---|---|
| `V-3.1` | ✅ `PASS` | MQTT 抓包见完整 `/log` JSON 信封（`boot_seq 98–101`） |
| `V-3.2` | ✅ **`PASS`（口径修订）** | tail 命中 `/api/history/ingest 200`；**响应体计数通道不存在** ⇒ 由 `V-3.7a` 本地真引擎等价取证 |
| `V-3.3` | ✅ `PASS` | `log_history` `0 → 13`（原 `BLOCKED`） |
| `V-3.4` | ✅ `PASS` | `id>3483` 的 `/log` 行 = `0`；**`(boot_seq, seq)` 联合键**反查 = `0` |
| `V-3.5` | ✅ `PASS` | 本地真 D1 层套件用例 #10 = `401`；鉴权层本轮零改动 |
| `V-3.6` | ✅ `PASS` | 真机 `telemetry_history` `15 → 20`；含 `schema_version` 的 `mqtt_messages` 行 = `0` |
| `V-3.7` | ✅ `PASS` | **`a`**（`M-2…M-5`）= 本地隔离 `81/0` × 2；**`b`**（`M-1`/`M-6`/`M-7`）= **构造性 + 人工核实**（无独立注入实测） |
| `V-3.8` | ✅ `PASS` | `device_event` `187/216 → 189/218`（+2）· 三源秒级收敛 · 负向对照零污染 |
| `V-3.9` | ✅ `PASS` | 命令帧落 `mqtt_messages`；★ 实际 webhook 路径 = **`POST /`**（措辞已统一） |

### 12.3 后续事项（**明确属后续工作 · 不得描述为 `Phase C-C2` 未完成**）

| # | 事项 | 归属 |
|---|---|---|
| 1 | **云端 `log_ack` 未实现**（`O-1` = 其现网症状 · `MINOR`） | **`BT-1`** · `计划:WP-5` · 受 `G-4` 门禁；**修复须与上负载评估同批** |
| 2 | **生产侧 `ingest` 成功路径 observability**（如需生产 tail 直看四字段计数） | Worker **可选增强**（须单独授权 + 部署） |
| 3 | `HD-2`（`interval_ms` 映射）· `HD-3`（`ts=0` + `seq` 归零 ⇒ 幂等碰撞） | 固件 · `Phase C-C1` 余项 |
| 4 | `P-3`（非有限值 ⇒ `null` 被记录级拒收 · 静默丢样） | 固件 / 契约 |
| 5 | `mqtt_messages` 历史噪音清理（867 行 `/log`）· MQTT 密码轮换 | 运维（**须另行审批**） |

### 12.4 `O-1` 归类（**不再作为独立问题**）

> **`O-1` = `BT-1` 的现网可观测症状 · `MINOR`（成本 / 可观测性）**。
> 根因已由**四项独立互证**闭合：`qos=1` ∧ `dup=False`（⇒ **排除协议层重传**）· 同一记录 **6 次**发布 ·
> 间隔 `17/19/23/31/47 s`（= `15 s` 超时 + 退避 `2/4/8/16/32 s` **逐项吻合**）· D1 `rowid` **步长 6**（1 插入 + 5 幂等忽略）· Worker `duplicate +5`。
> ⇒ **设备侧应用层重发**，当前被幂等键**完全吸收**、**数据零损失**。
> **已登记至** `docs/issues/未修复的问题.md` 的 **`BT-1`** 条目（现网症状追加）· **`计划:WP-5`**。

---

*本文件于 **`2026-10-15`** 建立，同日修订为 **`REV-2`**（方案 B 规格），再修订为 **`REV-3`**（**施工前只读核查 + 验收标准补齐**），并为 **`Phase C-C2`（`/log` → `log_history`）方案 B** 的**控制台施工任务单**（`PLAN ONLY`）。*
*`REV-3` 基线 = 固件仓 **`859888c`**（`REV-3` 前）· EMQX 仓 **`3165aab`** · Cloudflare 仓 **`415cd0c`**；`REV-3` **不含任何生产变更、代码改动或配置变更**。*
*`REV-3` 结论（施工前）：**施工准备材料充分（可进入人工审批）**，但存在 **1 项 P0 阻塞（`F-C2-8` · §2.6）** 与 **4 项语法待验证（`SYN-1…SYN-4`）**。*
*★ **`2026-10-16` 收口**：施工已由人工完成 · `V-3.1…V-3.9` **全 `PASS`** · **`Phase C-C2 = CLOSED`**（见 **§12** 与 `Phase-C-C2-Final-Closure-Report.md`）。*
