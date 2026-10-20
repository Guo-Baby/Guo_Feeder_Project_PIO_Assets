# G4 · C-2 生产部署与验收执行报告（Worker 部署）

- 报告日期：`2026-10-21`
- 执行窗口：`01:34:36` – `01:42:04 +0800`
- 执行依据：`G4-Production-Change-Plan-Rev2-2026-10-20.md`（**Rev.2.1** §4）+ `G4-Real-Hardware-Acceptance-Checklist.md`
- 证据目录：`docs/evidence/G4-C2-Deploy-2026-10-21/`（含 3 个可复现脚本）
- 前置：C-1 已于 `01:23:20` 完成（`PASS`，`0007` 应用，状态 `S1`）

---

## 1. 总状态

> # `BLOCKED`
>
> **不是 FAIL** —— 没有任何硬性判据失败。
> **不是无条件 PASS** —— `P-1'`（本次真正的新行为检查）的**真实流量**验证
> 因设备自 `01:20` 起静默（部署后近 10 min 真实写入 **0 行**）而 `NOT RUN`。
>
> 已取得的强证据：`$tenants/<tenant>/guo_feeder/<id>/down` 经**合成样本**直接落库为
> 合法 12-hex（`0b0077a7e402`），**不再是 `http_api`** ⇒ `B-1b` 的代码路径已被直接验证。
> 但这**不能替代**真实回流的验证，二者不得互相替换。
>
> **闭合条件**：设备恢复上行后，跑附录 A 的三条查询即可把 `P-1'/P-1''/P-3'` 由 `NOT RUN` 转正式结论。

---

## 2. 实际部署结果

| 项 | 部署前 | 部署后 |
|---|---|---|
| Worker | `guo-feeder-api` | 同 |
| **version id** | `826332ed-3a73-42c4-a090-a5bf737dc210`（v47） | **`d7fcd4f3-c9f3-4a7c-ba2f-c7e3bce74016`** |
| deployment | `c666aeeb-150e-4b10-a155-59a13bdaab0b` | `Created: 2026-10-20T17:39:04.490Z`（100%） |
| D1 绑定 | `0d8385ba-a1a6-4fcc-8e43-ae61c014c43d` | **同**（`versions view` 确认） |
| `LOG_BOOT_WATERMARK` | `"1"` | **`"1"`（未改动）** |
| 源码 commit | `e1e8b79` | 同 |
| 工作区 | 干净 | 干净（`worktree_dirty=0`） |
| 制品体积 | — | `157.33 KiB` / gzip `39.83 KiB`，Startup `2 ms` |

**命令与时间**：`wrangler deploy`（`4.145.0`，`D:/Guo_Feeder_Project/Cloudflare_Assets/guo-feeder-api`）
`01:38:56` → `01:39:08`（12 s），`deploy_exit=0`，`Uploaded (3.96 sec)` / `Deployed triggers (1.39 sec)`。

**回滚能力（部署前已确认，非假设）**：

- `wrangler rollback --help` ⇒ 位置参数为 **`version-id`**（不是 deployment id）。
- `deployments list` 实测：`c28f2eba-2f5e-49c0-8205-206b15d9858e` ↔ `f9647a00-e079-4de9-bcf1-8c9bc5344185`（v46，100%）。
- ⇒ 回滚命令为 `wrangler rollback f9647a00-e079-4de9-bcf1-8c9bc5344185 --message "…"`。**本次未触发。**

**已审定修复的制品核实（以源码为准，不凭 commit message）**：

| 修复 | 落点（实际源码） | 核实 |
|---|---|---|
| `B-1` 主题归属 | `device-registry.js::deviceIdFromTopic`（含 `$tenants` 剥 2 段 / 严格 3 段 / 方向白名单 `up,down,log`） | ✅ |
| `B-1b` 信任闸门 | `device-registry.js::topicClaimsDeviceFamily` + `resolveIngestDeviceId` 四级优先级 | ✅ |
| `B-2` / `B-2b` 幂等 | `mqtt-dedup.js::computeDedupKey`，缺 `msg_id` 返回 `null`（强度 C 不做幂等） | ✅ |
| `B-2c` log 方向 | `index.js` 方向判定补 `topic.endsWith("/log") ⇒ "log"` | ✅ |

关键文件 sha256：`index.js 9e53c5e2…` · `mqtt-dedup.js cf1c6e44…` · `device-registry.js ffab4a7e…` · `history-ingest.js 2e08227c…`

---

## 3. 验收矩阵

状态取值只允许 `PASS` / `FAIL` / `NOT RUN` / `BLOCKED` / `HUMAN_REQUIRED`。

### 3.1 手册 §4.4 探针项

| ID | 判据 | 实际执行 | 原始结果 | 状态 | 证据 |
|---|---|---|---|---|---|
| **P-2** 哨兵 | `guo_feeder/BADID/up` ⇒ `device_id='unresolved'`（不是 `BADID`/`http_api`） | 合成 ×1（`G4PROBE-P2-1792518018`） | `id=5764 · device_id=unresolved · dir=up` | **PASS** | `c2-probe-verify.txt` |
| **P-4** 去重 | 同 payload 连发 3 次 ⇒ D1 仅 1 行 | 合成 ×3，**再复压 ×2**（共 5 次，逐字节相同） | `id=5767 · 行数=1`（5 次投递）；复压 2 次均 `200 OK`，行数不变 | **PASS** | `c2-probe-verify.txt` · `c2-dedup-repress.txt` |
| **P-5** 无 `msg_id` | 同 payload 连发 2 次 ⇒ **2 行**，且 `dedup_key` 均 `NULL` | 合成 ×2（`{"c":"probe","m":"G4PROBE-P5-…"}`） | `id=5770,5771` · `n=2` · `null_keys=2` | **PASS**（**未误合并**） | `c2-probe-verify.txt` |
| **P-1'** `$tenants` 归属 | 窗口内 `$tenants/*` 行 `device_id` 为合法 12-hex | **真实流量：0 行（设备静默）**；合成 ×1 | 合成：`id=5765 · device_id=0b0077a7e402 · dir=down · topic=$tenants/n302933b/guo_feeder/0b0077a7e402/down` | **NOT RUN**（真实）/ **PASS-SYN**（合成） | `c2-post-traffic.txt` · `c2-probe-verify.txt` |
| **P-1''** plain 归属（回归） | plain 行仍为合法 12-hex | 由 P-4/P-5 的写入覆盖 | `guo_feeder/0b0077a7e402/up` ⇒ `device_id=0b0077a7e402` | **PASS-SYN** | `c2-probe-verify.txt` |
| **P-3'** 方向 | 窗口内 `direction='log'` 行数 > 0 | 合成 ×1（无真实 `/log` 流量） | `id=5766 · dir=log`；全表 `log=1`（**首次出现**） | **PASS-SYN** | `c2-post-traffic.txt` |
| **预期变化** `DEVICE-ATTR-SIDE` | `phone` / `hd1_run_*` 等非 12-hex 回落应变为 `unresolved` | 无新样本（设备静默） | 历史行未变（不可回溯） | **NOT RUN** | — |

### 3.2 数据完整性与结构

| ID | 判据 | 结果 | 状态 | 证据 |
|---|---|---|---|---|
| **I-1** | `dedup_key` 重复组 = 0 | `dup_key_groups=0` | **PASS** | `c2-invariants.txt` |
| **I-2** | 窗口内有 `msg_id` 的行必须全有键 | `null_key=0 / total=4` | **PASS** | 同上 |
| **V-1**（C-1 持续有效，`I-9`） | `col/idx/partial_idx/reg7 = 1/1/1/1` | `1/1/1/1` | **PASS** | 同上 |
| 唯一约束行为 | 重复投递应 `DO NOTHING`，**不得**抛错 | 5 次投递全部 `200 OK`，无 5xx、无约束异常 | **PASS** | `c2-dedup-repress.txt` |
| 历史行完好 | 不得删/改历史行 | `min_id=1` 不变；`keyed` 仅新增 4 条（全是 `G4PROBE-`）；历史 5745 行 `dedup_key` 仍全 `NULL` | **PASS** | `c2-invariants.txt` |
| 表规模 | 只升不降 | `n 5745 → 5751`（+6 = 探针 6 行）；`max_id 5763 → 5771` | **PASS** | 同上 |

### 3.3 audit / echo 方向判据（手册 §5.3 `R-6`）

| 项 | 结论 | 状态 |
|---|---|---|
| `I-8`（`down` 的 `echo>0`） | 部署后无真实下行回流（10 min 内 `real_rows=0`）⇒ 无法判定 | **NOT RUN** |
| `I-8b`（`up` **不要求** echo） | 已按 `R-6` 执行；本次**未**以 `up` 无 echo 判失败 | 遵从（非检查项） |
| `I-8c`（`log` 走 `log_history`） | `log_history` 近 30 min = 0（部署前后同为 0） | **NOT RUN** |
| `I-8d`（`unknown` 窗口内 = 0） | 全表 `unknown=876` 未增；探针未产生 `unknown` | **PASS（无新增）** |
| `audit/down` 归零是否已解决 | **未解决**：本次无任何 EMQX 侧证据 | **BLOCKED** |

★ **不得**由本次推出"漏记率已测"或"B-3 已解决"。EMQX 数据集成 / Rule / Action / Connector API **仍 403**，未取得任何 per-topic 证据。

### 3.4 失败判据（手册 §4.6）逐条核对

| # | 失败判据 | 本次实测 | 触发? |
|---|---|---|---|
| 1 | P-4 落 3 行 / P-5 落 1 行 | P-4=1 行、P-5=2 行 | ❌ 未触发 |
| 2 | P-1' 窗口内出现 `http_api` | 新增唯一 `$tenants` 行为 `0b0077a7e402`；历史 `http_api` 1539 行**未新增** | ❌ |
| 3 | `V-1.reg` 变 0 / 索引消失 | `1/1/1/1` | ❌ |
| 4 | `log_history` 增速降 > 50% | 基线 0、部署后 0 ⇒ **无法计算**（不是"通过"） | ❌ 未触发（`NOT MEASURABLE`） |
| 5 | `give_up` / `drop_unacked` 增长 | 未采（需串口，C-2 未授权设备操作） | `NOT RUN` |
| 6 | 唯一约束**抛出** | 5 次重复投递全部 200，无异常 | ❌ |

### 3.5 观测可用性（手册 §4.5）

| 通道 | 结论 |
|---|---|
| D1 只读 SQL | ✅ 可用（**本报告全部判定的唯一依据**） |
| `wrangler tail` | `INCONCLUSIVE` —— 后台运行 8 秒，**零输出、零报错**，不足以判定可用或不可用 |
| Workers Logs（`outcome=duplicate`） | **UNVERIFIED**（未验证） |
| EMQX 指标 | ❌ **403** |

⇒ 因此**没有**任何"去重率"结论。`outcome=duplicate` 的计数属于 `NOT MEASURABLE`（`N-2`）。

---

## 4. P-1′ 结论（本次核心）

**`$tenants` 前缀剥离已生效 —— 但只在合成样本上被直接证明。**

| 维度 | 事实 |
|---|---|
| 代码 | `deviceIdFromTopic` 对 `parts[0]==='$tenants'` 时执行 `parts.slice(2)`，再要求恰好 3 段、`seg[0]==='guo_feeder'`、方向在 `up,down,log`、`seg[1]` 为 12-hex |
| 合成实证 | `$tenants/n302933b/guo_feeder/0b0077a7e402/down` → `device_id=0b0077a7e402`、`dir=down`（id 5765） |
| 对照（部署前基线） | 全库 1539 行 `$tenants/*/down` 的 `device_id` **全为 `http_api`**，最新一条 `created_at=1792514438028` |
| 部署后同口径复查 | `http_api` 仍 **1539 行（未增）**；新增唯一 `$tenants` 行为合法 12-hex |
| **真实回流** | **0 行** ⇒ 无法证明"真实 EMQX 回环也走这条解析路径" |

★ 明确不写："生产已确认 `$tenants` 真实流量归属正确"。只能写"代码路径在合成样本上已被直接验证，
真实流量待复验"。

---

## 5. 去重与数据完整性结论

**已验证**

1. 同 `msg_id` + 同 payload 重复投递被拦截：5 次 → 1 行，且 HTTP 始终 200（`DO NOTHING`，非抛错）。
2. `dedup_key` 唯一性成立：重复组 = 0。
3. 无 `msg_id` 的 payload **未**被当作身份（`B-2b`）：同 payload 2 次 → 2 行、键均为 `NULL`，
   **没有发生误合并**（这是最严重风险的排除）。
4. 有 `msg_id` 的新写入全部有键（`null_key=0`）。
5. `C-1` 的列与部分唯一索引在部署后依然完好（`V-1=1/1/1/1`）。

**未验证**

- 真实流量的去重率（`N-2` `NOT MEASURABLE`：被索引拦截后 D1 看不到，Worker 日志通道未证实）。
- `/up`、`/log` 的完整性：**不得**用 `mqtt_messages` 总行数推断（该表自 `2026-10-16` 起只收 `up`/`down`，`/log` 走 `log_history`）。
- 不同统计口径的计数差值**不得**称为"丢失"（`A-1`/`A-2`/`A-5` 均为近似指标）。

---

## 6. audit / echo 结论

| 分类 | 内容 |
|---|---|
| **已证实事实** | `R-6` 已落实：仅 `down` 要求 `echo`；`up` 不要求。全库 `echo` 历史上只存在于 `down`（1539 行）。 |
| **间接证据** | 无。本次未取得任何 EMQX Rule / Action / Connector 计数。 |
| **未知项** | `audit/down` 自 `2026-10-20 02:50` 归零的原因；`I-8`（部署后 `down` 的 `echo>0`）因无下行流量无法判定；漏记（`B-3`）分层定位。 |
| **状态** | `I-8` = `NOT RUN` · `audit/down` 根因 = `BLOCKED`（403） |

---

## 7. 停止 / 回滚情况

**未发生停止，未执行回滚。**

- 全部 6 条失败判据均未触发（§3.4）。
- 探针首轮遇到 `403 error 1010`（Cloudflare 客户端签名拦截），**请求未到达 Worker**，
  随后用 D1 确认 `probe_rows=0`（零写入）后才改用浏览器 UA 重试。此过程**不构成**失败判据。
- 回滚目标保持可用：`f9647a00-e079-4de9-bcf1-8c9bc5344185`（v46）。

**若后续需要回滚**（手工执行，勿改判据）：

```bash
cd D:/Guo_Feeder_Project/Cloudflare_Assets/guo-feeder-api
node ../node_modules/wrangler/bin/wrangler.js rollback f9647a00-e079-4de9-bcf1-8c9bc5344185 --message "G4 C-2 rollback: <原因>"
```

回滚**不会**撤销 `0007` 的列与索引（旧代码不写该列，无害），也**不能**复活部署期间被
`DO NOTHING` 丢弃的独立事件。

---

## 8. 证据归档路径

- 报告：`docs/architecture/G4-C2-Deployment-and-Acceptance-Report-2026-10-21.md`
- 证据：`docs/evidence/G4-C2-Deploy-2026-10-21/`（共 16 个文件，含 3 个可复现脚本，索引见其中 `README.md`）
- 数据变动（**全部由探针产生，永久保留，不删除**）：
  - `mqtt_messages` 新增 6 行，`raw_payload` 均含 `G4PROBE-`：`id` = `5764,5765,5766,5767,5770,5771`
  - `device` 新增 1 行：`0b0077a7e402`（`CLAIM_PENDING`，手册 §4.4 允许的合成设备）
  - **真实设备 `288485896ce4` 的 `last_seen` 未被改动**（`1792509817`，与部署前基线一致）——
    这是选用合成 ID 而非真实 ID 的目的

---

## 9. 剩余问题与后续建议

| ID | 级别 | 问题 | 建议 |
|---|---|---|---|
| **`C2-P1P-REAL`** | **MAJOR（验收缺口）** | `P-1'/P-1''/P-3'` 的真实流量版本 `NOT RUN` | 设备恢复上行后跑附录 A，即可闭合 |
| **`C2-SEC-WEBHOOK-NOAUTH`** | **MAJOR（安全，既存）** | Pages `functions/api/webhook.js` 转发 Worker 根路径**不带 `X-Webhook-Key`**，而 Worker 根路径鉴权块被注释 ⇒ 公网**无鉴权即可写 `mqtt_messages`** | 需单独授权的 Worker 改动（启用 `requireWebhookKey`）；**本次未改** |
| **`C2-ID-GAP`** | MINOR（口径） | 本轮 `id_gap` 由 **18 → 20**（`max_id 5763→5771`，行数 `5745→5751`）：2 个序号被 P-4 的重复投递消耗 | 后续任何"id 缺口"分析**必须**扣除本轮的 2 个；缺口≠丢失 |
| **`C2-TAIL`** | MINOR（观测） | `wrangler tail` 8 秒零输出，可用性仍未定 | 若要测 `outcome=duplicate`，先单独验证 tail/logs |
| **`C2-ATTR-SIDE`** | MINOR（未验） | `phone`/`hd1_run_*` → `unresolved` 的预期变化无新样本可验 | C-3 窗口内顺带核对 |
| **`C2-REPRESS-ID`** | MINOR（未解释） | 复压 2 次重复投递**未**继续推进 `max_id`（前 2 次冲突各消耗 1 个 id），机制未确定 | 不影响判据；若后续要精确算序号，需在本地复现 |

**不得写成已解决**：`B-3` 漏记、`audit/down` 归零、`G4-R4` 阈值、`SEQ-SEM` 契约口径。

---

## 10. 门禁状态（逐项）

| 门禁 | 执行前 | 执行后 | 依据 |
|---|---|---|---|
| **`G-4`** | `BLOCKED` | **`BLOCKED`** | `G-4` 要求 `loop()` P99 **2 h+30 min** 长期窗口与 DRAM 阈值裁决（`G4-R4` 仍未批准）；C-1/C-2 均不构成其证据 |
| **`WP-5`** | 未解锁 / `BLOCKED` | **未解锁 / `BLOCKED`** | `WP-5` 依赖 `G-4` 通过；无独立解锁文件 |
| **`STOP-R1`** | 无解除文件 | **无解除文件** | 无对应审批记录 |
| **`BT-1`** | `CLOSED` | **`CLOSED`** | 未受影响 |

★ 部署成功、D1 写入成功、单项探针通过，**均不构成**解锁依据。
★ `LOG_BOOT_WATERMARK="1"` 未改动；`0007` 未改动；EMQX 未改动；设备未烧录/未重启。

---

## 附录 A：真实流量复验（设备恢复上行后执行）

```sql
-- A-1 P-1' 真实 $tenants 归属（期望：新增行全为 12-hex，http_api 不再增长）
SELECT device_id, COUNT(*) AS n, MAX(created_at) AS last_ms
  FROM mqtt_messages
 WHERE substr(topic,1,9)='$tenants/' AND created_at > 1792517900000
 GROUP BY device_id;

-- A-2 P-1'' plain 归属回归（期望：合法 12-hex，不出现 http_api）
SELECT device_id, COUNT(*) AS n
  FROM mqtt_messages
 WHERE substr(topic,1,9)<>'$tenants/' AND created_at > 1792517900000
   AND COALESCE(raw_payload,'') NOT LIKE '%G4PROBE-%'
 GROUP BY device_id ORDER BY n DESC;

-- A-3 P-3' 方向分布（期望 direction='log' 增长，unknown 不增长）
SELECT COALESCE(direction,'-') AS dir, COUNT(*) AS n
  FROM mqtt_messages
 WHERE created_at > 1792517900000 AND COALESCE(raw_payload,'') NOT LIKE '%G4PROBE-%'
 GROUP BY dir;
```

判定：A-1 出现 `http_api` ⇒ **立即回滚**（`B-1b` 未生效）；A-3 `unknown` 增长 ⇒ 检查是否有未归类主题。
