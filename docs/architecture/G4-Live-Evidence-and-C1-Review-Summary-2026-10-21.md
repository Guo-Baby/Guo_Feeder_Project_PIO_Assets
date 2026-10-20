# G4 线上只读取证 + C-1 最终审查 · 汇总报告

- 日期：`2026-10-21`（`00:28`–`00:45`，本机 UTC+8）
- 范围：阶段 1（线上版本只读）→ 阶段 2（B-3 EMQX 只读取证）→ 阶段 3（C-1 执行前审查），**严格按序**
- 性质：**全程只读**。未部署 / 未回滚 / 未应用 migration / 未改 EMQX / 未烧录 / 未重启设备 / 未发布任何测试消息
- 分阶段报告（各自独立、已先落盘）：
  1. `docs/architecture/G4-S1-Live-Worker-Version-Evidence-2026-10-21.md`
  2. `docs/architecture/G4-S2-B3-EMQX-Evidence-2026-10-21.md`
  3. `docs/architecture/G4-S3-C1-Final-Review-2026-10-21.md`

---

## A. 执行摘要

| 阶段 | 状态 | 一句话结果 |
|---|---|---|
| **1 线上版本** | ✅ **完成** | 当前 `deployment=c666aeeb-…` / `version=826332ed-…`（v47）；回退目标 `f9647a00-…`（v46）；`rollback` 参数是 **version id** ⇒ `C2-VER` **解除** |
| **2 B-3 取证** | ⚠️ **部分完成** | Broker 侧取全（`recv_msg=35`、`send_msg=16`、**丢弃全 0**）；**规则/Action/Connector 全 403**，无 Dashboard 登录态 ⇒ 根因**仍未定** |
| **3 C-1 审查** | ✅ **完成** | 结论 **`CONDITIONAL`** —— 技术前提全部核实通过，但有 **5 项文档前置条件**待补（+1 项属 C-3） |

**是否发生生产变更：没有。** 全部操作是 `GET`（Cloudflare API / EMQX API）与 `SELECT`（D1）。
可核对的写入证据：D1 每次查询的 `meta` 均为 `changed_db=false`、`rows_written=0`。

**当前不能得出的结论**：B-3 根因；漏记是否影响有限；线上 v47 对应哪个 commit；
生产 D1 的 migration 原子性；C-1 是否可以执行（只能"可提交人工审批"）。

---

## B. 阶段 1：线上 Worker 版本证据

### B.1 结论数据

| 项 | 值 |
|---|---|
| Worker / 环境 | `guo-feeder-api` · **无 `[env.*]`** ⇒ 仅顶层生产环境 |
| 账号 | `9e72d201efec6925b3a468a0e3fb4928`（`whoami` 可见账号**唯一**） |
| **当前 `version_id`** | `826332ed-3a73-42c4-a090-a5bf737dc210`（number **47**） |
| **当前 `deployment_id`** | `c666aeeb-150e-4b10-a155-59a13bdaab0b` |
| 分流 | 单版本 `100%`（`strategy=percentage`） |
| 部署时间 | `2026-10-19T12:56:06.861548Z`（= `10-19 20:56:06 +08`） |
| **回退目标 `version_id`** | `f9647a00-e079-4de9-bcf1-8c9bc5344185`（number **46**） |
| 对应 deployment | `c28f2eba-2f5e-49c0-8205-206b15d9858e` |
| 线上绑定 | `guofeeder_DB → 0d8385ba-…` ✅ 与配置一致 |
| 线上 vars | `LOG_BOOT_WATERMARK="1"` ★ **在线上版本元数据里确认** |
| Secrets（仅名称） | `ADMIN_API_TOKEN` · `EMQX_APP_ID` · `EMQX_APP_SECRET` · `WEBHOOK_SECRET` |

### B.2 查询方式与证据

| # | 命令 | 证据文件 |
|---|---|---|
| 1 | `wrangler deployments list --json` | `s1-deployments-list.json` |
| 2 | `wrangler deployments status --json` | `s1-deployments-status.json` |
| 3 | `wrangler versions list --json` | `s1-versions-list.json` |
| 4 | `wrangler versions view <id>` ×2（当前/上一版） | `s1-version-view-current.txt` · `s1-version-view-prev.txt` |
| 5 | `wrangler rollback --help` | `s1-rollback-help.txt` |
| 6 | `wrangler whoami` + 仓库基线 | `s1-whoami-and-baseline.txt` |

查询时间：`2026-10-21 00:28:24`–`00:29:22 +08`。全部 `--json` 输出原样保留。

### B.3 `rollback` 参数语义（核实结果）

```
wrangler rollback [version-id]
POSITIONALS  version-id  The ID of the Worker Version to rollback to
```

⇒ **位置参数是 version id**，不是 deployment id。回退命令应为
`npx wrangler rollback f9647a00-e079-4de9-bcf1-8c9bc5344185`。**未执行**。

### B.4 本地 vs 线上

| 对照 | 结果 | 性质 |
|---|---|---|
| 本地 HEAD | `e1e8b79`（`10-20T15:23Z`） | — |
| 线上 v47 | 部署于 `10-19T12:56Z` | — |
| HEAD 的 3 个修复（B-1b / B-2b / B-2c）是否在线 | **否**（部署早于提交约 **26.5 h**） | ✅ 可证 |
| 线上 v47 对应 commit | **无法确定**（`Source: Unknown (version_upload)`；部署时刻无对应提交） | UNVERIFIED |
| v47 是否已含 **`/down` 方向判定** | **是** ✅ | v47 任期 `audit/down=193` |
| v47 是否已含 **`$tenants` 设备段解析** | ❌ **否**（`00:55` 更正） | 全库 1 539 行 `$tenants/*/down` 的 `device_id` **全是 `http_api`**，最新一条是刚刚 ⇒ 见 S1 报告 §4.1-B |
| 原猜测"v47 ≈ `362ddfd`" | ❌ **已推翻**（`362ddfd` 早于 B-1 工作） | — |
| v47 是否已含 `B-2`（写 `dedup_key`） | **UNKNOWN**；若含则当前长期处于**缺列降级态**（D1 侧不可观测） | UNKNOWN |

★ **未决项**：`S1-U1`（commit 归属）· `S1-U2`（是否含未提交改动）· `S1-U3`（v46↔v47 行为差异）。

---

## C. 阶段 2：B-3 EMQX 证据

### C.1 采集条件

| 项 | 值 |
|---|---|
| 实例 | EMQX **Serverless** `n302933b.ala.cn-hangzhou.emqxsl.cn:8443` |
| 节点 / 监听器 | `emqxsl-prod@10.76.117.57` / `tcp:default` |
| 鉴权 | API Key（`EMQX_APP_ID/SECRET`） |
| 时间 | API 为 UTC；D1 `created_at` 为 epoch **毫秒** |
| 窗口 | `[connected_at, 查询时刻]` = `2026-10-20T15:22:26Z` → `2026-10-21T16:32Z`（≈70 min） |

### C.2 指标（按层，含单位）

| 层 | 指标 | 值 | 单位 | 来源 |
|---|---|---|---|---|
| Broker 收（设备→） | `recv_msg` | **35** | PUBLISH 条 | `/clients/dev_288485896ce4` |
| Broker 收（报文） | `recv_pkt` / `recv_cnt` | 113 / 116 | 报文 | 同上（**不是消息数**） |
| Broker 发（→设备） | `send_msg` | **16** | PUBLISH 条 | 同上 |
| Broker 丢弃 | `recv_msg.dropped` · `mqueue_dropped` · `send_msg.dropped.*` | **0 / 0 / 0** | 条 | 同上 |
| D1 `mqtt_messages` | `audit`+`up` | **4** | 行 | `s2-d1-window-counts.txt` |
| D1 `mqtt_messages` | `echo`+`down` | **16** | 行 | 同上 |
| D1 `log_history` | 行数 / 唯一 `seq` | **14 / 14**（`seq 6145–6158`） | 行 | 同上 |

### C.3 已证实的事实

1. 设备在线，会话 `clean_start=true`（计数按会话归零）⇒ 窗口边界**无需估时**
2. 下行 **16 ↔ 16** 一一对应（但只有 `echo` 一份）
3. Broker **会话层零丢弃** ⇒ 少消息不是会话队列/在途/超限造成
4. **全库 `echo` 只存在于 `down`**（1537 行，`MAX(created_at)=10-20 23:30`）——`echo/up` **从未出现**
5. `audit/down` 在 `2026-10-20 02:50` 后**归零**（此前 v47 任期有 193 行）
6. `direction='unknown'`（旧 `/log` 落库）终止于 `2026-10-16 01:20`
7. API 可读端点仅 `/clients`、`/subscriptions`；`/rules`、`/actions`、`/connectors`、`/stats` 等 **8 个端点 403（空 body）**

### C.4 待验证假设（不得当结论）

- 差额 **17 条**（35 − 4 − 14）主要来自 `/log` 主题（不走 `mqtt_messages`）—— 数量级合理，**无 per-topic 证据**
- `recv_msg` 含 QoS1 重传（`proto_ver=4` + `clean_start=true`）
- `audit/down` 归零源于规则被改/禁用（需 H-5）

### C.5 `NOT MEASURABLE`

`N-1` 规则 matched/failed（`/rules` 403）· `N-2` Action 重试/队列/丢弃（`/actions` 403）·
`N-3` Connector 成功率/超时（`/connectors` 403）· `N-4` Broker 全局收发（`/stats` 403）·
`N-5` per-topic 计数（无端点）· `N-6` Worker 5xx / `outcome=`（Logs 403，`tail` 未取）·
`N-7` 可比较口径的真实漏记率（口径不可对齐）

### C.6 下一步最小取证需求

需要 Dashboard 登录态，按阶段 2 §6 的 **H-1…H-7** 采集（两轮窗口：`10-20 23:22–23:35` 与
`10-20 02:40–03:00`）。**不要**点 Publish/Resend/Retry；**不要**改任何配置；截图须带时间范围。

---

## D. 阶段 3：C-1 最终审查

### D.1 生产 D1 身份与 migration 状态

| 项 | 结果 |
|---|---|
| 账号 / `database_id` / 绑定 | `9e72d201…` / `0d8385ba-a1a6-4fcc-8e43-ae61c014c43d` / `guofeeder_DB` —— **三者与线上 Worker 绑定一致** |
| `dedup_key` 列 | **0**（不存在） |
| `ux_mm_dedup_key` 索引 | **0**（不存在） |
| 索引 `sql LIKE '%WHERE dedup_key IS NOT NULL%'` | **0** |
| `d1_migrations` 含 `0007` | **0** |
| `d1 migrations list --remote` | 只列 `0007` 待应用 ⇒ `0001`–`0006` 已应用 |
| 表结构 vs fixture | **20 列逐一相同**（同名列序） ⇒ 本地 R-1…R-8 结论可引用 |
| `mqtt_messages` 现有索引 | **无**（不会与 `ux_mm_dedup_key` 冲突） |
| 历史回填风险 | **结构性为零**：`0007` 只含 `ALTER ADD COLUMN`（无 DEFAULT）+ 部分索引，**无任何 DML** |
| 基线可采集 | ✅ 已演练：`n=5743`、`min_id=1`、`max_id=5761`、`pl_len=817566`、`id_len=36129`、`tp_len=154790`、`devs=73` |

### D.2 指纹判据审查

- **成立**：`id <= MAX_ID_BEFORE` 严格排除并发新增（`AUTOINCREMENT` 单调）
- **成立**：全表 `COUNT` 变化不参与判定
- **局限（必须收窄表述）**：不覆盖"等长内容替换"、非文本列、指定 id 的删除+补写；
  生产**历史已有 18 个 id 缺口**（`MAX(id)=5761` vs `COUNT=5743`，非 cron 造成，`retention` 不含此表）
  ⇒ **不得**表述为"证明所有历史数据绝对未变"

### D.3 原子性证据及适用范围

- 本地 E-1/E-2/E-3：正式流程可应用/可登记；**整文件失败零残留、零登记**
- **只覆盖本地引擎**；生产原子性**未验证**（官方语义一致，但无生产失败注入证据）
- **不得**外推为"生产保证原子"

### D.4 执行/验证/停止/恢复方案的缺项

| ID | 缺什么 | 文档位置 | 最小修订 |
|---|---|---|---|
| `R-1` | 执行前**身份复核**（账号 / `database_id` / 线上绑定） | Rev.2 §2 | 加 3 行一致性检查 |
| `R-2` | 执行前**制品一致性**（`0007` 文件哈希） | Rev.2 §3.2 | `sha256sum` 必须 = `90dc0ec0da0335e8f55cab22121843dc83fd1dacbb7b6d0379bbd9d7ff7e2f1e` |
| `R-3` | 指纹**能力边界** + 抽样逐行复核 | Rev.2 §3.3 | 收窄措辞；新增 `V-2b`（固定 id 抽 50 行比对长度/`msg_id`） |
| `R-4` | `dedup_key` **列属性**与索引 `sql` 全文留档 | Rev.2 §3.3 | 断言无 `NOT NULL`/`DEFAULT`；保存索引 `sql` |
| `R-5` | 半成品**唯一恢复路径**、预期差异清单、自动判/人工判清单 | Rev.2 §3.4 / §5.3 | 补 3 段（半成品只允许"新增 `0008` 建索引"） |
| `R-6` | **C-3** 判据：`I-8`/`Q-5` 的 `echo` 断言永远误报 | Rev.2 §5.3 · 验收清单 §2 | 按 `direction='down'` 收窄 |

### D.5 最终状态

## `CONDITIONAL`

技术前提**全部通过**；缺的是 **5 项可明确列出的文档前置条件**（`R-1…R-5`，另 `R-6` 属 C-3）。
补齐后可重评为 `READY FOR HUMAN APPROVAL` —— 那也只是"可提交审批"，**不是执行许可**。

---

## E. 人工审批清单（**三项分开，不得合并**）

### E.1 C-1：生产 D1 migration `0007`

| 项 | 内容 |
|---|---|
| 授权前置 | `R-1…R-5` 已修订并入册；执行前 `migrations list --remote` 只列 `0007`；账号/`database_id`/绑定三方一致；基线已采集归档 |
| 风险 | 中低。只加列+部分唯一索引，**不改行为**（线上 v47 不写该列）；生产原子性未验证 ⇒ 需 10 min 复检 |
| 操作范围 | 仅 `wrangler d1 migrations apply guofeeder --remote`（禁止 `--file`），**一条命令** |
| 成功判据 | `V-1 = 1/1/1/1`；`V-2` 指纹与基线一致且 `keyed=0`；`V-3 = 0` |
| 停止/回退 | 任一不符 ⇒ 停止；回退 `DROP INDEX IF EXISTS ux_mm_dedup_key`（列保留，**不能复活已拦截记录**） |

### E.2 C-2：Worker 部署与行为验证

| 项 | 内容 |
|---|---|
| 授权前置 | C-1 成功；`CURRENT_VERSION_ID=826332ed-…` / `PREV_VERSION_ID=f9647a00-…` 已记录（✅ 阶段 1 完成）；设备在线；观测通道现状已记录 |
| 风险 | **高**。线上 v47 **无法追溯 commit**（`Source: Unknown`）⇒ 新代码与线上差异只有部分可预期。★ `00:55` 更正：v47 有 `/down` **方向**判定，但**没有** `$tenants` 设备段解析（1 539 行全为 `http_api`）⇒ `P-1'` 是**真正的新行为检查**；若 v47 已含 B-2，当前可能长期处于缺列降级态 |
| 操作范围 | 仅 `wrangler deploy`（**不改** `wrangler.toml` 任何 `vars`，含 `LOG_BOOT_WATERMARK`） |
| 成功判据 | P-1' `$tenants` 侧归到真实设备 ID · P-3' 出现 `direction='log'` · P-2 落 `unresolved` · **P-4 落 1 行** · **P-5 落 2 行且键 NULL** |
| 停止/回退 | Rev.2 §4.6 六条；回退 `wrangler rollback f9647a00-…`（**version id**） |

### E.3 C-3：观察窗口

| 项 | 内容 |
|---|---|
| 授权前置 | C-2 成功；`R-6` 已修订（`echo` 判据按方向收窄）；窗口负责人/采样表/停止条件下令人已定；`boot_seq` 基线已采 |
| 风险 | 中。**对账第三段（云端四计数）不可观测**；真实重复率不可测；`mqtt_messages` 自 `10-16` 起只收 `up`/`down` |
| 操作范围 | 只读：Rev.2 §5.2 SQL + 串口 `system.stats` + EMQX 控制台只读 |
| 成功判据 | 4 次采样 `I-1…I-9` 全成立；`A-1…A-5` 有解释；`N-1…N-4` 如实标 `NOT MEASURABLE` |
| 停止/回退 | Rev.2 §5.5；触发即按 C-2 回滚 |

★ 三条不得跨越的线：① 完成 C-1/C-2/C-3 **不等于** `G-4 PASS`、**不解锁** `WP-5`、**不解除** `STOP-R1`；
② B-1/B-2 修复**不代表** B-3 已解决，`mqtt_messages` **不得**作唯一完整性证据；
③ `G4-R4` 阈值仍为未批准候选值，`LOG_BOOT_WATERMARK` **已启用、不得改动**。

---

## F. 证据与仓库状态

### F.1 文件路径

| 类别 | 路径 |
|---|---|
| 阶段 1 报告 | `docs/architecture/G4-S1-Live-Worker-Version-Evidence-2026-10-21.md` |
| 阶段 2 报告 | `docs/architecture/G4-S2-B3-EMQX-Evidence-2026-10-21.md` |
| 阶段 3 报告 | `docs/architecture/G4-S3-C1-Final-Review-2026-10-21.md` |
| 本汇总 | `docs/architecture/G4-Live-Evidence-and-C1-Review-Summary-2026-10-21.md` |
| 阶段 1 原始证据 | `docs/evidence/G4-Live-Version-2026-10-21/` —— `s1-deployments-list.json`、`s1-deployments-status.json`、`s1-versions-list.json`、`s1-version-view-current.txt`、`s1-version-view-prev.txt`、`s1-rollback-help.txt`、`s1-whoami-and-baseline.txt`、`s3-d1-baseline.txt`、`s3-mqtt-messages-schema.txt` |
| 阶段 2 原始证据 | `docs/evidence/G4-B3-EMQX-2026-10-21/` —— `s2-endpoint-probe.txt`、`s2-clients.json`、`s2-device-client-detail.json`、`s2-d1-window-counts.txt`、`s2-window-anchor.txt` |
| 截图 | **无**（无 Dashboard 登录态；已改为人工取证清单） |

### F.2 仓库状态（本任务结束时）

| 仓 | 分支 | HEAD | `origin/main` | 工作区 |
|---|---|---|---|---|
| 固件 `PIO_Assets/Guo_Feeder_Project` | `wb` | `b1ae945` | `b1ae945` | **新增 4 份报告 + 2 个证据目录（未提交）** |
| 云端 `Cloudflare_Assets` | `main` | `e1e8b79` | `e1e8b79` | **干净**（本任务未改云端仓任何文件） |
| EMQX `EMQX_Assets` | `main` | `ec2364f` | — | **干净** |

### F.3 本任务是否产生修改 / 提交 / 推送

- **产生文件**：是。固件仓 `docs/architecture/` 下 4 份报告（工作文件/正式证据文档）+
  `docs/evidence/` 下 2 个证据目录（正式证据归档文件）。
- **提交 / 推送：没有。** 按任务约束"未经授权不得提交、推送"，本轮**未** `git add` / `commit` / `push`。
- 云端仓、EMQX 仓、生产配置、生产数据：**零修改**。
- 设备：**未烧录、未重启、未改配置**。

### F.4 非预期写入披露

**未发现任何非预期写入。** 可核对依据：
- D1 全部查询的 `meta` 均含 `changed_db=false`、`rows_written=0`
- EMQX 全部请求均为 `GET`
- Cloudflare 全部命令均为 `list` / `status` / `view` / `whoami`（只读）
- 未调用任何 publish / deploy / rollback / migrations apply

若后续发现有遗漏，应以 D1 `rows_written` 与 EMQX 审计日志为准复核。

---

## 附：本轮新增的 4 条**必须继承**的口径修正

1. **`recv_cnt` ≠ 消息数**：EMQX 的 `recv_cnt`/`recv_pkt` 是**报文数**，`recv_msg` 才是 PUBLISH 条数。
2. **`echo` 只存在于 `down`**：`audit + echo` 并存的假设对 `up`/`log` **不成立** ⇒ `I-8`/`Q-5` 必须按方向收窄。
3. **`mqtt_messages` 自 `2026-10-16` 起只收 `up`/`down`**：`/log` 走 `log_history`
   ⇒ **禁止**用"设备发出 vs `mqtt_messages` 行数"算漏记率。
4. **线上版本无法追溯到 commit**：`git HEAD` 不能替代线上版本 ID；
   且 v47 **已含 `/down` 方向判定但不含 `$tenants` 归属修复**（`00:55` 更正）⇒ C-2 的 `P-1'` 是**新行为检查**，不是回归检查。
