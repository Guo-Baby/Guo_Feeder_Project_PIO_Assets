# G4 · 范围裁决与真机前 Backlog（`2026-10-21`）

- 裁决人：用户（`02:34`）
- 记录目的：把"做什么 / 不做什么"固化，避免后续会话反复讨论；同时记录 **3 处按字面执行会造成严重后果**的技术点。
- 现状基线：C-1 = `PASS`（`0007` 已应用）· C-2 = `BLOCKED`（Worker `d7fcd4f3-…` 已上线，真实流量为 0）

---

## 0. 一句话

三仓范围已收敛：**A 类全做（可真机前完成）、B 类只写代码不做验收、C 类整体放弃、D 类按裁决落值**。
⇒ **UI 不被真机阻塞**（只依赖 A-1/A-2）。
但裁决中有 3 项按字面执行会造成「阈值永远不通过 / 作废刚上线的水位机制 / 无收益高风险」，见 §3。

---

## 1. 裁决记录

### 1.1 A 类（批次 0）—— **全部做，一次性补齐**

| # | 工作 | 裁决 | 落地 |
|---|---|---|---|
| A-1 | 历史读端点 | ✅ 做 | §4.1 |
| A-2 | 设备快照端点 | ✅ 做 | §4.2 |
| A-3 | `heartbeat` + `fw_version` | ✅ 做 | §4.3 |
| A-4 | 测试补齐 | ✅ 做 | §4.4 |

### 1.2 B 类（需要设备验收）—— **只写代码 / 只等烧录**

| # | 工作 | 裁决 | 说明 |
|---|---|---|---|
| B-1 | Homie 启用 | ⏸ **不改代码**，等真机 | 代码已 2641 行、`GF_HOMIE_ENABLED=0`；设备连好后烧录实测 |
| B-2 | `G4-R7` / 注释等小项 | ✅ 代码可先改 | 随下次烧录一起验 |
| **B-3** | **OLED 本地 UI** | ❌ **整体放弃（本次范围外）** | 定位为**未来的本地化操作 UI**，不影响"云端下发命令"主链路。**只保留"日志上云"接口**（`log` 模块已有，无需改） |
| B-4 | `WP-5` 前置 | ⏸ 等 `G-4` | 见 §3.2 |

### 1.3 C 类（EMQX 侧鉴权/身份）—— **整体放弃**

裁决理由（用户）：EMQX Serverless 免费版 API 受限；设备为**私人本地化部署**，同 broker 设备数 ≤ 5–10，broker 不公开；保存好地址与明文账号密码即可接受。

| # | 工作 | 裁决 | 放弃后的后果（必须记录） |
|---|---|---|---|
| C-1 | `webhook` 鉴权（`C2-SEC-WEBHOOK-NOAUTH`） | ❌ 不做 | **公网任何人只要知道 `guo-feeder-pagesdev.pages.dev` 即可向 `mqtt_messages` 写任意行**。这不是"broker 鉴权"问题，见 §3.3 |
| C-2 | `B-3` 漏记取证 | ❌ 不做 | **漏记率永远无法量化**；只能保留 `I-6`/`I-7` 的不等式近似 |
| C-3 | P1 认证（设备账号 CSV / APP slot / ACL） | ❌ 不做 | 设备用**固定用户名密码**；**无 APP 用户体系**（UI 只能"管理员单令牌"）；**ACL 不收窄** ⇒ 同 broker 设备间可互相订阅 |

### 1.4 D 类（人工裁决）

| # | 事项 | 裁决 | 备注 |
|---|---|---|---|
| D-1 | `G4-R4` DRAM 阈值 | **85%**（`2026-10-21 03:00` 最终裁决；不可用再放宽 **90%**） | ✅ 已落地，见 `G-4-Gate-Revision-Record-2026-10-20.md` **R-3** |
| D-2 | `SEQ-SEM`（`seq` 口径） | ✅ **保持跨 boot 连续，只改文档不改代码**（`2026-10-21 03:00` 最终裁决，推翻本表初版"先采用 boot 内"） | ✅ 已落地，见 `log_envelope_contract.md` **§2.0** |
| D-3 | `S-3-O3-d` OLED 方案 | ❌ 不做（随 B-3 一并放弃） | 未来再设计 |
| D-4 | `C2-SEC` 改法 | ❌ 不做（同 C-1） | — |

---

## 2. 裁决后的能力边界（任何对外说法都必须遵守）

**可以宣称**

- 设备 → 云端 → D1 主链路完整：`/api/ingest`、`/api/history/ingest`、`/api/presence/event` 均在线
- 去重可用：`dedup_key` + 部分唯一索引 + `DO NOTHING`（合成样本已证；真实流量待复验）
- 云端下发命令可用（既有链路）
- **UI 可只读展示**（做完 A-1/A-2 后）

**不可宣称**

- ❌ "鉴权完整" —— 写入入口有一段是公开的（§3.3）
- ❌ "多租户 / 多设备隔离安全" —— ACL 未收窄
- ❌ "漏记率已量化" —— `B-3` 取证放弃
- ❌ "OLED 本地 UI 已完成" —— 本次范围外
- ❌ "三仓编码完成" —— 见 §5

**已接受的风险（3 条，须长期记住）**

| ID | 风险 | 缓解 |
|---|---|---|
| `ACC-1` | 公网可写 `mqtt_messages`（`/api/webhook` → Worker `/`） | 把 `pages.dev` 域名当密码；不截图不外传；定期查异常写入 |
| `ACC-2` | 同 broker 设备间可互相订阅/冒充 | 设备数 ≤ 5–10 且私人；每设备独立用户名密码；发现异常立即改密 |
| `ACC-3` | 无 APP 用户体系 ⇒ UI 只能单令牌 | 前端**绝不**内置 `ADMIN_API_TOKEN`；用 Cloudflare Access 或服务端代理 |

---

## 3. 三处必须修正的技术点（附实测证据）

### 3.1 `D-2`：建议**改文档，不改代码**（与裁决方向相反）

**实测事实**

- 实现是**跨 boot 连续**：`src/log/log_manager.cpp:1975` `const uint32_t next_seq = s_meta_slot_seq + 1;`
  —— `s_meta_slot_seq` 持久化在 A/B 双 meta 槽，**重启不回零**。
- `log_boot_watermark` 表 **12 列全部为"跨 boot 连续 + 回绕检测"设计**：
  `max_boot_seq` / `max_seq_at_max` / `rewind_count` / `last_rewind_boot` / `epoch` / `prev_epoch_max` /
  `observed_max_boot_seq` / `ambiguous_count` / `last_rebase_id` …
- 该表 + `LOG_BOOT_WATERMARK="1"` 是 `0005` 刚上线并已验证的机制（`rewind` 检测依赖 seq 单调）。

**若按"boot 内"执行（改实现）**

1. `log_boot_watermark` 的 `max_seq_at_max` / `rewind_count` / `epoch` 全部失去语义 ⇒ **`0005` 机制作废**；
2. `log_history` 现有 **817 行**是按跨 boot 连续写入的 ⇒ 新旧数据口径断裂；
3. `BT-1` 刚验证通过的日志链路要**重新验收**；
4. 收益：零（boot 内序号是跨 boot 连续的子集，无任何新能力）。

**建议**：保持实现不变，把 `docs/interfaces/log_envelope_contract.md:58` 的措辞改成"**跨 boot 连续**"。
⇒ **一步到位、零代码改动、零重验**。若你坚持改实现，请单独授权，并接受 1–3 条后果。

### 3.2 `D-1`：70% 在当前口径下**空载即 FAIL**

**实测事实**

- 分母是 `internal.total`（≈ 251 KB），不是 SRAM 327 680；
- 指标用 `minimum_free`（**单向低水位**，一旦某刻低过就永远低）；
- **设备空载就已是 77.08%**。

**后果**：阈值设 70% ⇒ `G-4` **永远不可能通过**，不是"测试中看看会不会超"。
设 80% 也危险（空载 77%，业务峰值很容易破 80%）。

**建议（二选一）**

- **方案甲（推荐）**：先修口径 —— 分母固定 `internal.total`，指标改用**当前** `free_pct`（不用 `minimum_free`），阈值取 **85%**。修完再谈"是否收紧要 80%"。
- **方案乙**：阈值直接取 **85%**，口径不改 ⇒ 但 `minimum_free` 仍会随时间单向恶化，长期必然触发。

★ 无论选哪个，**必须先把 `minimum_free` 换成当前 free**，否则阈值大小只是"什么时候必然失败"的差别。

### 3.3 `C-1`：这是"公网可写端点"，不是"EMQX 鉴权"

**实测事实**

- `GET https://guo-feeder-pagesdev.pages.dev/api/webhook` → `200 MQTT Router OK`（公网可达）；
- `functions/api/webhook.js` 转发到 Worker **根路径**，**不带** `X-Webhook-Key`；
- Worker 根路径的鉴权块被 `/* */` 注释掉；
- 本次 C-2 的探针正是走这条路写进了 6 行（`G4PROBE-`）。

**关键区别**：`ACC-1` 与 broker 是否公开**无关**。攻击者不需要 broker，只需要这个 Pages 域名。
放弃 C-1 是合理取舍，但**不能写成"我们鉴权做过了"**，只能写"风险已知并接受"。

**零代码缓解（可选，不碰 EMQX）**：把该路径加入 Cloudflare WAF 速率限制 / 只允许 POST / 拒绝浏览器 UA。
这拦不住定向攻击，但能挡住扫描器。

---

## 4. A 类执行清单（文件级）

★ **A 类全部零 migration**：`device.fw_version` 列已存在；`telemetry_history` / `log_history` 索引齐备
（`ix_th_device_ts`、`ix_th_device_type_ts`、`ix_lh_device_ts`、`ux_th_dedup`、`ux_lh_dedup`）。

### 4.1 A-1 历史读端点

| 项 | 内容 |
|---|---|
| 路由 | `GET /api/history/telemetry?device_id=&type=&from=&to=&limit=`<br>`GET /api/history/log?device_id=&boot_seq=&from=&to=&limit=` |
| 鉴权 | `ADMIN_API_TOKEN`（Bearer），复用 `authorizeAdmin` |
| 文件 | `guo-feeder-api/src/index.js`（+ 如需，抽 `src/history-query.js`） |
| 索引 | telemetry 走 `ix_th_device_type_ts` / `ix_th_device_ts`；log 走 `ix_lh_device_ts` |
| 约束 | `limit ≤ 500`，默认 100；`to` 缺省 = now；响应含 `next_before` 游标 |
| 验收 | 本地 `wrangler dev` + 真 D1 只读回放；`EXPLAIN QUERY PLAN` 必须 `SEARCH … USING INDEX` |

### 4.2 A-2 设备快照端点

| 项 | 内容 |
|---|---|
| 路由 | `GET /api/admin/device/:id/snapshot` |
| 返回 | `device` 行（含 `fw_version`/`last_seen`/`state`）+ 最近 N 条 telemetry + 最近 log 水位（`log_boot_watermark`）+ 派生在线态 |
| 文件 | `guo-feeder-api/src/index.js` |
| 验收 | 同上；空数据设备也须返回 200（不得 500） |

### 4.3 A-3 `heartbeat` + `fw_version`

| 侧 | 内容 |
|---|---|
| 固件 | 走 **`/up` 业务通道**（`{"c":"heartbeat","i":"<新id>","p":{"up":<ms>,"fw":"<ver>","heap":<n>,"rssi":<n>}}`），**60 s 一次、仅 MQTT 连接态发送** |
| ★ 关键 | **不得**走 `log` 通道 —— 否则每条 heartbeat 占一个 `seq`，污染 `I-3`/`SEQ-HOLE` 判据 |
| 云端 | `/up` 分支识别 `c==heartbeat` ⇒ 更新 `device.fw_version`（列已存在）+ `last_seen`；可选写 `telemetry_history(type='heartbeat')` |
| 文件 | 固件 `src/cloud/`；Worker `src/index.js` |
| 验收 | 固件主机单测（40 套不回归）+ Worker 单测；`mqtt_messages` 会多普通 up 行，属预期 |

### 4.4 A-4 测试

新增用例：两读端点的鉴权/分页/空结果/非法参数；heartbeat 解析与 `fw_version` 落库。

---

## 5. 门禁与状态（裁决**不改变**门禁）

| 门禁 | 状态 | 说明 |
|---|---|---|
| `G-4` | **`BLOCKED`** | 需 §3.2 口径修正 + 阈值生效 + 2h+30min 窗口 |
| `WP-5` | **未解锁** | 依赖 `G-4` |
| `BT-1` | `CLOSED` | 未受影响 |
| `STOP-R1` | **无解除文件** | 未受影响 |

★ 本次裁决**不放宽任何已通过的门禁**，也不构成 `G-4`/`WP-5` 的解锁证据。
★ `C-2` 仍为 `BLOCKED`（`P-1'/P-1''/P-3'` 真实流量 `NOT RUN`，闭合见 C-2 报告附录 A）。

---

## 6. 推荐顺序

```
A-1 历史读端点 ──┐
A-2 快照端点   ──┼→ 只读看板（不被真机阻塞）
A-3 heartbeat  ──┘
A-4 测试
        ↓
B-2 小固件项（代码）→ 等烧录
        ↓
真机批次：G-4 窗口 → WP-5 → Homie 启用 → C-2 附录 A → C-3
```

`B-3`(OLED) / `C-1..C-3` / `D-3` / `D-4` **不在本轮范围**。
