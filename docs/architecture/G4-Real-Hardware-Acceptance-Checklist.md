# 焊接完成后的真机验收清单（G4 · 日志可信度 + 内存观测）

- 日期：`2026-10-20`
- 适用：硬件（电机 / 电磁阀 / 称重 / OLED / RTC）**焊接完成并确认上电安全**之后
- 前置门禁：`G-4 = BLOCKED` · `WP-5 未解锁` · `STOP-R1` 无解除文件
  ⇒ **本清单不解除任何门禁**，只定义"怎样才算真正验收过"
- 用法：逐项执行、逐项记录（`PASS` / `FAIL` / `BLOCKED` / `NOT RUN` + 证据路径）
  ★ **不允许**把"代码构建通过"写成任何一项的 PASS

---

## 0. 开测前必做（缺任一 ⇒ 全部标 NOT RUN）

| # | 项 | 判据 | 记录 |
|---|---|---|---|
| 0-1 | 硬件目视与万用表检查 | 无短路 / 无倒装 / 电源轨电压正确 | 照片 + 实测电压 |
| 0-2 | 固件来源可追溯 | `git rev-parse HEAD` + 构建产物 `sha256` + 烧录命令 | 三项都要存 |
| 0-3 | 回退件在手 | 上一版 `.bin` + `sha256`，且**已验证可烧回** | 路径 + 哈希 |
| 0-4 | 设备配置快照 | 先 `config_query` 存盘；★ **仓库有值 ≠ 设备实值** | JSON 原文 |
| 0-5 | 新 boot 基线 | 记录 `boot_seq`（烧录后第一次启动值） | 数字 |
| 0-6 | 串口会话 | 单会话跑完（每次重开都可能复位 MCU） | 日志文件 |

---

## 1. 完整硬件配置启动与连续运行

| # | 检查 | 判据 | 证据 |
|---|---|---|---|
| 1-1 | 冷启动成功 | 串口出现 `boot validated`，`boot_seq` 单调 +1 | 串口日志 |
| 1-2 | 各模块就绪 | `system.memory` / `system.stats` / `system.loop` / `system.memwatch` 四个命令**都**返回 `status:success` | 命令回包 |
| 1-3 | 无异常复位 | `reset_reason` ∉ {panic, task_watchdog, brownout, interrupt_watchdog} | `system.restart_status` |
| 1-4 | 连续运行 ≥ 30 min | 期间无 WDT 复位、无 `hole_overflow=1` | 串口 + `system.stats` |
| 1-5 | 外设真实动作 | 阀/电机按指令动作，且**动作与日志时间线能对上** | 视频 + 串口时间戳 |

★ 1-4 的 30 min **不是**门禁窗口（门禁是 2 h + 30 min），只是"焊接后能稳住"的最低线。

---

## 2. 日志端到端对账（最高优先级）

**目标**：在**同一时间窗口**内，让下面五段数字可比。

| # | 段 | 取值 | 期望关系 |
|---|---|---|---|
| 2-1 | ① 设备发出 | `system.stats.counters.cloud_records_sent` | 基准 |
| 2-2 | ② 设备已确认 | `counters.cloud_ack_ok` | ≈ ① |
| 2-3 | ③ 云端 accepted | `/api/history/ingest` 响应（Pages 日志） | ① ≈ accepted + duplicate |
| 2-4 | ④ D1 行数 | `SELECT COUNT(*) FROM log_history WHERE device_id=:dev AND boot_seq=:b` | ④ == Σ accepted |
| 2-5 | ⑤ 丢弃类 | `drop_overflow` / `drop_unacked` / `ack_lost` / `give_up` | 全 0 才能写"无丢弃" |

操作序列：

```
1) 记 t0 = 当前时刻；读 system.stats 存盘（基线 A）
2) 制造 ≥ 20 条可预期日志（例如连续 5 次阀动作 + 3 次配置查询）
3) 等 ≥ 60 s（确保上传 + ACK 窗口完整）
4) 读 system.stats 存盘（快照 B）；Δ = B − A
5) 用 t0 查 log_history 与 Pages 日志
6) 逐项填下表
```

| # | 项 | 判定 |
|---|---|---|
| 2-6 | Δ① == log_history 新增（本 boot） | PASS / FAIL |
| 2-7 | Δ② == Δ①（无 give_up） | PASS / FAIL |
| 2-8 | Δ`drop_*` 全 0 | PASS / FAIL |
| 2-9 | `acked_seq == 该窗口最后一条 seq` | PASS / FAIL |
| 2-10 | 时间戳语义正确（`log_history.ts` 与设备本地时间一致，允许时区偏移已记录） | PASS / FAIL |

★ **任一 FAIL 都不允许**用"再跑一次就好了"结案，必须留下差分快照。

---

## 3. 断网 / 重连 / 重复投递 / ACK 丢失 / 云端暂时失败

| # | 场景 | 注入方式 | 期望 | 观察点 |
|---|---|---|---|---|
| 3-1 | 断网（WiFi 断） | 路由器下线 AP | 不丢 WARN+（落 Flash）、`offline_skip` 增长、`drop_*` 不增长 | `system.stats` |
| 3-2 | 重连恢复 | 恢复 AP | 补发（`replay_records` 增长）后 `acked_seq` 追上 | 同上 |
| 3-3 | Broker 重连 | EMQX 侧断开客户端 | 同上；`recv_cnt` 归零属正常（每会话计数） | EMQX `/clients/{id}` |
| 3-4 | 重复投递 | 让 Worker 侧重复接收同一批（模拟） | `log_history` **不增行**；`duplicate` 计数增长 | ingest 响应 |
| 3-5 | ACK 丢失 | 阻断下行 `log_ack` | `cloud_ack_timeout` / `cloud_retx` 增长；**行不删**；重试后仍可 ACK | `system.stats` |
| 3-6 | 云端暂时失败 | Worker 返回 5xx（临时改 Pages 路由再恢复） | 设备重投；`duplicate` 吸收；不产生新行 | ingest 响应 + D1 |
| 3-7 | 设备重启后重放 | 断电重启 | `acked_seq` 归 0 ⇒ 重发历史；D1 幂等吸收（`duplicate`） | 前后行数对比 |

★ 3-6 需要改 Pages/Worker 路由 ⇒ **属生产变更**，需单独授权；未授权则标 `NOT RUN`。

---

## 4. 队列满 / 存储不足 / 内存压力 / 重启

| # | 场景 | 方法 | 期望 | 判据 |
|---|---|---|---|---|
| 4-1 | 云队列满 | 长时间离线 + 高日志率 | 淘汰最旧并 `drop_overflow++`；**不静默** | `drop_overflow > 0` 且可见 |
| 4-2 | Flash 段环压满 | 同上（WARN+ 落盘） | 段淘汰时 `seg_evicted_unacked++`、`drop_unacked++` | 计数可见 |
| 4-3 | Flash 空间不足 | 预置大文件占满 LittleFS | `file_storage` 写失败可见；**不崩溃** | 串口 + `system.flash` |
| 4-4 | 内存压力 | 多次 `workflow.create` 逼近上限 | `system.memwatch.alloc.final_fail` 增长（**仅显式分配点**）；无 panic | memwatch |
| 4-5 | 重启后行为 | 断电重启（3 次连续） | 每次 `boot_seq` +1；无隔离（`seq_failclosed=0`）；无 `corrupt_count` 增长 | `system.stats` |
| 4-6 | 元数据半写 | 上板注入 `logt` 的 meta 故障（若可用） | 选槽回退，`boot_seq` 不回退 | 串口 |

★ 4-4 的结论只能写"**已登记显式分配点**内无/有失败"。
Arduino `String`、第三方库（含 mbedTLS）、IDF 自身分配**不在覆盖范围**。

---

## 5. 硬件在环的时间线关联

| # | 场景 | 期望 |
|---|---|---|
| 5-1 | 电磁阀开/关 | `LOG` 中阀事件时间戳与视频/示波器**同向**（≤ 1 s 偏差） |
| 5-2 | 电机出粮 | 同上；且重量变化事件紧随其后 |
| 5-3 | 称重传感器 | `weight.value` 遥测与日志中的重量参数一致 |
| 5-4 | 缺水/超时保护 | `dispense_guard` 触发 ⇒ 有对应 WARN/CRITICAL 日志 |
| 5-5 | 断水/空桶 | 同上（若无对应 EventId ⇒ 登记为**观测缺口**，不是 PASS） |

---

## 6. 2.5 小时长窗口（`S-3-L3` + `G4-R4` H-3）

**前置**（缺一不可，见 `S-3-Spec-Revision-R3.md` §2.4）：硬件 bring-up · `config_query` 快照入库 ·
单独授权 · 新 boot 基线 · 设备专用（不与他人共用）

| # | 段 | 时长 | 记录 |
|---|---|---|---|
| 6-1 | 稳态 | 2 h | 每 10 min 一次 `system.stats` + `system.loop` + `system.memwatch` |
| 6-2 | 突发 | 30 min | 集中触发事件（阀/电机/配置查询），保持 ≥ 1 条/秒 |
| 6-3 | 结论 | — | **必须**同时给 `p99_us` **与** `max_us`（`S-3-O1`：单次 356 ms 尖峰真实存在） |
| 6-4 | 内存漂移 | 全程 | `system.memwatch.drift.dram.slope_per_hour`（窗口 ≥ 30 min 才 `valid`） |
| 6-5 | 连续块 | 全程 | `drift.dram.largest_min`（**碎片硬指标**，比占用率更有意义） |
| 6-6 | 低水位 | 全程 | `dram.minimum_free_since_boot` |
| 6-7 | 栈高水位 | 全程 | 先跑 `mw probe` 确认口径，再读 `stack.used_bytes` |

★ 配置必须**生产形态**：OLED 真实刷新、`weight.enable=true`。
否则结论只是"缩减配置"的结论（`S-3-O3` 已踩过）。

★ **不得**写"2.5 h 没崩 ⇒ 不存在泄漏/溢出风险"（记述纪律）。

---

## 7. B-3 漏记复查（只读取证清单）

| # | 阶段 | 数据源 | 是否可得 |
|---|---|---|---|
| 7-1 | 设备没产生 | 设备侧 `emit_total` | ✅ `system.stats` |
| 7-2 | 设备未发出 | `emit_total − cloud_records_sent`（**本轮新修，原恒 0**） | ✅ |
| 7-3 | Broker 未接收 | EMQX `/clients/{id}` 的 `recv_cnt` | ✅（只读 API 可用） |
| 7-4 | 规则未匹配 | EMQX 数据集成 → 规则 matched 计数 | ❌ API **403**，需控制台截图 |
| 7-5 | Action 未转发 | Action 的 success/failed/retry | ❌ 同上 |
| 7-6 | Pages 未收到 | Cloudflare Pages 请求数 | ⚠️ 部分可得 |
| 7-7 | Worker 未处理 | Workers 分析（`workersInvocationsAdaptive`） | ✅ GraphQL |
| 7-8 | D1 未入库 | `log_history` 行数 | ✅ 只读 SQL |

★ 7-4/7-5 的截图**必须**包含时间范围与规则名，否则无法与 7-7 对齐。
★ 在拿到 7-4/7-5 之前，**不得**定案"丢在哪一层"。

---

## 8. 未就位即标 NOT RUN 的清单（诚实边界）

以下项目在硬件未就位时**一律** `NOT RUN`：

- 5-1…5-5（除 5-3 若称重未装）
- 6-1…6-7（长窗口需生产配置）
- 3-6（需改生产路由）
- 4-1…4-3（需可复现的压力条件）
- 1-5（需执行机构通电）

★ `NOT RUN` **不等于** PASS。交付时必须单独列出 NOT RUN 清单。

---

## 9. 记录模板（每项一行）

```
| ID  | 场景 | 结论 | 证据路径 | 时间 | 备注 |
|-----|------|------|----------|------|------|
| 2-6 | Δ① 对账 | PASS | docs/evidence/.../stats-A.json, stats-B.json | 2026-MM-DD HH:MM | Δ①=23, 入库=23 |
```

结论取值只允许：`PASS` / `FAIL` / `BLOCKED` / `NOT RUN`。
`PARTIAL`、`基本通过`、`应该没问题` **一律不接受**。
