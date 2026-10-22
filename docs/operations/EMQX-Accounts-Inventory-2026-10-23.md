# EMQX 账号清单与清理建议（2026-10-23）

> 依据：EMQX 控制台「访问控制 → 客户端认证/授权 → 用户」两张截图 ＋「客户端」在线列表截图 ＋
> 设备侧 `data/config/mqtt.json` ＋ `mqtt_messages` 里的历史 device_id 分布。
>
> ⚠️ EMQX 的账号/ACL 配置**无法从 API 读取**（`403` 空 body）⇒ 本文件是唯一存档。

---

## 一、最关键的一条先说

| 客户端 ID（渲染后） | **认证用户名** | 说明 |
|---|---|---|
| `dev_288485896ce4` | **`GuoFeederDevice`** | 真实 ESP32 设备 |

**client_id 与 username 是两个不同的配置项**：

```json
"client_id": "dev_<device_id>",     // → dev_288485896ce4
"username":  "GuoFeederDevice"      // ← 认证/ACL 用的是这个
```

⇒ 把 ACL 加到 `dev_288485896ce4` 对这台设备**完全无效**。
同一个 `dev_288485896ce4` 账号从未被任何客户端使用过。

---

## 二、账号逐个判定

| 用户名 | 权限数 | 有客户端在用？ | 判定 | 理由 |
|---|---|---|---|---|
| **`GuoFeederDevice`** | 6 | ✅ 已连接（真实设备） | **必须保留** | 设备当前唯一认证账号 |
| **`dev_288485896ce4`** | 4 | ❌ 无 | **保留** | 规范账号（`usernameFor()` 产物），迁移目标 |
| **`test001`** | 1 | ✅ 已连接（MQTTX） | **保留** | 用户的手工测试账号 |
| `shouji` | 1 | ❌ 无 | **待确认** | 名字像"手机"，可能是用户自己的手机 MQTTX |
| `workbuddy` | 1 | ❌ 无 | **待确认（倾向可删）** | 无任何记录；疑似早期 WorkBuddy 会话建的测试号 |
| `GuoFeederDevice001` | 6 | ❌ 无 | **可删** | 早期手工建的重名测试号 |
| `dev_d163d0000001` | 1 | ⚠️ 有过会话（10-06，未连接） | **可删** | 探测用伪 device_id |
| `dev_d163d0000002` | 1 | ❌ 无 | **可删** | 同上 |
| `dev_d163d0000003` | 1 | ❌ 无 | **可删** | 同上 |
| `dev_a5a0000000a1` | 3 | ❌ 无 | **可删** | 同上（`mqtt_messages` 里仅 10 行 up） |
| `dev_b5b0000000b1` | 1 | ❌ 无 | **可删** | 同上 |
| `dev_c5c0000000c1` | 1 | ❌ 无 | **可删** | 同上 |
| `dev_3cfb56c8a5fc` | 5 | ❌ 无 | **可删** | 同上（1 行 up） |
| `dev_deadbeef0001` | — | ⚠️ 有过会话（10-05，未连接） | **可删** | 探测用伪 device_id；用户列表里若没它就说明账号已删、只剩僵尸会话 |

### 判定依据（不是猜的）

`mqtt_messages` 里这些伪 device_id 的残留行数（清理前统计）：

| device_id | up 行数 | 出现时间 |
|---|---|---|
| `a5a0000000a1` | 10 | 10-05 ~ 10-06 |
| `b5b0000000b1` | 2 | 10-06 |
| `c5c0000000c1` | 2 | 10-06 |
| `d163d0000001/2/3` | 各 1~3 | 10-05 |
| `3cfb56c8a5fc` | 1 | 10-05 |
| `deadbeef0001` | — | 10-05 |

全是 10 月 5–6 日**接入探测**留下的，之后再无活动 ⇒ 删除不影响任何现有功能。

---

## 三、建议的清理动作（在控制台逐个点 🗑）

**必须删（9 个）**

```
dev_d163d0000001
dev_d163d0000002
dev_d163d0000003
dev_a5a0000000a1
dev_b5b0000000b1
dev_c5c0000000c1
dev_3cfb56c8a5fc
dev_deadbeef0001
GuoFeederDevice001
```

**先确认再删（2 个）**

```
workbuddy      ← 我不认识这个账号，没有历史记录
shouji         ← 像是你手机的 MQTTX
```

**绝对不能删（3 个）**

```
GuoFeederDevice      ← 设备现在就用它认证
dev_288485896ce4     ← 规范账号，后续迁移目标
test001              ← 你的 MQTTX
```

⚠️ 删账号前先在「客户端」页 **清除会话**，否则可能出现"账号已删但会话还在"的僵尸连接
（当前 `dev_d163d0000001`、`dev_deadbeef0001` 就是"未连接"的历史会话）。

---

## 四、顺带发现的两个问题（P1，未修）

### 4.1 per-device ACL 事实上完全没生效

所有设备都用共享账号 `GuoFeederDevice` ⇒ `credential.js::buildAclRules()`
写出的"按设备隔离"规则**不被采纳**。P1 的核心不变量
「跨设备隔离（`X-1`）：设备只能访问自己的 topic」在**配置层被绕过**。

现在之所以没出事，是因为只有一台设备。**加到第二台就会互相串**。

### 4.2 共享账号 + 弱口令

`GuoFeederDevice` 是全局共享账号，任何拿到它的人都能订阅**所有设备**的
`guo_feeder/<任意 id>/#`。这与 P1 的账号体系（`dev_<device_id>` 一机一号）设计相反。

### 4.3 临时绕法（已实现）

`POST /api/admin/device/:id/acl` 支持 `{"username": "..."}` 覆盖账号名
（规则内容仍按 `device_id` 生成）⇒ 可以把 6 条规则直接写到 `GuoFeederDevice` 上。

根治方案（后续单独立项）：让设备的 `mqtt.username` 迁到 `dev_288485896ce4`，
走完 `provisionCredential()` 的 `credential_set` 闭环，然后删掉共享账号。

---

## 五、账号与 topic 的对应关系（备忘）

| 项 | 值 |
|---|---|
| 设备 device_id | `288485896ce4` |
| client_id 模板 | `dev_<device_id>` → `dev_288485896ce4` |
| **认证 username** | **`GuoFeederDevice`**（历史遗留，待迁移） |
| 规范 username | `dev_288485896ce4`（`usernameFor()`） |
| topic 前缀 | `guo_feeder/288485896ce4/…` |
| 4 条 topic | `…/down`、`…/up`、`…/log`、**`…/log/ack`** |
| ACL 应挂在哪 | **`GuoFeederDevice`**（当前）／`dev_288485896ce4`（迁移后） |
