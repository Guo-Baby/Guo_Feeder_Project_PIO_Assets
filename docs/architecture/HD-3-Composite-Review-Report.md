# HD-3 统一复核报告 —— 验收问题复核 + MQTT 下行故障诊断

> **生成时间**：`2026-10-19 05:20`（北京） · **性质**：核验 / 测试 / 诊断，**不含修复**
> **执行范围**：三个仓库状态 · migration 0006 · Worker 部署 · 真实设备操作记录 · Batch 1/2/3 · MQTT 下行链路
> **纪律**：未执行生产 D1 写入（仅 `SELECT`/`PRAGMA` 只读）· 未执行生产 rebase · 未 apply migration ·
> 未 deploy/rollback Worker · 未烧录/复位/断电/擦除设备 · 未修改 EMQX 配置 · 未 commit/push。
> **凭据**：本报告不涉及任何密钥、密码、Token 的明文；只记录"是否存在 / 是否鉴权通过"。

---

## A. 执行摘要

### A.1 三个批次的独立验收结论

| 批次 | 结论 | 判定依据（本轮实测） |
|---|---|---|
| **Batch 1 — R-11 水位换代** | **CONDITIONAL PASS** | 代码核对 9 项全过；单测 **85/0**；真实 D1 **55 PASS / 1 FAIL** —— 唯一 FAIL 是**过期断言 R-0d**（断言 0006 应"只在 drafts/"，而 0006 已转正并应用），**不是产品缺陷**，但使该脚本每次必红、掩盖真实回归 |
| **Batch 2 — C-4 冲突重试** | **PASS**（含缺口） | 真实 D1 回归本轮补齐：**55 PASS / 0 FAIL**；重试单测 **38/0**；★ **缺口**：真实 D1 全流程中 `conflict_retried=0`，**重试路径未获真实 D1 覆盖**（仅逻辑替身单测覆盖） |
| **Batch 3 — A1-03 参数契约** | **PASS** | 本轮**独立 CBOR 解码**复现 `seq=4613 / b=19 / pcount=5 / 0x5A=4864 / 0x5B=0(NVS) / 0x5C=0 / 0x5D=0`，与 10-19 04:16 串口原始日志**逐项一致** |

> **不得因其他测试通过而抵消门禁**：`BT-1`、`G-4`、`WP-5`、`STOP-R1` 四项**无任何关闭证据**，一律保持原状（见 §F）。

### A.2 三处报告矛盾的最终核实

| # | 矛盾描述 | 最终状态 | 决定性证据 |
|---|---|---|---|
| 1 | migration 0006「已应用 / 仍在 drafts / 已应用且有 11 条命令」 | ✅ **已应用（APPLIED）** | `d1_migrations` id=6 `0006_log_boot_watermark_epoch.sql` · `applied_at=2026-10-18 20:18:26 UTC`；生产 D1 实存 7 个新列 + `log_boot_watermark_audit` 15 列 + `ix_lbwa_device_created`；`migrations list --remote` = "No migrations to apply!" |
| 2 | Worker 部署 ID `f9647a00` / 回退目标 `508461f3` 在 `deployments list` 中查不到 | ✅ **两 ID 均真实存在** | 它们是 **script version id**（`versions list` 最新两条 = index 8/9），不是 deployment id。对应 deployment：`9f4fee53`(10-18 17:36:32Z)/`c28f2eba`(10-18 20:18:44Z)，与 0004/0005、0006 的应用时间分别相差 33 s / 18 s。**报告记录属实，仅是术语把 version id 记作"部署 ID"** |
| 3 | 「设备已烧录并完成端到端验证」vs「未操作设备、真机验证未运行」 | ✅ **操作真实发生过** | `.pio/hd3_b3_boot.log`（10-19 04:16）原始串口：`recovery=NVS boot_hw=18 -> boot_seq=19 reserved=4864 corrupt=1`；`.pio/hd3_powercycle_watch.log`（04:14，14 次 `power_on`）；`.pio/b19p.json`（04:17 生产 D1 原始取回） |

> 三处矛盾的**共同成因**：文档/报告更新滞后于执行，以及**术语不统一（version id vs deployment id）**，而非执行本身缺失。

### A.3 MQTT 下行故障定位结论

**结论分类：`INSUFFICIENT_EVIDENCE`**

- **已排除**：Wi-Fi 关联、TLS/认证、MQTT 连接、主题订阅、云端 log_ack 发布、设备接收 —— **均有实测证据支持 PASS**。
- **未能支持"设备无法接收下行"这一命题**：本轮所有证据表明下行通道在 `20:30 UTC`（北京 04:30）之前**完全正常且持续工作**。
- **已发现的真实缺陷（与当前命题无关，独立记录）**：`index.js` 中 `cloud_api` 来源的命令经 `publishToEMQX()` 发布，该函数**硬编码 topic `guo_feeder/down`（无 `device_id`）**，与设备订阅的 `guo_feeder/<device_id>/down` **不匹配** ⇒ 经 Pages 下发的命令**结构性到不了设备**。该路径近 16 天未被触发（`guo_feeder/down` 最近活动 10-03），属**潜伏故障**。
- **卡住的观测点**：**设备侧串口未读取**，故无法取得 Wi-Fi 状态、MQTT 回调触发、命令分发结果的直接证据。

### A.4 尚未解决的门禁与阻塞项

`BT-1 OPEN` · `G-4 BLOCKED` · `WP-5 未解锁` · `STOP-R1 未解除` —— **四项全部保持，本轮无任一关闭证据**。
新增阻塞：**生产 rebase 仍未执行**（`max_boot_seq=117`、`epoch=1`、`last_rebase_id=NULL`），上一轮 401 原因（本地 `.dev.vars` 的 `ADMIN_API_TOKEN` 对生产无效）未解决。

---

## B. 证据矩阵

> 「本轮实测」= 本次会话**亲自执行**并取得输出；「历史证据」= 本轮读到的既有原始文件（非推断）。

| # | 检查项 | 预期状态 | 实际观察 | 原始证据来源 | 本轮实测 | 结论（置信度） |
|---|---|---|---|---|---|---|
| B-1 | 固件仓 HEAD / 分支 | `wb` / `7f6b1fe` | `wb` / `7f6b1fe`；5 个 `M` + 2 个 `??` | `git status --porcelain` | ✅ 是 | 一致（**高**）；未提交文件 = 历史**故意保留** + WP6 范围外 |
| B-2 | 云端仓 HEAD / 工作区 | `main` / `de49325` | 完全一致，**工作区干净** | `git status` | ✅ 是 | 一致（**高**） |
| B-3 | EMQX 仓 | `main` / `ec2364f` | 完全一致，干净 | `git status` | ✅ 是 | 一致（**高**） |
| B-4 | 0006 是否在 `d1_migrations` | 已记录 | **id=6 存在**，`applied_at=2026-10-18 20:18:26 UTC` | `d1 execute --remote` | ✅ 是 | **APPLIED**（**高**） |
| B-5 | `log_boot_watermark` 7 个新列 | 全部存在 | `epoch/epoch_started_at/prev_epoch_max/observed_max_boot_seq/ambiguous_count/last_rebase_id/last_rebase_at` 全在 | `PRAGMA table_info` | ✅ 是 | PASS（**高**） |
| B-6 | 审计表与索引 | 存在 | `log_boot_watermark_audit` 15 列 + `ix_lbwa_device_created` | `PRAGMA table_info` / `sqlite_master` | ✅ 是 | PASS（**高**） |
| B-7 | 0006 文件头注释 | 与状态一致 | **仍写「草案 / NOT APPLIED / 位置 migrations/drafts/」**，但文件已在 `migrations/` 顶层且已应用 | 文件 §1-7 行 | ✅ 是 | **矛盾（文档缺陷）**（**高**） |
| B-8 | 生产 rebase 是否执行 | 未执行 | `epoch=1` · `max_boot_seq=117` · `observed=117` · `prev_epoch_max=NULL` · `rewind_count=365` · `last_rebase_id=NULL` | 生产 D1 `SELECT` | ✅ 是 | **NOT RUN 保持**（**高**） |
| B-9 | Worker 版本序列 | `f9647a00` 最新 | `versions list` index9=`f9647a00`、index8=`508461f3`、index7=`72023596`；**`31762ee8` 不存在** | `wrangler versions list --json` | ✅ 是 | 记录属实、术语待正名（**高**） |
| B-10 | 设备在线 | 在线 | `dev_288485896ce4` **connected=True**，`ip=110.180.51.142:33984`，`proto=MQTT/4`，`username=GuoFeederDevice`，`connected_at=2026-10-18T20:21:42Z` | EMQX `GET /clients`、`GET /clients/{id}` | ✅ 是 | PASS（**高**） |
| B-11 | 设备订阅主题 | `guo_feeder/<id>/down` | **`guo_feeder/288485896ce4/down`，QoS=1，`subscriptions_cnt=1`** | EMQX `GET /clients/{id}/subscriptions` | ✅ 是 | PASS（**高**） |
| B-12 | 下行消息是否到达设备 | 应在增长 | `recv_cnt 117→118`、`send_cnt 114→115`（26 s 间隔各 +1）；`mqueue_len=0`、`mqueue_dropped=0`、`inflight=0` | EMQX 客户端计数两次采样 | ✅ 是 | PASS（**高**） |
| B-13 | 最近下行消息性质 | 含命令 | 最近 12 条**全部是 `log_ack`**；`system`/`query` 最近 = `2026-10-18 04:26 UTC`（北京 12:26）；`guo_feeder/down`(legacy) 最近 = **10-03** | 生产 D1 `mqtt_messages` 分组 | ✅ 是 | **近 16 h 无命令下发**（**高**） |
| B-14 | 云端下发代码路径 | topic 含 device_id | `index.js:701-712` `source==="cloud_api"` ⇒ `publishToEMQX()` ⇒ `index.js:199` **topic 硬编码 `"guo_feeder/down"`** | 源码 | ✅ 是 | **FAIL（潜伏）**（**高**） |
| B-15 | log_ack 下发路径 | topic 含 device_id | `log-ack.js:157` `guo_feeder/${deviceId}/down`，经 `publishToDevice()`；生产日志 `status=200 code=ok` | 源码 + 生产 tail 日志 | ✅ 是 | PASS（**高**） |
| B-16 | A1-03 参数端到端 | 4 个新 PID 正确 | 本轮独立解码：`b=19 seq=4613 pcount=5`；`0x5A=4864`、`0x5B=0(NVS)`、`0x5C=0`、`0x5D=0` | 生产 D1 取回 + `.pio/decode_params.py` 重跑 | ✅ 是 | PASS（**高**） |
| B-17 | 串口 vs 上报参数 | 逐项一致 | 串口 `recovery=NVS boot_hw=18→19 reserved=4864 src=0 failclosed=0`；`trace corrupt=0` vs meta `corrupt=1` | `.pio/hd3_b3_boot.log` 行 37/38/260/262 | ⚠️ 历史证据 | PASS（**高**）；两者**异源**已文档化 |
| B-18 | 生产实时流量现状 | 有流量 | 45 s + 55 s 两次 tail 窗口**均为空**；`mqtt_messages` 停在 `20:30:52 UTC` | `wrangler tail`（两次） | ✅ 是 | **当前无上行入库流量**（**中**） |

---

## C. 测试记录

| 测试 | 执行环境 | 与生产隔离 | 退出码 | PASS / FAIL | 备注 |
|---|---|---|---|---|---|
| `test/real_d1/log_conflict_real_d1.sh` | 本地真实 D1（miniflare） | ✅ 独立 `persist` 目录 `.wrangler/hd3_log_conflict_<ts>/state`，脚本自证"本脚本未调用远端 D1 = 0"、测试设备 `0c0f11c0ffee`（合成） | **0** | **55 / 0** | 本轮补齐。覆盖 L-1..L-8 |
| `test/real_d1/watermark_rebase_real_d1.sh` | 本地真实 D1 | ✅ 独立 persist，合成设备 | **1** | **55 / 1** | 唯一 FAIL = **R-0d**（过期断言）。**重跑复现**（EXIT=1） |
| `test/current/log_boot_watermark_rebase.test.mjs` | 本地单元（逻辑替身） | ✅ 不触网 | 0 | **85 / 0** | 与报告"85 项"吻合 |
| `test/current/log_conflict_retry.test.mjs` | 本地单元（逻辑替身） | ✅ | 0 | **38 / 0** | 覆盖 F1-F3 表缺失熔断、E 系列聚合 |
| `test/current/log_boot_watermark.test.mjs` | 本地单元 | ✅ | 0 | **48 / 0** | 与报告"原有 48 项"吻合 |
| `test/current/log_conflict.test.mjs` | 本地单元 | ✅ | 0 | **54 / 0** | — |
| `test/current/log_ack_collision.test.mjs` | 本地单元 | ✅ | 0 | **61 / 0** | — |

**未执行项及原因**：
- **生产 rebase（dry-run / 实跑）**：本轮未授权执行；上一轮 401 的根因（本地令牌 ≠ 生产 secret）未解决 ⇒ 保持 `NOT RUN`。
- **全部云端/固件测试套件（`run_all.mjs`、`b3_collision_real_d1.sh`、`retention_real_d1.sh` 等）**：不在本轮指定范围，未执行 ⇒ 状态 **NOT RUN**。
- **固件编译/烧录/断电**：约束禁止 ⇒ **NOT RUN**。

**R-0d 失败详情**（原文）：

```
FAIL  R-0d 0006 只在 drafts/（顶层无 0006）  期望=[absent] 实际=[present]
```

该断言反映的是 0006 **尚未转正**时期的设计意图；0006 于 `2026-10-18 20:18:26 UTC` 转正并应用后，断言未同步更新。

---

## D. MQTT 端到端链路（设备 `288485896ce4`）

| 层 | 状态 | 证据 |
|---|---|---|
| **Wi-Fi 关联 / 取得 IP** | **PASS** | 设备公网 IP `110.180.51.142`、源端口 33984 的 TCP 会话自 `20:21:42Z` 起持续存在（EMQX 侧 `connected` 恒为 true） |
| **网关 / 路由 / DNS** | **NOT VERIFIED** | 无设备侧日志；由"TCP 会话建立并保持"间接推断可达，但**未取得 DNS 解析、网关的直接证据**（不得据此断言 PASS） |
| **TLS / 认证** | **PASS** | EMQX 侧会话建立成功，`proto_name=MQTT`、`proto_ver=4`、`username=GuoFeederDevice`、`keepalive=120`、`expiry=0`；EMQX 侧无该客户端的认证失败记录 |
| **MQTT 连接** | **PASS** | `connected=True` 持续；`connected_at` 自 `20:21:42Z` **未变化**（无掉线重连）；4 个客户端中仅本设备在线 |
| **主题订阅** | **PASS** | `GET /clients/dev_288485896ce4/subscriptions` ⇒ `guo_feeder/288485896ce4/down`，`qos=1`，`nl=0,rap=0,rh=0`，`subscriptions_cnt=1`，与项目规范 `guo_feeder/<device_id>/{down,up,log}` 一致 |
| **云端发布（log_ack 通道）** | **PASS** | `log-ack.js` 走 `publishToDevice()` → topic `guo_feeder/<id>/down`；生产日志 `[log-ack] published ... status=200 code=ok`；`mqtt_messages` 697 条记录持续至 `20:30 UTC` |
| **云端发布（cloud_api 命令通道）** | **FAIL**（潜伏，当前未触发） | `index.js:701-712` ⇒ `publishToEMQX()` ⇒ `index.js:199` 硬编码 `topic: "guo_feeder/down"`。**发布主题 ≠ 订阅主题** ⇒ 结构性不可达 |
| **消息实际到达设备** | **PASS**（至 `20:30 UTC`） | EMQX 计数 `send_cnt 114→115` 增长；`mqueue_len=0`、`mqueue_dropped=0`；设备 `log_history` 由 `b=8` 推进至 `b=21/seq=5132`（需收到 ACK 才会推进） |
| **当前是否有到达** | **NOT VERIFIED** | `20:30:50 UTC` 后 `mqtt_messages` 与 `log_history` 双双停更；两次 tail 窗口为空 |
| **命令解析 / CommandManager / WorkflowManager 分发** | **NOT VERIFIED** | 近 16 h 无任何命令下发 ⇒ 无样本可验证。需人工提供"最近一次尝试下发"的时间/命令/入口 |

**四态区分（对应 §3.4 要求）**：
- 客户端已连接：**是**（EMQX 实证）
- 订阅请求已发送：**是**（EMQX 已登记该订阅）
- Broker 已确认订阅：**是**（订阅出现在 EMQX 订阅表，非固件单条日志）
- 消息实际到达设备：**是（至 20:30 UTC）**，之后 **NOT VERIFIED**

---

## E. 问题清单与下一步

### BLOCKER

| # | 问题 | 影响 | 下一步 | 需授权 | 涉生产/真机 |
|---|---|---|---|---|---|
| **P-1** | 生产 rebase 从未执行（`max_boot_seq=117` vs 设备真实 `boot=21`），`rewind_count=365` 且持续增长，回退信号退化为噪声 | R-11 机制虽已上线但未产生实际收益；`[log-bootwm]` 每次上报都输出 `REWIND_CANDIDATE`，2026-10-19 04:22 一分钟内连报 7 次 | 用**生产** `ADMIN_API_TOKEN` 先跑 `dry_run=true` 复核，再决定是否实跑 | ✅ 需 | 生产 |
| **P-2** | `index.js` cloud_api 下发路径硬编码 `guo_feeder/down` | 任何经 Pages/Web 下发的命令**结构性到不了设备**；当前因近 16 天未触发而未暴露 | 改为 `guo_feeder/${device_id}/down` 并复用 `publishToDevice()`；先静态审查调用方 | ✅ 需 | 生产代码 |

> **★ 状态更新（`2026-10-19 21:05`）：`P-2` = ✅ `CLOSED`**（本表为**当时快照**，此处仅追加状态，不改原始结论）。
> 修复已部署（version `826332ed` / deployment `c666aeeb`），真机 E2E 两轮全链闭合。
> 权威记录 `HD-3-P2-Production-Deployment-and-E2E-Acceptance-Report.md` · 登记册 `docs/issues/未修复的问题.md` · 原始证据 `docs/evidence/HD-3-P2-2026-10-19/`。
> 残留 `R-2-1`（调用方须携带 `device_id`）**仍未关闭**。

### MAJOR

| # | 问题 | 影响 | 下一步 | 需授权 | 涉生产/真机 |
|---|---|---|---|---|---|
| **P-3** | `watermark_rebase_real_d1.sh` 的 `R-0d` 断言过期（0006 已转正） | 该脚本**每次必红**（55/1），掩盖真实回归，削弱其作为回归门禁的价值 | 把 R-0d 改为"0006 在顶层且已入库" | ❌ 否（仅改测试） | 否 |
| **P-4** | migration `0006` 文件头注释仍写「草案 / NOT APPLIED / 位于 `migrations/drafts/`」，与实际（已转正、已应用）矛盾 | 后来者可能误判 0006 未应用而**重复 apply**（虽 D1 幂等，但会制造运维误判）；也可能误以为需手工搬移 | 在**新** migration 或文档说明中更正；**不改已应用文件本身** | ❌ 否 | 文档 |
| **P-5** | C-4 重试路径无真实 D1 覆盖（真实 D1 全流程 `conflict_retried=0`） | 重试的**真实行为**（真 D1 错误码分类、批内重试上限、证据丢失率）未被端到端验证，仅逻辑替身单测支撑 | 在 `log_conflict_real_d1.sh` 中补"注入瞬时错误 → 重试 1 次 → 仍失败 → evidence-lost"用例 | ❌ 否 | 否 |
| **P-6** | `log_conflict_real_d1.sh:159` 存在残留调用（`migrations/: Is a directory`、`apply: command not found`） | 脚本输出含错误信息，易被误读为失败；源于 0004 转正后 drafts 子目录消失 | 删除该残留分支 | ❌ 否 | 否 |

### MINOR

| # | 问题 | 影响 | 下一步 |
|---|---|---|---|
| **P-7** | 设备自 `20:30:50 UTC` 起 40+ 分钟无新日志入库，而 EMQX 计数仍在增长 | 二者不一致，可能是设备空闲、上行主题/载荷不再命中 EMQX 规则、或规则被限流，**尚未定性** | 读取串口确认设备状态；或对比 EMQX 规则运行统计 |
| **P-8** | 报告把 **script version id** 记作"部署 ID" | 回退时按 `deployments list` 检索会找不到目标，产生运维歧义 | 报告中区分 `version id` / `deployment id`，并记录二者映射 |
| **P-9** | `0x5D LOG_P_TRACE_CORRUPT` 与串口 `corrupt=` 异源（trace 文件 vs meta 累计），实测 `trace=0` 而 `meta=1` | 名字相似易被混读，可能把"有损坏历史"误读为"无损坏" | 命名改为 `LOG_P_TRACE_CORRUPT_COUNT` 或强化契约说明 |
| **P-10** | `HD-3-Rebase-Runbook.md` 在**固件仓**，而引用它的 `0006` 在**云端仓** | 云端仓检索 Runbook 会落空 | 云端仓加指向链接 |

### QUESTION

| # | 问题 | 需要的输入 |
|---|---|---|
| **Q-1** | "设备无法接收 MQTT 下行"具体指哪一类消息？（log_ack / 具体命令 / 某次具体操作） | 用户给出**最近一次下发尝试**的时间、入口（Pages or API）、命令名与期望现象 |
| **Q-2** | 设备上行流量在 `20:30 UTC` 后是否真的停止？ | 串口日志，或一次受控的只读诊断下发 |
| **Q-3** | 生产 `ADMIN_API_TOKEN` 是否可用？上一轮 401 的根因是否确为令牌不一致？ | 人工提供正确的生产令牌指纹，或授权在控制台核对 secret |

### NIT

| # | 问题 |
|---|---|
| N-1 | `.wrangler/hd3_*` 存在大量历史 scratch 目录（每次测试一份），建议定期清理（均在 gitignore 内，无入库风险） |
| N-2 | 云端仓 `wrangler.toml` 中 `[vars] LOG_BOOT_WATERMARK = "1"` 位于文件末尾 `[triggers]` 之后，可读性差 |

---

## F. 最终门禁

| 门禁 / 判定项 | 结论 | 依据 |
|---|---|---|
| **接受 Batch 1（R-11）** | **否（CONDITIONAL）** | 实现与单测合格，但真实 D1 **1 FAIL**（P-3）必须先修；且生产 rebase 未执行（P-1），机制未产生实际收益 |
| **接受 Batch 2（C-4）** | **是（附缺口）** | 真实 D1 回归本轮补齐为 55/0；重试路径缺真实 D1 覆盖（P-5），列为**遗留项**而非阻断项 |
| **接受 Batch 3（A1-03）** | **是** | 本轮独立解码 + 串口原始证据 + 生产 D1 三方交叉一致 |
| **BT-1** | **保持 OPEN** | 无任何关闭证据 |
| **G-4** | **保持 BLOCKED** | 依赖 BT-1，BT-1 未关闭 |
| **WP-5** | **保持未解锁** | 依赖 G-4 |
| **STOP-R1** | **保持未解除** | 无书面解除文件 |
| **是否建议开始后续批次** | **否** | 建议先处理 P-3/P-4/P-6（纯本地/文档，零风险），再就 P-1/P-2 单独申请授权 |

### 术语纪律（本报告已遵守）

- 未将"本地代码测试通过"写成"生产环境验证通过"：Batch 1 的真实 D1 测试在**本地** miniflare 执行，与生产隔离，报告中一律标注环境。
- 未将"设备在线"写成"MQTT 下行正常"：二者在本报告中是**两个独立条目**（B-10 vs B-12/B-13）。
- 未将"Broker 发布成功"写成"设备已执行命令"：log_ack 的 `status=200` 仅证明 **EMQX 接受发布**，到达设备的证据另取 EMQX 计数与设备推进（B-12/B-16）。

---

## 附：本轮执行的关键命令（可复核）

```bash
# 0006 状态
wrangler d1 execute guofeeder --remote --command "SELECT id,name,applied_at FROM d1_migrations ORDER BY id;"
wrangler d1 execute guofeeder --remote --command "PRAGMA table_info(log_boot_watermark);"
wrangler d1 execute guofeeder --remote --command "SELECT sql FROM sqlite_master WHERE name='log_conflict';"
wrangler d1 migrations list guofeeder --remote          # → No migrations to apply!

# Worker 版本
wrangler versions list --json                            # f9647a00 / 508461f3 / 72023596 存在；31762ee8 不存在
wrangler deployments list --json                         # c28f2eba / 9f4fee53

# EMQX（只读 GET）
GET /clients
GET /clients/dev_288485896ce4
GET /clients/dev_288485896ce4/subscriptions

# 生产 D1（只读）
SELECT device_id,epoch,max_boot_seq,observed_max_boot_seq,prev_epoch_max,rewind_count,ambiguous_count,last_rebase_id,last_rebase_at,updated_at FROM log_boot_watermark;

# 测试
bash test/real_d1/log_conflict_real_d1.sh        # 55/0  EXIT=0
bash test/real_d1/watermark_rebase_real_d1.sh    # 55/1  EXIT=1（R-0d）
node test/current/log_boot_watermark_rebase.test.mjs   # 85/0
node test/current/log_conflict_retry.test.mjs          # 38/0
node test/current/log_boot_watermark.test.mjs          # 48/0
```
