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

> ★ **`2026-10-21` 修正**：本节原写的 `② ≈ ①`、`① ≈ accepted + duplicate`、`④ == Σ accepted`
> 三条关系**全部作废**（原因见 `G4-Production-Change-Plan-Review-2026-10-20.md` §E.4）：
> `cloud_ack_ok` 是**批次**数不是条数；`③` 的四个计数 **我方不可观测**
> （`/api/history/ingest` 不打印，响应只回 EMQX Action）；`seq` 也不是 boot 内序号。
> 现行判据以 `G4-Production-Change-Plan-Rev2-2026-10-20.md` §5.3 的 `I-1…I-9` 为准。

**目标**：在**同一时间窗口**内（且 `boot_seq` **不变**），让下面几段数字**可比**。

| # | 段 | 取值 | 关系 | 性质 |
|---|---|---|---|---|
| 2-1 | ① 设备首发出 | `counters.cloud_records_sent`（RAM，重启归零） | 基准 | 直接测量 |
| 2-2 | ② 设备已确认（**批次**） | `counters.cloud_ack_ok` | `Δ② ≤ Δcloud_batch_sent` | ✅ 严格不变量 |
| 2-3 | ③ 云端入库 | **Δ`log_history` 行数**（按 `device_id`+`boot_seq`+`created_at`） | `④ ≤ Δ①`；`④ == COUNT(DISTINCT seq)` | ✅ 严格不变量 |
| 2-4 | ③′ 响应四计数 | `/api/history/ingest` 的 `{accepted,duplicate,rejected,filtered}` | — | ❌ **NOT MEASURABLE**（不落日志，见 `LOG-COUNTERS`） |
| 2-5 | ⑤ 丢弃类 | `drop_overflow` / `drop_unacked` / `ack_lost` / `give_up` | 全 0 才能写"无丢弃" | ✅ 严格 |
| 2-6 | ⑥ `cloud_batch_sent` / `cloud_retx` | 同上 | `Δretx ≤ Δbatch_sent` | 记录项 |

操作序列：

```
1) 记 t0 与 boot_seq；读 system.stats 存盘（基线 A）
2) 制造 ≥ 20 条可预期日志（例如连续 5 次阀动作 + 3 次配置查询）
3) 等 ≥ 60 s（确保上传 + ACK 窗口完整）
4) 读 system.stats 存盘（快照 B）；Δ = B − A；★ 先确认 boot_seq 未变，变了则整轮作废
5) 用 t0 查 log_history（秒单位）与 mqtt_messages（毫秒单位）—— 不要混用
6) 逐项填下表
```

| # | 项 | 判定 |
|---|---|---|
| 2-7 | `boot_seq` 在窗口内未变 | PASS / **作废重来** |
| 2-8 | `Δ② ≤ Δcloud_batch_sent` | PASS / FAIL |
| 2-9 | `Δlog_history 行数 ≤ Δ①` | PASS / FAIL |
| 2-10 | `log_history 行数 == COUNT(DISTINCT seq)`（同一 `device_id`+`boot_seq`） | PASS / FAIL |
| 2-11 | `Δdrop_*` 全 0 | PASS / FAIL |
| 2-12 | `acked_seq` 单调不减（窗口 B ≥ 窗口 A） | PASS / FAIL |
| 2-13 | **记录项（不判 PASS/FAIL）**：`Δ① − Δlog_history` 的差额 | 附解释：在途 / 被过滤 / 被拒收 / 被去重 / Flash 补发，**当前不可分解** |

★ **禁止**：把 `Δ① − Δlog_history` 的差额直接写成"丢了多少条"（它含多个不可观测分量，见 `A-1`）。
★ **禁止**：用 `cloud_ack_ok` 与 `log_history` 行数相比（量纲不同）。
★ **禁止**：把本轮的 2 小时量产窗口当作 `S-3-L3` 的 2.5 小时门禁窗口。
| 2-14 | 时间戳语义正确（`log_history.ts` 与设备本地时间一致，允许时区偏移已记录） | PASS / FAIL |

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
| 7-2 | 设备未上云（**不可直接读成「未发出」**） | `emit_total − cloud_records_sent`（`cloud_records_sent` 于 `2026-10-20` 修好，原恒 0） | ⚠️ 可得，但**差额含多种分量**：DEBUG（本就不上云）+ 仍在队列/Flash 的在途 + 被 `drop_*` 淘汰的；**不可**把差额整体写成「设备未发出」 |
| 7-3 | Broker 未接收 | EMQX `/clients/{id}` 的 **`recv_msg`**（**不是** `recv_cnt`） | ✅（只读 API 可用）。★ `recv_cnt` 是**报文数**（含 CONNECT/PINGREQ/PUBACK），`recv_msg` 才是 PUBLISH 条数；实测同窗口 `recv_cnt=116` vs `recv_msg=35`，**混用会得出"丢 80%"的假结论** |
| 7-4 | 规则未匹配 | EMQX 数据集成 → 规则 matched 计数 | ❌ API **403**，需控制台截图 |
| 7-5 | Action 未转发 | Action 的 success/failed/retry | ❌ 同上 |
| 7-6 | Pages 未收到 | Cloudflare Pages 请求数 | ⚠️ 部分可得 |
| 7-7 | Worker 未处理 | Workers 分析（`workersInvocationsAdaptive`） | ✅ GraphQL |
| 7-8 | D1 未入库 | `log_history` 行数 | ✅ 只读 SQL |

★ 7-4/7-5 的截图**必须**包含时间范围与规则名，否则无法与 7-7 对齐。
★ 在拿到 7-4/7-5 之前，**不得**定案"丢在哪一层"。

### 7.9 归因分布判据（`R-6` 修订，**按方向分开，不得混算**）

`2026-10-21` 只读取证确定的真实分布：**全库 `echo` 只存在于 `direction='down'`（1 539 行），
从未出现 `echo/up`** ⇒ 任何"audit 与 echo 必须都 > 0"的表述，只要没限定方向，
就会在 `up` 上**永久误报**。

| # | 方向 | 必须验证 | 判据 | 明确**不**要求 |
|---|---|---|---|---|
| 7-9 | `down` | `echo` 存在性 | `n_echo > 0`（窗口 ≥ 10 min，echo 实测晚 33–37 s） | 不要求 `audit > 0`（`audit/down` 自 `2026-10-20 02:50` 起归零，**原因未查明**） |
| 7-10 | `up` | 上行仍在 + 归属合法 | `n_up > 0`；`device_id` 合法（见 7-11） | **不得**要求 `echo/up`（不存在是**正确状态**） |
| 7-11 | `up`/`down` | `$tenants` 归属 | `$tenants/*` 行 `device_id` 为合法 12-hex | 允许 `unresolved`（仅来自真实畸形 topic） |
| 7-12 | `log` | 日志契约 | 看 `log_history`（`rows == COUNT(DISTINCT seq)`）+ 设备计数 | **不得**套用 up/down 的 audit/echo；**不得**用 `mqtt_messages` 当唯一证据 |
| 7-13 | `unknown` | 旧路由残留 | 窗口内计数 = **0** | — |

统计口径（三句必须写进记录）：

1. 统计窗口**同一**（`:ts0_ms` 起），且 `boot_seq` 未变；
2. 全部查询排除探针行：`AND COALESCE(raw_payload,'') NOT LIKE '%G4PROBE-%'`；
3. `audit`/`echo` **按 `substr(topic,1,9)='$tenants/'` 区分**，并按 `direction` 分组 —— 二者不可省。

★ 反方向声明：本节修订**只是**消除不适用于 `up` 的 `echo` 断言，
**不构成** B-3 已解决的任何证据，也**不证明** `up` 的审计/去重链路正确。

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
| 2-9 | Δlog_history ≤ Δ① 对账 | PASS | docs/evidence/.../stats-A.json, stats-B.json | 2026-MM-DD HH:MM | Δ①=23, 入库=23（**差额需附解释**，见 2-13） |
```

结论取值只允许：`PASS` / `FAIL` / `BLOCKED` / `NOT RUN`。
`PARTIAL`、`基本通过`、`应该没问题` **一律不接受**。
