# 阶段四 · MQTT 客户端与安全优化 —— 实测、选型与报告（2026-10-25）

- 计划：`GuoFeeder-Optimization-Phase4-Plan-MQTT-Security-2026-10-25.md`（先于编码产出）
- 依据：`GuoFeeder-Optimization-Plan-Rev2-2026-10-24.md` 阶段四（P4-1 / P4-2 / P4-3）
- 结论：**P4-1 六项实测完成；P4-2 选型定为「维持方案 A，不实施浏览器 MQTT」；P4-3 完成 B-1 只读复核**
- ★ **本阶段零产品代码改动 ⇒ 未部署任何组件**（理由见 §4）

---

## 一、最重要的两条实测发现

这两条此前**从未被验证过**，且直接决定了阶段四的走向。

### 发现 1：改密**不会**断开既有连接

| 步骤 | 观测 |
|---|---|
| 测试用户用密码 A 建立 MQTT 连接并保持 | 在线，PINGREQ→PINGRESP 正常 |
| 通过 API 把密码改成 B | HTTP 200 |
| 改密后旧连接 | **仍在线**（`/clients` 可见，PINGRESP 正常） |
| 密码 B 新建连接 | rc=0（成功） |
| 密码 A 再试 | rc=4（凭据错误） |

⇒ **凭据轮换不会让已建立的旧会话失效**。

**对现有代码的影响**（已固化成回归测试）：
- `provisionCredential()`（轮换/初次签发）= 改密 + 重写 ACL，**不踢连接**
- `revokeDevice()`（停用）= 收紧 ACL + **显式踢全部会话**（前缀匹配 + 正式 ID 兜底，404 视为成功）

⇒ **只有「停用」路径会真正终止会话**。安全含义：**凭据泄漏后执行轮换，旧会话在 keepalive 到期前仍然有效**。

> ⚠️ 这**不是**本次引入的缺陷，是既有行为；本阶段**未修改**它（改生产行为需授权）。
> 已在 `tests/api/emqx_credential_rotation.test.mjs` 固化为基线：将来若改为「轮换也踢」，该测试会先变红。

### 发现 2：新建用户默认**无任何 pub/sub 权限**，且 ACL 无法用 API 授予

| 端点 | 结果 |
|---|---|
| `GET /authentication/password_based:built_in_database/users` | **200 ✅**（用户管理可用） |
| 建/改密/删用户 | **可用**（201 / 200 / 204，实测通过） |
| `/authorization/built_in_database/rules` | **403 ❌** |
| `/authorization/settings`、`/sources`、`/cache`、旧 `/acl` | **403 ❌** |
| `GET /clients*`（`like_clientid`） | **200 ✅** |
| `DELETE /clients/{id}` | **可用**（204；不存在时 404 `CLIENTID_NOT_FOUND`） |
| `GET /status` | 403 |

实测：新用户 CONNECT **rc=0**（能连），但 SUBSCRIBE 任意 topic 均返回 **`SUBACK rc=0x80`（订阅被拒）** ⇒ EMQX 默认 **deny**。

⇒ **权限必须显式授予，而授予权限的 API 不可用** ⇒ 自动化**无法**为浏览器用户签发最小权限。

> 📌 修正了历史存档 `EMQX-Accounts-Inventory-2026-10-23.md` 的笼统结论「账号/ACL 无法从 API 读取」：
> **用户管理其实可用**（路径是 `/authentication/password_based:built_in_database/users`），
> 真正不可用的只是 **ACL 管理**（`/authorization/*`）。

---

## 二、P4-1 六项实测逐条结果

| # | 项 | 结果 | 证据 |
|---|---|---|---|
| 1 | WSS 8084 浏览器可连 | ✅ | `HTTP/1.1 101 Switching Protocols`，子协议协商到 `mqtt` |
| 2 | 建/改密/删用户 API | ✅ | 建 `201` → 读回 `200` → 改密 `200` → 删 `204` |
| 3 | **改密是否断开既有连接** | ✅ 已验：**不断开** | 旧连接 PINGRESP 正常且仍在 `/clients`；新密码 rc=0、旧密码 rc=4 |
| 4 | `DELETE /clients/{id}` 生效时延 | ✅ **1.2 秒** | 204 后轮询至消失 |
| 5 | ACL 能否限制到单设备 topic | ⛔ **无法用 API 验证/授予** | `/authorization/*` 全 403；改为验证「默认行为」= 全拒（SUBACK 0x80） |
| 6 | 会话结束后的清理 | ✅ 彻底 | 删用户后重连 rc=5（未授权）；再 GET 用户 `404`；残留 `gf_p4_` 连接 `[]` |

补充（端口探测）：

| 端口 | 结果 |
|---|---|
| 8084（WSS） | 开放 · TLSv1.3 ✅ |
| 8883（MQTTS） | 开放 · TLSv1.3 ✅（本轮 MQTT 协议实测走这里） |
| 1883（MQTT 明文） | **不可达**（Serverless 未开放） |
| 443 | 不可达 |

**隔离证明**（全程未触碰生产账号）：
- 只操作 `gf_p4_probe_<ts>` / `gf_p4_c_<ts>` 前缀的用户与 client_id
- 脚本内置 `guard()`，命中 `GuoFeederDevice` / `test001` / `shouji` / `workbuddy` / `dev_288485896ce4` 立即 `exit 1`
- `GuoFeederDevice` 账号与其 6 条 ACL **零改动**
- 结束即删用户 + 踢测试连接，最终残留 `[]`

---

## 三、P4-2 三方案选型结论

| 方案 | 描述 | 结论 |
|---|---|---|
| **A** | Worker 代理命令（现状） | ✅ **维持** |
| B | 浏览器 MQTT 直接发命令 | ❌ 不实施 |
| C | 浏览器 MQTT 只读（状态/遥测），命令仍走 Worker | ❌ **当前不可行** |

**为什么不实施 B**：丢失 registry 校验 / 审计写入 / 云端去重 / 限流；且浏览器用户需**发布**权限，
而发布权限无法用 API 授予（发现 2）。

**为什么不实施 C**（原本的推荐候选）：C 的安全前提是「浏览器用户只能订阅自己设备的 topic」。
但：

1. 新建用户默认 **deny**，权限必须显式授予；
2. 授予权限的 **ACL API 全 403**；
3. 唯一可行的替代 = 在 EMQX **控制台手工预置一个静态账号 + 静态 ACL**，所有浏览器会话共用
   ⇒ 与现有 `GuoFeederDevice` 共享账号是**同一模式**，会**复现 P1 已知风险**（无按会话/按用户隔离）。

⇒ **方案 C 的安全前提在本环境不成立**。按规划「B 仅在明确接受保障降级且经独立安全评审后才考虑」，
且「UI 命令迁移到 MQTT 待用户在全阶段结束后提出」——**本阶段不实施任何浏览器直连 MQTT**。

**session minutes 预算**（若将来要实施）：网页端 1 连接常开 ≈ 1,440 分钟/日 ⇒ 必须「按需连接、隐藏即断」。
本阶段未产生任何浏览器连接，预算零消耗。

---

## 四、为什么本阶段没有部署

**零产品代码改动** —— 没有新增/修改任何 `guo-feeder-api/src` 或 `guo-feeder-pagesdev` 下的产品代码。

产出物只有：

| 文件 | 性质 |
|---|---|
| `scripts/p4_emqx_probe.py` | P4-1 实测脚本（可复现证据；内置保护名单守卫） |
| `tests/api/emqx_credential_rotation.test.mjs` | 21 条行为契约回归测试（纯静态，零副作用） |
| 本文档 + 计划文档 | 报告 |

⇒ 无可部署内容。Worker 仍为阶段三的 `c6720d7f`，Pages 仍为 `897f151e`，均未变动。

---

## 五、P4-3 旧任务只读复核

### B-1 `deviceIdFromTopic` 归属 —— 已实施**且已上线**，本轮无需处理

- 代码：`guo-feeder-api/src/device-registry.js::deviceIdFromTopic`（212 行），
  含 B-1 产物：方向白名单扩到 `up/down/log`、支持 `$tenants/<t>/` 固定剥 2 段、
  失败改写显式哨兵 `UNRESOLVED_DEVICE_ID = "unresolved"`（不再回落 `http_api` 等伪 ID）。
- 提交：`204212e`（B-1）+ `e1e8b79`（B-1b）。
- **上线证据**：生产旧库中 `mqtt_messages.device_id = 'unresolved'` 有 **459 条**
  （`first_at` 约 10-19，`last_at` 约 10-21）⇒ 新哨兵值确实由生产代码写出。

> 📌 这 459 条属于 **G4-R1 漏记**范畴（无法解析出 device_id 的消息），
> B-1 只是把「错误归属」变成「显式未解析」，**不是新问题**。本轮不修。

### 其余条目（需用户确认，本轮不动账）

G4-R1（mqtt_messages 漏记）· G4-R2（`drop_*` 未落库）· G4-M1（缺瞬时采样）·
S-3-O2（真机验证排期）· WP-5（门禁已解除，待排期）· 批量关账清单
（#64/#70-74/#101/#104/#106-111/#127-129/#145/#147/#148/#96）。

---

## 六、测试结果

| 套件 | 结果 |
|---|---|
| `tests/api/emqx_credential_rotation.test.mjs`（新增） | **21 PASS / 0 FAIL** |
| 根套件 `tests/api` + `tests/pages`（11+1 套件） | **943 PASS / 0 FAIL**（较阶段三 +21） |
| Worker 全量 `run_all.mjs` | 与阶段三一致（本阶段未触及 Worker 源码） |

**无新增失败。**

---

## 七、Git

| 仓库 | 内容 |
|---|---|
| `Cloudflare_Assets` | `scripts/p4_emqx_probe.py`（实测脚本）+ `tests/api/emqx_credential_rotation.test.mjs`（21 条契约测试） |
| `PIO_Assets` | 计划文档 + 本报告 |

阶段三的两个 commit 已在本轮开始前推送并核实（云仓 `30d2881`、固件仓 `a29f6b8`，均 0/0）。

---

## 八、遗留风险与需要你决策的点

1. **★★ 凭据轮换不失效旧会话**（发现 1）。当前只有「停用」会踢连接。
   若你认为「轮换就必须让旧凭据立刻失效」，`provisionCredential()` 需要补一次前缀踢连接 ——
   **这属于改生产行为，需你单独授权**（风险：可能踢掉正在重连的设备）。
2. **浏览器 MQTT 只读（方案 C）被 ACL API 403 阻塞**。若你仍想要它，
   需要你在 EMQX **控制台手工**为某个静态账号配置「只允许订阅 `guo_feeder/288485896ce4/+`」，
   并接受它是共享账号（复现 P1 风险）。
3. **WSS 8084 已验证可达**，但尚未用真实浏览器（MQTT.js）做过端到端验证；
   本轮用的是原生 MQTT over TLS（8883）+ WebSocket 握手（8084）组合验证。
4. **`GuoFeederDevice` 仍是共享账号**（P1 遗留）：所有设备共用同一认证用户名，
   按设备隔离的 ACL 无效。这与发现 2 叠加，是浏览器方案的根因。
5. 未验证项：`/authorization` 是否在 EMQX 控制台有替代开关（我无法访问控制台）；
   Serverless 是否有其它认证源（如 JWT）可用。

---

## 九、本阶段触及了什么

| 面 | 是否触及 |
|---|---|
| 生产 Worker / Pages 部署 | ❌ 未部署 |
| 生产数据库（旧库 / 新库） | ❌ 零改动（仅 1 条只读 SELECT 用于 B-1 取证） |
| EMQX 现有账号（`GuoFeederDevice` 等） | ❌ **零改动** |
| EMQX 现有 ACL | ❌ **零改动**（且 API 也改不了） |
| EMQX 规则 / Action / Connector | ❌ 零改动 |
| 固件 / 设备端 | ❌ 零改动 |
| EMQX 测试用户 | ✅ 建 1 个 `gf_p4_*`，**已删除**（残留 `[]`） |
