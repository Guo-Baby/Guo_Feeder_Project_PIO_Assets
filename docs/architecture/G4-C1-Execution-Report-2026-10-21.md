# G4 · C-1 正式执行报告（生产 D1 `0007` migration）

- 报告日期：`2026-10-21`
- 执行窗口：`01:21:30` – `01:25:00 +0800`
- 执行依据：`G4-Production-Change-Plan-Rev2-2026-10-20.md`（**Rev.2.1**，唯一执行手册）
- 证据目录：`docs/evidence/G4-C1-Exec-2026-10-21/`（含 2 个可复现脚本）
- 目标：D1 `guofeeder` = `0d8385ba-a1a6-4fcc-8e43-ae61c014c43d` · 账号 `9e72d201efec6925b3a468a0e3fb4928`
- 制品：`migrations/0007_mqtt_messages_dedup.sql` · `sha256=90dc0ec0da0335e8f55cab22121843dc83fd1dacbb7b6d0379bbd9d7ff7e2f1e`

## 执行人 / 授权

| 项 | 值 |
|---|---|
| 授权 | 用户本轮**书面授权**（"最终审查通过后可先提交、推送文档，再执行 C-1，无需再次询问"） |
| 执行人 | 本会话 AI 助手（受权代为执行，命令逐条留痕于证据目录） |
| 执行人确认时点 | `2026-10-21 01:22:45`（门禁采集开始前，已明确记录） |
| 验证责任窗口 | 执行结束 `01:23:20` 起 **10 分钟**内 ⇒ 实际完成 `01:23:33`–`01:23:54`（**21 秒**）+ 补充项至 `01:24:57` |

---

## 1. 最终审查结论

**结论：审查通过，无 P0 阻塞，准予提交/推送/执行。**

| # | 审查项 | 结论 | 证据 |
|---|---|---|---|
| 1.1 | Rev.2.1 `§2.A`/`§2.C`/`§3.2`/`§3.3`/`§3.4`/`§3.5`/`§3.7` 与停止条件相互一致 | ✅ 一致 | 手册实际行 `73–150` / `182–487` / `862–876`；停止条件 8 条只判 `down` 的 `echo`（`§5.5` #4） |
| 1.2 | 验收清单 R-6 已同步，`§7.9` 与 `recv_msg` 口径正确 | ✅ | 清单 `§7.9`（`7-9`…`7-13`）· `7-3` 已写 `recv_msg` 并注明 `recv_cnt=116 vs recv_msg=35` |
| 1.3 | 阶段 1 / 阶段 2 报告与原始证据引用链闭合 | ✅ | S3 报告 `CONDITIONAL` → 阶段 1 报告 §D 逐项关闭 → 阶段 2 报告 `READY FOR HUMAN APPROVAL`；证据目录 4 个均实存并被正文点名 |
| 1.4 | S3/S4/S5/S6 均 `STOP + HUMAN_REQUIRED` | ✅ | 手册 `§3.7.2` 表 + `§3.7.5` 流程图 |
| 1.5 | `apply_exit=0` 不视为成功 | ✅ | 手册 `§3.2` ★★（`S5` 伪成功实测） |
| 1.6 | 回滚后 `S5′` 不视为 `S0` | ✅ | 手册 `§3.5` 表末行 + `§3.7.2` |
| 1.7 | v47 `$tenants` 设备段结论已更正，不沿用旧推断 | ✅ | S1 报告 `§4.1-A` 加"部分推翻"横幅 + `§4.1-B`（全库 1 539 行 `$tenants/*/down` 的 `device_id` 全为 `http_api`） |
| 1.8 | R-6 **未**被表述为 B-3 已解决 | ✅ | 手册 `§0.1` 反方向声明 · `§7.9` ★ · 阶段 1 报告 `§D.3` |
| 1.9 | `SEQ-SEM` 契约口径未被擅自修改 | ✅ | 阶段 1 报告 §E 明记"未动"；`docs/issues/未修复的问题.md` 仍为待人工确认 |

**审查中修正的一处文档瑕疵（已随本次提交）**：`HANDOFF.md` `§1.0` 标题写"必读（2 个文件）"但列了 3 条 ⇒ 已改为"3 个文件"。属笔误，不影响任何判据。

**非阻塞观察项（O-1）**：远端仓库为 **PUBLIC**；证据含账号邮箱与 Account ID。核查确认该邮箱**已在 8 个历史已提交文件中公开**（`git grep` 于 `b1ae945`），本次新增证据沿用同一口径，**未新增暴露面**；证据中**无** token / 密钥 / 连接串 / `Authorization` 头。未对原始证据做脱敏改写（保持采集原样）。

---

## 2. 提交与推送

### 2.1 实际提交文件（36 个，全部为 G4 文档与可复现证据）

| commit | 范围 | 文件数 | 说明 |
|---|---|---|---|
| `0e46cc3` | `docs(g4)` | **11** | `HANDOFF.md` · `docs/README.md` · Rev.2.1 手册 · 验收清单 · `docs/issues/未修复的问题.md` + **6 份阶段报告**（阶段 1 / 阶段 2 / S1 / S2 / S3 / 汇总） |
| `542d5e0` | `evidence(g4)` | **25** | 4 个证据目录：`G4-Live-Version-2026-10-21/`(9) · `G4-B3-EMQX-2026-10-21/`(5) · `G4-R1R5-Rev3-2026-10-21/`(3) · `G4-C1-PreExec-2026-10-21/`(8) |

### 2.2 排除文件（逐项确认）

- **无任何固件源代码**：`src/`、`include/`、`platformio.ini`、`test/` 零改动（`git status` 提交后空）。
- 无云端仓改动：`Cloudflare_Assets` HEAD 仍 `e1e8b79` == `origin/main`，工作区干净。
- 无 `data/`、`.pio/`、`.workbuddy/`（已被 gitignore）。
- 未提交临时文件：本次使用的两个 commit message 临时文件在 `%TEMP%`，不在仓库内。

### 2.3 推送

```
to https://github.com/Guo-Baby/Guo_Feeder_Project_PIO_Assets.git
   b1ae945..542d5e0  wb -> main
PUSH_EXIT=0
```

普通 push（**无** `--force`）。推送后 `fetch` 复核：

| 项 | 值 |
|---|---|
| 本地 `HEAD` | `542d5e0f74de35bd1c38def4b183556c76f1b5de` |
| `origin/main` | `542d5e0f74de35bd1c38def4b183556c76f1b5de` |
| 远端 `refs/heads/main`（`ls-remote`） | `542d5e0f74de35bd1c38def4b183556c76f1b5de` |

⇒ **三方一致**。`origin/wb` 仍滞后（`ahead 124`），符合仓库约定（`wb` 仅作本地工作分支，进度以 `main` 为准）。

---

## 3. 执行前硬门禁（`01:22:45`–`01:23:09`，紧邻执行 6 秒）

证据：`g0-gate.txt`（脚本 `c1_gate.sh`）

| # | 条件 | 实测 | 判定 |
|---|---|---|---|
| 1 | Cloudflare 账号身份 | `9e72d201efec6925b3a468a0e3fb4928` | ✅ |
| 2 | `wrangler.toml` / `d1 list` / 线上 version 元数据 D1 一致 | 三者均 `0d8385ba-a1a6-4fcc-8e43-ae61c014c43d` | ✅ |
| 3 | 目标库唯一、无同名歧义 | `d1 list` 仅一条 `guofeeder` | ✅ |
| 4 | 线上 Worker 版本与绑定符合基线 | `826332ed-…`（`v47`，100%，`2026-10-19T12:56:06Z`）；`env.guofeeder_DB` 绑定同上；`LOG_BOOT_WATERMARK="1"` **未改** | ✅ |
| 5 | `0007` 制品三方哈希 | `sha256=90dc0ec0…2f1e` · `worktree_blob=eb300070…` · `HEAD_blob=eb300070…` | ✅ |
| 6 | 生产状态 = `S0` | `col=0 idx=0 partial_idx=0 col_ok=0 reg7=0`，`nn/dflt/ctype=null` | ✅ |
| 7 | 执行前基线**现采**（未沿用旧报告） | `01:21:30` 采集，见 §4 | ✅ |
| 8 | 验证工具/证据目录就绪 | 两个脚本已落盘，证据目录已建 | ✅ |

补充：`d1 migrations list --remote` 仅列 `0007` ⇒ `0001`–`0006` 已登记；`wrangler.toml` **无** `[env.*]`。

---

## 4. 执行前后数据基线

### 4.1 指纹（`FINGERPRINT_BEFORE`，固定 `MAX_ID_BEFORE = 5763`）

证据：`g1-pre-fingerprint.txt`（`01:21:30`）· `v2-fingerprint-after.txt`（`01:23:42`）

| 项 | 执行前 | 执行后 | 相等 |
|---|---|---|---|
| `n_old` | 5745 | 5745 | ✅ |
| `min_id` | 1 | 1 | ✅ |
| `max_id` | 5763 | 5763 | ✅ |
| `pl_len` | 817694 | 817694 | ✅ |
| `id_len` | 36151 | 36151 | ✅ |
| `tp_len` | 154882 | 154882 | ✅ |
| `keyed`（`dedup_key IS NOT NULL`） | —（列不存在） | **0** | ✅ 无回填 |
| `devs` / `no_msg_id` | 73 / 1698 | —（未再采） | 记录项 |

### 4.2 结构

| 项 | 执行前 | 执行后 |
|---|---|---|
| 表结构原文 | 20 列（`… msg_type INTEGER`） | **21 列**，末尾追加 `, dedup_key TEXT` |
| `dedup_key` 列 | 不存在（`A-2 n=0`） | 存在，`TEXT` / `notnull=0` / `dflt=NULL` / `pk=0` |
| 表上索引 | **0 个**（`idxs=<none>`） | **1 个**：`ux_mm_dedup_key`（UNIQUE + 部分） |
| 库体积 | `2134016` B | `2138112` B（+4096，与新增列/索引一致） |

★ 结构原文与本地 fixture（`test/real_d1/fixtures/mqtt_messages.sql`）执行前**逐列一致**，`0007` 只做 `ALTER + CREATE INDEX`，未改表定义。

---

## 5. migration 命令结果

证据：`g5-apply.txt`

```
命令      : wrangler d1 migrations apply guofeeder --remote
工作目录  : D:/Guo_Feeder_Project/Cloudflare_Assets/guo-feeder-api
开始      : 2026-10-21 01:23:15 +0800
结束      : 2026-10-21 01:23:20 +0800
耗时      : 5 s（命令内部 🚣 Executed 3 commands in 2.93ms）
待应用    : 1 migration（0007_mqtt_messages_dedup.sql）
交互       : 非交互上下文使用 fallback `yes`（wrangler 自身行为，已记录在输出里）
结果      : 0007_mqtt_messages_dedup.sql → ✅
apply_exit: 0
```

★★ **`apply_exit=0` 在本报告中不作为成功依据**。判定只取 §6 六项。

---

## 6. 六项验证（逐项）

| 项 | 判据 | 实测 | 判定 | 证据 |
|---|---|---|---|---|
| **V-1** | `col/idx/partial_idx/reg7 = 1/1/1/1` | `1 / 1 / 1 / 1` | ✅ **PASS** | `v1-structure.txt`（`01:23:34`） |
| **V-1b** | `type=TEXT` · `notnull=0` · `dflt_value=NULL` · `pk=0` | `TEXT / 0 / null / 0` | ✅ **PASS** | `v1b-column-attrs.txt`（`01:24:03`） |
| **V-1c** | `UNIQUE` + 单列 `(dedup_key)` + `WHERE dedup_key IS NOT NULL`，无 `COLLATE`/`DESC` | `CREATE UNIQUE INDEX ux_mm_dedup_key ON mqtt_messages(dedup_key) WHERE dedup_key IS NOT NULL` | ✅ **PASS** | `v1-structure.txt` |
| **V-2** | 四项与 `FINGERPRINT_BEFORE` 逐项相等；`keyed=0` | 六项全等、`keyed=0` | ✅ **PASS** | `v2-fingerprint-after.txt` |
| **V-2b** | 25 行全在且逐项相等（`device_id/dir/cmd/msg_id/tp_len/pl_len/pl_head`） | 25/25，`missing=[]`、`extra=[]`、`field_diffs=0` | ✅ **PASS** | `v2b-sample-post.txt` · `v2b-compare.txt` |
| **V-3** | `keyed_total=0` | `0` | ✅ **PASS** | `v3-keyed.txt` |

**V-3 增强证据（超出手册最低要求）**：`EXPLAIN QUERY PLAN SELECT id FROM mqtt_messages WHERE dedup_key='x'`
⇒ `SEARCH mqtt_messages USING COVERING INDEX ux_mm_dedup_key (dedup_key=?)`
⇒ 索引**真实可被查询规划器使用**，不是"建了但条件不匹配"。

**补充状态确认**：
- 登记二次确认：`d1 migrations list --remote` ⇒ `✅ No migrations to apply!`（`01:24:19`）。
- 索引总数 = **1**（`sqlite_master`，`01:24:39`）。
- apply 后 3 分钟复检：仍 `1/1/1/1`、`keyed=0`、库未再变（`v1e-recheck-3min.txt`，`01:24:57`）。

### 6.1 验证过程中的两次报错（均为查询写法问题，非结构问题）

| # | 现象 | 原因 | 处置 |
|---|---|---|---|
| E-1 | `V-1b` 首跑 `near "notnull": syntax error at offset 32` | SQL 用了 `AS notnull` —— `NOTNULL` 是 SQLite 关键字，**不能作未加引号的别名** | 去掉别名重跑（`01:24:03`）。★ SQL **未执行成功**，不产生任何状态变化 |
| E-2 | `P-4` 首跑 `APIError`（`pragma_index_list`） | `pragma_index_list()` 在 D1 不可用 | 改用 `sqlite_master` 列索引（总数 1） |

两次错误原文**保留未删**（同一文件内），并按事实标注；未用"改了就好"掩盖。

---

## 7. 生产数据完整性、异常与未验证事项

### 7.1 已确认

- 历史行**未被四项统计量捕获的改动为零**（`n_old`/`min_id`/`max_id`/三项长度和全等）。
- 25 行固定抽样**逐字段未变**（补充证据）。
- `dedup_key` 历史行为 **NULL**（`keyed=0`），**未回填**、未删行、未改行。
- `d1_migrations` 由 `apply` 自行写入，**未手工干预**；未用 `--file`。
- 表上原本**无任何索引**，现有且仅有 `ux_mm_dedup_key`。

### 7.2 异常

**无 P0 / 无 P1。** 唯一过程异常是 §6.1 的两条查询报错，均已归因并留痕。

### 7.3 未验证事项（必须随结论引用）

| # | 项 | 状态 | 说明 |
|---|---|---|---|
| N-1 | 生产 D1 的 **migration 原子性**（失败是否零残留） | `[未验]` | 本次**一次成功**，未触发失败路径。原子性仍只有本地引擎实测（`state-matrix.log`） |
| N-2 | 生产上 `S3`–`S6` 的真实行为 | `[未验]` | 生产从未进入这些状态；手册 §3.7.6 已标 `[未验]` |
| N-3 | 生产 `ALTER TABLE … DROP COLUMN` | `[未验]` | 本地可用，生产**未试过**；手册禁止在生产首次尝试 |
| N-4 | `0007` 的**实际去重效果** | **`NOT MEASURABLE`（当前）** | `insertMqttMessage()` 写入 `dedup_key` 的是 **C-2 的代码**；Worker 仍是 `v47`（旧代码不写该列）⇒ **此刻生产行为与 C-1 前完全相同**，列/索引处于"待用"状态 |
| N-5 | `B-3` 漏记根因 | **未定** | EMQX 数据集成类 API 仍 403；本次未做任何 EMQX 操作 |
| N-6 | `dedup_key` 非空后的唯一约束行为 | `NOT RUN` | 需要 C-2 代码先写入键；当前无一行有键 |

★ **不得**由本报告推出："生产已具备去重能力"、"C-1 提升了日志可信度"。
C-1 只完成**结构就绪**（列 + 部分唯一索引），效果取决于 C-2。

---

## 8. C-1 最终状态

> # `PASS`
>
> 判定依据：§3 八项门禁全绿 + §6 六项硬判据**全部通过** + §7 无异常。
> `apply_exit=0` 与 `No migrations to apply` **均未参与**该判定。

状态机位置：`S0` → **`S1`**（`col=1 / idx=1 / reg7=1`，定义正确）。

---

## 9. 门禁状态表（**本轮一条都没有改变**）

| 门禁 | 本次 C-1 前 | 本次 C-1 后 | 是否因 C-1 变化 |
|---|---|---|---|
| `G-4` | **`BLOCKED`** | **`BLOCKED`** | ❌ 未变 |
| `WP-5` | **未解锁** | **未解锁** | ❌ 未变 |
| `STOP-R1` | **无解除文件** | **无解除文件** | ❌ 未变 |
| `BT-1` | `CLOSED` | `CLOSED` | ❌ 未变 |

补充未解决项（与 C-1 无关，仍待人工）：
- `G4-R4` 的 `<70%` 阈值仍是**未经批准的建议值**；H-1…H-5 全为 `candidate:true`，`system.memwatch` 不据此判 PASS/FAIL。
- `B-3` 漏记：根因未定，EMQX 控制台只读取证仍待办。
- `SEQ-SEM`：`log_envelope_contract.md` 的 `seq` 口径未改，需人工确认。

---

## 10. 明确声明：未执行的操作

本次**仅**执行了：文档/证据的提交与普通 push、`d1 migrations apply guofeeder --remote`（一次）、执行后只读查询。

**明确未做**：

- ❌ **C-2 Worker 部署**（线上仍是 `v47` = `826332ed-…`，未变）
- ❌ **Worker 回滚 / 任何生产代码变更**
- ❌ **EMQX 规则 / Action / Connector / ACL 配置变更**（EMQX 仓 `ec2364f` 未动）
- ❌ **固件烧录 / 设备重启 / 设备配置修改**（设备仍 `sha256=94ba711c…`、`boot_seq=25`）
- ❌ 修改已应用的 migration、手工改 `d1_migrations`、用 `d1 execute --file` 代替正式流程
- ❌ 对 `S3`/`S4`/`S5`/`S6` 的任何自动恢复、补建 `0008`、`DROP` 对象
- ❌ 向生产 MQTT 发送任何探针消息
- ❌ 删除或改写任何证据

---

## 11. 下一步（**均需另行授权**）

1. **C-2 部署**：单独授权。部署后 `P-1'` 是**新行为检查**（不是回归）——`$tenants/*/down` 的 `device_id` 必须从 `http_api` 变成合法 12-hex；仍为 `http_api` ⇒ `B-1b` 未生效 ⇒ 按 §4.6 回滚到 `version f9647a00-…`。
2. **C-3 观察窗口**：C-2 之后、≥ 2 h，按 Rev.2.1 §5 执行（`R-6` 已修，`up` 不要求 `echo`）。
3. **建议加一次的 C-1 后置确认**：C-2 部署前重跑 `V-1`（`I-9` 要求 C-1 持续有效），确认仍是 `1/1/1/1`。
