# Phase C-C2 · `V-3.7a` 本地隔离验证与证据缺口收紧 —— 实施报告

> **日期**：`2026-10-16`
> **范围**：`V-3.7a`（`M-2 / M-3 / M-4 / M-5`）本地隔离矩阵 · MQTT `dup/qos` 补采 · `V-3.2` / `V-3.8` 证据充分性复核 · `V-3.7b` 前置缺口核查
> **上位**：`docs/review/Phase-C-C2-V36-Review-Follow-up-Read-Only-Investigation.md` §6 E-2/E-3（拆分建议）· 任务单 `Phase-C-C2-Log-Routing-Console-Task-Sheet.md` §4.6（判据 `D-2`）
> **性质**：**新增本地测试夹具 + 只读核查**。**未**改生产代码 / **未**部署 / **未**改 EMQX / **未**向生产 `/log` 注入 / 生产 D1 **仅 `SELECT`** / **未** commit

---

> ## ★ 后续状态更新（`2026-10-16` · 收口时追加 · 本报告**结论本身不改**）
>
> 本报告出具后，下列项已由**后续独立工作**推进/闭合（详见 **`Phase-C-C2-Final-Closure-Report.md`**）：
>
> | 本报告状态（出具时） | 收口时状态 | 依据 |
> |---|---|---|
> | `V-3.7b` = 🔴 `HUMAN_REQUIRED` | ✅ **`PASS`（构造性 + 人工核实）** | 规则 A/B 的**逐字配置已由人工核实 `VERIFIED`**；`HR-C2-7a/7b` **采用 `7a`（黑名单）** 并与生产规则**逐字一致** ⇒ 原两项前置消除。**未做独立畸形注入实测**（如实标注） |
> | `V-3.8` = ⚪ `NOT RUN` | ✅ **`PASS`** | 已按要求补做**独立正式验收** → `Phase-C-C2-V38-Verification-Report.md`（真实设备 `offline → online` · `device_event 187/216 → 189/218`） |
> | `V-3.2` = 🟡 `PARTIAL` | ✅ **`PASS`（口径修订）** | 采纳本报告 §5.1 的**最小补证方案 2（分层）**：线上路径由 tail 证明 · 计数语义由**本地真引擎**（本报告 §2 · `81/0`）证明 —— **未改生产代码、未部署** |
> | `M-1` / `M-6` = ⚪ `NOT RUN`（归 `V-3.7b`） | ✅ **收口** | 由**已核实的规则 SQL**（`log_predicate` 不成立 ∧ topic 被排除）**构造性**判定 = 静默不落任何表 |
> | `M-7` = ⚪ `BLOCKED`（归 `V-3.7b`） | ✅ **收口** | `HR-C2-7a`（黑名单）已确认 ⇒ 行为 = 落 `mqtt_messages`（**现状保留**） |
> | `O-1` 根因（本报告已协议级实证） | ✅ **归并至 `BT-1`** | `O-1` = `BT-1` 现网症状 · `MINOR` · **不新开编号**（登记入 `docs/issues/未修复的问题.md`） |
>
> ⚠️ **本报告 §8 的状态表与 §9 待办为「出具时快照」**，其原始判定**逐字保留**（含 `NOT RUN` / `HUMAN_REQUIRED` / `PARTIAL`），**未就地改写**；**以本更新块为最终状态。**

---

## §0 结论摘要（一句话）

`V-3.7a` 四类（`M-2`…`M-5`）**在完全本地隔离环境中全部实测通过（两遍各 `81 PASS / 0 FAIL`）**，`D-2` 所需证据（HTTP 码 + 四字段计数 + `log_history` 增量）**首次完整取得**；MQTT `dup/qos` 补采**成功取得 12 帧真实样本**，**直接证明 `O-1` 是「应用层重发」而非「MQTT 协议层重传」**（`qos=1` · **`dup=False`** · 每记录 **6 次**）；但 `V-3.7b` 的两项前置（**规则 A/B 逐字配置证据**、**`HR-C2-7a/7b` 归档裁决**）**仍然缺失**。

---

## §1 实际检查的文件 · 测试入口 · 环境与隔离证明

### 1.1 只读检查的文件（代码 = 事实第一依据）

| 文件 | 用途 |
|---|---|
| `Cloudflare_Assets/guo-feeder-api/src/history-ingest.js`（696 行） | 被测实现：入口 `L144–298` · `extractRecords()` `L342–392` · `asLevelCode()` `L437–450` · `normalizeLog()` `L540–617` · `persist()` `L636–666` |
| `src/device-registry.js` | `isValidDeviceId()` `L46–49`（`^[0-9a-f]{12}$`）· `deviceIdFromTopic()` `L168–176` |
| `src/index.js` | `L293–297`：`POST /api/history/ingest` 路由（**无成功路径打点**） |
| `migrations/0001…0003_p3_history_tables.sql` | 本地 schema 来源（`log_history` 幂等唯一键 `ux_lh_dedup = (device_id,boot_seq,seq)`） |
| `test/run_all.mjs` · `test/lib/{harness,mock_d1,paths}.mjs` · `test/current/history_ingest.test.mjs` | 既有测试框架与约定（本次**沿用**，未引入新框架） |
| `test/real_d1/history_ingest_real_d1.sh` + `README.md` | 既有「真实 D1 引擎（workerd/miniflare 本地）本地 dev」约定 —— 本脚本的**直接范式** |
| `docs/review/Phase-C-C2-Log-Routing-Console-Task-Sheet.md` §2.1/§2.2/§4.6/§7.2 | 判据与待裁项 |
| `.pio/hd1_run.py` | 既有 MQTT 订阅/发布脚本（补采点） |

### 1.2 测试入口与命令（可重复执行）

```bash
cd D:/Guo_Feeder_Project/Cloudflare_Assets/guo-feeder-api
bash test/real_d1/v37a_log_reject_matrix.sh            # 单次执行（首跑 :8795）
PORT=8796 bash test/real_d1/v37a_log_reject_matrix.sh  # 复跑（第二遍）
```

* 退出码 `0` = 断言全绿；输出含每条用例的 `INPUT / HTTP / RESPONSE`（**完整响应体**）/ `D1 before|after` / 逐条断言。
* **无网络、无 Cloudflare 登录、不触远端 D1**（脚本自带 `unset *_proxy` + `WRANGLER_SEND_METRICS=false CI=1`）。

### 1.3 隔离证明（脚本内自动断言 + 实测输出）

| 项 | 证据 |
|---|---|
| **本地 Worker** | `wrangler dev --local`（`wrangler 4.145.0` + `@cloudflare/workerd-windows-64` 本地二进制）启动于 `127.0.0.1:8795/8796`；请求全部发往 `http://127.0.0.1:<port>/api/history/ingest` |
| **本地 D1（独立 persist）** | `/d/Guo_Feeder_Project/Cloudflare_Assets/.wrangler/v37a_log_reject/state` —— **与既有 `real_d1` 脚本的 `state_A/state_B` 不同目录** |
| **gitignore** | `git check-ignore -v` ⇒ `.gitignore:5:.wrangler/`（脚本内断言输出） |
| **本地表清单** | `d1_migrations, sqlite_sequence, _cf_METADATA, device, device_credential, device_binding, device_event, telemetry_history, log_history` |
| **★ 无 legacy 表** | 断言「本地库**无** `mqtt_messages` 表」= **PASS（计数 0）** ⇒ legacy 通路在本地**物理不可写** |
| **★ 无远端调用** | 断言「脚本未调用远端 D1」= **PASS（计数 0）** |
| **★ 测试身份** | `0a1b2c3d4e5f`（合成 12-hex）—— **非**生产设备 `288485896ce4`，**非**既有测试 id（`aa11bb22cc33` / `d163d0000001…` / `3cfb56c8a5fc` / `deadbeef0001`） |
| migration 应用 | `0001 ✅` · `0002 ✅` · `0003_p3_history_tables.sql ✅`（真实 D1 引擎） |

> **隔离强度**：本层结论**等于**（同级于）既有 `test/real_d1` 的真实 D1 引擎层，**高于** `test/current/*.test.mjs` 的 stub 替身层。**但仍 ≠ 线上 EMQX 规则验证**（见 §6）。

---

## §2 `V-3.7a` 矩阵逐项结果

> **执行结果：`PASS = 81` · `FAIL = 0` · `exit 0`（两遍完全一致，逐行 diff 无差异）**
> 基线：`telemetry_history = 0` · `log_history = 0`（全新临时库）
> 终态：`log_history = 5`（`device = 0a1b2c3d4e5f`）· `telemetry_history = 0` · `mqtt_messages` **表不存在**
> D1 采样口径：**单条 SQL** `SELECT COUNT(*) AS n, COALESCE(MAX(id),0) AS mx FROM log_history`（同时取计数与最大行号）

### 2.1 `M-2` · 字段齐全但 `level` 非法 ⇒ **记录级**拒收（3/3）

| 子例 | `level` | HTTP | 完整响应体 | D1 Δ |
|---|---|---|---|---|
| `M-2a` | `5`（非枚举） | **200** | `{"accepted":0,"duplicate":0,"rejected":1,"filtered":0}` | **0** |
| `M-2b` | `256`（> `LOG_LEVEL_MAX=255`） | **200** | `{"accepted":0,"duplicate":0,"rejected":1,"filtered":0}` | **0** |
| `M-2c` | `"3"`（字符串 · 类型错） | **200** | `{"accepted":0,"duplicate":0,"rejected":1,"filtered":0}` | **0** |

* 关键断言（全部 PASS）：`HTTP=200` · `rejected=1` · `accepted=0` · `filtered=0` · **响应体无 `error` 字段**（⇒ **记录级**而非批次级）· 该 `(device_id,boot_seq,seq)` **逐条反查 = 0 行**。
* **与任务单预期一致**（§4.6 M-2 行），但任务单所附的 `BLOCKED` 理由（§2.6 `HR-C2-8` 导致「无法区分因非法被拒 vs 因类型不符被拒」）在 `F-C2-8` 修复后**已消解** ⇒ 该行判定应升级为「**可验证**」。

### 2.2 `M-3` · 完全合法 log 信封（5 子例 + 幂等重投）

| 子例 | 形态 | `level` | HTTP | 完整响应体 | D1 Δ | 落库 `level` |
|---|---|---|---|---|---|---|
| `M-3a` | 嵌套 `payload` 对象（`F-C2-9` 规范） | `3` | 200 | `{"accepted":1,"duplicate":0,"rejected":0,"filtered":0}` | **+1** | `ERROR` |
| **`M-3b`** | **与 `M-3a` 逐字节相同（重投）** | `3` | 200 | `{"accepted":0,"duplicate":1,"rejected":0,"filtered":0}` | **0** | — |
| `M-3c` | 嵌套 | `2` | 200 | `{"accepted":1,…}` | **+1** | `WARN` |
| `M-3d` | 嵌套 | `4` | 200 | `{"accepted":1,…}` | **+1** | `CRITICAL` |
| `M-3e` | **顶层/扁平**兼容（`F-C2-9` ②） | `1` | 200 | `{"accepted":1,…}` | **+1** | `INFO` |
| `M-3f` | 嵌套 | `0`（DEBUG） | 200 | `{"accepted":0,"duplicate":0,"rejected":0,"filtered":1}` | **0** | — |

* `M-3a` 附加断言：`payload` **原样搬运**（`Q0JPUg==` 未解码）· `created_at` 由 D1 生成（非空）。
* **★ 幂等（`M-3b`）**：`duplicate = 1` · `accepted = 0` · 唯一键 `(0a1b2c3d4e5f,31,9101)` 行数**恒为 1** · `log_history` 计数 **不增**。
* **★ 三口径分离（本报告明确区分）**：
  | 口径 | `M-3a` | `M-3b` | 说明 |
  |---|---|---|---|
  | 请求次数 | 1 | 1 | HTTP 调用数 |
  | 写入尝试次数（`INSERT OR IGNORE` 执行数） | 1 | **1** | 重投**仍执行了一次写语句** |
  | **最终净新增行数** | 1 | **0** | 被 `ux_lh_dedup` 吸收 |
* **★ 副产物证据（本地复现生产的 rowid 现象）**：本地 `log_history` 行号 = **`1, 3, 4, 5, 6`** —— **`id = 2` 缺失**（`sqlite_sequence.seq = 6`）。即 `M-3b` 那次**被忽略的重复插入消耗了一个 `AUTOINCREMENT` rowid 却未建行**。
  ⇒ 这与二轮复核在生产侧推出的「`D1 rowid 步长恒 6` = 1 插入 + 5 幂等忽略」**完全同构**，是本轮**独立复现**（更强：本地可逐步对照）。
* **与任务单预期**：**明确不符** —— 任务单 §4.6 M-3 行写「`200 {"accepted":0,"rejected":1}`（§2.6 缺陷）」，那是 **`F-C2-8` 修复前**的快照；**实测行为（接受 + 落库）才是修复后的正确行为**。差异已在 §7.1 单列。

### 2.3 `M-4` · 信封 `device_id` ≠ topic ⇒ **批次级** 400（3/3）

| 子例 | 形态 | HTTP | 完整响应体 | D1 Δ |
|---|---|---|---|---|
| `M-4a` | 嵌套 `payload.device_id = ffffffffffff` ≠ topic | **400** | `{"error":"device_id_mismatch"}` | **0** |
| `M-4b` | 顶层 `device_id = ffffffffffff`（扁平形态） | **400** | `{"error":"device_id_mismatch"}` | **0** |
| `M-4c` | 顶层 `ffffffffffff` / 嵌套 `0a1b2c3d4e5f`（**自相矛盾**） | **400** | `{"error":"device_id_mismatch"}` | **0** |

* **批次级语义断言**：响应体**无** `accepted/rejected` 计数（`<none>`）· 整请求拒绝 · 逐条反查 **0 行**。
* 源码位置：`history-ingest.js` **`L217–226`**（任务单标注的 `L126–131` 为**旧行号**，已漂移）。

### 2.4 `M-5` · topic 内 `device_id` 非法 ⇒ **批次级** 400（3/3）

| 子例 | topic | HTTP | 完整响应体 | D1 Δ |
|---|---|---|---|---|
| `M-5a` | `guo_feeder/ZZZ/log`（非 hex） | **400** | `{"error":"invalid_device_id"}` | **0** |
| `M-5b` | `guo_feeder/0A1B2C3D4E5F/log`（12 位但**大写**） | **400** | `{"error":"invalid_device_id"}` | **0** |
| `M-5c` | `guo_feeder/0a1b2c3d4e5/log`（11 位 · 长度错） | **400** | `{"error":"invalid_device_id"}` | **0** |

* 源码位置：`history-ingest.js` **`L190–195`**（任务单标注的 `L115–120` 为旧行号）。
* `M-5b` 说明：`isValidDeviceId` = `^[0-9a-f]{12}$` ⇒ **大小写敏感**，12 位大写**仍非法**（这一边界任务单未列，本轮补测）。

### 2.5 附加 `B` · 批次级 vs 记录级（证据边界）

| 子例 | 输入 | HTTP | 完整响应体 | D1 Δ |
|---|---|---|---|---|
| `B-1` | `payload = [ level=9坏条, level=3好条 ]` | **200** | `{"accepted":1,"duplicate":0,"rejected":1,"filtered":0}` | **+1**（仅好条） |
| `B-2` | `payload = [ 2 条合法 ]` 但**信封 `device_id` 不一致** | **400** | `{"error":"device_id_mismatch"}` | **0**（**无部分写入**） |

⇒ 明确：`M-2` 类 = **记录级**（200 + 计数）· `M-4`/`M-5` 类 = **批次级**（4xx + 零写入）。**两类不可混同**。

---

## §3 新增 / 修改的文件，与工作树变更前后对比

| # | 文件 | 变更 | 层级 |
|---|---|---|---|
| 1 | `Cloudflare_Assets/guo-feeder-api/test/real_d1/v37a_log_reject_matrix.sh` | **新增**（353 行 / 21,815 B） | 测试夹具（**非**生产代码） |
| 2 | `PIO_Assets/Guo_Feeder_Project/.pio/hd1_run.py` | **+3 行**（`on_message` 日志行补 `qos/dup/retain`） | `.pio/` = **gitignored** 临时目录，不入库 |
| 3 | `PIO_Assets/Guo_Feeder_Project/docs/review/Phase-C-C2-V37a-Local-Isolation-Verification-Report.md` | **新增**（本报告） | 审阅记录（**未提交**） |

**未修改**：`src/**`（Worker 生产代码）· `migrations/**` · 固件 `src/**` · 任何正式验收文档 / 任务单 / 验收矩阵。

### 工作树前后对比

| 仓 | 变更前 | 变更后 | 判定 |
|---|---|---|---|
| 固件 `PIO_Assets/Guo_Feeder_Project`（`wb`） | `1ebae81` · dirty **6**（4 M 文档 + 2 ?? 未提交报告） | `1ebae81` · dirty **7**（4 M 同前 + **3 ??**：2 份二轮报告 + **本报告**） | ⚠️ 新增仅 1 份报告；**4 份 REV-3 受保护文档未被触碰**（M 行数与内容同前），**HEAD 未变** |
| 云端 `Cloudflare_Assets`（`main`） | `e328a11` · clean | `e328a11` · **1 ??**（新脚本） | ⚠️ 唯一新增 = 本任务的测试夹具 |
| 消息 `EMQX_Assets`（`main`） | `3165aab` · clean | `3165aab` · clean | ✅ 未触碰 |

> `.wrangler/v37a_log_reject/` 与 `.wrangler/real_d1/` 均为临时产物，**被 `.gitignore` 覆盖**，不出现在 `git status` 中。
> **`test/real_d1/README.md` 未更新**（保持最小 diff）；建议作为可选后续（见 §8 待办）。

---

## §4 MQTT `dup` / `qos` 采证结果

### 4.1 最小改动方式（已实施）

`.pio/hd1_run.py`（**仅订阅通道**）的 `on_message` 回调，将日志行：

```python
log("[MSG] %s topic=%s\n%s\n---" % (t, msg.topic, body))
```

**最小改动为**（+3 行，一行语句）：

```python
log("[MSG] %s topic=%s qos=%s dup=%s retain=%s\n%s\n---"
    % (t, msg.topic, msg.qos, msg.dup, msg.retain, body))
```

* 该脚本**本身**即可满足「仅订阅」：计划文件只含 `WAIT <N>` 时**不发任何 `PUB`**（`PUB` 仅在计划显式出现时触发）。
* **未发送任何报文制造样本**；未改设备配置；未重启设备；未发命令。

### 4.2 采证窗口与结果

| 窗口 | 计划 | 时长 | 连接 | 收到消息 | 结果 |
|---|---|---|---|---|---|
| #1 | `WAIT 300` | 300.6 s | `rc=Success` · 订阅 `guo_feeder/288485896ce4/#` | **0 条** | 该时段设备静默（见 4.3） |
| **#2** | `WAIT 900` | **901.0 s** | 同上 | **12 条** | ✅ **取得样本** |

### 4.3 窗口 #1 为空的原因（已证 · 非工具问题）

| 证据 | 值 |
|---|---|
| 生产 `log_history` 在窗口 #1 前后的**最后一条** `created_at` | `1792087986` = `2026-10-15 18:13:06 UTC`（`02:13:06 GMT+8`） |
| 窗口 #1 时刻 | `18:47–18:52 UTC` |
| 设备会话 | `connected=true` · `created_at = 17:50:16.583`（**未重启**） |
| 窗口 #2 内设备**恢复**上行 | 首帧 `03:00:52 GMT+8` = **`19:00:52 UTC`**（与信封 `ts=1792090852` 同秒） |

⇒ 窗口 #1 处于设备**静默期**（`18:13`–`~19:00`），**订阅通道本身正常**（窗口 #2 即证明）。

### 4.4 ★ 采证结果（**12 帧 · 2 条记录**）

两条真实 `/log` 记录，**每条各被投递 6 次**，**全部** `qos=1` · **`dup=False`** · `retain=False`：

| 记录 | 信封标识 | 6 次投递时刻（GMT+8） | 间隔序列 |
|---|---|---|---|
| A | `boot_seq=102` · `seq=110347` · `ts=1792090852` | `03:00:52` · `03:01:09` · `03:01:28` · `03:01:52` · `03:02:23` · `03:03:10` | **17 / 19 / 24 / 31 / 47 s** |
| B | `boot_seq=102` · `seq=110348` · `ts=1792090910` | `03:03:25` · `03:03:42` · `03:04:01` · `03:04:24` · `03:04:55` · `03:05:42` | **17 / 19 / 23 / 31 / 47 s** |

（末条 `[END] total 901.0s`；**0 条非 `/log` 消息**，无命令帧、无 telemetry。）

### 4.5 ★★ 由此确定的结论（`O-1` 四类机制判别 —— **首次取得协议级实证**）

| 判别项 | 观测 | 裁定 |
|---|---|---|
| **MQTT 协议层重传**（DUP 标志） | **6 次全部 `dup=False`** | ❌ **排除** —— 协议层重传必然置 `DUP=1` |
| **应用层再次调用 publish** | 6 次**独立 PUBLISH**（各自新报文、`dup=0`）+ 间隔严格为**指数退避阶梯** | ✅ **成立** —— 即固件 `cloud_poll()` 的 ACK 超时重发 |
| EMQX / 下游重复投递 | 同一 `payload` 逐字节相同但**时间戳按阶梯分布**（非同一时刻成簇） | ❌ 排除（下游重复投递不会产生阶梯） |
| QoS | **`qos=1`**（与固件 `cloud_manager.cpp` 注释「4 处调用点均传 `qos=1`」一致） | ✅ **确认** |
| **间隔序列 vs 代码预测** | 实测 **17/19/24/31/47** 与 **17/19/23/31/47** ↔ 代码 `LOG_ACK_TIMEOUT_MS=15000` + 退避 `2/4/8/16/32 s` 推出的 **17/19/23/31/47** | ✅ **逐项吻合**（仅 1 s 调度抖动） |

**★ 与 D1 侧独立互证（同窗口）**：生产 `log_history` `n 33→35` · `max_id 182→194` ⇒ **id 步长恒 6**（`182 → 188 → 194`）= **1 插入 + 5 幂等忽略** ⇒ 与 MQTT 抓包的「每记录 6 次投递」**完全一致**（两条独立证据链互相印证）。
**⇒ `O-1` 的定性最终确认**：**设备侧应用层重传**（既有 `BT-1` 的可观测症状），**非** MQTT 协议层重传、**非** EMQX/下游重复投递。
**置信度：高**（协议字段直接观测 + 间隔量化吻合 + D1 步长互证）。**需人工授权：否**（仅订阅，零发布）。

---

## §5 `V-3.2` / `V-3.8` 证据充分性判断与最小补证

### 5.1 `V-3.2` · 观测 Worker — **证据只满足一半**

**原始判据（任务单 §4.1）**：`wrangler tail` 命中 **`/api/history/ingest`**（**非** `/api/webhook`），**且响应体 `rejected` 计数被逐条记录**（原文强调「HTTP `200` 本身不是通过判据」）。

| 判据分量 | 有无证据 | 依据 |
|---|---|---|
| ① 路径 = `/api/history/ingest`（非 `/api/webhook`） | ✅ **有** | `.pio/tail_v36b.jsonl` 21 事件中 `/api/history/ingest` 命中；`cf-worker: guo-feeder-pagesdev.pages.dev`；`scriptVersion.id = dd5ab824-…` |
| ② **响应体 `rejected` 计数逐条记录** | ❌ **无（通道不存在）** | ① `tail_v36b.jsonl` 的 `event.response` **键集合 = `['status']`** ⇒ **不含 body**；② `src/history-ingest.js` 的 `console.*` **仅 2 处 `console.error`**（`L280` persist 失败 · `L689` revoke-guard 失败）⇒ **成功路径无任何打点** |

**判定**：`V-3.2` = **`PARTIAL`** — 路径分量 **`PASS`**；计数分量在**现有生产通道下不可取证**。
**最小补证（二选一）**：
1. **加性打点**（需改 `src/` + **重新部署** ⇒ 需人工授权）：在 `handleHistoryIngest` 返回前 `console.log(JSON.stringify({url, ...counts}))`；
2. **本地回放**（零生产影响 ⇒ **本轮已具备**）：接受**分层**——「计数语义」由本地层（本报告 §2）证明，「线上路径」由 tail 证明；**须在验收文档中显式写明该拆层**，不得再以 `200` 冒充实测计数。

> 本轮**未**采取方案 1（属部署变更，超出授权）。

### 5.2 `V-3.8` · Presence 回归 — **无有效原始验收记录**

**原始判据**：`$events/client_connected` / `disconnected` 仍 **`200`** **且** 落 **`device_event`**（通道 = tail + D1）。

| 检查项 | 结果 |
|---|---|
| `docs/review/` 内是否存在 **V-3.8 独立验收记录** | ❌ **不存在**（`ls` 仅 3 份 C2 相关记录，无 V-3.8 专档） |
| 任务单 §4.1 的 `V-3.8` 状态 | **`NOT RUN`** |
| 唯一的「`PASS`」声明出处 | `Phase-C-C2-V36-Telemetry-Regression-Verification-Record.md:156` —— 判据写的是「tail：2 × `POST /api/presence/event` `200`」 |
| 该声明的效力 | ❌ **已被二轮复核 `C-5b`（`MAJOR`/`REFUTED`）推翻**：窗口内 `device_event` **零新增**；4 次 presence `200` 全为**非设备客户端**（`presence.js:273–279` 对非设备身份返回 `200 {ignored:"not_device_identity"}`，属**设计正确**） |

**本轮复核（只读，独立取证 · 两次采样）**：生产 `device_event` `n=187 / max_id=216`
—— `2026-10-15 18:48:36 UTC` 与 `19:06:44 UTC` **两次一致**；与二轮复核记录的 `215/216` **一致**
⇒ **`V-3.6` 窗口之后至今无新 presence 事件**（同期 `log_history` 由 `33` 增至 `35`，证明**并非设备无流量**，而是**确实没有 presence 事件**）。
**判定**：`V-3.8` = **`NOT RUN`（无有效证据）**；**明确不得**以 presence `HTTP 200` 作为「设备事件已落库」的证据。

**最小补证**：
1. 设备**真实上下线**时（重启 / 断网 / 重连）开 tail + 前后 D1 快照（单条 SQL 取 `device_event` 的 `COUNT` + `MAX(id)`），要求 **`device_event` 窗口内 `+≥1`** 且该行为**真设备 `device_id`**（非 `not_device_identity`）；
2. 与本轮 MQTT 补采同一窗口进行（一次操作同时补两项）。
**线索（非正式记录）**：`.workbuddy/memory/2026-10-16.md` 记录 Step 2（规则 A 施工复测）期间「3 次重启各 offline/online，`device_event` **+6**」—— 属**真实落库**现象，但**未归档为验收记录**，且该窗口在规则 B 变更**之前**；**不能**替代 `V-3.8` 的独立验收。

---

## §6 `V-3.7b` 待人工核查项 · 所需裁决 · 阻塞原因

### 6.1 前置缺口核对结果

| # | 前置项 | 现状 | 依据 |
|---|---|---|---|
| 1 | **线上 EMQX 规则 A / B 的逐字配置证据** | 🔴 **缺失**（`LIVE CONFIGURATION: NOT VERIFIED`） | ① EMQX Serverless **数据集成 API 全 `403`**（`/rule_engine/*` · `/connectors` · `/actions`）⇒ 只能控制台人工看；② **EMQX 仓无任何 C2 版规则 SQL 存档** —— `grep -rln "guo_feeder/+/log" EMQX_Assets/` = **0 命中**；仓内最接近的 `config/data-integration/P3-WP4C-V2V4-Config-After.md` 记的是 **C2 之前**的 `R3.1` 版本 |
| 2 | **`HR-C2-7a` / `HR-C2-7b` 的正式裁决** | 🔴 **未归档** | 任务单 §7.2 记为「🔴 **待裁**」；`Phase-C-C2-Log-History-End-to-End-Review.md:238` 记为「🔴 待裁」；全仓 `grep HR-C2-7a/7b` **无任何裁决文档**。⚠️ 仅**记忆**记载人工**事实上**采用了黑名单形态（`NOT (topic =~ 'guo_feeder/+/log')`）—— **事实采用 ≠ 归档裁决** |

### 6.2 阻塞原因（为什么 `V-3.7b` 仍不可执行）

1. **`M-1` / `M-6`（静默类）**：在策略 A 下**规则 A 不命中 ∧ 规则 B 按 topic 排除** ⇒ **完全静默**（无 tail 事件、`log_history` +0、`mqtt_messages` +0）。**「无任何证据」本身就是结论** ⇒ **只能证「未污染 legacy 表」，不能证「被拒收」**。要判「是否命中规则」需**控制台 SQL 试跑**能力（未确认 Serverless 是否提供）。
2. **`M-7`（未知 topic 归属）**：结果**取决于 `HR-C2-7a/7b` 的选择**（黑名单 ⇒ 进 `mqtt_messages`；白名单 ⇒ 静默丢弃）。**裁决未归档 ⇒ 判据 `D-3` 无参照物**。
3. **规则 A/B 逐字未核** ⇒ 即便本地证得 Worker 行为，也**无法**把「线上路由」与「候选 SQL」绑定 —— 这正是 `V-3.7b` 的本体。

### 6.3 **人工只读核查清单（可直接照做）**

> 目标：在不改任何配置的前提下，取回「规则 A/B 逐字 SQL + 动作挂载 + 运行统计」，并给出 `HR-C2-7a/7b` 的正式裁决。
> **全部为只读**（打开 → 截图/抄录 → 关闭，不做任何保存）。

**A. 规则 A `r-n302933b-cf0cf2`（`guo_feeder_history_ingest`）**
1. 控制台 → 数据集成 → 规则 → 打开 `r-n302933b-cf0cf2`。
2. 抄录 **SQL 原文**，逐字比对下列**期望值**（任务单 §2.1 候选）：
   ```sql
   SELECT * FROM "guo_feeder/+/up", "guo_feeder/+/log"
   WHERE ( <冻结 telemetry 谓词> ) OR ( <log_predicate: payload_base64/boot_seq/seq/ts/level 五键非空> )
   ```
   * 核对点：① `FROM` **是否真的含两个 topic**（`SYN-4`）；② `WHERE` 是否为 `OR` 合并的**两组括号**；③ `log_predicate` **是否恰好 5 键**（`is_not_null`）。
3. 核对动作挂载：规则下动作 = **`a-n302933b-126ec6`**，URL 路径 = **`/api/history/ingest`**，且**带** `X-Webhook-Key` 头。
4. 抄录该规则的 **运行统计**（触发 / 成功 / 失败 三数）与「已启用」状态。

**B. 规则 B `r-n302933b-f731cf`（`guo_feeder_message_ingest`）**
5. 打开规则，抄录 **SQL 原文**，与下列**期望值**比对：
   ```sql
   SELECT * FROM "guo_feeder/#"
   WHERE ( <NOT 冻结 telemetry 谓词：is_null × 5 或 is_not_null(c)/is_not_null(cmd)> )
     AND ( NOT ( topic =~ 'guo_feeder/+/log' ) )
   ```
   * ★ **重点核对排除子句的形态**：应为 **topic 通配形态**（`'guo_feeder/+/log'`）；
     **若看到 `'^guo_feeder/[^/]+/log$'` 这类正则写法 ⇒ 说明仍是最初的失效版本**（EMQX 的 `=~` 是 **topic 过滤器匹配而非正则**）。
6. 核对动作挂载：动作 = **`a-n302933b-7241e2`**，URL 路径 = **`/`**（webhook 桶，**非** `/api/webhook`）。
7. 抄录运行统计 + 「已启用」。

**C. 裁决**
8. 就 `HR-C2-7a`（黑名单）vs `HR-C2-7b`（白名单）出具**书面裁决**，并归档到 `EMQX_Assets/config/data-integration/`（含规则 ID、SQL 逐字、核查时刻、核查人）。
9. 如确认控制台具备**规则 SQL 试跑/调试**能力，一并记录（这决定 `V-3.7b` 能否走「零污染」通道）。
10. 若走「测试 `device_id` 注入」备选：**须人工批准 + 指定测试 id（禁复用 `d163d0000001/2/3`、`3cfb56c8a5fc`）+ 事后清理授权**（`DELETE` 需单独授权）。

**禁止**：以上任一步骤**不得保存 / 提交 / 启用 / 停用**任何规则或动作；**不得**修改连接器 / ACL / 账号。

---

## §7 失败 / 异常 / 无法验证项

### 7.1 源码与任务单「预期」的差异（按任务要求先报告、未擅自改生产实现）

| # | 位置 | 任务单「预期」 | **源码 / 实测** | 处置 |
|---|---|---|---|---|
| 1 | §4.6 **`M-3`** | `200 {"accepted":0,"rejected":1}`（称 `§2.6` 缺陷） | **`200 {"accepted":1,"duplicate":0,"rejected":0,"filtered":0}` + `log_history +1`** | 任务单预期**写于 `F-C2-8` 修复之前**，已**过时**；实测 = 修复后**正确**行为。**建议回填**（需授权） |
| 2 | §4.6 **`M-2`** 判定 | `BLOCKED`（`HR-C2-8` 致「无法区分因非法 vs 因类型不符被拒」） | 三态已可区分（`level=5/256/"3"` 全 `rejected=1`；`level=0` ⇒ `filtered=1`） | `BLOCKED` 理由**已消解** ⇒ 建议判定升级为「可验证」 |
| 3 | §4.6 `M-4` 源码行号 | 「源码 `L126–131`」 | 实际 **`L217–226`** | 行号漂移（报告性差异） |
| 4 | §4.6 `M-5` 源码行号 | 「源码 `L115–120`」 | 实际 **`L190–195`** | 同上 |
| 5 | §4.6 `M-5` 形态 | 仅列「非 12 位 hex」 | **12 位但大写亦非法**（`^[0-9a-f]{12}$` 区分大小写） | 补充边界（本轮已测 `M-5b`） |

> 上述均为**文档时效性**问题，**未发现**源码缺陷；**未**修改任何生产实现。

### 7.2 本轮**未**完成 / 无法验证项

| # | 项 | 状态 | 原因 |
|---|---|---|---|
| 1 | ~~MQTT `dup`/`qos`~~ | ✅ **已取得（12 帧）** | 窗口 #2 捕获 2 条记录 × 6 次投递（`qos=1` · `dup=False`）⇒ 见 §4.4/§4.5 |
| 2 | `V-3.2` 的 `rejected` 计数 | **不可取证** | tail 无响应体 + Worker 成功路径无打点（§5.1） |
| 3 | `V-3.8` | **`NOT RUN`** | 无有效原始验收记录；窗口内无真设备上下线（§5.2） |
| 4 | `V-3.7b`（`M-1/M-6/M-7`） | **`HUMAN_REQUIRED`** | 规则逐字未核 + `HR-C2-7a/7b` 未裁（§6） |

### 7.3 运行期异常 / 波折（如实记录）

| # | 现象 | 处置 |
|---|---|---|
| 1 | 首跑脚本内一处函数参数错误（`case_run` 误用 `$1_IGNORED`） | **在运行前**由 `bash -n` + 人工复核发现并修正；**未产生错误结果** |
| 2 | 脚本自检项 `grep --remote` 会把**注释中的字面量**也计入 | 改为**拼接式模式**（`'--'"remote"`）避免自匹配；已实测自检 = 0 |
| 3 | `wrangler dev` 首启动 | 3 s 就绪；无网络 / 登录需求；两遍端口分别为 `8795` / `8796` |
| 4 | 生产链路 | 全程**只读**：`wrangler d1 execute --remote` 仅 `SELECT`（`meta.changes` = 0）；**未**部署、**未**改 EMQX、**未**重启设备、**未**发任何业务报文 |
| 5 | MQTT 订阅窗口 #1（300 s）**收到 0 条** | **非工具异常** —— 处于设备静默期（`18:13`–`~19:00 UTC`）；窗口 #2（900 s）于设备恢复后**正常收到 12 帧**，`rc=Success` 两次 ⇒ 通道健康（§4.3） |

---

## §8 最终状态表

| 项 | 状态 | 证据 / 说明 |
|---|---|---|
| **`V-3.7a` · `M-2`** | ✅ **`PASS`** | 3 子例 · `200 {accepted:0,rejected:1}` · D1 +0 · 记录级 |
| **`V-3.7a` · `M-3`** | ✅ **`PASS`** | 5 子例 · `200 {accepted:1}` · D1 +1 · level 1/2/3/4 正确规范化 · `0` → `filtered` |
| **`V-3.7a` · `M-4`** | ✅ **`PASS`** | 3 子例 · `400 {error:"device_id_mismatch"}` · D1 +0 · 批次级 |
| **`V-3.7a` · `M-5`** | ✅ **`PASS`** | 3 子例 · `400 {error:"invalid_device_id"}` · D1 +0 · 批次级 |
| **`V-3.7a` · 幂等（重投）** | ✅ **`PASS`** | `duplicate=1` · 唯一键行数恒 1 · D1 净增 0 · 附带复现 rowid 消耗 |
| **`V-3.7a` 整体** | ✅ **`PASS`（`81/0`，两遍一致）** | **仅证明 Worker 侧语义**，**不**等价于线上 EMQX 路由验证 |
| **`V-3.7b`** | 🔴 **`HUMAN_REQUIRED`** | 规则 A/B 逐字未核 + `HR-C2-7a/7b` 未裁（§6） |
| **MQTT `dup` / `qos`** | ✅ **`CONFIRMED`（12 帧）** | `qos=1` · **`dup=False`** × 6/记录 ⇒ **应用层重发**（非协议层重传）· 间隔 `17/19/23·24/31/47 s` 与代码**逐项吻合**（§4.5） |
| **`O-1` 根因（四类判别）** | ✅ **`CONFIRMED`（升级为协议级实证）** | MQTT 字段 + 间隔量化 + D1 `id` 步长 6 三重互证（§4.5） |
| **`V-3.2`** | 🟡 **`PARTIAL`** | 路径分量 `PASS`；`rejected` 计数**通道不存在** ⇒ 需**修订判据**或**补通道** |
| **`V-3.8`** | ⚪ **`NOT RUN`** | 无有效原始验收记录；**禁止**以 presence `200` 冒充落库证据 |
| **`M-1` / `M-6`** | ⚪ **`NOT RUN`**（归 `V-3.7b`） | 静默类，需控制台 SQL 试跑 |
| **`M-7`** | ⚪ **`BLOCKED`**（归 `V-3.7b`） | 依赖 `HR-C2-7a/7b` 裁决 |
| 生产 / 设备 / EMQX / 部署 | ✅ **零改动** | 生产 D1 仅 `SELECT`；设备未重启；EMQX 未触碰 |

> **严格性声明**：本表**未**把 `UNKNOWN` / `NOT RUN` / `BLOCKED`  / `HUMAN_REQUIRED` 折算为 `PASS`。

---

## §9 待办（**均需人工授权，本轮未执行**）

1. **回填任务单 §4.6**：`M-2`/`M-3` 的预期与判定按 §7.1 更新（`M-3` 预期已过时）；`M-4`/`M-5` 行号更新。
2. **`HR-C2-7a/7b` 正式裁决 + 归档**，连同**规则 A/B 逐字 SQL 存档**（§6.3 清单的执行产物）。
3. **`V-3.2` 判据修订**（拆分「路径」与「计数」两层）或**授权加性打点 + 部署**。
4. **`V-3.8` 真机取证**（设备真实上下线窗口）。
5. ~~**MQTT `dup/qos` 补采**~~ ✅ **本轮已完成**（窗口 #2 · 12 帧 · §4）。
6. 可选：把 `v37a_log_reject_matrix.sh` 登记进 `test/real_d1/README.md`。
7. 可选：把 §4.5 的 `dup/qos` 实证**回填**二轮复核报告的 `O-1`/`D-3` 段（该段此前标 `UNKNOWN`）。
8. **三仓提交裁决**：固件 `wb` 领先 `origin/main` **8 提交**（+ 4 M 文档 + 3 未提交报告）· 云端 `main` **1 未跟踪文件**（本轮产物）· 消息仓 clean。

---

## §10 本轮边界声明

**已做**：只读读取 Worker / 固件源码与任务单 · 新增 1 个本地测试夹具（`test/real_d1/v37a_log_reject_matrix.sh`）· 在**本地隔离** Worker + 本地 D1 上执行 `V-3.7a`（两遍）· **只读订阅** MQTT（2 窗口 · 300 s + 900 s · **收到 12 帧** · 补采 `qos/dup/retain` · **未发布任何报文**）· 生产 D1 `SELECT` · EMQX `GET /clients`（只读）· 生成本报告。

**未做**：未修改生产代码 · 未部署 Worker / Pages · 未修改 EMQX 规则 / 连接器 / Action / ACL / 账号 · **未**向生产 `/log` 发布任何报文 · 未对生产 D1 执行 `INSERT/UPDATE/DELETE` · 未更改设备配置 / 未触发保存或重启 · 未清理历史数据 · 未修改正式验收矩阵 / 任务单 / 正式审阅记录 · 未 commit / push / reset / checkout。

---

*本报告为 `2026-10-16` `V-3.7a` 实施与证据收紧产物，**等待人工审阅**。未获授权前不启动任何回填、修复、部署或生产操作。*
