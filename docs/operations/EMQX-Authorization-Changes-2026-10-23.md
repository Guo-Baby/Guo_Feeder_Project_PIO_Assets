# EMQX 侧改动记录 · log_ack topic 隔离（2026-10-23）

> 配套：`docs/evidence/LogAckSplit-2026-10-23/README.md`
> 方案：`docs/architecture/Log-Ack-Topic-Split-Plan-2026-10-23.md`（方案 A）
>
> EMQX 的配置**无法从 API 读取**（`403` + 空 body），只能人工在控制台改
> ⇒ 本文件是**唯一权威存档**，改动后请同步更新「§四 变更历史」。

---

## 一、数据集成规则 `r-n302933b-f731cf`（规则 B · 消息流）

**路径**：EMQX 控制台 → 数据集成 → 规则 → `r-n302933b-f731cf` → SQL 编辑

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

### 改动说明

- 相对改动前，**只多了最后一行** `AND ( NOT ( topic =~ 'guo_feeder/+/log/ack' ) )`。
- `=~` 是 **EMQX topic 通配符匹配，不是正则**；`+` 只匹配**一级**。
  ⇒ 原有的 `guo_feeder/+/log` 作用于 3 段 topic，**匹配不到** 4 段的 `/log/ack`
  ⇒ 必须单独加一条，否则新 topic 照样被转发进 webhook。
- OR 组**必须带括号**，否则与后置 `AND` 结合出错（线上已带）。
- `FROM "guo_feeder/#"` 是**多层级**通配，所以新 topic 本来就在它的匹配范围内。

### 可选补充（当前不需要）

若之后发现 `$tenants/...` 副本仍在消耗 webhook 调用配额，可再加：

```sql
  AND ( NOT ( topic =~ '$tenants/+/guo_feeder/+/log/ack' ) )
```

**不加也不会产生 D1 行** —— 4 段 topic 会被 Worker 的
`domainFromTopic()`（只认 3 段）判 `unknown_topic` 400 拒收。
另外 Worker 侧已加**载荷级**闸门（`cmd === "log_ack"` ⇒ 不入 `mqtt_messages`），
所以即使规则漏排除也只浪费调用次数，不会污染数据。

---

## 二、客户端授权 ACL

**必须加 subscribe 授权，否则新 topic 永远收不到消息。**

### ★★★ 先确认「账号名」：本设备实际用的是 `GuoFeederDevice`

这一步是 2026-10-23 折腾数轮才定位的根因：

| 项 | 值 | 从哪来 |
|---|---|---|
| `client_id` | `dev_288485896ce4` | `GF_CLIENT_ID_TPL` 模板渲染 |
| **`username`** | **`GuoFeederDevice`** | `data/config/mqtt.json` 里的历史值 |

EMQX 的 ACL 挂在**用户名**上 ⇒ 把规则加到 `dev_288485896ce4` 对这台设备
**一次都不会参与授权判定**，而且不会有任何报错。
`dev_288485896ce4` 从未被任何客户端使用过（它是 `provisionCredential()` 推出的
规范账号，凭证下发流程在这台设备上没走完）。

**判断「这台设备用哪个账号」的唯一依据**：EMQX 控制台 → 客户端 →
看该会话的**「用户名」列**。`client_id` 会骗人，它是另一份按模板渲染的配置。

**路径**：EMQX 控制台 → 访问控制 → 客户端授权 → 使用内置数据库 →
用户 **`GuoFeederDevice`**（← 不是 `dev_288485896ce4`）→ 规则列表

| 顺序 | 主题 | 动作 | 权限 |
|---|---|---|---|
| 1 | `guo_feeder/288485896ce4/down` | 订阅 | 允许 |
| 2 | `guo_feeder/288485896ce4/up` | 发布 | 允许 |
| 3 | `guo_feeder/288485896ce4/log` | 发布 | 允许 |
| **4** | **`guo_feeder/288485896ce4/log/ack`** | **订阅** | **允许** |
| 5 | `homie/5/288485896ce4/#` | 发布 | 允许 |
| 6 | `homie/5/288485896ce4/+/+/set` | 订阅 | 允许 |

★ 共 **6 条**。这是 `credential.js::buildAclRules()` 的完整输出，
PUT 是**全量覆盖** ⇒ 少一条就会把那条权限打掉。

### 更可靠的做法：让 Worker 自己写（可覆盖账号名）

```bash
# 写到规范账号（仅在设备 username 已迁移到 dev_<id> 后才如此）
curl -X POST \
  "https://guo-feeder-api.guobaby.workers.dev/api/admin/device/288485896ce4/acl" \
  -H "Authorization: Bearer <ADMIN_API_TOKEN>"

# 写到设备实际认证的共享账号（当前状态用这个）
curl -X POST \
  "https://guo-feeder-api.guobaby.workers.dev/api/admin/device/288485896ce4/acl" \
  -H "Authorization: Bearer <ADMIN_API_TOKEN>" \
  -H "Content-Type: application/json" \
  -d '{"username":"GuoFeederDevice"}'
```

规则**内容**仍按 `device_id` 生成（topic 里是 device_id），只有**账号名**可覆盖。
返回体会**原样列出写入的 6 条规则**，可据此核对。
（`2026-10-23` 新增端点，只做 `PUT ACL`，**不轮换密码** ——
`provisionCredential()` 会同时重发密码，有「设备没收到 ⇒ 再也连不上」的窗口。）

### ★★ 为什么这一步最容易翻车（两个独立原因）

**原因 1：静默拒绝**

EMQX 对未匹配规则的订阅是**静默拒绝**：

- SUBACK 照常返回
- 客户端**没有任何错误**
- esp-mqtt 的 `MQTT_EVENT_SUBSCRIBED` **照样触发**

现象只是"订阅成功但永远收不到消息" ⇒ 固件侧表现为
`[Log Cloud] ack timeout retry=1/5 …` 连续重试，最终 give up ⇒ **日志空洞**。
⇒ **不能把 `MQTT_EVENT_SUBSCRIBED` 当"订阅成功"的证据。**

**原因 2：拒绝即不建订阅，事后授权不会补建**

被拒的那次 SUBSCRIBE **不会**因为事后把 ACL 配好而补建；
而固件的订阅只发生在 `MQTT_EVENT_CONNECTED`（`cloud_manager.cpp`）⇒
**只要设备不重连，新 topic 的订阅就一直不存在**。

⇒ 正确顺序：**先配 ACL → 再让设备重连（重启）→ 订阅才会被接受**。
实测反证（同一台已配好 ACL 的设备）：

| 操作 | 采样结果 | 判读 |
|---|---|---|
| 只探测，不重启 | 19 帧 / 19 unique / 0 重复 | 订阅不存在 ⇒ 只有旧 `/down` 在送 |
| 云端 reboot 后 | 105 帧 / 54 unique / **51 个 seq 到 2 次** | 订阅已建立 ⇒ 两 topic 都送 |

---

## 三、验证方法

### 3.1 数据集成是否生效（D1 侧，无需 EMQX 权限）

```sql
-- 期望：规则生效后不再增长
SELECT COUNT(*) FROM mqtt_messages
 WHERE topic LIKE '%/log/ack'
   AND created_at > (unixepoch()*1000 - 300000);
```

### 3.2 ACL 是否生效（设备串口）

```
[Log Cloud] ack ok boot=.. to=.. covered=1/1 acked=.. gc=.. floor=.. r=..
```
持续增长 = 收到回执。若变成 `ack timeout retry=n/5` ⇒ 回执没到。

**决定性判据**：把 `LOG_ACK_DUAL_PUBLISH` 置 `"0"`（只发新 topic）后
`ack ok` 仍持续增长 ⇒ 新 topic 真正可用。

**2026-10-23 04:38 实测结论（已通过）**：`DUAL="0"` 部署后 50 s 采样
`ack ok=23 / ack timeout=0 / 23 unique seq / 0 重复`
⇒ 回执全部来自新 topic ⇒ 端到端可用。

### 3.3 双 topic 都在送达的指纹

采样串口里同一 `seq` 的 `log_ack` 出现**2 次** ⇒ 两个 topic 都通；
只出现 **1 次** ⇒ 只有一条链路在送（此时旧 `/down` 通、新 topic 被拒）。

★ 该判据**只在 `LOG_ACK_DUAL_PUBLISH="1"` 时有效**。
`DUAL="0"` 时正确读法是 **零重复 + `ack ok` 增长 = 新 topic 独立可用**。

★ 采样脚本：`docs/evidence/LogAckSplit-2026-10-23/probe_dual.py`（只读串口，不复位）。
⚠️ 含正则的脚本**不要用 shell heredoc 生成** —— Git Bash 会把 `\d` 变成 `/d`，
导致匹配恒为 0、看起来像"完全没有 ack"。

---

## 四、变更历史

| 日期 | 改动 | 执行者 | 结果 |
|---|---|---|---|
| 2026-10-23 03:2x | 规则 B WHERE 追加 `NOT ( topic =~ 'guo_feeder/+/log/ack' )` | 用户（人工） | ✅ 已生效（新 topic 不再入库） |
| 2026-10-23 03:2x | ACL 追加 `guo_feeder/<id>/log/ack` subscribe，加到 **`dev_288485896ce4`** | 用户（人工，界面） | ❌ 无效 —— **账号错了**，设备用的是 `GuoFeederDevice` |
| 2026-10-23 04:2x | 同上，改加到 **`GuoFeederDevice`** | 用户（人工，界面） | ⚠️ 当次仍不通 —— 设备未重连，被拒的订阅不会补建 |
| 2026-10-23 04:34 | 云端 reboot（重连 → 重新 SUBSCRIBE） | 自动（`system reboot`） | ✅ **生效** —— 51/54 的 seq 双到达 |
| 2026-10-23 04:37 | `LOG_ACK_DUAL_PUBLISH` → `"0"`，Worker 部署 `8aa6a8be` | 自动（wrangler） | ✅ **通过** —— `ack ok=23`，零重复 |
| 2026-10-23 04:3x | 清理账号：删除 2 个自 10-05「永不过期」的僵尸会话 | 用户（人工） | ✅ 完成 |

**当前最终状态**：EMQX 规则排除 ✅ / ACL 对 `GuoFeederDevice` 生效 ✅ /
Worker 只发新 topic ✅ / 固件双订阅保留（可回退）。

### 已知但暂不处理的遗留

`GuoFeederDevice` 是**全局共享账号**，而 `buildAclRules()` 是按设备生成隔离规则的
⇒ **P1 的「跨设备隔离 X-1」在配置层被绕过**；且拿到该账号即可订阅
`guo_feeder/<任意 id>/#`。根治需把设备 `mqtt.username` 迁到 `dev_288485896ce4`
并走完 `credential_set` 闭环 —— 建议单独立项，不要和日志改造混在一起。
（详见 `docs/operations/EMQX-Accounts-Inventory-2026-10-23.md`）

---

## 五、回退

删掉 §一 里新增的那一行、删掉 §二 里第 4 条即可回到改动前。
固件同时订阅新旧两个 topic，Worker 保持 `LOG_ACK_DUAL_PUBLISH="1"` 时
即使 EMQX 全部回退，日志通道也不会断。
