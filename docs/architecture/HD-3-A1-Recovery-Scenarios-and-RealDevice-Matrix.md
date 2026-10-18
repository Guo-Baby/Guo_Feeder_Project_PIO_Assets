# `HD-3` · A-1：恢复场景定义、失效模式审查与真机验证矩阵

> **状态**：设计 + 本地实现已完成；**真机项全部 `NOT RUN`**（本轮只准备，不执行）
> 依据：`HD-3-A1-B3-Closure-Report.md`（闭环表）· `HD-3-A1-B3-Independent-Review.md`（缺陷来源）
> 代码权威判据：`src/log/log_meta_evidence.h::decide_recovery()`

---

## 1. 场景判定矩阵（**唯一权威表**）

| # | 场景 | NVS 证据 | 日志区（槽/legacy/段） | 首启标记 | 判定 | `boot_seq` | 状态 |
|---|---|---|---|---|---|---|---|
| ① | 真·首启（新机 / 新烧录且 NVS 空） | 无 | 空 | `true` | `PROVISION` | 1 | ✅ 已实现 |
| ② | 普通重启 | 有 | 有 | `false` | `FROM_NVS` | 高水位 + 1 | ✅ |
| ③ | 老固件升级到本固件 | 无 | 有（legacy/段） | `false` | `LEGACY` / `SEGMENT` | 高水位 + 1 | ✅ |
| ④ | **LittleFS 格式化**（`formatOnFail`） | 有 | 空 | `false` | `FROM_NVS` | 高水位 + 1 | ✅（原 `A1-01`） |
| ⑤ | NVS 证据丢失（日志区健在） | 无 | 有 | `false` | `SLOT` / `LEGACY` / `SEGMENT` | 高水位 + 1 | ✅ |
| ⑥ | NVS 证据丢失 **且** 日志区为空 | 无 | 空 | `false` | **`QUARANTINE`** | **不分配** | ✅（原 `A1-02`） |
| ⑦ | **两类存储同时丢失**（整片擦除） | 无 | 空 | `true` | `PROVISION` | 1 | ⚠️ **未关闭风险** |
| ⑧ | 受控恢复出厂（串口 `lqclear`） | 有（清隔离位） | 任意 | 任意 | `FROM_NVS` | **保留 hw + 1** | ✅ |

**⑦ 的纪律（务必原样引用）**：

> 整片擦除后 NVS 与 LittleFS **同时为空**，与"新机"在**设备侧不可区分**。
> 本设计**不**把它认定为"用户主动恢复出厂设置"，只承认"设备侧判据在此失效"。
> 因此 `R-2` 的幂等键复用风险**保持未关闭**，须由 §4 的服务端检测兜底。

**⑧ 的关键细节**：`log_quarantine_clear()` **保留** `boot_hw` / `reserved_hw`，只清隔离位
⇒ 下次开机从"已知最大 `boot_seq`"续推，**不回 1**；且运行期闸门不立即解除
（必须重启后重新判定，避免复用未固化的序号区间）。

---

## 2. 关键路径竞态与依赖审查（复核结果）

| # | 检查项 | 结论 | 依据 |
|---|---|---|---|
| 2.1 | 身份初始化顺序 | ✅ 已修正 | `main.cpp` 中 `device_identity_init()` **上移**到 `log_init()` 之前；否则 `log_set_first_boot()` 拿不到信号。仍早于 `wifi_init` / `cloud_init` |
| 2.2 | 循环依赖 | ✅ 无 | `log_manager.cpp` **不 include** `services/device_identity.h`，只接收注入的 bool ⇒ 分层方向仍是 `main → services/log` |
| 2.3 | 注入遗漏的可见性 | ✅ 可见 | 未调用 `log_set_first_boot()` 时 `seq_init()` 打 `[Log] WARN ... first boot will be quarantined`（安全但不可用，必须被发现） |
| 2.4 | NVS 写入的并发 | ✅ 单飞 | 唯一写入路径 = `seq_init()`（setup 期，无并发）+ `seq_reserve_next()`（由 `s_seq_reserving` 门闩保证单飞，门闩在临界区内翻转）；`quarantine_enter()` 只在 setup 期 |
| 2.5 | NVS 写入阻塞 | ✅ 可接受 | `Preferences` 写发生在 `seq_reserve_next()`，与既有 LittleFS 写同一位置、同一频率（每 256 条日志一次 + 每次开机一次） |
| 2.6 | 序号分配与隔离的次序 | ✅ 正确 | `seq_init` 先判定来源再算 `boot_seq`；`QUARANTINE` 分支**先 return**，不进入 `seq_reserve_next()`，`s_seq_base/limit/last` 全部置 0 |
| 2.7 | 隔离后能否被绕过 | ✅ 否 | `log_emit_internal()` 的 `s_seq_failclosed` 检查位于 `s_ready` 之后、一切状态变更之前；`s_seq_last++` 是唯一分配点 |
| 2.8 | 一个 boot 内 `s_meta_slot` 的正确性 | ✅ | 即使由 NVS 主导恢复，也**总是**先 `meta_read_best()` ⇒ 目标槽恒为"非当前有效槽"，不会覆盖唯一有效槽 |

---

## 3. 失效模式审查（"未知状态不得静默退化为 `BOOT_FRESH`"）

| 失效 | 行为 | 是否静默 | 结论 |
|---|---|---|---|
| NVS 证据写入失败 | `seq_reliable = false` + 串口 `NVS evidence write failed`；槽/legacy 仍是权威 | 否 | ✅ 不改变本次判定，但降级可见 |
| 隔离标记**两份都写失败** | 本次 boot 仍停写；串口 `QUARANTINE: BOTH markers failed -> not durable across reboot!` | **否**（显式告警） | ⚠️ 跨重启不保——这是硬件完全不可写的情形，已登记为残余风险 |
| 元数据**回读校验失败** | `meta_slot_write_raw()` 返回 false ⇒ 提交失败 ⇒ `seq_reliable=false`；目标槽停在 `commit=0`（无效），旧槽仍有效 | 否 | ✅ 不会误认"已提交" |
| `reserved` 饱和 / 序号耗尽 | `align_reserve()` 返回 `LOG_SEQ_EXHAUSTED` 或 `next_boot_seq()` 置 `ok=false` ⇒ **转隔离** | 否（串口 `seq space exhausted`） | ✅ 绝不回绕成 0 |
| 旧固件回滚 | `meta.bin`（`fmt=2`）每次提交原子镜像（临时文件 + rename） | — | ✅ 回滚可读；若镜像失败则 `seq_reliable=false` 留痕 |
| LittleFS 挂载失败（日志区不可写） | 有 NVS 证据 ⇒ `FROM_NVS` 继续推进 `boot_seq`；无证据 ⇒ 按矩阵判定 | 否 | ✅ 不会因为"写不进去"而判全新 |
| `first_boot` 未注入 | 保守按 `false` 处理 ⇒ 无证据即隔离 | 否（WARN） | ✅ 偏向安全 |

---

## 4. `A1-03` 远程可观测性方案（**仅方案，未改协议**）

### 4.1 现状

| 通道 | 是否可见 `seq_failclosed` / 隔离 |
|---|---|
| 串口 `[LogT] meta`（第二行） | ✅ 可见（`failclosed=` / `src=` / `nvs_*`） |
| 开机汇总行 `[Log] seq: … src=… failclosed=…` | ✅ 可见 |
| `LogFlashInfo`（C++ API） | ✅ 可见（新增 6 字段） |
| MQTT 上行 | ❌ **不可见**（`log_ack`/`result` 帧字段冻结；且隔离期间根本不产生日志） |
| 云端 | ❌ **不可见**（该设备日志静默） |

### 4.2 方案对比

| 编号 | 方案 | 需改协议 | 需改设备 | 实时性 | 评价 |
|---|---|---|---|---|---|
| **O-7（推荐）** | **服务端"日志静默"检测**：cron 统计每设备 `MAX(created_at)`，超过 N 天无新增 ⇒ 告警 | **否** | **否** | 天级 | ✅ 零协议变更、零设备改动、复用现有 cron 与数据；缺点是无法区分"隔离"与"设备离线" |
| O-3 | 新增 MQTT 状态 Topic（如 `guo_feeder/<id>/health`） | **是** | 是 | 秒级 | 信号最强，但属协议变更，需单独授权 |
| O-6 | 扩展现有 `system_command` 的 status 响应字段 | **是**（命令响应契约） | 是 | 请求级 | 比 O-3 侵入小，但仍需契约评审 |
| O-4 | 用 `log` 上行 envelope 携带告警 | 否（复用 envelope） | 是 | 不可用 | ❌ 隔离期间**禁止**分配 seq ⇒ 该通道在关键场景**恰好不可用** |

### 4.3 推荐

**先做 O-7**（纯服务端、零协议变更），它能覆盖"设备静默"这一类后果；
若后续需要区分**原因**（隔离 / 离线 / 掉电），再走 O-3 或 O-6 的正式协议评审。

> ⚠️ O-7 当前**未实现**（本轮只提交方案）。它与 §5 的 `boot_seq` 回退检测可共用同一次 cron 扫描。

---

## 5. 服务端 `boot_seq` 回退检测（**候选信号**）

### 5.1 为什么必须放在服务端

见矩阵 ⑦：整片擦除后设备侧无法自证 ⇒ 只能从"已入库的数据"里发现异常。

### 5.2 已完成的本地实现

| 项 | 内容 |
|---|---|
| 实现 | `Cloudflare_Assets/guo-feeder-api/src/log-bootseq-guard.js`（只读，单条 SELECT） |
| 判据 | 同一 `(device_id, boot_seq)` 的行**跨度 ≥ 7 天** 且行数 ≥ 2 ⇒ **候选** |
| 输出 | `console.error("[log-bootseq] … candidates=… detail=… note=BOOT_SEQ_REUSE_CANDIDATE(not proof)")` |
| 接入 | `index.js::scheduled()`，**默认关闭**（`env.LOG_BOOTSEQ_GUARD === "1"` 才运行）⇒ 未启用时**零额外 D1 调用** |
| 协议影响 | **无**（不新增接口/字段/表；不写库） |
| 测试 | `test/current/log_bootseq_guard.test.mjs`（31 项，L1 逻辑层） |
| 状态 | **已本地实现**；生产 `NOT DEPLOYED`；真实数据命中率 **`NOT RUN`** |

### 5.3 判据的局限（必须承认）

- 设备**长期不重启**（> 7 天）同样会命中 ⇒ 只是**候选**，不得自动处置、不得写成"已确认"；
- 输出**不含**任何行内容，只含 `dev / boot_seq / rows / span_days`；
- **精确**判定需要额外状态（二选一，均属后续授权）：

| 精确方案 | 需要 | 成本 |
|---|---|---|
| `device_log_boot_hw(device_id, max_boot_seq, updated_at)` 水位表 | 新表 + migration + ingest 侧 1 次写 | 可在 ingest 时 O(1) 判定 `incoming.boot_seq <= stored.max` |
| 设备侧上报 `recovery=PROVISION` 事件 | 协议字段（同 O-6） | 语义最准，但需协议变更 |

### 5.4 当前结论

> 服务端回退检测 = **仅本地实现 + 未部署 + 未在真实数据上验证**
> ⇒ 矩阵 ⑦ 的风险**保持未关闭**，**不得**标记为 PASS。

---

## 6. 真机验证矩阵（本轮**只准备，不执行**）

> 每项都需要：**设备 + 串口 + 破坏性测试授权**。执行前必须 `pio run` 成功且**已确认**目标板型号。

| # | 场景 | 前置条件 | 步骤 | 预期 | **禁止行为** | 数据恢复方式 | 失败后的安全处置 | 需授权 |
|---|---|---|---|---|---|---|---|---|
| `RD-1` | 正常启动 / 正常恢复 | 已烧录本固件、日志区与 NVS 均有证据 | 连续冷启动 3 次，每次记录 `[Log] seq:` 行 | `recovery=NVS`；`boot_seq` 单调 +1；`reserved` 不下降 | 不得 `fwipe` / `mwipe` / `levwipe` | 无需恢复 | 若 `boot_seq` 回退 ⇒ 立即停测并取证（说明 NVS 未生效） | 烧录 + 串口 |
| `RD-2` | **LittleFS 格式化**（原 `A1-01`） | 至少 1 次正常启动（NVS 有证据） | ① 记录 `boot=N`；② 擦除/格式化 LittleFS 分区（或写坏超级块使 `begin` 触发 `formatOnFail`）；③ 重启 | `recovery=NVS`；`boot_seq = N+1`；**不得**出现 `PROVISION` / `fresh` | **不得**同时擦 NVS | 日志区可从云端回灌（`log_history` 仍在） | 若出现 `PROVISION` ⇒ 立即 `lqclear` 前先停写，保留串口现场 | 破坏性 + 烧录 |
| `RD-3` | **连续两次重启**（原 `A1-02`） | 可复现"段损坏 + 双槽失效" | ① `mstage 2` + 破坏段头 + `mwipe` + `levwipe`；② 重启两次 | 第 1 次 `recovery=QUARANTINE`；第 2 次**仍** `QUARANTINE`（`failclosed=1`） | 不得在第 2 次重启前 `lqclear` | 需人工判读后 `lqclear`（保留 hw） | 若第 2 次变 `PROVISION` ⇒ **阻断级回归**，立即停测 | 破坏性 |
| `RD-4` | **NVS 擦除**（证据丢失，日志区健在） | 有槽/段数据 | ① `levwipe`（删 NVS 证据 + 隔离文件）；② 重启 | `recovery=SLOT`（或 LEGACY/SEGMENT）；`boot_seq` 继续 +1 | 不得同时删日志区 | 槽/段仍在，无需恢复 | 若判 `QUARANTINE` ⇒ 说明槽/段也失效，按 `RD-3` 处理 | 破坏性 |
| `RD-5` | **元数据半写**（两阶段中断） | — | ① `mstage 2`；② 触发一次 `seq` 预留（写 ≥256 条或改 `LOG_SEQ_RESERVE` 不可行 ⇒ 用 `logt` 触发）；③ 重启 | 旧槽仍有效；`boot_seq` **不回退**；串口有 `seq reservation failed` | 不得 `fwipe` | 旧槽即最后一次成功值 | 若回退 ⇒ 判为 `A1-05` 回归 | 破坏性 |
| `RD-6` | **写后回读异常** | — | ① `mstage 3`；② 触发预留 | 提交失败 ⇒ `seq_reliable=0`；下次预留重试成功 | 不得 `mwipe` | 槽未受损（回读失败发生在写入后，旧槽仍有效） | 连续 3 次仍失败 ⇒ 采集串口 + `logt meta` 双行 | 破坏性 |
| `RD-7` | **断电恢复** | 可控电源（继电器/拔插） | 在 `seq` 预留窗口内随机断电 ≥ 20 次，每次重启记录 | 无 `boot_seq` 回退；无重复 `(boot,seq)`；`reserved` 单调 | 不得在写入过程中手动格式化 | 无（断电安全是目标本身） | 出现任一回退 ⇒ 停止并保存 `logt meta` + 云端 `boot_seq` 分布 | 破坏性 + 断电 |
| `RD-8` | **整片擦除**（矩阵 ⑦） | — | ① 记录云端该设备 `MAX(boot_seq)`；② `erase_flash` + 重烧；③ 首次启动 | **预期会** `PROVISION` / `boot_seq=1` ⇒ 这是**已知未关闭风险**，用于**取证**而非验收 | 不得在取证前清空云端 | 云端历史保留（不回灌） | 取证后必须记录"服务端检测是否命中"（§5 未部署 ⇒ 预计**不命中**） | 破坏性 + 烧录 + 云端只读 |

**通用数据保护**：

1. 执行前导出云端该设备的 `log_history` 摘要（`boot_seq` 分布、行数、时间范围）作为对照基线；
2. 全程录串口日志到文件（含 `[Log] recovery=` / `[Log] seq:` / `[LogT] meta` 两行）；
3. **禁止**在真机测试中改动 `migrations`、部署 Worker、或修改 EMQX 规则；
4. 任一场景出现"`boot_seq` 回退"或"`recovery=PROVISION`（非 `RD-8`）"⇒ **立即停测**，按对应条目取证，不得继续。

---

## 7. 未关闭风险（明确保留）

| # | 风险 | 状态 | 兜底 |
|---|---|---|---|
| 1 | 整片擦除后幂等键复用（矩阵 ⑦） | **未关闭** | 服务端候选检测（已实现，**未部署、未验证**） |
| 2 | 隔离标记两份都写失败 ⇒ 跨重启不保 | **残余**（硬件完全不可写） | 显式串口告警；无自动兜底 |
| 3 | `A1-03` 远程可观测 | **仅串口** | O-7 方案（**未实现**） |
| 4 | 真机断电 / 格式化 / NVS 擦除 | **`NOT RUN`** | `RD-1`…`RD-8` 矩阵（待授权） |
