# Phase C-C2 · `V-3.6` Telemetry 回归 —— 真机验证记录

> **任务**：完成 `Phase-C-C2-Log-Routing-Console-Task-Sheet.md` §4.1 中唯一一项「窗口内无流量 ⇒ `NOT RUN`」的验收项 **`V-3.6`（Telemetry 回归）**。
> **日期**：`2026-10-16`（本地 `01:32`–`01:52` GMT+8 / UTC `17:32`–`17:52`）
> **设备**：`288485896ce4`（client_id `dev_288485896ce4`）
> **判定**：✅ **`V-3.6` = `PASS`**（两轮独立触发，tail 路径证据 + D1 落库证据双闭合）
> **副作用**：设备运行时配置 `mqtt.telemetry_enable` 临时置 `true`，**已恢复 `false`**（终态与 HD-1 收尾一致）
> **未改动**：EMQX 规则 / 连接器 / 动作 · Worker / Pages 代码与部署 · D1 结构 · 固件与编译门

---

> ## ⚠️ 本记录已由二轮复核**订正**（`2026-10-16`）—— 阅读时**必须**连同下条一起看
>
> **权威订正文件** → **`docs/review/Phase-C-C2-V36-Review-Follow-up-Read-Only-Investigation.md`**（只读复核 · A–E）
> **结论：`V-3.6` 主判定 `PASS` 可维持**；但本记录内有 **5 处结论/表述**须以订正为准（**原始 D1 数字 / 抓包 / tail 事件逐字保留，未改动**）：
>
> | # | 本记录原文 | **订正** | 性质 |
> |---|---|---|---|
> | 1 | `log_history` `17 → 20`，**净增 `+2`**（§3.1 / §3.5） | **非原子基线**：基线 `COUNT=17 @ 17:34`（max id 89）与 `MAX(id)=95 @ 17:36:36` 相隔 ≈3.5 min ⇒ 表格内数字**自相矛盾**。**窗口归属 `+2` 正确**，但**不得**写作「`20 − 17`」 | 算术/表述 |
> | 2 | 「测试期间**未观察到重启**」（`O-4`：`connected_at` 恒 `17:13:46`） | **已推翻**：设备**确实于 `17:50:16 UTC` 重启**（`connected_at` 更新 + `log_history` 出现 `boot_seq 102`）。原文结论**仅在其观测窗口内为真**。机制（代码事实）：末次 `config_set` `17:44:58` + **5 min** ⇒ `config_save()` ⇒ `request_restart()` ⇒ **10 s** 安全窗口 ⇒ 重启 | `MAJOR` |
> | 3 | `V-3.8`（Presence 回归）= ✅ `PASS`「tail：2 × `POST /api/presence/event` `200`」（§4） | **已撤回**：窗口内 `device_event` **+0**；实测 **4 次**（非 2 次）presence `200` **全部来自非设备客户端** ⇒ `presence.js` 返回 `not_device_identity`，**属设计正确**。**`HTTP 200` 不是 `V-3.8` 的通过判据**。`V-3.8` 的 `PASS` 依据 = **独立正式验收** `Phase-C-C2-V38-Verification-Report.md` | `MAJOR` |
> | 4 | tail「18 事件」（§3.2 / §4） | **订正**：那是 tail **运行中**的中途快照；落盘为 **21 事件** | 表述 |
> | 5 | 「★ **新问题 `O-1`**」（§5）· 「建议登记为**独立问题**」（§7） | **归并**：`O-1` **不是新缺陷**，是既有 **`BT-1`**（云端从不回 `log_ack`）的**现网可观测症状** ⇒ 已登记至 `docs/issues/未修复的问题.md` 的 **`BT-1`** 条目 · **`计划:WP-5`**；**不新开编号** | 归类 |
>
> **★ 根因闭合（后续追加取证）**：`O-1` = **设备侧应用层重发** —— MQTT 协议级实测 **`qos=1` · `dup=False`**（⇒ 排除协议层重传）·
> 同一记录 **6 次**发布 · 间隔 **`17/19/23/31/47 s`** = `LOG_ACK_TIMEOUT_MS(15 s)` + 退避 `2/4/8/16/32 s` **逐项吻合** · D1 `rowid` **步长 6**（1 插入 + 5 幂等忽略）。
> **影响**：`MINOR`（成本 / 可观测性）—— 当前被幂等键**完全吸收**，**数据零损失**；但 `BT-1` 正把日志上行**限流**到 ≈1 条 / 152 s，修复时**必须与负载评估同批**。

---

## §1 验收判据（任务单原文）

| 项 | 判据 |
|---|---|
| `V-3.6` | **Telemetry 回归**：`/up` telemetry **仍落 `telemetry_history`**（`+≥1`，**且不落** `mqtt_messages`） |
| 手段 | D1 前后增量 / `wrangler tail` |
| 等级 | L2 |

> ⚠️ 任务单把 `V-3.6` 与 `V-3.4`（`/log` 最终态隔离）**分开**：`V-3.6` 的被测对象是 **telemetry 信封**（`{ts,type,value,seq,schema_version}`，**无** `c` 字段），**不是**命令回包。
> 命令回包（`{"c":"ack"/"result"…}`）落 `mqtt_messages` 属 **`V-3.9`** 的**预期**行为，**不能**用于满足 `V-3.6`。

---

## §2 环境与基线

### §2.1 手段可用性核查（先于任何操作）

| 手段 | 结论 | 说明 |
|---|---|---|
| `mqttx` CLI | ❌ **未安装** | PATH / `npm -g` / `D:\tools` 均无；**未擅自安装** |
| `paho-mqtt`（`~/.workbuddy/.../envs/default`） | ✅ 可用（`2.1.0` + `cbor2`） | 仓内既有跑具 `.pio/hd1_run.py` 正是此实现 ⇒ **直接复用** |
| MQTT 账号 | ✅ `workbuddy` | ACL `userRules`：`guo_feeder/#` **all allow** ⇒ 可向 `/down` 发布 |
| `wrangler tail` | ✅（**必须走代理** `127.0.0.1:7890`） | 直连 `ETIMEDOUT`；经代理正常（与既有记忆一致） |
| D1 远端读 | ✅ `npx wrangler d1 execute --remote` | 只 `SELECT` |

> **对「使用 mqttx」的处置**：本机无 `mqttx`，改用**仓内已验证的等效通道** `.pio/hd1_run.py`
> （同为「TLS 8883 + 订阅 `guo_feeder/<did>/#` + 向 `/down` 发布」，且**同时留存原始抓包**）。
> 若需严格 `mqttx` 形态，属**工具选型**问题，不影响本项判据。

### §2.2 三表基线（`2026-10-15 17:33 UTC`，测试前）

| 表 | 计数 | `MAX(id)` |
|---|---|---|
| `telemetry_history` | `15` | `35` |
| `log_history` | `17` | `95` |
| `mqtt_messages` | `3465` | `3483` |

**关键前提**：`mqtt_messages` 中 `/log` 行**最后一笔 = `id 3483`（`17:20:22 UTC`）** ⇒ 该时点即**规则 B 通配形态生效**的分界。**`id > 3483` 中 `/log` 行数 = 0** 是本轮 `V-3.4` 的关联判据。

### §2.3 产出 telemetry 的前置条件

真实 telemetry 信封**只**由 `telemetry_publisher.cpp` 产出，受**双门**约束：

```
有效发送 = 编译门 GF_TELEMETRY_ENABLED==1  ∧  运行时开关 mqtt.telemetry_enable == true
```

- 设备当前固件 = **`c1_gate_on` 构建（`GF_TELEMETRY_ENABLED=1`）**（HD-1 收尾态）；
- 设备当前 `mqtt.telemetry_enable` 经**只读** `config_query` 实测 = **`false`**（安全态）。
- ⇒ **必须临时开启运行时开关**，否则 `/up` 上只会有命令回包（不足以满足 `V-3.6`）。

> `config_get_telemetry_enable()` = `module_ro(MQTT)["telemetry_enable"] | false` ⇒ **纯扁平键**，**fail-closed**。

---

## §3 执行序列与观测结果

### §3.1 Round A（完整轮 · `17:38:38`–`17:41:22 UTC`）

计划（`.pio/plan_v36.txt`，经 `/down` 下发）：

```
config_query mqtt telemetry_enable        (i=v36q01)
config_set   mqtt telemetry_enable=true   (i=v36s01)
config_set   mqtt telemetry_enable=false  (i=v36s02)
```

**MQTT 抓包**（`.pio/v36_mqtt.log`，同时订阅 `guo_feeder/288485896ce4/#`）：

| # | topic | 内容 | 判定 |
|---|---|---|---|
| 1 | `/up` | `{"s":0,"c":"result",…,"data":{"module":"mqtt","key":"telemetry_enable","value":false}}` | 测试前确为 **false** ✅ |
| 2 | `/up` | `{"ts":1792085942,"type":"climate.temperature","value":24.8,"seq":1,"schema_version":1}` | **telemetry ①** |
| 3 | `/up` | `{"ts":1792085942,"type":"climate.humidity","value":55.5,"seq":2,"schema_version":1}` | **telemetry ②** |
| 4 | `/up` | `{"ts":1792085942,"type":"weight.value","value":0,"seq":3,"schema_version":1}` | **telemetry ③** |

- **`set true` 与首批遥测同秒出现**（`ts` 均为 `1792085942`）⇒ **复现 HD-1 的「开关即时生效」结论**（无需重启）。
- 回包文案为 `"field updated, restart required"`（配置**持久化**层面语义），但**运行时门为每轮现读** ⇒ 二者不矛盾。
- `/up` 合计 12 帧 = **3 telemetry + 9 命令回包**（3 条命令 × [ack + result-queued + result-ok]）。
- `/log` 7 帧（见 §5 异常观察）。

**D1 增量**：

| 表 | 前 → 后 | 增量 | 明细 |
|---|---|---|---|
| `telemetry_history` | `15` → `18` | **`+3`** | `id 36/37/38` = **与抓包 1:1 对应**（`ts=1792085942`，`climate.temperature=24.8` / `climate.humidity=55.5` / `weight.value=0`，`seq 1/2/3`，`schema_version=1`） |
| `mqtt_messages` | `3465` → `3477` | `+12` | **全部 12 笔均是命令帧**：`id 3484-3486/3489-3491/3493-3495` = 9 × `/up` 回包；`id 3487/3488/3492` = 3 × `/down` |
| `mqtt_messages` 中含 `schema_version` 的行 | — | **`0`** | **telemetry 信封未进 `mqtt_messages`** ✅ |
| `log_history` | `17` → `20` | `+2`（净） | 期间设备持续发 `/log`（见 §5） |

> 🔴 **订正 1（`2026-10-16` 二轮复核）**：上表 `log_history` 的 `17 → 20` 为**非原子基线**——
> `COUNT=17` 落在 `17:34`（max id **89**），而 `MAX(id)=95` 落在 `17:36:36`，两者相隔 ≈3.5 min ⇒ **表格内数字自相矛盾**。
> **窗口归属 `+2`（`id 101/107`）结论正确**，但**不得**写作「`20 − 17`」。**其余行的数字未经改动。**

### §3.2 tail 窗口（`17:43:10`–`17:45:42 UTC`）

`wrangler tail --format json`（**经代理**）共 18 事件：

> 🔴 **订正 4（`2026-10-16`）**：此处 **18 事件**是 tail **运行中**的中途快照；**落盘为 21 事件**。
> 另：**Round A 无 tail 路径级证据** —— 首个 tail 会话（`tail_v36.jsonl`）因 `ETIMEDOUT` **零事件**，tail 实际只覆盖 Round B。

```
1 × POST /api/history/ingest  200  len=387   ← /log 帧
1 × POST /api/presence/event  200  len=146
... (Round B 触发后) ...
2 × POST /api/history/ingest  200  len=166 / len=163   ← ★ 两条 telemetry 信封
4 × POST /                    200  len=187/161/188/230 ← 命令帧（规则 B 桶）
```

### §3.3 Round B（复现轮 · `17:44:06`–`17:44:59 UTC`）

计划（`.pio/plan_v36b.txt`）：`set true` → 50 s → `set false`。

- 抓包（`.pio/v36b_mqtt.log`）：**2 条 telemetry** = `{"type":"climate.temperature","value":24.8,"seq":4}` + `{"type":"climate.humidity","value":55.5,"seq":5}`。
  （`weight.value` 未发：周期 `15 min` 未到且 `Δ=0 < 20 g`；climate 周期 `5 min` 已到 ⇒ **与策略草案一致**。）
- **tail 侧**：`17:44:10` 连续两笔 **`POST /api/history/ingest` 200**，`len=166 / len=163` —— 正是上述两信封
  （`{"topic":…,"clientid":…,"payload":{…}}` 包壳后 ≈ 163–166 B；对照 `/log` 帧恒为 `387 B`）⇒ **路径级证据闭合**。
- **D1**：`telemetry_history` `18` → `20`（`id 39/40`，`seq 4/5`）；`mqtt_messages` `3477` → `3485`（`+8` = 2 `/down` + 6 `/up` 命令回包）。

### §3.4 终态核对（`17:48`–`17:50 UTC`）

```
config_query mqtt telemetry_enable  →  {"key":"telemetry_enable","value":false}
90 s 静默窗口内 /up telemetry 帧数   →  0
```

### §3.5 最终 D1（`17:52 UTC`）

| 指标 | 值 | 说明 |
|---|---|---|
| `telemetry_history` | `20`（基线 `15`，**`+5`**） | Round A `+3` + Round B `+2` |
| `mqtt_messages` | `3489`（基线 `3465`，`+24`） | **全量可归因**：Round A `+12` + Round B `+8` + 终态 `config_query` 轮 `+4`（1 `/down` + 3 `/up`） |
| `mqtt_messages` 含 `schema_version` | **`0`** | ✅ |
| `mqtt_messages` 中 `/log` 且 `id > 3483` | **`0`** | ✅（顺带复证 `V-3.4`） |

---

## §4 判据核对

| 判据 | 结果 | 证据 |
|---|---|---|
| `/up` telemetry 落 `telemetry_history` `+≥1` | ✅ **PASS** | 两轮 `+3` / `+2`，且与 MQTT 抓包**字段级 1:1** |
| `/up` telemetry **不落** `mqtt_messages` | ✅ **PASS** | `schema_version` 命中 **0**；`mqtt_messages` 全部增量 100% 归因于命令帧 |
| 路由**路径**正确（`/api/history/ingest`，非 webhook 桶） | ✅ **PASS** | tail：telemetry 信封 => `POST /api/history/ingest` `200`；命令帧 => 另一桶，**互不混淆** |
| 可复现 | ✅ **PASS** | 两轮独立触发，结果同构 |

### §4.1 附带复证的相邻验收项（本轮流量顺带覆盖）

| 项 | 结果 | 证据 |
|---|---|---|
| `V-3.4`（`/log` 最终态隔离） | ✅ **PASS** | 窗口内 `/log` 帧 ≥7 条在线上，**`id 3484-3489` 区间内 `/log` 行 = 0**；末笔 `/log` 仍停在 `id 3483`。★ **关联须用 `(boot_seq, seq)` 联合键**（`boot_seq` 单键在 `mqtt_messages` 无独立列、文本匹配不可靠） |
| `V-3.8`（Presence 回归） | 🔴 **本行结论已撤回（`2026-10-16`）** | 原文依据「tail：2 × `POST /api/presence/event` `200`」**无效**：窗口内 `device_event` **+0**；实测 **4 次**（非 2 次）presence `200` **全部来自非设备客户端**（`presence.js` 返回 `not_device_identity`，属设计正确）。**`HTTP 200` 不得单独作为 `V-3.8` 通过判据**。`V-3.8` 的 `PASS` 依据 = **独立正式验收** `Phase-C-C2-V38-Verification-Report.md`（真实设备 `offline → online` · `device_event 187/216 → 189/218`） |
| `V-3.9`（命令下行 + 回包回归） | ✅ **PASS** | 24 笔 `/down` + `/up` 命令帧全部落 `mqtt_messages`，`direction` = `down`/`up` 正确；★ **实际 webhook 通路 = `POST /`**（非历史文字中的 `POST /api/webhook`） |

---

## §5 异常与观察（**不构成 `V-3.6` 失败**，但需登记）

| # | 观察 | 证据 | 建议 |
|---|---|---|---|
| **O-1** | **`/log` 帧重复发布**：同一帧 `(boot_seq=101, seq=110091, ts=1792085920)` 在 2.5 min 内被发布 **6 次**（`seq 110090` 1 次；`seq 110092` 另起） | `.pio/v36_mqtt.log` 计数 `6 × seq=110091` | 🔴 **已归并（`2026-10-16`）**：`O-1` **不是新缺陷**，是既有 **`BT-1`**（云端从不回 `log_ack`）的**现网可观测症状** ⇒ 已登记至 `docs/issues/未修复的问题.md` 的 **`BT-1`** 条目 · **`计划:WP-5`**；**不新开编号**。<br>**根因已闭合**：MQTT 实测 **`qos=1` · `dup=False`**（⇒ **排除协议层重传**）· 同一记录 **6 次**发布 · 间隔 **`17/19/23/31/47 s`** = `LOG_ACK_TIMEOUT_MS(15 s)` + 退避 `2/4/8/16/32 s` **逐项吻合** · D1 `rowid` **步长 6**（1 插入 + 5 幂等忽略）⇒ **设备侧应用层重发**。**影响 `MINOR`**（数据零损失 · 但把日志上行限流到 ≈1 条 / 152 s ⇒ 修 `BT-1` 时须与负载评估同批） |
| **O-2** | `mqttx` CLI **未安装** | PATH/`npm -g`/`D:\tools` 全无 | 若坚持 `mqttx` 形态，需新开会话窗口安装；本轮以等效通道完成 |
| **O-3** | `mqtt_messages` 历史 `/log` 噪音 **`867` 行**（`id ≤ 3483`） | `SELECT COUNT(*) … WHERE topic LIKE '%/log'` | **未清理**（属独立审批项，本轮**只读**） |
| **O-4** | 设备 `config_set` 后存在 **5 min 自动 `save`+重启**窗口 | 既有记忆 + HD-1 记录 | 🔴 **订正 2（`2026-10-16`）**：「`connected_at` 观测期内始终 `17:13:46`（**未重启**）」**已推翻** —— 设备**确实于 `17:50:16 UTC` 重启**（`connected_at` 更新 + `log_history` 出现 `boot_seq 102`）。机制：末次 `config_set` `17:44:58` + 5 min ⇒ `config_save()` ⇒ `request_restart()` ⇒ 10 s 安全窗口 ⇒ `ESP.restart()`。**原文结论仅在其观测窗口内为真**；写入值 = `false` = 既有持久值 ⇒ **无状态漂移**（该结论维持） |

---

## §6 本轮「未做」与「未改」

- ❌ 未安装 `mqttx`；未改 `hd1_run.py`（**原样复用**）。
- ❌ 未修改 EMQX 任何规则 / 连接器 / 动作（规则 A/B **保持** `V-3` 施工后状态）。
- ❌ 未部署 Worker / Pages；未 `git commit` / `push`。
- ❌ 未做任何 D1 写入语句（`wrangler d1 execute` 仅 `SELECT`；`changes` 列恒 `0`）。
- ❌ 未清理 `mqtt_messages` 历史噪音；未回填任务单状态（见 §7）。
- ✅ 设备运行时开关 `telemetry_enable`：`false → true（临时）→ false`（**终态 = 安全态**）。

---

## §7 建议回填（待人工裁决，本轮不动文档）→ ✅ **已于 `2026-10-16` 全部执行**

1. **任务单 `§4.1`**：`V-3.6` 由 `NOT RUN` → **`PASS`**（已回填 · 证据列含 tail + D1 · 两轮）。
2. **任务单 `§4.1`**：`V-3.4` / `V-3.9` 由本轮流量顺带覆盖（**已回填 `PASS`**）；🔴 **`V-3.8` 不再以此为依据** —— 其 `PASS` 依据 = 独立正式验收 `Phase-C-C2-V38-Verification-Report.md`（订正 3）。
3. **任务单 `§4.7` 未验证项汇总**：`V-3.6` 已移出；`V-3.7` 已由 **`V-3.7a`（本地隔离实测）+ `V-3.7b`（构造性 + 人工核实）** 收口 ⇒ **全部 `PASS`**。
4. **`O-1`**：**不**新开编号 —— 已归并至 **`BT-1`**（现网可观测症状 · `MINOR`）· 见 §5 与 `docs/issues/未修复的问题.md`。
5. `mqtt_messages` 867 行历史噪音清理：**独立审批项**（含 `DELETE`，须单独授权 + 备份）—— **仍然待批**。

> ✅ **本记录状态（`2026-10-16`）**：主判定 **`V-3.6` = `PASS`（维持）**；上述 **5 项订正**已落文；
> **`Phase C-C2 = CLOSED`**（见 `Phase-C-C2-Final-Closure-Report.md`）。**原始 D1 数字 / 抓包 / tail 事件均未改动。**

---

## §8 附件（工作树内，`data/` 与 `.pio/` 均不随仓库分发）

| 文件 | 内容 |
|---|---|
| `.pio/plan_v36.txt` / `plan_v36b.txt` / `plan_v36f.txt` | 三轮下发计划 |
| `.pio/v36_mqtt.log` | Round A 抓包（82 行 / 3 telemetry） |
| `.pio/v36b_mqtt.log` | Round B 抓包（56 行 / 2 telemetry） |
| `.pio/v36_final.log` | 终态静默窗口（32 行 / **0 telemetry**） |
| `.pio/tail_v36b.jsonl` | `wrangler tail` 原始事件（**落盘 21 事件** · 订正 4） |
| `.pio/tail_v36.jsonl` | 首次 tail 尝试（直连 `ETIMEDOUT`，**零事件** ⇒ **Round A 无路径级证据**；保留作「tail 必须走代理」之反例） |

---

*本记录为 `2026-10-16` 只读+单点可控触发验证之产物；不含代码改动、不含 EMQX/Worker/D1 变更。设备已恢复 `telemetry_enable=false` 安全态。*
*★ **`2026-10-16` 订正**：主判定 **`V-3.6` = `PASS` 维持**；本记录含 **5 项结论/表述订正**（见文首 ⚠️ 块）—— **原始证据（D1 数字 / 抓包 / tail 事件）逐字保留，未改动**。*
*权威订正文件 → `docs/review/Phase-C-C2-V36-Review-Follow-up-Read-Only-Investigation.md`；`Phase C-C2` 收口 → `docs/review/Phase-C-C2-Final-Closure-Report.md`。*
