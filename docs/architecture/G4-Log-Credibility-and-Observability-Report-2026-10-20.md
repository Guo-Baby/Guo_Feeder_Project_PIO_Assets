# G4 · 日志可信度与会话级验收报告（2026-10-20）

> 本文件是**本轮任务书的唯一总结**，对应 §11 要求的 A–G 七份报告。
> 逐项证据路径均可在仓库内复核。
> **结论先行**：日志链路具备"可测量、可对账"的形态；B-1/B-2 已达到可进入生产变更条件；
> G4-R4 观测能力已上板并**用数据确定了栈口径**；B-3 漏记**未解决**；
> **门禁状态一个都没有改变**（`G-4 = BLOCKED` / `WP-5` 未解锁 / `BT-1 = CLOSED` / `STOP-R1` 无解除）。

---

## A. 实际基线报告

### A.1 三仓状态（开工时核实，非引用旧报告）

| 仓 | 分支 | 开工时 HEAD | 现在 HEAD | 远端同步 |
|---|---|---|---|---|
| 固件 `PIO_Assets/Guo_Feeder_Project` | `wb` | `278878d` | `313e82e`（+2 commit） | 开工时 `origin/main` = `278878d`（0/0） |
| 云端 `Cloudflare_Assets` | `main` | `204212e` | `e1e8b79`（+1 commit） | 开工时 `origin/main` = `204212e`（0/0） |
| EMQX `EMQX_Assets` | `main` | `ec2364f` | `ec2364f`（**未动**） | 干净，无改动 |

### A.2 工作区状态

- 云端：开工时干净；本轮修改 9 文件（+261 / −21）
- EMQX：开工时干净；**全程未改**（无规则、无连接器、无 ACL 变更）
- 固件：开工时有 **1 个未跟踪文件** `docs/evidence/G-4-Stats-2026-10-20/pre-flash-firmware.bin`
  （1.4 MB，sha256 `a7ad2d24…`，为上一轮预烧录镜像，**未入库** —— 本轮归档时一并提交）

### A.3 与旧报告的差异（**必须显式记录**）

| 旧报告说法 | 实际核实结果 |
|---|---|
| 云端 `204212e` 已推送 | ✅ 成立（`origin/main` 一致） |
| 固件 `278878d` 已推送 | ✅ 成立（`origin/main` 一致） |
| `system.stats.counters.cloud_records_sent` 为已知 `S-1` 项 | ✅ 成立，且**本轮实测确认恒 0**（烧录前 `cloud_batch_sent=131` 而 `cloud_records_sent=0`） |
| B-2 的"弱身份（payload 哈希）"可用 | ❌ **不成立**。生产数据显示它会误合并独立事件（见 C.2）⇒ 本轮停用 |

---

## B. 日志可信度报告

### B.1 全链路（以**实际代码**为准，非设计文档）

```
产生             缓存/落盘                     排队/上传                    接收/入库
─────────────────────────────────────────────────────────────────────────────────────
log_emit()       RAM 环 64×128B (PSRAM优)  →  云队列 128 槽 (PSRAM优)  →  MQTT log topic
  │ 合并白名单     │ 满 ⇒ ring_drop++            │ 满 ⇒ FIFO 淘汰 + drop_overflow++
  │ (5 s 窗口)     ▼                             ▼
  │             Flash 段环 16 段×31 条        CBOR 组批(1 条/msg) → cloud_send_log()
  │               WARN+ 才落盘                  │
  │               ↓                            ▼
  └─ DEBUG 丢弃  ACK 前不删段                 等 log_ack（**非** PUBACK）
                                              │ 超时 ⇒ cloud_retx++ → 重发
                                              │ 重试耗尽 ⇒ give_up_batch（不推进水位、登记空洞）
                                              ▼
                                     Pages → Worker /api/history/ingest
                                              │ 解析 → 校验 → INSERT OR IGNORE
                                              │   (device_id, boot_seq, seq) 唯一
                                              │ → 逐条 written[]
                                              ▼
                                     ackPersistedLogs(): written[k] ? ACK
                                                         : 查存在性 → 存在才 ACK
```

证据（代码位置）：

- `src/log/log_manager.cpp`：`log_emit()`（合并/入环）、`log_task()`（routing + 一 append 单元）、
  `cloud_queue_push()`（淘汰记账）、`cloud_collect_batch()`、`cloud_try_send_batch()`、
  `cloud_handle_ack()`（`log_ack_classify` 判 ACCEPT/PARTIAL/DUPLICATE/IGNORE）、`cloud_give_up_batch()`
- `src/cloud/cloud_manager.cpp::cloud_send_log()`：**只**返回"已交给 MQTT"，不代表入库
- `src/history-ingest.js` → `src/log-ack.js::ackPersistedLogs()`

### B.2 各阶段的"确认"语义（逐段划清）

| 阶段 | 事件 | **能**证明什么 | **不能**证明什么 |
|---|---|---|---|
| 设备发出 | `cloud_send_log()` 返回 true | 载荷已交给 esp-mqtt | Broker 收到 / 云端入库 |
| Broker 收 | MQTT PUBACK | Broker 已接收 | 规则匹配 / Action 转发 |
| 设备确认 | 收到 `log_ack{boot,from,to}` | 云端**该行已存在**（唯一键命中或本次新增） | 内容一定一致（`collision` 例外，见下） |
| 云端接收 | HTTP 200 + 四字段体 | 请求被处理 | —— |
| 云端入库 | `written[k]=true` | 本次**新增**一行 | —— |
| 云端重复 | `written[k]=false` 且键存在 | 该键已存在（幂等命中） | 内容相同（需 `diffLogFields` 区分 `same_content` / `collision`） |

★ **纪律**：HTTP 200、Worker invocation success、MQTT PUBACK **任一单独都不构成"已入库"**。
本报告所有"已入库"结论都基于 `log_history` 行数或 `written[k]`。

### B.3 本轮修复的缺陷

| ID | 级别 | 缺陷 | 影响 | 修复 |
|---|---|---|---|---|
| **LOG-1** | **P1（MAJOR）** | `cloud_records_sent` **没有任何自增点**，恒为 0（长期登记为 `S-1`） | §4.3 要求的"设备发出/云端接收/入库"四段对账**缺第一段** ⇒ 无法判断"丢在设备侧还是云端" | `cloud_try_send_batch()` 首次发出时累加（`s_cloud_retry_n==0`）；重发仍走 `cloud_retx` |
| **DEDUP-1** | **P0（BLOCKER）** | `mqtt_messages` 幂等键的"等级 B"（缺 `msg_id` 时用 payload 哈希）会**误合并独立业务事件** | 生产实测：1 696 行无 `msg_id` 中 1 613 行落在 167 个同 payload 分组，去重会**吞掉 1 446 行**（85%）。抽样证明多为独立事件（心跳 472 次/39 天、动作命令 73 次/36 天） | **停用等级 B**：`msg_id` 缺失 ⇒ 返回 `null` ⇒ NULL 键 ⇒ 不去重（宁可多留一行） |
| **ATTR-1** | **P0（BLOCKER）** | `history-ingest` 在"topic 声称 `guo_feeder/…` 但设备段非法"时回落 `clientid` | `log_history` 唯一键含 `device_id` ⇒ 错归属 = 把 A 设备日志写进 B 设备键空间 = **不可恢复** | 新增 `topicClaimsDeviceFamily()`：族已声明即只认 topic，非法 ⇒ 400 拒绝 |
| **ATTR-2** | P2 | `/api/ingest` 只认 `/up`、`/down`，`/log` 落 `direction='unknown'`（实测 876 行） | 无法按方向审计 log 上行；`dedup_key` 的 direction 分量可解释性差 | 补 `log`，与 `TOPIC_DIRECTIONS` 同源 |

### B.4 未覆盖 / 未验证场景（**不构成 PASS**）

| 场景 | 状态 | 说明 |
|---|---|---|
| 队列满（`drop_overflow`）真机触发 | **NOT RUN** | 需长时间离线 + 高日志率；代码路径已在 `G-4` 压力轮验证过（历史），本轮未重复 |
| Flash 段淘汰（`drop_unacked`） | **NOT RUN** | 需 496 条 WARN+ 积压 |
| ACK 丢失端到端 | **部分** | 本轮新固件实测出现 `cloud_ack_timeout=3` / `cloud_retx=3` / `ack_lost=0`（真实超时后成功重发），但**未注入**硬丢失 |
| `boot_seq` 回退导致的整批"假重复" | **未修复（设计接受）** | 见 B.5 |
| Worker 侧 D1 写失败（5xx）下的重投 | **NOT RUN** | 需改生产路由（未授权） |
| B-3 漏记 | **未解决** | 见 F.1 |

### B.5 已接受但必须监测的残余风险

1. **`boot_seq` 回退 ⇒ 整批日志"假重复"被 ACK**
   若设备 `boot_seq` 回退（NVS/元数据损坏），新日志的 `(device_id, boot_seq, seq)` 会撞历史键 ⇒
   `INSERT OR IGNORE` 静默丢弃 ⇒ 查存在 ⇒ **存在即 ACK** ⇒ 设备认为已送达。
   **已有缓解**：设备侧隔离（`seq_failclosed`）+ 服务端水位表（`0005/0006`）+ `collision` 计数与 `log_conflict` 表。
   **未做的**：`LOG_BOOT_WATERMARK` 仍是默认关闭的开关。
   **监测方法**：生产只读查询 `log_conflict` 中 `kind='collision'` 的计数（>0 即告警）。

2. **`log_ack` 的"存在即 ACK"（`H-3-7-a` 已批准）**
   同一键但内容不同时仍 ACK（否则设备无第二条上行通道）。内容差异只进 `log_conflict`，**不重传**。
   ⇒ 结论只能写"键已存在"，不能写"内容已核对一致"。

3. **`mqtt_messages` 自身不可信**
   `G4-R5` B-3 已证它会漏记（`up/result` 96/105 个 ID），且历史上有伪 `device_id`。
   ⇒ **凡以 `mqtt_messages` 为证据的验收必须用串口或设备计数交叉验证**（本轮已遵守：所有日志结论来自 `system.stats` + 串口）。

### B.6 ★ 勘误（`2026-10-21` · 审阅核实后追加，**不改动上文原文**）

> 来源：`G4-Production-Change-Plan-Review-2026-10-20.md` §A.3。以下每条都是**本轮读码/读配置核实**的结果。

| # | 本文原表述 | 实际 | 影响 |
|---|---|---|---|
| B-6-1 | §B.5.1「`LOG_BOOT_WATERMARK` 仍是默认关闭的开关」 | ❌ **已启用**：`wrangler.toml:40` `[vars] LOG_BOOT_WATERMARK = "1"`，由 `362ddfd`（`2026-10-19`）引入 | ①"不得擅自开启"实际是"**不得改动**"；②`log_conflict` / `log_boot_watermark` **可作 C-3 的现成监测源** |
| B-6-2 | §B.1 链路图/§B.2 未区分 `mqtt_messages.timestamp` 语义 | `timestamp` 存设备 `t` 字段，而 `t` **被复用**：普通上行是 `time_get()`（**秒**）；分片 BEGIN 帧是**消息类型**；`log_ack.p.t` 是 `seq_to` | 任何"入库延迟"类计算必须加时间区间过滤 |
| B-6-3 | §B.2「云端入库 `written[k]=true`」隐含可观测 | `/api/history/ingest` **不打印** `{accepted,duplicate,rejected,filtered}`，响应只回 EMQX Action | 对账第三段在**我方不可观测** ⇒ 只能用 `log_history` 增量替代 `accepted` |
| B-6-4 | §B.5.1「`(device_id, boot_seq, seq)` 会撞历史键」的前提是 seq 是本 boot 序号 | `seq` 由 `s_meta_slot_seq + 1` **持久化推进**（跨 boot 连续，真机样例 `boot_seq=94, seq=108289`），与 `log_envelope_contract.md:58`「boot 内序号」不符 | 不得假设"新 boot 从 1 开始"；统计必须按 `boot_seq` 分组 |
| B-6-5 | 未提 | `seq` 在 `log_emit_internal()` 对**所有等级**分配，但 **DEBUG 不上云** ⇒ 云端 `seq` 空间**天然有洞** | **禁止**用"seq 连续"作完整性判据 |

★ 未发现差异、经复核确认无误的部分：§B.3 的四个缺陷描述、§C.1/§C.2 的 B-1/B-2 语义、
§D 的 H-1…H-5 定义与覆盖边界、§E 的烧录与串口数据。

---

## C. G4-R5 修复报告

### C.1 B-1：设备 ID 归属

**实现**（`src/device-registry.js`）

| 规则 | 行为 |
|---|---|
| 正常 `up/down/log` | 3 段、族名逐字、方向白名单、`isValidDeviceId` ⇒ 返回 ID |
| `$tenants/<t>/` 前缀 | **固定剥 2 段**（不模糊切片），再按 3 段校验 |
| 非法 ID / 畸形 topic / 缺 ID 段 | `deviceIdFromTopic` → `null` |
| 解析失败 | `resolveIngestDeviceId` → **哨兵 `unresolved`**（不回落 `http_api`） |
| `clientid` 与 topic 冲突 | **topic 优先**（绝不反向覆盖） |

**B-1b 新增：信任优先级（有序）**

```
① topic 解析成功                     ⇒ topic（reason="topic"）
② topic 声称本族但非法               ⇒ unresolved（reason="topic_conflict"）★ 不允许 clientid 改写
③ topic 不属本族 + clientid 是合法 ID ⇒ clientid（reason="clientid"）
④ 其余                              ⇒ unresolved（topic_missing / topic_unparsable）
```

> 评估结论（任务书 §5.1 要求）：**仍然存在**"仅凭合法字符串格式采信 clientid"的路径，
> 但**仅限 topic 完全不属于本族**时；且 `history-ingest` 域的 `clientid` 兜底
> 因"域判定早于身份解析"实际**不可达**（已在测试中固定该事实）。
> 伪造风险由 EMQX 的 clientid↔凭据绑定承担；**未**在本轮引入额外鉴权。

**`/down` 支持的影响评估**：`history-ingest` 显式收窄为 `["up","log"]`，
`/down` **不会**进入历史入库分支（测试 `B1-5-2` 固定）；`/down` 仅用于 `mqtt_messages` 归属。

**哨兵的下游安全性**：`unresolved` 不通过 `isValidDeviceId` ⇒ 不建档、不刷新 `last_seen`、
不进入退役守卫、不参与按设备聚合（`B1-8-*` 覆盖）。

### C.2 B-2：幂等语义（**核心变更**）

**键公式**

```
base   = source ‖ deviceId ‖ direction ‖ cmd
idPart = "id:" + msgId          （**必需**）
phPart = "ph:" + SHA256(payload)（附加成分）
key    = SHA256(base ‖ idPart ‖ phPart)
```

**强度分级（改后）**

| 等级 | 条件 | 结果 | 理由 |
|---|---|---|---|
| A | `msg_id` 非空 | 产生键（含 payload 哈希） | 业务身份明确；同 ID 不同内容不合并 |
| ~~B~~ | ~~`msg_id` 空、payload 非空~~ | **返回 `null`（停用）** | **payload 不唯一标识业务事件**（见下） |
| C | `msg_id` 缺失 | `null` ⇒ NULL 键 ⇒ 不去重 | 保守 |

**停用等级 B 的生产证据**（`mqtt_messages` 只读统计）

| 观测 | 数值 |
|---|---|
| 无 `msg_id` 行 | 1 696（unknown 876 / up 606 / down 214，约 14.9%） |
| 落在"payload 完全相同"分组内的行 | 1 613（167 组） |
| 若按 payload 去重会删掉 | **1 446 行（85%）** |
| 最大组 | `{"cmd":"system","id":"online","src":"device"}` **472 次**，跨度 **≈39 天** |
| 次大组 | `{"cmd":"execute_action","id":"8010","ob":"COMPUTER_RESET"}` **73 次**，跨度 **≈36 天** |
| 真重复样例 | `{"c":"log_ack","p":{"b":1,"f":261,"t":261}}` **11 次**，跨度 **2 731 ms** |

⇒ 前两类是**独立事件**（心跳 / 复用常量 id 的命令），最后一类才是重复投递。
⇒ **无法用 payload 区分二者** ⇒ 必须停用等级 B。**代价**：缺 `msg_id` 的上行仍可能留重复行（刻意接受）。

**其它检查项**

| 检查 | 结论 |
|---|---|
| 唯一索引与 `ON CONFLICT` 目标一致 | ✅ 均为 `ux_mm_dedup_key(dedup_key) WHERE dedup_key IS NOT NULL` |
| 预期重复 vs 真实 SQL 失败 | ✅ 只用 `ON CONFLICT(dedup_key)`；其它约束照常抛（`B2-7-9/10`、真 D1 `R-7`） |
| `changes=0` 是否对应预期冲突 | ✅ SQL 层保证；`changes` 不可得时**不猜**（记 `inserted` + warn） |
| migration 后曾降级的实例能否恢复幂等 | ⚠️ **是**：降级是模块级 `dedupDisabled` 标志，isolate 重建即复位。**同一 isolate** 内不会自愈（已知、可接受） |
| 并发/部分失败/重复 ACK | ⚠️ **未覆盖**：需真实 D1 并发，登记为 `G4-R5-U-1` |

**audit / echo 语义保留**：`source` 进键 ⇒ 同一业务消息两行共存。
真机与真 D1 均验证（`B2-8-3`、`R-4`）。

### C.3 是否具备进入生产变更阶段

✅ **具备（限 C-1 migration + C-2 Worker 部署）**，前置与回滚见
`docs/architecture/G4-Production-Change-Plan-2026-10-20.md`。

---

## D. G4-R4 观测报告

### D.1 指标定义 / 单位 / 任务归属

| ID | 字段 | 定义 | 单位 | 采样时机 | 任务归属 |
|---|---|---|---|---|---|
| **H-1** | `dram.largest_free_block` / `psram.*` | `ESP.getMaxAllocHeap()` / `getMaxAllocPsram()` | B | 读命令时即时 | 调用者上下文（`mqtt` 或 `loopTask`） |
| **H-2** | `*.minimum_free_since_boot` | `ESP.getMinFreeHeap()` / `getMinFreePsram()` | B | 同上 | 同上 |
| **H-3** | `drift.{dram,psram}.slope_per_hour` | 32 点 × 60 s 窗口的一阶最小二乘斜率 | B/h（`int32`） | 每 60 s 采样；**5 min 预热后**才入环 | `loopTask` |
| **H-4** | `alloc.*` | 显式分配点的三级失败计数 | 次数 / B | 每次 `gf_alloc_*` 调用 | 任意（短临界区） |
| **H-5** | `stack.hwm_raw` / `hwm_bytes` / `used_bytes` | `uxTaskGetStackHighWaterMark(nullptr)` | 原始值 + B | 读命令时即时 | **`loopTask`**（`pcTaskGetName` 核实） |

**H-3 语义细节（避免误读）**
- `valid=false` 原因码：`too_few_samples`（<4 点）/ `span_too_short`（<30 min）
- `valid=false` 时**仍给出** `free_min` / `largest_min` / `free_delta`（**直接测量**，非推断）
- `slope_per_hour` **只在 `valid=true` 时输出**（不给就代表"未知"，不是 0）
- 为什么用拟合而不是首末相减：主机探针 `M-3-2` 证明抖动序列的 `free_delta=0` 会**完全掩盖**抖动幅度（`free_min` 差 400 B）

**H-4 三级计数（区分点）**
- `psram_fail`：PSRAM 尝试失败（**不等于**最终失败）
- `dram_fallback_fail`：回退 DRAM 也失败 ⇒ 该次最终失败
- `final_fail`：逻辑最终失败 = `psram_only` 失败 + `dram_fallback_fail`
- 附带 `last_fail_site` / `last_fail_size` / `last_fail_ms` + 按登记点分解的 `sites{}`

### D.2 覆盖边界（**硬性表述约束**）

- **覆盖**：12 个登记点 / 22 处 `heap_caps_malloc` 调用点（6 个文件）
  `log.ring` / `log.flash_batch` / `log.cloud_q` / `log.cloud_batch` / `log.cbor` / `log.seg_buf` /
  `workflow.instances` / `workflow.steps` / `workflow.storage` / `capability.registry` / `homie.desc` / `config.buf`
- **不覆盖**：Arduino `String`（全仓约 498 处 `malloc/realloc`）、第三方库（含 mbedTLS、LittleFS 缓存、ArduinoJson）、
  FreeRTOS/IDF 自身分配
- ⇒ 允许的表述：**"已登记显式分配点内无失败"**（本机实测 `final_fail=0`）
- ⇒ **禁止**的表述："系统不存在分配失败" / "无内存风险"

该边界以 `alloc.coverage.{scope,excluded_note,claim_limit}` 随数据一起输出（防止下游误读）。

### D.3 阈值纪律

`system.memwatch.thresholds` 全部带 `candidate:true` / `approved:false`，且**不参与任何判定分支**：
`dram_min_free_candidate=32768` / `dram_largest_free_candidate=16384` /
`dram_drift_bytes_per_hour_candidate=-1024` / `stack_used_candidate=12288`。

`G4-R4` 的 `<70%` 仍为**未批准的历史建议值**（`HUMAN_REQUIRED`），本轮**未**把它写进代码。

### D.4 测试证据

- 主机探针 `test/mem_watch_host`（wasm32 freestanding + node）：**40 项全 PASS**
  （M-1 斜率精确性 / M-2 零漂移 / M-3 抖动 / M-4 折线 / M-5 窗口不足 / M-6 直接测量 /
   M-7 DRAM↔PSRAM 隔离 / M-8 栈口径 / M-9 边界 / M-10 满环大跨度）
- 真机：`system.memwatch` 全字段可用；H-3 在 5 min 预热后开始入环（实测 `samples=2, span=60000`）

### D.5 主机探针抓出的**两个自身缺陷**（重要留痕）

| 缺陷 | 表现 | 修正 |
|---|---|---|
| 栈口径判据 `raw > stack ⇒ words` | **数学上永不成立**（两种口径下 raw 都 ≤ stack）⇒ 判据无效却看起来"合理" | 改用 `raw×4 > stack ⇒ 字数不可能 ⇒ 必然是字节`（有效必要条件） |
| 单步深栈探测 | 下降量 = `depth − (当前深度 − 历史最深)`，被历史低点稀释：真机 `probe 4096` ⇒ delta=0；`probe 8192` ⇒ delta=3840（既非 8192 也非 2048） | 改**双步**（`d1`、`d1+2048`），用两次高水位差对 2048 反推 |

### D.6 内存与构建开销（实测）

| 项 | 基线（S-3 `72d7a610`） | 本轮（`94ba711c`） | Δ |
|---|---|---|---|
| RAM | 40.5% = 132 744 B | **41.1% = 134 720 B** | **+1 976 B** |
| Flash | 66.9% = 1 402 305 B | **67.5% = 1 416 605 B** | +14 300 B |

DRAM 增量来源：`s_mw.ring` 640 B + `s_snap` 640 B + 计数/状态约 60 B + 新 TU 的 `.bss` 对齐。
★ 该增量是**内部 RAM**（栈只能内部 RAM），在 73% 占用下每次 +2 KB 都值得记录。

---

## E. 测试与烧录报告

### E.1 测试总表

| 套件 | 结果 | 位置 |
|---|---|---|
| 云端 `run_all`（**24 套件**） | ✅ 全绿 | `docs/evidence/G4-2026-10-20/cloud-run_all.log` |
| B-1 单元 `g4r5_b1_topic_device_id` | ✅ **68/68** | 同上 |
| B-2 单元 `g4r5_b2_dedup_key` | ✅ **49/49** | 同上 |
| 历史入库 逻辑层 `history_ingest` | ✅ **35/35** | 同上 |
| 真 D1：`mqtt_dedup_real_d1` | ✅ **19/19** | `cloud-real_d1-dedup.log` |
| 真 D1：`history_ingest_real_d1` | ✅ **123/123** | 本轮实跑 |
| 固件主机 `test/mem_watch_host` | ✅ **40/40** | `.pio/g4r4mem_host` |
| 固件完整构建 | ✅ RAM 41.1% / Flash 67.5% | `docs/evidence/G4-2026-10-20/` |

**EXPECTED_FAILURE（历史契约，未弱化）**：`redaction` R-6、`mqtt_result_frame` C11（上轮已登记），
legacy 套件 7 项（`P1-6-1` 旧契约）。全部保留原断言语义，未删除、未改写。

### E.2 失败项

- 本轮开发过程中出现并在**提交前**修掉的失败：
  1. `B1-6-11`（契约变更：`topic_unparsable` → `topic_conflict`）—— 已更新并补 3 条新断言
  2. `B2-4-2`（等级 B 停用）—— 已改判并补 3 条证据回归
  3. 主机探针自身索引错误（`put(&s[i], i, …)` 双索引 ⇒ 越界写 `s[2i]`）—— 已修，并因此发现 M-3-2 断言数学上不成立
- 最终状态：**无未修复失败**

### E.3 NOT RUN（明确未做）

| 项 | 原因 |
|---|---|
| 队列满 / 段淘汰压力（真机） | 需长时间离线 + 高日志率，且会污染本轮基线 |
| Worker 5xx 重投 | 需改生产路由（未授权） |
| 硬件在环（阀/电机/称重时间线） | 器件未焊接 |
| 2.5 h 长窗口（`S-3-L3`） | 已裁决延期至预生产阶段；需生产配置 + 5 项前置 |
| 历史行 `dedup_key` 回填 | 不在本轮范围（需单独设计+授权） |
| `0007` 远端应用 / Worker 部署 | **未授权** |

### E.4 固件构建

| 项 | 值 |
|---|---|
| 构建命令 | `PLATFORMIO_BUILD_DIR=.pio/build/g4r4 pio run` |
| 平台 pio | `D:/platformIO/penv/Scripts/pio.exe` |
| 编译耗时 | 全量 214 s / 增量 32–51 s |
| 产物 | `.pio/build/g4r4/esp32-s3-devkitc-1/firmware.bin` |
| 首次产物 | `2987a66f98b362ad6242ad75123c214d9ccf3e3cb1b88c402d2902b96001a31a`（1 416 528 B） |
| **当前产物** | **`94ba711cfda8284087ce9857f632f115819120d211826558a76dd45990267c31`**（1 416 976 B） |
| 可复现性 | ✅ 提交后在**同一树**重新链接，`firmware.bin` 与归档件**逐字节相同**（`2987a66f` 已验证一轮） |
| 归档 | `docs/evidence/G4-2026-10-20/g4r4-target-{2987a66f,94ba711c}.bin` |

### E.5 烧录（**2 次**，第 2 次为修正第 1 次暴露的缺陷）

**烧录前状态（串口，只读，单会话）**

| 项 | 值 |
|---|---|
| 设备 | `COM8` / CH343 / `dev_288485896ce4` |
| 运行固件 | `72d7a610…744c`（S-3），`boot_seq=23`，uptime 20.7 h |
| 回退件 | `docs/evidence/S-3-Flash-2026-10-20/flash-target-72d7a610.bin`，sha256 **实测核对一致** |
| 配置 | `[CFG][I] boot validated`；Workflow 3 条从 Flash BIN 正常加载 |
| DRAM | total 250 604 / free 67 568（used 73.0%）/ largest 57 332 / min 55 180 |
| counters | `cloud_batch_sent=131` **`cloud_records_sent=0`**（缺陷实证）/ ack_ok 100 / drop_* 全 0 |

**烧录方式（关键）**：`esptool write_flash 0x10000 firmware.bin` ⇒ **仅 app 分区**

| 分区 | 偏移 | 是否被写 |
|---|---|---|
| `nvs` | 0x9000 | ❌ 未触碰（设备身份/隔离标志/证据保留） |
| `otadata` | 0xE000 | ❌ 未触碰 |
| `app0` | 0x10000 | ✅ 唯一写入目标 |
| `littlefs` | 0x410000 | ❌ 未触碰（配置 + 日志段保留） |

⇒ 烧录**不会**覆盖配置分区，也**不会**破坏 `/log` 数据（已由 `boot validated` + `total_records` 连续证实）。

| # | 时间 | 产物 sha256 | 结果 |
|---|---|---|---|
| 1 | 2026-10-20 15:0x | `2987a66f…` | ✅ 写入成功、hash verified；`boot_seq 23→24`；**但暴露 D.5 的两个缺陷**（`alloc.calls=0`、`psram.reason=ok`） |
| 2 | 2026-10-20 15:2x | `94ba711c…` | ✅ 写入成功、hash verified；`boot_seq 24→25`；缺陷全部消除 |

**烧录后验证（第 2 次，`boot_seq=25`）**

| 检查 | 结果 |
|---|---|
| 启动 | `[Log] seq: boot_seq=25 base=6145 limit=6400 reliable=1 failclosed=0` · `[CFG][I] boot validated` |
| 配置完整 | ✅ Workflow 3 条加载成功；`/log` `valid_segments=1` `total_records=31` |
| 新命令可用 | ✅ `system.memwatch` 全字段返回（含 `coverage` / `thresholds.candidate`） |
| H-1/H-2 | `largest=55 284` / `min_free=50 580`（TLS 期间从 62 064 降到 50 580 —— 正是 H-2 的价值） |
| H-3 | 5 min 预热后开始入环：`samples=2 span=60 000`，`valid=false reason=too_few_samples`（正确抑制斜率） |
| H-4 | `calls=18 psram_ok=18 psram_fail=0 dram_fail=0 final_fail=0`（开机分配已计入） |
| H-5 | `hwm_raw=9756` / `unit=bytes` / `unit_source=arithmetic_exclusion` / `used_bytes=6 628`（40%） |
| 栈口径实测 | `mw probe` ⇒ `delta=2432`（step 2 048，容差内）⇒ **bytes**，与算术结论**一致** |
| 日志链路 | `emit_total=12` → `cloud_records_sent=4`（**修复生效**）→ `ack_ok=3` / `ack_timeout=3` / `retx=3` / `ack_lost=0` / `drop_*=0` |
| 复位原因 | `power_on`（由 RTS 复位触发，非崩溃） |

**`mw probe` 的副作用（必须记录）**：探测会**永久压低**本次 boot 的 `hwm_raw`
（实测 9 756 → 5 548）。⇒ **`mw probe` 之后读到的栈高水位不再是生产形态值**。
纪律：先采集 H-5 基线，**再**决定是否做探测；报告引用 `used_bytes` 时必须注明是否探测过。

### E.6 恢复方案

| 场景 | 动作 |
|---|---|
| 新固件异常 | `esptool write_flash 0x10000 flash-target-72d7a610.bin`（回退到 S-3，配置与 `/log` 不动） |
| 更早版本 | `rollback-firmware-a2c8a038.bin`（同目录） |
| 元数据/段损坏 | 设备侧已有隔离（`seq_failclosed`）+ 双槽 meta + 段恢复；必要时 `log_evidence_wipe`（**受控**） |

---

## F. 遗留问题与生产变更计划

### F.1 分级清单

| ID | 级别 | 问题 | 阻止试运行？ | 可延期理由 / 观察方法 | 关闭所需证据 | 需单独授权 |
|---|---|---|---|---|---|---|
| **B-3** | **BLOCKER（对"证据完整性"而言）** | `mqtt_messages` 漏记（`up/result` 96/105 个 ID），根因未定 | **不阻止运行**，但**阻止**"以 `mqtt_messages` 作唯一证据的验收" | 可用串口/设备计数交叉验证（本轮已做）；漏记只影响审计表，不影响 `log_history` 与 ACK | EMQX 数据集成/规则/连接器的 matched·failed·retry 计数（当前 API **403**，需控制台截图） | ✅ 需（只读人工取证） |
| **G4-R4 阈值** | MAJOR（门禁） | `<70%` 为未批准的历史建议值 | 不阻止 | 已降级为参考指标；用 `largest_free_block` + `drift` + `final_fail` 作硬指标 | **人工裁决**阈值与判据 | ✅ 需（人工裁决） |
| **`0007` 未应用 / Worker 未部署** | MAJOR（交付） | B-1/B-2 代码在仓但未生效 | 不阻止本地运行 | 本地验收已全绿；生产变更方案已备 | C-1/C-2 执行 + C-3 观察窗口 ≥2 h | ✅ 需 |
| **B-2b 去重率下降** | MINOR（已知代价） | 缺 `msg_id` 的上行仍可能重复 | 不阻止 | 明确取舍（不误合并优先）；`mqtt_messages` 重复不影响 `log_history` | 若将来协议保证 payload 唯一，可重开 | ❌ |
| **`LOG_BOOT_WATERMARK` 默认关闭** | MINOR | `boot_seq` 回退的检测减弱 | 不阻止 | 设备侧隔离 + `collision` 计数仍在 | 生产开启并观察 1 周无 `rewind` | ✅ 需 |
| **`mw probe` 污染 hwm** | MINOR（方法学） | 探测后本次 boot 的 H-5 不再代表生产 | 不阻止 | 记录纪律：先采基线再探测 | 无需关闭（设计使然） | ❌ |
| **`offline_skip` 快速膨胀** | MINOR（观测） | 未连网时每轮 +1（实测 11 s 内 6 065） | 不阻止 | 该计数是"轮数"不是"条数"，语义已在文档标注 | 若要"离线时长"应另设时间累计 | ❌ |
| **`S-3-O3-d` OLED 方案** | MAJOR（待裁） | OLED 真传输时单帧 92–184 ms ⇒ 20 ms 门禁不可能通过 | 阻止**正式门禁**，不阻止试运行 | 4 选 1 待产品/架构决策 | 决策记录 | ✅ 需 |
| **`G4-R5-U-1…U-6`** | MINOR（未覆盖） | 并发同键 / D1 部分失败 / 真重投端到端 / 部署后计数 / 缺 `msg_id` 去重率 | 不阻止 | 生产观察窗口覆盖 | 生产数据 | ❌（随 C-3） |
| **`S-1`（本轮已修）** | ✅ CLOSED | `cloud_records_sent` 恒 0 | — | — | 已上板验证 `=4` | — |
| **`S-3-L3`** | 门禁证据 | 2.5 h 窗口未跑 | 阻止正式门禁 | 已裁决延期 | 生产配置 2 h + 30 min | ✅ 需 |

### F.2 生产变更计划

见 `docs/architecture/G4-Production-Change-Plan-2026-10-20.md`（独立文件，含前置条件、
`0007` 前后验证、部署顺序、**如何确认真的用了新版本**、五个可测比率、失败判据、
回滚的**不对称影响**、观察窗口与停止条件）。

**一句话**：C-1 先建列建索引 → C-2 再部署 Worker → C-3 观察 ≥2 h；
回滚索引**不能**复活已被去重去掉的独立事件。

---

## G. 交付索引

| 文件 | 作用 |
|---|---|
| `docs/architecture/G4-Log-Credibility-and-Observability-Report-2026-10-20.md` | **本文件**（A–G 汇总） |
| `docs/architecture/G4-Production-Change-Plan-2026-10-20.md` | 生产变更申请（未授权，待批） |
| `docs/architecture/G4-Real-Hardware-Acceptance-Checklist.md` | 焊接后真机验收清单（逐项判据） |
| `docs/architecture/G4-R4-R5-Joint-Investigation-Report.md` | 上一轮只读调查（根因与选项） |
| `docs/architecture/G4-R5-B1-B2-Implementation-and-Local-Acceptance-Report.md` | 上一轮 B-1/B-2 实施记录 |
| `docs/architecture/G4-R4-Gate-Revision-Draft-2026-10-20.md` | 门禁修订草案（**未裁决**） |
| `docs/evidence/G4-2026-10-20/` | 本轮证据：测试日志、两次烧录产物、串口会话 |
| `docs/issues/未修复的问题.md` | 问题登记册（本轮新增条目已追加） |
| `HANDOFF.md` / `docs/README.md` | 状态索引（已同步） |
