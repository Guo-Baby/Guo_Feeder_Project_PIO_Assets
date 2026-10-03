# P1-3 / Phase D 实施拆分计划（Implementation Plan，**待审核后编码**）

> **状态**：**计划稿（Plan Only，不含代码）** · 2026-10-03
> **前置**：Phase C ✅（设计冻结 Q1–Q13）· P1-1 D1 Schema ✅（**已 apply，禁止修改 `0001`**）
> **上游契约**：`P1-2-P1-4-注册与凭据签发设计.md`（含文末 **§Architecture Freeze Decision Q1–Q13**）
> **对应工作包**：`P1-实现清单.md` 的 **P1-3**（固件）+ **P1-2/P1-4 的云端落地**
> **纪律**：每个子阶段**独立 commit**；**编码前须本计划经人工审核通过**

---

## 0. 冻结边界（本计划全程不可越过）

| # | 禁止 | 说明 |
|---|---|---|
| 1 | 修改 **P0 identity**（`device_identity.*` / NVS `gfid` / `did`/`dver`） | P0 冻结 DV-1..DV-3 |
| 2 | 修改 **`topic_renderer()`** 与其契约 | P0 冻结 |
| 3 | 修改 **migration `0001`**（含放宽 CHECK） | Q12；云端**永不写 `state='TESTING'`** |
| 4 | 修改 **workflow / config 逻辑** | `credential_set` 不得触发任何业务副作用 |
| 5 | **保存 / 打印明文密码** | 不落 D1、日志、`device_event.payload`、串口；含修掉既有违规 |
| 6 | 改 `platformio.ini` / `data/**` | 本阶段无新配置项、无新模块（`gfcred` 走 NVS） |

### 0.1 本计划依据的 7 项设计修正（已并入 Phase C 冻结）

| 修正 | 落点 |
|---|---|
| ① `CredentialManager` 独立，CloudManager 只做传输 | **Q9**（+ §12 接口冻结） |
| ② `credential_confirm` 与普通 ACK 分离 | **Q10** |
| ③ 首次签发与轮换统一流程，不跳过验证 | **Q11**（+ §4.3 / §6.1） |
| ④ 不依赖 DB `state='TESTING'`，不改 migration | **Q12**（+ §4.2 四态派生） |
| ⑤ 实施顺序 D-1 → D-2 → D-3 | 本文件 §1–§3 |
| ⑥ Admin token 五项纪律 | **Q4**（补充表） |
| ⑦ `source` 升级为 security context + 更新协议文档 | **Q13**（文档更新在 D-2） |

---

## 1. Phase D-1 —— 云端基础能力（**不打开完整 provisioning flow**）

**目标**：把"可复用的地基"做出来并各自可测，**但先不接通"签发"这条业务链路**。

| 交付物（新增/修改） | 内容 | 不做 |
|---|---|---|
| `guo-feeder-api/src/emqx-admin.js` **新增** | EMQX 管理 API 的 Worker 版封装（`fetch` + Basic）：`createUser` · `replaceUserAclRules` · `deleteUser` · `deleteUserAclRules` · `kickClient` · `publishToDevice(topic, payload)`（**topic 参数化**）；统一把 HTTP 状态归一成 `{ok, status, code}` | 不写业务判断 |
| `guo-feeder-api/src/admin-auth.js` **新增** | `requireAdmin(request, env)`：解析 `Authorization: Bearer`；**常量时间比较**；缺失/错误/过期 ⇒ **统一 401 + 同一文案**；**不打印 token** | 不做用户体系 / JWT |
| `guo-feeder-api/src/credential-gen.js` **新增** | `generatePassword(32)`（base62，`crypto.getRandomValues`）；**不提供任何"记日志"接口** | 不落库、不返回给非 provisioning 路径 |
| `guo-feeder-api/src/device-registry.js` **新增** | `upsertDeviceOnIngest()`（幂等 `ON CONFLICT DO UPDATE`）· `getDevice()` · `deriveProvisionState()`（**四态派生**，Q7/Q12）· `appendEvent()`（`device_event` 审计） | 不做绑定/在线状态 |
| `guo-feeder-api/src/index.js` **修改** | 引入**路由**（`url.pathname`）：`GET /`（探活，保持原样）· **`POST /api/ingest`**（把现有 webhook 逻辑迁入 + **启用 `X-Webhook-Key` 校验** + 从 topic 2 级路径取 `device_id`）· **`GET /api/admin/device/:id`**（状态查询，走 `requireAdmin`） | **不实现** `/credential`（签发）· 不实现 `/rotate` `/revoke` `/claim` |
| `guo-feeder-pagesdev/functions/api/command.js` **修改** | 转发时带上 `device_id`（供 D-3 定向下发用） | 不做 UI 改版（P4） |
| `wrangler.toml` **不改** | D1 绑定保持 | — |

**⚠️ 需你手工执行（我无法代填密钥值）**：
```bash
npx wrangler secret put ADMIN_API_TOKEN --config guo-feeder-api/wrangler.toml
npx wrangler secret list --config guo-feeder-api/wrangler.toml   # 只显示名字
```

**D-1 验收（可独立完成，不碰设备）**

| # | 判据 |
|---|---|
| 1 | `POST /api/ingest` 带正确 `X-Webhook-Key` ⇒ 新 `device_id` 建档为 `CLAIM_PENDING`；重复上报 **不重复建档** |
| 2 | 缺 / 错 `X-Webhook-Key` ⇒ **401**（同一文案） |
| 3 | `GET /api/admin/device/<id>` 无 token ⇒ **401**；有正确 token ⇒ 返回 `device.state` + 四态派生结果 |
| 4 | **负向 grep**：`ADMIN_API_TOKEN` 字面量不出现在任何被跟踪文件；Worker 日志无 token |
| 5 | `deriveProvisionState()` 单元用例：构造 4 组行，断言四态判定正确（含 `timeout:` 前缀分支） |

---

## 2. Phase D-2 —— 固件（`gfcred` + `CredentialManager` + `credential_set` + `credential_confirm`）

**目标**：设备侧具备"接收凭据 → 试连验证 → 提升 → 回滚"的完整能力，**且 CloudManager 不参与状态判定**。

| 交付物 | 内容 |
|---|---|
| `src/services/cred_store.{h,cpp}` **新增** | NVS namespace **`gfcred`**（**与 `gfid` 物理隔离**）；双 slot `slot0`/`slot1`；每 slot 存 `username` / `password` / `generation` / `state`；`Preferences` 开/关成对（沿用 `device_identity.cpp` 的写法风格）；**不打印内容** |
| `src/services/credential_manager.{h,cpp}` **新增** | 凭据状态机（**Q9 独占方**）：`init()` · `task()`（非阻塞，挂 loop）· `handle_set(payload)` · `get_active(&u,&p)` · `notify_conn_result(ok, reason)`；**promote**（TESTING→ACTIVE + 旧 slot→INVALID）· **rollback**（丢弃 TESTING，保留 ACTIVE） |
| `src/services/command_manager.{h,cpp}` **修改** | ① `system_router()` **最前**判定 allow-list `credential_set` → 独立分支；② **Q13 来源门控**：仅 `cmd.source == "cloud"` 放行，`serial`/空 ⇒ 拒绝 + WARN + **绝不写 NVS**；③ 新增错误码 **14/15/16**（`CRED_INVALID_PAYLOAD` / `CRED_STORE_FAIL` / `CRED_GENERATION_STALE`）；④ **不改** `command_manager_execute()` 一级路由语义 |
| `src/cloud/cloud_manager.cpp` **修改** | ① `cloud_init()` 凭据来源 = `credential_manager_get_active()` **优先**、配置兜底；② **删除明文凭据串口打印**（`username` / `password` 两处 ⇒ **D-5 硬要求**）；③ 新增 `cloud_connect_with(user,pass)` / `cloud_reconnect()`；④ CONNACK 结果经 `cloud_notify_conn_result(ok, reason)` **上报**，**不自行判定语义**；⑤ **不引入任何 `ACTIVE/TESTING/generation/state` 概念** |
| `src/main.cpp` **修改** | 初始化顺序：`credential_manager_init()` 早于 `cloud_init()`；`credential_manager_task()` 加入 loop |
| `src/log/log_events.h` **（必要时）修改** | 凭据类 EventId（若缺维度）。⚠️ **新增 WARN 埋点会扰动 LogManager 496 条环计数** ⇒ 优先用 INFO（只上云不落 Flash），并登记回归影响 |
| `docs/interfaces/cloud_protocol.md` **修改** | 增补 `credential_set` / `credential_confirm` 协议正文 + **Q13 的 source 规则**（§2.3 新格式字段表同步） |
| `test/` **新增** | 见 §2.2 用例 |

### 2.1 ★ D-2 关键决策点（**需你审核**）

**试连（device testing）用哪个 `client_id`？**

| 方案 | 行为 | 评价 |
|---|---|---|
| **A. `dev_<device_id>`**（与正式一致） | 因 EMQX 的 `client_id` 唯一约束，新连接会**踢掉**旧会话（`reason=takenover`） | 简单，但**测试失败就瞬时断线**，需立刻用旧凭据重连；且会产生"设备被踢"告警噪音 |
| **B. 临时 `dev_<device_id>_t<generation>`**（**推荐**） | 与旧会话**并存**试连，成功后再切正式 `client_id` 重连 | **测试失败无感**（旧会话不受影响）；ACL 按 `username` 授权，与 `client_id` 无关 ⇒ 可行。代价：`client_id` 不再恒等于 `dev_<id>`（但**持久身份未变**，P0 契约针对的是**长期身份**，非试连瞬时值） |

⇒ **建议采用 B**；若你认为 `client_id` 必须始终恒等（更保守），则采用 A 并接受瞬时断线 + 立即回连。

### 2.2 D-2 验收（串口用例）

| # | 用例 | 期望 |
|---|---|---|
| D-1 | 凭据写入 → 重启 | 使用新凭据连接成功 |
| D-2 | 写入后**立即断电** | 启动后仍能连上（双 slot 不产生半写死锁） |
| D-2b | TESTING 遇**网络故障** | **不得**误判：原 `ACTIVE` 保持有效，TESTING 保留待重试 |
| D-3 | 凭据缺失/损坏 | 回退配置兜底 + 明确日志，**不阻塞启动** |
| D-4 | 轮换后旧凭据（服务端判定） | 被拒 |
| D-5 | **明文负向 grep** | 串口 / 日志**不出现**凭据明文（含修掉既有违规后的复查） |
| D-6 | P0 回归 | `gfid`/`did`/`dver` 与 topic 渲染结果**完全不变**（**编译产物字节比对**） |
| **新增 D-8** | 串口直接发 `cm {"cmd":"system","ob":"credential_set",...}` | **被拒**（`source="serial"`），**不写 NVS**，有明确错误码 |
| **新增 D-9** | 重放**旧 `generation`** | 拒绝（`CRED_GENERATION_STALE=16`） |
| **新增 D-10** | `credential_set` 后**无** `confirm` | 云端凭据停在 `PROVISIONING`（不得变 `ACTIVE`） |

---

## 3. Phase D-3 —— 联调完整 provisioning

**目标**：接通 ①–⑧ 全链路（Q11 统一流程）。

| 交付物 | 内容 |
|---|---|
| `guo-feeder-api/src/credential.js` **新增** | 签发链路实现：`claim_authorized()`（manifest / admin approve）· ①–⑧ 编排 · 幂等键 `(device_id, generation)` · 盲重试（`201`/`409` 成功，≤5 次指数退避）· `provision_error` 的 `<stage>:<detail>` 约定 |
| `guo-feeder-api/src/index.js` **修改** | 补齐端点：`POST /api/admin/device/:id/claim` · `POST /api/admin/device/:id/credential` · `/rotate` · `/revoke`；**ingest 侧新增 `credential_confirm` 分支（Q10）** |
| `Cloudflare_Assets/README.md` **修改** | 端点 + Secret + 验收命令 |
| `docs/architecture/P1-实现清单.md` **修改** | P1-2/P1-3 完成标记与用例结果 |

**D-3 端到端验收**

| # | 判据 |
|---|---|
| 1 | 未准入设备上行 ⇒ 建档 `CLAIM_PENDING`，**不签发** |
| 2 | 准入后签发 ⇒ 设备真实 `CONNACK` 成功 ⇒ 凭据 `ACTIVE`（`acl_ready_at` 非空）· `device.state = REGISTERED` |
| 3 | **只建账号不写 ACL** ⇒ 复现"能连不能收"，且**检测手段能发现**（C-4） |
| 4 | **普通 command ACK 到达时不改变凭据状态**（Q10 反证） |
| 5 | 轮换 ⇒ 新凭据 `ACTIVE`、旧世代 `REVOKED`（切主） |
| 6 | 退役 ⇒ `acl_update` **+** `session_terminate` 成对（缺一即失败） |
| 7 | `confirm` 不达 ⇒ 300 s 后 `timeout:confirm`（四态 TIMEOUT），可回收，**不产生 `ACTIVE` 行** |
| 8 | 全链路**无明文密码**（D1 / 日志 / event payload / 串口 四处负向 grep） |

---

## 4. 文件修改总表（合并视图）

**`Cloudflare_Assets`**
```
新增  guo-feeder-api/src/emqx-admin.js          (D-1)
新增  guo-feeder-api/src/admin-auth.js          (D-1)
新增  guo-feeder-api/src/credential-gen.js      (D-1)
新增  guo-feeder-api/src/device-registry.js     (D-1)
新增  guo-feeder-api/src/credential.js          (D-3)
修改  guo-feeder-api/src/index.js               (D-1 路由 → D-3 补端点/confirm 分支)
修改  guo-feeder-pagesdev/functions/api/command.js (D-1 带 device_id)
修改  README.md                                 (D-3)
Secret ADMIN_API_TOKEN                          (人工设置)
不动  guo-feeder-api/migrations/** · wrangler.toml 的 D1 绑定
```

**`PIO_Assets/Guo_Feeder_Project`**
```
新增  src/services/cred_store.{h,cpp}            (D-2)
新增  src/services/credential_manager.{h,cpp}    (D-2)
修改  src/services/command_manager.{h,cpp}       (D-2 独立分支 + source 门控 + 错误码)
修改  src/cloud/cloud_manager.cpp                (D-2 凭据来源 + 删明文打印 + 上报 CONNACK)
修改  src/main.cpp                               (D-2 初始化顺序 + task)
修改  src/log/log_events.h                       (D-2 视需要；评估环计数影响)
修改  docs/interfaces/cloud_protocol.md          (D-2 协议正文 + Q13)
新增  test/**                                    (D-2/D-3 用例)
修改  docs/architecture/P1-实现清单.md            (D-3)
不动  platformio.ini · data/** · device_identity.* · topic_renderer · system_command.cpp
```

---

## 5. 风险登记（D 级）

| # | 风险 | 缓解 |
|---|---|---|
| R-D1 | `gfcred` 写坏 ⇒ 设备失联 | 双 slot + 配置兜底 + 串口重配通道（D-3 用例） |
| R-D2 | 试连踢线（方案 A）⇒ 瞬时断线 | 采用方案 B 或"踢线后立即回连"（§2.1 待裁决） |
| R-D3 | 新增 WARN 埋点扰动 LogManager 环计数 | 优先 INFO；必要时登记回归基线变化 |
| R-D4 | Worker 加了路由，**既有 Pages 调试通路**（`/` 根 POST）被破坏 | `/` 根路径的 `cloud_api` 行为**保留兼容**，路由只**新增**不改旧 |
| R-D5 | `source` 门控被未来新入口绕过（BLE / Workflow） | 门控为 **allow-list**（默认拒绝）；新入口必须显式标注 source |
| R-D6 | 内存/栈：`CredentialManager` 若用 >1KB 结构上栈 | 结构体走静态/PSRAM 堆，**禁大结构上栈**（loopTask 余量 ≈6.5KB） |

---

## 6. 待你确认（编码前）

| # | 事项 | 我的建议 |
|---|---|---|
| 1 | 是否采用本计划的 **D-1 → D-2 → D-3** 拆分与各自的验收判据？ | 采用 |
| 2 | **§2.1 试连 `client_id` 方案 A / B** | **B**（临时 `_t<generation>`，失败无感） |
| 3 | D-1 是否**只**开放 `/ingest` 与 `/admin/device/:id`（即"不打开完整 provisioning flow"）？ | 是 |
| 4 | `ADMIN_API_TOKEN` 由你手工 `wrangler secret put` | 需要 |
| 5 | 本计划是否落为受跟踪文档（本文件即交付物） | 已落 |

> **审核通过后**：按 D-1 → D-2 → D-3 顺序实施，**每个子阶段独立 commit**；
> 任一子阶段完成即回报（改动文件清单 + 验证证据 + commit hash），再进入下一子阶段。
