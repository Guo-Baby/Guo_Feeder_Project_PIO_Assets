# P2-5 · Homie ACL（设备命名空间授权）—— 实施与验收

> **性质**：EMQX 授权层实现 + 真实 broker 实测。**未修改任何 firmware 代码**（P2-5 的职责是让 broker ACL 适配**已冻结**的 P2 Homie contract，不是反过来）。
> **状态**：**`P2-5 = CLOSED`** · **`P2-7 HARDWARE TEST UNBLOCKED`（WAITING FOR HUMAN HARDWARE SETUP）**
> **上位冻结文档**：`P2-0-architecture-review-and-design-freeze.md` **§16**（ACL Design，AD-11 / §16.1–§16.4）
> **前置**：P2-0.2 / P2-1…P2-4 / P2-6 已完成；`P2-7-Hardware-Test-Readiness.md` §5 把「P2-5 未落地」列为 P2-7 头号阻塞 ⇒ 本文即该阻塞的解除记录。

---

## 0. 结论摘要

| 项 | 结果 |
|---|---|
| 规则模板（新签发设备） | ✅ 5 条 = P1 三条 + Homie 两条 |
| 真实 EMQX 授权实测（自有密码探针身份） | ✅ **23/23 PASS** |
| 静态契约测试（离线） | ✅ **26/26 PASS** |
| Cloudflare 既有契约套件回归 | ✅ 5 套件全 OK |
| 已注册设备补写 | ✅ `dev_3cfb56c8a5fc` 3 → **5 条**（P1 全保留，回读校验通过） |
| 生产设备 `288485896ce4` | ✅ **未改动**（仍 3 条） |
| 探针身份残留 | ✅ **0**（账号与规则均已删除） |
| 兜底白名单 | ✅ **未动**（`deny all #`） |

---

## 1. ACL 最终规则

### 1.1 设备身份规则集（`dev_<device_id>`，共 5 条）

| # | Identity | Direction | Topic | Result |
|---|---|---|---|---|
| 1 | `dev_<id>` | SUB | `guo_feeder/<id>/down` | **ALLOW** |
| 2 | `dev_<id>` | PUB | `guo_feeder/<id>/up` | **ALLOW** |
| 3 | `dev_<id>` | PUB | `guo_feeder/<id>/log` | **ALLOW** |
| 4 | `dev_<id>` | **PUB** | `homie/5/<id>/#` | **ALLOW** ← P2-5 新增 |
| 5 | `dev_<id>` | **SUB** | `homie/5/<id>/+/+/set` | **ALLOW** ← P2-5 新增 |
| — | `dev_<id>` | SUB/PUB | 任何**其它** `<device_id>` 的 `homie/5/…` 或 `guo_feeder/…` | **DENY**（兜底） |
| — | 任意主体 | any | `homie/5/#` · `homie/5/+/#` · `homie/#` · `#` | **DENY**（兜底） |
| — | 全部用户（兜底） | all | `#` | **DENY**（白名单语义根基，**未触碰**） |

### 1.2 为什么是这个方向（不是"全 pub + 全 sub"）

与 `P2-0 §16.2` **逐字一致**：

- **PUB 用 `homie/5/<id>/#`** —— ACL **无"排除"语义**，无法用单条规则表达"除 `set` 外全部"。设备向**自己**的 `set` 发布是**无害自环**（设备不订阅自己要发的路径之外的东西）。§16.2 说明 1。
- **SUB 只给 `homie/5/<id>/+/+/set`** —— 固件实际订阅的 filter 就是 `<base>+/+/set`（`homie_set_filter()`，`homie_projection.h:222`），**逐字对齐**。
- **`<id>` 段逐字枚举，永不使用通配** —— 跨设备隔离不破（P1 核心不变量，§16.1）。

### 1.3 实现位置（3 处，两路径收敛）

| 路径 | 文件 | 作用 |
|---|---|---|
| **P1 动态签发**（真实设备走这条） | `Cloudflare_Assets/guo-feeder-api/src/credential.js` → `buildAclRules()` | 新签发设备即得 5 条 |
| **P0-4 静态模板** | `EMQX_Assets/config/acl-rules.v3.json` → `homieRuleTemplate` | `apply-acl.mjs --device-ids` 展开 |
| **展开逻辑（纯函数）** | `EMQX_Assets/scripts/acl-expand.mjs` | 两路径共用；带防通配校验 |
| **在册设备补写**（OQ-2） | `EMQX_Assets/scripts/backfill-homie-acl.mjs` | 读全量 → 合并 → PUT → 回读六步 |

> **★ PUT 是全量覆盖（F-28）**：只发 Homie 两条会**打掉 P1 三条**（设备立刻"能连上但什么都不通"）。三条实现都强制「合并后整体 PUT」。

---

## 2. P1 回归

| 项 | 结果 |
|---|---|
| P1 三条规则是否仍完整 | ✅ `dev_3cfb56c8a5fc` 补写后回读：`down`(sub) / `up`(pub) / `log`(pub) **全部在位** |
| 是否出现权限覆盖 | ❌ **无**（三条实现均先读全量再合并；`backfill-homie-acl.mjs` 显式断言 `P1 三条仍在`） |
| Cloudflare 既有契约套件 | ✅ `o1_o10_contract` · `p163a_evidence` · `hard_retirement` · `redaction` · `p161_contract(legacy, 7 EXPECTED_FAILURE)` 全部 `exit=0` |
| 其它账号是否受影响 | ✅ 12 个其它规则组**逐条未变**（见 §5 终态审计） |

---

## 3. Cross-device isolation

| 项 | 结果 |
|---|---|
| **own namespace**（探针 A 访问自己） | ✅ **PASS** — SUB `guo_feeder/<A>/down` / `homie/5/<A>/+/+/set` granted；PUB `homie/5/<A>/$state` / `water/valve` / `guo_feeder/<A>/up` / `log` 全 `accepted(0x0)` |
| **foreign namespace**（探针 A 访问 B 与生产） | ✅ **PASS（全部 DENY）** — SUB → `Not authorized`；PUB → `denied(0x87)` |
| 越权通配 `homie/5/#` · `#` | ✅ **DENY** |
| `fixture ACL isolation`（独立临时凭据） | ✅ **PASS**（本文实测，MQTT 5 reason code 为证） |
| **`real second-device isolation`**（第二台真实 ESP32-S3） | ⛔ **NOT RUN** —— 需第二台硬件，属 P2-7 实物阶段 |

> **★ 证据分级（不得混同）**：本轮只证明了 **fixture ACL isolation**（用两个**独立临时凭据** `dev_p25probea01`/`dev_p25probeb01` 取得真实 broker 授权结果）。**尚未**用第二台真实设备证明 —— 该结论**只能由 P2-7 实物阶段给出**，本文**不声称**。

### 3.1 实测明细（真实 EMQX，MQTT 5，读 reason code）

```
探针 A = dev_p25probea01 (namespace homie/5/p25probea01/)
探针 B = dev_p25probeb01 (namespace homie/5/p25probeb01/)
foreign = 288485896ce4   （仅作被拒目标；未改其 ACL）

3) A 自己的 namespace（应全部 ALLOW）
  [PASS] SUB own `guo_feeder/<A>/down`            granted(qos1)
  [PASS] SUB own `homie/5/<A>/+/+/set`            granted(qos1)
  [PASS] PUB own `homie/5/<A>/$state`             accepted(0x0)
  [PASS] PUB own `homie/5/<A>/water/valve`        accepted(0x0)
  [PASS] PUB own `guo_feeder/<A>/up`              accepted(0x0)
  [PASS] PUB own `guo_feeder/<A>/log`             accepted(0x0)

4) 跨设备 / 越权（应全部 DENY）
  [PASS] SUB B `homie/5/<B>/#`                    Not authorized
  [PASS] SUB B `homie/5/<B>/+/+/set`              Not authorized
  [PASS] SUB prod `homie/5/<prod>/#`              Not authorized
  [PASS] SUB 越权 `homie/5/#`                      Not authorized
  [PASS] SUB 越权 `#`                              Not authorized
  [PASS] SUB B `guo_feeder/<B>/down`              Not authorized
  [PASS] PUB B `homie/5/<B>/water/valve/set`      denied(0x87)
  [PASS] PUB prod `homie/5/<prod>/water/valve/set` denied(0x87)
  [PASS] PUB B `guo_feeder/<B>/up`                denied(0x87)
  [PASS] PUB prod `guo_feeder/<prod>/up`          denied(0x87)
```

> **观察方式合规**：不依赖固件"subscribe called"日志，而是读 **EMQX 实际授权结果**（MQTT5 SUBACK/PUBACK reason code；`0x87 = Not authorized`）。这与 P1 的历史教训一致。
> **3.1.1 探针使用 `rejectUnauthorized:false`**（本机无 `/emqxsl-ca.crt`）—— 仅影响**探针本地**的 TLS 链校验，**不影响** ACL 授权结论。

### 3.2 retained `/set`（P2-4 语义未被 ACL 改变）

ACL **不能**也**不试图**改变 retained 语义：`retain=true` 与 `retain=false` 走 **同一 topic、同一 ACL 规则**，授权结果相同；`event->retain` 的丢弃判定完全在**固件侧**（`homie_on_foreign()` 第一句）。
⇒ P2-7 的 **场景 A（实时 retained）/ 场景 B（broker 重放）** 由 ACL 侧**天然放行到设备**，再由设备丢弃 —— 与 §19.5 一致。**该两场景的端到端验证仍属 P2-7 实物阶段（NOT RUN）**。

---

## 4. 生产设备与已注册设备

| 设备 | 处理 | 结果 |
|---|---|---|
| **`288485896ce4`（生产）** | **未修改**（任务书 §八：非必要不动） | 仍 **3 条**；仅作为 **foreign 被拒目标** |
| **`3cfb56c8a5fc`（本机开发板，经人工指定）** | **补写** Homie 两条（六步流程 + 回读校验） | **3 → 5 条**，P1 三条全保留 ✅ |
| `a5a0000000a1` / `b5b0000000b1` / `c5c0000000c1`（P5 fixture） | **未修改** | 3 条 / `deny all #` / `deny all #`（与冻结状态一致） |
| `d163d0000001/2/3`（已硬退役/REVOKED） | **未修改** | 仍 `deny all #` |
| `GuoFeederDevice` / `GuoFeederDevice001`（P0 legacy） | **未修改** | 仍 6 条（3 设备规则 + 3 legacy） |
| 非设备账号（`workbuddy`/`test001`/`shouji`） | **未修改** | 仍 1 条 `guo_feeder/#` |

### 4.1 `dev_3cfb56c8a5fc` 补写前后完整 ACL

```
① 补写前（PUT 之前，实测）
    allow subscribe guo_feeder/3cfb56c8a5fc/down
    allow publish   guo_feeder/3cfb56c8a5fc/up
    allow publish   guo_feeder/3cfb56c8a5fc/log

② PUT 内容（= 原 3 条 + 新增 2 条）
    allow subscribe guo_feeder/3cfb56c8a5fc/down
    allow publish   guo_feeder/3cfb56c8a5fc/up
    allow publish   guo_feeder/3cfb56c8a5fc/log
    allow publish   homie/5/3cfb56c8a5fc/#          ← 新增
    allow subscribe homie/5/3cfb56c8a5fc/+/+/set    ← 新增

④ 回读（实测）
    5 条，逐条与 ② 一致
    [PASS] 规则数 = 原有 + 新增
    [PASS] P1 三条仍在（未被覆盖）
    [PASS] Homie pub `homie/5/<id>/#` 已存在
    [PASS] Homie sub `homie/5/<id>/+/+/set` 已存在
    [PASS] 未引入 `#` / `homie/5/#` / `homie/5/+…`（跨设备 allow）
```

---

## 5. 终态审计（真实 EMQX，实测）

```
== workbuddy (1)              allow all  guo_feeder/#          ← 未变
== test001 (1)                allow all  guo_feeder/#          ← 未变
== shouji (1)                 allow all  guo_feeder/#          ← 未变
== dev_d163d0000003 (1)       deny  all  #                     ← 未变（退役）
== dev_d163d0000002 (1)       deny  all  #                     ← 未变
== dev_d163d0000001 (1)       deny  all  #                     ← 未变
== dev_c5c0000000c1 (1)       deny  all  #                     ← 未变
== dev_b5b0000000b1 (1)       deny  all  #                     ← 未变
== dev_a5a0000000a1 (3)       guo_feeder/a5a0000000a1/{down,up,log}    ← 未变
== dev_3cfb56c8a5fc (5)       ← 本轮唯一改动：+ pub homie/5/3cfb56c8a5fc/#
                                                          + sub homie/5/3cfb56c8a5fc/+/+/set
== dev_288485896ce4 (3)       guo_feeder/288485896ce4/{down,up,log}    ← 生产未改
== GuoFeederDevice001 (6)     guo_feeder/288485896ce4/* + legacy       ← 未变
== GuoFeederDevice (6)        同上                                    ← 未变
== ALL-USER 兜底               deny all #  (qos 0,1,2)          ← 未变（白名单根基）
```

**零残留**：探针账号 `dev_p25probea01` / `dev_p25probeb01` 与其规则组**均已 DELETE**（`GET` 复核：账号数与规则组数回到初始 11 / 13）。

---

## 6. 测试与命令

| 用途 | 命令（在 `EMQX_Assets/`） |
|---|---|
| 静态契约测试（**离线**，26 断言） | `npm run test:p2-5` |
| 真实 broker 授权实测（需 `.env` + `mqtt` 包） | `npm run test:p2-5:broker`（需 `NODE_PATH=<装了 mqtt 的 node_modules>`） |
| 在册设备补写（OQ-2 流程） | `node --env-file-if-exists=.env scripts/backfill-homie-acl.mjs --device-id <id> [--apply]` |
| 静态模板套用（P0-4 路径） | `node --env-file-if-exists=.env scripts/apply-acl.mjs --device-ids <id1,id2> [--apply]` |
| Cloudflare 契约套件回归 | `cd ../Cloudflare_Assets/guo-feeder-api && node test/run_all.mjs` |

**静态测试覆盖面**（`test/p2_5_acl_check.mjs`，26/26）：规则总数/顺序 · 方向（P1 不得互换、Homie 不得放宽为全 pub+全 sub）· 无 `#`/`homie/5/#`/`homie/5/+/…` · device_id 段逐字 · 模板含占位符 · 展开确定性 · **两路径收敛**（`credential.js` 与 `acl-rules.v3.json` 对同一 id 产出完全相同的 Homie 规则）· 负向（4 类危险模板必须被拒 + 无 device_ids 必须拒绝）。

---

## 7. 残余 NOT RUN（如实记录，不得声称完成）

| 项 | 状态 | 原因 |
|---|---|---|
| `real second-device isolation` | ⛔ **NOT RUN** | 需**第二台真实 ESP32-S3**（或第二实机身份）；本文只有 fixture 级证据 |
| `dev_3cfb56c8a5fc` 的 **MQTT 级 own-device 登录测试** | ⛔ **NOT RUN** | P1 冻结：Worker 不回传 password，明文只在签发帧（N-3 后连 `mqtt_messages` 也读不到）⇒ 无法以该身份登录。已用**自有密码的探针身份**取得等价授权证据 |
| retained `/set` 场景 A/B 端到端 | ⛔ **NOT RUN** | 需真实设备 + broker 重放（P2-7 实物） |
| Homie 真机 discovery / `$state=ready` / LWT | ⛔ **NOT RUN** | P2-7 实物 |
| 生产 `288485896ce4` 的 Homie ACL | **有意未部署** | 非必要不动；其真机若需 Homie，按 §4.1 六步流程补写（需授权） |

---

## 8. 范围与纪律声明

**改动（3 仓，均为 P2-5 必需）**：

| 仓 | 文件 | 性质 |
|---|---|---|
| Cloudflare | `src/credential.js`（`buildAclRules`） | ACL 生成逻辑 |
| EMQX | `config/acl-rules.v3.json`（`homieRuleTemplate` + `_rules` ⑤⑥） | ACL 配置 |
| EMQX | `scripts/apply-acl.mjs`（展开 homieRuleTemplate）· `scripts/acl-expand.mjs`（新，纯逻辑）· `scripts/backfill-homie-acl.mjs`（新，补写） | ACL 工具 |
| EMQX | `test/p2_5_acl_check.mjs` · `test/p2_5_acl_broker_verify.mjs`（新）· `package.json`（2 个 script） | P2-5 测试/fixture |
| 固件 | 本文（doc-only） | 文档 |

**未修改**：Device Model · Homie Projection · Homie Bridge · Transport Facade · CloudManager · CommandManager · Capability Registry · P1 lifecycle · P2-0 冻结架构 · Flash 预算 · Homie topic/property/`$description`/`$state` 契约 · `/set` retained 安全 · Worker 业务 API · P2-6 预算 · P2-7 硬件实现。

**未做**：为通过 ACL 而修改 firmware · 部分 PUT · 动兜底白名单 · 改生产设备 · 无授权补写 · push。

---

## 9. 最终状态

```
P2-0.2  = FROZEN
P2-6    = CLOSED / PASS
P2-5    = CLOSED
  规则模板（新签发）        = 5 条（P1 三条 + Homie 两条）
  静态契约测试             = 26/26 PASS
  真实 broker 授权实测      = 23/23 PASS（fixture ACL isolation）
  在册设备补写             = dev_3cfb56c8a5fc 3 → 5 条（P1 保留，回读通过）
  生产设备 288485896ce4     = 未改动（3 条）
  零残留                   = 探针账号与规则均已删除
P2-7    = UNBLOCKED / WAITING FOR HUMAN HARDWARE SETUP
  ⛔ real second-device isolation = NOT RUN（需第二台硬件）
```
