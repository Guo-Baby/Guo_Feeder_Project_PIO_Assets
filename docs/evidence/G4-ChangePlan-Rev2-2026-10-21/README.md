# 证据：G4 生产变更计划 Rev.2 与上线前核实（`2026-10-21`）

> 对应报告：`docs/architecture/G4-Production-Change-Plan-Review-2026-10-20.md`
> 对应执行手册：`docs/architecture/G4-Production-Change-Plan-Rev2-2026-10-20.md`
> ★ 本目录全部为**本地**实验与**读码**结论。**未**接触远端 D1、**未**部署 Worker、**未**改 EMQX、**未**烧录。

---

## 1. 文件清单

| 文件 | 内容 | 结论 |
|---|---|---|
| `local-migration-experiments.log` | E-1 / E-2 / E-3 三项实验的最终状态查询 | 0007 在**缺表**库上失败且**不登记**；在**有表**库上成功且**登记**；migration **整文件原子** |
| `full-repro-0007-apply.log` | 用**正式** `d1 migrations apply` 从零复现 0007 的端到端流程（fixture 建表 + 预置 2 行完全重复） | `0007 ✅`；`col/idx/partial_idx/reg = 1/1/1/1`；历史 2 行重复**仍在**、键全 NULL |

原始 persist 目录（`Cloudflare_Assets/.wrangler/`，**已 gitignore**，仅本机留存）：
`g4c1_verify_20261021000217`（E-1）· `g4c1_ok_20261021000242`（E-2）·
`atomic_probe_000522`（E-3）· `g4c1_repro_000821`（端到端复现）。

## 2. 三项实验的设计与判据

| ID | 目的 | 构造 | 判据 |
|---|---|---|---|
| **E-1** | 正式流程在**缺 `mqtt_messages`** 的库上会怎样 | 空 persist ⇒ `d1 migrations apply --local` | `0001`–`0006` 登记、`0007` **不登记**（`no such table`） |
| **E-2** | 正式流程在**有表 + 有历史重复数据**的库上会怎样 | 先用 fixture 建同构表，再插 2 行**完全重复** | `0007 ✅` 且登记；结构 4 项全 1；历史行数不变、键全 NULL |
| **E-3** | migration 是否**原子**（中途失败会不会留半成品） | 临时工程：`CREATE TABLE` + `ALTER ADD COLUMN` + `SELECT * FROM 不存在的表` | 失败后 `tbl=0 / col_b=0 / reg=0` ⇒ **零残留、零登记** |

## 3. 为什么这三项重要

1. **E-1** 说明 `mqtt_messages` 没有 migration 定义它（P0 既有生产表）⇒ **本地重建库必须先用 fixture**；
   远端不受影响（表已存在），但执行前**必须**把生产 `sqlite_master.sql` 与 fixture 逐列比对（Rev.2 §2.5）。
2. **E-2** 说明 0007 **不会**追溯合并历史重复行、**不会**回填 `dedup_key` —— 这是设计，不是遗漏。
3. **E-3** 说明**不存在**"列建了但索引没建"或"执行了但没登记"的半成品状态
   ⇒ 失败后的恢复路径只有一条：**修好原因再 apply**；**禁止**手工补 SQL。

## 4. 证据的边界（必须随结论一起引用）

- E-3 是**本地 D1 引擎**（workerd/miniflare）的实测。**生产 D1 的原子性未在本阶段验证**。
  官方语义为"每个 migration 文件在事务中执行"，与本地一致，但**不得**据此替代生产观测
  ⇒ Rev.2 把 `V-1` 定为 C-3 的**每 10 min 复检项**。
- 本目录**没有**任何远端查询输出：Rev.1 报告 §8 的生产统计（`msg_id` 覆盖、payload 分组等）
  是**上一轮**会话内执行的只读结果，**未**在本轮回放、**未**重新验证。
  ⇒ 引用时必须注明"上一轮只读统计，非本轮重测"。
- 本目录**没有** Worker Logs / `wrangler tail` 的任何输出（历史 403/502，本轮未重试）。
