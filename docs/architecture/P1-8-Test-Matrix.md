# P1-8 Test & Acceptance —— 测试矩阵与执行条件（Preparation Round）

> **性质**：只读测试准备 + 验收项核对 + 可立即执行项的实测。**未修改任何业务代码，未 commit。**  
> **日期**：2026-10-07 · **基线**：Firmware `e36d5ac`(wb) · EMQX `080cb3c`(main) · Cloudflare `97ca496`(main)，三仓 clean、均未 push。  
> **来源**：`P1-实现清单.md` §3（用例表）/ §4（DoD）· `P1-5E-Implementation-Plan.md` §6（T-1…T-20）· `P1-7-Review-Report.md` §7/§9 · `P1-6-3-E-closure-review.md`（V-14…V-18）。

---

## 0. 执行分类图例

| 代号            | 含义                      | 外部依赖                    |
| ------------- | ----------------------- | ----------------------- |
| **STATIC**    | 静态 / 离线 / 纯计算，无外部依赖     | 无                       |
| **BUILD**     | 需固件构建                   | PlatformIO 工具链          |
| **HW**        | 需 ESP32-S3 真机 + 串口      | COM8（`1A86:55D3` CH343） |
| **LIVE-MQTT** | 需 EMQX broker（可含真实凭据连接） | EMQX Serverless         |
| **LIVE-CF**   | 需**部署版** Worker + 真实 D1 | Worker + D1（生产）         |

---

## 1. 矩阵 A —— 云端用例（`P1-实现清单.md` §3.1）

| #   | 用例               | 判据                                                | 类别                       | 当前状态                                                                                                 |
| --- | ---------------- | ------------------------------------------------- | ------------------------ | ---------------------------------------------------------------------------------------------------- |
| C-1 | 新设备首次上行（未知 id）   | D1 建档；`state=CLAIM_PENDING`（**不得**自动升 REGISTERED） | LIVE-CF + HW             | 待执行（需新 device_id + 真机上行）                                                                             |
| C-2 | 凭据签发全链路          | 建账号 + 写 ACL + 落库三者一致；设备能连且能订阅自己的 down             | LIVE-CF + LIVE-MQTT + HW | 待执行（D/E 轮**已有同构 LIVE 证据**，但非本轮 C-2 编号）                                                               |
| C-3 | 签发后**立即**连接（不等待） | 依 A-1：或成功，或**可检测地失败**                             | LIVE-CF + LIVE-MQTT      | 待执行                                                                                                  |
| C-4 | 只建账号不写 ACL（故障注入） | 复现"能连不能收"；检测手段能告警                                 | LIVE-CF                  | 待执行（**故障注入**，需授权写 EMQX ACL）                                                                          |
| C-5 | 凭据轮换             | 新凭据可用；旧凭据按策略失效                                    | LIVE-CF + LIVE-MQTT      | 待执行                                                                                                  |
| C-6 | 凭据回收（退役）         | 账号禁用 ⇒ 连不上 + 在线会话被断开                              | LIVE-CF + LIVE-MQTT      | 待执行（D 轮 Stage-1/2 已有同构证据）                                                                            |
| C-7 | 绑定 / 解绑 / 转让     | 唯一 owner；转让原子；历史可追溯                               | LIVE-CF                  | **离线面已全绿**（见 §3 P1-5 套件 279 PASS）；**LIVE 面 = P1-5-5**（见 §5）                                          |
| C-8 | 在线状态             | 上线/离线/`last_seen` 正确；断电 T 秒内判离线（**P1 acceptance**）。**`fw_version` 判据 = `DEFERRED TO P3`（不参与 P1 验收）**      | LIVE-MQTT + LIVE-CF      | `online`/`stale`/`last_seen` **已由 P1-6-3 E 轮 LIVE 证明**；**`fw_version` 已裁决延至 P3（见 §9 N-1）** |
| C-9 | 容量               | P1 生命周期事件 < 100 行/天（11 台）                         | **STATIC**（D1 只读）        | ✅ **本轮已执行通过**（见 §6）                                                                                  |

---

## 2. 矩阵 B —— 设备侧用例（`P1-实现清单.md` §3.2）

| #    | 用例                  | 判据                                                           | 类别                                                   | 当前状态                                             |
| ---- | ------------------- | ------------------------------------------------------------ | ---------------------------------------------------- | ------------------------------------------------ |
| D-1  | 凭据写入 → 重启           | 使用新凭据连接成功                                                    | HW + LIVE-MQTT                                       | 待执行（**重新签发凭据会改动生产凭据**，需授权）                       |
| D-2  | 写入后**立即断电**（双 slot） | 启动后仍能连上                                                      | HW                                                   | 待执行（可破坏性：需断电）                                    |
| D-2b | TESTING slot 遇网络故障  | **不得**误判为凭据失败                                                | HW                                                   | 待执行                                              |
| D-3  | 凭据缺失/损坏             | 按兜底策略 + 明确日志，**不阻塞启动**                                       | HW                                                   | 待执行（可破坏性：需擦 NVS `gfcred`）                        |
| D-4  | 轮换后旧凭据              | 按策略被拒（服务端判定）                                                 | HW + LIVE-MQTT                                       | 待执行（含凭据轮换 ⇒ 高风险）                                 |
| D-5  | 明文凭据泄露检查（负向 grep）   | 串口/日志**不出现**明文凭据                                             | **STATIC + 日志**                                      | ⚠️ **本轮已执行（现状 PASS，另见 §7 发现 N-2）**               |
| D-6  | P0 回归               | `gfid`/`did`/`dver` 不受 P1 影响；Topic 渲染结果不变                    | HW                                                   | 待执行（需串口读启动日志）                                    |
| D-7  | **生产模式检查**          | 模板缺 `<device_id>` ⇒ ERROR + 阻止上线（仅 production；dev/test 不受影响） | **STATIC/BUILD 已 PASS**；**LIVE 复现 = HW + LIVE-MQTT** | **静态+构建面 ✅ 已 PASS（P1-7 CLOSED）**；**LIVE 复现见 §8** |

---

## 3. 矩阵 C —— 越权与隔离（`P1-实现清单.md` §3.3）

| #   | 用例                         | 判据                          | 类别        | 当前状态                                                                |
| --- | -------------------------- | --------------------------- | --------- | ------------------------------------------------------------------- |
| X-1 | 设备 A 用**自己的**凭据订阅 B 的 down | **DENY**                    | LIVE-MQTT | 待执行（P0 已验证同构场景 `SUBACK 0x80`）                                       |
| X-2 | 用泄露凭据伪造 B 的 `device_id` 上行 | **不可能**（凭据与 `device_id` 绑定） | LIVE-MQTT | 待执行                                                                 |
| X-3 | 回收后的凭据尝试连接                 | **失败**且日志可追                 | LIVE-MQTT | **D 轮已有 LIVE 证据**（`Connection refused: Not authorized`）；可按 X-3 编号复跑 |

---

## 4. 矩阵 D —— 验收 DoD（`P1-实现清单.md` §4）

| #  | 判据                                                     | 类别                       | 当前状态                                                                                            |
| -- | ------------------------------------------------------ | ------------------------ | ----------------------------------------------------------------------------------------------- |
| D1 | 新增设备**无需人工改配置/ACL**即可安全上线（端到端）                         | LIVE-CF + LIVE-MQTT + HW | 待执行                                                                                             |
| D2 | 两台设备凭据**互不相同**；任一台泄露不影响另一台                             | **STATIC**（D1 只读）        | ✅ **本轮已执行通过**（见 §6）                                                                             |
| D3 | 退役后凭据回收 ⇒ **确实连不上**                                    | LIVE-MQTT                | **D 轮已 LIVE 证明**（`Connection refused`）；可按 D3 编号复跑                                               |
| D4 | 云端可回答：**在线否 / 最后活跃**（**P1 acceptance**）；**固件版本 = `DEFERRED TO P3`**                            | LIVE-CF                  | `online`、`last_seen` 可达成（✅）；**`fw_version` 已裁决延至 P3（见 §9 N-1）**                       |
| D5 | **P0 身份回归零变化**（id / topic 渲染 / RAM·Flash 增量可归因）        | HW + BUILD               | BUILD 面可核；**HW 面待执行**                                                                           |
| D6 | 云侧**不含明文凭据**落库/落日志/入仓库                                 | STATIC + LIVE-CF         | **❌ BLOCKED —— 不得 PASS**：生产 D1 `mqtt_messages` 实测存在**明文密码**（下行 `credential_set` 回显 + 上行 `config_query` 响应）；已裁决为**真实安全冲突，必须修复**。见 §12 N-3 Remediation Design |
| D7 | 每包独立 commit + 提交前 `git diff --cached --name-only` 全量核对 | STATIC（流程）               | ✅ 可核（本阶段各 commit 已遵守；见 §6）                                                                      |

---

## 5. 矩阵 E —— P1-5-5 正式验收（`P1-5E-Implementation-Plan.md` §6，T-1…T-20）

| 段     | 用例                                                                                                                            | 类别                             | 当前状态                                                                                        |
| ----- | ----------------------------------------------------------------------------------------------------------------------------- | ------------------------------ | ------------------------------------------------------------------------------------------- |
| A     | **T-1…T-6、T-8…T-12、T-14…T-16、T-18、T-19**                                                                                      | **离线（`wrangler dev` + 本地 D1）** | ✅ **离线面已全绿**（`.tmp_p15/` P1-5 套件 **279 PASS / 0 FAIL**，本轮复跑）；**`T-19` 的部署版面亦已只读预检通过**（见 §6） |
| A+B   | **T-7、T-12、T-13、T-16**（D1 SQL 证据）                                                                                             | **本地/远端 D1**                   | 待执行（远端 D1 只读通道 ✅ 可用）                                                                        |
| **C** | **T-13**（batch 原子性，须真 D1 确认一次）· **T-17**（真机 UNBOUND 态持续上行）· **T-18**（部署版响应）· **T-19**（401 body 逐字节）· **T-20**（D3-6 revoke 回归） | **部署版 + 真机**                   | **待执行 —— 需授权**（详见 §8.3）                                                                     |

---

## 6. 矩阵 F/G —— 继承项

| 来源                                          | 项                                                                                                                | 状态                                      |
| ------------------------------------------- | ---------------------------------------------------------------------------------------------------------------- | --------------------------------------- |
| **P1-6-3-E**（CLOSED，commit `080cb3c`）       | `V-14` stale 限定符 · `V-15` keepalive 死亡检测 · `V-16` 硬退役阻断（**继承自 D LIVE**）· `V-17` 乱序+同秒 tie · `V-18` `/clients` 过滤 | ✅ **全部 PASS，直接继承，不在 P1-8 重跑**           |
| **P1-7**（CLOSED，commit `bdd57fb`/`e36d5ac`） | production topic enforcement（静态 + 构建面）                                                                           | ✅ PASS；**LIVE 复现 = 本阶段延期项 D-7**（见 §8.1） |

---

## 7. 本轮已执行的测试与结果

| #    | 项                       | 命令                                                                                                | 结果                                                                                                                                    |
| ---- | ----------------------- | ------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------- |
| E-1  | Cloudflare 离线契约套件       | `node test/run_all.mjs`（`Cloudflare_Assets/guo-feeder-api`）                                       | ✅ **o1_o10 83/83 · p163a 51/51 · hard_retirement 80/80 · legacy 126 PASS + 7 EXPECTED_FAILURE**；**0 FAIL / 0 UNEXPECTED_PASS**；exit 0 |
| E-2  | P1-5 离线验收套件（P1-5-1/2/3） | `node .tmp_p15/{verify,route_verify,p153_verify}.mjs`                                             | ✅ **92 / 139 / 48 = 279 PASS / 0 FAIL**                                                                                               |
| E-3  | P1-7 静态+二进制门控断言         | `python test/p1_7_prod_check.py --bins`                                                           | ✅ **27 PASS / 0 FAIL**                                                                                                                |
| E-4  | 固件 dev 构型构建             | `pio run`                                                                                         | ✅ `[SUCCESS]` · 0 error / 0 warning（增量 no-op；首轮全量 0 error / 2 既有 warning）                                                             |
| E-5  | 固件 prod 构型构建            | `PLATFORMIO_BUILD_FLAGS="-DGF_ENV_PRODUCTION=1" pio run`                                          | ✅ `[SUCCESS]` · 0 error / 0 warning / **0 `macro redefined`**                                                                         |
| E-6  | **C-9 容量**              | D1 只读聚合                                                                                           | ✅ `device_event` 峰值 **35 行/天**（实测 2–5 台设备；远 < 100/天 判据）                                                                               |
| E-7  | **DoD-D2 一机一凭据**        | D1 只读                                                                                             | ✅ 每 `device_id` 独立凭据行；生产设备 5 代（gen 3–6 `REVOKED`，**gen 7 `ACTIVE`**）⇒ 单 ACTIVE、逐设备隔离                                                  |
| E-8  | **D-5 静态面**             | `grep -rn "Serial\..*(mqtt_password\|mqtt_username\|cred_pass\|cred_user)" src/`                  | ✅ **0 命中**；`cloud_manager.cpp:1547-1548` 打印 `username=(hidden)` / `password=(hidden)`                                                 |
| E-9  | **D-5 日志面**             | 今日日志（`verify_boot.log` / `wifi_restore.log` / `wifi_verify.log` / `mqtt_restore.log`，15:42–15:46） | ✅ 明文 `password=` 命中 **0**；见 `credential source: gfcred (ACTIVE)` + `(hidden)`                                                         |
| E-10 | **D-7/T-19 部署版预检**      | Worker 只读 GET                                                                                     | ✅ 缺 token → **401**、错 token → **401**，**body 逐字节相同**；合法 token → 200                                                                   |
| E-11 | 就绪度探测                   | Worker / EMQX / D1 只读                                                                             | ✅ Worker 200（`state=REGISTERED`、`hard_retired=false`、`presence{online,stale,source,age,timeout,…}`）· EMQX 200（5 clients）· D1 200      |

**环境就绪度**：COM8（CH343）**在位**、无残留占用 · esptool ✅ · `serial_batch.py`/`reset_mon.py` ✅ · mqttx-cli ✅ · `ADMIN_API_TOKEN` / `WEBHOOK_SECRET` ✅ · 远端 D1 只读 ✅ · 生产设备 `dev_288485896ce4` **online（ka=120, pv=4）**。

---

## 8. 未执行项与执行前置

### 8.1 D-7 LIVE 复现 —— **条件具备，待授权**

**为什么未执行**：需**烧录 production 固件**并**改动设备端 MQTT 配置**（测试窗口内生产设备将离线），属破坏性窗口操作，超出「只读准备」范围，需单独授权。

**授权后执行步骤（建议）**：

1. 记录窗口前基线：EMQX `/clients`、D1 五表计数、生产设备四字段、串口最新启动日志。
2. 烧录 **prod 固件**（`.pio/build/p17prod/esp32-s3-devkitc-1/firmware.bin`，`write_flash 0x10000`，**不碰 NVS / LittleFS**）。
3. **负向分支**：`config_set` 三个 topic = legacy（`guo_feeder/down|up|log`）+ `config_save` → 重启；  
   预期串口：`[Identity] ERROR MQTT blocked (production policy: subscribe_topic lacks <device_id>)`，且**无** `[Cloud] MQTT connecting...`；  
   预期 EMQX：该 client 不出现 / `connected=false`；**无 subscribe**。
4. **正向分支**：`config_set` 三 topic = V3 模板 + `config_save` → 重启；预期正常上线（`[Cloud] MQTT connected` + `subscriptions_cnt=1`）。
5. 恢复基线并复核；**恢复 dev 固件**（可选，但建议）。
6. 产出窗口内 Before/After 对照（同 P1-6-3 E-3 报告口径）。

### 8.2 设备侧用例 D-1…D-6（HW）—— **待授权/待窗口**

D-1/D-3/D-4 涉及**重新签发或擦除凭据**（会改变生产设备 MQTT 身份）；D-2/D-2b 需**断电/断网**破坏性操作。建议排在同一维护窗口，并先建立**恢复路径**（串口 `config_set` + `config_save`，或重新走签发）。

### 8.3 P1-5-5 的 C 段（T-13/T-17/T-18/T-19/T-20）—— **fixture 已裁决冻结，待创建授权**

**现状（只读实测）**：`device_binding` **0 行**（干净起点）· `device` 5 行 = `288485896ce4`(**REGISTERED**，生产) · `3cfb56c8a5fc`(**CLAIM_PENDING**，E 测试设备) · `d163d0000001/2/3`(**REVOKED**，D 测试对象) · 9 行 `device_credential`。

**fixture 方案已裁决并冻结 —— 见 §11「P1-5-5 Fixture Freeze（已批准）」。** 本轮**不创建** P5-A/B/C（创建即生产写：ingest / claim / credential / D1 / EMQX ACL），留待下一轮人工授权。

**要点提醒**：
- **T-15 / T-20 会 `REVOKE`（不可逆）** ⇒ 各自使用**独立牺牲 fixture**（P5-B / P5-C），**绝不**用生产设备。
- **T-17 前置**：需生产设备 `288485896ce4` 处于 `UNBOUND` 态持续上行。**禁止**为 T-17 主动执行 `bind → unbind`（属生产 D1 写）。若测试代码要求**字面 `UNBOUND`**，则**前置不满足** ⇒ 报 `T-17 fixture prerequisite not satisfied`，等人工裁决（详见 §11.5）。
- 所有 `POST /api/admin/**` 均为**生产写** ⇒ 需明确授权。

---

## 9. 本轮新发现

| #       | 级别                 | 发现                                                                                                                                                                                                                                                                                                                                                                                                                        | 证据           | 影响                                                                                                                                                                                                                                                 |
| ------- | ------------------ | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ------------ | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| **N-1** | **MAJOR（文档/范围冲突）→ ✅ 已裁决** | **`fw_version` 在 P1 内不可达成**：云端**已就绪**（`device.fw_version` 列 + `upsertDeviceOnIngest()` 接受 `fwVersion` + `index.js:1122-1132` 从 ingest 取 `msg.fw ?? msg.fw_version` + `/api/admin/user/:u/devices` 暴露），但**固件从不上报**：上线通告仅 `{"cmd":"system","id":"online","src":"device"}`（`cloud_manager.cpp:1918`），`src/` 全量 grep `fw_version` **0 命中**；D1 实测 5 行**全为 `NULL`**。而 §4 **DoD-D4 要求"固件版本可查"**、§3.1 **C-8 要求 `fw_version` 正确**。 | 见左           | **✅ 裁决：`fw_version` = `DEFERRED TO P3`。** `online/offline/last_seen` 仍为 P1 acceptance；C-8 / DoD-D4 的 `fw_version` 判据已标注延至 P3（`P1-实现清单.md` §0.1 裁决注 + §P1-6 目标/验证 + C-8 + D4 均同步）。**收口不再因 `fw_version == NULL` 判失败。** |
| **N-2** | MINOR（本地卫生）        | `.pio/` 下 **108 个历史串口日志**仍含**明文旧共享凭据**（P0 遗留共享 `username` / `password`）—— 全部为 **DEF-2 修复前**的产物；**当前固件已脱敏**（今日日志 0 命中）。`.pio/` 已被 `.gitignore:4` 忽略，**不入库**。                                                                                                                                                                                                                                               | E-8/E-9      | 仅影响本机磁盘；与既有待办「**MQTT 密码轮换**」相关。**本轮不动**。                                                                                                                                                                                                           |
| **N-3** | **MAJOR（安全冲突）→ ❌ 未关闭** | **`mqtt_messages` 持久化明文密码，且不止一条路径**：① **下行** `credential_set` 回显（`credential.js:283-295` → EMQX Rule 回投 → `index.js` `raw_payload`）② **上行** `config_query` 响应（含 `wifi.password` + `mqtt.password`）③ **Worker 日志**（`index.js:348` `console.log(body)`）。生产 D1 实测：`direction='down'` **493** 行 · 含 `password` **56** 行（`http_api` 型）；**上行 `config_query` 含 password 32 行**。 | D1 只读 + 代码 | **✅ 裁决：真实安全冲突，`DoD-D6` 不得 PASS，采用严格修复方向。** 本轮**只做修复设计**（见 §12）。 |

> **N-1 已裁决（`fw_version` → `DEFERRED TO P3`）。N-3 = `DoD-D6` BLOCKER（未关闭，须先修复）。**
> N-3 不阻止本轮「文档 / 设计」工作，但**阻止 `DoD-D6` 通过**；破坏性 LIVE 测试仍全部暂缓（见 §10）。



---

## 10. 结论

- **P1-8 首轮（准备 + 可立即执行项）完成。** 环境就绪度**全部满足**（硬件 / broker / 部署版 Worker / D1 / 密钥 / 工具链）。
- **已执行并 PASS**：Cloudflare 契约套件（83+51+80+126/7）、P1-5 离线套件（279）、P1-7 断言（27）、双构型构建、C-9 容量、DoD-D2、D-5、T-19 部署版预检、就绪度探测。
- **✅ N-1 已裁决**：`fw_version` = `DEFERRED TO P3`；`online/offline/last_seen` 仍为 P1 acceptance（C-8 / DoD-D4 已同步标注）。
- **❌ N-3 未关闭（`DoD-D6` BLOCKER）**：明文密码经 3 条路径进入生产 D1 / Worker 日志；采用严格修复方向，**修复设计见 §12**。
- **P1-5-5 fixture 已裁决冻结**（见 §11）；**本轮不创建** P5-A/B/C。
- **暂缓（未授权）**：**D-7 LIVE**（§8.1）· 设备侧 **D-1…D-6**（§8.2）· **P1-5-5 C 段 T-13/T-17/T-18/T-19/T-20**（§8.3）· **任何 credential rotation / revoke / bind / unbind / transfer / 生产写**。
- **未 commit / 未 push**：三仓 HEAD 未变（FW `e36d5ac` / EMQX `080cb3c` / CF `97ca496`）。

---

## 11. P1-5-5 Fixture Freeze（已批准）

> **来源**：2026-10-07 人工裁决（P1-8 Acceptance Decision）。**本轮不创建任何 fixture**（创建即生产写：ingest / claim / credential / D1 / EMQX ACL）——留待下一轮授权。

### 11.1 新建设备（一次性，标准生产链 ingest → claim → credential ⇒ `REGISTERED`）

| fixture | 用途（T 用例） | 终态要求 |
|---|---|---|
| **P5-A** | T-1 · T-2 · T-3 · T-4 · T-5 · T-6 · T-7 · T-8 · T-9 · T-9b · T-12 · T-13 · T-14 · T-16 | **不 REVOKE**；测试结束后保持可控状态；**不得**因测试便利而永久破坏 |
| **P5-B** | **仅 T-15** | **独立牺牲 fixture**：`BOUND → REVOKED`（不可逆） |
| **P5-C** | **仅 T-20** | **独立牺牲 fixture**：`REGISTERED + 无 active binding → REVOKED`（不可逆） |

### 11.2 复用已有 fixture（**只读**）

| 对象 | 仅允许 | 要求 |
|---|---|---|
| `3cfb56c8a5fc` | **T-10**（`CLAIM_PENDING` ⇒ 验证准入不足 422 `device_not_registered`） | **只读**；不得 claim / bind / credential rotation / revoke |
| `d163d0000001` | **T-11**（`REVOKED` ⇒ 三动作 422 `device_revoked`） | **只读**；不得改变其状态 |

> `d163d0000002/3` 同为 `REVOKED`，可作 T-11 的**备用**只读对象（若需要多对象）。

### 11.3 生产真机 `288485896ce4`

| 用途 | 要求 |
|---|---|
| **仅允许作为 T-17 只读观测 fixture** | **禁止**为 T-17 主动执行 `bind → unbind`（会写 `device.state`，属生产 D1 写） |

### 11.4 执行顺序约束（避免 fixture 互相干扰）

**先 T-12（1 个 user 绑 P5-A/B/C 三台）→ 解绑 P5-C → T-15（revoke P5-B）→ T-20（revoke P5-C）。**
原因：T-12 需 3 台同 user 设备；而 T-20 要求目标为「无 active binding」，故 P5-C 必须在其被 revoke **之前**处于未绑定态。

### 11.5 ★ T-17 前置（**待人工裁决**）

- `T-17` 判据原文：真机在 **`UNBOUND` 态**持续上行 ⇒ `last_seen` 刷新 / EMQX 会话在线 / 无拒收。
- **只读实测**：`288485896ce4` 当前 `state = REGISTERED`（**从未绑定** ⇒ 无 active binding，功能上等价"无主"，但**字面不是 `UNBOUND`**）+ 在线（`dev_288485896ce4 conn=true ka=120 pv=4`）。
- **判定**：若测试代码要求**字面 `UNBOUND` state** 才能执行 T-17 ⇒ **前置不满足**：

  ```
  T-17 fixture prerequisite not satisfied
  ```

  **不得自行修改生产设备状态**，等人工裁决（三选一：① 接受 `REGISTERED + 0 active binding` 作为等价前置；② 授权对生产设备 bind→unbind【生产写】；③ 延后 T-17）。

---

## 12. N-3 Remediation Design（设计稿 —— **待人工审核，未编码**）

> 属 `DoD-D6` BLOCKER 的修复设计。**本轮不修改任何业务代码。** 详见同目录 `P1-8-Acceptance-Decision.md` §2（完整版）。
> 摘要：最小脱敏点 = `Cloudflare_Assets/guo-feeder-api/src/index.js` 的 ingest 落库前（`raw_payload` / `payload_json` 生成处）；**不影响** `credential.js::publishToDevice()` 的真实 MQTT 下发。详见该文件 §2.A–§2.D。

**本轮结束 —— 等待人工审核 N-3 修复设计；不自行进入代码修复、不自行 commit、不自行开始 LIVE 轮次。**
