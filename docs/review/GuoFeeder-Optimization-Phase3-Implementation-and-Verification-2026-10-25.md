# Guo Feeder Optimization Rev.2 — 阶段 3 实施、部署与效果验证

- 日期：2026-10-25
- 范围：P-1 水位写入节流 · P-2 `max_seq_at_max` 修复 · P-3 采样口径 · P-4 W=600 · P-5 生产部署 · P-6 ACK topic 过期断言 · P-7 生产水位停滞处置
- 授权：用户已明确批准 P-1 ~ P-7 全部建议（见任务书 §一）
- 结论：**全部 7 项完成**，其中 **P-7 经核实不需要执行 rebase**（新库无停滞）

---

## 一、结果总览

| 项 | 状态 | 说明 |
|---|---|---|
| P-1 水位写入节流 | ✅ 已实施并上线 | `shouldWriteWatermark()` 闸门，W=600s |
| P-2 `max_seq_at_max` 修复 | ✅ 已实施并上线 | 绑定 `nextSeq` → `maxSeqAtMax` |
| P-3 接受采样口径 | ✅ 已确认 | `obs_count` / `ambiguous_count` / `max_seq_at_max` |
| P-4 W = 600 秒 | ✅ | `WM_WRITE_MIN_INTERVAL_SEC = 600` |
| P-5 生产部署 | ✅ | Worker `c6720d7f` @100%，2026-10-24T18:12:25Z |
| P-6 ACK topic 过期断言 | ✅ | 2 条 `/down` → `/log/ack`，另加 3 条防护断言 |
| P-7 生产水位停滞处置 | ✅ 调查完成，**不执行 rebase** | 新库水位健康，停滞状态未随历史数据迁移 |

**未部署**：Pages（本阶段无前端改动）、固件（约束禁止）。

---

## 二、实际代码改动

### 2.1 产品代码（2 个文件）

| 文件 | 改动 | 性质 |
|---|---|---|
| `guo-feeder-api/src/log-boot-watermark.js` | P-1 节流闸门 + P-2 绑定修复 | 行为变更（写入频率） |
| `guo-feeder-api/src/log-ack.js` | 协议注释修正（`/down` → 主通道 `/log/ack`） | **纯注释，零行为** |

#### P-1 节流闸门（`shouldWriteWatermark()`）

改造前 `recordBootWatermark()` **每批固定** 1×SELECT + 1×batch，与水位是否变化无关。

新增纯函数闸门，**任一成立立即写**：

| # | 条件 | 理由 |
|---|---|---|
| ① | 初始化（无既有行 / `max=0`） | 必须建行 |
| ② | `advance` | 判定水位推进 |
| ③ | `rewind > 0` 或 `rewind_only` | 回退证据不得丢 |
| ④ | `observed_max_boot_seq` 前沿推进 | rebase 的 `confirm <= observed_max_boot_seq` 依赖此列 |
| ⑤ | 距 `last_seen_at` ≥ 600s | rebase 前置条件「`last_seen_at` 在 30 天内」 |

关键设计点：

- **`last_seen_at` 由同一条 SELECT 带出**，不新增查询；**不用 isolate 内存缓存**代替读库（多 isolate 下会失准）——严格遵守任务书 §三。
- **逐设备独立决策**；全部被跳过时**不执行空 batch**（省一次 D1 往返）。
- **异常时钟的安全方向一律「写」**：`last_seen_at` 缺失 / 非数字 / 晚于当前时间 / `nowSec` 非数字 / 分析结果缺失 ⇒ 宁可多写，不可漏掉必要写入（5 个分支，全部有测试）。
- **判定语义零改动**：`nextMax` / `clearPrev` / `rewind` 原样保留，分类结果与修改前完全一致。

#### P-2 `max_seq_at_max` 绑定

epoch upsert 的 CASE then / CASE max 两处原绑定 `a.nextSeq`，而
`nextSeq = (advBoot > prevMax) ? advSeq : 0` ⇒ **普通 `same` 批恒传 0** ⇒
`MAX(max_seq_at_max, 0)` 永不推进，该列自迁移 `0006` 起冻结。

改为绑定 `a.maxSeqAtMax`（本批真实最大 seq），恢复 legacy 语义。

> ⚠️ 只影响这一列的**诊断值**；`nextMax`/`clearPrev`/`rewind` 均未动，判定水位与回退分类完全不变。
> **历史值不回填**——修复后只保证后续新写入恢复推进。

#### P-3 采样口径（已批准）

`obs_count` / `ambiguous_count` / `max_seq_at_max` 由「精确累计」变为「节流采样」。
三者**均不参与任何判定**（已核验：`ambiguous_count` 在 rebase 中只用于写审计的
`ambiguous_count_reset_from`，不是判定输入）。

### 2.2 测试（6 个文件）

| 文件 | 改动 |
|---|---|
| `test/current/log_boot_watermark.test.mjs` | 新增 D 段（闸门纯函数 18 条）+ E 段（落库集成 21 条） |
| `test/current/log_ack.test.mjs` | P-6：2 条过期 `/down` → `/log/ack`，新增 3 条防护断言 |
| `test/current/cloud_api_downlink_topic.test.mjs` | 附带修复（见 §七） |
| `test/real_d1/log_boot_watermark_real_d1.sh` | W-5/W-8 到期窗口；新增 W-10 真实 D1 节流验证 10 条 |
| `test/real_d1/watermark_rebase_real_d1.sh` | R-8d 到期窗口 + 采样口径说明 |
| `test/real_d1/log_conflict_real_d1.sh` | 附带修复（见 §七） |

**零 DDL、零迁移、零配置变更、零固件改动。**

---

## 三、测试结果

### 3.1 与 HEAD 基线的逐套件对照

基线 = `5d37651`（阶段 3 改动前的 HEAD），用 `git worktree` 独立检出后跑同一批脚本。

| 套件 | HEAD 基线 | 阶段 3 后 | 判定 |
|---|---|---|---|
| `log_boot_watermark.test.mjs` | 48 / 0 | **87 / 0** | +39 新断言，全绿 |
| `log_ack.test.mjs` | 60 / 2（2 条 `/down`） | **63 / 0** | P-6 修复 |
| `log_boot_watermark_real_d1.sh` | 43 / **2**（W-3c、W-5b） | **55 / 0** | P-2 修复 + W-10 新增 |
| `watermark_rebase_real_d1.sh` | 57 / 0 | **57 / 0** | 持平（R-8d 因采样口径改写后仍绿） |
| `log_conflict_real_d1.sh` | 57 / 2 | **60 / 0** | 附带修复 |
| `history_ingest_real_d1.sh` | 124 / 0 | **124 / 0** | 持平 |
| `cloud_api_downlink_topic.test.mjs` | 19 / **17** | **36 / 0** | 附带修复 |
| Worker 全量 `run_all.mjs` | 1707 总 / 1677 PASS / **21 FAIL** / 9 XFAIL | **1749 总 / 1738 PASS / 2 FAIL** / 9 XFAIL | 净 +42 断言，**FAIL −19** |
| 根套件 `tests/api` + `tests/pages` | 922 / 0 | **922 / 0** | 持平 |

### 3.2 真实 D1 上的节流验证（W-10，新增 10 条）

在真实 SQLite 引擎上验证（不是 mock）：

| 断言 | 结果 |
|---|---|
| W-10a 窗口到期 ⇒ `obs_count` 递增（写入发生） | PASS |
| W-10b 窗口未到 + 无状态变化 ⇒ `obs_count` **不变**（被节流） | PASS |
| W-10c `last_seen_at` **未**被刷新（证明没写库） | PASS |
| W-10d 摘要记录 `throttled=1` | PASS |
| W-10e 窗口再次到期 ⇒ 恢复写入 | PASS |
| W-10f/g `advance` ⇒ 立即写，`max_boot_seq` 推进到 99 | PASS |
| W-10h/i/j `rewind` ⇒ 立即写，`rewind_count` 递增，水位不降 | PASS |

### 3.3 历史失败 vs 新增失败

- **历史失败（与本次无关）**：`retention_policy`（1）、`s3_ingest_validate`（1）—— 详见 §七。
- **新增失败**：0。
- 过程中曾出现 2 次**环境性假红**（`log_boot_watermark` 48 FAIL、`history_ingest` 55 FAIL），
  根因是并行跑多个 `real_d1` 脚本导致本地 `d1 migrations apply` / Worker 端口争用。
  串行重跑后分别为 55/0 与 124/0。**不是产品缺陷。**

---

## 四、P-7：生产水位停滞 —— 核实结论：不执行 rebase

任务书明确：「此记录来自旧库，不得假设新库具有完全相同的当前状态」。实测确认该提醒是对的。

### 4.1 新库（`guofeeder-v2`）当前水位（部署前只读取）

```
device_id             = 288485896ce4
epoch                 = 1
max_boot_seq          = 98
max_seq_at_max        = 50967
prev_epoch_max        = null      ← 无歧义带上界
observed_max_boot_seq = 98
ambiguous_count       = 0         ← 不是旧库的 98.1%
obs_count             = 13
rewind_count          = 0
status                = open
```

设备实际日志：`log_history` 全部 `boot_seq = 98`（11 → 13 条），`seq` 50967→50977。

⇒ **水位 `max_boot_seq=98` 与设备实际 `boot=98` 完全一致，不存在停滞。**

### 4.2 停滞状态为何没带过来

| | 旧库 `guofeeder` | 新库 `guofeeder-v2` |
|---|---|---|
| `log_boot_watermark_audit` | **1 行** `rb-20261019-0001` | **0 行**（从未 rebase） |
| epoch / max / prev_epoch_max | 2 / 21 / **117** | 1 / 98 / **null** |
| `ambiguous_count` | 27,310（98.1%） | 0 |

旧库那条审计：`epoch 1→2`、`117→21`、`prev_epoch_max_after=117`、
`reason=full_erase_confirmed`、`actor=admin#f8122859`、`result=applied`（2026-10-19）。

⇒ 旧库「设备 boot 要越过 117 才推进」的停滞带，是那次**合法且已审计**的 rebase 造成的；
而 rebase 记录与历史日志**均未迁入新库**（阶段 D1 切库时明确不迁历史）。
新库从空表开始，首次观测即 `boot=98` ⇒ 记为 `init/advance`，**不存在歧义带**。

### 4.3 处置决定

**不执行 rebase**，理由：

1. 前置条件不成立 —— rebase 解决的是「水位卡在旧 epoch 上界之下」；新库 `prev_epoch_max=null`，无上界。
2. 无证据支持 —— 新库 `ambiguous_count=0`，没有任何歧义样本。
3. 任务书 §六.7 明确：前置条件不满足或无法判断时，**停止写操作**。此处正是该情形。
4. 强行 rebase 反而会**制造**一个本不存在的歧义带（`prev_epoch_max` 从 `null` 变成某个值），是净损害。

**后续观察点**：若将来设备 `boot_seq` 回退（例如重刷固件后从 0 开始），水位会记 `rewind_count`
并保留 `max_boot_seq=98`，那时才需要评估。当前不需要任何动作。

---

## 五、部署前后对比

### 5.1 部署信息

| 项 | 值 |
|---|---|
| 部署前生效版本 | `66d45848-5617-4853-acf8-8dc2f080ac8f`（2026-10-24T16:05:48Z） |
| **部署后生效版本** | **`c6720d7f-ce02-491f-acf8-8dc2f080ac8f`** @100%（2026-10-24T18:12:25Z） |
| **回滚目标** | `66d45848-5617-4853-acf8-8dc2f080ac8f` |
| D1 绑定（部署前核对） | `env.guofeeder_DB` → `632b1224-7f71-43e9-9575-4e82ee26f36d`（`guofeeder-v2`）✅ |
| 部署方式 | `versions upload`（不接管流量）→ 核对绑定与 vars → `versions deploy @100%` |
| Pages | **未部署**（本阶段无前端改动） |
| 固件 | **未改动**（约束禁止） |

部署前核对的三项生产配置（新版本内）：
`LOG_BOOT_WATERMARK="1"` · `LOG_ACK_DUAL_PUBLISH="0"` · `INGEST_VALIDATION="observe"` —— **全部未变**。

### 5.2 节流效果（实测）

### 5.2.1 可复现的核心指标：`obs_count` / 日志批次数

`obs_count` **只在发生水位写入时累加**，而 `log_history` 每批都会增加 ⇒
两者之比可以直接反映「多少批次真的落了水位」。

| 时点 | `obs_count` | `log_history` 条数 | 比值 |
|---|---|---|---|
| **部署前**（18:11Z 实测） | 13 | 13 | **1.000**（每批必写） |

部署后该比值的下降幅度 = 节流实际生效幅度。

### 5.2.2 观察窗口与实测结果

| 窗口 | 时长 | 新增日志批次 | 新增水位写入 | 结论 |
|---|---|---|---|---|
| 18:13Z – 18:29Z | 16 min（> 1 个 600s 周期） | **0** | 0 | 无流量，无法测 |
| 18:30Z – 18:52Z | 21 min | **0** | 0 | 无流量，无法测 |
| **合计** | **37 min（跨越 3 个 600s 周期）** | **0** | **0** | — |

**设备本身在线**（遥测持续写入：`telemetry_history` 101 → 111 条，最新一条距今 113 秒），
但 `log_history` 自 18:11Z 起**没有新增**——设备处于日志安静期
（历史观测：相邻日志间隔最长 3,356 秒 ≈ 56 分钟）。

⇒ **无法从本次生产短窗口得出可靠的绝对削减量**。按任务书 §九.7，此处如实说明，不伪造效果。

### 5.2.3 已有的实测证据（非推算）

| 证据 | 来源 | 性质 |
|---|---|---|
| 部署前 `obs/logs = 1.000`（每批必写） | 生产 D1 只读查询 | **实测** |
| 节流后：窗口未到 ⇒ `obs_count` 不变、`last_seen_at` 不刷新 | 真实 D1 引擎 W-10b/c | **实测** |
| 节流后：窗口到期 ⇒ 恢复写入 | 真实 D1 引擎 W-10a/e | **实测** |
| 节流后：`advance` / `rewind` 立即写，不等窗口 | 真实 D1 引擎 W-10f/g/h/i/j | **实测** |
| 生产运行的是新版本 `c6720d7f`，D1 绑定 `632b1224…` | Cloudflare `deployments status` / `versions view` | **实测** |
| 单元测试覆盖 5 个必写条件 + 5 个异常时钟分支 | D1–D17、E1–E10 | **实测** |

### 5.2.4 推算（**非实测**，基于旧库历史）

改造前每批固定 1×SELECT + 1×batch。旧库 2026-10-23 官方逐 SQL 统计：

- 水位 upsert **10,714 次 / 32,142 行写 = 当日全部写入的 35.9%**（10-24 为 42.5%）
- 累计 27,854 次观测中真正推进判定水位的 **≤ 21 次（0.075%）**

按此推算，节流后写入次数 ≈ 「advance 次数 + rewind 次数 + 前沿推进次数 + 每 600s 一次心跳」。

⚠️ **新库当前日志流量仅约 100 条/日**，按 600s 窗口理论上最多省 144 次写入/日——
绝对量远小于旧库。真正的量级收益只在日志风暴场景才体现。

### 5.2.5 建议你自行复核的命令

```bash
# 观察一段时间（建议 ≥ 30 分钟，最好跨越一次设备日志突发）
# 对比 log_history 增量 与 水位 obs_count 增量：
wrangler d1 execute guofeeder-v2 --remote --config guo-feeder-api/wrangler.toml --json \
  --command "SELECT (SELECT COUNT(*) FROM log_history WHERE device_id='288485896ce4') AS logs,
                    (SELECT obs_count FROM log_boot_watermark WHERE device_id='288485896ce4') AS obs,
                    (SELECT last_seen_at FROM log_boot_watermark WHERE device_id='288485896ce4') AS ls,
                    unixepoch() AS now"

# 实时看水位摘要（新代码新增 writes= / throttled= 两个字段，旧代码没有）
wrangler tail --config guo-feeder-api/wrangler.toml --format json | grep log-bootwm
```

`writes=` / `throttled=` 是本次新增的可观测计数器——**看到 `throttled>0` 即证明节流在生产生效**。

---

## 六、可靠性边界核验：ACK / 重放 / 回收未被削弱

| 不变量 | 验证方式 | 结果 |
|---|---|---|
| 只在持久化成功或确认记录已存在后发 ACK | `log_ack.test.mjs` 63 条 + `log_ack_publish_strict` 76 条 | 全绿 |
| `rejected` / `filtered` 不被确认 | `rows[]` 构造不含二者 | 全绿 |
| ACK 发布失败不回滚已完成的 D1 持久化 | `log_ack_edge` 48 条 | 全绿 |
| `log_conflict` / rewind / 去重 / 重试保留 | `log_conflict` 60 条（真实 D1）、`log_conflict_retry` 38 条 | 全绿 |
| `acked_seq` / `gc_seq` / hole 处理未改 | **未触碰任何设备端代码** | — |
| 迁移 0001–0009 未改、无新迁移 | `git diff --stat` 仅 8 个文件，无 `.sql` | ✅ |
| EMQX 规则 / ACL / 认证未改 | 未执行任何 EMQX 写操作 | ✅ |

**本阶段唯一的产品行为变化是「水位表的写入频率」**，且：
- 不改变任何判定结果（有 W-10f/g/h/i/j 与 D1–D17 覆盖）
- 不改变 ACK 时机（ACK 由 `history-ingest.js` 的 persist 结果驱动，与水位写入解耦）

---

## 七、补充调查：既有失败项是否需要修复

用户追加要求：调查**不属于本次任务**的失败项，判断是否需要修复。

### 7.1 已顺手修复（成本低、恢复真实覆盖）

| 套件 | 失败 | 根因 | 处理 | 结果 |
|---|---|---|---|---|
| `cloud_api_downlink_topic.test.mjs` | 17 条 | ① 管理端点统一要求 `Authorization: Bearer <token>`，而该测试的 `post()` **从未带该头** ⇒ 16 条期望 200 的用例全拿 401<br>② H2 正则要求 import 列表**只有** `publishToDevice`，实际是 `{ publishToDevice, replaceUserAclRules }` ⇒ 恒不匹配 | ① 加 `Authorization` 头<br>② 放宽为「引入了 publishToDevice」 | **19/17 → 36/0** |
| `log_conflict_real_d1.sh` | 2 条 | 与上次已修的**同类过期断言**：`0009` 按设计新增索引（5→7）、`0000` 让空库也建 `mqtt_messages` | 改为守住真正的不变量（0003 的 5 个索引按名仍在 / 表存在但 0 行） | **57/2 → 60/0** |

判定依据：两者都是**测试自身过时**（产品行为正确、甚至是安全加固后的正确行为），
修复只会**增加**覆盖，不会掩盖任何产品问题。

### 7.2 建议不修复（问题不大，修复收益低于成本）

| 套件 | 失败 | 根因 | 为什么可以不修 |
|---|---|---|---|
| `retention_policy.test.mjs` | 1 条：`scheduled()` 体 ≤ 10 行（任务单 §5.1），实际 21 行 | `scheduled()` 的职责从 1 项（retention）增长到 3 项（+ `logBootSeqGuard`、+ `UI_AUDIT_PURGE`），三者都是「薄调用 + 开关判断」 | 该断言的**实质**意图已被同段另外 3 条断言覆盖且全绿：① 入口调用 retention 唯一实现 ✅ ② 入口无 D1 SQL ✅ ③ 入口无循环 ✅。行数阈值只是代理指标。要修得改设计约束或重构入口——重构无功能收益且有风险，改阈值等于放宽设计约束。**建议保持失败作为「职责已增长」的可见提醒**，或由你决定是否把阈值放宽到 25。 |
| `s3_ingest_validate.test.mjs` | 1 条：S3-10-1b「cloud_api 仍走原写入路径（`mqtt_messages` 有 INSERT）」 | 测试桩下 EMQX publish 报 `CONFIG_MISSING`，请求在到达 INSERT 前就返回 502 | 断言的**目标**（cloud_api 不受 S2 拦截）已由 S3-10-1/S3-10-2 覆盖且 PASS。此条只是额外验证写入路径，被 mock 桩限制挡住。**要修需改桩让 publish 成功**，属测试基建改动，与产品无关。 |

**结论**：4 项既有失败中，2 项已修（恢复 19 条覆盖），2 项建议不修（均有等价断言覆盖，属设计约束/测试基建，非产品缺陷）。

---

## 八、Git 状态

| 仓库 | commit | 说明 | push |
|---|---|---|---|
| `Cloudflare_Assets` | `30d2881` | 阶段 3 实施（8 文件，+547/−19） | 待你确认后推送 |
| `PIO_Assets` | 本报告提交 | 阶段 3 实施与验证报告 | 待你确认后推送 |

工作区剩余：`scripts/enforce_watch.sh`（未跟踪，**从未触碰**）。

---

## 九、数据库与安全

| 项 | 状态 |
|---|---|
| 旧库 `guofeeder`（`0d8385ba…`） | **零改动**。本阶段对它只执行过 `SELECT`（读取旧水位与审计记录用于 P-7 对照） |
| 新库 `guofeeder-v2`（`632b1224…`） | 表结构/迁移**零改动**；仅业务写入（设备自然流量） |
| 新增迁移 | **0** |
| 密钥 / Turnstile / 生产安全设置 | **零改动** |
| EMQX 规则 / Action / Connector / ACL / 认证 | **零改动** |

---

## 十、剩余风险与未验证项

1. **节流的绝对收益依赖日志流量**。新库当前日志流量很低（约 100 条/日），
   按 600s 窗口，理论上最多省 144 次写入/日。真正的量级收益（旧库实测 10,714 次/日）
   只有在日志风暴场景才体现。当前窗口的**比例**证据可靠，**绝对值**需更长观察。
2. **`max_seq_at_max` 历史值不回填**。修复只保证后续写入恢复推进；
   新库当前该列 50967 是 boot=98 的首批值，后续 same 批才会继续推进。
3. **采样口径的语义变化**：`obs_count` 不再等于「观测到的日志条数」。
   任何把它当精确计数的运维判断都需要改口径（建议改用 `log_history` 计数）。
4. **UI 浏览器实操仍未验证**：沙箱无法登录生产 UI（workers.dev 不可达 + Turnstile）。
   本阶段无前端改动，影响面为零；但阶段 2 的 UI 项仍需你人工过一遍。
5. **未做灰度**：按 P-5「优先保持最小改动」直接 100% 部署。
   回滚目标 `66d45848` 已记录，回滚 = `wrangler versions deploy 66d45848...@100%`。
6. **后续独立事项**：阶段 4（MQTT 客户端与安全优化）；UI 控制命令迁移到 MQTT（待你另行提出）。
