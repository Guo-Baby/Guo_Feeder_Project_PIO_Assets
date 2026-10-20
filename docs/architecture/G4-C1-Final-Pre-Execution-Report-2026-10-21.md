# 阶段 2 报告：C-1 执行前最终复核（`0007` 生产 D1 migration）

- 日期：`2026-10-21`
- 复核依据：`G4-Production-Change-Plan-Rev2-2026-10-20.md`（**Rev.2.1**，唯一执行手册）
- 前置：阶段 1（`R-1`…`R-6`）已完成 → `G4-R1-R5-Rev3-and-R6-Report-2026-10-21.md`
- 证据目录：`docs/evidence/G4-C1-PreExec-2026-10-21/`
- **本阶段未执行任何生产变更**（全部 `SELECT` / `list` / `view`；`meta.changed_db=false`、`rows_written=0`）

---

## 结论

> # `READY FOR HUMAN APPROVAL`
>
> 只表示"**材料齐备，可提交人工审批**"，**不是执行许可**。
> 在收到对 C-1 的**单独书面授权**之前，不得执行 `migrations apply`。

`R-1`…`R-5` 六项前置**全部关闭**（`R-6` 属 C-3，也已关闭，见阶段 1 报告 §D）。
唯一的"未验"项是**生产上的半成品行为**——但它属于"若发生才需要"的分支，不构成执行前阻塞。

---

## 1. `R-1` 身份：四源一致性 ✅

| # | 来源 | 实测值 | 与目标一致 |
|---|---|---|---|
| ① | `wrangler whoami` Account ID | `9e72d201efec6925b3a468a0e3fb4928` | ✅ |
| ② | `wrangler.toml` | `binding=guofeeder_DB` · `database_name=guofeeder` · `database_id=0d8385ba-a1a6-4fcc-8e43-ae61c014c43d` | ✅ |
| ③ | `d1 list` | 仅一条 `guofeeder` = `0d8385ba-a1a6-4fcc-8e43-ae61c014c43d`（**无同名多库**） | ✅ |
| ④ | 线上 `version=826332ed…` 元数据 | `env.guofeeder_DB (0d8385ba-a1a6-4fcc-8e43-ae61c014c43d)` | ✅ |

证据：`r1-identity.txt` · `r2-artifact-and-binding.txt`

★ 附注（不影响结论）：`d1 list` 的 `num_tables` 列返回 **0**，与真实表数不符
（D1 API 未填充该字段）⇒ **不得**用这一列判断库状态。
★ 附注：`wrangler.toml` **无 `[env.*]` 段** ⇒ 无多环境歧义。
★ 附注：线上版本元数据同时确认 `LOG_BOOT_WATERMARK="1"`（本轮**不得改动**）。

---

## 2. `R-2` 制品一致性 ✅

| 项 | 值 | 判定 |
|---|---|---|
| `sha256sum` | `90dc0ec0da0335e8f55cab22121843dc83fd1dacbb7b6d0379bbd9d7ff7e2f1e` | ✅ == 审定值 |
| 工作区 blob | `eb300070be913ab272550cde4860448ed03575d2` | ✅ |
| `HEAD` blob | `eb300070be913ab272550cde4860448ed03575d2` | ✅ 与工作区相同 |
| 行数 | 74 行 | — |

⇒ 被审的制品 == 将被执行的制品（**且无未提交改动**）。

---

## 3. `R-5` 状态判定：**`S0`** ✅

执行前 `S-PROBE`（`r5-state-probe.txt`）：

```json
{"col":0,"idx":0,"partial_idx":0,"col_ok":0,"reg7":0,"nn":null,"dflt":null,"ctype":null}
```

| 字段 | 值 | `S0` 期望 | 判定 |
|---|---|---|---|
| `col` | 0 | 0 | ✅ |
| `idx` | 0 | 0 | ✅ |
| `partial_idx` | 0 | 0 | ✅ |
| `reg7` | 0 | 0 | ✅ |
| `nn` / `dflt` / `ctype` | `null` / `null` / `null`（列不存在） | 同 | ✅ |

⇒ **状态 = `S0`（0007 未应用，列与索引均不存在，登记不存在）** = 唯一允许直接 `apply` 的状态。

同时：`migrations list --remote` **只列 `0007`** ⇒ `0001`–`0006` 已登记，`0007` 待应用。

---

## 4. 表结构：与本地已验证 fixture 同构 ✅

| 项 | 结果 |
|---|---|
| 生产 `mqtt_messages` 列数 | **20** |
| fixture 列数 | **20** |
| 列集合完全一致 | **True** |
| `prod-only` / `fix-only` | 空 / 空 |
| `mqtt_messages` 上的索引 | **无**（`GROUP_CONCAT(name)` = `<none>`） |

生产列：`id, device_id, topic, direction, raw_payload, cmd, msg_id, object, stable_id,
version, status, error_code, message, timestamp, payload_json, created_at, status_text,
error_text, msg_type_text, msg_type`

⇒ 本地 `real_d1` 的 19/19 与 123/123 结论**可以引用**（schema 未分叉）。

⚠️ **未做**：逐列**类型/约束**的逐字比对（只比了列名集合）。
`0007` 只做 `ADD COLUMN`，不依赖既有列类型，故不构成阻塞；但若要把"同构"写到最强，
应补一次 `pragma_table_info` 全列 dump（`R-3` 的抽样已覆盖 25 行数据面）。
→ 列为**可选加强项**，不影响 `READY`。

---

## 5. 执行前基线已归档 ✅

| 项 | 值（`2026-10-21 00:58`，**执行时须重新采**） |
|---|---|
| `n` | **5 745** |
| `min_id` / `max_id` | `1` / **`5 763`** |
| `devs`（`COUNT(DISTINCT device_id)`） | 73 |
| `pl_len` / `id_len` / `tp_len` | `817 694` / `36 151` / `154 882` |
| `no_msg_id`（无 `msg_id` 行，参考） | 1 698 |
| `MAX_ID_BEFORE` | **5 763**（执行时会变，**必须现采**） |
| `V-2b` 抽样 | 25 行（`OFFSET 0 / 1430 / 2870 / 4300 / 5740`，各 5 行）已存 `v2b-sample-pre.txt` |

★ `MAX(id) − COUNT = 5 763 − 5 745 = 18` ⇒ **历史有 18 个 id 缺口**（上一轮已登记；
`retention.js` **不含** `mqtt_messages`，不是因为 cron 删除）。
⇒ 因此 `V-2` **不得**用"`max_id − min_id + 1 == n`"作判据。

---

## 6. 停止条件与恢复边界：已明确 ✅

| 场景 | 手册位置 | 动作 |
|---|---|---|
| 身份四源任一不等 | §2.0 | **停**，不执行 |
| `0007` 哈希不符 | §2.7 | **停**，重新审查 |
| 状态不是 `S0` | §3.7.5 流程图 | 按状态查 §3.7.4；`S3`/`S4`/`S5`/`S6` = **`STOP + HUMAN_REQUIRED`** |
| `apply_exit ≠ 0` | §3.2 | **停**，判状态 |
| `apply_exit = 0` 但 `V-1` 不全 1 | §3.4「伪成功」 | **停**，判状态（`S5`/`S6`） |
| `V-1b` 列属性不符 / `V-1c` 索引定义不符 | §3.3 | **P0 停 + 人工** |
| `V-2` 指纹变化 | §3.3 | 先排除并发与统计假象，再定 P0 |
| `V-2b` 25 行任一不等 | §3.3 | **P0 停 + 升级** |
| 已发生 `S3`–`S6` | §3.7.4 | **不自动** `0008`、**不自动**重跑、**不** DROP、**不**改登记表 |
| 执行后判定必须撤 | §3.5 | `DROP INDEX` ⇒ 进入 `S5′`（**不是 S0**），恢复需新 migration |

---

## 7. 执行方式：唯一正式路径 ✅

```bash
cd D:/Guo_Feeder_Project/Cloudflare_Assets/guo-feeder-api
node ../node_modules/wrangler/bin/wrangler.js d1 migrations apply guofeeder --remote
```

- ✅ 走 `d1_migrations` 登记（`migrations/README.md` 的要求）
- ❌ **禁止** `d1 execute --file migrations/0007_*.sql`
- ❌ **禁止** 手工 `INSERT/UPDATE/DELETE d1_migrations`
- ❌ **禁止** 与 Worker 部署、EMQX 改动"顺手一起做"

---

## 8. 本轮不成立 / 待人工的项（诚实边界）

| 项 | 状态 | 说明 |
|---|---|---|
| 生产 D1 半成品状态的**真实**行为（`S3`–`S6`） | **`[未验]`** | 只有本地实测（`state-matrix.log`）。生产从未进入这些状态，**不能**把本地结论当生产保证 |
| 生产 `ALTER TABLE … DROP COLUMN` 是否可用 | **`[未验]`** | 本地可用；手册**禁止**在生产首次尝试 ⇒ 这正是 `S3`/`S4` 必须人工的原因 |
| 生产 migration 事务原子性 | **`[未验]`（仅本地一致）** | 由 C-3 的每 10 min `V-1` 复检覆盖 |
| 逐列类型/约束逐字比对 | **未做**（只比列名集合） | 可选加强项 |
| `mqtt_messages` 漏记（B-3） | **仍未定** | 与本阶段无关，但**禁止**用该表单独作完整性证据 |

★ 明确**不得**由本报告推出的结论：
- ❌ "C-1 已授权"（本报告只是审批材料）
- ❌ "生产原子性已证明"
- ❌ "`echo` 判据改完 = B-3 解决"
- ❌ "`G-4` 可解锁 / `WP-5` 可解锁 / `STOP-R1` 可解除"（**门禁一条未动**）

---

## 9. 工作区与文件变更

| 项 | 状态 |
|---|---|
| 云端仓 | 工作区 **0** 改动；`HEAD == origin/main == e1e8b79` |
| EMQX 仓 | 工作区 **0** 改动；`HEAD = ec2364f` |
| 固件仓 | 工作区有改动（本轮的 2 份报告 + 1 个修订报告 + 证据目录 + 3 份文档修订）；**未提交、未推送** |
| 生产 D1 | **零写入**（`changed_db=false` / `rows_written=0`，见 `readonly-meta.txt`） |
| 生产 Worker | **未部署、未回滚** |
| 设备 | **未烧录、未重启、未改配置** |

---

## 10. 需要人工做的一件事

**书面授权（或不授权）C-1。** 授权前请确认：

1. 接受"**`apply_exit=0` 不是成功信号**"（`S5` 会伪成功）；
2. 接受"`S3`/`S4` = `STOP + HUMAN_REQUIRED`"（不给自动恢复命令）；
3. 接受"回滚后进入 `S5′`，恢复需新 migration"；
4. 指定**谁**在执行后 10 分钟内完成 `V-1`/`V-1b`/`V-1c`/`V-2`/`V-2b`/`V-3` 六项并回填记录。

授权后按 Rev.2.1 §3.2 执行；**执行期间不得同时做 C-2 的任何动作**。
