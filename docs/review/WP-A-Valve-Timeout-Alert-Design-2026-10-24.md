# WP-A 阀门安全超时告警 —— 最小设计与协议兼容性判定

> 日期：2026-10-24 · 依据：用户第四轮之后追加任务「阀门超时告警 → UI 开发」WP-A
> 检查范围：固件 `src/app/valve.cpp` · `src/log/*` · Worker `history-ingest.js` / `read-api.js` · D1 `log_history` · UI 读取路径
> 性质：**设计判定 + 实现规格**。本文所有结论以仓库当前代码与生产 D1 实测为准，不引用旧报告推测。

---

## 1. A1 只读检查结论（现状）

### 1.1 固件侧（已具备，本轮**零改动**）

`src/app/valve.cpp` `valve_task()`（`:360-395`）：

```
超时判定（now - open_start_time >= safety_timeout_ms）
  → ① Serial 打印
  → ② valve_set_gpio(false)          ← 本地关阀，同步 GPIO，最先完成
  → ③ event_push(EVENT_VALVE_ERROR, "Safety timeout", …)
  → ④ log_emit(LOG_VALVE_SAFETY_TIMEOUT, WARN,
               {LOG_P_OPEN_MS=实际开启 ms, LOG_P_LIMIT_MS=配置阈值 ms})
       （受 safety_timeout_reported 一次性锁保护：一次真实超时 = 一条记录）
```

- `LOG_VALVE_SAFETY_TIMEOUT = 0x0508`（`src/log/log_events.h:287`，WARN，flash+cloud 双开）。
- `LOG_P_OPEN_MS = 0x13` / `LOG_P_LIMIT_MS = 0x14` 均为**既有** ParamId（`:130-131`）。
- `log_emit()` 写 RAM 环后立即返回（`log_manager.h:7` 注释：`log_task()` 在 loop 消费）⇒ **告警构造/发送/重试不阻塞关阀**。
- 一次性锁在「真正开启 / 真正关闭 / force_close 成功」时解除 ⇒ 一次真实超时只产生一条记录；重复开阀再次超时会产生**新 seq 的新记录**（正确）。

### 1.2 传输与云端（已具备）

- 通道：`guo_feeder/<id>/log`（QoS1，store=0；耐久性由设备 Flash 段环保证）。
- Worker `/api/history/ingest`：`INSERT OR IGNORE INTO log_history`，唯一键 `(device_id, boot_seq, seq)`（索引 `ux_lh_dedup`）⇒ **重复投递被幂等吸收**；内容冲突记 `log_conflict`（既有机制）。
- `log_history` 列：`id, device_id, ts, level, code, boot_seq, seq, message, payload, created_at`。
- ⚠️ **`code` / `message` 恒为 NULL**（envelope 冻结 6 字段不含二者；`docs/ui-v1/07-log-decode.md` §E.3-1 已登记）⇒ 结构化内容只在 `payload`（base64 CBOR），而 Worker **禁止解码 CBOR**（契约 §8.2.2「Worker remains CBOR-blind」）。

### 1.3 UI 读取路径（现状缺口）

- `GET /api/history/log`（Pages 同源代理 `/api/read/history/log`）已返回 `payload`，过滤器只有 `device_id/level/from/to/before_id/limit`，**无解码能力**。
- `app.html` 是只读看板；`gf-api.js`（会话/目录/命令/四态）已建成但**尚无页面引用**。
- ⇒ 告警数据已在 D1，但**没有任何界面能把 0x0508 记录渲染成"阀门安全超时告警"**。这就是 WP-A 要实现缺口。

### 1.4 真实生产样本（黄金用例，只读取证）

扫描生产 `log_history` 最近 3000 条 WARN+ 记录（脚本 `Cloudflare_Assets/scripts/scan_valve_timeout_samples.py`）：

```
id=8682  ts=1792767676  boot_seq=83  seq=23323  level=WARN
  event_id = 0x0508 (LOG_VALVE_SAFETY_TIMEOUT)
  params: LOG_P_OPEN_MS=60006, LOG_P_LIMIT_MS=60000
```

即第三轮 60 s 超时实测的**端到端真实产物**（设备 → MQTT /log → EMQX → Worker → D1）。
样本 payload 存档：`Cloudflare_Assets/backups/sample-0508.txt`。

同窗事件分布（佐证 HX711 假阳性噪声规模，属遗留 #6，不在本轮范围）：
`0x0511×1795`（DispenseGuard 响应）· `0x0505×952`（force_close）· `0x0508×1`（阀门超时）。

---

## 2. 协议兼容性判定（A2.6）

**结论：无需任何协议扩展，无需提交协议差异。**

| A2.6 关注点 | 判定 |
|---|---|
| 新 `device_event.type` | **不引入**。该表 CHECK 约束 13 类生命周期事件，与设备运行时告警无关；告警不走此表 |
| 新 `EventId` | **不引入**。复用既有 `LOG_VALVE_SAFETY_TIMEOUT=0x0508`（P2-H 已分配） |
| 新 `ParamId` | **不引入**。复用既有 `LOG_P_OPEN_MS` / `LOG_P_LIMIT_MS` |
| 新 topic / ACL / 协议版本 | **不引入**。走既有 `/log` 通道 |
| 新 DDL / 迁移 | **不引入**。`log_history` 现有列足够 |
| envelope 改动 | **不做**。冻结 6 字段一字不动（不补 code/message —— 那是另一条增强路线，见 §6） |

依据：`docs/ui-v1/07-log-decode.md` 已裁定「UI **必须**解码 payload，不能读 message」；本轮把该设计在 Worker/UI 侧落地，属于**实现既有设计**，不是新协议。

---

## 3. 最小设计（实现规格）

### 3.1 分层职责

```
设备（零改动）                云端（新增解码）                UI（新增展示）
0x0508 + OPEN/LIMIT  →  /log → ingest（零改动，      →  /api/read/history/log?decode=1
关阀先于一切                CBOR-blind 原样落库）              → 告警区渲染
```

### 3.2 云端新增（Worker，零迁移）

1. **`scripts/gen-log-codes.mjs`**：从固件 `src/log/log_events.h` 生成
   `guo-feeder-api/src/log-codes.generated.js`：
   - `EVENTS[id] = {name, module, level}`（module 由 §5 段式前缀表推导；level 取枚举行尾注释）
   - `PARAMS[id] = {name}`；`LEVELS[0..4]`；`SOURCE_SHA = log_events.h 的 sha256`
   - **可重复**：同输入必须产出逐字节相同输出（T-1）。
2. **`read-api.js` `handleHistoryLog` 增加 `decode=1`**（规格 = 07 §6.1/6.2/6.3）：
   - 每项附加 `decoded = {ok, event:{id,name,module,level}, params[], time_source, warnings}`
   - 降级规则逐条实现（cbor_error / short_record / unknown_event / blob_overflow / crc_mismatch / unsupported_version），**保留 payload 原文**，不抛 500
   - 缺省 `decode` ≠ 1 ⇒ 老行为逐字节不变（看板不受影响）
   - 时间：记录 `ts=0` 或 `flags bit0=0` ⇒ 回退 `created_at`（秒），输出毫秒
   - ⚠️ `log_history` 无 `module` 列 ⇒ 禁止 SQL 按 module 过滤（07 §6.4）

### 3.3 UI 新增（见 WP-B，先记接口依赖）

- 记录页「告警」视图：拉 `decode=1` 日志，前端按 `decoded.event.id === "0x0508"` 过滤渲染。
- 每条告警展示：设备 ID · 时间（time_source 可信时）· `boot_seq/seq`（事件唯一标识）· 事件名 · **实际开启时长**（OPEN_MS）· **配置阈值**（LIMIT_MS）。
- **与命令执行结果物理分区**：告警来自 `log_history`，命令终态来自 `command_audit`；UI 不得把告警显示为原 `VALVE_OPEN` 命令的失败结果（A2.2）。

---

## 4. 失败与边界行为（A2.4 / A2.7 / A2.8，如实描述，不承诺更多）

| 场景 | 行为（现有机制，本轮不改） | 明确不承诺 |
|---|---|---|
| 本地关阀 | `valve_set_gpio(false)` 在 log_emit **之前**同步完成 | 不依赖任何云端成功 |
| 设备离线 | 记录留 Flash 段环（**硬上限 496 条**），上线后重投；环满记 `drop_ring` 等侧信道计数 | 不承诺无限期缓存 |
| 发送失败 | QoS1 + 未收到 `log_ack` 前有界重投（500 ms 节流，≤2 msg/s） | 不承诺 exactly-once |
| 重复投递 | `(device_id, boot_seq, seq)` 唯一键 `INSERT OR IGNORE`；同内容 = 幂等重放，异内容 = `log_conflict` 候选碰撞 | 不承诺异内容也被保留（首行赢） |
| Worker 处理失败 | 4xx 不重投 / 5xx 重投（EMQX 数据集成）；设备侧表现为未 ACK ⇒ 继续重投 | 不承诺 Worker 侧重试 |
| 告警链路整体失败 | 只丢**可观测性**，不影响关阀与后续安全流程（A2.8） | — |
| 告警 vs 命令状态 | 两表物理分离，永不互相覆盖；幂等由唯一键保证（A2.7） | — |

---

## 5. 验收口径（A3，映射到实现）

| # | 验收项 | 验证方式 |
|---|---|---|
| V1 | 关阀不依赖云端 | 代码路径审查（关阀先于 log_emit）+ 第三轮真机 60 s 实测记录（§1.4 样本即产物） |
| V2 | 正常开阀不误报超时 | 代码路径审查（阈值判定 + 一次性锁）；生产样本中 0x0508 仅 1 条且参数自洽（60006>60000） |
| V3 | 告警不覆盖原命令终态 | 单测：0x0508 ingest 后 command_audit 零变化；UI 分区渲染 |
| V4 | 重复上报不产生重复有效记录 | 单测：同 (device,boot,seq) 二次 ingest ⇒ 仍 1 行（复用既有 `ux_lh_dedup` 行为） |
| V5 | 云端拒绝/断连/失败不影响关阀 | 代码路径审查 + 设备侧重投语义文档化；**真机断链重投不重测**（第三轮已覆盖链路） |
| V6 | 既有回归不破坏 | `tests/api` + `tests/pages` 全量 + `decode` 缺省行为逐字节不变的单测 |
| V7 | 真实样本解码 | 黄金样本 id=8682 解码断言（event/level/params/时间源）——真实数据，非构造 |

硬件依赖：**无**（V1/V2 的真机部分已有第三轮证据；不重跑阀门动作）。

---

## 6. 记录在案但**不做**的增强（避免范围蔓延）

- **envelope 补 `code`/`message`**：能让 SQL 直接过滤、看板零解码显示事件名。但改冻结 envelope 形状 ⇒ 需协议差异评审，且 Worker 侧 `normalizeLog` 虽已预留这两个可选字段，固件信封生成器要同步改。**本轮不做**，留作 UI v1.2 候选。
- 告警独立 push/短信等外部通道：架构外，不做。

---

## 7. 涉及文件（预告）

| 仓 | 文件 | 动作 |
|---|---|---|
| Cloudflare_Assets | `scripts/gen-log-codes.mjs` | 新增 |
| Cloudflare_Assets | `guo-feeder-api/src/log-codes.generated.js` | 新增（生成物） |
| Cloudflare_Assets | `guo-feeder-api/src/read-api.js` | 改（decode=1） |
| Cloudflare_Assets | `tests/api/log_decode.test.mjs` | 新增 |
| Cloudflare_Assets | `guo-feeder-pagesdev/*` | 改（WP-B 页面） |
| Cloudflare_Assets | `backups/sample-0508.txt` | 新增（黄金样本，本地） |
| Guo_Feeder_Project | `docs/review/WP-A-…md` | 新增（设计+报告） |
| Guo_Feeder_Project | `.workbuddy/memory/2026-10-24.md` | 追加 |

**固件源码：零改动。**
