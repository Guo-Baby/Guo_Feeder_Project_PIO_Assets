# P3 · `WP4C` —— **实施阶段纪律**（`BINDING` · **约束后续所有生产变更**）

> ## 📌 本文档 = **执行约束（Binding）** —— 适用于 `WP4C` 及**后续所有生产变更**
>
> | 项 | 值 |
> |---|---|
> | **状态** | **`ACTIVE / BINDING`** —— 一经确立，**实施阶段全程不得违反** |
> | **建立日期** | **2026-10-12** |
> | **建立基线** | 固件仓 **`0d8b0bd`** · CF 仓 **`9b25203`** · EMQX 仓 **`eec9444`** |
> | **适用范围** | `WP4C` Telemetry Routing 实施 · **并作为后续生产变更的通用纪律** |
> | **上游** | `docs/review/P3-WP4C-EMQX-Rule-Design.md`（🔒 冻结设计）· `P3-WP4C-Implementation-Plan.md` · `P3-WP4C-Pre-Implementation-Checklist.md` · `P3-WP4C-Decisions.md` |
>
> **★ 定性**：本纪律**不引入新架构、不改冻结设计**，只**约束执行方式**。与冻结设计冲突时，**以冻结设计为准**，且**须 `STOP` 并请求人工裁决**。

---

## §1 冻结优先原则

### 1.1 实施阶段**不得修改**

| # | 对象 | 依据 |
|---|---|---|
| 1 | **`H-WP4C-03` 冻结设计**（`P3-WP4C-EMQX-Rule-Design.md`） | `H-WP4C-03 = CLOSED` |
| 2 | **telemetry predicate 逻辑** —— `(schema_version ∧ seq ∧ ts ∧ type ∧ value) ∧ ¬c ∧ ¬cmd` | 同上 §4 |
| 3 | **`/up` topic** | 冻结设计 `C-4` |
| 4 | **firmware topic / rendering** | 冻结设计 `C-3` |
| 5 | **Worker telemetry parsing 边界**（Worker **不解析** telemetry） | `DT:WP-4` §5.1.1 · `H-WP4-FW-01` |

### 1.2 发现设计不足时的**唯一**处置

```
        STOP
         ↓
创建新的 HUMAN_REQUIRED
         ↓
      等待人工裁决
```

> **禁止**：**现场修改设计** · 临时放宽谓词 · 顺手调整 topic / schema / 边界。
> **禁止**：以"先跑通再说"为由绕过冻结设计。

---

## §2 生产变更纪律

> **适用于每一次变更**（EMQX / Worker / D1 均适用）。**一次变更 = 一组「前 → 中 → 后」记录。**

### 2.1 变更**前**（必须全部完成）

| # | 动作 | 产出 |
|---|---|---|
| 1 | **保存当前配置基线** | 变更前配置快照（EMQX 规则 SQL / Action / Connector；Worker 版本） |
| 2 | **保存版本状态** | 平台版本 ID（如 Worker deployment version）+ 时间 |
| 3 | **保存 migration 状态** | 远端已应用清单（`migrations list --remote`） |
| 4 | **保存回滚信息** | 明确的回滚步骤 + 回滚判据 + 回滚后校验点 |

> **无基线不得变更**（尤其 EMQX：其配置 **API 不可回读**，基线**只能人工导出**）。

### 2.2 变更**中**

| # | 规则 |
|---|---|
| 1 | **一次只执行一个明确步骤**（禁止批量合并不同性质的变更） |
| 2 | **每一步必须记录执行结果**（命令 / 操作 / 原始输出 / 时间 / 操作者） |
| 3 | **失败立即停止**（不得带病继续下一步；不得"先做完再一起修"） |

### 2.3 变更**后**

| # | 动作 |
|---|---|
| 1 | **必须验证**（按 §3 的验证步骤，结果标注 `PASS / FAIL / BLOCKED`） |
| 2 | **必须更新仓库存档**（EMQX 控制台配置**必须回写** `EMQX_Assets/config/data-integration/`） |
| 3 | **必须提交实施记录**（变更对象 / 原因 / 前后状态 / 验证结论；**代码与文档分离提交**） |

---

## §3 不可跳过步骤纪律（**顺序固定**）

```
        ① 0003 migration
              ↓
        ② Worker deployment
              ↓
        ③ EMQX baseline export
              ↓
        ④ EMQX Rule creation
              ↓
        ⑤ Existing Rule narrowing
              ↓
        ⑥ Validation V-1 ~ V-7
              ↓
        ⑦ Archive update
```

> **★ ④ 与 ⑤ 必须同处一个变更窗口**（冻结设计 `-S2`）—— 只新增不收窄 ⇒ **telemetry 双投**。

### 3.1 明令禁止

| # | 禁止 | 原因 |
|---|---|---|
| 1 | **先建 Rule 后部署 Worker** | 规则目标不存在 ⇒ 投递落在 catch-all，**静默丢弃** |
| 2 | **先切 Action 后准备数据库** | 远端无表 ⇒ 全量 **`503` + 上游重投风暴** |
| 3 | **未导出基线直接修改 EMQX** | **不可回滚、不可审计**（配置 API 不可回读） |
| 4 | **未验证直接宣布完成** | 违反 `V-1…V-7` 判据 |

> **顺序铁律**：`0003` → Worker → EMQX 变更。**禁止调整顺序**（`H-WP4C-06` 已确认）。

---

## §4 回滚纪律

### 4.1 必须**停止并评估**的情形（触发条件）

| # | 触发 |
|---|---|
| 1 | **Rule predicate 命中异常**（误判 / 漏判） |
| 2 | **Action 失败**（非 2xx / 超时 / 重试放大） |
| 3 | **D1 写入失败**（`503` / `persist_failed` / 约束异常） |
| 4 | **Worker endpoint 异常**（非预期状态码 / 异常响应） |
| 5 | **双投 telemetry**（同一记录同时进 `telemetry_history` 与 `mqtt_messages`） |
| 6 | **`mqtt_messages` 污染**（命令链路被 telemetry 写入 / 记账断链） |

### 4.2 回滚**优先级**（**严格自上而下**）

```
        停止新增流量
              ↓
        恢复旧 Rule
              ↓
        恢复 Action
              ↓
          分析原因
```

> **★ 关键**：**先止损（停流）再回滚配置，最后才分析根因** —— 禁止"先查原因再决定是否止损"。
> **回滚基线来源** = §2.1 保存的**变更前配置基线**（EMQX 侧须人工导出）。

---

## §5 报告要求（**每一步之后必须输出**）

| # | 报告 | 必含字段 |
|---|---|---|
| 1 | **Commit / Change Report** | 修改对象 · 修改原因 · **是否超范围** |
| 2 | **Production Change Report** | **平台** · **操作时间** · **操作者** · **前后状态** |
| 3 | **Validation Report** | **`V-0`…`V-7`** 逐项 `PASS / FAIL / BLOCKED` |
| 4 | **Risk Report** | **新风险** · **偏离冻结设计** · **环境差异** —— **必须新增记录，不允许隐藏** |

---

## §6 纪律执行与冲突处置

| 场景 | 处置 |
|---|---|
| 纪律与冻结设计冲突 | **以冻结设计为准** ⇒ `STOP` + 新 `HUMAN_REQUIRED` |
| 环境与冻结设计不符（如平台限制） | **记录为风险（`R-*`）** + `STOP` + 新 `HUMAN_REQUIRED`，**不得自行绕过** |
| 授权范围外 | **不做** ⇒ 报告"超范围"并请求授权 |
| 验证未通过 | **不得宣布完成**；按 §4 评估回滚 |

---

## §7 关联文档

| 文档 | 关系 |
|---|---|
| `docs/review/P3-WP4C-EMQX-Rule-Design.md` | 🔒 冻结设计（§1 保护对象） |
| `docs/review/P3-WP4C-Implementation-Plan.md` | 实施计划（含 console-ready 规格） |
| `docs/review/P3-WP4C-Pre-Implementation-Checklist.md` | 检查清单 / 人工操作 / 基线采集 |
| `docs/review/P3-WP4C-Decisions.md` | 裁决记录（含实施阶段授权） |

---

*本文档于 **2026-10-12** 建立，为 **`WP4C` 实施阶段纪律（`BINDING`）**；
**零生产变更**，仅确立执行约束。基线 = 固件仓 **`0d8b0bd`** · CF 仓 **`9b25203`** · EMQX 仓 **`eec9444`**。*
