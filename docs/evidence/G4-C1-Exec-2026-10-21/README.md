# 证据目录 · G4 C-1（`0007` 生产 migration）执行

- 执行日期：`2026-10-21 01:21–01:25 +0800`
- 目标：D1 `guofeeder` = `0d8385ba-a1a6-4fcc-8e43-ae61c014c43d`（账号 `9e72d201…`）
- 命令：`wrangler d1 migrations apply guofeeder --remote`（wrangler `4.145.0`）
- 正式报告：`docs/architecture/G4-C1-Execution-Report-2026-10-21.md`

## 文件顺序

| 文件 | 阶段 | 内容 | 采集时间 |
|---|---|---|---|
| `c1_gate.sh` | 门禁 | 可复现脚本（只读） | — |
| `g0-gate.txt` | 门禁 | G-0…G-4 四源身份 / 制品三方哈希 / 登记 / S-PROBE | `01:22:45`–`01:23:09` |
| `g1-pre-A1A2.txt` | 基线 | 表结构原文 + 无 `dedup_key`（期望 0） | `01:23:0x` |
| `g1-pre-fingerprint.txt` | 基线 | `FINGERPRINT_BEFORE`（`n_old=5745` / `max_id=5763` / 三项长度和） | `01:2x` |
| `g1-pre-v2b-sample.txt` | 基线 | 25 行固定抽样原始（rn `0-4 / 1430-1434 / 2870-2874 / 4300-4304 / 5740-5744`） | `01:21:39` |
| `SAMPLE_IDS.txt` | 基线 | ★ 固定 id 列表（执行后必须按此重放，不重跑 OFFSET） | `01:22` |
| `g5-apply.txt` | 执行 | 命令、开始/结束时间、完整输出、`apply_exit` | `01:23:15`–`01:23:20` |
| `c1_verify.sh` | 验证 | 可复现脚本（只读） | — |
| `v1-structure.txt` | 验证 | `V-1`（`1/1/1/1`）+ `V-1c` 索引全文；含一次 `V-1b` 语法错误原文 | `01:23:34` |
| `v1b-column-attrs.txt` | 验证 | `V-1b` 修正后重跑：`TEXT / notnull=0 / dflt=NULL / pk=0` | `01:24:03` |
| `v2-fingerprint-after.txt` | 验证 | `V-2` 用 `id <= 5763` 重放，四项与基线**逐项相等**，`keyed=0` | `01:23:42` |
| `v2b-sample-post.txt` | 验证 | `V-2b` 按固定 id 列表重放，25 行原始 | `01:23:4x` |
| `v2b-compare.txt` | 验证 | `V-2b` 逐行比对结果：`field_diffs=0` / `MATCH_25` | 生成 `01:25` |
| `v3-keyed.txt` | 验证 | `V-3` `keyed_total=0` + `EXPLAIN` 走 `COVERING INDEX ux_mm_dedup_key` | `01:23:49` |
| `v1d-post-state.txt` | 补充 | 登记二次确认（`No migrations to apply`）、21 列结构、索引清单 | `01:24:19` |
| `v1e-recheck-3min.txt` | 补充 | apply 后 3 分钟复检：仍 `1/1/1/1`、`keyed=0` | `01:24:57` |
| `v1f-final-snapshot.txt` | 补充 | apply 后 8 分钟收尾快照：仍 `1/1/1/1`、`keyed=0`、`n_all=5745` | `01:27:36` |

## 三个必须随结论一起引用的限制

1. **两次 D1 `pragma` 报错均为「查询语法/支持度」问题，不是结构问题**：
   - `V-1b` 首跑用了 `AS notnull`（关键字别名）⇒ `syntax error`，SQL 未执行；
   - `P-4` 首跑用 `pragma_index_list()` ⇒ D1 `APIError`（该 pragma 不可用）。
   两者都已改用等价只读查询重取，原文保留在同一文件里，未删除。
2. **`V-2` 只能发现"被四项统计量捕获"的改动**，不能发现等长替换 / 未纳入字段变化 / 删除后补写；
   `V-2b` 的 25 行是补充证据，**不是**全表证明。
3. **本目录全部查询 `changed_db=false` / `rows_written=0`**（`apply` 那一条除外）；
   任何"生产已具备去重"的说法都不成立 —— 去重要靠 C-2 的代码，C-2 **未执行**。
