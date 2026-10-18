# BT-1 · EMQX Cloud 控制台修改与回滚操作手册

> **版本**：`rev.1`（`2026-10-17`）
> **适用范围**：Guo Feeder 生产 EMQX Cloud **Serverless** 部署（`n302***.ala.cn-hangzhou.emqxsl.cn:8443`）
> **★ 本次未修改任何 EMQX 对象**：本文档中所有"修改步骤"均为**待执行预案**，只在负责人单独授权后在控制台手工执行。
> 本轮唯一的生产操作是 **`POST /api/v5/publish` 到 `test/bt1/<run>/**`**（零规则命中、零 D1 写入、零设备投递）。

---

## 1. 在役对象（`2026-10-17` 快照，只读核对）

| # | 规则 ID | 控制台备注 | `FROM` |
|---|---|---|---|
| 1 | `r-n302933b-cf0cf2` | `guo_feeder_history_ingest` | `"guo_feeder/+/up", "guo_feeder/+/log"` |
| 2 | `r-n302933b-f731cf` | （history ingest 续） | `"guo_feeder/#"` |
| 3 | `r-n302933b-de1540` | `guo_feeder_presence_connected` | `"$events/client_connected"` |
| 4 | `r-n302933b-7e8a01` | `guo_feeder_presence_disconnected` | `"$events/client_disconnected"` |

**配额**：规则 **4/4 已用满**（Serverless Free）。
**关联**：动作 4 个（`a-…126ec6` / `a-…7241e2` / `a-…999c12` / `a-…0209ad`）· 连接器 2 个（`c-…0c1cb0` / `c-…20ead9`）· 启用状态与高级设置**未复核**（数据集成 API 全 `403`，只能控制台目视）。
**证据**：`EMQX_Assets/config/data-integration/EMQX-Live-Rules-Snapshot-2026-10-17.md`。

> **★ 配额含义**：**不能新增规则**。任何调整都必须是**就地修改现有规则**；也不得删除规则来腾额度。

---

## 2. 待执行预案：`N1-A`（排除全部 `down` topic）

### 2.1 为什么需要（以及为什么本轮没做）

`BT-1` 上线后，Worker 会向 `guo_feeder/<dev>/down` 发布 `log_ack`。该 topic **命中规则 2（`FROM "guo_feeder/#"`）** ⇒ 每条 ACK 都会作为一条 `direction='down'` 记录写进 D1 `mqtt_messages`（每条 ACK 一行，且随重投放大）。

`Q-5` 已裁决为 **`N1-A`**（排除全部 `down`）。**本轮不在授权范围内**（规则变更是控制台手工操作，且与契约验证无技术依赖），故**未执行**。

### 2.2 修改前必须记录（回滚基线）

控制台 → **数据集成 → 规则 →** 打开 `r-n302933b-f731cf` → **SQL 编辑器**，**逐字复制**当前 `WHERE` 全文到临时文件（这是回滚的唯一依据）。

当前（快照可见部分）：

```sql
SELECT
  *
FROM
  "guo_feeder/#"
WHERE
  ( <7 项 OR 展开谓词> )
  AND ( NOT ( topic =~ 'guo_feeder/+/log' ) )
```

> ⚠️ `=~` 是 **topic 通配符匹配**（`+` / `#`），**不是正则**；排除写法必须用 `'guo_feeder/+/down'`。

### 2.3 修改内容（**逐字段**）

| 位置 | 现状 | 改为 |
|---|---|---|
| 规则 2 `WHERE` 的**末尾** | `AND ( NOT ( topic =~ 'guo_feeder/+/log' ) )` | `AND ( NOT ( topic =~ 'guo_feeder/+/log' ) )`<br>`AND ( NOT ( topic =~ 'guo_feeder/+/down' ) )` |

**不加**新规则、**不加**新动作、**不改** `FROM`、**不改**连接器。

### 2.4 操作步骤（控制台）

1. 数据集成 → 规则 → `guo_feeder_history_ingest`（`r-…f731cf`）
2. 右上 **编辑** → SQL 编辑器
3. 在 `WHERE` 子句末尾追加上面那一行（**保留**原有全部谓词与括号）
4. 点 **SQL 校验/测试**（若可用）⇒ 无语法错误
5. **保存** → 确认规则仍为 **启用** 状态
6. 记录保存时间与截图（含规则 ID 与完整 `WHERE`）

### 2.5 修改后验证

| 验证项 | 方法 | 通过判据 |
|---|---|---|
| 规则仍启用 | 规则列表页 | 状态 = 启用 |
| 规则数未变 | 列表计数 | 仍 **4/4**（未新增对象） |
| `/up` 仍入库 | 观察 `telemetry_history` 行数继续增长 | 增长正常 |
| `/log` 仍入库 | 观察 `log_history` 行数继续增长 | 增长正常 |
| `down` 不再入库 | 设备产生一次 ACK 后查 `mqtt_messages` 中 `direction='down'` | **不再新增** |

> 建议在设备自然上报后观察 10–15 分钟；**不需要**为此向设备下发任何命令。

### 2.6 回滚（失败即执行）

1. 数据集成 → 规则 → 该规则 → 编辑 → SQL 编辑器
2. 把 `WHERE` **整段替换**回 §2.2 记录的原文（**不要**手工"删掉刚加的那一行"以外的任何改动）
3. 保存 → 确认启用状态
4. 验证：`mqtt_messages` 中 `direction='down'` **恢复增长**

**回滚判据（何时必须回滚）**：任一出现即回滚
- `/up` 或 `/log` 入库量骤降为 0
- 规则变为"已停止/异常"
- 保存后 SQL 报错且无法消除

### 2.7 影响面

| 对象 | 影响 |
|---|---|
| 真实设备流量 | **无**（`up`/`log` 谓词不变） |
| Pages / Worker | **无**（`down` 本就没有消费方） |
| D1 `telemetry_history` / `log_history` | **无** |
| D1 `mqtt_messages` | **`direction='down'` 从此不再新增**（历史 571 行保留） |
| 规则/动作/连接器配额 | **不变**（就地改 SQL） |

---

## 3. 备选：`N1-B`（仅排 `log_ack`，保留普通 `down` 审计）

`Q-16` 本轮**未采用**（`payload` 值比较语义 / null-safe 行为 / 配额影响均未实测）。若未来启用，**必须**用 null-safe 形式：

```sql
AND ( is_null(payload.c) OR payload.c != 'log_ack' )
```

> ⚠️ **危险写法**：`AND NOT ( payload.c = 'log_ack' )` 在三值逻辑下会把**所有无 `c` 键的 telemetry 行**一并排除（静默、无报错）。**不得使用**。

---

## 4. 本次生产操作清单（如实记录）

| 操作 | 是否执行 | 说明 |
|---|---|---|
| `POST /api/v5/publish` → `test/bt1/<run>/**` | ✅ 执行 | 12 条合成消息，无订阅者、无规则命中 |
| 修改规则 SQL | ❌ 未执行 | 见 §2（待授权） |
| 新增/删除规则、动作、连接器 | ❌ 未执行 | — |
| 修改 ACL / 用户 / 认证 | ❌ 未执行 | — |
| 向 `guo_feeder/**` 发布 | ❌ 未执行 | — |
| 部署 Worker | ❌ 未执行 | — |
| 写/删 D1 数据 | ❌ 未执行 | — |
