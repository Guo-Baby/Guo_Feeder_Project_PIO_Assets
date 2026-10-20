# 证据：`R-5` 六状态矩阵（`2026-10-21`）

## 文件

| 文件 | 内容 |
|---|---|
| `state_matrix.sh` | 可复现脚本：8 个实验（`S0`/`S3`/`S3+0008`/`S4`/`S5′`/`S5`/`S5+0009`/`S6`） |
| `state-matrix.log` | 上述脚本的**原始输出**（`2026-10-21 00:53`，wrangler 4.145.0） |

## 复现方式

```bash
cd D:/Guo_Feeder_Project/Cloudflare_Assets     # 必须在云端仓根目录（脚本用相对路径）
bash <本目录>/state_matrix.sh
```

脚本自己的行为：
- 在 `.wrangler/g4r5_matrix_<时间戳>/` 下建**隔离** persist 目录；
- 用 **假 `database_id`（`00000000-0000-0000-0000-000000000000`）** 的临时 `wrangler.toml`；
- 全部 `--local`；**不接触生产 D1、不接触线上 Worker**；
- 结束时保留目录（便于复核），不自动清理。

## 关键结论（原文见 `state-matrix.log`）

| 实验 | 结果 |
|---|---|
| `S0` 干净表 | `apply` 成功，`3 commands`，`col/idx/partial_idx/reg7 = 1/1/1/1` |
| `S3` 列在、未登记 | `X [ERROR] duplicate column name: dedup_key`；`reg7` 仍 0 |
| `S3` + 只建索引的 `0008` | **同样失败，且 `reg8=0`** ⇒ `0008` **未被执行** |
| `S4` 列+索引在、未登记 | 同一错误 |
| `S5′` 已登记、索引缺 + `0008` | `0008 ✅`，`2 commands`，`idx/partial_idx/reg8 = 1/1/1` |
| `S5` 已登记、列+索引都缺 | **`✅ No migrations to apply!` + `exit=0`**，结构仍 `0/0/0`（**伪成功**） |
| `S5` + 加列并建索引的 `0009` | `0009 ✅`，`3 commands`，结构恢复 |
| `S6` 非部分索引占位 | `idx=1` 但 **`partial_idx=0`** ⇒ 只看索引名会漏判 |
| `DROP COLUMN`（索引仍在） | 被拒：`error in index ux_mm_dedup_key after drop column` |
| `DROP INDEX` → `DROP COLUMN` | 本地**成功**（`col=0`）；**生产未验证** |

## 边界（不得外推）

- 以上全部是**本地 D1 引擎**行为。生产 D1 从未进入这些状态，
  ⇒ **不得**当作生产原子性/生产 `DROP COLUMN` 可用性的保证。
- 脚本里的 `0008`/`0009` 是**实验用临时文件**，写在隔离目录内，
  **不在** `guo-feeder-api/migrations/`，**不会**被真实部署拾取。
