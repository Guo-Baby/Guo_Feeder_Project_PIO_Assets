# 批次 2 实施指令：`C-4` 冲突证据补偿

> 交付对象：本地 AI Agent（单批可独立执行）
> 依据：`HD-3-Risk-Closure-Plan.md`（`rev.2`）§3
> 前置：`log_conflict` 已应用（`0004`）、线上 0 行；`log-ack.js` 现有"先 ACK 后落库"顺序
> **本指令不隐含任何生产授权**

---

## 1. 目标

在 **ACK 之后**的冲突落库失败路径上，把"完全静默"降级为"大概留有可检索证据"：

1. 失败后**在同一请求内重试 1 次**（仅针对可重试错误）；
2. 仍失败则输出**单行结构化** `console.error`，字段可 grep、可关联；
3. **不改** ACK 判定/时机、响应体、MQTT envelope、`duplicate` 口径、三态分类契约。

> 语义边界（必须写进注释与报告）：**只能降低证据丢失概率，不能保证绝不丢失**。

---

## 2. 实施范围

| 类型 | 路径 | 说明 |
|---|---|---|
| 修改 | `src/log-conflict.js` | `persistConflictEvents()` 内加重试 + 结构化失败日志；导出可测的判定辅助 |
| 新增 | `test/current/log_conflict_retry.test.mjs` | 新套件（≥ 12 项） |
| 修改 | `test/run_all.mjs` | 注册一行 |
| 修改 | `src/log-ack.js` | **仅在必要时**把 summary 计数接上（如新增 `conflict_retried`）；**不得**改 ACK 流程 |

**不得改动**：`log-ack.js` 的 `duplicate++` 位置、ACK 发布顺序、`history-ingest.js` 的响应体、`0004` migration、固件。

---

## 3. 要求

### 3.1 重试策略（精确）

```
第 1 次 batch 失败：
  ├─ reason 命中 NOT_RETRYABLE（no such table / 权限 / SQL 语法）⇒ 不重试，走 3.3
  └─ 其它（超时 / D1_ERROR / 5xx / 网络）⇒ 重试 1 次（同参数、同语句集合）
第 2 次仍失败 ⇒ 走 3.3
```

- **至多 1 次**重试，禁止循环/指数退避（避免拖长 ingest 时延）；
- 重试**不得**重复累计 `written`（第一次失败时 `written` 必须为 0；只有成功那次计数）；
- 重试必须复用**同一批**已聚合的唯一键集合（不得重新读输入，避免与首次不一致）；
- 表缺失熔断 `s_table_missing` 逻辑保留：命中后本 isolate 不再尝试，**也不重试**。

### 3.2 新计数（加性，只进日志/返回值）

`persistConflictEvents()` 返回值新增：

| 字段 | 含义 |
|---|---|
| `attempts` | 实际尝试次数（1 或 2） |
| `retried` | 是否发生重试（bool） |
| `retry_reason` | 首次失败原因（截断 ≤120 字） |

`log-ack.js` 的 `[log-ack] summary` 行追加 `conflict_retried=<0|1>`（**只追加，不删既有字段**）。

### 3.3 结构化失败日志（**只打一行**）

```
[log-conflict] evidence-lost dev=<device_id> b=<boot_seq> s=<seq> kind=<collision|unverified> n=<events> attempts=<1|2> reason=<sanitized>
```

- 每个"设备 + 批次"最多一行，**不得**每个事件一行（防刷屏）；
- `reason` 必须**截断且去换行**（≤120 字），不得包含 payload / 令牌 / 完整 SQL；
- `n` = 本批受影响事件数；
- 与既有 `[log-conflict] persist failed (...)` 告警**共存**时，保证**总行数 ≤2**（一次汇总 + 一次性告警），并在注释说明分工。

### 3.4 不得误判

- 重试与告警**只对** `conflict_events`（即 `collision` / `content_unverified`）生效；
- `same_content`、普通 `duplicate`、`accepted` 路径**不得**进入本逻辑；
- 三态互斥与 `duplicate = same + collision + unverified` 的守恒**保持不变**。

---

## 4. 约束

- 不改变 ACK：`summary.duplicate++` 与 `publishToDevice()` 的相对顺序**一字不动**；
- 不改变 HTTP 响应体（仍恰四字段）；
- 不新增 D1 表、不改 `0004`；
- 不部署 Worker、不写生产 D1（本地 workerd 允许）；
- 重试不得阻塞超过 **1 次**额外 D1 往返；若单次已接近超时预算，可**跳过重试**直接告警（需在代码里注释判据）。

---

## 5. 验收标准

| # | 判据 |
|---|---|
| B1 | 新增套件 **≥12 PASS / 0 FAIL**：①成功不重试 ②首次失败→重试成功（`written>0`,`attempts=2`）③两次失败（`failed=n`,`attempts=2`）④不可重试错误（`attempts=1`）⑤表缺失不重试 ⑥重复请求（同键）只落一行/一次告警 ⑦非冲突路径**不触发**任何重试 ⑧`reason` 截断 ⑨`n` 计数正确 ⑩`retried` 字段 ⑪返回结构向后兼容 ⑫空输入零调用 |
| B2 | 既有 `log_ack_collision.test.mjs` **仍 61 PASS / 0 FAIL** |
| B3 | 既有 `log_conflict.test.mjs` **仍 54 PASS / 0 FAIL**（若因新字段需调整，**只允许新增断言**） |
| B4 | `node test/run_all.mjs` 全绿（套件数 = 原 19 + 1） |
| B5 | `bash test/real_d1/log_conflict_real_d1.sh` **仍 55 PASS / 0 FAIL**（本地） |
| B6 | 代码中可 grep 到结构化日志模板，且不在成功路径打印 |

---

## 6. 必交报告

1. 改动清单 + `+/-` 行数；
2. 重试决策树（贴代码）；
3. 五次测试的命令 / EXIT / PASS-FAIL；
4. **失败留痕**：任何一轮失败的原因与修复，不得删断言；
5. 结构化日志样例（脱敏后的真实输出一行）；
6. 明确声明：**证据仍可能丢失**（写清在哪种情形下必然丢）；
7. `Worker = NOT DEPLOYED`、`生产 D1 = NOT WRITTEN`。

---

## 7. 失败 / 停止条件

1. 重试导致 ACK 顺序变化或响应体变化 ⇒ **停**；
2. 无法区分"可重试"与"表缺失"⇒ **停**（宁可只告警，不得盲目重试）；
3. 既有 61 / 54 项断言数下降且无法用"新增适配"解决 ⇒ **停**；
4. 为通过测试而把 `collision` 降级成 `same` 或反之 ⇒ **停**（这是契约破坏）。

---

## 8. 允许的 Git 操作

- 只读：`status` / `diff` / `log` / `show`；
- 需要提交时**必须**逐文件 `git add`（禁止 `-A`/`.`），且先 `git diff --cached --name-only` 核对；
- **禁止**：`push`、`reset --hard`、`clean`、`stash`、切分支、`rebase`/`merge`/`cherry-pick`；未授权不得 `commit`。

---

## 9. 明确禁止事项

1. 修改 ACK 语义/时机、`publishToDevice` 调用位置；
2. 修改 HTTP 四字段响应体；
3. 把 `duplicate` 口径改成"含 collision"或其他；
4. 新增 `log_conflict` 之外的持久表；
5. 在成功路径打印 `evidence-lost`；
6. 部署 / 写生产 D1；
7. 声称"已实现无损审计"。
