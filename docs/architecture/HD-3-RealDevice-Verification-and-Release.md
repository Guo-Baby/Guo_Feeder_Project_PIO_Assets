# HD-3 真机验证与发布记录（A-1 / 水位 / log_conflict / Retention）

> 日期：`2026-10-19` · 类型：**真机 + 生产变更**（本报告记录实际执行结果）
> 前序：`HD-3-Final-Audit-and-Closure.md`（2026-10-19 收尾审计）
> **本文件不覆盖任何既有报告。**

---

## 1. 总体状态

| 项 | 状态 | 依据 |
|---|---|---|
| **A-1（固件隔离/持久证据）** | **`VERIFIED ON DEVICE`**（含 1 项已知盲区实测确认） | RD-1 / RD-2 / RD-3 / RD-7a×2 / RD-2b / stage2 全部真机通过 |
| **`boot_seq` 独立水位** | **`END-TO-END VERIFIED`**（生产 D1 + Worker + 开关） | 表已应用、Worker 已部署、`LOG_BOOT_WATERMARK=1`、真实设备数据写入并捕获回退 |
| **`log_conflict`（H-3-7-b）** | **`APPLIED / DEPLOYED`**，但**线上无真实冲突样本** | 表已创建；线上 0 行（本轮未产生撞键）；本地真实 D1 55 PASS |
| **Retention（W7-G2）** | 见 §5 | 修复后重跑 |
| **整体 HD-3** | **`CONDITIONAL`** | 本地实施 + 服务端兜底均已落地；但「整片擦除」仍**需人工处置**，不是自动闭环 |

### 本地实施 vs 系统级验收（必须区分）

- **本地实施阶段**：可关闭。所有设计、代码、测试、迁移、部署均已完成并通过验证。
- **系统级验收**：**不能关闭**。理由：
  1. 整片擦除后设备侧**必然**从 `boot_seq=1` 重放（RD-8 实测），只能靠服务端**事后**发现；
  2. 该发现的产物是 `REWIND_CANDIDATE(not proof)`，**没有自动处置**，需人工确认；
  3. 若云端历史上存在 `boot_seq=1..N` 的记录（本设备历史从 75 开始，故未撞），则重放会**静默丢内容**（`OR IGNORE`），`log_conflict` 只能事后计数；
  4. 真机断电（物理拔电）未做，只做了 MCU 复位与分区擦除。

---

## 2. 阶段 1：A-1 真机验证（逐步记录）

**设备**：`ESP32-S3 / COM8 (CH343)` · **device_id**：`288485896ce4`（未变）
**固件**：`87b7d5c` 构建产物（`0x10000` app 分区，`Hash of data verified`）

### RD-1 换固件后首启（老固件 → 新固件升级路径）

| 项 | 内容 |
|---|---|
| 步骤 | `esptool write_flash -z 0x10000 firmware.bin`（`--after no_reset`）→ RTS 脉冲复位 → 抓 45 s |
| 串口证据 | `[Log] recovery=LEGACY boot_hw=104 -> boot_seq=105 reserved=111104 corrupt=1 (nvs=1 slot=0 legacy=1 seg_max=0)` |
| 预期 | 老固件只写过 `meta.bin` ⇒ 走 LEGACY，`boot_seq` 递增 |
| 实际 | **符合**：`104 → 105`，`failclosed=0`，未隔离 |
| 说明 | `nvs=1` 是**本 boot 之后**写入的（`[LogT] evidence ... gen=2`），boot 时 NVS 证据尚不存在 |

### RD-7a 复位重启 ×2（跨重启连续性）

| 轮次 | 串口证据 | 结论 |
|---|---|---|
| 1 | `recovery=NVS boot_hw=105 -> boot_seq=106 ... (nvs=1 slot=1 legacy=0)` | `105→106`；首启已建立双槽（`slot=1`） |
| 2 | `recovery=NVS boot_hw=106 -> boot_seq=107 ... (nvs=1 slot=1)` | `106→107`；`reserved` 每次 +256 |

**未做**：真·物理断电（需人工拔插 USB）。RTS 复位会清 RAM/外设但保留持续供电 ⇒ 对"持久化恢复"等价性高，**不能**验证"掉电瞬间半写"。

### RD-2 LittleFS 格式化（`erase_region 0x410000 0xBF0000`，42.2 s）

| 项 | 内容 |
|---|---|
| 步骤 | 擦除整个 `littlefs` 分区 → 复位 → 抓 40 s |
| 关键日志 | `E esp_littlefs: Corrupted dir pair at {0x0, 0x1}` → `Failed to initialize LittleFS` → `[System] LittleFS mounted`（**formatOnFail 自动格式化**） |
| 日志区 | `Flash ring ready: valid=0, oldest=0, newest=0, total_rec=0` |
| 恢复判据 | **`recovery=NVS boot_hw=107 -> boot_seq=108 (nvs=1 slot=0 legacy=0 seg_max=0)`** |
| 结论 | **`A1-01` 修复在真机成立**：日志区全空 + 双槽/legacy/段全无，但 NVS 证据在 ⇒ **不判"全新设备"**、**不从 1 重放**、`failclosed=0` 正常继续 |
| 附带 | 配置随分区清空（`[CFG][E] load failed, module=wifi/oled/valve/time/weight/mqtt`）⇒ 与"格式化"语义一致 |

### RD-2b 恢复配置（`uploadfs`）

| 项 | 内容 |
|---|---|
| 证据 | `recovery=NVS boot_hw=109 -> boot_seq=110` · `[CFG][I] init done, loaded 8/8 modules` · `Connecting to:wqs1` → `WiFi connected` → `MQTT connected` · `boot validated` |
| 结论 | 擦除后 `uploadfs` 可完全恢复；`boot_seq` 继续递增、未回退 |

### RD-3 NVS 擦除（`erase_region 0x9000 0x5000`）

| 项 | 内容 |
|---|---|
| 关键日志 | `[E] getString(): did NOT_FOUND` → `[Identity] new device_id=288485896ce4 (nvs written)` |
| 恢复判据 | **`recovery=SLOT boot_hw=110 -> boot_seq=111 (nvs=1 slot=1 legacy=0)`**，`src=1`，`failclosed=0` |
| 结论 | **NVS 丢失但日志区有效 ⇒ 走双槽，序号继续递增**；身份按 MAC 重新派生为**同一 ID**；WiFi/MQTT 不受影响（配置在 LittleFS） |
| 重要含义 | 此路径下 `s_first_boot=true`（did 本次才写入）。**若日志区也是空的**，就会判 `PROVISION` —— 这正是 RD-8 |

### 故障注入：`mstage 2`（两阶段提交中间态，真机）

| 项 | 内容 |
|---|---|
| 步骤 | `logt mstage 2` → `logt fill warn 300`（跨过 256 边界触发预留）→ `logt flush` → `logt fwipe`（删段以排除段高水位干扰）→ 复位 |
| 注入时证据 | `[LogT] mstage armed=2` · `[LogT] meta ... reliable=0 base=114177 limit=114432`（**预留失败 ⇒ 降级但仍以本地区间继续**） |
| 判定性重启 | `recovery=NVS boot_hw=115 -> boot_seq=116 reserved=114176` |
| 结论 | 半写槽 payload 为 `reserved=114432`；实测 `reserved=114176` ⇒ **半写槽未被采纳**，两阶段提交在真实 LittleFS 上成立 |
| 补充 | 首次尝试时 `reserved=113664` 曾疑似异常，追查为**段高水位 `seg_align` 抬高**（`align_reserve(113452)=113664`），非槽污染；故用 `fwipe` 消除歧义后复测 |

### RD-8 整片擦除（NVS + LittleFS 同时丢失）

**前置条件（全部满足后才执行）**：水位 migration 已应用 ✅ · Worker 已部署 ✅ · `LOG_BOOT_WATERMARK=1` ✅ · 真实 ingest 已验证水位写入 ✅ · 设备身份/恢复流程已确认 ✅ · 测试后查询方案就绪 ✅

| 项 | 内容 |
|---|---|
| 步骤 | `erase_region 0x9000 0x5000` + `erase_region 0x410000 0xBF0000`（50.2 s）→ 复位 |
| 设备侧 | `did NOT_FOUND` → 重新派生同 ID → `Flash ring valid=0 total_rec=0` → **`recovery=PROVISION (first boot of a new unit) -> boot_seq=1`**，`base=1 limit=256`，`failclosed=0`，`Connecting to:`（空） |
| 设备侧结论 | **矩阵 ⑦ 实测确认**：整片擦除后设备**无法**区分"新机"与"旧机被清"，判 `PROVISION`、从 **1** 开始、**不隔离**、无任何告警 |
| 恢复 | `uploadfs` → 启动 `recovery=NVS boot_hw=2 -> boot_seq=3`（期间一次 boot=2） |
| 服务端 | 见 §3.4：`max_boot_seq` **保持 117**，`rewind_count` 增到 20，`last_rewind_boot=3` |
| 附带损失 | `uploadfs` 覆盖 LittleFS ⇒ **boot=1 的未上传日志随段被清空**（`log_history` 只有 boot=2/3）。这是"日志区与配置同分区"的固有代价，**不是缺陷**，但必须写进运维认知 |

---

## 3. 阶段 2：`boot_seq` 水位（端到端）

### 3.1 Schema（已应用）

`migrations/0005_log_boot_watermark.sql`（**已执行于生产 D1**）：

```
log_boot_watermark(
  device_id TEXT PRIMARY KEY,      -- 行数量级 = 设备数
  max_boot_seq INTEGER NOT NULL,   -- 只升不降（MAX()）
  max_seq_at_max INTEGER,
  first_seen_at / last_seen_at / updated_at,
  obs_count INTEGER,               -- 观测条数（含重投）
  rewind_count INTEGER,            -- 观测到 boot < 水位 的次数（**观测计数，重投会放大**）
  last_rewind_at / last_rewind_boot,
  status TEXT CHECK(open|reviewed|benign|incident)
) + ix_lbw_updated + ix_lbw_rewind
```

### 3.2 Worker 接入（已部署）

- `src/log-boot-watermark.js`：`analyzeBatch` 四态（`init/advance/same/rewind_only`）；
  `LOG_BOOT_WM_UPSERT` 用 `MAX(max_boot_seq, excluded.max_boot_seq)` 保证单调。
- `src/history-ingest.js`：在 **`persist()` 之前**记录（`INSERT OR IGNORE` 丢弃内容时仍有证据）。
- 开关：`wrangler.toml` `[vars] LOG_BOOT_WATERMARK = "1"`（本次启用）。
- 成本：每批 **1 次 SELECT + 1 次 batch**（与批内行数无关）；未启用时 0 次。

### 3.3 线上验证（真实设备，非合成）

**第一次观测**（部署后第一次上传）：

```json
{"device_id":"288485896ce4","max_boot_seq":117,"max_seq_at_max":114441,
 "obs_count":11,"rewind_count":1,"last_rewind_boot":116}
```

- `max_boot_seq=117` = 当前 boot ✓
- `rewind_count=1 / last_rewind_boot=116` = **迟到批次（补发的 boot=116）被计为候选，水位未降** ✓

### 3.4 RD-8 后：服务端捕获设备侧不可辨的回退

```json
{"device_id":"288485896ce4","max_boot_seq":117,"max_seq_at_max":114441,
 "obs_count":30,"rewind_count":20,"last_rewind_boot":3,"last_rewind_at":"2026-10-18 17:43:07"}
```

| 观察 | 含义 |
|---|---|
| `max_boot_seq` **仍为 117** | 水位**没有被**回退批次拉低（`MAX()` 生效） |
| `rewind_count` 11→**20** | 新增的 9 条低 `boot_seq` 观测被计数 |
| `last_rewind_boot=3` | 最近一次候选的 `boot_seq` = 3（设备当前值） |
| `log_history` 新行 | `boot_seq=2`（3 行，seq 257-259）、`boot_seq=3`（10 行，seq 513-522）⇒ 这些是**新键**，故 `accepted` 而非 `duplicate` |

**结论**：设备侧 `PROVISION` 且**零告警**，服务端在**同一次 ingest 内**就记录到回退候选 ⇒ **A1-01 的未闭环风险已有服务端兜底**。

**仍未解决**：兜底只是"看得见"，不是"自动处置"。`REWIND_CANDIDATE` 需要人工确认；且若历史上存在 `boot_seq=1..N` 的行，重放会静默丢内容（本设备历史从 75 起，故本轮未撞）。

### 3.5 边界（实测 + 设计）

| 场景 | 行为 |
|---|---|
| 正常递增 | `max_boot_seq` 抬高，`obs_count` 累加 |
| 同 boot 重复 | `same`，水位不变 |
| 迟到/乱序 | 计 `rewind`，**水位不降**（RD-8 与首轮观测均实证） |
| 并发 | `MAX()` 单调 ⇒ 不会回退；`rewind_count` 可能放大（观测计数） |
| D1 读/写失败 | `failed` 计数 + 一行告警，**不影响 HTTP 响应 / ACK / duplicate** |
| 表缺失 | 熔断（本 isolate 不再尝试） |
| **迟到 vs 真回退** | **不可区分** ⇒ 只出候选 |
| **首次建立前的历史** | 不可见 |

---

## 4. 阶段 3：`log_conflict`（已应用 + 已部署）

| 项 | 状态 |
|---|---|
| migration | `0004_log_conflict.sql` **已应用**（`Executed 4 commands`，`✅`） |
| 表 | `log_conflict` 存在；`ux_lc_key` / `ix_lc_device_seq` 已建 |
| 线上行数 | **0**（本轮未产生撞键 ⇒ **路径未被真实冲突触发**，如实标注） |
| 本地真实 D1 | **55 PASS / 0 FAIL**（含首次冲突/聚合/不同字段/不同键/表缺失/不写 `log_history`） |
| ACK 语义 | 未变（ACK = 键存在，不保证内容已持久化） |
| 失败路径 | ACK 后 D1 失败 ⇒ 仅 `conflict_failed` 计数，**该批冲突无持久证据**（`EVIDENCE LOST ON D1 FAILURE`，协议边界） |
| 容量 | 行数上界 = 唯一 `conflict_key` 数；线上 0 行 ⇒ **无法给出增长率**（不编造） |
| 保留策略 | 建议 180 天 / 未建 Cron / 未清理 |

**为什么线上 0 行**：`log_conflict` 只在**同 `(boot,seq)` 但内容不同**时产生。RD-8 重放用的是 `boot_seq=1/2/3` 的**新键**（历史从 75 起），因此 `INSERT OR IGNORE` 没有命中冲突 ⇒ 没有冲突样本。这是**事实**，不是"功能未生效"的证明缺口 —— 功能路径已由本地真实 D1 覆盖。

---

## 5. 阶段 4：Retention

| 轮次 | 结果 | 说明 |
|---|---|---|
| 修复前（10-18） | 105 PASS / 1 FAIL | `W7-G2` 判据错误（要求工作区干净） |
| 基线增量修复后 | **106 PASS / 0 FAIL** | 判据改为相对基线增量 |
| 本轮第 1 次 | 105 PASS / **1 FAIL** | **测试期间我执行了 commit** ⇒ `git status` 从 4 条变 0 条 ⇒ W7-G2 **正确检出**（非缺陷） |
| 本轮第 2 次（干净基线） | **106 PASS / 0 FAIL · EXIT=0** | 重跑通过 |

**关于本轮第 1 次失败**：这恰好证明 `W7-G2` 仍有效 —— 它抓到了"测试期间工作区被改动"。按纪律如实保留，不删断言、不排除用例。

---

## 6. 全部测试证据

| # | 命令 | 类型 | 结果 |
|---|---|---|---|
| 1 | `python test/log_contract/run_contract_test.py` | wasm 真执行 | **ALL PASS**（8/8 步；`meta_slot` 32 + `meta_evidence` 83） |
| 2 | `pio run`（`.pio/build/hd3real`） | 编译 | **SUCCESS** · RAM 40.5% · Flash 66.8% |
| 3 | 真机 RD-1 / RD-7a×2 / RD-2 / RD-2b / RD-3 / stage2 / RD-8 | **真机** | 全部通过（见 §2） |
| 4 | `node test/run_all.mjs` | L1 全量 | **19/19 exit=0** |
| 5 | `bash test/real_d1/log_conflict_real_d1.sh` | 真实 D1 | **55 PASS / 0 FAIL** |
| 6 | `bash test/real_d1/log_boot_watermark_real_d1.sh` | 真实 D1 | **44 PASS / 0 FAIL** |
| 7 | `bash test/real_d1/b3_collision_real_d1.sh` | 真实 D1 | **53 PASS / 0 FAIL** |
| 8 | `bash test/real_d1/retention_real_d1.sh` | 真实 D1 | **106 PASS / 0 FAIL · EXIT=0**（干净基线；本轮第 1 次 105/1 的成因见 §5） |
| 9 | `wrangler d1 migrations apply --remote` | **生产** | 0004 ✅ / 0005 ✅ |
| 10 | `wrangler deploy` | **生产** | `508461f3-0dcf-443c-98f9-d185f412174e` |
| 11 | 生产只读查询（水位/历史/冲突） | **生产只读** | 见 §3.3 / §3.4 / §4 |

### 失败留痕（全部保留，未删断言）

| 轮次 | 失败 | 归因 | 处置 |
|---|---|---|---|
| 水位 real_d1 首轮 | `W-3f advances>=1` | 断言位置错（检查的是同 boot 批次） | 移到 W-4d |
| 冲突 L1 | `D4b` | 期望写错（聚合后 skipped=0） | 修正期望 |
| 水位 L1 | `A20/A21/C10` | **实现缺陷**：`Number(null)===0`；`devices` 截断前计数 | 加 `toInt`；计数移到截断后 |
| retention 本轮第 1 次 | `W7-G2`（增量 4） | **测试期间执行了 commit**（真实改动） | 干净基线重跑 |
| retention 早前 | `EXIT=2 syntax error` | 在脚本运行期间修改该脚本 | 该轮作废并重跑 |

---

## 7. Migration / 部署 / 开关（实际执行环境）

| 项 | 值 |
|---|---|
| D1 数据库 | `guofeeder`（`0d8385ba-a1a6-4fcc-8e43-ae61c014c43d`，APAC/SIN） |
| 已应用 migration | `0001` · `0002` · `0003` · **`0004`** · **`0005`** |
| 部署前 Worker 版本 | `31762ee8-89c4-4622-94ee-83d1f4aa73c7`（2026-10-04） |
| **部署后 Worker 版本** | **`508461f3-0dcf-443c-98f9-d185f412174e`** |
| 上传体积 | 129.07 KiB（gzip 32.58 KiB） |
| 环境变量 | `LOG_BOOT_WATERMARK = "1"`（`[vars]`，明文、非密钥） |
| Cron | `0 3 * * *`（未改） |
| `LOG_BOOTSEQ_GUARD` | 未设置（旧启发式通道保持关闭） |

### 回退方式（已记录）

1. **Worker**：`wrangler rollback 31762ee8-89c4-4622-94ee-83d1f4aa73c7`
2. **开关**：`LOG_BOOT_WATERMARK` 改 `"0"` 或删除 `[vars]` 段后重新 deploy（幂等，可随时关闭）
3. **表**：**不删除**（水位/冲突证据需保留；删除属独立的破坏性授权）

---

## 8. 提交清单（本任务）

| # | 仓库 | Hash | 范围 | 目的 | 回退 |
|---|---|---|---|---|---|
| C1 | 固件 (`wb`) | `87b7d5c` | `src/log/*`、`src/main.cpp`、`device_identity.*`、`test/log_contract/*` | A-1 双槽 + NVS 证据 + 隔离 + 损坏痕迹 | `git revert 87b7d5c` |
| C2 | 固件 (`wb`) | `2bd358b` | 10 篇 HD-3 文档 + `cloud_protocol.md` §8.2.5 | 过程记录与 ACK 语义澄清 | `git revert 2bd358b` |
| C3 | 云端 (`main`) | `94467d4` | `test/real_d1/retention_real_d1.sh` | W7-G2 基线增量判据 | `git revert 94467d4` |
| C4 | 云端 (`main`) | `e58d104` | `log-ack.js`、`log-conflict.js`、`migrations/0004`（当时在 drafts）、相关测试 | B-3 三态 + 冲突追踪 | `git revert e58d104` |
| C5 | 云端 (`main`) | `7b6ad7e` | `log-boot-watermark.js`、`0005`（drafts）、`history-ingest.js`、`index.js`、测试 | `boot_seq` 水位（默认关闭） | `git revert 7b6ad7e` |
| C6 | 云端 (`main`) | `362ddfd` | migration 转正 + `wrangler.toml` + 两个夹具 | **生产应用 + 部署 + 启用**（记录版本与回退） | Worker `rollback`；代码 `git revert 362ddfd` |

> C6 的 `!` 表示含生产变更。回退 C6 代码**不会**自动回滚已应用的 migration（表保留），需按 §7 处理。

**未提交（属其他任务，保持原状）**：`HANDOFF.md`、`docs/README.md`、`P3-History-D1-Plan.md`、`P3-WP3-*.md`、`docs/issues/未修复的问题.md`、`P3-WP6-*` ×2。

---

## 9. 工作区完整性

| 仓库 | 起始 HEAD | 结束 HEAD | 未提交内容 |
|---|---|---|---|
| 固件 (`wb`) | `3acf91c` | **`2bd358b`** | 6 个其他任务文档 + 2 篇 WP-6 报告（**原样保留**） |
| 云端 (`main`) | `a3473eb` | **`362ddfd`** | 无（全部 HD-3 文件已按 C3–C6 提交） |
| EMQX (`main`) | `ec2364f` | `ec2364f` | 无 |

- 无 `git reset --hard` / `clean` / `stash` / 切分支 / rebase / merge / cherry-pick
- 每次 commit 前均 `git diff --cached --name-only` 核对，未混入他任务文件
- 设备侧：仅对 `COM8`（`288485896ce4`）执行破坏性操作，未触碰其他设备
- 生产侧：仅执行本任务所需的 `migrations apply` / `deploy` / 只读查询；未删除任何数据、未改 Cron/EMQX

---

## 10. 剩余风险与未完成事项

| # | 项 | 状态 | 说明 / 所需授权 |
|---|---|---|---|
| R-1 | **整片擦除后必从 1 重放** | **已实测确认，未消除** | 设备侧无解；服务端只出候选。要"自动处置"需产品决策（如拒绝低 `boot_seq` 批次） |
| R-2 | 整片擦除重放**可能撞历史键并静默丢内容** | 存在 | 本设备历史从 75 起，本轮未撞；若历史含 `boot_seq=1..N` 则会撞。缓解：`log_conflict` 可事后计数（需人工看） |
| R-3 | **真·物理断电**未验证 | `NOT RUN` | 需人工拔插 USB 或可控电源；验证"掉电瞬间半写" |
| R-4 | `log_conflict` 线上无样本 | 未验证线上路径 | 需构造真实撞键（建议用独立测试 device_id，需授权写生产测试数据） |
| R-5 | `rewind_count` 是观测计数 | 设计如此 | 重投会放大；**不要**当精确事件数用 |
| R-6 | 迟到 vs 真回退不可区分 | 设计边界 | 只能人工用设备侧 `[Log] recovery=` 交叉确认 |
| R-7 | `uploadfs` 会丢未上传日志 | 固有限制 | 日志区与配置同分区；运维需知 |
| R-8 | `A1-03` 远程可观测 | 仅串口 | `[LogT] meta/evidence/trace` + `LogFlashInfo`；无 MQTT 通道（需协议授权） |
| R-9 | `BT-1` / `G-4` / `WP-5` / `STOP-R1` | **未动** | `BT-1` 仍 `OPEN` |

---

## 11. 逐项回答

**① A-1 是否通过真机验证？**
**是（限定范围）**。RD-1（老固件升级）、RD-7a×2（跨重启）、RD-2（格式化）、RD-2b（恢复）、RD-3（NVS 擦除）、`mstage 2`（半写槽）全部通过，`boot_seq` 全程单调递增、无隔离误触发。
未通过的部分：**真·物理断电**（`NOT RUN`）；**矩阵 ⑦（整片擦除）** 实测确认设备侧无解（这是设计边界，不是回归）。

**② 服务端水位是否完成端到端验证？**
**是**。migration 已应用、Worker 已部署、开关已启用；真实设备数据写入（`obs_count=11→30`），迟到日志被计为候选且 `max_boot_seq` 保持 117；RD-8 后 `rewind_count=20 / last_rewind_boot=3` 捕获到设备侧不可辨的回退。

**③ H-3-7-b 是否仍有未关闭风险？**
**有，两类**：
(a) **ACK 后 D1 写失败 ⇒ 该批冲突无持久证据**（协议边界，未解决，只计数）；
(b) **线上无真实冲突样本** ⇒ 生产路径未被真实撞键触发（本地真实 D1 已覆盖）。
表与代码均已上线，回退可控。

**④ Retention 是否稳定通过？**
**是**（见 §5/§6）：干净基线下重跑通过；本轮唯一失败是"测试期间执行 commit"被守卫正确检出，属真实变化而非缺陷。

**⑤ RD-8 是否执行、结果如何？**
**已执行**。设备侧：`recovery=PROVISION → boot_seq=1`、`failclosed=0`、无告警；服务端：`max_boot_seq` 保持 117、`rewind_count` 增至 20、`last_rewind_boot=3`。附带发现：`uploadfs` 覆盖 LittleFS 会丢掉 boot=1 的未上传日志。

**⑥ HD-3 是否可系统级关闭？**
**不能**。可关闭的是"本地实施 + 部署"阶段。系统级关闭的前提是 R-1/R-2（整片擦除后的自动处置与内容丢失防护）有产品决策，以及 R-3（物理断电）完成验证。
