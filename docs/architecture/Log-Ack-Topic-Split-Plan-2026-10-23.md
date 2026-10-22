# 方案：把 `log_ack` 从 `/down` 迁移到独立 Topic

- 日期：2026-10-23
- 状态：**方案待裁决**（未实施）
- 目标：把日志通道的回执从命令 topic 隔离出去，恢复 `/down`、`/up` 的调试可读性

---

## 一、现状：污染是怎么发生的

三条链路目前共用两条 topic：

| # | 方向 | Topic | 内容 |
|---|---|---|---|
| 1 | 设备 → 云 | `guo_feeder/<id>/log` | 日志记录（逐条） |
| 2 | 云 → 设备 | `guo_feeder/<id>/down` | **命令** + **`log_ack` 回执**（混在一起） |
| 3 | 设备 → 云 | `guo_feeder/<id>/up` | 命令回帧 |

而 EMQX 的数据集成规则是：

```sql
FROM guo_feeder/#
WHERE (7 项 OR 展开) AND ( NOT ( topic =~ 'guo_feeder/+/log' ) )
```

- `FROM guo_feeder/#` ⇒ **所有方向、所有层级**都进 webhook
- 排除条件只针对 **3 段**的 `/log`
- `log_ack` 的 payload 带 `p.c` ⇒ 命中 `is_not_null(payload.c)` ⇒ **通过过滤，被写入 `mqtt_messages`**

实测证据（`mqtt_messages` 按 topic 分组）：

| 方向 | 行数 | Topic |
|---|---|---|
| down | **2415** | `$tenants/n302933b/guo_feeder/288485896ce4/down` |
| up | 1662 | `guo_feeder/up`（legacy，无 device_id） |
| up | 1615 | `guo_feeder/288485896ce4/up` |
| unknown | 867 | `guo_feeder/288485896ce4/log` |
| down | 553 | `guo_feeder/288485896ce4/down` |

近 60 分钟：`log_ack` 下行 **133 条**，占全部 MQTT 消息（285 条）的 **46.7%**。

---

## 二、改动清单：是 5 处，不是 3 处

> ⚠️ 你判断「只需要改固件 + Cloudflare，EMQX 不用动」——**这一点需要修正**，见 §2.5。

| # | 层 | 文件 / 位置 | 改什么 | 必须 |
|---|---|---|---|---|
| 1 | 固件 | `src/services/topic_renderer.h/.cpp:28-30,146-148` | 新增模板 `guo_feeder/<device_id>/log/ack` | ✅ |
| 2 | 固件 | `src/cloud/cloud_manager.cpp:443-450` `cloud_transport_dispatch_rx()` | 让新 topic 走 **BUSINESS** 分支（见 §3 方案 A）| ✅ |
| 3 | Worker | `guo-feeder-api/src/log-ack.js:154` `logAckTopic()` | 返回值改成新 topic | ✅ |
| 4 | Worker | `guo-feeder-api/src/credential.js:116` `buildAclRules()` | **加一条 subscribe 授权** | ✅ |
| 5 | **EMQX** | 规则 `r-n302933b-...`（数据集成 SQL） | `WHERE` 加 `AND ( NOT ( topic =~ 'guo_feeder/+/log/ack' ) )` | ✅ |

### 2.5 为什么 EMQX 必须改

`FROM guo_feeder/#` 是**多层级通配**。现有排除 `topic =~ 'guo_feeder/+/log'` 里 `+` 只匹配**一级**，
所以 `guo_feeder/<id>/log/ack`（4 段）**不会**被排除。

⇒ 不改 EMQX，新 topic 照样被转发进 webhook、照样入库，**污染只是换个名字**。

改动收益也不止"干净"：EMQX 不转发后，Worker 的 webhook 调用量和 D1 写入直接减掉约 **46%**。

### 2.6 ACL 不加会怎样（最容易踩的坑）

`buildAclRules()` 目前只授权：

```js
{ topic: `guo_feeder/${deviceId}/down`, permission: "allow", action: "subscribe" }
```

不加新 topic ⇒ 设备订阅被 broker 拒绝。
**MQTT 的订阅拒绝是静默的**（SUBACK 0x80 或直接断开），设备侧只表现为"收不到 ack" ⇒
日志通道进入超时重试 ⇒ 看起来像"日志通道坏了"，极难查。

---

## 三、固件侧两种接法（推荐 A）

现有框架已经具备 topic-aware 分派：`mqtt_route_topic()` 返回
`BUSINESS` / `FOREIGN` / 未注册，未注册的直接丢弃（`cloud_manager.cpp:443-467`）。

### 方案 A（推荐）：把新 topic 也判为 BUSINESS

在 `mqtt_route_topic()` 里加一个比较分支即可。

- 复用现成的 `cloud_process_rx_message()` → `c == "log_ack"` 分支（`cloud_manager.cpp:1519-1544`）
- **零新增 handler**，`log_ack` 仍然走它自己的 bypass 路径（跳过命令 id 校验与去重缓存）
- 改动量最小、风险最低

### 方案 B：走 foreign 订阅机制

- 用 `cloud_transport_subscribe(filter, qos)` 注册 + 订阅
- 需要 `cloud_transport_set_foreign_handler()` 注册回调 —— **目前全仓无人注册**（`s_foreign_cb` 为 null，
  且 `s_foreign_subs.count` 为 0），等于启用一条从未跑过的路径
- 语义更干净，但引入未经生产验证的分支

⇒ **选 A**。B 留作将来多个外部 topic 时的重构方向。

---

## 四、⚠️ 切换原子性：这是最大风险

Worker 与固件**不可能同时**生效。任一侧先改都会断链：

| 顺序 | 后果 |
|---|---|
| Worker 先改 | 设备还没订阅新 topic ⇒ 收不到 ack ⇒ 停等超时 → 重试 5 次 → give up ⇒ **日志空洞** |
| 固件先改 | Worker 还在发旧 topic ⇒ 同样收不到 ⇒ 同上 |

### 建议的切换序列（任一步失败都可回退）

1. **固件：同时订阅旧 `/down` 和新 topic**（ACL 两条都授权）
2. Worker：`logAckTopic()` 改新 topic，只发新的
3. 观察设备侧 `acked` 单调增长、`gc` 不再被 `floor` 钳住（现况 `gc=12546=floor-1` 说明有未闭环积压）
4. 固件升级并稳定运行后，**再**从 `buildAclRules()` 撤掉旧 `/down` 的订阅授权（可选，属收尾）

★ ACL 在**连接时**下发 ⇒ 刷完固件必须让设备重连/重启一次才生效。

---

## 五、验证判据

| 层 | 判据 |
|---|---|
| 设备串口 | 出现 `[Cloud] MQTT subscribe ... filter=guo_feeder/<id>/log/ack` |
| 设备串口 | `[Log Cloud] ack ok boot=.. to=..` 持续出现（说明新 topic 上的 ack 被正确消费） |
| Worker | `logAckTopic()` 生效 —— EMQX `/publish` 的 topic 变为新值 |
| D1 | `SELECT COUNT(*) FROM mqtt_messages WHERE direction='down' AND raw_payload LIKE '%log_ack%' AND created_at > <切换时刻>` ⇒ **0** |
| D1 | `mqtt_messages` 每分钟新增行数应下降约一半 |
| 反证 | 若为 0 但设备日志通道停止推进 ⇒ 说明 EMQX 把 ack 也挡掉了（排除条件写太宽） |

---

## 六、工作量与影响面

| 项 | 量级 |
|---|---|
| 固件 | 约 40–60 行（1 个模板 + 1 个路由分支 + 1 条订阅）；需重烧 |
| 固件余量 | 当前 Flash 68.9% / RAM 41.5%，按 G-1/G-2 门禁重查即可（改动可忽略） |
| Worker | 2 行 |
| EMQX | 1 条规则 SQL 的 `WHERE`，**需在控制台改，改错会断掉 up/log 入库** |
| UI 影响 | `/down` 命令历史逻辑不变（命令仍走 `/down`）；但 `mqtt_messages` 里 `log_ack` 行消失 ⇒ 任何"下行条数"统计会掉约一半（这是**预期**）|

### EMQX 改动的额外风险与做法

- 线上规则同时承载 `up` / `log` 入库 ⇒ **先 `SELECT` 试跑**、只在 `WHERE` 追加一个 `AND` 子项
- 改前导出规则原文留档（`EMQX_Assets/config/data-integration/`）
- 追加而非重写，保留原有 7 项 OR 组与 `/log` 排除的位置

---

## 七、备选方案（若不想动 EMQX）

| 方案 | 说明 | 代价 |
|---|---|---|
| 把 ack 发到 **3 段** topic，如 `guo_feeder/<id>/ack` | `deviceIdFromTopic` 只认 `up`/`log`（`history-ingest.js:207`）⇒ ack 会被判 `invalid_device_id` 400 挡在入库之外 | 每条 ack 都打一次 400，Worker 日志噪声大；且仍占用 webhook 配额 |
| 只改 Worker 不改固件 | 不可行 | ack 必须被设备收到 |

⇒ 都不如方案 §2 干净，但「4 段 topic 天然无法归属」这一点可以作为**兜底保险**：
即使 EMQX 忘了改，数据也不会进 `mqtt_messages`，只是多一次 400。

---

## 八、顺带发现的既有问题（建议一并记录，非本次范围）

1. `guo_feeder/up`（**无 device_id** 的 legacy 主题）仍有 1662 行
2. `/log` 上行在 `mqtt_messages` 里 `direction` 记为 **`unknown`**（867 行）
3. `$tenants/...` 前缀副本与无前缀副本并存 ⇒ 同一帧可能双份入库
4. `gc=12546` 被 `floor=12547` 钳住而 `acked=15372` ⇒ 存在未闭环 seq 区间，`log_ack_gc_watermark()` 限制了段回收
