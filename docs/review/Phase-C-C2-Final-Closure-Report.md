# P3 · `Phase C-C2`（`/log` → `log_history`）—— **最终收口审计报告**

> **性质**：`FINAL AUDIT`（收口审计 + 文档收口 + 提交）。**不是**新一轮测试，**未**扩大验证范围。
> **日期**：`2026-10-16`（本地 `GMT+8`）
> **判定**：✅ **`Final Audit = PASS`** ⇒ ✅ **`Phase C-C2 = CLOSED`**
> **审计纪律**：代码 = 第一依据；设计文档用于解释预期；冲突必须报告。
> **生产操作**：**零** —— 未发 MQTT 报文 · 未踢设备 · 未发布测试 payload · 未 `INSERT`/`UPDATE`/`DELETE` D1 · 未改 EMQX · 未部署 · 未重启设备。

---

## §0 结论摘要

| 项 | 结论 |
|---|---|
| **A. 功能目标** | ✅ **已实际成立** —— 设备 `/log`（JSON 信封）→ EMQX 规则 A → Worker `/api/history/ingest` → D1 `log_history` **全链贯通**（`log_history` `0 → 13`，`level` 全 `INFO`，幂等键吸收重投）；且 `/log` **不再进入** legacy `mqtt_messages` |
| **B. 验收矩阵** | `V-3.1…V-3.9` **全部 `PASS`**（其中 `V-3.2` = `PASS（口径修订）` · `V-3.7b` = `PASS（构造性 + 人工核实）`）—— 详见 **§2** |
| **C. 未闭合项** | **无 `BLOCKER`**。① 必须人工裁决 = **0 项**；② 仅需文档修订 = 已在本轮完成；③ 可延后 = 6 项（**§4**）；④ 真正阻塞 Close = **0 项** |
| **D. 三仓状态** | 全部按本阶段范围内提交并 push（**§6**）；**无临时文件入库** · **无生产代码变更** · **无未授权配置变更** |
| **STOP 条件** | **无一命中**（`STOP-1…STOP-5` 逐条核对见 **§5**） |

> **本报告回答的 6 个问题（对应审计要求 §5）**：功能目标（§1）· 验收矩阵（§2）· 未闭合项（§4）· 代码/工作树（§6）· 文档收口（§3）· 后续事项（§4.3）。

---

## §1 A. 功能目标是否成立

### 1.1 设计目标（`Phase C-C2` 的定义）

> 让设备上行 **`/log` 会话**（JSON 信封 `{device_id,boot_seq,seq,ts,level,payload_base64}`）
> 经 **EMQX 规则分流** 投递到 **`/api/history/ingest`**，由 Worker **按 topic 域** 落入 **D1 `log_history`**，
> **同时**不再污染 legacy `mqtt_messages` 路径，且**不破坏** telemetry / presence / 命令下行。

### 1.2 成立证据（三个独立层级）

| 层 | 证据 | 结论 |
|---|---|---|
| **前置缺陷（已修）** | `F-C2-8`（`level` 整数 vs 字符串契约）· `F-C2-9`（EMQX 动作体把设备载荷包进 `payload`，Worker 原要求顶层）—— 代码已修并部署（Worker 版本 `dd5ab824-ec39-41f3-9990-a37903aaf61f`）；契约文档三层统一（`docs/interfaces/log_envelope_contract.md` · `cloud_protocol.md` §8.2.1） | ✅ 闭合 |
| **路由（人工施工）** | 人工在 EMQX 控制台改 **规则 A**（`FROM` 加 `"guo_feeder/+/log"` + `WHERE` 用 `OR` 追加 `log_predicate`）与 **规则 B**（追加 `AND ( NOT ( topic =~ 'guo_feeder/+/log' ) )`）· **人工已确认规则实现正确** | ✅ `VERIFIED`（人工事实，见 §3.4） |
| **真机端到端** | `log_history` `0 → 13`（`boot_seq 98–101` · `level` 全 `INFO` · 幂等吸收重复投递）· `/api/history/ingest` 全 `200` · **过渡双写于 `01:20:22` 终止** · presence / 命令回包无回归 | ✅ `PASS` |

### 1.3 关键实施事实（跨任务复用）

- **★ EMQX 规则 SQL 的 `=~` 是「topic 通配符匹配」（`+` / `#`），不是正则。**
  正则写法 `'^guo_feeder/[^/]+/log$'` **静默失效**（无报错，`NOT(…)` 恒 `true` ⇒ 排除形同虚设）。
  正确形态：`topic =~ 'guo_feeder/+/log'`。**已写入本报告与任务单，供后续 EMQX 规则编写复用。**
- 规则配额 **`4/4`** 全程未变（原地编辑 2 条规则，**未新增对象**）。

---

## §2 B. 最终验收矩阵

### 2.1 主矩阵（审计要求逐项）

| # | 项 | 最终状态 | 判定依据（证据） | 备注 |
|---|---|---|---|---|
| **`V-3.2`** | 观测 Worker | ✅ **`PASS`（口径修订）** | tail 命中 **`/api/history/ingest` `200`**（含 `/log` 帧 `len=387`、telemetry 帧 `len≈163/166`）· 均**非** `/api/webhook` | ⚠️ 原判据中「**响应体 `rejected` 计数被逐条记录**」**证据通道不存在**（§2.2）⇒ 判据口径修订为「路径 + 状态码」并由 `V-3.7a` 提供**等价**的四字段实测 |
| **`V-3.4`** | 最终态路由隔离 | ✅ **`PASS`** | **`id > 3483` 的 `/log` 行 = `0`** · **`(boot_seq, seq)` 联合键反查 = `0` 行** · 同期 `/up`/`/down` 行正常增长（C-3 对照成立） | ★ **关联必须用 `(boot_seq, seq)` 联合键** —— `boot_seq` **单键不可用**（同一 boot 下多记录，且文本匹配会误命中） |
| **`V-3.6`** | Telemetry 回归 | ✅ **`PASS`** | 真机两轮（`2026-10-16 01:32–01:52`）：`telemetry_history` `15 → 20`（**+5**，与 MQTT 抓包**字段级 1:1**）· 含 `schema_version` 的 `mqtt_messages` 行 = **`0`** · telemetry 信封路径 = `POST /api/history/ingest 200` | 附**限定条款**：Round A **无** tail 路径级证据（见 §3.2 订正 C-1/B-3）· 「测试期间未重启」表述**已订正** · 「顺带复证 `V-3.8` `PASS`」**已撤回**（见 §3.2） |
| **`V-3.7a`** | 畸形/无效 `/log` 处置（`M-2…M-5`） | ✅ **`PASS`** | **本地隔离** Worker + 本地 D1（真引擎）：`81 PASS / 0 FAIL`，**两遍逐行一致** · `M-2` 记录级 200 `rejected:1`（`5`/`256`/`"3"`）· `M-3` 接受并落库（`level 1..4`；`0` ⇒ `filtered`）+ 重投 `duplicate:1` 零净增 · `M-4`/`M-5` 批次级 `400` 零写入 · **零生产污染** | 夹具 `Cloudflare_Assets/guo-feeder-api/test/real_d1/v37a_log_reject_matrix.sh`（本轮**已提交**） |
| **`V-3.7b`** | `M-1`/`M-6`/`M-7` 处置 | ✅ **`PASS`（构造性 + 人工核实）** | 规则 A/B **逐字配置已人工核实 `VERIFIED`**（原 `HUMAN_REQUIRED` 前提消除）；`HR-C2-7a/7b` **采用形态 = `7a`（黑名单 `NOT ( topic =~ 'guo_feeder/+/log' )`）**，与生产实际规则**逐字一致** ⇒ `M-7`（未知 topic）行为 = **落 `mqtt_messages`（现状保留）**；`M-1`/`M-6`（缺字段 / 非 JSON）由 `log_predicate` + topic 排除 ⇒ **两条规则均不命中 ⇒ 静默不落任何表** | ⚠️ **判定性质 = 构造性**（规则 SQL 已核实 + Worker 源码静态推导 + 生产窗口内 `mqtt_messages` 零 `/log` 新增互证）；**未做独立畸形注入实测**（无合规注入手段 · 且本轮不发布测试 payload）。**不等于** `V-3.7a` 的实测 `PASS` |
| **`V-3.8`** | Presence 回归 | ✅ **`PASS`** | 独立正式验收（`2026-10-16 07:09–07:16 UTC`）：真实设备 `offline → online`；`device_event` `187/216 → 189/218`（**恰好 +2**）：`id 217 offline`（`reason:"kicked"`）+ `id 218 online`；tail 两请求 `POST /api/presence/event 200`；**三源秒级收敛**（EMQX `connected_at` ↔ `event_at` ↔ 设备自发 `/up {"cmd":"system","id":"online","src":"device"}`）· `peername` 端口 `:35514→:34313` 证新会话；**负向对照** = 非设备客户端 presence `200` 但 **D1 零新增** | ⚠️ **`HTTP 200` 未被单独用作 `PASS` 依据**；记录 → `docs/review/Phase-C-C2-V38-Verification-Report.md` |
| **`V-3.9`** | 命令下行 + webhook 回归 | ✅ **`PASS`**（措辞已统一） | `/down` 命令回包落 `mqtt_messages`，`direction` = `down`/`up` 正确；**现行 webhook 通路的实际路径 = `POST /`**（**不是**历史文字中的 `POST /api/webhook`） | ★ **措辞统一**：本文与任务单、审查文档一律改为实际路径 **`POST /`**；**不因路径名称差异重测** |

### 2.2 `V-3.2` 口径修订的依据与边界（**审计要求 §4**）

**事实（`CONFIRMED`）**：`wrangler tail` 的 `event.response` **键仅 `['status']`** ⇒ **不留响应体**；
且 `history-ingest.js` 的 `console.*` **仅 2 处 `console.error`** ⇒ **成功路径无打点**。
⇒ **原判据「生产 tail 直接看到 `accepted/duplicate/rejected/filtered` 计数」在现有通道下不可能取证。**

**处置（**未**修改生产代码、**未**部署）**：

| 原判据分量 | 修订后的取证方式 | 效力 |
|---|---|---|
| tail 命中 `/api/history/ingest`（非 webhook） | 生产 tail 实测 | ✅ **直接证据**（保持） |
| `rejected` 计数逐条记录 | **`V-3.7a` 本地隔离**：同一 Worker 源码 + 同一 `0003` schema + **真实 D1 引擎**，四字段（`accepted`/`duplicate`/`rejected`/`filtered`）逐类实测 | ✅ **等价证据**（`M-2`/`M-3`/`M-4`/`M-5` + 幂等重投） |

> **判定**：这属于**验收证据口径**的最小修订，**未改变任何设计目标**（端点、语义、落库行为均未动）。
> **可选增强（非阻塞 · 转后续）**：若将来需要在**生产**侧直接观测四字段计数，须为成功路径**增加打点并部署 Worker** —— 属**独立的 observability enhancement**，**不在本阶段授权范围内**（登记于 §4.3）。

### 2.3 补充矩阵（审计要求未单列，但为矩阵完整性如实标注）

| # | 项 | 状态 | 依据 |
|---|---|---|---|
| `V-3.1` | 设备真实发出 ≥1 条 `/log` 信封 | ✅ `PASS` | MQTT 只读抓包：`{device_id,boot_seq,seq,ts,level,payload_base64}` 逐字段可见（`boot_seq 98–101` · `seq 108xxx–110xxx`） |
| `V-3.3` | D1 落库 | ✅ `PASS`（原 `BLOCKED`） | `log_history` `0 → 13` · `payload` 原样 · 幂等键唯一 · `created_at` 由 D1 生成 |
| `V-3.5` | 鉴权不变（无 `X-Webhook-Key` ⇒ `401`） | ✅ `PASS` | 本地**真实 D1 层**套件 `history_ingest_real_d1.sh` **用例 #10 = `401`**（`2026-10-15` 实跑 `123 PASS / 0 FAIL`）；且 Worker 鉴权实现 `requireWebhookKey()` 本轮**零改动**（源码级确认） |

---

## §3 文档收口（本轮实际改动）

### 3.1 收口的正式文档

| # | 文档 | 收口内容 |
|---|---|---|
| 1 | `docs/review/Phase-C-C2-Log-Routing-Console-Task-Sheet.md` | 状态 `PRE-FLIGHT COMPLETE / AWAITING HUMAN DECISION` → **`CLOSED`**；`§4.1` 判据矩阵状态列回填最终值；`§4.6` `V-3.7` 判据状态回填；`§7.2` 人工裁决项登记解决；**新增 `§12` 收口记录**（最终矩阵 + 后续事项 + 人工事实归档） |
| 2 | `docs/review/Phase-C-C2-Log-History-End-to-End-Review.md` | 状态 `PRE-FLIGHT COMPLETE / BLOCKED` → **`CLOSED`**；`§7` 验证矩阵状态回填；`§12` 裁决表回填；`§13` Deliverables 收口 |
| 3 | `docs/review/Phase-C-C2-V36-Telemetry-Regression-Verification-Record.md` | **结论订正**（§3.2）：① `log_history 17→20 / 净增 +2` 的**非原子基线**问题；② 「测试期间未重启」**已推翻**；③ 「顺带复证 `V-3.8 PASS`」**已撤回**；④ tail 事件数 18 → **21**；⑤ `O-1` 归并至 `BT-1` |
| 4 | `HANDOFF.md` | `V-3`（`/log` 落库）条目 → **`CLOSED`**；删除「待人工审核 `HR-C2-*` / 跑 `V-3.1…V-3.9` 收口」等**已完成**表述；关联文档表更新 |
| 5 | `docs/README.md` | `Phase C` 阶段状态行 → **`Phase C-C2 = CLOSED`**；新增 4 份本轮验证/复核报告条目 + 最终收口报告条目 |
| 6 | `docs/issues/未修复的问题.md` | **`BT-1`** 条目**追加现网可观测症状**（原 `O-1`）—— **`O-1` 不再是独立问题** |
| 7 | **本报告**（新增） | `Phase-C-C2-Final-Closure-Report.md` |

> **未修改历史原始证据**：所有 D1 查询数字、MQTT 抓包、tail 事件、commit hash **逐字保留**；仅修订**结论 / 状态 / 说明**，或**追加** follow-up / closure 记录。

### 3.2 `V-3.6` 记录的 5 项结论订正（保留原始数据，仅改结论）

| # | 原文结论 | 订正 | 性质 |
|---|---|---|---|
| 1 | `log_history` `17 → 20`，**净增 `+2`** | **算术 / 基线订正**：基线两条查询**非原子**（`COUNT=17 @ 17:34`（max id 89）与 `MAX(id)=95 @ 17:36:36` 相隔 ≈3.5 min）⇒ 表格内数字**自相矛盾**。**窗口归属 `+2`（`id 101/107`）正确**，但**不得**写作「`20 − 17`」 | 表述 |
| 2 | 「测试期间**未观察到重启**」（`O-4`：`connected_at` 恒 `17:13:46`） | **已推翻**：设备**确实于 `17:50:16 UTC` 重启**（`connected_at` 更新 + `log_history` 出现 `boot_seq 102` 双证据）· 与代码语义逐秒吻合（末次 `config_set` `17:44:58` + 5 min ⇒ `config_save()` ⇒ `request_restart()` ⇒ 10 s 安全窗口 ⇒ 重启）。原文结论**仅在其观测窗口内为真** | `MAJOR` |
| 3 | `V-3.8`「顺带复证 `PASS`」（2 × presence `200`） | **已撤回**：窗口内 `device_event` **+0**；实测 **4 次**（非 2 次）presence `200` **全部来自非设备客户端** ⇒ `presence.js` 返回 `not_device_identity`，**属设计正确**。**仅凭 `HTTP 200` 判 `V-3.8` 是任务单明令禁止的做法**。`V-3.8` 的 `PASS` 依据 = 本报告 §2.1 的**独立正式验收** | `MAJOR` |
| 4 | tail「18 事件」 | **订正**：那是 tail **运行中**的中途快照；落盘为 **21 事件** | 表述 |
| 5 | 「★ 新问题 `O-1`」 | **归并**：`O-1` **不是新缺陷**，是既有 **`BT-1`**（云端从不回 `log_ack`）的**现网可观测症状** ⇒ **记录到 `BT-1` / `WP-5`**，不新开编号 | 归类 |

### 3.3 `V-3.9` 措辞统一

- 历史文字：`POST /api/webhook` → **实际路径：`POST /`**（`Cloudflare_Assets/guo-feeder-api/src/index.js` 的根路径桶）。
- **功能链路与既有 webhook 语义成立**；仅措辞统一，**未因路径名称差异重新测试**。

### 3.4 人工确认事实的正式归档（**文档收口动作，非新裁决**）

| 事实 | 归档位置 |
|---|---|
| EMQX 控制台**实际规则 A/B 已人工检查**，规则实现正确 | 本报告 §1.2 · 任务单 §12 |
| AI 的测试代码行为符合预期 | 任务单 §12 · 本报告 §2.1（`V-3.7a`） |
| **不再因 `403` 将规则 A/B 视为 `UNKNOWN`** | 本报告 §1.2 / §2.1（`V-3.7b` 状态由 `HUMAN_REQUIRED` → `PASS`） |
| `HR-C2-7a/7b` **实际采用 = `7a`（黑名单）**，与生产规则逐字一致 | 任务单 §2.2.4 / §12 · 本报告 §2.1 |

---

## §4 C. 未闭合项（按审计要求四分类）

### 4.1 必须人工裁决 = **0 项**

`HR-C2-1…HR-C2-8` 全部已解决（见 §4.2 表）；**不存在**必须由用户决定的验收政策选择。
（`V-3.2` 的口径修订属任务单上下文内**可自行完成**的最小文档修订，**未**改变设计目标 ⇒ **不构成 `STOP-2`**。）

### 4.2 仅需文档修订 = ✅ 本轮已全部完成

| ID | 原状态 | 处置 |
|---|---|---|
| `HR-C2-1` | ✅ 方案 B（`2026-10-15`） | 已裁 · 已实施 |
| `HR-C2-2` | 🚫 不适用 | 未选 A |
| `HR-C2-3` | ✅ 接受过渡期双写 | 已接受 · 过渡于 `01:20:22` 实测终止 |
| `HR-C2-4` | 🔴 解除规则 A/B 冻结约束 | ✅ **事实已满足**（规则已改并核实） ⇒ 收口 |
| `HR-C2-5` | 🔴 `log_predicate` 定义批准 | ✅ **事实已满足**（规则 A 已含 `log_predicate` 并实测命中） ⇒ 收口 |
| `HR-C2-6` | 🔴 施工顺序 + 作业授权 | ✅ **事实已满足**（A 先 B 后已执行） ⇒ 收口 |
| `HR-C2-7` | 🔴 `/log` 排除策略 = 策略 A | ✅ **事实已满足**（规则 B 按 topic 排除已生效，`V-3.4` = `0`） ⇒ 收口 |
| `HR-C2-7a`/`7b` | 🔴 候选形态选择 | ✅ **采用 `7a`（黑名单）**，人工已核实 ⇒ 收口（§3.4） |
| `HR-C2-8` | 🔴 `level` 契约处置 | ✅ **已修并部署**（`F-C2-8` 修复 · Worker `dd5ab824`）⇒ 收口 |

### 4.3 可延后到后续阶段 = **6 项**（**均不得描述为 `Phase C-C2` 未完成**）

| # | 事项 | 归属 | 说明 |
|---|---|---|---|
| 1 | **`BT-1` 云端 `log_ack`** | `BT-1` · `计划:WP-5` · 受 `G-4` 门禁 | 云端**从不回送** `log_ack` ⇒ 设备侧 `acked_seq` 不前进 ⇒ Flash 段永不回收。**现网症状 = `O-1`**（`/log` 帧重复发布 · `MINOR`）· 见 §4.4 |
| 2 | **`V-3.2` observability enhancement** | 云端 Worker（可选） | 生产 tail 不留响应体 + 成功路径无打点 ⇒ 若需**生产侧**直接观测四字段计数，须**为成功路径加打点并部署 Worker**（**独立授权**） |
| 3 | **`HD-2`**（`interval_ms` 映射 5/15 min）· **`HD-3`**（`ts=0` + `seq` 归零 ⇒ 幂等碰撞） | 固件 · `Phase C-C1` 余项 | 与 `Phase C-C2` 无耦合 |
| 4 | **`P-3`**（非有限值 ⇒ JSON `null` 被 Worker 记录级拒收 · 静默丢样） | 固件 / Worker 契约 | 独立登记 |
| 5 | **`BT-1` 修复时的负载评估** | `G-4` 裁决要求 | `BT-1` 现把日志上行**限流**到 ≈1 条 / 152 s；修好后上行速率**升两个数量级** ⇒ **必须与负载评估同批** |
| 6 | **`mqtt_messages` 历史噪音清理**（867 行 `/log`）+ **MQTT 密码轮换** | 运维 | **须另行审批**，不在本次授权内 |

### 4.4 `O-1` 的最终归类（**审计要求 §3 基线**）

> **`O-1` = `BT-1` 的现网可观测症状 · `MINOR`（成本 / 可观测性）· 不是新的独立缺陷。**

**根因闭合（四项独立互证）**：

| 证据 | 实测 | 与代码预测的关系 |
|---|---|---|
| MQTT 协议级字段 | 12 帧 · 全部 **`qos=1` · `dup=False`** | ⇒ **排除** MQTT 协议层重传（协议重传**必置 `DUP=1`**） |
| 同一记录发布次数 | `(boot_seq=102, seq 110347/110348)` 各 **6 次** | = 1 首发 + 5 重试 |
| 发布间隔阶梯 | `17 / 19 / 23·24 / 31 / 47 s` | = `LOG_ACK_TIMEOUT_MS=15000` + 退避 `2/4/8/16/32 s` ⇒ 预测 `17/19/23/31/47`，**逐项吻合** |
| D1 `rowid` 步长 | 恒 **6**（`log_history`）· 本地隔离亦复现（`id 1,3,4,5,6`，`id=2` 缺失） | = 1 插入 + 5 幂等忽略（被忽略的插入**消耗 `rowid` 却未建行**） |
| Worker `duplicate` 计数 | `+5` | 与 6 次投递一致 |

⇒ **结论**：设备侧**应用层重发**（`log_ack` 未实现的必然结果）；当前被幂等键**完全吸收**，**数据零损失**。
**记录位置**：`docs/issues/未修复的问题.md` → **`BT-1`** 条目（现网症状追加）；**不新开 `O-1`**。

### 4.5 真正阻塞 `Phase C-C2` Close = **0 项**

---

## §5 `STOP` 条件逐条核对（**无一命中**）

| 条件 | 核对结果 |
|---|---|
| **`STOP-1`** 发现真正影响设计目标的未解决功能缺陷 | ❌ 未发现 —— `/log` 全链贯通，`log_history` 稳定增长 |
| **`STOP-2`** 两个正式验收要求之间无法自行消解的冲突 | ❌ 未发现 —— `V-3.2` 的口径修订属任务单上下文内可完成的**证据口径**调整（§2.2） |
| **`STOP-3`** 必须改生产代码 / EMQX / Worker / 固件才能满足验收 | ❌ 未发现 —— 生产侧**零改动**即可满足 |
| **`STOP-4`** 无法确定某项能否标 `PASS`/`CLOSED` | ❌ 未发现 —— 逐项均有明确判定（§2） |
| **`STOP-5`** 工作树存在无法安全判断归属的意外修改 | ❌ 未发现 —— 逐文件归属已核对（**§6.2**） |

⇒ **无 `STOP`** ⇒ 按审计要求执行**文档收口 + 提交 + push + 正式关闭**。

---

## §6 D. 三仓代码 / 工作树

### 6.1 本阶段提交

| 仓 | 分支 | 本阶段提交 | 结论 |
|---|---|---|---|
| **固件** `PIO_Assets/Guo_Feeder_Project` | `wb`（同步远端 `main`） | 见 **§7.6 commit 记录** | ✅ 已提交并 push |
| **云端** `Cloudflare_Assets` | `main` | 见 **§7.6 commit 记录** | ✅ 已提交并 push |
| **消息** `EMQX_Assets` | `main` | **无本阶段变更** | ✅ **未制造空 commit**（工作树 clean） |

### 6.2 逐文件归属核对（**§8 检查项**）

| 文件 | 归属 | 判定 |
|---|---|---|
| `HANDOFF.md`（M）· `docs/README.md`（M）· `docs/review/Phase-C-C2-Log-History-End-to-End-Review.md`（M）· `docs/review/Phase-C-C2-Log-Routing-Console-Task-Sheet.md`（M） | `Phase C-C2` 文档（`REV-2`/`REV-3` + 本轮收口） | ✅ 属本阶段 |
| `docs/review/Phase-C-C2-V36-Telemetry-Regression-Verification-Record.md`（??）· `…-V36-Review-Follow-up-Read-Only-Investigation.md`（??）· `…-V37a-Local-Isolation-Verification-Report.md`（??）· `…-V38-Verification-Report.md`（??） | `Phase C-C2` 验证 / 复核记录 | ✅ 属本阶段 |
| `docs/review/Phase-C-C2-Final-Closure-Report.md`（新增）· `docs/issues/未修复的问题.md`（M · `BT-1` 追加） | 收口报告 / 缺陷登记 | ✅ 属本阶段 |
| `Cloudflare_Assets/guo-feeder-api/test/real_d1/v37a_log_reject_matrix.sh`（??） | **正式测试资产**（与既有 `history_ingest_real_d1.sh` 同层同约定） + `test/real_d1/README.md` 登记 | ✅ 应提交 |
| `Cloudflare_Assets/.wrangler/**` | 本地 persist（**已 gitignore**，`git check-ignore` 确认） | ✅ **未入库** |
| `PIO_Assets/.../.pio/**`（`hd1_run.py` · `v36/v37a/v38*` 产物） | `.pio/` **已 gitignore** 的临时目录 | ✅ **未入库** |
| `src/**`（三方生产代码） · `migrations/**` · EMQX 规则 / 连接器 / Action / ACL | **零改动** | ✅ 无生产代码变更 |

### 6.3 意外 / 未授权变更

**无**。未发现临时文件、未预期生成物、未授权代码修改或生产配置修改。
**受保护文档**（`AGENTS.md` · `docs/issues/未修复的问题.md` 的既有正文条目等历史正文）**未被删除或改写**（仅 `BT-1` 条目**追加**一段症状说明）。

---

## §7 收口动作与结果

### 7.1 不做额外生产测试（**审计要求 §7.2**）

严格未执行：未发 MQTT 命令 · 未踢设备 · 未发布测试 payload · 未 `INSERT`/`UPDATE`/`DELETE` D1 · 未修改 EMQX · 未部署 Worker · 未重启设备。

### 7.2 文档收口

已完成（清单见 **§3.1**）；**未修改历史原始证据**。

### 7.3 Commit 前检查（§8 检查项 1–7 全通过）

1. ✅ 三仓 diff 逐一检查（`git status --porcelain` + `git diff --stat`）
2. ✅ 新增文件逐一检查（6 个新文件，均属 `Phase C-C2`）
3. ✅ 无临时文件（`.wrangler/` · `.pio/` 均被 gitignore）
4. ✅ 修改均属本次 `Phase C-C2` 收口
5. ✅ 未修改受保护文档正文
6. ✅ 无生产代码变更
7. ✅ 无未授权配置变更

### 7.4 提交纪律

- 逐仓分别提交；**未 squash**、**未修改历史 commit**；`git add <显式文件>`（**未**用 `git add -A`/`.`）；提交前 `git diff --cached --name-only` 核对。
- Commit message 含反引号 ⇒ 采用 **写文件 + `git commit -F`**，提交后 `git log -1 --format=%B` 复核。

### 7.5 正式关闭

- **`Phase C-C2 = CLOSED`**（`2026-10-16`）。
- 最终验收矩阵已记录（§2）。
- 仍存在但**明确属后续工作**的事项已记录（§4.3）—— **不得**被描述为 `Phase C-C2` 未完成。

### 7.6 commit 记录（push 后回填）

| 仓 | commit | 说明 |
|---|---|---|
| 固件 `wb` | *（见当日工作日志 `2026-10-16.md`）* | `docs(review): close Phase C-C2 log routing`（文档收口 + 4 份验证记录 + 收口报告 + `BT-1` 症状追加） |
| 云端 `main` | *（见当日工作日志）* | `test(history-ingest): add V-3.7a local isolation matrix`（夹具 + README 登记） |
| 消息 `main` | — | 无变更 |

> ⚠️ commit hash / push 结果以 **§8 执行记录**（本轮实际执行的填充）与当日工作日志为准。

---

## §8 边界声明

- **本报告为收口审计**，不含代码修改、不含生产配置变更、不含数据库写操作。
- **唯一可能的写操作**：本阶段仅涉及**文档**与**测试夹具**；`D1` 全程仅 `SELECT`。
- **未扩大验证范围**：已有充分证据的项目（`V-3.6` / `V-3.7a` / `V-3.8`）**未重复测试**。
- **未推翻既有裁决**：`V-3.6` / `V-3.4` / `V-3.7a` / `V-3.8` / `V-3.9` / `O-1` 归类均按审计基线维持；未发现与其**直接矛盾**的代码或原始证据。
- **`G-1 HUMAN_REQUIRED` · `G-4 BLOCKED` 未变**（属 P3 计划门禁，与 `Phase C-C2` 收口无耦合）。

---

*本报告于 **`2026-10-16`** 建立，为 **`Phase C-C2`（`/log` → `log_history`）** 的**最终收口审计报告**（`FINAL AUDIT`）。*
*基线 = 固件仓 `wb` **`1ebae81`**（收口前）· 云端仓 `main` **`e328a11`**（收口前）· 消息仓 `main` **`3165aab`**（无变更）。*
*结论：**`Final Audit = PASS`** ⇒ **`Phase C-C2 = CLOSED`**（`V-3.1…V-3.9` 全 `PASS`）。*
*后续工作（**非** `Phase C-C2` 未完成项）：`BT-1` / `WP-5` `log_ack` · `V-3.2` observability enhancement · `HD-2` / `HD-3` / `P-3` · 历史噪音清理与 MQTT 密码轮换。*
