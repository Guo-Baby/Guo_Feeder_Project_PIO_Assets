# Phase D Final Review（P1-2 / P1-3 / P1-4 收口评审）

> **状态**：**已完成**（2026-10-04）· 真机端到端 **51 / 51 PASS**
> **对应工作包**：`P1-实现清单.md` 的 **P1-2**（凭据签发链路）+ **P1-3**（设备侧凭据管理）+ **P1-4**（设备注册）
> **上游契约**：`P1-2-P1-4-注册与凭据签发设计.md`（**Q1–Q13**）· `P1-0-EMQX-Capability-Test.md`（A-7 / **A-8** / Design Impact Freeze）
> **实施轨迹**：`P1-3-PhaseD-实施计划.md`（§7 = D-3 结果）
> **本文性质**：**评审记录**（范围 / 边界遵守 / 交付物 / 验收对照 / 缺陷 / 遗留 / 给 P1-5 的接口）。不含新代码。

---

## 0. 一句话结论

**Phase D 达成设计目标**：设备以「一机一密」身份上线，凭据的**签发 → 试连自证 → 生效 → 轮换切主 → 退役**全链路在**真机**上闭环；
`device_id` / topic 渲染等 **P0 身份链路零回归**；**D1 schema `0001` 未改动**；两处"只在真机上才会暴露"的缺陷已修复并回归。

---

## 1. 范围与目标

| 目标 | 结果 |
|---|---|
| 设备能被云端**发现并建档**（未知 `device_id` ⇒ `CLAIM_PENDING`，**不授权**） | ✅ |
| 经**准入**后为设备签发**一机一密**凭据（建账号 → 写 ACL → 落库 → 下发） | ✅ |
| **ACL 是否真的生效**由设备侧**真实建连**自证 | ✅ |
| 凭据可**轮换**（切主）与**退役**（ACL 收紧 + 踢会话） | ✅ |
| 失败**可检测、可对账、可回收**（四态派生 + `provision_error`） | ✅ |
| 明文凭据**不落** D1 / 日志 / 事件 / 串口 | ✅ |

**不在范围**（明确排除）：APP UI · 绑定/解绑（P1-5）· 在线状态（P1-6）· 遥测（P3）· OTA · 用户登录体系。

---

## 2. 冻结边界遵守情况（逐条核对）

### 2.1 `P1 Architecture Freeze`（P0/P1 基线）

| # | 边界 | 遵守 | 证据 |
|---|---|---|---|
| 1 | `username` = `dev_<device_id>` | ✅ | 云端 `usernameFor()` + D1 CHECK 双保险 |
| 2 | 设备侧 NVS `gfcred`，双 slot，`ACTIVE/TESTING/INVALID` | ✅ | `cred_store` / `credential_manager` |
| 3 | 未知 `device_id` **只能**进 `CLAIM_PENDING`，**严禁**自动升 `REGISTERED` | ✅ | `claimAuthorized()` 门控；实测未准入签发 ⇒ **403** |
| 4 | 解绑**不吊销** credential；**退役才 revoke** | ✅ | P1-5 未实现；退役路径独立 |
| 5 | **ACL 修改 ≠ 完整 revoke** ⇒ `update ACL` **必须**配 `terminate session` | ✅ | `revokeDevice()` 成对；实测「只收紧 ACL 会话仍在」 |
| 6 | `device_id` 不可变；`client_id` 必须 `dev_<device_id>`；Topic V3 | ✅ | **未改动** `device_identity` / `topic_renderer`；试连仅用 `_t<gen>` 瞬时后缀（方案 B，已审核） |
| 7 | 控制面**不得引入轮询** | ✅ | 对账为**手工/定时触发**端点，无常驻任务 |

### 2.2 Phase C `Architecture Freeze Decision`（Q1–Q8）

| # | 冻结内容 | 遵守 | 落点 |
|---|---|---|---|
| Q1 | 准入 = manifest/import + admin approve；未知设备只进 `CLAIM_PENDING` | ✅ | `claimAuthorized()` + `device_event(type='register', actor='cloud')` |
| Q2 | **唯一硬证据 = 设备凭据成功连接**；禁止 2xx / 固定等待 / GET 反查作 ACTIVE 条件 | ✅ | `activateCredential()` **只**由确认帧调用；3 s 仅作下发节奏 |
| Q3 | `system.credential_set` 唯一入口；payload 最小 4 字段；password **单向** | ✅ | 下行 payload `{username,password,generation,slot}`（`action` 为可选扩展） |
| Q4 | 单一 Admin token（Cloudflare Secret）；**不入库/不记日志/不进 payload/常量时间/错误不泄露** | ✅ | `admin-auth.js`；实测缺失/错误/未配置 ⇒ **同一 401** |
| Q5 | EMQX 建账号**幂等**（`201`/`409` 均成功），**无需 GET+CREATE** | ✅ | `ensureUserPassword()` 盲重试 ≤5 次退避 |
| Q6 | P0 共享凭据**不立即删除**（四步迁移） | ✅ | 未触碰 legacy 账号 / legacy ACL |
| Q7 | `confirm timeout = 300 s` / `retry = 5`；四态**不得合并**；网络失败**不得**判 invalid | ✅ | `CONFIRM_TIMEOUT_S` / 四态派生 / `timeout:` 前缀 |
| Q8 | `credential_set` **只能**由 cloud provisioning path 调用；经 CommandManager 且**分离识别** | ✅ | 固件 `system_router` **allow-list 最前** + 独立分支 |

### 2.3 Phase D 补充冻结（Q9–Q13）

| # | 冻结内容 | 遵守 | 落点 |
|---|---|---|---|
| Q9 | 固件 `CredentialManager` 独占凭据状态机；**CloudManager 只做传输、不得感知状态** | ✅ | `CloudManager` 只取 `CredentialIdentity`（不透明串）+ 回报 `notify_conn_result` |
| Q10 | **只有** `object=='credential_set' && confirmed===true` 可 `PROVISIONING→ACTIVE` | ✅ | `classifyCredentialFrame()`（**发现并修的缺陷 #2**） |
| Q11 | 首次签发与轮换**同构**，首次**不跳过** device testing | ✅ | `provisionCredential()` 单一路径；实测 gen6/gen7 均经试连 |
| Q12 | 云端**永不写** `state='TESTING'`；**禁止改 migration** | ✅ | `0001` 零改动；"测试中"由 `PROVISIONING` + metadata 派生 |
| Q13 | `CommandMessage.source` = **security context**；`serial`/internal 禁止 | ✅ | 串口下发实测 ⇒ `e:13`、**不写 NVS** |

### 2.4 本阶段"禁止改动"清单（全部为 **0 改动**）

`migration 0001` · `wrangler.toml` 的 D1 绑定 · `src/services/device_identity.*` · `src/services/topic_renderer*` · `src/services/system_command.cpp`（restart 契约） · `platformio.ini` · `data/**`。

---

## 3. 交付物

### 3.1 云端 `Cloudflare_Assets`（`main`）

| 文件 | 状态 | 内容 |
|---|---|---|
| `guo-feeder-api/src/credential.js` | **新增** | ①–⑧ 编排 · `handleCredentialConfirm` · `classifyCredentialFrame` · `revokeDevice` · `reclaimCredential` · `reconcileTimeouts` · `deriveProvisionState` |
| `guo-feeder-api/src/device-registry.js` | 扩展 | 建档 / 事件 / 准入 / 凭据读写 / 四态派生 |
| `guo-feeder-api/src/emqx-admin.js` | 扩展 | `ensureUserPassword` · `updateUser` · `getUser` · `listUsers` · `deleteUser` · `listClients` · `publishToDevice` |
| `guo-feeder-api/src/index.js` | 扩展 | `/api/admin/**` 全套端点 + ingest 建档与 confirm **显式分支** |
| `README.md` | 更新 | 路由 / 模块 / **8 条不变量** / 待办 / 本地开发坑 |
| Secrets | 线上 | `EMQX_APP_ID` · `EMQX_APP_SECRET` · `WEBHOOK_SECRET` · **`ADMIN_API_TOKEN`** |
| 部署 | 线上 | Worker 版本 `2355cfe8-6040-4b4a-aba9-2cbcef35678e` |

**端点**：`GET /api/admin/device/:id` · `POST …/claim` · `POST …/credential` · `POST …/credential/rotate` · `POST …/revoke` · `POST /api/admin/reconcile` · `POST /api/ingest`（`X-Webhook-Key`）。

### 3.2 固件 `PIO_Assets/Guo_Feeder_Project`（`wb`）

| 文件 | 状态 | 内容 |
|---|---|---|
| `src/services/cred_store.{h,cpp}` | 新增 | NVS `gfcred` 双 slot（先写内容后写 state，规避半写） |
| `src/services/credential_manager.{h,cpp}` | 新增 | 凭据状态机 `IDLE→WAITING_TEST→PROMOTING/ROLLBACK` |
| `src/services/command_manager.cpp` | 修改 | `credential_set` allow-list 独立分支 + Q13 来源门控 + 错误码 14/15/16 + confirm 唯一出口 |
| `src/cloud/cloud_manager.cpp` | 修改 | 取身份 / 回报连接结果 / **删明文打印** / 报文脱敏 / **换连前等 outbox 排空（缺陷 #1 修复）** |
| `src/main.cpp` | 修改 | `credential_manager_init()` 早于 `cloud_init()`；`credential_manager_task()` 入 loop |

**资源**：RAM **40.0 %**（131,144 B）· Flash **66.2 %**（1,388,721 B）—— 均在 80 % 预算内。

### 3.3 文档

`P1-0-EMQX-Capability-Test.md`（+A-7 / **+A-8 / A-8b**）· `P1-3-PhaseD-实施计划.md`（+§7 结果）· `P1-实现清单.md`（状态）· `cloud_protocol.md`（`credential_set` 正文 + Q13 + **确认帧投递保证**）· `HANDOFF.md`（进度 + 两处缺陷）· 本文件 · Cloudflare `README.md`。

### 3.4 测试资产（`.pio/`，**不入库**）

| 脚本 | 覆盖 | 结果 |
|---|---|---|
| `d3_local_test.py` | 云端离线全量（本地 D1 + 真 EMQX） | **43 / 43** |
| `d3_e2e.py` | **真机**端到端 D3-1…D3-8 | **51 / 51** |
| `p1_0_a7.py` / `p1_0_a8.py` / `p1_0_a8b.py` | EMQX 能力探针（幂等 / 改密 / PUT schema） | 全绿 |

---

## 4. 分阶段结果

| 阶段 | 内容 | 结论 |
|---|---|---|
| **D-1** | 云端基础能力（`emqx-admin` / `admin-auth` / `credential-gen` / `device-registry`；仅 `/api/ingest` + 状态查询） | ✅ 24 项本地验证 + 12 项 EMQX 单元验证 |
| **D-2** | 固件凭据管理（`gfcred` 双 slot + `CredentialManager` + `credential_set` + source 门控） | ✅ 上板验证：正向 promote / 负向 rollback / source 拒绝 / 明文纪律 |
| **D-3** | 联调完整 provisioning（云端 `credential.js` + 端点 + ingest 确认） | ✅ **真机 51 / 51** |

### D-3 验收明细（真机 `288485896ce4`）

| 用例 | 结果 |
|---|---|
| **D3-1** 首次签发 | ✅ 建档 `CLAIM_PENDING` → 未准入 **403** → 准入 ⇒ gen6 试连 `_t6` ⇒ CONNACK 自证 ⇒ `ACTIVE` + `acl_ready_at` 非空 + `REGISTERED` |
| **D3-2** 认证失败 rollback | ✅ 设备 `rollback (auth_failed)` ⇒ 恢复原 ACTIVE 重连；**D1 原 ACTIVE 未被误判** |
| **D3-3** ACL 延迟处理 | ✅ `acl_wait_ms` 3000–3012 ms；负向「只建账号不写 ACL」⇒ CONNACK ok 但 **`subscriptions_cnt=0`**（可检测），补 ACL 后恢复 1 |
| **D3-4** confirm 超时 | ✅ 合成设备真实等满 **300 s** ⇒ `timeout:confirm` / 四态 `TIMEOUT`，**未产生 ACTIVE 行** |
| **D3-5** rotate | ✅ gen7 `ACTIVE`、gen6 `REVOKED`（**切主**）；同时刻至多 1 条 ACTIVE |
| **D3-6** revoke | ✅ ACL 收紧 `deny #` + 踢会话 ⇒ 设备侧 `MQTT disconnected`；`device/credential = REVOKED`；审计成对 |
| **D3-7** old session terminate | ✅ ACL 收紧**后会话仍在**（≠ 完整 revoke）；踢连接后才断 |
| **D3-8** D1 一致性 | ✅ `device=REGISTERED` ↔ `credential gen7=ACTIVE` ↔ 设备 `gfcred ACTIVE`；无 `password` 泄漏 |

---

## 5. 缺陷与修复（**本阶段最重要的产出**）

> 三处都**不是**设计缺陷，而是"实现细节在真机/真边界上才暴露"的问题。已全部修复 + 回归。

| # | 层 | 缺陷 | 现象 | 修法 |
|---|---|---|---|---|
| **1** | 固件 `cloud_manager.cpp` | **换连前不同步 outbox** | `esp_mqtt_client_enqueue`（QoS1+store）**入队成功 ≠ 已投递**；promote 后立即 `stop+destroy` ⇒ 刚入队的 `credential_confirm` **被一起销毁**。串口有 `[Cloud UP] OK`，云端**永远收不到** ⇒ 凭据卡 `PROVISIONING`、300 s 后 `timeout:confirm` | 身份变化先进入「**等 outbox 排空**」（非阻塞：`outbox==0 && ≥400 ms` 或 `≥2500 ms` 上界）再重建；新增标志 `[Cloud] credential switch: outbox=N -> rebuild` |
| **2** | 云端 `credential.js` | **把"信息帧"误判为失败帧** | 设备的 `ack` 与**受理帧**同样带 `o:"credential_set"` 但**无 `confirmed`**；旧实现写入 `provision_error='device:unknown'` ⇒ 真确认帧到达时被**硬失败守卫**拒绝（`acl_ready_at` 恒为 NULL） | 新增 `classifyCredentialFrame()`：`confirmed:true`⇒confirm；`confirmed:false`/`s>=3`/`status∈{failed,error,timeout}`⇒failure；**其余 info（不动状态）** |
| **3** | 云端 `activateCredential` | **切主撞唯一索引** | 先升新 `ACTIVE`、后降旧 `ACTIVE` ⇒ 撞 `ux_cred_active`（`UNIQUE(device_id) WHERE state='ACTIVE'`） | 同一 D1 **`batch` 事务内「先降旧、再升新」** |

**通用教训（已写入记忆 / 技能）**
- 任何"等待某事件"的状态机，**进入等待态必须清空残留标志**（D-2 曾因此未验证即 promote）。
- **"入队成功"≠"已投递"**：凡换连/重连/销毁客户端前，必须等 outbox 排空。
- 语义分类要**显式三分（成功 / 失败 / 信息）**，不要用"是否等于成功"这种二值判断去吞掉中间态。

---

## 6. 验收 DoD 对照

**Phase C §14 完成判据**

| # | 判据 | 结果 |
|---|---|---|
| 1 | 未知 `device_id` 上行 ⇒ 停在 `CLAIM_PENDING`，无自动升级路径 | ✅ |
| 2 | 准入后签发链路可复盘（`device_event` 时间轴） | ✅ `register / credential_issue / acl_update / credential_activate / credential_rotate / session_terminate / retire` 齐全 |
| 3 | `PROVISIONING → ACTIVE` 每次转移都有证据（`acl_ready_at` 非空） | ✅ D1 CHECK + 实测 |
| 4 | 只建账号不写 ACL ⇒ 能连不能收**可复现且可检测** | ✅ `subscriptions_cnt=0` |
| 5 | 退役路径**同时**含 ACL 与踢连接 | ✅ 成对 + 设备侧观察到断开 |
| 6 | 全链路无明文密码（负向 grep） | ✅ 响应 / 事件 / 凭据投影 / 串口 |
| 7 | P0 身份链路零回归 | ✅ `device_id` / topic 渲染未改（文件级 0 改动 + 真机 `client_id=dev_288485896ce4`） |

**D-3 端到端 8 项**：见 §4 —— **全部通过**。

---

## 7. 遗留与风险

| # | 项 | 级别 | 说明 / 建议 |
|---|---|---|---|
| 1 | **EMQX 数据集成端点仍为 `/`**（未迁到 `/api/ingest`） | 🟡 中 | Serverless 的规则引擎**只能控制台改**（API 实测 `/rule_engine/*`、`/connectors`、`/actions` 均 **403**）。迁移步骤见 §8；`/api/ingest` 已就绪并验证 |
| 2 | 根路径 `/` 的 `cloud_api` 通路**无鉴权** | 🟡 中 | P0 遗留；随 P4 Pages 改版收敛 |
| 3 | **轮换窗口**：服务端换密 ↔ 设备切主之间存在"旧密码已失效、设备仍持旧凭据"的窗口 | 🟠 高（已知限制） | 根因：`username` 冻结 + EMQX 单账号单密码。缓解：**仅设备在线时轮换** + 网络类失败**不弃** ACTIVE。彻底消除需"双账号并存"（P1-5 后或 APP 阶段设计） |
| 4 | 无 `test/` 受跟踪用例集 | 🟡 中 | 当前证据在 `.pio/`（不入库）；建议把 D3-1…D3-8 落成受跟踪资产 |
| 5 | `ADMIN_API_TOKEN` / `WEBHOOK_SECRET` 为**单一静态**凭据 | 🟡 中 | P1 明确冻结为过渡方案；APP 阶段换 `user auth + RBAC` |
| 6 | P0 共享凭据仍在（`GuoFeederDevice*`） | 🟢 低 | Q6 冻结：**全部设备切一机一密后**再下掉；需**改 ACL + 踢会话**成对 |
| 7 | 对账目前需**手工触发** | 🟢 低 | 可按需挂定时 Automation（无常驻轮询） |

---

## 8. 数据集成端点迁移（`/` → `/api/ingest`）

**目标**：让 EMQX 的上行投递走**带密钥校验**的 `/api/ingest`，并保持根路径兼容以便回滚。

**已由本次完成的部分**
- `/api/ingest` 已实现并**实测**：`X-Webhook-Key` 缺失/错误 ⇒ **401**；正确 ⇒ 200 且落库；
  **不接受** `source="cloud_api"`；建档与 `credential_confirm` 分支均在该端点生效。
- 根路径 `/` 保持 P0 行为（无鉴权，**同样**执行建档/确认副作用）⇒ 迁移期间**双通路安全**。
- `WEBHOOK_SECRET` 已轮换（值只存 Worker Secret，不落仓库）。

**需在 EMQX 控制台执行（无 API 可用，约 2 分钟）**

| 步 | 操作 | 值 |
|---|---|---|
| 1 | 数据集成 → 打开现有规则 → HTTP Action → **URL** | `https://guo-feeder-api.guobaby.workers.dev/api/ingest` |
| 2 | 同一 Action → **请求头** | 新增 `X-Webhook-Key: <WEBHOOK_SECRET>`（与 Worker Secret 同值） |
| 3 | 保存并**用真机上行验证**（见 §9） | — |

**回滚**：把 URL 改回 `https://guo-feeder-api.guobaby.workers.dev/`（头可留，根路径忽略它）。

---

## 9. 给 P1-5（绑定）的接口与前置

**已就绪的输入**

| 项 | 现状 |
|---|---|
| 设备必须已 `REGISTERED` 且有 `ACTIVE` credential | ✅ 本阶段达成（`credential.state='ACTIVE'` + `acl_ready_at` 非空） |
| `device.state` 六态机 | ✅ `FACTORY → CLAIM_PENDING → REGISTERED → …`，`BOUND/UNBOUND` **已预留未使用** |
| `device_binding` 表（追加式 + 部分唯一索引保证唯一 owner） | ✅ 已在 `0001` 建好（**未使用**） |
| `device_event.type` 已含 `bind / unbind / transfer` | ✅ DDL 层已允许 |
| 凭证/身份**不因解绑而变** | ✅ 冻结（Freeze #5/#6）：解绑 ≠ 吊销 |

**P1-5 必须遵守**
1. **绑定不改凭据、不踢会话**（归属关系 ≠ MQTT 身份）；只有**退役**才 revoke。
2. 转让 = 「关旧 + 开新」**同一事务**（否则部分唯一索引会拒绝）。
3. 分配/解除 owner 属**控制面**，与 `credential.js` 一样走 `/api/admin/**`（`ADMIN_API_TOKEN`）直到 APP 鉴权就位。
4. 每个动作写 `device_event`（`bind`/`unbind`/`transfer`，`actor='user'|'cloud'`）。
5. **域名/事件/状态语义不得复用 `credential` 命名空间**（避免与 Q10 的确认语义混淆）。

**P1-5 需要新引入的**：用户标识的**来源**（P1 无用户体系 ⇒ 先用外部 `user_id` 字符串 + 管理端操作，APP 阶段再接真实鉴权）。

---

## 10. 放行结论

| 维度 | 结论 |
|---|---|
| 功能 | ✅ 达成（P1-2 / P1-3 / P1-4 全部闭环，真机验证） |
| 安全 | ✅ 一机一密 · 准入门控 · 明文边界 · 管理端点常量时间鉴权 · **无自动升级路径** |
| 稳定性 | ✅ 非阻塞状态机 · 幂等键 · 盲重试 · 四态可对账 · 两处真实缺陷已修并回归 |
| 回归 | ✅ P0 身份链路零改动；根路径通路保持兼容 |
| 遗留 | 🟡 数据集成端点迁移（控制台，§8）· 🟠 轮换窗口（已知限制，§7-3） |

**建议**：**Phase D 通过评审，放行 P1-5 规划**；同时把 §8 的控制台迁移与 §7-4 的用例固化列为 P1 收尾项。
