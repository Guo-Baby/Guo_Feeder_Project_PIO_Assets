# C-2b 部署证据：带 G4-R7 修复的第二次生产部署（保持 observe）

> **执行时间**：`2026-10-21 04:03:52`–`04:05:13 +0800`
> **授权**：用户第八轮「可以部署」（承接 S-1～S-6 报告中「要不要现在做第二次部署」的提问）
> **范围**：仅一次 Worker 部署，把 `7c96b18`（含 S-3 `b061a1c` + S-4 `7c96b18`）上线。
> **未做**：切 `enforce`、加读端点、改 EMQX、烧录、C-3、任何 D1 写操作。

## 1. 结果一句话

`1fb56499-…` → **`4eb8d91b-bdf7-42cb-9e22-81afd3670096`（100%）**，`deploy_exit=0`，
`INGEST_VALIDATION ("observe")` 生效值经版本元数据确认，`LOG_BOOT_WATERMARK ("1")` 未动，
D1 绑定 `0d8385ba-…` 未变，源码哈希部署前后逐字相同。

## 2. 文件索引

| 文件 | 内容 |
|---|---|
| `c2b-pre.txt` | 部署前门禁：HEAD==origin/main==`7c96b18`、工作区干净、四文件 sha256、`observe` 默认值、`enforce` 仅出现在解析处、G4-R7 SQL 已含两字段、部署前版本 `1fb56499` |
| `c2b-deploy.txt` | 部署记录：起止时间、完整输出、`deploy_exit=0`、新 version `4eb8d91b-…` |
| `c2b-post-verify.txt` | 生效验证：`deployments status` 100% = `4eb8d91b`、`versions view` 三项绑定、源码哈希复核 |
| `c2b-post-d1.txt` | 部署后生产只读快照：`n=5768 / mx=5790 / keyed=20`、`age_s=106`（设备在线）、水位表当前值 |
| `c2b-real-traffic.txt` | 真实流量核查：最近 16 行、`$tenants` 归属分组、方向分布 |
| `c2b-tail.txt` | `wrangler tail` 尝试结果：**`ETIMEDOUT`（沙箱网络）⇒ 失败，非零输出** |

## 3. 部署前门禁（`04:03:52`）

| 项 | 实测 | 判定 |
|---|---|---|
| 源码一致性 | `HEAD == origin/main == 7c96b18`，`worktree_dirty=0` | ✅ |
| `INGEST_VALIDATION` | `wrangler.toml:46 = "observe"`；`DEFAULT_INGEST_VALIDATION = "observe"` | ✅ 不会意外启用 enforce |
| `enforce` 字面位置 | 仅 `index.js:538` 分支判断 + `resolveValidationMode` 的 `raw.trim() === "enforce"` | ✅ 无隐式默认 |
| G4-R7 修复在制品内 | `LOG_REBASE_UPDATE` 同时含 `last_rewind_at = NULL` 与 `last_rewind_boot = NULL`（同一条 UPDATE） | ✅ |
| 部署前版本 | `1fb56499-49fb-4e07-b2c6-6fd35cf60329`（100%） | ✅ 回滚目标已记录 |

## 4. 部署（`04:04:08` → `04:04:19`）

```
Total Upload: 163.43 KiB / gzip: 41.60 KiB
Worker Startup Time: 1 ms
env.guofeeder_DB (guofeeder)      D1 Database
env.LOG_BOOT_WATERMARK ("1")      Environment Variable
env.INGEST_VALIDATION ("observe") Environment Variable
Current Version ID: 4eb8d91b-bdf7-42cb-9e22-81afd3670096
# deploy_exit=0
```

## 5. 生效验证（`04:04:23`）

- `deployments status`：**`(100%) 4eb8d91b-bdf7-42cb-9e22-81afd3670096`**
- `versions view`：`env.INGEST_VALIDATION ("observe")` · `env.LOG_BOOT_WATERMARK ("1")` · `env.guofeeder_DB (0d8385ba-a1a6-4fcc-8e43-ae61c014c43d)`
- 源码哈希部署前后**逐字相同**（`index.js cedbe9ad…` / `ingest-validate.js cd52846c…` / `log-boot-watermark-rebase.js fd5ad725…` / `wrangler.toml c22c8232…`）

★ **`deploy_exit=0` 与「100%」都不构成行为正确性证明**，下面的真实流量证据才是。

## 6. 本次最重要的收获：真实流量回来了

部署后采集发现设备**在线**：`age_s=106`、`n_30min=12`。

### 6.1 `P-1'` 真实版（C-2 原本缺的那块）

| `device_id` | 行数 | id 区间 | 说明 |
|---|---|---|---|
| **`288485896ce4`** | **16** | `5774`–`5790` | ✅ 真实设备，`$tenants/n302933b/guo_feeder/288485896ce4/down` 归属正确 |
| `0b0077a7e402` | 1 | `5765` | C-2 合成探针（`G4PROBE-P1T-…`） |
| `http_api` | 1539 | `73`–`5763` | 历史错误归属，**max id 冻结在 5763，没有任何新行** |

⇒ `B-1b` 在生产真实流量上生效：`$tenants` 前缀被正确剥离，`device_id` 是合法 12-hex，不再是 `http_api`。

### 6.2 `P-1''` 真实版（plain 主题）

`id=5779`：`topic=guo_feeder/288485896ce4/up` → `device_id=288485896ce4`、`dir=up`、`dedup_key=NULL`（该行无 `msg_id`）。
⇒ plain 主题归属未回归，且"缺 `msg_id` 不生成键"的旧语义保持。

### 6.3 observe 未拒绝合法报文的间接证据

`5780`–`5790`（11 条 `ack:25:6166…6176`，`20:02:33`–`20:02:54 UTC`）写于 **S-3 部署（`03:16:08`）之后、本次部署（`04:04:19`）之前**，全部正常落库、`direction=down`、`dedup_key` 均生成。
⇒ 观察模式下合法报文**照常写入**，符合设计。

## 7. 这次**没能**验证的（如实留缺）

| 项 | 原因 | 状态 |
|---|---|---|
| `observe` 的 `invalid_total` 实际值 | 计数器在 isolate 内存、无查询端点；`wrangler tail` 本次 **`ETIMEDOUT`**（沙箱网络，非 Worker 故障） | **`NOT MEASURABLE`**（≠ 0） |
| `G4-R7` 的行为 | rebase 只在检测到 rewind 时触发；本次部署期间未发生 rebase | **代码已上线，行为未触发** |
| `P-3'` 真实 `direction='log'` 行 | 真实流量中仍只有 C-2 探针那 1 行；设备当前只发 `up`/`down`（日志走 `log_history`） | **真实样本 `NOT RUN`** |
| `I-8` audit/echo 方向语义 | EMQX 数据集成 API 全 403 | **`BLOCKED`** |
| 切 `enforce` | 前置条件未满足（需真实流量 `invalid_total=0` + 单独授权） | **`NOT DONE`（按授权）** |

## 8. 水位表当前值（G4-R7 的观测对象）

```
device_id=288485896ce4  epoch=2  max_boot_seq=21  rewind_count=42
ambiguous_count=335  last_rewind_at=1792434955  last_rewind_boot=20
last_rebase_at=1792417227
```

`last_rewind_at`（`1792434955`）**晚于** `last_rebase_at`（`1792417227`）⇒ 说明上次 rebase 后又累积了 42 次 rewind，
这是**自洽**状态，不是 `G4-R7` 描述的"`rewind_count=0` 却有 `last_rewind_at`"矛盾态。
`G4-R7` 修复只在下一次 rebase 时才会产生可观测差异。

## 9. 回滚

```bash
wrangler rollback 1fb56499-49fb-4e07-b2c6-6fd35cf60329   # 位置参数是 version id
```

回滚代价说明：`1fb56499` 已含 S-3 校验器（observe），但**不含** `G4-R7`。回滚后生产行为与 `04:04` 之前完全相同。

## 10. 声明

- 本次**只**做了一次 Worker 部署，**没有**任何 D1 写操作（全部 `SELECT`，`changed_db=false`）。
- **未**切 `enforce`、**未**加读端点、**未**动 EMQX、**未**烧录、**未**重启设备、**未**执行 C-3、**未**修改已应用 migration、**未**删除任何历史证据。
- 门禁状态**一条未变**：`G-4 = BLOCKED` · `WP-5` 未解锁 · `BT-1 = CLOSED` · `STOP-R1` 无解除文件。
