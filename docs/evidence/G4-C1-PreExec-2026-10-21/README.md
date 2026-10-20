# 证据：C-1 执行前最终复核（`2026-10-21 00:58`）

全部为**只读**操作（`wrangler whoami` / `d1 list` / `versions view` /
`d1 migrations list` / `d1 execute --remote` 的 `SELECT`）。
`meta.changed_db=false`、`rows_written=0`（见 `readonly-meta.txt`）。

| 文件 | 对应项 | 关键值 |
|---|---|---|
| `r1-identity.txt` | `R-1` ①②③ | 账号 `9e72d201…`；`database_id=0d8385ba…`；`d1 list` 仅一条 `guofeeder` |
| `r2-artifact-and-binding.txt` | `R-1` ④ + `R-2` | 线上版本绑定 `env.guofeeder_DB (0d8385ba…)`、`env.LOG_BOOT_WATERMARK ("1")`；`sha256=90dc0ec0…`；两处 blob 均 `eb300070…` |
| `r5-state-probe.txt` | `R-5` 状态判定 + 结构 | `col/idx/partial_idx/col_ok/reg7 = 0/0/0/0/0`，`nn/dflt/ctype = null` ⇒ **`S0`**；表 20 列；`mqtt_messages` 无索引 |
| `a3-a4-fingerprint.txt` | 执行前基线 | `n=5745` · `min_id=1` · `max_id=5763` · `devs=73` · `pl_len=817694` · `id_len=36151` · `tp_len=154882` · `no_msg_id=1698` |
| `v2b-sample-pre.txt` | `R-3` 抽样 | 5 组 × 5 行 = **25 行**（`OFFSET 0/1430/2870/4300/5740`，均 `id <= 5763`） |
| `fixture-vs-prod-cols.txt` | 结构同构 | 生产 20 列 == fixture 20 列，`prod-only`/`fix-only` 均为空 |
| `readonly-meta.txt` | 只读证明 | `changes=0` · `changed_db=false` · `rows_read=5745` · `rows_written=0` |

## 使用注意

- 这些是**执行前快照**。真正执行时必须**重新采集**（`MAX_ID_BEFORE` 会变）。
- `v2b-sample-pre.txt` 的价值在**记录下来的 25 个 `id`**：执行后要按
  `WHERE id IN (…)` 重放，**不要**重跑 `OFFSET`（期间新增/删除会让 OFFSET 错位）。
- `d1 list` 的 `num_tables` 返回 **0**，与真实表数不符（API 未填充该列）
  ⇒ **不得**用该列判断库状态。
