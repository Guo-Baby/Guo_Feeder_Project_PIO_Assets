# Phase C-C2 · `V-3.6` Review Follow-up —— 只读复核报告（A–E）

> **性质**：**只读**工程复核（`READ-ONLY`）。本轮**未**修改代码 / 配置 / 数据库 / EMQX / Worker / Pages / 正式验收文档，**未**部署，**未**重启设备，**未** commit / push。
> **复核对象**：`docs/review/Phase-C-C2-V36-Telemetry-Regression-Verification-Record.md`（`V-3.6` 验证记录，未提交）
> **日期**：`2026-10-16`（本地 `02:20`–`03:05` GMT+8 / UTC `18:20`–`19:05`）
> **判定汇总**：`V-3.6` 结论 **可维持 `PASS`**；但原报告存在 **1 项事实性订正（MAJOR）**、**1 项同类订正（MAJOR）**、**若干表述/方法缺陷（MINOR）**。
> **一句话**：**主结论成立，副结论（"未重启"、「顺带复证 V-3.8」）不成立或不足；`O-1` 不是新缺陷，而是既有 `BT-1` 的可观测症状。**

---

## §0 结论摘要

| 编号 | 主题 | 严重度 | 结论状态 | 一句话 |
|---|---|---|---|---|
| **A-1** | 自动保存/重启机制 | — | **`CONFIRMED`** | 代码事实：`config_set` ⇒ 5 min 倒计时（**每次 set 重置**）⇒ 到点**先 save 再 request_restart** ⇒ 10 s 安全窗口 ⇒ `ESP.restart()` |
| **A-2** | 「测试期间未观察到重启」 | **`MAJOR`** | **`REFUTED`** | 设备**确实于 `17:50:16 UTC` 重启**（`connected_at` + `boot_seq` 101→102 双证据）。原报告结论仅在当时观测窗口内成立 |
| **A-3** | 测试结束时持久化状态 | — | **`CONFIRMED`**（间接但强） | 重启**只能在 save 成功后**被请求 ⇒ 落盘确已发生；且重启后 **30 min 零遥测** ⇒ 持久值有效 = `false` |
| **B-1** | `schema_version` 命中 = 0 的证明边界 | — | **`CONFIRMED`**（边界已厘清） | 可证「telemetry 信封未进 `mqtt_messages`」；**不可**证「路由正确」「未在上游丢弃」 |
| **B-2** | 5 条遥测逐条关联 | — | **`CONFIRMED`** | 关联键 = 幂等唯一键 `(device_id, type, ts, seq)`，**字段级逐一相符** |
| **B-3** | Round A 的 tail 覆盖 | **`MINOR`** | **`REFUTED`** | `tail_v36.jsonl` **零事件**（仅 `ETIMEDOUT`）⇒ **Round A 无路径级证据** |
| **B-4** | ingest 响应语义 / 是否留存 | **`MINOR`** | **`UNKNOWN`(响应体)** | 四字段语义已查清；但 **tail 不留响应体**、Worker 亦不打点 ⇒ `accepted/duplicate/rejected/filtered` **无实测值** |
| **B-5** | `V-3.6` 能否维持 `PASS` | — | **`CONFIRMED`** | **可维持**：Round B 三重证据自洽 + Round A 复现 + 负向对照 + `/log` 联合键反查 = 0 |
| **C-1** | `log_history` `17→20` 与「净增 +2」矛盾 | **`MAJOR`** | **`REFUTED`**(算术) | 基线两查询**非原子**（相隔 ≈3.5 min）⇒ 表格内数字自相矛盾。**窗口归属 +2 正确**，但不得写作「20 − 17」 |
| **C-2** | 三表行数 / `MAX(id)` 一致性 | **`MINOR`** | 部分 `REFUTED` | `telemetry_history` / `mqtt_messages` **全程自洽**；`log_history` 基线对**不自洽**（已解释） |
| **C-3** | `mqtt_messages` 增量逐条归因 | — | **`CONFIRMED`** | `id 3484–3507` = 6 条命令 × 4 帧，**逐条**含 `i` = `v36q01/s01/s02/c01/c02/f01` |
| **C-4** | tail 窗口是否覆盖 Round A | **`MINOR`** | **`REFUTED`** | tail 实际跨度 `17:43:10–17:46:20` ⇒ **仅覆盖 Round B** |
| **C-5a** | `V-3.4` 顺带复证 | — | **`CONFIRMED`**（现已补证） | 需**联合键**反查；`seq 110091/110092` 命中 = **0**（`boot_seq` 单键**不可用**：17 条同 boot 历史噪音） |
| **C-5b** | `V-3.8` 顺带复证 | **`MAJOR`** | **`REFUTED`** | 窗口内 `device_event` **零新增**；4 次 `presence 200` 系**非设备客户端**（`presence.js` 设计使然）⇒ **原"PASS"引用无效** |
| **C-5c** | `V-3.9` 顺带复证 | **`MINOR`** | **`CONFIRMED`**（措辞需改） | 24 帧落库 + `direction` 正确；但路径是 `POST /`（**非** `/api/webhook`） |
| **C-6** | 术语混用 | **`MINOR`** | **`CONFIRMED`** | 「净增 / 窗口归属 / D1 新增 / 幂等忽略」在原报告中被混用 |
| **D-1** | `O-1` 根因 | — | **`CONFIRMED`**（**非新缺陷**） | = 设备侧**应用层重传阶梯**，根因 **`BT-1`**（云端不回 `log_ack`）。**量化吻合**：退避 `2/4/8/16/32 s` + 超时 `15 s` |
| **D-2** | 影响评估 | — | **`CONFIRMED`** | 每记录 **6 次**尝试（1 + 5 重试）；`EMQX/Worker/D1 写尝试 ×6`，**最终新增 1 行** |
| **E-1** | `V-3.7` 安全注入可行性 | **`MAJOR`** | **`BLOCKED`**（部分可行） | **本地隔离回放可行**；**生产 `/log` 直发不可行**（污染 `log_history`）。建议**拆分 `V-3.7a/b`**，**不建议**整体 `DEFERRED` |

---

## §1 阶段 1 · 只读证据盘点

### 1.1 仓库与工作树（复核时点：`2026-10-16 02:20 GMT+8`）

| 仓 | 分支 | HEAD | 工作树 |
|---|---|---|---|
| 固件 | `wb` | `1ebae81` | `M HANDOFF.md` · `M docs/README.md` · `M docs/review/Phase-C-C2-Log-History-End-to-End-Review.md` · `M docs/review/Phase-C-C2-Log-Routing-Console-Task-Sheet.md` · `?? docs/review/Phase-C-C2-V36-Telemetry-Regression-Verification-Record.md` |
| 云端 | `main` | `e328a11` | clean |
| 消息 | `main` | `3165aab` | clean |

**与上一轮记录一致 —— 无新增漂移。本轮未做任何写操作。**

### 1.2 原始产物清单（全部存在）

| 文件 | 大小 | 有效性 |
|---|---|---|
| `.pio/plan_v36.txt` / `plan_v36b.txt` / `plan_v36f.txt` | 642 / 336 / 227 B | ✅ 完整 |
| `.pio/v36_mqtt.log`（Round A 抓包） | 6,124 B | ✅ 82 行 / 3 telemetry |
| `.pio/v36b_mqtt.log`（Round B 抓包） | 4,148 B | ✅ 56 行 / 2 telemetry |
| `.pio/v36_final.log`（终态静默窗） | 2,606 B | ✅ 32 行 / 0 telemetry |
| `.pio/tail_v36b.jsonl`（tail 正式产物） | 78,515 B · mtime `01:46:18` | ✅ **21 事件**（非报告所写的 18） |
| `.pio/tail_v36.jsonl`（tail 首次尝试） | 1,056 B | ⚠️ **仅 `ETIMEDOUT` 堆栈，零事件** |

### 1.3 关键实现定位（全部为**代码事实**）

| 主题 | 文件 : 行 |
|---|---|
| 重启倒计时起点 / 事务标志 | `src/services/config_manager.cpp:96–111` |
| `restart_timer_start()`（**唯一调用点 = `set_begin`**） | `config_manager.cpp:972–981`（调用点 `:1661`） |
| `pending_save_finalize()`（Release + Request Restart） | `config_manager.cpp:995–1018` |
| `restart_timer_check()`（超时 ⇒ **先 save 再** request_restart） | `config_manager.cpp:1021–1063`（由 `config_task()` `:1383` 调用） |
| `set_begin()`（首事务 Acquire 一次 + 每次刷新计时器 + 置 dirty） | `config_manager.cpp:1591–1668` |
| `config_save()`（dirty 才写；成功才 finalize） | `config_manager.cpp:1268–1360` |
| 超时常量 `CONFIG_RESTART_TIMEOUT_MS = 5UL*60*1000` | `src/services/config_manager.h:125` |
| 安全窗口 `SYSTEM_RESTART_SAFE_DELAY_MS = 10000UL` | `src/services/system_command.h:70` |
| 唯一 `ESP.restart()` | `src/services/system_command.cpp:343` |
| 日志 ACK 重试常量 `LOG_ACK_TIMEOUT_MS=15000` / `LOG_ACK_MAX_RETRY=5` | `src/log/log_events.h:529–530` |
| 退避序列 `base<<(retry-1)`，base `2000` ms | `src/log/log_ack.h:421–443` |
| 退避重发 + 放弃（`GIVE_UP_NOT_ADVANCE`） | `src/log/log_manager.cpp:2853–2899` |
| ingest 响应构造 `{accepted, duplicate, rejected, filtered}` | `Cloudflare_Assets/guo-feeder-api/src/history-ingest.js:288–296` |
| ingest 请求体打点（**仅根路径**） `[ingest] body:` | `src/index.js:389` |
| `presence` 非设备身份早退 | `src/presence.js:273–279` |

---

## §2 主题 A · 配置自动保存与重启时序

### A-1 `CONFIRMED` · 机制（代码事实 · 置信度 高 · 无需授权）

**真实调用链**：

```
命令通道 config_set
 └─ set_begin(module, schedule_restart = true)              config_manager.cpp:1591
     ├─ if (!s_pending_save)  system_command_critical_operation_acquire()
     │      ⇒ s_pending_save = true            ← **每个事务只 Acquire 一次**（连续 set 不累加）
     ├─ restart_timer_start()  ⇒ s_restart_since_ms = millis()   :1661 / :974
     │      ⇒ **每次 command-channel set 都重置 5 min 倒计时**（注释明写"从最后一次修改起算"）
     └─ m->dirty = true
main loop → config_task()                                    :1377
 └─ restart_timer_check()                                    :1021
     └─ if (millis() - s_restart_since_ms >= 300000)
         ├─ config_save()          ← **重启前必须先落盘**
         │    ├─ 无 dirty ⇒ 幂等成功 + pending_save_finalize()（空事务收尾）
         │    └─ 有 dirty ⇒ 原子提交（Data+Version+commit）成功才 pending_save_finalize()
         └─ pending_save_finalize()                          :995
              ├─ system_command_critical_operation_release()
              └─ system_command_request_restart()   ← **幂等；不重置倒计时**
SystemCommand 状态机
 └─ RESTART_REQUESTED → 等 critical_count 归零 → RESTART_PENDING
     → 倒计时 SYSTEM_RESTART_SAFE_DELAY_MS (10 s) → ESP.restart()   system_command.cpp:302–343
```

**结论（逐条回答 A-1 要求）**：

| 问 | 答 | 证据 |
|---|---|---|
| `config_set` 是否置 dirty | ✅ 是（`m->dirty = true`） | `:1664` |
| 是否安排延迟保存/重启 | ✅ 是（5 min 业务倒计时；**到点必 save + 必重启**） | `:1031–1059` |
| 重复设置是否重置计时器 | ✅ 是（每次 `set_begin` 都 `restart_timer_start()`） | `:1661` |
| 是否有"取消任务" | ⚠️ **仅 `CONFIG_CMD_RESTART` 命令带 `cancel` 字段**（`ConfigCommandTask.cancel`，`:199`）—— **与本次测试无关**；**无任何 API 可取消 5 min 倒计时** | `:199` |
| save 失败时 | ❌ 不 Release、不 request_restart，**重新武装完整 5 min 倒计时** | `:1041–1052` |

> **★ 关键推论**：`telemetry_enable` 的**运行期**语义（每轮现读 RAM）与**持久化**语义（5 min 后必落盘 + 必重启）是**两条独立路径**。原报告只描述了前者。

### A-2 `MAJOR` · `REFUTED` · 「测试期间未观察到重启」

**观察事实（新增证据，全部只读取得）**：

| 证据 | 值 | 来源 |
|---|---|---|
| 设备 `connected_at`（复核前，`02:19 GMT+8` 查询） | **`2026-10-15T17:13:46.481Z`**（旧会话） | EMQX `GET /clients` |
| 设备 `connected_at`（复核时，`02:2x GMT+8`） | **`2026-10-15T17:50:16.585Z`** | EMQX `GET /clients` |
| `log_history` 出现 **`boot_seq = 102`** | `min_id 128`，`min_ct = 1792086617`（=**17:50:17 UTC**） | D1 `SELECT` |
| `device_event` `online`/`offline` | `id 215 / 216`，`created_at = 1792086616`（=**17:50:16**） | D1 `SELECT` |

**时间线闭合**：

```
17:44:58  最后一次 command-channel set（i=v36c02, telemetry_enable=false）  ← 计时器起点
17:49:58  ≈ +300 s ⇒ restart_timer_check() 到期 ⇒ config_save()（落盘）
17:50:0x  pending_save_finalize() ⇒ Release + request_restart()
17:50:0x  SystemCommand: critical_count 归零 → RESTART_PENDING → +10 s 安全窗口
17:50:16  ESP.restart() ⇒ 设备断线重连 ⇒ connected_at = 17:50:16.585
17:50:17  首条 boot_seq = 102 的 /log 落 D1
```

**⇒ 结论**：**设备确实重启过**，且时刻与代码语义**逐秒吻合**（5 min + save + 10 s 窗口）。
**⇒ 原报告 "`connected_at` 全程 `17:13:46`（**未重启**）" 作为**永久性事实陈述已不成立**。
**证据边界**：该陈述在**当时**的观测窗口（`17:47`–`17:50`，即 `17:50:16` **之前**）内**为真**；缺陷在于把它写成了**不受时间限制的结论**，且**未预告** 5 min 倒计时这一**已由代码确定**的必然事件。
**影响**：`V-3.6` 判据**不涉及**"是否重启" ⇒ **不影响主结论**；但**影响**报告可信度与"安全态"叙述。
**推荐下一步**：原文改为 ——「观测窗口内（`17:38–17:48`）未发生重启；但按代码语义，`17:44:58` 的 `config_set` 必然在 `17:49:58` 触发 `save` + 重启。**实测已于 `17:50:16` 发生**（`boot_seq` 101→102）」。
**置信度**：**高** · **需人工授权**：否（仅为文档订正）

### A-3 `CONFIRMED`（间接但强）· 测试结束时**持久化**状态

**要求区分四种状态**：

| 状态 | 本次测试取值 | 依据 |
|---|---|---|
| ① **RAM 运行时配置** | `false`（`17:40:42` / `17:44:58` 两次 `set false`；`config_get_telemetry_enable()` = `module_ro(MQTT)["telemetry_enable"] \| false`，读 RAM） | `config_manager.cpp:3763–3766` |
| ② **已持久化配置文件** | **`false`（有效值）** —— 由 ① + ⑤ 推得 | 见下 |
| ③ **Active / Backup 代际** | 未知（未读 `version.json`；**不需读**） | — |
| ④ **重启后实际生效** | **`false`** | 见下 |

**为何 ②/④ 可判 `false`（无需重启设备、无需触发保存）**：

1. **重启只可能在 `config_save()` 成功后被请求**：`pending_save_finalize()`（`:995`）是**唯一**调用 `system_command_request_restart()` 的路径，而它只在 commit 全成功（`:1354`）或 `config_save()` 返回 `true`（`:1059`）时被调用。**观测到重启 ⇒ 落盘必然已成功发生**，且落盘内容 = 当时的 RAM 值 = `false`。
2. **重启后的实测行为**：`telemetry_history` 最后一行 = `id 40`（`created_at 1792086251` = `17:44:11 UTC`）。**自 `17:50:16` 重启至复核时点（`18:2x`，≈30 min）新增 = 0 行**。若持久值为 `true`，则重启后**必然**产出遥测（HD-1 已实测开启后**同秒**发首批）⇒ **四条通道全静默 ⇒ 运行期门为 `false`**。
3. **无驱动因素**：`17:50` 之后**无任何** `config_set`（`mqtt_messages` 新增行中 `id > 3507` 仅 1 行，且为设备自发的 `{"cmd":"system","id":"online","src":"device"}`，非配置命令）。

**残余不确定性**（如实登记）：**键"不存在"还是"显式 `false`"**无法从现有产物区分（两者经 `| false` 均为 `false`）。另 `HD-1 V-C1-19` 曾留下**字符串 `"yes"`**；本次两次 `set false` 已覆盖。**二者对有效值均无影响**。
**⇒ 未主动重启设备、未主动触发保存（符合任务约束 3）。**
**置信度**：**高** · **需人工授权**：否

---

## §3 主题 B · 遥测路由与 D1 隔离证据链

### B-1 `CONFIRMED` · `schema_version` 命中 = 0 的**证明边界**

**实测**：`SELECT COUNT(*) FROM mqtt_messages WHERE raw_payload LIKE '%schema_version%'` = **`0`**（当前仍为 0）。

| 能证明 | 不能证明 |
|---|---|
| ✅ **没有任何 telemetry 信封进入 `mqtt_messages`** —— telemetry 信封**唯一携带** `schema_version`，而 `/log` 信封携带 `payload_base64`、命令帧携带 `c` ⇒ 该串是 telemetry 的**判别指纹** | ❌ 不能证明 telemetry **被正确路由**（这是 tail + D1 的活） |
| ✅ 负向对照成立：若规则 B 误吞 telemetry，`raw_payload` 必含该串 ⇒ 计数器可**证伪** | ❌ 不能证明**上游未丢弃**（EMQX 规则若不命中，消息既不进 `mqtt_messages` 也不进 `log_history`，**完全静默**） |
| ✅ 该匹配针对 `raw_payload`（webhook 路径**逐字**存原始 JSON）⇒ 文本匹配有效 | ❌ 不覆盖 `/api/ingest`（P1 入口）落库的行——但本轮无该路径流量 |
| | ❌ 与"路由到 `/api/history/ingest`"**无逻辑等价关系**；须独立取证 |

**证据边界**：该判据是**必要不充分**的**隔离侧**证据。原报告把它与 tail/D1 并列使用 **方式正确**，但应当明写其"只否证、不正证"的性质。
**置信度**：高 · **需人工授权**：否

### B-2 `CONFIRMED` · 5 条遥测**逐条**关联

关联键 = **幂等唯一键本身** `(device_id, type, ts, seq)`。**逐条对照**：

| 源 | 抓包（`.pio/v36*_mqtt.log`） | D1 `telemetry_history` | created_at − ts |
|---|---|---|---|
| R-A ① | `{ts:1792085942, climate.temperature, 24.8, seq 1, sv 1}` | `id 36`：同 `ts/type/value/seq/sv` | 3 s |
| R-A ② | `{ts:1792085942, climate.humidity, 55.5, seq 2}` | `id 37`：同上 | 4 s |
| R-A ③ | `{ts:1792085942, weight.value, 0, seq 3}` | `id 38`：同上 | 4 s |
| R-B ① | `{ts:1792086246, climate.temperature, 24.8, seq 4}` | `id 39`：同上 | 4 s |
| R-B ② | `{ts:1792086246, climate.humidity, 55.5, seq 5}` | `id 40`：同上 | 5 s |

**边界**：`seq` 是 **boot lifetime 计数**，本轮两轮均处 `boot_seq = 101` 内且 `seq` 单调，**无跨 boot 歧义**；`(ts, type)` 亦互不相同 ⇒ **关联唯一**。
**结论**：**5 条全部 1:1 关联成功**，且 `telemetry_history` **恰为 +5**（无多余、无缺失）。**置信度 高**。

### B-3 `MINOR` · `REFUTED` · **Round A 无路径级证据**

`.pio/tail_v36.jsonl` = **1,056 B**，内容为 `AggregateError [ETIMEDOUT]`（IPv6 `/ IPv4` 直连均超时）⇒ **零事件**。
⇒ **Round A（`17:38:38–17:41:22`）没有 `wrangler tail` 覆盖**；其 tail 尝试在 Round A **之前**即失败。

**影响**：Round A 的证据 = **MQTT 抓包 + D1**（二重）；Round B = **三重**。原报告 §3.2 虽把 tail 窗口标为 `17:43:10–17:45:42`，但**未显式声明"Round A 无 tail"**，且 §4 判据表把"路由路径"整体判 `PASS`，读者会误以为两轮都有路径证据。
**推荐**：在判据表按轮拆分，明写 `R-A: 无路径证据（tail 失败，仅抓包 + D1）`。**置信度 高**。

### B-4 `MINOR` · 响应语义已查清 · **实测值 `UNKNOWN`**

**`POST /api/history/ingest` 响应语义（代码事实 · `history-ingest.js`）**：

| 字段 | 语义 | 代码位置 |
|---|---|---|
| `accepted` | **实际写入行数**（`persist()` 返回的 D1 changes） | `:292` |
| `duplicate` | `rows.length - inserted` —— **通过校验但被唯一索引幂等忽略** | `:293` |
| `rejected` | **记录级校验失败**条数（`normalize*` 返回 `null`）：非法 `level`（非整数 / 越界 / 类型错）、`device_id` 不一致等 | `:258` |
| `filtered` | **仅 log 域**：`level = 0`（DEBUG）**策略过滤**，不落库、**不计入 `rejected`**；**telemetry 域恒为 0** | `:262` |

**HTTP 状态语义**：

| 码 | 含义 |
|---|---|
| `200` | 批次已处理 —— **即使全部 `rejected` 也是 200**（记录级失败不影响 HTTP 状态） |
| `400` | `unparsable_payload` / `device_id_mismatch` / `invalid_device_id`（**批次级**校验失败） |
| `401` | 缺 / 错 `X-Webhook-Key` |
| `503` | `persist_failed`（暂时失败 ⇒ 上游重投 ⇒ 幂等吸收） |

**★ 关键缺口**：**现有产物未留存 `accepted/duplicate/rejected/filtered` 的值**。
- `wrangler tail --format json` 记录 **request/response 元数据**（URL / method / status / headers / 耗时），**不记录响应体**；
- 且 `history-ingest.js` **只有 2 处 `console.error`（失败路径）**，**无成功路径打点**（对比：`index.js:389` 对**根路径**打了 `[ingest] body:`）；
- **实测核对**：`tail_v36b.jsonl` 21 事件中 **8 条含 `logs`，全部为 `POST /`（根路径）的请求体**，`/api/history/ingest` 的 9 条**全部 `logs = 0`**。

**实际取到的替代证据（Round B）**：

| 证据 | 值 |
|---|---|
| 2 次 `POST /api/history/ingest` @ `17:44:10`，`content-length` = **166 / 163** | 与「EMQX 包壳 + telemetry 信封」体量吻合 |
| 7 次 `POST /api/history/ingest`，`content-length` = **387** | 与「包壳 + `/log` 信封（含 `payload_base64`）」吻合 |
| D1 增量 | `telemetry_history` +2（`id 39/40`）、`log_history` +1（`id 113`） |

**⇒ 边界声明**：`accepted = 1`、`duplicate = 5`（log 域）等**计数未能实测**；本轮**只能**由「D1 行数和 `id` 步长」**反推**（见 D-2）。
**对 `V-3.6` 的影响**：`V-3.6` 判据为「落 `telemetry_history` + 不落 `mqtt_messages`」，**手段 = D1 / tail** ⇒ **不要求**响应体计数 ⇒ **结论不受影响**。但 **`V-3.2` 的"逐条记录 `rejected`"要求仍未满足**，须单列。
**置信度 高** · **需人工授权**：如需实测计数 ⇒ 需"加一行 `console.log` + 部署"（**本轮禁止**）或改用本地回放（见 E）。

### B-5 `CONFIRMED` · **`V-3.6` `PASS` 可维持**（充分证据）

| 支撑 | 内容 |
|---|---|
| **三重自洽（Round B）** | 抓包 2 帧 `/up` telemetry（`seq 4,5`）↔ tail 2 次 `/api/history/ingest 200`（`len 166/163`，**同一秒**）↔ D1 2 行（`id 39/40`，键一致） |
| **独立复现（Round A）** | 抓包 3 帧 ↔ D1 3 行（`id 36/37/38`），**无 tail** 但其余自洽 |
| **负向对照** | `schema_version` 在 `mqtt_messages` 命中 = **0**（持续成立） |
| **`/log` 联合键反查 = 0** | `seq 110091 / 110092` 在 `mqtt_messages` 命中 **0**（见 C-5a） |
| **命令帧与遥测不混同** | 24 帧命令回包 100% 归因（见 C-3）；`/up` 上"遥测 vs 命令回包"经 `c` 字段区分 |

**⇒ 维持 `PASS`**（不降级、不升级）。**新增限定条款 3 条**：① Round A 无路径证据；② 响应体计数未实测；③ 原报告 A-2/C-1/C-5b 三处表述须订正。

---

## §4 主题 C · 计数与时间窗口一致性

### C-1 `MAJOR` · `REFUTED` · `log_history 17 → 20 / 净增 +2` 的算术矛盾

**原报告原文**：`| log_history | 17 → 20 | +2（净） |`

**根因（已重构，置信度 高）**：**基线的两条查询不是原子快照**，且**设备独立于测试持续写 `/log`**。

- 基线 ①②（`COUNT(*)` 与 `MAX(id)`）为**两条并行 `wrangler d1 execute`**，各自"启动 → 鉴权 → 查询"耗时不同（`wrangler` 冷启动可达分钟级）。
- **反推吻合**：`log_history` 在 `boot_seq=101` 段的 `id` 步长恒为 **6**（见 D-2），两记录间隔 ≈ 152 s ⇒
  - `MAX(id)=95` 的查询落在 `id 95`（`created_at 1792085796` = **17:36:36**）之后；
  - `COUNT=17` 若落在 `id 89`（**17:34:03**）之后、`id 95` 之前，则行集 = `{3,5,6,12,18,24,30,36,41,47,53,59,65,71,77,83,89}` = **恰 17 行** ✅
- ⇒ **两条查询实际相隔 ≈ 2.5–3.5 min**，`17` 与 `95` **各自正确，但不同时点**。

**正确表述**：

| 口径 | 值 |
|---|---|
| 基线（非原子） | `COUNT = 17`（≈`17:34`）· `MAX(id) = 95`（≈`17:36:36`） |
| **测试窗口归属（正确定义）** | `id > 95` 且落在测试会话内 ⇒ **`id 101`、`id 107`** ⇒ **+2** |
| **表净增长** | `20 − 17 = 3` —— **不可**归因于本次测试（第 3 行 = `id 95`，出现在基线两查询之间） |
| 报告表格中的数字 | **`17 → 20 / +2` 自相矛盾**，且与"用 `id>95` 过滤"的口径不一致 |

**影响**：`log_history` **不是** `V-3.6` 判据 ⇒ 不影响 `PASS`；但**报告内部不自洽**。
**推荐**：① 订正为「基线行集以 `MAX(id)=95` 为**边界标记**；窗口归属 = `id 101/107` = **+2**；表净增长 ≠ 窗口归属」；② 后续基线 **必须单条 SQL 同时取 `COUNT(*)` 与 `MAX(id)`**，并**记录 `SELECT` 时刻**。
**置信度 高** · **需人工授权**：否

### C-2 `MINOR` · 三表行数 / `MAX(id)` 一致性

| 表 | 基线 | R-A 后 | R-B 后 | 终态（17:52） | 复核时点 | 自洽？ |
|---|---|---|---|---|---|---|
| `telemetry_history` | 15 / 35 | 18 / 38 | 20 / 40 | 20 / 40 | **20 / 40**（未再增长） | ✅ **完全自洽**（`COUNT` 与 `MAX(id)` 同步 +3 / +2；`id` 连续） |
| `log_history` | **17 / 95** | — | — | 20 / 107 | 33 / 182 | ❌ **基线对不自洽**（见 C-1；已解释） |
| `mqtt_messages` | 3465 / 3483 | 3477 / 3495 | 3485 / 3503 | 3489 / 3507 | **3490 / 3508** | ✅ **完全自洽**（`3483+12=3495`、`+8=3503`、`+4=3507`；`COUNT` 同步 `+12/+8/+4/+1`） |

**注**：`telemetry_history` 与 `mqtt_messages` 的**基线对自洽**，反证 C-1 的异常**只**来自 `log_history`（该表在测试期间被设备持续写入，另两表**没有**这种背景流量 ⇒ 不受非原子性影响）。
**置信度 高**。

### C-3 `CONFIRMED` · `mqtt_messages` 增量**逐条**归因

`id 3484 – 3507`（**24 行**）**逐条**列出，`i`（命令 id）全部可读：

| 轮 | 命令 | 行区间 | 构成 |
|---|---|---|---|
| R-A | `v36q01` · `v36s01` · `v36s02` | `3484–3495`（12） | 3 × (`/down` + `ack` + `result-queued` + `result-ok`) |
| R-B | `v36c01` · `v36c02` | `3496–3503`（8） | 2 × 4 |
| 终态 | `v36f01` | `3504–3507`（4） | 1 × 4 |
| **非本轮** | — | **`3508`** | **`{"cmd":"system","id":"online","src":"device"}`** —— 设备**重启后自发**的旧格式上行（`17:50` 之后） |

**⇒ 逐条归因 ✅**（非"总数相减"）；且**无任何 `/log` 行**（`topic LIKE '%/log'` 在 `id>3483` = **0**）。
**置信度 高** · 无需授权。

### C-4 `MINOR` · `REFUTED` · tail 窗口**未覆盖** Round A

**tail 正式产物实测**（`tail_v36b.jsonl`，21 事件，`17:43:10 → 17:46:20`）：

| 路径 | 次数 | 备注 |
|---|---|---|
| `/api/history/ingest` | **9** | 2 × telemetry（`len 166/163` @ `17:44:10`）+ 7 × `/log`（`len 387`） |
| `/`（根/webhook 桶） | **8** | **全部含 `[ingest] body:` 请求体** = Round B 的 8 帧命令 |
| `/api/presence/event` | **4** | `17:43:52`(146) · `17:45:25`(170) · `17:46:19`(146) · `17:46:20`(170) |

⇒ **Round B（`17:44:06–17:44:59`）有完整路径证据；Round A（`17:38:38–17:41:22`）完全没有**。
⇒ **附带订正**：原报告写「共 18 事件」「presence 2 次」——**该数字取自 tail 仍在运行时的中途快照**，与落盘产物（**21 事件 / presence 4 次**）不一致（`MINOR` 计量缺陷）。
**置信度 高**。

### C-5a `CONFIRMED` · `V-3.4` 顺带复证 —— **现已满足原始判据**

**原始判据**（任务单 §4.5）：**C-1** topic 增量 = 0 · **C-2** 按 `boot_seq`/`seq` **逐条关联** = 0 · **C-3** 其它 topic 不变。

| 判据 | 本轮补证结果 |
|---|---|
| **C-1** | `id > 3483` 中 `topic LIKE '%/log'` = **0** ✅ |
| **C-2** | **联合键反查**：`raw_payload LIKE '%110091%'` = **0** · `'%110092%'` = **0** ✅ |
| **C-3** | `id 3484–3507` 全部为 `/up`(19) 与 `/down`(6)，`direction` 正确 ✅ |

**★ 重要方法论修正**：**`boot_seq` 单键反查不可用** —— `raw_payload LIKE '%"boot_seq":101%'` 命中 **17 行**（`id 3467–3483`，**规则 B 修复前的历史噪音**，与本轮同属 `boot_seq=101`）。**必须**用 `boot_seq` + `seq` **联合键**才能消歧（任务单原文即要求联合键，原报告只做了 topic 过滤）。
**置信度 高**。

### C-5b `MAJOR` · `REFUTED` · `V-3.8` 顺带复证 —— **原"PASS"引用无效**

**原始判据**：presence 回归：`$events/client_connected` / `disconnected` 仍 **`200` 落 `device_event`**（tail + D1）。

| 半边 | 结果 |
|---|---|
| `200` | ✅ tail 实测 **4 次** `POST /api/presence/event` → `200` |
| **落 `device_event`** | ❌ **测试窗口（`17:38–17:47`）内 `device_event` 新增 = 0 行** |

**根因（代码事实）**：`presence.js:273–279` ——

```
设备身份从 (username, clientid) 派生；非设备身份 ⇒ 200 {ignored:"not_device_identity"}（不落库）
                                        未知设备 ⇒ 200 {ignored:"unknown_device"}（不落库）
                                        REVOKED   ⇒ 200 {ignored:"device_revoked"}（不落库）
```

⇒ 窗口内 4 次 presence 事件的来源是**非设备客户端**（本轮的 `hd1_run_*` / `workbuddy` 等 paho 会话），**返回 200 且不写 `device_event` 属设计正确行为**。
⇒ **"HTTP 200" 完全不能作为 `V-3.8` 的证据**（任务单原文已明令禁止）—— 这正是本轮要求的甄别点。
**旁证**：`device_event` 中**确**存在设备 presence 行：`id 215 online / 216 offline` @ `1792086616`（**17:50:16**，即本次自动重启）—— **但在 `V-3.6` 窗口之外**，属重启事件，**不可**用于 `V-3.6` 的顺带复证。
**处置建议**：把原报告"顺带复证 `V-3.8` `PASS`"**撤回**，改写为「**未在 `V-3.6` 窗口内复证**；`V-3.8` 的既有结论须以其**原验收证据**为准（或在**设备**真实上下线时另行取证）」。
**置信度 高** · **需人工授权**：**是**（涉及验收矩阵状态表述）

### C-5c `MINOR` · `V-3.9` 顺带复证 —— 实质满足，**措辞需改**

**原始判据**：`/down` 命令回包仍落 `mqtt_messages`；`POST /api/webhook` 对合法 JSON 行为不变。

| 半边 | 结果 |
|---|---|
| 命令回包落 `mqtt_messages` | ✅ **24 帧逐条归因**（C-3），`direction` = `down`/`up` 正确 |
| **路径 = `/api/webhook`** | ❌ 实测落点是 **`POST /`（根路径）**—— **当前拓扑**下 EMQX 规则 B 的动作 URL 指向根路径；`/api/webhook` **本轮无流量** |

**注**：`index.js` 文件头亦声明「`POST /` = 既有通路（跑注册/确认副作用）」，且与 `/api/ingest` 共享同一实现 ⇒ **语义等价，路径不同**。
**处置建议**：判据措辞改为「`POST /`（既有 webhook 通路）」，结论仍 `PASS`。**置信度 高**。

### C-6 `MINOR` · 术语混用核查

| 原报告表述 | 问题 |
|---|---|
| `log_history \| 17 → 20 \| +2（净）` | **净增**（表差值 3）与**窗口归属**（2）混为一谈 |
| 「`mqtt_messages` `+24`」 | 恰好"消息帧数 = D1 新增行数"，但**未声明**二者在这条链上**恰好相等**、而在 `/log` 链上**不相等**（6 次尝试 : 1 行）⇒ 易被误推广 |
| 「Round A 12 帧 = 3 telemetry + 9 命令」 | ✅ 表述正确，但**未**同时给出"其中 3 帧是本机 `/down` 主动发布" |
| 「遥测 +3 / 命令 +12」 | ✅ 正确，且已注明 "100% 归因" —— **本项为正面范例** |

**建议**：全项目统一四层口径 —— **① 线上消息数 ② 到达端点次数 ③ D1 写入尝试次数 ④ D1 最终净增行数**。

---

## §5 主题 D · `O-1` 同一 `/log` 帧重复发布（根因已闭合）

> 被测键：`(device_id=288485896ce4, boot_seq=101, seq=110091, ts=1792085920)`

### D-1 `CONFIRMED` · **根因 = 设备侧应用层重传，由既有 `BT-1` 驱动**

**追踪链（全部代码事实）**：

```
log 记录生成
 └─ 入队 → 组批 → MQTT publish（经 cloud_send_log）
      └─ 置 s_cloud_inflight = true, s_cloud_deadline_ms = now + LOG_ACK_TIMEOUT_MS(15000)
cloud_poll()                                        log_manager.cpp:2853+
 ├─ (1) cloud_handle_ack()   ← 本设备**从未收到** log_ack（Worker 明文声明"不含 log_ack，属计划 WP-5"）
 ├─ (2) 在途超时 ⇒ s_cloud_retry_n++
 │      ├─ log_ack_should_give_up(n)  (n > LOG_ACK_MAX_RETRY = 5) ⇒ cloud_give_up_batch()
 │      │      ⇒ **GIVE_UP_NOT_ADVANCE**：不推进 ack 水位 · 不删段 · 登记 hole（留待下次开机重放）
 │      └─ 否则 backoff = log_ack_backoff_ms_ex(n, 2000) = 2/4/8/16/32 s
 │             ⇒ s_cloud_backoff_until_ms = now + backoff
 │             ⇒ s_cloud_inflight = false ⇒ 退避结束后 **(3)(7) 重新发送同一批次**
 └─ (3) 退避中则 return
```

**★ 量化吻合（决定性）**：

| 项 | 计算值 | 实测值 | 吻合 |
|---|---|---|---|
| 发布间隔序列 | `15s + 退避(2,4,8,16,32)` = **17 / 19 / 23 / 31 / 47 s** | 抓包间隔 **17 / 19 / 23 / 31 / 47 s** | ✅ **逐项一致** |
| 同一记录发布总次数 | `1 + LOG_ACK_MAX_RETRY(5)` = **6** | `seq 110091` 抓包 **6 次** | ✅ |
| 阶梯总时长 | `15+17+19+23+31+47` = **152 s** | D1 相邻记录 `created_at` 差 **153 s**（`1792085947→1792086100`） | ✅ |
| D1 rowid 消耗 | 每次尝试 1 个 ⇒ **6 个/记录** | `boot_seq=101` 的 `id` = `41,47,53,…,125`，**步长恒为 6** | ✅ **（与抓包相互独立）** |

### D-2 `CONFIRMED` · 影响评估（按四层口径）

| 层 | 每记录 | 说明 |
|---|---|---|
| ① 设备上行消息 | **6 次 publish** | 每帧信封 ≈ 250–285 B ⇒ ≈1.6 KB/记录；约 0.1 bit/s 等效带宽 —— **带宽可忽略** |
| ② EMQX 规则 A 命中 + HTTP Action | **6 次** | 每次匹配都触发一次 Action 调用（对 Serverless **调用计数**有成本） |
| ③ Worker 请求 | **6 次** | 其中 **5 次 `duplicate`**（同一幂等键） |
| ④ D1：写入**尝试** / **净增行** | 尝试 **6** / 净增 **1** | 尝试 = `INSERT OR IGNORE` 6 次（消耗 5 个 rowid）；**净增恒 1** |

**★ 反直觉结论**：`BT-1` 当前把日志上行**限流**到 ≈**1 条 / 152 s**（远低于设计上限 `LOG_TX_MIN_INTERVAL_MS = 500 ms` ⇒ 2 Hz × 30 条）。**修好 `BT-1` 后上行速率将上升约两个数量级** ⇒ 与 `G-4` 的「`BT-1` 修复须与设备侧负载评估**同批**」要求**完全一致**（本条**支持**既有裁决，不构成新问题）。

### D-3 四类机制甄别（逐项裁定）

| 候选 | 裁定 | 依据 |
|---|---|---|
| **① 应用层再次调用 publish** | ✅ **成立** | `cloud_poll()` 超时分支显式重发在途批次；间隔 = 超时 + 指数退避；放弃阈值 = 5 次 |
| ② MQTT 协议层重传（DUP） | ❌ **不成立**（无证据） | 6 次发布的**间隔随退避指数增长**；协议层重传通常在重连时成簇、不呈指数阶梯 |
| ③ EMQX / 下游重复投递 | ❌ **不成立** | 若为 broker 重投，`recv_msg` 计数与间隔不会呈此阶梯；且发布间隔由**设备侧**时间轴驱动 |
| ④ Worker 收重复请求但 D1 幂等忽略 | ✅ **成立（但为"结果"，非"根因"）** | 5 次 `duplicate`；由 ① 造成 |

**证据缺口（如实登记）**：**抓包未记录 MQTT `DUP` 标志与 QoS** —— 现有 `hd1_run.py` 的 `on_message` 只记录 `topic` 与解码后的 payload。⇒ 若需**协议级**排除 ②，需补采 `msg.dup` / `msg.qos`。
**但**：① 的**定量吻合（4 项独立吻合，含 D1 rowid 步长这一与抓包无关的证据）**已足以**判定根因**；②/③ 属**未被证据支持**而非"被排除"。
**置信度**：**高**（根因）· **中**（对 ②/③ 的排除强度）

### D-4 `O-1` 的**定性订正**（重要）

| 维度 | 原报告 | 复核结论 |
|---|---|---|
| 是否**新**缺陷 | 以 "★ 新问题 O-1" 登记 | ❌ **不是新缺陷** —— 它是**既有 `BT-1`**（云端不回 `log_ack`，`WP-5` 已登记、`G-4` 已门禁）的**可观测症状** |
| 严重度建议 | 未定级 | **`MINOR`（可观测性/成本）** —— 无数据丢失（幂等吸收）、无错误数据；成本项 = 6× Action/请求/写尝试。**`BT-1` 本身**仍为高优先（原级别不变） |
| 应对 | "建议单开只读调查" | **无需新开调查**（本轮已闭合）；改为：① 在 `BT-1` 记录下**补一条"现网可观测症状"**；② 在 `WP-5` 落地时**复核"症状消失"**（`log_history` `id` 步长应回落到 1）—— 这是一个**极佳的低成本验收指标** |

**置信度 高** · **需人工授权**：是（`BT-1` / `WP-5` 文档状态变更）
**本轮未做**：未改重传逻辑、未调 QoS、未改 `log_ack` 协议、未做任何生产变更。

---

## §6 主题 E · `V-3.7` 畸形样本**安全注入**可行性

### E-1 判据回顾（任务单 §4.6）与**时效订正**

原矩阵（`M-1…M-7`）**写于 `F-C2-8` 修复之前**，其中 **`M-3` 的"预期"已被修复改变**：

| 类 | 形态 | 规则 A 命中？ | **原预期** | **现预期（`F-C2-8` 修复后）** |
|---|---|---|---|---|
| `M-1` | 缺 ≥1 必需字段 | ❌（`log_predicate` 需 `payload_base64/boot_seq/seq/ts/level` 全非空） | 静默 | **静默（不变）** |
| `M-2` | 字段齐、`level` **非法** | ✅ | `200 {accepted:0,rejected:1}` | **同左（现已可区分）** |
| `M-3` | **完全合法** | ✅ | `200 {accepted:0,rejected:1}`（旧缺陷） | **`200 {accepted:1,rejected:0}` 且 `log_history +1`** ← **预期已变** |
| `M-4` | 信封 `device_id` ≠ topic 内 | ✅ | `400 device_id_mismatch` | 同左（源码 `:222–229`） |
| `M-5` | topic 内 `device_id` 非法 | ✅ | `400 invalid_device_id` | 同左 |
| `M-6` | `/log` 上发**非 JSON** | ❌ | 静默 | 静默（不变） |
| `M-7` | 未知 topic（如 `/foo`） | — | 取决于黑白名单 | **现为黑名单**：`FROM "guo_feeder/#"` ⇒ 进 `mqtt_messages` |

> ⚠️ **两项前置未决仍未消**：① **规则 A/B 的 `FROM`/`WHERE` 逐字未核**（Serverless 数据集成 API **全 403**，规则只能在控制台看）⇒ **`LIVE CONFIGURATION: NOT VERIFIED`**；② `HR-C2-7a/7b`（`M-7` 黑白名单）未见归档裁决。

### E-2 注入方案评估（**四方案 × 风险**）

| 方案 | 做法 | 生产污染风险 | 能验证什么 | 裁定 |
|---|---|---|---|---|
| **(a) 直发生产 `/log`** | 以 `workbuddy`（ACL `guo_feeder/#` all-allow）向 `guo_feeder/288485896ce4/log` 发布构造报文 | 🔴 **高**：`M-2/M-3/M-4/M-5` 会经**规则 A** 打到 ingest；**`M-3` 会在 `288485896ce4` 名下写入 `log_history`**（污染该设备历史）；且与真实流量**不可区分** | 全链路 | ❌ **不推荐** |
| **(b) 测试 `device_id` 命名空间** | 向 `guo_feeder/<TEST_ID>/log` 发布 | 🟡 **中**：`isRevoked` 对**未登记设备**返回 `false`（`isDeviceRevoked(null) = false`，`device-registry.js:63–66`）⇒ **不早退**，`M-3` 仍会在 `log_history` 写入**一条假设备行**（表级污染、非生产设备污染） | 全链路（含真实规则） | ⚠️ **可选**，须**人工批准** + 事**后清理授权**（`DELETE` 需单独授权） |
| **(c) ★ 本地隔离回放** | `wrangler dev`（本地 Worker）+ **本地 D1**（Miniflare），直接 `POST /api/history/ingest` 投喂 EMQX 形态报文 | 🟢 **零** | **Worker 侧全部**（校验 / 三态 / 四字段计数 / 落库 / 幂等）；**可读响应体** | ✅ **推荐（主）** |
| **(d) 控制台规则"SQL 测试"** | 用 EMQX 控制台的**规则调试/SQL 试跑**功能对合成 payload 求值 | 🟢 **零**（不发布） | **规则侧**（`M-1/M-6` 的"是否命中"、`M-7` 归属） | ✅ **推荐（辅）**，须人工确认 Serverless 控制台具备该功能 |

**★ 决定性论据（支持 (c) 为主）**：`V-3.7` 判据 **`D-2`** 要求「**tail 状态码 + `rejected`/`error` 与矩阵逐条一致**」。而本轮已实证 **`wrangler tail` 不留响应体**、且 `history-ingest.js` **无成功路径打点** ⇒ **即便做了 (a)/(b) 的线上注入，也读不到 `rejected` 计数** —— 除非 (i) 临时加 `console.log` + 部署（**代码/部署变更**），或 (ii) **本地回放（可直接读 HTTP 响应体）**。⇒ **(c) 不是"退而求其次"，而是当前**唯一**能取到 `D-2` 所需证据的合规路径。**

### E-3 推荐方案与前置条件

**建议把 `V-3.7` 拆为两项**（不改判据实质，只拆"取证通道"）：

| 子项 | 内容 | 通道 | 前置条件 |
|---|---|---|---|
| **`V-3.7a`**（可立即推进） | `M-2 / M-3 / M-4 / M-5`：逐类投喂 → 断言 **HTTP 码 + 四字段计数 + `log_history` 增量** | **(c) 本地 `wrangler dev` + 本地 D1** | ① 需新增**测试夹具**（`test/` 目录，**非**生产代码）；② 需 `wrangler dev` 可用（本地，不触生产） |
| **`V-3.7b`**（需人工） | `M-1 / M-6`（静默类 ⇒ 只需证"未污染 legacy"）· `M-7`（`HR-C2-7a/7b` 归属） | **(d) 控制台 SQL 试跑**（首选）或 **(b) 测试 device_id**（须批准） | ① 确认控制台具备 SQL 试跑；② 归档 `HR-C2-7a/7b` 裁决；③ 若走 (b)：**人工批准 + 指定测试 id（禁复用 `d163d0000001/2/3`、`3cfb56c8a5fc`）+ 事后清理授权** |

**对"整体 `DEFERRED`"的意见**：**不建议**。`V-3.7a`（4 类中的 4 类）**存在完全无污染的合规通道**，`DEFERRED` 会把**本可立即闭合**的部分一并挂起。**建议**：先落 `V-3.7a`，仅把 `V-3.7b` 中"需控制台/裁决"的部分登记为**待人工**。
**本轮未做**：未发布畸形报文、未创建测试数据、未改 EMQX / Worker 配置。**置信度**：中高（`(d)` 的控制台能力**未经验证** ⇒ 标 `QUESTION`）· **需人工授权**：**是**

---

## §7 汇总（任务要求的六项）

### 7.1 可以直接确认的结论

1. **配置机制（A-1）**：`config_set` ⇒ dirty + 5 min 倒计时（**每次 set 重置**）⇒ 到期**先 `config_save` 再 `request_restart`** ⇒ 10 s 安全窗口 ⇒ 重启。**无取消路径**。
2. **设备已重启（A-2）**：`17:50:16 UTC`，`boot_seq` `101 → 102`；时刻与代码语义**逐秒吻合**。
3. **持久化有效值 = `false`（A-3）**：重启只在 save 成功后发生 + 重启后 30 min **零遥测** ⇒ 四条状态中 ②④ 已判定。
4. **5 条遥测逐条关联成功（B-2）**：键 = `(device_id, type, ts, seq)`，字段级 1:1。
5. **`V-3.6` `PASS` 可维持（B-5）**：三重自洽 + 复现 + 负向对照。
6. **`mqtt_messages` 增量 100% 逐条归因（C-3）**：`3484–3507` = 6 命令 × 4 帧；`3508` 为重启后设备自发 `online`。
7. **`/log` 最终态隔离成立（C-5a）**：联合键 `(boot_seq, seq)` 反查 = **0**。
8. **`V-3.9` 实质成立（C-5c）**：24 帧命令落库 + `direction` 正确（路径为 `POST /`）。
9. **`O-1` 根因闭合（D-1）**：设备侧**应用层重传阶梯**，= `BT-1` 的可观测症状；4 项独立量化吻合。
10. **`V-3.7a` 存在无污染通道（E-3）**：本地 `wrangler dev` + 本地 D1。

### 7.2 被原始证据推翻的结论

| # | 原结论 | 推翻依据 |
|---|---|---|
| 1 | 「测试期间**未重启**」（写作永久事实） | `connected_at = 17:50:16Z` + `boot_seq 101→102` + `device_event 215/216` |
| 2 | 「`log_history 17 → 20 / 净增 +2`」 | 基线两查询**非原子**；窗口归属 = `id 101/107`；表净增 3 ≠ 窗口归属 2 |
| 3 | 「顺带复证 **`V-3.8` `PASS`**（tail 2× presence 200）」 | 窗口内 `device_event` **+0**；4 次 presence 全为**非设备客户端**；且次数应为 **4** 非 2 |
| 4 | 「Round A 有路径级证据」（隐含于判据表整体 `PASS`） | `tail_v36.jsonl` **零事件** |
| 5 | 「tail 共 18 事件 / presence 2 次」 | 落盘产物 = **21 事件 / presence 4 次**（原数为运行中快照） |
| 6 | 「`O-1` = ★ 新问题」 | 系既有 `BT-1` 症状；**非新缺陷** |

### 7.3 仍无法确认的事项（`UNKNOWN` / 未取到证据）

| # | 事项 | 缺失的原始证据 | 最小补证 |
|---|---|---|---|
| 1 | ingest 响应的 `accepted / duplicate / rejected / filtered` **实测值** | tail 不留响应体；Worker 成功路径无打点 | 本地回放读响应体（**零生产影响**） |
| 2 | 窗口内 4 次 presence 事件的**具体 clientid / username / reason** | presence 请求体未打点 | 本地回放，或下次设备真实上下线时 tail |
| 3 | MQTT 协议层是否参与重传（`DUP` / QoS） | 抓包未记录 `msg.dup` / `msg.qos` | 补采一条订阅脚本字段（**只读**） |
| 4 | `V-3.7b` 的 `M-1/M-6/M-7` 现网行为 | 规则 A/B 逐字未核（API 403）；`HR-C2-7a/7b` 未归档 | 控制台只读核对 + 人工裁决 |
| 5 | **基线** `log_history` 行集的精确快照 | 两查询非原子 | 后续基线**单条 SQL** 同时取 `COUNT` 与 `MAX(id)` + 记录查询时刻 |
| 6 | 持久化键是"缺省"还是"显式 `false`" | 未读 `version.json` / `mqtt.json` | 非必要（有效值等价） |

### 7.4 最小补证清单（按性价比排序）

| 优先 | 动作 | 通道 | 生产影响 | 授权 |
|---|---|---|---|---|
| **P0** | 订正原报告 3 处（A-2 重启 / C-1 计数 / C-5b V-3.8） | 文档 | 无 | 否（但涉及验收表述 ⇒ 建议授权） |
| **P1** | `V-3.7a`：本地 `wrangler dev` + 本地 D1 跑 `M-2…M-5` | 完全本地 | **零** | 否 |
| **P2** | 补采 MQTT `dup`/`qos` 字段（仅订阅，不发命令） | MQTT 订阅 | 极低 | 否 |
| **P3** | 控制台只读核对规则 A/B 逐字（解 `SYN-1…SYN-4`） | 人工控制台 | 无 | **是**（人工） |
| **P4** | `V-3.7b` 的 `M-7` 归属须先归档 `HR-C2-7a/7b` | 裁决 | — | **是** |
| **P5** | 为 `V-3.2` 补"响应体 `rejected` 计数"通道（本地回放 或 临时打点） | 本地/代码 | 零 / 需部署 | **是**（后者） |

### 7.5 建议维持 / 修订 / 暂停的验收状态

| 项 | 原状态 | **建议** | 理由 |
|---|---|---|---|
| **`V-3.6`** | `PASS` | ✅ **维持 `PASS`** + 3 条限定条款（R-A 无路径证据 / 响应体未实测 / 报告 3 处订正） | B-5 |
| **`V-3.4`** | `PASS`（原报告"顺带复证"） | ✅ **维持 `PASS`**，**证据升级**为联合键反查 = 0 | C-5a |
| **`V-3.8`** | `PASS`（原报告"顺带复证"） | ⚠️ **撤回"在 `V-3.6` 窗口内复证"的说法**；`V-3.8` 自身的状态应回到其**原验收证据**上判定（**不在本轮范围**） | C-5b |
| **`V-3.9`** | `PASS` | ✅ **维持 `PASS`**，判据措辞改为 `POST /`（既有 webhook 通路） | C-5c |
| **`V-3.7`** | `BLOCKED` | 🔀 **拆分**：`V-3.7a` = 可推进（本地，`NOT RUN` ⇒ 待执行）· `V-3.7b` = `HUMAN_REQUIRED`（控制台能力 + `HR-C2-7a/7b`） | E-3 |
| **`V-3.2`** | `PASS`（历史） | ⚠️ **建议复核**：其"逐条记录 `rejected` 计数"要求在现有 tail 通道下**无法取证** | B-4 |
| **`O-1`** | ★ 新问题（未定级） | ⚠️ **改判为 `BT-1` 的可观测症状**（`MINOR`），**不新开编号** | D-4 |
| **`V-3` 整体** | 仅 `V-3.7` 未闭合 | ✅ 维持；**新增** `V-3.7a/b` 拆分与 `V-3.2/V-3.8` 两项复核 | 上 |

### 7.6 意外发现的工作树变化与副作用

| # | 发现 | 说明 |
|---|---|---|
| 1 | **三仓工作树与上一轮记录完全一致** | 固件 `wb@1ebae81`（4 M + 1 ??）· 云端 `main@e328a11` clean · 消息 `main@3165aab` clean。**无新增漂移、无意外改动** |
| 2 | **本轮所有 D1 访问均为 `SELECT`** | `wrangler d1 execute` 返回的 `meta.changes` 恒为 `0`、`rows_written` 恒为 `0`（可自证） |
| 3 | **设备在本轮**（只读观测期间）**未被我方触发任何命令** | 未发 `config_set` / 未重启 / 未发布畸形报文；EMQX 仅 `GET /clients` |
| 4 | **设备在本轮复核期间状态稳定** | `GET /clients`（复核起、止各一次）：`dev_288485896ce4` 始终 `connected=true`、`subscriptions_cnt=1`、`keepalive=120`，**会话 `created_at` 恒为 `17:50:16.583`** ⇒ **复核窗口内未再发生重启**。⇒ 与 A-2 的"5 min 倒计时触发一次重启"**不矛盾**（本轮**未**发送任何 `config_set` ⇒ **未重新武装**倒计时） |
| 5 | ⚠️ **`log_history` / `mqtt_messages` 在复核期间**（未受我方影响）**继续增长** | `log_history 20 → 33`；`mqtt_messages` 新增 **1 行**（`id 3508`，设备自发 `online` 帧）。⇒ **再次印证"以总行数判定隔离"不可靠** |
| 6 | ⚠️ **`tail_v36.jsonl` 是"失败产物"而非"空产物"** | 内容为 `ETIMEDOUT` 堆栈。建议保留并在报告中标为**反例**（"tail 必须走代理"） |

---

## §8 本轮边界声明

**未做**：未修改固件 / 云端代码 · 未修改任务单 / 验收矩阵 / 正式审阅记录 · 未修改 EMQX 规则 / 连接器 / Action / ACL / 账号 · 未部署 Worker / Pages · 未重启设备 · 未更改设备配置 · 未执行任何 D1 `INSERT`/`UPDATE`/`DELETE` · 未清理历史噪音 · 未 commit / push / reset / checkout。

**已做**：只读读取固件与 Worker 源码 · 只读读取 `.pio/` 原始产物 · D1 `SELECT`（`meta.changes` = 0）· EMQX `GET /clients`（只读）· 生成本报告。

**产物**：本报告（`docs/review/Phase-C-C2-V36-Review-Follow-up-Read-Only-Investigation.md`）—— **新增未提交文件**；此外**无任何**工作树改动。

---

## §9 ★ 收口后状态更新（`2026-10-16` · 追加 · 本报告结论**不改**）

> 本报告 §7.5 的「`V-3` 最终状态」为**出具时快照**（`V-3.7` 未闭合）。其后由**后续独立工作**推进，现全部闭合 ——
> 权威 → **`docs/review/Phase-C-C2-Final-Closure-Report.md`**（**`Phase C-C2 = CLOSED`** · `V-3.1…V-3.9` 全 `PASS`）。

| 项 | 本报告（出具时） | 收口时 | 依据 |
|---|---|---|---|
| `V-3.7a` | 可推进（`NOT RUN`） | ✅ `PASS` | 本地隔离 `81 PASS / 0 FAIL` × 2 → `Phase-C-C2-V37a-Local-Isolation-Verification-Report.md` |
| `V-3.7b` | `HUMAN_REQUIRED` | ✅ `PASS`（构造性 + 人工核实） | 规则 A/B **人工核实 `VERIFIED`**；`HR-C2-7a`（黑名单）与生产规则逐字一致 |
| `V-3.2` | 建议复核（计数通道不存在） | ✅ `PASS`（口径修订） | 采纳**分层**取证：线上路径由 tail · 计数语义由本地真引擎 |
| `V-3.8` | `NOT RUN`（无有效记录） | ✅ `PASS` | 独立正式验收 → `Phase-C-C2-V38-Verification-Report.md` |
| `O-1` | 改判为 `BT-1` 症状 | ✅ **已归并** | 登记入 `docs/issues/未修复的问题.md` 的 **`BT-1`** 条目（`MINOR`） |
| `V-3.4` | 维持 `PASS` | ✅ 维持 | ★ 关联用 **`(boot_seq, seq)` 联合键**（`boot_seq` 单键不可用） |
| `V-3.9` | 维持 `PASS` | ✅ 维持 | 措辞统一为实际路径 **`POST /`** |

> ⚠️ 本报告 §7.5 / §7.6 的原始判定与工作树快照**逐字保留**；**以本 §9 为最终状态**。

---

*本报告为 `2026-10-16` 二轮只读复核产物，等待人工裁决。未获授权前不启动任何修复、回填或部署。*
*★ **`2026-10-16` 收口更新**：见 **§9** —— `Phase C-C2 = CLOSED`（`V-3.1…V-3.9` 全 `PASS`）。*
