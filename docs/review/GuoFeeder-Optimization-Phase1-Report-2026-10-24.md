# 第一阶段实施报告 · 观测基线与 D1 读取优化（2026-10-24）

> 计划依据：`GuoFeeder-Optimization-Plan-Rev2-2026-10-24.md` 阶段一（P1-1 ~ P1-8）。
> 边界：**本地实施、未部署、未应用迁移、未改线上任何状态**。
> 证据：`docs/evidence/D1-Worker-Usage-2026-10-23/`（GraphQL 原始返回）。

---

## 1. 代码基线与 Git 状态

| 项 | 值 |
|---|---|
| 云仓 | 分支 `main` · HEAD **`8e8d3e0`**（未提交本次改动） |
| 固件仓 | 分支 `wb` · HEAD `3cdd216`（仅新增文档） |
| Worker 线上版本 | `a5d14bdd-a062-4593-b73d-30c12570b460`（2026-10-23T16:10:02Z）— **本次未部署** |
| Pages 线上 | `b4aa1257` — **本次未部署** |
| D1 迁移 | `0001`–`0008` 已应用；**`0009` 为草案，未应用** |

---

## 2. 实际修改的文件及职责

| 文件 | 类型 | 职责 |
|---|---|---|
| `guo-feeder-api/src/read-api.js` | 改（+38/−6） | snapshot 增加 `counts` 参数（默认不算 COUNT，显式 `1` 才算）；校验前置（非法入参不触达 D1）；未计算时 `counts=null` + `counts_included=false` |
| `guo-feeder-pagesdev/assets/js/gf-app.js` | 改（+121/−72） | 新增 `fetchSnapshot()`（`recent=1&counts=0` + `home_snap` 缓存）；首页改为缓存驱动 + 手动刷新（失败保留旧值并标注时刻）；删除首页固件版本/最后上报字段；设备页删除条数 KPI 且不再调 `metrics`；重启回归轮询加 `counts=0`；新增 `cachedSnapshot()` 供重启基线 |
| `guo-feeder-api/migrations/0009_ui_read_indexes.sql` | **新增（草案）** | 4 条覆盖索引 + 根因/写成本/回滚说明 |
| `scripts/d1_usage_report.py` | 新增 | 只读 GraphQL 统计：日/小时/逐 SQL/Worker；原始留档、保留期、冷却、调用预算、不重试 |
| `scripts/local_index_plan_check.py` | 新增 | 本地 SQLite 复刻 schema，EXPLAIN 前后对比（零网络） |
| `tests/api/snapshot_counts.test.mjs` | 新增 | 11 项：证明"默认不发 COUNT"、`counts=1` 才发、非法入参不触达 D1、`counts=2` 不当 true |
| `tests/pages/ui_app_runtime.test.mjs` | 改（+55/−4） | 新增 T-16 五项（counts=0、二次进页不回源、字段精简、单位+时刻、无条数统计）；T-5-4 改为"已移除表规模"断言 |
| `tests/pages/ui_app.test.mjs` | 改（+28） | 新增 S-16 七项代码级断言（含 Worker 侧默认不算） |
| `.gitignore` | 改（+3） | 忽略 `scripts/usage-out/`（统计输出按保留期自动清理） |

---

## 3. 查询计划：修改前 / 修改后（本地 EXPLAIN QUERY PLAN 实测）

命令：`python scripts/local_index_plan_check.py`（合成样本：log 3 万 / tel 2 万 / mqtt 8 千 / event 3 千，双设备交错）

| 查询（生产实测消耗） | BEFORE（现状索引） | AFTER（+0009） | 判定 |
|---|---|---|---|
| Q1 log 列表（340 万读/156 次） | `SEARCH ix_lh_device_ts(device_id=?)` + **`USE TEMP B-TREE FOR ORDER BY`** | `SEARCH ix_lh_device_id(device_id=?)` | ✅ 扫描放大消失 |
| Q4 telemetry 列表（35.8 万/114 次） | 同上形态（临时 B 树） | `SEARCH ix_th_device_id` | ✅ |
| Q3 mqtt by msg_id（126 万/152 次） | **`SCAN mqtt_messages`（整表扫）** | `SEARCH ix_mm_device_msg_id(device_id=? AND msg_id=?)` | ✅ |
| Q10 device_event（6.8 万/110 次） | 临时 B 树 | `SEARCH ix_ev_device_id` | ✅ |
| Q2 snapshot 3×COUNT（147 万/113 次） | covering index **扫全分区** | 同（索引**无法**降低） | ⚠ 只能"不默认算" ⇒ 已由 P1-5 解决 |

> 注意：合成样本只证明**计划选择**，不代表生产行数分布。生产侧计划需部署后由 D1 控制台/`EXPLAIN` 复核（见 §7 待验）。

---

## 4. 用量：修改前 / 修改后

### 4.1 修改前（官方 GraphQL，已实测）

| UTC 日 | 查询次数 | Rows Read | Rows Written | Worker requests |
|---|---|---|---|---|
| 2026-10-23 | 60,020 | **7,299,585** | 88,457 | 26,751 |
| 2026-10-24（至 12:52Z） | 48,348 | 413,633 | 80,330 | 22,827 |

### 4.2 修改后（**PENDING — 未部署，无生产数字**）

本报告**不提供"修改后"实测值**，因为部署属未批准动作。给出的是**同负载静态推算**（标注为估算）：

| 项 | 10-23 实测 | 部署后估算 | 依据 |
|---|---|---|---|
| 读：4 条分页查询 | 5,091,000（68%） | ≈ 11,000 | 21,827 行/次 → 20 行/次（LIMIT） |
| 读：snapshot COUNT | 1,471,424（20%） | 0 | UI 不再请求 `counts` |
| 读：metrics 全表 COUNT | 165,428（2%） | 0 | UI 不再调用 |
| **读合计** | **7,299,585** | **≈ 0.75M（−90%）** | 估算 |
| 写：`0009` 索引维护 | — | **+11,700~12,000（+13%）** | log 11,725 次 INSERT × +1 索引项 |
| Worker 请求 | 26,751 | 基本不变（未改路由/轮询节奏） | — |

> ⚠️ **写成本必须如实看待**：索引不是免费的。log 每条 INSERT 当前写 3.25 行（表 1 + 索引 2 + 序列），加 1 个索引 ⇒ 4.25 行/次 ⇒ **+1.17 万写/日**。
> 这也是阶段三（watermark 32,142 写/日）必须同步治理的原因：索引 +12K 与 watermark −30K 配对，才能让**写量净下降**。
> **若用户优先保写额度，可只上 `log_history` + `telemetry_history` 两条索引**（读收益 ~50%，写 +约 1.2 万仍成立）—— 需用户裁决。

---

## 5. 测试项目 / 结果 / 未覆盖场景

**全量回归：10 套件 813 PASS / 0 FAIL**

| 套件 | 结果 | 备注 |
|---|---|---|
| ui_security | 273 PASS | 未受影响 |
| pages_auth | 104 PASS | 未受影响 |
| pages_webhook | 22 PASS | 未受影响 |
| log_decode | 29 PASS | 未受影响 |
| history_frames | 33 PASS | 未受影响 |
| history_command | 18 PASS | 未受影响 |
| wfadmin | 65 PASS | 未受影响 |
| **snapshot_counts（新）** | **11 PASS** | 默认零 COUNT / 显式才算 / 非法入参不触达 D1 |
| ui_app | 169 PASS（+7 S-16） | 代码级断言 |
| ui_app_runtime | 89 PASS（+5 T-16） | 真实模块 + DOM 桩 |

**新增断言覆盖的行为**：
1. 所有 snapshot 请求都带 `counts=0`（含重启回归轮询）
2. 二次进入首页命中 `home_snap` 缓存、**零请求**
3. 首页不再有固件版本/最后上报 KPI，三个数值卡都带名称+单位+时刻
4. 设备页不再渲染条数统计、不再请求 metrics
5. Worker：不传 counts ⇒ 零条 COUNT 语句；`counts=1` ⇒ 恰好 1 条；非法值 ⇒ 400 且不触达 D1

**未覆盖（如实）**：
- 生产部署后的真实 Rows Read/Written（需部署 + 48 h）
- 生产 `EXPLAIN QUERY PLAN`（需 `0009` 应用后）
- 多标签页并发下的缓存一致性（阶段二）
- 首页刷新失败路径的真实网络态（仅单测桩覆盖）
- EMQX 在线状态（阶段二）

---

## 6. 预期收益 vs 实际收益

| 项 | 预期 | 实际 |
|---|---|---|
| 读消耗 | −90%（估算） | **PENDING**（未部署，不预标） |
| 写消耗 | +13%（索引成本） | **PENDING** |
| 本地测试 | 全绿 | ✅ 813/0 |
| 查询计划 | 4 条改为索引扫描 | ✅ 本地实测 PASS |
| 首页请求 | 打开不再自动回源 | ✅ 单测证明（T-16-2） |

---

## 7. 风险 / 回滚 / 下一阶段解锁

**风险**
1. 索引未被生产计划采用（统计信息差异）⇒ 读收益落空，但仍会付写成本 → 部署后 24 h 必须核对 `EXPLAIN` + `rowsRead`。
2. 写 +13% 与现有写压力叠加（今日 8 万/日）⇒ 可能触顶；**建议与阶段三 watermark 降写同批上线**。
3. `counts` 默认关闭属**契约变更**（原响应总带 counts）⇒ 已保留 `counts=1` 兼容路径；无第三方消费者（仅自家 UI）。
4. 首页改为"缓存优先"后，首次打开仍会请求一次；若设备离线且本地无缓存 ⇒ 显示"尚无数据"（明确空态，不伪造）。

**回滚**
- 代码：`git checkout` 三个改动文件即可（无数据变更）。
- 索引：`DROP INDEX IF EXISTS ix_lh_device_id; ix_th_device_id; ix_mm_device_msg_id; ix_ev_device_id;`（不改数据）。
- `counts`：UI 回滚后 Worker 默认仍为 0 ⇒ 需同时回滚两端，或调用方显式传 `counts=1`（兼容）。

**旧任务影响与依赖**
- 无旧任务被关闭或降级。`G4-R2`（`drop_*` 落库）若走 DDL 路线，**建议与 `0009` 同批评审**（避免连续两次生产 DDL）。
- `G4-R1`（漏记观测）可复用本阶段观测脚本能力，但本阶段未实现逐帧计数。
- `WP-5` 排期需后于阶段三（日志写入语义改动）。
- 9 项"已完成未关账"任务仍待用户确认，**本轮未动**。

**下一阶段解锁条件**
1. 用户批准 §8 决策（尤其 D1：是否全量 4 条索引 / 是否接受 +13% 写）
2. 部署 + 48 h 观测：读降 ≥70%、写增幅 ≤ +20%
3. 生产 `EXPLAIN` 复核新索引被采用
4. 满足后 → 阶段二（首页 IA + EMQX 在线状态）开工

---

## 8. 需要用户裁决（本阶段产出的决策点）

| # | 决策 | 选项 |
|---|---|---|
| D1 | `0009` 索引范围 | 全 4 条（读 −90%，写 +13%）/ 仅 log+telemetry（读 −55%，写 +13% 中的 log 部分）/ 暂不应用 |
| D2 | 部署时序 | 仅部署代码（`counts` 默认关）先拿读收益 / 代码+索引同批 / 等阶段三一起上 |
| D3 | 首页"首次打开自动请求一次"是否接受 | 接受（否则首屏空白）/ 改为显式按钮（首屏只有空态） |
| D4 | `metrics` 端点去留 | 保留精确值供运维（现状）/ 加 `?approx=1` 近似 / 下线 |
| D5 | 索引写成本的对冲 | 等阶段三 watermark 降写一起 / 先只上读收益最大的 1–2 条 |
