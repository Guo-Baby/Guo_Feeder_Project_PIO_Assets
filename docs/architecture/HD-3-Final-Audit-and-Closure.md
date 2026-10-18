# HD-3 收尾审计：A-1 / boot_seq 水位 / log_conflict / Retention

> ⚠️ **状态更新（`2026-10-19 02:25` 追加，不改正文）**
> 本文是 **`2026-10-19 00:57` 的时点快照**。其后已发生（见 `HD-3-RealDevice-Verification-and-Release.md`）：
> ① `0004` / `0005` **已应用**到生产 D1；② Worker **已部署**（`508461f3-…`，前版 `72023596-…`）；
> ③ `LOG_BOOT_WATERMARK="1"` **已启用**；④ RD-1/2/3/7a/8 与 `mstage 2` **真机已跑**。
> ⇒ 下文所有 **`NOT APPLIED` / `NOT DEPLOYED` / 未部署** 描述**仅代表写作时点**，**不得**作为当前状态引用；
> 物理断电仍为 `NOT RUN`；门禁 `BT-1 OPEN` / `G-4 BLOCKED` / `WP-5` 未解锁 / `STOP-R1` 未解除 **均未变**。

> 日期：`2026-10-19` · 仓库基线：固件 `3acf91c` · 云端 `a3473eb`（**均未提交本轮改动**）
> 前序报告：`HD-3-Remaining-Closure-Report.md`（`2026-10-18`）· `HD-3-A1-Recovery-Scenarios-and-RealDevice-Matrix.md`
> **本文件为新一轮独立审计结果，不覆盖前序任何报告。**

---

## A. 总体结论

| 事项 | 结论 | 依据 |
|---|---|---|
| **事项 1 · A-1 隔离失败路径** | **`CONDITIONAL`**（本地闭环，真机未验证） | `A1-02` 残留（证据自毁）已从代码层消除；"三份标记全失败"仍是**明确标注的未闭环边界** |
| **事项 2 · boot_seq 水位** | **`IMPLEMENTED / DEFAULT-OFF / NOT DEPLOYED`** | 新表草案 + 新模块 + L1 48 PASS + 真实 D1 43 PASS；**未应用 migration、未部署、未启用** |
| **事项 3 · log_conflict 失败路径/容量** | **`LOCAL DONE / NOT APPLIED`**（本轮加固 3 项） | 指纹 v2（128 bit + 长度前缀）· 批内聚合 · 表缺失熔断；真实 D1 54 PASS |
| **事项 4 · Retention W7-G2** | **`FIXED`** | 判据改为"基线增量"；**重跑 106 PASS / 0 FAIL（原 105/1）** |
| **整体 HD-3** | **`CONDITIONAL`** | 本地实施阶段可关闭；系统级关闭仍需真机 + 生产授权 |

**本地开发任务是否可关闭**：**可以（仅限"本地实施 + 本地可验证"范围）**。
不得据此宣称 HD-3 系统级关闭 —— 真机断电/格式化/NVS 擦除、生产 D1、Worker 部署、migration 应用**全部未执行**。

---

## B. 逐项结果

### B.1 事项 1 —— A-1 隔离失败路径

#### 原始问题
`A1-02`（上一轮）：`BOOT_UNRECOVERABLE` 只置 RAM 标志；而同一 boot 内
`flash_drop_broken_segment()` 已 `LittleFS.remove()` 掉损坏段 ⇒ **第二次启动**
`scan_corrupt=0 + total_records=0` ⇒ 可能退化为 `REC_PROVISION` ⇒ `boot_seq=1` ⇒ 撞键。

#### 审查发现（本轮新增）

| 编号 | 严重性 | 发现 |
|---|---|---|
| **A1-10** | `MAJOR` | `flash_drop_broken_segment()` **先删段、后无任何跨重启记录** ⇒ 损坏证据在下一次启动归零（`A1-02` 的真正残留） |
| **A1-11** | `MINOR` | `quarantine_write_file()` 用 `open(path,"w")` **直接截断**目标 ⇒ 掉电在半写 ⇒ CRC 失败 ⇒ 隔离标记丢失（只剩 NVS 一份） |
| **A1-12** | `MAJOR` | 隔离两份标记（NVS + 文件）**同时失败**时，只有一行串口告警，**没有任何持久痕迹** |

#### 实际修改

| 文件 | 改动 |
|---|---|
| `src/log/log_meta_evidence.h` | 新增**损坏痕迹**纯逻辑：`pack_trace` / `parse_trace` / `trace_add`（饱和）/ `merge_corrupt`（取大）+ 头部第五条线索说明 |
| `src/log/log_manager.cpp` | 新增 `corrupt_trace_read(_path)` / `corrupt_trace_write_raw` / `corrupt_trace_bump`（写临时文件 → **回读校验** → `rename`）；`flash_drop_broken_segment()` **先固化痕迹再删段**；`flash_scan_all()` 开头与痕迹**取大**；`quarantine_write_file()` 改为 **临时文件 + 回读 + rename**（`A1-11`）；`quarantine_enter()` 双失败时 `corrupt_trace_bump(SEG_NONE, …)`（`A1-12`） |
| `src/log/log_manager.h` | `LogFlashInfo` 追加 `trace_corrupt` / `trace_last_seg`（诊断面） |
| `src/main.cpp` | `logt_print_meta` 增加一行 `[LogT] trace corrupt=… last_seg=…` |
| `test/log_contract/probe_meta_evidence.cpp` | 新增 **E11a–E11t（20 项）**：痕迹往返/CRC/magic/饱和/合并取大 + **跨重启断言**（`E11o`：段已删 + 痕迹=2 ⇒ `QUARANTINE`；`E11r`：隔离标记无法固化 ⇒ 下次仍隔离） |

#### 关键设计点（为什么现在能"跨重启"）

```
B1：发现 2 个坏段 ⇒ scan_corrupt=2 → 写 trace(count=2) → 删段 → 判 QUARANTINE
    └ 若隔离标记两份都失败 → 再 bump trace(count=3, seg=NONE)
B2：坏段已不存在 ⇒ scan_corrupt=0
    └ flash_scan_all() 读 trace=3 ⇒ merge_corrupt(0,3)=3 ⇒ **仍 QUARANTINE**
    └ 不再出现 "scan=0 + recs=0 ⇒ PROVISION ⇒ boot_seq=1"
```

**证据自毁链已被切断**：损坏痕迹独立于段文件；且"隔离无法固化"本身也会留下痕迹。

#### 未解决 / 未覆盖（**不得**表述为已闭环）

1. **三份标记全失败**（NVS 不可写 + `quarantine.bin` 不可写 + `corrupt.trace` 不可写）
   ⇒ 设备侧**没有任何跨重启证据** ⇒ 只能靠服务侧水位。已在头文件与串口告警中显式标注。
2. **整片擦除**（NVS + LittleFS 同时丢）⇒ 与"新机"本地不可区分 ⇒ 仍为未关闭风险（矩阵 ⑦）。
3. 所有结论均为**代码 + wasm 纯逻辑探针**证据；**真机断电/半写未验证**（见 §C.5）。

#### 测试
- `run_contract_test.py` **8/8 步 ALL PASS**；`probe_meta_slot` **32 项** · `probe_meta_evidence` **83 项**（原 63 + E11 20）
- `pio run` **SUCCESS** · RAM 40.5%（无变化）· Flash 66.8%（+约 1.3 KB）

---

### B.2 事项 2 —— 独立 boot_seq 水位与回退检测

#### 原始问题
`log-bootseq-guard.js` 用 `log_history` 的 `created_at` 跨度 ≥7 天作为回退依据 ——
① 依赖"行真的插进去了"（撞键被 `INSERT OR IGNORE` 丢弃时**零证据**）；
② 阈值既是漏报来源也是误报来源。

#### 审查发现

| 编号 | 严重性 | 发现 |
|---|---|---|
| **A1-13** | `MAJOR` | 崩溃/撞键场景下 `log_history` 无行 ⇒ **水位无法推进** ⇒ 回退检测在最需要时失效 |
| **A1-14** | `MINOR` | 单一"7 天跨度"阈值把"低频设备合法长跑"与"真回退"混为一谈 |

#### 实际修改（新增，未部署）

| 文件 | 说明 |
|---|---|
| `migrations/drafts/0005_log_boot_watermark.sql` | **草案**：`log_boot_watermark(device_id PK, max_boot_seq, max_seq_at_max, first_seen_at, last_seen_at, obs_count, rewind_count, last_rewind_*， status)` + 2 索引。位于 `drafts/` ⇒ `migrations apply` **不拾取** |
| `src/log-boot-watermark.js` | `toInt`（严格数值）· `analyzeBatch`（纯函数四态）· `LOG_BOOT_WM_UPSERT`（`MAX()` 单调 + `excluded.seen_count` 式累加）· `recordBootWatermark` · 表缺失熔断 · 摘要日志 |
| `src/history-ingest.js` | 在 **`persist()` 之前**接入（`LOG_BOOT_WATERMARK === "1"` 才运行） |
| `test/current/log_boot_watermark.test.mjs` | **48 项**（纯逻辑 + 失败路径 + 熔断 + 多设备 + 上限） |
| `test/real_d1/log_boot_watermark_real_d1.sh` | **真实 D1 · 43 PASS / 0 FAIL** |

#### 设计要点（逐条对应任务要求）

| 要求 | 实现 |
|---|---|
| 水位独立于去重结果 | 从 **payload** 取 `boot_seq`/`seq`，在 `INSERT OR IGNORE` **之前**写入 ⇒ 实测 `W-8`：`duplicate=1 / accepted=0 / log_history` 行数不变，**`obs_count` 仍 +1** |
| 区分正常递增/同 boot/迟到/乱序/回退 | `analyzeBatch` 四态：`init` / `advance` / `same` / `rewind_only`；批内混合（新+旧）⇒ `advance` 且 `rewind>0` |
| 不把迟到当回退、不降低水位 | 一律输出 `REWIND_CANDIDATE(not proof)`；SQL 用 `max_boot_seq = MAX(...)` ⇒ **只升不降** |
| 不用"跨度 7 天" | 该启发式仅保留在**旧模块**（`log-bootseq-guard.js`，默认关闭）；新水位不再依赖它 |
| 并发 | `MAX()` + 累加表达 ⇒ 乱序落库也不会降水位；`rewind_count` 会因重复投递放大（**观测计数**，已在模块头与报告中声明，不当精确指标） |
| 失败处理 | 读失败 ⇒ `failed`；写失败 ⇒ `failed`；表缺失 ⇒ **熔断**（本 isolate 不再尝试）；**任何失败都不改变 HTTP 响应** |
| 不删除/改写/拒绝原始日志 | 只 `INSERT ... ON CONFLICT DO UPDATE` 到独立表；不触碰 `log_history` |
| 默认关闭 | 未设 `LOG_BOOT_WATERMARK=1` ⇒ **零额外 D1 调用**；`wrangler.toml` / cron 未改 |

#### 检测保证 vs 检测盲区（必须同时读）

**能保证**：一旦某个 `boot_seq` 被上报过，水位就不会因迟到/乱序下降；后续出现更小的
`boot_seq` 一定被计为 `rewind` 候选（即使该批日志全部撞键被丢弃）。

**不能保证**（明确盲区）：
1. **无法区分**迟到 / 乱序 / 真回退 ⇒ 只出候选，需人工用设备侧 `[Log] recovery=` 交叉确认。
2. **迟到会污染计数**（`rewind_count` 是观测计数，非事件数）。
3. 水位表**首次建立前**的历史回退不可见（无基线）。
4. 若设备同时改变 `device_id`（换芯片/MAC 改写）⇒ 视为新设备，不触发候选。
5. **未部署** ⇒ 生产当前**没有**任何回退检测。

#### 测试
- L1 `log_boot_watermark` **48 PASS / 0 FAIL**
- 真实 D1 `log_boot_watermark_real_d1.sh` **43 PASS / 0 FAIL**（首次 / 递增 / 同 boot / 回退 / 迟到 / 撞键 / 表缺失 / 契约四字段）

---

### B.3 事项 3 —— log_conflict 失败路径、容量与保留

#### 原始问题
`H-3-7-b` 已实现"ACK 后落库"，但失败路径、指纹强度、批内重复、表未应用时的成本未审计。

#### 审查发现

| 编号 | 严重性 | 发现 | 处置 |
|---|---|---|---|
| **B3-07** | `MAJOR` | 指纹规范化 `key + ":" + value + "\u0001"` **无长度前缀** ⇒ `["a","b:c"]` 与 `["a:b","c"]` 拼出同一串（v1 可构造碰撞） | **已修**：长度前缀 + 16 hex → **32 hex（128 bit）** |
| **B3-08** | `MAJOR` | 20 条上限作用在**事件数**上；设备重投整段时同键重复会挤掉其他键 ⇒ **被截断的冲突永久无证据** | **已修**：批内按 `conflict_key` 聚合，截断作用在**唯一键**；`seen_count` 用 `excluded.seen_count` 一次加到位 |
| **B3-09** | `MINOR` | 迁移未应用时每批都尝试 `batch` 并失败 ⇒ 白耗 D1 调用 + 刷错误 | **已修**：`no such table` ⇒ 熔断（isolate 内不再尝试，重启即自动恢复） |
| **B3-10** | `INFO` | ACK 之后落库 ⇒ D1 失败时该批冲突**只有日志、无持久证据** | **不修**（协议边界）：ACK 已发出、设备不再重投；已标为 `EVIDENCE LOST ON D1 FAILURE` |

#### 实际修改

| 文件 | 改动 |
|---|---|
| `src/log-conflict.js` | 指纹 v2（长度前缀 + 4×FNV-1a → 32 hex）· `LOG_CONFLICT_UPSERT` 改 `VALUES(..., ?)` + `seen_count + excluded.seen_count` · 批内聚合 · 唯一键截断 · 表缺失熔断 · `__resetConflictWarned()` 同时重置熔断 · 头注释补"不得声称绝对无碰撞"与失败边界 |
| `src/log-ack.js` | summary 追加 `conflict_unique`（唯一键数） |
| `migrations/drafts/0004_log_conflict.sql` | 注释更新（32 hex · `excluded.seen_count` 语义 · 容量/保留建议 · **不创建任何 Cron**） |
| `test/current/log_conflict.test.mjs` | A10（拼接歧义回归）· D1/D1b（聚合与 `seen_count`）· D3b（熔断）· D4b（唯一键截断零丢弃）⇒ **54 PASS** |
| `test/real_d1/log_conflict_real_d1.sh` | 指纹长度断言 16 → 32 ⇒ **54 PASS** |

#### `seen_count` / 时间戳幂等语义（明确写死）

| 字段 | 语义 |
|---|---|
| `conflict_key` | `fingerprint(dev, boot, seq, kind, diff_fields, existing_fp, incoming_fp)` ⇒ 同一冲突恒同键 |
| `seen_count` | **首次插入 = 批内该键出现次数**；冲突时 `+= excluded.seen_count` ⇒ 等价于"逐次 +1"，但语句更少 |
| `first_seen_at` | 仅插入时写入，**永不更新** |
| `last_seen_at` | 每次命中刷新 |
| `status` | 人工处置（`open/acked/resolved`），代码**不自动改写** |

#### 容量与保留（量化边界）

| 项 | 值 / 说明 |
|---|---|
| 行数上界 | **= 唯一 `conflict_key` 数**（不是事件数）—— 反复重投只增 `seen_count` |
| 单行字节 | 约 250–300 B（两个 32 hex 指纹 + 文本字段） |
| 单请求上限 | 20 个**唯一键**（旧实现为 20 个事件） |
| 建议保留 | `status='open'` 且 `last_seen_at` 超 **180 天**（与 `log_history` 一致）⇒ 归档/删除 |
| 建议告警 | `seen_count > 10000` 或 `rewind_count > 0` 且未处置 |
| 实际增长率 | **无法估算** —— 缺"设备序号回绕发生率"这一生产输入 ⇒ **不编造数字** |
| 本轮是否建 Cron | **否** |

#### 审计能力声明（**不得**越界）

- 冲突**已落库**时：可审计（最小证据 = 差异字段名 + 两侧指纹，无 payload 原文）。
- D1 写失败时：**该批冲突不可事后审计**（只有 `[log-ack] summary` 计数 + 一行 `[log-conflict]` 告警）。
- 指纹是**概率**去重（128 bit），**不是**密码学保证；不得声称绝对无碰撞。

---

### B.4 事项 4 —— Retention 测试守卫（W7-G2）

#### 原始问题
`W7-G2` 直接断言"受保护文件 `git status --porcelain` 命中数 = 0"，而工作区**本来**就有
未提交成果 ⇒ **必然假阳性**；且修复方式不能是"stash/回滚他人改动"。

#### 审查发现
失败原因**不是**测试逻辑或实现缺陷，而是**判据选错**：把"工作区干净"当成"没被本次测试改动"。

#### 实际修改
`test/real_d1/retention_real_d1.sh`：
- 脚本启动（`mkdir -p "$SCRATCH"` 之后、任何测试动作之前）记录受保护文件的 porcelain **基线**到 `$SCRATCH/g2_baseline.txt`；
- `W7-G2` 改为 `diff -u 基线 当前 | grep -cE '^[+-][^+-]'` ⇒ **只度量本次测试期间产生的增量**；
- 失败时打印**增量明细**；
- `W7-G2a`（新增代码行不含 RET-1 令牌）**原样保留**作为附加越界判据。

**未做**（任务禁止项，逐条确认）：未忽略 dirty files、未扩大排除名单、未硬编码本轮文件名、未删除任何断言。

#### 守卫有效性自检（独立构造）

| 场景 | 检出数 | 结论 |
|---|---|---|
| 无变化 | `0` | 不误报 |
| 新增一处改动（模拟） | `1` | **能抓到** |
| 删除一处改动（模拟） | `1` | **能抓到** |

#### 修复前后对照（同一脚本、同一工作区）

| | 结果 | W7-G2 |
|---|---|---|
| 修复前（`2026-10-18`） | 105 PASS / **1 FAIL** | `FAIL`（期望 0，实际 2） |
| **修复后（本轮实跑）** | **106 PASS / 0 FAIL · EXIT=0** | **`PASS`（增量 = 0）** |

> 注：本轮第一次重跑曾因**在脚本运行期间修改该脚本**导致 `syntax error`（`EXIT=2`）。
> 该次运行**作废**，已在新目录重新完整执行（不修改脚本）后取得 106/0。此失败**与实现无关**，如实记录在此。

---

## C. 证据与风险

### C.1 失败路径与跨重启证据

| 场景 | 覆盖方式 | 结论 |
|---|---|---|
| 单槽损坏 / 双槽损坏 | `probe_meta_slot`（M 组 32 项） | 选槽回退正确 |
| 隔离标记 NVS 写失败、文件写成功 | `quarantine_enter` 双份独立 | 文件一份可保 |
| 隔离标记文件写失败、NVS 成功 | 同上 | NVS 一份可保 |
| **两份都失败** | **新增 trace 第三条**（`A1-12`） | 若 trace 可写 ⇒ 下次仍隔离（`E11r`） |
| **三份都失败** | 仅串口告警 + 显式文案 | **未闭环**（已标注），靠服务侧水位 |
| 损坏段被删除后的第二次启动 | `merge_corrupt` + `E11o/E11n` | **不再退化为 PROVISION** |
| 隔离标记半写（掉电） | 临时文件 + 回读 + rename（`A1-11`） | 不产生半写第三态（`rename` 仍是 unlink+rename，非严格原子） |

### C.2 水位检测的误报 / 漏报 / 并发边界

- **误报**：迟到批次、乱序投递 ⇒ 均计 `rewind`（**设计如此**，不自动处置）。
- **漏报**：水位首次建立前的历史；设备身份变化（视为新设备）；表未部署时**完全没有检测**。
- **并发**：水位单调安全（`MAX`）；`rewind_count` 可能被重复投递放大。
- **成本**：启用后每批 1 次 SELECT + 1 次 batch（与行数无关）；未启用 **0**。

### C.3 log_conflict 持久化失败 / 容量 / 保留

- ACK 后 D1 失败 ⇒ 该批**无持久证据**（`EVIDENCE LOST ON D1 FAILURE`）。
- 容量上界 = 唯一冲突键数；具体增长率**无生产数据**，不估算。
- 保留策略为**建议**（180 天），**未实施**，未建 Cron。

### C.4 Retention 守卫

见 §B.4：修复前 105/1 → 修复后 **106/0**；守卫检出能力已独立自证。

### C.5 未执行（明确保留，不以推断替代）

| 项 | 状态 | 原因 |
|---|---|---|
| 真机断电（≥20 次） | **`NOT RUN`** | 需设备 + 破坏性授权 |
| 真机 LittleFS 格式化后重启 | **`NOT RUN`** | 同上 |
| 真机 NVS 擦除 / 整片擦除 | **`NOT RUN`** | 同上 |
| 真机元数据半写 / 回读异常 | **`NOT RUN`** | 需故障注入夹具授权 |
| 生产 D1 查询/写入 | **`NOT RUN`** | 未授权 |
| Worker / Pages 部署 | **`NOT RUN`** | 未授权 |
| `0004` / `0005` migration 应用 | **`NOT RUN`** | 草案，禁止应用 |
| `LOG_BOOT_WATERMARK=1` 生产启用 | **`NOT RUN`** | 未授权 |
| 生产容量/增长率实测 | **`NOT RUN`** | 无数据源 |

---

## D. 工作区完整性

### D.1 HEAD 前后

| 仓库 | 开始时 | 结束时 | 变化 |
|---|---|---|---|
| 固件 `PIO_Assets/Guo_Feeder_Project` | `3acf91c` | `3acf91c` | 无 |
| 云端 `Cloudflare_Assets` | `a3473eb` | `a3473eb` | 无 |

### D.2 本轮新增文件

| 仓库 | 路径 | 说明 |
|---|---|---|
| 云端 | `guo-feeder-api/src/log-boot-watermark.js` | 水位实现 |
| 云端 | `guo-feeder-api/migrations/drafts/0005_log_boot_watermark.sql` | 草案（未应用） |
| 云端 | `guo-feeder-api/test/current/log_boot_watermark.test.mjs` | L1 · 48 项 |
| 云端 | `guo-feeder-api/test/real_d1/log_boot_watermark_real_d1.sh` | 真实 D1 · 43 项 |
| 固件 | `docs/architecture/HD-3-Final-Audit-and-Closure.md` | 本报告 |

### D.3 本轮修改文件

| 仓库 | 路径 | 改动性质 |
|---|---|---|
| 固件 | `src/log/log_meta_evidence.h` | 追加损坏痕迹纯逻辑 + 边界说明 |
| 固件 | `src/log/log_manager.cpp` | trace 读写 / scan 合并 / quarantine 原子化 / 双失败兜底 |
| 固件 | `src/log/log_manager.h` | `LogFlashInfo` +2 字段 |
| 固件 | `src/main.cpp` | `[LogT] trace` 打印 |
| 固件 | `test/log_contract/probe_meta_evidence.cpp` | E11（20 项） |
| 固件 | `test/log_contract/run_contract_test.py` | 第 8 步描述 |
| 云端 | `src/log-conflict.js` | 指纹 v2 / 聚合 / 熔断 |
| 云端 | `src/log-ack.js` | summary `conflict_unique` |
| 云端 | `src/history-ingest.js` | 水位接入（默认关闭） |
| 云端 | `migrations/drafts/0004_log_conflict.sql` | 注释/保留策略 |
| 云端 | `test/current/log_conflict.test.mjs` | 新增/修正断言 |
| 云端 | `test/real_d1/log_conflict_real_d1.sh` | 指纹长度断言 |
| 云端 | `test/real_d1/retention_real_d1.sh` | **W7-G2 基线判据** |
| 云端 | `test/run_all.mjs` | 注册水位套件 |

### D.4 既有未提交修改保护

| 项 | 状态 |
|---|---|
| 固件仓原有 5 个 `M` 文档（`HANDOFF.md` / `docs/README.md` / 两篇 P3 设计 / `issues` 单） | **原样保留**，未触碰 |
| 固件仓 `cloud_protocol.md`（H-3-7-c 改动） | 保留 |
| 云端仓 `history-ingest.js`（前序 B-3 注释） | 保留并叠加本轮改动 |
| 其他任务遗留文件 | 未被覆盖、删除、重排 |

### D.5 未执行的禁止操作（逐条确认）

- ❌ 无 `git reset` / `clean` / `stash` / `checkout` / 切分支 / rebase / merge / cherry-pick
- ❌ 无 `git commit` / `push`
- ❌ 无生产 D1 查询或写入；无 Worker / Pages 部署
- ❌ 未应用任何 migration（`0001–0003` 未改；`0004/0005` 仅草案）
- ❌ 未改 EMQX / Cron / 生产环境变量 / `wrangler.toml`
- ❌ 未发送设备命令、未重启、未烧录、未 OTA
- ✅ 门禁保持：`BT-1 OPEN` · `G-4 BLOCKED` · `WP-5` 未解锁 · `STOP-R1` 未解除
- ✅ RET-1 维持 `ACCEPTED WITH KNOWN LIMITATIONS`，未重开、未修改

---

## E. 测试证据（完整）

| # | 命令 | 类型 | 结果 |
|---|---|---|---|
| 1 | `python test/log_contract/run_contract_test.py` | wasm 真执行 | **ALL PASS（8/8 步）** · `probe_meta_slot` **32** · `probe_meta_evidence` **83** |
| 2 | `pio run -e esp32-s3-devkitc-1` | 编译 | **SUCCESS** · RAM 40.5% · Flash 66.8%（+约 1.3 KB） |
| 3 | `node test/run_all.mjs` | L1 全量 | **19/19 exit=0** |
| 4 | `node test/current/log_ack_collision.test.mjs` | L1 | **61 PASS / 0 FAIL** |
| 5 | `node test/current/log_conflict.test.mjs` | L1 | **54 PASS / 0 FAIL**（原 50） |
| 6 | `node test/current/log_boot_watermark.test.mjs` | L1 | **48 PASS / 0 FAIL**（新增） |
| 7 | `node test/current/log_bootseq_guard.test.mjs` | L1 | **31 PASS / 0 FAIL** |
| 8 | `bash test/real_d1/retention_real_d1.sh` | 真实 D1 | **106 PASS / 0 FAIL · EXIT=0**（原 105/1） |
| 9 | `bash test/real_d1/b3_collision_real_d1.sh` | 真实 D1 | **53 PASS / 0 FAIL · EXIT=0** |
| 10 | `bash test/real_d1/log_conflict_real_d1.sh` | 真实 D1 | **54 PASS / 0 FAIL · EXIT=0** |
| 11 | `bash test/real_d1/log_boot_watermark_real_d1.sh` | 真实 D1 | **43 PASS / 0 FAIL · EXIT=0**（新增） |

### E.1 失败历史（保留，不隐藏）

| 轮次 | 失败 | 归因 | 修复方式 |
|---|---|---|---|
| 水位 real_d1 第 1 次 | `W-3f advances>=1` | **夹具缺陷**：断言检查的是 W3b（同 boot）的摘要，`advance` 本应为 0 | 改断言位置（W-3f 改为 `devices=1`；`advances>=1` 移到 W-4d）；**实现未改、断言未放宽** |
| 冲突 L1 第 1 次 | `D4b` | 我写错了期望（聚合后 `skipped=0` 才是正确语义） | 修正期望为 `20 语句 / skipped=0 / written=22` |
| 水位 L1 第 1 次 | `A20/A21/C10` | **实现缺陷**：`Number(null)===0` 把非法值当合法；`devices` 在截断前计数 | 增加严格 `toInt`；`devices` 移到截断后 |
| retention 第 1 次 | `EXIT=2 syntax error` | **操作失误**：在该脚本运行期间修改它 | 该次作废；新目录完整重跑 → 106/0 |
| retention（前序） | `W7-G2` | 判据错误（工作区干净 vs 基线增量） | 已修（§B.4） |

---

## F. 最终门禁（逐条回答）

### 1. A-1 能否关闭？
**不能关闭为 `PASS`，但可从"阻断"降为"条件通过（本地）"。**
- `A1-02` 残留（证据自毁）**已在代码层消除**，并由 `E11o/E11r` 断言；
- 剩余未闭环：**三份标记全失败**、**整片擦除**、**真机断电/半写未验证**；
- ⇒ 判定 **`CONDITIONAL`**。要升为 `PASS` 必须先完成真机包 `RD-1…RD-8`（至少 RD-2/RD-3/RD-4/RD-7）。

### 2. B-3 是否仍在原有限定范围内通过？
**是。`PASS`（限 B-3 自身范围）**。
- 内容判定三态正确、`duplicate = same + collision + unverified` 精确成立（真实 D1 53 项）；
- HTTP 响应体仍**恰四字段**（`T02j` 未削弱）；`log_history` schema/索引未变；
- 未新增"每行查询"；本轮加固只影响冲突落库路径（且表未应用时熔断、零成本）。

### 3. `H-3-7-b` 状态？
**仍为 `LOCAL DONE / NOT APPLIED`**，并因本轮新发现**上调了本地实现质量**：
- 新增指纹 v2（修 `B3-07` 拼接歧义、64→128 bit）；
- 批内聚合 + 唯一键截断（修 `B3-08` 丢证据）；
- 表缺失熔断（修 `B3-09` 白耗 D1）。
- **migration 仍未应用、Worker 仍未部署、生产仍无 `log_conflict` 表。**
- 新增明确边界：`EVIDENCE LOST ON D1 FAILURE`（不宣称无损审计）。

### 4. HD-3 能否整体关闭？
**不能。只能关闭"本地实施阶段"。**
- 本地：A-1 条件通过、B-3 通过、H-3-7 a/c 已落地 + b 本地完成、Retention 守卫修复；
- 系统级：缺真机证据、缺 migration 应用、缺部署、缺生产水位数据；
- ⇒ 整体 **`CONDITIONAL`**，且 `BT-1 / G-4 / WP-5 / STOP-R1` 四道门禁**未动**（`BT-1` 仍 OPEN）。

### 5. 各生产动作需要哪些**独立**授权？

| 动作 | 需要的授权 | 前置条件 | 推荐顺序 |
|---|---|---|---|
| **应用 `0004_log_conflict`** | ① 批准草案升格为 `migrations/0004_*.sql` ② 生产 D1 写授权 | 真实 D1 54 项已过；需确认 `log_conflict` 增长可接受 | 第 2 步 |
| **应用 `0005_log_boot_watermark`** | ① 同上 ② 明确"默认关闭"开关策略 | 真实 D1 43 项已过 | 第 2 步（可与 0004 同批） |
| **启用 `LOG_BOOT_WATERMARK=1`** | 生产环境变量变更授权 | 表已存在；先手动跑一次只读查询评估噪声 | 第 3 步 |
| **部署 Worker** | Cloudflare 部署授权（含回滚方案） | migration 已应用（否则熔断降级，功能不可用） | 第 3 步 |
| **真机断电恢复（RD-7）** | 设备操作 + 破坏性测试授权 | 备份现网配置；准备恢复镜像 | 第 1 步（最优先） |
| **真机格式化 / NVS 擦除（RD-2/3/4）** | 同上 + 明确"允许清空设备日志" | 云端已有历史可交叉验证 | 第 1 步 |
| **整片擦除验证（RD-8）** | 同上 + 服务侧水位已部署 | 否则擦除后无任何检测手段 | **最后**（依赖水位上线） |
| **生产 D1 只读核对** | 只读查询授权（最小：`SELECT` on 3 表） | 无 | 可与真机并行 |

**推荐最小路径**：真机 `RD-7`（断电）→ 真机 `RD-2/3`（格式化/NVS）→ 只读核对 →
批准 `0004/0005` → 部署 Worker → 启用水位 → `RD-8`（整片擦除）。

---

## G. 本报告**不**主张的事（防越界清单）

1. 不主张 A-1 已消除 `R-2` / "整片擦除"风险。
2. 不主张设备侧隔离在"三份标记全失败"时仍能跨重启保持。
3. 不主张水位检测已在生产运行（**未部署、未启用**）。
4. 不主张 `log_conflict` 可做"无损审计"（D1 失败即丢证据）。
5. 不主张指纹"绝对无碰撞"（128 bit 概率保证）。
6. 不主张真机行为已验证（全部 `NOT RUN`）。
7. 不主张 HD-3 系统级关闭，也不主张任何门禁已解除。
