# G4 · 真机前 S-1～S-6 执行报告（`2026-10-21 03:00–04:05`）

> **性质**：正式执行报告（可审计）。
> **授权来源**：用户书面任务书《S-1 至 S-6 真机前工作执行任务书》（`2026-10-21 03:00`）。
> **总状态**：**`PARTIAL — 文档与编码全部完成；S-3 已部署 observe；S-4 修复未部署；真机项未验收`**。
> ★ 本报告**不**宣告任何真机验收通过，**不**因文档裁决完成而解除任何门禁。

---

## 0. 总状态与依据

| 项 | 状态 | 依据 |
|---|---|---|
| S-1 `seq` 契约 | ✅ **完成** | 3 份现行文档改「跨 boot 连续」；`log_manager.cpp` **零 diff** |
| S-2 DRAM 阈值 85% | ✅ **完成** | `R-3` 已写入门禁记录；BT-1 §6.2 / 验收清单 `6-8` / 草案横幅同步；历史 `FAIL@70%` 未动 |
| S-3 入站校验 + observe | ✅ **完成并已部署 observe** | 25 套件全绿 + 本地 15 用例 + 生产 version `1fb56499-…`（100%，`INGEST_VALIDATION="observe"`） |
| S-4 `G4-R7` | ⚠️ **代码+测试完成，★ 未部署** | `LOG_REBASE_UPDATE` 已改；套件 97/97；生产仍不含该修复（`S4-NOT-DEPLOYED`） |
| S-5 注释修正 | ✅ **完成** | `loop_stats.h` 预算 10 → 15 µs；`sizeof==576` 经编译器核实**本来正确** |
| S-6 Homie 构建矩阵 | ✅ **`BUILD PASS`**（两配置） | 基线 / 启用均 `SUCCESS`；ELF homie 符号 **2 → 21** 证明宏生效 |

**三类状态必须分开读**：文档 ✅ ≠ 生产生效 ✅ ≠ 真机验收 ✅。本报告只在前两格给 ✅。

---

## 1. 与任务书不一致之处（先声明，不猜）

| 任务书表述 | 仓库实际 | 处置 |
|---|---|---|
| 「S-4：修复 `G4-R7` 对应**固件**问题」，且「不烧录、不重启设备」 | `last_rewind_at` / `last_rewind_boot` 是**云端 D1 表** `log_boot_watermark` 的列（`migrations/0005` 第 53–54 行），固件侧 `grep -rn "last_rewind" src/` **零命中** | 按仓库实际在**云端仓**修复；在 `S4-NOT-DEPLOYED` 中显式留缺 |
| 任务书假定 S-4 属于固件批次 | 同上 ⇒ 固件仓本轮只有 S-5 一行代码改动 | 已在上表与问题册标注 |

> 其它项与仓库实际一致，无额外偏差。

---

## 2. S-1：`seq` 统一为跨 boot 连续

### 2.1 修改前后语义

| 位置 | 修改前 | 修改后 |
|---|---|---|
| `docs/interfaces/log_envelope_contract.md:58` | `seq` = 「boot 内日志序号」 | `seq` = 「**跨 boot 连续**日志序号」，指向新增 **§2.0** |
| 同上（新增 §2.0） | — | 唯一权威定义：持久化 `s_meta_slot_seq + 1` · 重启不回绕 · DEBUG 消耗 `seq` ⇒ 空洞非丢失 · 统计须按 `boot_seq` 分组 |
| `G4-Real-Hardware-Acceptance-Checklist.md:44` | 「`seq` 也不是 boot 内序号」（否定式） | 「`seq` 是**跨 boot 连续**序号（**不是** boot 内序号），权威定义见 §2.0」 |
| `P3-DT-WP4-History-Ingestion-Design.md:195` | 「boot 内日志序号」 | 「**跨 boot 连续**日志序号 —— 权威定义见 §2.0」 |

### 2.2 冲突搜索结果（`docs/evidence/G4-S1S2-Docs-2026-10-21/consistency.txt`）

`grep -rn "boot 内日志序号|boot 内序号" docs/ --include=*.md` 剩余命中 **7 条**，逐条归类：

| 文件 | 性质 | 是否现行定义 |
|---|---|---|
| `G4-Log-Credibility-and-Observability-Report-2026-10-20.md:133`（B-6-4） | **历史评审记录**（当时指出文档与实现不符，判断正确） | ❌ 非定义 |
| `G4-Production-Change-Plan-Review-2026-10-20.md:332`（`SEQ-SEM`） | **历史评审记录** | ❌ 非定义 |
| `docs/issues/未修复的问题.md:1151`（`PLAN-5`） | 问题册陈述 | ❌ 非定义 |
| `G4-Scope-Decision-...2026-10-21.md:103` | 决策过程分析（"boot 内是子集"） | ❌ 非定义 |
| `G4-Post-Decision-Execution-Plan-...md:56` | 计划表描述"将改这里" | ❌ 非定义 |
| `G4-Real-Hardware-Acceptance-Checklist.md:45` | **本次新写**的否定式说明 | ✅ 与契约一致 |
| `log_envelope_contract.md:80` | **本次新写**的修订沿革 | ✅ 与契约一致 |

⇒ **现行权威定义零冲突**；历史记录**未改写**（仅 `log_envelope_contract.md` 新增沿革段说明"之前写错了"）。

### 2.3 未改实现的证明

`git diff --stat src/log/log_manager.cpp` ⇒ **空**（本轮固件仓代码改动只有 `src/app/loop_stats.h` 一处注释）。
★ **未执行任何固件回归测试**（无代码改动，不虚报）。

---

## 3. S-2：DRAM 阈值 85% 与口径统一

### 3.1 `R-3` 固定判据（唯一权威）

```
DRAM occupancy % = (internal.total − minimum_free) / internal.total × 100
判定：严格 < 85% ⇒ PASS ；≥ 85% ⇒ FAIL
```

| 要素 | 规定 |
|---|---|
| 分母 | **只有** `internal.total`（`ESP.getHeapSize()`，本设备 250 428–251 172 B）；**禁止** `327 680` |
| `minimum_free` | **本次 boot 内**最低可用 internal heap；重启重置 |
| 语义 | 单次 boot 的**峰值占用**；跨 boot 不可比 |
| 数据无效 | `total` 缺失/为 0 · `minimum_free` 缺失/大于 `total` · 窗口不足（2 h + 30 min）· 配置非生产形态 ⇒ **`NOT VERIFIED`**，不得记 `PASS` |
| 备选 | `90%` 由用户预留但**本记录未启用** |

### 3.2 同步落点

| 文件 | 改动 |
|---|---|
| `G-4-Gate-Revision-Record-2026-10-20.md` | 新增 **§5 `R-3`**（批准来源/效力/公式/边界/历史保留/余量/不做什么）；文件头加"已取代 §1 的 70%"横幅；§2.1 代码块加日期补注 |
| `docs/review/BT-1-...Fix-Plan.md` §6.2 | 「内部 DRAM 峰值占用 **< 70%**」→「**< 85%**」+ 指向 `R-3` §5.2 |
| 同上 `BT1-T10` | 「阈值待人工批准」→「DRAM 项已批准 `< 85%`；其余仍未批准」 |
| `G4-Real-Hardware-Acceptance-Checklist.md` | 新增 **`6-8`**（公式 + 严格边界 + 禁止分母 + `NOT VERIFIED` 规则 + 四项独立性说明） |
| `G4-R4-Gate-Revision-Draft-2026-10-20.md` | 加"已被 `R-3` 取代（**仅阈值**）；§1.2/§1.3 缺陷分析仍有效"状态横幅 |

### 3.3 历史不追溯（逐条核对）

| 测量 | 原判定 | 现状 |
|---|---|---|
| `82.8%`（方案 A 35 条突发） | `FAIL` | **原文保留** |
| `77.08%`（boot 22 空载） | `FAIL` | **原文保留** |
| `77.16%`（boot 22 压力后） | `FAIL` | **原文保留** |

只新增一条**带日期**的补注：代入 85% 后 `77.08/77.16` 属 `PASS`、`82.8` 仍 `FAIL` —— 明确标注"这是口径换算说明，**不是重新判定**，**不得**用于宣称 `G-4` 通过"。

### 3.4 未弱化任何其它门槛

`G-4` 的 `loop()` P99（2 h + 30 min）、连续块、栈高水位、`WP-5` 前置、`STOP-R1` **一字未动**。
`R-3` §5.6 显式写明"不解除 `G-4` / `WP-5` / `STOP-R1`"。

---

## 4. S-3：入站 S2 校验器与 `observe` 部署

### 4.1 校验规则（实现即契约）

```
合法 ⇔ topic 是字符串且非空
      ∧ 形态 ∈ { guo_feeder/<dev>/<dir> , $tenants/<tenant>/guo_feeder/<dev>/<dir> }
      ∧ <dir> ∈ { up, down, log }          （与 device-registry.js::TOPIC_DIRECTIONS 同源）
      ∧ <dev> 非空                          （★ 不校验 12-hex，保护 P-2）
      ∧ payload 非空（非 ""/null/[]/{}）
```

**三条刻意的不做**（写进源码文件头）：
1. 不判断来源可信（通过 ≠ 可信）；
2. 不校验设备段格式 —— `P-2` 的既定行为是"族已声明但设备段非法 ⇒ 写 `unresolved` 哨兵"，此处拒绝会破坏它；
3. 不做 payload 语义校验 —— 缺 `msg_id` 合法（`dedup_key IS NULL` 是设计的一部分）。

### 4.2 `observe` 行为保证

| 要求 | 实现 |
|---|---|
| 失败不拒绝、不改变写入与响应 | 仅 `noteValidationFailure()`；写入路径**一行未改** |
| 通过分支语义不变 | 校验是**纯前置 if**；`resolveIngestDeviceId` / `dedup_key` / 退役守卫 / 响应码全部原样 |
| 无鉴权写入计数 | `noteUnauthenticatedWrite()`：请求**无** `X-Webhook-Key` 却进入写入路径时 +1，**不改鉴权结果** |
| 日志不含敏感信息 | 只打 `reason` / 计数 / 主题长度；**不打** payload、clientid、请求头 |
| 防洪 | 同一 reason 每 60 s ≤1 行 + 每 100 次 1 行 + 全 isolate 硬上限 200 行 |
| 指标口径披露 | `ingestMetricsSnapshot()` 返回 `scope:"isolate"` + `persistent:false` |
| 默认值 | `DEFAULT_INGEST_VALIDATION="observe"`；`resolveValidationMode()` 只有 `raw.trim() === "enforce"` 才强制，**不做 `toLowerCase()`**（避免 `"Enforce"` 意外激活） |

### 4.3 本地测试（25 套件 + 端到端）

| 层次 | 结果 | 证据 |
|---|---|---|
| 新增 S-3 套件 | **71 / 71 PASS** | `G4-S3-Ingest-2026-10-21/tests-all-suites.txt` |
| 全量 `node test/run_all.mjs` | **25 套件全绿，`exit=0`** | 同上（末行「所有套件通过」） |
| 本地 `wrangler dev --local`（真实 workerd + 本地 D1） | **15 / 15 用例 0 失配** | `local-e2e-run.txt` |
| 本地 D1 落库核对 | 见下表 | `local-d1-rows.txt` |

**本地 D1 实测（不是 mock）**：合法 plain → 归属正确；`$tenants/*/down` → `device_id` 正确、`dir=down`；
`$tenants/*/log` → `dir=log`；空 topic / 空 payload / `/upx` / `$SYS/...` **全部照旧写入**；
同 `msg_id` + 同 payload **发 3 次只留 1 行**；缺 `msg_id` 发 2 次留 **2 行且 `dedup_key` 均 NULL**；
`cloud_api` + `BADID` → **400**；`/api/ingest` 无 key → **401**。

**防洪实测**：54 次请求（含 40 条恶意空 topic）只产生 **3 条 `[validate]` + 3 条 `[unresolved]`**（若逐条打应为 ≈88 条）。
另修掉一个**既存**洪水点：`device_id` 归属失败原为每请求一行 `console.warn`（`S3-FLOOD-LEGACY`）。

### 4.4 生产部署（`observe`）

| 项 | 值 |
|---|---|
| 命令 | `wrangler d1 migrations apply` **未执行**（无 migration）· `wrangler deploy` |
| 时间 | `03:15:54` → `03:16:08`（+0800），`deploy_exit=0` |
| 制品 | `Total Upload: 163.36 KiB / gzip 41.59 KiB` · `Startup Time: 2 ms` |
| 部署前版本 | `d7fcd4f3-c9f3-4a7c-ba2f-c7e3bce74016`（100%，C-2） |
| **部署后版本** | **`1fb56499-49fb-4e07-b2c6-6fd35cf60329`（100%）** · deployment `Created: 2026-10-20T19:16:03.867Z` |
| 回滚目标 | **version** `d7fcd4f3-c9f3-4a7c-ba2f-c7e3bce74016` |
| env 实证 | `versions view` ⇒ `env.INGEST_VALIDATION ("observe")` · `env.LOG_BOOT_WATERMARK ("1")` · `env.guofeeder_DB (0d8385ba-…)` |
| D1 写入 | `changed_db=false` / `rows_written=0`（部署本身不写库） |

**★ 未切换 `enforce`**：`wrangler.toml` 与源码默认均为 `observe`，且版本元数据实证生效值为 `observe`。

### 4.5 `observe` 指标现状（如实）

| 项 | 事实 |
|---|---|
| 作用域 | per-isolate（module 作用域） |
| 持久化 | **无**（不落 D1/KV） |
| 重启 | 归零 |
| 可观测出口 | 仅 Workers Logs（`[observability.logs] persist=true`）；`tail` 可用性仍未验证（`C2-OBS`） |
| 生产读数 | **`NOT MEASURABLE`** —— 部署后无真实流量（最近一行 `02:52`，部署 `03:16`） |

### 4.6 顺带观察（**非本任务交付项**）

只读查询发现设备曾恢复上行（`2026-10-20 18:51–18:52 UTC`）：`$tenants/n302933b/guo_feeder/288485896ce4/down`
共 **5 行**，`device_id = 288485896ce4`（**不再是 `http_api`**）、`direction=down`、`dedup_key` 有值。
⇒ 为 C-2 的 `P-1'` **真实流量**版本提供了直接证据。
⚠️ 本报告**不**据此宣告 C-2 通过（属 C-2 范围，需人工认定）；⚠️ 这 5 行写于 S-3 部署**之前**，**不能**用于证明 observe 指标。

---

## 5. S-4：`G4-R7` 修复（云端仓）

| 项 | 内容 |
|---|---|
| 缺陷 | rebase 后 `rewind_count=0` 但 `last_rewind_at` 仍为旧值 ⇒ 自相矛盾行，会被误读为"刚发生过回退" |
| 修复 | `LOG_REBASE_UPDATE` **同一条** UPDATE 内新增 `last_rewind_at = NULL, last_rewind_boot = NULL` |
| 为什么同语句 | 单语句 = 单事务 ⇒ 不可能出现"只清一半"的中间态 |
| 测试 | 新增 `R7-1…R7-12`：SQL 文本含两字段 · 只有一条 UPDATE · rebase 后 `rewind_count=0` 且两时间戳均 NULL · **三字段一致性**（不只验一个）· CAS 未命中不得清空 · 审计保留 `rewind_count_before` |
| 结果 | 套件 **97 / 97 PASS** |
| ★ 未部署 | 生产 `1fb56499` **不含**此修复（`S4-NOT-DEPLOYED`，MAJOR） |
| 已知限制 | 审计表无 `last_rewind_*_before` 列 ⇒ rebase 丢弃旧时间戳（`G4-R7-LIMIT`，需 migration 授权） |

---

## 6. S-5 / S-6

### 6.1 S-5 注释

| 项 | 核实结果 |
|---|---|
| 原注释 | `loop_stats.h:22`：「读路径预算 ≤ **10 µs**」 |
| 权威值 | `R3`（`2026-10-20 03:05`）：预算 **10 → 15 µs**；目标板实测临界区 **12 µs** |
| 处置 | 改为 **15 µs**，并写明"12 µs 只能写'在 15 µs 预算内'，**不得**写'达标/PASS'"，附 R3 依据路径 |
| 顺带核实 | 「576 B」经 `clang++ -target wasm32` + `static_assert` 实测 `sizeof(LoopStatsSnapshot)==576` ⇒ **原表述正确，未改** |
| 统计行为 | **零改动**（只改注释） |
| 编译/静态检查 | `test/loop_stats_host/run.sh` ⇒ **82 / 82 PASS** |

### 6.2 S-6 Homie 构建矩阵

| 配置 | 结果 | RAM | Flash | firmware.bin | 耗时 |
|---|---|---|---|---|---|
| 基线（默认 `0`） | ✅ `BUILD PASS` | 134 720 B（41.1%） | 1 416 605 B（67.5%） | 1 416 976 B | 130.68 s |
| `-DGF_HOMIE_ENABLED=1` | ✅ `BUILD PASS` | 135 624 B（41.4%） | 1 427 757 B（68.1%） | 1 428 128 B | 222.78 s |
| **增量** | — | **+904 B（+0.67%）** | **+11 152 B（+0.79%）** | +11 152 B | — |

**宏真生效的三重证据**：① 编译命令行命中 `-DGF_HOMIE_ENABLED=1` **386 次**；
② **ELF homie 符号 2 → 21**（新增 `homie_desc_build` / `homie_publish_state` / `homie_parse_set_topic` …）；
③ 资源增量非零。⇒ 不是"宏被忽略所以两者相同"。

**环境**：PlatformIO **6.2.0** · `espressif32@7.0.1`（锁定）· 依赖全部精确版本（无 `@latest`）。
**只标 `BUILD PASS`**，❌ **不**是 Homie 功能 PASS（**未烧录、未上板**）。

---

## 7. 提交与远端一致性

| 仓 | 分支 | commit | 内容 | push |
|---|---|---|---|---|
| **云端** | `main` | `b061a1c` | S-3 校验器 + 测试 + toml（5 文件，+853/−1） | `e1e8b79..7c96b18` |
| 同上 | `main` | `7c96b18` | S-4 `G4-R7`（2 文件，+98） | 同上 |
| **固件** | `wb` → `main` | `670ed16` | S-5 注释（1 文件，+6/−1） | `9153871..e4713b3` |
| 同上 | | `834404c` | S-1/S-2 文档 + 问题册 + HANDOFF/README（11 文件） | 同上 |
| 同上 | | `e4713b3` | 4 个证据目录（27 文件） | 同上 |

**远端核验**：云端 `local == origin/main == 7c96b1894cdd…`；固件 `local == origin/main == e4713b30aa94…`（`git fetch` 后比对 `git ls-remote`）。
**排除**：无固件源码业务改动（除 S-5 注释）、无 EMQX 仓改动、无 `.pio` 构建产物（gitignored）、无临时脚本、**无密钥**（证据目录已 `grep` 扫描 `gho_/ghp_/Bearer/password=/secret=/BEGIN`：0 命中）。

---

## 8. 未完成 / 风险 / 待人工决定

| ID | 级别 | 内容 | 建议 |
|---|---|---|---|
| **`S4-NOT-DEPLOYED`** | **MAJOR** | `G4-R7` 修复在云端仓但**未部署**；生产 `1fb56499` 不含它 | 与「切 `enforce`」合并成**一次**部署，减少生产变更次数 |
| **`S3-ENFORCE-PENDING`** | **MAJOR** | 校验器只跑 `observe` ⇒ 非法报文**照旧写入** | 前置：真实流量下 `invalid_total=0` + **单独授权**；否则"公网写不进非法数据"**不成立** |
| **`S3-OBS-ISOLATE`** | MINOR | 计数器 per-isolate、不落库、重启归零；生产当前 `NOT MEASURABLE` | 要持久审计需新端点/表（未授权） |
| **`G4-R7-LIMIT`** | MINOR | rebase 丢弃旧 `last_rewind_*` 时间戳 | 需 migration 加 `*_before` 列（未授权） |
| **`C2-SEC-WEBHOOK-NOAUTH`** | **MAJOR（既存）** | 公网无鉴权仍可写库；本轮只加了**计数**与**非法格式拦截（未启用）** | 若要真封，需 Pages/Worker 鉴权协同（依赖 EMQX 调用方式，API 403） |
| `S6-BUILD-DIR` | MINOR | 改 flags 撞沙箱批量删除保护 | 已用独立 `PLATFORMIO_BUILD_DIR` 规避 |
| 真机独占 | — | `G-4`（P99 2 h + 30 min、DRAM 85% 复测、连续块、栈）· `WP-5` · Homie 上板 · `C-3` | 必须上板 |

---

## 9. 明确未执行的操作

- ❌ **未烧录**、❌ **未重启设备**、❌ **未修改设备配置**
- ❌ **未执行 C-3**
- ❌ **未切换 `INGEST_VALIDATION="enforce"`**（生产实证 `observe`）
- ❌ **未改 EMQX**（Rule / Action / Connector / 账号 / ACL 全未动）
- ❌ **未改已应用的 D1 migration**、❌ **未手工改 `d1_migrations`**、❌ **未用 `--file` 执行 SQL**
- ❌ **未清理历史生产数据**、❌ **未删除既有证据**
- ❌ **未把本地合成样本当作真实生产流量**（§4.6 的真实流量已单独标注来源与边界）

---

## 10. 门禁最终状态

| 门禁 | 状态 | 依据 |
|---|---|---|
| **`BT-1`** | **`CLOSED`（未变）** | 本轮无涉及 |
| **`G-4`** | **`BLOCKED`（未变）** | DRAM 阈值虽已裁决为 85%，但 `loop()` P99 仍 `NOT VERIFIED`；连续块 / 栈高水位未测；2 h + 30 min 窗口未跑 |
| **`WP-5`** | **未解锁（未变）** | 前置是 `G-4` |
| **`STOP-R1`** | **无解除文件（未变）** | 本轮无解除动作 |

**本轮真正关闭的两项**（问题册级，不是门禁）：`SEQ-SEM`（S-1）· `G4-R4`（S-2）。
★ `G4-R4` 关闭 **只表示"阈值争议已裁决"**，**不表示** `G4` 门禁通过。

---

## 11. 证据路径总表

| 目录 | 文件数 | 内容 |
|---|---|---|
| `docs/evidence/G4-Ingest-Hardening-2026-10-21/` | 6 | 影子校验（5754 行 → 只拒 9 行）+ 形态分布 + 9 行身份 |
| `docs/evidence/G4-S1S2-Docs-2026-10-21/` | 1 | 文档一致性 grep（冲突 / 85% 落点 / 分母口径） |
| `docs/evidence/G4-S3-Ingest-2026-10-21/` | 12 | 25 套件 · 端到端 15 用例 · 本地 D1 逐行 · dev 控制台 · 部署前后版本 · 生产只读快照 |
| `docs/evidence/G4-S6-Homie-Build-2026-10-21/` | 6 | 两配置构建日志 · 宏生效证据 · ELF 符号对比（2→21） |
