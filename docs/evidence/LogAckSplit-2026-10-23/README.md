# LOG-ACK-SPLIT · 日志回执 topic 隔离（2026-10-23）

> 目标：把日志通道的 `log_ack` 从命令下行 topic 分区出去，恢复 `/down`、`/up` 的调试可读性，
> 并顺带砍掉入库流量（`log_ack` 曾占 60 分钟 MQTT 流量 **46.7%**）。
> 方案依据：`docs/architecture/Log-Ack-Topic-Split-Plan-2026-10-23.md`（方案 A）。

---

## 一、最终形态

| 链路 | Topic | 方向 |
|---|---|---|
| 命令下行 | `guo_feeder/<device_id>/down` | 云 → 设备 |
| 遥测/回帧上行 | `guo_feeder/<device_id>/up` | 设备 → 云 |
| 日志上行 | `guo_feeder/<device_id>/log` | 设备 → 云 |
| **日志回执下行** | **`guo_feeder/<device_id>/log/ack`** | 云 → 设备 |

★ 新 topic **固定 4 段**是**有意**的：Worker `domainFromTopic()` 只认 3 段
⇒ 即使 EMQX 规则忘了排除，也会被判 `unknown_topic` 400 挡在入库之外。
这是"不靠记得改规则"的兜底保险。

---

## 二、改动清单（5 处，不是 3 处）

| # | 层 | 文件 | 改动 |
|---|---|---|---|
| 1 | 固件 | `src/services/topic_renderer.h` | 新增 `GF_TOPIC_TPL_ACK`（`guo_feeder/<device_id>/log/ack`） |
| 2 | 固件 | `src/cloud/cloud_manager.cpp` | 渲染 `mqtt_ack_topic`；**双订阅**（旧 + 新）；分派时把新 topic 判为 BUSINESS（复用既有 `c=="log_ack"` 分支，**零新增 handler**） |
| 3 | Worker | `guo-feeder-api/src/log-ack.js` | `logAckTopic()` 改新 topic；新增 `legacyLogAckTopic()` + 切换期双发开关 |
| 4 | Worker | `guo-feeder-api/src/credential.js` | `buildAclRules()` 增加新 topic 的 **subscribe** 授权（3 条 → 4 条业务规则，全量 6 条） |
| 5 | **EMQX** | 规则 `r-n302933b-f731cf`（数据集成） | `WHERE` 增加 `AND ( NOT ( topic =~ 'guo_feeder/+/log/ack' ) )` |

**为什么第 4 条不是可选项** —— 见 §三。

### 2.1 固件：双订阅，不用 foreign 通道

- `MQTT_EVENT_CONNECTED` 里在原有 subscribe **之后**追加一条新 topic 的 subscribe，
  原有那段代码**逐字未动**。
- 分派走 `mqtt_route_topic()` + 一个补充判定（`mqtt_ack_topic` 也判 BUSINESS）。
  **故意不走** `foreign subscription` 机制：全仓**没有任何模块**注册 foreign handler
  （`s_foreign_cb` 恒为 `nullptr`），启用它等于打开一条从未在生产跑过的路径。

### 2.2 Worker：切换期双发（`LOG_ACK_DUAL_PUBLISH`）

`log_ack` 同时发新 topic 和旧 `/down`，开关在 `wrangler.toml [vars]`：

- `"1"`（当前）：双发 —— 保证 ACL 未生效期间设备仍能收到 ACK
- `"0"`：只发新 topic —— 切换完成后的目标状态

设备对重复 ACK **幂等**（`log_ack_classify()` 判 `duplicate`，只推进水位不重复记账），
所以双发只增加下行流量，**不改变语义**。补发的成败单独计数
（`summary.legacy_dual` / `legacy_dual_failed`），**不进** `attempted` 与三态判定，
以免污染既有 ACK 统计口径。

---

## 三、★ 关键实测发现：ACL 缺失会**静默**吞掉订阅

### 3.1 现象

Worker 部署新 topic 后，设备固件**已经**订阅成功（串口有 SUBACK）：

```
[Cloud] MQTT subscribe(log-ack) filter=guo_feeder/288485896ce4/log/ack qos=1 msg_id=54973
[Cloud] MQTT subscribed msg_id=54973
```

但 45 秒串口采样（`ack-after-worker-deploy.txt`，第一次）：

| 指标 | 值 |
|---|---|
| `[Log Cloud] ack ok` | **0** |
| `ack timeout retry` | **2** |

⇒ 设备发了日志，收不到任何 ACK。log 通道事实上中断。

### 3.2 定位（不是猜的）

部署双发后再采样 45 秒，同一批 `seq` **各自只出现 1 次**：

```
RX log_ack 帧数 = 20 · 唯一 seq = 20 · 重复 seq = 0
```

两条 topic 都在发、只有一个在送达 ⇒ 送达的是旧 `/down`，
**新 `/log/ack` 被 broker 拒绝投递**。

根因：`buildAclRules()` 里没有新 topic 的 `subscribe` 授权。
EMQX 对未匹配规则的订阅**静默拒绝**（不报错、不给客户端可见信号，
esp-mqtt 侧 `MQTT_EVENT_SUBSCRIBED` 仍会触发）。

### 3.3 结论（写进纪律）

> **任何"新增设备下行 topic"的改动，ACL 授权与 topic 模板是同一次改动，不可分开发布。**
> 漏了 ACL 的表现不是报错，而是"订阅成功但永远收不到消息"——最难查的一类故障。

### 3.4 顺带补的一个缺口：只同步 ACL 的管理端点

`POST /api/admin/device/:id/acl`（`2026-10-23` 新增）

以前改 ACL 的唯一入口是 `provisionCredential()`，它会经 `ensureUserPassword()`
**同时重发密码**（`PUT` 后旧密码立即失效），存在"设备没收到 `credential_set`
⇒ 再也连不上"的窗口。只改 ACL 时没有任何理由去动凭据 ⇒ 单开一条只做
`PUT ACL` 的通道（全量覆盖、天然幂等、不碰凭据、写 `acl_update` 审计事件）。

> ⚠️ EMQX 授权/会话缓存可能让**既有会话**沿用旧判定 ⇒ 推完 ACL 若行为未变，
> 让设备重连一次（断电重启或凭证重发）。

---

## 四、当前状态（03:5x）

| 项 | 状态 |
|---|---|
| 固件编译 | **PASS**：Flash 68.9%（1,444,381）/ RAM 41.5%（135,880）/ 0 error |
| 固件烧录 | **PASS**：COM8，`Hash of data verified`，仅写 app `0x10000`，未擦片、未 uploadfs |
| 启动校验 | **PASS**：`boot validated` / `loaded 10/10` / 无 panic |
| 新 topic 渲染 | **PASS**：`MQTT log-ack=guo_feeder/288485896ce4/log/ack` |
| 双订阅 | **PASS**：两次 SUBACK（`msg_id=25161`、`54973`） |
| Worker 部署 | **DONE**（version `13c6a259`，`LOG_ACK_DUAL_PUBLISH=1`） |
| 日志通道 | **恢复**：双发后 45 秒 `ack ok = 20` / `timeout = 0` |
| 新 topic 实际投递 | **未通** —— ACL 未授权（见 §三） |
| EMQX 规则排除 | **未做**（需人工，见 §五） |

镜像：`flash-target-b2a727f5.bin`
`sha256 = b2a727f528aa3c1803f5e4d852a1a6a9aebf88b75aff28df34721de3a08ae789`（1,444,752 B）

---

## 五、EMQX 侧需要人工做的两件事

### 5.1 追加 ACL 授权（**必须**，否则新 topic 永远收不到消息）

访问控制 → 客户端认证/授权 → 用户 `dev_288485896ce4` → 增加规则：

| 字段 | 值 |
|---|---|
| 主题 | `guo_feeder/288485896ce4/log/ack` |
| 动作 | **订阅（Subscribe）** |
| 权限 | 允许（Allow） |

（等价做法：带 `ADMIN_API_TOKEN` 调 `POST /api/admin/device/288485896ce4/acl`，
新端点会一次性写入全部 6 条规则。）

### 5.2 规则 `r-n302933b-f731cf` 的完整 SQL（数据集成 → 该规则 → SQL 编辑）

```sql
SELECT
  *
FROM
  "guo_feeder/#"
WHERE
  (
      is_null(payload.schema_version)
   OR is_null(payload.seq)
   OR is_null(payload.ts)
   OR is_null(payload.type)
   OR is_null(payload.value)
   OR is_not_null(payload.c)
   OR is_not_null(payload.cmd)
  )
  AND ( NOT ( topic =~ 'guo_feeder/+/log' ) )
  AND ( NOT ( topic =~ 'guo_feeder/+/log/ack' ) )
```

改动只有**最后一行**（原有两行保持不变）。

- `=~` 是 **topic 通配符，不是正则**；`+` 只匹配**一级** ⇒
  原有的 `guo_feeder/+/log` **匹配不到** 4 段的 `/log/ack`，所以必须单独加一行。
- 若之后发现 `$tenants/...` 副本仍在消耗 webhook 调用，再加一行
  `AND ( NOT ( topic =~ '$tenants/+/guo_feeder/+/log/ack' ) )`。
  **不加也不会产生 D1 行** —— 4 段 topic 会被 Worker 判 `unknown_topic` 400 拒收，
  只是仍占用 webhook 调用次数。

### 5.3 顺序建议

1. 先加 ACL（§5.1）→ 让设备重连一次（断电重启或凭证重发）
2. 确认新 topic 可达：把 `LOG_ACK_DUAL_PUBLISH` 置 `"0"` 重新部署，
   若 `ack ok` 继续增长 ⇒ 新 topic 通了
3. 再改规则 SQL（§5.2）

---

## 六、验证判据

| 观察点 | 通过条件 |
|---|---|
| 设备串口 | `MQTT log-ack=guo_feeder/<id>/log/ack` |
| 设备串口 | `MQTT subscribed` 出现**两次**（每次连接） |
| 设备串口 | `[Log Cloud] ack ok …` 持续增长，且 `ack timeout retry` 不连续出现 |
| 双发期 | 同一 `seq` 的 `log_ack` **只出现 1 次**（新 topic 未通）/ **2 次**（已通） |
| `LOG_ACK_DUAL_PUBLISH=0` 后 | `ack ok` 仍增长 ⇒ 新 topic 真正可用 |
| D1 `mqtt_messages` | 不再新增 `raw_payload LIKE '%log_ack%'` 的行 |
| Worker | 不再出现 `unknown_topic` 400（若规则已排除） |

---

## 七、回退方式（任一步失败）

| 想回到 | 做法 |
|---|---|
| 旧 topic 单发 | `LOG_ACK_DUAL_PUBLISH="0"` → 改 `logAckTopic()` 回 `/down` → 重新 deploy |
| 固件回退 | 重烧上一版镜像（`docs/evidence/TestStepper-2026-10-22/flash-target-2d4b7899.bin`） |
| EMQX | 删掉 §5.2 的最后一行 |

固件**同时订阅新旧两个 topic** ⇒ 无论 Worker 发哪个，设备都收得到，切换过程无断链窗口。

---

## 八、同批处理的四个遗留问题（结论）

### 8.1 `guo_feeder/up`（无 device_id 的 legacy topic）—— **已删**

实测 3 组 legacy topic 共 **2101 行**（`id 1..2257`，全部为 10 月初旧拓扑）：

| topic | 行数 |
|---|---|
| `guo_feeder/up` | 1662 |
| `guo_feeder/down` | 404 |
| `$tenants/n302933b/guo_feeder/down` | 35 |

处置：**先整表备份再删**（`legacy-topic-rows-backup.json`，本地保留，未入库），
`DELETE` 返回 `changes = 2101`，D1 体积 4,075,520 → **3,567,616** 字节（约 −0.5 MB）。

### 8.2 `/log` 上行 `direction = 'unknown'` —— **历史问题，代码已修**

- 成因：旧版 `index.js` 判方向只认 `endsWith("/up")` / `"/down"`，漏了 `/log`
  （`G4-R5 · B-2c`，2026-10-20 已补）。
- 实测：`unknown` 共 876 行，`id 2265..3483`，**最近 24 小时 0 行**
  ⇒ 修复已生效，**不是当前问题**。
- 处置：把历史 867 行 `guo_feeder/<id>/log` 的 `direction` 回填为 `log`
  （`UPDATE` 返回 `changes = 867`，非破坏性）。
  剩余 9 行 `topic` 为空串的历史脏行，无归属可言，保留原样。

### 8.3 `$tenants` 副本与无前缀副本并存 —— **设计行为，不是 bug**

`mqtt-dedup.js` 顶部明确写了这是**故意**的：

- 规则 A（plain topic）⇒ `source='audit'` 主审计行
- 规则 B（`$tenants/<ns>/...` 副本）⇒ `source='echo'` **回环行**（实测晚 33–37 s）

`source` 是 `dedup_key` 的一部分，所以两者**永不合并**——合并会吃掉回环证据。

实测当前形态：`up` 只有 plain（设备直发）；`down` 有 plain（562）+ tenant（2873）。
本次改造后 `log_ack` 退出 `/down`，`down` 的绝大部分行数会直接消失，
存储压力自然缓解，所以**暂不改这套语义**。
如果之后仍要省空间，正确做法是加一个"只收 audit、丢弃 echo"的开关，而不是改 dedup 键。

### 8.4 `gc` 被 `floor` 钳住 —— **机制正常，但有一个待观察的隐患**

`gc=15721 floor=15722 acked=17009` 的读法（`log_manager.cpp:422-450`）：

| 量 | 含义 |
|---|---|
| `acked` | 云端确认过的**最大** seq（只用于观测） |
| `gc` | **连续**可回收水位：任何 `seq <= gc` 的记录都已被确认 |
| `floor` | Flash 中**尚未补发**的 backlog 最低 seq（`cloud_replay_arm()` 时设定） |

关系恒为 `gc = min(acked, floor - 1)`。所以 `floor=15722` ⇒ `gc=15721`，
两者是**同一件事的两种写法**，不是两个独立故障。

为什么必须钳：ACK 只证明"刚发出的那批被持久化"，**不证明**比它小的都已持久化
（Boot 期 live 记录的 seq 更大，可能先被 ACK）。若让单点高水位跨过仍在 Flash
等待补发的旧记录 ⇒ 补发被跳过 + 段被误回收 ⇒ 违反 at-least-once。

⇒ **不是 bug，是保守策略**。

**但有一个待观察点**：本 boot 内 `floor` 一直没清（串口从未出现
`[Log Cloud] replay sweep done`），而 `acked` 在持续增长 ⇒
"已 ACK 但不可回收"的区间在**单个 boot 内会持续变大**。

- 跨 boot 观察：上一 boot `gc=12546/floor=12547` → 本 boot `gc=15721/floor=15722`
  ⇒ 每次重启会重新 arm 到更高位置，段回收在那里发生，所以**不是永久泄漏**。
- 判据：若某次 boot 内 `floor` 长时间（> 30 分钟）不动且 `acked` 增长超过
  `LOG_CLOUD_QUEUE_SLOTS`（128）条 ⇒ 需要查 `cloud_poll()` 的清除条件
  （要求"补发 sweep 完成 **且** 队列中已无上一 Boot 的记录"）。
- 本次**不改**：属于独立缺陷，改动面在 at-least-once 契约上，需单独评审。

---

## 九、用户改完 EMQX 后的复测（04:00–04:10）

### 9.1 规则 B 的 WHERE 排除 —— ✅ 已生效

`guo_feeder/<id>/log/ack`（plain 形态）**不再入库**。
残留的只有 `$tenants/n302933b/guo_feeder/<id>/log/ack`（tenant 副本），
且它被 Worker 判 `unknown_topic` 400 拒收 ⇒ **不产生 D1 行**（只是仍占 webhook 调用）。

### 9.2 ACL 的 subscribe 授权 —— ❌ **未生效**

把 `LOG_ACK_DUAL_PUBLISH` 置 `"0"`（只发新 topic）重新部署，然后下发重启（boot 67）：

```
[Cloud] MQTT subscribe(log-ack) filter=guo_feeder/288485896ce4/log/ack qos=1 msg_id=57450
[Cloud] MQTT subscribed msg_id=57450          ← SUBACK 照常
[Log Cloud] ack timeout retry=1/5 backoff=2000ms
[Log Cloud] ack timeout retry=2/5 backoff=4000ms
ack ok = 0 | timeout = 3
```

串口里**没有任何 SUBACK 错误**（esp-mqtt 对 0x80 不打印）——这正是"静默拒绝"。

诊断闭环：把开关恢复 `"1"` 后同一设备立刻恢复
`ack ok = 9 / timeout = 1`（boot 68）⇒ 与 topic 内容无关，就是新 topic 没被授权。

> 交叉证据：04:03 那一分钟内，`ack:66:17701` 在 `$tenants/.../down` 与
> `$tenants/.../log/ack` 两份都能在 D1 查到 ⇒ **Worker 的 publish 本身是成功的**
> （否则双发分支不会触发），消息确实进了 EMQX，只是没投到订阅者。

### 9.3 新增（本次）：Worker 侧**载荷级**闸门

`index.js` 在写 `mqtt_messages` 之前加了：

```js
if(cmd === LOG_ACK_COMMAND)          // "log_ack"
{
    return jsonResponse({ ok: true, ignored: "log_ack" }, 200);
}
```

为什么 topic 拆分之外还要这一道：

| 理由 | 说明 |
|---|---|
| 拆分依赖**两处云端手工配置** | EMQX 规则的 `WHERE` + ACL 的 subscribe，任一失效就静默回潮 |
| 与 topic 形态无关 | 切换期双发（回执仍走 `/down`）时不重复污染；有人把 topic 改回去也不复发 |
| 立刻见效 | 不依赖 EMQX 权限，部署即可用 |

语义影响：只少记"回执"这类机械帧；命令 / 遥测 / result 的审计行不变
（早退发生在 `insertMqttMessage()` 之前，`log_ack` 是下行帧，
后续 `handleRegistrySideEffects()` 与 `cloud_api` 发布分支对它都无作用）。

**实测**：部署（`107cf13c`）后 `log_ack` 入库行数 **0**（04:07 之后再无新增）；
普通命令下行不受影响 —— 同一时段 `query` 命令仍正常落两行
（`id=10420` plain / `id=10421` tenant，`cmd=query`）。

### 9.4 当前状态

| 项 | 状态 |
|---|---|
| 固件双订阅 | ✅ 生效（`msg_id` 两次 SUBACK） |
| Worker 双发 | ✅ `LOG_ACK_DUAL_PUBLISH="1"`（保命，日志通道正常） |
| 新 topic 送达 | ❌ ACL 未授权 |
| D1 的 `log_ack` 污染 | ✅ 已归零（载荷级闸门） |
| MQTTX 里 `/down` 看到 ack | ⚠️ **仍有** —— 双发阶段回执还在 `/down`，只有 ACL 通了才能撤掉双发 |

⇒ **下一步只有一件事：把 ACL 的第 4 条真正写进去**，
然后 `LOG_ACK_DUAL_PUBLISH` 置 `"0"` 复测，`ack ok` 仍增长即完成。

---

## 十、★★ 真正的根因：设备登录的**不是** `dev_<device_id>`

### 10.1 发现过程（04:20 复测）

用户在控制台把 ACL 第 4 条加到 `dev_288485896ce4`（该账号权限数 3 → 4），再测：

**非破坏性指纹探测**（不部署、不重启，只采串口 40 s）：

```
RX log_ack frames : 17
unique seq        : 17
duplicates        : NONE          ← 同一 seq 只到 1 次
range             : 18501 .. 18517
```

两条 topic 都在发（`LOG_ACK_DUAL_PUBLISH="1"`），**同一个 seq 只到达 1 次**
⇒ 仍然只有一条链路在送 ⇒ 新 topic 还是没被授权。**但 ACL 明明加了。**

### 10.2 根因

EMQX 客户端列表（用户截图）里：

| 客户端 ID | **用户名** |
|---|---|
| `dev_288485896ce4` | **`GuoFeederDevice`** |

设备侧配置（`data/config/mqtt.json`，与设备实值一致）：

```json
"client_id": "dev_<device_id>",     // → 渲染成 dev_288485896ce4
"username":  "GuoFeederDevice"      // ← 认证用的是这个
```

⇒ **client_id 是 `dev_288485896ce4`，但认证账号是 `GuoFeederDevice`**。
ACL 挂在**用户名**上 ⇒ 加到 `dev_288485896ce4` 的规则**根本不参与本设备的授权判定**。
`dev_288485896ce4` 那个账号从未被设备使用过（它是 Worker `provisionCredential()`
按 `usernameFor()` 推出来的规范账号，凭证下发流程在这台设备上没走完）。

### 10.3 由此暴露的架构级问题（P1，未修）

1. **per-device ACL 事实上全部失效**：所有设备若都用共享账号 `GuoFeederDevice`，
   则 `credential.js::buildAclRules()` 写的按设备隔离规则**不被采纳**
   ⇒ P1 的核心不变量「跨设备隔离（X-1）」在**配置层被绕过**。
2. **共享账号 + 弱口令**：`GuoFeederDevice` 是全局共享账号，
   任何拿到它的客户端都能订阅**全部设备**的主题。
3. **凭证下发链路未闭环**：`provisionCredential()` 会做
   `PUT 密码 → 全量 ACL → 下发 credential_set`，这台设备的 `mqtt.username`
   仍是历史值 ⇒ 该流程要么从未成功，要么之后被配置覆盖。

### 10.4 处置

**临时（立刻可做）**：把 ACL 写到设备**实际使用的账号**上。
`/api/admin/device/:id/acl` 已支持覆盖账号名（`2026-10-23` 新增）：

```bash
curl -X POST \
  "https://guo-feeder-api.guobaby.workers.dev/api/admin/device/288485896ce4/acl" \
  -H "Authorization: Bearer <ADMIN_API_TOKEN>" \
  -H "Content-Type: application/json" \
  -d '{"username":"GuoFeederDevice"}'
```

规则内容仍按 `device_id=288485896ce4` 生成（topic 里是 device_id），
只有**账号名**被覆盖 ⇒ 6 条规则落到真正参与认证的账号上，返回体会原样列出。

**根治（后续单独立项）**：把设备的 `mqtt.username` 迁到 `dev_288485896ce4`
（需要 Worker 重新签发凭据并让设备 `credential_set` 成功应用），
然后删掉 `GuoFeederDevice` / `GuoFeederDevice001` 这两个共享账号。

### 10.5 另一个教训

`MQTT_EVENT_SUBSCRIBED` 不能证明订阅成功（见 §三），
而**"ACL 加了但没生效"也不能直接怪 EMQX** ——
先确认"这个账号到底是不是这台设备在用的账号"。
判断依据只有一处：EMQX **客户端列表里的"用户名"列**（`client_id` 会骗人，
因为 `client_id` 是按模板渲染的，`username` 是另一份配置）。

---

## 十一、✅ 切换完成（2026-10-23 04:33–04:40）

用户清理了 EMQX 账号与授权规则，给**正确的账号** `GuoFeederDevice` 配上 ACL，
并清除了两个自 10-05 起「永不过期」的僵尸测试会话（`dev_d163d0000001`、`dev_deadbeef0001`）。

### 11.1 ★★ 关键机制：改完 ACL **必须重新订阅**，否则永远不生效

这是我这一轮才定位到的最后一环，也是前面几次「ACL 明明加了却不通」的第二个原因：

EMQX 对 SUBSCRIBE 是**拒绝即不建订阅** —— 被拒的那次 SUBSCRIBE 不会因为事后授权而补建。
而固件的订阅动作只发生在 `MQTT_EVENT_CONNECTED` 里（`cloud_manager.cpp`）⇒
**只要设备不重连，新 topic 的订阅就一直不存在**。

所以顺序必须是：**先配 ACL → 再让设备重连（重启）→ 订阅才会被接受**。

反证（本轮实测，同一个已配好 ACL 的设备）：

| 操作 | 40~50 s 采样 | 判读 |
|---|---|---|
| 只探测（不重启） | 19 帧 / 19 unique / **0 重复** | 订阅仍不存在 ⇒ 只有旧 `/down` 在送 |
| 云端 reboot 后探测 | 105 帧 / 54 unique / **51 个 seq 到 2 次** | 两条 topic 都在送 ⇒ 订阅已建立 |

⇒ 结论：**ACL 生效的条件是「授权存在」+「重新订阅」两者同时满足**。
以后凡是改设备下行 topic 的 ACL，都必须配一次重连，否则会得到「加了但没用」的假结论。

### 11.2 撤销双发的独立验证（决定性）

`LOG_ACK_DUAL_PUBLISH` 置 `"0"` 并部署（version `8aa6a8be-4a48-4456-9721-7e4124ac78fc`，
`env.LOG_ACK_DUAL_PUBLISH ("0")`），此时 Worker **只**发新 topic：

```
ack_ok lines        = 23
ack_timeout lines   = 0
ack frames          = 23
unique seq          = 23
duplicated seq      = 0
```

**零重复 + ack_ok 持续增长** ⇒ 这 23 条回执只可能来自 `guo_feeder/<id>/log/ack`，
新 topic 端到端打通（EMQX 授权 → 投递 → 设备收到 → 水位推进）。

> 注：`probe_dual.py` 的判语在「双发已关」时会误报「只有一条链路在送」——
> 该脚本的判据只对 `DUAL="1"` 有效。DUAL=0 时正确读法是
> **零重复 + ack_ok>0 = 新 topic 独立可用**。

### 11.3 净化效果（D1，最近 10 分钟）

| 形态 | 方向 | 行数 |
|---|---|---|
| plain | down | 1 |
| plain | up | 3 |
| tenant | down | 1 |

- `log_ack` 入库：**0 行**
- 这 5 行**全部**来自本轮验证下发的一条 `query`（plain+tenant 各 1 条 down，加 3 条 up）
  ⇒ **背景 MQTT 噪声基本归零**
- 对比切换前：60 分钟 285 行、其中 `log_ack` 占 46.7%

### 11.4 最终状态

| 项 | 状态 | 证据 |
|---|---|---|
| 固件双订阅 | ✅ 保留（旧 `/down` + 新 `/log/ack`） | boot 日志两次 SUBACK（`40587` / `22815`） |
| Worker 双发 | ✅ **已关**（`"0"`） | 部署输出 `env.LOG_ACK_DUAL_PUBLISH ("0")` |
| 新 topic 送达 | ✅ **通** | 零重复 + `ack_ok=23` |
| EMQX 规则排除 | ✅ 生效 | `log_ack` 入库 0 行 |
| Worker 载荷级闸门 | ✅ 生效（第二道防线） | 同上 |
| 命令链路 | ✅ 正常 | `query_state valve_status` 回 `false` |

**固件侧刻意保留旧订阅**：不额外增加开销（`/down` 本来就是命令通道），
但保留了「Worker 双发开关重新置 `"1"` 即可回退」的能力 ⇒ 回滚只需一次部署，不用刷固件。

### 11.5 本轮踩到的工具坑（避免复发）

**用 shell heredoc 写 Python 脚本会把正则里的 `\d` 变成 `/d`**。
本轮 `probe_dual.py` 因此一直匹配 0 帧，导致我一度得出「没有 ack」的错误中间结论。
⇒ 以后含正则 / 转义字符的脚本一律用文件写入工具生成，不要走 `cat > ... <<EOF`。
