# 阶段 1 报告：线上 Worker version / deployment ID 只读采集

- 采集日期：`2026-10-21`（`00:28:24`–`00:29:22`，本机时间 UTC+8）
- 性质：**只读**。未部署、未回滚、未改 Worker/Secrets/vars/路由/绑定
- 证据目录：`docs/evidence/G4-Live-Version-2026-10-21/`
- 结论：**已完成**（线上部署可唯一识别，账号与环境无歧义）

---

## 1. 环境与身份核实

| 项 | 值 | 来源 |
|---|---|---|
| 仓库路径 | `D:/Guo_Feeder_Project/Cloudflare_Assets` | `pwd` |
| 分支 / HEAD | `main` / `e1e8b791e3d00fa1ac80b6f6d3c3646390ad2dc2` | `git rev-parse` |
| `origin/main` | `e1e8b791e3d00fa1ac80b6f6d3c3646390ad2dc2`（**与 HEAD 一致**） | `git rev-parse` |
| 工作区 | **干净**（`git status --porcelain` = 0 行） | `git status` |
| wrangler | **4.145.0** | `wrangler --version` |
| node | v22.22.2 | `node --version` |
| 登录身份 | OAuth Token，`wyl94@foxmail.com` | `wrangler whoami` |
| Account ID | `9e72d201efec6925b3a468a0e3fb4928`（**仅此一个账号可见**） | `wrangler whoami` |
| Worker 名 | `guo-feeder-api` | `guo-feeder-api/wrangler.toml:1` + `deployments status` |
| 环境 | **无 `[env.*]` 段** ⇒ 只有顶层（生产）环境，无 staging/多环境歧义 | `grep -n "^\[env\." wrangler.toml` = 无匹配 |

`whoami` 的 scope 含 `workers (write)` / `workers_tail (read)` / `d1 (write)` / `pages (write)`。
本阶段只使用**读**接口，未触发任何写操作。

证据：`s1-whoami-and-baseline.txt`

---

## 2. 线上部署（结果）

### 2.1 当前生效（`wrangler deployments status`）

| 项 | 值 |
|---|---|
| **deployment_id** | `c666aeeb-150e-4b10-a155-59a13bdaab0b` |
| **version_id** | `826332ed-3a73-42c4-a090-a5bf737dc210`（`versions list` 的 **number = 47**） |
| 分流 | 单一版本 `100%`（`strategy=percentage`，**无灰度分流**） |
| 部署时间 | `2026-10-19T12:56:06.861548Z`（UTC） = `2026-10-19 20:56:06 +08` |
| 部署来源 | `source=wrangler`，`annotations.workers/triggered_by=deployment`，无自定义 message |
| author | `wyl94@foxmail.com` |

证据：`s1-deployments-status.json`

### 2.2 回退目标（上一个部署）

| 项 | 值 |
|---|---|
| deployment_id | `c28f2eba-2f5e-49c0-8205-206b15d9858e` |
| **version_id** | `f9647a00-e079-4de9-bcf1-8c9bc5344185`（**number = 46**） |
| 部署时间 | `2026-10-18T20:18:44.567038Z` = `2026-10-19 04:18:44 +08` |

证据：`s1-deployments-list.json`（最近 10 次部署，见下表）

### 2.3 最近 10 次部署（升序，末行为最新）

| 时间（UTC） | deployment_id | version_id | number |
|---|---|---|---|
| 2026-10-05T19:12:54Z | `3a7fcab1-17f1-4a43-a9ae-3ee5999f71c9` | `f10e29a4-f5dc-4f5e-b3ec-890cf59369b2` | 38 |
| 2026-10-07T16:54:23Z | `e64bc869-e39f-43a8-a353-560408949343` | `846384cd-8b84-488b-a7a1-3ee5f9fae52a` | 39 |
| 2026-10-11T19:59:14Z | `8634d680-077a-43b1-a4ee-a6ac2e5b6376` | `99e6d937-4408-47b7-8170-4bfa71c9dfec` | 40 |
| 2026-10-15T14:25:11Z | `73d193b8-1673-4113-b7b8-b05eefbc4cd6` | `dd5ab824-ec39-41f3-9990-a37903aaf61f` | 41 |
| 2026-10-16T17:28:43Z | `b26c7688-…` | `3e1b2e0b-ac4b-4b92-983a-62e68730e2a8` | 42 |
| 2026-10-16T17:48:28Z | `533b0645-…` | `1ffbbe2e-ed09-43f9-b576-06380bdec8e0` | 43 |
| 2026-10-17T15:24:58Z | `3bf90ae2-…` | `72023596-657a-4a65-aa62-81848540f324` | 44 |
| 2026-10-18T17:36:32Z | `9f4fee53-d382-4ce5-905c-ca734c7ca2ab` | `508461f3-0dcf-443c-98f9-d185f412174e` | 45 |
| 2026-10-18T20:18:44Z | `c28f2eba-2f5e-49c0-8205-206b15d9858e` | `f9647a00-e079-4de9-bcf1-8c9bc5344185` | 46 |
| **2026-10-19T12:56:06Z** | **`c666aeeb-150e-4b10-a155-59a13bdaab0b`** | **`826332ed-3a73-42c4-a090-a5bf737dc210`** | **47** |

★ 前 5 行的 deployment UUID 在原始 JSON 里完整可见（`s1-deployments-list.json`），本表为可读性省略中间段。

### 2.4 当前版本的元数据（`wrangler versions view`）

| 项 | 值 |
|---|---|
| Version ID | `826332ed-3a73-42c4-a090-a5bf737dc210` |
| Created | `2026-10-19T12:56:06.205Z` |
| Handlers | `fetch, scheduled` |
| Compatibility Date | `2026-08-21`（与仓库 `wrangler.toml:5` 一致） |
| Source | **`Unknown (version_upload)`** ⇒ API **不返回**源码/commit 来源 |
| D1 绑定 | `env.guofeeder_DB` → `0d8385ba-a1a6-4fcc-8e43-ae61c014c43d`（与 `wrangler.toml:15` 一致） |
| 环境变量 | `env.LOG_BOOT_WATERMARK = "1"` ★ **在线上版本里确认** |
| Secrets（仅名称） | `ADMIN_API_TOKEN` · `EMQX_APP_ID` · `EMQX_APP_SECRET` · `WEBHOOK_SECRET` |

★ 回退目标 `f9647a00`（v46）的元数据**逐项一致**（同 D1 绑定、同 `LOG_BOOT_WATERMARK="1"`、同 4 个 secret 名）
⇒ **回退到 v46 不会改动环境变量与绑定**，只换代码。

证据：`s1-version-view-current.txt` · `s1-version-view-prev.txt`

---

## 3. `rollback` 参数语义（核实结果）

本机 `wrangler 4.145.0 --help` 原文：

```
wrangler rollback [version-id]

🔙 Rollback a deployment for a Worker

POSITIONALS
  version-id  The ID of the Worker Version to rollback to  [string]
```

结论：**位置参数是 version id，不是 deployment id**。
⇒ 回退到上一个版本的正确命令是：

```bash
npx wrangler rollback f9647a00-e079-4de9-bcf1-8c9bc5344185 --message "…"
```

★ **未执行** rollback（本任务禁止）。

证据：`s1-rollback-help.txt`

---

## 4. 线上版本 vs 本地 HEAD（对照）

| 对照项 | 结果 | 判据强度 |
|---|---|---|
| 线上 v47 部署时间 | `2026-10-19T12:56:06Z` | 直接观测 |
| 本地 HEAD `e1e8b79` 提交时间 | `2026-10-20T23:23:57+08`（= `2026-10-20T15:23Z`） | 直接观测 |
| ⇒ HEAD 的 3 个修复（B-1b / B-2b / B-2c）是否在线 | **否**（部署时间早于提交时间 **约 26.5 h**） | ✅ **可证**：commit 的代码不可能出现在更早构建的版本里 |
| 线上 v47 对应哪个 commit | **无法确定** | `versions view` 的 `Source: Unknown (version_upload)`；部署时刻（`10-19 20:56 +08`）**附近没有对应提交**（当天提交为 `01:51 / 04:12 / 04:13 / 04:23 +08`，下一个是 `10-20 01:48 +08`） |
| 线上 v47 **是否已含 B-1 的 `/down` 方向判定** | **是**（✅ 行为证据） | v47 任期 `audit/down = 193`（§4.1-A） |
| 线上 v47 **是否已含 `$tenants` 设备段解析** | ❌ **否**（✅ 决定性证据） | 全库 1 539 行 `$tenants/*/down` 的 `device_id` **全是 `http_api`**，最新一条是刚刚（§4.1-B） |
| 原先的猜测"v47 ≈ `362ddfd`" | ❌ **已推翻** | `362ddfd`（`10-19 01:51`）早于 B-1 工作，不可能含 `/down` 解析 |
| 线上 D1 绑定 | 与应用配置一致 | ✅ 直接观测 |
| 线上 `LOG_BOOT_WATERMARK` | `"1"` | ✅ **直接观测（线上版本元数据）** |

★★ 两条必须写进结论的纪律：

1. **`git HEAD` 不能代替线上版本 ID**：二者相差约 26.5 h，且线上版本**无法**追溯到任何 commit。
2. **"线上跑了什么代码"目前只有间接证据**（部署时间 + 绑定/变量一致）。
   要说"线上是某个提交"，只能靠 C-2 的**行为探针**，不能靠 Git。

### 4.1-A 原判断（`2026-10-21 00:40`）：v47 **已含** `/down` 解析

> ⚠️ **本节结论已在同日 `00:5x` 被 §4.1-B 部分推翻，保留原文作留痕。**
> 保留下来的部分：v47 的 `direction` 分支确实对 `/down` 生效。
> 被推翻的部分：**不能**据此说 v47 已修好 `$tenants` 的设备归属。

按版本任期切开 `direction='down'` 的落库来源：

| 周期 | 部署时间（+08） | `audit`/`down`（plain 主题） | `echo`/`down`（`$tenants`） |
|---|---|---|---|
| **v46** 任期 | `10-19 04:18` → `10-19 20:56` | **2** | **133** |
| **v47** 任期 | `10-19 20:56` → `10-20 02:50` | **193** | **482** |
| 现在（v47，`10-20 23:22` 起） | — | **0** | **16** |

★ 关键对照：v46 任期有 **133 条** `echo/down`（说明下行确实在发生），但只有 **2 条** `audit/down`；
切到 v47 后立刻变成 **193 : 482**。⇒ **v47 能把 plain `guo_feeder/<id>/down` 解析成真实设备 ID，
v46 不能**（`/down` 方向支持是 `B-1` 的一部分）。

⇒ 这条**推翻**了"v47 ≈ `362ddfd`"的弱推断：`362ddfd`（`10-19 01:51`）早于 B-1 工作，
不可能含 `/down` 解析。v47 只能是**某份已含 B-1 部分改动的工作树**构建出来的
（`204212e` 直到 `10-20 04:24` 才提交）。

★ 新增一项**未解决异常**（详见阶段 2 §4.2）：`10-20 02:50` 之后，plain `down` 的 `audit` 行**归零**，
只剩 `echo`。原因未知，需 EMQX 控制台核对（规则是否被改/禁用）。

★ 方法学注记：本节是对**已有只读数据**的二次切分（按 `created_at` 区间），
没有新增查询面，也没有触碰任何生产写入。`v46` 任期的 2 条 `audit/down` 无法解释，
**不排除**是当时某次手工/临时动作，故只作"近零 vs 193"的对照使用。

### 4.1-B ★★ 更正：v47 **不含** `$tenants` 前缀剥离（`00:55` 新读数）

§4.1-A 的两列只按 **topic 前缀** 切分，**没有看 `device_id`** —— 这是它的方法学漏洞。
补查 `device_id` 后结论反转：

| 查询 | 结果 |
|---|---|
| 全库 `$tenants/` 行的 `device_id` 分布 | **`http_api` = 1 539 行**（`down`），**没有任何合法 12-hex** |
| 近 7 天 `$tenants/` 行 | `http_api` = 1 476 行，`MAX(created_at)` = `1792514438028`（**刚刚**） |
| 对照：近 7 天 **plain** `down` 行 | `valid12` = 193 · `phone` = 16 · `hd1_run_*` = 7（测试客户端） |

⇒ **两点结论（必须分开写）**：

1. ✅ v47 的 `direction` 判定对 `/down` 生效（`audit/down` 有 193 行）—— 这点 §4.1-A 是对的；
2. ❌ **v47 没有**把 `$tenants/<tenant>/guo_feeder/<id>/down` 解析成设备 ID
   ⇒ `device_id` 仍回落 `clientid`（`http_api`）。

因此 §4.1-A 里"v47 能把 plain `guo_feeder/<id>/down` 解析成真实设备 ID，v46 不能"这句
**不成立**：plain 主题的解析是 B-1 **之前**就有的能力（`deviceIdFromTopic` 老代码就认 `/up`），
`audit/down` 从 2 涨到 193 只说明"那段时间下行命令被记下来了"，**不能**推出"方向分支是新加的"。

**这条更正直接改变 C-2 的 `P-1'` 语义**（原本以为它只是回归检查）：

| 项 | 更正前（§4.1-A 的隐含假设） | 更正后（实测） |
|---|---|---|
| `$tenants` 行归属 | 应该已经是合法 ID，`P-1'` 只需确认"没退步" | **当前是 `http_api`** ⇒ `P-1'` 是**真正的新行为检查**，基线就是"错的" |
| C-2 部署后的期望 | 不要出现 `http_api` | `$tenants` 行**必须变成**合法 12-hex；若仍是 `http_api` ⇒ **B-1b 未生效 ⇒ 回滚** |

★ 也顺带解释了一个"看起来矛盾"的现象：`echo/down` 一直在增长（下行没断），
但**归属一直是错的** —— "链路通"与"归属对"是两件独立的事，不能互相证明。

★ 另一条待确认的副作用：plain `down` 里出现 `device_id='phone'` / `hd1_run_*`，
说明这些是**非 12-hex 的 clientid 回落**（测试客户端）。`B-1b` 之后
`isValidDeviceId('phone')=false` ⇒ 这类行会变成 `unresolved`。
⇒ C-2 验证时必须把"`phone` 类行消失"视为**预期变化**，**不得**判为故障。

★ 证据原文：`docs/evidence/G4-Live-Version-2026-10-21/s3-d1-baseline.txt` 同批只读查询
（`d1 execute --remote`，`meta.changed_db=false`）。

---

## 5. 已确认 / 未确认（分类）

**直接观测到的事实**

- 当前 deployment = `c666aeeb-…`，version = `826332ed-…`（number 47），100% 单版本
- 上一 version = `f9647a00-…`（number 46），其 deployment = `c28f2eba-…`
- 线上版本的 D1 绑定、`LOG_BOOT_WATERMARK="1"`、4 个 secret 名称
- 账号唯一（`9e72d2…`），无 `[env.*]` ⇒ 无多环境歧义
- `wrangler rollback` 的位置参数是 **version id**

**与事实相符但未证实（不得当结论）**

- 线上 v47 是从 `204212e` 之前某份**已含 B-1 部分改动**的工作树构建的（行为证据支持，但具体 commit 无法确定）
- 线上版本**不含**任何 2026-10-20 的修复（这一点可证，见 §4 第 3 行）
- v47 是否已含 `B-2`（写 `dedup_key`）—— 若含，则当前一直处于**缺列降级态**；D1 侧**不可观测**（降级后仍能无键写入）
- v47 与 v46 的**行为**差异（只能靠探针，不能靠元数据）

**当前无法测量**

- 线上版本的源码哈希 / 构建产物哈希（API 不返回）
- 灰度/隔离实例清单（Workers 不提供"当前活着的 isolate 有哪些、跑哪个版本"）
- v47 是"从干净工作树部署"还是"含未提交改动"（`Source: Unknown`）

---

## 6. 对后续阶段的意义

- C-2 的 `C2-VER`（**唯一硬阻塞**）**已解除**：`CURRENT_VERSION_ID` / `PREV_VERSION_ID` 已采集，且回退命令的参数类型已核实。
- 但 C-2 **仍未获授权**；本阶段只是让"版本识别与回退准备"具备条件。
- ★ 新增一条风险：由于**线上版本无法追溯到 commit**，C-2 部署后的"是否真的换了代码"**只能**用行为探针判（HTTP 200 / 部署成功都不算）。
- ★ C-1 不受本阶段影响（migration 与 Worker 版本无关），但 §2.4 的 `LOG_BOOT_WATERMARK="1"` 是**线上确认**，
  比此前"读仓库 `wrangler.toml`"更强 ⇒ C-1/C-3 都**不得改动**该变量。

---

## 7. 未决项

| ID | 项 | 状态 |
|---|---|---|
| `S1-U1` | 线上 v47 对应哪个 commit | **UNVERIFIED**（API 不返回源码来源；时间无对应提交）。**但已证：v47 含 B-1 的 `/down` 解析**（§4.1） |
| `S1-U2` | v47 是否含未提交改动 | **UNKNOWN** |
| `S1-U3` | v47 与 v46 的行为差异 | **NOT RUN**（需 C-2 探针，且 C-2 未授权） |
