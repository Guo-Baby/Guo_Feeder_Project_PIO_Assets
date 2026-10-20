# 阶段 2 报告：B-3（`mqtt_messages` 漏记）EMQX 只读取证

- 采集日期：`2026-10-21`（`00:30:18`–`00:35:40`，本机时间 UTC+8）
- 性质：**只读**。未改 Rule / Connector / 凭据 / ACL / QoS / 主题 / 任何配置；**未发布任何测试消息**
- 证据目录：`docs/evidence/G4-B3-EMQX-2026-10-21/`
- 结论：**部分完成** —— Broker 侧可读指标已取全；**规则 / Action / Connector 指标 403**，
  控制台截图**未取得**（无 Dashboard 登录态），已给出逐项人工取证清单。
  **B-3 根因仍未定**，且本轮发现两条**修正既有假设**的新事实（§4）。

---

## 1. 实例、环境、权限

| 项 | 值 | 来源 |
|---|---|---|
| 部署形态 | EMQX **Serverless** | `EMQX_Assets/README.md:100` |
| API base | `https://n302933b.ala.cn-hangzhou.emqxsl.cn:8443/api/v5` | `EMQX_Assets/.env`（非敏感） |
| 节点 | `emqxsl-prod@10.76.117.57` | `/clients/dev_…` 的 `node` |
| 监听器 | `tcp:default`（MQTT over TCP） | 同上 `listener` |
| 租户来源 | `tenant_id_from = peersni`（SNI 决定租户，非用户名） | 同上 |
| 鉴权 | HTTP Basic，`EMQX_APP_ID`:`EMQX_APP_SECRET` | `EMQX_Assets/.env` |
| 时区 | API 时间为 **UTC**（`+00:00`）；D1 `created_at` 为 epoch **毫秒** | 实测 |

### 1.1 权限边界（本轮逐端点实测）

| 端点 | HTTP | 用途 |
|---|---|---|
| `/clients?limit=N` · `/clients/{id}` | **200** | ✅ 会话级指标 |
| `/subscriptions?limit=N` | **200** | ✅ 订阅关系 |
| `/status` · `/stats` · `/nodes` | **403** | ❌ 全局收发总量 |
| `/rules` | **403** | ❌ Rule matched / failed |
| `/actions` | **403** | ❌ Action 重试 / 队列 / 丢弃 |
| `/connectors` | **403** | ❌ Connector 成功率 / 超时 |
| `/sources` · `/monitor_current` | **403** | ❌ 历史监控曲线 |

★ 403 的 body **为空**（0 B），与"路径不存在"（404/JSON 错误）不同 ⇒ 属**授权层拒绝**，不是接口缺失。
★ 本机**无 EMQX Dashboard 登录态** ⇒ **无法截图**；按任务要求改为人工取证清单（§6），**未尝试绕过权限**。

证据：`s2-endpoint-probe.txt`

---

## 2. Broker 侧事实（设备当前在线）

`GET /clients/dev_288485896ce4`（`s2-device-client-detail.json`）：

| 字段 | 值 | 含义 |
|---|---|---|
| `clientid` / `username` | `dev_288485896ce4` / `GuoFeederDevice` | ★ 两者**不同** |
| `connected` / `connected_at` | `true` / `2026-10-20T15:22:26.468Z`（`10-20 23:22:26 +08`） | **本会话**起点 |
| `proto_ver` / `keepalive` | MQTT **4**（3.1.1） / 120 s | |
| `clean_start` / `expiry_interval` | `true` / `0` | 无持久会话 ⇒ 断开即清，**计数按会话归零** |
| **`recv_msg`** | **35** | ★ Broker 从设备收到的 **PUBLISH** 数 |
| `recv_pkt` / `recv_cnt` | 113 / 116 | **报文数**（含 PINGREQ/PUBACK）—— **不是消息数** |
| **`send_msg`** | **16** | Broker 发给设备的 PUBLISH |
| **`recv_msg.dropped`** | **0** | Broker 未丢弃入站消息 |
| `send_msg.dropped`（`.expired` / `.queue_full` / `.too_large`） | **0 / 0 / 0** | |
| `mqueue_dropped` / `mqueue_len` | **0** / 0 | 无积压、无丢弃 |
| `inflight_cnt` / `inflight_max` | 0 / 32 | 无在途 |
| `subscriptions_cnt` | 1 | 只订阅自己的下行主题 |

★★ **语义陷阱**：`recv_cnt = 116` 是**报文数**，`recv_msg = 35` 才是**消息数**。
用 116 对账会把 PINGREQ/PUBACK 也算进去，直接得出"丢了 80%"的错误结论。

其它客户端（`s2-clients.json`）：`phone/shouji`（在线，测试用）、`test001`、`dev_d163d0000001`、
`dev_deadbeef0001`（离线）。**除 `phone` 外无其它在线客户端**。

---

## 3. 与 D1 的同一窗口对照（**会话锚定**）

窗口 = `[connected_at, 查询时刻]`，约 70 min。因 `clean_start=true` + `expiry_interval=0`，
**会话计数天然从该时刻起算**，不需要估时。

| 层 | 指标 | 数值 | 来源 |
|---|---|---|---|
| Broker 收（设备→） | `recv_msg` | **35** | EMQX `/clients/dev_…` |
| Broker 发（→设备） | `send_msg` | **16** | 同上 |
| D1 `mqtt_messages` | `audit` + `up` | **4** | `s2-d1-window-counts.txt` |
| D1 `mqtt_messages` | `echo` + `down` | **16** | 同上 |
| D1 `log_history` | 行数 / 唯一 `seq` | **14 / 14**，`seq 6145…6158`，`created_at 15:22:27Z–15:29:18Z` | 同上 |
| D1 `mqtt_messages` | 其它组合（`audit/down`、`echo/up`、`unknown`） | **0 / 0 / 0** | 同上 |

### 3.1 可判定的两条

**① 下行 16 ↔ 16 一一对应（但只有 `echo` 一份）**

- `send_msg = 16`（Broker 发给设备的 PUBLISH）
- D1 中 `echo`（`$tenants/n302933b/guo_feeder/288485896ce4/down`）**16 行**
- 设备只订阅 1 个下行主题 ⇒ 这 16 条就是全部下行 ⇒ **下行链路在本窗口没有丢**

**② Broker 会话层零丢弃**

`recv_msg.dropped = 0`、`mqueue_dropped = 0`、`send_msg.dropped.* = 0`、`inflight_cnt = 0`
⇒ 若最终少了消息，**不是** Broker 会话队列/在途/超限造成的。

### 3.2 不可判定的差额

设备上报 35 条 PUBLISH，D1 里能对上的只有 **4（`up`）** + **14（`log`，走 `log_history`）** = 18 条，
**差额 17 条无法分层**：

| 候选原因 | 为什么现在分不了 |
|---|---|
| (a) Rule SQL 未匹配该主题 | `/rules` **403** |
| (b) Action 未转发 / 队列丢弃 / 重试耗尽 | `/actions`、`/connectors` **403** |
| (c) Worker 收到但未写库（5xx / 类型错误 / `invalid_device_id`） | Worker Logs **403**（`tail` 本轮**未**取） |
| (d) QoS 重传被计入 `recv_msg` | `clean_start=true`，重传在 MQTT 4 下完全可能；无 per-topic 分解 |

★ **不得**把这 17 条写成"漏记 17 条 / 漏记率 48%"：分母 35 混着 `up` 与 `log`，
而 `mqtt_messages` **按设计只收 `up`**（`log` 走 `/api/history/ingest`，**不写** `mqtt_messages`）
⇒ 35 与 4 **本来就不是同一口径**。

---

## 4. ★★ 两条修正既有假设的新事实

### 4.1 `echo`（`$tenants` 副本）**只存在于 `down` 方向**

```sql
SELECT direction, COUNT(*), MAX(created_at) FROM mqtt_messages
 WHERE substr(topic,1,9)='$tenants/' GROUP BY direction;
-- ⇒ 只有一行：down / 1537 / 1792510219288（= 2026-10-20 23:30:19 +08）
```

**全库从来没有过 `echo` + `up` 行。** 本窗口同样是 `audit/up = 4`、`echo/up = 0`。

⇒ 影响两处既有设计表述（**必须在 C-3 之前修正**）：

| 位置 | 原表述 | 实际 |
|---|---|---|
| `src/mqtt-dedup.js` 注释与 `0007` 的 `INV-K1` | 「同一条业务消息会被**两条规则各抓一次** ⇒ audit 与 echo 必然共存」 | **只对 `down` 成立**；`up`/`log` 只有一条规则，`echo` **不存在** |
| Rev.2 `§5.3 I-8` / `Q-5` | 「audit > 0 **且** echo > 0」 | 该判据只对 `direction='down'` 成立；对 `up` 断言 echo>0 会**永远误报** |

（去重键本身不受影响：`source` 进键 ⇒ 有 echo 时两条各留一行，无 echo 时只有一条。
受影响的是**验收判据**，不是写入逻辑。）

### 4.2 下行曾同时有 `audit` 与 `echo`，现在只剩 `echo`

| 时段（+08） | `audit`/`down`（plain `guo_feeder/<id>/down`） | `echo`/`down`（`$tenants/…`） |
|---|---|---|
| 2026-10-19 20:57:58 → 2026-10-20 02:50:19 | **193 行**（`device_id` 正确解析为 `288485896ce4`） | 同期也有 |
| 2026-10-20 23:22 起（本窗口） | **0 行** | **16 行** |

⇒ 两组数据之间**规则侧发生过变化**：下行不再产生 plain 主题的 audit 行。
**这不等于丢消息**（16 条都落库了，只是落在 echo 那一条链路上），但它说明：

- 线上规则集合**不是**静态的，"历史行为"不能直接外推
- `mqtt_messages` 的**来源构成会随规则变化而漂移** ⇒ 任何以"行数比例"为判据的验收都必须先固定规则形态

★ 该变化的**原因与时刻**需控制台确认（清单 H-5），本轮**不得**定案为"某项配置被误改"。

### 4.3 `log` 早已改走 history 端点（`unknown` 行的终点）

`device_id='288485896ce4'` 的 `direction='unknown'` 行共 **867 行**，最后一行的
`created_at = 2026-10-16 01:20:22 +08`；此后**再无**（`s2-d1-window-counts.txt` Q-C）。

⇒ `mqtt_messages` 自 `2026-10-16` 起只承载 `up`（+ `down` 的单一副本）。
B-3 的原判据（`up/result` 96/105 个 ID）**只在 `up` 方向内**成立，这条边界必须在所有后续引用中保留。

---

## 5. 已证实 / 待验证 / 不可测量

### 5.1 直接观测到的事实

1. 设备 `dev_288485896ce4` 在线，会话始于 `2026-10-20T15:22:26Z`，`clean_start=true`
2. 本会话 Broker 收 **35** PUBLISH、发 **16** PUBLISH；**各类丢弃计数全 0**
3. D1 同窗口：`audit/up` **4** 行 · `echo/down` **16** 行 · `log_history` **14** 行
4. 下行 16 ↔ 16 一一对应（**只有 echo 一份**）
5. 全库 `echo` 只存在 `down` 方向（1537 行），**从未出现 `echo/up`**
6. `audit/down` 行在 `2026-10-20 02:50` 后消失，此前有 193 行
7. `unknown`（`log`）行终止于 `2026-10-16 01:20`
8. API 可读端点仅 `/clients`、`/subscriptions`；其余 8 个端点 403（空 body）

### 5.2 与事实相符但**未证实**（不得当结论）

- 差额 17 主要是 `log` 主题（不走 `mqtt_messages`）—— 数量级合理，**无 per-topic 证据**
- `recv_msg` 含 QoS1 重传 —— `proto_ver=4` + `clean_start=true` 下**可能**，未验证
- `audit/down` 消失是规则被编辑 —— 时间上与"某次配置变化"吻合，但**无规则配置证据**

### 5.3 `NOT MEASURABLE`（本轮）

| ID | 项 | 阻塞原因 |
|---|---|---|
| `N-1` | Rule matched / passed / failed | `/rules` 403 + 无 Dashboard |
| `N-2` | Action 重试 / 队列 / inflight / 丢弃 | `/actions` 403 |
| `N-3` | Connector（HTTP→Pages）成功率 / 超时 / 错误 | `/connectors` 403 |
| `N-4` | Broker 全局 `recv_msg` / `send_msg` | `/stats` 403 |
| `N-5` | per-topic 收发计数 | 无可用端点 |
| `N-6` | Worker 5xx / `[mqtt-dedup] outcome=` | Worker Logs 403（`tail` 未取） |
| `N-7` | 可比较口径的真实漏记率 | §3.2 口径不可对齐 |

---

## 6. 人工取证清单（需 Dashboard 登录态）

> 两轮窗口：① 本报告窗口 `2026-10-20 23:22 → 23:35 +08`；② 对照窗口 `2026-10-20 02:40 → 03:00 +08`（`audit/down` 消失前后）。

| # | 页面 | 时间范围 | 需记录字段 |
|---|---|---|---|
| H-1 | **Monitoring → Clients** → `dev_288485896ce4` | ① | `recv_msg` / `recv_pkt` / `send_msg` / 各 `dropped`（与 §2 逐项对齐） |
| H-2 | **Integration → Rules** → 每条规则详情 | ①② | `matched` / `passed` / `failed` + **SQL 原文**（重点看 topic 过滤与 `$tenants` 用法） |
| H-3 | **Integration → Actions** → 每行 Metrics | ①② | `success` / `failed` / `retried` / `dropped` / `queued` / `inflight` |
| H-4 | **Integration → Connectors** → HTTP Connector | ①② | 成功率 / 失败率 / 超时数 / 最近错误文本 |
| H-5 | **Integration → Rules** 列表（含启用开关与更新时间） | — | **规则条数、各自 topic 模式、是否被禁用/改写**（用于解释 §4.2） |
| H-6 | **Monitoring → Overview** | ① | 全局收发曲线（确认 `recv_msg` 与总曲线比例） |
| H-7 | Rule SQL 的 **只读预览/测试**（不要 Publish） | — | `guo_feeder/+/up`、`+/log`、`+/down`、`$tenants/+/guo_feeder/+/down` 分别命中哪些 |

★ 取证纪律：**不要**点 Publish / Resend / Retry / Replay；**不要**改 SQL / Action / Connector；
截图**必须**带时间范围徽标，否则无法与 `created_at` 对齐。

---

## 7. 对 B-3 的净结论

1. **根因仍未定**。本轮**没有**拿到能定位到"规则 / Action / Connector"任一层的数据。
2. **排除**两项：Broker 会话层丢弃（全 0）、下行链路整链丢失（16↔16）。
3. **新增两项口径修正**（§4.1、§4.2）：
   - `echo` 只存在于 `down` ⇒ Rev.2 的 `I-8`/`Q-5` 必须按方向收窄
   - 下行来源构成发生过漂移 ⇒ 行数比例类判据必须先固定规则形态
4. **新增一条口径约束**（§4.3）：`mqtt_messages` 自 `2026-10-16` 起只承载 `up`/`down`，
   `/log` 走 `log_history` ⇒ **不得**用"设备发出 vs `mqtt_messages` 行数"算漏记率。
5. 单窗口样本太小（`up` 仅 4 条）⇒ **不足以**支撑"B-3 影响有限、可以试运行"。
   要支撑该结论，至少需要一次**活跃窗口**（阀动作 + 一批日志）配合 H-1…H-7。

---

## 8. 未决项

| ID | 项 | 状态 |
|---|---|---|
| `S2-U1` | Rule matched / Action 失败 / Connector 成功率 | **NOT MEASURABLE**（403 + 无 Dashboard） |
| `S2-U2` | 差额 17 条的逐主题分解 | **NOT MEASURABLE** |
| `S2-U3` | `recv_msg` 是否含 QoS1 重传 | **NOT VERIFIED** |
| `S2-U4` | `audit/down` 消失的原因与时刻 | **UNKNOWN**（需 H-5） |
| `S2-U5` | B-3 是否影响有限 | **证据不足**（单会话，`up` 样本 4 条） |
